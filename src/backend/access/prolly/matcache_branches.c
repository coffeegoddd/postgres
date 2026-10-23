/*-------------------------------------------------------------------------
 *
 * matcache_branches.c
 *	  Materialization cache "branches": kept materializations are the hidden
 *	  branches of detached checkouts, kept after their sessions leave.
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
#include "catalog/branch.h"
#include "catalog/branch_ops.h"
#include "catalog/pg_branch.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/lwlock.h"
#include "utils/fmgroids.h"
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
	scan = systable_beginscan(brel, BranchOidIndexId, true, snap, 1, &key);
	seen = HeapTupleIsValid(systable_getnext(scan));
	systable_endscan(scan);
	table_close(brel, AccessShareLock);
	return seen;
}

typedef struct VisitKept
{
	Snapshot	snap;
	ProllyKeptVisit visit;
	void	   *arg;
	List	   *states;			/* ProllyKeptState *, visited after the scan */
} VisitKept;

static void
note_kept(const char *name, Oid branch, void *arg)
{
	VisitKept  *vk = arg;
	ProllyKeptState *s;

	if (!detached_name(name))
		return;
	s = palloc0(sizeof(ProllyKeptState));
	s->name = pstrdup(name);
	s->branch = branch;
	vk->states = lappend(vk->states, s);
}

/*
 * The kept branches the snapshot sees made, whose commits' ordering
 * dependencies match the installed extensions: each with its head and
 * that commit's root.  (Visited once the branches are listed: a visit may
 * read catalogs of its own.)
 */
static void
branches_visit(Snapshot snap, ProllyKeptVisit visit, void *arg)
{
	ProllyDatabaseComponents dc;
	VisitKept	vk = {snap, visit, arg, NIL};

	prolly_database_components(&dc);
	branch_ops()->branches(note_kept, &vk);
	foreach_ptr(ProllyKeptState, s, vk.states)
	{
		ProllyCommitInfo info;

		if (s->branch == MyBranch || !made_seen(s->branch, snap) ||
			!PROLLY_CALL(dc.refs, resolve, s->name, &s->commit) ||
			!PROLLY_CALL(dc.refs, commit_get, &s->commit, &info) ||
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
 * Let go of the kept branches among names past the budget, the least
 * recently checked out first: as many as drop() deletes, one it can't
 * (a session on it) passed over for the next.
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
	left = n;
	for (int i = 0; i < n && left > prolly_kept_checkouts; i++)
	{
		if (drop(kept[i].name, arg))
		{
			pgstat_count_prolly(PGSTAT_PROLLY_KEPT_CHECKOUTS_EVICTED, 1);
			left--;
		}
	}
	pfree(kept);
}

const ProllyMatCacheRoutine prolly_matcache_branches = {
	.info = {
		.iface = PROLLY_IFACE_MATERIALIZATION_CACHE,
		.impl_id = 1,
		.format_version = 1,
		.name = "branches",
		.description = "detached checkouts' hidden branches, kept read-only after their sessions leave",
	},
	.kept = branches_kept,
	.visit = branches_visit,
	.note_use = branches_note_use,
	.evict = branches_evict,
	.shmem_size = branches_shmem_size,
	.shmem_init = branches_shmem_init,
};
