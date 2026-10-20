/*-------------------------------------------------------------------------
 *
 * prolly_seen.h
 *	  What a walk has seen: a map of hashes to small values of a fixed
 *	  size, held to a budget of memory.
 *
 * Past its budget, the map's entries go to a temporary file, sorted, as a
 * run, each run with a filter kept in memory (a Bloom filter, ten bits a
 * hash); a lookup reads memory, then each run whose filter may hold the
 * hash, newest first, by a binary search of the run in the file.  A value
 * found may be changed in place, in memory or in its run.  So a map of any
 * size takes about its budget in memory, as a hash join does, but as it
 * writes a run (its sorted copy beside the table: half again) and for its
 * filters (ten bits a hash it ever held, kept).  A transfer's maps that
 * live at once share maintenance_work_mem; runs aren't merged, so a map
 * of many runs reads its file more often for what it doesn't hold.
 *
 * src/include/access/prolly_seen.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_SEEN_H
#define PROLLY_SEEN_H

#include "access/prolly_types.h"

typedef struct ProllySeen ProllySeen;

/*
 * A map of hashes to values of value_size bytes (0: a set), keeping at most
 * budget bytes of entries in memory (maintenance_work_mem's, if 0), in the
 * current memory context.
 */
extern ProllySeen *prolly_seen_create(const char *name, Size value_size,
									  Size budget);

/* Whether hash is in it; if so, its value copied to value (if not NULL). */
extern bool prolly_seen_find(ProllySeen *seen, const ProllyHash *hash,
							 void *value);

/* Add hash with value; false, changing nothing, if it is in it already. */
extern bool prolly_seen_enter(ProllySeen *seen, const ProllyHash *hash,
							  const void *value);

/* Change the value of hash, which is in it. */
extern void prolly_seen_set(ProllySeen *seen, const ProllyHash *hash,
							const void *value);

/* How many hashes it holds, and how many runs went to its file. */
extern uint64 prolly_seen_count(ProllySeen *seen);
extern int	prolly_seen_runs(ProllySeen *seen);

extern void prolly_seen_destroy(ProllySeen *seen);

#endif							/* PROLLY_SEEN_H */
