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
 * runs, with catalog changes in this database held off: it holds the lock that has them
 * take turns (capture_rows.c) until the root is built.  It's the schema's relations that building
 * a root reads that it locks first, without that lock, as what's committing a catalog change can
 * hold one while waiting for that lock.  What a view depends on elsewhere can only be locked as
 * it's found, so building gives up on any lock quickly, before a deadlock could be found and a
 * user's commit be what fails for it: it's this that's an error, to try again.  That goes for
 * waiting for another transaction too, as for one that's written the same chunk and is still
 * open, which otherwise would have every catalog change wait for it.  So DDL on the schema, and catalog changes, wait for this and for capture to
 * catch up, and whatever then wants a table queued behind DDL waits too.  Row writers
 * themselves don't wait for it.  A change of a role from another database isn't held off.
 *
 * It's an error above READ COMMITTED, as it reads what's committed now, and in a
 * transaction that's written anything, which would be neither in it nor clearly out.
 * What can't be held in a commit, or a table capture holds no map of, is an error too.
 * The caller needs what it would to read the tables, as it's given all their rows, so a role
 * row security applies to is refused.  Those are asked before anything's written; what can't be
 * held in a commit is only found building the root, after the marker.
 *
 * It writes nothing of capture's: what was saved stays until the worker sees the next
 * marker.  The root's chunks are held by this transaction's having put them, and nothing else,
 * so it's to be committed in this transaction: a pointer to it after that may be refused.
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
#include "catalog/objectaddress.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
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
#include "storage/lock.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/injection_point.h"
#include "utils/rls.h"
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
	/* (as capture has let go of its map: what it has is of part of it, or nothing) */
	{
		Oid			types[1] = {OIDOID};
		Datum		values[1] = {ObjectIdGetDatum(relid)};

		if (SPI_execute_with_args("SELECT reason FROM pg_history.capture_healing WHERE relid OPERATOR(pg_catalog.=) $1",
								  1, types, values, NULL, true, 1) != SPI_OK_SELECT)
			elog(ERROR, "could not read the tables capture is healing");
		if (SPI_processed == 1)
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("capture is healing table \"%s\" of schema \"%s\"", table, schema),
					 errdetail("Its changes couldn't be applied: %s", SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1)),
					 errhint("Try the commit again once its rows have been told whole.")));
	}
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

/* Every relation of the schema that building a root reads, locked; those whose contents it holds. */
static List *
lock_schema_relations(Oid nsp, bool wait)
{
	List	   *relations = NIL;
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

		if (c->relkind == RELKIND_RELATION || c->relkind == RELKIND_PARTITIONED_TABLE ||
			c->relkind == RELKIND_SEQUENCE)
			tables = lappend_oid(tables, c->oid);
		if (c->relkind == RELKIND_RELATION || c->relkind == RELKIND_PARTITIONED_TABLE ||
			c->relkind == RELKIND_VIEW || c->relkind == RELKIND_MATVIEW ||
			c->relkind == RELKIND_COMPOSITE_TYPE ||
			c->relkind == RELKIND_SEQUENCE || c->relkind == RELKIND_FOREIGN_TABLE)
			relations = lappend_oid(relations, c->oid);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	foreach_oid(relid, relations)
	{
		if (wait)
			LockRelationOid(relid, AccessShareLock);
		else if (!ConditionalLockRelationOid(relid, AccessShareLock))
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("a relation of the schema was made during a commit from capture"),
					 errhint("Try the commit again.")));
	}
	list_free(relations);
	return tables;
}

/* What giving the caller all of a table's rows, or a sequence's position, asks of it. */
static void
check_may_read(Oid relid)
{
	Oid			user = GetUserId();

	if (pg_class_aclcheck(relid, user, ACL_SELECT) != ACLCHECK_OK &&
		pg_attribute_aclcheck_all(relid, user, ACL_SELECT, ACLMASK_ALL) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, get_relkind_objtype(get_rel_relkind(relid)), get_rel_name(relid));
	if (check_enable_rls(relid, InvalidOid, false) == RLS_ENABLED)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("a commit from capture can't be taken by a role row security applies to on table \"%s\"",
						get_rel_name(relid))));
}

/* The worker reading the slot of this database's capture, woken, if there is one. */
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
barrier_saved(SPIPlanPtr plan, XLogRecPtr lsn)
{
	Datum		values[1] = {LSNGetDatum(lsn)};
	bool		saved;

	if (SPI_execute_plan(plan, values, NULL, false, 1) != SPI_OK_SELECT)
		elog(ERROR, "could not read a barrier");
	saved = SPI_processed == 1;
	SPI_freetuptable(SPI_tuptable);
	return saved;
}

void
CaptureBarrier(ProllyCtx *ctx, const char *schema, int timeout_ms, PHash *root)
{
	Oid			nsp = get_namespace_oid(schema, false);
	int32		branch = NamespaceBranch(nsp);
	XLogRecPtr	lsn;
	TimestampTz start;
	const WsRowsRoutine *rows = WsRows;
	Oid			types[1] = {PG_LSNOID};
	SPIPlanPtr	plan;
	List	   *tables;
	int			nest;
	char		quickly[32];

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

	if (object_aclcheck(NamespaceRelationId, nsp, GetUserId(), ACL_USAGE) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_SCHEMA, schema);

	/* (preparing asks for pg_history's schema, which is what guards it, before anything's held or written) */
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect to SPI");
	plan = SPI_prepare("SELECT 1 FROM pg_history.capture_barriers WHERE lsn OPERATOR(pg_catalog.=) $1", 1, types);
	if (plan == NULL)
		elog(ERROR, "could not read a barrier");
	barrier_refs = VcRefsOpen("pointers", ChunkStoreOpen("pages"));

	tables = lock_schema_relations(nsp, true);
	INJECTION_POINT("capture-barrier-locked", NULL);
	LockSharedObject(DatabaseRelationId, MyDatabaseId, CAPTURE_ROWS_LOCK, ExclusiveLock);
	AcceptInvalidationMessages();
	if (get_namespace_oid(schema, true) != nsp)
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("schema \"%s\" changed during a commit from capture", schema),
				 errhint("Try the commit again.")));
	list_free(tables);
	tables = lock_schema_relations(nsp, false);
	foreach_oid(relid, tables)
		check_may_read(relid);

	{
		int32		data[1] = {branch};

		lsn = LogLogicalMessage(CAPTURE_BARRIER_PREFIX, (const char *) data, sizeof(data), false, true);
	}
	wake_worker();

	start = GetCurrentTimestamp();
	while (!barrier_saved(plan, lsn))
	{
		if (TimestampDifferenceExceeds(start, GetCurrentTimestamp(), timeout_ms))
			ereport(ERROR,
					(errcode(ERRCODE_QUERY_CANCELED),
					 errmsg("capture did not reach %X/%X in time", LSN_FORMAT_ARGS(lsn))));
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, 2, WAIT_EVENT_CAPTURE_BARRIER);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
		wake_worker();
	}

	INJECTION_POINT("capture-barrier-saved", NULL);

	/* (half of what would find a deadlock, as a user's commit must not be what one fails) */
	nest = NewGUCNestLevel();
	snprintf(quickly, sizeof(quickly), "%d", Max(DeadlockTimeout / 2, 1));
	if (LockTimeout == 0 || LockTimeout > Max(DeadlockTimeout / 2, 1))
		(void) set_config_option("lock_timeout", quickly, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	barrier_lsn = lsn;
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
	AtEOXact_GUC(true, nest);

	list_free(tables);
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "could not finish with SPI");
	UnlockSharedObject(DatabaseRelationId, MyDatabaseId, CAPTURE_ROWS_LOCK, ExclusiveLock);
}
