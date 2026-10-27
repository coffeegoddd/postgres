/*-------------------------------------------------------------------------
 *
 * prolly_log_count.c
 *	  count(*) of dolt_log(ref) from the commit's recorded count.
 *
 * A branch's kept commit records the commits it reaches, itself included
 * (the refs' commit_count(), prolly_history_commits()), as Dolt's commit
 * closure counts them.  So SELECT count(*) FROM dolt_log(ref), whose rows are those
 * commits while no history is severed, needs no walk of the history: a
 * query whose grouping is count(*) of dolt_log alone is planned as a scan
 * giving the one row, the count prolly_log_count() finds, in place of the
 * aggregate over the log's rows.  A commit no branch keeps with one, or a history
 * horizon (the log stops there), has the log counted as it gives its rows.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_log_count.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_function_scan.h"
#include "access/prolly_log_count.h"
#include "executor/executor.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"

static const CustomPathMethods count_path_methods;
static const CustomScanMethods count_scan_methods;

/* ------------------------------------------------------------------------
 * Planning
 * ------------------------------------------------------------------------
 */

/* dolt_log(ref)'s ref, if rel is the scan of its entry alone */
static Expr *
log_ref(PlannerInfo *root, RelOptInfo *rel)
{
	RangeTblEntry *rte;
	RangeTblFunction *rtfunc;
	FuncExpr   *f;

	if (rel->reloptkind != RELOPT_BASEREL || rel->baserestrictinfo != NIL ||
		!bms_is_empty(rel->lateral_relids))
		return NULL;
	rte = planner_rt_fetch(rel->relid, root);
	if (rte->rtekind != RTE_FUNCTION || list_length(rte->functions) != 1 ||
		rte->funcordinality)
		return NULL;
	rtfunc = linitial(rte->functions);
	if (!IsA(rtfunc->funcexpr, FuncExpr))
		return NULL;
	f = (FuncExpr *) rtfunc->funcexpr;
	if (f->funcid != F_DOLT_LOG || list_length(f->args) != 1)
		return NULL;
	/* (a ref computed once, before the count: not volatile, of no row) */
	if (contain_volatile_functions(linitial(f->args)) ||
		contain_var_clause(linitial(f->args)))
		return NULL;
	return linitial(f->args);
}

/* Is every aggregate in exprs count(*), and nothing outside one a column? */
static bool
counts_alone(List *exprs, Aggref **count)
{
	List	   *found = pull_var_clause((Node *) exprs,
										PVC_INCLUDE_AGGREGATES |
										PVC_INCLUDE_WINDOWFUNCS |
										PVC_INCLUDE_PLACEHOLDERS);

	*count = NULL;
	foreach_ptr(Node, n, found)
	{
		Aggref	   *a = (Aggref *) n;

		if (!IsA(n, Aggref) || a->aggfnoid != F_COUNT_ || !a->aggstar ||
			a->aggfilter != NULL || a->aggdistinct != NIL ||
			a->aggorder != NIL || a->agglevelsup != 0 ||
			a->aggsplit != AGGSPLIT_SIMPLE)
			return false;
		*count = a;
	}
	return *count != NULL;
}

void
prolly_log_count_paths(PlannerInfo *root, RelOptInfo *input_rel,
					   RelOptInfo *grouped_rel)
{
	Query	   *parse = root->parse;
	Expr	   *ref;
	Aggref	   *count;
	CustomPath *cp;

	if (parse->groupClause != NIL || parse->groupingSets != NIL ||
		parse->havingQual != NULL || !parse->hasAggs)
		return;
	if ((ref = log_ref(root, input_rel)) == NULL ||
		!counts_alone(grouped_rel->reltarget->exprs, &count))
		return;
	cp = makeNode(CustomPath);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = grouped_rel;
	cp->path.pathtarget = grouped_rel->reltarget;
	cp->path.param_info = NULL;
	cp->path.parallel_aware = false;
	cp->path.parallel_safe = false;
	cp->path.parallel_workers = 0;
	cp->path.rows = 1;
	cp->path.startup_cost = 0;
	cp->path.total_cost = cpu_tuple_cost;
	cp->path.pathkeys = NIL;
	cp->flags = 0;
	cp->custom_paths = NIL;
	cp->custom_private = list_make2(ref, count);
	cp->methods = &count_path_methods;
	add_path(grouped_rel, (Path *) cp);
}

/*
 * The plan: no scan relation, its scan tuple the count (count(*), which the
 * target list's references to it are made of); the ref's expression in
 * custom_exprs.
 */
static Plan *
count_plan(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
		   List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cs = makeNode(CustomScan);

	cs->scan.plan.targetlist = tlist;
	cs->scan.plan.qual = NIL;
	cs->scan.scanrelid = 0;
	cs->flags = best_path->flags;
	cs->custom_plans = NIL;
	cs->custom_exprs = list_make1(copyObject(linitial(best_path->custom_private)));
	cs->custom_scan_tlist =
		list_make1(makeTargetEntry(copyObject(lsecond(best_path->custom_private)),
								   1, NULL, false));
	cs->custom_private = NIL;
	cs->methods = &count_scan_methods;
	return (Plan *) cs;
}

static const CustomPathMethods count_path_methods = {
	.CustomName = "LogCount",
	.PlanCustomPath = count_plan,
};

/* ------------------------------------------------------------------------
 * Execution
 * ------------------------------------------------------------------------
 */

typedef struct CountScanState
{
	CustomScanState css;
	ExprState  *ref;
	bool		given;
} CountScanState;

static void
count_begin(CustomScanState *node, EState *estate, int eflags)
{
	CountScanState *cs = (CountScanState *) node;
	CustomScan *plan = (CustomScan *) node->ss.ps.plan;

	prolly_function_scan_check(F_DOLT_LOG);
	cs->ref = ExecInitExpr(linitial(plan->custom_exprs), &node->ss.ps);
}

static TupleTableSlot *
count_next(ScanState *node)
{
	CountScanState *cs = (CountScanState *) node;
	TupleTableSlot *slot = node->ss_ScanTupleSlot;
	ExprContext *econtext = node->ps.ps_ExprContext;
	bool		isnull;
	Datum		ref;

	ExecClearTuple(slot);
	if (cs->given)
		return slot;
	cs->given = true;
	ref = ExecEvalExpr(cs->ref, econtext, &isnull);
	/* (the function is strict: a NULL ref gives no rows) */
	slot->tts_values[0] =
		Int64GetDatum(isnull ? 0 : prolly_log_count(TextDatumGetCString(ref)));
	slot->tts_isnull[0] = false;
	return ExecStoreVirtualTuple(slot);
}

static bool
count_recheck(ScanState *node, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
count_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, count_next, count_recheck);
}

static void
count_rescan(CustomScanState *node)
{
	((CountScanState *) node)->given = false;
}

static void
count_end(CustomScanState *node)
{
}

static const CustomExecMethods count_exec_methods = {
	.CustomName = "LogCount",
	.BeginCustomScan = count_begin,
	.ExecCustomScan = count_exec,
	.EndCustomScan = count_end,
	.ReScanCustomScan = count_rescan,
};

static Node *
count_create_state(CustomScan *cs)
{
	CountScanState *ss = palloc0_object(CountScanState);

	NodeSetTag(ss, T_CustomScanState);
	ss->css.methods = &count_exec_methods;
	return (Node *) ss;
}

static const CustomScanMethods count_scan_methods = {
	.CustomName = "LogCount",
	.CreateCustomScanState = count_create_state,
};
