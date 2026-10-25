/*-------------------------------------------------------------------------
 *
 * refs_generations.h
 *	  Branch generations and the working sets their folds made, for the
 *	  "copies" refs (refs_copies.c): see refs_generations.c.
 *
 * src/backend/access/prolly/refs_generations.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef REFS_GENERATIONS_H
#define REFS_GENERATIONS_H

#include "access/prolly_refs.h"
#include "access/prolly_types.h"
#include "access/transam.h"
#include "utils/snapshot.h"

/* What a branch's fold was begun at: taken before it reads anything. */
typedef struct BranchFoldStamp
{
	Oid			branch;
	uint64		generation;
	uint64		overflow;
	uint64		epoch;
} BranchFoldStamp;

extern Size branch_generations_shmem_size(void);
extern void branch_generations_shmem_init(void *area);
extern void branch_generations_note_write(Oid branch);
extern void branch_note_head_restore(Oid branch);
extern bool branch_head_restore_unseen(Oid branch, Snapshot snap);
extern void branch_generations_note_relation(Oid branch, Oid relid,
											 int nrows);

/* The refs' fold_mark() and written_since() (prolly_refs.h). */
extern bool branch_fold_mark(Oid branch, Snapshot snap, ProllyFoldMark *mark);
extern bool branch_written_since(const ProllyFoldMark *mark, Snapshot snap,
								 Oid **relids, int *nrelids);
extern void branch_generations_forget(void);
extern void branch_generations_prepared_ended(FullTransactionId xid);

/*
 * A fold of branch's working set by snap: stamped before it, published
 * after it, and taken by a later one instead of folding (false: not
 * known).  baseline is the one it folded from (NULL: none); complete,
 * whether it left nothing out (an object a commit would refuse).  Either is
 * a no-op where nothing can be known (in recovery, in a transaction that
 * wrote the branch or a catalog, while any transaction is prepared).
 */
extern void branch_fold_stamp(Oid branch, BranchFoldStamp *stamp);
extern void branch_fold_publish(const BranchFoldStamp *stamp, Snapshot snap,
								const ProllyHash *baseline,
								const ProllyHash *root, bool complete);
extern bool branch_fold_known(Oid branch, Snapshot snap,
							  const ProllyHash *baseline, bool complete,
							  ProllyHash *root);

/*
 * branch_fold_known(), with what it was known at (at): for
 * branch_fold_unchanged() to ask later, in the same transaction, whether
 * it still holds, though the transaction's catalog changes since keep
 * anything new from being known.
 */
extern bool branch_fold_known_at(Oid branch, Snapshot snap,
								 const ProllyHash *baseline, bool complete,
								 ProllyHash *root, BranchFoldStamp *at);
extern bool branch_fold_unchanged(const BranchFoldStamp *at);

/*
 * The last fold of branch known, writers having ended or writing since
 * (one still known, nobody writing, is branch_fold_known()'s): its root, if
 * it left nothing out, was folded from baseline (NULL: none), and nothing
 * it was folded at has moved since but the generation (its writers), into
 * *root, and a mark as of when it was known into *mark (false: none).  The
 * working set is that root but for the relations written since the mark
 * (branch_written_since(), which may still say it can't tell).
 */
extern bool branch_fold_last(Oid branch, Snapshot snap,
							 const ProllyHash *baseline, ProllyHash *root,
							 ProllyFoldMark *mark);

/*
 * Do branch's pages hold only what snap sees and the transaction's own
 * changes?  (Its writers kept out by the caller.)
 */
extern bool branch_writers_settled(Oid branch, Snapshot snap);

/*
 * The fold worker's view: the databases, and the current database's
 * branches (with their generation), whose generation moved since it last
 * saw them; and what it saw.
 */
extern int	branch_generations_moved_databases(Oid *databases, int max);
extern int	branch_generations_moved(ProllyMovedBranch *branches, int max);
extern void branch_generations_saw(Oid branch, uint64 generation);
extern void branch_generations_saw_database(Oid database);

#endif							/* REFS_GENERATIONS_H */
