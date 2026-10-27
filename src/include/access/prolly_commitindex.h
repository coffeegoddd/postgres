/*-------------------------------------------------------------------------
 *
 * prolly_commitindex.h
 *	  Commit index interface: a database's commits by date, and each
 *	  commit's date and place in its history, found without reading it.
 *
 * Questions of time ("the commit a branch was at as of T", "the commits
 * made in this hour", "the oldest commits a window of seven days keeps")
 * would otherwise walk history from a head, a commit read a step.  The
 * index answers them with a seek: each commit indexed is found by its date
 * (then id), and by its id, with its date, height (prolly_history_height())
 * and parents.  A commit is named by its content, so what the index says of
 * it never changes.
 *
 * Contract:
 * - note_head() indexes commit head, and every commit it reaches that
 *   isn't indexed yet, stopping at one that is or one the store hasn't: so
 *   an indexed commit's ancestors are indexed, as far as the store holds
 *   them.  Branch heads are noted as they move (by a commit, a merge, a
 *   reset, a fetch or a clone); every commit a branch head reached is
 *   indexed.  Changes are with the transaction, as the refs' are.
 * - lookup() gives commit id's entry; false if it isn't indexed.
 * - scan() visits the entries dated from..to (both included), in date
 *   order (then id) forward or backward, until visit returns false.
 * - forget() removes commit id's entry (its history kept elsewhere now).
 *   The caller holds every ref (the refs' lock()).
 * - Entries name no chunks: they keep nothing from a collection, so an
 *   entry may outlive its commit (one no ref reaches any more): lookup()
 *   says what it was, not that the store still has it.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_commitindex.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_COMMITINDEX_H
#define PROLLY_COMMITINDEX_H

#include "access/prolly_refs.h"
#include "access/prolly_registry.h"
#include "access/sdir.h"

typedef struct ProllyCommitIndexEntry
{
	ProllyHash	id;
	TimestampTz committed_at;
	int64		height;
	int			nparents;
	ProllyHash	parents[PROLLY_MAX_PARENTS];
} ProllyCommitIndexEntry;

/* Visits an entry; false stops the scan.  The entry is good for the visit. */
typedef bool (*ProllyCommitIndexVisit) (const ProllyCommitIndexEntry *entry,
										void *arg);

typedef struct ProllyCommitIndexRoutine
{
	ProllyImplInfo info;

	void		(*note_head) (const ProllyHash *head);
	bool		(*lookup) (const ProllyHash *id, ProllyCommitIndexEntry *entry);
	void		(*scan) (TimestampTz from, TimestampTz to,
						 ScanDirection direction,
						 ProllyCommitIndexVisit visit, void *arg);
	void		(*forget) (const ProllyHash *id);
} ProllyCommitIndexRoutine;

#define PROLLY_COMMITINDEX_CALLBACK_NAMES \
	"note_head", "lookup", "scan", "forget"
#define PROLLY_COMMITINDEX_FIRST_CALLBACK note_head
#define PROLLY_COMMITINDEX_LAST_CALLBACK forget

static inline const ProllyCommitIndexRoutine *
prolly_commitindex_lookup(const char *name)
{
	return (const ProllyCommitIndexRoutine *)
		prolly_get_impl(PROLLY_IFACE_COMMIT_INDEX, name);
}

#endif							/* PROLLY_COMMITINDEX_H */
