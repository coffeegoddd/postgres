/*
 * test_vc.c
 *	  Version control over prolly trees: commits, branches, checkout, reset,
 *	  log, diff, merge (rows and DDL), merge base, remotes, and the chunk
 *	  costs of each operation.
 */
#include "test_util.h"

static const char *schema_v1 = "CREATE TABLE t (id int8 PRIMARY KEY, v int8)";
static const char *schema_v2 = "CREATE TABLE t (id int8 PRIMARY KEY, v int8, w text)";
static const char *schema_v3 = "CREATE TABLE t (id int8 PRIMARY KEY, v numeric)";

static void
create_table(VcRepo *r, const char *name, const char *schema)
{
	PHash		rv = vc_working(r),
				out;

	CHECK(vc_table_create(&r->ctx, &rv, name, schema, strlen(schema), &out) == 0);
	vc_set_working(r, &out);
}

static void
alter_table(VcRepo *r, const char *name, const char *schema)
{
	PHash		rv = vc_working(r),
				out;

	CHECK(vc_table_alter(&r->ctx, &rv, name, schema, strlen(schema), &out) == 0);
	vc_set_working(r, &out);
}

static void
drop_table(VcRepo *r, const char *name)
{
	PHash		rv = vc_working(r),
				out;

	CHECK(vc_table_drop(&r->ctx, &rv, name, &out) == 0);
	vc_set_working(r, &out);
}

static void
put_row(VcRepo *r, const char *table, uint64_t key, uint64_t version)
{
	EditBatch	b;
	PHash		rv = vc_working(r),
				out;

	batch_init(&b, 1);
	batch_put(&b, key, version);
	CHECK(vc_table_edit(&r->ctx, &rv, table, b.e, b.n, &out) == 0);
	vc_set_working(r, &out);
	batch_free(&b);
}

static void
del_row(VcRepo *r, const char *table, uint64_t key)
{
	EditBatch	b;
	PHash		rv = vc_working(r),
				out;

	batch_init(&b, 1);
	batch_del(&b, key);
	CHECK(vc_table_edit(&r->ctx, &rv, table, b.e, b.n, &out) == 0);
	vc_set_working(r, &out);
	batch_free(&b);
}

static void
apply_batch(VcRepo *r, const char *table, const EditBatch *b)
{
	PHash		rv = vc_working(r),
				out;

	CHECK(vc_table_edit(&r->ctx, &rv, table, b->e, b->n, &out) == 0);
	vc_set_working(r, &out);
}

/* Row version in the working set, or -1 if absent. */
static int64_t
row_version(VcRepo *r, const PHash *rv, const char *table, uint64_t key)
{
	VcTable		t;
	uint8_t		k[8];
	const uint8_t *v;
	size_t		vl;

	CHECK(vc_table_get(&r->ctx, rv, table, &t));
	put_u64(k, key);
	if (!t_get(&r->ctx, &t.data, k, 8, &v, &vl))
		return -1;
	return (int64_t) get_u64(v + 8);
}

static int64_t
working_row(VcRepo *r, const char *table, uint64_t key)
{
	PHash		rv = vc_working(r);

	return row_version(r, &rv, table, key);
}

static bool
schema_is(VcRepo *r, const PHash *rv, const char *table, const char *schema)
{
	VcTable		t;
	size_t		len;
	const uint8_t *s;

	if (!vc_table_get(&r->ctx, rv, table, &t))
		return false;
	s = vc_schema_get(&r->ctx, &t.schema, &len);
	return s && bytes_eq(s, len, (const uint8_t *) schema, strlen(schema));
}

static PHash
commit_root(VcRepo *r, const PHash *commit)
{
	VcCommit	c;

	CHECK(vc_commit_load(&r->ctx, commit, &c));
	return c.root;
}

static void
table_change_fn_count(void *arg, const VcTableChange *c)
{
	(*(int *) arg)++;
}

/* ------------------------------------------------------------------ tests */

static void
test_commit_and_log(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	PHash		c[4];
	VcCommit	cm;
	PHash		h;
	int			n = 0;

	vc_repo_init(&r, cs, &small_policy);
	c[0] = vc_head(&r)->head;
	create_table(&r, "t", schema_v1);
	c[1] = vc_commit(&r, "create t");
	put_row(&r, "t", 1, 1);
	c[2] = vc_commit(&r, "row 1");
	put_row(&r, "t", 2, 1);
	c[3] = vc_commit(&r, "row 2");

	/* log walks first parents newest first */
	h = vc_head(&r)->head;
	for (;;)
	{
		CHECK(vc_commit_load(&r.ctx, &h, &cm));
		CHECK(phash_eq(&h, &c[3 - n]));
		CHECK_EQ(cm.generation, (uint64_t) (4 - n));
		n++;
		if (cm.nparents == 0)
			break;
		h = cm.parents[0];
	}
	CHECK_EQ(n, 4);
	CHECK(vc_commit_load(&r.ctx, &c[2], &cm));
	CHECK(cm.msglen == 5 && memcmp(cm.msg, "row 1", 5) == 0);

	/* a commit's root value holds the schema and rows as of that commit */
	{
		PHash		r1 = commit_root(&r, &c[1]),
					r2 = commit_root(&r, &c[2]);

		CHECK(schema_is(&r, &r1, "t", schema_v1));
		CHECK(row_version(&r, &r1, "t", 1) == -1);
		CHECK(row_version(&r, &r2, "t", 1) == 1);
	}
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/*
 * Every intermediate working set is released when the next replaces it:
 * the store holds only what commits and working sets reach.
 */
static void
test_working_set_churn_frees(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	size_t		after_commit;

	vc_repo_init(&r, cs, &small_policy);
	create_table(&r, "t", schema_v1);
	for (uint64_t k = 0; k < 3000; k++)
		put_row(&r, "t", k, 0);
	vc_commit(&r, "load");
	check_store(cs, 0);
	after_commit = cs->ops->count(cs);
	/* rewriting the same rows over and over must not grow the store */
	for (int round = 0; round < 5; round++)
		for (uint64_t k = 0; k < 3000; k += 7)
			put_row(&r, "t", k, (uint64_t) round + 1);
	for (uint64_t k = 0; k < 3000; k += 7)
		put_row(&r, "t", k, 0);
	check_store(cs, 0);
	CHECK_EQ(cs->ops->count(cs), after_commit);
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* Committing an earlier root leaves the working root's later changes uncommitted. */
static void
test_commit_root_keeps_later_changes(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	PHash		as_of,
				c,
				w;
	VcCommit	cm;
	int			changed = 0;

	vc_repo_init(&r, cs, &small_policy);
	create_table(&r, "t", schema_v1);
	put_row(&r, "t", 1, 1);
	vc_commit(&r, "one");
	put_row(&r, "t", 2, 1);
	as_of = vc_working(&r);
	cs_ref_root(cs, &as_of);	/* what a barrier would hold */
	put_row(&r, "t", 3, 1);		/* lands after the barrier */
	c = vc_commit_root(&r, &as_of, "as of the barrier");
	cs_unref_root(cs, &as_of);

	CHECK(vc_commit_load(&r.ctx, &c, &cm) && phash_eq(&cm.root, &as_of));
	CHECK(phash_eq(&vc_head(&r)->head_root, &as_of));
	CHECK(row_version(&r, &cm.root, "t", 2) == 1);
	CHECK(row_version(&r, &cm.root, "t", 3) == -1);
	CHECK(working_row(&r, "t", 3) == 1);	/* still in the working set */
	w = vc_working(&r);
	vc_diff_tables(&r.ctx, &as_of, &w, table_change_fn_count, &changed);
	CHECK_EQ(changed, 1);
	/* the uncommitted change is what blocks a merge */
	{
		VcMergeResult m;

		CHECK(vc_branch_create(&r, "other", NULL) == VC_OK);
		CHECK(vc_merge(&r, "other", &m) == VC_ERR);
	}
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
}

static void
test_branch_checkout_isolation(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;

	vc_repo_init(&r, cs, &small_policy);
	create_table(&r, "t", schema_v1);
	put_row(&r, "t", 1, 1);
	vc_commit(&r, "base");
	CHECK(vc_branch_create(&r, "feature", NULL) == 0);
	CHECK(vc_branch_create(&r, "feature", NULL) == VC_ERR);

	/* uncommitted work stays with its branch across checkouts */
	put_row(&r, "t", 2, 1);
	CHECK(vc_checkout(&r, "feature") == 0);
	CHECK(working_row(&r, "t", 2) == -1);
	put_row(&r, "t", 3, 1);
	CHECK(vc_checkout(&r, "main") == 0);
	CHECK(working_row(&r, "t", 2) == 1);
	CHECK(working_row(&r, "t", 3) == -1);
	CHECK(vc_checkout(&r, "feature") == 0);
	CHECK(working_row(&r, "t", 3) == 1);
	CHECK(vc_checkout(&r, "nope") == VC_ERR);
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
}

static void
test_reset_hard(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	PHash		c1,
				c2;

	vc_repo_init(&r, cs, &small_policy);
	create_table(&r, "t", schema_v1);
	put_row(&r, "t", 1, 1);
	c1 = vc_commit(&r, "one");
	put_row(&r, "t", 1, 2);
	c2 = vc_commit(&r, "two");
	put_row(&r, "t", 1, 3);
	CHECK(vc_reset_hard(&r, &c2) == VC_OK);
	CHECK(working_row(&r, "t", 1) == 2);
	CHECK(vc_reset_hard(&r, &c1) == VC_OK);
	CHECK(working_row(&r, "t", 1) == 1);
	CHECK(phash_eq(&vc_head(&r)->head, &c1));
	CHECK(vc_reset_hard(&r, &c2) == VC_OK);
	CHECK(working_row(&r, "t", 1) == 2);
	check_store(cs, 0);
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

static void
test_diff_and_ddl(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	PHash		r0,
				r1,
				w;
	TableDiffs	d = {0};

	vc_repo_init(&r, cs, &small_policy);
	create_table(&r, "a", schema_v1);
	create_table(&r, "b", schema_v1);
	create_table(&r, "c", schema_v1);
	put_row(&r, "a", 1, 1);
	put_row(&r, "b", 1, 1);
	vc_commit(&r, "base");
	r0 = vc_head(&r)->head_root;

	put_row(&r, "a", 2, 1);		/* data change */
	alter_table(&r, "b", schema_v2);	/* DDL change */
	drop_table(&r, "c");
	create_table(&r, "d", schema_v1);
	vc_commit(&r, "changes");
	r1 = vc_head(&r)->head_root;
	vc_diff_tables(&r.ctx, &r0, &r1, table_change_fn, &d);
	CHECK(d.added == 1 && d.dropped == 1 && d.schema_changed == 1 && d.data_changed == 1);

	/* status: committed vs working */
	memset(&d, 0, sizeof(d));
	w = vc_working(&r);
	vc_diff_tables(&r.ctx, &r1, &w, table_change_fn, &d);
	CHECK(d.added + d.dropped + d.schema_changed + d.data_changed == 0);
	CHECK(schema_is(&r, &r1, "b", schema_v2));
	CHECK(schema_is(&r, &r0, "b", schema_v1));
	CHECK(vc_table_create(&r.ctx, &r1, "a", "x", 1, &w) == -1);
	CHECK(vc_table_drop(&r.ctx, &r1, "zzz", &w) == -1);
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
}

static void
test_merge_fast_forward_and_up_to_date(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	VcMergeResult m;
	PHash		fc;

	vc_repo_init(&r, cs, &small_policy);
	create_table(&r, "t", schema_v1);
	vc_commit(&r, "base");
	vc_branch_create(&r, "feature", NULL);
	vc_checkout(&r, "feature");
	put_row(&r, "t", 1, 1);
	fc = vc_commit(&r, "feature work");
	vc_checkout(&r, "main");
	CHECK(vc_merge(&r, "feature", &m) == 0);
	CHECK(m.status == VC_MERGE_FAST_FORWARD && phash_eq(&m.commit, &fc));
	CHECK(working_row(&r, "t", 1) == 1);
	vc_merge_result_free(&m);
	CHECK(vc_merge(&r, "feature", &m) == 0);
	CHECK(m.status == VC_MERGE_UP_TO_DATE);
	vc_merge_result_free(&m);

	/* refuse to merge over uncommitted changes */
	put_row(&r, "t", 9, 9);
	CHECK(vc_merge(&r, "feature", &m) == VC_ERR);
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
}

static void
test_three_way_merge(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	VcMergeResult m;
	VcCommit	c;
	PHash		w;

	vc_repo_init(&r, cs, &small_policy);
	create_table(&r, "t", schema_v1);
	create_table(&r, "u", schema_v1);
	for (uint64_t k = 0; k < 2000; k++)
		put_row(&r, "t", k, 0);
	vc_commit(&r, "base");
	vc_branch_create(&r, "feature", NULL);

	put_row(&r, "t", 5, 1);
	del_row(&r, "t", 6);
	put_row(&r, "u", 1, 1);
	vc_commit(&r, "main work");

	vc_checkout(&r, "feature");
	put_row(&r, "t", 1500, 2);
	put_row(&r, "t", 5000, 2);
	alter_table(&r, "u", schema_v2);	/* DDL on one side, rows on the other */
	create_table(&r, "v", schema_v1);
	vc_commit(&r, "feature work");

	vc_checkout(&r, "main");
	CHECK(vc_merge(&r, "feature", &m) == 0);
	CHECK(m.status == VC_MERGE_COMMITTED && m.nconflicts == 0);
	CHECK(vc_commit_load(&r.ctx, &m.commit, &c));
	CHECK(c.nparents == 2);
	w = vc_working(&r);
	CHECK(row_version(&r, &w, "t", 5) == 1);
	CHECK(row_version(&r, &w, "t", 6) == -1);
	CHECK(row_version(&r, &w, "t", 1500) == 2);
	CHECK(row_version(&r, &w, "t", 5000) == 2);
	CHECK(row_version(&r, &w, "u", 1) == 1);
	CHECK(schema_is(&r, &w, "u", schema_v2));
	CHECK(schema_is(&r, &w, "v", schema_v1));
	vc_merge_result_free(&m);
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
}

static void
test_merge_conflicts(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	VcMergeResult m;
	VcCommit	c;
	int			rows = 0,
				schemas = 0,
				tables = 0;

	vc_repo_init(&r, cs, &small_policy);
	create_table(&r, "t", schema_v1);
	create_table(&r, "s", schema_v1);
	create_table(&r, "d", schema_v1);
	put_row(&r, "t", 1, 0);
	vc_commit(&r, "base");
	vc_branch_create(&r, "feature", NULL);

	put_row(&r, "t", 1, 1);		/* row conflict */
	alter_table(&r, "s", schema_v2);	/* schema conflict */
	drop_table(&r, "d");		/* drop vs modify */
	put_row(&r, "t", 2, 1);		/* clean */
	vc_commit(&r, "main");

	vc_checkout(&r, "feature");
	put_row(&r, "t", 1, 2);
	alter_table(&r, "s", schema_v3);
	put_row(&r, "d", 1, 2);
	put_row(&r, "t", 3, 2);		/* clean */
	vc_commit(&r, "feature");

	vc_checkout(&r, "main");
	CHECK(vc_merge(&r, "feature", &m) == 0);
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
	CHECK(working_row(&r, "t", 3) == 2);
	CHECK(working_row(&r, "t", 2) == 1);
	CHECK(working_row(&r, "t", 1) == 1);
	put_row(&r, "t", 1, 3);		/* resolve */
	vc_merge_result_free(&m);
	vc_commit(&r, "resolve");
	CHECK(vc_commit_load(&r.ctx, &vc_head(&r)->head, &c));
	CHECK(c.nparents == 2);
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
}

static void
test_same_changes_same_root(void)
{
	/* two branches reaching the same contents by different paths agree */
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	PHash		a,
				b;

	vc_repo_init(&r, cs, &small_policy);
	create_table(&r, "t", schema_v1);
	vc_commit(&r, "base");
	vc_branch_create(&r, "other", NULL);
	for (uint64_t k = 0; k < 500; k++)
		put_row(&r, "t", k, k);
	create_table(&r, "z", schema_v2);
	a = vc_working(&r);
	vc_checkout(&r, "other");
	create_table(&r, "z", schema_v2);
	for (uint64_t k = 500; k-- > 0;)
		put_row(&r, "t", k, k);
	put_row(&r, "t", 9999, 1);
	del_row(&r, "t", 9999);
	b = vc_working(&r);
	CHECK(phash_eq(&a, &b));
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
}

static void
test_merge_base_shapes(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	PHash		base,
				x,
				y,
				mb,
				m1;
	VcMergeResult m;

	vc_repo_init(&r, cs, &small_policy);
	create_table(&r, "t", schema_v1);
	base = vc_commit(&r, "base");
	vc_branch_create(&r, "b", NULL);
	put_row(&r, "t", 1, 1);
	vc_commit(&r, "a1");
	put_row(&r, "t", 2, 1);
	x = vc_commit(&r, "a2");
	vc_checkout(&r, "b");
	put_row(&r, "t", 3, 1);
	y = vc_commit(&r, "b1");
	CHECK(vc_merge_base(&r.ctx, &x, &y, &mb) == 1 && phash_eq(&mb, &base));
	CHECK(vc_merge_base(&r.ctx, &y, &x, &mb) == 1 && phash_eq(&mb, &base));
	CHECK(vc_merge_base(&r.ctx, &x, &base, &mb) == 1 && phash_eq(&mb, &base));

	/* after merging main into b, the base of the two heads is main's head */
	CHECK(vc_merge(&r, "main", &m) == 0 && m.status == VC_MERGE_COMMITTED);
	m1 = m.commit;
	vc_merge_result_free(&m);
	CHECK(vc_merge_base(&r.ctx, &m1, &x, &mb) == 1 && phash_eq(&mb, &x));
	vc_checkout(&r, "main");
	put_row(&r, "t", 4, 1);
	vc_commit(&r, "a3");
	CHECK(vc_merge(&r, "b", &m) == 0 && m.status == VC_MERGE_COMMITTED);
	vc_merge_result_free(&m);
	CHECK(working_row(&r, "t", 1) == 1 && working_row(&r, "t", 3) == 1 &&
		  working_row(&r, "t", 4) == 1);
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
}

static void
test_push_fetch_clone(void)
{
	ChunkStore *rcs = memstore_create();
	ChunkStore *lcs = memstore_create();
	ChunkStore *ocs = memstore_create();
	VcRepo		remote,
				local,
				other;
	PHash		head;
	VcMergeResult m;

	vc_repo_init(&remote, rcs, &small_policy);
	create_table(&remote, "t", schema_v1);
	for (uint64_t k = 0; k < 300; k++)
		put_row(&remote, "t", k, 0);
	vc_commit(&remote, "seed");

	CHECK(vc_repo_clone(&local, lcs, &remote, 0) == VC_OK);
	CHECK(vc_repo_clone(&other, ocs, &remote, 0) == VC_OK);
	CHECK(phash_eq(&vc_head(&local)->head, &vc_head(&remote)->head));
	CHECK(working_row(&local, "t", 299) == 0);

	put_row(&local, "t", 1, 1);
	vc_commit(&local, "local change");
	CHECK(vc_push(&local, &remote, "main") == 0);
	CHECK(phash_eq(&vc_head(&remote)->head, &vc_head(&local)->head));
	CHECK(working_row(&remote, "t", 1) == 1);

	/* other's push is not a fast-forward of the remote: rejected */
	put_row(&other, "t", 2, 2);
	vc_commit(&other, "other change");
	CHECK(vc_push(&other, &remote, "main") == VC_ERR);

	/* fetch, merge, push succeeds */
	CHECK(vc_fetch(&other, &remote, "main", &head) == 0);
	CHECK(vc_branch_create(&other, "origin/main", &head) == 0);
	CHECK(vc_merge(&other, "origin/main", &m) == 0 && m.status == VC_MERGE_COMMITTED);
	vc_merge_result_free(&m);
	CHECK(vc_push(&other, &remote, "main") == 0);
	CHECK(working_row(&remote, "t", 1) == 1 && working_row(&remote, "t", 2) == 2);

	/* a new branch pushes too */
	vc_branch_create(&local, "topic", NULL);
	CHECK(vc_push(&local, &remote, "topic") == 0);
	CHECK(vc_branch_find(&remote, "topic") != NULL);

	check_store(rcs, 0);
	check_store(lcs, 0);
	check_store(ocs, 0);
	vc_repo_free(&remote);
	vc_repo_free(&local);
	vc_repo_free(&other);
	cs_destroy(rcs);
	cs_destroy(lcs);
	cs_destroy(ocs);
}

/* ---------------------------------------------------------------- scaling */

#define VC_K 50

typedef struct VcScale
{
	uint64_t	n;
	int			height;
	uint64_t	branch_reads,
				branch_writes;
	uint64_t	checkout_reads,
				checkout_writes;
	uint64_t	commit_reads,
				commit_writes;
	uint64_t	reset_reads;
	uint64_t	edit_writes;
	uint64_t	diff_reads;
	uint64_t	merge_reads,
				merge_writes;
	uint64_t	push_chunks;
	uint64_t	log_reads;
} VcScale;

static void
row_diff_count(void *arg, const ProllyDiff *d)
{
	(*(uint64_t *) arg)++;
}

static void
vc_random_edits(EditBatch *b, uint64_t n, size_t k, uint64_t version)
{
	b->n = 0;
	for (size_t i = 0; i < k; i++)
	{
		uint64_t	key = rnd_below(2 * n);

		if (rnd_below(2))
			batch_put(b, key | 1, version);
		else
			batch_put(b, key & ~1ULL, version);
	}
}

static void
vc_scale_case(uint64_t n, VcScale *s)
{
	ChunkStore *cs = memstore_create();
	ChunkStore *rcs = memstore_create();
	VcRepo		r,
				remote;
	PHash		data,
				rv,
				c0,
				c1,
				r0,
				r1;
	ChunkStoreStats st;
	EditBatch	b;
	VcMergeResult m;
	VcTable		t0,
				t1;
	uint64_t	ndiff = 0;

	memset(s, 0, sizeof(*s));
	s->n = n;
	rng_seed(n + 7);
	batch_init(&b, VC_K);
	vc_repo_init(&r, cs, &prolly_default_policy);
	create_table(&r, "t", schema_v1);
	data = build_even_rows(&r.ctx, n);
	rv = vc_working(&r);
	CHECK(vc_table_set_data(&r.ctx, &rv, "t", &data, &rv) == 0);
	vc_set_working(&r, &rv);
	c0 = vc_commit(&r, "load");
	s->height = t_height(&r.ctx, &data);
	CHECK(vc_repo_clone(&remote, rcs, &r, 0) == VC_OK);

	/* branch: a new ref, no chunks */
	st = cs->stats;
	CHECK(vc_branch_create(&r, "feature", NULL) == 0);
	s->branch_reads = STATS_DELTA(cs, st, gets);
	s->branch_writes = STATS_DELTA(cs, st, puts);

	/* checkout: switch refs, no chunks */
	st = cs->stats;
	CHECK(vc_checkout(&r, "feature") == 0);
	s->checkout_reads = STATS_DELTA(cs, st, gets);
	s->checkout_writes = STATS_DELTA(cs, st, puts);

	/* k row edits on the working set */
	vc_random_edits(&b, n, VC_K, 1);
	st = cs->stats;
	apply_batch(&r, "t", &b);
	s->edit_writes = STATS_DELTA(cs, st, new_chunks);

	/* commit: one commit chunk, whatever the table size */
	st = cs->stats;
	c1 = vc_commit(&r, "feature edits");
	s->commit_reads = STATS_DELTA(cs, st, gets);
	s->commit_writes = STATS_DELTA(cs, st, new_chunks);

	/* diff between commits: O(changes * height) */
	r0 = commit_root(&r, &c0);
	r1 = commit_root(&r, &c1);
	CHECK(vc_table_get(&r.ctx, &r0, "t", &t0) && vc_table_get(&r.ctx, &r1, "t", &t1));
	st = cs->stats;
	CHECK_OK(prolly_diff(&r.ctx, &t0.data, &t1.data, row_diff_count, &ndiff));
	s->diff_reads = STATS_DELTA(cs, st, gets);
	CHECK(ndiff > 0 && ndiff <= VC_K);

	/* push the new commit: only the changed chunks travel */
	st = rcs->stats;
	CHECK(vc_push(&r, &remote, "feature") == 0);
	s->push_chunks = STATS_DELTA(rcs, st, new_chunks);

	/* reset --hard to an older commit */
	st = cs->stats;
	CHECK(vc_reset_hard(&r, &c0) == VC_OK);
	s->reset_reads = STATS_DELTA(cs, st, gets);
	CHECK(vc_reset_hard(&r, &c1) == VC_OK);

	/* three-way merge of k changes on each side */
	vc_checkout(&r, "main");
	vc_random_edits(&b, n, VC_K, 2);
	apply_batch(&r, "t", &b);
	vc_commit(&r, "main edits");
	st = cs->stats;
	CHECK(vc_merge(&r, "feature", &m) == 0);
	CHECK(m.status == VC_MERGE_COMMITTED || m.status == VC_MERGE_CONFLICTS);
	s->merge_reads = STATS_DELTA(cs, st, gets);
	s->merge_writes = STATS_DELTA(cs, st, new_chunks);
	vc_merge_result_free(&m);

	/* log of the last 3 commits */
	{
		PHash		h = vc_head(&r)->head;
		VcCommit	c;

		st = cs->stats;
		for (int i = 0; i < 3; i++)
		{
			CHECK(vc_commit_load(&r.ctx, &h, &c));
			h = c.parents[0];
		}
		s->log_reads = STATS_DELTA(cs, st, gets);
	}

	batch_free(&b);
	vc_repo_free(&r);
	vc_repo_free(&remote);
	cs_destroy(cs);
	cs_destroy(rcs);
}

static void
test_vc_scaling(void)
{
	static const uint64_t sizes[] = {100000, 1000000};
	VcScale		s[2];

	printf("\n  %8s %2s %8s %8s %8s %6s %6s %9s %8s %10s %6s %4s\n",
		   "rows", "h", "branch", "checkout", "commit", "reset", "edit50",
		   "diff50 r", "push50", "merge r/w", "log3", "");
	for (int i = 0; i < 2; i++)
	{
		VcScale    *x = &s[i];
		uint64_t	h;
		char		br[32],
					co[32],
					cm[32],
					mg[48];

		vc_scale_case(sizes[i], x);
		h = (uint64_t) x->height;
		snprintf(br, sizeof(br), "%llu/%llu", (unsigned long long) x->branch_reads,
				 (unsigned long long) x->branch_writes);
		snprintf(co, sizeof(co), "%llu/%llu", (unsigned long long) x->checkout_reads,
				 (unsigned long long) x->checkout_writes);
		snprintf(cm, sizeof(cm), "%llu/%llu", (unsigned long long) x->commit_reads,
				 (unsigned long long) x->commit_writes);
		snprintf(mg, sizeof(mg), "%llu/%llu", (unsigned long long) x->merge_reads,
				 (unsigned long long) x->merge_writes);
		printf("  %8llu %2d %8s %8s %8s %6llu %6llu %9llu %8llu %10s %6llu\n",
			   (unsigned long long) x->n, x->height, br, co, cm,
			   (unsigned long long) x->reset_reads, (unsigned long long) x->edit_writes,
			   (unsigned long long) x->diff_reads, (unsigned long long) x->push_chunks,
			   mg, (unsigned long long) x->log_reads);

		/* O(1) */
		CHECK_EQ(x->branch_reads + x->branch_writes, 0);
		CHECK_EQ(x->checkout_reads + x->checkout_writes, 0);
		CHECK_EQ(x->commit_writes, 1);
		CHECK_LE(x->commit_reads, 1);
		CHECK_EQ(x->reset_reads, 1);
		CHECK_EQ(x->log_reads, 3);
		/* O(k log n); the +6 covers the root-value map and schema */
		CHECK_LE(x->edit_writes, 3 * VC_K * h + 6);
		CHECK_LE(x->diff_reads, 4 * VC_K * h + 2);
		CHECK_LE(x->push_chunks, 3 * VC_K * h + 6);
		CHECK_LE(x->merge_reads, 12 * VC_K * h + 40);
		CHECK_LE(x->merge_writes, 3 * VC_K * h + 12);
	}
	/* 10x the rows: costs follow height, not n */
	CHECK_LE(s[1].diff_reads, 2 * s[0].diff_reads * s[1].height / s[0].height);
	CHECK_LE(s[1].push_chunks, 2 * s[0].push_chunks * s[1].height / s[0].height);
	CHECK_LE(s[1].merge_reads, 2 * s[0].merge_reads * s[1].height / s[0].height);
	printf("%-44s", "");
}

/*
 * Merge base cost depends on how far the heads are from their common
 * ancestor, not on how long the history is behind it.
 */
static uint64_t
merge_base_reads(uint64_t history, uint64_t ahead)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	PHash		a,
				b,
				fork,
				mb;
	ChunkStoreStats st;
	uint64_t	reads;

	vc_repo_init(&r, cs, &small_policy);
	create_table(&r, "t", schema_v1);
	for (uint64_t i = 0; i < history; i++)
	{
		put_row(&r, "t", i % 64, i);
		vc_commit(&r, "history");
	}
	fork = vc_head(&r)->head;
	vc_branch_create(&r, "b", NULL);
	for (uint64_t i = 0; i < ahead; i++)
	{
		put_row(&r, "t", 1000 + i, i);
		vc_commit(&r, "a");
	}
	a = vc_head(&r)->head;
	vc_checkout(&r, "b");
	for (uint64_t i = 0; i < ahead; i++)
	{
		put_row(&r, "t", 2000 + i, i);
		vc_commit(&r, "b");
	}
	b = vc_head(&r)->head;
	st = cs->stats;
	CHECK(vc_merge_base(&r.ctx, &a, &b, &mb) == 1);
	reads = STATS_DELTA(cs, st, gets);
	CHECK(phash_eq(&mb, &fork));
	check_store(cs, 0);
	vc_repo_free(&r);
	cs_destroy(cs);
	return reads;
}

static void
test_merge_base_cost(void)
{
	uint64_t	short_hist = merge_base_reads(10, 20);
	uint64_t	long_hist = merge_base_reads(5000, 20);

	/* two loads to start, then one per commit down to the fork */
	CHECK_LE(short_hist, 2 * 20 + 3);
	CHECK_EQ(long_hist, short_hist);
}

int
main(void)
{
	RUN(test_commit_and_log);
	RUN(test_working_set_churn_frees);
	RUN(test_commit_root_keeps_later_changes);
	RUN(test_branch_checkout_isolation);
	RUN(test_reset_hard);
	RUN(test_diff_and_ddl);
	RUN(test_merge_fast_forward_and_up_to_date);
	RUN(test_three_way_merge);
	RUN(test_merge_conflicts);
	RUN(test_same_changes_same_root);
	RUN(test_merge_base_shapes);
	RUN(test_push_fetch_clone);
	RUN(test_merge_base_cost);
	RUN(test_vc_scaling);
	printf("test_vc: all tests passed\n");
	return 0;
}
