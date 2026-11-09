/*-------------------------------------------------------------------------
 *
 * branch_checkout.h
 *	  A session on a branch: checking one out, the database's default, and
 *	  whether others are on one.
 *
 * A session checks a branch out by its name (prolly/branch_link.h), which
 * puts the session on its catalog branch (catalog_branch, which discards
 * what was resolved from schema names; see prolly/branch_catalog.h) and
 * notes it for the session, so that what would take it from under the
 * session (renaming it, deleting it, or giving back its working set) can see
 * that it's in use.  No row is read or written, unless the branch is only a
 * head, which is first given a working set (prolly/branch_ops.h's
 * materialize).  A checkout is undone with the transaction or subtransaction
 * it's in, as catalog_branch is.
 *
 * A new session checks out default_branch, if it's set: for the database
 * (ALTER DATABASE ... SET default_branch), and for a connection, which may
 * name another.
 *
 * Setting catalog_branch itself puts a session on a catalog branch, which is
 * noted too, but without what a checkout waits for: it's for the engine's
 * own work, and its tests.
 *
 * When each catalog branch was last used is kept too, for what gives back a
 * branch's working set once it has been unused for a while: when a session
 * last left it, or it was last given a working set, since the server started.
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

	/*
	 * Whether this session is on a catalog branch, or is to be on it once
	 * its transaction ends (a function that set another only for itself).
	 */
	bool		(*mine) (int32 branch);

	/* A catalog branch noted as used now. */
	void		(*used) (int32 branch);

	/*
	 * How long, in milliseconds, since a catalog branch was last used: since
	 * a session last left it or it was noted used, or since the server
	 * started, whichever is later; never less.  After in_use, which holds off
	 * what would come on, what has left is in it.
	 */
	int64		(*unused_for) (int32 branch);
} BranchCheckoutRoutine;

extern PGDLLIMPORT const BranchCheckoutRoutine SessionBranchCheckout;

/* The implementation in use. */
extern PGDLLIMPORT const BranchCheckoutRoutine *BranchCheckout;

extern PGDLLIMPORT const ShmemCallbacks BranchCheckoutShmemCallbacks;

/*
 * At a backend's start, in its first transaction: its slot kept from then until it exits, and for a
 * client's session, default_branch checked out.
 */
extern void BranchCheckoutBackendStart(bool client);

/* The catalog branch this backend is now on, as catalog_branch changes. */
extern void BranchCheckoutNote(int32 branch);

#endif							/* PROLLY_BRANCH_CHECKOUT_H */
