"""Bytes the block device holding the data directory read and wrote during
the window (/proc/diskstats).  Includes any other activity on that device,
so the machine should be otherwise idle."""

from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

from pbench.core import registry
from pbench.core.interfaces import Collector, Unavailable
from pbench.core.stats import per, counter_delta


def device_for(path: Path) -> str:
    """The /proc/diskstats name of the block device holding path."""
    r = subprocess.run(["findmnt", "-n", "-o", "SOURCE", "-T", str(path)],
                       capture_output=True, text=True)
    source = re.sub(r"\[.*\]$", "", r.stdout.strip())
    if not source.startswith("/dev/"):
        raise Unavailable(f"{path} is not on a block device ({source or 'unknown'})")
    return os.path.basename(os.path.realpath(source))


def read_diskstats(device: str) -> dict[str, int]:
    with open("/proc/diskstats") as f:
        for line in f:
            p = line.split()
            if p[2] == device:
                return {"read_bytes": int(p[5]) * 512, "writes": int(p[7]),
                        "write_bytes": int(p[9]) * 512,
                        "flushes": int(p[18]) if len(p) > 18 else 0}
    raise Unavailable(f"device {device} not in /proc/diskstats")


@registry.collectors.register("diskstats")
class DiskstatsCollector(Collector):
    name = "diskstats"

    def check(self, ctx):
        self.device = device_for(ctx.cluster.data_dir)
        read_diskstats(self.device)

    def start(self, ctx):
        self.before = read_diskstats(self.device)

    def stop(self, ctx):
        self.after = read_diskstats(self.device)

    def results(self, ctx, outcome):
        d = counter_delta(self.before, self.after)
        return {
            "device_write_bytes": d["write_bytes"],
            "device_write_bytes_per_operation": per(d["write_bytes"], outcome.transactions),
            "device_read_bytes": d["read_bytes"],
            "device_writes": d["writes"],
            "device_flushes": d["flushes"],
        }
