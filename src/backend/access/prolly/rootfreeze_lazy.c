/*-------------------------------------------------------------------------
 *
 * rootfreeze_lazy.c
 *	  Root freezer "lazy": roots captured while writers are at work, frozen
 *	  later, in the background.
 *
 * Under dolt.transaction_commit every transaction commits, so a writer is
 * nearly always at work, and a keep that waited for none would almost
 * never keep anything, while one that locked them out would stall them
 * all.  This freezer has a commit's keep capture its roots among the
 * writers (O(1) for each relation, at a moment no writer is changing one of
 * its pages), recording the commit's snapshot with them, and leaves the
 * freezing to the fold worker, which freezes the roots kept so, oldest
 * first, before the database forgets the transaction ids they hold
 * (a root that falls past that horizon is retired, never given back).  A
 * move to such a commit before then freezes its roots itself
 * (freeze_now()).  A page is frozen as the capture freezer freezes it
 * (prolly_freeze_heap_page()).
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/rootfreeze_lazy.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"

static bool
lazy_at_capture(void)
{
	return false;
}

static bool
lazy_freeze_page(Page page, BlockNumber blkno,
				 const ProllyRootSnapshot *as_of, bool *undecided)
{
	return prolly_freeze_heap_page(page, blkno, as_of, undecided);
}

/*
 * (the fold worker, which runs wherever branches are written, finds the
 * roots to freeze in the refs: nothing to tell it here)
 */
static bool
lazy_freeze_later(void)
{
	return true;
}

static bool
lazy_freeze_now(void)
{
	return true;
}

const ProllyRootFreezerRoutine prolly_rootfreeze_lazy = {
	.info = {
		.iface = PROLLY_IFACE_ROOT_FREEZER,
		.impl_id = 2,
		.format_version = 1,
		.name = "lazy",
		.description = "roots captured among writers, frozen later in the background",
	},
	.at_capture = lazy_at_capture,
	.freeze_page = lazy_freeze_page,
	.freeze_later = lazy_freeze_later,
	.freeze_now = lazy_freeze_now,
};
