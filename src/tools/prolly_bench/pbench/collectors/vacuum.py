"""Reclamation cost after the window: VACUUM of the workload's tables
(heap vacuum, or the engine's reclamation through relation_vacuum)."""

from __future__ import annotations

import time

from pbench.core import registry
from pbench.core.interfaces import Collector, NotApplicable
from pbench.collectors.diskstats import device_for, read_diskstats


@registry.collectors.register("vacuum")
class VacuumCollector(Collector):
    name = "vacuum"
    post_order = 10

    def post(self, ctx, outcome):
        tables = [t for t in ctx.tables if ctx.cluster.scalar(
            f"SELECT to_regclass('{t}') IS NOT NULL", dbname=ctx.dbname) == "t"]
        if not tables:
            raise NotApplicable("workload has no tables")
        try:
            dev = device_for(ctx.cluster.data_dir)
            before = read_diskstats(dev)
        except Exception:
            dev = None
        t0 = time.monotonic()
        ctx.cluster.sql(f"VACUUM (ANALYZE false) {', '.join(tables)}",
                        dbname=ctx.dbname, options=ctx.session_options())
        out = {"vacuum_seconds": time.monotonic() - t0}
        if dev:
            after = read_diskstats(dev)
            out["vacuum_device_write_bytes"] = after["write_bytes"] - before["write_bytes"]
        return out
