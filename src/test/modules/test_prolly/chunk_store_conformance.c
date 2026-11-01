/*-------------------------------------------------------------------------
 *
 * chunk_store_conformance.c
 *	  SQL functions over the engine's chunk store interface
 *	  (prolly/chunkstore.h), for its conformance suite.
 *
 * Each takes the name of a registered store.  The suite
 * (sql/chunk_store_suite.inc) runs against each store in a
 * file of its own; chunk_store_stores lists those registered, so that a
 * store registered without its file shows.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/test_prolly/chunk_store_conformance.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "fmgr.h"
#include "funcapi.h"
#include "prolly/chunkstore.h"
#include "prolly/registry.h"
#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/tuplestore.h"
#include "varatt.h"

PG_FUNCTION_INFO_V1(test_chunk_stores);
PG_FUNCTION_INFO_V1(test_cs_put);
PG_FUNCTION_INFO_V1(test_cs_get);
PG_FUNCTION_INFO_V1(test_cs_get_many);
PG_FUNCTION_INFO_V1(test_cs_put_many);
PG_FUNCTION_INFO_V1(test_cs_has);
PG_FUNCTION_INFO_V1(test_cs_ref_root);
PG_FUNCTION_INFO_V1(test_cs_unref_root);
PG_FUNCTION_INFO_V1(test_cs_counts);
PG_FUNCTION_INFO_V1(test_cs_free);
PG_FUNCTION_INFO_V1(test_cs_free_unheld);
PG_FUNCTION_INFO_V1(test_cs_check);
PG_FUNCTION_INFO_V1(test_cs_reset_memory);
PG_FUNCTION_INFO_V1(test_cs_memory_miscount);
PG_FUNCTION_INFO_V1(test_cs_hold);
PG_FUNCTION_INFO_V1(test_cs_visit);
PG_FUNCTION_INFO_V1(test_cs_stats);

static ChunkStore *
store_arg(FunctionCallInfo fcinfo)
{
	return ChunkStoreOpen(text_to_cstring(PG_GETARG_TEXT_PP(0)));
}

static const uint8 *
hash_arg(FunctionCallInfo fcinfo, int n)
{
	bytea	   *b = PG_GETARG_BYTEA_PP(n);

	if (VARSIZE_ANY_EXHDR(b) != CHUNK_HASH_LEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a chunk's hash is %d bytes", CHUNK_HASH_LEN)));
	return (const uint8 *) VARDATA_ANY(b);
}

/* test_chunk_stores(): the stores registered, and which are transactional */
Datum
test_chunk_stores(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);
	foreach_ptr(ProllyImplementation, impl, ChunkStoreList())
	{
		Datum		values[2];
		bool		nulls[2] = {false, false};

		values[0] = CStringGetTextDatum(impl->name);
		values[1] = BoolGetDatum(((const ChunkStoreRoutine *) impl->routine)->transactional);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

/* test_cs_put(store, data, OUT stored, OUT hash): whether it stored it, and its name */
Datum
test_cs_put(PG_FUNCTION_ARGS)
{
	bytea	   *data = PG_GETARG_BYTEA_PP(1);
	uint8		hash[CHUNK_HASH_LEN];
	bytea	   *h = palloc(VARHDRSZ + CHUNK_HASH_LEN);
	TupleDesc	desc;
	Datum		values[2];
	bool		nulls[2] = {false, false};

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	values[0] = BoolGetDatum(cs_put(store_arg(fcinfo), VARDATA_ANY(data),
									VARSIZE_ANY_EXHDR(data), hash));
	SET_VARSIZE(h, VARHDRSZ + CHUNK_HASH_LEN);
	memcpy(VARDATA(h), hash, CHUNK_HASH_LEN);
	values[1] = PointerGetDatum(h);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc), values, nulls)));
}

/* hashes bytea[] argument n, flattened */
static uint8 *
hashes_arg(FunctionCallInfo fcinfo, int argno, int *n)
{
	Datum	   *elems;
	bool	   *nulls;
	uint8	   *hashes;

	deconstruct_array(PG_GETARG_ARRAYTYPE_P(argno), BYTEAOID, -1, false,
					  TYPALIGN_INT, &elems, &nulls, n);
	hashes = palloc(Max(*n, 1) * CHUNK_HASH_LEN);
	for (int i = 0; i < *n; i++)
	{
		bytea	   *b;

		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("a chunk's hash is not null")));
		b = DatumGetByteaPP(elems[i]);
		if (VARSIZE_ANY_EXHDR(b) != CHUNK_HASH_LEN)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("a chunk's hash is %d bytes", CHUNK_HASH_LEN)));
		memcpy(hashes + i * CHUNK_HASH_LEN, VARDATA_ANY(b), CHUNK_HASH_LEN);
	}
	return hashes;
}

/* test_cs_get(store, hash): the chunk's bytes, or NULL */
Datum
test_cs_get(PG_FUNCTION_ARGS)
{
	uint32		len;
	void	   *data = cs_get(store_arg(fcinfo), hash_arg(fcinfo, 1), &len);
	bytea	   *out;

	if (data == NULL)
		PG_RETURN_NULL();
	out = palloc(VARHDRSZ + len);
	SET_VARSIZE(out, VARHDRSZ + len);
	memcpy(VARDATA(out), data, len);
	PG_RETURN_BYTEA_P(out);
}

/* test_cs_get_many(store, hashes bytea[]): the chunks' bytes, NULLs for absent */
Datum
test_cs_get_many(PG_FUNCTION_ARGS)
{
	int			n;
	uint8	   *hashes = hashes_arg(fcinfo, 1, &n);
	void	  **out = palloc(Max(n, 1) * sizeof(void *));
	uint32	   *lens = palloc(Max(n, 1) * sizeof(uint32));
	Datum	   *res = palloc(Max(n, 1) * sizeof(Datum));
	bool	   *resnulls = palloc(Max(n, 1) * sizeof(bool));
	int			dims[1];
	int			lbs[1] = {1};

	cs_get_many(store_arg(fcinfo), hashes, n, out, lens);
	for (int i = 0; i < n; i++)
	{
		resnulls[i] = out[i] == NULL;
		if (out[i])
		{
			bytea	   *b = palloc(VARHDRSZ + lens[i]);

			SET_VARSIZE(b, VARHDRSZ + lens[i]);
			memcpy(VARDATA(b), out[i], lens[i]);
			res[i] = PointerGetDatum(b);
		}
	}
	dims[0] = n;
	PG_RETURN_ARRAYTYPE_P(construct_md_array(res, resnulls, 1, dims, lbs,
											 BYTEAOID, -1, false, TYPALIGN_INT));
}

/* test_cs_put_many(store, hashes bytea[], datas bytea[]): how many it stored */
Datum
test_cs_put_many(PG_FUNCTION_ARGS)
{
	int			n;
	uint8	   *hashes = hashes_arg(fcinfo, 1, &n);
	Datum	   *elems;
	bool	   *nulls;
	int			ndatas;
	const void **datas;
	uint32	   *lens;

	deconstruct_array(PG_GETARG_ARRAYTYPE_P(2), BYTEAOID, -1, false,
					  TYPALIGN_INT, &elems, &nulls, &ndatas);
	if (ndatas != n)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%d hashes for %d chunks", n, ndatas)));
	datas = palloc(Max(n, 1) * sizeof(void *));
	lens = palloc(Max(n, 1) * sizeof(uint32));
	for (int i = 0; i < n; i++)
	{
		bytea	   *b;

		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("a chunk's bytes are not null")));
		b = DatumGetByteaPP(elems[i]);
		datas[i] = VARDATA_ANY(b);
		lens[i] = VARSIZE_ANY_EXHDR(b);
	}
	PG_RETURN_INT64((int64) cs_put_many(store_arg(fcinfo), n, hashes, datas, lens));
}

Datum
test_cs_has(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(cs_has(store_arg(fcinfo), hash_arg(fcinfo, 1)));
}

Datum
test_cs_ref_root(PG_FUNCTION_ARGS)
{
	cs_ref_root(store_arg(fcinfo), hash_arg(fcinfo, 1));
	PG_RETURN_VOID();
}

Datum
test_cs_unref_root(PG_FUNCTION_ARGS)
{
	cs_unref_root(store_arg(fcinfo), hash_arg(fcinfo, 1));
	PG_RETURN_VOID();
}

/* test_cs_counts(store, hash, OUT refs, OUT roots): NULLs if absent */
Datum
test_cs_counts(PG_FUNCTION_ARGS)
{
	TupleDesc	desc;
	ChunkCounts c;
	Datum		values[2];
	bool		nulls[2] = {false, false};

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	if (cs_counts(store_arg(fcinfo), hash_arg(fcinfo, 1), &c))
	{
		values[0] = Int64GetDatum(c.refs);
		values[1] = Int64GetDatum(c.roots);
	}
	else
		nulls[0] = nulls[1] = true;
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc), values, nulls)));
}

/* test_cs_free(store, hashes bytea[]): how many it freed, the cascade's included */
Datum
test_cs_free(PG_FUNCTION_ARGS)
{
	int			n;
	uint8	   *hashes = hashes_arg(fcinfo, 1, &n);

	PG_RETURN_INT64((int64) cs_free(store_arg(fcinfo), hashes, n));
}

Datum
test_cs_free_unheld(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64((int64) cs_free_unheld(store_arg(fcinfo)));
}

/* test_cs_check(store, OUT ok, OUT chunks, OUT unheld, OUT why): the audit */
Datum
test_cs_check(PG_FUNCTION_ARGS)
{
	uint64		chunks,
				unheld;
	char	   *why = NULL;
	TupleDesc	desc;
	Datum		values[4];
	bool		nulls[4] = {false, false, false, false};

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	values[0] = BoolGetDatum(cs_check(store_arg(fcinfo), &chunks, &unheld, &why));
	values[1] = Int64GetDatum((int64) chunks);
	values[2] = Int64GetDatum((int64) unheld);
	if (why)
		values[3] = CStringGetTextDatum(why);
	else
		nulls[3] = true;
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc), values, nulls)));
}

Datum
test_cs_reset_memory(PG_FUNCTION_ARGS)
{
	MemoryChunkStoreReset();
	PG_RETURN_VOID();
}

/* test_cs_memory_miscount(hash): a reference too many counted, in "memory" */
Datum
test_cs_memory_miscount(PG_FUNCTION_ARGS)
{
	MemoryChunkStoreMiscount(hash_arg(fcinfo, 0));
	PG_RETURN_VOID();
}

/* test_cs_hold(store, hash): whether the store has it, held if so */
Datum
test_cs_hold(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(cs_hold(store_arg(fcinfo), hash_arg(fcinfo, 1)));
}

typedef struct VisitRows
{
	ReturnSetInfo *rsinfo;
} VisitRows;

static void
visit_row(void *arg, const uint8 *hash, uint32 len, const ChunkCounts *counts)
{
	ReturnSetInfo *rsinfo = ((VisitRows *) arg)->rsinfo;
	bytea	   *h = palloc(VARHDRSZ + CHUNK_HASH_LEN);
	Datum		values[4];
	bool		nulls[4] = {false, false, false, false};

	SET_VARSIZE(h, VARHDRSZ + CHUNK_HASH_LEN);
	memcpy(VARDATA(h), hash, CHUNK_HASH_LEN);
	values[0] = PointerGetDatum(h);
	values[1] = Int64GetDatum(len);
	values[2] = Int64GetDatum(counts->refs);
	values[3] = Int64GetDatum(counts->roots);
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

/* test_cs_visit(store): each chunk this transaction sees, with its counts */
Datum
test_cs_visit(PG_FUNCTION_ARGS)
{
	VisitRows	rows;

	InitMaterializedSRF(fcinfo, 0);
	rows.rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	cs_visit(store_arg(fcinfo), visit_row, &rows);
	return (Datum) 0;
}

/* test_cs_stats(store, reset): what the store was asked; then reset, if so */
Datum
test_cs_stats(PG_FUNCTION_ARGS)
{
	ChunkStore *cs = store_arg(fcinfo);
	const ChunkStoreStats *st = cs_stats(cs);
	TupleDesc	desc;
	Datum		values[7];
	bool		nulls[7] = {false, false, false, false, false, false, false};

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	values[0] = Int64GetDatum((int64) st->gets);
	values[1] = Int64GetDatum((int64) st->found);
	values[2] = Int64GetDatum((int64) st->puts);
	values[3] = Int64GetDatum((int64) st->stored);
	values[4] = Int64GetDatum((int64) st->holds);
	values[5] = Int64GetDatum((int64) st->held);
	values[6] = Int64GetDatum((int64) st->freed);
	if (PG_GETARG_BOOL(1))
		cs_stats_reset(cs);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc), values, nulls)));
}
