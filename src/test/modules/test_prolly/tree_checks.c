/*--------------------------------------------------------------------------
 *
 * tree_checks.c
 *		Conformance checks for the tree interface.
 *
 * Trees are built from the prolly_d1 profile's foundation components (node
 * format, chunker, hash, node store, log) and compared against an in-memory
 * reference map: keys are 0 .. MAX_KEY-1 as 4-byte big-endian integers, and
 * each key's value is derived from a version number, so any state of the
 * map is (key -> version or absent).
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_prolly/tree_checks.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_class.h"
#include "common/pg_prng.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "test_prolly.h"

#define MAX_KEY		20000
#define ABSENT		(-1)

typedef struct Ref
{
	int32		version[MAX_KEY];	/* ABSENT, or the value's version */
} Ref;

typedef struct KeyBuf
{
	uint8		bytes[4];
} KeyBuf;

static int
memcmp_keys(const ProllyKey *a, const ProllyKey *b, void *arg)
{
	int			r = memcmp(a->data, b->data, Min(a->len, b->len));

	if (r != 0)
		return r;
	return (a->len > b->len) - (a->len < b->len);
}

static void
make_env(const ProllyTreeRoutine *tree, ProllyTreeEnv *env)
{
	ProllyComponents c;

	(void) tree;
	prolly_components_for_am("prolly_d1", &c);
	memset(env, 0, sizeof(*env));
	env->node_format = c.node_format;
	env->chunker = c.chunker;
	env->hash = c.hash;
	env->node_store = c.node_store;
	env->log = c.log;
	PROLLY_CALL(env->node_store, create, NULL, NULL, RELPERSISTENCE_PERMANENT);
	env->store = PROLLY_CALL(env->node_store, open, NULL);
	env->compare = memcmp_keys;
}

static ProllyKey
key_of(int k, KeyBuf *buf)
{
	ProllyKey	key;

	buf->bytes[0] = (uint8) (k >> 24);
	buf->bytes[1] = (uint8) (k >> 16);
	buf->bytes[2] = (uint8) (k >> 8);
	buf->bytes[3] = (uint8) k;
	key.data = buf->bytes;
	key.len = 4;
	return key;
}

static int
key_int(const ProllyKey *key)
{
	if (key->len != 4)
		elog(ERROR, "tree returned a key of %u bytes", key->len);
	return (key->data[0] << 24) | (key->data[1] << 16) | (key->data[2] << 8) |
		key->data[3];
}

/* Value for (k, version): its length varies, up to a few hundred bytes. */
static ProllyValue
value_of(int k, int version, StringInfo buf)
{
	ProllyValue v;
	int			len = (k * 7 + version * 13) % 300;

	resetStringInfo(buf);
	appendStringInfo(buf, "%d:%d:", k, version);
	while (buf->len < len)
		appendStringInfoChar(buf, (char) ('a' + (k + buf->len) % 26));
	v.data = (const uint8 *) buf->data;
	v.len = buf->len;
	return v;
}

static bool
same_value(const ProllyValue *v, int k, int version)
{
	StringInfoData buf;
	ProllyValue want;
	bool		same;

	initStringInfo(&buf);
	want = value_of(k, version, &buf);
	same = v->len == want.len && memcmp(v->data, want.data, v->len) == 0;
	pfree(buf.data);
	return same;
}

static bool
same_hash(const ProllyNodeRef *a, const ProllyNodeRef *b)
{
	return a->hash.len == b->hash.len &&
		memcmp(a->hash.bytes, b->hash.bytes, a->hash.len) == 0;
}

/* Build the tree holding ref's items, in one pass. */
static void
build_ref(const ProllyTreeRoutine *tr, const ProllyTreeEnv *env, const Ref *ref,
		  ProllyNodeRef *root)
{
	ProllyTreeBuilder *b = PROLLY_CALL(tr, build_begin, env, CurrentMemoryContext);
	StringInfoData vbuf;

	initStringInfo(&vbuf);
	for (int k = 0; k < MAX_KEY; k++)
	{
		KeyBuf		kb;
		ProllyKey	key;
		ProllyValue value;

		if (ref->version[k] == ABSENT)
			continue;
		key = key_of(k, &kb);
		value = value_of(k, ref->version[k], &vbuf);
		PROLLY_CALL(tr, build_add, b, &key, &value);
	}
	PROLLY_CALL(tr, build_finish, b, root);
}

/*
 * Apply a batch: for each k in keys (sorted, unique), put version, or
 * delete if version is ABSENT.  Updates ref too.
 */
static void
apply_batch(const ProllyTreeRoutine *tr, const ProllyTreeEnv *env, Ref *ref,
			const int *keys, const int *versions, int n, ProllyNodeRef *root)
{
	ProllyEdit *edits = palloc(sizeof(ProllyEdit) * Max(n, 1));
	KeyBuf	   *kbs = palloc(sizeof(KeyBuf) * Max(n, 1));
	StringInfoData *vbufs = palloc(sizeof(StringInfoData) * Max(n, 1));
	ProllyNodeRef newroot;

	for (int i = 0; i < n; i++)
	{
		edits[i].key = key_of(keys[i], &kbs[i]);
		initStringInfo(&vbufs[i]);
		edits[i].is_delete = versions[i] == ABSENT;
		if (!edits[i].is_delete)
			edits[i].value = value_of(keys[i], versions[i], &vbufs[i]);
		ref->version[keys[i]] = versions[i];
	}
	PROLLY_CALL(tr, apply, env, root, edits, n, &newroot);
	*root = newroot;
	for (int i = 0; i < n; i++)
		pfree(vbufs[i].data);
	pfree(edits);
	pfree(kbs);
	pfree(vbufs);
}

/* A random batch of up to maxn edits over keys [0, range). */
static int
random_batch(pg_prng_state *rng, int maxn, int range, int version, int *keys,
			 int *versions)
{
	int			n = (int) pg_prng_uint64_range(rng, 1, maxn);
	bool	   *used = palloc0(sizeof(bool) * range);
	int			count = 0;

	for (int i = 0; i < n; i++)
		used[pg_prng_uint64_range(rng, 0, range - 1)] = true;
	for (int k = 0; k < range; k++)
	{
		if (!used[k])
			continue;
		keys[count] = k;
		versions[count] = pg_prng_uint64_range(rng, 0, 3) == 0 ? ABSENT : version;
		count++;
	}
	pfree(used);
	return count;
}

/* Every item of the tree, in order, matches ref; so does count. */
static void
check_matches(const ProllyTreeRoutine *tr, const ProllyTreeEnv *env,
			  const ProllyNodeRef *root, const Ref *ref, const char *when)
{
	ProllyTreeCursor *c = PROLLY_CALL(tr, cursor_open, env, root, CurrentMemoryContext);
	ProllyKey	key;
	ProllyValue value;
	int			next = 0;
	uint64		n = 0;

	PROLLY_CALL(tr, cursor_seek, c, NULL, ForwardScanDirection);
	while (PROLLY_CALL(tr, cursor_next, c, &key, &value))
	{
		int			k = key_int(&key);

		while (next < k && ref->version[next] == ABSENT)
			next++;
		if (k != next)
			elog(ERROR, "%s: scan returned key %d, expected %d", when, k, next);
		if (!same_value(&value, k, ref->version[k]))
			elog(ERROR, "%s: key %d has the wrong value", when, k);
		next++;
		n++;
	}
	PROLLY_CALL(tr, cursor_close, c);
	while (next < MAX_KEY && ref->version[next] == ABSENT)
		next++;
	if (next != MAX_KEY)
		elog(ERROR, "%s: scan ended before key %d", when, next);
	if (PROLLY_CALL(tr, count, env, root) != n)
		elog(ERROR, "%s: count is " UINT64_FORMAT ", scan found " UINT64_FORMAT,
			 when, PROLLY_CALL(tr, count, env, root), n);
}

static void
fill_ref(Ref *ref, int every, int version)
{
	for (int k = 0; k < MAX_KEY; k++)
		ref->version[k] = (k % every == 0) ? version : ABSENT;
}

void
tree_build_and_get(const ProllyImplInfo *impl)
{
	const ProllyTreeRoutine *tr = (const ProllyTreeRoutine *) impl;
	ProllyTreeEnv env;
	Ref		   *ref = palloc(sizeof(Ref));
	ProllyNodeRef root;

	make_env(tr, &env);
	fill_ref(ref, 3, 1);
	build_ref(tr, &env, ref, &root);

	for (int k = 0; k < MAX_KEY; k += 1)
	{
		KeyBuf		kb;
		ProllyKey	key = key_of(k, &kb);
		StringInfoData out;
		bool		found;

		initStringInfo(&out);
		found = PROLLY_CALL(tr, get, &env, &root, &key, &out);
		if (found != (ref->version[k] != ABSENT))
			elog(ERROR, "get(%d) %s", k, found ? "found an absent key" : "missed a key");
		if (found)
		{
			ProllyValue v = {(const uint8 *) out.data, out.len};

			if (!same_value(&v, k, ref->version[k]))
				elog(ERROR, "get(%d) returned the wrong value", k);
		}
		pfree(out.data);
	}
	check_matches(tr, &env, &root, ref, "built tree");
}

void
tree_cursor_scans(const ProllyImplInfo *impl)
{
	const ProllyTreeRoutine *tr = (const ProllyTreeRoutine *) impl;
	ProllyTreeEnv env;
	Ref		   *ref = palloc(sizeof(Ref));
	ProllyNodeRef root;
	ProllyNodeRef empty;
	ProllyTreeCursor *c;
	ProllyKey	key;
	ProllyValue value;
	static const int seeks[] = {0, 1, 2, 3, 299, 300, 301, 9999, 19998, 19999, 25000};

	make_env(tr, &env);
	fill_ref(ref, 3, 1);			/* keys 0, 3, 6, ..., 19998 */
	build_ref(tr, &env, ref, &root);

	c = PROLLY_CALL(tr, cursor_open, &env, &root, CurrentMemoryContext);
	for (int s = 0; s < lengthof(seeks); s++)
	{
		KeyBuf		kb;
		ProllyKey	target = key_of(seeks[s], &kb);
		int			want;
		int			steps = 0;

		/* Forward: the first key >= target, then ascending. */
		PROLLY_CALL(tr, cursor_seek, c, &target, ForwardScanDirection);
		want = seeks[s] + (3 - seeks[s] % 3) % 3;
		while (PROLLY_CALL(tr, cursor_next, c, &key, &value) && steps < 50)
		{
			if (key_int(&key) != want)
				elog(ERROR, "forward from %d: got %d, expected %d",
					 seeks[s], key_int(&key), want);
			want += 3;
			steps++;
		}
		if (steps == 0 && seeks[s] <= 19998)
			elog(ERROR, "forward from %d found nothing", seeks[s]);

		/* Backward: the last key <= target, then descending. */
		PROLLY_CALL(tr, cursor_seek, c, &target, BackwardScanDirection);
		want = Min(seeks[s], 19998);
		want -= want % 3;
		steps = 0;
		while (PROLLY_CALL(tr, cursor_next, c, &key, &value) && steps < 50)
		{
			if (key_int(&key) != want)
				elog(ERROR, "backward from %d: got %d, expected %d",
					 seeks[s], key_int(&key), want);
			want -= 3;
			steps++;
		}
	}

	/* Whole scans both ways. */
	{
		int			n = 0;

		PROLLY_CALL(tr, cursor_seek, c, NULL, BackwardScanDirection);
		while (PROLLY_CALL(tr, cursor_next, c, &key, &value))
		{
			if (key_int(&key) != 19998 - 3 * n)
				elog(ERROR, "backward scan: got %d at step %d", key_int(&key), n);
			n++;
		}
		if (n != 6667)
			elog(ERROR, "backward scan found %d keys, expected 6667", n);
	}
	PROLLY_CALL(tr, cursor_close, c);

	/* The empty tree. */
	memset(ref, 0xff, sizeof(Ref));	/* every version ABSENT (-1) */
	build_ref(tr, &env, ref, &empty);
	c = PROLLY_CALL(tr, cursor_open, &env, &empty, CurrentMemoryContext);
	PROLLY_CALL(tr, cursor_seek, c, NULL, ForwardScanDirection);
	if (PROLLY_CALL(tr, cursor_next, c, &key, &value))
		elog(ERROR, "the empty tree returned an item");
	PROLLY_CALL(tr, cursor_seek, c, NULL, BackwardScanDirection);
	if (PROLLY_CALL(tr, cursor_next, c, &key, &value))
		elog(ERROR, "the empty tree returned an item scanning backward");
	PROLLY_CALL(tr, cursor_close, c);
	if (PROLLY_CALL(tr, count, &env, &empty) != 0)
		elog(ERROR, "the empty tree has a nonzero count");
}

void
tree_apply_matches_reference(const ProllyImplInfo *impl)
{
	const ProllyTreeRoutine *tr = (const ProllyTreeRoutine *) impl;
	ProllyTreeEnv env;
	Ref		   *ref = palloc(sizeof(Ref));
	ProllyNodeRef root;
	pg_prng_state rng;
	int		   *keys = palloc(sizeof(int) * MAX_KEY);
	int		   *versions = palloc(sizeof(int) * MAX_KEY);

	make_env(tr, &env);
	pg_prng_seed(&rng, 20261013);
	fill_ref(ref, 2, 1);
	build_ref(tr, &env, ref, &root);

	for (int batch = 0; batch < 60; batch++)
	{
		/* Small batches mostly, sometimes large, sometimes in one region. */
		int			range = batch % 7 == 0 ? 500 : MAX_KEY;
		int			maxn = batch % 5 == 0 ? 2000 : 40;
		int			n = random_batch(&rng, maxn, range, batch + 2, keys, versions);

		CHECK_FOR_INTERRUPTS();
		apply_batch(tr, &env, ref, keys, versions, n, &root);
		check_matches(tr, &env, &root, ref, psprintf("after batch %d", batch));
	}

	/* Delete everything, in one batch. */
	{
		int			n = 0;

		for (int k = 0; k < MAX_KEY; k++)
		{
			keys[n] = k;
			versions[n] = ABSENT;
			n++;
		}
		apply_batch(tr, &env, ref, keys, versions, n, &root);
		check_matches(tr, &env, &root, ref, "after deleting everything");
	}
}

/*
 * The first key k >= min_key where a leaf ends on a chunk boundary (not a
 * forced split) when keys 0 .. k with version 1 are chunked, so a tree of
 * exactly those keys has a last leaf the chunker leaves clean.
 */
static int
leaf_boundary_key(const ProllyTreeEnv *env, int min_key)
{
	Page		page = palloc_aligned(BLCKSZ, PG_IO_ALIGN_SIZE, MCXT_ALLOC_ZERO);
	ProllyChunker *ch;
	StringInfoData vbuf;
	int			found = -1;

	PROLLY_CALL(env->node_format, page_init, page, BLCKSZ, 0);
	ch = PROLLY_CALL(env->chunker, begin, 0,
					 PROLLY_CALL(env->node_format, free_space, page),
					 CurrentMemoryContext);
	initStringInfo(&vbuf);
	for (int k = 0; k < MAX_KEY - 3 && found < 0; k++)
	{
		KeyBuf		kb;
		ProllyKey	key = key_of(k, &kb);
		ProllyValue value = value_of(k, 1, &vbuf);
		Size		space = PROLLY_CALL(env->node_format, item_space, 0, &key, &value);

		if (PROLLY_CALL(env->chunker, add, ch, &key, &value, space) ==
			PROLLY_CHUNK_SPLIT_AFTER && k >= min_key)
			found = k;
	}
	PROLLY_CALL(env->chunker, end, ch);
	pfree(vbuf.data);
	pfree(page);
	if (found < 0)
		elog(ERROR, "no leaf boundary at or after key %d", min_key);
	return found;
}

/*
 * Edits past the last key, when the last leaf ends on a chunk boundary.
 * Re-chunking that leaf leaves the chunker clean at the end of the level,
 * and the edits after it must still be applied there (this once looped
 * forever).
 */
void
tree_apply_past_the_end(const ProllyImplInfo *impl)
{
	const ProllyTreeRoutine *tr = (const ProllyTreeRoutine *) impl;
	ProllyTreeEnv env;
	Ref		   *ref = palloc(sizeof(Ref));
	ProllyNodeRef base;
	ProllyNodeRef root;
	int			last;

	make_env(tr, &env);
	last = leaf_boundary_key(&env, 500);	/* several leaves, two levels */
	memset(ref, 0xff, sizeof(Ref));
	for (int k = 0; k <= last; k++)
		ref->version[k] = 1;
	build_ref(tr, &env, ref, &base);

	/* Deleting an absent key past the end changes nothing. */
	{
		int			keys[1] = {last + 1};
		int			versions[1] = {ABSENT};

		root = base;
		apply_batch(tr, &env, ref, keys, versions, 1, &root);
		check_matches(tr, &env, &root, ref, "delete past the end");
		if (!same_hash(&root, &base))
			elog(ERROR, "deleting an absent key changed the root hash");
	}

	/* Putting keys past the end appends them. */
	{
		int			keys[2] = {last + 1, last + 2};
		int			versions[2] = {1, 1};

		root = base;
		apply_batch(tr, &env, ref, keys, versions, 2, &root);
		check_matches(tr, &env, &root, ref, "put past the end");
	}

	/* An edit inside the tree and one past the end, in one batch. */
	{
		int			keys[2] = {last / 2, last + 3};
		int			versions[2] = {2, 1};

		apply_batch(tr, &env, ref, keys, versions, 2, &root);
		check_matches(tr, &env, &root, ref, "edit inside and past the end");
	}
}

void
tree_history_independent(const ProllyImplInfo *impl)
{
	const ProllyTreeRoutine *tr = (const ProllyTreeRoutine *) impl;
	ProllyTreeEnv env;
	Ref		   *target = palloc(sizeof(Ref));
	Ref		   *scratch = palloc(sizeof(Ref));
	ProllyNodeRef built;
	ProllyNodeRef empty;
	int		   *keys = palloc(sizeof(int) * MAX_KEY);
	int		   *versions = palloc(sizeof(int) * MAX_KEY);
	pg_prng_state rng;

	make_env(tr, &env);

	/* The target state: a mix of keys and versions. */
	pg_prng_seed(&rng, 42);
	for (int k = 0; k < MAX_KEY; k++)
		target->version[k] = pg_prng_uint64_range(&rng, 0, 2) == 0 ? ABSENT :
			(int) pg_prng_uint64_range(&rng, 1, 5);
	build_ref(tr, &env, target, &built);

	memset(scratch, 0xff, sizeof(Ref));
	build_ref(tr, &env, scratch, &empty);

	for (int history = 0; history < 4; history++)
	{
		ProllyNodeRef root = empty;

		memset(scratch, 0xff, sizeof(Ref));
		pg_prng_seed(&rng, 1000 + history);

		/* Random churn: other keys and versions come and go. */
		for (int batch = 0; batch < 20; batch++)
		{
			int			n = random_batch(&rng, history == 0 ? 3000 : 300,
										 MAX_KEY, 7 + batch, keys, versions);

			apply_batch(tr, &env, scratch, keys, versions, n, &root);
		}

		/* Then fix every key to the target, in batches of varying size. */
		{
			int			k = history % 2 == 0 ? 0 : MAX_KEY - 1;
			int			step = history % 2 == 0 ? 1 : -1;
			int			done = 0;

			while (done < MAX_KEY)
			{
				int			size = (int) pg_prng_uint64_range(&rng, 1, 700);
				int			n = 0;

				for (int j = 0; j < size && done < MAX_KEY; j++, done++, k += step)
				{
					keys[n] = k;
					versions[n] = target->version[k];
					n++;
				}
				/* Batches must be in key order. */
				if (step < 0)
				{
					for (int a = 0, b = n - 1; a < b; a++, b--)
					{
						int			tk = keys[a],
									tv = versions[a];

						keys[a] = keys[b];
						versions[a] = versions[b];
						keys[b] = tk;
						versions[b] = tv;
					}
				}
				apply_batch(tr, &env, scratch, keys, versions, n, &root);
			}
		}

		check_matches(tr, &env, &root, target, psprintf("history %d", history));
		if (!same_hash(&root, &built))
			elog(ERROR, "history %d reached the same items with a different root hash",
				 history);
	}

	/* Emptying a tree gives the empty tree's hash. */
	{
		ProllyNodeRef root = built;
		int			n = 0;

		for (int k = 0; k < MAX_KEY; k++)
		{
			keys[n] = k;
			versions[n] = ABSENT;
			n++;
		}
		apply_batch(tr, &env, scratch, keys, versions, n, &root);
		if (!same_hash(&root, &empty))
			elog(ERROR, "deleting every key did not give the empty tree's hash");
	}
}

/* Collects node hashes during a walk. */
typedef struct HashSet
{
	ProllyHash *hashes;
	int			n;
	int			max;
	int			levels;
} HashSet;

static void
collect_node(const ProllyNodeRef *node, uint16 level, void *arg)
{
	HashSet    *s = arg;

	if (s->n == s->max)
	{
		s->max = Max(64, s->max * 2);
		s->hashes = s->hashes ? repalloc(s->hashes, sizeof(ProllyHash) * s->max) :
			palloc(sizeof(ProllyHash) * s->max);
	}
	s->hashes[s->n++] = node->hash;
	s->levels = Max(s->levels, level + 1);
}

static bool
in_set(const HashSet *s, const ProllyHash *h)
{
	for (int i = 0; i < s->n; i++)
		if (s->hashes[i].len == h->len && memcmp(s->hashes[i].bytes, h->bytes, h->len) == 0)
			return true;
	return false;
}

void
tree_edits_share_nodes(const ProllyImplInfo *impl)
{
	const ProllyTreeRoutine *tr = (const ProllyTreeRoutine *) impl;
	ProllyTreeEnv env;
	Ref		   *ref = palloc(sizeof(Ref));
	ProllyNodeRef before;
	ProllyNodeRef after;
	HashSet		a = {0},
				b = {0};
	int			fresh = 0;
	int			keys[1] = {10001};
	int			versions[1] = {99};

	make_env(tr, &env);
	fill_ref(ref, 1, 1);
	build_ref(tr, &env, ref, &before);
	after = before;
	apply_batch(tr, &env, ref, keys, versions, 1, &after);

	PROLLY_CALL(tr, walk, &env, &before, collect_node, &a);
	PROLLY_CALL(tr, walk, &env, &after, collect_node, &b);
	for (int i = 0; i < b.n; i++)
		if (!in_set(&a, &b.hashes[i]))
			fresh++;

	/* One changed value: a new path, plus a little re-chunking around it. */
	if (fresh == 0)
		elog(ERROR, "changing a value made no new nodes");
	if (fresh > 2 * b.levels + 2)
		elog(ERROR, "changing one value made %d new nodes in a %d-level tree of %d nodes",
			 fresh, b.levels, b.n);
	if (b.n < 10)
		elog(ERROR, "test tree has only %d nodes", b.n);
}

typedef struct DiffSeen
{
	const Ref  *from;
	const Ref  *to;
	int			last;
	int			changes;
} DiffSeen;

static void
check_change(ProllyDiffKind kind, const ProllyKey *key, const ProllyValue *before,
			 const ProllyValue *after, void *arg)
{
	DiffSeen   *s = arg;
	int			k = key_int(key);
	int			fv = s->from->version[k];
	int			tv = s->to->version[k];

	if (k <= s->last)
		elog(ERROR, "diff reported key %d after key %d", k, s->last);
	s->last = k;
	s->changes++;
	switch (kind)
	{
		case PROLLY_DIFF_ADDED:
			if (fv != ABSENT || tv == ABSENT || before != NULL || !same_value(after, k, tv))
				elog(ERROR, "diff: key %d wrongly reported added", k);
			break;
		case PROLLY_DIFF_REMOVED:
			if (fv == ABSENT || tv != ABSENT || after != NULL || !same_value(before, k, fv))
				elog(ERROR, "diff: key %d wrongly reported removed", k);
			break;
		case PROLLY_DIFF_MODIFIED:
			if (fv == ABSENT || tv == ABSENT || fv == tv ||
				!same_value(before, k, fv) || !same_value(after, k, tv))
				elog(ERROR, "diff: key %d wrongly reported modified", k);
			break;
	}
}

void
tree_diff_reports_changes(const ProllyImplInfo *impl)
{
	const ProllyTreeRoutine *tr = (const ProllyTreeRoutine *) impl;
	ProllyTreeEnv env;
	Ref		   *from = palloc(sizeof(Ref));
	Ref		   *to = palloc(sizeof(Ref));
	ProllyNodeRef rfrom;
	ProllyNodeRef rto;
	pg_prng_state rng;
	int		   *keys = palloc(sizeof(int) * MAX_KEY);
	int		   *versions = palloc(sizeof(int) * MAX_KEY);

	make_env(tr, &env);
	fill_ref(from, 2, 1);
	build_ref(tr, &env, from, &rfrom);
	memcpy(to, from, sizeof(Ref));
	rto = rfrom;
	pg_prng_seed(&rng, 7);

	for (int round = 0; round < 5; round++)
	{
		int			n = random_batch(&rng, 60, MAX_KEY, 10 + round, keys, versions);
		DiffSeen	seen = {from, to, -1, 0};
		int			want = 0;

		apply_batch(tr, &env, to, keys, versions, n, &rto);
		for (int k = 0; k < MAX_KEY; k++)
			if (from->version[k] != to->version[k])
				want++;
		PROLLY_CALL(tr, diff, &env, &rfrom, &rto, check_change, &seen);
		if (seen.changes != want)
			elog(ERROR, "diff reported %d changes, expected %d", seen.changes, want);
	}

	/* A tree has no differences from itself. */
	{
		DiffSeen	seen = {to, to, -1, 0};

		PROLLY_CALL(tr, diff, &env, &rto, &rto, check_change, &seen);
		if (seen.changes != 0)
			elog(ERROR, "a tree differs from itself");
	}
}

/* Every node a tree is made of passes the node format's structure check. */
typedef struct PageCheck
{
	const ProllyTreeEnv *env;
	int			nodes;
} PageCheck;

static void
check_node_page(const ProllyNodeRef *node, uint16 level, void *arg)
{
	PageCheck  *pc = arg;
	Buffer		buf = PROLLY_CALL(pc->env->node_store, read, pc->env->store, node);
	char	   *detail = NULL;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	if (!PROLLY_CALL(pc->env->node_format, page_check, BufferGetPage(buf), &detail))
		elog(ERROR, "node at level %u fails the page check: %s", level, detail);
	if (PROLLY_CALL(pc->env->node_format, page_level, BufferGetPage(buf)) != level)
		elog(ERROR, "node visited at level %u says it is at level %u", level,
			 PROLLY_CALL(pc->env->node_format, page_level, BufferGetPage(buf)));
	UnlockReleaseBuffer(buf);
	pc->nodes++;
}

void
tree_nodes_are_valid(const ProllyImplInfo *impl)
{
	const ProllyTreeRoutine *tr = (const ProllyTreeRoutine *) impl;
	ProllyTreeEnv env;
	Ref		   *ref = palloc(sizeof(Ref));
	ProllyNodeRef root;
	PageCheck	pc = {&env, 0};

	make_env(tr, &env);
	fill_ref(ref, 1, 4);			/* every key, longer values */
	build_ref(tr, &env, ref, &root);
	PROLLY_CALL(tr, walk, &env, &root, check_node_page, &pc);
	if (pc.nodes < 10)
		elog(ERROR, "tree has only %d nodes", pc.nodes);
}
