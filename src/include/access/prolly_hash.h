/*-------------------------------------------------------------------------
 *
 * prolly_hash.h
 *	  Content hash interface: hashing a node's canonical region.
 *
 * The hash function and its digest length are part of the canonical format
 * (specs/05-storage-targets.md, open question 4).  Candidates include
 * SHA-256 through src/common/cryptohash.c.  The chunker's boundary hash is a
 * separate concern of the chunker.
 *
 * Contract:
 * - Deterministic, and identical on every platform.
 * - The digest length is the same for every hash produced by one
 *   implementation, and at most PROLLY_HASH_MAX_LEN.
 * - finish() frees the context.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_hash.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_HASH_H
#define PROLLY_HASH_H

#include "access/prolly_registry.h"
#include "access/prolly_types.h"

typedef struct ProllyHashRoutine
{
	ProllyImplInfo info;

	ProllyHashCtx *(*begin) (MemoryContext mcxt);
	void		(*update) (ProllyHashCtx *ctx, const uint8 *data, Size len);
	void		(*finish) (ProllyHashCtx *ctx, ProllyHash *out);
} ProllyHashRoutine;

#define PROLLY_HASH_CALLBACK_NAMES "begin", "update", "finish"
#define PROLLY_HASH_FIRST_CALLBACK begin

static inline const ProllyHashRoutine *
prolly_hash_lookup(const char *name)
{
	return (const ProllyHashRoutine *)
		prolly_get_impl(PROLLY_IFACE_CONTENT_HASH, name);
}

#endif							/* PROLLY_HASH_H */
