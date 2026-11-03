/*-------------------------------------------------------------------------
 *
 * unit_engine_remote.c
 *	  Unit tests of remotes with windows on both sides: no implicit return
 *	  of cut history, shallow clones with hints, connected fetches, the
 *	  receiver's fast-forward check, deepening from remotes and archives
 *	  into segments, restoring from an archive alone, pull deepening to
 *	  find a merge base, branching on both sides, histories that do not
 *	  attach, and what each transfer costs.
 *
 * Each repository has a store of its own: the first the store a test runs
 * on (the memory store, then the history store), the rest memory stores of
 * the test's own, with the memory references.  The tests end a memory
 * store's holds, as a transaction's end would, to see what a call left
 * behind; on the history store they last until the test's subtransaction
 * is rolled back.  An archive is as transactional as the repository that
 * exports to it: the history store's tier for one on the history store.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/test/modules/test_prolly/unit_engine_remote.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "engine_test_util.h"

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

static bool
head_is(VcRepo *r, const char *name, const PHash *want)
{
	PHash		h = branch_head(r, name);

	return phash_eq(&h, want);
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

/* A repository named name on cs, with table t made (uncommitted). */
static void
setup(VcRepo *r, ChunkStore *cs, const char *name)
{
	PHash		rv,
				out;

	vc_repo_open(r, cs, t_refs(cs), &small_policy);
	if (name)
		vc_repo_set_name(r, name);
	rv = vc_working_lock(r);
	CHECK_EQ(vc_table_create(&r->ctx, &rv, "t", ddl, strlen(ddl), &out), VC_OK);
	vc_set_working(r, &out);
}

static int
clone(VcRepo *dst, ChunkStore *cs, VcRepo *src, uint32 depth)
{
	return vc_repo_clone(dst, cs, t_refs(cs), &small_policy, src, depth);
}

/* The version of row key at a root, -1 if it has none. */
static int64
row_at(VcRepo *r, const PHash *rv, uint64 key)
{
	VcTable		t;
	const uint8 *v;
	size_t		vl;
	uint8		k[8];

	CHECK(vc_table_get(&r->ctx, rv, "t", &t));
	put_u64(k, key);
	if (!t_get(&r->ctx, &t.data, k, 8, &v, &vl))
		return -1;
	return (int64) get_u64(v + 8);
}

#define KEYS 40000

/* Rows of table t at a commit, read from ctx's store alone. */
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
count_window_fn(void *arg, const PHash *h, VcLocation where, uint32 seg)
{
	if (where == VC_LOC_WINDOW)
		(*(int *) arg)++;
}

static int
count_window(VcRepo *r)
{
	int			n = 0;

	vc_locations(r, count_window_fn, &n);
	return n;
}

static void
boundary_fn(void *arg, const PHash *h, VcLocation where, uint32 seg)
{
	if (where == VC_LOC_BOUNDARY)
		*(PHash *) arg = *h;
}

static void
count_bounds_fn(void *arg, const PHash *h, VcLocation where, uint32 seg)
{
	if (where == VC_LOC_BOUNDARY)
		(*(int *) arg)++;
}

static bool
hint_is(VcRepo *r, const PHash *h, const char *want)
{
	char	   *got = vc_hint(r, h);
	bool		same = got != NULL && strcmp(got, want) == 0;

	if (got)
		pfree(got);
	return same;
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

/* The chunks letting go of segment seg must free: its closure, less all else held. */
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

/* What the store frees as its holds end (as the transaction's end would). */
static uint64
release(ChunkStore *cs)
{
	if (cs->routine != &MemoryChunkStore)
		return 0;				/* (held until the test's subtransaction ends) */
	MemoryChunkStoreEndHolds(cs);
	return cs_free_unheld(cs);
}

/* The chunks the store has, its holds ended and what nothing holds freed. */
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

/*
 * Whether what the store's holds no longer cover is seen freed: on a memory
 * store, whose holds a test ends; not on the history store, where they last
 * until the test's subtransaction is rolled back
 */
static bool
frees_seen(ChunkStore *cs)
{
	return cs->routine == &MemoryChunkStore;
}

#define STORED(cs) (cs_stats(cs)->stored)

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

/* Main: init <- A <- n commits more, all of them in c[0..n+1]. */
static PHash *
build_main(VcRepo *r, int n)
{
	PHash	   *c = palloc_array(PHash, n + 2);

	c[0] = head_of(r);
	c[1] = commit_rows(r, 0, 20000, 0, "A");
	for (int i = 2; i < n + 2; i++)
		c[i] = commit_rows(r, (uint64) i * 700, (uint64) i * 700 + 100, (uint64) i,
						   "main");
	return c;
}

/* ------------------------------------------------------------------ tests */

/* A laptop with full history pushing to a remote that discarded A, B sends none of it. */
ENGINE_TEST(test_push_does_not_resurrect)
{
	ChunkStore *ocs = memstore_create(),
			   *lcs = memstore_create_own();
	VcRepo		origin,
				laptop;
	Linear		l;
	uint32		seg;
	uint64		stored;
	PHash		e;
	LogAcc		acc;

	setup(&origin, ocs, "origin");
	l = build_linear(&origin);
	CHECK_EQ(clone(&laptop, lcs, &origin, 0), VC_OK);
	vc_repo_set_name(&laptop, "laptop");

	CHECK_EQ(vc_sever(&origin, &depth2, &seg), VC_OK);
	CHECK_EQ(vc_segment_discard(&origin, seg, NULL), VC_OK);
	(void) release(ocs);
	if (frees_seen(ocs))
		CHECK(!has(ocs, &l.a));

	e = commit_rows(&laptop, 500, 600, 9, "E");
	stored = STORED(ocs);
	CHECK_EQ(vc_push(&laptop, &origin, "main"), VC_OK);
	CHECK(head_is(&origin, "main", &e));
	if (frees_seen(ocs))
		CHECK(!has(ocs, &l.a) && !has(ocs, &l.b));
	CHECK(vc_locate(&origin, &l.b, NULL) == VC_LOC_DISCARDED);
	CHECK_LE(STORED(ocs) - stored, 3 * 100 * 6 + 10);
	/* (gone from the store once the holds end; past the window's edge in any case) */
	if (frees_seen(ocs))
		CHECK_EQ(log_from(&origin, &e, 0, &acc), VC_ERR_SEVERED);
	else
		CHECK_EQ(log_from(&origin, &e, VC_WINDOW_ONLY, &acc), VC_ERR_OUTSIDE_WINDOW);
	CHECK_EQ(acc.n, 3);
	settle_and_check(ocs);
	settle_and_check(lcs);
	vc_repo_free(&origin);
	vc_repo_free(&laptop);
	cs_destroy(ocs);
}

/* A clone at depth 3 takes three commits a branch, with hints for the rest. */
ENGINE_TEST(test_shallow_clone_with_hints)
{
	ChunkStore *ocs = memstore_create(),
			   *scs = memstore_create_own(),
			   *fcs = memstore_create_own();
	VcRepo		origin,
				shallow,
				full;
	PHash	   *c,
				f2;
	LogAcc		acc;
	int64	   *head;

	setup(&origin, ocs, "origin");
	c = build_main(&origin, 12);
	CHECK_EQ(vc_branch_create(&origin, "feature", &c[6]), VC_OK);
	CHECK_EQ(vc_checkout(&origin, "feature"), VC_OK);
	commit_rows(&origin, 30000, 30100, 50, "f1");
	f2 = commit_rows(&origin, 30100, 30200, 51, "f2");
	CHECK_EQ(vc_checkout(&origin, "main"), VC_OK);
	head = snap(&origin.ctx, &c[13]);

	CHECK_EQ(clone(&shallow, scs, &origin, 3), VC_OK);
	CHECK_EQ(clone(&full, fcs, &origin, 0), VC_OK);
	CHECK_EQ(count_window(&shallow), 6);
	CHECK(vc_locate(&shallow, &c[13], NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&shallow, &c[11], NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&shallow, &c[10], NULL) == VC_LOC_BOUNDARY);
	CHECK(hint_is(&shallow, &c[10], "origin"));
	CHECK(vc_locate(&shallow, &f2, NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&shallow, &c[6], NULL) == VC_LOC_WINDOW);	/* feature's base */
	CHECK(vc_locate(&shallow, &c[5], NULL) == VC_LOC_BOUNDARY);
	CHECK(vc_locate(&shallow, &c[1], NULL) == VC_LOC_UNKNOWN);	/* never named */
	CHECK_LT(settled_count(scs), settled_count(fcs));
	check_snap(&shallow.ctx, &c[13], head);
	CHECK_EQ(log_from(&shallow, &c[13], 0, &acc), VC_ERR_SEVERED);
	CHECK_EQ(acc.n, 3);
	CHECK(phash_eq(&shallow.ctx.err_hash, &c[10]));
	/* the clone's branches are the origin's */
	CHECK(head_is(&shallow, "feature", &f2));
	CHECK(head_is(&full, "main", &c[13]));
	settle_and_check(scs);
	settle_and_check(fcs);
	vc_repo_free(&origin);
	vc_repo_free(&shallow);
	vc_repo_free(&full);
	cs_destroy(ocs);
}

/* A clone goes only into a store with no repository. */
ENGINE_TEST(test_clone_into_empty_only)
{
	ChunkStore *ocs = memstore_create(),
			   *lcs = memstore_create_own();
	VcRepo		origin,
				laptop,
				again;

	PHash		state,
				head,
				h;

	setup(&origin, ocs, "origin");
	commit_rows(&origin, 0, 100, 1, "A");
	setup(&laptop, lcs, "laptop");
	head = commit_rows(&laptop, 0, 100, 2, "L");
	CHECK(vc_refs_get(laptop.refs, "vc/repo", VC_REF_READ, &state));
	CHECK_EQ(clone(&again, lcs, &origin, 0), VC_ERR);
	/* the repository there is as it was: its state, its branch, its name */
	CHECK(vc_refs_get(laptop.refs, "vc/repo", VC_REF_READ, &h));
	CHECK(phash_eq(&h, &state));
	CHECK(head_is(&laptop, "main", &head));
	CHECK(hint_is(&laptop, &head, "origin") == false);
	settle_and_check(lcs);
	vc_repo_free(&again);
	vc_repo_free(&origin);
	vc_repo_free(&laptop);
	cs_destroy(ocs);
}

/* Fetch connects: everything down to known commits, however small the window. */
ENGINE_TEST(test_fetch_connects_past_depth)
{
	ChunkStore *ocs = memstore_create(),
			   *lcs = memstore_create_own();
	VcRepo		origin,
				laptop;
	PHash		last = {{0}};
	VcMergeResult m;
	int			before;

	setup(&origin, ocs, "origin");
	(void) build_main(&origin, 4);
	CHECK_EQ(clone(&laptop, lcs, &origin, 2), VC_OK);
	before = count_window(&laptop);
	for (int i = 0; i < 6; i++)
		last = commit_rows(&origin, 100 + (uint64) i, 101 + (uint64) i, 70, "new");
	CHECK_EQ(vc_pull(&laptop, &origin, "main", 0, &m), VC_OK);
	CHECK(m.status == VC_MERGE_FAST_FORWARD && phash_eq(&m.commit, &last));
	vc_merge_result_free(&m);
	CHECK_EQ(count_window(&laptop), before + 6);
	CHECK(head_is(&laptop, "origin/main", &last));
	settle_and_check(lcs);
	vc_repo_free(&origin);
	vc_repo_free(&laptop);
	cs_destroy(ocs);
}

/* The remote checks fast-forward from its own history, across the sender's cut. */
ENGINE_TEST(test_push_ff_checked_by_receiver)
{
	ChunkStore *ocs = memstore_create(),
			   *lcs = memstore_create_own(),
			   *acs = memstore_create_own();
	VcSource	cold = {"cold", acs};
	VcRepo		origin,
				laptop;
	Linear		l;
	uint32		seg;
	PHash		e,
				x;
	uint64		count;

	setup(&origin, ocs, "origin");
	l = build_linear(&origin);
	CHECK_EQ(clone(&laptop, lcs, &origin, 0), VC_OK);
	/* branches the laptop does not have: one at B, one off on its own */
	CHECK_EQ(vc_branch_create(&origin, "rel", &l.b), VC_OK);
	CHECK_EQ(vc_branch_create(&origin, "side", &l.c), VC_OK);
	CHECK_EQ(vc_checkout(&origin, "side"), VC_OK);
	commit_rows(&origin, 100, 200, 40, "side work");
	CHECK_EQ(vc_checkout(&origin, "main"), VC_OK);

	CHECK_EQ(vc_sever(&laptop, &depth2, &seg), VC_OK);
	CHECK_EQ(vc_segment_export(&laptop, seg, &cold), VC_OK);
	(void) release(lcs);
	CHECK(!has(lcs, &l.b));

	/* rel moves from B to the laptop's new commit: the laptop cannot see B */
	e = commit_rows(&laptop, 700, 800, 41, "E");
	CHECK_EQ(vc_branch_create(&laptop, "rel", NULL), VC_OK);
	CHECK_EQ(vc_push(&laptop, &origin, "rel"), VC_OK);
	CHECK(head_is(&origin, "rel", &e));

	/* side diverged on the remote: not a fast-forward, and nothing is left behind */
	x = commit_rows(&laptop, 900, 1000, 42, "X");
	CHECK_EQ(vc_branch_create(&laptop, "side", NULL), VC_OK);
	count = settled_count(ocs);
	CHECK_EQ(vc_push(&laptop, &origin, "side"), VC_ERR);
	CHECK(!vc_in_window(&origin, &x));
	/* (what it sent before refusing, freed with its holds: rolled back on
	 * the history store, as the refusal's error would) */
	if (frees_seen(ocs))
	{
		CHECK_EQ(settled_count(ocs), count);
		CHECK(!has(ocs, &x));
	}
	settle_and_check(ocs);
	vc_repo_free(&origin);
	vc_repo_free(&laptop);
	cs_destroy(ocs);
}

/* Deepening brings history past the cut into a segment, not the window. */
ENGINE_TEST(test_deepen_from_remote)
{
	ChunkStore *ocs = memstore_create(),
			   *lcs = memstore_create_own();
	VcRepo		origin,
				laptop;
	PHash	   *c;
	uint32		seg,
				seg2;
	VcSource	src = {"origin", ocs};
	LogAcc		acc;
	size_t		want;
	RepoStateMark state;
	int64	   *s9;

	setup(&origin, ocs, "origin");
	c = build_main(&origin, 10);	/* c[0..11] */
	s9 = snap(&origin.ctx, &c[9]);
	CHECK_EQ(clone(&laptop, lcs, &origin, 2), VC_OK);
	CHECK(vc_locate(&laptop, &c[9], NULL) == VC_LOC_BOUNDARY);

	CHECK_EQ(vc_deepen(&laptop, &src, &c[11], 3, &seg), VC_ERR);	/* in the window */
	CHECK_EQ(vc_deepen(&laptop, &src, &c[9], 3, &seg), VC_OK);
	CHECK(seg != VC_NO_SEGMENT);
	CHECK_EQ(segment_size(&laptop, seg), 3);
	CHECK(vc_locate(&laptop, &c[9], NULL) == VC_LOC_SEGMENT);
	CHECK(vc_locate(&laptop, &c[7], NULL) == VC_LOC_SEGMENT);
	CHECK(vc_locate(&laptop, &c[6], NULL) == VC_LOC_BOUNDARY);
	CHECK(hint_is(&laptop, &c[6], "origin"));
	check_snap(&laptop.ctx, &c[9], s9);
	CHECK_EQ(log_from(&laptop, &c[11], 0, &acc), VC_ERR_SEVERED);
	CHECK_EQ(acc.n, 5);
	CHECK_EQ(log_from(&laptop, &c[11], VC_WINDOW_ONLY, &acc), VC_ERR_OUTSIDE_WINDOW);
	CHECK_EQ(acc.n, 2);
	settle_and_check(lcs);

	/* done looking: discard it, which frees exactly it; deepen again later */
	want = expected_frees(&laptop, seg);
	repo_state_mark(&laptop, &state);
	CHECK_EQ(vc_segment_discard(&laptop, seg, NULL), VC_OK);
	/* (the members' own chunks, the segment's, and the repository's own it left) */
	CHECK_EQ(release(lcs), want + 1 + repo_state_unheld(&laptop, &state));
	CHECK(vc_locate(&laptop, &c[9], NULL) == VC_LOC_DISCARDED);
	CHECK_EQ(vc_deepen(&laptop, &src, &c[9], 0, &seg2), VC_OK);
	CHECK_EQ(segment_size(&laptop, seg2), 10);	/* everything down to init */
	settle_and_check(lcs);
	vc_repo_free(&origin);
	vc_repo_free(&laptop);
	cs_destroy(ocs);
}

/* The archive describes itself: a new repository restores from it alone. */
ENGINE_TEST(test_restore_from_archive_alone)
{
	ChunkStore *cs = memstore_create(),
			   *acs = archive_store_create(),
			   *ncs = memstore_create_own();
	VcSource	cold = {"cold", acs};
	VcRepo		r,
				fresh;
	Linear		l;
	uint32		seg,
				nseg;
	int64	   *sb;
	PHash	   *mans;
	size_t		nm;
	VcManifest	m;
	PHash		newest = {{0}};
	uint64		g = 0;

	setup(&r, cs, "db1");
	l = build_linear(&r);
	sb = snap(&r.ctx, &l.b);
	CHECK_EQ(vc_sever(&r, &depth2, &seg), VC_OK);
	CHECK_EQ(vc_segment_export(&r, seg, &cold), VC_OK);
	vc_repo_free(&r);			/* the exporting repository is done with */

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

	setup(&fresh, ncs, "fresh");
	/* separately started repositories share nothing, not even init */
	CHECK(vc_locate(&fresh, &l.init, NULL) == VC_LOC_UNKNOWN);
	CHECK_EQ(vc_deepen(&fresh, &cold, &newest, 0, &nseg), VC_OK);
	CHECK_EQ(segment_size(&fresh, nseg), 3);
	CHECK_EQ(vc_segment_attach(&fresh, nseg, NULL), VC_OK);
	check_snap(&fresh.ctx, &l.b, sb);
	CHECK_EQ(vc_branch_create(&fresh, "restored", &l.b), VC_OK);
	settle_and_check(ncs);
	settle_and_check(acs);
	vc_manifest_free(&m);
	vc_repo_free(&fresh);
	cs_destroy(cs);
}

/*
 * Exporting to a remote that already has the history moves almost nothing.
 * (On the memory store alone: the laptop, a memory store, exports to the
 * origin, which an archive as transactional as the laptop's store must be;
 * on the history store the origin is not.)
 */
ENGINE_MEMORY_TEST(test_export_to_remote_with_history)
{
	ChunkStore *ocs = memstore_create(),
			   *lcs = memstore_create_own(),
			   *ccs = memstore_create_own();
	VcSource	osrc = {"origin", ocs};
	VcRepo		origin,
				laptop,
				cloned;
	Linear		l;
	uint32		seg;
	uint64		stored;

	setup(&origin, ocs, "origin");
	l = build_linear(&origin);
	CHECK_EQ(clone(&laptop, lcs, &origin, 0), VC_OK);
	vc_repo_set_name(&laptop, "laptop");
	CHECK_EQ(vc_sever(&laptop, &depth2, &seg), VC_OK);
	stored = STORED(ocs);
	CHECK_EQ(vc_segment_export(&laptop, seg, &osrc), VC_OK);
	CHECK_EQ(STORED(ocs) - stored, 1);	/* the manifest */
	(void) release(lcs);
	CHECK(!has(lcs, &l.a));
	settle_and_check(ocs);
	settle_and_check(lcs);

	/* the hint follows the history: a clone of the laptop knows where B went */
	CHECK_EQ(clone(&cloned, ccs, &laptop, 0), VC_OK);
	CHECK(vc_locate(&cloned, &l.b, NULL) == VC_LOC_BOUNDARY);
	CHECK(hint_is(&cloned, &l.b, "origin"));

	CHECK_EQ(vc_segment_attach(&laptop, seg, &osrc), VC_OK);
	CHECK(has(lcs, &l.a));
	settle_and_check(lcs);
	settle_and_check(ccs);
	vc_repo_free(&origin);
	vc_repo_free(&laptop);
	vc_repo_free(&cloned);
	cs_destroy(ocs);
}

/*
 * origin: main init <- A <- m2..m9;  feature A <- f1 <- f2.
 * A depth-2 clone cannot see the base A.  Pull deepens, if allowed enough.
 */
ENGINE_TEST(test_pull_deepens_for_merge_base)
{
	ChunkStore *ocs = memstore_create(),
			   *lcs = memstore_create_own();
	VcRepo		origin,
				laptop;
	PHash	   *c;
	PHash		working,
				w;
	VcMergeResult m;

	setup(&origin, ocs, "origin");
	c = build_main(&origin, 8);
	CHECK_EQ(vc_branch_create(&origin, "feature", &c[1]), VC_OK);
	CHECK_EQ(vc_checkout(&origin, "feature"), VC_OK);
	commit_rows(&origin, 31000, 31100, 60, "f1");
	commit_rows(&origin, 31100, 31200, 61, "f2");
	CHECK_EQ(vc_checkout(&origin, "main"), VC_OK);

	CHECK_EQ(clone(&laptop, lcs, &origin, 2), VC_OK);
	CHECK(vc_locate(&laptop, &c[1], NULL) == VC_LOC_BOUNDARY);
	working = vc_working(&laptop);

	CHECK_EQ(vc_pull(&laptop, &origin, "feature", 0, &m), VC_ERR_SEVERED);
	w = vc_working(&laptop);
	CHECK(phash_eq(&working, &w));
	CHECK_EQ(vc_pull(&laptop, &origin, "feature", 3, &m), VC_ERR_SEVERED);
	CHECK_EQ(vc_pull(&laptop, &origin, "feature", 32, &m), VC_OK);
	CHECK(m.status == VC_MERGE_COMMITTED);
	vc_merge_result_free(&m);
	/* the history it needed came in as segments, not into the window */
	CHECK(vc_locate(&laptop, &c[1], NULL) == VC_LOC_SEGMENT);
	w = vc_working(&laptop);
	CHECK_EQ(row_at(&laptop, &w, 31150), 61);
	CHECK_EQ(row_at(&laptop, &w, 9 * 700), 9);
	settle_and_check(lcs);
	vc_repo_free(&origin);
	vc_repo_free(&laptop);
	cs_destroy(ocs);
}

/* A merge's parent past the depth is a boundary even though the merge names it. */
ENGINE_TEST(test_merge_parent_beyond_depth)
{
	ChunkStore *ocs = memstore_create(),
			   *lcs = memstore_create_own();
	VcSource	osrc = {"origin", ocs};
	VcRepo		origin,
				laptop;
	PHash		c,
				e,
				mm,
				f;
	VcMergeResult m;
	LogAcc		acc;
	uint32		seg;

	setup(&origin, ocs, "origin");
	commit_rows(&origin, 0, 20000, 0, "A");
	CHECK_EQ(vc_branch_create(&origin, "feature", NULL), VC_OK);
	c = commit_rows(&origin, 100, 200, 1, "C");
	CHECK_EQ(vc_checkout(&origin, "feature"), VC_OK);
	e = commit_rows(&origin, 5000, 5100, 2, "E");
	CHECK_EQ(vc_checkout(&origin, "main"), VC_OK);
	CHECK_EQ(vc_merge(&origin, "feature", &m), VC_OK);
	CHECK(m.status == VC_MERGE_COMMITTED);
	mm = m.commit;
	vc_merge_result_free(&m);
	CHECK_EQ(vc_checkout(&origin, "feature"), VC_OK);
	f = commit_rows(&origin, 6000, 6100, 3, "F");
	CHECK_EQ(vc_checkout(&origin, "main"), VC_OK);

	CHECK_EQ(clone(&laptop, lcs, &origin, 1), VC_OK);
	CHECK(vc_locate(&laptop, &mm, NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&laptop, &f, NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&laptop, &c, NULL) == VC_LOC_BOUNDARY);
	CHECK(vc_locate(&laptop, &e, NULL) == VC_LOC_BOUNDARY);
	CHECK(hint_is(&laptop, &e, "origin"));
	CHECK(!has(lcs, &e));
	CHECK_EQ(log_from(&laptop, &mm, 0, &acc), VC_ERR_SEVERED);
	CHECK_EQ(acc.n, 1);
	CHECK_EQ(vc_deepen(&laptop, &osrc, &e, 1, &seg), VC_OK);
	CHECK(vc_locate(&laptop, &e, NULL) == VC_LOC_SEGMENT);
	settle_and_check(lcs);
	vc_repo_free(&origin);
	vc_repo_free(&laptop);
	cs_destroy(ocs);
}

/* Restoring discarded history on a remote is explicit: it deepens from a source. */
ENGINE_TEST(test_explicit_restore_on_remote)
{
	ChunkStore *ocs = memstore_create(),
			   *lcs = memstore_create_own();
	VcSource	lsrc = {"laptop", lcs};
	VcRepo		origin,
				laptop;
	Linear		l;
	uint32		seg,
				rseg;
	LogAcc		acc;

	setup(&origin, ocs, "origin");
	l = build_linear(&origin);
	CHECK_EQ(clone(&laptop, lcs, &origin, 0), VC_OK);
	CHECK_EQ(vc_sever(&origin, &depth2, &seg), VC_OK);
	CHECK_EQ(vc_segment_discard(&origin, seg, NULL), VC_OK);

	CHECK_EQ(vc_deepen(&origin, &lsrc, &l.b, 0, &rseg), VC_OK);
	CHECK_EQ(segment_size(&origin, rseg), 3);
	CHECK(vc_locate(&origin, &l.a, NULL) == VC_LOC_SEGMENT);
	CHECK_EQ(vc_segment_attach(&origin, rseg, NULL), VC_OK);
	CHECK_EQ(log_from(&origin, &l.d, VC_WINDOW_ONLY, &acc), VC_OK);
	CHECK_EQ(acc.n, 5);
	settle_and_check(ocs);
	vc_repo_free(&origin);
	vc_repo_free(&laptop);
	cs_destroy(ocs);
}

/* Both sides branch, the remote has cut its history, and work still flows. */
ENGINE_TEST(test_branching_both_sides)
{
	ChunkStore *ocs = memstore_create(),
			   *lcs = memstore_create_own(),
			   *acs = archive_store_create();
	VcSource	cold = {"cold", acs};
	VcRepo		origin,
				laptop;
	Linear		l;
	uint32		seg;
	uint64		stored;
	VcMergeResult m;
	PHash		w;

	setup(&origin, ocs, "origin");
	l = build_linear(&origin);
	CHECK_EQ(clone(&laptop, lcs, &origin, 0), VC_OK);
	CHECK_EQ(vc_sever(&origin, &depth2, &seg), VC_OK);
	CHECK_EQ(vc_segment_export(&origin, seg, &cold), VC_OK);

	/* origin: feature off C; laptop: topic off D */
	CHECK_EQ(vc_branch_create(&origin, "feature", &l.c), VC_OK);
	CHECK_EQ(vc_checkout(&origin, "feature"), VC_OK);
	commit_rows(&origin, 11000, 11100, 20, "of1");
	commit_rows(&origin, 11100, 11200, 21, "of2");
	CHECK_EQ(vc_checkout(&origin, "main"), VC_OK);
	CHECK_EQ(vc_branch_create(&laptop, "topic", NULL), VC_OK);
	CHECK_EQ(vc_checkout(&laptop, "topic"), VC_OK);
	commit_rows(&laptop, 12000, 12100, 30, "lt1");
	commit_rows(&laptop, 12100, 12200, 31, "lt2");

	/* the new branch travels without any cut history */
	stored = STORED(ocs);
	CHECK_EQ(vc_push(&laptop, &origin, "topic"), VC_OK);
	CHECK_LE(STORED(ocs) - stored, 2 * (3 * 100 * 6 + 10));
	(void) release(ocs);
	if (frees_seen(ocs))
		CHECK(!has(ocs, &l.a));
	CHECK(vc_locate(&origin, &l.a, NULL) != VC_LOC_WINDOW);

	/* and the remote's branch merges into the laptop's at their base C */
	CHECK_EQ(vc_pull(&laptop, &origin, "feature", 0, &m), VC_OK);
	CHECK(m.status == VC_MERGE_COMMITTED);
	vc_merge_result_free(&m);
	w = vc_working(&laptop);
	CHECK_EQ(row_at(&laptop, &w, 11150), 21);
	CHECK_EQ(row_at(&laptop, &w, 12150), 31);
	CHECK_EQ(vc_push(&laptop, &origin, "topic"), VC_OK);
	settle_and_check(ocs);
	settle_and_check(lcs);
	vc_repo_free(&origin);
	vc_repo_free(&laptop);
	cs_destroy(ocs);
}

/* Separately started repositories share no history: every transfer is refused. */
ENGINE_TEST(test_unrelated_histories_rejected)
{
	ChunkStore *acs = memstore_create(),
			   *bcs = memstore_create_own();
	VcRepo		a,
				b;
	PHash		head;
	VcMergeResult m;
	VcBranch	tb;
	PHash		bh;
	uint64		na,
				nb;

	setup(&a, acs, "a");
	setup(&b, bcs, "b");
	bh = branch_head(&b, "main");
	CHECK(!head_is(&a, "main", &bh));
	commit_rows(&a, 0, 1000, 1, "a1");
	commit_rows(&b, 0, 1000, 1, "b1");	/* same rows, different history */
	na = settled_count(acs);
	nb = settled_count(bcs);

	CHECK_EQ(vc_fetch(&a, &b, "main", &head), VC_ERR_UNRELATED);
	CHECK_EQ(vc_push(&a, &b, "main"), VC_ERR_UNRELATED);
	CHECK_EQ(vc_branch_create(&a, "side", NULL), VC_OK);
	CHECK_EQ(vc_push(&a, &b, "side"), VC_ERR_UNRELATED);	/* a new branch too */
	CHECK_EQ(vc_pull(&a, &b, "main", 100, &m), VC_ERR_UNRELATED);
	CHECK(!vc_branch_get(&a, "b/main", &tb));
	CHECK(!vc_branch_get(&b, "side", &tb));
	CHECK_EQ(settled_count(acs), na + 1);	/* the branch side's chunk */
	CHECK_EQ(settled_count(bcs), nb);
	settle_and_check(acs);
	settle_and_check(bcs);
	vc_repo_free(&a);
	vc_repo_free(&b);
	cs_destroy(acs);
}

/*
 * The remote moved on and cut the history that joins it to the laptop's:
 * fetch is refused as detached, naming the cut and its archive, until the
 * laptop restores that stretch from the archive.
 */
ENGINE_TEST(test_detached_fetch_until_repaired)
{
	ChunkStore *ocs = memstore_create(),
			   *lcs = memstore_create_own(),
			   *acs = archive_store_create();
	VcSource	cold = {"cold", acs};
	VcRepo		origin,
				laptop;
	PHash		c[6],
				head;
	uint32		seg,
				dseg;
	VcMergeResult m;
	uint64		before;

	setup(&origin, ocs, "origin");
	(void) build_linear(&origin);
	CHECK_EQ(clone(&laptop, lcs, &origin, 2), VC_OK);	/* knows C, D */
	for (int i = 0; i < 6; i++)
		c[i] = commit_rows(&origin, 100 + (uint64) i * 10, 105 + (uint64) i * 10,
						   50 + (uint64) i, "E..J");
	CHECK_EQ(vc_sever(&origin, &depth2, &seg), VC_OK);
	CHECK_EQ(vc_segment_export(&origin, seg, &cold), VC_OK);
	(void) release(ocs);

	before = settled_count(lcs);
	CHECK_EQ(vc_fetch(&laptop, &origin, "main", &head), VC_ERR_DETACHED);
	CHECK(phash_eq(&laptop.ctx.err_hash, &c[3]));
	CHECK(hint_is(&origin, &c[3], "cold"));
	CHECK_EQ(settled_count(lcs), before);
	CHECK_EQ(vc_pull(&laptop, &origin, "main", 100, &m), VC_ERR_DETACHED);
	settle_and_check(lcs);

	/* repair: bring the joining stretch in from the archive, then fetch */
	CHECK_EQ(vc_deepen(&laptop, &cold, &c[3], 0, &dseg), VC_OK);
	CHECK_EQ(segment_size(&laptop, dseg), 4);	/* E..H, stopping at D */
	CHECK_EQ(vc_pull(&laptop, &origin, "main", 0, &m), VC_OK);
	CHECK(m.status == VC_MERGE_FAST_FORWARD && phash_eq(&m.commit, &c[5]));
	vc_merge_result_free(&m);
	settle_and_check(lcs);
	vc_repo_free(&origin);
	vc_repo_free(&laptop);
	cs_destroy(ocs);
}

/*
 * The laptop worked offline and cut its own history: its push cannot be
 * shown to attach and is refused, leaving nothing on the remote, until the
 * laptop attaches its segment again.
 */
ENGINE_TEST(test_detached_push_until_attached)
{
	ChunkStore *ocs = memstore_create(),
			   *lcs = memstore_create_own(),
			   *acs = memstore_create_own();
	VcSource	cold = {"cold", acs};
	VcRepo		origin,
				laptop;
	PHash		c[5];
	uint32		seg;
	uint64		before;

	setup(&origin, ocs, "origin");
	(void) build_linear(&origin);
	CHECK_EQ(clone(&laptop, lcs, &origin, 0), VC_OK);
	vc_repo_set_name(&laptop, "laptop");
	for (int i = 0; i < 5; i++)
		c[i] = commit_rows(&laptop, 200 + (uint64) i * 10, 205 + (uint64) i * 10,
						   80 + (uint64) i, "offline");
	CHECK_EQ(vc_sever(&laptop, &depth2, &seg), VC_OK);
	CHECK_EQ(vc_segment_export(&laptop, seg, &cold), VC_OK);
	(void) release(lcs);

	before = settled_count(ocs);
	CHECK_EQ(vc_push(&laptop, &origin, "main"), VC_ERR_DETACHED);
	CHECK(phash_eq(&laptop.ctx.err_hash, &c[2]));
	/* (what it sent before refusing, freed with its holds: rolled back on
	 * the history store, as the refusal's error would) */
	if (frees_seen(ocs))
		CHECK_EQ(settled_count(ocs), before);
	settle_and_check(ocs);

	CHECK_EQ(vc_segment_attach(&laptop, seg, &cold), VC_OK);
	CHECK_EQ(vc_push(&laptop, &origin, "main"), VC_OK);
	CHECK(head_is(&origin, "main", &c[4]));
	settle_and_check(ocs);
	vc_repo_free(&origin);
	vc_repo_free(&laptop);
	cs_destroy(ocs);
}

/*
 * Attaching through a cut commit: the remote discarded B, but a new branch
 * built on B still attaches there (same hash, same history), and B itself
 * is not sent back.
 */
ENGINE_TEST(test_attach_at_cut_commit)
{
	ChunkStore *ocs = memstore_create(),
			   *lcs = memstore_create_own();
	VcRepo		origin,
				laptop;
	Linear		l;
	uint32		seg;
	PHash		x;
	LogAcc		acc;

	setup(&origin, ocs, "origin");
	l = build_linear(&origin);
	CHECK_EQ(clone(&laptop, lcs, &origin, 0), VC_OK);
	CHECK_EQ(vc_sever(&origin, &depth2, &seg), VC_OK);
	CHECK_EQ(vc_segment_discard(&origin, seg, NULL), VC_OK);

	CHECK_EQ(vc_branch_create(&laptop, "hotfix", &l.b), VC_OK);
	CHECK_EQ(vc_checkout(&laptop, "hotfix"), VC_OK);
	x = commit_rows(&laptop, 300, 310, 90, "fix on B");
	CHECK_EQ(vc_push(&laptop, &origin, "hotfix"), VC_OK);
	CHECK(head_is(&origin, "hotfix", &x));
	(void) release(ocs);
	if (frees_seen(ocs))
		CHECK(!has(ocs, &l.b));
	CHECK(vc_locate(&origin, &l.b, NULL) == VC_LOC_DISCARDED);
	/* (gone from the store once the holds end; past the window's edge in any case) */
	if (frees_seen(ocs))
		CHECK_EQ(log_from(&origin, &x, 0, &acc), VC_ERR_SEVERED);
	else
		CHECK_EQ(log_from(&origin, &x, VC_WINDOW_ONLY, &acc), VC_ERR_OUTSIDE_WINDOW);
	CHECK_EQ(acc.n, 1);
	settle_and_check(ocs);
	vc_repo_free(&origin);
	vc_repo_free(&laptop);
	cs_destroy(ocs);
}

/*
 * Discarded commits nothing held names are forgotten: a branch built on
 * one alone does not attach (the receiver knows none of its history), and
 * a commit sent that builds on one, and on what the receiver holds, brings
 * it back with what it needs; discarded history it still names stays.
 */
ENGINE_TEST(test_discarded_returns_only_when_built_on)
{
	ChunkStore *ocs = memstore_create(),
			   *lcs = memstore_create_own();
	VcRepo		origin,
				laptop;
	Linear		l;
	uint32		seg;
	VcMergeResult m;

	setup(&origin, ocs, "origin");
	l = build_linear(&origin);
	CHECK_EQ(clone(&laptop, lcs, &origin, 0), VC_OK);
	CHECK_EQ(vc_sever(&origin, &depth2, &seg), VC_OK);	/* init, A, B cut */
	CHECK_EQ(vc_segment_discard(&origin, seg, NULL), VC_OK);
	(void) release(ocs);
	CHECK(vc_locate(&origin, &l.a, NULL) == VC_LOC_UNKNOWN);	/* forgotten */
	CHECK(vc_locate(&origin, &l.b, NULL) == VC_LOC_DISCARDED);	/* C names it */

	/* a branch on A alone: origin knows nothing of its history */
	CHECK_EQ(vc_branch_create(&laptop, "side", &l.a), VC_OK);
	CHECK_EQ(vc_checkout(&laptop, "side"), VC_OK);
	commit_rows(&laptop, 400, 410, 50, "on A");
	CHECK_EQ(vc_push(&laptop, &origin, "side"), VC_ERR_UNRELATED);
	(void) release(ocs);
	if (frees_seen(ocs))
		CHECK(!has(ocs, &l.a));
	CHECK(vc_locate(&origin, &l.a, NULL) != VC_LOC_WINDOW);

	/* merged with D, which origin holds: it attaches, and A comes back */
	CHECK_EQ(vc_merge(&laptop, "main", &m), VC_OK);
	CHECK(m.status == VC_MERGE_COMMITTED);
	vc_merge_result_free(&m);
	CHECK_EQ(vc_push(&laptop, &origin, "side"), VC_OK);
	CHECK(vc_locate(&origin, &l.a, NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&origin, &l.init, NULL) == VC_LOC_WINDOW);
	CHECK(vc_locate(&origin, &l.b, NULL) == VC_LOC_DISCARDED);	/* not sent */
	settle_and_check(ocs);
	CHECK(has(ocs, &l.a));
	if (frees_seen(ocs))
		CHECK(!has(ocs, &l.b));
	vc_repo_free(&origin);
	vc_repo_free(&laptop);
	cs_destroy(ocs);
}

/* A push does not overwrite work not committed on the remote's branch. */
ENGINE_TEST(test_push_keeps_remote_work)
{
	ChunkStore *ocs = memstore_create(),
			   *lcs = memstore_create_own();
	VcRepo		origin,
				laptop;
	PHash		before,
				w;

	setup(&origin, ocs, "origin");
	before = commit_rows(&origin, 0, 100, 1, "A");
	CHECK_EQ(clone(&laptop, lcs, &origin, 0), VC_OK);
	put_rows(&origin, 500, 510, 7);		/* not committed */
	commit_rows(&laptop, 200, 210, 2, "L");
	CHECK_EQ(vc_push(&laptop, &origin, "main"), VC_ERR);
	CHECK(head_is(&origin, "main", &before));
	w = vc_working(&origin);
	CHECK_EQ(row_at(&origin, &w, 505), 7);
	/* committed, the remote's branch has moved on: not a fast-forward */
	vc_commit(&origin, "O");
	CHECK_EQ(vc_push(&laptop, &origin, "main"), VC_ERR);

	/*
	 * a merge under way there, its working set the head's (each side set
	 * row 5, ours kept, the conflict recorded): not overwritten either
	 */
	{
		VcMergeResult m;
		VcBranch	b;
		PHash		x;

		CHECK_EQ(vc_branch_create(&origin, "x", NULL), VC_OK);
		CHECK_EQ(vc_checkout(&origin, "x"), VC_OK);
		x = commit_rows(&origin, 5, 6, 77, "x");
		CHECK_EQ(vc_checkout(&origin, "main"), VC_OK);
		before = commit_rows(&origin, 5, 6, 88, "main");
		CHECK_EQ(vc_merge(&origin, "x", &m), VC_OK);
		CHECK(m.status == VC_MERGE_CONFLICTS);
		vc_merge_result_free(&m);
		vc_head(&origin, &b);
		CHECK(b.merging && phash_eq(&b.merge_head, &x));
		CHECK(phash_eq(&b.working, &b.head_root));
		vc_branch_free(&b);
		CHECK_EQ(vc_pull(&laptop, &origin, "main", 0, &m), VC_OK);	/* attached */
		vc_merge_result_free(&m);
		commit_rows(&laptop, 220, 230, 3, "L2");
		CHECK_EQ(vc_push(&laptop, &origin, "main"), VC_ERR);
		CHECK(head_is(&origin, "main", &before));
	}
	settle_and_check(ocs);
	vc_repo_free(&origin);
	vc_repo_free(&laptop);
	cs_destroy(ocs);
}

/*
 * A receiver finds the parents of what it holds (every commit it holds
 * read) only for a commit as old as its history's discarded part: a push
 * of new work after a discard reads what the push needs, not the window.
 */
ENGINE_TEST(test_transfer_reads_not_the_window)
{
	ChunkStore *ocs = memstore_create(),
			   *lcs = memstore_create_own();
	VcRepo		origin,
				laptop;
	uint32		seg;
	uint64		gets;

	setup(&origin, ocs, "origin");
	for (int i = 0; i < 40; i++)
		commit_rows(&origin, (uint64) i * 10, (uint64) i * 10 + 5, (uint64) i, "c");
	CHECK_EQ(clone(&laptop, lcs, &origin, 0), VC_OK);
	CHECK_EQ(vc_sever(&origin, &(VcWindow) {.depth = 30}, &seg), VC_OK);
	CHECK_EQ(vc_segment_discard(&origin, seg, NULL), VC_OK);
	commit_rows(&laptop, 900, 905, 99, "new");
	gets = cs_stats(ocs)->gets;
	CHECK_EQ(vc_push(&laptop, &origin, "main"), VC_OK);
	/* (the state, the branch, the fast-forward check: not 30 commits) */
	CHECK_LT(cs_stats(ocs)->gets - gets, 20);
	settle_and_check(ocs);
	vc_repo_free(&origin);
	vc_repo_free(&laptop);
	cs_destroy(ocs);
}

/* How many times a name is in the repository's state chunk. */
static int
state_names(VcRepo *r, const char *name)
{
	PHash		h;
	uint32		len;
	uint8	   *d;
	size_t		nl = strlen(name);
	int			n = 0;

	CHECK(vc_refs_get(r->refs, "vc/repo", VC_REF_READ, &h));
	d = cs_get(r->ctx.cs, h.b, &len);
	CHECK(d != NULL);
	for (uint32 i = 0; i + nl <= len; i++)
		if (memcmp(d + i, name, nl) == 0)
			n++;
	pfree(d);
	return n;
}

/* Boundaries share their hint: the state names each place history came from once. */
ENGINE_TEST(test_hints_named_once)
{
	ChunkStore *ocs = memstore_create(),
			   *lcs = memstore_create_own();
	VcSource	osrc = {"origin", ocs};
	VcRepo		origin,
				laptop;
	PHash	   *c;
	uint32		seg;
	int			bounds = 0;

	setup(&origin, ocs, "origin");
	c = build_main(&origin, 10);
	CHECK_EQ(vc_branch_create(&origin, "feature", &c[5]), VC_OK);
	CHECK_EQ(vc_checkout(&origin, "feature"), VC_OK);
	commit_rows(&origin, 30000, 30100, 50, "f1");
	CHECK_EQ(vc_checkout(&origin, "main"), VC_OK);

	/* two branches cut at depth 1: two boundaries, both hinting at origin */
	CHECK_EQ(clone(&laptop, lcs, &origin, 1), VC_OK);
	CHECK(vc_locate(&laptop, &c[10], NULL) == VC_LOC_BOUNDARY);
	CHECK(vc_locate(&laptop, &c[5], NULL) == VC_LOC_BOUNDARY);
	CHECK(hint_is(&laptop, &c[10], "origin") && hint_is(&laptop, &c[5], "origin"));
	CHECK_EQ(state_names(&laptop, "origin"), 1);
	/* deepening from origin leaves a boundary further down, the same hint */
	CHECK_EQ(vc_deepen(&laptop, &osrc, &c[10], 2, &seg), VC_OK);
	CHECK(vc_locate(&laptop, &c[8], NULL) == VC_LOC_BOUNDARY);
	CHECK(hint_is(&laptop, &c[8], "origin"));
	CHECK_EQ(state_names(&laptop, "origin"), 1);
	/* (c[10] is now the segment's, c[5] and c[8] the boundaries) */
	vc_locations(&laptop, count_bounds_fn, &bounds);
	CHECK_EQ(bounds, 2);
	settle_and_check(lcs);
	vc_repo_free(&origin);
	vc_repo_free(&laptop);
	cs_destroy(ocs);
}

/* Pull names its tracking branch after the remote: one without a name is refused. */
ENGINE_TEST(test_pull_needs_named_remote)
{
	ChunkStore *ocs = memstore_create(),
			   *lcs = memstore_create_own();
	VcRepo		origin,
				laptop;
	VcMergeResult m;
	VcBranch	tb;

	setup(&origin, ocs, NULL);
	commit_rows(&origin, 0, 100, 1, "A");
	CHECK_EQ(clone(&laptop, lcs, &origin, 0), VC_OK);
	commit_rows(&origin, 100, 200, 2, "B");
	CHECK_EQ(vc_pull(&laptop, &origin, "main", 0, &m), VC_ERR);
	CHECK(!vc_branch_get(&laptop, "/main", &tb));
	vc_repo_set_name(&origin, "origin");
	CHECK_EQ(vc_pull(&laptop, &origin, "main", 0, &m), VC_OK);
	CHECK(m.status == VC_MERGE_FAST_FORWARD);
	vc_merge_result_free(&m);
	settle_and_check(lcs);
	vc_repo_free(&origin);
	vc_repo_free(&laptop);
	cs_destroy(ocs);
}

/*
 * As attach_at_cut_commit, the segment exported before it is discarded:
 * B, still named by C, is known as discarded by what the archive's
 * manifest says of the segment, and a branch on it attaches there.
 */
ENGINE_TEST(test_attach_at_exported_cut_commit)
{
	ChunkStore *ocs = memstore_create(),
			   *lcs = memstore_create_own(),
			   *acs = archive_store_create();
	VcSource	cold = {"cold", acs};
	VcRepo		origin,
				laptop;
	Linear		l;
	uint32		seg;
	PHash		x;

	setup(&origin, ocs, "origin");
	l = build_linear(&origin);
	CHECK_EQ(clone(&laptop, lcs, &origin, 0), VC_OK);
	CHECK_EQ(vc_sever(&origin, &depth2, &seg), VC_OK);
	CHECK_EQ(vc_segment_export(&origin, seg, &cold), VC_OK);
	CHECK_EQ(vc_segment_discard(&origin, seg, &cold), VC_OK);
	CHECK(vc_locate(&origin, &l.b, NULL) == VC_LOC_DISCARDED);

	CHECK_EQ(vc_branch_create(&laptop, "hotfix", &l.b), VC_OK);
	CHECK_EQ(vc_checkout(&laptop, "hotfix"), VC_OK);
	x = commit_rows(&laptop, 300, 310, 90, "fix on B");
	CHECK_EQ(vc_push(&laptop, &origin, "hotfix"), VC_OK);
	CHECK(head_is(&origin, "hotfix", &x));
	(void) release(ocs);
	if (frees_seen(ocs))
		CHECK(!has(ocs, &l.b));
	CHECK(vc_locate(&origin, &l.b, NULL) == VC_LOC_DISCARDED);
	settle_and_check(ocs);
	vc_repo_free(&origin);
	vc_repo_free(&laptop);
	cs_destroy(ocs);
}

/* -------------------------------------------------------------- costs */

#define RS_K 20

static PHash
random_commit(VcRepo *r, uint64 ver)
{
	EditBatch	b;
	PHash		rv = vc_working_lock(r),
				out;

	batch_init(&b, RS_K);
	for (int j = 0; j < RS_K; j++)
		batch_put(&b, rnd_below_global(20000), ver);
	CHECK_EQ(vc_table_edit(&r->ctx, &rv, "t", b.e, b.n, &out), VC_OK);
	vc_set_working(r, &out);
	batch_free(&b);
	return vc_commit(r, "random");
}

/* Each transfer stores what its commits changed, not the history behind them. */
ENGINE_TEST(test_transfer_costs)
{
	ChunkStore *ocs = memstore_create(),
			   *scs = memstore_create_own(),
			   *fcs = memstore_create_own();
	VcSource	osrc = {"origin", ocs};
	VcRepo		origin,
				shallow,
				full;
	uint64		per_commit = 3 * RS_K * 6 + 10;
	uint64		stored;
	VcMergeResult m;
	PHash		head,
				edge = {{0}};
	uint32		seg;

	rng_seed_global(99);
	setup(&origin, ocs, "origin");
	commit_rows(&origin, 0, 20000, 0, "load");
	for (int i = 0; i < 30; i++)
		random_commit(&origin, (uint64) i + 1);

	CHECK_EQ(clone(&shallow, scs, &origin, 2), VC_OK);
	CHECK_EQ(clone(&full, fcs, &origin, 0), VC_OK);
	/* a shallow clone is one table plus two commits of change */
	CHECK_LE(settled_count(scs) + 25 * RS_K, settled_count(fcs));

	for (int i = 0; i < 5; i++)
		random_commit(&origin, 100 + (uint64) i);
	stored = STORED(scs);
	CHECK_EQ(vc_fetch(&shallow, &origin, "main", &head), VC_OK);
	CHECK_LE(STORED(scs) - stored, 5 * per_commit);

	CHECK_EQ(vc_branch_create(&shallow, "origin/main", &head), VC_OK);
	CHECK_EQ(vc_merge(&shallow, "origin/main", &m), VC_OK);
	CHECK(m.status == VC_MERGE_FAST_FORWARD);
	vc_merge_result_free(&m);
	for (int i = 0; i < 3; i++)
		random_commit(&shallow, 200 + (uint64) i);
	stored = STORED(ocs);
	CHECK_EQ(vc_push(&shallow, &origin, "main"), VC_OK);
	CHECK_LE(STORED(ocs) - stored, 3 * per_commit);

	vc_locations(&shallow, boundary_fn, &edge);
	stored = STORED(scs);
	CHECK_EQ(vc_deepen(&shallow, &osrc, &edge, 5, &seg), VC_OK);
	CHECK_LE(STORED(scs) - stored, 5 * per_commit);
	settle_and_check(scs);
	settle_and_check(ocs);
	vc_repo_free(&origin);
	vc_repo_free(&shallow);
	vc_repo_free(&full);
	cs_destroy(ocs);
}

static const ProllyUnitTest tests[] = {
	{"push_does_not_resurrect", test_push_does_not_resurrect},
	{"shallow_clone_with_hints", test_shallow_clone_with_hints},
	{"clone_into_empty_only", test_clone_into_empty_only},
	{"fetch_connects_past_depth", test_fetch_connects_past_depth},
	{"push_ff_checked_by_receiver", test_push_ff_checked_by_receiver},
	{"deepen_from_remote", test_deepen_from_remote},
	{"restore_from_archive_alone", test_restore_from_archive_alone},
	{"export_to_remote_with_history", test_export_to_remote_with_history},
	{"pull_deepens_for_merge_base", test_pull_deepens_for_merge_base},
	{"merge_parent_beyond_depth", test_merge_parent_beyond_depth},
	{"explicit_restore_on_remote", test_explicit_restore_on_remote},
	{"branching_both_sides", test_branching_both_sides},
	{"unrelated_histories_rejected", test_unrelated_histories_rejected},
	{"detached_fetch_until_repaired", test_detached_fetch_until_repaired},
	{"detached_push_until_attached", test_detached_push_until_attached},
	{"attach_at_cut_commit", test_attach_at_cut_commit},
	{"attach_at_exported_cut_commit", test_attach_at_exported_cut_commit},
	{"discarded_returns_only_when_built_on", test_discarded_returns_only_when_built_on},
	{"push_keeps_remote_work", test_push_keeps_remote_work},
	{"hints_named_once", test_hints_named_once},
	{"pull_needs_named_remote", test_pull_needs_named_remote},
	{"transfer_reads_not_the_window", test_transfer_reads_not_the_window},
	{"transfer_costs", test_transfer_costs},
};

const ProllyUnitSuite unit_engine_remote_suite = {"engine_remote", tests, lengthof(tests)};
