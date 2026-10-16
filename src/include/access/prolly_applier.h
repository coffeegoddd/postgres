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
 * - begin() opens rel for writing, if the user may insert, update and
 *   delete its rows; end() finishes the writes.
 * - insert() adds a row.  update() replaces a row equal to old with new,
 *   and remove() deletes one; both find old by the table's primary key, or
 *   by every column if it has none, and return false if there's no such
 *   row.
 * - Rows are written with index maintenance and the checks of NOT NULL,
 *   CHECK and unique constraints, but fire no user triggers and recheck no
 *   foreign keys, as logical replication's apply does.
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
