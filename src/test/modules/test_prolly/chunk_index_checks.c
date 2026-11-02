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

#include "utils/guc.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "portability/instr_time.h"
#include "prolly/chunk_format.h"
#include "prolly/chunk_index.h"
#include "prolly/chunk_log.h"
#include "prolly/chunk_store.h"
#include "storage/lmgr.h"
#include "utils/array.h"
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
PG_FUNCTION_INFO_V1(test_chunk_read_bench);
PG_FUNCTION_INFO_V1(test_chunk_compact);
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

	if (!RelationIsAnyChunkLog(rel))
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

/*
 * The test format for every backend from its start (test_prolly.chunk_format_refs,
 * the module preloaded): for programs' sessions, pg_dump's and pg_restore's
 */
static bool format_refs_setting = false;

extern void test_chunk_format_define_setting(void);

static void
assign_format_refs(bool newval, void *extra)
{
	ChunkFormat = newval ? &TestChunkFormat : &LeafChunkFormat;
}

void
test_chunk_format_define_setting(void)
{
	DefineCustomBoolVariable("test_prolly.chunk_format_refs",
							 "Chunks of the test format, with references, for every session.",
							 NULL, &format_refs_setting, false, PGC_SUSET, 0,
							 NULL, assign_format_refs, NULL);
}

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

/*
 * test_chunk_read_bench(log, idx, hashes, how, OUT chunks, OUT bytes, OUT
 * ms): the chunks named read from C, timed, one call each, in the order
 * given ('lookup': the index alone; 'read': the log alone, the chunks'
 * places found first, untimed; 'get': ChunkStoreGet), or ('get_many') a
 * thousand to a ChunkStoreGetMany; every byte read summed, as a reader
 * would touch it
 */
Datum
test_chunk_read_bench(PG_FUNCTION_ARGS)
{
	Relation	log = open_log(PG_GETARG_OID(0), AccessShareLock);
	Relation	idx = open_index(PG_GETARG_OID(1), AccessShareLock);
	ArrayType  *arr = PG_GETARG_ARRAYTYPE_P(2);
	char	   *how = text_to_cstring(PG_GETARG_TEXT_PP(3));
	Datum	   *elems;
	int			n;
	uint8	   *hashes;
	ChunkIndexEntry *places = NULL;
	int			nplaces = 0;
	uint8	   *buf = NULL;
	int64		found = 0;
	uint64		bytes = 0,
				sum = 0;
	instr_time	start,
				took;
	TupleDesc	desc;
	Datum		values[3];
	bool		nulls[3] = {false, false, false};

	/* (a NULL element an error) */
	deconstruct_array(arr, BYTEAOID, -1, false, TYPALIGN_INT, &elems, NULL, &n);
	hashes = palloc((Size) Max(n, 1) * CHUNK_HASH_LEN);
	for (int i = 0; i < n; i++)
		memcpy(hashes + (Size) i * CHUNK_HASH_LEN,
			   arg_hash(DatumGetByteaPP(elems[i])), CHUNK_HASH_LEN);

	/* (the log alone: the places found first, untimed) */
	if (strcmp(how, "read") == 0)
	{
		places = palloc_array(ChunkIndexEntry, Max(n, 1));
		buf = palloc(1024 * 1024);
		for (int i = 0; i < n; i++)
			if (ChunkIndex->lookup(idx, hashes + (Size) i * CHUNK_HASH_LEN, &places[nplaces]) &&
				places[nplaces].len <= 1024 * 1024)
				nplaces++;
	}

	INSTR_TIME_SET_CURRENT(start);
	if (strcmp(how, "lookup") == 0)
	{
		ChunkIndexEntry e;

		for (int i = 0; i < n; i++)
			found += ChunkIndex->lookup(idx, hashes + (Size) i * CHUNK_HASH_LEN, &e);
	}
	else if (strcmp(how, "read") == 0)
	{
		for (int i = 0; i < nplaces; i++)
		{
			ChunkLogOf(log)->read(log, places[i].loc, buf, places[i].len);
			found++;
			bytes += places[i].len;
			for (uint32 j = 0; j < places[i].len; j++)
				sum += buf[j];
		}
	}
	else if (strcmp(how, "get") == 0)
	{
		for (int i = 0; i < n; i++)
		{
			uint32		len;
			uint8	   *data = ChunkStoreGet(log, idx, hashes + (Size) i * CHUNK_HASH_LEN, &len);

			if (data == NULL)
				continue;
			found++;
			bytes += len;
			for (uint32 j = 0; j < len; j++)
				sum += data[j];
			pfree(data);
		}
	}
	else if (strcmp(how, "get_many") == 0)
	{
		void	  **out = palloc(1000 * sizeof(void *));
		uint32	   *lens = palloc(1000 * sizeof(uint32));

		for (int i = 0; i < n; i += 1000)
		{
			int			k = Min(1000, n - i);

			ChunkStoreGetMany(log, idx, hashes + (Size) i * CHUNK_HASH_LEN, k, out, lens);
			for (int m = 0; m < k; m++)
			{
				if (out[m] == NULL)
					continue;
				found++;
				bytes += lens[m];
				for (uint32 j = 0; j < lens[m]; j++)
					sum += ((uint8 *) out[m])[j];
				pfree(out[m]);
			}
		}
	}
	else
		elog(ERROR, "unknown read \"%s\"", how);
	INSTR_TIME_SET_CURRENT(took);
	INSTR_TIME_SUBTRACT(took, start);
	/* (the sum kept, lest the reads be optimized out) */
	if (sum == UINT64_MAX)
		elog(NOTICE, "sum %llu", (unsigned long long) sum);
	if (found < n)
		elog(NOTICE, "%lld of %d chunks not found (or, read, longer than 1 MB)",
			 (long long) (n - found), n);

	relation_close(idx, AccessShareLock);
	relation_close(log, AccessShareLock);
	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	values[0] = Int64GetDatum(found);
	values[1] = Int64GetDatum((int64) bytes);
	values[2] = Float8GetDatum(INSTR_TIME_GET_MILLISEC(took));
	return HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc), values, nulls));
}

/*
 * test_chunk_compact(log, idx, fraction, evacuate, OUT segments, OUT chunks,
 * OUT bytes): the log's segments to compact compacted (the log growing as
 * it must), or its end evacuated into the free segments below, in this
 * transaction
 */
Datum
test_chunk_compact(PG_FUNCTION_ARGS)
{
	Relation	log = open_log(PG_GETARG_OID(0), RowExclusiveLock);
	Relation	idx = open_index(PG_GETARG_OID(1), RowExclusiveLock);
	double		fraction = PG_GETARG_FLOAT8(2);
	bool		evacuate = PG_GETARG_BOOL(3);
	uint64		starts[4096];
	uint64		ends[4096];
	int			n;
	ChunkCompaction *c;
	uint64		moved = 0,
				moved_bytes = 0;
	TupleDesc	desc;
	Datum		values[3];
	bool		nulls[3] = {false, false, false};

	n = evacuate ? ChunkLogOf(log)->evacuable(log, lengthof(starts), starts, ends) :
		ChunkLogOf(log)->compactable(log, fraction, lengthof(starts), starts, ends);
	c = ChunkStoreCompactBegin(log, idx, n, starts, ends, CurrentMemoryContext);
	for (;;)
	{
		uint64		chunks,
					bytes;
		int			looked = ChunkStoreCompactStep(log, idx, c, 256,
												   evacuate && n > 0 ? starts[0] : 0,
												   evacuate ? 0 : MaxBlockNumber,
												   &chunks, &bytes);

		moved += chunks;
		moved_bytes += bytes;
		if (looked <= 0)
			break;
	}
	ChunkStoreCompactEnd(c);
	relation_close(idx, NoLock);
	relation_close(log, NoLock);
	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	values[0] = Int64GetDatum((int64) n);
	values[1] = Int64GetDatum((int64) moved);
	values[2] = Int64GetDatum((int64) moved_bytes);
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
