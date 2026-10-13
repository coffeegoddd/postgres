/*-------------------------------------------------------------------------
 *
 * prolly_rootlayout.h
 *	  Root layout interface: how a branch's root finds every relation's tree.
 *
 * A branch's root (its working-set root, published at SQL COMMIT) points
 * to two sub-roots: the catalog sub-root, holding every system catalog's
 * trees, and the user-data sub-root, holding user tables' and indexes'
 * trees.  Each sub-root maps a relation to the root of its tree.  "Did the
 * schema change?" is one hash comparison of catalog sub-roots, and diff
 * skips whichever sub-root didn't change.
 *
 * This is the local, Postgres-native layout: relations are identified by
 * OID, one entry per relation (a table and each of its indexes are separate
 * entries).  The canonical root of a version-control commit uses the same
 * shape with relations identified by name; it comes with the schema
 * canonicalizer, which derives those names.
 *
 * A layout is built from trees (prolly_tree.h), so a root is an immutable
 * value like a tree: operations never change one, and apply() returns a
 * new root sharing every unchanged node.  The caller passes the tree
 * implementation and its environment; the layout orders its own keys, so
 * env->compare is not used.
 *
 * Contract:
 * - History independence: the root hash depends only on which relations
 *   are in each sub-root and the hashes of their trees, never on the order
 *   or batching of the applies that produced it.  Removing every relation
 *   gives the empty root's hash.
 * - Which sub-root a relation belongs in is the caller's decision; the
 *   layout doesn't apply Postgres's catalog rules.
 * - apply() takes edits sorted by (sub-root, relid) with no duplicates.
 *   Setting a relation that is present replaces its tree; removing one that
 *   is absent does nothing.
 * - A relation's tree is referenced by hash only (no block hint), so the
 *   layout stays free of physical locations.
 * - Errors are raised with ereport().
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_rootlayout.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_ROOTLAYOUT_H
#define PROLLY_ROOTLAYOUT_H

#include "access/prolly_registry.h"
#include "access/prolly_tree.h"
#include "access/prolly_types.h"

typedef enum ProllySubRoot
{
	PROLLY_SUBROOT_CATALOG = 0,
	PROLLY_SUBROOT_USER_DATA = 1,
} ProllySubRoot;

#define PROLLY_NUM_SUBROOTS 2

/* What a layout is made of; filled in by the caller. */
typedef struct ProllyRootEnv
{
	const ProllyTreeRoutine *tree;
	const ProllyTreeEnv *tree_env;
} ProllyRootEnv;

/* One change for apply(): set relid's tree in sub, or remove relid. */
typedef struct ProllyRootEdit
{
	ProllySubRoot sub;
	Oid			relid;
	ProllyNodeRef tree;			/* ignored for a remove */
	bool		is_remove;
} ProllyRootEdit;

/* Called for each relation in a sub-root (scan). */
typedef void (*ProllyRootVisitRel) (ProllySubRoot sub, Oid relid,
									const ProllyNodeRef *tree, void *arg);

/* Called for each relation whose tree differs between two roots (diff). */
typedef void (*ProllyRootVisitChange) (ProllyDiffKind kind, ProllySubRoot sub,
									   Oid relid,
									   const ProllyNodeRef *before,	/* NULL if added */
									   const ProllyNodeRef *after,	/* NULL if removed */
									   void *arg);

typedef struct ProllyRootLayoutRoutine
{
	ProllyImplInfo info;

	/* The root with no relations (both sub-roots empty). */
	void		(*empty) (const ProllyRootEnv *env, ProllyNodeRef *root);

	/* Find relid's tree in sub; false if relid isn't there. */
	bool		(*lookup) (const ProllyRootEnv *env, const ProllyNodeRef *root,
						   ProllySubRoot sub, Oid relid, ProllyNodeRef *tree);

	/* Apply edits (sorted by sub-root, then relid) to root, giving *new_root. */
	void		(*apply) (const ProllyRootEnv *env, const ProllyNodeRef *root,
						  const ProllyRootEdit *edits, int nedits,
						  ProllyNodeRef *new_root);

	/* The root of sub, to compare by hash. */
	void		(*subroot) (const ProllyRootEnv *env, const ProllyNodeRef *root,
							ProllySubRoot sub, ProllyNodeRef *subroot);

	/* Visit each relation in sub, in relid order. */
	void		(*scan) (const ProllyRootEnv *env, const ProllyNodeRef *root,
						 ProllySubRoot sub, ProllyRootVisitRel visit, void *arg);

	/*
	 * Visit every node reachable from root, including every node of every
	 * relation's tree (what reclamation must keep), parents before children.
	 * A node shared by several trees may be visited more than once.
	 */
	void		(*walk) (const ProllyRootEnv *env, const ProllyNodeRef *root,
						 ProllyTreeVisitNode visit, void *arg);

	/*
	 * Report each relation whose presence or tree differs between from and
	 * to, once, in (sub-root, relid) order.  A sub-root with the same hash
	 * in both is skipped.
	 */
	void		(*diff) (const ProllyRootEnv *env, const ProllyNodeRef *from,
						 const ProllyNodeRef *to, ProllyRootVisitChange visit,
						 void *arg);
} ProllyRootLayoutRoutine;

#define PROLLY_ROOTLAYOUT_CALLBACK_NAMES \
	"empty", "lookup", "apply", "subroot", "scan", "walk", "diff"
#define PROLLY_ROOTLAYOUT_FIRST_CALLBACK empty

static inline const ProllyRootLayoutRoutine *
prolly_rootlayout_lookup(const char *name)
{
	return (const ProllyRootLayoutRoutine *)
		prolly_get_impl(PROLLY_IFACE_ROOT_LAYOUT, name);
}

#endif							/* PROLLY_ROOTLAYOUT_H */
