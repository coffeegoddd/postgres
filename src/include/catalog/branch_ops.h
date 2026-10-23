/*-------------------------------------------------------------------------
 *
 * branch_ops.h
 *	  Branch operations: making, deleting, checking out and listing a
 *	  database's branches.
 *
 * A branch has its own copy of every versioned object (one in a versioned
 * schema).  How the copies are made and kept is the implementation's
 * business: the one in branch_copies.c copies the source branch's catalog
 * rows and shares its relations' pages until either branch changes them.
 *
 * Contract:
 * - create and delete are transactional with the calling transaction: an
 *   aborted creation leaves no branch, an aborted deletion the branch.
 * - checkout changes the session's branch at once, and lasts until the
 *   next checkout or the end of the session.
 * - main can't be deleted, nor a branch any session is on.
 *
 * src/include/catalog/branch_ops.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BRANCH_OPS_H
#define BRANCH_OPS_H

/* Visits one branch, by name. */
typedef void (*BranchVisit) (const char *name, Oid branch, void *arg);

typedef struct BranchOpsRoutine
{
	const char *name;

	/*
	 * Make a branch called name, a copy of the branch start_point (NULL: the
	 * session's), and return its id.
	 */
	Oid			(*create) (const char *name, const char *start_point);

	/* Delete a branch with its copies. */
	void		(*drop) (const char *name);

	/*
	 * Call branch name newname, as deleting it and making one of the new
	 * name would check: refused if another session is on it, unless force
	 * (sessions on it stay on it, under its new name).
	 */
	void		(*rename) (const char *name, const char *newname, bool force);

	/*
	 * What create and drop check of the session, alone (a branch's name,
	 * the privileges they need, a transaction that can write): refused if
	 * the session may not make, or delete, a branch called name.
	 */
	void		(*check_create) (const char *name);
	void		(*check_drop) (const char *name);

	/* Whether check_create would take name as a branch's (its rules alone). */
	bool		(*name_ok) (const char *name);

	/*
	 * Make a branch as create does, without its checks of the session: the
	 * copies of one that exists already in all but them, which its first
	 * use makes.  The copies of the tables emptied names (qualified names,
	 * as the source branch has them; a name it hasn't, or of no plain
	 * table, passed over) begin empty instead, as TRUNCATE leaves a table:
	 * the table and its TOAST table given new storage, no page shared, its
	 * indexes built over no rows, and its change log told it was emptied.
	 * nowait: if a writer of start_point's is in the middle of changing it,
	 * nothing is made and InvalidOid returned, rather than waiting for the
	 * writer (which may be idle in its transaction for as long as it likes).
	 */
	Oid			(*make) (const char *name, const char *start_point,
						 List *emptied, bool nowait);

	/* Make name the session's branch. */
	void		(*checkout) (const char *name);

	/* The session's branch's name (palloc'd). */
	char	   *(*current) (void);

	/* Visit every branch. */
	void		(*branches) (BranchVisit visit, void *arg);

	/*
	 * Run fn with the session on branch name, back on its own branch after,
	 * whatever happens.
	 */
	void		(*run_on) (const char *name, void (*fn) (void *arg), void *arg);

	/*
	 * Optional: give each of the session branch's relations relids storage
	 * shared with branch source's relation of the same id, as a copy has
	 * it when made (its pages, its maps, how old its oldest row is), the
	 * storage it had going when the transaction commits.  Each is locked
	 * first, without waiting, as TRUNCATE locks it, and the source's as a
	 * copy locks them (against writers); then unchanged(arg), if given, says
	 * whether the source's are still as the caller found them.  False, with
	 * nothing shared, if a lock isn't free, unchanged says no, or one isn't
	 * on both branches alike or can't be shared (the locks taken are kept).
	 * Like TRUNCATE, it isn't MVCC-safe: a snapshot taken before it reads
	 * the new storage once it commits.
	 */
	bool		(*share) (Oid source, List *relids,
						  bool (*unchanged) (void *arg), void *arg);
} BranchOpsRoutine;

/* the branch operations of this database */
extern const BranchOpsRoutine *branch_ops(void);

#endif							/* BRANCH_OPS_H */
