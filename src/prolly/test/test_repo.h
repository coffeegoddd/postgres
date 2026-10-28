/*
 * test_repo.h
 *	  Repo-level helpers shared by the window and remote tests: a table of
 *	  versioned rows, snapshots, logs and expected frees.
 */
#ifndef TEST_REPO_H
#define TEST_REPO_H

#include "test_util.h"

#define KEYS 40000

static const char *ddl = "CREATE TABLE t (id int8 PRIMARY KEY, v int8)";

static inline void
put_rows(VcRepo *r, uint64_t lo, uint64_t hi, uint64_t ver)
{
	EditBatch	b;
	PHash		rv = vc_working(r),
				out;

	batch_init(&b, hi - lo);
	for (uint64_t k = lo; k < hi; k++)
		batch_put(&b, k, ver);
	CHECK(vc_table_edit(&r->ctx, &rv, "t", b.e, b.n, &out) == 0);
	vc_set_working(r, &out);
	batch_free(&b);
}

static inline PHash
commit_rows(VcRepo *r, uint64_t lo, uint64_t hi, uint64_t ver, const char *msg)
{
	put_rows(r, lo, hi, ver);
	return vc_commit(r, msg);
}

static inline void
setup(VcRepo *r, ChunkStore *cs)
{
	PHash		rv,
				out;

	vc_repo_init(r, cs, &small_policy);
	rv = vc_working(r);
	CHECK(vc_table_create(&r->ctx, &rv, "t", ddl, strlen(ddl), &out) == 0);
	vc_set_working(r, &out);
}

/* Rows of table t at a commit, read from ctx's store alone. */
static inline void
rows_at(ProllyCtx *ctx, const PHash *commit, int64_t *ver)
{
	VcCommit	c;
	VcTable		t;
	ProllyIter *it;
	const uint8_t *k,
			   *v;
	size_t		kl,
				vl;

	CHECK(vc_commit_load(ctx, commit, &c));
	CHECK(vc_table_get(ctx, &c.root, "t", &t));
	for (uint64_t i = 0; i < KEYS; i++)
		ver[i] = -1;
	it = t_iter(ctx, &t.data, NULL, 0);
	while (t_next(it, &k, &kl, &v, &vl))
		ver[get_u64(k)] = (int64_t) get_u64(v + 8);
	prolly_iter_close(it);
}

typedef struct Snap
{
	int64_t    *v;
} Snap;

static inline Snap
snap(ProllyCtx *ctx, const PHash *commit)
{
	Snap		s = {px_malloc(KEYS * sizeof(int64_t))};

	rows_at(ctx, commit, s.v);
	return s;
}

static inline void
check_snap(ProllyCtx *ctx, const PHash *commit, const Snap *s)
{
	int64_t    *got = px_malloc(KEYS * sizeof(int64_t));

	rows_at(ctx, commit, got);
	CHECK(memcmp(got, s->v, KEYS * sizeof(int64_t)) == 0);
	free(got);
}

typedef struct LogAcc
{
	PHash		v[64];
	int			n;
} LogAcc;

static inline void
log_fn(void *arg, const VcCommit *c)
{
	LogAcc	   *a = arg;

	CHECK(a->n < 64);
	a->v[a->n++] = c->hash;
}

static inline int
log_from(VcRepo *r, const PHash *start, unsigned flags, LogAcc *acc)
{
	acc->n = 0;
	return vc_log(r, start, 64, flags, log_fn, acc);
}

static inline void
count_tables_fn(void *arg, const VcTableChange *c)
{
	(*(int *) arg)++;
}

/* Every chunk some hold other than segment `skip` keeps alive. */
static inline void
held_elsewhere(VcRepo *r, uint32_t skip, PHashMap *out)
{
	phmap_init(out);
	for (size_t i = 0; i < r->loc.cap; i++)
		if (r->loc.used[i])
		{
			uint32_t	seg = VC_NO_SEGMENT;
			VcLocation	l = vc_locate(r, &r->loc.keys[i], &seg);

			if (l == VC_LOC_WINDOW || (l == VC_LOC_SEGMENT && seg != skip))
				closure(r->ctx.cs, &r->loc.keys[i], out);
		}
	for (size_t i = 0; i < r->nbr; i++)
		closure(r->ctx.cs, &r->br[i].working, out);
}

/* The chunks dropping segment `seg` must free: its closure minus all else held. */
static inline size_t
expected_frees(VcRepo *r, uint32_t seg)
{
	PHashMap	mine,
				other,
				only;
	size_t		n;

	phmap_init(&mine);
	for (size_t i = 0; i < r->segs[seg].n; i++)
		closure(r->ctx.cs, &r->segs[seg].commits[i], &mine);
	held_elsewhere(r, seg, &other);
	set_minus(&mine, &other, &only);
	n = set_size(&only);
	phmap_free(&mine);
	phmap_free(&other);
	phmap_free(&only);
	return n;
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

static inline Linear
build_linear(VcRepo *r)
{
	Linear		l;

	l.init = vc_head(r)->head;
	l.a = commit_rows(r, 0, 20000, 0, "A");
	l.b = commit_rows(r, 0, 300, 1, "B");
	l.c = commit_rows(r, 10000, 10300, 2, "C");
	l.d = commit_rows(r, 19000, 19300, 3, "D");
	return l;
}

static const VcWindow depth2 = {2, NULL, 0};

#endif							/* TEST_REPO_H */
