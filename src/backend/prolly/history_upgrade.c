/*-------------------------------------------------------------------------
 *
 * history_upgrade.c
 *	  The history store carried over by pg_upgrade.
 *
 * pg_upgrade transfers each database's history store (the relations of
 * schema pg_history, their TOAST tables and indexes) as it transfers the
 * relations users made: their files, the old cluster's, in place of the
 * new cluster's.  initdb made the new cluster's, matched to the old ones'
 * by OID (pg_upgrade --check refuses relations of the store not the new
 * cluster's: no concurrent reindex gives the store's indexes new OIDs);
 * the schema pg_dump --binary-upgrade restores
 * gives each the old one's file number first
 * (binary_upgrade_set_history_relfilenode), and its relfrozenxid and
 * relminmxid.  A tier's pack objects stay where they are, the new
 * cluster's to read (pg_upgrade refuses one whose tier's place is not the
 * old one's, binary_upgrade_history_tier_place); its marker, named by the
 * system, is the new system's once the files are there
 * (binary_upgrade_history_tier_mark).  A chunk log's lanes (prolly/
 * chunk_lane.h), made as backends wrote, are not initdb's: pg_dump
 * --binary-upgrade makes each again, its OID and file number the old
 * cluster's, and binary_upgrade_history_lane makes it its log's again; its
 * pages, its meta page naming it a lane, come with its files.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/history_upgrade.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "prolly/chunk_index.h"
#include "prolly/chunk_log.h"
#include "prolly/chunk_store.h"
#include "prolly/chunk_tier.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

static void
check_binary_upgrade(void)
{
	if (!IsBinaryUpgrade)
		ereport(ERROR,
				(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
				 errmsg("function can only be called when server is in binary upgrade mode")));
}

/*
 * The relation given the file number, if it has not that one: its pg_class
 * row's, and its storage of initdb's let go of now (as a binary upgrade's
 * RelationSetNewRelfilenumber lets go of it).  None is made for it: the
 * old cluster's files are put there, every one of them, before anything
 * reads it, and an access method's storage made anew would read what it
 * had.
 */
static void
set_relfilenode(Oid relid, char relkind, RelFileNumber relfilenumber)
{
	Relation	rel = relation_open(relid, AccessExclusiveLock);

	if (rel->rd_rel->relkind != relkind)
		elog(ERROR, "relation \"%s\" is not of kind '%c'", RelationGetRelationName(rel), relkind);
	if (RelationIsMapped(rel))
		elog(ERROR, "relation \"%s\" is mapped", RelationGetRelationName(rel));
	if (rel->rd_rel->relfilenode != relfilenumber)
	{
		Relation	pg_class = table_open(RelationRelationId, RowExclusiveLock);
		HeapTuple	tuple = SearchSysCacheLockedCopy1(RELOID, ObjectIdGetDatum(relid));
		ItemPointerData otid;
		SMgrRelation srel;

		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "could not find tuple for relation %u", relid);
		otid = tuple->t_self;
		((Form_pg_class) GETSTRUCT(tuple))->relfilenode = relfilenumber;
		CatalogTupleUpdate(pg_class, &otid, tuple);
		UnlockTuple(pg_class, &otid, InplaceUpdateTupleLock);
		heap_freetuple(tuple);
		table_close(pg_class, RowExclusiveLock);

		srel = smgropen(rel->rd_locator, rel->rd_backend);
		smgrdounlinkall(&srel, 1, false);
		smgrclose(srel);
		CommandCounterIncrement();
	}
	relation_close(rel, NoLock);
}

/*
 * binary_upgrade_set_history_relfilenode(rel, relfilenode, toast,
 * toast_index): a relation of pg_history given the old cluster's file
 * number, and its TOAST table and the TOAST table's index theirs (0: it has
 * none)
 */
Datum
binary_upgrade_set_history_relfilenode(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	RelFileNumber relfilenumber = PG_GETARG_OID(1);
	RelFileNumber toast = PG_GETARG_OID(2);
	RelFileNumber toast_index = PG_GETARG_OID(3);
	Relation	rel;
	char		relkind;
	Oid			toastrelid;

	check_binary_upgrade();
	rel = relation_open(relid, AccessExclusiveLock);
	if (RelationGetNamespace(rel) != PG_HISTORY_NAMESPACE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a relation of the history store",
						RelationGetRelationName(rel))));
	relkind = rel->rd_rel->relkind;
	toastrelid = rel->rd_rel->reltoastrelid;
	relation_close(rel, NoLock);
	if (relkind != RELKIND_RELATION && relkind != RELKIND_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table or an index", get_rel_name(relid))));
	if (OidIsValid(toastrelid) != (toast != InvalidRelFileNumber) ||
		(toast != InvalidRelFileNumber) != (toast_index != InvalidRelFileNumber))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("relation \"%s\" has a TOAST table in one cluster and not the other",
						get_rel_name(relid))));

	set_relfilenode(relid, relkind, relfilenumber);
	if (OidIsValid(toastrelid))
	{
		Relation	toastrel = relation_open(toastrelid, AccessExclusiveLock);
		List	   *indexes = RelationGetIndexList(toastrel);

		relation_close(toastrel, NoLock);
		if (list_length(indexes) != 1)
			elog(ERROR, "TOAST table of \"%s\" has %d indexes", get_rel_name(relid),
				 list_length(indexes));
		set_relfilenode(toastrelid, RELKIND_TOASTVALUE, toast);
		set_relfilenode(linitial_oid(indexes), RELKIND_INDEX, toast_index);
	}
	PG_RETURN_VOID();
}

/*
 * binary_upgrade_history_tier_place(): the place of the tier's objects the
 * settings say, a hash of the store's identity; NULL with none set
 */
Datum
binary_upgrade_history_tier_place(PG_FUNCTION_ARGS)
{
	uint64		place;

	/*
	 * (asked of a live old cluster too, by pg_upgrade --check; of settings
	 * only superusers see)
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to read the history store's tier's place")));
	place = ChunkTierPlace();
	if (place == 0)
		PG_RETURN_NULL();
	PG_RETURN_INT64((int64) place);
}

/*
 * binary_upgrade_history_formats(): the history store's formats this server
 * makes and reads, its meta pages' versions (a store of other formats is
 * not carried over)
 */
Datum
binary_upgrade_history_formats(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("chunk log %u, chunk index %u, tier %u",
											  ChunkLogFormatVersion(),
											  ChunkIndexFormatVersion(),
											  ChunkTierFormatVersion())));
}

/*
 * binary_upgrade_history_tier_mark(old_sysid): the database's tier, its
 * files the old cluster's, marked as this system's (the marker of its packs
 * named by this system; the old system's removed)
 */
Datum
binary_upgrade_history_tier_mark(PG_FUNCTION_ARGS)
{
	uint64		old_sysid = (uint64) PG_GETARG_INT64(0);
	Relation	rel;

	check_binary_upgrade();
	rel = relation_open(ChunkStoreTierLogRelid(), AccessExclusiveLock);
	ChunkTierMarkUpgraded(rel, old_sysid);
	relation_close(rel, NoLock);
	PG_RETURN_VOID();
}

/*
 * binary_upgrade_history_lane(lane, log): a lane of a chunk log, made again
 * by pg_dump --binary-upgrade, made the log's again (internally dependent
 * on it, as the lane's making made it)
 */
Datum
binary_upgrade_history_lane(PG_FUNCTION_ARGS)
{
	Oid			lane = PG_GETARG_OID(0);
	Oid			log = PG_GETARG_OID(1);
	ObjectAddress laneaddr,
				logaddr;

	check_binary_upgrade();
	if (get_rel_namespace(lane) != PG_HISTORY_NAMESPACE ||
		get_rel_namespace(log) != PG_HISTORY_NAMESPACE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("a lane and its log are relations of the history store")));
	ObjectAddressSet(laneaddr, RelationRelationId, lane);
	ObjectAddressSet(logaddr, RelationRelationId, log);
	recordDependencyOn(&laneaddr, &logaddr, DEPENDENCY_INTERNAL);
	PG_RETURN_VOID();
}
