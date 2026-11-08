/*-------------------------------------------------------------------------
 *
 * branch_checkout.c
 *	  A session on a branch, held for it.
 *
 * Which branch each backend is on is in shared memory: its slot has the
 * catalog branch catalog_branch now names, written as the setting changes,
 * and the one the session would be on were its transaction to abort: what it
 * was as the transaction began, which only a commit or prepare moves on.
 * Whatever else a transaction may yet return to, by a savepoint rolled back or
 * a function's setting undone, was set in it: by a checkout, which holds that
 * branch's lock until the transaction ends, or by setting catalog_branch
 * itself, which holds nothing, as below.  A checkout takes the branch's
 * lock, a database object lock of pg_database's class on its number with
 * sub-id 2 (its catalogs' lock has sub-id 1), in a mode only a change taking
 * the branch from its sessions conflicts with, and sets catalog_branch while
 * holding it.  That change asks for the lock conditionally, in a mode that
 * conflicts, which it holds to its transaction's end, and then reads the
 * slots: a checkout in a transaction still open holds the lock, one done is in
 * its slot, and one that comes after waits.  A session that sets
 * catalog_branch itself, directly or in a function's SET clause, takes no
 * lock, so it may land on a branch being taken, or return to one taken
 * meanwhile, and then finds it gone, as it would a schema dropped under it.
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
#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "miscadmin.h"
#include "prolly/branch_catalog.h"
#include "prolly/branch_checkout.h"
#include "prolly/branch_link.h"
#include "prolly/branch_ops.h"
#include "prolly/chunkstore.h"
#include "storage/lmgr.h"
#include "storage/ipc.h"
#include "storage/lock.h"
#include "storage/shmem.h"
#include "storage/proc.h"
#include "utils/guc.h"
#include "utils/guc_tables.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"

char	   *default_branch = NULL;

/* (how long a checkout waits for capture as it gives a branch its working set) */
#define BRANCH_CHECKOUT_TIMEOUT_MS 60000

/*
 * A backend's slot: its database, the catalog branch it's on, and the one it was on as its
 * transaction began, or 0.
 */
typedef struct CheckoutSlot
{
	pg_atomic_uint32 db;
	pg_atomic_uint32 branch;
	pg_atomic_uint32 base;
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
		pg_atomic_init_u32(&slots[i].branch, 0);
		pg_atomic_init_u32(&slots[i].base, 0);
	}
}

static void
hold_tag(LOCKTAG *tag, int32 branch)
{
	SET_LOCKTAG_OBJECT(*tag, MyDatabaseId, DatabaseRelationId, (Oid) branch, 2);
}

static bool exited = false;

static CheckoutSlot *
my_slot(void)
{
	if (slots == NULL || MyProcNumber < 0 || exited)
		return NULL;
	if (nslots == 0)
		nslots = slot_count();
	return MyProcNumber < nslots ? &slots[MyProcNumber] : NULL;
}

/*
 * (as the setting changes: anywhere, an abort too, so only what can't fail.  Outside a
 * transaction, or as the top one ends and what it set only for itself is undone, it's what the
 * next begins on too.)
 */
void
BranchCheckoutNote(int32 branch)
{
	CheckoutSlot *mine = my_slot();

	if (mine == NULL)
		return;
	pg_atomic_write_u32(&mine->db, MyDatabaseId);
	pg_atomic_write_u32(&mine->branch, (uint32) branch);
	if (GetCurrentTransactionNestLevel() <= 1 && !IsTransactionState())
		pg_atomic_write_u32(&mine->base, (uint32) branch);
}

/*
 * The catalog branch the session is on once its top transaction commits: what catalog_branch has
 * now, but for what only the transaction set (SET LOCAL, or a SET then a SET LOCAL), which its
 * commit undoes after its locks are let go of.
 */
static int32
branch_after_commit(void)
{
	struct config_generic *conf = find_option("catalog_branch", false, true, ERROR);
	GucStack   *st = conf != NULL ? conf->stack : NULL;

	if (st == NULL)
		return catalog_branch;
	switch (st->state)
	{
		case GUC_SAVE:
		case GUC_LOCAL:
			return st->prior.val.intval;
		case GUC_SET_LOCAL:
			return st->masked.val.intval;
		case GUC_SET:
			return catalog_branch;
	}
	return catalog_branch;
}

/* (at a commit or prepare, where it'll be is what it would return to) */
static void
checkout_xact_callback(XactEvent event, void *arg)
{
	CheckoutSlot *mine = my_slot();

	if (mine != NULL && (event == XACT_EVENT_COMMIT || event == XACT_EVENT_PARALLEL_COMMIT ||
						 event == XACT_EVENT_PREPARE))
		pg_atomic_write_u32(&mine->base, (uint32) branch_after_commit());
}

/* (a backend exiting is on no branch, whatever its abort, which comes after, sets) */
static void
checkout_exit(int code, Datum arg)
{
	CheckoutSlot *mine = my_slot();

	if (mine == NULL)
		return;
	pg_atomic_write_u32(&mine->branch, 0);
	pg_atomic_write_u32(&mine->base, 0);
	pg_atomic_write_u32(&mine->db, InvalidOid);
	exited = true;
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

		if (l->pid != MyProcPid || l->locktag.locktag_field1 != MyDatabaseId)
			continue;

		/* (a branch's catalogs locked: one of its objects made, changed or dropped) */
		if (l->locktag.locktag_type == LOCKTAG_OBJECT &&
			l->locktag.locktag_field2 == DatabaseRelationId && l->locktag.locktag_field4 == 1)
		{
			wrote = true;
			continue;
		}
		if (l->locktag.locktag_type != LOCKTAG_RELATION || (l->holdMask & ~reads) == 0)
			continue;

		/* (one dropped in the transaction, its schema gone with it, may have been a branch's) */
		nsp = get_rel_namespace((Oid) l->locktag.locktag_field2);
		wrote = !OidIsValid(nsp) || NamespaceBranch(nsp) != 0;
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

	/*
	 * (one that's only a head given a working set first, at its head; one not there refused.  It's
	 * made in a transaction left out of capture, so not in a block with what else would be left out
	 * too.  It's the engine's work, whoever checks out, as making a branch's catalog rows and
	 * history needs what only a superuser has)
	 */
	if (branch == 0)
	{
		ProllyCtx	ctx;
		Oid			save_userid;
		int			save_sec_context;

		if (IsTransactionBlock())
			ereport(ERROR,
					(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
					 errmsg("branch \"%s\" has no working set, and can't be given one in a transaction block", name),
					 errhint("Check it out outside a transaction block.")));
		GetUserIdAndSecContext(&save_userid, &save_sec_context);
		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID,
							   save_sec_context | SECURITY_LOCAL_USERID_CHANGE | SECURITY_RESTRICTED_OPERATION);
		prolly_ctx_init(&ctx, ChunkStoreOpen("pages"), &prolly_default_policy);
		branch = BranchOps->materialize(&ctx, name, BRANCH_CHECKOUT_TIMEOUT_MS);
		prolly_ctx_free(&ctx);
		SetUserIdAndSecContext(save_userid, save_sec_context);
	}

	/* (waiting, if the branch is being taken from its sessions, until that's done) */
	hold_tag(&tag, branch);
	(void) LockAcquire(&tag, AccessShareLock, false, false);

	/* (gone meanwhile, or given back, or another's: as it is now) */
	AcceptInvalidationMessages();
	if (BranchLink->branch_of(name) != branch)
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("branch \"%s\" changed as it was being checked out", name),
				 errhint("Try again.")));

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
		if (i != MyProcNumber && pg_atomic_read_u32(&slots[i].db) == MyDatabaseId &&
			(pg_atomic_read_u32(&slots[i].branch) == (uint32) branch ||
			 pg_atomic_read_u32(&slots[i].base) == (uint32) branch))
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
BranchCheckoutBackendStart(bool client)
{
	CheckoutSlot *mine = my_slot();

	before_shmem_exit(checkout_exit, (Datum) 0);
	RegisterXactCallback(checkout_xact_callback, NULL);
	BranchCheckoutNote(catalog_branch);
	if (mine != NULL)
		pg_atomic_write_u32(&mine->base, (uint32) catalog_branch);
	/* (in the session's first transaction, which has no snapshot of its own yet) */
	if (client && default_branch != NULL && default_branch[0] != '\0')
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		BranchCheckout->checkout(default_branch);
		PopActiveSnapshot();
	}
}
