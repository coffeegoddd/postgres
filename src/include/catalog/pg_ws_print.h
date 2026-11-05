/*-------------------------------------------------------------------------
 *
 * pg_ws_print.h
 *	  What a restore printed, of what a commit of a versioned schema holds.
 *
 * Printing is not a fixed point: what reads back from a commit's text may print
 * otherwise, with nothing changed.  A row says that an object printing prtprinted is
 * as a commit held it, prtheld.  A row is only for what printed otherwise, and only
 * counts while the object prints so; any change to it makes it print otherwise.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_ws_print.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_WS_PRINT_H
#define PG_WS_PRINT_H

#include "catalog/genbki.h"
#include "catalog/pg_ws_print_d.h"	/* IWYU pragma: export */

BEGIN_CATALOG_STRUCT

CATALOG(pg_ws_print,8853,WsPrintRelationId)
{
	Oid			prtnamespace BKI_LOOKUP(pg_namespace);	/* the versioned schema */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	text		prtkey BKI_FORCE_NOT_NULL;	/* the object, as a commit keys it */
	text		prtprinted BKI_FORCE_NOT_NULL;	/* what it printed once restored */
	text		prtheld BKI_FORCE_NOT_NULL; /* what the commit held of it */
#endif
} FormData_pg_ws_print;

END_CATALOG_STRUCT

typedef FormData_pg_ws_print *Form_pg_ws_print;

DECLARE_TOAST(pg_ws_print, 8854, 8855);

DECLARE_UNIQUE_INDEX_PKEY(pg_ws_print_namespace_key_index, 8856, WsPrintNamespaceKeyIndexId, pg_ws_print, btree(prtnamespace oid_ops, prtkey text_ops));

#endif							/* PG_WS_PRINT_H */
