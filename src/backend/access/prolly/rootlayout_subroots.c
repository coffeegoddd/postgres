/*-------------------------------------------------------------------------
 *
 * rootlayout_subroots.c
 *	  Root layout implementation "subroots": a branch root of sub-root
 *	  trees, each mapping relfilenumbers to relation trees.
 *
 * Layout.  The branch root is a tree with one item per sub-root, always
 * all of them, an empty sub-root being the empty tree:
 *
 *	key: uint8 sub-root number		value: uint8 hash length, hash
 *
 * Each sub-root is a tree with one item per relation:
 *
 *	key: uint32 relfilenumber, big-endian	value: uint8 hash length, hash
 *
 * Big-endian keys sort by memcmp in relation OID order.  Values hold hashes only,
 * never block hints: trees' items are hashed, and physical locations must
 * never be part of a hash.  So reading through
 * a reference costs a hash -> block map lookup; a hint in an unhashed side
 * record is an optimization for later, if measured to matter.
 *
 * Every tree here is built and changed through the tree interface, so the
 * root hash is history independent because each tree's is.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/rootlayout_subroots.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"
#include "miscadmin.h"

#define SUBROOTS_IMPL_ID	1

/* ------------------------------------------------------------------------
 * Keys and values
 * ------------------------------------------------------------------------
 */

typedef struct RelKey
{
	uint8		bytes[4];
} RelKey;

/* The tree environment with this layout's key order. */
static void
layout_env(const ProllyRootEnv *env, ProllyTreeEnv *tenv)
{
	*tenv = *env->tree_env;
	tenv->compare = prolly_bytes_compare;
	tenv->compare_arg = NULL;
}

static ProllyKey
sub_key(ProllySubRoot sub, uint8 *byte)
{
	ProllyKey	key;

	*byte = (uint8) sub;
	key.data = byte;
	key.len = 1;
	return key;
}

static ProllyKey
rel_key(Oid relid, RelKey *buf)
{
	ProllyKey	key;

	buf->bytes[0] = (uint8) (relid >> 24);
	buf->bytes[1] = (uint8) (relid >> 16);
	buf->bytes[2] = (uint8) (relid >> 8);
	buf->bytes[3] = (uint8) relid;
	key.data = buf->bytes;
	key.len = 4;
	return key;
}

static Oid
key_relid(const ProllyKey *key)
{
	if (key->len != 4)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid prolly root layout relation key")));
	return ((Oid) key->data[0] << 24) | ((Oid) key->data[1] << 16) |
		((Oid) key->data[2] << 8) | (Oid) key->data[3];
}

/* A reference must carry a hash that decode_ref() will accept. */
static void
check_ref(const ProllyNodeRef *ref)
{
	if (ref->hash.len == 0 || ref->hash.len > PROLLY_HASH_MAX_LEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("prolly root layout reference has an invalid hash length %u",
						(unsigned) ref->hash.len)));
}

static void
encode_ref(StringInfo out, const ProllyNodeRef *ref)
{
	check_ref(ref);
	appendStringInfoChar(out, (char) ref->hash.len);
	appendBinaryStringInfo(out, ref->hash.bytes, ref->hash.len);
}

static void
decode_ref(const uint8 *data, Size len, ProllyNodeRef *ref)
{
	if (len < 1 || data[0] == 0 || data[0] > PROLLY_HASH_MAX_LEN ||
		len != 1 + (Size) data[0])
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid prolly root layout reference")));
	memset(ref, 0, sizeof(*ref));
	ref->hash.len = data[0];
	memcpy(ref->hash.bytes, data + 1, data[0]);
	ref->location.blkno = InvalidBlockNumber;
}

static void
check_sub(ProllySubRoot sub)
{
	if ((int) sub < 0 || (int) sub >= PROLLY_NUM_SUBROOTS)
		elog(ERROR, "invalid prolly sub-root %d", (int) sub);
}

static bool
same_hash(const ProllyNodeRef *a, const ProllyNodeRef *b)
{
	return a->hash.len == b->hash.len &&
		memcmp(a->hash.bytes, b->hash.bytes, a->hash.len) == 0;
}

/* ------------------------------------------------------------------------
 * The branch root
 * ------------------------------------------------------------------------
 */

/* Build a branch root from its sub-roots. */
static void
build_root(const ProllyRootEnv *env, const ProllyNodeRef *subs,
		   ProllyNodeRef *root)
{
	ProllyTreeEnv tenv;
	ProllyTreeBuilder *b;

	layout_env(env, &tenv);
	b = PROLLY_CALL(env->tree, build_begin, &tenv, CurrentMemoryContext);
	for (int s = 0; s < PROLLY_NUM_SUBROOTS; s++)
	{
		uint8		byte;
		ProllyKey	key = sub_key((ProllySubRoot) s, &byte);
		StringInfoData v;
		ProllyValue value;

		initStringInfo(&v);
		encode_ref(&v, &subs[s]);
		value.data = (const uint8 *) v.data;
		value.len = v.len;
		PROLLY_CALL(env->tree, build_add, b, &key, &value);
		pfree(v.data);
	}
	PROLLY_CALL(env->tree, build_finish, b, root);
}

static void
subroots_subroot(const ProllyRootEnv *env, const ProllyNodeRef *root,
				 ProllySubRoot sub, ProllyNodeRef *subroot)
{
	ProllyTreeEnv tenv;
	uint8		byte;
	ProllyKey	key;
	StringInfoData v;

	check_sub(sub);
	layout_env(env, &tenv);
	key = sub_key(sub, &byte);
	initStringInfo(&v);
	if (!PROLLY_CALL(env->tree, get, &tenv, root, &key, &v))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly branch root has no sub-root %d", (int) sub)));
	decode_ref((const uint8 *) v.data, v.len, subroot);
	pfree(v.data);
}

static void
subroots_empty(const ProllyRootEnv *env, ProllyNodeRef *root)
{
	ProllyTreeEnv tenv;
	ProllyTreeBuilder *b;
	ProllyNodeRef subs[PROLLY_NUM_SUBROOTS];

	layout_env(env, &tenv);
	b = PROLLY_CALL(env->tree, build_begin, &tenv, CurrentMemoryContext);
	PROLLY_CALL(env->tree, build_finish, b, &subs[0]);
	for (int s = 1; s < PROLLY_NUM_SUBROOTS; s++)
		subs[s] = subs[0];
	build_root(env, subs, root);
}

/* ------------------------------------------------------------------------
 * Lookup, apply
 * ------------------------------------------------------------------------
 */

static bool
subroots_lookup(const ProllyRootEnv *env, const ProllyNodeRef *root,
				ProllySubRoot sub, Oid relid, ProllyNodeRef *tree)
{
	ProllyTreeEnv tenv;
	ProllyNodeRef subroot;
	RelKey		kb;
	ProllyKey	key = rel_key(relid, &kb);
	StringInfoData v;
	bool		found;

	subroots_subroot(env, root, sub, &subroot);
	layout_env(env, &tenv);
	initStringInfo(&v);
	found = PROLLY_CALL(env->tree, get, &tenv, &subroot, &key, &v);
	if (found)
		decode_ref((const uint8 *) v.data, v.len, tree);
	pfree(v.data);
	return found;
}

static void
subroots_apply(const ProllyRootEnv *env, const ProllyNodeRef *root,
			   const ProllyRootEdit *edits, int nedits, ProllyNodeRef *new_root)
{
	ProllyTreeEnv tenv;
	ProllyNodeRef subs[PROLLY_NUM_SUBROOTS];
	bool		changed = false;
	int			i = 0;

	/* Validate everything first, so a bad edit never reaches a tree. */
	for (int k = 0; k < nedits; k++)
	{
		check_sub(edits[k].sub);
		if (!edits[k].is_remove)
			check_ref(&edits[k].tree);
		if (k > 0 &&
			(edits[k - 1].sub > edits[k].sub ||
			 (edits[k - 1].sub == edits[k].sub &&
			  edits[k - 1].relid >= edits[k].relid)))
			elog(ERROR, "prolly root layout edits are not in strictly increasing (sub-root, relid) order");
	}

	layout_env(env, &tenv);
	for (int s = 0; s < PROLLY_NUM_SUBROOTS; s++)
	{
		int			start = i;
		int			n;
		ProllyEdit *tedits;
		RelKey	   *kbs;
		StringInfoData *vbufs;
		ProllyNodeRef old;

		subroots_subroot(env, root, (ProllySubRoot) s, &subs[s]);
		while (i < nedits && edits[i].sub == (ProllySubRoot) s)
			i++;
		n = i - start;
		if (n == 0)
			continue;

		tedits = palloc(sizeof(ProllyEdit) * n);
		kbs = palloc(sizeof(RelKey) * n);
		vbufs = palloc(sizeof(StringInfoData) * n);
		for (int k = 0; k < n; k++)
		{
			const ProllyRootEdit *e = &edits[start + k];

			tedits[k].key = rel_key(e->relid, &kbs[k]);
			tedits[k].is_delete = e->is_remove;
			initStringInfo(&vbufs[k]);
			if (!e->is_remove)
				encode_ref(&vbufs[k], &e->tree);
			tedits[k].value.data = (const uint8 *) vbufs[k].data;
			tedits[k].value.len = vbufs[k].len;
		}
		old = subs[s];
		PROLLY_CALL(env->tree, apply, &tenv, &old, tedits, n, &subs[s]);
		if (!same_hash(&old, &subs[s]))
			changed = true;
		for (int k = 0; k < n; k++)
			pfree(vbufs[k].data);
		pfree(tedits);
		pfree(kbs);
		pfree(vbufs);
	}

	if (changed)
		build_root(env, subs, new_root);
	else
		*new_root = *root;
}

/* ------------------------------------------------------------------------
 * Scan, walk
 * ------------------------------------------------------------------------
 */

/* Visit each (relid, tree) item of a sub-root tree. */
static void
scan_subroot(const ProllyRootEnv *env, const ProllyNodeRef *subroot,
			 ProllySubRoot sub, ProllyRootVisitRel visit, void *arg)
{
	ProllyTreeEnv tenv;
	ProllyTreeCursor *c;
	ProllyKey	key;
	ProllyValue value;

	layout_env(env, &tenv);
	c = PROLLY_CALL(env->tree, cursor_open, &tenv, subroot, CurrentMemoryContext);
	PROLLY_CALL(env->tree, cursor_seek, c, NULL, ForwardScanDirection);
	while (PROLLY_CALL(env->tree, cursor_next, c, &key, &value))
	{
		ProllyNodeRef tree;
		Oid			relid = key_relid(&key);

		CHECK_FOR_INTERRUPTS();
		decode_ref(value.data, value.len, &tree);
		visit(sub, relid, &tree, arg);
	}
	PROLLY_CALL(env->tree, cursor_close, c);
}

static void
subroots_scan(const ProllyRootEnv *env, const ProllyNodeRef *root,
			  ProllySubRoot sub, ProllyRootVisitRel visit, void *arg)
{
	ProllyNodeRef subroot;

	subroots_subroot(env, root, sub, &subroot);
	scan_subroot(env, &subroot, sub, visit, arg);
}

/* A relation's tree, collected during a walk. */
typedef struct WalkTrees
{
	ProllyNodeRef *trees;
	int			n;
	int			max;
} WalkTrees;

static void
collect_tree(ProllySubRoot sub, Oid relid, const ProllyNodeRef *tree, void *arg)
{
	WalkTrees  *w = arg;

	if (w->n == w->max)
	{
		w->max = Max(16, w->max * 2);
		w->trees = w->trees ? repalloc(w->trees, sizeof(ProllyNodeRef) * w->max) :
			palloc(sizeof(ProllyNodeRef) * w->max);
	}
	w->trees[w->n++] = *tree;
}

static void
subroots_walk(const ProllyRootEnv *env, const ProllyNodeRef *root,
			  ProllyTreeVisitNode visit, void *arg)
{
	ProllyTreeEnv tenv;

	layout_env(env, &tenv);
	PROLLY_CALL(env->tree, walk, &tenv, root, visit, arg);
	for (int s = 0; s < PROLLY_NUM_SUBROOTS; s++)
	{
		ProllyNodeRef subroot;
		WalkTrees	w = {0};

		subroots_subroot(env, root, (ProllySubRoot) s, &subroot);
		PROLLY_CALL(env->tree, walk, &tenv, &subroot, visit, arg);

		/* Collect first, so the cursor isn't held open across long walks. */
		scan_subroot(env, &subroot, (ProllySubRoot) s, collect_tree, &w);
		for (int k = 0; k < w.n; k++)
		{
			/* Relation trees use their own key order; walk needs none. */
			PROLLY_CALL(env->tree, walk, env->tree_env, &w.trees[k], visit, arg);
		}
		if (w.trees)
			pfree(w.trees);
	}
}

/* ------------------------------------------------------------------------
 * Diff
 * ------------------------------------------------------------------------
 */

typedef struct DiffArg
{
	ProllySubRoot sub;
	ProllyRootVisitChange visit;
	void	   *arg;
} DiffArg;

static void
diff_change(ProllyDiffKind kind, const ProllyKey *key, const ProllyValue *before,
			const ProllyValue *after, void *arg)
{
	DiffArg    *d = arg;
	ProllyNodeRef b;
	ProllyNodeRef a;

	if (before)
		decode_ref(before->data, before->len, &b);
	if (after)
		decode_ref(after->data, after->len, &a);
	d->visit(kind, d->sub, key_relid(key), before ? &b : NULL,
			 after ? &a : NULL, d->arg);
}

static void
subroots_diff(const ProllyRootEnv *env, const ProllyNodeRef *from,
			  const ProllyNodeRef *to, ProllyRootVisitChange visit, void *arg)
{
	ProllyTreeEnv tenv;

	if (same_hash(from, to))
		return;
	layout_env(env, &tenv);
	for (int s = 0; s < PROLLY_NUM_SUBROOTS; s++)
	{
		ProllyNodeRef fsub;
		ProllyNodeRef tsub;
		DiffArg		d = {(ProllySubRoot) s, visit, arg};

		subroots_subroot(env, from, (ProllySubRoot) s, &fsub);
		subroots_subroot(env, to, (ProllySubRoot) s, &tsub);
		if (same_hash(&fsub, &tsub))
			continue;
		PROLLY_CALL(env->tree, diff, &tenv, &fsub, &tsub, diff_change, &d);
	}
}

const ProllyRootLayoutRoutine prolly_rootlayout_subroots = {
	.info = {
		.iface = PROLLY_IFACE_ROOT_LAYOUT,
		.impl_id = SUBROOTS_IMPL_ID,
		.format_version = 3,
		.name = "subroots",
		.description = "catalog, user-data and local sub-roots, each a tree of relfilenumbers",
	},
	.empty = subroots_empty,
	.lookup = subroots_lookup,
	.apply = subroots_apply,
	.subroot = subroots_subroot,
	.scan = subroots_scan,
	.walk = subroots_walk,
	.diff = subroots_diff,
};
