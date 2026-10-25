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
 *   ("extension hstore"), an extension valued by its version and schema
 *   (and "versioned" after them if the schema is),
 *   another object by its address (its type, names and arguments, as
 *   pg_identify_object_as_address() gives them), each a text[] literal.
 *   Not roles.  It is no part of the root, so what changes outside never
 *   makes a branch's working set differ from its head.
 * - dependencies() visits each item of such a tree, in key order.
 * - sequences() visits each sequence a folded root holds, in name order,
 *   with its value: its last value, and whether it was given out.
 * - merge_schema() merges three folded roots' definitions: each object as
 *   the side that changed it has it; one both changed differently is a
 *   schema conflict, an error.  With kept, a table in such a conflict (both
 *   changed it differently, or one dropped it and the other changed it) is
 *   kept as ours has it instead (gone, if ours dropped it), and its name
 *   (qualified, as the merge's other names are) appended to *kept (char *):
 *   its rows are ours' too, for the caller to leave as they are.
 * - move_schema() makes the session branch's objects defined as a folded
 *   root defines them, from as another (the branch folded now) does: it
 *   changes the definitions, then calls rows(), which writes the tables'
 *   rows, then adds what the rows must satisfy.  A check, not-null or
 *   foreign key constraint it adds to a table the rows written may break
 *   is added unvalidated first, and breaking() (if given) called with them
 *   (ProllyAddedConstraint *): it takes the rows that break a check or
 *   NOT NULL away, which the move then validates, and checks a foreign
 *   key itself (marking it validated), which the move validates only
 *   without breaking().  rewritten says rows() writes every table's rows
 *   as to has them, so those the branch has needn't be kept: a table whose
 *   columns change type loses its rows first rather than converting them.
 * - Identity: a table or column keeps its identity through renames, but a
 *   root, by name, holds none of it.  identify() gives the working set's
 *   (as an identity record a commit keeps beside its root, and an anchor
 *   for the next time); renames(), merge_renames(), rename_description()
 *   and rename_root() read one version's objects under another's names,
 *   and rename_objects() renames the branch's own.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_canon.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_CANON_H
#define PROLLY_CANON_H

#include "access/prolly_fold.h"
#include "access/prolly_registry.h"
#include "access/prolly_tree.h"
#include "access/prolly_types.h"
#include "access/tupdesc.h"
#include "storage/itemptr.h"

/* Visits one dependency a commit records: its name and its value. */
typedef void (*ProllyCanonDependencyVisit) (const char *name,
											const char *value, void *arg);

/*
 * Visits one sequence of a canonical root, with its value (one recorded
 * without one, unreadable where it was folded, isn't visited).
 */
typedef void (*ProllyCanonSequenceVisit) (const char *name, int64 last,
										  bool called, void *arg);

/*
 * Visits one object (not a table) two canonical roots hold differently;
 * object is its kind, its canonical key's first byte (PROLLY_OBJECT_*).
 */
typedef void (*ProllyCanonObjectVisit) (const char *name, ProllyDiffKind kind,
										char object, void *arg);

/*
 * An object two canonical roots hold differently, as a summary of what
 * changed tells it: its name on each side (NULL where it isn't there;
 * both, and different, for one renamed), its kind (PROLLY_OBJECT_*),
 * whether its rows differ (a table's or materialized view's, a sequence's
 * value) and whether its definition does, and its item on each side (NULL
 * where it isn't there), for its definitions.  not_covered: an object a
 * read of a working set left out (from_name its name, the rest unknown).
 */
typedef struct ProllyObjectChange
{
	const char *from_name;
	const char *to_name;
	char		object;
	bool		data_change;
	bool		schema_change;
	const ProllyValue *from_item;
	const ProllyValue *to_item;
	bool		not_covered;
} ProllyObjectChange;

typedef void (*ProllyCanonChangeVisit) (const ProllyObjectChange *change,
										void *arg);

#define PROLLY_OBJECT_TABLE		't'
#define PROLLY_OBJECT_SCHEMA	'n'
#define PROLLY_OBJECT_UNKNOWN	'\0'	/* (one a read left out) */

/*
 * Visits one table of a canonical root.  unreadable is NULL, or why its
 * rows can't be read here (its tree's keys may be in another order than
 * the database's collations give now): its name, data and description
 * are still the root's.
 */
/* Why a root in an older format of prolly's history has unreadable rows. */
#define PROLLY_UNREADABLE_OLDER_FORMAT \
	"version was committed in an older format of prolly's history"

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
	char	   *generated;		/* a generated column's expression, or NULL */
} ProllyTableColumn;

/*
 * A version of a table as its commit's root describes it, read from the
 * root's items alone (table_metadata()): its data tree, its columns (names,
 * types and collations resolved in this database, in the order its rows
 * encode them), and the primary key keying its rows, with each key column's
 * btree operator family, collation and INDOPTION_* options; and its other
 * indexes, alike, each with why it can't be followed here (not a btree, a
 * partial or expression index, an ordering not built in), if it can't.
 */
typedef struct ProllyHistoricalIndex
{
	char	   *name;
	char	   *definition;		/* its statement, as the version has it */
	bool		unique;
	int			nkeys;
	AttrNumber	attnos[INDEX_MAX_KEYS]; /* 1-based, in the table's desc */
	Oid			opfamilies[INDEX_MAX_KEYS];
	Oid			collations[INDEX_MAX_KEYS];
	int16		options[INDEX_MAX_KEYS];
	int			ninclude;
	AttrNumber	include[INDEX_MAX_KEYS];	/* INCLUDE columns */
	const char *unusable;		/* why it can't be followed here, or NULL */
} ProllyHistoricalIndex;

typedef struct ProllyHistoricalTable
{
	ProllyNodeRef data;
	TupleDesc	desc;
	bool		keyed;			/* false: keyed by content */
	int			nkeys;
	AttrNumber	key_attnos[INDEX_MAX_KEYS]; /* 1-based, in desc */
	Oid			opfamilies[INDEX_MAX_KEYS];
	Oid			collations[INDEX_MAX_KEYS];
	int16		options[INDEX_MAX_KEYS];
	List	   *indexes;		/* its other indexes (ProllyHistoricalIndex) */
} ProllyHistoricalTable;

/* A saved named type; enum labels are String nodes in sort order. */
typedef struct ProllyTypeDefinition
{
	char	   *name;
	char	   *definition;
	List	   *labels;
} ProllyTypeDefinition;

/* A transaction's own row changes to a table: its tuple versions' TIDs. */
typedef struct ProllyRowChanges
{
	Oid			relid;
	ItemPointerData *tids;
	int			ntids;
} ProllyRowChanges;

/*
 * A rename that makes a version's names another's: table (qualified,
 * quoted, in the server's encoding) as the version names it; column (its
 * name, unquoted) or NULL for the table itself; to, the name it takes (a
 * table's qualified and quoted).  A list of renames is applied all at
 * once: a swap of two names is two renames.
 */
typedef struct ProllyRename
{
	char	   *table;
	char	   *column;
	char	   *to;
} ProllyRename;

/* A constraint a move adds: its table (qualified, quoted) and name. */
typedef struct ProllyAddedConstraint
{
	char	   *table;
	char	   *name;
	bool		foreign_key;
} ProllyAddedConstraint;

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

	/* NULL name: all tables; otherwise only that qualified, server-encoded name. */
	void		(*tables) (const ProllyNodeRef *root, const char *name,
						   ProllyCanonTableVisit visit, void *arg);
	/* Its materialized views' rows, as tables' (never unreadable). */
	void		(*materialized_views) (const ProllyNodeRef *root,
									   ProllyCanonTableVisit visit, void *arg);
	bool		(*same_schema) (const ProllyNodeRef *a, const ProllyNodeRef *b);
	void		(*objects) (const ProllyNodeRef *from, const ProllyNodeRef *to,
							ProllyCanonObjectVisit visit, void *arg);
	void		(*move_schema) (const ProllyNodeRef *from, const ProllyNodeRef *to,
								void (*rows) (void *arg),
								void (*breaking) (List *added, void *arg),
								void *arg, bool rewritten);
	void		(*sequences) (const ProllyNodeRef *root,
							  ProllyCanonSequenceVisit visit, void *arg);
	void		(*fold_dependencies) (ProllyNodeRef *tree);
	void		(*dependencies) (const ProllyNodeRef *tree,
								 ProllyCanonDependencyVisit visit, void *arg);
	void		(*merge_schema) (const ProllyNodeRef *base,
								 const ProllyNodeRef *ours,
								 const ProllyNodeRef *theirs,
								 ProllyNodeRef *merged, List **kept);
	void		(*walk_baseline) (const ProllyNodeRef *baseline,
								  ProllyTreeVisitNode visit, void *arg);

	/*
	 * A table's description, as tables() gives it, read back: its live
	 * columns in order (ProllyTableColumn *, in the server's encoding), and
	 * whether a primary key keys its rows.
	 */
	List	   *(*table_columns) (const ProllyValue *description, bool *keyed);
	/* Saved named types (ProllyTypeDefinition *, server encoding). */
	List	   *(*type_definitions) (const ProllyNodeRef *root);

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

	/*
	 * head with the current transaction's own row changes to these tables
	 * applied (the change finder's fold_rows()), and these sequences as
	 * they are now, into *root; everything else as head has it.  False if
	 * it can't be made so: a table or sequence head doesn't have, or a
	 * table head describes otherwise than it is now (its rows' encoding
	 * would differ), or rows that can't be applied.
	 */
	bool		(*fold_rows) (const ProllyNodeRef *head,
							  const ProllyRowChanges *changes, int nchanges,
							  const Oid *sequences, int nsequences,
							  ProllyNodeRef *root);

	/*
	 * Optional: baseline advanced past a commit of the transaction's own
	 * row changes (fold_rows() made root of head with them), into *next:
	 * each of those tables whose baseline holds head's rows for it (and
	 * whose finder can advance its state) holding root's instead, the
	 * rest as baseline has them.  False, next undefined, if no table's
	 * could be.
	 */
	bool		(*advance_baseline) (const ProllyNodeRef *baseline,
									 const ProllyNodeRef *head,
									 const ProllyNodeRef *root,
									 const ProllyRowChanges *changes,
									 int nchanges, ProllyNodeRef *next);

	/*
	 * Optional: each object from and to hold differently, tables and the
	 * rest, by name; a table renames (ProllyRename *, of tables, from's
	 * names to to's: renames()'s) says was renamed, dropped by its name in
	 * from and added by its name in to, as one renamed (its rows and
	 * definition perhaps changed too).  A dropped and an added object
	 * other than a table, of a kind whose items are the same (definition
	 * and rows), and the only such pair, is one renamed too.  Read from the
	 * roots' tree diff: equal subtrees skipped.
	 */
	void		(*changes) (const ProllyNodeRef *from, const ProllyNodeRef *to,
							List *renames, ProllyCanonChangeVisit visit,
							void *arg);

	/*
	 * Optional: the statements that make the object of kind object named
	 * name, from its item (changes()'s), appended to out: each ending in
	 * ";", a line each, rendered from the item alone (no catalog read,
	 * nothing run).  The item must be of a root of this build's format.
	 */
	void		(*render) (char object, const char *name, const ProllyValue *item,
						   StringInfo out);

	/*
	 * Optional: into, but for the tables named (qualified, as tables()
	 * names them), which are as from has them (their definitions and
	 * rows), or gone where from hasn't them, into *out.  Refused where a
	 * table named needs an object (a type, a sequence) the two roots hold
	 * differently: the error names them.
	 */
	void		(*splice) (const ProllyNodeRef *into, const ProllyNodeRef *from,
						   List *tables, ProllyNodeRef *out);

	/*
	 * Optional: baseline into, but for the tables named (qualified, as
	 * tables() names them), whose entries are as baseline from has them (or
	 * gone where from hasn't them), into *out: the baseline of a branch
	 * whose copies of those tables were made the copies from's branch has
	 * (their pages and change logs shared), which fold from its entries.
	 */
	void		(*splice_baseline) (const ProllyNodeRef *into,
									const ProllyNodeRef *from, List *tables,
									ProllyNodeRef *out);

	/*
	 * Optional: read_fold() of no table's rows, for reading definitions
	 * alone: every object described as it is now, each table's rows the
	 * baseline's (none if it hasn't the table as defined now).  root is
	 * read for definitions, never rows, nor taken for the working set.
	 */
	void		(*read_definitions) (const ProllyNodeRef *baseline,
									 ProllyNodeRef *root, List **left_out);

	/*
	 * Optional: the rows of the session branch's table (or prolly
	 * materialized view) relid that differ from baseline's, without
	 * folding them: the change finder's changes() from the baseline's data
	 * tree for it as defined now, which is put in *data.  False, having
	 * visited nothing, where the baseline hasn't it as defined now, the
	 * canonical form wouldn't read its rows, or the finder can't: the
	 * caller folds.
	 */
	bool		(*table_changes) (const ProllyNodeRef *baseline, Oid relid,
								  ProllyNodeRef *data,
								  ProllyRowChangeVisit visit, void *arg);

	/*
	 * Optional: whether fewer than limit of table relid's rows differ from
	 * baseline's (true), as table_changes() would find them but from the
	 * change finder's fewer_changes(): no more of its record read than
	 * limit entries, and no fold's work counted.  The baseline's data tree
	 * for the table is put in *data.  False where that isn't known.
	 */
	bool		(*table_fewer_changes) (const ProllyNodeRef *baseline,
										Oid relid, int limit,
										ProllyNodeRef *data);

	/*
	 * Optional: baseline with the current transaction's own row changes to
	 * these tables applied to its rows, and each table's change finder
	 * state advanced past them, into *next: a fold of the transaction's
	 * changes into the baseline as it commits.  A table it can't apply
	 * them to keeps its entry and its finder's record of them.  False,
	 * next undefined, if no table's could be.
	 */
	bool		(*advance_own) (const ProllyNodeRef *baseline,
								const ProllyRowChanges *changes, int nchanges,
								ProllyNodeRef *next);

	/*
	 * Optional: make the versioned schemas names (quoted, char *) in the
	 * session's branch, as a move making them would: each in place of an
	 * ordinary one of its name where a move would replace it, keeping its
	 * owner, privileges and comment; refused where a move would be.  All are
	 * judged against the database as it is before any is made, as a move
	 * judges the schemas it makes.  For a clone, which makes the extensions
	 * in versioned schemas before the move that makes the rest.
	 */
	void		(*make_versioned_schemas) (List *names);

	/*
	 * Optional: visit each table of the session branch's versioned schemas
	 * whose next fold from baseline (NULL: none) would read it in full, with
	 * why: no entry for it, its definition changed past what its rows can be
	 * mapped from, or what its change finder's full_reason() says.  Nothing
	 * is folded or read but the catalogs and the baseline.
	 */
	void		(*pending_full) (const ProllyNodeRef *baseline,
								 ProllyPendingFullVisit visit, void *arg);

	/*
	 * Optional: the identity of the session branch's tables and columns,
	 * into *identity, as a commit records it beside its root; and what to
	 * find it from next time, into *next_anchor (by the branch's own
	 * relations and attributes, which a rename keeps).  With an anchor (a
	 * next_anchor given before), each table and column is found in it, and
	 * one it hasn't, new since, is given its own name's identity (the first
	 * of that name's free).  With none (NULL), each is found by its name in
	 * known (an identity record, NULL: none): a working set just made what
	 * known describes.  Read from the catalogs, no relation opened.  With
	 * only valid, only that table is identified (and next_anchor is NULL).
	 */
	void		(*identify) (const ProllyValue *anchor, const ProllyValue *known,
							 Oid only, StringInfo identity,
							 StringInfo next_anchor);

	/*
	 * Optional: the renames (ProllyRename *) that make from's tables and
	 * columns (by identity record from_identity) have the names to's (by
	 * to_identity) of the same identity have; and that move each other
	 * out of the way of a name to gives an object of another identity, so
	 * that it reads as gone.  An identity record NULL or empty gives every
	 * object its own name's identity.
	 */
	List	   *(*renames) (const ProllyNodeRef *from,
							const ProllyValue *from_identity,
							const ProllyNodeRef *to,
							const ProllyValue *to_identity);

	/*
	 * Optional: the names a merge of three roots (base, ours, theirs; by
	 * their identity records) gives their tables and columns, as renames
	 * of each (renames[i], ProllyRename *), and the merged identity record,
	 * into *identity.  An object renamed on one side takes that side's
	 * name; renamed otherwise on both, or two objects of one name, is a
	 * schema conflict, an error.  A table keep_ours names (char *, as the
	 * merge would name it otherwise) takes ours' names instead, its own
	 * and its columns': a table in a schema conflict, kept as ours has
	 * it.
	 */
	void		(*merge_renames) (const ProllyNodeRef *roots,
								  const ProllyValue *identities,
								  List *keep_ours,
								  List **renames, StringInfo identity);

	/*
	 * Optional: a table's description (as tables() gives it) with its
	 * columns renamed as renames say for table (as the description's root
	 * names it), into *out.
	 */
	void		(*rename_description) (const ProllyValue *description,
									   const char *table, List *renames,
									   StringInfo out);

	/* Optional: root with renames applied to its tables and columns, into *out. */
	void		(*rename_root) (const ProllyNodeRef *root, List *renames,
								ProllyNodeRef *out);

	/*
	 * Optional: rename the session branch's tables and columns as renames
	 * say (as a move does, without user event triggers).
	 */
	void		(*rename_objects) (List *renames);

	/*
	 * Optional: the table name (qualified and quoted, as root names tables)
	 * as root describes it, into *t, from root's items alone: no row is read
	 * or re-sorted, nothing is written, and the session's tables aren't
	 * consulted.  Returns NULL, or why its rows can't be read here as root
	 * stores them, *t then unset: root is in another format; a column's
	 * type or collation isn't one this database has; the key is ordered by
	 * an extension, a collation whose version changed, or an operator class
	 * or type that isn't built in; or the key encoder can't order it.
	 * *found is whether root has the table (NULL is returned if not).
	 */
	const char *(*table_metadata) (const ProllyNodeRef *root, const char *name,
								   ProllyHistoricalTable *t, bool *found);

	/*
	 * Optional: the names of root's tables in schema (qualified and quoted,
	 * in the server's encoding), in root's order, from its items alone.
	 */
	List	   *(*table_names) (const ProllyNodeRef *root, const char *schema);

	/*
	 * Optional: the tables (qualified and quoted, in the server's encoding)
	 * a table as description describes it inherits from, or is a partition
	 * of: its parents, whose rows a read of them reads with its own.
	 */
	List	   *(*table_parents) (const ProllyValue *description);

	/*
	 * Optional: what objects() visits of from and to, read from their tree
	 * diff (equal subtrees skipped, not every object's items merged); and,
	 * if table isn't NULL, each table whose item differs (added, removed or
	 * modified, as the diff has it), by its name, into table.  Both in key
	 * order.
	 */
	void		(*objects_diffed) (const ProllyNodeRef *from,
								   const ProllyNodeRef *to,
								   ProllyCanonObjectVisit visit,
								   ProllyCanonObjectVisit table, void *arg);

	/*
	 * Optional: root with each of its sequences' values as they are now
	 * (as a fold reads them, whatever the session may), into *out; the
	 * rest as root has it.  False, *out undefined, where a sequence it
	 * holds isn't one now by its name.
	 */
	bool		(*sequences_now) (const ProllyNodeRef *root, ProllyNodeRef *out);
} ProllyCanonRoutine;

#define PROLLY_CANON_CALLBACK_NAMES \
	"walk", "fold", "tables", \
	"materialized_views", "same_schema", "objects", "move_schema", "sequences", \
	"fold_dependencies", "dependencies", \
	"merge_schema", "walk_baseline", "table_columns", "type_definitions", "read_fold", \
	"in_format", "format", "walk_changes", "discard_undescribed", "fold_rows", \
	"advance_baseline", "changes", "render", "splice", "splice_baseline", "read_definitions", "table_changes", "table_fewer_changes", "advance_own", \
	"make_versioned_schemas", "pending_full", "identify", "renames", "merge_renames", \
	"rename_description", "rename_root", "rename_objects", "table_metadata", \
	"table_names", "table_parents", "objects_diffed", "sequences_now"
#define PROLLY_CANON_FIRST_CALLBACK walk
#define PROLLY_CANON_LAST_CALLBACK sequences_now

/*
 * While set, the commits (ProllyHash *) a move takes its versions from: a
 * table the move makes is the relation (by OID) a commit of them was made
 * from for its name, as its record says; one it makes again stays the
 * relation it was (prolly_move_kept_relid()).  A table or materialized
 * view the move makes from such a commit is made by the owner the record
 * names (see prolly_move_maker()).
 */
extern PGDLLIMPORT List *prolly_move_provenance;

/*
 * The OID a table the session's move makes now (named relname, in schema
 * relnamespace) is to have, if the branch has no relation of it: the
 * relation its version is of, or the one it was, before it was made again
 * (InvalidOid: a new one).  heap_create_with_catalog() asks it.
 */
extern Oid	prolly_move_kept_relid(const char *relname, Oid relnamespace);

/*
 * While set, the relations (ProllyKeptRelation *) tables a move makes are
 * to be, by name, before any prolly_move_provenance records: a merge's
 * conflict resolution making the other side's relation the branch's.
 */
typedef struct ProllyKeptRelation
{
	char	   *name;			/* qualified, quoted as roots name tables */
	Oid			relid;
	Oid			owner;			/* the role it is made as (InvalidOid: the
								 * move's) */
} ProllyKeptRelation;

/*
 * The role a relation the running move makes from a commit is made by:
 * the owner the commit's record of relations names (or the one
 * prolly_move_kept names), for a table, a materialized view, or a sequence
 * such a table owns; InvalidOid for any other relation, or outside a move.
 * Each statement making it, its rows, the checks of them and its
 * materialized view's query run as that role, under
 * SECURITY_RESTRICTED_OPERATION (prolly_as_maker()), as REFRESH runs a
 * materialized view's owner's query: the role moving runs none of the
 * owner's code, and gains no relation it doesn't own.  The relation gets no
 * grants from its making.
 */
extern Oid	prolly_move_maker(Oid relid);

/* A role switch prolly_as_maker() made, for prolly_as_maker_end(). */
typedef struct ProllyMakerSwitch
{
	bool		switched;
	Oid			user;
	int			sec;
	int			nest;			/* (the GUC level its search_path is set at) */
	Oid			mover;			/* (prolly_move_mover()'s before) */
} ProllyMakerSwitch;

/*
 * The role running the move, while it runs as a maker; else the current
 * user.  Whether row-level security applies to the rows a move writes is
 * the mover's, as it was before makers: running as one restricts the move,
 * it never lets it write what the mover's policies would refuse.
 */
extern Oid	prolly_move_mover(void);

/*
 * Each object of the session's branch a commit may hold, but its prolly
 * tables, partitioned tables and materialized views, and its routines and
 * types: its OID, its owner, its kind ('v' view, 'S' sequence, 'f' foreign
 * table, 'N' schema, 'C' collation) and its name as commits' items name
 * it.  The refs record them with a commit's relations, so that a move
 * making one from the commit makes it as its recorded owner.
 */
extern void prolly_owned_objects(void (*emit) (Oid oid, Oid owner, char kind,
											   const char *name, void *arg),
								 void *arg);

/*
 * The same of the branch's routines ('P') and types ('T'), each emitted,
 * unless *generation is their listing's generation already (unchanged
 * since it was given: false, nothing emitted; 0 is none).  The listing is
 * kept from one call to the next, worked out again only where the caches'
 * invalidations say it changed, and its generation moves each time.
 */
extern bool prolly_owned_routines(uint64 *generation,
								  void (*emit) (Oid oid, Oid owner, char kind,
												const char *name, void *arg),
								  void *arg);

/*
 * Run as relid's maker (prolly_move_maker()), if it has one, until
 * prolly_as_maker_end(): callers end it in a PG_FINALLY().
 */
extern void prolly_as_maker(Oid relid, ProllyMakerSwitch *sw);
extern void prolly_as_maker_end(ProllyMakerSwitch *sw);

/*
 * The role a relation (name) a commit's record of relations lists as owned
 * by owner, then named owner_name, is made by: refused if that role no
 * longer exists, or is named otherwise now (another role, made with the OID
 * once the counter wrapped, or the role renamed).
 */
extern Oid	prolly_recorded_owner(const char *name, Oid owner,
								  const char *owner_name);

extern PGDLLIMPORT List *prolly_move_kept;

/*
 * The tree environment of folded canonical trees: the chunk store's,
 * ordered by bytes.
 */
extern void prolly_folded_tree_env(ProllyTreeEnv *env);

/*
 * The same, for writing a canonical root: its items declare the data trees
 * they name, as references its leaves hold.
 */
extern void prolly_folded_canon_env(ProllyTreeEnv *env);

static inline const ProllyCanonRoutine *
prolly_canon_lookup(const char *name)
{
	return (const ProllyCanonRoutine *)
		prolly_get_impl(PROLLY_IFACE_CANONICALIZER, name);
}

#endif							/* PROLLY_CANON_H */
