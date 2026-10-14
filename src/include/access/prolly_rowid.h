/*-------------------------------------------------------------------------
 *
 * prolly_rowid.h
 *	  Row identity interface: TIDs <-> rows.
 *
 * Postgres passes rows around by ItemPointer (index scans, triggers, EPQ,
 * CURRENT OF, ctid), and other index AMs store TIDs.  Prolly rows are
 * identified by key.  Implementations: TIDs derived from row content with
 * a map back to the row ("hashed"), or wide row ids.
 *
 * A row version is named by its content hash and its ordinal among
 * identical rows (always 0 for a table keyed by primary key; 0..count-1
 * for a keyless table's identical rows), so a TID doesn't depend on how
 * the table is keyed.  The map also records the row's current key.
 *
 * Contract:
 * - One TID names one row version: the same (content, ordinal) gets the
 *   same TID for as long as the map holds it, and different versions get
 *   different TIDs.  TIDs are valid heap-style item pointers (a valid
 *   block number, an offset from 1 to MaxHeapTuplesPerPage), because
 *   index AMs and TID bitmaps assume they are.
 * - tid_assign() records the version, if it isn't already, and returns its
 *   TID, tagging its writes with cid; it sees every write of the
 *   transaction, including cid's own; tid_find() returns, without writing, the TID tid_assign() gave a
 *   version (every row in a table has been given one when it was written).
 *   tid_resolve() finds what a TID names, whether or not that version
 *   still exists in the table: callers check the row itself.
 * - tid_rename() records that a version's content hash (and maybe its
 *   ordinal) changed while it stayed the same row (its bytes were
 *   re-encoded, as after DROP COLUMN): it keeps its TID, which now names
 *   the new version.
 * - A TID is not reused for another version until tid_forget() removes it,
 *   which only happens when no index can still hold it (VACUUM).
 * - The map lives in the relation's PROLLY_TREE_ROWID tree, through the
 *   engine, so it is transactional and per branch like the data.
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

typedef struct ProllyRowIdRoutine
{
	ProllyImplInfo info;

	void		(*tid_assign) (ProllyRootSet *roots, Relation rel,
							   const ProllyHash *content, uint64 ordinal,
							   const ProllyKey *key, CommandId cid,
							   ItemPointer tid);

	ProllyRowIdScan *(*scan_begin) (ProllyRootSet *roots, Relation rel,
									CommandId cid);
	/* false if the implementation can tell the version has no TID. */
	bool		(*tid_find) (ProllyRowIdScan *scan, const ProllyHash *content,
							 uint64 ordinal, ItemPointer tid);
	void		(*scan_end) (ProllyRowIdScan *scan);

	/* Appends the row's key to key_out; false if tid is unknown. */
	bool		(*tid_resolve) (ProllyRootSet *roots, Relation rel,
								ItemPointer tid, CommandId cid,
								StringInfo key_out, ProllyHash *content,
								uint64 *ordinal);

	void		(*tid_rename) (ProllyRootSet *roots, Relation rel,
							   const ProllyHash *old_content, uint64 old_ordinal,
							   const ProllyHash *new_content, uint64 new_ordinal,
							   const ProllyKey *key, CommandId cid);

	void		(*tid_forget) (Relation rel, ItemPointer tid, CommandId cid);
} ProllyRowIdRoutine;

#define PROLLY_ROWID_CALLBACK_NAMES \
	"tid_assign", "scan_begin", "tid_find", "scan_end", "tid_resolve", \
	"tid_rename", "tid_forget"
#define PROLLY_ROWID_FIRST_CALLBACK tid_assign

static inline const ProllyRowIdRoutine *
prolly_rowid_lookup(const char *name)
{
	return (const ProllyRowIdRoutine *)
		prolly_get_impl(PROLLY_IFACE_ROW_IDENTITY, name);
}

#endif							/* PROLLY_ROWID_H */
