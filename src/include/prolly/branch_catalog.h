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
 * of that name, nor a versioned one while a shared one exists.  The same
 * holds for extensions, whose names are not qualified by a schema.
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
#include "catalog/dependency.h"
#include "storage/lockdefs.h"
#include "utils/relcache.h"

/* The branch whose catalog rows this session sees (0: none). */
extern PGDLLIMPORT int catalog_branch;

extern void assign_catalog_branch(int newval, void *extra);

/*
 * True while a branch change runs the pg_namespace syscache callbacks, so
 * that a callback whose state does not depend on the session's branch can
 * tell this from a change to the namespaces themselves.
 */
extern PGDLLIMPORT bool catalog_branch_changing;

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

/*
 * What branch creation and deletion do to the catalogs: drop removes every
 * catalog row of a branch (its versioned schemas and all in them), leaving
 * every other branch's and the shared ones.  Storage is not its concern:
 * dropping a relation drops its storage as any DROP does.
 */
typedef struct BranchCatalogRoutine
{
	void		(*drop) (int32 branch);
} BranchCatalogRoutine;

extern PGDLLIMPORT const BranchCatalogRoutine *BranchCatalogs;
extern PGDLLIMPORT const BranchCatalogRoutine DependencyBranchCatalogs;

/*
 * Copies a branch's catalog rows to another branch: copy makes every one
 * of src's versioned schemas, and every object in them, anew on dst (which
 * has none), under new object ids, every reference between them rewritten
 * to the copies' (see branch_catalog_copy.c).  Each relation copied gets
 * storage of its own.  It errors out, copying nothing, if src holds an
 * object it doesn't know how to copy.
 */
typedef struct BranchCatalogCopyRoutine
{
	void		(*copy) (int32 src, int32 dst);
} BranchCatalogCopyRoutine;

extern PGDLLIMPORT const BranchCatalogCopyRoutine *BranchCatalogCopy;
extern PGDLLIMPORT const BranchCatalogCopyRoutine DependencyBranchCatalogCopy;

/*
 * The branch schema names resolve on now: the session's, or the one
 * referential integrity resolves a constraint's query on
 * (CatalogBranchResolveAs()).
 */
extern int32 CatalogBranchOfNames(void);

/*
 * Runs what follows on branch, as a query runs each time on the branch it
 * started on: answers the GUC nesting level to give CatalogBranchLeave(), or
 * -1 if the session is on it already.  (An error unwinds it with the
 * transaction or subtransaction, as it does a function's SET clause.)
 */
extern int	CatalogBranchEnter(int32 branch);
extern void CatalogBranchLeave(int nestlevel);

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

/*
 * Locks a branch's catalog rows in mode until the transaction ends: making
 * a versioned schema on the branch takes AccessShareLock, dropping the
 * branch's rows AccessExclusiveLock, so that no schema is made on a branch
 * while its rows are dropped.
 */
extern void LockBranchCatalogs(int32 branch, LOCKMODE mode);

/*
 * The branch a new versioned schema goes on: the session's, if it may (its
 * branch locked against a drop of its rows).
 */
extern int32 VersionedNamespaceBranch(const char *nspname);

/*
 * Extensions are shared (in a shared schema) or a branch's (in one of its
 * versioned schemas), never both under one name: ExtensionOidByName finds
 * the one this session sees, ExtensionCheckNameFree errors out unless one
 * may be made in schema nspid (and locks the name until the transaction
 * ends), and ExtensionCheckMove refuses a move between the two kinds or
 * between branches.
 */
extern Oid	ExtensionOidByName(const char *extname);
extern void ExtensionCheckNameFree(const char *extname, Oid nspid);
extern void ExtensionCheckMove(const char *extname, Oid oldnspid, Oid newnspid);

/*
 * Whether logical decoding leaves out changes to rel: decoding from a slot
 * made on a branch leaves out another branch's tables; from one made on
 * no branch, nothing.
 */
extern bool CatalogBranchSkipsRelation(Relation rel);

/*
 * Dependencies point from versioned objects to shared ones, never from a
 * shared object to a versioned one, nor between branches: a shared object
 * must not change when one branch changes.  (A temporary object may
 * depend on the session's versioned objects: it is the session's alone.)
 * CatalogBranchCheckDependencies errors out if depender may not depend on
 * referenced (nor belong to a shared object: an internal or extension
 * dependency, which a drop of the depender turns into a drop of the
 * shared object); CatalogBranchCheckMove if an object may not move from one
 * schema to another, which would change its branch.
 */
extern void CatalogBranchCheckDependencies(const ObjectAddress *depender,
										   const ObjectAddress *referenced,
										   int nreferenced,
										   DependencyType deptype);
extern void CatalogBranchCheckMove(Oid oldnspid, Oid newnspid);

/*
 * Errors out unless table childid may inherit from (or be a partition of)
 * table parentid: both shared, or both one branch's.  A dependency from a
 * branch's table on a shared parent points the right way, but the parent's
 * scans, row routing and DDL would reach into the branch.
 */
extern void CatalogBranchCheckInheritance(Oid childid, Oid parentid);

/*
 * Resolves schema names as on the branch of namespace nspid (no branch, if
 * it is a shared schema) until CatalogBranchResolveRestore is given what
 * this returned, on error too.  Referential integrity uses it for the
 * queries it runs on another branch's tables, which name them.
 */
extern int32 CatalogBranchResolveAs(Oid nspid);
extern void CatalogBranchResolveRestore(int32 saved);

#endif							/* PROLLY_BRANCH_CATALOG_H */
