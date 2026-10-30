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
#include "catalog/pg_class.h"
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

/* writing out listed blocks leaves them cached and clean */
static void
flush_listed_blocks(void)
{
	Relation	rel = open_md_relation();
	BlockNumber blocks[2] = {0, RelationGetNumberOfBlocks(rel) + 1000};
	Buffer		buf;

	FlushRelationBlocks(RelationGetSmgr(rel), MAIN_FORKNUM, blocks, 2);
	buf = LookupSharedBuffer(rel->rd_locator, MAIN_FORKNUM, 0);
	CHECK(BufferIsValid(buf));
	table_close(rel, AccessShareLock);
}

static const ProllyUnitTest tests[] = {
	{"md_pages_are_not_shared", md_pages_are_not_shared},
	{"cleanup_lock_unprepared", cleanup_lock_unprepared},
	{"lookup_cached_blocks", lookup_cached_blocks},
	{"flush_listed_blocks", flush_listed_blocks},
};

const ProllyUnitSuite unit_bufmgr_suite = {"bufmgr", tests, lengthof(tests)};
