/* -------------------------------------------------------------------------
 *
 * pgstat_block_map.c
 *	  Implementation of block map statistics.
 *
 * This file contains the implementation of block map statistics: what a
 * block-mapped relation file's changes did to the pages other maps share
 * (pages and tree nodes copied, pages claimed in place).  The storage
 * manager counts them as it prepares pages for a change; they are kept by
 * the relation file, so that the storage manager, which knows the file and
 * not the relation, can count them.
 *
 * Copyright (c) 2001-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_block_map.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/pgstat_internal.h"

/* a relation file's key within its database: tablespace and file number */
static inline uint64
block_map_objid(Oid spcoid, Oid relnumber)
{
	return ((uint64) spcoid << 32) | relnumber;
}

/*
 * Count an event of a relation file's block map.  Not in a critical
 * section: the pending entry may be made.
 */
void
pgstat_count_block_map(Oid spcoid, Oid dboid, Oid relnumber,
					   PgStat_BlockMapEvent event)
{
	PgStat_EntryRef *entry_ref;
	PgStat_BlockMapCounts *pending;

	if (!pgstat_track_counts)
		return;

	entry_ref = pgstat_prep_pending_entry(PGSTAT_KIND_BLOCK_MAP, dboid,
										  block_map_objid(spcoid, relnumber),
										  NULL);
	pending = (PgStat_BlockMapCounts *) entry_ref->pending;
	switch (event)
	{
		case PGSTAT_BLOCK_MAP_PAGE_COPIED:
			pending->pages_copied++;
			break;
		case PGSTAT_BLOCK_MAP_PAGE_CLAIMED:
			pending->pages_claimed++;
			break;
		case PGSTAT_BLOCK_MAP_NODE_COPIED:
			pending->nodes_copied++;
			break;
	}
}

/*
 * Drop a relation file's statistics, as its storage is unlinked.
 */
void
pgstat_drop_block_map(Oid spcoid, Oid dboid, Oid relnumber)
{
	if (!pgstat_drop_entry(PGSTAT_KIND_BLOCK_MAP, dboid,
						   block_map_objid(spcoid, relnumber), true))
		pgstat_request_entry_refs_gc();
}

/*
 * Support function for the SQL-callable pgstat* functions.  Returns the
 * collected statistics of a relation file, or NULL.
 */
PgStat_StatBlockMapEntry *
pgstat_fetch_stat_block_map(Oid spcoid, Oid dboid, Oid relnumber)
{
	return (PgStat_StatBlockMapEntry *)
		pgstat_fetch_entry(PGSTAT_KIND_BLOCK_MAP, dboid,
						   block_map_objid(spcoid, relnumber), NULL);
}

/*
 * Flush out pending stats for the entry
 *
 * If nowait is true and the lock could not be immediately acquired, returns
 * false without flushing the entry.  Otherwise returns true.
 */
bool
pgstat_block_map_flush_cb(PgStat_EntryRef *entry_ref, bool nowait)
{
	PgStat_BlockMapCounts *localent;
	PgStatShared_BlockMap *shent;

	localent = (PgStat_BlockMapCounts *) entry_ref->pending;
	shent = (PgStatShared_BlockMap *) entry_ref->shared_stats;

	if (!pgstat_lock_entry(entry_ref, nowait))
		return false;

#define BLOCK_MAP_ACC(fld) shent->stats.fld += localent->fld
	BLOCK_MAP_ACC(pages_copied);
	BLOCK_MAP_ACC(pages_claimed);
	BLOCK_MAP_ACC(nodes_copied);
#undef BLOCK_MAP_ACC

	pgstat_unlock_entry(entry_ref);
	return true;
}

void
pgstat_block_map_reset_timestamp_cb(PgStatShared_Common *header, TimestampTz ts)
{
	((PgStatShared_BlockMap *) header)->stats.stat_reset_timestamp = ts;
}
