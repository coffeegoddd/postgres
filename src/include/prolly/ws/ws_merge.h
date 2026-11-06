/*-------------------------------------------------------------------------
 *
 * ws_merge.h
 *	  Three-way merge of root values: objects by key, tables field by field.
 *
 * A table's columns merge by tag, its constraints, indexes and their settings by
 * name, its comments and grants by what each is of, and each other setting as a
 * whole: what one side changed is taken, and what both changed alike.  Objects,
 * foreign keys and roles merge by key, and a sequence keeps the further position,
 * by the way it goes.
 *
 * As in Dolt: an index on a column dropped is dropped with it, a foreign key on one
 * goes too, and so does one on or to a table dropped.  Primary keys that differ and a
 * table both added otherwise are conflicts, and so here is a key both sides changed alike.
 * A column dropped by one side and changed in a row by the other is a conflict, as in
 * Dolt.
 *
 * Unlike Dolt: a keyless table has no row to say that of, and the value goes; a delete
 * against a retype whose print differs, or a value changed under one name in a row
 * holding it, is a conflict, and so is one key inserted or one value set under
 * different types, rather than cast; both sides deleting one keyless row while its form
 * changes is a false conflict.  Checks of other names on one column both stay, where
 * Dolt's conflict,
 * as both can hold or be found violated; different privileges to one role conflict,
 * as Dolt has no grants to say otherwise.  Partition bounds that overlap are left for
 * ATTACH to refuse, on restore: a commit's bounds are text, not values.  So too what
 * only Postgres can tell of the whole: a key to columns no longer unique, or of a type
 * no longer alike, a child without a constraint its table now has.  Whoever applies a
 * merge restores it, and what is refused then is what the merge makes.  And both
 * sides reaching one set of a keyless table's rows still add up, as a multiset would,
 * where Dolt, seeing tables the same, keeps one side's.
 *
 * Keyed rows merge by key as each side wrote them, and where both changed a row,
 * column by column: a cell one side left is the other's, one both changed differently a
 * conflict.  Only what is kept is put in the merged form, so a row or cell no one touched
 * is never read: a dropped column's value goes, a value under another type, or one
 * changed under one name, must read as the merged type and is written as that type
 * prints it, not cast.  A row new on a side gets the default of a column only the
 * other has; a row with a base does not, as the side that added the column gave it what
 * it has.  A delete against a row the other side only reshaped stands.  A stored generated
 * column is copied unless its row is made from both sides, or written under another
 * expression or type, when it is computed from the merged values.
 *
 * Keyless rows have no identity but their cells, so all three maps are put in one form
 * and theirs' change in each row's count added to ours', when that form follows from
 * each row alone.  A retype, a value for each row or a type changed under one name does
 * not, and with rows changed on the side that lacks it is a row conflict; a reshape
 * that does follow is a row conflict too where theirs removes what ours does not have,
 * as a default set after a column was added, or where a row holds a value in a column
 * both sides added, unless both reached one set of rows.  Both sides
 * reaching one set of rows add up if the columns did not change; if they changed alike,
 * that is one change.
 *
 * Rows merge only once the schema has: any conflict in it, and no row is looked at, as no
 * value can be worked out with no one schema to mean it.  Computing those values, and
 * whether a value reads, is what WsEval is for, by the merged definitions alone, and the
 * only user code a merge runs; what the merged definitions cannot be made as is an
 * unapplied conflict.  A default that isn't immutable, or an identity, has no one
 * value for a merge to give: a new row needing it is a generated conflict.  A primary
 * key column of another type is a key conflict, as a key is its values' text.  A value
 * is read as a type by its text, not cast: boolean to text gives true beside t, and
 * integer DEFAULT 2.0 is a generated conflict for a new row.  Not seen here, for restoring
 * the merge to say: a column of a table's row type, an enum key renamed against a delete,
 * a multirange whose range's name Postgres clipped to make its name.  Nor can anything
 * here or there see a value renamed to what another was, as the text still reads: it
 * means the other now, keyless rows reaching one set through that too, nor a composite
 * type's attributes reordered.
 *
 * Where this differs from Dolt, whose merge tests its scenarios follow:
 *	- A column of another type on one side, with rows on the other: Dolt conflicts on the
 *	  schema unless it widens.  Here, in a table with a key, each row is read and must print as
 *	  it did, or it is a row conflict.  It is a column conflict if the other side wrote values
 *	  there and any row there before prints otherwise on the side that retyped, as a USING
 *	  would have it, and as an UPDATE too, which no row can tell apart: so a widening with
 *	  rows edited on both sides, which Dolt merges, conflicts here.  What is not seen: a USING
 *	  that leaves every row printing as it did, as with none yet; both retyping alike by
 *	  different USINGs; and a type changed under its name by one.  A keyless table has any
 *	  retype, with rows on the other side, a row conflict.
 *	- A column replaced through a rename, with values the other side wrote: Dolt moves them
 *	  by name; here it is a column conflict.
 *	- A json value both changed: Dolt merges by key; here it is a row conflict.
 *	- A keyless row both added, removed or changed: Dolt conflicts; here the counts add up, as
 *	  a multiset, so a row both changed differently is two rows.
 *	- A conflict in the schema: Dolt still merges other tables' rows; here no row is looked at,
 *	  though a column conflict that rows show is reported beside other tables' row conflicts.
 *	  Nor does a row conflict say what violations there would be.
 *	- A table both added, alike but for tags, as one retyped a column or renamed the table:
 *	  Dolt merges it; here it is a table conflict.
 *	- Defaults compare as Postgres prints them, so 1.23 and '1.23' of a real differ; Dolt's
 *	  are alike.
 *	- A primary key both changed alike: Dolt merges it; here it is a key conflict.
 *	- A row violating not null: Dolt takes it out of the table into its record; here it stays,
 *	  with the constraint set aside (ws_viol.h).
 *	- A default with no one value, as now() or a serial, for a new row on the other side: Dolt
 *	  works it out per row; here it is a generated conflict.
 *	- A sequence past the rows merged, or across branches: Dolt's tracker has that; here a
 *	  sequence goes to the further position alone, so the next value may meet a merged row.
 *
 * Conflicts, as a line for each, as "<kind> <where>":
 *	object <kind> <name>		both changed an object differently, or one dropped
 *								what the other changed
 *	table-ddl <table>			both changed something of a table's other than a
 *								column differently, what the merge makes is not a
 *								table a record can be, a partition or child lacks
 *								its table's columns, or its name is another's
 *	column <table>.<name>		both changed a column differently, one dropped what
 *								the other changed, one retyped it, printing a row
 *								otherwise, or replaced it, and the other wrote there
 *	column-name <table>.<name>	two different columns took the same name
 *	key <table>					the primary keys' columns differ, or either's do
 *								from their base's
 *	table <table>				one dropped a table the other changed, or both
 *								added it otherwise
 *	row <table> <key>			both changed a value differently, one deleted a row
 *								the other changed, or a value does not read as the
 *								type the other side gave its column, or prints
 *								otherwise as that
 *	unapplied <what was said>	Postgres would not have what the merge made
 *	generated <table> <key>		a default or generated value could not be computed, or
 *								a row has none that a merge can give
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/ws/ws_merge.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_WS_MERGE_H
#define PROLLY_WS_MERGE_H

#include "prolly/engine/prolly.h"

/*
 * What a merge made, and what conflicted.  Its root is the merge only if nothing did:
 * otherwise a table in conflict is not in it, and a map's entry in conflict is ours.
 */
typedef struct WsMergeResult
{
	PHash		root;
	int			nconflicts;
	char	  **conflicts;		/* lines, in order, palloc'd */
} WsMergeResult;

/*
 * What a merge has computed, among its merged definitions alone, in a schema it makes
 * the first time one is asked (ws_root_definitions_begin), under its name, as a function's
 * text may say that.  A function reading one of its tables or taking the next of its
 * sequences is refused, as they hold no rows there and hand out nothing.  A function is
 * otherwise as immutable as it says, as Postgres takes it to be for a generated column: what
 * reads a catalog, a table's size, catalog_branch or a sequence's state is told of that
 * schema, and a first merge in a backend moves lastval(), in allocating its branch.
 * A value is its output text, as a row
 * holds it.  Each false if it could not be done, with nothing left undone.
 */
typedef struct WsEvalRoutine
{
	const char *name;
	/*
	 * An expression, its columns already names, as text; NULL as null.  If it is not
	 * immutable, only that, true: it has no one value.
	 */
	bool		(*column_default) (void *arg, const char *expr, char **out, bool *varies);
	/* A generated column's expression over the columns of names, types and values (or NULL). */
	bool		(*generated) (void *arg, const char *expr, int n, const char *const *names,
							  const char *const *types, const char *const *values, char **out);
	/* Whether text reads as a value of a type, not cut to fit, and how it prints as one. */
	bool		(*reads_as) (void *arg, const char *type, const char *text, char **as);
} WsEvalRoutine;

/* Through WsSql: arg is the merge's. */
extern PGDLLIMPORT const WsEvalRoutine SqlWsEval;

typedef struct WsMergeRoutine
{
	const char *name;
	/* schema is the name the root goes by, which what a function's text says may name. */
	void		(*merge) (ProllyCtx *ctx, const PHash *base, const PHash *ours, const PHash *theirs,
						  const char *schema, const WsEvalRoutine *eval, WsMergeResult *out);
} WsMergeRoutine;

extern PGDLLIMPORT const WsMergeRoutine TagsWsMerge;

/* The implementation in use. */
extern PGDLLIMPORT const WsMergeRoutine *WsMerge;

#endif							/* PROLLY_WS_MERGE_H */
