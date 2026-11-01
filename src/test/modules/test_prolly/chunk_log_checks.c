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
#include "fmgr.h"
#include "prolly/chunk_log.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "varatt.h"

PG_FUNCTION_INFO_V1(test_chunk_log_append);
PG_FUNCTION_INFO_V1(test_chunk_log_read);
PG_FUNCTION_INFO_V1(test_chunk_log_end);

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
	Relation	rel = open_log(PG_GETARG_OID(0), ExclusiveLock);
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
