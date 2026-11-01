/*-------------------------------------------------------------------------
 *
 * chunk_store.c
 *	  The history store's chunks, from C (prolly/chunk_store.h).
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/chunk_store.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "common/cryptohash.h"
#include "common/sha2.h"
#include "miscadmin.h"
#include "prolly/chunk_log.h"
#include "prolly/chunk_store.h"
#include "utils/builtins.h"
#include "utils/rel.h"

void
ChunkHash(const void *data, size_t len, uint8 *out)
{
	pg_cryptohash_ctx *ctx = pg_cryptohash_create(PG_SHA256);
	uint8		digest[PG_SHA256_DIGEST_LENGTH];

	if (pg_cryptohash_init(ctx) < 0 ||
		pg_cryptohash_update(ctx, data, len) < 0 ||
		pg_cryptohash_final(ctx, digest, sizeof(digest)) < 0)
		elog(ERROR, "could not hash a chunk: %s", pg_cryptohash_error(ctx));
	pg_cryptohash_free(ctx);
	memcpy(out, digest, CHUNK_HASH_LEN);
}

/*
 * Bytes that aren't hash's are an error: of the chunk read from log at loc,
 * or (log NULL) of a put.
 */
static void
check_hash(const uint8 *hash, const void *data, size_t len, Relation log,
		   uint64 loc)
{
	uint8		actual[CHUNK_HASH_LEN];

	ChunkHash(data, len, actual);
	if (memcmp(actual, hash, CHUNK_HASH_LEN) != 0)
	{
		char		want[2 * CHUNK_HASH_LEN + 1];
		char		got[2 * CHUNK_HASH_LEN + 1];

		hex_encode((const char *) hash, CHUNK_HASH_LEN, want);
		want[2 * CHUNK_HASH_LEN] = '\0';
		hex_encode((const char *) actual, CHUNK_HASH_LEN, got);
		got[2 * CHUNK_HASH_LEN] = '\0';
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 log ?
				 errmsg("stored chunk %s does not match its hash", want) :
				 errmsg("bytes put as chunk %s do not hash to it", want),
				 log ?
				 errdetail("Its %zu bytes at %llu in chunk log \"%s\" hash to %s.",
						   len, (unsigned long long) loc,
						   RelationGetRelationName(log), got) :
				 errdetail("Its %zu bytes hash to %s.", len, got)));
	}
}

bool
ChunkStoreHas(Relation log, Relation idx, const uint8 *hash)
{
	ChunkIndexEntry e;

	return ChunkIndex->lookup(idx, hash, &e);
}

void *
ChunkStoreGet(Relation log, Relation idx, const uint8 *hash, uint32 *len)
{
	ChunkIndexEntry e;
	void	   *data;

	if (!ChunkIndex->lookup(idx, hash, &e))
		return NULL;
	data = palloc_extended(Max(e.len, 1), MCXT_ALLOC_HUGE);
	ChunkLog->read(log, e.loc, data, e.len);
	check_hash(hash, data, e.len, log, e.loc);
	*len = e.len;
	return data;
}

bool
ChunkStorePut(Relation log, Relation idx, const uint8 *hash,
			  const void *data, uint32 len)
{
	ChunkIndexEntry e;

	PreventCommandIfReadOnly("put a chunk");
	check_hash(hash, data, len, NULL, 0);

	/*
	 * (a transaction id first, so that the commit of a put that finds the
	 * chunk there already flushes the WAL that put it, whoever wrote it)
	 */
	(void) GetCurrentTransactionId();
	if (ChunkIndex->lookup(idx, hash, &e))
		return false;
	memset(&e, 0, sizeof(e));
	memcpy(e.hash, hash, CHUNK_HASH_LEN);
	e.len = len;
	e.loc = ChunkLog->append(log, data, len);
	return ChunkIndex->insert(idx, &e);
}
