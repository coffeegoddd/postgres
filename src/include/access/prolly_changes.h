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
 * - walk() visits the nodes a state keeps alive, for reclamation.  It is
 *   asked of every state, whichever finder wrote it, and passes over those
 *   it didn't write.
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

#include "access/prolly_fold.h"
#include "access/prolly_registry.h"
#include "access/prolly_tree.h"
#include "access/prolly_types.h"
#include "lib/stringinfo.h"
#include "storage/itemptr.h"
#include "utils/relcache.h"

/*
 * A table's baseline: its data tree at a fold, and the finder's state.
 * reshaped: the tree's rows were folded under another row shape than the
 * table's now, shape (the keying's row_shape() then), so a fold from it
 * reads the tree by prolly_table_fold_data(), which maps them, and rows
 * the state holds are in another's.
 */
typedef struct ProllyTableFold
{
	ProllyNodeRef data;
	ProllyValue state;
	bool		reshaped;
	ProllyValue shape;
} ProllyTableFold;

typedef struct ProllyChangeCounters
{
	uint64		pages;			/* pages read, of any version */
	uint64		candidates;		/* rows looked up */
	uint64		full;			/* tables whose every row was read */
	uint64		rows_read;		/* rows read: looked up, or every row */
	uint64		rows_written;	/* rows put in or taken out of the tree */
	uint64		merges;			/* folds that merged many changes with the
								 * baseline, a pass over each */
} ProllyChangeCounters;

typedef struct ProllyChangeFinderRoutine
{
	ProllyImplInfo info;

	/*
	 * env has the chunk store and the tree's components; the finder orders
	 * keys by the table's keying itself.  Without a baseline, no_baseline
	 * is why there is none (the table's first fold, or its definition
	 * changed).  A fold that reads the table in full counts and logs why
	 * (prolly_full_fold_noted()).
	 */
	void		(*fold) (Relation rel, const ProllyTreeEnv *env,
						 const ProllyTableFold *baseline,
						 ProllyFullFoldReason no_baseline, ProllyNodeRef *data,
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

	/*
	 * Optional, for a finder whose state (fold()'s next) names chunks:
	 * appends the references it holds to refs (the tree's
	 * value_reference()), which a baseline's tree declares.  It passes over
	 * a state it didn't write.
	 */
	void		(*state_refs) (const ProllyValue *state, StringInfo refs);

	/*
	 * Optional: the next state (into next) of a baseline that holds rel's
	 * last baseline's rows (whose state is state) with the current
	 * transaction's own changes applied, as fold_rows() applies them, and
	 * nothing else: as of the active snapshot, its fold's transaction this
	 * one.  The finder marks those changes folded (its own record of them
	 * consumed); the others since the last baseline stay to be folded.
	 * False, marking nothing, where the last baseline's state can't be
	 * advanced so: the caller keeps it.
	 */
	bool		(*advance) (Relation rel, const ProllyValue *state,
							StringInfo next);

	/*
	 * True if a branch's changes should be folded as it is left: for a
	 * finder that keeps what it reads changes from until the next fold (old
	 * versions of pages), so what it keeps for them is freed then rather
	 * than at its next commit.  False for one that keeps nothing a fold
	 * frees but its own record of the changes, which can wait.
	 */
	bool		(*fold_on_leave) (void);

	/*
	 * Optional: the rows of rel that differ from baseline's, as fold() from
	 * it would find them, visited each once in the keying's order as a
	 * tree's diff() visits a change, without making a tree (what they point
	 * to valid during the visit only).  False, having visited nothing,
	 * where fold() would read more than its record of the changes (the
	 * table in full, or what the baseline's state can't tell it): the
	 * caller folds instead.  Needs an active snapshot.
	 */
	bool		(*changes) (Relation rel, const ProllyTreeEnv *env,
							const ProllyTableFold *baseline,
							ProllyTreeVisitChange visit, void *arg);
} ProllyChangeFinderRoutine;

#define PROLLY_CHANGES_CALLBACK_NAMES "fold", "walk", "counters", "fold_rows", \
	"state_refs", "advance", "fold_on_leave", "changes"
#define PROLLY_CHANGES_FIRST_CALLBACK fold
#define PROLLY_CHANGES_LAST_CALLBACK changes

static inline const ProllyChangeFinderRoutine *
prolly_changes_lookup(const char *name)
{
	return (const ProllyChangeFinderRoutine *)
		prolly_get_impl(PROLLY_IFACE_CHANGE_FINDER, name);
}

#endif							/* PROLLY_CHANGES_H */
