/*-------------------------------------------------------------------------
 *
 * toast_destination.c
 *	  TOAST destinations that name values by content, found by tag.
 *
 * One destination a hash algorithm's tag: the chunk store of a versioned
 * table's database (access/prolly_toast.h) for BLAKE3, the destination of
 * the tables of the prolly access method.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/common/toast_destination.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_toast.h"
#include "access/toast_destination.h"
#include "catalog/pg_am_d.h"
#include "utils/rel.h"

bool		debug_toast_relation = false;

const ToastDestinationRoutine *
toast_relation_destination(Relation rel)
{
	if (rel->rd_rel->relam == PROLLY_TABLE_AM_OID && !debug_toast_relation)
		return &prolly_toast_chunkstore;
	return NULL;
}

const ToastDestinationRoutine *
toast_pointer_destination(const varlena *attr)
{
	if (VARATT_IS_EXTERNAL_ONDISK_HASH(attr))
		return &prolly_toast_chunkstore;
	elog(ERROR, "unrecognized TOAST pointer tag %u", VARTAG_EXTERNAL(attr));
	return NULL;				/* keep compiler quiet */
}
