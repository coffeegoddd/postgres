/*-------------------------------------------------------------------------
 *
 * branch_ops.h
 *	  The branch lifecycle: a branch of the database's repository made, with
 *	  a working set of its own.
 *
 * A branch is a branch of version control (prolly/engine/vc.h) and a
 * catalog branch (prolly/branch_catalog.h), linked (prolly/branch_link.h).
 * Its working set is the catalog branch's rows and tables.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/branch_ops.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_BRANCH_OPS_H
#define PROLLY_BRANCH_OPS_H

#include "prolly/engine/prolly.h"

typedef struct BranchOpsRoutine
{
	const char *name;

	/*
	 * A branch made at from's head, from's working set being its head: its
	 * catalog rows copied, and each of its relations sharing from's pages
	 * copy-on-write, so it costs what from has in objects, not rows.  It
	 * shows exactly what from had committed at one point: from's writers are
	 * waited out and held off while it's made, and capture has each of its
	 * tables from there.  The catalog branch made, which is linked to it.
	 * In a transaction that has written nothing, to be committed with it; an
	 * error if capture hasn't got there within timeout_ms.
	 */
	int32		(*create) (ProllyCtx *ctx, const char *name, const char *from, int timeout_ms);
} BranchOpsRoutine;

extern PGDLLIMPORT const BranchOpsRoutine SharedBranchOps;

/* The implementation in use. */
extern PGDLLIMPORT const BranchOpsRoutine *BranchOps;

#endif							/* PROLLY_BRANCH_OPS_H */
