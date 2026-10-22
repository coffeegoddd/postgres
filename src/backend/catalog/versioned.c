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
 * An extension may be installed in a versioned schema, and isn't versioned
 * by it: what its script makes there is shared, as it would be in an
 * ordinary schema.  Such an object's catalog rows say so (branch 0), and
 * those rows are what answer whether an object in a versioned schema is
 * versioned.  A table the script makes there is a shared heap table.  While
 * an extension is in a versioned schema, the schema can't be dropped or
 * renamed, so every branch keeps it.
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
#include "catalog/pg_extension.h"
#include "catalog/pg_index.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_policy.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "catalog/versioned.h"
#include "commands/extension.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
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

/*
 * Is this row of an object in a versioned schema a branch's, not shared?
 * An extension's members there are shared, and their rows say so.
 */
static bool
row_on_a_branch(Oid classid, HeapTuple tup)
{
	const CatalogBranchInfo *info = catalog_branch_info(classid);

	return info == NULL || OidIsValid(catalog_row_branch(info, tup));
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
	versioned = form->relam == PROLLY_TABLE_AM_OID ||
		(namespace_is_versioned(form->relnamespace) &&
		 row_on_a_branch(RelationRelationId, tup));

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
		case ExtensionRelationId:
			/* shared by every branch, whatever its schema */
			return false;
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
			versioned = namespace_is_versioned(DatumGetObjectId(nsp)) &&
				row_on_a_branch(object->classId, tup);
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

		/*
		 * An extension, and what it makes, may depend on the versioned
		 * schema it is in: every branch keeps that schema while it is.
		 */
		if (referenced[i].classId == NamespaceRelationId &&
			(depender->classId == ExtensionRelationId || creating_extension ||
			 OidIsValid(getExtensionOfObject(depender->classId,
											 depender->objectId))))
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
	ObjectAddress type;

	/* a versioned type's typed tables are each branch's, with their type */
	ObjectAddressSet(type, TypeRelationId, typeOid);
	if (!other_branches_have_copies() || object_is_versioned(&type))
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

/* the first extension in schema nspid, or InvalidOid */
Oid
extension_in_namespace(Oid nspid)
{
	Relation	rel = table_open(ExtensionRelationId, AccessShareLock);
	SysScanDesc scan = systable_beginscan(rel, InvalidOid, false, NULL,
										  0, NULL);
	HeapTuple	tup;
	Oid			ext = InvalidOid;

	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_extension e = (Form_pg_extension) GETSTRUCT(tup);

		if (e->extnamespace == nspid)
		{
			ext = e->oid;
			break;
		}
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	return ext;
}

/*
 * Error if an extension can't be created in schema nspid: a versioned
 * schema must be on every branch that has copies, since what the extension
 * makes in it is every branch's.
 */
void
check_versioned_extension_schema(Oid nspid)
{
	const CatalogBranchInfo *info;
	Relation	rel;
	ScanKeyData key[1];
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *having = NIL;
	Oid			lacking = InvalidOid;

	if (!namespace_is_versioned(nspid) || !other_branches_have_copies())
		return;

	info = catalog_branch_info(NamespaceRelationId);
	rel = table_open(NamespaceRelationId, AccessShareLock);
	ScanKeyInit(&key[0], Anum_pg_namespace_oid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(nspid));
	CatalogAllBranches = true;
	PG_TRY();
	{
		scan = systable_beginscan(rel, NamespaceOidIndexId, true, NULL, 1, key);
		while ((tup = systable_getnext(scan)) != NULL)
			having = lappend_oid(having, catalog_row_branch(info, tup));
		systable_endscan(scan);
	}
	PG_FINALLY();
	{
		CatalogAllBranches = false;
	}
	PG_END_TRY();
	table_close(rel, AccessShareLock);

	rel = table_open(BranchRelationId, AccessShareLock);
	scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		Oid			branch = ((Form_pg_branch) GETSTRUCT(tup))->oid;

		if (!list_member_oid(having, branch))
		{
			lacking = branch;
			break;
		}
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	if (OidIsValid(lacking))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot create an extension in schema \"%s\", which branch \"%s\" doesn't have",
						get_namespace_name(nspid), branch_name_for_message(lacking)),
				 errdetail("An extension is shared by every branch, and so is what it makes in a versioned schema."),
				 errhint("Install it in a schema every branch has.")));
}

/*
 * Error if a shared row an extension's script is making in a versioned
 * schema has the identity of another branch's object: the catalog's unique
 * indexes include the branch, so both rows would stand, and that branch
 * would resolve the name to the extension's object instead of its own.  The
 * session's branch needn't be checked: Postgres refuses a duplicate there as
 * it makes the object.  Each unique index with the schema in its key is
 * checked on its other key columns.
 */
void
check_extension_member_identity(Relation rel, HeapTuple tup)
{
	Oid			relid = RelationGetRelid(rel);
	const CatalogBranchInfo *info = catalog_branch_info(relid);
	AttrNumber	nspattno = get_object_attnum_namespace(relid);
	AttrNumber	oidattno = get_object_attnum_oid(relid);
	bool		isnull;
	Oid			self;
	List	   *indexes;
	ListCell   *lc;

	if (info == NULL || nspattno == InvalidAttrNumber || !other_branches_have_copies())
		return;
	self = DatumGetObjectId(heap_getattr(tup, oidattno, RelationGetDescr(rel), &isnull));
	indexes = RelationGetIndexList(rel);
	foreach(lc, indexes)
	{
		Relation	irel = index_open(lfirst_oid(lc), AccessShareLock);
		int			nkeys = IndexRelationGetNumberOfKeyAttributes(irel);
		ScanKeyData *keys = palloc(sizeof(ScanKeyData) * nkeys);
		int			n = 0;
		bool		has_nsp = false;
		Oid			other = InvalidOid;

		if (!irel->rd_index->indisunique)
		{
			index_close(irel, AccessShareLock);
			continue;
		}
		for (int i = 0; i < nkeys; i++)
		{
			AttrNumber	att = irel->rd_index->indkey.values[i];
			Oid			op;

			if (att == info->attnum)
				continue;
			if (att == nspattno)
				has_nsp = true;
			op = get_opfamily_member(irel->rd_opfamily[i], irel->rd_opcintype[i],
									 irel->rd_opcintype[i], BTEqualStrategyNumber);
			ScanKeyInit(&keys[n++], att, BTEqualStrategyNumber, get_opcode(op),
						heap_getattr(tup, att, RelationGetDescr(rel), &isnull));
			if (isnull)
				has_nsp = false;	/* (no identity to collide with) */
		}
		if (has_nsp)
		{
			SysScanDesc scan;
			HeapTuple	row;

			CatalogAllBranches = true;
			PG_TRY();
			{
				scan = systable_beginscan(rel, RelationGetRelid(irel), true,
										  NULL, n, keys);
				while (HeapTupleIsValid(row = systable_getnext(scan)))
				{
					Oid			branch = catalog_row_branch(info, row);
					Oid			oid = DatumGetObjectId(heap_getattr(row, oidattno,
																	RelationGetDescr(rel),
																	&isnull));

					if (OidIsValid(branch) && branch != MyBranch && oid != self)
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
		}
		index_close(irel, AccessShareLock);
		if (OidIsValid(other))
		{
			Datum		name = heap_getattr(tup, get_object_attnum_name(relid),
											RelationGetDescr(rel), &isnull);
			Oid			nsp = DatumGetObjectId(heap_getattr(tup, nspattno,
															RelationGetDescr(rel),
															&isnull));

			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("extension \"%s\" can't make \"%s.%s\": branch \"%s\" has an object of that name",
							get_extension_name(CurrentExtensionObject),
							get_namespace_name(nsp), NameStr(*DatumGetName(name)),
							branch_name_for_message(other)),
					 errdetail("An extension's objects in a versioned schema are every branch's."),
					 errhint("Drop or rename it on that branch, or install the extension in another schema.")));
		}
	}
	list_free(indexes);
}

/*
 * Error if a versioned schema is being dropped or renamed while an
 * extension is in it: the extension's objects are every branch's, and
 * every branch keeps the schema they are in.
 */
void
check_versioned_schema_change(Oid nspid, const char *what)
{
	Oid			ext;

	if (!namespace_is_versioned(nspid))
		return;
	ext = extension_in_namespace(nspid);
	if (OidIsValid(ext))
		ereport(ERROR,
				(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				 errmsg("cannot %s schema \"%s\" because extension \"%s\" is in it",
						what, get_namespace_name(nspid), get_extension_name(ext)),
				 errdetail("An extension in a versioned schema is shared by every branch, and every branch keeps the schema."),
				 errhint("Drop the extension first.")));
}

/*
 * The objects in versioned schemas the running extension script let go of
 * (ALTER EXTENSION ... DROP), in TopTransactionContext: each must be gone,
 * or a member again, when the script ends.
 */
static List *script_let_go = NIL;

/*
 * Error if an object would move between an extension and a versioned
 * schema's versioned objects (ALTER EXTENSION ... ADD or DROP), or an
 * extension between a versioned schema and another (ALTER EXTENSION ...
 * SET SCHEMA): one is shared, the other each branch's.  An object an
 * extension's script makes is shared already, so a script may add it.  A
 * script may let one of its own extension's objects go (update scripts do,
 * before dropping it), and the object is noted: when the script ends it
 * must be dropped, or a member again (extension_script_end()).
 */
void
check_versioned_extension_member(const ObjectAddress *object, bool adding,
								 Oid extension)
{
	Oid			nsp = get_object_namespace(object);

	if (!OidIsValid(nsp) || !namespace_is_versioned(nsp))
		return;
	if (adding && !object_is_versioned(object))
		return;
	if (!adding && creating_extension && extension == CurrentExtensionObject)
	{
		MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);
		ObjectAddress *a = palloc(sizeof(ObjectAddress));

		*a = *object;
		script_let_go = lappend(script_let_go, a);
		MemoryContextSwitchTo(old);
		return;
	}
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 adding ?
			 errmsg("cannot add %s to an extension: it is in versioned schema \"%s\"",
					getObjectDescription(object, false), get_namespace_name(nsp)) :
			 errmsg("cannot remove %s from its extension: it is in versioned schema \"%s\"",
					getObjectDescription(object, false), get_namespace_name(nsp)),
			 errdetail("An extension's objects are shared by every branch; a versioned schema's other objects are each branch's.")));
}

/*
 * An extension script begins: what an enclosing one let go is set aside
 * (returned), to be given back to extension_script_end().
 */
List *
extension_script_begin(void)
{
	List	   *outer = script_let_go;

	script_let_go = NIL;
	return outer;
}

/*
 * An extension script ends (succeeded, if check): each object it let go in
 * a versioned schema must be dropped, or a member again.  One left over
 * would be shared by every branch yet no extension's, which a branch's
 * moves would treat as their own.
 */
void
extension_script_end(List *outer, bool check)
{
	List	   *let_go = script_let_go;
	ListCell   *lc;

	script_let_go = outer;
	if (!check)
		return;
	foreach(lc, let_go)
	{
		ObjectAddress *a = lfirst(lc);
		char	   *desc = getObjectDescription(a, true);

		if (desc == NULL || OidIsValid(getExtensionOfObject(a->classId, a->objectId)))
			continue;
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("extension \"%s\"'s script let %s go without dropping it, in versioned schema \"%s\"",
						get_extension_name(CurrentExtensionObject), desc,
						get_namespace_name(get_object_namespace(a))),
				 errdetail("An object in a versioned schema is an extension's, shared by every branch, or a branch's own; a script may let one of its objects go only to drop it or to add it again."),
				 errhint("Install the extension in a schema that isn't versioned.")));
	}
}

void
check_versioned_extension_move(Oid oldnspid, Oid newnspid)
{
	if (oldnspid != newnspid &&
		(namespace_is_versioned(oldnspid) || namespace_is_versioned(newnspid)))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot move an extension into or out of a versioned schema"),
				 errhint("Drop the extension and create it in the schema wanted.")));
}

Oid
versioned_relation_am(Oid nspid, Oid amoid, char relkind, char relpersistence,
					  bool explicit)
{
	if (!RELKIND_HAS_TABLE_AM(relkind) && relkind != RELKIND_PARTITIONED_TABLE)
		return amoid;

	if (namespace_is_versioned(nspid) && creating_extension)
	{
		/* an extension's table is shared: an ordinary heap table */
		if (amoid == PROLLY_TABLE_AM_OID)
		{
			if (explicit)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("an extension's tables can't be prolly tables"),
						 errdetail("An extension is shared by every branch, and so are its tables.")));
			return HEAP_TABLE_AM_OID;
		}
		return amoid;
	}

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
