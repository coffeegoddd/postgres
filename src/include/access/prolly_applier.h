/*-------------------------------------------------------------------------
 *
 * prolly_applier.h
 *	  Row applier interface: writing rows into a table as changes made
 *	  elsewhere (another commit, another branch), not as a statement does.
 *
 * Moving a branch's working set to a commit (reset, fast-forward, merge,
 * checkout of a commit) applies the rows that differ, one at a time.
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
 *   old's bytes, and return false if there's no such row.
 * - Rows are written with index maintenance and the checks of NOT NULL,
 *   CHECK and unique and exclusion constraints (deferrable ones when they
 *   are due), but fire no user triggers and check no foreign keys, as
 *   logical replication's apply does.  A materialized view's rows are
 *   written with index maintenance, as its refresh writes them.  Nothing
 *   else in the session changes.
 * - Each write is visible to the next.
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
						   TupleTableSlot *row);
	bool		(*remove) (ProllyApplier *a, TupleTableSlot *old);
	void		(*end) (ProllyApplier *a);
} ProllyApplierRoutine;

#define PROLLY_APPLIER_CALLBACK_NAMES \
	"begin", "insert", "update", "remove", "end"
#define PROLLY_APPLIER_FIRST_CALLBACK begin

static inline const ProllyApplierRoutine *
prolly_applier_lookup(const char *name)
{
	return (const ProllyApplierRoutine *)
		prolly_get_impl(PROLLY_IFACE_ROW_APPLIER, name);
}

#endif							/* PROLLY_APPLIER_H */
