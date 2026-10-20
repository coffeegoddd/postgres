/*-------------------------------------------------------------------------
 *
 * prolly_fold.c
 *	  What the change finders share about folding a table in full: why a
 *	  fold read every row, counted and logged (prolly_fold.h).
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_fold.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_fold.h"
#include "pgstat.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

bool		debug_prolly_force_full_fold = false;
int			prolly_log_full_folds = -1;

static const char *const reason_names[PROLLY_FULL_REASONS] = {
	[PROLLY_FULL_NONE] = "none",
	[PROLLY_FULL_NO_BASELINE] = "no_baseline",
	[PROLLY_FULL_SHAPE_CHANGED] = "shape_changed",
	[PROLLY_FULL_NO_LOG] = "no_log",
	[PROLLY_FULL_LOG_CHANGED] = "log_changed",
	[PROLLY_FULL_STALE_SNAPSHOT] = "stale_snapshot",
	[PROLLY_FULL_UNSETTLED] = "unsettled",
	[PROLLY_FULL_STORAGE_UNSEEN] = "storage_unseen",
	[PROLLY_FULL_RECOVERY] = "recovery",
	[PROLLY_FULL_LOG_ENTRY] = "log_entry",
	[PROLLY_FULL_TOO_MANY] = "too_many",
	[PROLLY_FULL_KEYING_CHANGED] = "keying_changed",
	[PROLLY_FULL_UNSUPPORTED_KEY] = "unsupported_key",
	[PROLLY_FULL_FORCED] = "forced",
};

StaticAssertDecl(PGSTAT_PROLLY_FOLD_FULL_FORCED - PGSTAT_PROLLY_FOLD_FULL_NO_BASELINE ==
				 PROLLY_FULL_FORCED - PROLLY_FULL_NO_BASELINE,
				 "a counter for each full fold's reason, in the reasons' order");

const char *
prolly_full_fold_reason_name(ProllyFullFoldReason reason)
{
	if ((int) reason < 0 || reason >= PROLLY_FULL_REASONS)
		return "unknown";
	return reason_names[reason];
}

void
prolly_full_fold_noted(Relation rel, ProllyFullFoldReason reason, int64 rows,
					   double ms)
{
	if (reason == PROLLY_FULL_NONE)
		return;
	pgstat_count_prolly(PGSTAT_PROLLY_FOLD_FULL_NO_BASELINE +
						(reason - PROLLY_FULL_NO_BASELINE), 1);
	if (prolly_log_full_folds >= 0 && rows >= prolly_log_full_folds)
		ereport(LOG,
				(errmsg("prolly folded table \"%s.%s\" in full: %s, %lld rows read in %.0f ms",
						get_namespace_name(RelationGetNamespace(rel)),
						RelationGetRelationName(rel),
						prolly_full_fold_reason_name(reason),
						(long long) rows, ms)));
}
