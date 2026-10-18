/*-------------------------------------------------------------------------
 *
 * toast_destination.h
 *	  TOAST destinations other than a TOAST relation: stores that name the
 *	  values they hold by content.
 *
 * A value TOAST moves out of line goes to the table's TOAST relation, and
 * is named by an OID pointer (VARTAG_ONDISK_OID or VARTAG_ONDISK_OID8): a
 * value ID that one row owns.  A destination names a value by content
 * instead, with a hash pointer (VARTAG_ONDISK_SHA256): any row of any table
 * may hold it, and nothing deletes the value with a row.  detoast.c finds
 * the destination of a hash pointer by its tag, and asks it for the value
 * or a slice of it.
 *
 * src/include/access/toast_destination.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TOAST_DESTINATION_H
#define TOAST_DESTINATION_H

#include "varatt.h"

typedef struct ToastDestinationRoutine
{
	/*
	 * Bytes [offset, offset + length) of the value a hash pointer names
	 * (length < 0: to its end; fewer if it ends first), as an uncompressed
	 * varlena with a 4-byte header.  offset >= 0.
	 */
	varlena    *(*fetch) (varlena *attr, int32 offset, int32 length);
} ToastDestinationRoutine;

/* The destination of a hash pointer, by its tag. */
extern const ToastDestinationRoutine *toast_pointer_destination(const varlena *attr);

#endif							/* TOAST_DESTINATION_H */
