/*-------------------------------------------------------------------------
 *
 * unit_ws_schema.c
 *	  Unit tests of the working-set model's table schemas: the bytes a schema
 *	  is, what makes one invalid, malformed bytes refused, and the row shape
 *	  it gives.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/test/modules/test_prolly/unit_ws_schema.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "engine_test_util.h"
#include "mb/pg_wchar.h"
#include "prolly/ws/ws_schema.h"
#include "test_prolly.h"

/* t (id bigint PRIMARY KEY, v text COLLATE "C" DEFAULT '', g bigint GENERATED, w ...) */
static WsColumn example_cols[] = {
	{.tag = 7,.name = "id",.type = "bigint",.kind = WS_COL_STORED,.not_null = true,
	.identity = WS_IDENTITY_ALWAYS},
	{.tag = 300,.name = "v",.type = "text",.kind = WS_COL_STORED,.def = "''::text",
	.collation = "pg_catalog.\"C\""},
	{.tag = 5,.name = "g",.type = "bigint",.kind = WS_COL_GENERATED_STORED,.def = "(@{7} * 2)"},
	{.tag = 9,.name = "w",.type = "bigint",.kind = WS_COL_GENERATED_VIRTUAL,.def = "(@{7} + 1)"},
	{.tag = 0,.name = "e",.type = "integer",.kind = WS_COL_STORED},
};

static uint64 example_key[] = {7};

static WsTableSchema
example(void)
{
	WsTableSchema s = {.name = "t",.persistence = WS_PERSISTENCE_PERMANENT,
		.ncols = lengthof(example_cols),.cols = example_cols,
	.nkey = lengthof(example_key),.key = example_key};

	return s;
}

static bool
str_same(const char *a, const char *b)
{
	return (a == NULL && b == NULL) || (a && b && strcmp(a, b) == 0);
}

/* A schema's bytes are the format: changing them is changing every commit. */
static void
test_bytes(void)
{
	static const char want[] =
		"\x01"					/* version */
		"\x01" "t" "p"			/* name, persistence */
		"\x01"					/* columns */
		"\x07" "\x02" "id" "\x06" "bigint" "s" "\x01" "a" "\x00" "\x00"
		"\x01\x07";				/* key */
	WsColumn	cols[] = {{.tag = 7,.name = "id",.type = "bigint",.kind = WS_COL_STORED,
		.not_null = true,.identity = WS_IDENTITY_ALWAYS}};
	WsTableSchema s = {.name = "t",.persistence = WS_PERSISTENCE_PERMANENT,.ncols = 1,
	.cols = cols,.nkey = 1,.key = example_key};
	Buf			b = BUF_INIT;

	WsSchemaCodec->encode(&s, &b);
	CHECK_EQ(b.len, sizeof(want) - 1);
	CHECK(memcmp(b.data, want, b.len) == 0);
	buf_free(&b);

	/* each kind of column, a default and a collation, a tag of two bytes, unlogged */
	{
		static const char want2[] =
			"\x01" "\x01" "t" "u" "\x03"
			"\x07" "\x01" "k" "\x01" "i" "s" "\x01" "d" "\x00" "\x00"
			"\xac\x02" "\x01" "v" "\x01" "t" "s" "\x00" "\x00" "\x03" "\'x\'" "\x03" "\"C\""
			"\x05" "\x01" "g" "\x01" "i" "g" "\x00" "\x00" "\x01" "1" "\x00"
			"\x01\x07";
		WsColumn	c2[] = {
			{.tag = 7,.name = "k",.type = "i",.kind = WS_COL_STORED,.not_null = true,
			.identity = WS_IDENTITY_BY_DEFAULT},
			{.tag = 300,.name = "v",.type = "t",.kind = WS_COL_STORED,.def = "'x'",.collation = "\"C\""},
			{.tag = 5,.name = "g",.type = "i",.kind = WS_COL_GENERATED_STORED,.def = "1"},
		};
		WsTableSchema s2 = {.name = "t",.persistence = WS_PERSISTENCE_UNLOGGED,.ncols = 3,
		.cols = c2,.nkey = 1,.key = example_key};

		WsSchemaCodec->encode(&s2, &b);
		CHECK_EQ(b.len, sizeof(want2) - 1);
		CHECK(memcmp(b.data, want2, b.len) == 0);
		buf_free(&b);
	}
}

static void
test_round_trip(void)
{
	WsTableSchema s = example();
	WsTableSchema got;
	Buf			a = BUF_INIT,
				b = BUF_INIT;

	WsSchemaCodec->encode(&s, &a);
	WsSchemaCodec->decode(a.data, a.len, &got);
	CHECK(strcmp(got.name, "t") == 0 && got.persistence == WS_PERSISTENCE_PERMANENT);
	CHECK_EQ(got.ncols, s.ncols);
	for (int i = 0; i < s.ncols; i++)
	{
		CHECK_EQ(got.cols[i].tag, s.cols[i].tag);
		CHECK(strcmp(got.cols[i].name, s.cols[i].name) == 0);
		CHECK(strcmp(got.cols[i].type, s.cols[i].type) == 0);
		CHECK_EQ(got.cols[i].kind, s.cols[i].kind);
		CHECK_EQ(got.cols[i].not_null, s.cols[i].not_null);
		CHECK_EQ(got.cols[i].identity, s.cols[i].identity);
		CHECK(str_same(got.cols[i].def, s.cols[i].def));
		CHECK(str_same(got.cols[i].collation, s.cols[i].collation));
	}
	CHECK(got.nkey == 1 && got.key[0] == 7);

	/* written again, the same bytes */
	WsSchemaCodec->encode(&got, &b);
	CHECK(a.len == b.len && memcmp(a.data, b.data, a.len) == 0);
	ws_schema_free(&got);
	buf_free(&a);
	buf_free(&b);
}

/* A table with no columns and no key is one; so is an unlogged one. */
static void
test_smallest(void)
{
	WsTableSchema s = {.name = "empty",.persistence = WS_PERSISTENCE_UNLOGGED};
	WsTableSchema got;
	WsRowShape	shape;
	Buf			b = BUF_INIT;

	WsSchemaCodec->encode(&s, &b);
	WsSchemaCodec->decode(b.data, b.len, &got);
	CHECK(got.ncols == 0 && got.nkey == 0 && got.persistence == WS_PERSISTENCE_UNLOGGED);
	ws_schema_row_shape(&got, &shape);
	CHECK_EQ(shape.nstored, 0);
	ws_row_shape_free(&shape);
	ws_schema_free(&got);
	buf_free(&b);
}

static void
test_find(void)
{
	WsTableSchema s = example();

	CHECK(ws_schema_column(&s, 300) == &s.cols[1]);
	CHECK(ws_schema_column(&s, 0) == &s.cols[4]);
	CHECK(ws_schema_column(&s, 1) == NULL);
	CHECK(ws_schema_column_named(&s, "g") == &s.cols[2]);
	CHECK(ws_schema_column_named(&s, "nope") == NULL);
}

/*
 * The row shape: the key, then the other stored columns in attribute order,
 * generated or not; a virtual column has no value.
 */
static void
test_row_shape(void)
{
	WsTableSchema s = example();
	WsRowShape	shape;

	ws_schema_row_shape(&s, &shape);
	CHECK(shape.nkey == 1 && shape.nstored == 4);
	CHECK(shape.tags[0] == 7 && shape.tags[1] == 300 && shape.tags[2] == 5 && shape.tags[3] == 0);
	CHECK_EQ(ws_row_shape_at(&shape, 9), -1);
	ws_row_shape_free(&shape);

	/* a key on later attributes, out of their order: the key first, in its order */
	{
		uint64		key[] = {0, 300};
		WsColumn	cols[lengthof(example_cols)];

		memcpy(cols, example_cols, sizeof(cols));
		s.cols = cols;
		cols[1].not_null = true;
		cols[4].not_null = true;
		s.key = key;
		s.nkey = 2;
		CHECK(ws_schema_problem(&s) == NULL);
		ws_schema_row_shape(&s, &shape);
		CHECK(shape.nkey == 2 && shape.nstored == 4);
		CHECK(shape.tags[0] == 0 && shape.tags[1] == 300 && shape.tags[2] == 7 && shape.tags[3] == 5);
		ws_row_shape_free(&shape);
	}

	/* no key: every stored column, in attribute order */
	s.nkey = 0;
	s.key = NULL;
	ws_schema_row_shape(&s, &shape);
	CHECK(shape.nkey == 0 && shape.nstored == 4);
	CHECK(shape.tags[0] == 7 && shape.tags[1] == 300 && shape.tags[2] == 5 && shape.tags[3] == 0);
	ws_row_shape_free(&shape);
}

static void
shape_of_invalid(void *arg)
{
	WsColumn	cols[lengthof(example_cols)];
	WsTableSchema s = example();
	uint64		key[] = {8};
	WsRowShape	shape;

	memcpy(cols, example_cols, sizeof(cols));
	s.cols = cols;
	s.key = key;
	ws_schema_row_shape(&s, &shape);
}

/* A schema with a problem, a merge's not yet checked, gives no shape. */
static void
test_row_shape_invalid(void)
{
	expect_error(shape_of_invalid, NULL, ERRCODE_INTERNAL_ERROR, NULL);
}

/* The widest table there and back; names as they may be. */
static void
test_wide_and_odd(void)
{
	WsColumn   *cols = palloc0_array(WsColumn, WS_MAX_COLS);
	WsTableSchema s = {.name = "tab\there \"q\" \xc3\xa9",.persistence = WS_PERSISTENCE_PERMANENT,
	.ncols = WS_MAX_COLS,.cols = cols};
	WsTableSchema got;
	Buf			b = BUF_INIT;

	for (int i = 0; i < WS_MAX_COLS; i++)
	{
		cols[i].tag = (uint64) i * 1000;
		cols[i].name = psprintf("c\n%d", i);
		cols[i].type = "a type";
		cols[i].kind = WS_COL_STORED;
	}
	if (GetDatabaseEncoding() != PG_UTF8)
		s.name = "tab\there \"q\"";
	WsSchemaCodec->encode(&s, &b);
	WsSchemaCodec->decode(b.data, b.len, &got);
	CHECK_EQ(got.ncols, WS_MAX_COLS);
	CHECK(strcmp(got.name, s.name) == 0);
	CHECK(strcmp(got.cols[WS_MAX_COLS - 1].name, cols[WS_MAX_COLS - 1].name) == 0);
	ws_schema_free(&got);
	buf_free(&b);
}

typedef struct Invalid
{
	const char *problem;
	void		(*spoil) (WsTableSchema *s);
} Invalid;

static void
no_name(WsTableSchema *s)
{
	s->name = "";
}

static void
persistence(WsTableSchema *s)
{
	s->persistence = 't';
}

static void
col_no_name(WsTableSchema *s)
{
	s->cols[1].name = "";
}

static void
col_no_type(WsTableSchema *s)
{
	s->cols[1].type = "";
}

static void
col_kind(WsTableSchema *s)
{
	s->cols[1].kind = 'x';
}

static void
col_identity(WsTableSchema *s)
{
	s->cols[1].def = NULL;
	s->cols[1].identity = 'x';
}

static void
gen_no_expr(WsTableSchema *s)
{
	s->cols[2].def = NULL;
}

static void
gen_identity(WsTableSchema *s)
{
	s->cols[2].identity = WS_IDENTITY_ALWAYS;
}

static void
identity_default(WsTableSchema *s)
{
	s->cols[0].def = "1";
}

static void
same_tag(WsTableSchema *s)
{
	s->cols[1].tag = 7;
}

static void
same_name(WsTableSchema *s)
{
	s->cols[1].name = "id";
}

static void
key_missing(WsTableSchema *s)
{
	static uint64 k[] = {8};

	s->key = k;
}

static void
key_virtual(WsTableSchema *s)
{
	static uint64 k[] = {9};

	s->cols[3].not_null = true;
	s->key = k;
}

static void
key_nullable(WsTableSchema *s)
{
	static uint64 k[] = {300};

	s->key = k;
}

static void
key_twice(WsTableSchema *s)
{
	static uint64 k[] = {7, 7};

	s->key = k;
	s->nkey = 2;
}

static void
name_long(WsTableSchema *s)
{
	s->name = psprintf("%0*d", NAMEDATALEN, 0);
}

static void
col_name_long(WsTableSchema *s)
{
	s->cols[1].name = psprintf("%0*d", NAMEDATALEN, 0);
}

static void
identity_nullable(WsTableSchema *s)
{
	s->cols[0].not_null = false;
	s->nkey = 0;
}

static void
empty_default(WsTableSchema *s)
{
	s->cols[1].def = "";
}

static void
empty_collation(WsTableSchema *s)
{
	s->cols[1].collation = "";
}

static void
key_too_many(WsTableSchema *s)
{
	s->nkey = s->ncols + 1;
}

static const Invalid invalid[] = {
	{"a table's name too long", name_long},
	{"a column's name too long", col_name_long},
	{"an identity column that may be null", identity_nullable},
	{"an empty default", empty_default},
	{"an empty collation", empty_collation},
	{"a key of more columns than the table has", key_too_many},
	{"a table with no name", no_name},
	{"a table of no known persistence", persistence},
	{"a column with no name", col_no_name},
	{"a column with no type", col_no_type},
	{"a column of no known kind", col_kind},
	{"a column of no known identity", col_identity},
	{"a generated column with no expression", gen_no_expr},
	{"a generated column that is an identity", gen_identity},
	{"an identity column with a default", identity_default},
	{"two columns with one tag", same_tag},
	{"two columns with one name", same_name},
	{"a key with a column the table does not have", key_missing},
	{"a key with a virtual column", key_virtual},
	{"a key with a column that may be null", key_nullable},
	{"a key with a column twice", key_twice},
};

static WsTableSchema
spoiled(const Invalid *inv, WsColumn *cols)
{
	WsTableSchema s = example();

	memcpy(cols, example_cols, sizeof(example_cols));
	s.cols = cols;
	inv->spoil(&s);
	return s;
}

static void
encode_invalid(void *arg)
{
	WsColumn	cols[lengthof(example_cols)];
	WsTableSchema s = spoiled(arg, cols);
	Buf			b = BUF_INIT;

	WsSchemaCodec->encode(&s, &b);
}

/* What is invalid is named, and not written. */
static void
test_invalid(void)
{
	WsTableSchema s = example();

	CHECK(ws_schema_problem(&s) == NULL);
	for (size_t i = 0; i < lengthof(invalid); i++)
	{
		WsColumn	cols[lengthof(example_cols)];
		WsTableSchema bad = spoiled(&invalid[i], cols);
		const char *got = ws_schema_problem(&bad);

		CHECK(got != NULL && strcmp(got, invalid[i].problem) == 0);
		expect_error(encode_invalid, (void *) &invalid[i], ERRCODE_INTERNAL_ERROR, NULL);
	}
}

/*
 * Bytes refused as they should be: ERRCODE_DATA_CORRUPTED, for this reason
 * and no other (a cut short, a wrong length, would otherwise pass for it).
 */
static void
check_refused(const char *p, size_t len, const char *why)
{
	MemoryContext cxt = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	ErrorData  *edata = NULL;

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(cxt);
	PG_TRY();
	{
		WsTableSchema s;

		WsSchemaCodec->decode((const uint8 *) p, len, &s);
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(cxt);
		edata = CopyErrorData();
		FlushErrorState();
	}
	PG_END_TRY();
	RollbackAndReleaseCurrentSubTransaction();
	MemoryContextSwitchTo(cxt);
	CurrentResourceOwner = owner;
	if (edata == NULL)
		elog(ERROR, "no error, for: %s", why);
	if (edata->sqlerrcode != ERRCODE_DATA_CORRUPTED || strstr(edata->message, why) == NULL)
		elog(ERROR, "\"%s\", for: %s", edata->message, why);
}

/* Bytes that are no schema's are refused: however cut, or whatever is wrong. */
static void
test_malformed(void)
{
	WsTableSchema s = example();
	Buf			b = BUF_INIT;
	uint8	   *copy;
	static const struct
	{
		const char *p;
		size_t		len;
		const char *why;
	}			bad[] = {
		{"\x02" "\x01" "t" "p" "\x00" "\x00", 6, "a version not known"},
		{"\x01" "\x02" "t\0" "p" "\x00" "\x00", 7, "a NUL"},
		{"\x01" "\x09" "t", 3, "runs past its end"},
		{"\x01" "\x01" "t" "p" "\x01" "\x07" "\x02" "id" "\x01" "b" "s" "\x02" "\x00" "\x00" "\x00" "\x00" "\x00", 17, "neither"},
		{"\x01" "\x01" "t" "p" "\xc1\x0c" "\x00", 7, "more columns than a table can have"},
		{"\x01" "\x01" "t" "x" "\x00" "\x00", 6, "no known persistence"},
		{"\x01" "\x01" "t" "p" "\x00" "\x01" "\x07", 7, "a key of more columns"},
		/* a number not in its shortest form: one schema, other bytes */
		{"\x01" "\x81\x00" "t" "p" "\x00" "\x00", 7, "shortest form"},
		{"\x01" "\x01" "t" "p" "\x80\x00" "\x00", 7, "shortest form"},
		{"\x01" "\x01" "t" "p" "\x00" "\x80\x80\x00", 8, "shortest form"},
		/* a tag of bits past the 64th, dropped on reading */
		{"\x01" "\x01" "t" "p" "\x01" "\xff\xff\xff\xff\xff\xff\xff\xff\xff\x7f" "\x01" "c" "\x01" "b" "s" "\x00" "\x00" "\x00" "\x00" "\x00", 25, "shortest form"},
	};

	WsSchemaCodec->encode(&s, &b);

	/* cut anywhere */
	for (size_t n = 0; n < b.len; n++)
		check_refused((const char *) b.data, n, "a malformed table schema");
	/* a byte left over */
	copy = palloc(b.len + 1);
	memcpy(copy, b.data, b.len);
	copy[b.len] = 0;
	check_refused((const char *) copy, b.len + 1, "bytes left over");
	pfree(copy);
	buf_free(&b);

	for (size_t i = 0; i < lengthof(bad); i++)
		check_refused(bad[i].p, bad[i].len, bad[i].why);

	/* a name not in the database's encoding */
	if (GetDatabaseEncoding() == PG_UTF8)
		check_refused("\x01" "\x01" "\xff" "p" "\x00" "\x00", 6, "encoding");
}

static const ProllyUnitTest tests[] = {
	{"bytes", test_bytes},
	{"round_trip", test_round_trip},
	{"smallest", test_smallest},
	{"find", test_find},
	{"row_shape", test_row_shape},
	{"row_shape_invalid", test_row_shape_invalid},
	{"wide_and_odd", test_wide_and_odd},
	{"invalid", test_invalid},
	{"malformed", test_malformed},
};

const ProllyUnitSuite unit_ws_schema_suite = {"ws_schema", tests, lengthof(tests)};
