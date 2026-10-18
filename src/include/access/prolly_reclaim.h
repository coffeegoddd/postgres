/*-------------------------------------------------------------------------
 *
 * prolly_reclaim.h
 *	  Reclamation interface: deciding what in a database's chunk store is
 *	  garbage and freeing it.
 *
 * Live chunks are those the refs reach (prolly_refs.h, walk()): every
 * branch's head commit, baseline and working-set root, merges in progress,
 * remotes' and remote-tracking heads; from a commit its parents and root,
 * from a root its trees.  Everything else in the store is garbage.  A
 * collector drives the chunk store's side of a collection (its gc_*
 * callbacks, prolly_chunkstore.h): it marks, the store copies and swaps.
 *
 * Contract:
 * - Nothing a live ref reaches is ever freed, nor anything a transaction
 *   that may still commit has written or referenced since the collection
 *   began: commits go on during a collection, except for a short final
 *   window.  (One that references, after the swap, a chunk the collection
 *   found unreachable fails, by the chunk store's checks.)
 * - collect() reclaims the current database's chunk store: by default what
 *   was written since the last collection (the store's settled files are
 *   kept whole), or every chunk if full; result says what was kept and
 *   freed.  A crash at any point leaves the store as it was or as the
 *   collection made it, never with a live chunk missing.
 * - move() is a default collection that keeps every chunk: what was
 *   written since the last collection is rewritten into the store's
 *   settled files, garbage and all, so the journal a backend indexes by
 *   reading it stays short.  It frees nothing.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_reclaim.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_RECLAIM_H
#define PROLLY_RECLAIM_H

#include "access/prolly_chunkstore.h"
#include "access/prolly_registry.h"
#include "access/prolly_types.h"

typedef struct ProllyReclaimRoutine
{
	ProllyImplInfo info;

	/*
	 * Collect the current database's chunk store: what was written since
	 * the last collection, or (full) all of it.
	 */
	void		(*collect) (bool full, ProllyGcResult *result);

	/* Move what was written since the last collection, freeing nothing. */
	void		(*move) (ProllyGcResult *result);
} ProllyReclaimRoutine;

#define PROLLY_RECLAIM_CALLBACK_NAMES \
	"collect", "move"
#define PROLLY_RECLAIM_FIRST_CALLBACK collect
#define PROLLY_RECLAIM_LAST_CALLBACK move

#endif							/* PROLLY_RECLAIM_H */
