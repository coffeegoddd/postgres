"""Configuration, isolated server lifecycle, and persistent SQL client."""
import contextlib
import datetime
from decimal import Decimal, InvalidOperation, ROUND_CEILING
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import time

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
ENGINES = ("dolt", "doltgres", "prolly")
# Each pass is a separate set of measured observations; any subset may run, and
# separate runs' passes can be merged into one report (report DIR DIR...).
# timing: latency alone. memory: server-tree PSS/RSS/private memory, sampled
# every sample_interval_ms. cpu: CPU time and page faults; io: kernel read and
# write bytes -- both from one snapshot before and one after, nothing sampled
# in between to perturb them.
PASSES = ("timing", "memory", "cpu", "io")


def check_passes(passes):
    if not passes or set(passes) - set(PASSES) or len(set(passes)) != len(passes):
        raise ValueError("passes must select distinct passes among " + ", ".join(PASSES))


def default_dirty_sizes():
    return {"small": {"fraction": 0.0001}, "medium": {"fraction": 0.01},
            "large": {"fraction": 0.1}}


def dirty_rows(spec, n):
    if not isinstance(spec, dict) or set(spec) not in ({'rows'}, {'fraction'}):
        raise ValueError("each dirty size must specify exactly one of rows or fraction")
    if 'rows' in spec:
        count = spec['rows']
        if type(count) is not int:
            raise ValueError('dirty size rows must be an integer')
    else:
        try:
            fraction = Decimal(str(spec['fraction']))
            if not fraction.is_finite() or not 0 < fraction <= 1:
                raise ValueError('dirty fraction must be > 0 and <= 1')
            count = int((fraction*n).to_integral_value(rounding=ROUND_CEILING))
        except InvalidOperation:
            raise ValueError('dirty fraction must be numeric') from None
    if not 1 <= count <= n:
        raise ValueError(f'dirty size {count} must fit in table of {n} rows; no clamping is performed')
    return count


def log(message):
    print(f"[{datetime.datetime.now().isoformat(timespec='seconds')}] {message}", flush=True)


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(value, indent=2) + "\n")
    tmp.replace(path)


def sha256(path):
    with Path(path).open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def source_info(path):
    return {"path": str(path), "commit": subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=path, text=True).strip(),
        "status": subprocess.check_output(["git", "status", "--porcelain"], cwd=path, text=True)}


def init(root, smoke):
    root.mkdir(parents=True, exist_ok=True)
    reserved = ["config.json", "bin", "build", "results", *ENGINES]
    if any((root / name).exists() for name in reserved):
        raise ValueError(f"init would overlap existing benchmark files in {root}; choose another root")
    sources = Path.home() / "cursor_src"
    dg = sources / "doltgres"
    if not dg.exists():
        dg = sources / "doltgresql"
    config = {
        "schema_version": 1, "payload_bytes": 128 if smoke else 1024,
        "rows": [128, 512, 2048, 8192] if smoke else [],
        "sizes_gb": [] if smoke else [0.01, 0.1, 1, 10],
        "changes": [8] if smoke else [1000],
        "dirty_sizes": default_dirty_sizes(),
        "history_depths": [1, 4] if smoke else [1, 4, 16, 32],
        "repetitions": 2 if smoke else 7, "warmups": 1,
        "passes": list(PASSES), "sample_interval_ms": 10,
        "batch_rows": 1000, "timeout_seconds": 3600, "seed": 20261021,
        "engines": {
            "dolt": {"source": str(sources / "dolt"), "binary": str(root / "bin/dolt"), "port": 13360},
            "doltgres": {"source": str(dg), "binary": str(root / "bin/doltgres"), "port": 15432},
            "prolly": {"source": str(REPO), "binary": str(root / "build/postgres/inst/bin/postgres"), "port": 15433},
        },
    }
    write_json(root / "config.json", config)
    log(f"Created {root / 'config.json'}")


def config(root):
    c = json.loads((root / "config.json").read_text())
    w = c["payload_bytes"]
    if not isinstance(w, int) or not 1 <= w <= 16384:
        raise ValueError("payload_bytes must be an integer from 1 to 16384")
    rows = c["rows"] or [int(float(gb)*1_000_000_000) // (w+16) for gb in c["sizes_gb"]]
    c["row_counts"] = sorted(set(rows))
    for key, vals in (("rows", rows), ("changes", c["changes"]), ("history_depths", c["history_depths"])):
        if not vals or any(type(v) is not int or v <= 0 for v in vals):
            raise ValueError(f"{key} must contain positive integers")
    if max(c["changes"]) > min(rows):
        raise ValueError("every changed-row count must fit in the smallest table")
    c.setdefault('dirty_sizes', default_dirty_sizes())
    if not isinstance(c['dirty_sizes'], dict):
        raise ValueError('dirty_sizes must map profile names to rows or fraction specifications')
    for label, spec in c['dirty_sizes'].items():
        if not label or label in ('clean', 'diff') or not label.replace('_', '').isalnum():
            raise ValueError('dirty size names must be alphanumeric/underscore, excluding clean and diff')
        for n in rows:
            dirty_rows(spec, n)
    for k in ("repetitions", "batch_rows", "timeout_seconds"):
        if type(c[k]) is not int or c[k] <= 0:
            raise ValueError(f"{k} must be a positive integer")
    if c["batch_rows"] > 10000 or c["warmups"] < 1 or c["sample_interval_ms"] < 1:
        raise ValueError("batch_rows <= 10000, warmups >= 1, sample_interval_ms >= 1 required")
    check_passes(c["passes"])
    ports = [e["port"] for e in c["engines"].values()]
    if len(set(ports)) != len(ports) or any(not 1024 <= p <= 65535 for p in ports):
        raise ValueError("engines need distinct unprivileged ports")
    return c


def build(root, c, engines, jobs):
    (root / "bin").mkdir(exist_ok=True)
    (root / "build").mkdir(exist_ok=True)
    commands = [("driver", ["go", "build", "-trimpath", "-o", str(root / "bin/sql-driver"), "."], HERE / "driver")]
    for engine in engines:
        e = c["engines"][engine]
        source = Path(e["source"])
        if engine == "prolly":
            commands.append((engine, ["python3", str(REPO / "src/tools/prolly_bench/build_postgres.py"),
                                     "--source", str(source), "--build-dir", str(root / "build/postgres"),
                                     "--jobs", str(jobs), "--", "--with-openssl", "CFLAGS=-O2"], REPO))
        else:
            commands.append((engine, ["go", "build", "-trimpath", "-o", e["binary"],
                                      "./cmd/dolt" if engine == "dolt" else "./cmd/doltgres"],
                             source / "go" if engine == "dolt" else source))
    for name, cmd, cwd in commands:
        log(f"Building {name}; log: {root / 'build' / (name+'.log')}")
        before = source_info(c["engines"][name]["source"]) if name != "driver" else None
        with (root / "build" / (name + ".log")).open("w") as f:
            subprocess.run(cmd, cwd=cwd, stdout=f, stderr=subprocess.STDOUT, check=True)
        binary = root / "bin/sql-driver" if name == "driver" else Path(c["engines"][name]["binary"])
        after = source_info(c["engines"][name]["source"]) if before else None
        if before != after:
            raise RuntimeError(f"{name} source changed during build")
        write_json(root / "build" / (name + ".json"), {
            "command": cmd, "source": before, "binary": str(binary), "sha256": sha256(binary),
            "built_at": datetime.datetime.now(datetime.timezone.utc).isoformat()})


def identity(pid):
    try:
        return Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[19]
    except FileNotFoundError:
        return None


def running(root, engine):
    path = root / engine / "server.json"
    if not path.exists():
        return None
    info = json.loads(path.read_text())
    return info if identity(info["pid"]) == info["start_ticks"] else None


def start(root, c, engine):
    if running(root, engine):
        return False
    e = c["engines"][engine]
    binary = Path(e["binary"])
    if not binary.is_file():
        raise ValueError(f"Missing {binary}; run build first")
    with socket.socket() as s:
        # Match server restart behavior: closed connections in TIME_WAIT are not
        # active listeners and must not prevent an immediate prepare -> run.
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("127.0.0.1", e["port"]))  # Never connect to an unrelated server on this port.
    home = root / engine
    home.mkdir(exist_ok=True)
    data = home / "data"
    if engine == "prolly":
        pg_bin = binary.parent
        if not data.exists():
            subprocess.run([str(pg_bin / "initdb"), "-D", str(data), "-U", "postgres",
                            "--auth-local=trust", "--auth-host=trust", "--no-locale", "-E", "UTF8"],
                           check=True, stdout=subprocess.DEVNULL)
            with (data / "postgresql.conf").open("a") as f:
                f.write(f"\nlisten_addresses = '127.0.0.1'\nport = {e['port']}\n"
                        "unix_socket_directories = ''\nshared_buffers = '256MB'\n"
                        "autovacuum = off\nmax_wal_size = '8GB'\ncheckpoint_timeout = '30min'\n")
        cmd = [str(pg_bin / "pg_ctl"), "-D", str(data), "-l", str(home / "server.log"), "-w", "start"]
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
        pid = int((data / "postmaster.pid").read_text().splitlines()[0])
    else:
        data.mkdir(exist_ok=True)
        cfg = (f"log_level: warning\nlistener:\n  host: 127.0.0.1\n  port: {e['port']}\n"
               f"data_dir: {json.dumps(str(data))}\ncfg_dir: {json.dumps(str(home / '.doltcfg'))}\n")
        if engine == "doltgres":
            cfg += "user:\n  name: postgres\n  password: password\n"
        else:
            cfg += "behavior:\n  auto_gc_behavior:\n    enable: false\n"
        (home / "server.yaml").write_text(cfg)
        cmd = [str(binary), *( ["sql-server", "--config"] if engine == "dolt" else ["-config"]), str(home / "server.yaml")]
        with (home / "server.log").open("a") as f:
            p = subprocess.Popen(cmd, cwd=home, stdout=f, stderr=f, start_new_session=True)
        pid = p.pid
    write_json(home / "server.json", {"pid": pid, "start_ticks": identity(pid), "command": cmd})
    for _ in range(300):
        if not running(root, engine):
            break
        try:
            with socket.create_connection(("127.0.0.1", e["port"]), timeout=.2):
                log(f"{engine} listening on 127.0.0.1:{e['port']} (PID {pid})")
                return True
        except OSError:
            time.sleep(.1)
    stop(root, c, engine)
    raise RuntimeError(f"{engine} failed to start; see {home / 'server.log'}")


def stop(root, c, engine):
    info = running(root, engine)
    if not info:
        return
    if engine == "prolly":
        subprocess.run([str(Path(c["engines"][engine]["binary"]).parent / "pg_ctl"),
                        "-D", str(root / engine / "data"), "-m", "fast", "-w", "stop"],
                       check=True, stdout=subprocess.DEVNULL)
    else:
        os.kill(info["pid"], signal.SIGTERM)
        for _ in range(600):
            stat = Path(f"/proc/{info['pid']}/stat")
            if not running(root, engine) or (stat.exists() and stat.read_text().rsplit(")", 1)[1].split()[0] == "Z"):
                break
            time.sleep(.1)
        else:
            raise RuntimeError(f"{engine} did not exit; retained server.json for inspection")
        try:
            os.waitpid(info["pid"], os.WNOHANG)
        except ChildProcessError:
            pass
    (root / engine / "server.json").unlink(missing_ok=True)


@contextlib.contextmanager
def server(root, c, engine):
    owned = start(root, c, engine)
    try:
        yield running(root, engine)["pid"]
    finally:
        if owned:
            stop(root, c, engine)


class SQL:
    def __init__(self, root, c, engine, db=None):
        self.engine, self.timeout = engine, c["timeout_seconds"]
        port = c["engines"][engine]["port"]
        if engine == "dolt":
            driver, dsn = "mysql", f"root@tcp(127.0.0.1:{port})/{db or ''}?timeout=15s"
        else:
            driver, dsn = "pgx", f"postgres://postgres:password@127.0.0.1:{port}/{db or 'postgres'}?sslmode=disable&default_query_exec_mode=simple_protocol"
        self.p = subprocess.Popen([str(root / "bin/sql-driver"), driver, dsn],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.p.stdin.close()
        try:
            self.p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.p.terminate()
            self.p.wait(timeout=10)
        self.p.stdout.close()

    def request(self, **r):
        self.p.stdin.write(json.dumps({"timeout": self.timeout, **r}) + "\n")
        self.p.stdin.flush()
        line = self.p.stdout.readline()
        if not line:
            raise RuntimeError("SQL driver exited unexpectedly")
        result = json.loads(line)
        if result.get("error"):
            raise RuntimeError(f"{self.engine}: {result['error']}\nSQL: {r.get('sql', 'load batch')}")
        return result

    def query(self, sql):
        return self.request(sql=sql)

    def scalar(self, sql):
        return self.query(sql)["rows"][0][0]

    def vc_sql(self, op, *args):
        prefix = "CALL" if self.engine == "dolt" else "SELECT"
        return f"{prefix} dolt_{op}({','.join(literal(a) for a in args)})"

    def vc(self, op, *args):
        return self.query(self.vc_sql(op, *args))

    def hash(self, ref="HEAD"):
        fn = "dolt_hashof" if self.engine == "prolly" else "hashof"
        return self.scalar(f"SELECT {fn}({literal(ref)})")

    def diff_sql(self, a, b):
        table = "NULL::bench_rows" if self.engine == "prolly" else "'bench_rows'"
        return f"SELECT count(*) FROM dolt_diff({literal(a)},{literal(b)},{table})"


def literal(value):
    return "'" + str(value).replace("'", "''") + "'"


def disk_size(path):
    apparent = allocated = 0
    for base, _, files in os.walk(path):
        for name in files:
            try:
                s = (Path(base) / name).stat()
                apparent += s.st_size
                allocated += s.st_blocks * 512
            except FileNotFoundError:
                pass
    return {"apparent_bytes": apparent, "allocated_bytes": allocated}
