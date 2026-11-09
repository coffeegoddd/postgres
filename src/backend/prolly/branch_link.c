/*-------------------------------------------------------------------------
 *
 * branch_link.c
 *	  Which catalog branch each branch of the repository is, as rows of
 *	  pg_history.branches.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/prolly/branch_link.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_database.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "prolly/branch_catalog.h"
#include "prolly/branch_link.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

static void
link_connect(void)
{
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect to SPI");
}

static void
link_finish(void)
{
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "could not finish with SPI");
}

/*
 * (read directly, through a key, as a checkout reads it for any session, which the table's
 * privileges, its writers', don't give; as of now, as any statement reads)
 */
static Relation
links_open(void)
{
	Oid			history = get_namespace_oid("pg_history", false);

	return table_open(get_relname_relid("branches", history), AccessShareLock);
}

static int32
table_branch_of(const char *name)
{
	Relation	rel = links_open();
	Oid			key = get_relname_relid("branches_pkey", RelationGetNamespace(rel));
	ScanKeyData skey;
	Snapshot	snapshot;
	SysScanDesc scan;
	HeapTuple	tup;
	int32		branch = 0;

	ScanKeyEntryInitialize(&skey, 0, 1, BTEqualStrategyNumber, TEXTOID, C_COLLATION_OID, F_TEXTEQ,
						   CStringGetTextDatum(name));
	snapshot = RegisterSnapshot(GetLatestSnapshot());
	scan = systable_beginscan(rel, key, true, snapshot, 1, &skey);
	if (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		bool		isnull;

		branch = DatumGetInt32(heap_getattr(tup, 2, RelationGetDescr(rel), &isnull));
	}
	systable_endscan(scan);
	UnregisterSnapshot(snapshot);
	table_close(rel, AccessShareLock);
	return branch;
}

static char *
table_name_of(int32 branch)
{
	Relation	rel = links_open();
	Oid			key = get_relname_relid("branches_id_key", RelationGetNamespace(rel));
	ScanKeyData skey;
	Snapshot	snapshot;
	SysScanDesc scan;
	HeapTuple	tup;
	char	   *name = NULL;

	ScanKeyEntryInitialize(&skey, 0, 2, BTEqualStrategyNumber, INT4OID, InvalidOid, F_INT4EQ,
						   Int32GetDatum(branch));
	snapshot = RegisterSnapshot(GetLatestSnapshot());
	scan = systable_beginscan(rel, key, true, snapshot, 1, &skey);
	if (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		bool		isnull;

		name = TextDatumGetCString(heap_getattr(tup, 1, RelationGetDescr(rel), &isnull));
	}
	systable_endscan(scan);
	UnregisterSnapshot(snapshot);
	table_close(rel, AccessShareLock);
	return name;
}

static void
table_link(const char *name, int32 branch)
{
	Oid			types[2] = {TEXTOID, INT4OID};
	Datum		values[2] = {CStringGetTextDatum(name), Int32GetDatum(branch)};

	if (name[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a branch's name can't be empty")));
	if (branch <= 0 || !BranchIds->allocated(branch))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("branch %d has not been allocated", branch)));

	/* (one linking either at once waits for the other, and then finds it) */
	link_connect();
	if (SPI_execute_with_args("INSERT INTO pg_history.branches VALUES ($1, $2) ON CONFLICT DO NOTHING",
							  2, types, values, NULL, false, 0) != SPI_OK_INSERT)
		elog(ERROR, "could not write a branch's link");
	if (SPI_processed != 1)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("branch \"%s\" or catalog branch %d is linked already", name, branch)));
	link_finish();
}

static bool
table_unlink(const char *name)
{
	Oid			types[1] = {TEXTOID};
	Datum		values[1] = {CStringGetTextDatum(name)};
	bool		found;

	link_connect();
	if (SPI_execute_with_args("DELETE FROM pg_history.branches WHERE name OPERATOR(pg_catalog.=) $1",
							  1, types, values, NULL, false, 0) != SPI_OK_DELETE)
		elog(ERROR, "could not remove a branch's link");
	found = SPI_processed == 1;
	link_finish();
	return found;
}

static List *
table_names(void)
{
	Relation	rel = links_open();
	Snapshot	snapshot = RegisterSnapshot(GetLatestSnapshot());
	SysScanDesc scan = systable_beginscan(rel, InvalidOid, false, snapshot, 0, NULL);
	HeapTuple	tup;
	List	   *names = NIL;

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		bool		isnull;

		names = lappend(names, TextDatumGetCString(heap_getattr(tup, 1, RelationGetDescr(rel), &isnull)));
	}
	systable_endscan(scan);
	UnregisterSnapshot(snapshot);
	table_close(rel, AccessShareLock);
	return names;
}

const BranchLinkRoutine TableBranchLink = {
	.name = "table",
	.branch_of = table_branch_of,
	.name_of = table_name_of,
	.link = table_link,
	.unlink = table_unlink,
	.names = table_names,
};

const BranchLinkRoutine *BranchLink = &TableBranchLink;

/* (a database object lock of pg_database's class on the catalog branch's number, sub-id 3) */
void
LockBranchHead(int32 branch)
{
	LockDatabaseObject(DatabaseRelationId, (Oid) branch, 3, ExclusiveLock);
}
