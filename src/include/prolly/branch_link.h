/*-------------------------------------------------------------------------
 *
 * branch_link.h
 *	  Which catalog branch each branch of the database's repository is.
 *
 * A repository is a database's, and its branches are named, as Dolt's are;
 * a working set is the catalog rows and tables of one catalog branch, an
 * identifier allocated in this database (prolly/branch_catalog.h).  The link
 * between them is this database's own: what a commit holds, and what a
 * remote is sent, names neither the identifier nor the link.  One name is
 * one identifier, and the other way about.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/branch_link.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_BRANCH_LINK_H
#define PROLLY_BRANCH_LINK_H

#include "nodes/pg_list.h"

typedef struct BranchLinkRoutine
{
	const char *name;

	/* The catalog branch of a branch, or 0 if it has none. */
	int32		(*branch_of) (const char *name);

	/* The branch a catalog branch is, palloc'd, or NULL. */
	char	   *(*name_of) (int32 branch);

	/*
	 * A branch linked to a catalog branch, allocated in this database:
	 * ERRCODE_DUPLICATE_OBJECT if either is linked already.  Undone with the
	 * transaction.
	 */
	void		(*link) (const char *name, int32 branch);

	/* A branch's link gone; false if it had none. */
	bool		(*unlink) (const char *name);

	/* The name of every branch that has a catalog branch, palloc'd, as of now. */
	List	   *(*names) (void);
} BranchLinkRoutine;

extern PGDLLIMPORT const BranchLinkRoutine TableBranchLink;

/*
 * What reads a branch's head and writes one from a barrier's root holds this, of the branch's
 * catalog branch, from before its barrier to its transaction's end, so that no other writes the
 * head between: two, each with a root, would each write over what the other had.  It's a lock of
 * no row, so a transaction that has written nothing, as a barrier's is to be, can take it.
 */
extern void LockBranchHead(int32 branch);

/* The implementation in use. */
extern PGDLLIMPORT const BranchLinkRoutine *BranchLink;

#endif							/* PROLLY_BRANCH_LINK_H */
