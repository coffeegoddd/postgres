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
 * - create() makes rel's log, if it can (it may make none, as pg_upgrade
 *   restores rel: identity() then says so); called as rel (a new prolly
 *   table) is made, or as a table that has none is altered under a lock
 *   writers wait for, in that transaction.  Its log is versioned with it:
 *   each branch's copy of rel has its own, shared as the table is when the
 *   copy is made, and it is dropped with rel.
 * - note() is told each tuple version a write to rel made (newtid) or
 *   replaced (oldtid), or a speculative insertion took back (oldtid), after
 *   the write, by the (sub)transaction that wrote it, whether or not the
 *   write is the session's own (hushed writes of a version-control call
 *   too).  Either TID may be NULL; both NULL, every row may have changed
 *   (the table was truncated, or rewritten).  It ignores relations that
 *   aren't prolly tables with a log.  Errors are the write's.
 * - note_made() is note() told of the versions a write made in hand
 *   (nmade of them, each as heap stored it, its TID in t_self) and of the
 *   one it replaced, if any (oldtid): an insertion of many rows is noted at
 *   once, and its entries written together.  Optional: without it, each is
 *   told to note().
 * - identity() is rel's log's identity now: 0 if rel has none, and another
 *   value whenever the log is made anew (not when a branch's copy of it is
 *   made, which has its entries), so entries a baseline was folded with
 *   are known to still be there.
 * - read() visits the entries of rel's log the snapshot sees, in no
 *   particular order: a visit returns false to stop.  False if rel has no
 *   log, an entry says to read the table in full, or a visit stopped.
 * - relid() is the relation holding the log of the prolly table relid, if
 *   the log is one (InvalidOid if not): it takes the table's owner,
 *   tablespace and autovacuum settings as its TOAST table does.  Asked by
 *   autovacuum with only the table's pg_class row, so it opens nothing.
 *   Optional.
 * - is_log() says whether rel is a change log: its rows are no table's, and
 *   logical decoding passes over them.  Asked of any TOAST-kind relation,
 *   in critical sections too (RelationIsLogicallyLogged), so it is quick
 *   and must neither error nor allocate.
 * - consume() deletes the entries of rel's log the snapshot sees: called
 *   once a fold that read the log by this snapshot is to be the table's
 *   baseline (if the transaction aborts, they come back with the old
 *   baseline).  Entries consumed meanwhile by another transaction are let
 *   be.
 * - written_unseen() (optional): whether a transaction the snapshot doesn't
 *   see, committed, wrote or consumed an entry of rel's log: whether it
 *   changed the table's rows, as far as the log tells (an entry no longer
 *   in the log was consumed by a transaction the snapshot sees, or one
 *   that wrote it is seen too).
 * - consume_own() (optional) deletes the entries of rel's log the current
 *   transaction wrote and didn't abort: called once a baseline holding
 *   exactly its own changes, applied to the last, is to be the table's.
 *   False, deleting none, if the log can't tell them all (the caller then
 *   keeps the old baseline).
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
	Oid			(*relid) (Oid relid);
	void		(*note_made) (Relation rel, const ItemPointerData *oldtid,
							  HeapTuple *made, int nmade);
	bool		(*consume_own) (Relation rel);
	bool		(*written_unseen) (Relation rel, Snapshot snapshot);
} ProllyChangeLogRoutine;

#define PROLLY_CHANGELOG_CALLBACK_NAMES \
	"create", "note", "identity", "read", "consume", "is_log", \
	"relid", "note_made", "consume_own", "written_unseen"
#define PROLLY_CHANGELOG_FIRST_CALLBACK create
#define PROLLY_CHANGELOG_LAST_CALLBACK written_unseen

static inline const ProllyChangeLogRoutine *
prolly_changelog_lookup(const char *name)
{
	return (const ProllyChangeLogRoutine *)
		prolly_get_impl(PROLLY_IFACE_CHANGE_LOG, name);
}

#endif							/* PROLLY_CHANGELOG_H */
