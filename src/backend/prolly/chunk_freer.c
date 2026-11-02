/*-------------------------------------------------------------------------
 *
 * chunk_freer.c
 *	  Freeing the history store's chunks in the background.
 *
 * VACUUM of a chunk index, as it goes through the index (folding changes
 * of counts, freezing), counts the chunks nothing holds; if there are any,
 * it starts a chunk freer, a background worker connected to the database,
 * which frees them (ChunkStoreFreeUnheld()) in a transaction of its own,
 * and those their references alone held with them.  If the log has pages
 * chunk_compact_dead_fraction of whose bytes are dead, VACUUM counting
 * them too, it then compacts them (ChunkStoreCompact()), a few pages to a
 * transaction, until the log has grown by more than VACUUM let it (none
 * if a region's pages are free, which what it moves goes on, else a
 * region's): the pages it empties are free once the next VACUUM takes out
 * the entries it moved, for the next freer's.  Then it exits.  A freeing or compaction a crash
 * or a deadlock stops is done again after the next VACUUM.
 * chunk_free_in_background turns it off.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/chunk_freer.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/xact.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_class.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "prolly/chunk_format.h"
#include "prolly/chunk_freer.h"
#include "prolly/chunk_index.h"
#include "prolly/chunk_log.h"
#include "prolly/chunk_store.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "tcop/tcopprot.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

bool		chunk_free_in_background = true;
double		chunk_compact_dead_fraction = 0.5;

/* the pages compacted to a transaction */
#define COMPACT_PAGES	8

typedef struct ChunkFreerArgs
{
	Oid			dboid;
	Oid			log;
	Oid			idx;
	bool		compact;
	double		fraction;
	BlockNumber grow;
} ChunkFreerArgs;

bool
ChunkFreerLaunch(Oid log, Oid idx, bool compact, double fraction,
				 BlockNumber grow)
{
	BackgroundWorker bgw;
	ChunkFreerArgs args;

	if (!chunk_free_in_background)
		return false;

	/*
	 * (the freer frees by the leaf format, the only one a backend starts
	 * with: from a session using another it would let go of no chunk's
	 * references, which would stay counted)
	 */
	if (ChunkFormat != &LeafChunkFormat)
		return false;
	memset(&bgw, 0, sizeof(bgw));
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	bgw.bgw_restart_time = BGW_NEVER_RESTART;
	snprintf(bgw.bgw_library_name, MAXPGPATH, "postgres");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "ChunkFreerMain");
	snprintf(bgw.bgw_name, BGW_MAXLEN, "chunk freer for chunk index %u", idx);
	snprintf(bgw.bgw_type, BGW_MAXLEN, "chunk freer");
	args.dboid = MyDatabaseId;
	args.log = log;
	args.idx = idx;
	args.compact = compact;
	args.fraction = fraction;
	args.grow = grow;
	StaticAssertStmt(sizeof(args) <= BGW_EXTRALEN, "chunk freer arguments fit");
	memcpy(bgw.bgw_extra, &args, sizeof(args));

	/* (none free: the next VACUUM tries again) */
	return RegisterDynamicBackgroundWorker(&bgw, NULL);
}

/*
 * One step of the freer's in a transaction of its own, the index locked
 * (one freer of an index at a time: another would do the same again);
 * false if another freer has it, or the relations are gone.
 */
static bool
freer_step(const ChunkFreerArgs *args, uint64 *from, BlockNumber *limit,
		   int *found, uint64 *freed, uint64 *chunks, uint64 *bytes)
{
	Relation	log;
	Relation	idx;
	bool		done = false;

	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	if (ConditionalLockDatabaseObject(RelationRelationId, args->idx, 0,
									  ExclusiveLock))
	{
		/* (compaction appends to the log) */
		log = try_relation_open(args->log, from == NULL ? AccessShareLock : RowExclusiveLock);
		idx = try_relation_open(args->idx, RowExclusiveLock);
		if (log != NULL && idx != NULL && RelationIsChunkLog(log) &&
			RelationIsChunkIndex(idx))
		{
			BlockNumber nblocks = RelationGetNumberOfBlocks(log);

			done = true;
			if (from == NULL)
				*freed = ChunkStoreFreeUnheld(log, idx);
			else
			{
				/* (the log grown by no more than it may: then room or none) */
				if (!BlockNumberIsValid(*limit))
					*limit = nblocks + args->grow;
				*found = ChunkStoreCompact(log, idx, args->fraction, from,
										   COMPACT_PAGES, nblocks < *limit,
										   chunks, bytes);
			}
		}
		if (idx)
			relation_close(idx, NoLock);
		if (log)
			relation_close(log, NoLock);
	}
	PopActiveSnapshot();
	CommitTransactionCommand();
	return done;
}

void
ChunkFreerMain(Datum main_arg)
{
	ChunkFreerArgs args;
	uint64		freed = 0;
	uint64		moved = 0;
	uint64		moved_bytes = 0;

	memcpy(&args, MyBgworkerEntry->bgw_extra, sizeof(args));
	BackgroundWorkerUnblockSignals();
	BackgroundWorkerInitializeConnectionByOid(args.dboid, BOOTSTRAP_SUPERUSERID, 0);

	pgstat_report_activity(STATE_RUNNING, "freeing chunks nothing holds");
	if (freer_step(&args, NULL, NULL, NULL, &freed, NULL, NULL) && args.compact)
	{
		uint64		from = 0;
		BlockNumber limit = InvalidBlockNumber;

		pgstat_report_activity(STATE_RUNNING, "compacting the chunk log");
		for (;;)
		{
			int			found = 0;
			uint64		chunks = 0;
			uint64		bytes = 0;

			if (!freer_step(&args, &from, &limit, &found, NULL, &chunks, &bytes) ||
				found <= 0)
				break;
			moved += chunks;
			moved_bytes += bytes;
		}
	}
	pgstat_report_stat(true);
	ereport(DEBUG1,
			(errmsg_internal("chunk freer freed %llu chunks of chunk index %u, and moved %llu (%llu bytes)",
							 (unsigned long long) freed, args.idx,
							 (unsigned long long) moved,
							 (unsigned long long) moved_bytes)));
	proc_exit(0);
}
