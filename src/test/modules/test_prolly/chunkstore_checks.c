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

#include "access/htup_details.h"
#include "access/prolly_canon.h"
#include "access/prolly_config.h"
#include "access/prolly_chunkstore.h"
#include "access/prolly_hash.h"
#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "utils/tuplestore.h"
#include "miscadmin.h"
#include "utils/array.h"
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
	sha256(VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b), hash);
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
	PROLLY_CALL(cs(), conjoin, store());
	PG_RETURN_VOID();
}

static void
file_row(const char *name, int nchunks, uint64 size, void *arg)
{
	ReturnSetInfo *rsinfo = arg;
	Datum		values[3];
	bool		nulls[3] = {false, false, false};

	values[0] = CStringGetTextDatum(name);
	values[1] = Int32GetDatum(nchunks);
	values[2] = Int64GetDatum((int64) size);
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

/*
 * The canonical root of the session branch's versioned schemas, folded from
 * the branch's baseline (as commits fold), or from nothing with 'full': the
 * two must be the same.
 */
/* The session branch's working set, folded in full, from no baseline. */
static void
fold_in_full(ProllyNodeRef *root)
{
	PROLLY_CALL(prolly_canon_lookup("folded"), fold, NULL, root, NULL);
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
	refs.location.blkno = InvalidBlockNumber;
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
	refs.location.blkno = InvalidBlockNumber;
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
	PROLLY_CALL(prolly_changes_lookup("pages"), counters, &c, PG_GETARG_BOOL(0));
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
	PROLLY_CALL(canon, fold, NULL, &root, NULL);
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

/* test_refs_epoch(): the session's refs' epoch */
PG_FUNCTION_INFO_V1(test_refs_epoch);
Datum
test_refs_epoch(PG_FUNCTION_ARGS)
{
	ProllyDatabaseComponents dc;

	prolly_database_components(&dc);
	PG_RETURN_INT64((int64) PROLLY_CALL(dc.refs, epoch));
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
