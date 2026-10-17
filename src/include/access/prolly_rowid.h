/*-------------------------------------------------------------------------
 *
 * prolly_rowid.h
 *	  Row identity interface: TIDs <-> rows.
 *
 * Postgres passes rows around by ItemPointer (index scans, triggers, EPQ,
 * CURRENT OF, ctid), and other index AMs store TIDs.  Prolly rows are
 * identified by key.  Implementations: TIDs from a per-table counter with
 * a map both ways ("counter"), or wide row ids.
 *
 * A row version is a row key and its ordinal among identical rows (always
 * 0 for a table keyed by primary key; 0..count-1 for a keyless table's
 * identical rows); the map also records its content hash, so callers can
 * tell whether the row at the key is still that version.
 *
 * Contract:
 * - One TID names one row version, and a TID is never given to a second
 *   version: not after its version is deleted or updated, and not after
 *   the transaction that was given it aborts, because index entries made
 *   for it outlive the abort.  That holds for every branch of the table,
 *   since an index serves them all.  TIDs are valid heap-style item
 *   pointers (a valid block number, an offset from 1 to
 *   MaxHeapTuplesPerPage), because index AMs and TID bitmaps assume so.
 * - tid_assign() records a new version and returns its TID, tagging its
 *   writes with cid; it sees every write of the transaction, cid's own
 *   included, and returns the existing TID if the version already has one.
 *   tid_find() returns, without writing, the TID of the version at a key
 *   (every row has been given one when it was written).
 * - tid_resolve() finds what a TID names: a version that may still exist
 *   in the table (callers check the row itself), or one that is gone.
 * - tid_retire() records that the version a TID names is gone, deleted or
 *   (with a successor) updated to the version the successor TID names, by
 *   command cid; resolving it then says so, which is how an update chain
 *   is followed (like heap's t_ctid).  The TID itself stays taken, and so
 *   does the gone version's row (as the table keying encoded it), until
 *   VACUUM: like heap's dead tuples, it is what SnapshotAny returns.
 * - tid_renew() gives the version a TID names a new TID, written by the
 *   current transaction and command, and retires the old one with the
 *   new one as its successor: an update that leaves the row as it was
 *   still makes a new version, as heap does, so the command (and others
 *   waiting for it) can tell the row was changed.
 * - tid_rename() records that the row a TID names is now another version
 *   that is the same row: its bytes were re-encoded (after DROP COLUMN),
 *   the table was re-keyed, or one of identical rows took another's
 *   ordinal.  It keeps its TID.
 * - The map records, as the version's local metadata, the transaction that
 *   wrote it (xmin): tid_assign() records the current transaction for a
 *   new version; renaming or retiring it keeps it.
 * - dead_tids() returns the TIDs whose versions were retired by
 *   transactions that precede horizon: no snapshot can see them any more.
 * - A TID is forgotten (tid_forget()) only when no index can still hold
 *   it (VACUUM removed other index AMs' entries for it); its map entries
 *   go with the current transaction, and an implementation may give the
 *   TID out again at once (the entries a reuse writes replace them) --
 *   unless another root may hold it: a commit, or another branch, made
 *   since the TID was given out (the refs' epoch tells when those are
 *   made), still has the version with that TID.
 * - The map lives in the relation's PROLLY_TREE_ROWID and
 *   PROLLY_TREE_ROWKEY trees, through the engine, so it is transactional
 *   and per branch like the data; whatever makes TIDs unique across aborts
 *   is not.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_rowid.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_ROWID_H
#define PROLLY_ROWID_H

#include "access/prolly_registry.h"
#include "access/prolly_types.h"
#include "lib/stringinfo.h"
#include "storage/itemptr.h"
#include "utils/relcache.h"

/* A scan's view of a relation's map, for many tid_find() calls. */
typedef struct ProllyRowIdScan ProllyRowIdScan;

typedef enum ProllyTidState
{
	PROLLY_TID_UNKNOWN,			/* never assigned */
	PROLLY_TID_NAMED,			/* names a version: key, content, ordinal */
	PROLLY_TID_GONE,			/* its version is gone: see ProllyTidInfo */
} ProllyTidState;

/* Local metadata of the version a TID names, and what became of it. */
typedef struct ProllyTidInfo
{
	TransactionId xmin;			/* the transaction that wrote the version */
	ItemPointerData successor;	/* gone: invalid if deleted */
	CommandId	cid;			/* gone: the command that retired it */
} ProllyTidInfo;

typedef struct ProllyRowIdRoutine
{
	ProllyImplInfo info;

	void		(*tid_assign) (ProllyRootSet *roots, Relation rel,
							   const ProllyHash *content, uint64 ordinal,
							   const ProllyKey *key, CommandId cid,
							   ItemPointer tid);

	ProllyRowIdScan *(*scan_begin) (ProllyRootSet *roots, Relation rel,
									CommandId cid);
	/* false if the version at key has no TID. */
	bool		(*tid_find) (ProllyRowIdScan *scan, const ProllyKey *key,
							 uint64 ordinal, ItemPointer tid);
	void		(*scan_end) (ProllyRowIdScan *scan);

	/*
	 * For PROLLY_TID_NAMED, appends the row's key to out and sets content
	 * and ordinal; for PROLLY_TID_GONE, appends the gone version's row to
	 * out; for both, sets *info.
	 */
	ProllyTidState (*tid_resolve) (ProllyRootSet *roots, Relation rel,
								   ItemPointer tid, CommandId cid,
								   StringInfo out, ProllyHash *content,
								   uint64 *ordinal, ProllyTidInfo *info);

	void		(*tid_retire) (ProllyRootSet *roots, Relation rel,
							   ItemPointer tid, ItemPointer successor,
							   const ProllyValue *row, CommandId cid);

	void		(*tid_renew) (ProllyRootSet *roots, Relation rel,
							  ItemPointer otid, const ProllyValue *row,
							  CommandId cid, ItemPointer ntid);

	void		(*tid_rename) (ProllyRootSet *roots, Relation rel,
							   ItemPointer tid, const ProllyHash *content,
							   uint64 ordinal, const ProllyKey *key,
							   CommandId cid);

	int			(*dead_tids) (ProllyRootSet *roots, Relation rel,
							  TransactionId horizon, ItemPointer *tids);
	void		(*tid_forget) (Relation rel, const ItemPointerData *tids,
							   int ntids, CommandId cid);

	/*
	 * New storage at to replaces rel's (TRUNCATE, a rewrite): TIDs given out
	 * from it must not be any rel has given out, as other branches and
	 * commits keep rows with those.
	 */
	void		(*counter_carry) (Relation rel, const RelFileLocator *to);
} ProllyRowIdRoutine;

#define PROLLY_ROWID_CALLBACK_NAMES \
	"tid_assign", "scan_begin", "tid_find", "scan_end", "tid_resolve", \
	"tid_retire", "tid_renew", "tid_rename", "dead_tids", "tid_forget", \
	"counter_carry"
#define PROLLY_ROWID_FIRST_CALLBACK tid_assign
#define PROLLY_ROWID_LAST_CALLBACK counter_carry

static inline const ProllyRowIdRoutine *
prolly_rowid_lookup(const char *name)
{
	return (const ProllyRowIdRoutine *)
		prolly_get_impl(PROLLY_IFACE_ROW_IDENTITY, name);
}

#endif							/* PROLLY_ROWID_H */
