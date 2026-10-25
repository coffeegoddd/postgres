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
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_branch.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_index.h"
#include "catalog/pg_namespace.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "commands/trigger.h"
#include "catalog/versioned.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
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
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/regproc.h"
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

static HeapTuple class_row(Relation pg_class, Oid relid, Oid branch);

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
 * Try 'mode' on relation relid, as ConditionalLockRelationOid() does, but
 * waiting out a page sweep that holds it: asked, it lets go after the move
 * under way (bmap_compaction_let_go()), as it does for a waiter in the
 * lock's queue, so a hold of a moment doesn't turn the try away.  Any
 * other holder does.  *start is the operation's (see BranchOpsRoutine's
 * share()).
 */
static bool
try_lock_relation(Oid relid, LOCKMODE mode, TimestampTz *start)
{
	LOCKTAG		tag;

	SET_LOCKTAG_BRANCH_RELATION(tag, MyDatabaseId, relid,
								relation_lock_branch(relid));
	while (!ConditionalLockRelationOid(relid, mode))
	{
		if (!bmap_compaction_let_go(&tag, start))
			return ConditionalLockRelationOid(relid, mode);
	}
	return true;
}

/* the longest wait between tries at the copy's locks, in milliseconds */
#define COPY_LOCK_MAX_DELAY_MS	100

/*
 * Take 'mode' on every one of relids, or on none: a lock that would wait
 * gives back those already taken.  Returns the first relation that
 * couldn't be locked, or InvalidOid once all are.
 */
static Oid
lock_all_or_none(List *relids, LOCKMODE mode)
{
	int			ntaken = 0;
	Oid			busy = InvalidOid;
	ListCell   *lc;

	foreach(lc, relids)
	{
		if (!ConditionalLockRelationOid(lfirst_oid(lc), mode))
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
			UnlockRelationOid(lfirst_oid(lc), mode);
		}
	return busy;
}

/*
 * The session branch's versioned relations, TOAST-kind relations (TOAST
 * tables, change logs) after every table, in the order a writer takes
 * them: its table first, then those as it writes a value out of line or an
 * entry.
 */
static List *
branch_relations(void)
{
	Relation	rel = table_open(RelationRelationId, AccessShareLock);
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *relids = NIL;
	List	   *toasts = NIL;

	scan = catalog_branch_beginscan(rel, MyBranch);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_class c = (Form_pg_class) GETSTRUCT(tup);

		Assert(c->relbranch == MyBranch);
		if (c->relkind == RELKIND_TOASTVALUE)
			toasts = lappend_oid(toasts, c->oid);
		else
			relids = lappend_oid(relids, c->oid);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	relids = list_concat(relids, toasts);
	list_free(toasts);
	return relids;
}

/*
 * Write out the session branch's dirty pages of relids, in one pass over
 * the buffers.
 */
static void
flush_relations(List *relids)
{
	Relation	pg_class = table_open(RelationRelationId, AccessShareLock);
	SMgrRelation *srels = palloc_array(SMgrRelation, Max(list_length(relids), 1));
	int			nsrels = 0;

	foreach_oid(relid, relids)
	{
		HeapTuple	t = class_row(pg_class, relid, MyBranch);
		Form_pg_class c;

		if (t == NULL)
			continue;
		c = (Form_pg_class) GETSTRUCT(t);
		if (RELKIND_HAS_STORAGE(c->relkind) &&
			c->relpersistence == RELPERSISTENCE_PERMANENT &&
			RelFileNumberIsValid(c->relfilenode))
			srels[nsrels++] = smgropen(class_locator(c, c->relfilenode),
									   INVALID_PROC_NUMBER);
		heap_freetuple(t);
	}
	table_close(pg_class, AccessShareLock);
	if (nsrels > 0)
		FlushRelationsAllBuffers(srels, nsrels);
	pfree(srels);
}

/*
 * Take 'mode' on each of the session branch's relations relids, all at
 * once: tried, not waited for.  A lock in the lock queue would hold up
 * every writer that comes after it (one that conflicts with writers), for
 * as long as the writers before it take, and then for the copy.  So all
 * are taken at once or none is, and the try is repeated, a little later
 * each time, until they are (writers coming and going meanwhile go ahead),
 * or lock_timeout passes (0, the default, waits for as long as it takes).
 * Nor does the deadlock detector see this wait: lock_timeout or
 * statement_timeout bounds it.  The catalogs are read again after, as the
 * locks left them.  nowait: tried once; false, none taken, if one is held.
 */
static bool
lock_relations(List *relids, LOCKMODE mode, bool nowait)
{
	TimestampTz start = 0;
	long		delay = 1;
	Oid			busy;

	while (OidIsValid(busy = lock_all_or_none(relids, mode)))
	{
		if (nowait)
			return false;
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
	InvalidateCatalogSnapshot();
	return true;
}

/*
 * Lock the session branch's versioned relations against changes, so the
 * copy is of a state no writer is in the middle of (lock_relations()).
 * Returns them, for unlock_relations().  The source's dirty pages are
 * written first, outside the locks, so that what the copy writes under
 * them is only what changed meanwhile.  nowait: tried once; if a writer
 * holds one, none is taken and *busy set.
 */
static List *
lock_versioned_relations(bool nowait, bool *busy)
{
	List	   *relids = branch_relations();

	flush_relations(relids);
	if (!lock_relations(relids, ShareLock, nowait))
	{
		*busy = true;
		list_free(relids);
		return NIL;
	}
	return relids;
}

/* Let go of 'mode' on relids (only those). */
static void
unlock_relations(List *relids, LOCKMODE mode)
{
	ListCell   *lc;

	foreach(lc, relids)
		UnlockRelationOid(lfirst_oid(lc), mode);
}

/*
 * The relations of the session branch's tables named in emptied whose
 * copies begin with no rows: each plain table's, its TOAST table's and
 * both's indexes.  (Its change log is shared still: it is told the table
 * was emptied.)  Returns the tables, adding them all to *relids.
 */
static List *
emptied_relations(List *emptied, List **relids)
{
	List	   *tables = NIL;
	ListCell   *lc;

	foreach(lc, emptied)
	{
		Oid			relid;
		Oid			toast;
		List	   *rels;

		relid = RangeVarGetRelid(makeRangeVarFromNameList(stringToQualifiedNameList(lfirst(lc), NULL)),
								 NoLock, true);
		if (!OidIsValid(relid) || get_rel_relkind(relid) != RELKIND_RELATION ||
			list_member_oid(tables, relid))
			continue;
		tables = lappend_oid(tables, relid);
		rels = list_make1_oid(relid);
		{
			Relation	r = table_open(relid, AccessShareLock);

			toast = r->rd_rel->reltoastrelid;
			table_close(r, AccessShareLock);
		}
		if (OidIsValid(toast))
			rels = lappend_oid(rels, toast);
		foreach_oid(rel, rels)
		{
			Relation	r = table_open(rel, AccessShareLock);

			*relids = list_concat(*relids, RelationGetIndexList(r));
			table_close(r, AccessShareLock);
		}
		*relids = list_concat(*relids, rels);
	}
	return tables;
}

/*
 * Storage of a relation's copy made of the pages kept by snapshot k of
 * its storage: O(1).  If the relation changed since, its row takes the
 * oldest transaction ids they hold, and claims no page all-visible (the
 * copy has no visibility map); if not, the copy gets its maps too, as a
 * copy of the storage would, and its row says what the source's does.
 */
static RelFileNumber
copy_kept_storage(Form_pg_class c, const BranchKeptRelation *k)
{
	RelFileNumber relnumber = GetNewRelFileNumber(c->reltablespace, NULL,
												  c->relpersistence);
	SMgrRelation src = smgropen(class_locator(c, c->relfilenode),
								INVALID_PROC_NUMBER);

	/*
	 * (the source unchanged since: its maps are true of the pages kept, and
	 * held against VACUUM no bit is set meanwhile, while a writer only
	 * clears one, of a page whose kept version had it)
	 */
	if (bmap_snapshot_unchanged(src, k->snapshot))
	{
		(void) RelationCreateStorageFromSnapshot(class_locator(c, relnumber),
												 src, k->snapshot, true, false,
												 true);
		return relnumber;
	}
	(void) RelationCreateStorageFromSnapshot(class_locator(c, relnumber), src,
											 k->snapshot, false, false, true);
	if (TransactionIdIsNormal(k->frozenxid))
		c->relfrozenxid = k->frozenxid;
	if (MultiXactIdIsValid(k->minmxid))
		c->relminmxid = k->minmxid;
	c->relallvisible = 0;
	c->relallfrozen = 0;
	return relnumber;
}

/* The one of kept[] (nkept of them) of relation relid, or NULL. */
static const BranchKeptRelation *
kept_of(const BranchKeptRelation *const *kept, int nkept, Oid relid)
{
	for (int i = 0; i < nkept; i++)
	{
		if (kept[i]->relid == relid)
			return kept[i];
	}
	return NULL;
}

/*
 * Copy every row of the session branch into branch 'target'; the
 * relations in fresh get new, empty storage rather than a share of the
 * session branch's, a relation of kept[] (nkept of them) storage made of
 * the pages kept, the others a copy of their storage as it is.
 */
static void
copy_rows(Oid target, List *fresh, const BranchKeptRelation *const *kept,
		  int nkept)
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
		scan = catalog_branch_beginscan(rel, MyBranch);
		while ((tup = systable_getnext(scan)) != NULL)
		{
			HeapTuple	copy;

			Assert(catalog_row_branch(info, tup) == MyBranch);

			/* out-of-line values get their own copies, as rows of their own */
			copy = HeapTupleHasExternal(tup) ? toast_flatten_tuple(tup, desc)
				: heap_copytuple(tup);
			memcpy((char *) GETSTRUCT(copy) + info->offset, &target, sizeof(Oid));

			if (info->relid == RelationRelationId)
			{
				Form_pg_class c = (Form_pg_class) GETSTRUCT(copy);

				if (RELKIND_HAS_STORAGE(c->relkind))
				{
					const BranchKeptRelation *k = kept_of(kept, nkept, c->oid);

					if (!RelFileNumberIsValid(c->relfilenode))
						elog(ERROR, "versioned relation \"%s\" is mapped",
							 NameStr(c->relname));
					if (list_member_oid(fresh, c->oid))
					{
						RelFileNumber relnumber;

						relnumber = GetNewRelFileNumber(c->reltablespace, NULL,
														c->relpersistence);
						(void) RelationCreateStorage(class_locator(c, relnumber),
													 c->relpersistence, true);
						c->relfilenode = relnumber;
					}
					else
						c->relfilenode = k ? copy_kept_storage(c, k) : copy_storage(c);
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

		scan = catalog_branch_beginscan(rel, MyBranch);
		while ((tup = systable_getnext(scan)) != NULL)
		{
			Assert(catalog_row_branch(info, tup) == MyBranch);
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

/*
 * A branch's copies to make: the branch's id, the tables to empty, and,
 * nowait, whether a writer held them up; with tables (ntables of them),
 * made of the pages kept of those still kept (used[] says which), as
 * copy_from_kept() makes them.
 */
typedef struct CopyTo
{
	Oid			branch;
	List	   *emptied;		/* names, then the tables' ids */
	bool		nowait;
	bool		busy;
	const BranchKeptTable *tables;
	int			ntables;
	bool	   *used;
} CopyTo;

static void copy_from_kept(CopyTo *ct);

static void
copy_from_here(void *arg)
{
	CopyTo	   *ct = arg;
	List	   *locked;
	List	   *fresh = NIL;

	if (ct->tables != NULL)
	{
		copy_from_kept(ct);
		return;
	}

	/*
	 * (the change logs' entries the transaction holds, of rows on the pages
	 * the copy shares, written first: the copy's log holds them too)
	 */
	prolly_change_logs_flush();
	locked = lock_versioned_relations(ct->nowait, &ct->busy);
	if (ct->busy)
		return;

	ct->emptied = emptied_relations(ct->emptied, &fresh);
	copy_rows(ct->branch, fresh, NULL, 0);
	copyBranchSharedDependencies(MyBranch, ct->branch);
	unlock_relations(locked, ShareLock);
	list_free(locked);
}

static bool can_restore(Form_pg_class c, const BranchKeptRelation *k);

/*
 * Is kept relation k still the session branch's relation with the storage
 * that keeps it, its snapshot there, and its pages' transaction ids ones
 * the database still knows (can_restore())?
 */
static bool
still_kept(Relation pg_class, const BranchKeptRelation *k)
{
	HeapTuple	t = class_row(pg_class, k->relid, MyBranch);
	bool		ok;

	if (t == NULL)
		return false;
	ok = can_restore((Form_pg_class) GETSTRUCT(t), k);
	heap_freetuple(t);
	return ok;
}

/*
 * The copy of the session branch (the source) made of the pages kept of
 * its tables, where they are still kept: each such table's relations then
 * share a snapshot's pages, which no writer changes, so the source's
 * writers aren't locked out for them.  Every relation of the source is
 * locked against definition changes, VACUUM and anything letting go of
 * kept pages (ShareUpdateExclusiveLock, which writers don't take), all at
 * once and tried, never queued for; only the relations not copied from
 * kept pages (a table whose storage was replaced since, a sequence) are
 * locked against writers too, as a copy locks them, and their pages
 * written out first.  Of the tables emptied names, those not made of kept
 * pages begin empty, as a copy's do (the others are their head already).
 * nowait: tried once each; if one is held, nothing is made, ct->busy set.
 */
static void
copy_from_kept(CopyTo *ct)
{
	List	   *all;
	List	   *rest = NIL;
	List	   *fresh = NIL;
	const BranchKeptRelation **kept;
	int			nkept = 0;
	int			max = 0;
	Relation	pg_class;

	prolly_change_logs_flush();
	all = branch_relations();
	if (!lock_relations(all, ShareUpdateExclusiveLock, ct->nowait))
	{
		ct->busy = true;
		list_free(all);
		return;
	}

	pg_class = table_open(RelationRelationId, AccessShareLock);
	for (int t = 0; t < ct->ntables; t++)
		max += ct->tables[t].nrels;
	kept = palloc_array(const BranchKeptRelation *, Max(max, 1));
	for (int t = 0; t < ct->ntables; t++)
	{
		const BranchKeptTable *kt = &ct->tables[t];

		ct->used[t] = true;
		for (int r = 0; r < kt->nrels && ct->used[t]; r++)
			ct->used[t] = list_member_oid(all, kt->rels[r].relid) &&
				still_kept(pg_class, &kt->rels[r]);
		if (ct->used[t])
			for (int r = 0; r < kt->nrels; r++)
				kept[nkept++] = &kt->rels[r];
	}
	table_close(pg_class, AccessShareLock);
	foreach_oid(relid, all)
	{
		if (kept_of(kept, nkept, relid) == NULL)
			rest = lappend_oid(rest, relid);
	}

	/* (an error lets go of the locks as the transaction aborts) */
	flush_relations(rest);
	if (!lock_relations(rest, ShareLock, ct->nowait))
	{
		ct->busy = true;
		unlock_relations(all, ShareUpdateExclusiveLock);
		list_free(all);
		list_free(rest);
		pfree(kept);
		return;
	}
	ct->emptied = emptied_relations(ct->emptied, &fresh);
	{
		List	   *empty = NIL;

		/* (a table made of kept pages is its head already: not emptied) */
		foreach_oid(relid, ct->emptied)
		{
			if (kept_of(kept, nkept, relid) == NULL)
				empty = lappend_oid(empty, relid);
		}
		foreach_oid(relid, fresh)
		{
			if (kept_of(kept, nkept, relid) != NULL)
				fresh = foreach_delete_current(fresh, relid);
		}
		ct->emptied = empty;
	}
	copy_rows(ct->branch, fresh, kept, nkept);
	copyBranchSharedDependencies(MyBranch, ct->branch);
	unlock_relations(rest, ShareLock);
	unlock_relations(all, ShareUpdateExclusiveLock);
	list_free(all);
	list_free(rest);
	pfree(kept);
}

/*
 * Each of the new branch's copies of tables (their storage new and empty,
 * copy_rows()'s fresh) made valid as TRUNCATE leaves a table, under the
 * ACCESS EXCLUSIVE lock it takes: the table and its TOAST table given new
 * storage in the transaction (so a load into it builds its indexes once,
 * after its rows), each index built over no rows, and the change log told
 * the table was emptied.
 */
static void
empty_here(void *arg)
{
	ReindexParams params = {0};

	foreach_oid(relid, (List *) arg)
	{
		Relation	rel = table_open(relid, AccessExclusiveLock);
		Oid			toast = rel->rd_rel->reltoastrelid;

		RelationSetNewRelfilenumber(rel, rel->rd_rel->relpersistence);
		if (OidIsValid(toast))
		{
			Relation	t = table_open(toast, AccessExclusiveLock);

			RelationSetNewRelfilenumber(t, t->rd_rel->relpersistence);
			table_close(t, NoLock);
		}
		(void) reindex_relation(NULL, relid, REINDEX_REL_PROCESS_TOAST, &params);
		prolly_note_emptied(rel);
		table_close(rel, NoLock);
	}
	CommandCounterIncrement();
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

/*
 * Make branch name, the copies of branch source's (ct says how), its
 * pg_branch row first; InvalidOid, nothing made, if ct->nowait and a
 * writer held them up.
 */
static Oid
make_branch(const char *name, Oid source, CopyTo *ct)
{
	Relation	brel;
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

	branch = GetNewOidWithIndex(brel, BranchOidIndexId, Anum_pg_branch_oid);
	namestrcpy(&brname, name);
	values[Anum_pg_branch_oid - 1] = ObjectIdGetDatum(branch);
	values[Anum_pg_branch_brname - 1] = NameGetDatum(&brname);
	values[Anum_pg_branch_brownerrole - 1] = ObjectIdGetDatum(InvalidOid);
	nulls[Anum_pg_branch_browner - 1] = true;	/* (a new branch is clean) */
	tup = heap_form_tuple(RelationGetDescr(brel), values, nulls);
	CatalogTupleInsert(brel, tup);
	CommandCounterIncrement();

	ct->branch = branch;
	on_branch(source, copy_from_here, ct);
	if (ct->busy)
	{
		/* (nothing copied: the row taken back, as if never made) */
		CatalogTupleDelete(brel, &tup->t_self);
		heap_freetuple(tup);
		CommandCounterIncrement();
		table_close(brel, NoLock);
		return InvalidOid;
	}
	heap_freetuple(tup);
	CommandCounterIncrement();
	note_new_branch(branch);
	if (ct->emptied != NIL)
		on_branch(branch, empty_here, ct->emptied);

	table_close(brel, NoLock);
	return branch;
}

static Oid
copies_make(const char *name, const char *start_point, List *emptied,
			bool nowait)
{
	CopyTo		ct = {0};

	ct.emptied = emptied;
	ct.nowait = nowait;
	return make_branch(name, start_point ? branch_oid(start_point, false) : MyBranch,
					   &ct);
}

/*
 * Make a branch as make() does, of source's kept pages of tables[] where
 * they are still kept (used[], for each, says whether it was).
 */
static Oid
copies_make_kept(const char *name, const char *source,
				 const BranchKeptTable *tables, int ntables, bool *used,
				 List *emptied, bool nowait)
{
	CopyTo		ct = {0};

	ct.emptied = emptied;
	ct.nowait = nowait;
	ct.tables = tables;
	ct.ntables = ntables;
	ct.used = used;
	return make_branch(name, branch_oid(source, false), &ct);
}

static Oid
copies_create(const char *name, const char *start_point)
{
	copies_check_create(name);
	return copies_make(name, start_point, NIL, false);
}

/* (deleting a branch moves its head away: the head move's privileges) */
static void
copies_check_drop(const char *name)
{
	PreventCommandIfReadOnly("dolt_branch()");
	prolly_check_head_move(name);
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
				 errmsg("branch \"%s\" is checked out in another session", name),
				 errdetail("Another session is on it, or holds it for a moment (a page sweep reading its maps).")));
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

	/* (the branch moved away from its name, and a new name taken) */
	copies_check_drop(name);
	prolly_check_branch_rename(newname);
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
				 errdetail("Another session is on it, or holds it for a moment (a page sweep reading its maps)."),
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
 * Is relation relid stored alike on the session's branch (its row oc) and
 * on source (tc): the same kind, access method, columns as stored and
 * TOAST table, an index the same index, both permanent and in the same
 * tablespace?  Tuples from one are then read rightly by the other's
 * descriptor.
 */
static bool
stored_alike(Oid relid, Oid source, Form_pg_class oc, Form_pg_class tc)
{
	return oc->relkind == tc->relkind && RELKIND_HAS_STORAGE(oc->relkind) &&
		oc->relam == tc->relam && oc->relnatts == tc->relnatts &&
		oc->reltoastrelid == tc->reltoastrelid &&
		columns_alike(relid, source) &&
		(oc->relkind != RELKIND_INDEX || index_alike(relid, source)) &&
		oc->relpersistence == RELPERSISTENCE_PERMANENT &&
		tc->relpersistence == RELPERSISTENCE_PERMANENT &&
		oc->reltablespace == tc->reltablespace &&
		RelFileNumberIsValid(oc->relfilenode);
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
			 void *arg, TimestampTz *wait_start)
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

		if (!try_lock_relation(relid, AccessExclusiveLock, wait_start))
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
		if (!stored_alike(relid, source, oc, tc) ||
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
 * leaves the snapshot to its storage, whose slot names the transaction
 * that took it: the relation's next snapshot, or its page store's next
 * sweep, lets go of it then.)
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

/*
 * Snapshots to take, together (bmap_snapshot_take_many(): the generation
 * of sharing raised once, each relation's pages changed since its last
 * snapshot written out, and each page store synced once as the transaction
 * commits, which the refs naming them commit with): each one's storage
 * and where its id goes.
 */
typedef struct Taking
{
	SMgrRelation *rels;
	BranchKeptRelation **into;
	int			n;
} Taking;

static void
taking_begin(Taking *t, int max)
{
	t->rels = palloc_array(SMgrRelation, Max(max, 1));
	t->into = palloc_array(BranchKeptRelation *, Max(max, 1));
	t->n = 0;
}

static void
taking_add(Taking *t, RelFileLocator loc, BranchKeptRelation *k)
{
	t->rels[t->n] = smgropen(loc, INVALID_PROC_NUMBER);
	t->into[t->n] = k;
	t->n++;
}

static void
taking_end(Taking *t)
{
	uint64	   *ids = palloc0_array(uint64, Max(t->n, 1));

	/* (those taken noted even if a later one fails: to be let go of) */
	PG_TRY();
	{
		bmap_snapshot_take_many(t->rels, t->n, ids);
	}
	PG_FINALLY();
	{
		for (int i = 0; i < t->n; i++)
		{
			if (ids[i] != 0)
				note_taken(t->rels[i]->smgr_rlocator.locator, ids[i]);
		}
	}
	PG_END_TRY();
	for (int i = 0; i < t->n; i++)
		t->into[i]->snapshot = ids[i];
	pfree(ids);
	pfree(t->rels);
	pfree(t->into);
}

/* Keep a relation's pages as they are, by a new snapshot (of t's). */
static void
keep_now(Form_pg_class c, BranchKeptRelation *k, Taking *t)
{
	k->tablespace = c->reltablespace;
	k->relfilenumber = c->relfilenode;
	k->frozenxid = c->relfrozenxid;
	k->minmxid = c->relminmxid;
	taking_add(t, class_locator(c, c->relfilenode), k);
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
			BranchKeptRelation *kept, const BranchKeptRelation *release,
			int nrelease, bool (*settled) (void *arg), void *arg)
{
	Relation	pg_class;
	List	   *rows = NIL;
	bool		ok = true;
	int			i = 0;
	Taking		taking;

	let_go_of_abandoned();
	if (relids == NIL)
		return true;
	taking_begin(&taking, list_length(relids));
	if (OidIsValid(lock_all_or_none(relids, ShareLock)))
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
				keep_now(c, k, &taking);
		}
		if (ok)
			taking_end(&taking);

		/*
		 * (let go of with the relations held against a copy made of kept
		 * pages, which holds them against this as it reads what is kept)
		 */
		for (int r = 0; ok && r < nrelease; r++)
			let_go_of(kept_locator(&release[r]), release[r].snapshot);
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
 * still knows (none older than its datfrozenxid and datminmxid, whose
 * status may be gone)?  Asked before the pages kept are given back, and
 * before a branch is made of them.
 */
static bool
can_restore(Form_pg_class c, const BranchKeptRelation *k)
{
	BmapSnapshot snaps[BMAP_SNAPSHOTS_MAX];
	HeapTuple	dbtup;
	TransactionId datfrozenxid;
	MultiXactId datminmxid;
	int			n;

	dbtup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
	if (!HeapTupleIsValid(dbtup))
		elog(ERROR, "cache lookup failed for database %u", MyDatabaseId);
	datfrozenxid = ((Form_pg_database) GETSTRUCT(dbtup))->datfrozenxid;
	datminmxid = ((Form_pg_database) GETSTRUCT(dbtup))->datminmxid;
	ReleaseSysCache(dbtup);

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
			   BranchKeptRelation *renewed, TimestampTz *wait_start)
{
	Relation	pg_class;
	HeapTuple  *rows = palloc0_array(HeapTuple, Max(nkept, 1));
	bool	   *changed = palloc0_array(bool, Max(nkept, 1));
	bool		any = false;
	bool		ok = true;
	Taking		taking;

	let_go_of_abandoned();
	pg_class = table_open(RelationRelationId, RowExclusiveLock);
	taking_begin(&taking, nkept);

	/* (each checked before any is restored) */
	for (int i = 0; i < nkept && ok; i++)
	{
		rows[i] = class_row(pg_class, kept[i].relid, MyBranch);
		if (rows[i] == NULL ||
			!can_restore((Form_pg_class) GETSTRUCT(rows[i]), &kept[i]))
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
		if (!try_lock_relation(kept[i].relid, AccessExclusiveLock,
							   wait_start))
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

		renewed[i] = *k;
		if (!changed[i])
			continue;
		relnumber = GetNewRelFileNumber(oc->reltablespace, NULL,
										oc->relpersistence);
		(void) RelationCreateStorageFromSnapshot(class_locator(oc, relnumber),
												 smgropen(kept_locator(k),
														  INVALID_PROC_NUMBER),
												 k->snapshot, false, true, true);
		renewed[i].relfilenumber = relnumber;
		taking_add(&taking, class_locator(oc, relnumber), &renewed[i]);
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
		taking_end(&taking);
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

/*
 * Give the session branch's relations kept[] storage made of the pages
 * keep() kept of branch source's (see branch_ops.h).
 */
static bool
copies_share_kept(Oid source, const BranchKeptRelation *kept, int nkept,
				  TimestampTz *wait_start)
{
	bool		other = source != MyBranch;
	Relation	pg_class;
	HeapTuple  *rows = palloc0_array(HeapTuple, Max(nkept, 1));
	bool		ok = true;

	/* (not the pages of a branch being deleted) */
	if (other && !pin_branch(source))
		return false;

	/*
	 * (ours as TRUNCATE locks them; the source's against letting go of the
	 * pages kept, as a branch made of them holds them)
	 */
	for (int i = 0; i < nkept; i++)
	{
		LOCKTAG		tag;

		if (!try_lock_relation(kept[i].relid, AccessExclusiveLock,
							   wait_start))
			return false;
		if (!other)
			continue;
		SET_LOCKTAG_BRANCH_RELATION(tag, MyDatabaseId, kept[i].relid, source);
		if (LockAcquire(&tag, ShareUpdateExclusiveLock, false, true) ==
			LOCKACQUIRE_NOT_AVAIL)
			return false;
	}
	AcceptInvalidationMessages();

	pg_class = table_open(RelationRelationId, RowExclusiveLock);
	for (int i = 0; i < nkept && ok; i++)
	{
		HeapTuple	t = other ? class_row(pg_class, kept[i].relid, source) : NULL;

		rows[i] = class_row(pg_class, kept[i].relid, MyBranch);
		if (rows[i] == NULL || (other && t == NULL))
			ok = false;
		else if (!other)
			ok = can_restore((Form_pg_class) GETSTRUCT(rows[i]), &kept[i]);
		else
			ok = can_restore((Form_pg_class) GETSTRUCT(t), &kept[i]) &&
				stored_alike(kept[i].relid, source,
							 (Form_pg_class) GETSTRUCT(rows[i]),
							 (Form_pg_class) GETSTRUCT(t));
		if (t)
			heap_freetuple(t);
	}

	/* (nor while this session uses one's storage, as restore() refuses) */
	for (int i = 0; i < nkept && ok; i++)
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

		relnumber = GetNewRelFileNumber(oc->reltablespace, NULL,
										oc->relpersistence);
		(void) RelationCreateStorageFromSnapshot(class_locator(oc, relnumber),
												 smgropen(kept_locator(k),
														  INVALID_PROC_NUMBER),
												 k->snapshot, false, false, true);
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
	if (ok)
		CommandCounterIncrement();
	table_close(pg_class, RowExclusiveLock);
	for (int i = 0; i < nkept; i++)
	{
		if (rows[i])
			heap_freetuple(rows[i]);
	}
	pfree(rows);
	return ok;
}

static void
copies_let_go(const BranchKeptRelation *kept)
{
	let_go_of(kept_locator(kept), kept->snapshot);
}

/*
 * Add acl to the privileges of relation relid's row in catalog (pg_class,
 * or its column attnum's pg_attribute row: tup, a modifiable copy), as
 * owner's, recording its grantees' shared dependencies.  The privileges
 * the row has already stay: the ones its making gave it, as this branch's
 * default privileges do.
 */
static void
set_privileges(Relation catalog, HeapTuple tup, AttrNumber aclattr,
			   Oid relid, AttrNumber attnum, Oid owner, Acl *acl)
{
	TupleDesc	desc = RelationGetDescr(catalog);
	Datum	   *values = palloc0_array(Datum, desc->natts);
	bool	   *nulls = palloc0_array(bool, desc->natts);
	bool	   *replaces = palloc0_array(bool, desc->natts);
	bool		isnull;
	Datum		old = heap_getattr(tup, aclattr, desc, &isnull);
	Oid		   *oldmembers = NULL;
	Oid		   *newmembers = NULL;
	int			noldmembers = isnull ? 0 :
		aclmembers(DatumGetAclP(old), &oldmembers);
	int			nnewmembers;
	HeapTuple	newtup;

	if (!isnull)
		acl = aclmerge(DatumGetAclP(old), acl, owner);
	nnewmembers = aclmembers(acl, &newmembers);
	values[aclattr - 1] = PointerGetDatum(acl);
	replaces[aclattr - 1] = true;
	newtup = heap_modify_tuple(tup, desc, values, nulls, replaces);
	CatalogTupleUpdate(catalog, &newtup->t_self, newtup);
	updateAclDependencies(RelationRelationId, relid, attnum, owner,
						  noldmembers, oldmembers, nnewmembers, newmembers);
	heap_freetuple(newtup);
}

/* Branch's row of the object of catalog (by its OID index) with OID oid. */
static HeapTuple
branch_row(Oid catalog, Oid index, AttrNumber oidattr, Oid oid, Oid branch)
{
	List	   *rows = branch_rows(catalog, index, oidattr, oid, branch);

	return rows != NIL ? linitial(rows) : NULL;
}

/*
 * acl, of a relation owner fromowner had, as owner's: the grants
 * fromowner made, owner's, and fromowner's own privileges (all of them,
 * with acl NULL: the default, of a relation of kind relkind) a grant to it
 * from owner, so the role that made the relation keeps what it had.
 */
static Acl *
carried_acl(Acl *acl, char relkind, Oid fromowner, Oid owner)
{
	Acl		   *result;
	AclItem    *items;

	if (acl == NULL)
	{
		if (fromowner == owner)
			return NULL;
		acl = acldefault(relkind == RELKIND_SEQUENCE ? OBJECT_SEQUENCE :
						 OBJECT_TABLE, fromowner);
	}
	result = aclnewowner(acl, fromowner, owner);
	if (fromowner == owner)
		return result;
	items = ACL_DAT(acl);
	for (int i = 0; i < ACL_NUM(acl); i++)
	{
		AclItem		item;

		if (items[i].ai_grantee != fromowner || items[i].ai_grantor != fromowner)
			continue;
		item.ai_grantee = fromowner;
		item.ai_grantor = owner;
		ACLITEM_SET_PRIVS_GOPTIONS(item, ACLITEM_GET_PRIVS(items[i]),
								   ACL_NO_RIGHTS);
		result = aclupdate(result, &item, ACL_MODECHG_ADD, owner, DROP_RESTRICT);
	}
	return result;
}

/*
 * Add to each of the session branch's relations relids the privileges of
 * the relation of branch source of the same place in fromids, the
 * relation's and each column's (by the column's name), as the relation's
 * owner here gives them.  Nothing from one source hasn't, nor one of
 * another kind or another name now (made, or renamed, since the commit
 * that was found to hold it), nor where source is no branch here.
 */
static void
copies_carry_privileges(const char *source, List *relids, List *fromids)
{
	Oid			src = branch_oid(source, true);
	Relation	pg_class;
	Relation	pg_attribute;
	ListCell   *lr;
	ListCell   *lf;

	if (!OidIsValid(src) || src == MyBranch || relids == NIL)
		return;
	pg_class = table_open(RelationRelationId, RowExclusiveLock);
	pg_attribute = table_open(AttributeRelationId, RowExclusiveLock);
	forboth(lr, relids, lf, fromids)
	{
		Oid			relid = lfirst_oid(lr);
		Oid			fromid = lfirst_oid(lf);
		HeapTuple	tup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));
		Form_pg_class rel;
		char	   *nspname;
		HeapTuple	from;
		HeapTuple	fromnsp;
		Form_pg_class fromrel;
		Datum		acl;
		bool		isnull;
		Acl		   *carried;

		if (!HeapTupleIsValid(tup))
			continue;
		rel = (Form_pg_class) GETSTRUCT(tup);
		nspname = get_namespace_name(rel->relnamespace);
		from = branch_row(RelationRelationId, ClassOidIndexId,
						  Anum_pg_class_oid, fromid, src);
		if (from == NULL || nspname == NULL)
			continue;
		fromrel = (Form_pg_class) GETSTRUCT(from);
		fromnsp = branch_row(NamespaceRelationId, NamespaceOidIndexId,
							 Anum_pg_namespace_oid, fromrel->relnamespace, src);
		if (fromrel->relkind != rel->relkind ||
			strcmp(NameStr(fromrel->relname), NameStr(rel->relname)) != 0 ||
			fromnsp == NULL ||
			strcmp(NameStr(((Form_pg_namespace) GETSTRUCT(fromnsp))->nspname),
				   nspname) != 0)
			continue;
		acl = heap_getattr(from, Anum_pg_class_relacl,
						   RelationGetDescr(pg_class), &isnull);
		carried = carried_acl(isnull ? NULL : DatumGetAclP(acl), rel->relkind,
							  fromrel->relowner, rel->relowner);
		if (carried != NULL)
			set_privileges(pg_class, tup, Anum_pg_class_relacl, relid, 0,
						   rel->relowner, carried);
		foreach_ptr(HeapTupleData, att,
					branch_rows(AttributeRelationId, AttributeRelidNumIndexId,
								Anum_pg_attribute_attrelid, fromrel->oid, src))
		{
			Form_pg_attribute fromatt = (Form_pg_attribute) GETSTRUCT(att);
			HeapTuple	mine;

			if (fromatt->attnum <= 0 || fromatt->attisdropped)
				continue;
			acl = heap_getattr(att, Anum_pg_attribute_attacl,
							   RelationGetDescr(pg_attribute), &isnull);
			if (isnull)
				continue;
			mine = SearchSysCacheCopyAttName(relid, NameStr(fromatt->attname));
			if (!HeapTupleIsValid(mine))
				continue;
			set_privileges(pg_attribute, mine, Anum_pg_attribute_attacl, relid,
						   ((Form_pg_attribute) GETSTRUCT(mine))->attnum,
						   rel->relowner,
						   carried_acl(DatumGetAclP(acl), rel->relkind,
									   fromrel->relowner, rel->relowner));
			heap_freetuple(mine);
		}
		heap_freetuple(from);
		heap_freetuple(tup);
	}
	table_close(pg_attribute, RowExclusiveLock);
	table_close(pg_class, RowExclusiveLock);
	CommandCounterIncrement();
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
	.make_kept = copies_make_kept,
	.restore = copies_restore,
	.let_go = copies_let_go,
	.share_kept = copies_share_kept,
	.carry_privileges = copies_carry_privileges,
};

const BranchOpsRoutine *
branch_ops(void)
{
	return &branch_copies;
}
