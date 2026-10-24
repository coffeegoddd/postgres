/*-------------------------------------------------------------------------
 *
 * prolly_folder.c
 *	  The fold worker: branches' working sets folded in the background,
 *	  soon after they're written, so a status, a diff or a commit folds
 *	  only what came since.
 *
 * Folds are lazy: a branch's changes are folded when something asks for
 * its working set, every row changed since its baseline then.  With
 * prolly_fold_worker on, a launcher (a background worker of its own, from
 * the end of recovery, connected to no database) wakes every
 * prolly_fold_worker_naptime and starts a worker in each database with a
 * branch written since a worker last saw it (the refs' moved_databases()),
 * at most prolly_fold_worker_max_workers at once.  A database that's gone,
 * or accepts no connections, is passed over until written again.  The
 * worker takes a working-set checkpoint of each such branch (the refs'
 * checkpoint_unless_moved(), the branch's fold its baseline from then on),
 * one branch to a transaction so the refs are held no longer than its
 * fold, and exits once no branch has moved for two passes, or after
 * FOLDER_MAX_PASSES passes (the launcher takes the databases written in
 * turn, so one written without pause doesn't keep a slot from others).  A branch the
 * refs are held for, that moved during its fold, or whose store's root
 * isn't free, is folded again after a backoff (FOLDER_MAX_BACKOFF).  Writers pay nothing: what a fold costs is the worker's, off their
 * transactions.  A worker's error is reported, and the branch passed over
 * until it is written again.  Off, the launcher sleeps until the setting
 * changes.
 *
 * With prolly_fold_worker_batch_rows set (it is 0, off, by default), a
 * branch is folded once its writes are due, not at every pass: once the
 * writers that ended since its last fold wrote
 * prolly_fold_worker_batch_rows rows, once the first of them ended
 * prolly_fold_worker_max_delay ago, or once its writes have stopped (none
 * writing it, and none ended for a naptime); and, having nothing to batch,
 * one no writer of which has ended since.  Each fold writes the tree
 * nodes on the paths to the rows it changes, so a fold of many writes
 * shares the upper nodes that folds of a few each write again: batching
 * them is what keeps a busy branch's chunk writes near its rows' own.  A
 * commit or a status folds what the worker hasn't yet, as ever: at most a
 * batch's rows, or what came in the last max_delay.  A branch not yet due
 * keeps the worker running, as a pending one does.
 *
 * With prolly_fold_worker_cost_delay set (0, off, by default), the
 * worker's folds are paced by the cost-based delay, as vacuum is: see
 * pace_begin() below.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_folder.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"
#include "access/prolly_config.h"
#include "access/prolly_folder.h"
#include "access/prolly_types.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "commands/vacuum.h"
#include "catalog/branch.h"
#include "catalog/branch_ops.h"
#include "catalog/pg_database.h"
#include "commands/dbcommands.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/snapmgr.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

bool		prolly_fold_worker = true;
int			prolly_fold_worker_max_workers = 2;
int			prolly_fold_worker_naptime = 200;
int			prolly_fold_worker_batch_rows = 0;
int			prolly_fold_worker_max_delay = 1000;
double		prolly_fold_worker_cost_delay = 0;
int			prolly_fold_worker_cost_limit = -1;
bool		prolly_fold_paced = false;

/* The most databases, and branches of one, a pass looks at. */
#define FOLDER_MAX_DATABASES	64
#define FOLDER_MAX_BRANCHES		256

/*
 * A worker exits after this many passes, the launcher starting one again
 * if its database is still written, after the others waiting for a slot
 * (it takes them in turn): a busy database never keeps a slot for good.
 */
#define FOLDER_MAX_PASSES		64

/*
 * A branch whose fold was given up (the refs held, the branch moved
 * meanwhile, or the store's root not free) is tried again after 2^k - 1
 * passes for its k-th time in a row, k at most this: so a transaction
 * holding the refs long is asked of less and less, every 63 passes at the
 * most.
 */
#define FOLDER_MAX_BACKOFF		6

/* The launcher, once a server, from the end of recovery. */
void
ProllyFolderRegister(void)
{
	BackgroundWorker bgw;

	if (IsBinaryUpgrade)
		return;
	memset(&bgw, 0, sizeof(bgw));
	/* (connected to no database, to read pg_database) */
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	snprintf(bgw.bgw_library_name, MAXPGPATH, "postgres");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "ProllyFolderLauncherMain");
	snprintf(bgw.bgw_name, BGW_MAXLEN, "prolly fold launcher");
	snprintf(bgw.bgw_type, BGW_MAXLEN, "prolly fold launcher");
	bgw.bgw_restart_time = 5;
	RegisterBackgroundWorker(&bgw);
}

/* A worker for database dboid; false if none could be started. */
static bool
start_worker(Oid dboid, BackgroundWorkerHandle **handle)
{
	BackgroundWorker bgw;

	memset(&bgw, 0, sizeof(bgw));

	/*
	 * (it gives way as autovacuum does: a CREATE DATABASE copying its
	 * database, or an ALTER or DROP of it, ends it rather than waiting for
	 * it, or failing because it's connected; its fold rolled back, the
	 * branch stays to be folded)
	 */
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION | BGWORKER_INTERRUPTIBLE;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	snprintf(bgw.bgw_library_name, MAXPGPATH, "postgres");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "ProllyFolderWorkerMain");
	snprintf(bgw.bgw_name, BGW_MAXLEN, "prolly fold worker for database %u",
			 dboid);
	snprintf(bgw.bgw_type, BGW_MAXLEN, "prolly fold worker");
	bgw.bgw_restart_time = BGW_NEVER_RESTART;
	bgw.bgw_notify_pid = MyProcPid;
	bgw.bgw_main_arg = ObjectIdGetDatum(dboid);
	return RegisterDynamicBackgroundWorker(&bgw, handle);
}

typedef struct Running
{
	Oid			dboid;
	BackgroundWorkerHandle *handle;
} Running;

/*
 * Can a worker connect to database dboid?  Not if it's gone (dropped
 * since its branches were written), being dropped, or accepts no
 * connections: one would fail as it starts, every naptime.
 */
static bool
database_usable(Oid dboid)
{
	HeapTuple	tup;
	bool		usable = false;

	StartTransactionCommand();
	tup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(dboid));
	if (HeapTupleIsValid(tup))
	{
		Form_pg_database db = (Form_pg_database) GETSTRUCT(tup);

		usable = db->datallowconn && !database_is_invalid_form(db);
		ReleaseSysCache(tup);
	}
	CommitTransactionCommand();
	return usable;
}

void
ProllyFolderLauncherMain(Datum main_arg)
{
	Running		running[FOLDER_MAX_DATABASES];
	int			nrunning = 0;
	int			turn = 0;		/* the moved database to start from */

	ProllyDatabaseComponents dc;

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();
	BackgroundWorkerInitializeConnection(NULL, NULL, 0);
	prolly_database_components(&dc);

	for (;;)
	{
		Oid			databases[FOLDER_MAX_DATABASES];
		int			n;
		int			kept = 0;

		/* (off: asleep till the setting changes, a reload setting the latch) */
		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_EXIT_ON_PM_DEATH |
						 (prolly_fold_worker ? WL_TIMEOUT : 0),
						 prolly_fold_worker_naptime, WAIT_EVENT_PG_SLEEP);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/* (those whose worker exited let go of) */
		for (int i = 0; i < nrunning; i++)
		{
			pid_t		pid;

			if (GetBackgroundWorkerPid(running[i].handle, &pid) == BGWH_STOPPED)
				pfree(running[i].handle);
			else
				running[kept++] = running[i];
		}
		nrunning = kept;
		if (!prolly_fold_worker || !PROLLY_HAS(dc.refs, moved_databases))
			continue;

		/* (in turn, a pass starting from the next: none waits for good) */
		n = dc.refs->moved_databases(databases, lengthof(databases));
		turn = n > 0 ? (turn + 1) % n : 0;
		for (int k = 0; k < n && nrunning < prolly_fold_worker_max_workers; k++)
		{
			int			i = (turn + k) % n;
			bool		busy = false;
			BackgroundWorkerHandle *handle;

			for (int j = 0; j < nrunning && !busy; j++)
				busy = running[j].dboid == databases[i];
			if (busy)
				continue;
			if (!database_usable(databases[i]))
			{
				dc.refs->saw_database(databases[i]);
				continue;
			}
			if (!start_worker(databases[i], &handle))
				continue;
			running[nrunning].dboid = databases[i];
			running[nrunning].handle = handle;
			nrunning++;
		}
	}
}

/*
 * (run on a branch: its checkpoint, folded before the refs are taken, so a
 * commit, a reset or a merge waits for no fold of the worker's: if the
 * branch moved meanwhile, *folded is false and the transaction is rolled
 * back, to fold again next pass.  Whether its logs hold entries isn't asked
 * first: their estimate samples pages, and misses a few entries on a log
 * of many pages; a checkpoint with nothing to fold reads its logs by their
 * index, and consumes nothing, so moves no generation again.)
 */
static void
fold_branch(void *arg)
{
	bool	   *folded = arg;
	ProllyDatabaseComponents dc;
	ProllyNodeRef root;

	INJECTION_POINT("prolly-fold-worker-fold", NULL);
	prolly_database_components(&dc);
	if (PROLLY_HAS(dc.refs, checkpoint_unless_moved))
		*folded = dc.refs->checkpoint_unless_moved(&root);
	else
	{
		PROLLY_CALL(dc.refs, checkpoint, &root);
		*folded = true;
	}
}

/*
 * Pacing.  A fold of the worker's, with prolly_fold_worker_cost_delay set,
 * runs under the cost-based delay as a vacuum does: the buffer manager
 * charges the pages it reads and dirties (the change logs', the tables',
 * their indexes'), and prolly_fold_delay_point() the tree nodes it reads
 * (as hit pages: decoded nodes are cached, and the store's files are read
 * through the kernel's cache mostly) and writes (as dirtied pages: each is
 * appended, WAL-logged and synced), then naps once the fold has spent
 * prolly_fold_worker_cost_limit.  Off, nothing is charged, nor any delay
 * point reached: a fold costs what it did.
 */
static uint64 paced_nodes_read;
static uint64 paced_nodes_written;

static void
paced_nodes_seen(void)
{
	ProllyNodeCounters *c = pgstat_prolly_tree_counters();

	paced_nodes_read = c->nodes_read;
	paced_nodes_written = c->nodes_written;
}

static void
pace_begin(void)
{
	if (prolly_fold_worker_cost_delay <= 0)
		return;
	vacuum_cost_delay = prolly_fold_worker_cost_delay;
	vacuum_cost_limit = prolly_fold_worker_cost_limit > 0 ?
		prolly_fold_worker_cost_limit : VacuumCostLimit;
	VacuumCostBalance = 0;
	VacuumCostActive = true;
	paced_nodes_seen();
	prolly_fold_paced = true;
}

static void
pace_end(void)
{
	if (!prolly_fold_paced)
		return;
	prolly_fold_paced = false;
	VacuumCostActive = false;
	VacuumCostBalance = 0;
}

void
prolly_fold_delay_point_paced(void)
{
	ProllyNodeCounters *c = pgstat_prolly_tree_counters();

	/* (the counts move to the statistics when they're flushed) */
	if (c->nodes_read < paced_nodes_read ||
		c->nodes_written < paced_nodes_written)
		paced_nodes_read = paced_nodes_written = 0;
	VacuumCostBalance += (int) ((c->nodes_read - paced_nodes_read) *
								VacuumCostPageHit +
								(c->nodes_written - paced_nodes_written) *
								VacuumCostPageDirty);
	paced_nodes_seen();
	if (VacuumCostActive && VacuumCostBalance >= vacuum_cost_limit &&
		INTERRUPTS_CAN_BE_PROCESSED())
		pgstat_count_prolly(PGSTAT_PROLLY_FOLD_WORKER_COST_DELAYS, 1);
	vacuum_delay_point(false);
}

typedef struct Moved
{
	ProllyMovedBranch *branches;
	int			n;
	char	  **names;			/* found by the visit, NULL: none */
} Moved;

static void
name_moved(const char *name, Oid branch, void *arg)
{
	Moved	   *m = arg;

	/* (kept past the transaction that read them) */
	for (int i = 0; i < m->n; i++)
		if (m->branches[i].branch == branch)
			m->names[i] = MemoryContextStrdup(TopMemoryContext, name);
}

/*
 * Is a fold of b due at now: has a batch of rows been written since the
 * last, has the first of those writes waited max_delay, or have its writes
 * stopped?  (A batch of 0 rows: every pass.)  One no writer of which has
 * ended since (its first writer still open, say) has nothing to batch: due,
 * so it's folded and seen as it is, and the worker can exit while that
 * writer stays open.
 */
static bool
fold_due(const ProllyMovedBranch *b, TimestampTz now)
{
	if (prolly_fold_worker_batch_rows <= 0 || b->since == 0 ||
		b->rows >= (uint64) prolly_fold_worker_batch_rows)
		return true;
	if (TimestampDifferenceExceeds(b->since, now, prolly_fold_worker_max_delay))
		return true;
	return b->writers == 0 &&
		TimestampDifferenceExceeds(b->last_ended, now,
								   prolly_fold_worker_naptime);
}

/*
 * The branches whose folds were given up, in a row: how many times, and how
 * many passes are left before the next try.
 */
typedef struct Backoff
{
	Oid			branch;
	int			tries;
	int			wait;
} Backoff;

static Backoff backoffs[FOLDER_MAX_BRANCHES];
static int	nbackoffs = 0;

static Backoff *
backoff_of(Oid branch)
{
	for (int i = 0; i < nbackoffs; i++)
		if (backoffs[i].branch == branch)
			return &backoffs[i];
	return NULL;
}

/* A fold of branch given up again: waited for 2^tries - 1 passes. */
static void
back_off(Oid branch)
{
	Backoff    *b = backoff_of(branch);

	if (b == NULL)
	{
		if (nbackoffs == lengthof(backoffs))
			return;				/* (tried every pass, as before) */
		b = &backoffs[nbackoffs++];
		b->branch = branch;
		b->tries = 0;
	}
	b->tries = Min(b->tries + 1, FOLDER_MAX_BACKOFF);
	b->wait = (1 << b->tries) - 1;
}

/* branch folded, or passed over: no longer backed off. */
static void
backed_off_done(Oid branch)
{
	Backoff    *b = backoff_of(branch);

	if (b != NULL)
		*b = backoffs[--nbackoffs];
}

/* What a pass did. */
typedef enum PassResult
{
	PASS_IDLE,					/* no branch moved */
	PASS_FOLDED,				/* one folded, at least */
	PASS_PENDING,				/* some moved, none folded: again next pass */
} PassResult;

/*
 * One pass over the database's branches written since the last: each
 * folded in a transaction of its own.
 */
static PassResult
fold_pass(const ProllyRefsRoutine *refs)
{
	ProllyMovedBranch moved[FOLDER_MAX_BRANCHES];
	char	   *names[FOLDER_MAX_BRANCHES];
	Moved		m = {moved, 0, names};
	bool		any = false;
	bool		pending = false;
	TimestampTz now;
	int			ndue = 0;

	m.n = refs->moved_branches(moved, lengthof(moved));
	if (m.n == 0)
		return PASS_IDLE;

	/* (those not due yet left as moved, for a later pass) */
	now = GetCurrentTimestamp();
	for (int i = 0; i < m.n; i++)
	{
		Backoff    *b = backoff_of(moved[i].branch);

		if (b != NULL && b->wait > 0)
		{
			/* (given up last time: tried again once its wait is over) */
			b->wait--;
			pending = true;
		}
		else if (fold_due(&moved[i], now))
			moved[ndue++] = moved[i];
		else
		{
			pgstat_count_prolly(PGSTAT_PROLLY_FOLD_WORKER_DEFERRED, 1);
			elog(DEBUG1, "fold worker left branch %u for a later pass: "
				 UINT64_FORMAT " rows written since its last fold",
				 moved[i].branch, moved[i].rows);
		}
	}
	if (ndue < m.n)
	{
		/* (counted outside a transaction: flushed with the pass's stats) */
		AtEOXact_PgStat_Prolly();
		pending = true;
	}
	m.n = ndue;
	if (m.n == 0)
		return PASS_PENDING;
	memset(names, 0, sizeof(names));
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	branch_ops()->branches(name_moved, &m);
	PopActiveSnapshot();
	CommitTransactionCommand();

	for (int i = 0; i < m.n; i++)
	{
		bool		folded = false;

		if (names[i] == NULL && moved[i].branch != MyBranch)
		{
			refs->saw_branch(moved[i].branch, moved[i].generation);
			backed_off_done(moved[i].branch);
			continue;
		}
		StartTransactionCommand();
		PG_TRY();
		{
			PushActiveSnapshot(GetTransactionSnapshot());
			pace_begin();
			/* (the worker's own branch, the database's default, as it is) */
			if (moved[i].branch == MyBranch)
				fold_branch(&folded);
			else
				branch_ops()->run_on(names[i], fold_branch, &folded);
			pace_end();
			PopActiveSnapshot();
			if (folded)
			{
				CommitTransactionCommand();

				/*
				 * (seen as of before its fold read: a write since is seen
				 * again next pass; one not folded stays to be folded)
				 */
				refs->saw_branch(moved[i].branch, moved[i].generation);
				backed_off_done(moved[i].branch);
				pgstat_count_prolly(PGSTAT_PROLLY_FOLD_WORKER_FOLDS, 1);
				elog(DEBUG1, "fold worker folded branch %u at generation %llu",
					 moved[i].branch, (unsigned long long) moved[i].generation);
			}
			else
			{
				/*
				 * (moved meanwhile, the refs held by another, or the store's
				 * root not free: what this fold consumed given back, the
				 * branch not seen, so it's folded again once its backoff is
				 * over)
				 */
				AbortCurrentTransaction();
				pending = true;
				back_off(moved[i].branch);
				/* (counted outside a transaction: flushed with the pass's) */
				pgstat_count_prolly(PGSTAT_PROLLY_FOLD_WORKER_RETRIES, 1);
				AtEOXact_PgStat_Prolly();
				elog(DEBUG1, "fold worker left branch %u for its next pass: "
					 "the refs held, or the branch moved", moved[i].branch);
			}
		}
		PG_CATCH();
		{
			pace_end();
			HOLD_INTERRUPTS();
			EmitErrorReport();
			AbortOutOfAnyTransaction();
			FlushErrorState();
			RESUME_INTERRUPTS();

			/*
			 * (passed over until written again: an error that recurs isn't
			 * met again every pass)
			 */
			refs->saw_branch(moved[i].branch, moved[i].generation);
			backed_off_done(moved[i].branch);
		}
		PG_END_TRY();
		any = any || folded;
	}
	for (int i = 0; i < m.n; i++)
		if (names[i] != NULL)
			pfree(names[i]);
	return any ? PASS_FOLDED : pending ? PASS_PENDING : PASS_IDLE;
}

void
ProllyFolderWorkerMain(Datum main_arg)
{
	Oid			dboid = DatumGetObjectId(main_arg);
	int			idle = 0;
	int			passes = 0;
	ProllyDatabaseComponents dc;

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();
	BackgroundWorkerInitializeConnectionByOid(dboid, InvalidOid, 0);
	SetConfigOption("application_name", "prolly fold worker", PGC_USERSET,
					PGC_S_SESSION);
	prolly_database_components(&dc);
	if (!PROLLY_HAS(dc.refs, moved_branches))
		proc_exit(0);

	/*
	 * (until two passes in a row find no branch moved, one left pending, as
	 * for refs held, keeping it, asking again each naptime; or for at most
	 * FOLDER_MAX_PASSES passes)
	 */
	while (idle < 2 && passes++ < FOLDER_MAX_PASSES)
	{
		PassResult	r;

		CHECK_FOR_INTERRUPTS();
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}
		if (!prolly_fold_worker)
			break;
		r = fold_pass(dc.refs);
		idle = r == PASS_IDLE ? idle + 1 : 0;
		pgstat_report_stat(true);
		if (idle < 2)
			(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
							 prolly_fold_worker_naptime, WAIT_EVENT_PG_SLEEP);
		ResetLatch(MyLatch);
	}
	proc_exit(0);
}
