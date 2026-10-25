/*-------------------------------------------------------------------------
 *
 * jsonb_op.c
 *	 Special operators for jsonb only, used by various index access methods
 *
 * Copyright (c) 2014-2026, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/jsonb_op.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_jsonb.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/jsonb.h"
#include "varatt.h"

Datum
jsonb_exists(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb;
	text	   *key = PG_GETARG_TEXT_PP(1);
	JsonbValue	kval;
	JsonbValue *v = NULL;

	/*
	 * A versioned table's document stored as a JSON tree: an object's key
	 * looked up in the tree, an array's string elements read from it
	 * (prolly_jsonb.h).
	 */
	if (VARATT_IS_EXTERNAL_ONDISK_HASH(DatumGetPointer(PG_GETARG_DATUM(0))))
	{
		char	   *k = text_to_cstring(key);
		bool		found;

		if (prolly_jsonb_tree_exists(PG_GETARG_DATUM(0), &k, 1, false, &found))
			PG_RETURN_BOOL(found);
	}

	jb = PG_GETARG_JSONB_P(0);

	/*
	 * We only match Object keys (which are naturally always Strings), or
	 * string elements in arrays.  In particular, we do not match non-string
	 * scalar elements.  Existence of a key/element is only considered at the
	 * top level.  No recursion occurs.
	 */
	kval.type = jbvString;
	kval.val.string.val = VARDATA_ANY(key);
	kval.val.string.len = VARSIZE_ANY_EXHDR(key);

	v = findJsonbValueFromContainer(&jb->root,
									JB_FOBJECT | JB_FARRAY,
									&kval);

	PG_RETURN_BOOL(v != NULL);
}

/*
 * keys ?| (all: ?&) of a versioned table's document stored as a JSON tree,
 * found in the tree (prolly_jsonb.h): false if doc, as stored, isn't one.
 * NULL keys are passed over, as below.
 */
static bool
jsonb_tree_exists(Datum doc, ArrayType *keys, bool all, bool *result)
{
	Datum	   *key_datums;
	bool	   *key_nulls;
	int			elem_count;
	char	  **k;
	int			n = 0;

	if (!VARATT_IS_EXTERNAL_ONDISK_HASH(DatumGetPointer(doc)))
		return false;
	deconstruct_array_builtin(keys, TEXTOID, &key_datums, &key_nulls, &elem_count);
	k = palloc(sizeof(char *) * Max(elem_count, 1));
	for (int i = 0; i < elem_count; i++)
		if (!key_nulls[i])
			k[n++] = TextDatumGetCString(key_datums[i]);
	return prolly_jsonb_tree_exists(doc, k, n, all, result);
}

Datum
jsonb_exists_any(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb;
	ArrayType  *keys = PG_GETARG_ARRAYTYPE_P(1);
	int			i;
	Datum	   *key_datums;
	bool	   *key_nulls;
	int			elem_count;
	bool		found;

	if (jsonb_tree_exists(PG_GETARG_DATUM(0), keys, false, &found))
		PG_RETURN_BOOL(found);
	jb = PG_GETARG_JSONB_P(0);

	deconstruct_array_builtin(keys, TEXTOID, &key_datums, &key_nulls, &elem_count);

	for (i = 0; i < elem_count; i++)
	{
		JsonbValue	strVal;

		if (key_nulls[i])
			continue;

		strVal.type = jbvString;
		/* We rely on the array elements not being toasted */
		strVal.val.string.val = VARDATA_ANY(DatumGetPointer(key_datums[i]));
		strVal.val.string.len = VARSIZE_ANY_EXHDR(DatumGetPointer(key_datums[i]));

		if (findJsonbValueFromContainer(&jb->root,
										JB_FOBJECT | JB_FARRAY,
										&strVal) != NULL)
			PG_RETURN_BOOL(true);
	}

	PG_RETURN_BOOL(false);
}

Datum
jsonb_exists_all(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb;
	ArrayType  *keys = PG_GETARG_ARRAYTYPE_P(1);
	int			i;
	Datum	   *key_datums;
	bool	   *key_nulls;
	int			elem_count;
	bool		found;

	if (jsonb_tree_exists(PG_GETARG_DATUM(0), keys, true, &found))
		PG_RETURN_BOOL(found);
	jb = PG_GETARG_JSONB_P(0);

	deconstruct_array_builtin(keys, TEXTOID, &key_datums, &key_nulls, &elem_count);

	for (i = 0; i < elem_count; i++)
	{
		JsonbValue	strVal;

		if (key_nulls[i])
			continue;

		strVal.type = jbvString;
		/* We rely on the array elements not being toasted */
		strVal.val.string.val = VARDATA_ANY(DatumGetPointer(key_datums[i]));
		strVal.val.string.len = VARSIZE_ANY_EXHDR(DatumGetPointer(key_datums[i]));

		if (findJsonbValueFromContainer(&jb->root,
										JB_FOBJECT | JB_FARRAY,
										&strVal) == NULL)
			PG_RETURN_BOOL(false);
	}

	PG_RETURN_BOOL(true);
}

Datum
jsonb_contains(PG_FUNCTION_ARGS)
{
	Jsonb	   *val = PG_GETARG_JSONB_P(0);
	Jsonb	   *tmpl = PG_GETARG_JSONB_P(1);

	JsonbIterator *it1,
			   *it2;

	if (JB_ROOT_IS_OBJECT(val) != JB_ROOT_IS_OBJECT(tmpl))
		PG_RETURN_BOOL(false);

	it1 = JsonbIteratorInit(&val->root);
	it2 = JsonbIteratorInit(&tmpl->root);

	PG_RETURN_BOOL(JsonbDeepContains(&it1, &it2));
}

Datum
jsonb_contained(PG_FUNCTION_ARGS)
{
	/* Commutator of "contains" */
	Jsonb	   *tmpl = PG_GETARG_JSONB_P(0);
	Jsonb	   *val = PG_GETARG_JSONB_P(1);

	JsonbIterator *it1,
			   *it2;

	if (JB_ROOT_IS_OBJECT(val) != JB_ROOT_IS_OBJECT(tmpl))
		PG_RETURN_BOOL(false);

	it1 = JsonbIteratorInit(&val->root);
	it2 = JsonbIteratorInit(&tmpl->root);

	PG_RETURN_BOOL(JsonbDeepContains(&it1, &it2));
}

Datum
jsonb_ne(PG_FUNCTION_ARGS)
{
	Jsonb	   *jba = PG_GETARG_JSONB_P(0);
	Jsonb	   *jbb = PG_GETARG_JSONB_P(1);
	bool		res;

	res = (compareJsonbContainers(&jba->root, &jbb->root) != 0);

	PG_FREE_IF_COPY(jba, 0);
	PG_FREE_IF_COPY(jbb, 1);
	PG_RETURN_BOOL(res);
}

/*
 * B-Tree operator class operators, support function
 */
Datum
jsonb_lt(PG_FUNCTION_ARGS)
{
	Jsonb	   *jba = PG_GETARG_JSONB_P(0);
	Jsonb	   *jbb = PG_GETARG_JSONB_P(1);
	bool		res;

	res = (compareJsonbContainers(&jba->root, &jbb->root) < 0);

	PG_FREE_IF_COPY(jba, 0);
	PG_FREE_IF_COPY(jbb, 1);
	PG_RETURN_BOOL(res);
}

Datum
jsonb_gt(PG_FUNCTION_ARGS)
{
	Jsonb	   *jba = PG_GETARG_JSONB_P(0);
	Jsonb	   *jbb = PG_GETARG_JSONB_P(1);
	bool		res;

	res = (compareJsonbContainers(&jba->root, &jbb->root) > 0);

	PG_FREE_IF_COPY(jba, 0);
	PG_FREE_IF_COPY(jbb, 1);
	PG_RETURN_BOOL(res);
}

Datum
jsonb_le(PG_FUNCTION_ARGS)
{
	Jsonb	   *jba = PG_GETARG_JSONB_P(0);
	Jsonb	   *jbb = PG_GETARG_JSONB_P(1);
	bool		res;

	res = (compareJsonbContainers(&jba->root, &jbb->root) <= 0);

	PG_FREE_IF_COPY(jba, 0);
	PG_FREE_IF_COPY(jbb, 1);
	PG_RETURN_BOOL(res);
}

Datum
jsonb_ge(PG_FUNCTION_ARGS)
{
	Jsonb	   *jba = PG_GETARG_JSONB_P(0);
	Jsonb	   *jbb = PG_GETARG_JSONB_P(1);
	bool		res;

	res = (compareJsonbContainers(&jba->root, &jbb->root) >= 0);

	PG_FREE_IF_COPY(jba, 0);
	PG_FREE_IF_COPY(jbb, 1);
	PG_RETURN_BOOL(res);
}

Datum
jsonb_eq(PG_FUNCTION_ARGS)
{
	Jsonb	   *jba = PG_GETARG_JSONB_P(0);
	Jsonb	   *jbb = PG_GETARG_JSONB_P(1);
	bool		res;

	res = (compareJsonbContainers(&jba->root, &jbb->root) == 0);

	PG_FREE_IF_COPY(jba, 0);
	PG_FREE_IF_COPY(jbb, 1);
	PG_RETURN_BOOL(res);
}

Datum
jsonb_cmp(PG_FUNCTION_ARGS)
{
	Jsonb	   *jba = PG_GETARG_JSONB_P(0);
	Jsonb	   *jbb = PG_GETARG_JSONB_P(1);
	int			res;

	res = compareJsonbContainers(&jba->root, &jbb->root);

	PG_FREE_IF_COPY(jba, 0);
	PG_FREE_IF_COPY(jbb, 1);
	PG_RETURN_INT32(res);
}

/*
 * Hash operator class jsonb hashing function
 */
Datum
jsonb_hash(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	JsonbIterator *it;
	JsonbValue	v;
	JsonbIteratorToken r;
	uint32		hash = 0;

	if (JB_ROOT_COUNT(jb) == 0)
		PG_RETURN_INT32(0);

	it = JsonbIteratorInit(&jb->root);

	while ((r = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		switch (r)
		{
				/* Rotation is left to JsonbHashScalarValue() */
			case WJB_BEGIN_ARRAY:
				hash ^= JB_FARRAY;
				break;
			case WJB_BEGIN_OBJECT:
				hash ^= JB_FOBJECT;
				break;
			case WJB_KEY:
			case WJB_VALUE:
			case WJB_ELEM:
				JsonbHashScalarValue(&v, &hash);
				break;
			case WJB_END_ARRAY:
			case WJB_END_OBJECT:
				break;
			default:
				elog(ERROR, "invalid JsonbIteratorNext rc: %d", (int) r);
		}
	}

	PG_FREE_IF_COPY(jb, 0);
	PG_RETURN_INT32(hash);
}

Datum
jsonb_hash_extended(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	uint64		seed = PG_GETARG_INT64(1);
	JsonbIterator *it;
	JsonbValue	v;
	JsonbIteratorToken r;
	uint64		hash = 0;

	if (JB_ROOT_COUNT(jb) == 0)
		PG_RETURN_UINT64(seed);

	it = JsonbIteratorInit(&jb->root);

	while ((r = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		switch (r)
		{
				/* Rotation is left to JsonbHashScalarValueExtended() */
			case WJB_BEGIN_ARRAY:
				hash ^= ((uint64) JB_FARRAY) << 32 | JB_FARRAY;
				break;
			case WJB_BEGIN_OBJECT:
				hash ^= ((uint64) JB_FOBJECT) << 32 | JB_FOBJECT;
				break;
			case WJB_KEY:
			case WJB_VALUE:
			case WJB_ELEM:
				JsonbHashScalarValueExtended(&v, &hash, seed);
				break;
			case WJB_END_ARRAY:
			case WJB_END_OBJECT:
				break;
			default:
				elog(ERROR, "invalid JsonbIteratorNext rc: %d", (int) r);
		}
	}

	PG_FREE_IF_COPY(jb, 0);
	PG_RETURN_UINT64(hash);
}
