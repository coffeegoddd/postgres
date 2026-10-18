/*-------------------------------------------------------------------------
 *
 * prolly_toast.h
 *	  Versioned tables' values out of line, in their database's chunk store:
 *	  the TOAST destination of SHA-256 hash pointers.
 *
 * A value is stored as a value tree (prolly_tree.h, value_write()) in the
 * database's chunk store, and named by a VARTAG_ONDISK_SHA256 pointer to
 * the tree's root.  The tree holds the value's canonical bytes
 * (prolly_datum.h) where they differ from the bytes it holds only in form,
 * so that the pointer's hash is the column's reference in history: a
 * text-like value's bytes in UTF-8 when the database's encoding is
 * another; a jsonb document's canonical bytes.  The form is the pointer's,
 * in va_extinfo's high bits; fetching undoes it.
 *
 * src/include/access/prolly_toast.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_TOAST_H
#define PROLLY_TOAST_H

#include "access/toast_destination.h"
#include "lib/stringinfo.h"

/* What a hash pointer's tree holds */
#define PROLLY_TOAST_FORM_BYTES		0	/* the value's bytes */
#define PROLLY_TOAST_FORM_UTF8		1	/* a text-like value's, in UTF-8 */
#define PROLLY_TOAST_FORM_JSONB		2	/* a document's canonical bytes */

extern PGDLLIMPORT const ToastDestinationRoutine prolly_toast_chunkstore;

/*
 * Store a value of type typid (TOASTed or not) in the current database's
 * chunk store: the hash pointer naming it.
 */
extern varlena *prolly_toast_store(Datum value, Oid typid);

/*
 * The reference of the value tree a hash pointer names, appended to ref;
 * returns the pointer's form.
 */
extern int	prolly_toast_reference(const varlena *pointer, StringInfo ref);

#endif							/* PROLLY_TOAST_H */
