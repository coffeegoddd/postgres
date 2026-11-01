/*-------------------------------------------------------------------------
 *
 * chunk_log.c
 *	  The history store's bytes in pages of their own layout
 *	  (prolly/chunk_log.h), and the prolly_chunks table access method,
 *	  whose relations hold them.
 *
 * The access method exists so the pages are the log's: nothing reads them
 * as heap pages.  It creates, copies and truncates the relation's storage;
 * VACUUM and ANALYZE pass it by, a scan finds no tuples, and everything
 * else refuses.  The log's changes are WAL-logged with generic records.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/chunk_log.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/generic_xlog.h"
#include "access/multixact.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "prolly/branch_catalog.h"
#include "prolly/chunk_log.h"
#include "storage/bufmgr.h"
#include "storage/smgr.h"
#include "utils/fmgrprotos.h"
#include "utils/rel.h"

/* ------------------------------------------------------------ the pages */

#define CHUNK_LOG_MAGIC		0x50434C47	/* "PCLG" */
#define CHUNK_LOG_VERSION	1

#define CHUNK_PAGE_META		1
#define CHUNK_PAGE_DATA		2

#define META_BLOCK			0

/* The special space of each of the log's pages. */
typedef struct ChunkPageOpaqueData
{
	uint32		magic;			/* CHUNK_LOG_MAGIC */
	uint16		kind;			/* CHUNK_PAGE_META or CHUNK_PAGE_DATA */
	uint16		unused;
} ChunkPageOpaqueData;

typedef ChunkPageOpaqueData *ChunkPageOpaque;

/* The meta page's contents. */
typedef struct ChunkLogMetaData
{
	uint32		version;		/* CHUNK_LOG_VERSION */
	uint32		unused;
	uint64		end;			/* the log's length */
} ChunkLogMetaData;

/* The bytes of the log a data page holds, after its header. */
#define AREA_START		SizeOfPageHeaderData
#define AREA			(BLCKSZ - SizeOfPageHeaderData - \
						 MAXALIGN(sizeof(ChunkPageOpaqueData)))

#define LOC_BLOCK(loc)	((BlockNumber) (1 + (loc) / AREA))
#define LOC_OFFSET(loc)	((size_t) ((loc) % AREA))

static void
init_page(Page page, uint16 kind)
{
	ChunkPageOpaque opaque;

	PageInit(page, BLCKSZ, sizeof(ChunkPageOpaqueData));
	opaque = (ChunkPageOpaque) PageGetSpecialPointer(page);
	opaque->magic = CHUNK_LOG_MAGIC;
	opaque->kind = kind;
	opaque->unused = 0;
}

static void
check_page(Relation rel, BlockNumber blk, Page page, uint16 kind)
{
	ChunkPageOpaque opaque;

	if (PageIsNew(page) ||
		PageGetSpecialSize(page) != MAXALIGN(sizeof(ChunkPageOpaqueData)))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("block %u of \"%s\" is not a page of a chunk log",
						blk, RelationGetRelationName(rel))));
	opaque = (ChunkPageOpaque) PageGetSpecialPointer(page);
	if (opaque->magic != CHUNK_LOG_MAGIC || opaque->kind != kind)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("block %u of \"%s\" is not a %s page of a chunk log",
						blk, RelationGetRelationName(rel),
						kind == CHUNK_PAGE_META ? "meta" : "data")));
}

static ChunkLogMetaData *
page_meta(Relation rel, Page page)
{
	ChunkLogMetaData *meta;

	check_page(rel, META_BLOCK, page, CHUNK_PAGE_META);
	meta = (ChunkLogMetaData *) PageGetContents(page);
	if (meta->version != CHUNK_LOG_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("chunk log \"%s\" has format version %u, not %u",
						RelationGetRelationName(rel), meta->version,
						CHUNK_LOG_VERSION)));
	return meta;
}

/* Block blk, locked exclusively; the relation extended to it if needed. */
static Buffer
log_buffer(Relation rel, BlockNumber blk)
{
	Buffer		buf;

	if (blk < RelationGetNumberOfBlocks(rel))
	{
		buf = ReadBuffer(rel, blk);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		return buf;
	}
	return ExtendBufferedRelTo(BMR_REL(rel), MAIN_FORKNUM, NULL, 0, blk + 1,
							   RBM_ZERO_AND_LOCK);
}

/* ------------------------------------------------------------- the log */

static uint64
page_append(Relation rel, const void *data, size_t len)
{
	const char *bytes = data;
	Buffer		mbuf = log_buffer(rel, META_BLOCK);
	uint64		start,
				end;

	/*
	 * The transaction appending gets an id, so that its commit flushes the
	 * WAL of what it appended (one without would commit without a record)
	 */
	(void) GetCurrentTransactionId();

	if (PageIsNew(BufferGetPage(mbuf)))
	{
		GenericXLogState *st = GenericXLogStart(rel);
		Page		page = GenericXLogRegisterBuffer(st, mbuf,
													 GENERIC_XLOG_FULL_IMAGE);
		ChunkLogMetaData *meta;

		init_page(page, CHUNK_PAGE_META);
		meta = (ChunkLogMetaData *) PageGetContents(page);
		meta->version = CHUNK_LOG_VERSION;
		meta->unused = 0;
		meta->end = 0;
		((PageHeader) page)->pd_lower =
			(char *) meta + sizeof(ChunkLogMetaData) - (char *) page;
		GenericXLogFinish(st);
	}
	start = end = page_meta(rel, BufferGetPage(mbuf))->end;

	/* a page at a time, each change with the log's new end */
	while (len > 0)
	{
		BlockNumber blk = LOC_BLOCK(end);
		size_t		off = LOC_OFFSET(end),
					n = Min(len, AREA - off);
		Buffer		buf = log_buffer(rel, blk);
		bool		fresh = PageIsNew(BufferGetPage(buf)) || off == 0;
		GenericXLogState *st = GenericXLogStart(rel);
		Page		page = GenericXLogRegisterBuffer(st, buf,
													 fresh ? GENERIC_XLOG_FULL_IMAGE : 0);
		Page		mpage = GenericXLogRegisterBuffer(st, mbuf, 0);

		/*
		 * (a page past the log's end is new, or left by an append a crash
		 * cut short: either way written afresh)
		 */
		if (fresh)
			init_page(page, CHUNK_PAGE_DATA);
		else
			check_page(rel, blk, page, CHUNK_PAGE_DATA);
		memcpy((char *) page + AREA_START + off, bytes, n);
		((PageHeader) page)->pd_lower = AREA_START + off + n;
		end += n;
		((ChunkLogMetaData *) PageGetContents(mpage))->end = end;
		GenericXLogFinish(st);
		UnlockReleaseBuffer(buf);
		bytes += n;
		len -= n;
	}
	UnlockReleaseBuffer(mbuf);
	return start;
}

static uint64
page_end(Relation rel)
{
	Buffer		buf;
	uint64		end;

	if (RelationGetNumberOfBlocks(rel) == 0)
		return 0;
	buf = ReadBuffer(rel, META_BLOCK);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	end = PageIsNew(BufferGetPage(buf)) ? 0 :
		page_meta(rel, BufferGetPage(buf))->end;
	UnlockReleaseBuffer(buf);
	return end;
}

static void
page_read(Relation rel, uint64 loc, void *dst, size_t len)
{
	char	   *out = dst;
	uint64		end = page_end(rel);

	if (loc > end || len > end - loc)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("bytes %llu to %llu are past the end of chunk log \"%s\" (%llu)",
						(unsigned long long) loc,
						(unsigned long long) (loc + len),
						RelationGetRelationName(rel),
						(unsigned long long) end)));
	while (len > 0)
	{
		BlockNumber blk = LOC_BLOCK(loc);
		size_t		off = LOC_OFFSET(loc),
					n = Min(len, AREA - off);
		Buffer		buf = ReadBuffer(rel, blk);
		Page		page = BufferGetPage(buf);

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		check_page(rel, blk, page, CHUNK_PAGE_DATA);
		if (AREA_START + off + n > ((PageHeader) page)->pd_lower)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("block %u of chunk log \"%s\" ends before the log's end says",
							blk, RelationGetRelationName(rel))));
		memcpy(out, (char *) page + AREA_START + off, n);
		UnlockReleaseBuffer(buf);
		out += n;
		loc += n;
		len -= n;
	}
}

const ChunkLogRoutine PageChunkLog = {
	.append = page_append,
	.read = page_read,
	.end = page_end,
};

const ChunkLogRoutine *ChunkLog = &PageChunkLog;

/* ------------------------------------------------- the access method */

static void
refuse(const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("a chunk log relation does not support %s", what)));
}

static const TupleTableSlotOps *
cl_slot_callbacks(Relation relation)
{
	return &TTSOpsVirtual;
}

/* a scan of the log's relation finds no tuples (a SELECT, ANALYZE) */
static TableScanDesc
cl_scan_begin(Relation rel, Snapshot snapshot, int nkeys, ScanKeyData *key,
			  ParallelTableScanDesc pscan, uint32 flags)
{
	TableScanDesc scan = palloc0(sizeof(TableScanDescData));

	scan->rs_rd = rel;
	scan->rs_snapshot = snapshot;
	scan->rs_nkeys = nkeys;
	scan->rs_flags = flags;
	scan->rs_parallel = pscan;
	return scan;
}

static void
cl_scan_end(TableScanDesc scan)
{
	pfree(scan);
}

static void
cl_scan_rescan(TableScanDesc scan, ScanKeyData *key, bool set_params,
			   bool allow_strat, bool allow_sync, bool allow_pagemode)
{
}

static bool
cl_scan_getnextslot(TableScanDesc scan, ScanDirection direction,
					TupleTableSlot *slot)
{
	ExecClearTuple(slot);
	return false;
}

static void
cl_scan_set_tidrange(TableScanDesc scan, ItemPointer mintid,
					 ItemPointer maxtid)
{
	refuse("TID range scans");
}

static bool
cl_scan_getnextslot_tidrange(TableScanDesc scan, ScanDirection direction,
							 TupleTableSlot *slot)
{
	refuse("TID range scans");
	pg_unreachable();
}

static Size
cl_parallelscan_estimate(Relation rel)
{
	refuse("parallel scans");
	pg_unreachable();
}

static Size
cl_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan)
{
	refuse("parallel scans");
	pg_unreachable();
}

static void
cl_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan)
{
	refuse("parallel scans");
}

static void
cl_index_scan_begin(IndexScanDesc scan, uint32 flags)
{
	refuse("indexes");
}

static void
cl_index_scan_reset(IndexScanDesc scan)
{
	refuse("indexes");
}

static void
cl_index_scan_end(IndexScanDesc scan)
{
	refuse("indexes");
}

static bool
cl_fetch_tid(Relation rel, ItemPointer tid, Snapshot snapshot, bool *all_dead)
{
	refuse("indexes");
	pg_unreachable();
}

static bool
cl_tuple_fetch_row_version(Relation rel, ItemPointer tid, Snapshot snapshot,
						   TupleTableSlot *slot)
{
	refuse("fetching tuples");
	pg_unreachable();
}

static bool
cl_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
	refuse("fetching tuples");
	pg_unreachable();
}

static void
cl_tuple_get_latest_tid(TableScanDesc scan, ItemPointer tid)
{
	refuse("fetching tuples");
}

static bool
cl_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
							Snapshot snapshot)
{
	refuse("fetching tuples");
	pg_unreachable();
}

static TransactionId
cl_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
	refuse("indexes");
	pg_unreachable();
}

static void
cl_tuple_insert(Relation rel, TupleTableSlot *slot, CommandId cid,
				uint32 options, BulkInsertStateData *bistate)
{
	refuse("inserting tuples");
}

static void
cl_tuple_insert_speculative(Relation rel, TupleTableSlot *slot, CommandId cid,
							uint32 options, BulkInsertStateData *bistate,
							uint32 specToken)
{
	refuse("inserting tuples");
}

static void
cl_tuple_complete_speculative(Relation rel, TupleTableSlot *slot,
							  uint32 specToken, bool succeeded)
{
	refuse("inserting tuples");
}

static void
cl_multi_insert(Relation rel, TupleTableSlot **slots, int nslots,
				CommandId cid, uint32 options, BulkInsertStateData *bistate)
{
	refuse("inserting tuples");
}

static TM_Result
cl_tuple_delete(Relation rel, ItemPointer tid, CommandId cid, uint32 options,
				Snapshot snapshot, Snapshot crosscheck, bool wait,
				TM_FailureData *tmfd)
{
	refuse("deleting tuples");
	pg_unreachable();
}

static TM_Result
cl_tuple_update(Relation rel, ItemPointer otid, TupleTableSlot *slot,
				CommandId cid, uint32 options, Snapshot snapshot,
				Snapshot crosscheck, bool wait, TM_FailureData *tmfd,
				LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes)
{
	refuse("updating tuples");
	pg_unreachable();
}

static TM_Result
cl_tuple_lock(Relation rel, ItemPointer tid, Snapshot snapshot,
			  TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
			  LockWaitPolicy wait_policy, uint8 flags, TM_FailureData *tmfd)
{
	refuse("locking tuples");
	pg_unreachable();
}

static void
cl_finish_bulk_insert(Relation rel, uint32 options)
{
}

/*
 * The history store's relation is ordinary storage, WAL-logged: neither
 * unlogged nor temporary, and not a branch's (in a versioned schema).
 */
static void
cl_relation_set_new_filelocator(Relation rel,
								const RelFileLocator *newrlocator,
								char persistence,
								TransactionId *freezeXid,
								MultiXactId *minmulti)
{
	SMgrRelation srel;

	if (persistence != RELPERSISTENCE_PERMANENT)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a chunk log relation must be permanent")));
	if (NamespaceBranch(RelationGetNamespace(rel)) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a chunk log relation cannot be in a versioned schema")));

	*freezeXid = InvalidTransactionId;
	*minmulti = InvalidMultiXactId;
	srel = RelationCreateStorageWith(*newrlocator, persistence, true, SMGR_MD);
	smgrclose(srel);
}

static void
cl_relation_nontransactional_truncate(Relation rel)
{
	RelationTruncate(rel, 0);
}

/* (ALTER TABLE ... SET TABLESPACE: the pages as they are) */
static void
cl_relation_copy_data(Relation rel, const RelFileLocator *newrlocator)
{
	SMgrRelation dstrel;

	FlushRelationBuffers(rel);
	dstrel = RelationCreateStorageWith(*newrlocator,
									   rel->rd_rel->relpersistence, true,
									   RelationGetSmgr(rel)->smgr_which);
	RelationCopyStorage(RelationGetSmgr(rel), dstrel, MAIN_FORKNUM,
						rel->rd_rel->relpersistence);
	RelationDropStorage(rel);
	smgrclose(dstrel);
}

static void
cl_relation_copy_for_cluster(Relation OldTable, Relation NewTable,
							 Relation OldIndex, bool use_sort,
							 TransactionId OldestXmin, Snapshot snapshot,
							 TransactionId *xid_cutoff,
							 MultiXactId *multi_cutoff, double *num_tuples,
							 double *tups_vacuumed, double *tups_recently_dead)
{
	refuse("VACUUM FULL or CLUSTER");
}

/* nothing to vacuum: the log holds no tuples */
static void
cl_relation_vacuum(Relation rel, const VacuumParams *params,
				   BufferAccessStrategy bstrategy)
{
}

static bool
cl_scan_analyze_next_block(TableScanDesc scan, ReadStream *stream)
{
	return false;
}

static bool
cl_scan_analyze_next_tuple(TableScanDesc scan, double *liverows,
						   double *deadrows, TupleTableSlot *slot)
{
	return false;
}

static double
cl_index_build_range_scan(Relation table_rel, Relation index_rel,
						  IndexInfo *index_info, bool allow_sync,
						  bool anyvisible, bool progress,
						  BlockNumber start_blockno, BlockNumber numblocks,
						  IndexBuildCallback callback, void *callback_state,
						  TableScanDesc scan)
{
	refuse("indexes");
	pg_unreachable();
}

static void
cl_index_validate_scan(Relation table_rel, Relation index_rel,
					   IndexInfo *index_info, Snapshot snapshot,
					   ValidateIndexState *state)
{
	refuse("indexes");
}

static uint64
cl_relation_size(Relation rel, ForkNumber forkNumber)
{
	return table_block_relation_size(rel, forkNumber);
}

static bool
cl_relation_needs_toast_table(Relation rel)
{
	return false;
}

static void
cl_relation_estimate_size(Relation rel, int32 *attr_widths,
						  BlockNumber *pages, double *tuples,
						  double *allvisfrac)
{
	*pages = 0;
	*tuples = 0;
	*allvisfrac = 0;
}

static bool
cl_scan_bitmap_next_tuple(TableScanDesc scan, TupleTableSlot *slot,
						  bool *recheck, uint64 *lossy_pages,
						  uint64 *exact_pages)
{
	refuse("bitmap scans");
	pg_unreachable();
}

static bool
cl_scan_sample_next_block(TableScanDesc scan, SampleScanState *scanstate)
{
	refuse("TABLESAMPLE");
	pg_unreachable();
}

static bool
cl_scan_sample_next_tuple(TableScanDesc scan, SampleScanState *scanstate,
						  TupleTableSlot *slot)
{
	refuse("TABLESAMPLE");
	pg_unreachable();
}

static const TableAmRoutine chunk_log_routine = {
	.type = T_TableAmRoutine,
	.slot_callbacks = cl_slot_callbacks,
	.scan_begin = cl_scan_begin,
	.scan_end = cl_scan_end,
	.scan_rescan = cl_scan_rescan,
	.scan_getnextslot = cl_scan_getnextslot,
	.scan_set_tidrange = cl_scan_set_tidrange,
	.scan_getnextslot_tidrange = cl_scan_getnextslot_tidrange,
	.parallelscan_estimate = cl_parallelscan_estimate,
	.parallelscan_initialize = cl_parallelscan_initialize,
	.parallelscan_reinitialize = cl_parallelscan_reinitialize,
	.index_scan_begin = cl_index_scan_begin,
	.index_scan_reset = cl_index_scan_reset,
	.index_scan_end = cl_index_scan_end,
	.fetch_tid = cl_fetch_tid,
	.tuple_fetch_row_version = cl_tuple_fetch_row_version,
	.tuple_tid_valid = cl_tuple_tid_valid,
	.tuple_get_latest_tid = cl_tuple_get_latest_tid,
	.tuple_satisfies_snapshot = cl_tuple_satisfies_snapshot,
	.index_delete_tuples = cl_index_delete_tuples,
	.tuple_insert = cl_tuple_insert,
	.tuple_insert_speculative = cl_tuple_insert_speculative,
	.tuple_complete_speculative = cl_tuple_complete_speculative,
	.multi_insert = cl_multi_insert,
	.tuple_delete = cl_tuple_delete,
	.tuple_update = cl_tuple_update,
	.tuple_lock = cl_tuple_lock,
	.finish_bulk_insert = cl_finish_bulk_insert,
	.relation_set_new_filelocator = cl_relation_set_new_filelocator,
	.relation_nontransactional_truncate = cl_relation_nontransactional_truncate,
	.relation_copy_data = cl_relation_copy_data,
	.relation_copy_for_cluster = cl_relation_copy_for_cluster,
	.relation_vacuum = cl_relation_vacuum,
	.scan_analyze_next_block = cl_scan_analyze_next_block,
	.scan_analyze_next_tuple = cl_scan_analyze_next_tuple,
	.index_build_range_scan = cl_index_build_range_scan,
	.index_validate_scan = cl_index_validate_scan,
	.relation_size = cl_relation_size,
	.relation_needs_toast_table = cl_relation_needs_toast_table,
	.relation_estimate_size = cl_relation_estimate_size,
	.scan_bitmap_next_tuple = cl_scan_bitmap_next_tuple,
	.scan_sample_next_block = cl_scan_sample_next_block,
	.scan_sample_next_tuple = cl_scan_sample_next_tuple,
};

bool
RelationIsChunkLog(Relation rel)
{
	return rel->rd_tableam == &chunk_log_routine;
}

Datum
prolly_chunks_tableam_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&chunk_log_routine);
}
