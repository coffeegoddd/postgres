/*-------------------------------------------------------------------------
 *
 * rootretain_window.c
 *	  Root retention "window": every commit in its branch's history window
 *	  keeps its root, none outside it, under an optional cap.
 *
 * The history window is the history kept here, by each branch's settings
 * (prolly.history_window, a month by default, prolly.history_min_commits
 * and a branch's own); outside it is cold, for a sever.  Every branch has
 * one, so what a branch keeps is bounded by it.  A kept commit outside the
 * branch's window is retired, unless another branch's head is at it and
 * that branch keeps no root of it; one inside is kept, at the root
 * interval's granularity (prolly.root_interval and
 * prolly.root_interval_commits decide which commits are kept at all).
 * With prolly.history_root_max_size set, as the database has it, and the
 * roots of every branch holding more, the oldest in the window are
 * retired too, until they don't: a move to one of those commits then
 * restores the nearest root before it and writes the rows they differ by.
 * A root holding nothing the next doesn't is passed over (retiring it
 * frees nothing).  A tag's commit's root, and a branch's newest, are never
 * retired.
 *
 * Passes: each root is judged from what the refs give of it (its commit's
 * date, when it was kept, its rank among the branch's newest, whether its
 * history is severed, whether another branch's head is at it), with no
 * walk of the history nor of the branches; a pass is due once the branch
 * has kept half as many commits again as it keeps (or the cap is passed),
 * so its ranking of the branch's roots is spread over those keeps, and
 * what lies outside the window is retired within that many.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/rootretain_window.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"
#include "access/prolly_config.h"
#include "access/prolly_window.h"
#include "miscadmin.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

typedef struct WindowPass
{
	bool		windowed;		/* the window judged (else: the cap alone) */
	TimestampTz cut;			/* the branch's window's */
	int64		min_commits;	/* and its count of newest roots kept */
	int64		excess;			/* bytes over the cap still to retire */
} WindowPass;

static int64
window_max_bytes(void)
{
	return prolly_window_database_size("prolly.history_root_max_size");
}

static bool
window_due(const char *branch, int64 nlive, int64 keeps)
{
	return keeps >= Max(1, nlive / 2);
}

/*
 * A pass reads the branch's own window, which a pass ends at, from each
 * root as judged, without a walk of its history or of the branches: a
 * root is in it if its commit is dated within the window (now less its
 * window, the cut rounded down to its batch), or was kept within it (as
 * the branch's head moved there: a commit reset or amended away from
 * keeps its root, and a move back to it is O(1), until the move is older
 * than the window), or is among the branch's newest min_commits roots;
 * but never one on severed history (a parent past the horizon), whose
 * commit would hold that parent from collection.  Every branch has a
 * window (prolly.history_window, a month by default), so what a branch
 * keeps, and the refs' entries for it, are bounded by it.  Another
 * branch's commits are judged by seeks alone: one outside the branch's
 * window is kept while another branch's head is at it and that branch
 * keeps no root of it (one it keeps serves it), and retired otherwise.
 */
static void *
window_begin(const char *branch, int64 excess, bool windows)
{
	ProllyDatabaseComponents dc;
	WindowPass *wp = palloc0(sizeof(WindowPass));
	ProllyWindowPolicy p;

	wp->excess = excess;
	wp->cut = DT_NOBEGIN;
	prolly_database_components(&dc);
	if (!windows || dc.history_window == NULL ||
		!PROLLY_HAS(dc.history_window, policy))
		return wp;
	PROLLY_CALL(dc.history_window, policy, branch, &p);
	wp->cut = GetCurrentTimestamp() - p.keep_time;
	if (p.batch > 0)
		wp->cut -= ((wp->cut % p.batch) + p.batch) % p.batch;
	wp->min_commits = p.keep_commits;
	wp->windowed = true;
	return wp;
}

static ProllyRootVerdict
window_judge(void *pass, const ProllyRetainedRoot *root)
{
	WindowPass *wp = pass;
	bool		own = !wp->windowed ||
		(!root->on_severed &&
		 (root->committed_at >= wp->cut || root->kept_at >= wp->cut ||
		  root->younger < wp->min_commits));
	bool		kept = root->newest || root->tagged;

	/*
	 * (outside its window: kept while another branch's head is at it and
	 * that branch keeps no root of it, which would serve it)
	 */
	if (!own)
		return kept ? PROLLY_ROOT_PASS :
			root->head_elsewhere ? PROLLY_ROOT_RETIRE_IF_KEPT :
			PROLLY_ROOT_RETIRE;
	/* (from the newest, only what is out of the window: a cap thins the oldest) */
	if (root->from_newest || wp->excess <= 0)
		return PROLLY_ROOT_STOP;
	if (kept || root->bytes <= 0)
		return PROLLY_ROOT_PASS;
	wp->excess -= root->bytes;
	return PROLLY_ROOT_RETIRE;
}

static int64
window_end(void *pass)
{
	WindowPass *wp = pass;
	int64		left = wp->excess;

	pfree(wp);
	return left;
}

const ProllyRootRetentionRoutine prolly_rootretain_window = {
	.info = {
		.iface = PROLLY_IFACE_ROOT_RETENTION,
		.impl_id = 1,
		.format_version = 1,
		.name = "window",
		.description = "window: every commit in the history window keeps its root, under an optional cap",
	},
	.max_bytes = window_max_bytes,
	.due = window_due,
	.begin = window_begin,
	.judge = window_judge,
	.end = window_end,
};
