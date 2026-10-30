/*-------------------------------------------------------------------------
 *
 * branch_filter.h
 *	  The catalog filter: queries of the catalogs see the session's
 *	  branch's rows and the shared ones, not other branches'.
 *
 * A catalog row belongs to a branch when the object it is, or describes,
 * is in a versioned schema: a pg_class row by its relnamespace, a
 * pg_attribute row by its relation's, a pg_depend row by either object's.
 * Queries do not see rows of other branches' objects.  Lookups by object
 * id use catalog snapshots, which the filter leaves whole, so the
 * syscaches, dependency walks and logical decoding see every branch's
 * objects.
 *
 * The table access method calls the filter after it has found a tuple
 * visible and released the buffer's content lock (the filter may read the
 * catalogs to learn an object's branch, which must not happen under a
 * buffer lock).  Index-only scans of filtered catalogs always visit the
 * heap, and scans of them run page at a time.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/branch_filter.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_BRANCH_FILTER_H
#define PROLLY_BRANCH_FILTER_H

#include "access/htup.h"
#include "storage/block.h"
#include "storage/bufpage.h"
#include "storage/off.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"

/* Whether queries hide other branches' catalog rows (on by default). */
extern PGDLLIMPORT bool catalog_branch_filter;

/* Whether a scan of rel with snapshot must hide other branches' rows. */
extern bool CatalogBranchFilters(Relation rel, Snapshot snapshot);

/* Whether a visible tuple of rel is another branch's. */
extern bool CatalogBranchHides(Relation rel, HeapTuple tuple);

/*
 * Removes other branches' tuples from the n visible tuples at offsets of a
 * pinned (not locked) page of rel, keeping their order; returns how many
 * remain.
 */
extern uint32 CatalogBranchFilterPage(Relation rel, Page page,
									  BlockNumber block,
									  OffsetNumber *offsets, uint32 n);

/*
 * The branch of every catalog object, by class and object id: 0 for a
 * shared object (or one that does not exist), else the branch of the
 * versioned schema it is in or belongs to.  An object's branch never
 * changes while the object exists, so an implementation may keep what it
 * learned, and forget it when the object's catalog row is invalidated.
 *
 * reset forgets everything; stats reports what the implementation did, so
 * tests can see what an answer cost.
 */
typedef struct BranchMapStats
{
	uint64		probes;			/* questions asked */
	uint64		reads;			/* catalog reads made to answer them */
	uint64		forgotten;		/* answers dropped by invalidations */
	uint64		entries;		/* answers kept now */
} BranchMapStats;

typedef struct BranchMapRoutine
{
	int32		(*object_branch) (Oid classid, Oid objid);
	void		(*reset) (void);
	void		(*stats) (BranchMapStats *stats);
} BranchMapRoutine;

/*
 * The implementation in use: a per-backend cache filled on demand from
 * the catalogs, and kept by syscache invalidations.
 */
extern PGDLLIMPORT const BranchMapRoutine *BranchMap;
extern PGDLLIMPORT const BranchMapRoutine CachedBranchMap;

#endif							/* PROLLY_BRANCH_FILTER_H */
