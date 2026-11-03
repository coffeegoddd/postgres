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

#include "engine_test_util.h"
#include "prolly/ws/ws_schema.h"
#include "prolly/ws/ws_sql.h"
#include "test_prolly.h"

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
		"\x03" "pk" "\x01\x07";	/* key: its name, its columns */
	WsColumn	cols[] = {{.tag = 7,.name = "id",.type = "bigint",.kind = WS_COL_STORED,
		.not_null = true,.identity = WS_IDENTITY_ALWAYS}};
	WsTableSchema s = {.name = "t",.persistence = WS_PERSISTENCE_PERMANENT,.ncols = 1,
	.cols = cols,.key_name = "pk",.nkey = 1,.key = example_key};
	Buf			b = BUF_INIT;

	WsSchemaCodec->encode(&s, &b);
	CHECK_EQ(b.len, sizeof(want) - 1);
	CHECK(memcmp(b.data, want, b.len) == 0);
	buf_free(&b);

	/* a default of nothing and an empty one differ: its length plus one */
	cols[0].identity = WS_IDENTITY_NONE;
	cols[0].def = "";
	WsSchemaCodec->encode(&s, &b);
	CHECK_EQ(b.len, sizeof(want) - 1);
	CHECK_EQ(b.data[b.len - 7], 1);
	buf_free(&b);
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

static const Invalid invalid[] = {
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

typedef struct Decoding
{
	const uint8 *p;
	size_t		len;
} Decoding;

static void
decode_it(void *arg)
{
	Decoding   *d = arg;
	WsTableSchema s;

	WsSchemaCodec->decode(d->p, d->len, &s);
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
	}			bad[] = {
		/* an unknown version */
		{"\x02" "\x01" "t" "p" "\x00" "\x00" "\x00", 7},
		/* a NUL in the name */
		{"\x01" "\x02" "t\0" "p" "\x00" "\x00" "\x00", 8},
		/* a name past its end */
		{"\x01" "\x09" "t", 3},
		/* a not null that is neither */
		{"\x01" "\x01" "t" "p" "\x01" "\x07" "\x02" "id" "\x01" "b" "s" "\x02" "\x00" "\x00" "\x00" "\x00" "\x00", 17},
		/* more columns than a table has */
		{"\x01" "\x01" "t" "p" "\xc1\x0c" "\x00", 7},
		/* a schema with a problem: no persistence known */
		{"\x01" "\x01" "t" "x" "\x00" "\x00" "\x00", 7},
		/* a key of more columns than the table has */
		{"\x01" "\x01" "t" "p" "\x00" "\x02" "k" "\x01" "\x07", 9},
	};

	WsSchemaCodec->encode(&s, &b);

	/* cut anywhere */
	for (size_t n = 0; n < b.len; n++)
	{
		Decoding	d = {b.data, n};

		expect_error(decode_it, &d, ERRCODE_DATA_CORRUPTED, NULL);
	}
	/* a byte left over */
	copy = palloc(b.len + 1);
	memcpy(copy, b.data, b.len);
	copy[b.len] = 0;
	{
		Decoding	d = {copy, b.len + 1};

		expect_error(decode_it, &d, ERRCODE_DATA_CORRUPTED, NULL);
	}
	pfree(copy);
	buf_free(&b);

	for (size_t i = 0; i < lengthof(bad); i++)
	{
		Decoding	d = {(const uint8 *) bad[i].p, bad[i].len};

		expect_error(decode_it, &d, ERRCODE_DATA_CORRUPTED, NULL);
	}
}

/* The statement, to the byte: names quoted, each clause, references by name. */
static void
test_create_table(void)
{
	static const char want[] =
		"CREATE TABLE sch.t (\n"
		"    id bigint GENERATED ALWAYS AS IDENTITY NOT NULL,\n"
		"    v text COLLATE pg_catalog.\"C\" DEFAULT ''::text,\n"
		"    g bigint GENERATED ALWAYS AS ((id * 2)) STORED,\n"
		"    w bigint GENERATED ALWAYS AS ((id + 1)) VIRTUAL,\n"
		"    e integer DEFAULT 0,\n"
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
			   "    \"select\" integer DEFAULT (\"Odd Name\" @ 2),\n"
			   "    CONSTRAINT \"Odd Key\" PRIMARY KEY (\"Odd Name\")\n"
			   ")") != 0)
		elog(ERROR, "got: %s", b.data);
	buf_free(&b);

	/* renamed: the same stored text, the new name */
	cols[0].name = "renamed";
	ws_schema_create_table(&s, "x", &b);
	buf_append_byte(&b, 0);
	CHECK(strstr((char *) b.data, "DEFAULT (renamed @ 2)") != NULL);
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
	{"invalid", test_invalid},
	{"malformed", test_malformed},
	{"create_table", test_create_table},
	{"create_table_names", test_create_table_names},
	{"create_table_empty", test_create_table_empty},
	{"create_table_dangling", test_create_table_dangling},
	{"create_table_runs", test_create_table_runs},
};

const ProllyUnitSuite unit_ws_schema_suite = {"ws_schema", tests, lengthof(tests)};
