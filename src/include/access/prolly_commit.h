/*-------------------------------------------------------------------------
 *
 * prolly_commit.h
 *	  Commit pipeline interface: ordering and publishing branch roots.
 *
 * At commit, a transaction's new root must become a branch's head at the
 * instant its xid becomes visible (B1).  Implementations: a single lock, or
 * the ordered pipeline of specs/02-postgres-integration.md (design change 1).
 *
 * Usage, per branch a transaction changes:
 *		branch_lock(b);  (engine rebases onto head(b))
 *		ticket = enqueue(b, new_root, commit_lsn);
 *		branch_unlock(b);  (before the WAL flush)
 *		... RecordTransactionCommit() flushes ...
 *		publish(ticket);  (in ProcArrayEndTransaction)
 *
 * Contract:
 * - Roots are published in enqueue order per branch; publish() returns once
 *   ticket's root and every earlier one is the head.
 * - Nothing after enqueue() can fail short of PANIC, so the queue never has
 *   gaps.
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
#include "access/xlogdefs.h"

typedef uint64 ProllyCommitTicket;

typedef struct ProllyCommitRoutine
{
	ProllyImplInfo info;

	Size		(*shmem_size) (void);
	void		(*shmem_init) (void);

	void		(*branch_lock) (ProllyBranchId branch);
	ProllyCommitTicket (*enqueue) (ProllyBranchId branch,
								   const ProllyNodeRef *new_root,
								   XLogRecPtr commit_lsn);
	void		(*branch_unlock) (ProllyBranchId branch);
	void		(*publish) (ProllyCommitTicket ticket);

	/* The branch's current published head. */
	void		(*head) (ProllyBranchId branch, ProllyNodeRef *root);
} ProllyCommitRoutine;

#define PROLLY_COMMIT_CALLBACK_NAMES \
	"shmem_size", "shmem_init", "branch_lock", "enqueue", "branch_unlock", \
	"publish", "head"
#define PROLLY_COMMIT_FIRST_CALLBACK shmem_size

static inline const ProllyCommitRoutine *
prolly_commit_lookup(const char *name)
{
	return (const ProllyCommitRoutine *)
		prolly_get_impl(PROLLY_IFACE_COMMIT_PIPELINE, name);
}

#endif							/* PROLLY_COMMIT_H */
