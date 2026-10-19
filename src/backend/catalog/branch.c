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
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/prolly_config.h"
#include "access/table.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "catalog/branch.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_range.h"
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
#include "access/xact.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

Oid			MyBranch = BRANCH_MAIN_OID;

/*
 * Whether the session's branch is one it may only read (a session checked
 * out at a commit): its transactions start read-only.  Kept with MyBranch,
 * and put back with it when a transaction that left a branch aborts.
 */
bool		MyBranchReadOnly = false;
Oid			CatalogBranchIndexTable = InvalidOid;
bool		CatalogKeepRowBranch = false;
bool		BranchCreateOnBehalf = false;

/*
 * The branches the transaction has written, each with the subtransaction
 * that first wrote it: a write the transaction's own statements made, not
 * one made on the session's behalf (hushed).  A subtransaction's abort
 * forgets what it first wrote, so a later write is noted again.
 */
typedef struct WrittenBranch
{
	Oid			branch;
	SubTransactionId subid;
} WrittenBranch;

static List *written_branches = NIL;	/* of WrittenBranch */
static Oid	last_written = InvalidOid;	/* (the last noted, still noted) */
static List *hushes = NIL;		/* subtransaction of each hush in effect */
static bool catalog_written = false;	/* a versioned catalog row, too */
static List *sequences_written = NIL;	/* of Oid: versioned sequences */

static void note_catalog_write(Oid branch);

typedef struct BranchWriteCallbackItem
{
	BranchWriteCallback callback;
	void	   *arg;
} BranchWriteCallbackItem;

static List *write_callbacks = NIL;
bool		CatalogAllBranches = false;

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
	{RangeRelationId, offsetof(FormData_pg_range, rngbranch), Anum_pg_range_rngbranch},
	{CastRelationId, offsetof(FormData_pg_cast, castbranch), Anum_pg_cast_castbranch},
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

	if (info == NULL || CatalogAllBranches)
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
		case RangeRelationId:
			return catalog_object_branch(TypeRelationId,
										 ((Form_pg_range) form)->rngtypid);
		case CastRelationId:
			{
				/* a cast belongs where a versioned type it casts does */
				Form_pg_cast c = (Form_pg_cast) form;
				Oid			branch = catalog_object_branch(TypeRelationId,
														   c->castsource);

				return OidIsValid(branch) ? branch :
					catalog_object_branch(TypeRelationId, c->casttarget);
			}
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
	note_catalog_write(branch);

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
	note_catalog_write(branch);
}

/*
 * (a versioned row of another branch is changed only on the session's
 * behalf, hushed: what's left is the session's branch, so once that is
 * noted the row needn't be read)
 */
void
catalog_note_old_row(Relation rel, const ItemPointerData *tid)
{
	const CatalogBranchInfo *info = catalog_branch_info(RelationGetRelid(rel));
	HeapTupleData tup;
	Buffer		buf;

	if (info == NULL || hushes != NIL || last_written == MyBranch ||
		!database_has_versioned_schemas() || branch_written(MyBranch))
		return;
	tup.t_self = *tid;
	if (heap_fetch(rel, SnapshotAny, &tup, &buf, false))
	{
		note_catalog_write(catalog_row_branch(info, &tup));
		ReleaseBuffer(buf);
	}
}

/*
 * Is relation file number 'relnumber' in tablespace 'spcoid' the storage of
 * a relation on another branch than the session's?  For logical decoding,
 * which decodes the session branch's changes: another branch's relation
 * has no row the session sees.
 */
bool
relfilenumber_on_other_branch(Oid spcoid, RelFileNumber relnumber)
{
	Relation	rel;
	ScanKeyData keys[2];
	SysScanDesc scan;
	HeapTuple	tup;
	bool		other = false;

	if (spcoid == MyDatabaseTableSpace)
		spcoid = InvalidOid;
	ScanKeyInit(&keys[0], Anum_pg_class_reltablespace, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(spcoid));
	ScanKeyInit(&keys[1], Anum_pg_class_relfilenode, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(relnumber));

	rel = table_open(RelationRelationId, AccessShareLock);
	CatalogAllBranches = true;
	PG_TRY();
	{
		scan = systable_beginscan(rel, ClassTblspcRelfilenodeIndexId, true,
								  NULL, 2, keys);
		while ((tup = systable_getnext(scan)) != NULL)
		{
			Oid			branch = ((Form_pg_class) GETSTRUCT(tup))->relbranch;

			if (OidIsValid(branch) && branch != MyBranch)
				other = true;
		}
		systable_endscan(scan);
	}
	PG_FINALLY();
	{
		CatalogAllBranches = false;
	}
	PG_END_TRY();
	table_close(rel, AccessShareLock);
	return other;
}

/*
 * The branch a relation's locks are for: the session's if the relation is
 * versioned (its copy there is locked apart from other branches' copies),
 * else 0.  From its catalog row as the session sees it; 0 if it has none
 * (yet), or for a system catalog, never versioned.
 */
Oid
relation_lock_branch(Oid relid)
{
	HeapTuple	tup;
	Oid			branch;

	if (relid < FirstNormalObjectId || !IsTransactionState() ||
		!database_has_versioned_schemas())
		return InvalidOid;
	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tup))
		return InvalidOid;
	branch = ((Form_pg_class) GETSTRUCT(tup))->relbranch;
	ReleaseSysCache(tup);
	return branch;
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
	return check_out_branch(oid);
}

/*
 * Make branch the session's: from now on a versioned object's rows on it
 * are the ones seen.  Caches keyed by OID may hold another branch's rows,
 * so they are all dropped.
 */
void
set_session_branch(Oid branch)
{
	Oid			from = MyBranch;

	if (branch == MyBranch)
		return;
	MyBranch = branch;
	InvalidateSystemCaches();
	RelationCacheRebuildNew(from);
}

/*
 * Checking out.  A session holds a lock on its branch (AccessShareLock on
 * the branch as an object of the database, session-level) for as long as
 * it is on it, and deleting a branch takes AccessExclusiveLock until it
 * commits: a branch isn't deleted under a session, and a session can't
 * check out a branch being deleted.  main, which can't be deleted, isn't
 * locked.  A branch left keeps its lock until the transaction ends.  The
 * locks are of the user lock method, as advisory locks are: an aborted
 * transaction releases a session's locks of the default one.
 *
 * A prepared transaction outlives its session's stay on a branch, and may
 * have rows there that only it can commit: a branch it was on, or wrote,
 * would leave them on no branch at all if deleted.  So a transaction being
 * prepared locks each such branch itself (AccessShareLock on it as an
 * object, of the default method: PREPARE TRANSACTION passes it on, and a
 * restart keeps it) before the session's own locks on the branches it left
 * go, and deleting a branch takes AccessExclusiveLock there too.  A deletion
 * doesn't wait for one, as it doesn't for a session: a prepared
 * transaction may be finished only days later.
 *
 * A branch made in the transaction disappears if the transaction (or the
 * subtransaction that made it) aborts: a session checked out on it goes
 * back to the branch it came from.
 */

typedef struct NewBranch
{
	Oid			branch;
	SubTransactionId subid;		/* the subtransaction that made it */
	Oid			left;			/* the branch last left for it */
	bool		left_read_only; /* and whether the session only read it */
} NewBranch;

/* the branches this session holds locks on, in TopMemoryContext */
static List *locked_branches = NIL;

/* branches made by the transaction, oldest first, in TopMemoryContext */
static List *new_branches = NIL;

static bool branch_callbacks_registered = false;

static void
branch_locktag(LOCKTAG *tag, Oid branch)
{
	SET_LOCKTAG_OBJECT(*tag, MyDatabaseId, BranchRelationId, branch, 0);
	tag->locktag_lockmethodid = USER_LOCKMETHOD;
}

/*
 * Lock a branch to delete it, until the transaction ends; false if a
 * session is on it (or another transaction is deleting it).
 */
bool
lock_branch_for_delete(Oid branch)
{
	LOCKTAG		tag;

	branch_locktag(&tag, branch);
	return LockAcquire(&tag, AccessExclusiveLock, false, true) !=
		LOCKACQUIRE_NOT_AVAIL;
}

/* (a transaction's lock on a branch, kept by it once prepared) */
static void
branch_xact_locktag(LOCKTAG *tag, Oid branch)
{
	SET_LOCKTAG_OBJECT(*tag, MyDatabaseId, BranchRelationId, branch, 0);
}

/*
 * Lock a branch to delete or move it, until the transaction ends, against
 * the prepared transactions that were on it: an error, naming one, if any
 * is.
 */
void
lock_branch_unprepared(Oid branch, const char *name)
{
	LOCKTAG		tag;
	VirtualTransactionId *holders;
	char		gid[GIDSIZE];

	branch_xact_locktag(&tag, branch);
	if (LockAcquire(&tag, AccessExclusiveLock, false, true) !=
		LOCKACQUIRE_NOT_AVAIL)
		return;

	/* (none named if the one holding it is still being prepared) */
	holders = GetLockConflicts(&tag, AccessExclusiveLock, NULL);
	for (; VirtualTransactionIdIsValid(*holders); holders++)
	{
		if (TwoPhaseGetGidByVirtualXID(*holders, gid))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_IN_USE),
					 errmsg("branch \"%s\" has a prepared transaction on it",
							name),
					 errdetail("Prepared transaction \"%s\" was on it.", gid),
					 errhint("Finish it with COMMIT PREPARED or ROLLBACK PREPARED first.")));
	}
	ereport(ERROR,
			(errcode(ERRCODE_OBJECT_IN_USE),
			 errmsg("branch \"%s\" has a prepared transaction on it", name),
			 errhint("Finish it with COMMIT PREPARED or ROLLBACK PREPARED first.")));
}

/*
 * The transaction being prepared locks the branches it was on or wrote,
 * as a deletion sees (main, never deleted, isn't locked).
 */
static void
lock_prepared_branches(void)
{
	List	   *branches = list_concat_unique_oid(list_copy(locked_branches),
												  branches_written());

	foreach_oid(branch, branches)
	{
		LOCKTAG		tag;

		if (branch == BRANCH_MAIN_OID)
			continue;
		branch_xact_locktag(&tag, branch);
		(void) LockAcquire(&tag, AccessShareLock, false, false);
	}
	list_free(branches);
}

/* go back from branches the (sub)transaction 'subid' made (all: invalid) */
static void
undo_new_branches(SubTransactionId subid)
{
	/* newest first: a branch left for a newer one may be undone too */
	for (int i = list_length(new_branches) - 1; i >= 0; i--)
	{
		NewBranch  *nb = list_nth(new_branches, i);

		if (subid != InvalidSubTransactionId && nb->subid != subid)
			continue;
		if (MyBranch == nb->branch)
		{
			set_session_branch(nb->left);
			MyBranchReadOnly = nb->left_read_only;
		}
		new_branches = list_delete_nth_cell(new_branches, i);
		pfree(nb);
	}
}

/* release the locks of branches the session has left */
/*
 * Branches to return to if a (sub)transaction aborts, each with the
 * subtransaction that asked: an operation that checks out others on its way
 * leaves the session where it was if it fails.
 */
static List *returns = NIL;		/* of NewBranch, branch unused */

static void register_branch_callbacks(void);

int
branch_return_on_abort(void)
{
	MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);
	NewBranch  *nb = palloc(sizeof(NewBranch));
	int			token = list_length(returns);

	register_branch_callbacks();
	nb->branch = InvalidOid;
	nb->subid = GetCurrentSubTransactionId();
	nb->left = MyBranch;
	nb->left_read_only = MyBranchReadOnly;
	returns = lappend(returns, nb);
	MemoryContextSwitchTo(old);
	return token;
}

void
branch_return_done(int token)
{
	while (list_length(returns) > token)
	{
		pfree(llast(returns));
		returns = list_delete_last(returns);
	}
}

/* (oldest first: the earliest return of what aborts is where it began) */
static void
undo_returns(SubTransactionId subid)
{
	ListCell   *lc;
	bool		returned = false;

	foreach(lc, returns)
	{
		NewBranch  *nb = lfirst(lc);

		if (subid != InvalidSubTransactionId && nb->subid != subid)
			continue;
		if (!returned && MyBranch != nb->left)
		{
			set_session_branch(nb->left);
			MyBranchReadOnly = nb->left_read_only;
		}
		returned = true;
		returns = foreach_delete_current(returns, lc);
		pfree(nb);
	}
}

static void
release_left_branches(void)
{
	List	   *keep = NIL;
	ListCell   *lc;
	MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

	foreach(lc, locked_branches)
	{
		Oid			branch = lfirst_oid(lc);
		LOCKTAG		tag;

		if (branch == MyBranch)
		{
			keep = lappend_oid(keep, branch);
			continue;
		}
		branch_locktag(&tag, branch);
		LockRelease(&tag, AccessShareLock, true);
	}
	list_free(locked_branches);
	locked_branches = keep;
	MemoryContextSwitchTo(old);
}

static void forget_written(SubTransactionId subid);

void
RegisterBranchWriteCallback(BranchWriteCallback callback, void *arg)
{
	MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);
	BranchWriteCallbackItem *item = palloc(sizeof(BranchWriteCallbackItem));

	item->callback = callback;
	item->arg = arg;
	write_callbacks = lappend(write_callbacks, item);
	MemoryContextSwitchTo(old);
}

void
branch_note_write(Oid branch)
{
	MemoryContext old;
	WrittenBranch *wb;
	ListCell   *lc;

	if (hushes != NIL || !OidIsValid(branch) || branch == last_written)
		return;
	foreach(lc, written_branches)
	{
		if (((WrittenBranch *) lfirst(lc))->branch == branch)
		{
			last_written = branch;
			return;
		}
	}
	register_branch_callbacks();
	old = MemoryContextSwitchTo(TopMemoryContext);
	wb = palloc(sizeof(WrittenBranch));
	wb->branch = branch;
	wb->subid = GetCurrentSubTransactionId();
	written_branches = lappend(written_branches, wb);
	MemoryContextSwitchTo(old);
	last_written = branch;

	/* (a callback's error is the write's) */
	foreach(lc, write_callbacks)
	{
		BranchWriteCallbackItem *item = lfirst(lc);

		item->callback(branch, item->arg);
	}
}

BranchTupleHook branch_tuple_hook = NULL;

/* A versioned object's catalog row written: the branch's, and noted so. */
static void
note_catalog_write(Oid branch)
{
	if (OidIsValid(branch) && hushes == NIL)
		catalog_written = true;
	branch_note_write(branch);
}

bool
branch_catalog_written(void)
{
	return catalog_written;
}

void
branch_note_sequence(Relation seqrel)
{
	Oid			branch = seqrel->rd_rel->relbranch;

	/* (the last noted first: a statement's values are one sequence's) */
	if (OidIsValid(branch) && hushes == NIL &&
		(sequences_written == NIL ||
		 llast_oid(sequences_written) != RelationGetRelid(seqrel)) &&
		!list_member_oid(sequences_written, RelationGetRelid(seqrel)))
	{
		MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

		sequences_written = lappend_oid(sequences_written,
										RelationGetRelid(seqrel));
		MemoryContextSwitchTo(old);
	}
	branch_note_write(branch);
}

List *
branch_sequences_written(void)
{
	return sequences_written;
}

void
branch_note_tuple(Relation rel, const ItemPointerData *oldtid,
				  const ItemPointerData *newtid)
{
	/*
	 * The change log is told of every write, the session's own or not;
	 * not of a TOAST relation's versions, or a change log's (they are its
	 * table's).  A change log's entries are bookkeeping, no change of the
	 * session's: the hook isn't told of them either.
	 */
	if (rel->rd_rel->relkind != RELKIND_TOASTVALUE)
		prolly_note_change(rel, oldtid, newtid);
	else if (prolly_is_change_log(rel))
		return;
	if (branch_tuple_hook && hushes == NIL)
		branch_tuple_hook(rel, oldtid, newtid);
}

/*
 * As branch_note_tuple, of the versions a write made in hand (an insertion
 * of many rows, or one) and the one it replaced, if any.
 */
void
branch_note_made(Relation rel, const ItemPointerData *oldtid,
				 HeapTuple *made, int nmade)
{
	if (rel->rd_rel->relkind != RELKIND_TOASTVALUE)
		prolly_note_made(rel, oldtid, made, nmade);
	else if (prolly_is_change_log(rel))
		return;
	if (branch_tuple_hook && hushes == NIL)
		for (int i = 0; i < nmade; i++)
			branch_tuple_hook(rel, i == 0 ? oldtid : NULL, &made[i]->t_self);
}

void
branch_forget_written(Oid branch)
{
	ListCell   *lc;

	foreach(lc, written_branches)
	{
		WrittenBranch *wb = lfirst(lc);

		if (wb->branch == branch)
		{
			written_branches = foreach_delete_current(written_branches, lc);
			pfree(wb);
		}
	}
	last_written = InvalidOid;
	if (branch == MyBranch)
	{
		catalog_written = false;
		list_free(sequences_written);
		sequences_written = NIL;
	}
}

bool
branch_written(Oid branch)
{
	ListCell   *lc;

	foreach(lc, written_branches)
	{
		if (((WrittenBranch *) lfirst(lc))->branch == branch)
			return true;
	}
	return false;
}

List *
branches_written(void)
{
	List	   *branches = NIL;
	ListCell   *lc;

	foreach(lc, written_branches)
		branches = lappend_oid(branches, ((WrittenBranch *) lfirst(lc))->branch);
	return branches;
}

int
branch_writes_hush(void)
{
	MemoryContext old;
	int			depth = list_length(hushes);

	register_branch_callbacks();
	old = MemoryContextSwitchTo(TopMemoryContext);
	hushes = lappend_int(hushes, (int) GetCurrentSubTransactionId());
	MemoryContextSwitchTo(old);
	return depth;
}

/* (back to depth: a hush inside, left by an error, goes too) */
void
branch_writes_unhush(int depth)
{
	Assert(list_length(hushes) > depth);
	hushes = list_truncate(hushes, depth);
}

static void
forget_written(SubTransactionId subid)
{
	ListCell   *lc;

	/* (a transaction's end: what it wrote, gone; a subtransaction's: kept, as more) */
	if (subid == InvalidSubTransactionId)
	{
		catalog_written = false;
		list_free(sequences_written);
		sequences_written = NIL;
	}

	foreach(lc, written_branches)
	{
		WrittenBranch *wb = lfirst(lc);

		if (subid == InvalidSubTransactionId || wb->subid == subid)
		{
			written_branches = foreach_delete_current(written_branches, lc);
			pfree(wb);
		}
	}
	foreach(lc, hushes)
	{
		if (subid == InvalidSubTransactionId ||
			(SubTransactionId) lfirst_int(lc) == subid)
			hushes = foreach_delete_current(hushes, lc);
	}
	last_written = InvalidOid;
}

static void
branch_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
			undo_new_branches(InvalidSubTransactionId);
			undo_returns(InvalidSubTransactionId);
			release_left_branches();
			forget_written(InvalidSubTransactionId);
			break;
		case XACT_EVENT_PRE_PREPARE:
			lock_prepared_branches();
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_PREPARE:
			list_free_deep(new_branches);
			new_branches = NIL;
			list_free_deep(returns);
			returns = NIL;
			release_left_branches();
			forget_written(InvalidSubTransactionId);
			break;
		default:
			break;
	}
}

static void
branch_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
						SubTransactionId parentSubid, void *arg)
{
	ListCell   *lc;

	switch (event)
	{
		case SUBXACT_EVENT_ABORT_SUB:
			undo_new_branches(mySubid);
			undo_returns(mySubid);
			forget_written(mySubid);
			break;
		case SUBXACT_EVENT_COMMIT_SUB:
			foreach(lc, new_branches)
			{
				NewBranch  *nb = lfirst(lc);

				if (nb->subid == mySubid)
					nb->subid = parentSubid;
			}
			foreach(lc, written_branches)
			{
				WrittenBranch *wb = lfirst(lc);

				if (wb->subid == mySubid)
					wb->subid = parentSubid;
			}
			foreach(lc, returns)
			{
				NewBranch  *nb = lfirst(lc);

				if (nb->subid == mySubid)
					nb->subid = parentSubid;
			}
			foreach(lc, hushes)
			{
				if ((SubTransactionId) lfirst_int(lc) == mySubid)
					lfirst_int(lc) = (int) parentSubid;
			}
			break;
		default:
			break;
	}
}

static void
register_branch_callbacks(void)
{
	if (branch_callbacks_registered)
		return;
	RegisterXactCallback(branch_xact_callback, NULL);
	RegisterSubXactCallback(branch_subxact_callback, NULL);
	branch_callbacks_registered = true;
}

/*
 * Note a branch the current (sub)transaction has made, so that a session
 * on it goes back if it aborts.
 */
void
note_new_branch(Oid branch)
{
	MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);
	NewBranch  *nb = palloc(sizeof(NewBranch));

	register_branch_callbacks();
	nb->branch = branch;
	nb->subid = GetCurrentSubTransactionId();
	nb->left = MyBranch;
	nb->left_read_only = MyBranchReadOnly;
	new_branches = lappend(new_branches, nb);
	MemoryContextSwitchTo(old);
}

/*
 * Make branch the session's, locked for as long as the session is on it.
 * Waits for a deletion of it to end; false if it no longer exists.
 */
bool
check_out_branch(Oid branch)
{
	ListCell   *lc;

	register_branch_callbacks();

	if (branch != BRANCH_MAIN_OID && !list_member_oid(locked_branches, branch))
	{
		LOCKTAG		tag;
		MemoryContext old;

		branch_locktag(&tag, branch);
		(void) LockAcquire(&tag, AccessShareLock, true, false);
		old = MemoryContextSwitchTo(TopMemoryContext);
		locked_branches = lappend_oid(locked_branches, branch);
		MemoryContextSwitchTo(old);

		/* a deletion that held the lock has committed or not: see which */
		AcceptInvalidationMessages();
	}
	if (!SearchSysCacheExists1(BRANCHOID, ObjectIdGetDatum(branch)))
		return false;

	foreach(lc, new_branches)
	{
		NewBranch  *nb = lfirst(lc);

		if (nb->branch == branch && MyBranch != branch)
		{
			nb->left = MyBranch;
			nb->left_read_only = MyBranchReadOnly;
		}
	}
	if (branch != MyBranch)
		MyBranchReadOnly = false;	/* (its owner says otherwise, if not) */
	set_session_branch(branch);
	return true;
}
