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
 * Copied so far: schemas, tables (their columns, row types and their
 * arrays, TOAST tables), indexes, sequences, constraints (and the triggers
 * of foreign keys), column defaults, and their dependencies, owners,
 * comments and statistics.  A branch with any other kind of object is
 * refused, naming it: no copy is ever half made.
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
} CopyState;

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
					 c->relkind != RELKIND_TOASTVALUE) ||
					c->relispartition || c->relhassubclass ||
					c->relpersistence != RELPERSISTENCE_PERMANENT ||
					OidIsValid(c->reloftype))
					refuse(classid, oid);
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
				/* a table's row type, or its array (checked as the row type is) */
				ok = (t->typtype == TYPTYPE_COMPOSITE && OidIsValid(t->typrelid) &&
					  get_rel_relkind(t->typrelid) != RELKIND_COMPOSITE_TYPE) ||
					(t->typcategory == TYPCATEGORY_ARRAY && OidIsValid(t->typelem) &&
					 OidIsValid(get_typ_typrelid(t->typelem)) &&
					 get_rel_relkind(get_typ_typrelid(t->typelem)) != RELKIND_COMPOSITE_TYPE);
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
					  con->contype == CONSTRAINT_NOTNULL) &&
					!OidIsValid(con->contypid) && !OidIsValid(con->conparentid);
				heap_freetuple(tup);
				if (!ok)
					refuse(classid, oid);
				return;
			}
		case AttrDefaultRelationId:
			return;
		case TriggerRelationId:
			{
				Form_pg_trigger trg;
				bool		ok;

				tup = fetch_row(TriggerRelationId, TriggerOidIndexId,
								Anum_pg_trigger_oid, oid);
				trg = (Form_pg_trigger) GETSTRUCT(tup);
				/* (a foreign key's) */
				ok = trg->tgisinternal && OidIsValid(trg->tgconstraint) &&
					!OidIsValid(trg->tgparentid);
				heap_freetuple(tup);
				if (!ok)
					refuse(classid, oid);
				return;
			}
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

		if (obj.classid == RelationRelationId)
			LockRelationOid(obj.oid, AccessShareLock);
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

/* ------------------------------------------------------------------------
 * New object ids
 * ------------------------------------------------------------------------
 */

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

/* a constant of an object identifier type names an object: its copy's */
static void
map_const(CopyState *cs, Const *c)
{
	Oid			classid;

	c->consttype = map_type(cs, c->consttype);
	if (c->constisnull)
		return;
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

/*
 * Rewrites the object ids of an expression tree's nodes, of the kinds a
 * default, a check constraint or an index expression holds; any other kind
 * is refused, as a reference it might hold would not be rewritten.
 */
static Node *
remap_mutator(Node *node, CopyState *cs)
{
	if (node == NULL)
		return NULL;
	node = expression_tree_mutator(node, remap_mutator, cs);
	switch (nodeTag(node))
	{
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
		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot copy an expression with a node of type %d to another branch yet",
							(int) nodeTag(node))));
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
	/* the relation's storage is new: its file number is its object id */
	values[Anum_pg_class_relfilenode - 1] = values[Anum_pg_class_oid - 1];
	values[Anum_pg_class_relrewrite - 1] = ObjectIdGetDatum(InvalidOid);
	if (relkind == RELKIND_RELATION || relkind == RELKIND_TOASTVALUE)
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
	MAP_TREE(Anum_pg_type_typdefaultbin);
}

static void
fix_index(CopyState *cs, Datum *values, bool *nulls)
{
	MAP(Anum_pg_index_indexrelid, RelationRelationId);
	MAP(Anum_pg_index_indrelid, RelationRelationId);
	MAP_TREE(Anum_pg_index_indexprs);
	MAP_TREE(Anum_pg_index_indpred);
}

static void
fix_constraint(CopyState *cs, Datum *values, bool *nulls)
{
	MAP(Anum_pg_constraint_oid, ConstraintRelationId);
	MAP(Anum_pg_constraint_connamespace, NamespaceRelationId);
	MAP(Anum_pg_constraint_conrelid, RelationRelationId);
	MAP(Anum_pg_constraint_conindid, RelationRelationId);
	MAP(Anum_pg_constraint_confrelid, RelationRelationId);
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
	MAP_TREE(Anum_pg_trigger_tgqual);
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
				if (get_rel_relkind(obj.oid) == RELKIND_INDEX)
					copy_rows_of(cs, IndexRelationId, IndexRelidIndexId,
								 Anum_pg_index_indexrelid, obj.oid, fix_index);
				if (get_rel_relkind(obj.oid) == RELKIND_SEQUENCE)
					copy_rows_of(cs, SequenceRelationId, SequenceRelidIndexId,
								 Anum_pg_sequence_seqrelid, obj.oid, fix_sequence);
				copy_rows_of(cs, StatisticRelationId, StatisticRelidAttnumInhIndexId,
							 Anum_pg_statistic_starelid, obj.oid, fix_statistic);
				break;
			case TypeRelationId:
				copy_rows_of(cs, TypeRelationId, TypeOidIndexId,
							 Anum_pg_type_oid, obj.oid, fix_type);
				break;
			case ConstraintRelationId:
				copy_rows_of(cs, ConstraintRelationId, ConstraintOidIndexId,
							 Anum_pg_constraint_oid, obj.oid, fix_constraint);
				break;
			case AttrDefaultRelationId:
				copy_rows_of(cs, AttrDefaultRelationId, AttrDefaultOidIndexId,
							 Anum_pg_attrdef_oid, obj.oid, fix_attrdef);
				break;
			case TriggerRelationId:
				{
					HeapTuple	tup = fetch_row(TriggerRelationId, TriggerOidIndexId,
												Anum_pg_trigger_oid, obj.oid);
					Oid			tgrelid = ((Form_pg_trigger) GETSTRUCT(tup))->tgrelid;

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
		elog(ERROR, "a copy of a row of %s names object %u of the source branch in column %s",
			 get_rel_name(catid), oid, column);
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

		if (!c->constisnull)
		{
			switch (c->consttype)
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
					check_oid(cc->cs, cc->catid, DatumGetObjectId(c->constvalue),
							  cc->column);
					break;
			}
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

	(void) check_consts_walker(stringToNode(text), &cc);
	for (const char *p = text; *p;)
	{
		if (isdigit((unsigned char) *p) && (p == text || !isalnum((unsigned char) p[-1])))
		{
			char	   *end;
			unsigned long v = strtoul(p, &end, 10);

			if (v <= PG_UINT32_MAX)
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

		if (obj.classid != RelationRelationId)
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
		if (src->rd_rel->relkind == RELKIND_RELATION)
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
