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
 *   freed.  Another collection of the store running, it is refused: an
 *   error, or with if_free, false returned, nothing done.  A crash at any point leaves the store as it was or as the
 *   collection made it, never with a live chunk missing.
 * - move() rewrites the journal's sealed segments into a table file, every
 *   chunk, garbage and all (not settled: the next default collection still
 *   collects it), so the journal a backend indexes by reading it stays
 *   short.  It frees nothing, and leaves the table files as they are.
 *   Optional.
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
	 * the last collection, or (full) all of it.  False if if_free and
	 * another collection of the store is running.
	 */
	bool		(*collect) (bool full, bool if_free, ProllyGcResult *result);

	/* Move what was written since the last collection, freeing nothing. */
	void		(*move) (ProllyGcResult *result);

	/*
	 * Optional: retire the store's generations lo to hi, freeing what of
	 * their settled files nothing names, at the cost of those files, what
	 * was written since the last collection, and what the other settled
	 * files name in them; false if if_free and another collection runs.
	 */
	bool		(*retire) (uint32 lo, uint32 hi, bool if_free,
						   ProllyGcResult *result);

	/*
	 * Optional, with retire: the generations a retire is worth running on
	 * now, false if none: the oldest whose settled files are thought at
	 * least half garbage (the store's estimates), with the next ones as
	 * garbage after them while their files' bytes stay within budget (the
	 * first taken whatever its size), none newer than max_hi.  Generations
	 * whose files a conjoin merged go together.
	 */
	bool		(*choose) (uint64 budget, uint32 max_hi, uint32 *lo,
						   uint32 *hi);
} ProllyReclaimRoutine;

#define PROLLY_RECLAIM_CALLBACK_NAMES \
	"collect", "move", "retire", "choose"
#define PROLLY_RECLAIM_FIRST_CALLBACK collect
#define PROLLY_RECLAIM_LAST_CALLBACK choose

#endif							/* PROLLY_RECLAIM_H */
