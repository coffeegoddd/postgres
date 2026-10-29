/*
 * test_vcg.c
 *	  Parents in a commit graph: commits keep their hashes through a sever,
 *	  the oldest kept commit becomes a root, and what was cut comes back
 *	  only if it hashes to what the cut recorded.  Edges from other repos
 *	  are checked against their graph root, the generations at both ends,
 *	  and any edge already known.
 */
#include "test_util.h"
#include "../vcg.h"
#include "../vc.h"

static const char *ddl = "CREATE TABLE t (id int8 PRIMARY KEY, v int8)";

/* ---------------------------------------------------------------- helpers */

static PHash
root_with_table(ProllyCtx *ctx)
{
	PHash		rv = vc_root_empty(ctx),
				out;

	CHECK(vc_table_create(ctx, &rv, "t", ddl, strlen(ddl), &out) == 0);
	return out;
}

/* The head's root value with one row set. */
static PHash
edit_head(VcgRepo *r, uint64_t key, uint64_t ver)
{
	VcgCommit	c;
	VcTable		t;
	PHash		rv,
				out;
	EditBatch	b;

	CHECK(vcg_commit_load(&r->ctx, &vcg_head(r)->head, &c));
	rv = c.root;
	if (!vc_table_get(&r->ctx, &rv, "t", &t))
		rv = root_with_table(&r->ctx);
	batch_init(&b, 1);
	batch_put(&b, key, ver);
	CHECK(vc_table_edit(&r->ctx, &rv, "t", b.e, b.n, &out) == 0);
	batch_free(&b);
	return out;
}

static PHash
commit_one(VcgRepo *r, uint64_t key, uint64_t ver, const char *msg)
{
	PHash		rv = edit_head(r, key, ver);

	return vcg_commit(r, &rv, msg);
}

typedef struct LogAcc
{
	PHash		h[64];
	int			n;
} LogAcc;

static void
log_fn(void *arg, const VcgCommit *c)
{
	LogAcc	   *a = arg;

	if (a->n < 64)
		a->h[a->n] = c->hash;
	a->n++;
}

static int
log_len(VcgRepo *r, const PHash *from, int *rc)
{
	LogAcc		a = {0};

	*rc = vcg_log(r, from, 1000, log_fn, &a);
	return a.n;
}

static PHash
graph_root(VcgRepo *r)
{
	return r->graph->ops->root(r->graph);
}

static bool
same_graph(VcgRepo *r, const PHash *h)
{
	PHash		g = graph_root(r);

	return phash_eq(&g, h);
}

static bool
same_graphs(VcgRepo *a, VcgRepo *b)
{
	PHash		g = graph_root(b);

	return same_graph(a, &g);
}

/* init <- a <- b <- c <- d <- e on main; h[0] is init. */
static void
build_chain(VcgRepo *r, PHash h[6])
{
	static const char *names[] = {"init", "a", "b", "c", "d", "e"};

	h[0] = vcg_head(r)->head;
	for (int i = 1; i < 6; i++)
		h[i] = commit_one(r, (uint64_t) i, (uint64_t) i, names[i]);
}

static bool
is_root_with_seam(VcgRepo *r, const PHash *h, const PHash *seam)
{
	CgEdge		e;

	CHECK(vcg_edge(r, h, &e) == PROLLY_OK);
	return e.nparents == 0 && e.nseams == 1 && (seam == NULL || phash_eq(&e.seams[0], seam));
}

/* ------------------------------------------------------------------ tests */

static void
test_nonce_makes_commits_unique(void)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx;
	PHash		rv,
				a,
				b,
				c;
	uint8_t		nonce[VCG_NONCE_LEN] = {7};
	VcgCommit	ca,
				cb;

	prolly_ctx_init(&ctx, cs, &small_policy);
	rv = vc_root_empty(&ctx);
	/* the same root, generation, time and message: still two commits */
	a = vcg_commit_write(&ctx, &rv, 2, NULL, 5, "same");
	b = vcg_commit_write(&ctx, &rv, 2, NULL, 5, "same");
	CHECK(!phash_eq(&a, &b));
	CHECK(vcg_commit_load(&ctx, &a, &ca));
	CHECK(vcg_commit_load(&ctx, &b, &cb));
	CHECK(memcmp(ca.nonce, cb.nonce, VCG_NONCE_LEN) != 0);
	CHECK(ca.generation == 2 && ca.time == 5 && ca.msglen == 4 &&
		  memcmp(ca.msg, "same", 4) == 0 && phash_eq(&ca.root, &rv));
	/* a given nonce makes it deterministic: the hash is the content */
	b = vcg_commit_write(&ctx, &rv, 2, nonce, 5, "same");
	c = vcg_commit_write(&ctx, &rv, 2, nonce, 5, "same");
	CHECK(phash_eq(&b, &c));
	prolly_release_pins(&ctx);
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
}

/*
 * The example: init <- a <- b <- c <- d <- e, a window of three.  The sever
 * leaves c <- d <- e, c is a root, and no hash changed.
 */
static void
test_sever_makes_a_root(void)
{
	ChunkStore *cs = memstore_create();
	VcgRepo		r;
	PHash		h[6],
				before;
	uint32_t	seg;
	int			rc;

	vcg_repo_init(&r, cs, &small_policy);
	build_chain(&r, h);
	CHECK_EQ(log_len(&r, &h[5], &rc), 6);
	before = graph_root(&r);

	CHECK(vcg_sever(&r, 3, &seg) == VCG_OK);
	CHECK(seg == 0);
	CHECK(phash_eq(&vcg_head(&r)->head, &h[5]));
	CHECK_EQ(r.held.n, 3);
	CHECK(is_root_with_seam(&r, &h[3], &r.segs[0].hash));
	CHECK(!same_graph(&r, &before));

	/* the log ends at c, cleanly: c is where history starts here */
	CHECK_EQ(log_len(&r, &h[5], &rc), 3);
	CHECK(rc == VCG_OK);
	/* the cut commits have no edges here any more */
	{
		CgEdge		e;

		CHECK(vcg_edge(&r, &h[2], &e) == PROLLY_NOT_FOUND);
	}
	/* nothing more to cut */
	CHECK(vcg_sever(&r, 3, &seg) == VCG_OK);
	CHECK(seg == VCG_NO_SEGMENT);
	/* a commit on top moves the window: d's turn to be cut, e is unchanged */
	{
		PHash		f = commit_one(&r, 6, 6, "f");

		CHECK(vcg_sever(&r, 3, &seg) == VCG_OK);
		CHECK(seg == 1);
		CHECK(is_root_with_seam(&r, &h[4], &r.segs[1].hash));
		CHECK_EQ(log_len(&r, &f, &rc), 3);
	}
	check_store(cs, 0);
	vcg_repo_free(&r);
	cs_destroy(cs);
}

/* Relinking puts back exactly the graph that was cut: the same root hash. */
static void
test_relink_local_restores_graph(void)
{
	ChunkStore *cs = memstore_create();
	VcgRepo		r;
	PHash		h[6],
				before;
	uint32_t	seg;
	int			rc;
	CgEdge		e;

	vcg_repo_init(&r, cs, &small_policy);
	build_chain(&r, h);
	before = graph_root(&r);
	CHECK(vcg_sever(&r, 3, &seg) == VCG_OK);
	CHECK(vcg_relink(&r, seg, NULL) == VCG_OK);
	CHECK(same_graph(&r, &before));
	CHECK_EQ(log_len(&r, &h[5], &rc), 6);
	CHECK(vcg_edge(&r, &h[3], &e) == PROLLY_OK);
	CHECK(e.nparents == 1 && e.nseams == 0 && phash_eq(&e.parents[0], &h[2]));
	CHECK_EQ(r.held.n, 6);
	CHECK(r.segs[seg].state == VCG_SEG_RELINKED);
	CHECK(vcg_relink(&r, seg, NULL) == VCG_ERR);
	check_store(cs, 0);
	/* relinked commits are held once: cut and discarded again, they go */
	CHECK(vcg_sever(&r, 1, &seg) == VCG_OK);
	CHECK(vcg_segment_discard(&r, seg) == VCG_OK);
	cs_collect_orphans(cs);
	for (int i = 0; i < 5; i++)
		CHECK(!cs_has(cs, &h[i]));
	CHECK(cs_has(cs, &h[5]));
	vcg_repo_free(&r);
	cs_destroy(cs);
}

/* Exported history leaves the store and comes back whole from the archive. */
static void
test_export_and_relink_from_archive(void)
{
	ChunkStore *cs = memstore_create(),
			   *archive = memstore_create();
	VcgRepo		r;
	PHash		h[6],
				before;
	uint32_t	seg;
	int			rc;

	vcg_repo_init(&r, cs, &small_policy);
	build_chain(&r, h);
	before = graph_root(&r);
	CHECK(vcg_sever(&r, 3, &seg) == VCG_OK);
	CHECK(vcg_segment_export(&r, seg, archive) == VCG_OK);
	cs_collect_orphans(cs);
	for (int i = 0; i < 3; i++)
	{
		CHECK(!cs_has(cs, &h[i]));
		CHECK(cs_has(archive, &h[i]));
	}
	CHECK(!cs_has(cs, &r.segs[seg].hash));
	check_store(cs, 0);

	CHECK(vcg_relink(&r, seg, archive) == VCG_OK);
	CHECK(same_graph(&r, &before));
	CHECK_EQ(log_len(&r, &h[5], &rc), 6);
	CHECK(rc == VCG_OK);
	check_store(cs, 0);
	vcg_repo_free(&r);
	cs_destroy(cs);
	cs_destroy(archive);
}

/*
 * A discarded segment is known only by its hash, and that is enough to
 * refuse anything that is not it: a store without it, one with other bytes
 * under its name, one whose commits were altered.  A refusal changes
 * nothing; the true segment is then accepted.
 */
static void
test_relink_refuses_forgeries(void)
{
	ChunkStore *cs = memstore_create(),
			   *good = memstore_create(),
			   *other = memstore_create(),
			   *bad_seg = memstore_create(),
			   *bad_commit = memstore_create();
	VcgRepo		r,
				o;
	PHash		h[6],
				H,
				cut_graph,
				oh[6];
	uint32_t	seg,
				oseg;
	int			rc;

	vcg_repo_init(&r, cs, &small_policy);
	build_chain(&r, h);
	CHECK(vcg_sever(&r, 3, &seg) == VCG_OK);
	H = r.segs[seg].hash;
	CHECK(vcg_segment_export(&r, seg, good) == VCG_OK);
	CHECK(vcg_segment_export(&r, seg, bad_seg) == VCG_ERR);	/* not local now */
	{
		/* copies of the true archive, damaged two ways */
		PinSet		p1,
					p2;
		PHash		bad;

		pinset_init(&p1, bad_seg);
		pinset_init(&p2, bad_commit);
		CHECK(cs_copy_closure(good, &p1, &H, true, &bad) == CS_OK);
		CHECK(cs_copy_closure(good, &p2, &H, true, &bad) == CS_OK);
		for (int i = 0; i < 3; i++)
		{
			CHECK(cs_copy_closure(good, &p1, &h[i], true, &bad) == CS_OK);
			CHECK(cs_copy_closure(good, &p2, &h[i], true, &bad) == CS_OK);
		}
		/* held there, as an archive holds what it keeps */
		cs_ref_root(bad_seg, &H);
		cs_ref_root(bad_commit, &H);
		for (int i = 0; i < 3; i++)
		{
			cs_ref_root(bad_seg, &h[i]);
			cs_ref_root(bad_commit, &h[i]);
		}
		pinset_free(&p1);
		pinset_free(&p2);
	}
	CHECK(vcg_segment_discard(&r, seg) == VCG_OK);
	cs_collect_orphans(cs);
	cut_graph = graph_root(&r);

	/*
	 * Another history, cut the same way.  Its chunks are well formed, so
	 * only their hashes give them away: its segment's graph stored under
	 * H, and its commit a stored under our a's name.
	 */
	vcg_repo_init(&o, other, &small_policy);
	build_chain(&o, oh);
	CHECK(vcg_sever(&o, 3, &oseg) == VCG_OK);
	{
		size_t		len;
		const uint8_t *d = cs_get(other, &o.segs[oseg].hash, &len);

		bad_seg->ops->del(bad_seg, &H);
		CHECK(bad_seg->ops->put(bad_seg, &H, d, len));
		d = cs_get(other, &oh[1], &len);
		bad_commit->ops->del(bad_commit, &h[1]);
		CHECK(bad_commit->ops->put(bad_commit, &h[1], d, len));
	}
	CHECK(vcg_relink(&r, seg, other) == VCG_ERR_SEVERED);
	CHECK(vcg_relink(&r, seg, bad_seg) == VCG_ERR_FORGED);
	CHECK(vcg_relink(&r, seg, bad_commit) == VCG_ERR_FORGED);
	CHECK(same_graph(&r, &cut_graph));
	CHECK_EQ(log_len(&r, &h[5], &rc), 3);
	CHECK(r.segs[seg].state == VCG_SEG_DISCARDED);
	cs_collect_orphans(cs);
	check_store(cs, 0);

	CHECK(vcg_relink(&r, seg, good) == VCG_OK);
	CHECK_EQ(log_len(&r, &h[5], &rc), 6);
	check_store(cs, 0);
	vcg_repo_free(&r);
	vcg_repo_free(&o);
	cs_destroy(cs);
	cs_destroy(good);
	cs_destroy(other);
	cs_destroy(bad_seg);
	cs_destroy(bad_commit);
}

/*
 * Two cuts: the older segment's kept commit was itself cut later, so the
 * newer segment goes back first; then both give back the original graph.
 */
static void
test_relink_newest_first(void)
{
	ChunkStore *cs = memstore_create();
	VcgRepo		r;
	PHash		h[6];
	uint32_t	s0,
				s1;
	int			rc;

	vcg_repo_init(&r, cs, &small_policy);
	build_chain(&r, h);
	CHECK(vcg_sever(&r, 4, &s0) == VCG_OK);	/* cuts init, a; b is a root */
	CHECK(is_root_with_seam(&r, &h[2], &r.segs[s0].hash));
	{
		PHash		f = commit_one(&r, 6, 6, "f"),
					g = commit_one(&r, 7, 7, "g");

		(void) f;
		CHECK(vcg_sever(&r, 2, &s1) == VCG_OK);	/* cuts b..e; f is a root */
		CHECK_EQ(log_len(&r, &g, &rc), 2);
		CHECK(vcg_relink(&r, s0, NULL) == VCG_ERR_ORDER);
		CHECK(vcg_relink(&r, s1, NULL) == VCG_OK);
		CHECK_EQ(log_len(&r, &g, &rc), 6);	/* back to b, still a root */
		CHECK(is_root_with_seam(&r, &h[2], &r.segs[s0].hash));
		CHECK(vcg_relink(&r, s0, NULL) == VCG_OK);
		CHECK_EQ(log_len(&r, &g, &rc), 8);
		CHECK(rc == VCG_OK);
	}
	{
		uint64_t	n;

		CHECK(r.graph->ops->count(r.graph, &n) == PROLLY_OK);
		CHECK_EQ(n, 8);
	}
	check_store(cs, 0);
	vcg_repo_free(&r);
	cs_destroy(cs);
}

/*
 * A merge commit with one parent cut early and the other cut later: it
 * keeps a seam for each.  The older segment cannot go back while the
 * other parent is still cut, even though the merge commit itself is held.
 */
static void
test_merge_commit_cut_twice(void)
{
	ChunkStore *cs = memstore_create();
	VcgRepo		r;
	PHash		init,
				a,
				m;
	uint32_t	s0,
				s1;
	size_t		nconf;
	CgEdge		e;

	vcg_repo_init(&r, cs, &small_policy);
	init = vcg_head(&r)->head;
	CHECK(vcg_branch_create(&r, "side", &init) == VCG_OK);
	CHECK(vcg_checkout(&r, "side") == VCG_OK);
	commit_one(&r, 100, 1, "side");	/* gen 2 */
	CHECK(vcg_checkout(&r, "main") == VCG_OK);
	a = commit_one(&r, 1, 1, "a");	/* gen 2 */
	for (int i = 2; i <= 6; i++)
		commit_one(&r, (uint64_t) i, (uint64_t) i, "main");	/* gens 3..7 */
	CHECK(vcg_merge(&r, "side", &nconf) == VCG_OK);
	CHECK_EQ(nconf, 0);
	m = vcg_head(&r)->head;	/* gen 8, parents main@7 and side@2 */
	CHECK(vcg_edge(&r, &m, &e) == PROLLY_OK);
	CHECK_EQ(e.nparents, 2);

	/* side's branch goes; a branch at a keeps main's line whole */
	CHECK(vcg_branch_delete(&r, "main") == VCG_ERR);
	CHECK(vcg_branch_delete(&r, "side") == VCG_OK);
	CHECK(vcg_branch_find(&r, "side") == NULL);
	CHECK(vcg_branch_create(&r, "keep", &a) == VCG_OK);
	CHECK(vcg_sever(&r, 6, &s0) == VCG_OK);	/* cuts side@2 alone */
	CHECK(vcg_edge(&r, &m, &e) == PROLLY_OK);
	CHECK(e.nparents == 1 && e.nseams == 1);

	CHECK(vcg_branch_delete(&r, "keep") == VCG_OK);
	commit_one(&r, 9, 9, "n");
	commit_one(&r, 10, 10, "o");
	CHECK(vcg_sever(&r, 3, &s1) == VCG_OK);	/* keeps 8..10: main's line is cut */
	CHECK(vcg_edge(&r, &m, &e) == PROLLY_OK);
	CHECK(e.nparents == 0 && e.nseams == 2);
	CHECK(vcg_relink(&r, s0, NULL) == VCG_ERR_ORDER);
	CHECK(vcg_edge(&r, &m, &e) == PROLLY_OK);
	CHECK(e.nparents == 0 && e.nseams == 2);
	CHECK(vcg_relink(&r, s1, NULL) == VCG_OK);
	CHECK(vcg_edge(&r, &m, &e) == PROLLY_OK);
	CHECK(e.nparents == 1 && e.nseams == 1);
	CHECK(vcg_relink(&r, s0, NULL) == VCG_OK);
	CHECK(vcg_edge(&r, &m, &e) == PROLLY_OK);
	CHECK(e.nparents == 2 && e.nseams == 0);
	check_store(cs, 0);
	vcg_repo_free(&r);
	cs_destroy(cs);
}

/* A merge base inside the window is found; one past the cut is reported. */
static void
test_merge_base_and_the_cut(void)
{
	ChunkStore *cs = memstore_create();
	VcgRepo		r;
	PHash		h[6],
				f,
				base;
	uint32_t	seg;

	vcg_repo_init(&r, cs, &small_policy);
	build_chain(&r, h);
	CHECK(vcg_branch_create(&r, "feature", &h[2]) == VCG_OK);
	CHECK(vcg_checkout(&r, "feature") == VCG_OK);
	f = commit_one(&r, 50, 1, "f");
	CHECK(vcg_merge_base(&r, &h[5], &f, &base) == 1);
	CHECK(phash_eq(&base, &h[2]));
	/* main's window reaches c, feature's keeps b: the base survives a sever */
	CHECK(vcg_sever(&r, 3, &seg) == VCG_OK);
	CHECK(seg != VCG_NO_SEGMENT);
	CHECK(vcg_merge_base(&r, &h[5], &f, &base) == 1);
	CHECK(phash_eq(&base, &h[2]));
	/* a window of one keeps only heads: the base is past the cut */
	CHECK(vcg_sever(&r, 1, &seg) == VCG_OK);
	CHECK(vcg_merge_base(&r, &h[5], &f, &base) == VCG_ERR_SEVERED);
	CHECK(vcg_relink(&r, seg, NULL) == VCG_OK);
	CHECK(vcg_merge_base(&r, &h[5], &f, &base) == 1);
	CHECK(phash_eq(&base, &h[2]));
	vcg_repo_free(&r);
	cs_destroy(cs);
}

/* Merging: fast-forward, a two-parent commit, and conflicts committing nothing. */
static void
test_merge(void)
{
	ChunkStore *cs = memstore_create();
	VcgRepo		r;
	PHash		before;
	size_t		nconf;
	CgEdge		e;

	vcg_repo_init(&r, cs, &small_policy);
	commit_one(&r, 1, 1, "a");
	CHECK(vcg_branch_create(&r, "x", &vcg_head(&r)->head) == VCG_OK);
	CHECK(vcg_checkout(&r, "x") == VCG_OK);
	commit_one(&r, 2, 1, "x1");
	CHECK(vcg_checkout(&r, "main") == VCG_OK);
	CHECK(vcg_merge(&r, "x", &nconf) == VCG_OK);
	CHECK(phash_eq(&vcg_head(&r)->head, &vcg_branch_find(&r, "x")->head));
	commit_one(&r, 3, 1, "m1");
	CHECK(vcg_checkout(&r, "x") == VCG_OK);
	commit_one(&r, 4, 1, "x2");
	CHECK(vcg_checkout(&r, "main") == VCG_OK);
	CHECK(vcg_merge(&r, "x", &nconf) == VCG_OK && nconf == 0);
	CHECK(vcg_edge(&r, &vcg_head(&r)->head, &e) == PROLLY_OK);
	CHECK_EQ(e.nparents, 2);
	/* both sides set row 5 differently */
	commit_one(&r, 5, 1, "m5");
	CHECK(vcg_checkout(&r, "x") == VCG_OK);
	commit_one(&r, 5, 2, "x5");
	CHECK(vcg_checkout(&r, "main") == VCG_OK);
	before = vcg_head(&r)->head;
	CHECK(vcg_merge(&r, "x", &nconf) == VCG_OK);
	CHECK(nconf > 0);
	CHECK(phash_eq(&vcg_head(&r)->head, &before));
	check_store(cs, 0);
	vcg_repo_free(&r);
	cs_destroy(cs);
}

/*
 * Two repos that cut at different depths still sync: commits are the same
 * hashes everywhere, and an edge one side cut agrees with the other's.
 */
static void
test_sync_across_different_cuts(void)
{
	ChunkStore *ca = memstore_create(),
			   *cb = memstore_create();
	VcgRepo		a,
				b;
	PHash		h[6],
				head;
	uint32_t	seg;
	int			rc;

	vcg_repo_init(&a, ca, &small_policy);
	build_chain(&a, h);
	vcg_repo_init_empty(&b, cb, &small_policy);
	CHECK(vcg_fetch(&b, &a, "main", "main", &head) == VCG_OK);
	CHECK_EQ(log_len(&b, &head, &rc), 6);
	CHECK(same_graphs(&a, &b));

	/* a cuts at 2, b keeps everything; a commits, b fetches */
	CHECK(vcg_sever(&a, 2, &seg) == VCG_OK);
	CHECK(vcg_segment_discard(&a, seg) == VCG_OK);
	commit_one(&a, 7, 7, "f");
	{
		uint64_t	before = cb->stats.puts;

		CHECK(vcg_fetch(&b, &a, "main", "main", &head) == VCG_OK);
		CHECK_EQ(log_len(&b, &head, &rc), 7);
		CHECK(rc == VCG_OK);
		/* only the new commit's chunks and the graph's changed path came */
		CHECK_LE(cb->stats.puts - before, 40);
	}
	/* b commits, a fetches: the walk stops at d, which a cut nothing from */
	commit_one(&b, 8, 8, "g");
	CHECK(vcg_fetch(&a, &b, "main", "main", &head) == VCG_OK);
	CHECK_EQ(log_len(&a, &head, &rc), 4);	/* g f e d: d is a's root */
	CHECK(!same_graphs(&a, &b));
	/* nothing to send back */
	CHECK(vcg_push(&a, &b, "main") == VCG_OK);
	/* d is a root in a, d <- c in b: those agree, and nothing is sent */
	CHECK(vcg_branch_create(&a, "old", &h[4]) == VCG_OK);
	{
		CgEdge		e;

		CHECK(vcg_edge(&a, &h[4], &e) == PROLLY_OK);
		CHECK(e.nparents == 0 && e.nseams == 1);
	}
	CHECK(vcg_fetch(&b, &a, "old", "old", &head) == VCG_OK);
	CHECK_EQ(log_len(&b, &head, &rc), 5);
	check_store(ca, 0);
	check_store(cb, 0);
	vcg_repo_free(&a);
	vcg_repo_free(&b);
	cs_destroy(ca);
	cs_destroy(cb);
}

/* Push must fast-forward. */
static void
test_push_fast_forward_only(void)
{
	ChunkStore *ca = memstore_create(),
			   *cb = memstore_create();
	VcgRepo		a,
				b;
	PHash		head;

	vcg_repo_init(&a, ca, &small_policy);
	commit_one(&a, 1, 1, "a");
	vcg_repo_init_empty(&b, cb, &small_policy);
	CHECK(vcg_fetch(&b, &a, "main", "main", &head) == VCG_OK);
	commit_one(&a, 2, 1, "a2");
	CHECK(vcg_push(&a, &b, "main") == VCG_OK);
	CHECK(phash_eq(&vcg_head(&b)->head, &vcg_head(&a)->head));
	commit_one(&b, 3, 1, "b3");
	commit_one(&a, 4, 1, "a4");
	CHECK(vcg_push(&a, &b, "main") == VCG_ERR_NONFF);
	vcg_repo_free(&a);
	vcg_repo_free(&b);
	cs_destroy(ca);
	cs_destroy(cb);
}

/*
 * A sender whose graph lies: an edge that contradicts one the receiver
 * holds, an edge whose generations do not fit, a head its graph does not
 * cover.  Each is refused and the receiver is left as it was.
 */
static void
test_fetch_refuses_bad_edges(void)
{
	ChunkStore *ca = memstore_create(),
			   *cb = memstore_create();
	VcgRepo		a,
				b;
	PHash		h[6],
				head,
				broot,
				x;
	CgEdge		e;
	CgPut		p;

	vcg_repo_init(&a, ca, &small_policy);
	build_chain(&a, h);
	vcg_repo_init_empty(&b, cb, &small_policy);
	CHECK(vcg_fetch(&b, &a, "main", "main", &head) == VCG_OK);
	broot = graph_root(&b);

	/* a now claims d's parent is x, a sibling of c of the same generation */
	CHECK(vcg_branch_create(&a, "side", &h[2]) == VCG_OK);
	CHECK(vcg_checkout(&a, "side") == VCG_OK);
	x = commit_one(&a, 99, 1, "x");
	CHECK(vcg_checkout(&a, "main") == VCG_OK);
	CHECK(vcg_edge(&a, &h[4], &e) == PROLLY_OK);
	e.parents[0] = x;
	p = (CgPut) {5, h[4], &e};
	CHECK(a.graph->ops->apply(a.graph, &p, 1) == PROLLY_OK);
	prolly_release_pins(&a.ctx);
	commit_one(&a, 7, 7, "f");
	/*
	 * The walk from f stops at e, which b knows, so d's edge is never
	 * read: a lie below a commit both know cannot change b's history.
	 */
	CHECK(vcg_fetch(&b, &a, "main", "main", &head) == VCG_OK);
	{
		CHECK(vcg_edge(&b, &h[4], &e) == PROLLY_OK);
		CHECK(phash_eq(&e.parents[0], &h[3]));
	}
	/* a commit both know, whose edge a changed: refused */
	CHECK(vcg_edge(&a, &h[5], &e) == PROLLY_OK);
	e.parents[0] = x;
	e.parent_gen[0] = 4;
	p = (CgPut) {6, h[5], &e};
	CHECK(a.graph->ops->apply(a.graph, &p, 1) == PROLLY_OK);
	prolly_release_pins(&a.ctx);
	broot = graph_root(&b);
	{
		VcgBranch  *br = vcg_branch_find(&a, "main");
		PHash		keep = br->head;

		br->head = h[5];
		CHECK(vcg_fetch(&b, &a, "main", "main", &head) == VCG_ERR_CONFLICT);
		br->head = keep;
	}
	CHECK(same_graph(&b, &broot));

	/* a new commit whose edge skips a generation: refused */
	{
		PHash		g = commit_one(&a, 8, 8, "g");

		CHECK(vcg_edge(&a, &g, &e) == PROLLY_OK);
		e.parent_gen[0] = 3;
		p = (CgPut) {8, g, &e};
		CHECK(a.graph->ops->apply(a.graph, &p, 1) == PROLLY_OK);
		prolly_release_pins(&a.ctx);
		CHECK(vcg_fetch(&b, &a, "main", "main", &head) == VCG_ERR_FORGED);
		CHECK(same_graph(&b, &broot));

		/* and a head the sender's graph does not cover at all */
		p = (CgPut) {8, g, NULL};
		CHECK(a.graph->ops->apply(a.graph, &p, 1) == PROLLY_OK);
		prolly_release_pins(&a.ctx);
		CHECK(vcg_fetch(&b, &a, "main", "main", &head) == VCG_ERR_FORGED);
		CHECK(same_graph(&b, &broot));
	}
	cs_collect_orphans(cb);
	check_store(cb, 0);
	vcg_repo_free(&a);
	vcg_repo_free(&b);
	cs_destroy(ca);
	cs_destroy(cb);
}

/*
 * Fetching from a repo whose history was cut before it reaches anything
 * the receiver knows: detached, not unrelated.  From an unrelated repo:
 * unrelated.
 */
static void
test_fetch_detached_and_unrelated(void)
{
	ChunkStore *ca = memstore_create(),
			   *cb = memstore_create(),
			   *cc = memstore_create();
	VcgRepo		a,
				b,
				c;
	PHash		head,
				broot;
	uint32_t	seg;

	vcg_repo_init(&a, ca, &small_policy);
	commit_one(&a, 1, 1, "a");
	vcg_repo_init_empty(&b, cb, &small_policy);
	CHECK(vcg_fetch(&b, &a, "main", "main", &head) == VCG_OK);
	for (int i = 2; i < 8; i++)
		commit_one(&a, (uint64_t) i, 1, "more");
	CHECK(vcg_sever(&a, 3, &seg) == VCG_OK);
	CHECK(vcg_segment_discard(&a, seg) == VCG_OK);
	broot = graph_root(&b);
	CHECK(vcg_fetch(&b, &a, "main", "main", &head) == VCG_ERR_DETACHED);
	CHECK(same_graph(&b, &broot));

	vcg_repo_init(&c, cc, &small_policy);
	commit_one(&c, 1, 1, "c");
	CHECK(vcg_fetch(&b, &c, "main", "main", &head) == VCG_ERR_UNRELATED);
	CHECK(same_graph(&b, &broot));
	cs_collect_orphans(cb);
	check_store(cb, 0);
	vcg_repo_free(&a);
	vcg_repo_free(&b);
	vcg_repo_free(&c);
	cs_destroy(ca);
	cs_destroy(cb);
	cs_destroy(cc);
}

/*
 * A clone of a cut repo starts at the same root, knows the cut by its
 * hash, and can relink it from the archive the original exported to.
 */
static void
test_clone_carries_the_cut(void)
{
	ChunkStore *ca = memstore_create(),
			   *cb = memstore_create(),
			   *archive = memstore_create();
	VcgRepo		a,
				b;
	PHash		h[6],
				head,
				H;
	uint32_t	seg;
	int			rc;

	vcg_repo_init(&a, ca, &small_policy);
	build_chain(&a, h);
	CHECK(vcg_sever(&a, 3, &seg) == VCG_OK);
	H = a.segs[seg].hash;
	CHECK(vcg_segment_export(&a, seg, archive) == VCG_OK);
	vcg_repo_init_empty(&b, cb, &small_policy);
	CHECK(vcg_fetch(&b, &a, "main", "main", &head) == VCG_OK);
	CHECK_EQ(log_len(&b, &head, &rc), 3);
	CHECK(is_root_with_seam(&b, &h[3], &H));
	CHECK_EQ(b.nsegs, 0);
	seg = vcg_segment_find(&b, &H);
	CHECK(b.segs[seg].state == VCG_SEG_DISCARDED);
	CHECK(vcg_segment_find(&b, &H) == seg);
	CHECK(vcg_relink(&b, seg, archive) == VCG_OK);
	CHECK_EQ(log_len(&b, &head, &rc), 6);
	check_store(cb, 0);
	vcg_repo_free(&a);
	vcg_repo_free(&b);
	cs_destroy(ca);
	cs_destroy(cb);
	cs_destroy(archive);
}

/* Saved and reopened, a cut repo reads and relinks the same. */
static void
test_save_and_reopen(void)
{
	ChunkStore *cs = memstore_create();
	VcgRepo		r;
	PHash		h[6],
				state,
				groot;
	uint32_t	seg;
	int			rc;

	vcg_repo_init(&r, cs, &small_policy);
	build_chain(&r, h);
	CHECK(vcg_branch_create(&r, "other", &h[4]) == VCG_OK);
	CHECK(vcg_sever(&r, 3, &seg) == VCG_OK);
	state = vcg_repo_save(&r);
	groot = graph_root(&r);
	vcg_repo_free(&r);

	CHECK(vcg_repo_open(&r, cs, &small_policy, &state) == VCG_OK);
	CHECK(same_graph(&r, &groot));
	CHECK_EQ(r.nbr, 2);
	CHECK(phash_eq(&vcg_branch_find(&r, "other")->head, &h[4]));
	CHECK_EQ(log_len(&r, &h[5], &rc), 4);	/* other's window keeps b */
	CHECK(r.segs[seg].state == VCG_SEG_LOCAL);
	CHECK(vcg_relink(&r, seg, NULL) == VCG_OK);
	CHECK_EQ(log_len(&r, &h[5], &rc), 6);
	state = vcg_repo_save(&r);
	check_store(cs, 0);
	vcg_repo_free(&r);
	cs_destroy(cs);
}

/*
 * The saved repo is the size of the window: commits cut and discarded
 * leave nothing in it, and a discarded segment is found again by the hash
 * its kept commit records, after a reopen too.
 */
static void
test_state_does_not_grow_with_history(void)
{
	ChunkStore *cs = memstore_create(),
			   *archive = memstore_create();
	VcgRepo		r;
	PHash		state,
				first_cut,
				head;
	size_t		len10 = 0,
				len;
	uint32_t	seg;
	int			rc;

	vcg_repo_init(&r, cs, &small_policy);
	for (int i = 1; i <= 200; i++)
	{
		commit_one(&r, (uint64_t) i, (uint64_t) i, "step");
		CHECK(vcg_sever(&r, 5, &seg) == VCG_OK);
		if (seg == VCG_NO_SEGMENT)
			continue;
		if (r.nsegs == 1)
		{
			/* the first cut goes to an archive, then is let go here too */
			first_cut = r.segs[seg].hash;
			CHECK(vcg_segment_export(&r, seg, archive) == VCG_OK);
		}
		CHECK(vcg_segment_discard(&r, seg) == VCG_OK);
		state = vcg_repo_save(&r);
		CHECK(cs_get(cs, &state, &len) != NULL);
		if (i == 10)
			len10 = len;
	}
	CHECK(len10 > 0);
	/* a varint or two per commit held grows a byte as generations pass 127 */
	CHECK_LE(len, len10 + 16);
	head = vcg_head(&r)->head;
	CHECK_EQ(log_len(&r, &head, &rc), 5);
	vcg_repo_free(&r);

	/*
	 * Reopened, no segment is listed.  The first cut is found by its hash
	 * once the cuts after it are back, newest first, from the store's own
	 * history...  which is gone: the later cuts were discarded, so only
	 * the archive's first cut could come back, and it waits on them.
	 */
	CHECK(vcg_repo_open(&r, cs, &small_policy, &state) == VCG_OK);
	CHECK_EQ(r.nsegs, 0);
	seg = vcg_segment_find(&r, &first_cut);
	CHECK(vcg_relink(&r, seg, archive) == VCG_ERR_ORDER);
	{
		CgEdge		e;
		uint32_t	newest;

		CHECK(vcg_edge(&r, &r.segs[seg].hash, &e) == PROLLY_NOT_FOUND);
		/* the newest cut is named by the oldest commit kept */
		{
			LogAcc		a = {0};

			CHECK(vcg_log(&r, &head, 100, log_fn, &a) == VCG_OK);
			CHECK(vcg_edge(&r, &a.h[a.n - 1], &e) == PROLLY_OK);
		}
		CHECK(e.nparents == 0 && e.nseams == 1);
		newest = vcg_segment_find(&r, &e.seams[0]);
		CHECK(newest != seg);
		CHECK(vcg_relink(&r, newest, NULL) == VCG_ERR_SEVERED);
	}
	cs_collect_orphans(cs);
	check_store(cs, 0);
	vcg_repo_free(&r);
	cs_destroy(cs);
	cs_destroy(archive);
}

int
main(void)
{
	RUN(test_nonce_makes_commits_unique);
	RUN(test_sever_makes_a_root);
	RUN(test_relink_local_restores_graph);
	RUN(test_export_and_relink_from_archive);
	RUN(test_relink_refuses_forgeries);
	RUN(test_relink_newest_first);
	RUN(test_merge_commit_cut_twice);
	RUN(test_merge_base_and_the_cut);
	RUN(test_merge);
	RUN(test_sync_across_different_cuts);
	RUN(test_push_fast_forward_only);
	RUN(test_fetch_refuses_bad_edges);
	RUN(test_fetch_detached_and_unrelated);
	RUN(test_clone_carries_the_cut);
	RUN(test_save_and_reopen);
	RUN(test_state_does_not_grow_with_history);
	printf("test_vcg: all tests passed\n");
	return 0;
}
