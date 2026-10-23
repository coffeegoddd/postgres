/*-------------------------------------------------------------------------
 *
 * refs_generations.c
 *	  Each branch's generation, and the working set a fold of it made: the
 *	  folds the "copies" refs skip when nothing changed (#482).
 *
 * A fold of a branch's working set reads its rows (by a snapshot), its
 * versioned sequences (as they are now) and the catalogs.  Each branch has
 * a generation, in shared memory, that moves whenever a transaction that
 * wrote any of those ends (commits, aborts, or is prepared), and a count
 * of the transactions writing it now: each is told of (note_write) before
 * anyone else can see its write, by every writer, hushed or not.  An epoch
 * moves with every catalog change sent to other sessions, since a fold
 * describes objects by what they depend on outside the branch too (role
 * names, types elsewhere), and with every collection that dropped chunks
 * (a fold's tree no ref names is garbage to it).
 *
 * The branch's writers that ended are remembered too, so a snapshot can be
 * asked whether it sees every one that committed: the last few by their
 * transaction ids, those before by the latest of them (seen if it precedes
 * the snapshot's xmin, as each then does).
 *
 * A fold made while nobody wrote the branch, during which nothing ended
 * and the epoch stayed, by a snapshot that saw every writer that ended, is
 * the working set until one more ends or the epoch moves: it is published,
 * with the baseline it folded from and the root it made, and known once
 * its transaction commits (its tree's chunks are others' only then, and
 * gone if it aborts), if nothing ended meanwhile either.  A later fold by
 * a snapshot that sees every writer that ended too, from the same
 * baseline, takes that root instead of folding.  Nothing is published or
 * taken by a transaction that wrote the branch or a catalog (its fold
 * holds what nobody else sees), while any transaction is prepared (whose
 * commit nobody here is told of), or in recovery (nor of a replayed one's).
 *
 * Generations are taken from one counter, so one is never given twice: a
 * branch without an entry has the counter's value as of the last eviction
 * (and its writers are remembered with every evicted entry's), and an
 * entry made for a writer a new one.  A writer finding no entry free
 * counts as a writer of every branch, as does a prepared transaction as
 * it is committed or rolled back (one recovered at startup was never told
 * of).  Nothing survives a restart: the first fold after one folds.
 *
 * Each branch also keeps the relations whose rows its last writers wrote
 * (note_relation), each with the generation its writer ended at: the last
 * WRITTEN_RELATIONS of them, and the generation since which every one is
 * there.  A canonicalizer that marked a fold (branch_fold_mark()) asks
 * later which relations were written since (branch_written_since()), and
 * works out again only those.  Anything that changes a fold otherwise
 * moves what the mark took: a catalog change or collection the epoch, a
 * writer of every branch or a prepared transaction's end the overflow, a
 * writer of too many relations or an eviction the generation since which
 * they are all there.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/refs_generations.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"
#include "access/prolly_shmem.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "refs_generations.h"
#include "storage/lwlock.h"
#include "utils/injection_point.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"

#define BRANCH_GENERATIONS	1024
#define ENDED_WRITERS		8
#define WRITTEN_RELATIONS	32

/* A relation whose rows a writer that ended at generation wrote. */
typedef struct WrittenRelation
{
	uint64		generation;
	Oid			relid;
} WrittenRelation;

/*
 * Writers that ended: the last ENDED_WRITERS by id, and the latest id of
 * those before (invalid: none).
 */
typedef struct EndedWriters
{
	FullTransactionId last[ENDED_WRITERS];
	int			nlast;
	FullTransactionId before;
} EndedWriters;

typedef struct BranchGeneration
{
	Oid			database;
	Oid			branch;			/* InvalidOid: a free entry */
	uint64		generation;
	int			writers;		/* transactions writing it now */
	EndedWriters ended;
	bool		known;			/* the working set below is known */
	bool		complete;		/* and its fold left nothing out */
	uint64		epoch;			/* what it was folded at */
	uint64		overflow;
	ProllyHash	baseline;		/* folded from (len 0: none) */
	ProllyHash	root;
	uint64		seen;			/* the generation the fold worker last saw */
	/* the relations written: a ring of the last, all since rels_from */
	uint64		rels_from;
	int			nrels;
	int			relnext;
	WrittenRelation rels[WRITTEN_RELATIONS];
} BranchGeneration;

typedef struct BranchGenerations
{
	LWLock		lock;
	pg_atomic_uint64 epoch;
	uint64		counter;		/* the last generation given */
	uint64		absent;			/* a branch's without an entry */
	FullTransactionId absent_ended; /* the latest writer of one ended */
	int			overflow_writers;	/* writers that found no entry free */
	uint64		overflow;		/* moves as one of them ends */
	FullTransactionId overflow_ended;	/* the latest of them that ended */
	int			hand;			/* the next entry to evict, if it can be */
	BranchGeneration entries[BRANCH_GENERATIONS];
} BranchGenerations;

static BranchGenerations *gens = NULL;

/* A fold the transaction published, known once it commits. */
typedef struct PendingFold
{
	Oid			branch;
	uint64		generation;
	uint64		overflow;
	uint64		epoch;
	bool		complete;
	ProllyHash	baseline;
	ProllyHash	root;
	int			level;			/* the (sub)transaction's nesting level */
} PendingFold;

static List *pending = NIL;		/* in TopTransactionContext */
static bool publish_callbacks = false;

/*
 * The branches the transaction is noted writing, as their entries hold
 * them (or as an overflow writer).  More than fit are noted as overflow.
 */
#define MAX_WRITTEN 16
static Oid	written[MAX_WRITTEN];
static int	nwritten = 0;
static bool written_overflow = false;
static bool callback_registered = false;

/*
 * And the relations whose rows it wrote, on which branch: more than fit,
 * and none of its branches is known to have been written only in these.
 */
#define MAX_WRITTEN_RELS 64
static struct
{
	Oid			branch;
	Oid			relid;
}			written_rels[MAX_WRITTEN_RELS];
static int	nwritten_rels = 0;
static bool written_rels_overflow = false;

static BranchGenerations *
shared(void)
{
	if (gens == NULL)
		gens = prolly_shmem_area(&prolly_refs_copies.info);
	return gens;
}

Size
branch_generations_shmem_size(void)
{
	return sizeof(BranchGenerations);
}

void
branch_generations_shmem_init(void *area)
{
	BranchGenerations *g = area;

	memset(g, 0, sizeof(*g));
	LWLockInitialize(&g->lock, LWLockNewTrancheId("prolly branch generations"));
	pg_atomic_init_u64(&g->epoch, 0);
	g->counter = 1;
	g->absent = 1;
	gens = g;
}

/* The later of a and b (invalid: none). */
static FullTransactionId
later(FullTransactionId a, FullTransactionId b)
{
	if (!FullTransactionIdIsValid(a) ||
		(FullTransactionIdIsValid(b) && FullTransactionIdFollows(b, a)))
		return b;
	return a;
}

/* Remember a writer that ended (the lock held exclusively). */
static void
note_ended(EndedWriters *ew, FullTransactionId xid)
{
	if (!FullTransactionIdIsValid(xid))
		return;
	if (ew->nlast == ENDED_WRITERS)
	{
		ew->before = later(ew->before, ew->last[0]);
		memmove(&ew->last[0], &ew->last[1],
				sizeof(FullTransactionId) * (ENDED_WRITERS - 1));
		ew->nlast--;
	}
	ew->last[ew->nlast++] = xid;
}

/* The latest of a branch's writers that ended. */
static FullTransactionId
latest_ended(const EndedWriters *ew)
{
	FullTransactionId latest = ew->before;

	for (int i = 0; i < ew->nlast; i++)
		latest = later(latest, ew->last[i]);
	return latest;
}

/*
 * Does snap see xid, if it committed?  One that precedes its xmin is seen
 * whatever it did.
 */
static bool
sees(Snapshot snap, FullTransactionId xmin, FullTransactionId xid)
{
	if (!FullTransactionIdIsValid(xid) || FullTransactionIdPrecedes(xid, xmin))
		return true;
	return !TransactionIdDidCommit(XidFromFullTransactionId(xid)) ||
		!XidInMVCCSnapshot(XidFromFullTransactionId(xid), snap);
}

/*
 * ... and every writer up to latest, the latest of those no longer
 * remembered one by one?  Only if it precedes the xmin: they all do then.
 */
static bool
sees_all_to(FullTransactionId xmin, FullTransactionId latest)
{
	return !FullTransactionIdIsValid(latest) ||
		FullTransactionIdPrecedes(latest, xmin);
}

/*
 * Does snap see every writer of branch e (NULL: one without an entry) that
 * committed, and every overflow writer?  (The lock held.)
 */
static bool
sees_writers(BranchGenerations *g, BranchGeneration *e, Snapshot snap)
{
	FullTransactionId xmin = FullTransactionIdFromAllowableAt(ReadNextFullTransactionId(),
															  snap->xmin);

	if (!sees_all_to(xmin, g->overflow_ended))
		return false;
	if (e == NULL)
		return sees_all_to(xmin, g->absent_ended);
	if (!sees_all_to(xmin, e->ended.before))
		return false;
	for (int i = 0; i < e->ended.nlast; i++)
	{
		if (!sees(snap, xmin, e->ended.last[i]))
			return false;
	}
	return true;
}

/* The database's entry for branch (the lock held), or NULL. */
static BranchGeneration *
find_entry(BranchGenerations *g, Oid branch)
{
	for (int i = 0; i < BRANCH_GENERATIONS; i++)
	{
		BranchGeneration *e = &g->entries[i];

		if (e->branch == branch && e->database == MyDatabaseId)
			return e;
	}
	return NULL;
}

/*
 * A new entry for branch (the lock held exclusively), a free one or one
 * nobody is writing, its branch absent from then on; NULL if every one is
 * being written.  It starts with the writers every absent branch has.
 */
static BranchGeneration *
make_entry(BranchGenerations *g, Oid branch, uint64 generation)
{
	for (int n = 0; n < BRANCH_GENERATIONS; n++)
	{
		BranchGeneration *e = &g->entries[g->hand];

		g->hand = (g->hand + 1) % BRANCH_GENERATIONS;
		if (OidIsValid(e->branch) && e->writers > 0)
			continue;
		if (OidIsValid(e->branch))
		{
			g->absent = ++g->counter;
			g->absent_ended = later(g->absent_ended, latest_ended(&e->ended));
		}
		memset(e, 0, sizeof(*e));
		e->database = MyDatabaseId;
		e->branch = branch;
		e->generation = generation;
		e->ended.before = g->absent_ended;
		e->rels_from = generation;
		return e;
	}
	return NULL;
}

/*
 * A relation written by e's writer that just ended, at e's generation now
 * (the lock held exclusively): the oldest dropped if the ring is full,
 * every relation then known only from after its generation.
 */
static void
note_relation_ended(BranchGeneration *e, Oid relid)
{
	if (e->nrels == WRITTEN_RELATIONS)
		e->rels_from = Max(e->rels_from, e->rels[e->relnext].generation);
	else
		e->nrels++;
	e->rels[e->relnext].generation = e->generation;
	e->rels[e->relnext].relid = relid;
	e->relnext = (e->relnext + 1) % WRITTEN_RELATIONS;
}

/*
 * At the transaction's end (commit, abort or prepare), once others see it
 * and its catalog changes are sent: what it wrote moves on.  (Not before:
 * a fold in between would still find it writing.)
 */
static void
end_writes(ResourceReleasePhase phase, bool isCommit, bool isTopLevel,
		   void *arg)
{
	BranchGenerations *g = shared();
	FullTransactionId xid;

	if (phase != RESOURCE_RELEASE_AFTER_LOCKS || !isTopLevel ||
		IsTransactionState() || (nwritten == 0 && !written_overflow))
		return;
	xid = GetTopFullTransactionIdIfAny();

	/* (others see the transaction, but not yet what it wrote) */
	INJECTION_POINT("prolly-writes-ending", NULL);
	LWLockAcquire(&g->lock, LW_EXCLUSIVE);
	for (int i = 0; i < nwritten; i++)
	{
		BranchGeneration *e = find_entry(g, written[i]);

		/* (an entry being written is never evicted) */
		Assert(e != NULL && e->writers > 0);
		if (e == NULL)
			continue;
		e->writers--;
		e->generation = ++g->counter;
		e->known = false;
		note_ended(&e->ended, xid);
		if (written_rels_overflow)
			e->rels_from = e->generation;
		for (int j = 0; j < nwritten_rels; j++)
		{
			if (written_rels[j].branch == written[i])
				note_relation_ended(e, written_rels[j].relid);
		}
	}
	if (written_overflow)
	{
		g->overflow_writers--;
		g->overflow = ++g->counter;
		g->overflow_ended = later(g->overflow_ended, xid);
	}
	LWLockRelease(&g->lock);
	nwritten = 0;
	written_overflow = false;
	nwritten_rels = 0;
	written_rels_overflow = false;
}

void
branch_generations_note_write(Oid branch)
{
	BranchGenerations *g = shared();
	BranchGeneration *e;

	/* (an overflow writer writes every branch already) */
	if (g == NULL || !OidIsValid(branch) || written_overflow)
		return;
	for (int i = 0; i < nwritten; i++)
	{
		if (written[i] == branch)
			return;
	}

	/* (a write missed, to show the check of debug_prolly_verify_fold_caches) */
	if (IS_INJECTION_POINT_ATTACHED("prolly-branch-write-unnoted"))
		return;
	if (!callback_registered)
	{
		RegisterResourceReleaseCallback(end_writes, NULL);
		callback_registered = true;
	}
	LWLockAcquire(&g->lock, LW_EXCLUSIVE);
	e = find_entry(g, branch);
	if (e == NULL && nwritten < MAX_WRITTEN)
		e = make_entry(g, branch, ++g->counter);
	if (e != NULL && nwritten < MAX_WRITTEN)
	{
		e->writers++;
		written[nwritten++] = branch;
	}
	else
	{
		g->overflow_writers++;
		written_overflow = true;
	}
	LWLockRelease(&g->lock);
}

/*
 * Of a relation whose rows the transaction writes on branch (told of it
 * already): kept for the branch's entry as the transaction ends.  Called
 * for every row, so the last one is checked first.
 */
void
branch_generations_note_relation(Oid branch, Oid relid)
{
	if (!OidIsValid(branch) || !OidIsValid(relid) || written_rels_overflow ||
		written_overflow)
		return;
	if (nwritten_rels > 0 &&
		written_rels[nwritten_rels - 1].relid == relid &&
		written_rels[nwritten_rels - 1].branch == branch)
		return;
	for (int i = 0; i < nwritten_rels; i++)
	{
		if (written_rels[i].relid == relid && written_rels[i].branch == branch)
			return;
	}
	if (nwritten_rels == MAX_WRITTEN_RELS)
	{
		written_rels_overflow = true;
		return;
	}
	written_rels[nwritten_rels].branch = branch;
	written_rels[nwritten_rels].relid = relid;
	nwritten_rels++;
}

/*
 * Can a fold by snap be marked, or what was written since a mark be known?
 * As knowable(), but by a transaction that writes the branch too: its own
 * relations are known here.
 */
static bool
markable(Snapshot snap)
{
	return shared() != NULL && snap != NULL &&
		snap->snapshot_type == SNAPSHOT_MVCC && !RecoveryInProgress() &&
		!written_overflow && !written_rels_overflow &&
		!XactHasInvalidations() && !TwoPhaseAnyPrepared();
}

bool
branch_fold_mark(Oid branch, Snapshot snap, ProllyFoldMark *mark)
{
	BranchGenerations *g = shared();
	BranchGeneration *e;
	bool		ok;

	if (!markable(snap))
		return false;
	LWLockAcquire(&g->lock, LW_SHARED);
	e = find_entry(g, branch);
	ok = e != NULL && sees_writers(g, e, snap);
	if (ok)
	{
		mark->branch = branch;
		mark->generation = e->generation;
		mark->overflow = g->overflow;
		mark->epoch = pg_atomic_read_u64(&g->epoch);
	}
	LWLockRelease(&g->lock);
	return ok;
}

bool
branch_written_since(const ProllyFoldMark *mark, Snapshot snap, Oid **relids,
					 int *nrelids)
{
	BranchGenerations *g = shared();
	BranchGeneration *e;
	Oid		   *out;
	int			n = 0;
	int			own = 0;
	bool		ok;

	if (!markable(snap))
		return false;
	for (int i = 0; i < nwritten; i++)
	{
		if (written[i] == mark->branch)
			own = 1;
	}
	out = palloc_array(Oid, WRITTEN_RELATIONS + MAX_WRITTEN_RELS);
	LWLockAcquire(&g->lock, LW_SHARED);
	e = find_entry(g, mark->branch);

	/*
	 * (nobody else writing it, nor any overflow writer: a writer is seen
	 * committed before it ends here, and only then are its relations
	 * kept, so one still writing may be one the snapshot sees)
	 */
	ok = e != NULL && e->rels_from <= mark->generation &&
		e->writers == own && g->overflow_writers == 0 &&
		g->overflow == mark->overflow &&
		pg_atomic_read_u64(&g->epoch) == mark->epoch &&
		sees_writers(g, e, snap);
	for (int i = 0; ok && i < e->nrels; i++)
	{
		if (e->rels[i].generation > mark->generation)
			out[n++] = e->rels[i].relid;
	}
	LWLockRelease(&g->lock);
	if (!ok)
	{
		pfree(out);
		return false;
	}
	for (int i = 0; i < nwritten_rels; i++)
	{
		if (written_rels[i].branch == mark->branch)
			out[n++] = written_rels[i].relid;
	}
	*relids = out;
	*nrelids = n;
	return true;
}

/*
 * A prepared transaction committed or rolled back, once others see it: it
 * may have written any branch, as an overflow writer does.  One prepared
 * by this server was noted as it was (end_writes()), but one recovered at
 * startup was never told of, so a snapshot that does not see its commit
 * must not have its fold taken.  (No error, no allocation: interrupts are
 * held.)
 */
void
branch_generations_prepared_ended(FullTransactionId xid)
{
	BranchGenerations *g = shared();

	if (g == NULL)
		return;
	LWLockAcquire(&g->lock, LW_EXCLUSIVE);
	g->overflow = ++g->counter;
	g->overflow_ended = later(g->overflow_ended, xid);
	LWLockRelease(&g->lock);
}

/* (no error, no allocation: in critical sections too) */
void
branch_generations_forget(void)
{
	BranchGenerations *g = shared();

	if (g != NULL)
		pg_atomic_fetch_add_u64(&g->epoch, 1);
}

/*
 * Can anything be known here, of branch, by snap?  Not in recovery, nor by
 * a transaction that writes it (or found no entry) or changed a catalog,
 * nor while a transaction is prepared; only by an MVCC snapshot.
 */
static bool
knowable(Oid branch, Snapshot snap)
{
	if (shared() == NULL || snap == NULL ||
		snap->snapshot_type != SNAPSHOT_MVCC || RecoveryInProgress() ||
		written_overflow || XactHasInvalidations() || TwoPhaseAnyPrepared())
		return false;
	for (int i = 0; i < nwritten; i++)
	{
		if (written[i] == branch)
			return false;
	}
	return true;
}

static bool
same_hash(const ProllyHash *a, const ProllyHash *b)
{
	return a->len == b->len && memcmp(a->bytes, b->bytes, a->len) == 0;
}

/*
 * Before the fold reads anything, and so, after it, the catalogs as of the
 * epoch taken: their changes it counts are sent, so taken in now.
 */
void
branch_fold_stamp(Oid branch, BranchFoldStamp *stamp)
{
	BranchGenerations *g = shared();
	BranchGeneration *e;

	memset(stamp, 0, sizeof(*stamp));
	stamp->branch = branch;
	if (g == NULL)
		return;
	INJECTION_POINT("prolly-fold-stamp", NULL);
	LWLockAcquire(&g->lock, LW_SHARED);
	e = find_entry(g, branch);
	stamp->generation = e ? e->generation : g->absent;
	stamp->overflow = g->overflow;
	stamp->epoch = pg_atomic_read_u64(&g->epoch);
	LWLockRelease(&g->lock);
	AcceptInvalidationMessages();
	INJECTION_POINT("prolly-fold-stamped", NULL);
}

/*
 * Make a fold the transaction published known, as it commits (the lock
 * held exclusively): if nothing ended since its stamp, nobody writes, and
 * the epoch stayed.  (Its snapshot saw every writer that ended before it
 * was published, and none has since.)
 */
static void
make_known(BranchGenerations *g, const PendingFold *pf)
{
	BranchGeneration *e = find_entry(g, pf->branch);

	if ((e ? e->generation : g->absent) != pf->generation ||
		(e != NULL && e->writers > 0) ||
		g->overflow != pf->overflow || g->overflow_writers > 0 ||
		pg_atomic_read_u64(&g->epoch) != pf->epoch ||
		(e == NULL && (e = make_entry(g, pf->branch, pf->generation)) == NULL))
		return;
	e->known = true;
	e->complete = pf->complete;
	e->epoch = pf->epoch;
	e->overflow = pf->overflow;
	e->baseline = pf->baseline;
	e->root = pf->root;
}

/*
 * The transaction's folds published, made known as it commits; forgotten
 * if it aborts or is prepared, and with a subtransaction that aborts (whose
 * chunks are).
 */
static void
publish_xact(XactEvent event, void *arg)
{
	BranchGenerations *g = shared();

	if (pending == NIL)
		return;
	if (event == XACT_EVENT_COMMIT && nwritten == 0 && !written_overflow &&
		!XactHasInvalidations() && !TwoPhaseAnyPrepared())
	{
		LWLockAcquire(&g->lock, LW_EXCLUSIVE);
		foreach_ptr(PendingFold, pf, pending)
			make_known(g, pf);
		LWLockRelease(&g->lock);
	}
	if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_ABORT ||
		event == XACT_EVENT_PREPARE || event == XACT_EVENT_PARALLEL_COMMIT ||
		event == XACT_EVENT_PARALLEL_ABORT)
		pending = NIL;			/* (in the transaction's memory) */
}

static void
publish_subxact(SubXactEvent event, SubTransactionId mySubid,
				SubTransactionId parentSubid, void *arg)
{
	int			level = GetCurrentTransactionNestLevel();

	foreach_ptr(PendingFold, pf, pending)
	{
		if (pf->level < level)
			continue;
		if (event == SUBXACT_EVENT_ABORT_SUB)
			pending = foreach_delete_current(pending, pf);
		else if (event == SUBXACT_EVENT_COMMIT_SUB)
			pf->level = level - 1;
	}
}

/*
 * Published now, made known when the transaction commits: its tree's
 * chunks are others' only then.
 */
void
branch_fold_publish(const BranchFoldStamp *stamp, Snapshot snap,
					const ProllyHash *baseline, const ProllyHash *root,
					bool complete)
{
	BranchGenerations *g = shared();
	BranchGeneration *e;
	bool		valid;

	if (!knowable(stamp->branch, snap))
		return;
	LWLockAcquire(&g->lock, LW_SHARED);
	e = find_entry(g, stamp->branch);

	/* (nothing ended, nobody writing, since the stamp; every one seen) */
	valid = (e ? e->generation : g->absent) == stamp->generation &&
		(e == NULL || e->writers == 0) &&
		g->overflow == stamp->overflow && g->overflow_writers == 0 &&
		pg_atomic_read_u64(&g->epoch) == stamp->epoch &&
		sees_writers(g, e, snap);
	LWLockRelease(&g->lock);
	if (valid)
	{
		MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);
		PendingFold *pf = palloc0(sizeof(PendingFold));

		if (!publish_callbacks)
		{
			RegisterXactCallback(publish_xact, NULL);
			RegisterSubXactCallback(publish_subxact, NULL);
			publish_callbacks = true;
		}
		pf->branch = stamp->branch;
		pf->generation = stamp->generation;
		pf->overflow = stamp->overflow;
		pf->epoch = stamp->epoch;
		pf->complete = complete;
		if (baseline)
			pf->baseline = *baseline;
		pf->root = *root;
		pf->level = GetCurrentTransactionNestLevel();
		pending = lappend(pending, pf);
		MemoryContextSwitchTo(old);
	}
}

bool
branch_fold_known(Oid branch, Snapshot snap, const ProllyHash *baseline,
				  bool complete, ProllyHash *root)
{
	return branch_fold_known_at(branch, snap, baseline, complete, root, NULL);
}

bool
branch_fold_known_at(Oid branch, Snapshot snap, const ProllyHash *baseline,
					 bool complete, ProllyHash *root, BranchFoldStamp *at)
{
	BranchGenerations *g = shared();
	BranchGeneration *e;
	ProllyHash	none = {0};
	bool		found = false;

	if (!knowable(branch, snap))
		return false;
	LWLockAcquire(&g->lock, LW_SHARED);
	e = find_entry(g, branch);
	if (e != NULL && e->known && (e->complete || !complete) &&
		e->writers == 0 && g->overflow_writers == 0 &&
		e->overflow == g->overflow &&
		e->epoch == pg_atomic_read_u64(&g->epoch) &&
		same_hash(&e->baseline, baseline ? baseline : &none) &&
		sees_writers(g, e, snap))
	{
		*root = e->root;
		if (at != NULL)
		{
			at->branch = branch;
			at->generation = e->generation;
			at->overflow = g->overflow;
			at->epoch = e->epoch;
		}
		found = true;
	}
	LWLockRelease(&g->lock);
	return found;
}

/*
 * Is what was known as at still the branch's working set: nothing ended
 * and nobody writes it since, the epoch stayed?  Its snapshot saw every
 * writer that had ended then, and none has since.  Asked without
 * knowable(): the transaction's own catalog changes since (they are sent
 * only as it commits) leave it as true as it was, and a write of the
 * branch by the transaction is a writer of it.
 */
bool
branch_fold_unchanged(const BranchFoldStamp *at)
{
	BranchGenerations *g = shared();
	BranchGeneration *e;
	bool		unchanged;

	if (g == NULL || RecoveryInProgress())
		return false;
	LWLockAcquire(&g->lock, LW_SHARED);
	e = find_entry(g, at->branch);
	unchanged = e != NULL && e->known && e->generation == at->generation &&
		e->writers == 0 && g->overflow == at->overflow &&
		g->overflow_writers == 0 && e->overflow == g->overflow &&
		e->epoch == at->epoch &&
		pg_atomic_read_u64(&g->epoch) == at->epoch;
	LWLockRelease(&g->lock);
	return unchanged;
}

/*
 * The fold worker's view (prolly_folder.c): the databases with a branch
 * whose generation moved since the worker last saw it, into databases (at
 * most max), and their count.  Without shared memory, none.
 */
int
branch_generations_moved_databases(Oid *databases, int max)
{
	BranchGenerations *g = shared();
	int			n = 0;

	if (g == NULL)
		return 0;
	LWLockAcquire(&g->lock, LW_SHARED);
	for (int i = 0; i < BRANCH_GENERATIONS && n < max; i++)
	{
		BranchGeneration *e = &g->entries[i];
		bool		listed = false;

		if (!OidIsValid(e->branch) || e->generation == e->seen)
			continue;
		for (int j = 0; j < n && !listed; j++)
			listed = databases[j] == e->database;
		if (!listed)
			databases[n++] = e->database;
	}
	LWLockRelease(&g->lock);
	return n;
}

/*
 * The current database's branches whose generation moved since the fold
 * worker last saw it, each with its generation now, into branches and
 * generations (at most max); their count.
 */
int
branch_generations_moved(Oid *branches, uint64 *generations, int max)
{
	BranchGenerations *g = shared();
	int			n = 0;

	if (g == NULL)
		return 0;
	LWLockAcquire(&g->lock, LW_SHARED);
	for (int i = 0; i < BRANCH_GENERATIONS && n < max; i++)
	{
		BranchGeneration *e = &g->entries[i];

		if (!OidIsValid(e->branch) || e->database != MyDatabaseId ||
			e->generation == e->seen)
			continue;
		branches[n] = e->branch;
		generations[n] = e->generation;
		n++;
	}
	LWLockRelease(&g->lock);
	return n;
}

/*
 * The fold worker can't fold database's branches (gone, or accepting no
 * connections): each seen as it is, moved again only once written again.
 */
void
branch_generations_saw_database(Oid database)
{
	BranchGenerations *g = shared();

	if (g == NULL)
		return;
	LWLockAcquire(&g->lock, LW_EXCLUSIVE);
	for (int i = 0; i < BRANCH_GENERATIONS; i++)
	{
		BranchGeneration *e = &g->entries[i];

		if (OidIsValid(e->branch) && e->database == database)
			e->seen = e->generation;
	}
	LWLockRelease(&g->lock);
}

/* The fold worker saw branch at generation (no later one: kept as moved). */
void
branch_generations_saw(Oid branch, uint64 generation)
{
	BranchGenerations *g = shared();
	BranchGeneration *e;

	if (g == NULL)
		return;
	LWLockAcquire(&g->lock, LW_EXCLUSIVE);
	e = find_entry(g, branch);
	if (e != NULL && e->seen < generation && generation <= e->generation)
		e->seen = generation;
	LWLockRelease(&g->lock);
}
