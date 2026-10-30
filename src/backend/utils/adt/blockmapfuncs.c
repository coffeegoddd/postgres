/*-------------------------------------------------------------------------
 *
 * blockmapfuncs.c
 *	  Functions for a block-mapped relation's sharing of pages: what its
 *	  changes did to pages other maps share (cumulative statistics), and
 *	  which of its pages are shared now.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/blockmapfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "catalog/pg_class.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bmap.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

/*
 * pg_stat_get_block_map(relid): the statistics of the relation's current
 * file (pgstat_block_map.c), or nulls if it has none: not block-mapped, or
 * nothing counted since the file was made or its statistics reset.  The
 * relation is looked up in the catalog, not opened.
 */
Datum
pg_stat_get_block_map(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	TupleDesc	tupdesc;
	Datum		values[4] = {0};
	bool		nulls[4] = {true, true, true, true};
	HeapTuple	tuple;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (HeapTupleIsValid(tuple))
	{
		Form_pg_class form = (Form_pg_class) GETSTRUCT(tuple);
		PgStat_StatBlockMapEntry *entry = NULL;

		/* (a mapped catalog's file number isn't in pg_class: never block-mapped) */
		if (RelFileNumberIsValid(form->relfilenode) &&
			form->relpersistence == RELPERSISTENCE_PERMANENT)
			entry = pgstat_fetch_stat_block_map(OidIsValid(form->reltablespace) ?
												form->reltablespace :
												MyDatabaseTableSpace,
												MyDatabaseId,
												form->relfilenode);
		ReleaseSysCache(tuple);
		if (entry)
		{
			values[0] = Int64GetDatum(entry->pages_copied);
			values[1] = Int64GetDatum(entry->pages_claimed);
			values[2] = Int64GetDatum(entry->nodes_copied);
			nulls[0] = nulls[1] = nulls[2] = false;
			if (entry->stat_reset_timestamp != 0)
			{
				values[3] = TimestampTzGetDatum(entry->stat_reset_timestamp);
				nulls[3] = false;
			}
		}
	}
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * pg_relation_shared_pages(relation): the pages of the relation's main
 * fork; those shared, with another map or with a root captured of this
 * relation, which a change copies; and those another map may hold, as
 * VACUUM weighs them (a hint, which counts pages shared with a map since
 * dropped until they change or a sweep finds them its own).  Nulls if the
 * relation isn't block-mapped.  Reads the whole map: as costly as the
 * relation is long.
 */
Datum
pg_relation_shared_pages(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	TupleDesc	tupdesc;
	Datum		values[3] = {0};
	bool		nulls[3] = {true, true, true};
	Relation	rel;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	rel = try_relation_open(relid, AccessShareLock);
	if (rel != NULL)
	{
		if (RELKIND_HAS_STORAGE(rel->rd_rel->relkind) &&
			RelationGetSmgr(rel)->smgr_which == SMGR_BMAP)
		{
			SMgrRelation reln = RelationGetSmgr(rel);
			BlockNumber nblocks = smgrnblocks(reln, MAIN_FORKNUM);
			int64		shared = 0;
			int64		elsewhere = 0;

			for (BlockNumber blkno = 0; blkno < nblocks; blkno++)
			{
				bool		copies;

				CHECK_FOR_INTERRUPTS();
				reln = RelationGetSmgr(rel);
				if (smgrpageisshared(reln, MAIN_FORKNUM, blkno, &copies))
				{
					shared++;
					if (smgrpagesharedelsewhere(reln, MAIN_FORKNUM, blkno))
						elsewhere++;
				}
			}
			values[0] = Int64GetDatum((int64) nblocks);
			values[1] = Int64GetDatum(shared);
			values[2] = Int64GetDatum(elsewhere);
			nulls[0] = nulls[1] = nulls[2] = false;
		}
		relation_close(rel, AccessShareLock);
	}
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}
