/*-------------------------------------------------------------------------
 *
 * chunk_log_checks.c
 *	  SQL-callable access to the history store's chunk log
 *	  (prolly/chunk_log.h), for its tests.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/test/modules/test_prolly/chunk_log_checks.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/htup_details.h"
#include "fmgr.h"
#include "funcapi.h"
#include "prolly/chunk_log.h"
#include "prolly/chunk_region.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "varatt.h"

PG_FUNCTION_INFO_V1(test_chunk_log_append);
PG_FUNCTION_INFO_V1(test_chunk_log_read);
PG_FUNCTION_INFO_V1(test_chunk_log_end);
PG_FUNCTION_INFO_V1(test_chunk_log_space);
PG_FUNCTION_INFO_V1(test_chunk_log_regions);

static Relation
open_log(Oid relid, LOCKMODE mode)
{
	Relation	rel = relation_open(relid, mode);

	if (!RelationIsChunkLog(rel))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a chunk log relation",
						RelationGetRelationName(rel))));
	return rel;
}

/* test_chunk_log_append(rel regclass, data bytea) returns int8 */
Datum
test_chunk_log_append(PG_FUNCTION_ARGS)
{
	Relation	rel = open_log(PG_GETARG_OID(0), RowExclusiveLock);
	bytea	   *data = PG_GETARG_BYTEA_PP(1);
	uint64		loc;

	loc = ChunkLog->append(rel, VARDATA_ANY(data), VARSIZE_ANY_EXHDR(data));
	relation_close(rel, NoLock);
	PG_RETURN_INT64((int64) loc);
}

/* test_chunk_log_read(rel regclass, loc int8, len int4) returns bytea */
Datum
test_chunk_log_read(PG_FUNCTION_ARGS)
{
	Relation	rel = open_log(PG_GETARG_OID(0), AccessShareLock);
	int64		loc = PG_GETARG_INT64(1);
	int32		len = PG_GETARG_INT32(2);
	bytea	   *out;

	if (loc < 0 || len < 0 || (Size) len > MaxAllocSize - VARHDRSZ)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a chunk log's place and length are not negative, nor the length too large")));
	out = palloc(VARHDRSZ + len);
	SET_VARSIZE(out, VARHDRSZ + len);
	ChunkLog->read(rel, (uint64) loc, VARDATA(out), (size_t) len);
	relation_close(rel, AccessShareLock);
	PG_RETURN_BYTEA_P(out);
}

/* test_chunk_log_end(rel regclass) returns int8 */
Datum
test_chunk_log_end(PG_FUNCTION_ARGS)
{
	Relation	rel = open_log(PG_GETARG_OID(0), AccessShareLock);
	uint64		end = ChunkLog->end(rel);

	relation_close(rel, AccessShareLock);
	PG_RETURN_INT64((int64) end);
}

/* test_chunk_log_space(rel, OUT pages, OUT full, OUT dead, OUT free_pages) */
Datum
test_chunk_log_space(PG_FUNCTION_ARGS)
{
	Relation	rel = open_log(PG_GETARG_OID(0), AccessShareLock);
	uint32		pages,
				full,
				free_pages;
	uint64		dead;
	TupleDesc	desc;
	Datum		values[4];
	bool		nulls[4] = {false, false, false, false};

	ChunkLogSpace(rel, &pages, &full, &dead, &free_pages);
	relation_close(rel, AccessShareLock);
	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	values[0] = Int32GetDatum((int32) pages);
	values[1] = Int32GetDatum((int32) full);
	values[2] = Int64GetDatum((int64) dead);
	values[3] = Int32GetDatum((int32) free_pages);
	return HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc), values, nulls));
}

/* test_chunk_log_regions(rel): the regions live backends hold in the log */
Datum
test_chunk_log_regions(PG_FUNCTION_ARGS)
{
	Relation	rel = open_log(PG_GETARG_OID(0), AccessShareLock);
	ChunkRegionSet *set = ChunkRegionLive(rel->rd_locator,
										  ChunkLogGeneration(rel));

	relation_close(rel, AccessShareLock);
	PG_RETURN_INT32(set->n);
}
