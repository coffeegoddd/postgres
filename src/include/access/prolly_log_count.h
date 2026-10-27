/*-------------------------------------------------------------------------
 *
 * prolly_log_count.h
 *	  count(*) of dolt_log(ref), taken from the commit's count.
 *
 * src/include/access/prolly_log_count.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_LOG_COUNT_H
#define PROLLY_LOG_COUNT_H

#include "nodes/pathnodes.h"

/*
 * The rows dolt_log(ref) gives: the commits ref's commit reaches, as the
 * refs keep them (PROLLY_REFS_SPACE_COMMITS), while no history is
 * severed; counted by a walk of the log otherwise (a commit they keep none
 * for, a history horizon).  0 for HEAD before a first commit.
 */
extern int64 prolly_log_count(const char *ref);

/*
 * For a query whose grouping is count(*) of dolt_log(ref) alone (no
 * GROUP BY, HAVING or quals): a path giving the count, by
 * prolly_log_count(), in place of reading the log.
 */
extern void prolly_log_count_paths(PlannerInfo *root, RelOptInfo *input_rel,
								   RelOptInfo *grouped_rel);

#endif							/* PROLLY_LOG_COUNT_H */
