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
	VcWindow	depth1 = {1, NULL, 0};
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
	VcWindow	depth3 = {3, NULL, 0};

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
	VcWindow	w = {1, NULL, 0};

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
	VcWindow	bad = {0, NULL, 0};

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
		for (size_t i = 0; i < r->loc.cap; i++)
			if (r->loc.used[i] && vc_locate(r, &r->loc.keys[i], NULL) == VC_LOC_WINDOW &&
				!phmap_find(&win, &r->loc.keys[i]))
				n++;
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

			CHECK(vc_sever(&r, &(VcWindow) {depth, NULL, 0}, &seg) == VC_OK);
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
	VcWindow	w = {5, NULL, 0};
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

/*
 * The saved repo is the size of what it holds: commits cut and discarded
 * leave nothing in it.  Reopened, the commit just past the oldest kept one
 * is still known as discarded, read from the kept commit itself.
 */
static void
test_state_does_not_grow_with_history(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	PHash		state,
				head,
				edge;
	size_t		len10 = 0,
				len = 0;
	uint32_t	seg;
	LogAcc		acc;

	setup(&r, cs);
	CHECK(vc_window_track(&r, 5) == VC_OK);
	for (int i = 1; i <= 200; i++)
	{
		commit_rows(&r, (uint64_t) i, (uint64_t) i + 1, (uint64_t) i, "step");
		CHECK(vc_sever(&r, &(VcWindow) {5, NULL, 0}, &seg) == VC_OK);
		if (seg != VC_NO_SEGMENT)
			CHECK(vc_segment_discard(&r, seg) == VC_OK);
		state = vc_repo_save(&r);
		CHECK(cs_get(cs, &state, &len) != NULL);
		if (i == 10)
			len10 = len;
	}
	CHECK(len10 > 0);
	/* a varint or two per commit held grows a byte as the clock moves on */
	CHECK_LE(len, len10 + 16);
	head = vc_head(&r)->head;
	CHECK(log_from(&r, &head, 0, &acc) == VC_ERR_SEVERED && acc.n == 5);
	edge = r.ctx.err_hash;
	CHECK(vc_locate(&r, &edge, NULL) == VC_LOC_DISCARDED);
	vc_repo_free(&r);

	CHECK(vc_repo_open(&r, cs, &small_policy, &state) == VC_OK);
	CHECK_EQ(r.nsegs, 0);
	CHECK(vc_locate(&r, &edge, NULL) == VC_LOC_DISCARDED);
	CHECK(vc_window_verify(&r));
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
	printf("test_window: all tests passed\n");
	return 0;
}
