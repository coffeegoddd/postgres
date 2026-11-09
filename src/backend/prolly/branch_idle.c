/*-------------------------------------------------------------------------
 *
 * branch_idle.c
 *	  Giving back the working sets of branches gone unused, a database at a
 *	  time.
 *
 * The launcher keeps when it last had each database looked at, and starts
 * the next worker, for the one looked at longest ago that's due, once the
 * last has stopped.  One it can't start, with none free, is started at a
 * later look.  The worker gives back each branch's working set in a
 * transaction of its own, rolled back if it's given nothing, and stops.  An
 * error giving one back is reported and rolled back, the branch noted used,
 * and the worker goes on to the next.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/prolly/branch_idle.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "prolly/branch_checkout.h"
#include "prolly/branch_idle.h"
#include "prolly/branch_link.h"
#include "prolly/branch_ops.h"
#include "prolly/chunkstore.h"
#include "storage/ipc.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

int			branch_idle_timeout = 0;

/* The longest a database waits between looks. */
#define BRANCH_IDLE_LOOK_MAX_MS 60000

/*
 * (how long a worker waits to hold off a branch's writers and for capture as it gives its working
 * set back: checkouts of it wait as long)
 */
#define BRANCH_IDLE_CAPTURE_TIMEOUT_MS 10000

typedef struct IdleLooked
{
	Oid			dboid;
	TimestampTz at;
} IdleLooked;

static MemoryContext idle_cxt = NULL;
static List *looked = NIL;
static BackgroundWorkerHandle *running = NULL;

static bool
idle_launch(Oid dboid, BackgroundWorkerHandle **handle)
{
	BackgroundWorker bgw;

	memset(&bgw, 0, sizeof(bgw));
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION | BGWORKER_INTERRUPTIBLE;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	bgw.bgw_restart_time = BGW_NEVER_RESTART;
	snprintf(bgw.bgw_library_name, MAXPGPATH, "postgres");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "BranchIdleWorkerMain");
	snprintf(bgw.bgw_name, BGW_MAXLEN, "branch idle for database %u", dboid);
	snprintf(bgw.bgw_type, BGW_MAXLEN, "branch idle worker");
	bgw.bgw_main_arg = ObjectIdGetDatum(dboid);
	return RegisterDynamicBackgroundWorker(&bgw, handle);
}

void
BranchIdleLook(List *dbs)
{
	TimestampTz now = GetCurrentTimestamp();
	int			every = Min(branch_idle_timeout, BRANCH_IDLE_LOOK_MAX_MS);
	IdleLooked *due = NULL;
	List	   *kept = NIL;
	MemoryContext old;
	pid_t		pid;

	if (running != NULL)
	{
		if (GetBackgroundWorkerPid(running, &pid) != BGWH_STOPPED)
			return;
		pfree(running);
		running = NULL;
	}
	if (idle_cxt == NULL)
		idle_cxt = AllocSetContextCreate(TopMemoryContext, "branch idle", ALLOCSET_SMALL_SIZES);
	old = MemoryContextSwitchTo(idle_cxt);

	/* (each database in the list kept, one not looked at yet as looked at never) */
	foreach_oid(dboid, dbs)
	{
		IdleLooked *one = NULL;

		foreach_ptr(IdleLooked, l, looked)
		{
			if (l->dboid == dboid)
				one = l;
		}
		if (one == NULL)
		{
			one = palloc_object(IdleLooked);
			one->dboid = dboid;
			one->at = 0;
		}
		kept = lappend(kept, one);
		if (branch_idle_timeout > 0 && TimestampDifferenceExceeds(one->at, now, every) &&
			(due == NULL || one->at < due->at))
			due = one;
	}
	foreach_ptr(IdleLooked, l, looked)
	{
		if (!list_member_ptr(kept, l))
			pfree(l);
	}
	list_free(looked);
	looked = kept;
	if (due != NULL && idle_launch(due->dboid, &running))
		due->at = now;
	MemoryContextSwitchTo(old);
}

void
BranchIdleWorkerMain(Datum main_arg)
{
	Oid			dboid = DatumGetObjectId(main_arg);
	MemoryContext cxt;
	List	   *names;

	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();
	BackgroundWorkerInitializeConnectionByOid(dboid, InvalidOid, BGWORKER_BYPASS_ALLOWCONN | BGWORKER_SKIP_SETTINGS);

	/* (on no branch, so on none it would give back; a superuser's SQL under no one else's path) */
	SetConfigOption("catalog_branch", "0", PGC_SUSET, PGC_S_OVERRIDE);
	SetConfigOption("search_path", "pg_catalog, pg_temp", PGC_SUSET, PGC_S_OVERRIDE);
	cxt = AllocSetContextCreate(TopMemoryContext, "branch idle worker", ALLOCSET_DEFAULT_SIZES);

	/* (turned off since it was started) */
	if (branch_idle_timeout <= 0)
		proc_exit(0);

	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	{
		MemoryContext old = MemoryContextSwitchTo(cxt);

		names = BranchLink->names();
		MemoryContextSwitchTo(old);
	}
	PopActiveSnapshot();
	CommitTransactionCommand();

	/* (the setting as it was when this started, as it's gone before a reload could matter) */
	foreach_ptr(char, name, names)
	{
		volatile int32 branch = 0;

		CHECK_FOR_INTERRUPTS();
		SetCurrentStatementStartTimestamp();
		StartTransactionCommand();
		PG_TRY();
		{
			ProllyCtx	ctx;
			bool		given;

			PushActiveSnapshot(GetTransactionSnapshot());
			branch = BranchLink->branch_of(name);
			prolly_ctx_init(&ctx, ChunkStoreOpen("pages"), &prolly_default_policy);
			given = BranchOps->dematerialize_idle(&ctx, name, branch_idle_timeout, BRANCH_IDLE_CAPTURE_TIMEOUT_MS);
			prolly_ctx_free(&ctx);
			PopActiveSnapshot();
			if (given)
			{
				CommitTransactionCommand();
				ereport(DEBUG1,
						(errmsg_internal("gave back the working set of branch \"%s\", unused for %d ms",
										 name, branch_idle_timeout)));
			}
			else
				AbortCurrentTransaction();
		}
		PG_CATCH();
		{
			/* (reported, and not looked at again until it has been unused as long once more) */
			HOLD_INTERRUPTS();
			MemoryContextSwitchTo(cxt);
			EmitErrorReport();
			AbortOutOfAnyTransaction();
			FlushErrorState();
			RESUME_INTERRUPTS();
			BranchCheckout->used(branch);
		}
		PG_END_TRY();
	}
	proc_exit(0);
}
