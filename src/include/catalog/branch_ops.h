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
	 * use makes.
	 */
	Oid			(*make) (const char *name, const char *start_point);

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
} BranchOpsRoutine;

/* the branch operations of this database */
extern const BranchOpsRoutine *branch_ops(void);

#endif							/* BRANCH_OPS_H */
