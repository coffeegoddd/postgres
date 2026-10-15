/*-------------------------------------------------------------------------
 *
 * prolly_table.c
 *	  Table access method "prolly": heap's implementation, with the table,
 *	  its indexes and its TOAST table in block-mapped storage.
 *
 * A prolly table is versioned: each branch will have its own copy of it,
 * sharing pages copy-on-write, and commits will snapshot it.  Its pages
 * are heap's, read and written by heap's code, so the handler returns
 * heap's own routines: code that checks for heap by its routines (rather
 * than its OID) treats a prolly table as heap.  What differs is where the
 * pages live, which heap decides by the relation's access method when it
 * creates storage (heapam_handler.c).  Prolly tables are permanent:
 * temporary and unlogged tables are heap.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_table.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tableam.h"
#include "fmgr.h"

PG_FUNCTION_INFO_V1(prolly_tableam_handler);

Datum
prolly_tableam_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(GetHeapamTableAmRoutine());
}
