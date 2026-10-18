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
 * - walk() visits every node a canonical root keeps alive, for
 *   reclamation and transfer; a root the visitor declines isn't looked
 *   into (the trees it names are taken as seen), as a tree's node isn't.
 * - fold() builds the canonical root of the session branch's versioned
 *   schemas as the calling transaction sees them, folding their tables'
 *   rows into trees: the same for the same logical content, however the
 *   rows and objects came to be.  'baseline' (NULL: none) is the branch's
 *   last fold for a commit or checkpoint: its tables' trees are the
 *   starting point, and only what changed since is read.  With
 *   'next_baseline', the fold begins a new one, which holds once the
 *   transaction commits it as the branch's baseline.
 * - walk_baseline() visits every node a baseline keeps alive; a baseline
 *   the visitor declines isn't looked into, as a root isn't.
 * - tables() visits each table a folded root holds, in name order: its
 *   qualified name, its data tree and its definition as described.
 * - same_schema() is whether two folded roots hold the same objects with
 *   the same definitions, whatever their tables' rows.
 * - objects() visits each object other than a table (a schema, function,
 *   procedure, view, materialized view, type or sequence) that two folded
 *   roots hold differently, in the canonical tree's order: by kind, then
 *   qualified name (a routine's with its argument types).  A sequence
 *   whose value differs, or a materialized view whose rows do, differs.
 * - fold_dependencies() builds the tree of what the session branch's
 *   versioned objects need from outside the versioned schemas, for a
 *   commit to record beside its root: keyed by kind and identity
 *   ("extension hstore"), an extension valued by its version and schema,
 *   another object by its address (its type, names and arguments, as
 *   pg_identify_object_as_address() gives them), each a text[] literal.
 *   Not roles.  It is no part of the root, so what changes outside never
 *   makes a branch's working set differ from its head.
 * - dependencies() visits each item of such a tree, in key order.
 * - sequences() visits each sequence a folded root holds, in name order,
 *   with its value: its last value, and whether it was given out.
 * - merge_schema() merges three folded roots' definitions: each object as
 *   the side that changed it has it; one both changed differently is a
 *   schema conflict, an error.
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

/* Visits one dependency a commit records: its name and its value. */
typedef void (*ProllyCanonDependencyVisit) (const char *name,
											const char *value, void *arg);

/*
 * Visits one sequence of a canonical root, with its value (one recorded
 * without one, unreadable where it was folded, isn't visited).
 */
typedef void (*ProllyCanonSequenceVisit) (const char *name, int64 last,
										  bool called, void *arg);

/* Visits one object (not a table) two canonical roots hold differently. */
typedef void (*ProllyCanonObjectVisit) (const char *name, ProllyDiffKind kind,
										void *arg);

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

/* A table's column, as its description names it. */
typedef struct ProllyTableColumn
{
	char	   *name;
	char	   *type;			/* as format_type() writes it */
} ProllyTableColumn;

typedef struct ProllyCanonRoutine
{
	ProllyImplInfo info;

	void		(*walk) (const ProllyNodeRef *root, ProllyTreeVisitNode visit,
						 void *arg);
	/*
	 * The session branch's versioned schemas folded.  For a commit, objects
	 * the canonical form doesn't describe are refused; otherwise (a move's
	 * fold, a checkpoint's) they are left out of root, as read_fold() leaves
	 * them.
	 */
	void		(*fold) (const ProllyNodeRef *baseline, ProllyNodeRef *root,
						 ProllyNodeRef *next_baseline, bool commit);

	void		(*tables) (const ProllyNodeRef *root, ProllyCanonTableVisit visit,
						   void *arg);
	/* Its materialized views' rows, as tables' (never unreadable). */
	void		(*materialized_views) (const ProllyNodeRef *root,
									   ProllyCanonTableVisit visit, void *arg);
	bool		(*same_schema) (const ProllyNodeRef *a, const ProllyNodeRef *b);
	void		(*objects) (const ProllyNodeRef *from, const ProllyNodeRef *to,
							ProllyCanonObjectVisit visit, void *arg);
	void		(*move_schema) (const ProllyNodeRef *from, const ProllyNodeRef *to,
								void (*rows) (void *arg), void *arg);
	void		(*sequences) (const ProllyNodeRef *root,
							  ProllyCanonSequenceVisit visit, void *arg);
	void		(*fold_dependencies) (ProllyNodeRef *tree);
	void		(*dependencies) (const ProllyNodeRef *tree,
								 ProllyCanonDependencyVisit visit, void *arg);
	void		(*merge_schema) (const ProllyNodeRef *base,
								 const ProllyNodeRef *ours,
								 const ProllyNodeRef *theirs,
								 ProllyNodeRef *merged);
	void		(*walk_baseline) (const ProllyNodeRef *baseline,
								  ProllyTreeVisitNode visit, void *arg);

	/*
	 * A table's description, as tables() gives it, read back: its live
	 * columns in order (ProllyTableColumn *, in the server's encoding), and
	 * whether a primary key keys its rows.
	 */
	List	   *(*table_columns) (const ProllyValue *description, bool *keyed);

	/*
	 * fold() for reading what changed, never for a commit: objects the
	 * canonical form doesn't describe are left out of root, not refused, and
	 * their names appended to *left_out (if not NULL).  With only valid,
	 * only that table's rows are folded: the others' are the baseline's
	 * (none if it hasn't the table as defined now), so root reads right for
	 * that table alone.
	 */
	void		(*read_fold) (const ProllyNodeRef *baseline, Oid only,
							  ProllyNodeRef *root, List **left_out);

	/*
	 * Whether a root is of the format this build folds: one of another is
	 * read as tables() says (its rows not read), and never received from
	 * another store.
	 */
	bool		(*in_format) (const ProllyNodeRef *root);

	/* The format this build folds, as a root's "!:format" item says it. */
	void		(*format) (StringInfo out);

	/*
	 * Visit the nodes root to keeps alive that root from (NULL: none) may
	 * lack, its trees each diffed against from's of the same name: what
	 * changed, not all of to (a transfer sending what a receiver that has
	 * from lacks).
	 */
	void		(*walk_changes) (const ProllyNodeRef *from,
								 const ProllyNodeRef *to,
								 ProllyTreeVisitNode visit, void *arg);

	/*
	 * Drop the objects of the session branch's versioned schemas that the
	 * canonical form doesn't describe (and what depends on them): work no
	 * commit holds, which a move to a commit discards.
	 */
	void		(*discard_undescribed) (void);
} ProllyCanonRoutine;

#define PROLLY_CANON_CALLBACK_NAMES \
	"walk", "fold", "tables", \
	"materialized_views", "same_schema", "objects", "move_schema", "sequences", \
	"fold_dependencies", "dependencies", \
	"merge_schema", "walk_baseline", "table_columns", "read_fold", \
	"in_format", "format", "walk_changes", "discard_undescribed"
#define PROLLY_CANON_FIRST_CALLBACK walk
#define PROLLY_CANON_LAST_CALLBACK discard_undescribed

/*
 * The tree environment of folded canonical trees: the chunk store's,
 * ordered by bytes.
 */
extern void prolly_folded_tree_env(ProllyTreeEnv *env);

static inline const ProllyCanonRoutine *
prolly_canon_lookup(const char *name)
{
	return (const ProllyCanonRoutine *)
		prolly_get_impl(PROLLY_IFACE_CANONICALIZER, name);
}

#endif							/* PROLLY_CANON_H */
