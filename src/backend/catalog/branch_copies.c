/*-------------------------------------------------------------------------
 *
 * branch_copies.c
 *	  Branches as copies of the versioned schemas.
 *
 * A branch is a pg_branch row and a copy of every versioned object's
 * catalog rows, with the same OIDs and the branch's id in the branch column.
 * Making one copies the source branch's rows; a relation's copy gets a new
 * relation file number whose storage shares the source's pages until either
 * changes them (smgrshare()), or, where the storage manager can't share
 * (md.c's sequences), a copy of its files.  No page is read.
 *
 * The copy is a snapshot of the source as the making transaction sees it:
 * its versioned relations are locked against changes meanwhile (ShareLock,
 * which waits for writers to finish), so no change is in flight in a page
 * the copy shares.  Relation locks are by OID, so this also holds up writers
 * of the same objects on other branches for as long.
 *
 * Deleting a branch deletes its rows and drops its relations' storage when
 * the transaction commits; pages another branch still shares stay.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/catalog/branch_copies.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heaptoast.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/branch.h"
#include "catalog/branch_ops.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/pg_branch.h"
#include "catalog/pg_class.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "catalog/versioned.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/* the id of the branch called name; InvalidOid if none and missing_ok */
static Oid
branch_oid(const char *name, bool missing_ok)
{
	HeapTuple	tup;
	Oid			oid;

	tup = SearchSysCache1(BRANCHNAME, CStringGetDatum(name));
	if (!HeapTupleIsValid(tup))
	{
		if (missing_ok)
			return InvalidOid;
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("branch \"%s\" does not exist", name)));
	}
	oid = ((Form_pg_branch) GETSTRUCT(tup))->oid;
	ReleaseSysCache(tup);
	return oid;
}

static char *
branch_name(Oid branch)
{
	HeapTuple	tup;
	char	   *name;

	tup = SearchSysCache1(BRANCHOID, ObjectIdGetDatum(branch));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("the session's branch no longer exists")));
	name = pstrdup(NameStr(((Form_pg_branch) GETSTRUCT(tup))->brname));
	ReleaseSysCache(tup);
	return name;
}

/* the storage locator of a pg_class row */
static RelFileLocator
class_locator(Form_pg_class c, RelFileNumber relnumber)
{
	RelFileLocator loc;

	loc.spcOid = OidIsValid(c->reltablespace) ? c->reltablespace : MyDatabaseTableSpace;
	loc.dbOid = MyDatabaseId;
	loc.relNumber = relnumber;
	return loc;
}

/*
 * Give a relation's copy storage of its own: shared with the source's
 * pages where the storage manager can, else a copy of its files.
 */
static RelFileNumber
copy_storage(Form_pg_class c)
{
	RelFileLocator src = class_locator(c, c->relfilenode);
	SMgrRelation ssrc = smgropen(src, INVALID_PROC_NUMBER);
	RelFileNumber relnumber = GetNewRelFileNumber(c->reltablespace, NULL,
												  c->relpersistence);
	RelFileLocator dst = class_locator(c, relnumber);
	SMgrRelation sdst;

	if (c->relpersistence != RELPERSISTENCE_PERMANENT)
		elog(ERROR, "versioned relation \"%s\" is not permanent",
			 NameStr(c->relname));

	if (smgrcanshare(ssrc))
	{
		RelationCreateStorageShared(dst, ssrc, true);
		return relnumber;
	}

	/* as ALTER TABLE ... SET TABLESPACE copies an index's files */
	FlushRelationsAllBuffers(&ssrc, 1);
	sdst = RelationCreateStorage(dst, c->relpersistence, true);
	RelationCopyStorage(ssrc, sdst, MAIN_FORKNUM, c->relpersistence);
	for (ForkNumber fork = MAIN_FORKNUM + 1; fork <= MAX_FORKNUM; fork++)
	{
		if (smgrexists(ssrc, fork))
		{
			smgrcreate(sdst, fork, false);
			log_smgrcreate(&dst, fork);
			RelationCopyStorage(ssrc, sdst, fork, c->relpersistence);
		}
	}
	return relnumber;
}

/*
 * Lock the session branch's versioned relations against changes, so the
 * copy is of a state no writer is in the middle of.
 */
static void
lock_versioned_relations(void)
{
	Relation	rel = table_open(RelationRelationId, AccessShareLock);
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *relids = NIL;
	ListCell   *lc;

	scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_class c = (Form_pg_class) GETSTRUCT(tup);

		if (c->relbranch == MyBranch)
			relids = lappend_oid(relids, c->oid);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	foreach(lc, relids)
		LockRelationOid(lfirst_oid(lc), ShareLock);
	list_free(relids);

	/* read the catalogs as the locks left them */
	InvalidateCatalogSnapshot();
}

/* Copy every row of the session branch into branch 'target'. */
static void
copy_rows(Oid target)
{
	int			ncatalogs;
	const CatalogBranchInfo *catalogs = catalog_branch_catalogs(&ncatalogs);

	for (int i = 0; i < ncatalogs; i++)
	{
		const CatalogBranchInfo *info = &catalogs[i];
		Relation	rel = table_open(info->relid, RowExclusiveLock);
		TupleDesc	desc = RelationGetDescr(rel);
		CatalogIndexState indstate = CatalogOpenIndexes(rel);
		SysScanDesc scan;
		HeapTuple	tup;

		/* rows added here are the target's, not seen by this scan */
		scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
		while ((tup = systable_getnext(scan)) != NULL)
		{
			HeapTuple	copy;

			if (catalog_row_branch(info, tup) != MyBranch)
				continue;

			/* out-of-line values get their own copies, as rows of their own */
			copy = HeapTupleHasExternal(tup) ? toast_flatten_tuple(tup, desc)
				: heap_copytuple(tup);
			memcpy((char *) GETSTRUCT(copy) + info->offset, &target, sizeof(Oid));

			if (info->relid == RelationRelationId)
			{
				Form_pg_class c = (Form_pg_class) GETSTRUCT(copy);

				if (RELKIND_HAS_STORAGE(c->relkind))
				{
					if (!RelFileNumberIsValid(c->relfilenode))
						elog(ERROR, "versioned relation \"%s\" is mapped",
							 NameStr(c->relname));
					c->relfilenode = copy_storage(c);
				}
			}

			CatalogKeepRowBranch = true;
			PG_TRY();
			{
				CatalogTupleInsertWithInfo(rel, copy, indstate);
			}
			PG_FINALLY();
			{
				CatalogKeepRowBranch = false;
			}
			PG_END_TRY();
			heap_freetuple(copy);
		}
		systable_endscan(scan);
		CatalogCloseIndexes(indstate);
		table_close(rel, RowExclusiveLock);
	}
}

/* Delete every row of the session branch, and drop its storage at commit. */
static void
delete_rows(void)
{
	int			ncatalogs;
	const CatalogBranchInfo *catalogs = catalog_branch_catalogs(&ncatalogs);

	for (int i = 0; i < ncatalogs; i++)
	{
		const CatalogBranchInfo *info = &catalogs[i];
		Relation	rel = table_open(info->relid, RowExclusiveLock);
		SysScanDesc scan;
		HeapTuple	tup;

		scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
		while ((tup = systable_getnext(scan)) != NULL)
		{
			if (catalog_row_branch(info, tup) != MyBranch)
				continue;
			if (info->relid == RelationRelationId)
			{
				Form_pg_class c = (Form_pg_class) GETSTRUCT(tup);

				if (RELKIND_HAS_STORAGE(c->relkind) &&
					RelFileNumberIsValid(c->relfilenode))
					RelationDropStorageByLocator(class_locator(c, c->relfilenode));
			}
			CatalogTupleDelete(rel, &tup->t_self);
		}
		systable_endscan(scan);
		table_close(rel, RowExclusiveLock);
	}
}

/*
 * Run 'fn' with the session on branch 'branch', back on its own branch
 * after, whatever happens.
 */
static void
on_branch(Oid branch, void (*fn) (void *arg), void *arg)
{
	Oid			saved = MyBranch;

	set_session_branch(branch);
	PG_TRY();
	{
		fn(arg);
	}
	PG_FINALLY();
	{
		set_session_branch(saved);
	}
	PG_END_TRY();
}

static void
copy_from_here(void *arg)
{
	lock_versioned_relations();
	copy_rows(*(Oid *) arg);
}

static void
delete_here(void *arg)
{
	delete_rows();
}

static Oid
copies_create(const char *name, const char *start_point)
{
	Relation	brel;
	Oid			source;
	Oid			branch;
	Datum		values[Natts_pg_branch];
	bool		nulls[Natts_pg_branch] = {0};
	NameData	brname;
	HeapTuple	tup;

	if (name[0] == '\0' || strlen(name) >= NAMEDATALEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("invalid branch name \"%s\"", name)));
	PreventCommandIfReadOnly("dolt_branch()");

	brel = table_open(BranchRelationId, ShareRowExclusiveLock);
	if (OidIsValid(branch_oid(name, true)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("branch \"%s\" already exists", name)));
	source = start_point ? branch_oid(start_point, false) : MyBranch;

	branch = GetNewOidWithIndex(brel, BranchOidIndexId, Anum_pg_branch_oid);
	namestrcpy(&brname, name);
	values[Anum_pg_branch_oid - 1] = ObjectIdGetDatum(branch);
	values[Anum_pg_branch_brname - 1] = NameGetDatum(&brname);
	tup = heap_form_tuple(RelationGetDescr(brel), values, nulls);
	CatalogTupleInsert(brel, tup);
	heap_freetuple(tup);
	CommandCounterIncrement();

	on_branch(source, copy_from_here, &branch);
	CommandCounterIncrement();

	table_close(brel, NoLock);
	return branch;
}

static void
copies_drop(const char *name)
{
	Relation	brel;
	Oid			branch;
	HeapTuple	tup;

	PreventCommandIfReadOnly("dolt_branch()");
	brel = table_open(BranchRelationId, ShareRowExclusiveLock);
	branch = branch_oid(name, false);
	if (branch == BRANCH_MAIN_OID)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("branch \"main\" can't be deleted")));
	if (branch == MyBranch)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("branch \"%s\" is checked out", name),
				 errhint("Check out another branch first.")));
	if (CountBranchBackends(MyDatabaseId, branch) > 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("branch \"%s\" is checked out in another session", name)));

	on_branch(branch, delete_here, NULL);

	tup = SearchSysCache1(BRANCHOID, ObjectIdGetDatum(branch));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for branch %u", branch);
	CatalogTupleDelete(brel, &tup->t_self);
	ReleaseSysCache(tup);
	CommandCounterIncrement();
	table_close(brel, NoLock);
}

static void
copies_checkout(const char *name)
{
	set_session_branch(branch_oid(name, false));
}

static char *
copies_current(void)
{
	return branch_name(MyBranch);
}

static void
copies_branches(BranchVisit visit, void *arg)
{
	Relation	brel = table_open(BranchRelationId, AccessShareLock);
	SysScanDesc scan;
	HeapTuple	tup;

	scan = systable_beginscan(brel, BranchNameIndexId, true, NULL, 0, NULL);
	while ((tup = systable_getnext_ordered(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_branch b = (Form_pg_branch) GETSTRUCT(tup);

		visit(NameStr(b->brname), b->oid, arg);
	}
	systable_endscan(scan);
	table_close(brel, AccessShareLock);
}

static const BranchOpsRoutine branch_copies = {
	.name = "copies",
	.create = copies_create,
	.drop = copies_drop,
	.checkout = copies_checkout,
	.current = copies_current,
	.branches = copies_branches,
};

const BranchOpsRoutine *
branch_ops(void)
{
	return &branch_copies;
}

bool
database_uses_branch_copies(void)
{
	Relation	brel;
	SysScanDesc scan;
	HeapTuple	tup;
	bool		more = false;

	if (MyDatabaseVersioned)
		return false;
	if (database_has_versioned_schemas())
		return true;

	/* a branch besides main, even if the session's has no versioned schema */
	brel = table_open(BranchRelationId, AccessShareLock);
	scan = systable_beginscan(brel, InvalidOid, false, NULL, 0, NULL);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		if (((Form_pg_branch) GETSTRUCT(tup))->oid != BRANCH_MAIN_OID)
		{
			more = true;
			break;
		}
	}
	systable_endscan(scan);
	table_close(brel, AccessShareLock);
	return more;
}
