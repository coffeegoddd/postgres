/*-------------------------------------------------------------------------
 *
 * ws_rows.h
 *	  A table's rows as its row map, and back.
 *
 * Each table holding rows has its own map, a partition's under its own name; a
 * partitioned table holds none, whatever its partitions do, and only its own
 * rows are a table's when others inherit from it.  So a row moved between
 * partitions is a row gone from one map and new in another.
 *
 * Capture reads a batch at a time, so a table of any size takes the memory of
 * a batch and the values in it.  One set of rows is one map, however they lie in
 * the table and whatever the session's settings.  Two rows whose keys print alike
 * would be one row, so they are refused.
 *
 * Restore writes the rows as held: it computes no stored generated column, fires
 * no trigger and reading a value checks no domain, however deep in it.  An index
 * already on the table is kept up, which runs what it is made of, so whoever wants
 * none of a table's code run makes its indexes after, which run it then.  It does
 * check other rows' keys, deferrable or not, as what is written is all of a state.
 * So only a table's owner may, in a transaction that can
 * write, and a partition's rows go in before it is attached, whose check of its bound
 * then stands behind them; unless the table's own checks imply its bound, as ATTACH
 * then scans nothing, so whatever makes rows from two sides must see to that.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/ws/ws_rows.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_WS_ROWS_H
#define PROLLY_WS_ROWS_H

#include "prolly/engine/prolly.h"
#include "prolly/ws/ws_schema.h"

typedef struct WsRowsRoutine
{
	const char *name;
	/* The table's rows as a row map; the record says what is read. */
	void		(*capture) (ProllyCtx *ctx, const char *schema, const WsTableSchema *s, PHash *map);
	/* A row map's rows put in the table, an empty one of the record's made so. */
	void		(*restore) (ProllyCtx *ctx, const char *schema, const WsTableSchema *s,
							const PHash *map);
} WsRowsRoutine;

extern PGDLLIMPORT const WsRowsRoutine DirectWsRows;

/* The implementation in use. */
extern PGDLLIMPORT const WsRowsRoutine *WsRows;

#endif							/* PROLLY_WS_ROWS_H */
