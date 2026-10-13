"""Crash recovery after the window: stop the server with -m immediate and
time the restart until it accepts connections, recording how much WAL had
to be replayed (since the last checkpoint's redo point)."""

from __future__ import annotations

import time

from pbench.core import registry
from pbench.core.interfaces import Collector


@registry.collectors.register("recovery")
class RecoveryCollector(Collector):
    name = "recovery"
    post_order = 100  # last: it crashes the server

    def post(self, ctx, outcome):
        replay = float(ctx.cluster.scalar(
            "SELECT pg_wal_lsn_diff(pg_current_wal_insert_lsn(), redo_lsn) "
            "FROM pg_control_checkpoint()"))
        ctx.cluster.stop("immediate")
        t0 = time.monotonic()
        ctx.cluster.start()
        ctx.cluster.wait_ready()
        seconds = time.monotonic() - t0
        return {"recovery_seconds": seconds, "recovery_wal_bytes": replay,
                "recovery_seconds_per_gb": seconds / (replay / 2**30) if replay else None}
