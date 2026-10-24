/*-------------------------------------------------------------------------
 *
 * prolly_jsontree.h
 *	  JSON tree interface: a jsonb document stored as a tree of its
 *	  structure, so a path is read, and a value set, without the whole
 *	  document.
 *
 * A document's tree is a tree of the tree interface (prolly_tree.h) whose
 * items are the document's scalars, each keyed by its path, in jsonb's own
 * order: an object's keys as jsonb sorts them (shorter first, then by
 * bytes), an array's elements by index, a container before what it holds.
 * An empty container is an item of its own, at its path; a document that
 * is one scalar is one item, at the empty path.  The tree's items are a
 * function of the document alone, and a tree's root of its items alone,
 * so the root is a function of the document: the same document has the
 * same root however it was made, built whole or edited a value at a time.
 * jsonb's semantics are kept: numerics as jsonb holds them (1.0 stays
 * 1.0), JSON null as an item (SQL NULL is no document), strings in UTF-8.
 *
 * Contract:
 * - build() writes the tree of doc into env's chunk store and sets *root;
 *   false (nothing to use written) if the document can't be one, a path
 *   longer than the tree's longest key.
 * - read() gives the document back, every item read; get() the value at
 *   a path (path[i] an object's key or an array's index, as #> takes
 *   them), reading only the items under it and the nodes above them: NULL
 *   if there is none.  A path neither handles (an array index that isn't
 *   a non-negative integer) makes get() return false, for the caller to
 *   read the document whole.
 * - set() gives the tree of jsonb_set(doc, path, value, create) in
 *   *new_root, by applying edits for the items under path alone, and
 *   *delta the change in the document's jsonb size (exact for the value
 *   replaced, so an estimate only in jsonb's padding).  It returns false
 *   for what it doesn't handle (an array index that isn't one of the
 *   array's, a document that is a scalar): the caller sets the document
 *   whole.
 * - diff() reports each scalar path whose value differs between two
 *   trees, in order, skipping the subtrees both share unread, so d changed
 *   items cost O((d + 1) log n) nodes; diff_documents() reports the same
 *   for two documents in memory, which a caller without trees uses.
 * - Every tree read must have been written by the same implementation:
 *   read() checks what it reads is a document's (paths well formed, an
 *   array's indexes from 0 without gaps, nothing under a scalar, strings
 *   in UTF-8, numerics as numeric builds them).
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

	bool		(*build) (const ProllyTreeEnv *env, Jsonb *doc,
						  ProllyNodeRef *root);
	Jsonb	   *(*read) (const ProllyTreeEnv *env, const ProllyNodeRef *root);
	bool		(*get) (const ProllyTreeEnv *env, const ProllyNodeRef *root,
						char **path, int npath, Jsonb **result);
	bool		(*set) (const ProllyTreeEnv *env, const ProllyNodeRef *root,
						char **path, int npath, Jsonb *value, bool create,
						ProllyNodeRef *new_root, int64 *delta);
	void		(*diff) (const ProllyTreeEnv *env, const ProllyNodeRef *from,
						 const ProllyNodeRef *to, ProllyJsonChangeVisit visit,
						 void *arg);
	void		(*diff_documents) (Jsonb *from, Jsonb *to,
								   ProllyJsonChangeVisit visit, void *arg);
} ProllyJsonTreeRoutine;

#define PROLLY_JSONTREE_CALLBACK_NAMES \
	"build", "read", "get", "set", "diff", "diff_documents"
#define PROLLY_JSONTREE_FIRST_CALLBACK build
#define PROLLY_JSONTREE_LAST_CALLBACK diff_documents

static inline const ProllyJsonTreeRoutine *
prolly_jsontree_lookup(const char *name)
{
	return (const ProllyJsonTreeRoutine *)
		prolly_get_impl(PROLLY_IFACE_JSON_TREE, name);
}

#endif							/* PROLLY_JSONTREE_H */
