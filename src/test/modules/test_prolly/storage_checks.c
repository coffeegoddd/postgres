/*--------------------------------------------------------------------------
 *
 * storage_checks.c
 *		Conformance checks for the node store interface.
 *
 * Nodes written here are raw pages with a marker, logged with
 * log_newpage_buffer, so the checks don't depend on any node format or log
 * implementation.  Hashes are random, so repeated runs against the same
 * cluster don't collide with earlier runs' nodes.
 *
 * A node store may be cluster-wide, so what these checks write stays in it
 * (until reclamation exists, roadmap step 4f).
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_prolly/storage_checks.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xloginsert.h"
#include "catalog/pg_class.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "port/pg_bswap.h"
#include "storage/bufmgr.h"
#include "test_prolly.h"
#include "utils/builtins.h"

#define TEST_HASH_LEN 32

static void
random_hash(ProllyHash *hash)
{
	memset(hash, 0, sizeof(*hash));
	hash->len = TEST_HASH_LEN;
	if (!pg_strong_random(hash->bytes, TEST_HASH_LEN))
		elog(ERROR, "could not generate random bytes");
}

/* Write a node holding marker; return its (unpublished) reference. */
static Buffer
write_node(const ProllyNodeStoreRoutine *ns, ProllyNodeStore *store,
		   uint64 marker, ProllyNodeRef *ref)
{
	Buffer		buf = PROLLY_CALL(ns, new_node, store, ref);
	Page		page = BufferGetPage(buf);

	if (ref->location.blkno != BufferGetBlockNumber(buf))
		elog(ERROR, "new_node's reference names block %u, the buffer is block %u",
			 ref->location.blkno, BufferGetBlockNumber(buf));

	START_CRIT_SECTION();
	PageInit(page, BLCKSZ, 0);
	memcpy(PageGetContents(page), &marker, sizeof(marker));
	((PageHeader) page)->pd_lower += MAXALIGN(sizeof(marker));
	MarkBufferDirty(buf);
	log_newpage_buffer(buf, true);
	END_CRIT_SECTION();
	return buf;
}

static void
check_marker(const ProllyNodeStoreRoutine *ns, ProllyNodeStore *store,
			 const ProllyNodeRef *ref, uint64 want, const char *how)
{
	Buffer		buf = PROLLY_CALL(ns, read, store, ref);
	uint64		got;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	memcpy(&got, PageGetContents(BufferGetPage(buf)), sizeof(got));
	UnlockReleaseBuffer(buf);
	if (got != want)
		elog(ERROR, "reading %s gave the node with marker " UINT64_FORMAT
			 ", expected " UINT64_FORMAT, how, got, want);
}

static ProllyNodeStore *
open_store(const ProllyNodeStoreRoutine *ns)
{
	PROLLY_CALL(ns, create, NULL, NULL, RELPERSISTENCE_PERMANENT);
	return PROLLY_CALL(ns, open, NULL);
}

void
nodestore_new_publish_read(const ProllyImplInfo *impl)
{
	const ProllyNodeStoreRoutine *ns = (const ProllyNodeStoreRoutine *) impl;
	ProllyNodeStore *store = open_store(ns);
	ProllyNodeRef ref;
	ProllyNodeRef by_hash;
	ProllyHash	hash;
	ProllyHash	other;
	Buffer		buf;

	random_hash(&hash);
	random_hash(&other);
	buf = write_node(ns, store, 42, &ref);
	if (PROLLY_CALL(ns, contains, store, &hash))
		elog(ERROR, "store contains a hash before it was published");
	PROLLY_CALL(ns, publish, store, buf, &hash, &ref);
	UnlockReleaseBuffer(buf);

	if (ref.hash.len != hash.len || memcmp(ref.hash.bytes, hash.bytes, hash.len) != 0)
		elog(ERROR, "publish did not set the reference's hash");
	if (!PROLLY_CALL(ns, contains, store, &hash))
		elog(ERROR, "store does not contain a published hash");
	if (PROLLY_CALL(ns, contains, store, &other))
		elog(ERROR, "store contains a hash that was never published");

	check_marker(ns, store, &ref, 42, "by hash and location");
	memset(&by_hash, 0, sizeof(by_hash));
	by_hash.hash = hash;
	by_hash.location.blkno = InvalidBlockNumber;
	check_marker(ns, store, &by_hash, 42, "by hash only");

	PROLLY_CALL(ns, close, store);
}

/* Enough nodes to split a hash map many times; all must stay findable. */
void
nodestore_many_nodes(const ProllyImplInfo *impl)
{
	const ProllyNodeStoreRoutine *ns = (const ProllyNodeStoreRoutine *) impl;
	ProllyNodeStore *store = open_store(ns);
	const int	n = 5000;
	ProllyHash *hashes = palloc(sizeof(ProllyHash) * n);
	BlockNumber *blocks = palloc(sizeof(BlockNumber) * n);

	for (int i = 0; i < n; i++)
	{
		ProllyNodeRef ref;
		Buffer		buf = write_node(ns, store, (uint64) i, &ref);

		random_hash(&hashes[i]);
		PROLLY_CALL(ns, publish, store, buf, &hashes[i], &ref);
		blocks[i] = BufferGetBlockNumber(buf);
		UnlockReleaseBuffer(buf);
		CHECK_FOR_INTERRUPTS();
	}

	for (int i = 0; i < n; i++)
	{
		ProllyNodeRef ref;

		if (!PROLLY_CALL(ns, contains, store, &hashes[i]))
			elog(ERROR, "node %d of %d is no longer found", i, n);
		memset(&ref, 0, sizeof(ref));
		ref.hash = hashes[i];
		ref.location.blkno = InvalidBlockNumber;
		check_marker(ns, store, &ref, (uint64) i, "by hash only");
	}
	PROLLY_CALL(ns, close, store);
}

void
nodestore_release_frees(const ProllyImplInfo *impl)
{
	const ProllyNodeStoreRoutine *ns = (const ProllyNodeStoreRoutine *) impl;
	ProllyNodeStore *store = open_store(ns);
	ProllyNodeRef kept;
	ProllyNodeRef released;
	ProllyHash	h1;
	ProllyHash	h2;
	Buffer		buf;

	random_hash(&h1);
	random_hash(&h2);
	buf = write_node(ns, store, 1, &kept);
	PROLLY_CALL(ns, publish, store, buf, &h1, &kept);
	UnlockReleaseBuffer(buf);
	buf = write_node(ns, store, 2, &released);
	PROLLY_CALL(ns, publish, store, buf, &h2, &released);
	UnlockReleaseBuffer(buf);

	PROLLY_CALL(ns, release, store, &released);
	if (PROLLY_CALL(ns, contains, store, &h2))
		elog(ERROR, "a released node is still found");
	if (!PROLLY_CALL(ns, contains, store, &h1))
		elog(ERROR, "releasing one node lost another");
	check_marker(ns, store, &kept, 1, "a node after another was released");
	PROLLY_CALL(ns, close, store);
}

/* ------------------------------------------------------------------------
 * SQL-callable: write nodes now, verify them later
 * ------------------------------------------------------------------------
 */

PG_FUNCTION_INFO_V1(prolly_store_write);
PG_FUNCTION_INFO_V1(prolly_store_verify);

static uint64
splitmix64(uint64 *state)
{
	uint64		z = (*state += UINT64CONST(0x9e3779b97f4a7c15));

	z = (z ^ (z >> 30)) * UINT64CONST(0xbf58476d1ce4e5b9);
	z = (z ^ (z >> 27)) * UINT64CONST(0x94d049bb133111eb);
	return z ^ (z >> 31);
}

static void
seeded_hash(int64 seed, int i, ProllyHash *hash)
{
	uint64		state = (uint64) seed * 1000003 + (uint64) i;

	memset(hash, 0, sizeof(*hash));
	hash->len = TEST_HASH_LEN;
	for (int w = 0; w < TEST_HASH_LEN / 8; w++)
	{
		uint64		v = splitmix64(&state);

		memcpy(hash->bytes + 8 * w, &v, sizeof(v));
	}
}

static uint64
seeded_marker(int64 seed, int i)
{
	return (uint64) seed * 1000000 + (uint64) i;
}

static const ProllyNodeStoreRoutine *
store_by_name(text *name)
{
	return prolly_nodestore_lookup(text_to_cstring(name));
}

Datum
prolly_store_write(PG_FUNCTION_ARGS)
{
	const ProllyNodeStoreRoutine *ns = store_by_name(PG_GETARG_TEXT_PP(0));
	int64		seed = PG_GETARG_INT64(1);
	int32		n = PG_GETARG_INT32(2);
	ProllyNodeStore *store = open_store(ns);

	for (int i = 0; i < n; i++)
	{
		ProllyNodeRef ref;
		ProllyHash	hash;
		Buffer		buf = write_node(ns, store, seeded_marker(seed, i), &ref);

		seeded_hash(seed, i, &hash);
		PROLLY_CALL(ns, publish, store, buf, &hash, &ref);
		UnlockReleaseBuffer(buf);
		CHECK_FOR_INTERRUPTS();
	}
	PROLLY_CALL(ns, close, store);
	PG_RETURN_INT32(n);
}

Datum
prolly_store_verify(PG_FUNCTION_ARGS)
{
	const ProllyNodeStoreRoutine *ns = store_by_name(PG_GETARG_TEXT_PP(0));
	int64		seed = PG_GETARG_INT64(1);
	int32		n = PG_GETARG_INT32(2);
	ProllyNodeStore *store = PROLLY_CALL(ns, open, NULL);

	for (int i = 0; i < n; i++)
	{
		ProllyNodeRef ref;

		memset(&ref, 0, sizeof(ref));
		seeded_hash(seed, i, &ref.hash);
		ref.location.blkno = InvalidBlockNumber;
		if (!PROLLY_CALL(ns, contains, store, &ref.hash))
			elog(ERROR, "node %d of seed " INT64_FORMAT " is not in the store",
				 i, seed);
		check_marker(ns, store, &ref, seeded_marker(seed, i), "after the fact");
		CHECK_FOR_INTERRUPTS();
	}
	PROLLY_CALL(ns, close, store);
	PG_RETURN_INT32(n);
}
