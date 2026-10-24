/* -------------------------------------------------------------------------
 *
 * pg_sequence.h
 *	  definition of the "sequence" system catalog (pg_sequence)
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_sequence.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 * -------------------------------------------------------------------------
 */
#ifndef PG_SEQUENCE_H
#define PG_SEQUENCE_H

#include "catalog/genbki.h"
#include "catalog/pg_sequence_d.h"	/* IWYU pragma: export */

BEGIN_CATALOG_STRUCT

CATALOG(pg_sequence,2224,SequenceRelationId)
{
	Oid			seqrelid BKI_LOOKUP(pg_class);
	Oid			seqtypid BKI_LOOKUP(pg_type);
	int64		seqstart;
	int64		seqincrement;
	int64		seqmax;
	int64		seqmin;
	int64		seqcache;
	bool		seqcycle;
	Oid			seqbranch BKI_DEFAULT(0);	/* branch of a versioned object's row; 0 if shared */
} FormData_pg_sequence;

END_CATALOG_STRUCT

/* ----------------
 *		Form_pg_sequence corresponds to a pointer to a tuple with
 *		the format of pg_sequence relation.
 * ----------------
 */
typedef FormData_pg_sequence *Form_pg_sequence;

DECLARE_UNIQUE_INDEX_PKEY(pg_sequence_seqrelid_index, 5002, SequenceRelidIndexId, pg_sequence, btree(seqrelid oid_ops, seqbranch oid_ops));
DECLARE_INDEX(pg_sequence_branch_index, 9173, SequenceBranchIndexId, pg_sequence, btree(seqbranch oid_ops));

MAKE_SYSCACHE(SEQRELID, pg_sequence_seqrelid_index, 32);

#endif							/* PG_SEQUENCE_H */
