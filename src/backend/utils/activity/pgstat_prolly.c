/* -------------------------------------------------------------------------
 *
 * pgstat_prolly.c
 *	  Implementation of the prolly storage engine's statistics.
 *
 * This file contains the implementation of the prolly engine's work
 * counters: tree nodes read and written, chunks appended, bytes hashed,
 * rows folded, commits written and read, head moves, value memo lookups, pages copied, the
 * bytes the chunk store and the block map write to their files
 * themselves, the journal read to index it, and the chunk store's
 * collections, moves and conjoins with what collections kept and freed
 * and the page-store blocks they read, and the canonical columns decoded
 * and long values read.
 * It is kept separate from pgstat.c to enforce the line between the
 * statistics access / storage implementation and the details about
 * individual types of statistics.
 *
 * This statistics kind uses a database's OID as the key of its one entry,
 * with InvalidOid as the object ID.  Backends count in PendingProllyStats,
 * static memory, so counting is an increment with no lookup and works in
 * any context; the tree counts nodes through a ProllyNodeCounters of its
 * own, kept here too.  At the end of every transaction that counted
 * anything, the pending counts are marked for pgstat_report_stat() to
 * flush into the database's entry.
 *
 * Copyright (c) 2001-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_prolly.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/prolly_types.h"
#include "miscadmin.h"
#include "utils/pgstat_internal.h"

PgStat_Counter PendingProllyStats[PGSTAT_PROLLY_NUM_COUNTERS];

/* what trees whose environments count here count */
static ProllyNodeCounters PendingProllyTree;

/*
 * ... and the refs' and commits' trees, apart: their nodes count in
 * nodes_read and nodes_written too, and in refs_nodes_read and
 * refs_nodes_written, so that a caller can tell a table's trees' work from
 * theirs (whose leaves hold hashes and dates, so where their chunks split
 * varies from commit to commit).
 */
static ProllyNodeCounters PendingProllyRefsTree;

/* what this backend has flushed, since it began */
static PgStat_Counter SessionProllyStats[PGSTAT_PROLLY_NUM_COUNTERS];

static const char *const counter_names[PGSTAT_PROLLY_NUM_COUNTERS] = {
	[PGSTAT_PROLLY_NODES_READ] = "nodes_read",
	[PGSTAT_PROLLY_NODES_WRITTEN] = "nodes_written",
	[PGSTAT_PROLLY_VALUE_NODES_WRITTEN] = "value_nodes_written",
	[PGSTAT_PROLLY_CHUNKS_APPENDED] = "chunks_appended",
	[PGSTAT_PROLLY_CHUNK_BYTES_APPENDED] = "chunk_bytes_appended",
	[PGSTAT_PROLLY_BYTES_HASHED] = "bytes_hashed",
	[PGSTAT_PROLLY_FOLD_ROWS_READ] = "fold_rows_read",
	[PGSTAT_PROLLY_FOLD_ROWS_WRITTEN] = "fold_rows_written",
	[PGSTAT_PROLLY_FOLD_PAGES] = "fold_pages",
	[PGSTAT_PROLLY_FOLD_CANDIDATES] = "fold_candidates",
	[PGSTAT_PROLLY_FOLD_FULL] = "fold_full_tables",
	[PGSTAT_PROLLY_FOLD_FULL_NO_BASELINE] = "fold_full_no_baseline",
	[PGSTAT_PROLLY_FOLD_FULL_SHAPE_CHANGED] = "fold_full_shape_changed",
	[PGSTAT_PROLLY_FOLD_FULL_NO_LOG] = "fold_full_no_log",
	[PGSTAT_PROLLY_FOLD_FULL_LOG_CHANGED] = "fold_full_log_changed",
	[PGSTAT_PROLLY_FOLD_FULL_STALE_SNAPSHOT] = "fold_full_stale_snapshot",
	[PGSTAT_PROLLY_FOLD_FULL_UNSETTLED] = "fold_full_unsettled",
	[PGSTAT_PROLLY_FOLD_FULL_STORAGE_UNSEEN] = "fold_full_storage_unseen",
	[PGSTAT_PROLLY_FOLD_FULL_RECOVERY] = "fold_full_recovery",
	[PGSTAT_PROLLY_FOLD_FULL_LOG_ENTRY] = "fold_full_log_entry",
	[PGSTAT_PROLLY_FOLD_FULL_TOO_MANY] = "fold_full_too_many",
	[PGSTAT_PROLLY_FOLD_FULL_KEYING_CHANGED] = "fold_full_keying_changed",
	[PGSTAT_PROLLY_FOLD_FULL_UNSUPPORTED_KEY] = "fold_full_unsupported_key",
	[PGSTAT_PROLLY_FOLD_FULL_MEMORY_BUDGET] = "fold_full_memory_budget",
	[PGSTAT_PROLLY_FOLD_FULL_FORCED] = "fold_full_forced",
	[PGSTAT_PROLLY_FOLD_FULL_VERIFY] = "fold_full_verify",
	[PGSTAT_PROLLY_FOLD_FULL_SPILLED] = "fold_full_spilled",
	[PGSTAT_PROLLY_FOLD_MAPPED] = "fold_mapped_tables",
	[PGSTAT_PROLLY_FOLD_MERGE_BUILDS] = "fold_merge_builds",
	[PGSTAT_PROLLY_LOG_PAGES_READ] = "log_pages_read",
	[PGSTAT_PROLLY_FOLD_VERIFIED] = "fold_verified",
	[PGSTAT_PROLLY_FOLD_VERIFY_MISMATCHES] = "fold_verify_mismatches",
	[PGSTAT_PROLLY_COMMITS] = "commits",
	[PGSTAT_PROLLY_HEAD_MOVES] = "head_moves",
	[PGSTAT_PROLLY_FIRST_USE_REFUSALS] = "first_use_refusals",
	[PGSTAT_PROLLY_FIRST_USE_TABLES_SCANNED] = "first_use_tables_scanned",
	[PGSTAT_PROLLY_MEMO_HITS] = "memo_hits",
	[PGSTAT_PROLLY_MEMO_MISSES] = "memo_misses",
	[PGSTAT_PROLLY_PAGE_COPIES_FOLD] = "page_copies_fold",
	[PGSTAT_PROLLY_PAGE_COPIES_SHARED] = "page_copies_shared",
	[PGSTAT_PROLLY_JOURNAL_BYTES_WRITTEN] = "journal_bytes_written",
	[PGSTAT_PROLLY_TABLE_FILE_BYTES_WRITTEN] = "table_file_bytes_written",
	[PGSTAT_PROLLY_MANIFEST_BYTES_WRITTEN] = "manifest_bytes_written",
	[PGSTAT_PROLLY_GC_BYTES_COPIED] = "gc_bytes_copied",
	[PGSTAT_PROLLY_CONJOIN_BYTES_COPIED] = "conjoin_bytes_copied",
	[PGSTAT_PROLLY_BMAP_STORE_BYTES_WRITTEN] = "bmap_store_bytes_written",
	[PGSTAT_PROLLY_BMAP_EXTEND_BYTES_WRITTEN] = "bmap_extend_bytes_written",
	[PGSTAT_PROLLY_BMAP_BULK_BYTES_WRITTEN] = "bmap_bulk_bytes_written",
	[PGSTAT_PROLLY_JOURNAL_SCANS] = "journal_scans",
	[PGSTAT_PROLLY_JOURNAL_BYTES_SCANNED] = "journal_bytes_scanned",
	[PGSTAT_PROLLY_GC_COLLECTIONS] = "gc_collections",
	[PGSTAT_PROLLY_GC_MOVES] = "gc_moves",
	[PGSTAT_PROLLY_GC_CHUNKS_KEPT] = "gc_chunks_kept",
	[PGSTAT_PROLLY_GC_CHUNKS_FREED] = "gc_chunks_freed",
	[PGSTAT_PROLLY_GC_BYTES_FREED] = "gc_bytes_freed",
	[PGSTAT_PROLLY_CONJOINS] = "conjoins",
	[PGSTAT_PROLLY_GC_WINDOW_USEC] = "gc_window_usec",
	[PGSTAT_PROLLY_CHUNK_FILE_READS] = "chunk_file_reads",
	[PGSTAT_PROLLY_GC_PAGES_SCANNED] = "gc_pages_scanned",
	[PGSTAT_PROLLY_COMMITS_READ] = "commits_read",
	[PGSTAT_PROLLY_FOLDS_REUSED] = "folds_reused",
	[PGSTAT_PROLLY_DESCRIBE_CACHE_HITS] = "describe_cache_hits",
	[PGSTAT_PROLLY_APPLY_ROWS_BATCHED] = "apply_rows_batched",
	[PGSTAT_PROLLY_APPLY_LOADS] = "apply_loads",
	[PGSTAT_PROLLY_NODE_CACHE_HITS] = "node_cache_hits",
	[PGSTAT_PROLLY_NODE_CACHE_MISSES] = "node_cache_misses",
	[PGSTAT_PROLLY_FOLD_ROWS_LOCATED] = "fold_rows_located",
	[PGSTAT_PROLLY_BMAP_MAP_PAGES_COPIED] = "bmap_map_pages_copied",
	[PGSTAT_PROLLY_BMAP_REFCOUNT_UPDATES] = "bmap_refcount_updates",
	[PGSTAT_PROLLY_BMAP_BLOCKS_FREED] = "bmap_blocks_freed",
	[PGSTAT_PROLLY_APPLY_ROWS] = "apply_rows",
	[PGSTAT_PROLLY_APPLY_INDEX_TUPLES] = "apply_index_tuples",
	[PGSTAT_PROLLY_CATALOG_ROWS_COPIED] = "catalog_rows_copied",
	[PGSTAT_PROLLY_FOLD_CHANGES_SPILLED] = "fold_changes_spilled",
	[PGSTAT_PROLLY_FOLD_CATALOG_SCANS] = "fold_catalog_scans",
	[PGSTAT_PROLLY_FOLD_OBJECTS] = "fold_objects",
	[PGSTAT_PROLLY_FOLD_TABLES] = "fold_tables",
	[PGSTAT_PROLLY_FOLD_INCREMENTAL] = "fold_incremental",
	[PGSTAT_PROLLY_BMAP_PAGES_PRESERVED] = "bmap_pages_preserved",
	[PGSTAT_PROLLY_BMAP_PRESERVE_WAITS] = "bmap_preserve_waits",
	[PGSTAT_PROLLY_MOVE_PLANS] = "move_plans",
	[PGSTAT_PROLLY_FIRST_USE_SOURCES_BUSY] = "first_use_sources_busy",
	[PGSTAT_PROLLY_BMAP_RELEASE_QUEUED] = "bmap_release_queued",
	[PGSTAT_PROLLY_BMAP_RELEASE_DRAINED] = "bmap_release_drained",
	[PGSTAT_PROLLY_BMAP_RELEASE_BLOCKS_FREED] = "bmap_release_blocks_freed",
	[PGSTAT_PROLLY_BMAP_RELEASE_REFCOUNT_UPDATES] = "bmap_release_refcount_updates",
	[PGSTAT_PROLLY_MOVE_TABLES_SHARED] = "move_tables_shared",
	[PGSTAT_PROLLY_KEPT_CHECKOUTS_REUSED] = "kept_checkouts_reused",
	[PGSTAT_PROLLY_KEPT_CHECKOUTS_EVICTED] = "kept_checkouts_evicted",
	[PGSTAT_PROLLY_MOVE_TABLES_WRITTEN] = "move_tables_written",
	[PGSTAT_PROLLY_PAGES_KEPT] = "pages_kept",
	[PGSTAT_PROLLY_RELATIONS_RESTORED] = "relations_restored",
	[PGSTAT_PROLLY_FIRST_USE_TABLES_KEPT] = "first_use_tables_kept",
	[PGSTAT_PROLLY_BMAP_BLOCKS_MOVED] = "bmap_blocks_moved",
	[PGSTAT_PROLLY_COLUMNS_DECODED] = "columns_decoded",
	[PGSTAT_PROLLY_VALUES_READ] = "values_read",
	[PGSTAT_PROLLY_VALUE_BYTES_READ] = "value_bytes_read",
	[PGSTAT_PROLLY_BMAP_RELEASE_GROWTH_DRAINS] = "bmap_release_growth_drains",
	[PGSTAT_PROLLY_FOLD_WORKER_FOLDS] = "fold_worker_folds",
	[PGSTAT_PROLLY_FOLD_WORKER_DEFERRED] = "fold_worker_deferred",
	[PGSTAT_PROLLY_FOLD_WORKER_RETRIES] = "fold_worker_retries",
	[PGSTAT_PROLLY_FOLD_WORKER_COST_DELAYS] = "fold_worker_cost_delays",
	[PGSTAT_PROLLY_REWRITE_CHECKPOINTS_SKIPPED] = "rewrite_checkpoints_skipped",
	[PGSTAT_PROLLY_DEFINITIONS_REUSED] = "definitions_reused",
	[PGSTAT_PROLLY_STATUS_REUSED] = "status_reused",
	[PGSTAT_PROLLY_AUTO_COLLECTIONS_SKIPPED] = "auto_collections_skipped",
	[PGSTAT_PROLLY_REFS_NODES_READ] = "refs_nodes_read",
	[PGSTAT_PROLLY_REFS_NODES_WRITTEN] = "refs_nodes_written",
	[PGSTAT_PROLLY_BMAP_REFCOUNT_READS] = "bmap_refcount_reads",
	[PGSTAT_PROLLY_STATUS_FROM_FOLD] = "status_from_fold",
	[PGSTAT_PROLLY_COMMIT_CACHE_HITS] = "commit_cache_hits",
	[PGSTAT_PROLLY_COMMIT_CACHE_MISSES] = "commit_cache_misses",
	[PGSTAT_PROLLY_KEPT_STATUS_CHECKS] = "kept_status_checks",
	[PGSTAT_PROLLY_RESTORES_LOCK_BUSY] = "restores_lock_busy",
	[PGSTAT_PROLLY_RECORDS_FORGOTTEN] = "records_forgotten",
	[PGSTAT_PROLLY_BRANCHES_LISTED] = "branches_listed",
	[PGSTAT_PROLLY_BMAP_ROOTS_CAPTURED] = "bmap_roots_captured",
	[PGSTAT_PROLLY_BMAP_ROOTS_RELEASED] = "bmap_roots_released",
	[PGSTAT_PROLLY_BMAP_ROOTS_SHARED] = "bmap_roots_shared",
	[PGSTAT_PROLLY_BMAP_MAP_PAGES_STAMPED] = "bmap_map_pages_stamped",
	[PGSTAT_PROLLY_BMAP_CHANGED_NODES_READ] = "bmap_changed_nodes_read",
	[PGSTAT_PROLLY_ROOTS_CAPTURE_BUSY] = "roots_capture_busy",
	[PGSTAT_PROLLY_ROOTS_FROZEN_LATER] = "roots_frozen_later",
	[PGSTAT_PROLLY_BUFFER_GENERATIONS] = "buffer_generations",
	[PGSTAT_PROLLY_BUFFER_REL_GENERATIONS] = "buffer_rel_generations",
	[PGSTAT_PROLLY_BMAP_ROOTS_PAGES_READ] = "bmap_roots_pages_read",
	[PGSTAT_PROLLY_HORIZON_ENTRIES_READ] = "horizon_entries_read",
};

/*
 * The node counters a tree environment points at (its counters) to count
 * into these statistics.
 */
ProllyNodeCounters *
pgstat_prolly_tree_counters(void)
{
	return &PendingProllyTree;
}

/* The node counters the refs' and commits' trees count into. */
ProllyNodeCounters *
pgstat_prolly_refs_tree_counters(void)
{
	return &PendingProllyRefsTree;
}

const char *
pgstat_prolly_counter_name(PgStat_ProllyCounter c)
{
	Assert(c >= 0 && c < PGSTAT_PROLLY_NUM_COUNTERS);
	return counter_names[c];
}

/* Move the tree's counts into PendingProllyStats. */
static void
absorb_tree_counts(void)
{
	PendingProllyStats[PGSTAT_PROLLY_NODES_READ] += PendingProllyTree.nodes_read;
	PendingProllyStats[PGSTAT_PROLLY_NODES_WRITTEN] += PendingProllyTree.nodes_written;
	PendingProllyStats[PGSTAT_PROLLY_VALUE_NODES_WRITTEN] +=
		PendingProllyTree.value_nodes_written;
	memset(&PendingProllyTree, 0, sizeof(PendingProllyTree));
	PendingProllyStats[PGSTAT_PROLLY_NODES_READ] += PendingProllyRefsTree.nodes_read;
	PendingProllyStats[PGSTAT_PROLLY_NODES_WRITTEN] += PendingProllyRefsTree.nodes_written;
	PendingProllyStats[PGSTAT_PROLLY_VALUE_NODES_WRITTEN] +=
		PendingProllyRefsTree.value_nodes_written;
	PendingProllyStats[PGSTAT_PROLLY_REFS_NODES_READ] += PendingProllyRefsTree.nodes_read;
	PendingProllyStats[PGSTAT_PROLLY_REFS_NODES_WRITTEN] +=
		PendingProllyRefsTree.nodes_written;
	memset(&PendingProllyRefsTree, 0, sizeof(PendingProllyRefsTree));
}

static bool
have_pending(void)
{
	if (PendingProllyTree.nodes_read != 0 || PendingProllyTree.nodes_written != 0 ||
		PendingProllyTree.value_nodes_written != 0)
		return true;
	if (PendingProllyRefsTree.nodes_read != 0 ||
		PendingProllyRefsTree.nodes_written != 0 ||
		PendingProllyRefsTree.value_nodes_written != 0)
		return true;
	for (int i = 0; i < PGSTAT_PROLLY_NUM_COUNTERS; i++)
		if (PendingProllyStats[i] != 0)
			return true;
	return false;
}

/*
 * At the end of a transaction: if it counted anything, have the next
 * pgstat_report_stat() flush it (counting doesn't tell it so itself, to
 * stay an increment).
 */
void
AtEOXact_PgStat_Prolly(void)
{
	if (have_pending())
		pgstat_report_fixed = true;
}

/*
 * Flush out locally pending counts into this database's entry.
 *
 * If they could not be flushed due to lock contention, return true.
 */
bool
pgstat_prolly_flush_cb(bool nowait)
{
	PgStat_EntryRef *entry_ref;
	PgStatShared_Prolly *shstats;

	if (!have_pending())
		return false;

	/* (a process connected to no database has no entry to count in) */
	if (!OidIsValid(MyDatabaseId))
	{
		memset(PendingProllyStats, 0, sizeof(PendingProllyStats));
		memset(&PendingProllyTree, 0, sizeof(PendingProllyTree));
		memset(&PendingProllyRefsTree, 0, sizeof(PendingProllyRefsTree));
		return false;
	}

	entry_ref = pgstat_get_entry_ref_locked(PGSTAT_KIND_PROLLY, MyDatabaseId,
											InvalidOid, nowait);
	if (!entry_ref)
		return true;

	absorb_tree_counts();
	shstats = (PgStatShared_Prolly *) entry_ref->shared_stats;
	for (int i = 0; i < PGSTAT_PROLLY_NUM_COUNTERS; i++)
	{
		shstats->stats.counts[i] += PendingProllyStats[i];
		SessionProllyStats[i] += PendingProllyStats[i];
	}
	pgstat_unlock_entry(entry_ref);

	memset(PendingProllyStats, 0, sizeof(PendingProllyStats));
	return false;
}

/*
 * What this backend has counted since it began, flushed or not, into
 * counts[] (one for each counter): its own work alone, which other
 * backends' doesn't move, for a caller measuring its own statements.
 */
void
pgstat_prolly_session_counts(PgStat_Counter *counts)
{
	for (int i = 0; i < PGSTAT_PROLLY_NUM_COUNTERS; i++)
		counts[i] = SessionProllyStats[i] + PendingProllyStats[i];
	counts[PGSTAT_PROLLY_NODES_READ] += PendingProllyTree.nodes_read;
	counts[PGSTAT_PROLLY_NODES_WRITTEN] += PendingProllyTree.nodes_written;
	counts[PGSTAT_PROLLY_VALUE_NODES_WRITTEN] +=
		PendingProllyTree.value_nodes_written;
	counts[PGSTAT_PROLLY_NODES_READ] += PendingProllyRefsTree.nodes_read;
	counts[PGSTAT_PROLLY_NODES_WRITTEN] += PendingProllyRefsTree.nodes_written;
	counts[PGSTAT_PROLLY_VALUE_NODES_WRITTEN] +=
		PendingProllyRefsTree.value_nodes_written;
	counts[PGSTAT_PROLLY_REFS_NODES_READ] += PendingProllyRefsTree.nodes_read;
	counts[PGSTAT_PROLLY_REFS_NODES_WRITTEN] += PendingProllyRefsTree.nodes_written;
}

void
pgstat_prolly_reset_timestamp_cb(PgStatShared_Common *header, TimestampTz ts)
{
	((PgStatShared_Prolly *) header)->stats.stat_reset_timestamp = ts;
}

/*
 * Support function for the SQL-callable pgstat* functions.  Returns the
 * counters of database dboid, or NULL if it has counted nothing.
 */
PgStat_ProllyStats *
pgstat_fetch_stat_prolly(Oid dboid)
{
	return (PgStat_ProllyStats *) pgstat_fetch_entry(PGSTAT_KIND_PROLLY,
													  dboid, InvalidOid, NULL);
}
