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
 * The copy is of the source as its writers left it: its versioned
 * relations are locked against changes meanwhile (ShareLock, which waits
 * for writers to finish), so no change is in flight in a page the copy
 * shares, and the copy holds every row they committed, whether the making
 * transaction's snapshot sees it or not (what moves the copy reads it by
 * one that does, or, where the snapshot can't move, empties the copies
 * that hold what it doesn't see).  The locks are let go once the copy is
 * made, not held to the end of the transaction: the copy shares no page a
 * later change could be in (one changes its own), and whatever the making
 * transaction does next with the new branch (moving it to a commit, say)
 * needn't hold up the source's writers.
 *
 * Deleting a branch deletes its rows and drops its relations' storage when
 * the transaction commits; pages another branch still shares stay.  It
 * waits for no session: one on the branch holds a lock deleting conflicts
 * with (check_out_branch()), and deleting fails instead.
 *
 * Privileges: making a branch copies every versioned object and locks
 * them, so it takes CREATE on the database; deleting one drops every
 * branch's copies of objects whoever owns them, so it takes owning the
 * database.  The copies keep their owners and privileges.
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
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_branch.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "catalog/versioned.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
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
	SMgrRelation ssrc;
	RelFileNumber relnumber;
	RelFileLocator dst;
	SMgrRelation sdst;

	if (c->relpersistence != RELPERSISTENCE_PERMANENT)
		elog(ERROR, "versioned relation \"%s\" is not permanent",
			 NameStr(c->relname));

	/*
	 * Storage made in this transaction that skips WAL (wal_level minimal; a
	 * new relation, or one truncated or rewritten) is synced at commit; a
	 * copy of it would be neither logged nor synced.
	 */
	if (RelFileLocatorSkippingWAL(src))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot copy relation \"%s\" to a branch in the transaction that gave it its storage",
						NameStr(c->relname)),
				 errdetail("Its storage skips WAL until the transaction commits."),
				 errhint("Commit first, or set \"wal_level\" to \"replica\" or higher.")));

	ssrc = smgropen(src, INVALID_PROC_NUMBER);
	relnumber = GetNewRelFileNumber(c->reltablespace, NULL, c->relpersistence);
	dst = class_locator(c, relnumber);

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
 * copy is of a state no writer is in the middle of.  Returns them, for
 * unlock_relations().
 */
static List *
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

	/* read the catalogs as the locks left them */
	InvalidateCatalogSnapshot();
	return relids;
}

/* Let go of the locks lock_versioned_relations() took (only those). */
static void
unlock_relations(List *relids)
{
	ListCell   *lc;

	foreach(lc, relids)
		UnlockRelationOid(lfirst_oid(lc), ShareLock);
	list_free(relids);
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
				pgstat_drop_relation_by_id(c->relkind, c->oid, c->relbranch);
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
	List	   *locked = lock_versioned_relations();

	copy_rows(*(Oid *) arg);
	copyBranchSharedDependencies(MyBranch, *(Oid *) arg);
	unlock_relations(locked);
}

static void
delete_here(void *arg)
{
	delete_rows();
	dropBranchSharedDependencies(MyBranch);
}

/*
 * Is name a branch's name by git's rules (git check-ref-format, as Dolt
 * checks them)?  "~" and "^" would be read as ancestry steps, ":" as a
 * refspec, and so on: a branch so named couldn't be named again.  As in
 * Dolt, a name is ASCII, and may begin with "-" (given after "--") but
 * isn't "-" alone (git's previous branch), nor, as in git, "--" (the end
 * of the options, spec 18, D14); "HEAD", in any case, the
 * session branch's head, isn't a branch's name.  (Nor is a commit's id:
 * the refs, which know commits, refuse that.)
 */
static bool
branch_name_ok(const char *name)
{
	size_t		len = strlen(name);

	if (len == 0 || len >= NAMEDATALEN || strcmp(name, "-") == 0 ||
		strcmp(name, "--") == 0)
		return false;

	if (pg_strcasecmp(name, "HEAD") == 0 || strcmp(name, "@") == 0 ||
		name[0] == '/' || name[len - 1] == '/' ||
		name[len - 1] == '.' || strstr(name, "..") != NULL ||
		strstr(name, "@{") != NULL || strstr(name, "//") != NULL)
		return false;

	/* each "/"-separated part: not beginning with ".", nor ending ".lock" */
	for (const char *part = name; part != NULL;)
	{
		const char *end = strchr(part, '/');
		size_t		plen = end ? (size_t) (end - part) : strlen(part);

		if (part[0] == '.' ||
			(plen >= 5 && strncmp(part + plen - 5, ".lock", 5) == 0))
			return false;
		part = end ? end + 1 : NULL;
	}
	for (const char *p = name; *p; p++)
	{
		unsigned char c = (unsigned char) *p;

		if (c <= ' ' || c >= 0x7f || strchr("~^:?*[\\", c) != NULL)
			return false;
	}
	return true;
}

static void
copies_check_create(const char *name)
{
	AclResult	aclresult;

	if (!branch_name_ok(name))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("invalid branch name \"%s\"", name),
				 errdetail("A branch's name follows git's rules for one, in ASCII: no spaces, control characters or any of ~ ^ : ? * [ \\; no \"..\", \"@{\" or \"//\"; not \"HEAD\" (in any case) or \"@\"; not \"-\" or \"--\" alone, or beginning with \"/\", nor ending with \"/\" or \".\"; and no \"/\"-separated part beginning with \".\" or ending with \".lock\".")));
	PreventCommandIfReadOnly("dolt_branch()");
	/* (one made on the session's behalf is the database's mechanism, not the role's) */
	if (BranchCreateOnBehalf)
		return;
	aclresult = object_aclcheck(DatabaseRelationId, MyDatabaseId, GetUserId(),
								ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_DATABASE,
					   get_database_name(MyDatabaseId));
}

static Oid
copies_make(const char *name, const char *start_point)
{
	Relation	brel;
	Oid			source;
	Oid			branch;
	Datum		values[Natts_pg_branch];
	bool		nulls[Natts_pg_branch] = {0};
	NameData	brname;
	HeapTuple	tup;

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
	values[Anum_pg_branch_brownerrole - 1] = ObjectIdGetDatum(InvalidOid);
	nulls[Anum_pg_branch_browner - 1] = true;	/* (a new branch is clean) */
	tup = heap_form_tuple(RelationGetDescr(brel), values, nulls);
	CatalogTupleInsert(brel, tup);
	heap_freetuple(tup);
	CommandCounterIncrement();

	on_branch(source, copy_from_here, &branch);
	CommandCounterIncrement();
	note_new_branch(branch);

	table_close(brel, NoLock);
	return branch;
}

static Oid
copies_create(const char *name, const char *start_point)
{
	copies_check_create(name);
	return copies_make(name, start_point);
}

static void
copies_check_drop(const char *name)
{
	PreventCommandIfReadOnly("dolt_branch()");
	if (!object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId()))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to delete branch \"%s\"", name),
				 errdetail("Only the owner of database \"%s\" can delete its branches.",
						   get_database_name(MyDatabaseId))));
}

static void
copies_drop(const char *name)
{
	Relation	brel;
	Oid			branch;
	HeapTuple	tup;

	copies_check_drop(name);
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
	if (!lock_branch_for_delete(branch))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("branch \"%s\" is checked out in another session", name)));
	lock_branch_unprepared(branch, name);

	on_branch(branch, delete_here, NULL);

	tup = SearchSysCache1(BRANCHOID, ObjectIdGetDatum(branch));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for branch %u", branch);
	CatalogTupleDelete(brel, &tup->t_self);
	ReleaseSysCache(tup);
	CommandCounterIncrement();
	table_close(brel, NoLock);
}

/*
 * Renaming changes only the pg_branch row: copies, and sessions, know a
 * branch by its id.  Not main, which every database has.
 */
static void
copies_rename(const char *name, const char *newname, bool force)
{
	Relation	brel;
	Oid			branch;
	HeapTuple	tup;
	NameData	brname;

	copies_check_drop(name);
	copies_check_create(newname);
	brel = table_open(BranchRelationId, ShareRowExclusiveLock);
	branch = branch_oid(name, false);
	if (branch == BRANCH_MAIN_OID)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("branch \"main\" can't be renamed")));
	if (OidIsValid(branch_oid(newname, true)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("branch \"%s\" already exists", newname)));
	/* (the session's own lock on its branch doesn't conflict) */
	if (!force && !lock_branch_for_delete(branch))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("branch \"%s\" is checked out in another session", name),
				 errhint("Rename it anyway with -f.")));

	tup = SearchSysCacheCopy1(BRANCHOID, ObjectIdGetDatum(branch));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for branch %u", branch);
	namestrcpy(&brname, newname);
	((Form_pg_branch) GETSTRUCT(tup))->brname = brname;
	CatalogTupleUpdate(brel, &tup->t_self, tup);
	heap_freetuple(tup);
	CommandCounterIncrement();
	table_close(brel, NoLock);
}

static void
copies_checkout(const char *name)
{
	if (!check_out_branch(branch_oid(name, false)))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("branch \"%s\" does not exist", name)));
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

static void
copies_run_on(const char *name, void (*fn) (void *arg), void *arg)
{
	on_branch(branch_oid(name, false), fn, arg);
}

static const BranchOpsRoutine branch_copies = {
	.name = "copies",
	.create = copies_create,
	.drop = copies_drop,
	.rename = copies_rename,
	.check_create = copies_check_create,
	.check_drop = copies_check_drop,
	.name_ok = branch_name_ok,
	.make = copies_make,
	.checkout = copies_checkout,
	.current = copies_current,
	.branches = copies_branches,
	.run_on = copies_run_on,
};

const BranchOpsRoutine *
branch_ops(void)
{
	return &branch_copies;
}
