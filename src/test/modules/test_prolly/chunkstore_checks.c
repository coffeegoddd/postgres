/*--------------------------------------------------------------------------
 *
 * chunkstore_checks.c
 *		Checks of the server's chunk store, from SQL: chunks named by their
 *		SHA-256, as hex text, and the store's root.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_prolly/chunkstore_checks.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_canon.h"
#include "access/prolly_config.h"
#include "access/prolly_chunkstore.h"
#include "access/prolly_hash.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/tuplestore.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "varatt.h"

/* (a plain function: PROLLY_CALL evaluates its routine more than once) */
static inline const ProllyChunkStoreRoutine *
cs(void)
{
	return prolly_chunk_store();
}

static ProllyChunkStore *
store(void)
{
	return PROLLY_CALL(cs(), open, MyDatabaseId,
					   MyDatabaseTableSpace);
}

static text *
hash_text(const ProllyHash *h)
{
	char	   *hex = palloc(h->len * 2 + 1);

	for (int i = 0; i < h->len; i++)
		sprintf(hex + 2 * i, "%02x", h->bytes[i]);
	return cstring_to_text(hex);
}

static void
text_hash(text *t, ProllyHash *h)
{
	char	   *s = text_to_cstring(t);
	size_t		n = strlen(s);

	memset(h, 0, sizeof(*h));
	if (n % 2 != 0 || n / 2 > PROLLY_HASH_MAX_LEN)
		elog(ERROR, "invalid chunk hash \"%s\"", s);
	h->len = n / 2;
	for (size_t i = 0; i < h->len; i++)
	{
		unsigned int b;

		if (sscanf(s + 2 * i, "%2x", &b) != 1)
			elog(ERROR, "invalid chunk hash \"%s\"", s);
		h->bytes[i] = b;
	}
}

static void
sha256(const void *data, Size len, ProllyHash *h)
{
	const ProllyHashRoutine *r = prolly_hash_lookup("sha256");
	ProllyHashCtx *ctx = PROLLY_CALL(r, begin, CurrentMemoryContext);

	PROLLY_CALL(r, update, ctx, data, len);
	PROLLY_CALL(r, finish, ctx, h);
}

/* Put a chunk; returns its hash. */
PG_FUNCTION_INFO_V1(test_chunk_put);
Datum
test_chunk_put(PG_FUNCTION_ARGS)
{
	bytea	   *data = PG_GETARG_BYTEA_PP(0);
	ProllyHash	h;

	sha256(VARDATA_ANY(data), VARSIZE_ANY_EXHDR(data), &h);
	PROLLY_CALL(cs(), put, store(), &h, VARDATA_ANY(data),
				VARSIZE_ANY_EXHDR(data));
	PG_RETURN_TEXT_P(hash_text(&h));
}

/* A chunk's bytes, or NULL if the store has none by that hash. */
PG_FUNCTION_INFO_V1(test_chunk_get);
Datum
test_chunk_get(PG_FUNCTION_ARGS)
{
	ProllyHash	h;
	StringInfoData out;
	bytea	   *result;

	text_hash(PG_GETARG_TEXT_PP(0), &h);
	initStringInfo(&out);
	if (!PROLLY_CALL(cs(), get, store(), &h, &out))
		PG_RETURN_NULL();
	result = palloc(VARHDRSZ + out.len);
	SET_VARSIZE(result, VARHDRSZ + out.len);
	memcpy(VARDATA(result), out.data, out.len);
	PG_RETURN_BYTEA_P(result);
}

PG_FUNCTION_INFO_V1(test_chunk_has);
Datum
test_chunk_has(PG_FUNCTION_ARGS)
{
	ProllyHash	h;

	text_hash(PG_GETARG_TEXT_PP(0), &h);
	PG_RETURN_BOOL(PROLLY_CALL(cs(), has, store(), &h));
}

/* The store's root, or NULL if it never had one. */
PG_FUNCTION_INFO_V1(test_chunk_root);
Datum
test_chunk_root(PG_FUNCTION_ARGS)
{
	ProllyHash	h;

	PROLLY_CALL(cs(), root, store(), &h);
	if (h.len == 0)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(hash_text(&h));
}

/* Move the root from expected (NULL: none) to root; false if it moved. */
PG_FUNCTION_INFO_V1(test_chunk_move_root);
Datum
test_chunk_move_root(PG_FUNCTION_ARGS)
{
	ProllyHash	expected;
	ProllyHash	root;

	memset(&expected, 0, sizeof(expected));
	if (!PG_ARGISNULL(0))
		text_hash(PG_GETARG_TEXT_PP(0), &expected);
	if (PG_ARGISNULL(1))
		elog(ERROR, "a root is needed");
	text_hash(PG_GETARG_TEXT_PP(1), &root);
	PG_RETURN_BOOL(PROLLY_CALL(cs(), move_root, store(),
							   &expected, &root));
}

/* The canonical root of the session branch's versioned schemas, folded. */
PG_FUNCTION_INFO_V1(test_fold_root);
Datum
test_fold_root(PG_FUNCTION_ARGS)
{
	const ProllyCanonRoutine *canon = prolly_canon_lookup("folded");
	ProllyNodeRef root;

	PROLLY_CALL(canon, fold, &root);
	PG_RETURN_TEXT_P(hash_text(&root.hash));
}

static int
memcmp_order(const ProllyKey *a, const ProllyKey *b, void *arg)
{
	int			r = memcmp(a->data, b->data, Min(a->len, b->len));

	return r != 0 ? r : (a->len > b->len) - (a->len < b->len);
}

/*
 * The folded canonical tree's items: each key, and its value with bytes
 * that aren't printable shown as \xNN.
 */
PG_FUNCTION_INFO_V1(test_fold_items);
Datum
test_fold_items(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	const ProllyCanonRoutine *canon = prolly_canon_lookup("folded");
	const ProllyTreeRoutine *tree = prolly_tree_lookup("cow");
	ProllyNodeRef root;
	ProllyTreeEnv env;
	ProllyTreeCursor *c;
	ProllyKey	k;
	ProllyValue v;

	InitMaterializedSRF(fcinfo, 0);
	PROLLY_CALL(canon, fold, &root);
	memset(&env, 0, sizeof(env));
	env.chunk_format = prolly_chunkformat_lookup("basic");
	env.chunker = prolly_chunker_lookup("bytes");
	env.hash = prolly_hash_lookup("sha256");
	env.chunk_store = cs();
	env.chunks = store();
	env.compare = memcmp_order;
	c = PROLLY_CALL(tree, cursor_open, &env, &root, CurrentMemoryContext);
	PROLLY_CALL(tree, cursor_seek, c, NULL, ForwardScanDirection);
	while (PROLLY_CALL(tree, cursor_next, c, &k, &v))
	{
		Datum		values[2];
		bool		nulls[2] = {false, false};
		StringInfoData vs;

		initStringInfo(&vs);
		for (uint32 i = 0; i < v.len; i++)
		{
			if (v.data[i] >= 32 && v.data[i] < 127)
				appendStringInfoChar(&vs, (char) v.data[i]);
			else if (v.data[i] == '\n')
				appendStringInfoChar(&vs, '\n');
			else
				appendStringInfo(&vs, "\\x%02x", v.data[i]);
		}
		values[0] = PointerGetDatum(cstring_to_text_with_len((const char *) k.data, k.len));
		values[1] = PointerGetDatum(cstring_to_text(vs.data));
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	PROLLY_CALL(tree, cursor_close, c);
	return (Datum) 0;
}
