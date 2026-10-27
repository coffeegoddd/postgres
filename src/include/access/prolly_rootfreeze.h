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
 * Contract:
 * - at_capture() says whether a root is frozen as it is captured, inside
 *   the commit (its cost the commit's), or later.
 * - freeze_page() makes 'page', a copy of block 'blkno' of a heap-format
 *   relation (a table, a TOAST table, a change log), the page as of
 *   'snapshot', in place:
 *   only versions whose inserting transaction the snapshot sees committed
 *   (or the current transaction, whose commit the snapshot's commit is),
 *   and that no transaction it sees committed (or this one) deleted, are
 *   left, with xmin frozen, xmax invalid and no update chain; the others
 *   are dead line pointers, but for a HOT chain's root, which redirects to
 *   the version left.  Line pointers are never made unused, nor moved.
 *   The page's all-visible flag and prune hint are cleared.  Returns
 *   whether any version was left.
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

typedef struct ProllyRootFreezerRoutine
{
	ProllyImplInfo info;

	bool		(*at_capture) (void);
	bool		(*freeze_page) (Page page, BlockNumber blkno, Snapshot snapshot);
} ProllyRootFreezerRoutine;

#define PROLLY_ROOTFREEZER_CALLBACK_NAMES \
	"at_capture", "freeze_page"
#define PROLLY_ROOTFREEZER_FIRST_CALLBACK at_capture
#define PROLLY_ROOTFREEZER_LAST_CALLBACK freeze_page

#endif							/* PROLLY_ROOTFREEZE_H */
