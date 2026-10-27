/*-------------------------------------------------------------------------
 *
 * rootretain_window.c
 *	  Root retention "window": every commit in the history window keeps
 *	  its root, none outside it, under an optional cap.
 *
 * The history window (the history window interface's window(): each
 * branch lineage's by its settings, by time, by count, or both) is the
 * history kept here; outside it is cold, for a sever.  So a kept commit
 * outside every lineage's window is retired (one a branch reset away from,
 * or replaced by a commit on the same parent, may be another branch's
 * head), and one inside is kept, at the
 * root interval's granularity (prolly.root_interval and
 * prolly.root_interval_commits decide which commits are kept at all).
 * With prolly.history_root_max_size set, as the database has it, and the
 * roots of every branch holding more, the oldest in the window are
 * retired too, until they don't: a move to one of those commits then
 * restores the nearest root before it and writes the rows they differ by.
 * A root holding nothing the next doesn't is passed over (retiring it
 * frees nothing).  A tag's commit's root, and a branch's newest, are never
 * retired.
 *
 * Passes: the windows are evaluated once a pass, which walks them, so a
 * pass is due only once the branch has kept half as many commits again
 * as it keeps (or the cap is passed): the walk's cost is spread over
 * those keeps, and what lies outside the window is retired within that
 * many.
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
#include "utils/hsearch.h"
#include "utils/memutils.h"

typedef struct WindowPass
{
	HTAB	   *own;			/* the branch's lineage's window's commits */
	HTAB	   *any;			/* and every lineage's */
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

/* A hash as a key: its bytes past its length zero. */
static void
hash_key(const ProllyHash *h, ProllyHash *key)
{
	memset(key, 0, sizeof(*key));
	key->len = h->len;
	memcpy(key->bytes, h->bytes, h->len);
}

static bool
note_in(const ProllyHash *commit, void *arg)
{
	ProllyHash	key;
	bool		found;

	hash_key(commit, &key);
	(void) hash_search((HTAB *) arg, &key, HASH_ENTER, &found);
	return true;
}

static HTAB *
window_set(const ProllyWindowRoutine *w, const char *branch)
{
	HASHCTL		ctl;
	HTAB	   *set;

	ctl.keysize = sizeof(ProllyHash);
	ctl.entrysize = sizeof(ProllyHash);
	ctl.hcxt = CurrentMemoryContext;
	set = hash_create("prolly root retention window", 256, &ctl,
					  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	PROLLY_CALL(w, window, branch, note_in, set);
	return set;
}

static bool
window_has(HTAB *set, const ProllyHash *commit)
{
	ProllyHash	key;

	if (set == NULL)
		return true;
	hash_key(commit, &key);
	return hash_search(set, &key, HASH_FIND, NULL) != NULL;
}

/*
 * A pass reads the branch's own lineage's window, which a pass ends at,
 * and every lineage's: a branch's kept commits may be another's too (a
 * branch made at one, or reset away from it), each kept while any window
 * holds it, and passed over while only another's does.  A commit no
 * branch has any more (reset or amended away from) is in none.  With no
 * window of its own, as by default, the branch keeps everything: a move
 * back to a commit it was reset away from is O(1) too.
 */
static void *
window_begin(const char *branch, int64 excess)
{
	ProllyDatabaseComponents dc;
	WindowPass *wp = palloc0(sizeof(WindowPass));
	ProllyWindowPolicy p;

	wp->excess = excess;
	prolly_database_components(&dc);
	if (dc.history_window == NULL || !PROLLY_HAS(dc.history_window, window) ||
		!PROLLY_HAS(dc.history_window, policy))
		return wp;
	PROLLY_CALL(dc.history_window, policy, branch, &p);
	if (p.keep_time <= 0 && p.keep_commits <= 0 && p.max_bytes <= 0)
		return wp;
	wp->own = window_set(dc.history_window, branch);
	wp->any = window_set(dc.history_window, NULL);
	return wp;
}

static ProllyRootVerdict
window_judge(void *pass, const ProllyRetainedRoot *root)
{
	WindowPass *wp = pass;
	bool		own = window_has(wp->own, &root->commit);
	bool		any = own || window_has(wp->any, &root->commit);
	bool		kept = root->newest || root->tagged;

	if (!any)
		return kept ? PROLLY_ROOT_PASS : PROLLY_ROOT_RETIRE;
	if (!own)
		return PROLLY_ROOT_PASS;
	/* (from the newest, only what is out of the window: a cap thins the oldest) */
	if (root->from_newest || wp->excess <= 0)
		return PROLLY_ROOT_STOP;
	if (kept || root->bytes <= 0)
		return PROLLY_ROOT_PASS;
	wp->excess -= root->bytes;
	return PROLLY_ROOT_RETIRE;
}

static void
window_end(void *pass)
{
	WindowPass *wp = pass;

	if (wp->own)
		hash_destroy(wp->own);
	if (wp->any)
		hash_destroy(wp->any);
	pfree(wp);
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
