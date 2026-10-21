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
 *   (the table was truncated, or rewritten).  oldtup, if not NULL, is the
 *   version at oldtid in hand (the writer still has its page pinned), so it
 *   needn't be read again.  It ignores relations that aren't prolly tables
 *   with a log.  Errors are the write's.
 * - note_emptied() (optional) is told rel was emptied (truncated) by the
 *   (sub)transaction: a fold that sees it starts from no rows, the writes
 *   after it applied.  Without it, note() is told every row may have
 *   changed.
 * - note_made() is note() told of the versions a write made in hand
 *   (nmade of them, each as heap stored it, its TID in t_self) and of the
 *   one it replaced, if any (oldtid): an insertion of many rows is noted at
 *   once, and its entries written together.  Optional: without it, each is
 *   told to note().
 * - identity() is rel's log's identity now: 0 if rel has none, and another
 *   value whenever the log is made anew (not when a branch's copy of it is
 *   made, which has its entries), so entries a baseline was folded with
 *   are known to still be there.
 * - read() visits the entries of rel's log the snapshot sees and the
 *   position since doesn't hold (NULL: every one), in no particular order:
 *   a visit returns false to stop.  An entry NULL says the table was
 *   emptied (truncated) by its transaction: a visit takes its other
 *   entries, of before and after, as rows to look up, not to apply.  False
 *   if rel has no log, an entry says to read the table in full, or a visit
 *   stopped.  The transaction's own entries are among them.  An entry is
 *   read whether or not a fold since consumed it: since may be an older
 *   snapshot's fold's, which didn't.
 * - relid() is the relation holding the log of the prolly table relid, if
 *   the log is one (InvalidOid if not): it takes the table's owner,
 *   tablespace and autovacuum settings as its TOAST table does.  Asked by
 *   autovacuum with only the table's pg_class row, so it opens nothing.
 *   Optional.
 * - is_log() says whether rel is a change log: its rows are no table's, and
 *   logical decoding passes over them.  Asked of any TOAST-kind relation,
 *   in critical sections too (RelationIsLogicallyLogged), so it is quick
 *   and must neither error nor allocate.
 * - consume() is called once a fold that read the log by this snapshot is
 *   to be the table's baseline: the transaction's own entries are marked
 *   consumed (the next fold reads its transaction's again, but those), and
 *   those of transactions every
 *   snapshot sees finished that this one saw (trimmed); the others it read
 *   are behind the baseline's position.  If the transaction aborts, they
 *   come back with the old baseline.
 * - written_unseen() (optional): whether a transaction the snapshot doesn't
 *   see, committed, wrote or consumed an entry of rel's log: whether it
 *   changed the table's rows, as far as the log tells (an entry no longer
 *   in the log was consumed by a transaction the snapshot sees, or one
 *   that wrote it is seen too).
 * - read_unseen() (optional) visits the entries a fold at position was
 *   held (of transactions it saw finished, of its own those it consumed)
 *   that the snapshot doesn't see, of transactions committed: for a
 *   snapshot older than that fold to undo them.  False if one says to read
 *   the table in full, or a visit stopped.  Entries are kept for it:
 *   trimmed only below OldestXmin.  *consumed is set if one it visited was
 *   consumed: kept only while a snapshot may not see its consuming
 *   transaction finished, so a position that doesn't hold it (this
 *   snapshot's) can't count on finding it later.  Without it, such a
 *   snapshot's fold reads the table in full.
 * - flush() (optional) writes the entries the transaction holds unwritten
 *   to the logs of the session's branch: called before the session's
 *   branch changes in the transaction (a log's OID is every branch's, its
 *   rows each branch's own).
 * - consume_own() (optional), given the position of the baseline being
 *   advanced, deletes the entries of rel's log the current
 *   transaction wrote and didn't abort: called once a baseline holding
 *   exactly its own changes, applied to the last, is to be the table's.
 *   False, deleting none, if the log can't tell them all, or the
 *   position's own transaction wrote entries since its fold (a position
 *   has one own, which the current transaction's would replace): the
 *   caller then keeps the old baseline.  Where the log holds others'
 *   entries since that position the active snapshot sees (changes the new
 *   baseline doesn't hold either), *rest is the position the new baseline
 *   holds, which still reads them: that one's xmax, with those of its
 *   running that wrote such entries or are running still, and the current
 *   transaction its own.  Else rest->xmax is invalid, and the new baseline
 *   holds what the snapshot sees.
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
#include "access/transam.h"
#include "storage/itemptr.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"

/*
 * A fold's position in a log: the transactions whose entries it holds are
 * those below xmax but those it saw running (xip), and of its own (own,
 * which may have written more since) those its fold consumed.
 */
typedef struct ProllyLogPosition
{
	FullTransactionId xmax;
	FullTransactionId own;		/* invalid: none */
	uint32		nxip;
	const FullTransactionId *xip;
} ProllyLogPosition;

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
						 HeapTuple oldtup, const ItemPointerData *newtid);
	uint64		(*identity) (Relation rel);
	bool		(*read) (Relation rel, Snapshot snapshot,
						 const ProllyLogPosition *since,
						 ProllyChangeLogVisit visit, void *arg);
	void		(*consume) (Relation rel, Snapshot snapshot);
	bool		(*is_log) (Relation rel);
	Oid			(*relid) (Oid relid);
	void		(*note_made) (Relation rel, const ItemPointerData *oldtid,
							  HeapTuple oldtup, HeapTuple *made, int nmade);
	bool		(*consume_own) (Relation rel, const ProllyLogPosition *since,
								ProllyLogPosition *rest);
	bool		(*written_unseen) (Relation rel, Snapshot snapshot);
	bool		(*read_unseen) (Relation rel, Snapshot snapshot,
								const ProllyLogPosition *was,
								ProllyChangeLogVisit visit, void *arg,
								bool *consumed);
	void		(*flush) (void);
	void		(*note_emptied) (Relation rel);
} ProllyChangeLogRoutine;

#define PROLLY_CHANGELOG_CALLBACK_NAMES \
	"create", "note", "identity", "read", "consume", "is_log", \
	"relid", "note_made", "consume_own", "written_unseen", "read_unseen", \
	"flush", "note_emptied"
#define PROLLY_CHANGELOG_FIRST_CALLBACK create
#define PROLLY_CHANGELOG_LAST_CALLBACK note_emptied

static inline const ProllyChangeLogRoutine *
prolly_changelog_lookup(const char *name)
{
	return (const ProllyChangeLogRoutine *)
		prolly_get_impl(PROLLY_IFACE_CHANGE_LOG, name);
}

#endif							/* PROLLY_CHANGELOG_H */
