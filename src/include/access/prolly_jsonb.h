/*-------------------------------------------------------------------------
 *
 * prolly_jsonb.h
 *	  jsonb's path operators on a document's JSON tree (prolly_jsontree.h),
 *	  when the document is stored as one.
 *
 * jsonb's operators and functions take a document whole: a detoast reads
 * every item of a JSON tree.  ->, ->>, #>, #>>, ? and jsonb_set() call
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
 */
extern bool prolly_jsonb_tree_set(Datum doc, ArrayType *path, Jsonb *value,
								  bool create, Datum *result);

#endif							/* PROLLY_JSONB_H */
