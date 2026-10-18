"""Instructions the server's processes executed during the window, counted
by Cachegrind (the cachegrind cluster): the sum over the processes that
ended in it, the measured clients' backends.  For machines without
hardware counters; reported, not gated.

(The sessions the collectors read with end in the window too; theirs are
the same queries every run.)"""

from __future__ import annotations

import re
import time
from pathlib import Path

from pbench.collectors.instructions import postmaster_pid
from pbench.core import registry
from pbench.core.interfaces import Collector, Unavailable
from pbench.core.stats import per

_SUMMARY = re.compile(r"^summary:\s+(\d+)", re.M)


def instructions(path: Path) -> int:
    """Ir from a cachegrind.out file (its summary line's first event)."""
    m = _SUMMARY.search(path.read_text(errors="replace"))
    if not m:
        raise RuntimeError(f"no summary in {path}")
    return int(m.group(1))


def children(pid: int) -> set[int]:
    try:
        text = Path(f"/proc/{pid}/task/{pid}/children").read_text()
    except OSError:
        return set()
    return {int(p) for p in text.split()}


@registry.collectors.register("cachegrind")
class CachegrindCollector(Collector):
    name = "cachegrind"

    def check(self, ctx):
        if not hasattr(ctx.cluster, "counts_dir"):
            raise Unavailable("needs the cachegrind cluster ([run] cluster = \"cachegrind\")")

    def start(self, ctx):
        self.pid = postmaster_pid(ctx.cluster.data_dir)
        self.resident = children(self.pid)   # the server's own processes
        self.before = set(ctx.cluster.counts_dir.glob("cachegrind.out.*"))

    def stop(self, ctx):
        # Every process the window forked has ended and written its count.
        deadline = time.monotonic() + 600
        while children(self.pid) - self.resident:
            if time.monotonic() > deadline:
                raise RuntimeError("server processes of the window did not end")
            time.sleep(0.05)
        self.files = sorted(set(ctx.cluster.counts_dir.glob("cachegrind.out.*")) - self.before)

    def results(self, ctx, outcome):
        if not self.files:
            raise RuntimeError("no server process ended in the window")
        total = sum(instructions(f) for f in self.files)
        return {"cachegrind_instructions": float(total),
                "cachegrind_processes": float(len(self.files)),
                "cachegrind_instructions_per_operation": per(total, outcome.transactions)}
