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
 * invalidated, and of the versioned tables using the row type of any relation it invalidated, so
 * that a composite type, a view or a table changed under a versioned table's column is caught too,
 * whichever branch it's on.
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
#include "catalog/indexing.h"
#include "catalog/pg_class.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "prolly/branch_catalog.h"
#include "storage/sinval.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

bool
BranchTypeNamesObject(Oid typid)
{
	switch (typid)
	{
		case REGCLASSOID:
		case REGCOLLATIONOID:
		case REGCONFIGOID:
		case REGDATABASEOID:
		case REGDICTIONARYOID:
		case REGNAMESPACEOID:
		case REGOPEROID:
		case REGOPERATOROID:
		case REGPROCOID:
		case REGPROCEDUREOID:
		case REGROLEOID:
		case REGTYPEOID:
		case ACLITEMOID:
			return true;
	}
	return false;
}

/* Whether a type is, or holds, a value naming an object. */
static bool
type_names_objects(Oid typid)
{
	HeapTuple	tup;
	Form_pg_type typ;
	bool		names = false;

	check_stack_depth();
	if (BranchTypeNamesObject(typid))
		return true;
	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", typid);
	typ = (Form_pg_type) GETSTRUCT(tup);
	if (typ->typtype == TYPTYPE_DOMAIN)
		names = type_names_objects(typ->typbasetype);
	else if (typ->typtype == TYPTYPE_RANGE)
		names = type_names_objects(get_range_subtype(typid));
	else if (typ->typtype == TYPTYPE_MULTIRANGE)
		names = type_names_objects(get_multirange_range(typid));
	else if (IsTrueArrayType(typ))
		names = type_names_objects(typ->typelem);
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
				names = type_names_objects(((Form_pg_attribute) GETSTRUCT(att))->atttypid);
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
				 errmsg("versioned table \"%s.%s\" cannot be a typed table", get_namespace_name(rel->relnamespace),
						NameStr(rel->relname))));
	for (AttrNumber a = 1; a <= rel->relnatts; a++)
	{
		HeapTuple	att = SearchSysCache2(ATTNUM, ObjectIdGetDatum(relid), Int16GetDatum(a));
		Form_pg_attribute attr;

		if (!HeapTupleIsValid(att))
			continue;
		attr = (Form_pg_attribute) GETSTRUCT(att);
		if (!attr->attisdropped && type_names_objects(attr->atttypid))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("column \"%s\" of versioned table \"%s.%s\" cannot be of type %s, which names an object",
							NameStr(attr->attname), get_namespace_name(rel->relnamespace), NameStr(rel->relname),
							format_type_be(attr->atttypid)),
					 errdetail("A rename changes such a value with no change of the row, and names differ between branches.")));
		ReleaseSysCache(att);
	}
	ReleaseSysCache(tup);
}

/*
 * The versioned tables that use a type, through arrays, domains, ranges, multiranges and the
 * composite types of relations of any kind, asked of: a type changed since is what they use now.
 */
static void
check_type_users(Oid typid, List **seen)
{
	Relation	depend;
	ScanKeyData keys[2];
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *types = NIL;
	List	   *relations = NIL;

	if (list_member_oid(*seen, typid))
		return;
	*seen = lappend_oid(*seen, typid);
	depend = table_open(DependRelationId, AccessShareLock);
	ScanKeyInit(&keys[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(TypeRelationId));
	ScanKeyInit(&keys[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(typid));
	scan = systable_beginscan(depend, DependReferenceIndexId, true, NULL, 2, keys);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend dep = (Form_pg_depend) GETSTRUCT(tup);

		if (dep->classid == TypeRelationId)
			types = lappend_oid(types, dep->objid);
		else if (dep->classid == RelationRelationId && dep->objsubid > 0)
			relations = list_append_unique_oid(relations, dep->objid);
	}
	systable_endscan(scan);
	table_close(depend, AccessShareLock);
	foreach_oid(other, types)
		check_type_users(other, seen);
	foreach_oid(relid, relations)
	{
		Oid			rowtype = get_rel_type_id(relid);

		check_table(relid);
		if (OidIsValid(rowtype))
			check_type_users(rowtype, seen);
	}
	list_free(types);
	list_free(relations);
}

void
BranchTablesCheckAtCommit(void)
{
	SharedInvalidationMessage *msgs;
	bool		relcache;
	int			n;
	List	   *seen = NIL;

	/*
	 * (not in a binary upgrade, which Postgres's own check of reg* columns lets regclass, regrole and
	 * regtype through: such a table from before this refused them is refused at its next DDL)
	 */
	if (!OidIsValid(MyDatabaseId) || IsBootstrapProcessingMode() || IsBinaryUpgrade || IsParallelWorker() ||
		!TransactionIdIsValid(GetTopTransactionIdIfAny()))
		return;
	CommandCounterIncrement();
	n = xactGetCommittedInvalidationMessages(&msgs, &relcache);

	/*
	 * Each relation invalidated, and those using its row type, as a composite type, a view or a table
	 * changed is a type that changed.  (The syscache isn't branch filtered, and object ids are each
	 * one branch's, so another branch's are found too.  A message for all relations, of no database,
	 * is of publications, not columns.)
	 */
	for (int i = 0; i < n; i++)
	{
		Oid			rowtype;

		if (msgs[i].id != SHAREDINVALRELCACHE_ID || msgs[i].rc.dbId != MyDatabaseId ||
			!OidIsValid(msgs[i].rc.relId))
			continue;
		check_table(msgs[i].rc.relId);
		rowtype = get_rel_type_id(msgs[i].rc.relId);
		if (OidIsValid(rowtype))
			check_type_users(rowtype, &seen);
	}
	list_free(seen);
}
