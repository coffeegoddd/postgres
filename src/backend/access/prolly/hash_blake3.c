/*-------------------------------------------------------------------------
 *
 * hash_blake3.c
 *	  Content hash implementation: BLAKE3, its first 20 bytes.
 *
 * BLAKE3 through src/common's copy of its reference implementation
 * (common/blake3.h), which picks the widest SIMD instructions the CPU has
 * at run time; every variant gives the same digest, so the canonical format
 * doesn't depend on the machine.  The digest is BLAKE3's default output cut
 * to its first 20 bytes, as Dolt keeps 20 bytes of its hash.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/hash_blake3.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"
#include "common/blake3.h"
#include "pgstat.h"

#define BLAKE3_DIGEST_LEN	20

struct ProllyHashCtx
{
	blake3_hasher hasher;
};

static ProllyHashCtx *
blake3_begin(MemoryContext mcxt)
{
	ProllyHashCtx *hctx = MemoryContextAlloc(mcxt, sizeof(ProllyHashCtx));

	blake3_hasher_init(&hctx->hasher);
	return hctx;
}

static void
blake3_update(ProllyHashCtx *hctx, const uint8 *data, Size len)
{
	pgstat_count_prolly(PGSTAT_PROLLY_BYTES_HASHED, len);
	blake3_hasher_update(&hctx->hasher, data, len);
}

static void
blake3_finish(ProllyHashCtx *hctx, ProllyHash *out)
{
	StaticAssertStmt(BLAKE3_DIGEST_LEN <= PROLLY_HASH_MAX_LEN,
					 "BLAKE3 digest does not fit ProllyHash");

	blake3_hasher_finalize(&hctx->hasher, out->bytes, BLAKE3_DIGEST_LEN);
	out->len = BLAKE3_DIGEST_LEN;
	pfree(hctx);
}

const ProllyHashRoutine prolly_hash_blake3 = {
	.info = {
		.iface = PROLLY_IFACE_CONTENT_HASH,
		.impl_id = 2,
		.format_version = 1,
		.name = "blake3",
		.description = "BLAKE3, its first 20 bytes",
	},
	.begin = blake3_begin,
	.update = blake3_update,
	.finish = blake3_finish,
};
