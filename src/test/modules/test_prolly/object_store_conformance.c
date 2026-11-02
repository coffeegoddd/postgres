/*-------------------------------------------------------------------------
 *
 * object_store_conformance.c
 *	  SQL-callable checks of the object stores (prolly/object_store.h),
 *	  each named by its implementation: conformance runs the same tests
 *	  against every one registered.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/test/modules/test_prolly/object_store_conformance.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "funcapi.h"
#include "prolly/object_store.h"
#include "prolly/registry.h"
#include "utils/builtins.h"
#include "utils/tuplestore.h"
#include "varatt.h"

PG_FUNCTION_INFO_V1(test_os_stores);
PG_FUNCTION_INFO_V1(test_os_put);
PG_FUNCTION_INFO_V1(test_os_read);
PG_FUNCTION_INFO_V1(test_os_size);
PG_FUNCTION_INFO_V1(test_os_list);
PG_FUNCTION_INFO_V1(test_os_remove);

static ObjectStore *
store_arg(FunctionCallInfo fcinfo)
{
	return ObjectStoreOpen(text_to_cstring(PG_GETARG_TEXT_PP(0)),
						   text_to_cstring(PG_GETARG_TEXT_PP(1)));
}

/* test_os_stores() returns setof text: the implementations registered */
Datum
test_os_stores(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	ListCell   *lc;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	foreach(lc, ObjectStoreList())
	{
		const ProllyImplementation *impl = lfirst(lc);
		Datum		value = CStringGetTextDatum(impl->name);
		bool		null = false;

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, &value, &null);
	}
	return (Datum) 0;
}

/* test_os_put(store, location, name, data) */
Datum
test_os_put(PG_FUNCTION_ARGS)
{
	ObjectStore *os = store_arg(fcinfo);
	bytea	   *data = PG_GETARG_BYTEA_PP(3);

	os->routine->put(os, text_to_cstring(PG_GETARG_TEXT_PP(2)),
					 VARDATA_ANY(data), VARSIZE_ANY_EXHDR(data));
	os->routine->close(os);
	PG_RETURN_VOID();
}

/* test_os_read(store, location, name, off, len) returns bytea */
Datum
test_os_read(PG_FUNCTION_ARGS)
{
	ObjectStore *os = store_arg(fcinfo);
	int64		off = PG_GETARG_INT64(3);
	int32		len = PG_GETARG_INT32(4);
	bytea	   *out;

	if (off < 0 || len < 0)
		elog(ERROR, "a range from %lld, %d long", (long long) off, len);
	out = palloc(VARHDRSZ + len);
	SET_VARSIZE(out, VARHDRSZ + len);
	os->routine->read(os, text_to_cstring(PG_GETARG_TEXT_PP(2)), (uint64) off,
					  VARDATA(out), len);
	os->routine->close(os);
	PG_RETURN_BYTEA_P(out);
}

/* test_os_size(store, location, name) returns int8, NULL if not there */
Datum
test_os_size(PG_FUNCTION_ARGS)
{
	ObjectStore *os = store_arg(fcinfo);
	uint64		size;

	bool		there = os->routine->size(os, text_to_cstring(PG_GETARG_TEXT_PP(2)), &size);

	os->routine->close(os);
	if (!there)
		PG_RETURN_NULL();
	PG_RETURN_INT64((int64) size);
}

static void
list_one(void *arg, const char *name, uint64 size)
{
	ReturnSetInfo *rsinfo = arg;
	Datum		values[2] = {CStringGetTextDatum(name), Int64GetDatum((int64) size)};
	bool		nulls[2] = {false, false};

	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

/* test_os_list(store, location, prefix) returns setof (name, size) */
Datum
test_os_list(PG_FUNCTION_ARGS)
{
	ObjectStore *os = store_arg(fcinfo);

	InitMaterializedSRF(fcinfo, 0);
	os->routine->list(os, text_to_cstring(PG_GETARG_TEXT_PP(2)), list_one,
					  fcinfo->resultinfo);
	os->routine->close(os);
	return (Datum) 0;
}

/* test_os_remove(store, location, name) */
Datum
test_os_remove(PG_FUNCTION_ARGS)
{
	ObjectStore *os = store_arg(fcinfo);

	os->routine->remove(os, text_to_cstring(PG_GETARG_TEXT_PP(2)));
	os->routine->close(os);
	PG_RETURN_VOID();
}
