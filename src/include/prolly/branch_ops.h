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
	 * A branch made from another, from: its catalog rows copied, and each of its relations sharing
	 * from's pages copy-on-write, so it costs what from has in objects and not rows, and then the
	 * rows that differ from what it's to have written on it.  It's made at commit, or at from's head
	 * if that's NULL, its working set that commit's, from's uncommitted changes left on from.  With
	 * take_changes (and no commit), it's made at from's head with from's working set, changes and
	 * all, and from is reset to its head.  It's of exactly what from had committed at one point:
	 * from's writers are waited out and held off while it's made, and capture has each table from
	 * there.  The catalog branch made, which is linked to it.  In a transaction that has written
	 * nothing, to be committed with it; an error if capture hasn't got there within timeout_ms.
	 */
	int32		(*create) (ProllyCtx *ctx, const char *name, const char *from, const PHash *commit,
						   bool take_changes, int timeout_ms);
} BranchOpsRoutine;

extern PGDLLIMPORT const BranchOpsRoutine SharedBranchOps;

/* The implementation in use. */
extern PGDLLIMPORT const BranchOpsRoutine *BranchOps;

#endif							/* PROLLY_BRANCH_OPS_H */
