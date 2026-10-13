"""Space amplification after the window: on-disk size of the workload's
tables (with indexes) over the logical size of their rows."""

from __future__ import annotations

from pbench.core import registry
from pbench.core.interfaces import Collector, NotApplicable


@registry.collectors.register("space")
class SpaceCollector(Collector):
    name = "space"
    post_order = 0

    def post(self, ctx, outcome):
        tables = [t for t in ctx.tables if ctx.cluster.scalar(
            f"SELECT to_regclass('{t}') IS NOT NULL", dbname=ctx.dbname) == "t"]
        if not tables:
            raise NotApplicable("workload has no tables")
        disk = sum(float(ctx.cluster.scalar(
            f"SELECT pg_total_relation_size('{t}')", dbname=ctx.dbname)) for t in tables)
        logical = sum(float(ctx.cluster.scalar(
            f"SELECT coalesce(sum(pg_column_size(t.*)), 0) FROM {t} t",
            dbname=ctx.dbname)) for t in tables)
        return {"on_disk_bytes": disk, "logical_bytes": logical,
                "space_amplification": disk / logical if logical else None,
                "database_bytes": float(ctx.cluster.scalar(
                    "SELECT pg_database_size(current_database())", dbname=ctx.dbname))}
