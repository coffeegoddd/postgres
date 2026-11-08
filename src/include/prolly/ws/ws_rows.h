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
 * Apply writes as restore does, into a table as it stands, attached or not: what it puts in is a
 * root's rows of the same table, so neither the table's checks nor a partition's bound are asked of
 * them again.  It finds a keyed row by its primary key, and a keyless one by what it prints as, in
 * one scan of the table, holding in memory what it's to take away.
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

	/*
	 * The table's rows, which are from's, made to's, writing only the rows that differ: each of
	 * from's taken away before any of to's is put in, so that one row can have what another had.
	 * As restore writes them, no trigger, rule or domain's check runs, and the table's indexes are
	 * kept.  A row from has and the table doesn't is an error.  The table is as s has it; from's
	 * rows are in was's shape, which may differ from it by the columns other than its key, as a
	 * table altered has them now: a row's fields where the table still has them, and null in
	 * those it added.  A row the target has so is kept as it is.  How many rows were taken away
	 * and put in, a keyed row changed being both, and kept.
	 */
	void		(*apply) (ProllyCtx *ctx, const char *schema, const WsTableSchema *was,
						  const PHash *from, const WsTableSchema *s, const PHash *to,
						  uint64 *deleted, uint64 *inserted, uint64 *kept);
} WsRowsRoutine;

extern PGDLLIMPORT const WsRowsRoutine DirectWsRows;

/* The implementation in use. */
extern PGDLLIMPORT const WsRowsRoutine *WsRows;

/* How many of a keyless row a row map's value says, one or more. */
extern uint64 ws_rows_keyless_count(const uint8 *k, size_t kl, const uint8 *v, size_t vl);

/*
 * Edits of keyless rows (one more of a row, or with del, one fewer) as edits of map: an entry for
 * each row changed, with its count, or gone at none.  Sorts edits and returns how many there now are.
 * Removing more of a row than the map holds, net of what's added in the same call, is an error, as
 * the map isn't what's been told.  What the edits
 * given pointed to is left to the caller's memory context; new values are allocated in it.
 */
extern size_t ws_rows_keyless_merge(ProllyCtx *ctx, const PHash *map, ProllyEdit *edits, size_t n);

#endif							/* PROLLY_WS_ROWS_H */
