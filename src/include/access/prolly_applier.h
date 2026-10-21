/*-------------------------------------------------------------------------
 *
 * prolly_applier.h
 *	  Row applier interface: writing rows into a table as changes made
 *	  elsewhere (another commit, another branch), not as a statement does.
 *
 * Moving a branch's working set to a commit (reset, fast-forward, merge,
 * checkout of a commit) applies the rows that differ, one at a time, those
 * it inserts many at once.
 *
 * Contract:
 * - begin() opens rel, a table with storage (a partition's rows are
 *   applied to the partition), for writing, if the user may select,
 *   insert, update and delete its rows, row-level security doesn't apply
 *   to them, and they may update and delete the rows of each table whose
 *   foreign keys reference rel, without row-level security there either;
 *   or a materialized view, if the user may refresh it.  end() finishes
 *   the writes.
 * - insert() adds a row.  update() replaces a row equal to old with new,
 *   and remove() deletes one; both find old by the table's primary key
 *   (deferrable or not), or, if it has none, a row whose every column has
 *   old's bytes, and return false if there's no such row.  update() gives
 *   each column keep marks (keep may be NULL: none) the value the row
 *   found has, as it is stored: a value TOAST stored out of line keeps its
 *   pointer, so it is neither read nor stored again.
 * - Rows are written with index maintenance and the checks of NOT NULL,
 *   CHECK and unique and exclusion constraints (deferrable ones when they
 *   are due), but fire no user triggers and check no foreign keys, as
 *   logical replication's apply does.  A materialized view's rows are
 *   written with index maintenance, as its refresh writes them.  Nothing
 *   else in the session changes.
 * - immediate() has the rows written after it checked against the
 *   deferrable unique and exclusion constraints as an immediate one checks
 *   them: insert() or update() of a row breaking one raises the error,
 *   whether the constraint is INITIALLY DEFERRED or SET CONSTRAINTS defers
 *   it.  (A caller that records the rows refused, writing each in a
 *   subtransaction, calls it.)  Calling it again changes nothing.
 * - Each write is visible to the next, and holds nothing (no buffer pin)
 *   once it returns, so writes may be made in subtransactions.
 * - insert_many() (optional; NULL: insert() each) adds n rows, each as
 *   insert() would, written together (a page's rows at once).  An
 *   implementation may leave a table it fills from nothing, new in the
 *   transaction (no other sees it), without index entries until the next
 *   write of another kind or end(), then build its indexes over the rows
 *   (#490): a row breaking a unique or exclusion constraint is then
 *   refused there, not as it is written.  That build is refused in a
 *   subtransaction begun since the rows were written (rolled back, it
 *   would leave them unindexed).  A load's failure ends the applier, even
 *   if the caller catches it: every later write but end() raises an error.
 * - remove_all() (optional; NULL: remove() each row) removes every row at
 *   once, as TRUNCATE does: the table given new, empty storage, under the
 *   ACCESS EXCLUSIVE lock that takes, firing no trigger and checking no
 *   foreign key.  It returns false, having removed nothing, if it can't:
 *   the session uses the table otherwise (rel is to be open once).  As
 *   TRUNCATE, it isn't MVCC-safe: a snapshot taken before it, once it
 *   commits, sees the table empty.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_applier.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_APPLIER_H
#define PROLLY_APPLIER_H

#include "access/prolly_registry.h"
#include "executor/tuptable.h"
#include "utils/relcache.h"

typedef struct ProllyApplier ProllyApplier;

typedef struct ProllyApplierRoutine
{
	ProllyImplInfo info;

	ProllyApplier *(*begin) (Relation rel);
	void		(*insert) (ProllyApplier *a, TupleTableSlot *row);
	bool		(*update) (ProllyApplier *a, TupleTableSlot *old,
						   TupleTableSlot *row, const bool *keep);
	bool		(*remove) (ProllyApplier *a, TupleTableSlot *old);
	void		(*immediate) (ProllyApplier *a);
	void		(*end) (ProllyApplier *a);
	void		(*insert_many) (ProllyApplier *a, TupleTableSlot **rows, int n);
	bool		(*remove_all) (ProllyApplier *a);
} ProllyApplierRoutine;

#define PROLLY_APPLIER_CALLBACK_NAMES \
	"begin", "insert", "update", "remove", "immediate", "end", "insert_many", \
	"remove_all"
#define PROLLY_APPLIER_FIRST_CALLBACK begin
#define PROLLY_APPLIER_LAST_CALLBACK remove_all

static inline const ProllyApplierRoutine *
prolly_applier_lookup(const char *name)
{
	return (const ProllyApplierRoutine *)
		prolly_get_impl(PROLLY_IFACE_ROW_APPLIER, name);
}

#endif							/* PROLLY_APPLIER_H */
