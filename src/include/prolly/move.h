/*-------------------------------------------------------------------------
 *
 * move.h
 *	  A branch's versioned schema moved to a root value in place.
 *
 * The schema's tables come to hold what the root holds, by writing only what differs, in the
 * caller's transaction.  That transaction must have written nothing yet, as its writes are left out
 * of capture: capture is told instead that each table changed has the root's row map, so that as
 * the transaction commits, capture has the root.  Writers of the schema's tables and sequences are
 * held off from the start until the transaction ends, and readers go on reading what was there
 * until it commits.  An error leaves all of it as it was, sequences too.
 *
 * It starts from capture's root as of the moment the writers are held off, so it needs what a
 * commit from capture does (prolly/capture.h): capture_to_trees, READ COMMITTED, and capture to
 * get there within the time given.  Writing the rows is a table's owner's, as restoring them is.
 * The root is the history store's.
 *
 * Only rows and sequences' positions move so far: a root whose tables, their definitions, objects,
 * foreign keys, roles or violations aren't the schema's is refused.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/move.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_MOVE_H
#define PROLLY_MOVE_H

#include "prolly/engine/prolly.h"

/* What a move wrote. */
typedef struct WsMoveStats
{
	int			tables;			/* whose rows changed */
	uint64		deleted;		/* rows taken away, a keyed row changed among them */
	uint64		inserted;		/* and put in */
	int			sequences;		/* whose positions changed */
} WsMoveStats;

typedef struct WsMoveRoutine
{
	const char *name;
	void		(*move) (ProllyCtx *ctx, const char *schema, const PHash *to, int timeout_ms,
						 WsMoveStats *stats);
} WsMoveRoutine;

extern PGDLLIMPORT const WsMoveRoutine InPlaceWsMove;

/* The implementation in use. */
extern PGDLLIMPORT const WsMoveRoutine *WsMove;

#endif							/* PROLLY_MOVE_H */
