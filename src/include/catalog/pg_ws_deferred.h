/*-------------------------------------------------------------------------
 *
 * pg_ws_deferred.h
 *	  Constraints a commit holds that are not on their tables, as rows violate
 *	  them (prolly/ws/ws_viol.h).
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/catalog/pg_ws_deferred.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_WS_DEFERRED_H
#define PG_WS_DEFERRED_H

#include "catalog/genbki.h"
#include "catalog/pg_ws_deferred_d.h"	/* IWYU pragma: export */

BEGIN_CATALOG_STRUCT

CATALOG(pg_ws_deferred,8857,WsDeferredRelationId)
{
	Oid			defrelid BKI_LOOKUP(pg_class);	/* the table it is of */
	char		defkind;		/* check, not null, unique, exclusion, index,
								 * foreign key, replica identity or clustering:
								 * c, n, u, x, i, f, r or l */
	NameData	defname;		/* its name */
	Oid			defrefrelid BKI_LOOKUP_OPT(pg_class);	/* what a foreign key references, or 0 */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	bytea		defrecord BKI_FORCE_NOT_NULL;	/* the record that holds it, or a
												 * foreign key's value */
	bytea		defkey BKI_FORCE_NOT_NULL;	/* a foreign key's key, or empty */
#endif
} FormData_pg_ws_deferred;

END_CATALOG_STRUCT

typedef FormData_pg_ws_deferred *Form_pg_ws_deferred;

DECLARE_TOAST(pg_ws_deferred, 8858, 8859);

DECLARE_UNIQUE_INDEX_PKEY(pg_ws_deferred_relid_kind_name_index, 8860, WsDeferredRelidKindNameIndexId, pg_ws_deferred, btree(defrelid oid_ops, defkind char_ops, defname name_ops));

#endif							/* PG_WS_DEFERRED_H */
