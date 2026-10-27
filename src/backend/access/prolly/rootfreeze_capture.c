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
 * entry pointing at the root finds it.
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
#include "access/transam.h"
#include "access/xact.h"
#include "storage/bufpage.h"
#include "utils/snapmgr.h"

/* Does 'snapshot' see transaction 'xid' committed (or is it this one)? */
static bool
committed_seen(TransactionId xid, bool hinted_committed, Snapshot snapshot)
{
	if (!TransactionIdIsNormal(xid))
		return xid == FrozenTransactionId || xid == BootstrapTransactionId;
	if (TransactionIdIsCurrentTransactionId(xid))
		return true;
	if (XidInMVCCSnapshot(xid, snapshot))
		return false;
	return hinted_committed || TransactionIdDidCommit(xid);
}

/* Is the version left: inserted, and not deleted, as the snapshot sees? */
static bool
version_left(HeapTupleHeader tup, Snapshot snapshot)
{
	TransactionId xmax;

	if (!HeapTupleHeaderXminFrozen(tup))
	{
		if (HeapTupleHeaderXminInvalid(tup) ||
			!committed_seen(HeapTupleHeaderGetRawXmin(tup),
							HeapTupleHeaderXminCommitted(tup), snapshot))
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
						   snapshot);
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

static bool
capture_freeze_page(Page page, BlockNumber blkno, Snapshot snapshot)
{
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	bool		left[MaxHeapTuplesPerPage + 1] = {0};
	bool		any = false;

	if (PageIsNew(page) || PageIsEmpty(page))
		return false;
	Assert(maxoff <= MaxHeapTuplesPerPage);

	/* (each version judged before any line pointer changes) */
	for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId		lp = PageGetItemId(page, off);

		if (ItemIdIsNormal(lp))
			left[off] = version_left((HeapTupleHeader) PageGetItem(page, lp),
									 snapshot);
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
	PageClearAllVisible(page);
	PageClearFull(page);
	((PageHeader) page)->pd_prune_xid = InvalidTransactionId;
	return any;
}

static bool
capture_at_capture(void)
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
};
