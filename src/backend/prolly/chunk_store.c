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

#include "prolly/chunk_log.h"
#include "prolly/chunk_store.h"

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
	data = palloc(Max(e.len, 1));
	ChunkLog->read(log, e.loc, data, e.len);
	*len = e.len;
	return data;
}

bool
ChunkStorePut(Relation log, Relation idx, const uint8 *hash,
			  const void *data, uint32 len)
{
	ChunkIndexEntry e;

	if (ChunkIndex->lookup(idx, hash, &e))
		return false;
	memset(&e, 0, sizeof(e));
	memcpy(e.hash, hash, CHUNK_HASH_LEN);
	e.len = len;
	e.loc = ChunkLog->append(log, data, len);
	return ChunkIndex->insert(idx, &e);
}
