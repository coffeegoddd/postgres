/*-------------------------------------------------------------------------
 *
 * prolly_chunkpages.c
 *	  A chunk store in pages of our own layout, through the buffer manager:
 *	  one of three history stores compared (with table rows and dirstore).
 *
 * The store is an append-only byte log across a relation's pages.  Block 0
 * is a meta page holding the log's end; every other page is a standard
 * page header followed by log bytes, so the unused tail is a page hole.
 * cp_append writes bytes at the log's end (a chunk may span pages) and
 * returns their offset; cp_read reads them back.  Changes are logged with
 * generic WAL records, so checkpoints, crash recovery, standbys and base
 * backups take the pages as they take any relation's, with no hook.  A
 * hash index (an ordinary table) maps a chunk's hash to its offset.
 *
 * The relation's access method, prolly_chunks, exists so the pages are
 * ours: nothing reads them as heap pages.  It creates and sizes the files,
 * VACUUM and ANALYZE pass it by, and everything else refuses.
 *
 * contrib/prolly_chunkpages/prolly_chunkpages.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/generic_xlog.h"
#include "access/multixact.h"
#include "access/relation.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "commands/vacuum.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/rel.h"

PG_MODULE_MAGIC_EXT(.name = "prolly_chunkpages",.version = PG_VERSION);

PG_FUNCTION_INFO_V1(prolly_chunks_handler);
PG_FUNCTION_INFO_V1(cp_append);
PG_FUNCTION_INFO_V1(cp_read);
PG_FUNCTION_INFO_V1(cp_end);

#define META_BLOCK	0
#define AREA_START	SizeOfPageHeaderData
#define AREA		(BLCKSZ - AREA_START)

static void
refuse(const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("a chunk page relation does not support %s", what)));
}

/* ------------------------------------------------------- the access method */

static TableScanDesc
cp_scan_begin(Relation rel, Snapshot snapshot, int nkeys, ScanKeyData *key, ParallelTableScanDesc pscan, uint32 flags)
{
	TableScanDesc scan = palloc0(sizeof(TableScanDescData));

	/* a scan of the log's relation finds no tuples (ANALYZE, a SELECT) */
	scan->rs_rd = rel;
	scan->rs_snapshot = snapshot;
	scan->rs_nkeys = nkeys;
	scan->rs_flags = flags;
	scan->rs_parallel = pscan;
	return scan;
}

static void
cp_scan_end(TableScanDesc scan)
{
	pfree(scan);
}

static void
cp_scan_rescan(TableScanDesc scan, ScanKeyData *key, bool set_params, bool allow_strat, bool allow_sync, bool allow_pagemode)
{
}

static bool
cp_scan_getnextslot(TableScanDesc scan, ScanDirection direction, TupleTableSlot *slot)
{
	ExecClearTuple(slot);
	return false;
}

static void
cp_scan_set_tidrange(TableScanDesc scan, ItemPointer mintid, ItemPointer maxtid)
{
	refuse("scan_set_tidrange");
}

static bool
cp_scan_getnextslot_tidrange(TableScanDesc scan, ScanDirection direction, TupleTableSlot *slot)
{
	refuse("scan_getnextslot_tidrange");
	pg_unreachable();
}

static Size
cp_parallelscan_estimate(Relation rel)
{
	refuse("parallelscan_estimate");
	pg_unreachable();
}

static Size
cp_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan)
{
	refuse("parallelscan_initialize");
	pg_unreachable();
}

static void
cp_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan)
{
	refuse("parallelscan_reinitialize");
}

static void
cp_index_scan_begin(IndexScanDesc scan, uint32 flags)
{
	refuse("index_scan_begin");
}

static void
cp_index_scan_reset(IndexScanDesc scan)
{
	refuse("index_scan_reset");
}

static void
cp_index_scan_end(IndexScanDesc scan)
{
	refuse("index_scan_end");
}

static bool
cp_fetch_tid(Relation rel, ItemPointer tid, Snapshot snapshot, bool *all_dead)
{
	refuse("fetch_tid");
	pg_unreachable();
}

static bool
cp_tuple_fetch_row_version(Relation rel, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot)
{
	refuse("tuple_fetch_row_version");
	pg_unreachable();
}

static bool
cp_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
	refuse("tuple_tid_valid");
	pg_unreachable();
}

static void
cp_tuple_get_latest_tid(TableScanDesc scan, ItemPointer tid)
{
	refuse("tuple_get_latest_tid");
}

static bool
cp_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot, Snapshot snapshot)
{
	refuse("tuple_satisfies_snapshot");
	pg_unreachable();
}

static TransactionId
cp_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
	refuse("index_delete_tuples");
	pg_unreachable();
}

static void
cp_tuple_insert(Relation rel, TupleTableSlot *slot, CommandId cid, uint32 options, BulkInsertStateData *bistate)
{
	refuse("tuple_insert");
}

static void
cp_tuple_insert_speculative(Relation rel, TupleTableSlot *slot, CommandId cid, uint32 options, BulkInsertStateData *bistate, uint32 specToken)
{
	refuse("tuple_insert_speculative");
}

static void
cp_tuple_complete_speculative(Relation rel, TupleTableSlot *slot, uint32 specToken, bool succeeded)
{
	refuse("tuple_complete_speculative");
}

static void
cp_multi_insert(Relation rel, TupleTableSlot **slots, int nslots, CommandId cid, uint32 options, BulkInsertStateData *bistate)
{
	refuse("multi_insert");
}

static TM_Result
cp_tuple_delete(Relation rel, ItemPointer tid, CommandId cid, uint32 options, Snapshot snapshot, Snapshot crosscheck, bool wait, TM_FailureData *tmfd)
{
	refuse("tuple_delete");
	pg_unreachable();
}

static TM_Result
cp_tuple_update(Relation rel, ItemPointer otid, TupleTableSlot *slot, CommandId cid, uint32 options, Snapshot snapshot, Snapshot crosscheck, bool wait, TM_FailureData *tmfd, LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes)
{
	refuse("tuple_update");
	pg_unreachable();
}

static TM_Result
cp_tuple_lock(Relation rel, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, CommandId cid, LockTupleMode mode, LockWaitPolicy wait_policy, uint8 flags, TM_FailureData *tmfd)
{
	refuse("tuple_lock");
	pg_unreachable();
}

static void
cp_finish_bulk_insert(Relation rel, uint32 options)
{
	refuse("finish_bulk_insert");
}

static void
cp_relation_copy_data(Relation rel, const RelFileLocator *newrlocator)
{
	refuse("relation_copy_data");
}

static void
cp_relation_copy_for_cluster(Relation OldTable, Relation NewTable, Relation OldIndex, bool use_sort, TransactionId OldestXmin, Snapshot snapshot, TransactionId *xid_cutoff, MultiXactId *multi_cutoff, double *num_tuples, double *tups_vacuumed, double *tups_recently_dead)
{
	refuse("relation_copy_for_cluster");
}

static double
cp_index_build_range_scan(Relation table_rel, Relation index_rel, IndexInfo *index_info, bool allow_sync, bool anyvisible, bool progress, BlockNumber start_blockno, BlockNumber numblocks, IndexBuildCallback callback, void *callback_state, TableScanDesc scan)
{
	refuse("index_build_range_scan");
	pg_unreachable();
}

static void
cp_index_validate_scan(Relation table_rel, Relation index_rel, IndexInfo *index_info, Snapshot snapshot, ValidateIndexState *state)
{
	refuse("index_validate_scan");
}

static bool
cp_scan_bitmap_next_tuple(TableScanDesc scan, TupleTableSlot *slot, bool *recheck, uint64 *lossy_pages, uint64 *exact_pages)
{
	refuse("scan_bitmap_next_tuple");
	pg_unreachable();
}

static bool
cp_scan_sample_next_block(TableScanDesc scan, SampleScanState *scanstate)
{
	refuse("scan_sample_next_block");
	pg_unreachable();
}

static bool
cp_scan_sample_next_tuple(TableScanDesc scan, SampleScanState *scanstate, TupleTableSlot *slot)
{
	refuse("scan_sample_next_tuple");
	pg_unreachable();
}

static const TupleTableSlotOps *
cp_slot_callbacks(Relation relation)
{
	return &TTSOpsVirtual;
}

static void
cp_relation_set_new_filelocator(Relation rel, const RelFileLocator *rlocator, char persistence,
								TransactionId *freezeXid, MultiXactId *minmulti)
{
	SMgrRelation srel;

	*freezeXid = InvalidTransactionId;
	*minmulti = InvalidMultiXactId;
	srel = RelationCreateStorage(*rlocator, persistence, true);
	smgrclose(srel);
}

static void
cp_relation_nontransactional_truncate(Relation rel)
{
	RelationTruncate(rel, 0);
}

static void
cp_relation_vacuum(Relation rel, const VacuumParams *params, BufferAccessStrategy bstrategy)
{
	/* nothing to vacuum: the log holds no tuples */
}

static bool
cp_scan_analyze_next_block(TableScanDesc scan, ReadStream *stream)
{
	return false;
}

static bool
cp_scan_analyze_next_tuple(TableScanDesc scan, double *liverows, double *deadrows,
						   TupleTableSlot *slot)
{
	return false;
}

static uint64
cp_relation_size(Relation rel, ForkNumber forkNumber)
{
	return table_block_relation_size(rel, forkNumber);
}

static bool
cp_relation_needs_toast_table(Relation rel)
{
	return false;
}

static void
cp_relation_estimate_size(Relation rel, int32 *attr_widths, BlockNumber *pages,
						  double *tuples, double *allvisfrac)
{
	*pages = 0;
	*tuples = 0;
	*allvisfrac = 0;
}

static const TableAmRoutine cp_routine = {
	.type = T_TableAmRoutine,
	.slot_callbacks = cp_slot_callbacks,
	.scan_begin = cp_scan_begin,
	.scan_end = cp_scan_end,
	.scan_rescan = cp_scan_rescan,
	.scan_getnextslot = cp_scan_getnextslot,
	.scan_set_tidrange = cp_scan_set_tidrange,
	.scan_getnextslot_tidrange = cp_scan_getnextslot_tidrange,
	.parallelscan_estimate = cp_parallelscan_estimate,
	.parallelscan_initialize = cp_parallelscan_initialize,
	.parallelscan_reinitialize = cp_parallelscan_reinitialize,
	.index_scan_begin = cp_index_scan_begin,
	.index_scan_reset = cp_index_scan_reset,
	.index_scan_end = cp_index_scan_end,
	.fetch_tid = cp_fetch_tid,
	.tuple_fetch_row_version = cp_tuple_fetch_row_version,
	.tuple_tid_valid = cp_tuple_tid_valid,
	.tuple_get_latest_tid = cp_tuple_get_latest_tid,
	.tuple_satisfies_snapshot = cp_tuple_satisfies_snapshot,
	.index_delete_tuples = cp_index_delete_tuples,
	.tuple_insert = cp_tuple_insert,
	.tuple_insert_speculative = cp_tuple_insert_speculative,
	.tuple_complete_speculative = cp_tuple_complete_speculative,
	.multi_insert = cp_multi_insert,
	.tuple_delete = cp_tuple_delete,
	.tuple_update = cp_tuple_update,
	.tuple_lock = cp_tuple_lock,
	.finish_bulk_insert = cp_finish_bulk_insert,
	.relation_set_new_filelocator = cp_relation_set_new_filelocator,
	.relation_nontransactional_truncate = cp_relation_nontransactional_truncate,
	.relation_copy_data = cp_relation_copy_data,
	.relation_copy_for_cluster = cp_relation_copy_for_cluster,
	.relation_vacuum = cp_relation_vacuum,
	.scan_analyze_next_block = cp_scan_analyze_next_block,
	.scan_analyze_next_tuple = cp_scan_analyze_next_tuple,
	.index_build_range_scan = cp_index_build_range_scan,
	.index_validate_scan = cp_index_validate_scan,
	.relation_size = cp_relation_size,
	.relation_needs_toast_table = cp_relation_needs_toast_table,
	.relation_estimate_size = cp_relation_estimate_size,
	.scan_bitmap_next_tuple = cp_scan_bitmap_next_tuple,
	.scan_sample_next_block = cp_scan_sample_next_block,
	.scan_sample_next_tuple = cp_scan_sample_next_tuple,
};

Datum
prolly_chunks_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&cp_routine);
}

/* -------------------------------------------------------------- the log */

static Relation
open_store(Oid relid, LOCKMODE mode)
{
	Relation	rel = relation_open(relid, mode);

	if (rel->rd_tableam != &cp_routine)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a chunk page relation", RelationGetRelationName(rel))));
	return rel;
}

/* Block blk, locked exclusively; extended to it, and initialized, if new. */
static Buffer
log_buffer(Relation rel, BlockNumber blk)
{
	if (blk < RelationGetNumberOfBlocks(rel))
	{
		Buffer		buf = ReadBuffer(rel, blk);

		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		return buf;
	}
	return ExtendBufferedRelTo(BMR_REL(rel), MAIN_FORKNUM, NULL, 0, blk + 1, RBM_ZERO_AND_LOCK);
}

static uint64
meta_end(Page meta)
{
	uint64		end;

	memcpy(&end, (char *) meta + AREA_START, sizeof(end));
	return end;
}

/*
 * cp_append(rel regclass, data bytea) returns int8
 *
 * Writes data at the end of rel's log and returns its offset.  One writer
 * at a time (the relation's ExclusiveLock); readers are not blocked.
 */
Datum
cp_append(PG_FUNCTION_ARGS)
{
	Relation	rel = open_store(PG_GETARG_OID(0), ExclusiveLock);
	bytea	   *data = PG_GETARG_BYTEA_PP(1);
	const char *bytes = VARDATA_ANY(data);
	size_t		left = VARSIZE_ANY_EXHDR(data);
	Buffer		mbuf = log_buffer(rel, META_BLOCK);
	uint64		start,
				end;

	if (PageIsNew(BufferGetPage(mbuf)))
	{
		GenericXLogState *st = GenericXLogStart(rel);
		Page		meta = GenericXLogRegisterBuffer(st, mbuf, GENERIC_XLOG_FULL_IMAGE);

		PageInit(meta, BLCKSZ, 0);
		end = 0;
		memcpy((char *) meta + AREA_START, &end, sizeof(end));
		((PageHeader) meta)->pd_lower = AREA_START + sizeof(end);
		GenericXLogFinish(st);
	}
	start = end = meta_end(BufferGetPage(mbuf));
	while (left > 0)
	{
		BlockNumber blk = 1 + end / AREA;
		size_t		off = end % AREA,
					n = Min(left, AREA - off);
		Buffer		buf = log_buffer(rel, blk);
		GenericXLogState *st = GenericXLogStart(rel);
		Page		page = GenericXLogRegisterBuffer(st, buf, PageIsNew(BufferGetPage(buf)) ?
													 GENERIC_XLOG_FULL_IMAGE : 0);
		Page		meta = GenericXLogRegisterBuffer(st, mbuf, 0);

		if (PageIsNew(page))
			PageInit(page, BLCKSZ, 0);
		memcpy((char *) page + AREA_START + off, bytes, n);
		((PageHeader) page)->pd_lower = AREA_START + off + n;
		end += n;
		memcpy((char *) meta + AREA_START, &end, sizeof(end));
		GenericXLogFinish(st);
		UnlockReleaseBuffer(buf);
		bytes += n;
		left -= n;
	}
	UnlockReleaseBuffer(mbuf);
	relation_close(rel, NoLock);
	PG_RETURN_INT64((int64) start);
}

/* cp_read(rel regclass, loc int8, len int4) returns bytea */
Datum
cp_read(PG_FUNCTION_ARGS)
{
	Relation	rel = open_store(PG_GETARG_OID(0), AccessShareLock);
	uint64		at = (uint64) PG_GETARG_INT64(1);
	size_t		left = (size_t) PG_GETARG_INT32(2);
	bytea	   *out = palloc(VARHDRSZ + left);
	char	   *dst = VARDATA(out);

	SET_VARSIZE(out, VARHDRSZ + left);
	while (left > 0)
	{
		BlockNumber blk = 1 + at / AREA;
		size_t		off = at % AREA,
					n = Min(left, AREA - off);
		Buffer		buf = ReadBuffer(rel, blk);

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		memcpy(dst, (char *) BufferGetPage(buf) + AREA_START + off, n);
		UnlockReleaseBuffer(buf);
		dst += n;
		at += n;
		left -= n;
	}
	relation_close(rel, AccessShareLock);
	PG_RETURN_BYTEA_P(out);
}

/* cp_end(rel regclass) returns int8: the log's length */
Datum
cp_end(PG_FUNCTION_ARGS)
{
	Relation	rel = open_store(PG_GETARG_OID(0), AccessShareLock);
	uint64		end = 0;

	if (RelationGetNumberOfBlocks(rel) > 0)
	{
		Buffer		buf = ReadBuffer(rel, META_BLOCK);

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		if (!PageIsNew(BufferGetPage(buf)))
			end = meta_end(BufferGetPage(buf));
		UnlockReleaseBuffer(buf);
	}
	relation_close(rel, AccessShareLock);
	PG_RETURN_INT64((int64) end);
}
