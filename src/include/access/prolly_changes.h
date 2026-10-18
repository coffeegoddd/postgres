/*-------------------------------------------------------------------------
 *
 * prolly_changes.h
 *	  Change finder interface: a table's data tree now, from its tree at a
 *	  fold and what changed since.
 *
 * A fold turns a table's rows into a data tree (for a commit, or a
 * working-set checkpoint): its baseline.  The change finder makes the
 * table's tree as the transaction sees it now from the baseline's tree and
 * the rows changed since, without reading the rest of the table; and
 * begins new baselines.  What it needs besides the tree it keeps in a
 * state of its own (bytes), recorded with the baseline by the caller.
 *
 * Contract:
 * - fold() sets *data to the table's data tree as the transaction's active
 *   snapshot sees it: the tree a fold of every row would give (canonical),
 *   whatever the baseline.  'baseline' is the tree and state of the
 *   table's last fold (NULL: none, or one of other rows' keying or
 *   encoding); a baseline the finder can't use (the table was rewritten
 *   since) makes it read every row.
 * - With 'next', fold() begins a new fold of the table and appends its
 *   state to next: with *data, the next baseline, which holds once the
 *   transaction commits the two as the table's baseline.  Rows the
 *   snapshot doesn't see as they will be committed (written by
 *   transactions it doesn't see) stay as they were in *data, and the next
 *   fold looks at them again.
 * - walk() visits the nodes a state keeps alive, for reclamation.
 * - counters() reports (and with reset, zeroes) what the session's folds
 *   have read: pages, candidate rows, and tables folded in full.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_changes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_CHANGES_H
#define PROLLY_CHANGES_H

#include "access/prolly_registry.h"
#include "access/prolly_tree.h"
#include "access/prolly_types.h"
#include "lib/stringinfo.h"
#include "storage/itemptr.h"
#include "utils/relcache.h"

/* A table's baseline: its data tree at a fold, and the finder's state. */
typedef struct ProllyTableFold
{
	ProllyNodeRef data;
	ProllyValue state;
} ProllyTableFold;

typedef struct ProllyChangeCounters
{
	uint64		pages;			/* pages read, of any version */
	uint64		candidates;		/* rows looked up */
	uint64		full;			/* tables whose every row was read */
} ProllyChangeCounters;

typedef struct ProllyChangeFinderRoutine
{
	ProllyImplInfo info;

	/*
	 * env has the chunk store and the tree's components; the finder orders
	 * keys by the table's keying itself.
	 */
	void		(*fold) (Relation rel, const ProllyTreeEnv *env,
						 const ProllyTableFold *baseline, ProllyNodeRef *data,
						 StringInfo next);
	void		(*walk) (const ProllyTreeEnv *env, const ProllyValue *state,
						 ProllyTreeVisitNode visit, void *arg);
	void		(*counters) (ProllyChangeCounters *counters, bool reset);

	/*
	 * The current transaction's own changes to rel, applied to data_in (a
	 * commit's data tree of rel) into *data: of the tuple versions tids
	 * names (in any order, repeated), those the transaction made and didn't
	 * replace are put, those it replaced that it hadn't made deleted; those
	 * of its subtransactions that aborted, neither.  False if they can't be
	 * applied so (a keyless row's copies would go below none), *data then
	 * undefined.  Needs an active snapshot.
	 */
	bool		(*fold_rows) (Relation rel, const ProllyTreeEnv *env,
							  const ProllyNodeRef *data_in,
							  ItemPointerData *tids, int ntids,
							  ProllyNodeRef *data);
} ProllyChangeFinderRoutine;

#define PROLLY_CHANGES_CALLBACK_NAMES "fold", "walk", "counters", "fold_rows"
#define PROLLY_CHANGES_FIRST_CALLBACK fold
#define PROLLY_CHANGES_LAST_CALLBACK fold_rows

static inline const ProllyChangeFinderRoutine *
prolly_changes_lookup(const char *name)
{
	return (const ProllyChangeFinderRoutine *)
		prolly_get_impl(PROLLY_IFACE_CHANGE_FINDER, name);
}

#endif							/* PROLLY_CHANGES_H */
