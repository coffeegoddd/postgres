/*--------------------------------------------------------------------------
 *
 * tree_checks.c
 *		Conformance checks for the tree interface.
 *
 * Trees are built from the prolly_d1 profile's foundation components (node
 * format, chunker, hash, node store, log), or, when tree_checks_in_chunks
 * is set, as chunks in the database's chunk store (basic chunk format,
 * byte chunker, SHA-256), and compared against an in-memory
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

bool		tree_checks_in_chunks = false;

static void
make_env(const ProllyTreeRoutine *tree, ProllyTreeEnv *env)
{
	ProllyComponents c;

	(void) tree;
	memset(env, 0, sizeof(*env));
	if (tree_checks_in_chunks)
	{
		env->chunk_format = prolly_chunkformat_lookup("basic");
		env->chunker = prolly_chunker_lookup("bytes");
		env->hash = prolly_hash_lookup("sha256");
		env->chunk_store = prolly_chunk_store();
		env->chunks = PROLLY_CALL(env->chunk_store, open, MyDatabaseId,
								  MyDatabaseTableSpace);
		env->compare = memcmp_keys;
		return;
	}
	prolly_components_for_am("prolly_d1", &c);
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

	StringInfoData at;

	initStringInfo(&at);
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

		/* The item at each position is the one the scan finds there. */
		resetStringInfo(&at);
		if (!PROLLY_CALL(tr, key_at, env, root, n, &at) ||
			at.len != key.len || memcmp(at.data, key.data, key.len) != 0)
			elog(ERROR, "%s: key_at(" UINT64_FORMAT ") is not key %d",
				 when, n, k);
		next++;
		n++;
	}
	resetStringInfo(&at);
	if (PROLLY_CALL(tr, key_at, env, root, n, &at) ||
		PROLLY_CALL(tr, key_at, env, root, n + 1000, &at))
		elog(ERROR, "%s: key_at past the last item found one", when);
	pfree(at.data);
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
	Size		capacity;

	if (env->chunk_store != NULL)
		capacity = 64 * 1024 - PROLLY_CALL(env->chunk_format, header_size);
	else
	{
		PROLLY_CALL(env->node_format, page_init, page, BLCKSZ, 0);
		capacity = PROLLY_CALL(env->node_format, free_space, page);
	}
	ch = PROLLY_CALL(env->chunker, begin, 0, capacity, CurrentMemoryContext);
	initStringInfo(&vbuf);
	for (int k = 0; k < MAX_KEY - 3 && found < 0; k++)
	{
		KeyBuf		kb;
		ProllyKey	key = key_of(k, &kb);
		ProllyValue value = value_of(k, 1, &vbuf);
		Size		space = env->chunk_store != NULL ?
			PROLLY_CALL(env->chunk_format, item_space, 0, &key, &value) :
			PROLLY_CALL(env->node_format, item_space, 0, &key, &value);

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

	make_env(tr, &env);
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

	make_env(tr, &env);
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

	make_env(tr, &env);
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

	make_env(tr, &env);
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

	make_env(tr, &env);
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

/* Every node a tree is made of passes the node format's structure check. */
typedef struct PageCheck
{
	const ProllyTreeEnv *env;
	int			nodes;
} PageCheck;

static bool
check_node_page(const ProllyNodeRef *node, uint16 level, void *arg)
{
	PageCheck  *pc = arg;
	Buffer		buf;
	char	   *detail = NULL;

	if (pc->env->chunk_store != NULL)
	{
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
	buf = PROLLY_CALL(pc->env->node_store, read, pc->env->store, node);

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	if (!PROLLY_CALL(pc->env->node_format, page_check, BufferGetPage(buf), &detail))
		elog(ERROR, "node at level %u fails the page check: %s", level, detail);
	if (PROLLY_CALL(pc->env->node_format, page_level, BufferGetPage(buf)) != level)
		elog(ERROR, "node visited at level %u says it is at level %u", level,
			 PROLLY_CALL(pc->env->node_format, page_level, BufferGetPage(buf)));
	UnlockReleaseBuffer(buf);
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
	PageCheck	pc = {&env, 0};

	make_env(tr, &env);
	fill_ref(ref, 1, 4);			/* every key, longer values */
	build_ref(tr, &env, ref, &root);
	PROLLY_CALL(tr, walk, &env, &root, check_node_page, &pc);
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

	make_env(tr, &env);
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
	pfree(vbuf.data);
}
