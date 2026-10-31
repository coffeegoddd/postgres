/*-------------------------------------------------------------------------
 *
 * branch_catalog.h
 *	  Branched catalogs: versioned schemas, whose catalog rows belong to
 *	  one branch each, beside shared schemas that every branch sees.
 *
 * A versioned schema has one pg_namespace row per branch, all with the
 * same name, and the row's nspbranch says which branch it belongs to.
 * Every object made in it is a row of its own in the other catalogs, under
 * object ids of its own, so a branch's objects never share an object id
 * with another branch's, and lookups by object id need no branch.  A
 * shared schema's row has nspbranch 0.
 *
 * A session's branch is the catalog_branch setting (0: none).  Schema
 * names resolve to the session's branch's row first and then to a shared
 * one, so a session on no branch cannot name a versioned schema at all.
 *
 * A name belongs to shared schemas or to versioned ones, never to both:
 * a shared schema cannot be made while any branch has a versioned schema
 * of that name, nor a versioned one while a shared one exists.
 *
 * Branch identifiers are allocated per database by a BranchIdRoutine.
 * This file knows only the identifiers; what a branch is called, and what
 * it holds besides its catalog rows, is kept elsewhere.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/branch_catalog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_BRANCH_CATALOG_H
#define PROLLY_BRANCH_CATALOG_H

#include "access/htup.h"

/* The branch whose catalog rows this session sees (0: none). */
extern PGDLLIMPORT int catalog_branch;

extern void assign_catalog_branch(int newval, void *extra);

/*
 * Allocates branch identifiers: positive numbers, each handed out once in
 * the database it was allocated in.
 *
 * allocate returns a new identifier.  It is not transactional: an
 * identifier allocated by a transaction that aborts is not handed out
 * again, so nothing that kept it can be confused with a later branch.
 *
 * allocated says whether an identifier has been handed out in this
 * database (and so whether rows may be made on that branch).
 */
typedef struct BranchIdRoutine
{
	int32		(*allocate) (void);
	bool		(*allocated) (int32 branch);
} BranchIdRoutine;

/*
 * The implementation in use: one sequence per database, made with the
 * database (see SequenceBranchIds).
 */
extern PGDLLIMPORT const BranchIdRoutine *BranchIds;
extern PGDLLIMPORT const BranchIdRoutine SequenceBranchIds;

/* The namespace named nspname this session sees: its branch's, else shared. */
extern HeapTuple SearchNamespaceByName(const char *nspname);
extern Oid	NamespaceOidByName(const char *nspname);

/* The branch a namespace belongs to (0: shared, or no such namespace). */
extern int32 NamespaceBranch(Oid nspid);

/*
 * The branch a function runs on, whatever the caller's, given its pg_proc
 * row; -1 if it runs on the caller's.  A function in a versioned schema
 * runs on its schema's branch.  A shared security definer function runs on
 * no branch, unless it sets catalog_branch itself: what its names mean is
 * its owner's choice, never its caller's.
 */
extern int32 FunctionBranch(HeapTuple proctup);

/*
 * Errors out unless a schema named nspname may be made (or renamed to) on
 * branch (0: shared), and locks the name until the transaction ends, so
 * that a concurrent transaction cannot make it the other kind.
 */
extern void NamespaceCheckNameFree(const char *nspname, int32 branch);

/* The branch a new versioned schema goes on: the session's, if it may. */
extern int32 VersionedNamespaceBranch(const char *nspname);

#endif							/* PROLLY_BRANCH_CATALOG_H */
