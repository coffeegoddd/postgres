/*-------------------------------------------------------------------------
 *
 * unit_engine_window.c
 *	  Unit tests of the history window: severing commits into segments,
 *	  exporting them whole to an archive, discarding them, attaching them
 *	  again, branching and merge shapes across the window's edge, stopping
 *	  at the edge, the window's floors, ceilings and low marks, and the
 *	  cost of each.
 *
 * They run on the memory store, with the memory references: they end its
 * holds, as a transaction's end would, to see what a sever, an export or
 * a discard frees.  An archive is a memory store of the test's own.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/test/modules/test_prolly/unit_engine_window.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "engine_test_util.h"
#include "utils/timestamp.h"

#define KEYS 40000

static const char *ddl = "CREATE TABLE t (id int8 PRIMARY KEY, v int8)";

static const VcWindow depth2 = {.depth = 2};

/* ---------------------------------------------------------------- helpers */

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

static void
put_rows(VcRepo *r, uint64 lo, uint64 hi, uint64 ver)
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
}

static PHash
commit_rows(VcRepo *r, uint64 lo, uint64 hi, uint64 ver, const char *msg)
{
	put_rows(r, lo, hi, ver);
	return vc_commit(r, msg);
}

/* A repository on the memory store with table t made (uncommitted). */
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

/* Rows of table t at a commit, read from ctx's store alone. */
static void
rows_at(ProllyCtx *ctx, const PHash *commit, int64 *ver)
{
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
}

typedef struct Snap
{
	int64	   *v;
} Snap;

static Snap
snap(ProllyCtx *ctx, const PHash *commit)
{
	Snap		s = {palloc(KEYS * sizeof(int64))};

	rows_at(ctx, commit, s.v);
	return s;
}

static void
check_snap(ProllyCtx *ctx, const PHash *commit, const Snap *s)
{
	int64	   *got = palloc(KEYS * sizeof(int64));

	rows_at(ctx, commit, got);
	CHECK(memcmp(got, s->v, KEYS * sizeof(int64)) == 0);
	pfree(got);
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
log_from(VcRepo *r, const PHash *start, unsigned flags, LogAcc *acc)
{
	acc->n = 0;
	return vc_log(r, start, 64, flags, log_fn, acc);
}

static void
count_tables_fn(void *arg, const VcTableChange *c)
{
	(*(int *) arg)++;
}

/* (what the window's commits, other segments and working sets hold) */
typedef struct HeldArg
{
	VcRepo	   *r;
	uint32		skip;
	PHashMap   *out;
} HeldArg;

static void
held_loc_fn(void *arg, const PHash *h, VcLocation l, uint32 seg)
{
	HeldArg    *a = arg;

	if (l == VC_LOC_WINDOW || (l == VC_LOC_SEGMENT && seg != a->skip))
		closure(a->r->ctx.cs, h, a->out);
}

static void
held_working_fn(void *arg, const VcBranch *b)
{
	HeldArg    *a = arg;

	closure(a->r->ctx.cs, &b->working, a->out);
}

/*
 * The chunks letting go of segment seg must free: its members' closure
 * minus all else held (the window's commits, other local segments, and the
 * branches' working sets)
 */
static size_t
expected_frees(VcRepo *r, uint32 seg)
{
	PHashMap	mine,
				other,
				only;
	VcSegment	s;
	HeldArg		a = {r, seg, &other};
	size_t		n;

	CHECK(vc_segment_get(r, seg, &s));
	phmap_init(&mine);
	for (size_t i = 0; i < s.n; i++)
		closure(r->ctx.cs, &s.commits[i], &mine);
	vc_segment_free(&s);
	phmap_init(&other);
	vc_locations(r, held_loc_fn, &a);
	vc_branch_list(r, held_working_fn, &a);
	set_minus(&mine, &other, &only);
	n = set_size(&only);
	phmap_free(&mine);
	phmap_free(&other);
	phmap_free(&only);
	return n;
}

static size_t
segment_size(VcRepo *r, uint32 seg)
{
	VcSegment	s;
	size_t		n;

	if (!vc_segment_get(r, seg, &s))
		return 0;
	n = s.n;
	vc_segment_free(&s);
	return n;
}

/*
 * What the memory store frees as its holds end (as the transaction's end
 * would, and the freeing after it): the commits' chunks a call let go of.
 */
static uint64
release(ChunkStore *cs)
{
	if (!engine_test_on_memory())
		return 0;				/* (held until the test's subtransaction ends) */
	MemoryChunkStoreEndHolds(cs);
	return cs_free_unheld(cs);
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

static VcSource
archive_of(ChunkStore *archive)
{
	VcSource	s = {"archive", archive};

	return s;
}

/* ------------------------------------------------------------------ tests */

/* Severing A <- B to keep C <- D moves holds only: nothing copied or freed. */
ENGINE_TEST(test_detach_moves_holds_only)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	Linear		l;
	uint32		seg,
				seg2;
	ChunkStoreStats st;
	uint64		count;
	LogAcc		acc;
	PHash		mb;
	Snap		sb;
	int			n = 0;

	setup(&r, cs);
	l = build_linear(&r);
	sb = snap(&r.ctx, &l.b);
	release(cs);
	count = store_count(cs);
	st = *cs_stats(cs);
	CHECK(vc_sever(&r, &depth2, &seg) == VC_OK && seg != VC_NO_SEGMENT);
	/* (the window's commits, branches' and repository's chunks read) */
	CHECK_LE(STATS_DELTA(cs, st, gets), 2 * depth2.depth + 6);
	/* (a segment's chunk, the location map's change, a repository's) */
	CHECK_LE(STATS_DELTA(cs, st, stored), 4 * 3);
	/* nothing of history freed: the segment holds what the commits did */
	CHECK_LE(release(cs), 3);	/* (the replaced repository chunk, map) */
	CHECK_LE(store_count(cs), count + 4 * 3);
	settle_and_check(cs);

	CHECK(vc_locate(&r, &l.d, NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&r, &l.c, NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&r, &l.b, NULL) == VC_LOC_SEGMENT);
	CHECK(vc_locate(&r, &l.a, NULL) == VC_LOC_SEGMENT);
	CHECK(vc_locate(&r, &l.init, NULL) == VC_LOC_SEGMENT);
	CHECK_EQ(segment_size(&r, seg), 3);

	/* still local: reads past the edge work, unless told to stop there */
	CHECK(log_from(&r, &l.d, 0, &acc) == VC_OK && acc.n == 5);
	CHECK(log_from(&r, &l.d, VC_WINDOW_ONLY, &acc) == VC_ERR_OUTSIDE_WINDOW);
	CHECK(acc.n == 2 && phash_eq(&r.ctx.err_hash, &l.b));
	CHECK(vc_merge_base_in(&r, &l.d, &l.b, 0, &mb) == 1 && phash_eq(&mb, &l.b));
	CHECK(vc_merge_base_in(&r, &l.d, &l.b, VC_WINDOW_ONLY, &mb) == VC_ERR_OUTSIDE_WINDOW);
	CHECK(vc_merge_base_in(&r, &l.d, &l.c, VC_WINDOW_ONLY, &mb) == 1 && phash_eq(&mb, &l.c));
	CHECK(vc_diff_commits(&r, &l.b, &l.d, 0, count_tables_fn, &n) == VC_OK && n == 1);
	CHECK(vc_diff_commits(&r, &l.b, &l.d, VC_WINDOW_ONLY, count_tables_fn, &n) ==
		  VC_ERR_OUTSIDE_WINDOW);
	CHECK(vc_diff_commits(&r, &l.c, &l.d, VC_WINDOW_ONLY, count_tables_fn, &n) == VC_OK);
	check_snap(&r.ctx, &l.b, &sb);

	/* nothing new to cut: a second sever makes no segment */
	CHECK(vc_sever(&r, &depth2, &seg2) == VC_OK && seg2 == VC_NO_SEGMENT);

	/* attach: the holds move back, nothing of history copied or freed */
	st = *cs_stats(cs);
	CHECK_EQ(vc_segment_attach(&r, seg, NULL), VC_OK);
	CHECK_LE(STATS_DELTA(cs, st, stored), 3);	/* (map and repository) */
	CHECK_LE(release(cs), 4);	/* (the segment's chunk; map and repository) */
	CHECK(vc_locate(&r, &l.a, NULL) == VC_LOC_WINDOW);
	CHECK(!vc_segment_get(&r, seg, &(VcSegment) {0}));
	CHECK(log_from(&r, &l.d, VC_WINDOW_ONLY, &acc) == VC_OK && acc.n == 5);
	check_snap(&r.ctx, &l.b, &sb);
	settle_and_check(cs);
	pfree(sb.v);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* Export: a self-contained A <- B in the archive, freed here, exactly. */
ENGINE_TEST(test_export_self_contained)
{
	ChunkStore *cs = memstore_create();
	ChunkStore *archive = archive_store_create();
	VcSource	arch = archive_of(archive);
	ProllyCtx	actx;
	VcRepo		r;
	Linear		l;
	uint32		seg;
	Snap		sa,
				sb;
	size_t		want;
	ChunkStoreStats st;
	LogAcc		acc;
	PHash		mb;
	int			n = 0;
	uint64		diff_before,
				freed;

	setup(&r, cs);
	l = build_linear(&r);
	sa = snap(&r.ctx, &l.a);
	sb = snap(&r.ctx, &l.b);
	st = *cs_stats(cs);
	CHECK(vc_diff_commits(&r, &l.c, &l.d, 0, count_tables_fn, &n) == VC_OK);
	diff_before = STATS_DELTA(cs, st, gets);

	CHECK_EQ(vc_sever(&r, &depth2, &seg), VC_OK);
	release(cs);
	want = expected_frees(&r, seg);
	CHECK(want > 0);
	CHECK_EQ(vc_segment_export(&r, seg, &arch), VC_OK);
	/*
	 * (the members' own chunks, and the segment's chunk it replaced, and the
	 * location map's and repository's: the members marked exported)
	 */
	freed = release(cs);
	if (engine_test_on_memory())
		CHECK_EQ(freed, want + 3);
	settle_and_check(cs);
	CHECK(vc_locate(&r, &l.b, NULL) == VC_LOC_EXPORTED);

	/* the archive alone holds A and B whole */
	prolly_ctx_init(&actx, archive, &small_policy);
	check_snap(&actx, &l.a, &sa);
	check_snap(&actx, &l.b, &sb);
	CHECK(cs_has(archive, l.init.b));
	CHECK(!cs_has(archive, l.c.b));
	if (engine_test_on_memory())
	{
		MemoryChunkStoreEndHolds(archive);
		check_store(archive, 0);
	}

	/* here, history ends at the cut (gone from the store once the holds
	 * end; past the window's edge in any case) */
	if (engine_test_on_memory())
		CHECK(log_from(&r, &l.d, 0, &acc) == VC_ERR_SEVERED);
	else
		CHECK(log_from(&r, &l.d, VC_WINDOW_ONLY, &acc) == VC_ERR_OUTSIDE_WINDOW);
	CHECK(acc.n == 2 && phash_eq(&r.ctx.err_hash, &l.b));
	if (engine_test_on_memory())
	{
		CHECK(vc_merge_base_in(&r, &l.d, &l.b, 0, &mb) == VC_ERR_SEVERED);
		CHECK(vc_diff_commits(&r, &l.a, &l.d, 0, count_tables_fn, &n) == VC_ERR_SEVERED);
	}
	CHECK(vc_merge_base_in(&r, &l.d, &l.b, VC_WINDOW_ONLY, &mb) == VC_ERR_OUTSIDE_WINDOW);
	CHECK(vc_diff_commits(&r, &l.a, &l.d, VC_WINDOW_ONLY, count_tables_fn, &n) == VC_ERR_OUTSIDE_WINDOW);
	CHECK(vc_reset_hard(&r, &l.b) == VC_ERR_OUTSIDE_WINDOW);

	/* work inside the window costs what it did */
	st = *cs_stats(cs);
	CHECK(vc_diff_commits(&r, &l.c, &l.d, VC_WINDOW_ONLY, count_tables_fn, &n) == VC_OK);
	CHECK_EQ(STATS_DELTA(cs, st, gets), diff_before);

	/* attached from the archive: exactly what was freed comes back */
	st = *cs_stats(cs);
	CHECK_EQ(vc_segment_attach(&r, seg, &arch), VC_OK);
	if (engine_test_on_memory())
		CHECK_EQ(STATS_DELTA(cs, st, stored), freed - 3 + 2);	/* (+ map, repository) */
	settle_and_check(cs);
	check_snap(&r.ctx, &l.b, &sb);
	CHECK(log_from(&r, &l.d, VC_WINDOW_ONLY, &acc) == VC_OK && acc.n == 5);
	CHECK(phash_eq(&acc.v[2], &l.b) && phash_eq(&acc.v[4], &l.init));
	/* and the archive lets go of it: its manifest's root is gone */
	if (engine_test_on_memory())
	{
		MemoryChunkStoreEndHolds(archive);
		(void) cs_free_unheld(archive);
		CHECK_EQ(store_count(archive), 0);
	}

	pfree(sa.v);
	pfree(sb.v);
	prolly_ctx_free(&actx);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* Sliding the window and exporting again sends only what changed. */
ENGINE_TEST(test_second_export_copies_diff)
{
	ChunkStore *cs = memstore_create();
	ChunkStore *archive = archive_store_create();
	VcSource	arch = archive_of(archive);
	ProllyCtx	actx;
	VcRepo		r;
	Linear		l;
	uint32		s1,
				s2;
	uint64		first,
				second;
	ChunkStoreStats st;
	Snap		sc,
				sd;
	int			h;
	VcTable		t;
	VcCommit	c;
	PHash		head;

	setup(&r, cs);
	l = build_linear(&r);
	CHECK_EQ(vc_sever(&r, &depth2, &s1), VC_OK);
	st = *cs_stats(archive);
	CHECK_EQ(vc_segment_export(&r, s1, &arch), VC_OK);
	first = STATS_DELTA(archive, st, stored);

	commit_rows(&r, 5000, 5100, 4, "E");
	commit_rows(&r, 7000, 7100, 5, "F");
	sc = snap(&r.ctx, &l.c);
	sd = snap(&r.ctx, &l.d);
	CHECK(vc_sever(&r, &depth2, &s2) == VC_OK && s2 != VC_NO_SEGMENT);
	CHECK_EQ(segment_size(&r, s2), 2);	/* C and D */
	st = *cs_stats(archive);
	CHECK_EQ(vc_segment_export(&r, s2, &arch), VC_OK);
	second = STATS_DELTA(archive, st, stored);
	head = head_of(&r);
	CHECK(vc_commit_load(&r.ctx, &head, &c));
	CHECK(vc_table_get(&r.ctx, &c.root, "t", &t));
	vc_commit_release(&c);
	h = t_height(&r.ctx, &t.data);
	/* two commits of 300 rows each: about rows x height chunks, not the table */
	CHECK_LE(second, 2 * 3 * 300 * (uint64) h + 20);
	CHECK_LE(second * 4, first);

	prolly_ctx_init(&actx, archive, &small_policy);
	check_snap(&actx, &l.c, &sc);
	check_snap(&actx, &l.d, &sd);
	if (engine_test_on_memory())
	{
		MemoryChunkStoreEndHolds(archive);
		check_store(archive, 0);
	}
	settle_and_check(cs);

	pfree(sc.v);
	pfree(sd.v);
	prolly_ctx_free(&actx);
	vc_repo_free(&r);
	cs_destroy(cs);
}

ENGINE_TEST(test_discard_frees_exactly)
{
	ChunkStore *cs = memstore_create();
	ChunkStore *archive = archive_store_create();
	VcSource	arch = archive_of(archive);
	VcRepo		r;
	Linear		l;
	uint32		seg;
	size_t		want;
	RepoStateMark state;
	LogAcc		acc;
	Snap		sd;

	setup(&r, cs);
	l = build_linear(&r);
	sd = snap(&r.ctx, &l.d);
	CHECK_EQ(vc_sever(&r, &depth2, &seg), VC_OK);
	release(cs);
	want = expected_frees(&r, seg);
	repo_state_mark(&r, &state);
	CHECK_EQ(vc_segment_discard(&r, seg, NULL), VC_OK);
	/* (the members' own chunks, the segment's, and the repository's own it left) */
	if (engine_test_on_memory())
		CHECK_EQ(release(cs), want + 1 + repo_state_unheld(&r, &state));
	settle_and_check(cs);
	/* C names B, so B is known as discarded; A, below it, is forgotten */
	CHECK(vc_locate(&r, &l.b, NULL) == VC_LOC_DISCARDED);
	CHECK(vc_locate(&r, &l.a, NULL) == VC_LOC_UNKNOWN);
	CHECK_EQ(segment_size(&r, seg), 0);
	check_snap(&r.ctx, &l.d, &sd);
	/* (gone from the store once the holds end; past the window's edge in any case) */
	if (engine_test_on_memory())
		CHECK(log_from(&r, &l.d, 0, &acc) == VC_ERR_SEVERED && acc.n == 2);
	CHECK(log_from(&r, &l.d, VC_WINDOW_ONLY, &acc) == VC_ERR_OUTSIDE_WINDOW && acc.n == 2);
	/* a segment discarded is no segment */
	CHECK_EQ(vc_segment_attach(&r, seg, NULL), VC_ERR);
	CHECK_EQ(vc_segment_export(&r, seg, &arch), VC_ERR);
	CHECK_EQ(vc_segment_discard(&r, seg, NULL), VC_ERR);

	pfree(sd.v);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/*
 * main:    init <- A <- B <- C <- D
 * feature:              B <- E <- F
 * With depth 2 the window is {C, D, E, F}; both branches cut at B.
 */
ENGINE_TEST(test_branches_cut_at_shared_ancestor)
{
	ChunkStore *cs = memstore_create();
	ChunkStore *archive = archive_store_create();
	VcSource	arch = archive_of(archive);
	VcRepo		r;
	PHash		init,
				a,
				b,
				c,
				d,
				e,
				f,
				mb,
				working,
				w;
	uint32		seg;
	LogAcc		acc;
	VcMergeResult m;
	VcBranch	br;
	VcTable		t;
	const uint8 *v;
	size_t		vl;
	uint8		k[8];

	setup(&r, cs);
	init = head_of(&r);
	a = commit_rows(&r, 0, 20000, 0, "A");
	b = commit_rows(&r, 0, 200, 1, "B");
	CHECK_EQ(vc_branch_create(&r, "feature", NULL), VC_OK);
	c = commit_rows(&r, 3000, 3200, 2, "C");
	d = commit_rows(&r, 6000, 6200, 3, "D");
	CHECK_EQ(vc_checkout(&r, "feature"), VC_OK);
	e = commit_rows(&r, 12000, 12200, 4, "E");
	f = commit_rows(&r, 15000, 15200, 5, "F");
	CHECK_EQ(vc_checkout(&r, "main"), VC_OK);

	CHECK_EQ(vc_sever(&r, &depth2, &seg), VC_OK);
	CHECK(vc_locate(&r, &c, NULL) == VC_LOC_WINDOW && vc_locate(&r, &d, NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&r, &e, NULL) == VC_LOC_WINDOW && vc_locate(&r, &f, NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&r, &b, NULL) == VC_LOC_SEGMENT && vc_locate(&r, &a, NULL) == VC_LOC_SEGMENT);
	CHECK(vc_locate(&r, &init, NULL) == VC_LOC_SEGMENT);
	settle_and_check(cs);

	/* both branches stop at the shared ancestor */
	CHECK(log_from(&r, &f, VC_WINDOW_ONLY, &acc) == VC_ERR_OUTSIDE_WINDOW && acc.n == 2);
	CHECK(phash_eq(&r.ctx.err_hash, &b));
	CHECK(log_from(&r, &d, VC_WINDOW_ONLY, &acc) == VC_ERR_OUTSIDE_WINDOW && acc.n == 2);
	/* window-only: the base is named, but it lies past the edge */
	CHECK(vc_merge_base_in(&r, &d, &f, VC_WINDOW_ONLY, &mb) == VC_ERR_OUTSIDE_WINDOW);
	CHECK(phash_eq(&r.ctx.err_hash, &b));
	CHECK(vc_merge_base_in(&r, &d, &f, 0, &mb) == 1 && phash_eq(&mb, &b));

	/* exported: merging the branches needs their base, which is gone */
	CHECK_EQ(vc_segment_export(&r, seg, &arch), VC_OK);
	settle_and_check(cs);
	working = vc_working(&r);
	/* (gone from the store once the holds end: until then, readable) */
	if (engine_test_on_memory())
	{
		CHECK_EQ(vc_merge(&r, "feature", &m), VC_ERR_SEVERED);
		CHECK(phash_eq(&r.ctx.err_hash, &b));
		vc_head(&r, &br);
		CHECK(phash_eq(&working, &br.working) && !br.merging);
		vc_branch_free(&br);
	}

	/* attached from the archive, the merge goes through */
	CHECK_EQ(vc_segment_attach(&r, seg, &arch), VC_OK);
	CHECK(vc_merge(&r, "feature", &m) == VC_OK && m.status == VC_MERGE_COMMITTED);
	vc_merge_result_free(&m);
	w = vc_working(&r);
	CHECK(vc_table_get(&r.ctx, &w, "t", &t));
	put_u64(k, 15000);
	CHECK(t_get(&r.ctx, &t.data, k, 8, &v, &vl) && get_u64(v + 8) == 5);
	put_u64(k, 6000);
	CHECK(t_get(&r.ctx, &t.data, k, 8, &v, &vl) && get_u64(v + 8) == 3);
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/*
 * A branch whose head is old keeps that head in the window; the commits
 * between it and the window around main are cut.
 * old:  A;  main: init <- A <- B <- C <- D
 */
ENGINE_TEST(test_stale_branch_head_stays)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	PHash		init,
				a,
				b,
				d,
				mb;
	uint32		seg;
	LogAcc		acc;
	Snap		sa;

	setup(&r, cs);
	init = head_of(&r);
	a = commit_rows(&r, 0, 20000, 0, "A");
	CHECK_EQ(vc_branch_create(&r, "old", NULL), VC_OK);
	b = commit_rows(&r, 0, 500, 1, "B");
	commit_rows(&r, 8000, 8500, 2, "C");
	d = commit_rows(&r, 16000, 16500, 3, "D");
	sa = snap(&r.ctx, &a);

	CHECK_EQ(vc_sever(&r, &depth2, &seg), VC_OK);
	/* old's window (its head and parent) keeps A and init; only B is cut */
	CHECK(vc_locate(&r, &a, NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&r, &init, NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&r, &b, NULL) == VC_LOC_SEGMENT);
	CHECK_EQ(segment_size(&r, seg), 1);

	CHECK_EQ(vc_segment_discard(&r, seg, NULL), VC_OK);
	settle_and_check(cs);
	CHECK_EQ(vc_checkout(&r, "old"), VC_OK);
	check_snap(&r.ctx, &a, &sa);
	CHECK(log_from(&r, &a, VC_WINDOW_ONLY, &acc) == VC_OK && acc.n == 2);
	/* (gone from the store once the holds end; past the window's edge in any case) */
	if (engine_test_on_memory())
		CHECK(log_from(&r, &d, 0, &acc) == VC_ERR_SEVERED && acc.n == 2);
	CHECK(log_from(&r, &d, VC_WINDOW_ONLY, &acc) == VC_ERR_OUTSIDE_WINDOW && acc.n == 2);
	CHECK(phash_eq(&r.ctx.err_hash, &b));
	/* A is D's ancestor only through B, which is gone: unknown, not "no" */
	if (engine_test_on_memory())
		CHECK(vc_merge_base_in(&r, &d, &a, 0, &mb) == VC_ERR_SEVERED);
	CHECK(vc_merge_base_in(&r, &d, &a, VC_WINDOW_ONLY, &mb) == VC_ERR_OUTSIDE_WINDOW);

	pfree(sa.v);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/*
 * A merge commit whose first parent is cut and second parent is a branch
 * head: M = merge(C, E), depth 1 keeps only M and E.
 */
ENGINE_TEST(test_merge_commit_parents_across_edge)
{
	ChunkStore *cs = memstore_create();
	ChunkStore *archive = archive_store_create();
	VcSource	arch = archive_of(archive);
	VcRepo		r;
	PHash		c,
				e,
				mm;
	uint32		seg;
	VcMergeResult m;
	VcCommit	cm;
	LogAcc		acc;
	VcWindow	depth1 = {.depth = 1};
	Snap		se;

	setup(&r, cs);
	commit_rows(&r, 0, 20000, 0, "A");
	commit_rows(&r, 0, 100, 1, "B");
	CHECK_EQ(vc_branch_create(&r, "feature", NULL), VC_OK);
	c = commit_rows(&r, 4000, 4100, 2, "C");
	CHECK_EQ(vc_checkout(&r, "feature"), VC_OK);
	e = commit_rows(&r, 9000, 9100, 3, "E");
	se = snap(&r.ctx, &e);
	CHECK_EQ(vc_checkout(&r, "main"), VC_OK);
	CHECK(vc_merge(&r, "feature", &m) == VC_OK && m.status == VC_MERGE_COMMITTED);
	mm = m.commit;
	vc_merge_result_free(&m);

	CHECK_EQ(vc_sever(&r, &depth1, &seg), VC_OK);
	CHECK(vc_locate(&r, &mm, NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&r, &e, NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&r, &c, NULL) == VC_LOC_SEGMENT);
	CHECK_EQ(vc_segment_export(&r, seg, &arch), VC_OK);
	settle_and_check(cs);

	CHECK(vc_commit_load(&r.ctx, &mm, &cm) && cm.nparents == 2);
	vc_commit_release(&cm);
	/* (gone from the store once the holds end; past the window's edge in any case) */
	if (engine_test_on_memory())
		CHECK(log_from(&r, &mm, 0, &acc) == VC_ERR_SEVERED && acc.n == 1);
	CHECK(log_from(&r, &mm, VC_WINDOW_ONLY, &acc) == VC_ERR_OUTSIDE_WINDOW && acc.n == 1);
	CHECK(phash_eq(&r.ctx.err_hash, &c));
	CHECK_EQ(vc_checkout(&r, "feature"), VC_OK);
	check_snap(&r.ctx, &e, &se);

	pfree(se.v);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* Overlapping windows on two branches cut only what neither covers. */
ENGINE_TEST(test_overlapping_branch_windows)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	PHash		h[8];
	uint32		seg;
	VcWindow	depth3 = {.depth = 3};

	/* main: init, 1..6; side from 3: s1, s2 */
	setup(&r, cs);
	h[0] = head_of(&r);
	h[1] = commit_rows(&r, 0, 20000, 0, "1");
	h[2] = commit_rows(&r, 0, 100, 1, "2");
	h[3] = commit_rows(&r, 100, 200, 2, "3");
	CHECK_EQ(vc_branch_create(&r, "side", NULL), VC_OK);
	h[4] = commit_rows(&r, 200, 300, 3, "4");
	h[5] = commit_rows(&r, 300, 400, 4, "5");
	h[6] = commit_rows(&r, 400, 500, 5, "6");
	CHECK_EQ(vc_checkout(&r, "side"), VC_OK);
	commit_rows(&r, 30000, 30100, 6, "s1");
	h[7] = commit_rows(&r, 30100, 30200, 7, "s2");
	CHECK_EQ(vc_checkout(&r, "main"), VC_OK);

	/* main covers 4..6; side (s2 gen 6) covers s2, s1 and 3 */
	CHECK_EQ(vc_sever(&r, &depth3, &seg), VC_OK);
	CHECK(vc_locate(&r, &h[6], NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&r, &h[4], NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&r, &h[3], NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&r, &h[7], NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&r, &h[2], NULL) == VC_LOC_SEGMENT);
	CHECK(vc_locate(&r, &h[1], NULL) == VC_LOC_SEGMENT);
	CHECK(vc_locate(&r, &h[0], NULL) == VC_LOC_SEGMENT);
	CHECK_EQ(segment_size(&r, seg), 3);
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/*
 * One run of test_wider_window_stays_cut: main init, A, B, C, D, a branch
 * old at A.  Depth 2 cuts B alone (old keeps A and init); old dropped, a
 * window over all five is severed while B is still readable (in its
 * segment, and on the history store its chunks not yet freed).
 */
static void
wider_window_run(const VcWindow *wide)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	Linear		l;
	uint32		seg;
	VcWindow	depth2 = {.depth = 2};
	VcCommit	c;

	setup(&r, cs);
	l.init = head_of(&r);
	l.a = commit_rows(&r, 0, 20000, 0, "A");
	CHECK_EQ(vc_branch_create(&r, "old", NULL), VC_OK);
	l.b = commit_rows(&r, 0, 300, 1, "B");
	l.c = commit_rows(&r, 10000, 10300, 2, "C");
	l.d = commit_rows(&r, 19000, 19300, 3, "D");
	CHECK_EQ(vc_sever(&r, &depth2, &seg), VC_OK);
	CHECK(vc_locate(&r, &l.b, NULL) == VC_LOC_SEGMENT);
	CHECK(vc_locate(&r, &l.a, NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&r, &l.init, NULL) == VC_LOC_WINDOW);
	CHECK_EQ(vc_branch_delete(&r, "old"), VC_OK);
	CHECK(vc_commit_load(&r.ctx, &l.b, &c));
	vc_commit_release(&c);

	/* A and init are reached only through B, cut: they are cut too */
	CHECK_EQ(vc_sever(&r, wide, &seg), VC_OK);
	CHECK(seg != VC_NO_SEGMENT);
	CHECK(vc_locate(&r, &l.a, NULL) == VC_LOC_SEGMENT);
	CHECK(vc_locate(&r, &l.init, NULL) == VC_LOC_SEGMENT);
	CHECK(vc_locate(&r, &l.c, NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&r, &l.d, NULL) == VC_LOC_WINDOW);
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/*
 * A wider window does not bring back what is cut: a sever stops at a commit
 * without a reference, though it can still be read, and does not keep what
 * lies past it.  (With a depth alone, with a ceiling too, and with a low
 * mark.)
 */
ENGINE_TEST(test_wider_window_stays_cut)
{
	wider_window_run(&(VcWindow) {.depth = 5});
	wider_window_run(&(VcWindow) {.depth = 5,.max_commits = 100});
	/* (past B, A is held below the ceiling: walked through, D's window would
	 * be at its high mark, and cut back to D) */
	wider_window_run(&(VcWindow) {.depth = 5,.max_commits = 3,.low_commits = 1});
}

ENGINE_TEST(test_pinned_commit_stays)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	Linear		l;
	uint32		seg;
	VcWindow	w = {.depth = 1};

	setup(&r, cs);
	l = build_linear(&r);
	w.pinned = &l.a;
	w.npinned = 1;
	CHECK_EQ(vc_sever(&r, &w, &seg), VC_OK);
	CHECK(vc_locate(&r, &l.a, NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&r, &l.d, NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&r, &l.b, NULL) == VC_LOC_SEGMENT);
	CHECK(vc_locate(&r, &l.c, NULL) == VC_LOC_SEGMENT);
	CHECK_EQ(vc_segment_discard(&r, seg, NULL), VC_OK);
	settle_and_check(cs);
	CHECK_EQ(vc_branch_create(&r, "tag-a", &l.a), VC_OK);
	vc_repo_free(&r);
	cs_destroy(cs);
}

ENGINE_TEST(test_refuse_outside_window)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	Linear		l;
	uint32		seg;
	VcWindow	bad = {.depth = 0};
	VcWindow	low_only = {.low_commits = 1};
	VcBranch	none;

	setup(&r, cs);
	l = build_linear(&r);
	CHECK_EQ(vc_sever(&r, &bad, &seg), VC_ERR);
	CHECK_EQ(vc_sever(&r, &low_only, &seg), VC_ERR);
	CHECK_EQ(vc_sever(&r, &depth2, &seg), VC_OK);
	CHECK_EQ(vc_reset_hard(&r, &l.a), VC_ERR_OUTSIDE_WINDOW);
	CHECK_EQ(vc_reset_soft(&r, &l.a), VC_ERR_OUTSIDE_WINDOW);
	CHECK_EQ(vc_branch_create(&r, "x", &l.b), VC_ERR_OUTSIDE_WINDOW);
	CHECK(!vc_branch_get(&r, "x", &none));
	CHECK_EQ(vc_reset_hard(&r, &l.c), VC_OK);
	CHECK_EQ(vc_segment_attach(&r, seg, NULL), VC_OK);
	CHECK_EQ(vc_reset_hard(&r, &l.a), VC_OK);
	CHECK_EQ(vc_branch_create(&r, "x", &l.b), VC_OK);
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* ---------------------------------------------------------------- scaling */

typedef struct WinScale
{
	int			history;
	uint64		sever_reads;
	uint64		sever_writes;
	uint64		sever_cut;
	uint64		first_export;
	uint64		slide_export;
	uint64		diff_reads_before;
	uint64		diff_reads_after;
	uint64		commit_writes_after;
} WinScale;

#define WS_ROWS 30000
#define WS_K 20

static PHash
random_commit(VcRepo *r, int k, uint64 ver, const char *msg)
{
	EditBatch	b;
	PHash		rv = vc_working_lock(r),
				out;

	batch_init(&b, (size_t) k);
	for (int j = 0; j < k; j++)
		batch_put(&b, rnd_below_global(WS_ROWS), ver);
	CHECK_EQ(vc_table_edit(&r->ctx, &rv, "t", b.e, b.n, &out), VC_OK);
	vc_set_working(r, &out);
	batch_free(&b);
	return vc_commit(r, msg);
}

static void
win_scale_case(int history, WinScale *s)
{
	ChunkStore *cs = memstore_create();
	ChunkStore *archive = archive_store_create();
	VcSource	arch = archive_of(archive);
	VcRepo		r;
	VcWindow	w = {.depth = 5};
	uint32		seg;
	ChunkStoreStats st;
	PHash		prev = {{0}},
				last = {{0}};
	int			n = 0;

	memset(s, 0, sizeof(*s));
	s->history = history;
	rng_seed_global((uint64) history);
	setup(&r, cs);
	commit_rows(&r, 0, WS_ROWS, 0, "load");
	for (int i = 0; i < history; i++)
	{
		prev = head_of(&r);
		last = random_commit(&r, WS_K, (uint64) i + 1, "step");
	}
	st = *cs_stats(cs);
	CHECK_EQ(vc_diff_commits(&r, &prev, &last, 0, count_tables_fn, &n), VC_OK);
	s->diff_reads_before = STATS_DELTA(cs, st, gets);

	st = *cs_stats(cs);
	CHECK_EQ(vc_sever(&r, &w, &seg), VC_OK);
	s->sever_reads = STATS_DELTA(cs, st, gets);
	s->sever_writes = STATS_DELTA(cs, st, stored);
	s->sever_cut = segment_size(&r, seg);
	st = *cs_stats(archive);
	CHECK_EQ(vc_segment_export(&r, seg, &arch), VC_OK);
	s->first_export = STATS_DELTA(archive, st, stored);

	/* slide the window by 5 commits and export again */
	for (int i = 0; i < 5; i++)
	{
		prev = head_of(&r);
		st = *cs_stats(cs);
		last = random_commit(&r, WS_K, 1000 + (uint64) i, "slide");
		s->commit_writes_after = STATS_DELTA(cs, st, stored);
	}
	CHECK(vc_sever(&r, &w, &seg) == VC_OK && seg != VC_NO_SEGMENT);
	CHECK_EQ(segment_size(&r, seg), 5);
	st = *cs_stats(archive);
	CHECK_EQ(vc_segment_export(&r, seg, &arch), VC_OK);
	s->slide_export = STATS_DELTA(archive, st, stored);

	st = *cs_stats(cs);
	CHECK_EQ(vc_diff_commits(&r, &prev, &last, VC_WINDOW_ONLY, count_tables_fn, &n), VC_OK);
	s->diff_reads_after = STATS_DELTA(cs, st, gets);
	settle_and_check(cs);
	if (engine_test_on_memory())
	{
		MemoryChunkStoreEndHolds(archive);
		check_store(archive, 0);
	}
	vc_repo_free(&r);
	cs_destroy(cs);
}

ENGINE_TEST(test_window_scaling)
{
	static const int histories[] = {20, 400};
	WinScale	s[2];

	for (int i = 0; i < 2; i++)
	{
		win_scale_case(histories[i], &s[i]);

		/*
		 * detach reads the window only (its commits, the branch's chunk and
		 * head, the repository's): the commits cut are listed by their
		 * references, not read
		 */
		CHECK_LE(s[i].sever_reads, 2 * 5 + 5);
		/*
		 * and writes a segment's chunk, a repository's, and the location
		 * map's nodes the commits cut change: no chunk of history
		 */
		CHECK_LE(s[i].sever_writes, 2 + s[i].sever_cut);
		/* sliding 5 commits exports about 5 commits of change */
		CHECK_LE(s[i].slide_export, 5 * (3 * WS_K * 6 + 10));
		/* inside the window nothing got slower */
		CHECK_EQ(s[i].diff_reads_after, s[i].diff_reads_before);
		/* a commit: its chunk and its branch's, the tree's changes aside */
		CHECK_LE(s[i].commit_writes_after, 3 * WS_K * 6 + 8);
	}
	/* 20x the history: sliding costs the same (and severing, the window) */
	CHECK_LE(s[1].slide_export, 2 * s[0].slide_export);
}

typedef struct CutCount
{
	int			discarded,
				window,
				other;
} CutCount;

static void
cut_count_fn(void *arg, const PHash *h, VcLocation where, uint32 seg)
{
	CutCount   *c = arg;

	c->discarded += where == VC_LOC_DISCARDED;
	c->window += where == VC_LOC_WINDOW;
	c->other += where != VC_LOC_DISCARDED && where != VC_LOC_WINDOW;
}

/* The repository's own state: its chunk and its location map's, in bytes. */
static uint64
state_bytes(VcRepo *r)
{
	VcRefs	   *refs = t_refs(r->ctx.cs);
	PHash		repo;
	PHashMap	set;
	uint64		n = 0;

	CHECK(vc_refs_get(refs, "vc/repo", VC_REF_READ, &repo));
	phmap_init(&set);
	closure(r->ctx.cs, &repo, &set);
	for (size_t i = 0; i < set.cap; i++)
		if (set.used[i])
		{
			uint32		len;
			void	   *d = cs_get(r->ctx.cs, set.keys[i].b, &len);

			CHECK(d != NULL);
			n += len;
			pfree(d);
		}
	phmap_free(&set);
	return n;
}

/*
 * The repository's state is the size of what it holds: commits cut and
 * discarded leave nothing in it.  The commit just past the oldest kept one
 * is known as discarded; an older edge is forgotten once the commit
 * naming it goes.  A second view of the repository sees the same.
 */
ENGINE_TEST(test_state_does_not_grow_with_history)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	PHash		head,
				edge,
				first_edge = {{0}};
	uint64		bytes10 = 0,
				bytes = 0;
	uint32		seg;
	LogAcc		acc;
	CutCount	cc = {0};

	setup(&r, cs);
	for (int i = 1; i <= 400; i++)
	{
		commit_rows(&r, (uint64) i, (uint64) i + 1, (uint64) i, "step");
		CHECK_EQ(vc_sever(&r, &(VcWindow) {.depth = 5}, &seg), VC_OK);
		if (seg != VC_NO_SEGMENT)
			CHECK_EQ(vc_segment_discard(&r, seg, NULL), VC_OK);
		/* (what is discarded, freed: its transaction ended) */
		release(cs);
		if (i == 10)
		{
			head = head_of(&r);
			/* (gone from the store once the holds end; past the window's
			 * edge in any case) */
			if (engine_test_on_memory())
				CHECK_EQ(log_from(&r, &head, 0, &acc), VC_ERR_SEVERED);
			else
				CHECK_EQ(log_from(&r, &head, VC_WINDOW_ONLY, &acc), VC_ERR_OUTSIDE_WINDOW);
			first_edge = r.ctx.err_hash;
			CHECK(vc_locate(&r, &first_edge, NULL) == VC_LOC_DISCARDED);
		}
		if (i == 10 || i == 400)
		{
			settle_and_check(cs);
			bytes = state_bytes(&r);
			if (i == 10)
				bytes10 = bytes;
		}
	}
	CHECK(bytes10 > 0);
	/* a varint grows a byte as segment ids move on: not the 390 commits cut */
	CHECK_LE(bytes, bytes10 + 16);
	head = head_of(&r);
	/* (gone from the store once the holds end; past the window's edge in any case) */
	if (engine_test_on_memory())
		CHECK(log_from(&r, &head, 0, &acc) == VC_ERR_SEVERED && acc.n == 5);
	CHECK(log_from(&r, &head, VC_WINDOW_ONLY, &acc) == VC_ERR_OUTSIDE_WINDOW && acc.n == 5);
	edge = r.ctx.err_hash;
	CHECK(vc_locate(&r, &edge, NULL) == VC_LOC_DISCARDED);
	CHECK(vc_locate(&r, &first_edge, NULL) == VC_LOC_UNKNOWN);
	vc_locations(&r, cut_count_fn, &cc);
	CHECK_EQ(cc.window, 5);
	CHECK_LE(cc.discarded, 1);
	CHECK_EQ(cc.other, 0);
	vc_repo_free(&r);

	/* another view of it: the same */
	vc_repo_open(&r, cs, t_refs(cs), &small_policy);
	CHECK(vc_locate(&r, &edge, NULL) == VC_LOC_DISCARDED);
	CHECK(vc_locate(&r, &first_edge, NULL) == VC_LOC_UNKNOWN);
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* Branches cost what they are, however many there are. */
ENGINE_TEST(test_many_branches)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	char		name[32];
	ChunkStoreStats st;
	VcBranch	b;
	int			n = 20000;
	uint64		first_reads;

	setup(&r, cs);
	commit_rows(&r, 0, 100, 0, "base");
	st = *cs_stats(cs);
	CHECK_EQ(vc_branch_create(&r, "first", NULL), VC_OK);
	first_reads = STATS_DELTA(cs, st, gets);
	for (int i = 0; i < n; i++)
	{
		snprintf(name, sizeof(name), "br-%d", i);
		CHECK_EQ(vc_branch_create(&r, name, NULL), VC_OK);
	}
	/* (the last made as the first was: the same few chunks read, one written) */
	{
		ChunkStoreStats before = *cs_stats(cs);

		CHECK_EQ(vc_branch_create(&r, "last", NULL), VC_OK);
		CHECK_EQ(STATS_DELTA(cs, before, gets), first_reads);
		CHECK_EQ(STATS_DELTA(cs, before, puts), 1);
	}
	for (int i = 0; i < n; i += 3)
	{
		snprintf(name, sizeof(name), "br-%d", i);
		CHECK_EQ(vc_branch_delete(&r, name), VC_OK);
	}
	for (int i = 0; i < n; i++)
	{
		snprintf(name, sizeof(name), "br-%d", i);
		CHECK_EQ(vc_branch_get(&r, name, &b), i % 3 != 0);
		if (i % 3 != 0)
			vc_branch_free(&b);
	}
	CHECK(vc_branch_get(&r, "main", &b));
	vc_branch_free(&b);
	CHECK_EQ(vc_branch_delete(&r, "main"), VC_ERR);
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* ---------------------------------------------------- floors and ceilings */

typedef struct FakeClock
{
	uint64		us;
} FakeClock;

static uint64
fake_now(void *arg)
{
	return ((FakeClock *) arg)->us;
}

#define SEC UINT64CONST(1000000)

/*
 * Fake clocks start from the real one: a repository's first commit is made
 * on the server's clock, and no commit's time goes back past its parent's.
 */
static uint64
clock_base(void)
{
	return (uint64) (GetCurrentTimestamp() +
					 (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * USECS_PER_DAY) + 1000 * SEC;
}

/* A commit of k random row changes on the branch checked out, after dt seconds. */
static PHash
timed_commit(VcRepo *r, FakeClock *clk, uint64 dt, int k, uint64 ver)
{
	EditBatch	b;
	PHash		rv = vc_working_lock(r),
				out;

	clk->us += dt * SEC;
	batch_init(&b, (size_t) k);
	for (int i = 0; i < k; i++)
		batch_put(&b, rnd_below_global(KEYS), ver);
	CHECK_EQ(vc_table_edit(&r->ctx, &rv, "t", b.e, b.n, &out), VC_OK);
	vc_set_working(r, &out);
	batch_free(&b);
	return vc_commit(r, "timed");
}

static uint64
own_bytes(const VcCommit *c)
{
	uint64		pb = 0;

	for (uint32 i = 0; i < c->nparents; i++)
		if (c->parent_bytes[i] > pb)
			pb = c->parent_bytes[i];
	return c->bytes - pb;
}

/*
 * One head's window straight from its definition, added to out: every
 * ancestor of the head in the window now (or the head itself) that a floor
 * keeps and no ceiling cuts.
 */
static void
expected_head(VcRepo *r, const PHash *head, const VcWindow *w, uint64 now, PHashMap *out)
{
	VcCommit	H;
	PHashMap	anc;
	PHash	   *stack = palloc(sizeof(PHash) * 100000);
	size_t		n = 0;
	bool		created;

	CHECK(vc_commit_load(&r->ctx, head, &H));
	phmap_init(&anc);
	stack[n++] = *head;
	while (n > 0)
	{
		PHash		h = stack[--n];
		VcCommit	c;
		uint64		after,
					span;
		bool		floor,
					ceil;

		phmap_ref(&anc, &h, &created);
		/* (a sever only shrinks the window: what is past its edge stays out,
		 * readable or not) */
		if (!created || (!phash_eq(&h, head) && vc_locate(r, &h, NULL) != VC_LOC_WINDOW) ||
			!vc_commit_load(&r->ctx, &h, &c))
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
		for (uint32 j = 0; j < c.nparents; j++)
		{
			CHECK(n < 100000);
			stack[n++] = c.parents[j];
		}
		vc_commit_release(&c);
	}
	vc_commit_release(&H);
	phmap_free(&anc);
	pfree(stack);
}

typedef struct ExpectArg
{
	VcRepo	   *r;
	const VcWindow *w;
	uint64		now;
	PHashMap   *out;
} ExpectArg;

static void
expect_branch_fn(void *arg, const VcBranch *b)
{
	ExpectArg  *a = arg;

	expected_head(a->r, &b->head, a->w, a->now, a->out);
}

/* The window of every head, straight from its definition. */
static void
expected_window(VcRepo *r, const VcWindow *w, uint64 now, PHashMap *out)
{
	ExpectArg	a = {r, w, now, out};

	phmap_init(out);
	vc_branch_list(r, expect_branch_fn, &a);
}

typedef struct WinSet
{
	PHashMap	set;
} WinSet;

static void
window_set_fn(void *arg, const PHash *h, VcLocation where, uint32 seg)
{
	bool		created;

	if (where == VC_LOC_WINDOW)
		phmap_ref(&((WinSet *) arg)->set, h, &created);
}

/* The commits the window's history holds: those with references. */
static uint64
held_count(VcRepo *r)
{
	WinSet		got;
	uint64		n;

	phmap_init(&got.set);
	vc_locations(r, window_set_fn, &got);
	n = set_size(&got.set);
	phmap_free(&got.set);
	return n;
}

/* Severs with w and checks the window left is exactly the definition's. */
static void
sever_and_compare(VcRepo *r, const VcWindow *w, uint64 now)
{
	PHashMap	want;
	WinSet		got;
	uint32		seg;

	expected_window(r, w, now, &want);
	CHECK_EQ(vc_sever(r, w, &seg), VC_OK);
	if (seg != VC_NO_SEGMENT)
		CHECK_EQ(vc_segment_discard(r, seg, NULL), VC_OK);
	/* (what is discarded, freed, as its transaction's end would: gone) */
	release(r->ctx.cs);
	phmap_init(&got.set);
	vc_locations(r, window_set_fn, &got);
	CHECK(set_equal(&want, &got.set));
	phmap_free(&want);
	phmap_free(&got.set);
}

/* A commit's change bytes are exactly the chunks its root adds over its first parent's. */
ENGINE_TEST(test_change_bytes)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	FakeClock	clk = {clock_base()};
	VcCommit	c,
				p;
	PHash		h;

	setup(&r, cs);
	vc_set_clock(&r, fake_now, &clk);
	rng_seed_global(11);
	commit_rows(&r, 0, 2000, 0, "load");
	for (int i = 1; i <= 20; i++)
	{
		PHashMap	mine,
					theirs,
					only;
		uint64		sum = 0;

		h = timed_commit(&r, &clk, 1, 1 + (int) rnd_below_global(60), (uint64) i);
		CHECK(vc_commit_load(&r.ctx, &h, &c) && vc_commit_load(&r.ctx, &c.parents[0], &p));
		phmap_init(&mine);
		phmap_init(&theirs);
		closure(cs, &c.root, &mine);
		closure(cs, &p.root, &theirs);
		set_minus(&mine, &theirs, &only);
		for (size_t j = 0; j < only.cap; j++)
			if (only.used[j])
			{
				uint32		len;
				void	   *d = cs_get(cs, only.keys[j].b, &len);

				CHECK(d != NULL);
				sum += len;
				pfree(d);
			}
		CHECK_EQ(own_bytes(&c), sum);
		CHECK_EQ(c.bytes, p.bytes + sum);
		CHECK(c.time > p.time);
		vc_commit_release(&c);
		vc_commit_release(&p);
		phmap_free(&mine);
		phmap_free(&theirs);
		phmap_free(&only);
	}
	/* a commit written with a time before its parent's is placed after it */
	h = head_of(&r);
	CHECK(vc_commit_load(&r.ctx, &h, &p));
	h = vc_commit_write(&r.ctx, &p.root, &p.hash, 1, p.time - 100 * SEC, "early");
	CHECK(vc_commit_load(&r.ctx, &h, &c));
	CHECK_EQ(c.time, p.time + 1);
	vc_commit_release(&c);
	vc_commit_release(&p);
	/* a clock that goes back still gives a later time than the parent's */
	clk.us -= 500 * SEC;
	h = timed_commit(&r, &clk, 0, 1, 99);
	CHECK(vc_commit_load(&r.ctx, &h, &c) && vc_commit_load(&r.ctx, &c.parents[0], &p));
	CHECK(c.time > p.time);
	vc_commit_release(&c);
	vc_commit_release(&p);
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* Each limit alone, as floor and as ceiling, and a ceiling beating a floor. */
ENGINE_TEST(test_window_limits)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	FakeClock	clk = {clock_base()};
	PHash		h[41];
	VcCommit	c;
	uint64		sz[41];

	setup(&r, cs);
	vc_set_clock(&r, fake_now, &clk);
	rng_seed_global(12);
	commit_rows(&r, 0, 4000, 0, "load");
	for (int i = 1; i <= 40; i++)
	{
		/* one commit a minute, of varying size */
		h[i] = timed_commit(&r, &clk, 60, 1 + (int) rnd_below_global(40), (uint64) i);
		CHECK(vc_commit_load(&r.ctx, &h[i], &c));
		sz[i] = own_bytes(&c);
		vc_commit_release(&c);
	}

	/* commits: a floor of 8 under a ceiling of 5 keeps 5 */
	sever_and_compare(&r, &(VcWindow) {.depth = 8, .max_commits = 5}, clk.us);
	CHECK_EQ(held_count(&r), 5);
	/* ceilings only shrink: 8 under 20 is still the 5 left */
	sever_and_compare(&r, &(VcWindow) {.depth = 8, .max_commits = 20}, clk.us);
	CHECK_EQ(held_count(&r), 5);
	vc_repo_free(&r);
	cs_destroy(cs);

	/* the rest on fresh copies of the same history */
	for (int variant = 0; variant < 6; variant++)
	{
		uint64		after = 0,
					span = 0;
		uint64		keep = 0;

		cs = memstore_create();
		clk.us = clock_base();
		setup(&r, cs);
		vc_set_clock(&r, fake_now, &clk);
		rng_seed_global(12);
		commit_rows(&r, 0, 4000, 0, "load");
		for (int i = 1; i <= 40; i++)
			timed_commit(&r, &clk, 60, 1 + (int) rnd_below_global(40), (uint64) i);
		switch (variant)
		{
			case 5:
				/* exactly the bytes after commit 38: it is no longer needed */
				sever_and_compare(&r, &(VcWindow) {.keep_bytes = sz[40] + sz[39]}, clk.us);
				CHECK_EQ(held_count(&r), 2);
				break;
			case 0:
				/* bytes floor: keep until the commits after reach 10 sizes' worth */
				for (int i = 40; i >= 1 && after < 10 * sz[40]; i--, keep++)
					after += sz[i];
				sever_and_compare(&r, &(VcWindow) {.depth = 1, .keep_bytes = 10 * sz[40]}, clk.us);
				CHECK_EQ(held_count(&r), keep);
				break;
			case 1:
				/* bytes ceiling: at most what the last 12 commits changed */
				for (int i = 40; i > 28; i--)
					span += sz[i];
				sever_and_compare(&r, &(VcWindow) {.depth = 30, .max_bytes = span}, clk.us);
				CHECK_EQ(held_count(&r), 12);
				break;
			case 2:
				/* time floor: ten minutes keeps the commits of the last ten minutes */
				sever_and_compare(&r, &(VcWindow) {.depth = 1, .keep_seconds = 600}, clk.us);
				CHECK_EQ(held_count(&r), 10);
				break;
			case 3:
				/* time ceiling beats a commit floor */
				sever_and_compare(&r, &(VcWindow) {.depth = 30, .max_seconds = 300}, clk.us);
				CHECK_EQ(held_count(&r), 5);
				break;
			case 4:
				/* the head stays however old: an hour later, a one-minute ceiling */
				clk.us += 3600 * SEC;
				sever_and_compare(&r, &(VcWindow) {.depth = 30, .max_seconds = 60}, clk.us);
				CHECK_EQ(held_count(&r), 1);
				break;
		}
		settle_and_check(cs);
		vc_repo_free(&r);
		cs_destroy(cs);
	}
}

/*
 * Random branching, merging, clocks and policies: after every sever the
 * window is exactly what its definition says.
 */
ENGINE_TEST(test_window_policy_random)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	FakeClock	clk = {clock_base()};
	const char *names[] = {"main", "b1", "b2", "b3"};
	PHash		h;

	setup(&r, cs);
	vc_set_clock(&r, fake_now, &clk);
	rng_seed_global(13);
	commit_rows(&r, 0, 4000, 0, "load");
	h = head_of(&r);
	for (int b = 1; b < 4; b++)
		CHECK_EQ(vc_branch_create(&r, names[b], &h), VC_OK);
	for (int step = 1; step <= 300; step++)
	{
		int			b = (int) rnd_below_global(4);

		CHECK_EQ(vc_checkout(&r, names[b]), VC_OK);
		if (rnd_below_global(8) == 0)
		{
			VcMergeResult m;
			int			o = (int) rnd_below_global(4);

			if (o != b && vc_merge(&r, names[o], &m) == VC_OK)
			{
				if (m.status == VC_MERGE_CONFLICTS)
				{
					h = head_of(&r);
					CHECK_EQ(vc_reset_hard(&r, &h), VC_OK);
				}
				vc_merge_result_free(&m);
			}
		}
		else
			timed_commit(&r, &clk, rnd_below_global(120), 1 + (int) rnd_below_global(30),
						 (uint64) step);
		if (step % 10 == 0)
		{
			VcWindow	w = {.depth = (uint32) rnd_below_global(12)};
			uint64		unit = 3000;

			if (rnd_below_global(2))
				w.keep_bytes = unit * (1 + rnd_below_global(20));
			if (rnd_below_global(2))
				w.keep_seconds = 60 * (1 + rnd_below_global(20));
			if (rnd_below_global(3) == 0)
				w.max_commits = 2 + rnd_below_global(15);
			if (rnd_below_global(3) == 0)
				w.max_bytes = unit * (2 + rnd_below_global(30));
			if (rnd_below_global(3) == 0)
				w.max_seconds = 60 * (2 + rnd_below_global(30));
			if (!w.depth && !w.keep_bytes && !w.keep_seconds && !w.max_commits &&
				!w.max_bytes && !w.max_seconds)
				w.depth = 1;
			CHECK_EQ(vc_checkout(&r, "main"), VC_OK);
			sever_and_compare(&r, &w, clk.us);
		}
	}
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* ------------------------------------------------------------- low marks */

static uint64
tighter(uint64 limit, uint64 mark)
{
	if (mark == 0)
		return limit;
	if (limit == 0)
		return mark;
	return mark < limit ? mark : limit;
}

typedef struct MarkedArg
{
	VcRepo	   *r;
	const VcWindow *high;
	const VcWindow *low;
	uint64		now;
	PHashMap   *held;
	PHashMap   *out;
} MarkedArg;

static void
marked_branch_fn(void *arg, const VcBranch *b)
{
	MarkedArg  *a = arg;
	PHashMap	hs;
	bool		over = false;

	phmap_init(&hs);
	expected_head(a->r, &b->head, a->high, a->now, &hs);
	for (size_t j = 0; j < hs.cap && !over; j++)
	{
		VcCommit	c;

		if (!hs.used[j] || !vc_commit_load(&a->r->ctx, &hs.keys[j], &c))
			continue;
		for (uint32 k = 0; k < c.nparents; k++)
			if (!phmap_find(&hs, &c.parents[k]) && phmap_find(a->held, &c.parents[k]))
				over = true;
		vc_commit_release(&c);
	}
	phmap_free(&hs);
	expected_head(a->r, &b->head, over ? a->low : a->high, a->now, a->out);
}

/*
 * The marked window straight from its definition: a head whose high-mark
 * window would let go of a held commit (a parent of the window's commits,
 * outside it, still held) is cut back to its low-mark window; any other
 * head keeps its high-mark window.
 */
static void
expected_marked(VcRepo *r, const VcWindow *w, uint64 now, PHashMap *held,
				PHashMap *out)
{
	VcWindow	high = *w,
				low = *w;
	MarkedArg	a = {r, &high, &low, now, held, out};

	high.low_commits = high.low_bytes = high.low_seconds = 0;
	low.max_commits = tighter(w->max_commits, w->low_commits);
	low.max_bytes = tighter(w->max_bytes, w->low_bytes);
	low.max_seconds = tighter(w->max_seconds, w->low_seconds);
	low.low_commits = low.low_bytes = low.low_seconds = 0;
	phmap_init(out);
	vc_branch_list(r, marked_branch_fn, &a);
}

/* Severs with a marked window and checks the window left is the definition's. */
static void
sever_marked_and_compare(VcRepo *r, const VcWindow *w, uint64 now)
{
	PHashMap	want;
	WinSet		held,
				got;
	uint32		seg;

	phmap_init(&held.set);
	vc_locations(r, window_set_fn, &held);
	expected_marked(r, w, now, &held.set, &want);
	CHECK_EQ(vc_sever(r, w, &seg), VC_OK);
	if (seg != VC_NO_SEGMENT)
		CHECK_EQ(vc_segment_discard(r, seg, NULL), VC_OK);
	release(r->ctx.cs);
	phmap_init(&got.set);
	vc_locations(r, window_set_fn, &got);
	CHECK(set_equal(&want, &got.set));
	phmap_free(&want);
	phmap_free(&held.set);
	phmap_free(&got.set);
}

static void
sever_discard(VcRepo *r, const VcWindow *w)
{
	uint32		seg;

	CHECK_EQ(vc_sever(r, w, &seg), VC_OK);
	if (seg != VC_NO_SEGMENT)
		CHECK_EQ(vc_segment_discard(r, seg, NULL), VC_OK);
	release(r->ctx.cs);
}

/*
 * A line of commits severed after each one: the window holds h commits,
 * and the next makes h + 1, or, past the high mark of N, the low mark L.
 */
static void
marks_line(uint64 N, uint64 low, uint64 want_low)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	FakeClock	clk = {clock_base()};
	VcWindow	w = {.max_commits = N, .low_commits = low};
	uint64		h;
	int			resets = 0;

	setup(&r, cs);
	vc_set_clock(&r, fake_now, &clk);
	rng_seed_global(21);
	commit_rows(&r, 0, 2000, 0, "load");
	sever_discard(&r, &w);
	h = held_count(&r);			/* the initial commit and the load, if under N */
	CHECK_EQ(h, N >= 2 ? 2 : 1);
	for (int i = 1; i <= (int) (4 * N + 3); i++)
	{
		timed_commit(&r, &clk, 1, 1 + (int) rnd_below_global(20), (uint64) i);
		sever_marked_and_compare(&r, &w, clk.us);
		h = h + 1 <= N ? h + 1 : want_low;
		if (h == want_low && want_low < N)
			resets++;
		CHECK_EQ(held_count(&r), h);
		CHECK_LE(held_count(&r), N);
	}
	if (want_low < N)
		CHECK(resets >= 3);
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* low_commits = 1: the window grows to N commits, then holds only the head. */
ENGINE_TEST(test_marks_tumbling)
{
	marks_line(8, 1, 1);
	marks_line(1, 1, 1);		/* a high mark of one: only ever the head */
}

/* A low mark between: it grows to N, then back to the last L. */
ENGINE_TEST(test_marks_hysteresis)
{
	marks_line(10, 4, 4);
	marks_line(10, 9, 9);
}

/* A low mark at or past the high mark slides, as with no low mark. */
ENGINE_TEST(test_marks_at_high_slide)
{
	marks_line(10, 10, 10);
	marks_line(10, 25, 10);
}

/* Low marks alone are refused: they need a high mark to fill to. */
ENGINE_TEST(test_marks_need_high)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	uint32		seg;

	setup(&r, cs);
	commit_rows(&r, 0, 100, 0, "load");
	CHECK_EQ(vc_sever(&r, &(VcWindow) {.low_commits = 1}, &seg), VC_ERR);
	CHECK_EQ(vc_sever(&r, &(VcWindow) {.low_bytes = 100, .low_seconds = 5}, &seg), VC_ERR);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/*
 * A floor is the high mark too: a depth floor of 30 with no ceiling fills
 * to 30 commits, then is cut to the low mark, which wins over the floor.
 */
ENGINE_TEST(test_marks_floor_is_high)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	FakeClock	clk = {clock_base()};
	VcWindow	w = {.depth = 30, .low_commits = 5};
	bool		cut = false;

	setup(&r, cs);
	vc_set_clock(&r, fake_now, &clk);
	rng_seed_global(22);
	commit_rows(&r, 0, 2000, 0, "load");
	for (int i = 1; i <= 70; i++)
	{
		timed_commit(&r, &clk, 1, 3, (uint64) i);
		sever_marked_and_compare(&r, &w, clk.us);
		CHECK_LE(held_count(&r), 30);
		cut |= held_count(&r) == 5;
	}
	CHECK(cut);
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/*
 * Each branch fills and empties on its own: a busy branch is cut back to
 * its low mark while a quiet one keeps its whole window, shared history
 * included.
 */
ENGINE_TEST(test_marks_branches_independent)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	FakeClock	clk = {clock_base()};
	VcWindow	w = {.max_commits = 12, .low_commits = 2};
	PHash		fork,
				head;
	PHashMap	held;
	WinSet		got;

	setup(&r, cs);
	vc_set_clock(&r, fake_now, &clk);
	rng_seed_global(23);
	commit_rows(&r, 0, 2000, 0, "load");
	for (int i = 1; i <= 5; i++)
		timed_commit(&r, &clk, 1, 2, (uint64) i);
	fork = head_of(&r);
	CHECK_EQ(vc_branch_create(&r, "busy", &fork), VC_OK);
	CHECK_EQ(vc_checkout(&r, "busy"), VC_OK);
	for (int i = 6; i <= 30; i++)
		timed_commit(&r, &clk, 1, 2, (uint64) i);
	CHECK_EQ(vc_checkout(&r, "main"), VC_OK);
	timed_commit(&r, &clk, 1, 2, 31);
	sever_marked_and_compare(&r, &w, clk.us);
	/* main (7 commits deep at most) is short of 12: all of its window stays */
	phmap_init(&held);
	head = head_of(&r);
	expected_head(&r, &head, &(VcWindow) {.max_commits = 12}, clk.us, &held);
	phmap_init(&got.set);
	vc_locations(&r, window_set_fn, &got);
	for (size_t i = 0; i < held.cap; i++)
		if (held.used[i])
			CHECK(phmap_find(&got.set, &held.keys[i]) != NULL);
	CHECK(phmap_find(&got.set, &fork) != NULL);
	/* busy is cut to its last 2, so beside main's 7 the window holds 9 */
	CHECK_EQ(held_count(&r), set_size(&held) + 2);
	phmap_free(&held);
	phmap_free(&got.set);
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* A pinned commit stays through a cut to the low mark. */
ENGINE_TEST(test_marks_pinned)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	FakeClock	clk = {clock_base()};
	PHash		pin;
	VcWindow	w = {.max_commits = 6, .low_commits = 1};
	uint32		seg;

	setup(&r, cs);
	vc_set_clock(&r, fake_now, &clk);
	rng_seed_global(24);
	commit_rows(&r, 0, 2000, 0, "load");
	pin = timed_commit(&r, &clk, 1, 2, 1);
	w.pinned = &pin;
	w.npinned = 1;
	for (int i = 2; i <= 20; i++)
	{
		timed_commit(&r, &clk, 1, 2, (uint64) i);
		sever_discard(&r, &w);
		CHECK(vc_locate(&r, &pin, &seg) == VC_LOC_WINDOW);
		CHECK_LE(held_count(&r), 7);
	}
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/*
 * Random histories, branches and merges under random marked windows: every
 * sever leaves exactly the definition's window.
 */
ENGINE_TEST(test_marks_random)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	FakeClock	clk = {clock_base()};
	const char *names[] = {"main", "b1", "b2", "b3"};
	PHash		h;

	setup(&r, cs);
	vc_set_clock(&r, fake_now, &clk);
	rng_seed_global(25);
	commit_rows(&r, 0, 4000, 0, "load");
	h = head_of(&r);
	for (int b = 1; b < 4; b++)
		CHECK_EQ(vc_branch_create(&r, names[b], &h), VC_OK);
	for (int round = 0; round < 6; round++)
	{
		VcWindow	w = {0};
		uint64		unit = 3000;

		/* one window per round, so that branches fill and are cut under it */
		if (rnd_below_global(2))
			w.depth = (uint32) (4 + rnd_below_global(20));
		if (rnd_below_global(3) == 0)
			w.keep_bytes = unit * (4 + rnd_below_global(20));
		if (rnd_below_global(3) == 0)
			w.keep_seconds = 60 * (4 + rnd_below_global(20));
		if (rnd_below_global(2))
			w.max_commits = 4 + rnd_below_global(20);
		if (rnd_below_global(3) == 0)
			w.max_bytes = unit * (6 + rnd_below_global(30));
		if (rnd_below_global(3) == 0)
			w.max_seconds = 60 * (6 + rnd_below_global(30));
		if (!w.depth && !w.keep_bytes && !w.keep_seconds && !w.max_commits &&
			!w.max_bytes && !w.max_seconds)
			w.max_commits = 10;
		if (rnd_below_global(2))
			w.low_commits = 1 + rnd_below_global(6);
		if (rnd_below_global(3) == 0)
			w.low_bytes = unit * (1 + rnd_below_global(6));
		if (rnd_below_global(3) == 0)
			w.low_seconds = 60 * (1 + rnd_below_global(6));
		if (!w.low_commits && !w.low_bytes && !w.low_seconds)
			w.low_commits = 1;
		for (int step = 1; step <= 80; step++)
		{
			int			b = (int) rnd_below_global(4);

			CHECK_EQ(vc_checkout(&r, names[b]), VC_OK);
			if (rnd_below_global(8) == 0)
			{
				VcMergeResult m;
				int			o = (int) rnd_below_global(4);

				if (o != b && vc_merge(&r, names[o], &m) == VC_OK)
				{
					if (m.status == VC_MERGE_CONFLICTS)
					{
						h = head_of(&r);
						CHECK_EQ(vc_reset_hard(&r, &h), VC_OK);
					}
					vc_merge_result_free(&m);
				}
			}
			else
				timed_commit(&r, &clk, rnd_below_global(120), 1 + (int) rnd_below_global(30),
							 (uint64) (round * 100 + step));
			if (step % 3 == 0)
			{
				CHECK_EQ(vc_checkout(&r, "main"), VC_OK);
				sever_marked_and_compare(&r, &w, clk.us);
			}
		}
	}
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/*
 * A parent two kept commits name stays known as discarded while either
 * does: X and Y share P; Y is cut and discarded later, and X still names P.
 */
ENGINE_TEST(test_discarded_parent_still_named)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	PHash		p,
				x,
				y;
	uint32		seg;
	VcWindow	depth1 = {.depth = 1};

	setup(&r, cs);
	commit_rows(&r, 0, 100, 0, "base");
	p = commit_rows(&r, 0, 10, 1, "P");
	CHECK_EQ(vc_branch_create(&r, "x", NULL), VC_OK);
	CHECK_EQ(vc_checkout(&r, "x"), VC_OK);
	x = commit_rows(&r, 20, 30, 2, "X");
	CHECK_EQ(vc_checkout(&r, "main"), VC_OK);
	y = commit_rows(&r, 40, 50, 3, "Y");
	sever_discard(&r, &depth1);
	CHECK(vc_locate(&r, &p, NULL) == VC_LOC_DISCARDED);
	CHECK(vc_locate(&r, &x, NULL) == VC_LOC_WINDOW);

	/* Y cut and discarded: X still names P */
	commit_rows(&r, 60, 70, 4, "Z");
	CHECK_EQ(vc_sever(&r, &depth1, &seg), VC_OK);
	CHECK_EQ(segment_size(&r, seg), 1);
	CHECK(vc_locate(&r, &y, NULL) == VC_LOC_SEGMENT);
	CHECK_EQ(vc_segment_discard(&r, seg, NULL), VC_OK);
	release(cs);
	CHECK(vc_locate(&r, &y, NULL) == VC_LOC_DISCARDED);	/* Z names it */
	CHECK(vc_locate(&r, &p, NULL) == VC_LOC_DISCARDED);	/* X names it */

	/* X's branch gone and severed: nothing names P, forgotten */
	CHECK_EQ(vc_branch_delete(&r, "x"), VC_OK);
	sever_discard(&r, &depth1);
	CHECK(vc_locate(&r, &x, NULL) == VC_LOC_UNKNOWN);
	CHECK(vc_locate(&r, &p, NULL) == VC_LOC_UNKNOWN);
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/*
 * A sever cuts at most max_cut commits; the rest wait for the next, which
 * cuts them.
 */
ENGINE_TEST(test_sever_bounded)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	Linear		l;
	uint32		s1,
				s2,
				s3;
	VcWindow	w = {.depth = 2, .max_cut = 2};

	setup(&r, cs);
	l = build_linear(&r);
	CHECK_EQ(vc_sever(&r, &w, &s1), VC_OK);
	CHECK_EQ(segment_size(&r, s1), 2);
	CHECK_EQ(vc_sever(&r, &w, &s2), VC_OK);
	CHECK(s2 == s1 + 1 && segment_size(&r, s2) == 1);
	CHECK_EQ(vc_sever(&r, &w, &s3), VC_OK);
	CHECK_EQ(s3, VC_NO_SEGMENT);
	CHECK(vc_locate(&r, &l.c, NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&r, &l.b, NULL) == VC_LOC_SEGMENT);
	CHECK(vc_locate(&r, &l.a, NULL) == VC_LOC_SEGMENT);
	CHECK(vc_locate(&r, &l.init, NULL) == VC_LOC_SEGMENT);
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/*
 * An archive must be another store, as transactional as the repository's,
 * and an exported segment comes back only from the one it went to.
 */
ENGINE_TEST(test_archive_checked)
{
	ChunkStore *cs = memstore_create();
	ChunkStore *archive = archive_store_create();
	VcSource	arch = archive_of(archive);
	VcSource	self = {"self", cs};
	VcSource	other = {"other", archive};
	VcSource	pages = {"pages", ChunkStoreOpen("pages")};
	VcRepo		r;
	Linear		l;
	uint32		seg;

	setup(&r, cs);
	l = build_linear(&r);
	CHECK_EQ(vc_sever(&r, &depth2, &seg), VC_OK);
	CHECK_EQ(vc_segment_export(&r, seg, &self), VC_ERR);
	CHECK_EQ(vc_segment_export(&r, seg, &pages), VC_ERR);	/* transactional */
	CHECK_EQ(vc_segment_export(&r, seg, &arch), VC_OK);
	CHECK_EQ(vc_segment_attach(&r, seg, &other), VC_ERR);	/* not where it went */
	CHECK_EQ(vc_segment_attach(&r, seg, NULL), VC_ERR);
	CHECK_EQ(vc_segment_discard(&r, seg, &other), VC_ERR);
	CHECK(vc_locate(&r, &l.b, NULL) == VC_LOC_EXPORTED);
	CHECK_EQ(vc_segment_attach(&r, seg, &arch), VC_OK);
	CHECK(vc_locate(&r, &l.b, NULL) == VC_LOC_WINDOW);
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

static const ProllyUnitTest tests[] = {
	{"detach_moves_holds_only", test_detach_moves_holds_only},
	{"export_self_contained", test_export_self_contained},
	{"second_export_copies_diff", test_second_export_copies_diff},
	{"discard_frees_exactly", test_discard_frees_exactly},
	{"branches_cut_at_shared_ancestor", test_branches_cut_at_shared_ancestor},
	{"stale_branch_head_stays", test_stale_branch_head_stays},
	{"merge_commit_parents_across_edge", test_merge_commit_parents_across_edge},
	{"overlapping_branch_windows", test_overlapping_branch_windows},
	{"wider_window_stays_cut", test_wider_window_stays_cut},
	{"pinned_commit_stays", test_pinned_commit_stays},
	{"refuse_outside_window", test_refuse_outside_window},
	{"window_scaling", test_window_scaling},
	{"state_does_not_grow_with_history", test_state_does_not_grow_with_history},
	{"many_branches", test_many_branches},
	{"change_bytes", test_change_bytes},
	{"window_limits", test_window_limits},
	{"window_policy_random", test_window_policy_random},
	{"marks_tumbling", test_marks_tumbling},
	{"marks_hysteresis", test_marks_hysteresis},
	{"marks_at_high_slide", test_marks_at_high_slide},
	{"marks_need_high", test_marks_need_high},
	{"marks_floor_is_high", test_marks_floor_is_high},
	{"marks_branches_independent", test_marks_branches_independent},
	{"marks_pinned", test_marks_pinned},
	{"marks_random", test_marks_random},
	{"discarded_parent_still_named", test_discarded_parent_still_named},
	{"sever_bounded", test_sever_bounded},
	{"archive_checked", test_archive_checked},
};

const ProllyUnitSuite unit_engine_window_suite = {"engine_window", tests, lengthof(tests)};
