/*--------------------------------------------------------------------------
 *
 * tree_checks.c
 *		Conformance checks for the tree interface.
 *
 * Trees are built as chunks in the database's chunk store (basic chunk
 * format, byte chunker, SHA-256), and compared against an in-memory
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

#include "access/prolly_datum.h"
#include "access/xact.h"
#include "catalog/pg_class.h"
#include "common/pg_prng.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
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
make_env(ProllyTreeEnv *env)
{
	memset(env, 0, sizeof(*env));
	env->chunk_format = prolly_chunkformat_lookup("basic");
	env->chunker = prolly_chunker_lookup("bytes");
	env->hash = prolly_hash_lookup("sha256");
	env->chunk_store = prolly_chunk_store();
	env->chunks = PROLLY_CALL(env->chunk_store, open, MyDatabaseId,
							  MyDatabaseTableSpace);
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

/*
 * Every item of the tree, in order, matches ref; so does count.  The
 * cursor lives in a context of its own, opened, sought and advanced while
 * a shorter one is current, reset after every step: what it keeps between
 * calls must be in its own.
 */
static void
check_matches(const ProllyTreeRoutine *tr, const ProllyTreeEnv *env,
			  const ProllyNodeRef *root, const Ref *ref, const char *when)
{
	MemoryContext outer = CurrentMemoryContext;
	MemoryContext ccxt = AllocSetContextCreate(outer, "tree check cursor",
											   ALLOCSET_DEFAULT_SIZES);
	MemoryContext step = AllocSetContextCreate(outer, "tree check step",
											   ALLOCSET_DEFAULT_SIZES);
	ProllyTreeCursor *c;
	ProllyKey	key;
	ProllyValue value;
	int			next = 0;
	uint64		n = 0;

	MemoryContextSwitchTo(step);
	c = PROLLY_CALL(tr, cursor_open, env, root, ccxt);
	PROLLY_CALL(tr, cursor_seek, c, NULL, ForwardScanDirection);
	MemoryContextReset(step);
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
		MemoryContextReset(step);
	}
	MemoryContextSwitchTo(outer);
	PROLLY_CALL(tr, cursor_close, c);
	MemoryContextDelete(step);
	MemoryContextDelete(ccxt);
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

	make_env(&env);
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

	make_env(&env);
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

	make_env(&env);
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
	ProllyChunker *ch;
	StringInfoData vbuf;
	int			found = -1;
	Size		capacity = 64 * 1024 - PROLLY_CALL(env->chunk_format, header_size);

	ch = PROLLY_CALL(env->chunker, begin, 0, capacity, CurrentMemoryContext);
	initStringInfo(&vbuf);
	for (int k = 0; k < MAX_KEY - 3 && found < 0; k++)
	{
		KeyBuf		kb;
		ProllyKey	key = key_of(k, &kb);
		ProllyValue value = value_of(k, 1, &vbuf);
		Size		space = PROLLY_CALL(env->chunk_format, item_space, 0, &key,
										&value);

		if (PROLLY_CALL(env->chunker, add, ch, &key, &value, space) ==
			PROLLY_CHUNK_SPLIT_AFTER && k >= min_key)
			found = k;
	}
	PROLLY_CALL(env->chunker, end, ch);
	pfree(vbuf.data);
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

	make_env(&env);
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

	make_env(&env);

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

static bool
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
	return true;
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

	make_env(&env);
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
		case PROLLY_DIFF_NOT_COVERED:
			/* (a kind of table a read leaves out, never a tree's) */
			elog(ERROR, "diff: key %d reported as not covered", k);
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

	make_env(&env);
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

/* Counts changes and checks their order (diffs of unrelated trees). */
static void
count_change(ProllyDiffKind kind, const ProllyKey *key, const ProllyValue *before,
			 const ProllyValue *after, void *arg)
{
	DiffSeen   *s = arg;
	int			k = key_int(key);

	if (k <= s->last)
		elog(ERROR, "diff reported key %d after key %d", k, s->last);
	s->last = k;
	s->changes++;
}

/*
 * The counters count every node read and every new node written: building
 * a tree writes each of its nodes, building it again reads and writes none,
 * and a walk reads each once.
 */
void
tree_counts_nodes(const ProllyImplInfo *impl)
{
	const ProllyTreeRoutine *tr = (const ProllyTreeRoutine *) impl;
	ProllyTreeEnv env;
	ProllyNodeCounters cnt = {0};
	Ref		   *ref = palloc(sizeof(Ref));
	ProllyNodeRef root;
	ProllyNodeRef again;
	HashSet		nodes = {0};
	HashSet		walked = {0};

	make_env(&env);
	/* Values not in the store yet, even when the check runs again. */
	fill_ref(ref, 1, 1000 + (int) pg_prng_uint64_range(&pg_global_prng_state, 0, 1000000));
	env.counters = &cnt;
	build_ref(tr, &env, ref, &root);
	env.counters = NULL;
	PROLLY_CALL(tr, walk, &env, &root, collect_node, &nodes);
	if (cnt.nodes_written != (uint64) nodes.n)
		elog(ERROR, "building a tree of %d nodes counted %llu written", nodes.n,
			 (unsigned long long) cnt.nodes_written);

	memset(&cnt, 0, sizeof(cnt));
	env.counters = &cnt;
	build_ref(tr, &env, ref, &again);
	if (!same_hash(&root, &again) || cnt.nodes_written != 0)
		elog(ERROR, "building a stored tree again counted %llu nodes written",
			 (unsigned long long) cnt.nodes_written);
	/* The store says where a stored node is; reusing it reads nothing. */
	if (cnt.nodes_read != 0)
		elog(ERROR, "building a stored tree of %d nodes again counted %llu read",
			 nodes.n, (unsigned long long) cnt.nodes_read);

	memset(&cnt, 0, sizeof(cnt));
	PROLLY_CALL(tr, walk, &env, &root, collect_node, &walked);
	if (cnt.nodes_read != (uint64) nodes.n || cnt.nodes_written != 0)
		elog(ERROR, "walking a tree of %d nodes counted %llu read, %llu written",
			 nodes.n, (unsigned long long) cnt.nodes_read,
			 (unsigned long long) cnt.nodes_written);
}

/*
 * A cursor seeking keys in order reads each node about once, as a scan
 * does: a seek keeps the nodes of the path it already holds.  Seeking every
 * key forward, then every key backward, each reads no more than the tree's
 * nodes plus a path.
 */
void
tree_seeks_in_order(const ProllyImplInfo *impl)
{
	const ProllyTreeRoutine *tr = (const ProllyTreeRoutine *) impl;
	ProllyTreeEnv env;
	Ref		   *ref = palloc(sizeof(Ref));
	ProllyNodeRef root;
	HashSet		nodes = {0};
	ProllyNodeCounters cnt = {0};
	ScanDirection dirs[] = {ForwardScanDirection, BackwardScanDirection};

	make_env(&env);
	fill_ref(ref, 1, 1);
	build_ref(tr, &env, ref, &root);
	PROLLY_CALL(tr, walk, &env, &root, collect_node, &nodes);

	for (int t = 0; t < lengthof(dirs); t++)
	{
		bool		forward = ScanDirectionIsForward(dirs[t]);
		ProllyTreeCursor *c;
		uint64		bound = (uint64) nodes.n + nodes.levels;

		memset(&cnt, 0, sizeof(cnt));
		env.counters = &cnt;
		c = PROLLY_CALL(tr, cursor_open, &env, &root, CurrentMemoryContext);
		for (int i = 0; i < MAX_KEY; i++)
		{
			int			k = forward ? i : MAX_KEY - 1 - i;
			KeyBuf		kb;
			ProllyKey	target = key_of(k, &kb);
			ProllyKey	key;
			ProllyValue value;

			PROLLY_CALL(tr, cursor_seek, c, &target, dirs[t]);
			if (!PROLLY_CALL(tr, cursor_next, c, &key, &value) ||
				key_int(&key) != k)
				elog(ERROR, "seeking key %d did not find it", k);
		}
		PROLLY_CALL(tr, cursor_close, c);
		env.counters = NULL;
		if (cnt.nodes_read > bound)
			elog(ERROR, "seeking %d keys %s in a tree of %d nodes read %llu nodes, more than %llu",
				 MAX_KEY, forward ? "forward" : "backward", nodes.n,
				 (unsigned long long) cnt.nodes_read,
				 (unsigned long long) bound);
	}
}

/*
 * Diff reads only what changed: with d changed keys spread over the tree,
 * it reads O(d log n) nodes, a few root-to-leaf paths per change, never
 * every leaf.  A tree diffed against itself reads nothing, and against the
 * empty tree reads each node of the other once.
 */
void
tree_diff_reads_changed_paths(const ProllyImplInfo *impl)
{
	const ProllyTreeRoutine *tr = (const ProllyTreeRoutine *) impl;
	ProllyTreeEnv env;
	Ref		   *from = palloc(sizeof(Ref));
	Ref		   *to = palloc(sizeof(Ref));
	Ref		   *none = palloc(sizeof(Ref));
	ProllyNodeRef rfrom;
	ProllyNodeRef rempty;
	HashSet		nodes = {0};
	ProllyNodeCounters cnt = {0};
	int			ds[] = {1, 2, 4, 8};

	make_env(&env);
	fill_ref(from, 1, 1);
	build_ref(tr, &env, from, &rfrom);
	PROLLY_CALL(tr, walk, &env, &rfrom, collect_node, &nodes);
	if (nodes.levels < 3 || nodes.n < 200)
		elog(ERROR, "test tree has only %d nodes in %d levels", nodes.n, nodes.levels);

	for (int t = 0; t < lengthof(ds); t++)
	{
		int			d = ds[t];
		int			keys[8];
		int			versions[8];
		ProllyNodeRef rto = rfrom;
		DiffSeen	seen = {from, to, -1, 0};
		uint64		bound = (uint64) d * (4 * nodes.levels + 4);

		memcpy(to, from, sizeof(Ref));
		for (int i = 0; i < d; i++)
		{
			keys[i] = i * (MAX_KEY / d) + 17;
			/* A modification, a removal, a modification, ... */
			versions[i] = i % 2 == 0 ? 5 : ABSENT;
		}
		apply_batch(tr, &env, to, keys, versions, d, &rto);

		memset(&cnt, 0, sizeof(cnt));
		env.counters = &cnt;
		PROLLY_CALL(tr, diff, &env, &rfrom, &rto, check_change, &seen);
		env.counters = NULL;
		if (seen.changes != d)
			elog(ERROR, "diff reported %d changes, expected %d", seen.changes, d);
		if (cnt.nodes_read > bound)
			elog(ERROR, "diff of %d changes in a %d-level tree of %d nodes read %llu nodes, more than %llu",
				 d, nodes.levels, nodes.n, (unsigned long long) cnt.nodes_read,
				 (unsigned long long) bound);
		if (cnt.nodes_written != 0)
			elog(ERROR, "diff wrote nodes");
	}

	/* Against itself: nothing read. */
	memset(&cnt, 0, sizeof(cnt));
	env.counters = &cnt;
	{
		DiffSeen	seen = {from, from, -1, 0};

		PROLLY_CALL(tr, diff, &env, &rfrom, &rfrom, check_change, &seen);
		if (seen.changes != 0 || cnt.nodes_read != 0)
			elog(ERROR, "a tree diffed against itself reported %d changes and read %llu nodes",
				 seen.changes, (unsigned long long) cnt.nodes_read);
	}

	/* Against the empty tree, both ways: every key, every node once. */
	fill_ref(none, 1, ABSENT);
	build_ref(tr, &env, none, &rempty);
	for (int way = 0; way < 2; way++)
	{
		DiffSeen	seen = {way == 0 ? none : from, way == 0 ? from : none, -1, 0};

		memset(&cnt, 0, sizeof(cnt));
		PROLLY_CALL(tr, diff, &env, way == 0 ? &rempty : &rfrom,
					way == 0 ? &rfrom : &rempty, check_change, &seen);
		if (seen.changes != MAX_KEY)
			elog(ERROR, "diff with the empty tree reported %d changes, expected %d",
				 seen.changes, MAX_KEY);
		if (cnt.nodes_read != (uint64) nodes.n + 1)
			elog(ERROR, "diff with the empty tree read %llu nodes, expected %d",
				 (unsigned long long) cnt.nodes_read, nodes.n + 1);
	}
	env.counters = NULL;
}

/*
 * Diff of trees that share nothing, and of trees whose heights differ: a
 * small tree against a large one, both ways, against a reference merge.
 */
void
tree_diff_across_heights(const ProllyImplInfo *impl)
{
	const ProllyTreeRoutine *tr = (const ProllyTreeRoutine *) impl;
	ProllyTreeEnv env;
	Ref		   *small = palloc(sizeof(Ref));
	Ref		   *large = palloc(sizeof(Ref));
	ProllyNodeRef rsmall;
	ProllyNodeRef rlarge;
	HashSet		hs = {0};
	HashSet		hl = {0};

	make_env(&env);
	fill_ref(large, 1, 2);
	/* A few keys, some in large with the same value, some with another. */
	fill_ref(small, 1, ABSENT);
	for (int k = 0; k < MAX_KEY; k += 997)
		small->version[k] = (k / 997) % 2 == 0 ? 2 : 3;
	small->version[MAX_KEY - 1] = 2;
	build_ref(tr, &env, small, &rsmall);
	build_ref(tr, &env, large, &rlarge);
	PROLLY_CALL(tr, walk, &env, &rsmall, collect_node, &hs);
	PROLLY_CALL(tr, walk, &env, &rlarge, collect_node, &hl);
	if (hs.levels >= hl.levels)
		elog(ERROR, "test trees have the same height (%d)", hs.levels);

	for (int way = 0; way < 2; way++)
	{
		Ref		   *rf = way == 0 ? small : large;
		Ref		   *rt = way == 0 ? large : small;
		DiffSeen	seen = {rf, rt, -1, 0};
		int			want = 0;

		for (int k = 0; k < MAX_KEY; k++)
			if (rf->version[k] != rt->version[k])
				want++;
		PROLLY_CALL(tr, diff, &env, way == 0 ? &rsmall : &rlarge,
					way == 0 ? &rlarge : &rsmall, check_change, &seen);
		if (seen.changes != want)
			elog(ERROR, "diff across heights reported %d changes, expected %d",
				 seen.changes, want);
	}

	/* Two unrelated trees: every key of both, in order. */
	{
		Ref		   *odd = palloc(sizeof(Ref));
		ProllyNodeRef rodd;
		DiffSeen	seen = {large, odd, -1, 0};

		for (int k = 0; k < MAX_KEY; k++)
			odd->version[k] = k % 2 == 1 ? 7 : ABSENT;
		build_ref(tr, &env, odd, &rodd);
		PROLLY_CALL(tr, diff, &env, &rlarge, &rodd, count_change, &seen);
		if (seen.changes != MAX_KEY)
			elog(ERROR, "diff of unrelated trees reported %d changes, expected %d",
				 seen.changes, MAX_KEY);
	}
}

/*
 * Keys of five bytes whose order ignores the last (a "spelling"), as a
 * collation can find 'a' and 'A' equal.
 */
static int
spelled_compare(const ProllyKey *a, const ProllyKey *b, void *arg)
{
	return memcmp(a->data, b->data, 4);
}

static ProllyKey
spelled_key(int k, uint8 spelling, uint8 *buf)
{
	ProllyKey	key;

	buf[0] = (uint8) (k >> 24);
	buf[1] = (uint8) (k >> 16);
	buf[2] = (uint8) (k >> 8);
	buf[3] = (uint8) k;
	buf[4] = spelling;
	key.data = buf;
	key.len = 5;
	return key;
}

/* Keys 0..n-1, spelled 0 except key respelled, spelled 1. */
static void
build_spelled(const ProllyTreeRoutine *tr, const ProllyTreeEnv *env, int n,
			  int respelled, ProllyNodeRef *root)
{
	ProllyTreeBuilder *b = PROLLY_CALL(tr, build_begin, env, CurrentMemoryContext);
	ProllyValue v = {(const uint8 *) "v", 1};

	for (int k = 0; k < n; k++)
	{
		uint8		buf[5];
		ProllyKey	key = spelled_key(k, k == respelled ? 1 : 0, buf);

		PROLLY_CALL(tr, build_add, b, &key, &v);
	}
	PROLLY_CALL(tr, build_finish, b, root);
}

typedef struct SpelledDiff
{
	int			changes;
	bool		right;
} SpelledDiff;

static void
check_spelled_change(ProllyDiffKind kind, const ProllyKey *key,
					 const ProllyValue *before, const ProllyValue *after,
					 void *arg)
{
	SpelledDiff *s = arg;

	s->changes++;
	s->right = kind == PROLLY_DIFF_MODIFIED && key->len == 5 &&
		key->data[3] == 77 && key->data[4] == 1;
}

/*
 * A put of a key equal by the order to one in the tree but written
 * differently replaces it with the new bytes: the tree is the one built
 * with them, cursors return them, and diff reports the change.
 */
void
tree_equal_keys_take_new_bytes(const ProllyImplInfo *impl)
{
	const ProllyTreeRoutine *tr = (const ProllyTreeRoutine *) impl;
	ProllyTreeEnv env;
	ProllyNodeRef before;
	ProllyNodeRef after;
	ProllyNodeRef built;
	ProllyEdit	edit;
	uint8		buf[5];
	ProllyTreeCursor *c;
	ProllyKey	key;
	ProllyValue value;
	SpelledDiff seen = {0, false};
	int			n = 3000;

	make_env(&env);
	env.compare = spelled_compare;
	build_spelled(tr, &env, n, -1, &before);
	build_spelled(tr, &env, n, 77, &built);

	memset(&edit, 0, sizeof(edit));
	edit.key = spelled_key(77, 1, buf);
	edit.value.data = (const uint8 *) "v";
	edit.value.len = 1;
	PROLLY_CALL(tr, apply, &env, &before, &edit, 1, &after);
	if (after.hash.len != built.hash.len ||
		memcmp(after.hash.bytes, built.hash.bytes, built.hash.len) != 0)
		elog(ERROR, "respelling a key gave a different tree from building it so");

	c = PROLLY_CALL(tr, cursor_open, &env, &after, CurrentMemoryContext);
	PROLLY_CALL(tr, cursor_seek, c, &edit.key, ForwardScanDirection);
	if (!PROLLY_CALL(tr, cursor_next, c, &key, &value) || key.len != 5 ||
		key.data[3] != 77 || key.data[4] != 1)
		elog(ERROR, "a respelled key reads back as it was written before");
	PROLLY_CALL(tr, cursor_close, c);

	PROLLY_CALL(tr, diff, &env, &before, &after, check_spelled_change, &seen);
	if (seen.changes != 1 || !seen.right)
		elog(ERROR, "diff reported %d changes for a respelled key, %s",
			 seen.changes, seen.right ? "as modified" : "not as a modification to it");
}

/* Every node a tree is made of is a chunk node at its level. */
typedef struct NodeCheck
{
	const ProllyTreeEnv *env;
	int			nodes;
} NodeCheck;

static bool
check_node(const ProllyNodeRef *node, uint16 level, void *arg)
{
	NodeCheck  *pc = arg;
	StringInfoData data;
	ProllyChunkNode cn;

	initStringInfo(&data);
	if (!PROLLY_CALL(pc->env->chunk_store, get, pc->env->chunks, &node->hash,
					 &data))
		elog(ERROR, "node at level %u is not in the chunk store", level);
	if (!PROLLY_CALL(pc->env->chunk_format, open, (const uint8 *) data.data,
					 data.len, &cn))
		elog(ERROR, "node at level %u is not a chunk node", level);
	if (cn.level != level)
		elog(ERROR, "node visited at level %u says it is at level %u",
			 level, cn.level);
	pfree(data.data);
	pc->nodes++;
	return true;
}

void
tree_nodes_are_valid(const ProllyImplInfo *impl)
{
	const ProllyTreeRoutine *tr = (const ProllyTreeRoutine *) impl;
	ProllyTreeEnv env;
	Ref		   *ref = palloc(sizeof(Ref));
	ProllyNodeRef root;
	NodeCheck	pc = {&env, 0};

	make_env(&env);
	fill_ref(ref, 1, 4);			/* every key, longer values */
	build_ref(tr, &env, ref, &root);
	PROLLY_CALL(tr, walk, &env, &root, check_node, &pc);
	if (pc.nodes < 10)
		elog(ERROR, "tree has only %d nodes", pc.nodes);
}

/*
 * Large values: a value longer than a node goes out of line, and comes
 * back whole from get, cursors and diff; the root hash still depends only
 * on the items (built at once, or by edits that wrote the values another
 * way); a small change to a large value writes a few nodes; and walks visit
 * the out-of-line values' nodes.
 */
#define BIG_KEYS	40
#define CHUNK_MIN_BYTES	512	/* the value trees' smallest chunk */

static void
big_value(int k, int version, StringInfo buf)
{
	int			len = (k % 4 == 0) ? 100 : 3000 + k * 7919 % 200000;
	uint64		x = (uint64) k * 2654435761u + version;

	resetStringInfo(buf);
	while (buf->len < len)
	{
		x = x * 6364136223846793005u + 1442695040888963407u;
		appendStringInfoChar(buf, (char) ('a' + (x >> 59)));
	}
	if (version > 0)
		buf->data[len / 2] = '#';
}

static bool
count_visit(const ProllyNodeRef *node, uint16 level, void *arg)
{
	(*(int *) arg)++;
	return true;
}

typedef struct BigDiff
{
	int			nchanges;
	int			key;
	bool		values_ok;
} BigDiff;

static void
big_diff_visit(ProllyDiffKind kind, const ProllyKey *key,
			   const ProllyValue *before, const ProllyValue *after, void *arg)
{
	BigDiff    *d = arg;
	StringInfoData want;

	d->nchanges++;
	d->key = key_int(key);
	initStringInfo(&want);
	big_value(d->key, 0, &want);
	d->values_ok = kind == PROLLY_DIFF_MODIFIED && before && after &&
		before->len == want.len && memcmp(before->data, want.data, want.len) == 0;
	big_value(d->key, 1, &want);
	d->values_ok &= after->len == want.len &&
		memcmp(after->data, want.data, want.len) == 0;
	pfree(want.data);
}

/* Whether reading [offset, offset + len) of ref fails as corrupted. */
static bool
value_read_corrupt(const ProllyTreeRoutine *tr, ProllyTreeEnv *env,
				   const ProllyValue *ref, uint64 offset, uint64 len)
{
	MemoryContext cxt = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	StringInfoData got;
	bool		corrupt = false;

	initStringInfo(&got);
	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(cxt);
	PG_TRY();
	{
		PROLLY_CALL(tr, value_read, env, ref, offset, len, &got);
		ReleaseCurrentSubTransaction();
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(cxt);
		edata = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		corrupt = edata->sqlerrcode == ERRCODE_DATA_CORRUPTED;
		FreeErrorData(edata);
	}
	PG_END_TRY();
	MemoryContextSwitchTo(cxt);
	CurrentResourceOwner = owner;
	pfree(got.data);
	return corrupt;
}

/*
 * A value tree on its own: a value of a few megabytes comes back whole and
 * by slices (any offset and length, past the end too), reading only the
 * nodes a slice needs; and inserting or deleting a byte in its middle
 * writes O(depth) nodes, not one per chunk after it.
 */
static void
value_trees(const ProllyTreeRoutine *tr, ProllyTreeEnv *env)
{
	ProllyNodeCounters counters = {0};
	StringInfoData val;
	StringInfoData ref;
	StringInfoData got;
	ProllyValue r;
	uint64		x = 42;
	static const uint64 offsets[] = {0, 1, 4095, 4096, 100000, 1999999, 2999999};
	static const uint64 lens[] = {0, 1, 7, 5000, 70000};

	initStringInfo(&val);
	while (val.len < 3000000)
	{
		x = x * 6364136223846793005u + 1442695040888963407u;
		appendStringInfoChar(&val, (char) (x >> 56));
	}
	initStringInfo(&ref);
	PROLLY_CALL(tr, value_write, env, (const uint8 *) val.data, val.len, &ref);
	r.data = (const uint8 *) ref.data;
	r.len = ref.len;

	initStringInfo(&got);
	PROLLY_CALL(tr, value_read, env, &r, 0, PG_UINT64_MAX, &got);
	if (got.len != val.len || memcmp(got.data, val.data, val.len) != 0)
		elog(ERROR, "a value tree of %d bytes read back otherwise", val.len);

	/* a reference is its root and length, and is made back from them */
	{
		ProllyHash	root;
		uint64		len;

		PROLLY_CALL(tr, value_root, &r, &root, &len);
		resetStringInfo(&got);
		PROLLY_CALL(tr, value_reference, &root, len, &got);
		if (len != (uint64) val.len || got.len != ref.len ||
			memcmp(got.data, ref.data, ref.len) != 0)
			elog(ERROR, "a value tree's reference isn't made back from its root and length");
	}

	for (int i = 0; i < lengthof(offsets); i++)
	{
		for (int j = 0; j < lengthof(lens); j++)
		{
			uint64		want = offsets[i] >= (uint64) val.len ? 0 :
				Min(lens[j], (uint64) val.len - offsets[i]);

			resetStringInfo(&got);
			counters.nodes_read = 0;
			env->counters = &counters;
			PROLLY_CALL(tr, value_read, env, &r, offsets[i], lens[j], &got);
			env->counters = NULL;
			if ((uint64) got.len != want ||
				memcmp(got.data, val.data + offsets[i], got.len) != 0)
				elog(ERROR, "a slice of %llu bytes at %llu read %d bytes, or others",
					 (unsigned long long) lens[j], (unsigned long long) offsets[i],
					 got.len);
			if (counters.nodes_read > 4 + lens[j] / CHUNK_MIN_BYTES)
				elog(ERROR, "a slice of %llu bytes at %llu read %llu nodes",
					 (unsigned long long) lens[j], (unsigned long long) offsets[i],
					 (unsigned long long) counters.nodes_read);
		}
	}
	resetStringInfo(&got);
	PROLLY_CALL(tr, value_read, env, &r, val.len, 10, &got);
	PROLLY_CALL(tr, value_read, env, &r, PG_UINT64_MAX - 1, 10, &got);
	if (got.len != 0)
		elog(ERROR, "a slice past a value's end read %d bytes", got.len);

	/* A reference saying fewer or more bytes than its tree holds. */
	for (int d = -1; d <= 1; d += 2)
	{
		StringInfoData bad;
		ProllyValue b;

		initStringInfo(&bad);
		appendBinaryStringInfo(&bad, ref.data, 1 + (uint8) ref.data[0]);
		prolly_put_varint(&bad, val.len + d);
		b.data = (const uint8 *) bad.data;
		b.len = bad.len;
		if (!value_read_corrupt(tr, env, &b, 0, PG_UINT64_MAX) ||
			!value_read_corrupt(tr, env, &b, 100, 10))
			elog(ERROR, "a value tree of %d bytes read through a reference saying %d",
				 val.len, val.len + d);
		pfree(bad.data);
	}

	/* A byte inserted in the middle, then deleted. */
	for (int pass = 0; pass < 2; pass++)
	{
		StringInfoData edited;
		StringInfoData eref;
		int			mid = val.len / 2;

		initStringInfo(&edited);
		appendBinaryStringInfo(&edited, val.data, mid);
		if (pass == 0)
			appendStringInfoChar(&edited, 'x');
		appendBinaryStringInfo(&edited, val.data + mid + (pass == 0 ? 0 : 1),
							   val.len - mid - (pass == 0 ? 0 : 1));
		initStringInfo(&eref);
		counters.nodes_written = 0;
		env->counters = &counters;
		PROLLY_CALL(tr, value_write, env, (const uint8 *) edited.data,
					edited.len, &eref);
		env->counters = NULL;
		if (counters.nodes_written > 8)
			elog(ERROR, "%s one byte in a %d-byte value wrote %llu nodes",
				 pass == 0 ? "inserting" : "deleting", val.len,
				 (unsigned long long) counters.nodes_written);
		pfree(edited.data);
		pfree(eref.data);
	}
	pfree(val.data);
	pfree(ref.data);
	pfree(got.data);
}

void
tree_large_values(const ProllyImplInfo *impl)
{
	const ProllyTreeRoutine *tr = (const ProllyTreeRoutine *) impl;
	ProllyTreeEnv env;
	ProllyNodeCounters counters = {0};
	ProllyTreeBuilder *b;
	ProllyNodeRef built;
	ProllyNodeRef edited;
	ProllyNodeRef changed;
	StringInfoData vbuf;
	ProllyTreeCursor *c;
	ProllyKey	key;
	ProllyValue value;
	int			nkeys = 0;
	int			nnodes = 0;
	BigDiff		d = {0};

	make_env(&env);
	initStringInfo(&vbuf);

	/* Built at once. */
	b = PROLLY_CALL(tr, build_begin, &env, CurrentMemoryContext);
	for (int k = 0; k < BIG_KEYS; k++)
	{
		KeyBuf		kb;

		key = key_of(k, &kb);
		big_value(k, 0, &vbuf);
		value.data = (const uint8 *) vbuf.data;
		value.len = vbuf.len;
		PROLLY_CALL(tr, build_add, b, &key, &value);
	}
	PROLLY_CALL(tr, build_finish, b, &built);

	/* By edits, each value first written as another, in two batches. */
	b = PROLLY_CALL(tr, build_begin, &env, CurrentMemoryContext);
	PROLLY_CALL(tr, build_finish, b, &edited);
	for (int pass = 0; pass < 2; pass++)
	{
		for (int half = 0; half < 2; half++)
		{
			ProllyEdit	edits[BIG_KEYS];
			KeyBuf		kbs[BIG_KEYS];
			StringInfoData bufs[BIG_KEYS];
			int			n = 0;

			for (int k = half; k < BIG_KEYS; k += 2)
			{
				initStringInfo(&bufs[n]);
				big_value(k, pass == 0 ? 1 : 0, &bufs[n]);
				edits[n].key = key_of(k, &kbs[n]);
				edits[n].value.data = (const uint8 *) bufs[n].data;
				edits[n].value.len = bufs[n].len;
				edits[n].is_delete = false;
				n++;
			}
			PROLLY_CALL(tr, apply, &env, &edited, edits, n, &edited);
		}
	}
	if (!same_hash(&built, &edited))
		elog(ERROR, "a tree of large values built at once and by edits differ");

	/* Every value comes back whole, from get and from a cursor. */
	for (int k = 0; k < BIG_KEYS; k++)
	{
		KeyBuf		kb;
		StringInfoData got;

		key = key_of(k, &kb);
		initStringInfo(&got);
		big_value(k, 0, &vbuf);
		if (!PROLLY_CALL(tr, get, &env, &built, &key, &got) ||
			got.len != vbuf.len || memcmp(got.data, vbuf.data, got.len) != 0)
			elog(ERROR, "get of key %d returned another value", k);
		pfree(got.data);
	}
	c = PROLLY_CALL(tr, cursor_open, &env, &built, CurrentMemoryContext);
	PROLLY_CALL(tr, cursor_seek, c, NULL, ForwardScanDirection);
	while (PROLLY_CALL(tr, cursor_next, c, &key, &value))
	{
		big_value(key_int(&key), 0, &vbuf);
		if (value.len != vbuf.len || memcmp(value.data, vbuf.data, value.len) != 0)
			elog(ERROR, "a cursor returned another value for key %d", key_int(&key));
		nkeys++;
	}
	PROLLY_CALL(tr, cursor_close, c);
	if (nkeys != BIG_KEYS)
		elog(ERROR, "a cursor returned %d of %d keys", nkeys, BIG_KEYS);

	/* Walks visit the values' nodes: many more than the leaves of keys. */
	PROLLY_CALL(tr, walk, &env, &built, count_visit, &nnodes);
	if (nnodes < BIG_KEYS)
		elog(ERROR, "a walk visited only %d nodes of %d large values", nnodes,
			 BIG_KEYS);

	/* One byte changed in a large value: few nodes written; diff shows it. */
	{
		ProllyEdit	edit;
		KeyBuf		kb;

		big_value(13, 1, &vbuf);
		edit.key = key_of(13, &kb);
		edit.value.data = (const uint8 *) vbuf.data;
		edit.value.len = vbuf.len;
		edit.is_delete = false;
		env.counters = &counters;
		PROLLY_CALL(tr, apply, &env, &built, &edit, 1, &changed);
		env.counters = NULL;
		if (counters.nodes_written > 12)
			elog(ERROR, "changing one byte of a %u-byte value wrote %llu nodes",
				 vbuf.len, (unsigned long long) counters.nodes_written);
	}
	PROLLY_CALL(tr, diff, &env, &built, &changed, big_diff_visit, &d);
	if (d.nchanges != 1 || d.key != 13 || !d.values_ok)
		elog(ERROR, "diff of one changed large value reported %d changes (key %d, values %s)",
			 d.nchanges, d.key, d.values_ok ? "right" : "wrong");

	/* One byte inserted in the middle: still few nodes, as by byte counts. */
	{
		ProllyEdit	edit;
		KeyBuf		kb;
		StringInfoData ins;

		big_value(17, 0, &vbuf);
		initStringInfo(&ins);
		appendBinaryStringInfo(&ins, vbuf.data, vbuf.len / 2);
		appendStringInfoChar(&ins, '#');
		appendBinaryStringInfo(&ins, vbuf.data + vbuf.len / 2,
							   vbuf.len - vbuf.len / 2);
		edit.key = key_of(17, &kb);
		edit.value.data = (const uint8 *) ins.data;
		edit.value.len = ins.len;
		edit.is_delete = false;
		counters.nodes_written = 0;
		env.counters = &counters;
		PROLLY_CALL(tr, apply, &env, &built, &edit, 1, &changed);
		env.counters = NULL;
		if (counters.nodes_written > 12)
			elog(ERROR, "inserting one byte in a %u-byte value wrote %llu nodes",
				 vbuf.len, (unsigned long long) counters.nodes_written);
		pfree(ins.data);
	}
	pfree(vbuf.data);
	value_trees(tr, &env);
}

/* The key of large_keys' item i: n bytes, all 'a' but its first few. */
static ProllyKey
large_key(int i, int n, StringInfo buf)
{
	ProllyKey	key;

	resetStringInfo(buf);
	appendStringInfo(buf, "%04d", i);
	while (buf->len < n)
		appendStringInfoChar(buf, 'a');
	key.data = (const uint8 *) buf->data;
	key.len = buf->len;
	return key;
}

/*
 * Keys of many kilobytes (each one ending every node it is in, by the
 * chunker's odds), and keys of key_max() bytes with the longest values
 * kept in line: a tree of one, or of many, still has a root, the same
 * however it was made; a key too large for two to fit in a node is
 * refused.  Over chunks only (a page doesn't hold such keys).
 */
void
tree_large_keys(const ProllyImplInfo *impl)
{
	const ProllyTreeRoutine *tr = (const ProllyTreeRoutine *) impl;
	ProllyTreeEnv env;
	StringInfoData kbuf;
	StringInfoData vbuf;
	ProllyNodeRef root;
	ProllyNodeRef edited;
	ProllyTreeBuilder *b;
	MemoryContext cxt = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	bool		refused = false;
	int			key_lens[2];
	ProllyValue value;

	make_env(&env);
	initStringInfo(&kbuf);
	initStringInfo(&vbuf);
	key_lens[0] = 10000;
	key_lens[1] = PROLLY_CALL(tr, key_max, env.chunk_format, env.hash);
	if (key_lens[1] < 10000 || key_lens[1] > 40000)
		elog(ERROR, "key_max() is %d bytes", key_lens[1]);

	for (int l = 0; l < lengthof(key_lens); l++)
	{
		int			klen = key_lens[l];

		/* (2048 bytes: the longest value a leaf keeps in line) */
		resetStringInfo(&vbuf);
		appendStringInfoChar(&vbuf, 'v');
		while (l > 0 && vbuf.len < 2048)
			appendStringInfoChar(&vbuf, 'v');
		value.data = (const uint8 *) vbuf.data;
		value.len = vbuf.len;

		for (int nkeys = 1; nkeys <= 40; nkeys += 39)
		{
			ProllyKey	key;
			StringInfoData got;

			/* built at once */
			b = PROLLY_CALL(tr, build_begin, &env, CurrentMemoryContext);
			for (int i = 0; i < nkeys; i++)
			{
				key = large_key(i, klen, &kbuf);
				PROLLY_CALL(tr, build_add, b, &key, &value);
			}
			PROLLY_CALL(tr, build_finish, b, &root);
			if (PROLLY_CALL(tr, count, &env, &root) != nkeys)
				elog(ERROR, "a tree of %d keys of %d bytes counts " UINT64_FORMAT,
					 nkeys, klen, PROLLY_CALL(tr, count, &env, &root));
			initStringInfo(&got);
			key = large_key(nkeys - 1, klen, &kbuf);
			if (!PROLLY_CALL(tr, get, &env, &root, &key, &got) ||
				got.len != value.len || memcmp(got.data, value.data, value.len) != 0)
				elog(ERROR, "a tree of %d keys of %d bytes lost its last",
					 nkeys, klen);

			/* and by edits, one key at a time: the same tree */
			b = PROLLY_CALL(tr, build_begin, &env, CurrentMemoryContext);
			PROLLY_CALL(tr, build_finish, b, &edited);
			for (int i = nkeys - 1; i >= 0; i--)
			{
				ProllyEdit	edit;
				ProllyNodeRef next;

				edit.key = large_key(i, klen, &kbuf);
				edit.value = value;
				edit.is_delete = false;
				PROLLY_CALL(tr, apply, &env, &edited, &edit, 1, &next);
				edited = next;
			}
			if (!same_hash(&root, &edited))
				elog(ERROR, "a tree of %d keys of %d bytes built at once and by edits differ",
					 nkeys, klen);
			pfree(got.data);
		}
	}

	/* a key larger than half a node */
	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(cxt);
	PG_TRY();
	{
		ProllyKey	key = large_key(0, 40000, &kbuf);

		b = PROLLY_CALL(tr, build_begin, &env, CurrentMemoryContext);
		PROLLY_CALL(tr, build_add, b, &key, &value);
		key = large_key(1, 40000, &kbuf);
		PROLLY_CALL(tr, build_add, b, &key, &value);
		PROLLY_CALL(tr, build_finish, b, &root);
		ReleaseCurrentSubTransaction();
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(cxt);
		edata = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		refused = strstr(edata->message, "is too large") != NULL;
		if (!refused)
			elog(ERROR, "a key of 40000 bytes failed otherwise: %s", edata->message);
	}
	PG_END_TRY();
	MemoryContextSwitchTo(cxt);
	CurrentResourceOwner = owner;
	if (!refused)
		elog(ERROR, "a tree of keys of 40000 bytes was built");
	pfree(kbuf.data);
	pfree(vbuf.data);
}

/*
 * Values that hold references to value trees of their own (value_write()'s),
 * which the environment's value_refs() finds: they read back as they were
 * put, from get, cursors and diff, long ones too; a tree built at once and
 * one built by edits are the same; and a walk reaches every value tree a
 * value references, from the nodes alone, with no value_refs() set.
 *
 * A held value here is a byte, the number of references, then each
 * reference, then filler.
 */
#define HELD_KEYS	30

static void
held_refs(const ProllyValue *value, void *arg, StringInfo refs)
{
	const char *p = (const char *) value->data;
	const char *end = p + value->len;
	int			n = (uint8) *p++;

	(*(int *) arg)++;
	for (int i = 0; i < n; i++)
	{
		const char *start = p;

		p += 1 + (uint8) *p;
		(void) prolly_get_varint(&p, end);
		appendBinaryStringInfo(refs, start, p - start);
	}
}

/* Blob b's bytes: random, 5 to 40 KB. */
static void
held_blob(int b, StringInfo buf)
{
	uint64		x = 977 * (uint64) b + 3;
	int			len = 5000 + b * 7919 % 35000;

	resetStringInfo(buf);
	while (buf->len < len)
	{
		x = x * 6364136223846793005u + 1442695040888963407u;
		appendStringInfoChar(buf, (char) (x >> 56));
	}
}

/* Key k's value at version v, its references taken from refs. */
static void
held_value(int k, int v, StringInfo *refs, StringInfo buf)
{
	int			n = (k + v) % 3;

	resetStringInfo(buf);
	appendStringInfoChar(buf, (char) n);
	for (int i = 0; i < n; i++)
	{
		StringInfo	r = refs[(k * 2 + i + v) % 8];

		appendBinaryStringInfo(buf, r->data, r->len);
	}
	/* every fifth value long, stored out of line itself */
	for (int i = 0; i < (k % 5 == 0 ? 3000 : 20); i++)
		appendStringInfoChar(buf, (char) ('a' + (k + i) % 26));
}

typedef struct HeldDiff
{
	StringInfo *refs;
	int			nchanges;
	bool		ok;
} HeldDiff;

static void
held_diff_visit(ProllyDiffKind kind, const ProllyKey *key,
				const ProllyValue *before, const ProllyValue *after, void *arg)
{
	HeldDiff   *d = arg;
	StringInfoData want;

	d->nchanges++;
	initStringInfo(&want);
	held_value(key_int(key), 0, d->refs, &want);
	d->ok &= before && before->len == want.len &&
		memcmp(before->data, want.data, want.len) == 0;
	held_value(key_int(key), 1, d->refs, &want);
	d->ok &= after && after->len == want.len &&
		memcmp(after->data, want.data, want.len) == 0;
	pfree(want.data);
}

/* A prefetch hint: the chunks heard, into a HashSet. */
static void
held_hint(void *arg, const ProllyHash *hashes, int n)
{
	for (int i = 0; i < n; i++)
	{
		ProllyNodeRef ref = {0};

		ref.hash = hashes[i];
		(void) collect_node(&ref, 0, arg);
	}
}

/* A chunk's reference, into a HashSet. */
static void
held_reference(const ProllyHash *ref, void *arg)
{
	ProllyNodeRef node = {0};

	node.hash = *ref;
	(void) collect_node(&node, 0, arg);
}

/* Whether set holds the root of the value tree ref names. */
static bool
holds_ref(const HashSet *set, const StringInfo ref)
{
	ProllyHash	h;

	memset(&h, 0, sizeof(h));
	h.len = (uint8) ref->data[0];
	memcpy(h.bytes, ref->data + 1, h.len);
	return in_set(set, &h);
}

void
tree_held_references(const ProllyImplInfo *impl)
{
	const ProllyTreeRoutine *tr = (const ProllyTreeRoutine *) impl;
	ProllyTreeEnv env;
	StringInfo	refs[8];
	StringInfoData vbuf;
	ProllyTreeBuilder *b;
	ProllyNodeRef built;
	ProllyNodeRef edited;
	ProllyNodeRef changed;
	ProllyTreeCursor *c;
	ProllyKey	key;
	ProllyValue value;
	HashSet		visited = {0};
	HashSet		heard = {0};
	HashSet		named = {0};
	HashSet		diffed = {0};
	HeldDiff	d = {refs, 0, true};
	int			calls = 0;
	int			n = 0;

	make_env(&env);
	initStringInfo(&vbuf);
	for (int i = 0; i < 8; i++)
	{
		refs[i] = makeStringInfo();
		held_blob(i, &vbuf);
		PROLLY_CALL(tr, value_write, &env, (const uint8 *) vbuf.data, vbuf.len,
					refs[i]);
	}
	env.value_refs = held_refs;
	env.value_refs_arg = &calls;

	b = PROLLY_CALL(tr, build_begin, &env, CurrentMemoryContext);
	for (int k = 0; k < HELD_KEYS; k++)
	{
		KeyBuf		kb;

		key = key_of(k, &kb);
		held_value(k, 0, refs, &vbuf);
		value.data = (const uint8 *) vbuf.data;
		value.len = vbuf.len;
		PROLLY_CALL(tr, build_add, b, &key, &value);
	}
	PROLLY_CALL(tr, build_finish, b, &built);
	if (calls != HELD_KEYS)
		elog(ERROR, "value_refs() was called %d times for %d values", calls,
			 HELD_KEYS);

	/* By edits, in two batches. */
	b = PROLLY_CALL(tr, build_begin, &env, CurrentMemoryContext);
	PROLLY_CALL(tr, build_finish, b, &edited);
	for (int half = 0; half < 2; half++)
	{
		ProllyEdit	edits[HELD_KEYS];
		KeyBuf		kbs[HELD_KEYS];
		StringInfoData bufs[HELD_KEYS];
		int			m = 0;

		for (int k = half; k < HELD_KEYS; k += 2)
		{
			initStringInfo(&bufs[m]);
			held_value(k, 0, refs, &bufs[m]);
			edits[m].key = key_of(k, &kbs[m]);
			edits[m].value.data = (const uint8 *) bufs[m].data;
			edits[m].value.len = bufs[m].len;
			edits[m].is_delete = false;
			m++;
		}
		PROLLY_CALL(tr, apply, &env, &edited, edits, m, &edited);
	}
	if (!same_hash(&built, &edited))
		elog(ERROR, "a tree of values holding references built at once and by edits differ");

	/* Read back as put. */
	for (int k = 0; k < HELD_KEYS; k++)
	{
		KeyBuf		kb;
		StringInfoData got;

		key = key_of(k, &kb);
		initStringInfo(&got);
		held_value(k, 0, refs, &vbuf);
		if (!PROLLY_CALL(tr, get, &env, &built, &key, &got) ||
			got.len != vbuf.len || memcmp(got.data, vbuf.data, got.len) != 0)
			elog(ERROR, "get of key %d returned another value", k);
		pfree(got.data);
	}
	c = PROLLY_CALL(tr, cursor_open, &env, &built, CurrentMemoryContext);
	PROLLY_CALL(tr, cursor_seek, c, NULL, ForwardScanDirection);
	while (PROLLY_CALL(tr, cursor_next, c, &key, &value))
	{
		held_value(key_int(&key), 0, refs, &vbuf);
		if (value.len != vbuf.len || memcmp(value.data, vbuf.data, value.len) != 0)
			elog(ERROR, "a cursor returned another value for key %d", key_int(&key));
		n++;
	}
	PROLLY_CALL(tr, cursor_close, c);
	if (n != HELD_KEYS)
		elog(ERROR, "a cursor returned %d of %d keys", n, HELD_KEYS);

	/*
	 * A walk, with nothing to say how values are laid out, reaches them all;
	 * and so do the prefetch hints it gives a store that hears them.
	 */
	env.value_refs = NULL;
	if (PROLLY_HAS(env.chunk_store, read_through))
		PROLLY_CALL(env.chunk_store, read_through, env.chunks, NULL, held_hint,
					&heard);
	PROLLY_CALL(tr, walk, &env, &built, collect_node, &visited);
	if (PROLLY_HAS(env.chunk_store, read_through))
		PROLLY_CALL(env.chunk_store, read_through, env.chunks, NULL, NULL, NULL);
	/*
	 * And so does what the tree says its nodes' chunks name, read from the
	 * chunks alone (as the chunk store checks what it stores, and collects).
	 */
	if (PROLLY_HAS(tr, references))
	{
		StringInfoData chunk;

		initStringInfo(&chunk);
		for (int j = 0; j < visited.n; j++)
		{
			resetStringInfo(&chunk);
			if (!PROLLY_CALL(env.chunk_store, get, env.chunks, &visited.hashes[j],
							 &chunk))
				elog(ERROR, "a node a walk visited isn't in the store");
			PROLLY_CALL(tr, references, env.chunk_format, chunk.data, chunk.len,
						held_reference, &named);
		}
		pfree(chunk.data);
	}
	for (int i = 0; i < 8; i++)
	{
		if (PROLLY_HAS(tr, references) && !holds_ref(&named, refs[i]))
			elog(ERROR, "no node's chunk names value tree %d, which values reference",
				 i);
		if (!holds_ref(&visited, refs[i]))
			elog(ERROR, "a walk didn't reach value tree %d, which values reference",
				 i);
		if (PROLLY_HAS(env.chunk_store, read_through) && !holds_ref(&heard, refs[i]))
			elog(ERROR, "a walk's prefetch hints didn't name value tree %d", i);
	}

	/* Every other value changed: diff reports them as put. */
	env.value_refs = held_refs;
	{
		ProllyEdit	edits[HELD_KEYS];
		KeyBuf		kbs[HELD_KEYS];
		StringInfoData bufs[HELD_KEYS];
		int			m = 0;

		for (int k = 0; k < HELD_KEYS; k += 2)
		{
			initStringInfo(&bufs[m]);
			held_value(k, 1, refs, &bufs[m]);
			edits[m].key = key_of(k, &kbs[m]);
			edits[m].value.data = (const uint8 *) bufs[m].data;
			edits[m].value.len = bufs[m].len;
			edits[m].is_delete = false;
			m++;
		}
		PROLLY_CALL(tr, apply, &env, &built, edits, m, &changed);
	}
	PROLLY_CALL(tr, diff, &env, &built, &changed, held_diff_visit, &d);
	if (d.nchanges != HELD_KEYS / 2 || !d.ok)
		elog(ERROR, "diff of values holding references reported %d changes (values %s)",
			 d.nchanges, d.ok ? "right" : "wrong");

	/* diff_nodes() reaches the value trees the changed values reference. */
	PROLLY_CALL(tr, diff_nodes, &env, &built, &changed, collect_node, &diffed);
	for (int k = 0; k < HELD_KEYS; k += 2)
		for (int i = 0; i < (k + 1) % 3; i++)
			if (!holds_ref(&diffed, refs[(k * 2 + i + 1) % 8]))
				elog(ERROR, "diff_nodes() didn't reach value tree %d, which a changed value references",
					 (k * 2 + i + 1) % 8);
	pfree(vbuf.data);
}
