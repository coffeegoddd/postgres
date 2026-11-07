/*-------------------------------------------------------------------------
 *
 * capture_barrier.c
 *	  A versioned schema's root value as of an exact point: every transaction
 *	  committed before it, and none after.
 *
 * Capture runs behind the tables.  So this writes a marker in the WAL, flushed,
 * and wakes the worker, which at the marker applies what it holds and saves each of
 * the branch's row maps as of it.  The root is then what prolly/ws/ws_restore.h's
 * capture makes of the schema, with those for its tables' rows.
 *
 * What the WAL doesn't carry (definitions, sequences' positions) is read as this
 * runs, with catalog changes held off: it holds the lock that has them take turns
 * (capture_rows.c) until the root is built, having locked the schema's tables first,
 * as one of those committing could hold a table and be waiting for that lock.  So
 * DDL and catalog changes wait for this, and for capture to catch up.  Row writers
 * never do.
 *
 * It's an error above READ COMMITTED, as it reads what's committed now, and in a
 * transaction that's written anything, which would be neither in it nor clearly out.
 * What can't be held in a commit, or a table capture holds no map of, is an error too.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/capture_barrier.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "prolly/branch_catalog.h"
#include "prolly/capture.h"
#include "prolly/chunkstore.h"
#include "prolly/engine/vc_refs.h"
#include "prolly/ws/ws_restore.h"
#include "prolly/ws/ws_rows.h"
#include "replication/message.h"
#include "replication/slot.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procnumber.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/pg_lsn.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/inval.h"
#include "utils/wait_event.h"

/* What a table's rows are read as while a barrier makes its root. */
static XLogRecPtr barrier_lsn = InvalidXLogRecPtr;
static VcRefs *barrier_refs = NULL;

static void
captured_rows(ProllyCtx *ctx, const char *schema, const WsTableSchema *s, PHash *map)
{
	char	   *table = ws_name_from_utf8(s->name);
	Oid			relid = get_relname_relid(table, get_namespace_oid(schema, false));
	char		name[128];

	if (!OidIsValid(relid))
		elog(ERROR, "table \"%s\" of schema \"%s\" is gone", table, schema);

	/* (a partitioned table holds no rows of its own, as capture has it) */
	if (get_rel_relkind(relid) == RELKIND_PARTITIONED_TABLE)
	{
		if (prolly_empty(ctx, WS_ROW_VREFS, map) < 0)
			prolly_report(ctx);
		pfree(table);
		return;
	}
	snprintf(name, sizeof(name), CAPTURE_BARRIER_REF_PREFIX "%X/%X/%u", LSN_FORMAT_ARGS(barrier_lsn), relid);
	if (!barrier_refs->routine->get(barrier_refs, name, VC_REF_READ, map))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("capture holds no row map of table \"%s\" of schema \"%s\"", table, schema)));
	pfree(table);
}

static const WsRowsRoutine CapturedWsRows = {
	.name = "captured",
	.capture = captured_rows,
};

/* Every table of the schema, of a kind with rows, locked so that no one's DDL on them is in flight. */
static List *
lock_schema_tables(Oid nsp, bool wait)
{
	List	   *tables = NIL;
	Relation	rel = table_open(RelationRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;

	ScanKeyInit(&key, Anum_pg_class_relnamespace, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(nsp));
	scan = systable_beginscan(rel, InvalidOid, false, NULL, 1, &key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_class c = (Form_pg_class) GETSTRUCT(tup);

		if (c->relkind == RELKIND_RELATION || c->relkind == RELKIND_PARTITIONED_TABLE)
			tables = lappend_oid(tables, c->oid);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	foreach_oid(relid, tables)
	{
		if (wait)
			LockRelationOid(relid, AccessShareLock);
		else if (!ConditionalLockRelationOid(relid, AccessShareLock))
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("a table of the schema was made during a commit from capture"),
					 errhint("Try the commit again.")));
	}
	return tables;
}

/* The worker reading the slot of this database's capture, woken; an error if there's none. */
static void
wake_worker(void)
{
	char		name[NAMEDATALEN];
	ReplicationSlot *slot;
	ProcNumber	proc = INVALID_PROC_NUMBER;

	snprintf(name, NAMEDATALEN, CAPTURE_SLOT_PREFIX "%u", MyDatabaseId);
	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	slot = SearchNamedReplicationSlot(name, false);
	if (slot != NULL)
		proc = slot->active_proc;
	LWLockRelease(ReplicationSlotControlLock);
	if (proc != INVALID_PROC_NUMBER)
		SetLatch(&GetPGProcByNumber(proc)->procLatch);
}

/* Whether the worker has saved the maps as of the marker. */
static bool
barrier_saved(XLogRecPtr lsn)
{
	Oid			types[1] = {PG_LSNOID};
	Datum		values[1] = {LSNGetDatum(lsn)};

	if (SPI_execute_with_args("SELECT 1 FROM pg_history.capture_barriers WHERE lsn = $1",
							  1, types, values, NULL, false, 1) != SPI_OK_SELECT)
		elog(ERROR, "could not read a barrier");
	return SPI_processed == 1;
}

/* A reference of a barrier's, gone with it. */
static void
note_ref(void *arg, const char *name, const PHash *hash)
{
	List	  **names = arg;

	*names = lappend(*names, pstrdup(name));
}

void
CaptureBarrier(ProllyCtx *ctx, const char *schema, int timeout_ms, PHash *root)
{
	Oid			nsp = get_namespace_oid(schema, false);
	int32		branch = NamespaceBranch(nsp);
	char		prefix[64];
	XLogRecPtr	lsn;
	TimestampTz start;
	const WsRowsRoutine *rows = WsRows;
	List	   *names = NIL;
	Oid			types[1] = {PG_LSNOID};
	Datum		values[1];

	if (!capture_to_trees)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("a commit from capture needs capture_to_trees on")));
	if (branch == 0)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("schema \"%s\" is not versioned", schema)));
	if (IsolationUsesXactSnapshot())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a commit from capture can't run above READ COMMITTED")));
	if (TransactionIdIsValid(GetTopTransactionIdIfAny()))
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("a commit from capture can't run in a transaction that has written anything")));

	/*
	 * Tables first, as a transaction committing a catalog change can hold one while it waits for the
	 * lock below; then that lock; then whichever tables were made meanwhile, which this can't wait for.
	 */
	(void) lock_schema_tables(nsp, true);
	LockSharedObject(DatabaseRelationId, MyDatabaseId, CAPTURE_ROWS_LOCK, ExclusiveLock);
	AcceptInvalidationMessages();
	(void) lock_schema_tables(nsp, false);

	{
		int32		data[1] = {branch};

		lsn = LogLogicalMessage(CAPTURE_BARRIER_PREFIX, (const char *) data, sizeof(data), false, true);
	}
	wake_worker();

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect to SPI");
	start = GetCurrentTimestamp();
	while (!barrier_saved(lsn))
	{
		if (TimestampDifferenceExceeds(start, GetCurrentTimestamp(), timeout_ms))
			ereport(ERROR,
					(errcode(ERRCODE_QUERY_CANCELED),
					 errmsg("capture did not reach %X/%X in time", LSN_FORMAT_ARGS(lsn))));
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, 2, WAIT_EVENT_CAPTURE_WORKER_MAIN);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
		wake_worker();
	}

	barrier_lsn = lsn;
	barrier_refs = VcRefsOpen("pointers", ChunkStoreOpen("pages"));
	WsRows = &CapturedWsRows;
	PG_TRY();
	{
		WsRootIO->capture(ctx, schema, root);
	}
	PG_FINALLY();
	{
		WsRows = rows;
	}
	PG_END_TRY();

	snprintf(prefix, sizeof(prefix), CAPTURE_BARRIER_REF_PREFIX "%X/%X/", LSN_FORMAT_ARGS(lsn));
	barrier_refs->routine->list(barrier_refs, prefix, note_ref, &names);
	foreach_ptr(char, name, names)
		(void) barrier_refs->routine->remove(barrier_refs, name);
	values[0] = LSNGetDatum(lsn);
	if (SPI_execute_with_args("DELETE FROM pg_history.capture_barriers WHERE lsn = $1",
							  1, types, values, NULL, false, 0) != SPI_OK_DELETE)
		elog(ERROR, "could not forget a barrier");
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "could not finish with SPI");
	UnlockSharedObject(DatabaseRelationId, MyDatabaseId, CAPTURE_ROWS_LOCK, ExclusiveLock);
}
