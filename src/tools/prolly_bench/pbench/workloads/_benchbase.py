"""Running BenchBase (https://github.com/cmu-db/benchbase) benchmarks.

[tools] benchbase in the config points at an unpacked BenchBase Postgres
distribution (the directory holding benchbase.jar).  Tables are created with
the table AM under test by passing default_table_access_method in the JDBC
connection options.
"""

from __future__ import annotations

import csv
import json
import math
import re
import shutil
import time
from pbench.core import process
from contextlib import nullcontext
from pathlib import Path
from typing import Optional
from xml.sax.saxutils import escape

from pbench.core.interfaces import Context, Outcome, Unavailable, Window
from pbench.core.stats import LatencyHistogram
from pbench.workloads._common import (check_client_error, children_cpu_seconds,
                                      save_histogram)

_TPCC_TYPES = ["NewOrder", "Payment", "OrderStatus", "Delivery", "StockLevel"]
_TPCH_TYPES = [f"Q{i}" for i in range(1, 23)]


def _dist(ctx: Context) -> Path:
    return Path(ctx.tools.get("benchbase", ""))


def check(ctx: Context) -> None:
    if shutil.which("java") is None:
        raise Unavailable("java is not installed")
    if not (_dist(ctx) / "benchbase.jar").exists():
        raise Unavailable(f"no benchbase.jar under [tools] benchbase = {_dist(ctx)}")


def _config(ctx: Context, bench: str, scalefactor: float, seconds: int,
            serial: bool = False, suffix: str = "") -> Path:
    if bench not in {"tpcc", "tpch"}:
        raise ValueError(f"no BenchBase config template for {bench}")
    if not math.isfinite(scalefactor) or scalefactor <= 0:
        raise ValueError("BenchBase scale factor must be finite and positive")
    names = _TPCC_TYPES if bench == "tpcc" else _TPCH_TYPES
    weights = ctx.params.get("weights", "45,43,4,4,4" if bench == "tpcc"
                             else ",".join("1" for _ in names))
    isolation = ctx.params.get("isolation", "TRANSACTION_SERIALIZABLE")
    types = "\n".join(f"    <transactiontype><name>{t}</name></transactiontype>"
                      for t in names)
    xml = f"""<?xml version="1.0"?>
<parameters>
  <type>POSTGRES</type>
  <driver>org.postgresql.Driver</driver>
  <url>{escape(ctx.cluster.jdbc_url(ctx.dbname, ctx.session_options()))}&amp;reWriteBatchedInserts=true</url>
  <username>postgres</username>
  <password></password>
  <reconnectOnConnectionFailure>true</reconnectOnConnectionFailure>
  <isolation>{escape(isolation)}</isolation>
  <batchsize>128</batchsize>
  <randomSeed>{ctx.params.get('seed', 1)}</randomSeed>
  <scalefactor>{scalefactor}</scalefactor>
  <terminals>{ctx.key.clients}</terminals>
  <works>
    <work>
      <serial>{str(serial).lower()}</serial>
      <time>{0 if serial else seconds}</time>
      <rate>unlimited</rate>
      <weights>{escape(weights)}</weights>
    </work>
  </works>
  <transactiontypes>
{types}
  </transactiontypes>
</parameters>
"""
    path = ctx.raw_dir / f"benchbase-{bench}{suffix}.xml"
    path.write_text(xml)
    return path


def _run(ctx: Context, bench: str, cfg: Path, out_dir: Path, *, create: bool,
         load: bool, execute: bool, tag: str) -> str:
    args = ["java", "-jar", "benchbase.jar", "-b", bench, "-c", str(cfg.resolve()),
            f"--create={str(create).lower()}", f"--load={str(load).lower()}",
            f"--execute={str(execute).lower()}", "-d", str(out_dir.resolve())]
    if tag == "correctness":
        args.insert(1, "-Dpbench.answers=" + str((ctx.raw_dir / "tpch-answers.jsonl").resolve()))
    if execute:
        out_dir.mkdir(parents=True, exist_ok=True)
        args += ["--json-histograms", str((out_dir / "transactions.json").resolve())]
    r = process.run(args, cwd=_dist(ctx), capture_output=True, text=True,
                    timeout=ctx.params.get("timeout_seconds"))
    logged = r.stdout + r.stderr
    out = " ".join(args) + "\n\n" + logged
    (ctx.raw_dir / f"benchbase-{tag}.out").write_text(out)
    check_client_error(out)
    if r.returncode != 0:
        raise RuntimeError(f"BenchBase {tag} failed (exit {r.returncode}): {out[-2000:]}")
    if load:
        # Some loaders catch SQL exceptions, log them and still exit zero.
        # Only BenchBase's own ERROR level counts, not the word elsewhere
        # (the command line's paths, a driver's message).
        errors = [line for line in logged.splitlines()
                  if re.match(r"\s*\[ERROR\s*\]", line)]
        if errors:
            raise RuntimeError(f"BenchBase {tag} logged errors: {'; '.join(errors)[:2000]}")
    return logged


def load(ctx: Context, bench: str, *, scalefactor: float) -> None:
    cfg = _config(ctx, bench, scalefactor, 1)
    _run(ctx, bench, cfg, ctx.raw_dir / "benchbase-load", create=True, load=True,
         execute=False, tag="load")


def execute(ctx: Context, bench: str, window: Optional[Window], *,
            scalefactor: float, seconds: Optional[int] = None,
            serial: bool = False) -> Outcome:
    """Run the benchmark; inside window if one is given.

    serial (TPC-H only): Q1-Q22 once each in order, untimed, instead of for
    seconds.
    """
    seconds = ctx.measure_seconds if seconds is None else seconds
    tag = "measure" if window else "warmup"
    # BenchBase writes per-transaction samples during the run: keep them in
    # RAM, and copy only the small summary files to the results.
    out_dir = ctx.scratch_dir / f"benchbase-{tag}"
    cfg = _config(ctx, bench, scalefactor, seconds, serial)
    with window or nullcontext():
        cpu0 = children_cpu_seconds()
        started = time.monotonic()
        output = _run(ctx, bench, cfg, out_dir, create=False, load=False, execute=True,
                      tag=tag)
        process_seconds = time.monotonic() - started
        client_cpu = children_cpu_seconds() - cpu0

    summary_files = sorted(out_dir.glob("*.summary.json"))
    if not summary_files:
        raise RuntimeError(f"BenchBase wrote no summary in {out_dir}")
    summary = json.loads(summary_files[-1].read_text())

    hist = LatencyHistogram()
    queries: dict[str, dict[str, float]] = {}
    for raw in out_dir.glob("*.raw.csv"):
        with open(raw) as f:
            for row in csv.DictReader(f):
                latency = float(row["Latency (microseconds)"])
                hist.add(latency)
                if bench == "tpch":
                    query = queries.setdefault(row["Transaction Name"],
                                               {"count": 0, "total_ms": 0.0})
                    query["count"] += 1
                    query["total_ms"] += latency / 1000
        raw.unlink()
    keep = ctx.raw_dir / f"benchbase-{tag}"
    keep.mkdir(exist_ok=True)
    for f in out_dir.iterdir():
        if not f.name.endswith(".samples.csv"):
            shutil.copy2(f, keep / f.name)
    if bench == "tpch":
        (keep / "queries.json").write_text(json.dumps({
            name: {**query, "mean_ms": query["total_ms"] / query["count"]}
            for name, query in queries.items()}, indent=2) + "\n")
    save_histogram(ctx, tag, hist)

    def ms(us):
        return None if us is None else us / 1000.0

    requests = float(summary.get("Measured Requests", 0))
    if summary.get("Final State") not in {"DONE", "EXIT"} or requests <= 0:
        raise RuntimeError(f"BenchBase {tag} did not complete useful work: {summary}")
    counts = json.loads((keep / "transactions.json").read_text())
    for kind in ("completed", "aborted", "rejected", "unexpected"):
        count = counts[kind]["NUM_SAMPLES"]
        if type(count) is not int or count < 0:
            raise RuntimeError(f"invalid BenchBase {kind} count")
    completed = counts["completed"]["NUM_SAMPLES"]
    if not completed or requests != hist.count:
        raise RuntimeError("BenchBase has no successful work or incomplete latency records")
    unexpected = counts["unexpected"]["NUM_SAMPLES"]
    # This BenchBase revision treats PostgreSQL deadlocks as unexpected,
    # not retryable. They are ordinary contention failures in TPC-C; keep
    # them in failed_transactions, but reject any other unexpected error.
    # (BenchBase counts in the histogram only errors while measuring, but
    # logs every one, and may log one twice: the log can have more.)
    failed = re.findall(r"will not be retried\.\.\. sql state \[(\w*)\]", output)
    deadlocks = failed.count("40P01") if bench == "tpcc" else 0
    others = len(failed) - deadlocks
    if others or unexpected > deadlocks:
        raise RuntimeError(f"BenchBase {tag} had {max(others, unexpected - deadlocks)} "
                           "unexpected SQL errors")
    if bench == "tpch":
        if counts["aborted"]["NUM_SAMPLES"] or counts["rejected"]["NUM_SAMPLES"]:
            raise RuntimeError(f"TPC-H {tag} had aborted or rejected queries")
        if serial:
            # Raw samples have each query's measured run; anything else
            # isn't the serial run of Q1-Q22.
            if (set(queries) != set(_TPCH_TYPES)
                    or any(query["count"] != 1 for query in queries.values())):
                raise RuntimeError(f"TPC-H {tag} did not complete one measured run of Q1-Q22")
    metrics = {
        "latency_samples": hist.count,
        "completed_operations": completed,
        "aborted_operations": counts["aborted"]["NUM_SAMPLES"],
        "rejected_operations": counts["rejected"]["NUM_SAMPLES"],
        # Upstream latency samples count attempts in its active phase,
        # while outcome histograms span the entire client lifecycle.
        # Its goodput divides the latter by the former's time, and can
        # exceed throughput. Keep these as client-reported diagnostics.
        "client_reported_tps": summary.get("Throughput (requests/second)"),
        "client_reported_goodput_tps": summary.get("Goodput (requests/second)"),
        "measured_requests": requests,
        "tps": completed / process_seconds,
        "client_process_seconds": process_seconds,
        "transactions": completed,
        "failed_transactions": unexpected,
        "latency_mean_ms": ms(hist.mean()),
        "latency_p50_ms": ms(hist.reported_percentile(50)),
        "latency_p99_ms": ms(hist.reported_percentile(99)),
        "latency_p999_ms": ms(hist.reported_percentile(99.9)),
        "latency_max_ms": ms(hist.max_us or None),
    }
    return Outcome(metrics=metrics, transactions=completed,
                   client_cpu_seconds=client_cpu,
                   notes=[f"BenchBase final state: {summary.get('Final State')}",
                          "throughput and resource denominator use successful operations over the client process; "
                          "latencies cover active-phase attempts, including failures"])
