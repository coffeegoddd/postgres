/*-------------------------------------------------------------------------
 *
 * branch_ops.h
 *	  The branch lifecycle: a branch of the database's repository made, with
 *	  a working set of its own.
 *
 * A branch is a branch of version control (prolly/engine/vc.h) and, while
 * it's materialized, a catalog branch (prolly/branch_catalog.h), linked
 * (prolly/branch_link.h): its working set is the catalog branch's rows and
 * tables.  A branch with no catalog branch is only its head, which is
 * always its working set too.
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
	 * A branch made from another, from, at commit, or at from's head if that's NULL: only its head,
	 * as Dolt's is, whatever the commit, with no working set until it's materialized; 0.  With
	 * materialize, it's given one as it's made, as materialize would; its catalog branch.  With
	 * take_changes (and no commit), it's made at from's head with from's working set, changes and
	 * all, and from is reset to its head, from having a working set.  In a transaction that has
	 * written nothing, to be committed with it; an error if capture hasn't got there within
	 * timeout_ms.
	 */
	int32		(*create) (ProllyCtx *ctx, const char *name, const char *from, const PHash *commit,
						   bool take_changes, bool materialize, int timeout_ms);

	/*
	 * A branch given a working set at its head, if it has none: its catalog rows copied from a
	 * branch that has one, one at the same head if there is, and each of its relations sharing that
	 * one's pages copy-on-write, so it costs what that one has in objects, and then the rows that
	 * differ written on it.  It's of exactly what that one had committed at one point, its writers
	 * waited out and held off while it's made.  Two at once make one.  Its catalog branch.  As
	 * create is asked to be.
	 */
	int32		(*materialize) (ProllyCtx *ctx, const char *name, int timeout_ms);

	/*
	 * A branch's working set given back, its head kept: refused for "main", which keeps its own so
	 * that there's always one to make another from; while another session is on it, while it's a
	 * database's or role's default_branch, or while its working set isn't its head.  In a
	 * transaction that has written nothing, as create is asked to be.
	 */
	void		(*dematerialize) (ProllyCtx *ctx, const char *name, int timeout_ms);

	/*
	 * A branch's working set given back as dematerialize gives it back, if it has gone unused for
	 * idle_ms (prolly/branch_checkout.h's unused_for): true.  False where dematerialize would refuse,
	 * and while it has been used within idle_ms, its transaction then to be rolled back: one with
	 * changes not committed is noted used, so it isn't looked at again for as long.
	 */
	bool		(*dematerialize_idle) (ProllyCtx *ctx, const char *name, int idle_ms, int timeout_ms);

	/*
	 * A branch renamed, its head, working set and sessions kept: its name only.  Refused for "main"
	 * and onto "main", onto its own name, while it's a default_branch, while another session is on it
	 * unless forced, and while a branch has the new name unless forced, that one then deleted as a
	 * forced delete is (and so in a transaction that has written nothing, as delete is asked to be).
	 */
	void		(*rename) (ProllyCtx *ctx, const char *name, const char *to, bool force, int timeout_ms);

	/*
	 * A branch deleted: its working set, if it has one, given back as dematerialize does, changes
	 * and all, and its head.  Refused for "main", for the branch this session is on, and while it's
	 * a default_branch; unless forced, while another session is on it, and while its head isn't in
	 * the history of the branch this session is on (or "main", if none).  In a transaction that has
	 * written nothing, as it's done in one left out of capture; an error if capture hasn't got there
	 * within timeout_ms.
	 */
	void		(*remove) (ProllyCtx *ctx, const char *name, bool force, int timeout_ms);
} BranchOpsRoutine;

extern PGDLLIMPORT const BranchOpsRoutine SharedBranchOps;

/* The implementation in use. */
extern PGDLLIMPORT const BranchOpsRoutine *BranchOps;

#endif							/* PROLLY_BRANCH_OPS_H */
