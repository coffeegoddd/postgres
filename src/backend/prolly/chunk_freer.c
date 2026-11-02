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
#include "storage/lock.h"
#include "tcop/tcopprot.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

bool		chunk_free_in_background = true;
double		chunk_compact_dead_fraction = 0.5;

/*
 * the segments a freer compacts (or evacuates) at most (a log may give
 * fewer: the tier's are packs); how much it moves a transaction is the
 * log's (compact_batch)
 */
#define COMPACT_SEGMENTS	4096

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
	 * (the freer frees by the format a backend starts with: from a session
	 * using another it would let go of no chunk's references, which would
	 * stay counted)
	 */
	if (ChunkFormat != ChunkFormatConfigured)
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

/*
 * The freeing of an index's chunks nothing holds is one freer's at a time:
 * its lock, a session's, taken by the freer as it starts to free and let
 * go of as it is done (a commit asking for a freer while one has it starts
 * none: that one, let go of it, looks again for buckets marked).
 */
#define FREER_LOCK_SUBID	1

static void
freer_locktag(LOCKTAG *tag, Oid dboid, Oid idx)
{
	SET_LOCKTAG_OBJECT(*tag, dboid, RelationRelationId, idx, FREER_LOCK_SUBID);
}

/* the indexes this transaction marked a bucket of, a freer asked for each */
typedef struct WantedFreer
{
	Oid			log;
	Oid			idx;
} WantedFreer;

static WantedFreer *wanted = NULL;
static int	nwanted = 0;
static int	wanted_cap = 0;

static void
freer_xact_callback(XactEvent event, void *arg)
{
	int			n = nwanted;

	/*
	 * (asked for until the transaction ends: a root is let go of as it
	 * commits, its pre-commit's)
	 */
	if (event != XACT_EVENT_COMMIT && event != XACT_EVENT_ABORT &&
		event != XACT_EVENT_PARALLEL_ABORT && event != XACT_EVENT_PREPARE)
		return;
	nwanted = 0;
	if (event != XACT_EVENT_COMMIT)
		return;
	for (int i = 0; i < n; i++)
	{
		LOCKTAG		tag;

		/* (one has it: it looks again once it lets go) */
		freer_locktag(&tag, MyDatabaseId, wanted[i].idx);
		if (LockAcquire(&tag, ExclusiveLock, true, true) == LOCKACQUIRE_NOT_AVAIL)
			continue;
		LockRelease(&tag, ExclusiveLock, true);
		(void) ChunkFreerLaunch(wanted[i].log, wanted[i].idx, false, false,
								chunk_compact_dead_fraction, 0);
	}
}

void
ChunkFreerWanted(Relation idx)
{
	static bool registered = false;
	uint64		stamp;
	Oid			log = ChunkIndex->log_of(idx, &stamp);

	if (!OidIsValid(log))
		return;
	for (int i = 0; i < nwanted; i++)
		if (wanted[i].idx == RelationGetRelid(idx))
			return;
	if (!registered)
	{
		RegisterXactCallback(freer_xact_callback, NULL);
		registered = true;
	}
	if (nwanted == wanted_cap)
	{
		wanted_cap = Max(4, wanted_cap * 2);
		wanted = wanted ? repalloc(wanted, wanted_cap * sizeof(*wanted)) :
			MemoryContextAlloc(TopMemoryContext, wanted_cap * sizeof(*wanted));
	}
	wanted[nwanted].log = log;
	wanted[nwanted].idx = RelationGetRelid(idx);
	nwanted++;
}

/* what the freer does in a transaction */
typedef enum FreerOp
{
	FREER_FREE,					/* free the chunks nothing holds, of the
								 * buckets marked */
	FREER_UNMARK,				/* unmark the buckets freed */
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
	/* freeing: the buckets marked from this one, those being freed */
	uint32		from;
	uint32	   *buckets;
	int			nbuckets;
	uint64		buckets_looked; /* the buckets looked in */
	int			kept;			/* the buckets left marked, a pass's */
	TransactionId *waits;		/* transactions running there */
	int			nwaits;
} FreerState;

#define FREE_BUCKETS		1024

/* a chunk of a bucket marked, its holds none: to free */
static void
note_unheld(void *arg, const ChunkIndexEntry *e)
{
	List	  **unheld = arg;

	if (e->refs == 0 && e->roots == 0)
	{
		uint8	   *h = palloc(CHUNK_HASH_LEN);

		memcpy(h, e->hash, CHUNK_HASH_LEN);
		*unheld = lappend(*unheld, h);
	}
}

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
	/* (freeing waits for another freer's compaction step; compacting, not) */
	if (op == FREER_FREE || op == FREER_UNMARK ?
		(LockDatabaseObject(RelationRelationId, args->idx, 0, ExclusiveLock), true) :
		ConditionalLockDatabaseObject(RelationRelationId, args->idx, 0,
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
			int			batch;
			uint64		batch_bytes;

			done = true;
			switch (op)
			{
				case FREER_FREE:
					{
						List	   *unheld = NIL;
						uint8	   *hashes;
						int			i = 0;

						fs->nbuckets = ChunkIndex->marked(idx, fs->from, FREE_BUCKETS,
														  fs->buckets);
						for (int b = 0; b < fs->nbuckets; b++)
							ChunkIndex->visit_bucket(idx, fs->buckets[b], note_unheld, &unheld);
						fs->buckets_looked += fs->nbuckets;
						hashes = palloc(Max(list_length(unheld), 1) * CHUNK_HASH_LEN);
						foreach_ptr(uint8, h, unheld)
							memcpy(hashes + (Size) i++ * CHUNK_HASH_LEN, h, CHUNK_HASH_LEN);
						if (i > 0)
							fs->freed += ChunkStoreFree(log, idx, hashes, i);
						list_free_deep(unheld);
						pfree(hashes);
						break;
					}
				case FREER_UNMARK:
					fs->nwaits = 0;
					for (int b = 0; b < fs->nbuckets; b++)
					{
						TransactionId w;

						if (!ChunkIndex->unmark(idx, fs->buckets[b], &w))
						{
							if (TransactionIdIsValid(w))
								fs->waits[fs->nwaits++] = w;
							else
								fs->kept++;
						}
					}
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
					ChunkLogOf(log)->compact_batch(log, &batch, &batch_bytes);
					fs->looked = ChunkStoreCompactStep(log, idx, fs->c, batch, batch_bytes,
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

/*
 * The chunks nothing holds freed, those of the buckets marked, and what
 * they alone held: a batch of buckets a transaction, then each unmarked in
 * another that holds none now; one with a transaction running there waited
 * for, and freed again.  The freeing's lock let go of, the buckets marked
 * looked at again: one marked since, as it was held, is freed too.  False
 * if the index is gone, or another freer has it.
 */
static bool
freer_free(const ChunkFreerArgs *args, FreerState *fs)
{
	LOCKTAG		tag;

	freer_locktag(&tag, args->dboid, args->idx);
	fs->buckets = MemoryContextAlloc(fs->cxt, FREE_BUCKETS * sizeof(uint32));
	fs->waits = MemoryContextAlloc(fs->cxt, FREE_BUCKETS * sizeof(TransactionId));

	/* (another frees them: this one compacts, if it is to) */
	if (LockAcquire(&tag, ExclusiveLock, true, true) == LOCKACQUIRE_NOT_AVAIL)
		return true;
	for (;;)
	{
		int			left;

		fs->from = 0;
		fs->kept = 0;
		for (;;)
		{
			if (!freer_step(args, FREER_FREE, fs))
			{
				LockRelease(&tag, ExclusiveLock, true);
				return false;
			}
			if (fs->nbuckets == 0)
				break;
			if (!freer_step(args, FREER_UNMARK, fs))
			{
				LockRelease(&tag, ExclusiveLock, true);
				return false;
			}
			if (fs->nwaits > 0)
			{
				/* (the transactions there ended, the batch freed again) */
				StartTransactionCommand();
				for (int i = 0; i < fs->nwaits; i++)
					XactLockTableWait(fs->waits[i], NULL, NULL, XLTW_None);
				CommitTransactionCommand();
				continue;
			}
			fs->from = fs->buckets[fs->nbuckets - 1] + 1;
		}
		LockRelease(&tag, ExclusiveLock, true);

		/*
		 * (marked since, by a commit that found the lock held: looked at
		 * again, unless the lock is another's now)
		 */
		StartTransactionCommand();
		{
			Relation	idx = try_relation_open(args->idx, AccessShareLock);

			left = idx ? ChunkIndex->marked(idx, 0, fs->kept + 1, fs->buckets) : 0;
			if (idx)
				relation_close(idx, AccessShareLock);
		}
		CommitTransactionCommand();
		if (left <= fs->kept ||
			LockAcquire(&tag, ExclusiveLock, true, true) == LOCKACQUIRE_NOT_AVAIL)
			return true;
	}
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
	if (freer_free(&args, &fs))
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
			(errmsg_internal("chunk freer freed %llu chunks of chunk index %u, looking in %llu buckets marked, and moved %llu (%llu bytes)",
							 (unsigned long long) fs.freed, args.idx,
							 (unsigned long long) fs.buckets_looked,
							 (unsigned long long) moved,
							 (unsigned long long) moved_bytes)));
	proc_exit(0);
}
