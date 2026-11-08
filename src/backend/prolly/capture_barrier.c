/*-------------------------------------------------------------------------
 *
 * capture_barrier.c
 *	  A versioned schema's root value as of an exact point: every transaction
 *	  committed before it, and none after.
 *
 * Capture runs behind the tables.  So this writes a marker in the WAL, flushed,
 * and wakes the worker, which at the marker applies what it holds and saves each of
 * the branch's row maps as of it.  The root is then what prolly/ws/ws_restore.h's
 * capture makes of the schema, with those for its tables' rows.  A branch's root is
 * each of its schemas' at one marker, all of them locked as one schema's are.
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
 * transaction that's written anything, which would be neither in it nor clearly out.  One
 * whose writes are all to be left out of capture, from before it takes the root, is clearly out.
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
#include "commands/dbcommands.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "prolly/branch_catalog.h"
#include "prolly/capture.h"
#include "prolly/chunkstore.h"
#include "prolly/engine/vc.h"
#include "prolly/engine/vc_refs.h"
#include "prolly/ws/ws_dbroot.h"
#include "prolly/ws/ws_restore.h"
#include "prolly/ws/ws_rows.h"
#include "prolly/ws/ws_schema.h"
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

static List *branch_schemas(int32 branch);

/*
 * What a table's rows are read as while a barrier makes its root; and what they're written as,
 * as a root's violations are checked by restoring it, undone.
 */
static XLogRecPtr barrier_lsn = InvalidXLogRecPtr;
static VcRefs *barrier_refs = NULL;
static const WsRowsRoutine *barrier_rows = NULL;

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
					 errdetail("Its changes couldn't be applied: %s.", SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1)),
					 errhint("Try the commit again once capture has read its rows again.")));
	}
	if (!barrier_refs->routine->get(barrier_refs, name, VC_REF_READ, map))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("capture holds no row map of table \"%s\" of schema \"%s\"", table, schema)));
	pfree(table);
}

static void
captured_restore(ProllyCtx *ctx, const char *schema, const WsTableSchema *s, const PHash *map)
{
	barrier_rows->restore(ctx, schema, s, map);
}

static void
captured_apply(ProllyCtx *ctx, const char *schema, const WsTableSchema *was, const PHash *from,
			   const WsTableSchema *s, const PHash *to, uint64 *deleted, uint64 *inserted, uint64 *kept)
{
	barrier_rows->apply(ctx, schema, was, from, s, to, deleted, inserted, kept);
}

static const WsRowsRoutine CapturedWsRows = {
	.name = "captured",
	.capture = captured_rows,
	.restore = captured_restore,
	.apply = captured_apply,
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

/*
 * Whether this database's capture has its slot, made and kept, and not lost: a slot being made
 * reads every table under the lock what changes their rows untold takes turns on, and is kept once
 * that's committed; one invalidated is dropped and made again.
 */
static bool
slot_kept(void)
{
	char		name[NAMEDATALEN];
	ReplicationSlot *slot;
	bool		kept = false;

	snprintf(name, NAMEDATALEN, CAPTURE_SLOT_PREFIX "%u", MyDatabaseId);
	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	slot = SearchNamedReplicationSlot(name, false);
	if (slot != NULL)
	{
		SpinLockAcquire(&slot->mutex);
		kept = slot->data.persistency == RS_PERSISTENT && slot->data.invalidated == RS_INVAL_NONE;
		SpinLockRelease(&slot->mutex);
	}
	LWLockRelease(ReplicationSlotControlLock);
	return kept;
}

/*
 * Waits for this database's capture to have its slot, since start; an error past timeout_ms.  (A
 * marker is read only by a slot made before it, and a slot's making waits for the lock a barrier
 * holds, which this is called without, and for every transaction with an ID to end: so one that has
 * an ID, as a move's left out, can't wait for it, and it's a serialization failure.)
 */
static void
slot_wait(TimestampTz start, int timeout_ms)
{
	while (!slot_kept())
	{
		if (TransactionIdIsValid(GetTopTransactionIdIfAny()))
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("capture's slot was lost as a commit from capture held its tables"),
					 errhint("Try the commit again once capture has made its slot again.")));
		if (TimestampDifferenceExceeds(start, GetCurrentTimestamp(), timeout_ms))
			ereport(ERROR,
					(errcode(ERRCODE_QUERY_CANCELED),
					 errmsg("capture of database \"%s\" has no slot made in time", get_database_name(MyDatabaseId)),
					 errhint("Capture makes its slot once the database has a versioned schema, with capture_to_trees on.")));
		wake_worker();
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, 10, WAIT_EVENT_CAPTURE_BARRIER);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * Every branch's changes are applied together, and the slot moves only once they're committed, so
 * the slot's position is how far capture has got for each; a branch's own position moves only when
 * it has changes, so it isn't.  A slot made again, as one invalidated is, reads every table whole
 * as of where it begins, so from there on it has all that was committed before.  A table being
 * healed has had changes passed over, its rows to be told whole: until then capture hasn't got
 * there for it, which is an error, as a commit of its schema is.  Any table of the database, as
 * this is of all of it, and for as long as a writer that keeps it from being healed goes on.
 */
void
CaptureWaitFor(XLogRecPtr lsn, int timeout_ms)
{
	char		name[NAMEDATALEN];
	TimestampTz start = GetCurrentTimestamp();

	snprintf(name, NAMEDATALEN, CAPTURE_SLOT_PREFIX "%u", MyDatabaseId);
	for (;;)
	{
		ReplicationSlot *slot;
		XLogRecPtr	reached = InvalidXLogRecPtr;
		bool		lost = false;

		LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
		slot = SearchNamedReplicationSlot(name, false);
		if (slot != NULL)
		{
			SpinLockAcquire(&slot->mutex);
			reached = slot->data.confirmed_flush;
			lost = slot->data.invalidated != RS_INVAL_NONE;
			SpinLockRelease(&slot->mutex);
		}
		LWLockRelease(ReplicationSlotControlLock);
		if (slot == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("database \"%s\" is not being captured", get_database_name(MyDatabaseId))));
		if (lost)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("capture's replication slot \"%s\" was invalidated", name),
					 errdetail("What it hadn't applied is read again from the tables, once the slot is made again.")));
		if (reached >= lsn)
			break;
		if (TimestampDifferenceExceeds(start, GetCurrentTimestamp(), timeout_ms))
			ereport(ERROR,
					(errcode(ERRCODE_QUERY_CANCELED),
					 errmsg("capture did not reach %X/%X in time", LSN_FORMAT_ARGS(lsn)),
					 errdetail("It has reached %X/%X.", LSN_FORMAT_ARGS(reached))));
		wake_worker();
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, 2, WAIT_EVENT_CAPTURE_WAIT);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect to SPI");
	if (SPI_execute("SELECT relid, reason FROM pg_history.capture_healing ORDER BY relid LIMIT 1", true, 1) != SPI_OK_SELECT)
		elog(ERROR, "could not read the tables capture is healing");
	if (SPI_processed == 1)
	{
		bool		isnull;
		Oid			relid = DatumGetObjectId(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
		char	   *table = get_rel_name(relid);

		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("capture is healing table \"%s\" (OID %u)", table ? table : "?", relid),
				 errdetail("Its changes couldn't be applied: %s.", SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2)),
				 errhint("Try again once capture has read its rows again.")));
	}
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "could not finish with SPI");
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

/* Each of mode's privileges on one of pg_history's tables, asked of the caller. */
static void
check_history_table(Oid history, const char *table, AclMode mode)
{
	Oid			relid = get_relname_relid(table, history);

	if (pg_class_aclmask(relid, GetUserId(), mode, ACLMASK_ALL) != mode)
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE, table);
}

/*
 * What a barrier reads of capture's own (its barriers, what's being healed, and the maps it saved),
 * and with leaving_out what holding a map told writes, asked of the caller as the executor asks it:
 * so writing pg_history is a superuser's.
 */
static void
check_history(bool leaving_out)
{
	Oid			history = get_namespace_oid("pg_history", false);

	if (object_aclcheck(NamespaceRelationId, history, GetUserId(), ACL_USAGE) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_SCHEMA, "pg_history");
	check_history_table(history, "capture_barriers", ACL_SELECT);
	check_history_table(history, "capture_healing", ACL_SELECT);
	check_history_table(history, "pointers", leaving_out ? ACL_SELECT | ACL_INSERT | ACL_UPDATE : ACL_SELECT);
}

/* What reading each of the schema's tables and sequences asks, without locking them. */
static void
check_may_read_all(Oid nsp)
{
	Relation	rel = table_open(RelationRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *relations = NIL;

	ScanKeyInit(&key, Anum_pg_class_relnamespace, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(nsp));
	scan = systable_beginscan(rel, InvalidOid, false, NULL, 1, &key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_class c = (Form_pg_class) GETSTRUCT(tup);

		if (c->relkind == RELKIND_RELATION || c->relkind == RELKIND_PARTITIONED_TABLE ||
			c->relkind == RELKIND_SEQUENCE)
			relations = lappend_oid(relations, c->oid);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	foreach_oid(relid, relations)
		check_may_read(relid);
	list_free(relations);
}

/*
 * What's asked of a barrier before anything is held or written, of whatever the schema has now:
 * what it then holds is asked again once it's locked.  The schema's branch.
 */
/* What's asked of a barrier whatever it's of. */
static void
barrier_check_common(bool leaving_out)
{
	if (!capture_to_trees)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("a commit from capture needs capture_to_trees on")));
	if (IsolationUsesXactSnapshot())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a commit from capture can't run above READ COMMITTED")));
	if (TransactionIdIsValid(GetTopTransactionIdIfAny()))
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("a commit from capture can't run in a transaction that has written anything")));
	check_history(leaving_out);
}

static int32
barrier_check(const char *schema, Oid nsp, bool leaving_out)
{
	int32		branch = NamespaceBranch(nsp);

	if (!capture_to_trees)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("a commit from capture needs capture_to_trees on")));
	if (branch == 0)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("schema \"%s\" is not versioned", schema)));
	barrier_check_common(leaving_out);
	if (object_aclcheck(NamespaceRelationId, nsp, GetUserId(), ACL_USAGE) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_SCHEMA, schema);
	check_may_read_all(nsp);
	return branch;
}

/*
 * The roots of n schemas of one branch at one point, each in roots.  (with hold, catalog changes
 * keep waiting until the transaction ends)
 */
static void
barrier_take(ProllyCtx *ctx, const char *const *schemas, const Oid *nsps, int n, int32 branch,
			 bool whole_branch, int timeout_ms, PHash *roots, bool hold)
{
	XLogRecPtr	lsn;
	TimestampTz start;
	const WsRowsRoutine *rows = WsRows;
	Oid			types[1] = {PG_LSNOID};
	SPIPlanPtr	plan;
	List	   *tables = NIL;
	int			nest;
	bool		quick;
	char		quickly[32];
	MemoryContext cxt = CurrentMemoryContext;

	/* (asked of the caller as it began, and asked again by preparing) */
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect to SPI");
	plan = SPI_prepare("SELECT 1 FROM pg_history.capture_barriers WHERE lsn OPERATOR(pg_catalog.=) $1", 1, types);
	if (plan == NULL)
		elog(ERROR, "could not read a barrier");
	barrier_refs = VcRefsOpen("pointers", ChunkStoreOpen("pages"));

	/* (the slot first, holding no table, and the lock let go of again if it's gone once held) */
	start = GetCurrentTimestamp();
	slot_wait(start, timeout_ms);
	for (int i = 0; i < n; i++)
		tables = list_concat(tables, lock_schema_relations(nsps[i], true));
	INJECTION_POINT("capture-barrier-locked", NULL);
	for (;;)
	{
		LockSharedObject(DatabaseRelationId, MyDatabaseId, CAPTURE_ROWS_LOCK, ExclusiveLock);
		if (slot_kept())
			break;
		UnlockSharedObject(DatabaseRelationId, MyDatabaseId, CAPTURE_ROWS_LOCK, ExclusiveLock);
		slot_wait(start, timeout_ms);
	}
	AcceptInvalidationMessages();
	for (int i = 0; i < n; i++)
		if (get_namespace_oid(schemas[i], true) != nsps[i])
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("schema \"%s\" changed during a commit from capture", schemas[i]),
					 errhint("Try the commit again.")));
	/* (and with the whole branch's, none made since they were read: each of the n is still there) */
	if (whole_branch && list_length(branch_schemas(branch)) != n)
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("a schema of branch %d was made during a commit from capture", branch),
				 errhint("Try the commit again.")));
	list_free(tables);
	tables = NIL;
	for (int i = 0; i < n; i++)
		tables = list_concat(tables, lock_schema_relations(nsps[i], false));
	foreach_oid(relid, tables)
		check_may_read(relid);

	{
		int32		data[1] = {branch};

		lsn = LogLogicalMessage(CAPTURE_BARRIER_PREFIX, (const char *) data, sizeof(data), false, true);
	}
	wake_worker();

	while (!barrier_saved(plan, lsn))
	{
		if (TimestampDifferenceExceeds(start, GetCurrentTimestamp(), timeout_ms))
			ereport(ERROR,
					(errcode(ERRCODE_QUERY_CANCELED),
					 errmsg("capture did not reach %X/%X in time", LSN_FORMAT_ARGS(lsn))));

		/* (a slot made again, as one lost is, would wait for this lock to read the tables) */
		if (!slot_kept() && !barrier_saved(plan, lsn))
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("capture's slot was lost as a commit from capture waited for it"),
					 errhint("Try the commit again once capture has made its slot again.")));
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, 2, WAIT_EVENT_CAPTURE_BARRIER);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
		wake_worker();
	}

	INJECTION_POINT("capture-barrier-saved", NULL);

	/*
	 * (half of what would find a deadlock, as a user's commit must not be what one fails: so past it,
	 * waiting on another transaction, it's a serialization failure, as it can't tell whether the
	 * other waits on it in turn: one that put the same chunks, its barrier taken just before, mostly
	 * doesn't)
	 */
	nest = NewGUCNestLevel();
	quick = LockTimeout == 0 || LockTimeout > Max(DeadlockTimeout / 2, 1);
	snprintf(quickly, sizeof(quickly), "%d", Max(DeadlockTimeout / 2, 1));
	if (quick)
		(void) set_config_option("lock_timeout", quickly, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	barrier_lsn = lsn;
	barrier_rows = rows;
	WsRows = &CapturedWsRows;
	PG_TRY();
	{
		for (int i = 0; i < n; i++)
			WsRootIO->capture(ctx, schemas[i], &roots[i]);
	}
	PG_CATCH();
	{
		WsRows = rows;
		if (quick && geterrcode() == ERRCODE_LOCK_NOT_AVAILABLE)
		{
			ErrorData  *waited;

			/*
			 * (what it waited on kept, as the lock timeout's context names it: with every frame
			 * around it, so none is added again)
			 */
			MemoryContextSwitchTo(cxt);
			waited = CopyErrorData();
			FlushErrorState();
			error_context_stack = NULL;
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("could not serialize a commit from capture: it waited on another transaction"),
					 errdetail("It waits for another transaction for at most %s ms, as the other may wait for it.",
							   quickly),
					 errhint("Try the commit again."),
					 waited->context ? errcontext("%s", waited->context) : 0));
		}
		PG_RE_THROW();
	}
	PG_END_TRY();
	WsRows = rows;
	AtEOXact_GUC(true, nest);

	list_free(tables);
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "could not finish with SPI");
	if (!hold)
		UnlockSharedObject(DatabaseRelationId, MyDatabaseId, CAPTURE_ROWS_LOCK, ExclusiveLock);
}

void
CaptureBarrier(ProllyCtx *ctx, const char *schema, int timeout_ms, PHash *root)
{
	Oid			nsp = get_namespace_oid(schema, false);

	barrier_take(ctx, &schema, &nsp, 1, barrier_check(schema, nsp, false), false, timeout_ms, root, false);
}

/* (each of the branch's schemas, by name) */
static int
schema_name_cmp(const void *a, const void *b)
{
	return strcmp(*(const char *const *) a, *(const char *const *) b);
}

/* The names of a branch's schemas, as the catalogs have them now (pg_namespace has every branch's). */
static List *
branch_schemas(int32 branch)
{
	Relation	rel = table_open(NamespaceRelationId, AccessShareLock);
	SysScanDesc scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
	HeapTuple	tup;
	List	   *names = NIL;

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_namespace ns = (Form_pg_namespace) GETSTRUCT(tup);

		if (ns->nspbranch == branch)
			names = lappend(names, pstrdup(NameStr(ns->nspname)));
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	return names;
}

void
CaptureBranchBarrier(ProllyCtx *ctx, int32 branch, int timeout_ms, PHash *root)
{
	List	   *names;
	const char **schemas;
	char	  **keys;
	Oid		   *nsps;
	PHash	   *roots;
	int			n;
	int			i = 0;
	int			level;

	if (branch <= 0 || !BranchIds->allocated(branch))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("branch %d has not been allocated", branch)));
	barrier_check_common(false);
	level = CatalogBranchEnter(branch);
	names = branch_schemas(branch);
	n = list_length(names);
	schemas = palloc_array(const char *, Max(n, 1));
	keys = palloc_array(char *, Max(n, 1));
	nsps = palloc_array(Oid, Max(n, 1));
	roots = palloc_array(PHash, Max(n, 1));
	foreach_ptr(char, name, names)
		schemas[i++] = name;
	qsort(schemas, n, sizeof(const char *), schema_name_cmp);
	for (i = 0; i < n; i++)
	{
		/* (one dropped since they were read, as one dropped once they're locked) */
		nsps[i] = get_namespace_oid(schemas[i], true);
		if (!OidIsValid(nsps[i]))
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("schema \"%s\" changed during a commit from capture", schemas[i]),
					 errhint("Try the commit again.")));
		(void) barrier_check(schemas[i], nsps[i], false);
		/* (its name as the root has it, before anything's written) */
		keys[i] = ws_name_to_utf8(schemas[i]);
	}

	/* (with no schema, nothing to wait for: an empty root, as of any point) */
	*root = vc_root_empty(ctx);
	if (n > 0)
		barrier_take(ctx, schemas, nsps, n, branch, true, timeout_ms, roots, false);
	for (i = 0; i < n; i++)
		WsDbRoot->put_schema(ctx, root, keys[i], &roots[i], root);
	CatalogBranchLeave(level);
}


/*
 * (its writes are left out, so the root is of all that's committed, and none of it; and the lock
 * catalog changes take turns on is held to its end, which its own commit takes once it has the
 * tables, as one of theirs may want a table it has)
 */
void
CaptureBarrierLeavingOut(ProllyCtx *ctx, const char *schema, int timeout_ms, void (*hold) (void *arg),
						 void *arg, PHash *root)
{
	Oid			nsp = get_namespace_oid(schema, false);
	int32		branch = barrier_check(schema, nsp, true);

	/* (before leaving out gives this transaction an ID, which a slot being made would wait for) */
	slot_wait(GetCurrentTimestamp(), timeout_ms);
	CaptureLeaveOut();
	hold(arg);
	barrier_take(ctx, &schema, &nsp, 1, branch, false, timeout_ms, root, true);
}
