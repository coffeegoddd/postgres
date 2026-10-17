/*-------------------------------------------------------------------------
 *
 * prolly_reclaim.h
 *	  Reclamation interface: deciding what in a database's chunk store is
 *	  garbage and freeing it.
 *
 * Live chunks are those the refs reach (prolly_refs.h, walk()): every
 * branch's head commit, baseline and working-set root, merges in progress,
 * remotes' and remote-tracking heads; from a commit its parents and root,
 * from a root its trees.  Everything else in the store is garbage.
 *
 * Contract:
 * - Nothing a live ref reaches is ever freed, nor anything a transaction
 *   that may still commit has written or referenced since the collection
 *   began: commits go on during a collection, except for a short final
 *   window.
 * - collect() reclaims the database's chunk store: by default what was
 *   written since the last collection, or every chunk if full.  A crash at
 *   any point leaves the store as it was or as the collection made it,
 *   never with a live chunk missing.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_reclaim.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_RECLAIM_H
#define PROLLY_RECLAIM_H

#include "access/prolly_registry.h"
#include "access/prolly_types.h"

/* What one collection did. */
typedef struct ProllyGcResult
{
	uint64		chunks_kept;
	uint64		chunks_freed;
	uint64		bytes_freed;
} ProllyGcResult;

typedef struct ProllyReclaimRoutine
{
	ProllyImplInfo info;

	/*
	 * Collect the current database's chunk store: what was written since
	 * the last collection, or (full) all of it.
	 */
	void		(*collect) (bool full, ProllyGcResult *result);
} ProllyReclaimRoutine;

#define PROLLY_RECLAIM_CALLBACK_NAMES \
	"collect"
#define PROLLY_RECLAIM_FIRST_CALLBACK collect
#define PROLLY_RECLAIM_LAST_CALLBACK collect

#endif							/* PROLLY_RECLAIM_H */
