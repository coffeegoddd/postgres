"""Write amplification: the bytes the server wrote for each byte of row
data the workload wrote (specs/20-write-amplification.md).

The collector has its own interval, wider than the window. Before the
window it checkpoints, so the warm-up's dirty pages aren't charged to the
window, and has prolly fold first, so the load's trees aren't either. It doesn't wait for clients there: a workload may connect before
the window opens (S6's snapshot holder, the V suite's background S1).
After the window it waits for the measured clients to exit, and
then has prolly fold the working set into trees (prolly_checkpoint(); plain
SQL COMMIT writes no chunks). It checkpoints again, so the window's dirty
pages are written, and then takes its final readings. Deferred writes
caused by the window's work therefore fall inside the interval, whichever
engine defers them.

Logical bytes are the new row versions the SQL wrote:
(rows inserted + rows updated) x the table's average row size, for every
workload table, from the same statistics for every engine. Deleted rows
add no bytes and are reported as a count.

Autovacuum workers flush their statistics only when they exit
(autovacuum.c never reports them sooner), so the collector waits for none
to be running at each end of the interval.  Otherwise a worker's pages
(and, on prolly, the files its collection writes) would be charged to
the interval its exit fell in.
"""

from __future__ import annotations

import time
from typing import Optional

from pbench.collectors._snapshot import wait_for_clients
from pbench.core import registry
from pbench.core.interfaces import Collector, Context, Outcome
from pbench.core.stats import counter_delta, per

# Ratios below this many logical bytes aren't reported: the denominator is
# noise (a read-only workload's catalog writes, a V operation's commit).
MIN_LOGICAL_BYTES = 1 << 20

# Relation I/O by every backend type: the checkpointer and background
# writer write the pages the window dirtied.
_IO_SQL = ("SELECT coalesce(sum(write_bytes), 0) + coalesce(sum(extend_bytes), 0), "
           "coalesce(sum(writes), 0), coalesce(sum(extends), 0) "
           "FROM pg_stat_io WHERE object = 'relation'")
_PROLLY_SQL = "SELECT string_agg(name || '=' || value, E'\\n') FROM pg_stat_prolly"


def _is_prolly(ctx: Context) -> bool:
    return ctx.table_am.startswith("prolly")


def materialize_mode(ctx: Context) -> str:
    """What the flush does for prolly: "prolly_checkpoint" or "none"."""
    mode = ctx.params.get("write_amp_materialize", "auto")
    if mode == "auto":
        mode = "prolly_checkpoint" if ctx.key.suite == "S" else "none"
    return mode if _is_prolly(ctx) else "none"


def wait_for_autovacuum(ctx: Context, timeout_seconds: float = 600.0) -> None:
    """Wait until no autovacuum worker is running: each has exited, and so
    flushed what it counted."""
    sql = ("SELECT count(*) FROM pg_stat_activity "
           "WHERE backend_type = 'autovacuum worker'")
    deadline = time.monotonic() + timeout_seconds
    while int(ctx.cluster.scalar(sql)) > 0:
        if time.monotonic() > deadline:
            raise RuntimeError(f"autovacuum workers still running after {timeout_seconds:.0f} s")
        time.sleep(0.05)


def checkpoint(ctx: Context, timeout_seconds: float = 60.0) -> None:
    """CHECKPOINT, then wait until the checkpointer has reported it.

    The command returns when the checkpoint is done, before the
    checkpointer flushes its statistics (checkpointer.c): its page writes
    reach pg_stat_io a moment later, with the checkpoint's num_requested.
    """
    before = int(ctx.cluster.scalar("SELECT num_requested FROM pg_stat_checkpointer"))
    ctx.cluster.sql("CHECKPOINT")
    deadline = time.monotonic() + timeout_seconds
    while int(ctx.cluster.scalar("SELECT num_requested FROM pg_stat_checkpointer")) <= before:
        if time.monotonic() > deadline:
            raise RuntimeError("the checkpointer did not report the checkpoint")
        time.sleep(0.01)
    # pg_stat_io is flushed just after pg_stat_checkpointer: wait until
    # the checkpointer's row stops changing.
    row = "SELECT coalesce(sum(writes), 0) FROM pg_stat_io WHERE backend_type = 'checkpointer'"
    last = ctx.cluster.scalar(row)
    while True:
        time.sleep(0.05)
        now = ctx.cluster.scalar(row)
        if now == last or time.monotonic() > deadline:
            return
        last = now


def _tuples(ctx: Context) -> dict[str, tuple[float, float, float]]:
    """{table: (inserted, updated, deleted)} from the cumulative statistics."""
    out = {}
    for t in ctx.tables:
        row = ctx.cluster.scalar(
            f"SELECT coalesce(pg_stat_get_tuples_inserted(r), 0), "
            f"coalesce(pg_stat_get_tuples_updated(r), 0), "
            f"coalesce(pg_stat_get_tuples_deleted(r), 0) "
            f"FROM (SELECT to_regclass('{t}') AS r) s", dbname=ctx.dbname)
        ins, upd, dele = (float(v or 0) for v in row.split("|"))
        out[t] = (ins, upd, dele)
    return out


def _avg_row_bytes(ctx: Context, table: str) -> float:
    if ctx.cluster.scalar(f"SELECT to_regclass('{table}') IS NOT NULL", dbname=ctx.dbname) != "t":
        return 0.0
    return float(ctx.cluster.scalar(
        f"SELECT coalesce(avg(pg_column_size(t.*)), 0) FROM {table} t", dbname=ctx.dbname))


def _prolly(ctx: Context) -> dict[str, float]:
    rows = ctx.cluster.scalar(_PROLLY_SQL, dbname=ctx.dbname)
    return {k: float(v) for k, v in (r.split("=", 1) for r in rows.splitlines() if r)}


def _ratio(numerator: Optional[float], logical: float, outcome: Outcome) -> Optional[float]:
    if numerator is None or outcome.read_only or logical < MIN_LOGICAL_BYTES:
        return None
    return numerator / logical


@registry.collectors.register("write_amp")
class WriteAmpCollector(Collector):
    name = "write_amp"

    def before_window(self, ctx):
        wait_for_autovacuum(ctx)
        # Fold what came before (the load, the warm-up) first, so the fold
        # after the window writes the window's changes only: nothing folds
        # the working set between the load and the window.
        if materialize_mode(ctx) == "prolly_checkpoint":
            ctx.cluster.sql("SELECT prolly_checkpoint()", dbname=ctx.dbname)
        checkpoint(ctx)

    def _snap(self, ctx) -> dict:
        s = {}
        s["lsn"] = ctx.cluster.scalar("SELECT pg_current_wal_insert_lsn()")
        page_bytes, writes, extends = ctx.cluster.scalar(_IO_SQL).split("|")
        s["io"] = {"bytes": float(page_bytes), "writes": float(writes), "extends": float(extends)}
        s["tuples"] = _tuples(ctx)
        s["prolly"] = _prolly(ctx) if _is_prolly(ctx) else {}
        return s

    def start(self, ctx):
        self.before = self._snap(ctx)

    def after_window(self, ctx, outcome):
        wait_for_clients(ctx)
        wait_for_autovacuum(ctx)
        self.mode = materialize_mode(ctx)
        self.unmaterialized = None
        if self.mode == "prolly_checkpoint":
            self.unmaterialized = self._snap(ctx)
            ctx.cluster.sql("SELECT prolly_checkpoint()", dbname=ctx.dbname)
        checkpoint(ctx)
        wait_for_autovacuum(ctx)
        self.after = self._snap(ctx)

    def results(self, ctx, outcome):
        b, a = self.before, self.after
        n = outcome.transactions
        wal = float(ctx.cluster.scalar(f"SELECT pg_wal_lsn_diff('{a['lsn']}', '{b['lsn']}')"))
        io = counter_delta(b["io"], a["io"])

        rows_written = rows_deleted = logical = 0.0
        for t, (ins, upd, dele) in a["tuples"].items():
            ins0, upd0, del0 = b["tuples"].get(t, (0.0, 0.0, 0.0))
            if ins < ins0 or upd < upd0 or dele < del0:
                raise RuntimeError(f"tuple counters went backwards for {t}")
            written = (ins - ins0) + (upd - upd0)
            rows_written += written
            rows_deleted += dele - del0
            if written:
                logical += written * _avg_row_bytes(ctx, t)

        m: dict[str, Optional[float]] = {
            "wa.logical_bytes": logical,
            "wa.logical_rows_written": rows_written,
            "wa.logical_rows_deleted": rows_deleted,
            "wa.wal.total_bytes": wal,
            "wa.pages.write_bytes": io["bytes"],
            "wa.pages.writes": io["writes"],
            "wa.pages.extends": io["extends"],
        }
        server = wal + io["bytes"]
        if _is_prolly(ctx):
            chunks = counter_delta(b["prolly"], a["prolly"])
            m["wa.tree.chunk_payload_bytes"] = chunks.get("chunk_bytes_appended")
            if self.unmaterialized is not None:
                late = counter_delta(self.unmaterialized["prolly"], a["prolly"])
                m["wa.tree.materialize_chunk_payload_bytes"] = late.get("chunk_bytes_appended")
                m["wa.wal.materialize_bytes"] = float(ctx.cluster.scalar(
                    f"SELECT pg_wal_lsn_diff('{a['lsn']}', '{self.unmaterialized['lsn']}')"))
        m["wa.server_bytes"] = server
        for name in [k for k, v in m.items() if k.endswith("_bytes") and k != "wa.logical_bytes"]:
            m[f"{name}_per_operation"] = per(m[name], n) if m[name] is not None else None
        m["wa.logical_bytes_per_operation"] = per(logical, n)

        m["wa_wal"] = _ratio(wal, logical, outcome)
        m["wa_pages"] = _ratio(io["bytes"], logical, outcome)
        m["wa_server"] = _ratio(server, logical, outcome)
        if _is_prolly(ctx):
            m["wa_tree_payload"] = _ratio(m["wa.tree.chunk_payload_bytes"], logical, outcome)
        return m
