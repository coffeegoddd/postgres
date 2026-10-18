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
 * out of line, in a value tree: the value's bytes cut into chunks where a
 * rolling hash (buzhash) of the last bytes says so, keyed by their offsets
 * (8 bytes, big-endian), built as any tree is.  The leaf item then holds
 *
 *	uint8 hash length, hash of the value tree's root, varint value length
 *
 * with the chunk format's PROLLY_ITEM_VALUE_REF flag.  Every read (get,
 * cursors, diff) returns the bytes; walks visit the value trees too.  The
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

#include "access/prolly_builtins.h"
#include "access/prolly_datum.h"
#include "miscadmin.h"
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
 * A chunk node by hash, from the chunk store, its bytes allocated in cxt:
 * the context of whatever keeps the node (an iterator, a cursor), which
 * may outlive the one current at the read.
 */
static void
node_read_in(const ProllyTreeEnv *env, const ProllyNodeRef *ref, TNode *node,
			 MemoryContext cxt)
{
	StringInfoData data;
	MemoryContext old = MemoryContextSwitchTo(cxt);

	node->chunk.data = NULL;
	initStringInfo(&data);
	MemoryContextSwitchTo(old);
	if (!PROLLY_CALL(env->chunk_store, get, env->chunks, &ref->hash, &data))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly tree node missing from its chunk store")));
	if (env->counters)
		env->counters->nodes_read++;
	if (!PROLLY_CALL(env->chunk_format, open, (const uint8 *) data.data,
					 data.len, &node->chunk))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly chunk is not a tree node")));
	node->level = node->chunk.level;
	node->nitems = node->chunk.nitems;
	node->ref = *ref;
}

/* Read a node kept no longer than the current context. */
static void
node_read(const ProllyTreeEnv *env, const ProllyNodeRef *ref, TNode *node)
{
	node_read_in(env, ref, node, CurrentMemoryContext);
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
	bool		ref;			/* leaf items: the value is a value tree's */
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
		flags[i] = items[i].ref ? PROLLY_ITEM_VALUE_REF : 0;
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
	pfree(chunk.data);
	pfree(keys);
	pfree(values);
	pfree(flags);
}

static Size
level_capacity(const ProllyTreeEnv *env, uint16 level)
{
	return CHUNK_NODE_MAX - PROLLY_CALL(env->chunk_format, header_size);
}

/* ------------------------------------------------------------------------
 * Values out of line
 * ------------------------------------------------------------------------
 */

#define VALUE_INLINE_MAX	2048	/* longer leaf values go out of line */
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
}

static void
encode_value_ref(StringInfo out, const ProllyHash *hash, uint64 len)
{
	appendStringInfoChar(out, (char) hash->len);
	appendBinaryStringInfo(out, hash->bytes, hash->len);
	prolly_put_varint(out, len);
}

static void
decode_value_ref(const ProllyValue *value, ProllyNodeRef *root, uint64 *len)
{
	const char *p = (const char *) value->data;
	const char *end = p + value->len;

	if (value->len < 1 || (uint8) p[0] > PROLLY_HASH_MAX_LEN ||
		value->len < 1 + (uint32) (uint8) p[0])
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid prolly value reference")));
	memset(root, 0, sizeof(*root));
	root->hash.len = (uint8) p[0];
	memcpy(root->hash.bytes, p + 1, root->hash.len);
	p += 1 + root->hash.len;
	*len = prolly_get_varint(&p, end);
}

static ProllyTreeBuilder *builder_create(const ProllyTreeEnv *env, uint16 base,
										 MemoryContext mcxt);
static ProllyTreeCursor *cow_cursor_open(const ProllyTreeEnv *env,
										 const ProllyNodeRef *root,
										 MemoryContext mcxt);
static bool cow_cursor_next(ProllyTreeCursor *c, ProllyKey *key,
							ProllyValue *value);
static void cow_cursor_close(ProllyTreeCursor *c);
static void builder_add_at(ProllyTreeBuilder *b, uint16 level, const Item *item);
static void builder_finish(ProllyTreeBuilder *b, ProllyNodeRef *root);

/* Store value's bytes as a value tree; append its reference to out. */
static void
write_value_tree(const ProllyTreeEnv *env, const ProllyValue *value,
				 StringInfo out)
{
	ProllyTreeEnv venv;
	ProllyTreeBuilder *b;
	ProllyNodeRef root;
	uint32		start = 0;
	uint32		h = 0;

	if (!buz_ready)
		buz_init();
	value_env(env, &venv);
	b = builder_create(&venv, 0, CurrentMemoryContext);
	for (uint32 i = 0; i < value->len; i++)
	{
		uint32		n = i - start + 1;	/* bytes in the chunk so far */
		bool		cut;

		h = rotl32(h, 1) ^ buz_table[value->data[i]];
		if (n > BUZ_WINDOW)
			h ^= rotl32(buz_table[value->data[i - BUZ_WINDOW]], BUZ_WINDOW);
		cut = (n >= CHUNK_MIN && (h & CHUNK_MASK) == CHUNK_MASK) ||
			n >= CHUNK_MAX || i + 1 == value->len;
		if (cut)
		{
			uint8		keybuf[8];
			Item		item;

			for (int k = 0; k < 8; k++)
				keybuf[k] = (uint8) (((uint64) start) >> (8 * (7 - k)));
			item.key.data = keybuf;
			item.key.len = 8;
			item.value.data = value->data + start;
			item.value.len = n;
			item.count = 1;
			item.ref = false;
			builder_add_at(b, 0, &item);
			start = i + 1;
			h = 0;
		}
	}
	builder_finish(b, &root);
	encode_value_ref(out, &root.hash, value->len);
}

/* Append the bytes of the value a reference refers to. */
static void
resolve_value(const ProllyTreeEnv *env, const ProllyValue *ref, StringInfo out)
{
	ProllyTreeEnv venv;
	ProllyNodeRef root;
	uint64		len;
	ProllyTreeCursor *c;
	ProllyKey	k;
	ProllyValue v;
	int			before = out->len;

	value_env(env, &venv);
	decode_value_ref(ref, &root, &len);
	c = cow_cursor_open(&venv, &root, CurrentMemoryContext);
	while (cow_cursor_next(c, &k, &v))
		appendBinaryStringInfo(out, (const char *) v.data, v.len);
	cow_cursor_close(c);
	if ((uint64) (out->len - before) != len)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly value tree holds %d bytes, its reference says %llu",
						out->len - before, (unsigned long long) len)));
}

/*
 * A leaf item for key and value, the value stored out of line if it is
 * long (its reference then built in refbuf, which the item points into).
 */
static void
leaf_item(const ProllyTreeEnv *env, const ProllyKey *key,
		  const ProllyValue *value, Item *item, StringInfo refbuf)
{
	item->key = *key;
	item->count = 1;
	item->ref = value->len > VALUE_INLINE_MAX;
	if (item->ref)
	{
		write_value_tree(env, value, refbuf);
		item->value.data = (const uint8 *) refbuf->data;
		item->value.len = refbuf->len;
	}
	else
		item->value = *value;
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
	parent.ref = false;
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
	StringInfoData refbuf;

	initStringInfo(&refbuf);
	leaf_item(b->env, key, value, &item, &refbuf);
	builder_add_at(b, 0, &item);
	pfree(refbuf.data);
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
	node_read_in(env, root, &it->f[0].node, it->cxt);
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
	node_read_in(it->env, &ref, &it->f[d + 1].node, it->cxt);
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
	item->ref = (flags & PROLLY_ITEM_VALUE_REF) != 0;
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
			if (flags & PROLLY_ITEM_VALUE_REF)
				resolve_value(env, &v, value);
			else
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
 * Tell the store which chunks the walk reads next: an internal node's
 * children, or a leaf's value trees (a source a store reads through can
 * fetch them at once).
 */
static void
prefetch_below(const ProllyTreeEnv *env, TNode *node)
{
	ProllyHash *hashes;
	int			n = 0;

	/* (only when a hint would hear them) */
	if (env->chunk_store == NULL || !PROLLY_HAS(env->chunk_store, prefetch) ||
		node->nitems == 0 ||
		!PROLLY_CALL(env->chunk_store, prefetch, env->chunks, NULL, 0))
		return;
	hashes = palloc(sizeof(ProllyHash) * node->nitems);
	for (int i = 0; i < node->nitems; i++)
	{
		ProllyNodeRef ref;

		if (node->level > 0)
		{
			uint64		count;

			node_child(env, node, i, &ref, &count);
		}
		else
		{
			ProllyKey	k;
			ProllyValue v;
			uint8		flags;
			uint64		len;

			node_item(env, node, i, &k, &v, &flags);
			if (!(flags & PROLLY_ITEM_VALUE_REF))
				continue;
			decode_value_ref(&v, &ref, &len);
		}
		hashes[n++] = ref.hash;
	}
	(void) PROLLY_CALL(env->chunk_store, prefetch, env->chunks, hashes, n);
	pfree(hashes);
}

/* Whether the visitor took the node. */
static bool
walk_node(const ProllyTreeEnv *env, const ProllyNodeRef *ref,
		  ProllyTreeVisitNode visit, void *arg)
{
	TNode		node;
	bool		took;

	CHECK_FOR_INTERRUPTS();
	node_read(env, ref, &node);
	took = visit(&node.ref, node.level, arg);
	if (took)
		prefetch_below(env, &node);
	if (!took)
		;						/* the visitor has seen what is below */
	else if (node.level == 0)
	{
		/* Value trees of values stored out of line. */
		for (int i = 0; i < node.nitems; i++)
		{
			ProllyKey	k;
			ProllyValue v;
			uint8		flags;

			node_item(env, &node, i, &k, &v, &flags);
			if (flags & PROLLY_ITEM_VALUE_REF)
			{
				ProllyTreeEnv venv;
				ProllyNodeRef vroot;
				uint64		len;

				value_env(env, &venv);
				decode_value_ref(&v, &vroot, &len);
				(void) walk_node(&venv, &vroot, visit, arg);
			}
		}
	}
	else
	{
		for (int i = 0; i < node.nitems; i++)
		{
			ProllyNodeRef child;
			uint64		count;

			node_child(env, &node, i, &child, &count);
			(void) walk_node(env, &child, visit, arg);
		}
	}
	node_release(&node);
	return took;
}

static bool
cow_walk(const ProllyTreeEnv *env, const ProllyNodeRef *root,
		 ProllyTreeVisitNode visit, void *arg)
{
	return walk_node(env, root, visit, arg);
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
	if (flags & PROLLY_ITEM_VALUE_REF)
	{
		resetStringInfo(&c->value);
		resolve_value(it->env, value, &c->value);
		value->data = (const uint8 *) c->value.data;
		value->len = c->value.len;
	}
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
	e.item.ref = false;
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
			 * chunker means the window has resynchronized.
			 */
			if (!iter_next_node(&it))
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
cow_apply(const ProllyTreeEnv *env, const ProllyNodeRef *root,
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
			StringInfoData refbuf;

			/* In the apply's memory, which lasts until it ends. */
			initStringInfo(&refbuf);
			leaf_item(env, &edits[i].key, &edits[i].value, &e.item, &refbuf);
		}
		e.is_delete = edits[i].is_delete;
		editlist_add(&cur, &e);
	}

	node_read(env, root, &rootnode);
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
		apply_root(env, root, &cur, &result);

		/* A root with one child isn't a root: its child is. */
		for (;;)
		{
			TNode		n;
			ProllyNodeRef child;
			uint64		count;

			node_read(env, &result, &n);
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
	node_read_in(env, root, &s->f[0].node, s->cxt);
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
	node_read_in(s->env, &ref, &child->node, s->cxt);
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
	if (flags & PROLLY_ITEM_VALUE_REF)
	{
		resetStringInfo(buf);
		resolve_value(env, value, buf);
		value->data = (const uint8 *) buf->data;
		value->len = buf->len;
	}
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
				visit(PROLLY_DIFF_ADDED, &kb, NULL, reported(env, &vb, fb, &bufb),
					  arg);
				side_advance(&b);
			}
			else
			{
				side_leaf_item(&a, &ka, &va, &fa);
				visit(PROLLY_DIFF_REMOVED, &ka, reported(env, &va, fa, &bufa),
					  NULL, arg);
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
			visit(PROLLY_DIFF_REMOVED, &ka, reported(env, &va, fa, &bufa), NULL,
				  arg);
			side_advance(&a);
		}
		else if (r > 0)
		{
			visit(PROLLY_DIFF_ADDED, &kb, NULL, reported(env, &vb, fb, &bufb),
				  arg);
			side_advance(&b);
		}
		else
		{
			/*
			 * Equal keys can be written differently; that is a change too.
			 * Values out of line are equal exactly when their references are.
			 */
			if (fa != fb || va.len != vb.len || memcmp(va.data, vb.data, va.len) != 0 ||
				ka.len != kb.len || memcmp(ka.data, kb.data, ka.len) != 0)
				visit(PROLLY_DIFF_MODIFIED, &kb, reported(env, &va, fa, &bufa),
					  reported(env, &vb, fb, &bufb), arg);
			side_advance(&a);
			side_advance(&b);
		}
	}
	side_close(&a);
	side_close(&b);
	pfree(bufa.data);
	pfree(bufb.data);
}

/* A leaf item's value tree, if it is stored out of line: walked. */
static void
walk_value(const ProllyTreeEnv *env, const ProllyValue *value, uint8 flags,
		   ProllyTreeVisitNode visit, void *arg)
{
	ProllyTreeEnv venv;
	ProllyNodeRef vroot;
	uint64		len;

	if (!(flags & PROLLY_ITEM_VALUE_REF))
		return;
	value_env(env, &venv);
	decode_value_ref(value, &vroot, &len);
	(void) walk_node(&venv, &vroot, visit, arg);
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
		(void) walk_node(env, to, visit, arg);
		return;
	}
	if (same_hash(&from->hash, &to->hash))
		return;
	side_open(&a, env, from);
	side_open(&b, env, to);
	if (b.depth >= 0)
		(void) visit(to, side_height(&b), arg);

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
				(void) walk_node(env, &cb, visit, arg);
			}
			else
			{
				side_leaf_item(&b, &kb, &vb, &fb);
				walk_value(env, &vb, fb, visit, arg);
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
					walk_value(env, &vb, fb, visit, arg);
				if (r == 0)
					side_advance(&a);
				side_advance(&b);
			}
		}
	}
	side_close(&a);
	side_close(&b);
}

const ProllyTreeRoutine prolly_tree_cow = {
	.info = {
		.iface = PROLLY_IFACE_TREE,
		.impl_id = COW_IMPL_ID,
		.format_version = 1,
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
	.count = cow_count,
	.walk = cow_walk,
	.diff = cow_diff,
	.diff_nodes = cow_diff_nodes,
};
