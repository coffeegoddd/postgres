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

#include "common/relpath.h"

/* Visits one branch, by name. */
typedef void (*BranchVisit) (const char *name, Oid branch, void *arg);

/*
 * A relation's pages as keep() kept them, for restore() to give back: the
 * storage that keeps them (its tablespace, 0 for the database's, and file
 * number), the snapshot's id there, and the relation's oldest unfrozen
 * transaction and multixact ids then (its pages hold none older).
 */
typedef struct BranchKeptRelation
{
	Oid			relid;
	Oid			tablespace;
	RelFileNumber relfilenumber;
	uint64		snapshot;
	TransactionId frozenxid;
	MultiXactId minmxid;
} BranchKeptRelation;

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

	/*
	 * Optional: keep the pages of the session branch's relations relids as
	 * they are, each in a snapshot of its storage that later changes don't
	 * touch, into kept[] (one for each, in order): O(1) in each relation's
	 * size.  A relation whose pages are still those of a snapshot in
	 * prior[] (nprior of them, of its storage) is kept by that one again;
	 * only the others take a snapshot.  Every one of relids is locked
	 * against writers first, without waiting (and let go after); then
	 * settled(arg), if given, says whether their pages hold exactly what
	 * the caller found them to.  False, nothing kept, if a lock isn't free,
	 * settled says no, or one can't be kept (its storage can't, or keeps
	 * too many snapshots already).  Snapshots taken by a transaction that
	 * aborts (or a subtransaction) are let go of later.
	 */
	bool		(*keep) (List *relids, const BranchKeptRelation *prior,
						 int nprior, BranchKeptRelation *kept,
						 bool (*settled) (void *arg), void *arg);

	/*
	 * Optional: give the session branch's relations kept[] (nkept of them,
	 * a table's together) back the pages keep() kept, the storage they had
	 * going when the transaction commits, into renewed[] (each one kept as
	 * it is now, by a snapshot of its new storage or, all unchanged since,
	 * the same one).  O(1) in each relation's size, and nothing if none
	 * changed since: if one did, each gets new storage made of its
	 * snapshot (with no free space or visibility map), its row the oldest
	 * transaction ids it had then (one unchanged may hold hints, in its
	 * buffers, true of it alone).  All or none: false, nothing changed,
	 * if one isn't on the branch with the storage that kept it, its
	 * snapshot is gone, its rows' transaction ids are older than the
	 * database still knows, or one that changed is in use (each is locked
	 * as TRUNCATE locks it, without waiting, and refused, as TRUNCATE
	 * refuses, while the session itself has it open or an AFTER trigger's
	 * event queued on it; the locks taken are kept).
	 * Like TRUNCATE, it isn't MVCC-safe: a snapshot taken before it reads
	 * the restored storage once it commits.
	 */
	bool		(*restore) (const BranchKeptRelation *kept, int nkept,
							BranchKeptRelation *renewed);

	/*
	 * Optional: let go of a snapshot keep() or restore() took, if its
	 * storage still has it (a dropped relation's went with it).
	 */
	void		(*let_go) (const BranchKeptRelation *kept);
} BranchOpsRoutine;

/* the branch operations of this database */
extern const BranchOpsRoutine *branch_ops(void);

#endif							/* BRANCH_OPS_H */
