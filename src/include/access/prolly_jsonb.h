/*-------------------------------------------------------------------------
 *
 * prolly_jsonb.h
 *	  jsonb's path operators on a document's JSON tree (prolly_jsontree.h),
 *	  when the document is stored as one.
 *
 * jsonb's operators and functions take a document whole: a detoast reads
 * every item of a JSON tree.  ->, ->>, #>, #>>, ?, ?|, ?&, #-, subscripts
 * (read and assigned), jsonb_set(), jsonb_set_lax() and jsonb_insert() call
 * these first, with their argument as it is stored.  When it is a hash
 * pointer to a JSON tree (prolly_toast.h), in a database whose encoding
 * is UTF-8, they read only the items under the path, or set the value by
 * editing the tree, and return true.  Otherwise, and for a path the tree
 * leaves to a whole read or edit, they return false and the caller does
 * what it does on the document, so a result never depends on how its
 * document is stored.  Every tree they read is checked as it is read
 * (prolly_jsontree.h).
 *
 * src/include/access/prolly_jsonb.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_JSONB_H
#define PROLLY_JSONB_H

#include "access/prolly_jsontree.h"
#include "utils/array.h"
#include "utils/jsonb.h"

/* What a document must be for a path read to follow its first step */
typedef enum ProllyJsonbKind
{
	PROLLY_JSONB_ANY,			/* #>: an object's key or an array's index */
	PROLLY_JSONB_OBJECT,		/* -> text and ?: an object's key only */
	PROLLY_JSONB_ARRAY,			/* -> int: an array's index only */
} ProllyJsonbKind;

/*
 * Whether doc (a jsonb argument as stored) is a JSON tree: *kind whether
 * the document is an object (jbvObject) or an array (jbvArray).
 */
extern bool prolly_jsonb_tree_kind(Datum doc, enum jbvType *kind);

/*
 * doc #> path, if doc (a jsonb argument as stored) is a JSON tree: *found
 * the value there, NULL if there is none or doc isn't of kind.
 */
extern bool prolly_jsonb_tree_get(Datum doc, ProllyJsonbKind kind,
								  char **path, int npath, Jsonb **found);

/*
 * jsonb_set(doc, path, value, create), if doc (as stored) is a JSON tree:
 * *result a hash pointer to the edited tree, doc itself if nothing
 * changed, or the document whole if it became too short to be a tree.
 * The edited tree's nodes are the top transaction's chunks (it takes a
 * transaction ID), so the edit is declined where they wouldn't outlive
 * the result or can't be written: in a parallel operation, a
 * subtransaction, a read-only transaction or recovery.  A result that no
 * row takes leaves its nodes unreferenced, for the chunk store's
 * collection to remove.
 */
extern bool prolly_jsonb_tree_set(Datum doc, ArrayType *path, Jsonb *value,
								  bool create, Datum *result);

/*
 * doc ?| keys (all: ?&), if doc (as stored) is a JSON tree: *result whether
 * any (all) of keys[0..nkeys) is one of its object's keys, or its array's
 * string elements.
 */
extern bool prolly_jsonb_tree_exists(Datum doc, char **keys, int nkeys,
									 bool all, bool *result);

/*
 * edit(doc, path, arg), if doc (as stored) is a JSON tree and the edit
 * changes it at path alone (prolly_jsontree.h's edit()): *result as
 * prolly_jsonb_tree_set() gives it, declined where it declines.  The edit
 * is given a document of what is along path only, so it is a function of
 * that and of path (jsonb_insert(), jsonb_delete_path(), a subscript's
 * assignment).
 */
extern bool prolly_jsonb_tree_edit(Datum doc, char **path, int npath,
								   ProllyJsonEdit edit, void *arg,
								   Datum *result);

/*
 * path's elements as strings: false if path has more than one dimension or
 * a NULL element (the caller's own error, or result, then).
 */
extern bool prolly_jsonb_path_elements(ArrayType *path, char ***elems,
									   int *n);

#endif							/* PROLLY_JSONB_H */
