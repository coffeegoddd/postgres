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
 * may hold it, and nothing deletes the value with a row.  A table's access
 * method chooses its destination; the toaster's passes are the same, a
 * value they move out going to the destination rather than to the TOAST
 * relation, and a hash pointer a row is given being kept as it is (it has
 * no owner).  detoast.c finds the destination of a hash pointer by its tag,
 * and asks it for the value or a slice of it.
 *
 * src/include/access/toast_destination.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TOAST_DESTINATION_H
#define TOAST_DESTINATION_H

#include "utils/relcache.h"
#include "varatt.h"

typedef struct ToastDestinationRoutine
{
	/*
	 * Store a value of a column of type typid (not external; compressed or
	 * not) moved out of line from a row of rel: the hash pointer naming it.
	 */
	varlena    *(*save) (Relation rel, Datum value, Oid typid);

	/*
	 * A hash pointer a row of rel is given for a column of type typid:
	 * whether the row may hold it as it is (save() would store such a
	 * value as the pointer has it), the value it names then kept as
	 * save()'s are.
	 */
	bool		(*reference) (Relation rel, varlena *pointer, Oid typid);

	/*
	 * Bytes [offset, offset + length) of the value a hash pointer names
	 * (length < 0: to its end; fewer if it ends first), as an uncompressed
	 * varlena with a 4-byte header.  offset >= 0.
	 */
	varlena    *(*fetch) (varlena *attr, int32 offset, int32 length);
} ToastDestinationRoutine;

/* The destination of a hash pointer, by its tag. */
extern const ToastDestinationRoutine *toast_pointer_destination(const varlena *attr);

/*
 * Values moved out of line to the TOAST relation whatever the destination
 * (a developer option, to make OID pointers in tables that have another).
 */
extern PGDLLIMPORT bool debug_toast_relation;

/* A table's destination, by its access method (NULL: its TOAST relation). */
extern const ToastDestinationRoutine *toast_relation_destination(Relation rel);

#endif							/* TOAST_DESTINATION_H */
