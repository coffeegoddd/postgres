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
#include "access/xloginsert.h"
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
#include "catalog/pg_enum.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_statistic_ext_data.h"
#include "catalog/pg_type.h"
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
	 * new relation, or one truncated or rewritten) is logged at commit; a
	 * copy of it would be neither.
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

/* the longest wait between tries at a lock a move restores under */
#define KEPT_LOCK_MAX_DELAY_MS	10

/* prolly.restore_lock_wait: how long a lock's other holders are waited for */
int			prolly_restore_lock_wait = 100;

/*
 * Try 'mode' on tag, the lock of relation relid on the branch it names
 * (taken as ConditionalLockRelationOid() takes it), or, for InvalidOid,
 * on a lock of another branch's relation, as LockAcquire() tries it.  A
 * holder that would turn the try away is dealt with first:
 *
 * - a page sweep is asked to let go after the move under way
 *   (bmap_compaction_let_go()), as it does for a waiter in the lock's
 *   queue, and waited out;
 * - an autovacuum worker is asked to cancel its task
 *   (CancelAutoVacuumHolders()), once, as Postgres asks one blocking a
 *   lock waiter, and waited for: it gives up its vacuum at its next check
 *   for interrupts, and comes back to the relation later;
 * - any other holder is waited for, tried again a little later each time.
 *
 * The waits are short, and never as long as it takes, because giving up
 * costs only the rows the move then writes instead, and because the
 * deadlock detector doesn't see them: the caller holds the locks it has
 * taken (the relations restored before, and pg_class), and a holder
 * waiting for one of them waits for as long as this does.  A statement
 * holding the lock lets go within a moment, a transaction left open won't
 * soon: other holders are waited for until prolly.restore_lock_wait after
 * the operation's first wait.  A worker asked to cancel is sure to go, so
 * workers are waited for longer, until deadlock_timeout after the
 * operation first asked one (Postgres's own wait before it cancels one);
 * once both waits are spent, none is asked.
 * lock_timeout, if set, caps both.  Both run from *wait, the operation's,
 * so they bound all its tries together, however many relations it locks.
 * False if the holder is still there after them, counted in
 * restores_lock_busy.
 */
static bool
try_lock(const LOCKTAG *tag, Oid relid, LOCKMODE mode, BranchLockWait *wait)
{
	int			bound = prolly_restore_lock_wait;
	int			cancel_bound = DeadlockTimeout;
	long		delay = 1;

	if (LockTimeout > 0)
	{
		bound = Min(bound, LockTimeout);
		cancel_bound = Min(cancel_bound, LockTimeout);
	}
	for (;;)
	{
		TimestampTz now;
		bool		others_done;

		CHECK_FOR_INTERRUPTS();
		if (OidIsValid(relid) ? ConditionalLockRelationOid(relid, mode) :
			LockAcquire(tag, mode, false, true) != LOCKACQUIRE_NOT_AVAIL)
			return true;
		if (bmap_compaction_let_go(tag, &wait->start))
			continue;
		now = GetCurrentTimestamp();
		if (wait->start == 0)
			wait->start = now;
		others_done = TimestampDifferenceExceeds(wait->start, now, bound);

		/*
		 * (a worker is asked only while one asked could still be waited
		 * for: once the workers' wait is spent too, the move writes the
		 * rows anyway, and a cancel would only cost the worker its vacuum)
		 */
		if (!(others_done && wait->asked != 0 &&
			  TimestampDifferenceExceeds(wait->asked, now, cancel_bound)) &&
			CancelAutoVacuumHolders(tag, mode, wait->asked_xacts,
									&wait->nasked,
									BRANCH_LOCK_WAIT_MAX_ASKED) > 0 &&
			wait->asked == 0)
			wait->asked = now;
		if (others_done &&
			(wait->asked == 0 ||
			 TimestampDifferenceExceeds(wait->asked, now, cancel_bound)))
		{
			pgstat_count_prolly(PGSTAT_PROLLY_RESTORES_LOCK_BUSY, 1);
			return false;
		}
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 delay, WAIT_EVENT_BRANCH_KEPT_LOCK);
		ResetLatch(MyLatch);
		delay = Min(delay * 2, KEPT_LOCK_MAX_DELAY_MS);
	}
}

/* try_lock() on relation relid of the session's branch */
static bool
try_lock_relation(Oid relid, LOCKMODE mode, BranchLockWait *wait)
{
	LOCKTAG		tag;

	SET_LOCKTAG_BRANCH_RELATION(tag, MyDatabaseId, relid,
								relation_lock_branch(relid));
	return try_lock(&tag, relid, mode, wait);
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
 * Are the pages of storage reln still those root k keeps of it?  A root
 * holding them as they were shares reln's subtrees until one changes
 * (bmap_root_unchanged()); a frozen one holds copies, so reln's pages
 * changed since are looked for (bmap_changed_pages(), which reads the
 * map's leaves).
 */
static bool
kept_unchanged(SMgrRelation reln, const BranchKeptRelation *k)
{
	RelFileLocator loc = reln->smgr_rlocator.locator;
	BlockNumber root_nblocks;
	BlockNumber nblocks;
	BlockNumber *blknos;
	uint32		n;

	uint32		since;

	if (k->frozenxid != FrozenTransactionId)
		return bmap_root_unchanged(reln, k->snapshot);
	if (!bmap_root_held(loc, k->snapshot, &root_nblocks) ||
		!bmap_root_base(reln, k->snapshot, &since))
		return false;
	n = bmap_changed_pages(reln, since, &blknos, &nblocks);
	if (blknos)
		pfree(blknos);
	return n == 0 && nblocks == root_nblocks;
}

/*
 * Storage of a relation's copy made of the pages kept by root k of its
 * storage: O(1).  If the relation changed since, its row takes the
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
	/* (a frozen root's pages are copies, of which the source's maps say nothing) */
	if (k->frozenxid != FrozenTransactionId && kept_unchanged(src, k))
	{
		(void) RelationCreateStorageFromRoot(class_locator(c, relnumber),
											 k->snapshot, src, NULL, src, true);
		return relnumber;
	}
	(void) RelationCreateStorageFromRoot(class_locator(c, relnumber),
										 k->snapshot, NULL, NULL, src, true);
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

		pgstat_count_prolly(PGSTAT_PROLLY_BRANCHES_LISTED, 1);
		visit(NameStr(b->brname), b->oid, arg);
	}
	systable_endscan(scan);
	table_close(brel, AccessShareLock);
}

static void
copies_branches_while(BranchVisitWhile visit, void *arg)
{
	Relation	brel = table_open(BranchRelationId, AccessShareLock);
	Relation	irel = index_open(BranchNameIndexId, AccessShareLock);
	SysScanDesc scan;
	HeapTuple	tup;

	scan = systable_beginscan_ordered(brel, irel, NULL, 0, NULL);
	while ((tup = systable_getnext_ordered(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_branch b = (Form_pg_branch) GETSTRUCT(tup);

		pgstat_count_prolly(PGSTAT_PROLLY_BRANCHES_LISTED, 1);
		if (!visit(NameStr(b->brname), b->oid, arg))
			break;
	}
	systable_endscan_ordered(scan);
	index_close(irel, AccessShareLock);
	table_close(brel, AccessShareLock);
}

/*
 * The branches named with prefix: an ordered scan of the name index from
 * prefix, until a name doesn't begin with it (names compare as bytes, as
 * the index orders them).
 */
static void
copies_branches_named(const char *prefix, BranchVisit visit, void *arg)
{
	Relation	brel = table_open(BranchRelationId, AccessShareLock);
	Relation	irel = index_open(BranchNameIndexId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;
	NameData	start;
	size_t		len = strlen(prefix);

	namestrcpy(&start, prefix);
	ScanKeyInit(&key, Anum_pg_branch_brname, BTGreaterEqualStrategyNumber,
				F_NAMEGE, NameGetDatum(&start));
	scan = systable_beginscan_ordered(brel, irel, NULL, 1, &key);
	while ((tup = systable_getnext_ordered(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_branch b = (Form_pg_branch) GETSTRUCT(tup);

		if (strncmp(NameStr(b->brname), prefix, len) != 0)
			break;
		visit(NameStr(b->brname), b->oid, arg);
	}
	systable_endscan_ordered(scan);
	index_close(irel, AccessShareLock);
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
			 void *arg, BranchLockWait *wait)
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

		if (!try_lock_relation(relid, AccessExclusiveLock, wait))
			return false;
		SET_LOCKTAG_BRANCH_RELATION(tag, MyDatabaseId, relid, source);
		if (!try_lock(&tag, InvalidOid, ShareLock, wait))
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
 * Keeping a branch's pages (keep(), restore()).  A relation's pages are
 * kept by a root of its block-mapped storage captured into its page store
 * (bmap_root_capture()), which outlives the relation's storage: a restore
 * gives the relation new storage made of the root, and the branch's other
 * kept commits keep their roots as they are.  A root is in the store, not
 * the transaction: one captured by a transaction (or subtransaction) that
 * aborts holds nothing, but keeps its slot until the store reclaims it, so
 * each captured is remembered until its transaction commits, and the
 * stores of those whose transactions aborted are reclaimed the next time
 * this backend keeps or restores (reclaiming writes, which an abort
 * can't), or, if it never does, as it exits.  (A crash in between leaves
 * the slot to its store's next sweep.)  Letting go of a root is
 * transactional: it takes effect as the transaction letting go commits.
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

/*
 * Let go of root 'id' of the page store of storage locator, as the
 * transaction commits, if the store still holds it.
 */
static void
let_go_of(RelFileLocator locator, uint64 id)
{
	if (id != 0)
		(void) bmap_root_release(locator, id);
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

/* The stores of roots aborted (sub)transactions captured, reclaimed. */
static void
let_go_of_abandoned(void)
{
	while (abandoned != NIL)
	{
		TakenSnapshot *t = linitial(abandoned);
		bool		done = false;

		abandoned = list_delete_first(abandoned);
		foreach_ptr(TakenSnapshot, o, abandoned)
			done |= o->locator.spcOid == t->locator.spcOid;
		if (!done)
			(void) bmap_roots_reclaim(t->locator, false);
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

/*
 * Roots to capture, together (bmap_root_capture_many(): the generation of
 * sharing raised once, each relation's pages changed since its last
 * capture written out, and made durable by the transaction's commit, which
 * the refs naming them commit with, through the images of those pages it
 * logged): each one's storage and where its id goes.
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
		bmap_root_capture_many(t->rels, t->n, ids);
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

/* Keep a relation's pages as they are, by a new root (of t's). */
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

/* Are relation c's pages heap pages, whose versions a root freezes? */
static bool
heap_format(Form_pg_class c)
{
	return c->relkind == RELKIND_RELATION || c->relkind == RELKIND_TOASTVALUE ||
		c->relkind == RELKIND_MATVIEW;
}

/*
 * Keep relation c's pages (row k) by a root holding them frozen as of
 * snapshot (see branch_ops.h's keep()): each page changed since the
 * relation's root among prior[] (the first of its storage that its store
 * still holds; none: every page) copied, frozen and written into a copy of
 * that root (temporary storage, dropped as the transaction ends, made a
 * root of), each logged as an image once.  The relation's own pages are
 * left as they are, and its epoch raised so that each one's next change
 * marks it changed since.  Unchanged since, it keeps that root.
 */
static void
keep_frozen(Form_pg_class c, BranchKeptRelation *k,
			const BranchKeptRelation *prior, int nprior,
			BranchFreezePage freeze, Snapshot snapshot)
{
	RelFileLocator live = class_locator(c, c->relfilenode);
	SMgrRelation lsrel = smgropen(live, INVALID_PROC_NUMBER);
	const BranchKeptRelation *base = NULL;
	BmapRootId	base_root = 0;
	BlockNumber base_nblocks = 0;
	uint32		since = 0;
	BlockNumber *blknos = NULL;
	BlockNumber nblocks;
	uint32		n;
	RelFileNumber tnumber;
	RelFileLocator tloc;
	SMgrRelation tsrel;
	PGIOAlignedBlock copy;
	uint32		live_epoch;

	/*
	 * (a kept commit's root may be one of another storage's pages, named
	 * as of this one by a restore: only a root captured of this storage,
	 * or the one it was made a copy of, tells its pages changed since)
	 */
	for (int p = 0; p < nprior && base_root == 0; p++)
	{
		if (prior[p].relid == c->oid &&
			prior[p].relfilenumber == c->relfilenode &&
			prior[p].tablespace == c->reltablespace &&
			prior[p].frozenxid == FrozenTransactionId &&
			bmap_root_held(live, prior[p].snapshot, &base_nblocks) &&
			bmap_root_base(lsrel, prior[p].snapshot, &since))
		{
			base = &prior[p];
			base_root = base->snapshot;
		}
	}
	if (base_root == 0)
	{
		BmapRootId	made_of = bmap_made_of(lsrel);

		if (made_of != 0 && bmap_root_held(live, made_of, &base_nblocks) &&
			bmap_root_base(lsrel, made_of, &since))
			base_root = made_of;
		else
			since = 0;
	}
	n = bmap_changed_pages(lsrel, since, &blknos, &nblocks);
	if (base && n == 0 && nblocks == base_nblocks)
	{
		*k = *base;
		if (blknos)
			pfree(blknos);
		return;
	}

	/* the copy: of the root, or empty; its length the relation's */
	tnumber = GetNewRelFileNumber(c->reltablespace, NULL, c->relpersistence);
	tloc = class_locator(c, tnumber);
	if (base_root != 0)
		tsrel = RelationCreateStorageFromRoot(tloc, base_root, NULL, NULL,
											  NULL, true);
	else
		tsrel = RelationCreateStorageWith(tloc, RELPERSISTENCE_PERMANENT, true,
										  SMGR_BMAP);
	if (smgrnblocks(tsrel, MAIN_FORKNUM) > nblocks)
		smgrtruncate(tsrel, &(ForkNumber) {MAIN_FORKNUM}, 1,
					 &(BlockNumber) {smgrnblocks(tsrel, MAIN_FORKNUM)}, &nblocks);
	else if (smgrnblocks(tsrel, MAIN_FORKNUM) < nblocks)
		ReleaseBuffer(ExtendBufferedRelTo(BMR_SMGR(tsrel, RELPERSISTENCE_PERMANENT),
										  MAIN_FORKNUM, NULL,
										  EB_SKIP_EXTENSION_LOCK | EB_CLEAR_SIZE_CACHE, nblocks,
										  RBM_ZERO_ON_ERROR));

	/* each page changed: frozen, and the copy's (a new block of its own) */
	for (uint32 i = 0; i < n; i++)
	{
		Buffer		buf;

		buf = ReadBufferWithoutRelcache(live, MAIN_FORKNUM, blknos[i],
										RBM_NORMAL, NULL, true);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		memcpy(copy.data, BufferGetPage(buf), BLCKSZ);
		UnlockReleaseBuffer(buf);
		if (!PageIsNew((Page) copy.data))
			(void) freeze((Page) copy.data, blknos[i], snapshot);
		buf = ReadBufferWithoutRelcache(tloc, MAIN_FORKNUM, blknos[i],
										RBM_ZERO_AND_LOCK, NULL, true);
		START_CRIT_SECTION();
		memcpy(BufferGetPage(buf), copy.data, BLCKSZ);
		MarkBufferDirty(buf);
		if (!PageIsNew(BufferGetPage(buf)))
			log_newpage_buffer(buf, true);
		END_CRIT_SECTION();
		UnlockReleaseBuffer(buf);
		CHECK_FOR_INTERRUPTS();
	}
	if (blknos)
		pfree(blknos);

	/* the root, of the relation; the relation's next changes told anew */
	live_epoch = bmap_raise_epoch(lsrel);
	k->tablespace = c->reltablespace;
	k->relfilenumber = c->relfilenode;
	k->snapshot = bmap_root_capture_logged(tsrel, c->relfilenode, live_epoch);
	note_taken(live, k->snapshot);
	k->frozenxid = FrozenTransactionId;
	k->minmxid = InvalidMultiXactId;
	RelationDropStorageByLocator(tloc);
	pgstat_count_prolly(PGSTAT_PROLLY_PAGES_KEPT, 1);
}

/* Can relation c's storage keep its pages, by a root captured? */
static bool
can_keep(Form_pg_class c)
{
	RelFileLocator loc;

	if (!RELKIND_HAS_STORAGE(c->relkind) ||
		c->relpersistence != RELPERSISTENCE_PERMANENT ||
		!RelFileNumberIsValid(c->relfilenode))
		return false;
	loc = class_locator(c, c->relfilenode);
	return !RelFileLocatorSkippingWAL(loc) && bmap_relation_exists(loc);
}

/*
 * Keep the pages of the session branch's relations relids (see
 * branch_ops.h).
 */
static bool
copies_keep(List *relids, const BranchKeptRelation *prior, int nprior,
			BranchKeptRelation *kept, const BranchKeptRelation *release,
			int nrelease, bool (*settled) (void *arg), void *arg,
			BranchFreezePage freeze, Snapshot snapshot)
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
			if (freeze && heap_format(c))
			{
				k->relid = c->oid;
				keep_frozen(c, k, prior, nprior, freeze, snapshot);
				continue;
			}
			for (int p = 0; p < nprior && !again; p++)
			{
				if (prior[p].relid == c->oid &&
					prior[p].relfilenumber == c->relfilenode &&
					prior[p].tablespace == c->reltablespace &&
					bmap_root_unchanged(smgropen(class_locator(c, c->relfilenode),
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
 * What is left of relation c's (its row k's) kept pages: restorable if it
 * is still on the branch with the storage that kept it (or the storage a
 * restore of its pages gave it since), its root still held by its page
 * store, and its rows' transaction ids ones the database still knows (none
 * older than its datfrozenxid and datminmxid, whose status may be gone);
 * gone if the root is no longer held, or the relation is on other storage
 * now (a rewrite, a truncation: its rows may no longer be stored as the
 * root has them); held if the root is still there but can't be given back.
 */
static BranchKeptStatus
kept_status_of(Form_pg_class c, const BranchKeptRelation *k)
{
	HeapTuple	dbtup;
	TransactionId datfrozenxid;
	MultiXactId datminmxid;

	if (c->relfilenode != k->relfilenumber ||
		c->reltablespace != k->tablespace ||
		!bmap_root_held(kept_locator(k), k->snapshot, NULL))
		return BRANCH_KEPT_GONE;

	dbtup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
	if (!HeapTupleIsValid(dbtup))
		elog(ERROR, "cache lookup failed for database %u", MyDatabaseId);
	datfrozenxid = ((Form_pg_database) GETSTRUCT(dbtup))->datfrozenxid;
	datminmxid = ((Form_pg_database) GETSTRUCT(dbtup))->datminmxid;
	ReleaseSysCache(dbtup);
	if (c->relpersistence != RELPERSISTENCE_PERMANENT ||
		(TransactionIdIsNormal(k->frozenxid) &&
		 TransactionIdPrecedes(k->frozenxid, datfrozenxid)) ||
		(MultiXactIdIsValid(k->minmxid) &&
		 MultiXactIdPrecedes(k->minmxid, datminmxid)))
		return BRANCH_KEPT_HELD;
	return BRANCH_KEPT_RESTORABLE;
}

/*
 * Can relation c's (its row k's) kept pages be given back
 * (kept_status_of())?  Asked before they are, and before a branch is made
 * of them.
 */
static bool
can_restore(Form_pg_class c, const BranchKeptRelation *k)
{
	return kept_status_of(c, k) == BRANCH_KEPT_RESTORABLE;
}

/*
 * Relation relid moved from storage old to new storage dst, old dropped as
 * the transaction commits: each of the branch's kept commits' roots of old
 * that carry[] names (ncarry of them) named as of dst in carried[], so
 * that a later restore finds the relation on the storage that keeps them.
 * A root is its page store's, not old's: nothing is copied, and it holds
 * what it held.  One not of old is left as carry[] names it.
 */
static void
carry_kept(Oid relid, RelFileLocator old, RelFileLocator dst,
		   const BranchKeptRelation *carry, int ncarry,
		   BranchKeptRelation *carried)
{
	for (int j = 0; j < ncarry; j++)
	{
		const BranchKeptRelation *c = &carry[j];

		if (c->relid != relid ||
			!RelFileLocatorEquals(kept_locator(c), old))
			continue;
		carried[j] = *c;
		carried[j].relfilenumber = dst.relNumber;
	}
}

/*
 * Give the session branch's relations kept[] back the pages keep() kept
 * (see branch_ops.h).
 */
static bool
copies_restore(const BranchKeptRelation *kept, int nkept,
			   BranchKeptRelation *renewed, const BranchKeptRelation *carry,
			   int ncarry, BranchKeptRelation *carried,
			   BranchLockWait *wait)
{
	Relation	pg_class;
	HeapTuple  *rows = palloc0_array(HeapTuple, Max(nkept, 1));
	bool	   *changed = palloc0_array(bool, Max(nkept, 1));
	bool		any = false;
	bool		ok = true;

	let_go_of_abandoned();
	for (int j = 0; j < ncarry; j++)
		carried[j] = carry[j];
	pg_class = table_open(RelationRelationId, RowExclusiveLock);

	/* (each checked before any is restored) */
	for (int i = 0; i < nkept && ok; i++)
	{
		rows[i] = class_row(pg_class, kept[i].relid, MyBranch);
		if (rows[i] == NULL ||
			!can_restore((Form_pg_class) GETSTRUCT(rows[i]), &kept[i]))
			ok = false;
		else if (!kept_unchanged(smgropen(kept_locator(&kept[i]),
										  INVALID_PROC_NUMBER),
								 &kept[i]))
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
							   wait))
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
		(void) RelationCreateStorageFromRoot(class_locator(oc, relnumber),
											 k->snapshot, NULL,
											 smgropen(class_locator(oc, oc->relfilenode),
													  INVALID_PROC_NUMBER),
											 NULL, true);

		/* (the root keeps the pages still: the new storage is of it) */
		renewed[i].relfilenumber = relnumber;
		carry_kept(k->relid, class_locator(oc, oc->relfilenode),
				   class_locator(oc, relnumber), carry, ncarry, carried);
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

/*
 * Give the session branch's relations kept[] storage made of the pages
 * keep() kept of branch source's (see branch_ops.h).
 */
static bool
copies_share_kept(Oid source, const BranchKeptRelation *kept, int nkept,
				  const BranchKeptRelation *carry, int ncarry,
				  BranchKeptRelation *carried, BranchLockWait *wait)
{
	bool		other = source != MyBranch;
	Relation	pg_class;
	HeapTuple  *rows = palloc0_array(HeapTuple, Max(nkept, 1));
	RelFileLocator *sources = palloc0_array(RelFileLocator, Max(nkept, 1));
	bool		ok = true;

	for (int j = 0; j < ncarry; j++)
		carried[j] = carry[j];

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
							   wait))
			return false;
		if (!other)
			continue;
		SET_LOCKTAG_BRANCH_RELATION(tag, MyDatabaseId, kept[i].relid, source);
		if (!try_lock(&tag, InvalidOid, ShareUpdateExclusiveLock,
					  wait))
			return false;
	}
	AcceptInvalidationMessages();

	pg_class = table_open(RelationRelationId, RowExclusiveLock);
	for (int i = 0; i < nkept && ok; i++)
	{
		HeapTuple	t = other ? class_row(pg_class, kept[i].relid, source) : NULL;

		rows[i] = class_row(pg_class, kept[i].relid, MyBranch);
		if (t != NULL)
			sources[i] = class_locator((Form_pg_class) GETSTRUCT(t),
									   ((Form_pg_class) GETSTRUCT(t))->relfilenode);
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

	/*
	 * (nothing to do if every relation's storage is of its root still: a
	 * first use made of these pages, say)
	 */
	if (ok)
	{
		bool		any = false;

		for (int i = 0; i < nkept && !any; i++)
		{
			Form_pg_class oc = (Form_pg_class) GETSTRUCT(rows[i]);

			any = !kept_unchanged(smgropen(class_locator(oc, oc->relfilenode),
										   INVALID_PROC_NUMBER),
								  &kept[i]);
		}
		ok = any;
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
		(void) RelationCreateStorageFromRoot(class_locator(oc, relnumber),
											 k->snapshot, NULL, NULL,
											 other ? smgropen(sources[i], INVALID_PROC_NUMBER) : NULL,
											 true);

		/*
		 * (the session's own kept roots carried, as restore() carries
		 * them; another branch's pages are of another lineage, which the
		 * session's kept commits of the table aren't)
		 */
		if (!other)
			carry_kept(k->relid, class_locator(oc, oc->relfilenode),
					   class_locator(oc, relnumber), carry, ncarry, carried);
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

/*
 * Hold the pages kept again, for the session branch's relation as stored
 * now (see branch_ops.h): another root of the same subtrees.
 */
static bool
copies_hold(const BranchKeptRelation *kept, BranchKeptRelation *held)
{
	Relation	pg_class = table_open(RelationRelationId, AccessShareLock);
	HeapTuple	t = class_row(pg_class, kept->relid, MyBranch);
	bool		ok = false;

	if (t != NULL)
	{
		Form_pg_class c = (Form_pg_class) GETSTRUCT(t);
		uint64		root = bmap_root_copy(kept_locator(kept), kept->snapshot);

		if (root != 0)
		{
			*held = *kept;
			held->tablespace = c->reltablespace;
			held->relfilenumber = c->relfilenode;
			held->snapshot = root;
			note_taken(kept_locator(held), root);
			ok = true;
		}
		heap_freetuple(t);
	}
	table_close(pg_class, AccessShareLock);
	return ok;
}

static void
copies_let_go(const BranchKeptRelation *kept)
{
	let_go_of(kept_locator(kept), kept->snapshot);
}

/*
 * What is left of the pages keep() kept of the session branch's relation
 * kept (see branch_ops.h).
 */
static BranchKeptStatus
copies_kept_status(const BranchKeptRelation *kept)
{
	Relation	pg_class = table_open(RelationRelationId, AccessShareLock);
	HeapTuple	t = class_row(pg_class, kept->relid, MyBranch);
	BranchKeptStatus status = BRANCH_KEPT_GONE;

	if (t != NULL)
	{
		status = kept_status_of((Form_pg_class) GETSTRUCT(t), kept);
		heap_freetuple(t);
	}
	table_close(pg_class, AccessShareLock);
	return status;
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
 * Branch src's row of relation fromid, if it is still the one relation rel
 * of the session's branch was made from: of the same kind and name, in a
 * schema of the same name.  NULL if it isn't (made, or renamed, since the
 * commit that was found to hold it), or src hasn't it.
 */
static HeapTuple
carried_from(Oid src, Form_pg_class rel, Oid fromid)
{
	char	   *nspname = get_namespace_name(rel->relnamespace);
	HeapTuple	from;
	HeapTuple	fromnsp;
	Form_pg_class fromrel;

	if (nspname == NULL)
		return NULL;
	from = branch_row(RelationRelationId, ClassOidIndexId, Anum_pg_class_oid,
					  fromid, src);
	if (from == NULL)
		return NULL;
	fromrel = (Form_pg_class) GETSTRUCT(from);
	fromnsp = branch_row(NamespaceRelationId, NamespaceOidIndexId,
						 Anum_pg_namespace_oid, fromrel->relnamespace, src);
	if (fromrel->relkind != rel->relkind ||
		strcmp(NameStr(fromrel->relname), NameStr(rel->relname)) != 0 ||
		fromnsp == NULL ||
		strcmp(NameStr(((Form_pg_namespace) GETSTRUCT(fromnsp))->nspname),
			   nspname) != 0)
	{
		heap_freetuple(from);
		return NULL;
	}
	return from;
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
		HeapTuple	from;
		Form_pg_class fromrel;
		Datum		acl;
		bool		isnull;
		Acl		   *carried;

		if (!HeapTupleIsValid(tup))
			continue;
		rel = (Form_pg_class) GETSTRUCT(tup);
		from = carried_from(src, rel, fromid);
		if (from == NULL)
			continue;
		fromrel = (Form_pg_class) GETSTRUCT(from);
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

/*
 * Carrying planner statistics (carry_statistics()).  A table a move made
 * from source's rows takes source's statistics of them: its columns', its
 * expression indexes' and its extended statistics' data.  A statistic's
 * values are of the column's type, so a column is carried only where the
 * types are one: the same type, shared by every branch, or a versioned one
 * the move made again (another OID) that is the same by identity, an enum
 * of the same name and labels in the same order (its values, label OIDs,
 * mapped label to label) or a domain of the same name over the same
 * shared base type, or an array of either.  Other versioned types
 * (composites and table row types, ranges, arrays of those) aren't: their
 * values hold OIDs of source's own.
 */
typedef struct CarriedType
{
	Oid			ours;			/* the column's type here */
	bool		enumtype;		/* a versioned enum's, its labels mapped */
	int			nlabels;		/* an enum's: source's label OIDs ... */
	Oid		   *theirs;
	Oid		   *mine;			/* ... and ours, in the same order */

	/*
	 * An array of a versioned enum or domain: the element types (source's,
	 * ours), the enum's labels above, and the array type's alignment.
	 */
	bool		array;
	Oid			theirelem;
	Oid			ourelem;
	char		typalign;
} CarriedType;

/* Do two types' rows name the same type: the same name, in like schemas? */
static bool
same_type_name(Form_pg_type a, Oid abranch, Form_pg_type b, Oid bbranch)
{
	HeapTuple	an = branch_row(NamespaceRelationId, NamespaceOidIndexId,
								Anum_pg_namespace_oid, a->typnamespace, abranch);
	HeapTuple	bn = branch_row(NamespaceRelationId, NamespaceOidIndexId,
								Anum_pg_namespace_oid, b->typnamespace, bbranch);

	return an != NULL && bn != NULL &&
		strcmp(NameStr(a->typname), NameStr(b->typname)) == 0 &&
		strcmp(NameStr(((Form_pg_namespace) GETSTRUCT(an))->nspname),
			   NameStr(((Form_pg_namespace) GETSTRUCT(bn))->nspname)) == 0;
}

/* An enum's labels on branch, in their order, as names and OIDs. */
static int
enum_labels(Oid typid, Oid branch, char ***names, Oid **oids)
{
	List	   *rows = branch_rows(EnumRelationId, EnumTypIdLabelIndexId,
								   Anum_pg_enum_enumtypid, typid, branch);
	int			n = list_length(rows);
	HeapTuple  *sorted = palloc_array(HeapTuple, Max(n, 1));
	int			i = 0;

	foreach_ptr(HeapTupleData, row, rows)
	{
		int			j = i++;

		/* (insertion sort by enumsortorder: an enum has few labels) */
		while (j > 0 &&
			   ((Form_pg_enum) GETSTRUCT(sorted[j - 1]))->enumsortorder >
			   ((Form_pg_enum) GETSTRUCT(row))->enumsortorder)
		{
			sorted[j] = sorted[j - 1];
			j--;
		}
		sorted[j] = row;
	}
	*names = palloc_array(char *, Max(n, 1));
	*oids = palloc_array(Oid, Max(n, 1));
	for (i = 0; i < n; i++)
	{
		Form_pg_enum e = (Form_pg_enum) GETSTRUCT(sorted[i]);

		(*names)[i] = pstrdup(NameStr(e->enumlabel));
		(*oids)[i] = e->oid;
	}
	return n;
}

/*
 * Is source's column type theirs (on branch src) the session branch's ours
 * by identity (see above)?  If so, *ct says how to carry its values.
 */
static bool
carried_type(Oid theirs, Oid src, Oid ours, CarriedType *ct)
{
	/* (NULL for a shared type: branch_row() finds only branch's own) */
	HeapTuple	tt = branch_row(TypeRelationId, TypeOidIndexId,
								Anum_pg_type_oid, theirs, src);
	HeapTuple	ot = SearchSysCache1(TYPEOID, ObjectIdGetDatum(ours));
	Form_pg_type o;
	Form_pg_type t;
	bool		alike = false;

	memset(ct, 0, sizeof(*ct));
	ct->ours = ours;
	if (!HeapTupleIsValid(ot))
		return false;
	o = (Form_pg_type) GETSTRUCT(ot);
	if (tt == NULL)
		/* (source's is shared: so must ours be, the same one) */
		alike = theirs == ours && !OidIsValid(o->typbranch);
	else if (OidIsValid(o->typbranch) &&
			 same_type_name((t = (Form_pg_type) GETSTRUCT(tt)), src, o,
							MyBranch) &&
			 t->typtype == o->typtype)
	{
		if (o->typtype == TYPTYPE_DOMAIN)
		{
			HeapTuple	bt = SearchSysCache1(TYPEOID,
											 ObjectIdGetDatum(o->typbasetype));

			alike = t->typbasetype == o->typbasetype &&
				t->typtypmod == o->typtypmod && HeapTupleIsValid(bt) &&
				!OidIsValid(((Form_pg_type) GETSTRUCT(bt))->typbranch);
			if (HeapTupleIsValid(bt))
				ReleaseSysCache(bt);
		}
		else if (o->typtype == TYPTYPE_ENUM)
		{
			char	  **tnames;
			char	  **onames;
			int			n = enum_labels(theirs, src, &tnames, &ct->theirs);

			alike = enum_labels(ours, MyBranch, &onames, &ct->mine) == n;
			for (int i = 0; alike && i < n; i++)
				alike = strcmp(tnames[i], onames[i]) == 0;
			ct->nlabels = n;
			ct->enumtype = true;
		}
		else if (o->typtype == TYPTYPE_BASE && IsTrueArrayType(o) &&
				 IsTrueArrayType(t))
		{
			CarriedType elem;

			/* (an array of one of the above: its elements' carried so) */
			alike = carried_type(t->typelem, src, o->typelem, &elem);
			if (alike)
			{
				*ct = elem;
				ct->ours = ours;
				ct->array = true;
				ct->theirelem = t->typelem;
				ct->ourelem = o->typelem;
				ct->typalign = o->typalign;
			}
		}
	}
	ReleaseSysCache(ot);
	return alike;
}

/*
 * Array a of source's versioned enum or domain values made one of newelem,
 * ours: an enum's label OIDs mapped to ours (NULL if one isn't found), a
 * domain's values (of the same shared base type: the same bytes) as they
 * are.  Read only as the array's own element type.
 */
static ArrayType *
carried_elements(ArrayType *a, Oid newelem, const CarriedType *ct)
{
	ArrayType  *copy;
	Datum	   *elems;
	bool	   *nulls;
	int			n;

	if (!ct->enumtype)
	{
		copy = (ArrayType *) palloc(VARSIZE(a));
		memcpy(copy, a, VARSIZE(a));
		ARR_ELEMTYPE(copy) = newelem;
		return copy;
	}
	/* (an enum's values: OIDs, by value) */
	deconstruct_array(a, ARR_ELEMTYPE(a), sizeof(Oid), true, TYPALIGN_INT,
					  &elems, &nulls, &n);
	for (int i = 0; i < n; i++)
	{
		int			k;

		if (nulls[i])
			continue;
		for (k = 0; k < ct->nlabels; k++)
			if (ct->theirs[k] == DatumGetObjectId(elems[i]))
				break;
		if (k == ct->nlabels)
			return NULL;
		elems[i] = ObjectIdGetDatum(ct->mine[k]);
	}
	return construct_md_array(elems, nulls, ARR_NDIM(a), ARR_DIMS(a),
							  ARR_LBOUND(a), newelem, sizeof(Oid), true,
							  TYPALIGN_INT);
}

/*
 * A pg_statistic slot's values (an array) of source's column of type
 * theirs, as the session branch's column of ct->ours takes them.
 *
 * - A slot of the column's type: copied as it is for a shared type; for a
 *   versioned enum or domain, carried_elements()'s; for an array of one,
 *   each element array carried so (the slot an array of arrays).
 * - For an array of a versioned enum or domain, a slot of its element type
 *   (its most common elements): carried_elements()'s.
 * - A slot of another type (an array column's element counts, a range's
 *   lengths, a tsvector's lexemes): copied as it is if that type is
 *   shared; the column isn't carried (*ok false) if it is a versioned type
 *   of source's.
 *
 * *ok is false too if an enum's label isn't found.  Values are only ever
 * read as the array's own element type.
 */
static Datum
carried_values(Datum values, Oid theirs, Oid src, const CarriedType *ct,
			   bool *ok)
{
	ArrayType  *a = DatumGetArrayTypeP(values);
	Oid			elem = ARR_ELEMTYPE(a);
	ArrayType  *carried = NULL;

	*ok = true;
	if (elem == theirs && theirs == ct->ours && !ct->enumtype && !ct->array)
		return values;
	if (elem == theirs && !ct->array)
		carried = carried_elements(a, ct->ours, ct);
	else if (ct->array && elem == ct->theirelem)
		carried = carried_elements(a, ct->ourelem, ct);
	else if (ct->array && elem == theirs)
	{
		Datum	   *inner;
		bool	   *nulls;
		int			n;

		deconstruct_array(a, elem, -1, false, ct->typalign,
						  &inner, &nulls, &n);
		for (int i = 0; i < n; i++)
		{
			ArrayType  *e;

			if (nulls[i])
				continue;
			e = carried_elements(DatumGetArrayTypeP(inner[i]), ct->ourelem, ct);
			if (e == NULL)
			{
				*ok = false;
				return values;
			}
			inner[i] = PointerGetDatum(e);
		}
		carried = construct_md_array(inner, nulls, ARR_NDIM(a), ARR_DIMS(a),
									 ARR_LBOUND(a), ct->ours, -1, false,
									 ct->typalign);
	}
	else
	{
		HeapTuple	versioned = branch_row(TypeRelationId, TypeOidIndexId,
										   Anum_pg_type_oid, elem, src);

		if (versioned != NULL)
		{
			heap_freetuple(versioned);
			*ok = false;
		}
		return values;
	}
	if (carried == NULL)
	{
		*ok = false;
		return values;
	}
	return PointerGetDatum(carried);
}

/*
 * Copy source's pg_statistic rows of its relation fromid (on branch src)
 * to the session branch's relid: each attribute's to the attribute of the
 * same name here (byname: a table's columns) or the same number (an
 * index's), of a type carried_type() takes, its values made ours, and its
 * operators and collations ones this branch has.
 */
static void
carry_statistic_rows(Relation pg_statistic, Oid src, Oid fromid, Oid relid,
					 bool byname)
{
	TupleDesc	desc = RelationGetDescr(pg_statistic);
	List	   *fromatts = branch_rows(AttributeRelationId,
									   AttributeRelidNumIndexId,
									   Anum_pg_attribute_attrelid, fromid, src);

	foreach_ptr(HeapTupleData, stat,
				branch_rows(StatisticRelationId, StatisticRelidAttnumInhIndexId,
							Anum_pg_statistic_starelid, fromid, src))
	{
		Form_pg_statistic st = (Form_pg_statistic) GETSTRUCT(stat);
		Form_pg_attribute fromatt = NULL;
		HeapTuple	mine;
		Form_pg_attribute att;
		Datum		values[Natts_pg_statistic] = {0};
		bool		nulls[Natts_pg_statistic] = {0};
		bool		replaces[Natts_pg_statistic] = {0};
		CarriedType ct;
		bool		ok = true;
		HeapTuple	carried;

		foreach_ptr(HeapTupleData, a, fromatts)
			if (((Form_pg_attribute) GETSTRUCT(a))->attnum == st->staattnum)
				fromatt = (Form_pg_attribute) GETSTRUCT(a);
		if (fromatt == NULL || fromatt->attisdropped)
			continue;
		mine = byname ? SearchSysCacheAttName(relid, NameStr(fromatt->attname)) :
			SearchSysCache2(ATTNUM, ObjectIdGetDatum(relid),
							Int16GetDatum(fromatt->attnum));
		if (!HeapTupleIsValid(mine))
			continue;
		att = (Form_pg_attribute) GETSTRUCT(mine);
		ok = !att->attisdropped && att->attcollation == fromatt->attcollation &&
			carried_type(fromatt->atttypid, src, att->atttypid, &ct);
		values[Anum_pg_statistic_staattnum - 1] = Int16GetDatum(att->attnum);
		ReleaseSysCache(mine);
		for (int k = 0; ok && k < STATISTIC_NUM_SLOTS; k++)
		{
			Oid			op = (&st->staop1)[k];
			Oid			coll = (&st->stacoll1)[k];
			bool		isnull;
			Datum		v = heap_getattr(stat, Anum_pg_statistic_stavalues1 + k,
										 desc, &isnull);

			if ((OidIsValid(op) &&
				 !SearchSysCacheExists1(OPEROID, ObjectIdGetDatum(op))) ||
				(OidIsValid(coll) &&
				 !SearchSysCacheExists1(COLLOID, ObjectIdGetDatum(coll))))
				ok = false;
			else if (!isnull)
			{
				values[Anum_pg_statistic_stavalues1 + k - 1] =
					carried_values(v, fromatt->atttypid, src, &ct, &ok);
				replaces[Anum_pg_statistic_stavalues1 + k - 1] = true;
			}
		}
		if (!ok)
			continue;
		values[Anum_pg_statistic_starelid - 1] = ObjectIdGetDatum(relid);
		replaces[Anum_pg_statistic_starelid - 1] = true;
		replaces[Anum_pg_statistic_staattnum - 1] = true;
		values[Anum_pg_statistic_stabranch - 1] = ObjectIdGetDatum(MyBranch);
		replaces[Anum_pg_statistic_stabranch - 1] = true;
		carried = heap_modify_tuple(stat, desc, values, nulls, replaces);
		CatalogTupleInsert(pg_statistic, carried);
		heap_freetuple(carried);
	}
}

/*
 * Is source's type theirs the session branch's ours: the same OID, or each
 * a versioned type of the same name (one the move made again)?  Whether a
 * column's statistics can be carried is carried_type()'s to say.
 */
static bool
same_type_identity(Oid theirs, Oid src, Oid ours)
{
	HeapTuple	tt;
	HeapTuple	ot;
	bool		same;

	if (theirs == ours)
		return true;
	tt = branch_row(TypeRelationId, TypeOidIndexId, Anum_pg_type_oid,
					theirs, src);
	ot = SearchSysCache1(TYPEOID, ObjectIdGetDatum(ours));
	same = tt != NULL && HeapTupleIsValid(ot) &&
		OidIsValid(((Form_pg_type) GETSTRUCT(ot))->typbranch) &&
		same_type_name((Form_pg_type) GETSTRUCT(tt), src,
					   (Form_pg_type) GETSTRUCT(ot), MyBranch);
	if (HeapTupleIsValid(ot))
		ReleaseSysCache(ot);
	return same;
}

/*
 * Are the live columns of source's relation fromid (on branch src) now the
 * ones of the session branch's relid, made from source's head: the same
 * names, in the same order, of the same types (same_type_identity()) and
 * the same collations?  If source's working set renamed, retyped, added or dropped
 * columns since its head, its statistics may describe other columns than
 * the ones of the same name here, and none are carried.
 */
static bool
columns_as_committed(Oid relid, Oid fromid, Oid src)
{
	List	   *ours = branch_rows(AttributeRelationId, AttributeRelidNumIndexId,
								   Anum_pg_attribute_attrelid, relid, MyBranch);
	List	   *theirs = branch_rows(AttributeRelationId, AttributeRelidNumIndexId,
									 Anum_pg_attribute_attrelid, fromid, src);
	ListCell   *a = list_head(ours);
	ListCell   *b = list_head(theirs);

	for (;;)
	{
		Form_pg_attribute o = NULL;
		Form_pg_attribute t = NULL;

		for (; a != NULL; a = lnext(ours, a))
		{
			o = (Form_pg_attribute) GETSTRUCT((HeapTuple) lfirst(a));
			if (o->attnum > 0 && !o->attisdropped)
				break;
			o = NULL;
		}
		for (; b != NULL; b = lnext(theirs, b))
		{
			t = (Form_pg_attribute) GETSTRUCT((HeapTuple) lfirst(b));
			if (t->attnum > 0 && !t->attisdropped)
				break;
			t = NULL;
		}
		if (o == NULL || t == NULL)
			return o == NULL && t == NULL;
		if (strcmp(NameStr(o->attname), NameStr(t->attname)) != 0 ||
			o->atttypmod != t->atttypmod ||
			o->attcollation != t->attcollation ||
			!same_type_identity(t->atttypid, src, o->atttypid))
			return false;
		a = lnext(ours, a);
		b = lnext(theirs, b);
	}
}

/* The name of branch's relation relid (NULL: none). */
static char *
branch_relname(Oid relid, Oid branch)
{
	HeapTuple	t = branch_row(RelationRelationId, ClassOidIndexId,
							   Anum_pg_class_oid, relid, branch);

	return t != NULL ? pstrdup(NameStr(((Form_pg_class) GETSTRUCT(t))->relname)) : NULL;
}

/*
 * The statistics of relid's expression indexes, each from source's index
 * of fromid of the same name and expressions (an index's statistics are
 * of its expressions' values, by column number).
 */
static void
carry_index_statistics(Relation pg_statistic, Oid src, Oid fromid, Oid relid)
{
	List	   *theirs = branch_rows(IndexRelationId, IndexIndrelidIndexId,
									 Anum_pg_index_indrelid, fromid, src);

	foreach_ptr(HeapTupleData, mine,
				branch_rows(IndexRelationId, IndexIndrelidIndexId,
							Anum_pg_index_indrelid, relid, MyBranch))
	{
		Oid			ours = ((Form_pg_index) GETSTRUCT(mine))->indexrelid;
		char	   *name;

		if (heap_attisnull(mine, Anum_pg_index_indexprs, NULL) ||
			(name = branch_relname(ours, MyBranch)) == NULL)
			continue;
		foreach_ptr(HeapTupleData, their, theirs)
		{
			Oid			from = ((Form_pg_index) GETSTRUCT(their))->indexrelid;
			char	   *fromname = branch_relname(from, src);

			if (fromname != NULL && strcmp(fromname, name) == 0 &&
				same_column(IndexRelationId, mine, their,
							Anum_pg_index_indexprs) &&
				same_column(IndexRelationId, mine, their,
							Anum_pg_index_indkey))
				carry_statistic_rows(pg_statistic, src, from, ours, false);
		}
	}
}

/* Is each column of m's keys named here as on source's fromid? */
static bool
keys_named_alike(Form_pg_statistic_ext m, Oid fromid, Oid src)
{
	List	   *theirs = branch_rows(AttributeRelationId, AttributeRelidNumIndexId,
									 Anum_pg_attribute_attrelid, fromid, src);

	for (int i = 0; i < m->stxkeys.dim1; i++)
	{
		char	   *mine = get_attname(m->stxrelid, m->stxkeys.values[i], true);
		bool		found = false;

		foreach_ptr(HeapTupleData, a, theirs)
		{
			Form_pg_attribute t = (Form_pg_attribute) GETSTRUCT(a);

			if (t->attnum == m->stxkeys.values[i])
				found = mine != NULL && !t->attisdropped &&
					strcmp(NameStr(t->attname), mine) == 0;
		}
		if (!found)
			return false;
	}
	return true;
}

/*
 * The data of relid's extended statistics objects, each from source's
 * object on fromid of the same name and kinds over the same columns by
 * number, each of the same name on both (keys_named_alike()): the data
 * names its columns by number, so an object whose columns' numbers differ
 * (a column dropped before them on one side) isn't carried.  Not one on expressions,
 * whose data holds pg_statistic rows of source's, nor one over a column of
 * a type the move made again: the data holds the types' OIDs.
 */
static void
carry_extended_statistics(Oid src, Oid fromid, Oid relid)
{
	Relation	data = table_open(StatisticExtDataRelationId, RowExclusiveLock);
	TupleDesc	desc = RelationGetDescr(data);
	List	   *theirs = branch_rows(StatisticExtRelationId,
									 StatisticExtRelidIndexId,
									 Anum_pg_statistic_ext_stxrelid, fromid, src);

	foreach_ptr(HeapTupleData, mine,
				branch_rows(StatisticExtRelationId, StatisticExtRelidIndexId,
							Anum_pg_statistic_ext_stxrelid, relid, MyBranch))
	{
		Form_pg_statistic_ext m = (Form_pg_statistic_ext) GETSTRUCT(mine);
		bool		types_shared = true;

		if (!heap_attisnull(mine, Anum_pg_statistic_ext_stxexprs, NULL) ||
			branch_rows(StatisticExtDataRelationId,
						StatisticExtDataStxoidInhIndexId,
						Anum_pg_statistic_ext_data_stxoid, m->oid,
						MyBranch) != NIL)
			continue;
		for (int i = 0; i < m->stxkeys.dim1 && types_shared; i++)
		{
			HeapTuple	att = SearchSysCache2(ATTNUM, ObjectIdGetDatum(relid),
											  Int16GetDatum(m->stxkeys.values[i]));
			HeapTuple	typ;

			types_shared = false;
			if (!HeapTupleIsValid(att))
				break;
			typ = SearchSysCache1(TYPEOID, ObjectIdGetDatum(((Form_pg_attribute) GETSTRUCT(att))->atttypid));
			types_shared = HeapTupleIsValid(typ) &&
				!OidIsValid(((Form_pg_type) GETSTRUCT(typ))->typbranch);
			if (HeapTupleIsValid(typ))
				ReleaseSysCache(typ);
			ReleaseSysCache(att);
		}
		if (!types_shared)
			continue;
		foreach_ptr(HeapTupleData, their, theirs)
		{
			Form_pg_statistic_ext t = (Form_pg_statistic_ext) GETSTRUCT(their);

			if (strcmp(NameStr(t->stxname), NameStr(m->stxname)) != 0 ||
				!keys_named_alike(m, fromid, src) ||
				!heap_attisnull(their, Anum_pg_statistic_ext_stxexprs, NULL) ||
				!same_column(StatisticExtRelationId, mine, their,
							 Anum_pg_statistic_ext_stxkeys) ||
				!same_column(StatisticExtRelationId, mine, their,
							 Anum_pg_statistic_ext_stxkind))
				continue;
			foreach_ptr(HeapTupleData, row,
						branch_rows(StatisticExtDataRelationId,
									StatisticExtDataStxoidInhIndexId,
									Anum_pg_statistic_ext_data_stxoid, t->oid,
									src))
			{
				Datum		values[Natts_pg_statistic_ext_data] = {0};
				bool		nulls[Natts_pg_statistic_ext_data] = {0};
				bool		replaces[Natts_pg_statistic_ext_data] = {0};
				HeapTuple	carried;

				values[Anum_pg_statistic_ext_data_stxoid - 1] = ObjectIdGetDatum(m->oid);
				replaces[Anum_pg_statistic_ext_data_stxoid - 1] = true;
				values[Anum_pg_statistic_ext_data_stxdbranch - 1] = ObjectIdGetDatum(MyBranch);
				replaces[Anum_pg_statistic_ext_data_stxdbranch - 1] = true;
				carried = heap_modify_tuple(row, desc, values, nulls, replaces);
				CatalogTupleInsert(data, carried);
				heap_freetuple(carried);
			}
			break;
		}
	}
	table_close(data, RowExclusiveLock);
}

/*
 * Give each of the session branch's tables relids that has no statistics
 * the planner statistics of the table of branch source of the same place
 * in fromids (see branch_ops.h).  A move that made the table loaded it
 * from source's head (and its index builds counted the rows); source's
 * statistics describe source's rows as its last analyze sampled them.
 */
static void
copies_carry_statistics(const char *source, List *relids, List *fromids)
{
	Oid			src = branch_oid(source, true);
	Relation	pg_class;
	Relation	pg_statistic;
	ListCell   *lr;
	ListCell   *lf;

	if (!OidIsValid(src) || src == MyBranch || relids == NIL)
		return;
	pg_class = table_open(RelationRelationId, RowExclusiveLock);
	pg_statistic = table_open(StatisticRelationId, RowExclusiveLock);
	forboth(lr, relids, lf, fromids)
	{
		Oid			relid = lfirst_oid(lr);
		Oid			fromid = lfirst_oid(lf);
		HeapTuple	tup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));
		Form_pg_class rel;
		HeapTuple	from = NULL;
		Form_pg_class fromrel;

		if (!HeapTupleIsValid(tup))
			continue;
		rel = (Form_pg_class) GETSTRUCT(tup);
		if ((rel->relkind != RELKIND_RELATION &&
			 rel->relkind != RELKIND_MATVIEW &&
			 rel->relkind != RELKIND_PARTITIONED_TABLE) ||
			branch_rows(StatisticRelationId, StatisticRelidAttnumInhIndexId,
						Anum_pg_statistic_starelid, relid, MyBranch) != NIL ||
			(from = carried_from(src, rel, fromid)) == NULL ||
			!columns_as_committed(relid, fromid, src))
		{
			if (from != NULL)
				heap_freetuple(from);
			heap_freetuple(tup);
			continue;
		}
		fromrel = (Form_pg_class) GETSTRUCT(from);
		carry_statistic_rows(pg_statistic, src, fromid, relid, true);
		carry_index_statistics(pg_statistic, src, fromid, relid);
		carry_extended_statistics(src, fromid, relid);
		if (rel->reltuples < 0 && fromrel->reltuples >= 0)
		{
			rel->reltuples = fromrel->reltuples;
			rel->relpages = fromrel->relpages;
			CatalogTupleUpdate(pg_class, &tup->t_self, tup);
		}
		heap_freetuple(from);
		heap_freetuple(tup);
	}
	table_close(pg_statistic, RowExclusiveLock);
	table_close(pg_class, RowExclusiveLock);
	CommandCounterIncrement();
}

/* The owner of branch's relation relid (InvalidOid: none). */
static Oid
copies_relation_owner(const char *branch, Oid relid)
{
	Oid			b = branch_oid(branch, true);
	HeapTuple	tup;
	Oid			owner;

	if (!OidIsValid(b))
		return InvalidOid;
	tup = branch_row(RelationRelationId, ClassOidIndexId, Anum_pg_class_oid,
					 relid, b);
	if (tup == NULL)
		return InvalidOid;
	owner = ((Form_pg_class) GETSTRUCT(tup))->relowner;
	heap_freetuple(tup);
	return owner;
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
	.branches_named = copies_branches_named,
	.branches_while = copies_branches_while,
	.run_on = copies_run_on,
	.share = copies_share,
	.keep = copies_keep,
	.make_kept = copies_make_kept,
	.restore = copies_restore,
	.let_go = copies_let_go,
	.kept_status = copies_kept_status,
	.share_kept = copies_share_kept,
	.hold = copies_hold,
	.carry_privileges = copies_carry_privileges,
	.carry_statistics = copies_carry_statistics,
	.relation_owner = copies_relation_owner,
};

const BranchOpsRoutine *
branch_ops(void)
{
	return &branch_copies;
}
