/* -------------------------------------------------------------------------
 *
 * pgstat_prolly.c
 *	  Implementation of the prolly storage engine's statistics.
 *
 * This file contains the implementation of the prolly engine's work
 * counters: tree nodes read and written, chunks appended, bytes hashed,
 * rows folded, commits, head moves, value memo lookups, pages copied.  It
 * is kept separate from pgstat.c to enforce the line between the statistics
 * access / storage implementation and the details about individual types of
 * statistics.
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
	[PGSTAT_PROLLY_COMMITS] = "commits",
	[PGSTAT_PROLLY_HEAD_MOVES] = "head_moves",
	[PGSTAT_PROLLY_MEMO_HITS] = "memo_hits",
	[PGSTAT_PROLLY_MEMO_MISSES] = "memo_misses",
	[PGSTAT_PROLLY_PAGE_COPIES_FOLD] = "page_copies_fold",
	[PGSTAT_PROLLY_PAGE_COPIES_SHARED] = "page_copies_shared",
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
}

static bool
have_pending(void)
{
	if (PendingProllyTree.nodes_read != 0 || PendingProllyTree.nodes_written != 0 ||
		PendingProllyTree.value_nodes_written != 0)
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
		return false;
	}

	entry_ref = pgstat_get_entry_ref_locked(PGSTAT_KIND_PROLLY, MyDatabaseId,
											InvalidOid, nowait);
	if (!entry_ref)
		return true;

	absorb_tree_counts();
	shstats = (PgStatShared_Prolly *) entry_ref->shared_stats;
	for (int i = 0; i < PGSTAT_PROLLY_NUM_COUNTERS; i++)
		shstats->stats.counts[i] += PendingProllyStats[i];
	pgstat_unlock_entry(entry_ref);

	memset(PendingProllyStats, 0, sizeof(PendingProllyStats));
	return false;
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
