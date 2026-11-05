/*-------------------------------------------------------------------------
 *
 * ws_viol.h
 *	  Constraint violations: rows of a merged root that its constraints refuse.
 *
 * A merge can make rows no side had, and rows a constraint one side added refuses.
 * Those are not its conflicts: it is finished, and the rows are for someone to fix.
 * Each is found by asking the rows themselves, with no constraint there to refuse them,
 * in a schema made for that and undone:
 *	foreign key		rows whose referenced row is not there
 *	check			rows for which its expression is false
 *	unique			rows sharing their key with another
 *	not null		rows with that column NULL
 * and an exclusion constraint, which only Postgres can apply, without its rows.  A check
 * or foreign key not valid has none, and a domain's constraints are not applied to rows
 * restored, so not here.  Unique keys compare by their collation and their type's
 * equality, whatever an index's operator class would say.
 *
 * They are held in the root's ~viol map: (table, a zero byte, constraint, a zero byte, the
 * row's key as a conflict prints it, or nothing for a constraint alone) to its kind, so
 * one set is one map.  Only the root's rows say what they are, so a root is rechecked as
 * it stands, whoever fixed what.
 *
 * What a root's rows cannot be, with no constraint on them, is a merge's conflict, and
 * not a violation: a value of no type it now has, two keys alike as 1.0 and 1.00 are, a
 * row outside its partition, an expression that raises for a row.  So is what Postgres
 * refuses of a root with no rows, where no constraint can be violated: a view of a column
 * dropped.  Those are one line, unapplied <what Postgres said>.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/ws/ws_viol.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_WS_VIOL_H
#define PROLLY_WS_VIOL_H

#include "prolly/engine/prolly.h"
#include "prolly/ws/ws_merge.h"

typedef struct WsViolation
{
	char	   *table;
	char	   *constraint;
	char	   *kind;			/* foreign key, check, unique, not null, exclusion */
	char	   *rowkey;			/* NULL for a constraint alone */
} WsViolation;

typedef struct WsViolRoutine
{
	const char *name;

	/*
	 * A root as its rows now are, its ~viol map what they violate, in *out; false, with
	 * what was wrong in *why, palloc'd, if it is one no constraint explains.
	 */
	bool		(*recheck) (ProllyCtx *ctx, const PHash *root, PHash *out, char **why);

	/* What a root holds of them, in key order. */
	void		(*list) (ProllyCtx *ctx, const PHash *root, WsViolation **out, int *n);
} WsViolRoutine;

extern PGDLLIMPORT const WsViolRoutine QueryWsViol;

/* The implementation in use. */
extern PGDLLIMPORT const WsViolRoutine *WsViol;

/*
 * A merge with no conflicts, checked: its root rechecked, or an unapplied conflict added
 * if it is refused.
 */
extern void ws_merge_check(ProllyCtx *ctx, WsMergeResult *result);

#endif							/* PROLLY_WS_VIOL_H */
