/*-------------------------------------------------------------------------
 *
 * prolly_log_scan.c
 *	  dolt_log(ref) read by a scan that gives each commit as it is found.
 *
 * As a function, dolt_log() gives every commit its ref reaches before the
 * first is read by the query (a function scan keeps a function's rows
 * whole), so dolt_log ... LIMIT 10 reads the whole history.  Planned, the
 * entry is scanned instead by a custom scan pulling dolt_log's rows from
 * the log's stream (prolly_log_begin(): the commits in the log's order, a
 * heap of those found, each read as it is given), so the query reads only
 * as many commits as it takes, their parents with them.  The rows, their
 * order and the errors are the function's.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_log_scan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_log.h"
#include "access/htup_details.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_proc.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "executor/executor.h"
#include "funcapi.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/restrictinfo.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/typcache.h"
#include "optimizer/paths.h"

/* the columns dolt_log gives */
#define LOG_COLUMNS		6

/* ------------------------------------------------------------------------
 * Planning
 * ------------------------------------------------------------------------
 */

/* dolt_log(ref)'s call, if rte is its entry's (with the ref's own value) */
static FuncExpr *
log_call(RangeTblEntry *rte)
{
	RangeTblFunction *rtfunc;
	FuncExpr   *f;

	if (rte->rtekind != RTE_FUNCTION || list_length(rte->functions) != 1 ||
		rte->funcordinality)
		return NULL;
	rtfunc = linitial(rte->functions);
	if (!IsA(rtfunc->funcexpr, FuncExpr))
		return NULL;
	f = (FuncExpr *) rtfunc->funcexpr;
	if (f->funcid != F_DOLT_LOG || list_length(f->args) != 1)
		return NULL;
	return f;
}

bool
prolly_log_rte(RangeTblEntry *rte)
{
	return log_call(rte) != NULL;
}

static const CustomPathMethods log_path_methods;
static const CustomScanMethods log_scan_methods;

/*
 * The order the scan gives its rows in, as pathkeys: commit_order
 * descending, then date descending (then by hash, which the pathkeys
 * leave out), so ORDER BY commit_order DESC needs no sort.
 */
static List *
log_pathkeys(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	TupleDesc	desc = get_expr_result_tupdesc(((RangeTblFunction *) linitial(rte->functions))->funcexpr,
											   false);
	static const AttrNumber order[] = {6, 4};	/* commit_order, date */
	List	   *pathkeys = NIL;

	for (int i = 0; i < lengthof(order); i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, order[i] - 1);
		TypeCacheEntry *tc = lookup_type_cache(att->atttypid, TYPECACHE_GT_OPR);
		Var		   *v;
		List	   *pk;

		if (!OidIsValid(tc->gt_opr))
			break;
		v = makeVar(rel->relid, order[i], att->atttypid, att->atttypmod,
					att->attcollation, 0);
		pk = build_expression_pathkey(root, (Expr *) v, tc->gt_opr, rel->relids,
									  false);
		if (pk == NIL)
			break;
		pathkeys = list_concat_unique_ptr(pathkeys, pk);
	}
	return pathkeys;
}

/*
 * The path, in place of the function scan's (it reads no more commits than
 * the function, and its first rows cost only theirs): the function scan's
 * rows, its cost a row at a time.
 */
bool
prolly_log_set_paths(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	FuncExpr   *f = log_call(rte);
	CustomPath *cp;
	QualCost	qcost;
	Cost		per_row;

	/* (a ref the scan can compute before it reads: not of a later relation) */
	if (contain_volatile_functions(linitial(f->args)) ||
		!bms_is_empty(pull_varnos(root, linitial(f->args))))
		return false;
	cost_qual_eval(&qcost, rel->baserestrictinfo, root);
	per_row = cpu_tuple_cost + qcost.per_tuple + cpu_operator_cost;
	cp = makeNode(CustomPath);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = rel;
	cp->path.pathtarget = rel->reltarget;
	cp->path.param_info = NULL;
	cp->path.parallel_aware = false;
	cp->path.parallel_safe = false;
	cp->path.parallel_workers = 0;
	cp->path.rows = rel->rows;
	cp->path.startup_cost = qcost.startup;
	cp->path.total_cost = qcost.startup + per_row * rel->tuples;
	cp->path.pathkeys = log_pathkeys(root, rel, rte);
	cp->flags = 0;
	cp->custom_paths = NIL;
	cp->custom_private = NIL;
	cp->methods = &log_path_methods;
	add_path(rel, (Path *) cp);
	return true;
}

/*
 * The plan: no scan relation (the entry is a function's), its scan tuple
 * the function's columns; the ref's expression in custom_exprs.
 */
static Plan *
log_plan(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
		 List *tlist, List *clauses, List *custom_plans)
{
	RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);
	RangeTblFunction *rtfunc = linitial(rte->functions);
	CustomScan *cs = makeNode(CustomScan);
	List	   *scan_tlist = NIL;
	TupleDesc	desc = get_expr_result_tupdesc(rtfunc->funcexpr, false);

	for (int i = 1; i <= desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i - 1);

		scan_tlist = lappend(scan_tlist,
							 makeTargetEntry((Expr *) makeVar(rel->relid, i,
															  att->atttypid,
															  att->atttypmod,
															  att->attcollation,
															  0),
											 i, pstrdup(NameStr(att->attname)),
											 false));
	}
	/* (and the row whole, made of them: l, row_to_json(l), l IS NOT NULL) */
	scan_tlist = lappend(scan_tlist,
						 makeTargetEntry((Expr *) makeWholeRowVar(rte, rel->relid, 0, false),
										 desc->natts + 1, NULL, false));
	cs->scan.plan.targetlist = tlist;
	cs->scan.plan.qual = extract_actual_clauses(clauses, false);
	cs->scan.scanrelid = 0;
	cs->flags = best_path->flags;
	cs->custom_plans = NIL;
	cs->custom_exprs = list_make1(copyObject(linitial(log_call(rte)->args)));
	cs->custom_scan_tlist = scan_tlist;
	cs->custom_relids = bms_copy(rel->relids);
	cs->custom_private = NIL;
	cs->methods = &log_scan_methods;
	return (Plan *) cs;
}

static const CustomPathMethods log_path_methods = {
	.CustomName = "LogScan",
	.PlanCustomPath = log_plan,
};

/* ------------------------------------------------------------------------
 * Execution
 * ------------------------------------------------------------------------
 */

typedef struct LogScanState
{
	CustomScanState css;
	TupleDesc	rowdesc;		/* the row's whole: the function's columns */
	ExprState  *ref;
	MemoryContext cxt;			/* the stream's, gone with a rescan */
	ProllyLogStream *stream;
	bool		begun;
} LogScanState;

/*
 * The function's own check, which a function scan makes as it begins: the
 * scan reads what the function would, so one who can't execute it can't.
 */
static void
check_execute(Oid funcid)
{
	AclResult	aclresult = object_aclcheck(ProcedureRelationId, funcid,
											GetUserId(), ACL_EXECUTE);

	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION, get_func_name(funcid));
	InvokeFunctionExecuteHook(funcid);
}

static void
log_begin(CustomScanState *node, EState *estate, int eflags)
{
	LogScanState *ls = (LogScanState *) node;
	CustomScan *cs = (CustomScan *) node->ss.ps.plan;

	TupleDesc	scan = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	TupleDesc	row = CreateTemplateTupleDesc(LOG_COLUMNS);

	check_execute(F_DOLT_LOG);
	/* (the scan's tuple: the function's columns, then the row whole) */
	for (int i = 0; i < LOG_COLUMNS; i++)
		TupleDescCopyEntry(row, i + 1, scan, i + 1);
	TupleDescFinalize(row);
	ls->rowdesc = BlessTupleDesc(row);
	ls->ref = ExecInitExpr(linitial(cs->custom_exprs), &node->ss.ps);
	ls->cxt = AllocSetContextCreate(estate->es_query_cxt, "dolt_log scan",
									ALLOCSET_DEFAULT_SIZES);
}

static TupleTableSlot *
log_next(ScanState *node)
{
	LogScanState *ls = (LogScanState *) node;
	TupleTableSlot *slot = node->ss_ScanTupleSlot;
	MemoryContext old;

	ExecClearTuple(slot);
	old = MemoryContextSwitchTo(ls->cxt);
	if (!ls->begun)
	{
		ExprContext *econtext = node->ps.ps_ExprContext;
		bool		isnull;
		Datum		ref = ExecEvalExpr(ls->ref, econtext, &isnull);

		/* (the function is strict: a NULL ref gives nothing) */
		ls->stream = isnull ? NULL :
			prolly_log_begin(TextDatumGetCString(ref));
		ls->begun = true;
	}
	if (ls->stream != NULL &&
		prolly_log_next(ls->stream, slot->tts_values, slot->tts_isnull))
	{
		slot->tts_values[LOG_COLUMNS] =
			HeapTupleGetDatum(heap_form_tuple(ls->rowdesc, slot->tts_values,
											  slot->tts_isnull));
		slot->tts_isnull[LOG_COLUMNS] = false;
		ExecStoreVirtualTuple(slot);
	}
	MemoryContextSwitchTo(old);
	return slot;
}

static bool
log_recheck(ScanState *node, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
log_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, log_next, log_recheck);
}

static void
log_rescan(CustomScanState *node)
{
	LogScanState *ls = (LogScanState *) node;

	MemoryContextReset(ls->cxt);
	ls->stream = NULL;
	ls->begun = false;
}

static void
log_end(CustomScanState *node)
{
	LogScanState *ls = (LogScanState *) node;

	MemoryContextDelete(ls->cxt);
}

static const CustomExecMethods log_exec_methods = {
	.CustomName = "LogScan",
	.BeginCustomScan = log_begin,
	.ExecCustomScan = log_exec,
	.EndCustomScan = log_end,
	.ReScanCustomScan = log_rescan,
};

static Node *
log_create_state(CustomScan *cs)
{
	LogScanState *ls = palloc0_object(LogScanState);

	NodeSetTag(ls, T_CustomScanState);
	ls->css.methods = &log_exec_methods;
	return (Node *) ls;
}

static const CustomScanMethods log_scan_methods = {
	.CustomName = "LogScan",
	.CreateCustomScanState = log_create_state,
};
