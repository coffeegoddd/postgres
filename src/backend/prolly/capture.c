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
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/wait_event.h"

#define CAPTURE_SLOT_PREFIX "prolly_capture_"

/* How often the launcher looks, and how often a worker looks for what is new. */
#define CAPTURE_LAUNCHER_NAPTIME_MS 5000
#define CAPTURE_WORKER_NAPTIME_MS 1000

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
capture_launch(Oid dboid)
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
	return RegisterDynamicBackgroundWorker(&bgw, NULL);
}

static bool schema_made = false;

static void
capture_at_xact_end(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_COMMIT && schema_made && capture_in_background && capture_possible() &&
		!IsBinaryUpgrade)
		(void) capture_launch(MyDatabaseId);
	if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_ABORT ||
		event == XACT_EVENT_PARALLEL_COMMIT || event == XACT_EVENT_PARALLEL_ABORT ||
		event == XACT_EVENT_PREPARE)
		schema_made = false;
}

void
CaptureSchemaMade(void)
{
	static bool registered = false;

	if (!registered)
	{
		RegisterXactCallback(capture_at_xact_end, NULL);
		registered = true;
	}
	schema_made = true;
}

/* ---------------------------------------------------------------- the worker */

/* Whether this database has a versioned schema, on any branch. */
static bool
database_has_versioned_schema(void)
{
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	bool		found = false;

	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	rel = table_open(NamespaceRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);
	while (!found && (tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
		found = ((Form_pg_namespace) GETSTRUCT(tup))->nspbranch != 0;
	table_endscan(scan);
	table_close(rel, AccessShareLock);
	PopActiveSnapshot();
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

void
CaptureWorkerMain(Datum main_arg)
{
	Oid			dboid = DatumGetObjectId(main_arg);
	char		name[NAMEDATALEN];

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();
	BackgroundWorkerInitializeConnectionByOid(dboid, InvalidOid, 0);

	/* (this sees every branch's schemas, not those of the branch it isn't on) */
	SetConfigOption("catalog_branch_filter", "off", PGC_SUSET, PGC_S_OVERRIDE);
	slot_name_of(dboid, name);

	if (!capture_possible())
		proc_exit(0);
	if (!database_has_versioned_schema())
	{
		ReplicationSlot *slot;

		LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
		slot = SearchNamedReplicationSlot(name, false);
		LWLockRelease(ReplicationSlotControlLock);
		if (slot != NULL)
			ReplicationSlotDrop(name, true);
		proc_exit(0);
	}

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	{
		ReplicationSlot *slot = SearchNamedReplicationSlot(name, false);
		bool		exists = slot != NULL;
		bool		held = exists && slot->active_proc != INVALID_PROC_NUMBER;

		LWLockRelease(ReplicationSlotControlLock);

		/* (it has its worker) */
		if (held)
			proc_exit(0);
		if (!exists)
			capture_slot_create(name);
	}
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
		if (!database_has_versioned_schema())
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
	ReplicationSlot *slot;

	slot_name_of(dboid, name);
	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	slot = SearchNamedReplicationSlot(name, false);
	LWLockRelease(ReplicationSlotControlLock);
	if (slot == NULL)
		return;
	TerminateBackgroundWorkersForDatabase(dboid);
	ReplicationSlotDrop(name, false);
}

/* -------------------------------------------------------------- the launcher */

/* The databases that can be connected to, to look in each once. */
static List *
databases_to_look_in(void)
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

		if (db->datallowconn && !db->datistemplate)
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

/* This server's capture slots: all of them, or those with no worker. */
static List *
slots_to_see_to(bool drop)
{
	List	   *found = NIL;

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (int i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];
		const char *name = NameStr(s->data.name);

		if (s->in_use && s->active_proc == INVALID_PROC_NUMBER &&
			strncmp(name, CAPTURE_SLOT_PREFIX, strlen(CAPTURE_SLOT_PREFIX)) == 0)
			found = lappend(found, drop ? (void *) pstrdup(name) : (void *) (uintptr_t) s->data.database);
	}
	LWLockRelease(ReplicationSlotControlLock);
	return found;
}

void
CaptureLauncherMain(Datum main_arg)
{
	List	   *to_look_in;

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	BackgroundWorkerUnblockSignals();
	BackgroundWorkerInitializeConnection(NULL, NULL, 0);

	if (!capture_possible())
		proc_exit(0);
	to_look_in = databases_to_look_in();

	for (;;)
	{
		ListCell   *lc;

		CHECK_FOR_INTERRUPTS();
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}
		if (!capture_in_background)
		{
			foreach(lc, slots_to_see_to(true))
				ReplicationSlotDrop(lfirst(lc), true);
		}
		else
		{
			/* (with none free to start one, the next look tries again) */
			while (to_look_in != NIL && capture_launch(linitial_oid(to_look_in)))
				to_look_in = list_delete_first(to_look_in);
			foreach(lc, slots_to_see_to(false))
				(void) capture_launch(ObjectIdGetDatum((Oid) (uintptr_t) lfirst(lc)));
		}
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
