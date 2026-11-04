/*-------------------------------------------------------------------------
 *
 * unit_ws_row.c
 *	  Unit tests of the working-set model's rows: the bytes a row is, what
 *	  leaves them unchanged, malformed bytes refused, and COPY text.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/test/modules/test_prolly/unit_ws_row.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "engine_test_util.h"
#include "prolly/ws/ws_row.h"
#include "test_prolly.h"

static WsField
text_field(const char *s)
{
	WsField		f = {(const uint8 *) s, strlen(s), false};

	return f;
}

static const WsField null_field = {NULL, 0, true};

/*
 * Fields set as if each had a value: a NULL, or a value, after decoding is
 * decoding's doing.
 */
static void
scribble(WsField *f, int n)
{
	for (int i = 0; i < n; i++)
	{
		f[i].p = (const uint8 *) "scribbled";
		f[i].len = 9;
		f[i].null = false;
	}
}

static bool
bytes_are(const Buf *b, const char *want, size_t n)
{
	return b->len == n && (n == 0 || memcmp(b->data, want, n) == 0);
}

static bool
field_is(const WsField *f, const char *want)
{
	return !f->null && f->len == strlen(want) && memcmp(f->p, want, f->len) == 0;
}

/* A row's bytes are the format: changing them is changing every commit. */
static void
test_bytes(void)
{
	uint64		tags[] = {7, 300, 5};
	WsRowShape	s;
	WsField		f[] = {text_field("ab"), text_field("xyz"), text_field("q")};
	Buf			k = BUF_INIT,
				v = BUF_INIT;

	/* keyed: the key's values length-prefixed; cells by tag, not by place */
	ws_row_shape_init(&s, tags, 3, 1);
	WsRowCodec->encode(&s, f, &k, &v);
	CHECK(bytes_are(&k, "\x02" "ab", 3));
	CHECK(bytes_are(&v, "\x05\x01q" "\xac\x02\x03xyz", 9));
	ws_row_shape_free(&s);

	/* keyless: the mark and all the cells; the value the caller's */
	ws_row_shape_init(&s, tags, 3, 0);
	WsRowCodec->encode(&s, f, &k, &v);
	CHECK(bytes_are(&k, "r" "\x05\x01q" "\x07\x02" "ab" "\xac\x02\x03xyz", 14));
	CHECK_EQ(v.len, 0);
	ws_row_shape_free(&s);
	buf_free(&k);
	buf_free(&v);
}

static void
test_round_trip(void)
{
	uint64		tags[] = {7, 8, 300, 5, 9};
	WsRowShape	s;
	WsField		f[] = {text_field("k1"), text_field(""), text_field("three"), null_field,
	text_field("nine")};
	WsField		got[5];
	Buf			k = BUF_INIT,
				v = BUF_INIT;

	for (int nkey = 0; nkey <= 2; nkey++)
	{
		ws_row_shape_init(&s, tags, 5, nkey);
		WsRowCodec->encode(&s, f, &k, &v);
		scribble(got, 5);
		WsRowCodec->decode(&s, k.data, k.len, v.data, v.len, got);
		CHECK(field_is(&got[0], "k1"));
		CHECK(field_is(&got[1], ""));	/* an empty value is no NULL */
		CHECK(field_is(&got[2], "three"));
		CHECK(got[3].null);
		CHECK(field_is(&got[4], "nine"));
		ws_row_shape_free(&s);
	}
	buf_free(&k);
	buf_free(&v);
}

/*
 * What a schema change leaves as it was: adding a nullable column, dropping
 * one that is NULL, and reordering columns change no byte of a row; decoding
 * under the new shape gives the new column NULL, and leaves out a dropped
 * column's cell.
 */
static void
test_schema_changes(void)
{
	uint64		was[] = {1, 2, 3};
	uint64		added[] = {1, 3, 2, 4};
	uint64		dropped[] = {1, 3};
	WsField		f[] = {text_field("k"), text_field("two"), text_field("three")};
	WsField		fa[] = {text_field("k"), text_field("three"), text_field("two"), null_field};
	WsField		got[4];
	WsRowShape	s0,
				s1,
				s2;
	Buf			k0 = BUF_INIT,
				v0 = BUF_INIT,
				k1 = BUF_INIT,
				v1 = BUF_INIT;

	ws_row_shape_init(&s0, was, 3, 1);
	ws_row_shape_init(&s1, added, 4, 1);
	ws_row_shape_init(&s2, dropped, 2, 1);
	WsRowCodec->encode(&s0, f, &k0, &v0);
	WsRowCodec->encode(&s1, fa, &k1, &v1);
	CHECK(k0.len == k1.len && memcmp(k0.data, k1.data, k0.len) == 0);
	CHECK(v0.len == v1.len && memcmp(v0.data, v1.data, v0.len) == 0);

	scribble(got, 4);
	WsRowCodec->decode(&s1, k0.data, k0.len, v0.data, v0.len, got);
	CHECK(field_is(&got[1], "three") && field_is(&got[2], "two") && got[3].null);
	scribble(got, 2);
	WsRowCodec->decode(&s2, k0.data, k0.len, v0.data, v0.len, got);
	CHECK(field_is(&got[0], "k") && field_is(&got[1], "three"));

	ws_row_shape_free(&s0);
	ws_row_shape_free(&s1);
	ws_row_shape_free(&s2);
	buf_free(&k0);
	buf_free(&v0);
	buf_free(&k1);
	buf_free(&v1);
}

/*
 * The rows at the edges: a keyless row all NULL, its key the mark alone; a
 * key-only row, its value empty and no buffer at all; the smallest and
 * largest tags; a keyless row with a cell its shape has no column for.
 */
static void
test_edge_rows(void)
{
	uint64		tags[] = {0, PG_UINT64_MAX};
	uint64		one[] = {5};
	WsField		nulls[] = {null_field, null_field};
	WsField		f[] = {text_field("low"), text_field("high")};
	WsField		got[2];
	WsRowShape	s;
	Buf			k = BUF_INIT,
				v = BUF_INIT;

	ws_row_shape_init(&s, tags, 2, 0);
	WsRowCodec->encode(&s, nulls, &k, &v);
	CHECK(bytes_are(&k, "r", 1));
	scribble(got, 2);
	WsRowCodec->decode(&s, k.data, k.len, NULL, 0, got);
	CHECK(got[0].null && got[1].null);

	WsRowCodec->encode(&s, f, &k, &v);
	scribble(got, 2);
	WsRowCodec->decode(&s, k.data, k.len, NULL, 0, got);
	CHECK(field_is(&got[0], "low") && field_is(&got[1], "high"));
	ws_row_shape_free(&s);

	/* every column the key's: no cells, and no value */
	ws_row_shape_init(&s, tags, 2, 2);
	WsRowCodec->encode(&s, f, &k, &v);
	CHECK_EQ(v.len, 0);
	scribble(got, 2);
	WsRowCodec->decode(&s, k.data, k.len, NULL, 0, got);
	CHECK(field_is(&got[0], "low") && field_is(&got[1], "high"));
	ws_row_shape_free(&s);

	/* a keyless row read with a shape of one column: the other cells left out */
	ws_row_shape_init(&s, tags, 2, 0);
	WsRowCodec->encode(&s, f, &k, &v);
	ws_row_shape_free(&s);
	ws_row_shape_init(&s, one, 1, 0);
	scribble(got, 1);
	WsRowCodec->decode(&s, k.data, k.len, NULL, 0, got);
	CHECK(got[0].null);
	ws_row_shape_free(&s);
	buf_free(&k);
	buf_free(&v);
}

/* A key read on its own, through the codec: diff's way in. */
static void
test_keys(void)
{
	uint64		tags[] = {7, 8, 9};
	WsField		f[] = {text_field("a"), text_field("bb"), text_field("v")};
	WsField		got[3];
	WsCell		c[WS_MAX_COLS];
	WsRowShape	s;
	Buf			k = BUF_INIT,
				v = BUF_INIT;

	ws_row_shape_init(&s, tags, 3, 2);
	WsRowCodec->encode(&s, f, &k, &v);
	scribble(got, 2);
	WsRowCodec->key_fields(&s, k.data, k.len, got);
	CHECK(field_is(&got[0], "a") && field_is(&got[1], "bb"));
	ws_row_shape_free(&s);

	ws_row_shape_init(&s, tags, 3, 0);
	WsRowCodec->encode(&s, f, &k, &v);
	CHECK_EQ(WsRowCodec->key_cells(k.data, k.len, c), 3);
	CHECK(c[0].tag == 7 && c[1].tag == 8 && c[2].tag == 9);
	CHECK(c[1].len == 2 && memcmp(c[1].p, "bb", 2) == 0);
	ws_row_shape_free(&s);
	buf_free(&k);
	buf_free(&v);
}

/* (the same tag twice, read for a tag past them) */
static void
cell_get_unordered(void *arg)
{
	const uint8 *p;
	size_t		len;

	(void) WsRowCodec->cell_get((const uint8 *) "\x01\x00\x01\x00", 4, 9, &p, &len);
}

static void
test_cell_get(void)
{
	WsCell		c[] = {{9, (const uint8 *) "nine", 4}, {2, (const uint8 *) "two", 3}};
	WsCell		out[WS_MAX_COLS];
	Buf			v = BUF_INIT;
	const uint8 *p;
	size_t		len;

	WsRowCodec->cells_encode(c, 2, &v);
	CHECK_EQ(WsRowCodec->cells(v.data, v.len, out), 2);
	CHECK(out[0].tag == 2 && out[1].tag == 9);
	CHECK(WsRowCodec->cell_get(v.data, v.len, 9, &p, &len));
	CHECK(len == 4 && memcmp(p, "nine", 4) == 0);
	CHECK(!WsRowCodec->cell_get(v.data, v.len, 3, &p, &len));
	CHECK(!WsRowCodec->cell_get(v.data, v.len, 10, &p, &len));
	CHECK(!WsRowCodec->cell_get(v.data, 0, 2, &p, &len));
	buf_free(&v);
	expect_error(cell_get_unordered, NULL, ERRCODE_DATA_CORRUPTED, NULL);
}

static void
encode_twice(void *arg)
{
	WsCell		c[] = {{4, (const uint8 *) "a", 1}, {4, (const uint8 *) "b", 1}};
	Buf			out = BUF_INIT;

	WsRowCodec->cells_encode(c, 2, &out);
}

static void
encode_too_many(void *arg)
{
	WsCell	   *c = palloc0_array(WsCell, WS_MAX_COLS + 1);
	Buf			out = BUF_INIT;

	for (int i = 0; i <= WS_MAX_COLS; i++)
		c[i].tag = (uint64) i;
	WsRowCodec->cells_encode(c, WS_MAX_COLS + 1, &out);
}

/* What cells would refuse to read is refused as it is written. */
static void
test_cells_encode_refuses(void)
{
	expect_error(encode_twice, NULL, ERRCODE_INTERNAL_ERROR, NULL);
	expect_error(encode_too_many, NULL, ERRCODE_INTERNAL_ERROR, NULL);
}

typedef struct Decoding
{
	const char *k;
	size_t		kl;
	const char *v;
	size_t		vl;
	int			nkey;
} Decoding;

static void
decode_it(void *arg)
{
	Decoding   *d = arg;
	uint64		tags[] = {1, 2, 3};
	WsField		f[3];
	WsRowShape	s;

	ws_row_shape_init(&s, tags, 3, d->nkey);
	WsRowCodec->decode(&s, (const uint8 *) d->k, d->kl, (const uint8 *) d->v, d->vl, f);
}

/* Malformed bytes are refused by name, whatever is wrong with them. */
static void
test_malformed(void)
{
	static const Decoding bad[] = {
		/* a key's value past its end, and bytes left over */
		{"\x05" "ab", 3, "", 0, 1},
		{"\x01" "a" "z", 3, "", 0, 1},
		/* a cell past its end, and a varint cut short */
		{"\x01" "a", 2, "\x02\x05q", 3, 1},
		{"\x01" "a", 2, "\x80", 1, 1},
		/* cells out of order, and the same tag twice */
		{"\x01" "a", 2, "\x03\x01q\x02\x01r", 6, 1},
		{"\x01" "a", 2, "\x02\x01q\x02\x01r", 6, 1},
		/* a keyless row without its mark, and an empty key */
		{"x", 1, "", 0, 0},
		{"", 0, "", 0, 0},
		/* a value's cell for the key's column */
		{"\x01" "a", 2, "\x01\x01q", 3, 1},
		/* numbers not in their shortest form: one row, other bytes */
		{"\x81\x00" "a", 3, "", 0, 1},
		{"\x01" "a", 2, "\x82\x00\x01q", 4, 1},
		{"\x01" "a", 2, "\x02\x81\x00q", 4, 1},
		{"\x01" "a", 2, "\xff\xff\xff\xff\xff\xff\xff\xff\xff\x7f\x01q", 12, 1},
	};
	Buf			wide = BUF_INIT;
	Decoding	d = {"\x01" "a", 2, NULL, 0, 1};

	for (size_t i = 0; i < lengthof(bad); i++)
		expect_error(decode_it, (void *) &bad[i], ERRCODE_DATA_CORRUPTED, NULL);

	/* one cell more than a table has columns */
	for (int i = 0; i <= WS_MAX_COLS; i++)
	{
		buf_append_uvarint(&wide, (uint64) i + 10);
		buf_append_uvarint(&wide, 0);
	}
	d.v = (const char *) wide.data;
	d.vl = wide.len;
	expect_error(decode_it, &d, ERRCODE_DATA_CORRUPTED, NULL);
	buf_free(&wide);
}

static void
shape_twice(void *arg)
{
	uint64		tags[] = {4, 4};
	WsRowShape	s;

	ws_row_shape_init(&s, tags, 2, 0);
}

static void
null_key(void *arg)
{
	uint64		tags[] = {1};
	WsField		f[] = {null_field};
	WsRowShape	s;
	Buf			k = BUF_INIT,
				v = BUF_INIT;

	ws_row_shape_init(&s, tags, 1, 1);
	WsRowCodec->encode(&s, f, &k, &v);
}

static void
test_shapes(void)
{
	uint64		tags[] = {30, 10, 20};
	WsRowShape	s;

	ws_row_shape_init(&s, tags, 3, 1);
	CHECK_EQ(ws_row_shape_at(&s, 30), 0);
	CHECK_EQ(ws_row_shape_at(&s, 10), 1);
	CHECK_EQ(ws_row_shape_at(&s, 20), 2);
	CHECK_EQ(ws_row_shape_at(&s, 15), -1);
	ws_row_shape_free(&s);
	expect_error(shape_twice, NULL, ERRCODE_INTERNAL_ERROR, NULL);
	expect_error(null_key, NULL, ERRCODE_INTERNAL_ERROR, NULL);
}

/* The most columns a table can have go through, each with a cell. */
static void
test_widest_row(void)
{
	uint64	   *tags = palloc(WS_MAX_COLS * sizeof(uint64));
	WsField    *f = palloc(WS_MAX_COLS * sizeof(WsField));
	WsField    *got = palloc(WS_MAX_COLS * sizeof(WsField));
	WsRowShape	s;
	Buf			k = BUF_INIT,
				v = BUF_INIT;

	for (int i = 0; i < WS_MAX_COLS; i++)
	{
		tags[i] = (uint64) (WS_MAX_COLS - i);
		f[i] = text_field("x");
	}
	/* (with no key: every column a cell, as many as the codec reads) */
	for (int nkey = 0; nkey <= 1; nkey++)
	{
		ws_row_shape_init(&s, tags, WS_MAX_COLS, nkey);
		WsRowCodec->encode(&s, f, &k, &v);
		scribble(got, WS_MAX_COLS);
		WsRowCodec->decode(&s, k.data, k.len, v.data, v.len, got);
		for (int i = 0; i < WS_MAX_COLS; i++)
			CHECK(field_is(&got[i], "x"));
		ws_row_shape_free(&s);
	}
	buf_free(&k);
	buf_free(&v);
	pfree(tags);
	pfree(f);
	pfree(got);
}

static void
split_too_many(void *arg)
{
	WsField		f[1];
	Buf			scratch = BUF_INIT;

	ws_copy_split("a\tb", 3, f, 1, &scratch);
}

static void
split_none_expected(void *arg)
{
	WsField		f[1];
	Buf			scratch = BUF_INIT;

	ws_copy_split("\t", 1, f, 0, &scratch);
}

static void
split_too_few(void *arg)
{
	WsField		f[2];
	Buf			scratch = BUF_INIT;

	ws_copy_split("a", 1, f, 2, &scratch);
}

/* COPY text: every escape there and back, NULL apart from a literal \N. */
static void
test_copy_text(void)
{
	const char *hard = "a\\b\nc\rd\te\bf\fg\vh\\N";
	uint64		tags[] = {1, 2, 3, 4};
	WsField		f[] = {text_field(hard), null_field, text_field(""), text_field("\\N")};
	WsField		got[4];
	WsRowShape	s;
	Buf			line = BUF_INIT,
				scratch = BUF_INIT;

	ws_row_shape_init(&s, tags, 4, 0);
	ws_copy_line(&s, f, &line);
	/* the line as a real COPY FROM reads it */
	CHECK(bytes_are(&line,
					"a\\\\b\\nc\\rd\\te\\bf\\fg\\vh\\\\N\t\\N\t\t\\\\N\n",
					strlen("a\\\\b\\nc\\rd\\te\\bf\\fg\\vh\\\\N\t\\N\t\t\\\\N\n")));
	ws_copy_split((const char *) line.data, line.len - 1, got, 4, &scratch);
	CHECK(field_is(&got[0], hard));
	CHECK(got[1].null);
	CHECK(field_is(&got[2], ""));
	CHECK(field_is(&got[3], "\\N"));
	ws_row_shape_free(&s);
	expect_error(split_too_many, NULL, ERRCODE_INTERNAL_ERROR, NULL);
	expect_error(split_too_few, NULL, ERRCODE_INTERNAL_ERROR, NULL);
	expect_error(split_none_expected, NULL, ERRCODE_INTERNAL_ERROR, NULL);

	/* a line ending in a tab: a last field that is empty, no NULL */
	ws_copy_split("a\t", 2, got, 2, &scratch);
	CHECK(field_is(&got[0], "a") && field_is(&got[1], ""));
	/* an empty line of one field: empty */
	ws_copy_split("", 0, got, 1, &scratch);
	CHECK(field_is(&got[0], ""));
	/* a value ending in a backslash, there and back */
	ws_row_shape_init(&s, tags, 1, 0);
	f[0] = text_field("end\\");
	ws_copy_line(&s, f, &line);
	ws_copy_split((const char *) line.data, line.len - 1, got, 1, &scratch);
	CHECK(field_is(&got[0], "end\\"));
	ws_row_shape_free(&s);
	/* a table with no stored columns: an empty line, of no fields */
	ws_row_shape_init(&s, tags, 0, 0);
	ws_copy_line(&s, f, &line);
	CHECK(bytes_are(&line, "\n", 1));
	ws_copy_split((const char *) line.data, line.len - 1, got, 0, &scratch);
	ws_row_shape_free(&s);
	buf_free(&line);
	buf_free(&scratch);
}

static const ProllyUnitTest tests[] = {
	{"bytes", test_bytes},
	{"round_trip", test_round_trip},
	{"schema_changes", test_schema_changes},
	{"edge_rows", test_edge_rows},
	{"keys", test_keys},
	{"cell_get", test_cell_get},
	{"cells_encode_refuses", test_cells_encode_refuses},
	{"malformed", test_malformed},
	{"shapes", test_shapes},
	{"widest_row", test_widest_row},
	{"copy_text", test_copy_text},
};

const ProllyUnitSuite unit_ws_row_suite = {"ws_row", tests, lengthof(tests)};
