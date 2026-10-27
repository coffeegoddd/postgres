/*-------------------------------------------------------------------------
 *
 * prolly_log.h
 *	  dolt_log(ref)'s rows as a stream, and the scan that reads them so:
 *	  see prolly_log_scan.c.
 *
 * src/include/access/prolly_log.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_LOG_H
#define PROLLY_LOG_H

#include "nodes/pathnodes.h"
#include "parser/parsetree.h"

typedef struct ProllyLogStream ProllyLogStream;

/*
 * The commits ref reaches, in dolt_log's order, given one at a time, each
 * read as it is found: the first n cost about n commits read.  NULL (an
 * empty stream) for HEAD before a first commit.
 */
extern ProllyLogStream *prolly_log_begin(const char *ref);

/* dolt_log's next row into values and nulls (6 of each); false at the end. */
extern bool prolly_log_next(ProllyLogStream *stream, Datum *values, bool *nulls);

/* Is rte dolt_log(ref)'s? */
extern bool prolly_log_rte(RangeTblEntry *rte);

/*
 * Its path, in place of the function scan's: a scan giving each row as it
 * is found; false, adding none, where the scan can't compute the ref first.
 */
extern bool prolly_log_set_paths(PlannerInfo *root, RelOptInfo *rel,
								 RangeTblEntry *rte);

#endif							/* PROLLY_LOG_H */
