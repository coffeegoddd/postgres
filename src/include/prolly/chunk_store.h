/*-------------------------------------------------------------------------
 *
 * chunk_store.h
 *	  The history store's chunks, from C: has, get and put over a chunk log
 *	  (prolly/chunk_log.h) and its index (prolly/chunk_index.h), with no SQL
 *	  and no executor.
 *
 * A chunk is put by appending its bytes to the log and inserting its
 * entry, with no references, into the index; one already there is not put
 * again.  Two backends putting one chunk at once both append it: the
 * entry of one is the chunk's, and the other's bytes are unused.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/chunk_store.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_CHUNK_STORE_H
#define PROLLY_CHUNK_STORE_H

#include "prolly/chunk_index.h"

/* A chunk's name: the first CHUNK_HASH_LEN bytes of the SHA-256 of its bytes. */
extern void ChunkHash(const void *data, size_t len, uint8 *out);

/* Whether the store has the chunk named hash. */
extern bool ChunkStoreHas(Relation log, Relation idx, const uint8 *hash);

/*
 * The chunk's bytes, palloc'd, and their length in *len; NULL if absent.
 * Bytes that don't hash to its name are an error.
 */
extern void *ChunkStoreGet(Relation log, Relation idx, const uint8 *hash,
						   uint32 *len);

/*
 * Stores the chunk unless the store has it; true if it stored it.  A hash
 * that isn't the bytes' is an error.
 */
extern bool ChunkStorePut(Relation log, Relation idx, const uint8 *hash,
						  const void *data, uint32 len);

#endif							/* PROLLY_CHUNK_STORE_H */
