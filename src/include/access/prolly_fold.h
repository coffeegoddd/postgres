/*-------------------------------------------------------------------------
 *
 * prolly_fold.h
 *	  What the change finders share about folding a table in full: why a
 *	  fold read every row, counted and logged.
 *
 * A fold reads a table in full, every row, when it can't fold from the
 * table's baseline: it has none, or it can't trust what changed since.
 * Each change finder says why (ProllyFullFoldReason), and
 * prolly_full_fold_noted() counts it in pg_stat_prolly, under the reason,
 * and logs it (prolly.log_full_folds) when it read enough rows to matter.
 *
 * A baseline folded under another row shape (DDL that changed rows' bytes
 * without writing them: a column added with a default, one dropped, a type
 * relabeled, the key changed) is mapped to the shape now from its own rows
 * (prolly_fold_map()), not from the table's, and folded from as any
 * baseline is.
 *
 * src/include/access/prolly_fold.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_FOLD_H
#define PROLLY_FOLD_H

#include "access/prolly_keying.h"
#include "access/prolly_tree.h"
#include "executor/tuptable.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"

typedef enum ProllyFullFoldReason
{
	PROLLY_FULL_NONE = 0,		/* (a fold from the baseline) */
	PROLLY_FULL_NO_BASELINE,	/* no baseline: the table's first fold */
	PROLLY_FULL_SHAPE_CHANGED,	/* its definition changed since */
	PROLLY_FULL_NO_LOG,			/* nothing tells what changed: no change
								 * log, or no block map */
	PROLLY_FULL_LOG_CHANGED,	/* the baseline isn't of the log as it is
								 * (another finder's, or a log made since) */
	PROLLY_FULL_STALE_SNAPSHOT, /* the snapshot doesn't see the baseline's
								 * fold */
	PROLLY_FULL_UNSETTLED,		/* the baseline's fold was such a fold */
	PROLLY_FULL_STORAGE_UNSEEN, /* a rewrite or truncation the snapshot
								 * doesn't see */
	PROLLY_FULL_RECOVERY,		/* during recovery */
	PROLLY_FULL_LOG_ENTRY,		/* an entry says to: truncated, rewritten, a
								 * row the log couldn't hold */
	PROLLY_FULL_TOO_MANY,		/* more rows changed than the table holds */
	PROLLY_FULL_KEYING_CHANGED, /* entries written under another keying */
	PROLLY_FULL_UNSUPPORTED_KEY,	/* a key it can't look rows up by */
	PROLLY_FULL_MEMORY_BUDGET,	/* the changes held passed
								 * maintenance_work_mem (no longer: a
								 * fold sorts them; kept for the
								 * counters' order) */
	PROLLY_FULL_FORCED,			/* debug_prolly_force_full_fold */
	PROLLY_FULL_VERIFY,			/* the verifier's, to compare with */
} ProllyFullFoldReason;

#define PROLLY_FULL_REASONS (PROLLY_FULL_VERIFY + 1)

/*
 * Visits a table whose next fold will read it in full (dolt_pending_full_folds()):
 * its qualified name, its OID and why.
 */
typedef void (*ProllyPendingFullVisit) (const char *name, Oid relid,
										ProllyFullFoldReason reason,
										void *arg);

/* debug_prolly_force_full_fold: every fold reads its table in full. */
extern PGDLLIMPORT bool debug_prolly_force_full_fold;

/* Set while the verifier folds a table in full (fold_verify.c). */
extern PGDLLIMPORT bool prolly_fold_verifying;

/*
 * Cleared while a fold is made only to check another against it
 * (debug_prolly_verify_fold_caches): the change finders count none of its
 * work, so a fold's counted work is its own whether or not it is checked.
 */
extern PGDLLIMPORT bool prolly_fold_counted;

/*
 * debug_prolly_verify_fold_caches: a working set taken from an earlier
 * fold, not folded (#482), is folded too, and checked against it
 * (prolly_check_fold_cache(): left_out, an object the new fold left out
 * where the earlier one left none; NULL if none).  So is what a fold takes
 * from the canonicalizer's caches (#483): worked out again, and an error
 * if it differs.
 */
extern PGDLLIMPORT bool debug_prolly_verify_fold_caches;
extern void prolly_check_fold_cache(const char *branch,
									const ProllyNodeRef *taken,
									const ProllyNodeRef *folded,
									const char *left_out);

/* prolly.fold_verify_rate: tables autovacuum's verifier checks a pass. */
extern PGDLLIMPORT double prolly_fold_verify_rate;

/* prolly.fold_verify_fatal: a verifier's mismatch is a PANIC. */
extern PGDLLIMPORT bool prolly_fold_verify_fatal;

/*
 * Must a fold read its table in full, whatever its baseline?  Forced, or
 * the verifier's; *reason says which.
 */
static inline bool
prolly_full_fold_wanted(ProllyFullFoldReason *reason)
{
	if (prolly_fold_verifying)
		*reason = PROLLY_FULL_VERIFY;
	else if (debug_prolly_force_full_fold)
		*reason = PROLLY_FULL_FORCED;
	else
		return false;
	return true;
}

/* A table the verifier checked, and whether its folds matched. */
typedef void (*ProllyFoldVerifyVisit) (const char *name, bool matches,
									   void *arg);

/*
 * The session branch's tables (rate: the fraction of them sampled), each
 * folded from its baseline and in full by one snapshot, the trees
 * compared: a mismatch warned of, counted, and its table read in full at
 * its next fold.  The mismatches.
 */
extern int	prolly_verify_folds(double rate, ProllyFoldVerifyVisit visit,
								void *arg);

/*
 * prolly.fold_on_rewrite: a rewrite of a versioned table folds its branch
 * as a checkpoint (prolly_fold_rewritten(), prolly_vcs.c).
 */
extern PGDLLIMPORT bool prolly_fold_on_rewrite;
extern bool prolly_fold_rewritten(Oid relid);

/* prolly.log_full_folds: rows past which a full fold is logged (-1: none). */
extern PGDLLIMPORT int prolly_log_full_folds;

/* A reason's name, as pg_stat_prolly's counters and the log give it. */
extern const char *prolly_full_fold_reason_name(ProllyFullFoldReason reason);

/* What a full fold read and wrote, and what its sort took. */
typedef struct ProllyFullFold
{
	int64		rows_read;
	int64		rows_written;
	bool		spilled;		/* its sort went to disk */
	int64		sort_kb;		/* the sort's memory, or its disk if spilled */
} ProllyFullFold;

/*
 * Every row snap sees of rel folded into a data tree, in key order (the
 * keying's), identical rows of a keyless table counted once; a keyed
 * table's rows with one key are an error.  The rows are sorted in
 * maintenance_work_mem, spilling past it, so a table of any size takes
 * about that much.  slot is rel's, to read rows into.
 */
extern void prolly_fold_all(Relation rel, Snapshot snap, TupleTableSlot *slot,
							const ProllyKeyingRoutine *kr, ProllyKeying *k,
							bool keyless, const ProllyTreeRoutine *tree,
							const ProllyTreeEnv *env, ProllyNodeRef *data,
							ProllyFullFold *done);

/*
 * A change finder's changed rows sorted in maintenance_work_mem, spilling
 * past it: each put with its key, its row (an entry's tuple, or an
 * encoded row), a delta and where the row is (invalid: not known), and
 * read back in the keying's order, keys the keying calls equal by their
 * bytes.  So a fold from the baseline whose changes outgrow memory reads
 * them back in order, a part at a time, rather than reading every row.
 */
typedef struct ProllyChangeSort ProllyChangeSort;

typedef struct ProllySortedChange
{
	ProllyKey	key;			/* valid until the next read */
	const char *row;
	uint32		rowlen;
	int64		delta;
	ItemPointerData tid;
} ProllySortedChange;

extern ProllyChangeSort *prolly_change_sort_begin(const ProllyKeyingRoutine *kr,
												  ProllyKeying *k);
extern void prolly_change_sort_put(ProllyChangeSort *s, const ProllyKey *key,
								   const char *row, uint32 rowlen, int64 delta,
								   const ItemPointerData *tid);
/* The changes put sorted: none may be put after. */
extern void prolly_change_sort_done(ProllyChangeSort *s);
extern bool prolly_change_sort_next(ProllyChangeSort *s,
									ProllySortedChange *change);
/* The sort ended, its files removed; *spilled: whether it went to disk. */
extern void prolly_change_sort_end(ProllyChangeSort *s, bool *spilled);

/* Re-sort historical keys, spilling; false (data unset) if keys now coincide. */
extern bool prolly_fold_reorder(const ProllyKeyingRoutine *kr, ProllyKeying *k,
								const ProllyTreeRoutine *tree, const ProllyTreeEnv *env,
								const ProllyNodeRef *from, ProllyNodeRef *data);

/*
 * A baseline's tree, from, whose rows were folded under the row shape old
 * (rel's keying's row_shape() then), mapped to rel's rows as they are now
 * into *data, without reading the table: each row read as it was folded
 * (the keying's open_shape()), given rel's columns now (those dropped
 * since left out, those added since their missing value, or NULL, those
 * retyped since their datums, where the type's cast keeps them), and
 * encoded and keyed as rel's are, in key order: sorted again, in
 * maintenance_work_mem, spilling past it, if the key changed (keyless
 * rows, whose key is their bytes, copies merged).  *rows gets how many
 * rows were read.  False, *data unset, if they can't be mapped so: the
 * keying reads no rows of old, a column's type changed by a cast that
 * computes (a rewrite), or the rows can't be the table's (a key twice).
 */
extern bool prolly_fold_map(Relation rel, const ProllyTreeEnv *env,
							const ProllyNodeRef *from, const ProllyValue *old,
							ProllyNodeRef *data, int64 *rows);

/*
 * Could rows folded under the row shape old be mapped to rel's now
 * (prolly_fold_map()), as far as is known without reading them: the
 * keying reads rows of old, and each column of rel's is had from theirs?
 * (Their keys' order is known only by reading them.)
 */
extern bool prolly_fold_mappable(Relation rel, const ProllyValue *old);

/*
 * Would mapping rel's baseline cost less than reading rel in full?  Both
 * read every row: a map decodes and encodes the baseline's, while a full
 * fold scans the heap, dead versions and all, and encodes the live ones,
 * which costs less per row.  So a map is chosen only once the table's
 * statistics count prolly.map_reshaped_dead_ratio dead versions for each
 * live row (0: always), a rough break-even.
 */
extern PGDLLIMPORT double prolly_map_reshaped_dead_ratio;
extern bool prolly_fold_map_cheaper(Relation rel);

/*
 * A baseline's tree as a fold from it reads it, into *data: its own, or,
 * folded under another row shape (reshaped), its rows mapped to rel's
 * now (prolly_fold_map(), counted in pg_stat_prolly).  A finder asks for
 * it only once it will fold from the baseline, not read rel in full, as a
 * map reads every row of the baseline.  False if they can't be mapped.
 */
struct ProllyTableFold;
extern bool prolly_table_fold_data(Relation rel, const ProllyTreeEnv *env,
								   const struct ProllyTableFold *baseline,
								   ProllyNodeRef *data);

/* The last full fold's (for tests). */
extern ProllyFullFold prolly_last_full_fold;

/* A full fold of rel done: counted under reason, logged if it was large. */
extern void prolly_full_fold_noted(Relation rel, ProllyFullFoldReason reason,
								   int64 rows, double ms);

#endif							/* PROLLY_FOLD_H */
