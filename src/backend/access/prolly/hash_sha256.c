/*-------------------------------------------------------------------------
 *
 * hash_sha256.c
 *	  Content hash implementation: SHA-256 through src/common/cryptohash.c.
 *
 * The digest is the full 32 bytes.  cryptohash uses OpenSSL when Postgres is
 * built with it (hardware-accelerated where the CPU allows), and its own
 * implementation otherwise; both give identical digests, so the canonical
 * format doesn't depend on the build.
 *
 * In the backend with OpenSSL, cryptohash contexts are tracked by the
 * current ResourceOwner, so begin() needs one; an error between begin() and
 * finish() releases the context with the owner.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/hash_sha256.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"
#include "common/cryptohash.h"
#include "common/sha2.h"
#include "pgstat.h"

struct ProllyHashCtx
{
	pg_cryptohash_ctx *ctx;
};

static void
sha256_error(pg_cryptohash_ctx *ctx, const char *what)
{
	elog(ERROR, "could not %s SHA-256 content hash: %s",
		 what, pg_cryptohash_error(ctx));
}

static ProllyHashCtx *
sha256_begin(MemoryContext mcxt)
{
	ProllyHashCtx *hctx = MemoryContextAlloc(mcxt, sizeof(ProllyHashCtx));

	hctx->ctx = pg_cryptohash_create(PG_SHA256);
	if (hctx->ctx == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	if (pg_cryptohash_init(hctx->ctx) < 0)
		sha256_error(hctx->ctx, "initialize");
	return hctx;
}

static void
sha256_update(ProllyHashCtx *hctx, const uint8 *data, Size len)
{
	pgstat_count_prolly(PGSTAT_PROLLY_BYTES_HASHED, len);
	if (pg_cryptohash_update(hctx->ctx, data, len) < 0)
		sha256_error(hctx->ctx, "update");
}

static void
sha256_finish(ProllyHashCtx *hctx, ProllyHash *out)
{
	StaticAssertStmt(PG_SHA256_DIGEST_LENGTH <= PROLLY_HASH_MAX_LEN,
					 "SHA-256 digest does not fit ProllyHash");

	if (pg_cryptohash_final(hctx->ctx, out->bytes, sizeof(out->bytes)) < 0)
		sha256_error(hctx->ctx, "finish");
	out->len = PG_SHA256_DIGEST_LENGTH;
	pg_cryptohash_free(hctx->ctx);
	pfree(hctx);
}

const ProllyHashRoutine prolly_hash_sha256 = {
	.info = {
		.iface = PROLLY_IFACE_CONTENT_HASH,
		.impl_id = 1,
		.format_version = 1,
		.name = "sha256",
		.description = "SHA-256 (32-byte digest) through cryptohash",
	},
	.begin = sha256_begin,
	.update = sha256_update,
	.finish = sha256_finish,
};
