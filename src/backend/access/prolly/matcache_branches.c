/*-------------------------------------------------------------------------
 *
 * matcache_branches.c
 *	  Materialization cache "branches": kept materializations are the hidden
 *	  branches of detached checkouts, kept after their sessions leave, and
 *	  the commits branches keep their pages as of.
 *
 * A detached checkout of a commit is a session on a hidden branch at the
 * commit (PROLLY_DETACHED_PREFIX and its hash), its copies made by its
 * first use: the commit's tables as heap pages and native indexes.  Its
 * head never moves and no transaction writes its tables (its sessions'
 * transactions are read-only, no connection names it, and no push,
 * rename or move changes it), so once made its working set is its
 * commit's root for as long as it exists, across restarts too.  Such a
 * branch is kept when no session is on it any more, up to
 * prolly.kept_checkouts of them per database: a later checkout of the
 * commit is a checkout of the branch, and a move or first use elsewhere
 * can give a table the storage of a kept branch that has its target's.
 *
 * A branch's pages are kept as of the last commits it was at, as the refs
 * record (their kept_commits()): those are kept states too, a table's
 * relations by roots of their storage, as immutable as a kept
 * checkout's however the branch moves on.  Which commits keep them is
 * the root retention's.
 *
 * The checkout sweep lets go of the kept branches past the budget, least
 * recently checked out first.  Recency is kept in shared memory, a slot
 * for each of the last KEPT_USES branches checked out; one not known (since
 * a restart, or pushed out) is older than every one known, and of those
 * the older branch (its smaller OID) goes first.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/matcache_branches.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/prolly_builtins.h"
#include "access/prolly_deps.h"
#include "access/prolly_shmem.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/branch.h"
#include "catalog/branch_ops.h"
#include "catalog/pg_branch.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/lwlock.h"
#include "utils/fmgroids.h"
#include "utils/injection_point.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

int			prolly_kept_checkouts = 4;

/* the branches whose last checkout is remembered, at most */
#define KEPT_USES	64

typedef struct KeptUse
{
	Oid			database;
	Oid			branch;
	uint64		used;			/* when, by the clock (0: a free slot) */
} KeptUse;

typedef struct KeptUses
{
	LWLock		lock;
	uint64		clock;
	KeptUse		uses[KEPT_USES];
} KeptUses;

static KeptUses *kept_uses = NULL;

static KeptUses *
shared(void)
{
	if (kept_uses == NULL)
		kept_uses = prolly_shmem_area(&prolly_matcache_branches.info);
	return kept_uses;
}

static Size
branches_shmem_size(void)
{
	return sizeof(KeptUses);
}

static void
branches_shmem_init(void *area)
{
	KeptUses   *k = area;

	memset(k, 0, sizeof(*k));
	LWLockInitialize(&k->lock, LWLockNewTrancheId("prolly kept checkouts"));
	kept_uses = k;
}

static bool
detached_name(const char *name)
{
	return strncmp(name, PROLLY_DETACHED_PREFIX,
				   strlen(PROLLY_DETACHED_PREFIX)) == 0;
}

/* A hidden branch with copies (one with only its head holds nothing). */
static bool
branches_kept(const char *name)
{
	return detached_name(name) && strlen(name) < NAMEDATALEN &&
		SearchSysCacheExists1(BRANCHNAME, CStringGetDatum(name));
}

/* Does snap see branch's pg_branch row, made with its copies? */
static bool
made_seen(Oid branch, Snapshot snap)
{
	Relation	brel = table_open(BranchRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	bool		seen;

	ScanKeyInit(&key, Anum_pg_branch_oid, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(branch));
	/* (registered for the scan: a caller's may be one it hasn't, as the latest) */
	snap = RegisterSnapshot(snap);
	scan = systable_beginscan(brel, BranchOidIndexId, true, snap, 1, &key);
	seen = HeapTupleIsValid(systable_getnext(scan));
	systable_endscan(scan);
	UnregisterSnapshot(snap);
	table_close(brel, AccessShareLock);
	return seen;
}

/* The kept checkouts found, visited after the scan of their names. */
static void
note_kept(const char *name, Oid branch, void *arg)
{
	List	  **states = arg;
	ProllyKeptState *s;

	s = palloc0(sizeof(ProllyKeptState));
	s->kind = PROLLY_KEPT_CHECKOUT;
	s->name = pstrdup(name);
	s->branch = branch;
	*states = lappend(*states, s);
}

/* Does snap see the transaction that kept a commit's pages committed? */
static bool
kept_seen(const ProllyKeptCommit *kc, Snapshot snap)
{
	TransactionId xid = XidFromFullTransactionId(kc->kept_by);

	/*
	 * (the refs show a keeping that committed, or this transaction's own;
	 * so seen unless the snapshot was taken while it ran)
	 */
	if (!TransactionIdIsNormal(xid))
		return false;
	return TransactionIdIsCurrentTransactionId(xid) ||
		!XidInMVCCSnapshot(xid, snap);
}

/* A branch's kept commits, each as a kept state. */
typedef struct KeptCommits
{
	const ProllyKeptState *of;	/* its branch */
	Snapshot	snap;
	List	   *states;
} KeptCommits;

static void
note_kept_commit(const ProllyKeptCommit *kc, void *arg)
{
	KeptCommits *k = arg;
	ProllyKeptState *s;
	ProllyKeptCommit *copy;
	ProllyKeptTable *tables;

	if (!kept_seen(kc, k->snap))
		return;
	copy = palloc(sizeof(ProllyKeptCommit));
	*copy = *kc;
	tables = palloc_array(ProllyKeptTable, Max(kc->ntables, 1));
	for (int t = 0; t < kc->ntables; t++)
	{
		BranchKeptRelation *rels = palloc_array(BranchKeptRelation,
												kc->tables[t].nrels);

		memcpy(rels, kc->tables[t].rels,
			   sizeof(BranchKeptRelation) * kc->tables[t].nrels);
		tables[t].name = pstrdup(kc->tables[t].name);
		tables[t].nrels = kc->tables[t].nrels;
		tables[t].rels = rels;
	}
	copy->tables = tables;
	s = palloc0(sizeof(ProllyKeptState));
	*s = *k->of;
	s->commit = kc->commit;
	s->kept = copy;
	k->states = lappend(k->states, s);
}

/* A branch keeping pages as of commits the filter asks for. */
typedef struct KeptHolder
{
	char	   *name;
	List	   *commits;		/* ProllyHash * */
} KeptHolder;

static void
note_holder(const char *name, const ProllyHash *commit, void *arg)
{
	List	  **holders = arg;
	ProllyHash *c = palloc(sizeof(ProllyHash));
	KeptHolder *h;

	*c = *commit;
	foreach_ptr(KeptHolder, x, *holders)
	{
		if (strcmp(x->name, name) == 0)
		{
			x->commits = lappend(x->commits, c);
			return;
		}
	}
	h = palloc(sizeof(KeptHolder));
	h->name = pstrdup(name);
	h->commits = list_make1(c);
	*holders = lappend(*holders, h);
}

static int
holder_cmp(const ListCell *a, const ListCell *b)
{
	return strcmp(((const KeptHolder *) lfirst(a))->name,
				  ((const KeptHolder *) lfirst(b))->name);
}

static bool
hash_equal(const ProllyHash *a, const ProllyHash *b)
{
	return a->len == b->len && memcmp(a->bytes, b->bytes, a->len) == 0;
}

static bool
hash_listed(List *hashes, const ProllyHash *h)
{
	foreach_ptr(ProllyHash, x, hashes)
		if (hash_equal(x, h))
			return true;
	return false;
}

/*
 * The kept branches the snapshot sees made (the hidden branches' prefix
 * of the names alone), then the kept commits the filter asks for (an
 * index's, in the refs) whose keeping it sees committed (every branch's
 * but the kept branches', whose own pages serve as the branch), whose
 * commits' ordering dependencies match the installed extensions: each
 * with its commit and that commit's root.  (Visited once the branches are
 * found: a visit may read catalogs of its own.)
 */
static void
branches_visit(Snapshot snap, const ProllyKeptFilter *filter,
			   ProllyKeptVisit visit, void *arg)
{
	ProllyDatabaseComponents dc;
	List	   *checkouts = NIL;
	List	   *commits = NIL;
	List	   *holders = NIL;

	prolly_database_components(&dc);
	branch_ops()->branches_named(PROLLY_DETACHED_PREFIX, note_kept, &checkouts);
	foreach_ptr(ProllyKeptState, s, checkouts)
	{
		if (s->branch == MyBranch || !made_seen(s->branch, snap) ||
			!PROLLY_CALL(dc.refs, resolve, s->name, &s->commit) ||
			(filter->commit != NULL && !hash_equal(&s->commit, filter->commit)))
			continue;
		commits = lappend(commits, s);
	}
	if (PROLLY_HAS(dc.refs, kept_commit) && PROLLY_HAS(dc.refs, kept_holders) &&
		debug_prolly_page_roots)
	{
		if (filter->commit != NULL)
			PROLLY_CALL(dc.refs, kept_holders, filter->commit, NULL,
						note_holder, &holders);
		for (int t = 0; t < filter->ntables; t++)
			PROLLY_CALL(dc.refs, kept_holders, NULL, &filter->tables[t],
						note_holder, &holders);
	}
	/* (in the branches' name order, as a listing of them would have them) */
	list_sort(holders, holder_cmp);
	foreach_ptr(KeptHolder, h, holders)
	{
		const char *name = h->name;
		ProllyKeptState of = {PROLLY_KEPT_COMMIT, name, InvalidOid};
		KeptCommits k = {&of, snap, NIL};
		HeapTuple	tup;

		/* (a branch with copies, not a kept one: its own pages serve as it) */
		if (detached_name(name) || strlen(name) >= NAMEDATALEN)
			continue;
		tup = SearchSysCache1(BRANCHNAME, CStringGetDatum(name));
		if (!HeapTupleIsValid(tup))
			continue;
		of.branch = ((Form_pg_branch) GETSTRUCT(tup))->oid;
		ReleaseSysCache(tup);
		/* (each once, a seek: a branch may keep thousands) */
		{
			List	   *asked = NIL;

			foreach_ptr(ProllyHash, c, h->commits)
			{
				if (hash_listed(asked, c))
					continue;
				asked = lappend(asked, c);
				PROLLY_CALL(dc.refs, kept_commit, name, c, note_kept_commit,
							&k);
			}
		}
		foreach_ptr(ProllyKeptState, s, k.states)
			commits = lappend(commits, s);
	}
	foreach_ptr(ProllyKeptState, s, commits)
	{
		ProllyCommitInfo info;

		if (!PROLLY_CALL(dc.refs, commit_get, &s->commit, &info) ||
			!prolly_ordering_dependencies_match(&info))
			continue;
		s->root = info.root;
		visit(s, arg);
	}
}

/* Remember that the branch called name was checked out now. */
static void
branches_note_use(const char *name)
{
	KeptUses   *k = shared();
	HeapTuple	tup;
	Oid			branch;
	KeptUse    *slot = NULL;

	if (k == NULL || !detached_name(name) || strlen(name) >= NAMEDATALEN)
		return;
	tup = SearchSysCache1(BRANCHNAME, CStringGetDatum(name));
	if (!HeapTupleIsValid(tup))
		return;
	branch = ((Form_pg_branch) GETSTRUCT(tup))->oid;
	ReleaseSysCache(tup);

	LWLockAcquire(&k->lock, LW_EXCLUSIVE);
	for (int i = 0; i < KEPT_USES; i++)
	{
		KeptUse    *u = &k->uses[i];

		if (u->database == MyDatabaseId && u->branch == branch)
		{
			slot = u;
			break;
		}
		if (slot == NULL || u->used < slot->used)
			slot = u;
	}
	slot->database = MyDatabaseId;
	slot->branch = branch;
	slot->used = ++k->clock;
	LWLockRelease(&k->lock);
}

/* A kept branch, for eviction's order. */
typedef struct KeptByUse
{
	char	   *name;
	Oid			branch;
	uint64		used;
} KeptByUse;

static int
by_use(const void *a, const void *b)
{
	const KeptByUse *x = a;
	const KeptByUse *y = b;

	if (x->used != y->used)
		return x->used < y->used ? -1 : 1;
	if (x->branch != y->branch)
		return x->branch < y->branch ? -1 : 1;
	return 0;
}

/*
 * Let go of the idle kept branches among names past the budget, the least
 * recently checked out first: one a session is on now isn't idle, and is
 * neither counted nor let go of.  Each drop() leaves one fewer, whether it
 * deleted the branch or found another sweep had (a concurrent sweep
 * doesn't take the next one too); one it can't delete (a session came to
 * it) is passed over for the next.
 */
static void
branches_evict(List *names, bool (*drop) (const char *name, void *arg),
			   void *arg)
{
	KeptUses   *k = shared();
	KeptByUse  *kept = palloc_array(KeptByUse, Max(list_length(names), 1));
	int			n = 0;
	int			left;

	foreach_ptr(char, name, names)
	{
		HeapTuple	tup;

		if (!detached_name(name) || strlen(name) >= NAMEDATALEN)
			continue;
		tup = SearchSysCache1(BRANCHNAME, CStringGetDatum(name));
		if (!HeapTupleIsValid(tup))
			continue;
		kept[n].name = name;
		kept[n].branch = ((Form_pg_branch) GETSTRUCT(tup))->oid;
		kept[n].used = 0;
		ReleaseSysCache(tup);
		/* (one a session is on isn't idle: not counted, nor let go of) */
		if (!branch_in_use(kept[n].branch))
			n++;
	}
	if (n <= prolly_kept_checkouts)
		return;
	if (k != NULL)
	{
		LWLockAcquire(&k->lock, LW_SHARED);
		for (int i = 0; i < n; i++)
			for (int j = 0; j < KEPT_USES; j++)
				if (k->uses[j].database == MyDatabaseId &&
					k->uses[j].branch == kept[i].branch)
					kept[i].used = k->uses[j].used;
		LWLockRelease(&k->lock);
	}
	qsort(kept, n, sizeof(KeptByUse), by_use);
	INJECTION_POINT("prolly-kept-evict", NULL);
	left = n;
	for (int i = 0; i < n && left > prolly_kept_checkouts; i++)
	{
		if (drop(kept[i].name, arg))
			left--;
	}
	pfree(kept);
}

const ProllyMatCacheRoutine prolly_matcache_branches = {
	.info = {
		.iface = PROLLY_IFACE_MATERIALIZATION_CACHE,
		.impl_id = 1,
		.format_version = 1,
		.name = "branches",
		.description = "detached checkouts' hidden branches, kept read-only after their sessions leave, and branches' kept commits",
	},
	.kept = branches_kept,
	.visit = branches_visit,
	.note_use = branches_note_use,
	.evict = branches_evict,
	.shmem_size = branches_shmem_size,
	.shmem_init = branches_shmem_init,
};
