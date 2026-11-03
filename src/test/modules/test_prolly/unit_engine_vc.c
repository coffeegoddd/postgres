/*-------------------------------------------------------------------------
 *
 * unit_engine_vc.c
 *	  Unit tests of version control over prolly trees, on a repository's
 *	  references: commits, branches, checkout, reset, log, diff, merge
 *	  (rows and DDL), merge base, and the chunk costs of each operation.
 *
 * Most run on both stores: on "memory" with the memory references, on
 * "pages" with the history store's own rows (pg_history.pointers), both
 * rolled back with the test's subtransaction there.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/test/modules/test_prolly/unit_engine_vc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "engine_test_util.h"

static const char *schema_v1 = "CREATE TABLE t (id int8 PRIMARY KEY, v int8)";
static const char *schema_v2 = "CREATE TABLE t (id int8 PRIMARY KEY, v int8, w text)";
static const char *schema_v3 = "CREATE TABLE t (id int8 PRIMARY KEY, v numeric)";

static void
repo_open(VcRepo *r, ChunkStore *cs, const ChunkPolicy *policy)
{
	vc_repo_open(r, cs, t_refs(cs), policy);
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
head_root_of(VcRepo *r)
{
	VcBranch	b;
	PHash		h;

	vc_head(r, &b);
	h = b.head_root;
	vc_branch_free(&b);
	return h;
}

static void
create_table(VcRepo *r, const char *name, const char *schema)
{
	PHash		rv = vc_working(r),
				out;

	CHECK_EQ(vc_table_create(&r->ctx, &rv, name, schema, strlen(schema), &out), VC_OK);
	vc_set_working(r, &out);
}

static void
alter_table(VcRepo *r, const char *name, const char *schema)
{
	PHash		rv = vc_working(r),
				out;

	CHECK_EQ(vc_table_alter(&r->ctx, &rv, name, schema, strlen(schema), &out), VC_OK);
	vc_set_working(r, &out);
}

static void
drop_table(VcRepo *r, const char *name)
{
	PHash		rv = vc_working(r),
				out;

	CHECK_EQ(vc_table_drop(&r->ctx, &rv, name, &out), VC_OK);
	vc_set_working(r, &out);
}

static void
apply_batch(VcRepo *r, const char *table, const EditBatch *b)
{
	PHash		rv = vc_working(r),
				out;

	CHECK_EQ(vc_table_edit(&r->ctx, &rv, table, b->e, b->n, &out), VC_OK);
	vc_set_working(r, &out);
}

static void
put_row(VcRepo *r, const char *table, uint64 key, uint64 version)
{
	EditBatch	b;

	batch_init(&b, 1);
	batch_put(&b, key, version);
	apply_batch(r, table, &b);
	batch_free(&b);
}

static void
del_row(VcRepo *r, const char *table, uint64 key)
{
	EditBatch	b;

	batch_init(&b, 1);
	batch_del(&b, key);
	apply_batch(r, table, &b);
	batch_free(&b);
}

/* Row version in a root value, or -1 if absent. */
static int64
row_version(VcRepo *r, const PHash *rv, const char *table, uint64 key)
{
	VcTable		t;
	uint8		k[8];
	const uint8 *v;
	size_t		vl;
	int64		ver;

	CHECK(vc_table_get(&r->ctx, rv, table, &t));
	put_u64(k, key);
	if (!t_get(&r->ctx, &t.data, k, 8, &v, &vl))
		return -1;
	ver = (int64) get_u64(v + 8);
	px_free((void *) v);
	return ver;
}

static int64
working_row(VcRepo *r, const char *table, uint64 key)
{
	PHash		rv = vc_working(r);

	return row_version(r, &rv, table, key);
}

static bool
schema_is(VcRepo *r, const PHash *rv, const char *table, const char *schema)
{
	VcTable		t;
	size_t		len;
	uint8	   *s;
	bool		is;

	if (!vc_table_get(&r->ctx, rv, table, &t))
		return false;
	s = vc_schema_get(&r->ctx, &t.schema, &len);
	is = s && bytes_eq(s, len, (const uint8 *) schema, strlen(schema));
	px_free(s);
	return is;
}

static PHash
commit_root(VcRepo *r, const PHash *commit)
{
	VcCommit	c;
	PHash		root;

	CHECK(vc_commit_load(&r->ctx, commit, &c));
	root = c.root;
	vc_commit_release(&c);
	return root;
}

static void
table_change_fn_count(void *arg, const VcTableChange *c)
{
	(*(int *) arg)++;
}

static void
count_branch_fn(void *arg, const VcBranch *b)
{
	(*(int *) arg)++;
}

typedef struct LogAcc
{
	PHash		v[8];
	int			n;
} LogAcc;

static void
log_fn(void *arg, const VcCommit *c)
{
	LogAcc	   *a = arg;

	CHECK(a->n < 8);
	a->v[a->n++] = c->hash;
}

static int
log_from(VcRepo *r, const PHash *start, size_t max, unsigned flags, LogAcc *acc)
{
	acc->n = 0;
	return vc_log(r, start, max, flags, log_fn, acc);
}

/* ------------------------------------------------------------------ tests */

typedef struct RefList
{
	int			n;
	char		names[4][32];
	PHash		hashes[4];
} RefList;

static void
ref_list_fn(void *arg, const char *name, const PHash *hash)
{
	RefList    *l = arg;

	CHECK(l->n < 4);
	strlcpy(l->names[l->n], name, sizeof(l->names[0]));
	l->hashes[l->n++] = *hash;
}

/*
 * The references' contract, on each implementation: a name read back as
 * set, moved, added only once, listed by prefix in order, removed; each
 * holding the chunk it names.
 */
ENGINE_TEST(test_refs_contract)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &small_policy);
	VcRefs	   *refs = t_refs(cs);
	PHash		a = build_even_rows(&ctx, 10),
				b = build_even_rows(&ctx, 20),
				got;
	RefList		l = {0};
	ChunkCounts counts;

	CHECK(!vc_refs_get(refs, "test/x", VC_REF_READ, &got));
	/* a name too long for any reference is none, on either implementation */
	{
		char		longname[300];

		memset(longname, 'n', sizeof(longname) - 1);
		longname[sizeof(longname) - 1] = '\0';
		CHECK(!vc_refs_get(refs, longname, VC_REF_READ, &got));
		CHECK(!vc_refs_remove(refs, longname));
	}
	CHECK(vc_refs_add(refs, "test/x", &a));
	CHECK(!vc_refs_add(refs, "test/x", &b));
	CHECK(vc_refs_get(refs, "test/x", VC_REF_UPDATE, &got) && phash_eq(&got, &a));
	vc_refs_set(refs, "test/x", &b);
	CHECK(vc_refs_get(refs, "test/x", VC_REF_READ, &got) && phash_eq(&got, &b));
	vc_refs_set(refs, "test/y", &a);
	vc_refs_set(refs, "tesu", &a);	/* (just past the prefix) */
	CHECK(cs_counts(cs, a.b, &counts));
	CHECK_EQ(counts.roots, 2);
	CHECK(cs_counts(cs, b.b, &counts));
	CHECK_EQ(counts.roots, 1);

	vc_refs_list(refs, "test/", ref_list_fn, &l);
	CHECK_EQ(l.n, 2);
	CHECK(strcmp(l.names[0], "test/x") == 0 && phash_eq(&l.hashes[0], &b));
	CHECK(strcmp(l.names[1], "test/y") == 0 && phash_eq(&l.hashes[1], &a));

	CHECK(vc_refs_remove(refs, "test/x"));
	CHECK(!vc_refs_remove(refs, "test/x"));
	CHECK(!vc_refs_get(refs, "test/x", VC_REF_READ, &got));
	CHECK(cs_counts(cs, b.b, &counts));
	CHECK_EQ(counts.roots, 0);
	CHECK(vc_refs_remove(refs, "test/y"));
	CHECK(vc_refs_remove(refs, "tesu"));
	cs_destroy(cs);
}

ENGINE_TEST(test_commit_and_log)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	PHash		c[4];
	VcCommit	cm;
	PHash		h,
				r1,
				r2;
	int			n = 0;

	repo_open(&r, cs, &small_policy);
	c[0] = head_of(&r);
	create_table(&r, "t", schema_v1);
	c[1] = vc_commit(&r, "create t");
	put_row(&r, "t", 1, 1);
	c[2] = vc_commit(&r, "row 1");
	put_row(&r, "t", 2, 1);
	c[3] = vc_commit(&r, "row 2");

	/* log walks first parents newest first */
	h = head_of(&r);
	for (;;)
	{
		bool		root;

		CHECK(vc_commit_load(&r.ctx, &h, &cm));
		CHECK(phash_eq(&h, &c[3 - n]));
		CHECK_EQ(cm.generation, (uint64) (4 - n));
		CHECK(vc_in_window(&r, &h));
		n++;
		root = cm.nparents == 0;
		if (!root)
			h = cm.parents[0];
		vc_commit_release(&cm);
		if (root)
			break;
	}
	CHECK_EQ(n, 4);

	/* vc_log: the same, newest first, as far as max, the root last */
	{
		LogAcc		acc;

		CHECK_EQ(log_from(&r, &c[3], 8, VC_WINDOW_ONLY, &acc), VC_OK);
		CHECK_EQ(acc.n, 4);
		for (int i = 0; i < 4; i++)
			CHECK(phash_eq(&acc.v[i], &c[3 - i]));
		CHECK_EQ(log_from(&r, &c[3], 2, 0, &acc), VC_OK);
		CHECK(acc.n == 2 && phash_eq(&acc.v[1], &c[2]));
	}
	CHECK(vc_commit_load(&r.ctx, &c[2], &cm));
	CHECK(cm.msglen == 5 && memcmp(cm.msg, "row 1", 5) == 0);
	vc_commit_release(&cm);

	/* a commit's root value holds the schema and rows as of that commit */
	r1 = commit_root(&r, &c[1]);
	r2 = commit_root(&r, &c[2]);
	CHECK(schema_is(&r, &r1, "t", schema_v1));
	CHECK_EQ(row_version(&r, &r1, "t", 1), -1);
	CHECK_EQ(row_version(&r, &r2, "t", 1), 1);

	/* a second view of the repository sees what the first wrote */
	{
		VcRepo		again;

		repo_open(&again, cs, &small_policy);
		h = head_of(&again);
		CHECK(phash_eq(&h, &c[3]));
		vc_repo_free(&again);
	}
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/*
 * Every intermediate working set goes when the next replaces it: the store
 * holds only what commits and working sets reach.
 */
ENGINE_MEMORY_TEST(test_working_set_churn_frees)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	uint64		after_commit;

	repo_open(&r, cs, &small_policy);
	create_table(&r, "t", schema_v1);
	for (uint64 k = 0; k < 3000; k++)
		put_row(&r, "t", k, 0);
	vc_commit(&r, "load");
	settle_and_check(cs);
	after_commit = store_count(cs);
	/* rewriting the same rows over and over must not grow the store */
	for (int round = 0; round < 5; round++)
		for (uint64 k = 0; k < 3000; k += 7)
			put_row(&r, "t", k, (uint64) round + 1);
	for (uint64 k = 0; k < 3000; k += 7)
		put_row(&r, "t", k, 0);
	settle_and_check(cs);
	CHECK_EQ(store_count(cs), after_commit);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* Committing an earlier root leaves the working root's later changes uncommitted. */
ENGINE_TEST(test_commit_root_keeps_later_changes)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	PHash		as_of,
				c,
				w,
				hr;
	VcCommit	cm;
	VcMergeResult m;
	int			changed = 0;

	repo_open(&r, cs, &small_policy);
	create_table(&r, "t", schema_v1);
	put_row(&r, "t", 1, 1);
	vc_commit(&r, "one");
	put_row(&r, "t", 2, 1);
	/* (held until the transaction ends, as everything it writes is) */
	as_of = vc_working(&r);
	put_row(&r, "t", 3, 1);		/* lands after the barrier */
	c = vc_commit_root(&r, &as_of, "as of the barrier");

	CHECK(vc_commit_load(&r.ctx, &c, &cm) && phash_eq(&cm.root, &as_of));
	hr = head_root_of(&r);
	CHECK(phash_eq(&hr, &as_of));
	CHECK_EQ(row_version(&r, &cm.root, "t", 2), 1);
	CHECK_EQ(row_version(&r, &cm.root, "t", 3), -1);
	vc_commit_release(&cm);
	CHECK_EQ(working_row(&r, "t", 3), 1);	/* still in the working set */
	w = vc_working(&r);
	vc_diff_tables(&r.ctx, &as_of, &w, table_change_fn_count, &changed);
	CHECK_EQ(changed, 1);
	/* the uncommitted change is what blocks a merge */
	CHECK_EQ(vc_branch_create(&r, "other", NULL), VC_OK);
	CHECK_EQ(vc_merge(&r, "other", &m), VC_ERR);
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

ENGINE_TEST(test_branch_checkout_isolation)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	int			nbranches = 0;

	repo_open(&r, cs, &small_policy);
	create_table(&r, "t", schema_v1);
	put_row(&r, "t", 1, 1);
	vc_commit(&r, "base");
	CHECK_EQ(vc_branch_create(&r, "feature", NULL), VC_OK);
	CHECK_EQ(vc_branch_create(&r, "feature", NULL), VC_ERR);

	/* uncommitted work stays with its branch across checkouts */
	put_row(&r, "t", 2, 1);
	CHECK_EQ(vc_checkout(&r, "feature"), VC_OK);
	CHECK_EQ(working_row(&r, "t", 2), -1);
	put_row(&r, "t", 3, 1);
	CHECK_EQ(vc_checkout(&r, "main"), VC_OK);
	CHECK_EQ(working_row(&r, "t", 2), 1);
	CHECK_EQ(working_row(&r, "t", 3), -1);
	CHECK_EQ(vc_checkout(&r, "feature"), VC_OK);
	CHECK_EQ(working_row(&r, "t", 3), 1);
	CHECK_EQ(vc_checkout(&r, "nope"), VC_ERR);

	/*
	 * Uncommitted work outlives its transaction: each branch holds its
	 * working root (on "memory", what nothing holds is freed here)
	 */
	settle_and_check(cs);
	CHECK_EQ(working_row(&r, "t", 3), 1);
	CHECK_EQ(vc_checkout(&r, "main"), VC_OK);
	CHECK_EQ(working_row(&r, "t", 2), 1);
	CHECK_EQ(vc_checkout(&r, "feature"), VC_OK);

	/* the branch checked out is not deleted; another is, its work with it */
	CHECK_EQ(vc_branch_delete(&r, "feature"), VC_ERR);
	/* main is never deleted, checked out or not; a misnamed one is none */
	CHECK_EQ(vc_branch_delete(&r, "main"), VC_ERR);
	CHECK_EQ(vc_branch_delete(&r, ""), VC_ERR);
	CHECK_EQ(vc_branch_create(&r, "", NULL), VC_ERR);
	CHECK_EQ(vc_checkout(&r, ""), VC_ERR);
	CHECK_EQ(vc_checkout(&r, "main"), VC_OK);
	CHECK_EQ(vc_branch_delete(&r, "feature"), VC_OK);
	CHECK_EQ(vc_branch_delete(&r, "feature"), VC_ERR);
	CHECK_EQ(vc_checkout(&r, "feature"), VC_ERR);
	vc_branch_list(&r, count_branch_fn, &nbranches);
	CHECK_EQ(nbranches, 1);
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

ENGINE_TEST(test_reset_hard)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	PHash		c1,
				c2,
				h;

	repo_open(&r, cs, &small_policy);
	create_table(&r, "t", schema_v1);
	put_row(&r, "t", 1, 1);
	c1 = vc_commit(&r, "one");
	put_row(&r, "t", 1, 2);
	c2 = vc_commit(&r, "two");
	put_row(&r, "t", 1, 3);
	CHECK_EQ(vc_reset_hard(&r, &c2), VC_OK);
	CHECK_EQ(working_row(&r, "t", 1), 2);
	CHECK_EQ(vc_reset_hard(&r, &c1), VC_OK);
	CHECK_EQ(working_row(&r, "t", 1), 1);
	h = head_of(&r);
	CHECK(phash_eq(&h, &c1));
	CHECK_EQ(vc_reset_hard(&r, &c2), VC_OK);
	CHECK_EQ(working_row(&r, "t", 1), 2);

	/* soft: the head moves, the working root stays, its change uncommitted */
	put_row(&r, "t", 1, 4);
	CHECK_EQ(vc_reset_soft(&r, &c1), VC_OK);
	h = head_of(&r);
	CHECK(phash_eq(&h, &c1));
	CHECK_EQ(working_row(&r, "t", 1), 4);
	{
		PHash		hr = head_root_of(&r);

		CHECK_EQ(row_version(&r, &hr, "t", 1), 1);
	}
	h = vc_commit(&r, "over one");
	{
		VcCommit	cm;

		CHECK(vc_commit_load(&r.ctx, &h, &cm));
		CHECK(cm.nparents == 1 && phash_eq(&cm.parents[0], &c1));
		vc_commit_release(&cm);
	}
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

typedef struct TableDiffs
{
	int			added,
				dropped,
				schema_changed,
				data_changed;
} TableDiffs;

static void
table_change_fn(void *arg, const VcTableChange *c)
{
	TableDiffs *d = arg;

	if (!c->has_from)
		d->added++;
	else if (!c->has_to)
		d->dropped++;
	else
	{
		if (!phash_eq(&c->from.schema, &c->to.schema))
			d->schema_changed++;
		if (!phash_eq(&c->from.data, &c->to.data))
			d->data_changed++;
	}
}

ENGINE_TEST(test_diff_and_ddl)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	PHash		r0,
				r1,
				w;
	TableDiffs	d = {0};

	repo_open(&r, cs, &small_policy);
	create_table(&r, "a", schema_v1);
	create_table(&r, "b", schema_v1);
	create_table(&r, "c", schema_v1);
	put_row(&r, "a", 1, 1);
	put_row(&r, "b", 1, 1);
	vc_commit(&r, "base");
	r0 = head_root_of(&r);

	put_row(&r, "a", 2, 1);		/* data change */
	alter_table(&r, "b", schema_v2);	/* DDL change */
	drop_table(&r, "c");
	create_table(&r, "d", schema_v1);
	vc_commit(&r, "changes");
	r1 = head_root_of(&r);
	vc_diff_tables(&r.ctx, &r0, &r1, table_change_fn, &d);
	CHECK(d.added == 1 && d.dropped == 1 && d.schema_changed == 1 && d.data_changed == 1);

	/* status: committed vs working */
	memset(&d, 0, sizeof(d));
	w = vc_working(&r);
	vc_diff_tables(&r.ctx, &r1, &w, table_change_fn, &d);
	CHECK_EQ(d.added + d.dropped + d.schema_changed + d.data_changed, 0);
	CHECK(schema_is(&r, &r1, "b", schema_v2));
	CHECK(schema_is(&r, &r0, "b", schema_v1));
	CHECK_EQ(vc_table_create(&r.ctx, &r1, "a", "x", 1, &w), VC_ERR);
	CHECK_EQ(vc_table_drop(&r.ctx, &r1, "zzz", &w), VC_ERR);
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

ENGINE_TEST(test_merge_fast_forward_and_up_to_date)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	VcMergeResult m;
	PHash		fc;

	repo_open(&r, cs, &small_policy);
	create_table(&r, "t", schema_v1);
	vc_commit(&r, "base");
	CHECK_EQ(vc_branch_create(&r, "feature", NULL), VC_OK);
	CHECK_EQ(vc_checkout(&r, "feature"), VC_OK);
	put_row(&r, "t", 1, 1);
	fc = vc_commit(&r, "feature work");
	CHECK_EQ(vc_checkout(&r, "main"), VC_OK);
	CHECK_EQ(vc_merge(&r, "feature", &m), VC_OK);
	CHECK(m.status == VC_MERGE_FAST_FORWARD && phash_eq(&m.commit, &fc));
	CHECK_EQ(working_row(&r, "t", 1), 1);
	vc_merge_result_free(&m);
	CHECK_EQ(vc_merge(&r, "feature", &m), VC_OK);
	CHECK(m.status == VC_MERGE_UP_TO_DATE);
	vc_merge_result_free(&m);
	CHECK_EQ(vc_merge(&r, "nope", &m), VC_ERR);

	/* refuse to merge over uncommitted changes */
	put_row(&r, "t", 9, 9);
	CHECK_EQ(vc_merge(&r, "feature", &m), VC_ERR);
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

ENGINE_TEST(test_three_way_merge)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	VcMergeResult m;
	VcCommit	c;
	PHash		w;

	repo_open(&r, cs, &small_policy);
	create_table(&r, "t", schema_v1);
	create_table(&r, "u", schema_v1);
	{
		EditBatch	b;

		batch_init(&b, 2000);
		for (uint64 k = 0; k < 2000; k++)
			batch_put(&b, k, 0);
		apply_batch(&r, "t", &b);
		batch_free(&b);
	}
	vc_commit(&r, "base");
	CHECK_EQ(vc_branch_create(&r, "feature", NULL), VC_OK);

	put_row(&r, "t", 5, 1);
	del_row(&r, "t", 6);
	put_row(&r, "u", 1, 1);
	vc_commit(&r, "main work");

	CHECK_EQ(vc_checkout(&r, "feature"), VC_OK);
	put_row(&r, "t", 1500, 2);
	put_row(&r, "t", 5000, 2);
	alter_table(&r, "u", schema_v2);	/* DDL on one side, rows on the other */
	create_table(&r, "v", schema_v1);
	vc_commit(&r, "feature work");

	CHECK_EQ(vc_checkout(&r, "main"), VC_OK);
	CHECK_EQ(vc_merge(&r, "feature", &m), VC_OK);
	CHECK(m.status == VC_MERGE_COMMITTED && m.nconflicts == 0);
	CHECK(vc_commit_load(&r.ctx, &m.commit, &c));
	CHECK_EQ(c.nparents, 2);
	vc_commit_release(&c);
	w = vc_working(&r);
	CHECK_EQ(row_version(&r, &w, "t", 5), 1);
	CHECK_EQ(row_version(&r, &w, "t", 6), -1);
	CHECK_EQ(row_version(&r, &w, "t", 1500), 2);
	CHECK_EQ(row_version(&r, &w, "t", 5000), 2);
	CHECK_EQ(row_version(&r, &w, "u", 1), 1);
	CHECK(schema_is(&r, &w, "u", schema_v2));
	CHECK(schema_is(&r, &w, "v", schema_v1));
	vc_merge_result_free(&m);
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

ENGINE_TEST(test_merge_conflicts)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	VcMergeResult m;
	VcCommit	c;
	PHash		h;
	int			rows = 0,
				schemas = 0,
				tables = 0;

	repo_open(&r, cs, &small_policy);
	create_table(&r, "t", schema_v1);
	create_table(&r, "s", schema_v1);
	create_table(&r, "d", schema_v1);
	put_row(&r, "t", 1, 0);
	vc_commit(&r, "base");
	CHECK_EQ(vc_branch_create(&r, "feature", NULL), VC_OK);

	put_row(&r, "t", 1, 1);		/* row conflict */
	alter_table(&r, "s", schema_v2);	/* schema conflict */
	drop_table(&r, "d");		/* drop vs modify */
	put_row(&r, "t", 2, 1);		/* clean */
	vc_commit(&r, "main");

	CHECK_EQ(vc_checkout(&r, "feature"), VC_OK);
	put_row(&r, "t", 1, 2);
	alter_table(&r, "s", schema_v3);
	put_row(&r, "d", 1, 2);
	put_row(&r, "t", 3, 2);		/* clean */
	vc_commit(&r, "feature");

	CHECK_EQ(vc_checkout(&r, "main"), VC_OK);
	CHECK_EQ(vc_merge(&r, "feature", &m), VC_OK);
	CHECK(m.status == VC_MERGE_CONFLICTS);
	for (size_t i = 0; i < m.nconflicts; i++)
	{
		if (m.conflicts[i].kind == VC_CONFLICT_ROW)
		{
			rows++;
			CHECK(strcmp(m.conflicts[i].table, "t") == 0 && get_u64(m.conflicts[i].key) == 1);
		}
		else if (m.conflicts[i].kind == VC_CONFLICT_SCHEMA)
		{
			schemas++;
			CHECK(strcmp(m.conflicts[i].table, "s") == 0);
		}
		else
		{
			tables++;
			CHECK(strcmp(m.conflicts[i].table, "d") == 0);
		}
	}
	CHECK(rows == 1 && schemas == 1 && tables == 1);
	/* clean changes are applied; conflicts keep ours until resolved */
	CHECK_EQ(working_row(&r, "t", 3), 2);
	CHECK_EQ(working_row(&r, "t", 2), 1);
	CHECK_EQ(working_row(&r, "t", 1), 1);
	/* a pending merge is not merged over */
	{
		VcMergeResult again;

		CHECK_EQ(vc_merge(&r, "feature", &again), VC_ERR);
	}
	put_row(&r, "t", 1, 3);		/* resolve */
	vc_merge_result_free(&m);
	vc_commit(&r, "resolve");
	h = head_of(&r);
	CHECK(vc_commit_load(&r.ctx, &h, &c));
	CHECK_EQ(c.nparents, 2);
	vc_commit_release(&c);
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* Two branches reaching the same contents by different paths agree. */
ENGINE_TEST(test_same_changes_same_root)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	PHash		a,
				b;

	repo_open(&r, cs, &small_policy);
	create_table(&r, "t", schema_v1);
	vc_commit(&r, "base");
	CHECK_EQ(vc_branch_create(&r, "other", NULL), VC_OK);
	for (uint64 k = 0; k < 500; k++)
		put_row(&r, "t", k, k);
	create_table(&r, "z", schema_v2);
	a = vc_working(&r);
	CHECK_EQ(vc_checkout(&r, "other"), VC_OK);
	create_table(&r, "z", schema_v2);
	for (uint64 k = 500; k-- > 0;)
		put_row(&r, "t", k, k);
	put_row(&r, "t", 9999, 1);
	del_row(&r, "t", 9999);
	b = vc_working(&r);
	CHECK(phash_eq(&a, &b));
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

ENGINE_TEST(test_merge_base_shapes)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	PHash		base,
				x,
				y,
				mb,
				m1;
	VcMergeResult m;

	repo_open(&r, cs, &small_policy);
	create_table(&r, "t", schema_v1);
	base = vc_commit(&r, "base");
	CHECK_EQ(vc_branch_create(&r, "b", NULL), VC_OK);
	put_row(&r, "t", 1, 1);
	vc_commit(&r, "a1");
	put_row(&r, "t", 2, 1);
	x = vc_commit(&r, "a2");
	CHECK_EQ(vc_checkout(&r, "b"), VC_OK);
	put_row(&r, "t", 3, 1);
	y = vc_commit(&r, "b1");
	CHECK(vc_merge_base(&r.ctx, &x, &y, &mb) == 1 && phash_eq(&mb, &base));
	CHECK(vc_merge_base(&r.ctx, &y, &x, &mb) == 1 && phash_eq(&mb, &base));
	CHECK(vc_merge_base(&r.ctx, &x, &base, &mb) == 1 && phash_eq(&mb, &base));
	CHECK(vc_merge_base_in(&r, &x, &y, VC_WINDOW_ONLY, &mb) == 1 && phash_eq(&mb, &base));

	/* after merging main into b, the base of the two heads is main's head */
	CHECK(vc_merge(&r, "main", &m) == VC_OK && m.status == VC_MERGE_COMMITTED);
	m1 = m.commit;
	vc_merge_result_free(&m);
	CHECK(vc_merge_base(&r.ctx, &m1, &x, &mb) == 1 && phash_eq(&mb, &x));
	CHECK_EQ(vc_checkout(&r, "main"), VC_OK);
	put_row(&r, "t", 4, 1);
	vc_commit(&r, "a3");
	CHECK(vc_merge(&r, "b", &m) == VC_OK && m.status == VC_MERGE_COMMITTED);
	vc_merge_result_free(&m);
	CHECK(working_row(&r, "t", 1) == 1 && working_row(&r, "t", 3) == 1 &&
		  working_row(&r, "t", 4) == 1);
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/*
 * A commit without its reference is outside the window: window-only calls
 * refuse it, naming it; a commit not in the store is past a cut.
 */
ENGINE_TEST(test_window_edge)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	PHash		c1,
				root,
				orphan,
				absent,
				mb;
	VcBranch	none;
	VcCommit	c;

	repo_open(&r, cs, &small_policy);
	create_table(&r, "t", schema_v1);
	c1 = vc_commit(&r, "one");
	/* (a commit written, but not kept: no reference) */
	root = commit_root(&r, &c1);
	orphan = vc_commit_write(&r.ctx, &root, &c1, 1, 0, "outside");
	CHECK(!vc_in_window(&r, &orphan));
	CHECK(vc_in_window(&r, &c1));
	CHECK_EQ(vc_reset_hard(&r, &orphan), VC_ERR_OUTSIDE_WINDOW);
	CHECK(phash_eq(&r.ctx.err_hash, &orphan));
	CHECK_EQ(vc_branch_create(&r, "x", &orphan), VC_ERR_OUTSIDE_WINDOW);
	CHECK(!vc_branch_get(&r, "x", &none));
	CHECK_EQ(vc_merge_base_in(&r, &orphan, &c1, VC_WINDOW_ONLY, &mb),
			 VC_ERR_OUTSIDE_WINDOW);
	CHECK_EQ(vc_merge_base_in(&r, &orphan, &c1, 0, &mb), 1);
	CHECK(phash_eq(&mb, &c1));
	CHECK(vc_commit_load(&r.ctx, &orphan, &c));
	vc_commit_release(&c);

	memset(absent.b, 0x3c, PHASH_LEN);
	CHECK_EQ(vc_merge_base_in(&r, &absent, &c1, 0, &mb), VC_ERR_SEVERED);
	CHECK(phash_eq(&r.ctx.err_hash, &absent));

	/* log and diff stop at the edge, or at a commit not there */
	{
		LogAcc		acc;
		int			n = 0;

		CHECK_EQ(log_from(&r, &orphan, 8, VC_WINDOW_ONLY, &acc), VC_ERR_OUTSIDE_WINDOW);
		CHECK(acc.n == 0 && phash_eq(&r.ctx.err_hash, &orphan));
		CHECK_EQ(log_from(&r, &orphan, 8, 0, &acc), VC_OK);
		CHECK(acc.n == 3 && phash_eq(&acc.v[1], &c1));
		CHECK_EQ(log_from(&r, &absent, 8, 0, &acc), VC_ERR_SEVERED);
		CHECK(phash_eq(&r.ctx.err_hash, &absent));
		CHECK_EQ(vc_diff_commits(&r, &c1, &orphan, VC_WINDOW_ONLY,
								 table_change_fn_count, &n), VC_ERR_OUTSIDE_WINDOW);
		CHECK(phash_eq(&r.ctx.err_hash, &orphan));
		CHECK_EQ(vc_diff_commits(&r, &c1, &orphan, 0, table_change_fn_count, &n), VC_OK);
		CHECK_EQ(n, 0);
		CHECK_EQ(vc_diff_commits(&r, &absent, &c1, 0, table_change_fn_count, &n),
				 VC_ERR_SEVERED);
		CHECK(phash_eq(&r.ctx.err_hash, &absent));
	}

	/* histories sharing no commit have no merge base, window-only or not */
	{
		PHash		other = vc_commit_write(&r.ctx, &root, NULL, 0, 1, "another root");

		mb = absent;
		CHECK_EQ(vc_merge_base_in(&r, &other, &orphan, 0, &mb), 0);
		CHECK_EQ(vc_merge_base(&r.ctx, &other, &c1, &mb), 0);
		CHECK(phash_eq(&mb, &absent));	/* (untouched) */
	}
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* ---------------------------------------------------------------- scaling */

#define VC_K 50

typedef struct VcScale
{
	uint64		n;
	int			height;
	uint64		branch_reads,
				branch_writes;
	uint64		checkout_reads,
				checkout_writes;
	uint64		commit_reads,
				commit_writes;
	uint64		reset_reads;
	uint64		edit_writes;
	uint64		diff_reads;
	uint64		merge_reads,
				merge_writes;
	uint64		log_reads;
} VcScale;

static void
row_diff_count(void *arg, const ProllyDiff *d)
{
	(*(uint64 *) arg)++;
}

static void
vc_random_edits(EditBatch *b, uint64 n, size_t k, uint64 version)
{
	b->n = 0;
	for (size_t i = 0; i < k; i++)
	{
		uint64		key = rnd_below_global(2 * n);

		if (rnd_below_global(2))
			batch_put(b, key | 1, version);
		else
			batch_put(b, key & ~UINT64CONST(1), version);
	}
}

static void
vc_scale_case(uint64 n, VcScale *s)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	PHash		data,
				rv,
				c0,
				c1,
				r0,
				r1,
				h;
	ChunkStoreStats st;
	EditBatch	b;
	VcMergeResult m;
	VcTable		t0,
				t1;
	uint64		ndiff = 0;

	memset(s, 0, sizeof(*s));
	s->n = n;
	rng_seed_global(n + 7);
	batch_init(&b, VC_K);

	/*
	 * Small chunks keep both sizes unsaturated (and a level apart): with
	 * large ones, k edits touch nearly every leaf of the smaller tree, which
	 * hides how cost grows.
	 */
	repo_open(&r, cs, &small_policy);
	create_table(&r, "t", schema_v1);
	data = build_even_rows(&r.ctx, n);
	rv = vc_working(&r);
	CHECK_EQ(vc_table_set_data(&r.ctx, &rv, "t", &data, &rv), VC_OK);
	vc_set_working(&r, &rv);
	c0 = vc_commit(&r, "load");
	s->height = t_height(&r.ctx, &data);

	/* branch: the branch's chunk and its head read, a new branch's written */
	st = *cs_stats(cs);
	CHECK_EQ(vc_branch_create(&r, "feature", NULL), VC_OK);
	s->branch_reads = STATS_DELTA(cs, st, gets);
	s->branch_writes = STATS_DELTA(cs, st, puts);

	/* checkout: the branch's chunk and its head read, nothing written */
	st = *cs_stats(cs);
	CHECK_EQ(vc_checkout(&r, "feature"), VC_OK);
	s->checkout_reads = STATS_DELTA(cs, st, gets);
	s->checkout_writes = STATS_DELTA(cs, st, puts);

	/* k row edits on the working set */
	vc_random_edits(&b, n, VC_K, 1);
	st = *cs_stats(cs);
	apply_batch(&r, "t", &b);
	s->edit_writes = STATS_DELTA(cs, st, stored);

	/* commit: a commit chunk and the branch's, whatever the table size */
	st = *cs_stats(cs);
	c1 = vc_commit(&r, "feature edits");
	s->commit_reads = STATS_DELTA(cs, st, gets);
	s->commit_writes = STATS_DELTA(cs, st, stored);

	/* diff between commits: O(changes * height) */
	r0 = commit_root(&r, &c0);
	r1 = commit_root(&r, &c1);
	CHECK(vc_table_get(&r.ctx, &r0, "t", &t0) && vc_table_get(&r.ctx, &r1, "t", &t1));
	st = *cs_stats(cs);
	CHECK_OK(prolly_diff(&r.ctx, &t0.data, &t1.data, row_diff_count, &ndiff));
	s->diff_reads = STATS_DELTA(cs, st, gets);
	CHECK(ndiff > 0 && ndiff <= VC_K);

	/* reset --hard to an older commit */
	st = *cs_stats(cs);
	CHECK_EQ(vc_reset_hard(&r, &c0), VC_OK);
	s->reset_reads = STATS_DELTA(cs, st, gets);
	CHECK_EQ(vc_reset_hard(&r, &c1), VC_OK);

	/* three-way merge of k changes on each side */
	CHECK_EQ(vc_checkout(&r, "main"), VC_OK);
	vc_random_edits(&b, n, VC_K, 2);
	apply_batch(&r, "t", &b);
	vc_commit(&r, "main edits");
	st = *cs_stats(cs);
	CHECK_EQ(vc_merge(&r, "feature", &m), VC_OK);
	CHECK(m.status == VC_MERGE_COMMITTED || m.status == VC_MERGE_CONFLICTS);
	s->merge_reads = STATS_DELTA(cs, st, gets);
	s->merge_writes = STATS_DELTA(cs, st, stored);
	vc_merge_result_free(&m);

	/* log of the last 3 commits */
	h = head_of(&r);
	st = *cs_stats(cs);
	for (int i = 0; i < 3; i++)
	{
		VcCommit	c;

		CHECK(vc_commit_load(&r.ctx, &h, &c));
		h = c.parents[0];
		vc_commit_release(&c);
	}
	s->log_reads = STATS_DELTA(cs, st, gets);

	batch_free(&b);
	vc_repo_free(&r);
	cs_destroy(cs);
}

ENGINE_TEST(test_vc_scaling)
{
	static const uint64 sizes[] = {20000, 200000};
	VcScale		s[2];

	for (int i = 0; i < 2; i++)
	{
		VcScale    *x = &s[i];
		uint64		h;

		vc_scale_case(sizes[i], x);
		h = (uint64) x->height;

		/*
		 * O(1): a branch's chunk and its head commit, read or written (and
		 * the repository's chunk, for a branch made)
		 */
		CHECK_LE(x->branch_reads, 3);
		CHECK_EQ(x->branch_writes, 1);
		CHECK_LE(x->checkout_reads, 2);
		CHECK_EQ(x->checkout_writes, 0);
		/* a commit chunk and the branch's chunk */
		CHECK_EQ(x->commit_writes, 2);
		/* O(k log n): a commit reads what changed, to measure its change bytes */
		CHECK_LE(x->commit_reads, 4 * VC_K * h + 6);
		/* the commit, the branch's chunk and head, and the repository's */
		CHECK_LE(x->reset_reads, 4);
		CHECK_EQ(x->log_reads, 3);
		/* O(k log n); the +8 covers the root-value map, schema and branch */
		CHECK_LE(x->edit_writes, 3 * VC_K * h + 8);
		CHECK_LE(x->diff_reads, 4 * VC_K * h + 2);
		CHECK_LE(x->merge_reads, 12 * VC_K * h + 40);
		CHECK_LE(x->merge_writes, 3 * VC_K * h + 12);
	}
	/* the sizes a level apart */
	CHECK_LT(s[0].height, s[1].height);
	/* 10x the rows: costs follow height, not n */
	CHECK_LE(s[1].diff_reads, 2 * s[0].diff_reads * s[1].height / s[0].height);
	CHECK_LE(s[1].merge_reads, 2 * s[0].merge_reads * s[1].height / s[0].height);
	CHECK_LE(s[1].commit_reads, 2 * s[0].commit_reads * s[1].height / s[0].height);
}

/*
 * Merge base cost depends on how far the heads are from their common
 * ancestor, not on how long the history is behind it.
 */
static uint64
merge_base_reads(uint64 history, uint64 ahead)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	PHash		a,
				b,
				fork,
				mb;
	ChunkStoreStats st;
	uint64		reads;

	repo_open(&r, cs, &small_policy);
	create_table(&r, "t", schema_v1);
	for (uint64 i = 0; i < history; i++)
	{
		put_row(&r, "t", i % 64, i);
		vc_commit(&r, "history");
	}
	fork = head_of(&r);
	CHECK_EQ(vc_branch_create(&r, "b", NULL), VC_OK);
	for (uint64 i = 0; i < ahead; i++)
	{
		put_row(&r, "t", 1000 + i, i);
		vc_commit(&r, "a");
	}
	a = head_of(&r);
	CHECK_EQ(vc_checkout(&r, "b"), VC_OK);
	for (uint64 i = 0; i < ahead; i++)
	{
		put_row(&r, "t", 2000 + i, i);
		vc_commit(&r, "b");
	}
	b = head_of(&r);
	st = *cs_stats(cs);
	CHECK_EQ(vc_merge_base(&r.ctx, &a, &b, &mb), 1);
	reads = STATS_DELTA(cs, st, gets);
	CHECK(phash_eq(&mb, &fork));
	settle_and_check(cs);
	vc_repo_free(&r);
	cs_destroy(cs);
	return reads;
}

ENGINE_TEST(test_merge_base_cost)
{
	uint64		short_hist = merge_base_reads(10, 20);
	uint64		long_hist = merge_base_reads(5000, 20);

	/* two loads to start, then one per commit down to the fork */
	CHECK_LE(short_hist, 2 * 20 + 3);
	CHECK_EQ(long_hist, short_hist);
}

/*
 * Reading references frees what it asks with: a walk of history asks of
 * one a commit, and a long one must not grow with it.
 */
ENGINE_TEST(test_refs_reads_free_their_memory)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	PHash		h;
	MemoryContext cxt,
				old;
	MemoryContextCounters c;

	repo_open(&r, cs, &small_policy);
	h = head_of(&r);
	(void) vc_in_window(&r, &h);	/* (its plan made) */
	cxt = AllocSetContextCreate(CurrentMemoryContext, "refs reads",
								ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cxt);
	for (int i = 0; i < 2000; i++)
		CHECK(vc_in_window(&r, &h));
	MemoryContextSwitchTo(old);
	memset(&c, 0, sizeof(c));
	MemoryContextMemConsumed(cxt, &c);
	/* (some 70 bytes a read, were the arguments kept) */
	CHECK_LE(c.totalspace - c.freespace, 16 * 1024);
	MemoryContextDelete(cxt);
	vc_repo_free(&r);
	cs_destroy(cs);
}

static const ProllyUnitTest tests[] = {
	{"refs_contract", test_refs_contract},
	{"refs_reads_free_their_memory", test_refs_reads_free_their_memory},
	{"commit_and_log", test_commit_and_log},
	{"working_set_churn_frees", test_working_set_churn_frees},
	{"commit_root_keeps_later_changes", test_commit_root_keeps_later_changes},
	{"branch_checkout_isolation", test_branch_checkout_isolation},
	{"reset_hard", test_reset_hard},
	{"diff_and_ddl", test_diff_and_ddl},
	{"merge_fast_forward_and_up_to_date", test_merge_fast_forward_and_up_to_date},
	{"three_way_merge", test_three_way_merge},
	{"merge_conflicts", test_merge_conflicts},
	{"same_changes_same_root", test_same_changes_same_root},
	{"merge_base_shapes", test_merge_base_shapes},
	{"window_edge", test_window_edge},
	{"vc_scaling", test_vc_scaling},
	{"merge_base_cost", test_merge_base_cost},
};

const ProllyUnitSuite unit_engine_vc_suite = {"engine_vc", tests, lengthof(tests)};
