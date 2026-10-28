/*-------------------------------------------------------------------------
 *
 * prolly_history.h
 *	  A table's rows at each commit of the head's history
 *	  (dolt_history_table()) or at one ref (dolt_at()), and the scan that
 *	  reads only those its primary key's quals bound: see
 *	  prolly_history_scan.c.
 *
 * src/include/access/prolly_history.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_HISTORY_H
#define PROLLY_HISTORY_H

#include "access/prolly_merge.h"
#include "nodes/execnodes.h"
#include "nodes/pathnodes.h"
#include "parser/parsetree.h"

/*
 * relid's table's rows at each commit the head reaches, with the commit,
 * into rsinfo's store as dolt_history_table() gives them; with bounds
 * (nbounds of them), only those whose primary keys any of them bounds.
 */
extern void prolly_history_rows(Oid relid, const ProllyKeyBounds *bounds,
								int nbounds, ReturnSetInfo *rsinfo);

/*
 * relid's table's rows at ref whose primary keys any of bounds[] (nbounds
 * of them) bounds, into rsinfo's store as dolt_at() gives them: a commit's
 * data tree sought for each bound where it can be, else read whole.
 */
extern void prolly_at_rows(const char *ref, Oid relid,
						   const ProllyKeyBounds *bounds, int nbounds,
						   ReturnSetInfo *rsinfo);

/* Is rte dolt_history_table(tbl)'s, or dolt_at(ref, tbl)'s? */
extern bool prolly_history_rte(RangeTblEntry *rte);

/* Its paths beside the function scan's: one seeking the trees, if any. */
extern void prolly_history_set_paths(PlannerInfo *root, RelOptInfo *rel,
									 RangeTblEntry *rte);

#endif							/* PROLLY_HISTORY_H */
