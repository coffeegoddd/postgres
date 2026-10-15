/*-------------------------------------------------------------------------
 *
 * coord_k1.c
 *	  K1 implementation of the coordination interface.
 *
 * K1: row locks in a separate lock table, the Postgres lock manager's.  A
 * row is locked by a heavyweight lock on (database, table, hash of its
 * key) (LOCKTAG_PROLLY_ROW), in the lock mode heap uses for a tuple lock of
 * the same strength, so the four row lock modes conflict as on heap:
 *
 *	FOR KEY SHARE		AccessShareLock
 *	FOR SHARE			RowShareLock
 *	FOR NO KEY UPDATE	ExclusiveLock
 *	FOR UPDATE			AccessExclusiveLock
 *
 * Rows are rows of a branch: the branch salts the key's hash, and is the
 * table lock's third field, so writers on different branches never wait for
 * each other.
 *
 * Locks are held until the end of the (sub)transaction that took them, by
 * the lock manager, which also finds deadlocks; prepared transactions keep
 * theirs.  Two keys whose hashes agree in the tag's 48 bits share a lock: a
 * writer may wait needlessly, never write a row another holds.
 *
 * Whole tables.  Writing a whole tree (a NULL key: rebuilding it) takes
 * the table's own lock (LOCKTAG_PROLLY_TABLE) in ShareLock for share modes
 * and ExclusiveLock otherwise, as coordination "single_writer" does; every
 * row lock first takes that lock in an intent mode (RowShareLock for share
 * modes, RowExclusiveLock otherwise), which the whole-table modes conflict
 * with, so a whole-table writer waits for row lockers and keeps them out.
 *
 * Escalation.  A heavyweight lock costs shared memory, which is bounded, so
 * past prolly_max_row_locks_per_table row locks on one table in one
 * transaction, the transaction locks the whole table in the mode it needs
 * instead: like heap's, its later writes no longer take one lock each, but
 * they keep other writers out of the table until it ends.
 *
 * Writers.  writer_of() finds a transaction that holds a write lock on a
 * key (or on any row of the table), for a uniqueness check to wait for, as
 * heap reports a tuple's inserter or deleter.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/coord_k1.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/twophase.h"

#include "access/prolly_builtins.h"
#include "access/prolly_xact.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"

int			prolly_max_row_locks_per_table = 64;

/* Row locks this transaction took, per table, for escalation. */
typedef struct K1Count
{
	Oid			relid;
	LocalTransactionId lxid;
	int64		nlocks;
} K1Count;

static HTAB *counts = NULL;

static bool
is_share(LockTupleMode mode)
{
	return mode == LockTupleKeyShare || mode == LockTupleShare;
}

static LOCKMODE
row_mode(LockTupleMode mode)
{
	switch (mode)
	{
		case LockTupleKeyShare:
			return AccessShareLock;
		case LockTupleShare:
			return RowShareLock;
		case LockTupleNoKeyExclusive:
			return ExclusiveLock;
		case LockTupleExclusive:
			return AccessExclusiveLock;
	}
	elog(ERROR, "unrecognized row lock mode %d", (int) mode);
	return NoLock;				/* keep compiler quiet */
}

static uint64
key_hash(const ProllyKey *key)
{
	return hash_bytes_extended(key->data, key->len, prolly_session_branch());
}

static void
table_tag(LOCKTAG *tag, Relation tree)
{
	SET_LOCKTAG_PROLLY_TABLE(*tag, MyDatabaseId, RelationGetRelid(tree));
	tag->locktag_field3 = prolly_session_branch();
}

static void
row_tag(LOCKTAG *tag, Relation tree, const ProllyKey *key)
{
	SET_LOCKTAG_PROLLY_ROW(*tag, MyDatabaseId, RelationGetRelid(tree),
						   key_hash(key));
}

/* The table's row locks taken in this transaction, counting this one. */
static int64
count_lock(Relation tree)
{
	Oid			relid = RelationGetRelid(tree);
	K1Count    *c;
	bool		found;

	if (counts == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(K1Count);
		ctl.hcxt = TopMemoryContext;
		counts = hash_create("prolly k1 row lock counts", 16, &ctl,
							 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	c = hash_search(counts, &relid, HASH_ENTER, &found);
	if (!found || c->lxid != MyProc->vxid.lxid)
	{
		c->lxid = MyProc->vxid.lxid;
		c->nlocks = 0;
	}
	return ++c->nlocks;
}

static bool
acquire(const LOCKTAG *tag, LOCKMODE mode, bool dont_wait)
{
	return LockAcquire(tag, mode, false, dont_wait) != LOCKACQUIRE_NOT_AVAIL;
}

static TM_Result
not_available(Relation tree, LockWaitPolicy wait_policy)
{
	if (wait_policy == LockWaitError)
		ereport(ERROR,
				(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
				 errmsg("could not obtain lock on row in relation \"%s\"",
						RelationGetRelationName(tree))));
	return TM_WouldBlock;
}

static TM_Result
k1_lock_row(Relation tree, const ProllyKey *key, LockTupleMode mode,
			LockWaitPolicy wait_policy, TM_FailureData *tmfd)
{
	bool		dont_wait = (wait_policy != LockWaitBlock);
	LOCKMODE	whole = is_share(mode) ? ShareLock : ExclusiveLock;
	LOCKTAG		ttag;
	LOCKTAG		rtag;

	table_tag(&ttag, tree);
	if (LockHeldByMe(&ttag, whole, true))
		return TM_Ok;
	if (key == NULL)
		return acquire(&ttag, whole, dont_wait) ? TM_Ok :
			not_available(tree, wait_policy);

	if (!acquire(&ttag, is_share(mode) ? RowShareLock : RowExclusiveLock,
				 dont_wait))
		return not_available(tree, wait_policy);
	row_tag(&rtag, tree, key);
	if (LockHeldByMe(&rtag, row_mode(mode), true))
		return TM_Ok;
	if (count_lock(tree) > prolly_max_row_locks_per_table)
		return acquire(&ttag, whole, dont_wait) ? TM_Ok :
			not_available(tree, wait_policy);
	return acquire(&rtag, row_mode(mode), dont_wait) ? TM_Ok :
		not_available(tree, wait_policy);
}

/*
 * An inserter locks the key it inserts when it writes it, so ON CONFLICT
 * needs nothing more: a second inserter of the key waits for the first's
 * transaction, as on heap, and finds its row.
 */
static TM_Result
k1_insert_intent(Relation tree, const ProllyKey *key, uint32 spec_token,
				 TM_FailureData *tmfd)
{
	if (key == NULL)
		return TM_Ok;
	return k1_lock_row(tree, key, LockTupleExclusive, LockWaitBlock, tmfd);
}

static void
k1_insert_intent_complete(Relation tree, const ProllyKey *key,
						  uint32 spec_token, bool succeeded)
{
}

/* The first holder of a lock conflicting with mode on tag that has an xid. */
/*
 * The xid of a lock holder, a running transaction or a prepared one (whose
 * locks a dummy PGPROC holds, with no process); invalid if it has none.
 */
static TransactionId
holder_xid(VirtualTransactionId vxid)
{
	TransactionId xid;
	TransactionId xmin;
	int			nsubxid;
	bool		overflowed;
	bool		more;

	ProcNumberGetTransactionIds(vxid.procNumber, &xid, &xmin, &nsubxid,
								&overflowed);
	if (!TransactionIdIsValid(xid))
		xid = TwoPhaseGetXidByVirtualXID(vxid, &more);
	return xid;
}

static TransactionId
holder(const LOCKTAG *tag, LOCKMODE mode)
{
	VirtualTransactionId *holders = GetLockConflicts(tag, mode, NULL);

	for (int i = 0; VirtualTransactionIdIsValid(holders[i]); i++)
	{
		/*
		 * A writer that has no xid yet hasn't written anything: what the
		 * caller sees is still what is committed.
		 */
		TransactionId xid = holder_xid(holders[i]);

		if (TransactionIdIsValid(xid))
			return xid;
	}
	return InvalidTransactionId;
}

static TransactionId
k1_writer_of(Relation tree, const ProllyKey *key)
{
	LOCKTAG		tag;
	TransactionId xid;

	/*
	 * On the table's lock, ShareLock conflicts with the write intents and
	 * whole-table writes, RowShareLock with whole-table writes only; on a
	 * row's, RowShareLock conflicts with its write modes.  Neither conflicts
	 * with share locks, and this transaction's own locks never count.
	 */
	table_tag(&tag, tree);
	if (key == NULL)
		return holder(&tag, ShareLock);
	xid = holder(&tag, RowShareLock);
	if (TransactionIdIsValid(xid))
		return xid;
	row_tag(&tag, tree, key);
	return holder(&tag, RowShareLock);
}

/* Holders of the table's write intents or whole-table write locks. */
static int
k1_current_writers(Relation tree, TransactionId *xids, int max)
{
	LOCKTAG		tag;
	VirtualTransactionId *holders;
	int			n = 0;

	table_tag(&tag, tree);
	holders = GetLockConflicts(&tag, ShareLock, NULL);
	for (int i = 0; VirtualTransactionIdIsValid(holders[i]); i++)
	{
		TransactionId xid = holder_xid(holders[i]);

		if (!TransactionIdIsValid(xid))
			continue;
		if (n < max)
			xids[n] = xid;
		n++;
	}
	return n;
}

/*
 * SSI at table granularity: a read of any range of a tree locks the whole
 * relation in predicate.c, as Postgres does once a relation has too many
 * finer locks.  More transactions fail to serialize than on heap, never
 * fewer.
 */
static void
k1_predicate_lock_range(Relation tree, Snapshot snapshot, const ProllyKey *lo,
						const ProllyKey *hi)
{
	PredicateLockRelation(tree, snapshot);
}

/* The lock manager releases everything at the end of the (sub)transaction. */
static void
k1_xact_end(void)
{
}

static void
k1_subxact_abort(SubTransactionId subxid)
{
}

const ProllyCoordRoutine prolly_coord_k1 = {
	.info = {
		.iface = PROLLY_IFACE_COORDINATION,
		.impl_id = 1,
		.format_version = 1,
		.name = "k1",
		.description = "K1: row locks by key hash on the Postgres lock manager",
	},
	.lock_row = k1_lock_row,
	.insert_intent = k1_insert_intent,
	.insert_intent_complete = k1_insert_intent_complete,
	.writer_of = k1_writer_of,
	.current_writers = k1_current_writers,
	.predicate_lock_range = k1_predicate_lock_range,
	.xact_commit = k1_xact_end,
	.xact_abort = k1_xact_end,
	.subxact_abort = k1_subxact_abort,
};
