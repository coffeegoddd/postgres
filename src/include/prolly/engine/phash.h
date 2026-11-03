/*-------------------------------------------------------------------------
 *
 * phash.h
 *	  A chunk's name in the engine (the history store's: the first
 *	  CHUNK_HASH_LEN bytes of the SHA-256 of its bytes, prolly/chunk_store.h's
 *	  ChunkHash), and a map from names.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/engine/phash.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_ENGINE_PHASH_H
#define PROLLY_ENGINE_PHASH_H

#include "prolly/chunk_store.h"
#include "prolly/engine/util.h"

#define PHASH_LEN CHUNK_HASH_LEN
#define PHASH_HEX_LEN (PHASH_LEN * 2)

typedef struct PHash
{
	uint8		b[PHASH_LEN];
} PHash;

StaticAssertDecl(sizeof(PHash) == PHASH_LEN, "a PHash is its bytes");

/* The full SHA-256 of the bytes (pg_cryptohash). */
extern void px_sha256(const void *data, size_t len, uint8 out[32]);
extern void phash_compute(const void *data, size_t len, PHash *out);
extern void phash_hex(const PHash *h, char out[PHASH_HEX_LEN + 1]);

static inline bool
phash_eq(const PHash *a, const PHash *b)
{
	return memcmp(a->b, b->b, PHASH_LEN) == 0;
}

/* Open-addressing map from a PHash to a uint64 slot. */
typedef struct PHashMap
{
	PHash	   *keys;
	uint64	   *vals;
	uint8	   *used;
	size_t		cap;
	size_t		n;
} PHashMap;

extern void phmap_init(PHashMap *m);
extern void phmap_free(PHashMap *m);
extern uint64 *phmap_find(PHashMap *m, const PHash *k);
/* The slot for k, inserted with value 0 if absent. */
extern uint64 *phmap_ref(PHashMap *m, const PHash *k, bool *created);
extern bool phmap_remove(PHashMap *m, const PHash *k);

#endif							/* PROLLY_ENGINE_PHASH_H */
