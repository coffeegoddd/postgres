/*-------------------------------------------------------------------------
 *
 * tree_cow.c
 *	  Tree implementation "cow": copy-on-write prolly trees in a chunk store.
 *
 * Layout.  Leaves (level 0) hold the tree's items.  An internal item's key
 * is the largest key in its child's subtree, and its value is the child's
 * reference:
 *
 *	uint8 hash length, hash, varint item count of the subtree
 *
 * Storage.  Nodes are chunks of their own length in a chunk store
 * (prolly_chunkformat.h): all of a chunk is canonical, and a node is found
 * by its hash alone.
 *
 * Shape.  Each level's items are cut into nodes by the chunker.  The root
 * is the first level, counting up from the leaves, that has exactly one
 * node.  So the shape, and every hash, is a function of the items alone
 * (history independence, prolly_tree.h).
 *
 * Build.  Items stream into level 0; each node a level closes adds an item
 * to the level above.  At the end each level is flushed in turn until one
 * has exactly one node.
 *
 * Apply.  Every node boundary is a point where the chunker's state is
 * reset: a node that ended with a boundary key leaves it clean, and one
 * that ended because the next item didn't fit leaves it exactly as a clean
 * chunker would be after taking that next item.  So chunking may restart
 * at the start of any old node, and once the new chunking closes a node
 * exactly where an old node ended, every following old node is what new
 * chunking would produce.  Edits are applied one level at a time: at each
 * level, windows start at the old node holding the next edit, run the
 * merged old items and edits through a fresh chunker, and end at the first
 * such resynchronization.  The nodes a window replaced and the nodes it
 * made become deletes and puts at the level above.  The root's level is
 * rebuilt whole, growing new levels if it now needs more than one node,
 * and single-child roots are then removed.
 *
 * Large values.  A leaf value longer than VALUE_INLINE_MAX bytes is stored
 * out of line, in a value tree, and the leaf item then holds its reference
 *
 *	uint8 hash length, hash of the value tree's root, varint value length
 *
 * with the chunk format's PROLLY_ITEM_VALUE_REF flag.  Every read (get,
 * cursors, diff) returns the bytes; walks visit the value trees too.
 *
 * A value may hold references to value trees of its own (a row's long
 * columns, written with value_write()).  The environment's value_refs()
 * finds them as each item is put, and the item leads with them, flagged
 * PROLLY_ITEM_VALUE_REFS:
 *
 *	varint length of the references, each reference, then the value (or
 *	its value tree's reference)
 *
 * so a walk finds them from the node alone.  The references to other
 * chunks a value names (the environment's chunk_refs()) lead before those,
 * the same way, flagged PROLLY_ITEM_CHUNK_REFS: references() reports them,
 * walks don't follow them.  Reads return the value without either.
 *
 * A value tree is addressed by byte counts, as Dolt's blob trees are: the
 * value's bytes are cut into chunks where a rolling hash (buzhash) of the
 * last bytes says so, each chunk the one item of a leaf, with no key; an
 * internal node's items have no key either, and record each child's
 * reference and byte count.  The levels above the leaves are cut by the
 * chunker, given each child's hash as its item's key.  So inserting bytes
 * changes the chunks around them and the path above, not every chunk after
 * them, and a slice is read by descending through the byte counts.  The
 * limits and the rolling hash are part of the canonical format.
 *
 * Diff.  Each tree is walked by a cursor that stands on an item of some
 * node: a leaf item, or an internal item standing for its child's whole
 * subtree, whose hash the item holds.  A cursor that finishes a node moves
 * up to the parent's next item, so it walks as coarsely as it can.  Where
 * both stand on subtrees of the same height and hash, both skip them
 * unread; otherwise the taller descends (both, at equal heights), and leaf
 * items are merged by key.  A node is read only when one of its ancestors'
 * subtrees differed, so equal regions cost nothing below the level where
 * the chunkings of the two trees line up again.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/tree_cow.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/prolly_builtins.h"
#include "access/prolly_datum.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

#define COW_IMPL_ID		1
#define MAX_LEVELS		32

/* a chunk node's limit, for the chunker's forced split */
#define CHUNK_NODE_MAX	(64 * 1024)

/* ------------------------------------------------------------------------
 * Nodes
 * ------------------------------------------------------------------------
 */

typedef struct TNode
{
	ProllyChunkNode chunk;		/* its bytes (ours); data NULL if none */
	uint16		level;
	int			nitems;
	ProllyNodeRef ref;
} TNode;

/* Does the node hold a node read? */
static inline bool
node_loaded(const TNode *node)
{
	return node->chunk.data != NULL;
}

/*
 * Kept nodes.  Each level's pass of an apply descends from the root to the
 * nodes of its level, so a node at level L would be read once for each of
 * the L + 1 passes below and at it: a tree of height H would read about
 * H^2 / 2 nodes per edited path, where H + 1 are needed.  And a caller
 * that makes several small changes in a row (a transaction's commit:
 * seeking its rows, then applying them; getting and putting refs entries
 * one at a time) descends the same upper nodes each time.  So while a
 * scope is open (an apply's own, or one keep_nodes() opened), every node
 * read or written above the leaves, and every root read, is kept until
 * the scope ends, and read from the chunk store once.  Other leaves aren't
 * kept: they are read once by a pass that changes them, and a large change
 * would otherwise hold every leaf it read.  A node is found by its hash,
 * which names its bytes, so a kept node is never stale; and a scope's
 * caller reads only trees it holds, so none of their nodes leaves the
 * store while it is open.  A scope keeps at most maintenance_work_mem of
 * them; past it, nodes are read again each time, as without one.
 */
typedef struct KeptKey
{
	uint8		len;
	uint8		bytes[PROLLY_HASH_MAX_LEN];
} KeptKey;

typedef struct KeptNode
{
	KeptKey		key;
	uint8	   *data;
	Size		len;
} KeptNode;

typedef struct KeepScope
{
	HTAB	   *nodes;
	MemoryContext cxt;			/* the scope lasts as long as this does */
	struct KeepScope *outer;
	MemoryContextCallback ended;
	Size		bytes;			/* of the nodes kept */
} KeepScope;

static KeepScope *keeping = NULL;
/*
 * An apply whose root is a leaf: the last leaf it writes, which is its new
 * root if that is a leaf, kept by the apply once it knows (the leaves
 * written before it aren't read again).
 */
static bool keep_written_leaves = false;
static StringInfoData last_leaf = {NULL};
static ProllyHash last_leaf_hash;

/* (the scope's context going away ends it, on error too) */
static void
keep_scope_ended(void *arg)
{
	KeepScope  *ks = arg;

	/* (scopes nest: an inner one's context goes first) */
	if (keeping == ks)
		keeping = ks->outer;
	else
	{
		for (KeepScope *s = keeping; s != NULL; s = s->outer)
			if (s->outer == ks)
			{
				s->outer = ks->outer;
				break;
			}
	}
}

static void
keep_scope_open(MemoryContext cxt)
{
	KeepScope  *ks = MemoryContextAllocZero(cxt, sizeof(KeepScope));
	HASHCTL		ctl;

	ctl.keysize = sizeof(KeptKey);
	ctl.entrysize = sizeof(KeptNode);
	ctl.hcxt = cxt;
	ks->nodes = hash_create("prolly kept nodes", 16, &ctl,
							HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	ks->cxt = cxt;
	ks->outer = keeping;
	ks->ended.func = keep_scope_ended;
	ks->ended.arg = ks;
	MemoryContextRegisterResetCallback(cxt, &ks->ended);
	keeping = ks;
}

static void
kept_key(const ProllyHash *hash, KeptKey *key)
{
	memset(key, 0, sizeof(*key));
	key->len = hash->len;
	memcpy(key->bytes, hash->bytes, hash->len);
}

/* Keep a node's bytes until the open scope ends, if one is open. */
static void
kept_put(const ProllyHash *hash, const char *data, Size len)
{
	KeptKey		key;
	KeptNode   *kn;
	bool		found;

	if (keeping == NULL)
		return;

	/*
	 * (no more than maintenance_work_mem kept: past it, nodes are read
	 * again, as without a scope)
	 */
	if (keeping->bytes + len > (Size) maintenance_work_mem * 1024)
		return;
	kept_key(hash, &key);
	kn = hash_search(keeping->nodes, &key, HASH_ENTER, &found);
	if (!found)
	{
		kn->data = MemoryContextAlloc(keeping->cxt, Max(len, 1));
		memcpy(kn->data, data, len);
		kn->len = len;
		keeping->bytes += len + sizeof(KeptNode);
	}
}

/* (as node_read_level()'s level) a node of any level a tree may have */
#define LEVEL_ANY		(-1)

/*
 * A chunk node by hash, from the chunk store (or the open scope's kept
 * nodes), its bytes allocated in cxt: the context of whatever keeps the
 * node (an iterator, a cursor), which may outlive the one current at the
 * read.  root: the node is a tree's root, kept even if it is a leaf.
 * level: the level the node must be at (its parent's less one), or
 * LEVEL_ANY.  A node's level comes from its chunk, which may be another
 * server's: none is MAX_LEVELS - 1 or more (every walk keeps a frame a
 * level, and a build makes a level above its root's one node: so 30 is the
 * highest a tree's root is written at, and read at), each child is one
 * level below its parent, so a walk's depth is the root's level, and an
 * internal node has children.
 */
static void
node_read_level(const ProllyTreeEnv *env, const ProllyNodeRef *ref, TNode *node,
				MemoryContext cxt, bool root, int level)
{
	StringInfoData data;
	MemoryContext old;
	KeptNode   *kn = NULL;

	CHECK_FOR_INTERRUPTS();
	old = MemoryContextSwitchTo(cxt);

	node->chunk.data = NULL;
	initStringInfo(&data);
	MemoryContextSwitchTo(old);
	if (keeping != NULL)
	{
		KeptKey		key;

		kept_key(&ref->hash, &key);
		kn = hash_search(keeping->nodes, &key, HASH_FIND, NULL);
	}
	if (kn != NULL)
		appendBinaryStringInfo(&data, (const char *) kn->data, kn->len);
	else
	{
		if (!PROLLY_CALL(env->chunk_store, get, env->chunks, &ref->hash, &data))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("prolly tree node missing from its chunk store")));
		if (env->counters)
			env->counters->nodes_read++;
	}
	if (!PROLLY_CALL(env->chunk_format, open, (const uint8 *) data.data,
					 data.len, &node->chunk))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly chunk is not a tree node")));
	node->level = node->chunk.level;
	node->nitems = node->chunk.nitems;
	node->ref = *ref;
	if (node->level >= MAX_LEVELS - 1)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly tree node of level %u: a tree's levels are 0 to %d",
						node->level, MAX_LEVELS - 2)));
	if (node->level > 0 && node->nitems == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly tree node of level %u with no children",
						node->level)));
	if (level != LEVEL_ANY && node->level != level)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly tree node of level %u below a node of level %d",
						node->level, level + 1)));
	if (kn == NULL && (root || node->level > 0))
		kept_put(&ref->hash, data.data, data.len);
}

static void
node_read_keep(const ProllyTreeEnv *env, const ProllyNodeRef *ref, TNode *node,
			   MemoryContext cxt, bool root)
{
	node_read_level(env, ref, node, cxt, root, LEVEL_ANY);
}

static void
node_read_in(const ProllyTreeEnv *env, const ProllyNodeRef *ref, TNode *node,
			 MemoryContext cxt)
{
	node_read_keep(env, ref, node, cxt, false);
}

/* Read a node kept no longer than the current context. */
static void
node_read(const ProllyTreeEnv *env, const ProllyNodeRef *ref, TNode *node)
{
	node_read_in(env, ref, node, CurrentMemoryContext);
}

/* Read the child at ref of a node of level parent, in cxt. */
static void
node_read_child(const ProllyTreeEnv *env, const ProllyNodeRef *ref, TNode *node,
				MemoryContext cxt, uint16 parent)
{
	node_read_level(env, ref, node, cxt, false, (int) parent - 1);
}

static void
node_release(TNode *node)
{
	if (node->chunk.data != NULL)
		pfree((void *) node->chunk.data);
	node->chunk.data = NULL;
}

static void
node_item(const ProllyTreeEnv *env, const TNode *node, int i, ProllyKey *key,
		  ProllyValue *value, uint8 *flags)
{
	PROLLY_CALL(env->chunk_format, item_get, &node->chunk, i, key, value,
				flags);
}

static void
encode_child(StringInfo out, const ProllyHash *hash, uint64 count)
{
	appendStringInfoChar(out, (char) hash->len);
	appendBinaryStringInfo(out, hash->bytes, hash->len);
	prolly_put_varint(out, count);
}

static void
decode_child(const ProllyValue *value, ProllyHash *hash, uint64 *count)
{
	const char *p = (const char *) value->data;
	const char *end = p + value->len;

	if (value->len < 1 || (uint8) p[0] > PROLLY_HASH_MAX_LEN ||
		value->len < 1 + (uint32) (uint8) p[0])
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid prolly child reference")));
	memset(hash, 0, sizeof(*hash));
	hash->len = (uint8) p[0];
	memcpy(hash->bytes, p + 1, hash->len);
	p += 1 + hash->len;
	*count = prolly_get_varint(&p, end);
}

/* The reference and item count of internal item i's child. */
static void
node_child(const ProllyTreeEnv *env, const TNode *node, int i,
		   ProllyNodeRef *ref, uint64 *count)
{
	ProllyKey	key;
	ProllyValue value;

	Assert(node->level > 0);
	node_item(env, node, i, &key, &value, NULL);
	memset(ref, 0, sizeof(*ref));
	decode_child(&value, &ref->hash, count);
}

static int
cmp_keys(const ProllyTreeEnv *env, const ProllyKey *a, const ProllyKey *b)
{
	return env->compare(a, b, env->compare_arg);
}

/* First item whose key is >= key; nitems if none. */
static int
lower_bound(const ProllyTreeEnv *env, const TNode *node, const ProllyKey *key)
{
	int			lo = 0;
	int			hi = node->nitems;

	while (lo < hi)
	{
		int			mid = lo + (hi - lo) / 2;
		ProllyKey	k;
		ProllyValue v;

		node_item(env, node, mid, &k, &v, NULL);
		if (cmp_keys(env, &k, key) < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* First item whose key is > key; nitems if none. */
static int
upper_bound(const ProllyTreeEnv *env, const TNode *node, const ProllyKey *key)
{
	int			lo = 0;
	int			hi = node->nitems;

	while (lo < hi)
	{
		int			mid = lo + (hi - lo) / 2;
		ProllyKey	k;
		ProllyValue v;

		node_item(env, node, mid, &k, &v, NULL);
		if (cmp_keys(env, &k, key) <= 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/*
 * Write a node and return its reference.  If the store already has a node
 * with the same hash, that node is used and nothing is written.
 */
typedef struct Item
{
	ProllyKey	key;
	ProllyValue value;
	uint64		count;			/* internal items: child's item count */
	uint8		flags;			/* leaf items: the chunk format's */
} Item;

static void
write_node(const ProllyTreeEnv *env, uint16 level, const Item *items, int n,
		   ProllyNodeRef *ref, uint64 *count)
{
	ProllyKey  *keys = palloc(sizeof(ProllyKey) * Max(n, 1));
	ProllyValue *values = palloc(sizeof(ProllyValue) * Max(n, 1));
	uint8	   *flags = palloc(Max(n, 1));
	StringInfoData chunk;
	ProllyHashCtx *hctx;

	*count = 0;
	for (int i = 0; i < n; i++)
	{
		keys[i] = items[i].key;
		values[i] = items[i].value;
		flags[i] = items[i].flags;
		*count += level == 0 ? 1 : items[i].count;
	}
	initStringInfo(&chunk);
	PROLLY_CALL(env->chunk_format, encode, level, n, keys, values, flags, &chunk);

	memset(ref, 0, sizeof(*ref));
	hctx = PROLLY_CALL(env->hash, begin, CurrentMemoryContext);
	PROLLY_CALL(env->hash, update, hctx, (const uint8 *) chunk.data, chunk.len);
	PROLLY_CALL(env->hash, finish, hctx, &ref->hash);
	if (!PROLLY_CALL(env->chunk_store, has, env->chunks, &ref->hash))
	{
		if (env->counters)
			env->counters->nodes_written++;
		PROLLY_CALL(env->chunk_store, put, env->chunks, &ref->hash, chunk.data,
					chunk.len);
	}
	/* (an apply's new root may be read again, to drop single-child roots) */
	if (level > 0)
		kept_put(&ref->hash, chunk.data, chunk.len);
	else if (keep_written_leaves)
	{
		if (last_leaf.data == NULL)
		{
			MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

			initStringInfo(&last_leaf);
			MemoryContextSwitchTo(old);
		}
		resetStringInfo(&last_leaf);
		appendBinaryStringInfo(&last_leaf, chunk.data, chunk.len);
		last_leaf_hash = ref->hash;
	}
	pfree(chunk.data);
	pfree(keys);
	pfree(values);
	pfree(flags);
}

static Size
node_capacity(const ProllyChunkFormatRoutine *format)
{
	return CHUNK_NODE_MAX - PROLLY_CALL(format, header_size);
}

static Size
level_capacity(const ProllyTreeEnv *env, uint16 level)
{
	return node_capacity(env->chunk_format);
}

/* ------------------------------------------------------------------------
 * Values out of line
 * ------------------------------------------------------------------------
 */

#define VALUE_INLINE_MAX	2048	/* longer leaf values go out of line */
#define CHUNK_REFS_INLINE_MAX 1024	/* more declared references go in a
										 * tree of their own */
#define CHUNK_MIN			512
#define CHUNK_MAX			4096
#define CHUNK_MASK			0x7FF	/* a boundary every ~2 KB past the minimum */
#define BUZ_WINDOW			48

static uint32 buz_table[256];
static bool buz_ready = false;

/*
 * The rolling hash's table: fixed by the format, from splitmix64 seeded
 * with "prolly" (0x70726f6c6c79), so it is the same on every build.
 */
static void
buz_init(void)
{
	uint64		x = UINT64CONST(0x70726f6c6c79);

	for (int i = 0; i < 256; i++)
	{
		uint64		z;

		x += UINT64CONST(0x9E3779B97F4A7C15);
		z = x;
		z = (z ^ (z >> 30)) * UINT64CONST(0xBF58476D1CE4E5B9);
		z = (z ^ (z >> 27)) * UINT64CONST(0x94D049BB133111EB);
		z ^= z >> 31;
		buz_table[i] = (uint32) z;
	}
	buz_ready = true;
}

static inline uint32
rotl32(uint32 v, int n)
{
	n &= 31;
	return n == 0 ? v : (v << n) | (v >> (32 - n));
}

static int
memcmp_compare(const ProllyKey *a, const ProllyKey *b, void *arg)
{
	int			r = memcmp(a->data, b->data, Min(a->len, b->len));

	if (r != 0)
		return r;
	return (a->len > b->len) - (a->len < b->len);
}

/* A value tree's environment: env's components, keys by memcmp. */
static void
value_env(const ProllyTreeEnv *env, ProllyTreeEnv *venv)
{
	*venv = *env;
	venv->compare = memcmp_compare;
	venv->compare_arg = NULL;
	venv->value_refs = NULL;
	venv->value_refs_arg = NULL;
	venv->chunk_refs = NULL;
	venv->chunk_refs_arg = NULL;
}

static void
encode_value_ref(StringInfo out, const ProllyHash *hash, uint64 len)
{
	appendStringInfoChar(out, (char) hash->len);
	appendBinaryStringInfo(out, hash->bytes, hash->len);
	prolly_put_varint(out, len);
}

/* Read the reference at *p, and move past it. */
static void
next_value_ref(const char **p, const char *end, ProllyNodeRef *root,
			   uint64 *len)
{
	if (end - *p < 1 || (uint8) (*p)[0] > PROLLY_HASH_MAX_LEN ||
		end - *p < 1 + (uint8) (*p)[0])
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid prolly value reference")));
	memset(root, 0, sizeof(*root));
	root->hash.len = (uint8) (*p)[0];
	memcpy(root->hash.bytes, *p + 1, root->hash.len);
	*p += 1 + root->hash.len;
	*len = prolly_get_varint(p, end);
}

static void
decode_value_ref(const ProllyValue *value, ProllyNodeRef *root, uint64 *len)
{
	const char *p = (const char *) value->data;

	next_value_ref(&p, p + value->len, root, len);
}

/*
 * Building a value tree.  Leaves are written as the rolling hash cuts
 * them; each level above keeps the children of the node it is filling and
 * its chunker, which is given each child's hash as its item's key.
 */
typedef struct ValueLevel
{
	ProllyChunker *chunker;
	ProllyHash *hashes;			/* the node being filled: its children */
	uint64	   *counts;			/* and their byte counts */
	int			n;
	int			max;
	int			emitted;		/* nodes written */
	ProllyNodeRef last;			/* the last of them */
} ValueLevel;

typedef struct ValueBuilder
{
	const ProllyTreeEnv *env;
	StringInfoData child;		/* a child's item, for its chunker */
	ValueLevel	levels[MAX_LEVELS];
} ValueBuilder;

static void value_add(ValueBuilder *vb, uint16 level, const ProllyHash *hash,
					  uint64 count);

/* Write the node level is filling, and add it to the level above. */
static void
value_flush(ValueBuilder *vb, uint16 level)
{
	ValueLevel *vl = &vb->levels[level];
	Item	   *items;
	StringInfoData *children;
	ProllyNodeRef ref;
	uint64		count;
	uint64		bytes = 0;

	if (vl->n == 0)
		return;
	items = palloc0(sizeof(Item) * vl->n);
	children = palloc(sizeof(StringInfoData) * vl->n);
	for (int i = 0; i < vl->n; i++)
	{
		initStringInfo(&children[i]);
		encode_child(&children[i], &vl->hashes[i], vl->counts[i]);
		items[i].value.data = (const uint8 *) children[i].data;
		items[i].value.len = children[i].len;
		items[i].count = vl->counts[i];
		bytes += vl->counts[i];
	}
	write_node(vb->env, level, items, vl->n, &ref, &count);
	for (int i = 0; i < vl->n; i++)
		pfree(children[i].data);
	pfree(children);
	pfree(items);
	vl->n = 0;
	vl->emitted++;
	vl->last = ref;
	value_add(vb, level + 1, &ref.hash, bytes);
}

/* A child of the node level (above the leaves) is filling. */
static void
value_add(ValueBuilder *vb, uint16 level, const ProllyHash *hash, uint64 count)
{
	ValueLevel *vl;
	ProllyKey	key = {hash->bytes, hash->len};
	ProllyKey	nokey = {NULL, 0};
	ProllyValue value;
	ProllyChunkDecision d;

	if (level >= MAX_LEVELS)
		elog(ERROR, "prolly value tree is more than %d levels high", MAX_LEVELS);
	vl = &vb->levels[level];
	if (vl->chunker == NULL)
	{
		vl->chunker = PROLLY_CALL(vb->env->chunker, begin, level,
								  level_capacity(vb->env, level),
								  CurrentMemoryContext);
		vl->max = 64;
		vl->hashes = palloc(sizeof(ProllyHash) * vl->max);
		vl->counts = palloc(sizeof(uint64) * vl->max);
	}
	resetStringInfo(&vb->child);
	encode_child(&vb->child, hash, count);
	value.data = (const uint8 *) vb->child.data;
	value.len = vb->child.len;
	d = PROLLY_CALL(vb->env->chunker, add, vl->chunker, &key, &value,
					PROLLY_CALL(vb->env->chunk_format, item_space, level,
								&nokey, &value));
	if (d == PROLLY_CHUNK_SPLIT_BEFORE)
		value_flush(vb, level);
	if (vl->n == vl->max)
	{
		vl->max *= 2;
		vl->hashes = repalloc(vl->hashes, sizeof(ProllyHash) * vl->max);
		vl->counts = repalloc(vl->counts, sizeof(uint64) * vl->max);
	}
	vl->hashes[vl->n] = *hash;
	vl->counts[vl->n] = count;
	vl->n++;
	if (d == PROLLY_CHUNK_SPLIT_AFTER)
		value_flush(vb, level);
}

/* A leaf: one chunk of the value's bytes. */
static void
value_leaf(ValueBuilder *vb, const uint8 *data, uint32 len)
{
	Item		item = {0};
	ProllyNodeRef ref;
	uint64		count;

	item.value.data = data;
	item.value.len = len;
	item.count = 1;
	write_node(vb->env, 0, &item, len > 0 ? 1 : 0, &ref, &count);
	vb->levels[0].emitted++;
	vb->levels[0].last = ref;
	if (len > 0)
		value_add(vb, 1, &ref.hash, len);
}

/*
 * Does a value's chunk end at its nth byte, chunk[n - 1]?  h is the
 * rolling hash of the chunk so far, 0 as it begins, and moves on by that
 * byte: a boundary where its low bits are all set, past CHUNK_MIN bytes,
 * or at CHUNK_MAX.  (The value's end ends its last chunk too: the caller's
 * to see.)  A chunk's end so depends on its own bytes alone, which a
 * splice relies on to cut only around its edit.
 */
static inline bool
value_cut(uint32 *h, const uint8 *chunk, Size n)
{
	*h = rotl32(*h, 1) ^ buz_table[chunk[n - 1]];
	if (n > BUZ_WINDOW)
		*h ^= rotl32(buz_table[chunk[n - 1 - BUZ_WINDOW]], BUZ_WINDOW);
	return (n >= CHUNK_MIN && (*h & CHUNK_MASK) == CHUNK_MASK) ||
		n >= CHUNK_MAX;
}

/* Store len bytes as a value tree; append its reference to out. */
static void
write_value_tree(const ProllyTreeEnv *env, const uint8 *data, Size len,
				 StringInfo out)
{
	ProllyTreeEnv venv;
	ValueBuilder *vb = palloc0(sizeof(ValueBuilder));
	ProllyNodeRef root;
	Size		start = 0;
	uint32		h = 0;
	uint64		written = env->counters ? env->counters->nodes_written : 0;

	if (!buz_ready)
		buz_init();
	value_env(env, &venv);
	vb->env = &venv;
	initStringInfo(&vb->child);
	for (Size i = 0; i < len; i++)
	{
		Size		n = i - start + 1;	/* bytes in the chunk so far */

		if (value_cut(&h, data + start, n) || i + 1 == len)
		{
			value_leaf(vb, data + start, (uint32) n);
			start = i + 1;
			h = 0;
		}
	}
	if (len == 0)
		value_leaf(vb, NULL, 0);

	/* The first level, counting up, that has exactly one node is the root. */
	for (uint16 level = 0;; level++)
	{
		if (level >= MAX_LEVELS)
			elog(ERROR, "prolly value tree build found no root");
		if (level > 0)
			value_flush(vb, level);
		if (vb->levels[level].emitted == 1)
		{
			root = vb->levels[level].last;
			break;
		}
	}
	for (int i = 0; i < MAX_LEVELS; i++)
	{
		ValueLevel *vl = &vb->levels[i];

		if (vl->chunker == NULL)
			continue;
		PROLLY_CALL(env->chunker, end, vl->chunker);
		pfree(vl->hashes);
		pfree(vl->counts);
	}
	pfree(vb->child.data);
	pfree(vb);
	encode_value_ref(out, &root.hash, len);
	if (env->counters)
		env->counters->value_nodes_written +=
			env->counters->nodes_written - written;
}

/*
 * Declared references too many for an item to keep (a commit of very many
 * parents, a merge state of thousands of commits): kept in a tree of their
 * own, whose leaves declare them (PROLLY_ITEM_CHUNK_REFS, all of each
 * leaf's value), so a collection follows them through it; the item
 * declares the tree's root alone.  Nothing reads them but a collection.
 */
#define CHUNK_REFS_LEAF_MAX	(16 * 1024)
static void
write_refs_tree(const ProllyTreeEnv *env, const char *refs, Size len,
				StringInfo out)
{
	ProllyTreeEnv venv;
	ValueBuilder *vb = palloc0(sizeof(ValueBuilder));
	ProllyNodeRef root;
	StringInfoData leaf;
	const char *p = refs;
	const char *end = refs + len;

	value_env(env, &venv);
	vb->env = &venv;
	initStringInfo(&vb->child);
	initStringInfo(&leaf);
	while (p < end)
	{
		const char *from = p;
		Item		item = {0};
		ProllyNodeRef ref;
		uint64		count;

		/* (whole references, up to a leaf's worth) */
		while (p < end && p - from < CHUNK_REFS_LEAF_MAX)
		{
			ProllyNodeRef r;
			uint64		n;

			next_value_ref(&p, end, &r, &n);
		}
		resetStringInfo(&leaf);
		prolly_put_varint(&leaf, p - from);
		appendBinaryStringInfo(&leaf, from, p - from);
		item.value.data = (const uint8 *) leaf.data;
		item.value.len = leaf.len;
		item.count = 1;
		item.flags = PROLLY_ITEM_CHUNK_REFS;
		write_node(vb->env, 0, &item, 1, &ref, &count);
		vb->levels[0].emitted++;
		vb->levels[0].last = ref;
		value_add(vb, 1, &ref.hash, p - from);
	}
	for (uint16 level = 0;; level++)
	{
		if (level >= MAX_LEVELS)
			elog(ERROR, "prolly references tree build found no root");
		if (level > 0)
			value_flush(vb, level);
		if (vb->levels[level].emitted == 1)
		{
			root = vb->levels[level].last;
			break;
		}
	}
	for (int i = 0; i < MAX_LEVELS; i++)
	{
		ValueLevel *vl = &vb->levels[i];

		if (vl->chunker == NULL)
			continue;
		PROLLY_CALL(env->chunker, end, vl->chunker);
		pfree(vl->hashes);
		pfree(vl->counts);
	}
	pfree(leaf.data);
	pfree(vb->child.data);
	pfree(vb);
	encode_value_ref(out, &root.hash, len);
}

/*
 * Tell the store which children of a value tree's internal node a read of
 * bytes [offset, offset + len) of it reads, so it may read them together.
 */
static void
prefetch_range(const ProllyTreeEnv *env, TNode *node, uint64 offset,
			   uint64 len)
{
	ProllyHash *hashes;
	int			n = 0;

	if (env->chunk_store == NULL || !PROLLY_HAS(env->chunk_store, prefetch) ||
		node->nitems < 2)
		return;
	hashes = palloc(sizeof(ProllyHash) * node->nitems);
	for (int i = 0; i < node->nitems && len > 0; i++)
	{
		ProllyNodeRef child;
		uint64		count;
		uint64		take;

		node_child(env, node, i, &child, &count);
		if (offset >= count)
		{
			offset -= count;
			continue;
		}
		hashes[n++] = child.hash;
		take = Min(len, count - offset);
		len -= take;
		offset = 0;
	}
	if (n > 1)
		(void) PROLLY_CALL(env->chunk_store, prefetch, env->chunks, hashes, n);
	pfree(hashes);
}

/*
 * Append bytes [offset, offset + len) of the subtree at ref, len > 0, which
 * its parent (or the reference, for the root) says holds size bytes.
 */
static void
read_value_range(const ProllyTreeEnv *env, const ProllyNodeRef *ref, int level,
				 uint64 size, uint64 offset, uint64 len, StringInfo out)
{
	TNode		node;

	CHECK_FOR_INTERRUPTS();
	node_read_level(env, ref, &node, CurrentMemoryContext, false, level);
	if (node.level == 0)
	{
		ProllyKey	k;
		ProllyValue v = {NULL, 0};

		if (node.nitems > 0)
			node_item(env, &node, 0, &k, &v, NULL);
		if (node.nitems > 1 || v.len != size)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("prolly value tree leaf holds %u bytes, its parent says %llu",
							v.len, (unsigned long long) size)));
		appendBinaryStringInfo(out, (const char *) v.data + offset, (int) len);
	}
	else
	{
		uint64		held = 0;

		prefetch_range(env, &node, offset, len);
		for (int i = 0; i < node.nitems; i++)
		{
			ProllyNodeRef child;
			uint64		count;
			uint64		take;

			node_child(env, &node, i, &child, &count);
			held += count;
			if (len == 0 || offset >= count)
			{
				offset -= Min(offset, count);
				continue;
			}
			take = Min(len, count - offset);
			read_value_range(env, &child, node.level - 1, count, offset, take, out);
			len -= take;
			offset = 0;
		}
		if (held != size)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("prolly value tree node holds %llu bytes, its parent or reference says %llu",
							(unsigned long long) held, (unsigned long long) size)));
	}
	node_release(&node);
}

/*
 * Append bytes [offset, offset + len) of the value a reference refers to,
 * as many as there are.
 */
static void
read_value(const ProllyTreeEnv *env, const ProllyValue *ref, uint64 offset,
		   uint64 len, StringInfo out)
{
	ProllyTreeEnv venv;
	ProllyNodeRef root;
	uint64		total;

	value_env(env, &venv);
	decode_value_ref(ref, &root, &total);
	if (offset >= total || len == 0)
		return;
	len = Min(len, total - offset);
	if (len > (uint64) (MaxAllocSize - 1) - (uint64) out->len)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("prolly value of %llu bytes is too large to read",
						(unsigned long long) len)));
	enlargeStringInfo(out, (int) len);
	read_value_range(&venv, &root, LEVEL_ANY, total, offset, len, out);
}

/* Append the bytes of the value a reference refers to. */
static void
resolve_value(const ProllyTreeEnv *env, const ProllyValue *ref, StringInfo out)
{
	read_value(env, ref, 0, PG_UINT64_MAX, out);
}

/*
 * Splicing a value tree.  A level's nodes as a cursor sees them: one node
 * at a time, from the node an edit's path holds, reading each parent as it
 * is reached, so only the nodes around the edit are read.
 */
typedef struct SpliceNode
{
	ProllyHash	hash;
	uint64		count;
	int			nkids;			/* above the leaves, a node made: its children */
	ProllyHash	only;			/* and with one child, it */
} SpliceNode;

typedef struct SpliceLevel
{
	SpliceNode *nodes;
	int			n;
	int			max;
} SpliceLevel;

/* A cursor over one level's old nodes. */
typedef struct SpliceCursor
{
	bool		top;			/* the root's level: the root alone */
	bool		at_end;
	TNode		parent;			/* not top: the node above holding the node */
	int			index;			/* the node's place among its children */
	ProllyNodeRef ref;			/* the node */
	uint64		count;			/* its bytes */
	uint64		start;			/* where they begin in the value */
} SpliceCursor;

static void
splice_push(SpliceLevel *sl, const ProllyHash *hash, uint64 count, int nkids,
			const ProllyHash *only)
{
	if (sl->n == sl->max)
	{
		sl->max = sl->max ? sl->max * 2 : 16;
		sl->nodes = sl->nodes ? repalloc(sl->nodes, sizeof(SpliceNode) * sl->max) :
			palloc(sizeof(SpliceNode) * sl->max);
	}
	memset(&sl->nodes[sl->n], 0, sizeof(SpliceNode));
	sl->nodes[sl->n].hash = *hash;
	sl->nodes[sl->n].count = count;
	sl->nodes[sl->n].nkids = nkids;
	if (only)
		sl->nodes[sl->n].only = *only;
	sl->n++;
}

/* Cursor k on child i of its parent (already read). */
static void
splice_cursor_at(const ProllyTreeEnv *env, SpliceCursor *c, int i)
{
	c->index = i;
	node_child(env, &c->parent, i, &c->ref, &c->count);
}

/*
 * Move cursor k (of levels 0 .. height) to the next node of its level:
 * the next child of its parent, or the first of the parent's next sibling,
 * which the cursor above moves to.
 */
static void
splice_next(const ProllyTreeEnv *env, SpliceCursor *cs, int k)
{
	SpliceCursor *c = &cs[k];

	if (c->at_end)
		return;
	c->start += c->count;
	if (c->top)
	{
		c->at_end = true;
		return;
	}
	if (c->index + 1 < c->parent.nitems)
	{
		splice_cursor_at(env, c, c->index + 1);
		return;
	}
	splice_next(env, cs, k + 1);
	node_release(&c->parent);
	if (cs[k + 1].at_end)
	{
		c->at_end = true;
		return;
	}
	node_read_level(env, &cs[k + 1].ref, &c->parent, CurrentMemoryContext, false,
					k + 1);
	splice_cursor_at(env, c, 0);
}

/* A level above the leaves being made again: its chunker, its node. */
typedef struct SpliceFill
{
	const ProllyTreeEnv *env;
	uint16		level;
	ProllyChunker *chunker;
	SpliceNode *kids;			/* the node being filled */
	int			n;
	int			max;
	SpliceLevel out;			/* the nodes made */
	StringInfoData child;
} SpliceFill;

static void
splice_flush(SpliceFill *sf)
{
	Item	   *items;
	StringInfoData *children;
	ProllyNodeRef ref;
	uint64		count;
	uint64		bytes = 0;

	if (sf->n == 0)
		return;
	items = palloc0(sizeof(Item) * sf->n);
	children = palloc(sizeof(StringInfoData) * sf->n);
	for (int i = 0; i < sf->n; i++)
	{
		initStringInfo(&children[i]);
		encode_child(&children[i], &sf->kids[i].hash, sf->kids[i].count);
		items[i].value.data = (const uint8 *) children[i].data;
		items[i].value.len = children[i].len;
		items[i].count = sf->kids[i].count;
		bytes += sf->kids[i].count;
	}
	write_node(sf->env, sf->level, items, sf->n, &ref, &count);
	for (int i = 0; i < sf->n; i++)
		pfree(children[i].data);
	pfree(children);
	pfree(items);
	splice_push(&sf->out, &ref.hash, bytes, sf->n,
				sf->n == 1 ? &sf->kids[0].hash : NULL);
	sf->n = 0;
}

/* Feed a child to the level being made; true if it ended a node. */
static bool
splice_fill(SpliceFill *sf, const ProllyHash *hash, uint64 count)
{
	ProllyKey	key = {hash->bytes, hash->len};
	ProllyKey	nokey = {NULL, 0};
	ProllyValue value;
	ProllyChunkDecision d;

	resetStringInfo(&sf->child);
	encode_child(&sf->child, hash, count);
	value.data = (const uint8 *) sf->child.data;
	value.len = sf->child.len;
	d = PROLLY_CALL(sf->env->chunker, add, sf->chunker, &key, &value,
					PROLLY_CALL(sf->env->chunk_format, item_space, sf->level,
								&nokey, &value));
	if (d == PROLLY_CHUNK_SPLIT_BEFORE)
		splice_flush(sf);
	if (sf->n == sf->max)
	{
		sf->max = sf->max ? sf->max * 2 : 64;
		sf->kids = sf->kids ? repalloc(sf->kids, sizeof(SpliceNode) * sf->max) :
			palloc(sizeof(SpliceNode) * sf->max);
	}
	memset(&sf->kids[sf->n], 0, sizeof(SpliceNode));
	sf->kids[sf->n].hash = *hash;
	sf->kids[sf->n].count = count;
	sf->n++;
	if (d == PROLLY_CHUNK_SPLIT_AFTER)
	{
		splice_flush(sf);
		return true;
	}
	return false;
}

static void
splice_fill_begin(SpliceFill *sf, const ProllyTreeEnv *env, uint16 level)
{
	memset(sf, 0, sizeof(*sf));
	sf->env = env;
	sf->level = level;
	sf->chunker = PROLLY_CALL(env->chunker, begin, level,
							  level_capacity(env, level), CurrentMemoryContext);
	initStringInfo(&sf->child);
}

static void
splice_fill_end(SpliceFill *sf)
{
	PROLLY_CALL(sf->env->chunker, end, sf->chunker);
	if (sf->kids)
		pfree(sf->kids);
	pfree(sf->child.data);
}

/* A leaf a splice cut: written, and added to the new leaves. */
static void
splice_leaf(const ProllyTreeEnv *env, const char *data, uint32 len,
			SpliceLevel *leaves)
{
	Item		item = {0};
	ProllyNodeRef ref;
	uint64		count;

	item.value.data = (const uint8 *) data;
	item.value.len = len;
	item.count = 1;
	write_node(env, 0, &item, 1, &ref, &count);
	splice_push(leaves, &ref.hash, len, 0, NULL);
}

/* The bytes of the leaf at ref, which its parent says holds count. */
static void
splice_leaf_bytes(const ProllyTreeEnv *env, const ProllyNodeRef *ref,
				  uint64 count, uint64 from, StringInfo out)
{
	read_value_range(env, ref, 0, count, from, count - from, out);
}

/*
 * The value ref names, bytes [offset, offset + del) replaced by insert's
 * ilen bytes, as a value tree; its reference appended to out.
 *
 * The tree is walked down to the leaf holding the edit, a cursor at each
 * level on the edit's path.  The new bytes are cut from that leaf's start
 * as write_value_tree() cuts them (each leaf's cut depends on its bytes
 * alone), the old bytes after the edit read a leaf at a time as needed,
 * until a cut past the edit falls where an old leaf begins.  Then each
 * level above is made again from the start of the node on the path, with
 * a chunker begun there (the build's decides from the items since its node
 * began): that node's children before the path, the nodes made below, and
 * the old ones after them, until a node ends where an old node begins.  So
 * an edit reads the nodes on its path, the leaves it touches and the few
 * nodes around them, and writes O(depth + its length) nodes.
 */
static void
splice_value(const ProllyTreeEnv *env, const ProllyValue *ref, uint64 offset,
			 uint64 del, const uint8 *insert, Size ilen, StringInfo out)
{
	ProllyTreeEnv venv;
	ProllyNodeRef root;
	uint64		total;
	uint64		newtotal;
	TNode		rnode;
	uint16		height;
	SpliceCursor cs[MAX_LEVELS];
	TNode		path[MAX_LEVELS];	/* the node on the edit's path, by level */
	int			pidx[MAX_LEVELS];	/* the path's child's place in it */
	SpliceLevel made = {0};		/* the nodes made at the level below */
	uint64		written = env->counters ? env->counters->nodes_written : 0;
	ProllyHash	top;

	value_env(env, &venv);
	decode_value_ref(ref, &root, &total);
	if (offset > total)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("splice at byte %llu of a value of %llu bytes",
						(unsigned long long) offset, (unsigned long long) total)));
	del = Min(del, total - offset);
	newtotal = total - del + ilen;

	node_read(&venv, &root, &rnode);
	height = rnode.level;

	/* A value of one leaf, or none left: written whole, being small. */
	if (height == 0 || newtotal == 0)
	{
		StringInfoData whole;

		node_release(&rnode);
		initStringInfo(&whole);
		read_value(env, ref, 0, offset, &whole);
		appendBinaryStringInfo(&whole, (const char *) insert, ilen);
		read_value(env, ref, offset + del, PG_UINT64_MAX, &whole);
		write_value_tree(env, (const uint8 *) whole.data, whole.len, out);
		pfree(whole.data);
		return;
	}
	if (!buz_ready)
		buz_init();

	/* down to the leaf holding the first byte edited (appending: the last) */
	memset(cs, 0, sizeof(cs));
	cs[height].top = true;
	cs[height].ref = root;
	cs[height].count = total;
	path[height] = rnode;
	for (int k = height - 1; k >= 0; k--)
	{
		SpliceCursor *c = &cs[k];
		int			i = 0;

		node_read_level(&venv, &cs[k + 1].ref, &c->parent, CurrentMemoryContext,
						false, k + 1);
		if (k + 1 < height)
			path[k + 1] = c->parent;
		c->start = cs[k + 1].start;
		splice_cursor_at(&venv, c, 0);
		while (i + 1 < c->parent.nitems && c->start + c->count <= offset)
		{
			c->start += c->count;
			splice_cursor_at(&venv, c, ++i);
		}
		pidx[k + 1] = i;
	}
	/* (each path node a copy of its own: the cursors' parents move on) */
	for (int k = 1; k < height; k++)
	{
		TNode		copy = path[k];
		char	   *data = palloc(copy.chunk.len);

		memcpy(data, copy.chunk.data, copy.chunk.len);
		copy.chunk.data = (const uint8 *) data;
		path[k] = copy;
	}

	/* the leaves: from the path's leaf, until a cut meets an old leaf */
	{
		StringInfoData chunk;
		StringInfoData tail;
		uint64		pos = cs[0].start;	/* the next new byte's position */
		uint64		op = offset + del;	/* the next old byte after the edit */
		uint64		edit_end = offset + ilen;	/* in the new bytes */
		int64		shift = (int64) ilen - (int64) del;
		uint32		h = 0;

		initStringInfo(&chunk);
		initStringInfo(&tail);
		if (offset > cs[0].start)
			read_value_range(&venv, &cs[0].ref, 0, cs[0].count, 0,
							 offset - cs[0].start, &tail);
		appendBinaryStringInfo(&tail, (const char *) insert, ilen);
		for (;;)
		{
			bool		cut = false;

			for (int t = 0; t < tail.len; t++)
			{
				appendStringInfoChar(&chunk, tail.data[t]);
				pos++;
				cut = value_cut(&h, (const uint8 *) chunk.data, chunk.len) ||
					pos == newtotal;
				if (cut)
				{
					splice_leaf(&venv, chunk.data, chunk.len, &made);
					resetStringInfo(&chunk);
					h = 0;
				}
			}
			if (pos == newtotal)
				break;
			/* (past leaves the edit deleted, unread) */
			while (!cs[0].at_end && cs[0].start + cs[0].count <= op)
				splice_next(&venv, cs, 0);
			/* a cut past the edit where an old leaf begins: the rest are old */
			if (cut && pos >= edit_end && (int64) pos - shift == (int64) op &&
				!cs[0].at_end && cs[0].start == op)
				break;
			Assert(!cs[0].at_end);
			resetStringInfo(&tail);
			splice_leaf_bytes(&venv, &cs[0].ref, cs[0].count, op - cs[0].start, &tail);
			op = cs[0].start + cs[0].count;
			splice_next(&venv, cs, 0);
		}
		/* (the old leaves the edit replaced passed: the cursor on the first kept) */
		while (!cs[0].at_end && cs[0].start + cs[0].count <= op)
			splice_next(&venv, cs, 0);
		Assert(chunk.len == 0);
		pfree(chunk.data);
		pfree(tail.data);
	}

	/*
	 * Each level above, from its path node's start: its children before the
	 * path, the nodes made below, then the old ones the cursor below is on,
	 * until a node ends where an old node begins.
	 */
	for (uint16 level = 1; level <= height; level++)
	{
		SpliceFill	sf;
		TNode	   *pn = &path[level];
		bool		resynced = false;
		bool		cut = false;

		splice_fill_begin(&sf, &venv, level);
		for (int i = 0; i < pidx[level]; i++)
		{
			ProllyNodeRef child;
			uint64		count;

			node_child(&venv, pn, i, &child, &count);
			(void) splice_fill(&sf, &child.hash, count);
		}
		for (int i = 0; i < made.n; i++)
			cut = splice_fill(&sf, &made.nodes[i].hash, made.nodes[i].count);
		for (;;)
		{
			SpliceCursor *below = &cs[level - 1];

			if (below->at_end)
				break;
			if (cut && !below->top && below->index == 0)
			{
				resynced = true;
				break;
			}
			cut = splice_fill(&sf, &below->ref.hash, below->count);
			splice_next(&venv, cs, level - 1);
		}
		if (!resynced)
			splice_flush(&sf);
		pfree(made.nodes);
		made = sf.out;
		splice_fill_end(&sf);
	}

	/*
	 * The root level made: one node, or more, a level grown above them.  A
	 * root of one child is no root: the first level counting up that has a
	 * single node is.
	 */
	for (uint16 level = height + 1; made.n > 1; level++)
	{
		SpliceFill	sf;

		if (level >= MAX_LEVELS)
			elog(ERROR, "prolly value tree is more than %d levels high", MAX_LEVELS);
		splice_fill_begin(&sf, &venv, level);
		for (int i = 0; i < made.n; i++)
			(void) splice_fill(&sf, &made.nodes[i].hash, made.nodes[i].count);
		splice_flush(&sf);
		pfree(made.nodes);
		made = sf.out;
		splice_fill_end(&sf);
	}
	top = made.nodes[0].hash;
	if (made.nodes[0].nkids == 1)
	{
		/* (one child: it, and so on down while each has one) */
		ProllyNodeRef r = {0};

		r.hash = made.nodes[0].only;
		for (int want = LEVEL_ANY;;)
		{
			TNode		n;
			ProllyNodeRef child;
			uint64		count;

			top = r.hash;
			node_read_level(&venv, &r, &n, CurrentMemoryContext, false, want);
			want = (int) n.level - 1;
			if (n.level == 0 || n.nitems != 1)
			{
				node_release(&n);
				break;
			}
			node_child(&venv, &n, 0, &child, &count);
			node_release(&n);
			r = child;
		}
	}
	encode_value_ref(out, &top, newtotal);
	pfree(made.nodes);
	for (int k = 0; k < height; k++)
		node_release(&cs[k].parent);
	for (int k = 1; k <= height; k++)
		node_release(&path[k]);
	if (env->counters)
		env->counters->value_nodes_written +=
			env->counters->nodes_written - written;
}

static void
cow_value_reference(const ProllyHash *root, uint64 len, StringInfo ref)
{
	encode_value_ref(ref, root, len);
}

static void
cow_value_root(const ProllyValue *ref, ProllyHash *root, uint64 *len)
{
	ProllyNodeRef r;

	decode_value_ref(ref, &r, len);
	*root = r.hash;
}

/* (a tree's nodes go into the store before its root does) */
static bool
cow_value_present(const ProllyTreeEnv *env, const ProllyValue *ref)
{
	ProllyNodeRef root;
	uint64		len;

	decode_value_ref(ref, &root, &len);
	return PROLLY_CALL(env->chunk_store, has, env->chunks, &root.hash);
}

/*
 * A leaf item for key and value: the value stored out of line if it is
 * long, and led by the references it holds if the environment finds any.
 * What the item stores is built in buf, which it points into.
 */
static void
leaf_item(const ProllyTreeEnv *env, const ProllyKey *key,
		  const ProllyValue *value, Item *item, StringInfo buf)
{
	int			first = buf->len;

	item->key = *key;
	item->count = 1;
	item->flags = 0;
	if (env->value_refs)
	{
		int			start = buf->len;
		int			n;

		/* the references, then their length put before them */
		env->value_refs(value, env->value_refs_arg, buf);
		n = buf->len - start;
		if (n > 0)
		{
			StringInfoData len;
			char		lenbuf[16];

			len.data = lenbuf;
			len.len = 0;
			len.maxlen = sizeof(lenbuf);
			prolly_put_varint(&len, n);
			enlargeStringInfo(buf, len.len);
			memmove(buf->data + start + len.len, buf->data + start, n);
			memcpy(buf->data + start, len.data, len.len);
			buf->len += len.len;
			buf->data[buf->len] = '\0';
			item->flags |= PROLLY_ITEM_VALUE_REFS;
		}
	}
	if (env->chunk_refs)
	{
		int			start = buf->len;
		int			n;

		/* the chunks named, appended, then moved before all of it */
		env->chunk_refs(key, value, env->chunk_refs_arg, buf);
		n = buf->len - start;
		if (n > CHUNK_REFS_INLINE_MAX)
		{
			/* (too many to keep inline: a tree of them, the item naming it) */
			char	   *all = palloc(n);

			memcpy(all, buf->data + start, n);
			buf->len = start;
			write_refs_tree(env, all, n, buf);
			pfree(all);
			n = buf->len - start;
		}
		if (n > 0)
		{
			char		lenbuf[16];
			StringInfoData len = {lenbuf, 0, sizeof(lenbuf), 0};
			char		refsbuf[CHUNK_REFS_INLINE_MAX];

			/* [first, start) the value's own references; then n of these */
			prolly_put_varint(&len, n);
			memcpy(refsbuf, buf->data + start, n);
			enlargeStringInfo(buf, len.len);
			memmove(buf->data + first + len.len + n, buf->data + first,
					start - first);
			memcpy(buf->data + first, len.data, len.len);
			memcpy(buf->data + first + len.len, refsbuf, n);
			buf->len += len.len;
			buf->data[buf->len] = '\0';
			item->flags |= PROLLY_ITEM_CHUNK_REFS;
		}
	}
	if (value->len > VALUE_INLINE_MAX)
	{
		item->flags |= PROLLY_ITEM_VALUE_REF;
		write_value_tree(env, value->data, value->len, buf);
	}
	else
		appendBinaryStringInfo(buf, (const char *) value->data, value->len);
	item->value.data = (const uint8 *) buf->data;
	item->value.len = buf->len;
}

/*
 * A leaf item's value as stored, without the references it leads with:
 * the value, or its value tree's reference.  Sets *refs to those (empty if
 * none).
 */
static void
item_payload(ProllyValue *value, uint8 flags, ProllyValue *refs,
			 ProllyValue *chunk_refs)
{
	if (chunk_refs)
	{
		chunk_refs->data = value->data;
		chunk_refs->len = 0;
	}
	if (flags & PROLLY_ITEM_CHUNK_REFS)
	{
		const char *p = (const char *) value->data;
		const char *end = p + value->len;
		uint64		n = prolly_get_varint(&p, end);

		if (n > (uint64) (end - p))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid prolly chunk references")));
		if (chunk_refs)
		{
			chunk_refs->data = (const uint8 *) p;
			chunk_refs->len = (uint32) n;
		}
		value->len -= (p + n) - (const char *) value->data;
		value->data = (const uint8 *) (p + n);
	}
	refs->data = value->data;
	refs->len = 0;
	if (flags & PROLLY_ITEM_VALUE_REFS)
	{
		const char *p = (const char *) value->data;
		const char *end = p + value->len;
		uint64		n = prolly_get_varint(&p, end);

		if (n > (uint64) (end - p))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid prolly value references")));
		refs->data = (const uint8 *) p;
		refs->len = (uint32) n;
		value->len -= (p + n) - (const char *) value->data;
		value->data = (const uint8 *) (p + n);
	}
}

/*
 * A leaf value as read, without the references it leads with: if it is out
 * of line, its bytes are appended to out (true); otherwise value is left
 * pointing at them.
 */
static bool
leaf_value_into(const ProllyTreeEnv *env, ProllyValue *value, uint8 flags,
				StringInfo out)
{
	ProllyValue refs;

	item_payload(value, flags, &refs, NULL);
	if (!(flags & PROLLY_ITEM_VALUE_REF))
		return false;
	resolve_value(env, value, out);
	return true;
}

/* A leaf value as read: its bytes, from its value tree if out of line. */
static void
leaf_value(const ProllyTreeEnv *env, ProllyValue *value, uint8 flags,
		   StringInfo buf)
{
	if (flags & PROLLY_ITEM_VALUE_REF)
		resetStringInfo(buf);
	if (leaf_value_into(env, value, flags, buf))
	{
		value->data = (const uint8 *) buf->data;
		value->len = buf->len;
	}
}

/* ------------------------------------------------------------------------
 * Building one level
 * ------------------------------------------------------------------------
 */

/* Called with each node a level closes. */
typedef void (*EmitNode) (void *arg, uint16 level, const ProllyKey *maxkey,
						  const ProllyNodeRef *ref, uint64 count);

typedef struct LevelBuf
{
	const ProllyTreeEnv *env;
	uint16		level;
	Size		capacity;
	ProllyChunker *chunker;
	MemoryContext cxt;			/* item copies; reset after each node */
	Item	   *items;
	int			nitems;
	int			maxitems;
	EmitNode	emit;
	void	   *emit_arg;
} LevelBuf;

static void
levelbuf_init(LevelBuf *lb, const ProllyTreeEnv *env, uint16 level,
			  EmitNode emit, void *emit_arg)
{
	lb->env = env;
	lb->level = level;
	lb->capacity = level_capacity(env, level);
	lb->chunker = PROLLY_CALL(env->chunker, begin, level, lb->capacity,
							  CurrentMemoryContext);
	lb->cxt = AllocSetContextCreate(CurrentMemoryContext, "prolly level",
									ALLOCSET_DEFAULT_SIZES);
	lb->maxitems = 64;
	lb->items = palloc(sizeof(Item) * lb->maxitems);
	lb->nitems = 0;
	lb->emit = emit;
	lb->emit_arg = emit_arg;
}

static void
levelbuf_free(LevelBuf *lb)
{
	PROLLY_CALL(lb->env->chunker, end, lb->chunker);
	MemoryContextDelete(lb->cxt);
	pfree(lb->items);
}

static void
levelbuf_flush(LevelBuf *lb)
{
	ProllyNodeRef ref;
	uint64		count;

	if (lb->nitems == 0)
		return;
	write_node(lb->env, lb->level, lb->items, lb->nitems, &ref, &count);
	lb->emit(lb->emit_arg, lb->level, &lb->items[lb->nitems - 1].key, &ref, count);
	lb->nitems = 0;
	MemoryContextReset(lb->cxt);
}

static void
levelbuf_append(LevelBuf *lb, const Item *item)
{
	Item	   *copy;
	uint8	   *buf;

	if (lb->nitems == lb->maxitems)
	{
		lb->maxitems *= 2;
		lb->items = repalloc(lb->items, sizeof(Item) * lb->maxitems);
	}
	copy = &lb->items[lb->nitems++];
	*copy = *item;
	buf = MemoryContextAlloc(lb->cxt, Max(item->key.len + item->value.len, 1));
	memcpy(buf, item->key.data, item->key.len);
	memcpy(buf + item->key.len, item->value.data, item->value.len);
	copy->key.data = buf;
	copy->value.data = buf + item->key.len;
}

static void
levelbuf_add(LevelBuf *lb, const Item *item)
{
	Size		space = PROLLY_CALL(lb->env->chunk_format, item_space, lb->level,
									&item->key, &item->value);
	ProllyChunkDecision d;

	if (space > lb->capacity)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("prolly item of %zu bytes does not fit in a node", space),
				 errdetail("Items larger than a node are not supported yet.")));

	/*
	 * An internal node holds two items at least, or the levels above would
	 * not narrow: a key too large for two of its internal items to fit is
	 * refused.
	 */
	if (lb->level > 0 && space > lb->capacity / 2)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("prolly key of %u bytes is too large", item->key.len),
				 errdetail("A key may take at most half a tree node (%zu bytes) with its child's reference.",
						   lb->capacity / 2)));

	d = PROLLY_CALL(lb->env->chunker, add, lb->chunker, &item->key, &item->value,
					space);
	if (d == PROLLY_CHUNK_SPLIT_BEFORE)
		levelbuf_flush(lb);
	levelbuf_append(lb, item);
	if (d == PROLLY_CHUNK_SPLIT_AFTER)
		levelbuf_flush(lb);
}

/* ------------------------------------------------------------------------
 * Building a whole tree (or the levels from some level up)
 * ------------------------------------------------------------------------
 */

struct ProllyTreeBuilder
{
	const ProllyTreeEnv *env;
	MemoryContext cxt;
	uint16		base;			/* lowest level being built */
	bool		started[MAX_LEVELS];
	LevelBuf	levels[MAX_LEVELS];
	int			emitted[MAX_LEVELS];
	ProllyNodeRef last_ref[MAX_LEVELS];
	bool		any_items;
};

static void builder_add_at(ProllyTreeBuilder *b, uint16 level, const Item *item);

static void
builder_emit(void *arg, uint16 level, const ProllyKey *maxkey,
			 const ProllyNodeRef *ref, uint64 count)
{
	ProllyTreeBuilder *b = arg;
	StringInfoData child;
	Item		parent;

	b->emitted[level]++;
	b->last_ref[level] = *ref;

	initStringInfo(&child);
	encode_child(&child, &ref->hash, count);
	parent.key = *maxkey;
	parent.value.data = (const uint8 *) child.data;
	parent.value.len = child.len;
	parent.count = count;
	parent.flags = 0;
	builder_add_at(b, level + 1, &parent);
	pfree(child.data);
}

static void
builder_add_at(ProllyTreeBuilder *b, uint16 level, const Item *item)
{
	if (level >= MAX_LEVELS)
		elog(ERROR, "prolly tree is more than %d levels high", MAX_LEVELS);
	if (!b->started[level])
	{
		MemoryContext old = MemoryContextSwitchTo(b->cxt);

		levelbuf_init(&b->levels[level], b->env, level, builder_emit, b);
		b->started[level] = true;
		MemoryContextSwitchTo(old);
	}
	b->any_items = true;
	levelbuf_add(&b->levels[level], item);
}

static ProllyTreeBuilder *
builder_create(const ProllyTreeEnv *env, uint16 base, MemoryContext mcxt)
{
	ProllyTreeBuilder *b = MemoryContextAllocZero(mcxt, sizeof(ProllyTreeBuilder));

	b->env = env;
	b->base = base;
	b->cxt = AllocSetContextCreate(mcxt, "prolly tree builder",
								   ALLOCSET_DEFAULT_SIZES);
	return b;
}

/* An empty leaf: the empty tree. */
static void
empty_tree(const ProllyTreeEnv *env, ProllyNodeRef *root)
{
	uint64		count;

	write_node(env, 0, NULL, 0, root, &count);
}

static void
builder_finish(ProllyTreeBuilder *b, ProllyNodeRef *root)
{
	MemoryContext old = MemoryContextSwitchTo(b->cxt);

	if (!b->any_items)
		empty_tree(b->env, root);
	else
	{
		for (uint16 level = b->base;; level++)
		{
			/* Each level with more than one node feeds the one above. */
			if (level >= MAX_LEVELS || !b->started[level])
				elog(ERROR, "prolly tree build found no root at level %u", level);
			levelbuf_flush(&b->levels[level]);
			if (b->emitted[level] == 1)
			{
				/* The first level with one node is the root. */
				*root = b->last_ref[level];
				break;
			}
		}
	}
	for (int i = 0; i < MAX_LEVELS; i++)
	{
		if (b->started[i])
			levelbuf_free(&b->levels[i]);
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(b->cxt);
	pfree(b);
}

static ProllyTreeBuilder *
cow_build_begin(const ProllyTreeEnv *env, MemoryContext mcxt)
{
	return builder_create(env, 0, mcxt);
}

static void
cow_build_add(ProllyTreeBuilder *b, const ProllyKey *key, const ProllyValue *value)
{
	Item		item;
	StringInfoData stored;

	initStringInfo(&stored);
	leaf_item(b->env, key, value, &item, &stored);
	builder_add_at(b, 0, &item);
	pfree(stored.data);
}

static void
cow_build_finish(ProllyTreeBuilder *b, ProllyNodeRef *root)
{
	builder_finish(b, root);
}

/* ------------------------------------------------------------------------
 * Iterating one level of an existing tree
 * ------------------------------------------------------------------------
 */

typedef struct Frame
{
	TNode		node;
	int			idx;
} Frame;

typedef struct Iter
{
	const ProllyTreeEnv *env;
	MemoryContext cxt;			/* where the nodes it keeps are */
	ProllyNodeRef root;
	int			target;			/* frame index of the level iterated */
	Frame		f[MAX_LEVELS];
	bool		at_end;
} Iter;

static uint16
iter_root_level(const Iter *it)
{
	return it->f[0].node.level;
}

static void
iter_open(Iter *it, const ProllyTreeEnv *env, const ProllyNodeRef *root,
		  uint16 level)
{
	memset(it, 0, sizeof(*it));
	it->env = env;
	it->cxt = CurrentMemoryContext;
	it->root = *root;
	node_read_keep(env, root, &it->f[0].node, it->cxt, true);
	if (level > it->f[0].node.level)
		elog(ERROR, "prolly tree has no level %u", level);
	it->target = it->f[0].node.level - level;
}

static void
iter_close(Iter *it)
{
	for (int i = 0; i <= it->target; i++)
		node_release(&it->f[i].node);
}

/* Load the child of frame d at its idx into frame d + 1. */
static void
iter_load_child(Iter *it, int d)
{
	ProllyNodeRef ref;
	uint64		count;

	node_child(it->env, &it->f[d].node, it->f[d].idx, &ref, &count);
	/* Already loaded (a seek near the last one): keep it, unread. */
	if (node_loaded(&it->f[d + 1].node) &&
		it->f[d + 1].node.ref.hash.len == ref.hash.len &&
		memcmp(it->f[d + 1].node.ref.hash.bytes, ref.hash.bytes,
			   ref.hash.len) == 0)
		return;
	node_release(&it->f[d + 1].node);
	node_read_child(it->env, &ref, &it->f[d + 1].node, it->cxt,
					it->f[d].node.level);
}

/*
 * Position at the start of the target-level node that holds key (the first
 * whose largest key is >= key, else the last); NULL means the first.  With
 * rightmost, position at the last item of the last node instead.
 */
static void
iter_seek_node(Iter *it, const ProllyKey *key, bool rightmost)
{
	it->at_end = false;
	for (int d = 0; d <= it->target; d++)
	{
		TNode	   *node = &it->f[d].node;

		if (d == it->target)
			it->f[d].idx = rightmost ? node->nitems - 1 : 0;
		else
		{
			if (rightmost)
				it->f[d].idx = node->nitems - 1;
			else if (key == NULL)
				it->f[d].idx = 0;
			else
				it->f[d].idx = Min(lower_bound(it->env, node, key), node->nitems - 1);
			iter_load_child(it, d);
		}
	}
	if (it->f[it->target].node.nitems == 0)
		it->at_end = true;
}

/* Is there a target-level node after the one it is in?  (Nothing read.) */
static bool
iter_has_next_node(const Iter *it)
{
	for (int d = it->target - 1; d >= 0; d--)
	{
		if (it->f[d].idx + 1 < it->f[d].node.nitems)
			return true;
	}
	return false;
}

/* Move to the next target-level node (idx 0); false at the end. */
static bool
iter_next_node(Iter *it)
{
	int			d;

	for (d = it->target - 1; d >= 0; d--)
	{
		if (it->f[d].idx + 1 < it->f[d].node.nitems)
			break;
	}
	if (d < 0)
	{
		it->at_end = true;
		return false;
	}
	it->f[d].idx++;
	for (; d < it->target; d++)
	{
		iter_load_child(it, d);
		it->f[d + 1].idx = 0;
	}
	return true;
}

/* Move to the previous target-level node (its last item); false at start. */
static bool
iter_prev_node(Iter *it)
{
	int			d;

	for (d = it->target - 1; d >= 0; d--)
	{
		if (it->f[d].idx > 0)
			break;
	}
	if (d < 0)
	{
		it->at_end = true;
		return false;
	}
	it->f[d].idx--;
	for (; d < it->target; d++)
	{
		iter_load_child(it, d);
		it->f[d + 1].idx = it->f[d + 1].node.nitems - 1;
	}
	return true;
}

static Frame *
iter_frame(Iter *it)
{
	return &it->f[it->target];
}

/* The current target-level item, as an Item (count filled in). */
static void
iter_item(Iter *it, Item *item)
{
	Frame	   *fr = iter_frame(it);

	uint8		flags;

	node_item(it->env, &fr->node, fr->idx, &item->key, &item->value, &flags);
	item->flags = flags;
	if (fr->node.level == 0)
		item->count = 1;
	else
	{
		ProllyNodeRef ref;

		node_child(it->env, &fr->node, fr->idx, &ref, &item->count);
	}
}

/* ------------------------------------------------------------------------
 * Get, count, walk
 * ------------------------------------------------------------------------
 */

static bool
cow_get(const ProllyTreeEnv *env, const ProllyNodeRef *root,
		const ProllyKey *key, StringInfo value)
{
	Iter		it;
	Frame	   *fr;
	int			i;
	bool		found = false;

	iter_open(&it, env, root, 0);
	iter_seek_node(&it, key, false);
	fr = iter_frame(&it);
	i = lower_bound(env, &fr->node, key);
	if (i < fr->node.nitems)
	{
		ProllyKey	k;
		ProllyValue v;

		uint8		flags;

		node_item(env, &fr->node, i, &k, &v, &flags);
		if (cmp_keys(env, &k, key) == 0)
		{
			if (!leaf_value_into(env, &v, flags, value))
				appendBinaryStringInfo(value, v.data, v.len);
			found = true;
		}
	}
	iter_close(&it);
	return found;
}

static uint64
cow_count(const ProllyTreeEnv *env, const ProllyNodeRef *root)
{
	TNode		node;
	uint64		total = 0;

	node_read(env, root, &node);
	if (node.level == 0)
		total = node.nitems;
	else
	{
		for (int i = 0; i < node.nitems; i++)
		{
			ProllyNodeRef ref;
			uint64		count;

			node_child(env, &node, i, &ref, &count);
			total += count;
		}
	}
	node_release(&node);
	return total;
}

/*
 * The keys below key: a descent of key's path, adding the counts of the
 * children before the one that may hold it (an internal item's key is its
 * child's greatest), then the leaf's keys before it.
 */
static uint64
cow_rank(const ProllyTreeEnv *env, const ProllyNodeRef *root,
		 const ProllyKey *key)
{
	ProllyNodeRef ref = *root;
	uint64		below = 0;
	int			level = LEVEL_ANY;

	for (;;)
	{
		TNode		node;
		int			idx;
		uint64		count;

		node_read_level(env, &ref, &node, CurrentMemoryContext, false, level);
		level = (int) node.level - 1;
		idx = lower_bound(env, &node, key);
		if (node.level == 0)
		{
			below += idx;
			node_release(&node);
			return below;
		}
		for (int i = 0; i < idx && i < node.nitems; i++)
		{
			ProllyNodeRef child;

			node_child(env, &node, i, &child, &count);
			below += count;
		}
		if (idx >= node.nitems)
		{
			node_release(&node);
			return below;
		}
		node_child(env, &node, idx, &ref, &count);
		node_release(&node);
	}
}

/* Append h to *hashes, which holds *n of room for *max. */
static void
add_hash(ProllyHash **hashes, int *n, int *max, const ProllyHash *h)
{
	if (*n == *max)
	{
		*max *= 2;
		*hashes = repalloc(*hashes, sizeof(ProllyHash) * *max);
	}
	(*hashes)[(*n)++] = *h;
}

/*
 * Tell the store which chunks the walk reads next: an internal node's
 * children, or a leaf's value trees (a source a store reads through can
 * fetch them at once).
 */
static void
prefetch_below(const ProllyTreeEnv *env, TNode *node)
{
	ProllyHash *hashes;
	int			n = 0;
	int			max;

	/* (only when a hint would hear them) */
	if (env->chunk_store == NULL || !PROLLY_HAS(env->chunk_store, prefetch) ||
		node->nitems == 0 ||
		!PROLLY_CALL(env->chunk_store, prefetch, env->chunks, NULL, 0))
		return;
	max = node->nitems;
	hashes = palloc(sizeof(ProllyHash) * max);
	for (int i = 0; i < node->nitems; i++)
	{
		ProllyNodeRef ref;
		ProllyKey	k;
		ProllyValue v;
		ProllyValue refs;
		uint8		flags;
		uint64		len;

		if (node->level > 0)
		{
			node_child(env, node, i, &ref, &len);
			add_hash(&hashes, &n, &max, &ref.hash);
			continue;
		}
		node_item(env, node, i, &k, &v, &flags);
		if (!(flags & (PROLLY_ITEM_VALUE_REF | PROLLY_ITEM_VALUE_REFS)))
			continue;
		item_payload(&v, flags, &refs, NULL);
		if (flags & PROLLY_ITEM_VALUE_REF)
		{
			decode_value_ref(&v, &ref, &len);
			add_hash(&hashes, &n, &max, &ref.hash);
		}
		for (const char *p = (const char *) refs.data, *end = p + refs.len; p < end;)
		{
			next_value_ref(&p, end, &ref, &len);
			add_hash(&hashes, &n, &max, &ref.hash);
		}
	}
	(void) PROLLY_CALL(env->chunk_store, prefetch, env->chunks, hashes, n);
	pfree(hashes);
}

static bool walk_node(const ProllyTreeEnv *env, const ProllyNodeRef *ref, int level,
					  int nest, ProllyTreeVisitNode visit, void *arg);

/*
 * How deep value trees nest: a row's leaf names value trees (nest 1): a
 * long value's, or a long jsonb document's JSON tree, whose leaves name
 * the value trees of its long strings (nest 2), whose leaves hold bytes
 * and name nothing.  A walk refuses a deeper one, which only a chunk made
 * elsewhere could be: each nesting is a recursion, and a chain of them
 * would overflow the stack.
 */
#define MAX_VALUE_NESTING	2

/*
 * A leaf item's value trees walked: its own, if it is stored out of line,
 * and those of the references it holds.  nest: how deep the leaf's tree
 * is in value trees (0 for a tree the walk began at).
 */
static void
walk_value(const ProllyTreeEnv *env, const ProllyValue *value, uint8 flags,
		   int nest, ProllyTreeVisitNode visit, void *arg)
{
	ProllyValue v = *value;
	ProllyValue refs;
	ProllyTreeEnv venv;
	ProllyNodeRef vroot;
	uint64		len;

	if (!(flags & (PROLLY_ITEM_VALUE_REF | PROLLY_ITEM_VALUE_REFS)))
		return;
	if (nest >= MAX_VALUE_NESTING)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly value tree naming value trees more than %d deep",
						MAX_VALUE_NESTING)));
	item_payload(&v, flags, &refs, NULL);
	value_env(env, &venv);
	if (flags & PROLLY_ITEM_VALUE_REF)
	{
		decode_value_ref(&v, &vroot, &len);
		(void) walk_node(&venv, &vroot, LEVEL_ANY, nest + 1, visit, arg);
	}
	for (const char *p = (const char *) refs.data, *end = p + refs.len; p < end;)
	{
		next_value_ref(&p, end, &vroot, &len);
		(void) walk_node(&venv, &vroot, LEVEL_ANY, nest + 1, visit, arg);
	}
}

/* Whether the visitor took the node. */
static bool
walk_node(const ProllyTreeEnv *env, const ProllyNodeRef *ref, int level,
		  int nest, ProllyTreeVisitNode visit, void *arg)
{
	TNode		node;
	bool		took;

	CHECK_FOR_INTERRUPTS();
	check_stack_depth();
	node_read_level(env, ref, &node, CurrentMemoryContext, false, level);
	took = visit(&node.ref, node.level, arg);
	if (took)
		prefetch_below(env, &node);
	if (!took)
		;						/* the visitor has seen what is below */
	else if (node.level == 0)
	{
		/* Value trees of values stored out of line, and of references. */
		for (int i = 0; i < node.nitems; i++)
		{
			ProllyKey	k;
			ProllyValue v;
			uint8		flags;

			node_item(env, &node, i, &k, &v, &flags);
			walk_value(env, &v, flags, nest, visit, arg);
		}
	}
	else
	{
		for (int i = 0; i < node.nitems; i++)
		{
			ProllyNodeRef child;
			uint64		count;

			node_child(env, &node, i, &child, &count);
			(void) walk_node(env, &child, node.level - 1, nest, visit, arg);
		}
	}
	node_release(&node);
	return took;
}

static bool
cow_walk(const ProllyTreeEnv *env, const ProllyNodeRef *root,
		 ProllyTreeVisitNode visit, void *arg)
{
	return walk_node(env, root, LEVEL_ANY, 0, visit, arg);
}

/* ------------------------------------------------------------------------
 * Cursors
 * ------------------------------------------------------------------------
 */

struct ProllyTreeCursor
{
	Iter		it;
	ScanDirection direction;
	bool		positioned;		/* seek done */
	bool		advance;		/* step before returning the next item */
	StringInfoData value;		/* the last value stored out of line */
};

static ProllyTreeCursor *
cow_cursor_open(const ProllyTreeEnv *env, const ProllyNodeRef *root,
				MemoryContext mcxt)
{
	ProllyTreeCursor *c = MemoryContextAllocZero(mcxt, sizeof(ProllyTreeCursor));
	MemoryContext old = MemoryContextSwitchTo(mcxt);

	initStringInfo(&c->value);
	/* (the nodes the cursor keeps are read in its context) */
	iter_open(&c->it, env, root, 0);
	MemoryContextSwitchTo(old);
	return c;
}

/* Fix up a target-frame idx that ran off its node. */
static void
cursor_settle(ProllyTreeCursor *c)
{
	Iter	   *it = &c->it;

	while (!it->at_end)
	{
		Frame	   *fr = iter_frame(it);

		if (fr->idx >= fr->node.nitems)
		{
			if (!iter_next_node(it))
				break;
		}
		else if (fr->idx < 0)
		{
			if (!iter_prev_node(it))
				break;
		}
		else
			break;
	}
}

static void
cow_cursor_seek(ProllyTreeCursor *c, const ProllyKey *key, ScanDirection direction)
{
	Iter	   *it = &c->it;
	Frame	   *fr;

	c->direction = direction;
	c->positioned = true;
	c->advance = false;

	if (ScanDirectionIsBackward(direction))
	{
		if (key == NULL)
		{
			iter_seek_node(it, NULL, true);
			return;
		}
		iter_seek_node(it, key, false);
		fr = iter_frame(it);
		fr->idx = upper_bound(it->env, &fr->node, key) - 1;
	}
	else
	{
		iter_seek_node(it, key, false);
		fr = iter_frame(it);
		if (key != NULL)
			fr->idx = lower_bound(it->env, &fr->node, key);
	}
	cursor_settle(c);
}

static bool
cow_cursor_next(ProllyTreeCursor *c, ProllyKey *key, ProllyValue *value)
{
	Iter	   *it = &c->it;
	Frame	   *fr;
	uint8		flags;

	if (!c->positioned)
		cow_cursor_seek(c, NULL, ForwardScanDirection);
	if (c->advance)
	{
		fr = iter_frame(it);
		fr->idx += ScanDirectionIsBackward(c->direction) ? -1 : 1;
		cursor_settle(c);
		c->advance = false;
	}
	if (it->at_end)
		return false;
	fr = iter_frame(it);
	node_item(it->env, &fr->node, fr->idx, key, value, &flags);
	leaf_value(it->env, value, flags, &c->value);
	c->advance = true;
	return true;
}

static void
cow_cursor_close(ProllyTreeCursor *c)
{
	iter_close(&c->it);
	pfree(c->value.data);
	pfree(c);
}

/* ------------------------------------------------------------------------
 * Apply
 * ------------------------------------------------------------------------
 */

/* An edit at some level: a put (with count at internal levels). */
typedef struct LevelEdit
{
	Item		item;
	bool		is_delete;
} LevelEdit;

typedef struct EditList
{
	LevelEdit  *edits;
	int			n;
	int			max;
} EditList;

static void
editlist_add(EditList *l, const LevelEdit *e)
{
	if (l->n == l->max)
	{
		l->max = Max(16, l->max * 2);
		l->edits = l->edits ? repalloc(l->edits, sizeof(LevelEdit) * l->max) :
			palloc(sizeof(LevelEdit) * l->max);
	}
	l->edits[l->n++] = *e;
}

static ProllyKey
copy_key(const ProllyKey *k)
{
	ProllyKey	c;
	uint8	   *buf = palloc(Max(k->len, 1));

	memcpy(buf, k->data, k->len);
	c.data = buf;
	c.len = k->len;
	return c;
}

/* A node a window made, or one it replaced. */
typedef struct WindowNode
{
	ProllyKey	maxkey;
	ProllyNodeRef ref;
	uint64		count;
} WindowNode;

typedef struct Window
{
	WindowNode *made;
	int			nmade;
	int			maxmade;
} Window;

static void
window_emit(void *arg, uint16 level, const ProllyKey *maxkey,
			const ProllyNodeRef *ref, uint64 count)
{
	Window	   *w = arg;

	if (w->nmade == w->maxmade)
	{
		w->maxmade = Max(8, w->maxmade * 2);
		w->made = w->made ? repalloc(w->made, sizeof(WindowNode) * w->maxmade) :
			palloc(sizeof(WindowNode) * w->maxmade);
	}
	w->made[w->nmade].maxkey = copy_key(maxkey);
	w->made[w->nmade].ref = *ref;
	w->made[w->nmade].count = count;
	w->nmade++;
}

static int
leveledit_cmp(const void *a, const void *b, void *arg)
{
	const LevelEdit *ea = a;
	const LevelEdit *eb = b;
	int			r = cmp_keys((const ProllyTreeEnv *) arg, &ea->item.key, &eb->item.key);

	if (r != 0)
		return r;
	/* For the same key, the delete sorts before the put. */
	return (int) eb->is_delete - (int) ea->is_delete;
}

static void
parent_put(EditList *out, const WindowNode *n)
{
	LevelEdit	e;
	StringInfoData child;

	initStringInfo(&child);
	encode_child(&child, &n->ref.hash, n->count);
	e.item.key = n->maxkey;
	e.item.value.data = (const uint8 *) child.data;
	e.item.value.len = child.len;
	e.item.count = n->count;
	e.item.flags = 0;
	e.is_delete = false;
	editlist_add(out, &e);
}

/*
 * Apply edits (sorted, at level) to the old tree's nodes at that level, by
 * windows.  Returns the edits for the level above: deletes of replaced
 * nodes' keys and puts of the nodes made, minus nodes that came out the
 * same.
 */
static void
apply_level(const ProllyTreeEnv *env, const ProllyNodeRef *root, uint16 level,
			const EditList *edits, EditList *parent)
{
	Iter		it;
	int			i = 0;

	iter_open(&it, env, root, level);
	while (i < edits->n)
	{
		LevelBuf	lb;
		Window		w = {0};
		WindowNode *replaced = NULL;
		int			nreplaced = 0;
		int			maxreplaced = 0;
		EditList	out = {0};
		int			window_start = i;

		iter_seek_node(&it, &edits->edits[i].item.key, false);
		levelbuf_init(&lb, env, level, window_emit, &w);

		for (;;)
		{
			Frame	   *fr = iter_frame(&it);
			Item		old;
			int			r;

			CHECK_FOR_INTERRUPTS();
			if (fr->idx == 0)
			{
				/* Starting an old node: it will be replaced. */
				ProllyKey	k;
				ProllyValue v;

				if (nreplaced == maxreplaced)
				{
					maxreplaced = Max(8, maxreplaced * 2);
					replaced = replaced ? repalloc(replaced, sizeof(WindowNode) * maxreplaced) :
						palloc(sizeof(WindowNode) * maxreplaced);
				}
				node_item(env, &fr->node, fr->node.nitems - 1, &k, &v, NULL);
				replaced[nreplaced].maxkey = copy_key(&k);
				replaced[nreplaced].ref = fr->node.ref;
				nreplaced++;
			}

			iter_item(&it, &old);
			while (i < edits->n &&
				   (r = cmp_keys(env, &edits->edits[i].item.key, &old.key)) < 0)
			{
				if (!edits->edits[i].is_delete)
					levelbuf_add(&lb, &edits->edits[i].item);
				i++;
			}
			if (i < edits->n &&
				cmp_keys(env, &edits->edits[i].item.key, &old.key) == 0)
			{
				if (!edits->edits[i].is_delete)
					levelbuf_add(&lb, &edits->edits[i].item);
				i++;
			}
			else
				levelbuf_add(&lb, &old);

			fr->idx++;
			if (fr->idx < fr->node.nitems)
				continue;

			/*
			 * Finished an old node.  If it was the level's last, the remaining
			 * edits come after every old key and belong to this window, so
			 * this must be checked before resynchronization: a clean chunker
			 * here would otherwise leave them unconsumed, and the next window
			 * would re-chunk this same node forever.  Otherwise a clean
			 * chunker means the window has resynchronized, and the next node
			 * is read only if the window goes on into it.
			 */
			if (!iter_has_next_node(&it))
			{
				for (; i < edits->n; i++)
				{
					if (!edits->edits[i].is_delete)
						levelbuf_add(&lb, &edits->edits[i].item);
				}
				levelbuf_flush(&lb);
				break;
			}
			if (lb.nitems == 0)
				break;			/* resynchronized: the rest is unchanged */
			(void) iter_next_node(&it);
		}
		levelbuf_free(&lb);

		/* Every window consumes an edit; one that doesn't would loop. */
		if (i == window_start)
			elog(ERROR, "prolly tree apply made no progress at level %u", level);

		/* Edits for the level above, dropping nodes that came out the same. */
		for (int r2 = 0; r2 < nreplaced; r2++)
		{
			bool		same = false;

			for (int m = 0; m < w.nmade; m++)
			{
				if (w.made[m].ref.hash.len == replaced[r2].ref.hash.len &&
					memcmp(w.made[m].ref.hash.bytes, replaced[r2].ref.hash.bytes,
						   replaced[r2].ref.hash.len) == 0 &&
					cmp_keys(env, &w.made[m].maxkey, &replaced[r2].maxkey) == 0)
				{
					w.made[m].count = PG_UINT64_MAX;	/* mark: unchanged */
					same = true;
					break;
				}
			}
			if (!same)
			{
				LevelEdit	e = {0};

				e.item.key = replaced[r2].maxkey;
				e.is_delete = true;
				editlist_add(&out, &e);
			}
		}
		for (int m = 0; m < w.nmade; m++)
		{
			if (w.made[m].count != PG_UINT64_MAX)
				parent_put(&out, &w.made[m]);
		}
		qsort_arg(out.edits, out.n, sizeof(LevelEdit), leveledit_cmp,
				  unconstify(ProllyTreeEnv *, env));
		for (int k = 0; k < out.n; k++)
		{
			/* A delete followed by a put of the same key is just the put. */
			if (out.edits[k].is_delete && k + 1 < out.n &&
				cmp_keys(env, &out.edits[k].item.key, &out.edits[k + 1].item.key) == 0)
				continue;
			editlist_add(parent, &out.edits[k]);
		}
	}
	iter_close(&it);
}

/* Rebuild the root's level from its items and edits, growing if needed. */
static void
apply_root(const ProllyTreeEnv *env, const ProllyNodeRef *root,
		   const EditList *edits, ProllyNodeRef *new_root)
{
	Iter		it;
	ProllyTreeBuilder *b;
	uint16		level;
	int			i = 0;

	iter_open(&it, env, root, 0);
	level = iter_root_level(&it);
	iter_close(&it);
	iter_open(&it, env, root, level);
	iter_seek_node(&it, NULL, false);

	b = builder_create(env, level, CurrentMemoryContext);
	if (!it.at_end)
	{
		Frame	   *fr = iter_frame(&it);

		for (fr->idx = 0; fr->idx < fr->node.nitems; fr->idx++)
		{
			Item		old;

			iter_item(&it, &old);
			while (i < edits->n && cmp_keys(env, &edits->edits[i].item.key, &old.key) < 0)
			{
				if (!edits->edits[i].is_delete)
					builder_add_at(b, level, &edits->edits[i].item);
				i++;
			}
			if (i < edits->n && cmp_keys(env, &edits->edits[i].item.key, &old.key) == 0)
			{
				if (!edits->edits[i].is_delete)
					builder_add_at(b, level, &edits->edits[i].item);
				i++;
			}
			else
				builder_add_at(b, level, &old);
		}
	}
	for (; i < edits->n; i++)
	{
		if (!edits->edits[i].is_delete)
			builder_add_at(b, level, &edits->edits[i].item);
	}
	iter_close(&it);
	builder_finish(b, new_root);
}

static void
apply_edits(const ProllyTreeEnv *env, const ProllyNodeRef *root,
			const ProllyEdit *edits, int nedits, ProllyNodeRef *new_root)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext,
											  "prolly apply", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	EditList	cur = {0};
	TNode		rootnode;
	uint16		rootlevel;
	ProllyNodeRef result = *root;
	bool		changed = true;

	for (int i = 0; i < nedits; i++)
	{
		LevelEdit	e;

		if (i > 0 && cmp_keys(env, &edits[i - 1].key, &edits[i].key) >= 0)
			elog(ERROR, "prolly tree edits are not in strictly increasing key order");
		if (edits[i].is_delete)
		{
			memset(&e.item, 0, sizeof(e.item));
			e.item.key = edits[i].key;
		}
		else
		{
			StringInfoData stored;

			/* In the apply's memory, which lasts until it ends. */
			initStringInfo(&stored);
			leaf_item(env, &edits[i].key, &edits[i].value, &e.item, &stored);
		}
		e.is_delete = edits[i].is_delete;
		editlist_add(&cur, &e);
	}

	node_read_keep(env, root, &rootnode, CurrentMemoryContext, true);
	rootlevel = rootnode.level;
	node_release(&rootnode);

	if (nedits == 0)
		changed = false;
	for (uint16 level = 0; changed && level < rootlevel; level++)
	{
		EditList	next = {0};

		apply_level(env, root, level, &cur, &next);
		cur = next;
		if (cur.n == 0)
			changed = false;
	}
	if (changed)
	{
		keep_written_leaves = rootlevel == 0;
		last_leaf_hash.len = 0;
		apply_root(env, root, &cur, &result);
		keep_written_leaves = false;
		if (last_leaf_hash.len > 0 &&
			last_leaf_hash.len == result.hash.len &&
			memcmp(last_leaf_hash.bytes, result.hash.bytes,
				   result.hash.len) == 0)
			kept_put(&result.hash, last_leaf.data, last_leaf.len);

		/* A root with one child isn't a root: its child is. */
		for (int want = LEVEL_ANY;;)
		{
			TNode		n;
			ProllyNodeRef child;
			uint64		count;

			node_read_level(env, &result, &n, CurrentMemoryContext, false, want);
			want = (int) n.level - 1;
			if (n.level == 0 || n.nitems != 1)
			{
				node_release(&n);
				break;
			}
			node_child(env, &n, 0, &child, &count);
			node_release(&n);
			result = child;
		}
	}

	MemoryContextSwitchTo(old);
	*new_root = result;
	MemoryContextDelete(cxt);
}

/*
 * An apply reads each node it keeps once (see "Kept nodes"): in the open
 * scope, if there is one, else in its own.
 */
static void
cow_apply(const ProllyTreeEnv *env, const ProllyNodeRef *root,
		  const ProllyEdit *edits, int nedits, ProllyNodeRef *new_root)
{
	MemoryContext cxt = NULL;

	if (keeping == NULL)
	{
		cxt = AllocSetContextCreate(CurrentMemoryContext, "prolly apply nodes",
									ALLOCSET_DEFAULT_SIZES);
		keep_scope_open(cxt);
	}
	PG_TRY();
	{
		apply_edits(env, root, edits, nedits, new_root);
	}
	PG_FINALLY();
	{
		keep_written_leaves = false;
		if (cxt)
			MemoryContextDelete(cxt);
	}
	PG_END_TRY();
}

/*
 * Until cxt is reset or deleted, every node read or written above the
 * leaves, and every root read, is read from the chunk store once.
 */
static void
cow_keep_nodes(MemoryContext cxt)
{
	keep_scope_open(cxt);
}

/* ------------------------------------------------------------------------
 * Diff
 * ------------------------------------------------------------------------
 */

static bool
same_hash(const ProllyHash *a, const ProllyHash *b)
{
	return a->len == b->len && memcmp(a->bytes, b->bytes, a->len) == 0;
}

/* A diff cursor: frames 0 .. depth are read, and it stands on f[depth]. */
typedef struct DiffSide
{
	const ProllyTreeEnv *env;
	MemoryContext cxt;			/* where the nodes it keeps are */
	Frame		f[MAX_LEVELS];
	int			depth;			/* -1 at the end */
} DiffSide;

static void
side_open(DiffSide *s, const ProllyTreeEnv *env, const ProllyNodeRef *root)
{
	s->env = env;
	s->cxt = CurrentMemoryContext;
	s->depth = 0;
	node_read_keep(env, root, &s->f[0].node, s->cxt, true);
	s->f[0].idx = 0;
	if (s->f[0].node.nitems == 0)
	{
		node_release(&s->f[0].node);
		s->depth = -1;			/* the empty tree */
	}
}

static void
side_close(DiffSide *s)
{
	for (int d = 0; d <= s->depth; d++)
		node_release(&s->f[d].node);
}

/* The level of the node it stands in: 0 for a leaf item. */
static uint16
side_height(const DiffSide *s)
{
	return s->f[s->depth].node.level;
}

/* The child of the internal item it stands on. */
static void
side_child(const DiffSide *s, ProllyNodeRef *ref)
{
	uint64		count;

	node_child(s->env, &s->f[s->depth].node, s->f[s->depth].idx, ref, &count);
}

/* Stand on the first item of the child of the item it stands on. */
static void
side_descend(DiffSide *s)
{
	ProllyNodeRef ref;
	Frame	   *child;

	if (s->depth + 1 >= MAX_LEVELS)
		elog(ERROR, "prolly tree is more than %d levels high", MAX_LEVELS);
	side_child(s, &ref);
	child = &s->f[s->depth + 1];
	node_read_child(s->env, &ref, &child->node, s->cxt,
					s->f[s->depth].node.level);
	if (child->node.nitems == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly tree has an empty node below its root")));
	child->idx = 0;
	s->depth++;
}

/* Move past the item (or subtree) it stands on, up past finished nodes. */
static void
side_advance(DiffSide *s)
{
	s->f[s->depth].idx++;
	while (s->f[s->depth].idx >= s->f[s->depth].node.nitems)
	{
		node_release(&s->f[s->depth].node);
		if (--s->depth < 0)
			return;
		s->f[s->depth].idx++;
	}
}

static void
side_leaf_item(const DiffSide *s, ProllyKey *key, ProllyValue *value,
			   uint8 *flags)
{
	node_item(s->env, &s->f[s->depth].node, s->f[s->depth].idx, key, value,
			  flags);
}

/* A leaf value as a diff reports it: its bytes, read if out of line. */
static const ProllyValue *
reported(const ProllyTreeEnv *env, ProllyValue *value, uint8 flags,
		 StringInfo buf)
{
	leaf_value(env, value, flags, buf);
	return value;
}

static void
cow_diff(const ProllyTreeEnv *env, const ProllyNodeRef *from,
		 const ProllyNodeRef *to, ProllyTreeVisitChange visit, void *arg)
{
	DiffSide	a;
	DiffSide	b;
	StringInfoData bufa;
	StringInfoData bufb;
	uint8		fa;
	uint8		fb;

	if (same_hash(&from->hash, &to->hash))
		return;
	initStringInfo(&bufa);
	initStringInfo(&bufb);

	side_open(&a, env, from);
	side_open(&b, env, to);

	for (;;)
	{
		ProllyKey	ka,
					kb;
		ProllyValue va,
					vb;
		int			r;

		CHECK_FOR_INTERRUPTS();
		if (a.depth < 0 && b.depth < 0)
			break;

		/* What is left of one tree: its leaf items, reported one by one. */
		if (a.depth < 0 || b.depth < 0)
		{
			DiffSide   *rest = a.depth < 0 ? &b : &a;

			if (side_height(rest) > 0)
				side_descend(rest);
			else if (rest == &b)
			{
				side_leaf_item(&b, &kb, &vb, &fb);
				if (!visit(PROLLY_DIFF_ADDED, &kb, NULL,
						   reported(env, &vb, fb, &bufb), arg))
					break;
				side_advance(&b);
			}
			else
			{
				side_leaf_item(&a, &ka, &va, &fa);
				if (!visit(PROLLY_DIFF_REMOVED, &ka,
						   reported(env, &va, fa, &bufa), NULL, arg))
					break;
				side_advance(&a);
			}
			continue;
		}

		/*
		 * Subtrees are compared only at equal heights.  Equal hashes mean
		 * equal items, which start right after what each side has already
		 * passed, so both skip them.
		 */
		if (side_height(&a) > side_height(&b))
		{
			side_descend(&a);
			continue;
		}
		if (side_height(&b) > side_height(&a))
		{
			side_descend(&b);
			continue;
		}
		if (side_height(&a) > 0)
		{
			ProllyNodeRef ca;
			ProllyNodeRef cb;

			side_child(&a, &ca);
			side_child(&b, &cb);
			if (same_hash(&ca.hash, &cb.hash))
			{
				side_advance(&a);
				side_advance(&b);
			}
			else
			{
				side_descend(&a);
				side_descend(&b);
			}
			continue;
		}

		/* Both on leaf items: merge by key. */
		side_leaf_item(&a, &ka, &va, &fa);
		side_leaf_item(&b, &kb, &vb, &fb);
		r = cmp_keys(env, &ka, &kb);
		if (r < 0)
		{
			if (!visit(PROLLY_DIFF_REMOVED, &ka, reported(env, &va, fa, &bufa),
					   NULL, arg))
				break;
			side_advance(&a);
		}
		else if (r > 0)
		{
			if (!visit(PROLLY_DIFF_ADDED, &kb, NULL,
					   reported(env, &vb, fb, &bufb), arg))
				break;
			side_advance(&b);
		}
		else
		{
			/*
			 * Equal keys can be written differently; that is a change too.
			 * Values out of line are equal exactly when their references are.
			 */
			if ((fa != fb || va.len != vb.len ||
				 memcmp(va.data, vb.data, va.len) != 0 ||
				 ka.len != kb.len || memcmp(ka.data, kb.data, ka.len) != 0) &&
				!visit(PROLLY_DIFF_MODIFIED, &kb, reported(env, &va, fa, &bufa),
					   reported(env, &vb, fb, &bufb), arg))
				break;
			side_advance(&a);
			side_advance(&b);
		}
	}
	side_close(&a);
	side_close(&b);
	pfree(bufa.data);
	pfree(bufb.data);
}

/* A leaf item a diff without an order found on one side, its bytes copied. */
typedef struct LooseItem
{
	ProllyKey	key;
	ProllyValue value;
	uint8		flags;
} LooseItem;

static int
loose_item_cmp(const void *a, const void *b)
{
	const LooseItem *x = a;
	const LooseItem *y = b;
	int			r = memcmp(x->key.data, y->key.data, Min(x->key.len, y->key.len));

	return r != 0 ? r : (x->key.len > y->key.len) - (x->key.len < y->key.len);
}

/* The nodes of a side not yet matched or read, and its leaf items. */
typedef struct LooseSide
{
	ProllyNodeRef *nodes;
	uint16	   *levels;
	int			n;
	int			max;
	LooseItem  *items;
	int			nitems;
	int			maxitems;
} LooseSide;

static void
loose_add_node(LooseSide *s, const ProllyNodeRef *ref, uint16 level)
{
	if (s->n == s->max)
	{
		s->max = Max(16, s->max * 2);
		s->nodes = s->nodes ? repalloc_array(s->nodes, ProllyNodeRef, s->max) :
			palloc_array(ProllyNodeRef, s->max);
		s->levels = s->levels ? repalloc_array(s->levels, uint16, s->max) :
			palloc_array(uint16, s->max);
	}
	s->nodes[s->n] = *ref;
	s->levels[s->n++] = level;
}

#define LOOSE_GONE		UINT16_MAX	/* a node both trees hold, dropped */

static int
loose_node_cmp(const ProllyNodeRef *a, const ProllyNodeRef *b)
{
	int			r = memcmp(a->hash.bytes, b->hash.bytes,
						   Min(a->hash.len, b->hash.len));

	return r != 0 ? r : (int) a->hash.len - (int) b->hash.len;
}

typedef struct LoosePair
{
	ProllyNodeRef ref;
	uint16		level;
} LoosePair;

static int
loose_pair_cmp(const void *a, const void *b)
{
	return loose_node_cmp(&((const LoosePair *) a)->ref,
						  &((const LoosePair *) b)->ref);
}

/* s's nodes in order of their hashes, their levels with them. */
static void
loose_sort(LooseSide *s)
{
	LoosePair  *p;

	if (s->n < 2)
		return;
	p = palloc_array(LoosePair, s->n);
	for (int i = 0; i < s->n; i++)
	{
		p[i].ref = s->nodes[i];
		p[i].level = s->levels[i];
	}
	qsort(p, s->n, sizeof(LoosePair), loose_pair_cmp);
	for (int i = 0; i < s->n; i++)
	{
		s->nodes[i] = p[i].ref;
		s->levels[i] = p[i].level;
	}
	pfree(p);
}

/* Read node i of s: its children become s's nodes, or its items its items. */
static void
loose_expand(const ProllyTreeEnv *env, LooseSide *s, int i)
{
	TNode		node;
	ProllyNodeRef ref = s->nodes[i];
	uint16		level = s->levels[i];

	s->nodes[i] = s->nodes[--s->n];
	s->levels[i] = s->levels[s->n];
	node_read_level(env, &ref, &node, CurrentMemoryContext, false, level);
	for (int j = 0; j < node.nitems; j++)
	{
		if (node.level > 0)
		{
			ProllyNodeRef child;
			uint64		count;

			node_child(env, &node, j, &child, &count);
			loose_add_node(s, &child, node.level - 1);
		}
		else
		{
			LooseItem  *it;
			ProllyKey	k;
			ProllyValue v;
			uint8		flags;

			if (s->nitems == s->maxitems)
			{
				s->maxitems = Max(64, s->maxitems * 2);
				s->items = s->items ?
					repalloc_huge(s->items, sizeof(LooseItem) * s->maxitems) :
					MemoryContextAllocHuge(CurrentMemoryContext,
										   sizeof(LooseItem) * s->maxitems);
			}
			node_item(env, &node, j, &k, &v, &flags);
			it = &s->items[s->nitems++];
			it->key.data = (const uint8 *) memcpy(palloc(Max(k.len, 1)), k.data, k.len);
			it->key.len = k.len;
			it->value.data = (const uint8 *) memcpy(palloc(Max(v.len, 1)), v.data, v.len);
			it->value.len = v.len;
			it->flags = flags;
		}
	}
	node_release(&node);
}

/*
 * diff(), where env->compare can't order the trees' keys (they were built
 * in an order this session can't give): by the nodes' hashes, from the
 * top level down, a node both trees hold is skipped, the rest read; their
 * leaf items matched by their keys' bytes.  As diff, it reads O((d + 1)
 * log n) nodes for d changed keys (a key is in one node of a tree, so one
 * a node both hold has isn't elsewhere in either); its changes come in the
 * order of the keys' bytes.
 */
static void
cow_diff_unordered(const ProllyTreeEnv *env, const ProllyNodeRef *from,
				   const ProllyNodeRef *to, ProllyTreeVisitChange visit,
				   void *arg)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext,
											  "prolly unordered diff",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	LooseSide	sides[2] = {0};
	StringInfoData bufa;
	StringInfoData bufb;
	int			ia = 0;
	int			ib = 0;

	if (same_hash(&from->hash, &to->hash))
	{
		MemoryContextSwitchTo(old);
		MemoryContextDelete(cxt);
		return;
	}
	for (int side = 0; side < 2; side++)
	{
		TNode		root;

		node_read(env, side == 0 ? from : to, &root);
		loose_add_node(&sides[side], side == 0 ? from : to, root.level);
		node_release(&root);
	}

	/* the highest level's nodes, those not both trees', read, till leaves */
	for (;;)
	{
		int			top = -1;

		CHECK_FOR_INTERRUPTS();
		for (int side = 0; side < 2; side++)
			for (int i = 0; i < sides[side].n; i++)
				top = Max(top, sides[side].levels[i]);
		if (top < 0)
			break;

		/* (one both hold at this level: in neither, unread) */
		for (int side = 0; side < 2; side++)
			loose_sort(&sides[side]);
		for (int i = 0, j = 0; i < sides[0].n && j < sides[1].n;)
		{
			int			r = loose_node_cmp(&sides[0].nodes[i], &sides[1].nodes[j]);

			if (r == 0 && sides[0].levels[i] == top && sides[1].levels[j] == top)
			{
				sides[0].levels[i] = sides[1].levels[j] = LOOSE_GONE;
				i++;
				j++;
			}
			else if (r <= 0)
				i++;
			else
				j++;
		}
		for (int side = 0; side < 2; side++)
			for (int i = sides[side].n - 1; i >= 0; i--)
				if (sides[side].levels[i] == LOOSE_GONE)
				{
					sides[side].nodes[i] = sides[side].nodes[--sides[side].n];
					sides[side].levels[i] = sides[side].levels[sides[side].n];
				}
				else if (sides[side].levels[i] == top)
					loose_expand(env, &sides[side], i);
	}

	for (int side = 0; side < 2; side++)
		if (sides[side].nitems > 1)
			qsort(sides[side].items, sides[side].nitems, sizeof(LooseItem),
				  loose_item_cmp);
	initStringInfo(&bufa);
	initStringInfo(&bufb);
	while (ia < sides[0].nitems || ib < sides[1].nitems)
	{
		LooseItem  *a = ia < sides[0].nitems ? &sides[0].items[ia] : NULL;
		LooseItem  *b = ib < sides[1].nitems ? &sides[1].items[ib] : NULL;
		int			r = a == NULL ? 1 : b == NULL ? -1 : loose_item_cmp(a, b);
		ProllyValue va;
		ProllyValue vb;

		CHECK_FOR_INTERRUPTS();
		if (a)
			va = a->value;
		if (b)
			vb = b->value;
		if (r < 0)
		{
			if (!visit(PROLLY_DIFF_REMOVED, &a->key,
					   reported(env, &va, a->flags, &bufa), NULL, arg))
				break;
			ia++;
		}
		else if (r > 0)
		{
			if (!visit(PROLLY_DIFF_ADDED, &b->key, NULL,
					   reported(env, &vb, b->flags, &bufb), arg))
				break;
			ib++;
		}
		else
		{
			/* (values out of line are equal exactly when their references are) */
			if ((a->flags != b->flags || va.len != vb.len ||
				 memcmp(va.data, vb.data, va.len) != 0) &&
				!visit(PROLLY_DIFF_MODIFIED, &b->key,
					   reported(env, &va, a->flags, &bufa),
					   reported(env, &vb, b->flags, &bufb), arg))
				break;
			ia++;
			ib++;
		}
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
}

/*
 * A bound of a node's keys in diff_estimate(): an internal item's key is
 * the largest of its child's, so a child holds the keys above the one
 * before it (its parent's lower bound, for the first) up to its own.
 * Unbounded: no key below (a lower bound), or none above (an upper one).
 */
typedef struct EstimateBound
{
	ProllyKey	key;
	bool		unbounded;
} EstimateBound;

/*
 * A side of diff_estimate(): the nodes not yet matched or read, each with
 * its level, item count and the bounds of its keys, and the hashes of its
 * leaf items read (the key's, then the item's whole).
 */
typedef struct EstimateItem
{
	uint64		key;
	uint64		whole;
} EstimateItem;

typedef struct EstimateNode
{
	ProllyNodeRef ref;
	uint16		level;
	uint64		count;
	EstimateBound lo;			/* the keys above lo */
	EstimateBound hi;			/* up to hi */
} EstimateNode;

typedef struct EstimateSide
{
	EstimateNode *nodes;
	int			n;
	int			max;
	EstimateItem *items;
	int			nitems;
	int			maxitems;
} EstimateSide;

static void
estimate_add_node(EstimateSide *s, const EstimateNode *node)
{
	if (s->n == s->max)
	{
		s->max = Max(16, s->max * 2);
		s->nodes = s->nodes ? repalloc_array(s->nodes, EstimateNode, s->max) :
			palloc_array(EstimateNode, s->max);
	}
	s->nodes[s->n++] = *node;
}

static void
estimate_drop_node(EstimateSide *s, int i)
{
	s->nodes[i] = s->nodes[--s->n];
}

static int
estimate_node_cmp(const void *a, const void *b)
{
	return loose_node_cmp(&((const EstimateNode *) a)->ref,
						  &((const EstimateNode *) b)->ref);
}

/* s's nodes in order of their hashes. */
static void
estimate_sort(EstimateSide *s)
{
	if (s->n > 1)
		qsort(s->nodes, s->n, sizeof(EstimateNode), estimate_node_cmp);
}

/* A key's copy, for a bound that outlives the node it was read from. */
static EstimateBound
estimate_bound(const ProllyKey *key)
{
	EstimateBound b;
	uint8	   *data = palloc(Max(key->len, 1));

	memcpy(data, key->data, key->len);
	b.key.data = data;
	b.key.len = key->len;
	b.unbounded = false;
	return b;
}

/* Read node i of s: its children become s's nodes, or its items' hashes its items. */
static void
estimate_expand(const ProllyTreeEnv *env, EstimateSide *s, int i)
{
	TNode		node;
	EstimateNode parent = s->nodes[i];
	EstimateBound lo = parent.lo;

	estimate_drop_node(s, i);
	node_read_level(env, &parent.ref, &node, CurrentMemoryContext, false,
					parent.level);
	for (int j = 0; j < node.nitems; j++)
	{
		ProllyKey	k;
		ProllyValue v;
		uint8		flags;

		node_item(env, &node, j, &k, &v, &flags);
		if (node.level > 0)
		{
			EstimateNode child;

			memset(&child.ref, 0, sizeof(child.ref));
			decode_child(&v, &child.ref.hash, &child.count);
			child.level = node.level - 1;
			child.lo = lo;
			child.hi = j == node.nitems - 1 ? parent.hi : estimate_bound(&k);
			lo = child.hi;
			estimate_add_node(s, &child);
		}
		else
		{
			uint64		key;

			if (s->nitems == s->maxitems)
			{
				s->maxitems = Max(64, s->maxitems * 2);
				s->items = s->items ?
					repalloc_array(s->items, EstimateItem, s->maxitems) :
					palloc_array(EstimateItem, s->maxitems);
			}
			key = hash_bytes_extended(k.data, k.len, 0);
			s->items[s->nitems].key = key;
			s->items[s->nitems++].whole =
				hash_bytes_extended(v.data, v.len, key ^ flags);
		}
	}
	node_release(&node);
}

static int
estimate_item_cmp(const void *a, const void *b)
{
	const EstimateItem *x = a;
	const EstimateItem *y = b;

	if (x->key != y->key)
		return x->key < y->key ? -1 : 1;
	return (x->whole > y->whole) - (x->whole < y->whole);
}

/*
 * Walk sides down (diff_unordered()'s walk: by the nodes' hashes, from the
 * top level down, a node both trees hold dropped unread), a level only if
 * its nodes that differ fit in limit with the *read already: true once
 * every node that differs is read, false where the walk stopped before a
 * level, the nodes left those that differ at it (and below, of a side
 * lower there).
 */
static bool
estimate_walk(const ProllyTreeEnv *env, EstimateSide *sides, uint64 *read,
			  uint64 limit)
{
	for (;;)
	{
		int			top = -1;
		uint64		here = 0;

		CHECK_FOR_INTERRUPTS();
		for (int side = 0; side < 2; side++)
			for (int i = 0; i < sides[side].n; i++)
				top = Max(top, sides[side].nodes[i].level);
		if (top < 0)
			return true;
		for (int side = 0; side < 2; side++)
			estimate_sort(&sides[side]);
		for (int i = 0, j = 0; i < sides[0].n && j < sides[1].n;)
		{
			EstimateNode *a = &sides[0].nodes[i];
			EstimateNode *b = &sides[1].nodes[j];
			int			r = loose_node_cmp(&a->ref, &b->ref);

			if (r == 0 && a->level == top && b->level == top)
			{
				a->level = b->level = LOOSE_GONE;
				i++;
				j++;
			}
			else if (r <= 0)
				i++;
			else
				j++;
		}
		for (int side = 0; side < 2; side++)
			for (int i = sides[side].n - 1; i >= 0; i--)
			{
				if (sides[side].nodes[i].level == LOOSE_GONE)
					estimate_drop_node(&sides[side], i);
				else if (sides[side].nodes[i].level == top)
					here++;
			}
		if (*read + here > limit)
			return false;
		for (int side = 0; side < 2; side++)
			for (int i = sides[side].n - 1; i >= 0; i--)
			{
				if (sides[side].nodes[i].level == top)
				{
					estimate_expand(env, &sides[side], i);
					(*read)++;
				}
			}
	}
}

/* The items sides read: those on one side, and those whose values differ. */
static uint64
estimate_items(EstimateSide *sides)
{
	uint64		changes = 0;

	for (int side = 0; side < 2; side++)
		if (sides[side].nitems > 1)
			qsort(sides[side].items, sides[side].nitems, sizeof(EstimateItem),
				  estimate_item_cmp);
	for (int i = 0, j = 0; i < sides[0].nitems || j < sides[1].nitems;)
	{
		EstimateItem *a = i < sides[0].nitems ? &sides[0].items[i] : NULL;
		EstimateItem *b = j < sides[1].nitems ? &sides[1].items[j] : NULL;

		if (b == NULL || (a != NULL && a->key < b->key))
		{
			changes++;
			i++;
		}
		else if (a == NULL || b->key < a->key)
		{
			changes++;
			j++;
		}
		else
		{
			if (a->whole != b->whole)
				changes++;
			i++;
			j++;
		}
	}
	return changes;
}

/* Bounds compared, each as the lower or the upper one it is. */
static int
estimate_bound_cmp(const ProllyTreeEnv *env, const EstimateBound *a, bool a_hi,
				   const EstimateBound *b, bool b_hi)
{
	if (a->unbounded || b->unbounded)
	{
		int			ra = a->unbounded ? (a_hi ? 1 : -1) : 0;
		int			rb = b->unbounded ? (b_hi ? 1 : -1) : 0;

		return (ra > rb) - (ra < rb);
	}
	return cmp_keys(env, &a->key, &b->key);
}

static int
estimate_hi_cmp(const void *a, const void *b, void *arg)
{
	return estimate_bound_cmp(arg, &((const EstimateNode *) a)->hi, true,
							  &((const EstimateNode *) b)->hi, true);
}

/*
 * A part of the key space the walk left unread: the nodes of both sides
 * whose keys overlap, chained (each side's nodes cover the keys apart, so
 * the nodes that hold a key, on each side, are in one part).  Its changes
 * are those its own nodes' walk finds: a key both sides hold differently,
 * or one holds and the other doesn't, is under a node of each part's that
 * holds it.
 */
typedef struct EstimatePart
{
	int			first;			/* in the parts' nodes */
	int			n;
	uint64		rows;			/* items its nodes hold */
	uint64		order;			/* its first node's hash: a random order */
} EstimatePart;

typedef struct EstimatePartNode
{
	int			side;
	EstimateNode node;
} EstimatePartNode;

static int
estimate_part_cmp(const void *a, const void *b)
{
	uint64		x = ((const EstimatePart *) a)->order;
	uint64		y = ((const EstimatePart *) b)->order;

	return (x > y) - (x < y);
}

/*
 * The parts a sample walks at most: enough to tell how many changes a row
 * holds to within a few tens of percent, the rest of the budget left
 * unread (a move plans every candidate's tables).
 */
#define ESTIMATE_SAMPLE_PARTS	64

/*
 * The changes under the nodes the walk left unread (sides'), from a sample
 * of its parts (ESTIMATE_SAMPLE_PARTS of them, in a random order), walked
 * whole while they fit in max_nodes with *read: the
 * changes they hold, for each row under them, times the rows unread.  A
 * part is walked down from the nodes that differ, so it reads only the
 * nodes that differ under them, as the walk would have: where the sample
 * is every part, it gives the diff's count.  With none walked (the first
 * didn't fit), every row under them counts as changed: above the count.
 */
static uint64
estimate_sampled(const ProllyTreeEnv *env, EstimateSide *sides, uint64 *read,
				 uint64 max_nodes)
{
	EstimatePartNode *seq;
	EstimatePart *parts;
	int			nseq = 0;
	int			nparts = 0;
	uint64		unread = 0;
	uint64		sampled_rows = 0;
	uint64		sampled = 0;
	double		estimate;

	for (int side = 0; side < 2; side++)
	{
		if (sides[side].n > 1)
			qsort_arg(sides[side].nodes, sides[side].n, sizeof(EstimateNode),
					  estimate_hi_cmp, (void *) env);
		for (int i = 0; i < sides[side].n; i++)
			unread += sides[side].nodes[i].count;
	}
	seq = palloc_array(EstimatePartNode, Max(sides[0].n + sides[1].n, 1));
	parts = palloc_array(EstimatePart, Max(sides[0].n + sides[1].n, 1));

	/*
	 * (each side's nodes in key order, taken lowest first; a part ends where
	 * the next node's keys begin at or past the highest it holds)
	 */
	{
		int			i = 0;
		int			j = 0;
		const EstimateBound *top = NULL;

		while (i < sides[0].n || j < sides[1].n)
		{
			int			side;
			EstimateNode *next;

			if (j >= sides[1].n)
				side = 0;
			else if (i >= sides[0].n)
				side = 1;
			else
				side = estimate_bound_cmp(env, &sides[0].nodes[i].lo, false,
										  &sides[1].nodes[j].lo, false) <= 0 ? 0 : 1;
			next = side == 0 ? &sides[0].nodes[i++] : &sides[1].nodes[j++];
			if (nparts == 0 ||
				estimate_bound_cmp(env, &next->lo, false, top, true) >= 0)
			{
				EstimatePart *p = &parts[nparts++];

				p->first = nseq;
				p->n = 0;
				p->rows = 0;
				p->order = 0;
				memcpy(&p->order, next->ref.hash.bytes,
					   Min(sizeof(p->order), next->ref.hash.len));
				top = &next->hi;
			}
			else if (estimate_bound_cmp(env, &next->hi, true, top, true) > 0)
				top = &next->hi;
			seq[nseq].side = side;
			seq[nseq++].node = *next;
			parts[nparts - 1].n++;
			parts[nparts - 1].rows += next->count;
		}
	}
	if (nparts > 1)
		qsort(parts, nparts, sizeof(EstimatePart), estimate_part_cmp);

	for (int p = 0; p < nparts && p < ESTIMATE_SAMPLE_PARTS; p++)
	{
		EstimateSide part[2] = {0};

		for (int k = 0; k < parts[p].n; k++)
			estimate_add_node(&part[seq[parts[p].first + k].side],
							  &seq[parts[p].first + k].node);
		if (!estimate_walk(env, part, read, max_nodes))
			break;
		sampled += estimate_items(part);
		sampled_rows += parts[p].rows;
	}
	if (sampled_rows == 0)
		return unread;
	estimate = ceil((double) sampled * unread / sampled_rows);
	return estimate >= (double) unread ? unread : (uint64) estimate;
}

/*
 * How many keys diff() would report between from and to, estimated
 * reading at most max_nodes nodes.  The trees are walked from the top
 * level down (estimate_walk()), a node both hold dropped unread; where
 * every node that differs is read, their leaf items are matched by their
 * bytes' hashes, which gives diff's count (keys equal by the tree's order
 * but not as bytes, or a hash collision, aside).
 *
 * Where they don't fit in half of max_nodes, the walk stops before the
 * first level whose nodes that differ don't, and the rest is spent on a
 * sample of the parts of the key space it left unread (estimate_sampled()),
 * each walked down whole, to its changes: those the parts walked hold for
 * each row under them, times the rows under every part.  Counting every row
 * under the nodes unread overstates a few changes spread over a large
 * table by orders of magnitude: a thousand keys updated across a million
 * rows leave a thousand of its leaves differing, and the levels above
 * nearly every node.  How many nodes differ can't tell those apart from as
 * many runs of keys rewritten either, a leaf's worth or more each; the
 * changes a part holds for each of its rows can, whatever the levels above
 * show.  Where the sample is every part, the estimate is diff's count;
 * where not even one part fits, every row under the nodes unread counts as
 * changed, above it.
 */
static uint64
cow_diff_estimate(const ProllyTreeEnv *env, const ProllyNodeRef *from,
				  const ProllyNodeRef *to, uint64 max_nodes)
{
	MemoryContext cxt;
	MemoryContext old;
	EstimateSide sides[2] = {0};
	uint64		read = 0;
	uint64		changes;

	if (same_hash(&from->hash, &to->hash))
		return 0;
	cxt = AllocSetContextCreate(CurrentMemoryContext, "prolly diff estimate",
								ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cxt);
	for (int side = 0; side < 2; side++)
	{
		TNode		root;
		EstimateNode n;

		node_read(env, side == 0 ? from : to, &root);
		n.ref = *(side == 0 ? from : to);
		n.level = root.level;
		n.count = 0;
		n.lo.unbounded = n.hi.unbounded = true;
		n.lo.key.data = n.hi.key.data = NULL;
		n.lo.key.len = n.hi.key.len = 0;
		for (int i = 0; i < root.nitems; i++)
		{
			ProllyNodeRef child;
			uint64		c = 1;

			if (root.level > 0)
				node_child(env, &root, i, &child, &c);
			n.count += c;
		}
		estimate_add_node(&sides[side], &n);
		node_release(&root);
		read++;
	}
	if (estimate_walk(env, sides, &read, max_nodes / 2))
		changes = estimate_items(sides);
	else
		changes = estimate_sampled(env, sides, &read, max_nodes);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
	return changes;
}

/*
 * Visit the nodes of to that from may not have, as the diff finds them:
 * subtrees with equal hashes skipped without being read, a subtree only to
 * has walked whole, value trees of values added or changed walked.  So
 * with d changed keys it visits O((d + 1) log n) nodes.  (A node from has
 * elsewhere may be visited; none to has that from lacks is missed.)  The
 * visitor declining a node skips what is below it only where the node is
 * walked whole.
 */
static void
cow_diff_nodes(const ProllyTreeEnv *env, const ProllyNodeRef *from,
			   const ProllyNodeRef *to, ProllyTreeVisitNode visit, void *arg)
{
	DiffSide	a;
	DiffSide	b;

	if (from == NULL || from->hash.len == 0)
	{
		(void) walk_node(env, to, LEVEL_ANY, 0, visit, arg);
		return;
	}
	if (same_hash(&from->hash, &to->hash))
		return;
	side_open(&a, env, from);
	side_open(&b, env, to);
	/* (the empty tree too: a table emptied has it for its root) */
	(void) visit(to, b.depth >= 0 ? side_height(&b) : 0, arg);

	while (b.depth >= 0)
	{
		ProllyNodeRef cb;
		ProllyKey	ka,
					kb;
		ProllyValue va,
					vb;
		uint8		fa,
					fb;

		CHECK_FOR_INTERRUPTS();

		/* What is left of to: all of it. */
		if (a.depth < 0)
		{
			if (side_height(&b) > 0)
			{
				side_child(&b, &cb);
				(void) walk_node(env, &cb, side_height(&b) - 1, 0, visit, arg);
			}
			else
			{
				side_leaf_item(&b, &kb, &vb, &fb);
				walk_value(env, &vb, fb, 0, visit, arg);
			}
			side_advance(&b);
			continue;
		}
		if (side_height(&a) > side_height(&b))
		{
			side_descend(&a);
			continue;
		}
		if (side_height(&b) > side_height(&a))
		{
			side_child(&b, &cb);
			(void) visit(&cb, side_height(&b) - 1, arg);
			side_descend(&b);
			continue;
		}
		if (side_height(&a) > 0)
		{
			ProllyNodeRef ca;

			side_child(&a, &ca);
			side_child(&b, &cb);
			if (same_hash(&ca.hash, &cb.hash))
			{
				side_advance(&a);
				side_advance(&b);
			}
			else
			{
				(void) visit(&cb, side_height(&b) - 1, arg);
				side_descend(&a);
				side_descend(&b);
			}
			continue;
		}

		/* Both on leaf items: a value to added or changed is new. */
		side_leaf_item(&a, &ka, &va, &fa);
		side_leaf_item(&b, &kb, &vb, &fb);
		{
			int			r = cmp_keys(env, &ka, &kb);

			if (r < 0)
				side_advance(&a);
			else
			{
				if (r > 0 || fa != fb || va.len != vb.len ||
					memcmp(va.data, vb.data, va.len) != 0)
					walk_value(env, &vb, fb, 0, visit, arg);
				if (r == 0)
					side_advance(&a);
				side_advance(&b);
			}
		}
	}
	side_close(&a);
	side_close(&b);
}

/*
 * An internal node's children, a leaf's value trees (its values' own, and
 * those of the references they hold): read from the chunk alone, so the
 * chunk store can check what it names before it is anyone's, and keep
 * what it names when it collects.
 */
static void
cow_references(const ProllyChunkFormatRoutine *format, const void *data,
			   Size len, ProllyChunkRefVisit visit, void *arg)
{
	ProllyChunkNode node;

	if (!PROLLY_CALL(format, open, data, len, &node))
		return;
	for (int i = 0; i < node.nitems; i++)
	{
		ProllyKey	k;
		ProllyValue v;
		uint8		flags;
		ProllyHash	ref;
		ProllyNodeRef root;
		ProllyValue refs;
		ProllyValue chunk_refs;
		uint64		n;

		PROLLY_CALL(format, item_get, &node, i, &k, &v, &flags);
		if (node.level > 0)
		{
			decode_child(&v, &ref, &n);
			visit(&ref, arg);
			continue;
		}
		if (!(flags & (PROLLY_ITEM_VALUE_REF | PROLLY_ITEM_VALUE_REFS |
					   PROLLY_ITEM_CHUNK_REFS)))
			continue;
		item_payload(&v, flags, &refs, &chunk_refs);
		for (const char *p = (const char *) chunk_refs.data,
			 *end = p + chunk_refs.len; p < end;)
		{
			next_value_ref(&p, end, &root, &n);
			visit(&root.hash, arg);
		}
		if (flags & PROLLY_ITEM_VALUE_REF)
		{
			decode_value_ref(&v, &root, &n);
			visit(&root.hash, arg);
		}
		for (const char *p = (const char *) refs.data, *end = p + refs.len; p < end;)
		{
			next_value_ref(&p, end, &root, &n);
			visit(&root.hash, arg);
		}
	}
}

/*
 * The longest key levelbuf_add() takes at every level: an internal item of
 * it, with the longest child reference (a hash of hash's, the largest
 * count), takes at most half a node.  A leaf item of it fits in a node
 * with a value of up to half a node, and a leaf value is the references it
 * holds, then at most VALUE_INLINE_MAX bytes or a value tree's reference.
 */
static Size
cow_key_max(const ProllyChunkFormatRoutine *format,
			const ProllyHashRoutine *hash)
{
	Size		half = node_capacity(format) / 2;
	ProllyKey	key = {NULL, 0};
	ProllyHash	h;
	ProllyValue child;

	/* (a hash's length is the same for every hash a routine makes) */
	PROLLY_CALL(hash, finish, PROLLY_CALL(hash, begin, CurrentMemoryContext),
				&h);
	child.data = NULL;
	child.len = 1 + h.len + 10;

	/* (the space an item takes grows with its key, by a byte or more) */
	key.len = half;
	while (key.len > 0 &&
		   PROLLY_CALL(format, item_space, 1, &key, &child) > half)
		key.len--;
	return key.len;
}

const ProllyTreeRoutine prolly_tree_cow = {
	.info = {
		.iface = PROLLY_IFACE_TREE,
		.impl_id = COW_IMPL_ID,
		.format_version = 3,
		.name = "cow",
		.description = "copy-on-write prolly trees in a chunk store",
	},
	.build_begin = cow_build_begin,
	.build_add = cow_build_add,
	.build_finish = cow_build_finish,
	.get = cow_get,
	.cursor_open = cow_cursor_open,
	.cursor_seek = cow_cursor_seek,
	.cursor_next = cow_cursor_next,
	.cursor_close = cow_cursor_close,
	.apply = cow_apply,
	.keep_nodes = cow_keep_nodes,
	.count = cow_count,
	.walk = cow_walk,
	.diff = cow_diff,
	.diff_nodes = cow_diff_nodes,
	.diff_unordered = cow_diff_unordered,
	.diff_estimate = cow_diff_estimate,
	.rank = cow_rank,
	.references = cow_references,
	.value_write = write_value_tree,
	.value_read = read_value,
	.value_splice = splice_value,
	.value_reference = cow_value_reference,
	.value_root = cow_value_root,
	.value_present = cow_value_present,
	.key_max = cow_key_max,
};
