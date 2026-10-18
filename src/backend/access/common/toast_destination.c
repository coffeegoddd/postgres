/*-------------------------------------------------------------------------
 *
 * toast_destination.c
 *	  TOAST destinations that name values by content, found by tag.
 *
 * One destination a hash algorithm's tag: the chunk store of a versioned
 * table's database (access/prolly_toast.h) for SHA-256.
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

const ToastDestinationRoutine *
toast_pointer_destination(const varlena *attr)
{
	if (VARATT_IS_EXTERNAL_ONDISK_HASH(attr))
		return &prolly_toast_chunkstore;
	elog(ERROR, "unrecognized TOAST pointer tag %u", VARTAG_EXTERNAL(attr));
	return NULL;				/* keep compiler quiet */
}
