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
 * descendants.
 *
 * Spilling.  Past prolly_write_set_memory, a transaction's write sets are
 * spilled: each moves its keys into a spill tree in the node store (keyed
 * as the write set is, each value a key's newest version and, if it
 * differs, the newest one the current command may see, with their command
 * ids), and starts over empty in memory.  Reads look at memory, then the
 * spill tree, then the base.  A command that began before the spill (an
 * open cursor) can't be served, since versions it mustn't see are gone
 * from memory: reading for it is an error.  For each subtransaction open
 * at a spill, a mark keeps the spill tree as it was without that
 * subtransaction's versions, which rolling back to it restores.
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
#include "access/relation.h"
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

/* The spill tree as it was without a subtransaction's versions. */
typedef struct D1SpillMark
{
	SubTransactionId subxid;
	ProllyNodeRef root;			/* hash.len == 0: no spill tree */
	struct D1SpillMark *next;	/* newer subtransactions first */
} D1SpillMark;

/* A write set's spilled state. */
typedef struct D1Spill
{
	ProllyNodeRef root;			/* hash.len == 0: none */
	CommandId	cid;			/* the command current at the last spill */
	D1SpillMark *marks;
} D1Spill;

/* A write set's contents before it was emptied, for subtransaction abort. */
typedef struct D1Saved
{
	SubTransactionId subxid;	/* the subtransaction that emptied it */
	RBTree	   *entries;
	int			nentries;
	MemoryContext data_mcxt;
	ProllyNodeRef base;
	bool		replaced;
	D1Order		order;
	D1Spill		spill;
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
	Oid			relid;			/* the relation, for its key order */
	SubTransactionId created_subxid;

	ProllyNodeRef base;			/* hash.len == 0: empty */
	bool		replaced;		/* new storage: base ignores the head */

	RBTree	   *entries;
	int			nentries;
	MemoryContext data_mcxt;	/* entries and their versions */
	D1Spill		spill;
	uint64		spill_gen;		/* changes when spill.root does */
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
	ProllyBranchId branch;		/* the branch its writes go to */
} D1Xact;

static D1Xact *xact = NULL;

static int	entry_compare(const RBTNode *a, const RBTNode *b, void *arg);
static void entry_combine(RBTNode *existing, const RBTNode *newdata, void *arg);
static RBTNode *entry_alloc(void *arg);
static void entry_free(RBTNode *node, void *arg);
static void spill_clear(D1Spill *sp);
static bool same_hash(const ProllyNodeRef *a, const ProllyNodeRef *b);
static void maybe_spill(void);

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

/* The transaction's branch once it writes; until then the session's. */
static ProllyBranchId
current_branch(void)
{
	return xact ? xact->branch : prolly_session_branch();
}

static void
current_head(ProllyRootSet *roots)
{
	roots->has_root = PROLLY_CALL(backend.c.commit, head, current_branch(),
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
	xact->branch = prolly_session_branch();
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

/*
 * Keep the key order of a data tree's write set current while it's empty
 * (a spill tree, ordered too, counts as contents).
 */
static void
refresh_order(D1WriteSet *ws, Relation rel)
{
	D1Order		order;

	if (is_local(ws->sub) || ws->nentries > 0 || ws->spill.root.hash.len > 0)
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
			saved->data_mcxt = ws->data_mcxt;
			saved->base = ws->base;
			saved->replaced = ws->replaced;
			saved->order = ws->order;
			saved->spill = ws->spill;
			saved->prev = ws->saved;
			ws->saved = saved;
			ws->data_mcxt = AllocSetContextCreate(xact->mcxt,
												  "prolly d1 write set",
												  ALLOCSET_DEFAULT_SIZES);
			MemoryContextSwitchTo(ws->data_mcxt);
			ws->entries = rbt_create(sizeof(D1Entry), entry_compare,
									 entry_combine, entry_alloc, entry_free, ws);
			MemoryContextSwitchTo(old);
			ws->nentries = 0;
			spill_clear(&ws->spill);
			ws->spill_gen++;
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
	ws->relid = RelationGetRelid(rel);
	ws->data_mcxt = AllocSetContextCreate(xact->mcxt, "prolly d1 write set",
										  ALLOCSET_DEFAULT_SIZES);
	spill_clear(&ws->spill);
	MemoryContextSwitchTo(ws->data_mcxt);
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
	return MemoryContextAlloc(((D1WriteSet *) arg)->data_mcxt, sizeof(D1Entry));
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
	MemoryContext old = MemoryContextSwitchTo(ws->data_mcxt);
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
	maybe_spill();
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
 * Spilling
 * ------------------------------------------------------------------------
 */

#define SPILL_DELETE	0x01	/* the version deletes the key */
#define SPILL_OWN_KEY	0x02	/* the version wrote the key with other bytes */

/* Keys a spill or a commit applies at once. */
#define EDIT_CHUNK		8192

/*
 * A spilled version, pointing into its encoding.  A spill tree's value is
 * a count (1 or 2), then each version, oldest first: command id (4 bytes),
 * flags, the key's bytes if the version wrote it with other bytes than
 * the tree's key (length, 4 bytes, then the bytes), and the value (length,
 * then the bytes).  Native byte order: spill trees are local and live only
 * as long as their transaction.
 */
typedef struct D1SpillVersion
{
	CommandId	cid;
	bool		is_delete;
	ProllyKey	key;
	ProllyValue value;
} D1SpillVersion;

static void
spill_clear(D1Spill *sp)
{
	memset(sp, 0, sizeof(*sp));
	sp->root.location.blkno = InvalidBlockNumber;
	sp->cid = InvalidCommandId;
}

static int
spill_decode(const ProllyKey *key, const ProllyValue *value, D1SpillVersion *out)
{
	const uint8 *p = value->data;
	const uint8 *end = p + value->len;
	int			n;

	if (value->len < 1)
		elog(ERROR, "prolly spilled value is empty");
	n = *p++;
	if (n < 1 || n > 2)
		elog(ERROR, "prolly spilled value has %d versions", n);
	for (int i = 0; i < n; i++)
	{
		uint8		flags;
		uint32		len;

		if (end - p < 5)
			elog(ERROR, "prolly spilled value is corrupt");
		memcpy(&out[i].cid, p, 4);
		p += 4;
		flags = *p++;
		out[i].is_delete = (flags & SPILL_DELETE) != 0;
		out[i].key = *key;
		if (flags & SPILL_OWN_KEY)
		{
			if (end - p < 4)
				elog(ERROR, "prolly spilled value is corrupt");
			memcpy(&len, p, 4);
			p += 4;
			if (end - p < len)
				elog(ERROR, "prolly spilled value is corrupt");
			out[i].key.data = p;
			out[i].key.len = len;
			p += len;
		}
		if (end - p < 4)
			elog(ERROR, "prolly spilled value is corrupt");
		memcpy(&len, p, 4);
		p += 4;
		if (end - p < len)
			elog(ERROR, "prolly spilled value is corrupt");
		out[i].value.data = p;
		out[i].value.len = len;
		p += len;
	}
	if (p != end)
		elog(ERROR, "prolly spilled value is corrupt");
	return n;
}

static void
spill_encode(const ProllyKey *key, const D1SpillVersion *v, int n, StringInfo out)
{
	appendStringInfoChar(out, (char) n);
	for (int i = 0; i < n; i++)
	{
		uint8		flags = v[i].is_delete ? SPILL_DELETE : 0;
		bool		own = v[i].key.len != key->len ||
			memcmp(v[i].key.data, key->data, key->len) != 0;
		uint32		len;

		if (own)
			flags |= SPILL_OWN_KEY;
		appendBinaryStringInfo(out, &v[i].cid, 4);
		appendStringInfoChar(out, (char) flags);
		if (own)
		{
			len = v[i].key.len;
			appendBinaryStringInfo(out, &len, 4);
			appendBinaryStringInfo(out, v[i].key.data, len);
		}
		len = v[i].is_delete ? 0 : v[i].value.len;
		appendBinaryStringInfo(out, &len, 4);
		if (len > 0)
			appendBinaryStringInfo(out, v[i].value.data, len);
	}
}

/* The newest spilled version command cid may see, as visible_version(). */
static const D1SpillVersion *
spill_visible(const D1SpillVersion *v, int n, CommandId cid)
{
	for (int i = n - 1; i >= 0; i--)
	{
		if (cid == InvalidCommandId || v[i].cid < cid ||
			v[i].cid == InvalidCommandId)
			return &v[i];
	}
	return NULL;
}

/*
 * A command that began before the last spill may need versions the spill
 * kept only the newest of.
 */
static void
spill_check_reader(const D1WriteSet *ws, CommandId cid)
{
	if (ws->spill.root.hash.len > 0 && cid != InvalidCommandId &&
		ws->spill.cid != InvalidCommandId && cid < ws->spill.cid)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot read a prolly table as of an earlier command once the transaction's writes were spilled"),
				 errhint("Raise \"prolly_write_set_memory\", or close cursors before writing much.")));
}

/*
 * Of a key's versions, oldest first, keep those commands from cid on can
 * need: the newest, and the newest command cid may see if that's older.
 */
static int
spill_prune(D1SpillVersion *v, int n, CommandId cid)
{
	const D1SpillVersion *seen;

	if (n == 0)
		return 0;
	seen = spill_visible(v, n - 1, cid);
	if (seen == NULL)
	{
		v[0] = v[n - 1];
		return 1;
	}
	v[0] = *seen;
	v[1] = v[n - 1];
	return 2;
}

static int
edit_cmp(const ProllyKey *a, const ProllyKey *b, const ProllyTreeEnv *env)
{
	return env->compare(a, b, env->compare_arg);
}

/*
 * The spill tree from with ws's versions of subtransactions before below
 * (InvalidSubTransactionId: all of them) added, as of command cid.
 */
static void
spill_into(D1WriteSet *ws, const ProllyTreeEnv *env, const ProllyNodeRef *from,
		   CommandId cid, SubTransactionId below, ProllyNodeRef *out)
{
	ProllyEdit *edits = palloc(sizeof(ProllyEdit) * Max(ws->nentries, 1));
	int			n = 0;
	RBTreeIterator it;
	D1Entry    *e;
	ProllyNodeRef base = *from;

	if (base.hash.len == 0)
	{
		ProllyTreeBuilder *b = PROLLY_CALL(backend.c.tree, build_begin, env,
										   CurrentMemoryContext);

		PROLLY_CALL(backend.c.tree, build_finish, b, &base);
	}
	rbt_begin_iterate(ws->entries, LeftRightWalk, &it);
	while ((e = (D1Entry *) rbt_iterate(&it)) != NULL)
	{
		D1SpillVersion *v = palloc(sizeof(D1SpillVersion) * (e->nversions + 2));
		int			nv = 0;
		ProllyKey	k = {e->key, e->keylen};
		StringInfoData old;
		StringInfoData enc;
		bool		mine = false;

		for (int i = 0; i < e->nversions; i++)
			mine |= (below == InvalidSubTransactionId ||
					 e->versions[i].subxid < below);
		if (!mine)
			continue;
		initStringInfo(&old);
		if (from->hash.len > 0 &&
			PROLLY_CALL(backend.c.tree, get, env, from, &k, &old))
		{
			ProllyValue ov = {(const uint8 *) old.data, old.len};

			nv = spill_decode(&k, &ov, v);
		}
		for (int i = 0; i < e->nversions; i++)
		{
			const D1Version *ver = &e->versions[i];

			if (below != InvalidSubTransactionId && ver->subxid >= below)
				continue;
			v[nv].cid = ver->cid;
			v[nv].is_delete = ver->is_delete;
			v[nv].key = version_key(e, ver);
			v[nv].value.data = ver->value;
			v[nv].value.len = ver->len;
			nv++;
		}
		nv = spill_prune(v, nv, cid);
		initStringInfo(&enc);
		spill_encode(&k, v, nv, &enc);
		edits[n].key = k;
		edits[n].is_delete = false;
		edits[n].value.data = (const uint8 *) enc.data;
		edits[n].value.len = enc.len;
		n++;
	}
	if (n > 0)
		PROLLY_CALL(backend.c.tree, apply, env, &base, edits, n, out);
	else
		*out = base;
}

static D1SpillMark *
find_mark(D1SpillMark *marks, SubTransactionId subxid)
{
	for (; marks; marks = marks->next)
	{
		if (marks->subxid == subxid)
			return marks;
	}
	return NULL;
}

/*
 * Move ws's versions into its spill tree, and into the mark of each open
 * subtransaction, then empty it in memory.
 */
static void
spill_writeset(D1WriteSet *ws, CommandId cid, const SubTransactionId *open,
			   int nopen)
{
	D1Order		order = ws->order;
	ProllyTreeEnv env;
	ProllyNodeRef old_root = ws->spill.root;
	ProllyNodeRef new_root;
	D1SpillMark *marks = NULL;
	MemoryContext tmp;
	MemoryContext old;

	if (ws->nentries == 0 || order_lost(&order))
		return;
	tree_env(&order, &env);
	tmp = AllocSetContextCreate(CurrentMemoryContext, "prolly d1 spill",
								ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(tmp);
	spill_into(ws, &env, &old_root, cid, InvalidSubTransactionId, &new_root);

	/*
	 * Open subtransactions, innermost first; each mark is the spill tree
	 * without that subtransaction's versions (nor its children's).  One new
	 * since the last spill starts from that spill's tree, all of whose
	 * versions were written before it began.
	 */
	for (int i = nopen - 1; i >= 0; i--)
	{
		D1SpillMark *had = find_mark(ws->spill.marks, open[i]);
		D1SpillMark *m;

		m = MemoryContextAlloc(xact->mcxt, sizeof(D1SpillMark));
		m->subxid = open[i];
		spill_into(ws, &env, had ? &had->root : &old_root, cid, open[i],
				   &m->root);
		m->next = marks;
		marks = m;
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(tmp);

	while (ws->spill.marks)
	{
		D1SpillMark *next = ws->spill.marks->next;

		pfree(ws->spill.marks);
		ws->spill.marks = next;
	}
	ws->spill.root = new_root;
	ws->spill.cid = cid;
	ws->spill.marks = marks;
	ws->spill_gen++;

	/* Empty in memory. */
	MemoryContextReset(ws->data_mcxt);
	old = MemoryContextSwitchTo(ws->data_mcxt);
	ws->entries = rbt_create(sizeof(D1Entry), entry_compare, entry_combine,
							 entry_alloc, entry_free, ws);
	MemoryContextSwitchTo(old);
	ws->nentries = 0;
}

/* Spill every write set if the transaction's writes use too much memory. */
static void
maybe_spill(void)
{
	Size		used = 0;
	SubTransactionId open_buf[64];
	SubTransactionId *open = open_buf;
	int			nopen;
	CommandId	cid;

	for (D1WriteSet *ws = xact->all; ws; ws = ws->next)
		used += MemoryContextMemAllocated(ws->data_mcxt, true);
	if (used <= (Size) prolly_write_set_memory * 1024)
		return;

	nopen = GetOpenSubTransactionIds(open_buf, lengthof(open_buf));
	if (nopen > lengthof(open_buf))
	{
		open = palloc(sizeof(SubTransactionId) * nopen);
		(void) GetOpenSubTransactionIds(open, nopen);
	}
	cid = GetCurrentCommandId(false);
	for (D1WriteSet *ws = xact->all; ws; ws = ws->next)
		spill_writeset(ws, cid, open, nopen);
	if (open != open_buf)
		pfree(open);
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
									  current_branch(), snapshot,
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

	/* the write set's spill tree */
	ProllyTreeCursor *sc;		/* NULL: none */
	uint64		spill_gen;		/* ws->spill_gen when sc was opened */
	bool		s_have;
	bool		s_done;
	bool		s_skip_last;	/* skip keys equal to last */
	ProllyKey	s_key;
	ProllyValue s_value;

	/* the write set, by the last key taken from it */
	bool		o_started;
	bool		o_have;
	bool		o_done;
	D1Entry    *o_entry;
	StringInfoData o_bound;		/* seek key, or last key taken */
	bool		o_inclusive;
	bool		o_from_end;		/* no bound: start at the first/last key */

	/* where the scan is: the seek, and the last full key returned */
	StringInfoData seek;
	bool		seek_from_end;
	StringInfoData last;
	bool		have_last;

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
	c->sc = NULL;
	if (c->ws && c->ws->spill.root.hash.len > 0)
		c->sc = PROLLY_CALL(backend.c.tree, cursor_open, &c->env,
							&c->ws->spill.root, c->mcxt);
	c->spill_gen = c->ws ? c->ws->spill_gen : 0;
	/* Unpositioned until a seek; an empty base stays done. */
	c->t_have = c->o_have = c->s_have = false;
	c->t_done = c->o_done = c->s_done = true;
	c->s_skip_last = false;
}

static void
cursor_unbind(ProllyCursor *c)
{
	if (c->tc)
		PROLLY_CALL(backend.c.tree, cursor_close, c->tc);
	c->tc = NULL;
	if (c->sc)
		PROLLY_CALL(backend.c.tree, cursor_close, c->sc);
	c->sc = NULL;
	if (c->ws == NULL)
		close_order(&c->order);
}

/*
 * The write set was spilled (or a spill undone) since the cursor looked:
 * read the new spill tree, and the emptied write set, from where the scan
 * is.  Keys already returned are not returned again.
 */
static void
cursor_respill(ProllyCursor *c)
{
	MemoryContext old = MemoryContextSwitchTo(c->mcxt);

	spill_check_reader(c->ws, c->cid);
	if (c->sc)
		PROLLY_CALL(backend.c.tree, cursor_close, c->sc);
	c->sc = NULL;
	if (c->ws->spill.root.hash.len > 0)
		c->sc = PROLLY_CALL(backend.c.tree, cursor_open, &c->env,
							&c->ws->spill.root, c->mcxt);
	c->spill_gen = c->ws->spill_gen;
	c->s_have = false;
	c->s_done = (c->sc == NULL);
	c->o_have = false;
	c->o_done = false;
	resetStringInfo(&c->o_bound);
	if (c->have_last)
	{
		ProllyKey	last = {(const uint8 *) c->last.data, c->last.len};

		if (c->sc)
			PROLLY_CALL(backend.c.tree, cursor_seek, c->sc, &last, c->dir);
		c->s_skip_last = true;
		appendBinaryStringInfo(&c->o_bound, c->last.data, c->last.len);
		c->o_inclusive = false;
		c->o_from_end = false;
		c->o_started = true;
	}
	else
	{
		ProllyKey	seek = {(const uint8 *) c->seek.data, c->seek.len};

		if (c->sc)
			PROLLY_CALL(backend.c.tree, cursor_seek, c->sc,
						c->seek_from_end ? NULL : &seek, c->dir);
		c->s_skip_last = false;
		appendBinaryStringInfo(&c->o_bound, c->seek.data, c->seek.len);
		c->o_inclusive = true;
		c->o_from_end = c->seek_from_end;
		c->o_started = false;
	}
	MemoryContextSwitchTo(old);
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
	initStringInfo(&c->seek);
	initStringInfo(&c->last);
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

	if (c->ws)
		spill_check_reader(c->ws, c->cid);
	if (c->unordered)
	{
		/* Only a forward scan of everything needs no order. */
		if (key || !forward || c->sc)
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
	if (c->sc)
	{
		PROLLY_CALL(backend.c.tree, cursor_seek, c->sc, fk, c->dir);
		c->s_done = false;
	}
	c->s_have = false;
	c->s_skip_last = false;
	resetStringInfo(&c->seek);
	c->seek_from_end = (fk == NULL);
	if (fk)
		appendBinaryStringInfo(&c->seek, (const char *) fk->data, fk->len);
	c->have_last = false;

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

/* Fetch the spill tree's next item, if none is pending. */
static void
fill_spill(ProllyCursor *c)
{
	while (!c->s_have && !c->s_done)
	{
		if (!PROLLY_CALL(backend.c.tree, cursor_next, c->sc, &c->s_key,
						 &c->s_value) ||
			!in_kind(c, &c->s_key))
		{
			c->s_done = true;
			return;
		}
		if (c->s_skip_last)
		{
			ProllyKey	last = {(const uint8 *) c->last.data, c->last.len};

			if (cursor_compare(c, &c->s_key, &last) == 0)
				continue;
			c->s_skip_last = false;
		}
		c->s_have = true;
	}
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

/* Return full key fk and value, remembering where the scan is. */
static bool
emit(ProllyCursor *c, ProllyKey fk, const uint8 *data, uint32 len,
	 ProllyKey *key, ProllyValue *value)
{
	MemoryContext old = MemoryContextSwitchTo(c->mcxt);

	resetStringInfo(&c->last);
	appendBinaryStringInfo(&c->last, (const char *) fk.data, fk.len);
	c->have_last = true;
	MemoryContextSwitchTo(old);
	return_key(c, fk.data, fk.len, key);
	value->data = data;
	value->len = len;
	return true;
}

/* Compare in the scan's direction. */
static int
dir_compare(ProllyCursor *c, const ProllyKey *a, const ProllyKey *b)
{
	int			r = cursor_compare(c, a, b);

	return ScanDirectionIsForward(c->dir) ? r : -r;
}

/*
 * The next key of the merge of the write set (newest), its spill tree,
 * then the base tree, each key's newest version the command may see.
 */
static bool
d1_cursor_next(ProllyCursor *c, ProllyKey *key, ProllyValue *value)
{
	if (c->unordered)
		return next_unordered(c, key, value);
	if (c->ws && c->ws->spill_gen != c->spill_gen)
		cursor_respill(c);

	for (;;)
	{
		ProllyKey	ok = {NULL, 0};
		ProllyKey	mink;
		const ProllyKey *min = NULL;
		bool		at_t;
		bool		at_s;
		bool		at_o;

		fill_tree(c);
		if (c->sc)
			fill_spill(c);
		fill_overlay(c);
		if (!c->t_have && !c->s_have && !c->o_have)
			return false;

		if (c->o_have)
		{
			ok.data = c->o_entry->key;
			ok.len = c->o_entry->keylen;
		}
		if (c->t_have)
			min = &c->t_key;
		if (c->s_have && (min == NULL || dir_compare(c, &c->s_key, min) < 0))
			min = &c->s_key;
		if (c->o_have && (min == NULL || dir_compare(c, &ok, min) < 0))
			min = &ok;
		mink = *min;
		at_t = c->t_have && dir_compare(c, &c->t_key, &mink) == 0;
		at_s = c->s_have && dir_compare(c, &c->s_key, &mink) == 0;
		at_o = c->o_have && dir_compare(c, &ok, &mink) == 0;

		if (at_o)
		{
			const D1Version *v = visible_version(c->o_entry, c->cid);

			c->o_have = false;
			if (v)
			{
				c->s_have &= !at_s;
				c->t_have &= !at_t;
				if (v->is_delete)
					continue;
				return emit(c, version_key(c->o_entry, v), v->value, v->len,
							key, value);
			}
			/* Written only by this command: what it sees comes next. */
		}
		if (at_s)
		{
			D1SpillVersion sv[2];
			int			n = spill_decode(&c->s_key, &c->s_value, sv);
			const D1SpillVersion *v = spill_visible(sv, n, c->cid);

			c->s_have = false;
			if (v)
			{
				c->t_have &= !at_t;
				if (v->is_delete)
					continue;
				return emit(c, v->key, v->value.data, v->value.len, key, value);
			}
		}
		if (at_t)
		{
			c->t_have = false;
			return emit(c, c->t_key, c->t_value.data, c->t_value.len, key,
						value);
		}
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

/*
 * The commits since the snapshot's that changed tree's data (or index)
 * tree: consecutive roots whose trees for it differ.
 */
static int
d1_unseen_writers(Relation tree, Snapshot snapshot, TransactionId **xids)
{
	ProllyBranchCommit *commits;
	int			n;
	int			nx = 0;
	ProllySubRoot sub = subroot_for(tree, PROLLY_TREE_DATA);
	RelFileNumber relnumber = tree->rd_locator.relNumber;
	ProllyNodeRef prev;

	backend_init();
	n = PROLLY_CALL(backend.c.commit, commits_since, current_branch(),
					snapshot, &commits);
	*xids = palloc(sizeof(TransactionId) * Max(n, 1));
	for (int i = 0; i < n; i++)
	{
		ProllyRootSet rs;
		ProllyNodeRef t;

		rs.has_root = commits[i].has_root;
		rs.root = commits[i].root;
		find_tree(&rs, sub, relnumber, &t);
		if (i > 0 && !same_hash(&prev, &t))
			(*xids)[nx++] = commits[i].xid;
		prev = t;
	}
	pfree(commits);
	return nx;
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
		if (ws->spill.root.hash.len > 0)
		{
			D1Order		o = ws->order;
			ProllyTreeEnv env;
			StringInfoData sv;
			bool		decided = false;

			spill_check_reader(ws, cid);
			tree_env(&o, &env);
			initStringInfo(&sv);
			if (PROLLY_CALL(backend.c.tree, get, &env, &ws->spill.root, fk, &sv))
			{
				ProllyValue enc = {(const uint8 *) sv.data, sv.len};
				D1SpillVersion v[2];
				int			n = spill_decode(fk, &enc, v);
				const D1SpillVersion *seen = spill_visible(v, n, cid);

				if (seen)
				{
					decided = true;
					found = !seen->is_delete;
					if (found)
						appendBinaryStringInfo(value,
											   (const char *) seen->value.data,
											   seen->value.len);
				}
			}
			pfree(sv.data);
			if (decided)
			{
				free_key(sub, &kb);
				return found;
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
 * materialize_onto() for a write set with a spill tree: the spill tree's
 * and memory's newest versions, merged in key order (memory's win), applied
 * a chunk at a time, so memory stays bounded however much was spilled.
 */
static void
materialize_spilled(D1WriteSet *ws, const ProllyTreeEnv *env,
					const ProllyNodeRef *from, ProllyNodeRef *out)
{
	MemoryContext chunk = AllocSetContextCreate(CurrentMemoryContext,
												"prolly d1 commit chunk",
												ALLOCSET_DEFAULT_SIZES);
	MemoryContext old;
	ProllyEdit *edits = palloc(sizeof(ProllyEdit) * EDIT_CHUNK);
	int			n = 0;
	ProllyTreeCursor *sc = PROLLY_CALL(backend.c.tree, cursor_open, env,
									   &ws->spill.root, CurrentMemoryContext);
	RBTreeIterator it;
	D1Entry    *e;
	ProllyKey	sk;
	ProllyValue sv;
	bool		s_have;
	ProllyNodeRef root = *from;

	PROLLY_CALL(backend.c.tree, cursor_seek, sc, NULL, ForwardScanDirection);
	s_have = PROLLY_CALL(backend.c.tree, cursor_next, sc, &sk, &sv);
	rbt_begin_iterate(ws->entries, LeftRightWalk, &it);
	e = (D1Entry *) rbt_iterate(&it);
	while (s_have || e)
	{
		int			cmp;
		ProllyEdit *ed = &edits[n];

		while (e && e->nversions == 0)
			e = (D1Entry *) rbt_iterate(&it);
		if (!s_have && !e)
			break;
		if (!e)
			cmp = -1;
		else if (!s_have)
			cmp = 1;
		else
		{
			ProllyKey	ek = {e->key, e->keylen};

			cmp = edit_cmp(&sk, &ek, env);
		}

		old = MemoryContextSwitchTo(chunk);
		if (cmp >= 0)
		{
			/* Memory's newest version. */
			const D1Version *v = &e->versions[e->nversions - 1];

			ed->key = version_key(e, v);
			ed->is_delete = v->is_delete;
			ed->value.data = v->value;
			ed->value.len = v->len;
			e = (D1Entry *) rbt_iterate(&it);
		}
		else
		{
			/* The spill tree's newest, copied: the cursor moves on. */
			D1SpillVersion v[2];
			int			nv = spill_decode(&sk, &sv, v);
			uint8	   *copy = palloc(Max(v[nv - 1].key.len + v[nv - 1].value.len, 1));

			memcpy(copy, v[nv - 1].key.data, v[nv - 1].key.len);
			memcpy(copy + v[nv - 1].key.len, v[nv - 1].value.data,
				   v[nv - 1].value.len);
			ed->key.data = copy;
			ed->key.len = v[nv - 1].key.len;
			ed->is_delete = v[nv - 1].is_delete;
			ed->value.data = copy + v[nv - 1].key.len;
			ed->value.len = v[nv - 1].value.len;
		}
		MemoryContextSwitchTo(old);
		n++;
		if (cmp <= 0)
			s_have = PROLLY_CALL(backend.c.tree, cursor_next, sc, &sk, &sv);
		if (n == EDIT_CHUNK)
		{
			PROLLY_CALL(backend.c.tree, apply, env, &root, edits, n, &root);
			MemoryContextReset(chunk);
			n = 0;
		}
	}
	if (n > 0)
		PROLLY_CALL(backend.c.tree, apply, env, &root, edits, n, &root);
	PROLLY_CALL(backend.c.tree, cursor_close, sc);
	MemoryContextDelete(chunk);
	pfree(edits);
	*out = root;
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
		if (ws->spill.root.hash.len > 0)
			order_lost_error();
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
	if (ws->spill.root.hash.len > 0)
	{
		materialize_spilled(ws, &env, &base, out);
		return;
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

	PROLLY_CALL(commit, branch_lock, xact->branch);
	tip.has_root = PROLLY_CALL(commit, tip, xact->branch, &tip.root);
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
	xact->ticket = PROLLY_CALL(commit, enqueue, xact->branch, &new_root,
							   GetCurrentTransactionId());
	xact->enqueued = true;
	PROLLY_CALL(commit, branch_unlock, xact->branch);
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

/*
 * Two-phase state: the branch, then per write set: sub (1 byte), relnumber, relid, replaced
 * (1 byte), the base's and the final tree's hashes (length byte, then the
 * bytes), the key order (kind byte: 0 memcmp, 1 table keying, 2 index
 * keying; suffixed byte; description length (4 bytes), description).
 * Native byte order: the state never leaves this cluster.
 */
#define ORDER_MEMCMP	0
#define ORDER_KEYING	1
#define ORDER_INDEX		2

static void
put_hash(StringInfo out, const ProllyNodeRef *ref)
{
	appendStringInfoChar(out, (char) ref->hash.len);
	appendBinaryStringInfo(out, ref->hash.bytes, ref->hash.len);
}

static void
d1_xact_prepare(StringInfo state)
{
	D1WriteSet **sets;
	int			n = 0;

	if (xact == NULL || xact->all == NULL)
	{
		/*
		 * Writes made and rolled back in subtransactions leave state but
		 * nothing to carry into the prepared transaction.
		 */
		xact_reset();
		return;
	}

	for (D1WriteSet *ws = xact->all; ws; ws = ws->next)
		n++;
	sets = palloc(sizeof(D1WriteSet *) * n);
	n = 0;
	for (D1WriteSet *ws = xact->all; ws; ws = ws->next)
		sets[n++] = ws;
	qsort(sets, n, sizeof(D1WriteSet *), ws_order);

	/*
	 * Each tree as this transaction leaves it, durable in the node store;
	 * COMMIT PREPARED replays the difference from its base onto whatever
	 * the branch holds then.
	 */
	appendBinaryStringInfo(state, &xact->branch, sizeof(ProllyBranchId));
	for (int i = 0; i < n; i++)
	{
		D1WriteSet *ws = sets[i];
		ProllyNodeRef final;
		StringInfoData desc;
		uint8		kind = ORDER_MEMCMP;
		uint32		dlen;

		materialize(ws, &final);
		initStringInfo(&desc);
		if (ws->order.keying)
		{
			kind = ORDER_KEYING;
			PROLLY_CALL(ws->order.routine, describe, ws->order.keying, &desc);
		}
		else if (ws->order.ix)
		{
			kind = ORDER_INDEX;
			PROLLY_CALL(ws->order.ix_routine, describe, ws->order.ix, &desc);
		}
		appendStringInfoChar(state, (char) ws->sub);
		appendBinaryStringInfo(state, &ws->relnumber, sizeof(RelFileNumber));
		appendBinaryStringInfo(state, &ws->relid, sizeof(Oid));
		appendStringInfoChar(state, (char) ws->replaced);
		put_hash(state, &ws->base);
		put_hash(state, &final);
		appendStringInfoChar(state, (char) kind);
		appendStringInfoChar(state, (char) ws->order.suffixed);
		dlen = desc.len;
		appendBinaryStringInfo(state, &dlen, sizeof(uint32));
		appendBinaryStringInfo(state, desc.data, desc.len);
		pfree(desc.data);
	}
	pfree(sets);

	/* Its memory goes with the transaction's; no commit or abort follows. */
	xact_reset();
}

typedef struct D1PreparedTree
{
	ProllySubRoot sub;
	RelFileNumber relnumber;
	Oid			relid;
	bool		replaced;
	ProllyNodeRef base;
	ProllyNodeRef final;
	uint8		kind;
	bool		suffixed;
	ProllyValue desc;
} D1PreparedTree;

static const char *
get_bytes(const char *p, const char *end, void *out, Size len)
{
	if (end - p < (ptrdiff_t) len)
		elog(ERROR, "prolly prepared state is corrupt");
	memcpy(out, p, len);
	return p + len;
}

static const char *
get_hash(const char *p, const char *end, ProllyNodeRef *ref)
{
	uint8		len;

	memset(ref, 0, sizeof(*ref));
	ref->location.blkno = InvalidBlockNumber;
	p = get_bytes(p, end, &len, 1);
	if (len > PROLLY_HASH_MAX_LEN)
		elog(ERROR, "prolly prepared state is corrupt");
	ref->hash.len = len;
	return get_bytes(p, end, ref->hash.bytes, len);
}

/* Replaying a diff as edits onto a tree, a chunk at a time. */
typedef struct D1Replay
{
	const ProllyTreeEnv *env;
	ProllyNodeRef root;
	ProllyEdit *edits;
	int			n;
	MemoryContext chunk;
} D1Replay;

static void
replay_flush(D1Replay *r)
{
	if (r->n > 0)
		PROLLY_CALL(backend.c.tree, apply, r->env, &r->root, r->edits, r->n,
					&r->root);
	MemoryContextReset(r->chunk);
	r->n = 0;
}

static void
replay_change(ProllyDiffKind kind, const ProllyKey *key,
			  const ProllyValue *before, const ProllyValue *after, void *arg)
{
	D1Replay   *r = arg;
	ProllyEdit *ed = &r->edits[r->n];
	uint32		vlen = after ? after->len : 0;
	uint8	   *copy = MemoryContextAlloc(r->chunk, Max(key->len + vlen, 1));

	memcpy(copy, key->data, key->len);
	if (after)
		memcpy(copy + key->len, after->data, after->len);
	ed->key.data = copy;
	ed->key.len = key->len;
	ed->is_delete = (after == NULL);
	ed->value.data = copy + key->len;
	ed->value.len = vlen;
	if (++r->n == EDIT_CHUNK)
		replay_flush(r);
}

/* The key order a prepared tree was written in. */
static void
prepared_order(const D1PreparedTree *t, Relation *rel, D1Order *order)
{
	ProllyComponents c;

	memset(order, 0, sizeof(*order));
	*rel = NULL;
	if (t->kind == ORDER_MEMCMP)
		return;
	*rel = relation_open(t->relid, AccessShareLock);
	prolly_components_for_relation(*rel, &c);
	order->suffixed = t->suffixed;
	if (t->kind == ORDER_INDEX)
	{
		order->ix_routine = c.index_keying;
		order->ix = PROLLY_CALL(c.index_keying, open, *rel, &t->desc,
								CurrentMemoryContext);
	}
	else
	{
		order->routine = c.keying;
		order->keying = PROLLY_CALL(c.keying, open, *rel, &t->desc,
									CurrentMemoryContext);
	}
}

static void
d1_commit_prepared(TransactionId xid, const char *state, Size len)
{
	const char *p = state;
	const char *end = state + len;
	D1PreparedTree *trees = NULL;
	int			n = 0;
	int			cap = 0;
	ProllyRootSet tip;
	ProllyRootEdit *edits;
	ProllyNodeRef new_root;
	const ProllyCommitRoutine *commit;
	ProllyBranchId branch;

	backend_init();
	p = get_bytes(p, end, &branch, sizeof(ProllyBranchId));
	while (p < end)
	{
		D1PreparedTree *t;
		uint8		b;
		uint32		dlen;

		if (n == cap)
		{
			cap = Max(cap * 2, 8);
			trees = trees ? repalloc(trees, sizeof(D1PreparedTree) * cap) :
				palloc(sizeof(D1PreparedTree) * cap);
		}
		t = &trees[n++];
		p = get_bytes(p, end, &b, 1);
		t->sub = (ProllySubRoot) b;
		p = get_bytes(p, end, &t->relnumber, sizeof(RelFileNumber));
		p = get_bytes(p, end, &t->relid, sizeof(Oid));
		p = get_bytes(p, end, &b, 1);
		t->replaced = (b != 0);
		p = get_hash(p, end, &t->base);
		p = get_hash(p, end, &t->final);
		p = get_bytes(p, end, &t->kind, 1);
		p = get_bytes(p, end, &b, 1);
		t->suffixed = (b != 0);
		p = get_bytes(p, end, &dlen, sizeof(uint32));
		if (end - p < (ptrdiff_t) dlen)
			elog(ERROR, "prolly prepared state is corrupt");
		t->desc.data = (const uint8 *) p;
		t->desc.len = dlen;
		p += dlen;
	}

	/* This transaction publishes the root when it commits. */
	xact_init();
	xact->branch = branch;
	commit = backend.c.commit;
	PROLLY_CALL(commit, branch_lock, xact->branch);
	tip.has_root = PROLLY_CALL(commit, tip, xact->branch, &tip.root);
	if (!tip.has_root)
		PROLLY_CALL(backend.c.root_layout, empty, &backend.root_env, &tip.root);
	tip.has_root = true;

	edits = palloc(sizeof(ProllyRootEdit) * Max(n, 1));
	for (int i = 0; i < n; i++)
	{
		D1PreparedTree *t = &trees[i];
		ProllyNodeRef current;

		edits[i].sub = t->sub;
		edits[i].relnumber = t->relnumber;
		edits[i].is_remove = false;
		edits[i].tree = t->final;
		find_tree(&tip, t->sub, t->relnumber, &current);
		if (!t->replaced && !same_hash(&current, &t->base))
		{
			/*
			 * Others committed to the tree since: replay this transaction's
			 * changes, the difference from its base, onto theirs.  Its row
			 * locks, held since, kept them off these keys.
			 */
			Relation	rel;
			D1Order		order;
			ProllyTreeEnv env;
			D1Replay	r;
			ProllyNodeRef from = t->base;

			prepared_order(t, &rel, &order);
			tree_env(&order, &env);
			if (from.hash.len == 0)
			{
				ProllyTreeBuilder *b = PROLLY_CALL(backend.c.tree, build_begin,
												   &env, CurrentMemoryContext);

				PROLLY_CALL(backend.c.tree, build_finish, b, &from);
			}
			r.env = &env;
			r.root = current;
			if (r.root.hash.len == 0)
				r.root = from;
			r.edits = palloc(sizeof(ProllyEdit) * EDIT_CHUNK);
			r.n = 0;
			r.chunk = AllocSetContextCreate(CurrentMemoryContext,
											"prolly d1 replay",
											ALLOCSET_DEFAULT_SIZES);
			PROLLY_CALL(backend.c.tree, diff, &env, &from, &t->final,
						replay_change, &r);
			replay_flush(&r);
			MemoryContextDelete(r.chunk);
			pfree(r.edits);
			edits[i].tree = r.root;
			close_order(&order);
			if (rel)
				relation_close(rel, AccessShareLock);
		}
	}
	PROLLY_CALL(backend.c.root_layout, apply, &backend.root_env, &tip.root,
				edits, n, &new_root);
	xact->ticket = PROLLY_CALL(commit, enqueue, xact->branch, &new_root,
							   xid);
	xact->enqueued = true;
	PROLLY_CALL(commit, branch_unlock, xact->branch);
	pfree(edits);
	pfree(trees);
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
	MemoryContextDelete(ws->data_mcxt);
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

			MemoryContextDelete(ws->data_mcxt);
			ws->data_mcxt = saved->data_mcxt;
			ws->entries = saved->entries;
			ws->nentries = saved->nentries;
			ws->base = saved->base;
			ws->replaced = saved->replaced;
			ws->order = saved->order;
			ws->spill = saved->spill;
			ws->spill_gen++;
			ws->saved = saved->prev;
		}

		/*
		 * Spilled versions of the subtransaction (or its children): the
		 * spill tree as it was without them, kept when it was open at a
		 * spill.
		 */
		{
			D1SpillMark **link = &ws->spill.marks;
			D1SpillMark *keep = find_mark(ws->spill.marks, subxid);

			if (keep)
			{
				ws->spill.root = keep->root;
				ws->spill_gen++;
			}
			while (*link)
			{
				if ((*link)->subxid >= subxid)
				{
					D1SpillMark *gone = *link;

					*link = gone->next;
					pfree(gone);
				}
				else
					link = &(*link)->next;
			}
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
	.unseen_writers = d1_unseen_writers,
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
	.commit_prepared = d1_commit_prepared,
	.vacuum = d1_vacuum,
	.node_counters = d1_node_counters,
};
