/*
 * test_window.c
 *	  The history window: severing commits into segments, exporting them
 *	  whole to an archive, discarding them, attaching them again, branching
 *	  and merge shapes across the window's edge, stopping at the edge,
 *	  remotes, and the cost of each.
 */
#include "test_repo.h"

/* ------------------------------------------------------------------ tests */

/* Severing A <- B to keep C <- D moves holds only: nothing copied or freed. */
static void
test_detach_moves_holds_only(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	Linear		l;
	uint32_t	seg,
				seg2;
	ChunkStoreStats st;
	size_t		count;
	LogAcc		acc;
	PHash		mb;
	Snap		sb;
	int			n = 0;

	setup(&r, cs);
	l = build_linear(&r);
	sb = snap(&r.ctx, &l.b);
	count = cs->ops->count(cs);
	st = cs->stats;
	CHECK(vc_sever(&r, &depth2, &seg) == VC_OK && seg != VC_NO_SEGMENT);
	CHECK_EQ(STATS_DELTA(cs, st, frees), 0);
	CHECK_EQ(STATS_DELTA(cs, st, new_chunks), 0);
	CHECK_LE(STATS_DELTA(cs, st, gets), 2 * depth2.depth + 2);	/* the window only */
	CHECK_EQ(cs->ops->count(cs), count);
	check_store(cs, 0);

	CHECK(vc_locate(&r, &l.d, NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&r, &l.c, NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&r, &l.b, NULL) == VC_LOC_SEGMENT);
	CHECK(vc_locate(&r, &l.a, NULL) == VC_LOC_SEGMENT);
	CHECK(vc_locate(&r, &l.init, NULL) == VC_LOC_SEGMENT);
	CHECK_EQ(r.segs[seg].n, 3);

	/* still local: reads past the edge work, unless told to stop there */
	CHECK(log_from(&r, &l.d, 0, &acc) == VC_OK && acc.n == 5);
	CHECK(log_from(&r, &l.d, VC_WINDOW_ONLY, &acc) == VC_ERR_OUTSIDE_WINDOW);
	CHECK(acc.n == 2 && phash_eq(&r.ctx.err_hash, &l.b));
	CHECK(vc_merge_base_in(&r, &l.d, &l.b, 0, &mb) == 1 && phash_eq(&mb, &l.b));
	CHECK(vc_merge_base_in(&r, &l.d, &l.b, VC_WINDOW_ONLY, &mb) == VC_ERR_OUTSIDE_WINDOW);
	CHECK(vc_merge_base_in(&r, &l.d, &l.c, VC_WINDOW_ONLY, &mb) == 1 && phash_eq(&mb, &l.c));
	CHECK(vc_diff_commits(&r, &l.b, &l.d, 0, count_tables_fn, &n) == VC_OK && n == 1);
	CHECK(vc_diff_commits(&r, &l.b, &l.d, VC_WINDOW_ONLY, count_tables_fn, &n) == VC_ERR_OUTSIDE_WINDOW);
	CHECK(vc_diff_commits(&r, &l.c, &l.d, VC_WINDOW_ONLY, count_tables_fn, &n) == VC_OK);
	check_snap(&r.ctx, &l.b, &sb);

	/* nothing new to cut: a second sever makes no segment */
	CHECK(vc_sever(&r, &depth2, &seg2) == VC_OK && seg2 == VC_NO_SEGMENT);

	/* attach: the holds move back, still nothing copied or freed */
	st = cs->stats;
	CHECK(vc_segment_attach(&r, seg) == VC_OK);
	CHECK_EQ(STATS_DELTA(cs, st, frees) + STATS_DELTA(cs, st, new_chunks), 0);
	CHECK(vc_locate(&r, &l.a, NULL) == VC_LOC_WINDOW);
	CHECK(log_from(&r, &l.d, VC_WINDOW_ONLY, &acc) == VC_OK && acc.n == 5);
	check_store(cs, 0);

	free(sb.v);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* Export: a self-contained A <- B in the archive, freed here, exactly. */
static void
test_export_self_contained(void)
{
	ChunkStore *cs = memstore_create();
	ChunkStore *archive = memstore_create();
	ProllyCtx	actx;
	VcRepo		r;
	Linear		l;
	uint32_t	seg;
	Snap		sa,
				sb;
	size_t		want;
	ChunkStoreStats st;
	LogAcc		acc;
	PHash		mb;
	int			n = 0;
	uint64_t	diff_before,
				freed;

	setup(&r, cs);
	l = build_linear(&r);
	sa = snap(&r.ctx, &l.a);
	sb = snap(&r.ctx, &l.b);
	st = cs->stats;
	CHECK(vc_diff_commits(&r, &l.c, &l.d, 0, count_tables_fn, &n) == VC_OK);
	diff_before = STATS_DELTA(cs, st, gets);

	CHECK(vc_sever(&r, &depth2, &seg) == VC_OK);
	want = expected_frees(&r, seg);
	CHECK(want > 0);
	st = cs->stats;
	CHECK(vc_segment_export(&r, seg, &(VcSource) {"archive", archive}) == VC_OK);
	freed = STATS_DELTA(cs, st, frees);
	CHECK_EQ(freed, want);
	check_store(cs, 0);
	CHECK(vc_locate(&r, &l.b, NULL) == VC_LOC_EXPORTED);

	/* the archive alone holds A and B whole */
	prolly_ctx_init(&actx, archive, &small_policy);
	check_snap(&actx, &l.a, &sa);
	check_snap(&actx, &l.b, &sb);
	CHECK(archive->ops->has(archive, &l.init));
	CHECK(!archive->ops->has(archive, &l.c));
	check_store(archive, 0);

	/* here, history ends at the cut */
	CHECK(log_from(&r, &l.d, 0, &acc) == VC_ERR_SEVERED);
	CHECK(acc.n == 2 && phash_eq(&r.ctx.err_hash, &l.b));
	CHECK(vc_merge_base_in(&r, &l.d, &l.b, 0, &mb) == VC_ERR_SEVERED);
	CHECK(vc_diff_commits(&r, &l.a, &l.d, 0, count_tables_fn, &n) == VC_ERR_SEVERED);
	CHECK(vc_reset_hard(&r, &l.b) == VC_ERR_OUTSIDE_WINDOW);

	/* work inside the window costs what it did */
	st = cs->stats;
	CHECK(vc_diff_commits(&r, &l.c, &l.d, VC_WINDOW_ONLY, count_tables_fn, &n) == VC_OK);
	CHECK_EQ(STATS_DELTA(cs, st, gets), diff_before);

	/* attach from the archive: exactly what was freed comes back */
	st = cs->stats;
	CHECK(vc_segment_attach(&r, seg) == VC_OK);
	CHECK_EQ(STATS_DELTA(cs, st, new_chunks), freed);
	check_store(cs, 0);
	check_snap(&r.ctx, &l.b, &sb);
	CHECK(log_from(&r, &l.d, VC_WINDOW_ONLY, &acc) == VC_OK && acc.n == 5);
	CHECK(phash_eq(&acc.v[2], &l.b) && phash_eq(&acc.v[4], &l.init));

	free(sa.v);
	free(sb.v);
	prolly_ctx_free(&actx);
	vc_repo_free(&r);
	cs_destroy(cs);
	cs_destroy(archive);
}

/* Sliding the window and exporting again sends only what changed. */
static void
test_second_export_copies_diff(void)
{
	ChunkStore *cs = memstore_create();
	ChunkStore *archive = memstore_create();
	ProllyCtx	actx;
	VcRepo		r;
	Linear		l;
	uint32_t	s1,
				s2;
	uint64_t	first,
				second;
	ChunkStoreStats st;
	Snap		sc,
				sd;
	int			h;

	setup(&r, cs);
	l = build_linear(&r);
	CHECK(vc_sever(&r, &depth2, &s1) == VC_OK);
	st = archive->stats;
	CHECK(vc_segment_export(&r, s1, &(VcSource) {"archive", archive}) == VC_OK);
	first = STATS_DELTA(archive, st, new_chunks);

	commit_rows(&r, 5000, 5100, 4, "E");
	commit_rows(&r, 7000, 7100, 5, "F");
	sc = snap(&r.ctx, &l.c);
	sd = snap(&r.ctx, &l.d);
	CHECK(vc_sever(&r, &depth2, &s2) == VC_OK && s2 != VC_NO_SEGMENT);
	CHECK_EQ(r.segs[s2].n, 2);	/* C and D */
	st = archive->stats;
	CHECK(vc_segment_export(&r, s2, &(VcSource) {"archive", archive}) == VC_OK);
	second = STATS_DELTA(archive, st, new_chunks);
	{
		VcTable		t;
		VcCommit	c;

		CHECK(vc_commit_load(&r.ctx, &vc_head(&r)->head, &c));
		CHECK(vc_table_get(&r.ctx, &c.root, "t", &t));
		h = t_height(&r.ctx, &t.data);
	}
	/* two commits of 300 rows each: about rows x height chunks, not the table */
	CHECK_LE(second, 2 * 3 * 300 * (uint64_t) h + 20);
	CHECK_LE(second * 4, first);

	prolly_ctx_init(&actx, archive, &small_policy);
	check_snap(&actx, &l.c, &sc);
	check_snap(&actx, &l.d, &sd);
	check_store(archive, 0);
	check_store(cs, 0);

	free(sc.v);
	free(sd.v);
	prolly_ctx_free(&actx);
	vc_repo_free(&r);
	cs_destroy(cs);
	cs_destroy(archive);
}

static void
test_discard_frees_exactly(void)
{
	ChunkStore *cs = memstore_create();
	ChunkStore *archive = memstore_create();
	VcRepo		r;
	Linear		l;
	uint32_t	seg;
	size_t		want;
	ChunkStoreStats st;
	LogAcc		acc;
	Snap		sd;

	setup(&r, cs);
	l = build_linear(&r);
	sd = snap(&r.ctx, &l.d);
	CHECK(vc_sever(&r, &depth2, &seg) == VC_OK);
	want = expected_frees(&r, seg);
	st = cs->stats;
	CHECK(vc_segment_discard(&r, seg) == VC_OK);
	CHECK_EQ(STATS_DELTA(cs, st, frees), want);
	check_store(cs, 0);
	/* C names B, so B is known as discarded; A, below it, is forgotten */
	CHECK(vc_locate(&r, &l.b, NULL) == VC_LOC_DISCARDED);
	CHECK(vc_locate(&r, &l.a, NULL) == VC_LOC_UNKNOWN);
	CHECK_EQ(r.segs[seg].n, 0);
	check_snap(&r.ctx, &l.d, &sd);
	CHECK(log_from(&r, &l.d, 0, &acc) == VC_ERR_SEVERED && acc.n == 2);
	CHECK(vc_segment_attach(&r, seg) == VC_ERR_SEVERED);
	CHECK(vc_segment_export(&r, seg, &(VcSource) {"archive", archive}) == VC_ERR);
	CHECK(vc_segment_discard(&r, seg) == VC_ERR);

	free(sd.v);
	vc_repo_free(&r);
	cs_destroy(cs);
	cs_destroy(archive);
}

/*
 * main:    init <- A <- B <- C <- D
 * feature:              B <- E <- F
 * With depth 2 the window is {C, D, E, F}; both branches cut at B.
 */
static void
test_branches_cut_at_shared_ancestor(void)
{
	ChunkStore *cs = memstore_create();
	ChunkStore *archive = memstore_create();
	VcRepo		r;
	PHash		init,
				a,
				b,
				c,
				d,
				e,
				f,
				mb;
	uint32_t	seg;
	LogAcc		acc;
	VcMergeResult m;
	PHash		working;

	setup(&r, cs);
	init = vc_head(&r)->head;
	a = commit_rows(&r, 0, 20000, 0, "A");
	b = commit_rows(&r, 0, 200, 1, "B");
	CHECK(vc_branch_create(&r, "feature", NULL) == VC_OK);
	c = commit_rows(&r, 3000, 3200, 2, "C");
	d = commit_rows(&r, 6000, 6200, 3, "D");
	vc_checkout(&r, "feature");
	e = commit_rows(&r, 12000, 12200, 4, "E");
	f = commit_rows(&r, 15000, 15200, 5, "F");
	vc_checkout(&r, "main");

	CHECK(vc_sever(&r, &depth2, &seg) == VC_OK);
	CHECK(vc_locate(&r, &c, NULL) == VC_LOC_WINDOW && vc_locate(&r, &d, NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&r, &e, NULL) == VC_LOC_WINDOW && vc_locate(&r, &f, NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&r, &b, NULL) == VC_LOC_SEGMENT && vc_locate(&r, &a, NULL) == VC_LOC_SEGMENT);
	CHECK(vc_locate(&r, &init, NULL) == VC_LOC_SEGMENT);
	check_store(cs, 0);

	/* both branches stop at the shared ancestor */
	CHECK(log_from(&r, &f, VC_WINDOW_ONLY, &acc) == VC_ERR_OUTSIDE_WINDOW && acc.n == 2);
	CHECK(phash_eq(&r.ctx.err_hash, &b));
	CHECK(log_from(&r, &d, VC_WINDOW_ONLY, &acc) == VC_ERR_OUTSIDE_WINDOW && acc.n == 2);
	/* window-only: the base is named, but it lies past the edge */
	CHECK(vc_merge_base_in(&r, &d, &f, VC_WINDOW_ONLY, &mb) == VC_ERR_OUTSIDE_WINDOW);
	CHECK(phash_eq(&r.ctx.err_hash, &b));
	CHECK(vc_merge_base_in(&r, &d, &f, 0, &mb) == 1 && phash_eq(&mb, &b));

	/* exported: merging the branches needs their base, which is gone */
	CHECK(vc_segment_export(&r, seg, &(VcSource) {"archive", archive}) == VC_OK);
	check_store(cs, 0);
	working = vc_working(&r);
	CHECK(vc_merge(&r, "feature", &m) == VC_ERR_SEVERED);
	CHECK(phash_eq(&r.ctx.err_hash, &b));
	CHECK(phash_eq(&working, &vc_head(&r)->working) && !vc_head(&r)->merging);

	/* attach from the archive and the merge goes through */
	CHECK(vc_segment_attach(&r, seg) == VC_OK);
	CHECK(vc_merge(&r, "feature", &m) == VC_OK && m.status == VC_MERGE_COMMITTED);
	vc_merge_result_free(&m);
	{
		PHash		w = vc_working(&r);
		VcTable		t;
		const uint8_t *v;
		size_t		vl;
		uint8_t		k[8];

		CHECK(vc_table_get(&r.ctx, &w, "t", &t));
		put_u64(k, 15000);
		CHECK(t_get(&r.ctx, &t.data, k, 8, &v, &vl) && get_u64(v + 8) == 5);
		put_u64(k, 6000);
		CHECK(t_get(&r.ctx, &t.data, k, 8, &v, &vl) && get_u64(v + 8) == 3);
	}
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
	cs_destroy(archive);
}

/*
 * A branch whose head is old keeps that head in the window; the commits
 * between it and the window around main are cut.
 * old:  A;  main: init <- A <- B <- C <- D
 */
static void
test_stale_branch_head_stays(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	PHash		init,
				a,
				b,
				d,
				mb;
	uint32_t	seg;
	LogAcc		acc;
	Snap		sa;

	setup(&r, cs);
	init = vc_head(&r)->head;
	a = commit_rows(&r, 0, 20000, 0, "A");
	CHECK(vc_branch_create(&r, "old", NULL) == VC_OK);
	b = commit_rows(&r, 0, 500, 1, "B");
	commit_rows(&r, 8000, 8500, 2, "C");
	d = commit_rows(&r, 16000, 16500, 3, "D");
	sa = snap(&r.ctx, &a);

	CHECK(vc_sever(&r, &depth2, &seg) == VC_OK);
	/* old's window (its head and parent) keeps A and init; only B is cut */
	CHECK(vc_locate(&r, &a, NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&r, &init, NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&r, &b, NULL) == VC_LOC_SEGMENT);
	CHECK_EQ(r.segs[seg].n, 1);

	CHECK(vc_segment_discard(&r, seg) == VC_OK);
	check_store(cs, 0);
	vc_checkout(&r, "old");
	check_snap(&r.ctx, &a, &sa);
	CHECK(log_from(&r, &a, VC_WINDOW_ONLY, &acc) == VC_OK && acc.n == 2);
	CHECK(log_from(&r, &d, 0, &acc) == VC_ERR_SEVERED && acc.n == 2);
	CHECK(phash_eq(&r.ctx.err_hash, &b));
	/* A is D's ancestor only through B, which is gone: unknown, not "no" */
	CHECK(vc_merge_base_in(&r, &d, &a, 0, &mb) == VC_ERR_SEVERED);

	free(sa.v);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/*
 * A merge commit whose first parent is cut and second parent is a branch
 * head: M = merge(C, E), depth 1 keeps only M and E.
 */
static void
test_merge_commit_parents_across_edge(void)
{
	ChunkStore *cs = memstore_create();
	ChunkStore *archive = memstore_create();
	VcRepo		r;
	PHash		c,
				e,
				mm;
	uint32_t	seg;
	VcMergeResult m;
	VcCommit	cm;
	LogAcc		acc;
	VcWindow	depth1 = {.depth = 1};
	Snap		se;

	setup(&r, cs);
	commit_rows(&r, 0, 20000, 0, "A");
	commit_rows(&r, 0, 100, 1, "B");
	CHECK(vc_branch_create(&r, "feature", NULL) == VC_OK);
	c = commit_rows(&r, 4000, 4100, 2, "C");
	vc_checkout(&r, "feature");
	e = commit_rows(&r, 9000, 9100, 3, "E");
	se = snap(&r.ctx, &e);
	vc_checkout(&r, "main");
	CHECK(vc_merge(&r, "feature", &m) == VC_OK && m.status == VC_MERGE_COMMITTED);
	mm = m.commit;
	vc_merge_result_free(&m);

	CHECK(vc_sever(&r, &depth1, &seg) == VC_OK);
	CHECK(vc_locate(&r, &mm, NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&r, &e, NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&r, &c, NULL) == VC_LOC_SEGMENT);
	CHECK(vc_segment_export(&r, seg, &(VcSource) {"archive", archive}) == VC_OK);
	check_store(cs, 0);

	CHECK(vc_commit_load(&r.ctx, &mm, &cm) && cm.nparents == 2);
	CHECK(log_from(&r, &mm, 0, &acc) == VC_ERR_SEVERED && acc.n == 1);
	CHECK(phash_eq(&r.ctx.err_hash, &c));
	vc_checkout(&r, "feature");
	check_snap(&r.ctx, &e, &se);

	free(se.v);
	vc_repo_free(&r);
	cs_destroy(cs);
	cs_destroy(archive);
}

/* Overlapping windows on two branches cut only what neither covers. */
static void
test_overlapping_branch_windows(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	PHash		h[8];
	uint32_t	seg;
	VcWindow	depth3 = {.depth = 3};

	/* main: init, 1..6; side from 3: s1, s2 (stored in h[7] and r) */
	setup(&r, cs);
	h[0] = vc_head(&r)->head;
	h[1] = commit_rows(&r, 0, 20000, 0, "1");
	h[2] = commit_rows(&r, 0, 100, 1, "2");
	h[3] = commit_rows(&r, 100, 200, 2, "3");
	CHECK(vc_branch_create(&r, "side", NULL) == VC_OK);
	h[4] = commit_rows(&r, 200, 300, 3, "4");
	h[5] = commit_rows(&r, 300, 400, 4, "5");
	h[6] = commit_rows(&r, 400, 500, 5, "6");
	vc_checkout(&r, "side");
	commit_rows(&r, 30000, 30100, 6, "s1");
	h[7] = commit_rows(&r, 30100, 30200, 7, "s2");
	vc_checkout(&r, "main");

	/* main covers 4..6; side (s2 gen 6) covers s2, s1 and 3 */
	CHECK(vc_sever(&r, &depth3, &seg) == VC_OK);
	CHECK(vc_locate(&r, &h[6], NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&r, &h[4], NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&r, &h[3], NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&r, &h[7], NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&r, &h[2], NULL) == VC_LOC_SEGMENT);
	CHECK(vc_locate(&r, &h[1], NULL) == VC_LOC_SEGMENT);
	CHECK(vc_locate(&r, &h[0], NULL) == VC_LOC_SEGMENT);
	CHECK_EQ(r.segs[seg].n, 3);
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
}

static void
test_pinned_commit_stays(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	Linear		l;
	uint32_t	seg;
	VcWindow	w = {.depth = 1};

	setup(&r, cs);
	l = build_linear(&r);
	w.pinned = &l.a;
	w.npinned = 1;
	CHECK(vc_sever(&r, &w, &seg) == VC_OK);
	CHECK(vc_locate(&r, &l.a, NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&r, &l.d, NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&r, &l.b, NULL) == VC_LOC_SEGMENT);
	CHECK(vc_locate(&r, &l.c, NULL) == VC_LOC_SEGMENT);
	CHECK(vc_segment_discard(&r, seg) == VC_OK);
	check_store(cs, 0);
	CHECK(vc_branch_create(&r, "tag-a", &l.a) == VC_OK);
	vc_repo_free(&r);
	cs_destroy(cs);
}

static void
test_refuse_outside_window(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	Linear		l;
	uint32_t	seg;
	VcWindow	bad = {.depth = 0};

	setup(&r, cs);
	l = build_linear(&r);
	CHECK(vc_sever(&r, &bad, &seg) == VC_ERR);
	CHECK(vc_sever(&r, &depth2, &seg) == VC_OK);
	CHECK(vc_reset_hard(&r, &l.a) == VC_ERR_OUTSIDE_WINDOW);
	CHECK(vc_branch_create(&r, "x", &l.b) == VC_ERR_OUTSIDE_WINDOW);
	CHECK(vc_branch_find(&r, "x") == NULL);
	CHECK(vc_reset_hard(&r, &l.c) == VC_OK);
	CHECK(vc_segment_attach(&r, seg) == VC_OK);
	CHECK(vc_reset_hard(&r, &l.a) == VC_OK);
	CHECK(vc_branch_create(&r, "x", &l.b) == VC_OK);
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
}

static void
test_remotes_across_edge(void)
{
	ChunkStore *cs = memstore_create();
	ChunkStore *archive = memstore_create();
	ChunkStore *ocs = memstore_create();
	ChunkStore *ccs = memstore_create();
	ChunkStore *rcs = memstore_create();
	VcRepo		r,
				origin,
				clone,
				fresh;
	Linear		l;
	uint32_t	seg;
	LogAcc		acc;
	PHash		head,
				e;
	Snap		sd;
	VcMergeResult m;

	setup(&r, cs);
	l = build_linear(&r);
	sd = snap(&r.ctx, &l.d);
	CHECK(vc_repo_clone(&origin, ocs, &r, 0) == VC_OK);	/* full history */
	CHECK(vc_sever(&r, &depth2, &seg) == VC_OK);
	CHECK(vc_segment_export(&r, seg, &(VcSource) {"archive", archive}) == VC_OK);

	/* a clone of the severed repo knows B only as a boundary */
	CHECK(vc_repo_clone(&clone, ccs, &r, 0) == VC_OK);
	CHECK(vc_locate(&clone, &l.b, NULL) == VC_LOC_BOUNDARY);
	CHECK(vc_locate(&clone, &l.c, NULL) == VC_LOC_WINDOW);
	CHECK(!ccs->ops->has(ccs, &l.a));
	CHECK(log_from(&clone, &l.d, 0, &acc) == VC_ERR_SEVERED && acc.n == 2);
	check_snap(&clone.ctx, &l.d, &sd);
	check_store(ccs, 0);

	/*
	 * pushing to an unrelated remote: the walk reaches this repo's own cut
	 * before anything the remote knows, so attachment cannot be shown
	 */
	vc_repo_init(&fresh, rcs, &small_policy);
	CHECK(vc_branch_create(&r, "topic", NULL) == VC_OK);
	{
		size_t		before = rcs->ops->count(rcs);

		CHECK(vc_push(&r, &fresh, "topic") == VC_ERR_DETACHED);
		CHECK(phash_eq(&r.ctx.err_hash, &l.b));
		CHECK(strcmp(vc_hint(&r, &l.b), "archive") == 0);
		CHECK_EQ(rcs->ops->count(rcs), before);
		CHECK(vc_branch_find(&fresh, "topic") == NULL);
	}
	check_store(rcs, 0);

	/* fetching new work from a full-history remote needs nothing past the edge */
	e = commit_rows(&origin, 100, 200, 9, "E on origin");
	CHECK(vc_fetch(&r, &origin, "main", &head) == VC_OK && phash_eq(&head, &e));
	CHECK(vc_branch_create(&r, "origin/main", &head) == VC_OK);
	CHECK(vc_merge(&r, "origin/main", &m) == VC_OK && m.status == VC_MERGE_FAST_FORWARD);
	vc_merge_result_free(&m);
	CHECK(vc_locate(&r, &l.b, NULL) == VC_LOC_EXPORTED);

	/* and pushing back to it is a fast-forward found inside the window */
	commit_rows(&r, 300, 400, 10, "F");
	CHECK(vc_push(&r, &origin, "main") == VC_OK);
	check_store(ocs, 0);
	check_store(cs, 0);

	free(sd.v);
	vc_repo_free(&r);
	vc_repo_free(&origin);
	vc_repo_free(&clone);
	vc_repo_free(&fresh);
	cs_destroy(cs);
	cs_destroy(archive);
	cs_destroy(ocs);
	cs_destroy(ccs);
	cs_destroy(rcs);
}

/* ------------------------------------------------------ incremental window */

typedef struct OutsideArg
{
	PHashMap   *win;
	int			n;
} OutsideArg;

static void
outside_fn(void *arg, const PHash *h, VcLocation where, uint32_t seg)
{
	OutsideArg *a = arg;

	if (where == VC_LOC_WINDOW && !phmap_find(a->win, h))
		a->n++;
}

/* Commits a sever with this window would cut, computed from scratch. */
static int
full_cut_count(VcRepo *r, uint32_t depth)
{
	ChunkStore *cs2 = memstore_create();
	VcRepo		probe;
	int			n = 0;

	(void) cs2;
	(void) probe;
	/* the window as a set: every head's commits within depth */
	{
		PHashMap	win;

		phmap_init(&win);
		for (size_t i = 0; i < r->nbr; i++)
		{
			PHash	   *stack = px_malloc(sizeof(PHash) * 4096);
			uint64_t   *bound = px_malloc(sizeof(uint64_t) * 4096);
			size_t		sn = 0;
			VcCommit	c;

			CHECK(vc_commit_load(&r->ctx, &r->br[i].head, &c));
			stack[sn] = r->br[i].head;
			bound[sn++] = c.generation > depth ? c.generation - depth : 0;
			while (sn > 0)
			{
				PHash		h = stack[--sn];
				uint64_t	b = bound[sn];
				bool		created;

				phmap_ref(&win, &h, &created);
				if (!vc_commit_load(&r->ctx, &h, &c))
					continue;
				for (uint32_t p = 0; p < c.nparents; p++)
					if (c.parent_gen[p] > b && sn < 4096)
					{
						stack[sn] = c.parents[p];
						bound[sn++] = b;
					}
			}
			free(stack);
			free(bound);
		}
		{
			OutsideArg	a = {&win, 0};

			vc_locations(r, outside_fn, &a);
			n = a.n;
		}
		phmap_free(&win);
	}
	cs_destroy(cs2);
	return n;
}

/*
 * Random branch work with the window tracked incrementally: the counts
 * always equal a recomputation, and each incremental sever cuts exactly
 * what a full one would.
 */
static void
test_incremental_window_matches_full(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	const uint32_t depth = 3;
	int			nbranches = 1,
				severs = 0,
				cut_total = 0;
	char		name[32];

	setup(&r, cs);
	commit_rows(&r, 0, 2000, 0, "base");
	CHECK(vc_window_track(&r, depth) == VC_OK);
	CHECK(vc_window_verify(&r));
	rng_seed(8080);
	for (int step = 0; step < 400; step++)
	{
		uint64_t	op = rnd_below(100);

		if (op < 45)
		{
			/* commit on a random branch */
			size_t		b = rnd_below(r.nbr);

			CHECK(vc_checkout(&r, r.br[b].name) == VC_OK);
			commit_rows(&r, rnd_below(2000), rnd_below(2000) + 2000, (uint64_t) step, "c");
		}
		else if (op < 65)
		{
			/* branch at a random head or the current head */
			size_t		b = rnd_below(r.nbr);

			snprintf(name, sizeof(name), "b%d", nbranches++);
			CHECK(vc_branch_create(&r, name, &r.br[b].head) == VC_OK);
		}
		else if (op < 75 && r.nbr > 2)
		{
			size_t		b = rnd_below(r.nbr);

			if (b != r.cur)
				CHECK(vc_branch_delete(&r, r.br[b].name) == VC_OK);
		}
		else if (op < 82)
		{
			/* reset the current branch to another branch's head */
			size_t		b = rnd_below(r.nbr);

			CHECK(vc_reset_hard(&r, &r.br[b].head) == VC_OK);
		}
		else if (op < 90)
		{
			/* merge another branch in; a conflict is committed as ours */
			size_t		b = rnd_below(r.nbr);
			VcMergeResult m;
			int			rc;

			if (b == r.cur)
				continue;
			rc = vc_merge(&r, r.br[b].name, &m);
			if (rc == VC_ERR_SEVERED)
				continue;		/* their base was cut: correctly refused */
			CHECK(rc == VC_OK);
			if (m.status == VC_MERGE_CONFLICTS)
				vc_commit(&r, "resolve");
			vc_merge_result_free(&m);
		}
		else
		{
			/* sever: incremental, checked against a full recomputation */
			int			want = full_cut_count(&r, depth);
			uint32_t	seg;

			CHECK(vc_sever(&r, &(VcWindow) {.depth = depth}, &seg) == VC_OK);
			CHECK_EQ(seg == VC_NO_SEGMENT ? 0 : (int) r.segs[seg].n, want);
			if (seg != VC_NO_SEGMENT)
				CHECK(vc_segment_discard(&r, seg) == VC_OK);
			severs++;
			cut_total += want;
			CHECK_EQ(full_cut_count(&r, depth), 0);
		}
		CHECK(vc_window_verify(&r));
	}
	CHECK(severs > 20 && cut_total > 50);
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* Branches are found, created and deleted by name in O(1). */
static void
test_many_branches(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	char		name[32];
	double		t0;

	setup(&r, cs);
	commit_rows(&r, 0, 100, 0, "base");
	CHECK(vc_window_track(&r, 4) == VC_OK);
	t0 = now_sec();
	for (int i = 0; i < 100000; i++)
	{
		snprintf(name, sizeof(name), "br-%d", i);
		CHECK(vc_branch_create(&r, name, NULL) == VC_OK);
	}
	for (int i = 0; i < 100000; i += 3)
	{
		snprintf(name, sizeof(name), "br-%d", i);
		CHECK(vc_branch_delete(&r, name) == VC_OK);
	}
	for (int i = 0; i < 100000; i++)
	{
		snprintf(name, sizeof(name), "br-%d", i);
		CHECK((vc_branch_find(&r, name) != NULL) == (i % 3 != 0));
	}
	CHECK(vc_branch_find(&r, "main") != NULL);
	CHECK(vc_branch_delete(&r, "main") == VC_ERR);	/* the current branch */
	CHECK(vc_window_verify(&r));
	CHECK(now_sec() - t0 < 5.0);
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* ---------------------------------------------------------------- scaling */

typedef struct WinScale
{
	int			history;
	uint64_t	sever_reads;
	uint64_t	sever_frees_writes;
	uint64_t	first_export;
	uint64_t	slide_export;
	uint64_t	diff_reads_before;
	uint64_t	diff_reads_after;
	uint64_t	commit_writes_after;
} WinScale;

#define WS_ROWS 30000
#define WS_K 20

static void
win_scale_case(int history, WinScale *s)
{
	ChunkStore *cs = memstore_create();
	ChunkStore *archive = memstore_create();
	VcRepo		r;
	VcWindow	w = {.depth = 5};
	uint32_t	seg;
	ChunkStoreStats st;
	PHash		prev,
				last;
	int			n = 0;

	memset(s, 0, sizeof(*s));
	s->history = history;
	rng_seed((uint64_t) history);
	setup(&r, cs);
	commit_rows(&r, 0, WS_ROWS, 0, "load");
	for (int i = 0; i < history; i++)
	{
		EditBatch	b;
		PHash		rv = vc_working(&r),
					out;

		batch_init(&b, WS_K);
		for (int j = 0; j < WS_K; j++)
			batch_put(&b, rnd_below(WS_ROWS), (uint64_t) i + 1);
		CHECK(vc_table_edit(&r.ctx, &rv, "t", b.e, b.n, &out) == 0);
		vc_set_working(&r, &out);
		batch_free(&b);
		prev = vc_head(&r)->head;
		last = vc_commit(&r, "step");
	}
	st = cs->stats;
	CHECK(vc_diff_commits(&r, &prev, &last, 0, count_tables_fn, &n) == VC_OK);
	s->diff_reads_before = STATS_DELTA(cs, st, gets);

	st = cs->stats;
	CHECK(vc_sever(&r, &w, &seg) == VC_OK);
	s->sever_reads = STATS_DELTA(cs, st, gets);
	s->sever_frees_writes = STATS_DELTA(cs, st, frees) + STATS_DELTA(cs, st, new_chunks);
	st = archive->stats;
	CHECK(vc_segment_export(&r, seg, &(VcSource) {"archive", archive}) == VC_OK);
	s->first_export = STATS_DELTA(archive, st, new_chunks);

	/* slide the window by 5 commits and export again */
	for (int i = 0; i < 5; i++)
	{
		EditBatch	b;
		PHash		rv = vc_working(&r),
					out;

		batch_init(&b, WS_K);
		for (int j = 0; j < WS_K; j++)
			batch_put(&b, rnd_below(WS_ROWS), 1000 + (uint64_t) i);
		CHECK(vc_table_edit(&r.ctx, &rv, "t", b.e, b.n, &out) == 0);
		vc_set_working(&r, &out);
		batch_free(&b);
		prev = vc_head(&r)->head;
		st = cs->stats;
		last = vc_commit(&r, "slide");
		s->commit_writes_after = STATS_DELTA(cs, st, new_chunks);
	}
	CHECK(vc_sever(&r, &w, &seg) == VC_OK && seg != VC_NO_SEGMENT);
	CHECK_EQ(r.segs[seg].n, 5);
	st = archive->stats;
	CHECK(vc_segment_export(&r, seg, &(VcSource) {"archive", archive}) == VC_OK);
	s->slide_export = STATS_DELTA(archive, st, new_chunks);

	st = cs->stats;
	CHECK(vc_diff_commits(&r, &prev, &last, VC_WINDOW_ONLY, count_tables_fn, &n) == VC_OK);
	s->diff_reads_after = STATS_DELTA(cs, st, gets);
	check_store(cs, 0);
	check_store(archive, 0);

	vc_repo_free(&r);
	cs_destroy(cs);
	cs_destroy(archive);
}

static void
test_window_scaling(void)
{
	static const int histories[] = {20, 400};
	WinScale	s[2];

	printf("\n  %8s %10s %12s %12s %12s %11s %8s\n", "history",
		   "sever r", "sever f+w", "1st export", "slide exp", "diff r b/a", "commit w");
	for (int i = 0; i < 2; i++)
	{
		char		dr[32];

		win_scale_case(histories[i], &s[i]);
		snprintf(dr, sizeof(dr), "%llu/%llu",
				 (unsigned long long) s[i].diff_reads_before,
				 (unsigned long long) s[i].diff_reads_after);
		printf("  %8d %10llu %12llu %12llu %12llu %11s %8llu\n", s[i].history,
			   (unsigned long long) s[i].sever_reads,
			   (unsigned long long) s[i].sever_frees_writes,
			   (unsigned long long) s[i].first_export,
			   (unsigned long long) s[i].slide_export, dr,
			   (unsigned long long) s[i].commit_writes_after);

		/* detach reads the window only and touches no chunk */
		CHECK_LE(s[i].sever_reads, 2 * 5 + 2);
		CHECK_EQ(s[i].sever_frees_writes, 0);
		/* sliding 5 commits exports about 5 commits of change */
		CHECK_LE(s[i].slide_export, 5 * (3 * WS_K * 6 + 10));
		/* inside the window nothing got slower */
		CHECK_EQ(s[i].diff_reads_after, s[i].diff_reads_before);
		CHECK_EQ(s[i].commit_writes_after, 1);
	}
	/* 20x the history: severing and sliding cost the same */
	CHECK_EQ(s[1].sever_reads, s[0].sever_reads);
	CHECK_LE(s[1].slide_export, 2 * s[0].slide_export);
	printf("%-44s", "");
}

typedef struct CutCount
{
	int			discarded,
				window;
} CutCount;

static void
cut_count_fn(void *arg, const PHash *h, VcLocation where, uint32_t seg)
{
	CutCount   *c = arg;

	c->discarded += where == VC_LOC_DISCARDED;
	c->window += where == VC_LOC_WINDOW;
}

/*
 * The saved repo is the size of what it holds: commits cut and discarded
 * leave nothing in it, and a save writes what changed, not the history.
 * Reopened, the commit just past the oldest kept one is still known as
 * discarded; an older edge is forgotten once the commit naming it goes.
 * With tracked severs that edge is read from the kept commit itself; with
 * full ones the sever records it.
 */
static void
state_run(bool tracked)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	PHash		state,
				head,
				edge,
				first_edge = {{0}};
	size_t		len10 = 0,
				len = 0;
	uint64_t	wrote10 = 0,
				wrote = 0;
	uint32_t	seg;
	LogAcc		acc;
	CutCount	cc = {0};

	setup(&r, cs);
	if (tracked)
		CHECK(vc_window_track(&r, 5) == VC_OK);
	for (int i = 1; i <= 400; i++)
	{
		uint64_t	before;

		commit_rows(&r, (uint64_t) i, (uint64_t) i + 1, (uint64_t) i, "step");
		CHECK(vc_sever(&r, &(VcWindow) {.depth = 5}, &seg) == VC_OK);
		if (seg != VC_NO_SEGMENT)
		{
			CHECK(r.segs[seg].edge_known == !tracked);
			CHECK(vc_segment_discard(&r, seg) == VC_OK);
		}
		if (i == 10)
		{
			CHECK(log_from(&r, &vc_head(&r)->head, 0, &acc) == VC_ERR_SEVERED);
			first_edge = r.ctx.err_hash;
			CHECK(vc_locate(&r, &first_edge, NULL) == VC_LOC_DISCARDED);
		}
		before = cs->stats.new_bytes;
		state = vc_repo_save(&r);
		CHECK(cs_get(cs, &state, &len) != NULL);
		if (i == 10)
		{
			len10 = len;
			wrote10 = cs->stats.new_bytes - before;
		}
		wrote = cs->stats.new_bytes - before;
	}
	CHECK(len10 > 0);
	/* a varint grows a byte as the clock and segment ids move on */
	CHECK_LE(len, len10 + 16);
	/* a save writes a commit's worth of locations, not the history */
	CHECK_LE(wrote, 2 * wrote10 + 2048);
	head = vc_head(&r)->head;
	CHECK(log_from(&r, &head, 0, &acc) == VC_ERR_SEVERED && acc.n == 5);
	edge = r.ctx.err_hash;
	CHECK(vc_locate(&r, &edge, NULL) == VC_LOC_DISCARDED);
	CHECK(vc_locate(&r, &first_edge, NULL) == VC_LOC_UNKNOWN);
	vc_locations(&r, cut_count_fn, &cc);
	CHECK_EQ(cc.window, 5);
	CHECK_LE(cc.discarded, 1);
	vc_repo_free(&r);

	CHECK(vc_repo_open(&r, cs, &small_policy, &state) == VC_OK);
	/* ids are kept; every segment stands as discarded, holding nothing */
	for (size_t i = 0; i < r.nsegs; i++)
		CHECK(r.segs[i].state == VC_SEG_DISCARDED && r.segs[i].n == 0);
	CHECK(vc_locate(&r, &edge, NULL) == VC_LOC_DISCARDED);
	CHECK(vc_locate(&r, &first_edge, NULL) == VC_LOC_UNKNOWN);
	CHECK(vc_window_verify(&r));
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
}

static void
test_state_does_not_grow_with_history(void)
{
	state_run(true);
	state_run(false);
}

/* ---------------------------------------------------- floors and ceilings */

typedef struct FakeClock
{
	uint64_t	us;
} FakeClock;

static uint64_t
fake_now(void *arg)
{
	return ((FakeClock *) arg)->us;
}

#define SEC 1000000ULL

/*
 * Fake clocks start from the real one: a repo's first commit is made on
 * the system clock, and no commit's time goes back past its parent's.
 */
static uint64_t
clock_base(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t) ts.tv_sec * SEC + 1000 * SEC;
}

/* A commit of k random row changes on the current branch, after dt seconds. */
static PHash
timed_commit(VcRepo *r, FakeClock *clk, uint64_t dt, int k, uint64_t ver)
{
	EditBatch	b;
	PHash		rv = vc_working(r),
				out;

	clk->us += dt * SEC;
	batch_init(&b, (size_t) k);
	for (int i = 0; i < k; i++)
		batch_put(&b, rnd_below(KEYS), ver);
	CHECK(vc_table_edit(&r->ctx, &rv, "t", b.e, b.n, &out) == 0);
	vc_set_working(r, &out);
	batch_free(&b);
	return vc_commit(r, "timed");
}

static uint64_t
own_bytes(const VcCommit *c)
{
	uint64_t	pb = 0;

	for (uint32_t i = 0; i < c->nparents; i++)
		if (c->parent_bytes[i] > pb)
			pb = c->parent_bytes[i];
	return c->bytes - pb;
}

/*
 * One head's window straight from its definition, added to out: every
 * ancestor of the head (or the head itself) that a floor keeps and no
 * ceiling cuts.
 */
static void
expected_head(VcRepo *r, const PHash *head, const VcWindow *w, uint64_t now, PHashMap *out)
{
	VcCommit	H;
	PHashMap	anc;
	PHash	   *stack = px_malloc(sizeof(PHash) * 100000);
	size_t		n = 0;
	bool		created;

	CHECK(vc_commit_load(&r->ctx, head, &H));
	phmap_init(&anc);
	stack[n++] = *head;
	while (n > 0)
	{
		PHash		h = stack[--n];
		VcCommit	c;
		uint64_t	after,
					span;
		bool		floor,
					ceil;

		phmap_ref(&anc, &h, &created);
		if (!created || !vc_commit_load(&r->ctx, &h, &c))
			continue;
		after = H.bytes - c.bytes;
		span = after + own_bytes(&c);
		floor = (!w->depth && !w->keep_bytes && !w->keep_seconds) ||
			(w->depth && H.generation - c.generation < w->depth) ||
			(w->keep_bytes && after < w->keep_bytes) ||
			(w->keep_seconds && now - c.time < w->keep_seconds * SEC);
		ceil = (w->max_commits && H.generation - c.generation >= w->max_commits) ||
			(w->max_bytes && span > w->max_bytes) ||
			(w->max_seconds && now - c.time >= w->max_seconds * SEC);
		if (phash_eq(&h, head) || (floor && !ceil))
			phmap_ref(out, &h, &created);
		for (uint32_t j = 0; j < c.nparents; j++)
			stack[n++] = c.parents[j];
	}
	phmap_free(&anc);
	free(stack);
}

/* The window of every head, straight from its definition. */
static void
expected_window(VcRepo *r, const VcWindow *w, uint64_t now, PHashMap *out)
{
	phmap_init(out);
	for (size_t i = 0; i < r->nbr; i++)
		expected_head(r, &r->br[i].head, w, now, out);
}

typedef struct WinSet
{
	PHashMap	set;
} WinSet;

static void
window_set_fn(void *arg, const PHash *h, VcLocation where, uint32_t seg)
{
	bool		created;

	if (where == VC_LOC_WINDOW)
		phmap_ref(&((WinSet *) arg)->set, h, &created);
}

/* Severs with w and checks the window left is exactly the definition's. */
static void
sever_and_compare(VcRepo *r, const VcWindow *w, uint64_t now)
{
	PHashMap	want;
	WinSet		got;
	uint32_t	seg;

	expected_window(r, w, now, &want);
	CHECK(vc_sever(r, w, &seg) == VC_OK);
	if (seg != VC_NO_SEGMENT)
		CHECK(vc_segment_discard(r, seg) == VC_OK);
	phmap_init(&got.set);
	vc_locations(r, window_set_fn, &got);
	CHECK(set_equal(&want, &got.set));
	CHECK_EQ(r->nheld, set_size(&got.set));
	phmap_free(&want);
	phmap_free(&got.set);
}

/* A commit's change bytes are exactly the chunks its root adds over its first parent's. */
static void
test_change_bytes(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	FakeClock	clk = {clock_base()};

	setup(&r, cs);
	vc_set_clock(&r, fake_now, &clk);
	rng_seed(11);
	commit_rows(&r, 0, 2000, 0, "load");
	for (int i = 1; i <= 20; i++)
	{
		PHash		h = timed_commit(&r, &clk, 1, 1 + (int) rnd_below(60), (uint64_t) i);
		VcCommit	c,
					p;
		PHashMap	mine,
					theirs,
					only;
		uint64_t	sum = 0;

		CHECK(vc_commit_load(&r.ctx, &h, &c) && vc_commit_load(&r.ctx, &c.parents[0], &p));
		phmap_init(&mine);
		phmap_init(&theirs);
		closure(cs, &c.root, &mine);
		closure(cs, &p.root, &theirs);
		set_minus(&mine, &theirs, &only);
		for (size_t j = 0; j < only.cap; j++)
			if (only.used[j])
			{
				size_t		len;

				CHECK(cs_get(cs, &only.keys[j], &len) != NULL);
				sum += len;
			}
		CHECK_EQ(own_bytes(&c), sum);
		CHECK_EQ(c.bytes, p.bytes + sum);
		CHECK(c.time > p.time);
		phmap_free(&mine);
		phmap_free(&theirs);
		phmap_free(&only);
	}
	/* a commit written with a time before its parent's is placed after it */
	{
		VcCommit	p,
					c;
		PHash		h;

		CHECK(vc_commit_load(&r.ctx, &vc_head(&r)->head, &p));
		h = vc_commit_write(&r.ctx, &p.root, &p.hash, 1, p.time - 100 * SEC, "early");
		CHECK(vc_commit_load(&r.ctx, &h, &c));
		CHECK_EQ(c.time, p.time + 1);
		prolly_release_pins(&r.ctx);
		cs_collect_orphans(cs);
	}
	/* a clock that goes back still gives a later time than the parent's */
	{
		VcCommit	c,
					p;
		PHash		h;

		clk.us -= 500 * SEC;
		h = timed_commit(&r, &clk, 0, 1, 99);
		CHECK(vc_commit_load(&r.ctx, &h, &c) && vc_commit_load(&r.ctx, &c.parents[0], &p));
		CHECK(c.time > p.time);
	}
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* Each limit alone, as floor and as ceiling, and a ceiling beating a floor. */
static void
test_window_limits(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	FakeClock	clk = {clock_base()};
	PHash		h[41];
	VcCommit	c;
	uint64_t	sz[41];

	setup(&r, cs);
	vc_set_clock(&r, fake_now, &clk);
	rng_seed(12);
	commit_rows(&r, 0, 4000, 0, "load");
	for (int i = 1; i <= 40; i++)
	{
		/* one commit a minute, of varying size */
		h[i] = timed_commit(&r, &clk, 60, 1 + (int) rnd_below(40), (uint64_t) i);
		CHECK(vc_commit_load(&r.ctx, &h[i], &c));
		sz[i] = own_bytes(&c);
	}

	/* commits: a floor of 8 under a ceiling of 5 keeps 5 */
	sever_and_compare(&r, &(VcWindow) {.depth = 8, .max_commits = 5}, clk.us);
	CHECK_EQ(r.nheld, 5);
	/* ceilings only shrink: 8 under 20 is still the 5 left */
	sever_and_compare(&r, &(VcWindow) {.depth = 8, .max_commits = 20}, clk.us);
	CHECK_EQ(r.nheld, 5);
	vc_repo_free(&r);
	cs_destroy(cs);

	/* the rest on fresh copies of the same history */
	for (int variant = 0; variant < 6; variant++)
	{
		uint64_t	after = 0,
					span = 0;
		int			keep = 0;

		cs = memstore_create();
		setup(&r, cs);
		vc_set_clock(&r, fake_now, &clk);
		clk.us = clock_base();
		rng_seed(12);
		commit_rows(&r, 0, 4000, 0, "load");
		for (int i = 1; i <= 40; i++)
			timed_commit(&r, &clk, 60, 1 + (int) rnd_below(40), (uint64_t) i);
		switch (variant)
		{
			case 5:
				/* exactly the bytes after commit 38: it is no longer needed */
				sever_and_compare(&r, &(VcWindow) {.keep_bytes = sz[40] + sz[39]}, clk.us);
				CHECK_EQ(r.nheld, 2);
				break;
			case 0:
				/* bytes floor: keep until the commits after reach 10 sizes' worth */
				for (int i = 40; i >= 1 && after < 10 * sz[40]; i--, keep++)
					after += sz[i];
				sever_and_compare(&r, &(VcWindow) {.depth = 1, .keep_bytes = 10 * sz[40]}, clk.us);
				CHECK_EQ(r.nheld, (uint64_t) keep);
				break;
			case 1:
				/* bytes ceiling: at most what the last 12 commits changed */
				for (int i = 40; i > 28; i--)
					span += sz[i];
				sever_and_compare(&r, &(VcWindow) {.depth = 30, .max_bytes = span}, clk.us);
				CHECK_EQ(r.nheld, 12);
				break;
			case 2:
				/* time floor: ten minutes keeps the commits of the last ten minutes */
				sever_and_compare(&r, &(VcWindow) {.depth = 1, .keep_seconds = 600}, clk.us);
				CHECK_EQ(r.nheld, 10);
				break;
			case 3:
				/* time ceiling beats a commit floor */
				sever_and_compare(&r, &(VcWindow) {.depth = 30, .max_seconds = 300}, clk.us);
				CHECK_EQ(r.nheld, 5);
				break;
			case 4:
				/* the head stays however old: an hour later, a one-minute ceiling */
				clk.us += 3600 * SEC;
				sever_and_compare(&r, &(VcWindow) {.depth = 30, .max_seconds = 60}, clk.us);
				CHECK_EQ(r.nheld, 1);
				break;
		}
		check_store(cs, 0);
		vc_repo_free(&r);
		cs_destroy(cs);
	}
}

/*
 * Random branching, merging, clocks and policies: after every sever the
 * window is exactly what its definition says, and the held count agrees.
 */
static void
test_window_policy_random(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	FakeClock	clk = {clock_base()};
	const char *names[] = {"main", "b1", "b2", "b3"};

	setup(&r, cs);
	vc_set_clock(&r, fake_now, &clk);
	rng_seed(13);
	commit_rows(&r, 0, 4000, 0, "load");
	for (int b = 1; b < 4; b++)
		CHECK(vc_branch_create(&r, names[b], &vc_head(&r)->head) == VC_OK);
	for (int step = 1; step <= 300; step++)
	{
		int			b = (int) rnd_below(4);

		vc_checkout(&r, names[b]);
		if (rnd_below(8) == 0)
		{
			VcMergeResult m;
			int			o = (int) rnd_below(4);

			if (o != b && vc_merge(&r, names[o], &m) == VC_OK &&
				m.status == VC_MERGE_CONFLICTS)
				vc_reset_hard(&r, &vc_head(&r)->head);
			vc_merge_result_free(&m);
		}
		else
			timed_commit(&r, &clk, rnd_below(120), 1 + (int) rnd_below(30), (uint64_t) step);
		if (step % 10 == 0)
		{
			VcWindow	w = {.depth = (uint32_t) rnd_below(12)};
			uint64_t	unit = 3000;

			if (rnd_below(2))
				w.keep_bytes = unit * (1 + rnd_below(20));
			if (rnd_below(2))
				w.keep_seconds = 60 * (1 + rnd_below(20));
			if (rnd_below(3) == 0)
				w.max_commits = 2 + rnd_below(15);
			if (rnd_below(3) == 0)
				w.max_bytes = unit * (2 + rnd_below(30));
			if (rnd_below(3) == 0)
				w.max_seconds = 60 * (2 + rnd_below(30));
			vc_checkout(&r, "main");
			sever_and_compare(&r, &w, clk.us);
		}
	}
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/*
 * Severing only when due keeps the window within a tenth over its ceiling,
 * and the counters survive a reopen.
 */
static void
test_sever_when_due(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	FakeClock	clk = {clock_base()};
	VcWindow	w = {.max_commits = 200};
	int			severs = 0;
	uint64_t	most = 0;
	PHash		st;

	setup(&r, cs);
	vc_set_clock(&r, fake_now, &clk);
	rng_seed(14);
	commit_rows(&r, 0, 4000, 0, "load");
	for (int i = 1; i <= 3000; i++)
	{
		timed_commit(&r, &clk, 1, 1, (uint64_t) i);
		if (r.nheld > most)
			most = r.nheld;
		if (vc_sever_due(&r, &w))
		{
			uint32_t	seg;

			CHECK(vc_sever(&r, &w, &seg) == VC_OK);
			if (seg != VC_NO_SEGMENT)
				CHECK(vc_segment_discard(&r, seg) == VC_OK);
			severs++;
			CHECK_EQ(r.since_commits, 0);
		}
	}
	/* held grows while commits come: due after W / (slack - 1) of them */
	CHECK_LE(most, 200 + 200 / (VC_SEVER_SLACK - 1) + 2);
	/* about one sever per tenth of the window, not one per commit */
	CHECK_LE(severs, 3000 * VC_SEVER_SLACK / 200 + 20);
	CHECK(severs >= 3000 / 200);

	/* a time ceiling: due once a tenth of it has passed */
	{
		VcWindow	tw = {.depth = 1, .max_seconds = 1000};

		CHECK(vc_sever(&r, &tw, &(uint32_t) {0}) == VC_OK);
		CHECK(!vc_sever_due(&r, &(VcWindow) {.depth = 1, .max_seconds = 1000000000}));
		clk.us += 99 * SEC;
		CHECK(!vc_sever_due(&r, &(VcWindow) {.depth = 1, .max_seconds = 1000}) ||
			  r.since_commits * VC_SEVER_SLACK >= r.nheld);
		clk.us += 2 * SEC;
		CHECK(vc_sever_due(&r, &tw));
	}

	r.since_commits = 7;
	r.since_bytes = 1234;
	st = vc_repo_save(&r);
	{
		VcRepo		r2;
		WinSet		got;

		CHECK(vc_repo_open(&r2, cs, &small_policy, &st) == VC_OK);
		CHECK_EQ(r2.nheld, r.nheld);
		CHECK_EQ(r2.since_commits, 7);
		CHECK_EQ(r2.since_bytes, 1234);
		CHECK_EQ(r2.last_sever, r.last_sever);
		phmap_init(&got.set);
		vc_locations(&r2, window_set_fn, &got);
		CHECK_EQ(set_size(&got.set), r2.nheld);
		phmap_free(&got.set);
		vc_repo_free(&r2);
	}
	cs_collect_orphans(cs);
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* ------------------------------------------------------------ low marks */

static uint64_t
tighter(uint64_t limit, uint64_t mark)
{
	if (mark == 0)
		return limit;
	if (limit == 0)
		return mark;
	return mark < limit ? mark : limit;
}

/*
 * The marked window straight from its definition: a head whose high-mark
 * window would let go of a held commit (a parent of the window's commits,
 * outside it, still held) is cut back to its low-mark window; any other
 * head keeps its high-mark window.
 */
static void
expected_marked(VcRepo *r, const VcWindow *w, uint64_t now, PHashMap *held,
				PHashMap *out)
{
	VcWindow	high = *w,
				low = *w;

	high.low_commits = high.low_bytes = high.low_seconds = 0;
	low.max_commits = tighter(w->max_commits, w->low_commits);
	low.max_bytes = tighter(w->max_bytes, w->low_bytes);
	low.max_seconds = tighter(w->max_seconds, w->low_seconds);
	low.low_commits = low.low_bytes = low.low_seconds = 0;
	phmap_init(out);
	for (size_t i = 0; i < r->nbr; i++)
	{
		PHashMap	hs;
		bool		over = false;

		phmap_init(&hs);
		expected_head(r, &r->br[i].head, &high, now, &hs);
		for (size_t j = 0; j < hs.cap && !over; j++)
		{
			VcCommit	c;

			if (!hs.used[j] || !vc_commit_load(&r->ctx, &hs.keys[j], &c))
				continue;
			for (uint32_t k = 0; k < c.nparents; k++)
				if (!phmap_find(&hs, &c.parents[k]) && phmap_find(held, &c.parents[k]))
					over = true;
		}
		phmap_free(&hs);
		expected_head(r, &r->br[i].head, over ? &low : &high, now, out);
	}
}

/* Severs with a marked window and checks the window left is the definition's. */
static void
sever_marked_and_compare(VcRepo *r, const VcWindow *w, uint64_t now)
{
	PHashMap	want;
	WinSet		held,
				got;
	uint32_t	seg;

	phmap_init(&held.set);
	vc_locations(r, window_set_fn, &held);
	expected_marked(r, w, now, &held.set, &want);
	CHECK(vc_sever(r, w, &seg) == VC_OK);
	if (seg != VC_NO_SEGMENT)
		CHECK(vc_segment_discard(r, seg) == VC_OK);
	phmap_init(&got.set);
	vc_locations(r, window_set_fn, &got);
	CHECK(set_equal(&want, &got.set));
	CHECK_EQ(r->nheld, set_size(&got.set));
	phmap_free(&want);
	phmap_free(&held.set);
	phmap_free(&got.set);
}

static void
sever_discard(VcRepo *r, const VcWindow *w)
{
	uint32_t	seg;

	CHECK(vc_sever(r, w, &seg) == VC_OK);
	if (seg != VC_NO_SEGMENT)
		CHECK(vc_segment_discard(r, seg) == VC_OK);
}

/*
 * A line of commits severed after each one: the window holds h commits,
 * and the next makes h + 1, or, past the high mark of N, the low mark L.
 */
static void
marks_line(uint64_t N, uint64_t low, uint64_t want_low)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	FakeClock	clk = {clock_base()};
	VcWindow	w = {.max_commits = N, .low_commits = low};
	uint64_t	h;
	int			resets = 0;

	setup(&r, cs);
	vc_set_clock(&r, fake_now, &clk);
	rng_seed(21);
	commit_rows(&r, 0, 2000, 0, "load");
	sever_discard(&r, &w);
	h = r.nheld;				/* the initial commit and the load, if under N */
	CHECK_EQ(h, N >= 2 ? 2 : 1);
	for (int i = 1; i <= (int) (4 * N + 3); i++)
	{
		timed_commit(&r, &clk, 1, 1 + (int) rnd_below(20), (uint64_t) i);
		sever_marked_and_compare(&r, &w, clk.us);
		h = h + 1 <= N ? h + 1 : want_low;
		if (h == want_low && want_low < N)
			resets++;
		CHECK_EQ(r.nheld, h);
		CHECK(r.nheld <= N);
	}
	if (want_low < N)
		CHECK(resets >= 3);
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* low_commits = 1: the window grows to N commits, then holds only the head. */
static void
test_marks_tumbling(void)
{
	marks_line(8, 1, 1);
	marks_line(1, 1, 1);		/* a high mark of one: only ever the head */
}

/* A low mark between: it grows to N, then back to the last L. */
static void
test_marks_hysteresis(void)
{
	marks_line(10, 4, 4);
	marks_line(10, 9, 9);
}

/* A low mark at or past the high mark slides, as with no low mark. */
static void
test_marks_at_high_slide(void)
{
	marks_line(10, 10, 10);
	marks_line(10, 25, 10);
}

/* Low marks alone are refused: they need a high mark to fill to. */
static void
test_marks_need_high(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	uint32_t	seg;

	setup(&r, cs);
	commit_rows(&r, 0, 100, 0, "load");
	CHECK(vc_sever(&r, &(VcWindow) {.low_commits = 1}, &seg) == VC_ERR);
	CHECK(vc_sever(&r, &(VcWindow) {.low_bytes = 100, .low_seconds = 5}, &seg) == VC_ERR);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/*
 * A floor is the high mark too: a depth floor of 30 with no ceiling fills
 * to 30 commits, then is cut to the low mark, which wins over the floor.
 */
static void
test_marks_floor_is_high(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	FakeClock	clk = {clock_base()};
	VcWindow	w = {.depth = 30, .low_commits = 5};
	bool		cut = false;

	setup(&r, cs);
	vc_set_clock(&r, fake_now, &clk);
	rng_seed(22);
	commit_rows(&r, 0, 2000, 0, "load");
	for (int i = 1; i <= 70; i++)
	{
		timed_commit(&r, &clk, 1, 3, (uint64_t) i);
		sever_marked_and_compare(&r, &w, clk.us);
		CHECK(r.nheld <= 30);
		cut |= r.nheld == 5;
	}
	CHECK(cut);
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/*
 * Each branch fills and empties on its own: a busy branch is cut back to
 * its low mark while a quiet one keeps its whole window, shared history
 * included.
 */
static void
test_marks_branches_independent(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	FakeClock	clk = {clock_base()};
	VcWindow	w = {.max_commits = 12, .low_commits = 2};
	PHash		fork;
	PHashMap	held;
	WinSet		got;

	setup(&r, cs);
	vc_set_clock(&r, fake_now, &clk);
	rng_seed(23);
	commit_rows(&r, 0, 2000, 0, "load");
	for (int i = 1; i <= 5; i++)
		timed_commit(&r, &clk, 1, 2, (uint64_t) i);
	fork = vc_head(&r)->head;
	CHECK(vc_branch_create(&r, "busy", &fork) == VC_OK);
	CHECK(vc_checkout(&r, "busy") == VC_OK);
	for (int i = 6; i <= 30; i++)
		timed_commit(&r, &clk, 1, 2, (uint64_t) i);
	CHECK(vc_checkout(&r, "main") == VC_OK);
	timed_commit(&r, &clk, 1, 2, 31);
	sever_marked_and_compare(&r, &w, clk.us);
	/* main (7 commits deep at most) is short of 12: all of its window stays */
	phmap_init(&held);
	expected_head(&r, &vc_head(&r)->head, &(VcWindow) {.max_commits = 12}, clk.us, &held);
	phmap_init(&got.set);
	vc_locations(&r, window_set_fn, &got);
	for (size_t i = 0; i < held.cap; i++)
		if (held.used[i])
			CHECK(phmap_find(&got.set, &held.keys[i]) != NULL);
	CHECK(phmap_find(&got.set, &fork) != NULL);
	/* busy is cut to its last 2, so beside main's 7 the window holds 9 */
	CHECK_EQ(r.nheld, set_size(&held) + 2);
	phmap_free(&held);
	phmap_free(&got.set);
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* A pinned commit stays through a cut to the low mark. */
static void
test_marks_pinned(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	FakeClock	clk = {clock_base()};
	PHash		pin;
	VcWindow	w = {.max_commits = 6, .low_commits = 1};
	uint32_t	seg;

	setup(&r, cs);
	vc_set_clock(&r, fake_now, &clk);
	rng_seed(24);
	commit_rows(&r, 0, 2000, 0, "load");
	pin = timed_commit(&r, &clk, 1, 2, 1);
	w.pinned = &pin;
	w.npinned = 1;
	for (int i = 2; i <= 20; i++)
	{
		timed_commit(&r, &clk, 1, 2, (uint64_t) i);
		sever_discard(&r, &w);
		CHECK(vc_locate(&r, &pin, &seg) == VC_LOC_WINDOW);
		CHECK(r.nheld <= 7);
	}
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/*
 * Random histories, branches and merges under random marked windows: every
 * sever leaves exactly the definition's window.
 */
static void
test_marks_random(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	FakeClock	clk = {clock_base()};
	const char *names[] = {"main", "b1", "b2", "b3"};

	setup(&r, cs);
	vc_set_clock(&r, fake_now, &clk);
	rng_seed(25);
	commit_rows(&r, 0, 4000, 0, "load");
	for (int b = 1; b < 4; b++)
		CHECK(vc_branch_create(&r, names[b], &vc_head(&r)->head) == VC_OK);
	for (int round = 0; round < 6; round++)
	{
		VcWindow	w = {0};
		uint64_t	unit = 3000;

		/* one window per round, so that branches fill and are cut under it */
		if (rnd_below(2))
			w.depth = (uint32_t) (4 + rnd_below(20));
		if (rnd_below(3) == 0)
			w.keep_bytes = unit * (4 + rnd_below(20));
		if (rnd_below(3) == 0)
			w.keep_seconds = 60 * (4 + rnd_below(20));
		if (rnd_below(2))
			w.max_commits = 4 + rnd_below(20);
		if (rnd_below(3) == 0)
			w.max_bytes = unit * (6 + rnd_below(30));
		if (rnd_below(3) == 0)
			w.max_seconds = 60 * (6 + rnd_below(30));
		if (!w.depth && !w.keep_bytes && !w.keep_seconds && !w.max_commits &&
			!w.max_bytes && !w.max_seconds)
			w.max_commits = 10;
		if (rnd_below(2))
			w.low_commits = 1 + rnd_below(6);
		if (rnd_below(3) == 0)
			w.low_bytes = unit * (1 + rnd_below(6));
		if (rnd_below(3) == 0)
			w.low_seconds = 60 * (1 + rnd_below(6));
		if (!w.low_commits && !w.low_bytes && !w.low_seconds)
			w.low_commits = 1;
		for (int step = 1; step <= 80; step++)
		{
			int			b = (int) rnd_below(4);

			vc_checkout(&r, names[b]);
			if (rnd_below(8) == 0)
			{
				VcMergeResult m;
				int			o = (int) rnd_below(4);

				if (o != b && vc_merge(&r, names[o], &m) == VC_OK &&
					m.status == VC_MERGE_CONFLICTS)
					vc_reset_hard(&r, &vc_head(&r)->head);
				vc_merge_result_free(&m);
			}
			else
				timed_commit(&r, &clk, rnd_below(120), 1 + (int) rnd_below(30),
							 (uint64_t) (round * 100 + step));
			if (step % 3 == 0)
			{
				vc_checkout(&r, "main");
				sever_marked_and_compare(&r, &w, clk.us);
			}
		}
	}
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/*
 * Severing when due with a tumbling window: the window stays within a
 * tenth over its high mark, and the severs stay about one per tenth of it
 * however small the window is after a cut.
 */
static void
test_marks_when_due(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	FakeClock	clk = {clock_base()};
	VcWindow	w = {.max_commits = 200, .low_commits = 1};
	int			severs = 0,
				resets = 0;
	uint64_t	most = 0;

	setup(&r, cs);
	vc_set_clock(&r, fake_now, &clk);
	rng_seed(26);
	commit_rows(&r, 0, 4000, 0, "load");
	for (int i = 1; i <= 3000; i++)
	{
		timed_commit(&r, &clk, 1, 1, (uint64_t) i);
		if (r.nheld > most)
			most = r.nheld;
		if (vc_sever_due(&r, &w))
		{
			uint64_t	before = r.nheld;

			sever_discard(&r, &w);
			severs++;
			if (r.nheld == 1 && before > 1)
				resets++;
		}
	}
	CHECK_LE(most, 200 + 200 / (VC_SEVER_SLACK - 1) + 2);
	CHECK(resets >= 3000 / 230);
	/* due once a tenth of the held commits are new: about ln(200) x 10 per batch */
	CHECK_LE(severs, (3000 / 200 + 1) * 70);
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
}

int
main(void)
{
	RUN(test_detach_moves_holds_only);
	RUN(test_export_self_contained);
	RUN(test_second_export_copies_diff);
	RUN(test_discard_frees_exactly);
	RUN(test_state_does_not_grow_with_history);
	RUN(test_branches_cut_at_shared_ancestor);
	RUN(test_stale_branch_head_stays);
	RUN(test_merge_commit_parents_across_edge);
	RUN(test_overlapping_branch_windows);
	RUN(test_pinned_commit_stays);
	RUN(test_refuse_outside_window);
	RUN(test_remotes_across_edge);
	RUN(test_incremental_window_matches_full);
	RUN(test_many_branches);
	RUN(test_window_scaling);
	RUN(test_change_bytes);
	RUN(test_window_limits);
	RUN(test_window_policy_random);
	RUN(test_sever_when_due);
	RUN(test_marks_tumbling);
	RUN(test_marks_hysteresis);
	RUN(test_marks_at_high_slide);
	RUN(test_marks_need_high);
	RUN(test_marks_floor_is_high);
	RUN(test_marks_branches_independent);
	RUN(test_marks_pinned);
	RUN(test_marks_random);
	RUN(test_marks_when_due);
	printf("test_window: all tests passed\n");
	return 0;
}
