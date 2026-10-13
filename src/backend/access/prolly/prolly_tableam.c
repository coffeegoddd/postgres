/*-------------------------------------------------------------------------
 *
 * prolly_tableam.c
 *	  Table access method adapter for prolly engines.
 *
 * One TableAmRoutine serves every prolly table AM (prolly_d1, prolly_d2,
 * ...); the AM a relation uses selects its components through
 * prolly_config.h.  This file translates table AM calls into calls on the
 * prolly interfaces and holds no engine logic.
 *
 * Where a callback maps directly onto an engine callback it calls through
 * the engine interface already.  The rest need translation that isn't
 * designed yet (specs/02-postgres-integration.md, A1) and raise
 * ERRCODE_FEATURE_NOT_SUPPORTED naming the table AM, the engine and the
 * callback.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_tableam.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/multixact.h"
#include "access/prolly_config.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "commands/defrem.h"
#include "utils/fmgrprotos.h"
#include "utils/rel.h"

pg_noreturn static void
not_implemented(Relation rel, const char *callback)
{
	ProllyComponents components;

	prolly_components_for_relation(rel, &components);
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("table access method \"%s\" does not implement %s yet",
					get_am_name(rel->rd_rel->relam), callback),
			 errdetail("The table uses engine \"%s\".",
					   components.engine->info.name)));
	pg_unreachable();
}

/* ------------------------------------------------------------------------
 * Slots and scans
 * ------------------------------------------------------------------------
 */

static const TupleTableSlotOps *
prolly_slot_callbacks(Relation rel)
{
	not_implemented(rel, "slot_callbacks");
}

static TableScanDesc
prolly_scan_begin(Relation rel, Snapshot snapshot, int nkeys,
				  ScanKeyData *key, ParallelTableScanDesc pscan, uint32 flags)
{
	not_implemented(rel, "scan_begin");
}

static void
prolly_scan_end(TableScanDesc scan)
{
	not_implemented(scan->rs_rd, "scan_end");
}

static void
prolly_scan_rescan(TableScanDesc scan, ScanKeyData *key, bool set_params,
				   bool allow_strat, bool allow_sync, bool allow_pagemode)
{
	not_implemented(scan->rs_rd, "scan_rescan");
}

static bool
prolly_scan_getnextslot(TableScanDesc scan, ScanDirection direction,
						TupleTableSlot *slot)
{
	not_implemented(scan->rs_rd, "scan_getnextslot");
}

static void
prolly_scan_set_tidrange(TableScanDesc scan, ItemPointer mintid,
						 ItemPointer maxtid)
{
	not_implemented(scan->rs_rd, "scan_set_tidrange");
}

static bool
prolly_scan_getnextslot_tidrange(TableScanDesc scan, ScanDirection direction,
								 TupleTableSlot *slot)
{
	not_implemented(scan->rs_rd, "scan_getnextslot_tidrange");
}

static Size
prolly_parallelscan_estimate(Relation rel)
{
	not_implemented(rel, "parallelscan_estimate");
}

static Size
prolly_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan)
{
	not_implemented(rel, "parallelscan_initialize");
}

static void
prolly_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan)
{
	not_implemented(rel, "parallelscan_reinitialize");
}

/* ------------------------------------------------------------------------
 * Index scans and fetching rows by TID
 * ------------------------------------------------------------------------
 */

static void
prolly_index_scan_begin(IndexScanDesc scan, uint32 flags)
{
	not_implemented(scan->heapRelation, "index_scan_begin");
}

static void
prolly_index_scan_reset(IndexScanDesc scan)
{
	not_implemented(scan->heapRelation, "index_scan_reset");
}

static void
prolly_index_scan_end(IndexScanDesc scan)
{
	not_implemented(scan->heapRelation, "index_scan_end");
}

static bool
prolly_fetch_tid(Relation rel, ItemPointer tid, Snapshot snapshot,
				 bool *all_dead)
{
	not_implemented(rel, "fetch_tid");
}

static bool
prolly_tuple_fetch_row_version(Relation rel, ItemPointer tid,
							   Snapshot snapshot, TupleTableSlot *slot)
{
	not_implemented(rel, "tuple_fetch_row_version");
}

static bool
prolly_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
	not_implemented(scan->rs_rd, "tuple_tid_valid");
}

static void
prolly_tuple_get_latest_tid(TableScanDesc scan, ItemPointer tid)
{
	not_implemented(scan->rs_rd, "tuple_get_latest_tid");
}

static bool
prolly_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
								Snapshot snapshot)
{
	not_implemented(rel, "tuple_satisfies_snapshot");
}

static TransactionId
prolly_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
	not_implemented(rel, "index_delete_tuples");
}

/* ------------------------------------------------------------------------
 * Row modification
 * ------------------------------------------------------------------------
 */

static void
prolly_tuple_insert(Relation rel, TupleTableSlot *slot, CommandId cid,
					uint32 options, BulkInsertStateData *bistate)
{
	not_implemented(rel, "tuple_insert");
}

static void
prolly_tuple_insert_speculative(Relation rel, TupleTableSlot *slot,
								CommandId cid, uint32 options,
								BulkInsertStateData *bistate, uint32 specToken)
{
	not_implemented(rel, "tuple_insert_speculative");
}

static void
prolly_tuple_complete_speculative(Relation rel, TupleTableSlot *slot,
								  uint32 specToken, bool succeeded)
{
	not_implemented(rel, "tuple_complete_speculative");
}

static void
prolly_multi_insert(Relation rel, TupleTableSlot **slots, int nslots,
					CommandId cid, uint32 options,
					BulkInsertStateData *bistate)
{
	not_implemented(rel, "multi_insert");
}

static TM_Result
prolly_tuple_delete(Relation rel, ItemPointer tid, CommandId cid,
					uint32 options, Snapshot snapshot, Snapshot crosscheck,
					bool wait, TM_FailureData *tmfd)
{
	not_implemented(rel, "tuple_delete");
}

static TM_Result
prolly_tuple_update(Relation rel, ItemPointer otid, TupleTableSlot *slot,
					CommandId cid, uint32 options, Snapshot snapshot,
					Snapshot crosscheck, bool wait, TM_FailureData *tmfd,
					LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes)
{
	not_implemented(rel, "tuple_update");
}

static TM_Result
prolly_tuple_lock(Relation rel, ItemPointer tid, Snapshot snapshot,
				  TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
				  LockWaitPolicy wait_policy, uint8 flags,
				  TM_FailureData *tmfd)
{
	not_implemented(rel, "tuple_lock");
}

static void
prolly_finish_bulk_insert(Relation rel, uint32 options)
{
	not_implemented(rel, "finish_bulk_insert");
}

/* ------------------------------------------------------------------------
 * Relation storage: these map directly onto the engine interface.
 * ------------------------------------------------------------------------
 */

static void
prolly_relation_set_new_filelocator(Relation rel,
									const RelFileLocator *newrlocator,
									char persistence,
									TransactionId *freezeXid,
									MultiXactId *minmulti)
{
	ProllyComponents components;
	const ProllyEngineRoutine *engine;

	/* Rows carry no xids, so there is nothing to freeze (A1). */
	*freezeXid = InvalidTransactionId;
	*minmulti = InvalidMultiXactId;

	prolly_components_for_relation(rel, &components);
	engine = components.engine;
	PROLLY_CALL(engine, tree_create, rel, newrlocator, persistence);
}

static void
prolly_relation_nontransactional_truncate(Relation rel)
{
	ProllyComponents components;
	const ProllyEngineRoutine *engine;

	prolly_components_for_relation(rel, &components);
	engine = components.engine;
	PROLLY_CALL(engine, tree_truncate, rel);
}

static void
prolly_relation_copy_data(Relation rel, const RelFileLocator *newrlocator)
{
	ProllyComponents components;
	const ProllyEngineRoutine *engine;

	prolly_components_for_relation(rel, &components);
	engine = components.engine;
	PROLLY_CALL(engine, tree_copy, rel, newrlocator);
}

static void
prolly_relation_copy_for_cluster(Relation OldTable, Relation NewTable,
								 Relation OldIndex, bool use_sort,
								 TransactionId OldestXmin, Snapshot snapshot,
								 TransactionId *xid_cutoff,
								 MultiXactId *multi_cutoff,
								 double *num_tuples, double *tups_vacuumed,
								 double *tups_recently_dead)
{
	not_implemented(OldTable, "relation_copy_for_cluster");
}

static void
prolly_relation_vacuum(Relation rel, const VacuumParams *params,
					   BufferAccessStrategy bstrategy)
{
	ProllyComponents components;
	const ProllyEngineRoutine *engine;

	prolly_components_for_relation(rel, &components);
	engine = components.engine;
	PROLLY_CALL(engine, vacuum, rel, params, bstrategy);
}

/* ------------------------------------------------------------------------
 * ANALYZE, index builds, sizes and sampling
 * ------------------------------------------------------------------------
 */

static bool
prolly_scan_analyze_next_block(TableScanDesc scan, ReadStream *stream)
{
	not_implemented(scan->rs_rd, "scan_analyze_next_block");
}

static bool
prolly_scan_analyze_next_tuple(TableScanDesc scan, double *liverows,
							   double *deadrows, TupleTableSlot *slot)
{
	not_implemented(scan->rs_rd, "scan_analyze_next_tuple");
}

static double
prolly_index_build_range_scan(Relation table_rel, Relation index_rel,
							  IndexInfo *index_info, bool allow_sync,
							  bool anyvisible, bool progress,
							  BlockNumber start_blockno, BlockNumber numblocks,
							  IndexBuildCallback callback,
							  void *callback_state, TableScanDesc scan)
{
	not_implemented(table_rel, "index_build_range_scan");
}

static void
prolly_index_validate_scan(Relation table_rel, Relation index_rel,
						   IndexInfo *index_info, Snapshot snapshot,
						   ValidateIndexState *state)
{
	not_implemented(table_rel, "index_validate_scan");
}

static uint64
prolly_relation_size(Relation rel, ForkNumber forkNumber)
{
	not_implemented(rel, "relation_size");
}

static bool
prolly_relation_needs_toast_table(Relation rel)
{
	not_implemented(rel, "relation_needs_toast_table");
}

static Oid
prolly_relation_toast_am(Relation rel)
{
	not_implemented(rel, "relation_toast_am");
}

static void
prolly_relation_fetch_toast_slice(Relation toastrel, Oid8 valueid,
								  int32 attrsize, int32 sliceoffset,
								  int32 slicelength, varlena *result)
{
	not_implemented(toastrel, "relation_fetch_toast_slice");
}

static void
prolly_relation_estimate_size(Relation rel, int32 *attr_widths,
							  BlockNumber *pages, double *tuples,
							  double *allvisfrac)
{
	not_implemented(rel, "relation_estimate_size");
}

static bool
prolly_scan_bitmap_next_tuple(TableScanDesc scan, TupleTableSlot *slot,
							  bool *recheck, uint64 *lossy_pages,
							  uint64 *exact_pages)
{
	not_implemented(scan->rs_rd, "scan_bitmap_next_tuple");
}

static bool
prolly_scan_sample_next_block(TableScanDesc scan, SampleScanState *scanstate)
{
	not_implemented(scan->rs_rd, "scan_sample_next_block");
}

static bool
prolly_scan_sample_next_tuple(TableScanDesc scan, SampleScanState *scanstate,
							  TupleTableSlot *slot)
{
	not_implemented(scan->rs_rd, "scan_sample_next_tuple");
}

/* ------------------------------------------------------------------------
 * The routine and the handlers
 * ------------------------------------------------------------------------
 */

static const TableAmRoutine prolly_methods = {
	.type = T_TableAmRoutine,

	.slot_callbacks = prolly_slot_callbacks,

	.scan_begin = prolly_scan_begin,
	.scan_end = prolly_scan_end,
	.scan_rescan = prolly_scan_rescan,
	.scan_getnextslot = prolly_scan_getnextslot,

	.scan_set_tidrange = prolly_scan_set_tidrange,
	.scan_getnextslot_tidrange = prolly_scan_getnextslot_tidrange,

	.parallelscan_estimate = prolly_parallelscan_estimate,
	.parallelscan_initialize = prolly_parallelscan_initialize,
	.parallelscan_reinitialize = prolly_parallelscan_reinitialize,

	.index_scan_begin = prolly_index_scan_begin,
	.index_scan_reset = prolly_index_scan_reset,
	.index_scan_end = prolly_index_scan_end,
	.fetch_tid = prolly_fetch_tid,

	.tuple_fetch_row_version = prolly_tuple_fetch_row_version,
	.tuple_tid_valid = prolly_tuple_tid_valid,
	.tuple_get_latest_tid = prolly_tuple_get_latest_tid,
	.tuple_satisfies_snapshot = prolly_tuple_satisfies_snapshot,
	.index_delete_tuples = prolly_index_delete_tuples,

	.tuple_insert = prolly_tuple_insert,
	.tuple_insert_speculative = prolly_tuple_insert_speculative,
	.tuple_complete_speculative = prolly_tuple_complete_speculative,
	.multi_insert = prolly_multi_insert,
	.tuple_delete = prolly_tuple_delete,
	.tuple_update = prolly_tuple_update,
	.tuple_lock = prolly_tuple_lock,
	.finish_bulk_insert = prolly_finish_bulk_insert,

	.relation_set_new_filelocator = prolly_relation_set_new_filelocator,
	.relation_nontransactional_truncate = prolly_relation_nontransactional_truncate,
	.relation_copy_data = prolly_relation_copy_data,
	.relation_copy_for_cluster = prolly_relation_copy_for_cluster,
	.relation_vacuum = prolly_relation_vacuum,
	.scan_analyze_next_block = prolly_scan_analyze_next_block,
	.scan_analyze_next_tuple = prolly_scan_analyze_next_tuple,
	.index_build_range_scan = prolly_index_build_range_scan,
	.index_validate_scan = prolly_index_validate_scan,

	.relation_size = prolly_relation_size,
	.relation_needs_toast_table = prolly_relation_needs_toast_table,
	.relation_toast_am = prolly_relation_toast_am,
	.relation_fetch_toast_slice = prolly_relation_fetch_toast_slice,

	.relation_estimate_size = prolly_relation_estimate_size,

	.scan_bitmap_next_tuple = prolly_scan_bitmap_next_tuple,
	.scan_sample_next_block = prolly_scan_sample_next_block,
	.scan_sample_next_tuple = prolly_scan_sample_next_tuple,
};

/*
 * Every prolly table AM returns the same routine; the relation's AM name
 * selects the engine (prolly_config.c).
 */
Datum
prolly_d1_tableam_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&prolly_methods);
}

Datum
prolly_d2_tableam_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&prolly_methods);
}

Datum
prolly_d3a_tableam_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&prolly_methods);
}

Datum
prolly_d3b_tableam_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&prolly_methods);
}

Datum
prolly_d4_tableam_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&prolly_methods);
}
