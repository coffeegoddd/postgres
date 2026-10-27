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
from engines import ENGINES, literal
# Each pass is a separate set of measured observations; any subset may run, and
# separate runs' passes can be merged into one report (report DIR DIR...).
# timing: latency alone. memory: server-tree PSS/RSS/private memory, sampled
# every sample_interval_ms. cpu: CPU time and page faults; io: kernel read and
# write bytes -- both from one snapshot before and one after, nothing sampled
# in between to perturb them.  counters: the engine's own work counters
# (Engine.counters()), read before and after, for engines that keep them.
PASSES = ("timing", "memory", "cpu", "io", "counters")


def engine_passes(c, engine):
    """The configured passes an engine runs: the counters pass only where
    the engine keeps work counters."""
    return [p for p in c["passes"] if p != "counters" or ENGINES[engine].has_counters]


def check_passes(passes):
    if not passes or set(passes) - set(PASSES) or len(set(passes)) != len(passes):
        raise ValueError("passes must select distinct passes among " + ", ".join(PASSES))


def default_dirty_sizes():
    # (four sizes: the least a dirty-row scaling fit takes)
    return {"xsmall": {"fraction": 0.00001}, "small": {"fraction": 0.0001},
            "medium": {"fraction": 0.01}, "large": {"fraction": 0.1}}


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
    config = {
        "schema_version": 1, "payload_bytes": 128 if smoke else 1024,
        "rows": [128, 512, 2048, 8192] if smoke else [],
        "sizes_gb": [] if smoke else [0.01, 0.1, 1, 10],
        "changes": [8] if smoke else [1000],
        "dirty_sizes": default_dirty_sizes(),
        "history_depths": [1, 4] if smoke else [32, 100, 500],
        "repetitions": 2 if smoke else 1, "warmups": 0,
        "passes": list(PASSES), "sample_interval_ms": 10,
        "batch_rows": 1000, "timeout_seconds": 3600, "seed": 20261021,
        "engines": {name: e.default_config(root, sources) for name, e in ENGINES.items()},
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
    if c["batch_rows"] > 10000 or c["warmups"] < 0 or c["sample_interval_ms"] < 1:
        raise ValueError("batch_rows <= 10000, warmups >= 0, sample_interval_ms >= 1 required")
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
        commands.append((engine, *ENGINES[engine].build_command(root, c, jobs)))
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
    pid, cmd = ENGINES[engine].start(root, c, home, binary)
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
    ENGINES[engine].stop(root, c, info, lambda: running(root, engine))
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
        self.adapter = ENGINES[engine]
        driver, dsn = self.adapter.connection(c["engines"][engine]["port"], db)
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
        return self.adapter.vc_sql(op, args)

    def vc(self, op, *args):
        return self.query(self.vc_sql(op, *args))

    def hash_sql(self, ref="HEAD"):
        return self.adapter.hash_sql(ref)

    def hash(self, ref="HEAD"):
        return self.scalar(self.hash_sql(ref))

    def diff_sql(self, a, b):
        return self.adapter.diff_sql(a, b)

    def ids_in(self, ids):
        return self.adapter.ids_in(ids)

    def table_count_sql(self):
        return self.adapter.table_count_sql()

    def counters(self):
        return self.adapter.counters(self)

    def window_state(self, branches):
        return self.adapter.window_state(self, branches)

    def target(self, chain, index, c):
        return self.adapter.target(self, chain, index, c)

    def path_counters(self):
        return self.adapter.path_counters(self)

    def at_sql(self, commit, ids):
        return self.adapter.at_sql(commit, ids)

    def merge_sql(self, branch, message):
        return self.adapter.merge_sql(branch, message)

    def before_merge(self):
        return self.adapter.before_merge(self)


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
