/*-------------------------------------------------------------------------
 *
 * phash.c
 *	  Chunk names, and maps from them.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/engine/phash.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/cryptohash.h"
#include "common/sha2.h"
#include "prolly/engine/phash.h"

void
px_sha256(const void *data, size_t len, uint8 out[32])
{
	pg_cryptohash_ctx *ctx = pg_cryptohash_create(PG_SHA256);

	if (pg_cryptohash_init(ctx) < 0 ||
		pg_cryptohash_update(ctx, data, len) < 0 ||
		pg_cryptohash_final(ctx, out, PG_SHA256_DIGEST_LENGTH) < 0)
		elog(ERROR, "could not hash: %s", pg_cryptohash_error(ctx));
	pg_cryptohash_free(ctx);
}

void
phash_compute(const void *data, size_t len, PHash *out)
{
	ChunkHash(data, len, out->b);
}

void
phash_hex(const PHash *h, char out[PHASH_HEX_LEN + 1])
{
	static const char digits[] = "0123456789abcdef";

	for (int i = 0; i < PHASH_LEN; i++)
	{
		out[i * 2] = digits[h->b[i] >> 4];
		out[i * 2 + 1] = digits[h->b[i] & 15];
	}
	out[PHASH_HEX_LEN] = '\0';
}

static size_t
phmap_slot(const PHash *k, size_t cap)
{
	uint64		x;

	memcpy(&x, k->b, sizeof(x));
	return (size_t) (x & (cap - 1));
}

void
phmap_init(PHashMap *m)
{
	memset(m, 0, sizeof(*m));
}

void
phmap_free(PHashMap *m)
{
	px_free(m->keys);
	px_free(m->vals);
	px_free(m->used);
	memset(m, 0, sizeof(*m));
}

uint64 *
phmap_find(PHashMap *m, const PHash *k)
{
	if (m->cap == 0)
		return NULL;
	for (size_t i = phmap_slot(k, m->cap);; i = (i + 1) & (m->cap - 1))
	{
		if (!m->used[i])
			return NULL;
		if (phash_eq(&m->keys[i], k))
			return &m->vals[i];
	}
}

static void
phmap_grow(PHashMap *m)
{
	PHashMap	old = *m;
	size_t		cap = old.cap ? old.cap * 2 : 64;

	m->keys = px_malloc(cap * sizeof(PHash));
	m->vals = px_malloc(cap * sizeof(uint64));
	m->used = px_malloc(cap);
	memset(m->used, 0, cap);
	m->cap = cap;
	m->n = 0;
	for (size_t i = 0; i < old.cap; i++)
	{
		if (old.used[i])
		{
			bool		created;

			*phmap_ref(m, &old.keys[i], &created) = old.vals[i];
		}
	}
	phmap_free(&old);
}

uint64 *
phmap_ref(PHashMap *m, const PHash *k, bool *created)
{
	if ((m->n + 1) * 10 > m->cap * 7)
		phmap_grow(m);
	for (size_t i = phmap_slot(k, m->cap);; i = (i + 1) & (m->cap - 1))
	{
		if (!m->used[i])
		{
			m->used[i] = 1;
			m->keys[i] = *k;
			m->vals[i] = 0;
			m->n++;
			*created = true;
			return &m->vals[i];
		}
		if (phash_eq(&m->keys[i], k))
		{
			*created = false;
			return &m->vals[i];
		}
	}
}

bool
phmap_remove(PHashMap *m, const PHash *k)
{
	size_t		mask,
				i,
				j;

	if (m->cap == 0)
		return false;
	mask = m->cap - 1;
	for (i = phmap_slot(k, m->cap);; i = (i + 1) & mask)
	{
		if (!m->used[i])
			return false;
		if (phash_eq(&m->keys[i], k))
			break;
	}
	m->used[i] = 0;
	m->n--;
	/* (later entries of the probe run, that may now sit too far, shifted back) */
	for (j = (i + 1) & mask; m->used[j]; j = (j + 1) & mask)
	{
		size_t		home = phmap_slot(&m->keys[j], m->cap);
		bool		stays = i <= j ? (home > i && home <= j) : (home > i || home <= j);

		if (!stays)
		{
			m->keys[i] = m->keys[j];
			m->vals[i] = m->vals[j];
			m->used[i] = 1;
			m->used[j] = 0;
			i = j;
		}
	}
	return true;
}
