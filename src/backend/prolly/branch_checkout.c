/*-------------------------------------------------------------------------
 *
 * branch_checkout.c
 *	  A session on a branch, held for it.
 *
 * A checked-out branch is held in shared memory: each backend's slot says
 * which branch it's on as of its last commit, and which one a checkout in its
 * open transaction has taken.  (Not a session lock: an abort lets go of all
 * of those, and a user's can be let go of by pg_advisory_unlock_all.)  A
 * checkout takes the branch's lock, a database object lock of pg_database's
 * class on its number with sub-id 2 (its catalogs' lock has sub-id 1), in a
 * mode only a change taking the branch from its sessions conflicts with, and
 * notes it in its slot while holding that, until its transaction ends.  That
 * change asks for the lock conditionally, in a mode that conflicts, which it
 * holds to its transaction's end, and then reads the slots: a checkout that
 * got there first has noted it, and one that comes after waits for it.
 *
 * A checkout is undone with the transaction or subtransaction it's in, as
 * catalog_branch, which it sets, is: so what it took is noted with the
 * subtransaction's nesting level, and the slot's taken branch is the last of
 * those still standing; at the top transaction's commit it's the one held.
 *
 * Whether a transaction has written to a versioned schema is told by the
 * locks it holds: a write, or a change of an object, takes its relation's
 * lock in a mode above what a read or a row lock takes, and keeps it until
 * the transaction ends.  Only a transaction that has an id can have written.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/prolly/branch_checkout.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/twophase.h"
#include "access/xact.h"
#include "catalog/pg_database.h"
#include "miscadmin.h"
#include "prolly/branch_catalog.h"
#include "prolly/branch_checkout.h"
#include "prolly/branch_link.h"
#include "storage/lmgr.h"
#include "storage/ipc.h"
#include "storage/lock.h"
#include "storage/shmem.h"
#include "storage/proc.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

char	   *default_branch = NULL;

/* A backend's slot: its database, the branch it holds, and the one its transaction has taken, or 0. */
typedef struct CheckoutSlot
{
	pg_atomic_uint32 db;
	pg_atomic_uint32 held;
	pg_atomic_uint32 taken;
} CheckoutSlot;

static CheckoutSlot *slots = NULL;
static int	nslots = 0;

static void BranchCheckoutShmemRequest(void *arg);
static void BranchCheckoutShmemInit(void *arg);

const ShmemCallbacks BranchCheckoutShmemCallbacks = {
	.request_fn = BranchCheckoutShmemRequest,
	.init_fn = BranchCheckoutShmemInit,
};

static int
slot_count(void)
{
	/* (every process that may run a transaction has a number below it) */
	return MaxBackends + NUM_AUXILIARY_PROCS + max_prepared_xacts;
}

static void
BranchCheckoutShmemRequest(void *arg)
{
	nslots = slot_count();
	ShmemRequestStruct(.name = "Branch Checkouts",
					   .size = mul_size(nslots, sizeof(CheckoutSlot)),
					   .ptr = (void **) &slots,
		);
}

static void
BranchCheckoutShmemInit(void *arg)
{
	for (int i = 0; i < nslots; i++)
	{
		pg_atomic_init_u32(&slots[i].db, InvalidOid);
		pg_atomic_init_u32(&slots[i].held, 0);
		pg_atomic_init_u32(&slots[i].taken, 0);
	}
}

static CheckoutSlot *
my_slot(void)
{
	if (nslots == 0)
		nslots = slot_count();
	Assert(MyProcNumber >= 0 && MyProcNumber < nslots);
	return &slots[MyProcNumber];
}

/* What checkouts in the transaction took: each branch, and the nesting level it was taken at. */
typedef struct Taken
{
	int32		branch;
	int			level;
} Taken;

static Taken *taken = NULL;
static int	ntaken = 0;
static int	taken_cap = 0;
static bool callbacks_registered = false;

static void
hold_tag(LOCKTAG *tag, int32 branch)
{
	SET_LOCKTAG_OBJECT(*tag, MyDatabaseId, DatabaseRelationId, (Oid) branch, 2);
}

/* (the slot's taken branch: the last of those standing) */
static void
taken_note(void)
{
	pg_atomic_write_u32(&my_slot()->taken, ntaken > 0 ? (uint32) taken[ntaken - 1].branch : 0);
}

static void
checkout_xact_callback(XactEvent event, void *arg)
{
	CheckoutSlot *mine;

	if (ntaken == 0)
		return;
	mine = my_slot();
	switch (event)
	{
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_PREPARE:
			pg_atomic_write_u32(&mine->held, (uint32) taken[ntaken - 1].branch);
			ntaken = 0;
			taken_note();
			break;
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
			ntaken = 0;
			taken_note();
			break;
		default:
			break;
	}
}

static void
checkout_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
						  SubTransactionId parentSubid, void *arg)
{
	int			level = GetCurrentTransactionNestLevel();

	if (event != SUBXACT_EVENT_ABORT_SUB)
		return;
	while (ntaken > 0 && taken[ntaken - 1].level >= level)
		ntaken--;
	taken_note();
}

/* (a backend exiting is on no branch) */
static void
checkout_exit(int code, Datum arg)
{
	CheckoutSlot *mine = my_slot();

	pg_atomic_write_u32(&mine->taken, 0);
	pg_atomic_write_u32(&mine->held, 0);
	pg_atomic_write_u32(&mine->db, InvalidOid);
}

/* Whether the transaction holds a versioned relation's lock in a mode that writes or changes it. */
static bool
wrote_versioned(void)
{
	LockData   *locks;
	bool		wrote = false;
	LOCKMASK	reads = LOCKBIT_ON(AccessShareLock) | LOCKBIT_ON(RowShareLock);

	if (!TransactionIdIsValid(GetTopTransactionIdIfAny()))
		return false;
	locks = GetLockStatusData();
	for (int i = 0; i < locks->nelements && !wrote; i++)
	{
		LockInstanceData *l = &locks->locks[i];
		Oid			nsp;

		if (l->pid != MyProcPid || l->locktag.locktag_type != LOCKTAG_RELATION ||
			l->locktag.locktag_field1 != MyDatabaseId || (l->holdMask & ~reads) == 0)
			continue;
		nsp = get_rel_namespace((Oid) l->locktag.locktag_field2);
		wrote = OidIsValid(nsp) && NamespaceBranch(nsp) != 0;
	}
	return wrote;
}

static void
session_checkout(const char *name)
{
	int32		branch;
	char		value[12];
	LOCKTAG		tag;

	if (wrote_versioned())
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("cannot check out branch \"%s\" in a transaction that has changed a versioned schema", name),
				 errhint("Commit or roll back the transaction first.")));
	branch = BranchLink->branch_of(name);
	if (branch == 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("branch \"%s\" not found", name)));

	if (!callbacks_registered)
	{
		RegisterXactCallback(checkout_xact_callback, NULL);
		RegisterSubXactCallback(checkout_subxact_callback, NULL);
		before_shmem_exit(checkout_exit, (Datum) 0);
		pg_atomic_write_u32(&my_slot()->db, MyDatabaseId);
		callbacks_registered = true;
	}
	if (ntaken == taken_cap)
	{
		taken_cap = Max(4, taken_cap * 2);
		taken = taken == NULL ? MemoryContextAlloc(TopMemoryContext, sizeof(Taken) * taken_cap) :
			repalloc(taken, sizeof(Taken) * taken_cap);
	}

	/* (waiting, if the branch is being taken from its sessions, until that's done) */
	hold_tag(&tag, branch);
	(void) LockAcquire(&tag, AccessShareLock, false, false);
	taken[ntaken].branch = branch;
	taken[ntaken].level = GetCurrentTransactionNestLevel();
	ntaken++;
	taken_note();

	/* (gone meanwhile, or another's: as it is now) */
	AcceptInvalidationMessages();
	if (BranchLink->branch_of(name) != branch)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("branch \"%s\" not found", name)));

	snprintf(value, sizeof(value), "%d", branch);
	(void) set_config_option("catalog_branch", value, PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SET, true, 0, false);
}

static char *
session_current(void)
{
	return catalog_branch == 0 ? NULL : BranchLink->name_of(catalog_branch);
}

static bool
session_in_use(int32 branch)
{
	LOCKTAG		tag;

	/* (a checkout of it in a transaction still open holds its lock; one done is in its slot) */
	hold_tag(&tag, branch);
	if (LockAcquire(&tag, AccessExclusiveLock, false, true) == LOCKACQUIRE_NOT_AVAIL)
		return true;
	if (nslots == 0)
		nslots = slot_count();
	for (int i = 0; i < nslots; i++)
	{
		if (i == MyProcNumber || pg_atomic_read_u32(&slots[i].db) != MyDatabaseId)
			continue;
		if (pg_atomic_read_u32(&slots[i].held) == (uint32) branch ||
			pg_atomic_read_u32(&slots[i].taken) == (uint32) branch)
			return true;
	}
	return false;
}

const BranchCheckoutRoutine SessionBranchCheckout = {
	.name = "session",
	.checkout = session_checkout,
	.current = session_current,
	.in_use = session_in_use,
};

const BranchCheckoutRoutine *BranchCheckout = &SessionBranchCheckout;

void
BranchCheckoutSessionStart(void)
{
	if (default_branch != NULL && default_branch[0] != '\0')
		BranchCheckout->checkout(default_branch);
}
