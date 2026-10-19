"""The prolly engine's work counters during the window, from pg_stat_prolly
(nodes read and written, chunks appended, bytes hashed, rows folded,
commits, head moves, value-memo lookups): each counter's total, and per
completed unit of work."""

from __future__ import annotations

from pbench.collectors._snapshot import snapshot
from pbench.core import registry
from pbench.core.interfaces import Collector, NotApplicable, NotImplementedByEngine
from pbench.core.stats import counter_delta, per

VIEW = "pg_stat_prolly"


def engine_metrics(before: dict[str, float], after: dict[str, float],
                   denominator) -> dict[str, float]:
    """engine.<counter> (the window's total) and engine.<counter>_per_operation."""
    delta = counter_delta(before, after)
    out = {}
    for name, value in sorted(delta.items()):
        out[f"engine.{name}"] = value
        out[f"engine.{name}_per_operation"] = per(value, denominator)
    return out


@registry.collectors.register("engine_counters")
class EngineCountersCollector(Collector):
    name = "engine_counters"

    def check(self, ctx):
        if not ctx.table_am.startswith("prolly"):
            raise NotApplicable(f"{ctx.table_am} has no engine counters")
        if ctx.cluster.scalar(f"SELECT to_regclass('{VIEW}') IS NOT NULL") != "t":
            raise NotImplementedByEngine(f"no {VIEW} view")

    def _snap(self, ctx):
        rows = snapshot(ctx, f"SELECT string_agg(name || '=' || value, E'\\n') FROM {VIEW}",
                        dbname=ctx.dbname)
        return {k: float(v) for k, v in (r.split("=", 1) for r in rows.splitlines() if r)}

    def start(self, ctx):
        self.before = self._snap(ctx)

    def stop(self, ctx):
        self.after = self._snap(ctx)

    def results(self, ctx, outcome):
        return engine_metrics(self.before, self.after, outcome.transactions)
