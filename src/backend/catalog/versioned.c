/*-------------------------------------------------------------------------
 *
 * versioned.c
 *	  Versioned schemas: which objects are versioned, and the rules that
 *	  keep shared objects from depending on them.
 *
 * Everything in a versioned schema (CREATE SCHEMA ... VERSIONED) is
 * versioned: it will exist once per branch.  An object without a schema
 * is versioned if what it belongs to is: a column, default, trigger, rule
 * or policy if its table is, an index if its table is, a TOAST table if
 * its table is (a TOAST table has its table's access method, and only
 * versioned schemas hold prolly tables).  Everything else is shared.
 *
 * Dependencies point one way: a versioned object may depend on shared
 * ones, but a shared object may never depend on a versioned one, since it
 * would then point into one branch only.  Checked wherever Postgres
 * records a dependency, so foreign keys, column types, defaults, views,
 * triggers and function bodies are covered alike.
 *
 * Since a shared object is the same on every branch, dropping one has to
 * respect every branch's dependents, not only the session's: a column
 * type, default or view on another branch would otherwise be left naming
 * an object that is gone.  CASCADE can't reach them (a session changes
 * only its own branch), so a dependent on another branch refuses the drop.
 *
 * Tables in a versioned schema are prolly tables, and prolly tables live
 * only in versioned schemas.
 *
 * Objects are looked up with SnapshotSelf, so an object made earlier in
 * the same command is found: dependencies are often recorded before a
 * command counter increment.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/catalog/versioned.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/branch.h"
#include "catalog/dependency.h"
#include "catalog/pg_am.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_branch.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_class.h"
#include "catalog/pg_default_acl.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_index.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_policy.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "catalog/versioned.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/* does the database have any versioned schema?  -1: not known yet */
static int	any_versioned = -1;
static bool callback_registered = false;

static void
namespace_changed(Datum arg, SysCacheIdentifier cacheid, uint32 hashvalue)
{
	any_versioned = -1;
}

/*
 * Most databases have no versioned schema; then nothing here costs more
 * than this test.
 */
bool
database_has_versioned_schemas(void)
{
	if (!callback_registered)
	{
		CacheRegisterSyscacheCallback(NAMESPACEOID, namespace_changed, (Datum) 0);
		callback_registered = true;
	}
	if (any_versioned < 0)
	{
		Relation	rel = table_open(NamespaceRelationId, AccessShareLock);
		SysScanDesc scan = systable_beginscan(rel, InvalidOid, false,
											  SnapshotSelf, 0, NULL);
		HeapTuple	tup;

		any_versioned = 0;
		while ((tup = systable_getnext(scan)) != NULL)
		{
			if (((Form_pg_namespace) GETSTRUCT(tup))->nspversioned)
			{
				any_versioned = 1;
				break;
			}
		}
		systable_endscan(scan);
		table_close(rel, AccessShareLock);
	}
	return any_versioned == 1;
}

/*
 * A copy of the catalog row of object 'objid' in catalog 'classid', seen
 * with SnapshotSelf, or NULL if there is none.
 */
static HeapTuple
fetch_row(Oid classid, Oid objid)
{
	Relation	rel;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;

	AttrNumber	oidattno;
	Oid			oidindex;

	/* pg_attrdef isn't among the object classes objectaddress.c describes */
	if (classid == AttrDefaultRelationId)
	{
		oidattno = Anum_pg_attrdef_oid;
		oidindex = AttrDefaultOidIndexId;
	}
	else
	{
		oidattno = get_object_attnum_oid(classid);
		oidindex = get_object_oid_index(classid);
	}

	rel = table_open(classid, AccessShareLock);
	ScanKeyInit(&key, oidattno, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(objid));
	scan = systable_beginscan(rel, oidindex, true, SnapshotSelf, 1, &key);
	tup = systable_getnext(scan);
	if (HeapTupleIsValid(tup))
		tup = heap_copytuple(tup);
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	return tup;
}

bool
namespace_is_versioned(Oid nspid)
{
	HeapTuple	tup;
	bool		versioned;

	if (!OidIsValid(nspid) || !database_has_versioned_schemas())
		return false;
	tup = fetch_row(NamespaceRelationId, nspid);
	if (!HeapTupleIsValid(tup))
		return false;
	versioned = ((Form_pg_namespace) GETSTRUCT(tup))->nspversioned;
	heap_freetuple(tup);
	return versioned;
}

static bool
relation_is_versioned(Oid relid)
{
	HeapTuple	tup = fetch_row(RelationRelationId, relid);
	Form_pg_class form;
	bool		versioned;

	if (!HeapTupleIsValid(tup))
		return false;
	form = (Form_pg_class) GETSTRUCT(tup);
	versioned = namespace_is_versioned(form->relnamespace) ||
		form->relam == PROLLY_TABLE_AM_OID;

	/* an index is versioned if its table is (a TOAST table's index, too) */
	if (!versioned &&
		(form->relkind == RELKIND_INDEX ||
		 form->relkind == RELKIND_PARTITIONED_INDEX))
	{
		Relation	irel = table_open(IndexRelationId, AccessShareLock);
		ScanKeyData key;
		SysScanDesc scan;
		HeapTuple	itup;

		ScanKeyInit(&key, Anum_pg_index_indexrelid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(relid));
		scan = systable_beginscan(irel, IndexRelidIndexId, true, SnapshotSelf,
								  1, &key);
		itup = systable_getnext(scan);
		if (HeapTupleIsValid(itup))
			versioned = relation_is_versioned(((Form_pg_index) GETSTRUCT(itup))->indrelid);
		systable_endscan(scan);
		table_close(irel, AccessShareLock);
	}
	heap_freetuple(tup);
	return versioned;
}

bool
object_is_versioned(const ObjectAddress *object)
{
	HeapTuple	tup;
	Oid			relid = InvalidOid;
	AttrNumber	nspattno;
	bool		versioned = false;

	if (!database_has_versioned_schemas())
		return false;

	switch (object->classId)
	{
		case NamespaceRelationId:
			return namespace_is_versioned(object->objectId);
		case RelationRelationId:
			return relation_is_versioned(object->objectId);
		case DefaultAclRelationId:
			/* versioned if it is for a versioned schema; global ones are shared */
			tup = fetch_row(DefaultAclRelationId, object->objectId);
			if (!HeapTupleIsValid(tup))
				return false;
			versioned = namespace_is_versioned(((Form_pg_default_acl) GETSTRUCT(tup))->defaclnamespace);
			heap_freetuple(tup);
			return versioned;
		case AccessMethodOperatorRelationId:
		case AccessMethodProcedureRelationId:
			/* an operator family's entries: versioned if the family is */
			tup = fetch_row(object->classId, object->objectId);
			if (!HeapTupleIsValid(tup))
				return false;
			{
				ObjectAddress family;

				ObjectAddressSet(family, OperatorFamilyRelationId,
								 object->classId == AccessMethodOperatorRelationId ?
								 ((Form_pg_amop) GETSTRUCT(tup))->amopfamily :
								 ((Form_pg_amproc) GETSTRUCT(tup))->amprocfamily);
				versioned = object_is_versioned(&family);
			}
			heap_freetuple(tup);
			return versioned;
		case CastRelationId:
			/* versioned if a type it casts is */
			tup = fetch_row(CastRelationId, object->objectId);
			if (!HeapTupleIsValid(tup))
				return false;
			{
				Form_pg_cast c = (Form_pg_cast) GETSTRUCT(tup);
				ObjectAddress type;

				ObjectAddressSet(type, TypeRelationId, c->castsource);
				versioned = object_is_versioned(&type);
				ObjectAddressSet(type, TypeRelationId, c->casttarget);
				versioned = versioned || object_is_versioned(&type);
			}
			heap_freetuple(tup);
			return versioned;
		case AttrDefaultRelationId:
		case TriggerRelationId:
		case RewriteRelationId:
		case PolicyRelationId:
			/* versioned if their table is */
			tup = fetch_row(object->classId, object->objectId);
			if (!HeapTupleIsValid(tup))
				return false;
			if (object->classId == AttrDefaultRelationId)
				relid = ((Form_pg_attrdef) GETSTRUCT(tup))->adrelid;
			else if (object->classId == TriggerRelationId)
				relid = ((Form_pg_trigger) GETSTRUCT(tup))->tgrelid;
			else if (object->classId == RewriteRelationId)
				relid = ((Form_pg_rewrite) GETSTRUCT(tup))->ev_class;
			else
				relid = ((Form_pg_policy) GETSTRUCT(tup))->polrelid;
			heap_freetuple(tup);
			return relation_is_versioned(relid);
		default:
			break;
	}

	/* anything else in a schema is versioned if its schema is */
	if (!is_objectclass_supported(object->classId))
		return false;
	nspattno = get_object_attnum_namespace(object->classId);
	if (nspattno == InvalidAttrNumber)
		return false;
	tup = fetch_row(object->classId, object->objectId);
	if (HeapTupleIsValid(tup))
	{
		Relation	rel = table_open(object->classId, AccessShareLock);
		bool		isnull;
		Datum		nsp = heap_getattr(tup, nspattno, RelationGetDescr(rel),
									   &isnull);

		if (!isnull)
			versioned = namespace_is_versioned(DatumGetObjectId(nsp));
		table_close(rel, AccessShareLock);
		heap_freetuple(tup);
	}
	return versioned;
}


void
check_versioned_dependencies(const ObjectAddress *depender,
							 const ObjectAddress *referenced,
							 int nreferenced)
{
	int			depender_versioned = -1;

	if (!database_has_versioned_schemas())
		return;

	for (int i = 0; i < nreferenced; i++)
	{
		if (!object_is_versioned(&referenced[i]))
			continue;
		if (depender_versioned < 0)
			depender_versioned = object_is_versioned(depender) ? 1 : 0;
		if (depender_versioned == 0)
		{
			/* an object made in this command isn't in the caches yet */
			char	   *dep = getObjectDescription(depender, true);
			char	   *ref = getObjectDescription(&referenced[i], true);

			if (ref == NULL)
				ref = pstrdup("an object in a versioned schema");
			if (dep)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("%s can't depend on %s, which is versioned", dep, ref),
						 errdetail("Objects outside a versioned schema are shared by every branch, and can't depend on an object that exists per branch.")));
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("an object outside a versioned schema can't depend on %s, which is versioned", ref),
						 errdetail("Objects outside a versioned schema are shared by every branch, and can't depend on an object that exists per branch.")));
		}
	}
}

/*
 * Does a branch other than the session's have copies?  Only then can a
 * catalog row of another branch exist.  Asked of pg_branch, not of the
 * schemas: those the session sees are its own branch's, so a branch that
 * has dropped every versioned schema would answer for all of them.
 */
bool
other_branches_have_copies(void)
{
	Relation	rel = table_open(BranchRelationId, AccessShareLock);
	SysScanDesc scan = systable_beginscan(rel, InvalidOid, false, NULL,
										  0, NULL);
	HeapTuple	tup;
	bool		found = false;

	while ((tup = systable_getnext(scan)) != NULL)
	{
		if (((Form_pg_branch) GETSTRUCT(tup))->oid != MyBranch)
		{
			found = true;
			break;
		}
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	return found;
}

/* a branch's name for a message */
static char *
branch_name_for_message(Oid branch)
{
	HeapTuple	tup = SearchSysCache1(BRANCHOID, ObjectIdGetDatum(branch));
	char	   *name;

	if (!HeapTupleIsValid(tup))
		return psprintf("%u", branch);
	name = pstrdup(NameStr(((Form_pg_branch) GETSTRUCT(tup))->brname));
	ReleaseSysCache(tup);
	return name;
}

/*
 * The branches other than the session's whose objects depend on object
 * (on that column, for a column).  With data_only, only those that can
 * hold its values: a column, a typed table, or a type built on it (a
 * domain, array, range or composite), whose own uses there can't be
 * followed from here.  The scan builds no cache entry, so it may see every
 * branch's rows.
 */
static List *
branches_depending(const ObjectAddress *object, bool data_only)
{
	const CatalogBranchInfo *info = catalog_branch_info(DependRelationId);
	Relation	depRel;
	ScanKeyData key[3];
	int			nkeys;
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *others = NIL;

	depRel = table_open(DependRelationId, AccessShareLock);
	ScanKeyInit(&key[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(object->classId));
	ScanKeyInit(&key[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(object->objectId));
	if (object->objectSubId != 0)
	{
		/* a column: only what depends on that column */
		ScanKeyInit(&key[2], Anum_pg_depend_refobjsubid,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(object->objectSubId));
		nkeys = 3;
	}
	else
		nkeys = 2;

	CatalogAllBranches = true;
	PG_TRY();
	{
		scan = systable_beginscan(depRel, DependReferenceIndexId, true,
								  NULL, nkeys, key);
		while (HeapTupleIsValid(tup = systable_getnext(scan)))
		{
			Form_pg_depend dep = (Form_pg_depend) GETSTRUCT(tup);
			Oid			branch = catalog_row_branch(info, tup);

			if (!OidIsValid(branch) || branch == MyBranch)
				continue;
			if (data_only && dep->classid != TypeRelationId &&
				dep->classid != RelationRelationId)
				continue;
			others = list_append_unique_oid(others, branch);
		}
		systable_endscan(scan);
	}
	PG_FINALLY();
	{
		CatalogAllBranches = false;
	}
	PG_END_TRY();
	table_close(depRel, AccessShareLock);
	return others;
}

/*
 * Error if an object on a branch other than the session's depends on
 * object, which is about to be dropped.  Only a shared object can have
 * such dependents: a versioned one is the session's branch's own copy.
 * Callers dropping many objects ask other_branches_have_copies() once
 * first.
 */
void
check_other_branch_dependents(const ObjectAddress *object)
{
	List	   *others;

	if (object_is_versioned(object))
		return;
	others = branches_depending(object, false);
	if (others != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				 errmsg("cannot drop %s because objects on branch \"%s\" depend on it",
						getObjectDescription(object, false),
						branch_name_for_message(linitial_oid(others))),
				 list_length(others) > 1 ?
				 errdetail("Objects on %d branches other than the session's depend on it.",
						   list_length(others)) : 0,
				 errhint("It is shared by every branch.  Drop or change what depends on it on each branch first; CASCADE reaches only the session's branch.")));
}

/*
 * The name of a branch other than the session's whose objects depend on
 * object (only those that can hold its values, with data_only), or NULL if
 * there is none: for changes to a shared object that check or rebuild its
 * dependents, which can only be done on their own branch.
 */
char *
other_branch_dependent(const ObjectAddress *object, bool data_only)
{
	List	   *others;

	if (!other_branches_have_copies() || object_is_versioned(object))
		return NULL;
	others = branches_depending(object, data_only);
	return others != NIL ? branch_name_for_message(linitial_oid(others)) : NULL;
}

/*
 * The name of a branch other than the session's that has a typed table of
 * composite type typeOid, or NULL if none has.
 */
char *
other_branch_typed_table(Oid typeOid)
{
	const CatalogBranchInfo *info;
	Relation	rel;
	ScanKeyData key[1];
	SysScanDesc scan;
	HeapTuple	tup;
	Oid			other = InvalidOid;

	if (!other_branches_have_copies())
		return NULL;
	info = catalog_branch_info(RelationRelationId);
	rel = table_open(RelationRelationId, AccessShareLock);
	ScanKeyInit(&key[0], Anum_pg_class_reloftype, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(typeOid));
	CatalogAllBranches = true;
	PG_TRY();
	{
		scan = systable_beginscan(rel, InvalidOid, false, NULL, 1, key);
		while (HeapTupleIsValid(tup = systable_getnext(scan)))
		{
			Oid			branch = catalog_row_branch(info, tup);

			if (OidIsValid(branch) && branch != MyBranch)
			{
				other = branch;
				break;
			}
		}
		systable_endscan(scan);
	}
	PG_FINALLY();
	{
		CatalogAllBranches = false;
	}
	PG_END_TRY();
	table_close(rel, AccessShareLock);
	return OidIsValid(other) ? branch_name_for_message(other) : NULL;
}

Oid
versioned_relation_am(Oid nspid, Oid amoid, char relkind, char relpersistence,
					  bool explicit)
{
	if (!RELKIND_HAS_TABLE_AM(relkind) && relkind != RELKIND_PARTITIONED_TABLE)
		return amoid;

	if (namespace_is_versioned(nspid))
	{
		if (relpersistence != RELPERSISTENCE_PERMANENT)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("a versioned schema can't hold unlogged or temporary tables")));
		if (explicit && OidIsValid(amoid) && amoid != PROLLY_TABLE_AM_OID)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("tables in a versioned schema are prolly tables"),
					 errhint("Leave out USING, or use USING prolly.")));
		return PROLLY_TABLE_AM_OID;
	}

	if (amoid == PROLLY_TABLE_AM_OID)
	{
		if (explicit)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("prolly tables can only be created in a versioned schema"),
					 errhint("Create the table in a schema made with CREATE SCHEMA ... VERSIONED.")));
		/* prolly as the default means heap outside versioned schemas */
		return HEAP_TABLE_AM_OID;
	}
	return amoid;
}

void
check_versioned_namespace_move(Oid oldnspid, Oid newnspid)
{
	if (namespace_is_versioned(oldnspid) != namespace_is_versioned(newnspid))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot move objects between a versioned schema and a schema that isn't")));
}
