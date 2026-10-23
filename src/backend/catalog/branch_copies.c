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
 * relations are locked against changes meanwhile (ShareLock, taken once no
 * writer holds them: tried again until then, not queued for, so writers
 * coming after aren't held up), so no change is in flight in a page the
 * copy shares, and the copy holds every row they committed, whether the making
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

#include "access/prolly_config.h"

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
#include "catalog/pg_attribute.h"
#include "catalog/pg_branch.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_index.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "catalog/versioned.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/proc.h"
#include "storage/smgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

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

/* the longest wait between tries at the copy's locks, in milliseconds */
#define COPY_LOCK_MAX_DELAY_MS	100

/*
 * Take ShareLock on every one of relids, or on none: a lock that would
 * wait gives back those already taken.  Returns the first relation that
 * couldn't be locked, or InvalidOid once all are.
 */
static Oid
lock_all_or_none(List *relids)
{
	int			ntaken = 0;
	Oid			busy = InvalidOid;
	ListCell   *lc;

	foreach(lc, relids)
	{
		if (!ConditionalLockRelationOid(lfirst_oid(lc), ShareLock))
		{
			busy = lfirst_oid(lc);
			break;
		}
		ntaken++;
	}
	if (OidIsValid(busy))
		foreach(lc, relids)
		{
			if (ntaken-- == 0)
				break;
			UnlockRelationOid(lfirst_oid(lc), ShareLock);
		}
	return busy;
}

/*
 * Lock the session branch's versioned relations against changes, so the
 * copy is of a state no writer is in the middle of.  Returns them, for
 * unlock_relations().
 *
 * The locks are tried, not waited for: a ShareLock in the lock queue would
 * hold up every writer that comes after it, for as long as the writers
 * before it take, and then for the copy.  So all are taken at once or none
 * is, and the try is repeated, a little later each time, until they are
 * (writers coming and going meanwhile go ahead), or lock_timeout passes
 * (0, the default, waits for as long as it takes).  Nor does the deadlock
 * detector see this wait: lock_timeout or statement_timeout bounds it.
 *
 * The source's dirty pages are written first, outside the locks, so that
 * what the copy writes under them is only what changed meanwhile.
 */
static List *
lock_versioned_relations(void)
{
	Relation	rel = table_open(RelationRelationId, AccessShareLock);
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *relids = NIL;
	List	   *toasts = NIL;
	SMgrRelation *srels = NULL;
	int			nsrels = 0;
	int			maxsrels = 0;
	TimestampTz start = 0;
	long		delay = 1;
	Oid			busy;

	scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_class c = (Form_pg_class) GETSTRUCT(tup);

		if (c->relbranch != MyBranch)
			continue;
		if (c->relkind == RELKIND_TOASTVALUE)
			toasts = lappend_oid(toasts, c->oid);
		else
			relids = lappend_oid(relids, c->oid);

		if (RELKIND_HAS_STORAGE(c->relkind) &&
			c->relpersistence == RELPERSISTENCE_PERMANENT &&
			RelFileNumberIsValid(c->relfilenode))
		{
			if (nsrels == maxsrels)
			{
				maxsrels = maxsrels ? maxsrels * 2 : 16;
				srels = srels ? repalloc_array(srels, SMgrRelation, maxsrels)
					: palloc_array(SMgrRelation, maxsrels);
			}
			srels[nsrels++] = smgropen(class_locator(c, c->relfilenode),
									   INVALID_PROC_NUMBER);
		}
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	/* in one pass over the buffers, not one per relation under the locks */
	if (nsrels > 0)
	{
		FlushRelationsAllBuffers(srels, nsrels);
		pfree(srels);
	}

	/*
	 * TOAST-kind relations (TOAST tables, change logs) after every table,
	 * in the order a writer takes them: its table first, then those as it
	 * writes a value out of line or an entry.
	 */
	relids = list_concat(relids, toasts);
	list_free(toasts);

	while (OidIsValid(busy = lock_all_or_none(relids)))
	{
		if (start == 0)
			start = GetCurrentTimestamp();
		else if (LockTimeout > 0 &&
				 TimestampDifferenceExceeds(start, GetCurrentTimestamp(),
											LockTimeout))
		{
			char	   *relname = get_rel_name(busy);

			ereport(ERROR,
					(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					 errmsg("could not lock relation \"%s\" of branch \"%s\" to copy it",
							relname ? relname : "?", branch_name(MyBranch)),
					 errdetail("A transaction writing to it was still in progress when \"lock_timeout\" passed."),
					 errhint("Try again once the branch's writers are done, or raise \"lock_timeout\".")));
		}

		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 delay, WAIT_EVENT_BRANCH_COPY_LOCK);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
		delay = Min(delay * 2, COPY_LOCK_MAX_DELAY_MS);
	}

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
			pgstat_count_prolly(PGSTAT_PROLLY_CATALOG_ROWS_COPIED, 1);
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
	List	   *locked;

	/*
	 * (the change logs' entries the transaction holds, of rows on the pages
	 * the copy shares, written first: the copy's log holds them too)
	 */
	prolly_change_logs_flush();
	locked = lock_versioned_relations();

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
 * of the options: divergence D14 in src/tools/prolly_bench/corpus/dolt/
 * divergences.md); "HEAD", in any case, the
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

/* A copy of branch's pg_class row of relation relid (any branch's), or NULL. */
static HeapTuple
class_row(Relation pg_class, Oid relid, Oid branch)
{
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup = NULL;

	ScanKeyInit(&key[0], Anum_pg_class_oid, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&key[1], Anum_pg_class_relbranch, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(branch));
	CatalogAllBranches = true;
	PG_TRY();
	{
		scan = systable_beginscan(pg_class, ClassOidIndexId, true, NULL, 2, key);
		tup = systable_getnext(scan);
		if (HeapTupleIsValid(tup))
			tup = heap_copytuple(tup);
		systable_endscan(scan);
	}
	PG_FINALLY();
	{
		CatalogAllBranches = false;
	}
	PG_END_TRY();
	return tup;
}

/*
 * Copies of the rows a branch-aware catalog has for relation relid on
 * branch, found by an index whose first column is the relation, in its
 * order.
 */
static List *
branch_rows(Oid catalog, Oid index, AttrNumber relattr, Oid relid,
			Oid branch)
{
	Relation	rel = table_open(catalog, AccessShareLock);
	const CatalogBranchInfo *info = catalog_branch_info(catalog);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *rows = NIL;

	ScanKeyInit(&key, relattr, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	CatalogAllBranches = true;
	PG_TRY();
	{
		scan = systable_beginscan(rel, index, true, NULL, 1, &key);
		while ((tup = systable_getnext(scan)) != NULL)
			if (catalog_row_branch(info, tup) == branch)
				rows = lappend(rows, heap_copytuple(tup));
		systable_endscan(scan);
	}
	PG_FINALLY();
	{
		CatalogAllBranches = false;
	}
	PG_END_TRY();
	table_close(rel, AccessShareLock);
	return rows;
}

/* Is column attnum of two rows of catalog the same value? */
static bool
same_column(Oid catalog, HeapTuple a, HeapTuple b, AttrNumber attnum)
{
	Relation	rel = table_open(catalog, AccessShareLock);
	TupleDesc	desc = RelationGetDescr(rel);
	Form_pg_attribute att = TupleDescAttr(desc, attnum - 1);
	bool		anull;
	bool		bnull;
	Datum		av = heap_getattr(a, attnum, desc, &anull);
	Datum		bv = heap_getattr(b, attnum, desc, &bnull);
	bool		same;

	same = anull == bnull &&
		(anull || datum_image_eq(av, bv, att->attbyval, att->attlen));
	table_close(rel, AccessShareLock);
	return same;
}

/*
 * Are relation relid's columns, on the session's branch and on source,
 * stored alike: the same numbers, types, lengths, alignment, storage,
 * compression, collations and dropped columns, and the same values for a
 * column a row too short to hold it is read as having?  A share gives the
 * relation the source's tuples, which are read by the session branch's
 * descriptor: equal definitions don't make equal layouts (a dropped
 * column, still in the tuples written before the drop, is described by
 * neither).
 */
static bool
columns_alike(Oid relid, Oid source)
{
	List	   *ours = branch_rows(AttributeRelationId, AttributeRelidNumIndexId,
								   Anum_pg_attribute_attrelid, relid, MyBranch);
	List	   *theirs = branch_rows(AttributeRelationId, AttributeRelidNumIndexId,
									 Anum_pg_attribute_attrelid, relid, source);
	ListCell   *a;
	ListCell   *b;

	if (list_length(ours) != list_length(theirs))
		return false;
	forboth(a, ours, b, theirs)
	{
		Form_pg_attribute o = (Form_pg_attribute) GETSTRUCT((HeapTuple) lfirst(a));
		Form_pg_attribute t = (Form_pg_attribute) GETSTRUCT((HeapTuple) lfirst(b));

		if (o->attnum != t->attnum || o->atttypid != t->atttypid ||
			o->atttypmod != t->atttypmod || o->attlen != t->attlen ||
			o->attbyval != t->attbyval || o->attalign != t->attalign ||
			o->attstorage != t->attstorage ||
			o->attcompression != t->attcompression ||
			o->attisdropped != t->attisdropped ||
			o->attcollation != t->attcollation ||
			o->atthasmissing != t->atthasmissing ||
			(o->atthasmissing &&
			 !same_column(AttributeRelationId, lfirst(a), lfirst(b),
						  Anum_pg_attribute_attmissingval)))
			return false;
	}
	return true;
}

/*
 * Is index relid defined alike on the session's branch and on source: the
 * same columns, expressions, operator classes, collations, options,
 * predicate and uniqueness?  (Not one the source has rebuilt otherwise.)
 */
static bool
index_alike(Oid relid, Oid source)
{
	static const AttrNumber compared[] = {
		Anum_pg_index_indnatts, Anum_pg_index_indnkeyatts,
		Anum_pg_index_indisunique, Anum_pg_index_indnullsnotdistinct,
		Anum_pg_index_indisprimary, Anum_pg_index_indisexclusion,
		Anum_pg_index_indkey, Anum_pg_index_indcollation,
		Anum_pg_index_indclass, Anum_pg_index_indoption,
		Anum_pg_index_indexprs, Anum_pg_index_indpred,
	};
	List	   *ours = branch_rows(IndexRelationId, IndexRelidIndexId,
								   Anum_pg_index_indexrelid, relid, MyBranch);
	List	   *theirs = branch_rows(IndexRelationId, IndexRelidIndexId,
									 Anum_pg_index_indexrelid, relid, source);

	if (list_length(ours) != 1 || list_length(theirs) != 1)
		return false;
	for (int i = 0; i < lengthof(compared); i++)
		if (!same_column(IndexRelationId, linitial(ours), linitial(theirs),
						 compared[i]))
			return false;
	return true;
}

/*
 * Give the session branch's relations relids the source branch's storage
 * (#491): each a share of the source's (copy_storage()'s), its row taking
 * the source's say of how old its oldest row is and how big it is (the
 * pages are the source's).  The source is held against deletion first
 * (pin_branch()), and each relation must be stored alike on both: the
 * same kind, access method, columns as stored and TOAST table, and an
 * index the same index.  Checked first, all of them, so a refusal changes
 * nothing.
 */
static bool
copies_share(Oid source, List *relids, bool (*unchanged) (void *arg),
			 void *arg)
{
	Relation	pg_class;
	List	   *ours = NIL;
	List	   *theirs = NIL;
	ListCell   *a;
	ListCell   *b;
	bool		alike = true;

	/* (not the storage of a branch being deleted) */
	if (!pin_branch(source))
		return false;

	/* (ours as TRUNCATE locks them; the source's as a copy does, its own) */
	foreach_oid(relid, relids)
	{
		LOCKTAG		tag;

		if (!ConditionalLockRelationOid(relid, AccessExclusiveLock))
			return false;
		SET_LOCKTAG_BRANCH_RELATION(tag, MyDatabaseId, relid, source);
		if (LockAcquire(&tag, ShareLock, false, true) == LOCKACQUIRE_NOT_AVAIL)
			return false;
	}
	AcceptInvalidationMessages();
	if (unchanged && !unchanged(arg))
		return false;

	pg_class = table_open(RelationRelationId, RowExclusiveLock);
	foreach_oid(relid, relids)
	{
		HeapTuple	o = class_row(pg_class, relid, MyBranch);
		HeapTuple	t = class_row(pg_class, relid, source);
		Form_pg_class oc;
		Form_pg_class tc;

		if (o == NULL || t == NULL)
		{
			alike = false;
			break;
		}
		oc = (Form_pg_class) GETSTRUCT(o);
		tc = (Form_pg_class) GETSTRUCT(t);
		if (oc->relkind != tc->relkind || !RELKIND_HAS_STORAGE(oc->relkind) ||
			oc->relam != tc->relam || oc->relnatts != tc->relnatts ||
			oc->reltoastrelid != tc->reltoastrelid ||
			!columns_alike(relid, source) ||
			(oc->relkind == RELKIND_INDEX && !index_alike(relid, source)) ||
			oc->relpersistence != RELPERSISTENCE_PERMANENT ||
			tc->relpersistence != RELPERSISTENCE_PERMANENT ||
			oc->reltablespace != tc->reltablespace ||
			!RelFileNumberIsValid(oc->relfilenode) ||
			!RelFileNumberIsValid(tc->relfilenode) ||
			RelFileLocatorSkippingWAL(class_locator(tc, tc->relfilenode)) ||
			!smgrcanshare(smgropen(class_locator(tc, tc->relfilenode),
								   INVALID_PROC_NUMBER)))
		{
			alike = false;
			break;
		}
		ours = lappend(ours, o);
		theirs = lappend(theirs, t);
	}
	if (!alike)
	{
		table_close(pg_class, RowExclusiveLock);
		return false;
	}
	forboth(a, ours, b, theirs)
	{
		HeapTuple	o = lfirst(a);
		Form_pg_class oc = (Form_pg_class) GETSTRUCT(o);
		Form_pg_class tc = (Form_pg_class) GETSTRUCT((HeapTuple) lfirst(b));
		RelFileNumber relnumber;

		relnumber = GetNewRelFileNumber(oc->reltablespace, NULL,
										oc->relpersistence);
		RelationCreateStorageShared(class_locator(oc, relnumber),
									smgropen(class_locator(tc, tc->relfilenode),
											 INVALID_PROC_NUMBER),
									true);
		RelationDropStorageByLocator(class_locator(oc, oc->relfilenode));
		oc->relfilenode = relnumber;
		oc->relpages = tc->relpages;
		oc->reltuples = tc->reltuples;
		oc->relallvisible = tc->relallvisible;
		oc->relallfrozen = tc->relallfrozen;
		oc->relfrozenxid = tc->relfrozenxid;
		oc->relminmxid = tc->relminmxid;
		CatalogTupleUpdate(pg_class, &o->t_self, o);
	}
	CommandCounterIncrement();
	table_close(pg_class, RowExclusiveLock);
	return true;
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
	.share = copies_share,
};

const BranchOpsRoutine *
branch_ops(void)
{
	return &branch_copies;
}
