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
#include "nodes/pg_list.h"
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
 * While set, a branch is made on the session's behalf (its writer branch):
 * without the role's privilege to make branches.
 */
extern PGDLLIMPORT bool BranchCreateOnBehalf;

/*
 * While set, catalog scans return every branch's rows: for scans that
 * gather facts about all of them (oldest XIDs, what to vacuum) and build no
 * cache, since an OID can name a row on each branch.
 */
extern PGDLLIMPORT bool CatalogAllBranches;

/*
 * Writes to a branch's versioned objects: rows of a versioned table or
 * sequence, or a versioned object's catalog rows.  The first write of each
 * branch in a transaction is noted (and again after a subtransaction that
 * first wrote it aborts), and callbacks are told; a write made on the
 * session's behalf while hushed (a branch's copies made, a branch moved to
 * a commit) isn't the transaction's.  The callbacks' errors are the
 * write's.
 */
typedef void (*BranchWriteCallback) (Oid branch, void *arg);

extern void RegisterBranchWriteCallback(BranchWriteCallback callback,
										void *arg);
extern void branch_note_write(Oid branch);
extern bool branch_written(Oid branch);

/*
 * A prolly table's tuple versions a write made (newtid) or replaced
 * (oldtid), or a speculative insertion took back (oldtid), once it has:
 * told to the table's change log, and to branch_tuple_hook, if set, unless
 * hushed.
 */
typedef void (*BranchTupleHook) (Relation rel, const ItemPointerData *oldtid,
								 const ItemPointerData *newtid);
extern PGDLLIMPORT BranchTupleHook branch_tuple_hook;
/* (oldtup: the version at oldtid in hand, if the caller has it, else NULL) */
extern void branch_note_tuple(Relation rel, const ItemPointerData *oldtid,
							  HeapTuple oldtup, const ItemPointerData *newtid);
/* ... or of the versions a write made, in hand */
extern void branch_note_made(Relation rel, const ItemPointerData *oldtid,
							 HeapTuple oldtup, HeapTuple *made, int nmade);

/*
 * Whether the transaction wrote a versioned object's catalog row, and the
 * versioned sequences whose values it changed (branch_note_sequence()),
 * not hushed; both forgotten with the session's branch's writes.
 */
extern bool branch_catalog_written(void);
extern void branch_note_sequence(Relation seqrel);
extern List *branch_sequences_written(void);

/* the branch's next write is noted (and told) again, as a first one */
extern void branch_forget_written(Oid branch);
extern List *branches_written(void);
extern int	branch_writes_hush(void);
extern void branch_writes_unhush(int depth);
/* hushed history work's own DDL (not user code's within it): no user events */
extern bool branch_ddl_internal(void);

/*
 * Run the statements hushed, unhushed however they end (an error caught
 * without a subtransaction's abort leaves nothing hushed).
 */
#define BRANCH_WRITES_HUSHED(...) \
	do { \
		int			hush_depth_ = branch_writes_hush(); \
		PG_TRY(_hushed); \
		{ \
			__VA_ARGS__; \
		} \
		PG_FINALLY(_hushed); \
		{ \
			branch_writes_unhush(hush_depth_); \
		} \
		PG_END_TRY(_hushed); \
	} while (0)

/*
 * A catalog row of a versioned object is to be updated or deleted: its
 * branch is written (as the new row's is, as it is stamped).
 */
extern void catalog_note_old_row(Relation rel, const ItemPointerData *tid);

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

/*
 * hold a branch against deletion until the transaction ends, as a session
 * on it does, without waiting; false if it is gone or being deleted
 */
extern bool pin_branch(Oid branch);

/* and against prepared transactions that were on it; error if one was */
extern void lock_branch_unprepared(Oid branch, const char *name);

/* note a branch the current subtransaction made */
extern void note_new_branch(Oid branch);

/*
 * If the current (sub)transaction aborts, the session goes back to the branch
 * it is on now; until branch_return_done() with the token returned, once
 * the operation that asked has succeeded.
 */
extern int	branch_return_on_abort(void);
extern void branch_return_done(int token);

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
