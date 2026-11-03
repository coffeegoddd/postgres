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
	WsRows->encode(&s, f, &k, &v);
	CHECK(bytes_are(&k, "\x02" "ab", 3));
	CHECK(bytes_are(&v, "\x05\x01q" "\xac\x02\x03xyz", 9));
	ws_row_shape_free(&s);

	/* keyless: the mark and all the cells; the value the caller's */
	ws_row_shape_init(&s, tags, 3, 0);
	WsRows->encode(&s, f, &k, &v);
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
		WsRows->encode(&s, f, &k, &v);
		WsRows->decode(&s, k.data, k.len, v.data, v.len, got);
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
	WsRows->encode(&s0, f, &k0, &v0);
	WsRows->encode(&s1, fa, &k1, &v1);
	CHECK(k0.len == k1.len && memcmp(k0.data, k1.data, k0.len) == 0);
	CHECK(v0.len == v1.len && memcmp(v0.data, v1.data, v0.len) == 0);

	WsRows->decode(&s1, k0.data, k0.len, v0.data, v0.len, got);
	CHECK(field_is(&got[1], "three") && field_is(&got[2], "two") && got[3].null);
	WsRows->decode(&s2, k0.data, k0.len, v0.data, v0.len, got);
	CHECK(field_is(&got[0], "k") && field_is(&got[1], "three"));

	ws_row_shape_free(&s0);
	ws_row_shape_free(&s1);
	ws_row_shape_free(&s2);
	buf_free(&k0);
	buf_free(&v0);
	buf_free(&k1);
	buf_free(&v1);
}

static void
test_cell_get(void)
{
	WsCell		c[] = {{9, (const uint8 *) "nine", 4}, {2, (const uint8 *) "two", 3}};
	WsCell		out[WS_MAX_COLS];
	Buf			v = BUF_INIT;
	const uint8 *p;
	size_t		len;

	WsRows->cells_encode(c, 2, &v);
	CHECK_EQ(WsRows->cells(v.data, v.len, out), 2);
	CHECK(out[0].tag == 2 && out[1].tag == 9);
	CHECK(ws_cell_get(v.data, v.len, 9, &p, &len));
	CHECK(len == 4 && memcmp(p, "nine", 4) == 0);
	CHECK(!ws_cell_get(v.data, v.len, 3, &p, &len));
	buf_free(&v);
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
	WsRows->decode(&s, (const uint8 *) d->k, d->kl, (const uint8 *) d->v, d->vl, f);
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
	};

	for (size_t i = 0; i < lengthof(bad); i++)
		expect_error(decode_it, (void *) &bad[i], ERRCODE_DATA_CORRUPTED, NULL);
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
	WsRows->encode(&s, f, &k, &v);
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
	ws_row_shape_init(&s, tags, WS_MAX_COLS, 1);
	WsRows->encode(&s, f, &k, &v);
	WsRows->decode(&s, k.data, k.len, v.data, v.len, got);
	for (int i = 0; i < WS_MAX_COLS; i++)
		CHECK(field_is(&got[i], "x"));
	ws_row_shape_free(&s);
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
	CHECK_EQ(line.data[line.len - 1], '\n');
	ws_copy_split((const char *) line.data, line.len - 1, got, 4, &scratch);
	CHECK(field_is(&got[0], hard));
	CHECK(got[1].null);
	CHECK(field_is(&got[2], ""));
	CHECK(field_is(&got[3], "\\N"));
	ws_row_shape_free(&s);
	buf_free(&line);
	buf_free(&scratch);
	expect_error(split_too_many, NULL, ERRCODE_INTERNAL_ERROR, NULL);
	expect_error(split_too_few, NULL, ERRCODE_INTERNAL_ERROR, NULL);
}

static const ProllyUnitTest tests[] = {
	{"bytes", test_bytes},
	{"round_trip", test_round_trip},
	{"schema_changes", test_schema_changes},
	{"cell_get", test_cell_get},
	{"malformed", test_malformed},
	{"shapes", test_shapes},
	{"widest_row", test_widest_row},
	{"copy_text", test_copy_text},
};

const ProllyUnitSuite unit_ws_row_suite = {"ws_row", tests, lengthof(tests)};
