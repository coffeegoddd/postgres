/*-------------------------------------------------------------------------
 *
 * capture.c
 *	  Keeping each branch's working tree equal to its tables, from the WAL.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/capture.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "access/xlog_internal.h"
#include "catalog/indexing.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "prolly/capture.h"
#include "replication/logical.h"
#include "replication/logicalctl.h"
#include "replication/slot.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/injection_point.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/wait_event.h"

#define CAPTURE_SLOT_PREFIX "prolly_capture_"

/* How often the launcher looks, and how often a worker looks for what is new. */
#define CAPTURE_LAUNCHER_NAPTIME_MS 5000
#define CAPTURE_WORKER_NAPTIME_MS 1000

/* The sub-id of the lock on a database that says who may set or clear its mark. */
#define CAPTURE_MARK_LOCK 1

bool		capture_in_background = true;

static void
slot_name_of(Oid dboid, char *name)
{
	snprintf(name, NAMEDATALEN, CAPTURE_SLOT_PREFIX "%u", dboid);
}

static bool
capture_possible(void)
{
	return wal_level >= WAL_LEVEL_REPLICA && max_replication_slots > 0;
}

/* -------------------------------------------------------------- the plugin */

static void
capture_startup(LogicalDecodingContext *ctx, OutputPluginOptions *opt, bool is_init)
{
	opt->output_type = OUTPUT_PLUGIN_BINARY_OUTPUT;
}

static void
capture_begin(LogicalDecodingContext *ctx, ReorderBufferTXN *txn)
{
}

static void
capture_change(LogicalDecodingContext *ctx, ReorderBufferTXN *txn, Relation relation,
			   ReorderBufferChange *change)
{
}

static void
capture_commit(LogicalDecodingContext *ctx, ReorderBufferTXN *txn, XLogRecPtr commit_lsn)
{
}

void
CaptureOutputPluginInit(OutputPluginCallbacks *callbacks)
{
	callbacks->startup_cb = capture_startup;
	callbacks->begin_cb = capture_begin;
	callbacks->change_cb = capture_change;
	callbacks->commit_cb = capture_commit;
}

/* ---------------------------------------------------------------- starting */

static bool
capture_launch(Oid dboid, BackgroundWorkerHandle **handle)
{
	BackgroundWorker bgw;

	memset(&bgw, 0, sizeof(bgw));
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION | BGWORKER_INTERRUPTIBLE;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	bgw.bgw_restart_time = BGW_NEVER_RESTART;
	snprintf(bgw.bgw_library_name, MAXPGPATH, "postgres");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "CaptureWorkerMain");
	snprintf(bgw.bgw_name, BGW_MAXLEN, "capture for database %u", dboid);
	snprintf(bgw.bgw_type, BGW_MAXLEN, "capture worker");
	bgw.bgw_main_arg = ObjectIdGetDatum(dboid);
	return RegisterDynamicBackgroundWorker(&bgw, handle);
}

/* Whether this database's slot has its worker. */
static bool
slot_is_held(Oid dboid)
{
	char		name[NAMEDATALEN];
	ReplicationSlot *slot;
	bool		held;

	slot_name_of(dboid, name);
	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	slot = SearchNamedReplicationSlot(name, false);
	held = slot != NULL && slot->active_proc != INVALID_PROC_NUMBER;
	LWLockRelease(ReplicationSlotControlLock);
	return held;
}

static bool schema_made = false;

static void
capture_at_xact_end(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_COMMIT && schema_made && capture_in_background && capture_possible() &&
		!IsBinaryUpgrade && !slot_is_held(MyDatabaseId))
		(void) capture_launch(MyDatabaseId, NULL);
	if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_ABORT ||
		event == XACT_EVENT_PARALLEL_COMMIT || event == XACT_EVENT_PARALLEL_ABORT ||
		event == XACT_EVENT_PREPARE)
		schema_made = false;
}

/* Sets or clears this database's mark, with the lock that says who may. */
static void
database_mark(bool versioned)
{
	Relation	rel = table_open(DatabaseRelationId, RowExclusiveLock);
	HeapTuple	tup = SearchSysCacheLockedCopy1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
	ItemPointerData otid;

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for database %u", MyDatabaseId);
	otid = tup->t_self;
	if (((Form_pg_database) GETSTRUCT(tup))->datversioned != versioned)
	{
		((Form_pg_database) GETSTRUCT(tup))->datversioned = versioned;
		CatalogTupleUpdate(rel, &otid, tup);
		CommandCounterIncrement();
	}
	UnlockTuple(rel, &otid, InplaceUpdateTupleLock);
	heap_freetuple(tup);
	table_close(rel, RowExclusiveLock);
}

static bool
database_is_marked(void)
{
	HeapTuple	tup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
	bool		marked;

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for database %u", MyDatabaseId);
	marked = ((Form_pg_database) GETSTRUCT(tup))->datversioned;
	ReleaseSysCache(tup);
	return marked;
}

/*
 * To be called before the schema is: what it waits for mustn't be waiting for that schema's name.
 * Held until this commits, the lock keeps a worker that finds no schema from clearing the mark.  A
 * database already marked needs no more than a shared hold of it, so makers don't wait on each
 * other; only one that has to set the mark waits, and is waited for.
 */
void
CaptureSchemaMade(void)
{
	static bool registered = false;

	if (!registered)
	{
		RegisterXactCallback(capture_at_xact_end, NULL);
		registered = true;
	}

	for (;;)
	{
		bool		marked;

		AcceptInvalidationMessages();
		marked = database_is_marked();
		LockSharedObject(DatabaseRelationId, MyDatabaseId, CAPTURE_MARK_LOCK, marked ? ShareLock : ExclusiveLock);
		if (!marked)
		{
			database_mark(true);
			break;
		}
		if (database_is_marked())
			break;

		/* (cleared before this got the lock, and so held no earlier in this transaction) */
		UnlockSharedObject(DatabaseRelationId, MyDatabaseId, CAPTURE_MARK_LOCK, ShareLock);
	}
	schema_made = true;
}

/* ---------------------------------------------------------------- the worker */

/* Whether this database has a versioned schema, on any branch, in the transaction running. */
static bool
versioned_schema_exists(void)
{
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	bool		found = false;

	PushActiveSnapshot(GetTransactionSnapshot());
	rel = table_open(NamespaceRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);
	while (!found && (tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
		found = ((Form_pg_namespace) GETSTRUCT(tup))->nspbranch != 0;
	table_endscan(scan);
	table_close(rel, AccessShareLock);
	PopActiveSnapshot();
	return found;
}

static bool
database_has_versioned_schema(void)
{
	bool		found;

	StartTransactionCommand();
	found = versioned_schema_exists();
	CommitTransactionCommand();
	return found;
}

static void
capture_slot_create(const char *name)
{
	LogicalDecodingContext *ctx;

	StartTransactionCommand();
	ReplicationSlotCreate(name, true, RS_EPHEMERAL, false, false, false, false);
	EnsureLogicalDecodingEnabled();
	ctx = CreateInitDecodingContext(PROLLY_CAPTURE_PLUGIN, NIL, false, false, InvalidXLogRecPtr,
									XL_ROUTINE(.page_read = read_local_xlog_page,
											   .segment_open = wal_segment_open,
											   .segment_close = wal_segment_close),
									NULL, NULL, NULL);
	DecodingContextFindStartpoint(ctx);
	FreeDecodingContext(ctx);
	ReplicationSlotPersist();
	ReplicationSlotRelease();
	CommitTransactionCommand();
}

/* Moves the slot up to what is flushed, so it holds nothing back. */
static void
capture_advance(void)
{
	XLogRecPtr	to = GetFlushRecPtr(NULL);

	if (MyReplicationSlot->data.confirmed_flush >= to)
		return;
	StartTransactionCommand();
	(void) LogicalSlotAdvanceAndCheckSnapState(to, NULL);
	CommitTransactionCommand();
}

/*
 * Clears the mark of a database with no versioned schema; false if one is being made, or there
 * now is one.  It looks again with the lock held, which whoever makes one holds until they commit.
 */
static bool
capture_clear_mark(void)
{
	bool		cleared = false;

	INJECTION_POINT("capture-clear-mark", NULL);
	StartTransactionCommand();
	if (ConditionalLockSharedObject(DatabaseRelationId, MyDatabaseId, CAPTURE_MARK_LOCK, ExclusiveLock) &&
		!versioned_schema_exists())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		database_mark(false);
		PopActiveSnapshot();
		cleared = true;
	}
	CommitTransactionCommand();
	return cleared;
}

/*
 * Makes this database's slot if it has none.  False, with why, if this worker has nothing to do:
 * it has a worker, or its name is some other slot's.  An invalidated slot has lost WAL it needs,
 * so it is dropped and made again.
 */
static bool
capture_slot_ready(const char *name)
{
	ReplicationSlot *slot;
	bool		exists;
	bool		held = false;
	bool		ours = true;
	bool		lost = false;

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	slot = SearchNamedReplicationSlot(name, false);
	exists = slot != NULL;
	if (exists)
	{
		held = slot->active_proc != INVALID_PROC_NUMBER;
		ours = slot->data.database == MyDatabaseId &&
			strcmp(NameStr(slot->data.plugin), PROLLY_CAPTURE_PLUGIN) == 0;
		lost = slot->data.invalidated != RS_INVAL_NONE;
	}
	LWLockRelease(ReplicationSlotControlLock);
	if (held)
		return false;
	if (!ours)
	{
		ereport(WARNING,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("replication slot \"%s\" is not capture's, so this database is not being captured", name)));
		return false;
	}
	if (lost)
	{
		ereport(WARNING,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("capture's replication slot \"%s\" was invalidated, and is being made again", name)));
		ReplicationSlotDrop(name, true);
		exists = false;
	}
	if (!exists)
		capture_slot_create(name);
	return true;
}

void
CaptureWorkerMain(Datum main_arg)
{
	Oid			dboid = DatumGetObjectId(main_arg);
	char		name[NAMEDATALEN];

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();
	BackgroundWorkerInitializeConnectionByOid(dboid, InvalidOid, BGWORKER_BYPASS_ALLOWCONN);

	/* (this sees every branch's schemas, not those of the branch it isn't on) */
	SetConfigOption("catalog_branch_filter", "off", PGC_SUSET, PGC_S_OVERRIDE);
	slot_name_of(dboid, name);

	if (!capture_possible())
		proc_exit(0);

	/* (a mark with no schema is cleared whatever is wrong with the slot; the launcher takes the slot) */
	if (!database_has_versioned_schema() && capture_clear_mark())
		proc_exit(0);
	if (!capture_slot_ready(name))
		proc_exit(0);
	ReplicationSlotAcquire(name, true, true);

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/* the launcher drops a slot with no worker, if capture is off */
		if (!capture_in_background)
			break;
		if (!database_has_versioned_schema() && capture_clear_mark())
		{
			ReplicationSlotRelease();
			ReplicationSlotDrop(name, true);
			break;
		}
		capture_advance();
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 CAPTURE_WORKER_NAPTIME_MS, WAIT_EVENT_CAPTURE_WORKER_MAIN);
		ResetLatch(MyLatch);
	}
	proc_exit(0);
}

void
CaptureDatabaseDropping(Oid dboid)
{
	char		name[NAMEDATALEN];
	TimestampTz start = GetCurrentTimestamp();

	slot_name_of(dboid, name);

	/* (another can't start: it waits on the lock held on the database) */
	TerminateBackgroundWorkersForDatabase(dboid);
	while (slot_is_held(dboid))
	{
		CHECK_FOR_INTERRUPTS();

		/* (the count of a database's slots says, if this doesn't get there) */
		if (TimestampDifferenceExceeds(start, GetCurrentTimestamp(), 5000))
			break;
		pg_usleep(10000);
	}
}

/* -------------------------------------------------------------- the launcher */

typedef struct CaptureRunning
{
	Oid			dboid;
	BackgroundWorkerHandle *handle;
} CaptureRunning;

/* The databases marked as having versioned schemas. */
static List *
databases_marked(void)
{
	List	   *dbs = NIL;
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	MemoryContext cxt = CurrentMemoryContext;

	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	rel = table_open(DatabaseRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);
	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_database db = (Form_pg_database) GETSTRUCT(tup);

		if (db->datversioned)
		{
			MemoryContext old = MemoryContextSwitchTo(cxt);

			dbs = lappend_oid(dbs, db->oid);
			MemoryContextSwitchTo(old);
		}
	}
	table_endscan(scan);
	table_close(rel, AccessShareLock);
	PopActiveSnapshot();
	CommitTransactionCommand();
	return dbs;
}

/* Drops capture's slots with no worker, of no database in the list; with none, all of them. */
static void
drop_slots_not_of(List *dbs)
{
	List	   *names = NIL;
	ListCell   *lc;

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (int i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];

		if (s->in_use && s->active_proc == INVALID_PROC_NUMBER &&
			strcmp(NameStr(s->data.plugin), PROLLY_CAPTURE_PLUGIN) == 0 &&
			strncmp(NameStr(s->data.name), CAPTURE_SLOT_PREFIX, strlen(CAPTURE_SLOT_PREFIX)) == 0 &&
			!list_member_oid(dbs, s->data.database))
			names = lappend(names, pstrdup(NameStr(s->data.name)));
	}
	LWLockRelease(ReplicationSlotControlLock);
	foreach(lc, names)
		ReplicationSlotDrop(lfirst(lc), true);
}

void
CaptureLauncherMain(Datum main_arg)
{
	/* (each a palloc in this, to last from one look to the next) */
	MemoryContext top = AllocSetContextCreate(TopMemoryContext, "capture launcher", ALLOCSET_DEFAULT_SIZES);
	MemoryContext look = AllocSetContextCreate(top, "capture launcher look", ALLOCSET_DEFAULT_SIZES);
	List	   *running = NIL;
	bool		none_free;
	bool		warned = false;

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	BackgroundWorkerUnblockSignals();
	BackgroundWorkerInitializeConnection(NULL, NULL, 0);

	if (!capture_possible())
		proc_exit(0);

	for (;;)
	{
		List	   *marked;
		ListCell   *lc;
		MemoryContext old;

		CHECK_FOR_INTERRUPTS();
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}
		MemoryContextReset(look);
		old = MemoryContextSwitchTo(look);
		marked = capture_in_background ? databases_marked() : NIL;
		drop_slots_not_of(marked);
		foreach(lc, running)
		{
			CaptureRunning *one = lfirst(lc);
			pid_t		pid;

			if (GetBackgroundWorkerPid(one->handle, &pid) == BGWH_STOPPED)
			{
				running = foreach_delete_current(running, lc);
				pfree(one->handle);
				pfree(one);
			}
		}
		none_free = false;
		foreach(lc, marked)
		{
			Oid			dboid = lfirst_oid(lc);
			ListCell   *r;
			bool		have = slot_is_held(dboid);
			BackgroundWorkerHandle *handle;

			foreach(r, running)
				have = have || ((CaptureRunning *) lfirst(r))->dboid == dboid;
			if (have)
				continue;
			MemoryContextSwitchTo(top);
			if (capture_launch(dboid, &handle))
			{
				CaptureRunning *one = palloc_object(CaptureRunning);

				one->dboid = dboid;
				one->handle = handle;
				running = lappend(running, one);
			}
			else
				none_free = true;
			MemoryContextSwitchTo(look);
		}

		/* (once, for as long as it lasts; the next look tries again) */
		if (none_free && !warned)
			ereport(WARNING,
					(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
					 errmsg("no background worker is free for capture, so a database with versioned schemas is not being captured"),
					 errhint("Each such database takes one.  You might need to increase \"max_worker_processes\".")));
		warned = none_free;
		MemoryContextSwitchTo(old);
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 CAPTURE_LAUNCHER_NAPTIME_MS, WAIT_EVENT_CAPTURE_LAUNCHER_MAIN);
		ResetLatch(MyLatch);
	}
}

void
CaptureLauncherRegister(void)
{
	BackgroundWorker bgw;

	if (IsBinaryUpgrade)
		return;
	memset(&bgw, 0, sizeof(bgw));
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	bgw.bgw_restart_time = 5;
	snprintf(bgw.bgw_library_name, MAXPGPATH, "postgres");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "CaptureLauncherMain");
	snprintf(bgw.bgw_name, BGW_MAXLEN, "capture launcher");
	snprintf(bgw.bgw_type, BGW_MAXLEN, "capture launcher");
	RegisterBackgroundWorker(&bgw);
}
