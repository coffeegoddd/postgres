/*
 * test_remote.c
 *	  Remote operations with windows on both sides: no implicit resurrection
 *	  of cut history, shallow clones at the receiver's depth with hints,
 *	  connected fetches, receiver-side fast-forward checks, deepening from
 *	  remotes and archives into segments, archive manifests, pull that
 *	  deepens to find a merge base, branching on both sides, and costs.
 */
#include "test_repo.h"

static void
name_repo(VcRepo *r, const char *name)
{
	vc_repo_set_name(r, name);
}

static int
count_window(VcRepo *r)
{
	int			n = 0;

	for (size_t i = 0; i < r->loc.cap; i++)
		if (r->loc.used[i] && vc_locate(r, &r->loc.keys[i], NULL) == VC_LOC_WINDOW)
			n++;
	return n;
}

/* Origin: init <- A <- ... with n commits after A on main. */
static PHash *
build_main(VcRepo *r, int n)
{
	PHash	   *c = px_malloc((size_t) (n + 2) * sizeof(PHash));

	c[0] = vc_head(r)->head;
	c[1] = commit_rows(r, 0, 20000, 0, "A");
	for (int i = 2; i < n + 2; i++)
		c[i] = commit_rows(r, (uint64_t) i * 700, (uint64_t) i * 700 + 100,
						   (uint64_t) i, "main");
	return c;
}

/* ------------------------------------------------------------------ tests */

/* A full-history laptop pushing to a remote that discarded A, B sends none of it. */
static void
test_push_does_not_resurrect(void)
{
	ChunkStore *ocs = memstore_create(),
			   *lcs = memstore_create();
	VcRepo		origin,
				laptop;
	Linear		l;
	uint32_t	seg;
	ChunkStoreStats st;
	PHash		e;
	LogAcc		acc;

	setup(&origin, ocs);
	name_repo(&origin, "origin");
	l = build_linear(&origin);
	CHECK(vc_repo_clone(&laptop, lcs, &origin, 0) == VC_OK);
	name_repo(&laptop, "laptop");

	CHECK(vc_sever(&origin, &depth2, &seg) == VC_OK);
	CHECK(vc_segment_discard(&origin, seg) == VC_OK);
	CHECK(!ocs->ops->has(ocs, &l.a));

	e = commit_rows(&laptop, 500, 600, 9, "E");
	st = ocs->stats;
	CHECK(vc_push(&laptop, &origin, "main") == VC_OK);
	CHECK(phash_eq(&vc_head(&origin)->head, &e));
	CHECK(!ocs->ops->has(ocs, &l.a) && !ocs->ops->has(ocs, &l.b));
	CHECK(vc_locate(&origin, &l.b, NULL) == VC_LOC_DISCARDED);
	CHECK_LE(STATS_DELTA(ocs, st, new_chunks), 3 * 100 * 6 + 10);
	CHECK(log_from(&origin, &e, 0, &acc) == VC_ERR_SEVERED && acc.n == 3);
	check_store(ocs, 0);
	check_store(lcs, 0);

	vc_repo_free(&origin);
	vc_repo_free(&laptop);
	cs_destroy(ocs);
	cs_destroy(lcs);
}

/* A clone at depth 3 takes three commits per branch, with hints for the rest. */
static void
test_shallow_clone_with_hints(void)
{
	ChunkStore *ocs = memstore_create(),
			   *scs = memstore_create(),
			   *fcs = memstore_create();
	VcRepo		origin,
				shallow,
				full;
	PHash	   *c,
				f2;
	LogAcc		acc;
	Snap		head;

	setup(&origin, ocs);
	name_repo(&origin, "origin");
	c = build_main(&origin, 12);
	CHECK(vc_branch_create(&origin, "feature", &c[6]) == VC_OK);
	vc_checkout(&origin, "feature");
	commit_rows(&origin, 30000, 30100, 50, "f1");
	f2 = commit_rows(&origin, 30100, 30200, 51, "f2");
	vc_checkout(&origin, "main");
	head = snap(&origin.ctx, &c[13]);

	CHECK(vc_repo_clone(&shallow, scs, &origin, 3) == VC_OK);
	CHECK(vc_repo_clone(&full, fcs, &origin, 0) == VC_OK);
	CHECK_EQ(count_window(&shallow), 6);
	CHECK(vc_locate(&shallow, &c[13], NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&shallow, &c[11], NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&shallow, &c[10], NULL) == VC_LOC_BOUNDARY);
	CHECK(strcmp(vc_hint(&shallow, &c[10]), "origin") == 0);
	CHECK(vc_locate(&shallow, &f2, NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&shallow, &c[6], NULL) == VC_LOC_WINDOW);	/* feature's base */
	CHECK(vc_locate(&shallow, &c[5], NULL) == VC_LOC_BOUNDARY);
	CHECK(vc_locate(&shallow, &c[1], NULL) == VC_LOC_UNKNOWN);	/* never named */
	CHECK(scs->ops->count(scs) < fcs->ops->count(fcs));
	check_snap(&shallow.ctx, &c[13], &head);
	CHECK(log_from(&shallow, &c[13], 0, &acc) == VC_ERR_SEVERED && acc.n == 3);
	CHECK(phash_eq(&shallow.ctx.err_hash, &c[10]));
	check_store(scs, 0);

	free(c);
	free(head.v);
	vc_repo_free(&origin);
	vc_repo_free(&shallow);
	vc_repo_free(&full);
	cs_destroy(ocs);
	cs_destroy(scs);
	cs_destroy(fcs);
}

/* Fetch connects: everything down to known commits, however small the window. */
static void
test_fetch_connects_past_depth(void)
{
	ChunkStore *ocs = memstore_create(),
			   *lcs = memstore_create();
	VcRepo		origin,
				laptop;
	PHash	   *c;
	PHash		last = {{0}};
	VcMergeResult m;
	int			before;

	setup(&origin, ocs);
	name_repo(&origin, "origin");
	c = build_main(&origin, 4);
	CHECK(vc_repo_clone(&laptop, lcs, &origin, 2) == VC_OK);
	before = count_window(&laptop);
	for (int i = 0; i < 6; i++)
		last = commit_rows(&origin, 100 + (uint64_t) i, 101 + (uint64_t) i, 70, "new");
	CHECK(vc_pull(&laptop, &origin, "main", 0, &m) == VC_OK);
	CHECK(m.status == VC_MERGE_FAST_FORWARD && phash_eq(&m.commit, &last));
	vc_merge_result_free(&m);
	CHECK_EQ(count_window(&laptop), before + 6);
	check_store(lcs, 0);

	free(c);
	vc_repo_free(&origin);
	vc_repo_free(&laptop);
	cs_destroy(ocs);
	cs_destroy(lcs);
}

/* The remote checks fast-forward from its own history, across the sender's cut. */
static void
test_push_ff_checked_by_receiver(void)
{
	ChunkStore *ocs = memstore_create(),
			   *lcs = memstore_create(),
			   *acs = memstore_create();
	VcRepo		origin,
				laptop;
	Linear		l;
	uint32_t	seg;
	PHash		e,
				x;
	size_t		count;

	setup(&origin, ocs);
	name_repo(&origin, "origin");
	l = build_linear(&origin);
	CHECK(vc_repo_clone(&laptop, lcs, &origin, 0) == VC_OK);
	/* branches the laptop does not have: one at B, one off on its own */
	CHECK(vc_branch_create(&origin, "rel", &l.b) == VC_OK);
	CHECK(vc_branch_create(&origin, "side", &l.c) == VC_OK);
	vc_checkout(&origin, "side");
	commit_rows(&origin, 100, 200, 40, "side work");
	vc_checkout(&origin, "main");

	CHECK(vc_sever(&laptop, &depth2, &seg) == VC_OK);
	CHECK(vc_segment_export(&laptop, seg, &(VcSource) {"cold", acs}) == VC_OK);
	CHECK(!lcs->ops->has(lcs, &l.b));

	/* rel moves from B to the laptop's new commit: the laptop cannot see B */
	e = commit_rows(&laptop, 700, 800, 41, "E");
	CHECK(vc_branch_create(&laptop, "rel", NULL) == VC_OK);
	CHECK(vc_push(&laptop, &origin, "rel") == VC_OK);
	CHECK(phash_eq(&vc_branch_find(&origin, "rel")->head, &e));

	/* side diverged on the remote: not a fast-forward, and nothing is left behind */
	x = commit_rows(&laptop, 900, 1000, 42, "X");
	CHECK(vc_branch_create(&laptop, "side", NULL) == VC_OK);
	count = ocs->ops->count(ocs);
	CHECK(vc_push(&laptop, &origin, "side") == VC_ERR);
	CHECK_EQ(ocs->ops->count(ocs), count);
	CHECK(!ocs->ops->has(ocs, &x));
	check_store(ocs, 0);

	vc_repo_free(&origin);
	vc_repo_free(&laptop);
	cs_destroy(ocs);
	cs_destroy(lcs);
	cs_destroy(acs);
}

/* Deepening brings history past the cut into a segment, not the window. */
static void
test_deepen_from_remote(void)
{
	ChunkStore *ocs = memstore_create(),
			   *lcs = memstore_create();
	VcRepo		origin,
				laptop;
	PHash	   *c;
	uint32_t	seg,
				seg2;
	VcSource	src = {"origin", NULL};
	LogAcc		acc;
	size_t		want;
	ChunkStoreStats st;
	Snap		s9;

	setup(&origin, ocs);
	name_repo(&origin, "origin");
	c = build_main(&origin, 10);	/* c[0..11] */
	s9 = snap(&origin.ctx, &c[9]);
	CHECK(vc_repo_clone(&laptop, lcs, &origin, 2) == VC_OK);
	CHECK(vc_locate(&laptop, &c[9], NULL) == VC_LOC_BOUNDARY);
	src.cs = ocs;

	CHECK(vc_deepen(&laptop, &src, &c[11], 3, &seg) == VC_ERR);	/* in the window */
	CHECK(vc_deepen(&laptop, &src, &c[9], 3, &seg) == VC_OK && seg != VC_NO_SEGMENT);
	CHECK_EQ(laptop.segs[seg].n, 3);
	CHECK(vc_locate(&laptop, &c[9], NULL) == VC_LOC_SEGMENT);
	CHECK(vc_locate(&laptop, &c[7], NULL) == VC_LOC_SEGMENT);
	CHECK(vc_locate(&laptop, &c[6], NULL) == VC_LOC_BOUNDARY);
	CHECK(strcmp(vc_hint(&laptop, &c[6]), "origin") == 0);
	check_snap(&laptop.ctx, &c[9], &s9);
	CHECK(log_from(&laptop, &c[11], 0, &acc) == VC_ERR_SEVERED && acc.n == 5);
	CHECK(log_from(&laptop, &c[11], VC_WINDOW_ONLY, &acc) == VC_ERR_OUTSIDE_WINDOW && acc.n == 2);
	check_store(lcs, 0);

	/* done looking: discard it, which frees exactly it; deepen again later */
	want = expected_frees(&laptop, seg);
	st = lcs->stats;
	CHECK(vc_segment_discard(&laptop, seg) == VC_OK);
	CHECK_EQ(STATS_DELTA(lcs, st, frees), want);
	CHECK(vc_locate(&laptop, &c[9], NULL) == VC_LOC_DISCARDED);
	CHECK(vc_deepen(&laptop, &src, &c[9], 0, &seg2) == VC_OK);
	CHECK_EQ(laptop.segs[seg2].n, 10);	/* everything down to init */
	check_store(lcs, 0);

	free(c);
	free(s9.v);
	vc_repo_free(&origin);
	vc_repo_free(&laptop);
	cs_destroy(ocs);
	cs_destroy(lcs);
}

/* The archive describes itself: a new repo restores from it alone. */
static void
test_restore_from_archive_alone(void)
{
	ChunkStore *cs = memstore_create(),
			   *acs = memstore_create(),
			   *ncs = memstore_create();
	VcRepo		r,
				fresh;
	Linear		l;
	uint32_t	seg,
				nseg;
	Snap		sb;
	PHash	   *mans;
	size_t		nm;
	VcManifest	m;
	PHash		newest = {{0}};
	uint64_t	g = 0;

	setup(&r, cs);
	name_repo(&r, "db1");
	l = build_linear(&r);
	sb = snap(&r.ctx, &l.b);
	CHECK(vc_sever(&r, &depth2, &seg) == VC_OK);
	CHECK(vc_segment_export(&r, seg, &(VcSource) {"cold", acs}) == VC_OK);
	vc_repo_free(&r);
	cs_destroy(cs);				/* the exporting repo is gone */

	vc_archive_list(acs, &mans, &nm);
	CHECK_EQ(nm, 1);
	CHECK(vc_manifest_load(acs, &mans[0], &m));
	CHECK(strcmp(m.repo, "db1") == 0 && m.n == 3 && m.nb == 0);
	for (size_t i = 0; i < m.n; i++)
		if (m.gens[i] > g)
		{
			g = m.gens[i];
			newest = m.members[i];
		}
	CHECK(phash_eq(&newest, &l.b));

	vc_repo_init(&fresh, ncs, &small_policy);
	/* separately started repos share nothing, not even init */
	CHECK(vc_locate(&fresh, &l.init, NULL) == VC_LOC_UNKNOWN);
	CHECK(vc_deepen(&fresh, &(VcSource) {"cold", acs}, &newest, 0, &nseg) == VC_OK);
	CHECK_EQ(fresh.segs[nseg].n, 3);
	CHECK(vc_segment_attach(&fresh, nseg) == VC_OK);
	check_snap(&fresh.ctx, &l.b, &sb);
	CHECK(vc_branch_create(&fresh, "restored", &l.b) == VC_OK);
	check_store(ncs, 0);
	check_store(acs, 0);

	vc_manifest_free(&m);
	free(mans);
	free(sb.v);
	vc_repo_free(&fresh);
	cs_destroy(acs);
	cs_destroy(ncs);
}

/* Discarding an exported segment frees the archive copy, sparing what others share. */
static void
test_archive_discard_frees_only_unshared(void)
{
	ChunkStore *cs = memstore_create(),
			   *acs = memstore_create();
	VcRepo		r;
	Linear		l;
	uint32_t	s1,
				s2;
	PHashMap	c1,
				c2,
				only1;
	ChunkStoreStats st;
	Snap		sc;
	ProllyCtx	actx;

	setup(&r, cs);
	l = build_linear(&r);
	CHECK(vc_sever(&r, &depth2, &s1) == VC_OK);
	CHECK(vc_segment_export(&r, s1, &(VcSource) {"cold", acs}) == VC_OK);
	commit_rows(&r, 5000, 5100, 7, "E");
	commit_rows(&r, 6000, 6100, 8, "F");
	sc = snap(&r.ctx, &l.c);
	CHECK(vc_sever(&r, &depth2, &s2) == VC_OK);
	CHECK(vc_segment_export(&r, s2, &(VcSource) {"cold", acs}) == VC_OK);

	phmap_init(&c1);
	phmap_init(&c2);
	closure(acs, &r.segs[s1].manifest, &c1);
	closure(acs, &r.segs[s2].manifest, &c2);
	set_minus(&c1, &c2, &only1);
	st = acs->stats;
	CHECK(vc_segment_discard(&r, s1) == VC_OK);
	CHECK_EQ(STATS_DELTA(acs, st, frees), set_size(&only1));
	CHECK(!acs->ops->has(acs, &l.a));
	check_store(acs, 0);

	prolly_ctx_init(&actx, acs, &small_policy);
	check_snap(&actx, &l.c, &sc);	/* the second segment is whole */
	CHECK(vc_segment_attach(&r, s2) == VC_OK);
	check_store(cs, 0);

	phmap_free(&c1);
	phmap_free(&c2);
	phmap_free(&only1);
	free(sc.v);
	prolly_ctx_free(&actx);
	vc_repo_free(&r);
	cs_destroy(cs);
	cs_destroy(acs);
}

/* Exporting to a remote that already has the history moves almost nothing. */
static void
test_export_to_remote_with_history(void)
{
	ChunkStore *ocs = memstore_create(),
			   *lcs = memstore_create(),
			   *ccs = memstore_create();
	VcRepo		origin,
				laptop,
				clone;
	Linear		l;
	uint32_t	seg;
	ChunkStoreStats st;

	setup(&origin, ocs);
	name_repo(&origin, "origin");
	l = build_linear(&origin);
	CHECK(vc_repo_clone(&laptop, lcs, &origin, 0) == VC_OK);
	name_repo(&laptop, "laptop");
	CHECK(vc_sever(&laptop, &depth2, &seg) == VC_OK);
	st = ocs->stats;
	CHECK(vc_segment_export(&laptop, seg, &(VcSource) {"origin", ocs}) == VC_OK);
	CHECK_EQ(STATS_DELTA(ocs, st, new_chunks), 1);	/* the manifest */
	CHECK(!lcs->ops->has(lcs, &l.a));
	check_store(ocs, 0);
	check_store(lcs, 0);

	/* the hint follows the history: a clone of the laptop knows where B went */
	CHECK(vc_repo_clone(&clone, ccs, &laptop, 0) == VC_OK);
	CHECK(vc_locate(&clone, &l.b, NULL) == VC_LOC_BOUNDARY);
	CHECK(strcmp(vc_hint(&clone, &l.b), "origin") == 0);

	CHECK(vc_segment_attach(&laptop, seg) == VC_OK);
	CHECK(lcs->ops->has(lcs, &l.a));
	check_store(lcs, 0);

	vc_repo_free(&origin);
	vc_repo_free(&laptop);
	vc_repo_free(&clone);
	cs_destroy(ocs);
	cs_destroy(lcs);
	cs_destroy(ccs);
}

/*
 * origin: main init <- A <- m2..m9;  feature A <- f1 <- f2.
 * A depth-2 clone cannot see the base A.  Pull deepens, if allowed enough.
 */
static void
test_pull_deepens_for_merge_base(void)
{
	ChunkStore *ocs = memstore_create(),
			   *lcs = memstore_create();
	VcRepo		origin,
				laptop;
	PHash	   *c;
	PHash		working;
	VcMergeResult m;

	setup(&origin, ocs);
	name_repo(&origin, "origin");
	c = build_main(&origin, 8);
	CHECK(vc_branch_create(&origin, "feature", &c[1]) == VC_OK);
	vc_checkout(&origin, "feature");
	commit_rows(&origin, 31000, 31100, 60, "f1");
	commit_rows(&origin, 31100, 31200, 61, "f2");
	vc_checkout(&origin, "main");

	CHECK(vc_repo_clone(&laptop, lcs, &origin, 2) == VC_OK);
	CHECK(vc_locate(&laptop, &c[1], NULL) == VC_LOC_BOUNDARY);
	working = vc_working(&laptop);

	CHECK(vc_pull(&laptop, &origin, "feature", 0, &m) == VC_ERR_SEVERED);
	CHECK(phash_eq(&working, &vc_head(&laptop)->working));
	CHECK(vc_pull(&laptop, &origin, "feature", 3, &m) == VC_ERR_SEVERED);
	CHECK(vc_pull(&laptop, &origin, "feature", 32, &m) == VC_OK);
	CHECK(m.status == VC_MERGE_COMMITTED);
	vc_merge_result_free(&m);
	/* the history it needed came in as segments, not into the window */
	CHECK(vc_locate(&laptop, &c[1], NULL) == VC_LOC_SEGMENT);
	{
		PHash		w = vc_working(&laptop);
		VcTable		t;
		const uint8_t *v;
		size_t		vl;
		uint8_t		k[8];

		CHECK(vc_table_get(&laptop.ctx, &w, "t", &t));
		put_u64(k, 31150);
		CHECK(t_get(&laptop.ctx, &t.data, k, 8, &v, &vl) && get_u64(v + 8) == 61);
		put_u64(k, 9 * 700);
		CHECK(t_get(&laptop.ctx, &t.data, k, 8, &v, &vl) && get_u64(v + 8) == 9);
	}
	check_store(lcs, 0);

	free(c);
	vc_repo_free(&origin);
	vc_repo_free(&laptop);
	cs_destroy(ocs);
	cs_destroy(lcs);
}

/* A merge's parent past the depth is a boundary even though the merge names it. */
static void
test_merge_parent_beyond_depth(void)
{
	ChunkStore *ocs = memstore_create(),
			   *lcs = memstore_create();
	VcRepo		origin,
				laptop;
	PHash		c,
				e,
				mm,
				f;
	VcMergeResult m;
	LogAcc		acc;
	uint32_t	seg;

	setup(&origin, ocs);
	name_repo(&origin, "origin");
	commit_rows(&origin, 0, 20000, 0, "A");
	CHECK(vc_branch_create(&origin, "feature", NULL) == VC_OK);
	c = commit_rows(&origin, 100, 200, 1, "C");
	vc_checkout(&origin, "feature");
	e = commit_rows(&origin, 5000, 5100, 2, "E");
	vc_checkout(&origin, "main");
	CHECK(vc_merge(&origin, "feature", &m) == VC_OK && m.status == VC_MERGE_COMMITTED);
	mm = m.commit;
	vc_merge_result_free(&m);
	vc_checkout(&origin, "feature");
	f = commit_rows(&origin, 6000, 6100, 3, "F");
	vc_checkout(&origin, "main");

	CHECK(vc_repo_clone(&laptop, lcs, &origin, 1) == VC_OK);
	CHECK(vc_locate(&laptop, &mm, NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&laptop, &f, NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&laptop, &c, NULL) == VC_LOC_BOUNDARY);
	CHECK(vc_locate(&laptop, &e, NULL) == VC_LOC_BOUNDARY);
	CHECK(strcmp(vc_hint(&laptop, &e), "origin") == 0);
	CHECK(!lcs->ops->has(lcs, &e));
	CHECK(log_from(&laptop, &mm, 0, &acc) == VC_ERR_SEVERED && acc.n == 1);
	CHECK(vc_deepen(&laptop, &(VcSource) {"origin", ocs}, &e, 1, &seg) == VC_OK);
	CHECK(vc_locate(&laptop, &e, NULL) == VC_LOC_SEGMENT);
	check_store(lcs, 0);

	vc_repo_free(&origin);
	vc_repo_free(&laptop);
	cs_destroy(ocs);
	cs_destroy(lcs);
}

/* Restoring discarded history on a remote is explicit: it deepens from a source. */
static void
test_explicit_restore_on_remote(void)
{
	ChunkStore *ocs = memstore_create(),
			   *lcs = memstore_create();
	VcRepo		origin,
				laptop;
	Linear		l;
	uint32_t	seg,
				rseg;
	LogAcc		acc;

	setup(&origin, ocs);
	name_repo(&origin, "origin");
	l = build_linear(&origin);
	CHECK(vc_repo_clone(&laptop, lcs, &origin, 0) == VC_OK);
	CHECK(vc_sever(&origin, &depth2, &seg) == VC_OK);
	CHECK(vc_segment_discard(&origin, seg) == VC_OK);

	CHECK(vc_deepen(&origin, &(VcSource) {"laptop", lcs}, &l.b, 0, &rseg) == VC_OK);
	CHECK_EQ(origin.segs[rseg].n, 3);
	CHECK(vc_locate(&origin, &l.a, NULL) == VC_LOC_SEGMENT);
	CHECK(vc_segment_attach(&origin, rseg) == VC_OK);
	CHECK(log_from(&origin, &l.d, VC_WINDOW_ONLY, &acc) == VC_OK && acc.n == 5);
	check_store(ocs, 0);

	vc_repo_free(&origin);
	vc_repo_free(&laptop);
	cs_destroy(ocs);
	cs_destroy(lcs);
}

/* Both sides branch, the remote has cut its history, work still flows. */
static void
test_branching_both_sides(void)
{
	ChunkStore *ocs = memstore_create(),
			   *lcs = memstore_create(),
			   *acs = memstore_create();
	VcRepo		origin,
				laptop;
	Linear		l;
	uint32_t	seg;
	ChunkStoreStats st;
	VcMergeResult m;
	PHash		w;
	VcTable		t;
	const uint8_t *v;
	size_t		vl;
	uint8_t		k[8];

	setup(&origin, ocs);
	name_repo(&origin, "origin");
	l = build_linear(&origin);
	CHECK(vc_repo_clone(&laptop, lcs, &origin, 0) == VC_OK);
	CHECK(vc_sever(&origin, &depth2, &seg) == VC_OK);
	CHECK(vc_segment_export(&origin, seg, &(VcSource) {"cold", acs}) == VC_OK);

	/* origin: feature off C; laptop: topic off D */
	CHECK(vc_branch_create(&origin, "feature", &l.c) == VC_OK);
	vc_checkout(&origin, "feature");
	commit_rows(&origin, 11000, 11100, 20, "of1");
	commit_rows(&origin, 11100, 11200, 21, "of2");
	vc_checkout(&origin, "main");
	CHECK(vc_branch_create(&laptop, "topic", NULL) == VC_OK);
	vc_checkout(&laptop, "topic");
	commit_rows(&laptop, 12000, 12100, 30, "lt1");
	commit_rows(&laptop, 12100, 12200, 31, "lt2");

	/* the new branch travels without any cut history */
	st = ocs->stats;
	CHECK(vc_push(&laptop, &origin, "topic") == VC_OK);
	CHECK_LE(STATS_DELTA(ocs, st, new_chunks), 2 * (3 * 100 * 6 + 10));
	CHECK(!ocs->ops->has(ocs, &l.a));

	/* and the remote's branch merges into the laptop's at their base C */
	CHECK(vc_pull(&laptop, &origin, "feature", 0, &m) == VC_OK);
	CHECK(m.status == VC_MERGE_COMMITTED);
	vc_merge_result_free(&m);
	w = vc_working(&laptop);
	CHECK(vc_table_get(&laptop.ctx, &w, "t", &t));
	put_u64(k, 11150);
	CHECK(t_get(&laptop.ctx, &t.data, k, 8, &v, &vl) && get_u64(v + 8) == 21);
	put_u64(k, 12150);
	CHECK(t_get(&laptop.ctx, &t.data, k, 8, &v, &vl) && get_u64(v + 8) == 31);
	CHECK(vc_push(&laptop, &origin, "topic") == VC_OK);
	check_store(ocs, 0);
	check_store(lcs, 0);

	vc_repo_free(&origin);
	vc_repo_free(&laptop);
	cs_destroy(ocs);
	cs_destroy(lcs);
	cs_destroy(acs);
}


/* ------------------------------------------------------------ attachment */

/* Separately started repos share no history: every transfer is refused. */
static void
test_unrelated_histories_rejected(void)
{
	ChunkStore *acs = memstore_create(),
			   *bcs = memstore_create();
	VcRepo		a,
				b;
	PHash		head;
	VcMergeResult m;
	size_t		na,
				nb;

	setup(&a, acs);
	name_repo(&a, "a");
	setup(&b, bcs);
	name_repo(&b, "b");
	CHECK(!phash_eq(&vc_head(&a)->head, &vc_head(&b)->head));
	commit_rows(&a, 0, 1000, 1, "a1");
	commit_rows(&b, 0, 1000, 1, "b1");	/* same rows, different history */
	na = acs->ops->count(acs);
	nb = bcs->ops->count(bcs);

	CHECK(vc_fetch(&a, &b, "main", &head) == VC_ERR_UNRELATED);
	CHECK(vc_push(&a, &b, "main") == VC_ERR_UNRELATED);
	CHECK(vc_branch_create(&a, "side", NULL) == VC_OK);
	CHECK(vc_push(&a, &b, "side") == VC_ERR_UNRELATED);	/* a new branch too */
	CHECK(vc_pull(&a, &b, "main", 100, &m) == VC_ERR_UNRELATED);
	CHECK(vc_branch_find(&a, "b/main") == NULL);
	CHECK_EQ(acs->ops->count(acs), na);
	CHECK_EQ(bcs->ops->count(bcs), nb);
	check_store(acs, 0);
	check_store(bcs, 0);

	vc_repo_free(&a);
	vc_repo_free(&b);
	cs_destroy(acs);
	cs_destroy(bcs);
}

/*
 * The remote moved on and cut the history that joins it to the laptop's:
 * fetch is refused as detached, naming the cut and its archive, until the
 * laptop restores that stretch from the archive.
 */
static void
test_detached_fetch_until_repaired(void)
{
	ChunkStore *ocs = memstore_create(),
			   *lcs = memstore_create(),
			   *acs = memstore_create();
	VcRepo		origin,
				laptop;
	Linear		l;
	PHash		c[6],
				head;
	uint32_t	seg,
				dseg;
	VcMergeResult m;
	size_t		before;

	setup(&origin, ocs);
	name_repo(&origin, "origin");
	l = build_linear(&origin);
	CHECK(vc_repo_clone(&laptop, lcs, &origin, 2) == VC_OK);	/* knows C, D */
	for (int i = 0; i < 6; i++)
		c[i] = commit_rows(&origin, 100 + (uint64_t) i * 10, 105 + (uint64_t) i * 10,
						   50 + (uint64_t) i, "E..J");
	CHECK(vc_sever(&origin, &depth2, &seg) == VC_OK);
	CHECK(vc_segment_export(&origin, seg, &(VcSource) {"cold", acs}) == VC_OK);

	before = lcs->ops->count(lcs);
	CHECK(vc_fetch(&laptop, &origin, "main", &head) == VC_ERR_DETACHED);
	CHECK(phash_eq(&laptop.ctx.err_hash, &c[3]));
	CHECK(strcmp(vc_hint(&origin, &c[3]), "cold") == 0);
	CHECK_EQ(lcs->ops->count(lcs), before);
	CHECK(vc_pull(&laptop, &origin, "main", 100, &m) == VC_ERR_DETACHED);
	check_store(lcs, 0);

	/* repair: bring the joining stretch in from the archive, then fetch */
	CHECK(vc_deepen(&laptop, &(VcSource) {"cold", acs}, &c[3], 0, &dseg) == VC_OK);
	CHECK_EQ(laptop.segs[dseg].n, 4);	/* E..H, stopping at D */
	CHECK(vc_pull(&laptop, &origin, "main", 0, &m) == VC_OK);
	CHECK(m.status == VC_MERGE_FAST_FORWARD && phash_eq(&m.commit, &c[5]));
	vc_merge_result_free(&m);
	(void) l;
	check_store(lcs, 0);

	vc_repo_free(&origin);
	vc_repo_free(&laptop);
	cs_destroy(ocs);
	cs_destroy(lcs);
	cs_destroy(acs);
}

/*
 * The laptop worked offline and cut its own history: its push cannot be
 * shown to attach and is refused, leaving nothing on the remote, until the
 * laptop attaches its segment again.
 */
static void
test_detached_push_until_attached(void)
{
	ChunkStore *ocs = memstore_create(),
			   *lcs = memstore_create(),
			   *acs = memstore_create();
	VcRepo		origin,
				laptop;
	PHash		c[5];
	uint32_t	seg;
	size_t		before;

	setup(&origin, ocs);
	name_repo(&origin, "origin");
	build_linear(&origin);
	CHECK(vc_repo_clone(&laptop, lcs, &origin, 0) == VC_OK);
	name_repo(&laptop, "laptop");
	for (int i = 0; i < 5; i++)
		c[i] = commit_rows(&laptop, 200 + (uint64_t) i * 10, 205 + (uint64_t) i * 10,
						   80 + (uint64_t) i, "offline");
	CHECK(vc_sever(&laptop, &depth2, &seg) == VC_OK);
	CHECK(vc_segment_export(&laptop, seg, &(VcSource) {"cold", acs}) == VC_OK);

	before = ocs->ops->count(ocs);
	CHECK(vc_push(&laptop, &origin, "main") == VC_ERR_DETACHED);
	CHECK(phash_eq(&laptop.ctx.err_hash, &c[2]));
	CHECK_EQ(ocs->ops->count(ocs), before);
	check_store(ocs, 0);

	CHECK(vc_segment_attach(&laptop, seg) == VC_OK);
	CHECK(vc_push(&laptop, &origin, "main") == VC_OK);
	CHECK(phash_eq(&vc_head(&origin)->head, &c[4]));
	check_store(ocs, 0);

	vc_repo_free(&origin);
	vc_repo_free(&laptop);
	cs_destroy(ocs);
	cs_destroy(lcs);
	cs_destroy(acs);
}

/*
 * Attaching through a cut commit: the remote discarded B, but a new branch
 * built on B still attaches there (same hash, same history), and B itself
 * is not sent back.
 */
static void
test_attach_at_cut_commit(void)
{
	ChunkStore *ocs = memstore_create(),
			   *lcs = memstore_create();
	VcRepo		origin,
				laptop;
	Linear		l;
	uint32_t	seg;
	PHash		x;
	LogAcc		acc;

	setup(&origin, ocs);
	name_repo(&origin, "origin");
	l = build_linear(&origin);
	CHECK(vc_repo_clone(&laptop, lcs, &origin, 0) == VC_OK);
	CHECK(vc_sever(&origin, &depth2, &seg) == VC_OK);
	CHECK(vc_segment_discard(&origin, seg) == VC_OK);

	CHECK(vc_branch_create(&laptop, "hotfix", &l.b) == VC_OK);
	vc_checkout(&laptop, "hotfix");
	x = commit_rows(&laptop, 300, 310, 90, "fix on B");
	CHECK(vc_push(&laptop, &origin, "hotfix") == VC_OK);
	CHECK(phash_eq(&vc_branch_find(&origin, "hotfix")->head, &x));
	CHECK(!ocs->ops->has(ocs, &l.b));
	CHECK(vc_locate(&origin, &l.b, NULL) == VC_LOC_DISCARDED);
	CHECK(log_from(&origin, &x, 0, &acc) == VC_ERR_SEVERED && acc.n == 1);
	check_store(ocs, 0);

	vc_repo_free(&origin);
	vc_repo_free(&laptop);
	cs_destroy(ocs);
	cs_destroy(lcs);
}

/* ---------------------------------------------------------------- scaling */

#define RS_K 20

static PHash
random_commit(VcRepo *r, uint64_t ver)
{
	EditBatch	b;
	PHash		rv = vc_working(r),
				out;

	batch_init(&b, RS_K);
	for (int j = 0; j < RS_K; j++)
		batch_put(&b, rnd_below(20000), ver);
	CHECK(vc_table_edit(&r->ctx, &rv, "t", b.e, b.n, &out) == 0);
	vc_set_working(r, &out);
	batch_free(&b);
	return vc_commit(r, "random");
}

static void
test_transfer_costs(void)
{
	ChunkStore *ocs = memstore_create(),
			   *scs = memstore_create(),
			   *fcs = memstore_create();
	VcRepo		origin,
				shallow,
				full;
	ChunkStoreStats st;
	uint64_t	per_commit = 3 * RS_K * 6 + 10;
	uint64_t	fetch_new,
				push_new,
				deepen_new;
	VcMergeResult m;
	PHash		head;
	uint32_t	seg;

	rng_seed(99);
	setup(&origin, ocs);
	name_repo(&origin, "origin");
	commit_rows(&origin, 0, 20000, 0, "load");
	for (int i = 0; i < 30; i++)
		random_commit(&origin, (uint64_t) i + 1);

	CHECK(vc_repo_clone(&shallow, scs, &origin, 2) == VC_OK);
	CHECK(vc_repo_clone(&full, fcs, &origin, 0) == VC_OK);
	printf("\n  clone chunks: shallow(depth 2) %zu, full %zu\n",
		   scs->ops->count(scs), fcs->ops->count(fcs));
	/* a shallow clone is one table plus two commits of change */
	CHECK_LE(scs->ops->count(scs) + 25 * RS_K,
			 fcs->ops->count(fcs));

	for (int i = 0; i < 5; i++)
		random_commit(&origin, 100 + (uint64_t) i);
	st = scs->stats;
	CHECK(vc_fetch(&shallow, &origin, "main", &head) == VC_OK);
	fetch_new = STATS_DELTA(scs, st, new_chunks);

	CHECK(vc_branch_create(&shallow, "origin/main", &head) == VC_OK);
	CHECK(vc_merge(&shallow, "origin/main", &m) == VC_OK && m.status == VC_MERGE_FAST_FORWARD);
	vc_merge_result_free(&m);
	for (int i = 0; i < 3; i++)
		random_commit(&shallow, 200 + (uint64_t) i);
	st = ocs->stats;
	CHECK(vc_push(&shallow, &origin, "main") == VC_OK);
	push_new = STATS_DELTA(ocs, st, new_chunks);

	{
		PHash		edge = {{0}};

		for (size_t i = 0; i < shallow.loc.cap; i++)
			if (shallow.loc.used[i] && shallow.loc.vals[i] == UINT64_MAX)
				edge = shallow.loc.keys[i];
		st = scs->stats;
		CHECK(vc_deepen(&shallow, &(VcSource) {"origin", ocs}, &edge, 5, &seg) == VC_OK);
		deepen_new = STATS_DELTA(scs, st, new_chunks);
	}
	printf("  fetch 5 commits: %llu chunks, push 3: %llu, deepen 5: %llu\n%-44s",
		   (unsigned long long) fetch_new, (unsigned long long) push_new,
		   (unsigned long long) deepen_new, "");
	CHECK_LE(fetch_new, 5 * per_commit);
	CHECK_LE(push_new, 3 * per_commit);
	CHECK_LE(deepen_new, 5 * per_commit);
	check_store(scs, 0);
	check_store(ocs, 0);

	vc_repo_free(&origin);
	vc_repo_free(&shallow);
	vc_repo_free(&full);
	cs_destroy(ocs);
	cs_destroy(scs);
	cs_destroy(fcs);
}

int
main(void)
{
	RUN(test_push_does_not_resurrect);
	RUN(test_shallow_clone_with_hints);
	RUN(test_fetch_connects_past_depth);
	RUN(test_push_ff_checked_by_receiver);
	RUN(test_deepen_from_remote);
	RUN(test_restore_from_archive_alone);
	RUN(test_archive_discard_frees_only_unshared);
	RUN(test_export_to_remote_with_history);
	RUN(test_pull_deepens_for_merge_base);
	RUN(test_merge_parent_beyond_depth);
	RUN(test_explicit_restore_on_remote);
	RUN(test_branching_both_sides);
	RUN(test_unrelated_histories_rejected);
	RUN(test_detached_fetch_until_repaired);
	RUN(test_detached_push_until_attached);
	RUN(test_attach_at_cut_commit);
	RUN(test_transfer_costs);
	printf("test_remote: all tests passed\n");
	return 0;
}
