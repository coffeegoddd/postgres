/* -------------------------------------------------------------------------
 *
 * pg_seclabel.h
 *	  definition of the "security label" system catalog (pg_seclabel)
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_seclabel.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 * -------------------------------------------------------------------------
 */
#ifndef PG_SECLABEL_H
#define PG_SECLABEL_H

#include "catalog/genbki.h"
#include "catalog/pg_seclabel_d.h"	/* IWYU pragma: export */

/* ----------------
 *		pg_seclabel definition.  cpp turns this into
 *		typedef struct FormData_pg_seclabel
 * ----------------
 */
BEGIN_CATALOG_STRUCT

CATALOG(pg_seclabel,3596,SecLabelRelationId)
{
	Oid			objoid;			/* OID of the object itself */
	Oid			classoid BKI_LOOKUP(pg_class);	/* OID of table containing the
												 * object */
	int32		objsubid;		/* column number, or 0 if not used */

	Oid			objbranch BKI_DEFAULT(0);	/* branch of a versioned object's row; 0 if shared */
#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	text		provider BKI_FORCE_NOT_NULL;	/* name of label provider */
	text		label BKI_FORCE_NOT_NULL;	/* security label of the object */
#endif
} FormData_pg_seclabel;

END_CATALOG_STRUCT

DECLARE_TOAST(pg_seclabel, 3598, 3599);

DECLARE_UNIQUE_INDEX_PKEY(pg_seclabel_object_index, 3597, SecLabelObjectIndexId, pg_seclabel, btree(objoid oid_ops, classoid oid_ops, objsubid int4_ops, provider text_ops, objbranch oid_ops));
DECLARE_INDEX(pg_seclabel_branch_index, 9193, SecLabelBranchIndexId, pg_seclabel, btree(objbranch oid_ops));

#endif							/* PG_SECLABEL_H */
