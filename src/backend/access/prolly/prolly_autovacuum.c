/*-------------------------------------------------------------------------
 *
 * prolly_autovacuum.c
 *	  Prolly's stores reclaimed by autovacuum.
 *
 * After its tables, a database's autovacuum worker severs the history
 * past the database's window (the history window's maintain(): as the
 * database's settings say), then collects the chunk store, in full if it
 * severed some, else when what a default collection would collect (the journal segment
 * appends go to, written since the last collection, and the table files
 * not settled, received since) has passed prolly.gc_journal_threshold and
 * the settled files' size times prolly.gc_growth_scale_factor: a default
 * collection, by the database's reclamation.  Only sizes are read, not the
 * journal.  Another collection running, it skips its own quietly (counted
 * in pg_stat_prolly's auto_collections_skipped): that one collects what
 * was due.  Then it moves the journal (the reclamation's move(): its
 * segments rewritten as a table file, nothing freed) once its segments
 * have passed prolly.journal_move_threshold, which bounds what a backend
 * reads to index the journal when collections are rare (a large store) or
 * off; after a collection, they haven't.  Then, with more table files than
 * prolly.conjoin_max_files, it conjoins the smallest (the chunk store's
 * conjoin()), so a lookup searches no more files than that, as Dolt does
 * past 256.  It drains the page stores' release queues (what dropped
 * relations' storage held, let go of in the background: see bmap.c), at
 * most prolly.release_max_per_pass entries a pass, under the cost-based
 * delay.  It sweeps a page store when bmap_sweep_due() says so.  It
 * folds the working set (a checkpoint, as prolly_checkpoint() does) once a
 * table's change log holds more entries than prolly.autofold_threshold of
 * the table's rows, so a commit folds little (#511): folded before the
 * refs are taken, and given up if they're held, the branch moved
 * meanwhile or a relation it reads is locked by another (the refs'
 * checkpoint_unless_moved(), as the fold worker folds), so a commit, or a
 * DROP, never waits for it; it is the backstop for a branch
 * the fold worker doesn't reach.  Each
 * runs in a transaction of its own, an error reported and passed over, as
 * autovacuum's tables' are; a collection, a move and a conjoin read and
 * copy under autovacuum's cost-based delay.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_autovacuum.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/prolly_autovacuum.h"
#include "access/prolly_chunkstore.h"
#include "access/prolly_config.h"
#include "access/prolly_fold.h"
#include "access/prolly_refs.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/branch_ops.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_tablespace.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bmap.h"
#include "storage/lmgr.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

int			prolly_gc_journal_threshold = 128;
double		prolly_gc_growth_scale_factor = 0.2;
int			prolly_journal_move_threshold = 256;
int			prolly_conjoin_max_files = 256;
int			prolly_gc_file_size = 65536;
bool		prolly_log_copied_bytes = false;
double		prolly_autofold_threshold = 0.05;
int			prolly_release_max_per_pass = 1000000;

/* fewer than two files would be conjoined at every pass: -1 turns it off */
static bool
check_conjoin_max_files(int *newval, void **extra, GucSource source)
{
	if (*newval == 0 || *newval == 1)
	{
		GUC_check_errdetail("Use at least 2 files, or -1 to turn conjoining off.");
		return false;
	}
	return true;
}

/* 0 would move the journal at every pass: -1 turns moving off */
static bool
check_move_threshold(int *newval, void **extra, GucSource source)
{
	if (*newval == 0)
	{
		GUC_check_errdetail("Use -1 to turn moving the journal off.");
		return false;
	}
	return true;
}

void
prolly_autovacuum_init(void)
{
	static bool defined = false;

	if (defined)
		return;
	DefineCustomIntVariable("prolly.gc_journal_threshold",
							"How far the chunk journal grows before autovacuum collects the chunk store.",
							"-1 turns automatic collection off.",
							&prolly_gc_journal_threshold,
							128, -1, MAX_KILOBYTES / 1024,
							PGC_SIGHUP,
							GUC_UNIT_MB,
							NULL, NULL, NULL);
	DefineCustomRealVariable("prolly.gc_growth_scale_factor",
							 "How far the chunk journal grows before autovacuum collects the chunk store, as a fraction of the settled table files' size.",
							 NULL,
							 &prolly_gc_growth_scale_factor,
							 0.2, 0.0, 100.0,
							 PGC_SIGHUP,
							 0,
							 NULL, NULL, NULL);
	DefineCustomIntVariable("prolly.journal_move_threshold",
							"How far the chunk journal grows before autovacuum moves its chunks into a table file.",
							"Moving frees nothing; -1 turns it off.",
							&prolly_journal_move_threshold,
							256, -1, MAX_KILOBYTES / 1024,
							PGC_SIGHUP,
							GUC_UNIT_MB,
							check_move_threshold, NULL, NULL);
	DefineCustomIntVariable("prolly.conjoin_max_files",
							"How many table files a chunk store may have before autovacuum conjoins the smallest.",
							"-1 turns it off.",
							&prolly_conjoin_max_files,
							256, -1, INT_MAX,
							PGC_SIGHUP,
							0,
							check_conjoin_max_files, NULL, NULL);
	DefineCustomIntVariable("prolly.gc_file_size",
							"How large each table file a collection writes grows before it starts another.",
							"Files of a set size can be sent whole to a remote and conjoined later.",
							&prolly_gc_file_size,
							65536, 1024, MAX_KILOBYTES,
							PGC_SIGHUP,
							GUC_UNIT_KB,
							NULL, NULL, NULL);
	DefineCustomIntVariable("prolly.release_max_per_pass",
							"How many release queue entries (dropped storage's subtrees and blocks) one autovacuum pass's release step lets go of at most in each tablespace, and one VACUUM command (or one autovacuum pass's VACUUMs) before vacuuming.",
							"-1 sets no limit.  Draining is paced by the cost-based vacuum delay either way.",
							&prolly_release_max_per_pass,
							1000000, -1, INT_MAX,
							PGC_SIGHUP,
							0,
							NULL, NULL, NULL);
	DefineCustomBoolVariable("prolly.log_copied_bytes",
							 "WAL-logs the bytes a chunk store copies from its own files, not where they come from.",
							 "For debugging, and comparing the two.",
							 &prolly_log_copied_bytes,
							 false,
							 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);
	DefineCustomRealVariable("prolly.autofold_threshold",
							 "How large a versioned table's change log grows, as a fraction of the table's size, before autovacuum folds the working set.",
							 "So a commit folds little.  0 turns it off; a table with autovacuum_enabled = false is passed over.  The fold never waits for the refs: held, it is tried again next time.",
							 &prolly_autofold_threshold,
							 0.05, 0.0, 100.0,
							 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);
	DefineCustomBoolVariable("prolly.fold_on_rewrite",
							 "Folds the branch of a versioned table a rewrite (ALTER TABLE, REFRESH MATERIALIZED VIEW) read and wrote every row of, as a checkpoint, as the rewrite ends.",
							 "So the next commit folds only what changed since; the rewrite pays for the fold.",
							 &prolly_fold_on_rewrite,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);
	DefineCustomRealVariable("prolly.map_reshaped_dead_ratio",
							 "Dead row versions per live row past which a table whose row shape changed is folded by mapping its baseline's rows, not by reading it in full.",
							 "Both read every row; a full fold reads the heap's, which costs less per row than decoding the baseline's until dead versions dominate the heap.  0 always maps.",
							 &prolly_map_reshaped_dead_ratio,
							 1.5, 0.0, 1000.0,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);
	DefineCustomBoolVariable("prolly.vacuum_copy_kept_pages",
							 "Has VACUUM freeze, and set all-visible, pages shared only with the commits a table's pages are kept as of, copying them.",
							 "Off, VACUUM leaves such pages as it leaves any shared page with nothing to reclaim, and only an aggressive VACUUM freezes them (copying them).  On, it freezes and sets them all-visible like unshared pages, at the cost of a copy of each page so changed, kept until its commit stops being kept.",
							 &vacuum_copy_kept_pages,
							 false,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);
	DefineCustomIntVariable("prolly.root_interval",
							"The least time between the commits a branch's pages are kept as of.",
							"A move to a commit between them restores the nearest kept one before it and writes the rows they differ by.  -1: every commit, or a second under dolt.transaction_commit.",
							&prolly_root_interval,
							-1, -1, INT_MAX,
							PGC_USERSET,
							GUC_UNIT_MS,
							NULL, NULL, NULL);
	DefineCustomIntVariable("prolly.root_interval_commits",
							"The least number of commits between the commits a branch's pages are kept as of.",
							"1 keeps every commit the interval in time allows.",
							&prolly_root_interval_commits,
							1, 1, INT_MAX,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);
	DefineCustomBoolVariable("prolly.freeze_roots",
							 "Freeze the roots a commit captures as the commit keeps them.",
							 "A frozen root holds copies of the pages changed since the relation's last, frozen as of the commit, and shares none with the relation; off, a root shares the relation's pages as they are.  Under dolt.transaction_commit roots are captured among the writers and frozen later, in the background, or by a move to their commit; off, they are kept only while no other writer is at work.",
							 &prolly_freeze_roots,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);
	DefineCustomIntVariable("prolly.restore_min_changes",
							"How many of a table's rows must have changed since a kept commit before a move back to it gives the table its kept pages back, rather than writing those rows.",
							"Restoring a table's pages costs about as much as writing a few of its rows back and syncing them.  0 always restores.",
							&prolly_restore_min_changes,
							2, 0, INT_MAX,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);
	DefineCustomIntVariable("prolly.restore_lock_wait",
							"How long a move giving a table shared pages waits for another holder of one of the table's locks, before it writes the table's rows instead.",
							"An autovacuum worker holding one is asked to cancel and waited for apart from this; lock_timeout, if set, caps the wait.  0 doesn't wait.  A restore of the branch's own kept pages queues for its locks instead, as TRUNCATE does.",
							&prolly_restore_lock_wait,
							100, 0, INT_MAX,
							PGC_USERSET,
							GUC_UNIT_MS,
							NULL, NULL, NULL);
	DefineCustomIntVariable("prolly.log_full_folds",
							"Logs each fold that reads its table in full, with why, once it reads at least this many rows.",
							"-1 logs none; 0 logs every one.",
							&prolly_log_full_folds,
							-1, -1, INT_MAX,
							PGC_SUSET,
							0,
							NULL, NULL, NULL);
	DefineCustomRealVariable("prolly.fold_verify_rate",
							 "The fraction of a database's versioned tables autovacuum folds in full at a pass, to check their folds from their baselines.",
							 "0 checks none.",
							 &prolly_fold_verify_rate,
							 0, 0, 1,
							 PGC_SIGHUP,
							 0,
							 NULL, NULL, NULL);
	DefineCustomIntVariable("prolly.kept_checkouts",
							"How many detached checkouts' hidden branches a database keeps after their sessions leave.",
							"A later checkout of the commit is then a branch switch, and a move or first use can share a kept checkout's tables instead of writing their rows.  0 keeps none.",
							&prolly_kept_checkouts,
							4, 0, 1000,
							PGC_SUSET,
							0,
							NULL, NULL, NULL);
	DefineCustomBoolVariable("prolly.fold_verify_fatal",
							 "Makes a fold the verifier finds wrong a PANIC, for testing.",
							 NULL,
							 &prolly_fold_verify_fatal,
							 false,
							 PGC_SUSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
	defined = true;
}

/* the store's sizes the trigger weighs */
typedef struct Sizes
{
	uint64		segments;		/* the journal's segments that have chunks */
	uint64		unsettled;		/* the table files not settled */
	uint64		settled;		/* the settled table files */
} Sizes;

static void
journal_segment(const char *name, int nchunks, uint64 size, bool settled,
				void *arg)
{
	Sizes	   *s = arg;

	if (nchunks != 0)
		s->segments += size;
}

static void
settled_file(const char *name, int nchunks, uint64 size, bool settled,
			 void *arg)
{
	Sizes	   *s = arg;

	if (settled)
		s->settled += size;
	else
		s->unsettled += size;
}

static bool
collection_due(void)
{
	const ProllyChunkStoreRoutine *cs = prolly_chunk_store();
	ProllyChunkStore *store;
	Sizes		s = {0, 0, 0};
	uint64		collected;

	if (prolly_gc_journal_threshold < 0)
		return false;
	store = PROLLY_CALL(cs, open, MyDatabaseId, MyDatabaseTableSpace);
	PROLLY_CALL(cs, segments, store, false, journal_segment, &s);
	PROLLY_CALL(cs, table_files, store, settled_file, &s);
	collected = s.segments + s.unsettled;
	return collected > 0 &&
		collected > (uint64) prolly_gc_journal_threshold * 1024 * 1024 &&
		collected > s.settled * prolly_gc_growth_scale_factor;
}

/*
 * A collection is refused while another of the store runs (its lock tried,
 * never waited for): several workers find the same store due at once.
 * The one that runs collects what was due, so the others skip it quietly,
 * counted (auto_collections_skipped), rather than each reporting the
 * refusal as an error; dolt_gc() still reports it.
 */
/*
 * History past the database's window severed (the history window's
 * maintain()), before the collection: as the database's settings say,
 * never a session's.
 */
/* The history window wants this visit's collection a full one. */
static bool full_due = false;

static void
sever(void)
{
	ProllyDatabaseComponents dc;

	full_due = false;
	prolly_database_components(&dc);
	if (dc.history_window == NULL || !PROLLY_HAS(dc.history_window, maintain))
		return;
	(void) PROLLY_CALL(dc.history_window, maintain);
	/* (asked here: a collection runs in a transaction that hasn't read) */
	full_due = PROLLY_HAS(dc.history_window, collect_due) &&
		PROLLY_CALL(dc.history_window, collect_due);
}

static void
collect(void)
{
	ProllyDatabaseComponents dc;
	ProllyGcResult r;
	bool		full = full_due;

	/*
	 * (full where the history window wants severed history freed, no
	 * oftener than it allows)
	 */
	full_due = false;
	prolly_database_components(&dc);
	if (!full && !collection_due())
		return;
	VacuumUpdateCosts();
	VacuumCostBalance = 0;
	if (!PROLLY_CALL(dc.reclamation, collect, full, true, &r))
	{
		pgstat_count_prolly(PGSTAT_PROLLY_AUTO_COLLECTIONS_SKIPPED, 1);
		ereport(DEBUG1,
				(errmsg_internal("automatic collection of database \"%s\" skipped: another collection of its chunk store is running",
								 get_database_name(MyDatabaseId))));
		return;
	}
	if (full && PROLLY_HAS(dc.history_window, collected))
		PROLLY_CALL(dc.history_window, collected);
	/* (then, a step of its own, the records of the commits it took) */
	if (PROLLY_HAS(dc.refs, forget_records))
		(void) PROLLY_CALL(dc.refs, forget_records);
	ereport(DEBUG1,
			(errmsg_internal("automatic collection of database \"%s\": %llu chunks kept, %llu freed, %llu bytes freed, appends held off %llu us",
							 get_database_name(MyDatabaseId),
							 (unsigned long long) r.chunks_kept,
							 (unsigned long long) r.chunks_freed,
							 (unsigned long long) r.bytes_freed,
							 (unsigned long long) r.window_usec)));
}

/*
 * The sizes are read again rather than kept from collection_due(): a
 * collection run since has sealed and rewritten the journal.
 */
static bool
move_due(const ProllyDatabaseComponents *dc)
{
	const ProllyChunkStoreRoutine *cs = prolly_chunk_store();
	ProllyChunkStore *store;
	Sizes		s = {0, 0, 0};

	if (prolly_journal_move_threshold < 0 || !PROLLY_HAS(dc->reclamation, move))
		return false;
	store = PROLLY_CALL(cs, open, MyDatabaseId, MyDatabaseTableSpace);
	PROLLY_CALL(cs, segments, store, false, journal_segment, &s);
	return s.segments > (uint64) prolly_journal_move_threshold * 1024 * 1024;
}

static void
move(void)
{
	ProllyDatabaseComponents dc;
	ProllyGcResult r;

	prolly_database_components(&dc);
	if (!move_due(&dc))
		return;
	VacuumUpdateCosts();
	VacuumCostBalance = 0;
	PROLLY_CALL(dc.reclamation, move, &r);
	ereport(DEBUG1,
			(errmsg_internal("automatic move of the chunk journal of database \"%s\": %llu chunks moved, appends held off %llu us",
							 get_database_name(MyDatabaseId),
							 (unsigned long long) r.chunks_kept,
							 (unsigned long long) r.window_usec)));
}

/*
 * The table files conjoined, the smallest, if there are more than
 * prolly.conjoin_max_files (which conjoin() checks from the manifest, not
 * opening the files).
 */
static void
conjoin(void)
{
	const ProllyChunkStoreRoutine *cs = prolly_chunk_store();

	if (prolly_conjoin_max_files < 0)
		return;
	VacuumUpdateCosts();
	VacuumCostBalance = 0;
	PROLLY_CALL(cs, conjoin, PROLLY_CALL(cs, open, MyDatabaseId,
										 MyDatabaseTableSpace),
				prolly_conjoin_max_files);
}

/* the tablespaces a database's page stores may be in */
static List *
store_tablespaces(void)
{
	Relation	rel = table_open(TableSpaceRelationId, AccessShareLock);
	TableScanDesc scan = table_beginscan_catalog(rel, 0, NULL);
	HeapTuple	tup;
	List	   *spcs = NIL;

	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Oid			spc = ((Form_pg_tablespace) GETSTRUCT(tup))->oid;

		if (spc != GLOBALTABLESPACE_OID)
			spcs = lappend_oid(spcs, spc);
	}
	table_endscan(scan);
	table_close(rel, AccessShareLock);
	return spcs;
}

/*
 * Settle the captured roots of the current database's page stores, in
 * every tablespace, so that none needs the commit log to say what became
 * of a transaction older than any running now (bmap_roots_reclaim()):
 * before the database's oldest transaction id advances, and the log may
 * forget those.
 */
void
prolly_settle_page_stores(void)
{
	List	   *spcs = store_tablespaces();
	ListCell   *lc;

	foreach(lc, spcs)
	{
		RelFileLocator store = {lfirst_oid(lc), MyDatabaseId, InvalidRelFileNumber};

		(void) bmap_roots_reclaim(store, true);
	}
	list_free(spcs);
}

/*
 * Sweep the current database's page stores, in every tablespace that has
 * one (only those bmap_sweep_due() names, with due_only), adding to total.
 */
void
prolly_sweep_page_stores(bool due_only, BmapSweep *total)
{
	List	   *spcs = store_tablespaces();
	ListCell   *lc;

	memset(total, 0, sizeof(*total));
	foreach(lc, spcs)
	{
		BmapSweep	r;

		if (due_only && !bmap_sweep_due(lfirst_oid(lc), MyDatabaseId))
			continue;
		bmap_sweep(lfirst_oid(lc), MyDatabaseId, &r);
		total->relations += r.relations;
		total->freed += r.freed;
		total->recounted += r.recounted;
		total->truncated += r.truncated;
		total->moved += r.moved;
		total->stuck += r.stuck;
	}
}

static void
sweep(void)
{
	BmapSweep	r;

	prolly_sweep_page_stores(true, &r);
	ereport(DEBUG1,
			(errmsg_internal("automatic sweep of database \"%s\": %llu blocks freed, %llu counts rewritten, %llu blocks moved, %llu blocks truncated",
							 get_database_name(MyDatabaseId),
							 (unsigned long long) r.freed,
							 (unsigned long long) r.recounted,
							 (unsigned long long) r.moved,
							 (unsigned long long) r.truncated)));
}

/*
 * Drain the current database's release queues, in each tablespace at most
 * prolly.release_max_per_pass entries, a step at a time under the
 * cost-based delay.  A queue another backend drains, or a sweep counts, is
 * passed over.
 */
void
prolly_release_page_stores(BmapRelease *total)
{
	List	   *spcs = store_tablespaces();
	ListCell   *lc;

	memset(total, 0, sizeof(*total));
	VacuumUpdateCosts();
	VacuumCostBalance = 0;
	foreach(lc, spcs)
	{
		BmapRelease done = {0};

		while ((prolly_release_max_per_pass < 0 ||
				done.entries < (uint64) prolly_release_max_per_pass) &&
			   bmap_release_step(lfirst_oid(lc), MyDatabaseId, false, &done))
			vacuum_delay_point(false);
		total->entries += done.entries;
		total->freed += done.freed;
		total->unrefs += done.unrefs;
	}
}

static void
release(void)
{
	BmapRelease r;

	prolly_release_page_stores(&r);
	ereport(DEBUG1,
			(errmsg_internal("automatic release of database \"%s\": %llu entries, %llu blocks freed, %llu counts set",
							 get_database_name(MyDatabaseId),
							 (unsigned long long) r.entries,
							 (unsigned long long) r.freed,
							 (unsigned long long) r.unrefs)));
}

/* A log of fewer entries than this is never due, whatever its table's size. */
#define AUTOFOLD_MIN_ENTRIES	1000

/*
 * Is a versioned table's change log past the threshold?  The entries no
 * fold has consumed (the change log's estimate, from a sample of its
 * pages) against the table's rows: what the next fold would read against
 * what a full one would.  One saying to read the table in full is due.
 */
static bool
autofold_table_due(Relation rel)
{
	PgStat_StatTabEntry *stats;
	double		pending;
	double		rows;

	if (rel->rd_options &&
		!((StdRdOptions *) rel->rd_options)->autovacuum.enabled)
		return false;
	pending = prolly_change_log_pending(rel);
	if (pending < 0)
		return true;
	if (pending < AUTOFOLD_MIN_ENTRIES)
		return false;
	stats = pgstat_fetch_stat_tabentry(RelationGetRelid(rel));
	rows = stats ? (double) stats->live_tuples : 0;
	rows = Max(rows, rel->rd_rel->reltuples);
	return pending > prolly_autofold_threshold * rows;
}

/*
 * The working set folded, once a versioned table's log is past
 * prolly.autofold_threshold: a checkpoint of the worker's branch (the
 * default), every table folded from its baseline, so the next commit's
 * fold reads only what came after.
 */
static void
autofold(void)
{
	Relation	classrel;
	TableScanDesc scan;
	HeapTuple	tup;
	bool		due = false;
	ProllyDatabaseComponents components;
	ProllyNodeRef root;

	if (prolly_autofold_threshold <= 0)
		return;
	PushActiveSnapshot(GetTransactionSnapshot());
	classrel = table_open(RelationRelationId, AccessShareLock);
	scan = table_beginscan_catalog(classrel, 0, NULL);
	while (!due && (tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class form = (Form_pg_class) GETSTRUCT(tup);
		Relation	rel;

		if (form->relam != PROLLY_TABLE_AM_OID ||
			form->relkind != RELKIND_RELATION)
			continue;
		rel = try_table_open(form->oid, AccessShareLock);
		if (rel == NULL)
			continue;
		due = autofold_table_due(rel);
		table_close(rel, AccessShareLock);
	}
	table_endscan(scan);
	table_close(classrel, AccessShareLock);

	/*
	 * (the scan's snapshots let go before the fold, which reads by one of
	 * its own: one from before a writer the fold sees committed, as one
	 * kept while the scan waited for a table's lock would be, would hold
	 * the xmin horizon below that writer, and the fold's trim of the log
	 * would keep its rows, which no fold after, finding nothing new, trims)
	 */
	PopActiveSnapshot();
	if (due)
	{
		bool		folded = true;

		prolly_database_components(&components);
		if (PROLLY_HAS(components.refs, checkpoint_unless_moved))
		{
			/*
			 * (a fold given up rolled back: the log's entries it consumed
			 * given back, for the next fold)
			 */
			MemoryContext cxt = CurrentMemoryContext;
			ResourceOwner owner = CurrentResourceOwner;

			BeginInternalSubTransaction(NULL);
			MemoryContextSwitchTo(cxt);
			PG_TRY();
			{
				folded = components.refs->checkpoint_unless_moved(&root);
			}
			PG_CATCH();
			{
				/*
				 * (a relation's lock another holds, which the fold doesn't
				 * wait for: given up, as if the refs were held)
				 */
				Assert(!RelationLocksNoWait);
				if (geterrcode() != ERRCODE_LOCK_NOT_AVAILABLE ||
					!RelationLockRefused)
					PG_RE_THROW();
				MemoryContextSwitchTo(cxt);
				FlushErrorState();
				folded = false;
			}
			PG_END_TRY();
			if (folded)
				ReleaseCurrentSubTransaction();
			else
				RollbackAndReleaseCurrentSubTransaction();
			MemoryContextSwitchTo(cxt);
			CurrentResourceOwner = owner;
		}
		else
			PROLLY_CALL(components.refs, checkpoint, &root);
		if (folded)
			ereport(DEBUG1,
					(errmsg_internal("automatic fold of database \"%s\"",
									 get_database_name(MyDatabaseId))));
		else
			ereport(DEBUG1,
					(errmsg_internal("automatic fold of database \"%s\" given up: the refs held, the branch moved, or a relation locked",
									 get_database_name(MyDatabaseId))));
	}
}

/* The default branch's folds checked against full ones, a sample of tables. */
static void
verify(void)
{
	int			mismatches;

	if (prolly_fold_verify_rate <= 0)
		return;
	mismatches = prolly_verify_folds(prolly_fold_verify_rate, NULL, NULL);
	ereport(DEBUG1,
			(errmsg_internal("automatic fold verification of database \"%s\": %d mismatches",
							 get_database_name(MyDatabaseId), mismatches)));
}

void
prolly_autovacuum(void)
{
	void		(*steps[]) (void) = {autofold, sever, collect, move, conjoin,
	release, sweep, verify};

	for (int i = 0; i < lengthof(steps); i++)
	{
		StartTransactionCommand();
		PG_TRY();
		{
			steps[i] ();
			CommitTransactionCommand();
		}
		PG_CATCH();
		{
			HOLD_INTERRUPTS();
			EmitErrorReport();
			AbortOutOfAnyTransaction();
			FlushErrorState();
			RESUME_INTERRUPTS();
		}
		PG_END_TRY();
	}
}
