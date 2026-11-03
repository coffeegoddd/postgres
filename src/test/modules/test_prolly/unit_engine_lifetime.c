/*-------------------------------------------------------------------------
 *
 * unit_engine_lifetime.c
 *	  Unit tests of the engine's trees' lifetime in a chunk store: chunks
 *	  checked as they are put, references and roots holding trees, holds
 *	  keeping the work in flight, freeing exactly what a tree alone
 *	  reached, damage named, exact chunk diffs, repair from another store,
 *	  rebuilding from reverse deltas, and what each costs.
 *
 * Most run on the memory store alone: they end its holds, as a
 * transaction's end would, to free within a test, or damage it.  The
 * scaling test runs on both, but for severing and repair.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/test/modules/test_prolly/unit_engine_lifetime.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "engine_test_util.h"

/* ---------------------------------------------------------------- helpers */

static PHashMap
chunk_diff_set(ProllyCtx *ctx, const PHash *a, const PHash *b)
{
	PHashMap	d;

	phmap_init(&d);
	CHECK_OK(prolly_chunk_diff(ctx, a, b, set_add_fn, &d));
	return d;
}

/* Rows of a tree as a key -> version array; returns the row count. */
static uint64
read_rows(ProllyCtx *ctx, const PHash *root, int64 *ver, uint64 keyspace)
{
	ProllyIter *it = t_iter(ctx, root, NULL, 0);
	const uint8 *k,
			   *v;
	size_t		kl,
				vl;
	uint64		n = 0;

	for (uint64 i = 0; i < keyspace; i++)
		ver[i] = -1;
	while (t_next(it, &k, &kl, &v, &vl))
	{
		uint64		key = get_u64(k);

		CHECK(key < keyspace);
		ver[key] = (int64) get_u64(v + 8);
		n++;
	}
	prolly_iter_close(it);
	return n;
}

/* A root taken on h. */
static PHash
held(ChunkStore *cs, PHash h)
{
	cs_ref_root(cs, h.b);
	return h;
}

/*
 * The store's holds ended, as its transaction's end would, and what
 * nothing else holds freed; how many.
 */
static uint64
release(ChunkStore *cs)
{
	if (cs->routine != &MemoryChunkStore)
		return 0;				/* (held until the test's subtransaction ends) */
	MemoryChunkStoreEndHolds(cs);
	return cs_free_unheld(cs);
}

/* A tree's root let go of, and it freed with all it alone reached. */
static uint64
sever(ChunkStore *cs, const PHash *h)
{
	cs_unref_root(cs, h->b);
	return cs_free(cs, h->b, 1);
}

static uint64
level_of(ChunkStore *cs, const PHash *h)
{
	uint32		len;
	uint8	   *d = cs_get(cs, h->b, &len);
	NodeHeader	hdr;

	CHECK(d && engine_node_header_parse(d, len, &hdr));
	pfree(d);
	return (uint64) hdr.level;
}

/* The leaf of root that holds key (the level-0 chunk an update rewrites). */
static PHash
leaf_holding(ProllyCtx *ctx, const PHash *root, uint64 key)
{
	EditBatch	b;
	PHash		edited,
				leaf = {{0}};
	PHashMap	d;
	int			found = 0;

	batch_init(&b, 1);
	batch_put(&b, key, 999999);
	edited = t_apply(ctx, root, b.e, b.n);
	d = chunk_diff_set(ctx, root, &edited);
	for (size_t i = 0; i < d.cap; i++)
		if (d.used[i] && level_of(ctx->cs, &d.keys[i]) == 0)
		{
			leaf = d.keys[i];
			found++;
		}
	CHECK_EQ(found, 1);
	phmap_free(&d);
	batch_free(&b);
	return leaf;
}

/* (a store of a test's own, copied from: a's closure, a root on a) */
static void
archive_copy(ChunkStore *archive, ChunkStore *src, const PHash *root)
{
	(void) cs_copy_closure(src, archive, root->b, 1, "copy", "the store");
	cs_ref_root(archive, root->b);
}

/* ------------------------------------------------------------- format */

ENGINE_TEST(test_edits_keep_tree_policy)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	small = t_ctx(cs, &small_policy),
				dflt = t_ctx(cs, &prolly_default_policy);
	PHash		a,
				d,
				edited,
				want_small,
				want_dflt;
	EditBatch	b;

	a = build_even_rows(&small, 5000);
	batch_init(&b, 3);
	batch_put(&b, 1, 1);
	batch_put(&b, 5001, 1);
	batch_del(&b, 4000);
	/* edited through a context whose policy differs: the tree's own wins */
	edited = t_apply(&dflt, &a, b.e, b.n);
	want_small = t_apply(&small, &a, b.e, b.n);
	CHECK(phash_eq(&edited, &want_small));
	d = build_even_rows(&dflt, 5000);
	want_dflt = t_apply(&dflt, &d, b.e, b.n);
	CHECK(!phash_eq(&edited, &want_dflt));
	batch_free(&b);
	cs_destroy(cs);
}

typedef struct PutArg
{
	ChunkStore *cs;
	const void *data;
	uint32		len;
} PutArg;

static void
put_body(void *arg)
{
	PutArg	   *p = arg;
	uint8		hash[CHUNK_HASH_LEN];

	(void) cs_put(p->cs, p->data, p->len, hash);
}

ENGINE_MEMORY_TEST(test_bad_chunks_rejected)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &small_policy);
	Buf			b = BUF_INIT;
	PHash		garbage,
				leaf,
				root;
	ChunkPolicy broken = {4096, 4096, 8192};	/* target must exceed min */
	static const uint8 junk[] = "not a chunk";
	PutArg		p = {cs};
	uint8		key[8];
	const uint8 *v;
	size_t		vl;

	/* unparsable bytes and invalid policies never enter the store */
	p.data = junk;
	p.len = sizeof(junk);
	expect_error(put_body, &p, ERRCODE_DATA_CORRUPTED, NULL);
	engine_node_header_write(&b, &broken, 0, 0, 0);
	p.data = b.data;
	p.len = b.len;
	expect_error(put_body, &p, ERRCODE_DATA_CORRUPTED, NULL);
	CHECK_EQ(store_count(cs), 0);

	/* a strong reference to an absent chunk is refused, naming it */
	memset(garbage.b, 0xab, PHASH_LEN);
	b.len = 0;
	engine_node_header_write(&b, &small_policy, 1, 0, 1);
	put_u64(key, 4);
	buf_append_uvarint(&b, 8);
	buf_append(&b, key, 8);
	buf_append_uvarint(&b, PHASH_LEN + 1);
	buf_append(&b, garbage.b, PHASH_LEN);
	buf_append_byte(&b, 3);
	p.data = b.data;
	p.len = b.len;
	expect_error(put_body, &p, ERRCODE_FOREIGN_KEY_VIOLATION, &garbage);
	CHECK_EQ(store_count(cs), 0);

	/* damage below a valid parent: CORRUPT, at the damaged chunk */
	leaf = build_even_rows(&ctx, 3);
	memcpy(b.data + b.len - 1 - PHASH_LEN, leaf.b, PHASH_LEN);
	CHECK_OK(prolly_put_chunk(&ctx, b.data, b.len, &root));
	MemoryChunkStoreDamage(cs, leaf.b, 0);
	put_u64(key, 2);
	CHECK_EQ(prolly_get(&ctx, &root, key, 8, &v, &vl), PROLLY_ERR_CORRUPT);
	CHECK(ctx.err_has_hash && phash_eq(&ctx.err_hash, &leaf));
	buf_free(&b);
	cs_destroy(cs);
}

ENGINE_TEST(test_child_level_mismatch_is_corrupt)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &small_policy);
	PHash		leaf,
				root;
	Buf			b = BUF_INIT;
	uint8		val[PHASH_LEN + 1];
	uint8		k[8];
	const uint8 *v;
	size_t		vl;

	leaf = build_even_rows(&ctx, 3);
	/* a level-2 node pointing straight at a leaf skips a level */
	memcpy(val, leaf.b, PHASH_LEN);
	val[PHASH_LEN] = 3;
	engine_node_header_write(&b, &small_policy, 2, 0, 1);
	buf_append_uvarint(&b, 8);
	put_u64(k, 4);
	buf_append(&b, k, 8);
	buf_append_uvarint(&b, sizeof(val));
	buf_append(&b, val, sizeof(val));
	CHECK_OK(prolly_put_chunk(&ctx, b.data, b.len, &root));
	put_u64(k, 2);
	CHECK_EQ(prolly_get(&ctx, &root, k, 8, &v, &vl), PROLLY_ERR_CORRUPT);
	CHECK(ctx.err_has_hash && phash_eq(&ctx.err_hash, &leaf));
	buf_free(&b);
	cs_destroy(cs);
}

/* --------------------------------------------------- holds and freeing */

typedef struct ApplyArg
{
	ProllyCtx  *ctx;
	const PHash *root;
	const ProllyEdit *e;
	size_t		n;
} ApplyArg;

static void
apply_body(void *arg)
{
	ApplyArg   *a = arg;
	PHash		out;

	(void) prolly_apply(a->ctx, a->root, a->e, a->n, &out);
}

/* A map whose values hold other trees: the trees live as long as the map. */
ENGINE_MEMORY_TEST(test_value_refs_hold_trees)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &small_policy);
	PHash		tables[3],
				map,
				smaller,
				missing,
				out;
	uint8		vals[3][PHASH_LEN + 4];
	uint8		keys[3][8];
	ProllyKV	kvs[3];
	ProllyEdit	e;
	ApplyArg	aa = {&ctx, NULL, &e, 1};
	PHashMap	before,
				after,
				gone;

	for (int i = 0; i < 3; i++)
	{
		tables[i] = build_even_rows(&ctx, 2000 + 1000 * (uint64) i);
		memcpy(vals[i], tables[i].b, PHASH_LEN);
		memcpy(vals[i] + PHASH_LEN, "meta", 4);
		put_u64(keys[i], (uint64) i);
		kvs[i] = (ProllyKV) {keys[i], 8, vals[i], sizeof(vals[i])};
	}
	CHECK_OK(prolly_build(&ctx, 1, kvs, 3, &map));
	held(cs, map);
	release(cs);
	check_store(cs, 0);
	for (int i = 0; i < 3; i++)
		CHECK_EQ(t_count(&ctx, &tables[i]), 2000 + 1000 * (uint64) i);

	/* dropping one entry frees exactly what only the old map reached */
	e = (ProllyEdit) {keys[1], 8, NULL, 0, true};
	CHECK_OK(prolly_apply(&ctx, &map, &e, 1, &smaller));
	held(cs, smaller);
	release(cs);
	phmap_init(&before);
	phmap_init(&after);
	closure(cs, &map, &before);
	closure(cs, &smaller, &after);
	set_minus(&before, &after, &gone);
	CHECK(phmap_find(&gone, &tables[1]) != NULL);
	CHECK_EQ(sever(cs, &map), set_size(&gone));
	CHECK(!cs_has(cs, tables[1].b));
	check_store(cs, 0);
	map = smaller;
	phmap_free(&before);
	phmap_free(&after);
	phmap_free(&gone);

	/* a value naming an absent tree is refused, naming it */
	memset(missing.b, 0x5a, PHASH_LEN);
	memcpy(vals[0], missing.b, PHASH_LEN);
	e = (ProllyEdit) {keys[0], 8, vals[0], sizeof(vals[0]), false};
	aa.root = &map;
	expect_error(apply_body, &aa, ERRCODE_FOREIGN_KEY_VIOLATION, &missing);
	/* a value too short to hold its references is refused */
	e = (ProllyEdit) {keys[0], 8, vals[0], 5, false};
	CHECK_EQ(prolly_apply(&ctx, &map, &e, 1, &out), PROLLY_ERR_BAD_VALUE);
	release(cs);
	check_store(cs, 0);

	/* dropping the map frees everything */
	sever(cs, &map);
	CHECK_EQ(store_count(cs), 0);
	cs_destroy(cs);
}

/*
 * Random versions derived from random held versions, random drops, the
 * audit after every step, and each held version still reads back whole.
 */
ENGINE_MEMORY_TEST(test_refcounts_random_versions)
{
	enum
	{
		KEYS = 6000, SLOTS = 8, STEPS = 150
	};
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &small_policy);
	PHash		root[SLOTS];
	int64	   *model[SLOTS];
	bool		live[SLOTS] = {false};
	int64	   *got = palloc(KEYS * sizeof(int64));
	EditBatch	b;
	int			created = 0,
				dropped = 0,
				verified = 0;

	rng_seed_global(77);
	batch_init(&b, 400);
	for (int s = 0; s < SLOTS; s++)
		model[s] = palloc(KEYS * sizeof(int64));
	root[0] = held(cs, build_even_rows(&ctx, KEYS / 2));
	for (int k = 0; k < KEYS; k++)
		model[0][k] = k % 2 ? -1 : 0;
	live[0] = true;
	release(cs);

	for (int step = 0; step < STEPS; step++)
	{
		int			from,
					to = (int) rnd_below_global(SLOTS);

		do
			from = (int) rnd_below_global(SLOTS);
		while (!live[from]);
		if (live[to] && to != from && rnd_below_global(4) == 0)
		{
			sever(cs, &root[to]);	/* drop a version */
			live[to] = false;
			dropped++;
		}
		else
		{
			size_t		n = 1 + rnd_below_global(rnd_below_global(5) == 0 ? 400 : 20);
			PHash		next;

			b.n = 0;
			memcpy(got, model[from], KEYS * sizeof(int64));
			for (size_t i = 0; i < n; i++)
			{
				uint64		k = rnd_below_global(KEYS);

				if (rnd_below_global(3) == 0)
				{
					batch_del(&b, k);
					got[k] = -1;
				}
				else
				{
					batch_put(&b, k, (uint64) step);
					got[k] = step;
				}
			}
			next = held(cs, t_apply(&ctx, &root[from], b.e, b.n));
			if (live[to])
				sever(cs, &root[to]);
			root[to] = next;
			memcpy(model[to], got, KEYS * sizeof(int64));
			live[to] = true;
			created++;
		}
		release(cs);
		check_store(cs, 0);
		for (int s = 0; s < SLOTS; s++)
			if (live[s] && rnd_below_global(3) == 0)
			{
				read_rows(&ctx, &root[s], got, KEYS);
				CHECK(memcmp(got, model[s], KEYS * sizeof(int64)) == 0);
				verified++;
			}
	}
	CHECK(created > 80 && dropped > 10 && verified > 100);
	for (int s = 0; s < SLOTS; s++)
	{
		if (live[s])
			sever(cs, &root[s]);
		pfree(model[s]);
	}
	CHECK_EQ(store_count(cs), 0);
	pfree(got);
	batch_free(&b);
	cs_destroy(cs);
}

/*
 * A writer reuses chunks of a tree whose only root goes meanwhile.  The
 * writer's holds, and the references of the nodes it puts, keep them:
 * without them its result would dangle.
 */
ENGINE_MEMORY_TEST(test_reuse_race_holds)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &small_policy);
	PHash		a,
				same,
				b;
	EditBatch	eb;
	PHashMap	only_a;
	int64	   *got = palloc(40000 * sizeof(int64));

	/* identical contents: every chunk the writer puts is there already */
	a = held(cs, build_even_rows(&ctx, 20000));
	release(cs);
	same = build_even_rows(&ctx, 20000);
	CHECK(phash_eq(&a, &same));
	CHECK_EQ(sever(cs, &a), 0);
	CHECK_EQ(read_rows(&ctx, &same, got, 40000), 20000);
	CHECK(release(cs) > 0);
	CHECK_EQ(store_count(cs), 0);

	/* overlapping contents: only a's own chunks go */
	a = held(cs, build_even_rows(&ctx, 20000));
	release(cs);
	batch_init(&eb, 50);
	for (uint64 i = 0; i < 50; i++)
		batch_put(&eb, i * 397 % 40000, 7);
	b = t_apply(&ctx, &a, eb.e, eb.n);
	only_a = chunk_diff_set(&ctx, &a, &b);
	CHECK_EQ(sever(cs, &a), set_size(&only_a));
	CHECK_EQ(read_rows(&ctx, &b, got, 40000), 20000 + 25);
	held(cs, b);
	release(cs);
	check_store(cs, 0);
	sever(cs, &b);
	CHECK_EQ(store_count(cs), 0);

	phmap_free(&only_a);
	batch_free(&eb);
	pfree(got);
	cs_destroy(cs);
}

/* A failed edit leaves nothing behind once its holds end. */
ENGINE_MEMORY_TEST(test_failed_edit_leaves_no_garbage)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &small_policy);
	PHash		t,
				map,
				bogus;
	uint8		val[PHASH_LEN];
	uint8	   *keys = palloc(3000 * 8);
	ProllyEdit *e = palloc(3000 * sizeof(ProllyEdit));
	ApplyArg	aa = {&ctx, &map, e, 3000};
	uint64		before;

	t = held(cs, build_even_rows(&ctx, 100));
	memcpy(val, t.b, PHASH_LEN);
	memset(bogus.b, 0x77, PHASH_LEN);
	CHECK_OK(prolly_empty(&ctx, 1, &map));
	held(cs, map);
	release(cs);
	before = store_count(cs);

	/* many good entries written before the last one names an absent tree */
	for (int i = 0; i < 3000; i++)
	{
		put_u64(keys + i * 8, (uint64) i);
		e[i] = (ProllyEdit) {keys + i * 8, 8, i == 2999 ? bogus.b : val, PHASH_LEN, false};
	}
	expect_error(apply_body, &aa, ERRCODE_FOREIGN_KEY_VIOLATION, &bogus);
	CHECK(store_count(cs) > before);
	release(cs);
	CHECK_EQ(store_count(cs), before);
	check_store(cs, 0);

	sever(cs, &map);
	sever(cs, &t);
	CHECK_EQ(store_count(cs), 0);
	pfree(keys);
	pfree(e);
	cs_destroy(cs);
}

/*
 * Work in flight whose holds end before it is rooted (its transaction
 * aborted, or its server crashed) is what nothing holds: the audit counts
 * it, freeing the unheld reclaims exactly it.
 */
ENGINE_MEMORY_TEST(test_unheld_reclaimed)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &small_policy);
	PHash		kept,
				lost;
	EditBatch	b;
	PHashMap	only_lost;
	uint64		chunks,
				unheld;
	char	   *why = NULL;

	kept = held(cs, build_even_rows(&ctx, 10000));
	release(cs);
	batch_init(&b, 40);
	for (uint64 i = 0; i < 40; i++)
		batch_put(&b, i * 499 % 20000, 1);
	lost = t_apply(&ctx, &kept, b.e, b.n);	/* never rooted: in flight */
	only_lost = chunk_diff_set(&ctx, &lost, &kept);

	MemoryChunkStoreEndHolds(cs);
	CHECK(cs_check(cs, &chunks, &unheld, &why));
	CHECK(unheld > 0);
	CHECK_EQ(cs_free_unheld(cs), set_size(&only_lost));
	check_store(cs, 0);
	CHECK_EQ(t_count(&ctx, &kept), 10000);
	CHECK(!cs_has(cs, lost.b));

	phmap_free(&only_lost);
	batch_free(&b);
	sever(cs, &kept);
	CHECK_EQ(store_count(cs), 0);
	cs_destroy(cs);
}

static void
count_rows_fn(void *arg, const ProllyDiff *d)
{
	(*(uint64 *) arg)++;
}

/* A missing chunk is an error naming it, for every operation that needs it. */
ENGINE_MEMORY_TEST(test_missing_chunk_errors)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &small_policy);
	PHash		a,
				b,
				near,
				leaf,
				out;
	EditBatch	eb;
	ProllyIter *it;
	const uint8 *k,
			   *v;
	size_t		kl,
				vl;
	uint8		key[8];
	int			rc;
	uint64		n = 0;

	a = held(cs, build_even_rows(&ctx, 10000));
	batch_init(&eb, 1);
	batch_put(&eb, 19998, 5);
	b = held(cs, t_apply(&ctx, &a, eb.e, eb.n));
	eb.n = 0;
	batch_put(&eb, 10000, 5);
	near = held(cs, t_apply(&ctx, &a, eb.e, eb.n));
	leaf = leaf_holding(&ctx, &a, 10000);
	release(cs);
	MemoryChunkStoreDrop(cs, leaf.b);	/* damage: a's leaf for 10000 */

	put_u64(key, 10000);
	CHECK_EQ(prolly_get(&ctx, &a, key, 8, &v, &vl), PROLLY_ERR_MISSING);
	CHECK(ctx.err_has_hash && phash_eq(&ctx.err_hash, &leaf));
	put_u64(key, 2);
	CHECK_EQ(prolly_get(&ctx, &a, key, 8, &v, &vl), PROLLY_OK);
	CHECK_EQ(t_count(&ctx, &a), 10000);	/* the root alone answers count */

	CHECK_OK(prolly_iter_open(&ctx, &a, NULL, 0, &it));
	while ((rc = prolly_iter_next(it, &k, &kl, &v, &vl)) == 1)
		n++;
	prolly_iter_close(it);
	CHECK(rc == PROLLY_ERR_MISSING && n > 0 && n < 10000);

	/* a diff that never needs the damaged leaf does not touch it */
	n = 0;
	CHECK_OK(prolly_diff(&ctx, &a, &b, count_rows_fn, &n));
	CHECK_EQ(n, 1);
	CHECK_EQ(prolly_diff(&ctx, &a, &near, count_rows_fn, &n), PROLLY_ERR_MISSING);
	CHECK(phash_eq(&ctx.err_hash, &leaf));
	eb.n = 0;
	batch_put(&eb, 10000, 1);	/* (in the damaged leaf, whatever its bounds) */
	CHECK_EQ(prolly_apply(&ctx, &a, eb.e, eb.n, &out), PROLLY_ERR_MISSING);
	CHECK(phash_eq(&ctx.err_hash, &leaf));
	/* an edit away from it (its own leaf's path) does not touch it */
	eb.n = 0;
	batch_put(&eb, 3, 1);
	CHECK_OK(prolly_apply(&ctx, &a, eb.e, eb.n, &out));

	batch_free(&eb);
	cs_destroy(cs);
}

typedef struct FreeArg
{
	ChunkStore *cs;
} FreeArg;

static void
free_unheld_body(void *arg)
{
	(void) cs_free_unheld(((FreeArg *) arg)->cs);
}

/*
 * Freeing a tree whose store is damaged below it is an error naming the
 * chunk gone, not a crash, nor counts gone wrong: with the chunk put back,
 * the audit passes and the rest frees.
 */
ENGINE_MEMORY_TEST(test_free_over_damage)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &small_policy);
	PHash		a,
				leaf,
				again;
	FreeArg		fa = {cs};
	uint32		len;
	uint8	   *bytes;

	a = build_even_rows(&ctx, 5000);	/* held, not rooted */
	leaf = leaf_holding(&ctx, &a, 4000);
	MemoryChunkStoreEndHolds(cs);
	bytes = cs_get(cs, leaf.b, &len);
	MemoryChunkStoreDrop(cs, leaf.b);	/* damage under a tree to free */
	expect_error(free_unheld_body, &fa, ERRCODE_DATA_CORRUPTED, &leaf);

	/*
	 * The leaf put back, with the one reference to it its drop took (its
	 * parent's, which the freeing stopped at)
	 */
	CHECK(cs_put(cs, bytes, len, again.b));
	CHECK(phash_eq(&again, &leaf));
	MemoryChunkStoreMiscount(leaf.b);
	MemoryChunkStoreEndHolds(cs);
	check_store(cs, store_count(cs));
	CHECK(cs_free_unheld(cs) > 0);
	CHECK_EQ(store_count(cs), 0);
	pfree(bytes);
	cs_destroy(cs);
}

/* ------------------------------------------------------------ chunk diff */

ENGINE_MEMORY_TEST(test_chunk_diff_exact)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &small_policy);
	EditBatch	eb;

	batch_init(&eb, 9000);
	for (int trial = 0; trial < 24; trial++)
	{
		PHash		a,
					b;
		PHashMap	na,
					nb,
					want,
					got;
		uint64		n = trial % 6 == 0 ? 3 : 4000 + 1000 * (uint64) (trial % 5);
		size_t		changes = trial % 4 == 0 ? 1 : trial % 4 == 1 ? 30 : trial % 4 == 2 ? 600 : 9000;

		rng_seed_global(500 + (uint64) trial);
		a = build_even_rows(&ctx, n);
		eb.n = 0;
		for (size_t i = 0; i < changes; i++)
		{
			uint64		k = rnd_below_global(2 * n + 10);

			if (rnd_below_global(3) == 0)
				batch_del(&eb, k);
			else
				batch_put(&eb, k, (uint64) trial);
		}
		b = trial == 7 ? t_empty(&ctx) : trial == 11 ? a : t_apply(&ctx, &a, eb.e, eb.n);
		phmap_init(&na);
		phmap_init(&nb);
		closure(cs, &a, &na);
		closure(cs, &b, &nb);
		for (int dir = 0; dir < 2; dir++)
		{
			const PHash *x = dir ? &b : &a,
					   *y = dir ? &a : &b;

			set_minus(dir ? &nb : &na, dir ? &na : &nb, &want);
			got = chunk_diff_set(&ctx, x, y);
			CHECK(set_equal(&got, &want));
			phmap_free(&want);
			phmap_free(&got);
		}
		phmap_free(&na);
		phmap_free(&nb);
		release(cs);
	}
	batch_free(&eb);
	cs_destroy(cs);
}

/* Severing a while b is held frees exactly chunk_diff(a, b). */
ENGINE_MEMORY_TEST(test_sever_frees_chunk_diff)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &small_policy);
	EditBatch	eb;
	int64	   *got = palloc(30000 * sizeof(int64));

	batch_init(&eb, 3000);
	for (int trial = 0; trial < 10; trial++)
	{
		PHash		a,
					b;
		PHashMap	d;
		uint64		rows;
		size_t		changes = trial < 5 ? 1 + (size_t) trial * 7 : 3000;

		rng_seed_global(900 + (uint64) trial);
		a = held(cs, build_even_rows(&ctx, 12000));
		eb.n = 0;
		for (size_t i = 0; i < changes; i++)
			batch_put(&eb, rnd_below_global(30000), (uint64) trial);
		b = held(cs, t_apply(&ctx, &a, eb.e, eb.n));
		release(cs);
		d = chunk_diff_set(&ctx, &a, &b);
		rows = read_rows(&ctx, &b, got, 30000);
		CHECK_EQ(sever(cs, &a), set_size(&d));
		for (size_t i = 0; i < d.cap; i++)
			if (d.used[i])
				CHECK(!cs_has(cs, d.keys[i].b));
		CHECK_EQ(read_rows(&ctx, &b, got, 30000), rows);
		check_store(cs, 0);
		sever(cs, &b);
		CHECK_EQ(store_count(cs), 0);
		phmap_free(&d);
	}
	pfree(got);
	batch_free(&eb);
	cs_destroy(cs);
}

/* ----------------------------------------------------------------- repair */

ENGINE_MEMORY_TEST(test_repair_restores_severed_tree)
{
	ChunkStore *cs = memstore_create();
	ChunkStore *archive = memstore_create_own();
	ProllyCtx	ctx = t_ctx(cs, &small_policy);
	PHash		a,
				b;
	EditBatch	eb;
	PHashMap	only_a;
	int64	   *want = palloc(20000 * sizeof(int64));
	int64	   *got = palloc(20000 * sizeof(int64));
	ChunkStoreStats st,
				ast;

	a = held(cs, build_even_rows(&ctx, 8000));
	batch_init(&eb, 60);
	rng_seed_global(31);
	for (int i = 0; i < 60; i++)
		batch_put(&eb, rnd_below_global(20000), 3);
	b = held(cs, t_apply(&ctx, &a, eb.e, eb.n));
	release(cs);
	read_rows(&ctx, &a, want, 20000);
	archive_copy(archive, cs, &a);
	only_a = chunk_diff_set(&ctx, &a, &b);

	/* sever a, then bring it back */
	sever(cs, &a);
	CHECK(!cs_has(cs, a.b));
	st = *cs_stats(cs);
	ast = *cs_stats(archive);
	CHECK_EQ(prolly_repair(&ctx, archive, &a), set_size(&only_a));
	/* only what was missing travels */
	CHECK_EQ(STATS_DELTA(cs, st, stored), set_size(&only_a));
	CHECK_EQ(STATS_DELTA(archive, ast, gets), set_size(&only_a));
	held(cs, a);
	release(cs);
	check_store(cs, 0);
	read_rows(&ctx, &a, got, 20000);
	CHECK(memcmp(got, want, 20000 * sizeof(int64)) == 0);

	/* repairing something already whole copies nothing */
	st = *cs_stats(cs);
	CHECK_EQ(prolly_repair(&ctx, archive, &a), 0);
	CHECK_EQ(STATS_DELTA(cs, st, stored), 0);
	release(cs);

	phmap_free(&only_a);
	pfree(want);
	pfree(got);
	batch_free(&eb);
	cs_destroy(cs);
}

typedef struct RepairArg
{
	ProllyCtx  *ctx;
	ChunkStore *source;
	const PHash *root;
} RepairArg;

static void
repair_body(void *arg)
{
	RepairArg  *r = arg;

	(void) prolly_repair(r->ctx, r->source, r->root);
}

/* A source that lies or is incomplete cannot get a partial tree accepted. */
ENGINE_MEMORY_TEST(test_repair_rejects_bad_source)
{
	ChunkStore *cs = memstore_create();
	ChunkStore *archive = memstore_create_own();
	ProllyCtx	ctx = t_ctx(cs, &small_policy);
	PHash		a,
				b,
				victim = {{0}};
	EditBatch	eb;
	PHashMap	only_a;
	RepairArg	ra = {&ctx, archive, &a};
	ChunkStoreStats st;
	uint64		before;
	uint32		len;
	uint8	   *bytes;

	a = held(cs, build_even_rows(&ctx, 8000));
	batch_init(&eb, 30);
	for (uint64 i = 0; i < 30; i++)
		batch_put(&eb, i * 541 % 16000, 9);
	b = held(cs, t_apply(&ctx, &a, eb.e, eb.n));
	release(cs);
	archive_copy(archive, cs, &a);
	only_a = chunk_diff_set(&ctx, &a, &b);
	for (size_t i = 0; i < only_a.cap; i++)
		if (only_a.used[i] && level_of(cs, &only_a.keys[i]) == 0)
			victim = only_a.keys[i];
	sever(cs, &a);
	before = store_count(cs);

	/*
	 * a flipped byte in the source is caught by its name (the copy is one
	 * batch, refused whole: nothing of it stored)
	 */
	bytes = cs_get(archive, victim.b, &len);	/* (for its length) */
	pfree(bytes);
	st = *cs_stats(cs);
	MemoryChunkStoreDamage(archive, victim.b, len - 1);
	expect_error(repair_body, &ra, ERRCODE_DATA_CORRUPTED, &victim);
	CHECK_EQ(STATS_DELTA(cs, st, stored), 0);
	CHECK(!cs_has(cs, a.b));
	release(cs);
	CHECK_EQ(store_count(cs), before);
	check_store(cs, 0);
	MemoryChunkStoreDamage(archive, victim.b, len - 1);

	/* a chunk the source lacks is named */
	st = *cs_stats(cs);
	MemoryChunkStoreDrop(archive, victim.b);
	expect_error(repair_body, &ra, ERRCODE_UNDEFINED_OBJECT, &victim);
	CHECK_EQ(STATS_DELTA(cs, st, stored), 0);
	CHECK(!cs_has(cs, a.b));
	release(cs);
	CHECK_EQ(store_count(cs), before);
	check_store(cs, 0);

	phmap_free(&only_a);
	batch_free(&eb);
	cs_destroy(cs);
}

/* --------------------------------------------------------- reverse deltas */

/*
 * Keep only the newest version and a reverse delta per step; every older
 * version rebuilds to its exact original hash.
 */
ENGINE_MEMORY_TEST(test_rebuild_from_reverse_deltas)
{
	enum
	{
		VERSIONS = 30, ROWS = 20000
	};
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &small_policy);
	PHash		v[VERSIONS],
				delta[VERSIONS],
				cur,
				forged,
				out;
	EditBatch	eb;
	uint64		tree_bytes,
				delta_bytes = 0,
				cur_bytes;
	uint32		len;
	uint8	   *copy;

	rng_seed_global(4242);
	batch_init(&eb, 40);
	v[0] = held(cs, build_even_rows(&ctx, ROWS));
	for (int i = 1; i < VERSIONS; i++)
	{
		eb.n = 0;
		for (int j = 0; j < 40; j++)
		{
			uint64		k = rnd_below_global(2 * ROWS);

			if (j % 5 == 0)
				batch_del(&eb, k);
			else
				batch_put(&eb, k, (uint64) i);
		}
		v[i] = held(cs, t_apply(&ctx, &v[i - 1], eb.e, eb.n));
	}
	for (int i = 0; i + 1 < VERSIONS; i++)
	{
		CHECK_OK(prolly_delta_write(&ctx, &v[i + 1], &v[i], &delta[i]));
		held(cs, delta[i]);
		pfree(cs_get(cs, delta[i].b, &len));
		delta_bytes += len;
	}
	release(cs);
	tree_bytes = store_bytes(cs);

	/* drop every tree but the newest */
	for (int i = 0; i + 1 < VERSIONS; i++)
		sever(cs, &v[i]);
	check_store(cs, 0);
	cur_bytes = store_bytes(cs);
	CHECK(!cs_has(cs, v[0].b));
	/* deltas are a small fraction of the trees they replace */
	CHECK_LE(delta_bytes * 4, tree_bytes - cur_bytes);

	/* walk back, newest to oldest, rebuilding each exactly */
	cur = v[VERSIONS - 1];
	for (int i = VERSIONS - 2; i >= 0; i--)
	{
		PHash		prev;

		CHECK_OK(prolly_delta_apply(&ctx, &delta[i], &prev));
		CHECK(phash_eq(&prev, &v[i]));
		cur = prev;
	}
	CHECK_EQ(t_count(&ctx, &cur), ROWS);
	release(cs);
	check_store(cs, 0);

	/* a delta whose base is gone is a named error */
	CHECK_EQ(prolly_delta_apply(&ctx, &delta[0], &out), PROLLY_ERR_MISSING);
	CHECK(phash_eq(&ctx.err_hash, &v[1]));

	/* a delta that does not reproduce its target is no delta of its trees */
	copy = cs_get(cs, delta[VERSIONS - 2].b, &len);
	copy[2 + PHASH_LEN] ^= 1;	/* the recorded target */
	CHECK_OK(prolly_put_chunk(&ctx, copy, len, &forged));
	CHECK_EQ(prolly_delta_apply(&ctx, &forged, &out), PROLLY_ERR_BAD_DELTA);
	CHECK(phash_eq(&ctx.err_hash, &forged));
	pfree(copy);
	release(cs);

	batch_free(&eb);
	cs_destroy(cs);
}

/* ---------------------------------------------------------------- scaling */

#define LT_K 100
/* entries a small_policy node can hold: max_bytes over the smallest item */
#define SMALL_MAX_FANOUT (1024 / 30 + 1)

typedef struct LifeScale
{
	uint64		n;
	int			height;
	uint64		chunk_diff_reads;
	uint64		sever_frees;
	uint64		sever_reads;
	uint64		repair_copied;
	uint64		repair_src_reads;
	uint64		repair_holds;
	uint64		delta_bytes;
	uint64		delta_apply_reads;
	uint64		delta_apply_writes;
	uint64		hold_frees;		/* frees when a tree another shares is dropped */
} LifeScale;

static void
life_scale_case(uint64 n, LifeScale *s)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &small_policy);
	PHash		a,
				b,
				delta,
				back,
				same;
	EditBatch	eb;
	PHashMap	d;
	ChunkStoreStats st,
				ast;
	uint32		len;

	memset(s, 0, sizeof(*s));
	s->n = n;
	rng_seed_global(n * 3);

	/*
	 * Small chunks keep both sizes unsaturated: with large chunks, 100
	 * edits touch nearly every internal node at 100k rows, which hides how
	 * cost grows.
	 */
	a = held(cs, build_even_rows(&ctx, n));
	s->height = t_height(&ctx, &a);
	batch_init(&eb, LT_K);
	for (int i = 0; i < LT_K; i++)
		batch_put(&eb, rnd_below_global(2 * n), 1);
	b = held(cs, t_apply(&ctx, &a, eb.e, eb.n));
	release(cs);

	st = *cs_stats(cs);
	d = chunk_diff_set(&ctx, &a, &b);
	s->chunk_diff_reads = STATS_DELTA(cs, st, gets);

	CHECK_OK(prolly_delta_write(&ctx, &b, &a, &delta));
	held(cs, delta);
	release(cs);
	pfree(cs_get(cs, delta.b, &len));
	s->delta_bytes = len;

	/*
	 * Severing a, repairing it and dropping it again, on a memory store: what
	 * is freed, and copied back, freed within the test (on the history
	 * store, nothing is freed until the transaction ends, so a is still
	 * there and the delta is replayed over it)
	 */
	if (cs->routine == &MemoryChunkStore)
	{
		ChunkStore *archive = memstore_create_own();

		archive_copy(archive, cs, &a);
		st = *cs_stats(cs);
		s->sever_frees = sever(cs, &a);
		s->sever_reads = STATS_DELTA(cs, st, gets);
		CHECK_EQ(s->sever_frees, set_size(&d));

		st = *cs_stats(cs);
		ast = *cs_stats(archive);
		s->repair_copied = prolly_repair(&ctx, archive, &a);
		s->repair_holds = STATS_DELTA(cs, st, holds);
		s->repair_src_reads = STATS_DELTA(archive, ast, gets);
		CHECK_EQ(s->repair_copied, set_size(&d));
		CHECK_EQ(STATS_DELTA(cs, st, stored), s->repair_copied);
		release(cs);			/* not rooted: repair's copy goes again */
		CHECK(!cs_has(cs, a.b));
	}

	st = *cs_stats(cs);
	CHECK_OK(prolly_delta_apply(&ctx, &delta, &back));
	s->delta_apply_reads = STATS_DELTA(cs, st, gets);
	s->delta_apply_writes = STATS_DELTA(cs, st, puts);	/* (found there or not) */
	CHECK(phash_eq(&back, &a));
	held(cs, back);
	release(cs);

	/* dropping a tree that shares everything with a rooted one frees nothing */
	same = held(cs, t_apply(&ctx, &b, eb.e, 0));
	release(cs);
	if (cs->routine == &MemoryChunkStore)
		s->hold_frees = sever(cs, &same);

	phmap_free(&d);
	batch_free(&eb);
	cs_destroy(cs);
}

/*
 * What each costs, at two sizes a tree's height apart: chunk diffs,
 * severing, repair and deltas follow the edits and the height, not the
 * rows.  (Severing and repair, what a memory store frees within a test, on
 * the memory store alone; the rest on both.)
 */
ENGINE_TEST(test_lifetime_scaling)
{
	static const uint64 sizes[] = {20000, 200000};
	LifeScale	s[2];
	bool		memory = engine_test_on_memory();

	for (int i = 0; i < 2; i++)
	{
		LifeScale  *x = &s[i];
		uint64		h;

		life_scale_case(sizes[i], x);
		h = (uint64) x->height;

		/* chunk diff: O(d h), like row diff */
		CHECK_LE(x->chunk_diff_reads, 4 * LT_K * h + 2);
		/* delta: O(d) bytes; replay costs the same as the edits it records */
		CHECK_LE(x->delta_bytes, LT_K * 40 + 64);
		CHECK_LE(x->delta_apply_reads, 4 * LT_K * h + 2);
		CHECK_LE(x->delta_apply_writes, 3 * LT_K * h);
		if (!memory)
			continue;
		/* sever: O(d h) chunks freed, with no chunk read through the store */
		CHECK_LE(x->sever_frees, 3 * LT_K * h);
		CHECK_EQ(x->sever_reads, 0);
		/* repair: exactly the missing chunks read and written */
		CHECK_EQ(x->repair_src_reads, x->repair_copied);
		CHECK_LE(x->repair_holds, x->repair_copied * SMALL_MAX_FANOUT + 1);
		CHECK_EQ(x->hold_frees, 0);
	}
	/* the sizes a level apart */
	CHECK_LT(s[0].height, s[1].height);
	/* 10x the rows: costs follow height, not n */
	CHECK_LE(s[1].chunk_diff_reads, 2 * s[0].chunk_diff_reads * s[1].height / s[0].height);
	CHECK_LE(s[1].delta_bytes, 2 * s[0].delta_bytes);
	CHECK_LE(s[1].delta_apply_reads, 2 * s[0].delta_apply_reads * s[1].height / s[0].height);
	/* (the chunk diffs and deltas measured: on every store) */
	CHECK(s[0].chunk_diff_reads > 0 && s[0].delta_apply_reads > 0 && s[0].delta_apply_writes > 0);
	if (memory)
	{
		CHECK_LE(s[1].sever_frees, 2 * s[0].sever_frees * s[1].height / s[0].height);
		CHECK_LE(s[1].repair_copied, 2 * s[0].repair_copied * s[1].height / s[0].height);
		CHECK_LE(s[1].repair_holds, 2 * s[0].repair_holds * s[1].height / s[0].height);
		CHECK(s[0].sever_frees > 0 && s[0].repair_copied > 0);
	}
}

static const ProllyUnitTest tests[] = {
	{"edits_keep_tree_policy", test_edits_keep_tree_policy},
	{"bad_chunks_rejected", test_bad_chunks_rejected},
	{"child_level_mismatch_is_corrupt", test_child_level_mismatch_is_corrupt},
	{"value_refs_hold_trees", test_value_refs_hold_trees},
	{"refcounts_random_versions", test_refcounts_random_versions},
	{"reuse_race_holds", test_reuse_race_holds},
	{"failed_edit_leaves_no_garbage", test_failed_edit_leaves_no_garbage},
	{"unheld_reclaimed", test_unheld_reclaimed},
	{"missing_chunk_errors", test_missing_chunk_errors},
	{"free_over_damage", test_free_over_damage},
	{"chunk_diff_exact", test_chunk_diff_exact},
	{"sever_frees_chunk_diff", test_sever_frees_chunk_diff},
	{"repair_restores_severed_tree", test_repair_restores_severed_tree},
	{"repair_rejects_bad_source", test_repair_rejects_bad_source},
	{"rebuild_from_reverse_deltas", test_rebuild_from_reverse_deltas},
	{"lifetime_scaling", test_lifetime_scaling},
};

const ProllyUnitSuite unit_engine_lifetime_suite = {"engine_lifetime", tests, lengthof(tests)};
