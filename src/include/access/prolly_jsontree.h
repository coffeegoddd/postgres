/*-------------------------------------------------------------------------
 *
 * prolly_jsontree.h
 *	  JSON tree interface: a jsonb document stored as a tree of its
 *	  structure, so a path is read, and a value set, without the whole
 *	  document.
 *
 * A document's tree is a tree of the tree interface (prolly_tree.h) keyed
 * by paths, in jsonb's own order: an object's keys as jsonb sorts them
 * (shorter first, then by bytes), an array's elements by index, a
 * container before what it holds.  A container whose jsonb bytes are
 * PROLLY_JSON_PACK_MAX or fewer is one item, at its path, holding those
 * bytes; a longer one is no item of its own, but its children's: each
 * scalar an item, each container one item or its children's again.  So
 * the items are a function of the document alone, and a tree's root of
 * its items alone: the same document has the same root however it was
 * made, built whole or edited a value at a time.  jsonb's semantics are
 * kept: numerics as jsonb holds them (1.0 stays 1.0), JSON null as a value
 * (SQL NULL is no document), strings in UTF-8 whatever the database's
 * encoding.  A document is a tree only if its canonical bytes (a version
 * byte and its jsonb binary) are longer than PROLLY_JSON_TREE_MIN, and it
 * is a container: a shorter one, or a scalar, is its bytes.
 *
 * Contract:
 * - build() writes the tree of a document, given as its jsonb binary with
 *   its strings in UTF-8 (canonical jsonb bytes after their version
 *   byte), into env's chunk store and sets *root; false (nothing to use
 *   written) if the document can't be one: a scalar, too short, or a path
 *   longer than the tree's longest key.
 * - read() gives the document back, in the database's encoding, every
 *   item read; get() the value at a path (path[i] an object's key or an
 *   array's index, as #> takes them, a negative index counting from the
 *   end), reading only the items under it and the nodes above them: NULL
 *   if there is none.  A path get() doesn't handle makes it return false,
 *   for the caller to read the document whole ("paths" handles every
 *   path).  kind() is whether the document is an object (jbvObject) or an
 *   array (jbvArray), reading the nodes to its first item.  get(), set()
 *   and diff() work in UTF-8: callers in another encoding read whole.
 * - set() gives the tree of jsonb_set(doc, path, value, create) in
 *   *new_root, by applying edits for the items under path and the
 *   containers above it alone, and *delta an estimate of the change in
 *   the document's jsonb size.  When the result is too short to be a tree,
 *   it is *whole instead (else NULL): no estimate decides what is a tree.
 *   It returns false for what it doesn't handle (an array index that isn't
 *   one of the array's, counting from either end, a key too long for the
 *   tree): the caller sets the document whole.
 * - diff() reports each scalar path whose value differs between two
 *   trees, in order (an empty container counting as a scalar), skipping
 *   the subtrees both share unread, so d changed items cost
 *   O((d + 1) log n) nodes; diff_documents() reports the same for two
 *   documents in memory, which a caller without trees uses.
 * - Every tree read must have been written by the same implementation:
 *   read() checks what it reads is a document's tree (paths well formed,
 *   an array's indexes from 0 without gaps, nothing under an item, each
 *   container packed exactly when it is short enough) and the document
 *   what jsonb builds, as canonical jsonb bytes are checked.  get(),
 *   kind() and set() check the items and nodes they read, and only those:
 *   an index is found by its key, and an array's size taken from its last
 *   element's key, so a gap in an array's indexes elsewhere is not seen
 *   (the array as read by read() is refused).
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_jsontree.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_JSONTREE_H
#define PROLLY_JSONTREE_H

#include "access/prolly_registry.h"
#include "access/prolly_tree.h"
#include "utils/jsonb.h"

/*
 * A document whose canonical bytes are this long or shorter is not a tree:
 * the length past which a row holds a value out of line.
 */
#define PROLLY_JSON_TREE_MIN	2048

/* A container whose jsonb bytes are this long or shorter is one item. */
#define PROLLY_JSON_PACK_MAX	1024

/*
 * Called for each changed scalar path diff() finds: the path's npath
 * elements (keys, and indexes in decimal), and the value before and after
 * (NULL for a path added or removed).
 */
typedef void (*ProllyJsonChangeVisit) (ProllyDiffKind kind, char **path,
									   int npath, Jsonb *before, Jsonb *after,
									   void *arg);

typedef struct ProllyJsonTreeRoutine
{
	ProllyImplInfo info;

	bool		(*build) (const ProllyTreeEnv *env, const char *doc, Size len,
						  ProllyNodeRef *root);
	Jsonb	   *(*read) (const ProllyTreeEnv *env, const ProllyNodeRef *root);
	bool		(*get) (const ProllyTreeEnv *env, const ProllyNodeRef *root,
						char **path, int npath, Jsonb **result);
	enum jbvType (*kind) (const ProllyTreeEnv *env, const ProllyNodeRef *root);
	bool		(*set) (const ProllyTreeEnv *env, const ProllyNodeRef *root,
						char **path, int npath, Jsonb *value, bool create,
						ProllyNodeRef *new_root, int64 *delta, Jsonb **whole);
	void		(*diff) (const ProllyTreeEnv *env, const ProllyNodeRef *from,
						 const ProllyNodeRef *to, ProllyJsonChangeVisit visit,
						 void *arg);
	void		(*diff_documents) (Jsonb *from, Jsonb *to,
								   ProllyJsonChangeVisit visit, void *arg);
} ProllyJsonTreeRoutine;

#define PROLLY_JSONTREE_CALLBACK_NAMES \
	"build", "read", "get", "kind", "set", "diff", "diff_documents"
#define PROLLY_JSONTREE_FIRST_CALLBACK build
#define PROLLY_JSONTREE_LAST_CALLBACK diff_documents

/*
 * The memory this backend's last whole read of a tree took, its working
 * memory and the document: "paths" bounds it by a multiple of the
 * document's size.  For tests.
 */
extern PGDLLIMPORT Size prolly_jsontree_read_memory;

static inline const ProllyJsonTreeRoutine *
prolly_jsontree_lookup(const char *name)
{
	return (const ProllyJsonTreeRoutine *)
		prolly_get_impl(PROLLY_IFACE_JSON_TREE, name);
}

#endif							/* PROLLY_JSONTREE_H */
