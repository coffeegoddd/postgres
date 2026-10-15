/*-------------------------------------------------------------------------
 *
 * engine_d1.c
 *	  D1 implementation of the engine interface.
 *
 * D1: immutable copy-on-write: hash-addressed nodes, full-chunk WAL.
 *
 * Roots.  A database's working set is one branch root (root layout
 * interface), whose head the commit pipeline keeps.  A relation's trees
 * are found in it by relfilenumber: its data tree in the catalog or
 * user-data sub-root, its local metadata (row identity map) in the local
 * sub-root.  Local metadata of every kind shares one tree per relation,
 * each key prefixed by its kind byte, except local metadata by row
 * (PROLLY_TREE_ROWKEY), which has its own tree in the keyed local sub-root,
 * ordered as the data tree is.  A relation absent from the root
 * has empty trees, and an absent root (a database that never committed a
 * prolly write) has no relations.
 *
 * Private writes.  A transaction's writes stay in memory, in a write set
 * per tree it changed: a red-black tree of keys, each with the versions
 * written to it, tagged with the command id and subtransaction that wrote
 * them.  Cursors and lookups merge the base tree with the write set,
 * seeing a key's newest version from an earlier command, so a command
 * never sees its own writes (Halloween protection) and later commands see
 * earlier ones.  Aborting subtransaction S drops every version written by
 * S or a subtransaction started after it, which are exactly S's
 * descendants.  Nothing reaches the node store until commit.
 *
 * Key order.  A data tree is ordered by the keying its META tree records
 * (PROLLY_META_KEYING) when it is only read, and by the relation's current
 * keying when it is written: the table AM rebuilds a tree whose keying
 * changed before writing it.
 *
 * Commit.  xact_precommit applies every write set's newest versions to
 * the tree it was based on, then locks the branch and looks at the tip:
 * a tree nobody else changed meanwhile takes the result as it is; one
 * another transaction changed gets the same edits replayed onto the tip's
 * version.  It builds the new branch root and enqueues it; xact_commit
 * publishes it.  The replay needs no merge: D1 relies on its writers
 * holding the right to write what they write (a row's key, or a whole
 * tree being rebuilt, from coordination) from before they write it until
 * their transaction ends, so no other transaction committed a change to
 * a key in the write set, and a replaced tree (emptied or rebuilt) was
 * written by nobody else.
 *
 * Snapshots.  An MVCC snapshot reads the root of the newest commit it sees
 * as committed (the commit pipeline's snapshot_head), so every scan of a
 * snapshot reads the same roots, and a prolly table agrees with heap
 * tables read with the same snapshot.  Other snapshots (dirty, self, any,
 * none) read the newest head, as writes do.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/engine_d1.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"
#include "access/prolly_xact.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "lib/rbtree.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/* ------------------------------------------------------------------------
 * Per-backend state
 * ------------------------------------------------------------------------
 */

/* Components and the open node store, set up on first use. */
typedef struct D1Backend
{
	bool		ready;
	ProllyComponents c;
	ProllyNodeStore *store;
	ProllyTreeEnv memcmp_env;	/* layout and local-metadata trees */
	ProllyRootEnv root_env;
	ProllyNodeCounters counters;	/* every tree's, through memcmp_env */
} D1Backend;

static D1Backend backend;

struct ProllyRootSet
{
	bool		has_root;
	ProllyNodeRef root;
};

/* One version of a key, in a write set. */
typedef struct D1Version
{
	CommandId	cid;
	SubTransactionId subxid;
	bool		is_delete;
	uint8	   *value;
	uint32		len;

	/*
	 * The key as this version wrote it, if its bytes differ from the
	 * entry's (keys equal by the tree's order, such as 'a' and 'A' under a
	 * case-insensitive collation); NULL: the entry's.
	 */
	uint8	   *key;
	uint32		keylen;
} D1Version;

/* A key in a write set, with its versions in the order they were written. */
typedef struct D1Entry
{
	RBTNode		rbtnode;
	uint8	   *key;
	uint32		keylen;
	int			nversions;
	int			maxversions;
	D1Version  *versions;
} D1Entry;

/*
 * A tree's key order: a table's data tree by its table keying, an index's
 * tree by its index keying; neither set means memcmp (local metadata).
 */
typedef struct D1Order
{
	const ProllyKeyingRoutine *routine;
	ProllyKeying *keying;
	const ProllyIndexKeyingRoutine *ix_routine;
	ProllyIndexKeying *ix;
	bool		suffixed;		/* keys are a data key, then suffix bytes */
} D1Order;

static inline bool
order_set(const D1Order *order)
{
	return order->keying != NULL || order->ix != NULL;
}

/* A write set's contents before it was emptied, for subtransaction abort. */
typedef struct D1Saved
{
	SubTransactionId subxid;	/* the subtransaction that emptied it */
	RBTree	   *entries;
	int			nentries;
	ProllyNodeRef base;
	bool		replaced;
	D1Order		order;
	struct D1Saved *prev;
} D1Saved;

/*
 * A tree's private writes.  Keys of local-metadata trees include the kind
 * prefix.
 */
typedef struct D1WriteSet
{
	ProllySubRoot sub;
	RelFileNumber relnumber;
	SubTransactionId created_subxid;

	ProllyNodeRef base;			/* hash.len == 0: empty */
	bool		replaced;		/* new storage: base ignores the head */

	RBTree	   *entries;
	int			nentries;
	D1Saved    *saved;			/* contents before each emptying */
	D1Order		order;			/* data and index trees: the key order */
	struct D1WriteSet *next;	/* all write sets, newest first */
} D1WriteSet;

typedef struct D1WsKey
{
	ProllySubRoot sub;
	RelFileNumber relnumber;
} D1WsKey;

typedef struct D1WsHashEntry
{
	D1WsKey		key;
	D1WriteSet *ws;
} D1WsHashEntry;

/* The current transaction's state. */
typedef struct D1Xact
{
	MemoryContext mcxt;
	HTAB	   *writesets;
	D1WriteSet *all;
	bool		enqueued;
	ProllyCommitTicket ticket;
} D1Xact;

static D1Xact *xact = NULL;

static int	entry_compare(const RBTNode *a, const RBTNode *b, void *arg);
static void entry_combine(RBTNode *existing, const RBTNode *newdata, void *arg);
static RBTNode *entry_alloc(void *arg);
static void entry_free(RBTNode *node, void *arg);

static int
memcmp_keys(const ProllyKey *a, const ProllyKey *b, void *arg)
{
	int			r = memcmp(a->data, b->data, Min(a->len, b->len));

	if (r != 0)
		return r;
	return (a->len > b->len) - (a->len < b->len);
}

static void
backend_init(void)
{
	if (backend.ready)
		return;
	prolly_components_for_engine(&prolly_engine_d1.info, &backend.c);
	backend.store = PROLLY_CALL(backend.c.node_store, open, NULL);
	memset(&backend.memcmp_env, 0, sizeof(backend.memcmp_env));
	backend.memcmp_env.node_format = backend.c.node_format;
	backend.memcmp_env.chunker = backend.c.chunker;
	backend.memcmp_env.hash = backend.c.hash;
	backend.memcmp_env.node_store = backend.c.node_store;
	backend.memcmp_env.store = backend.store;
	backend.memcmp_env.log = backend.c.log;
	backend.memcmp_env.compare = memcmp_keys;
	backend.memcmp_env.counters = &backend.counters;
	backend.root_env.tree = backend.c.tree;
	backend.root_env.tree_env = &backend.memcmp_env;
	backend.ready = true;
}

/* ------------------------------------------------------------------------
 * Which tree
 * ------------------------------------------------------------------------
 */

static void
check_kind(ProllyTreeKind kind)
{
	if ((int) kind < 0 || (int) kind > PROLLY_TREE_KIND_LAST)
		elog(ERROR, "invalid prolly tree kind %d", (int) kind);
}

static ProllySubRoot
subroot_for(Relation rel, ProllyTreeKind kind)
{
	check_kind(kind);
	if (kind == PROLLY_TREE_ROWKEY)
		return PROLLY_SUBROOT_LOCAL_KEYED;
	if (kind != PROLLY_TREE_DATA)
		return PROLLY_SUBROOT_LOCAL;
	return IsCatalogRelation(rel) ? PROLLY_SUBROOT_CATALOG :
		PROLLY_SUBROOT_USER_DATA;
}

static bool
is_local(ProllySubRoot sub)
{
	return sub == PROLLY_SUBROOT_LOCAL;
}

/* A key with its kind prefix, for local-metadata trees. */
typedef struct D1KeyBuf
{
	StringInfoData buf;
	ProllyKey	key;
} D1KeyBuf;

static const ProllyKey *
full_key(ProllySubRoot sub, ProllyTreeKind kind, const ProllyKey *key,
		 D1KeyBuf *kb)
{
	if (!is_local(sub))
		return key;
	initStringInfo(&kb->buf);
	appendStringInfoChar(&kb->buf, (char) kind);
	if (key)
		appendBinaryStringInfo(&kb->buf, (const char *) key->data, key->len);
	kb->key.data = (const uint8 *) kb->buf.data;
	kb->key.len = kb->buf.len;
	return &kb->key;
}

static void
free_key(ProllySubRoot sub, D1KeyBuf *kb)
{
	if (is_local(sub))
		pfree(kb->buf.data);
}


static int
keying_compare(const ProllyKey *a, const ProllyKey *b, void *arg)
{
	D1Order    *o = arg;
	ProllyKey	da;
	ProllyKey	db;
	int			r;

	if (o->ix)
		return PROLLY_CALL(o->ix_routine, compare, o->ix, a, b);
	if (!o->suffixed)
		return PROLLY_CALL(o->routine, compare, o->keying, a, b);

	/* By the data keys, then by the suffixes' bytes. */
	if (a->len < PROLLY_ROWKEY_SUFFIX_LEN || b->len < PROLLY_ROWKEY_SUFFIX_LEN)
		elog(ERROR, "prolly key by row is too short");
	da.data = a->data;
	da.len = a->len - PROLLY_ROWKEY_SUFFIX_LEN;
	db.data = b->data;
	db.len = b->len - PROLLY_ROWKEY_SUFFIX_LEN;
	r = PROLLY_CALL(o->routine, compare, o->keying, &da, &db);
	if (r != 0)
		return r;
	return memcmp(a->data + da.len, b->data + db.len, PROLLY_ROWKEY_SUFFIX_LEN);
}

/* The tree environment for a tree in order (NULL: memcmp). */
static void
tree_env(const D1Order *order, ProllyTreeEnv *env)
{
	*env = backend.memcmp_env;
	if (order && order_set(order))
	{
		env->compare = keying_compare;
		env->compare_arg = (void *) order;
	}
}

static bool d1_get(ProllyRootSet *roots, Relation tree, ProllyTreeKind kind,
				   const ProllyKey *key, CommandId cid, StringInfo value);

/*
 * A relation's key order for its data tree (or an index's tree): as
 * recorded in roots (with the transaction's writes) if roots is given,
 * else its current keying.
 */
static void
open_order(Relation rel, ProllySubRoot sub, ProllyRootSet *roots,
		   MemoryContext mcxt, D1Order *order)
{
	ProllyComponents c;
	StringInfoData desc;
	ProllyKey	key = {(const uint8 *) PROLLY_META_KEYING, PROLLY_META_KEYING_LEN};
	ProllyValue d;
	bool		recorded = false;

	memset(order, 0, sizeof(*order));
	if (is_local(sub))
		return;
	order->suffixed = (sub == PROLLY_SUBROOT_LOCAL_KEYED);
	prolly_components_for_relation(rel, &c);
	initStringInfo(&desc);
	if (roots)
		recorded = d1_get(roots, rel, PROLLY_TREE_META, &key, InvalidCommandId,
						  &desc);
	d.data = (const uint8 *) desc.data;
	d.len = desc.len;
	if (rel->rd_rel->relkind == RELKIND_INDEX)
	{
		order->ix_routine = c.index_keying;
		order->ix = PROLLY_CALL(c.index_keying, open, rel,
								recorded ? &d : NULL, mcxt);
	}
	else
	{
		order->routine = c.keying;
		order->keying = PROLLY_CALL(c.keying, open, rel, recorded ? &d : NULL,
									mcxt);
	}
	pfree(desc.data);
}

static void
close_order(D1Order *order)
{
	if (order->keying)
		PROLLY_CALL(order->routine, close, order->keying);
	if (order->ix)
		PROLLY_CALL(order->ix_routine, close, order->ix);
}

/* relnumber's tree in sub of root, or an empty reference. */
static void
find_tree(const ProllyRootSet *roots, ProllySubRoot sub, RelFileNumber relnumber,
		  ProllyNodeRef *tree)
{
	memset(tree, 0, sizeof(*tree));
	tree->location.blkno = InvalidBlockNumber;
	if (roots == NULL || !roots->has_root)
		return;
	if (!PROLLY_CALL(backend.c.root_layout, lookup, &backend.root_env,
					 &roots->root, sub, relnumber, tree))
	{
		memset(tree, 0, sizeof(*tree));
		tree->location.blkno = InvalidBlockNumber;
	}
}

static void
current_head(ProllyRootSet *roots)
{
	roots->has_root = PROLLY_CALL(backend.c.commit, head, PROLLY_MAIN_BRANCH,
								  &roots->root);
}

/* ------------------------------------------------------------------------
 * Write sets
 * ------------------------------------------------------------------------
 */

static void
xact_init(void)
{
	HASHCTL		ctl;

	if (xact != NULL)
		return;
	xact = MemoryContextAllocZero(TopTransactionContext, sizeof(D1Xact));
	xact->mcxt = AllocSetContextCreate(TopTransactionContext,
									   "prolly d1 private writes",
									   ALLOCSET_DEFAULT_SIZES);
	ctl.keysize = sizeof(D1WsKey);
	ctl.entrysize = sizeof(D1WsHashEntry);
	ctl.hcxt = xact->mcxt;
	xact->writesets = hash_create("prolly d1 write sets", 16, &ctl,
								  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	prolly_xact_register_engine(&prolly_engine_d1);
}

static void
xact_reset(void)
{
	if (xact == NULL)
		return;
	/* Keyings live in xact->mcxt and hold nothing else. */
	MemoryContextDelete(xact->mcxt);
	pfree(xact);
	xact = NULL;
}

static D1WriteSet *
find_writeset(ProllySubRoot sub, RelFileNumber relnumber)
{
	D1WsKey		key;
	D1WsHashEntry *he;

	if (xact == NULL)
		return NULL;
	memset(&key, 0, sizeof(key));
	key.sub = sub;
	key.relnumber = relnumber;
	he = hash_search(xact->writesets, &key, HASH_FIND, NULL);
	return he ? he->ws : NULL;
}

/* Keep the key order of a data tree's write set current while it's empty. */
static void
refresh_order(D1WriteSet *ws, Relation rel)
{
	D1Order		order;

	if (is_local(ws->sub) || ws->nentries > 0)
		return;

	/*
	 * The old keying is left to be freed with the transaction's memory: a
	 * saved state may still order its entries by it.
	 */
	open_order(rel, ws->sub, NULL, xact->mcxt, &order);
	ws->order = order;
}

/*
 * The write set of relnumber's tree in sub, made if needed.  A new write
 * set over existing storage starts from the newest head, read after taking
 * the relation's write lock.
 */
static D1WriteSet *
get_writeset(Relation rel, ProllySubRoot sub, RelFileNumber relnumber,
			 bool replace)
{
	D1WsKey		key;
	D1WsHashEntry *he;
	bool		found;
	D1WriteSet *ws;
	MemoryContext old;

	backend_init();
	xact_init();
	memset(&key, 0, sizeof(key));
	key.sub = sub;
	key.relnumber = relnumber;
	he = hash_search(xact->writesets, &key, HASH_FIND, &found);
	if (found)
	{
		ws = he->ws;
		if (replace)
		{
			/*
			 * Emptied again (truncate, rebuild): start over, keeping the old
			 * contents until the subtransaction is known to commit.
			 */
			D1Saved    *saved;

			old = MemoryContextSwitchTo(xact->mcxt);
			saved = palloc(sizeof(D1Saved));
			saved->subxid = GetCurrentSubTransactionId();
			saved->entries = ws->entries;
			saved->nentries = ws->nentries;
			saved->base = ws->base;
			saved->replaced = ws->replaced;
			saved->order = ws->order;
			saved->prev = ws->saved;
			ws->saved = saved;
			ws->entries = rbt_create(sizeof(D1Entry), entry_compare,
									 entry_combine, entry_alloc, entry_free, ws);
			MemoryContextSwitchTo(old);
			ws->nentries = 0;
			memset(&ws->base, 0, sizeof(ws->base));
			ws->base.location.blkno = InvalidBlockNumber;
			ws->replaced = true;
		}
		refresh_order(ws, rel);
		return ws;
	}

	/*
	 * The newest head is the base; others may commit changes to keys this
	 * transaction doesn't write meanwhile, which commit replays these
	 * writes over.  The write set is entered only once it's complete, so
	 * an error here leaves nothing behind for a retry to find.
	 */
	(void) GetCurrentTransactionId();

	old = MemoryContextSwitchTo(xact->mcxt);
	ws = palloc0(sizeof(D1WriteSet));
	ws->sub = sub;
	ws->relnumber = relnumber;
	ws->entries = rbt_create(sizeof(D1Entry), entry_compare, entry_combine,
							 entry_alloc, entry_free, ws);
	MemoryContextSwitchTo(old);
	ws->base.location.blkno = InvalidBlockNumber;
	ws->created_subxid = GetCurrentSubTransactionId();
	if (replace)
		ws->replaced = true;
	else
	{
		ProllyRootSet head;

		current_head(&head);
		find_tree(&head, sub, relnumber, &ws->base);
	}
	he = hash_search(xact->writesets, &key, HASH_ENTER, NULL);
	he->ws = ws;
	ws->next = xact->all;
	xact->all = ws;
	refresh_order(ws, rel);
	return ws;
}

static int
ws_compare(D1WriteSet *ws, const uint8 *a, uint32 alen, const uint8 *b,
		   uint32 blen)
{
	ProllyKey	ka = {a, alen};
	ProllyKey	kb = {b, blen};

	if (order_set(&ws->order))
		return keying_compare(&ka, &kb, &ws->order);
	return memcmp_keys(&ka, &kb, NULL);
}

static int
entry_compare(const RBTNode *a, const RBTNode *b, void *arg)
{
	const D1Entry *ea = (const D1Entry *) a;
	const D1Entry *eb = (const D1Entry *) b;

	return ws_compare(arg, ea->key, ea->keylen, eb->key, eb->keylen);
}

static void
entry_combine(RBTNode *existing, const RBTNode *newdata, void *arg)
{
	/* Callers find before inserting, so keys are never combined. */
	elog(ERROR, "prolly d1 write set key inserted twice");
}

static RBTNode *
entry_alloc(void *arg)
{
	return MemoryContextAlloc(xact->mcxt, sizeof(D1Entry));
}

/*
 * Only the node: rbt_delete() may have moved another entry's contents into
 * the node being deleted, so contents are freed before deleting.
 */
static void
entry_free(RBTNode *node, void *arg)
{
	pfree(node);
}

static D1Entry *
ws_find(D1WriteSet *ws, const ProllyKey *key)
{
	D1Entry		probe;

	probe.key = (uint8 *) key->data;
	probe.keylen = key->len;
	return (D1Entry *) rbt_find(ws->entries, &probe.rbtnode);
}

static void
ws_write(D1WriteSet *ws, const ProllyKey *key, const ProllyValue *value,
		 CommandId cid)
{
	D1Entry    *e = ws_find(ws, key);
	SubTransactionId subxid = GetCurrentSubTransactionId();
	MemoryContext old = MemoryContextSwitchTo(xact->mcxt);
	D1Version  *v;

	if (e == NULL)
	{
		D1Entry		probe;
		bool		isnew;

		memset(&probe, 0, sizeof(probe));
		probe.key = palloc(Max(key->len, 1));
		memcpy(probe.key, key->data, key->len);
		probe.keylen = key->len;
		e = (D1Entry *) rbt_insert(ws->entries, &probe.rbtnode, &isnew);
		Assert(isnew);
		ws->nentries++;
	}

	v = e->nversions > 0 ? &e->versions[e->nversions - 1] : NULL;
	if (v == NULL || v->cid != cid || v->subxid != subxid)
	{
		if (e->nversions == e->maxversions)
		{
			e->maxversions = Max(2, e->maxversions * 2);
			e->versions = e->versions ?
				repalloc(e->versions, sizeof(D1Version) * e->maxversions) :
				palloc(sizeof(D1Version) * e->maxversions);
		}
		v = &e->versions[e->nversions++];
		v->cid = cid;
		v->subxid = subxid;
	}
	else
	{
		/* The same command writing the key again. */
		if (v->value)
			pfree(v->value);
		if (v->key)
			pfree(v->key);
	}

	v->key = NULL;
	v->keylen = 0;
	if (key->len != e->keylen || memcmp(key->data, e->key, key->len) != 0)
	{
		v->key = palloc(Max(key->len, 1));
		memcpy(v->key, key->data, key->len);
		v->keylen = key->len;
	}
	v->is_delete = (value == NULL);
	v->value = NULL;
	v->len = 0;
	if (value)
	{
		v->value = palloc(Max(value->len, 1));
		memcpy(v->value, value->data, value->len);
		v->len = value->len;
	}
	MemoryContextSwitchTo(old);
}

/* The key bytes version v of entry e was written with. */
static inline ProllyKey
version_key(const D1Entry *e, const D1Version *v)
{
	ProllyKey	k;

	k.data = v->key ? v->key : e->key;
	k.len = v->key ? v->keylen : e->keylen;
	return k;
}

/*
 * The newest version a command cid may see; NULL if none.  Versions
 * written with InvalidCommandId (rebuilt rows) are seen by every command.
 */
static const D1Version *
visible_version(const D1Entry *e, CommandId cid)
{
	for (int i = e->nversions - 1; i >= 0; i--)
	{
		if (cid == InvalidCommandId || e->versions[i].cid < cid ||
			e->versions[i].cid == InvalidCommandId)
			return &e->versions[i];
	}
	return NULL;
}

/*
 * Keys that can't be ordered any more (their keying's type, operator
 * family or collation was dropped) are matched by their bytes instead:
 * the write set's entries, sorted by bytes, looked up by binary search.
 */
static bool
order_lost(const D1Order *order)
{
	return order->keying != NULL &&
		!PROLLY_CALL(order->routine, can_compare, order->keying);
}

static int
entry_bytes_cmp(const void *a, const void *b)
{
	const D1Entry *ea = *(D1Entry *const *) a;
	const D1Entry *eb = *(D1Entry *const *) b;
	ProllyKey	ka = {ea->key, ea->keylen};
	ProllyKey	kb = {eb->key, eb->keylen};

	return memcmp_keys(&ka, &kb, NULL);
}

static D1Entry **
entries_by_bytes(D1WriteSet *ws, int *n)
{
	D1Entry   **arr = palloc(sizeof(D1Entry *) * Max(ws->nentries, 1));
	RBTreeIterator it;
	D1Entry    *e;

	*n = 0;
	rbt_begin_iterate(ws->entries, LeftRightWalk, &it);
	while ((e = (D1Entry *) rbt_iterate(&it)) != NULL)
		arr[(*n)++] = e;
	qsort(arr, *n, sizeof(D1Entry *), entry_bytes_cmp);
	return arr;
}

static D1Entry *
find_by_bytes(D1Entry **arr, int n, const ProllyKey *key)
{
	int			lo = 0;
	int			hi = n - 1;

	while (lo <= hi)
	{
		int			mid = lo + (hi - lo) / 2;
		ProllyKey	k = {arr[mid]->key, arr[mid]->keylen};
		int			r = memcmp_keys(key, &k, NULL);

		if (r == 0)
			return arr[mid];
		if (r < 0)
			hi = mid - 1;
		else
			lo = mid + 1;
	}
	return NULL;
}

static void
order_lost_error(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot look up rows of this prolly table by key"),
			 errdetail("A type, operator family or collation its rows are keyed by has been dropped."),
			 errhint("Write to the table to rebuild it.")));
}

/* ------------------------------------------------------------------------
 * Root sets
 * ------------------------------------------------------------------------
 */

static ProllyRootSet *
d1_roots_acquire(Snapshot snapshot)
{
	ProllyRootSet *roots = palloc0(sizeof(ProllyRootSet));

	backend_init();
	if (snapshot != NULL && snapshot->snapshot_type == SNAPSHOT_MVCC)
		roots->has_root = PROLLY_CALL(backend.c.commit, snapshot_head,
									  PROLLY_MAIN_BRANCH, snapshot,
									  &roots->root);
	else
		current_head(roots);
	return roots;
}

static void
d1_roots_release(ProllyRootSet *roots)
{
	pfree(roots);
}

/*
 * A tree's base for reading: from roots, so a reader sees what was
 * committed in the root set it holds, with the transaction's own writes
 * over it.  A write set that replaced the tree (emptied or rebuilt it) has
 * its own base instead.  (A write set's base is the newest head when it
 * was made; a reader may hold an older root set, taken before the
 * transaction waited for the right to write the table.)
 */
static void
read_base(ProllyRootSet *roots, ProllySubRoot sub, RelFileNumber relnumber,
		  D1WriteSet *ws, ProllyNodeRef *base)
{
	if (ws && ws->replaced)
		*base = ws->base;
	else
		find_tree(roots, sub, relnumber, base);
}

/* ------------------------------------------------------------------------
 * Cursors
 * ------------------------------------------------------------------------
 */

struct ProllyCursor
{
	MemoryContext mcxt;
	ProllyRootSet *roots;
	Relation	tree;
	ProllySubRoot sub;
	ProllyTreeKind kind;
	CommandId	cid;
	D1Order		order;			/* data trees without a write set */
	ProllyTreeEnv env;
	D1WriteSet *ws;
	ProllyNodeRef base;			/* the base tree read */
	bool		replaced;		/* ...was the write set's own */

	/* the base tree */
	ProllyTreeCursor *tc;		/* NULL: empty */
	bool		t_have;
	bool		t_done;
	ProllyKey	t_key;
	ProllyValue t_value;

	/* the write set, by the last key taken from it */
	bool		o_started;
	bool		o_have;
	bool		o_done;
	D1Entry    *o_entry;
	StringInfoData o_bound;		/* seek key, or last key taken */
	bool		o_inclusive;
	bool		o_from_end;		/* no bound: start at the first/last key */

	ScanDirection dir;

	/* Keys matched by bytes: the base tree, then the write set's rest. */
	bool		unordered;
	D1Entry   **by_bytes;
	int			nby_bytes;
	int			by_bytes_pos;
};

static int
cursor_compare(ProllyCursor *c, const ProllyKey *a, const ProllyKey *b)
{
	return c->env.compare(a, b, c->env.compare_arg);
}

/* Does a full key of the base or write set belong to this cursor's kind? */
static bool
in_kind(ProllyCursor *c, const ProllyKey *key)
{
	return !is_local(c->sub) || (key->len > 0 && key->data[0] == (uint8) c->kind);
}

/* Find the cursor's write set, key order and base tree; in c->mcxt. */
static void
cursor_bind(ProllyCursor *c)
{
	RelFileNumber relnumber = c->tree->rd_locator.relNumber;

	c->ws = find_writeset(c->sub, relnumber);
	if (c->ws)
	{
		/* The write set's key order, which it keeps owning. */
		refresh_order(c->ws, c->tree);
		c->order = c->ws->order;
	}
	else
		open_order(c->tree, c->sub, c->roots, c->mcxt, &c->order);
	c->unordered = order_lost(&c->order);
	tree_env(c->unordered ? NULL : &c->order, &c->env);
	read_base(c->roots, c->sub, relnumber, c->ws, &c->base);
	c->replaced = c->ws && c->ws->replaced;
	c->tc = NULL;
	if (c->base.hash.len > 0)
		c->tc = PROLLY_CALL(backend.c.tree, cursor_open, &c->env, &c->base,
							c->mcxt);
	/* Unpositioned until a seek; an empty base stays done. */
	c->t_have = c->o_have = false;
	c->t_done = c->o_done = true;
}

static void
cursor_unbind(ProllyCursor *c)
{
	if (c->tc)
		PROLLY_CALL(backend.c.tree, cursor_close, c->tc);
	c->tc = NULL;
	if (c->ws == NULL)
		close_order(&c->order);
}

/* Whether two orders put keys in the same order (the same keying). */
static bool
same_order(const D1Order *a, const D1Order *b)
{
	StringInfoData da;
	StringInfoData db;
	bool		same;

	if (a->keying == NULL || b->keying == NULL)
		return a->keying == b->keying && a->ix == b->ix;
	if (a->keying == b->keying)
		return true;
	initStringInfo(&da);
	initStringInfo(&db);
	PROLLY_CALL(a->routine, describe, a->keying, &da);
	PROLLY_CALL(b->routine, describe, b->keying, &db);
	same = da.len == db.len && memcmp(da.data, db.data, da.len) == 0;
	pfree(da.data);
	pfree(db.data);
	return same;
}

/*
 * Whether the transaction has, since the cursor was bound, made the tree's
 * write set or given the tree a new base (emptied or rebuilt it).  A write
 * set already bound is read live, so later writes to it need no rebind.
 * A tree rebuilt in another key order (the table was re-keyed) is not
 * rebound: the cursor goes on reading the tree in the order it began with,
 * as a scan of the tree from before the rebuild does.
 */
static bool
cursor_stale(ProllyCursor *c)
{
	D1WriteSet *ws = find_writeset(c->sub, c->tree->rd_locator.relNumber);
	bool		changed;

	if (ws != c->ws || (ws && ws->replaced) != c->replaced)
		changed = true;
	else if (c->replaced)
		changed = ws->base.hash.len != c->base.hash.len ||
			memcmp(ws->base.hash.bytes, c->base.hash.bytes,
				   ws->base.hash.len) != 0;
	else
		changed = false;

	if (changed && c->order.keying != NULL)
	{
		D1Order		now;
		bool		same;

		if (ws)
		{
			refresh_order(ws, c->tree);
			return same_order(&c->order, &ws->order);
		}
		open_order(c->tree, c->sub, c->roots, CurrentMemoryContext, &now);
		same = same_order(&c->order, &now);
		close_order(&now);
		return same;
	}
	return changed;
}

static ProllyCursor *
d1_cursor_open(ProllyRootSet *roots, Relation tree, ProllyTreeKind kind,
			   CommandId cid)
{
	MemoryContext mcxt = AllocSetContextCreate(CurrentMemoryContext,
											   "prolly d1 cursor",
											   ALLOCSET_SMALL_SIZES);
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	ProllyCursor *c = palloc0(sizeof(ProllyCursor));

	backend_init();
	c->mcxt = mcxt;
	c->roots = roots;
	c->tree = tree;
	c->sub = subroot_for(tree, kind);
	c->kind = kind;
	c->cid = cid;
	cursor_bind(c);
	initStringInfo(&c->o_bound);
	c->dir = ForwardScanDirection;
	MemoryContextSwitchTo(old);
	return c;
}

static void
d1_cursor_seek(ProllyCursor *c, const ProllyKey *key, ScanDirection direction)
{
	MemoryContext old = MemoryContextSwitchTo(c->mcxt);
	bool		forward = !ScanDirectionIsBackward(direction);
	D1KeyBuf	kb;
	bool		kb_used = false;
	const ProllyKey *fk;
	uint8		next_kind;
	ProllyKey	bound;

	c->dir = forward ? ForwardScanDirection : BackwardScanDirection;

	/* A seek sees every write made before it, as a lookup does. */
	if (cursor_stale(c))
	{
		cursor_unbind(c);
		cursor_bind(c);
	}

	if (c->unordered)
	{
		/* Only a forward scan of everything needs no order. */
		if (key || !forward)
			order_lost_error();
		if (c->tc)
		{
			PROLLY_CALL(backend.c.tree, cursor_seek, c->tc, NULL, c->dir);
			c->t_done = false;
		}
		c->t_have = false;
		if (c->by_bytes)
			pfree(c->by_bytes);
		c->by_bytes = NULL;
		c->nby_bytes = 0;
		if (c->ws)
			c->by_bytes = entries_by_bytes(c->ws, &c->nby_bytes);
		c->by_bytes_pos = 0;
		MemoryContextSwitchTo(old);
		return;
	}

	/* Where the base tree starts. */
	if (key || (is_local(c->sub) && forward))
	{
		fk = full_key(c->sub, c->kind, key, &kb);
		kb_used = is_local(c->sub);
	}
	else if (!is_local(c->sub))
		fk = NULL;
	else
	{
		/*
		 * Backward from the end of a local kind: from just below the next
		 * kind's prefix, which no key equals (every key is longer).
		 */
		next_kind = (uint8) c->kind + 1;
		bound.data = &next_kind;
		bound.len = 1;
		fk = &bound;
	}

	if (c->tc)
	{
		PROLLY_CALL(backend.c.tree, cursor_seek, c->tc, fk, c->dir);
		c->t_done = false;
	}
	c->t_have = false;

	resetStringInfo(&c->o_bound);
	c->o_from_end = (fk == NULL);
	if (fk)
		appendBinaryStringInfo(&c->o_bound, (const char *) fk->data, fk->len);
	c->o_inclusive = true;
	c->o_started = false;
	c->o_have = false;
	c->o_done = (c->ws == NULL);

	if (kb_used)
		free_key(c->sub, &kb);
	MemoryContextSwitchTo(old);
}

/* Fetch the base tree's next item, if none is pending. */
static void
fill_tree(ProllyCursor *c)
{
	if (c->t_have || c->t_done)
		return;
	if (!PROLLY_CALL(backend.c.tree, cursor_next, c->tc, &c->t_key, &c->t_value) ||
		!in_kind(c, &c->t_key))
	{
		c->t_done = true;
		return;
	}
	c->t_have = true;
}

/* Fetch the write set's next key, if none is pending. */
static void
fill_overlay(ProllyCursor *c)
{
	D1Entry		probe;
	D1Entry    *e;
	bool		forward = ScanDirectionIsForward(c->dir);
	ProllyKey	k;

	if (c->o_have || c->o_done)
		return;
	if (c->o_from_end && !c->o_started)
	{
		/* No bound: the first or last key. */
		RBTreeIterator it;

		rbt_begin_iterate(c->ws->entries,
						  forward ? LeftRightWalk : RightLeftWalk, &it);
		e = (D1Entry *) rbt_iterate(&it);
	}
	else
	{
		probe.key = (uint8 *) c->o_bound.data;
		probe.keylen = c->o_bound.len;
		e = (D1Entry *) (forward ?
						 rbt_find_great(c->ws->entries, &probe.rbtnode, c->o_inclusive) :
						 rbt_find_less(c->ws->entries, &probe.rbtnode, c->o_inclusive));
	}
	c->o_started = true;

	if (e == NULL)
	{
		c->o_done = true;
		return;
	}
	k.data = e->key;
	k.len = e->keylen;
	if (!in_kind(c, &k))
	{
		c->o_done = true;
		return;
	}
	c->o_entry = e;
	c->o_have = true;
	resetStringInfo(&c->o_bound);
	appendBinaryStringInfo(&c->o_bound, (const char *) e->key, e->keylen);
	c->o_inclusive = false;
	c->o_from_end = false;
}

static void
return_key(ProllyCursor *c, const uint8 *data, uint32 len, ProllyKey *key)
{
	if (is_local(c->sub))
	{
		key->data = data + 1;
		key->len = len - 1;
	}
	else
	{
		key->data = data;
		key->len = len;
	}
}

static bool
next_unordered(ProllyCursor *c, ProllyKey *key, ProllyValue *value)
{
	for (;;)
	{
		D1Entry    *e;
		const D1Version *v;

		fill_tree(c);
		if (c->t_have)
		{
			c->t_have = false;
			e = find_by_bytes(c->by_bytes, c->nby_bytes, &c->t_key);
			if (e && visible_version(e, c->cid) != NULL)
				continue;		/* the write set's version comes later */
			return_key(c, c->t_key.data, c->t_key.len, key);
			*value = c->t_value;
			return true;
		}
		if (c->by_bytes_pos >= c->nby_bytes)
			return false;
		e = c->by_bytes[c->by_bytes_pos++];
		v = visible_version(e, c->cid);
		if (v == NULL || v->is_delete)
			continue;
		{
			ProllyKey	vk = version_key(e, v);

			return_key(c, vk.data, vk.len, key);
		}
		value->data = v->value;
		value->len = v->len;
		return true;
	}
}

static bool
d1_cursor_next(ProllyCursor *c, ProllyKey *key, ProllyValue *value)
{
	bool		forward = ScanDirectionIsForward(c->dir);

	if (c->unordered)
		return next_unordered(c, key, value);

	for (;;)
	{
		int			cmp;
		const D1Version *v;

		fill_tree(c);
		fill_overlay(c);
		if (!c->t_have && !c->o_have)
			return false;

		if (!c->o_have)
			cmp = -1;
		else if (!c->t_have)
			cmp = 1;
		else
		{
			ProllyKey	ok = {c->o_entry->key, c->o_entry->keylen};

			cmp = cursor_compare(c, &c->t_key, &ok);
			if (!forward)
				cmp = -cmp;
		}

		if (cmp < 0)
		{
			/* Only in the base tree. */
			c->t_have = false;
			return_key(c, c->t_key.data, c->t_key.len, key);
			*value = c->t_value;
			return true;
		}

		/* In the write set, and maybe in the base tree too. */
		c->o_have = false;
		v = visible_version(c->o_entry, c->cid);
		if (v == NULL)
		{
			if (cmp == 0)
			{
				c->t_have = false;
				return_key(c, c->t_key.data, c->t_key.len, key);
				*value = c->t_value;
				return true;
			}
			continue;			/* written only by this command */
		}
		if (cmp == 0)
			c->t_have = false;
		if (v->is_delete)
			continue;
		{
			ProllyKey	vk = version_key(c->o_entry, v);

			return_key(c, vk.data, vk.len, key);
		}
		value->data = v->value;
		value->len = v->len;
		return true;
	}
}

static void
d1_cursor_close(ProllyCursor *c)
{
	cursor_unbind(c);
	MemoryContextDelete(c->mcxt);
}

static bool
d1_cursor_can_seek(ProllyCursor *c)
{
	return !c->unordered;
}

static bool
d1_get(ProllyRootSet *roots, Relation tree, ProllyTreeKind kind,
	   const ProllyKey *key, CommandId cid, StringInfo value)
{
	ProllySubRoot sub = subroot_for(tree, kind);
	RelFileNumber relnumber = tree->rd_locator.relNumber;
	D1WriteSet *ws;
	D1KeyBuf	kb;
	const ProllyKey *fk;
	ProllyNodeRef base;
	bool		found = false;

	backend_init();
	ws = find_writeset(sub, relnumber);
	fk = full_key(sub, kind, key, &kb);
	if (ws)
	{
		D1Entry    *e;

		refresh_order(ws, tree);
		if (!is_local(sub))
		{
			D1Order		o = ws->order;

			if (order_lost(&o))
				order_lost_error();
		}
		e = ws_find(ws, fk);
		if (e)
		{
			const D1Version *v = visible_version(e, cid);

			if (v)
			{
				if (!v->is_delete)
					appendBinaryStringInfo(value, (const char *) v->value, v->len);
				free_key(sub, &kb);
				return !v->is_delete;
			}
		}
	}

	read_base(roots, sub, relnumber, ws, &base);
	if (base.hash.len > 0)
	{
		D1Order		order;
		ProllyTreeEnv env;

		if (ws)
		{
			order = ws->order;
		}
		else
			open_order(tree, sub, roots, CurrentMemoryContext, &order);
		if (order_lost(&order))
		{
			if (!ws)
				close_order(&order);
			order_lost_error();
		}
		tree_env(&order, &env);
		found = PROLLY_CALL(backend.c.tree, get, &env, &base, fk, value);
		if (!ws)
			close_order(&order);
	}
	free_key(sub, &kb);
	return found;
}

static bool
d1_has_writes(Relation tree, ProllyTreeKind kind)
{
	backend_init();
	return find_writeset(subroot_for(tree, kind), tree->rd_locator.relNumber) != NULL;
}

static bool
d1_key_at(ProllyRootSet *roots, Relation tree, uint64 ordinal, StringInfo key)
{
	ProllySubRoot sub = subroot_for(tree, PROLLY_TREE_DATA);
	RelFileNumber relnumber = tree->rd_locator.relNumber;
	ProllyNodeRef base;

	backend_init();
	read_base(roots, sub, relnumber, find_writeset(sub, relnumber), &base);
	if (base.hash.len == 0)
		return false;
	return PROLLY_CALL(backend.c.tree, key_at, &backend.memcmp_env, &base,
					   ordinal, key);
}

static uint64
d1_count_range(ProllyRootSet *roots, Relation tree, ProllyTreeKind kind,
			   const ProllyKey *lo, const ProllyKey *hi)
{
	ProllySubRoot sub = subroot_for(tree, kind);
	RelFileNumber relnumber = tree->rd_locator.relNumber;
	D1WriteSet *ws;
	ProllyNodeRef base;
	uint64		n = 0;

	if (lo || hi || is_local(sub))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("prolly engine \"d1\" counts only whole data trees so far")));
	backend_init();
	ws = find_writeset(sub, relnumber);
	read_base(roots, sub, relnumber, ws, &base);
	if (base.hash.len > 0)
		n = PROLLY_CALL(backend.c.tree, count, &backend.memcmp_env, &base);
	if (ws)
		n += ws->nentries;		/* an estimate: some may replace or delete */
	return n;
}

/* ------------------------------------------------------------------------
 * Writes
 * ------------------------------------------------------------------------
 */

static void
d1_put(Relation tree, ProllyTreeKind kind, const ProllyKey *key,
	   const ProllyValue *value, CommandId cid)
{
	ProllySubRoot sub = subroot_for(tree, kind);
	D1WriteSet *ws = get_writeset(tree, sub, tree->rd_locator.relNumber, false);
	D1KeyBuf	kb;
	const ProllyKey *fk = full_key(sub, kind, key, &kb);

	Assert(value != NULL);
	ws_write(ws, fk, value, cid);
	free_key(sub, &kb);
}

static void
d1_remove(Relation tree, ProllyTreeKind kind, const ProllyKey *key,
		  CommandId cid)
{
	ProllySubRoot sub = subroot_for(tree, kind);
	D1WriteSet *ws = get_writeset(tree, sub, tree->rd_locator.relNumber, false);
	D1KeyBuf	kb;
	const ProllyKey *fk = full_key(sub, kind, key, &kb);

	ws_write(ws, fk, NULL, cid);
	free_key(sub, &kb);
}

/* ------------------------------------------------------------------------
 * Storage
 * ------------------------------------------------------------------------
 */

static void
d1_tree_create(Relation rel, const RelFileLocator *newrlocator, char persistence)
{
	backend_init();
	PROLLY_CALL(backend.c.node_store, create, rel, newrlocator, persistence);
	(void) get_writeset(rel, subroot_for(rel, PROLLY_TREE_DATA),
						newrlocator->relNumber, true);
	(void) get_writeset(rel, PROLLY_SUBROOT_LOCAL, newrlocator->relNumber, true);
	(void) get_writeset(rel, PROLLY_SUBROOT_LOCAL_KEYED, newrlocator->relNumber,
						true);
}

static void
d1_tree_truncate(Relation rel)
{
	backend_init();
	(void) get_writeset(rel, subroot_for(rel, PROLLY_TREE_DATA),
						rel->rd_locator.relNumber, true);
	(void) get_writeset(rel, PROLLY_SUBROOT_LOCAL, rel->rd_locator.relNumber,
						true);
	(void) get_writeset(rel, PROLLY_SUBROOT_LOCAL_KEYED,
						rel->rd_locator.relNumber, true);
}

static void
d1_tree_reset(Relation rel, ProllyTreeKind kind)
{
	ProllySubRoot sub = subroot_for(rel, kind);

	if (is_local(sub))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("prolly engine \"d1\" resets only data trees and local metadata by row")));
	backend_init();
	(void) get_writeset(rel, sub, rel->rd_locator.relNumber, true);
}

/*
 * materialize() for a write set whose keys can't be ordered any more: the
 * base tree's rows and the newest versions, matched by bytes, built into a
 * new tree in byte order.  Readers of it can't order its keys either, so
 * they read it the same way; the next write rebuilds it under the table's
 * current keying.
 */
typedef struct D1Row
{
	ProllyKey	key;
	ProllyValue value;
} D1Row;

static int
row_bytes_cmp(const void *a, const void *b)
{
	return memcmp_keys(&((const D1Row *) a)->key, &((const D1Row *) b)->key,
					   NULL);
}

static void
materialize_unordered(D1WriteSet *ws, const ProllyNodeRef *base,
					  ProllyNodeRef *out)
{
	int			nentries;
	D1Entry   **entries = entries_by_bytes(ws, &nentries);
	D1Row	   *rows;
	int			nrows = 0;
	int			cap = Max(nentries, 64);
	ProllyTreeBuilder *b;

	rows = palloc(sizeof(D1Row) * cap);

	/* The base's rows the write set doesn't replace or delete. */
	if (base->hash.len > 0)
	{
		ProllyTreeCursor *tc = PROLLY_CALL(backend.c.tree, cursor_open,
										   &backend.memcmp_env, base,
										   CurrentMemoryContext);
		ProllyKey	key;
		ProllyValue value;

		PROLLY_CALL(backend.c.tree, cursor_seek, tc, NULL, ForwardScanDirection);
		while (PROLLY_CALL(backend.c.tree, cursor_next, tc, &key, &value))
		{
			D1Entry    *e = find_by_bytes(entries, nentries, &key);
			uint8	   *copy;

			if (e && e->nversions > 0)
				continue;
			if (nrows == cap)
			{
				cap *= 2;
				rows = repalloc(rows, sizeof(D1Row) * cap);
			}
			copy = palloc(Max(key.len + value.len, 1));
			memcpy(copy, key.data, key.len);
			memcpy(copy + key.len, value.data, value.len);
			rows[nrows].key.data = copy;
			rows[nrows].key.len = key.len;
			rows[nrows].value.data = copy + key.len;
			rows[nrows].value.len = value.len;
			nrows++;
		}
		PROLLY_CALL(backend.c.tree, cursor_close, tc);
	}

	/* The write set's newest versions. */
	for (int i = 0; i < nentries; i++)
	{
		D1Entry    *e = entries[i];
		const D1Version *v;

		if (e->nversions == 0)
			continue;
		v = &e->versions[e->nversions - 1];
		if (v->is_delete)
			continue;
		if (nrows == cap)
		{
			cap *= 2;
			rows = repalloc(rows, sizeof(D1Row) * cap);
		}
		rows[nrows].key = version_key(e, v);
		rows[nrows].value.data = v->value;
		rows[nrows].value.len = v->len;
		nrows++;
	}

	qsort(rows, nrows, sizeof(D1Row), row_bytes_cmp);
	b = PROLLY_CALL(backend.c.tree, build_begin, &backend.memcmp_env,
					CurrentMemoryContext);
	for (int i = 0; i < nrows; i++)
		PROLLY_CALL(backend.c.tree, build_add, b, &rows[i].key, &rows[i].value);
	PROLLY_CALL(backend.c.tree, build_finish, b, out);
	pfree(rows);
	pfree(entries);
}

/*
 * Write ws's newest versions into the tree from (its base, or a newer
 * version of the tree to replay them onto); returns its root in *out.
 */
static void
materialize_onto(D1WriteSet *ws, const ProllyNodeRef *from, ProllyNodeRef *out)
{
	ProllyEdit *edits;
	int			n = 0;
	RBTreeIterator it;
	D1Entry    *e;
	ProllyNodeRef base = *from;
	D1Order		order = ws->order;
	ProllyTreeEnv env;

	if (order_lost(&order))
	{
		materialize_unordered(ws, from, out);
		return;
	}
	tree_env(&order, &env);
	if (base.hash.len == 0)
	{
		ProllyTreeBuilder *b = PROLLY_CALL(backend.c.tree, build_begin, &env,
										   CurrentMemoryContext);

		PROLLY_CALL(backend.c.tree, build_finish, b, &base);
	}

	edits = palloc(sizeof(ProllyEdit) * Max(ws->nentries, 1));
	rbt_begin_iterate(ws->entries, LeftRightWalk, &it);
	while ((e = (D1Entry *) rbt_iterate(&it)) != NULL)
	{
		const D1Version *v;

		if (e->nversions == 0)
			continue;
		v = &e->versions[e->nversions - 1];
		edits[n].key = version_key(e, v);
		edits[n].is_delete = v->is_delete;
		edits[n].value.data = v->value;
		edits[n].value.len = v->len;
		n++;
	}
	if (n > 0)
		PROLLY_CALL(backend.c.tree, apply, &env, &base, edits, n, out);
	else
		*out = base;
	pfree(edits);
}

static void
materialize(D1WriteSet *ws, ProllyNodeRef *out)
{
	materialize_onto(ws, &ws->base, out);
}

static void
d1_tree_copy(Relation rel, const RelFileLocator *newrlocator)
{
	ProllySubRoot subs[3] = {subroot_for(rel, PROLLY_TREE_DATA), PROLLY_SUBROOT_LOCAL,
	PROLLY_SUBROOT_LOCAL_KEYED};

	backend_init();
	for (int i = 0; i < lengthof(subs); i++)
	{
		D1WriteSet *from = get_writeset(rel, subs[i], rel->rd_locator.relNumber,
										false);
		D1WriteSet *to;
		ProllyNodeRef tree;

		materialize(from, &tree);
		to = get_writeset(rel, subs[i], newrlocator->relNumber, true);
		to->base = tree;
	}
}

static void
d1_vacuum(Relation rel, const struct VacuumParams *params,
		  BufferAccessStrategy bstrategy)
{
	/* Nothing to do until reclamation frees unreachable nodes. */
}

/* ------------------------------------------------------------------------
 * Transaction end
 * ------------------------------------------------------------------------
 */

static int
ws_order(const void *a, const void *b)
{
	const D1WriteSet *wa = *(D1WriteSet *const *) a;
	const D1WriteSet *wb = *(D1WriteSet *const *) b;

	if (wa->sub != wb->sub)
		return wa->sub < wb->sub ? -1 : 1;
	if (wa->relnumber != wb->relnumber)
		return wa->relnumber < wb->relnumber ? -1 : 1;
	return 0;
}

static bool
same_hash(const ProllyNodeRef *a, const ProllyNodeRef *b)
{
	return a->hash.len == b->hash.len &&
		memcmp(a->hash.bytes, b->hash.bytes, a->hash.len) == 0;
}

static void
d1_xact_precommit(void)
{
	const ProllyCommitRoutine *commit;
	ProllyRootSet tip;
	D1WriteSet **sets;
	ProllyRootEdit *edits;
	int			n = 0;
	ProllyNodeRef new_root;

	if (xact == NULL || xact->all == NULL)
		return;
	commit = backend.c.commit;

	for (D1WriteSet *ws = xact->all; ws; ws = ws->next)
		n++;
	sets = palloc(sizeof(D1WriteSet *) * n);
	n = 0;
	for (D1WriteSet *ws = xact->all; ws; ws = ws->next)
		sets[n++] = ws;
	qsort(sets, n, sizeof(D1WriteSet *), ws_order);

	/* The new trees, built before the branch lock: the costly part. */
	edits = palloc(sizeof(ProllyRootEdit) * n);
	for (int i = 0; i < n; i++)
	{
		edits[i].sub = sets[i]->sub;
		edits[i].relnumber = sets[i]->relnumber;
		edits[i].is_remove = false;
		materialize(sets[i], &edits[i].tree);
	}

	PROLLY_CALL(commit, branch_lock, PROLLY_MAIN_BRANCH);
	tip.has_root = PROLLY_CALL(commit, tip, PROLLY_MAIN_BRANCH, &tip.root);
	if (!tip.has_root)
		PROLLY_CALL(backend.c.root_layout, empty, &backend.root_env, &tip.root);
	tip.has_root = true;

	/*
	 * Under it, replay the edits onto trees others changed since: their
	 * changes are to keys this transaction didn't write.
	 */
	for (int i = 0; i < n; i++)
	{
		D1WriteSet *ws = sets[i];
		ProllyNodeRef current;

		if (ws->replaced)
			continue;
		find_tree(&tip, ws->sub, ws->relnumber, &current);
		if (!same_hash(&current, &ws->base))
			materialize_onto(ws, &current, &edits[i].tree);
	}

	PROLLY_CALL(backend.c.root_layout, apply, &backend.root_env, &tip.root,
				edits, n, &new_root);
	xact->ticket = PROLLY_CALL(commit, enqueue, PROLLY_MAIN_BRANCH, &new_root);
	xact->enqueued = true;
	PROLLY_CALL(commit, branch_unlock, PROLLY_MAIN_BRANCH);
	pfree(sets);
	pfree(edits);
}

static void
d1_xact_commit(void)
{
	if (xact != NULL && xact->enqueued)
		PROLLY_CALL(backend.c.commit, publish, xact->ticket);
	xact_reset();
}

static void
d1_xact_abort(void)
{
	if (xact != NULL && xact->enqueued)
		PROLLY_CALL(backend.c.commit, abandon, xact->ticket);
	xact_reset();
}

static void
d1_xact_prepare(void)
{
	if (xact != NULL && xact->all != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot PREPARE a transaction that has written to a prolly table")));

	/*
	 * Writes made and rolled back in subtransactions leave state but
	 * nothing to carry into the prepared transaction.  Its memory goes
	 * with the transaction's, and no commit or abort event follows here.
	 */
	xact_reset();
}

/* Forget a write set made in an aborted subtransaction. */
static void
drop_writeset(D1WriteSet *ws)
{
	D1WsKey		key;
	D1WriteSet **link;

	memset(&key, 0, sizeof(key));
	key.sub = ws->sub;
	key.relnumber = ws->relnumber;
	(void) hash_search(xact->writesets, &key, HASH_REMOVE, NULL);
	for (link = &xact->all; *link != ws; link = &(*link)->next)
		;
	*link = ws->next;
}

static void
d1_subxact_abort(SubTransactionId subxid)
{
	D1WriteSet *next;

	if (xact == NULL)
		return;
	for (D1WriteSet *ws = xact->all; ws; ws = next)
	{
		RBTreeIterator it;
		D1Entry    *e;
		ProllyKey  *empty;
		int			nempty = 0;

		next = ws->next;

		/*
		 * Made by the subtransaction or one of its children: everything in
		 * it is theirs, and the relation lock it relied on is released with
		 * them.
		 */
		if (ws->created_subxid >= subxid)
		{
			drop_writeset(ws);
			continue;
		}

		/* Undo emptyings done by the subtransaction, newest first. */
		while (ws->saved && ws->saved->subxid >= subxid)
		{
			D1Saved    *saved = ws->saved;

			ws->entries = saved->entries;
			ws->nentries = saved->nentries;
			ws->base = saved->base;
			ws->replaced = saved->replaced;
			ws->order = saved->order;
			ws->saved = saved->prev;
		}

		/* Keys left with no versions, deleted after the walk. */
		empty = palloc(sizeof(ProllyKey) * Max(ws->nentries, 1));
		rbt_begin_iterate(ws->entries, LeftRightWalk, &it);
		while ((e = (D1Entry *) rbt_iterate(&it)) != NULL)
		{
			while (e->nversions > 0 &&
				   e->versions[e->nversions - 1].subxid >= subxid)
			{
				D1Version  *v = &e->versions[--e->nversions];

				if (v->value)
					pfree(v->value);
				if (v->key)
					pfree(v->key);
			}
			if (e->nversions == 0)
			{
				empty[nempty].data = e->key;
				empty[nempty].len = e->keylen;
				nempty++;
			}
		}

		/*
		 * Find each again: deleting a node can move another's contents, so
		 * pointers from the walk may be stale.
		 */
		for (int i = 0; i < nempty; i++)
		{
			uint8	   *key = (uint8 *) empty[i].data;

			e = ws_find(ws, &empty[i]);
			Assert(e != NULL && e->key == key);
			if (e->versions)
				pfree(e->versions);
			rbt_delete(ws->entries, &e->rbtnode);
			pfree(key);
			ws->nentries--;
		}
		pfree(empty);
	}
}

static void
d1_node_counters(ProllyNodeCounters *counters)
{
	*counters = backend.counters;
}

const ProllyEngineRoutine prolly_engine_d1 = {
	.info = {
		.iface = PROLLY_IFACE_ENGINE,
		.impl_id = 1,
		.format_version = 1,
		.name = "d1",
		.description = "D1: immutable copy-on-write: hash-addressed nodes, full-chunk WAL",
	},
	.tree_create = d1_tree_create,
	.tree_truncate = d1_tree_truncate,
	.tree_reset = d1_tree_reset,
	.tree_copy = d1_tree_copy,
	.roots_acquire = d1_roots_acquire,
	.roots_release = d1_roots_release,
	.cursor_open = d1_cursor_open,
	.cursor_seek = d1_cursor_seek,
	.cursor_next = d1_cursor_next,
	.cursor_close = d1_cursor_close,
	.cursor_can_seek = d1_cursor_can_seek,
	.get = d1_get,
	.has_writes = d1_has_writes,
	.key_at = d1_key_at,
	.count_range = d1_count_range,
	.put = d1_put,
	.remove = d1_remove,
	.subxact_abort = d1_subxact_abort,
	.xact_abort = d1_xact_abort,
	.xact_precommit = d1_xact_precommit,
	.xact_commit = d1_xact_commit,
	.xact_prepare = d1_xact_prepare,
	.vacuum = d1_vacuum,
	.node_counters = d1_node_counters,
};
