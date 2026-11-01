/*-------------------------------------------------------------------------
 *
 * chunk_freer.c
 *	  Freeing the history store's chunks in the background.
 *
 * VACUUM of a chunk index, as it goes through the index (folding changes
 * of counts, freezing), counts the chunks nothing holds; if there are any,
 * it starts a chunk freer, a background worker connected to the database,
 * which frees them (ChunkStoreFreeUnheld()) in a transaction of its own,
 * and those their references alone held with them, and exits.  A freeing
 * a crash or a deadlock stops is done again after the next VACUUM.
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
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "tcop/tcopprot.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

bool		chunk_free_in_background = true;

typedef struct ChunkFreerArgs
{
	Oid			dboid;
	Oid			log;
	Oid			idx;
} ChunkFreerArgs;

bool
ChunkFreerLaunch(Oid log, Oid idx)
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
	StaticAssertStmt(sizeof(args) <= BGW_EXTRALEN, "chunk freer arguments fit");
	memcpy(bgw.bgw_extra, &args, sizeof(args));

	/* (none free: the next VACUUM tries again) */
	return RegisterDynamicBackgroundWorker(&bgw, NULL);
}

void
ChunkFreerMain(Datum main_arg)
{
	ChunkFreerArgs args;
	Relation	log;
	Relation	idx;
	uint64		freed;

	memcpy(&args, MyBgworkerEntry->bgw_extra, sizeof(args));
	BackgroundWorkerUnblockSignals();
	BackgroundWorkerInitializeConnectionByOid(args.dboid, BOOTSTRAP_SUPERUSERID, 0);

	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	pgstat_report_activity(STATE_RUNNING, "freeing chunks nothing holds");

	/* (one freer of an index at a time: another would do the same again) */
	if (!ConditionalLockDatabaseObject(RelationRelationId, args.idx, 0,
									   ExclusiveLock))
	{
		PopActiveSnapshot();
		CommitTransactionCommand();
		proc_exit(0);
	}
	log = try_relation_open(args.log, AccessShareLock);
	idx = try_relation_open(args.idx, RowExclusiveLock);
	if (log == NULL || idx == NULL || !RelationIsChunkLog(log) ||
		!RelationIsChunkIndex(idx))
		freed = 0;
	else
		freed = ChunkStoreFreeUnheld(log, idx);
	if (idx)
		relation_close(idx, NoLock);
	if (log)
		relation_close(log, NoLock);
	PopActiveSnapshot();
	CommitTransactionCommand();
	pgstat_report_stat(true);
	ereport(DEBUG1,
			(errmsg_internal("chunk freer freed %llu chunks of chunk index %u",
							 (unsigned long long) freed, args.idx)));
	proc_exit(0);
}
