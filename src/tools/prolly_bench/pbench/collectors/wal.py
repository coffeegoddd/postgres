"""WAL volume during the window: bytes (from WAL positions), records and
full-page images (from pg_stat_wal)."""

from __future__ import annotations

from pbench.core import registry
from pbench.core.interfaces import Collector
from pbench.core.stats import per


@registry.collectors.register("wal")
class WalCollector(Collector):
    name = "wal"

    _SQL = ("SELECT pg_current_wal_insert_lsn(), wal_records, wal_fpi, wal_fpi_bytes "
            "FROM pg_stat_wal")

    def _snap(self, ctx):
        lsn, records, fpi, fpi_bytes = ctx.cluster.scalar(self._SQL).split("|")
        return lsn, int(records), int(fpi), int(fpi_bytes)

    def start(self, ctx):
        self.before = self._snap(ctx)

    def stop(self, ctx):
        self.after = self._snap(ctx)

    def results(self, ctx, outcome):
        wal_bytes = float(ctx.cluster.scalar(
            f"SELECT pg_wal_lsn_diff('{self.after[0]}', '{self.before[0]}')"))
        n = outcome.transactions
        return {
            "wal_bytes": wal_bytes,
            "wal_bytes_per_txn": per(wal_bytes, n),
            # pg_stat_wal is flushed by backends about once a second, so these
            # can lag slightly; WAL bytes come from WAL positions and don't.
            "wal_records": self.after[1] - self.before[1],
            "wal_fpi": self.after[2] - self.before[2],
            "wal_fpi_bytes": self.after[3] - self.before[3],
        }
