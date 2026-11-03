/*-------------------------------------------------------------------------
 *
 * unit_engine_rewrite.c
 *	  Unit tests of the history mode REWRITE: a sever lets the cut history
 *	  go at once and writes the kept commits again without it, so the
 *	  oldest kept commits become roots.  Their rows, times and messages
 *	  stay; their descendants get new hashes, and heads, merge heads and
 *	  the hook follow.  The mode is the repository's, and it has no
 *	  remotes.
 *
 * They run on the memory store, with the memory references, ending its
 * holds as a transaction's end would to see what a sever let go of; and on
 * the history store, with its rows, but for what only ended holds show.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/test/modules/test_prolly/unit_engine_rewrite.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "engine_test_util.h"

static const char *ddl = "CREATE TABLE t (id int8 PRIMARY KEY, v int8)";

static const VcWindow depth2 = {.depth = 2};

/* ---------------------------------------------------------------- helpers */

static PHash
branch_head(VcRepo *r, const char *name)
{
	VcBranch	b;
	PHash		h;

	CHECK(vc_branch_get(r, name, &b));
	h = b.head;
	vc_branch_free(&b);
	return h;
}

static PHash
head_of(VcRepo *r)
{
	VcBranch	b;
	PHash		h;

	vc_head(r, &b);
	h = b.head;
	vc_branch_free(&b);
	return h;
}

static PHash
commit_rows(VcRepo *r, uint64 lo, uint64 hi, uint64 ver, const char *msg)
{
	EditBatch	b;
	PHash		rv = vc_working_lock(r),
				out;

	batch_init(&b, hi - lo);
	for (uint64 k = lo; k < hi; k++)
		batch_put(&b, k, ver);
	CHECK_EQ(vc_table_edit(&r->ctx, &rv, "t", b.e, b.n, &out), VC_OK);
	vc_set_working(r, &out);
	batch_free(&b);
	return vc_commit(r, msg);
}

static void
setup(VcRepo *r, ChunkStore *cs)
{
	PHash		rv,
				out;

	vc_repo_open(r, cs, t_refs(cs), &small_policy);
	rv = vc_working_lock(r);
	CHECK_EQ(vc_table_create(&r->ctx, &rv, "t", ddl, strlen(ddl), &out), VC_OK);
	vc_set_working(r, &out);
}

typedef struct Moves
{
	PHash		old[256],
				now[256];
	int			n;
} Moves;

static void
hook(void *arg, const PHash *old, const PHash *now)
{
	Moves	   *m = arg;

	CHECK(m->n < 256);
	m->old[m->n] = *old;
	m->now[m->n] = *now;
	m->n++;
}

/* Where a commit went: its new hash, or itself if it was not written again. */
static PHash
moved(const Moves *m, const PHash *old)
{
	for (int i = 0; i < m->n; i++)
		if (phash_eq(&m->old[i], old))
			return m->now[i];
	return *old;
}

/* A repository in REWRITE mode, with table t made, the hook listening. */
static void
rewriting(VcRepo *r, ChunkStore *cs, Moves *m)
{
	setup(r, cs);
	vc_set_history_mode(r, VC_HISTORY_REWRITE);
	memset(m, 0, sizeof(*m));
	vc_set_rewrite_hook(r, hook, m);
}

typedef struct Where
{
	int			window,
				other;
} Where;

static void
where_fn(void *arg, const PHash *h, VcLocation l, uint32 seg)
{
	Where	   *w = arg;

	if (l == VC_LOC_WINDOW)
		w->window++;
	else
		w->other++;
}

static int
window_size(VcRepo *r)
{
	Where		w = {0};

	vc_locations(r, where_fn, &w);
	return w.window;
}

typedef struct LogAcc
{
	PHash		v[64];
	int			n;
} LogAcc;

static void
log_fn(void *arg, const VcCommit *c)
{
	LogAcc	   *a = arg;

	CHECK(a->n < 64);
	a->v[a->n++] = c->hash;
}

static int
log_from(VcRepo *r, const PHash *start, LogAcc *acc)
{
	acc->n = 0;
	return vc_log(r, start, 64, 0, log_fn, acc);
}

#define KEYS 40000

static int64 *
snap(ProllyCtx *ctx, const PHash *commit)
{
	int64	   *ver = palloc(KEYS * sizeof(int64));
	VcCommit	c;
	VcTable		t;
	ProllyIter *it;
	const uint8 *k,
			   *v;
	size_t		kl,
				vl;

	CHECK(vc_commit_load(ctx, commit, &c));
	CHECK(vc_table_get(ctx, &c.root, "t", &t));
	vc_commit_release(&c);
	for (uint64 i = 0; i < KEYS; i++)
		ver[i] = -1;
	it = t_iter(ctx, &t.data, NULL, 0);
	while (t_next(it, &k, &kl, &v, &vl))
		ver[get_u64(k)] = (int64) get_u64(v + 8);
	prolly_iter_close(it);
	return ver;
}

static void
check_snap(ProllyCtx *ctx, const PHash *commit, const int64 *want)
{
	int64	   *got = snap(ctx, commit);

	CHECK(memcmp(got, want, KEYS * sizeof(int64)) == 0);
	pfree(got);
}

/* What the store frees as its holds end (as the transaction's end would). */
static uint64
release(ChunkStore *cs)
{
	if (!engine_test_on_memory())
		return 0;				/* (held until the test's subtransaction ends) */
	MemoryChunkStoreEndHolds(cs);
	return cs_free_unheld(cs);
}

static uint64
settled_count(ChunkStore *cs)
{
	uint64		chunks,
				unheld;
	char	   *why = NULL;

	(void) release(cs);
	CHECK(cs_check(cs, &chunks, &unheld, &why));
	return chunks;
}

static bool
has(ChunkStore *cs, const PHash *h)
{
	return cs_has(cs, h->b);
}

/* init <- A <- B <- C <- D, each a modest change over a 20000-row table. */
typedef struct Linear
{
	PHash		init,
				a,
				b,
				c,
				d;
} Linear;

static Linear
build_linear(VcRepo *r)
{
	Linear		l;

	l.init = head_of(r);
	l.a = commit_rows(r, 0, 20000, 0, "A");
	l.b = commit_rows(r, 0, 300, 1, "B");
	l.c = commit_rows(r, 10000, 10300, 2, "C");
	l.d = commit_rows(r, 19000, 19300, 3, "D");
	return l;
}

/* ------------------------------------------------------------------ tests */

/* init <- A <- B <- C <- D, a window of two: C is a root now, and the rest is gone. */
ENGINE_TEST(test_cut_commit_becomes_root)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	Moves		m;
	Linear		l;
	int64	   *sc,
			   *sd;
	uint32		seg;
	LogAcc		acc;
	VcCommit	c;
	Where		w = {0};
	PHash		c2,
				d2,
				h;

	rewriting(&r, cs, &m);
	l = build_linear(&r);
	sc = snap(&r.ctx, &l.c);
	sd = snap(&r.ctx, &l.d);
	CHECK_EQ(vc_sever(&r, &depth2, &seg), VC_OK);
	CHECK(seg == VC_NO_SEGMENT);
	CHECK(!vc_segment_get(&r, 0, &(VcSegment) {0}));

	/* C and D were written again, and the hook heard both */
	CHECK_EQ(m.n, 2);
	c2 = moved(&m, &l.c);
	d2 = moved(&m, &l.d);
	CHECK(!phash_eq(&c2, &l.c) && !phash_eq(&d2, &l.d));
	h = branch_head(&r, "main");
	CHECK(phash_eq(&h, &d2));

	/* the log ends at C, cleanly: C has no parents */
	CHECK_EQ(log_from(&r, &d2, &acc), VC_OK);
	CHECK_EQ(acc.n, 2);
	CHECK(phash_eq(&acc.v[1], &c2));
	CHECK(vc_commit_load(&r.ctx, &c2, &c));
	CHECK_EQ(c.nparents, 0);
	CHECK_EQ(c.generation, 1);
	vc_commit_release(&c);
	CHECK(vc_commit_load(&r.ctx, &d2, &c));
	CHECK(c.nparents == 1 && phash_eq(&c.parents[0], &c2) && c.generation == 2);
	vc_commit_release(&c);

	/* the same rows, as of the same commits */
	check_snap(&r.ctx, &c2, sc);
	check_snap(&r.ctx, &d2, sd);

	/* nothing else is known: no segment, no boundary, no cut */
	vc_locations(&r, where_fn, &w);
	CHECK_EQ(w.window, 2);
	CHECK_EQ(w.other, 0);

	/* and nothing of the old history is in the store (once its holds end) */
	if (engine_test_on_memory())
	{
		(void) release(cs);
		CHECK(!has(cs, &l.init) && !has(cs, &l.a) && !has(cs, &l.b));
		CHECK(!has(cs, &l.c) && !has(cs, &l.d));
	}
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* Severing every commit, the store holds the window and no more. */
ENGINE_TEST(test_store_stays_bounded)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	Moves		m;
	uint64		at50 = 0;
	uint32		seg;
	LogAcc		acc;
	PHash		h;

	rewriting(&r, cs, &m);
	vc_set_rewrite_hook(&r, NULL, NULL);
	commit_rows(&r, 0, 2000, 0, "load");
	for (int i = 1; i <= 150; i++)
	{
		commit_rows(&r, (uint64) (i * 7) % 2000, (uint64) (i * 7) % 2000 + 5, (uint64) i,
					"step");
		CHECK_EQ(vc_sever(&r, &(VcWindow) {.depth = 10}, &seg), VC_OK);
		if (i == 50)
			at50 = settled_count(cs);
	}
	h = head_of(&r);
	CHECK_EQ(log_from(&r, &h, &acc), VC_OK);
	CHECK_EQ(acc.n, 10);
	CHECK_EQ(window_size(&r), 10);
	/* the window is ten commits whatever the history's length (the store
	 * the window's size once what it let go of is freed) */
	if (engine_test_on_memory())
		CHECK_LE(settled_count(cs), at50 + at50 / 10);
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/*
 * Two branches over shared history: the shared commit is written once and
 * both are written on it, so they still have a merge base and merge.
 */
ENGINE_TEST(test_branches_share_the_new_root)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	Moves		m;
	Linear		l;
	PHash		f,
				fh,
				mh,
				base,
				c2;
	uint32		seg;
	VcCommit	cf,
				cd;
	VcMergeResult res;

	rewriting(&r, cs, &m);
	l = build_linear(&r);
	CHECK_EQ(vc_branch_create(&r, "feature", &l.c), VC_OK);
	CHECK_EQ(vc_checkout(&r, "feature"), VC_OK);
	f = commit_rows(&r, 30000, 30010, 9, "feature");
	CHECK_EQ(vc_checkout(&r, "main"), VC_OK);
	CHECK_EQ(vc_sever(&r, &depth2, &seg), VC_OK);

	fh = branch_head(&r, "feature");
	mh = head_of(&r);
	c2 = moved(&m, &l.c);
	{
		PHash		f2 = moved(&m, &f);

		CHECK(phash_eq(&fh, &f2));
	}
	CHECK(vc_commit_load(&r.ctx, &fh, &cf));
	CHECK(vc_commit_load(&r.ctx, &mh, &cd));
	CHECK(cf.nparents == 1 && cd.nparents == 1);
	CHECK(phash_eq(&cf.parents[0], &cd.parents[0]));
	CHECK(phash_eq(&cf.parents[0], &c2));
	vc_commit_release(&cf);
	vc_commit_release(&cd);
	CHECK_EQ(vc_merge_base_in(&r, &mh, &fh, 0, &base), 1);
	CHECK(phash_eq(&base, &c2));
	CHECK_EQ(vc_merge(&r, "feature", &res), VC_OK);
	CHECK(res.status == VC_MERGE_COMMITTED);
	vc_merge_result_free(&res);
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* A merge commit with one parent cut keeps the other. */
ENGINE_TEST(test_merge_commit_loses_its_cut_parent)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	Moves		m;
	PHash		a,
				mh,
				h,
				p;
	uint32		seg;
	VcCommit	c;
	VcMergeResult res;

	rewriting(&r, cs, &m);
	a = commit_rows(&r, 0, 100, 1, "a");
	CHECK_EQ(vc_branch_create(&r, "side", &a), VC_OK);
	CHECK_EQ(vc_checkout(&r, "side"), VC_OK);
	commit_rows(&r, 1000, 1010, 2, "side");
	CHECK_EQ(vc_checkout(&r, "main"), VC_OK);
	for (int i = 0; i < 4; i++)
		commit_rows(&r, (uint64) i, (uint64) i + 1, (uint64) (10 + i), "main");
	CHECK_EQ(vc_merge(&r, "side", &res), VC_OK);
	CHECK(res.status == VC_MERGE_COMMITTED);
	mh = res.commit;
	vc_merge_result_free(&res);
	CHECK_EQ(vc_branch_delete(&r, "side"), VC_OK);
	/* the merge and its main parent stay; the side commit (generation 3) goes */
	CHECK_EQ(vc_sever(&r, &depth2, &seg), VC_OK);
	h = head_of(&r);
	{
		PHash		m2 = moved(&m, &mh);

		CHECK(phash_eq(&h, &m2));
	}
	CHECK(vc_commit_load(&r.ctx, &h, &c));
	CHECK_EQ(c.nparents, 1);
	p = c.parents[0];
	vc_commit_release(&c);
	CHECK(vc_commit_load(&r.ctx, &p, &c));
	CHECK_EQ(c.nparents, 0);
	vc_commit_release(&c);
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* Nothing cut, nothing written: every hash stays. */
ENGINE_TEST(test_nothing_cut_nothing_moves)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	Moves		m;
	Linear		l;
	uint32		seg;
	PHash		h;

	rewriting(&r, cs, &m);
	l = build_linear(&r);
	CHECK_EQ(vc_sever(&r, &(VcWindow) {.depth = 10}, &seg), VC_OK);
	CHECK_EQ(m.n, 0);
	h = head_of(&r);
	CHECK(phash_eq(&h, &l.d));
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* A pending merge head follows its commit. */
ENGINE_TEST(test_merge_head_follows)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	Moves		m;
	Linear		l;
	PHash		x,
				x2;
	uint32		seg;
	VcMergeResult res;
	VcBranch	b;

	rewriting(&r, cs, &m);
	l = build_linear(&r);
	/* a merge left pending by a conflict: both sides set row 5 */
	CHECK_EQ(vc_branch_create(&r, "x", &l.c), VC_OK);
	CHECK_EQ(vc_checkout(&r, "x"), VC_OK);
	x = commit_rows(&r, 5, 6, 77, "x");
	CHECK_EQ(vc_checkout(&r, "main"), VC_OK);
	commit_rows(&r, 5, 6, 88, "main");
	CHECK_EQ(vc_merge(&r, "x", &res), VC_OK);
	CHECK(res.status == VC_MERGE_CONFLICTS);
	vc_merge_result_free(&res);
	CHECK_EQ(vc_branch_delete(&r, "x"), VC_OK);
	vc_head(&r, &b);
	CHECK(b.merging && phash_eq(&b.merge_head, &x));
	vc_branch_free(&b);

	CHECK_EQ(vc_sever(&r, &depth2, &seg), VC_OK);
	x2 = moved(&m, &x);
	CHECK(!phash_eq(&x2, &x));
	vc_head(&r, &b);
	CHECK(b.merging && phash_eq(&b.merge_head, &x2));
	vc_branch_free(&b);
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/*
 * A pinned commit with cut history beneath it is replaced as any: the hook
 * names its new hash, which a later sever's pin names to keep it; the old
 * hash, gone, is no commit to pin (VC_ERR_SEVERED).
 */
ENGINE_TEST(test_pinned_follows_the_hook)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	Moves		m;
	Linear		l;
	uint32		seg;
	PHash		b2;
	VcWindow	w = {.depth = 1,.npinned = 1};

	rewriting(&r, cs, &m);
	l = build_linear(&r);
	w.pinned = &l.b;
	CHECK_EQ(vc_sever(&r, &w, &seg), VC_OK);
	b2 = moved(&m, &l.b);
	CHECK(!phash_eq(&b2, &l.b));
	CHECK(vc_in_window(&r, &b2));
	CHECK(!vc_in_window(&r, &l.b));
	m.n = 0;
	commit_rows(&r, 0, 10, 50, "E");
	w.pinned = &b2;
	CHECK_EQ(vc_sever(&r, &w, &seg), VC_OK);
	CHECK(vc_in_window(&r, &b2));	/* a root: nothing beneath it to cut */
	CHECK_EQ(window_size(&r), 2);
	/* the old hash, gone (its transaction's holds ended), is no commit */
	if (engine_test_on_memory())
	{
		(void) release(cs);
		w.pinned = &l.b;
		CHECK_EQ(vc_sever(&r, &w, &seg), VC_ERR_SEVERED);
		CHECK(vc_in_window(&r, &b2));
	}
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/*
 * Severs that cut at most one commit each (max_cut) write again only what
 * the window keeps: commits outside it not cut yet keep their hashes, for
 * a later sever to cut.
 */
ENGINE_TEST(test_max_cut_leaves_the_rest)
{
	/*
	 * Of init, A and B, one is cut a sever, whichever the listing (by hash)
	 * comes to first: the others, outside the window and not cut, keep
	 * their hashes meanwhile; once B is cut, C and D are written again,
	 * once.  (Repeated: in one order of the three of six, B, A, init, no
	 * commit outside the window has a parent cut before it.)
	 */
	for (int run = 0; run < 8; run++)
	{
		ChunkStore *cs = memstore_create();
		VcRepo		r;
		Moves		m;
		Linear		l;
		uint32		seg;
		VcWindow	w = {.depth = 2,.max_cut = 1};

		rewriting(&r, cs, &m);
		l = build_linear(&r);
		for (int i = 0; i < 3; i++)
		{
			int			held = 0;

			CHECK_EQ(vc_sever(&r, &w, &seg), VC_OK);
			held += vc_in_window(&r, &l.init) ? 1 : 0;
			held += vc_in_window(&r, &l.a) ? 1 : 0;
			held += vc_in_window(&r, &l.b) ? 1 : 0;
			CHECK_EQ(held, 2 - i);
		}
		CHECK_EQ(m.n, 2);
		CHECK_EQ(window_size(&r), 2);
		settle_and_check(cs);
		vc_repo_free(&r);
		cs_destroy(cs);
	}
}

/*
 * What the window measures from a head is unchanged by a rewrite: change
 * bytes and generations after each commit, and its time.
 */
ENGINE_TEST(test_measures_stay)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	Moves		m;
	PHash		h[12],
				head;
	VcCommit	before[12],
				after,
				H0,
				H1;
	uint32		seg;

	rewriting(&r, cs, &m);
	for (int i = 0; i < 12; i++)
		h[i] = commit_rows(&r, (uint64) i * 100, (uint64) i * 100 + 20 + i, (uint64) i, "m");
	for (int i = 0; i < 12; i++)
		CHECK(vc_commit_load(&r.ctx, &h[i], &before[i]));
	H0 = before[11];
	CHECK_EQ(vc_sever(&r, &(VcWindow) {.depth = 6}, &seg), VC_OK);
	head = head_of(&r);
	CHECK(vc_commit_load(&r.ctx, &head, &H1));
	for (int i = 6; i < 12; i++)
	{
		PHash		now = moved(&m, &h[i]);

		CHECK(vc_commit_load(&r.ctx, &now, &after));
		CHECK_EQ(H1.bytes - after.bytes, H0.bytes - before[i].bytes);
		CHECK_EQ(H1.generation - after.generation, H0.generation - before[i].generation);
		CHECK_EQ(after.time, before[i].time);
		CHECK(phash_eq(&after.root, &before[i].root));
		vc_commit_release(&after);
	}
	vc_commit_release(&H1);
	for (int i = 0; i < 12; i++)
		vc_commit_release(&before[i]);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/*
 * The mode is the repository's: another view of it (another backend's)
 * sees it, and its severs rewrite; set back to KEEP, they make segments.
 */
ENGINE_TEST(test_mode_is_the_repository)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r,
				other;
	Moves		m;
	uint32		seg;
	LogAcc		acc;
	PHash		h;

	rewriting(&r, cs, &m);
	build_linear(&r);
	vc_repo_open(&other, cs, t_refs(cs), &small_policy);
	CHECK(vc_history_mode(&other) == VC_HISTORY_REWRITE);
	CHECK_EQ(vc_sever(&other, &depth2, &seg), VC_OK);
	CHECK(seg == VC_NO_SEGMENT);
	CHECK_EQ(m.n, 0);			/* (the hook is r's alone) */
	h = head_of(&r);
	CHECK_EQ(log_from(&r, &h, &acc), VC_OK);
	CHECK_EQ(acc.n, 2);

	vc_set_history_mode(&other, VC_HISTORY_KEEP);
	CHECK(vc_history_mode(&r) == VC_HISTORY_KEEP);
	commit_rows(&r, 0, 10, 50, "after");
	CHECK_EQ(vc_sever(&r, &depth2, &seg), VC_OK);
	CHECK(seg != VC_NO_SEGMENT);
	settle_and_check(cs);
	vc_repo_free(&other);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* Hashes that do not last cannot be shared: no remotes either way. */
ENGINE_TEST(test_no_remotes)
{
	ChunkStore *ca = memstore_create(),
			   *cb = memstore_create_own(),
			   *cc = memstore_create_own();
	VcRepo		a,
				b,
				c;
	Moves		m;
	PHash		head;
	VcMergeResult res;
	VcSource	src;
	uint32		seg;

	rewriting(&a, ca, &m);
	vc_repo_set_name(&a, "a");
	commit_rows(&a, 0, 10, 1, "a");
	setup(&b, cb);
	vc_repo_set_name(&b, "b");
	CHECK_EQ(vc_push(&a, &b, "main"), VC_ERR_REWRITTEN);
	CHECK_EQ(vc_push(&b, &a, "main"), VC_ERR_REWRITTEN);
	CHECK_EQ(vc_fetch(&b, &a, "main", &head), VC_ERR_REWRITTEN);
	CHECK_EQ(vc_fetch(&a, &b, "main", &head), VC_ERR_REWRITTEN);
	CHECK_EQ(vc_pull(&b, &a, "main", 0, &res), VC_ERR_REWRITTEN);
	CHECK_EQ(vc_pull(&a, &b, "main", 0, &res), VC_ERR_REWRITTEN);
	src.name = "b";
	src.cs = cb;
	head = head_of(&b);
	CHECK_EQ(vc_deepen(&a, &src, &head, 0, &seg), VC_ERR_REWRITTEN);
	CHECK_EQ(vc_repo_clone(&c, cc, t_refs(cc), &small_policy, &a, 0), VC_ERR_REWRITTEN);
	/* (nothing left behind: no repository made in the clone's store) */
	CHECK(!vc_refs_get(t_refs(cc), "vc/repo", VC_REF_READ, &head));
	settle_and_check(cb);
	vc_repo_free(&c);
	vc_repo_free(&a);
	vc_repo_free(&b);
	cs_destroy(ca);
}

/*
 * Low marks: each cut writes again only what the low mark keeps, so a
 * tumbling window (low_commits 1) writes one commit per batch, the head,
 * which becomes the root; between cuts nothing is written.  A low mark of
 * three writes three, the oldest a root.
 */
static void
marks_rewrite(uint64 low)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	Moves		m;
	uint32		seg;
	VcWindow	w = {.max_commits = 8,.low_commits = low};
	int			cuts = 0;
	uint64		held;

	rewriting(&r, cs, &m);
	commit_rows(&r, 0, 2000, 0, "load");
	CHECK_EQ(vc_sever(&r, &w, &seg), VC_OK);
	held = (uint64) window_size(&r);
	for (int i = 1; i <= 40; i++)
	{
		int			before = m.n;

		commit_rows(&r, (uint64) (i * 7) % 2000, (uint64) (i * 7) % 2000 + 5, (uint64) i,
					"step");
		CHECK_EQ(vc_sever(&r, &w, &seg), VC_OK);
		CHECK(seg == VC_NO_SEGMENT);
		if (held + 1 <= 8)
		{
			/* filling: nothing written again */
			held++;
			CHECK_EQ(m.n, before);
		}
		else
		{
			LogAcc		acc;
			VcCommit	c;
			PHash		h = head_of(&r);

			/* cut: the low mark's commits written again, the oldest a root */
			held = low;
			cuts++;
			CHECK_EQ(m.n - before, (int) low);
			CHECK_EQ(log_from(&r, &h, &acc), VC_OK);
			CHECK_EQ(acc.n, (int) low);
			CHECK(vc_commit_load(&r.ctx, &acc.v[acc.n - 1], &c));
			CHECK_EQ(c.nparents, 0);
			CHECK_EQ(c.generation, 1);
			vc_commit_release(&c);
		}
		CHECK_EQ((uint64) window_size(&r), held);
		m.n = 0;
	}
	CHECK_LE(4, cuts);
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

ENGINE_TEST(test_low_marks)
{
	marks_rewrite(1);
	marks_rewrite(3);
}

static const ProllyUnitTest tests[] = {
	{"cut_commit_becomes_root", test_cut_commit_becomes_root},
	{"store_stays_bounded", test_store_stays_bounded},
	{"branches_share_the_new_root", test_branches_share_the_new_root},
	{"merge_commit_loses_its_cut_parent", test_merge_commit_loses_its_cut_parent},
	{"nothing_cut_nothing_moves", test_nothing_cut_nothing_moves},
	{"merge_head_follows", test_merge_head_follows},
	{"measures_stay", test_measures_stay},
	{"pinned_follows_the_hook", test_pinned_follows_the_hook},
	{"max_cut_leaves_the_rest", test_max_cut_leaves_the_rest},
	{"mode_is_the_repository", test_mode_is_the_repository},
	{"no_remotes", test_no_remotes},
	{"low_marks", test_low_marks},
};

const ProllyUnitSuite unit_engine_rewrite_suite = {"engine_rewrite", tests, lengthof(tests)};
