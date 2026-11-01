/*-------------------------------------------------------------------------
 *
 * branch_catalog_copy.c
 *	  Copying a branch's catalog rows to another branch.
 *
 * A branch's catalog rows are its versioned schemas' pg_namespace rows and
 * every row of the objects in them.  Copying them to a new branch makes
 * every one again under a new object id: each schema anew on the new
 * branch, each object in it, and each row that describes one (a column, a
 * default, a dependency, a comment, the statistics).  Every reference from
 * a copied row to a copied object is rewritten to the copy's object id;
 * references to shared objects stay.  References are of two kinds: object
 * id columns, which each catalog's copy rewrites by name, and the object
 * ids inside expression trees (pg_node_tree columns: defaults, check
 * constraints, index expressions), which a mutator rewrites node by node.
 *
 * The objects are found from the branch's schemas through pg_depend, as a
 * drop of them cascades: every object in a schema depends on it, and every
 * object that belongs to another (a table's row type, index, TOAST table,
 * constraint or default) depends on that one.  So the copy costs what the
 * branch has, O(C).  Temporary objects that depend on the branch's (a
 * temporary view of its table) are the session's, and are not copied.
 *
 * Copied: schemas; tables, partitioned tables and their partitions,
 * views, materialized views and composite types (their columns, row
 * types, TOAST tables, inheritance and partition bounds); indexes and
 * partitioned indexes; sequences; constraints (and foreign keys'
 * triggers, a shared referenced table's too); column defaults; triggers,
 * rules and policies; functions, procedures and aggregates; the branch's
 * own types (domains, enums, ranges, base types), casts, operators and
 * operator classes and families; extended statistics objects; extensions;
 * default privileges; membership of shared publications; and every
 * object's dependencies, owners, comments and statistics.  A branch with
 * any other kind of object (a collation, a text search object, a foreign
 * table, an object with initial privileges...) is refused, naming it, as
 * is one whose tables store values of its own composite, enum or range
 * types, which name their types in the data the branches share: no copy is
 * ever half made.
 *
 * Locks: the source branch exclusively, which DDL recording a dependency
 * on the branch's objects holds shared (CatalogBranchCheckDependencies()),
 * then each of its relations against index builds and ANALYZE (and its
 * sequences against nextval()).  DDL takes its relations' locks first, so
 * a transaction of several DDL statements on the branch racing a copy can
 * deadlock with it; the deadlock detector then aborts one of them.
 *
 * Every row made is then checked: no object id column, and no number in an
 * expression tree, may name an object of the source branch.  A reference a
 * copy failed to rewrite is so an error, not a branch that reads another
 * branch's objects.
 *
 * Storage: each copied relation gets storage of its own, empty, in the
 * storage manager of its source (a table's indexes rebuilt on it, so they
 * are valid), except a sequence's, whose page is copied, its value with it.
 * Sharing a table's pages with its copy is the branch lifecycle's, which
 * replaces this empty storage.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/prolly/branch_catalog_copy.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_publication.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_publication_namespace.h"
#include "catalog/pg_publication_rel.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_partitioned_table.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_am.h"
#include "catalog/pg_default_acl.h"
#include "catalog/pg_range.h"
#include "catalog/pg_enum.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_description.h"
#include "catalog/pg_index.h"
#include "catalog/pg_init_privs.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_policy.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_seclabel.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_shdepend.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "catalog/storage.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "prolly/branch_catalog.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/* an object, by class and object id */
typedef struct CopyKey
{
	Oid			classid;
	Oid			oid;
} CopyKey;

typedef struct CopyEntry
{
	CopyKey		k;				/* hash key: must be first */
	Oid			newoid;
} CopyEntry;

/* a source object id, whatever its class, for the check of rows made */
typedef struct SourceEntry
{
	Oid			oid;			/* hash key */
} SourceEntry;

typedef struct CopyState
{
	int32		src;
	int32		dst;
	HTAB	   *map;			/* CopyEntry by CopyKey */
	HTAB	   *sources;		/* SourceEntry: every source object's id */
	CopyKey    *objs;			/* the objects, in the order found */
	int			nobjs;
	int			cap;
	List	   *made;			/* (Oid catalog, HeapTuple) of each row made */
	CopyKey		current;		/* the object whose rows are being copied */
} CopyState;

static void refuse(Oid classid, Oid oid);

typedef struct MadeRow
{
	Oid			catid;
	HeapTuple	tup;
} MadeRow;

static Oid	copy_map(CopyState *cs, Oid classid, Oid oid);

/* ------------------------------------------------------------------------
 * Finding the objects
 * ------------------------------------------------------------------------
 */

static HeapTuple
fetch_row(Oid catid, Oid oidindex, AttrNumber oidatt, Oid oid)
{
	Relation	rel;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;

	rel = table_open(catid, AccessShareLock);
	ScanKeyInit(&key, oidatt, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(oid));
	scan = systable_beginscan(rel, oidindex, true, NULL, 1, &key);
	tup = systable_getnext(scan);
	if (HeapTupleIsValid(tup))
		tup = heap_copytuple(tup);
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for object %u of catalog %u", oid, catid);
	return tup;
}

static bool
relation_is_temp(Oid relid)
{
	return isAnyTempNamespace(get_rel_namespace(relid));
}

/* whether object (classid, oid) is a temporary one: the session's, not copied */
static bool
object_is_temp(Oid classid, Oid oid)
{
	HeapTuple	tup;
	bool		temp = false;

	switch (classid)
	{
		case RelationRelationId:
			return relation_is_temp(oid);
		case TypeRelationId:
			tup = fetch_row(TypeRelationId, TypeOidIndexId, Anum_pg_type_oid, oid);
			temp = isAnyTempNamespace(((Form_pg_type) GETSTRUCT(tup))->typnamespace);
			break;
		case AttrDefaultRelationId:
			tup = fetch_row(AttrDefaultRelationId, AttrDefaultOidIndexId,
							Anum_pg_attrdef_oid, oid);
			temp = relation_is_temp(((Form_pg_attrdef) GETSTRUCT(tup))->adrelid);
			break;
		case ConstraintRelationId:
			tup = fetch_row(ConstraintRelationId, ConstraintOidIndexId,
							Anum_pg_constraint_oid, oid);
			temp = isAnyTempNamespace(((Form_pg_constraint) GETSTRUCT(tup))->connamespace);
			break;
		case TriggerRelationId:
			tup = fetch_row(TriggerRelationId, TriggerOidIndexId,
							Anum_pg_trigger_oid, oid);
			temp = relation_is_temp(((Form_pg_trigger) GETSTRUCT(tup))->tgrelid);
			break;
		case RewriteRelationId:
			tup = fetch_row(RewriteRelationId, RewriteOidIndexId,
							Anum_pg_rewrite_oid, oid);
			temp = relation_is_temp(((Form_pg_rewrite) GETSTRUCT(tup))->ev_class);
			break;
		case PolicyRelationId:
			tup = fetch_row(PolicyRelationId, PolicyOidIndexId,
							Anum_pg_policy_oid, oid);
			temp = relation_is_temp(((Form_pg_policy) GETSTRUCT(tup))->polrelid);
			break;
		default:
			{
				ObjectAddress addr;
				Oid			nspid;

				ObjectAddressSet(addr, classid, oid);
				if (!is_objectclass_supported(classid))
					return false;
				nspid = get_object_namespace(&addr);
				return OidIsValid(nspid) && isAnyTempNamespace(nspid);
			}
	}
	heap_freetuple(tup);
	return temp;
}

static void
refuse(Oid classid, Oid oid)
{
	ObjectAddress addr;

	ObjectAddressSet(addr, classid, oid);
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot copy %s to another branch yet",
					getObjectDescription(&addr, false))));
}

/* errors out unless the copy knows how to copy object (classid, oid) */
static void
check_copyable(Oid classid, Oid oid)
{
	HeapTuple	tup;

	switch (classid)
	{
		case RelationRelationId:
			{
				Form_pg_class c;

				tup = fetch_row(RelationRelationId, ClassOidIndexId,
								Anum_pg_class_oid, oid);
				c = (Form_pg_class) GETSTRUCT(tup);
				if ((c->relkind != RELKIND_RELATION &&
					 c->relkind != RELKIND_INDEX &&
					 c->relkind != RELKIND_SEQUENCE &&
					 c->relkind != RELKIND_TOASTVALUE &&
					 c->relkind != RELKIND_VIEW &&
					 c->relkind != RELKIND_MATVIEW &&
					 c->relkind != RELKIND_COMPOSITE_TYPE &&
					 c->relkind != RELKIND_PARTITIONED_TABLE &&
					 c->relkind != RELKIND_PARTITIONED_INDEX) ||
					c->relpersistence != RELPERSISTENCE_PERMANENT ||
					OidIsValid(c->reloftype) ||
					/* (storage made as heap's is: only heap's tables) */
					((c->relkind == RELKIND_RELATION || c->relkind == RELKIND_MATVIEW ||
					  c->relkind == RELKIND_PARTITIONED_TABLE) &&
					 OidIsValid(c->relam) && c->relam != HEAP_TABLE_AM_OID))
					refuse(classid, oid);
				/* (a rebuild on the copy would make an invalid index valid) */
				if (c->relkind == RELKIND_INDEX || c->relkind == RELKIND_PARTITIONED_INDEX)
				{
					HeapTuple	itup = fetch_row(IndexRelationId, IndexRelidIndexId,
												 Anum_pg_index_indexrelid, oid);

					if (!((Form_pg_index) GETSTRUCT(itup))->indisvalid)
						refuse(classid, oid);
					heap_freetuple(itup);
				}
				heap_freetuple(tup);
				return;
			}
		case TypeRelationId:
			{
				Form_pg_type t;
				bool		ok;

				tup = fetch_row(TypeRelationId, TypeOidIndexId,
								Anum_pg_type_oid, oid);
				t = (Form_pg_type) GETSTRUCT(tup);
				/*
				 * a relation's row type, an array, a domain, an enum, a range
				 * (and its multirange), or a base type (an extension's, its
				 * functions copied with it); not a shell or pseudo type
				 */
				ok = t->typtype == TYPTYPE_COMPOSITE ||
					t->typtype == TYPTYPE_DOMAIN ||
					t->typtype == TYPTYPE_ENUM ||
					t->typtype == TYPTYPE_RANGE ||
					t->typtype == TYPTYPE_MULTIRANGE ||
					/* (a base type's functions, an extension's, are copied with it) */
					t->typtype == TYPTYPE_BASE;
				heap_freetuple(tup);
				if (!ok)
					refuse(classid, oid);
				return;
			}
		case ConstraintRelationId:
			{
				Form_pg_constraint con;
				bool		ok;

				tup = fetch_row(ConstraintRelationId, ConstraintOidIndexId,
								Anum_pg_constraint_oid, oid);
				con = (Form_pg_constraint) GETSTRUCT(tup);
				ok = (con->contype == CONSTRAINT_CHECK ||
					  con->contype == CONSTRAINT_FOREIGN ||
					  con->contype == CONSTRAINT_PRIMARY ||
					  con->contype == CONSTRAINT_UNIQUE ||
					  con->contype == CONSTRAINT_NOTNULL ||
					  con->contype == CONSTRAINT_EXCLUSION);
				heap_freetuple(tup);
				if (!ok)
					refuse(classid, oid);
				return;
			}
		case AttrDefaultRelationId:
		case DefaultAclRelationId:
		case CastRelationId:
		case StatisticExtRelationId:
		case ExtensionRelationId:
		case PublicationRelRelationId:
		case PublicationNamespaceRelationId:
		case OperatorRelationId:
		case OperatorFamilyRelationId:
		case OperatorClassRelationId:
		case AccessMethodOperatorRelationId:
		case AccessMethodProcedureRelationId:
		case PolicyRelationId:
		case RewriteRelationId:
			return;
		case ProcedureRelationId:
		case TriggerRelationId:
			return;
		default:
			refuse(classid, oid);
	}
}

static void
add_object(CopyState *cs, Oid classid, Oid oid)
{
	CopyKey		key;
	CopyEntry  *e;
	SourceEntry *s;
	bool		found;

	key.classid = classid;
	key.oid = oid;
	e = hash_search(cs->map, &key, HASH_ENTER, &found);
	if (found)
		return;
	e->newoid = InvalidOid;
	s = hash_search(cs->sources, &oid, HASH_ENTER, &found);
	(void) s;
	if (cs->nobjs == cs->cap)
	{
		cs->cap *= 2;
		cs->objs = repalloc_array(cs->objs, CopyKey, cs->cap);
	}
	cs->objs[cs->nobjs++] = key;
}

static bool
in_copy(CopyState *cs, Oid classid, Oid oid)
{
	CopyKey		key;

	key.classid = classid;
	key.oid = oid;
	return hash_search(cs->map, &key, HASH_FIND, NULL) != NULL;
}

/*
 * The source branch's objects: its schemas, then every object that depends
 * on one already found (pg_depend's reference index, so each object costs
 * an index probe), but temporary ones.
 */
static void
collect(CopyState *cs)
{
	Relation	rel;
	SysScanDesc scan;
	HeapTuple	tup;

	rel = table_open(NamespaceRelationId, AccessShareLock);
	scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_namespace nsp = (Form_pg_namespace) GETSTRUCT(tup);

		if (nsp->nspbranch == cs->src)
			add_object(cs, NamespaceRelationId, nsp->oid);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	rel = table_open(DependRelationId, AccessShareLock);
	for (int i = 0; i < cs->nobjs; i++)
	{
		CopyKey		obj = cs->objs[i];
		ScanKeyData key[2];

		/*
		 * (no index made, no ANALYZE, nor a sequence's value moved, while
		 * it's copied; DML may run, the copy's storage being its own)
		 */
		if (obj.classid == RelationRelationId)
		{
			LockRelationOid(obj.oid, ShareUpdateExclusiveLock);
			/* (dropped while the copy waited for it) */
			if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(obj.oid)))
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("a relation of branch %d was dropped while it was being copied",
								cs->src)));
			if (get_rel_relkind(obj.oid) == RELKIND_SEQUENCE)
				LockRelationOid(obj.oid, ShareLock);
		}
		ScanKeyInit(&key[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(obj.classid));
		ScanKeyInit(&key[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(obj.oid));
		scan = systable_beginscan(rel, DependReferenceIndexId, true, NULL, 2, key);
		while (HeapTupleIsValid(tup = systable_getnext(scan)))
		{
			Form_pg_depend dep = (Form_pg_depend) GETSTRUCT(tup);

			if (in_copy(cs, dep->classid, dep->objid) ||
				object_is_temp(dep->classid, dep->objid))
				continue;
			check_copyable(dep->classid, dep->objid);
			add_object(cs, dep->classid, dep->objid);
		}
		systable_endscan(scan);
	}
	table_close(rel, AccessShareLock);
}

/*
 * A value of a composite, enum or range type names its type's object ids
 * in the table data that holds it (an enum's value is its label's id, a
 * composite's and an array's header its type's), which the copy and its
 * source would share: a copied table may not store a value of a copied
 * one, directly, in an array or through a domain.
 */
static bool
type_stored_names_copy(CopyState *cs, Oid typid)
{
	for (;;)
	{
		Oid			elem = get_element_type(typid);

		/* (an array's header names its element type, whatever it is) */
		if (OidIsValid(elem) && in_copy(cs, TypeRelationId, elem))
			return true;
		if (OidIsValid(elem))
			typid = elem;
		else if (get_typtype(typid) == TYPTYPE_DOMAIN)
			typid = getBaseType(typid);
		else
			break;
	}
	return in_copy(cs, TypeRelationId, typid) &&
		(get_typtype(typid) == TYPTYPE_COMPOSITE ||
		 get_typtype(typid) == TYPTYPE_ENUM ||
		 get_typtype(typid) == TYPTYPE_RANGE ||
		 get_typtype(typid) == TYPTYPE_MULTIRANGE);
}

static void
check_stored_types(CopyState *cs)
{
	for (int i = 0; i < cs->nobjs; i++)
	{
		CopyKey		obj = cs->objs[i];
		Relation	rel;

		/* (an index's tuples hold its expressions' values too) */
		if (obj.classid != RelationRelationId ||
			!RELKIND_HAS_STORAGE(get_rel_relkind(obj.oid)))
			continue;
		rel = relation_open(obj.oid, NoLock);
		for (int a = 0; a < RelationGetDescr(rel)->natts; a++)
		{
			Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel), a);

			if (!att->attisdropped && type_stored_names_copy(cs, att->atttypid))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot copy %s to another branch: column \"%s\" holds values of the branch's own type %s",
								getObjectDescription(&(ObjectAddress) {RelationRelationId, obj.oid, 0}, false),
								NameStr(att->attname),
								format_type_be(att->atttypid)),
						 errdetail("The table's data, which the branches share, names the type's object ids."),
						 errhint("Make the type in a shared schema.")));
		}
		relation_close(rel, NoLock);
	}
}

/* ------------------------------------------------------------------------
 * New object ids
 * ------------------------------------------------------------------------
 */

/*
 * An enum's labels' new object ids: an even id compares as the labels'
 * order (enum_cmp()), so, as EnumValuesCreate() does, even ids are taken,
 * sorted, and given in the labels' order.  A constant of the enum names a
 * label (its value is the label's id), so each label is mapped, as an
 * object, and is one of the source's for the check of rows made.
 */
static void
assign_enum_oids(CopyState *cs, Oid typid)
{
	Relation	rel = table_open(EnumRelationId, RowExclusiveLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *labels = NIL;
	Oid		   *oids;
	int			n;
	int			i;

	ScanKeyInit(&key, Anum_pg_enum_enumtypid, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(typid));
	scan = systable_beginscan(rel, EnumTypIdSortOrderIndexId, true, NULL, 1, &key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
		labels = lappend_oid(labels, ((Form_pg_enum) GETSTRUCT(tup))->oid);
	systable_endscan(scan);

	n = list_length(labels);
	oids = palloc_array(Oid, Max(n, 1));
	for (i = 0; i < n; i++)
	{
		do
			oids[i] = GetNewOidWithIndex(rel, EnumOidIndexId, Anum_pg_enum_oid);
		while (oids[i] & 1);
	}
	qsort(oids, n, sizeof(Oid), oid_cmp);
	table_close(rel, RowExclusiveLock);

	i = 0;
	/* (in the map and the source's ids, not among the objects copied) */
	foreach_oid(label, labels)
	{
		CopyKey		k = {EnumRelationId, label};
		CopyEntry  *e = hash_search(cs->map, &k, HASH_ENTER, NULL);

		e->newoid = oids[i++];
		(void) hash_search(cs->sources, &label, HASH_ENTER, NULL);
	}
}

static void
assign_oids(CopyState *cs)
{
	for (int i = 0; i < cs->nobjs; i++)
	{
		CopyKey		obj = cs->objs[i];
		CopyEntry  *e = hash_search(cs->map, &obj, HASH_FIND, NULL);
		Relation	rel;

		switch (obj.classid)
		{
			case RelationRelationId:
				{
					HeapTuple	tup = fetch_row(RelationRelationId, ClassOidIndexId,
												Anum_pg_class_oid, obj.oid);
					Form_pg_class c = (Form_pg_class) GETSTRUCT(tup);

					/* (an id no file has, so the relation's file number too) */
					rel = table_open(RelationRelationId, RowExclusiveLock);
					e->newoid = GetNewRelFileNumber(c->reltablespace, rel,
													c->relpersistence);
					table_close(rel, RowExclusiveLock);
					heap_freetuple(tup);
					break;
				}
			case NamespaceRelationId:
				rel = table_open(NamespaceRelationId, RowExclusiveLock);
				e->newoid = GetNewOidWithIndex(rel, NamespaceOidIndexId,
											   Anum_pg_namespace_oid);
				table_close(rel, RowExclusiveLock);
				break;
			case TypeRelationId:
				rel = table_open(TypeRelationId, RowExclusiveLock);
				e->newoid = GetNewOidWithIndex(rel, TypeOidIndexId,
											   Anum_pg_type_oid);
				table_close(rel, RowExclusiveLock);
				if (get_typtype(obj.oid) == TYPTYPE_ENUM)
					assign_enum_oids(cs, obj.oid);
				break;
			case ConstraintRelationId:
				rel = table_open(ConstraintRelationId, RowExclusiveLock);
				e->newoid = GetNewOidWithIndex(rel, ConstraintOidIndexId,
											   Anum_pg_constraint_oid);
				table_close(rel, RowExclusiveLock);
				break;
			case AttrDefaultRelationId:
				rel = table_open(AttrDefaultRelationId, RowExclusiveLock);
				e->newoid = GetNewOidWithIndex(rel, AttrDefaultOidIndexId,
											   Anum_pg_attrdef_oid);
				table_close(rel, RowExclusiveLock);
				break;
			case TriggerRelationId:
				rel = table_open(TriggerRelationId, RowExclusiveLock);
				e->newoid = GetNewOidWithIndex(rel, TriggerOidIndexId,
											   Anum_pg_trigger_oid);
				table_close(rel, RowExclusiveLock);
				break;
			case OperatorRelationId:
				rel = table_open(OperatorRelationId, RowExclusiveLock);
				e->newoid = GetNewOidWithIndex(rel, OperatorOidIndexId, Anum_pg_operator_oid);
				table_close(rel, RowExclusiveLock);
				break;
			case OperatorFamilyRelationId:
				rel = table_open(OperatorFamilyRelationId, RowExclusiveLock);
				e->newoid = GetNewOidWithIndex(rel, OpfamilyOidIndexId, Anum_pg_opfamily_oid);
				table_close(rel, RowExclusiveLock);
				break;
			case OperatorClassRelationId:
				rel = table_open(OperatorClassRelationId, RowExclusiveLock);
				e->newoid = GetNewOidWithIndex(rel, OpclassOidIndexId, Anum_pg_opclass_oid);
				table_close(rel, RowExclusiveLock);
				break;
			case AccessMethodOperatorRelationId:
				rel = table_open(AccessMethodOperatorRelationId, RowExclusiveLock);
				e->newoid = GetNewOidWithIndex(rel, AccessMethodOperatorOidIndexId, Anum_pg_amop_oid);
				table_close(rel, RowExclusiveLock);
				break;
			case AccessMethodProcedureRelationId:
				rel = table_open(AccessMethodProcedureRelationId, RowExclusiveLock);
				e->newoid = GetNewOidWithIndex(rel, AccessMethodProcedureOidIndexId, Anum_pg_amproc_oid);
				table_close(rel, RowExclusiveLock);
				break;
			case StatisticExtRelationId:
				rel = table_open(StatisticExtRelationId, RowExclusiveLock);
				e->newoid = GetNewOidWithIndex(rel, StatisticExtOidIndexId,
											   Anum_pg_statistic_ext_oid);
				table_close(rel, RowExclusiveLock);
				break;
			case ExtensionRelationId:
				rel = table_open(ExtensionRelationId, RowExclusiveLock);
				e->newoid = GetNewOidWithIndex(rel, ExtensionOidIndexId,
											   Anum_pg_extension_oid);
				table_close(rel, RowExclusiveLock);
				break;
			case PublicationRelRelationId:
				rel = table_open(PublicationRelRelationId, RowExclusiveLock);
				e->newoid = GetNewOidWithIndex(rel, PublicationRelObjectIndexId,
											   Anum_pg_publication_rel_oid);
				table_close(rel, RowExclusiveLock);
				break;
			case PublicationNamespaceRelationId:
				rel = table_open(PublicationNamespaceRelationId, RowExclusiveLock);
				e->newoid = GetNewOidWithIndex(rel, PublicationNamespaceObjectIndexId,
											   Anum_pg_publication_namespace_oid);
				table_close(rel, RowExclusiveLock);
				break;
			case CastRelationId:
				rel = table_open(CastRelationId, RowExclusiveLock);
				e->newoid = GetNewOidWithIndex(rel, CastOidIndexId,
											   Anum_pg_cast_oid);
				table_close(rel, RowExclusiveLock);
				break;
			case DefaultAclRelationId:
				rel = table_open(DefaultAclRelationId, RowExclusiveLock);
				e->newoid = GetNewOidWithIndex(rel, DefaultAclOidIndexId,
											   Anum_pg_default_acl_oid);
				table_close(rel, RowExclusiveLock);
				break;
			case ProcedureRelationId:
				rel = table_open(ProcedureRelationId, RowExclusiveLock);
				e->newoid = GetNewOidWithIndex(rel, ProcedureOidIndexId,
											   Anum_pg_proc_oid);
				table_close(rel, RowExclusiveLock);
				break;
			case PolicyRelationId:
				rel = table_open(PolicyRelationId, RowExclusiveLock);
				e->newoid = GetNewOidWithIndex(rel, PolicyOidIndexId,
											   Anum_pg_policy_oid);
				table_close(rel, RowExclusiveLock);
				break;
			case RewriteRelationId:
				rel = table_open(RewriteRelationId, RowExclusiveLock);
				e->newoid = GetNewOidWithIndex(rel, RewriteOidIndexId,
											   Anum_pg_rewrite_oid);
				table_close(rel, RowExclusiveLock);
				break;
			default:
				refuse(obj.classid, obj.oid);
		}
	}
}

/* the copy's object id for (classid, oid), or oid: a shared object's */
static Oid
copy_map(CopyState *cs, Oid classid, Oid oid)
{
	CopyKey		key;
	CopyEntry  *e;

	if (!OidIsValid(oid))
		return oid;
	key.classid = classid;
	key.oid = oid;
	e = hash_search(cs->map, &key, HASH_FIND, NULL);
	return e != NULL ? e->newoid : oid;
}

/* ------------------------------------------------------------------------
 * Expression trees
 * ------------------------------------------------------------------------
 */

static Oid
map_type(CopyState *cs, Oid typid)
{
	return copy_map(cs, TypeRelationId, typid);
}

/* whether type typid's values are object ids (oid, regclass, ...) */
static bool
oid_valued_type(Oid typid)
{
	switch (typid)
	{
		case OIDOID:
		case REGCLASSOID:
		case REGTYPEOID:
		case REGPROCOID:
		case REGPROCEDUREOID:
		case REGOPEROID:
		case REGOPERATOROID:
		case REGNAMESPACEOID:
		case REGCOLLATIONOID:
		case REGCONFIGOID:
		case REGDICTIONARYOID:
		case REGROLEOID:
			return true;
	}
	return false;
}

/* an array of object ids' elements */
static void
oid_array_elems(Datum array, Datum **elems, int *n)
{
	ArrayType  *arr = DatumGetArrayTypeP(array);

	deconstruct_array(arr, ARR_ELEMTYPE(arr), sizeof(Oid), true, TYPALIGN_INT,
					  elems, NULL, n);
}

/* a constant of an object identifier type names an object: its copy's */
static void
map_const(CopyState *cs, Const *c)
{
	Oid			classid;

	/* an enum's value is its label's id: the copy's label's */
	if (in_copy(cs, TypeRelationId, c->consttype) &&
		get_typtype(c->consttype) == TYPTYPE_ENUM)
	{
		c->consttype = map_type(cs, c->consttype);
		if (!c->constisnull)
			c->constvalue = ObjectIdGetDatum(copy_map(cs, EnumRelationId,
													  DatumGetObjectId(c->constvalue)));
		return;
	}

	/*
	 * A value of another copied type, or an array of one (a composite's, an
	 * array's header names its type), would name the source's type: refused
	 */
	if (copy_map(cs, TypeRelationId, c->consttype) != c->consttype ||
		(OidIsValid(get_element_type(c->consttype)) &&
		 copy_map(cs, TypeRelationId, get_element_type(c->consttype)) !=
		 get_element_type(c->consttype)))
		refuse(cs->current.classid, cs->current.oid);
	if (c->constisnull)
		return;
	/* (an array of object ids can't be rewritten in place: none may be copied) */
	if (oid_valued_type(get_element_type(c->consttype)))
	{
		Datum	   *elems;
		int			n;

		oid_array_elems(c->constvalue, &elems, &n);
		for (int i = 0; i < n; i++)
		{
			Oid			oid = DatumGetObjectId(elems[i]);

			if (hash_search(cs->sources, &oid, HASH_FIND, NULL) != NULL)
				refuse(cs->current.classid, cs->current.oid);
		}
		return;
	}
	switch (c->consttype)
	{
		case REGCLASSOID:
			classid = RelationRelationId;
			break;
		case REGTYPEOID:
			classid = TypeRelationId;
			break;
		case REGNAMESPACEOID:
			classid = NamespaceRelationId;
			break;
		case REGPROCOID:
		case REGPROCEDUREOID:
			classid = ProcedureRelationId;
			break;
		case REGOPEROID:
		case REGOPERATOROID:
			classid = OperatorRelationId;
			break;
		default:
			return;
	}
	c->constvalue = ObjectIdGetDatum(copy_map(cs, classid,
											  DatumGetObjectId(c->constvalue)));
}

static List *
map_oid_list(CopyState *cs, Oid classid, List *oids)
{
	List	   *mapped = NIL;

	foreach_oid(oid, oids)
		mapped = lappend_oid(mapped, copy_map(cs, classid, oid));
	return mapped;
}

static Node *remap_mutator(Node *node, CopyState *cs);

/*
 * A query (a view's, a rule's, a SQL function body's, a subquery's): its
 * parts by the mutator, and what the mutator doesn't reach, its range table
 * entries and their permission entries, here.
 */
static Query *
remap_query(Query *query, CopyState *cs)
{
	Query	   *q = query_tree_mutator(query, remap_mutator, cs,
									   QTW_EXAMINE_SORTGROUP);

	foreach_node(RangeTblEntry, rte, q->rtable)
	{
		rte->relid = copy_map(cs, RelationRelationId, rte->relid);
		rte->coltypes = map_oid_list(cs, TypeRelationId, rte->coltypes);
	}
	foreach_node(RTEPermissionInfo, perm, q->rteperminfos)
		perm->relid = copy_map(cs, RelationRelationId, perm->relid);
	/* (the constraints, a primary key's, that a GROUP BY relies on) */
	q->constraintDeps = map_oid_list(cs, ConstraintRelationId, q->constraintDeps);
	return q;
}

/*
 * Rewrites the object ids of an expression tree's nodes, of the kinds a
 * default, a constraint, an index expression, a policy, a trigger's
 * condition, a view or rule, or a SQL function's body holds; any other kind
 * is refused, as a reference it might hold would not be rewritten.
 */
static Node *
remap_mutator(Node *node, CopyState *cs)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Query))
		return (Node *) remap_query((Query *) node, cs);
	node = expression_tree_mutator(node, remap_mutator, cs);
	switch (nodeTag(node))
	{
		case T_FromExpr:
		case T_JoinExpr:
		case T_RangeTblRef:
		case T_SubLink:
		case T_GroupingFunc:
		case T_WithCheckOption:
			break;
		case T_TargetEntry:
			((TargetEntry *) node)->resorigtbl =
				copy_map(cs, RelationRelationId, ((TargetEntry *) node)->resorigtbl);
			break;
		case T_SortGroupClause:
			{
				SortGroupClause *sgc = (SortGroupClause *) node;

				sgc->eqop = copy_map(cs, OperatorRelationId, sgc->eqop);
				sgc->sortop = copy_map(cs, OperatorRelationId, sgc->sortop);
				break;
			}
		case T_WindowClause:
			{
				WindowClause *wc = (WindowClause *) node;

				wc->startInRangeFunc = copy_map(cs, ProcedureRelationId, wc->startInRangeFunc);
				wc->endInRangeFunc = copy_map(cs, ProcedureRelationId, wc->endInRangeFunc);
				break;
			}
		case T_Aggref:
			{
				Aggref	   *a = (Aggref *) node;

				a->aggfnoid = copy_map(cs, ProcedureRelationId, a->aggfnoid);
				a->aggtype = map_type(cs, a->aggtype);
				a->aggtranstype = map_type(cs, a->aggtranstype);
				a->aggargtypes = map_oid_list(cs, TypeRelationId, a->aggargtypes);
				break;
			}
		case T_WindowFunc:
			{
				WindowFunc *w = (WindowFunc *) node;

				w->winfnoid = copy_map(cs, ProcedureRelationId, w->winfnoid);
				w->wintype = map_type(cs, w->wintype);
				break;
			}
		case T_CommonTableExpr:
			((CommonTableExpr *) node)->ctecoltypes =
				map_oid_list(cs, TypeRelationId, ((CommonTableExpr *) node)->ctecoltypes);
			break;
		case T_RangeTblFunction:
			((RangeTblFunction *) node)->funccoltypes =
				map_oid_list(cs, TypeRelationId, ((RangeTblFunction *) node)->funccoltypes);
			break;
		case T_SetOperationStmt:
			((SetOperationStmt *) node)->colTypes =
				map_oid_list(cs, TypeRelationId, ((SetOperationStmt *) node)->colTypes);
			break;
		case T_SetToDefault:
			((SetToDefault *) node)->typeId = map_type(cs, ((SetToDefault *) node)->typeId);
			break;
		case T_List:
		case T_BoolExpr:
		case T_NullTest:
		case T_BooleanTest:
		case T_CaseWhen:
		case T_CollateExpr:
			break;
		case T_Var:
			((Var *) node)->vartype = map_type(cs, ((Var *) node)->vartype);
			break;
		case T_Const:
			map_const(cs, (Const *) node);
			break;
		case T_Param:
			((Param *) node)->paramtype = map_type(cs, ((Param *) node)->paramtype);
			break;
		case T_FuncExpr:
			{
				FuncExpr   *f = (FuncExpr *) node;

				f->funcid = copy_map(cs, ProcedureRelationId, f->funcid);
				f->funcresulttype = map_type(cs, f->funcresulttype);
				break;
			}
		case T_OpExpr:
		case T_DistinctExpr:
		case T_NullIfExpr:
			{
				OpExpr	   *o = (OpExpr *) node;

				o->opno = copy_map(cs, OperatorRelationId, o->opno);
				o->opfuncid = copy_map(cs, ProcedureRelationId, o->opfuncid);
				o->opresulttype = map_type(cs, o->opresulttype);
				break;
			}
		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *o = (ScalarArrayOpExpr *) node;

				o->opno = copy_map(cs, OperatorRelationId, o->opno);
				o->opfuncid = copy_map(cs, ProcedureRelationId, o->opfuncid);
				o->hashfuncid = copy_map(cs, ProcedureRelationId, o->hashfuncid);
				o->negfuncid = copy_map(cs, ProcedureRelationId, o->negfuncid);
				break;
			}
		case T_RelabelType:
			((RelabelType *) node)->resulttype = map_type(cs, ((RelabelType *) node)->resulttype);
			break;
		case T_CoerceViaIO:
			((CoerceViaIO *) node)->resulttype = map_type(cs, ((CoerceViaIO *) node)->resulttype);
			break;
		case T_ArrayCoerceExpr:
			((ArrayCoerceExpr *) node)->resulttype = map_type(cs, ((ArrayCoerceExpr *) node)->resulttype);
			break;
		case T_CaseExpr:
			((CaseExpr *) node)->casetype = map_type(cs, ((CaseExpr *) node)->casetype);
			break;
		case T_CaseTestExpr:
			((CaseTestExpr *) node)->typeId = map_type(cs, ((CaseTestExpr *) node)->typeId);
			break;
		case T_ArrayExpr:
			{
				ArrayExpr  *a = (ArrayExpr *) node;

				a->array_typeid = map_type(cs, a->array_typeid);
				a->element_typeid = map_type(cs, a->element_typeid);
				break;
			}
		case T_RowExpr:
			((RowExpr *) node)->row_typeid = map_type(cs, ((RowExpr *) node)->row_typeid);
			break;
		case T_CoalesceExpr:
			((CoalesceExpr *) node)->coalescetype = map_type(cs, ((CoalesceExpr *) node)->coalescetype);
			break;
		case T_MinMaxExpr:
			((MinMaxExpr *) node)->minmaxtype = map_type(cs, ((MinMaxExpr *) node)->minmaxtype);
			break;
		case T_SQLValueFunction:
			((SQLValueFunction *) node)->type = map_type(cs, ((SQLValueFunction *) node)->type);
			break;
		case T_FieldSelect:
			((FieldSelect *) node)->resulttype = map_type(cs, ((FieldSelect *) node)->resulttype);
			break;
		case T_NextValueExpr:
			{
				NextValueExpr *n = (NextValueExpr *) node;

				n->seqid = copy_map(cs, RelationRelationId, n->seqid);
				n->typeId = map_type(cs, n->typeId);
				break;
			}
		case T_SubscriptingRef:
			{
				SubscriptingRef *s = (SubscriptingRef *) node;

				s->refcontainertype = map_type(cs, s->refcontainertype);
				s->refelemtype = map_type(cs, s->refelemtype);
				s->refrestype = map_type(cs, s->refrestype);
				break;
			}
		case T_NamedArgExpr:
		case T_JsonIsPredicate:
		case T_PartitionBoundSpec:
		case T_PartitionRangeDatum:
			break;
		case T_RowCompareExpr:
			((RowCompareExpr *) node)->opnos =
				map_oid_list(cs, OperatorRelationId, ((RowCompareExpr *) node)->opnos);
			break;
		case T_FieldStore:
			((FieldStore *) node)->resulttype = map_type(cs, ((FieldStore *) node)->resulttype);
			break;
		case T_ConvertRowtypeExpr:
			((ConvertRowtypeExpr *) node)->resulttype =
				map_type(cs, ((ConvertRowtypeExpr *) node)->resulttype);
			break;
		case T_CoerceToDomain:
			((CoerceToDomain *) node)->resulttype = map_type(cs, ((CoerceToDomain *) node)->resulttype);
			break;
		case T_CoerceToDomainValue:
			((CoerceToDomainValue *) node)->typeId = map_type(cs, ((CoerceToDomainValue *) node)->typeId);
			break;
		default:
			{
				ObjectAddress addr;

				ObjectAddressSet(addr, cs->current.classid, cs->current.oid);
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot copy %s to another branch yet",
								getObjectDescription(&addr, false)),
						 errdetail("Its expression has a node of type %d, which the copy doesn't rewrite yet.",
								   (int) nodeTag(node))));
			}
	}
	return node;
}

static Datum
remap_tree(CopyState *cs, Datum tree)
{
	Node	   *node = stringToNode(TextDatumGetCString(tree));

	node = remap_mutator(node, cs);
	return CStringGetTextDatum(nodeToString(node));
}

/* ------------------------------------------------------------------------
 * Copying rows
 * ------------------------------------------------------------------------
 */

/*
 * Makes the copy of a catalog row: values[] of the source row, changed by
 * the caller, inserted, and kept for the check of rows made.
 */
static void
insert_row(CopyState *cs, Relation rel, Datum *values, bool *nulls)
{
	HeapTuple	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	MadeRow    *made = palloc(sizeof(MadeRow));

	CatalogTupleInsert(rel, tup);
	made->catid = RelationGetRelid(rel);
	made->tup = tup;
	cs->made = lappend(cs->made, made);
}

/* the rows of catalog catid whose columns key match, each passed to fix */
typedef void (*FixRow) (CopyState *cs, Datum *values, bool *nulls);

static void
copy_rows(CopyState *cs, Oid catid, Oid indexid, int nkeys, ScanKey keys,
		  FixRow fix)
{
	Relation	rel = table_open(catid, RowExclusiveLock);
	TupleDesc	desc = RelationGetDescr(rel);
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *rows = NIL;

	/* (read all, then insert: an insert must not meet its own row) */
	scan = systable_beginscan(rel, indexid, true, NULL, nkeys, keys);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
		rows = lappend(rows, heap_copytuple(tup));
	systable_endscan(scan);

	foreach_ptr(HeapTupleData, row, rows)
	{
		Datum	   *values = palloc_array(Datum, desc->natts);
		bool	   *nulls = palloc_array(bool, desc->natts);

		heap_deform_tuple(row, desc, values, nulls);
		fix(cs, values, nulls);
		insert_row(cs, rel, values, nulls);
	}
	table_close(rel, RowExclusiveLock);
}

/* the rows of catalog catid keyed (by its index) on one object id column */
static void
copy_rows_of(CopyState *cs, Oid catid, Oid indexid, AttrNumber att, Oid oid,
			 FixRow fix)
{
	ScanKeyData key;

	ScanKeyInit(&key, att, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(oid));
	copy_rows(cs, catid, indexid, 1, &key, fix);
}

#define MAP(att, classid) \
	(values[(att) - 1] = ObjectIdGetDatum(copy_map(cs, (classid), \
												   DatumGetObjectId(values[(att) - 1]))))
#define MAP_TREE(att) \
	do { \
		if (!nulls[(att) - 1]) \
			values[(att) - 1] = remap_tree(cs, values[(att) - 1]); \
	} while (0)

static void
fix_namespace(CopyState *cs, Datum *values, bool *nulls)
{
	MAP(Anum_pg_namespace_oid, NamespaceRelationId);
	values[Anum_pg_namespace_nspbranch - 1] = Int32GetDatum(cs->dst);
}

/*
 * A TOAST table's name, and its index's, name its table's object id, in
 * the one schema pg_toast: the copy's name its table's copy.
 */
static Datum
toast_name(CopyState *cs, Datum name)
{
	unsigned int tableoid;
	int			len;
	NameData   *newname = palloc0(sizeof(NameData));
	const char *old = NameStr(*DatumGetName(name));

	if (sscanf(old, "pg_toast_%u%n", &tableoid, &len) != 1)
		elog(ERROR, "unexpected name of TOAST relation: \"%s\"", old);
	snprintf(NameStr(*newname), NAMEDATALEN, "pg_toast_%u%s",
			 copy_map(cs, RelationRelationId, tableoid), old + len);
	return NameGetDatum(newname);
}

static void
fix_class(CopyState *cs, Datum *values, bool *nulls)
{
	char		relkind = DatumGetChar(values[Anum_pg_class_relkind - 1]);

	if (DatumGetObjectId(values[Anum_pg_class_relnamespace - 1]) == PG_TOAST_NAMESPACE)
		values[Anum_pg_class_relname - 1] = toast_name(cs, values[Anum_pg_class_relname - 1]);

	MAP(Anum_pg_class_oid, RelationRelationId);
	MAP(Anum_pg_class_relnamespace, NamespaceRelationId);
	MAP(Anum_pg_class_reltype, TypeRelationId);
	MAP(Anum_pg_class_reltoastrelid, RelationRelationId);
	MAP_TREE(Anum_pg_class_relpartbound);
	/* the relation's storage is new: its file number is its object id */
	if (RELKIND_HAS_STORAGE(relkind))
		values[Anum_pg_class_relfilenode - 1] = values[Anum_pg_class_oid - 1];
	values[Anum_pg_class_relrewrite - 1] = ObjectIdGetDatum(InvalidOid);
	if (relkind == RELKIND_RELATION || relkind == RELKIND_TOASTVALUE ||
		relkind == RELKIND_MATVIEW)
	{
		values[Anum_pg_class_relfrozenxid - 1] = TransactionIdGetDatum(RecentXmin);
		values[Anum_pg_class_relminmxid - 1] = MultiXactIdGetDatum(GetOldestMultiXactId());
	}
}

static void
fix_attribute(CopyState *cs, Datum *values, bool *nulls)
{
	MAP(Anum_pg_attribute_attrelid, RelationRelationId);
	MAP(Anum_pg_attribute_atttypid, TypeRelationId);
}

static void
fix_type(CopyState *cs, Datum *values, bool *nulls)
{
	if (DatumGetObjectId(values[Anum_pg_type_typnamespace - 1]) == PG_TOAST_NAMESPACE)
		values[Anum_pg_type_typname - 1] = toast_name(cs, values[Anum_pg_type_typname - 1]);
	MAP(Anum_pg_type_oid, TypeRelationId);
	MAP(Anum_pg_type_typnamespace, NamespaceRelationId);
	MAP(Anum_pg_type_typrelid, RelationRelationId);
	MAP(Anum_pg_type_typelem, TypeRelationId);
	MAP(Anum_pg_type_typarray, TypeRelationId);
	MAP(Anum_pg_type_typbasetype, TypeRelationId);
	MAP(Anum_pg_type_typsubscript, ProcedureRelationId);
	MAP(Anum_pg_type_typinput, ProcedureRelationId);
	MAP(Anum_pg_type_typoutput, ProcedureRelationId);
	MAP(Anum_pg_type_typreceive, ProcedureRelationId);
	MAP(Anum_pg_type_typsend, ProcedureRelationId);
	MAP(Anum_pg_type_typmodin, ProcedureRelationId);
	MAP(Anum_pg_type_typmodout, ProcedureRelationId);
	MAP(Anum_pg_type_typanalyze, ProcedureRelationId);
	MAP_TREE(Anum_pg_type_typdefaultbin);
}

static void
fix_range(CopyState *cs, Datum *values, bool *nulls)
{
	MAP(Anum_pg_range_rngtypid, TypeRelationId);
	MAP(Anum_pg_range_rngsubtype, TypeRelationId);
	MAP(Anum_pg_range_rngmultitypid, TypeRelationId);
	MAP(Anum_pg_range_rngconstruct2, ProcedureRelationId);
	MAP(Anum_pg_range_rngconstruct3, ProcedureRelationId);
	MAP(Anum_pg_range_rngmltconstruct0, ProcedureRelationId);
	MAP(Anum_pg_range_rngmltconstruct1, ProcedureRelationId);
	MAP(Anum_pg_range_rngmltconstruct2, ProcedureRelationId);
	MAP(Anum_pg_range_rngcanonical, ProcedureRelationId);
	MAP(Anum_pg_range_rngsubdiff, ProcedureRelationId);
}

/* an oidvector's or oid[]'s object ids, each its copy's */
static Datum
map_oidvector(CopyState *cs, Oid classid, Datum v)
{
	oidvector  *old = (oidvector *) DatumGetPointer(v);
	Oid		   *oids = palloc_array(Oid, Max(old->dim1, 1));

	for (int i = 0; i < old->dim1; i++)
		oids[i] = copy_map(cs, classid, old->values[i]);
	return PointerGetDatum(buildoidvector(oids, old->dim1));
}

static Datum
map_oidarray(CopyState *cs, Oid classid, Datum a)
{
	Datum	   *elems;
	int			n;

	deconstruct_array_builtin(DatumGetArrayTypeP(a), OIDOID, &elems, NULL, &n);
	for (int i = 0; i < n; i++)
		elems[i] = ObjectIdGetDatum(copy_map(cs, classid, DatumGetObjectId(elems[i])));
	return PointerGetDatum(construct_array_builtin(elems, n, OIDOID));
}

static void
fix_proc(CopyState *cs, Datum *values, bool *nulls)
{
	MAP(Anum_pg_proc_oid, ProcedureRelationId);
	MAP(Anum_pg_proc_pronamespace, NamespaceRelationId);
	MAP(Anum_pg_proc_prorettype, TypeRelationId);
	MAP(Anum_pg_proc_provariadic, TypeRelationId);
	MAP(Anum_pg_proc_prosupport, ProcedureRelationId);
	values[Anum_pg_proc_proargtypes - 1] =
		map_oidvector(cs, TypeRelationId, values[Anum_pg_proc_proargtypes - 1]);
	if (!nulls[Anum_pg_proc_proallargtypes - 1])
		values[Anum_pg_proc_proallargtypes - 1] =
			map_oidarray(cs, TypeRelationId, values[Anum_pg_proc_proallargtypes - 1]);
	if (!nulls[Anum_pg_proc_protrftypes - 1])
		values[Anum_pg_proc_protrftypes - 1] =
			map_oidarray(cs, TypeRelationId, values[Anum_pg_proc_protrftypes - 1]);
	MAP_TREE(Anum_pg_proc_prosqlbody);
	MAP_TREE(Anum_pg_proc_proargdefaults);
}

static void
fix_policy(CopyState *cs, Datum *values, bool *nulls)
{
	MAP(Anum_pg_policy_oid, PolicyRelationId);
	MAP(Anum_pg_policy_polrelid, RelationRelationId);
	MAP_TREE(Anum_pg_policy_polqual);
	MAP_TREE(Anum_pg_policy_polwithcheck);
}

static void
fix_rewrite(CopyState *cs, Datum *values, bool *nulls)
{
	MAP(Anum_pg_rewrite_oid, RewriteRelationId);
	MAP(Anum_pg_rewrite_ev_class, RelationRelationId);
	MAP_TREE(Anum_pg_rewrite_ev_qual);
	MAP_TREE(Anum_pg_rewrite_ev_action);
}

/* An enum's labels, each anew, under the ids assign_enum_oids() gave them */
static void
copy_enum(CopyState *cs, Oid typid)
{
	Relation	rel = table_open(EnumRelationId, RowExclusiveLock);
	TupleDesc	desc = RelationGetDescr(rel);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *rows = NIL;

	ScanKeyInit(&key, Anum_pg_enum_enumtypid, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(typid));
	scan = systable_beginscan(rel, EnumTypIdSortOrderIndexId, true, NULL, 1, &key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
		rows = lappend(rows, heap_copytuple(tup));
	systable_endscan(scan);

	foreach_ptr(HeapTupleData, row, rows)
	{
		Datum	   *values = palloc_array(Datum, desc->natts);
		bool	   *nulls = palloc_array(bool, desc->natts);

		heap_deform_tuple(row, desc, values, nulls);
		MAP(Anum_pg_enum_oid, EnumRelationId);
		MAP(Anum_pg_enum_enumtypid, TypeRelationId);
		insert_row(cs, rel, values, nulls);
	}
	table_close(rel, RowExclusiveLock);
}

static void
fix_index(CopyState *cs, Datum *values, bool *nulls)
{
	MAP(Anum_pg_index_indexrelid, RelationRelationId);
	MAP(Anum_pg_index_indrelid, RelationRelationId);
	values[Anum_pg_index_indclass - 1] =
		map_oidvector(cs, OperatorClassRelationId, values[Anum_pg_index_indclass - 1]);
	MAP_TREE(Anum_pg_index_indexprs);
	MAP_TREE(Anum_pg_index_indpred);
}

static void
fix_constraint(CopyState *cs, Datum *values, bool *nulls)
{
	MAP(Anum_pg_constraint_oid, ConstraintRelationId);
	MAP(Anum_pg_constraint_connamespace, NamespaceRelationId);
	MAP(Anum_pg_constraint_conrelid, RelationRelationId);
	MAP(Anum_pg_constraint_contypid, TypeRelationId);
	MAP(Anum_pg_constraint_conindid, RelationRelationId);
	MAP(Anum_pg_constraint_confrelid, RelationRelationId);
	MAP(Anum_pg_constraint_conparentid, ConstraintRelationId);
	for (int a = 0; a < 4; a++)
	{
		static const AttrNumber ops[] = {Anum_pg_constraint_conpfeqop,
			Anum_pg_constraint_conppeqop, Anum_pg_constraint_conffeqop,
		Anum_pg_constraint_conexclop};

		if (!nulls[ops[a] - 1])
			values[ops[a] - 1] = map_oidarray(cs, OperatorRelationId, values[ops[a] - 1]);
	}
	MAP_TREE(Anum_pg_constraint_conbin);
}

static void
fix_trigger(CopyState *cs, Datum *values, bool *nulls)
{
	const char *name = NameStr(*DatumGetName(values[Anum_pg_trigger_tgname - 1]));
	char		kind;
	unsigned int oid;

	MAP(Anum_pg_trigger_oid, TriggerRelationId);

	/*
	 * A foreign key's trigger is named after its object id, and on a shared
	 * referenced table, which its copy's is on too: named after the copy's
	 */
	if (sscanf(name, "RI_ConstraintTrigger_%c_%u", &kind, &oid) == 2)
	{
		NameData   *newname = palloc0(sizeof(NameData));

		snprintf(NameStr(*newname), NAMEDATALEN, "RI_ConstraintTrigger_%c_%u",
				 kind, DatumGetObjectId(values[Anum_pg_trigger_oid - 1]));
		values[Anum_pg_trigger_tgname - 1] = NameGetDatum(newname);
	}
	MAP(Anum_pg_trigger_tgrelid, RelationRelationId);
	MAP(Anum_pg_trigger_tgconstrrelid, RelationRelationId);
	MAP(Anum_pg_trigger_tgconstrindid, RelationRelationId);
	MAP(Anum_pg_trigger_tgconstraint, ConstraintRelationId);
	MAP(Anum_pg_trigger_tgfoid, ProcedureRelationId);
	MAP(Anum_pg_trigger_tgparentid, TriggerRelationId);
	MAP_TREE(Anum_pg_trigger_tgqual);
}

static void
fix_operator(CopyState *cs, Datum *values, bool *nulls)
{
	MAP(Anum_pg_operator_oid, OperatorRelationId);
	MAP(Anum_pg_operator_oprnamespace, NamespaceRelationId);
	MAP(Anum_pg_operator_oprleft, TypeRelationId);
	MAP(Anum_pg_operator_oprright, TypeRelationId);
	MAP(Anum_pg_operator_oprresult, TypeRelationId);
	MAP(Anum_pg_operator_oprcom, OperatorRelationId);
	MAP(Anum_pg_operator_oprnegate, OperatorRelationId);
	MAP(Anum_pg_operator_oprcode, ProcedureRelationId);
	MAP(Anum_pg_operator_oprrest, ProcedureRelationId);
	MAP(Anum_pg_operator_oprjoin, ProcedureRelationId);
}

static void
fix_opfamily(CopyState *cs, Datum *values, bool *nulls)
{
	MAP(Anum_pg_opfamily_oid, OperatorFamilyRelationId);
	MAP(Anum_pg_opfamily_opfnamespace, NamespaceRelationId);
}

static void
fix_opclass(CopyState *cs, Datum *values, bool *nulls)
{
	MAP(Anum_pg_opclass_oid, OperatorClassRelationId);
	MAP(Anum_pg_opclass_opcnamespace, NamespaceRelationId);
	MAP(Anum_pg_opclass_opcfamily, OperatorFamilyRelationId);
	MAP(Anum_pg_opclass_opcintype, TypeRelationId);
	MAP(Anum_pg_opclass_opckeytype, TypeRelationId);
}

static void
fix_amop(CopyState *cs, Datum *values, bool *nulls)
{
	MAP(Anum_pg_amop_oid, AccessMethodOperatorRelationId);
	MAP(Anum_pg_amop_amopfamily, OperatorFamilyRelationId);
	MAP(Anum_pg_amop_amoplefttype, TypeRelationId);
	MAP(Anum_pg_amop_amoprighttype, TypeRelationId);
	MAP(Anum_pg_amop_amopopr, OperatorRelationId);
	MAP(Anum_pg_amop_amopsortfamily, OperatorFamilyRelationId);
}

static void
fix_amproc(CopyState *cs, Datum *values, bool *nulls)
{
	MAP(Anum_pg_amproc_oid, AccessMethodProcedureRelationId);
	MAP(Anum_pg_amproc_amprocfamily, OperatorFamilyRelationId);
	MAP(Anum_pg_amproc_amproclefttype, TypeRelationId);
	MAP(Anum_pg_amproc_amprocrighttype, TypeRelationId);
	MAP(Anum_pg_amproc_amproc, ProcedureRelationId);
}

static void
fix_inherits(CopyState *cs, Datum *values, bool *nulls)
{
	MAP(Anum_pg_inherits_inhrelid, RelationRelationId);
	MAP(Anum_pg_inherits_inhparent, RelationRelationId);
}

static void
fix_partitioned(CopyState *cs, Datum *values, bool *nulls)
{
	MAP(Anum_pg_partitioned_table_partrelid, RelationRelationId);
	MAP(Anum_pg_partitioned_table_partdefid, RelationRelationId);
	values[Anum_pg_partitioned_table_partclass - 1] =
		map_oidvector(cs, OperatorClassRelationId,
					  values[Anum_pg_partitioned_table_partclass - 1]);
	MAP_TREE(Anum_pg_partitioned_table_partexprs);
}

static void
fix_aggregate(CopyState *cs, Datum *values, bool *nulls)
{
	MAP(Anum_pg_aggregate_aggfnoid, ProcedureRelationId);
	MAP(Anum_pg_aggregate_aggtransfn, ProcedureRelationId);
	MAP(Anum_pg_aggregate_aggfinalfn, ProcedureRelationId);
	MAP(Anum_pg_aggregate_aggcombinefn, ProcedureRelationId);
	MAP(Anum_pg_aggregate_aggserialfn, ProcedureRelationId);
	MAP(Anum_pg_aggregate_aggdeserialfn, ProcedureRelationId);
	MAP(Anum_pg_aggregate_aggmtransfn, ProcedureRelationId);
	MAP(Anum_pg_aggregate_aggminvtransfn, ProcedureRelationId);
	MAP(Anum_pg_aggregate_aggmfinalfn, ProcedureRelationId);
	MAP(Anum_pg_aggregate_aggsortop, OperatorRelationId);
	MAP(Anum_pg_aggregate_aggtranstype, TypeRelationId);
	MAP(Anum_pg_aggregate_aggmtranstype, TypeRelationId);
}

static void
fix_statistic_ext(CopyState *cs, Datum *values, bool *nulls)
{
	MAP(Anum_pg_statistic_ext_oid, StatisticExtRelationId);
	MAP(Anum_pg_statistic_ext_stxrelid, RelationRelationId);
	MAP(Anum_pg_statistic_ext_stxnamespace, NamespaceRelationId);
	MAP_TREE(Anum_pg_statistic_ext_stxexprs);
}

static void
fix_extension(CopyState *cs, Datum *values, bool *nulls)
{
	MAP(Anum_pg_extension_oid, ExtensionRelationId);
	MAP(Anum_pg_extension_extnamespace, NamespaceRelationId);
	if (!nulls[Anum_pg_extension_extconfig - 1])
		values[Anum_pg_extension_extconfig - 1] =
			map_oidarray(cs, RelationRelationId, values[Anum_pg_extension_extconfig - 1]);
}

/*
 * A shared publication a copied table or schema is a member of: locked as
 * ALTER PUBLICATION locks it, against its drop, and there still.
 */
static void
lock_publication(Oid pubid)
{
	LockDatabaseObject(PublicationRelationId, pubid, 0, AccessShareLock);
	if (!SearchSysCacheExists1(PUBLICATIONOID, ObjectIdGetDatum(pubid)))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("a publication of the branch's tables was dropped while it was being copied")));
}

static void
fix_publication_rel(CopyState *cs, Datum *values, bool *nulls)
{
	lock_publication(DatumGetObjectId(values[Anum_pg_publication_rel_prpubid - 1]));
	MAP(Anum_pg_publication_rel_oid, PublicationRelRelationId);
	MAP(Anum_pg_publication_rel_prrelid, RelationRelationId);
	MAP_TREE(Anum_pg_publication_rel_prqual);
}

static void
fix_publication_namespace(CopyState *cs, Datum *values, bool *nulls)
{
	lock_publication(DatumGetObjectId(values[Anum_pg_publication_namespace_pnpubid - 1]));
	MAP(Anum_pg_publication_namespace_oid, PublicationNamespaceRelationId);
	MAP(Anum_pg_publication_namespace_pnnspid, NamespaceRelationId);
}

static void
fix_cast(CopyState *cs, Datum *values, bool *nulls)
{
	MAP(Anum_pg_cast_oid, CastRelationId);
	MAP(Anum_pg_cast_castsource, TypeRelationId);
	MAP(Anum_pg_cast_casttarget, TypeRelationId);
	MAP(Anum_pg_cast_castfunc, ProcedureRelationId);
}

static void
fix_default_acl(CopyState *cs, Datum *values, bool *nulls)
{
	MAP(Anum_pg_default_acl_oid, DefaultAclRelationId);
	MAP(Anum_pg_default_acl_defaclnamespace, NamespaceRelationId);
}

static void
fix_attrdef(CopyState *cs, Datum *values, bool *nulls)
{
	MAP(Anum_pg_attrdef_oid, AttrDefaultRelationId);
	MAP(Anum_pg_attrdef_adrelid, RelationRelationId);
	MAP_TREE(Anum_pg_attrdef_adbin);
}

static void
fix_sequence(CopyState *cs, Datum *values, bool *nulls)
{
	MAP(Anum_pg_sequence_seqrelid, RelationRelationId);
	MAP(Anum_pg_sequence_seqtypid, TypeRelationId);
}

static void
fix_depend(CopyState *cs, Datum *values, bool *nulls)
{
	Oid			classid = DatumGetObjectId(values[Anum_pg_depend_classid - 1]);
	Oid			refclassid = DatumGetObjectId(values[Anum_pg_depend_refclassid - 1]);

	MAP(Anum_pg_depend_objid, classid);
	MAP(Anum_pg_depend_refobjid, refclassid);
}

static void
fix_shdepend(CopyState *cs, Datum *values, bool *nulls)
{
	Oid			classid = DatumGetObjectId(values[Anum_pg_shdepend_classid - 1]);

	MAP(Anum_pg_shdepend_objid, classid);
}

static void
fix_description(CopyState *cs, Datum *values, bool *nulls)
{
	Oid			classid = DatumGetObjectId(values[Anum_pg_description_classoid - 1]);

	MAP(Anum_pg_description_objoid, classid);
}

static void
fix_statistic(CopyState *cs, Datum *values, bool *nulls)
{
	MAP(Anum_pg_statistic_starelid, RelationRelationId);
	MAP(Anum_pg_statistic_staop1, OperatorRelationId);
	MAP(Anum_pg_statistic_staop2, OperatorRelationId);
	MAP(Anum_pg_statistic_staop3, OperatorRelationId);
	MAP(Anum_pg_statistic_staop4, OperatorRelationId);
	MAP(Anum_pg_statistic_staop5, OperatorRelationId);

	/*
	 * A slot's values are an array of the column's type, whose header names
	 * it: a copied type's (a base type's values name no object), the copy's
	 */
	for (int k = 0; k < STATISTIC_NUM_SLOTS; k++)
	{
		AttrNumber	att = Anum_pg_statistic_stavalues1 + k;

		if (!nulls[att - 1])
		{
			ArrayType  *arr = DatumGetArrayTypePCopy(values[att - 1]);

			ARR_ELEMTYPE(arr) = copy_map(cs, TypeRelationId, ARR_ELEMTYPE(arr));
			values[att - 1] = PointerGetDatum(arr);
		}
	}
}

/* errors out if any row of catalog catid names one of the objects */
static void
refuse_rows(CopyState *cs, Oid catid, Oid indexid, AttrNumber objatt,
			AttrNumber classatt, const char *what)
{
	Relation	rel = table_open(catid, AccessShareLock);

	for (int i = 0; i < cs->nobjs; i++)
	{
		ScanKeyData key[2];
		SysScanDesc scan;

		ScanKeyInit(&key[0], objatt, BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(cs->objs[i].oid));
		ScanKeyInit(&key[1], classatt, BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(cs->objs[i].classid));
		scan = systable_beginscan(rel, indexid, true, NULL, 2, key);
		if (HeapTupleIsValid(systable_getnext(scan)))
		{
			ObjectAddress addr;

			ObjectAddressSet(addr, cs->objs[i].classid, cs->objs[i].oid);
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot copy the %s of %s to another branch yet",
							what, getObjectDescription(&addr, false))));
		}
		systable_endscan(scan);
	}
	table_close(rel, AccessShareLock);
}

static void
copy_all_rows(CopyState *cs)
{
	refuse_rows(cs, SecLabelRelationId, SecLabelObjectIndexId,
				Anum_pg_seclabel_objoid, Anum_pg_seclabel_classoid,
				"security label");
	refuse_rows(cs, InitPrivsRelationId, InitPrivsObjIndexId,
				Anum_pg_init_privs_objoid, Anum_pg_init_privs_classoid,
				"initial privileges");

	for (int i = 0; i < cs->nobjs; i++)
	{
		CopyKey		obj = cs->objs[i];
		ScanKeyData key[3];

		cs->current = obj;

		switch (obj.classid)
		{
			case NamespaceRelationId:
				NamespaceCheckNameFree(get_namespace_name(obj.oid), cs->dst);
				copy_rows_of(cs, NamespaceRelationId, NamespaceOidIndexId,
							 Anum_pg_namespace_oid, obj.oid, fix_namespace);
				break;
			case RelationRelationId:
				copy_rows_of(cs, RelationRelationId, ClassOidIndexId,
							 Anum_pg_class_oid, obj.oid, fix_class);
				copy_rows_of(cs, AttributeRelationId, AttributeRelidNumIndexId,
							 Anum_pg_attribute_attrelid, obj.oid, fix_attribute);
				if (get_rel_relkind(obj.oid) == RELKIND_INDEX ||
					get_rel_relkind(obj.oid) == RELKIND_PARTITIONED_INDEX)
					copy_rows_of(cs, IndexRelationId, IndexRelidIndexId,
								 Anum_pg_index_indexrelid, obj.oid, fix_index);
				if (get_rel_relkind(obj.oid) == RELKIND_SEQUENCE)
					copy_rows_of(cs, SequenceRelationId, SequenceRelidIndexId,
								 Anum_pg_sequence_seqrelid, obj.oid, fix_sequence);
				copy_rows_of(cs, StatisticRelationId, StatisticRelidAttnumInhIndexId,
							 Anum_pg_statistic_starelid, obj.oid, fix_statistic);
				/* a partition's, or a child's, place under its parent */
				copy_rows_of(cs, InheritsRelationId, InheritsRelidSeqnoIndexId,
							 Anum_pg_inherits_inhrelid, obj.oid, fix_inherits);
				if (get_rel_relkind(obj.oid) == RELKIND_PARTITIONED_TABLE)
					copy_rows_of(cs, PartitionedRelationId, PartitionedRelidIndexId,
								 Anum_pg_partitioned_table_partrelid, obj.oid,
								 fix_partitioned);
				break;
			case TypeRelationId:
				copy_rows_of(cs, TypeRelationId, TypeOidIndexId,
							 Anum_pg_type_oid, obj.oid, fix_type);
				switch (get_typtype(obj.oid))
				{
					case TYPTYPE_ENUM:
						copy_enum(cs, obj.oid);
						break;
					case TYPTYPE_RANGE:
						copy_rows_of(cs, RangeRelationId, RangeTypidIndexId,
									 Anum_pg_range_rngtypid, obj.oid, fix_range);
						break;
				}
				break;
			case ProcedureRelationId:
				copy_rows_of(cs, ProcedureRelationId, ProcedureOidIndexId,
							 Anum_pg_proc_oid, obj.oid, fix_proc);
				if (get_func_prokind(obj.oid) == PROKIND_AGGREGATE)
					copy_rows_of(cs, AggregateRelationId, AggregateFnoidIndexId,
								 Anum_pg_aggregate_aggfnoid, obj.oid, fix_aggregate);
				break;
			case OperatorRelationId:
				copy_rows_of(cs, OperatorRelationId, OperatorOidIndexId, Anum_pg_operator_oid, obj.oid, fix_operator);
				break;
			case OperatorFamilyRelationId:
				copy_rows_of(cs, OperatorFamilyRelationId, OpfamilyOidIndexId, Anum_pg_opfamily_oid, obj.oid, fix_opfamily);
				break;
			case OperatorClassRelationId:
				copy_rows_of(cs, OperatorClassRelationId, OpclassOidIndexId, Anum_pg_opclass_oid, obj.oid, fix_opclass);
				break;
			case AccessMethodOperatorRelationId:
				copy_rows_of(cs, AccessMethodOperatorRelationId, AccessMethodOperatorOidIndexId, Anum_pg_amop_oid, obj.oid, fix_amop);
				break;
			case AccessMethodProcedureRelationId:
				copy_rows_of(cs, AccessMethodProcedureRelationId, AccessMethodProcedureOidIndexId, Anum_pg_amproc_oid, obj.oid, fix_amproc);
				break;
			case StatisticExtRelationId:
				/* (its data, which names types, is ANALYZE's to make again) */
				copy_rows_of(cs, StatisticExtRelationId, StatisticExtOidIndexId,
							 Anum_pg_statistic_ext_oid, obj.oid, fix_statistic_ext);
				break;
			case ExtensionRelationId:
				copy_rows_of(cs, ExtensionRelationId, ExtensionOidIndexId,
							 Anum_pg_extension_oid, obj.oid, fix_extension);
				break;
			case PublicationRelRelationId:
				copy_rows_of(cs, PublicationRelRelationId, PublicationRelObjectIndexId,
							 Anum_pg_publication_rel_oid, obj.oid, fix_publication_rel);
				break;
			case PublicationNamespaceRelationId:
				copy_rows_of(cs, PublicationNamespaceRelationId,
							 PublicationNamespaceObjectIndexId,
							 Anum_pg_publication_namespace_oid, obj.oid,
							 fix_publication_namespace);
				break;
			case PolicyRelationId:
				copy_rows_of(cs, PolicyRelationId, PolicyOidIndexId,
							 Anum_pg_policy_oid, obj.oid, fix_policy);
				break;
			case RewriteRelationId:
				copy_rows_of(cs, RewriteRelationId, RewriteOidIndexId,
							 Anum_pg_rewrite_oid, obj.oid, fix_rewrite);
				break;
			case ConstraintRelationId:
				copy_rows_of(cs, ConstraintRelationId, ConstraintOidIndexId,
							 Anum_pg_constraint_oid, obj.oid, fix_constraint);
				break;
			case AttrDefaultRelationId:
				copy_rows_of(cs, AttrDefaultRelationId, AttrDefaultOidIndexId,
							 Anum_pg_attrdef_oid, obj.oid, fix_attrdef);
				break;
			case CastRelationId:
				copy_rows_of(cs, CastRelationId, CastOidIndexId,
							 Anum_pg_cast_oid, obj.oid, fix_cast);
				break;
			case DefaultAclRelationId:
				copy_rows_of(cs, DefaultAclRelationId, DefaultAclOidIndexId,
							 Anum_pg_default_acl_oid, obj.oid, fix_default_acl);
				break;
			case TriggerRelationId:
				{
					HeapTuple	tup = fetch_row(TriggerRelationId, TriggerOidIndexId,
												Anum_pg_trigger_oid, obj.oid);
					Oid			tgrelid = ((Form_pg_trigger) GETSTRUCT(tup))->tgrelid;

					/*
					 * (a shared referenced table: locked as ADD FOREIGN KEY
					 * locks it, so that no change of it misses the trigger)
					 */
					if (!in_copy(cs, RelationRelationId, tgrelid))
						LockRelationOid(tgrelid, ShareRowExclusiveLock);
					copy_rows_of(cs, TriggerRelationId, TriggerOidIndexId,
								 Anum_pg_trigger_oid, obj.oid, fix_trigger);

					/*
					 * a copied foreign key's trigger on a shared referenced
					 * table, which is not copied: its cached triggers change
					 */
					if (!in_copy(cs, RelationRelationId, tgrelid))
						CacheInvalidateRelcacheByRelid(tgrelid);
					heap_freetuple(tup);
					break;
				}
		}

		/* what every object has: its dependencies, owner and comments */
		ScanKeyInit(&key[0], Anum_pg_depend_classid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(obj.classid));
		ScanKeyInit(&key[1], Anum_pg_depend_objid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(obj.oid));
		copy_rows(cs, DependRelationId, DependDependerIndexId, 2, key,
				  fix_depend);
		ScanKeyInit(&key[0], Anum_pg_shdepend_dbid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(MyDatabaseId));
		ScanKeyInit(&key[1], Anum_pg_shdepend_classid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(obj.classid));
		ScanKeyInit(&key[2], Anum_pg_shdepend_objid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(obj.oid));
		copy_rows(cs, SharedDependRelationId, SharedDependDependerIndexId, 3,
				  key, fix_shdepend);
		ScanKeyInit(&key[0], Anum_pg_description_objoid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(obj.oid));
		ScanKeyInit(&key[1], Anum_pg_description_classoid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(obj.classid));
		copy_rows(cs, DescriptionRelationId, DescriptionObjIndexId, 2, key,
				  fix_description);
	}
}

/* ------------------------------------------------------------------------
 * The check of rows made
 * ------------------------------------------------------------------------
 */

static void
check_oid(CopyState *cs, Oid catid, Oid oid, const char *column)
{
	if (OidIsValid(oid) &&
		hash_search(cs->sources, &oid, HASH_FIND, NULL) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a copy of a row of %s names object %u of the source branch in column %s",
						get_rel_name(catid), oid, column),
				 errdetail("The copy doesn't rewrite that reference yet.")));
}

/* whether a field's name (not NUL-terminated) has part in it */
static bool
field_has(const char *field, int len, const char *part)
{
	int			plen = strlen(part);

	for (int i = 0; i + plen <= len; i++)
	{
		if (strncmp(field + i, part, plen) == 0)
			return true;
	}
	return false;
}

typedef struct CheckConsts
{
	CopyState  *cs;
	Oid			catid;
	const char *column;
} CheckConsts;

/* a constant of an object identifier type: its value is an object id */
static bool
check_consts_walker(Node *node, CheckConsts *cc)
{
	if (node == NULL)
		return false;
	if (IsA(node, Const))
	{
		Const	   *c = (Const *) node;

		if (!c->constisnull && oid_valued_type(c->consttype))
			check_oid(cc->cs, cc->catid, DatumGetObjectId(c->constvalue),
					  cc->column);
		else if (!c->constisnull && oid_valued_type(get_element_type(c->consttype)))
		{
			Datum	   *elems;
			int			n;

			oid_array_elems(c->constvalue, &elems, &n);
			for (int i = 0; i < n; i++)
				check_oid(cc->cs, cc->catid, DatumGetObjectId(elems[i]), cc->column);
		}
		return false;
	}
	return expression_tree_walker(node, check_consts_walker, cc);
}

/*
 * Every number in an expression tree's text that could be an object id (a
 * node's fields print as numbers), and every constant whose value is one (a
 * constant's value prints as its bytes).
 */
static void
check_tree(CopyState *cs, Oid catid, const char *text, const char *column)
{
	CheckConsts cc = {cs, catid, column};
	const char *field = "";
	int			fieldlen = 0;

	(void) check_consts_walker(stringToNode(text), &cc);

	for (const char *p = text; *p;)
	{
		if (*p == ':' && isalpha((unsigned char) p[1]))
		{
			/* a field's name: its numbers follow it */
			field = ++p;
			while (isalnum((unsigned char) *p) || *p == '_')
				p++;
			fieldlen = p - field;
		}
		else if (isdigit((unsigned char) *p) && (p == text || !isalnum((unsigned char) p[-1])))
		{
			char	   *end;
			unsigned long v = strtoul(p, &end, 10);
			bool		not_oid;

			/* (a position in the statement's text, a typmod, a length) */
			not_oid = (fieldlen == 8 && strncmp(field, "location", 8) == 0) ||
				field_has(field, fieldlen, "typmod") ||
				(fieldlen >= 3 && strncmp(field + fieldlen - 3, "len", 3) == 0);
			if (!not_oid && v <= PG_UINT32_MAX)
				check_oid(cs, catid, (Oid) v, column);
			p = end;
		}
		else
			p++;
	}
}

/*
 * No row made names an object of the source branch: every column of an
 * object identifier type, and every number in an expression tree.
 */
static void
check_made(CopyState *cs)
{
	foreach_ptr(MadeRow, made, cs->made)
	{
		Relation	rel = table_open(made->catid, AccessShareLock);
		TupleDesc	desc = RelationGetDescr(rel);
		Datum	   *values = palloc_array(Datum, desc->natts);
		bool	   *nulls = palloc_array(bool, desc->natts);

		heap_deform_tuple(made->tup, desc, values, nulls);
		for (int i = 0; i < desc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(desc, i);
			const char *column = NameStr(att->attname);

			if (nulls[i])
				continue;
			switch (att->atttypid)
			{
				case OIDOID:
				case REGPROCOID:
					check_oid(cs, made->catid, DatumGetObjectId(values[i]), column);
					break;
				case OIDVECTOROID:
					{
						oidvector  *v = (oidvector *) DatumGetPointer(values[i]);

						for (int k = 0; k < v->dim1; k++)
							check_oid(cs, made->catid, v->values[k], column);
						break;
					}
				case OIDARRAYOID:
					{
						Datum	   *elems;
						int			n;

						deconstruct_array_builtin(DatumGetArrayTypeP(values[i]),
												  OIDOID, &elems, NULL, &n);
						for (int k = 0; k < n; k++)
							check_oid(cs, made->catid, DatumGetObjectId(elems[k]), column);
						break;
					}
				case PG_NODE_TREEOID:
					check_tree(cs, made->catid, TextDatumGetCString(values[i]), column);
					break;
				case ANYARRAYOID:
					/* (statistics' values: an array's header names its type) */
					check_oid(cs, made->catid,
							  ARR_ELEMTYPE(DatumGetArrayTypeP(values[i])), column);
					break;
			}
		}
		table_close(rel, AccessShareLock);
	}
}

/* ------------------------------------------------------------------------
 * Storage
 * ------------------------------------------------------------------------
 */

static void
make_storage(CopyState *cs)
{
	List	   *tables = NIL;

	for (int i = 0; i < cs->nobjs; i++)
	{
		CopyKey		obj = cs->objs[i];
		Oid			newoid;
		Relation	src;
		Relation	dst;
		SMgrRelation dstsmgr;

		if (obj.classid != RelationRelationId ||
			!RELKIND_HAS_STORAGE(get_rel_relkind(obj.oid)))
			continue;
		newoid = copy_map(cs, RelationRelationId, obj.oid);
		src = relation_open(obj.oid, AccessShareLock);
		dst = relation_open(newoid, AccessExclusiveLock);
		dstsmgr = RelationCreateStorageWith(dst->rd_locator,
											dst->rd_rel->relpersistence, true,
											RelationGetSmgr(src)->smgr_which);
		if (src->rd_rel->relkind == RELKIND_SEQUENCE)
		{
			/* (its page as the buffers have it, not as the file does) */
			FlushRelationBuffers(src);
			RelationCopyStorage(RelationGetSmgr(src), dstsmgr, MAIN_FORKNUM,
								src->rd_rel->relpersistence);
		}
		if (src->rd_rel->relkind == RELKIND_RELATION ||
			src->rd_rel->relkind == RELKIND_MATVIEW)
			tables = lappend_oid(tables, newoid);
		relation_close(dst, NoLock);
		relation_close(src, NoLock);
	}
	CommandCounterIncrement();

	/* (indexes valid on their empty tables) */
	foreach_oid(relid, tables)
	{
		ReindexParams params = {0};

		(void) reindex_relation(NULL, relid, REINDEX_REL_PROCESS_TOAST, &params);
	}
	CommandCounterIncrement();

	/*
	 * A rebuild counted the empty tables' pages and rows: the copies keep
	 * their sources' counts, as their statistics, so that their storage,
	 * shared with the source's, is planned for as the source's is.
	 */
	{
		Relation	pg_class = table_open(RelationRelationId, RowExclusiveLock);

		for (int i = 0; i < cs->nobjs; i++)
		{
			CopyKey		obj = cs->objs[i];
			HeapTuple	src;
			HeapTuple	dst;
			Form_pg_class s;
			Form_pg_class d;

			if (obj.classid != RelationRelationId ||
				!RELKIND_HAS_STORAGE(get_rel_relkind(obj.oid)))
				continue;
			src = fetch_row(RelationRelationId, ClassOidIndexId, Anum_pg_class_oid, obj.oid);
			dst = fetch_row(RelationRelationId, ClassOidIndexId, Anum_pg_class_oid,
							copy_map(cs, RelationRelationId, obj.oid));
			s = (Form_pg_class) GETSTRUCT(src);
			d = (Form_pg_class) GETSTRUCT(dst);
			d->relpages = s->relpages;
			d->reltuples = s->reltuples;
			d->relallvisible = s->relallvisible;
			d->relallfrozen = s->relallfrozen;
			CatalogTupleUpdate(pg_class, &dst->t_self, dst);
			heap_freetuple(src);
			heap_freetuple(dst);
		}
		table_close(pg_class, RowExclusiveLock);
	}
}

/* ------------------------------------------------------------------------
 * The copy
 * ------------------------------------------------------------------------
 */

static void
dependency_copy(int32 src, int32 dst)
{
	CopyState	cs;
	HASHCTL		ctl;

	if (src <= 0 || dst <= 0 || src == dst)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot copy branch %d to branch %d", src, dst)));
	if (!BranchIds->allocated(dst))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("branch %d has not been allocated", dst)));

	/* nothing made on either meanwhile */
	LockBranchCatalogs(src, AccessExclusiveLock);
	LockBranchCatalogs(dst, AccessExclusiveLock);

	memset(&cs, 0, sizeof(cs));
	cs.src = src;
	cs.dst = dst;
	ctl.keysize = sizeof(CopyKey);
	ctl.entrysize = sizeof(CopyEntry);
	ctl.hcxt = CurrentMemoryContext;
	cs.map = hash_create("branch copy map", 1024, &ctl,
						 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(SourceEntry);
	cs.sources = hash_create("branch copy sources", 1024, &ctl,
							 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	cs.cap = 64;
	cs.objs = palloc_array(CopyKey, cs.cap);

	/* the new branch has no schema yet */
	{
		Relation	rel = table_open(NamespaceRelationId, AccessShareLock);
		SysScanDesc scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
		HeapTuple	tup;

		while (HeapTupleIsValid(tup = systable_getnext(scan)))
		{
			if (((Form_pg_namespace) GETSTRUCT(tup))->nspbranch == dst)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("branch %d has schemas already", dst)));
		}
		systable_endscan(scan);
		table_close(rel, AccessShareLock);
	}

	collect(&cs);
	check_stored_types(&cs);
	assign_oids(&cs);
	copy_all_rows(&cs);
	check_made(&cs);
	CommandCounterIncrement();
	make_storage(&cs);
}

const BranchCatalogCopyRoutine DependencyBranchCatalogCopy = {
	.copy = dependency_copy,
};

const BranchCatalogCopyRoutine *BranchCatalogCopy = &DependencyBranchCatalogCopy;

/* SQL: pg_branch_catalog_copy(src int4, dst int4) returns void */
Datum
pg_branch_catalog_copy(PG_FUNCTION_ARGS)
{
	BranchCatalogCopy->copy(PG_GETARG_INT32(0), PG_GETARG_INT32(1));
	PG_RETURN_VOID();
}
