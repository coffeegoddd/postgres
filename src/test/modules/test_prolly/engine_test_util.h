/*-------------------------------------------------------------------------
 *
 * engine_test_util.h
 *	  Helpers for the engine's unit tests: deterministic randomness, keys
 *	  and rows, calls that must succeed, stores and chunk closures (the
 *	  prototype engine's tests' helpers, on the server's).
 *
 * A test runs on this backend's memory store, emptied first, and then on
 * the history store ("pages") in a subtransaction rolled back after it,
 * its chunks of the engine's formats (ENGINE_TEST sets the format for the
 * test, and puts back the one before, whatever the test does).
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/test/modules/test_prolly/engine_test_util.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ENGINE_TEST_UTIL_H
#define ENGINE_TEST_UTIL_H

#include "access/xact.h"
#include "portability/instr_time.h"
#include "prolly/chunkstore.h"
#include "prolly/engine/prolly.h"
#include "utils/resowner.h"
#include "test_prolly.h"

/* The store a test's body runs on: "memory", then "pages". */
static const char *engine_test_store = "memory";

/* The store, empty if it is the memory one. */
static inline ChunkStore *
memstore_create(void)
{
	if (strcmp(engine_test_store, "memory") == 0)
		MemoryChunkStoreReset();
	return ChunkStoreOpen(engine_test_store);
}

static inline void
cs_destroy(ChunkStore *cs)
{
	if (strcmp(engine_test_store, "memory") == 0)
		MemoryChunkStoreReset();
}

/*
 * Runs body on the store named, with the engine's chunk format; on "pages"
 * in a subtransaction of its own, rolled back whatever the body does (an
 * error raised again after it), so it leaves nothing in the store.
 */
static inline void
engine_test_run(void (*body) (void), const char *store)
{
	const ChunkFormatRoutine *format = ChunkFormat;
	bool		own_xact = strcmp(store, "memory") != 0;
	MemoryContext cxt = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;

	engine_test_store = store;
	ChunkFormat = &EngineChunkFormat;
	if (own_xact)
	{
		BeginInternalSubTransaction(NULL);
		MemoryContextSwitchTo(cxt);
	}
	PG_TRY();
	{
		body();
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		ChunkFormat = format;
		engine_test_store = "memory";
		MemoryContextSwitchTo(cxt);
		edata = CopyErrorData();
		FlushErrorState();
		if (own_xact)
		{
			RollbackAndReleaseCurrentSubTransaction();
			MemoryContextSwitchTo(cxt);
			CurrentResourceOwner = owner;
		}
		if (edata->message)
			edata->message = psprintf("on store \"%s\": %s", store, edata->message);
		ReThrowError(edata);
	}
	PG_END_TRY();
	ChunkFormat = format;
	engine_test_store = "memory";
	if (own_xact)
	{
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(cxt);
		CurrentResourceOwner = owner;
	}
}

/* A unit test whose body runs on the memory store, then the pages one. */
#define ENGINE_TEST(name) \
	static void name##_body(void); \
	static void \
	name(void) \
	{ \
		engine_test_run(name##_body, "memory"); \
		engine_test_run(name##_body, "pages"); \
	} \
	static void \
	name##_body(void)

/*
 * A unit test whose body runs on the memory store alone: one that ends its
 * holds, or damages its store, by the memory store's hooks for tests.
 */
#define ENGINE_MEMORY_TEST(name) \
	static void name##_body(void); \
	static void \
	name(void) \
	{ \
		engine_test_run(name##_body, "memory"); \
	} \
	static void \
	name##_body(void)

/*
 * body(arg) raises an error of code code, its message naming the chunk h
 * unless h is NULL; in a subtransaction, rolled back after it.
 */
static inline void
expect_error(void (*body) (void *), void *arg, int code, const PHash *h)
{
	MemoryContext cxt = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	ErrorData  *edata = NULL;

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(cxt);
	PG_TRY();
	{
		body(arg);
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(cxt);
		edata = CopyErrorData();
		FlushErrorState();
	}
	PG_END_TRY();
	RollbackAndReleaseCurrentSubTransaction();
	MemoryContextSwitchTo(cxt);
	CurrentResourceOwner = owner;
	if (edata == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg_internal("CHECK failed: no error raised")));
	if (edata->sqlerrcode != code)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg_internal("CHECK failed: error \"%s\", of code %s, not %s",
								 edata->message, unpack_sql_state(edata->sqlerrcode),
								 unpack_sql_state(code))));
	if (h)
	{
		char		hex[PHASH_HEX_LEN + 1];

		phash_hex(h, hex);
		if (strstr(edata->message, hex) == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg_internal("CHECK failed: error \"%s\" names no chunk %s",
									 edata->message, hex)));
	}
	FreeErrorData(edata);
}

/* (the bytes of every chunk the store has) */
static void
add_len(void *arg, const uint8 *hash, uint32 len, const ChunkCounts *counts)
{
	*(uint64 *) arg += len;
}

static inline uint64
store_bytes(ChunkStore *cs)
{
	uint64		n = 0;

	cs_visit(cs, add_len, &n);
	return n;
}

static uint64 rng_state = UINT64CONST(0x9e3779b97f4a7c15);

static inline void
rng_seed_global(uint64 s)
{
	rng_state = s * UINT64CONST(0x9e3779b97f4a7c15) + 1;
}

static inline uint64
rnd_global(void)
{
	uint64		x = rng_state;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	rng_state = x;
	return x * UINT64CONST(0x2545f4914f6cdd1d);
}

static inline uint64
rnd_below_global(uint64 n)
{
	return rnd_global() % n;
}

/* (put_u64 and get_u64: test_prolly.h's) */

static inline double
now_sec(void)
{
	instr_time	t;

	INSTR_TIME_SET_CURRENT(t);
	return INSTR_TIME_GET_DOUBLE(t);
}

/* (the chunks the store has) */
static void
add_one(void *arg, const uint8 *hash, uint32 len, const ChunkCounts *counts)
{
	(*(uint64 *) arg)++;
}

static inline uint64
store_count(ChunkStore *cs)
{
	uint64		n = 0;

	cs_visit(cs, add_one, &n);
	return n;
}

#define CHECK_OK(expr) \
	do { \
		int			check_rc_ = (expr); \
		if (check_rc_ != PROLLY_OK) \
			ereport(ERROR, \
					(errcode(ERRCODE_INTERNAL_ERROR), \
					 errmsg_internal("CHECK failed: %s returned %d", #expr, check_rc_))); \
	} while (0)

/* A context on the store, its policy policy. */
static inline ProllyCtx
t_ctx(ChunkStore *cs, const ChunkPolicy *policy)
{
	ProllyCtx	ctx;

	prolly_ctx_init(&ctx, cs, policy);
	return ctx;
}

/* Wrappers for calls that must succeed. */
static inline PHash
t_empty(ProllyCtx *ctx)
{
	PHash		h;

	CHECK_OK(prolly_empty(ctx, 0, &h));
	return h;
}

static inline PHash
t_build(ProllyCtx *ctx, const ProllyKV *kvs, size_t n)
{
	PHash		h;

	CHECK_OK(prolly_build(ctx, 0, kvs, n, &h));
	return h;
}

static inline PHash
t_apply(ProllyCtx *ctx, const PHash *root, const ProllyEdit *e, size_t n)
{
	PHash		h;

	CHECK_OK(prolly_apply(ctx, root, e, n, &h));
	return h;
}

static inline bool
t_get(ProllyCtx *ctx, const PHash *root, const uint8 *key, size_t klen,
	  const uint8 **val, size_t *vlen)
{
	int			rc = prolly_get(ctx, root, key, klen, val, vlen);

	CHECK(rc == PROLLY_OK || rc == PROLLY_NOT_FOUND);
	return rc == PROLLY_OK;
}

static inline uint64
t_count(ProllyCtx *ctx, const PHash *root)
{
	uint64		n;

	CHECK_OK(prolly_count(ctx, root, &n));
	return n;
}

static inline int
t_height(ProllyCtx *ctx, const PHash *root)
{
	int			h;

	CHECK_OK(prolly_height(ctx, root, &h));
	return h;
}

static inline ProllyIter *
t_iter(ProllyCtx *ctx, const PHash *root, const uint8 *start, size_t slen)
{
	ProllyIter *it;

	CHECK_OK(prolly_iter_open(ctx, root, start, slen, &it));
	return it;
}

static inline bool
t_next(ProllyIter *it, const uint8 **k, size_t *kl, const uint8 **v,
	   size_t *vl)
{
	int			rc = prolly_iter_next(it, k, kl, v, vl);

	CHECK(rc >= 0);
	return rc == 1;
}

/* The store's audit passes, with at most max_orphans chunks nothing holds. */
static inline void
check_store(ChunkStore *cs, size_t max_orphans)
{
	uint64		chunks,
				unheld;
	char	   *why = NULL;

	if (!cs_check(cs, &chunks, &unheld, &why))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg_internal("store invariant broken: %s", why ? why : "?")));
	CHECK_LE(unheld, max_orphans);
}

/* Row values are 16 bytes: the key, then a version. */
#define VAL_LEN 16

static inline void
make_val(uint8 *o, uint64 key, uint64 version)
{
	put_u64(o, key);
	put_u64(o + 8, version);
}

/* A map of n rows with keys 0, 2, 4, ... and version 0. */
static inline PHash
build_even_rows(ProllyCtx *ctx, uint64 n)
{
	uint8	   *keys = px_malloc(n * 8);
	uint8	   *vals = px_malloc(n * VAL_LEN);
	ProllyKV   *kvs = px_malloc(n * sizeof(ProllyKV));
	PHash		root;

	for (uint64 i = 0; i < n; i++)
	{
		put_u64(keys + i * 8, i * 2);
		make_val(vals + i * VAL_LEN, i * 2, 0);
		kvs[i] = (ProllyKV) {keys + i * 8, 8, vals + i * VAL_LEN, VAL_LEN};
	}
	root = t_build(ctx, kvs, n);
	px_free(keys);
	px_free(vals);
	px_free(kvs);
	return root;
}

/* A batch of row edits with owned key and value storage. */
typedef struct EditBatch
{
	ProllyEdit *e;
	uint8	   *keys;
	uint8	   *vals;
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
batch_put(EditBatch *b, uint64 key, uint64 version)
{
	size_t		i = b->n++;

	put_u64(b->keys + i * 8, key);
	make_val(b->vals + i * VAL_LEN, key, version);
	b->e[i] = (ProllyEdit) {b->keys + i * 8, 8, b->vals + i * VAL_LEN, VAL_LEN, false};
}

static inline void
batch_del(EditBatch *b, uint64 key)
{
	size_t		i = b->n++;

	put_u64(b->keys + i * 8, key);
	b->e[i] = (ProllyEdit) {b->keys + i * 8, 8, NULL, 0, true};
}

static inline void
batch_free(EditBatch *b)
{
	px_free(b->e);
	px_free(b->keys);
	px_free(b->vals);
}

/* Chunk closures and hash sets. */
typedef struct NodeWalk
{
	ChunkStore *cs;
	PHashMap   *set;
} NodeWalk;

static void node_walk_fn(void *arg, const PHash *h, bool strong);

/* Every chunk in the strong closure of h. */
static inline void
closure(ChunkStore *cs, const PHash *h, PHashMap *set)
{
	NodeWalk	w = {cs, set};
	bool		created;
	uint32		len;
	uint8	   *d;

	phmap_ref(set, h, &created);
	if (!created)
		return;
	d = cs->routine->get(cs, h->b, &len);
	CHECK(d != NULL);
	CHECK(engine_chunk_refs(d, len, node_walk_fn, &w));
	pfree(d);
}

static void
node_walk_fn(void *arg, const PHash *h, bool strong)
{
	NodeWalk   *w = arg;

	if (strong)
		closure(w->cs, h, w->set);
}

static inline void
set_add_fn(void *arg, const PHash *h)
{
	bool		created;

	phmap_ref(arg, h, &created);
	CHECK(created);				/* chunk diff reports each node once */
}

static inline size_t
set_size(const PHashMap *m)
{
	return m->n;
}

/* a minus b, as a set */
static inline void
set_minus(const PHashMap *a, const PHashMap *b, PHashMap *out)
{
	bool		created;

	phmap_init(out);
	for (size_t i = 0; i < a->cap; i++)
		if (a->used[i] && !phmap_find((PHashMap *) b, &a->keys[i]))
			phmap_ref(out, &a->keys[i], &created);
}

static inline bool
set_equal(const PHashMap *a, const PHashMap *b)
{
	if (a->n != b->n)
		return false;
	for (size_t i = 0; i < a->cap; i++)
		if (a->used[i] && !phmap_find((PHashMap *) b, &a->keys[i]))
			return false;
	return true;
}

/* Small chunks make tall trees from few rows, exercising every level. */
static const ChunkPolicy small_policy = {64, 256, 1024};

/* the prototype's new_chunks: the server's stored */
#define STATS_DELTA(cs, before, field) (cs_stats(cs)->field - (before).field)

#endif							/* ENGINE_TEST_UTIL_H */
