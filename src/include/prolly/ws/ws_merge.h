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
 * goes too, and so does one on or to a table dropped.  Primary keys that differ, a key
 * both sides changed even alike, and a table both added otherwise are conflicts.
 * A column dropped by one side and changed in a row by the other is a conflict, as in
 * Dolt, though only for a row both changed.  Unlike Dolt, whose checks of one column conflict, checks of other names both stay,
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
 * not, and with rows changed on the side that lacks it is a row conflict.  Both sides
 * reaching one set of rows add up if the columns did not change; if they changed alike,
 * that is one change.
 *
 * Computing those values, and whether a value reads, is what WsEval is for, and the only
 * user code a merge runs.  A default that isn't immutable, or an identity, has no one
 * value for a merge to give: a new row needing it is a generated conflict.  A primary
 * key column of another type is a key conflict, as a key is its values' text.
 *
 * Conflicts, as a line for each, as "<kind> <where>":
 *	object <kind> <name>		both changed an object differently, or one dropped
 *								what the other changed
 *	table-ddl <table>			both changed something of a table's other than a
 *								column differently, what the merge makes is not a
 *								table a record can be, a partition or child lacks
 *								its table's columns, or its name is another's
 *	column <table>.<name>		both changed a column differently, or one dropped
 *								what the other changed
 *	column-name <table>.<name>	two different columns took the same name
 *	key <table>					the primary keys' columns differ, or either's do
 *								from their base's
 *	table <table>				one dropped a table the other changed, or both
 *								added it otherwise
 *	row <table> <key>			both changed a value differently, one deleted a row
 *								the other changed, or a value does not read as the
 *								type the other side gave its column
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
 * What a merge has computed, in the schema of arg's choosing that has what the
 * expressions name, and has the types as merged.  A value is its output text, as a row
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

/* Through WsSql: arg is a WsEvalSchemas, each schema tried in turn. */
extern PGDLLIMPORT const WsEvalRoutine SqlWsEval;

typedef struct WsEvalSchemas
{
	const char *const *schemas;
	int			n;
} WsEvalSchemas;

typedef struct WsMergeRoutine
{
	const char *name;
	void		(*merge) (ProllyCtx *ctx, const PHash *base, const PHash *ours, const PHash *theirs,
						  const WsEvalRoutine *eval, void *eval_arg, WsMergeResult *out);
} WsMergeRoutine;

extern PGDLLIMPORT const WsMergeRoutine TagsWsMerge;

/* The implementation in use. */
extern PGDLLIMPORT const WsMergeRoutine *WsMerge;

#endif							/* PROLLY_WS_MERGE_H */
