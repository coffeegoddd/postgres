/*-------------------------------------------------------------------------
 *
 * prolly_tree.h
 *	  Tree interface: persistent prolly-tree operations.
 *
 * A tree is an immutable, sorted map from keys to values, identified by its
 * root node.  Operations never change a tree; apply() returns a new root
 * that shares every unchanged node with the old one.  Engines build their
 * tables, indexes and root sets from trees; diff and merge work on them.
 *
 * A tree is made of the foundation components in a ProllyTreeEnv, which the
 * caller (an engine) fills in from its configuration: node format, chunker,
 * content hash, node store and log, plus the key order.
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
 * - New nodes are WAL-logged through env->log before they are published in
 *   env->node_store, so a published node survives a crash.
 * - Keys and values returned by a cursor stay valid until the next call on
 *   it, or until it is closed.  Cursors and other operations hold buffer
 *   pins only while they run; nodes are immutable, so they take no locks
 *   beyond the buffer content locks needed to read a page.
 * - Errors are raised with ereport(); an item too large for a node is an
 *   error (large values go out of line, in a later step).
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
#include "access/prolly_hash.h"
#include "access/prolly_log.h"
#include "access/prolly_nodeformat.h"
#include "access/prolly_nodestore.h"
#include "access/prolly_registry.h"
#include "access/prolly_types.h"
#include "access/sdir.h"
#include "lib/stringinfo.h"

/* Key order: negative, zero or positive, like memcmp. */
typedef int (*ProllyKeyCompare) (const ProllyKey *a, const ProllyKey *b,
								 void *arg);

/* What a tree is made of; filled in by the caller. */
typedef struct ProllyTreeEnv
{
	const ProllyNodeFormatRoutine *node_format;
	const ProllyChunkerRoutine *chunker;
	const ProllyHashRoutine *hash;
	const ProllyNodeStoreRoutine *node_store;
	ProllyNodeStore *store;		/* opened by the caller */
	const ProllyLogRoutine *log;
	ProllyKeyCompare compare;
	void	   *compare_arg;
} ProllyTreeEnv;

/* One change for apply(): a put, or a delete if is_delete. */
typedef struct ProllyEdit
{
	ProllyKey	key;
	ProllyValue value;			/* ignored for a delete */
	bool		is_delete;
} ProllyEdit;

typedef struct ProllyTreeBuilder ProllyTreeBuilder;
typedef struct ProllyTreeCursor ProllyTreeCursor;

/* Called for each node of a tree (walk). */
typedef void (*ProllyTreeVisitNode) (const ProllyNodeRef *node, uint16 level,
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

	/* Number of items. */
	uint64		(*count) (const ProllyTreeEnv *env, const ProllyNodeRef *root);

	/*
	 * Append the key of the item at position ordinal (from 0, in key order)
	 * to key; false if there are no more items.  O(log n), from subtree
	 * counts.
	 */
	bool		(*key_at) (const ProllyTreeEnv *env, const ProllyNodeRef *root,
						   uint64 ordinal, StringInfo key);

	/* Visit every node reachable from root, parents before children. */
	void		(*walk) (const ProllyTreeEnv *env, const ProllyNodeRef *root,
						 ProllyTreeVisitNode visit, void *arg);

	/*
	 * Report each key whose presence or value differs between from and to,
	 * once, in key order.  Subtrees with the same hash are skipped.
	 */
	void		(*diff) (const ProllyTreeEnv *env, const ProllyNodeRef *from,
						 const ProllyNodeRef *to, ProllyTreeVisitChange visit,
						 void *arg);
} ProllyTreeRoutine;

#define PROLLY_TREE_CALLBACK_NAMES \
	"build_begin", "build_add", "build_finish", "get", "cursor_open", \
	"cursor_seek", "cursor_next", "cursor_close", "apply", "count", "key_at", \
	"walk", "diff"
#define PROLLY_TREE_FIRST_CALLBACK build_begin

static inline const ProllyTreeRoutine *
prolly_tree_lookup(const char *name)
{
	return (const ProllyTreeRoutine *)
		prolly_get_impl(PROLLY_IFACE_TREE, name);
}

#endif							/* PROLLY_TREE_H */
