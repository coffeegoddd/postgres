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
#include "prolly/ws/ws_schema.h"
#include "prolly/ws/ws_sql.h"
#include "test_prolly.h"
#include "utils/guc.h"

/* t (id bigint PRIMARY KEY, v text COLLATE "C" DEFAULT '', g bigint GENERATED, w ...) */
static WsColumn example_cols[] = {
	{.tag = 7,.name = "id",.type = "bigint",.kind = WS_COL_STORED,.not_null = true,
	.identity = WS_IDENTITY_ALWAYS},
	{.tag = 300,.name = "v",.type = "text",.kind = WS_COL_STORED,.def = "''::text",
	.collation = "pg_catalog.\"C\""},
	{.tag = 5,.name = "g",.type = "bigint",.kind = WS_COL_GENERATED_STORED,.def = "(@{7} * 2)"},
	{.tag = 9,.name = "w",.type = "bigint",.kind = WS_COL_GENERATED_VIRTUAL,.def = "(@{7} + 1)"},
	{.tag = 11,.name = "e",.type = "integer",.kind = WS_COL_STORED,.def = "0"},
};

static uint64 example_key[] = {7};

static WsTableSchema
example(void)
{
	WsTableSchema s = {.name = "t",.ncols = lengthof(example_cols),.cols = example_cols,.key_name = "t_pkey",
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
		"\x01" "t"				/* name */
		"\x01"					/* columns */
		"\x07" "\x02" "id" "\x06" "bigint" "s" "\x01" "a" "\x00" "\x00"
		"\x02" "pk" "\x01\x07"	/* key: its name, its columns */
		"\x00"					/* constraints */
		"\x00";					/* indexes */
	WsColumn	cols[] = {{.tag = 7,.name = "id",.type = "bigint",.kind = WS_COL_STORED,
		.not_null = true,.identity = WS_IDENTITY_ALWAYS}};
	WsTableSchema s = {.name = "t",.ncols = 1,
	.cols = cols,.key_name = "pk",.nkey = 1,.key = example_key};
	Buf			b = BUF_INIT;

	WsSchemaCodec->encode(&s, &b);
	CHECK_EQ(b.len, sizeof(want) - 1);
	CHECK(memcmp(b.data, want, b.len) == 0);
	buf_free(&b);

	/* each kind of column, a default and a collation, a tag of two bytes */
	{
		static const char want2[] =
			"\x01" "\x01" "t" "\x03"
			"\x07" "\x01" "k" "\x01" "i" "s" "\x01" "d" "\x00" "\x00"
			"\xac\x02" "\x01" "v" "\x01" "t" "s" "\x00" "\x00" "\x03" "\'x\'" "\x03" "\"C\""
			"\x05" "\x01" "g" "\x01" "i" "g" "\x00" "\x00" "\x01" "1" "\x00"
			"\x02" "pk" "\x01\x07" "\x00" "\x00";
		WsColumn	c2[] = {
			{.tag = 7,.name = "k",.type = "i",.kind = WS_COL_STORED,.not_null = true,
			.identity = WS_IDENTITY_BY_DEFAULT},
			{.tag = 300,.name = "v",.type = "t",.kind = WS_COL_STORED,.def = "'x'",.collation = "\"C\""},
			{.tag = 5,.name = "g",.type = "i",.kind = WS_COL_GENERATED_STORED,.def = "1"},
		};
		WsTableSchema s2 = {.name = "t",.ncols = 3,
		.cols = c2,.key_name = "pk",.nkey = 1,.key = example_key};

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
	CHECK(strcmp(got.name, "t") == 0);
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
	CHECK(got.nkey == 1 && got.key[0] == 7 && strcmp(got.key_name, "t_pkey") == 0);

	/* written again, the same bytes */
	WsSchemaCodec->encode(&got, &b);
	CHECK(a.len == b.len && memcmp(a.data, b.data, a.len) == 0);
	ws_schema_free(&got);
	buf_free(&a);
	buf_free(&b);
}

/* A table with no columns and no key is one. */
static void
test_smallest(void)
{
	WsTableSchema s = {.name = "empty"};
	WsTableSchema got;
	WsRowShape	shape;
	Buf			b = BUF_INIT;

	WsSchemaCodec->encode(&s, &b);
	WsSchemaCodec->decode(b.data, b.len, &got);
	CHECK(got.ncols == 0 && got.nkey == 0);
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
	CHECK(ws_schema_column(&s, 11) == &s.cols[4]);
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
	CHECK(shape.tags[0] == 7 && shape.tags[1] == 300 && shape.tags[2] == 5 && shape.tags[3] == 11);
	CHECK_EQ(ws_row_shape_at(&shape, 9), -1);
	ws_row_shape_free(&shape);

	/* a key on later attributes, out of their order: the key first, in its order */
	{
		uint64		key[] = {11, 300};
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
		CHECK(shape.tags[0] == 11 && shape.tags[1] == 300 && shape.tags[2] == 7 && shape.tags[3] == 5);
		ws_row_shape_free(&shape);
	}

	/* no key: every stored column, in attribute order */
	s.nkey = 0;
	s.key = NULL;
	s.key_name = NULL;
	ws_schema_row_shape(&s, &shape);
	CHECK(shape.nkey == 0 && shape.nstored == 4);
	CHECK(shape.tags[0] == 7 && shape.tags[1] == 300 && shape.tags[2] == 5 && shape.tags[3] == 11);
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
	WsTableSchema s = {.name = "tab\there \"q\" \xc3\xa9",.ncols = WS_MAX_COLS,.cols = cols};
	WsTableSchema got;
	Buf			b = BUF_INIT;

	for (int i = 0; i < WS_MAX_COLS; i++)
	{
		cols[i].tag = (uint64) (i + 1) * 1000;
		cols[i].name = psprintf("c\n%d", i);
		cols[i].type = "a type";
		cols[i].kind = WS_COL_STORED;
	}
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
col_no_tag(WsTableSchema *s)
{
	s->cols[0].tag = 0;
}

static void
no_name(WsTableSchema *s)
{
	s->name = "";
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
key_no_name(WsTableSchema *s)
{
	s->key_name = NULL;
}

static void
name_no_key(WsTableSchema *s)
{
	s->nkey = 0;
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
lone_at(WsTableSchema *s)
{
	s->cols[1].def = "a @ b";
}

static void
tag_leading_zero(WsTableSchema *s)
{
	s->cols[2].def = "(@{07} * 2)";
}

static void
key_too_many(WsTableSchema *s)
{
	s->nkey = s->ncols + 1;
}

static const Invalid invalid[] = {
	{"a column with no tag", col_no_tag},
	{"a table's name too long", name_long},
	{"a column's name too long", col_name_long},
	{"an identity column that may be null", identity_nullable},
	{"an empty default", empty_default},
	{"an empty collation", empty_collation},
	{"a key of more columns than the table has", key_too_many},
	{"an expression with malformed column references", lone_at},
	{"an expression with malformed column references", tag_leading_zero},
	{"a table with no name", no_name},
	{"a column with no name", col_no_name},
	{"a column with no type", col_no_type},
	{"a column of no known kind", col_kind},
	{"a column of no known identity", col_identity},
	{"a generated column with no expression", gen_no_expr},
	{"a generated column that is an identity", gen_identity},
	{"an identity column with a default", identity_default},
	{"two columns with one tag", same_tag},
	{"two columns with one name", same_name},
	{"a key with no name", key_no_name},
	{"a key's name with no key", name_no_key},
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
		{"\x02" "\x01" "t" "\x00" "\x00" "\x00" "\x00" "\x00", 8, "a version not known"},
		{"\x01" "\x02" "t\0" "\x00" "\x00" "\x00" "\x00" "\x00", 9, "a NUL"},
		{"\x01" "\x09" "t", 3, "runs past its end"},
		{"\x01" "\x01" "t" "\x01" "\x07" "\x02" "id" "\x01" "b" "s" "\x02" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00", 19, "neither"},
		{"\x01" "\x01" "t" "\xc1\x0c" "\x00" "\x00" "\x00", 8, "more columns than a table can have"},
		{"\x01" "\x01" "t" "\x00" "\x02" "k" "\x01" "\x07" "\x00" "\x00", 10, "a key of more columns"},
		/* constraints */
		{"\x01" "\x01" "t" "\x01" "\x07" "\x01" "a" "\x01" "i" "s" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x01" "\x01" "c" "c" "\x08" "\x01" "x" "\x00", 24, "flags not known"},
		{"\x01" "\x01" "t" "\x01" "\x07" "\x01" "a" "\x01" "i" "s" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x01" "\x01" "c" "z" "\x00" "\x01" "\x07" "\x00", 24, "no known kind"},
		{"\x01" "\x01" "t" "\x01" "\x07" "\x01" "a" "\x01" "i" "s" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x7f" "\x01" "\x00", 19, "more constraints than bytes"},
		{"\x01" "\x01" "t" "\x01" "\x07" "\x01" "a" "\x01" "i" "s" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x01" "\x01" "c" "u" "\x00" "\x02" "\x07" "\x07" "\x00", 25, "more columns than the table has"},
		{"\x01" "\x01" "t" "\x01" "\x07" "\x01" "a" "\x01" "i" "s" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x01" "\x01" "c" "u" "\x00" "\x01" "\x09" "\x00", 24, "does not have"},
		{"\x01" "\x01" "t" "\x01" "\x07" "\x01" "a" "\x01" "i" "s" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x01" "\x01" "c" "c" "\x00" "\x01" "@" "\x00", 24, "malformed"},
		/* indexes */
		{"\x01" "\x01" "t" "\x01" "\x07" "\x01" "a" "\x01" "i" "s" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x01" "\x01" "x" "\x01" "m" "\x04" "\x01" "\x07" "\x00" "\x00" "d" "d" "\x00" "\x00", 31, "index's flags not known"},
		{"\x01" "\x01" "t" "\x01" "\x07" "\x01" "a" "\x01" "i" "s" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x7f", 18, "more indexes than bytes"},
		{"\x01" "\x01" "t" "\x01" "\x07" "\x01" "a" "\x01" "i" "s" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x01" "\x01" "x" "\x01" "m" "\x00" "\xff\x01" "\x00" "\x00" "\x00", 27, "too many keys"},
		{"\x01" "\x01" "t" "\x01" "\x07" "\x01" "a" "\x01" "i" "s" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x01" "\x01" "x" "\x01" "m" "\x00" "\x01" "\x09" "\x00" "\x00" "d" "d" "\x00" "\x00", 31, "does not have"},
		{"\x01" "\x01" "t" "\x01" "\x07" "\x01" "a" "\x01" "i" "s" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x01" "\x01" "x" "\x01" "m" "\x00" "\x01" "\x07" "\x00" "\x00" "z" "d" "\x00" "\x00", 31, "no known order"},
		/* a number not in its shortest form: one schema, other bytes */
		{"\x01" "\x81\x00" "t" "\x00" "\x00" "\x00" "\x00" "\x00", 9, "shortest form"},
		{"\x01" "\x01" "t" "\x80\x00" "\x00" "\x00" "\x00" "\x00", 9, "shortest form"},
		{"\x01" "\x01" "t" "\x00" "\x00" "\x80\x80\x00" "\x00" "\x00", 10, "shortest form"},
		/* a tag of bits past the 64th, dropped on reading */
		{"\x01" "\x01" "t" "\x01" "\xff\xff\xff\xff\xff\xff\xff\xff\xff\x7f" "\x01" "c" "\x01" "b" "s" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00", 27, "shortest form"},
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

	/* a name not UTF8, whatever the database's encoding */
	check_refused("\x01" "\x01" "\xff" "\x00" "\x00" "\x00", 6, "not UTF8");
}

/* The statement, to the byte: names quoted, each clause, references by name. */
static void
test_create_table(void)
{
	static const char want[] =
		"CREATE TABLE sch.t (\n"
		"    id bigint GENERATED ALWAYS AS IDENTITY NOT NULL,\n"
		"    v text COLLATE pg_catalog.\"C\" DEFAULT (''::text),\n"
		"    g bigint GENERATED ALWAYS AS ((id * 2)) STORED,\n"
		"    w bigint GENERATED ALWAYS AS ((id + 1)) VIRTUAL,\n"
		"    e integer DEFAULT (0),\n"
		"    CONSTRAINT t_pkey PRIMARY KEY (id)\n"
		")";
	WsTableSchema s = example();
	Buf			b = BUF_INIT;

	ws_schema_create_table(&s, "sch", &b);
	buf_append_byte(&b, 0);
	if (strcmp((char *) b.data, want) != 0)
		elog(ERROR, "got: %s", b.data);
	buf_free(&b);
}

/* A name that needs quoting is; a column renamed is followed by reference. */
static void
test_create_table_names(void)
{
	WsColumn	cols[] = {
		{.tag = 1,.name = "Odd Name",.type = "integer",.kind = WS_COL_STORED,.not_null = true},
		{.tag = 2,.name = "select",.type = "integer",.kind = WS_COL_STORED,.def = "(@{1} @@ 2)"},
	};
	uint64		key[] = {1};
	WsTableSchema s = {.name = "Odd Table",.ncols = 2,
	.cols = cols,.key_name = "Odd Key",.nkey = 1,.key = key};
	Buf			b = BUF_INIT;

	ws_schema_create_table(&s, "Odd Schema", &b);
	buf_append_byte(&b, 0);
	if (strcmp((char *) b.data,
			   "CREATE TABLE \"Odd Schema\".\"Odd Table\" (\n"
			   "    \"Odd Name\" integer NOT NULL,\n"
			   "    \"select\" integer DEFAULT ((\"Odd Name\" @ 2)),\n"
			   "    CONSTRAINT \"Odd Key\" PRIMARY KEY (\"Odd Name\")\n"
			   ")") != 0)
		elog(ERROR, "got: %s", b.data);
	buf_free(&b);

	/* renamed: the same stored text, the new name */
	cols[0].name = "renamed";
	ws_schema_create_table(&s, "x", &b);
	buf_append_byte(&b, 0);
	CHECK(strstr((char *) b.data, "DEFAULT ((renamed @ 2))") != NULL);
	buf_free(&b);
}

/* The smallest statement: no columns, no key. */
static void
test_create_table_empty(void)
{
	WsTableSchema s = {.name = "e"};
	Buf			b = BUF_INIT;

	ws_schema_create_table(&s, "x", &b);
	buf_append_byte(&b, 0);
	CHECK(strcmp((char *) b.data, "CREATE TABLE x.e ()") == 0);
	buf_free(&b);
}

static void
create_dangling(void *arg)
{
	WsColumn	cols[] = {{.tag = 1,.name = "a",.type = "integer",.kind = WS_COL_STORED,
	.def = "(@{9} + 1)"}};
	WsTableSchema s = {.name = "t",.ncols = 1,.cols = cols};
	Buf			b = BUF_INIT;

	ws_schema_create_table(&s, "x", &b);
}

static void
test_create_table_dangling(void)
{
	/* (a problem with the record, now, before any statement is written) */
	expect_error(create_dangling, NULL, ERRCODE_INTERNAL_ERROR, NULL);
}

typedef struct Injection
{
	const char *type;
	const char *collation;
	const char *def;
	bool		not_null;
} Injection;

static void
create_injected(void *arg)
{
	Injection  *in = arg;
	WsColumn	cols[] = {{.tag = 1,.name = "a",.type = (char *) in->type,.kind = WS_COL_STORED,
	.collation = (char *) in->collation,.def = (char *) in->def,.not_null = in->not_null}};
	WsTableSchema s = {.name = "t",.ncols = 1,.cols = cols};
	Buf			b = BUF_INIT;

	ws_schema_create_table(&s, "x", &b);
}

/*
 * A commit may have come from anywhere.  Text that would make the statement
 * anything but what the record says, by closing what it is put in, by a
 * comment that hides what follows it, or by a clause of its own, is refused
 * as corruption: no statement is written with it.
 */
static void
test_create_table_injection(void)
{
	static const Injection bad[] = {
		/* a type that goes on */
		{.type = "int); DROP TABLE x.y; --"},
		{.type = "int, b int"},
		{.type = "int NOT NULL"},
		{.type = "int COLLATE \"C\""},
		{.type = "setof integer"},
		/* a type that creates what the record does not have */
		{.type = "serial"},
		{.type = "pg_catalog.bigserial"},
		/* a collation that goes on, spaces or none */
		{.type = "text", .collation = "\"C\"; DROP TABLE x.y"},
		{.type = "text", .collation = "\"C\" NOT NULL"},
		{.type = "text", .collation = "ucs_basic);DROP/**/TABLE/**/x.y;CREATE/**/TABLE/**/x.z(a/**/int"},
		{.type = "text", .collation = "C,b/**/int"},
		/* a comment in the type hides the default's quote: its string runs */
		{.type = "integer --", .def = "'\n); DROP TABLE x.y; CREATE TABLE x.z (a int --'"},
		/* an expression that goes on */
		{.type = "integer", .def = "1); DROP TABLE x.y; SELECT (1"},
		{.type = "integer", .def = "1), b integer DEFAULT (2"},
		{.type = "integer", .def = "1) NOT NULL DEFAULT (2"},
		{.type = "integer", .def = "1) CHECK (false"},
		{.type = "integer", .def = "1); --"},
		/* a comment in the type, and an expression other than the record reads */
		{.type = "text --", .def = "'harmless\nDEFAULT (pg_read_file('/etc/passwd')) --'"},
		/* a collation not in its one form, or cut short */
		{.type = "text", .collation = " \"C\""},
		{.type = "text", .collation = "pg_catalog . \"C\""},
		{.type = "text", .collation = "12345678901234567890123456789012345678901234567890123456789012345678"},
		/* a clause standing in for another */
		{.type = "integer --", .not_null = true, .def = "1\nNOT NULL NOT NULL --"},
		{.type = "integer", .not_null = true, .def = "1) NOT NULL NO INHERIT --"},
		/* the text's fault, by each class a parser raises */
		{.type = "float(0)"},
		{.type = "integer", .def = "1::float(0)"},
		{.type = "text", .def = "E'\\u12'"},
		{.type = "text", .def = "E'\\xFF'"},
		{.type = "integer", .def = "UNIQUE (SELECT 1)"},
		/* a record's text no expression, the statement's a clean default */
		{.type = "integer --", .def = "1) FROM pg_authid WHERE (true /*\nDEFAULT (1) --*/"},
		{.type = "integer --", .def = "1) /*\nDEFAULT (1) --*/; --"},
		{.type = "integer --", .def = "1) AS foo /*\nDEFAULT (1) --*/; --"},
	};
	static const Injection good[] = {
		{.type = "character varying(10)"},
		{.type = "numeric(10,2)[]"},
		{.type = "\"My Schema\".\"My Type\""},
		{.type = "timestamp(3) with time zone"},
		{.type = "text", .collation = "pg_catalog.\"C\""},
		{.type = "integer", .def = "(1 + 2)"},
		{.type = "text", .def = "'); DROP TABLE x.y; --'::text"},
		{.type = "text", .def = "'x'::text COLLATE \"C\""},
		{.type = "integer", .def = "CASE WHEN true THEN 1 ELSE 2 END"},
		{.type = "integer", .def = "1 --\n"},
	};

	for (size_t i = 0; i < lengthof(bad); i++)
		expect_error(create_injected, (void *) &bad[i], ERRCODE_DATA_CORRUPTED, NULL);
	for (size_t i = 0; i < lengthof(good); i++)
		create_injected((void *) &good[i]);
}

static void
create_unchecked(void *arg)
{
	WsColumn	cols[] = {{.tag = 1,.name = "a",.type = "integer",.kind = WS_COL_STORED}};
	uint64		key[] = {9};
	WsTableSchema s = {.name = "t",.ncols = 1,
	.cols = cols,.key_name = "k",.nkey = 1,.key = key};
	Buf			b = BUF_INIT;

	ws_schema_create_table(&s, "x", &b);
}

/* (a generated column whose clause another stands in for) */
static void
generated_replaced(void *arg)
{
	WsColumn	cols[] = {{.tag = 1,.name = "a",.type = "integer --",.not_null = true,
	.kind = WS_COL_GENERATED_STORED,.def = "1\nNOT NULL NOT NULL --"}};
	WsTableSchema s = {.name = "t",.ncols = 1,.cols = cols};
	Buf			b = BUF_INIT;

	ws_schema_create_table(&s, "x", &b);
}

/*
 * A clause on a column whose record has no expression: a string opened in
 * one column's text swallows the next's line, so that the clause it opened is
 * the next column's.  Refused for that, by the message: the record has nothing
 * to compare it with.
 */
static void
check_stray_clause(const char *def, const char *why)
{
	WsColumn	cols[] = {
		{.tag = 1,.name = "a",.type = "integer --",.kind = WS_COL_STORED,.def = (char *) def},
		{.tag = 2,.name = "b",.type = "integer --')",.kind = WS_COL_STORED},
	};
	WsTableSchema s = {.name = "t",.ncols = 2,.cols = cols};
	MemoryContext cxt = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	ErrorData  *edata = NULL;
	Buf			b = BUF_INIT;

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(cxt);
	PG_TRY();
	{
		ws_schema_create_table(&s, "x", &b);
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
		elog(ERROR, "not refused for %s: %s", why, edata->message);
	FreeErrorData(edata);
}

static void
test_create_table_stray_clause(void)
{
	check_stray_clause("1 /*\nDEFAULT (1), b integer DEFAULT ('x --*/",
					   "a default the column does not have");
	check_stray_clause("1 /*\nDEFAULT (1), b integer GENERATED ALWAYS AS ('x --*/",
					   "a column's generation");
}

/* A check and a unique constraint: the record, its bytes and its statement. */
static void
test_constraints(void)
{
	WsColumn	cols[] = {
		{.tag = 7,.name = "a",.type = "integer",.kind = WS_COL_STORED},
		{.tag = 8,.name = "b",.type = "integer",.kind = WS_COL_STORED},
	};
	uint64		both[] = {8, 7};
	WsConstraint cons[] = {
		{.name = "Odd Check",.kind = WS_CONSTRAINT_CHECK,.expr = "(@{7} > @{8})"},
		{.name = "u",.kind = WS_CONSTRAINT_UNIQUE,.ncols = 2,.cols = both,
		.nulls_not_distinct = true,.deferrable = true,.initdeferred = true},
	};
	WsTableSchema s = {.name = "t",.ncols = 2,.cols = cols,.nconstraints = 2,.constraints = cons};
	static const char want[] =
		"\x01" "\x01" "t" "\x02"
		"\x07" "\x01" "a" "\x07" "integer" "s" "\x00" "\x00" "\x00" "\x00"
		"\x08" "\x01" "b" "\x07" "integer" "s" "\x00" "\x00" "\x00" "\x00"
		"\x00" "\x00"			/* no key */
		"\x02"					/* constraints */
		"\x09" "Odd Check" "c" "\x00" "\x0d" "(@{7} > @{8})"
		"\x01" "u" "u" "\x07" "\x02\x08\x07"
		"\x00";					/* indexes */
	Buf			b = BUF_INIT;
	Buf			sql = BUF_INIT;
	WsTableSchema got;

	WsSchemaCodec->encode(&s, &b);
	CHECK_EQ(b.len, sizeof(want) - 1);
	CHECK(memcmp(b.data, want, b.len) == 0);
	WsSchemaCodec->decode(b.data, b.len, &got);
	CHECK_EQ(got.nconstraints, 2);
	CHECK(strcmp(got.constraints[0].expr, "(@{7} > @{8})") == 0);
	CHECK(got.constraints[1].cols[0] == 8 && got.constraints[1].initdeferred);
	ws_schema_free(&got);

	ws_schema_create_table(&s, "x", &sql);
	buf_append_byte(&sql, 0);
	CHECK(strcmp((char *) sql.data,
				 "CREATE TABLE x.t (\n"
				 "    a integer,\n"
				 "    b integer,\n"
				 "    CONSTRAINT \"Odd Check\" CHECK ((a > b)),\n"
				 "    CONSTRAINT u UNIQUE NULLS NOT DISTINCT (b, a) DEFERRABLE INITIALLY DEFERRED\n"
				 ")") == 0);
	buf_free(&sql);
	buf_free(&b);
}

static void
constraint_invalid(const WsConstraint *cons, int n, const char *why)
{
	WsColumn	cols[] = {
		{.tag = 7,.name = "a",.type = "integer",.kind = WS_COL_STORED},
		{.tag = 8,.name = "v",.type = "integer",.kind = WS_COL_GENERATED_VIRTUAL,.def = "1"},
	};
	WsTableSchema s = {.name = "t",.ncols = 2,.cols = cols,.key_name = NULL,
	.nconstraints = n,.constraints = (WsConstraint *) cons};
	const char *problem = ws_schema_problem(&s);

	if (problem == NULL || strstr(problem, why) == NULL)
		elog(ERROR, "for \"%s\": %s", why, problem ? problem : "no problem found");
}

static void
test_constraint_problems(void)
{
	uint64		a[] = {7};
	uint64		twice[] = {7, 7};
	uint64		none[] = {9};
	uint64		virt[] = {8};

#define CHECK_C {.name = "c",.kind = WS_CONSTRAINT_CHECK,.expr = "true"}
#define BAD(why, ...) \
	do { \
		const WsConstraint one[] = {__VA_ARGS__}; \
		constraint_invalid(one, lengthof(one), why); \
	} while (0)
	BAD("no name", {.kind = WS_CONSTRAINT_CHECK,.expr = "true"});
	BAD("name too long", {.name = "1234567890123456789012345678901234567890123456789012345678901234",
		.kind = WS_CONSTRAINT_CHECK,.expr = "true"});
	BAD("does not have", {.name = "x",.kind = WS_CONSTRAINT_CHECK,.expr = "(@{99} > 0)"});
	BAD("more than the table has", {.name = "x",.kind = WS_CONSTRAINT_UNIQUE,.ncols = 3,.cols = a});
	BAD("alike", {.name = "x",.kind = WS_CONSTRAINT_UNIQUE,.ncols = 1,.cols = a},
		{.name = "y",.kind = WS_CONSTRAINT_UNIQUE,.ncols = 1,.cols = a});
	BAD("no known kind", {.name = "x",.kind = 'z'});
	BAD("not in order", {.name = "d",.kind = WS_CONSTRAINT_CHECK,.expr = "true"}, CHECK_C);
	BAD("not in order", CHECK_C, CHECK_C);
	BAD("no expression", {.name = "x",.kind = WS_CONSTRAINT_CHECK});
	BAD("malformed", {.name = "x",.kind = WS_CONSTRAINT_CHECK,.expr = "@"});
	BAD("unique constraint's fields", {.name = "x",.kind = WS_CONSTRAINT_CHECK,.expr = "true",.deferrable = true});
	BAD("with an expression", {.name = "x",.kind = WS_CONSTRAINT_UNIQUE,.expr = "true",.ncols = 1,.cols = a});
	BAD("no columns", {.name = "x",.kind = WS_CONSTRAINT_UNIQUE});
	BAD("does not have", {.name = "x",.kind = WS_CONSTRAINT_UNIQUE,.ncols = 1,.cols = none});
	BAD("column twice", {.name = "x",.kind = WS_CONSTRAINT_UNIQUE,.ncols = 2,.cols = twice});
	BAD("virtual", {.name = "x",.kind = WS_CONSTRAINT_UNIQUE,.ncols = 1,.cols = virt});
	BAD("not deferrable", {.name = "x",.kind = WS_CONSTRAINT_UNIQUE,.ncols = 1,.cols = a,.initdeferred = true});
#undef BAD
#undef CHECK_C
	{
		WsColumn	cols[] = {{.tag = 7,.name = "a",.type = "integer",.kind = WS_COL_STORED,.not_null = true}};
		WsConstraint same = {.name = "k",.kind = WS_CONSTRAINT_CHECK,.expr = "true"};
		WsTableSchema s = {.name = "t",.ncols = 1,.cols = cols,.key_name = "k",.nkey = 1,.key = a,
		.nconstraints = 1,.constraints = &same};

		CHECK(strstr(ws_schema_problem(&s), "named as the key") != NULL);
		same.name = "u";
		same.kind = WS_CONSTRAINT_UNIQUE;
		same.expr = NULL;
		same.ncols = 1;
		same.cols = a;
		CHECK(strstr(ws_schema_problem(&s), "alike the key") != NULL);
		/* (not alike: CREATE TABLE makes both) */
		same.deferrable = true;
		CHECK(ws_schema_problem(&s) == NULL);
		same.deferrable = false;
		same.nulls_not_distinct = true;
		CHECK(ws_schema_problem(&s) == NULL);
	}
	{
		uint64		ab[] = {7, 8};
		uint64		ba[] = {8, 7};
		WsColumn	cols[] = {
			{.tag = 7,.name = "a",.type = "integer",.kind = WS_COL_STORED},
			{.tag = 8,.name = "b",.type = "integer",.kind = WS_COL_STORED},
		};
		WsConstraint two[] = {
			{.name = "x",.kind = WS_CONSTRAINT_UNIQUE,.ncols = 2,.cols = ab},
			{.name = "y",.kind = WS_CONSTRAINT_UNIQUE,.ncols = 2,.cols = ba},
		};
		WsTableSchema s = {.name = "t",.ncols = 2,.cols = cols,.nconstraints = 2,.constraints = two};

		/* (another order is another index) */
		CHECK(ws_schema_problem(&s) == NULL);
	}
}

/* What a check's text could add to the statement is refused. */
static void
check_injection(void *arg)
{
	WsColumn	cols[] = {{.tag = 7,.name = "a",.type = "integer",.kind = WS_COL_STORED}};
	WsConstraint con = {.name = "c",.kind = WS_CONSTRAINT_CHECK,.expr = (char *) arg};
	WsTableSchema s = {.name = "t",.ncols = 1,.cols = cols,.nconstraints = 1,.constraints = &con};
	Buf			b = BUF_INIT;

	ws_schema_create_table(&s, "x", &b);
}

/*
 * A comment opened in one check's text and closed in the next: the statement
 * holds one check where the record has two.
 */
static void
check_spanning(void *arg)
{
	WsColumn	cols[] = {{.tag = 7,.name = "a",.type = "integer",.kind = WS_COL_STORED}};
	WsConstraint cons[] = {
		{.name = "c",.kind = WS_CONSTRAINT_CHECK,.expr = "true /*"},
		{.name = "d",.kind = WS_CONSTRAINT_CHECK,.expr = "*/ AND false"},
	};
	WsTableSchema s = {.name = "t",.ncols = 1,.cols = cols,.nconstraints = 2,.constraints = cons};
	Buf			b = BUF_INIT;

	ws_schema_create_table(&s, "x", &b);
}

static void
test_constraint_injection(void)
{
	static const char *const bad[] = {
		"true) NO INHERIT --",
		"true) NOT VALID --",
		"true) NOT ENFORCED --",
		"true), CONSTRAINT d CHECK (false",
		"true), UNIQUE (a",
		"true) ENFORCED --",
	};

	for (int i = 0; i < lengthof(bad); i++)
		expect_error(check_injection, (void *) bad[i], ERRCODE_DATA_CORRUPTED, NULL);
	expect_error(check_spanning, NULL, ERRCODE_DATA_CORRUPTED, NULL);
}

/* An index: the record, its bytes and its statement. */
static void
test_indexes(void)
{
	WsColumn	cols[] = {
		{.tag = 7,.name = "a",.type = "text",.kind = WS_COL_STORED},
		{.tag = 8,.name = "Odd",.type = "integer",.kind = WS_COL_STORED},
	};
	WsIndexKey keys[] = {
		{.tag = 7,.collation = "pg_catalog.\"C\"",.opclass = "pg_catalog.text_pattern_ops",
		.order = WS_INDEX_ORDER_DESC,.nulls = WS_INDEX_NULLS_LAST},
		{.expr = "lower(@{7})",.order = WS_INDEX_ORDER_DEFAULT,.nulls = WS_INDEX_NULLS_DEFAULT},
	};
	uint64		inc[] = {8};
	WsIndex		ix = {.name = "Odd Idx",.method = "btree",.unique = true,.nulls_not_distinct = true,
		.nkeys = 2,.keys = keys,.ninclude = 1,.include = inc,.predicate = "(@{8} > 0)"};
	WsTableSchema s = {.name = "t",.ncols = 2,.cols = cols,.nindexes = 1,.indexes = &ix};
	Buf			b = BUF_INIT;
	Buf			sql = BUF_INIT;
	WsTableSchema got;

	WsSchemaCodec->encode(&s, &b);
	WsSchemaCodec->decode(b.data, b.len, &got);
	CHECK_EQ(got.nindexes, 1);
	CHECK(strcmp(got.indexes[0].keys[1].expr, "lower(@{7})") == 0);
	CHECK(got.indexes[0].keys[0].order == WS_INDEX_ORDER_DESC && got.indexes[0].unique);
	CHECK(got.indexes[0].include[0] == 8 && strcmp(got.indexes[0].predicate, "(@{8} > 0)") == 0);
	{
		Buf			again = BUF_INIT;

		WsSchemaCodec->encode(&got, &again);
		CHECK(again.len == b.len && memcmp(again.data, b.data, b.len) == 0);
		buf_free(&again);
	}
	ws_schema_free(&got);
	ws_schema_create_index(&s, 0, "x", &sql);
	buf_append_byte(&sql, 0);
	CHECK(strcmp((char *) sql.data,
				 "CREATE UNIQUE INDEX \"Odd Idx\" ON x.t USING btree "
				 "(a COLLATE pg_catalog.\"C\" pg_catalog.text_pattern_ops DESC NULLS LAST, (lower(a))) "
				 "INCLUDE (\"Odd\") NULLS NOT DISTINCT WHERE ((\"Odd\" > 0))") == 0);
	buf_free(&sql);
	buf_free(&b);
}

static void
index_invalid(WsIndex *ix, const char *why)
{
	WsColumn	cols[] = {{.tag = 7,.name = "a",.type = "text",.kind = WS_COL_STORED}};
	static uint64 tags[] = {7};
	WsConstraint con = {.name = "c",.kind = WS_CONSTRAINT_UNIQUE,.ncols = 1,.cols = tags};
	WsTableSchema s = {.name = "t",.ncols = 1,.cols = cols,.nconstraints = 1,.constraints = &con,
	.nindexes = 1,.indexes = ix};
	const char *problem = ws_schema_problem(&s);

	if (problem == NULL || strstr(problem, why) == NULL)
		elog(ERROR, "for \"%s\": %s", why, problem ? problem : "no problem found");
}

static void
test_index_problems(void)
{
	static const struct
	{
		const char *why;
		WsIndex		ix;
		WsIndexKey	key;
	}			cases[] = {
#define OK_KEY {.tag = 7,.order = WS_INDEX_ORDER_DEFAULT,.nulls = WS_INDEX_NULLS_DEFAULT}
		{"no name", {.method = "btree"}, OK_KEY},
		{"no method", {.name = "x"}, OK_KEY},
		{"too long", {.name = "1234567890123456789012345678901234567890123456789012345678901234",
		.method = "btree"}, OK_KEY},
		{"named as a unique constraint", {.name = "c",.method = "btree"}, OK_KEY},
		{"does not have", {.name = "x",.method = "btree",.predicate = "(@{9} > 0)"}, OK_KEY},
		{"malformed", {.name = "x",.method = "btree",.predicate = "@"}, OK_KEY},
		{"no known order", {.name = "x",.method = "btree"}, {.tag = 7,.order = 'a',.nulls = 'd'}},
		{"its default order of nulls", {.name = "x",.method = "btree"}, {.tag = 7,.order = 'd',.nulls = 'l'}},
		{"its default order of nulls", {.name = "x",.method = "btree"}, {.tag = 7,.order = 'D',.nulls = 'f'}},
		{"not unique", {.name = "x",.method = "btree",.nulls_not_distinct = true}, OK_KEY},
		{"neither", {.name = "x",.method = "btree"}, {.order = 'd',.nulls = 'd'}},
		{"neither", {.name = "x",.method = "btree"}, {.tag = 7,.expr = "true",.order = 'd',.nulls = 'd'}},
		{"does not have", {.name = "x",.method = "btree"}, {.tag = 9,.order = 'd',.nulls = 'd'}},
		{"does not have", {.name = "x",.method = "btree"}, {.expr = "(@{9})",.order = 'd',.nulls = 'd'}},
		{"malformed", {.name = "x",.method = "btree"}, {.expr = "@",.order = 'd',.nulls = 'd'}},
		{"no known order", {.name = "x",.method = "btree"}, {.tag = 7,.order = 'z',.nulls = 'd'}},
		{"order of nulls", {.name = "x",.method = "btree"}, {.tag = 7,.order = 'd',.nulls = 'z'}},
		{"empty collation", {.name = "x",.method = "btree"}, {.tag = 7,.collation = "",.order = 'd',.nulls = 'd'}},
#undef OK_KEY
	};

	for (int i = 0; i < lengthof(cases); i++)
	{
		WsIndex		ix = cases[i].ix;
		WsIndexKey	key = cases[i].key;

		ix.nkeys = 1;
		ix.keys = &key;
		index_invalid(&ix, cases[i].why);
	}
	{
		WsIndex		ix = {.name = "x",.method = "btree"};

		index_invalid(&ix, "no keys");
	}
	{
		WsIndexKey	key = {.tag = 7,.order = 'd',.nulls = 'd'};
		uint64		none[] = {9};
		WsIndex		ix = {.name = "x",.method = "btree",.nkeys = 1,.keys = &key,.ninclude = 1,.include = none};

		index_invalid(&ix, "including a column the table does not have");
		ix.ninclude = INDEX_MAX_KEYS;
		index_invalid(&ix, "too many columns");
	}
	{
		/* a key's name; indexes out of order; a check's name is free to take */
		WsColumn	cols[] = {{.tag = 7,.name = "a",.type = "text",.kind = WS_COL_STORED,.not_null = true}};
		uint64		a[] = {7};
		WsIndexKey	key = {.tag = 7,.order = 'd',.nulls = 'd'};
		WsConstraint check = {.name = "c",.kind = WS_CONSTRAINT_CHECK,.expr = "true"};
		WsIndex		two[] = {
			{.name = "y",.method = "btree",.nkeys = 1,.keys = &key},
			{.name = "x",.method = "btree",.nkeys = 1,.keys = &key},
		};
		WsIndex		named = {.name = "k",.method = "btree",.nkeys = 1,.keys = &key};
		WsIndex		as_check = {.name = "c",.method = "btree",.nkeys = 1,.keys = &key};
		WsTableSchema s = {.name = "t",.ncols = 1,.cols = cols,.nindexes = 2,.indexes = two};

		CHECK(strstr(ws_schema_problem(&s), "not in order") != NULL);
		s.nindexes = 1;
		s.indexes = &named;
		s.key_name = "k";
		s.nkey = 1;
		s.key = a;
		CHECK(strstr(ws_schema_problem(&s), "named as the key") != NULL);
		s.key_name = NULL;
		s.nkey = 0;
		s.key = NULL;
		s.indexes = &as_check;
		s.nconstraints = 1;
		s.constraints = &check;
		CHECK(ws_schema_problem(&s) == NULL);
	}
}

/* What an index's text could add to the statement is refused. */
typedef struct IndexAttack
{
	const char *method;
	const char *expr;
	const char *collation;
	const char *opclass;
	const char *predicate;
} IndexAttack;

static void
index_injected(void *arg)
{
	const IndexAttack *in = arg;
	WsColumn	cols[] = {{.tag = 7,.name = "a",.type = "text",.kind = WS_COL_STORED}};
	WsIndexKey	key = {.tag = in->expr ? 0 : 7,.expr = (char *) in->expr,.collation = (char *) in->collation,
	.opclass = (char *) in->opclass,.order = 'd',.nulls = 'd'};
	WsIndex		ix = {.name = "i",.method = (char *) (in->method ? in->method : "btree"),.nkeys = 1,.keys = &key,
	.predicate = (char *) in->predicate};
	WsTableSchema s = {.name = "t",.ncols = 1,.cols = cols,.nindexes = 1,.indexes = &ix};
	Buf			b = BUF_INIT;

	ws_schema_create_index(&s, 0, "x", &b);
}

static void
index_spanned(void *arg)
{
	WsColumn	cols[] = {
		{.tag = 7,.name = "a",.type = "text",.kind = WS_COL_STORED},
		{.tag = 8,.name = "b",.type = "text",.kind = WS_COL_STORED},
	};
	WsIndexKey	keys[] = {
		{.expr = "1) --",.order = 'd',.nulls = 'd'},
		{.expr = (char *) arg,.order = 'd',.nulls = 'd'},
	};
	uint64		b_tag[] = {8};
	const char *expr = arg;
	WsIndex		ix = {.name = "i",.method = "btree",.nkeys = 2,.keys = keys};
	WsTableSchema s = {.name = "t",.ncols = 2,.cols = cols,.nindexes = 1,.indexes = &ix};
	Buf			b = BUF_INIT;

	/* (INCLUDE compared whole: the count alone would refuse it otherwise) */
	if (strstr(expr, "INCLUDE (b DESC)") != NULL)
	{
		ix.ninclude = 1;
		ix.include = b_tag;
	}
	ws_schema_create_index(&s, 0, "x", &b);
}

static void
test_index_injection(void)
{
	static const IndexAttack bad[] = {
		{.expr = "1), (2"},
		{.expr = "1)) WITH (fillfactor=10) --"},
		{.expr = "@{7})) INCLUDE (a) --"},
		{.expr = "@{7})) WHERE (true) --"},
		{.collation = " \"C\""},
		{.collation = "\"C\" "},
		{.collation = "pg_catalog . \"C\""},
		{.collation = "12345678901234567890123456789012345678901234567890123456789012345678"},
		{.collation = "\"C\" text_ops"},
		{.collation = "\"C\" DESC"},
		{.collation = "\"C\", a"},
		{.collation = "\"C\") WITH (fillfactor = 10) --"},
		{.opclass = "text_ops DESC"},
		{.opclass = "text_ops (siglen = 1)"},
		{.opclass = "text_ops, a"},
		{.opclass = "text_ops) WHERE (true) --"},
		{.predicate = "true) TABLESPACE x --"},
		{.predicate = "true) FROM y WHERE (true"},
		{.predicate = "true) AS foo --"},
	};
	static const char *const same[] = {"true) AND (false", "true) --"};

	for (int i = 0; i < lengthof(bad); i++)
		expect_error(index_injected, (void *) &bad[i], ERRCODE_DATA_CORRUPTED, NULL);

	/*
	 * A comment in one key's expression and a newline in the next's, so that
	 * the statement says more after them; each alone is the expression it was.
	 */
	{
		static const char *const clause[] = {
			"2 /*\n, ((2))) INCLUDE (b DESC) -- */",
			"2 /*\n, ((2))) INCLUDE (b) -- */",
			"2 /*\n, ((2))) WITH (fillfactor = 10) -- */",
			"2 /*\n, ((2))) TABLESPACE x -- */",
			"2 /*\n, ((2))) WHERE (true) -- */",
			"2 /*\n, ((2))) NULLS NOT DISTINCT -- */",
		};

		for (int i = 0; i < lengthof(clause); i++)
			expect_error(index_spanned, (void *) clause[i], ERRCODE_DATA_CORRUPTED, NULL);
	}

	/* (these make what they say: an expression's tree is what is compared) */
	for (int i = 0; i < lengthof(same); i++)
	{
		Buf			b = BUF_INIT;
		WsColumn	cols[] = {{.tag = 7,.name = "a",.type = "text",.kind = WS_COL_STORED}};
		WsIndexKey	key = {.tag = 7,.order = 'd',.nulls = 'd'};
		WsIndex		ix = {.name = "i",.method = "btree",.nkeys = 1,.keys = &key,
		.predicate = (char *) same[i]};
		WsTableSchema s = {.name = "t",.ncols = 1,.cols = cols,.nindexes = 1,.indexes = &ix};

		ws_schema_create_index(&s, 0, "x", &b);
		buf_append_byte(&b, 0);
		CHECK(strstr((char *) b.data, same[i]) != NULL);
		buf_free(&b);
	}
}

/* An expression's references take the names the record has now. */
static void
test_create_table_renamed(void)
{
	WsColumn	cols[] = {
		{.tag = 7,.name = "now_named",.type = "integer",.kind = WS_COL_STORED},
		{.tag = 8,.name = "b",.type = "integer",.kind = WS_COL_GENERATED_STORED,.def = "(@{7} * 2)"},
	};
	WsTableSchema s = {.name = "t",.ncols = 2,.cols = cols};
	Buf			b = BUF_INIT;

	ws_schema_create_table(&s, "x", &b);
	buf_append_byte(&b, 0);
	CHECK(strstr((char *) b.data, "(now_named * 2)") != NULL);
	buf_free(&b);
}

/* A collation is checked as names, under whichever quoting the session has. */
static void
test_create_table_quoting(void)
{
	int			level = NewGUCNestLevel();
	WsColumn	cols[] = {{.tag = 1,.name = "a",.type = "text",.kind = WS_COL_STORED,
	.collation = "pg_catalog.\"C\""}};
	WsTableSchema s = {.name = "t",.ncols = 1,.cols = cols};
	Buf			b = BUF_INIT;

	(void) set_config_option("quote_all_identifiers", "on", PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);
	ws_schema_create_table(&s, "x", &b);
	buf_free(&b);
	AtEOXact_GUC(true, level);
}

/* A schema with a problem, a merge's not yet checked, gives no statement. */
static void
test_create_table_unchecked(void)
{
	expect_error(create_unchecked, NULL, ERRCODE_INTERNAL_ERROR, NULL);
	expect_error(generated_replaced, NULL, ERRCODE_DATA_CORRUPTED, NULL);
}

/* (what failed, in the log: always false, for a CHECK to fail on) */
static bool
elog_msg(const char *msg)
{
	elog(WARNING, "got: %s", msg);
	return false;
}

static char *
one(const char *sql)
{
	WsSqlRows	rows;
	char	   *v;

	WsSql->run(sql, 0, NULL, &rows);
	CHECK(rows.nrows == 1 && rows.ncols == 1);
	v = pstrdup(ws_sql_value(&rows, 0, 0));
	ws_sql_rows_free(&rows);
	return v;
}

/* (the statement run in a schema of its own, the catalogs asked what it made) */
static void
create_and_read(void *arg)
{
	WsColumn	cols[] = {
		{.tag = 1,.name = "id",.type = "bigint",.kind = WS_COL_STORED,.not_null = true,
		.identity = WS_IDENTITY_BY_DEFAULT},
		{.tag = 2,.name = "v",.type = "character varying(10)",.kind = WS_COL_STORED,
		.def = "'x'::character varying",.collation = "pg_catalog.\"C\""},
		{.tag = 3,.name = "g",.type = "bigint",.kind = WS_COL_GENERATED_STORED,.def = "(@{1} * 2)"},
		{.tag = 4,.name = "w",.type = "bigint",.kind = WS_COL_GENERATED_VIRTUAL,.def = "(@{1} + 1)"},
		{.tag = 5,.name = "n",.type = "integer",.kind = WS_COL_STORED,.not_null = true},
	};
	uint64		key[] = {5, 1};
	WsTableSchema s = {.name = "made",.ncols = lengthof(cols),
	.cols = cols,.key_name = "made_pk",.nkey = 2,.key = key};
	Buf			b = BUF_INIT;
	int			level = WsSql->begin();
	WsSqlRows	rows;
	char	   *got;

	WsSql->run("CREATE SCHEMA ws_made", 0, NULL, &rows);
	ws_sql_rows_free(&rows);
	ws_schema_create_table(&s, "ws_made", &b);
	buf_append_byte(&b, 0);
	WsSql->run((char *) b.data, 0, NULL, &rows);
	ws_sql_rows_free(&rows);
	buf_free(&b);

	got = one("SELECT string_agg(attname || '|' || format_type(atttypid, atttypmod) || '|' || "
			  "attnotnull || '|' || attidentity::text || '|' || attgenerated::text || '|' || "
			  "coalesce((SELECT pg_get_expr(adbin, adrelid) FROM pg_attrdef "
			  " WHERE adrelid = attrelid AND adnum = attnum), '') || '|' || "
			  "coalesce((SELECT collname::text FROM pg_collation c WHERE c.oid = attcollation), ''), "
			  "';' ORDER BY attnum) FROM pg_attribute "
			  "WHERE attrelid = 'ws_made.made'::regclass AND attnum > 0");
	if (strcmp(got,
			   "id|bigint|true|d|||;"
			   "v|character varying(10)|false|||'x'::character varying|C;"
			   "g|bigint|false||s|(id * 2)|;"
			   "w|bigint|false||v|(id + 1)|;"
			   "n|integer|true||||") != 0)
		elog(ERROR, "got: %s", got);
	CHECK(strcmp(one("SELECT pg_get_constraintdef(oid) FROM pg_constraint "
					 "WHERE conname = 'made_pk' AND conrelid = 'ws_made.made'::regclass"),
				 "PRIMARY KEY (n, id)") == 0);
	WsSql->end(level);
	/* (all of it undone) */
	WsSql->run("DO $$ BEGIN RAISE EXCEPTION 'done'; END $$", 0, NULL, &rows);
}

/* What the statement makes is what the record says, by the catalogs. */
static void
test_create_table_runs(void)
{
	WsSqlFailure f;

	CHECK(!WsSql->attempt_fn(create_and_read, NULL, &f));
	CHECK(strcmp(f.message, "done") == 0 || elog_msg(f.message));
	ws_sql_failure_free(&f);
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
	{"create_table", test_create_table},
	{"create_table_names", test_create_table_names},
	{"create_table_empty", test_create_table_empty},
	{"create_table_dangling", test_create_table_dangling},
	{"create_table_injection", test_create_table_injection},
	{"create_table_unchecked", test_create_table_unchecked},
	{"create_table_quoting", test_create_table_quoting},
	{"constraints", test_constraints},
	{"constraint_problems", test_constraint_problems},
	{"constraint_injection", test_constraint_injection},
	{"indexes", test_indexes},
	{"index_problems", test_index_problems},
	{"index_injection", test_index_injection},
	{"create_table_renamed", test_create_table_renamed},
	{"create_table_stray_clause", test_create_table_stray_clause},
	{"create_table_runs", test_create_table_runs},
};

const ProllyUnitSuite unit_ws_schema_suite = {"ws_schema", tests, lengthof(tests)};
