/*-------------------------------------------------------------------------
 *
 * history_upgrade.c
 *	  The history store carried over by pg_upgrade.
 *
 * pg_upgrade transfers each database's history store (the relations of
 * schema pg_history, their TOAST tables and indexes) as it transfers the
 * relations users made: their files, the old cluster's, in place of the
 * new cluster's.  initdb made the new cluster's, of the same OIDs (the
 * same server makes both); the schema pg_dump --binary-upgrade restores
 * gives each the old one's file number first
 * (binary_upgrade_set_history_relfilenode), and its relfrozenxid and
 * relminmxid.  A tier's pack objects stay where they are, the new
 * cluster's to read (pg_upgrade refuses one whose tier's place is not the
 * old one's, binary_upgrade_history_tier_place); its marker, named by the
 * system, is the new system's once the files are there
 * (binary_upgrade_history_tier_mark).
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/history_upgrade.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/xact.h"
#include "catalog/binary_upgrade.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "prolly/chunk_store.h"
#include "prolly/chunk_tier.h"
#include "storage/lmgr.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/relcache.h"

static void
check_binary_upgrade(void)
{
	if (!IsBinaryUpgrade)
		ereport(ERROR,
				(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
				 errmsg("function can only be called when server is in binary upgrade mode")));
}

/* the relation given the file number, if it has not that one */
static void
set_relfilenode(Oid relid, char relkind, RelFileNumber relfilenumber)
{
	Relation	rel = relation_open(relid, AccessExclusiveLock);

	if (rel->rd_rel->relkind != relkind)
		elog(ERROR, "relation \"%s\" is not of kind '%c'", RelationGetRelationName(rel), relkind);
	if (rel->rd_rel->relfilenode != relfilenumber)
	{
		if (relkind == RELKIND_INDEX)
			binary_upgrade_next_index_pg_class_relfilenumber = relfilenumber;
		else if (relkind == RELKIND_TOASTVALUE)
			binary_upgrade_next_toast_pg_class_relfilenumber = relfilenumber;
		else
			binary_upgrade_next_heap_pg_class_relfilenumber = relfilenumber;
		RelationSetNewRelfilenumber(rel, rel->rd_rel->relpersistence);
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

	/* (asked of a live old cluster too, by pg_upgrade --check) */
	place = ChunkTierPlace();
	if (place == 0)
		PG_RETURN_NULL();
	PG_RETURN_INT64((int64) place);
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
