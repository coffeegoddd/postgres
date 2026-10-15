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

#include "access/htup_details.h"
#include "access/transam.h"
#include "catalog/branch.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_conversion.h"
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
#include "executor/tuptable.h"
#include "utils/rel.h"

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
};

const CatalogBranchInfo *
catalog_branch_info(Oid relid)
{
	/* every branch-aware catalog is a system catalog */
	if (relid >= FirstNormalObjectId)
		return NULL;
	for (int i = 0; i < lengthof(branch_catalogs); i++)
	{
		if (branch_catalogs[i].relid == relid)
			return &branch_catalogs[i];
	}
	return NULL;
}

Oid
catalog_row_branch(const CatalogBranchInfo *info, HeapTuple tup)
{
	Oid			branch;

	memcpy(&branch, (char *) GETSTRUCT(tup) + info->offset, sizeof(Oid));
	return branch;
}

/* the branch a row of this catalog belongs to */
static Oid
row_branch(Relation rel, HeapTuple tup)
{
	return InvalidOid;
}

void
catalog_row_set_branch(Relation rel, HeapTuple tup)
{
	const CatalogBranchInfo *info = catalog_branch_info(RelationGetRelid(rel));
	Oid			branch;

	if (info == NULL)
		return;
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
		HeapTuple	newtup;

		newtup = heap_modify_tuple_by_cols(tup, RelationGetDescr(rel), 1,
										   &attnum, &value, &isnull);
		tup->t_len = newtup->t_len;
		tup->t_data = newtup->t_data;
		return;
	}
	memcpy((char *) GETSTRUCT(tup) + info->offset, &branch, sizeof(Oid));
}

void
catalog_slot_set_branch(Relation rel, TupleTableSlot *slot)
{
	const CatalogBranchInfo *info = catalog_branch_info(RelationGetRelid(rel));

	if (info == NULL)
		return;
	slot_getallattrs(slot);
	slot->tts_values[info->attnum - 1] = ObjectIdGetDatum(InvalidOid);
	slot->tts_isnull[info->attnum - 1] = false;
}
