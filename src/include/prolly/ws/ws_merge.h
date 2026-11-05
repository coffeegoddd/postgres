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
 * ATTACH to refuse, on restore: a commit's bounds are text, not values.
 *
 * Conflicts, as a line for each, as "<kind> <where>":
 *	object <kind> <name>		both changed an object differently, or one dropped
 *								what the other changed
 *	table-ddl <table>			both changed something of a table's other than a
 *								column differently, what the merge makes is not a
 *								table a record can be, a partition or child lacks
 *								what its table has, or its name is another's
 *	column <table>.<name>		both changed a column differently, or one dropped
 *								what the other changed
 *	column-name <table>.<name>	two different columns took the same name
 *	key <table>					the primary keys' columns differ, or either's do
 *								from their base's
 *	table <table>				one dropped a table the other changed, or both
 *								added it otherwise
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

typedef struct WsMergeRoutine
{
	const char *name;
	void		(*merge) (ProllyCtx *ctx, const PHash *base, const PHash *ours, const PHash *theirs,
						  WsMergeResult *out);
} WsMergeRoutine;

extern PGDLLIMPORT const WsMergeRoutine TagsWsMerge;

/* The implementation in use. */
extern PGDLLIMPORT const WsMergeRoutine *WsMerge;

#endif							/* PROLLY_WS_MERGE_H */
