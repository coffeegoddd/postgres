/*-------------------------------------------------------------------------
 *
 * prolly_commit.h
 *	  Commit pipeline interface: branch heads, and publishing new ones.
 *
 * Every branch of a database has a head: the working-set root its readers
 * see.  A transaction that changed a branch makes a new root, which must
 * become the head when, and only when, the transaction commits, and must
 * survive a crash exactly when the commit does.  Implementations: "serial"
 * (one committer per branch at a time, until its transaction ends), or an
 * ordered pipeline that rebases under a short per-branch lock and
 * publishes roots in queue order.
 *
 * Usage at commit, for each branch the transaction changed:
 *		branch_lock(b);
 *		tip(b, &base);		(the engine rebases its writes onto base)
 *		ticket = enqueue(b, &new_root);
 *		branch_unlock(b);
 *		... the commit record is written and flushed ...
 *		publish(ticket);	(or abandon(ticket) if the transaction aborts)
 *
 * Contract:
 * - Branches are those of the current database (MyDatabaseId), numbered
 *   by the database's refs.
 * - tip() and enqueue() need the branch lock.  tip() returns the root the
 *   next commit must build on: the newest root enqueued for the branch, or
 *   its head; false if the branch has never had a root.
 * - enqueue() WAL-logs the new root with the current transaction's xid
 *   (assigning one if needed).  After a crash, and on a standby, a
 *   branch's head is the root of the last enqueued transaction that
 *   committed.
 * - Roots are published in enqueue order per branch; publish() returns once
 *   ticket's root and every earlier one is the head.  If the transaction
 *   aborts after enqueue(), abandon() is called instead; its root never
 *   becomes the head, and the implementation makes sure no other commit
 *   was built on it.
 * - head() returns the published head, without the branch lock; false if
 *   the branch has never had a root.
 * - copy_database() gives a new database dst_dboid the branches of
 *   src_dboid, as part of the current transaction: their heads become
 *   dst's when, and only when, the transaction commits (CREATE DATABASE
 *   copies the template's catalogs and files, and this copies the roots
 *   they refer to).  If the transaction aborts, dst is left with no
 *   branches.  The caller makes sure nothing writes to src meanwhile.
 *   Errors if dst already has branches.
 * - drop_database() forgets every branch of dboid, at once and durably
 *   (DROP DATABASE removes the database's files the same way).
 * - usage() reports how many heads and roots waiting to be published or
 *   abandoned the implementation holds, across all databases.
 * - checkpoint() makes every branch's state as of the checkpoint's redo
 *   point durable; redo() replays the implementation's own ProllyCommit
 *   records (prolly_xlog.h).
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_commit.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_COMMIT_H
#define PROLLY_COMMIT_H

#include "access/prolly_registry.h"
#include "access/prolly_types.h"
#include "access/xlogreader.h"

typedef uint64 ProllyCommitTicket;

typedef struct ProllyCommitRoutine
{
	ProllyImplInfo info;

	Size		(*shmem_size) (void);
	void		(*shmem_init) (void *area);

	void		(*branch_lock) (ProllyBranchId branch);
	bool		(*tip) (ProllyBranchId branch, ProllyNodeRef *root);
	ProllyCommitTicket (*enqueue) (ProllyBranchId branch,
								   const ProllyNodeRef *new_root);
	void		(*branch_unlock) (ProllyBranchId branch);
	void		(*publish) (ProllyCommitTicket ticket);
	void		(*abandon) (ProllyCommitTicket ticket);

	/* The branch's current published head. */
	bool		(*head) (ProllyBranchId branch, ProllyNodeRef *root);

	/* Databases. */
	void		(*copy_database) (Oid src_dboid, Oid dst_dboid);
	void		(*drop_database) (Oid dboid);

	void		(*usage) (int *nheads, int *npending);

	void		(*checkpoint) (int flags);
	void		(*redo) (XLogReaderState *record);
} ProllyCommitRoutine;

#define PROLLY_COMMIT_CALLBACK_NAMES \
	"shmem_size", "shmem_init", "branch_lock", "tip", "enqueue", \
	"branch_unlock", "publish", "abandon", "head", "copy_database", \
	"drop_database", "usage", "checkpoint", "redo"
#define PROLLY_COMMIT_FIRST_CALLBACK shmem_size

static inline const ProllyCommitRoutine *
prolly_commit_lookup(const char *name)
{
	return (const ProllyCommitRoutine *)
		prolly_get_impl(PROLLY_IFACE_COMMIT_PIPELINE, name);
}

#endif							/* PROLLY_COMMIT_H */
