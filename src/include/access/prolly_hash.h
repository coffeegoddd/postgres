/*-------------------------------------------------------------------------
 *
 * prolly_hash.h
 *	  Content hash interface: hashing a node's canonical region.
 *
 * The hash function and its digest length are part of the canonical format:
 * the server names every chunk with one (prolly_content_hash()).  The
 * chunker's boundary hash is a separate concern of the chunker.
 *
 * Contract:
 * - Deterministic, and identical on every platform.
 * - The digest length is the same for every hash produced by one
 *   implementation, and at most PROLLY_HASH_MAX_LEN.
 * - finish() frees the context.
 * - A digest's first four bytes are spread evenly over its inputs: tables
 *   keyed by content hash take them as the key's hash, computing nothing
 *   (prolly_hash_bucket()).
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

/*
 * A content hash's hash in a table keyed by it: its first four bytes, spread
 * evenly already (the contract above).  bytes holds at least four (a key of
 * a shorter hash, zero-padded, still hashes deterministically).
 */
static inline uint32
prolly_hash_bucket(const uint8 *bytes)
{
	uint32		v;

	memcpy(&v, bytes, sizeof(v));
	return v;
}

#define PROLLY_HASH_CALLBACK_NAMES "begin", "update", "finish"
#define PROLLY_HASH_FIRST_CALLBACK begin
#define PROLLY_HASH_LAST_CALLBACK finish

static inline const ProllyHashRoutine *
prolly_hash_lookup(const char *name)
{
	return (const ProllyHashRoutine *)
		prolly_get_impl(PROLLY_IFACE_CONTENT_HASH, name);
}

#endif							/* PROLLY_HASH_H */
