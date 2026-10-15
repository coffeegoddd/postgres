/*--------------------------------------------------------------------------
 *
 * catalog_checks.c
 *		Checks of how catalog writes fill in a row's branch column.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_prolly/catalog_checks.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/pg_class.h"
#include "catalog/pg_description.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/*
 * Memory left in a context after n inserts into pg_description of rows with
 * 1 KiB descriptions, formed with the branch column null if null_branch.
 */
static Size
insert_descriptions(Relation rel, int n, bool null_branch, Oid first_objoid)
{
	MemoryContext ctx = AllocSetContextCreate(CurrentMemoryContext,
											  "branch column check",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(ctx);
	char	   *payload = palloc(1024 + 1);
	Datum		values[Natts_pg_description];
	bool		nulls[Natts_pg_description] = {0};
	Size		left;

	memset(payload, 'x', 1024);
	payload[1024] = '\0';
	values[Anum_pg_description_classoid - 1] = ObjectIdGetDatum(RelationRelationId);
	values[Anum_pg_description_objsubid - 1] = Int32GetDatum(0);
	values[Anum_pg_description_objbranch - 1] = ObjectIdGetDatum(InvalidOid);
	nulls[Anum_pg_description_objbranch - 1] = null_branch;
	values[Anum_pg_description_description - 1] = CStringGetTextDatum(payload);

	for (int i = 0; i < n; i++)
	{
		HeapTuple	tup;

		values[Anum_pg_description_objoid - 1] = ObjectIdGetDatum(first_objoid + i);
		tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
		CatalogTupleInsert(rel, tup);
		heap_freetuple(tup);
	}

	MemoryContextSwitchTo(old);
	left = MemoryContextMemAllocated(ctx, true);
	MemoryContextDelete(ctx);
	return left;
}

/*
 * test_catalog_null_branch_no_leak(n): inserting n catalog rows formed with
 * the branch column null leaves about as much memory behind as inserting n
 * rows with it set; the rows written in their place are freed.  Rows go to
 * pg_description for objects that don't exist, so the caller rolls back.
 */
PG_FUNCTION_INFO_V1(test_catalog_null_branch_no_leak);

Datum
test_catalog_null_branch_no_leak(PG_FUNCTION_ARGS)
{
	int32		n = PG_GETARG_INT32(0);
	Relation	rel = table_open(DescriptionRelationId, RowExclusiveLock);
	Size		set_left = insert_descriptions(rel, n, false, 4000000000U);
	Size		null_left = insert_descriptions(rel, n, true, 4100000000U);

	table_close(rel, RowExclusiveLock);
	if (null_left > set_left + (Size) n * 512)
		elog(ERROR, "rows formed with a null branch column left %zu bytes; rows with it set left %zu",
			 null_left, set_left);
	PG_RETURN_BOOL(true);
}
