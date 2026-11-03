/*-------------------------------------------------------------------------
 *
 * chunk_freer.c
 *	  Freeing the history store's chunks in the background.
 *
 * A transaction letting go of a chunk's hold marks its bucket of the chunk
 * index (prolly/chunk_index.h's marked), and as it commits starts a chunk
 * freer, a background worker connected to the database (unless one is
 * freeing the index's chunks: see below), which frees what nothing holds
 * in the buckets marked, and what they alone held, in transactions of its
 * own: freeing costs the buckets marked, not the index.  VACUUM of a chunk
 * index, as it goes through the index (folding changes of counts,
 * freezing), marks a bucket with chunks nothing holds (a chunk put and
 * never held), and starts a freer if any is marked.  The log's space is
 * handed out and taken back a segment at a time (prolly/chunk_log.h);
 * if it has segments chunk_compact_dead_fraction of whose bytes are dead,
 * VACUUM counting them too, the freer then compacts them: their chunks
 * found in one visit of the index, and moved (ChunkStoreCompactStep()) a
 * batch to a transaction, into segments free, or (none free) a segment
 * new, filled whole.  And if the live chunks of the log's last segments
 * fit in the free segments below them, it evacuates them there, so that
 * the end is free, for VACUUM to cut off.  The segments it empties are
 * free once the next VACUUM takes out the entries it moved.  Then it
 * exits.  A compaction a crash or a deadlock stops is done again after the
 * next VACUUM; a freeing, by the next freer (the marks are WAL-logged).
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
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "tcop/tcopprot.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

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
	uint32		generation;		/* that registration's */
	bool		compact;
	bool		evacuate;
	double		fraction;
	BlockNumber grow;
} ChunkFreerArgs;

bool
ChunkFreerLaunch(Oid log, Oid idx, bool owner, bool compact, bool evacuate,
				 double fraction, BlockNumber grow, uint32 generation)
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
	args.generation = generation;
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
 * an entry an index, made as a transaction first marks a bucket of it):
 * the commits that asked for it, a count; whether a freer is registered,
 * or freeing (the freeing its), when it was, and which registration it is;
 * and the transactions running that passes left buckets marked for, which
 * one freer, the index's waiter, waits on.  A commit that marked a bucket
 * asks; it registers a freer if none is.  A freer passes over the buckets
 * marked again and again until a pass begins with no commit asking since.
 * Buckets it left for transactions running there, it lets the freeing go
 * and waits on them, the freeing another's to take meanwhile, and asks
 * once they end, however they end (committed, aborted, prepared and then
 * either): none is missed, and none waits holding the freeing.
 */
typedef struct FreerKey
{
	Oid			dboid;
	Oid			idx;
} FreerKey;

#define FREER_WAITING		64

typedef struct FreerEntry
{
	FreerKey	key;
	uint64		asked;			/* the commits that asked, a count */
	bool		worker;			/* a freer registered, or freeing */
	bool		started;		/* and it started */
	uint32		generation;		/* which registration it is */
	TimestampTz registered;
	bool		waiter;			/* a freer waiting on the transactions */
	ProcNumber	waiter_proc;	/* its, to wake */
	int			nwaiting;
	TransactionId waiting[FREER_WAITING];
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
		e->generation = 0;
		e->registered = 0;
		e->waiter = false;
		e->waiter_proc = INVALID_PROC_NUMBER;
		e->nwaiting = 0;
	}
	return e;
}

/*
 * The freeing taken for a freer to be registered now, its registration in
 * *generation: none is, or one registered was not started in time (entry
 * locked); false if another has it
 */
static bool
freer_take(FreerEntry *e, uint32 *generation)
{
	if (e->worker && !(!e->started &&
					   TimestampDifferenceExceeds(e->registered, GetCurrentTimestamp(),
												  FREER_START_MS)))
		return false;
	e->worker = true;
	e->started = false;
	e->generation++;
	e->registered = GetCurrentTimestamp();
	*generation = e->generation;
	return true;
}

/* the freeing let go of, if it is registration generation's still */
static void
freer_let_go(Oid dboid, Oid idx, uint32 generation)
{
	FreerEntry *e = freer_entry(dboid, idx, false);

	if (e)
	{
		if (e->worker && e->generation == generation)
			e->worker = false;
		dshash_release_lock(freer_table(), e);
	}
}

/* a freer for the index registered (the freeing taken for it), or let go */
static void
freer_register(Oid log, Oid idx, uint32 generation)
{
	/* (none free: the next to ask, or VACUUM, tries again) */
	if (!ChunkFreerLaunch(log, idx, true, false, false, chunk_compact_dead_fraction,
						  0, generation))
		freer_let_go(MyDatabaseId, idx, generation);
}

/* asking: a freer registered for the index if none is */
static void
freer_ask(Oid dboid, Oid log, Oid idx)
{
	FreerEntry *e = freer_entry(dboid, idx, false);
	uint32		generation;
	bool		reg;

	if (e == NULL)
		return;
	e->asked++;
	reg = freer_take(e, &generation);
	dshash_release_lock(freer_table(), e);
	if (reg)
		freer_register(log, idx, generation);
}

/* the indexes this transaction marked a bucket of */
typedef struct MarkedIndex
{
	Oid			log;
	Oid			idx;
} MarkedIndex;

static MarkedIndex *marked_indexes = NULL;
static int	nmarked = 0;
static int	marked_cap = 0;

/* (a freer's own transactions: it passes again if they mark) */
static bool am_freer = false;
static bool freer_marked = false;

static void
freer_xact_callback(XactEvent event, void *arg)
{
	int			n = nmarked;

	/*
	 * (asked for until the transaction ends: a root is let go of as it
	 * commits, its pre-commit's)
	 */
	if (event != XACT_EVENT_COMMIT && event != XACT_EVENT_ABORT &&
		event != XACT_EVENT_PARALLEL_ABORT && event != XACT_EVENT_PREPARE)
		return;
	nmarked = 0;

	/*
	 * (prepared, it asks too: a pass finds its changes running, and the
	 * waiter waits on it until it is committed or rolled back, which no
	 * callback here sees)
	 */
	if (event != XACT_EVENT_COMMIT && event != XACT_EVENT_PREPARE)
		return;
	for (int i = 0; i < n; i++)
		freer_ask(MyDatabaseId, marked_indexes[i].log, marked_indexes[i].idx);
}

void
ChunkFreerChanged(Relation idx, bool marked)
{
	static bool registered = false;
	uint64		stamp;
	Oid			log;

	if (am_freer)
	{
		freer_marked |= marked;
		return;
	}
	if (!marked)
		return;
	for (int i = 0; i < nmarked; i++)
		if (marked_indexes[i].idx == RelationGetRelid(idx))
			return;
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
	if (nmarked == marked_cap)
	{
		marked_cap = Max(4, marked_cap * 2);
		marked_indexes = marked_indexes ?
			repalloc(marked_indexes, marked_cap * sizeof(*marked_indexes)) :
			MemoryContextAlloc(TopMemoryContext, marked_cap * sizeof(*marked_indexes));
	}
	marked_indexes[nmarked].log = log;
	marked_indexes[nmarked].idx = RelationGetRelid(idx);
	nmarked++;
}

/* this freer's freeing, and its waiting, let go of as it exits, however */
static Oid	exit_idx = InvalidOid;
static Oid	exit_log = InvalidOid;
static uint32 exit_generation = 0;
static bool exit_freeing = false;
static bool exit_waiting = false;
static bool exit_wait_after = false;	/* to wait after compacting */
static bool exit_registered = false;

static void
freer_exit(int code, Datum arg)
{
	FreerEntry *e;

	if (!exit_freeing && !exit_waiting && !exit_wait_after)
		return;
	e = freer_entry(MyDatabaseId, exit_idx, false);
	if (e)
	{
		bool		left = false;

		if (exit_freeing && e->worker && e->generation == exit_generation)
			e->worker = false;
		if (exit_waiting)
		{
			e->waiter = false;
			e->waiter_proc = INVALID_PROC_NUMBER;
			left = e->nwaiting > 0;
		}

		/*
		 * (stopped compacting, the waiting not taken yet: none waiting, the
		 * transactions left to the next)
		 */
		else if (exit_wait_after)
			left = !e->waiter && e->nwaiting > 0;
		dshash_release_lock(freer_table(), e);

		/*
		 * (a waiter stopped, transactions left to wait on: a freer asked
		 * for, whose pass finds them, and waits on them in its stead)
		 */
		if (left)
			freer_ask(MyDatabaseId, exit_log, exit_idx);
	}
	exit_freeing = exit_waiting = exit_wait_after = false;
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
	/* the transactions running buckets were left for (a pass's, the first few) */
	int			nrunning;
	TransactionId running[FREER_WAITING];
	/* buckets left with chunks nothing holds, no transaction running there */
	uint32	   *retry;
	int			nretry;
	bool		retrying;
	bool		retry_left;		/* one left, freed again: nothing running there */
	bool		wait;			/* this freer the index's waiter, once done */
	bool		wait_after;		/* or to be, once it has compacted, if none is */
} FreerState;

static void
note_running(FreerState *fs, TransactionId xid)
{
	for (int i = 0; i < fs->nrunning; i++)
		if (fs->running[i] == xid)
			return;
	/* (past the few, the next pass's: a waiter asks again once they end) */
	if (fs->nrunning < FREER_WAITING)
		fs->running[fs->nrunning++] = xid;
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
			BlockNumber nblocks;
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
						else
							fs->retry_left = true;
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
					/* (what compaction appends to: this backend's part of the log) */
					nblocks = op == FREER_BEGIN_COMPACT ? ChunkLogOf(log)->write_blocks(log) : 0;
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
 * Waiting on the transactions passes left buckets marked for, this freer
 * the index's waiter, nothing of the freeing held: as any of them ends
 * (each looked at a second at a time, or as a pass tells of more), it is
 * taken off, and a freer asked for; until none is left.  A transaction
 * stays on until it ends, so a waiter stopped leaves it to the next.  (A
 * waiter is a background worker as long as the transactions it waits on
 * run, one an index.)
 */
static void
freer_wait(const ChunkFreerArgs *args)
{
	pgstat_report_activity(STATE_RUNNING, "waiting on transactions holding chunks to free");
	for (;;)
	{
		FreerEntry *e;
		bool		ended = false;
		bool		none;
		int			kept = 0;

		StartTransactionCommand();
		(void) GetTransactionSnapshot();
		e = freer_entry(args->dboid, args->idx, false);
		if (e == NULL)
		{
			CommitTransactionCommand();
			return;
		}
		for (int i = 0; i < e->nwaiting; i++)
		{
			if (TransactionIdIsInProgress(e->waiting[i]))
				e->waiting[kept++] = e->waiting[i];
			else
				ended = true;
		}
		e->nwaiting = kept;
		none = kept == 0 && !ended;
		if (none)
		{
			e->waiter = false;
			e->waiter_proc = INVALID_PROC_NUMBER;
			exit_waiting = false;
		}
		dshash_release_lock(freer_table(), e);
		CommitTransactionCommand();
		if (none)
			return;
		if (ended)
		{
			INJECTION_POINT("chunk-freer-waited", NULL);
			freer_ask(args->dboid, args->log, args->idx);
			continue;
		}
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 1000, PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * The chunks nothing holds freed, those of the buckets marked, and what
 * they alone held: a batch of buckets a transaction, then each unmarked in
 * another that holds none now; one with a transaction running there left.
 * Pass after pass, until one begins with no commit asking since the last;
 * then the freeing let go of, and, buckets left for transactions running,
 * their ends waited on (this freer the index's waiter, or the waiter told
 * of them).  This freer frees only if the freeing is its (registered for
 * it), or no other's; else it asks the other to pass again, and frees
 * nothing.  False if the index is gone.
 */
static bool
freer_free(const ChunkFreerArgs *args, FreerState *fs)
{
	FreerEntry *e;
	uint32		generation = args->generation;
	bool		mine;
	bool		retried_pass = false;

	e = freer_entry(args->dboid, args->idx, true);
	mine = args->owner ? e->worker && e->generation == generation :
		freer_take(e, &generation);
	if (mine)
		e->started = true;
	else
		e->asked++;
	dshash_release_lock(freer_table(), e);
	if (!mine)
		return true;
	exit_idx = args->idx;
	exit_log = args->log;
	exit_generation = generation;
	exit_freeing = true;
	before_shmem_exit(freer_exit, 0);
	exit_registered = true;
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
		fs->retry_left = false;
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

		/*
		 * (a bucket freed again and left again, nothing running there: one
		 * more pass, once; one left still is VACUUM's to find)
		 */
		if (!again && fs->retry_left && !retried_pass)
		{
			again = true;
			retried_pass = true;
		}
		if (!again)
		{
			ProcNumber	wake = INVALID_PROC_NUMBER;

			/* (the transactions to wait on the waiter's, this freer it if none is) */
			for (int r = 0; r < fs->nrunning; r++)
			{
				bool		known = false;

				for (int w = 0; w < e->nwaiting && !known; w++)
					known = e->waiting[w] == fs->running[r];
				if (!known && e->nwaiting < FREER_WAITING)
					e->waiting[e->nwaiting++] = fs->running[r];
			}
			/*
			 * (a freer to compact after: the waiting not taken now, which
			 * would hold it as the compaction runs; taken after, if no
			 * other has it)
			 */
			if (fs->nrunning > 0 && !e->waiter && (args->compact || args->evacuate))
				fs->wait_after = exit_wait_after = true;
			else if (fs->nrunning > 0 && !e->waiter)
			{
				e->waiter = true;
				e->waiter_proc = MyProcNumber;
				exit_waiting = true;
				fs->wait = true;
			}
			else if (fs->nrunning > 0)
				wake = e->waiter_proc;
			if (e->worker && e->generation == generation)
				e->worker = false;
			exit_freeing = false;
			dshash_release_lock(freer_table(), e);
			/* (the waiter told of more) */
			if (wake != INVALID_PROC_NUMBER)
				SetLatch(&GetPGProcByNumber(wake)->procLatch);
			break;
		}
		dshash_release_lock(freer_table(), e);
	}
	return true;

gone:
	freer_exit(0, 0);
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

		/* (the waiting last: a compaction waits behind no transaction) */
		if (fs.wait_after)
		{
			FreerEntry *e = freer_entry(args.dboid, args.idx, false);

			if (e && !e->waiter && e->nwaiting > 0)
			{
				e->waiter = true;
				e->waiter_proc = MyProcNumber;
				exit_waiting = true;
				fs.wait = true;
			}
			if (e)
				dshash_release_lock(freer_table(), e);
			exit_wait_after = false;
		}
		if (fs.wait)
			freer_wait(&args);
	}
	if (exit_registered)
		cancel_before_shmem_exit(freer_exit, 0);
	pgstat_report_stat(true);
	ereport(DEBUG1,
			(errmsg_internal("chunk freer freed %llu chunks of chunk index %u, looking in %llu buckets marked, and moved %llu (%llu bytes)",
							 (unsigned long long) fs.freed, args.idx,
							 (unsigned long long) fs.buckets_looked,
							 (unsigned long long) moved,
							 (unsigned long long) moved_bytes)));
	proc_exit(0);
}
