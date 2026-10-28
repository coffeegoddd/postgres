/*
 * phash.h
 *	  Content hashes (SHA-256 truncated to 20 bytes) and a hash-keyed map.
 */
#ifndef PHASH_H
#define PHASH_H

#include "prolly_util.h"

#define PHASH_LEN 20
#define PHASH_HEX_LEN (PHASH_LEN * 2)

typedef struct PHash
{
	uint8_t		b[PHASH_LEN];
} PHash;

extern void sha256(const void *data, size_t len, uint8_t out[32]);
extern void phash_compute(const void *data, size_t len, PHash *out);
extern void phash_hex(const PHash *h, char out[PHASH_HEX_LEN + 1]);

static inline bool
phash_eq(const PHash *a, const PHash *b)
{
	return memcmp(a->b, b->b, PHASH_LEN) == 0;
}

/* Open-addressing map from PHash to a uint64_t slot. */
typedef struct PHashMap
{
	PHash	   *keys;
	uint64_t   *vals;
	uint8_t    *used;
	size_t		cap;
	size_t		n;
} PHashMap;

extern void phmap_init(PHashMap *m);
extern void phmap_free(PHashMap *m);
extern uint64_t *phmap_find(PHashMap *m, const PHash *k);
/* Returns the slot for k, inserting it with value 0 if absent. */
extern uint64_t *phmap_ref(PHashMap *m, const PHash *k, bool *created);

#endif							/* PHASH_H */
