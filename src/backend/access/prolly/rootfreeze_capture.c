/*-------------------------------------------------------------------------
 *
 * rootfreeze_capture.c
 *	  Root freezer "capture": a root frozen as it is captured, inside the
 *	  commit.
 *
 * The commit's keep has locked its tables against writers, so the pages
 * hold only versions of transactions that ended, and the committing
 * transaction's own.  Each heap-format page the root captures afresh (one
 * changed since the relation's last root) is frozen as of the commit's
 * snapshot (freeze_page()), and the root holds the frozen copy, the
 * relation keeping its page as it is (bmap_root_capture_frozen()).  The
 * cost is the commit's: a page's freezing and its image, for each page
 * changed since the last root, as a capture's images of those pages were.
 *
 * Freezing a page as of a snapshot is the heap's visibility, judged once:
 * a version is left if the snapshot sees its inserter committed and no
 * deleter that it sees committed (the current transaction counts as
 * committed: the snapshot is its own commit's).  A version left is frozen
 * (xmin frozen, xmax invalid, its update chain cut: its updater, which
 * the snapshot doesn't see, made no version left).  A version not left is
 * a dead line pointer, never an unused one: the root's index pages are
 * kept as they are, and an index entry may still point at it; a later
 * VACUUM of a relation made of the root removes both as it would any dead
 * version.  A HOT chain's root version that isn't left, with a version of
 * the chain that is, redirects to it, as pruning would, so that an index
 * entry pointing at the root finds it.  The page is then defragmented, as
 * pruning leaves it, so the dead versions' bytes neither stay in the root
 * nor go into its image.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/rootfreeze_capture.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/prolly_builtins.h"
#include "storage/procarray.h"
#include "access/transam.h"
#include "access/xact.h"
#include "storage/bufpage.h"
#include "utils/snapmgr.h"

/* Is xid one of the committing transaction's own (as_of's)? */
static bool
own_xid(TransactionId xid, const ProllyRootSnapshot *as_of)
{
	if (as_of->own == NULL)
		return TransactionIdIsCurrentTransactionId(xid);
	for (int i = 0; i < as_of->nown; i++)
		if (TransactionIdEquals(as_of->own[i], xid))
			return true;
	return false;
}

/*
 * Does as_of see transaction 'xid' committed (or is it one of the
 * committing transaction's own)?  One it doesn't see that hasn't aborted
 * makes the page *undecided: a later snapshot may see it.
 */
static bool
committed_seen(TransactionId xid, bool hinted_committed,
			   const ProllyRootSnapshot *as_of, bool *undecided)
{
	if (!TransactionIdIsNormal(xid))
		return xid == FrozenTransactionId || xid == BootstrapTransactionId;
	if (own_xid(xid, as_of))
		return true;
	if (XidInMVCCSnapshot(xid, as_of->snapshot))
	{
		if (TransactionIdIsInProgress(xid) || TransactionIdDidCommit(xid))
			*undecided = true;
		return false;
	}
	return hinted_committed || TransactionIdDidCommit(xid);
}

/* Is the version left: inserted, and not deleted, as the snapshot sees? */
static bool
version_left(HeapTupleHeader tup, const ProllyRootSnapshot *as_of,
			 bool *undecided)
{
	TransactionId xmax;

	if (!HeapTupleHeaderXminFrozen(tup))
	{
		if (HeapTupleHeaderXminInvalid(tup) ||
			!committed_seen(HeapTupleHeaderGetRawXmin(tup),
							HeapTupleHeaderXminCommitted(tup), as_of,
							undecided))
			return false;
	}
	if ((tup->t_infomask & HEAP_XMAX_INVALID) ||
		HEAP_XMAX_IS_LOCKED_ONLY(tup->t_infomask))
		return true;
	xmax = HeapTupleHeaderGetUpdateXid(tup);
	if (!TransactionIdIsValid(xmax))
		return true;
	return !committed_seen(xmax,
						   !(tup->t_infomask & HEAP_XMAX_IS_MULTI) &&
						   (tup->t_infomask & HEAP_XMAX_COMMITTED),
						   as_of, undecided);
}

/*
 * The first version left of the HOT chain from offset 'off' (that one
 * included), following each version's update while it stays on the page
 * and the next was made by its updater: InvalidOffsetNumber if none.
 */
static OffsetNumber
chain_left(Page page, OffsetNumber off, const bool *left)
{
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	TransactionId prior = InvalidTransactionId;

	while (off >= FirstOffsetNumber && off <= maxoff)
	{
		ItemId		lp = PageGetItemId(page, off);
		HeapTupleHeader tup;

		if (!ItemIdIsNormal(lp))
			break;
		tup = (HeapTupleHeader) PageGetItem(page, lp);
		if (TransactionIdIsValid(prior) &&
			!TransactionIdEquals(prior, HeapTupleHeaderGetRawXmin(tup)))
			break;
		if (left[off])
			return off;
		if (!HeapTupleHeaderIsHotUpdated(tup))
			break;
		prior = HeapTupleHeaderGetUpdateXid(tup);
		off = ItemPointerGetOffsetNumber(&tup->t_ctid);
	}
	return InvalidOffsetNumber;
}

bool
prolly_freeze_heap_page(Page page, BlockNumber blkno,
						const ProllyRootSnapshot *as_of, bool *undecided)
{
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	bool		left[MaxHeapTuplesPerPage + 1] = {0};
	bool		any = false;

	*undecided = false;
	if (PageIsNew(page))
		return false;
	if (PageIsEmpty(page))
	{
		PageSetAllVisible(page);
		return false;
	}
	Assert(maxoff <= MaxHeapTuplesPerPage);

	/* (each version judged before any line pointer changes) */
	for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId		lp = PageGetItemId(page, off);

		if (ItemIdIsNormal(lp))
			left[off] = version_left((HeapTupleHeader) PageGetItem(page, lp),
									 as_of, undecided);
	}

	/* chains' roots first: they redirect to a version found by its chain */
	for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId		lp = PageGetItemId(page, off);
		OffsetNumber to;

		if (ItemIdIsRedirected(lp))
		{
			to = chain_left(page, ItemIdGetRedirect(lp), left);
			if (OffsetNumberIsValid(to))
				ItemIdSetRedirect(lp, to);
			else
				ItemIdSetDead(lp);
		}
		else if (ItemIdIsNormal(lp) && !left[off] &&
				 !HeapTupleHeaderIsHeapOnly((HeapTupleHeader) PageGetItem(page, lp)))
		{
			to = chain_left(page, off, left);
			if (OffsetNumberIsValid(to))
				ItemIdSetRedirect(lp, to);
			else
				ItemIdSetDead(lp);
		}
	}

	/* then the versions: dead, or frozen */
	for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId		lp = PageGetItemId(page, off);
		HeapTupleHeader tup;

		if (!ItemIdIsNormal(lp))
			continue;
		if (!left[off])
		{
			ItemIdSetDead(lp);
			continue;
		}
		tup = (HeapTupleHeader) PageGetItem(page, lp);
		tup->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_XMIN_COMMITTED |
							 HEAP_XMIN_INVALID | HEAP_COMBOCID | HEAP_MOVED);
		tup->t_infomask |= HEAP_XMIN_FROZEN | HEAP_XMAX_INVALID;
		HeapTupleHeaderSetXmax(tup, InvalidTransactionId);
		tup->t_infomask2 &= ~(HEAP_HOT_UPDATED | HEAP_KEYS_UPDATED);
		ItemPointerSet(&tup->t_ctid, blkno, off);
		any = true;
	}
	/*
	 * (all-visible, and all-frozen, if no dead line pointer is left: every
	 * version left is frozen, and an index entry may point at a dead one,
	 * which an index-only scan must not take for a row)
	 */
	PageSetAllVisible(page);
	for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
		if (ItemIdIsDead(PageGetItemId(page, off)))
		{
			PageClearAllVisible(page);
			break;
		}
	PageClearFull(page);
	((PageHeader) page)->pd_prune_xid = InvalidTransactionId;

	/*
	 * The versions made dead keep no storage (their line pointers are dead,
	 * as pruning leaves them): the rest moved together, as pruning moves
	 * them, so that the page's hole, which its image leaves out, is all
	 * the free space.  A page whose rows all moved off is imaged as little
	 * more than its line pointers.
	 */
	PageRepairFragmentation(page);
	return any;
}

static bool
capture_at_capture(void)
{
	return true;
}

static bool
capture_freeze_page(Page page, BlockNumber blkno,
					const ProllyRootSnapshot *as_of, bool *undecided)
{
	return prolly_freeze_heap_page(page, blkno, as_of, undecided);
}

/* (a root not frozen at capture is kept only while no writer is at work) */
static bool
capture_freeze_later(void)
{
	return false;
}

/*
 * (its own roots are frozen as they are captured; one another freezer left
 * unfrozen, captured among writers, is frozen on a move to it all the
 * same: never given back as captured, and frozen it is O(1) to give back)
 */
static bool
capture_freeze_now(void)
{
	return true;
}

const ProllyRootFreezerRoutine prolly_rootfreeze_capture = {
	.info = {
		.iface = PROLLY_IFACE_ROOT_FREEZER,
		.impl_id = 1,
		.format_version = 1,
		.name = "capture",
		.description = "a root's heap pages frozen as of the commit as it is captured",
	},
	.at_capture = capture_at_capture,
	.freeze_page = capture_freeze_page,
	.freeze_later = capture_freeze_later,
	.freeze_now = capture_freeze_now,
};
