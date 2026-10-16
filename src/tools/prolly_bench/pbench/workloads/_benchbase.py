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
import subprocess
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


def _config(ctx: Context, bench: str, scalefactor: float, seconds: int) -> Path:
    if bench not in {"tpcc", "tpch"}:
        raise ValueError(f"no BenchBase config template for {bench}")
    if not math.isfinite(scalefactor) or scalefactor <= 0:
        raise ValueError("BenchBase scale factor must be finite and positive")
    serial = bench == "tpch" and ctx.params.get("mode", "serial") == "serial"
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
  <scalefactor>{scalefactor}</scalefactor>
  <terminals>{1 if serial else ctx.key.clients}</terminals>
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
    path = ctx.raw_dir / f"benchbase-{bench}.xml"
    path.write_text(xml)
    return path


def _run(ctx: Context, bench: str, cfg: Path, out_dir: Path, *, create: bool,
         load: bool, execute: bool, tag: str) -> str:
    args = ["java", "-jar", "benchbase.jar", "-b", bench, "-c", str(cfg.resolve()),
            f"--create={str(create).lower()}", f"--load={str(load).lower()}",
            f"--execute={str(execute).lower()}", "-d", str(out_dir.resolve())]
    if execute:
        out_dir.mkdir(parents=True, exist_ok=True)
        args += ["--json-histograms", str((out_dir / "transactions.json").resolve())]
    r = subprocess.run(args, cwd=_dist(ctx), capture_output=True, text=True)
    out = " ".join(args) + "\n\n" + r.stdout + r.stderr
    (ctx.raw_dir / f"benchbase-{tag}.out").write_text(out)
    check_client_error(out)
    if r.returncode != 0:
        raise RuntimeError(f"BenchBase {tag} failed (exit {r.returncode}): {out[-2000:]}")
    # Some loaders catch SQL exceptions, log them and still exit zero.
    errors = [line for line in out.splitlines()
              if re.search(r"\bERROR\b", line)]
    if load and errors:
        raise RuntimeError(f"BenchBase {tag} logged errors: {'; '.join(errors)[:2000]}")
    return out


def load(ctx: Context, bench: str, *, scalefactor: float) -> None:
    cfg = _config(ctx, bench, scalefactor, 1)
    _run(ctx, bench, cfg, ctx.raw_dir / "benchbase-load", create=True, load=True,
         execute=False, tag="load")


def execute(ctx: Context, bench: str, window: Optional[Window], *,
            scalefactor: float, seconds: Optional[int] = None) -> Outcome:
    """Run the benchmark; inside window if one is given."""
    seconds = ctx.measure_seconds if seconds is None else seconds
    tag = "measure" if window else "warmup"
    # BenchBase writes per-transaction samples during the run: keep them in
    # RAM, and copy only the small summary files to the results.
    out_dir = ctx.scratch_dir / f"benchbase-{tag}"
    cfg = _config(ctx, bench, scalefactor, seconds)
    cpu0 = children_cpu_seconds()
    with window or nullcontext():
        output = _run(ctx, bench, cfg, out_dir, create=False, load=False, execute=True,
                      tag=tag)
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
    unexpected = counts["unexpected"]["NUM_SAMPLES"]
    # This BenchBase revision treats PostgreSQL deadlocks as unexpected,
    # not retryable. They are ordinary contention failures in TPC-C; keep
    # them in failed_transactions, but reject any other unexpected error.
    deadlocks = len(re.findall(r"will not be retried\.\.\. sql state \[40P01\]", output)) \
        if bench == "tpcc" else 0
    if unexpected != deadlocks:
        raise RuntimeError(f"BenchBase {tag} had {unexpected} unexpected SQL errors")
    if bench == "tpch":
        if counts["aborted"]["NUM_SAMPLES"] or counts["rejected"]["NUM_SAMPLES"]:
            raise RuntimeError(f"TPC-H {tag} had aborted or rejected queries")
        if ctx.params.get("mode", "serial") == "serial":
            # BenchBase's untimed serial mode executes each query cold,
            # then hot. Raw samples contain only the hot run; the success
            # histogram includes both executions.
            if (set(queries) != set(_TPCH_TYPES)
                    or any(query["count"] != 1 for query in queries.values())
                    or counts["completed"]["NUM_SAMPLES"] != 44):
                raise RuntimeError(f"TPC-H {tag} did not complete one cold and one measured run of Q1-Q22")
    metrics = {
        "tps": summary.get("Throughput (requests/second)"),
        "goodput_tps": summary.get("Goodput (requests/second)"),
        "transactions": requests,
        "failed_transactions": unexpected,
        "latency_mean_ms": ms(hist.mean()),
        "latency_p50_ms": ms(hist.percentile(50)),
        "latency_p99_ms": ms(hist.percentile(99)),
        "latency_p999_ms": ms(hist.percentile(99.9)),
        "latency_max_ms": ms(hist.max_us or None),
    }
    return Outcome(metrics=metrics, transactions=requests,
                   client_cpu_seconds=client_cpu,
                   notes=[f"BenchBase final state: {summary.get('Final State')}"])
