"""Machine-wide busy CPU, explicitly not attributed to the database server."""

from __future__ import annotations

import os

from pbench.core import registry
from pbench.core.interfaces import Collector
from pbench.core.stats import counter_delta

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
        total = counter_delta({"busy": self.before}, {"busy": self.after})["busy"]
        # /proc/stat cannot identify the server. Do not subtract child CPU
        # from a machine-wide counter and publish it as server CPU.
        return {
            "host_busy_cpu_seconds": total,
            "client_cpu_seconds": outcome.client_cpu_seconds,
        }
