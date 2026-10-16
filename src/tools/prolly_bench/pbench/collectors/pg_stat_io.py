"""Buffer-manager I/O during the window, from pg_stat_io: hits, reads,
writes, extends, evictions and fsyncs on relations, summed over backend
types and contexts."""

from __future__ import annotations

from pbench.core import registry
from pbench.core.interfaces import Collector
from pbench.core.stats import counter_delta

_COLS = ["hits", "reads", "writes", "extends", "evictions", "fsyncs",
         "read_bytes", "write_bytes"]


@registry.collectors.register("pg_stat_io")
class PgStatIoCollector(Collector):
    name = "pg_stat_io"

    def _snap(self, ctx):
        sums = ", ".join(f"coalesce(sum({c}), 0)" for c in _COLS)
        vals = ctx.cluster.scalar(
            f"SELECT {sums} FROM pg_stat_io WHERE object = 'relation'").split("|")
        return dict(zip(_COLS, (float(v) for v in vals)))

    def start(self, ctx):
        self.before = self._snap(ctx)

    def stop(self, ctx):
        # Cumulative statistics may lag backend work. Never sleep here:
        # doing so extends every collector stopped after this one.
        self.after = self._snap(ctx)

    def results(self, ctx, outcome):
        d = counter_delta(self.before, self.after)
        lookups = d["hits"] + d["reads"]
        return {
            "io_hit_ratio": d["hits"] / lookups if lookups else None,
            "io_reads": d["reads"],
            "io_writes": d["writes"],
            "io_extends": d["extends"],
            "io_evictions": d["evictions"],
            "io_fsyncs": d["fsyncs"],
            "io_read_bytes": d["read_bytes"],
            "io_write_bytes": d["write_bytes"],
        }
