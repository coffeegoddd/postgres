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
#include "catalog/pg_am_d.h"
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
#include "utils/lsyscache.h"

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
		if (!PROLLY_CALL(nf, item_append, page, &key, &value))
			elog(ERROR, "item %d did not fit in an empty-enough page", i);
	}
}

void
nodeformat_items_round_trip(const ProllyImplInfo *impl)
{
	const ProllyNodeFormatRoutine *nf = (const ProllyNodeFormatRoutine *) impl;
	Page		page = new_page();
	const int	n = 40;

	fill_page(nf, page, 0, 0, n);
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

		test_item(i, keybuf, valbuf, &want_key, &want_value);
		PROLLY_CALL(nf, item_get, page, i, &key, &value);
		if (key.len != want_key.len || memcmp(key.data, want_key.data, key.len) != 0)
			elog(ERROR, "item %d has the wrong key", i);
		if (value.len != want_value.len ||
			memcmp(value.data, want_value.data, value.len) != 0)
			elog(ERROR, "item %d has the wrong value", i);
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

	PROLLY_CALL(nf, page_init, page, TEST_PAGE_SIZE, 0);
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
		need = PROLLY_CALL(nf, item_space, &key, &value);
		before = PROLLY_CALL(nf, free_space, page);
		ok = PROLLY_CALL(nf, item_append, page, &key, &value);

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
		fmgr_info(get_opfamily_proc(opfamily, spec->type, spec->type, BTORDER_PROC),
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
	Oid			prolly_am = get_index_am_oid("prolly", false);
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
	};

	for (int i = 0; i < lengthof(cases); i++)
		check_key_round_trip(ke, &cases[i]);
}

/* ------------------------------------------------------------------------
 * Value encoder
 * ------------------------------------------------------------------------
 */

#define VAL_COLS 5

static TupleDesc
value_tupdesc(void)
{
	TupleDesc	desc = CreateTemplateTupleDesc(VAL_COLS);

	TupleDescInitEntry(desc, 1, "i4", INT4OID, -1, 0);
	TupleDescInitEntry(desc, 2, "t", TEXTOID, -1, 0);
	TupleDescInitEntry(desc, 3, "i8", INT8OID, -1, 0);
	TupleDescInitEntry(desc, 4, "b", BOOLOID, -1, 0);
	TupleDescInitEntry(desc, 5, "n", NUMERICOID, -1, 0);
	TupleDescFinalize(desc);
	return desc;
}

static void
store_row(TupleTableSlot *slot, int i)
{
	char	   *longtext = palloc(501);

	memset(longtext, 'x', 500);
	longtext[500] = '\0';

	ExecClearTuple(slot);
	slot->tts_values[0] = Int32GetDatum(i * 1000 - 7);
	slot->tts_values[1] = CStringGetTextDatum(i % 3 == 0 ? "" : i % 3 == 1 ? "abc" : longtext);
	slot->tts_values[2] = Int64GetDatum((int64) i << 40);
	slot->tts_values[3] = BoolGetDatum(i % 2 == 0);
	slot->tts_values[4] = numeric_datum(i % 2 ? "3.14159" : "-1e30");
	for (int c = 0; c < VAL_COLS; c++)
		slot->tts_isnull[c] = (i >> c) & 1 && i >= 8;
	ExecStoreVirtualTuple(slot);
}

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

void
valenc_round_trip(const ProllyImplInfo *impl)
{
	const ProllyValueEncoderRoutine *ve = (const ProllyValueEncoderRoutine *) impl;
	TupleDesc	desc = value_tupdesc();
	ProllyValueEncoder *enc = PROLLY_CALL(ve, begin, desc, CurrentMemoryContext);
	TupleTableSlot *src = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	const TupleTableSlotOps *const kinds[] = {
		&TTSOpsVirtual, &TTSOpsMinimalTuple, &TTSOpsHeapTuple,
	};

	for (int i = 0; i < 32; i++)
	{
		StringInfoData buf;
		ProllyValue value;

		store_row(src, i);
		initStringInfo(&buf);
		PROLLY_CALL(ve, encode, enc, src, &buf);
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
 * Reference encoding of row 1 (see store_row), and of row 9 (with NULLs).
 * These bytes are the canonical format: an implementation whose encoding
 * differs between platforms fails here on all but one of them.  The
 * expected bytes were recorded on x86-64 Linux, the platform of the first
 * implementation ("heaptuple", which is platform-dependent by decision;
 * open question 12).
 */
#define REFERENCE_ROW1_HEX "2a000000000000000000050002001800e103000009616263000000000001000000138082030087052823"
#define REFERENCE_ROW9_HEX "2900000000000000000005000300181603000000000000000000000000090000138082030087052823"

static const char *const reference_hex[] = {
	REFERENCE_ROW1_HEX,
	REFERENCE_ROW9_HEX,
};

void
valenc_platform_independent(const ProllyImplInfo *impl)
{
	const ProllyValueEncoderRoutine *ve = (const ProllyValueEncoderRoutine *) impl;
	TupleDesc	desc = value_tupdesc();
	ProllyValueEncoder *enc = PROLLY_CALL(ve, begin, desc, CurrentMemoryContext);
	TupleTableSlot *virt = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	TupleTableSlot *heap = MakeSingleTupleTableSlot(desc, &TTSOpsHeapTuple);
	static const int rows[] = {1, 9};

	for (int r = 0; r < lengthof(rows); r++)
	{
		StringInfoData a,
					b,
					hex;

		/* The same row from two kinds of slot must encode identically. */
		store_row(virt, rows[r]);
		ExecCopySlot(heap, virt);
		initStringInfo(&a);
		initStringInfo(&b);
		PROLLY_CALL(ve, encode, enc, virt, &a);
		PROLLY_CALL(ve, encode, enc, heap, &b);
		if (a.len != b.len || memcmp(a.data, b.data, a.len) != 0)
			elog(ERROR, "row %d encodes differently from a heap tuple slot",
				 rows[r]);

		initStringInfo(&hex);
		for (int i = 0; i < a.len; i++)
			appendStringInfo(&hex, "%02x", (uint8) a.data[i]);
		if (strcmp(hex.data, reference_hex[r]) != 0)
			elog(ERROR, "row %d encodes as %s, the reference encoding is %s",
				 rows[r], hex.data, reference_hex[r]);
	}
	ExecDropSingleTupleTableSlot(virt);
	ExecDropSingleTupleTableSlot(heap);
	PROLLY_CALL(ve, end, enc);
}
