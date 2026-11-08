/*-------------------------------------------------------------------------
 *
 * move.h
 *	  A branch's versioned schema moved to a root value in place.
 *
 * The schema's tables come to hold what the root holds, by writing only what differs, in the
 * caller's transaction.  That transaction must have written nothing yet, as its writes are left out
 * of capture: capture is told instead that each table changed has the root's row map, so that as
 * the transaction commits, capture has the root.  Writers of the schema's tables are held off from
 * the start until the transaction ends, and readers go on reading what was there until it commits;
 * its sequences are held off from both.  So are the database's catalog changes, from when capture's
 * root is taken.  An error leaves all of it as it was, sequences too.
 *
 * Capture is told the target's maps, not what the rows written print as, so the target is to hold
 * each value as its type prints it under a statement's settings (prolly/ws/ws_sql.h), as what
 * capture made holds it: its text read back in prints the same.
 *
 * It starts from capture's root as of the moment the writers are held off, so it needs what a
 * commit from capture does (prolly/capture.h): capture_to_trees, READ COMMITTED, and capture to
 * get there within the time given.  It's for the owner of each of the schema's tables and sequences,
 * which is asked before anything's held, as is all a commit from capture asks first.
 * The root is the history store's.
 *
 * What the target doesn't have as the schema has it is dropped, with what depends on it in the
 * schema, and so is a table the target keeps that dropping took a part of, as a column whose type
 * went.  Then what the target has and the schema now lacks is made as restore makes it, a table
 * with all its rows (prolly/ws/ws_restore.h's restore_missing), and each table kept has only the
 * rows that differ written.  A table whose definition differs is made again whole, so far, rather
 * than altered, and so is an object that differs only in its comments, owner or grants, with what
 * depends on it.  A materialized view that held rows is refreshed, after what it reads, as what it's
 * made of may have changed, which runs what it is made of.  Roles are made what the target has them
 * as a checkout makes them, if its roles differ.  A root with constraint violations is refused.
 *
 * An enum's value the target has can't be used in the transaction that adds it, so prepare adds
 * them first, in a transaction of its own; a move without it makes such a type again, and every
 * table using it.
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
	int			made;			/* tables made, or made again, and loaded whole */
	int			dropped;		/* tables the target doesn't have */
} WsMoveStats;

typedef struct WsMoveRoutine
{
	const char *name;

	/*
	 * What a move can't do in its own transaction, as Postgres can't use an enum's value in the
	 * one that adds it: each value the target's enums have and the schema's lack, added where it
	 * goes, in the caller's transaction, to commit before the move.  It only adds, so nothing is
	 * lost if the move is then refused or never made.
	 */
	void		(*prepare) (ProllyCtx *ctx, const char *schema, const PHash *to);
	void		(*move) (ProllyCtx *ctx, const char *schema, const PHash *to, int timeout_ms,
						 WsMoveStats *stats);
} WsMoveRoutine;

extern PGDLLIMPORT const WsMoveRoutine InPlaceWsMove;

/* The implementation in use. */
extern PGDLLIMPORT const WsMoveRoutine *WsMove;

#endif							/* PROLLY_MOVE_H */
