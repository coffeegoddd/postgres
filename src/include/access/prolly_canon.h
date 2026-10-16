/*-------------------------------------------------------------------------
 *
 * prolly_canon.h
 *	  Canonicalizer interface: a working set's canonical form.
 *
 * A version-control commit's root hash must depend only on the logical
 * content (schema and data) of what it commits, never on how it was
 * produced: not on OIDs, relfilenumbers, the order objects were created
 * in, which columns were dropped or added with defaults, or the session's
 * settings.  The canonicalizer derives that form from the database's
 * catalogs and a working set, by name, sharing the working set's data
 * trees (which are canonical as written).
 *
 * Contract:
 * - prepare() makes every table's rows canonical, as writes of the current
 *   transaction: rows stored before a column was dropped, or added with a
 *   default, are rewritten.  The caller then takes the working set the
 *   transaction will commit (the engine's pending_root).
 * - canonical_root() builds the canonical root of a working set: the same
 *   for the same logical content in any database or cluster.  An object
 *   it doesn't cover yet is an error, never a hash that might depend on
 *   history.
 * - walk() visits every node a canonical root keeps alive, for
 *   reclamation.
 * - fold() builds the canonical root of the session branch's versioned
 *   schemas as the calling transaction sees them, folding their tables'
 *   rows into trees: the same for the same logical content, however the
 *   rows and objects came to be.
 * - tables() visits each table a folded root holds, in name order: its
 *   qualified name, its data tree and its definition as described.
 * - same_schema() is whether two folded roots hold the same objects with
 *   the same definitions, whatever their tables' rows.
 * - move_schema() makes the session branch's objects defined as a folded
 *   root defines them, from as another (the branch folded now) does: it
 *   changes the definitions, then calls rows(), which writes the tables'
 *   rows, then adds what the rows must satisfy.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_canon.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_CANON_H
#define PROLLY_CANON_H

#include "access/prolly_registry.h"
#include "access/prolly_tree.h"
#include "access/prolly_types.h"

/*
 * Visits one table of a canonical root.  unreadable is NULL, or why its
 * rows can't be read here (its tree's keys may be in another order than
 * the database's collations give now): its name, data and description
 * are still the root's.
 */
typedef void (*ProllyCanonTableVisit) (const char *name,
									   const ProllyNodeRef *data,
									   const ProllyValue *description,
									   const char *unreadable,
									   void *arg);

typedef struct ProllyCanonRoutine
{
	ProllyImplInfo info;

	void		(*prepare) (void);
	void		(*canonical_root) (const ProllyNodeRef *working_set,
								   ProllyNodeRef *root);
	void		(*walk) (const ProllyNodeRef *root, ProllyTreeVisitNode visit,
						 void *arg);
	void		(*fold) (ProllyNodeRef *root);
	void		(*tables) (const ProllyNodeRef *root, ProllyCanonTableVisit visit,
						   void *arg);
	/* Its materialized views' rows, as tables' (never unreadable). */
	void		(*materialized_views) (const ProllyNodeRef *root,
									   ProllyCanonTableVisit visit, void *arg);
	bool		(*same_schema) (const ProllyNodeRef *a, const ProllyNodeRef *b);
	void		(*move_schema) (const ProllyNodeRef *from, const ProllyNodeRef *to,
								void (*rows) (void *arg), void *arg);
} ProllyCanonRoutine;

#define PROLLY_CANON_CALLBACK_NAMES \
	"prepare", "canonical_root", "walk", "fold", "tables", \
	"materialized_views", "same_schema", "move_schema"
#define PROLLY_CANON_FIRST_CALLBACK prepare

static inline const ProllyCanonRoutine *
prolly_canon_lookup(const char *name)
{
	return (const ProllyCanonRoutine *)
		prolly_get_impl(PROLLY_IFACE_CANONICALIZER, name);
}

#endif							/* PROLLY_CANON_H */
