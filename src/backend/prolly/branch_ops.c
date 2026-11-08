/*-------------------------------------------------------------------------
 *
 * branch_ops.c
 *	  A branch made, its working set sharing its parent's pages.
 *
 * A branch is made at the point its parent's barrier marks, with the parent's
 * relations held: each table and sequence of the parent's schemas is locked so
 * as to wait out the writers in progress and hold off new ones (a lock that
 * keeps writers out, which VACUUM's and REFRESH's wait for too), once this
 * transaction is left out of capture, as taking one gives it an id.  Every
 * tuple on the parent's pages is then of a transaction that has committed or
 * aborted, so the new branch, sharing them, shows exactly what the parent had
 * committed then, and the barrier's root is that too: the parent's working
 * set.  Catalog changes in the database wait until the transaction ends, as
 * they do for any commit from capture that's left out.
 *
 * The catalog rows are copied (prolly/branch_catalog.h), each relation's
 * storage sharing its source's copy-on-write, but a sequence's, whose page is
 * copied.  The new branch's tables then hold the parent's working set, and
 * what differs from what it's to hold is written on it as a move writes it
 * (prolly/move.h): a commit's rows, or the head's, leaving the parent's
 * changes on the parent; or with its changes taken, nothing, and the parent
 * is moved back to its head.  A schema the target lacks is dropped, and one it
 * has that isn't there is restored.  Capture is told each table's rows, as the
 * target has them, once all that's written is.  The branch of version control
 * is made at its commit, and linked to the catalog branch.  All in the
 * caller's transaction: an abort leaves none of it, the shared storage dropped
 * as any made in it is.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/prolly/branch_ops.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/pg_namespace.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "prolly/branch_catalog.h"
#include "prolly/branch_link.h"
#include "prolly/branch_ops.h"
#include "prolly/capture.h"
#include "prolly/chunkstore.h"
#include "prolly/engine/vc.h"
#include "prolly/engine/vc_refs.h"
#include "prolly/move.h"
#include "prolly/ws/ws_dbroot.h"
#include "prolly/ws/ws_restore.h"
#include "prolly/ws/ws_root.h"
#include "prolly/ws/ws_schema.h"
#include "prolly/ws/ws_sql.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

/* What the parent's writers are held off for: the branch, and how long a lock is waited for. */
typedef struct Holding
{
	int32		branch;
	int			timeout_ms;
	TimestampTz start;			/* when the holding began */
	TimestampTz round;			/* when this round of it began */
	int			user_timeout;	/* the session's own lock_timeout, or 0 */
} Holding;

/* (a TOAST table's values are named by its id, which the branch's copy of it doesn't have) */
static void
refuse_values_out_of_line(Oid table, Oid toast)
{
	Relation	rel = table_open(toast, NoLock);
	Snapshot	snapshot = RegisterSnapshot(GetLatestSnapshot());
	TableScanDesc scan = table_beginscan(rel, snapshot, 0, NULL, 0);
	TupleTableSlot *slot = table_slot_create(rel, NULL);
	bool		any = table_scan_getnextslot(scan, ForwardScanDirection, slot);

	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
	UnregisterSnapshot(snapshot);
	table_close(rel, NoLock);
	if (any)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("table \"%s\" holds values stored out of line, which a branch can't share",
						get_rel_name(table)),
				 errdetail("A value stored out of line is named by its TOAST table's object id, which a branch's copy doesn't have.")));
}

/* (each index of a relation, as the catalogs have them now) */
static List *
indexes_of(Relation pg_index, Oid relid, List *rels)
{
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;

	ScanKeyInit(&key, Anum_pg_index_indrelid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(relid));
	scan = systable_beginscan(pg_index, IndexIndrelidIndexId, true, NULL, 1, &key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
		rels = lappend_oid(rels, ((Form_pg_index) GETSTRUCT(tup))->indexrelid);
	systable_endscan(scan);
	return rels;
}

/*
 * Each table, materialized view and sequence of a branch's schemas, its owner's to share, with its
 * TOAST table and every index of either, in order of their ids, as the catalogs have them now; and
 * the TOAST tables in toasts, each after its table in tables.
 */
static List *
relations_of(int32 branch, List **tables, List **toasts)
{
	List	   *nsps = NIL;
	List	   *rels = NIL;
	Relation	rel;
	Relation	pg_index;
	SysScanDesc scan;
	HeapTuple	tup;

	*tables = NIL;
	*toasts = NIL;
	rel = table_open(NamespaceRelationId, AccessShareLock);
	scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_namespace ns = (Form_pg_namespace) GETSTRUCT(tup);

		if (ns->nspbranch == branch)
			nsps = lappend_oid(nsps, ns->oid);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	rel = table_open(RelationRelationId, AccessShareLock);
	pg_index = table_open(IndexRelationId, AccessShareLock);
	foreach_oid(nsp, nsps)
	{
		ScanKeyData key;

		ScanKeyInit(&key, Anum_pg_class_relnamespace, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(nsp));
		scan = systable_beginscan(rel, InvalidOid, false, NULL, 1, &key);
		while (HeapTupleIsValid(tup = systable_getnext(scan)))
		{
			Form_pg_class c = (Form_pg_class) GETSTRUCT(tup);

			if (c->relkind != RELKIND_RELATION && c->relkind != RELKIND_MATVIEW &&
				c->relkind != RELKIND_SEQUENCE)
				continue;
			if (!object_ownercheck(RelationRelationId, c->oid, GetUserId()))
				aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(c->relkind), NameStr(c->relname));
			rels = indexes_of(pg_index, c->oid, lappend_oid(rels, c->oid));
			if (OidIsValid(c->reltoastrelid))
			{
				rels = indexes_of(pg_index, c->reltoastrelid, lappend_oid(rels, c->reltoastrelid));
				*tables = lappend_oid(*tables, c->oid);
				*toasts = lappend_oid(*toasts, c->reltoastrelid);
			}
		}
		systable_endscan(scan);
	}
	table_close(pg_index, AccessShareLock);
	table_close(rel, AccessShareLock);
	list_free(nsps);
	list_sort(rels, list_oid_cmp);
	return rels;
}

/*
 * The parent held: its DDL and its commits waited out and held off (its catalogs' lock, and its
 * head's), and each of its relations locked so that its writers are waited out and held off, with
 * VACUUM and whatever else changes its pages.  A transaction of the parent's can take these in any
 * order, a write and then DDL, so no order of taking them rules out a deadlock: so each is waited
 * for at most half of what would find one, and past that all taken so far are let go of, giving
 * way, and taken again, until the time given is up.  A relation dropped or made meanwhile is found
 * as they're taken again.
 */
/*
 * (what's left of this round's wait, which all of it together keeps under half of what would find a
 * deadlock, as one waiting on what this holds began after this took it; and of the whole time, and
 * the session's own bound: none left, and this round gives way)
 */
static void
round_budget(Holding *h)
{
	TimestampTz now = GetCurrentTimestamp();
	long		left = Max(DeadlockTimeout / 2, 1) - TimestampDifferenceMilliseconds(h->round, now);
	long		whole = h->timeout_ms - TimestampDifferenceMilliseconds(h->start, now);
	char		ms[32];

	left = Min(left, whole);
	if (h->user_timeout > 0)
		left = Min(left, h->user_timeout);
	if (left <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
				 errmsg("a round of holding off a branch's writers ran out of time")));
	snprintf(ms, sizeof(ms), "%ld", left);
	(void) set_config_option("lock_timeout", ms, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
}

static bool
hold_try(Holding *h)
{
	List	   *tables;
	List	   *toasts;
	List	   *rels;
	List	   *again;
	List	   *t2;
	List	   *x2;
	bool		same;

	round_budget(h);
	LockBranchCatalogs(h->branch, AccessExclusiveLock);
	round_budget(h);
	LockBranchHead(h->branch);
	rels = relations_of(h->branch, &tables, &toasts);
	foreach_oid(relid, rels)
	{
		round_budget(h);
		LockRelationOid(relid, ExclusiveLock);
	}
	again = relations_of(h->branch, &t2, &x2);
	same = equal(rels, again);
	if (same)
	{
		ListCell   *t;
		ListCell   *x;

		forboth(t, tables, x, toasts)
			refuse_values_out_of_line(lfirst_oid(t), lfirst_oid(x));
	}
	list_free(rels);
	list_free(again);
	list_free(tables);
	list_free(toasts);
	list_free(t2);
	list_free(x2);
	return same;
}

static void
lock_writers_out(void *arg)
{
	Holding    *h = arg;
	MemoryContext cxt = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	int			nest = NewGUCNestLevel();

	h->start = GetCurrentTimestamp();
	h->user_timeout = LockTimeout;
	for (;;)
	{
		bool		held = false;
		bool		gave_way = false;

		h->round = GetCurrentTimestamp();
		BeginInternalSubTransaction(NULL);
		MemoryContextSwitchTo(cxt);
		PG_TRY();
		{
			held = hold_try(h);
		}
		PG_CATCH();
		{
			ErrorData  *e;

			MemoryContextSwitchTo(cxt);
			e = CopyErrorData();
			FlushErrorState();
			RollbackAndReleaseCurrentSubTransaction();
			MemoryContextSwitchTo(cxt);
			CurrentResourceOwner = owner;
			if (e->sqlerrcode != ERRCODE_LOCK_NOT_AVAILABLE)
				ReThrowError(e);
			FreeErrorData(e);
			gave_way = true;
		}
		PG_END_TRY();
		if (!gave_way)
		{
			if (held)
			{
				ReleaseCurrentSubTransaction();
				MemoryContextSwitchTo(cxt);
				CurrentResourceOwner = owner;
				break;
			}
			RollbackAndReleaseCurrentSubTransaction();
			MemoryContextSwitchTo(cxt);
			CurrentResourceOwner = owner;
		}
		if (TimestampDifferenceExceeds(h->start, GetCurrentTimestamp(), h->timeout_ms))
			ereport(ERROR,
					(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					 errmsg("could not hold off the writers of the branch being made from in time")));
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, 10, WAIT_EVENT_BRANCH_HOLD_GIVE_WAY);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}
	AtEOXact_GUC(true, nest);
}

typedef struct Told
{
	ProllyCtx  *ctx;
	Oid			nsp;			/* the schema being told of */
	const char *schema;
} Told;

/* (a table's map told as its rows; a partitioned table holds none of its own) */
static bool
tell_table(void *arg, const char *name, const VcTable *entry)
{
	Told	   *t = arg;
	char	   *table = ws_name_from_utf8(name);
	Oid			relid = get_relname_relid(table, t->nsp);
	Relation	rel;

	if (!OidIsValid(relid))
		elog(ERROR, "table \"%s\" of schema \"%s\" isn't there", table, t->schema);
	rel = relation_open(relid, AccessShareLock);
	if (rel->rd_rel->relkind == RELKIND_RELATION)
		CaptureRowsAre(rel, &entry->data);
	relation_close(rel, AccessShareLock);
	pfree(table);
	return true;
}

static bool
tell_schema(void *arg, const char *name, const PHash *root)
{
	Told	   *t = arg;
	char	   *schema = ws_name_from_utf8(name);

	t->schema = schema;
	t->nsp = get_namespace_oid(schema, false);
	WsRoot->tables(t->ctx, root, tell_table, t);
	pfree(schema);
	return true;
}

typedef enum MoveStep
{
	MOVE_MAKE,					/* a schema the target has that isn't there, restored */
	MOVE_KEPT,					/* one both have, moved to the target's */
	MOVE_DROP,					/* one the target lacks, dropped with all in it */
} MoveStep;

typedef struct Moving
{
	ProllyCtx  *ctx;
	const PHash *other;			/* the branch root the other side is */
	MoveStep	step;
} Moving;

static void
run_statement(const char *sql)
{
	WsSqlRows	rows;
	int			level = WsSql->begin();

	WsSql->run(sql, 0, NULL, &rows);
	ws_sql_rows_free(&rows);
	WsSql->end(level);
}

static bool
move_schema(void *arg, const char *name, const PHash *root)
{
	Moving	   *m = arg;
	char	   *schema = ws_name_from_utf8(name);
	PHash		other;
	bool		has = WsDbRoot->get_schema(m->ctx, m->other, name, &other);

	if (m->step == MOVE_DROP && !has)
		run_statement(psprintf("DROP SCHEMA %s CASCADE", quote_identifier(schema)));
	else if (m->step == MOVE_KEPT && has)
	{
		WsMoveStats stats;

		WsMove->move_from(m->ctx, schema, &other, root, &stats);
	}
	else if (m->step == MOVE_MAKE && !has)
	{
		run_statement(psprintf("CREATE SCHEMA %s VERSIONED", quote_identifier(schema)));
		WsRootIO->restore(m->ctx, root, NULL, schema);
	}
	pfree(schema);
	return true;
}

/*
 * A branch whose working set holds from made to hold to, in a transaction left out of capture
 * that holds off its writers, and capture told each of its tables' rows, as to has them: the
 * schemas to has that aren't there made first, as a schema it keeps may use what's in them; then
 * those kept moved, and last those it lacks dropped, once nothing kept uses them.
 */
static void
move_branch(ProllyCtx *ctx, int32 branch, const PHash *from, const PHash *to)
{
	int			level = CatalogBranchEnter(branch);
	Moving		m = {ctx, from, MOVE_MAKE};
	Told		told = {ctx};

	if (!phash_eq(from, to))
	{
		WsDbRoot->schemas(ctx, to, move_schema, &m);
		CommandCounterIncrement();
		m.step = MOVE_KEPT;
		WsDbRoot->schemas(ctx, to, move_schema, &m);
		CommandCounterIncrement();
		m.other = to;
		m.step = MOVE_DROP;
		WsDbRoot->schemas(ctx, from, move_schema, &m);
		CommandCounterIncrement();
	}
	WsDbRoot->schemas(ctx, to, tell_schema, &told);
	CatalogBranchLeave(level);
}

/* A commit the repository has, in its window, loaded into at: an error otherwise. */
static void
commit_check(ProllyCtx *ctx, VcRepo *repo, const PHash *commit, VcCommit *at)
{
	char		hex[PHASH_HEX_LEN + 1];

	if (vc_commit_load(ctx, commit, at))
	{
		if (vc_in_window(repo, commit))
			return;
		vc_commit_release(at);
		phash_hex(commit, hex);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("commit %s is outside the window", hex),
				 errhint("Attach the history it's in first.")));
	}
	phash_hex(commit, hex);
	ereport(ERROR,
			(errcode(ERRCODE_UNDEFINED_OBJECT),
			 errmsg("commit %s not found", hex)));
}

static int32
shared_create(ProllyCtx *ctx, const char *name, const char *from, const PHash *commit,
			  bool take_changes, int timeout_ms)
{
	ChunkStore *cs = ChunkStoreOpen("pages");
	int32		parent = BranchLink->branch_of(from);
	int32		branch;
	VcRepo		repo;
	VcBranch	head;
	VcBranch	taken;
	VcCommit	at;
	PHash		at_commit;
	PHash		root;
	PHash		target;
	Holding		holding;

	if (commit != NULL && take_changes)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a branch made at a commit can't take another's changes")));
	if (name[0] == '\0' || strlen(name) >= NAMEDATALEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("a branch's name is 1 to %d bytes", NAMEDATALEN - 1)));
	if (parent == 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("branch \"%s\" not found", from)));

	if (BranchLink->branch_of(name) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("a branch named \"%s\" already exists", name)));
	vc_repo_open(&repo, cs, VcRefsOpen("pointers", cs), &prolly_default_policy);

	/* (what's refused whatever the parent's head turns out to be, before anything's held) */
	if (!vc_branch_get(&repo, from, &head))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("branch \"%s\" has no commit", from)));
	vc_branch_free(&head);
	if (commit != NULL)
	{
		commit_check(ctx, &repo, commit, &at);
		vc_commit_release(&at);
	}

	/*
	 * (the parent at one point, its writers held off until this transaction ends; with its DDL and
	 * its commits, as a table made meanwhile would have writers the barrier doesn't hold off, and a
	 * commit writing its head from a root taken before this one's would leave the head behind the
	 * working set: so the head is read once they're held off)
	 */
	holding.branch = parent;
	holding.timeout_ms = timeout_ms;
	CaptureBranchBarrierLeavingOut(ctx, parent, timeout_ms, lock_writers_out, &holding, &root);

	if (BranchLink->branch_of(name) != 0 || vc_branch_get(&repo, name, &taken))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("a branch named \"%s\" already exists", name)));
	if (!vc_branch_get(&repo, from, &head))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("branch \"%s\" has no commit", from)));

	/* (where it's made: the commit, or from's head as it is now) */
	at_commit = commit != NULL ? *commit : head.head;
	commit_check(ctx, &repo, &at_commit, &at);
	target = take_changes ? root : at.root;

	branch = BranchIds->allocate();
	BranchCatalogCopy->copy(parent, branch, true);
	CommandCounterIncrement();

	/*
	 * (its tables hold the parent's working set, the root: what differs from what it's to hold
	 * written; and with its changes taken, the parent's written back to its head)
	 */
	move_branch(ctx, branch, &root, &target);
	if (take_changes)
		move_branch(ctx, parent, &root, &head.head_root);

	if (vc_branch_create(&repo, name, &at_commit) != 0)
		elog(ERROR, "could not make branch \"%s\" of version control", name);

	/* (its working root, with the changes it took, as its tables hold it) */
	if (take_changes)
	{
		if (vc_checkout(&repo, name) != 0)
			elog(ERROR, "could not check out branch \"%s\" of version control", name);
		vc_set_working(&repo, &root);
	}
	BranchLink->link(name, branch);
	vc_commit_release(&at);
	vc_branch_free(&head);
	vc_repo_free(&repo);
	return branch;
}

const BranchOpsRoutine SharedBranchOps = {
	.name = "shared",
	.create = shared_create,
};

const BranchOpsRoutine *BranchOps = &SharedBranchOps;
