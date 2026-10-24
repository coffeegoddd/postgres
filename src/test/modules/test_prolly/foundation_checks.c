/*--------------------------------------------------------------------------
 *
 * foundation_checks.c
 *		Conformance checks for the foundation interfaces: chunker, key
 *		encoder, value encoder, chunk format and table file format.
 *
 * Each check is written against the interface's contract in its header,
 * not against any one implementation, so every implementation registered
 * for the interface is held to it.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_prolly/foundation_checks.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/tupdesc_details.h"
#include "access/prolly_canon.h"
#include "access/prolly_config.h"
#include "access/prolly_datum.h"
#include "access/prolly_toast.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_am.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_index.h"
#include "catalog/pg_opclass_d.h"
#include "catalog/namespace.h"
#include "catalog/pg_type_d.h"
#include "commands/defrem.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "nodes/value.h"
#include "port/pg_crc32c.h"
#include "test_prolly.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/typcache.h"

/* A fixture whose wire input normalizes ASCII case, unlike text input. */
PG_FUNCTION_INFO_V1(test_normalizing_recv);
Datum
test_normalizing_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	int			len;
	char	   *str = pq_getmsgtext(buf, buf->len - buf->cursor, &len);
	text	   *result;

	for (int i = 0; i < len; i++)
		if (str[i] >= 'A' && str[i] <= 'Z')
			str[i] += 'a' - 'A';
	result = cstring_to_text_with_len(str, len);
	pfree(str);
	PG_RETURN_TEXT_P(result);
}

/* ------------------------------------------------------------------------
 * Chunker
 * ------------------------------------------------------------------------
 */

#define CHUNK_ITEMS		3000
#define CHUNK_CAPACITY	4000

/* Item i: 4-byte ascending key, item space between 20 and 20 + maxval. */
static void
chunk_item(int i, int maxval, uint8 *keybuf, ProllyKey *key, ProllyValue *value,
		   Size *space)
{
	static const uint8 zeros[1024] = {0};

	keybuf[0] = (uint8) (i >> 24);
	keybuf[1] = (uint8) (i >> 16);
	keybuf[2] = (uint8) (i >> 8);
	keybuf[3] = (uint8) i;
	key->data = keybuf;
	key->len = 4;
	value->data = zeros;
	value->len = (i * 7919) % (maxval + 1);
	*space = 20 + value->len;
}

/* Feed items [first, first + n) and record the decisions. */
static void
chunk_run(const ProllyChunkerRoutine *ch, ProllyChunker *c, int first, int n,
		  int maxval, ProllyChunkDecision *out)
{
	for (int i = 0; i < n; i++)
	{
		uint8		keybuf[4];
		ProllyKey	key;
		ProllyValue value;
		Size		space;

		chunk_item(first + i, maxval, keybuf, &key, &value, &space);
		out[i] = PROLLY_CALL(ch, add, c, &key, &value, space);
	}
}

void
chunker_deterministic(const ProllyImplInfo *impl)
{
	const ProllyChunkerRoutine *ch = (const ProllyChunkerRoutine *) impl;
	ProllyChunkDecision *a = palloc(sizeof(ProllyChunkDecision) * CHUNK_ITEMS);
	ProllyChunkDecision *b = palloc(sizeof(ProllyChunkDecision) * CHUNK_ITEMS);
	int			boundaries = 0;

	for (uint16 level = 0; level < 3; level++)
	{
		ProllyChunker *c1 = PROLLY_CALL(ch, begin, level, CHUNK_CAPACITY,
										CurrentMemoryContext);
		ProllyChunker *c2 = PROLLY_CALL(ch, begin, level, CHUNK_CAPACITY,
										CurrentMemoryContext);

		chunk_run(ch, c1, 0, CHUNK_ITEMS, 200, a);
		chunk_run(ch, c2, 0, CHUNK_ITEMS, 200, b);
		PROLLY_CALL(ch, end, c1);
		PROLLY_CALL(ch, end, c2);

		for (int i = 0; i < CHUNK_ITEMS; i++)
		{
			if (a[i] != b[i])
				elog(ERROR, "level %u item %d: decisions %d and %d differ",
					 level, i, a[i], b[i]);
			if (a[i] != PROLLY_CHUNK_CONTINUE)
				boundaries++;
		}
	}
	if (boundaries == 0)
		elog(ERROR, "no boundaries in %d items", 3 * CHUNK_ITEMS);
}

void
chunker_forced_split(const ProllyImplInfo *impl)
{
	const ProllyChunkerRoutine *ch = (const ProllyChunkerRoutine *) impl;
	ProllyChunker *c = PROLLY_CALL(ch, begin, 0, CHUNK_CAPACITY,
								   CurrentMemoryContext);
	Size		node = 0;
	int			forced = 0;

	/* Items up to 1000 bytes, so a 4000-byte node fills often. */
	for (int i = 0; i < CHUNK_ITEMS; i++)
	{
		uint8		keybuf[4];
		ProllyKey	key;
		ProllyValue value;
		Size		space;
		ProllyChunkDecision d;

		chunk_item(i, 1000, keybuf, &key, &value, &space);
		d = PROLLY_CALL(ch, add, c, &key, &value, space);
		if (d == PROLLY_CHUNK_SPLIT_BEFORE)
		{
			forced++;
			node = 0;
		}
		node += space;
		if (node > CHUNK_CAPACITY)
			elog(ERROR, "item %d made a node of %zu bytes, capacity %d",
				 i, node, CHUNK_CAPACITY);
		if (d == PROLLY_CHUNK_SPLIT_AFTER)
			node = 0;
	}
	PROLLY_CALL(ch, end, c);
	if (forced == 0)
		elog(ERROR, "no forced splits with items up to a quarter of capacity");
}

/* ------------------------------------------------------------------------
 * Key encoder
 * ------------------------------------------------------------------------
 */

#define MAX_KEY_COLS 3

typedef struct KeyColSpec
{
	Oid			type;
	Oid			collation;
	int16		options;		/* INDOPTION_* */
	Oid			opclass;		/* InvalidOid: the default for type */
} KeyColSpec;

typedef struct KeyRow
{
	Datum		values[MAX_KEY_COLS];
	bool		isnull[MAX_KEY_COLS];
} KeyRow;

typedef struct KeyCase
{
	int			ncols;
	KeyColSpec	cols[MAX_KEY_COLS];
	ProllyKeyDesc desc;
	Oid			types[MAX_KEY_COLS];
	Oid			opfamilies[MAX_KEY_COLS];
	Oid			collations[MAX_KEY_COLS];
	int16		options[MAX_KEY_COLS];
	FmgrInfo	cmp[MAX_KEY_COLS];
} KeyCase;

static void
key_case_init(KeyCase *kc)
{
	for (int i = 0; i < kc->ncols; i++)
	{
		KeyColSpec *spec = &kc->cols[i];
		Oid			opclass = OidIsValid(spec->opclass) ? spec->opclass :
			GetDefaultOpClass(spec->type, BTREE_AM_OID);
		Oid			opfamily = get_opclass_family(opclass);

		kc->types[i] = spec->type;
		kc->opfamilies[i] = opfamily;
		kc->collations[i] = spec->collation;
		kc->options[i] = spec->options;
		fmgr_info(get_opfamily_proc(opfamily, get_opclass_input_type(opclass),
									get_opclass_input_type(opclass), BTORDER_PROC),
				  &kc->cmp[i]);
	}
	kc->desc.nkeys = kc->ncols;
	kc->desc.types = kc->types;
	kc->desc.opfamilies = kc->opfamilies;
	kc->desc.collations = kc->collations;
	kc->desc.options = kc->options;
}

/* The order the contract requires: opclass order, DESC, NULLS FIRST/LAST. */
static int
reference_compare(KeyCase *kc, const KeyRow *a, const KeyRow *b)
{
	for (int i = 0; i < kc->ncols; i++)
	{
		bool		nulls_first = (kc->options[i] & INDOPTION_NULLS_FIRST) != 0;
		int			r;

		if (a->isnull[i] && b->isnull[i])
			continue;
		if (a->isnull[i])
			return nulls_first ? -1 : 1;
		if (b->isnull[i])
			return nulls_first ? 1 : -1;
		r = DatumGetInt32(FunctionCall2Coll(&kc->cmp[i], kc->collations[i],
											a->values[i], b->values[i]));
		if (kc->options[i] & INDOPTION_DESC)
			INVERT_COMPARE_RESULT(r);
		if (r != 0)
			return r < 0 ? -1 : 1;
	}
	return 0;
}

/* A value of type from its text form, through the type's input function. */
static Datum
typed_value(Oid type, const char *s)
{
	Oid			input;
	Oid			ioparam;

	getTypeInputInfo(type, &input, &ioparam);
	return OidInputFunctionCall(input, unconstify(char *, s), ioparam, -1);
}

static Datum
numeric_datum(const char *s)
{
	return DirectFunctionCall3(numeric_in, CStringGetDatum(s),
							   ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
}

/* Test values for one column; nulls appended if with_nulls. */
static int
column_values(Oid type, bool with_nulls, Datum *values, bool *isnull)
{
	static const int64 ints[] = {PG_INT32_MIN, -100000, -256, -1, 0, 1, 7,
	255, 256, 65536, PG_INT32_MAX};
	static const char *const texts[] = {"", "a", "A", "ab", "aB", "Ab", "b",
	"B", "a b", "a-b", "ab ", "10", "9", "zz", "Z", "~", "abc", "abd"};
	static const char *const numerics[] = {"-1e10", "-1", "-0.5", "0", "0.0001",
	"1", "1.0", "1.5", "2", "1e20", "NaN", "Infinity", "-Infinity"};
	static const char *const arrays[] = {"{}", "{1}", "{1,2}", "{2}", "{1,NULL}",
	"{NULL}", "{{1,2},{3,4}}", "[0:1]={1,2}", "{-5,7}"};
	static const char *const cidrs[] = {"0.0.0.0/0", "10.0.0.0/8",
	"10.0.0.0/16", "10.1.0.0/16", "10.0.0.1/32", "192.168.0.0/16",
	"192.168.1.0/24", "::/0", "2001:db8::/32", "2001:db8::1/128"};
	int			n = 0;

	switch (type)
	{
		case INT4OID:
			for (int i = 0; i < lengthof(ints); i++)
				values[n++] = Int32GetDatum((int32) ints[i]);
			break;
		case INT8OID:
			for (int i = 0; i < lengthof(ints); i++)
				values[n++] = Int64GetDatum(ints[i] * (int64) 4294967296 + i);
			values[n++] = Int64GetDatum(PG_INT64_MIN);
			values[n++] = Int64GetDatum(PG_INT64_MAX);
			break;
		case INT2OID:
			for (int i = 0; i < lengthof(ints); i++)
				values[n++] = Int16GetDatum((int16) Max(Min(ints[i], PG_INT16_MAX),
														PG_INT16_MIN));
			break;
		case BOOLOID:
			values[n++] = BoolGetDatum(false);
			values[n++] = BoolGetDatum(true);
			break;
		case TEXTOID:
		case VARCHAROID:
			for (int i = 0; i < lengthof(texts); i++)
				values[n++] = CStringGetTextDatum(texts[i]);
			break;
		case NUMERICOID:
			for (int i = 0; i < lengthof(numerics); i++)
				values[n++] = numeric_datum(numerics[i]);
			break;
		case INT4ARRAYOID:
			for (int i = 0; i < lengthof(arrays); i++)
				values[n++] = typed_value(INT4ARRAYOID, arrays[i]);
			break;
		case CIDROID:
			/* (its opfamily registers a comparison for inet alone) */
			for (int i = 0; i < lengthof(cidrs); i++)
				values[n++] = typed_value(CIDROID, cidrs[i]);
			break;
		default:
			elog(ERROR, "no test values for type %u", type);
	}
	for (int i = 0; i < n; i++)
		isnull[i] = false;
	if (with_nulls)
	{
		values[n] = (Datum) 0;
		isnull[n++] = true;
	}
	return n;
}

/* Rows: the cross product of each column's values. */
static KeyRow *
key_rows(KeyCase *kc, bool with_nulls, int *nrows)
{
	Datum		vals[MAX_KEY_COLS][32];
	bool		nulls[MAX_KEY_COLS][32];
	int			counts[MAX_KEY_COLS];
	int			total = 1;
	KeyRow	   *rows;

	for (int c = 0; c < kc->ncols; c++)
	{
		counts[c] = column_values(kc->types[c], with_nulls, vals[c], nulls[c]);
		total *= counts[c];
	}
	rows = palloc(sizeof(KeyRow) * total);
	for (int r = 0; r < total; r++)
	{
		int			rest = r;

		for (int c = kc->ncols - 1; c >= 0; c--)
		{
			rows[r].values[c] = vals[c][rest % counts[c]];
			rows[r].isnull[c] = nulls[c][rest % counts[c]];
			rest /= counts[c];
		}
	}
	*nrows = total;
	return rows;
}

static char *
describe_row(KeyCase *kc, const KeyRow *row)
{
	StringInfoData s;

	initStringInfo(&s);
	appendStringInfoChar(&s, '(');
	for (int i = 0; i < kc->ncols; i++)
	{
		Oid			out;
		bool		varlena;

		if (i > 0)
			appendStringInfoString(&s, ", ");
		if (row->isnull[i])
		{
			appendStringInfoString(&s, "NULL");
			continue;
		}
		getTypeOutputInfo(kc->types[i], &out, &varlena);
		appendStringInfo(&s, "'%s'", OidOutputFunctionCall(out, row->values[i]));
	}
	appendStringInfoChar(&s, ')');
	return s.data;
}

/* Every pair of rows must compare as reference_compare says. */
static void
check_key_order(const ProllyKeyEncoderRoutine *ke, KeyCase *kc, bool with_nulls)
{
	ProllyKeyEncoder *enc;
	KeyRow	   *rows;
	int			nrows;
	StringInfoData *keys;

	key_case_init(kc);
	for (int i = 0; i < kc->ncols; i++)
	{
		if (!PROLLY_CALL(ke, supports, kc->opfamilies[i], kc->types[i],
						 kc->collations[i]))
			elog(ERROR, "encoder does not support type %s",
				 format_type_be(kc->types[i]));
	}
	enc = PROLLY_CALL(ke, begin, &kc->desc, CurrentMemoryContext);
	rows = key_rows(kc, with_nulls, &nrows);
	keys = palloc(sizeof(StringInfoData) * nrows);
	for (int r = 0; r < nrows; r++)
	{
		initStringInfo(&keys[r]);
		PROLLY_CALL(ke, encode, enc, rows[r].values, rows[r].isnull, &keys[r]);
	}

	for (int a = 0; a < nrows; a++)
	{
		for (int b = 0; b < nrows; b++)
		{
			ProllyKey	ka = {(const uint8 *) keys[a].data, keys[a].len};
			ProllyKey	kb = {(const uint8 *) keys[b].data, keys[b].len};
			int			got = PROLLY_CALL(ke, compare, enc, &ka, &kb);
			int			want = reference_compare(kc, &rows[a], &rows[b]);

			got = got < 0 ? -1 : (got > 0 ? 1 : 0);
			if (got != want)
				elog(ERROR, "%s vs %s: compare gave %d, opclass order is %d",
					 describe_row(kc, &rows[a]), describe_row(kc, &rows[b]),
					 got, want);
		}
	}
	PROLLY_CALL(ke, end, enc);
}

/* A collation that isn't C, if the build and database encoding have one. */
static Oid
non_c_collation(void)
{
#ifdef USE_ICU
	if (is_encoding_supported_by_icu(GetDatabaseEncoding()))
		return get_collation_oid(list_make1(makeString("unicode")), false);
#endif
	return DEFAULT_COLLATION_OID;
}

void
keyenc_order_matches_opclass(const ProllyImplInfo *impl)
{
	const ProllyKeyEncoderRoutine *ke = (const ProllyKeyEncoderRoutine *) impl;
	KeyCase		cases[] = {
		{1, {{INT2OID}}},
		{1, {{INT4OID}}},
		{1, {{INT8OID}}},
		{1, {{BOOLOID}}},
		{1, {{TEXTOID, C_COLLATION_OID}}},
		{1, {{TEXTOID, non_c_collation()}}},
		{1, {{VARCHAROID, C_COLLATION_OID}}},
		{1, {{VARCHAROID, non_c_collation()}}},
		{1, {{VARCHAROID, C_COLLATION_OID, 0, VARCHAR_BTREE_PATTERN_OPS_OID}}},
		{1, {{NUMERICOID}}},
		{2, {{INT4OID}, {TEXTOID, C_COLLATION_OID}}},
		{2, {{TEXTOID, non_c_collation()}, {INT8OID}}},
		{2, {{NUMERICOID}, {BOOLOID}}},
		{2, {{INT4OID}, {VARCHAROID, C_COLLATION_OID}}},
		{1, {{CIDROID}}},
		{2, {{CIDROID}, {INT4OID}}},
		{1, {{INT4ARRAYOID}}},
		{2, {{INT4ARRAYOID}, {TEXTOID, C_COLLATION_OID}}},
	};

	for (int i = 0; i < lengthof(cases); i++)
		check_key_order(ke, &cases[i], false);
}

void
keyenc_desc_and_nulls(const ProllyImplInfo *impl)
{
	const ProllyKeyEncoderRoutine *ke = (const ProllyKeyEncoderRoutine *) impl;
	static const int16 opts[] = {
		0, INDOPTION_NULLS_FIRST, INDOPTION_DESC,
		INDOPTION_DESC | INDOPTION_NULLS_FIRST,
	};

	for (int o = 0; o < lengthof(opts); o++)
	{
		KeyCase		cases[] = {
			{1, {{INT4OID, InvalidOid, opts[o]}}},
			{1, {{TEXTOID, C_COLLATION_OID, opts[o]}}},
			{1, {{TEXTOID, non_c_collation(), opts[o]}}},
			{1, {{VARCHAROID, C_COLLATION_OID, opts[o]}}},
			{1, {{VARCHAROID, non_c_collation(), opts[o]}}},
			{1, {{VARCHAROID, C_COLLATION_OID, opts[o], VARCHAR_BTREE_PATTERN_OPS_OID}}},
			{1, {{CIDROID, InvalidOid, opts[o]}}},
			{1, {{NUMERICOID, InvalidOid, opts[o]}}},
			{2, {{INT8OID, InvalidOid, opts[o]}, {TEXTOID, C_COLLATION_OID, 0}}},
			{2, {{BOOLOID, InvalidOid, 0}, {INT2OID, InvalidOid, opts[o]}}},
		};

		for (int i = 0; i < lengthof(cases); i++)
			check_key_order(ke, &cases[i], true);
	}
}

/* ------------------------------------------------------------------------
 * Value encoder
 * ------------------------------------------------------------------------
 */

typedef enum ColKind
{
	COL_I4,
	COL_TEXT,
	COL_I8,
	COL_BOOL,
	COL_NUMERIC,
	COL_I4_ARRAY,
	COL_JSON,
	COL_COMPOSITE,				/* pg_am's row type */
} ColKind;

typedef struct ColSpec
{
	ColKind		kind;
	bool		dropped;
	bool		has_missing;	/* added with DEFAULT 5 (COL_I4 only) */
} ColSpec;

static Oid
col_type(ColKind kind)
{
	switch (kind)
	{
		case COL_I4:
			return INT4OID;
		case COL_TEXT:
			return TEXTOID;
		case COL_I8:
			return INT8OID;
		case COL_BOOL:
			return BOOLOID;
		case COL_NUMERIC:
			return NUMERICOID;
		case COL_I4_ARRAY:
			return INT4ARRAYOID;
		case COL_JSON:
			return JSONOID;
		case COL_COMPOSITE:
			return get_rel_type_id(AccessMethodRelationId);
	}
	return InvalidOid;
}

static TupleDesc
make_desc(const ColSpec *cols, int n)
{
	TupleDesc	desc = CreateTemplateTupleDesc(n);

	for (int i = 0; i < n; i++)
	{
		TupleDescInitEntry(desc, i + 1, psprintf("c%d", i + 1),
						   col_type(cols[i].kind), -1, 0);
		TupleDescAttr(desc, i)->attisdropped = cols[i].dropped;
		if (cols[i].has_missing)
		{
			if (desc->constr == NULL)
			{
				desc->constr = palloc0(sizeof(TupleConstr));
				desc->constr->missing = palloc0(sizeof(AttrMissing) * n);
			}
			TupleDescAttr(desc, i)->atthasmissing = true;
			desc->constr->missing[i].am_present = true;
			desc->constr->missing[i].am_value = Int32GetDatum(5);
		}
		populate_compact_attribute(desc, i);
	}
	TupleDescFinalize(desc);
	return desc;
}

static Datum
composite_value(int i, const char *name)
{
	TupleDesc	desc = lookup_rowtype_tupdesc_copy(col_type(COL_COMPOSITE), -1);
	Datum		values[4];
	bool		nulls[4] = {false, false, false, i % 4 == 3};

	Assert(desc->natts == 4);
	values[0] = ObjectIdGetDatum(1000 + i);
	values[1] = DirectFunctionCall1(namein, CStringGetDatum(name));
	values[2] = ObjectIdGetDatum(0);
	values[3] = CharGetDatum('t');
	return heap_copy_tuple_as_datum(heap_form_tuple(desc, values, nulls), desc);
}

/* Row i's value for a column of kind; text-like values contain text. */
static Datum
col_value(ColKind kind, int i, const char *text)
{
	switch (kind)
	{
		case COL_I4:
			return Int32GetDatum(i * 1000 - 7);
		case COL_TEXT:
			return CStringGetTextDatum(i % 3 == 0 ? "" : text);
		case COL_I8:
			return Int64GetDatum((int64) i << 40);
		case COL_BOOL:
			return BoolGetDatum(i % 2 == 0);
		case COL_NUMERIC:
			return numeric_datum(i % 2 ? "3.14159" : "-1e30");
		case COL_I4_ARRAY:
			return typed_value(INT4ARRAYOID,
							   i % 2 ? "[0:1][2:3]={{1,2},{NULL,4}}" : "{5,6,7}");
		case COL_JSON:
			return typed_value(JSONOID,
							   psprintf("{\"k\": \"%s\", \"n\": %d}", text, i));
		case COL_COMPOSITE:
			return composite_value(i, text);
	}
	return (Datum) 0;
}

/*
 * Fill slot (laid out as cols) with row i.  Columns whose bit is set in
 * nullmask are NULL, as are dropped columns.
 */
static void
store_row(TupleTableSlot *slot, const ColSpec *cols, int i, uint32 nullmask,
		  const char *text)
{
	ExecClearTuple(slot);
	for (int c = 0; c < slot->tts_tupleDescriptor->natts; c++)
	{
		slot->tts_isnull[c] = cols[c].dropped || ((nullmask >> c) & 1);
		slot->tts_values[c] = slot->tts_isnull[c] ? (Datum) 0 :
			col_value(cols[c].kind, i, text);
	}
	ExecStoreVirtualTuple(slot);
}

static void
encode_row(const ProllyValueEncoderRoutine *ve, ProllyValueEncoder *enc,
		   TupleTableSlot *slot, StringInfo out)
{
	resetStringInfo(out);
	PROLLY_CALL(ve, encode, enc, slot, out);
}

static bool
same_bytes(StringInfo a, StringInfo b)
{
	return a->len == b->len && memcmp(a->data, b->data, a->len) == 0;
}

static char *
hex(StringInfo s)
{
	StringInfoData h;

	initStringInfo(&h);
	for (int i = 0; i < s->len; i++)
		appendStringInfo(&h, "%02x", (uint8) s->data[i]);
	return h.data;
}

/* Every live column of want and got (both laid out as desc) prints alike. */
static void
check_same_row(TupleTableSlot *want, TupleTableSlot *got, int row)
{
	TupleDesc	desc = want->tts_tupleDescriptor;

	slot_getallattrs(want);
	slot_getallattrs(got);
	for (int c = 0; c < desc->natts; c++)
	{
		Oid			out;
		bool		varlena;
		char	   *a;
		char	   *b;

		if (TupleDescAttr(desc, c)->attisdropped)
			continue;
		if (want->tts_isnull[c] != got->tts_isnull[c])
			elog(ERROR, "row %d column %d: NULL did not round-trip", row, c + 1);
		if (want->tts_isnull[c])
			continue;
		getTypeOutputInfo(TupleDescAttr(desc, c)->atttypid, &out, &varlena);
		a = OidOutputFunctionCall(out, want->tts_values[c]);
		b = OidOutputFunctionCall(out, got->tts_values[c]);
		if (strcmp(a, b) != 0)
			elog(ERROR, "row %d column %d: \"%s\" decoded as \"%s\"",
				 row, c + 1, a, b);
	}
}

static const ColSpec all_cols[] = {
	{COL_I4}, {COL_TEXT}, {COL_I8}, {COL_BOOL}, {COL_NUMERIC},
	{COL_I4_ARRAY}, {COL_JSON}, {COL_COMPOSITE},
};

void
valenc_round_trip(const ProllyImplInfo *impl)
{
	const ProllyValueEncoderRoutine *ve = (const ProllyValueEncoderRoutine *) impl;
	TupleDesc	desc = make_desc(all_cols, lengthof(all_cols));
	ProllyValueEncoder *enc = PROLLY_CALL(ve, begin, desc, CurrentMemoryContext);
	TupleTableSlot *src = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	const TupleTableSlotOps *const kinds[] = {
		&TTSOpsVirtual, &TTSOpsMinimalTuple, &TTSOpsHeapTuple,
	};
	StringInfoData buf;

	initStringInfo(&buf);
	for (int i = 0; i < 64; i++)
	{
		ProllyValue value;

		/* rows 0..7 have no NULLs; later rows NULL a varying set */
		store_row(src, all_cols, i, i < 8 ? 0 : (uint32) (i * 37), "abc");
		encode_row(ve, enc, src, &buf);
		value.data = (const uint8 *) buf.data;
		value.len = buf.len;

		for (int k = 0; k < lengthof(kinds); k++)
		{
			TupleTableSlot *dst = MakeSingleTupleTableSlot(desc, kinds[k]);

			PROLLY_CALL(ve, decode, enc, &value, dst);
			check_same_row(src, dst, i);
			ExecDropSingleTupleTableSlot(dst);
		}
	}
	ExecDropSingleTupleTableSlot(src);
	PROLLY_CALL(ve, end, enc);
}

/*
 * Long values out of line: a value whose canonical bytes are longer than
 * 2048 is stored as a reference in the encoder's value store, and the row
 * holds only the reference; refs() lists those references; the row decodes
 * whole.  A value of exactly 2048 bytes stays in the row.  Encoding a long
 * value without a value store is an error, never a row in another format.
 */
void
valenc_long_values(const ProllyImplInfo *impl)
{
	const ProllyValueEncoderRoutine *ve = (const ProllyValueEncoderRoutine *) impl;
	static const ColSpec cols[] = {{COL_I4}, {COL_TEXT}, {COL_I4_ARRAY}};
	TupleDesc	desc = make_desc(cols, lengthof(cols));
	ProllyValueEncoder *enc = PROLLY_CALL(ve, begin, desc, CurrentMemoryContext);
	TupleTableSlot *src = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	TupleTableSlot *dst = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	ProllyTreeEnv env;
	ProllyValueStore values = {0};
	StringInfoData buf;
	StringInfoData refs;
	static const int lens[] = {2048, 2049, 100000};

	prolly_folded_tree_env(&env);
	values.tree = prolly_tree_lookup("cow");
	values.env = &env;
	initStringInfo(&buf);
	initStringInfo(&refs);
	for (int l = 0; l < lengthof(lens); l++)
	{
		char	   *text = palloc(lens[l] + 1);
		ProllyValue row;
		bool		failed = false;
		MemoryContext cxt = CurrentMemoryContext;

		for (int i = 0; i < lens[l]; i++)
			text[i] = 'a' + (i * 7 + i / 26) % 26;
		text[lens[l]] = '\0';
		store_row(src, cols, 1, 0, text);

		/* without a value store: an error, for a long value only */
		PG_TRY();
		{
			encode_row(ve, enc, src, &buf);
		}
		PG_CATCH();
		{
			MemoryContextSwitchTo(cxt);
			FlushErrorState();
			failed = true;
		}
		PG_END_TRY();
		if (failed != (lens[l] > 2048))
			elog(ERROR, "a %d-byte value without a value store %s", lens[l],
				 failed ? "failed" : "encoded");

		PROLLY_CALL(ve, set_values, enc, &values);
		encode_row(ve, enc, src, &buf);
		row.data = (const uint8 *) buf.data;
		row.len = buf.len;
		if ((lens[l] > 2048) != (buf.len < 200))
			elog(ERROR, "a row with a %d-byte value encoded in %d bytes",
				 lens[l], buf.len);
		resetStringInfo(&refs);
		PROLLY_CALL(ve, refs, enc, &row, &refs);
		if ((lens[l] > 2048) != (refs.len > 0))
			elog(ERROR, "a row with a %d-byte value lists %d bytes of references",
				 lens[l], refs.len);
		if (refs.len > 0)
		{
			StringInfoData got;
			ProllyValue ref = {(const uint8 *) refs.data, refs.len};

			initStringInfo(&got);
			PROLLY_CALL(values.tree, value_read, &env, &ref, 0, PG_UINT64_MAX, &got);
			if (got.len != lens[l] || memcmp(got.data, text, got.len) != 0)
				elog(ERROR, "a %d-byte value's reference names %d other bytes",
					 lens[l], got.len);
			pfree(got.data);
		}

		/* a row with a byte after its values: refs() refuses it, as decode() */
		{
			StringInfoData bad;
			ProllyValue badrow;

			initStringInfo(&bad);
			appendBinaryStringInfo(&bad, buf.data, buf.len);
			appendStringInfoChar(&bad, '\x01');
			badrow.data = (const uint8 *) bad.data;
			badrow.len = bad.len;
			failed = false;
			PG_TRY();
			{
				PROLLY_CALL(ve, refs, enc, &badrow, &refs);
			}
			PG_CATCH();
			{
				MemoryContextSwitchTo(cxt);
				FlushErrorState();
				failed = true;
			}
			PG_END_TRY();
			if (!failed)
				elog(ERROR, "a row with a trailing byte listed its references");
			pfree(bad.data);
		}
		PROLLY_CALL(ve, decode, enc, &row, dst);
		check_same_row(src, dst, l);
		PROLLY_CALL(ve, set_values, enc, NULL);
		pfree(text);
	}
	ExecDropSingleTupleTableSlot(src);
	ExecDropSingleTupleTableSlot(dst);
	PROLLY_CALL(ve, end, enc);
}

/*
 * Long values kept: decode_kept() of a row against one with the same long
 * values leaves them unread and marks them, and encode_kept() of what it
 * decoded, the kept columns taken from that row, gives the row encode()
 * gave, without reading them.  A column marked kept where that row holds
 * no reference is an error.
 */
void
valenc_kept_values(const ProllyImplInfo *impl)
{
	const ProllyValueEncoderRoutine *ve = (const ProllyValueEncoderRoutine *) impl;
	static const ColSpec cols[] = {{COL_I4}, {COL_TEXT}, {COL_I4_ARRAY}};
	TupleDesc	desc = make_desc(cols, lengthof(cols));
	ProllyValueEncoder *enc = PROLLY_CALL(ve, begin, desc, CurrentMemoryContext);
	TupleTableSlot *src = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	TupleTableSlot *dst = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	ProllyTreeEnv env;
	ProllyValueStore values = {0};
	StringInfoData long_row;
	StringInfoData short_row;
	StringInfoData again;
	ProllyValue lrow;
	ProllyValue srow;
	bool		kept[3];
	char	   *text = palloc(100001);
	bool		failed = false;
	MemoryContext cxt = CurrentMemoryContext;

	prolly_folded_tree_env(&env);
	values.tree = prolly_tree_lookup("cow");
	values.env = &env;
	PROLLY_CALL(ve, set_values, enc, &values);
	for (int i = 0; i < 100000; i++)
		text[i] = 'a' + (i * 7 + i / 26) % 26;
	text[100000] = '\0';
	initStringInfo(&long_row);
	initStringInfo(&short_row);
	initStringInfo(&again);
	store_row(src, cols, 1, 0, text);
	encode_row(ve, enc, src, &long_row);
	lrow.data = (const uint8 *) long_row.data;
	lrow.len = long_row.len;
	store_row(src, cols, 1, 0, "short");
	encode_row(ve, enc, src, &short_row);
	srow.data = (const uint8 *) short_row.data;
	srow.len = short_row.len;

	PROLLY_CALL(ve, decode_kept, enc, &lrow, &lrow, dst, kept);
	if (kept[0] || !kept[1] || kept[2] || !dst->tts_isnull[1])
		elog(ERROR, "decode_kept() didn't keep just the long value");
	PROLLY_CALL(ve, encode_kept, enc, dst, &lrow, kept, &again);
	if (again.len != long_row.len ||
		memcmp(again.data, long_row.data, again.len) != 0)
		elog(ERROR, "encode_kept() of a row kept from itself isn't the row");

	/* the short row: nothing kept, encoded as encode() does */
	PROLLY_CALL(ve, decode_kept, enc, &srow, &lrow, dst, kept);
	if (kept[0] || kept[1] || kept[2])
		elog(ERROR, "decode_kept() kept a value the rows don't share");
	resetStringInfo(&again);
	PROLLY_CALL(ve, encode_kept, enc, dst, &lrow, kept, &again);
	if (again.len != short_row.len ||
		memcmp(again.data, short_row.data, again.len) != 0)
		elog(ERROR, "encode_kept() with nothing kept isn't encode()");

	/* kept from a row holding no reference there */
	kept[1] = true;
	resetStringInfo(&again);
	PG_TRY();
	{
		PROLLY_CALL(ve, encode_kept, enc, dst, &srow, kept, &again);
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(cxt);
		FlushErrorState();
		failed = true;
	}
	PG_END_TRY();
	if (!failed)
		elog(ERROR, "encode_kept() kept a reference the row doesn't hold");

	pfree(text);
	ExecDropSingleTupleTableSlot(src);
	ExecDropSingleTupleTableSlot(dst);
	PROLLY_CALL(ve, end, enc);
}

/*
 * Reference encodings of rows 1 and 9 of a table (int4, text, int8, bool,
 * numeric); row 9 has NULLs.  These bytes are the canonical format, so they
 * are the same on every platform.  An implementation whose bytes depend on
 * the platform fails here on all but one.
 */
#define REFERENCE_ROW1_HEX "050004000003e10361626308000001000000000001000e0003000000000005000305872328"
#define REFERENCE_ROW9_HEX "0509000800000900000000000e0003000000000005000305872328"

static const char *const reference_hex[] = {
	REFERENCE_ROW1_HEX,
	REFERENCE_ROW9_HEX,
};

void
valenc_platform_independent(const ProllyImplInfo *impl)
{
	const ProllyValueEncoderRoutine *ve = (const ProllyValueEncoderRoutine *) impl;
	TupleDesc	desc = make_desc(all_cols, 5);
	ProllyValueEncoder *enc = PROLLY_CALL(ve, begin, desc, CurrentMemoryContext);
	TupleTableSlot *virt = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	TupleTableSlot *heap = MakeSingleTupleTableSlot(desc, &TTSOpsHeapTuple);
	static const int rows[] = {1, 9};
	static const uint32 nullmasks[] = {0, 0x9};
	StringInfoData a,
				b;

	initStringInfo(&a);
	initStringInfo(&b);
	for (int r = 0; r < lengthof(rows); r++)
	{
		/* The same row from two kinds of slot must encode identically. */
		store_row(virt, all_cols, rows[r], nullmasks[r], "abc");
		ExecCopySlot(heap, virt);
		encode_row(ve, enc, virt, &a);
		encode_row(ve, enc, heap, &b);
		if (!same_bytes(&a, &b))
			elog(ERROR, "row %d encodes differently from a heap tuple slot",
				 rows[r]);
		if (strcmp(hex(&a), reference_hex[r]) != 0)
			elog(ERROR, "row %d encodes as %s, the reference encoding is %s",
				 rows[r], hex(&a), reference_hex[r]);
	}
	ExecDropSingleTupleTableSlot(virt);
	ExecDropSingleTupleTableSlot(heap);
	PROLLY_CALL(ve, end, enc);
}

/*
 * A table that dropped a column, or added nullable columns that are NULL,
 * encodes rows exactly as one that never had those columns.
 */
void
valenc_independent_of_history(const ProllyImplInfo *impl)
{
	const ProllyValueEncoderRoutine *ve = (const ProllyValueEncoderRoutine *) impl;
	static const ColSpec plain[] = {{COL_I4}, {COL_TEXT}, {COL_NUMERIC}};
	static const ColSpec history[] = {
		{COL_I4}, {COL_I8, true}, {COL_TEXT}, {COL_COMPOSITE, true},
		{COL_NUMERIC}, {COL_JSON}, {COL_I4_ARRAY},
	};
	TupleDesc	pdesc = make_desc(plain, lengthof(plain));
	TupleDesc	hdesc = make_desc(history, lengthof(history));
	ProllyValueEncoder *penc = PROLLY_CALL(ve, begin, pdesc, CurrentMemoryContext);
	ProllyValueEncoder *henc = PROLLY_CALL(ve, begin, hdesc, CurrentMemoryContext);
	TupleTableSlot *pslot = MakeSingleTupleTableSlot(pdesc, &TTSOpsVirtual);
	TupleTableSlot *hslot = MakeSingleTupleTableSlot(hdesc, &TTSOpsHeapTuple);
	StringInfoData a,
				b;

	initStringInfo(&a);
	initStringInfo(&b);
	for (int i = 0; i < 16; i++)
	{
		uint32		pnulls = (i >= 8) ? (uint32) (i & 0x7) : 0;
		/* the same live columns NULL; added columns 5, 6 NULL */
		uint32		hnulls = (pnulls & 0x1) | ((pnulls & 0x2) << 1) |
			((pnulls & 0x4) << 2) | 0x60;

		store_row(pslot, plain, i, pnulls, "abc");
		store_row(hslot, history, i, hnulls, "abc");
		encode_row(ve, penc, pslot, &a);
		encode_row(ve, henc, hslot, &b);
		if (!same_bytes(&a, &b))
			elog(ERROR, "row %d: %s without the table's history, %s with it",
				 i, hex(&a), hex(&b));
	}

	/* An added column that is not NULL must change the bytes. */
	store_row(hslot, history, 1, 0x20, "abc");
	encode_row(ve, henc, hslot, &b);
	store_row(pslot, plain, 1, 0, "abc");
	encode_row(ve, penc, pslot, &a);
	if (same_bytes(&a, &b))
		elog(ERROR, "a non-NULL added column did not change the encoding");
}

/* The session's client_encoding must not change the bytes. */
void
valenc_independent_of_session(const ProllyImplInfo *impl)
{
	const ProllyValueEncoderRoutine *ve = (const ProllyValueEncoderRoutine *) impl;
	static const ColSpec cols[] = {{COL_TEXT}, {COL_JSON}, {COL_COMPOSITE}};
	TupleDesc	desc = make_desc(cols, lengthof(cols));
	ProllyValueEncoder *enc = PROLLY_CALL(ve, begin, desc, CurrentMemoryContext);
	TupleTableSlot *slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	const char *text = "caf\xc3\xa9";	/* "café" in UTF-8 */
	char	   *old_encoding = pstrdup(GetConfigOption("client_encoding", false, false));
	StringInfoData utf8,
				latin1;

	if (GetDatabaseEncoding() != PG_UTF8)
		return;					/* the test text needs a UTF-8 database */

	initStringInfo(&utf8);
	initStringInfo(&latin1);
	store_row(slot, cols, 1, 0, text);
	PG_TRY();
	{
		SetConfigOption("client_encoding", "UTF8", PGC_USERSET, PGC_S_SESSION);
		encode_row(ve, enc, slot, &utf8);
		SetConfigOption("client_encoding", "LATIN1", PGC_USERSET, PGC_S_SESSION);
		encode_row(ve, enc, slot, &latin1);
	}
	PG_FINALLY();
	{
		SetConfigOption("client_encoding", old_encoding, PGC_USERSET, PGC_S_SESSION);
	}
	PG_END_TRY();

	if (!same_bytes(&utf8, &latin1))
		elog(ERROR, "client_encoding changed the encoding: %s under UTF8, %s under LATIN1",
			 hex(&utf8), hex(&latin1));
}

/*
 * A column added with a default: rows from before it decode to the default,
 * and rows written since keep their own values, NULL included.
 */
void
valenc_missing_values(const ProllyImplInfo *impl)
{
	const ProllyValueEncoderRoutine *ve = (const ProllyValueEncoderRoutine *) impl;
	static const ColSpec before[] = {{COL_I4}, {COL_TEXT}};
	static const ColSpec after[] = {{COL_I4}, {COL_TEXT}, {COL_I4, false, true}};
	TupleDesc	bdesc = make_desc(before, lengthof(before));
	TupleDesc	adesc = make_desc(after, lengthof(after));
	ProllyValueEncoder *benc = PROLLY_CALL(ve, begin, bdesc, CurrentMemoryContext);
	ProllyValueEncoder *aenc = PROLLY_CALL(ve, begin, adesc, CurrentMemoryContext);
	TupleTableSlot *bslot = MakeSingleTupleTableSlot(bdesc, &TTSOpsVirtual);
	TupleTableSlot *aslot = MakeSingleTupleTableSlot(adesc, &TTSOpsVirtual);
	TupleTableSlot *out = MakeSingleTupleTableSlot(adesc, &TTSOpsVirtual);
	StringInfoData buf;
	ProllyValue value;

	initStringInfo(&buf);

	/* A row from before the column: the default. */
	store_row(bslot, before, 2, 0, "old");
	encode_row(ve, benc, bslot, &buf);
	value.data = (const uint8 *) buf.data;
	value.len = buf.len;
	PROLLY_CALL(ve, decode, aenc, &value, out);
	slot_getallattrs(out);
	if (out->tts_isnull[2] || DatumGetInt32(out->tts_values[2]) != 5)
		elog(ERROR, "a row from before the column did not get its default");

	/* Rows written since: their own value, and NULL stays NULL. */
	for (int nullit = 0; nullit < 2; nullit++)
	{
		store_row(aslot, after, 3, nullit ? 0x4 : 0, "new");
		encode_row(ve, aenc, aslot, &buf);
		value.data = (const uint8 *) buf.data;
		value.len = buf.len;
		PROLLY_CALL(ve, decode, aenc, &value, out);
		check_same_row(aslot, out, 3);
	}
}

/* ------------------------------------------------------------------------
 * SQL-callable: canonical bytes of a value, and their round trip
 * ------------------------------------------------------------------------
 */

PG_FUNCTION_INFO_V1(prolly_canonical_value);
PG_FUNCTION_INFO_V1(prolly_canonical_round_trip);
PG_FUNCTION_INFO_V1(prolly_canonical_decode);
PG_FUNCTION_INFO_V1(test_toast_hash_pointer);
PG_FUNCTION_INFO_V1(test_toast_pointer);
PG_FUNCTION_INFO_V1(test_toast_pointer_bytes);
PG_FUNCTION_INFO_V1(test_toast_pointer_value);
PG_FUNCTION_INFO_V1(test_toast_counters);

Datum
prolly_canonical_value(PG_FUNCTION_ARGS)
{
	Oid			type = get_fn_expr_argtype(fcinfo->flinfo, 0);
	ProllyDatumCodec *codec = prolly_datum_codec(type, -1, CurrentMemoryContext);
	StringInfoData buf;

	initStringInfo(&buf);
	prolly_datum_encode(codec, PG_GETARG_DATUM(0), &buf);
	PG_RETURN_BYTEA_P(cstring_to_text_with_len(buf.data, buf.len));
}

Datum
prolly_canonical_round_trip(PG_FUNCTION_ARGS)
{
	Oid			type = get_fn_expr_argtype(fcinfo->flinfo, 0);
	ProllyDatumCodec *codec = prolly_datum_codec(type, -1, CurrentMemoryContext);
	StringInfoData buf;

	initStringInfo(&buf);
	prolly_datum_encode(codec, PG_GETARG_DATUM(0), &buf);
	PG_RETURN_DATUM(prolly_datum_decode(codec, buf.data, buf.len));
}

Datum
prolly_canonical_decode(PG_FUNCTION_ARGS)
{
	Oid			type = get_fn_expr_argtype(fcinfo->flinfo, 1);
	ProllyDatumCodec *codec;
	bytea	   *b;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();
	codec = prolly_datum_codec(type, -1, CurrentMemoryContext);
	b = PG_GETARG_BYTEA_PP(0);
	PG_RETURN_DATUM(prolly_datum_decode(codec, VARDATA_ANY(b),
										VARSIZE_ANY_EXHDR(b)));
}

Datum
test_toast_hash_pointer(PG_FUNCTION_ARGS)
{
	Oid			type = get_fn_expr_argtype(fcinfo->flinfo, 0);

	if (get_typlen(type) != -1)
		elog(ERROR, "only a varlena value can be stored by content");
	PG_RETURN_POINTER(prolly_toast_store(PG_GETARG_DATUM(0), type));
}

Datum
test_toast_counters(PG_FUNCTION_ARGS)
{
	ProllyNodeCounters c;
	TupleDesc	desc;
	Datum		values[2];
	bool		nulls[2] = {false, false};

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	prolly_toast_counters(&c, PG_GETARG_BOOL(0));
	values[0] = Int64GetDatum(c.nodes_read);
	values[1] = Int64GetDatum(c.nodes_written);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc),
													  values, nulls)));
}

Datum
test_toast_pointer_bytes(PG_FUNCTION_ARGS)
{
	varlena    *v = (varlena *) DatumGetPointer(PG_GETARG_DATUM(0));

	if (get_typlen(get_fn_expr_argtype(fcinfo->flinfo, 0)) != -1 ||
		!VARATT_IS_EXTERNAL_ONDISK_HASH(v))
		elog(ERROR, "not a hash pointer");
	PG_RETURN_BYTEA_P(cstring_to_text_with_len((const char *) v,
											   VARSIZE_EXTERNAL(v)));
}

Datum
test_toast_pointer_value(PG_FUNCTION_ARGS)
{
	bytea	   *b = PG_GETARG_BYTEA_PP(0);
	varlena    *p = (varlena *) VARDATA_ANY(b);
	varlena    *copy;

	if (VARSIZE_ANY_EXHDR(b) != (VARHDRSZ_EXTERNAL + sizeof(varatt_external_hash)) ||
		!VARATT_IS_EXTERNAL_ONDISK_HASH(p))
		elog(ERROR, "not a hash pointer's bytes");
	copy = palloc((VARHDRSZ_EXTERNAL + sizeof(varatt_external_hash)));
	memcpy(copy, p, (VARHDRSZ_EXTERNAL + sizeof(varatt_external_hash)));
	PG_RETURN_BYTEA_P(detoast_attr(copy));
}

Datum
test_toast_pointer(PG_FUNCTION_ARGS)
{
	varlena    *v = (varlena *) DatumGetPointer(PG_GETARG_DATUM(0));
	varatt_external_hash p;
	TupleDesc	desc;
	Datum		values[4];
	bool		nulls[4] = {false, false, false, false};
	StringInfoData root;

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	if (get_typlen(get_fn_expr_argtype(fcinfo->flinfo, 0)) != -1 ||
		!VARATT_IS_EXTERNAL_ONDISK_HASH(v))
		PG_RETURN_NULL();
	VARATT_EXTERNAL_GET_POINTER(p, v);
	initStringInfo(&root);
	for (int i = 0; i < sizeof(p.va_hash); i++)
		appendStringInfo(&root, "%02x", p.va_hash[i]);
	values[0] = Int32GetDatum(p.va_extinfo >> VARLENA_EXTSIZE_BITS);
	values[1] = Int32GetDatum(p.va_rawsize);
	values[2] = Int32GetDatum(VARATT_EXTINFO_GET_EXTSIZE(p.va_extinfo));
	values[3] = CStringGetTextDatum(root.data);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc),
													  values, nulls)));
}

/*
 * Chunk node format: a node's items read back as written, at its level,
 * with its flags; the node's size is its header plus its items' spaces;
 * and the same items give the same bytes.
 */
void
chunkformat_items_round_trip(const ProllyImplInfo *impl)
{
	const ProllyChunkFormatRoutine *cf = (const ProllyChunkFormatRoutine *) impl;
	int			n = 300;
	ProllyKey  *keys = palloc(sizeof(ProllyKey) * n);
	ProllyValue *values = palloc(sizeof(ProllyValue) * n);
	uint8	   *flags = palloc(n);
	StringInfoData a;
	StringInfoData b;
	ProllyChunkNode node;
	Size		expect;

	for (int i = 0; i < n; i++)
	{
		/* keys and values of 0 to a few hundred bytes */
		char	   *k = psprintf("key %05d", i);
		StringInfoData v;

		initStringInfo(&v);
		for (int j = 0; j < (i * 37) % 400; j++)
			appendStringInfoChar(&v, (char) (i + j));
		keys[i].data = (const uint8 *) k;
		keys[i].len = strlen(k);
		values[i].data = (const uint8 *) v.data;
		values[i].len = v.len;
		flags[i] = i % 3 == 0 ? PROLLY_ITEM_VALUE_REF : 0;
	}
	initStringInfo(&a);
	initStringInfo(&b);
	appendStringInfoString(&b, "not the start");
	PROLLY_CALL(cf, encode, 7, n, keys, values, flags, &a);
	PROLLY_CALL(cf, encode, 7, n, keys, values, flags, &b);
	if (b.len - strlen("not the start") != a.len ||
		memcmp(a.data, b.data + strlen("not the start"), a.len) != 0)
		elog(ERROR, "the same items gave different bytes");

	expect = PROLLY_CALL(cf, header_size);
	for (int i = 0; i < n; i++)
		expect += PROLLY_CALL(cf, item_space, 7, &keys[i], &values[i]);
	if (expect != a.len)
		elog(ERROR, "a node of %d bytes, where its header and items take %zu",
			 a.len, expect);

	if (!PROLLY_CALL(cf, open, (const uint8 *) a.data, a.len, &node))
		elog(ERROR, "an encoded node doesn't open");
	if (node.level != 7 || node.nitems != n)
		elog(ERROR, "node level %u with %d items, written as 7 with %d",
			 node.level, node.nitems, n);
	for (int i = 0; i < n; i++)
	{
		ProllyKey	k;
		ProllyValue v;
		uint8		f;

		PROLLY_CALL(cf, item_get, &node, i, &k, &v, &f);
		if (k.len != keys[i].len || memcmp(k.data, keys[i].data, k.len) != 0 ||
			v.len != values[i].len || memcmp(v.data, values[i].data, v.len) != 0 ||
			f != flags[i])
			elog(ERROR, "item %d doesn't read back as written", i);
	}

	/* an empty leaf too */
	resetStringInfo(&a);
	PROLLY_CALL(cf, encode, 0, 0, keys, values, flags, &a);
	if (!PROLLY_CALL(cf, open, (const uint8 *) a.data, a.len, &node) ||
		node.nitems != 0 || node.level != 0)
		elog(ERROR, "an empty leaf doesn't read back");
}

/* Bytes that aren't a whole node of the format don't open as one. */
void
chunkformat_rejects_other_bytes(const ProllyImplInfo *impl)
{
	const ProllyChunkFormatRoutine *cf = (const ProllyChunkFormatRoutine *) impl;
	ProllyKey	key = {(const uint8 *) "k", 1};
	ProllyValue value = {(const uint8 *) "a value", 7};
	StringInfoData a;
	ProllyChunkNode node;

	initStringInfo(&a);
	PROLLY_CALL(cf, encode, 0, 1, &key, &value, NULL, &a);
	for (Size cut = 0; cut < (Size) a.len; cut++)
	{
		/* a cut short node whose items no longer fit opens as none */
		if (cut < PROLLY_CALL(cf, header_size) + 4 &&
			PROLLY_CALL(cf, open, (const uint8 *) a.data, cut, &node))
			elog(ERROR, "%zu of a node's %d bytes open as a node", cut, a.len);
	}
	if (PROLLY_CALL(cf, open, (const uint8 *) "random bytes, not a node", 24, &node))
		elog(ERROR, "random bytes open as a node");
}

/* Table files, read from memory. */
typedef struct MemFile
{
	const char *data;
	Size		len;
} MemFile;

/* a file in memory mapped: its bytes themselves */
static const char *
memfile_map(void *arg, uint64 offset, Size len)
{
	MemFile    *f = arg;

	if (offset > f->len || len > f->len - offset)
		return NULL;
	return f->data + offset;
}

static bool
memfile_read(void *arg, uint64 offset, char *buf, Size len)
{
	MemFile    *f = arg;

	if (offset > f->len || len > f->len - offset)
		return false;
	memcpy(buf, f->data + offset, len);
	return true;
}

/* chunk i of a test file: some repetitive (compressible), some not */
static void
tablefile_chunk(int i, StringInfo out, ProllyHash *hash)
{
	const ProllyHashRoutine *h = prolly_content_hash();
	ProllyHashCtx *ctx;
	uint32		x = 2166136261u ^ (uint32) i;

	resetStringInfo(out);
	for (int j = 0; j < (i * 131) % 9000; j++)
	{
		if (i % 2 == 0)
			appendStringInfoChar(out, (char) ('a' + j % 3));
		else
		{
			x = x * 16777619u + 7;
			appendStringInfoChar(out, (char) (x >> 24));
		}
	}
	ctx = PROLLY_CALL(h, begin, CurrentMemoryContext);
	PROLLY_CALL(h, update, ctx, (const uint8 *) out->data, out->len);
	PROLLY_CALL(h, finish, ctx, hash);
}

static void
tablefile_build(const ProllyTableFileRoutine *tf, int n, StringInfo file)
{
	ProllyTableFileWriter *w = PROLLY_CALL(tf, write_begin, CurrentMemoryContext);
	StringInfoData chunk;
	StringInfoData part;

	initStringInfo(&chunk);
	initStringInfo(&part);
	for (int i = 0; i < n; i++)
	{
		ProllyHash	hash;

		tablefile_chunk(i, &chunk, &hash);
		PROLLY_CALL(tf, write_add, w, &hash, chunk.data, chunk.len, &part);
		/* the caller may write each part out and start again */
		if (i % 7 == 0)
		{
			appendBinaryStringInfo(file, part.data, part.len);
			resetStringInfo(&part);
		}
	}
	PROLLY_CALL(tf, write_finish, w, &part);
	appendBinaryStringInfo(file, part.data, part.len);
}

static int
hash_order(const ProllyHash *a, const ProllyHash *b)
{
	int			r = memcmp(a->bytes, b->bytes, Min(a->len, b->len));

	return r != 0 ? r : (a->len > b->len) - (a->len < b->len);
}

/*
 * Table file format: every chunk added is found and decodes to its bytes;
 * the index lists them in hash order; a chunk not added isn't found; and
 * the same chunks in the same order give the same bytes.
 */
void
tablefile_round_trip(const ProllyImplInfo *impl)
{
	const ProllyTableFileRoutine *tf = (const ProllyTableFileRoutine *) impl;
	int			n = 200;
	StringInfoData a;
	StringInfoData b;
	StringInfoData chunk;
	StringInfoData out;
	MemFile		mf;
	ProllyTableFile *f;
	ProllyHash	prev = {0};

	initStringInfo(&a);
	initStringInfo(&b);
	initStringInfo(&chunk);
	initStringInfo(&out);
	tablefile_build(tf, n, &a);
	tablefile_build(tf, n, &b);
	if (a.len != b.len || memcmp(a.data, b.data, a.len) != 0)
		elog(ERROR, "the same chunks gave different table files");

	mf.data = a.data;
	mf.len = a.len;
	/* (mapped, as a chunk store's own files are; read, as a remote's) */
	f = PROLLY_CALL(tf, open, memfile_read, memfile_map, &mf, a.len,
					CurrentMemoryContext);
	if (f == NULL)
		elog(ERROR, "a table file written doesn't open");
	if (PROLLY_CALL(tf, count, f) != n)
		elog(ERROR, "a table file of %d chunks lists %d", n,
			 PROLLY_CALL(tf, count, f));
	for (int i = 0; i < n; i++)
	{
		ProllyHash	hash;
		uint64		offset;
		uint32		len;

		tablefile_chunk(i, &chunk, &hash);
		if (!PROLLY_CALL(tf, find, f, &hash, &offset, &len))
			elog(ERROR, "chunk %d isn't found", i);
		if (offset > (uint64) a.len || len > a.len - offset)
			elog(ERROR, "chunk %d is past the file's end", i);
		resetStringInfo(&out);
		PROLLY_CALL(tf, decode, f, a.data + offset, len, &out);
		if (out.len != chunk.len || memcmp(out.data, chunk.data, out.len) != 0)
			elog(ERROR, "chunk %d doesn't decode to its bytes", i);
	}
	for (int i = 0; i < n; i++)
	{
		ProllyHash	hash;
		uint64		offset;
		uint32		len;

		PROLLY_CALL(tf, entry, f, i, &hash, &offset, &len);
		if (i > 0 && hash_order(&prev, &hash) >= 0)
			elog(ERROR, "entries %d and %d aren't in hash order", i - 1, i);
		prev = hash;
	}
	{
		ProllyHash	hash;
		uint64		offset;
		uint32		len;

		tablefile_chunk(n, &chunk, &hash);
		if (PROLLY_CALL(tf, find, f, &hash, &offset, &len))
			elog(ERROR, "a chunk not added is found");
	}
	PROLLY_CALL(tf, close, f);

	/* and a file of no chunks, where nothing is found */
	resetStringInfo(&a);
	tablefile_build(tf, 0, &a);
	mf.data = a.data;
	mf.len = a.len;
	f = PROLLY_CALL(tf, open, memfile_read, NULL, &mf, a.len, CurrentMemoryContext);
	if (f == NULL || PROLLY_CALL(tf, count, f) != 0)
		elog(ERROR, "an empty table file doesn't read back");
	{
		ProllyHash	hash;
		uint64		offset;
		uint32		len;

		tablefile_chunk(0, &chunk, &hash);
		if (PROLLY_CALL(tf, find, f, &hash, &offset, &len) ||
			PROLLY_CALL(tf, locate, f, &hash) != -1)
			elog(ERROR, "a chunk is found in an empty table file");
	}
	PROLLY_CALL(tf, close, f);

	/* and of one chunk; a hash of another length isn't found in it */
	resetStringInfo(&a);
	tablefile_build(tf, 1, &a);
	mf.data = a.data;
	mf.len = a.len;
	f = PROLLY_CALL(tf, open, memfile_read, NULL, &mf, a.len, CurrentMemoryContext);
	if (f == NULL || PROLLY_CALL(tf, count, f) != 1)
		elog(ERROR, "a table file of one chunk doesn't read back");
	{
		ProllyHash	hash;
		uint64		offset;
		uint32		len;

		tablefile_chunk(0, &chunk, &hash);
		if (!PROLLY_CALL(tf, find, f, &hash, &offset, &len) ||
			PROLLY_CALL(tf, locate, f, &hash) != 0)
			elog(ERROR, "the chunk of a one-chunk table file isn't found");
		resetStringInfo(&out);
		PROLLY_CALL(tf, decode, f, a.data + offset, len, &out);
		if (out.len != chunk.len || memcmp(out.data, chunk.data, out.len) != 0)
			elog(ERROR, "the chunk of a one-chunk table file doesn't decode");
		hash.len--;
		if (PROLLY_CALL(tf, find, f, &hash, &offset, &len))
			elog(ERROR, "a hash of another length is found");
	}
	PROLLY_CALL(tf, close, f);
}

/*
 * A 20-byte hash for shared_prefixes: group g's first 8 bytes (all zero,
 * all 0xff, or one in between), then member m in its last 12.
 */
static void
prefix_hash(int g, int m, ProllyHash *h)
{
	memset(h, 0, sizeof(*h));
	h->len = 20;
	memset(h->bytes, g == 0 ? 0x00 : g == 1 ? 0xff : 0x40 + g, 8);
	h->bytes[18] = (uint8) (m >> 8);
	h->bytes[19] = (uint8) m;
}

/*
 * Chunks whose hashes share their first bytes, at the ends of the hash
 * space and between: every one is found, and hashes between them (or
 * past them) that weren't added aren't.
 */
void
tablefile_shared_prefixes(const ProllyImplInfo *impl)
{
	const ProllyTableFileRoutine *tf = (const ProllyTableFileRoutine *) impl;
	ProllyTableFileWriter *w = PROLLY_CALL(tf, write_begin, CurrentMemoryContext);
	StringInfoData file;
	MemFile		mf;
	ProllyTableFile *f;
	int			ngroups = 5;
	int			nmembers = 40;

	initStringInfo(&file);
	/* (added out of hash order; members 2, 4, 6, ... only) */
	for (int m = nmembers; m > 0; m--)
		for (int g = 0; g < ngroups; g++)
		{
			ProllyHash	h;
			char		data[16];

			prefix_hash(g, m * 2, &h);
			snprintf(data, sizeof(data), "%d.%d", g, m * 2);
			PROLLY_CALL(tf, write_add, w, &h, data, strlen(data), &file);
		}
	PROLLY_CALL(tf, write_finish, w, &file);
	mf.data = file.data;
	mf.len = file.len;
	f = PROLLY_CALL(tf, open, memfile_read, NULL, &mf, file.len, CurrentMemoryContext);
	if (f == NULL)
		elog(ERROR, "a table file of shared prefixes doesn't open");
	for (int g = 0; g < ngroups; g++)
		for (int m = 0; m <= nmembers * 2 + 1; m++)
		{
			ProllyHash	h;
			uint64		offset;
			uint32		len;
			bool		added = m > 0 && m <= nmembers * 2 && m % 2 == 0;
			StringInfoData out;
			char		data[16];

			prefix_hash(g, m, &h);
			if (PROLLY_CALL(tf, find, f, &h, &offset, &len) != added)
				elog(ERROR, "chunk %d.%d is %sfound", g, m, added ? "not " : "");
			if (!added)
				continue;
			initStringInfo(&out);
			PROLLY_CALL(tf, decode, f, file.data + offset, len, &out);
			snprintf(data, sizeof(data), "%d.%d", g, m);
			if (strcmp(out.data, data) != 0)
				elog(ERROR, "chunk %d.%d reads as \"%s\"", g, m, out.data);
			pfree(out.data);
		}
	PROLLY_CALL(tf, close, f);
}

/*
 * Bytes that aren't a whole table file don't open as one, and a chunk's
 * bytes changed don't decode.
 */
void
tablefile_rejects_other_bytes(const ProllyImplInfo *impl)
{
	const ProllyTableFileRoutine *tf = (const ProllyTableFileRoutine *) impl;
	StringInfoData a;
	MemFile		mf;
	ProllyTableFile *f;
	ProllyHash	hash;
	uint64		offset;
	uint32		len;
	StringInfoData out;
	char	   *copy;
	volatile bool failed = false;

	initStringInfo(&a);
	initStringInfo(&out);
	tablefile_build(tf, 20, &a);
	for (Size cut = 0; cut < (Size) a.len; cut += 1 + cut / 8)
	{
		mf.data = a.data;
		mf.len = cut;
		if (PROLLY_CALL(tf, open, memfile_read, NULL, &mf, cut, CurrentMemoryContext))
			elog(ERROR, "%zu of a table file's %d bytes open as one", cut, a.len);
	}
	/* a byte of the index changed */
	f = NULL;
	mf.data = a.data;
	mf.len = a.len;
	f = PROLLY_CALL(tf, open, memfile_read, NULL, &mf, a.len, CurrentMemoryContext);
	PROLLY_CALL(tf, entry, f, 0, &hash, &offset, &len);
	copy = palloc(a.len);
	memcpy(copy, a.data, a.len);
	copy[a.len - 40] ^= 1;
	mf.data = copy;
	if (PROLLY_CALL(tf, open, memfile_read, NULL, &mf, a.len, CurrentMemoryContext))
		elog(ERROR, "a table file with its index changed opens");

	/* a byte of a chunk changed */
	memcpy(copy, a.data, a.len);
	copy[offset + len / 2] ^= 1;
	PG_TRY();
	{
		PROLLY_CALL(tf, decode, f, copy + offset, len, &out);
	}
	PG_CATCH();
	{
		FlushErrorState();
		failed = true;
	}
	PG_END_TRY();
	if (!failed)
		elog(ERROR, "a changed chunk decodes");
	PROLLY_CALL(tf, close, f);
}

/*
 * The bytes the "basic" table file format keeps in memory for each chunk
 * of an open file of n chunks named by 20-byte hashes: its index read, or
 * (mapped) used where the file has it.
 */
PG_FUNCTION_INFO_V1(test_tablefile_bytes_per_chunk);
Datum
test_tablefile_bytes_per_chunk(PG_FUNCTION_ARGS)
{
	int32		n = PG_GETARG_INT32(0);
	bool		mapped = PG_GETARG_BOOL(1);
	const ProllyTableFileRoutine *tf = prolly_tablefile_lookup("basic");
	ProllyTableFileWriter *w = PROLLY_CALL(tf, write_begin, CurrentMemoryContext);
	MemoryContext cxt;
	StringInfoData file;
	MemFile		mf;
	ProllyTableFile *f;
	Size		used;

	initStringInfo(&file);
	for (int i = 0; i < n; i++)
	{
		ProllyHash	h;
		uint32		x = (uint32) i * 2654435761u;

		memset(&h, 0, sizeof(h));
		h.len = 20;
		for (int j = 0; j < 20; j++, x = x * 1103515245u + 12345)
			h.bytes[j] = (uint8) (x >> 24);
		PROLLY_CALL(tf, write_add, w, &h, "c", 1, &file);
	}
	PROLLY_CALL(tf, write_finish, w, &file);
	mf.data = file.data;
	mf.len = file.len;
	cxt = AllocSetContextCreate(CurrentMemoryContext, "table file",
								ALLOCSET_DEFAULT_SIZES);
	f = PROLLY_CALL(tf, open, memfile_read, mapped ? memfile_map : NULL, &mf,
					file.len, cxt);
	if (f == NULL)
		elog(ERROR, "the table file doesn't open");
	used = MemoryContextMemAllocated(cxt, true);
	PROLLY_CALL(tf, close, f);
	MemoryContextDelete(cxt);
	PG_RETURN_FLOAT8((double) used / n);
}

/* a little-endian uint32 of a table file test's, read or written */
static uint32
le32_at(const char *p)
{
	return (uint32) (uint8) p[0] | (uint32) (uint8) p[1] << 8 |
		(uint32) (uint8) p[2] << 16 | (uint32) (uint8) p[3] << 24;
}

static void
le32_put(char *p, uint32 v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (char) (v >> (8 * i));
}

static uint64
le64_at(const char *p)
{
	return le32_at(p) | (uint64) le32_at(p + 4) << 32;
}

static void
le64_put(char *p, uint64 v)
{
	le32_put(p, (uint32) v);
	le32_put(p + 4, (uint32) (v >> 32));
}

/*
 * Whether a "basic" table file, its index changed by change and its CRC
 * made again (so only the index's own checks can refuse it), opens.  The
 * index: n entries of an 8-byte prefix and a uint32 ordinal, then n uint64
 * offsets, then the suffixes, then the filter; the footer: the index's
 * offset (uint64),
 * n (uint32), the hash's length, three zero bytes, the index's CRC-32C,
 * the version (uint32), the magic.
 */
static bool
basic_opens_changed(const StringInfoData *file, void (*change) (char *index, uint32 n,
																 char *footer))
{
	const ProllyTableFileRoutine *tf = prolly_tablefile_lookup("basic");
	char	   *copy = palloc(file->len);
	char	   *footer = copy + file->len - 32;
	uint64		index_offset;
	uint32		n;
	pg_crc32c	crc;
	MemFile		mf;
	ProllyTableFile *f;

	memcpy(copy, file->data, file->len);
	index_offset = le64_at(footer);
	n = le32_at(footer + 8);
	change(copy + index_offset, n, footer);
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, copy + index_offset, (footer - copy) - index_offset);
	FIN_CRC32C(crc);
	le32_put(footer + 16, crc);
	mf.data = copy;
	mf.len = file->len;
	f = PROLLY_CALL(tf, open, memfile_read, NULL, &mf, file->len,
					CurrentMemoryContext);
	if (f != NULL)
		PROLLY_CALL(tf, close, f);
	pfree(copy);
	return f != NULL;
}

static void
change_nothing(char *index, uint32 n, char *footer)
{
}

static void
change_repeat_ordinal(char *index, uint32 n, char *footer)
{
	memcpy(index + 8, index + 12 + 8, 4);
}

static void
change_ordinal_past(char *index, uint32 n, char *footer)
{
	le32_put(index + 8, n);
}

static void
change_short_length(char *index, uint32 n, char *footer)
{
	char	   *offsets = index + n * 12;

	le64_put(offsets + 8, le64_at(offsets) + 1);
}

static void
change_lengths_sum(char *index, uint32 n, char *footer)
{
	le64_put(index + n * 12, 1);
}

static void
change_hash_order(char *index, uint32 n, char *footer)
{
	char		entry[12];

	memcpy(entry, index, 12);
	memcpy(index, index + 12, 12);
	memcpy(index + 12, entry, 12);
}

static void
change_version(char *index, uint32 n, char *footer)
{
	le32_put(footer + 20, 1);
}

/*
 * The "basic" table file format's own checks of an index: each damage
 * below, its CRC made again, is refused at open; and a file of another
 * version is told from a damaged one.
 */
PG_FUNCTION_INFO_V1(test_tablefile_basic_index);
Datum
test_tablefile_basic_index(PG_FUNCTION_ARGS)
{
	const ProllyTableFileRoutine *tf = prolly_tablefile_lookup("basic");
	static const struct
	{
		const char *what;
		void		(*change) (char *index, uint32 n, char *footer);
	}			damages[] = {
		{"an ordinal repeated", change_repeat_ordinal},
		{"an ordinal past the chunks", change_ordinal_past},
		{"a length shorter than a chunk's framing", change_short_length},
		{"lengths not adding up to the index's offset", change_lengths_sum},
		{"hashes out of order", change_hash_order},
		{"another version", change_version},
	};
	StringInfoData file;
	MemFile		mf;

	initStringInfo(&file);
	tablefile_build(tf, 20, &file);
	if (!basic_opens_changed(&file, change_nothing))
		elog(ERROR, "a table file with its CRC made again doesn't open");
	for (int i = 0; i < lengthof(damages); i++)
		if (basic_opens_changed(&file, damages[i].change))
			elog(ERROR, "a table file with %s opens", damages[i].what);

	mf.data = file.data;
	mf.len = file.len;
	if (tf->version(memfile_read, &mf, file.len) != tf->info.format_version)
		elog(ERROR, "a table file's version isn't its format's");
	file.data[file.len - 12] = 1;	/* (the version's low byte) */
	if (tf->version(memfile_read, &mf, file.len) != 1)
		elog(ERROR, "a table file of version 1 isn't told as that");
	file.data[file.len - 1] ^= 1;	/* (the magic) */
	if (tf->version(memfile_read, &mf, file.len) != 0)
		elog(ERROR, "bytes that aren't a table file have a version");
	PG_RETURN_BOOL(true);
}
