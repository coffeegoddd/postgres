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
 * Unlike Dolt, whose checks of one column conflict, checks of other names both stay,
 * as both can hold or be found violated; different privileges to one role conflict,
 * as Dolt has no grants to say otherwise.  Partition bounds that overlap are left for
 * ATTACH to refuse, on restore: a commit's bounds are text, not values.  So too what
 * only Postgres can tell of the whole: a key to columns no longer unique, or of a type
 * no longer alike, a child without a constraint its table now has.  Whoever applies a
 * merge restores it, and what is refused then is what the merge makes.  And both
 * sides reaching one set of a keyless table's rows still add up, as a multiset would,
 * where Dolt, seeing tables the same, keeps one side's.
 *
 * Rows merge by key, and where both sides changed a row, column by column; a keyless
 * table's as a multiset, each side's change in a row's count added.  A row one side
 * changed is moved into the merged columns: a dropped column's value goes, a column
 * the other side added with a default gets it, and a value under a type the other
 * side changed, or changed under one name, must read as that type.  A stored generated column is copied as it is
 * unless a merge makes its row from both sides, or the side wrote it under another
 * expression or without it, when it is computed from the merged values.  Those, a
 * default's value and whether a value reads, are what WsEval is for, and the only user
 * code a merge runs.
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
 *	generated <table> <key>		a default or generated value could not be computed
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
 * expressions name, and has the types as merged.  A value is its output text, as a row holds it.  Each false if it
 * could not be done, with nothing left undone.
 */
typedef struct WsEvalRoutine
{
	const char *name;
	/* An expression, its columns already names, as a value of a type; NULL as null. */
	bool		(*column_default) (void *arg, const char *expr, const char *type, char **out);
	/*
	 * A generated column's value: expr over the columns of names and types,
	 * values[i] each's text, or NULL.
	 */
	bool		(*generated) (void *arg, const char *expr, const char *type, int n,
							  const char *const *names, const char *const *types,
							  const char *const *values, char **out);
	/* Whether text reads as a value of a type. */
	bool		(*reads_as) (void *arg, const char *type, const char *text);
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
