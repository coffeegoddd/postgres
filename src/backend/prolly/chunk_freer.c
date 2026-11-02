/*-------------------------------------------------------------------------
 *
 * chunk_freer.c
 *	  Freeing the history store's chunks in the background.
 *
 * VACUUM of a chunk index, as it goes through the index (folding changes
 * of counts, freezing), counts the chunks nothing holds; if there are any,
 * it starts a chunk freer, a background worker connected to the database,
 * which frees them (ChunkStoreFreeUnheld()) in a transaction of its own,
 * and those their references alone held with them.  The log's space is
 * handed out and taken back a segment at a time (prolly/chunk_log.h);
 * if it has segments chunk_compact_dead_fraction of whose bytes are dead,
 * VACUUM counting them too, the freer then compacts them: their chunks
 * found in one visit of the index, and moved (ChunkStoreCompactStep()) a
 * batch to a transaction, into segments free, or (none free) a segment
 * new, filled whole.  And if the live chunks of the log's last segments
 * fit in the free segments below them, it evacuates them there, so that
 * the end is free, for VACUUM to cut off.  The segments it empties are
 * free once the next VACUUM takes out the entries it moved.  Then it
 * exits.  A freeing or compaction a crash or a deadlock stops is done
 * again after the next VACUUM.
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
#include "utils/injection_point.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

bool		chunk_free_in_background = true;
double		chunk_compact_dead_fraction = 0.5;

/*
 * the segments a freer compacts (or evacuates) at most, and the chunks it
 * moves to a transaction
 */
#define COMPACT_SEGMENTS	4096
#define COMPACT_BATCH		256

typedef struct ChunkFreerArgs
{
	Oid			dboid;
	Oid			log;
	Oid			idx;
	bool		compact;
	bool		evacuate;
	double		fraction;
	BlockNumber grow;
} ChunkFreerArgs;

bool
ChunkFreerLaunch(Oid log, Oid idx, bool compact, bool evacuate,
				 double fraction, BlockNumber grow)
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
	args.evacuate = evacuate;
	args.fraction = fraction;
	args.grow = grow;
	StaticAssertStmt(sizeof(args) <= BGW_EXTRALEN, "chunk freer arguments fit");
	memcpy(bgw.bgw_extra, &args, sizeof(args));

	/* (none free: the next VACUUM tries again) */
	return RegisterDynamicBackgroundWorker(&bgw, NULL);
}

/* what the freer does in a transaction */
typedef enum FreerOp
{
	FREER_FREE,					/* free the chunks nothing holds */
	FREER_BEGIN_COMPACT,		/* find the segments to compact, their chunks */
	FREER_BEGIN_EVACUATE,		/* find the segments to evacuate, their chunks */
	FREER_STEP,					/* move the next batch */
} FreerOp;

typedef struct FreerState
{
	MemoryContext cxt;			/* for the compaction, across transactions */
	ChunkCompaction *c;
	BlockNumber limit;			/* the log's pages it may grow to */
	uint64		below;			/* where room for moves must end before */
	int			looked;			/* the step's chunks looked at, or -1 */
	uint64		freed;
	uint64		chunks;
	uint64		bytes;
} FreerState;

/*
 * One step of the freer's in a transaction of its own, the index locked
 * (one freer of an index at a time: another would do the same again);
 * false if another freer has it, or the relations are gone.
 */
static bool
freer_step(const ChunkFreerArgs *args, FreerOp op, FreerState *fs)
{
	Relation	log;
	Relation	idx;
	bool		done = false;

	if (op == FREER_STEP)
		INJECTION_POINT("chunk-freer-step", NULL);
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	if (ConditionalLockDatabaseObject(RelationRelationId, args->idx, 0,
									  ExclusiveLock))
	{
		/* (compaction appends to the log) */
		log = try_relation_open(args->log, op == FREER_FREE ? AccessShareLock : RowExclusiveLock);
		idx = try_relation_open(args->idx, RowExclusiveLock);
		if (log != NULL && idx != NULL && RelationIsAnyChunkLog(log) &&
			RelationIsChunkIndex(idx))
		{
			BlockNumber nblocks = RelationGetNumberOfBlocks(log);
			uint64		starts[COMPACT_SEGMENTS];
			uint64		ends[COMPACT_SEGMENTS];
			int			n;

			done = true;
			switch (op)
			{
				case FREER_FREE:
					fs->freed = ChunkStoreFreeUnheld(log, idx);
					break;
				case FREER_BEGIN_COMPACT:
				case FREER_BEGIN_EVACUATE:
					n = op == FREER_BEGIN_COMPACT ?
						ChunkLogOf(log)->compactable(log, args->fraction, COMPACT_SEGMENTS, starts, ends) :
						ChunkLogOf(log)->evacuable(log, COMPACT_SEGMENTS, starts, ends);
					fs->c = ChunkStoreCompactBegin(log, idx, n, starts, ends, fs->cxt);

					/*
					 * (compacting, the log grown by no more than it may;
					 * evacuating, into room below the end alone, never new)
					 */
					fs->limit = op == FREER_BEGIN_COMPACT ? nblocks + args->grow : 0;
					fs->below = op == FREER_BEGIN_EVACUATE && n > 0 ? starts[0] : 0;
					break;
				case FREER_STEP:
					fs->looked = ChunkStoreCompactStep(log, idx, fs->c, COMPACT_BATCH,
													   fs->below, fs->limit,
													   &fs->chunks, &fs->bytes);
					break;
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

/* a compaction, or an evacuation, begun and stepped through to its end */
static void
freer_compact(const ChunkFreerArgs *args, FreerOp begin, FreerState *fs,
			  uint64 *moved, uint64 *moved_bytes)
{
	if (!freer_step(args, begin, fs))
		return;
	for (;;)
	{
		fs->looked = 0;
		fs->chunks = fs->bytes = 0;
		if (!freer_step(args, FREER_STEP, fs))
			break;
		*moved += fs->chunks;
		*moved_bytes += fs->bytes;
		if (fs->looked <= 0)
			break;
	}
	ChunkStoreCompactEnd(fs->c);
	fs->c = NULL;
}

void
ChunkFreerMain(Datum main_arg)
{
	ChunkFreerArgs args;
	FreerState	fs = {0};
	uint64		moved = 0;
	uint64		moved_bytes = 0;

	memcpy(&args, MyBgworkerEntry->bgw_extra, sizeof(args));
	BackgroundWorkerUnblockSignals();
	BackgroundWorkerInitializeConnectionByOid(args.dboid, BOOTSTRAP_SUPERUSERID, 0);
	fs.cxt = AllocSetContextCreate(TopMemoryContext, "chunk freer",
								   ALLOCSET_DEFAULT_SIZES);

	pgstat_report_activity(STATE_RUNNING, "freeing chunks nothing holds");
	if (freer_step(&args, FREER_FREE, &fs))
	{
		if (args.compact)
		{
			pgstat_report_activity(STATE_RUNNING, "compacting the chunk log");
			freer_compact(&args, FREER_BEGIN_COMPACT, &fs, &moved, &moved_bytes);
		}
		if (args.evacuate)
		{
			pgstat_report_activity(STATE_RUNNING, "evacuating the chunk log's end");
			freer_compact(&args, FREER_BEGIN_EVACUATE, &fs, &moved, &moved_bytes);
		}
	}
	pgstat_report_stat(true);
	ereport(DEBUG1,
			(errmsg_internal("chunk freer freed %llu chunks of chunk index %u, and moved %llu (%llu bytes)",
							 (unsigned long long) fs.freed, args.idx,
							 (unsigned long long) moved,
							 (unsigned long long) moved_bytes)));
	proc_exit(0);
}
