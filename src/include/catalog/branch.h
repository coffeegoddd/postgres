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
#include "executor/tuptable.h"
#include "utils/relcache.h"

/* a branch-aware catalog's branch column */
typedef struct CatalogBranchInfo
{
	Oid			relid;			/* the catalog */
	Size		offset;			/* the column's offset in the row's struct */
	AttrNumber	attnum;			/* the column */
} CatalogBranchInfo;

/* the catalog's branch column, or NULL if it has none */
extern const CatalogBranchInfo *catalog_branch_info(Oid relid);

/* a row's branch: its branch column (0 for shared rows) */
extern Oid	catalog_row_branch(const CatalogBranchInfo *info, HeapTuple tup);

/*
 * The row to write for tup: tup itself with its branch column set, or, if
 * the column is null, a new row with it set, which the caller frees.
 */
extern HeapTuple catalog_row_with_branch(Relation rel, HeapTuple tup);
extern void catalog_slot_set_branch(Relation rel, TupleTableSlot *slot);

#endif							/* CATALOG_BRANCH_H */
