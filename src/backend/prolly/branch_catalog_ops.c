/*-------------------------------------------------------------------------
 *
 * branch_catalog_ops.c
 *	  What creating and deleting a branch does to the catalogs.
 *
 * A branch's catalog rows are its versioned schemas' pg_namespace rows and
 * every row of the objects in them.  Dropping them is a DROP SCHEMA ...
 * CASCADE of each of the branch's schemas at once, through the dependency
 * machinery, so that every row an object has anywhere goes with it, and
 * its storage with it.  The cascade cannot reach another branch's objects
 * nor shared ones, since no object depends on a branch's but that
 * branch's own (and temporary objects, which go too).  It costs what the
 * branch has: O(C) in the objects of its schemas.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/prolly/branch_catalog_ops.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/dependency.h"
#include "catalog/pg_namespace.h"
#include "fmgr.h"
#include "prolly/branch_catalog.h"
#include "utils/fmgrprotos.h"

static void
dependency_drop(int32 branch)
{
	Relation	rel;
	SysScanDesc scan;
	HeapTuple	tup;
	ObjectAddresses *schemas = new_object_addresses();

	if (branch <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%d is not a branch", branch)));

	/* the branch's schemas: pg_namespace holds few rows */
	rel = table_open(NamespaceRelationId, AccessShareLock);
	scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_namespace nsp = (Form_pg_namespace) GETSTRUCT(tup);
		ObjectAddress addr;

		if (nsp->nspbranch != branch)
			continue;
		ObjectAddressSet(addr, NamespaceRelationId, nsp->oid);
		add_exact_object_address(&addr, schemas);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	performMultipleDeletions(schemas, DROP_CASCADE,
							 PERFORM_DELETION_INTERNAL | PERFORM_DELETION_QUIETLY);
	free_object_addresses(schemas);
}

const BranchCatalogRoutine DependencyBranchCatalogs = {
	.drop = dependency_drop,
};

const BranchCatalogRoutine *BranchCatalogs = &DependencyBranchCatalogs;

/* SQL: pg_branch_catalog_drop(branch int4) returns void */
Datum
pg_branch_catalog_drop(PG_FUNCTION_ARGS)
{
	BranchCatalogs->drop(PG_GETARG_INT32(0));
	PG_RETURN_VOID();
}
