"""Progress on the terminal: one line per cell."""

from __future__ import annotations

import sys

from pbench.core import registry
from pbench.core.interfaces import CellResult, RunInfo, Sink, Status

_SHOW = [("tps", "tps", "{:.0f}"), ("latency_p99_ms", "p99", "{:.2f}ms"),
         ("rows_per_second", "rows/s", "{:.0f}"), ("commit_ms", "commit", "{:.1f}ms"),
         ("pass_percent", "pass", "{:.3f}%"), ("not_ok", "not_ok", "{:.0f}"),
         ("not_implemented", "not_impl", "{:.0f}"), ("crashed", "crashed", "{:.0f}")]


@registry.sinks.register("console")
class ConsoleSink(Sink):
    name = "console"

    def open(self, run: RunInfo) -> None:
        print(f"{run.suite} suite, table AM {run.table_am}, label {run.label}")
        print(f"results: {run.out_dir}")
        for w in run.warnings:
            print(f"WARNING: {w}")
        sys.stdout.flush()

    def cell(self, r: CellResult) -> None:
        k = r.key
        name = f"{k.workload} {k.size}{' ' + k.variant if k.variant else ''} c{k.clients} r{k.rep}"
        line = f"{name:<40} {r.status.value:<16} {r.seconds:6.1f}s"
        if r.status == Status.OK:
            line += "  " + "  ".join(f"{label}={fmt.format(r.metrics[m])}"
                                     for m, label, fmt in _SHOW
                                     if r.metrics.get(m) is not None)
        else:
            line += f"  {r.message[:120]}"
        print(line, flush=True)

    def close(self, run: RunInfo, results: list[CellResult]) -> None:
        counts: dict[str, int] = {}
        for r in results:
            counts[r.status.value] = counts.get(r.status.value, 0) + 1
        print("done: " + ", ".join(f"{v} {k}" for k, v in sorted(counts.items())))
        print(f"results: {run.out_dir}")
