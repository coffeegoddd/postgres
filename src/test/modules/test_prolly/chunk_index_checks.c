/*-------------------------------------------------------------------------
 *
 * chunk_index_checks.c
 *	  SQL-callable access to the history store's index and chunks
 *	  (prolly/chunk_index.h, prolly/chunk_store.h), for their tests.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/test/modules/test_prolly/chunk_index_checks.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "fmgr.h"
#include "funcapi.h"
#include "prolly/chunk_format.h"
#include "prolly/chunk_log.h"
#include "prolly/chunk_pin.h"
#include "prolly/chunk_store.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "varatt.h"

PG_FUNCTION_INFO_V1(test_chunk_put);
PG_FUNCTION_INFO_V1(test_chunk_get);
PG_FUNCTION_INFO_V1(test_chunk_has);
PG_FUNCTION_INFO_V1(test_chunk_index_entry);
PG_FUNCTION_INFO_V1(test_chunk_index_adjust);
PG_FUNCTION_INFO_V1(test_chunk_index_remove);
PG_FUNCTION_INFO_V1(test_chunk_index_shape);
PG_FUNCTION_INFO_V1(test_chunk_index_unfrozen);
PG_FUNCTION_INFO_V1(test_chunk_format_refs);
PG_FUNCTION_INFO_V1(test_chunk_ref_root);
PG_FUNCTION_INFO_V1(test_chunk_unref_root);
PG_FUNCTION_INFO_V1(test_chunk_check);
PG_FUNCTION_INFO_V1(test_chunk_free);
PG_FUNCTION_INFO_V1(test_chunk_free_unheld);
PG_FUNCTION_INFO_V1(test_chunk_hold);

static Relation
open_index(Oid relid, LOCKMODE mode)
{
	Relation	rel = relation_open(relid, mode);

	if (!RelationIsChunkIndex(rel))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a chunk index relation",
						RelationGetRelationName(rel))));
	return rel;
}

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

static const uint8 *
arg_hash(bytea *h)
{
	if (VARSIZE_ANY_EXHDR(h) != CHUNK_HASH_LEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a chunk's hash is %d bytes", CHUNK_HASH_LEN)));
	return (const uint8 *) VARDATA_ANY(h);
}

static Datum
entry_record(FunctionCallInfo fcinfo, Datum *values, int n)
{
	TupleDesc	desc;
	bool		nulls[4] = {false, false, false, false};

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	Assert(desc->natts == n);
	return HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc), values, nulls));
}

/* test_chunk_put(log, idx, hash, data) returns bool */
Datum
test_chunk_put(PG_FUNCTION_ARGS)
{
	Relation	log = open_log(PG_GETARG_OID(0), RowExclusiveLock);
	Relation	idx = open_index(PG_GETARG_OID(1), RowExclusiveLock);
	bytea	   *data = PG_GETARG_BYTEA_PP(3);
	bool		stored;

	stored = ChunkStorePut(log, idx, arg_hash(PG_GETARG_BYTEA_PP(2)),
						   VARDATA_ANY(data), VARSIZE_ANY_EXHDR(data));
	relation_close(idx, NoLock);
	relation_close(log, NoLock);
	PG_RETURN_BOOL(stored);
}

/* test_chunk_get(log, idx, hash) returns bytea, NULL if absent */
Datum
test_chunk_get(PG_FUNCTION_ARGS)
{
	Relation	log = open_log(PG_GETARG_OID(0), AccessShareLock);
	Relation	idx = open_index(PG_GETARG_OID(1), AccessShareLock);
	uint32		len;
	void	   *bytes = ChunkStoreGet(log, idx, arg_hash(PG_GETARG_BYTEA_PP(2)), &len);
	bytea	   *out;

	relation_close(idx, AccessShareLock);
	relation_close(log, AccessShareLock);
	if (bytes == NULL)
		PG_RETURN_NULL();
	out = palloc(VARHDRSZ + len);
	SET_VARSIZE(out, VARHDRSZ + len);
	memcpy(VARDATA(out), bytes, len);
	PG_RETURN_BYTEA_P(out);
}

/* test_chunk_has(log, idx, hash) returns bool */
Datum
test_chunk_has(PG_FUNCTION_ARGS)
{
	Relation	log = open_log(PG_GETARG_OID(0), AccessShareLock);
	Relation	idx = open_index(PG_GETARG_OID(1), AccessShareLock);
	bool		has = ChunkStoreHas(log, idx, arg_hash(PG_GETARG_BYTEA_PP(2)));

	relation_close(idx, AccessShareLock);
	relation_close(log, AccessShareLock);
	PG_RETURN_BOOL(has);
}

/* test_chunk_index_entry(idx, hash, OUT len, OUT loc, OUT refs, OUT roots) */
Datum
test_chunk_index_entry(PG_FUNCTION_ARGS)
{
	Relation	idx = open_index(PG_GETARG_OID(0), AccessShareLock);
	ChunkIndexEntry e;
	Datum		values[4];
	bool		found = ChunkIndex->lookup(idx, arg_hash(PG_GETARG_BYTEA_PP(1)), &e);

	relation_close(idx, AccessShareLock);
	if (!found)
		PG_RETURN_NULL();
	values[0] = Int32GetDatum((int32) e.len);
	values[1] = Int64GetDatum((int64) e.loc);
	values[2] = Int64GetDatum((int64) e.refs);
	values[3] = Int64GetDatum((int64) e.roots);
	return entry_record(fcinfo, values, 4);
}

/* test_chunk_index_adjust(idx, hash, drefs, droots, OUT refs, OUT roots) */
Datum
test_chunk_index_adjust(PG_FUNCTION_ARGS)
{
	Relation	idx = open_index(PG_GETARG_OID(0), RowExclusiveLock);
	ChunkIndexEntry e;
	Datum		values[2];
	bool		found = ChunkIndex->adjust(idx, arg_hash(PG_GETARG_BYTEA_PP(1)),
										   PG_GETARG_INT32(2), PG_GETARG_INT32(3), &e);

	relation_close(idx, NoLock);
	if (!found)
		PG_RETURN_NULL();
	values[0] = Int64GetDatum((int64) e.refs);
	values[1] = Int64GetDatum((int64) e.roots);
	return entry_record(fcinfo, values, 2);
}

/* test_chunk_index_remove(idx, hash) returns bool */
Datum
test_chunk_index_remove(PG_FUNCTION_ARGS)
{
	Relation	idx = open_index(PG_GETARG_OID(0), RowExclusiveLock);
	bool		removed = ChunkIndex->remove(idx, arg_hash(PG_GETARG_BYTEA_PP(1)));

	relation_close(idx, NoLock);
	PG_RETURN_BOOL(removed);
}

/* test_chunk_index_shape(idx, OUT buckets, OUT pages, OUT entries) */
Datum
test_chunk_index_shape(PG_FUNCTION_ARGS)
{
	Relation	idx = open_index(PG_GETARG_OID(0), AccessShareLock);
	uint32		buckets,
				pages;
	uint64		entries;
	Datum		values[3];

	ChunkIndexShape(idx, &buckets, &pages, &entries);
	relation_close(idx, AccessShareLock);
	values[0] = Int32GetDatum((int32) buckets);
	values[1] = Int32GetDatum((int32) pages);
	values[2] = Int64GetDatum((int64) entries);
	return entry_record(fcinfo, values, 3);
}

/* test_chunk_index_unfrozen(idx) returns int8 */
Datum
test_chunk_index_unfrozen(PG_FUNCTION_ARGS)
{
	Relation	idx = open_index(PG_GETARG_OID(0), AccessShareLock);
	uint64		n = ChunkIndexUnfrozen(idx);

	relation_close(idx, AccessShareLock);
	PG_RETURN_INT64((int64) n);
}

/*
 * A format for the store's tests: a chunk of 'R', a count n, and n hashes
 * (then anything) references those n chunks; any other chunk is a leaf.
 */
static void
test_refs(const void *data, size_t len, ChunkRefFn fn, void *arg)
{
	const uint8 *b = data;

	if (len < 2 || b[0] != 'R')
		return;
	if (len < 2 + (size_t) b[1] * CHUNK_HASH_LEN)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("a test chunk names %u references and holds fewer", b[1])));
	for (int i = 0; i < b[1]; i++)
		fn(arg, b + 2 + i * CHUNK_HASH_LEN);
}

static const ChunkFormatRoutine TestChunkFormat = {
	.refs = test_refs,
};

/* test_chunk_format_refs(on bool): the test format for this backend, or not */
Datum
test_chunk_format_refs(PG_FUNCTION_ARGS)
{
	ChunkFormat = PG_GETARG_BOOL(0) ? &TestChunkFormat : &LeafChunkFormat;
	PG_RETURN_VOID();
}

static Datum
root_change(FunctionCallInfo fcinfo, bool ref)
{
	Relation	idx = open_index(PG_GETARG_OID(0), RowExclusiveLock);
	ChunkIndexEntry e;
	Datum		values[2];

	if (ref)
		ChunkStoreRefRoot(idx, arg_hash(PG_GETARG_BYTEA_PP(1)), &e);
	else
		ChunkStoreUnrefRoot(idx, arg_hash(PG_GETARG_BYTEA_PP(1)), &e);
	relation_close(idx, NoLock);
	values[0] = Int64GetDatum((int64) e.refs);
	values[1] = Int64GetDatum((int64) e.roots);
	return entry_record(fcinfo, values, 2);
}

/* test_chunk_ref_root(idx, hash, OUT refs, OUT roots) */
Datum
test_chunk_ref_root(PG_FUNCTION_ARGS)
{
	return root_change(fcinfo, true);
}

/* test_chunk_unref_root(idx, hash, OUT refs, OUT roots) */
Datum
test_chunk_unref_root(PG_FUNCTION_ARGS)
{
	return root_change(fcinfo, false);
}

/* test_chunk_check(log, idx, OUT ok, OUT chunks, OUT unheld, OUT why) */
Datum
test_chunk_check(PG_FUNCTION_ARGS)
{
	Relation	log = open_log(PG_GETARG_OID(0), AccessShareLock);
	Relation	idx = open_index(PG_GETARG_OID(1), AccessShareLock);
	uint64		chunks,
				unheld;
	char	   *why;
	bool		ok = ChunkStoreCheck(log, idx, &chunks, &unheld, &why);
	TupleDesc	desc;
	Datum		values[4];
	bool		nulls[4] = {false, false, false, false};

	relation_close(idx, AccessShareLock);
	relation_close(log, AccessShareLock);
	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	values[0] = BoolGetDatum(ok);
	values[1] = Int64GetDatum((int64) chunks);
	values[2] = Int64GetDatum((int64) unheld);
	if (why)
		values[3] = CStringGetTextDatum(why);
	else
		nulls[3] = true;
	return HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc), values, nulls));
}

/* test_chunk_free(log, idx, hash) returns int8: the chunks freed */
Datum
test_chunk_free(PG_FUNCTION_ARGS)
{
	Relation	log = open_log(PG_GETARG_OID(0), AccessShareLock);
	Relation	idx = open_index(PG_GETARG_OID(1), RowExclusiveLock);
	uint64		freed = ChunkStoreFree(log, idx, arg_hash(PG_GETARG_BYTEA_PP(2)), 1);

	relation_close(idx, NoLock);
	relation_close(log, AccessShareLock);
	PG_RETURN_INT64((int64) freed);
}

/* test_chunk_free_unheld(log, idx) returns int8: the chunks freed */
Datum
test_chunk_free_unheld(PG_FUNCTION_ARGS)
{
	Relation	log = open_log(PG_GETARG_OID(0), AccessShareLock);
	Relation	idx = open_index(PG_GETARG_OID(1), RowExclusiveLock);
	uint64		freed = ChunkStoreFreeUnheld(log, idx);

	relation_close(idx, NoLock);
	relation_close(log, AccessShareLock);
	PG_RETURN_INT64((int64) freed);
}

/* test_chunk_hold(idx, hash): holds the chunk until the transaction ends */
Datum
test_chunk_hold(PG_FUNCTION_ARGS)
{
	Relation	idx = open_index(PG_GETARG_OID(0), RowExclusiveLock);
	bool		held = ChunkStoreHold(idx, arg_hash(PG_GETARG_BYTEA_PP(1)));

	relation_close(idx, NoLock);
	PG_RETURN_BOOL(held);
}
