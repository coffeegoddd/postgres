/*-------------------------------------------------------------------------
 *
 * prolly_history_scan.c
 *	  dolt_history_table(NULL::t) read by the rows its primary key's quals
 *	  bound: a scan of its own that seeks each commit's data tree.
 *
 * As a function, dolt_history_table() reads every row of every commit the
 * head reaches, the query's quals filtering them after.  Planned where the
 * quals compare its row's primary key columns ((row).id = 5, (row).id IN
 * (...), (row).id < 10, an equality on the key's leading columns and then
 * a range on the next) to values known before the scan (constants,
 * parameters, an outer row's), the entry is scanned instead by a custom
 * scan: the values are computed once, and each commit is read by seeking
 * its data tree for each bound (the diff-merge's bounded_rows()), O(the
 * tree's depth) a bound, and a commit whose table is the tree the commit
 * read before it had reads nothing and gives that commit's rows again.
 * So a key's history costs the commits times the trees' depth, not their
 * rows.  A commit whose version can't be sought so (keyed otherwise, or
 * defined otherwise than the table is now) is read whole.  The quals all
 * stay the scan's filter, so a bound is only ever an optimization; the
 * rows, and the errors, are the function's.  EXPLAIN shows the quals the
 * scan bounds by ("History Key").
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_history_scan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/prolly_history.h"
#include "access/stratnum.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_index.h"
#include "catalog/pg_proc_d.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_proc.h"
#include "utils/acl.h"
#include "executor/executor.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/restrictinfo.h"
#include "utils/array.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/tuplestore.h"

/* ------------------------------------------------------------------------
 * Planning
 * ------------------------------------------------------------------------
 */

/* The table dolt_history_table(tbl)'s entry reads (InvalidOid: not one). */
static Oid
history_relid(RangeTblEntry *rte)
{
	RangeTblFunction *rtfunc;
	FuncExpr   *f;
	Node	   *arg;

	if (rte->rtekind != RTE_FUNCTION || list_length(rte->functions) != 1 ||
		rte->funcordinality)
		return InvalidOid;
	rtfunc = linitial(rte->functions);
	if (!IsA(rtfunc->funcexpr, FuncExpr))
		return InvalidOid;
	f = (FuncExpr *) rtfunc->funcexpr;
	if (f->funcid != F_DOLT_HISTORY_TABLE_ANYELEMENT || list_length(f->args) != 1)
		return InvalidOid;
	arg = linitial(f->args);
	if (!IsA(arg, Const))
		return InvalidOid;
	return get_typ_typrelid(exprType(arg));
}

bool
prolly_history_rte(RangeTblEntry *rte)
{
	return OidIsValid(history_relid(rte));
}

/* The table's primary key, as the scan bounds it. */
typedef struct HistoryKey
{
	int			ncolumns;
	AttrNumber *attnos;			/* the table's */
	Oid		   *opfamilies;
	Oid		   *opcintypes;
	Oid		   *collations;
} HistoryKey;

/*
 * relid's primary key, if a btree one of plain ascending columns (whose
 * bounds sort as its keys); false if none.
 */
static bool
history_key(Oid relid, HistoryKey *key)
{
	Relation	rel = table_open(relid, AccessShareLock);
	Oid			pk = RelationGetPrimaryKeyIndex(rel, false);
	Relation	ix;
	bool		ok;

	table_close(rel, AccessShareLock);
	if (!OidIsValid(pk))
		return false;
	ix = index_open(pk, AccessShareLock);
	ok = ix->rd_rel->relam == BTREE_AM_OID;
	key->ncolumns = IndexRelationGetNumberOfKeyAttributes(ix);
	key->attnos = palloc_array(AttrNumber, key->ncolumns);
	key->opfamilies = palloc_array(Oid, key->ncolumns);
	key->opcintypes = palloc_array(Oid, key->ncolumns);
	key->collations = palloc_array(Oid, key->ncolumns);
	for (int c = 0; ok && c < key->ncolumns; c++)
	{
		key->attnos[c] = ix->rd_index->indkey.values[c];
		key->opfamilies[c] = ix->rd_opfamily[c];
		key->opcintypes[c] = ix->rd_opcintype[c];
		key->collations[c] = ix->rd_indcollation[c];
		ok = key->attnos[c] > 0 && ix->rd_indoption[c] == 0;
	}
	index_close(ix, AccessShareLock);
	return ok;
}

/* Is node the entry's row's column attno: (row).column? */
static bool
row_column(Node *node, Index relid, AttrNumber attno)
{
	FieldSelect *fs;
	Var		   *v;

	if (IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;
	if (!IsA(node, FieldSelect))
		return false;
	fs = (FieldSelect *) node;
	if (!IsA(fs->arg, Var))
		return false;
	v = (Var *) fs->arg;
	return v->varno == relid && v->varlevelsup == 0 && v->varattno == 1 &&
		fs->fieldnum == attno;
}

/* Is expr computed before the scan reads a row: none of the entry's, not volatile? */
static bool
known_before(Node *expr, Index relid)
{
	return !bms_is_member(relid, pull_varnos(NULL, expr)) &&
		!contain_volatile_functions(expr);
}

/*
 * A restriction clause that bounds key column c: (row).column op value or
 * value op (row).column (commuted), or (row).column = ANY (array), with
 * the operator the column's operator family's, of the column's type and
 * collation; the clause as column op value (*strategy its strategy, *in
 * whether it is = ANY), else NULL.
 */
static Expr *
key_clause(RelOptInfo *rel, const HistoryKey *key, int c, RestrictInfo *rinfo,
		   int *strategy, bool *in)
{
	Node	   *left;
	Node	   *right;
	Oid			opno;
	Oid			collid;
	Oid			lefttype;
	Oid			righttype;
	Expr	   *clause = rinfo->clause;

	if (rinfo->pseudoconstant)
		return NULL;
	*in = false;
	if (IsA(clause, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *sa = (ScalarArrayOpExpr *) clause;

		if (!sa->useOr || list_length(sa->args) != 2)
			return NULL;
		left = linitial(sa->args);
		right = lsecond(sa->args);
		opno = sa->opno;
		collid = sa->inputcollid;
		*in = true;
	}
	else if (IsA(clause, OpExpr) && list_length(((OpExpr *) clause)->args) == 2)
	{
		OpExpr	   *op = (OpExpr *) clause;

		left = linitial(op->args);
		right = lsecond(op->args);
		opno = op->opno;
		collid = op->inputcollid;
		if (!row_column(left, rel->relid, key->attnos[c]))
		{
			Node	   *t = left;

			left = right;
			right = t;
			opno = get_commutator(opno);
			if (!OidIsValid(opno))
				return NULL;
		}
	}
	else
		return NULL;
	if (!row_column(left, rel->relid, key->attnos[c]) ||
		!known_before(right, rel->relid) ||
		!op_in_opfamily(opno, key->opfamilies[c]))
		return NULL;
	get_op_opfamily_properties(opno, key->opfamilies[c], false, strategy,
							   &lefttype, &righttype);
	/* (the value of the column's own type, so it bounds its keys as it is) */
	if (lefttype != key->opcintypes[c] || righttype != key->opcintypes[c] ||
		(OidIsValid(key->collations[c]) && collid != key->collations[c]) ||
		(*in && *strategy != BTEqualStrategyNumber))
		return NULL;
	if (*in)
	{
		ScalarArrayOpExpr *sa = copyObject((ScalarArrayOpExpr *) clause);

		sa->opno = opno;
		sa->opfuncid = get_opcode(opno);
		sa->args = list_make2(left, right);
		return (Expr *) sa;
	}
	return make_opclause(opno, BOOLOID, false, (Expr *) left, (Expr *) right,
						 InvalidOid, collid);
}

static const CustomPathMethods history_path_methods;
static const CustomScanMethods history_scan_methods;

/*
 * The path: where the restriction clauses bound the primary key's leading
 * columns (equalities, at most one of them an IN list, then a range on the
 * next column), a scan seeking each commit.  Cheaper than the function's
 * scan of every row, which stays a path too.  custom_private: the table,
 * then each bounding clause's column and strategy, then the clauses.
 */
void
prolly_history_set_paths(PlannerInfo *root, RelOptInfo *rel,
						 RangeTblEntry *rte)
{
	Oid			relid = history_relid(rte);
	HistoryKey	key;
	List	   *keys = NIL;
	List	   *clauses = NIL;
	bool		have_in = false;
	CustomPath *cp;

	if (!history_key(relid, &key))
		return;
	for (int c = 0; c < key.ncolumns; c++)
	{
		bool		equal = false;
		bool		lo = false;
		bool		hi = false;

		foreach_ptr(RestrictInfo, rinfo, rel->baserestrictinfo)
		{
			int			strategy;
			bool		in;
			Expr	   *clause = key_clause(rel, &key, c, rinfo, &strategy, &in);

			if (clause == NULL || equal || (in && have_in))
				continue;
			if (strategy == BTEqualStrategyNumber)
			{
				equal = true;
				have_in |= in;
			}
			else if (strategy == BTLessStrategyNumber ||
					 strategy == BTLessEqualStrategyNumber)
			{
				if (hi)
					continue;
				hi = true;
			}
			else if (lo)
				continue;
			else
				lo = true;
			keys = lappend(keys, list_make5_int(c, strategy, in,
												(int) key.opfamilies[c],
												(int) key.opcintypes[c]));
			llast(keys) = lappend_int(llast(keys), (int) key.collations[c]);
			clauses = lappend(clauses, clause);
		}
		/* (the ranges' column ends the bounds; so does a column unbounded) */
		if (!equal)
		{
			/* (a range's column: its equalities were none) */
			break;
		}
		/* (an equality's ranges on the same column dropped) */
		{
			List	   *k2 = NIL;
			List	   *c2 = NIL;
			ListCell   *a;
			ListCell   *b;

			forboth(a, keys, b, clauses)
			{
				List	   *k = lfirst(a);

				if (linitial_int(k) == c && lsecond_int(k) != BTEqualStrategyNumber)
					continue;
				k2 = lappend(k2, k);
				c2 = lappend(c2, lfirst(b));
			}
			keys = k2;
			clauses = c2;
		}
	}
	if (keys == NIL)
		return;

	cp = makeNode(CustomPath);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = rel;
	cp->path.pathtarget = rel->reltarget;
	cp->path.param_info = NULL;
	cp->path.parallel_aware = false;
	cp->path.parallel_safe = false;
	cp->path.parallel_workers = 0;
	cp->path.rows = rel->rows;
	/* (a seek a bound and commit, against every row of every commit) */
	cp->path.startup_cost = 0;
	cp->path.total_cost = cpu_operator_cost * (1 + list_length(keys)) +
		cpu_tuple_cost * rel->rows;
	cp->path.pathkeys = NIL;
	cp->flags = 0;
	cp->custom_paths = NIL;
	cp->custom_private = list_make3(makeInteger((int) relid), keys, clauses);
	cp->methods = &history_path_methods;
	add_path(rel, (Path *) cp);
}

/*
 * The plan: no scan relation (the entry is a function's), its scan tuple
 * the function's columns (custom_scan_tlist); the bounding clauses in
 * custom_exprs, so that their values are fixed up as any expression's.
 * custom_private: the table, then each clause's column, strategy, whether
 * it is an IN list, and its key column's operator family, type and
 * collation.
 */
static Plan *
history_plan(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
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
		Var		   *v = makeVar(rel->relid, i, att->atttypid, att->atttypmod,
								att->attcollation, 0);

		scan_tlist = lappend(scan_tlist,
							 makeTargetEntry((Expr *) v, i,
											 pstrdup(NameStr(att->attname)),
											 false));
	}
	/* (and the row whole, made of them: h, row_to_json(h), h IS NOT NULL) */
	scan_tlist = lappend(scan_tlist,
						 makeTargetEntry((Expr *) makeWholeRowVar(rte, rel->relid, 0, false),
										 desc->natts + 1, NULL, false));
	cs->scan.plan.targetlist = tlist;
	cs->scan.plan.qual = extract_actual_clauses(clauses, false);
	cs->scan.scanrelid = 0;
	cs->flags = best_path->flags;
	cs->custom_plans = NIL;
	cs->custom_exprs = copyObject(lthird(best_path->custom_private));
	cs->custom_scan_tlist = scan_tlist;
	cs->custom_relids = bms_copy(rel->relids);
	cs->custom_private = list_make2(linitial(best_path->custom_private),
									lsecond(best_path->custom_private));
	cs->methods = &history_scan_methods;
	return (Plan *) cs;
}

static const CustomPathMethods history_path_methods = {
	.CustomName = "HistoryScan",
	.PlanCustomPath = history_plan,
};

/* ------------------------------------------------------------------------
 * Execution
 * ------------------------------------------------------------------------
 */

typedef struct HistoryScanState
{
	CustomScanState css;
	Oid			relid;
	List	   *keys;			/* each clause's column, strategy, IN, and the
								 * key column's operator family, type and
								 * collation */
	List	   *values;			/* each clause's value (ExprState) */
	TupleDesc	rowdesc;		/* the function's columns: the row whole's */
	TupleTableSlot *row;		/* a row of the store, in rowdesc */
	bool		whole;			/* is the row whole used? */
	Tuplestorestate *store;
	bool		read;
} HistoryScanState;

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
history_begin(CustomScanState *node, EState *estate, int eflags)
{
	HistoryScanState *hs = (HistoryScanState *) node;
	CustomScan *cs = (CustomScan *) node->ss.ps.plan;

	TupleDesc	scan = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	int			ncolumns = scan->natts - 1;
	Bitmapset  *used = NULL;

	check_execute(F_DOLT_HISTORY_TABLE_ANYELEMENT);
	/*
	 * (the scan's tuple: the function's columns, then the row whole, formed
	 * from them only if the plan reads it)
	 */
	hs->rowdesc = CreateTemplateTupleDesc(ncolumns);
	for (int i = 0; i < ncolumns; i++)
		TupleDescCopyEntry(hs->rowdesc, i + 1, scan, i + 1);
	TupleDescFinalize(hs->rowdesc);
	hs->rowdesc = BlessTupleDesc(hs->rowdesc);
	hs->row = ExecInitExtraTupleSlot(estate, hs->rowdesc, &TTSOpsMinimalTuple);
	pull_varattnos((Node *) cs->scan.plan.targetlist, INDEX_VAR, &used);
	pull_varattnos((Node *) cs->scan.plan.qual, INDEX_VAR, &used);
	hs->whole = bms_is_member(scan->natts - FirstLowInvalidHeapAttributeNumber,
							  used);
	hs->relid = (Oid) intVal(linitial(cs->custom_private));
	hs->keys = lsecond(cs->custom_private);
	hs->values = NIL;
	foreach_ptr(Expr, clause, cs->custom_exprs)
	{
		List	   *args = IsA(clause, ScalarArrayOpExpr) ?
			((ScalarArrayOpExpr *) clause)->args : ((OpExpr *) clause)->args;

		hs->values = lappend(hs->values,
							 ExecInitExpr((Expr *) lsecond(args), &node->ss.ps));
	}
}

/* values[] sorted by the key column's btree order, for distinct_values() */
typedef struct ValueOrder
{
	FmgrInfo	cmp;
	Oid			collation;
} ValueOrder;

static int
value_cmp(const void *a, const void *b, void *arg)
{
	ValueOrder *vo = arg;

	return DatumGetInt32(FunctionCall2Coll(&vo->cmp, vo->collation,
										   *(const Datum *) a,
										   *(const Datum *) b));
}

/*
 * values[] (n of them) sorted by the key column's btree order (its
 * operator family's comparison of type, with its collation), each set of
 * equal ones made one: two values equal as the key compares them (5 and 5,
 * 1.0 and 1.00, citext's 'abc' and 'ABC') bound the same keys, which read
 * twice would give their rows twice.  How many are left.
 */
static int
distinct_values(Datum *values, int n, Oid opfamily, Oid type, Oid collation)
{
	ValueOrder	vo;
	int			kept = 0;

	if (n <= 1)
		return n;
	fmgr_info(get_opfamily_proc(opfamily, type, type, BTORDER_PROC), &vo.cmp);
	vo.collation = collation;
	qsort_arg(values, n, sizeof(Datum), value_cmp, &vo);
	for (int i = 0; i < n; i++)
		if (kept == 0 || value_cmp(&values[kept - 1], &values[i], &vo) != 0)
			values[kept++] = values[i];
	return kept;
}

/*
 * The bounds the clauses' values make, into *bounds (how many returned):
 * each IN list's distinct value an equality bound of its own (so no two
 * bounds overlap: they differ in that column's value); none if a value is
 * NULL (no key equals or is bounded by it).
 */
static int
history_bounds(HistoryScanState *hs, ExprContext *econtext,
			   ProllyKeyBounds **bounds)
{
	int			neq = 0;
	Datum	   *eq;
	Datum	   *in_values = NULL;
	int			nin = -1;		/* (no IN list) */
	int			in_column = -1;
	ProllyKeyBounds b = {0};
	ListCell   *a;
	ListCell   *v;
	int			n = 0;

	forboth(a, hs->keys, v, hs->values)
		if (lsecond_int((List *) lfirst(a)) == BTEqualStrategyNumber)
			neq++;
	eq = palloc0_array(Datum, Max(neq, 1));
	forboth(a, hs->keys, v, hs->values)
	{
		List	   *k = lfirst(a);
		int			c = linitial_int(k);
		int			strategy = lsecond_int(k);
		bool		isnull;
		Datum		d = ExecEvalExpr(lfirst(v), econtext, &isnull);

		if (isnull)
			return 0;
		if (lthird_int(k))
		{
			ArrayType  *arr = DatumGetArrayTypeP(d);
			int16		typlen;
			bool		typbyval;
			char		typalign;
			bool	   *nulls;
			Datum	   *elems;
			int			nelems;

			get_typlenbyvalalign(ARR_ELEMTYPE(arr), &typlen, &typbyval, &typalign);
			deconstruct_array(arr, ARR_ELEMTYPE(arr), typlen, typbyval, typalign,
							  &elems, &nulls, &nelems);
			in_values = palloc_array(Datum, Max(nelems, 1));
			nin = 0;
			for (int i = 0; i < nelems; i++)
				if (!nulls[i])
					in_values[nin++] = elems[i];
			in_column = c;
			nin = distinct_values(in_values, nin, (Oid) list_nth_int(k, 3),
								  (Oid) list_nth_int(k, 4),
								  (Oid) list_nth_int(k, 5));
			continue;
		}
		if (strategy == BTEqualStrategyNumber)
			eq[c] = d;
		else if (strategy == BTLessStrategyNumber ||
				 strategy == BTLessEqualStrategyNumber)
		{
			b.has_hi = true;
			b.hi = d;
			b.hi_strict = strategy == BTLessStrategyNumber;
		}
		else
		{
			b.has_lo = true;
			b.lo = d;
			b.lo_strict = strategy == BTGreaterStrategyNumber;
		}
	}
	b.neq = neq;
	if (nin == 0)
		return 0;
	*bounds = palloc_array(ProllyKeyBounds, nin < 0 ? 1 : nin);
	for (int i = 0; i < (nin < 0 ? 1 : nin); i++)
	{
		(*bounds)[n] = b;
		(*bounds)[n].eq = palloc_array(Datum, Max(neq, 1));
		memcpy((*bounds)[n].eq, eq, sizeof(Datum) * neq);
		if (nin >= 0)
			(*bounds)[n].eq[in_column] = in_values[i];
		n++;
	}
	return n;
}

static TupleTableSlot *
history_next(ScanState *node)
{
	HistoryScanState *hs = (HistoryScanState *) node;
	TupleTableSlot *slot = node->ss_ScanTupleSlot;

	if (!hs->read)
	{
		ExprContext *econtext = node->ps.ps_ExprContext;
		ProllyKeyBounds *bounds = NULL;
		int			nbounds;
		ReturnSetInfo rsinfo;
		MemoryContext old;

		ResetExprContext(econtext);
		old = MemoryContextSwitchTo(econtext->ecxt_per_query_memory);
		hs->store = tuplestore_begin_heap(false, false, work_mem);
		nbounds = history_bounds(hs, econtext, &bounds);
		if (nbounds > 0)
		{
			memset(&rsinfo, 0, sizeof(rsinfo));
			rsinfo.type = T_ReturnSetInfo;
			rsinfo.setResult = hs->store;
			rsinfo.setDesc = hs->rowdesc;
			prolly_history_rows(hs->relid, bounds, nbounds, &rsinfo);
		}
		MemoryContextSwitchTo(old);
		hs->read = true;
	}
	ExecClearTuple(slot);
	if (tuplestore_gettupleslot(hs->store, true, false, hs->row))
	{
		int			ncolumns = hs->rowdesc->natts;

		slot_getallattrs(hs->row);
		memcpy(slot->tts_values, hs->row->tts_values, ncolumns * sizeof(Datum));
		memcpy(slot->tts_isnull, hs->row->tts_isnull, ncolumns * sizeof(bool));
		slot->tts_isnull[ncolumns] = !hs->whole;
		slot->tts_values[ncolumns] = (Datum) 0;
		if (hs->whole)
		{
			MemoryContext old =
				MemoryContextSwitchTo(node->ps.ps_ExprContext->ecxt_per_tuple_memory);

			slot->tts_values[ncolumns] =
				HeapTupleGetDatum(heap_form_tuple(hs->rowdesc, slot->tts_values,
												  slot->tts_isnull));
			MemoryContextSwitchTo(old);
		}
		ExecStoreVirtualTuple(slot);
	}
	return slot;
}

static bool
history_recheck(ScanState *node, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
history_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, history_next, history_recheck);
}

static void
history_rescan(CustomScanState *node)
{
	HistoryScanState *hs = (HistoryScanState *) node;

	if (hs->store)
		tuplestore_end(hs->store);
	hs->store = NULL;
	hs->read = false;
}

static void
history_end(CustomScanState *node)
{
	history_rescan(node);
}

static void
history_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	CustomScan *cs = (CustomScan *) node->ss.ps.plan;
	List	   *context;

	context = set_deparse_context_plan(es->deparse_cxt, (Plan *) cs, ancestors);
	ExplainPropertyText("History Key",
						deparse_expression((Node *) make_ands_explicit(cs->custom_exprs),
										   context, es->verbose, false),
						es);
}

static const CustomExecMethods history_exec_methods = {
	.CustomName = "HistoryScan",
	.BeginCustomScan = history_begin,
	.ExecCustomScan = history_exec,
	.EndCustomScan = history_end,
	.ReScanCustomScan = history_rescan,
	.ExplainCustomScan = history_explain,
};

static Node *
history_create_state(CustomScan *cs)
{
	HistoryScanState *hs = palloc0_object(HistoryScanState);

	NodeSetTag(hs, T_CustomScanState);
	hs->css.methods = &history_exec_methods;
	return (Node *) hs;
}

static const CustomScanMethods history_scan_methods = {
	.CustomName = "HistoryScan",
	.CreateCustomScanState = history_create_state,
};
