"""Crash recovery after the window: stop the server with -m immediate and
time the restart until it accepts connections, recording how much WAL had
to be replayed (since the last checkpoint's redo point)."""

from __future__ import annotations

import time

from pbench.core import registry
from pbench.core.interfaces import Collector
from pbench.core.correctness import table_digests
from pbench.core.validation import atomic_json


@registry.collectors.register("recovery")
class RecoveryCollector(Collector):
    name = "recovery"
    post_order = -100  # before VACUUM or other maintenance changes the fixture

    def post(self, ctx, outcome):
        before = table_digests(ctx)
        replay = float(ctx.cluster.scalar(
            "SELECT pg_wal_lsn_diff(pg_current_wal_insert_lsn(), redo_lsn) "
            "FROM pg_control_checkpoint()"))
        ctx.cluster.stop("immediate")
        t0 = time.monotonic()
        ctx.cluster.start()
        ctx.cluster.wait_ready()
        seconds = time.monotonic() - t0
        after = table_digests(ctx)
        atomic_json(ctx.raw_dir / "recovery-correctness.json", {
            "before": before, "after": after, "equal": before == after,
            "scope": "process crash, warm OS cache after integrity scan, before maintenance"})
        if before != after:
            raise RuntimeError("recovered table contents differ from committed pre-crash data")
        return {"process_restart_seconds": seconds, "recovery_wal_bytes": replay,
                "recovery_seconds_per_gb": seconds / (replay / 2**30) if replay else None}
