/*-------------------------------------------------------------------------
 *
 * branch_toast.h
 *	  Which TOAST relation a value stored out of line is in, on a branch.
 *
 * A value stored out of line is named by its TOAST relation's object id.  A
 * branch's copy of a table shares its TOAST relation's pages with the one it
 * was copied from, under an id of its own, so a value on a page the two share
 * names the source's.  The value is in the copy all the same: its pages are
 * shared copy-on-write with the heap's, so every value a branch's heap names
 * is in the branch's own TOAST relation, the source's through sharing and its
 * own written there.  So the id a value names tells which TOAST relation it's
 * of, and the branch's copy of that relation is where to read it, and where to
 * delete it, which leaves the source's pages as they were.
 *
 * Each copy is recorded with its source as it's made (pg_history.toast_copies),
 * so a value named by any relation a branch's copy came from, through branches
 * of branches, is found in the copy on the branch.  A copy dropped stops being
 * one, its own copies taking on its sources, so that its id, used again, isn't
 * taken for a copy.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/branch_toast.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_BRANCH_TOAST_H
#define PROLLY_BRANCH_TOAST_H

#include "utils/relcache.h"

typedef struct BranchToastRoutine
{
	const char *name;

	/* A TOAST relation shared as a copy of another, recorded, in the caller's transaction. */
	void		(*copied) (Oid copy, Oid source);

	/*
	 * A TOAST relation being dropped: what was copied of it has its sources
	 * as its own, and it's no copy any more.  For any TOAST relation, and any
	 * role dropping one.
	 */
	void		(*dropped) (Oid toastrelid);

	/*
	 * The TOAST relation to read a value named toastrelid from: the copy of
	 * it on the branch names resolve on, if it's another branch's; else
	 * itself.  An error if it's another branch's and that one has none.
	 */
	Oid			(*to_read) (Oid toastrelid);

	/* The TOAST relation of heap's to delete a value named toastrelid from: heap's own. */
	Oid			(*to_delete) (Relation heap, Oid toastrelid);
} BranchToastRoutine;

extern PGDLLIMPORT const BranchToastRoutine CopiesBranchToast;

/* The implementation in use. */
extern PGDLLIMPORT const BranchToastRoutine *BranchToast;

#endif							/* PROLLY_BRANCH_TOAST_H */
