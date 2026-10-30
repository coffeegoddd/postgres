/*-------------------------------------------------------------------------
 *
 * unit_bufmgr.c
 *	  Unit tests of the buffer manager's calls for storage that shares
 *	  pages, on md.c relations.
 *
 * A page of a relation whose storage manager doesn't prepare pages before
 * they change is never shared, and the buffer manager never asks its
 * storage manager about it; the lookups and drops of single cached blocks
 * that a storage manager of its own uses work on any relation.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/test_prolly/unit_bufmgr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "executor/spi.h"
#include "storage/bufmgr.h"
#include "storage/smgr.h"
#include "test_prolly.h"
#include "utils/rel.h"

static Relation
open_md_relation(void)
{
	return table_open(RelationRelationId, AccessShareLock);
}

/* an md.c page is never shared, whatever lock it is checked under */
static void
md_pages_are_not_shared(void)
{
	Relation	rel = open_md_relation();
	Buffer		buf = ReadBuffer(rel, 0);

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	CHECK(!BufferPageIsShared(buf));
	CHECK(!BufferPageIsSharedElsewhere(buf));
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);

	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	CHECK(!BufferPageIsShared(buf));
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);

	/* raising the relation's generation of sharing changes nothing for it */
	MarkRelationPagesShared(rel->rd_locator);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	CHECK(!BufferPageIsShared(buf));
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);

	ReleaseBuffer(buf);
	table_close(rel, AccessShareLock);
}

/* a cleanup lock taken without preparing, then prepared, as VACUUM would */
static void
cleanup_lock_unprepared(void)
{
	Relation	rel = open_md_relation();
	Buffer		buf = ReadBuffer(rel, 0);

	LockBufferForCleanupUnprepared(buf);
	CHECK(!BufferPageIsShared(buf));
	BufferPrepareForChange(buf);
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);

	CHECK(ConditionalLockBufferForCleanupUnprepared(buf));
	BufferPrepareForChange(buf);
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);

	CHECK(ConditionalLockBufferShared(buf));
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);

	ReleaseBuffer(buf);
	table_close(rel, AccessShareLock);
}

/* a cached block is found without reading anything; an uncached one isn't */
static void
lookup_cached_blocks(void)
{
	Relation	rel = open_md_relation();
	Buffer		buf = ReadBuffer(rel, 0);
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);

	CHECK_EQ(LookupSharedBuffer(rel->rd_locator, MAIN_FORKNUM, 0), buf);
	/* a block past the end can't be cached */
	CHECK_EQ(LookupSharedBuffer(rel->rd_locator, MAIN_FORKNUM, nblocks + 1000),
			 InvalidBuffer);
	CHECK_EQ(LookupSharedBuffer(rel->rd_locator, FSM_FORKNUM, nblocks + 1000),
			 InvalidBuffer);

	ReleaseBuffer(buf);
	table_close(rel, AccessShareLock);
}

/*
 * A scratch table of 'nblocks' blocks or more, all cached; dropped by
 * drop_scratch().
 */
static Relation
make_scratch(const char *name, int nblocks)
{
	char	   *sql = psprintf("CREATE TABLE %s (a int, pad text);"
							   "INSERT INTO %s SELECT g, repeat('x', 1000)"
							   " FROM generate_series(1, %d) g;"
							   "SELECT count(*) FROM %s",
							   name, name, nblocks * 7, name);

	CHECK_EQ(SPI_connect(), SPI_OK_CONNECT);
	CHECK(SPI_execute(sql, false, 0) >= 0);
	SPI_finish();
	return table_open(RelnameGetRelid(name), AccessExclusiveLock);
}

static void
drop_scratch(Relation rel)
{
	char	   *sql = psprintf("DROP TABLE %s", RelationGetRelationName(rel));

	table_close(rel, NoLock);
	CHECK_EQ(SPI_connect(), SPI_OK_CONNECT);
	CHECK_EQ(SPI_execute(sql, false, 0), SPI_OK_UTILITY);
	SPI_finish();
}

/* dirty a cached page of rel, as a change of its pd_prune_xid would */
static void
dirty_page(Relation rel, BlockNumber blk)
{
	Buffer		buf = ReadBuffer(rel, blk);

	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	MarkBufferDirty(buf);
	CHECK(BufferIsDirty(buf));
	UnlockReleaseBuffer(buf);
}

/* is block blk of rel's main fork cached dirty? */
static bool
cached_dirty(Relation rel, BlockNumber blk)
{
	Buffer		buf = ReadBuffer(rel, blk);
	bool		dirty;

	/* (BufferIsDirty() asks for the lock a change would take) */
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	dirty = BufferIsDirty(buf);
	UnlockReleaseBuffer(buf);
	return dirty;
}

/*
 * Writing out listed blocks cleans those that were dirty and leaves them
 * cached; a listed block not cached (past the end) is passed over.  (That a
 * block not listed stays dirty isn't checked: the background writer or a
 * checkpoint may write it out meanwhile.)
 */
static void
flush_listed_blocks(void)
{
	Relation	rel = make_scratch("unit_bufmgr_flush", 3);
	BlockNumber blocks[2] = {0, RelationGetNumberOfBlocks(rel) + 1000};

	dirty_page(rel, 0);
	dirty_page(rel, 1);
	FlushRelationBlocks(RelationGetSmgr(rel), MAIN_FORKNUM, blocks, 2);
	CHECK(BufferIsValid(LookupSharedBuffer(rel->rd_locator, MAIN_FORKNUM, 0)));
	CHECK(!cached_dirty(rel, 0));
	drop_scratch(rel);
}

/* dropping one cached block drops that block's buffer alone, dirty or not */
static void
drop_one_block(void)
{
	Relation	rel = make_scratch("unit_bufmgr_drop_block", 3);

	dirty_page(rel, 1);
	CHECK(BufferIsValid(LookupSharedBuffer(rel->rd_locator, MAIN_FORKNUM, 0)));
	CHECK(BufferIsValid(LookupSharedBuffer(rel->rd_locator, MAIN_FORKNUM, 1)));

	DropBufferBlock(rel->rd_locator, MAIN_FORKNUM, 1);
	CHECK_EQ(LookupSharedBuffer(rel->rd_locator, MAIN_FORKNUM, 1), InvalidBuffer);
	CHECK(BufferIsValid(LookupSharedBuffer(rel->rd_locator, MAIN_FORKNUM, 0)));

	/* a block not cached is nothing to drop */
	DropBufferBlock(rel->rd_locator, MAIN_FORKNUM, 1);
	drop_scratch(rel);
}

/*
 * dropping an unlinked relation's buffers drops them, dirty or not (checked
 * of the main fork, the only one a new table has)
 */
static void
drop_unlinked_relation_buffers(void)
{
	Relation	rel = make_scratch("unit_bufmgr_drop_unlinked", 3);
	SMgrRelation srel = RelationGetSmgr(rel);
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);

	dirty_page(rel, 0);
	DropUnlinkedRelationsAllBuffers(&srel, 1);
	for (BlockNumber blk = 0; blk < nblocks; blk++)
		CHECK_EQ(LookupSharedBuffer(rel->rd_locator, MAIN_FORKNUM, blk),
				 InvalidBuffer);
	drop_scratch(rel);
}

static const ProllyUnitTest tests[] = {
	{"md_pages_are_not_shared", md_pages_are_not_shared},
	{"cleanup_lock_unprepared", cleanup_lock_unprepared},
	{"lookup_cached_blocks", lookup_cached_blocks},
	{"flush_listed_blocks", flush_listed_blocks},
	{"drop_one_block", drop_one_block},
	{"drop_unlinked_relation_buffers", drop_unlinked_relation_buffers},
};

const ProllyUnitSuite unit_bufmgr_suite = {"bufmgr", tests, lengthof(tests)};
