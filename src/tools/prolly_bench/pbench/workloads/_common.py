"""Helpers shared by workloads: running pgbench and timed SQL scripts."""

from __future__ import annotations

import os
import re
import resource
import signal
import subprocess
from pbench.core import process
import time
import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional
from contextlib import nullcontext

from pbench.core.interfaces import Context, NotImplementedByEngine
from pbench.core.sqlerr import raise_for_sql_error
from pbench.core.stats import LatencyHistogram

SCRIPTS_DIR = Path(__file__).resolve().parents[2] / "scripts"

# Every prolly "not implemented" error says this (prolly_impl.h,
# prolly_tableam.c, prolly_indexam.c).  Used only where a client program
# doesn't report SQLSTATEs; the runner also checks the server log.
_NOT_IMPLEMENTED_RE = re.compile(r"ERROR:\s+(.*does not implement.*)")


def children_cpu_seconds() -> float:
    ru = resource.getrusage(resource.RUSAGE_CHILDREN)
    return ru.ru_utime + ru.ru_stime


def check_client_error(output: str) -> None:
    m = _NOT_IMPLEMENTED_RE.search(output)
    if m:
        raise NotImplementedByEngine(m.group(1).strip())


# ---------------------------------------------------------------------------
# pgbench
# ---------------------------------------------------------------------------

@dataclass
class PgbenchResult:
    transactions: float = 0
    failed: float = 0
    skipped: float = 0
    tps: Optional[float] = None
    latency: LatencyHistogram = field(default_factory=LatencyHistogram)
    client_cpu_seconds: float = 0.0
    output: str = ""

    def metrics(self) -> dict[str, Optional[float]]:
        def ms(us):
            return None if us is None else us / 1000.0
        return {
            "latency_samples": self.latency.count,
            "tps": self.tps,
            "transactions": self.transactions,
            "failed_transactions": self.failed,
            "skipped_transactions": self.skipped,
            "attempted_operations": self.transactions + self.failed,
            "completed_operations": self.transactions,
            "latency_mean_ms": ms(self.latency.mean()),
            "latency_p50_ms": ms(self.latency.reported_percentile(50)),
            "latency_p99_ms": ms(self.latency.reported_percentile(99)),
            "latency_p999_ms": ms(self.latency.reported_percentile(99.9)),
            "latency_max_ms": ms(self.latency.max_us or None),
        }


def parse_pgbench_output(text: str) -> dict[str, Optional[float]]:
    out: dict[str, Optional[float]] = {}
    m = re.search(r"number of transactions actually processed: (\d+)", text)
    out["transactions"] = float(m.group(1)) if m else None
    m = re.search(r"number of failed transactions: (\d+)", text)
    out["failed"] = float(m.group(1)) if m else 0.0
    m = re.search(r"number of transactions skipped: (\d+)", text)
    out["skipped"] = float(m.group(1)) if m else 0.0
    m = re.search(r"tps = ([\d.]+) \(without initial connection time\)", text)
    out["tps"] = float(m.group(1)) if m else None
    return out


def parse_pgbench_logs(log_dir: Path, prefix: str, hist: LatencyHistogram) -> int:
    """Feed per-transaction latencies from pgbench --log files into hist,
    deleting the logs.  Returns the number of failed/skipped transactions."""
    failed = 0
    # pgbench names its logs <prefix>.<pid> and <prefix>.<pid>.<thread>.
    name_re = re.compile(re.escape(prefix) + r"\.\d+(\.\d+)?$")
    for path in sorted(log_dir.iterdir()):
        if not name_re.match(path.name):
            continue
        with open(path) as f:
            for line in f:
                parts = line.split()
                if len(parts) < 3:
                    raise ValueError(f"malformed pgbench latency record in {path}")
                if parts[2] in ("failed", "skipped"):
                    failed += 1
                else:
                    hist.add(float(parts[2]))
        path.unlink()
    return failed


def pgbench_init(ctx: Context, scale: int) -> None:
    """pgbench -i into ctx.dbname with the table AM under test."""
    r = process.run(
        [ctx.cluster.bin("pgbench"), "-i", "-q", "-s", str(scale), "-I", "dtGvp",
         ctx.dbname],
        capture_output=True, text=True,
        env=ctx.cluster.client_env(ctx.session_options()))
    (ctx.raw_dir / "pgbench-init.log").write_text(r.stdout + r.stderr)
    if r.returncode != 0:
        check_client_error(r.stderr)
        raise RuntimeError(f"pgbench -i failed: {r.stderr[-2000:]}")


def run_pgbench(ctx: Context, *, seconds: int, builtin: Optional[str] = None,
                scripts: Optional[list[tuple[str, int]]] = None,
                defines: Optional[dict[str, object]] = None,
                clients: Optional[int] = None, tag: str = "measure",
                log_latency: bool = True, window=None) -> PgbenchResult:
    """Run pgbench for seconds; tag names the raw output files."""
    clients = clients or ctx.key.clients
    threads = max(1, min(clients, os.cpu_count() or 1))
    args = [ctx.cluster.bin("pgbench"), "-n", "-c", str(clients), "-j", str(threads),
            "-T", str(seconds), "--random-seed", str(ctx.params.get("seed", 1)),
            "-M", str(ctx.params.get("protocol", "prepared"))]
    if builtin:
        args += ["-b", builtin]
    if ctx.params.get("rate"):
        args += ["--rate", str(ctx.params["rate"])]
    if ctx.params.get("latency_limit_ms"):
        args += ["--latency-limit", str(ctx.params["latency_limit_ms"])]
    for name, weight in scripts or []:
        args += ["-f", f"{SCRIPTS_DIR / name}@{weight}"]
    for k, v in (defines or {}).items():
        args += ["-D", f"{k}={v}"]
    log_prefix = f"pgbench-{tag}"
    rate = float(ctx.params.get("latency_sampling_rate", 1.0))
    if log_latency:
        # Logs go to RAM, not the device under test; only the histogram is kept.
        args += ["-l", f"--log-prefix={ctx.scratch_dir / log_prefix}"]
        if rate < 1.0:
            args += [f"--sampling-rate={rate}"]
    args.append(ctx.dbname)

    with window or nullcontext():
        cpu0 = children_cpu_seconds()
        r = process.run(args, capture_output=True, text=True, timeout=seconds + 120,
                           env=ctx.cluster.client_env(ctx.session_options()))
        client_cpu = children_cpu_seconds() - cpu0
    res = PgbenchResult(client_cpu_seconds=client_cpu,
                        output=r.stdout + r.stderr)
    (ctx.raw_dir / f"{log_prefix}.out").write_text(" ".join(args) + "\n\n" + res.output)
    if r.returncode != 0:
        check_client_error(res.output)
        raise RuntimeError(f"pgbench failed (exit {r.returncode}): {res.output[-2000:]}")

    parsed = parse_pgbench_output(res.output)
    res.transactions = parsed["transactions"] or 0
    res.failed = parsed["failed"] or 0
    res.skipped = parsed["skipped"] or 0
    res.tps = parsed["tps"]
    if res.transactions <= 0 or res.tps is None or res.tps <= 0:
        raise RuntimeError("pgbench completed no useful work or omitted throughput")
    if log_latency:
        failed_samples = parse_pgbench_logs(ctx.scratch_dir, log_prefix, res.latency)
        if rate == 1.0 and (res.latency.count != res.transactions
                            or failed_samples != res.failed + res.skipped):
            raise RuntimeError("pgbench latency records do not cover all reported operations")
        save_histogram(ctx, tag, res.latency, rate)
    return res


def save_histogram(ctx: Context, tag: str, hist: LatencyHistogram,
                   sampling_rate: float = 1.0) -> None:
    import json
    d = hist.to_dict()
    d["sampling_rate"] = sampling_rate
    (ctx.raw_dir / f"latency-{tag}.json").write_text(json.dumps(d) + "\n")


class BackgroundPgbench:
    """pgbench running alongside a measurement, reporting progress each
    second, so its throughput during the window can be computed."""

    def __init__(self, ctx: Context, builtin: str, clients: int, tag: str):
        self.ctx = ctx
        self.out_path = ctx.raw_dir / f"pgbench-{tag}.out"
        self.args = [ctx.cluster.bin("pgbench"), "-n", "-c", str(clients),
                     "-j", str(max(1, min(clients, os.cpu_count() or 1))),
                     "--random-seed", str(ctx.params.get("seed", 1)),
                     "-T", "86400", "-P", "1", "-b", builtin, ctx.dbname]
        self.proc: Optional[subprocess.Popen] = None

    def __enter__(self):
        self.out = open(self.out_path, "w")
        self.proc = subprocess.Popen(
            self.args, stdout=self.out, stderr=subprocess.STDOUT, text=True,
            env=self.ctx.cluster.client_env(self.ctx.session_options()))
        time.sleep(2)  # let clients connect
        if self.proc.poll() is not None:
            self.out.close()
            raise RuntimeError("background pgbench exited during startup")
        return self

    def __exit__(self, *exc):
        assert self.proc is not None
        self.proc.send_signal(signal.SIGINT)
        try:
            self.proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        self.out.close()
        if self.proc.returncode not in (0, -signal.SIGINT):
            raise RuntimeError(f"background pgbench failed: exit {self.proc.returncode}")
        parsed = parse_pgbench_output(self.out_path.read_text())
        if not parsed.get("transactions") or not parsed.get("tps"):
            raise RuntimeError("background pgbench completed no useful work")
        if parsed.get("failed"):
            raise RuntimeError(f"background pgbench failed transactions: {parsed['failed']}")

    def tps(self) -> Optional[float]:
        rates = [float(m.group(1)) for m in re.finditer(
            r"progress: [\d.]+ s, ([\d.]+) tps", self.out_path.read_text())]
        rates = rates[1:]  # first second includes connection ramp-up
        return sum(rates) / len(rates) if rates else None


# ---------------------------------------------------------------------------
# Timed SQL in one session
# ---------------------------------------------------------------------------

_TIME_RE = re.compile(r"^Time: ([\d.]+) ms", re.M)


def timed_session(ctx: Context, statements: list[str], tag: str,
                  cluster=None) -> list[float]:
    """Run statements in one psql session; return each one's time in ms.

    One session, because version-control state such as the checked-out
    branch belongs to the session.
    """
    cluster = cluster or ctx.cluster
    if ctx.table_am == "prolly_d1":
        def snap(i, phase):
            return ("\\timing off\nSELECT 'PBENCH_COUNTER|" + str(i) + "|" + phase +
                    "|' || nodes_read || '|' || nodes_written "
                    "FROM prolly_node_counters('prolly_d1');\n")
        script = "".join(snap(i, "before") + "\\timing on\n" + sql.rstrip().rstrip(";") + ";\n" +
                         snap(i, "after") for i, sql in enumerate(statements))
    else:
        script = "\\timing on\n" + "\n".join(s.rstrip().rstrip(";") + ";"
                                             for s in statements) + "\n"
    (ctx.raw_dir / f"{tag}.sql").write_text(script)
    res = cluster.sql(script, dbname=ctx.dbname, options=ctx.session_options(),
                      check=False)
    (ctx.raw_dir / f"{tag}.out").write_text(res.stdout + res.stderr)
    raise_for_sql_error(res, script)
    times = [float(t) for t in _TIME_RE.findall(res.stdout + res.stderr)]
    if len(times) != len(statements):
        raise RuntimeError(f"expected {len(statements)} timings, got {len(times)}")
    if ctx.table_am == "prolly_d1":
        snapshots = {}
        for i, phase, reads, writes in re.findall(r"^PBENCH_COUNTER\|(\d+)\|(before|after)\|(\d+)\|(\d+)$", res.stdout, re.M):
            key = (int(i), phase)
            if key in snapshots:
                raise RuntimeError("duplicate backend counter snapshot")
            snapshots[key] = (int(reads), int(writes))
        if len(snapshots) != 2 * len(statements):
            raise RuntimeError("missing backend counter snapshots")
        counters = []
        for i, sql in enumerate(statements):
            delta = [a - b for a, b in zip(snapshots[i, "after"], snapshots[i, "before"])]
            if min(delta) < 0:
                raise RuntimeError("backend counters reset during an operation")
            counters.append({"sql": sql, "milliseconds": times[i],
                             "nodes_read": delta[0], "nodes_written": delta[1]})
        (ctx.raw_dir / f"{tag}-counters.json").write_text(json.dumps(counters, indent=2) + "\n")
    return times
