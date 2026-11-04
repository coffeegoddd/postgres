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
	{.tag = 0,.name = "e",.type = "integer",.kind = WS_COL_STORED,.def = "0"},
};

static uint64 example_key[] = {7};

static WsTableSchema
example(void)
{
	WsTableSchema s = {.name = "t",.persistence = WS_PERSISTENCE_PERMANENT,
		.ncols = lengthof(example_cols),.cols = example_cols,.key_name = "t_pkey",
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
		"\x02" "pk" "\x01\x07";	/* key: its name, its columns */
	WsColumn	cols[] = {{.tag = 7,.name = "id",.type = "bigint",.kind = WS_COL_STORED,
		.not_null = true,.identity = WS_IDENTITY_ALWAYS}};
	WsTableSchema s = {.name = "t",.persistence = WS_PERSISTENCE_PERMANENT,.ncols = 1,
	.cols = cols,.key_name = "pk",.nkey = 1,.key = example_key};
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
			"\x02" "pk" "\x01\x07";
		WsColumn	c2[] = {
			{.tag = 7,.name = "k",.type = "i",.kind = WS_COL_STORED,.not_null = true,
			.identity = WS_IDENTITY_BY_DEFAULT},
			{.tag = 300,.name = "v",.type = "t",.kind = WS_COL_STORED,.def = "'x'",.collation = "\"C\""},
			{.tag = 5,.name = "g",.type = "i",.kind = WS_COL_GENERATED_STORED,.def = "1"},
		};
		WsTableSchema s2 = {.name = "t",.persistence = WS_PERSISTENCE_UNLOGGED,.ncols = 3,
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
	CHECK(got.nkey == 1 && got.key[0] == 7 && strcmp(got.key_name, "t_pkey") == 0);

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
	s.key_name = NULL;
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
	{"a table's name too long", name_long},
	{"a column's name too long", col_name_long},
	{"an identity column that may be null", identity_nullable},
	{"an empty default", empty_default},
	{"an empty collation", empty_collation},
	{"a key of more columns than the table has", key_too_many},
	{"an expression with malformed column references", lone_at},
	{"an expression with malformed column references", tag_leading_zero},
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
		{"\x02" "\x01" "t" "p" "\x00" "\x00" "\x00", 7, "a version not known"},
		{"\x01" "\x02" "t\0" "p" "\x00" "\x00" "\x00", 8, "a NUL"},
		{"\x01" "\x09" "t", 3, "runs past its end"},
		{"\x01" "\x01" "t" "p" "\x01" "\x07" "\x02" "id" "\x01" "b" "s" "\x02" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00", 18, "neither"},
		{"\x01" "\x01" "t" "p" "\xc1\x0c" "\x00", 7, "more columns than a table can have"},
		{"\x01" "\x01" "t" "x" "\x00" "\x00" "\x00", 7, "no known persistence"},
		{"\x01" "\x01" "t" "p" "\x00" "\x02" "k" "\x01" "\x07", 9, "a key of more columns"},
		/* a number not in its shortest form: one schema, other bytes */
		{"\x01" "\x81\x00" "t" "p" "\x00" "\x00" "\x00", 8, "shortest form"},
		{"\x01" "\x01" "t" "p" "\x80\x00" "\x00" "\x00", 8, "shortest form"},
		{"\x01" "\x01" "t" "p" "\x00" "\x00" "\x80\x80\x00", 9, "shortest form"},
		/* a tag of bits past the 64th, dropped on reading */
		{"\x01" "\x01" "t" "p" "\x01" "\xff\xff\xff\xff\xff\xff\xff\xff\xff\x7f" "\x01" "c" "\x01" "b" "s" "\x00" "\x00" "\x00" "\x00" "\x00" "\x00", 26, "shortest form"},
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
	check_refused("\x01" "\x01" "\xff" "p" "\x00" "\x00" "\x00", 7, "not UTF8");
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
	WsTableSchema s = {.name = "Odd Table",.persistence = WS_PERSISTENCE_UNLOGGED,.ncols = 2,
	.cols = cols,.key_name = "Odd Key",.nkey = 1,.key = key};
	Buf			b = BUF_INIT;

	ws_schema_create_table(&s, "Odd Schema", &b);
	buf_append_byte(&b, 0);
	if (strcmp((char *) b.data,
			   "CREATE UNLOGGED TABLE \"Odd Schema\".\"Odd Table\" (\n"
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
	WsTableSchema s = {.name = "e",.persistence = WS_PERSISTENCE_PERMANENT};
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
	WsTableSchema s = {.name = "t",.persistence = WS_PERSISTENCE_PERMANENT,.ncols = 1,.cols = cols};
	Buf			b = BUF_INIT;

	ws_schema_create_table(&s, "x", &b);
}

static void
test_create_table_dangling(void)
{
	expect_error(create_dangling, NULL, ERRCODE_DATA_CORRUPTED, NULL);
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
	WsTableSchema s = {.name = "t",.persistence = WS_PERSISTENCE_PERMANENT,.ncols = 1,.cols = cols};
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
		/* a clause standing in for another */
		{.type = "integer --", .not_null = true, .def = "1\nNOT NULL NOT NULL --"},
		{.type = "integer", .not_null = true, .def = "1) NOT NULL NO INHERIT --"},
		/* the text's fault, by each class a parser raises */
		{.type = "float(0)"},
		{.type = "integer", .def = "1::float(0)"},
		{.type = "text", .def = "E'\\u12'"},
		{.type = "text", .def = "E'\\xFF'"},
		{.type = "integer UNIQUE (SELECT 1)"},
		{.type = "integer", .def = "UNIQUE (SELECT 1)"},
		/* a record's text no expression, the statement's a clean default */
		{.type = "integer --", .def = "1) FROM pg_authid WHERE (true /*\nDEFAULT (1) --*/"},
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
	WsTableSchema s = {.name = "t",.persistence = WS_PERSISTENCE_PERMANENT,.ncols = 1,
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
	WsTableSchema s = {.name = "t",.persistence = WS_PERSISTENCE_PERMANENT,.ncols = 1,.cols = cols};
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
	WsTableSchema s = {.name = "t",.persistence = WS_PERSISTENCE_PERMANENT,.ncols = 2,.cols = cols};
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

/* A collation is checked as names, under whichever quoting the session has. */
static void
test_create_table_quoting(void)
{
	int			level = NewGUCNestLevel();
	WsColumn	cols[] = {{.tag = 1,.name = "a",.type = "text",.kind = WS_COL_STORED,
	.collation = "pg_catalog.\"C\""}};
	WsTableSchema s = {.name = "t",.persistence = WS_PERSISTENCE_PERMANENT,.ncols = 1,.cols = cols};
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
	WsTableSchema s = {.name = "made",.persistence = WS_PERSISTENCE_UNLOGGED,.ncols = lengthof(cols),
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

	CHECK(strcmp(one("SELECT relpersistence FROM pg_class WHERE oid = 'ws_made.made'::regclass"),
				 "u") == 0);
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
	{"create_table_stray_clause", test_create_table_stray_clause},
	{"create_table_runs", test_create_table_runs},
};

const ProllyUnitSuite unit_ws_schema_suite = {"ws_schema", tests, lengthof(tests)};
