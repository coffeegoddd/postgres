/*-------------------------------------------------------------------------
 *
 * prolly_autovacuum.c
 *	  Prolly's stores reclaimed by autovacuum.
 *
 * After its tables, a database's autovacuum worker collects the chunk
 * store when what a default collection would collect (the journal segment
 * appends go to, written since the last collection, and the table files
 * not settled, received since) has passed prolly.gc_journal_threshold and
 * the settled files' size times prolly.gc_growth_scale_factor: a default
 * collection, by the database's reclamation.  Only sizes are read, not the
 * journal.  It sweeps a page store when bmap_sweep_due()
 * says so.  Each runs in a transaction of its own, an error reported and
 * passed over, as autovacuum's tables' are; a collection reads under
 * autovacuum's cost-based delay.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_autovacuum.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/prolly_autovacuum.h"
#include "access/prolly_config.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/pg_tablespace.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "storage/bmap.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

int			prolly_gc_journal_threshold = 128;
double		prolly_gc_growth_scale_factor = 0.2;

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
	defined = true;
}

/* the store's sizes the trigger weighs */
typedef struct Sizes
{
	uint64		segment;		/* the segment appends go to */
	int			chunks;			/* whether it has any (0: none) */
	uint64		unsettled;		/* the table files not settled */
	uint64		settled;		/* the settled table files */
} Sizes;

static void
last_segment(const char *name, int nchunks, uint64 size, bool settled,
			 void *arg)
{
	Sizes	   *s = arg;

	s->segment = size;
	s->chunks = nchunks;
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
	Sizes		s = {0, 0, 0, 0};
	uint64		collected;

	if (prolly_gc_journal_threshold < 0)
		return false;
	store = PROLLY_CALL(cs, open, MyDatabaseId, MyDatabaseTableSpace);
	PROLLY_CALL(cs, segments, store, false, last_segment, &s);
	PROLLY_CALL(cs, table_files, store, settled_file, &s);
	collected = (s.chunks != 0 ? s.segment : 0) + s.unsettled;
	return collected > 0 &&
		collected > (uint64) prolly_gc_journal_threshold * 1024 * 1024 &&
		collected > s.settled * prolly_gc_growth_scale_factor;
}

static void
collect(void)
{
	ProllyDatabaseComponents dc;
	ProllyGcResult r;

	if (!collection_due())
		return;
	VacuumUpdateCosts();
	VacuumCostBalance = 0;
	prolly_database_components(&dc);
	PROLLY_CALL(dc.reclamation, collect, false, &r);
	ereport(DEBUG1,
			(errmsg_internal("automatic collection of database \"%s\": %llu chunks kept, %llu freed, %llu bytes freed, appends held off %llu us",
							 get_database_name(MyDatabaseId),
							 (unsigned long long) r.chunks_kept,
							 (unsigned long long) r.chunks_freed,
							 (unsigned long long) r.bytes_freed,
							 (unsigned long long) r.window_usec)));
}

/*
 * Sweep the current database's page stores, in every tablespace that has
 * one (only those bmap_sweep_due() names, with due_only), adding to total.
 */
void
prolly_sweep_page_stores(bool due_only, BmapSweep *total)
{
	Relation	rel = table_open(TableSpaceRelationId, AccessShareLock);
	TableScanDesc scan = table_beginscan_catalog(rel, 0, NULL);
	HeapTuple	tup;
	List	   *spcs = NIL;
	ListCell   *lc;

	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Oid			spc = ((Form_pg_tablespace) GETSTRUCT(tup))->oid;

		if (spc != GLOBALTABLESPACE_OID)
			spcs = lappend_oid(spcs, spc);
	}
	table_endscan(scan);
	table_close(rel, AccessShareLock);

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
		total->stuck += r.stuck;
	}
}

static void
sweep(void)
{
	BmapSweep	r;

	prolly_sweep_page_stores(true, &r);
	ereport(DEBUG1,
			(errmsg_internal("automatic sweep of database \"%s\": %llu blocks freed, %llu counts rewritten, %llu blocks truncated",
							 get_database_name(MyDatabaseId),
							 (unsigned long long) r.freed,
							 (unsigned long long) r.recounted,
							 (unsigned long long) r.truncated)));
}

void
prolly_autovacuum(void)
{
	void		(*steps[]) (void) = {collect, sweep};

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
