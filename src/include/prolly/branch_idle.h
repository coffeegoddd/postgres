/*-------------------------------------------------------------------------
 *
 * branch_idle.h
 *	  Giving back the working sets of branches gone unused.
 *
 * With branch_idle_timeout set, each database with a versioned schema is
 * looked at in turn by a worker capture's launcher starts, one at a time:
 * each of its branches with a working set, but "main", has it given back
 * once it has gone unused for that long (prolly/branch_ops.h's
 * dematerialize_idle), and is left as it is if it has changes not
 * committed, or is a default_branch.  A database is looked at no more often
 * than every branch_idle_timeout, or every minute if that's less, so a
 * working set goes up to that, and a look of the launcher's, after its time.
 * What came before the server started isn't kept: a branch used only before
 * then has gone unused since the start.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/branch_idle.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_BRANCH_IDLE_H
#define PROLLY_BRANCH_IDLE_H

#include "nodes/pg_list.h"

/* How long a branch goes unused before its working set is given back, in ms; 0 is never. */
extern PGDLLIMPORT int branch_idle_timeout;

/* In capture's launcher, at each look: the databases with versioned schemas, one looked at if due. */
extern void BranchIdleLook(List *dbs);

pg_noreturn extern void BranchIdleWorkerMain(Datum main_arg);

#endif							/* PROLLY_BRANCH_IDLE_H */
