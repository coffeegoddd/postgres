"""Check every selected engine before starting builds, servers, or data loads."""
import json
import os
from pathlib import Path
import shlex
import shutil
import socket
import subprocess
import tempfile

import core


class Checks:
    def __init__(self):
        self.errors = []

    def test(self, label, action):
        try:
            action()
            return True
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
            self.errors.append(f"{label}: {error}")
            return False

    def finish(self):
        if self.errors:
            raise RuntimeError("Preflight failed before starting work:\n  - " + "\n  - ".join(self.errors))


def require_executable(path):
    if not Path(path).is_file() or not os.access(path, os.X_OK):
        raise ValueError(f"missing or non-executable {path}; build it or fix config.json")


def require_tools(names):
    missing = [name for name in names if not shutil.which(name)]
    if missing:
        raise ValueError("missing commands on PATH: " + ', '.join(missing))


def probe(command, cwd=None, expected=(0,), timeout=90):
    p = subprocess.run(command, cwd=cwd, stdin=subprocess.DEVNULL,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=timeout)
    if p.returncode not in expected:
        detail = '\n'.join(p.stdout.strip().splitlines()[-8:])
        raise RuntimeError(f"{shlex.join(map(str, command))} exited {p.returncode}\n{detail}")
    return p.stdout


def writable(path):
    # Test the closest existing parent without creating permanent directories.
    while not path.exists():
        path = path.parent
    with tempfile.TemporaryFile(dir=path) as f:
        f.write(b'vc_bench preflight')
        f.flush()


def source(path, entry):
    if not path.is_dir() or not (path / entry).exists():
        raise ValueError(f"missing source checkout or {entry}: {path}; fix engines.*.source in config.json")
    probe(['git', 'rev-parse', '--show-toplevel'], cwd=path)


def go_dependencies(directory, target):
    # Go handles module-specific minimum/toolchain versions, including automatic
    # toolchain selection. This can download dependencies, but does not build them.
    probe(['go', 'list', '-mod=readonly', '-deps', target], cwd=directory, timeout=120)


def go_compilers(directory):
    env = json.loads(probe(['go', 'env', '-json', 'CC', 'CXX', 'CGO_ENABLED'], cwd=directory))
    if env['CGO_ENABLED'] != '1':
        raise ValueError('these engine builds require CGO_ENABLED=1')
    with tempfile.TemporaryDirectory(prefix='vc-bench-cgo-') as tmp:
        tmp = Path(tmp)
        for key, suffix in [('CC', 'c'), ('CXX', 'cpp')]:
            compiler = shlex.split(env[key])
            if not compiler:
                raise ValueError(f'Go {key} is empty')
            program = tmp / ('probe.' + suffix)
            # Dolt/Doltgres link ICU through go-icu-regex. Probe actual headers and
            # linked symbols, not just pkg-config metadata or compiler existence.
            program.write_text('#include <unicode/uregex.h>\n'
                               'int main(void) { UErrorCode s = U_ZERO_ERROR; '
                               'URegularExpression *r = uregex_openC("x", 0, 0, &s); '
                               'if (r) uregex_close(r); return U_FAILURE(s); }\n')
            flags = shlex.split(os.environ.get('CGO_CPPFLAGS', ''))
            flags += shlex.split(os.environ.get('CGO_CFLAGS' if key == 'CC' else 'CGO_CXXFLAGS', ''))
            if key == 'CXX':
                flags += ['-std=c++17']
            output = tmp / ('probe-' + key)
            probe([*compiler, *flags, str(program), '-o', str(output),
                   *shlex.split(os.environ.get('CGO_LDFLAGS', '')), '-licui18n', '-licuuc', '-licudata'])
            probe([str(output)])


def postgres_dependencies(root, checkout):
    # The real configure script is the authoritative check for compiler/parser
    # versions and development libraries (ICU, OpenSSL, readline, zlib, etc.).
    # Run it in a disposable directory before building any engine.
    with tempfile.TemporaryDirectory(prefix='.vc-bench-configure-', dir=root) as tmp:
        probe([str(checkout / 'configure'), f'--prefix={tmp}/inst', '--with-openssl', 'CFLAGS=-O2'],
              cwd=tmp, timeout=120)


def available_port(root, engine, port):
    if core.running(root, engine):
        return
    with socket.socket() as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(('127.0.0.1', port))


def installed(root, c, engine):
    binary = Path(c['engines'][engine]['binary'])
    require_executable(binary)
    adapter = core.ENGINES[engine]
    probe([str(binary), *adapter.version_args], timeout=20)
    adapter.preflight_installed(binary, probe, require_executable)
    provenance = root / 'build' / (engine + '.json')
    if provenance.exists():
        expected = json.loads(provenance.read_text())['sha256']
        if core.sha256(binary) != expected:
            raise ValueError(f'{binary} does not match build provenance; rebuild or use a separate root')


def fixtures(root, c, engine):
    from experiment import fixture_path, signature
    errors = []
    for n in c['row_counts']:
        path = fixture_path(root, engine, n)
        if not path.exists():
            errors.append(f'{path.name}: not prepared')
            continue
        state = json.loads(path.read_text())
        if state.get('signature') != signature(c, n):
            errors.append(f'{path.name}: fixture configuration differs')
        for k in c['changes']:
            key = f"k{k}_h{max(c['history_depths'])}"
            if not state.get('fixtures', {}).get(key, {}).get('ready'):
                errors.append(f'{path.name}/{key}: incomplete')
    if errors:
        raise ValueError('; '.join(errors) + '; run prepare first')


def check(root, c, engines, stage):
    checks = Checks()
    core.log(f'Preflight for {stage}: {", ".join(engines)}')
    checks.test('data root must be writable', lambda: writable(root))
    if stage == 'build':
        tools_ok = checks.test('build tools', lambda: require_tools(['go', 'git']))
        if tools_ok:
            checks.test('SQL driver dependencies', lambda: go_dependencies(core.HERE / 'driver', '.'))
        for engine in engines:
            e = c['engines'][engine]
            checkout = Path(e['source'])
            adapter = core.ENGINES[engine]
            source_ok = checks.test(f'{engine} source', lambda: source(checkout, adapter.source_entry))
            checks.test(f'{engine} binary destination', lambda: writable(Path(e['binary']).parent))
            adapter.preflight_build(checks, root, checkout, source_ok, tools_ok)
    else:
        if stage in ('prepare', 'run'):
            def driver():
                binary = root / 'bin/sql-driver'
                require_executable(binary)
                output = probe([str(binary)], expected=(2,))
                if 'usage: sql-driver DRIVER DSN' not in output:
                    raise ValueError('unexpected SQL driver; rebuild bin/sql-driver')
            checks.test('SQL driver', driver)
        if stage in ('prepare', 'run') and set(c['passes']) - {'timing'}:
            from memory import snapshot
            checks.test('Linux /proc memory and I/O access', lambda: snapshot(os.getpid()))
        for engine in engines:
            checks.test(f'{engine} installed binaries/libraries', lambda: installed(root, c, engine))
            checks.test(f'{engine} data directory', lambda: writable(root / engine / 'data'))
            port = c['engines'][engine]['port']
            checks.test(f'{engine} port {port}', lambda: available_port(root, engine, port))
            if stage == 'run':
                checks.test(f'{engine} fixtures', lambda: fixtures(root, c, engine))
    checks.finish()
    core.log(f'Preflight passed; root {root}; {shutil.disk_usage(root).free / 1e9:.1f} GB free')
