/*-------------------------------------------------------------------------
 *
 * prolly_seen.h
 *	  What a walk has seen: a map of hashes to small values of a fixed
 *	  size, held to a budget of memory.
 *
 * Past its budget, the map's entries go to temporary files, sorted, as
 * runs, each run with a Bloom filter kept in memory; a lookup reads memory,
 * then each run whose filter may hold the hash, newest first, by a search
 * of the run's file.  A value found may be changed in place, in memory or
 * in its run.  Runs are merged, four of a level into one of the next, so a
 * lookup checks about 3 log4(n / t) filters for n hashes, t the table's
 * capacity, and reads a run only where its filter can't rule the hash out.
 *
 * The map's memory is its budget: half for its filters, which are folded
 * smaller as the map grows past what that half holds at ten bits a hash
 * (so their false positives rise, never their false negatives), and the
 * rest for its table, less two buffers (a run's file's, read and written,
 * a block each or a 32nd of a small budget), but a quarter at least, and
 * a merge's buffers in the table's place while it is freed.  However many
 * runs it has, a run costs only its filter and a few words.  So a map
 * holds to its budget from 64kB up.  A transfer's maps that live at once
 * share maintenance_work_mem.
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
 * A map of hashes to values of value_size bytes (0: a set), keeping about
 * budget bytes in memory (maintenance_work_mem's, if 0), in the current
 * memory context; its files are the current resource owner's.
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

/* How many hashes it holds, and how many runs are in its files. */
extern uint64 prolly_seen_count(ProllySeen *seen);
extern int	prolly_seen_runs(ProllySeen *seen);

/* What a map has cost, from its creation. */
typedef struct ProllySeenStats
{
	Size		budget;			/* bytes, as created */
	Size		filter_budget;	/* the filters' share */
	Size		memory_peak;	/* most bytes its memory context held, as
								 * runs were written or merged (its most) or
								 * as asked */
	Size		filter_bytes;	/* its filters' now */
	Size		filter_bytes_peak;
	uint64		filter_folds;	/* filters halved to fit their share */
	uint64		lookups;		/* finds, enters, sets */
	uint64		filter_checks;	/* runs a lookup considered */
	int			max_filter_checks;	/* the most by one lookup */
	uint64		run_searches;	/* runs read: a filter didn't rule out */
	uint64		false_positives;	/* runs read in vain */
	uint64		record_reads;	/* records compared searching runs */
	uint64		block_reads;	/* reads of a run's file searching it */
	uint64		runs_written;	/* from the table */
	uint64		merges;
	uint64		records_merged; /* records written again by merges */
	int			runs;			/* now */
	int			runs_peak;
	uint64		temp_bytes;		/* of records in its files now */
	uint64		temp_bytes_peak;	/* most, a merge's output with inputs */
} ProllySeenStats;

extern void prolly_seen_stats(ProllySeen *seen, ProllySeenStats *stats);

extern void prolly_seen_destroy(ProllySeen *seen);

#endif							/* PROLLY_SEEN_H */
