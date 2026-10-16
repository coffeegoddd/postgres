/*--------------------------------------------------------------------------
 *
 * foundation_checks.c
 *		Conformance checks for the foundation interfaces: node format,
 *		chunker, key encoder and value encoder.
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

#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/tupdesc_details.h"
#include "access/prolly_datum.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_am.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_index.h"
#include "catalog/namespace.h"
#include "catalog/pg_type_d.h"
#include "commands/defrem.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "nodes/value.h"
#include "test_prolly.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"

/* ------------------------------------------------------------------------
 * Node format
 * ------------------------------------------------------------------------
 */

#define TEST_PAGE_SIZE BLCKSZ

static Page
new_page(void)
{
	return (Page) palloc_aligned(TEST_PAGE_SIZE, PG_IO_ALIGN_SIZE, MCXT_ALLOC_ZERO);
}

/* Item i of a test sequence: keys ascend, value lengths vary. */
static void
test_item(int i, uint8 *keybuf, uint8 *valbuf, ProllyKey *key, ProllyValue *value)
{
	int			vlen = (i * 37) % 120;

	keybuf[0] = (uint8) (i >> 24);
	keybuf[1] = (uint8) (i >> 16);
	keybuf[2] = (uint8) (i >> 8);
	keybuf[3] = (uint8) i;
	for (int j = 0; j < vlen; j++)
		valbuf[j] = (uint8) (i + j);
	key->data = keybuf;
	key->len = 4;
	value->data = valbuf;
	value->len = vlen;
}

/* Block hint stored with item i in internal nodes. */
static BlockNumber
test_hint(int i)
{
	return (BlockNumber) (i * 2654435761u) % MaxBlockNumber;
}

static void
fill_page(const ProllyNodeFormatRoutine *nf, Page page, uint16 level,
		  int first, int n)
{
	uint8		keybuf[4];
	uint8		valbuf[128];
	ProllyKey	key;
	ProllyValue value;

	PROLLY_CALL(nf, page_init, page, TEST_PAGE_SIZE, level);
	for (int i = first; i < first + n; i++)
	{
		test_item(i, keybuf, valbuf, &key, &value);
		if (!PROLLY_CALL(nf, item_append, page, &key, &value, test_hint(i), 0))
			elog(ERROR, "item %d did not fit in an empty-enough page", i);
	}
}

void
nodeformat_items_round_trip(const ProllyImplInfo *impl)
{
	const ProllyNodeFormatRoutine *nf = (const ProllyNodeFormatRoutine *) impl;
	Page		page = new_page();
	const int	n = 40;

	for (uint16 level = 0; level < 2; level++)
	{
		fill_page(nf, page, level, 0, n);
		if (PROLLY_CALL(nf, item_count, page) != n)
			elog(ERROR, "item_count is %d, expected %d",
				 PROLLY_CALL(nf, item_count, page), n);

		for (int i = n - 1; i >= 0; i--)
		{
			uint8		keybuf[4];
			uint8		valbuf[128];
			ProllyKey	want_key,
						key;
			ProllyValue want_value,
						value;
			BlockNumber hint = PROLLY_CALL(nf, item_hint, page, i);

			test_item(i, keybuf, valbuf, &want_key, &want_value);
			PROLLY_CALL(nf, item_get, page, i, &key, &value, NULL);
			if (key.len != want_key.len ||
				memcmp(key.data, want_key.data, key.len) != 0)
				elog(ERROR, "level %u item %d has the wrong key", level, i);
			if (value.len != want_value.len ||
				memcmp(value.data, want_value.data, value.len) != 0)
				elog(ERROR, "level %u item %d has the wrong value", level, i);
			if (hint != (level == 0 ? InvalidBlockNumber : test_hint(i)))
				elog(ERROR, "level %u item %d has block hint %u", level, i, hint);
		}
	}
}

void
nodeformat_level_round_trip(const ProllyImplInfo *impl)
{
	const ProllyNodeFormatRoutine *nf = (const ProllyNodeFormatRoutine *) impl;
	Page		page = new_page();
	static const uint16 levels[] = {0, 1, 2, 300, PG_UINT16_MAX};

	for (int i = 0; i < lengthof(levels); i++)
	{
		PROLLY_CALL(nf, page_init, page, TEST_PAGE_SIZE, levels[i]);
		if (PROLLY_CALL(nf, page_level, page) != levels[i])
			elog(ERROR, "level %u read back as %u", levels[i],
				 PROLLY_CALL(nf, page_level, page));
		if (PROLLY_CALL(nf, item_count, page) != 0)
			elog(ERROR, "a new page at level %u is not empty", levels[i]);
	}
}

void
nodeformat_space_accounting(const ProllyImplInfo *impl)
{
	const ProllyNodeFormatRoutine *nf = (const ProllyNodeFormatRoutine *) impl;
	Page		page = new_page();
	bool		filled = false;

	PROLLY_CALL(nf, page_init, page, TEST_PAGE_SIZE, 1);
	for (int i = 0; i < TEST_PAGE_SIZE; i++)
	{
		uint8		keybuf[4];
		uint8		valbuf[128];
		ProllyKey	key;
		ProllyValue value;
		Size		need;
		Size		before;
		bool		ok;

		test_item(i, keybuf, valbuf, &key, &value);
		need = PROLLY_CALL(nf, item_space, 1, &key, &value);
		before = PROLLY_CALL(nf, free_space, page);
		ok = PROLLY_CALL(nf, item_append, page, &key, &value, test_hint(i), 0);

		if (ok != (need <= before))
			elog(ERROR, "item %d of %zu bytes %s with %zu bytes free",
				 i, need, ok ? "was appended" : "was refused", before);
		if (ok && PROLLY_CALL(nf, free_space, page) != before - need)
			elog(ERROR, "appending %zu bytes changed free space from %zu to %zu",
				 need, before, PROLLY_CALL(nf, free_space, page));
		if (!ok)
		{
			if (PROLLY_CALL(nf, free_space, page) != before)
				elog(ERROR, "a refused item changed free space");
			filled = true;
			break;
		}
	}
	if (!filled)
		elog(ERROR, "page never filled");
}

static void
canonical_copy(const ProllyNodeFormatRoutine *nf, Page page, StringInfo out)
{
	const uint8 *data;
	Size		len;

	PROLLY_CALL(nf, canonical_bytes, page, &data, &len);
	resetStringInfo(out);
	appendBinaryStringInfo(out, data, len);
}

void
nodeformat_canonical_bytes(const ProllyImplInfo *impl)
{
	const ProllyNodeFormatRoutine *nf = (const ProllyNodeFormatRoutine *) impl;
	Page		a = new_page();
	Page		b = new_page();
	StringInfoData ca,
				cb;
	char	   *special;
	Size		special_len;

	initStringInfo(&ca);
	initStringInfo(&cb);

	/* Same items, but b starts as garbage and has a scribbled special space. */
	fill_page(nf, a, 0, 0, 30);
	memset(b, 0x5a, TEST_PAGE_SIZE);
	fill_page(nf, b, 0, 0, 30);
	special = PROLLY_CALL(nf, special_space, b, &special_len);
	memset(special, 0xa5, special_len);

	canonical_copy(nf, a, &ca);
	canonical_copy(nf, b, &cb);
	if (ca.len != cb.len || memcmp(ca.data, cb.data, ca.len) != 0)
		elog(ERROR, "same items gave different canonical bytes");

	/* Block hints are not canonical: different hints, same bytes. */
	fill_page(nf, a, 1, 0, 30);
	canonical_copy(nf, a, &ca);
	PROLLY_CALL(nf, page_init, b, TEST_PAGE_SIZE, 1);
	for (int i = 0; i < 30; i++)
	{
		uint8		keybuf[4];
		uint8		valbuf[128];
		ProllyKey	key;
		ProllyValue value;

		test_item(i, keybuf, valbuf, &key, &value);
		PROLLY_CALL(nf, item_append, b, &key, &value, InvalidBlockNumber, 0);
	}
	canonical_copy(nf, b, &cb);
	if (ca.len != cb.len || memcmp(ca.data, cb.data, ca.len) != 0)
		elog(ERROR, "block hints changed the canonical bytes");
	fill_page(nf, a, 0, 0, 30);
	canonical_copy(nf, a, &ca);

	/* A different item, or a different level, must change them. */
	fill_page(nf, b, 0, 1, 30);
	canonical_copy(nf, b, &cb);
	if (ca.len == cb.len && memcmp(ca.data, cb.data, ca.len) == 0)
		elog(ERROR, "different items gave the same canonical bytes");

	fill_page(nf, b, 1, 0, 30);
	canonical_copy(nf, b, &cb);
	if (ca.len == cb.len && memcmp(ca.data, cb.data, ca.len) == 0)
		elog(ERROR, "different levels gave the same canonical bytes");
}

void
nodeformat_page_check(const ProllyImplInfo *impl)
{
	const ProllyNodeFormatRoutine *nf = (const ProllyNodeFormatRoutine *) impl;
	Page		page = new_page();
	Page		bad = new_page();
	char	   *detail = NULL;
	int			caught = 0;

	fill_page(nf, page, 0, 0, 30);
	if (!PROLLY_CALL(nf, page_check, page, &detail))
		elog(ERROR, "a valid page failed the check: %s", detail);

	/*
	 * Flip single bytes in the page header and the start of the content.  The
	 * check need not catch every flip (some land in item data), but it must
	 * catch flips in the structural fields.
	 */
	for (int off = 0; off < SizeOfPageHeaderData + 16; off++)
	{
		memcpy(bad, page, TEST_PAGE_SIZE);
		((uint8 *) bad)[off] ^= 0x40;
		if (!PROLLY_CALL(nf, page_check, bad, &detail))
			caught++;
	}

	/* pd_lower and pd_upper must be checked */
	memcpy(bad, page, TEST_PAGE_SIZE);
	((PageHeader) bad)->pd_lower += 1;
	if (PROLLY_CALL(nf, page_check, bad, &detail))
		elog(ERROR, "page_check missed a corrupted pd_lower");
	memcpy(bad, page, TEST_PAGE_SIZE);
	((PageHeader) bad)->pd_upper -= 2;
	if (PROLLY_CALL(nf, page_check, bad, &detail))
		elog(ERROR, "page_check missed a corrupted pd_upper");

	if (caught == 0)
		elog(ERROR, "page_check caught none of the corrupted bytes");
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

void
chunker_reset_equals_begin(const ProllyImplInfo *impl)
{
	const ProllyChunkerRoutine *ch = (const ProllyChunkerRoutine *) impl;
	ProllyChunkDecision *a = palloc(sizeof(ProllyChunkDecision) * CHUNK_ITEMS);
	ProllyChunkDecision *b = palloc(sizeof(ProllyChunkDecision) * CHUNK_ITEMS);
	ProllyChunker *used = PROLLY_CALL(ch, begin, 0, CHUNK_CAPACITY,
									  CurrentMemoryContext);
	ProllyChunker *fresh = PROLLY_CALL(ch, begin, 0, CHUNK_CAPACITY,
									   CurrentMemoryContext);

	/* Leave state behind (including, likely, a pending split), then reset. */
	chunk_run(ch, used, 5000, 777, 1000, a);
	PROLLY_CALL(ch, reset, used);

	chunk_run(ch, used, 0, CHUNK_ITEMS, 1000, a);
	chunk_run(ch, fresh, 0, CHUNK_ITEMS, 1000, b);
	for (int i = 0; i < CHUNK_ITEMS; i++)
	{
		if (a[i] != b[i])
			elog(ERROR, "item %d: decision %d after reset, %d after begin",
				 i, a[i], b[i]);
	}
	PROLLY_CALL(ch, end, used);
	PROLLY_CALL(ch, end, fresh);
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
	Oid			am;				/* whose default opclass; btree if invalid */
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
		Oid			am = OidIsValid(spec->am) ? spec->am : BTREE_AM_OID;
		Oid			opclass = GetDefaultOpClass(spec->type, am);
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

static void
check_key_round_trip(const ProllyKeyEncoderRoutine *ke, KeyCase *kc)
{
	ProllyKeyEncoder *enc;
	KeyRow	   *rows;
	int			nrows;

	key_case_init(kc);
	enc = PROLLY_CALL(ke, begin, &kc->desc, CurrentMemoryContext);
	rows = key_rows(kc, true, &nrows);
	for (int r = 0; r < nrows; r++)
	{
		StringInfoData k1,
					k2;
		ProllyKey	key;
		KeyRow		back;

		initStringInfo(&k1);
		initStringInfo(&k2);
		PROLLY_CALL(ke, encode, enc, rows[r].values, rows[r].isnull, &k1);
		key.data = (const uint8 *) k1.data;
		key.len = k1.len;
		if (!PROLLY_CALL(ke, decode, enc, &key, back.values, back.isnull))
			continue;			/* allowed: callers read the row instead */
		if (reference_compare(kc, &rows[r], &back) != 0)
			elog(ERROR, "%s decoded as %s", describe_row(kc, &rows[r]),
				 describe_row(kc, &back));
		for (int c = 0; c < kc->ncols; c++)
		{
			if (rows[r].isnull[c] != back.isnull[c])
				elog(ERROR, "%s: NULL in column %d did not round-trip",
					 describe_row(kc, &rows[r]), c + 1);
		}
		PROLLY_CALL(ke, encode, enc, back.values, back.isnull, &k2);
		if (k1.len != k2.len || memcmp(k1.data, k2.data, k1.len) != 0)
			elog(ERROR, "%s re-encoded differently", describe_row(kc, &rows[r]));
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
	Oid			prolly_am = get_index_am_oid("prolly_d1_index", false);
	KeyCase		cases[] = {
		{1, {{INT2OID}}},
		{1, {{INT4OID}}},
		{1, {{INT8OID}}},
		{1, {{BOOLOID}}},
		{1, {{TEXTOID, C_COLLATION_OID}}},
		{1, {{TEXTOID, non_c_collation()}}},
		{1, {{NUMERICOID}}},
		{1, {{INT4OID, InvalidOid, 0, prolly_am}}},
		{1, {{TEXTOID, C_COLLATION_OID, 0, prolly_am}}},
		{2, {{INT4OID}, {TEXTOID, C_COLLATION_OID}}},
		{2, {{TEXTOID, non_c_collation()}, {INT8OID}}},
		{2, {{NUMERICOID}, {BOOLOID}}},
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
			{1, {{NUMERICOID, InvalidOid, opts[o]}}},
			{2, {{INT8OID, InvalidOid, opts[o]}, {TEXTOID, C_COLLATION_OID, 0}}},
			{2, {{BOOLOID, InvalidOid, 0}, {INT2OID, InvalidOid, opts[o]}}},
		};

		for (int i = 0; i < lengthof(cases); i++)
			check_key_order(ke, &cases[i], true);
	}
}

/*
 * Encodings are self-delimiting: prefix_length() finds where the first k
 * columns end, even with other bytes after the key, and that prefix is
 * the encoding of the first k columns alone.
 */
static void
check_key_prefixes(const ProllyKeyEncoderRoutine *ke, KeyCase *kc)
{
	ProllyKeyEncoder *enc;
	KeyRow	   *rows;
	int			nrows;

	key_case_init(kc);
	enc = PROLLY_CALL(ke, begin, &kc->desc, CurrentMemoryContext);
	rows = key_rows(kc, true, &nrows);
	for (int r = 0; r < nrows; r++)
	{
		StringInfoData full;
		ProllyKey	key;
		int			bare;

		initStringInfo(&full);
		PROLLY_CALL(ke, encode, enc, rows[r].values, rows[r].isnull, &full);
		bare = full.len;
		appendStringInfoString(&full, "trailing bytes");
		key.data = (const uint8 *) full.data;
		key.len = full.len;
		if (PROLLY_CALL(ke, prefix_length, enc, &key, kc->ncols) != bare)
			elog(ERROR, "%s: prefix_length of every column is not the key's length",
				 describe_row(kc, &rows[r]));
		for (int k = 1; k < kc->ncols; k++)
		{
			ProllyKeyDesc d = kc->desc;
			ProllyKeyEncoder *pe;
			StringInfoData part;

			d.nkeys = k;
			pe = PROLLY_CALL(ke, begin, &d, CurrentMemoryContext);
			initStringInfo(&part);
			PROLLY_CALL(ke, encode, pe, rows[r].values, rows[r].isnull, &part);
			if (PROLLY_CALL(ke, prefix_length, enc, &key, k) != part.len ||
				memcmp(part.data, full.data, part.len) != 0)
				elog(ERROR, "%s: its first %d columns are not their own encoding",
					 describe_row(kc, &rows[r]), k);
			PROLLY_CALL(ke, end, pe);
		}
	}
	PROLLY_CALL(ke, end, enc);
}

void
keyenc_prefix_lengths(const ProllyImplInfo *impl)
{
	const ProllyKeyEncoderRoutine *ke = (const ProllyKeyEncoderRoutine *) impl;
	KeyCase		cases[] = {
		{1, {{INT4OID}}},
		{2, {{INT4OID, InvalidOid, INDOPTION_DESC}, {TEXTOID, C_COLLATION_OID}}},
		{2, {{TEXTOID, non_c_collation(), INDOPTION_NULLS_FIRST}, {INT8OID}}},
		{2, {{NUMERICOID}, {BOOLOID, InvalidOid, INDOPTION_DESC}}},
		{2, {{INT4ARRAYOID}, {TEXTOID, C_COLLATION_OID, INDOPTION_DESC}}},
	};

	for (int i = 0; i < lengthof(cases); i++)
		check_key_prefixes(ke, &cases[i]);
}

void
keyenc_decode_round_trip(const ProllyImplInfo *impl)
{
	const ProllyKeyEncoderRoutine *ke = (const ProllyKeyEncoderRoutine *) impl;
	KeyCase		cases[] = {
		{1, {{INT2OID, InvalidOid, INDOPTION_DESC}}},
		{1, {{INT4OID}}},
		{1, {{INT8OID, InvalidOid, INDOPTION_DESC | INDOPTION_NULLS_FIRST}}},
		{1, {{BOOLOID}}},
		{1, {{TEXTOID, C_COLLATION_OID, INDOPTION_DESC}}},
		{1, {{TEXTOID, non_c_collation()}}},
		{1, {{NUMERICOID, InvalidOid, INDOPTION_DESC}}},
		{2, {{INT4OID}, {TEXTOID, C_COLLATION_OID}}},
		{1, {{INT4ARRAYOID, InvalidOid, INDOPTION_DESC}}},
	};

	for (int i = 0; i < lengthof(cases); i++)
		check_key_round_trip(ke, &cases[i]);
}

/*
 * equal_bytes(): if true, keys that compare equal are the same bytes.
 * Integer and C-collated text keys must say so (row locks name rows by
 * their key bytes); numeric keys can't (1.0 and 1.00 are equal).
 */
static bool
check_equal_bytes(const ProllyKeyEncoderRoutine *ke, KeyCase *kc)
{
	ProllyKeyEncoder *enc;
	KeyRow	   *rows;
	int			nrows;
	StringInfoData *keys;
	bool		says;

	key_case_init(kc);
	enc = PROLLY_CALL(ke, begin, &kc->desc, CurrentMemoryContext);
	says = PROLLY_CALL(ke, equal_bytes, enc);
	rows = key_rows(kc, true, &nrows);
	keys = palloc(sizeof(StringInfoData) * nrows);
	for (int r = 0; r < nrows; r++)
	{
		initStringInfo(&keys[r]);
		PROLLY_CALL(ke, encode, enc, rows[r].values, rows[r].isnull, &keys[r]);
	}
	for (int a = 0; a < nrows && says; a++)
	{
		for (int b = 0; b < nrows; b++)
		{
			ProllyKey	ka = {(const uint8 *) keys[a].data, keys[a].len};
			ProllyKey	kb = {(const uint8 *) keys[b].data, keys[b].len};

			if (PROLLY_CALL(ke, compare, enc, &ka, &kb) == 0 &&
				(ka.len != kb.len || memcmp(ka.data, kb.data, ka.len) != 0))
				elog(ERROR, "%s and %s compare equal as different bytes, but equal_bytes says they can't",
					 describe_row(kc, &rows[a]), describe_row(kc, &rows[b]));
		}
	}
	PROLLY_CALL(ke, end, enc);
	return says;
}

void
keyenc_equal_bytes(const ProllyImplInfo *impl)
{
	const ProllyKeyEncoderRoutine *ke = (const ProllyKeyEncoderRoutine *) impl;
	KeyCase		ints = {2, {{INT4OID}, {INT8OID, InvalidOid, INDOPTION_DESC}}};
	KeyCase		text = {1, {{TEXTOID, C_COLLATION_OID}}};
	KeyCase		other = {1, {{TEXTOID, non_c_collation()}}};
	KeyCase		num = {1, {{NUMERICOID}}};
	ProllyKeyEncoder *enc;
	Datum		one[2];
	bool		isnull[1] = {false};
	StringInfoData a;
	StringInfoData b;
	ProllyKey	ka;
	ProllyKey	kb;

	if (!check_equal_bytes(ke, &ints))
		elog(ERROR, "integer keys must be named by their bytes");
	if (!check_equal_bytes(ke, &text))
		elog(ERROR, "C-collated text keys must be named by their bytes");
	(void) check_equal_bytes(ke, &other);

	/* 1.0 and 1.00: equal, encoded differently. */
	key_case_init(&num);
	enc = PROLLY_CALL(ke, begin, &num.desc, CurrentMemoryContext);
	one[0] = DirectFunctionCall3(numeric_in, CStringGetDatum("1.0"),
								 ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
	one[1] = DirectFunctionCall3(numeric_in, CStringGetDatum("1.00"),
								 ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
	initStringInfo(&a);
	initStringInfo(&b);
	PROLLY_CALL(ke, encode, enc, &one[0], isnull, &a);
	PROLLY_CALL(ke, encode, enc, &one[1], isnull, &b);
	ka.data = (const uint8 *) a.data;
	ka.len = a.len;
	kb.data = (const uint8 *) b.data;
	kb.len = b.len;
	if (PROLLY_CALL(ke, compare, enc, &ka, &kb) == 0 &&
		(a.len != b.len || memcmp(a.data, b.data, a.len) != 0) &&
		PROLLY_CALL(ke, equal_bytes, enc))
		elog(ERROR, "numeric 1.0 and 1.00 are equal keys with different bytes, but equal_bytes says they can't be");
	PROLLY_CALL(ke, end, enc);
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
