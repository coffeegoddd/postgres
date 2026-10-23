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
#include "access/multixact.h"
#include "access/relation.h"
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
#include "commands/trigger.h"
#include "catalog/versioned.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bmap.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/proc.h"
#include "storage/smgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
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
 * Give the session branch's relations relids the source branch's storage
 * (#491): each a share of the source's (copy_storage()'s), its row taking
 * the source's say of how old its oldest row is and how big it is (the
 * pages are the source's).  Checked first, all of them, so a refusal
 * changes nothing.
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

/*
 * Keeping a branch's pages (keep(), restore()).  A snapshot of a relation's
 * block-mapped storage (bmap_snapshot_take()) is in the storage, not the
 * transaction: one taken by a transaction (or subtransaction) that aborts
 * would be kept by nobody, so each taken is remembered until its
 * transaction commits, and one whose transaction aborted is let go of the
 * next time this backend keeps or restores (letting go writes, which an
 * abort can't), or, if it never does, as it exits.  (A crash in between
 * leaves the snapshot to its storage, until the relation is dropped.)
 */
typedef struct TakenSnapshot
{
	RelFileLocator locator;
	uint64		id;
	int			level;			/* the (sub)transaction's nesting level */
} TakenSnapshot;

static List *taken = NIL;		/* TakenSnapshot *, in TopMemoryContext */
static List *abandoned = NIL;	/* and those to let go of */
static bool taken_callbacks = false;

static void
taken_xact(XactEvent event, void *arg)
{
	MemoryContext old;

	if (taken == NIL)
		return;
	if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_PARALLEL_COMMIT ||
		event == XACT_EVENT_PREPARE)
	{
		list_free_deep(taken);
		taken = NIL;
	}
	else if (event == XACT_EVENT_ABORT || event == XACT_EVENT_PARALLEL_ABORT)
	{
		old = MemoryContextSwitchTo(TopMemoryContext);
		abandoned = list_concat(abandoned, taken);
		MemoryContextSwitchTo(old);
		list_free(taken);
		taken = NIL;
	}
}

static void
taken_subxact(SubXactEvent event, SubTransactionId mySubid,
			  SubTransactionId parentSubid, void *arg)
{
	int			level = GetCurrentTransactionNestLevel();
	MemoryContext old;

	foreach_ptr(TakenSnapshot, t, taken)
	{
		if (t->level < level)
			continue;
		if (event == SUBXACT_EVENT_ABORT_SUB)
		{
			old = MemoryContextSwitchTo(TopMemoryContext);
			abandoned = lappend(abandoned, t);
			MemoryContextSwitchTo(old);
			taken = foreach_delete_current(taken, t);
		}
		else if (event == SUBXACT_EVENT_COMMIT_SUB)
			t->level = level - 1;
	}
}

static void let_go_at_exit(int code, Datum arg);

static void
note_taken(RelFileLocator locator, uint64 id)
{
	MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);
	TakenSnapshot *t = palloc(sizeof(TakenSnapshot));

	if (!taken_callbacks)
	{
		RegisterXactCallback(taken_xact, NULL);
		RegisterSubXactCallback(taken_subxact, NULL);
		before_shmem_exit(let_go_at_exit, (Datum) 0);
		taken_callbacks = true;
	}
	t->locator = locator;
	t->id = id;
	t->level = GetCurrentTransactionNestLevel();
	taken = lappend(taken, t);
	MemoryContextSwitchTo(old);
}

/* Let go of a snapshot of storage locator, if it still has it. */
static void
let_go_of(RelFileLocator locator, uint64 id)
{
	if (id != 0 && bmap_relation_exists(locator))
		(void) bmap_snapshot_release(smgropen(locator, INVALID_PROC_NUMBER), id);
}

static void let_go_of_abandoned(void);

/*
 * As the backend exits: the snapshots of its aborted transactions (and of
 * the one it is in, aborted now), let go of in a transaction of their own.
 * Before the backend leaves its database; a failure is reported, not
 * raised, the snapshots left to their storage.
 */
static void
let_go_at_exit(int code, Datum arg)
{
	if (taken == NIL && abandoned == NIL)
		return;
	AbortOutOfAnyTransaction();
	if (abandoned == NIL)
		return;
	PG_TRY();
	{
		StartTransactionCommand();
		let_go_of_abandoned();
		CommitTransactionCommand();
	}
	PG_CATCH();
	{
		EmitErrorReport();
		FlushErrorState();
		AbortOutOfAnyTransaction();
	}
	PG_END_TRY();
}

/* The snapshots of aborted (sub)transactions, let go of. */
static void
let_go_of_abandoned(void)
{
	while (abandoned != NIL)
	{
		TakenSnapshot *t = linitial(abandoned);

		abandoned = list_delete_first(abandoned);
		let_go_of(t->locator, t->id);
		pfree(t);
	}
}

static RelFileLocator
kept_locator(const BranchKeptRelation *k)
{
	RelFileLocator loc;

	loc.spcOid = OidIsValid(k->tablespace) ? k->tablespace : MyDatabaseTableSpace;
	loc.dbOid = MyDatabaseId;
	loc.relNumber = k->relfilenumber;
	return loc;
}

/* (a relation keeping more than this many snapshots is kept by none more) */
#define KEEP_SNAPSHOTS_MAX	(BMAP_SNAPSHOTS_MAX - 4)

/* Keep a relation's pages as they are, by a new snapshot. */
static void
keep_now(Form_pg_class c, BranchKeptRelation *k)
{
	RelFileLocator loc = class_locator(c, c->relfilenode);
	SMgrRelation srel = smgropen(loc, INVALID_PROC_NUMBER);

	k->tablespace = c->reltablespace;
	k->relfilenumber = c->relfilenode;
	k->frozenxid = c->relfrozenxid;
	k->minmxid = c->relminmxid;
	k->snapshot = bmap_snapshot_take(srel);
	note_taken(loc, k->snapshot);
	pgstat_count_prolly(PGSTAT_PROLLY_PAGES_KEPT, 1);
}

/* Can relation c's storage keep its pages, by one snapshot more? */
static bool
can_keep(Form_pg_class c)
{
	RelFileLocator loc;
	BmapSnapshot snaps[BMAP_SNAPSHOTS_MAX];

	if (!RELKIND_HAS_STORAGE(c->relkind) ||
		c->relpersistence != RELPERSISTENCE_PERMANENT ||
		!RelFileNumberIsValid(c->relfilenode))
		return false;
	loc = class_locator(c, c->relfilenode);
	return !RelFileLocatorSkippingWAL(loc) && bmap_relation_exists(loc) &&
		bmap_snapshot_list(smgropen(loc, INVALID_PROC_NUMBER), snaps,
						   BMAP_SNAPSHOTS_MAX) <= KEEP_SNAPSHOTS_MAX;
}

/*
 * Keep the pages of the session branch's relations relids (see
 * branch_ops.h).
 */
static bool
copies_keep(List *relids, const BranchKeptRelation *prior, int nprior,
			BranchKeptRelation *kept, bool (*settled) (void *arg), void *arg)
{
	Relation	pg_class;
	List	   *rows = NIL;
	bool		ok = true;
	int			i = 0;

	let_go_of_abandoned();
	if (relids == NIL)
		return true;
	if (OidIsValid(lock_all_or_none(relids)))
		return false;
	PG_TRY();
	{
		AcceptInvalidationMessages();
		if (settled && !settled(arg))
			ok = false;

		/* (each checked before any is kept) */
		pg_class = table_open(RelationRelationId, AccessShareLock);
		foreach_oid(relid, relids)
		{
			HeapTuple	t;

			if (!ok)
				break;
			t = class_row(pg_class, relid, MyBranch);
			if (t == NULL || !can_keep((Form_pg_class) GETSTRUCT(t)))
				ok = false;
			if (t != NULL)
				rows = lappend(rows, t);
		}
		table_close(pg_class, AccessShareLock);

		foreach_ptr(HeapTupleData, t, rows)
		{
			Form_pg_class c = (Form_pg_class) GETSTRUCT(t);
			BranchKeptRelation *k = &kept[i++];
			bool		again = false;

			if (!ok)
				break;
			memset(k, 0, sizeof(*k));
			k->relid = c->oid;
			for (int p = 0; p < nprior && !again; p++)
			{
				if (prior[p].relid == c->oid &&
					prior[p].relfilenumber == c->relfilenode &&
					prior[p].tablespace == c->reltablespace &&
					bmap_snapshot_unchanged(smgropen(class_locator(c, c->relfilenode),
													 INVALID_PROC_NUMBER),
											prior[p].snapshot))
				{
					*k = prior[p];
					again = true;
				}
			}
			if (!again)
				keep_now(c, k);
		}
	}
	PG_FINALLY();
	{
		foreach_oid(relid, relids)
			UnlockRelationOid(relid, ShareLock);
	}
	PG_END_TRY();
	list_free_deep(rows);
	return ok;
}

/*
 * Is relation c (its row k) still on the branch with the storage that kept
 * it, its snapshot there, and its rows' transaction ids ones the database
 * still knows?
 */
static bool
can_restore(Form_pg_class c, const BranchKeptRelation *k,
			TransactionId datfrozenxid, MultiXactId datminmxid)
{
	BmapSnapshot snaps[BMAP_SNAPSHOTS_MAX];
	int			n;

	if (c->relfilenode != k->relfilenumber ||
		c->reltablespace != k->tablespace ||
		c->relpersistence != RELPERSISTENCE_PERMANENT ||
		!bmap_relation_exists(kept_locator(k)) ||
		(TransactionIdIsNormal(k->frozenxid) &&
		 TransactionIdPrecedes(k->frozenxid, datfrozenxid)) ||
		(MultiXactIdIsValid(k->minmxid) &&
		 MultiXactIdPrecedes(k->minmxid, datminmxid)))
		return false;
	n = bmap_snapshot_list(smgropen(kept_locator(k), INVALID_PROC_NUMBER),
						   snaps, BMAP_SNAPSHOTS_MAX);
	for (int s = 0; s < Min(n, BMAP_SNAPSHOTS_MAX); s++)
	{
		if (snaps[s].id == k->snapshot)
			return true;
	}
	return false;
}

/*
 * Give the session branch's relations kept[] back the pages keep() kept
 * (see branch_ops.h).
 */
static bool
copies_restore(const BranchKeptRelation *kept, int nkept,
			   BranchKeptRelation *renewed)
{
	Relation	pg_class;
	HeapTuple	dbtup;
	TransactionId datfrozenxid;
	MultiXactId datminmxid;
	HeapTuple  *rows = palloc0_array(HeapTuple, Max(nkept, 1));
	bool	   *changed = palloc0_array(bool, Max(nkept, 1));
	bool		any = false;
	bool		ok = true;

	let_go_of_abandoned();
	dbtup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
	if (!HeapTupleIsValid(dbtup))
		elog(ERROR, "cache lookup failed for database %u", MyDatabaseId);
	datfrozenxid = ((Form_pg_database) GETSTRUCT(dbtup))->datfrozenxid;
	datminmxid = ((Form_pg_database) GETSTRUCT(dbtup))->datminmxid;
	ReleaseSysCache(dbtup);

	pg_class = table_open(RelationRelationId, RowExclusiveLock);

	/* (each checked before any is restored) */
	for (int i = 0; i < nkept && ok; i++)
	{
		rows[i] = class_row(pg_class, kept[i].relid, MyBranch);
		if (rows[i] == NULL ||
			!can_restore((Form_pg_class) GETSTRUCT(rows[i]), &kept[i],
						 datfrozenxid, datminmxid))
			ok = false;
		else if (!bmap_snapshot_unchanged(smgropen(kept_locator(&kept[i]),
												   INVALID_PROC_NUMBER),
										  kept[i].snapshot))
			any = true;
	}

	/*
	 * If any changed, every one is given new storage, locked first as
	 * TRUNCATE locks it: one whose pages didn't change may still hold, in
	 * its buffers, hints true of it as it is alone (an index entry dead
	 * because its row is), which the storage keeping the pages never got.
	 */
	for (int i = 0; i < nkept && ok && any; i++)
	{
		if (!ConditionalLockRelationOid(kept[i].relid, AccessExclusiveLock))
			ok = false;
		changed[i] = true;
	}

	/*
	 * (nor while this session still uses one's storage, as TRUNCATE
	 * refuses to: a scan of it open, or an AFTER trigger's event queued
	 * holding a location in it, which would then name a row of the restored
	 * pages)
	 */
	for (int i = 0; i < nkept && ok && any; i++)
	{
		Relation	rel = relation_open(kept[i].relid, NoLock);

		if (rel->rd_refcnt != 1 || AfterTriggerPendingOnRel(kept[i].relid))
			ok = false;
		relation_close(rel, NoLock);
	}

	for (int i = 0; i < nkept && ok; i++)
	{
		const BranchKeptRelation *k = &kept[i];
		HeapTuple	o = rows[i];
		Form_pg_class oc = (Form_pg_class) GETSTRUCT(o);
		RelFileNumber relnumber;
		SMgrRelation nrel;

		renewed[i] = *k;
		if (!changed[i])
			continue;
		relnumber = GetNewRelFileNumber(oc->reltablespace, NULL,
										oc->relpersistence);
		nrel = RelationCreateStorageFromSnapshot(class_locator(oc, relnumber),
												 smgropen(kept_locator(k),
														  INVALID_PROC_NUMBER),
												 k->snapshot, true);
		renewed[i].relfilenumber = relnumber;
		renewed[i].snapshot = bmap_snapshot_take(nrel);
		note_taken(class_locator(oc, relnumber), renewed[i].snapshot);
		RelationDropStorageByLocator(class_locator(oc, oc->relfilenode));
		oc->relfilenode = relnumber;
		if (TransactionIdIsNormal(k->frozenxid))
			oc->relfrozenxid = k->frozenxid;
		if (MultiXactIdIsValid(k->minmxid))
			oc->relminmxid = k->minmxid;
		oc->relallvisible = 0;
		oc->relallfrozen = 0;
		CatalogTupleUpdate(pg_class, &o->t_self, o);
		pgstat_count_prolly(PGSTAT_PROLLY_RELATIONS_RESTORED, 1);
	}
	if (ok && any)
		CommandCounterIncrement();
	table_close(pg_class, RowExclusiveLock);
	for (int i = 0; i < nkept; i++)
	{
		if (rows[i])
			heap_freetuple(rows[i]);
	}
	pfree(rows);
	pfree(changed);
	return ok;
}

static void
copies_let_go(const BranchKeptRelation *kept)
{
	let_go_of(kept_locator(kept), kept->snapshot);
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
	.keep = copies_keep,
	.restore = copies_restore,
	.let_go = copies_let_go,
};

const BranchOpsRoutine *
branch_ops(void)
{
	return &branch_copies;
}
