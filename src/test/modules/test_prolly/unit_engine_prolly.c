/*-------------------------------------------------------------------------
 *
 * unit_engine_prolly.c
 *	  The prolly tree's unit tests (the prototype engine's test_prolly):
 *	  maps checked against a model, history independence, diff, merge, and
 *	  chunk-count bounds that pin down the asymptotic costs.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/test/modules/test_prolly/unit_engine_prolly.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "engine_test_util.h"
#include "fmgr.h"
#include "miscadmin.h"


/* ------------------------------------------------------------------ model */

typedef struct Model
{
	uint64	keyspace;
	bool	   *present;
	uint64   *version;
} Model;

static void
model_init(Model *m, uint64 keyspace)
{
	m->keyspace = keyspace;
	m->present = palloc0((keyspace) * (sizeof(bool)));
	m->version = palloc0((keyspace) * (sizeof(uint64)));
}

static void
model_free(Model *m)
{
	px_free(m->present);
	px_free(m->version);
}

static uint64
model_count(const Model *m)
{
	uint64	n = 0;

	for (uint64 k = 0; k < m->keyspace; k++)
		n += m->present[k];
	return n;
}

static PHash
model_build(ProllyCtx *ctx, const Model *m)
{
	EditBatch	b;
	ProllyKV   *kvs = px_malloc(m->keyspace * sizeof(ProllyKV));
	size_t		n = 0;
	PHash		root;

	batch_init(&b, m->keyspace);
	for (uint64 k = 0; k < m->keyspace; k++)
	{
		if (!m->present[k])
			continue;
		batch_put(&b, k, m->version[k]);
		kvs[n++] = (ProllyKV) {b.e[b.n - 1].key, 8, b.e[b.n - 1].val, VAL_LEN};
	}
	root = t_build(ctx, kvs, n);
	px_free(kvs);
	batch_free(&b);
	return root;
}

static void
check_matches_model(ProllyCtx *ctx, const PHash *root, const Model *m)
{
	ProllyIter *it = t_iter(ctx, root, NULL, 0);
	const uint8 *k,
			   *v;
	size_t		kl,
				vl;
	uint64	next = 0,
				seen = 0;
	PHash		canonical;

	while (t_next(it, &k, &kl, &v, &vl))
	{
		uint64	key;

		CHECK(kl == 8 && vl == VAL_LEN);
		key = get_u64(k);
		CHECK(key >= next && key < m->keyspace);
		for (; next < key; next++)
			CHECK(!m->present[next]);
		CHECK(m->present[key]);
		CHECK(get_u64(v) == key && get_u64(v + 8) == m->version[key]);
		next = key + 1;
		seen++;
	}
	prolly_iter_close(it);
	for (; next < m->keyspace; next++)
		CHECK(!m->present[next]);
	CHECK_EQ(t_count(ctx, root), seen);
	CHECK_EQ(seen, model_count(m));

	/* same contents, built from scratch, must give the same root */
	canonical = model_build(ctx, m);
	CHECK(phash_eq(root, &canonical));
}

/* ------------------------------------------------------------------ tests */

ENGINE_TEST(test_sha256_vectors)
{
	static const struct
	{
		const char *in;
		const char *hex;
	}			vec[] = {
		{"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
		{"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
		{"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
		"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
	};

	for (size_t i = 0; i < sizeof(vec) / sizeof(vec[0]); i++)
	{
		uint8		out[32];
		char		hex[65];

		px_sha256(vec[i].in, strlen(vec[i].in), out);
		for (int j = 0; j < 32; j++)
			snprintf(hex + j * 2, 3, "%02x", out[j]);
		CHECK(strcmp(hex, vec[i].hex) == 0);
	}

	/* a chunk's name: the first 20 bytes of the SHA-256 of its bytes */
	{
		PHash		h;
		char		hex[PHASH_HEX_LEN + 1];

		phash_compute("abc", 3, &h);
		phash_hex(&h, hex);
		CHECK_STR_EQ(hex, "ba7816bf8f01cfea414140de5dae2223b00361a3");
	}
}

ENGINE_TEST(test_empty_map)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &prolly_default_policy);
	PHash		e = t_empty(&ctx);
	PHash		built = t_build(&ctx, NULL, 0);
	const uint8 *v;
	size_t		vl;
	uint8		key[8];
	ProllyIter *it;
	ProllyDiffList d;

	/* (the store the test runs on, in turn) */
	CHECK(cs->routine == (strcmp(engine_test_store, "pages") == 0 ?
						  &PagesChunkStore : &MemoryChunkStore));
	CHECK(phash_eq(&e, &built));
	CHECK_EQ(t_count(&ctx, &e), 0);
	CHECK_EQ(t_height(&ctx, &e), 1);
	put_u64(key, 1);
	CHECK(!t_get(&ctx, &e, key, 8, &v, &vl));
	it = t_iter(&ctx, &e, NULL, 0);
	CHECK(!t_next(it, &v, &vl, &v, &vl));
	prolly_iter_close(it);
	CHECK_OK(prolly_diff_collect(&ctx, &e, &e, &d));
	CHECK_EQ(d.n, 0);
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
}

/*
 * Random batches of puts and deletes, checked against a model after every
 * batch: contents, count, point reads, and equality with a fresh build.
 */
static void
random_edits_case(const ChunkPolicy *policy, uint64 seed, uint64 keyspace,
				  int rounds, size_t max_batch)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, policy);
	PHash		root = t_empty(&ctx);
	Model		m;
	EditBatch	b;

	rng_seed_global(seed);
	model_init(&m, keyspace);
	batch_init(&b, max_batch);
	for (int r = 0; r < rounds; r++)
	{
		size_t		n = 1 + rnd_below_global(max_batch);
		uint64	lo = rnd_below_global(keyspace);
		uint64	span = r % 3 == 0 ? keyspace : 1 + rnd_below_global(keyspace / 8);

		b.n = 0;
		for (size_t i = 0; i < n; i++)
		{
			/* clustered or scattered keys; repeated keys in a batch too */
			uint64	k = (lo + rnd_below_global(span)) % keyspace;

			if (rnd_below_global(100) < (r < rounds / 2 ? 25u : 55u))
			{
				batch_del(&b, k);
				m.present[k] = false;
			}
			else
			{
				uint64	ver = rnd_global();

				batch_put(&b, k, ver);
				m.present[k] = true;
				m.version[k] = ver;
			}
		}
		root = t_apply(&ctx, &root, b.e, b.n);
		check_matches_model(&ctx, &root, &m);
		for (int i = 0; i < 20; i++)
		{
			uint64	k = rnd_below_global(keyspace);
			uint8		key[8];
			const uint8 *v;
			size_t		vl;
			bool		found;

			put_u64(key, k);
			found = t_get(&ctx, &root, key, 8, &v, &vl);
			CHECK(found == m.present[k]);
			if (found)
				CHECK(get_u64(v + 8) == m.version[k]);
		}
	}

	/* delete everything: back to the canonical empty map */
	b.n = 0;
	batch_free(&b);
	batch_init(&b, keyspace);
	for (uint64 k = 0; k < keyspace; k++)
		if (m.present[k])
		{
			batch_del(&b, k);
			m.present[k] = false;
		}
	root = t_apply(&ctx, &root, b.e, b.n);
	{
		PHash		e = t_empty(&ctx);

		CHECK(phash_eq(&root, &e));
	}
	batch_free(&b);
	model_free(&m);
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
}

ENGINE_TEST(test_random_edits_small_chunks)
{
	random_edits_case(&small_policy, 1, 20000, 120, 400);
	random_edits_case(&small_policy, 2, 3000, 200, 20);
}

ENGINE_TEST(test_random_edits_default_chunks)
{
	random_edits_case(&prolly_default_policy, 3, 60000, 60, 3000);
}

ENGINE_TEST(test_single_key_edits)
{
	/* one-edit batches: inserts at both ends, the middle, and deletes */
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &small_policy);
	PHash		root = t_empty(&ctx);
	Model		m;
	EditBatch	b;

	rng_seed_global(4);
	model_init(&m, 4000);
	batch_init(&b, 1);
	for (int i = 0; i < 3000; i++)
	{
		uint64	k = i % 3 == 0 ? (uint64) i : i % 3 == 1 ? 3999 - (uint64) i : rnd_below_global(4000);

		b.n = 0;
		if (i > 2000 && i % 2)
		{
			batch_del(&b, k);
			m.present[k] = false;
		}
		else
		{
			batch_put(&b, k, (uint64) i);
			m.present[k] = true;
			m.version[k] = (uint64) i;
		}
		root = t_apply(&ctx, &root, b.e, b.n);
		if (i % 250 == 0)
			check_matches_model(&ctx, &root, &m);
	}
	check_matches_model(&ctx, &root, &m);
	batch_free(&b);
	model_free(&m);
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
}

ENGINE_TEST(test_history_independence)
{
	const uint64 n = 30000;
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &small_policy);
	PHash		expect = build_even_rows(&ctx, n);
	uint64   *order = px_malloc(n * sizeof(uint64));
	EditBatch	b;

	batch_init(&b, n);
	for (int trial = 0; trial < 4; trial++)
	{
		PHash		root = t_empty(&ctx);
		size_t		done = 0;

		rng_seed_global(100 + (uint64) trial);
		for (uint64 i = 0; i < n; i++)
			order[i] = i;
		if (trial == 1)
			for (uint64 i = 0; i < n / 2; i++)
			{
				uint64	t = order[i];

				order[i] = order[n - 1 - i];
				order[n - 1 - i] = t;
			}
		else if (trial > 1)
			for (uint64 i = n - 1; i > 0; i--)
			{
				uint64	j = rnd_below_global(i + 1),
							t = order[i];

				order[i] = order[j];
				order[j] = t;
			}
		while (done < n)
		{
			size_t		chunk = 1 + rnd_below_global(trial == 3 ? 50 : 5000);

			b.n = 0;
			for (size_t i = 0; i < chunk && done < n; i++, done++)
				batch_put(&b, order[done] * 2, 0);
			root = t_apply(&ctx, &root, b.e, b.n);
		}
		CHECK(phash_eq(&root, &expect));

		/* churn: delete a random half, put it back, same root again */
		b.n = 0;
		for (uint64 i = 0; i < n / 2; i++)
			batch_del(&b, order[i] * 2);
		root = t_apply(&ctx, &root, b.e, b.n);
		CHECK(!phash_eq(&root, &expect));
		b.n = 0;
		for (uint64 i = 0; i < n / 2; i++)
			batch_put(&b, order[i] * 2, 0);
		root = t_apply(&ctx, &root, b.e, b.n);
		CHECK(phash_eq(&root, &expect));
	}
	px_free(order);
	batch_free(&b);
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
}

ENGINE_TEST(test_iter_from_key)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &small_policy);
	PHash		root = build_even_rows(&ctx, 5000);

	for (uint64 start = 0; start < 10002; start += 37)
	{
		uint8		key[8];
		ProllyIter *it;
		const uint8 *k,
				   *v;
		size_t		kl,
					vl;
		uint64	expect = (start + 1) / 2 * 2;
		int			seen = 0;

		put_u64(key, start);
		it = t_iter(&ctx, &root, key, 8);
		while (seen < 30 && t_next(it, &k, &kl, &v, &vl))
		{
			CHECK(get_u64(k) == expect);
			expect += 2;
			seen++;
		}
		if (start >= 9999)
			CHECK(seen == 0);
		prolly_iter_close(it);
	}
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
}

typedef struct DiffExpect
{
	Model	   *from;
	Model	   *to;
	uint64	next;
	uint64	n;
} DiffExpect;

static void
diff_check_fn(void *arg, const ProllyDiff *d)
{
	DiffExpect *x = arg;
	uint64	k = get_u64(d->key);

	CHECK(k >= x->next);
	for (; x->next < k; x->next++)
		CHECK(x->from->present[x->next] == x->to->present[x->next] &&
			  (!x->from->present[x->next] ||
			   x->from->version[x->next] == x->to->version[x->next]));
	CHECK((d->from != NULL) == x->from->present[k]);
	CHECK((d->to != NULL) == x->to->present[k]);
	if (d->from)
		CHECK(get_u64(d->from + 8) == x->from->version[k]);
	if (d->to)
		CHECK(get_u64(d->to + 8) == x->to->version[k]);
	CHECK(!(d->from && d->to) || x->from->version[k] != x->to->version[k]);
	x->next = k + 1;
	x->n++;
}

ENGINE_TEST(test_diff_matches_model)
{
	const uint64 keyspace = 20000;
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &small_policy);

	for (int trial = 0; trial < 12; trial++)
	{
		Model		a,
					b;
		PHash		ra,
					rb;
		DiffExpect	x;
		size_t		changes = trial < 4 ? 1 + (size_t) trial : trial < 8 ? 300 : 8000;

		rng_seed_global(200 + (uint64) trial);
		model_init(&a, keyspace);
		model_init(&b, keyspace);
		for (uint64 k = 0; k < keyspace; k++)
			if (rnd_below_global(3))
			{
				a.present[k] = b.present[k] = true;
				a.version[k] = b.version[k] = k;
			}
		for (size_t i = 0; i < changes; i++)
		{
			uint64	k = rnd_below_global(keyspace);

			b.present[k] = rnd_below_global(4) != 0;
			b.version[k] = rnd_global();
		}
		ra = model_build(&ctx, &a);
		rb = model_build(&ctx, &b);
		for (int dir = 0; dir < 2; dir++)
		{
			x.from = dir ? &b : &a;
			x.to = dir ? &a : &b;
			x.next = 0;
			x.n = 0;
			CHECK_OK(prolly_diff(&ctx, dir ? &rb : &ra, dir ? &ra : &rb, diff_check_fn, &x));
			for (; x.next < keyspace; x.next++)
				CHECK(x.from->present[x.next] == x.to->present[x.next] &&
					  (!x.from->present[x.next] ||
					   x.from->version[x.next] == x.to->version[x.next]));
		}
		model_free(&a);
		model_free(&b);
	}
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
}

/*
 * One side's edits for a grafting trial: scattered puts, inserts and
 * deletes, and runs of them, on keys the other side has not claimed.
 */
static void
graft_side(EditBatch *b, uint8 *claimed, uint64 keyspace, int runs,
		   int scattered, uint64 version)
{
	for (int r = 0; r < runs + scattered; r++)
	{
		uint64	start = rnd_global() % keyspace;
		uint64	len = r < runs ? 1 + rnd_global() % 400 : 1;
		int			kind = (int) (rnd_global() % 3);

		for (uint64 k = start; k < start + len && k < keyspace; k++)
		{
			if (claimed[k])
				continue;
			claimed[k] = 1;
			if (kind == 0 && k % 2 == 0)
				batch_put(b, k, version);	/* an update */
			else if (kind == 1 && k % 2 == 1)
				batch_put(b, k, version);	/* an insert */
			else if (kind == 2 && k % 2 == 0)
				batch_del(b, k);
		}
	}
}

/*
 * A merge takes theirs' subtrees ours left alone whole, by ref, and gets
 * the same tree as applying both sides' edits to the base would, also
 * where a rechunking next to a graft moves the boundary before it.
 */
ENGINE_TEST(test_merge_grafts)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &small_policy);
	uint64		rows = 20000,
				keyspace = rows * 2;
	/* (in 711 and 1978, theirs onto ours, a rechunking crosses a graft's start) */
	int			grafted = 0,
				trials = 42;
	PHash		base = build_even_rows(&ctx, rows);

	for (int trial = 0; trial < trials; trial++)
	{
		uint8    *claimed = palloc0((keyspace) * (1));
		EditBatch	o,
					t,
					all;
		PHash		ours,
					theirs,
					expect;
		ProllyMergeResult mr;

		int			seed = trial < 40 ? trial : trial == 40 ? 711 : 1978;

		rng_seed_global(7000 + (uint64) seed);
		batch_init(&o, keyspace);
		batch_init(&t, keyspace);
		batch_init(&all, keyspace);
		/* (some trials one-sided, some with both sides busy) */
		graft_side(&o, claimed, keyspace, seed % 4, 1 + seed % 30, 1);
		graft_side(&t, claimed, keyspace, (seed / 4) % 5, 1 + seed % 17, 2);
		for (size_t i = 0; i < o.n; i++)
			all.e[all.n++] = o.e[i];
		for (size_t i = 0; i < t.n; i++)
			all.e[all.n++] = t.e[i];
		ours = t_apply(&ctx, &base, o.e, o.n);
		theirs = t_apply(&ctx, &base, t.e, t.n);
		expect = t_apply(&ctx, &base, all.e, all.n);
		CHECK_OK(prolly_merge(&ctx, &base, &ours, &theirs, &mr));
		CHECK_EQ(mr.nconflicts, 0);
		CHECK(phash_eq(&mr.root, &expect));
		grafted += mr.ngrafts > 0;
		prolly_merge_result_free(&mr);
		CHECK_OK(prolly_merge(&ctx, &base, &theirs, &ours, &mr));
		CHECK_EQ(mr.nconflicts, 0);
		CHECK(phash_eq(&mr.root, &expect));
		prolly_merge_result_free(&mr);
		batch_free(&o);
		batch_free(&t);
		batch_free(&all);
		px_free(claimed);
	}
	elog(DEBUG1, "%d of %d grafted", grafted, trials);
	CHECK(grafted >= trials / 2);

	/*
	 * Theirs rewrote a block of 4000 rows, ours a few rows elsewhere: the
	 * merge reads paths, not the block's leaves on either side.
	 */
	{
		EditBatch	o,
					t;
		PHash		ours,
					theirs;
		ProllyMergeResult mr;
		uint64	gets;

		batch_init(&o, 16);
		batch_init(&t, 4000);
		for (uint64 k = 0; k < 4000; k++)
			batch_put(&t, 10000 + k * 2, 3);
		for (uint64 k = 0; k < 5; k++)
			batch_put(&o, 30000 + k * 1000, 4);
		ours = t_apply(&ctx, &base, o.e, o.n);
		theirs = t_apply(&ctx, &base, t.e, t.n);
		gets = cs->stats.gets;
		CHECK_OK(prolly_merge(&ctx, &base, &ours, &theirs, &mr));
		gets = cs->stats.gets - gets;
		elog(DEBUG1, "%llu reads, %zu grafts", (unsigned long long) gets, mr.ngrafts);
		CHECK(mr.ngrafts > 0);
		CHECK(gets < 200);
		prolly_merge_result_free(&mr);
		batch_free(&o);
		batch_free(&t);
	}
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
}

ENGINE_TEST(test_merge)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &small_policy);
	PHash		base = build_even_rows(&ctx, 10000);
	PHash		ours,
				theirs,
				expect;
	EditBatch	o,
				t,
				all;
	ProllyMergeResult mr;

	batch_init(&o, 64);
	batch_init(&t, 64);
	batch_init(&all, 128);

	/* disjoint changes merge cleanly, from both directions */
	batch_put(&o, 10, 1);
	batch_put(&o, 11, 1);
	batch_del(&o, 5000);
	batch_put(&t, 19998, 2);
	batch_put(&t, 20001, 2);
	batch_del(&t, 0);
	/* the same change on both sides is not a conflict */
	batch_put(&o, 4000, 7);
	batch_put(&t, 4000, 7);
	batch_del(&o, 4002);
	batch_del(&t, 4002);
	for (size_t i = 0; i < o.n; i++)
		all.e[all.n++] = o.e[i];
	for (size_t i = 0; i < t.n; i++)
		all.e[all.n++] = t.e[i];
	ours = t_apply(&ctx, &base, o.e, o.n);
	theirs = t_apply(&ctx, &base, t.e, t.n);
	expect = t_apply(&ctx, &base, all.e, all.n);
	CHECK_OK(prolly_merge(&ctx, &base, &ours, &theirs, &mr));
	CHECK_EQ(mr.nconflicts, 0);
	CHECK(phash_eq(&mr.root, &expect));
	prolly_merge_result_free(&mr);
	CHECK_OK(prolly_merge(&ctx, &base, &theirs, &ours, &mr));
	CHECK_EQ(mr.nconflicts, 0);
	CHECK(phash_eq(&mr.root, &expect));
	prolly_merge_result_free(&mr);

	/* trivial merges */
	CHECK_OK(prolly_merge(&ctx, &base, &base, &theirs, &mr));
	CHECK(phash_eq(&mr.root, &theirs) && mr.nconflicts == 0);
	prolly_merge_result_free(&mr);
	CHECK_OK(prolly_merge(&ctx, &base, &ours, &base, &mr));
	CHECK(phash_eq(&mr.root, &ours) && mr.nconflicts == 0);
	prolly_merge_result_free(&mr);

	/* update/update and delete/update conflicts keep ours */
	o.n = t.n = 0;
	batch_put(&o, 100, 1);
	batch_put(&t, 100, 2);
	batch_del(&o, 200);
	batch_put(&t, 200, 2);
	batch_put(&t, 300, 2);
	ours = t_apply(&ctx, &base, o.e, o.n);
	theirs = t_apply(&ctx, &base, t.e, t.n);
	CHECK_OK(prolly_merge(&ctx, &base, &ours, &theirs, &mr));
	CHECK_EQ(mr.nconflicts, 2);
	CHECK(get_u64(mr.conflicts[0].key) == 100 && get_u64(mr.conflicts[1].key) == 200);
	CHECK(get_u64(mr.conflicts[0].ours + 8) == 1 && get_u64(mr.conflicts[0].theirs + 8) == 2);
	CHECK(mr.conflicts[1].ours == NULL && mr.conflicts[1].base != NULL);
	{
		const uint8 *v;
		size_t		vl;
		uint8		key[8];

		put_u64(key, 300);
		CHECK(t_get(&ctx, &mr.root, key, 8, &v, &vl) && get_u64(v + 8) == 2);
		put_u64(key, 100);
		CHECK(t_get(&ctx, &mr.root, key, 8, &v, &vl) && get_u64(v + 8) == 1);
	}
	prolly_merge_result_free(&mr);
	batch_free(&o);
	batch_free(&t);
	batch_free(&all);
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
}

/* ---------------------------------------------------------------- scaling */

typedef struct ScaleResult
{
	uint64	n;
	int			height;
	uint64	chunks;			/* chunks in the base tree */
	double		get_reads;		/* per lookup */
	uint64	count_reads;
	uint64	edit1_reads;
	uint64	edit1_writes;
	uint64	editk_reads;
	uint64	editk_writes;
	uint64	diff_reads;
	uint64	merge_reads;
	uint64	merge_writes;
	double		build_sec;
	double		editk_usec;
} ScaleResult;

#define SCALE_K 100

static void
random_edits(EditBatch *b, uint64 n, size_t k, uint64 version)
{
	b->n = 0;
	for (size_t i = 0; i < k; i++)
	{
		uint64	k2 = rnd_below_global(2 * n);

		switch (rnd_below_global(3))
		{
			case 0:
				batch_put(b, k2 | 1, version);	/* insert a new odd key */
				break;
			case 1:
				batch_put(b, k2 & ~1ULL, version);	/* update */
				break;
			default:
				batch_del(b, k2 & ~1ULL);
				break;
		}
	}
}

static void
count_fn(void *arg, const ProllyDiff *d)
{
	(*(uint64 *) arg)++;
}

static void
scale_case(uint64 n, ScaleResult *r)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &prolly_default_policy);
	PHash		root,
				edited,
				ours,
				theirs;
	ChunkStoreStats s;
	EditBatch	b;
	double		t;
	uint64	ndiff = 0;
	ProllyMergeResult mr;

	memset(r, 0, sizeof(*r));
	rng_seed_global(n);
	r->n = n;
	t = now_sec();
	root = build_even_rows(&ctx, n);
	r->build_sec = now_sec() - t;
	r->chunks = store_count(cs);
	r->height = t_height(&ctx, &root);
	batch_init(&b, 2 * SCALE_K);

	s = cs->stats;
	for (int i = 0; i < 1000; i++)
	{
		uint8		key[8];
		const uint8 *v;
		size_t		vl;

		put_u64(key, rnd_below_global(n) * 2);
		CHECK(t_get(&ctx, &root, key, 8, &v, &vl));
	}
	r->get_reads = STATS_DELTA(cs, s, gets) / 1000.0;

	s = cs->stats;
	CHECK_EQ(t_count(&ctx, &root), n);
	r->count_reads = STATS_DELTA(cs, s, gets);

	/* single-row update, averaged over 50 rows */
	s = cs->stats;
	for (int i = 0; i < 50; i++)
	{
		b.n = 0;
		batch_put(&b, rnd_below_global(n) * 2, 1000 + (uint64) i);
		edited = t_apply(&ctx, &root, b.e, b.n);
	}
	r->edit1_reads = STATS_DELTA(cs, s, gets) / 50;
	r->edit1_writes = STATS_DELTA(cs, s, stored) / 50;

	/* a batch of k random inserts, updates and deletes */
	random_edits(&b, n, SCALE_K, 1);
	s = cs->stats;
	t = now_sec();
	edited = t_apply(&ctx, &root, b.e, b.n);
	r->editk_usec = (now_sec() - t) * 1e6;
	r->editk_reads = STATS_DELTA(cs, s, gets);
	r->editk_writes = STATS_DELTA(cs, s, stored);

	s = cs->stats;
	CHECK_OK(prolly_diff(&ctx, &root, &edited, count_fn, &ndiff));
	r->diff_reads = STATS_DELTA(cs, s, gets);
	CHECK(ndiff > 0 && ndiff <= SCALE_K);

	/* three-way merge of k changes on each side */
	random_edits(&b, n, SCALE_K, 2);
	ours = t_apply(&ctx, &root, b.e, b.n);
	random_edits(&b, n, SCALE_K, 3);
	theirs = t_apply(&ctx, &root, b.e, b.n);
	s = cs->stats;
	CHECK_OK(prolly_merge(&ctx, &root, &ours, &theirs, &mr));
	r->merge_reads = STATS_DELTA(cs, s, gets);
	r->merge_writes = STATS_DELTA(cs, s, stored);
	prolly_merge_result_free(&mr);

	batch_free(&b);
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
}

static void
check_scale_bounds(const ScaleResult *r)
{
	uint64	h = (uint64) r->height;

	/* point read: exactly one chunk per level */
	CHECK(r->get_reads == (double) h);
	/* count: the root holds subtree counts */
	CHECK_EQ(r->count_reads, 1);
	/* one edit rewrites O(1) chunks per level */
	CHECK_LE(r->edit1_writes, 3 * h);
	CHECK_LE(r->edit1_reads, 4 * h + 2);
	/* k edits: O(k log n), never more than a few chunks per edit per level */
	CHECK_LE(r->editk_writes, 3 * SCALE_K * h);
	CHECK_LE(r->editk_reads, 4 * SCALE_K * h + 2);
	/* diff: O(d log n) reads */
	CHECK_LE(r->diff_reads, 4 * SCALE_K * h + 2);
	/* merge: two diffs plus applying theirs */
	CHECK_LE(r->merge_reads, 12 * SCALE_K * h + 6);
	CHECK_LE(r->merge_writes, 3 * SCALE_K * h);
}

/* cost at 10x the rows may grow only with height, never with n */
static void
check_sublinear(const ScaleResult *small, const ScaleResult *big,
				uint64 small_cost, uint64 big_cost, const char *what)
{
	double		hratio = (double) big->height / small->height;
	double		allowed = 2.0 * hratio * (double) (small_cost ? small_cost : 1);

	if ((double) big_cost > allowed)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg_internal("%s grew with n: %llu at n=%llu vs %llu at n=%llu",
								 what, (unsigned long long) big_cost,
								 (unsigned long long) big->n,
								 (unsigned long long) small_cost,
								 (unsigned long long) small->n)));
	}
}

ENGINE_TEST(test_scaling)
{
	static const uint64 sizes[] = {10000, 100000, 1000000};
	ScaleResult r[3];

	elog(DEBUG1, "  %9s %3s %7s %6s %5s %10s %10s %10s %9s %10s %10s %8s",
		   "rows", "h", "chunks", "get", "count", "edit1 r/w",
		   "edit100 r/w", "us/edit", "diff100 r", "merge r/w", "build s", "");
	for (int i = 0; i < 3; i++)
	{
		char		e1[48],
					ek[48],
					mg[48];

		scale_case(sizes[i], &r[i]);
		snprintf(e1, sizeof(e1), "%llu/%llu", (unsigned long long) r[i].edit1_reads,
				 (unsigned long long) r[i].edit1_writes);
		snprintf(ek, sizeof(ek), "%llu/%llu", (unsigned long long) r[i].editk_reads,
				 (unsigned long long) r[i].editk_writes);
		snprintf(mg, sizeof(mg), "%llu/%llu", (unsigned long long) r[i].merge_reads,
				 (unsigned long long) r[i].merge_writes);
		elog(DEBUG1, "  %9llu %3d %7llu %6.1f %5llu %10s %10s %10.1f %9llu %10s %10.2f",
			   (unsigned long long) r[i].n, r[i].height,
			   (unsigned long long) r[i].chunks, r[i].get_reads,
			   (unsigned long long) r[i].count_reads, e1, ek,
			   r[i].editk_usec / SCALE_K, (unsigned long long) r[i].diff_reads,
			   mg, r[i].build_sec);
		check_scale_bounds(&r[i]);
	}
	/*
	 * At 10k rows the tree has so few leaves that 100 edits touch most of
	 * them, so compare the two larger sizes, where they do not.
	 */
	check_sublinear(&r[1], &r[2], r[1].edit1_writes, r[2].edit1_writes, "edit1 writes");
	check_sublinear(&r[1], &r[2], r[1].editk_writes, r[2].editk_writes, "edit100 writes");
	check_sublinear(&r[1], &r[2], r[1].editk_reads, r[2].editk_reads, "edit100 reads");
	check_sublinear(&r[1], &r[2], r[1].diff_reads, r[2].diff_reads, "diff reads");
	check_sublinear(&r[1], &r[2], r[1].merge_reads, r[2].merge_reads, "merge reads");
	check_sublinear(&r[1], &r[2], r[1].merge_writes, r[2].merge_writes, "merge writes");
}

/* Structural sharing: a new version stores only the chunks it changed. */
ENGINE_TEST(test_structural_sharing)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &prolly_default_policy);
	/* (the store's bytes before: "pages" may hold others' chunks) */
	uint64		before = store_bytes(cs);
	PHash		root = build_even_rows(&ctx, 200000);
	uint64		base_bytes = store_bytes(cs) - before;
	uint64	h = (uint64) t_height(&ctx, &root);
	EditBatch	b;

	rng_seed_global(9);
	batch_init(&b, 10);
	for (int v = 0; v < 100; v++)
	{
		ChunkStoreStats s = cs->stats;

		random_edits(&b, 200000, 10, (uint64) v + 10);
		root = t_apply(&ctx, &root, b.e, b.n);
		CHECK_LE(STATS_DELTA(cs, s, stored), 3 * 10 * h);
	}
	/* 100 versions cost a few full copies' worth, not 100 */
	CHECK_LE(store_bytes(cs) - before - base_bytes, base_bytes * 5);
	batch_free(&b);
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
}


/* (the bytes a context's chunks in use take: its space, but its free space) */
static Size
context_used(MemoryContext cxt)
{
	MemoryContextCounters c;

	MemoryContextMemConsumed(cxt, &c);
	return c.totalspace - c.freespace;
}

/*
 * A merge frees what it copied (its diffs' keys and values, and its
 * conflicts' once its result is freed): merges one after another in one
 * context take no more memory than the first.
 */
ENGINE_TEST(test_merge_frees_its_copies)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &small_policy);
	PHash		base = build_even_rows(&ctx, 5000);
	EditBatch	ob,
				tb;
	PHash		ours,
				theirs;
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "merges",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContext old;
	Size		first = 0;

	batch_init(&ob, 2000);
	batch_init(&tb, 2000);
	for (uint64 k = 0; k < 2000; k++)
	{
		batch_put(&ob, k * 2, 1);
		/* (the first 100 conflicts, both sides changing them differently) */
		batch_put(&tb, k < 100 ? k * 2 : k * 2 + 4000, 2);
	}
	ours = t_apply(&ctx, &base, ob.e, ob.n);
	theirs = t_apply(&ctx, &base, tb.e, tb.n);
	old = MemoryContextSwitchTo(cxt);
	for (int round = 0; round < 5; round++)
	{
		ProllyMergeResult mr;

		CHECK_OK(prolly_merge(&ctx, &base, &ours, &theirs, &mr));
		CHECK_EQ(mr.nconflicts, 100);
		prolly_merge_result_free(&mr);
		if (round == 0)
			first = context_used(cxt);
		else
			CHECK_LE(context_used(cxt), first);
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
	batch_free(&ob);
	batch_free(&tb);
	cs_destroy(cs);
}

/*
 * An iterator reads its nodes in the context it was opened in: a caller
 * resetting its own context between calls (a scan's per-row context) frees
 * none the cursor holds.
 */
ENGINE_TEST(test_iter_across_context_resets)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &small_policy);
	PHash		root = build_even_rows(&ctx, 5000);
	ProllyIter *it = t_iter(&ctx, &root, NULL, 0);
	MemoryContext row = AllocSetContextCreate(CurrentMemoryContext, "row",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContext old;
	const uint8 *k,
			   *v;
	size_t		kl,
				vl;
	uint64		n = 0;

	for (;;)
	{
		bool		more;

		old = MemoryContextSwitchTo(row);
		more = t_next(it, &k, &kl, &v, &vl);
		MemoryContextSwitchTo(old);
		if (!more)
			break;
		CHECK_EQ(get_u64(k), n * 2);
		CHECK_EQ(get_u64(v), n * 2);
		n++;
		MemoryContextReset(row);
	}
	CHECK_EQ(n, 5000);
	prolly_iter_close(it);
	MemoryContextDelete(row);
	cs_destroy(cs);
}

/*
 * A delta naming more edits than its bytes could hold (two bytes each at
 * least) is no delta: refused as one, without allocating for its count.
 */
ENGINE_TEST(test_corrupt_delta_count)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &small_policy);
	PHash		base = build_even_rows(&ctx, 10);
	Buf			b = BUF_INIT;
	PHash		delta,
				out;

	buf_append_byte(&b, CHUNK_TAG_DELTA);
	buf_append_byte(&b, CHUNK_FORMAT_VERSION);
	buf_append(&b, base.b, PHASH_LEN);
	buf_append(&b, base.b, PHASH_LEN);
	buf_append_uvarint(&b, UINT64CONST(1) << 40);
	CHECK_OK(prolly_put_chunk(&ctx, b.data, b.len, &delta));
	CHECK_EQ(prolly_delta_apply(&ctx, &delta, &out), PROLLY_ERR_BAD_DELTA);
	CHECK(ctx.err_has_hash && phash_eq(&ctx.err_hash, &delta));
	buf_free(&b);
	cs_destroy(cs);
}

static const ProllyUnitTest tests[] = {
	{"sha256_vectors", test_sha256_vectors},
	{"empty_map", test_empty_map},
	{"random_edits_small_chunks", test_random_edits_small_chunks},
	{"random_edits_default_chunks", test_random_edits_default_chunks},
	{"single_key_edits", test_single_key_edits},
	{"history_independence", test_history_independence},
	{"iter_from_key", test_iter_from_key},
	{"diff_matches_model", test_diff_matches_model},
	{"merge", test_merge},
	{"merge_grafts", test_merge_grafts},
	{"structural_sharing", test_structural_sharing},
	{"scaling", test_scaling},
	{"merge_frees_its_copies", test_merge_frees_its_copies},
	{"iter_across_context_resets", test_iter_across_context_resets},
	{"corrupt_delta_count", test_corrupt_delta_count},
};

const ProllyUnitSuite unit_engine_prolly_suite = {"engine_prolly", tests, lengthof(tests)};

PG_FUNCTION_INFO_V1(test_engine_build);
PG_FUNCTION_INFO_V1(test_engine_build_finished);

/* whether the last build of test_engine_build ran to its end */
static bool build_finished = false;

/*
 * test_engine_build(rows int8, cancel bool) returns int8: a map of rows rows built on
 * this backend's memory store (emptied first, and after), and rows; for a
 * cancel of a long build: test_engine_build_finished() says whether the
 * last one ran to its end (a cancel raised only after it, by the executor,
 * would not show it was cancelled)
 */
Datum
test_engine_build(PG_FUNCTION_ARGS)
{
	int64		rows = PG_GETARG_INT64(0);
	const ChunkFormatRoutine *format = ChunkFormat;
	uint64		count = 0;

	ChunkFormat = &EngineChunkFormat;
	PG_TRY();
	{
		ChunkStore *cs;
		ProllyCtx	ctx;
		PHash		root;

		build_finished = false;
		engine_test_store = "memory";

		/*
		 * (cancelled, if asked, at the build's first check for interrupts,
		 * however fast it runs: its own statement's cancel, as a user's)
		 */
		if (PG_GETARG_BOOL(1))
		{
			QueryCancelPending = true;
			InterruptPending = true;
		}
		cs = memstore_create();
		ctx = t_ctx(cs, &prolly_default_policy);
		root = build_even_rows(&ctx, (uint64) rows);
		build_finished = true;
		count = t_count(&ctx, &root);
	}
	PG_FINALLY();
	{
		ChunkFormat = format;
		MemoryChunkStoreReset();
	}
	PG_END_TRY();
	PG_RETURN_INT64((int64) count);
}

Datum
test_engine_build_finished(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(build_finished);
}
