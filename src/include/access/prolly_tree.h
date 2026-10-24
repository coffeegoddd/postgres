/*-------------------------------------------------------------------------
 *
 * prolly_tree.h
 *	  Tree interface: persistent prolly-tree operations.
 *
 * A tree is an immutable, sorted map from keys to values, identified by its
 * root node.  Operations never change a tree; apply() returns a new root
 * that shares every unchanged node with the old one.  A commit's tables
 * are folded into trees, and its canonical root is one; diff and merge work
 * on them.
 *
 * A tree is made of the components in a ProllyTreeEnv, which the caller
 * fills in: chunk format, chunker, content hash and chunk store, plus the
 * key order.
 *
 * Contract:
 * - History independence: the root hash depends only on the tree's items
 *   (keys and values) and on the components' formats, never on how the tree
 *   was produced (which builds and applies, in what order and batch sizes).
 *   This is the lowest layer of canonical form: a commit's root hash
 *   depends only on its logical content.
 * - The empty tree is a tree (a leaf with no items) and has a root hash.
 * - Keys are unique and ordered by env->compare.  build_add() takes keys in
 *   strictly increasing order; apply() takes edits sorted by key with no
 *   duplicate keys.  Deleting an absent key does nothing; putting an
 *   existing key replaces its value.
 * - Nodes are chunks in a chunk store, which logs new ones, so a node put
 *   there survives a crash.
 * - Keys and values returned by a cursor stay valid until the next call on
 *   it, or until it is closed.  Nodes are immutable, so reading them takes
 *   no locks.
 * - Errors are raised with ereport(); an item too large for a node is an
 *   error.  A long leaf value goes out of line, into a value tree.
 * - If env->counters is set, every node an operation reads from the chunk
 *   store adds one to nodes_read, and every node it writes (not one the
 *   store already had) one to nodes_written, and to value_nodes_written
 *   too if it is a value tree's, so callers can check an operation's cost
 *   by counting.
 * - references() visits the chunks a node's chunk names: an internal
 *   node's children, a leaf's values stored out of line (their trees'
 *   roots), and the references its items declare (value_refs()'s and
 *   chunk_refs()'s).  It reads nothing but the chunk; one that isn't a
 *   node of the format names none.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_tree.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_TREE_H
#define PROLLY_TREE_H

#include "access/prolly_chunker.h"
#include "access/prolly_chunkformat.h"
#include "access/prolly_chunkstore.h"
#include "access/prolly_hash.h"
#include "access/prolly_registry.h"
#include "access/prolly_types.h"
#include "access/sdir.h"
#include "lib/stringinfo.h"

/* Key order: negative, zero or positive, like memcmp. */
typedef int (*ProllyKeyCompare) (const ProllyKey *a, const ProllyKey *b,
								 void *arg);

/*
 * Keys ordered by their bytes, a key before the longer ones it begins: the
 * order of trees keyed by encoded names or hashes, the canonical root's
 * among them.  A ProllyKeyCompare.
 */
static inline int
prolly_bytes_compare(const ProllyKey *a, const ProllyKey *b, void *arg)
{
	int			r = memcmp(a->data, b->data, Min(a->len, b->len));

	return r != 0 ? r : (a->len > b->len) - (a->len < b->len);
}

/* What a tree is made of; filled in by the caller. */
typedef struct ProllyTreeEnv
{
	const ProllyChunkerRoutine *chunker;
	const ProllyHashRoutine *hash;
	ProllyKeyCompare compare;
	void	   *compare_arg;
	ProllyNodeCounters *counters;	/* optional */
	const ProllyChunkFormatRoutine *chunk_format;
	const ProllyChunkStoreRoutine *chunk_store;
	ProllyChunkStore *chunks;	/* opened by the caller */

	/*
	 * Optional, for a tree whose leaf values hold references to value trees
	 * of their own (value_write()'s): appends the references a value holds
	 * to refs, one after another.  A leaf records them, so walks reach those
	 * value trees from the nodes alone, without knowing how values are laid
	 * out.  Every tree written with values that hold references must have
	 * it set.
	 */
	void		(*value_refs) (const ProllyValue *value, void *arg,
							   StringInfo refs);
	void	   *value_refs_arg;

	/*
	 * Optional, for a tree whose leaf values name other chunks, not trees of
	 * their own (a commit's parents, a branch's head, a table's data tree in
	 * a canonical root): appends the references the item for key and value
	 * holds to refs, as value_refs() does (value_reference()'s).  A leaf
	 * records them, so references() reports them, and a collection follows
	 * them without knowing how values are laid out; walks don't, those
	 * chunks being no part of the tree (their callers walk them as they
	 * mean to).  Every tree written with values that name chunks must have
	 * it set.
	 */
	void		(*chunk_refs) (const ProllyKey *key, const ProllyValue *value,
							   void *arg, StringInfo refs);
	void	   *chunk_refs_arg;
} ProllyTreeEnv;

/*
 * Where values out of line are stored and read: a tree implementation's
 * value trees, in an environment's chunk store; and, optionally, a memo of
 * the values TOAST stored out of line that were stored there before
 * (prolly_memo.h).
 */
typedef struct ProllyValueStore
{
	const struct ProllyTreeRoutine *tree;
	const ProllyTreeEnv *env;
	const struct ProllyValueMemoRoutine *memo;	/* NULL if none */

	/*
	 * Decoding gives a long value a hash pointer can name as that pointer,
	 * unread (prolly_toast_pointer()): for rows written back to a table,
	 * which then hold the tree history has.  Other readers get values.
	 */
	bool		pointers;
} ProllyValueStore;

/* One change for apply(): a put, or a delete if is_delete. */
typedef struct ProllyEdit
{
	ProllyKey	key;
	ProllyValue value;			/* ignored for a delete */
	bool		is_delete;
} ProllyEdit;

typedef struct ProllyTreeBuilder ProllyTreeBuilder;
typedef struct ProllyTreeCursor ProllyTreeCursor;

/*
 * Called for each node of a tree (walk); returns whether to visit the
 * nodes below it (its children, and a leaf's value trees).
 */
typedef bool (*ProllyTreeVisitNode) (const ProllyNodeRef *node, uint16 level,
									 void *arg);

/*
 * Called for each row a read of changes finds (the change finder's
 * changes()), as a tree's diff() calls ProllyTreeVisitChange: false stops
 * the read there.
 */
typedef bool (*ProllyRowChangeVisit) (ProllyDiffKind kind,
									  const ProllyKey *key,
									  const ProllyValue *before,	/* NULL if added */
									  const ProllyValue *after,	/* NULL if removed */
									  void *arg);

/* Called for each key that differs between two trees (diff). */
typedef void (*ProllyTreeVisitChange) (ProllyDiffKind kind,
									   const ProllyKey *key,
									   const ProllyValue *before,	/* NULL if added */
									   const ProllyValue *after,	/* NULL if removed */
									   void *arg);

typedef struct ProllyTreeRoutine
{
	ProllyImplInfo info;

	/* Build a tree from items given in key order. */
	ProllyTreeBuilder *(*build_begin) (const ProllyTreeEnv *env,
									   MemoryContext mcxt);
	void		(*build_add) (ProllyTreeBuilder *builder, const ProllyKey *key,
							  const ProllyValue *value);
	/* Finish, set *root, and free the builder. */
	void		(*build_finish) (ProllyTreeBuilder *builder, ProllyNodeRef *root);

	/* Look up key; on success append its value to *value. */
	bool		(*get) (const ProllyTreeEnv *env, const ProllyNodeRef *root,
						const ProllyKey *key, StringInfo value);

	/*
	 * Cursors.  cursor_seek positions before the first key >= key (forward)
	 * or after the last key <= key (backward); a NULL key means the start or
	 * the end.  cursor_next returns false when there are no more items.
	 * A seek reads only the nodes on the new key's path that the cursor
	 * doesn't hold already, so seeks in key order read each node about
	 * once, as a scan does.
	 */
	ProllyTreeCursor *(*cursor_open) (const ProllyTreeEnv *env,
									  const ProllyNodeRef *root,
									  MemoryContext mcxt);
	void		(*cursor_seek) (ProllyTreeCursor *cursor, const ProllyKey *key,
								ScanDirection direction);
	bool		(*cursor_next) (ProllyTreeCursor *cursor, ProllyKey *key,
								ProllyValue *value);
	void		(*cursor_close) (ProllyTreeCursor *cursor);

	/* Apply edits (sorted by key) to root, giving *new_root. */
	void		(*apply) (const ProllyTreeEnv *env, const ProllyNodeRef *root,
						  const ProllyEdit *edits, int nedits,
						  ProllyNodeRef *new_root);

	/* Number of items, from the subtree counts internal items record. */
	uint64		(*count) (const ProllyTreeEnv *env, const ProllyNodeRef *root);

	/*
	 * Visit every node reachable from root (value trees too), parents
	 * before children, skipping what lies below a node the visitor
	 * declines.  Returns whether the visitor took root.
	 */
	bool		(*walk) (const ProllyTreeEnv *env, const ProllyNodeRef *root,
						 ProllyTreeVisitNode visit, void *arg);

	/*
	 * Report each key whose presence or value differs between from and to,
	 * once, in key order; a key equal in both by the tree's order but
	 * written differently is modified (reported as to writes it).  Subtrees
	 * with the same hash are skipped without being read, so with d changed
	 * keys it reads O((d + 1) log n) nodes, not every leaf.
	 */
	void		(*diff) (const ProllyTreeEnv *env, const ProllyNodeRef *from,
						 const ProllyNodeRef *to, ProllyTreeVisitChange visit,
						 void *arg);

	/*
	 * Visit the nodes of to (value trees too) that from may lack, as diff
	 * finds them: O((d + 1) log n) nodes for d changed keys, every one to
	 * has that from lacks among them (and perhaps some it has elsewhere).
	 * from NULL or empty: every node of to, as walk.
	 */
	void		(*diff_nodes) (const ProllyTreeEnv *env, const ProllyNodeRef *from,
							   const ProllyNodeRef *to,
							   ProllyTreeVisitNode visit, void *arg);


	/* The chunks a node's chunk names, in the chunk format. */
	void		(*references) (const ProllyChunkFormatRoutine *format,
							   const void *data, Size len,
							   ProllyChunkRefVisit visit, void *arg);

	/*
	 * Values out of line, as the tree stores a long leaf value: a value
	 * tree in the same chunk store.  value_write() stores len bytes and
	 * appends their reference to ref: a function of the bytes alone, which
	 * names the value tree's root and records the length.  value_read()
	 * appends bytes [offset, offset + len) of the value a reference names
	 * (fewer if it ends first), reading only the nodes that hold them, so a
	 * slice costs O(depth + its length).  walk() visits a value tree from
	 * its root as any tree.
	 */
	void		(*value_write) (const ProllyTreeEnv *env, const uint8 *data,
								Size len, StringInfo ref);
	void		(*value_read) (const ProllyTreeEnv *env, const ProllyValue *ref,
							   uint64 offset, uint64 len, StringInfo out);

	/* Whether the value tree a reference names is in the chunk store. */
	bool		(*value_present) (const ProllyTreeEnv *env,
								  const ProllyValue *ref);

	/*
	 * A value tree's reference from its root and length, appended to ref;
	 * and the root and length a reference names.
	 */
	void		(*value_reference) (const ProllyHash *root, uint64 len,
									StringInfo ref);
	void		(*value_root) (const ProllyValue *ref, ProllyHash *root,
							   uint64 *len);

	/*
	 * The longest key a tree holds in format, its nodes referenced by
	 * hash's hashes, whatever its size and shape: build_add() and apply()
	 * take every key up to it, with any value whose references
	 * (value_refs()) take less than half a node, and may refuse a longer
	 * one.
	 */
	Size		(*key_max) (const ProllyChunkFormatRoutine *format,
							const ProllyHashRoutine *hash);

	/*
	 * Optional: diff() of trees whose keys env->compare can't order (a
	 * table's, ordered by collations only its relation gives, where the
	 * session has none): the same changes, found by the nodes' hashes and
	 * matched by the keys' bytes (keys equal only by the tree's order, not
	 * as bytes, are a removal and an addition), reported in the order of
	 * those bytes.  It reads O((d + 1) log n) nodes too.
	 */
	void		(*diff_unordered) (const ProllyTreeEnv *env,
								   const ProllyNodeRef *from,
								   const ProllyNodeRef *to,
								   ProllyTreeVisitChange visit, void *arg);

	/*
	 * Optional: how many keys diff() would report, estimated reading at
	 * most max_nodes nodes, for a caller choosing whether a diff is worth
	 * walking.  Exact where the nodes that differ fit in max_nodes; past
	 * it, never less than diff's count (each node left unread counted as
	 * if every item it holds changed).
	 */
	uint64		(*diff_estimate) (const ProllyTreeEnv *env,
								  const ProllyNodeRef *from,
								  const ProllyNodeRef *to,
								  uint64 max_nodes);

	/*
	 * Optional: how many of root's keys are less than key, exactly, from
	 * the subtree counts internal items record: the nodes on key's path
	 * alone are read.  Two ranks count the keys in a range.
	 */
	uint64		(*rank) (const ProllyTreeEnv *env, const ProllyNodeRef *root,
						 const ProllyKey *key);
} ProllyTreeRoutine;

#define PROLLY_TREE_CALLBACK_NAMES \
	"build_begin", "build_add", "build_finish", "get", "cursor_open", \
	"cursor_seek", "cursor_next", "cursor_close", "apply", "count", \
	"walk", "diff", "diff_nodes", "references", "value_write", "value_read", \
	"value_present", "value_reference", "value_root", "key_max", \
	"diff_unordered", "diff_estimate", "rank"
#define PROLLY_TREE_FIRST_CALLBACK build_begin
#define PROLLY_TREE_LAST_CALLBACK rank

static inline const ProllyTreeRoutine *
prolly_tree_lookup(const char *name)
{
	return (const ProllyTreeRoutine *)
		prolly_get_impl(PROLLY_IFACE_TREE, name);
}

#endif							/* PROLLY_TREE_H */
