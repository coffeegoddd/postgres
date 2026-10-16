"""Space amplification after the window: on-disk size of the workload's
tables (with indexes) over the logical size of their rows."""

from __future__ import annotations

from pbench.core import registry
from pbench.core.interfaces import Collector, NotApplicable
from pathlib import Path


def physical_size(root: Path) -> tuple[int, int]:
    """Apparent and allocated bytes, counted once per inode (including WAL)."""
    apparent = allocated = 0
    seen = set()
    for p in root.rglob("*"):
        if p.is_symlink():
            raise RuntimeError(f"external storage requires explicit accounting: {p}")
        try:
            stat = p.stat()
        except FileNotFoundError:
            continue  # transient server files
        key = (stat.st_dev, stat.st_ino)
        if key not in seen:
            seen.add(key)
            apparent += stat.st_size
            allocated += stat.st_blocks * 512
    return apparent, allocated


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
        apparent, allocated = physical_size(ctx.cluster.data_dir)
        return {"relation_bytes": disk, "row_representation_bytes": logical,
                # pg_total_relation_size omits D1's global nodestore. These
                # scopes are explicit; neither is a cross-engine amplification.
                "cluster_apparent_bytes_including_wal": apparent,
                "cluster_allocated_bytes_including_wal": allocated,
                "database_bytes": float(ctx.cluster.scalar(
                    "SELECT pg_database_size(current_database())", dbname=ctx.dbname))}
