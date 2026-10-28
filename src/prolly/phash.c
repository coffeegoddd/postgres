/*
 * phash.c
 *	  SHA-256, content hashes and the PHash map.
 */
#include "phash.h"

static const uint32_t sha256_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void
sha256_block(uint32_t st[8], const uint8_t *p)
{
	uint32_t	w[64];
	uint32_t	a, b, c, d, e, f, g, h;

	for (int i = 0; i < 16; i++)
		w[i] = (uint32_t) p[i * 4] << 24 | (uint32_t) p[i * 4 + 1] << 16 |
			(uint32_t) p[i * 4 + 2] << 8 | (uint32_t) p[i * 4 + 3];
	for (int i = 16; i < 64; i++)
	{
		uint32_t	s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t	s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	a = st[0], b = st[1], c = st[2], d = st[3];
	e = st[4], f = st[5], g = st[6], h = st[7];
	for (int i = 0; i < 64; i++)
	{
		uint32_t	S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
		uint32_t	ch = (e & f) ^ (~e & g);
		uint32_t	t1 = h + S1 + ch + sha256_k[i] + w[i];
		uint32_t	S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
		uint32_t	mj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t	t2 = S0 + mj;

		h = g, g = f, f = e, e = d + t1;
		d = c, c = b, b = a, a = t1 + t2;
	}
	st[0] += a, st[1] += b, st[2] += c, st[3] += d;
	st[4] += e, st[5] += f, st[6] += g, st[7] += h;
}

void
sha256(const void *data, size_t len, uint8_t out[32])
{
	uint32_t	st[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
	0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	const uint8_t *p = data;
	size_t		rem = len;
	uint8_t		tail[128];
	size_t		tlen;
	uint64_t	bits = (uint64_t) len * 8;

	while (rem >= 64)
	{
		sha256_block(st, p);
		p += 64;
		rem -= 64;
	}
	memcpy(tail, p, rem);
	tail[rem] = 0x80;
	tlen = rem + 1 <= 56 ? 64 : 128;
	memset(tail + rem + 1, 0, tlen - rem - 1);
	for (int i = 0; i < 8; i++)
		tail[tlen - 1 - i] = (uint8_t) (bits >> (8 * i));
	sha256_block(st, tail);
	if (tlen == 128)
		sha256_block(st, tail + 64);
	for (int i = 0; i < 8; i++)
	{
		out[i * 4] = (uint8_t) (st[i] >> 24);
		out[i * 4 + 1] = (uint8_t) (st[i] >> 16);
		out[i * 4 + 2] = (uint8_t) (st[i] >> 8);
		out[i * 4 + 3] = (uint8_t) st[i];
	}
}

void
phash_compute(const void *data, size_t len, PHash *out)
{
	uint8_t		full[32];

	sha256(data, len, full);
	memcpy(out->b, full, PHASH_LEN);
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
	uint64_t	x;

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
	free(m->keys);
	free(m->vals);
	free(m->used);
	memset(m, 0, sizeof(*m));
}

uint64_t *
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
	m->vals = px_malloc(cap * sizeof(uint64_t));
	m->used = calloc(cap, 1);
	if (m->used == NULL)
		abort();
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

uint64_t *
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
	/* shift back later entries of the probe run that may now sit too far */
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
