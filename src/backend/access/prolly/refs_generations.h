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

#include "access/prolly_types.h"
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
extern void branch_generations_forget(void);

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

#endif							/* REFS_GENERATIONS_H */
