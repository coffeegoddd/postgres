/*--------------------------------------------------------------------------
 *
 * chunkstore_checks.c
 *		Checks of the server's chunk store, from SQL: chunks named by their
 *		content hash, as hex text, and the store's root.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_prolly/chunkstore_checks.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "access/prolly_canon.h"
#include "access/prolly_config.h"
#include "access/prolly_datum.h"
#include "access/prolly_deps.h"
#include "access/prolly_fold.h"
#include "access/prolly_chunkstore.h"
#include "access/prolly_hash.h"
#include "access/prolly_memo.h"
#include "access/prolly_remote.h"
#include "access/prolly_seen.h"
#include "access/prolly_refs.h"
#include "access/prolly_tree.h"
#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "utils/tuplestore.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
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
hash_with(const ProllyHashRoutine *r, const void *data, Size len,
		  ProllyHash *h)
{
	ProllyHashCtx *ctx = PROLLY_CALL(r, begin, CurrentMemoryContext);

	PROLLY_CALL(r, update, ctx, data, len);
	PROLLY_CALL(r, finish, ctx, h);
}

/* data's name in the store: its hash by the server's content hash */
static void
content_hash(const void *data, Size len, ProllyHash *h)
{
	hash_with(prolly_content_hash(), data, len, h);
}

/*
 * data's content hash, as hex text: by the implementation named (in pieces
 * of piece bytes, if that is more than 0), or by the server's.
 */
PG_FUNCTION_INFO_V1(test_content_hash);
Datum
test_content_hash(PG_FUNCTION_ARGS)
{
	bytea	   *data;
	const ProllyHashRoutine *r = prolly_content_hash();
	int32		piece = PG_ARGISNULL(2) ? 0 : PG_GETARG_INT32(2);
	const uint8 *p;
	Size		left;
	ProllyHashCtx *ctx;
	ProllyHash	h;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();
	data = PG_GETARG_BYTEA_PP(0);
	p = (const uint8 *) VARDATA_ANY(data);
	left = VARSIZE_ANY_EXHDR(data);
	if (!PG_ARGISNULL(1))
	{
		char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(1));

		r = (const ProllyHashRoutine *)
			prolly_lookup_impl(PROLLY_IFACE_CONTENT_HASH, name);
		if (r == NULL)
			elog(ERROR, "no content hash named \"%s\"", name);
	}
	ctx = PROLLY_CALL(r, begin, CurrentMemoryContext);
	while (left > 0)
	{
		Size		n = (piece > 0 && left > (Size) piece) ? (Size) piece : left;

		PROLLY_CALL(r, update, ctx, p, n);
		p += n;
		left -= n;
	}
	PROLLY_CALL(r, finish, ctx, &h);
	PG_RETURN_TEXT_P(hash_text(&h));
}

/*
 * BLAKE3's SIMD variants, each compiled in (as its build found the
 * compiler able to) and run by this CPU, against its portable code: the
 * dispatcher picks only the widest, so the others never run otherwise.
 * Their functions are weak references: a variant left out of the build is
 * NULL.
 */
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__) || defined(__aarch64__))
typedef void (*blake3_hash_many_fn) (const uint8_t *const *inputs,
									 size_t num_inputs, size_t blocks,
									 const uint32_t key[8], uint64_t counter,
									 bool increment_counter, uint8_t flags,
									 uint8_t flags_start, uint8_t flags_end,
									 uint8_t *out);
typedef void (*blake3_compress_fn) (uint32_t cv[8], const uint8_t block[64],
									uint8_t block_len, uint64_t counter,
									uint8_t flags);

#define BLAKE3_HASH_MANY(v) \
	extern void blake3_hash_many_##v(const uint8_t *const *, size_t, size_t, \
									 const uint32_t[8], uint64_t, bool, \
									 uint8_t, uint8_t, uint8_t, uint8_t *) \
		__attribute__((weak))
#define BLAKE3_COMPRESS(v) \
	extern void blake3_compress_in_place_##v(uint32_t[8], const uint8_t[64], \
											 uint8_t, uint64_t, uint8_t) \
		__attribute__((weak))

BLAKE3_HASH_MANY(portable);
BLAKE3_COMPRESS(portable);
#if defined(__aarch64__)
BLAKE3_HASH_MANY(neon);
#else
BLAKE3_HASH_MANY(sse2);
BLAKE3_COMPRESS(sse2);
BLAKE3_HASH_MANY(sse41);
BLAKE3_COMPRESS(sse41);
BLAKE3_HASH_MANY(avx2);
BLAKE3_HASH_MANY(avx512);
BLAKE3_COMPRESS(avx512);
#endif

#define BLAKE3_CHUNK_START 1
#define BLAKE3_CHUNK_END 2

/* a variant's results equal the portable code's, for 1 to 16 chunks */
static bool
blake3_variant_agrees(const char *name, blake3_hash_many_fn hash_many,
					  blake3_compress_fn compress)
{
	static const uint32_t iv[8] = {
		0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
		0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19
	};
	uint8_t		input[16 * 1024];
	const uint8_t *inputs[16];
	uint8_t		want[16 * 32];
	uint8_t		got[16 * 32];
	bool		ok = true;

	for (int i = 0; i < (int) sizeof(input); i++)
		input[i] = (uint8_t) (i * 7 + (i >> 8));
	for (int i = 0; i < 16; i++)
		inputs[i] = input + i * 1024;

	/* (the counter crossing 2^32 as it increments past the first chunks) */
	for (size_t n = 1; hash_many != NULL && n <= 16; n++)
	{
		blake3_hash_many_portable(inputs, n, 16, iv, UINT64CONST(0xfffffffe),
								  true, 0, BLAKE3_CHUNK_START,
								  BLAKE3_CHUNK_END, want);
		hash_many(inputs, n, 16, iv, UINT64CONST(0xfffffffe), true, 0,
				  BLAKE3_CHUNK_START, BLAKE3_CHUNK_END, got);
		if (memcmp(want, got, n * 32) != 0)
		{
			elog(WARNING, "BLAKE3 %s hash_many differs for %zu chunks",
				 name, n);
			ok = false;
		}
	}
	for (int i = 0; compress != NULL && i < 5; i++)
	{
		static const uint8_t lens[] = {0, 1, 13, 63, 64};
		uint8_t		len = lens[i];
		uint32_t	cv_want[8];
		uint32_t	cv_got[8];

		memcpy(cv_want, iv, sizeof(iv));
		memcpy(cv_got, iv, sizeof(iv));
		blake3_compress_in_place_portable(cv_want, input + len, len,
										  UINT64CONST(0x100000001),
										  BLAKE3_CHUNK_START);
		compress(cv_got, input + len, len, UINT64CONST(0x100000001),
				 BLAKE3_CHUNK_START);
		if (memcmp(cv_want, cv_got, sizeof(cv_want)) != 0)
		{
			elog(WARNING, "BLAKE3 %s compress differs for a %u-byte block",
				 name, len);
			ok = false;
		}
	}
	return ok;
}
#endif

/*
 * Whether every BLAKE3 SIMD variant built and runnable here agrees with
 * the portable code (true where there are none to check).
 */
PG_FUNCTION_INFO_V1(test_blake3_variants);
Datum
test_blake3_variants(PG_FUNCTION_ARGS)
{
	bool		ok = true;

#if defined(__GNUC__) && defined(__aarch64__)
	ok &= blake3_variant_agrees("neon", blake3_hash_many_neon, NULL);
#elif defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
	__builtin_cpu_init();
	if (__builtin_cpu_supports("sse2"))
		ok &= blake3_variant_agrees("sse2", blake3_hash_many_sse2,
									blake3_compress_in_place_sse2);
	if (__builtin_cpu_supports("sse4.1"))
		ok &= blake3_variant_agrees("sse41", blake3_hash_many_sse41,
									blake3_compress_in_place_sse41);
	if (__builtin_cpu_supports("avx2"))
		ok &= blake3_variant_agrees("avx2", blake3_hash_many_avx2, NULL);
	if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512vl"))
		ok &= blake3_variant_agrees("avx512", blake3_hash_many_avx512,
									blake3_compress_in_place_avx512);
#endif
	PG_RETURN_BOOL(ok);
}

/* Put a chunk; returns its hash. */
PG_FUNCTION_INFO_V1(test_chunk_put);
Datum
test_chunk_put(PG_FUNCTION_ARGS)
{
	bytea	   *data = PG_GETARG_BYTEA_PP(0);
	ProllyHash	h;

	content_hash(VARDATA_ANY(data), VARSIZE_ANY_EXHDR(data), &h);
	PROLLY_CALL(cs(), put, store(), &h, VARDATA_ANY(data),
				VARSIZE_ANY_EXHDR(data));
	PG_RETURN_TEXT_P(hash_text(&h));
}

/* chunks to put as the transaction commits; in TopMemoryContext */
static List *at_commit = NIL;

static void
put_at_commit(XactEvent event, void *arg)
{
	ListCell   *lc;

	if (event == XACT_EVENT_PRE_COMMIT)
	{
		foreach(lc, at_commit)
		{
			bytea	   *data = lfirst(lc);
			ProllyHash	h;

			content_hash(VARDATA(data), VARSIZE(data) - VARHDRSZ, &h);
			PROLLY_CALL(cs(), put, store(), &h, VARDATA(data),
						VARSIZE(data) - VARHDRSZ);
		}
	}
	if (event == XACT_EVENT_PRE_COMMIT || event == XACT_EVENT_ABORT)
	{
		list_free_deep(at_commit);
		at_commit = NIL;
	}
}

/*
 * Put a chunk as the transaction commits, from a transaction callback
 * registered now: one that runs after the chunk store's own if the store
 * is opened after this (callbacks run newest first).
 */
PG_FUNCTION_INFO_V1(test_chunk_put_at_commit);
Datum
test_chunk_put_at_commit(PG_FUNCTION_ARGS)
{
	static bool registered = false;
	bytea	   *data = PG_GETARG_BYTEA_P_COPY(0);
	MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);
	bytea	   *copy = palloc(VARSIZE(data));

	memcpy(copy, data, VARSIZE(data));
	at_commit = lappend(at_commit, copy);
	MemoryContextSwitchTo(old);
	if (!registered)
	{
		RegisterXactCallback(put_at_commit, NULL);
		registered = true;
	}
	(void) GetCurrentTransactionId();
	PG_RETURN_VOID();
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

/* get_many()'s answers, kept by request */
typedef struct ManyGot
{
	bytea	  **got;
	int		   *visits;
} ManyGot;

static void
many_got(int i, const char *data, Size len, void *arg)
{
	ManyGot    *mg = arg;

	mg->visits[i]++;
	if (data != NULL)
	{
		bytea	   *b = palloc(VARHDRSZ + len);

		SET_VARSIZE(b, VARHDRSZ + len);
		memcpy(VARDATA(b), data, len);
		mg->got[i] = b;
	}
}

/*
 * Get several chunks at once: each request's (ordinal, bytes, times
 * visited), bytes NULL if the store has none.
 */
PG_FUNCTION_INFO_V1(test_chunk_get_many);
Datum
test_chunk_get_many(PG_FUNCTION_ARGS)
{
	ArrayType  *arr = PG_GETARG_ARRAYTYPE_P(0);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Datum	   *elems;
	bool	   *nulls;
	int			n;
	ProllyHash *hashes;
	ManyGot		mg;

	InitMaterializedSRF(fcinfo, 0);
	deconstruct_array_builtin(arr, TEXTOID, &elems, &nulls, &n);
	hashes = palloc(sizeof(ProllyHash) * Max(n, 1));
	for (int i = 0; i < n; i++)
		text_hash(DatumGetTextPP(elems[i]), &hashes[i]);
	mg.got = palloc0(sizeof(bytea *) * Max(n, 1));
	mg.visits = palloc0(sizeof(int) * Max(n, 1));
	PROLLY_CALL(cs(), get_many, store(), hashes, n, many_got, &mg);
	for (int i = 0; i < n; i++)
	{
		Datum		values[3];
		bool		isnull[3] = {false, mg.got[i] == NULL, false};

		values[0] = Int32GetDatum(i + 1);
		values[1] = PointerGetDatum(mg.got[i]);
		values[2] = Int32GetDatum(mg.visits[i]);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, isnull);
	}
	return (Datum) 0;
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

/* test_chunk_table_file()'s source: an array's elements */
typedef struct ArraySource
{
	Datum	   *elems;
	bool	   *nulls;
	int			n;
	int			i;
} ArraySource;

static bool
array_next(void *arg, ProllyHash *hash, StringInfo data)
{
	ArraySource *as = arg;
	bytea	   *b;

	while (as->i < as->n && as->nulls[as->i])
		as->i++;
	if (as->i >= as->n)
		return false;
	b = DatumGetByteaPP(as->elems[as->i++]);
	content_hash(VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b), hash);
	appendBinaryStringInfo(data, VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b));
	return true;
}

/* Add a table file of the chunks in an array (each once). */
PG_FUNCTION_INFO_V1(test_chunk_table_file);
Datum
test_chunk_table_file(PG_FUNCTION_ARGS)
{
	ArrayType  *arr = PG_GETARG_ARRAYTYPE_P(0);
	ArraySource as = {0};

	deconstruct_array(arr, BYTEAOID, -1, false, TYPALIGN_INT, &as.elems,
					  &as.nulls, &as.n);
	PROLLY_CALL(cs(), add_table_file, store(), array_next, &as);
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(test_chunk_conjoin);
Datum
test_chunk_conjoin(PG_FUNCTION_ARGS)
{
	PROLLY_CALL(cs(), conjoin, store(), PG_ARGISNULL(0) ? 0 : PG_GETARG_INT32(0));
	PG_RETURN_VOID();
}

static void
file_row(const char *name, int nchunks, uint64 size, bool settled, void *arg)
{
	ReturnSetInfo *rsinfo = arg;
	Datum		values[4];
	bool		nulls[4] = {false, false, false, false};

	values[0] = CStringGetTextDatum(name);
	values[1] = Int32GetDatum(nchunks);
	values[2] = Int64GetDatum((int64) size);
	values[3] = BoolGetDatum(settled);
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

PG_FUNCTION_INFO_V1(test_chunk_table_files);
Datum
test_chunk_table_files(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);
	PROLLY_CALL(cs(), table_files, store(), file_row, rsinfo);
	return (Datum) 0;
}

/* The journal's segments, oldest first. */
PG_FUNCTION_INFO_V1(test_chunk_segments);
Datum
test_chunk_segments(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	PROLLY_CALL(cs(), segments, store(), true, file_row, rsinfo);
	return (Datum) 0;
}

/* Seal the journal's segment: appends go on to a new one. */
PG_FUNCTION_INFO_V1(test_chunk_seal);
Datum
test_chunk_seal(PG_FUNCTION_ARGS)
{
	PROLLY_CALL(cs(), seal, store());
	PG_RETURN_VOID();
}

/*
 * The canonical root of the session branch's versioned schemas, folded from
 * the branch's baseline (as commits fold), or from nothing with 'full': the
 * two must be the same.
 */
/* The session branch's working set, folded in full, from no baseline. */
static void
fold_in_full(ProllyNodeRef *root)
{
	PROLLY_CALL(prolly_canon_lookup("folded"), fold, NULL, root, NULL, true);
}

PG_FUNCTION_INFO_V1(test_fold_root);
Datum
test_fold_root(PG_FUNCTION_ARGS)
{
	ProllyNodeRef root;

	if (PG_NARGS() > 0 && PG_GETARG_BOOL(0))
		fold_in_full(&root);
	else
	{
		ProllyDatabaseComponents dc;

		prolly_database_components(&dc);
		(void) PROLLY_CALL(dc.refs, working_set, NULL, &root);
	}
	PG_RETURN_TEXT_P(hash_text(&root.hash));
}

typedef struct Reaches
{
	ProllyHash	target;
	bool		found;
} Reaches;

static bool
reaches_node(const ProllyNodeRef *node, uint16 level, void *arg)
{
	Reaches    *r = arg;

	if (node->hash.len == r->target.len &&
		memcmp(node->hash.bytes, r->target.bytes, node->hash.len) == 0)
		r->found = true;
	return !r->found;
}

/*
 * Whether the refs' walk (what reclamation keeps) reaches the commit whose
 * id is given, from the chunk store's root.
 */
PG_FUNCTION_INFO_V1(test_refs_walk_reaches);
Datum
test_refs_walk_reaches(PG_FUNCTION_ARGS)
{
	ProllyDatabaseComponents dc;
	ProllyNodeRef refs;
	Reaches		r;

	memset(&r, 0, sizeof(r));
	text_hash(PG_GETARG_TEXT_PP(0), &r.target);
	memset(&refs, 0, sizeof(refs));
	PROLLY_CALL(cs(), root, store(), &refs.hash);
	prolly_database_components(&dc);
	PROLLY_CALL(dc.refs, walk, &refs, reaches_node, &r);
	PG_RETURN_BOOL(r.found);
}

typedef struct Declines
{
	ProllyHash	declined;
	int64		visited;
} Declines;

static bool
count_node(const ProllyNodeRef *node, uint16 level, void *arg)
{
	Declines   *d = arg;

	d->visited++;
	return !(node->hash.len == d->declined.len &&
			 memcmp(node->hash.bytes, d->declined.bytes, node->hash.len) == 0);
}

/*
 * How many nodes the refs' walk visits from the chunk store's root, the
 * node whose hash is given (none if NULL) declined: what lies below it,
 * through a commit's root and parents too, isn't visited.
 */
PG_FUNCTION_INFO_V1(test_refs_walk_count);
Datum
test_refs_walk_count(PG_FUNCTION_ARGS)
{
	ProllyDatabaseComponents dc;
	ProllyNodeRef refs;
	Declines	d;

	memset(&d, 0, sizeof(d));
	if (!PG_ARGISNULL(0))
		text_hash(PG_GETARG_TEXT_PP(0), &d.declined);
	memset(&refs, 0, sizeof(refs));
	PROLLY_CALL(cs(), root, store(), &refs.hash);
	prolly_database_components(&dc);
	PROLLY_CALL(dc.refs, walk, &refs, count_node, &d);
	PG_RETURN_INT64(d.visited);
}

static void
tree_row(const char *name, const ProllyNodeRef *data,
		 const ProllyValue *description, const char *unreadable, void *arg)
{
	ReturnSetInfo *rsinfo = arg;
	Datum		values[3];
	bool		nulls[3] = {0};

	/* (canonical names are UTF-8) */
	values[0] = CStringGetTextDatum(pg_any_to_server(name, strlen(name), PG_UTF8));
	values[1] = PointerGetDatum(hash_text(&data->hash));
	if (unreadable)
		values[2] = CStringGetTextDatum(unreadable);
	else
		nulls[2] = true;
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

/*
 * test_fold_trees(): each table's data tree in the session branch's working
 * set, folded once: its qualified name, the tree's hash, and why its tree
 * can't be read as ordered now (a key's collation changed), if it can't.
 * (The tree depends on the rows' keys and encodings, the node format and
 * the hash, not on the table's definition.)
 */
PG_FUNCTION_INFO_V1(test_fold_trees);
Datum
test_fold_trees(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	ProllyNodeRef root;

	InitMaterializedSRF(fcinfo, 0);
	fold_in_full(&root);
	PROLLY_CALL(prolly_canon_lookup("folded"), tables, &root, tree_row, rsinfo);
	return (Datum) 0;
}

/* The session's last full fold: rows read, whether it spilled, its sort's kB. */
PG_FUNCTION_INFO_V1(test_last_full_fold);
Datum
test_last_full_fold(PG_FUNCTION_ARGS)
{
	TupleDesc	desc;
	Datum		values[3];
	bool		nulls[3] = {0};

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	values[0] = Int64GetDatum(prolly_last_full_fold.rows_read);
	values[1] = BoolGetDatum(prolly_last_full_fold.spilled);
	values[2] = Int64GetDatum(prolly_last_full_fold.sort_kb);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc),
													 values, nulls)));
}

/* What the session's folds have read: pages, candidates, full tables. */
PG_FUNCTION_INFO_V1(test_fold_counters);
Datum
test_fold_counters(PG_FUNCTION_ARGS)
{
	TupleDesc	desc;
	ProllyChangeCounters c;
	Datum		values[3];
	bool		nulls[3] = {0};

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	PROLLY_CALL(prolly_change_finder(), counters, &c, PG_GETARG_BOOL(0));
	values[0] = Int64GetDatum((int64) c.pages);
	values[1] = Int64GetDatum((int64) c.candidates);
	values[2] = Int64GetDatum((int64) c.full);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc),
													  values, nulls)));
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
	PROLLY_CALL(canon, fold, NULL, &root, NULL, true);
	memset(&env, 0, sizeof(env));
	env.chunk_format = prolly_chunkformat_lookup("basic");
	env.chunker = prolly_chunker_lookup("bytes");
	env.hash = prolly_content_hash();
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

/* The session's refs resolve ref, or error. */
static void
resolve_ref(const ProllyDatabaseComponents *dc, text *ref, ProllyHash *id)
{
	char	   *name = text_to_cstring(ref);

	if (!PROLLY_CALL(dc->refs, resolve, name, id))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("prolly ref \"%s\" does not exist", name)));
}

/*
 * test_commit_with(parent): commit the working set's root, as a merge's
 * commit is made (the refs' commit_with), with parent as its one parent.
 */
PG_FUNCTION_INFO_V1(test_commit_with);
Datum
test_commit_with(PG_FUNCTION_ARGS)
{
	ProllyDatabaseComponents dc;
	ProllyHash	parent;
	ProllyNodeRef root;
	ProllyHash	id;

	prolly_database_components(&dc);
	resolve_ref(&dc, PG_GETARG_TEXT_PP(0), &parent);
	(void) PROLLY_CALL(dc.refs, working_set, NULL, &root);
	PROLLY_CALL(dc.refs, commit_with, "committed with", &parent, 1, &root, &id);
	PG_RETURN_TEXT_P(hash_text(&id));
}

/* test_refs_set_head(ref): make ref's commit the session branch's head */
PG_FUNCTION_INFO_V1(test_refs_set_head);
Datum
test_refs_set_head(PG_FUNCTION_ARGS)
{
	ProllyDatabaseComponents dc;
	ProllyHash	id;

	prolly_database_components(&dc);
	resolve_ref(&dc, PG_GETARG_TEXT_PP(0), &id);
	PROLLY_CALL(dc.refs, set_head, &id);
	PG_RETURN_VOID();
}

/*
 * test_refs_value(key): the value of the refs' entry key (a tag, then a
 * name), from the chunk store's root; NULL if there is none.
 */
PG_FUNCTION_INFO_V1(test_refs_value);
Datum
test_refs_value(PG_FUNCTION_ARGS)
{
	const ProllyTreeRoutine *tree = prolly_tree_lookup("cow");
	ProllyTreeEnv env;
	ProllyNodeRef refs;
	text	   *key = PG_GETARG_TEXT_PP(0);
	ProllyKey	k = {(const uint8 *) VARDATA_ANY(key), VARSIZE_ANY_EXHDR(key)};
	StringInfoData v;
	bytea	   *out;

	prolly_folded_tree_env(&env);
	memset(&refs, 0, sizeof(refs));
	PROLLY_CALL(cs(), root, store(), &refs.hash);
	initStringInfo(&v);
	if (refs.hash.len == 0 || !PROLLY_CALL(tree, get, &env, &refs, &k, &v))
		PG_RETURN_NULL();
	out = palloc(VARHDRSZ + v.len);
	SET_VARSIZE(out, VARHDRSZ + v.len);
	memcpy(VARDATA(out), v.data, v.len);
	PG_RETURN_BYTEA_P(out);
}

/* len bytes at data, as a bytea */
static bytea *
bytes_bytea(const char *data, Size len)
{
	bytea	   *b = palloc(VARHDRSZ + len);

	SET_VARSIZE(b, VARHDRSZ + len);
	memcpy(VARDATA(b), data, len);
	return b;
}

typedef struct OneChunk
{
	ProllyHash	hash;
	bytea	   *data;
	bool		given;
	bool		stored;			/* data is framed already */
} OneChunk;

static bool
one_chunk(void *arg, ProllyHash *hash, StringInfo data)
{
	OneChunk   *c = arg;

	if (c->given)
		return false;
	c->given = true;
	*hash = c->hash;
	if (c->stored)
		appendBinaryStringInfo(data, VARDATA_ANY(c->data), VARSIZE_ANY_EXHDR(c->data));
	else
		prolly_chunk_frame(VARDATA_ANY(c->data), VARSIZE_ANY_EXHDR(c->data), data);
	return true;
}

/*
 * test_remote_put(url, hash, data): add a chunk to a remote under the hash
 * given, whatever its bytes hash to, as a remote that lies would.
 */
PG_FUNCTION_INFO_V1(test_remote_put);
Datum
test_remote_put(PG_FUNCTION_ARGS)
{
	char	   *url = text_to_cstring(PG_GETARG_TEXT_PP(0));
	const ProllyRemoteRoutine *rr = prolly_remote_for_url(url);
	ProllyRemote *r = PROLLY_CALL(rr, open, url, true);
	OneChunk	c;

	memset(&c, 0, sizeof(c));
	text_hash(PG_GETARG_TEXT_PP(1), &c.hash);
	c.data = PG_GETARG_BYTEA_PP(2);
	PROLLY_CALL(rr, add_chunks, r, one_chunk, &c);
	PROLLY_CALL(rr, close, r);
	PG_RETURN_VOID();
}

/*
 * test_remote_put_stored(url, hash, frame): the same, the chunk given as
 * stored (framed), as it is.
 */
PG_FUNCTION_INFO_V1(test_remote_put_stored);
Datum
test_remote_put_stored(PG_FUNCTION_ARGS)
{
	char	   *url = text_to_cstring(PG_GETARG_TEXT_PP(0));
	const ProllyRemoteRoutine *rr = prolly_remote_for_url(url);
	ProllyRemote *r = PROLLY_CALL(rr, open, url, true);
	OneChunk	c;

	memset(&c, 0, sizeof(c));
	text_hash(PG_GETARG_TEXT_PP(1), &c.hash);
	c.data = PG_GETARG_BYTEA_PP(2);
	c.stored = true;
	PROLLY_CALL(rr, add_chunks, r, one_chunk, &c);
	PROLLY_CALL(rr, close, r);
	PG_RETURN_VOID();
}

/* test_remote_get_stored(url, hash): a remote's chunk as it gives it. */
PG_FUNCTION_INFO_V1(test_remote_get_stored);
Datum
test_remote_get_stored(PG_FUNCTION_ARGS)
{
	char	   *url = text_to_cstring(PG_GETARG_TEXT_PP(0));
	const ProllyRemoteRoutine *rr = prolly_remote_for_url(url);
	ProllyRemote *r = PROLLY_CALL(rr, open, url, false);
	ProllyHash	h;
	StringInfoData out;
	bool		found;

	text_hash(PG_GETARG_TEXT_PP(1), &h);
	initStringInfo(&out);
	found = PROLLY_CALL(rr, get, r, &h, &out);
	PROLLY_CALL(rr, close, r);
	if (!found)
		PG_RETURN_NULL();
	PG_RETURN_BYTEA_P(bytes_bytea(out.data, out.len));
}

/*
 * test_seen_check(n, budget): n hashes entered in a walk's map of budget
 * bytes, each with a value; each found with it, then changed and found
 * changed, and as many others not found.  The runs it wrote (an error if
 * any check fails).
 */
PG_FUNCTION_INFO_V1(test_seen_check);
Datum
test_seen_check(PG_FUNCTION_ARGS)
{
	int			n = PG_GETARG_INT32(0);
	ProllySeen *seen = prolly_seen_create("test seen", sizeof(int64),
										  (Size) PG_GETARG_INT32(1));
	int			runs;

	for (int pass = 0; pass < 4; pass++)
	{
		for (int i = 0; i < n; i++)
		{
			char		name[32];
			ProllyHash	h;
			int64		v = (int64) i * 10 + (pass >= 2 ? 1 : 0);
			int64		got = -1;

			snprintf(name, sizeof(name), "%s %d", pass == 3 ? "absent" : "seen", i);
			content_hash(name, strlen(name), &h);
			if (pass == 0)
			{
				if (!prolly_seen_enter(seen, &h, &v))
					elog(ERROR, "hash %d entered twice", i);
				continue;
			}
			if (pass == 3)
			{
				if (prolly_seen_find(seen, &h, NULL))
					elog(ERROR, "hash %d found, never entered", i);
				continue;
			}
			if (!prolly_seen_find(seen, &h, &got) || got != v)
				elog(ERROR, "hash %d found with %lld, not %lld", i,
					 (long long) got, (long long) v);
			if (prolly_seen_enter(seen, &h, &v))
				elog(ERROR, "hash %d entered again", i);
			if (pass == 1)
			{
				v++;
				prolly_seen_set(seen, &h, &v);
			}
		}
	}
	if (prolly_seen_count(seen) != (uint64) n)
		elog(ERROR, "the map counts %llu, not %d",
			 (unsigned long long) prolly_seen_count(seen), n);
	runs = prolly_seen_runs(seen);
	prolly_seen_destroy(seen);
	PG_RETURN_INT32(runs);
}

/* test_chunk_stored(hash): a chunk of the store as stored (framed). */
PG_FUNCTION_INFO_V1(test_chunk_stored);
Datum
test_chunk_stored(PG_FUNCTION_ARGS)
{
	ProllyHash	h;
	StringInfoData out;

	text_hash(PG_GETARG_TEXT_PP(0), &h);
	initStringInfo(&out);
	if (!prolly_chunk_get_stored(cs(), store(), &h, &out))
		PG_RETURN_NULL();
	PG_RETURN_BYTEA_P(bytes_bytea(out.data, out.len));
}

/*
 * test_chunk_frame(data, compress): a chunk framed as stored, compressed
 * if that saves (or not at all).
 */
PG_FUNCTION_INFO_V1(test_chunk_frame);
Datum
test_chunk_frame(PG_FUNCTION_ARGS)
{
	bytea	   *data = PG_GETARG_BYTEA_PP(0);
	uint32		len = VARSIZE_ANY_EXHDR(data);
	StringInfoData out;

	initStringInfo(&out);
	if (PG_GETARG_BOOL(1))
		prolly_chunk_frame(VARDATA_ANY(data), len, &out);
	else
	{
		char		head[PROLLY_FRAME_HEADER] = {PROLLY_CODEC_NONE,
			len & 0xff, (len >> 8) & 0xff, (len >> 16) & 0xff, (len >> 24) & 0xff};

		appendBinaryStringInfo(&out, head, PROLLY_FRAME_HEADER);
		appendBinaryStringInfo(&out, VARDATA_ANY(data), len);
	}
	PG_RETURN_BYTEA_P(bytes_bytea(out.data, out.len));
}

typedef struct Collected
{
	HTAB	   *seen;
	Tuplestorestate *out;
	TupleDesc	desc;
} Collected;

static bool
collect_node(const ProllyNodeRef *node, uint16 level, void *arg)
{
	Collected  *c = arg;
	ProllyHash	key = prolly_hash_key(&node->hash);
	bool		found;
	Datum		value;
	bool		null = false;

	(void) hash_search(c->seen, &key, HASH_ENTER, &found);
	if (found)
		return false;
	value = PointerGetDatum(hash_text(&node->hash));
	tuplestore_putvalues(c->out, c->desc, &value, &null);
	return true;
}

/* test_commit_chunks(commit): every chunk the commit's walk reaches, once. */
PG_FUNCTION_INFO_V1(test_commit_chunks);
Datum
test_commit_chunks(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	ProllyDatabaseComponents dc;
	ProllyHash	id;
	Collected	c;
	HASHCTL		ctl;

	text_hash(PG_GETARG_TEXT_PP(0), &id);
	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	ctl.keysize = sizeof(ProllyHash);
	ctl.entrysize = sizeof(ProllyHash);
	ctl.hcxt = CurrentMemoryContext;
	c.seen = hash_create("test commit chunks", 64, &ctl,
						 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	c.out = rsinfo->setResult;
	c.desc = rsinfo->setDesc;
	prolly_database_components(&dc);
	PROLLY_CALL(dc.refs, walk_commit, &id, collect_node, &c);
	return (Datum) 0;
}

/*
 * test_remote_set_head(url, branch, commit, format): a remote's branch set
 * to commit (whatever it is), its format recorded as given if it has none.
 */
PG_FUNCTION_INFO_V1(test_remote_set_head);
Datum
test_remote_set_head(PG_FUNCTION_ARGS)
{
	char	   *url = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *branch = text_to_cstring(PG_GETARG_TEXT_PP(1));
	const ProllyRemoteRoutine *rr = prolly_remote_for_url(url);
	ProllyRemote *r = PROLLY_CALL(rr, open, url, true);
	ProllyHash	head;
	ProllyHash	none = {0};
	bool		moved;

	text_hash(PG_GETARG_TEXT_PP(2), &head);
	moved = PROLLY_CALL(rr, move_head, r, branch, &none, &head,
						text_to_cstring(PG_GETARG_TEXT_PP(3)), false);
	PROLLY_CALL(rr, close, r);
	PG_RETURN_BOOL(moved);
}

/*
 * test_root_without_format(root): a folded root as the format before
 * "!:format" made it, the item removed: what an older build committed.
 */
PG_FUNCTION_INFO_V1(test_root_without_format);
Datum
test_root_without_format(PG_FUNCTION_ARGS)
{
	const ProllyTreeRoutine *tree = prolly_tree_lookup("cow");
	ProllyTreeEnv env;
	ProllyNodeRef root;
	ProllyNodeRef out;
	ProllyEdit	e;

	prolly_folded_tree_env(&env);
	memset(&root, 0, sizeof(root));
	text_hash(PG_GETARG_TEXT_PP(0), &root.hash);
	memset(&e, 0, sizeof(e));
	e.key.data = (const uint8 *) "!:format";
	e.key.len = 8;
	e.is_delete = true;
	PROLLY_CALL(tree, apply, &env, &root, &e, 1, &out);
	PG_RETURN_TEXT_P(hash_text(&out.hash));
}

/*
 * test_root_with_format(root, format): a folded root with its "!:format"
 * item set to format: what a build of that format committed.
 */
PG_FUNCTION_INFO_V1(test_root_with_format);
Datum
test_root_with_format(PG_FUNCTION_ARGS)
{
	const ProllyTreeRoutine *tree = prolly_tree_lookup("cow");
	char	   *format = text_to_cstring(PG_GETARG_TEXT_PP(1));
	ProllyTreeEnv env;
	ProllyNodeRef root;
	ProllyNodeRef out;
	ProllyEdit	e;

	prolly_folded_tree_env(&env);
	memset(&root, 0, sizeof(root));
	text_hash(PG_GETARG_TEXT_PP(0), &root.hash);
	memset(&e, 0, sizeof(e));
	e.key.data = (const uint8 *) "!:format";
	e.key.len = 8;
	e.value.data = (const uint8 *) format;
	e.value.len = strlen(format);
	PROLLY_CALL(tree, apply, &env, &root, &e, 1, &out);
	PG_RETURN_TEXT_P(hash_text(&out.hash));
}

/*
 * test_root_without_sequence_values(root): a folded root whose sequences
 * have no values recorded, as before the fold recorded them (and as a fold
 * that couldn't read one leaves it).
 */
PG_FUNCTION_INFO_V1(test_root_without_sequence_values);
Datum
test_root_without_sequence_values(PG_FUNCTION_ARGS)
{
	const ProllyTreeRoutine *tree = prolly_tree_lookup("cow");
	ProllyTreeEnv env;
	ProllyNodeRef root;
	ProllyNodeRef out;
	ProllyTreeCursor *c;
	ProllyKey	from = {(const uint8 *) "s:", 2};
	ProllyKey	k;
	ProllyValue v;
	ProllyEdit *edits = NULL;
	int			nedits = 0;

	prolly_folded_tree_env(&env);
	memset(&root, 0, sizeof(root));
	text_hash(PG_GETARG_TEXT_PP(0), &root.hash);
	c = PROLLY_CALL(tree, cursor_open, &env, &root, CurrentMemoryContext);
	PROLLY_CALL(tree, cursor_seek, c, &from, ForwardScanDirection);
	while (PROLLY_CALL(tree, cursor_next, c, &k, &v) &&
		   k.len >= 2 && memcmp(k.data, "s:", 2) == 0)
	{
		const uint8 *nl = memchr(v.data, '\n', v.len);
		ProllyEdit *e;

		if (nl == NULL || nl + 1 == v.data + v.len)
			continue;
		edits = edits ? repalloc(edits, sizeof(ProllyEdit) * (nedits + 1)) :
			palloc(sizeof(ProllyEdit));
		e = &edits[nedits++];
		memset(e, 0, sizeof(*e));
		e->key.data = (const uint8 *) pnstrdup((const char *) k.data, k.len);
		e->key.len = k.len;
		e->value.data = (const uint8 *) pnstrdup((const char *) v.data, nl + 1 - v.data);
		e->value.len = nl + 1 - v.data;
	}
	PROLLY_CALL(tree, cursor_close, c);
	if (nedits == 0)
		out = root;
	else
		PROLLY_CALL(tree, apply, &env, &root, edits, nedits, &out);
	PG_RETURN_TEXT_P(hash_text(&out.hash));
}

static void
note_dependency_text(const char *name, const char *value, void *arg)
{
	ArrayBuildState **astate = arg;

	*astate = accumArrayResult(*astate,
							   CStringGetTextDatum(psprintf("%s => %s", name, value)),
							   false, TEXTOID, CurrentMemoryContext);
}

/*
 * test_commit_dependencies(commit): what a commit records its versioned
 * objects needed from outside, each "name => value" (tabs as \t), in key
 * order; NULL if it doesn't record them.
 */
PG_FUNCTION_INFO_V1(test_commit_dependencies);
Datum
test_commit_dependencies(PG_FUNCTION_ARGS)
{
	ProllyDatabaseComponents dc;
	ProllyHash	id;
	ProllyCommitInfo info;
	ArrayBuildState *astate = NULL;

	prolly_database_components(&dc);
	text_hash(PG_GETARG_TEXT_PP(0), &id);
	if (!PROLLY_CALL(dc.refs, commit_get, &id, &info))
		elog(ERROR, "no such commit");
	if (!info.has_dependencies)
		PG_RETURN_NULL();
	PROLLY_CALL(dc.canonicalizer, dependencies, &info.dependencies,
				note_dependency_text, &astate);
	if (astate == NULL)
		PG_RETURN_ARRAYTYPE_P(construct_empty_array(TEXTOID));
	PG_RETURN_DATUM(makeArrayResult(astate, CurrentMemoryContext));
}

static void
commit_item(const ProllyTreeRoutine *tree, ProllyTreeBuilder *b,
			const char *key, const char *value, int len)
{
	ProllyKey	k = {(const uint8 *) key, strlen(key)};
	ProllyValue v = {(const uint8 *) value, len};

	PROLLY_CALL(tree, build_add, b, &k, &v);
}

/*
 * A commit naming root, with parent (none if its len is 0), as the refs
 * write one; recording deps if given, or recording none (as one made
 * before commits recorded them).
 */
static void
make_commit_needing(const ProllyHash *root, const ProllyHash *parent,
					const char *message, const ProllyHash *deps, ProllyHash *id)
{
	const ProllyTreeRoutine *tree = prolly_tree_lookup("cow");
	ProllyTreeEnv env;
	ProllyTreeBuilder *b;
	ProllyNodeRef ref;
	char		hashed[1 + PROLLY_HASH_MAX_LEN];
	char		parents[1 + PROLLY_HASH_MAX_LEN];
	char		time[8] = {0};

	prolly_folded_tree_env(&env);
	hashed[0] = (char) root->len;
	memcpy(hashed + 1, root->bytes, root->len);
	parents[0] = (char) parent->len;
	memcpy(parents + 1, parent->bytes, parent->len);
	b = PROLLY_CALL(tree, build_begin, &env, CurrentMemoryContext);
	commit_item(tree, b, "author", "test", 4);
	if (deps)
	{
		char		hashed_deps[1 + PROLLY_HASH_MAX_LEN];

		hashed_deps[0] = (char) deps->len;
		memcpy(hashed_deps + 1, deps->bytes, deps->len);
		commit_item(tree, b, "dependencies", hashed_deps, 1 + deps->len);
	}
	commit_item(tree, b, "message", message, strlen(message));
	commit_item(tree, b, "parents", parents, parent->len ? 1 + parent->len : 0);
	commit_item(tree, b, "root", hashed, 1 + root->len);
	commit_item(tree, b, "time", time, 8);
	PROLLY_CALL(tree, build_finish, b, &ref);
	*id = ref.hash;
}

static void
make_commit(const ProllyHash *root, const ProllyHash *parent,
			const char *message, ProllyHash *id)
{
	make_commit_needing(root, parent, message, NULL, id);
}

/* A dependency for test_commit_needing(), by its name and value. */
typedef struct TestDependency
{
	char	   *name;
	char	   *value;
} TestDependency;

static int
test_dependency_cmp(const ListCell *a, const ListCell *b)
{
	const char *x = ((TestDependency *) lfirst(a))->name;
	const char *y = ((TestDependency *) lfirst(b))->name;
	int			c = memcmp(x, y, Min(strlen(x), strlen(y)));

	return c != 0 ? c : (int) strlen(x) - (int) strlen(y);
}

/*
 * test_commit_needing(root, names, values): a commit with no parents
 * naming root and recording the dependencies given, each a name and a
 * value as the canonicalizer's dependencies() visits them; its id.  For
 * what a commit made elsewhere may record.
 */
PG_FUNCTION_INFO_V1(test_commit_needing);
Datum
test_commit_needing(PG_FUNCTION_ARGS)
{
	const ProllyTreeRoutine *tree = prolly_tree_lookup("cow");
	ProllyTreeEnv env;
	ProllyTreeBuilder *b;
	ProllyNodeRef deps;
	ProllyHash	root;
	ProllyHash	none = {0};
	ProllyHash	id;
	Datum	   *names;
	Datum	   *values;
	int			nnames;
	int			nvalues;
	List	   *items = NIL;
	ListCell   *lc;

	text_hash(PG_GETARG_TEXT_PP(0), &root);
	deconstruct_array_builtin(PG_GETARG_ARRAYTYPE_P(1), TEXTOID, &names, NULL, &nnames);
	deconstruct_array_builtin(PG_GETARG_ARRAYTYPE_P(2), TEXTOID, &values, NULL, &nvalues);
	if (nnames != nvalues)
		elog(ERROR, "as many names as values, please");
	for (int i = 0; i < nnames; i++)
	{
		TestDependency *d = palloc(sizeof(TestDependency));

		d->name = TextDatumGetCString(names[i]);
		d->value = TextDatumGetCString(values[i]);
		items = lappend(items, d);
	}
	list_sort(items, test_dependency_cmp);

	prolly_folded_tree_env(&env);
	b = PROLLY_CALL(tree, build_begin, &env, CurrentMemoryContext);
	foreach(lc, items)
	{
		TestDependency *d = lfirst(lc);
		ProllyKey	k = {(const uint8 *) d->name, strlen(d->name)};
		ProllyValue v = {(const uint8 *) d->value, strlen(d->value)};

		PROLLY_CALL(tree, build_add, b, &k, &v);
	}
	PROLLY_CALL(tree, build_finish, b, &deps);
	make_commit_needing(&root, &none, "needing", &deps.hash, &id);
	PG_RETURN_TEXT_P(hash_text(&id));
}

/*
 * test_check_dependencies(commits, install): check what the commits record
 * they need against the database, as a clone (install) or a move does.
 */
PG_FUNCTION_INFO_V1(test_check_dependencies);
Datum
test_check_dependencies(PG_FUNCTION_ARGS)
{
	ProllyDatabaseComponents dc;
	Datum	   *commits;
	int			ncommits;
	List	   *trees = NIL;

	prolly_database_components(&dc);
	deconstruct_array_builtin(PG_GETARG_ARRAYTYPE_P(0), TEXTOID, &commits, NULL, &ncommits);
	for (int i = 0; i < ncommits; i++)
	{
		ProllyHash	id;
		ProllyCommitInfo info;

		text_hash(DatumGetTextPP(commits[i]), &id);
		if (!PROLLY_CALL(dc.refs, commit_get, &id, &info))
			elog(ERROR, "no such commit");
		if (info.has_dependencies)
		{
			ProllyNodeRef *tree = palloc(sizeof(ProllyNodeRef));

			*tree = info.dependencies;
			trees = lappend(trees, tree);
		}
	}
	prolly_check_dependencies(trees, PG_GETARG_BOOL(1), "check");
	PG_RETURN_VOID();
}

/*
 * test_commit_of(root, message): a commit with no parents naming root, as
 * the refs write one; its id.
 */
PG_FUNCTION_INFO_V1(test_commit_of);
Datum
test_commit_of(PG_FUNCTION_ARGS)
{
	ProllyHash	root;
	ProllyHash	none = {0};
	ProllyHash	id;

	text_hash(PG_GETARG_TEXT_PP(0), &root);
	make_commit(&root, &none, text_to_cstring(PG_GETARG_TEXT_PP(1)), &id);
	PG_RETURN_TEXT_P(hash_text(&id));
}

/*
 * test_commit_chain(parent, n): n commits in a line after parent, each
 * naming parent's root; the last one's id.
 */
PG_FUNCTION_INFO_V1(test_commit_chain);
Datum
test_commit_chain(PG_FUNCTION_ARGS)
{
	ProllyDatabaseComponents dc;
	ProllyCommitInfo info;
	ProllyHash	head;
	int32		n = PG_GETARG_INT32(1);
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext,
											  "test commit chain",
											  ALLOCSET_DEFAULT_SIZES);

	prolly_database_components(&dc);
	text_hash(PG_GETARG_TEXT_PP(0), &head);
	if (!PROLLY_CALL(dc.refs, commit_get, &head, &info))
		elog(ERROR, "no such commit");
	for (int32 i = 0; i < n; i++)
	{
		MemoryContext old = MemoryContextSwitchTo(cxt);
		ProllyHash	next;

		make_commit(&info.root.hash, &head, "chained", &next);
		MemoryContextSwitchTo(old);
		head = next;
		MemoryContextReset(cxt);
		CHECK_FOR_INTERRUPTS();
	}
	PG_RETURN_TEXT_P(hash_text(&head));
}

/* test_refs_set_upstream(branch, remote, remote_branch) */
PG_FUNCTION_INFO_V1(test_refs_set_upstream);
Datum
test_refs_set_upstream(PG_FUNCTION_ARGS)
{
	ProllyDatabaseComponents dc;

	prolly_database_components(&dc);
	PROLLY_CALL(dc.refs, upstream_set, text_to_cstring(PG_GETARG_TEXT_PP(0)),
				text_to_cstring(PG_GETARG_TEXT_PP(1)),
				text_to_cstring(PG_GETARG_TEXT_PP(2)));
	PG_RETURN_VOID();
}

/*
 * test_walk_commit_count(id): how many nodes the refs' walk from one
 * commit visits (its own, its root's, its ancestors').
 */
PG_FUNCTION_INFO_V1(test_walk_commit_count);
Datum
test_walk_commit_count(PG_FUNCTION_ARGS)
{
	ProllyDatabaseComponents dc;
	ProllyHash	id;
	Declines	d;

	memset(&d, 0, sizeof(d));
	text_hash(PG_GETARG_TEXT_PP(0), &id);
	prolly_database_components(&dc);
	PROLLY_CALL(dc.refs, walk_commit, &id, count_node, &d);
	PG_RETURN_INT64(d.visited);
}

/*
 * Put a tree node naming chunks: at level 0 a leaf whose values are stored
 * out of line in them, above it an internal node of them as children.
 */
PG_FUNCTION_INFO_V1(test_chunk_put_node);
Datum
test_chunk_put_node(PG_FUNCTION_ARGS)
{
	int			level = PG_GETARG_INT32(0);
	ArrayType  *refs = PG_GETARG_ARRAYTYPE_P(1);
	const ProllyChunkFormatRoutine *fmt = prolly_chunkformat_lookup("basic");
	Datum	   *elems;
	int			n;
	ProllyKey  *keys;
	ProllyValue *values;
	uint8	   *flags;
	StringInfoData node;
	ProllyHash	h;

	deconstruct_array_builtin(refs, TEXTOID, &elems, NULL, &n);
	keys = palloc(sizeof(ProllyKey) * Max(n, 1));
	values = palloc(sizeof(ProllyValue) * Max(n, 1));
	flags = palloc(sizeof(uint8) * Max(n, 1));
	for (int i = 0; i < n; i++)
	{
		StringInfoData v;
		ProllyHash	ref;

		text_hash(DatumGetTextPP(elems[i]), &ref);
		initStringInfo(&v);
		appendStringInfoChar(&v, (char) ref.len);
		appendBinaryStringInfo(&v, ref.bytes, ref.len);
		prolly_put_varint(&v, 1);
		keys[i].data = (const uint8 *) psprintf("k%06d", i);
		keys[i].len = strlen((const char *) keys[i].data);
		values[i].data = (const uint8 *) v.data;
		values[i].len = v.len;
		flags[i] = level == 0 ? PROLLY_ITEM_VALUE_REF : 0;
	}
	initStringInfo(&node);
	PROLLY_CALL(fmt, encode, (uint16) level, n, keys, values, flags, &node);
	content_hash(node.data, node.len, &h);
	PROLLY_CALL(cs(), put, store(), &h, node.data, node.len);
	PG_RETURN_TEXT_P(hash_text(&h));
}

/* test_chunk_declaring_tree()'s declared references */
typedef struct DeclaredRefs
{
	ProllyHash *hashes;
	int			n;
} DeclaredRefs;

static void
declare_all(const ProllyKey *key, const ProllyValue *value, void *arg,
			StringInfo refs)
{
	DeclaredRefs *d = arg;

	for (int i = 0; i < d->n; i++)
		PROLLY_CALL(prolly_tree_lookup("cow"), value_reference, &d->hashes[i],
					0, refs);
}

/*
 * A tree of one item ('k' -> 'v') whose item declares every chunk of
 * hashes, as a commit declares its parents; its root's hash.
 */
PG_FUNCTION_INFO_V1(test_chunk_declaring_tree);
Datum
test_chunk_declaring_tree(PG_FUNCTION_ARGS)
{
	ArrayType  *arr = PG_GETARG_ARRAYTYPE_P(0);
	const ProllyTreeRoutine *tree = prolly_tree_lookup("cow");
	ProllyTreeEnv env;
	ProllyTreeBuilder *b;
	ProllyNodeRef ref;
	DeclaredRefs d;
	Datum	   *elems;
	ProllyKey	k = {(const uint8 *) "k", 1};
	ProllyValue v = {(const uint8 *) "v", 1};

	deconstruct_array(arr, TEXTOID, -1, false, TYPALIGN_INT, &elems, NULL, &d.n);
	d.hashes = palloc(sizeof(ProllyHash) * Max(d.n, 1));
	for (int i = 0; i < d.n; i++)
		text_hash(DatumGetTextPP(elems[i]), &d.hashes[i]);
	prolly_folded_tree_env(&env);
	env.chunk_refs = declare_all;
	env.chunk_refs_arg = &d;
	b = PROLLY_CALL(tree, build_begin, &env, CurrentMemoryContext);
	PROLLY_CALL(tree, build_add, b, &k, &v);
	PROLLY_CALL(tree, build_finish, b, &ref);
	{
		StringInfoData got;

		initStringInfo(&got);
		if (!PROLLY_CALL(tree, get, &env, &ref, &k, &got) ||
			got.len != 1 || got.data[0] != 'v')
			elog(ERROR, "the declaring tree's item doesn't read back as written");
		pfree(got.data);
	}
	PG_RETURN_TEXT_P(hash_text(&ref.hash));
}

PG_FUNCTION_INFO_V1(test_chunk_put_leaf);
Datum
test_chunk_put_leaf(PG_FUNCTION_ARGS)
{
	ArrayType  *vals = PG_GETARG_ARRAYTYPE_P(0);
	const ProllyChunkFormatRoutine *fmt = prolly_chunkformat_lookup("basic");
	Datum	   *elems;
	int			n;
	ProllyKey  *keys;
	ProllyValue *values;
	uint8	   *flags;
	StringInfoData node;
	ProllyHash	h;

	deconstruct_array(vals, BYTEAOID, -1, false, TYPALIGN_INT, &elems, NULL, &n);
	keys = palloc(sizeof(ProllyKey) * Max(n, 1));
	values = palloc(sizeof(ProllyValue) * Max(n, 1));
	flags = palloc0(sizeof(uint8) * Max(n, 1));
	for (int i = 0; i < n; i++)
	{
		bytea	   *v = DatumGetByteaPP(elems[i]);

		keys[i].data = (const uint8 *) psprintf("k%06d", i);
		keys[i].len = strlen((const char *) keys[i].data);
		values[i].data = (const uint8 *) VARDATA_ANY(v);
		values[i].len = VARSIZE_ANY_EXHDR(v);
	}
	initStringInfo(&node);
	PROLLY_CALL(fmt, encode, 0, n, keys, values, flags, &node);
	content_hash(node.data, node.len, &h);
	PROLLY_CALL(cs(), put, store(), &h, node.data, node.len);
	PG_RETURN_TEXT_P(hash_text(&h));
}

/* Collect the store's garbage: what was kept and freed. */
PG_FUNCTION_INFO_V1(test_chunk_gc);
Datum
test_chunk_gc(PG_FUNCTION_ARGS)
{
	bool		full = PG_GETARG_BOOL(0);
	ProllyDatabaseComponents dc;
	ProllyGcResult r;
	TupleDesc	desc;
	Datum		values[4];
	bool		nulls[4] = {false, false, false, false};

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	prolly_database_components(&dc);
	PROLLY_CALL(dc.reclamation, collect, full, &r);
	values[0] = Int64GetDatum((int64) r.chunks_kept);
	values[1] = Int64GetDatum((int64) r.chunks_freed);
	values[2] = Int64GetDatum((int64) r.bytes_freed);
	values[3] = Int64GetDatum((int64) r.window_usec);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc),
													  values, nulls)));
}

typedef struct NewNodes
{
	HTAB	   *seen;
	bool		adding;			/* walking the first commit */
	int64		visited;
} NewNodes;

static bool
note_node(const ProllyNodeRef *node, uint16 level, void *arg)
{
	NewNodes   *n = arg;
	ProllyHash	h;
	bool		found;

	memset(&h, 0, sizeof(h));
	h.len = node->hash.len;
	memcpy(h.bytes, node->hash.bytes, h.len);
	hash_search(n->seen, &h, n->adding ? HASH_ENTER : HASH_FIND, &found);
	if (found)
		return false;
	if (!n->adding)
		n->visited++;
	return true;
}

/*
 * test_walk_new_count(from, to): how many nodes the refs' walk from commit
 * to visits that it doesn't from commit from: what to added to the store.
 */
PG_FUNCTION_INFO_V1(test_walk_new_count);
Datum
test_walk_new_count(PG_FUNCTION_ARGS)
{
	ProllyDatabaseComponents dc;
	ProllyHash	from;
	ProllyHash	to;
	HASHCTL		ctl;
	NewNodes	n = {0};

	text_hash(PG_GETARG_TEXT_PP(0), &from);
	text_hash(PG_GETARG_TEXT_PP(1), &to);
	ctl.keysize = sizeof(ProllyHash);
	ctl.entrysize = sizeof(ProllyHash);
	ctl.hcxt = CurrentMemoryContext;
	n.seen = hash_create("test_walk_new_count", 1024, &ctl,
						 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	prolly_database_components(&dc);
	n.adding = true;
	PROLLY_CALL(dc.refs, walk_commit, &from, note_node, &n);
	n.adding = false;
	PROLLY_CALL(dc.refs, walk_commit, &to, note_node, &n);
	hash_destroy(n.seen);
	PG_RETURN_INT64(n.visited);
}

/* test_memo_counters(reset): the value memo's hits and misses. */
PG_FUNCTION_INFO_V1(test_memo_counters);
Datum
test_memo_counters(PG_FUNCTION_ARGS)
{
	TupleDesc	desc;
	ProllyMemoCounters c;
	Datum		values[2];
	bool		nulls[2] = {0};

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	PROLLY_CALL(prolly_memo_lookup("shared"), counters, &c, PG_GETARG_BOOL(0));
	values[0] = Int64GetDatum((int64) c.hits);
	values[1] = Int64GetDatum((int64) c.misses);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc),
													  values, nulls)));
}
