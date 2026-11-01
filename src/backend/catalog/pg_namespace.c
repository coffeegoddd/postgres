/*-------------------------------------------------------------------------
 *
 * pg_namespace.c
 *	  routines to support manipulation of the pg_namespace relation
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_namespace.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/transam.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_namespace.h"
#include "prolly/branch_catalog.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/syscache.h"


/* ----------------
 * NamespaceCreate
 *
 * Create a namespace (schema) with the given name and owner OID.
 *
 * If isTemp is true, this schema is a per-backend schema for holding
 * temporary tables.  Currently, it is used to prevent it from being
 * linked as a member of any active extension.  (If someone does CREATE
 * TEMP TABLE in an extension script, we don't want the temp schema to
 * become part of the extension). And to avoid checking for default ACL
 * for temp namespace (as it is not necessary).
 * ---------------
 */
Oid
NamespaceCreate(const char *nspName, Oid ownerId, bool isTemp)
{
	return NamespaceCreateOnBranch(nspName, ownerId, isTemp, 0);
}

/*
 * NamespaceCreateOnBranch
 *
 * Create a namespace that belongs to one branch (a versioned schema's row
 * on that branch), or to every branch if branch is 0.
 */
Oid
NamespaceCreateOnBranch(const char *nspName, Oid ownerId, bool isTemp,
						int32 branch)
{
	Relation	nspdesc;
	HeapTuple	tup;
	Oid			nspoid;
	bool		nulls[Natts_pg_namespace];
	Datum		values[Natts_pg_namespace];
	NameData	nname;
	TupleDesc	tupDesc;
	ObjectAddress myself;
	int			i;
	Acl		   *nspacl;

	/* sanity checks */
	if (!nspName)
		elog(ERROR, "no namespace name supplied");

	/*
	 * make sure there is no existing namespace of same name, on this branch
	 * or shared, nor (for a shared one) on any branch
	 */
	NamespaceCheckNameFree(nspName, branch);

	if (!isTemp)
		nspacl = get_user_default_acl(OBJECT_SCHEMA, ownerId,
									  InvalidOid);
	else
		nspacl = NULL;

	nspdesc = table_open(NamespaceRelationId, RowExclusiveLock);
	tupDesc = nspdesc->rd_att;

	/* initialize nulls and values */
	for (i = 0; i < Natts_pg_namespace; i++)
	{
		nulls[i] = false;
		values[i] = (Datum) 0;
	}

	nspoid = GetNewOidWithIndex(nspdesc, NamespaceOidIndexId,
								Anum_pg_namespace_oid);

	/*
	 * Object ids below FirstNormalObjectId are initdb's objects', which are
	 * shared and answered so without reading the catalogs.  A server started
	 * in single-user mode right after initdb still hands them out: no
	 * versioned schema then, nor so any of its objects.  (Once the counter is
	 * past them, it never hands them out again.)
	 */
	if (branch != 0 && nspoid < FirstNormalObjectId)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot make versioned schema \"%s\" before the server first starts",
						nspName),
				 errdetail("Object ids below %u are kept for initdb's objects, which are shared.",
						   FirstNormalObjectId)));
	values[Anum_pg_namespace_oid - 1] = ObjectIdGetDatum(nspoid);
	namestrcpy(&nname, nspName);
	values[Anum_pg_namespace_nspname - 1] = NameGetDatum(&nname);
	values[Anum_pg_namespace_nspowner - 1] = ObjectIdGetDatum(ownerId);
	values[Anum_pg_namespace_nspbranch - 1] = Int32GetDatum(branch);
	if (nspacl != NULL)
		values[Anum_pg_namespace_nspacl - 1] = PointerGetDatum(nspacl);
	else
		nulls[Anum_pg_namespace_nspacl - 1] = true;


	tup = heap_form_tuple(tupDesc, values, nulls);

	CatalogTupleInsert(nspdesc, tup);
	Assert(OidIsValid(nspoid));

	table_close(nspdesc, RowExclusiveLock);

	/* Record dependencies */
	myself.classId = NamespaceRelationId;
	myself.objectId = nspoid;
	myself.objectSubId = 0;

	/* dependency on owner */
	recordDependencyOnOwner(NamespaceRelationId, nspoid, ownerId);

	/* dependencies on roles mentioned in default ACL */
	recordDependencyOnNewAcl(NamespaceRelationId, nspoid, 0, ownerId, nspacl);

	/* dependency on extension ... but not for magic temp schemas */
	if (!isTemp)
		recordDependencyOnCurrentExtension(&myself, false);

	/* Post creation hook for new schema */
	InvokeObjectPostCreateHook(NamespaceRelationId, nspoid, 0);

	return nspoid;
}
