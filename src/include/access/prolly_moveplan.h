/*-------------------------------------------------------------------------
 *
 * prolly_moveplan.h
 *	  Move planner interface: what moving a working set from one canonical
 *	  root to another would write, table by table, and what it would cost.
 *
 * A branch's first use copies some branch's working set and moves the
 * copy to its head; the copy that is cheapest to move is the one to make.
 * A planner compares two roots without reading any table: each table the
 * roots hold alike is passed over, each whose rows differ estimated from
 * its trees, each whose definition differs, or only one root has, from
 * the rows the move would write.
 *
 * Contract:
 * - plan() fills *plan with the tables a move from root from to root to
 *   writes (those alike in both are left out), each with what the move
 *   does with it, the rows that differ (an estimate never below the
 *   diff's count), the rows to has, and the estimated cost, in
 *   microseconds; plan->cost is their sum.  Tables are named by their
 *   qualified, server-encoded names, as the canonicalizer's tables()
 *   names them.
 * - With PROLLY_PLAN_RELOADS (the move's working set is new in the
 *   transaction, which no other sees: a branch's first use), a table whose
 *   rows differ may be planned PROLLY_TABLE_RELOAD instead, where emptying
 *   it and loading every row to has (its indexes built once, after) costs
 *   less than writing the rows that differ; not one with a deferrable
 *   unique or exclusion constraint (its rows are rechecked a row at a time,
 *   so it can't be loaded so).
 * - The work is bounded: a table's estimate reads a bounded number of
 *   tree nodes, whatever the size of its diff, and planning stops once
 *   the cost passes bound (plan->over then set, the tables after it not
 *   planned).
 * - It writes nothing, takes no lock, and reads only the session branch's
 *   catalogs (by its tables' names, for what writing their rows costs:
 *   their indexes and constraints).  A table the session branch hasn't is costed as one
 *   with a primary key alone.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_moveplan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_MOVEPLAN_H
#define PROLLY_MOVEPLAN_H

#include "access/prolly_registry.h"
#include "access/prolly_types.h"
#include "nodes/pg_list.h"

/* What a move does with a table. */
typedef enum ProllyTableMoveKind
{
	PROLLY_TABLE_ROWS,			/* the rows that differ written */
	PROLLY_TABLE_RELOAD,		/* emptied, then every row of to's loaded */
	PROLLY_TABLE_REDEFINED,		/* its definition moved, then its rows */
	PROLLY_TABLE_ADDED,			/* to's alone: made, and its rows written */
	PROLLY_TABLE_DROPPED,		/* from's alone: dropped */
} ProllyTableMoveKind;

typedef struct ProllyTableMove
{
	char	   *name;
	ProllyTableMoveKind kind;
	uint64		changes;		/* rows that differ (an estimate) */
	uint64		rows;			/* rows to has */
	double		cost;			/* estimated microseconds */
} ProllyTableMove;

typedef struct ProllyMovePlan
{
	List	   *tables;			/* ProllyTableMove * */
	double		cost;			/* their sum */
	bool		over;			/* stopped once past the bound */
} ProllyMovePlan;

/* plan()'s flags */
#define PROLLY_PLAN_RELOADS		0x01	/* tables may be emptied and loaded */

typedef struct ProllyMovePlannerRoutine
{
	ProllyImplInfo info;

	void		(*plan) (const ProllyNodeRef *from, const ProllyNodeRef *to,
						 int flags, double bound, ProllyMovePlan *plan);
} ProllyMovePlannerRoutine;

#define PROLLY_MOVEPLAN_CALLBACK_NAMES \
	"plan"
#define PROLLY_MOVEPLAN_FIRST_CALLBACK plan
#define PROLLY_MOVEPLAN_LAST_CALLBACK plan

static inline const ProllyMovePlannerRoutine *
prolly_move_planner_lookup(const char *name)
{
	return (const ProllyMovePlannerRoutine *)
		prolly_get_impl(PROLLY_IFACE_MOVE_PLANNER, name);
}

#endif							/* PROLLY_MOVEPLAN_H */
