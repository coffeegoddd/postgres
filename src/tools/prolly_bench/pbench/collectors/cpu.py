"""CPU time during the window, system-wide from /proc/stat.  Server CPU is
the total minus what the client programs (pgbench, BenchBase) used, so the
machine should be otherwise idle."""

from __future__ import annotations

import os

from pbench.core import registry
from pbench.core.interfaces import Collector
from pbench.core.stats import per

_HZ = os.sysconf("SC_CLK_TCK")


def busy_seconds() -> float:
    with open("/proc/stat") as f:
        fields = [int(x) for x in f.readline().split()[1:]]
    user, nice, system, idle, iowait, irq, softirq, steal = fields[:8]
    return (user + nice + system + irq + softirq + steal) / _HZ


@registry.collectors.register("cpu")
class CpuCollector(Collector):
    name = "cpu"

    def start(self, ctx):
        self.before = busy_seconds()

    def stop(self, ctx):
        self.after = busy_seconds()

    def results(self, ctx, outcome):
        total = self.after - self.before
        server = total - outcome.client_cpu_seconds \
            if outcome.client_cpu_seconds is not None else None
        n = outcome.transactions
        return {
            "cpu_seconds": total,
            "client_cpu_seconds": outcome.client_cpu_seconds,
            "server_cpu_seconds": server,
            "server_cpu_ms_per_txn": None if per(server, n) is None else per(server, n) * 1000,
        }
