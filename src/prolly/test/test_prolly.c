/*
 * test_prolly.c
 *	  Prolly map correctness against a model, history independence, diff,
 *	  merge, and chunk-count bounds that pin down the asymptotic costs.
 */
#include "test_util.h"

/* ------------------------------------------------------------------ model */

typedef struct Model
{
	uint64_t	keyspace;
	bool	   *present;
	uint64_t   *version;
} Model;

static void
model_init(Model *m, uint64_t keyspace)
{
	m->keyspace = keyspace;
	m->present = calloc(keyspace, sizeof(bool));
	m->version = calloc(keyspace, sizeof(uint64_t));
}

static void
model_free(Model *m)
{
	free(m->present);
	free(m->version);
}

static uint64_t
model_count(const Model *m)
{
	uint64_t	n = 0;

	for (uint64_t k = 0; k < m->keyspace; k++)
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
	for (uint64_t k = 0; k < m->keyspace; k++)
	{
		if (!m->present[k])
			continue;
		batch_put(&b, k, m->version[k]);
		kvs[n++] = (ProllyKV) {b.e[b.n - 1].key, 8, b.e[b.n - 1].val, VAL_LEN};
	}
	root = prolly_build(ctx, kvs, n);
	free(kvs);
	batch_free(&b);
	return root;
}

static void
check_matches_model(ProllyCtx *ctx, const PHash *root, const Model *m)
{
	ProllyIter *it = prolly_iter_open(ctx, root, NULL, 0);
	const uint8_t *k,
			   *v;
	size_t		kl,
				vl;
	uint64_t	next = 0,
				seen = 0;
	PHash		canonical;

	while (prolly_iter_next(it, &k, &kl, &v, &vl))
	{
		uint64_t	key;

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
	CHECK_EQ(prolly_count(ctx, root), seen);
	CHECK_EQ(seen, model_count(m));

	/* same contents, built from scratch, must give the same root */
	canonical = model_build(ctx, m);
	CHECK(phash_eq(root, &canonical));
}

/* ------------------------------------------------------------------ tests */

static void
test_sha256_vectors(void)
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
		uint8_t		out[32];
		char		hex[65];

		sha256(vec[i].in, strlen(vec[i].in), out);
		for (int j = 0; j < 32; j++)
			snprintf(hex + j * 2, 3, "%02x", out[j]);
		CHECK(strcmp(hex, vec[i].hex) == 0);
	}
}

static void
test_empty_map(void)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = {cs, prolly_default_policy};
	PHash		e = prolly_empty(&ctx);
	PHash		built = prolly_build(&ctx, NULL, 0);
	const uint8_t *v;
	size_t		vl;
	uint8_t		key[8];
	ProllyIter *it;
	ProllyDiffList d;

	CHECK(phash_eq(&e, &built));
	CHECK_EQ(prolly_count(&ctx, &e), 0);
	CHECK_EQ(prolly_height(&ctx, &e), 1);
	put_u64(key, 1);
	CHECK(!prolly_get(&ctx, &e, key, 8, &v, &vl));
	it = prolly_iter_open(&ctx, &e, NULL, 0);
	CHECK(!prolly_iter_next(it, &v, &vl, &v, &vl));
	prolly_iter_close(it);
	prolly_diff_collect(&ctx, &e, &e, &d);
	CHECK_EQ(d.n, 0);
	cs_destroy(cs);
}

/*
 * Random batches of puts and deletes, checked against a model after every
 * batch: contents, count, point reads, and equality with a fresh build.
 */
static void
random_edits_case(const ChunkPolicy *policy, uint64_t seed, uint64_t keyspace,
				  int rounds, size_t max_batch)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = {cs, *policy};
	PHash		root = prolly_empty(&ctx);
	Model		m;
	EditBatch	b;

	rng_seed(seed);
	model_init(&m, keyspace);
	batch_init(&b, max_batch);
	for (int r = 0; r < rounds; r++)
	{
		size_t		n = 1 + rnd_below(max_batch);
		uint64_t	lo = rnd_below(keyspace);
		uint64_t	span = r % 3 == 0 ? keyspace : 1 + rnd_below(keyspace / 8);

		b.n = 0;
		for (size_t i = 0; i < n; i++)
		{
			/* clustered or scattered keys; repeated keys in a batch too */
			uint64_t	k = (lo + rnd_below(span)) % keyspace;

			if (rnd_below(100) < (r < rounds / 2 ? 25u : 55u))
			{
				batch_del(&b, k);
				m.present[k] = false;
			}
			else
			{
				uint64_t	ver = rnd();

				batch_put(&b, k, ver);
				m.present[k] = true;
				m.version[k] = ver;
			}
		}
		root = prolly_apply(&ctx, &root, b.e, b.n);
		check_matches_model(&ctx, &root, &m);
		for (int i = 0; i < 20; i++)
		{
			uint64_t	k = rnd_below(keyspace);
			uint8_t		key[8];
			const uint8_t *v;
			size_t		vl;
			bool		found;

			put_u64(key, k);
			found = prolly_get(&ctx, &root, key, 8, &v, &vl);
			CHECK(found == m.present[k]);
			if (found)
				CHECK(get_u64(v + 8) == m.version[k]);
		}
	}

	/* delete everything: back to the canonical empty map */
	b.n = 0;
	batch_free(&b);
	batch_init(&b, keyspace);
	for (uint64_t k = 0; k < keyspace; k++)
		if (m.present[k])
		{
			batch_del(&b, k);
			m.present[k] = false;
		}
	root = prolly_apply(&ctx, &root, b.e, b.n);
	{
		PHash		e = prolly_empty(&ctx);

		CHECK(phash_eq(&root, &e));
	}
	batch_free(&b);
	model_free(&m);
	cs_destroy(cs);
}

static void
test_random_edits_small_chunks(void)
{
	random_edits_case(&small_policy, 1, 20000, 120, 400);
	random_edits_case(&small_policy, 2, 3000, 200, 20);
}

static void
test_random_edits_default_chunks(void)
{
	random_edits_case(&prolly_default_policy, 3, 60000, 60, 3000);
}

static void
test_single_key_edits(void)
{
	/* one-edit batches: inserts at both ends, the middle, and deletes */
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = {cs, small_policy};
	PHash		root = prolly_empty(&ctx);
	Model		m;
	EditBatch	b;

	rng_seed(4);
	model_init(&m, 4000);
	batch_init(&b, 1);
	for (int i = 0; i < 3000; i++)
	{
		uint64_t	k = i % 3 == 0 ? (uint64_t) i : i % 3 == 1 ? 3999 - (uint64_t) i : rnd_below(4000);

		b.n = 0;
		if (i > 2000 && i % 2)
		{
			batch_del(&b, k);
			m.present[k] = false;
		}
		else
		{
			batch_put(&b, k, (uint64_t) i);
			m.present[k] = true;
			m.version[k] = (uint64_t) i;
		}
		root = prolly_apply(&ctx, &root, b.e, b.n);
		if (i % 250 == 0)
			check_matches_model(&ctx, &root, &m);
	}
	check_matches_model(&ctx, &root, &m);
	batch_free(&b);
	model_free(&m);
	cs_destroy(cs);
}

static void
test_history_independence(void)
{
	const uint64_t n = 30000;
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = {cs, small_policy};
	PHash		expect = build_even_rows(&ctx, n);
	uint64_t   *order = px_malloc(n * sizeof(uint64_t));
	EditBatch	b;

	batch_init(&b, n);
	for (int trial = 0; trial < 4; trial++)
	{
		PHash		root = prolly_empty(&ctx);
		size_t		done = 0;

		rng_seed(100 + (uint64_t) trial);
		for (uint64_t i = 0; i < n; i++)
			order[i] = i;
		if (trial == 1)
			for (uint64_t i = 0; i < n / 2; i++)
			{
				uint64_t	t = order[i];

				order[i] = order[n - 1 - i];
				order[n - 1 - i] = t;
			}
		else if (trial > 1)
			for (uint64_t i = n - 1; i > 0; i--)
			{
				uint64_t	j = rnd_below(i + 1),
							t = order[i];

				order[i] = order[j];
				order[j] = t;
			}
		while (done < n)
		{
			size_t		chunk = 1 + rnd_below(trial == 3 ? 50 : 5000);

			b.n = 0;
			for (size_t i = 0; i < chunk && done < n; i++, done++)
				batch_put(&b, order[done] * 2, 0);
			root = prolly_apply(&ctx, &root, b.e, b.n);
		}
		CHECK(phash_eq(&root, &expect));

		/* churn: delete a random half, put it back, same root again */
		b.n = 0;
		for (uint64_t i = 0; i < n / 2; i++)
			batch_del(&b, order[i] * 2);
		root = prolly_apply(&ctx, &root, b.e, b.n);
		CHECK(!phash_eq(&root, &expect));
		b.n = 0;
		for (uint64_t i = 0; i < n / 2; i++)
			batch_put(&b, order[i] * 2, 0);
		root = prolly_apply(&ctx, &root, b.e, b.n);
		CHECK(phash_eq(&root, &expect));
	}
	free(order);
	batch_free(&b);
	cs_destroy(cs);
}

static void
test_iter_from_key(void)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = {cs, small_policy};
	PHash		root = build_even_rows(&ctx, 5000);

	for (uint64_t start = 0; start < 10002; start += 37)
	{
		uint8_t		key[8];
		ProllyIter *it;
		const uint8_t *k,
				   *v;
		size_t		kl,
					vl;
		uint64_t	expect = (start + 1) / 2 * 2;
		int			seen = 0;

		put_u64(key, start);
		it = prolly_iter_open(&ctx, &root, key, 8);
		while (seen < 30 && prolly_iter_next(it, &k, &kl, &v, &vl))
		{
			CHECK(get_u64(k) == expect);
			expect += 2;
			seen++;
		}
		if (start >= 9999)
			CHECK(seen == 0);
		prolly_iter_close(it);
	}
	cs_destroy(cs);
}

typedef struct DiffExpect
{
	Model	   *from;
	Model	   *to;
	uint64_t	next;
	uint64_t	n;
} DiffExpect;

static void
diff_check_fn(void *arg, const ProllyDiff *d)
{
	DiffExpect *x = arg;
	uint64_t	k = get_u64(d->key);

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

static void
test_diff_matches_model(void)
{
	const uint64_t keyspace = 20000;
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = {cs, small_policy};

	for (int trial = 0; trial < 12; trial++)
	{
		Model		a,
					b;
		PHash		ra,
					rb;
		DiffExpect	x;
		size_t		changes = trial < 4 ? 1 + (size_t) trial : trial < 8 ? 300 : 8000;

		rng_seed(200 + (uint64_t) trial);
		model_init(&a, keyspace);
		model_init(&b, keyspace);
		for (uint64_t k = 0; k < keyspace; k++)
			if (rnd_below(3))
			{
				a.present[k] = b.present[k] = true;
				a.version[k] = b.version[k] = k;
			}
		for (size_t i = 0; i < changes; i++)
		{
			uint64_t	k = rnd_below(keyspace);

			b.present[k] = rnd_below(4) != 0;
			b.version[k] = rnd();
		}
		ra = model_build(&ctx, &a);
		rb = model_build(&ctx, &b);
		for (int dir = 0; dir < 2; dir++)
		{
			x.from = dir ? &b : &a;
			x.to = dir ? &a : &b;
			x.next = 0;
			x.n = 0;
			prolly_diff(&ctx, dir ? &rb : &ra, dir ? &ra : &rb, diff_check_fn, &x);
			for (; x.next < keyspace; x.next++)
				CHECK(x.from->present[x.next] == x.to->present[x.next] &&
					  (!x.from->present[x.next] ||
					   x.from->version[x.next] == x.to->version[x.next]));
		}
		model_free(&a);
		model_free(&b);
	}
	cs_destroy(cs);
}

static void
test_merge(void)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = {cs, small_policy};
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
	ours = prolly_apply(&ctx, &base, o.e, o.n);
	theirs = prolly_apply(&ctx, &base, t.e, t.n);
	expect = prolly_apply(&ctx, &base, all.e, all.n);
	prolly_merge(&ctx, &base, &ours, &theirs, &mr);
	CHECK_EQ(mr.nconflicts, 0);
	CHECK(phash_eq(&mr.root, &expect));
	prolly_merge_result_free(&mr);
	prolly_merge(&ctx, &base, &theirs, &ours, &mr);
	CHECK_EQ(mr.nconflicts, 0);
	CHECK(phash_eq(&mr.root, &expect));
	prolly_merge_result_free(&mr);

	/* trivial merges */
	prolly_merge(&ctx, &base, &base, &theirs, &mr);
	CHECK(phash_eq(&mr.root, &theirs) && mr.nconflicts == 0);
	prolly_merge_result_free(&mr);
	prolly_merge(&ctx, &base, &ours, &base, &mr);
	CHECK(phash_eq(&mr.root, &ours) && mr.nconflicts == 0);
	prolly_merge_result_free(&mr);

	/* update/update and delete/update conflicts keep ours */
	o.n = t.n = 0;
	batch_put(&o, 100, 1);
	batch_put(&t, 100, 2);
	batch_del(&o, 200);
	batch_put(&t, 200, 2);
	batch_put(&t, 300, 2);
	ours = prolly_apply(&ctx, &base, o.e, o.n);
	theirs = prolly_apply(&ctx, &base, t.e, t.n);
	prolly_merge(&ctx, &base, &ours, &theirs, &mr);
	CHECK_EQ(mr.nconflicts, 2);
	CHECK(get_u64(mr.conflicts[0].key) == 100 && get_u64(mr.conflicts[1].key) == 200);
	CHECK(get_u64(mr.conflicts[0].ours + 8) == 1 && get_u64(mr.conflicts[0].theirs + 8) == 2);
	CHECK(mr.conflicts[1].ours == NULL && mr.conflicts[1].base != NULL);
	{
		const uint8_t *v;
		size_t		vl;
		uint8_t		key[8];

		put_u64(key, 300);
		CHECK(prolly_get(&ctx, &mr.root, key, 8, &v, &vl) && get_u64(v + 8) == 2);
		put_u64(key, 100);
		CHECK(prolly_get(&ctx, &mr.root, key, 8, &v, &vl) && get_u64(v + 8) == 1);
	}
	prolly_merge_result_free(&mr);
	batch_free(&o);
	batch_free(&t);
	batch_free(&all);
	cs_destroy(cs);
}

/* ---------------------------------------------------------------- scaling */

typedef struct ScaleResult
{
	uint64_t	n;
	int			height;
	uint64_t	chunks;			/* chunks in the base tree */
	double		get_reads;		/* per lookup */
	uint64_t	count_reads;
	uint64_t	edit1_reads;
	uint64_t	edit1_writes;
	uint64_t	editk_reads;
	uint64_t	editk_writes;
	uint64_t	diff_reads;
	uint64_t	merge_reads;
	uint64_t	merge_writes;
	double		build_sec;
	double		editk_usec;
} ScaleResult;

#define SCALE_K 100

static void
random_edits(EditBatch *b, uint64_t n, size_t k, uint64_t version)
{
	b->n = 0;
	for (size_t i = 0; i < k; i++)
	{
		uint64_t	k2 = rnd_below(2 * n);

		switch (rnd_below(3))
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
	(*(uint64_t *) arg)++;
}

static void
scale_case(uint64_t n, ScaleResult *r)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = {cs, prolly_default_policy};
	PHash		root,
				edited,
				ours,
				theirs;
	ChunkStoreStats s;
	EditBatch	b;
	double		t;
	uint64_t	ndiff = 0;
	ProllyMergeResult mr;

	memset(r, 0, sizeof(*r));
	rng_seed(n);
	r->n = n;
	t = now_sec();
	root = build_even_rows(&ctx, n);
	r->build_sec = now_sec() - t;
	r->chunks = cs->ops->count(cs);
	r->height = prolly_height(&ctx, &root);
	batch_init(&b, 2 * SCALE_K);

	s = cs->stats;
	for (int i = 0; i < 1000; i++)
	{
		uint8_t		key[8];
		const uint8_t *v;
		size_t		vl;

		put_u64(key, rnd_below(n) * 2);
		CHECK(prolly_get(&ctx, &root, key, 8, &v, &vl));
	}
	r->get_reads = STATS_DELTA(cs, s, gets) / 1000.0;

	s = cs->stats;
	CHECK_EQ(prolly_count(&ctx, &root), n);
	r->count_reads = STATS_DELTA(cs, s, gets);

	/* single-row update, averaged over 50 rows */
	s = cs->stats;
	for (int i = 0; i < 50; i++)
	{
		b.n = 0;
		batch_put(&b, rnd_below(n) * 2, 1000 + (uint64_t) i);
		edited = prolly_apply(&ctx, &root, b.e, b.n);
	}
	r->edit1_reads = STATS_DELTA(cs, s, gets) / 50;
	r->edit1_writes = STATS_DELTA(cs, s, new_chunks) / 50;

	/* a batch of k random inserts, updates and deletes */
	random_edits(&b, n, SCALE_K, 1);
	s = cs->stats;
	t = now_sec();
	edited = prolly_apply(&ctx, &root, b.e, b.n);
	r->editk_usec = (now_sec() - t) * 1e6;
	r->editk_reads = STATS_DELTA(cs, s, gets);
	r->editk_writes = STATS_DELTA(cs, s, new_chunks);

	s = cs->stats;
	prolly_diff(&ctx, &root, &edited, count_fn, &ndiff);
	r->diff_reads = STATS_DELTA(cs, s, gets);
	CHECK(ndiff > 0 && ndiff <= SCALE_K);

	/* three-way merge of k changes on each side */
	random_edits(&b, n, SCALE_K, 2);
	ours = prolly_apply(&ctx, &root, b.e, b.n);
	random_edits(&b, n, SCALE_K, 3);
	theirs = prolly_apply(&ctx, &root, b.e, b.n);
	s = cs->stats;
	prolly_merge(&ctx, &root, &ours, &theirs, &mr);
	r->merge_reads = STATS_DELTA(cs, s, gets);
	r->merge_writes = STATS_DELTA(cs, s, new_chunks);
	prolly_merge_result_free(&mr);

	batch_free(&b);
	cs_destroy(cs);
}

static void
check_scale_bounds(const ScaleResult *r)
{
	uint64_t	h = (uint64_t) r->height;

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
				uint64_t small_cost, uint64_t big_cost, const char *what)
{
	double		hratio = (double) big->height / small->height;
	double		allowed = 2.0 * hratio * (double) (small_cost ? small_cost : 1);

	if ((double) big_cost > allowed)
	{
		fprintf(stderr, "\n%s grew with n: %llu at n=%llu vs %llu at n=%llu\n",
				what, (unsigned long long) big_cost, (unsigned long long) big->n,
				(unsigned long long) small_cost, (unsigned long long) small->n);
		exit(1);
	}
}

static void
test_scaling(void)
{
	static const uint64_t sizes[] = {10000, 100000, 1000000};
	ScaleResult r[3];

	printf("\n");
	printf("  %9s %3s %7s %6s %5s %10s %10s %10s %9s %10s %10s %8s\n",
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
		printf("  %9llu %3d %7llu %6.1f %5llu %10s %10s %10.1f %9llu %10s %10.2f\n",
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
	printf("%-44s", "");
}

/* Structural sharing: a new version stores only the chunks it changed. */
static void
test_structural_sharing(void)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = {cs, prolly_default_policy};
	PHash		root = build_even_rows(&ctx, 200000);
	uint64_t	base_bytes = cs->ops->bytes(cs);
	uint64_t	h = (uint64_t) prolly_height(&ctx, &root);
	EditBatch	b;

	rng_seed(9);
	batch_init(&b, 10);
	for (int v = 0; v < 100; v++)
	{
		ChunkStoreStats s = cs->stats;

		random_edits(&b, 200000, 10, (uint64_t) v + 10);
		root = prolly_apply(&ctx, &root, b.e, b.n);
		CHECK_LE(STATS_DELTA(cs, s, new_chunks), 3 * 10 * h);
	}
	/* 100 versions cost a few full copies' worth, not 100 */
	CHECK_LE(cs->ops->bytes(cs) - base_bytes, base_bytes * 5);
	batch_free(&b);
	cs_destroy(cs);
}

int
main(void)
{
	RUN(test_sha256_vectors);
	RUN(test_empty_map);
	RUN(test_random_edits_small_chunks);
	RUN(test_random_edits_default_chunks);
	RUN(test_single_key_edits);
	RUN(test_history_independence);
	RUN(test_iter_from_key);
	RUN(test_diff_matches_model);
	RUN(test_merge);
	RUN(test_structural_sharing);
	RUN(test_scaling);
	printf("test_prolly: all tests passed\n");
	return 0;
}
