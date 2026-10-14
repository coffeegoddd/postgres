/*-------------------------------------------------------------------------
 *
 * coord_singlewriter.c
 *	  Coordination implementation "single_writer": one writer per table.
 *
 * A table has one writer at a time.  The right to write it is a
 * heavyweight lock of its own (LOCKTAG_PROLLY_TABLE), separate from the
 * relation's locks, so it conflicts only with itself: every writing
 * statement already holds RowExclusiveLock on the relation, which an
 * ExclusiveLock on the relation would conflict with, making two writers
 * that had both started deadlock.
 *
 * lock_row ignores the key and takes the table's lock:
 *
 *	FOR KEY SHARE, FOR SHARE			ShareLock
 *	FOR NO KEY UPDATE, FOR UPDATE		ExclusiveLock
 *
 * so writes and the exclusive row locks are serialized per table, while
 * any number of sessions hold share row locks (the FOR KEY SHARE locks of
 * foreign-key checks among them) and writers wait for them.  The lock is
 * held until the end of the (sub)transaction that took it, by the lock
 * manager, so there is no state to clean up and deadlocks are found by
 * Postgres's detector.
 *
 * Coarser than row locking: two sessions locking different rows of one
 * table in conflicting modes wait for each other.  Row-level coordination
 * replaces this.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/coord_singlewriter.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"
#include "storage/lmgr.h"
#include "utils/rel.h"

static LOCKMODE
table_mode(LockTupleMode mode)
{
	switch (mode)
	{
		case LockTupleKeyShare:
		case LockTupleShare:
			return ShareLock;
		case LockTupleNoKeyExclusive:
		case LockTupleExclusive:
			return ExclusiveLock;
	}
	elog(ERROR, "unrecognized row lock mode %d", (int) mode);
	return NoLock;				/* keep compiler quiet */
}

static TM_Result
singlewriter_lock_row(Relation tree, const ProllyKey *key, LockTupleMode mode,
					  LockWaitPolicy wait_policy, TM_FailureData *tmfd)
{
	bool		dont_wait = (wait_policy != LockWaitBlock);

	if (LockProllyTable(RelationGetRelid(tree), table_mode(mode), dont_wait))
		return TM_Ok;
	if (wait_policy == LockWaitError)
		ereport(ERROR,
				(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
				 errmsg("could not obtain lock on row in relation \"%s\"",
						RelationGetRelationName(tree))));
	return TM_WouldBlock;
}

/* Speculative insertion has no one to race with: the inserter is the writer. */
static TM_Result
singlewriter_insert_intent(Relation tree, const ProllyKey *key,
						   uint32 spec_token, TM_FailureData *tmfd)
{
	return singlewriter_lock_row(tree, key, LockTupleExclusive, LockWaitBlock,
								 tmfd);
}

static void
singlewriter_insert_intent_complete(Relation tree, const ProllyKey *key,
									uint32 spec_token, bool succeeded)
{
}

const ProllyCoordRoutine prolly_coord_single_writer = {
	.info = {
		.iface = PROLLY_IFACE_COORDINATION,
		.impl_id = 4,
		.format_version = 1,
		.name = "single_writer",
		.description = "one writer per table: a table lock of its own, shared "
		"by share row locks",
	},
	.lock_row = singlewriter_lock_row,
	.insert_intent = singlewriter_insert_intent,
	.insert_intent_complete = singlewriter_insert_intent_complete,
};
