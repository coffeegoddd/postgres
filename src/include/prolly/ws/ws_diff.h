/*-------------------------------------------------------------------------
 *
 * ws_diff.h
 *	  What changed between two root values: schema first, then rows, with columns
 *	  matched by tag.
 *
 * A stream of what changed, for what reads it to make of it what it needs, and one
 * line of text for each so that a test can name what it expects:
 *	object added|dropped|changed <kind> <name>
 *	foreign key|sequence|role added|dropped|changed <name>
 *	table added|dropped <table>
 *	table changed <table>				its settings: anything else of its record
 *	column added|dropped <table>.<name>
 *	column renamed <table>.<old> <new>
 *	column retyped <table>.<name> <old type> -> <new type>
 *	column default <table>.<name>		its default, or how it is generated
 *	column changed <table>.<name>		its not-null, identity or collation
 *	column moved <table>				the columns both have, in another order
 *	key changed <table>
 *	constraint|index added|dropped|changed <table> <name>
 *	data <table> same|changed			whether the row map is the same tree
 *	rows <table> +<inserted> -<deleted> ~<updated>
 *	rewritten <table> <n>				rows whose bytes changed but whose values, in
 *										the columns both sides have, did not
 *
 * A row counts as updated only if a value in a column both sides have changed, so
 * renaming a column changes no row.  Under a changed key rows are not the same rows,
 * so all are inserted and deleted.  A table's large values compare by what names their
 * tree, which is equal if and only if they are.  Reads of a keyed table's changes
 * take no more memory than a row's; a keyless table's whose columns changed, its
 * changed rows'.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/ws/ws_diff.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_WS_DIFF_H
#define PROLLY_WS_DIFF_H

#include "prolly/engine/prolly.h"

typedef enum WsDiffWhat
{
	WS_DIFF_OBJECT,
	WS_DIFF_FOREIGN_KEY,
	WS_DIFF_SEQUENCE,
	WS_DIFF_ROLE,
	WS_DIFF_TABLE,
	WS_DIFF_COLUMN,
	WS_DIFF_COLUMN_RENAMED,
	WS_DIFF_COLUMN_RETYPED,
	WS_DIFF_COLUMN_DEFAULT,
	WS_DIFF_COLUMN_CHANGED,
	WS_DIFF_COLUMNS_MOVED,
	WS_DIFF_KEY,
	WS_DIFF_CONSTRAINT,
	WS_DIFF_INDEX,
	WS_DIFF_DATA,
	WS_DIFF_ROWS,
	WS_DIFF_REWRITTEN,
} WsDiffWhat;

typedef enum WsDiffAction
{
	WS_DIFF_ADDED,
	WS_DIFF_DROPPED,
	WS_DIFF_CHANGED,
} WsDiffAction;

/* One thing that changed.  What it points to is gone when the function returns. */
typedef struct WsDiffEvent
{
	WsDiffWhat	what;
	WsDiffAction action;		/* if it was added, dropped or changed */
	const char *table;			/* a table's name, UTF8, of what is a table's */
	const char *name;			/* a column's, constraint's, index's, or object's */
	const char *kind;			/* an object's */
	const char *from;			/* a column's old type */
	const char *to;				/* a column's new name or type */
	bool		same;			/* of WS_DIFF_DATA */
	uint64		inserted;
	uint64		deleted;
	uint64		updated;		/* of WS_DIFF_ROWS */
	uint64		rewritten;		/* of WS_DIFF_REWRITTEN */
} WsDiffEvent;

typedef void (*WsDiffFn) (void *arg, const WsDiffEvent *event);

typedef struct WsDiffRoutine
{
	const char *name;
	/* What changed from one root value to another, schema first; each table by name. */
	void		(*diff) (ProllyCtx *ctx, const PHash *from, const PHash *to, WsDiffFn fn, void *arg);
} WsDiffRoutine;

extern PGDLLIMPORT const WsDiffRoutine TagsWsDiff;

/* The implementation in use. */
extern PGDLLIMPORT const WsDiffRoutine *WsDiff;

/* An event's line, appended to out. */
extern void ws_diff_line(const WsDiffEvent *event, Buf *out);

#endif							/* PROLLY_WS_DIFF_H */
