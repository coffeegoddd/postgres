/*-------------------------------------------------------------------------
 *
 * branch_tables.c
 *	  What a versioned schema's tables can't be.
 *
 * A versioned table's rows go into commits as their values' text, which is what capture and the
 * working set hold.  A value naming an object (regclass and the other reg* types, and aclitem) is
 * text a rename changes with no change of the row, and names differ between branches and clusters,
 * so such a column is refused, however it's made: directly, or inside an array, a domain, a range
 * or a composite type.  So is a typed table, which a commit can't hold by what it is.  What a commit
 * refuses of a table that can change any time (its grants, comments and the like), and a table of
 * another access method, which branched catalogs are made to hold, are refused as a commit's made,
 * as they always have been.
 *
 * It's asked as a transaction commits or prepares, of each table whose relcache entry it
 * invalidated, so every way of making or changing one is caught, whichever branch it's on.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/branch_tables.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "prolly/branch_catalog.h"
#include "storage/sinval.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

/* Whether a type is, or holds, a value naming an object. */
static bool
type_names_objects(Oid typid, int depth)
{
	HeapTuple	tup;
	Form_pg_type typ;
	bool		names = false;

	check_stack_depth();
	switch (typid)
	{
		case REGCLASSOID:
		case REGTYPEOID:
		case REGPROCOID:
		case REGPROCEDUREOID:
		case REGOPEROID:
		case REGOPERATOROID:
		case REGCONFIGOID:
		case REGDICTIONARYOID:
		case REGNAMESPACEOID:
		case REGROLEOID:
		case REGCOLLATIONOID:
		case ACLITEMOID:
			return true;
	}
	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", typid);
	typ = (Form_pg_type) GETSTRUCT(tup);
	if (typ->typtype == TYPTYPE_DOMAIN)
		names = type_names_objects(typ->typbasetype, depth + 1);
	else if (typ->typtype == TYPTYPE_RANGE)
		names = type_names_objects(get_range_subtype(typid), depth + 1);
	else if (typ->typtype == TYPTYPE_MULTIRANGE)
		names = type_names_objects(get_multirange_range(typid), depth + 1);
	else if (IsTrueArrayType(typ))
		names = type_names_objects(typ->typelem, depth + 1);
	else if (typ->typtype == TYPTYPE_COMPOSITE && OidIsValid(typ->typrelid))
	{
		Oid			relid = typ->typrelid;
		HeapTuple	reltup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
		int			natts;

		if (!HeapTupleIsValid(reltup))
			elog(ERROR, "cache lookup failed for relation %u", relid);
		natts = ((Form_pg_class) GETSTRUCT(reltup))->relnatts;
		ReleaseSysCache(reltup);

		for (AttrNumber a = 1; a <= natts && !names; a++)
		{
			HeapTuple	att = SearchSysCache2(ATTNUM, ObjectIdGetDatum(relid), Int16GetDatum(a));

			if (!HeapTupleIsValid(att))
				continue;
			if (!((Form_pg_attribute) GETSTRUCT(att))->attisdropped)
				names = type_names_objects(((Form_pg_attribute) GETSTRUCT(att))->atttypid, depth + 1);
			ReleaseSysCache(att);
		}
	}
	ReleaseSysCache(tup);
	return names;
}

static void
check_table(Oid relid)
{
	HeapTuple	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	Form_pg_class rel;

	/* (one made and dropped in the transaction is nothing to ask of) */
	if (!HeapTupleIsValid(tup))
		return;
	rel = (Form_pg_class) GETSTRUCT(tup);
	if ((rel->relkind != RELKIND_RELATION && rel->relkind != RELKIND_PARTITIONED_TABLE) ||
		NamespaceBranch(rel->relnamespace) == 0)
	{
		ReleaseSysCache(tup);
		return;
	}
	if (OidIsValid(rel->reloftype))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("versioned table \"%s\" cannot be a typed table", NameStr(rel->relname))));
	for (AttrNumber a = 1; a <= rel->relnatts; a++)
	{
		HeapTuple	att = SearchSysCache2(ATTNUM, ObjectIdGetDatum(relid), Int16GetDatum(a));
		Form_pg_attribute attr;

		if (!HeapTupleIsValid(att))
			continue;
		attr = (Form_pg_attribute) GETSTRUCT(att);
		if (!attr->attisdropped && type_names_objects(attr->atttypid, 0))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("column \"%s\" of versioned table \"%s\" cannot be of type %s, which names an object",
							NameStr(attr->attname), NameStr(rel->relname), format_type_be(attr->atttypid)),
					 errdetail("A rename changes such a value with no change of the row, and names differ between branches.")));
		ReleaseSysCache(att);
	}
	ReleaseSysCache(tup);
}

/* Every table of every branch, for an invalidation of them all. */
static void
check_all_tables(void)
{
	Relation	rel = table_open(RelationRelationId, AccessShareLock);
	SysScanDesc scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
	HeapTuple	tup;
	List	   *tables = NIL;

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_class c = (Form_pg_class) GETSTRUCT(tup);

		if (c->relkind == RELKIND_RELATION || c->relkind == RELKIND_PARTITIONED_TABLE)
			tables = lappend_oid(tables, c->oid);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	foreach_oid(relid, tables)
		check_table(relid);
	list_free(tables);
}

void
BranchTablesCheckAtCommit(void)
{
	SharedInvalidationMessage *msgs;
	bool		relcache;
	int			n;
	int			gucs;

	if (!OidIsValid(MyDatabaseId) || IsBootstrapProcessingMode() || IsBinaryUpgrade || IsParallelWorker() ||
		!TransactionIdIsValid(GetTopTransactionIdIfAny()))
		return;
	CommandCounterIncrement();
	n = xactGetCommittedInvalidationMessages(&msgs, &relcache);
	if (n == 0)
		return;

	/* (the tables of whichever branch it changed, not only the session's) */
	gucs = NewGUCNestLevel();
	(void) set_config_option("catalog_branch_filter", "off", PGC_SUSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	for (int i = 0; i < n; i++)
	{
		if (msgs[i].id != SHAREDINVALRELCACHE_ID || msgs[i].rc.dbId != MyDatabaseId)
			continue;
		if (!OidIsValid(msgs[i].rc.relId))
		{
			check_all_tables();
			break;
		}
		check_table(msgs[i].rc.relId);
	}
	AtEOXact_GUC(true, gucs);
}
