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
#include "prolly/engine/vc.h"
#include "prolly/ws/ws_row.h"
#include "test_prolly.h"

static WsField
text_field(const char *s)
{
	WsField		f = {(const uint8 *) s, strlen(s), false, false};

	return f;
}

static const WsField null_field = {NULL, 0, true, false};

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
		f[i].large = true;
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
	CHECK(bytes_are(&v, "\x00" "\x05\x02q" "\xac\x02\x06xyz", 10));
	ws_row_shape_free(&s);

	/* keyless: the mark and all the cells; the value the caller's */
	ws_row_shape_init(&s, tags, 3, 0);
	WsRowCodec->encode(&s, f, &k, &v);
	CHECK(bytes_are(&k, "r" "\x05\x02q" "\x07\x04" "ab" "\xac\x02\x06xyz", 14));
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

	/* every column the key's: no cells, and no trees */
	ws_row_shape_init(&s, tags, 2, 2);
	WsRowCodec->encode(&s, f, &k, &v);
	CHECK(bytes_are(&v, "\x00", 1));
	scribble(got, 2);
	WsRowCodec->decode(&s, k.data, k.len, v.data, v.len, got);
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
	WsCell		got;

	(void) WsRowCodec->cell_get((const uint8 *) "\x00\x01\x00\x01\x00", 5, 9, &got);
}

static void
test_cell_get(void)
{
	WsCell		c[] = {{9, (const uint8 *) "nine", 4}, {2, (const uint8 *) "two", 3}};
	WsCell		out[WS_MAX_COLS];
	Buf			v = BUF_INIT;
	WsCell		one;

	WsRowCodec->cells_encode(c, 2, &v);
	CHECK_EQ(WsRowCodec->cells(v.data, v.len, out), 2);
	CHECK(out[0].tag == 2 && out[1].tag == 9);
	CHECK(WsRowCodec->cell_get(v.data, v.len, 9, &one));
	CHECK(one.len == 4 && memcmp(one.p, "nine", 4) == 0 && !one.large);
	CHECK(!WsRowCodec->cell_get(v.data, v.len, 3, &one));
	CHECK(!WsRowCodec->cell_get(v.data, v.len, 10, &one));
	CHECK(!WsRowCodec->cell_get((const uint8 *) "\x00", 1, 2, &one));
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
		{"\x01" "a", 2, "\x00\x02\x0aq", 4, 1},
		{"\x01" "a", 2, "\x00\x80", 2, 1},
		/* cells out of order, and the same tag twice */
		{"\x01" "a", 2, "\x00\x03\x02q\x02\x02r", 7, 1},
		{"\x01" "a", 2, "\x00\x02\x02q\x02\x02r", 7, 1},
		/* a keyless row without its mark, and an empty key */
		{"x", 1, "", 0, 0},
		{"", 0, "", 0, 0},
		/* a value's cell for the key's column */
		{"\x01" "a", 2, "\x00\x01\x02q", 4, 1},
		/* numbers not in their shortest form: one row, other bytes */
		{"\x81\x00" "a", 3, "", 0, 1},
		{"\x01" "a", 2, "\x00\x82\x00\x02q", 5, 1},
		{"\x01" "a", 2, "\x00\x02\x82\x00q", 5, 1},
		/* a value with no count of its trees, and one of more than it holds */
		{"\x01" "a", 2, "", 0, 1},
		{"\x01" "a", 2, "\x01", 1, 1},
		{"\x01" "a", 2, "\x80\x00", 2, 1},
		{"\x01" "a", 2, "\x00\xff\xff\xff\xff\xff\xff\xff\xff\xff\x7f\x02q", 13, 1},
	};
	Buf			wide = BUF_INIT;
	Decoding	d = {"\x01" "a", 2, NULL, 0, 1};

	for (size_t i = 0; i < lengthof(bad); i++)
		expect_error(decode_it, (void *) &bad[i], ERRCODE_DATA_CORRUPTED, NULL);

	/* one cell more than a table has columns */
	buf_append_uvarint(&wide, 0);
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

/* A value none of whose pieces is another's. */
static uint8 *
large_value(size_t len, uint32 seed)
{
	uint8	   *p = palloc(len);

	for (size_t i = 0; i < len; i++)
	{
		seed = seed * 2654435761U + 1;
		p[i] = (uint8) (seed >> 24);
	}
	return p;
}

static WsField
bytes_field(const uint8 *p, size_t len)
{
	WsField		f = {p, len, false, false};

	return f;
}

static bool
field_holds(const WsField *f, const uint8 *p, size_t len)
{
	return !f->null && !f->large && f->len == len && memcmp(f->p, p, len) == 0;
}

/* A row's large values come back, whichever side of the limit, with or without a key. */
ENGINE_TEST(test_large_round_trip)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &prolly_default_policy);
	uint64		tags[] = {9, 3, 7, 5};
	uint8	   *key = large_value(5000, 1);
	uint8	   *big = large_value(100000, 2);
	uint8	   *at_limit = large_value(WS_LARGE_CELL, 3);
	uint8	   *over = large_value(WS_LARGE_CELL + 1, 4);
	WsField		f[4];
	WsField		got[4];
	WsCell		c[WS_MAX_COLS];
	WsRowShape	s;
	Buf			k = BUF_INIT,
				v = BUF_INIT,
				held = BUF_INIT;

	f[0] = bytes_field(key, 5000);
	f[1] = bytes_field(big, 100000);
	f[2] = bytes_field(at_limit, WS_LARGE_CELL);
	f[3] = bytes_field(over, WS_LARGE_CELL + 1);

	/* keyed: the key's in the key however long; two trees, one cell inline */
	ws_row_shape_init(&s, tags, 4, 1);
	ws_row_store(&ctx, &s, f, &k, &v);
	CHECK(k.len > 5000);
	CHECK(v.len < WS_LARGE_CELL + 200);
	CHECK_EQ(v.data[0], 2);
	CHECK_EQ(WsRowCodec->cells(v.data, v.len, c), 3);
	CHECK(c[0].tag == 3 && c[0].large);
	CHECK(c[1].tag == 5 && c[1].large);
	CHECK(c[2].tag == 7 && !c[2].large);
	/* (the trees in order of their tags, not of the columns) */
	CHECK(memcmp(v.data + 1, c[0].p, PHASH_LEN) == 0);
	CHECK(memcmp(v.data + 1 + PHASH_LEN, c[1].p, PHASH_LEN) == 0);
	scribble(got, 4);
	ws_row_load(&ctx, &s, k.data, k.len, v.data, v.len, got, &held);
	CHECK(field_holds(&got[0], key, 5000));
	CHECK(field_holds(&got[1], big, 100000));
	CHECK(field_holds(&got[2], at_limit, WS_LARGE_CELL));
	CHECK(field_holds(&got[3], over, WS_LARGE_CELL + 1));
	ws_row_shape_free(&s);

	/* keyless: all in cells of its key, its value naming its trees */
	ws_row_shape_init(&s, tags, 4, 0);
	ws_row_store(&ctx, &s, f, &k, &v);
	CHECK(k.len < WS_LARGE_CELL + 400);
	CHECK_EQ(v.len, 0);
	WsRowCodec->keyless_value(k.data, k.len, (const uint8 *) "\x05", 1, &v);
	CHECK_EQ(v.data[0], 3);
	CHECK_EQ(v.len, 1 + 3 * PHASH_LEN + 1);
	{
		const uint8 *payload;
		size_t		plen;

		WsRowCodec->keyless_payload(k.data, k.len, v.data, v.len, &payload, &plen);
		CHECK(plen == 1 && payload[0] == 5);
	}
	scribble(got, 4);
	ws_row_load(&ctx, &s, k.data, k.len, v.data, v.len, got, &held);
	CHECK(field_holds(&got[0], key, 5000));
	CHECK(field_holds(&got[1], big, 100000));
	CHECK(field_holds(&got[2], at_limit, WS_LARGE_CELL));
	CHECK(field_holds(&got[3], over, WS_LARGE_CELL + 1));
	ws_row_shape_free(&s);
	buf_free(&k);
	buf_free(&v);
	buf_free(&held);
}

/* One value is one set of bytes, whatever row it is in; and an edit shares the rest. */
ENGINE_TEST(test_large_sharing)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &prolly_default_policy);
	size_t		len = 100000;
	uint8	   *a = large_value(len, 7);
	uint8	   *b = palloc(len + 5);
	PHash		ra,
				ra2,
				rb;
	ProllyDiffList d = {0};

	WsLargeValues->put(&ctx, a, len, &ra);
	WsLargeValues->put(&ctx, a, len, &ra2);
	CHECK(phash_eq(&ra, &ra2));

	/* a byte changed: one piece */
	memcpy(b, a, len);
	b[50000] ^= 1;
	WsLargeValues->put(&ctx, b, len, &rb);
	CHECK(!phash_eq(&ra, &rb));
	CHECK_OK(prolly_diff_collect(&ctx, &ra, &rb, &d));
	CHECK_EQ(d.n, 1);
	prolly_diff_list_free(&d);

	/* bytes added at the end of a whole number of pieces: one new piece */
	memcpy(b, a, len);
	memcpy(b + len, "abcde", 5);
	WsLargeValues->put(&ctx, b, len + 5, &rb);
	memset(&d, 0, sizeof(d));
	CHECK_OK(prolly_diff_collect(&ctx, &ra, &rb, &d));
	CHECK_EQ(d.n, 1);
	prolly_diff_list_free(&d);
}

typedef struct RefsSeen
{
	PHash		want[4];
	int			nwant;
	int			strong;
} RefsSeen;

static void
ref_seen(void *arg, const PHash *h, bool strong)
{
	RefsSeen   *seen = arg;

	for (int i = 0; i < seen->nwant; i++)
		if (strong && phash_eq(h, &seen->want[i]))
			seen->strong++;
}

/* Storage finds a row's trees knowing nothing of rows: what keeps them. */
ENGINE_TEST(test_large_trees_held)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &prolly_default_policy);
	uint64		tags[] = {1, 2};
	uint8	   *big = large_value(10000, 11);
	WsField		f[2];
	WsCell		c[WS_MAX_COLS];
	WsRowShape	s;
	Buf			k = BUF_INIT,
				v = BUF_INIT;
	ProllyKV	kv;
	PHash		map;
	uint8	   *node;
	uint32		len;
	RefsSeen	seen = {0};
	uint64		total;

	for (int keyed = 1; keyed >= 0; keyed--)
	{
		f[0] = keyed ? text_field("k") : bytes_field(big, 10000);
		f[1] = bytes_field(big, 10000);
		ws_row_shape_init(&s, tags, 2, keyed);
		ws_row_store(&ctx, &s, f, &k, &v);
		if (keyed)
			CHECK_EQ(WsRowCodec->cells(v.data, v.len, c), 1);
		else
		{
			CHECK_EQ(WsRowCodec->key_cells(k.data, k.len, c), 2);
			WsRowCodec->keyless_value(k.data, k.len, (const uint8 *) "\x01", 1, &v);
		}
		WsRowCodec->large_decode(c[0].p, c[0].len, &seen.want[0], &total);
		CHECK_EQ(total, 10000);
		seen.nwant = 1;
		seen.strong = 0;
		kv.key = k.data;
		kv.klen = k.len;
		kv.val = v.data;
		kv.vlen = v.len;
		CHECK_OK(prolly_build(&ctx, WS_ROW_VREFS, &kv, 1, &map));
		node = cs_get(cs, map.b, &len);
		CHECK(engine_chunk_refs(node, len, ref_seen, &seen));
		CHECK_EQ(seen.strong, keyed ? 1 : 2);
		pfree(node);
		ws_row_shape_free(&s);
	}
	buf_free(&k);
	buf_free(&v);
}

/* A row map's value that is not its count and hashes is not stored. */
ENGINE_TEST(test_large_counted_values)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &prolly_default_policy);
	ProllyKV	kv = {(const uint8 *) "k", 1, NULL, 0};
	PHash		map;

	kv.val = (const uint8 *) "";
	CHECK_EQ(prolly_build(&ctx, WS_ROW_VREFS, &kv, 1, &map), PROLLY_ERR_BAD_VALUE);
	kv.val = (const uint8 *) "\x01" "short";
	kv.vlen = 6;
	CHECK_EQ(prolly_build(&ctx, WS_ROW_VREFS, &kv, 1, &map), PROLLY_ERR_BAD_VALUE);
	kv.val = (const uint8 *) "\x00";
	kv.vlen = 1;
	CHECK_OK(prolly_build(&ctx, WS_ROW_VREFS, &kv, 1, &map));
}

typedef struct LargeCellBad
{
	const uint8 *v;
	size_t		vl;
} LargeCellBad;

static void
decode_large_bad(void *arg)
{
	LargeCellBad *b = arg;
	uint64		tags[] = {1, 2};
	WsField		f[2];
	WsRowShape	s;

	ws_row_shape_init(&s, tags, 2, 1);
	WsRowCodec->decode(&s, (const uint8 *) "\x01" "a", 2, b->v, b->vl, f);
}

static void
encode_inline_too_long(void *arg)
{
	uint64		tags[] = {1, 2};
	uint8	   *big = palloc0(WS_LARGE_CELL + 1);
	WsField		f[2];
	WsRowShape	s;
	Buf			k = BUF_INIT,
				v = BUF_INIT;

	f[0] = text_field("k");
	f[1] = bytes_field(big, WS_LARGE_CELL + 1);
	ws_row_shape_init(&s, tags, 2, 1);
	WsRowCodec->encode(&s, f, &k, &v);
}

static void
encode_large_key(void *arg)
{
	uint64		tags[] = {1};
	WsField		f[1];
	WsRowShape	s;
	Buf			k = BUF_INIT,
				v = BUF_INIT;

	f[0] = text_field("k");
	f[0].large = true;
	ws_row_shape_init(&s, tags, 1, 1);
	WsRowCodec->encode(&s, f, &k, &v);
}

/* A value's trees and cells must say the same, or a tree could go uncounted. */
static void
test_large_malformed(void)
{
	PHash		h1,
				h2;
	uint64		total = WS_LARGE_CELL + 1;
	Buf			named = BUF_INIT;
	Buf			v = BUF_INIT;
	uint8	   *big = palloc0(WS_LARGE_CELL + 1);
	LargeCellBad b;
	WsCell		out;

	memset(h1.b, 0x11, PHASH_LEN);
	memset(h2.b, 0x22, PHASH_LEN);
	WsRowCodec->large_encode(&h1, total, &named);

#define START(nrefs, ref) \
	do { \
		v.len = 0; \
		buf_append_uvarint(&v, (nrefs)); \
		if ((nrefs) > 0) \
			buf_append(&v, (ref)->b, PHASH_LEN); \
	} while (0)
#define CELL(tag, marked, p, n) \
	do { \
		buf_append_uvarint(&v, (tag)); \
		buf_append_uvarint(&v, ((uint64) (n) << 1) | (marked)); \
		buf_append(&v, (p), (n)); \
	} while (0)
#define REFUSED() \
	do { \
		b.v = v.data; \
		b.vl = v.len; \
		expect_error(decode_large_bad, &b, ERRCODE_DATA_CORRUPTED, NULL); \
	} while (0)

	/* the good one, so the others fail for what they say */
	START(1, &h1);
	CELL(2, 1, named.data, named.len);
	CHECK(WsRowCodec->cell_get(v.data, v.len, 2, &out));
	CHECK(out.large && out.len == named.len);

	/* a value in its cell that is over the limit: another set of bytes for it */
	START(0, &h1);
	CELL(2, 0, big, WS_LARGE_CELL + 1);
	REFUSED();
	/* a tree no cell names, a cell no tree says, and two that differ */
	START(1, &h1);
	REFUSED();
	START(0, &h1);
	CELL(2, 1, named.data, named.len);
	REFUSED();
	START(1, &h2);
	CELL(2, 1, named.data, named.len);
	REFUSED();
	/* a large cell of a short value's length, and one that is no tree */
	named.len = 0;
	WsRowCodec->large_encode(&h1, WS_LARGE_CELL, &named);
	START(1, &h1);
	CELL(2, 1, named.data, named.len);
	REFUSED();
	START(1, &h1);
	CELL(2, 1, (const uint8 *) "short", 5);
	REFUSED();

	expect_error(encode_inline_too_long, NULL, ERRCODE_INTERNAL_ERROR, NULL);
	expect_error(encode_large_key, NULL, ERRCODE_INTERNAL_ERROR, NULL);
	buf_free(&named);
	buf_free(&v);
#undef START
#undef CELL
#undef REFUSED
}

typedef struct BadTree
{
	ProllyCtx  *ctx;
	PHash		root;
	uint64		len;
} BadTree;

static void
read_bad_tree(void *arg)
{
	BadTree    *b = arg;
	Buf			out = BUF_INIT;

	WsLargeValues->get(b->ctx, &b->root, b->len, &out);
}

/* A tree that is not the value its cell says is refused, not read short. */
ENGINE_TEST(test_large_wrong_tree)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &prolly_default_policy);
	uint8	   *big = large_value(10000, 21);
	uint8		off[2][8] = {{0}};
	ProllyKV	kvs[2];
	BadTree		b = {&ctx};
	Buf			out = BUF_INIT;

	WsLargeValues->put(&ctx, big, 10000, &b.root);
	WsLargeValues->get(&ctx, &b.root, 10000, &out);
	CHECK(out.len == 10000 && memcmp(out.data, big, 10000) == 0);

	/* longer and shorter than it says */
	b.len = 9999;
	expect_error(read_bad_tree, &b, ERRCODE_DATA_CORRUPTED, NULL);
	b.len = 10001;
	expect_error(read_bad_tree, &b, ERRCODE_DATA_CORRUPTED, NULL);

	/* a piece missing between two others */
	off[1][6] = 8000 >> 8;
	off[1][7] = 8000 & 0xff;
	kvs[0] = (ProllyKV) {off[0], 8, big, 4000};
	kvs[1] = (ProllyKV) {off[1], 8, big + 4000, 4000};
	CHECK_OK(prolly_build(&ctx, 0, kvs, 2, &b.root));
	b.len = 8000;
	expect_error(read_bad_tree, &b, ERRCODE_DATA_CORRUPTED, NULL);
	buf_free(&out);
}

typedef struct KeylessBad
{
	ProllyCtx  *ctx;
	const WsRowShape *s;
	Buf		   *k;
	Buf		   *v;
} KeylessBad;

static void
load_keyless_bad(void *arg)
{
	KeylessBad *b = arg;
	WsField		got[1];
	Buf			held = BUF_INIT;

	ws_row_load(b->ctx, b->s, b->k->data, b->k->len, b->v->data, b->v->len, got, &held);
}

/* A keyless row's value says what its key names, or a read refuses it. */
ENGINE_TEST(test_large_keyless_refs)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &prolly_default_policy);
	uint64		tags[] = {1};
	uint8	   *big = large_value(5000, 31);
	WsField		f[1];
	WsRowShape	s;
	Buf			k = BUF_INIT,
				v = BUF_INIT;
	KeylessBad	b = {&ctx, &s, &k, &v};

	f[0] = bytes_field(big, 5000);
	ws_row_shape_init(&s, tags, 1, 0);
	ws_row_store(&ctx, &s, f, &k, &v);
	WsRowCodec->keyless_value(k.data, k.len, (const uint8 *) "\x01", 1, &v);
	v.data[1] ^= 1;
	expect_error(load_keyless_bad, &b, ERRCODE_DATA_CORRUPTED, NULL);
	v.len = 0;
	buf_append_uvarint(&v, 0);
	expect_error(load_keyless_bad, &b, ERRCODE_DATA_CORRUPTED, NULL);
	ws_row_shape_free(&s);
	buf_free(&k);
	buf_free(&v);
}

static void
store_a_tree_as_a_value(void *arg)
{
	ProllyCtx  *ctx = arg;
	uint64		tags[] = {1, 2};
	WsField		f[2];
	WsRowShape	s;
	Buf			k = BUF_INIT,
				v = BUF_INIT;

	f[0] = text_field("k");
	f[1] = text_field("named");
	f[1].large = true;
	ws_row_shape_init(&s, tags, 2, 1);
	ws_row_store(ctx, &s, f, &k, &v);
}

static void
encode_a_bad_tree(void *arg)
{
	WsCell		c[] = {{4, (const uint8 *) "short", 5, true}};
	Buf			out = BUF_INIT;

	WsRowCodec->cells_encode(c, 1, &out);
}

ENGINE_TEST(test_large_refused_on_writing)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &prolly_default_policy);

	expect_error(store_a_tree_as_a_value, &ctx, ERRCODE_INTERNAL_ERROR, NULL);
	expect_error(encode_a_bad_tree, NULL, ERRCODE_DATA_CORRUPTED, NULL);
}

/* Pieces sharing a leaf are as much the value: nothing hangs on one piece a leaf. */
ENGINE_TEST(test_large_pieces_sharing_leaves)
{
	static const ChunkPolicy wide = {16384, 65536, 262144};
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &wide);
	uint8	   *big = large_value(100000, 41);
	PHash		root;
	Buf			out = BUF_INIT;

	WsLargeValues->put(&ctx, big, 100000, &root);
	WsLargeValues->get(&ctx, &root, 100000, &out);
	CHECK(out.len == 100000 && memcmp(out.data, big, 100000) == 0);
	buf_free(&out);
}

/* One row map's row, its key the given one, as a map. */
static PHash
map_of_row(ProllyCtx *ctx, const WsRowShape *s, WsField *f)
{
	Buf			k = BUF_INIT,
				v = BUF_INIT;
	ProllyKV	kv;
	PHash		map;

	ws_row_store(ctx, s, f, &k, &v);
	kv.key = k.data;
	kv.klen = k.len;
	kv.val = v.data;
	kv.vlen = v.len;
	CHECK_OK(prolly_build(ctx, WS_ROW_VREFS, &kv, 1, &map));
	buf_free(&k);
	buf_free(&v);
	return map;
}

/* A row's trees are held by the row map and go with it, one tree in two columns too. */
ENGINE_MEMORY_TEST(test_large_trees_freed)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &prolly_default_policy);
	uint64		tags[] = {1, 2, 3};
	uint8	   *big = large_value(20000, 51);
	uint8	   *other = large_value(20000, 52);
	WsField		f[3];
	WsRowShape	s;
	PHash		map,
				next,
				tree,
				kept;
	Buf			out = BUF_INIT;

	ws_row_shape_init(&s, tags, 3, 1);
	f[0] = text_field("k");
	f[1] = bytes_field(big, 20000);
	f[2] = bytes_field(big, 20000);
	map = map_of_row(&ctx, &s, f);
	WsLargeValues->put(&ctx, big, 20000, &tree);
	cs_ref_root(cs, map.b);

	f[2] = bytes_field(other, 20000);
	f[1] = bytes_field(other, 20000);
	next = map_of_row(&ctx, &s, f);
	WsLargeValues->put(&ctx, other, 20000, &kept);
	cs_ref_root(cs, next.b);
	settle_and_check(cs);
	CHECK(cs_has(cs, tree.b));

	/* the first row goes, and its tree with it; the other stays whole */
	cs_unref_root(cs, map.b);
	CHECK(cs_free(cs, map.b, 1) > 0);
	settle_and_check(cs);
	CHECK(!cs_has(cs, tree.b));
	WsLargeValues->get(&ctx, &kept, 20000, &out);
	CHECK(out.len == 20000 && memcmp(out.data, other, 20000) == 0);
	ws_row_shape_free(&s);
	buf_free(&out);
}

static void
count_chunk(void *arg, const PHash *h)
{
	uint64	   *total = arg;

	(*total)++;
}

/* What a commit measures counts a large value's tree, and shares its unchanged pieces. */
ENGINE_TEST(test_large_change_bytes)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &prolly_default_policy);
	size_t		len = 200000;
	uint64		tags[] = {1, 2};
	uint8	   *big = large_value(len, 61);
	WsField		f[2];
	WsRowShape	s;
	PHash		empty,
				one,
				two,
				rv,
				rv1,
				rv2,
				c1,
				c2,
				c3;
	uint64		nodes = 0;
	VcCommit	k;
	uint64		b1,
				b2,
				b3;

	ws_row_shape_init(&s, tags, 2, 1);
	f[0] = text_field("k");
	f[1] = bytes_field(big, len);
	one = map_of_row(&ctx, &s, f);
	CHECK_OK(prolly_empty(&ctx, 0, &empty));

	/* every piece new against nothing */
	CHECK_OK(prolly_chunk_diff_values(&ctx, &one, &empty, count_chunk, &nodes));
	CHECK(nodes >= len / 4000);

	/* a byte changed: a piece and its way up */
	big[100000] ^= 1;
	two = map_of_row(&ctx, &s, f);
	nodes = 0;
	CHECK_OK(prolly_chunk_diff_values(&ctx, &two, &one, count_chunk, &nodes));
	CHECK(nodes > 0 && nodes < 10);

	/* nothing changed, and a tree of no references */
	nodes = 0;
	CHECK_OK(prolly_chunk_diff_values(&ctx, &one, &one, count_chunk, &nodes));
	CHECK_OK(prolly_chunk_diff_values(&ctx, &empty, &one, count_chunk, &nodes));
	CHECK_EQ(nodes, 0);

	/* and through a commit: a value's size, then a piece's */
	rv = vc_root_empty(&ctx);
	vc_table_put(&ctx, &rv, "t", "x", 1, &one, &rv1);
	vc_table_put(&ctx, &rv, "t", "x", 1, &two, &rv2);
	c1 = vc_commit_write(&ctx, &rv, NULL, 0, 1, "none");
	c2 = vc_commit_write(&ctx, &rv1, &c1, 1, 2, "large");
	c3 = vc_commit_write(&ctx, &rv2, &c2, 1, 3, "edit");
	CHECK(vc_commit_load(&ctx, &c1, &k));
	b1 = k.bytes;
	vc_commit_release(&k);
	CHECK(vc_commit_load(&ctx, &c2, &k));
	b2 = k.bytes;
	vc_commit_release(&k);
	CHECK(vc_commit_load(&ctx, &c3, &k));
	b3 = k.bytes;
	vc_commit_release(&k);
	CHECK(b2 - b1 >= len);
	CHECK(b3 - b2 > 4000 && b3 - b2 < 40000);
	ws_row_shape_free(&s);
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
	{"large_round_trip", test_large_round_trip},
	{"large_sharing", test_large_sharing},
	{"large_trees_held", test_large_trees_held},
	{"large_counted_values", test_large_counted_values},
	{"large_malformed", test_large_malformed},
	{"large_wrong_tree", test_large_wrong_tree},
	{"large_keyless_refs", test_large_keyless_refs},
	{"large_refused_on_writing", test_large_refused_on_writing},
	{"large_pieces_sharing_leaves", test_large_pieces_sharing_leaves},
	{"large_trees_freed", test_large_trees_freed},
	{"large_change_bytes", test_large_change_bytes},
};

const ProllyUnitSuite unit_ws_row_suite = {"ws_row", tests, lengthof(tests)};
