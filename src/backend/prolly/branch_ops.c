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

#include "executor/executor.h"
#include "storage/lockdefs.h"
#include "access/stratnum.h"
#include "utils/fmgroids.h"
#include "utils/snapmgr.h"
#include "utils/guc.h"
#include "utils/acl.h"
#include "executor/tuptable.h"
#include "access/tableam.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
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
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

/* What the parent's writers are held off for: the branch, and how long a lock is waited for. */
typedef struct Holding
{
	int32		branch;
	int			timeout_ms;
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

/*
 * Each table, materialized view and sequence of a branch's schemas, its owner's to share, locked so
 * that its writers are waited out and held off, with VACUUM and whatever else changes its pages,
 * and so its TOAST table and every index: in order of their ids, as what else locks more than one
 * does, waiting at most the time given.
 */
static void
lock_writers_out(void *arg)
{
	Holding    *h = arg;
	List	   *nsps = NIL;
	List	   *tables = NIL;
	List	   *rels = NIL;
	Relation	rel;
	SysScanDesc scan;
	HeapTuple	tup;
	int			nest;

	rel = table_open(NamespaceRelationId, AccessShareLock);
	scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_namespace ns = (Form_pg_namespace) GETSTRUCT(tup);

		if (ns->nspbranch == h->branch)
			nsps = lappend_oid(nsps, ns->oid);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	rel = table_open(RelationRelationId, AccessShareLock);
	foreach_oid(nsp, nsps)
	{
		ScanKeyData key;

		ScanKeyInit(&key, Anum_pg_class_relnamespace, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(nsp));
		scan = systable_beginscan(rel, InvalidOid, false, NULL, 1, &key);
		while (HeapTupleIsValid(tup = systable_getnext(scan)))
		{
			Form_pg_class c = (Form_pg_class) GETSTRUCT(tup);

			if (c->relkind == RELKIND_RELATION || c->relkind == RELKIND_MATVIEW ||
				c->relkind == RELKIND_SEQUENCE)
			{
				if (!object_ownercheck(RelationRelationId, c->oid, GetUserId()))
					aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(c->relkind), NameStr(c->relname));
				tables = lappend_oid(tables, c->oid);
			}
		}
		systable_endscan(scan);
	}
	table_close(rel, AccessShareLock);

	/* (the lock's wait bounded, as the barrier's is) */
	nest = NewGUCNestLevel();
	if (h->timeout_ms > 0 && (LockTimeout == 0 || LockTimeout > h->timeout_ms))
	{
		char		ms[32];

		snprintf(ms, sizeof(ms), "%d", h->timeout_ms);
		(void) set_config_option("lock_timeout", ms, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	}
	list_sort(tables, list_oid_cmp);
	foreach_oid(relid, tables)
	{
		Relation	r;
		Oid			toast;

		LockRelationOid(relid, ExclusiveLock);
		r = relation_open(relid, NoLock);
		toast = r->rd_rel->reltoastrelid;
		rels = list_concat(rels, RelationGetIndexList(r));
		relation_close(r, NoLock);
		if (OidIsValid(toast))
		{
			LockRelationOid(toast, ExclusiveLock);
			r = relation_open(toast, NoLock);
			rels = list_concat(rels, RelationGetIndexList(r));
			relation_close(r, NoLock);
			refuse_values_out_of_line(relid, toast);
		}
	}
	list_sort(rels, list_oid_cmp);
	foreach_oid(relid, rels)
		LockRelationOid(relid, ExclusiveLock);
	AtEOXact_GUC(true, nest);
	list_free(tables);
	list_free(rels);
	list_free(nsps);
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

	/*
	 * (the parent's DDL waited out and held off, as a table made meanwhile would have writers the
	 * barrier doesn't hold off; and its head, as a commit writing it from a root taken before this
	 * one's would leave it behind the working set)
	 */
	LockBranchCatalogs(parent, AccessExclusiveLock);
	LockBranchHead(parent);

	if (BranchLink->branch_of(name) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("a branch named \"%s\" already exists", name)));
	vc_repo_open(&repo, cs, VcRefsOpen("pointers", cs), &prolly_default_policy);
	if (vc_branch_get(&repo, name, &taken))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("a branch named \"%s\" already exists", name)));
	if (!vc_branch_get(&repo, from, &head))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("branch \"%s\" has no commit", from)));

	/* (where it's made: the commit, which the repository has in its window, or from's head) */
	at_commit = commit != NULL ? *commit : head.head;
	if (!vc_commit_load(ctx, &at_commit, &at))
	{
		char		hex[PHASH_HEX_LEN + 1];

		phash_hex(&at_commit, hex);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("commit %s not found", hex)));
	}
	if (!vc_in_window(&repo, &at_commit))
	{
		char		hex[PHASH_HEX_LEN + 1];

		phash_hex(&at_commit, hex);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("commit %s is outside the window", hex),
				 errhint("Attach the history it's in first.")));
	}

	/* (the parent at one point, its writers held off until this transaction ends) */
	holding.branch = parent;
	holding.timeout_ms = timeout_ms;
	CaptureBranchBarrierLeavingOut(ctx, parent, timeout_ms, lock_writers_out, &holding, &root);
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
		(void) vc_working_lock(&repo);
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
