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

Prolly's chunk store writes its journal, table files and manifests
itself, outside shared buffers, so pg_stat_io doesn't see them;
pg_stat_prolly counts them (journal_bytes_written, ...), and the copies
collection and conjoining make of chunks already stored.  The block map
writes some of its page store itself too: a copy-on-write remap's blocks
are pages written (bmap_store_bytes_written, added to pg_stat_io's);
its extends for shared buffers pg_stat_io counts already, and its bulk
writes' (index builds, copies) heap's relations count nowhere either, so
those two are reported, not added.

WAL is split by resource manager (pg_walinspect's pg_get_wal_stats over
the interval): row records
(Heap, Heap2), index records, prolly's block map (BlockMap) and chunks
(ProllyChunks, which hold every chunk byte the chunk store's files then
hold again), full-page images from any of them, and the rest.  The WAL is
read before the final checkpoint, which would recycle it; a replication
slot keeps it from the window's own checkpoints where wal_level allows
one.  At wal_level = minimal (the counter runs, which have no timed
checkpoints) there is no slot, and WAL a checkpoint in the window
recycled fails the read rather than going uncounted.  The WAL's interval
therefore ends before the final checkpoint's record; the pages' after it.

Logical bytes are the new row versions the SQL wrote:
(rows inserted + rows updated) x the table's average row size, for every
workload table, from the same statistics for every engine. Deleted rows
add no bytes and are reported as a count.

A workload's other instances (ctx.peers: V4's clone, which the clone,
fetch and push write to) are checkpointed, waited for and read like the
origin, and their WAL, pages and chunk-store bytes added in; logical
bytes are the origin's.

Autovacuum workers flush their statistics only when they exit
(autovacuum.c never reports them sooner), so the collector waits for none
to be running at each end of the interval.  Otherwise a worker's pages
(and, on prolly, the files its collection writes) would be charged to
the interval its exit fell in.
"""

from __future__ import annotations

import copy
import time
from typing import Optional

from pbench.collectors._snapshot import wait_for_clients
from pbench.core import registry
from pbench.core.interfaces import Collector, Context, Outcome, Unavailable
from pbench.core.stats import counter_delta, per
from pbench.core.validation import atomic_json

# Ratios below this many logical bytes aren't reported: the denominator is
# noise (a read-only workload's catalog writes, a V operation's commit).
MIN_LOGICAL_BYTES = 1 << 20

# Relation I/O by every backend type: the checkpointer and background
# writer write the pages the window dirtied.
_IO_SQL = ("SELECT coalesce(sum(write_bytes), 0) + coalesce(sum(extend_bytes), 0), "
           "coalesce(sum(writes), 0), coalesce(sum(extends), 0) "
           "FROM pg_stat_io WHERE object = 'relation'")
SLOT = "pbench_write_amp"

# WAL record bytes (without full-page images, which are counted apart)
# by what wrote them; resource managers not named here are "other".
RMGR_GROUPS = {"Heap": "row", "Heap2": "row",
               "Btree": "index", "Hash": "index", "Gin": "index", "Gist": "index",
               "SPGist": "index", "BRIN": "index",
               "BlockMap": "blockmap", "ProllyChunks": "chunk"}
WAL_GROUPS = ("row", "index", "blockmap", "chunk", "other")

# pg_stat_prolly's counters of the chunk store's file bytes (wa.tree.file_bytes).
TREE_FILES = {"journal_bytes_written": "journal", "table_file_bytes_written": "table_file",
              "manifest_bytes_written": "manifest"}

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


def _on(ctx: Context, cluster) -> Context:
    """ctx, reading another instance (a peer) instead of the origin."""
    c = copy.copy(ctx)
    c.cluster = cluster
    return c


def _sum(deltas: list[dict[str, float]]) -> dict[str, float]:
    out: dict[str, float] = {}
    for d in deltas:
        for k, v in d.items():
            out[k] = out.get(k, 0.0) + v
    return out


def wal_split(rows: list[tuple[str, float, float, float]]) -> dict[str, float]:
    """wa.wal.* from pg_get_wal_stats rows (rmgr, records, record bytes, FPI bytes)."""
    out = {f"wa.wal.{g}_bytes": 0.0 for g in WAL_GROUPS}
    out["wa.wal.fpi_bytes"] = out["wa.wal.records"] = 0.0
    for rmgr, count, record_bytes, fpi_bytes in rows:
        out[f"wa.wal.{RMGR_GROUPS.get(rmgr, 'other')}_bytes"] += record_bytes
        out["wa.wal.fpi_bytes"] += fpi_bytes
        out["wa.wal.records"] += count
    return out


def _wal_stats(ctx: Context, start: str, end: str) -> list[tuple[str, float, float, float]]:
    # An interval that wrote no WAL (a read-only cell at wal_level =
    # minimal) has no record to find: pg_get_wal_stats would fail on it.
    if float(ctx.cluster.scalar(f"SELECT pg_wal_lsn_diff('{end}', '{start}')")) <= 0:
        return []
    # pg_get_wal_stats quietly stops at the flushed position: the records
    # since the last checkpoint may not be flushed yet.  A switch flushes
    # them; its own record is after end.
    if float(ctx.cluster.scalar(
            f"SELECT pg_wal_lsn_diff(pg_current_wal_flush_lsn(), '{end}')")) < 0:
        ctx.cluster.sql("SELECT pg_switch_wal()")
    out = ctx.cluster.scalar(
        "SELECT string_agg(concat_ws('|', \"resource_manager/record_type\", count, "
        f"record_size, fpi_size), E'\\n') "
        f"FROM pg_get_wal_stats('{start}', '{end}', false) WHERE count > 0")
    rows = []
    for line in out.splitlines():
        if line:
            rmgr, *nums = line.split("|")
            rows.append((rmgr, *(float(v) for v in nums)))
    return rows


def _ratio(numerator: Optional[float], logical: float, outcome: Outcome) -> Optional[float]:
    if numerator is None or outcome.read_only or logical < MIN_LOGICAL_BYTES:
        return None
    return numerator / logical


@registry.collectors.register("write_amp")
class WriteAmpCollector(Collector):
    name = "write_amp"

    @staticmethod
    def _walinspect(ctx):
        r = ctx.cluster.sql("CREATE EXTENSION IF NOT EXISTS pg_walinspect", check=False)
        if not r.ok:
            raise Unavailable(f"pg_walinspect: {r.stderr.strip()}")

    def check(self, ctx):
        self._walinspect(ctx)

    def _drop_slot(self, ctx):
        # (if it's there: whatever failed may have come before or after)
        ctx.cluster.sql(f"SELECT pg_drop_replication_slot(slot_name) FROM pg_replication_slots "
                        f"WHERE slot_name = '{SLOT}'")

    def before_window(self, ctx):
        self.peers = [_on(ctx, p) for p in getattr(ctx, "peers", [])]
        self.slot = ctx.cluster.scalar("SHOW wal_level") != "minimal"
        # (a peer is made in measure(), after check(): its WAL is read too)
        for c in self.peers:
            self._walinspect(c)
        try:
            for c in [ctx] + self.peers:
                wait_for_autovacuum(c)
                # Fold what came before (the load, the warm-up) first, so the
                # fold after the window writes the window's changes only:
                # nothing folds the working set between the load and the window.
                if materialize_mode(ctx) == "prolly_checkpoint":
                    c.cluster.sql("SELECT prolly_checkpoint()", dbname=ctx.dbname)
                if self.slot:
                    # Before the checkpoint: the slot holds WAL from the redo point on.
                    c.cluster.sql(f"SELECT pg_create_physical_replication_slot('{SLOT}', true)")
                checkpoint(c)
        except Exception:
            if self.slot:
                for c in [ctx] + self.peers:
                    self._drop_slot(c)
            raise

    def _snap(self, ctx, tuples: bool = True) -> dict:
        s = {}
        s["lsn"] = ctx.cluster.scalar("SELECT pg_current_wal_insert_lsn()")
        page_bytes, writes, extends = ctx.cluster.scalar(_IO_SQL).split("|")
        s["io"] = {"bytes": float(page_bytes), "writes": float(writes), "extends": float(extends)}
        s["tuples"] = _tuples(ctx) if tuples else {}
        s["prolly"] = _prolly(ctx) if _is_prolly(ctx) else {}
        return s

    def start(self, ctx):
        self.before = self._snap(ctx)
        self.peers_before = [self._snap(p, tuples=False) for p in self.peers]

    def after_window(self, ctx, outcome):
        instances = [ctx] + self.peers
        try:
            for c in instances:
                wait_for_clients(c)
                wait_for_autovacuum(c)
            self.mode = materialize_mode(ctx)
            self.unmaterialized = None
            if self.mode == "prolly_checkpoint":
                self.unmaterialized = self._snap(ctx)
                for c in instances:
                    c.cluster.sql("SELECT prolly_checkpoint()", dbname=ctx.dbname)
            # each instance's WAL, read before its final checkpoint
            self.wal_ends = [c.cluster.scalar("SELECT pg_current_wal_insert_lsn()")
                             for c in instances]
            self.wal_end = self.wal_ends[0]
            self.rmgrs = [row for c, x, end in zip(instances, [self.before] + self.peers_before,
                                                   self.wal_ends)
                          for row in _wal_stats(c, x["lsn"], end)]
        finally:
            if self.slot:
                for c in instances:
                    self._drop_slot(c)
        for c in instances:
            checkpoint(c)
            wait_for_autovacuum(c)
        self.after = self._snap(ctx)
        self.peers_after = [self._snap(p, tuples=False) for p in self.peers]

    def _pairs(self, ctx):
        """(instance's ctx, before, after) for the origin and each peer."""
        return [(ctx, self.before, self.after)] + list(
            zip(self.peers, self.peers_before, self.peers_after))

    def results(self, ctx, outcome):
        b, a = self.before, self.after
        n = outcome.transactions
        wal = sum(float(c.cluster.scalar(f"SELECT pg_wal_lsn_diff('{end}', '{x['lsn']}')"))
                  for (c, x, _), end in zip(self._pairs(ctx), self.wal_ends))
        io = _sum([counter_delta(x["io"], y["io"]) for _, x, y in self._pairs(ctx)])

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

        atomic_json(ctx.raw_dir / "write-amp-wal.json", {
            "start_lsn": b["lsn"], "end_lsn": self.wal_end, "slot": self.slot,
            "rmgrs": [dict(zip(("resource_manager", "records", "record_bytes", "fpi_bytes"), r))
                      for r in self.rmgrs]})
        split = wal_split(self.rmgrs)

        m: dict[str, Optional[float]] = {
            "wa.logical_bytes": logical,
            "wa.logical_rows_written": rows_written,
            "wa.logical_rows_deleted": rows_deleted,
            # (the origin, and the workload's other instances)
            "wa.instances": float(1 + len(self.peers)),
            "wa.wal.total_bytes": wal,
            **split,
            # Page headers and alignment: in the LSN difference, in no record.
            "wa.wal.unattributed_bytes": wal - sum(
                v for k, v in split.items() if k != "wa.wal.records"),
            # about 0.5%; much more is a segment switch's padding
            # (archive_timeout), not the work
            "wa.wal.unattributed_fraction": (wal - sum(
                v for k, v in split.items() if k != "wa.wal.records")) / wal if wal else None,
            "wa.pages.write_bytes": io["bytes"],
            "wa.pages.writes": io["writes"],
            "wa.pages.extends": io["extends"],
        }
        pages = io["bytes"]
        tree_files = None
        if _is_prolly(ctx):
            chunks = _sum([counter_delta(x["prolly"], y["prolly"]) for _, x, y in self._pairs(ctx)])
            m["wa.tree.chunk_payload_bytes"] = chunks.get("chunk_bytes_appended")
            # (absent from a build before these counters: then no total)
            if all(c in chunks for c in TREE_FILES):
                for c, part in TREE_FILES.items():
                    m[f"wa.tree.{part}_bytes"] = chunks[c]
                tree_files = sum(chunks[c] for c in TREE_FILES)
                # Breakdowns of the table-file bytes, not more of them.
                m["wa.tree.gc_copied_bytes"] = chunks.get("gc_bytes_copied")
                m["wa.tree.conjoin_copied_bytes"] = chunks.get("conjoin_bytes_copied")
                # The block map's own page-store writes.  A copy-on-write
                # remap's no other counter has: they are pages written.  Its
                # extends for shared buffers pg_stat_io counts already, and
                # its bulk writes' heap's md relations count nowhere either:
                # both are reported, not added.
                store = chunks.get("bmap_store_bytes_written")
                m["wa.pages.bmap_store_bytes"] = store
                m["wa.pages.bmap_extend_bytes"] = chunks.get("bmap_extend_bytes_written")
                m["wa.pages.bmap_bulk_bytes"] = chunks.get("bmap_bulk_bytes_written")
                m["wa.pages.io_bytes"] = io["bytes"]
                pages += store or 0.0
            m["wa.tree.file_bytes"] = tree_files
        m["wa.pages.write_bytes"] = pages
        server: Optional[float] = wal + pages
        if _is_prolly(ctx):
            server = server + tree_files if tree_files is not None else None
            if self.unmaterialized is not None:
                late = counter_delta(self.unmaterialized["prolly"], a["prolly"])
                m["wa.tree.materialize_chunk_payload_bytes"] = late.get("chunk_bytes_appended")
                if tree_files is not None:
                    m["wa.tree.materialize_file_bytes"] = sum(late[c] for c in TREE_FILES)
                m["wa.wal.materialize_bytes"] = float(ctx.cluster.scalar(
                    f"SELECT pg_wal_lsn_diff('{self.wal_end}', '{self.unmaterialized['lsn']}')"))
        m["wa.server_bytes"] = server
        for name in [k for k, v in m.items() if k.endswith("_bytes") and k != "wa.logical_bytes"]:
            m[f"{name}_per_operation"] = per(m[name], n) if m[name] is not None else None
        m["wa.logical_bytes_per_operation"] = per(logical, n)

        m["wa_wal"] = _ratio(wal, logical, outcome)
        m["wa_pages"] = _ratio(pages, logical, outcome)
        m["wa_server"] = _ratio(server, logical, outcome)
        if _is_prolly(ctx):
            m["wa_tree_payload"] = _ratio(m["wa.tree.chunk_payload_bytes"], logical, outcome)
            m["wa_tree"] = _ratio(tree_files, logical, outcome)
        return m
