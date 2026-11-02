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
#include "access/subtrans.h"
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
#include "storage/dsm_registry.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "tcop/tcopprot.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

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
	bool		owner;			/* registered for the index's freeing */
	bool		compact;
	bool		evacuate;
	double		fraction;
	BlockNumber grow;
} ChunkFreerArgs;

bool
ChunkFreerLaunch(Oid log, Oid idx, bool owner, bool compact, bool evacuate,
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
	args.owner = owner;
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
 * Each index's freeing, shared (a hash of its own in dynamic shared memory,
 * an entry an index, made as a transaction first changes the index's
 * counts): the commits that asked for it, a count; whether a freer is
 * registered, or freeing, and when it was registered; and the
 * transactions running that a pass left buckets marked for (a few; past
 * them, any).  A commit that marked a bucket asks; so does one of those
 * transactions' (or any, past the few) that changed the counts.  One that
 * asks registers a freer if none is; a freer passes over the buckets
 * marked again and again until a pass begins with no commit asking since,
 * then lets go.  So a freer is registered once for many commits, and none
 * is missed: what a commit marks is passed over after it.
 */
typedef struct FreerKey
{
	Oid			dboid;
	Oid			idx;
} FreerKey;

#define FREER_RUNNING		8

typedef struct FreerEntry
{
	FreerKey	key;
	uint64		asked;			/* the commits that asked, a count */
	bool		worker;			/* a freer registered, or freeing */
	bool		started;		/* and it started */
	TimestampTz registered;
	/* the transactions running a pass left buckets marked for (or any) */
	int			nrunning;
	bool		any_running;
	TransactionId running[FREER_RUNNING];
} FreerEntry;

static const dshash_parameters freer_params = {
	sizeof(FreerKey),
	sizeof(FreerEntry),
	dshash_memcmp,
	dshash_memhash,
	dshash_memcpy,
	0
};

/* (a freer registered and not started in this long: taken for gone) */
#define FREER_START_MS		10000

static dshash_table *freers = NULL;

static dshash_table *
freer_table(void)
{
	bool		found;

	if (freers == NULL)
		freers = GetNamedDSHash("prolly chunk freers", &freer_params, &found);
	return freers;
}

/* the index's entry, made if it is not there, locked; the caller lets go */
static FreerEntry *
freer_entry(Oid dboid, Oid idx, bool make)
{
	FreerKey	key = {.dboid = dboid,.idx = idx};
	FreerEntry *e;
	bool		found;

	if (!make)
		return dshash_find(freer_table(), &key, true);
	e = dshash_find_or_insert(freer_table(), &key, &found);
	if (!found)
	{
		e->asked = 0;
		e->worker = false;
		e->started = false;
		e->registered = 0;
		e->nrunning = 0;
		e->any_running = false;
	}
	return e;
}

/*
 * Whether a freer is to be registered now: none is, or one registered was
 * not started in time (entry locked)
 */
static bool
freer_take(FreerEntry *e)
{
	if (e->worker && !(!e->started &&
					   TimestampDifferenceExceeds(e->registered, GetCurrentTimestamp(),
												  FREER_START_MS)))
		return false;
	e->worker = true;
	e->started = false;
	e->registered = GetCurrentTimestamp();
	return true;
}

/* a freer for the index registered (the entry taken for it), or let go */
static void
freer_register(Oid log, Oid idx)
{
	if (!ChunkFreerLaunch(log, idx, true, false, false, chunk_compact_dead_fraction, 0))
	{
		FreerEntry *e = freer_entry(MyDatabaseId, idx, false);

		/* (none free: the next to ask, or VACUUM, tries again) */
		if (e)
		{
			e->worker = false;
			dshash_release_lock(freer_table(), e);
		}
	}
}

/* the indexes this transaction changed the counts of, and whether it marked */
typedef struct ChangedIndex
{
	Oid			log;
	Oid			idx;
	bool		marked;
	TransactionId xid;			/* the transaction's, as it changed them */
} ChangedIndex;

static ChangedIndex *changed = NULL;
static int	nchanged = 0;
static int	changed_cap = 0;

/* (a freer's own transactions: it passes again if they mark) */
static bool am_freer = false;
static bool freer_marked = false;

static void
freer_xact_callback(XactEvent event, void *arg)
{
	int			n = nchanged;

	/*
	 * (asked for until the transaction ends: a root is let go of as it
	 * commits, its pre-commit's)
	 */
	if (event != XACT_EVENT_COMMIT && event != XACT_EVENT_ABORT &&
		event != XACT_EVENT_PARALLEL_ABORT && event != XACT_EVENT_PREPARE)
		return;
	nchanged = 0;
	if (event != XACT_EVENT_COMMIT)
		return;
	for (int i = 0; i < n; i++)
	{
		FreerEntry *e = freer_entry(MyDatabaseId, changed[i].idx, false);
		bool		reg = false;
		bool		waited;

		if (e == NULL)
			continue;
		waited = e->any_running;
		for (int r = 0; r < e->nrunning && !waited; r++)
			waited = e->running[r] == changed[i].xid;
		if (changed[i].marked || waited)
		{
			e->asked++;
			reg = freer_take(e);
		}
		dshash_release_lock(freer_table(), e);
		if (reg)
			freer_register(changed[i].log, changed[i].idx);
	}
}

void
ChunkFreerChanged(Relation idx, bool marked)
{
	static bool registered = false;
	uint64		stamp;
	Oid			log;
	int			i;

	if (am_freer)
	{
		freer_marked |= marked;
		return;
	}
	for (i = 0; i < nchanged; i++)
		if (changed[i].idx == RelationGetRelid(idx))
			break;
	if (i < nchanged)
	{
		changed[i].marked |= marked;
		return;
	}
	log = ChunkIndex->log_of(idx, &stamp);
	if (!OidIsValid(log) || !chunk_free_in_background ||
		ChunkFormat != ChunkFormatConfigured)
		return;

	/* (the entry made now, in the transaction: not as it commits) */
	dshash_release_lock(freer_table(),
						freer_entry(MyDatabaseId, RelationGetRelid(idx), true));
	if (!registered)
	{
		RegisterXactCallback(freer_xact_callback, NULL);
		registered = true;
	}
	if (nchanged == changed_cap)
	{
		changed_cap = Max(4, changed_cap * 2);
		changed = changed ? repalloc(changed, changed_cap * sizeof(*changed)) :
			MemoryContextAlloc(TopMemoryContext, changed_cap * sizeof(*changed));
	}
	changed[nchanged].log = log;
	changed[nchanged].idx = RelationGetRelid(idx);
	changed[nchanged].marked = marked;
	changed[nchanged].xid = GetTopTransactionIdIfAny();
	nchanged++;
}

/* a freer's exit: its index's freeing let go of, should it not have been */
static void
freer_exit(int code, Datum arg)
{
	FreerEntry *e = freer_entry(MyDatabaseId, DatumGetObjectId(arg), false);

	if (e)
	{
		e->worker = false;
		dshash_release_lock(freer_table(), e);
	}
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
	/* the transactions running buckets were left for (or any, past them) */
	int			nrunning;
	bool		any_running;
	TransactionId running[FREER_RUNNING];
	/* buckets left with chunks nothing holds, no transaction running there */
	uint32	   *retry;
	int			nretry;
	bool		retrying;
} FreerState;

static void
note_running(FreerState *fs, TransactionId xid)
{
	for (int i = 0; i < fs->nrunning; i++)
		if (fs->running[i] == xid)
			return;
	if (fs->nrunning < FREER_RUNNING)
		fs->running[fs->nrunning++] = xid;
	else
		fs->any_running = true;
}

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
					fs->nretry = 0;
					for (int b = 0; b < fs->nbuckets; b++)
					{
						TransactionId w;

						if (ChunkIndex->unmark(idx, fs->buckets[b], &w))
							continue;
						if (TransactionIdIsValid(w))
							note_running(fs, SubTransGetTopmostTransaction(w));
						else if (!fs->retrying)
							fs->retry[fs->nretry++] = fs->buckets[b];
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
 * another that holds none now; one with a transaction running there left
 * (that one's commit, or the next to change the counts, asks again).  Pass
 * after pass, until one begins with no commit asking since the last; then
 * the index's freeing let go of.  This freer frees only if the freeing is
 * its (registered for it), or no other's; else it asks the other to pass
 * again, and frees nothing.  False if the index is gone.
 */
static bool
freer_free(const ChunkFreerArgs *args, FreerState *fs)
{
	FreerEntry *e;
	bool		mine;

	e = freer_entry(args->dboid, args->idx, true);
	mine = args->owner || freer_take(e);
	if (mine)
		e->started = true;
	else
		e->asked++;
	dshash_release_lock(freer_table(), e);
	if (!mine)
		return true;
	before_shmem_exit(freer_exit, ObjectIdGetDatum(args->idx));
	am_freer = true;
	fs->buckets = MemoryContextAlloc(fs->cxt, FREE_BUCKETS * sizeof(uint32));
	fs->retry = MemoryContextAlloc(fs->cxt, FREE_BUCKETS * sizeof(uint32));
	for (;;)
	{
		uint64		asked;
		bool		again;

		e = freer_entry(args->dboid, args->idx, false);
		asked = e->asked;
		dshash_release_lock(freer_table(), e);
		freer_marked = false;
		fs->from = 0;
		fs->nrunning = 0;
		fs->any_running = false;
		INJECTION_POINT("chunk-freer-pass", NULL);
		for (;;)
		{
			uint32		next;

			fs->retrying = false;
			if (!freer_step(args, FREER_FREE, fs))
				goto gone;
			INJECTION_POINT("chunk-freer-freed", NULL);
			if (fs->nbuckets > 0 && !freer_step(args, FREER_UNMARK, fs))
				goto gone;
			if (fs->nbuckets == 0)
				break;
			next = fs->buckets[fs->nbuckets - 1] + 1;

			/*
			 * (buckets left with chunks nothing holds and no transaction
			 * running there: one that ended between the two, its change seen
			 * now; freed again, once)
			 */
			if (fs->nretry > 0)
			{
				memcpy(fs->buckets, fs->retry, fs->nretry * sizeof(uint32));
				fs->nbuckets = fs->nretry;
				fs->retrying = true;
				if (!freer_step(args, FREER_FREE, fs) ||
					!freer_step(args, FREER_UNMARK, fs))
					goto gone;
			}
			fs->from = next;
		}
		INJECTION_POINT("chunk-freer-passed", NULL);
		e = freer_entry(args->dboid, args->idx, false);
		again = e->asked != asked || freer_marked;
		if (!again)
		{
			e->nrunning = fs->nrunning;
			memcpy(e->running, fs->running, fs->nrunning * sizeof(TransactionId));
			e->any_running = fs->any_running;
			e->worker = false;
		}
		dshash_release_lock(freer_table(), e);
		if (!again)
			break;
	}
	cancel_before_shmem_exit(freer_exit, ObjectIdGetDatum(args->idx));
	return true;

gone:
	cancel_before_shmem_exit(freer_exit, ObjectIdGetDatum(args->idx));
	freer_exit(0, ObjectIdGetDatum(args->idx));
	return false;
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
