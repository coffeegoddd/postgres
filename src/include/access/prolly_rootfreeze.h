/*-------------------------------------------------------------------------
 *
 * prolly_rootfreeze.h
 *	  Root freezer interface: a kept commit's pages made independent of the
 *	  transactions that wrote them.
 *
 * A commit's pages are kept by roots of each relation's block-mapped
 * storage (bmap_root_capture()).  Captured as they are, a heap page holds
 * tuple versions with their transactions' ids: versions deleted before the
 * commit, versions of transactions that hadn't committed then, and live
 * ones whose ids the database forgets once it freezes past them.  Such a
 * root can be given back only while those ids' status is known, and only
 * to a snapshot that judges them as the commit did.  A frozen root holds
 * instead each heap page as of the commit: the versions the commit's
 * snapshot sees, their xmin frozen and xmax cleared, every other version
 * dead (never unused: an index entry may point at it, and the root's index
 * pages are kept as they are), a HOT chain whose root version is gone kept
 * reachable through a redirect.  Any session can be given such pages, and
 * they never go stale.
 *
 * A root captured while other transactions write the relation (under
 * dolt.transaction_commit, whose every transaction commits, a writer is
 * nearly always at work) holds their versions too, of transactions the
 * commit didn't see and that may commit later.  It is never given back as
 * it is: it is kept with the commit's snapshot (ProllyRootSnapshot) and
 * frozen as of it, later, before the database forgets the ids it holds.
 *
 * Contract:
 * - at_capture() says whether a root is frozen as it is captured, inside
 *   the commit (its cost the commit's), or later: captured among the
 *   writers, kept unfrozen with the commit's snapshot, and frozen by the
 *   fold worker or by a move to the commit (freeze_later(),
 *   freeze_now()).
 * - freeze_page() makes 'page', a copy of block 'blkno' of a heap-format
 *   relation (a table, a TOAST table, a change log), the page as of
 *   'as_of', in place:
 *   only versions whose inserting transaction the snapshot sees committed
 *   (or one of the committing transaction's own ids), and that no
 *   transaction it sees committed (or one of those) deleted, are left,
 *   with xmin frozen, xmax invalid and no update chain; the others are
 *   dead line pointers, but for a HOT chain's root, which redirects to the
 *   version left.  Line pointers are never made unused, nor moved.  The
 *   page's prune hint is cleared, and its all-visible flag set if no dead
 *   line pointer is left (every version left is frozen, so the page is
 *   all-visible and all-frozen: a restore of the root sets its visibility
 *   map bits from the flag), cleared otherwise.  Returns whether any
 *   version was left.  *undecided says whether the page as frozen depends
 *   on a transaction the snapshot doesn't see that hasn't aborted (running
 *   then, or begun since, and perhaps committed now): a later snapshot may
 *   see it, so the same page frozen as of that one may differ.
 * - freeze_later() says whether a root captured as it is, writers at
 *   work, is frozen later, in the background, before the database forgets
 *   the ids it holds; asked as such a root is about to be captured, it
 *   has the background freezing told one is coming.  False: no root is
 *   captured so, and a commit keeps a root only while no writer is at
 *   work.
 * - freeze_now() says whether a move to a commit whose root isn't frozen
 *   yet freezes it then (O(the pages changed since the relation's last
 *   frozen root)), rather than going to the nearest frozen root before
 *   it and writing the rows between.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_rootfreeze.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_ROOTFREEZE_H
#define PROLLY_ROOTFREEZE_H

#include "access/prolly_registry.h"
#include "storage/bufpage.h"
#include "utils/snapshot.h"

/*
 * What a root is frozen as of: the commit's MVCC snapshot (of the other
 * transactions), and the committing transaction's own ids (its top-level
 * one and its committed subtransactions'), which commit with it.  'own'
 * NULL (nown 0): the current transaction's, a root frozen inside its
 * commit.
 */
typedef struct ProllyRootSnapshot
{
	Snapshot	snapshot;
	int			nown;
	TransactionId *own;
} ProllyRootSnapshot;

typedef struct ProllyRootFreezerRoutine
{
	ProllyImplInfo info;

	bool		(*at_capture) (void);
	bool		(*freeze_page) (Page page, BlockNumber blkno,
								const ProllyRootSnapshot *as_of,
								bool *undecided);
	bool		(*freeze_later) (void);
	bool		(*freeze_now) (void);
} ProllyRootFreezerRoutine;

#define PROLLY_ROOTFREEZER_CALLBACK_NAMES \
	"at_capture", "freeze_page", "freeze_later", "freeze_now"
#define PROLLY_ROOTFREEZER_FIRST_CALLBACK at_capture
#define PROLLY_ROOTFREEZER_LAST_CALLBACK freeze_now

/*
 * A heap-format page frozen as of as_of, as freeze_page() describes (the
 * built-in freezers' common rule).
 */
extern bool prolly_freeze_heap_page(Page page, BlockNumber blkno,
									const ProllyRootSnapshot *as_of,
									bool *undecided);


#endif							/* PROLLY_ROOTFREEZE_H */
