/*-------------------------------------------------------------------------
 *
 * branch_filter.c
 *	  The catalog filter, and the map of catalog objects to branches it
 *	  asks.
 *
 * A query of a catalog must not see another branch's rows.  Which rows are
 * a branch's is decided row by row, from what the row is or describes:
 *
 * - a pg_namespace row says its branch in nspbranch;
 * - a row of a catalog with a namespace column (pg_class, pg_type,
 *	 pg_proc, pg_constraint, operators, collations, text search objects,
 *	 extended statistics, extensions) is its namespace's branch's;
 * - a row describing an object (pg_attribute, pg_index, pg_attrdef,
 *	 pg_trigger, pg_rewrite, pg_policy, pg_inherits, pg_sequence,
 *	 pg_statistic, pg_enum, pg_range, pg_aggregate, pg_amop, ...) is that
 *	 object's branch's, and a pg_depend, pg_description, pg_init_privs or
 *	 pg_seclabel row is hidden when an object it names is;
 * - the rows of shared catalogs that name this database's objects
 *	 (pg_shdepend) are those objects' branches', and so are a publication's
 *	 or a subscription's rows naming a branch's table or schema.
 *
 * An object's branch comes from the branch map (BranchMapRoutine).  The
 * map in use answers from a per-backend cache, and on a miss reads the
 * object's row by its object id: its namespace's nspbranch, or for an
 * object that belongs to another (a column default, a trigger, a TOAST
 * table), its owner's branch.  An object's branch cannot change while it
 * exists, so what the cache learned stays right; it forgets an entry when
 * a syscache invalidation names the object's row, which covers an object
 * dropped (and its object id later reused), and it forgets everything on
 * a cache reset.  So the cost of the filter is what queries read, the
 * first time each object is asked about, plus what changed since: never a
 * rebuild of everything another branch holds.  A branch change costs
 * nothing here, since the map does not depend on the session's branch.
 *
 * The map reads with a catalog snapshot, which sees every branch's rows,
 * and if that does not find the object (one dropped since the query's
 * snapshot was taken), with SnapshotAny: an object's row is not removed
 * while any snapshot can see a row describing it.
 *
 * The filter runs after the table access method found a tuple visible and
 * released the buffer's content lock: the map may read the catalogs, which
 * must not be done under a buffer lock.  The catalogs it reads are read
 * with snapshots the filter leaves alone, so it never filters its own
 * reads.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/prolly/branch_filter.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/transam.h"
#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_default_acl.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_description.h"
#include "catalog/pg_enum.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_index.h"
#include "catalog/pg_init_privs.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_partitioned_table.h"
#include "catalog/pg_policy.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_publication_namespace.h"
#include "catalog/pg_publication_rel.h"
#include "catalog/pg_range.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_seclabel.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_shdepend.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_statistic_ext_data.h"
#include "catalog/pg_subscription_rel.h"
#include "catalog/pg_transform.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_ts_config_map.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_ts_parser.h"
#include "catalog/pg_ts_template.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "prolly/branch_catalog.h"
#include "prolly/branch_filter.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

bool		catalog_branch_filter = true;

/*
 * The objects a catalog row is or describes, whose branches decide whether
 * it is hidden: at most two.
 */
typedef struct RowOwners
{
	int			n;
	Oid			classid[2];
	Oid			objid[2];
} RowOwners;

static inline void
add_owner(RowOwners *owners, Oid classid, Oid objid)
{
	if (OidIsValid(objid))
	{
		owners->classid[owners->n] = classid;
		owners->objid[owners->n] = objid;
		owners->n++;
	}
}

/*
 * The owners of a row of catalog relid, or false if the catalog's rows
 * never belong to a branch (filtered_catalog() lists the same catalogs).
 * A pg_namespace row is its own owner.
 */
static bool
row_owners(Oid relid, HeapTuple tup, RowOwners *owners)
{
	void	   *row = GETSTRUCT(tup);

	owners->n = 0;
	switch (relid)
	{
			/* rows of objects in a namespace */
		case NamespaceRelationId:
			add_owner(owners, NamespaceRelationId, ((Form_pg_namespace) row)->oid);
			break;
		case RelationRelationId:
			{
				Form_pg_class c = (Form_pg_class) row;

				/* a TOAST table or index is its table's */
				if (c->relnamespace == PG_TOAST_NAMESPACE)
					add_owner(owners, RelationRelationId, c->oid);
				else
					add_owner(owners, NamespaceRelationId, c->relnamespace);
				break;
			}
		case TypeRelationId:
			add_owner(owners, NamespaceRelationId, ((Form_pg_type) row)->typnamespace);
			break;
		case ProcedureRelationId:
			add_owner(owners, NamespaceRelationId, ((Form_pg_proc) row)->pronamespace);
			break;
		case ConstraintRelationId:
			add_owner(owners, NamespaceRelationId, ((Form_pg_constraint) row)->connamespace);
			break;
		case OperatorRelationId:
			add_owner(owners, NamespaceRelationId, ((Form_pg_operator) row)->oprnamespace);
			break;
		case OperatorClassRelationId:
			add_owner(owners, NamespaceRelationId, ((Form_pg_opclass) row)->opcnamespace);
			break;
		case OperatorFamilyRelationId:
			add_owner(owners, NamespaceRelationId, ((Form_pg_opfamily) row)->opfnamespace);
			break;
		case CollationRelationId:
			add_owner(owners, NamespaceRelationId, ((Form_pg_collation) row)->collnamespace);
			break;
		case ConversionRelationId:
			add_owner(owners, NamespaceRelationId, ((Form_pg_conversion) row)->connamespace);
			break;
		case StatisticExtRelationId:
			add_owner(owners, NamespaceRelationId, ((Form_pg_statistic_ext) row)->stxnamespace);
			break;
		case TSConfigRelationId:
			add_owner(owners, NamespaceRelationId, ((Form_pg_ts_config) row)->cfgnamespace);
			break;
		case TSDictionaryRelationId:
			add_owner(owners, NamespaceRelationId, ((Form_pg_ts_dict) row)->dictnamespace);
			break;
		case TSParserRelationId:
			add_owner(owners, NamespaceRelationId, ((Form_pg_ts_parser) row)->prsnamespace);
			break;
		case TSTemplateRelationId:
			add_owner(owners, NamespaceRelationId, ((Form_pg_ts_template) row)->tmplnamespace);
			break;
		case ExtensionRelationId:
			add_owner(owners, NamespaceRelationId, ((Form_pg_extension) row)->extnamespace);
			break;
		case DefaultAclRelationId:
			add_owner(owners, NamespaceRelationId, ((Form_pg_default_acl) row)->defaclnamespace);
			break;

			/* rows describing a relation */
		case AttributeRelationId:
			add_owner(owners, RelationRelationId, ((Form_pg_attribute) row)->attrelid);
			break;
		case IndexRelationId:
			add_owner(owners, RelationRelationId, ((Form_pg_index) row)->indexrelid);
			break;
		case AttrDefaultRelationId:
			add_owner(owners, RelationRelationId, ((Form_pg_attrdef) row)->adrelid);
			break;
		case TriggerRelationId:
			{
				Form_pg_trigger t = (Form_pg_trigger) row;

				/*
				 * a foreign key's triggers on a shared table belong to the
				 * key's branch
				 */
				add_owner(owners, RelationRelationId, t->tgrelid);
				add_owner(owners, ConstraintRelationId, t->tgconstraint);
				break;
			}
		case RewriteRelationId:
			add_owner(owners, RelationRelationId, ((Form_pg_rewrite) row)->ev_class);
			break;
		case PolicyRelationId:
			add_owner(owners, RelationRelationId, ((Form_pg_policy) row)->polrelid);
			break;
		case InheritsRelationId:
			add_owner(owners, RelationRelationId, ((Form_pg_inherits) row)->inhrelid);
			break;
		case SequenceRelationId:
			add_owner(owners, RelationRelationId, ((Form_pg_sequence) row)->seqrelid);
			break;
		case StatisticRelationId:
			add_owner(owners, RelationRelationId, ((Form_pg_statistic) row)->starelid);
			break;
		case PartitionedRelationId:
			add_owner(owners, RelationRelationId, ((Form_pg_partitioned_table) row)->partrelid);
			break;
		case ForeignTableRelationId:
			add_owner(owners, RelationRelationId, ((Form_pg_foreign_table) row)->ftrelid);
			break;

			/* rows describing other objects */
		case EnumRelationId:
			add_owner(owners, TypeRelationId, ((Form_pg_enum) row)->enumtypid);
			break;
		case RangeRelationId:
			add_owner(owners, TypeRelationId, ((Form_pg_range) row)->rngtypid);
			break;
		case AggregateRelationId:
			add_owner(owners, ProcedureRelationId, ((Form_pg_aggregate) row)->aggfnoid);
			break;
		case AccessMethodOperatorRelationId:
			add_owner(owners, OperatorFamilyRelationId, ((Form_pg_amop) row)->amopfamily);
			break;
		case AccessMethodProcedureRelationId:
			add_owner(owners, OperatorFamilyRelationId, ((Form_pg_amproc) row)->amprocfamily);
			break;
		case CastRelationId:
			add_owner(owners, TypeRelationId, ((Form_pg_cast) row)->castsource);
			add_owner(owners, TypeRelationId, ((Form_pg_cast) row)->casttarget);
			break;
		case TransformRelationId:
			add_owner(owners, TypeRelationId, ((Form_pg_transform) row)->trftype);
			break;
		case TSConfigMapRelationId:
			add_owner(owners, TSConfigRelationId, ((Form_pg_ts_config_map) row)->mapcfg);
			break;
		case StatisticExtDataRelationId:
			add_owner(owners, StatisticExtRelationId, ((Form_pg_statistic_ext_data) row)->stxoid);
			break;

			/* rows naming any objects */
		case DependRelationId:
			{
				Form_pg_depend d = (Form_pg_depend) row;

				add_owner(owners, d->classid, d->objid);
				add_owner(owners, d->refclassid, d->refobjid);
				break;
			}
		case DescriptionRelationId:
			{
				Form_pg_description d = (Form_pg_description) row;

				add_owner(owners, d->classoid, d->objoid);
				break;
			}
		case InitPrivsRelationId:
			{
				Form_pg_init_privs p = (Form_pg_init_privs) row;

				add_owner(owners, p->classoid, p->objoid);
				break;
			}
		case SecLabelRelationId:
			{
				FormData_pg_seclabel *l = (FormData_pg_seclabel *) row;

				add_owner(owners, l->classoid, l->objoid);
				break;
			}

			/*
			 * A shared catalog's rows of this database's objects; another
			 * database's are left as they are, since this backend cannot
			 * read that database's catalogs to learn their branches.
			 */
		case SharedDependRelationId:
			{
				Form_pg_shdepend d = (Form_pg_shdepend) row;

				if (d->dbid == MyDatabaseId)
					add_owner(owners, d->classid, d->objid);
				break;
			}

			/* publications and subscriptions of a branch's tables */
		case PublicationRelRelationId:
			add_owner(owners, RelationRelationId, ((Form_pg_publication_rel) row)->prrelid);
			break;
		case PublicationNamespaceRelationId:
			add_owner(owners, NamespaceRelationId, ((Form_pg_publication_namespace) row)->pnnspid);
			break;
		case SubscriptionRelRelationId:
			add_owner(owners, RelationRelationId, ((Form_pg_subscription_rel) row)->srrelid);
			break;
		default:
			return false;
	}
	return true;
}

/*
 * Set while a page-at-a-time scan filters a page: the full scans that meet
 * every branch's objects, which alone make the map learn every relation at
 * once (learn_all_relations()).
 */
static bool in_page_scan = false;

/* Whether rows of catalog relid can belong to a branch: row_owners' list. */
static bool
filtered_catalog(Oid relid)
{
	switch (relid)
	{
		case NamespaceRelationId:
		case RelationRelationId:
		case TypeRelationId:
		case ProcedureRelationId:
		case ConstraintRelationId:
		case OperatorRelationId:
		case OperatorClassRelationId:
		case OperatorFamilyRelationId:
		case CollationRelationId:
		case ConversionRelationId:
		case StatisticExtRelationId:
		case TSConfigRelationId:
		case TSDictionaryRelationId:
		case TSParserRelationId:
		case TSTemplateRelationId:
		case ExtensionRelationId:
		case DefaultAclRelationId:
		case AttributeRelationId:
		case IndexRelationId:
		case AttrDefaultRelationId:
		case TriggerRelationId:
		case RewriteRelationId:
		case PolicyRelationId:
		case InheritsRelationId:
		case SequenceRelationId:
		case StatisticRelationId:
		case PartitionedRelationId:
		case ForeignTableRelationId:
		case EnumRelationId:
		case RangeRelationId:
		case AggregateRelationId:
		case AccessMethodOperatorRelationId:
		case AccessMethodProcedureRelationId:
		case CastRelationId:
		case TransformRelationId:
		case TSConfigMapRelationId:
		case StatisticExtDataRelationId:
		case DependRelationId:
		case DescriptionRelationId:
		case SharedDependRelationId:
		case InitPrivsRelationId:
		case SecLabelRelationId:
		case PublicationRelRelationId:
		case PublicationNamespaceRelationId:
		case SubscriptionRelRelationId:
			return true;
		default:
			return false;
	}
}

bool
CatalogBranchSees(Oid classid, Oid objid)
{
	int32		branch;

	if (!catalog_branch_filter)
		return true;
	branch = BranchMap->object_branch(classid, objid);
	return branch == 0 || branch == catalog_branch;
}

List *
CatalogBranchSeenOids(Oid classid, List *oids)
{
	List	   *seen = NIL;

	foreach_oid(objid, oids)
	{
		if (CatalogBranchSees(classid, objid))
			seen = lappend_oid(seen, objid);
	}
	return seen;
}

bool
CatalogBranchFilters(Relation rel, Snapshot snapshot)
{
	Oid			relid = RelationGetRelid(rel);

	/* ordinary tables first: this is asked on every heap fetch */
	if (relid >= FirstNormalObjectId || !catalog_branch_filter)
		return false;
	if (snapshot == NULL || snapshot->snapshot_type != SNAPSHOT_MVCC ||
		snapshot->is_catalog)
		return false;
	if (!IsNormalProcessingMode())
		return false;
	return filtered_catalog(relid);
}

static inline bool
branch_hidden(int32 branch)
{
	return branch != 0 && branch != catalog_branch;
}

bool
CatalogBranchHides(Relation rel, HeapTuple tuple)
{
	RowOwners	owners;
	Oid			relid = RelationGetRelid(rel);

	if (relid == NamespaceRelationId)
		return branch_hidden(((Form_pg_namespace) GETSTRUCT(tuple))->nspbranch);

	if (!row_owners(relid, tuple, &owners))
		return false;
	for (int i = 0; i < owners.n; i++)
	{
		if (branch_hidden(BranchMap->object_branch(owners.classid[i],
												   owners.objid[i])))
			return true;
	}
	return false;
}

uint32
CatalogBranchFilterPage(Relation rel, Page page, BlockNumber block,
						OffsetNumber *offsets, uint32 n)
{
	uint32		kept = 0;
	HeapTupleData tuple;

	tuple.t_tableOid = RelationGetRelid(rel);
	in_page_scan = true;
	PG_TRY();
	{
		for (uint32 i = 0; i < n; i++)
		{
			ItemId		lp = PageGetItemId(page, offsets[i]);

			tuple.t_data = (HeapTupleHeader) PageGetItem(page, lp);
			tuple.t_len = ItemIdGetLength(lp);
			ItemPointerSet(&tuple.t_self, block, offsets[i]);
			if (!CatalogBranchHides(rel, &tuple))
				offsets[kept++] = offsets[i];
		}
	}
	PG_FINALLY();
	{
		in_page_scan = false;
	}
	PG_END_TRY();
	return kept;
}

/*
 * The cached branch map.
 *
 * An entry is keyed by the object's class and, for the classes whose rows
 * an invalidation can name, the syscache hash of its object id (the hash
 * an invalidation carries), else the object id itself.  It holds the
 * object's branch, or, for an object whose branch is its owner's (a TOAST
 * table, a column default, a trigger), the owner, so that an owner moving
 * to another schema is seen through the owner's own entry.
 */
typedef struct BranchMapKey
{
	Oid			classid;
	uint32		key;
} BranchMapKey;

typedef struct BranchMapEntry
{
	BranchMapKey k;				/* hash key: must be first */
	Oid			objid;
	Oid			owner_class;	/* InvalidOid: branch is the answer */
	Oid			owner_objid;
	int32		branch;
} BranchMapEntry;

static HTAB *branch_map = NULL;
static BranchMapStats map_stats;
static bool callbacks_registered = false;

/*
 * The last answer, which most questions repeat: a catalog's rows that
 * describe one object (a relation's pg_attribute rows) are mostly next to
 * each other.  Forgotten whenever the map forgets anything.
 */
static struct
{
	bool		valid;
	Oid			classid;
	Oid			objid;
	int32		branch;
}			last_answer;

/*
 * The classes whose rows the map forgets on invalidation, and their
 * syscaches.  Each callback is one of a backend's few syscache callback
 * slots, so only the classes whose objects can move between schemas, or
 * are dropped and made most often, are listed; the others' answers last
 * until a cache reset, which is right as long as their objects keep their
 * branch, as every object does.
 */
static const struct
{
	Oid			classid;
	SysCacheIdentifier cacheid;
}			evicting_classes[] = {
	{NamespaceRelationId, NAMESPACEOID},
	{RelationRelationId, RELOID},
	{TypeRelationId, TYPEOID},
	{ProcedureRelationId, PROCOID},
	{ConstraintRelationId, CONSTROID},
};

static int
evicting_cache(Oid classid)
{
	for (int i = 0; i < lengthof(evicting_classes); i++)
	{
		if (evicting_classes[i].classid == classid)
			return evicting_classes[i].cacheid;
	}
	return -1;
}

/*
 * Relations a page-at-a-time scan had the map learn one by one since it
 * last learned them all (learn_all_relations()), and whether it has.  A
 * full scan of a catalog that names relations (pg_attribute) learns every
 * branch's relation anyway: learning them at once costs the memory the
 * scan's would, an entry of some 50 bytes for each relation (its table,
 * TOAST table and their indexes) of every branch, in each backend that
 * scans so.
 */
static uint32 relation_misses = 0;
static bool relations_learned = false;

/* misses after which the map learns every relation at once */
#define BRANCH_MAP_BULK_MISSES	64

/*
 * Counts the map's changes, and the answers it could not keep (an object
 * not found), so that the last answer is kept only if the map neither
 * changed nor declined while it was found.
 */
static uint64 map_generation = 0;

static void
map_reset(void)
{
	map_generation++;
	last_answer.valid = false;
	relation_misses = 0;
	relations_learned = false;
	if (branch_map != NULL)
	{
		map_stats.forgotten += hash_get_num_entries(branch_map);
		hash_destroy(branch_map);
		branch_map = NULL;
	}
}

static void
map_invalidate(Datum arg, SysCacheIdentifier cacheid, uint32 hashvalue)
{
	BranchMapKey key;

	/* a branch change: no object changed branch */
	if (branch_map == NULL || catalog_branch_changing)
		return;
	map_generation++;
	if (hashvalue == 0)
	{
		map_reset();
		return;
	}
	key.classid = DatumGetObjectId(arg);
	key.key = hashvalue;
	if (hash_search(branch_map, &key, HASH_REMOVE, NULL) != NULL)
	{
		map_stats.forgotten++;
		last_answer.valid = false;
	}
}

static HTAB *
map_table(void)
{
	if (!callbacks_registered)
	{
		for (int i = 0; i < lengthof(evicting_classes); i++)
			CacheRegisterSyscacheCallback(evicting_classes[i].cacheid,
										  map_invalidate,
										  ObjectIdGetDatum(evicting_classes[i].classid));
		callbacks_registered = true;
	}
	if (branch_map == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(BranchMapKey);
		ctl.entrysize = sizeof(BranchMapEntry);
		ctl.hcxt = TopMemoryContext;
		branch_map = hash_create("branch map", 256, &ctl,
								 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	return branch_map;
}

static BranchMapKey
map_key(Oid classid, Oid objid)
{
	BranchMapKey key;
	int			cacheid = evicting_cache(classid);

	memset(&key, 0, sizeof(key));
	key.classid = classid;
	key.key = cacheid >= 0 ?
		GetSysCacheHashValue1(cacheid, ObjectIdGetDatum(objid)) : objid;
	return key;
}

/*
 * Reads the row of catalog catid whose object id (column oidatt, indexed by
 * oidindex) is objid, and returns column att of it; false if there is no
 * such row.
 */
static bool
read_column(Oid catid, Oid oidindex, AttrNumber oidatt, Oid objid,
			AttrNumber att, Datum *value)
{
	Relation	rel;
	ScanKeyData key;
	bool		found = false;

	map_stats.reads++;
	rel = table_open(catid, AccessShareLock);
	ScanKeyInit(&key, oidatt, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objid));
	for (int pass = 0; pass < 2 && !found; pass++)
	{
		SysScanDesc scan;
		HeapTuple	tup;
		bool		isnull;

		/* first as of now, then any version still there */
		scan = systable_beginscan(rel, oidindex, true,
								  pass == 0 ? NULL : SnapshotAny, 1, &key);
		tup = systable_getnext(scan);
		if (HeapTupleIsValid(tup))
		{
			*value = heap_getattr(tup, att, RelationGetDescr(rel), &isnull);
			found = !isnull;
		}
		systable_endscan(scan);
	}
	table_close(rel, AccessShareLock);
	return found;
}

/* The relation a TOAST table or index belongs to, by its dependency. */
static Oid
toast_owner(Oid relid)
{
	Relation	rel;
	ScanKeyData key[2];
	Oid			owner = InvalidOid;

	map_stats.reads++;
	rel = table_open(DependRelationId, AccessShareLock);
	ScanKeyInit(&key[0], Anum_pg_depend_classid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1], Anum_pg_depend_objid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(relid));
	for (int pass = 0; pass < 2 && !OidIsValid(owner); pass++)
	{
		SysScanDesc scan;
		HeapTuple	tup;

		scan = systable_beginscan(rel, DependDependerIndexId, true,
								  pass == 0 ? NULL : SnapshotAny, 2, key);
		while (HeapTupleIsValid(tup = systable_getnext(scan)))
		{
			Form_pg_depend d = (Form_pg_depend) GETSTRUCT(tup);

			if (d->refclassid == RelationRelationId &&
				(d->deptype == DEPENDENCY_INTERNAL || d->deptype == DEPENDENCY_AUTO))
			{
				owner = d->refobjid;
				break;
			}
		}
		systable_endscan(scan);
	}
	table_close(rel, AccessShareLock);
	return owner;
}

/*
 * The objects whose branch is another object's: the owner's class, and
 * the column naming the owner.
 */
static const struct
{
	Oid			classid;
	Oid			oidindex;
	AttrNumber	oidatt;
	Oid			owner_class;
	AttrNumber	owner_att;
}			owned_classes[] = {
	{AttrDefaultRelationId, AttrDefaultOidIndexId, Anum_pg_attrdef_oid,
	RelationRelationId, Anum_pg_attrdef_adrelid},
	{TriggerRelationId, TriggerOidIndexId, Anum_pg_trigger_oid,
	RelationRelationId, Anum_pg_trigger_tgrelid},
	{RewriteRelationId, RewriteOidIndexId, Anum_pg_rewrite_oid,
	RelationRelationId, Anum_pg_rewrite_ev_class},
	{PolicyRelationId, PolicyOidIndexId, Anum_pg_policy_oid,
	RelationRelationId, Anum_pg_policy_polrelid},
	{AccessMethodOperatorRelationId, AccessMethodOperatorOidIndexId, Anum_pg_amop_oid,
	OperatorFamilyRelationId, Anum_pg_amop_amopfamily},
	{AccessMethodProcedureRelationId, AccessMethodProcedureOidIndexId, Anum_pg_amproc_oid,
	OperatorFamilyRelationId, Anum_pg_amproc_amprocfamily},
	{TransformRelationId, TransformOidIndexId, Anum_pg_transform_oid,
	TypeRelationId, Anum_pg_transform_trftype},
	{ExtensionRelationId, ExtensionOidIndexId, Anum_pg_extension_oid,
	NamespaceRelationId, Anum_pg_extension_extnamespace},
	{PublicationRelRelationId, PublicationRelObjectIndexId, Anum_pg_publication_rel_oid,
	RelationRelationId, Anum_pg_publication_rel_prrelid},
	{PublicationNamespaceRelationId, PublicationNamespaceObjectIndexId,
		Anum_pg_publication_namespace_oid,
	NamespaceRelationId, Anum_pg_publication_namespace_pnnspid},
	{DefaultAclRelationId, DefaultAclOidIndexId, Anum_pg_default_acl_oid,
	NamespaceRelationId, Anum_pg_default_acl_defaclnamespace},
};

static int32 cached_object_branch_depth(Oid classid, Oid objid, int depth);

/*
 * Learns an object's branch, or its owner, from the catalogs.  Returns
 * false if the object was not found (nothing is kept for it then).
 */
static bool
learn(Oid classid, Oid objid, BranchMapEntry *e)
{
	Datum		value;

	e->owner_class = InvalidOid;
	e->owner_objid = InvalidOid;
	e->branch = 0;

	if (classid == NamespaceRelationId)
	{
		if (!read_column(NamespaceRelationId, NamespaceOidIndexId,
						 Anum_pg_namespace_oid, objid,
						 Anum_pg_namespace_nspbranch, &value))
			return false;
		e->branch = DatumGetInt32(value);
		return true;
	}

	/*
	 * A cast is a branch's if either of its types is (casts from a shared
	 * type to a branch's are how extensions such as citext reach their
	 * types): its source's, else its target's.  Their rows are hidden as
	 * either type's (row_owners()); the cast is the one branch's that may
	 * have it.
	 */
	if (classid == CastRelationId)
	{
		Datum		target;

		if (!read_column(CastRelationId, CastOidIndexId, Anum_pg_cast_oid,
						 objid, Anum_pg_cast_castsource, &value) ||
			!read_column(CastRelationId, CastOidIndexId, Anum_pg_cast_oid,
						 objid, Anum_pg_cast_casttarget, &target))
			return false;
		e->owner_class = TypeRelationId;
		e->owner_objid = DatumGetObjectId(value);
		if (cached_object_branch_depth(TypeRelationId, e->owner_objid, 1) == 0)
			e->owner_objid = DatumGetObjectId(target);
		return true;
	}

	/*
	 * A foreign key's triggers on the referenced table belong to the key,
	 * whose table may be on a branch when the referenced one is shared
	 */
	if (classid == TriggerRelationId)
	{
		if (!read_column(TriggerRelationId, TriggerOidIndexId,
						 Anum_pg_trigger_oid, objid,
						 Anum_pg_trigger_tgconstraint, &value))
			return false;
		if (OidIsValid(DatumGetObjectId(value)))
		{
			e->owner_class = ConstraintRelationId;
			e->owner_objid = DatumGetObjectId(value);
			return true;
		}
	}

	for (int i = 0; i < lengthof(owned_classes); i++)
	{
		if (owned_classes[i].classid != classid)
			continue;
		if (!read_column(classid, owned_classes[i].oidindex,
						 owned_classes[i].oidatt, objid,
						 owned_classes[i].owner_att, &value))
			return false;
		e->owner_class = owned_classes[i].owner_class;
		e->owner_objid = DatumGetObjectId(value);
		return true;
	}

	if (is_objectclass_supported(classid) &&
		get_object_attnum_namespace(classid) != InvalidAttrNumber)
	{
		Oid			nspid;

		if (!read_column(classid, get_object_oid_index(classid),
						 get_object_attnum_oid(classid), objid,
						 get_object_attnum_namespace(classid), &value))
			return false;
		nspid = DatumGetObjectId(value);
		if (classid == RelationRelationId && nspid == PG_TOAST_NAMESPACE)
		{
			/*
			 * (not yet known while the TOAST table's own dependency is
			 * being recorded: nothing is kept then)
			 */
			e->owner_class = RelationRelationId;
			e->owner_objid = toast_owner(objid);
			if (!OidIsValid(e->owner_objid))
				return false;
		}
		else
		{
			e->owner_class = NamespaceRelationId;
			e->owner_objid = nspid;
		}
		return true;
	}

	/* an object of a class whose objects never belong to a branch */
	return true;
}

/* Keeps that object objid of classid is owner_objid's of owner_class. */
static void
map_enter(Oid classid, Oid objid, Oid owner_class, Oid owner_objid)
{
	BranchMapKey key = map_key(classid, objid);
	BranchMapEntry *e;
	bool		found;

	e = hash_search(map_table(), &key, HASH_ENTER, &found);
	e->objid = objid;
	e->owner_class = owner_class;
	e->owner_objid = owner_objid;
	e->branch = 0;
}

/*
 * Learns every relation's owner from one scan of pg_class and one of
 * pg_index: a relation's namespace, a TOAST table's table (its pg_class row
 * names it, as toast_owner()'s dependency does), and a TOAST table's index's
 * table (its pg_index row).  A catalog snapshot sees every
 * branch's rows.  Relations made after are learned one by one, and those
 * changed forgotten, as any are.
 */
static void
learn_all_relations(void)
{
	Relation	rel;
	SysScanDesc scan;
	HeapTuple	tup;

	map_stats.reads++;
	rel = table_open(RelationRelationId, AccessShareLock);
	/* (after the open: invalidations it, and pg_index's, process may reset the map) */
	relations_learned = true;
	scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_class c = (Form_pg_class) GETSTRUCT(tup);

		if (c->oid < FirstNormalObjectId)
			continue;
		if (c->relnamespace != PG_TOAST_NAMESPACE)
			map_enter(RelationRelationId, c->oid,
					  NamespaceRelationId, c->relnamespace);
		if (OidIsValid(c->reltoastrelid))
			map_enter(RelationRelationId, c->reltoastrelid,
					  RelationRelationId, c->oid);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	/*
	 * A TOAST table's index is in pg_toast too, and only pg_index names its
	 * table: every index the scan above left out is its table's.
	 */
	rel = table_open(IndexRelationId, AccessShareLock);
	scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_index i = (Form_pg_index) GETSTRUCT(tup);
		BranchMapKey key;
		BranchMapEntry *e;

		if (i->indexrelid < FirstNormalObjectId)
			continue;
		key = map_key(RelationRelationId, i->indexrelid);
		e = hash_search(map_table(), &key, HASH_FIND, NULL);
		if (e == NULL || e->objid != i->indexrelid)
			map_enter(RelationRelationId, i->indexrelid,
					  RelationRelationId, i->indrelid);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
}

static int32
cached_object_branch_depth(Oid classid, Oid objid, int depth)
{
	BranchMapKey key;
	BranchMapEntry *e;
	BranchMapEntry learned;
	bool		found;

	map_stats.probes++;
	if (!OidIsValid(objid))
		return 0;
	if (depth > 4)
	{
		map_generation++;		/* (not kept: see map_generation) */
		return 0;
	}

	/*
	 * Objects initdb made are shared, and need no reading (nor could the
	 * TOAST tables of the catalogs be found an owner: pinned objects have no
	 * dependencies recorded)
	 */
	if (objid < FirstNormalObjectId)
		return 0;

	key = map_key(classid, objid);
	e = hash_search(map_table(), &key, HASH_FIND, NULL);

	/*
	 * A scan meeting many relations the map doesn't know (a full scan of
	 * pg_attribute, which names every branch's) would read pg_class once
	 * for each: past a few, the map reads it once for all of them.
	 */
	if ((e == NULL || e->objid != objid) && classid == RelationRelationId &&
		in_page_scan && !relations_learned &&
		++relation_misses >= BRANCH_MAP_BULK_MISSES)
	{
		learn_all_relations();
		e = hash_search(map_table(), &key, HASH_FIND, NULL);
	}
	if (e == NULL || e->objid != objid)
	{
		/*
		 * Reading the catalogs can take locks, and so process invalidations
		 * that change the table: nothing found in it is kept across this.
		 */
		if (!learn(classid, objid, &learned))
		{
			map_generation++;	/* (not kept: see map_generation) */
			return 0;
		}
		e = hash_search(map_table(), &key, HASH_ENTER, &found);
		e->objid = objid;
		e->owner_class = learned.owner_class;
		e->owner_objid = learned.owner_objid;
		e->branch = learned.branch;
	}

	if (!OidIsValid(e->owner_class))
		return e->branch;
	return cached_object_branch_depth(e->owner_class, e->owner_objid, depth + 1);
}

/*
 * The namespace of an object, or of the object it belongs to, read without
 * the map (only the dependency check asks, and rarely); InvalidOid if it
 * has none.
 */
static Oid
object_namespace(Oid classid, Oid objid, int depth)
{
	Datum		value;

	if (depth > 4)
		return InvalidOid;
	for (int i = 0; i < lengthof(owned_classes); i++)
	{
		if (owned_classes[i].classid != classid)
			continue;
		if (!read_column(classid, owned_classes[i].oidindex,
						 owned_classes[i].oidatt, objid,
						 owned_classes[i].owner_att, &value))
			return InvalidOid;
		if (owned_classes[i].owner_class == NamespaceRelationId)
			return DatumGetObjectId(value);
		return object_namespace(owned_classes[i].owner_class,
								DatumGetObjectId(value), depth + 1);
	}
	if (is_objectclass_supported(classid) &&
		get_object_attnum_namespace(classid) != InvalidAttrNumber &&
		read_column(classid, get_object_oid_index(classid),
					get_object_attnum_oid(classid), objid,
					get_object_attnum_namespace(classid), &value))
		return DatumGetObjectId(value);
	return InvalidOid;
}

void
CatalogBranchCheckDependencies(const ObjectAddress *depender,
							   const ObjectAddress *referenced,
							   int nreferenced, DependencyType deptype)
{
	int32		depender_branch = -1;

	for (int i = 0; i < nreferenced; i++)
	{
		int32		branch;

		branch = BranchMap->object_branch(referenced[i].classId,
										  referenced[i].objectId);
		if (branch == 0)
		{
			/*
			 * A branch's object may depend on a shared one, but not belong
			 * to it: dropping an object that belongs to another drops that
			 * one, and so dropping the branch's would drop the shared one,
			 * and everything of every branch that depends on it.
			 */
			if (deptype == DEPENDENCY_INTERNAL ||
				deptype == DEPENDENCY_EXTENSION)
			{
				/*
				 * (by their schemas, which finds objects made earlier in
				 * this command too: the map takes those for shared)
				 */
				Oid			depnsp = object_namespace(depender->classId,
													  depender->objectId, 0);
				Oid			refnsp;

				/* (shared DDL, the common case, needs no second read) */
				if (!OidIsValid(depnsp) || NamespaceBranch(depnsp) == 0)
					continue;
				refnsp = object_namespace(referenced[i].classId,
										  referenced[i].objectId, 0);
				if (OidIsValid(refnsp) && NamespaceBranch(refnsp) == 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("an object in a versioned schema cannot belong to %s",
									getObjectDescription(&referenced[i], false)),
							 errdetail("A shared object's parts are shared too.")));
			}
			continue;
		}
		if (depender_branch < 0)
			depender_branch = BranchMap->object_branch(depender->classId,
													   depender->objectId);
		if (depender_branch == branch)
			continue;
		if (depender_branch == 0)
		{
			Oid			nspid = object_namespace(depender->classId,
												 depender->objectId, 0);

			/*
			 * a temporary object is the session's alone, and a TOAST table
			 * (its dependency on its table being what is recorded now) is
			 * its table's
			 */
			if (isAnyTempNamespace(nspid) || nspid == PG_TOAST_NAMESPACE)
				continue;
		}
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 depender_branch == 0 ?
				 errmsg("an object outside a versioned schema cannot depend on %s",
						getObjectDescription(&referenced[i], false)) :
				 errmsg("an object cannot depend on another branch's %s",
						getObjectDescription(&referenced[i], false)),
				 errdetail("Objects in versioned schemas may depend on shared objects, not the other way round.")));
	}
}

void
CatalogBranchCheckMove(Oid oldnspid, Oid newnspid)
{
	if (NamespaceBranch(oldnspid) != NamespaceBranch(newnspid))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot move objects between a versioned schema and another kind or branch of schema")));
}

void
CatalogBranchCheckInheritance(Oid childid, Oid parentid)
{
	Oid			childnsp = get_rel_namespace(childid);
	Oid			parentnsp = get_rel_namespace(parentid);

	/*
	 * (a temporary table is the session's alone: its parent's scans in
	 * other sessions pass over it)
	 */
	if (isAnyTempNamespace(childnsp))
		return;
	if (NamespaceBranch(childnsp) != NamespaceBranch(parentnsp))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("\"%s\" cannot inherit from, or be a partition of, \"%s\"",
						get_rel_name(childid), get_rel_name(parentid)),
				 errdetail("A table and its parent must both be shared, or both be one branch's: a parent's scans and changes reach its children.")));
}

static int32
cached_object_branch(Oid classid, Oid objid)
{
	int32		branch;
	uint64		generation;

	if (last_answer.valid && last_answer.objid == objid &&
		last_answer.classid == classid)
	{
		map_stats.probes++;
		return last_answer.branch;
	}
	generation = map_generation;
	branch = cached_object_branch_depth(classid, objid, 0);
	if (map_generation == generation)
	{
		last_answer.classid = classid;
		last_answer.objid = objid;
		last_answer.branch = branch;
		last_answer.valid = true;
	}
	return branch;
}

static void
cached_stats(BranchMapStats *stats)
{
	*stats = map_stats;
	stats->entries = branch_map != NULL ? hash_get_num_entries(branch_map) : 0;
}

const BranchMapRoutine CachedBranchMap = {
	.object_branch = cached_object_branch,
	.reset = map_reset,
	.stats = cached_stats,
};

const BranchMapRoutine *BranchMap = &CachedBranchMap;
