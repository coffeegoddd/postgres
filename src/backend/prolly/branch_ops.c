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
 * committed then, and the barrier's root is that too: each table's row map as
 * of the marker is the new table's, which capture is told (CaptureRowsAre).
 * Catalog changes in the database wait until the transaction ends, as they do
 * for any commit from capture that's left out.
 *
 * The catalog rows are copied (prolly/branch_catalog.h), each relation's
 * storage sharing its source's copy-on-write, but a sequence's, whose page is
 * copied; the branch of version control is made at the parent's head, and
 * linked to the catalog branch.  All in the caller's transaction: an abort
 * leaves none of it, the shared storage dropped as any made in it is.
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
#include "access/table.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
#include "prolly/branch_catalog.h"
#include "prolly/branch_link.h"
#include "prolly/branch_ops.h"
#include "prolly/capture.h"
#include "prolly/chunkstore.h"
#include "prolly/engine/vc.h"
#include "prolly/engine/vc_refs.h"
#include "prolly/ws/ws_dbroot.h"
#include "prolly/ws/ws_root.h"
#include "prolly/ws/ws_schema.h"
#include "storage/lmgr.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

/* Each table and sequence of a branch's schemas, locked so that its writers are waited out. */
static void
lock_writers_out(void *arg)
{
	int32		branch = *(int32 *) arg;
	List	   *nsps = NIL;
	List	   *rels = NIL;
	Relation	rel;
	SysScanDesc scan;
	HeapTuple	tup;

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
	scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_class c = (Form_pg_class) GETSTRUCT(tup);

		if (list_member_oid(nsps, c->relnamespace) &&
			(c->relkind == RELKIND_RELATION || c->relkind == RELKIND_MATVIEW ||
			 c->relkind == RELKIND_SEQUENCE))
			rels = lappend_oid(rels, c->oid);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	/* (in object id order, as another of these takes them) */
	list_sort(rels, list_oid_cmp);
	foreach_oid(relid, rels)
		LockRelationOid(relid, ShareLock);
	list_free(rels);
	list_free(nsps);
}

typedef struct Told
{
	ProllyCtx  *ctx;
	int32		branch;
	Oid			nsp;			/* the schema being told of, on the new branch */
	const char *schema;
} Told;

/* (a table's map told as the new one's rows; a partitioned table holds none of its own) */
static bool
tell_table(void *arg, const char *name, const VcTable *entry)
{
	Told	   *t = arg;
	char	   *table = ws_name_from_utf8(name);
	Oid			relid = get_relname_relid(table, t->nsp);
	Relation	rel;

	if (!OidIsValid(relid))
		elog(ERROR, "table \"%s\" of schema \"%s\" wasn't copied", table, t->schema);
	rel = relation_open(relid, NoLock);
	if (rel->rd_rel->relkind == RELKIND_RELATION)
		CaptureRowsAre(rel, &entry->data);
	relation_close(rel, NoLock);
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

static int32
shared_create(ProllyCtx *ctx, const char *name, const char *from, int timeout_ms)
{
	ChunkStore *cs = ChunkStoreOpen("pages");
	int32		parent = BranchLink->branch_of(from);
	int32		branch;
	VcRepo		repo;
	VcBranch	head;
	VcBranch	taken;
	PHash		root;
	int			level;
	Told		told;

	if (parent == 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("branch \"%s\" not found", from)));
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

	/* (the parent at one point, its writers held off until this transaction ends) */
	CaptureBranchBarrierLeavingOut(ctx, parent, timeout_ms, lock_writers_out, &parent, &root);
	if (!phash_eq(&root, &head.head_root))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("branch \"%s\" has changes not committed", from),
				 errdetail("A branch is made only from one whose working set is its head.")));

	branch = BranchIds->allocate();
	BranchCatalogCopy->copy(parent, branch, true);
	CommandCounterIncrement();

	/* (each table's rows, as capture is to have them, are the parent's as of the marker) */
	level = CatalogBranchEnter(branch);
	told.ctx = ctx;
	told.branch = branch;
	WsDbRoot->schemas(ctx, &root, tell_schema, &told);
	CatalogBranchLeave(level);

	if (vc_branch_create(&repo, name, &head.head) != 0)
		elog(ERROR, "could not make branch \"%s\" of version control", name);
	BranchLink->link(name, branch);
	vc_branch_free(&head);
	vc_repo_free(&repo);
	return branch;
}

const BranchOpsRoutine SharedBranchOps = {
	.name = "shared",
	.create = shared_create,
};

const BranchOpsRoutine *BranchOps = &SharedBranchOps;
