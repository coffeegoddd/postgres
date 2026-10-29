/*
 * test_rewrite.c
 *	  History mode REWRITE: a sever lets the cut history go at once and
 *	  writes the kept commits again without it, so the oldest kept commits
 *	  become roots.  Their rows, times and messages stay; their descendants
 *	  get new hashes, and heads, merge heads, images and the hook follow.
 */
#include "test_repo.h"

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
static const PHash *
moved(const Moves *m, const PHash *old)
{
	for (int i = 0; i < m->n; i++)
		if (phash_eq(&m->old[i], old))
			return &m->now[i];
	return old;
}

typedef struct Where
{
	int			window,
				other;
} Where;

static void
where_fn(void *arg, const PHash *h, VcLocation l, uint32_t seg)
{
	Where	   *w = arg;

	if (l == VC_LOC_WINDOW)
		w->window++;
	else
		w->other++;
}

static void
rewriting(VcRepo *r, ChunkStore *cs, Moves *m)
{
	setup(r, cs);
	vc_set_history_mode(r, VC_HISTORY_REWRITE);
	memset(m, 0, sizeof(*m));
	vc_set_rewrite_hook(r, hook, m);
}

/* init <- A <- B <- C <- D, a window of two: C is a root now, and the rest is gone. */
static void
test_cut_commit_becomes_root(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	Moves		m;
	Linear		l;
	Snap		sc,
				sd;
	uint32_t	seg;
	LogAcc		acc;
	VcCommit	c;
	Where		w = {0};
	PHash		c2,
				d2;

	rewriting(&r, cs, &m);
	l = build_linear(&r);
	sc = snap(&r.ctx, &l.c);
	sd = snap(&r.ctx, &l.d);
	CHECK(vc_sever(&r, &depth2, &seg) == VC_OK);
	CHECK(seg == VC_NO_SEGMENT);
	CHECK_EQ(r.nsegs, 0);

	/* C and D were written again, and the hook heard both */
	CHECK_EQ(m.n, 2);
	c2 = *moved(&m, &l.c);
	d2 = *moved(&m, &l.d);
	CHECK(!phash_eq(&c2, &l.c) && !phash_eq(&d2, &l.d));
	CHECK(phash_eq(&vc_head(&r)->head, &d2));

	/* the log ends at C, cleanly: C has no parents */
	CHECK(log_from(&r, &d2, 0, &acc) == VC_OK);
	CHECK_EQ(acc.n, 2);
	CHECK(phash_eq(&acc.v[1], &c2));
	CHECK(vc_commit_load(&r.ctx, &c2, &c));
	CHECK_EQ(c.nparents, 0);
	CHECK_EQ(c.generation, 1);
	CHECK(vc_commit_load(&r.ctx, &d2, &c));
	CHECK(c.nparents == 1 && phash_eq(&c.parents[0], &c2) && c.generation == 2);

	/* the same rows, as of the same commits */
	check_snap(&r.ctx, &c2, &sc);
	check_snap(&r.ctx, &d2, &sd);

	/* nothing else is known: no segment, no boundary, no cut */
	vc_locations(&r, where_fn, &w);
	CHECK_EQ(w.window, 2);
	CHECK_EQ(w.other, 0);
	CHECK_EQ(r.nheld, 2);

	/* and nothing of the old history is in the store */
	cs_collect_orphans(cs);
	CHECK(!cs_has(cs, &l.init) && !cs_has(cs, &l.a) && !cs_has(cs, &l.b));
	CHECK(!cs_has(cs, &l.c) && !cs_has(cs, &l.d));
	check_store(cs, 0);
	free(sc.v);
	free(sd.v);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* Severing every commit, the store holds the window and no more. */
static void
test_store_stays_bounded(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	Moves		m;
	size_t		at100 = 0;
	uint32_t	seg;
	LogAcc		acc;

	rewriting(&r, cs, &m);
	vc_set_rewrite_hook(&r, NULL, NULL);
	commit_rows(&r, 0, 2000, 0, "load");
	for (int i = 1; i <= 300; i++)
	{
		commit_rows(&r, (uint64_t) (i * 7) % 2000, (uint64_t) (i * 7) % 2000 + 5, (uint64_t) i, "step");
		CHECK(vc_sever(&r, &(VcWindow) {.depth = 10}, &seg) == VC_OK);
		cs_collect_orphans(cs);
		if (i == 100)
			at100 = cs->ops->count(cs);
	}
	CHECK(log_from(&r, &vc_head(&r)->head, 0, &acc) == VC_OK);
	CHECK_EQ(acc.n, 10);
	CHECK_EQ(r.nheld, 10);
	/* the window is ten commits whatever the history's length */
	CHECK_LE(cs->ops->count(cs), at100 + at100 / 10);
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/*
 * Two branches over shared history: the shared commit is written once and
 * both are written on it, so they still have a merge base and merge.
 */
static void
test_branches_share_the_new_root(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	Moves		m;
	Linear		l;
	PHash		f,
				base;
	uint32_t	seg;
	VcCommit	cf,
				cd;
	VcMergeResult res;

	rewriting(&r, cs, &m);
	l = build_linear(&r);
	CHECK(vc_branch_create(&r, "feature", &l.c) == VC_OK);
	CHECK(vc_checkout(&r, "feature") == VC_OK);
	f = commit_rows(&r, 30000, 30010, 9, "feature");
	CHECK(vc_checkout(&r, "main") == VC_OK);
	CHECK(vc_sever(&r, &depth2, &seg) == VC_OK);

	CHECK(vc_commit_load(&r.ctx, &vc_branch_find(&r, "feature")->head, &cf));
	CHECK(vc_commit_load(&r.ctx, &vc_head(&r)->head, &cd));
	CHECK(phash_eq(&vc_branch_find(&r, "feature")->head, moved(&m, &f)));
	CHECK(cf.nparents == 1 && cd.nparents == 1);
	CHECK(phash_eq(&cf.parents[0], &cd.parents[0]));
	CHECK(phash_eq(&cf.parents[0], moved(&m, &l.c)));
	CHECK(vc_merge_base_in(&r, &vc_head(&r)->head, &vc_branch_find(&r, "feature")->head, 0,
						   &base) == 1);
	CHECK(phash_eq(&base, moved(&m, &l.c)));
	CHECK(vc_merge(&r, "feature", &res) == VC_OK);
	CHECK(res.status == VC_MERGE_COMMITTED);
	vc_merge_result_free(&res);
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* A merge commit with one parent cut keeps the other. */
static void
test_merge_commit_loses_its_cut_parent(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	Moves		m;
	PHash		a,
				mh;
	uint32_t	seg;
	VcCommit	c;
	VcMergeResult res;

	rewriting(&r, cs, &m);
	a = commit_rows(&r, 0, 100, 1, "a");
	CHECK(vc_branch_create(&r, "side", &a) == VC_OK);
	CHECK(vc_checkout(&r, "side") == VC_OK);
	commit_rows(&r, 1000, 1010, 2, "side");
	CHECK(vc_checkout(&r, "main") == VC_OK);
	for (int i = 0; i < 4; i++)
		commit_rows(&r, (uint64_t) i, (uint64_t) i + 1, (uint64_t) (10 + i), "main");
	CHECK(vc_merge(&r, "side", &res) == VC_OK && res.status == VC_MERGE_COMMITTED);
	mh = res.commit;
	vc_merge_result_free(&res);
	CHECK(vc_branch_delete(&r, "side") == VC_OK);
	/* the merge and its main parent stay; the side commit (generation 3) goes */
	CHECK(vc_sever(&r, &depth2, &seg) == VC_OK);
	CHECK(vc_commit_load(&r.ctx, &vc_head(&r)->head, &c));
	CHECK(phash_eq(&c.hash, moved(&m, &mh)));
	CHECK_EQ(c.nparents, 1);
	CHECK(vc_commit_load(&r.ctx, &c.parents[0], &c));
	CHECK_EQ(c.nparents, 0);
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* Nothing cut, nothing written: every hash stays. */
static void
test_nothing_cut_nothing_moves(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	Moves		m;
	Linear		l;
	uint32_t	seg;

	rewriting(&r, cs, &m);
	l = build_linear(&r);
	CHECK(vc_sever(&r, &(VcWindow) {.depth = 10}, &seg) == VC_OK);
	CHECK_EQ(m.n, 0);
	CHECK(phash_eq(&vc_head(&r)->head, &l.d));
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* Images and a pending merge head follow their commits. */
static void
test_images_and_merge_heads_follow(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	Moves		m;
	Linear		l;
	PHash		img,
				got,
				x;
	uint32_t	seg;
	VcMergeResult res;
	VcBranch   *b;

	rewriting(&r, cs, &m);
	l = build_linear(&r);
	/* any chunk will do as an image's hold here */
	{
		VcCommit	c;

		CHECK(vc_commit_load(&r.ctx, &l.d, &c));
		img = c.root;
	}
	CHECK(vc_image_set(&r, &l.d, &img) == VC_OK);
	/* a merge left pending by a conflict: both sides set row 5 */
	CHECK(vc_branch_create(&r, "x", &l.c) == VC_OK);
	CHECK(vc_checkout(&r, "x") == VC_OK);
	x = commit_rows(&r, 5, 6, 77, "x");
	CHECK(vc_checkout(&r, "main") == VC_OK);
	commit_rows(&r, 5, 6, 88, "main");
	CHECK(vc_merge(&r, "x", &res) == VC_OK);
	CHECK(res.status == VC_MERGE_CONFLICTS);
	vc_merge_result_free(&res);
	CHECK(vc_branch_delete(&r, "x") == VC_OK);
	b = vc_head(&r);
	CHECK(b->merging && phash_eq(&b->merge_head, &x));

	CHECK(vc_sever(&r, &depth2, &seg) == VC_OK);
	CHECK(phash_eq(&vc_head(&r)->merge_head, moved(&m, &x)));
	CHECK(!phash_eq(moved(&m, &x), &x));
	CHECK(!vc_image_get(&r, &l.d, &got));
	CHECK(vc_image_get(&r, moved(&m, &l.d), &got) && phash_eq(&got, &img));
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/*
 * What the window measures from a head is unchanged by a rewrite: change
 * bytes and generations after each commit, and its time.
 */
static void
test_measures_stay(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	Moves		m;
	PHash		h[12];
	VcCommit	before[12],
				after,
				H0,
				H1;
	uint32_t	seg;

	rewriting(&r, cs, &m);
	for (int i = 0; i < 12; i++)
		h[i] = commit_rows(&r, (uint64_t) i * 100, (uint64_t) i * 100 + 20 + i, (uint64_t) i, "m");
	for (int i = 0; i < 12; i++)
		CHECK(vc_commit_load(&r.ctx, &h[i], &before[i]));
	H0 = before[11];
	CHECK(vc_sever(&r, &(VcWindow) {.depth = 6}, &seg) == VC_OK);
	CHECK(vc_commit_load(&r.ctx, &vc_head(&r)->head, &H1));
	for (int i = 6; i < 12; i++)
	{
		CHECK(vc_commit_load(&r.ctx, moved(&m, &h[i]), &after));
		CHECK_EQ(H1.bytes - after.bytes, H0.bytes - before[i].bytes);
		CHECK_EQ(H1.generation - after.generation, H0.generation - before[i].generation);
		CHECK_EQ(after.time, before[i].time);
		CHECK(phash_eq(&after.root, &before[i].root));
	}
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* The incremental window stays exact across rewrites. */
static void
test_tracked_window(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	Moves		m;
	uint32_t	seg;

	rewriting(&r, cs, &m);
	CHECK(vc_window_track(&r, 3) == VC_OK);
	commit_rows(&r, 0, 100, 0, "load");
	for (int i = 1; i <= 30; i++)
	{
		commit_rows(&r, (uint64_t) i, (uint64_t) i + 1, (uint64_t) i, "step");
		CHECK(vc_sever(&r, &(VcWindow) {.depth = 3}, &seg) == VC_OK);
		CHECK(vc_window_verify(&r));
	}
	CHECK_EQ(r.nheld, 3);
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* Saved and reopened, the mode stays, and severing goes on rewriting. */
static void
test_save_and_reopen(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	Moves		m;
	PHash		st;
	uint32_t	seg;
	LogAcc		acc;

	rewriting(&r, cs, &m);
	build_linear(&r);
	CHECK(vc_sever(&r, &depth2, &seg) == VC_OK);
	st = vc_repo_save(&r);
	vc_repo_free(&r);
	CHECK(vc_repo_open(&r, cs, &small_policy, &st) == VC_OK);
	CHECK(r.history == VC_HISTORY_REWRITE);
	commit_rows(&r, 0, 10, 50, "after");
	CHECK(vc_sever(&r, &depth2, &seg) == VC_OK);
	CHECK(seg == VC_NO_SEGMENT);
	CHECK(log_from(&r, &vc_head(&r)->head, 0, &acc) == VC_OK);
	CHECK_EQ(acc.n, 2);
	cs_collect_orphans(cs);
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* Hashes that do not last cannot be shared: no remotes either way. */
static void
test_no_remotes(void)
{
	ChunkStore *ca = memstore_create(),
			   *cb = memstore_create(),
			   *cc = memstore_create();
	VcRepo		a,
				b,
				c;
	Moves		m;
	PHash		head;

	rewriting(&a, ca, &m);
	commit_rows(&a, 0, 10, 1, "a");
	setup(&b, cb);
	CHECK(vc_push(&a, &b, "main") == VC_ERR_REWRITTEN);
	CHECK(vc_fetch(&b, &a, "main", &head) == VC_ERR_REWRITTEN);
	CHECK(vc_fetch(&a, &b, "main", &head) == VC_ERR_REWRITTEN);
	CHECK(vc_repo_clone(&c, cc, &a, 0) == VC_ERR_REWRITTEN);
	vc_repo_free(&a);
	vc_repo_free(&b);
	cs_destroy(ca);
	cs_destroy(cb);
	cs_destroy(cc);
}

int
main(void)
{
	RUN(test_cut_commit_becomes_root);
	RUN(test_store_stays_bounded);
	RUN(test_branches_share_the_new_root);
	RUN(test_merge_commit_loses_its_cut_parent);
	RUN(test_nothing_cut_nothing_moves);
	RUN(test_images_and_merge_heads_follow);
	RUN(test_measures_stay);
	RUN(test_tracked_window);
	RUN(test_save_and_reopen);
	RUN(test_no_remotes);
	printf("test_rewrite: all tests passed\n");
	return 0;
}
