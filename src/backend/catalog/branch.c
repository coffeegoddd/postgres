/*-------------------------------------------------------------------------
 *
 * branch.c
 *	  Branch-aware catalogs: the catalogs whose rows can belong to a
 *	  versioned object, and their branch columns.
 *
 * A versioned object exists once per branch, with the same OID on every
 * branch, so each catalog that can hold its rows has a branch column: the
 * branch a row belongs to, or 0 for a shared row.  The column ends the
 * catalog's unique indexes, so a versioned object's rows can repeat once
 * per branch; it is the last column of the catalog's fixed part, so its
 * offset is known without deforming the row.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/catalog/branch.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/transam.h"
#include "catalog/branch.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_branch.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_default_acl.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_description.h"
#include "catalog/pg_enum.h"
#include "catalog/pg_index.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_partitioned_table.h"
#include "catalog/pg_policy.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_seclabel.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_statistic_ext_data.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_ts_config_map.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_ts_parser.h"
#include "catalog/pg_ts_template.h"
#include "catalog/pg_type.h"
#include "catalog/versioned.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "storage/proc.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

Oid			MyBranch = BRANCH_MAIN_OID;
Oid			CatalogBranchIndexTable = InvalidOid;
bool		CatalogKeepRowBranch = false;

static const CatalogBranchInfo branch_catalogs[] = {
	{NamespaceRelationId, offsetof(FormData_pg_namespace, nspbranch), Anum_pg_namespace_nspbranch},
	{RelationRelationId, offsetof(FormData_pg_class, relbranch), Anum_pg_class_relbranch},
	{AttributeRelationId, offsetof(FormData_pg_attribute, attbranch), Anum_pg_attribute_attbranch},
	{TypeRelationId, offsetof(FormData_pg_type, typbranch), Anum_pg_type_typbranch},
	{ProcedureRelationId, offsetof(FormData_pg_proc, probranch), Anum_pg_proc_probranch},
	{IndexRelationId, offsetof(FormData_pg_index, indbranch), Anum_pg_index_indbranch},
	{ConstraintRelationId, offsetof(FormData_pg_constraint, conbranch), Anum_pg_constraint_conbranch},
	{AttrDefaultRelationId, offsetof(FormData_pg_attrdef, adbranch), Anum_pg_attrdef_adbranch},
	{DependRelationId, offsetof(FormData_pg_depend, depbranch), Anum_pg_depend_depbranch},
	{TriggerRelationId, offsetof(FormData_pg_trigger, tgbranch), Anum_pg_trigger_tgbranch},
	{RewriteRelationId, offsetof(FormData_pg_rewrite, ev_branch), Anum_pg_rewrite_ev_branch},
	{PolicyRelationId, offsetof(FormData_pg_policy, polbranch), Anum_pg_policy_polbranch},
	{EnumRelationId, offsetof(FormData_pg_enum, enumbranch), Anum_pg_enum_enumbranch},
	{SequenceRelationId, offsetof(FormData_pg_sequence, seqbranch), Anum_pg_sequence_seqbranch},
	{DescriptionRelationId, offsetof(FormData_pg_description, objbranch), Anum_pg_description_objbranch},
	{InheritsRelationId, offsetof(FormData_pg_inherits, inhbranch), Anum_pg_inherits_inhbranch},
	{StatisticRelationId, offsetof(FormData_pg_statistic, stabranch), Anum_pg_statistic_stabranch},
	{StatisticExtRelationId, offsetof(FormData_pg_statistic_ext, stxbranch), Anum_pg_statistic_ext_stxbranch},
	{StatisticExtDataRelationId, offsetof(FormData_pg_statistic_ext_data, stxdbranch), Anum_pg_statistic_ext_data_stxdbranch},
	{PartitionedRelationId, offsetof(FormData_pg_partitioned_table, partbranch), Anum_pg_partitioned_table_partbranch},
	{AggregateRelationId, offsetof(FormData_pg_aggregate, aggbranch), Anum_pg_aggregate_aggbranch},
	{OperatorRelationId, offsetof(FormData_pg_operator, oprbranch), Anum_pg_operator_oprbranch},
	{OperatorClassRelationId, offsetof(FormData_pg_opclass, opcbranch), Anum_pg_opclass_opcbranch},
	{OperatorFamilyRelationId, offsetof(FormData_pg_opfamily, opfbranch), Anum_pg_opfamily_opfbranch},
	{AccessMethodOperatorRelationId, offsetof(FormData_pg_amop, amopbranch), Anum_pg_amop_amopbranch},
	{AccessMethodProcedureRelationId, offsetof(FormData_pg_amproc, amprocbranch), Anum_pg_amproc_amprocbranch},
	{CollationRelationId, offsetof(FormData_pg_collation, collbranch), Anum_pg_collation_collbranch},
	{ConversionRelationId, offsetof(FormData_pg_conversion, conbranch), Anum_pg_conversion_conbranch},
	{TSConfigRelationId, offsetof(FormData_pg_ts_config, cfgbranch), Anum_pg_ts_config_cfgbranch},
	{TSConfigMapRelationId, offsetof(FormData_pg_ts_config_map, mapbranch), Anum_pg_ts_config_map_mapbranch},
	{TSDictionaryRelationId, offsetof(FormData_pg_ts_dict, dictbranch), Anum_pg_ts_dict_dictbranch},
	{TSParserRelationId, offsetof(FormData_pg_ts_parser, prsbranch), Anum_pg_ts_parser_prsbranch},
	{TSTemplateRelationId, offsetof(FormData_pg_ts_template, tmplbranch), Anum_pg_ts_template_tmplbranch},
	{SecLabelRelationId, offsetof(FormData_pg_seclabel, objbranch), Anum_pg_seclabel_objbranch},
	{DefaultAclRelationId, offsetof(FormData_pg_default_acl, defaclbranch), Anum_pg_default_acl_defaclbranch},
};

/* every branch-aware catalog's OID is below this; a table indexes them */
#define BRANCH_CATALOG_OID_LIMIT	FirstGenbkiObjectId

static const CatalogBranchInfo *by_oid[BRANCH_CATALOG_OID_LIMIT];
static bool by_oid_built = false;

const CatalogBranchInfo *
catalog_branch_catalogs(int *n)
{
	*n = lengthof(branch_catalogs);
	return branch_catalogs;
}

const CatalogBranchInfo *
catalog_branch_info(Oid relid)
{
	if (relid >= BRANCH_CATALOG_OID_LIMIT)
		return NULL;
	if (unlikely(!by_oid_built))
	{
		for (int i = 0; i < lengthof(branch_catalogs); i++)
		{
			Assert(branch_catalogs[i].relid < BRANCH_CATALOG_OID_LIMIT);
			by_oid[branch_catalogs[i].relid] = &branch_catalogs[i];
		}
		by_oid_built = true;
	}
	return by_oid[relid];
}

bool
catalog_tuple_on_branch(HeapTuple tup)
{
	const CatalogBranchInfo *info = catalog_branch_info(tup->t_tableOid);
	Oid			branch;

	if (info == NULL)
		return true;
	branch = catalog_row_branch(info, tup);
	return branch == InvalidOid || branch == MyBranch;
}

Oid
catalog_row_branch(const CatalogBranchInfo *info, HeapTuple tup)
{
	Oid			branch;

	memcpy(&branch, (char *) GETSTRUCT(tup) + info->offset, sizeof(Oid));
	return branch;
}

/*
 * The branch of object objid's row in catalog classid: the session's
 * branch if the object is versioned, 0 if it is shared or its catalog has
 * no branch.  Looked up with SnapshotSelf, since the row may have been
 * made earlier in the same command.
 */
Oid
catalog_object_branch(Oid classid, Oid objid)
{
	const CatalogBranchInfo *info = catalog_branch_info(classid);
	Relation	rel;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;
	Oid			branch = InvalidOid;
	AttrNumber	oidattno;
	Oid			oidindex;

	if (info == NULL || !OidIsValid(objid))
		return InvalidOid;
	if (classid == AttrDefaultRelationId)
	{
		oidattno = Anum_pg_attrdef_oid;
		oidindex = AttrDefaultOidIndexId;
	}
	else if (is_objectclass_supported(classid))
	{
		oidattno = get_object_attnum_oid(classid);
		oidindex = get_object_oid_index(classid);
	}
	else
		return InvalidOid;

	rel = table_open(classid, AccessShareLock);
	ScanKeyInit(&key, oidattno, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objid));
	scan = systable_beginscan(rel, oidindex, true, SnapshotSelf, 1, &key);
	tup = systable_getnext(scan);
	if (HeapTupleIsValid(tup))
		branch = catalog_row_branch(info, tup);
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	return branch;
}

/* the table an existing index is on, from its pg_index row */
static Oid
index_table(Oid indexid)
{
	Relation	rel = table_open(IndexRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;
	Oid			relid = InvalidOid;

	ScanKeyInit(&key, Anum_pg_index_indexrelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(indexid));
	scan = systable_beginscan(rel, IndexRelidIndexId, true, SnapshotSelf,
							  1, &key);
	tup = systable_getnext(scan);
	if (HeapTupleIsValid(tup))
		relid = ((Form_pg_index) GETSTRUCT(tup))->indrelid;
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	return relid;
}

static Oid
namespace_branch(Oid nspid)
{
	return namespace_is_versioned(nspid) ? MyBranch : InvalidOid;
}

/*
 * The branch a new or changed row of this catalog belongs to: the session's
 * branch if it is a versioned object's row, else 0.  A row is versioned if
 * its object's schema is, or if the object it belongs to is (whose row
 * already carries its branch).
 */
static Oid
row_branch(Relation rel, HeapTuple tup)
{
	void	   *form = GETSTRUCT(tup);

	/* bootstrap makes only shared rows, some before pg_namespace exists */
	if (IsBootstrapProcessingMode())
		return InvalidOid;

	/* a versioned schema's own row may be the database's first */
	if (RelationGetRelid(rel) == NamespaceRelationId)
		return ((Form_pg_namespace) form)->nspversioned ? MyBranch : InvalidOid;

	if (!database_has_versioned_schemas())
		return InvalidOid;

	switch (RelationGetRelid(rel))
	{
		case RelationRelationId:
			{
				Form_pg_class c = (Form_pg_class) form;

				if (namespace_is_versioned(c->relnamespace) ||
					c->relam == PROLLY_TABLE_AM_OID)
					return MyBranch;
				/*
				 * An index belongs where its table does (a TOAST table's,
				 * too): the table index_create is making it for, or, once
				 * made, the one its pg_index row names.
				 */
				if (c->relkind == RELKIND_INDEX ||
					c->relkind == RELKIND_PARTITIONED_INDEX)
					return catalog_object_branch(RelationRelationId,
												 OidIsValid(CatalogBranchIndexTable) ?
												 CatalogBranchIndexTable :
												 index_table(c->oid));
				return InvalidOid;
			}
		case TypeRelationId:
			return namespace_branch(((Form_pg_type) form)->typnamespace);
		case ProcedureRelationId:
			return namespace_branch(((Form_pg_proc) form)->pronamespace);
		case OperatorRelationId:
			return namespace_branch(((Form_pg_operator) form)->oprnamespace);
		case OperatorClassRelationId:
			return namespace_branch(((Form_pg_opclass) form)->opcnamespace);
		case OperatorFamilyRelationId:
			return namespace_branch(((Form_pg_opfamily) form)->opfnamespace);
		case CollationRelationId:
			return namespace_branch(((Form_pg_collation) form)->collnamespace);
		case ConversionRelationId:
			return namespace_branch(((Form_pg_conversion) form)->connamespace);
		case TSConfigRelationId:
			return namespace_branch(((Form_pg_ts_config) form)->cfgnamespace);
		case TSDictionaryRelationId:
			return namespace_branch(((Form_pg_ts_dict) form)->dictnamespace);
		case TSParserRelationId:
			return namespace_branch(((Form_pg_ts_parser) form)->prsnamespace);
		case TSTemplateRelationId:
			return namespace_branch(((Form_pg_ts_template) form)->tmplnamespace);
		case StatisticExtRelationId:
			return namespace_branch(((Form_pg_statistic_ext) form)->stxnamespace);
		case ConstraintRelationId:
			return namespace_branch(((Form_pg_constraint) form)->connamespace);
		case DefaultAclRelationId:
			/* a schema's default privileges; global ones (no schema) are shared */
			return namespace_branch(((Form_pg_default_acl) form)->defaclnamespace);
		case AttributeRelationId:
			return catalog_object_branch(RelationRelationId,
										 ((Form_pg_attribute) form)->attrelid);
		case IndexRelationId:
			return catalog_object_branch(RelationRelationId,
										 ((Form_pg_index) form)->indexrelid);
		case AttrDefaultRelationId:
			return catalog_object_branch(RelationRelationId,
										 ((Form_pg_attrdef) form)->adrelid);
		case TriggerRelationId:
			return catalog_object_branch(RelationRelationId,
										 ((Form_pg_trigger) form)->tgrelid);
		case RewriteRelationId:
			return catalog_object_branch(RelationRelationId,
										 ((Form_pg_rewrite) form)->ev_class);
		case PolicyRelationId:
			return catalog_object_branch(RelationRelationId,
										 ((Form_pg_policy) form)->polrelid);
		case SequenceRelationId:
			return catalog_object_branch(RelationRelationId,
										 ((Form_pg_sequence) form)->seqrelid);
		case InheritsRelationId:
			return catalog_object_branch(RelationRelationId,
										 ((Form_pg_inherits) form)->inhrelid);
		case StatisticRelationId:
			return catalog_object_branch(RelationRelationId,
										 ((Form_pg_statistic) form)->starelid);
		case PartitionedRelationId:
			return catalog_object_branch(RelationRelationId,
										 ((Form_pg_partitioned_table) form)->partrelid);
		case EnumRelationId:
			return catalog_object_branch(TypeRelationId,
										 ((Form_pg_enum) form)->enumtypid);
		case StatisticExtDataRelationId:
			return catalog_object_branch(StatisticExtRelationId,
										 ((Form_pg_statistic_ext_data) form)->stxoid);
		case AggregateRelationId:
			return catalog_object_branch(ProcedureRelationId,
										 ((Form_pg_aggregate) form)->aggfnoid);
		case AccessMethodOperatorRelationId:
			return catalog_object_branch(OperatorFamilyRelationId,
										 ((Form_pg_amop) form)->amopfamily);
		case AccessMethodProcedureRelationId:
			return catalog_object_branch(OperatorFamilyRelationId,
										 ((Form_pg_amproc) form)->amprocfamily);
		case TSConfigMapRelationId:
			return catalog_object_branch(TSConfigRelationId,
										 ((Form_pg_ts_config_map) form)->mapcfg);
		case DependRelationId:
			return catalog_object_branch(((Form_pg_depend) form)->classid,
										 ((Form_pg_depend) form)->objid);
		case DescriptionRelationId:
			return catalog_object_branch(((Form_pg_description) form)->classoid,
										 ((Form_pg_description) form)->objoid);
		case SecLabelRelationId:
			return catalog_object_branch(((FormData_pg_seclabel *) form)->classoid,
										 ((FormData_pg_seclabel *) form)->objoid);
	}
	return InvalidOid;
}

HeapTuple
catalog_row_with_branch(Relation rel, HeapTuple tup)
{
	const CatalogBranchInfo *info = catalog_branch_info(RelationGetRelid(rel));
	Oid			branch;

	if (info == NULL)
		return tup;

	/* a branch's copy of a row carries its branch already */
	if (CatalogKeepRowBranch &&
		!heap_attisnull(tup, info->attnum, RelationGetDescr(rel)))
		return tup;

	branch = row_branch(rel, tup);

	/*
	 * A row formed with the column null (callers that start from all-null
	 * values) is formed again; otherwise the column is at its fixed offset.
	 */
	if (heap_attisnull(tup, info->attnum, RelationGetDescr(rel)))
	{
		Datum		value = ObjectIdGetDatum(branch);
		bool		isnull = false;
		int			attnum = info->attnum;

		return heap_modify_tuple_by_cols(tup, RelationGetDescr(rel), 1,
										 &attnum, &value, &isnull);
	}
	memcpy((char *) GETSTRUCT(tup) + info->offset, &branch, sizeof(Oid));
	return tup;
}

void
catalog_slot_set_branch(Relation rel, TupleTableSlot *slot)
{
	const CatalogBranchInfo *info = catalog_branch_info(RelationGetRelid(rel));
	HeapTuple	tup;
	Oid			branch;

	if (info == NULL)
		return;
	slot_getallattrs(slot);
	/* the rule reads the row's other columns, so it gets a formed row */
	slot->tts_isnull[info->attnum - 1] = false;
	slot->tts_values[info->attnum - 1] = ObjectIdGetDatum(InvalidOid);
	tup = heap_form_tuple(slot->tts_tupleDescriptor, slot->tts_values,
						  slot->tts_isnull);
	branch = row_branch(rel, tup);
	heap_freetuple(tup);
	slot->tts_values[info->attnum - 1] = ObjectIdGetDatum(branch);
}

/*
 * Check out the branch a connection named ("dbname/branch"), if the
 * database has one by that name; false if it has none.
 */
bool
branch_session_init(const char *branch)
{
	HeapTuple	tup;
	Oid			oid;

	tup = SearchSysCache1(BRANCHNAME, CStringGetDatum(branch));
	if (!HeapTupleIsValid(tup))
		return false;
	oid = ((Form_pg_branch) GETSTRUCT(tup))->oid;
	ReleaseSysCache(tup);
	set_session_branch(oid);
	return true;
}

/*
 * Make branch the session's: from now on a versioned object's rows on it
 * are the ones seen.  Caches keyed by OID may hold another branch's rows,
 * so they are all dropped.
 */
void
set_session_branch(Oid branch)
{
	if (MyProc != NULL)
		MyProc->branchId = branch;
	if (branch == MyBranch)
		return;
	MyBranch = branch;
	InvalidateSystemCaches();
}
