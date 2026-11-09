/*--------------------------------------------------------------------------
 *
 * bmap_checks.c
 *		Functions for testing block-mapped storage: copies of a relation's
 *		storage made with smgrshare(), inspected and dropped below the
 *		catalogs.
 *
 * Every function names storage by relation file number, in the database's
 * default tablespace.  A copy made here has no catalog row; it lasts until
 * test_bmap_unlink() drops it.  test_bmap_write() changes a page as an
 * access method would, so a shared page is copied first.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_prolly/bmap_checks.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam_xlog.h"
#include "access/htup_details.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/catalog.h"
#include "catalog/pg_class.h"
#include "catalog/storage.h"
#include "common/cryptohash.h"
#include "common/md5.h"
#include "executor/instrument.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "portability/instr_time.h"
#include "storage/bmap.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"
#include "utils/tuplestore.h"

static RelFileLocator
locator_of(Oid relnumber)
{
	RelFileLocator loc;

	loc.spcOid = MyDatabaseTableSpace;
	loc.dbOid = MyDatabaseId;
	loc.relNumber = relnumber;
	return loc;
}

static SMgrRelation
bmap_smgr(Oid relnumber)
{
	SMgrRelation srel = smgropen(locator_of(relnumber), INVALID_PROC_NUMBER);

	if (srel->smgr_which != SMGR_BMAP)
		elog(ERROR, "relation file number %u is not block-mapped", relnumber);
	return srel;
}

/*
 * test_bmap_share(relnumber, transactional): a new relation file number
 * whose storage shares relnumber's pages, dropped if the transaction aborts
 * when transactional.
 */
PG_FUNCTION_INFO_V1(test_bmap_share);

Datum
test_bmap_share(PG_FUNCTION_ARGS)
{
	SMgrRelation src = bmap_smgr(PG_GETARG_OID(0));
	RelFileNumber copy = GetNewRelFileNumber(MyDatabaseTableSpace, NULL,
											 RELPERSISTENCE_PERMANENT);

	RelationCreateStorageShared(locator_of(copy), src, PG_GETARG_BOOL(1));
	PG_RETURN_OID(copy);
}

/*
 * An MD5 digest of every page of relnumber's storage, read through shared
 * buffers, with each page's checksum left out (it is set only when a page
 * is written); with 'heap', as heap pages with what changes no row (hint
 * bits, free space) left out too.
 */
static Datum
digest(Oid relnumber, bool heap)
{
	RelFileLocator loc = locator_of(relnumber);
	SMgrRelation srel = bmap_smgr(loc.relNumber);
	BlockNumber nblocks = smgrnblocks(srel, MAIN_FORKNUM);
	pg_cryptohash_ctx *ctx = pg_cryptohash_create(PG_MD5);
	uint8		sum[MD5_DIGEST_LENGTH];
	char		hex[2 * MD5_DIGEST_LENGTH + 1];
	PGAlignedBlock copy;

	if (pg_cryptohash_init(ctx) < 0)
		elog(ERROR, "could not initialize MD5");
	for (BlockNumber blk = 0; blk < nblocks; blk++)
	{
		Buffer		buf = ReadBufferWithoutRelcache(loc, MAIN_FORKNUM, blk,
													RBM_NORMAL, NULL, true);

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		memcpy(copy.data, BufferGetPage(buf), BLCKSZ);
		UnlockReleaseBuffer(buf);
		((PageHeader) copy.data)->pd_checksum = 0;
		if (heap)
			heap_mask(copy.data, blk);
		if (pg_cryptohash_update(ctx, (uint8 *) copy.data, BLCKSZ) < 0)
			elog(ERROR, "could not update MD5");
	}
	if (pg_cryptohash_final(ctx, sum, sizeof(sum)) < 0)
		elog(ERROR, "could not finish MD5");
	pg_cryptohash_free(ctx);
	hex_encode((char *) sum, sizeof(sum), hex);
	hex[sizeof(hex) - 1] = '\0';
	PG_RETURN_TEXT_P(cstring_to_text(hex));
}

/* test_bmap_digest(relnumber): a digest of the storage's pages */
PG_FUNCTION_INFO_V1(test_bmap_digest);

Datum
test_bmap_digest(PG_FUNCTION_ARGS)
{
	return digest(PG_GETARG_OID(0), false);
}

/*
 * test_bmap_rows_digest(relnumber): a digest of the storage's heap pages'
 * rows, leaving out hint bits, which may be set on a shared page in place.
 */
PG_FUNCTION_INFO_V1(test_bmap_rows_digest);

Datum
test_bmap_rows_digest(PG_FUNCTION_ARGS)
{
	return digest(PG_GETARG_OID(0), true);
}

/*
 * test_bmap_own_wal(): the bytes of WAL this backend has written, leaving
 * out full-page images.  The difference between two calls is the WAL of the
 * work between them, whatever other backends log meanwhile, and whether or
 * not a checkpoint made that work log a page's first image.
 */
PG_FUNCTION_INFO_V1(test_bmap_own_wal);

Datum
test_bmap_own_wal(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64((int64) (pgWalUsage.wal_bytes - pgWalUsage.wal_fpi_bytes));
}

/*
 * test_bmap_map(relnumber, fork): every block's physical block, birth epoch
 * and reference count, of the main fork (fork 0) or the visibility map (2).
 */
PG_FUNCTION_INFO_V1(test_bmap_map);

Datum
test_bmap_map(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	SMgrRelation srel = bmap_smgr(PG_GETARG_OID(0));
	ForkNumber	fork = (ForkNumber) PG_GETARG_INT32(1);
	BlockNumber nblocks;

	if (fork != MAIN_FORKNUM && fork != VISIBILITYMAP_FORKNUM)
		elog(ERROR, "fork %d is not block-mapped", fork);
	nblocks = smgrnblocks(srel, fork);
	InitMaterializedSRF(fcinfo, 0);
	for (BlockNumber blk = 0; blk < nblocks; blk++)
	{
		Datum		values[4];
		bool		nulls[4] = {0};
		BlockNumber phys;
		uint32		epoch;
		uint32		refs;

		bmap_inspect_block(srel, fork, blk, &phys, &epoch, &refs);
		values[0] = Int64GetDatum(blk);
		values[1] = Int64GetDatum(phys);
		values[2] = Int64GetDatum(epoch);
		values[3] = Int32GetDatum(refs);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

/*
 * test_bmap_past_end(relnumber): the entries a map has past its relation's
 * length (a truncation's, until let go of), as test_bmap_map() does.
 */
PG_FUNCTION_INFO_V1(test_bmap_past_end);

Datum
test_bmap_past_end(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	SMgrRelation srel = bmap_smgr(PG_GETARG_OID(0));
	uint64		end = bmap_inspect_map_blocks(srel);

	InitMaterializedSRF(fcinfo, 0);
	for (uint64 blk = smgrnblocks(srel, MAIN_FORKNUM); blk < end; blk++)
	{
		Datum		values[4];
		bool		nulls[4] = {0};
		BlockNumber phys;
		uint32		epoch;
		uint32		refs;

		bmap_inspect_block(srel, MAIN_FORKNUM, (BlockNumber) blk, &phys, &epoch,
						   &refs);
		if (!BlockNumberIsValid(phys))
			continue;
		values[0] = Int64GetDatum(blk);
		values[1] = Int64GetDatum(phys);
		values[2] = Int64GetDatum(epoch);
		values[3] = Int32GetDatum(refs);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

/* test_bmap_epoch(relnumber): the map's epoch */
PG_FUNCTION_INFO_V1(test_bmap_epoch);

Datum
test_bmap_epoch(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(bmap_inspect_epoch(bmap_smgr(PG_GETARG_OID(0))));
}

/*
 * test_bmap_exists(relnumber): whether block-mapped storage of that file
 * number (in the database's tablespace) exists
 */
PG_FUNCTION_INFO_V1(test_bmap_exists);

Datum
test_bmap_exists(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(bmap_relation_exists(locator_of(PG_GETARG_OID(0))));
}

/* test_bmap_free_blocks(): the page store's count of free blocks */
PG_FUNCTION_INFO_V1(test_bmap_free_blocks);

Datum
test_bmap_free_blocks(PG_FUNCTION_ARGS)
{
	BlockNumber hw;
	uint32		nfree;

	bmap_inspect_allocator(locator_of(InvalidRelFileNumber), &hw, &nfree);
	PG_RETURN_INT64(nfree);
}

/*
 * test_bmap_store_sync(): the slot following the page store's flushes for
 * shares and captures, as bmap_inspect_store_sync() reads it (written and
 * flushing NULL if it has none)
 */
PG_FUNCTION_INFO_V1(test_bmap_store_sync);

Datum
test_bmap_store_sync(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[2];
	bool		nulls[2] = {false, false};
	uint64		written;
	uint32		flushing;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	if (bmap_inspect_store_sync(locator_of(InvalidRelFileNumber), &written,
								&flushing))
	{
		values[0] = Int64GetDatum((int64) written);
		values[1] = Int32GetDatum((int32) flushing);
	}
	else
		nulls[0] = nulls[1] = true;
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/* test_bmap_sweep_due(): is a sweep of the page store due? */
PG_FUNCTION_INFO_V1(test_bmap_sweep_due);

Datum
test_bmap_sweep_due(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(bmap_sweep_due(MyDatabaseTableSpace, MyDatabaseId));
}

/*
 * test_bmap_drop_orphans(): the storage of transactions that never
 * committed, dropped at this transaction's commit; how many.
 */
PG_FUNCTION_INFO_V1(test_bmap_drop_orphans);

Datum
test_bmap_drop_orphans(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(bmap_drop_orphans(MyDatabaseTableSpace, MyDatabaseId));
}

/*
 * test_bmap_unlink(relnumber, drain): drop storage, its blocks queued for
 * release; with drain, the queue drained after, freeing what no copy holds.
 * The records are flushed before returning: this transaction has no
 * commit record that would flush them.
 */
PG_FUNCTION_INFO_V1(test_bmap_unlink);

Datum
test_bmap_unlink(PG_FUNCTION_ARGS)
{
	SMgrRelation srel = bmap_smgr(PG_GETARG_OID(0));

	smgrdounlinkall(&srel, 1, false);
	if (PG_GETARG_BOOL(1))
	{
		BmapRelease done = {0};

		bmap_release_drain(MyDatabaseTableSpace, MyDatabaseId, 0, &done);
	}
	XLogFlush(XactLastRecEnd);
	PG_RETURN_VOID();
}

/*
 * test_bmap_release_drain(max_entries): the release queue of the database's
 * page store drained, of at most max_entries (0: all of it); the entries
 * popped, the blocks freed and the counts set.  Flushed, as above.
 */
PG_FUNCTION_INFO_V1(test_bmap_release_drain);

Datum
test_bmap_release_drain(PG_FUNCTION_ARGS)
{
	BmapRelease done = {0};
	TupleDesc	desc;
	Datum		values[3];
	bool		nulls[3] = {0};

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	bmap_release_drain(MyDatabaseTableSpace, MyDatabaseId,
					   (uint64) Max(PG_GETARG_INT64(0), 0), &done);
	XLogFlush(XactLastRecEnd);
	values[0] = Int64GetDatum((int64) done.entries);
	values[1] = Int64GetDatum((int64) done.freed);
	values[2] = Int64GetDatum((int64) done.unrefs);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc),
													  values, nulls)));
}

/*
 * test_bmap_sweep(): the database's page store in its default tablespace
 * swept, as a maintenance command sweeps it: the blocks nothing held freed,
 * the counts of those zeroed, the blocks the store was shortened by, and
 * those moved down for it.  Flushed, as above.
 */
PG_FUNCTION_INFO_V1(test_bmap_sweep);

Datum
test_bmap_sweep(PG_FUNCTION_ARGS)
{
	BmapSweep	r;
	TupleDesc	desc;
	Datum		values[4];
	bool		nulls[4] = {0};

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	bmap_sweep(MyDatabaseTableSpace, MyDatabaseId, &r);
	XLogFlush(XactLastRecEnd);
	values[0] = Int64GetDatum((int64) r.freed);
	values[1] = Int64GetDatum((int64) r.recounted);
	values[2] = Int64GetDatum((int64) r.truncated);
	values[3] = Int64GetDatum((int64) r.moved);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc),
													  values, nulls)));
}

/* test_bmap_release_pending(): the entries the release queue holds */
PG_FUNCTION_INFO_V1(test_bmap_release_pending);

Datum
test_bmap_release_pending(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64((int64) bmap_release_pending(MyDatabaseTableSpace,
												 MyDatabaseId));
}

/*
 * An injection point callback, attached with injection_points_attach(name,
 * 'test_prolly', 'test_bmap_write_one_crash', '\xNN'): of the locked
 * buffers the point passes, write out the one numbered NN alone, then
 * crash, as a crash with only that page written leaves the storage.  A
 * point passing no buffers just crashes; the truncation point that passes
 * any passes one, the map's header, so NN must be 0.
 */
extern PGDLLEXPORT void test_bmap_write_one_crash(const char *name,
												  const void *private_data,
												  void *arg);

void
test_bmap_write_one_crash(const char *name, const void *private_data,
						  void *arg)
{
	uint8		i = *(const uint8 *) private_data;
	Buffer		buf;

	if (arg == NULL)
		elog(PANIC, "crashing at injection point \"%s\"", name);
	if (i != 0)
		elog(ERROR, "injection point \"%s\" passes one buffer, not %u",
			 name, (unsigned) i + 1);
	buf = ((Buffer *) arg)[i];

	if (BufferIsValid(buf))
		FlushOneBuffer(buf);
	elog(PANIC, "crashing at injection point \"%s\" with buffer %u written",
		 name, (unsigned) i);
}

/*
 * test_bmap_truncate(relnumber, nblocks, done): shorten the logical length;
 * if done, finish as RelationTruncate() does, letting go of the blocks
 * past it (otherwise as a crash before that leaves them).
 */
PG_FUNCTION_INFO_V1(test_bmap_truncate);

Datum
test_bmap_truncate(PG_FUNCTION_ARGS)
{
	SMgrRelation srel = bmap_smgr(PG_GETARG_OID(0));
	ForkNumber	fork = MAIN_FORKNUM;
	BlockNumber old = smgrnblocks(srel, MAIN_FORKNUM);
	BlockNumber nblocks = PG_GETARG_INT32(1);

	smgrpretruncate(srel, &fork, 1);
	smgrtruncate(srel, &fork, 1, &old, &nblocks);
	if (PG_GETARG_BOOL(2))
		smgrposttruncate(srel, &fork, 1);
	PG_RETURN_VOID();
}

/* test_bmap_extend(relnumber, n): add n zeroed blocks */
PG_FUNCTION_INFO_V1(test_bmap_extend);

Datum
test_bmap_extend(PG_FUNCTION_ARGS)
{
	SMgrRelation srel = bmap_smgr(PG_GETARG_OID(0));

	smgrzeroextend(srel, MAIN_FORKNUM, smgrnblocks(srel, MAIN_FORKNUM),
				   PG_GETARG_INT32(1), false);
	PG_RETURN_VOID();
}

/*
 * test_bmap_write(relnumber, blkno, value): change a page as an access
 * method would (an exclusive lock, a change, a WAL record): a byte of its
 * hole, between pd_lower and pd_upper, which no reader looks at.
 */
PG_FUNCTION_INFO_V1(test_bmap_write);

Datum
test_bmap_write(PG_FUNCTION_ARGS)
{
	RelFileLocator loc = locator_of(PG_GETARG_OID(0));
	BlockNumber blkno = (BlockNumber) PG_GETARG_INT64(1);
	int32		value = PG_GETARG_INT32(2);
	Buffer		buf;
	Page		page;
	PageHeader	ph;

	(void) bmap_smgr(loc.relNumber);
	buf = ReadBufferWithoutRelcache(loc, MAIN_FORKNUM, blkno, RBM_NORMAL,
									NULL, true);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buf);
	ph = (PageHeader) page;
	if (ph->pd_upper <= ph->pd_lower)
		elog(ERROR, "block %u has no hole", blkno);

	START_CRIT_SECTION();
	((char *) page)[ph->pd_lower] = (char) value;
	MarkBufferDirty(buf);
	log_newpage_buffer(buf, false);
	END_CRIT_SECTION();
	UnlockReleaseBuffer(buf);
	PG_RETURN_VOID();
}

/*
 * test_bmap_extend_at(relnumber, blkno): write an initialized, empty page
 * at blkno, past the relation's end, as a hash index's bucket allocation
 * writes the last block of a range: told to the storage manager
 * (smgrpreextend()), logged, then written with smgrextend(), leaving the
 * blocks between the old end and it unwritten.
 */
PG_FUNCTION_INFO_V1(test_bmap_extend_at);

Datum
test_bmap_extend_at(PG_FUNCTION_ARGS)
{
	SMgrRelation srel = bmap_smgr(PG_GETARG_OID(0));
	BlockNumber blkno = (BlockNumber) PG_GETARG_INT64(1);
	PGIOAlignedBlock page;

	PageInit(page.data, BLCKSZ, 0);
	smgrpreextend(srel, MAIN_FORKNUM, blkno, 1);
	log_newpage(&srel->smgr_rlocator.locator, MAIN_FORKNUM, blkno, page.data,
				true);
	PageSetChecksum(page.data, blkno);
	smgrextend(srel, MAIN_FORKNUM, blkno, page.data, false);
	PG_RETURN_VOID();
}

/*
 * test_bmap_init_block(relnumber, blkno): initialize a block below the
 * relation's end as an access method makes a new page there (a hash
 * index's new bucket): read zeroed and locked, initialized, logged.
 */
PG_FUNCTION_INFO_V1(test_bmap_init_block);

Datum
test_bmap_init_block(PG_FUNCTION_ARGS)
{
	RelFileLocator loc = locator_of(PG_GETARG_OID(0));
	Buffer		buf;

	(void) bmap_smgr(loc.relNumber);
	buf = ReadBufferWithoutRelcache(loc, MAIN_FORKNUM,
									(BlockNumber) PG_GETARG_INT64(1),
									RBM_ZERO_AND_LOCK, NULL, true);
	START_CRIT_SECTION();
	PageInit(BufferGetPage(buf), BLCKSZ, 0);
	MarkBufferDirty(buf);
	log_newpage_buffer(buf, true);
	END_CRIT_SECTION();
	UnlockReleaseBuffer(buf);
	PG_RETURN_VOID();
}

/*
 * test_bmap_read(relnumber, blkno, buffered): read a page, through shared
 * buffers or with smgrread(): 'zeroes' if it is all zeroes, else the byte
 * test_bmap_write() changes (the first of its hole).
 */
PG_FUNCTION_INFO_V1(test_bmap_read);

Datum
test_bmap_read(PG_FUNCTION_ARGS)
{
	RelFileLocator loc = locator_of(PG_GETARG_OID(0));
	SMgrRelation srel = bmap_smgr(loc.relNumber);
	BlockNumber blkno = (BlockNumber) PG_GETARG_INT64(1);
	PGIOAlignedBlock page;
	PageHeader	ph = (PageHeader) page.data;

	if (PG_GETARG_BOOL(2))
	{
		Buffer		buf = ReadBufferWithoutRelcache(loc, MAIN_FORKNUM, blkno,
													RBM_NORMAL, NULL, true);

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		memcpy(page.data, BufferGetPage(buf), BLCKSZ);
		UnlockReleaseBuffer(buf);
	}
	else
		smgrread(srel, MAIN_FORKNUM, blkno, page.data);
	if (pg_memory_is_all_zeros(page.data, BLCKSZ))
		PG_RETURN_TEXT_P(cstring_to_text("zeroes"));
	if (ph->pd_lower >= BLCKSZ)
		elog(ERROR, "block %u has no hole", blkno);
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("%d",
											  (int) (uint8) page.data[ph->pd_lower])));
}

static void
mapped_row(BlockNumber blkno, BlockNumber phys, void *arg)
{
	ReturnSetInfo *rsinfo = arg;
	Datum		values[2];
	bool		nulls[2] = {0};

	values[0] = Int64GetDatum(blkno);
	values[1] = Int64GetDatum(phys);
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

/*
 * test_bmap_mapped(relnumber): the logical blocks that have a physical
 * block, and theirs, found walking the map's nodes, not every block below
 * the length, so a map with holes over billions of blocks costs only them.
 */
PG_FUNCTION_INFO_V1(test_bmap_mapped);

Datum
test_bmap_mapped(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);
	bmap_inspect_entries(bmap_smgr(PG_GETARG_OID(0)), mapped_row, rsinfo);
	return (Datum) 0;
}

/*
 * Read a block zeroed and locked, as an access method about to initialize
 * it does, and let it go uninitialized.  Zeroed only if not already in a
 * buffer.
 */
PG_FUNCTION_INFO_V1(test_bmap_zero_block);
Datum
test_bmap_zero_block(PG_FUNCTION_ARGS)
{
	RelFileLocator loc = locator_of(PG_GETARG_OID(0));
	Buffer		buf;

	(void) bmap_smgr(loc.relNumber);
	buf = ReadBufferWithoutRelcache(loc, MAIN_FORKNUM, PG_GETARG_INT32(1),
									RBM_ZERO_AND_LOCK, NULL, true);
	UnlockReleaseBuffer(buf);
	PG_RETURN_VOID();
}

/* Try for a block's cleanup lock without waiting, and let it go. */
PG_FUNCTION_INFO_V1(test_bmap_cleanup_lock);
Datum
test_bmap_cleanup_lock(PG_FUNCTION_ARGS)
{
	RelFileLocator loc = locator_of(PG_GETARG_OID(0));
	Buffer		buf;
	bool		got;

	(void) bmap_smgr(loc.relNumber);
	buf = ReadBufferWithoutRelcache(loc, MAIN_FORKNUM, PG_GETARG_INT32(1),
									RBM_NORMAL, NULL, true);
	got = ConditionalLockBufferForCleanup(buf);
	if (got)
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buf);
	PG_RETURN_BOOL(got);
}

/*
 * Drop a block's page from the buffer pool without writing it, as the block
 * map does with a block given other contents (DropBufferBlock()).
 */
PG_FUNCTION_INFO_V1(test_bmap_drop_buffer);
Datum
test_bmap_drop_buffer(PG_FUNCTION_ARGS)
{
	DropBufferBlock(locator_of(PG_GETARG_OID(0)), MAIN_FORKNUM,
					PG_GETARG_INT32(1));
	PG_RETURN_VOID();
}

/*
 * Have each of a relation's cached pages checked again before it next
 * changes, as when its storage manager begins sharing them
 * (MarkRelationPagesShared()), though none is shared.
 */
PG_FUNCTION_INFO_V1(test_bmap_mark_shared);
Datum
test_bmap_mark_shared(PG_FUNCTION_ARGS)
{
	MarkRelationPagesShared(locator_of(PG_GETARG_OID(0)));
	PG_RETURN_VOID();
}

static void
tree_row(uint16 level, BlockNumber first, BlockNumber block, uint32 refs,
		 void *arg)
{
	ReturnSetInfo *rsinfo = arg;
	Datum		values[4];
	bool		nulls[4] = {0};

	values[0] = Int32GetDatum(level);
	values[1] = Int64GetDatum(first);
	values[2] = Int64GetDatum(block);
	values[3] = Int32GetDatum(refs);
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

/* test_bmap_tree(relnumber): a tree map's nodes, as reached */
PG_FUNCTION_INFO_V1(test_bmap_tree);

Datum
test_bmap_tree(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);
	bmap_inspect_tree(bmap_smgr(PG_GETARG_OID(0)), tree_row, rsinfo);
	return (Datum) 0;
}

/* test_bmap_root_capture(relnumber): a root captured; its id */
PG_FUNCTION_INFO_V1(test_bmap_root_capture);

Datum
test_bmap_root_capture(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64((int64) bmap_root_capture(bmap_smgr(PG_GETARG_OID(0))));
}

/*
 * test_bmap_root_capture_of(copy, of): a frozen root captured of 'copy',
 * recorded as of 'of' at its epoch now (bmap_root_capture_of())
 */
PG_FUNCTION_INFO_V1(test_bmap_root_capture_of);

Datum
test_bmap_root_capture_of(PG_FUNCTION_ARGS)
{
	SMgrRelation of = bmap_smgr(PG_GETARG_OID(1));

	PG_RETURN_INT64((int64) bmap_root_capture_of(bmap_smgr(PG_GETARG_OID(0)),
												 of, (uint32) bmap_inspect_epoch(of)));
}

/* test_bmap_root_release(id): let go of it as the transaction commits */
PG_FUNCTION_INFO_V1(test_bmap_root_release);

Datum
test_bmap_root_release(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(bmap_root_release(locator_of(InvalidRelFileNumber),
									 (BmapRootId) PG_GETARG_INT64(0)));
}

/* test_bmap_root_held(id): is it held?  Its length, if so (else NULL) */
PG_FUNCTION_INFO_V1(test_bmap_root_held);

Datum
test_bmap_root_held(PG_FUNCTION_ARGS)
{
	BlockNumber nblocks;

	if (!bmap_root_held(locator_of(InvalidRelFileNumber),
						(BmapRootId) PG_GETARG_INT64(0), &nblocks))
		PG_RETURN_NULL();
	PG_RETURN_INT64(nblocks);
}

/*
 * test_bmap_share_root(id): a new relation file number whose storage is the
 * root's, which test_bmap_unlink() drops.
 */
PG_FUNCTION_INFO_V1(test_bmap_share_root);

Datum
test_bmap_share_root(PG_FUNCTION_ARGS)
{
	RelFileNumber copy = GetNewRelFileNumber(MyDatabaseTableSpace, NULL,
											 RELPERSISTENCE_PERMANENT);
	SMgrRelation dst = smgropen(locator_of(copy), INVALID_PROC_NUMBER);

	bmap_share_root((BmapRootId) PG_GETARG_INT64(0), dst, NULL, NULL, NULL);
	PG_RETURN_OID(copy);
}

/* test_bmap_roots(): the roots the database's store holds, and its slots */
PG_FUNCTION_INFO_V1(test_bmap_roots);

Datum
test_bmap_roots(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[2];
	bool		nulls[2] = {0};
	uint64		held;
	uint64		slots;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	bmap_inspect_roots(locator_of(InvalidRelFileNumber), &held, &slots, NULL);
	values[0] = Int64GetDatum((int64) held);
	values[1] = Int64GetDatum((int64) slots);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/* test_bmap_roots_of(relnumber): the roots held captured of it */
PG_FUNCTION_INFO_V1(test_bmap_roots_of);

Datum
test_bmap_roots_of(PG_FUNCTION_ARGS)
{
	uint64		held;
	uint64		slots;

	bmap_inspect_roots(locator_of(PG_GETARG_OID(0)), &held, &slots, NULL);
	PG_RETURN_INT64((int64) held);
}

/* test_bmap_roots_naming(): the slots in use that name a transaction */
PG_FUNCTION_INFO_V1(test_bmap_roots_naming);

Datum
test_bmap_roots_naming(PG_FUNCTION_ARGS)
{
	uint64		held;
	uint64		slots;
	uint64		naming;

	bmap_inspect_roots(locator_of(InvalidRelFileNumber), &held, &slots, &naming);
	PG_RETURN_INT64((int64) naming);
}

/*
 * test_bmap_refs_file_asks(): how many times this backend asked the
 * reference-count files' lengths of the file system.
 */
PG_FUNCTION_INFO_V1(test_bmap_refs_file_asks);

Datum
test_bmap_refs_file_asks(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64((int64) bmap_refs_file_asks());
}

/*
 * test_bmap_standby_hold(): the standby hold published (NULL: none), and
 * whether a standby runs an online backup.
 */
PG_FUNCTION_INFO_V1(test_bmap_standby_hold);

Datum
test_bmap_standby_hold(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[2];
	bool		nulls[2] = {0};
	bool		backups;
	XLogRecPtr	hold = bmap_standby_hold(&backups);

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	values[0] = LSNGetDatum(hold);
	nulls[0] = !XLogRecPtrIsValid(hold);
	values[1] = BoolGetDatum(backups);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * test_bmap_root_counts(): whether the roots' counts of the database's page
 * store are whole, the roots they count, and the blocks whose counts differ
 * from the roots counted anew
 */
PG_FUNCTION_INFO_V1(test_bmap_root_counts);

Datum
test_bmap_root_counts(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[3];
	bool		nulls[3] = {0};
	uint64		counted;
	uint64		differ;
	bool		whole;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	whole = bmap_inspect_root_counts(locator_of(InvalidRelFileNumber),
									 &counted, &differ);
	values[0] = BoolGetDatum(whole);
	values[1] = Int64GetDatum((int64) counted);
	values[2] = Int64GetDatum((int64) differ);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/* test_bmap_roots_reclaim(settle): the abandoned roots reclaimed */
PG_FUNCTION_INFO_V1(test_bmap_roots_reclaim);

Datum
test_bmap_roots_reclaim(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32((int32) bmap_roots_reclaim(locator_of(InvalidRelFileNumber),
												 PG_GETARG_BOOL(0)));
}

/*
 * test_bmap_lookups(relnumber, loops): look every logical block up 'loops'
 * times, as a read's combining does; the microseconds it took.
 */
PG_FUNCTION_INFO_V1(test_bmap_lookups);

Datum
test_bmap_lookups(PG_FUNCTION_ARGS)
{
	SMgrRelation srel = bmap_smgr(PG_GETARG_OID(0));
	int32		loops = PG_GETARG_INT32(1);
	BlockNumber nblocks = smgrnblocks(srel, MAIN_FORKNUM);
	instr_time	start;
	instr_time	elapsed;
	uint64		sum = 0;

	INSTR_TIME_SET_CURRENT(start);
	for (int32 i = 0; i < loops; i++)
		for (BlockNumber blk = 0; blk < nblocks; blk++)
			sum += smgrmaxcombine(srel, MAIN_FORKNUM, blk);
	INSTR_TIME_SET_CURRENT(elapsed);
	INSTR_TIME_SUBTRACT(elapsed, start);
	if (sum == 0 && nblocks > 0)
		elog(ERROR, "no block found");
	PG_RETURN_FLOAT8(INSTR_TIME_GET_MICROSEC(elapsed));
}

/*
 * test_synced_pages_note(lsn): say in pg_control that pages were synced
 * instead of logged, relying on no recovery beginning at or before 'lsn',
 * as a share or its replay does once the pages are durable.  For tests of
 * what then refuses to begin there.
 */
PG_FUNCTION_INFO_V1(test_synced_pages_note);

Datum
test_synced_pages_note(PG_FUNCTION_ARGS)
{
	XLogNoteSyncedPages(PG_GETARG_LSN(0));
	PG_RETURN_VOID();
}
