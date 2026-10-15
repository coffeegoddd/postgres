/*-------------------------------------------------------------------------
 *
 * prolly_coord.h
 *	  Coordination interface: row locks, insert intents, predicate locks and
 *	  commit markers for prolly trees.
 *
 * Implementations: K1 (separate lock table), K2 (coordination plane on live
 * nodes), K3 (locks in the page), and "single_writer" (one writer per
 * table, until row-level coordination is built).
 *
 * Contract:
 * - Operations act for the current (sub)transaction.
 * - Waiting is done with XactLockTableWait() and friends, so deadlock
 *   detection is Postgres's.  Results use the table AM's TM_Result and
 *   TM_FailureData so the adapter can return them unchanged.
 * - Lock state never outlives its transaction or subtransaction, except for
 *   prepared transactions and commit markers.
 * - Nothing here is ever part of a hashed chunk: hashes cover canonical
 *   content only, never transactional state.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_coord.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_COORD_H
#define PROLLY_COORD_H

#include "access/prolly_registry.h"
#include "access/prolly_types.h"
#include "access/tableam.h"
#include "nodes/lockoptions.h"

typedef struct ProllyCoordRoutine
{
	ProllyImplInfo info;

	Size		(*shmem_size) (void);
	void		(*shmem_init) (void *area);

	/*
	 * Lock key in tree in mode, waiting per wait_policy.  On anything but
	 * TM_Ok, tmfd says who holds or changed the row.  Writers lock what
	 * they write first: the row's key in an exclusive mode, before
	 * inserting, updating or deleting it, or a NULL key to write the whole
	 * tree (rebuilding it); readers taking row locks lock the row's key.
	 * The engine's private writes happen after the lock is held.
	 */
	TM_Result	(*lock_row) (Relation tree, const ProllyKey *key,
							 LockTupleMode mode, LockWaitPolicy wait_policy,
							 TM_FailureData *tmfd);

	/*
	 * A transaction other than this one that holds a write lock on key in
	 * tree (NULL: on any part of tree), or InvalidTransactionId: the one a
	 * uniqueness check must wait for before deciding a row it found (or a
	 * TID it can't resolve) is really there, as heap reports a tuple's
	 * inserter or deleter.  The caller waits (XactLockTableWait()) and
	 * asks again: there may be more than one.
	 */
	TransactionId (*writer_of) (Relation tree, const ProllyKey *key);

	/*
	 * Speculative insertion (INSERT ... ON CONFLICT): announce intent to
	 * insert key with spec_token, so others wait with
	 * SpeculativeInsertionWait(); then confirm or withdraw it.
	 */
	TM_Result	(*insert_intent) (Relation tree, const ProllyKey *key,
								  uint32 spec_token, TM_FailureData *tmfd);
	void		(*insert_intent_complete) (Relation tree, const ProllyKey *key,
										   uint32 spec_token, bool succeeded);

	/*
	 * Transactions other than this one that hold write locks on tree now;
	 * stores at most max of them in xids, returns how many there are.
	 */
	int			(*current_writers) (Relation tree, TransactionId *xids, int max);

	/* SSI: record a read of [lo, hi] (NULL = unbounded) with predicate.c. */
	void		(*predicate_lock_range) (Relation tree, Snapshot snapshot,
										 const ProllyKey *lo,
										 const ProllyKey *hi);

	/*
	 * True if key was written by a transaction that committed after snapshot
	 * was taken (from commit markers, kept until the xmin horizon passes
	 * them); sets *writer.
	 */
	bool		(*written_since) (Relation tree, const ProllyKey *key,
								  Snapshot snapshot, TransactionId *writer);

	/* End of transaction: turn write locks into commit markers, or drop. */
	void		(*xact_commit) (void);
	void		(*xact_abort) (void);
	void		(*subxact_abort) (SubTransactionId subxid);

	/* Drop commit markers no snapshot at or after oldest_xmin can need. */
	void		(*prune_markers) (TransactionId oldest_xmin);
} ProllyCoordRoutine;

#define PROLLY_COORD_CALLBACK_NAMES \
	"shmem_size", "shmem_init", "lock_row", "writer_of", "insert_intent", \
	"insert_intent_complete", "current_writers", "predicate_lock_range", "written_since", \
	"xact_commit", "xact_abort", "subxact_abort", "prune_markers"
#define PROLLY_COORD_FIRST_CALLBACK shmem_size

/* Row locks a transaction takes on one table before locking all of it. */
extern PGDLLIMPORT int prolly_max_row_locks_per_table;

static inline const ProllyCoordRoutine *
prolly_coord_lookup(const char *name)
{
	return (const ProllyCoordRoutine *)
		prolly_get_impl(PROLLY_IFACE_COORDINATION, name);
}

#endif							/* PROLLY_COORD_H */
