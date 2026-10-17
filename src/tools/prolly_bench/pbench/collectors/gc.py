"""Explicit post-run engine GC, with cluster-wide retained storage accounting.

prolly's chunk-store GC isn't built yet: until it is, every run reports
the collector as not implemented, rather than measuring nothing."""
from pbench.core import registry
from pbench.core.interfaces import Collector, NotApplicable, NotImplementedByEngine


@registry.collectors.register("gc")
class GcCollector(Collector):
    name = "gc"
    post_order = 20

    def check(self, ctx):
        if ctx.table_am != "prolly":
            raise NotApplicable(f"{ctx.table_am} has no engine GC")
        raise NotImplementedByEngine("prolly's chunk-store GC isn't built yet")
