/*--------------------------------------------------------------------------
 *
 * applier_checks.c
 *		Checks of the row applier, from SQL: rows of a table's row type
 *		inserted, updated and removed as a reset or merge writes them.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_prolly/applier_checks.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/prolly_applier.h"
#include "access/table.h"
#include "access/tableam.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

/* A table's row (a composite of its row type) into a slot of the table. */
static TupleTableSlot *
row_slot(Relation rel, Datum row)
{
	HeapTupleHeader td = DatumGetHeapTupleHeader(row);
	HeapTupleData tuple;
	TupleTableSlot *slot = table_slot_create(rel, NULL);

	tuple.t_len = HeapTupleHeaderGetDatumLength(td);
	ItemPointerSetInvalid(&tuple.t_self);
	tuple.t_tableOid = RelationGetRelid(rel);
	tuple.t_data = td;
	ExecForceStoreHeapTuple(heap_copytuple(&tuple), slot, true);
	return slot;
}

/*
 * test_apply(op text, old anyelement, new anyelement) returns bool
 *
 * op is insert (new), update (old to new) or remove (old), applied to the
 * table whose row type the arguments are; false if old wasn't found.
 */
PG_FUNCTION_INFO_V1(test_apply);
Datum
test_apply(PG_FUNCTION_ARGS)
{
	char	   *op = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Oid			relid = get_typ_typrelid(get_fn_expr_argtype(fcinfo->flinfo, 1));
	const ProllyApplierRoutine *ar = prolly_applier_lookup("replication");
	Relation	rel;
	ProllyApplier *a;
	TupleTableSlot *old = NULL;
	TupleTableSlot *new = NULL;
	bool		done = true;

	if (!OidIsValid(relid))
		elog(ERROR, "test_apply needs a table's row type");
	rel = table_open(relid, RowExclusiveLock);
	if (!PG_ARGISNULL(1))
		old = row_slot(rel, PG_GETARG_DATUM(1));
	if (!PG_ARGISNULL(2))
		new = row_slot(rel, PG_GETARG_DATUM(2));
	a = PROLLY_CALL(ar, begin, rel);
	if (strcmp(op, "insert") == 0 && new)
		PROLLY_CALL(ar, insert, a, new);
	else if (strcmp(op, "update") == 0 && old && new)
		done = PROLLY_CALL(ar, update, a, old, new, NULL);
	else if (strcmp(op, "remove") == 0 && old)
		done = PROLLY_CALL(ar, remove, a, old);
	else
		elog(ERROR, "test_apply: bad operation \"%s\" or arguments", op);
	PROLLY_CALL(ar, end, a);
	if (old)
		ExecDropSingleTupleTableSlot(old);
	if (new)
		ExecDropSingleTupleTableSlot(new);
	table_close(rel, NoLock);
	PG_RETURN_BOOL(done);
}
