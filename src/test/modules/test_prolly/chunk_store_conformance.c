/*-------------------------------------------------------------------------
 *
 * chunk_store_conformance.c
 *	  SQL functions over the engine's chunk store interface
 *	  (prolly/chunkstore.h), for its conformance suite.
 *
 * Each takes the name of a registered store.  The suite
 * (sql/include/chunk_store_conformance.sql) runs against each store in a
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
#include "utils/builtins.h"
#include "utils/tuplestore.h"
#include "varatt.h"

PG_FUNCTION_INFO_V1(test_chunk_stores);
PG_FUNCTION_INFO_V1(test_cs_put);
PG_FUNCTION_INFO_V1(test_cs_get);
PG_FUNCTION_INFO_V1(test_cs_has);
PG_FUNCTION_INFO_V1(test_cs_ref_root);
PG_FUNCTION_INFO_V1(test_cs_unref_root);
PG_FUNCTION_INFO_V1(test_cs_counts);
PG_FUNCTION_INFO_V1(test_cs_free);
PG_FUNCTION_INFO_V1(test_cs_free_unheld);
PG_FUNCTION_INFO_V1(test_cs_check);
PG_FUNCTION_INFO_V1(test_cs_reset_memory);

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

/* test_cs_put(store, hash, data): true if it stored the chunk */
Datum
test_cs_put(PG_FUNCTION_ARGS)
{
	bytea	   *data = PG_GETARG_BYTEA_PP(2);

	PG_RETURN_BOOL(cs_put(store_arg(fcinfo), hash_arg(fcinfo, 1),
						  VARDATA_ANY(data), VARSIZE_ANY_EXHDR(data)));
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

/* test_cs_free(store, hash): how many it freed, the cascade's included */
Datum
test_cs_free(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64((int64) cs_free(store_arg(fcinfo), hash_arg(fcinfo, 1), 1));
}

Datum
test_cs_free_unheld(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64((int64) cs_free_unheld(store_arg(fcinfo)));
}

/* test_cs_check(store): the audit's verdict, and what is wrong */
Datum
test_cs_check(PG_FUNCTION_ARGS)
{
	uint64		chunks,
				unheld;
	char	   *why = NULL;

	if (!cs_check(store_arg(fcinfo), &chunks, &unheld, &why))
		ereport(NOTICE, (errmsg("the store's audit: %s", why)));
	PG_RETURN_BOOL(why == NULL);
}

Datum
test_cs_reset_memory(PG_FUNCTION_ARGS)
{
	MemoryChunkStoreReset();
	PG_RETURN_VOID();
}
