/*-------------------------------------------------------------------------
 *
 * branch_checkout.h
 *	  A session on a branch: checking one out, the database's default, and
 *	  whether others are on one.
 *
 * A session checks a branch out by its name (prolly/branch_link.h), which
 * puts the session on its catalog branch (catalog_branch, which discards
 * what was resolved from schema names; see prolly/branch_catalog.h) and
 * holds the branch for the session, so that what would take it from under
 * the session (renaming it, deleting it, or giving back its working set) can
 * see that it's in use.  No row is read or written.  A checkout is undone
 * with the transaction or subtransaction it's in, the hold with it.
 *
 * A new session checks out default_branch, if it's set: for the database
 * (ALTER DATABASE ... SET default_branch), and for a connection, which may
 * name another.
 *
 * Setting catalog_branch itself puts a session on a catalog branch and holds
 * nothing: it's for the engine's own work, and its tests.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/branch_checkout.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_BRANCH_CHECKOUT_H
#define PROLLY_BRANCH_CHECKOUT_H

#include "storage/shmem.h"

/* The branch a new session checks out, or "" for none. */
extern PGDLLIMPORT char *default_branch;

typedef struct BranchCheckoutRoutine
{
	const char *name;

	/*
	 * The session put on a branch, by name.  Refused in a transaction that
	 * has written to a versioned schema's tables, or changed one's objects,
	 * on whichever branch: those changes would be left on the branch it's
	 * leaving, as uncommitted as the transaction is.
	 */
	void		(*checkout) (const char *name);

	/* The branch the session has checked out, palloc'd, or NULL. */
	char	   *(*current) (void);

	/*
	 * Whether a session other than this one has a catalog branch checked
	 * out; if none has, none can until this transaction ends, which is what's
	 * for a change that takes the branch from them.
	 */
	bool		(*in_use) (int32 branch);
} BranchCheckoutRoutine;

extern PGDLLIMPORT const BranchCheckoutRoutine SessionBranchCheckout;

/* The implementation in use. */
extern PGDLLIMPORT const BranchCheckoutRoutine *BranchCheckout;

extern PGDLLIMPORT const ShmemCallbacks BranchCheckoutShmemCallbacks;

/* At a session's start, in its first transaction: default_branch checked out. */
extern void BranchCheckoutSessionStart(void);

#endif							/* PROLLY_BRANCH_CHECKOUT_H */
