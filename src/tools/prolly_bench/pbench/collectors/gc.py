"""Explicit post-run engine GC, with cluster-wide retained storage accounting."""
import time
from pbench.core import registry
from pbench.core.interfaces import Collector, NotApplicable
from pbench.collectors.space import physical_size


@registry.collectors.register("gc")
class GcCollector(Collector):
    name = "gc"
    post_order = 20

    def check(self, ctx):
        if ctx.table_am != "prolly_d1":
            raise NotApplicable("GC accounting currently has a D1 adapter only")

    def post(self, ctx, outcome):
        before = physical_size(ctx.cluster.data_dir)[1]
        start = time.monotonic()
        kept, freed = ctx.cluster.scalar("SELECT nodes_kept, nodes_freed FROM prolly_gc()",
                                         dbname=ctx.dbname).split("|")
        elapsed = time.monotonic() - start
        after = physical_size(ctx.cluster.data_dir)[1]
        return {"gc_seconds": elapsed, "gc_nodes_kept": int(kept), "gc_nodes_freed": int(freed),
                "gc_cluster_allocated_before": before, "gc_cluster_allocated_after": after}
