"""Engine-specific counters (commit-pipeline queue depth, lock memory, ...)
from the engine's statistics view.  No engine exposes one yet."""

from __future__ import annotations

from pbench.core import registry
from pbench.core.interfaces import Collector, NotApplicable, NotImplementedByEngine

VIEW = "pg_stat_prolly"


@registry.collectors.register("engine_counters")
class EngineCountersCollector(Collector):
    name = "engine_counters"

    def check(self, ctx):
        if not ctx.table_am.startswith("prolly"):
            raise NotApplicable(f"{ctx.table_am} has no engine counters")
        if ctx.cluster.scalar(f"SELECT to_regclass('{VIEW}') IS NOT NULL") != "t":
            raise NotImplementedByEngine(f"no {VIEW} view yet")

    def _snap(self, ctx):
        rows = ctx.cluster.scalar(f"SELECT name || '=' || value FROM {VIEW}",
                                  dbname=ctx.dbname)
        return {k: float(v) for k, v in (r.split("=", 1) for r in rows.splitlines() if r)}

    def start(self, ctx):
        self.before = self._snap(ctx)

    def stop(self, ctx):
        self.after = self._snap(ctx)

    def results(self, ctx, outcome):
        return {f"engine.{k}": v - self.before.get(k, 0.0) for k, v in self.after.items()}
