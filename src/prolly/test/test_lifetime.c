/*
 * test_lifetime.c
 *	  Severing and repairing trees: self-describing chunks, reference
 *	  counts, holds and the reuse race, freeing, orphan collection, error
 *	  reporting, exact chunk diffs, verified repair, rebuild from reverse
 *	  deltas, and the cost of each.
 */
#include "test_util.h"

/* ---------------------------------------------------------------- helpers */

typedef struct NodeWalk
{
	ChunkStore *cs;
	PHashMap   *set;
} NodeWalk;

static void node_walk_fn(void *arg, const PHash *h, bool strong);

/* Every chunk in the strong closure of h. */
static void
closure(ChunkStore *cs, const PHash *h, PHashMap *set)
{
	NodeWalk	w = {cs, set};
	bool		created;
	size_t		len;
	const uint8_t *d;

	phmap_ref(set, h, &created);
	if (!created)
		return;
	d = cs->ops->get(cs, h, &len);
	CHECK(d != NULL);
	CHECK(chunk_refs(d, len, node_walk_fn, &w));
}

static void
node_walk_fn(void *arg, const PHash *h, bool strong)
{
	NodeWalk   *w = arg;

	if (strong)
		closure(w->cs, h, w->set);
}

static void
set_add_fn(void *arg, const PHash *h)
{
	bool		created;

	phmap_ref(arg, h, &created);
	CHECK(created);				/* chunk diff reports each node once */
}

static size_t
set_size(const PHashMap *m)
{
	return m->n;
}

/* a minus b, as a set. */
static void
set_minus(const PHashMap *a, const PHashMap *b, PHashMap *out)
{
	bool		created;

	phmap_init(out);
	for (size_t i = 0; i < a->cap; i++)
		if (a->used[i] && !phmap_find((PHashMap *) b, &a->keys[i]))
			phmap_ref(out, &a->keys[i], &created);
}

static bool
set_equal(const PHashMap *a, const PHashMap *b)
{
	if (a->n != b->n)
		return false;
	for (size_t i = 0; i < a->cap; i++)
		if (a->used[i] && !phmap_find((PHashMap *) b, &a->keys[i]))
			return false;
	return true;
}

static PHashMap
chunk_diff_set(ProllyCtx *ctx, const PHash *a, const PHash *b)
{
	PHashMap	d;

	phmap_init(&d);
	CHECK_OK(prolly_chunk_diff(ctx, a, b, set_add_fn, &d));
	return d;
}

/* Rows of a tree as a key -> version array; returns the row count. */
static uint64_t
read_rows(ProllyCtx *ctx, const PHash *root, int64_t *ver, uint64_t keyspace)
{
	ProllyIter *it = t_iter(ctx, root, NULL, 0);
	const uint8_t *k,
			   *v;
	size_t		kl,
				vl;
	uint64_t	n = 0;

	for (uint64_t i = 0; i < keyspace; i++)
		ver[i] = -1;
	while (t_next(it, &k, &kl, &v, &vl))
	{
		uint64_t	key = get_u64(k);

		CHECK(key < keyspace);
		ver[key] = (int64_t) get_u64(v + 8);
		n++;
	}
	prolly_iter_close(it);
	return n;
}

static PHash
held(ProllyCtx *ctx, PHash h)
{
	cs_ref_root(ctx->cs, &h);
	return h;
}

static uint64_t
level_of(ChunkStore *cs, const PHash *h)
{
	size_t		len;
	const uint8_t *d = cs->ops->get(cs, h, &len);
	NodeHeader	hdr;

	CHECK(d && node_header_parse(d, len, &hdr));
	return (uint64_t) hdr.level;
}

/* The leaf of root that holds key (found as the level-0 chunk an update rewrites). */
static PHash
leaf_holding(ProllyCtx *ctx, const PHash *root, uint64_t key)
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
	CHECK(found == 1);
	phmap_free(&d);
	batch_free(&b);
	return leaf;
}

/* ------------------------------------------------------------- format */

static void
test_edits_keep_tree_policy(void)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	small,
				dflt;
	PHash		a,
				edited,
				want_small,
				want_dflt;
	EditBatch	b;

	prolly_ctx_init(&small, cs, &small_policy);
	prolly_ctx_init(&dflt, cs, &prolly_default_policy);
	a = build_even_rows(&small, 5000);
	batch_init(&b, 3);
	batch_put(&b, 1, 1);
	batch_put(&b, 5001, 1);
	batch_del(&b, 4000);
	/* edited through a context whose policy differs: the tree's own wins */
	edited = t_apply(&dflt, &a, b.e, b.n);
	want_small = t_apply(&small, &a, b.e, b.n);
	CHECK(phash_eq(&edited, &want_small));
	{
		PHash		d = build_even_rows(&dflt, 5000);

		want_dflt = t_apply(&dflt, &d, b.e, b.n);
		CHECK(!phash_eq(&edited, &want_dflt));
	}
	batch_free(&b);
	prolly_ctx_free(&small);
	prolly_ctx_free(&dflt);
	cs_destroy(cs);
}

static void
test_bad_chunks_rejected(void)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx;
	Buf			b = BUF_INIT;
	PHash		h,
				bad,
				garbage,
				root;
	ChunkPolicy broken = {4096, 4096, 8192};	/* target must exceed min */
	static const uint8_t junk[] = "not a chunk";
	uint8_t		key[8];
	const uint8_t *v;
	size_t		vl;

	prolly_ctx_init(&ctx, cs, &small_policy);

	/* unparsable bytes and invalid policies never enter the store */
	CHECK(cs_put(cs, junk, sizeof(junk), &h, &bad) == CS_ERR_CORRUPT);
	node_header_write(&b, &broken, 0, 0, 0);
	CHECK(cs_put(cs, b.data, b.len, &h, &bad) == CS_ERR_CORRUPT);
	CHECK(cs->ops->count(cs) == 0);

	/* a strong reference to an absent chunk is refused, naming it */
	memset(garbage.b, 0xab, PHASH_LEN);
	b.len = 0;
	node_header_write(&b, &small_policy, 1, 0, 1);
	buf_append_uvarint(&b, 1);
	buf_append_byte(&b, 'k');
	buf_append_uvarint(&b, PHASH_LEN + 1);
	buf_append(&b, garbage.b, PHASH_LEN);
	buf_append_byte(&b, 1);
	CHECK(cs_put(cs, b.data, b.len, &h, &bad) == CS_ERR_MISSING);
	CHECK(phash_eq(&bad, &garbage));

	/* damage below a valid parent surfaces as CORRUPT at the damaged chunk */
	cs->ops->put(cs, &garbage, junk, sizeof(junk));	/* bypasses validation */
	CHECK(cs_put(cs, b.data, b.len, &root, &bad) == CS_OK);
	put_u64(key, 0);
	CHECK(prolly_get(&ctx, &root, (const uint8_t *) "k", 1, &v, &vl) == PROLLY_ERR_CORRUPT);
	CHECK(phash_eq(&ctx.err_hash, &garbage));
	buf_free(&b);
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
}

static void
test_child_level_mismatch_is_corrupt(void)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx;
	PHash		leaf,
				root,
				bad;
	Buf			b = BUF_INIT;
	uint8_t		val[PHASH_LEN + 1];
	const uint8_t *v;
	size_t		vl;

	prolly_ctx_init(&ctx, cs, &small_policy);
	leaf = build_even_rows(&ctx, 3);
	/* a level-2 node pointing straight at a leaf skips a level */
	memcpy(val, leaf.b, PHASH_LEN);
	val[PHASH_LEN] = 3;
	node_header_write(&b, &small_policy, 2, 0, 1);
	buf_append_uvarint(&b, 8);
	{
		uint8_t		k[8];

		put_u64(k, 4);
		buf_append(&b, k, 8);
	}
	buf_append_uvarint(&b, sizeof(val));
	buf_append(&b, val, sizeof(val));
	CHECK(cs_put(cs, b.data, b.len, &root, &bad) == CS_OK);
	{
		uint8_t		k[8];

		put_u64(k, 2);
		CHECK(prolly_get(&ctx, &root, k, 8, &v, &vl) == PROLLY_ERR_CORRUPT);
	}
	buf_free(&b);
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
}

/* --------------------------------------------------- holds and freeing */

/* A map whose values hold other trees: the trees live as long as the map. */
static void
test_value_refs_hold_trees(void)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx;
	PHash		tables[3],
				map,
				missing;
	uint8_t		vals[3][PHASH_LEN + 4];
	uint8_t		keys[3][8];
	ProllyKV	kvs[3];
	ProllyEdit	e;

	prolly_ctx_init(&ctx, cs, &small_policy);
	for (int i = 0; i < 3; i++)
	{
		tables[i] = build_even_rows(&ctx, 2000 + 1000 * (uint64_t) i);
		memcpy(vals[i], tables[i].b, PHASH_LEN);
		memcpy(vals[i] + PHASH_LEN, "meta", 4);
		put_u64(keys[i], (uint64_t) i);
		kvs[i] = (ProllyKV) {keys[i], 8, vals[i], sizeof(vals[i])};
	}
	CHECK_OK(prolly_build(&ctx, 1, kvs, 3, &map));
	held(&ctx, map);
	prolly_release_pins(&ctx);
	check_store(cs, 0);
	for (int i = 0; i < 3; i++)
		CHECK(t_count(&ctx, &tables[i]) == 2000 + 1000 * (uint64_t) i);

	/* dropping one entry frees exactly what only the old map reached */
	{
		PHash		smaller;
		PHashMap	before,
					after,
					gone;
		uint64_t	f0;

		e = (ProllyEdit) {keys[1], 8, NULL, 0, true};
		CHECK_OK(prolly_apply(&ctx, &map, &e, 1, &smaller));
		held(&ctx, smaller);
		prolly_release_pins(&ctx);
		phmap_init(&before);
		phmap_init(&after);
		closure(cs, &map, &before);
		closure(cs, &smaller, &after);
		set_minus(&before, &after, &gone);
		CHECK(phmap_find(&gone, &tables[1]) != NULL);
		f0 = cs->stats.frees;
		cs_unref_root(cs, &map);
		CHECK_EQ(cs->stats.frees - f0, set_size(&gone));
		CHECK(!cs->ops->has(cs, &tables[1]));
		check_store(cs, 0);
		map = smaller;
		phmap_free(&before);
		phmap_free(&after);
		phmap_free(&gone);
	}

	/* a value naming an absent tree is refused */
	memset(missing.b, 0x5a, PHASH_LEN);
	memcpy(vals[0], missing.b, PHASH_LEN);
	e = (ProllyEdit) {keys[0], 8, vals[0], sizeof(vals[0]), false};
	{
		PHash		out;

		CHECK(prolly_apply(&ctx, &map, &e, 1, &out) == PROLLY_ERR_MISSING);
		CHECK(phash_eq(&ctx.err_hash, &missing));
	}
	/* a value too short to hold its references is refused */
	e = (ProllyEdit) {keys[0], 8, vals[0], 5, false};
	{
		PHash		out;

		CHECK(prolly_apply(&ctx, &map, &e, 1, &out) == PROLLY_ERR_CORRUPT);
	}
	prolly_release_pins(&ctx);
	check_store(cs, 0);

	/* dropping the map frees everything */
	cs_unref_root(cs, &map);
	CHECK_EQ(cs->ops->count(cs), 0);
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
}

/*
 * Random versions derived from random held versions, random drops, the
 * audit after every step, and each held version still reads back whole.
 */
static void
test_refcounts_random_versions(void)
{
	enum
	{
		KEYS = 6000, SLOTS = 8, STEPS = 150
	};
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx;
	PHash		root[SLOTS];
	int64_t    *model[SLOTS];
	bool		live[SLOTS] = {false};
	int64_t    *got = px_malloc(KEYS * sizeof(int64_t));
	EditBatch	b;
	int			created = 0,
				dropped = 0,
				verified = 0;

	rng_seed(77);
	prolly_ctx_init(&ctx, cs, &small_policy);
	batch_init(&b, 400);
	for (int s = 0; s < SLOTS; s++)
		model[s] = px_malloc(KEYS * sizeof(int64_t));
	root[0] = held(&ctx, build_even_rows(&ctx, KEYS / 2));
	for (int k = 0; k < KEYS; k++)
		model[0][k] = k % 2 ? -1 : 0;
	live[0] = true;
	prolly_release_pins(&ctx);

	for (int step = 0; step < STEPS; step++)
	{
		int			from,
					to = (int) rnd_below(SLOTS);

		do
			from = (int) rnd_below(SLOTS);
		while (!live[from]);
		if (live[to] && to != from && rnd_below(4) == 0)
		{
			cs_unref_root(cs, &root[to]);	/* drop a version */
			live[to] = false;
			dropped++;
		}
		else
		{
			size_t		n = 1 + rnd_below(rnd_below(5) == 0 ? 400 : 20);
			PHash		next;

			b.n = 0;
			memcpy(got, model[from], KEYS * sizeof(int64_t));
			for (size_t i = 0; i < n; i++)
			{
				uint64_t	k = rnd_below(KEYS);

				if (rnd_below(3) == 0)
				{
					batch_del(&b, k);
					got[k] = -1;
				}
				else
				{
					batch_put(&b, k, (uint64_t) step);
					got[k] = step;
				}
			}
			next = held(&ctx, t_apply(&ctx, &root[from], b.e, b.n));
			if (live[to])
				cs_unref_root(cs, &root[to]);
			root[to] = next;
			memcpy(model[to], got, KEYS * sizeof(int64_t));
			live[to] = true;
			created++;
		}
		prolly_release_pins(&ctx);
		check_store(cs, 0);
		for (int s = 0; s < SLOTS; s++)
			if (live[s] && rnd_below(3) == 0)
			{
				read_rows(&ctx, &root[s], got, KEYS);
				CHECK(memcmp(got, model[s], KEYS * sizeof(int64_t)) == 0);
				verified++;
			}
	}
	CHECK(created > 80 && dropped > 10 && verified > 100);
	for (int s = 0; s < SLOTS; s++)
	{
		if (live[s])
			cs_unref_root(cs, &root[s]);
		free(model[s]);
	}
	CHECK_EQ(cs->ops->count(cs), 0);
	free(got);
	batch_free(&b);
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
}

/*
 * A writer reuses chunks of a tree whose only hold is about to go.  The
 * writer's pins must keep them: without them its result would dangle.
 */
static void
test_reuse_race_pins(void)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	w1,
				w2;
	PHash		a,
				same,
				b;
	EditBatch	eb;
	PHashMap	only_a;
	uint64_t	f0;
	int64_t    *got = px_malloc(40000 * sizeof(int64_t));

	prolly_ctx_init(&w1, cs, &small_policy);
	prolly_ctx_init(&w2, cs, &small_policy);

	/* identical contents: every chunk w2 writes already exists */
	a = build_even_rows(&w1, 20000);
	same = build_even_rows(&w2, 20000);
	CHECK(phash_eq(&a, &same));
	f0 = cs->stats.frees;
	prolly_release_pins(&w1);
	CHECK_EQ(cs->stats.frees - f0, 0);
	CHECK_EQ(read_rows(&w2, &same, got, 40000), 20000);
	prolly_release_pins(&w2);
	CHECK_EQ(cs->ops->count(cs), 0);

	/* overlapping contents: only a's own chunks go */
	a = build_even_rows(&w1, 20000);
	batch_init(&eb, 50);
	for (uint64_t i = 0; i < 50; i++)
		batch_put(&eb, i * 397 % 40000, 7);
	b = t_apply(&w2, &a, eb.e, eb.n);
	only_a = chunk_diff_set(&w2, &a, &b);
	f0 = cs->stats.frees;
	prolly_release_pins(&w1);
	CHECK_EQ(cs->stats.frees - f0, set_size(&only_a));
	CHECK_EQ(read_rows(&w2, &b, got, 40000), 20000 + 25);
	held(&w2, b);
	prolly_release_pins(&w2);
	check_store(cs, 0);
	cs_unref_root(cs, &b);
	CHECK_EQ(cs->ops->count(cs), 0);

	phmap_free(&only_a);
	batch_free(&eb);
	free(got);
	prolly_ctx_free(&w1);
	prolly_ctx_free(&w2);
	cs_destroy(cs);
}

/* A failed edit leaves nothing behind once its pins go. */
static void
test_failed_edit_leaves_no_garbage(void)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx;
	PHash		t,
				map,
				out;
	uint8_t		val[PHASH_LEN],
				bogus[PHASH_LEN];
	uint8_t    *keys = px_malloc(3000 * 8);
	ProllyEdit *e = px_malloc(3000 * sizeof(ProllyEdit));
	size_t		before;

	prolly_ctx_init(&ctx, cs, &small_policy);
	t = held(&ctx, build_even_rows(&ctx, 100));
	memcpy(val, t.b, PHASH_LEN);
	memset(bogus, 0x77, PHASH_LEN);
	CHECK_OK(prolly_empty(&ctx, 1, &map));
	held(&ctx, map);
	prolly_release_pins(&ctx);
	before = cs->ops->count(cs);

	/* many good entries written before the last one names an absent tree */
	for (int i = 0; i < 3000; i++)
	{
		put_u64(keys + i * 8, (uint64_t) i);
		e[i] = (ProllyEdit) {keys + i * 8, 8, i == 2999 ? bogus : val, PHASH_LEN, false};
	}
	CHECK(prolly_apply(&ctx, &map, e, 3000, &out) == PROLLY_ERR_MISSING);
	CHECK(memcmp(ctx.err_hash.b, bogus, PHASH_LEN) == 0);
	CHECK(phash_eq(&out, &map));
	CHECK(cs->ops->count(cs) > before);
	prolly_release_pins(&ctx);
	CHECK_EQ(cs->ops->count(cs), before);
	check_store(cs, 0);

	cs_unref_root(cs, &map);
	cs_unref_root(cs, &t);
	CHECK_EQ(cs->ops->count(cs), 0);
	free(keys);
	free(e);
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
}

static void
clear_pins_fn(void *arg, const PHash *h)
{
	ChunkStore *cs = arg;

	cs->ops->meta(cs, h)->pins = 0;
}

/* Pins do not survive a crash; the orphan sweep reclaims their chunks. */
static void
test_orphan_sweep_after_crash(void)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx;
	PHash		kept,
				lost;
	EditBatch	b;
	PHashMap	only_lost;
	size_t		orphans;
	char		why[200];

	prolly_ctx_init(&ctx, cs, &small_policy);
	kept = held(&ctx, build_even_rows(&ctx, 10000));
	prolly_release_pins(&ctx);
	batch_init(&b, 40);
	for (uint64_t i = 0; i < 40; i++)
		batch_put(&b, i * 499 % 20000, 1);
	lost = t_apply(&ctx, &kept, b.e, b.n);	/* never held: work in flight */
	only_lost = chunk_diff_set(&ctx, &lost, &kept);

	/* crash: transient pins are gone */
	cs->ops->foreach(cs, clear_pins_fn, cs);
	ctx.pins.n = 0;
	CHECK(cs_check_invariants(cs, &orphans, why, sizeof(why)));
	CHECK(orphans > 0);
	CHECK_EQ(cs_collect_orphans(cs), set_size(&only_lost));
	check_store(cs, 0);
	CHECK_EQ(t_count(&ctx, &kept), 10000);
	CHECK(!cs->ops->has(cs, &lost));

	phmap_free(&only_lost);
	batch_free(&b);
	cs_unref_root(cs, &kept);
	CHECK_EQ(cs->ops->count(cs), 0);
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
}

static void
count_rows_fn(void *arg, const ProllyDiff *d)
{
	(*(uint64_t *) arg)++;
}

/* A missing chunk is an error naming it, for every operation that needs it. */
static void
test_missing_chunk_errors(void)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx;
	PHash		a,
				b,
				near,
				leaf,
				out;
	EditBatch	eb;
	ProllyIter *it;
	const uint8_t *k,
			   *v;
	size_t		kl,
				vl;
	uint8_t		key[8];
	int			rc;
	uint64_t	n = 0;

	prolly_ctx_init(&ctx, cs, &small_policy);
	a = held(&ctx, build_even_rows(&ctx, 10000));
	batch_init(&eb, 1);
	batch_put(&eb, 19998, 5);
	b = held(&ctx, t_apply(&ctx, &a, eb.e, eb.n));
	eb.n = 0;
	batch_put(&eb, 10000, 5);
	near = held(&ctx, t_apply(&ctx, &a, eb.e, eb.n));
	leaf = leaf_holding(&ctx, &a, 10000);
	prolly_release_pins(&ctx);
	cs->ops->del(cs, &leaf);	/* damage: a's leaf for key 10000 is gone */

	put_u64(key, 10000);
	CHECK(prolly_get(&ctx, &a, key, 8, &v, &vl) == PROLLY_ERR_MISSING);
	CHECK(phash_eq(&ctx.err_hash, &leaf));
	put_u64(key, 2);
	CHECK(prolly_get(&ctx, &a, key, 8, &v, &vl) == PROLLY_OK);
	CHECK(t_count(&ctx, &a) == 10000);	/* the root alone answers count */

	CHECK_OK(prolly_iter_open(&ctx, &a, NULL, 0, &it));
	while ((rc = prolly_iter_next(it, &k, &kl, &v, &vl)) == 1)
		n++;
	prolly_iter_close(it);
	CHECK(rc == PROLLY_ERR_MISSING && n > 0 && n < 10000);

	/* a diff that never needs the damaged leaf does not touch it */
	n = 0;
	CHECK_OK(prolly_diff(&ctx, &a, &b, count_rows_fn, &n));
	CHECK_EQ(n, 1);
	CHECK(prolly_diff(&ctx, &a, &near, count_rows_fn, &n) == PROLLY_ERR_MISSING);
	CHECK(phash_eq(&ctx.err_hash, &leaf));
	eb.n = 0;
	batch_put(&eb, 10001, 1);
	CHECK(prolly_apply(&ctx, &a, eb.e, eb.n, &out) == PROLLY_ERR_MISSING);
	CHECK(phash_eq(&out, &a));
	eb.n = 0;
	batch_put(&eb, 3, 1);
	CHECK_OK(prolly_apply(&ctx, &a, eb.e, eb.n, &out));

	batch_free(&eb);
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
}

/* ------------------------------------------------------------ chunk diff */

static void
test_chunk_diff_exact(void)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx;
	EditBatch	eb;

	prolly_ctx_init(&ctx, cs, &small_policy);
	batch_init(&eb, 20000);
	for (int trial = 0; trial < 24; trial++)
	{
		PHash		a,
					b;
		PHashMap	na,
					nb,
					want,
					got;
		uint64_t	n = trial % 6 == 0 ? 3 : 4000 + 1000 * (uint64_t) (trial % 5);
		size_t		changes = trial % 4 == 0 ? 1 : trial % 4 == 1 ? 30 : trial % 4 == 2 ? 600 : 9000;

		rng_seed(500 + (uint64_t) trial);
		a = build_even_rows(&ctx, n);
		eb.n = 0;
		for (size_t i = 0; i < changes; i++)
		{
			uint64_t	k = rnd_below(2 * n + 10);

			if (rnd_below(3) == 0)
				batch_del(&eb, k);
			else
				batch_put(&eb, k, (uint64_t) trial);
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
		prolly_release_pins(&ctx);
	}
	batch_free(&eb);
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
}

/* Severing a while b is held frees exactly chunk_diff(a, b). */
static void
test_sever_frees_chunk_diff(void)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx;
	EditBatch	eb;
	int64_t    *got = px_malloc(30000 * sizeof(int64_t));

	prolly_ctx_init(&ctx, cs, &small_policy);
	batch_init(&eb, 3000);
	for (int trial = 0; trial < 10; trial++)
	{
		PHash		a,
					b;
		PHashMap	d;
		uint64_t	f0,
					rows;
		size_t		changes = trial < 5 ? 1 + (size_t) trial * 7 : 3000;

		rng_seed(900 + (uint64_t) trial);
		a = held(&ctx, build_even_rows(&ctx, 12000));
		eb.n = 0;
		for (size_t i = 0; i < changes; i++)
			batch_put(&eb, rnd_below(30000), (uint64_t) trial);
		b = held(&ctx, t_apply(&ctx, &a, eb.e, eb.n));
		prolly_release_pins(&ctx);
		d = chunk_diff_set(&ctx, &a, &b);
		rows = read_rows(&ctx, &b, got, 30000);
		f0 = cs->stats.frees;
		cs_unref_root(cs, &a);
		CHECK_EQ(cs->stats.frees - f0, set_size(&d));
		for (size_t i = 0; i < d.cap; i++)
			if (d.used[i])
				CHECK(!cs->ops->has(cs, &d.keys[i]));
		CHECK_EQ(read_rows(&ctx, &b, got, 30000), rows);
		check_store(cs, 0);
		cs_unref_root(cs, &b);
		CHECK_EQ(cs->ops->count(cs), 0);
		phmap_free(&d);
	}
	free(got);
	batch_free(&eb);
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
}

/* ----------------------------------------------------------------- repair */

static PHash
archive_copy(ChunkStore *archive, ChunkStore *src, const PHash *root)
{
	PinSet		ps;
	PHash		bad;

	pinset_init(&ps, archive);
	CHECK(cs_copy_closure(src, &ps, root, true, &bad) == CS_OK);
	cs_ref_root(archive, root);
	pinset_free(&ps);
	return *root;
}

static void
test_repair_restores_severed_tree(void)
{
	ChunkStore *cs = memstore_create();
	ChunkStore *archive = memstore_create();
	ProllyCtx	ctx;
	PHash		a,
				b;
	EditBatch	eb;
	PHashMap	only_a;
	int64_t    *want = px_malloc(20000 * sizeof(int64_t));
	int64_t    *got = px_malloc(20000 * sizeof(int64_t));
	uint64_t	n0,
				g0;

	prolly_ctx_init(&ctx, cs, &small_policy);
	a = held(&ctx, build_even_rows(&ctx, 8000));
	batch_init(&eb, 60);
	rng_seed(31);
	for (int i = 0; i < 60; i++)
		batch_put(&eb, rnd_below(20000), 3);
	b = held(&ctx, t_apply(&ctx, &a, eb.e, eb.n));
	prolly_release_pins(&ctx);
	read_rows(&ctx, &a, want, 20000);
	archive_copy(archive, cs, &a);
	only_a = chunk_diff_set(&ctx, &a, &b);

	/* sever a, then bring it back */
	cs_unref_root(cs, &a);
	CHECK(!cs->ops->has(cs, &a));
	n0 = cs->stats.new_chunks;
	g0 = archive->stats.gets;
	CHECK_OK(prolly_repair(&ctx, archive, &a));
	/* only what was missing travels */
	CHECK_EQ(cs->stats.new_chunks - n0, set_size(&only_a));
	CHECK_EQ(archive->stats.gets - g0, set_size(&only_a));
	held(&ctx, a);
	prolly_release_pins(&ctx);
	check_store(cs, 0);
	read_rows(&ctx, &a, got, 20000);
	CHECK(memcmp(got, want, 20000 * sizeof(int64_t)) == 0);

	/* repairing something already whole copies nothing */
	n0 = cs->stats.new_chunks;
	CHECK_OK(prolly_repair(&ctx, archive, &a));
	CHECK_EQ(cs->stats.new_chunks - n0, 0);
	prolly_release_pins(&ctx);

	phmap_free(&only_a);
	free(want);
	free(got);
	batch_free(&eb);
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
	cs_destroy(archive);
}

/* A source that lies or is incomplete cannot get a partial tree accepted. */
static void
test_repair_rejects_bad_source(void)
{
	ChunkStore *cs = memstore_create();
	ChunkStore *archive = memstore_create();
	ProllyCtx	ctx;
	PHash		a,
				b,
				victim = {{0}};
	EditBatch	eb;
	PHashMap	only_a;
	size_t		before,
				len;
	uint8_t    *bytes;

	prolly_ctx_init(&ctx, cs, &small_policy);
	a = held(&ctx, build_even_rows(&ctx, 8000));
	batch_init(&eb, 30);
	for (uint64_t i = 0; i < 30; i++)
		batch_put(&eb, i * 541 % 16000, 9);
	b = held(&ctx, t_apply(&ctx, &a, eb.e, eb.n));
	prolly_release_pins(&ctx);
	archive_copy(archive, cs, &a);
	only_a = chunk_diff_set(&ctx, &a, &b);
	for (size_t i = 0; i < only_a.cap; i++)
		if (only_a.used[i] && level_of(cs, &only_a.keys[i]) == 0)
			victim = only_a.keys[i];
	cs_unref_root(cs, &a);
	before = cs->ops->count(cs);

	/* a flipped byte in the source is caught by its hash */
	bytes = (uint8_t *) archive->ops->get(archive, &victim, &len);
	bytes[len - 1] ^= 1;
	CHECK(prolly_repair(&ctx, archive, &a) == PROLLY_ERR_CORRUPT);
	CHECK(phash_eq(&ctx.err_hash, &victim));
	CHECK(!cs->ops->has(cs, &a));
	prolly_release_pins(&ctx);
	CHECK_EQ(cs->ops->count(cs), before);
	check_store(cs, 0);
	bytes[len - 1] ^= 1;

	/* a chunk the source lacks is named */
	archive->ops->del(archive, &victim);
	CHECK(prolly_repair(&ctx, archive, &a) == PROLLY_ERR_MISSING);
	CHECK(phash_eq(&ctx.err_hash, &victim));
	prolly_release_pins(&ctx);
	CHECK_EQ(cs->ops->count(cs), before);
	check_store(cs, 0);

	phmap_free(&only_a);
	batch_free(&eb);
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
	cs_destroy(archive);
}

/* --------------------------------------------------------- reverse deltas */

/*
 * Keep only the newest version and a reverse delta per step; every older
 * version rebuilds to its exact original hash.
 */
static void
test_rebuild_from_reverse_deltas(void)
{
	enum
	{
		VERSIONS = 30, ROWS = 20000
	};
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx;
	PHash		v[VERSIONS],
				delta[VERSIONS];
	EditBatch	eb;
	uint64_t	tree_bytes,
				delta_bytes = 0,
				cur_bytes;

	rng_seed(4242);
	prolly_ctx_init(&ctx, cs, &small_policy);
	batch_init(&eb, 40);
	v[0] = held(&ctx, build_even_rows(&ctx, ROWS));
	for (int i = 1; i < VERSIONS; i++)
	{
		eb.n = 0;
		for (int j = 0; j < 40; j++)
		{
			uint64_t	k = rnd_below(2 * ROWS);

			if (j % 5 == 0)
				batch_del(&eb, k);
			else
				batch_put(&eb, k, (uint64_t) i);
		}
		v[i] = held(&ctx, t_apply(&ctx, &v[i - 1], eb.e, eb.n));
	}
	for (int i = 0; i + 1 < VERSIONS; i++)
	{
		size_t		len;

		CHECK_OK(prolly_delta_write(&ctx, &v[i + 1], &v[i], &delta[i]));
		held(&ctx, delta[i]);
		cs->ops->get(cs, &delta[i], &len);
		delta_bytes += len;
	}
	prolly_release_pins(&ctx);
	tree_bytes = cs->ops->bytes(cs);

	/* drop every tree but the newest */
	for (int i = 0; i + 1 < VERSIONS; i++)
		cs_unref_root(cs, &v[i]);
	check_store(cs, 0);
	cur_bytes = cs->ops->bytes(cs);
	CHECK(!cs->ops->has(cs, &v[0]));
	/* deltas are a small fraction of the trees they replace */
	CHECK_LE(delta_bytes * 4, tree_bytes - cur_bytes);

	/* walk back, newest to oldest, rebuilding each exactly */
	{
		PHash		cur = v[VERSIONS - 1];

		for (int i = VERSIONS - 2; i >= 0; i--)
		{
			PHash		prev;

			CHECK_OK(prolly_delta_apply(&ctx, &delta[i], &prev));
			CHECK(phash_eq(&prev, &v[i]));
			cur = prev;
		}
		CHECK_EQ(t_count(&ctx, &cur), ROWS);
	}
	prolly_release_pins(&ctx);
	check_store(cs, 0);

	/* a delta whose base is gone is a named error */
	CHECK(prolly_delta_apply(&ctx, &delta[0], &v[0]) == PROLLY_ERR_MISSING);
	CHECK(phash_eq(&ctx.err_hash, &v[1]));

	/* a delta that does not reproduce its target is corrupt */
	{
		size_t		len;
		const uint8_t *d = cs->ops->get(cs, &delta[VERSIONS - 2], &len);
		uint8_t    *copy = px_memdup(d, len);
		PHash		forged,
					out;

		copy[2 + PHASH_LEN] ^= 1;	/* the recorded target */
		CHECK_OK(prolly_put_chunk(&ctx, copy, len, &forged));
		CHECK(prolly_delta_apply(&ctx, &forged, &out) == PROLLY_ERR_CORRUPT);
		free(copy);
		prolly_release_pins(&ctx);
	}

	batch_free(&eb);
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
}

/* ---------------------------------------------------------------- scaling */

#define LT_K 100
/* entries a small_policy node can hold: max_bytes over the smallest item */
#define SMALL_MAX_FANOUT (1024 / 30 + 1)

typedef struct LifeScale
{
	uint64_t	n;
	int			height;
	uint64_t	chunk_diff_reads;
	uint64_t	sever_frees;
	uint64_t	sever_ref_updates;
	uint64_t	sever_reads;
	uint64_t	repair_copied;
	uint64_t	repair_src_reads;
	uint64_t	repair_has;
	uint64_t	delta_bytes;
	uint64_t	delta_apply_reads;
	uint64_t	delta_apply_writes;
	uint64_t	hold_frees;		/* frees when an unrelated tree is dropped */
} LifeScale;

static void
life_scale_case(uint64_t n, LifeScale *s)
{
	ChunkStore *cs = memstore_create();
	ChunkStore *archive = memstore_create();
	ProllyCtx	ctx;
	PHash		a,
				b,
				delta,
				back;
	EditBatch	eb;
	PHashMap	d;
	ChunkStoreStats st;
	size_t		len;

	memset(s, 0, sizeof(*s));
	s->n = n;
	rng_seed(n * 3);
	/*
	 * Small chunks keep both sizes unsaturated: with large chunks, 100
	 * edits touch nearly every internal node at 100k rows, which hides how
	 * cost grows.
	 */
	prolly_ctx_init(&ctx, cs, &small_policy);
	a = held(&ctx, build_even_rows(&ctx, n));
	s->height = t_height(&ctx, &a);
	batch_init(&eb, LT_K);
	for (int i = 0; i < LT_K; i++)
		batch_put(&eb, rnd_below(2 * n), 1);
	b = held(&ctx, t_apply(&ctx, &a, eb.e, eb.n));
	prolly_release_pins(&ctx);
	archive_copy(archive, cs, &a);

	st = cs->stats;
	d = chunk_diff_set(&ctx, &a, &b);
	s->chunk_diff_reads = STATS_DELTA(cs, st, gets);

	CHECK_OK(prolly_delta_write(&ctx, &b, &a, &delta));
	held(&ctx, delta);
	prolly_release_pins(&ctx);
	cs->ops->get(cs, &delta, &len);
	s->delta_bytes = len;

	st = cs->stats;
	cs_unref_root(cs, &a);
	s->sever_frees = STATS_DELTA(cs, st, frees);
	s->sever_ref_updates = STATS_DELTA(cs, st, ref_updates);
	s->sever_reads = STATS_DELTA(cs, st, gets);
	CHECK_EQ(s->sever_frees, set_size(&d));

	st = archive->stats;
	{
		uint64_t	n0 = cs->stats.new_chunks,
					h0 = cs->stats.has_calls;

		CHECK_OK(prolly_repair(&ctx, archive, &a));
		s->repair_copied = cs->stats.new_chunks - n0;
		s->repair_has = cs->stats.has_calls - h0;
	}
	s->repair_src_reads = STATS_DELTA(archive, st, gets);
	CHECK_EQ(s->repair_copied, set_size(&d));
	prolly_release_pins(&ctx);	/* not held: repair's copy goes away again */
	CHECK(!cs->ops->has(cs, &a));

	st = cs->stats;
	CHECK_OK(prolly_delta_apply(&ctx, &delta, &back));
	s->delta_apply_reads = STATS_DELTA(cs, st, gets);
	s->delta_apply_writes = STATS_DELTA(cs, st, new_chunks);
	CHECK(phash_eq(&back, &a));
	held(&ctx, back);
	prolly_release_pins(&ctx);

	/* dropping a tree that shares everything with a held one frees ~nothing */
	{
		PHash		same = held(&ctx, t_apply(&ctx, &b, eb.e, 0));

		prolly_release_pins(&ctx);
		st = cs->stats;
		cs_unref_root(cs, &same);
		s->hold_frees = STATS_DELTA(cs, st, frees);
	}

	phmap_free(&d);
	batch_free(&eb);
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
	cs_destroy(archive);
}

static void
test_lifetime_scaling(void)
{
	static const uint64_t sizes[] = {100000, 1000000};
	LifeScale	s[2];

	printf("\n  %8s %2s %9s %8s %10s %8s %11s %9s %9s %10s\n",
		   "rows", "h", "cdiff r", "sever f", "sever ref", "sever r",
		   "repair c/r", "repair h", "delta B", "rebuild r/w");
	for (int i = 0; i < 2; i++)
	{
		LifeScale  *x = &s[i];
		uint64_t	h;
		char		rp[32],
					rb[32];

		life_scale_case(sizes[i], x);
		h = (uint64_t) x->height;
		snprintf(rp, sizeof(rp), "%llu/%llu", (unsigned long long) x->repair_copied,
				 (unsigned long long) x->repair_src_reads);
		snprintf(rb, sizeof(rb), "%llu/%llu", (unsigned long long) x->delta_apply_reads,
				 (unsigned long long) x->delta_apply_writes);
		printf("  %8llu %2d %9llu %8llu %10llu %8llu %11s %9llu %9llu %10s\n",
			   (unsigned long long) x->n, x->height,
			   (unsigned long long) x->chunk_diff_reads,
			   (unsigned long long) x->sever_frees,
			   (unsigned long long) x->sever_ref_updates,
			   (unsigned long long) x->sever_reads, rp,
			   (unsigned long long) x->repair_has,
			   (unsigned long long) x->delta_bytes, rb);

		/* chunk diff: O(d h), like row diff */
		CHECK_LE(x->chunk_diff_reads, 4 * LT_K * h + 2);
		/*
		 * sever: O(d h) chunks freed, and one count update per reference
		 * they hold, so O(d h f) updates for fanout f
		 */
		CHECK_LE(x->sever_frees, 3 * LT_K * h);
		CHECK_LE(x->sever_ref_updates, x->sever_frees * SMALL_MAX_FANOUT);
		CHECK_EQ(x->sever_reads, 0);	/* frees read through the backend */
		/* repair: exactly the missing chunks are read and written */
		CHECK_EQ(x->repair_src_reads, x->repair_copied);
		CHECK_LE(x->repair_has, x->repair_copied * SMALL_MAX_FANOUT + 1);
		/* delta: O(d) bytes; replay costs the same as the edits it records */
		CHECK_LE(x->delta_bytes, LT_K * 40 + 64);
		CHECK_LE(x->delta_apply_reads, 4 * LT_K * h + 2);
		CHECK_LE(x->delta_apply_writes, 3 * LT_K * h);
		CHECK_EQ(x->hold_frees, 0);
	}
	/* 10x the rows: costs follow height, not n */
	CHECK_LE(s[1].chunk_diff_reads, 2 * s[0].chunk_diff_reads * s[1].height / s[0].height);
	CHECK_LE(s[1].sever_frees, 2 * s[0].sever_frees * s[1].height / s[0].height);
	CHECK_LE(s[1].sever_ref_updates, 2 * s[0].sever_ref_updates * s[1].height / s[0].height);
	CHECK_LE(s[1].repair_copied, 2 * s[0].repair_copied * s[1].height / s[0].height);
	CHECK_LE(s[1].repair_has, 2 * s[0].repair_has * s[1].height / s[0].height);
	CHECK_LE(s[1].delta_bytes, 2 * s[0].delta_bytes);
	CHECK_LE(s[1].delta_apply_reads, 2 * s[0].delta_apply_reads * s[1].height / s[0].height);
	printf("%-44s", "");
}

int
main(void)
{
	RUN(test_edits_keep_tree_policy);
	RUN(test_bad_chunks_rejected);
	RUN(test_child_level_mismatch_is_corrupt);
	RUN(test_value_refs_hold_trees);
	RUN(test_refcounts_random_versions);
	RUN(test_reuse_race_pins);
	RUN(test_failed_edit_leaves_no_garbage);
	RUN(test_orphan_sweep_after_crash);
	RUN(test_missing_chunk_errors);
	RUN(test_chunk_diff_exact);
	RUN(test_sever_frees_chunk_diff);
	RUN(test_repair_restores_severed_tree);
	RUN(test_repair_rejects_bad_source);
	RUN(test_rebuild_from_reverse_deltas);
	RUN(test_lifetime_scaling);
	printf("test_lifetime: all tests passed\n");
	return 0;
}
