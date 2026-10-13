"""Running BenchBase (https://github.com/cmu-db/benchbase) benchmarks.

[tools] benchbase in the config points at an unpacked BenchBase Postgres
distribution (the directory holding benchbase.jar).  Tables are created with
the table AM under test by passing default_table_access_method in the JDBC
connection options.
"""

from __future__ import annotations

import csv
import json
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


def _dist(ctx: Context) -> Path:
    return Path(ctx.tools.get("benchbase", ""))


def check(ctx: Context) -> None:
    if shutil.which("java") is None:
        raise Unavailable("java is not installed")
    if not (_dist(ctx) / "benchbase.jar").exists():
        raise Unavailable(f"no benchbase.jar under [tools] benchbase = {_dist(ctx)}")


def _config(ctx: Context, bench: str, scalefactor: int, seconds: int) -> Path:
    if bench != "tpcc":
        raise ValueError(f"no BenchBase config template for {bench}")
    isolation = ctx.params.get("isolation", "TRANSACTION_SERIALIZABLE")
    types = "\n".join(f"    <transactiontype><name>{t}</name></transactiontype>"
                      for t in _TPCC_TYPES)
    xml = f"""<?xml version="1.0"?>
<parameters>
  <type>POSTGRES</type>
  <driver>org.postgresql.Driver</driver>
  <url>{escape(ctx.cluster.jdbc_url(ctx.dbname, ctx.session_options()))}&amp;reWriteBatchedInserts=true</url>
  <username>postgres</username>
  <password></password>
  <reconnectOnConnectionFailure>true</reconnectOnConnectionFailure>
  <isolation>{isolation}</isolation>
  <batchsize>128</batchsize>
  <scalefactor>{scalefactor}</scalefactor>
  <terminals>{ctx.key.clients}</terminals>
  <works>
    <work>
      <time>{seconds}</time>
      <rate>unlimited</rate>
      <weights>{ctx.params.get("weights", "45,43,4,4,4")}</weights>
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
    r = subprocess.run(args, cwd=_dist(ctx), capture_output=True, text=True)
    out = " ".join(args) + "\n\n" + r.stdout + r.stderr
    (ctx.raw_dir / f"benchbase-{tag}.out").write_text(out)
    check_client_error(out)
    if r.returncode != 0:
        raise RuntimeError(f"BenchBase {tag} failed (exit {r.returncode}): {out[-2000:]}")
    return out


def load(ctx: Context, bench: str, *, scalefactor: int) -> None:
    cfg = _config(ctx, bench, scalefactor, 1)
    _run(ctx, bench, cfg, ctx.raw_dir / "benchbase-load", create=True, load=True,
         execute=False, tag="load")


def execute(ctx: Context, bench: str, window: Optional[Window], *,
            scalefactor: int, seconds: Optional[int] = None) -> Outcome:
    """Run the benchmark; inside window if one is given."""
    seconds = seconds or ctx.measure_seconds
    tag = "measure" if window else "warmup"
    # BenchBase writes per-transaction samples during the run: keep them in
    # RAM, and copy only the small summary files to the results.
    out_dir = ctx.scratch_dir / f"benchbase-{tag}"
    cfg = _config(ctx, bench, scalefactor, seconds)
    cpu0 = children_cpu_seconds()
    with window or nullcontext():
        _run(ctx, bench, cfg, out_dir, create=False, load=False, execute=True,
             tag=tag)
    client_cpu = children_cpu_seconds() - cpu0

    summary_files = sorted(out_dir.glob("*.summary.json"))
    if not summary_files:
        raise RuntimeError(f"BenchBase wrote no summary in {out_dir}")
    summary = json.loads(summary_files[-1].read_text())

    hist = LatencyHistogram()
    for raw in out_dir.glob("*.raw.csv"):
        with open(raw) as f:
            for row in csv.DictReader(f):
                hist.add(float(row["Latency (microseconds)"]))
        raw.unlink()
    keep = ctx.raw_dir / f"benchbase-{tag}"
    keep.mkdir(exist_ok=True)
    for f in out_dir.iterdir():
        if not f.name.endswith(".samples.csv"):
            shutil.copy2(f, keep / f.name)
    save_histogram(ctx, tag, hist)

    def ms(us):
        return None if us is None else us / 1000.0

    requests = float(summary.get("Measured Requests", 0))
    metrics = {
        "tps": summary.get("Throughput (requests/second)"),
        "goodput_tps": summary.get("Goodput (requests/second)"),
        "transactions": requests,
        "latency_mean_ms": ms(hist.mean()),
        "latency_p50_ms": ms(hist.percentile(50)),
        "latency_p99_ms": ms(hist.percentile(99)),
        "latency_p999_ms": ms(hist.percentile(99.9)),
        "latency_max_ms": ms(hist.max_us or None),
    }
    return Outcome(metrics=metrics, transactions=requests,
                   client_cpu_seconds=client_cpu,
                   notes=[f"BenchBase final state: {summary.get('Final State')}"])
