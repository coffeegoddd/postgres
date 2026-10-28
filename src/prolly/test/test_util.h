/*
 * test_util.h
 *	  Assertions, deterministic randomness and key helpers for the tests.
 */
#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <time.h>

#include "vc.h"

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "\n%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			exit(1); \
		} \
	} while (0)

#define CHECK_LE(a, b) \
	do { \
		unsigned long long _a = (a), _b = (b); \
		if (!(_a <= _b)) { \
			fprintf(stderr, "\n%s:%d: CHECK failed: %s <= %s (%llu > %llu)\n", \
					__FILE__, __LINE__, #a, #b, _a, _b); \
			exit(1); \
		} \
	} while (0)

#define CHECK_EQ(a, b) \
	do { \
		unsigned long long _a = (a), _b = (b); \
		if (_a != _b) { \
			fprintf(stderr, "\n%s:%d: CHECK failed: %s == %s (%llu != %llu)\n", \
					__FILE__, __LINE__, #a, #b, _a, _b); \
			exit(1); \
		} \
	} while (0)

static inline double
now_sec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

#define RUN(fn) \
	do { \
		double _t = now_sec(); \
		printf("%-44s", #fn); \
		fflush(stdout); \
		fn(); \
		printf("ok  %.2fs\n", now_sec() - _t); \
	} while (0)

static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;

static inline void
rng_seed(uint64_t s)
{
	rng_state = s * 0x9e3779b97f4a7c15ULL + 1;
}

static inline uint64_t
rnd(void)
{
	uint64_t	x = rng_state;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	rng_state = x;
	return x * 0x2545f4914f6cdd1dULL;
}

static inline uint64_t
rnd_below(uint64_t n)
{
	return rnd() % n;
}

static inline void
put_u64(uint8_t *o, uint64_t v)
{
	for (int i = 7; i >= 0; i--, v >>= 8)
		o[i] = (uint8_t) v;
}

static inline uint64_t
get_u64(const uint8_t *p)
{
	uint64_t	v = 0;

	for (int i = 0; i < 8; i++)
		v = v << 8 | p[i];
	return v;
}

/* Row values are 16 bytes: the key, then a version. */
#define VAL_LEN 16

static inline void
make_val(uint8_t *o, uint64_t key, uint64_t version)
{
	put_u64(o, key);
	put_u64(o + 8, version);
}

/* Builds a map of n rows with keys 0, 2, 4, ... and version 0. */
static inline PHash
build_even_rows(ProllyCtx *ctx, uint64_t n)
{
	uint8_t    *keys = px_malloc(n * 8);
	uint8_t    *vals = px_malloc(n * VAL_LEN);
	ProllyKV   *kvs = px_malloc(n * sizeof(ProllyKV));
	PHash		root;

	for (uint64_t i = 0; i < n; i++)
	{
		put_u64(keys + i * 8, i * 2);
		make_val(vals + i * VAL_LEN, i * 2, 0);
		kvs[i] = (ProllyKV) {keys + i * 8, 8, vals + i * VAL_LEN, VAL_LEN};
	}
	root = prolly_build(ctx, kvs, n);
	free(keys);
	free(vals);
	free(kvs);
	return root;
}

/* A batch of row edits with owned key and value storage. */
typedef struct EditBatch
{
	ProllyEdit *e;
	uint8_t    *keys;
	uint8_t    *vals;
	size_t		n;
} EditBatch;

static inline void
batch_init(EditBatch *b, size_t cap)
{
	b->e = px_malloc(cap * sizeof(ProllyEdit));
	b->keys = px_malloc(cap * 8);
	b->vals = px_malloc(cap * VAL_LEN);
	b->n = 0;
}

static inline void
batch_put(EditBatch *b, uint64_t key, uint64_t version)
{
	size_t		i = b->n++;

	put_u64(b->keys + i * 8, key);
	make_val(b->vals + i * VAL_LEN, key, version);
	b->e[i] = (ProllyEdit) {b->keys + i * 8, 8, b->vals + i * VAL_LEN, VAL_LEN, false};
}

static inline void
batch_del(EditBatch *b, uint64_t key)
{
	size_t		i = b->n++;

	put_u64(b->keys + i * 8, key);
	b->e[i] = (ProllyEdit) {b->keys + i * 8, 8, NULL, 0, true};
}

static inline void
batch_free(EditBatch *b)
{
	free(b->e);
	free(b->keys);
	free(b->vals);
}

/* Small chunks make tall trees from few rows, exercising every level. */
static const ChunkPolicy small_policy = {64, 256, 1024};

#define STATS_DELTA(cs, before, field) ((cs)->stats.field - (before).field)

#endif							/* TEST_UTIL_H */
