/*-------------------------------------------------------------------------
 *
 * pg_branch.h
 *	  definition of the "branch" system catalog (pg_branch)
 *
 * A database's branches.  A versioned object (one in a versioned schema)
 * exists once per branch; its catalog rows carry the branch's OID.  Branch
 * 0 means shared: no branch.  main is a bootstrap row.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_branch.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_BRANCH_H
#define PG_BRANCH_H

#include "catalog/genbki.h"
#include "catalog/pg_branch_d.h"	/* IWYU pragma: export */

/* ----------------
 *		pg_branch definition.  cpp turns this into
 *		typedef struct FormData_pg_branch
 * ----------------
 */
BEGIN_CATALOG_STRUCT

CATALOG(pg_branch,9424,BranchRelationId)
{
	Oid			oid;			/* oid: the branch id in catalog rows */
	NameData	brname;			/* the branch's name */

	/*
	 * The role that claimed the branch for its owner; 0 when no owner is
	 * recorded.
	 */
	Oid			brownerrole BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_authid);

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	/* the writer whose changes the branch's working set holds, if recorded */
	text		browner BKI_DEFAULT(_null_) BKI_FORCE_NULL;
#endif
} FormData_pg_branch;

END_CATALOG_STRUCT

/* ----------------
 *		Form_pg_branch corresponds to a pointer to a tuple with
 *		the format of pg_branch relation.
 * ----------------
 */
typedef FormData_pg_branch *Form_pg_branch;

DECLARE_TOAST(pg_branch, 9700, 9701);

DECLARE_UNIQUE_INDEX_PKEY(pg_branch_oid_index, 9425, BranchOidIndexId, pg_branch, btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_branch_name_index, 9426, BranchNameIndexId, pg_branch, btree(brname name_ops));

MAKE_SYSCACHE(BRANCHOID, pg_branch_oid_index, 4);
MAKE_SYSCACHE(BRANCHNAME, pg_branch_name_index, 4);

/*
 * The kinds of the version control's objects that a role makes but no
 * catalog lists: a branch, a tag, a stash, kept in a database's refs.  Each
 * is recorded with an id of its own as a shared dependency of its maker
 * (pg_shdepend's classid and objid), so that the role commands see it as
 * any object the role owns.
 */
DECLARE_OID_DEFINING_MACRO(RefsBranchClassId, 9907);
DECLARE_OID_DEFINING_MACRO(RefsTagClassId, 9908);
DECLARE_OID_DEFINING_MACRO(RefsStashClassId, 9909);

#define IsRefsMadeClassId(classId) \
	((classId) == RefsBranchClassId || (classId) == RefsTagClassId || \
	 (classId) == RefsStashClassId)

#endif							/* PG_BRANCH_H */
