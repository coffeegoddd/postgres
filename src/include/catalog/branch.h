/*-------------------------------------------------------------------------
 *
 * branch.h
 *	  Branch-aware catalogs: the catalogs whose rows can belong to a
 *	  versioned object, and their branch columns.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/branch.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CATALOG_BRANCH_H
#define CATALOG_BRANCH_H

#include "access/htup.h"
#include "access/transam.h"
#include "executor/tuptable.h"
#include "utils/relcache.h"

/* a branch-aware catalog's branch column */
typedef struct CatalogBranchInfo
{
	Oid			relid;			/* the catalog */
	Size		offset;			/* the column's offset in the row's struct */
	AttrNumber	attnum;			/* the column */
} CatalogBranchInfo;

/* the session's branch: a versioned object's rows on it are the ones seen */
extern PGDLLIMPORT Oid MyBranch;
extern PGDLLIMPORT bool MyBranchReadOnly;

/*
 * While set, index_create() is making an index of this table: its pg_class
 * row belongs where the table's does (a TOAST table's index is in pg_toast).
 */
extern PGDLLIMPORT Oid CatalogBranchIndexTable;

/* while set, catalog writes keep the branch a row carries: a branch's copy */
extern PGDLLIMPORT bool CatalogKeepRowBranch;

/*
 * While set, catalog scans return every branch's rows: for scans that
 * gather facts about all of them (oldest XIDs, what to vacuum) and build no
 * cache, since an OID can name a row on each branch.
 */
extern PGDLLIMPORT bool CatalogAllBranches;

/* the catalog's branch column, or NULL if it has none */
extern const CatalogBranchInfo *catalog_branch_info(Oid relid);

/* every branch-aware catalog */
extern const CatalogBranchInfo *catalog_branch_catalogs(int *n);

/* a row's branch: its branch column (0 for shared rows) */
extern Oid	catalog_row_branch(const CatalogBranchInfo *info, HeapTuple tup);

/* check out the pg_branch row a connection named; false if there is none */
extern bool branch_session_init(const char *branch);

/* the branch a relation's locks are for (0: not versioned) */
extern Oid	relation_lock_branch(Oid relid);

/* switch the session to another branch */
extern void set_session_branch(Oid branch);

/* check out a branch, locked while the session is on it; false if gone */
extern bool check_out_branch(Oid branch);

/* lock a branch to delete it; false if a session is on it */
extern bool lock_branch_for_delete(Oid branch);

/* note a branch the current subtransaction made */
extern void note_new_branch(Oid branch);

/* is this storage another branch's relation's (for logical decoding)? */
extern bool relfilenumber_on_other_branch(Oid spcoid, RelFileNumber relnumber);

/* the branch of the catalog row of object objid in catalog classid (0: shared) */
extern Oid	catalog_object_branch(Oid classid, Oid objid);

/* is a row of a branch-aware catalog on the session's branch (or shared)? */
extern bool catalog_tuple_on_branch(HeapTuple tup);

/*
 * Can this row be seen from the session's branch?  Only branch-aware
 * catalogs hold rows of other branches; every other table answers at once.
 */
static inline bool
tuple_on_branch(HeapTuple tup)
{
	if (likely(tup->t_tableOid >= FirstGenbkiObjectId))
		return true;
	return catalog_tuple_on_branch(tup);
}

/*
 * Does this relation hold rows of several branches?  Its pages' all-visible
 * flags then say nothing about which rows a session sees.
 */
static inline bool
relation_has_branches(Oid relid)
{
	return relid < FirstGenbkiObjectId && catalog_branch_info(relid) != NULL;
}

/*
 * The row to write for tup: tup itself with its branch column set, or, if
 * the column is null, a new row with it set, which the caller frees.
 */
extern HeapTuple catalog_row_with_branch(Relation rel, HeapTuple tup);
extern void catalog_slot_set_branch(Relation rel, TupleTableSlot *slot);

#endif							/* CATALOG_BRANCH_H */
