/*-------------------------------------------------------------------------
 *
 * prolly_changelog.h
 *	  Change log interface: a prolly table's row changes, recorded as they
 *	  are written, for folds to read.
 *
 * Heap reports each tuple version a write to a prolly table made or
 * replaced; the change log records it as an entry the same transaction
 * writes, so a snapshot sees the entry exactly when it sees the write.  A
 * fold reads the entries its snapshot sees instead of looking for what
 * changed, and once a fold that read them is the table's baseline they are
 * consumed.
 *
 * An entry is a heap tuple of the table's row type and a count delta.  For
 * a table with a primary key (checked immediately), the tuple holds the
 * version's key columns (every other column null) and the delta is 0; for
 * one without, the version's whole row, +1 for a version made and -1 for
 * one replaced.  Its columns are read by the table's descriptor now, as
 * heap reads an old tuple: a column added since reads as its default.  An
 * entry may instead say the table must be read in full (a row it can't
 * hold); then it has no tuple.
 *
 * Contract:
 * - create() makes rel's log; called once, as rel (a new prolly table) is
 *   made, in the transaction that makes it.  Its log is versioned with it:
 *   each branch's copy of rel has its own, shared as the table is when the
 *   copy is made, and it is dropped with rel.
 * - note() is told each tuple version a write to rel made (newtid) or
 *   replaced (oldtid), or a speculative insertion took back (oldtid), after
 *   the write, by the (sub)transaction that wrote it, whether or not the
 *   write is the session's own (hushed writes of a version-control call
 *   too).  Either TID may be NULL; both NULL, every row may have changed
 *   (the table was truncated, or rewritten).  It ignores relations that
 *   aren't prolly tables with a log.  Errors are the write's.
 * - identity() is rel's log's identity now: 0 if rel has none, and another
 *   value whenever the log is made anew (not when a branch's copy of it is
 *   made, which has its entries), so entries a baseline was folded with
 *   are known to still be there.
 * - read() visits the entries of rel's log the snapshot sees, in no
 *   particular order: a visit returns false to stop.  False if rel has no
 *   log, an entry says to read the table in full, or a visit stopped.
 * - reset() empties rel's log, and leaves an entry to read the table in
 *   full: for a table whose rows were replaced without writes to note
 *   (its storage made anew) where no other transaction sees its branch, so
 *   the entries before are of rows it no longer has.
 * - is_log() says whether rel is a change log: its rows are no table's, and
 *   logical decoding passes over them.  Asked of any TOAST-kind relation,
 *   in critical sections too (RelationIsLogicallyLogged), so it is quick
 *   and must neither error nor allocate.
 * - consume() deletes the entries of rel's log the snapshot sees: called
 *   once a fold that read the log by this snapshot is to be the table's
 *   baseline (if the transaction aborts, they come back with the old
 *   baseline).  Entries consumed meanwhile by another transaction are let
 *   be.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_changelog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_CHANGELOG_H
#define PROLLY_CHANGELOG_H

#include "access/htup.h"
#include "access/prolly_registry.h"
#include "storage/itemptr.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"

/*
 * One entry: its tuple (in memory valid only during the visit) and its
 * count delta.
 */
typedef bool (*ProllyChangeLogVisit) (HeapTuple entry, int32 delta, void *arg);

typedef struct ProllyChangeLogRoutine
{
	ProllyImplInfo info;

	void		(*create) (Relation rel);
	void		(*note) (Relation rel, const ItemPointerData *oldtid,
						 const ItemPointerData *newtid);
	uint64		(*identity) (Relation rel);
	bool		(*read) (Relation rel, Snapshot snapshot,
						 ProllyChangeLogVisit visit, void *arg);
	void		(*consume) (Relation rel, Snapshot snapshot);
	bool		(*is_log) (Relation rel);
	void		(*reset) (Relation rel);
} ProllyChangeLogRoutine;

#define PROLLY_CHANGELOG_CALLBACK_NAMES \
	"create", "note", "identity", "read", "consume", "is_log", "reset"
#define PROLLY_CHANGELOG_FIRST_CALLBACK create
#define PROLLY_CHANGELOG_LAST_CALLBACK reset

static inline const ProllyChangeLogRoutine *
prolly_changelog_lookup(const char *name)
{
	return (const ProllyChangeLogRoutine *)
		prolly_get_impl(PROLLY_IFACE_CHANGE_LOG, name);
}

#endif							/* PROLLY_CHANGELOG_H */
