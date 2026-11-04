/*-------------------------------------------------------------------------
 *
 * unit_ws_fk.c
 *	  Unit tests of a versioned schema's foreign keys: their bytes, what is
 *	  wrong with one, their map on a root value and their statement.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/test/modules/test_prolly/unit_ws_fk.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "engine_test_util.h"
#include "nodes/parsenodes.h"
#include "prolly/ws/ws_fk.h"
#include "prolly/ws/ws_root.h"
#include "test_prolly.h"

static uint64 two[] = {7, 8};
static uint64 ref_two[] = {300, 5};
static uint64 eight[] = {8};

static WsForeignKey
example(void)
{
	WsForeignKey fk = {.table = "c",.name = "k",.ref_table = "p",.ncols = 2,.cols = two,
		.ref_cols = ref_two,.match = FKCONSTR_MATCH_FULL,.on_update = FKCONSTR_ACTION_CASCADE,
		.on_delete = FKCONSTR_ACTION_SETNULL,.nset = 1,.set_cols = eight,
	.deferrable = true,.initdeferred = true,.not_valid = true};

	return fk;
}

/* A key's bytes are the format: changing them is changing every commit. */
static void
test_bytes(void)
{
	WsForeignKey fk = example();
	static const char want_key[] = "c\0k";
	static const char want_val[] =
		"\x01" "p" "f" "c" "n" "\x07"
		"\x02" "\x07\x08" "\xac\x02\x05"
		"\x01" "\x08";
	Buf			k = BUF_INIT;
	Buf			v = BUF_INIT;
	WsForeignKey got;

	WsFkCodec->encode_key(&fk, &k);
	WsFkCodec->encode_value(&fk, &v);
	CHECK_EQ(k.len, sizeof(want_key) - 1);
	CHECK(memcmp(k.data, want_key, k.len) == 0);
	CHECK_EQ(v.len, sizeof(want_val) - 1);
	CHECK(memcmp(v.data, want_val, v.len) == 0);
	WsFkCodec->decode(k.data, k.len, v.data, v.len, &got);
	CHECK(strcmp(got.table, "c") == 0 && strcmp(got.name, "k") == 0 && strcmp(got.ref_table, "p") == 0);
	CHECK(got.ncols == 2 && got.cols[1] == 8 && got.ref_cols[0] == 300);
	CHECK(got.match == FKCONSTR_MATCH_FULL && got.nset == 1 && got.set_cols[0] == 8);
	CHECK(got.deferrable && got.initdeferred && got.not_valid);
	ws_fk_free(&got);
	buf_free(&k);
	buf_free(&v);
}

typedef struct Bad
{
	const char *key;
	size_t		klen;
	const char *val;
	size_t		vlen;
} Bad;

static void
decode_bad(void *arg)
{
	const Bad  *b = arg;
	WsForeignKey fk;

	WsFkCodec->decode((const uint8 *) b->key, b->klen, (const uint8 *) b->val, b->vlen, &fk);
}

/* Only a key's own bytes are read as it. */
static void
test_malformed(void)
{
#define GOODV "\x01" "p" "s" "a" "a" "\x00" "\x01" "\x07" "\x05" "\x00"
#define BAD(k, v) {k, sizeof(k) - 1, v, sizeof(v) - 1}
	static const Bad bad[] = {
		BAD("c", GOODV),		/* no name */
		BAD("\xff\0k", GOODV),	/* not UTF8 */
		BAD("c\0k\0x", GOODV),	/* a NUL in a name */
		BAD("c\0", GOODV),		/* an empty name */
		BAD("c\0k", GOODV "\x00"),	/* bytes left over */
		BAD("c\0k", "\x01" "p" "s" "a" "a" "\x08" "\x01" "\x07" "\x05" "\x00"), /* flags */
		BAD("c\0k", "\x01" "p" "z" "a" "a" "\x00" "\x01" "\x07" "\x05" "\x00"), /* match */
		BAD("c\0k", "\x01" "p" "s" "z" "a" "\x00" "\x01" "\x07" "\x05" "\x00"), /* action */
		BAD("c\0k", "\x01" "p" "s" "a" "z" "\x00" "\x01" "\x07" "\x05" "\x00"), /* action */
		BAD("c\0k", "\x01" "p" "s" "a" "a" "\x00" "\x00" "\x00"),	/* no columns */
		BAD("c\0k", "\x01" "p" "s" "a" "a" "\x00" "\x01" "\x87\x00" "\x05" "\x00"), /* not shortest */
		BAD("c\0k", "\x01" "p" "s" "a" "a" "\x00" "\x7f"),	/* too many */
		BAD("c\0k", "\x09" "p"),	/* runs past */
		BAD("c\0k", "\x01" "p" "s" "a" "a" "\x00" "\x01" "\x07" "\x05" "\x01" "\x07"), /* set: no action */
		BAD("c\0k", "\x01" "p" "s" "a" "n" "\x00" "\x01" "\x07" "\x05" "\x01" "\x09"), /* set: not of it */
	};
#undef BAD
#undef GOODV

	for (int i = 0; i < lengthof(bad); i++)
		expect_error(decode_bad, (void *) &bad[i], ERRCODE_DATA_CORRUPTED, NULL);
}

static void
test_problems(void)
{
	WsForeignKey base = {.table = "c",.name = "k",.ref_table = "p",.ncols = 1,.cols = two,
		.ref_cols = ref_two,.match = FKCONSTR_MATCH_SIMPLE,.on_update = FKCONSTR_ACTION_NOACTION,
	.on_delete = FKCONSTR_ACTION_NOACTION};
	uint64		twice[] = {7, 7};
	uint64		zero[] = {0};
	WsForeignKey fk;

	CHECK(ws_fk_problem(&base) == NULL);
#define BAD(why, field, value) \
	do { \
		fk = base; \
		fk.field = value; \
		CHECK(ws_fk_problem(&fk) != NULL && strstr(ws_fk_problem(&fk), why) != NULL); \
	} while (0)
	BAD("no table", table, "");
	BAD("no name", name, "");
	BAD("to no table", ref_table, "");
	BAD("too long", name, "1234567890123456789012345678901234567890123456789012345678901234");
	BAD("no columns", ncols, 0);
	BAD("or too many", ncols, INDEX_MAX_KEYS + 1);
	BAD("no tag", cols, zero);
	BAD("no tag", ref_cols, zero);
	BAD("no known match", match, 'p');
	BAD("no known action", on_update, 'z');
	BAD("no known action", on_delete, 'z');
	BAD("initially deferred", initdeferred, true);
#undef BAD
	fk = base;
	fk.ncols = 2;
	fk.ref_cols = twice;
	CHECK(strstr(ws_fk_problem(&fk), "to a column twice") != NULL);
	fk = base;
	fk.nset = 1;
	fk.set_cols = eight;
	CHECK(strstr(ws_fk_problem(&fk), "no action setting") != NULL);
	fk.on_delete = FKCONSTR_ACTION_SETNULL;
	CHECK(strstr(ws_fk_problem(&fk), "not of it") != NULL);
	fk = base;
	fk.nset = 2;
	CHECK(strstr(ws_fk_problem(&fk), "more columns than it has") != NULL);
}

/* The map on a root value: one set of keys, one map, in any order. */
ENGINE_TEST(test_map)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &prolly_default_policy);
	WsForeignKey a = example();
	WsForeignKey b = example();
	WsForeignKey in_order[2];
	WsForeignKey reversed[2];
	PHash		m1;
	PHash		m2;
	PHash		rv = vc_root_empty(&ctx);
	PHash		got;
	WsForeignKey *list;
	int			n;

	b.name = "a key";
	in_order[0] = a;
	in_order[1] = b;
	reversed[0] = b;
	reversed[1] = a;
	ws_fk_map(&ctx, in_order, 2, &m1);
	ws_fk_map(&ctx, reversed, 2, &m2);
	CHECK(memcmp(m1.b, m2.b, PHASH_LEN) == 0);
	WsRoot->put_map(&ctx, &rv, WS_ROOT_FOREIGN_KEYS, &m1, &rv);
	CHECK(WsRoot->get_map(&ctx, &rv, WS_ROOT_FOREIGN_KEYS, &got));
	ws_fk_list(&ctx, &got, &list, &n);
	CHECK_EQ(n, 2);
	CHECK(strcmp(list[0].name, "a key") == 0 && strcmp(list[1].name, "k") == 0);
	ws_fk_free(&list[0]);
	ws_fk_free(&list[1]);
}

static WsColumn child_cols[] = {
	{.tag = 7,.name = "a",.type = "integer",.kind = WS_COL_STORED},
	{.tag = 8,.name = "Odd b",.type = "integer",.kind = WS_COL_STORED},
};
static WsColumn parent_cols[] = {
	{.tag = 300,.name = "x",.type = "integer",.kind = WS_COL_STORED},
	{.tag = 5,.name = "y",.type = "integer",.kind = WS_COL_STORED},
};

/* The statement, by the records' names: a column renamed is its new name. */
static void
test_statement(void)
{
	WsTableSchema c = {.name = "c",.ncols = 2,.cols = child_cols};
	WsTableSchema p = {.name = "p",.ncols = 2,.cols = parent_cols};
	WsForeignKey fk = example();
	Buf			b = BUF_INIT;

	ws_fk_add_statement(&fk, &c, &p, "x", &b);
	buf_append_byte(&b, 0);
	CHECK(strcmp((char *) b.data,
				 "ALTER TABLE x.c ADD CONSTRAINT k FOREIGN KEY (a, \"Odd b\") REFERENCES x.p (x, y) "
				 "MATCH FULL ON UPDATE CASCADE ON DELETE SET NULL (\"Odd b\") "
				 "DEFERRABLE INITIALLY DEFERRED NOT VALID") == 0);
	buf_free(&b);
}

static void
statement_missing(void *arg)
{
	WsTableSchema c = {.name = "c",.ncols = 2,.cols = child_cols};
	WsTableSchema p = {.name = "p",.ncols = 1,.cols = parent_cols};
	WsForeignKey fk = example();
	Buf			b = BUF_INIT;

	/* (p has no column 5) */
	ws_fk_add_statement(&fk, &c, &p, "x", &b);
}

/* A name of any characters is a name: nothing of it is the statement. */
static void
test_statement_quoting(void)
{
	WsColumn	odd[] = {{.tag = 7,.name = "a\"); DROP TABLE x.y; --",.type = "integer",.kind = WS_COL_STORED}};
	WsColumn	ref[] = {{.tag = 300,.name = "*/",.type = "integer",.kind = WS_COL_STORED}};
	WsTableSchema c = {.name = "c\"; --",.ncols = 1,.cols = odd};
	WsTableSchema p = {.name = "/* p",.ncols = 1,.cols = ref};
	uint64		tag7[] = {7};
	uint64		tag300[] = {300};
	WsForeignKey fk = {.table = "c\"; --",.name = "k\n--",.ref_table = "/* p",.ncols = 1,.cols = tag7,
		.ref_cols = tag300,.match = FKCONSTR_MATCH_SIMPLE,.on_update = FKCONSTR_ACTION_NOACTION,
	.on_delete = FKCONSTR_ACTION_NOACTION};
	Buf			b = BUF_INIT;

	ws_fk_add_statement(&fk, &c, &p, "x", &b);
	buf_free(&b);
	expect_error(statement_missing, NULL, ERRCODE_DATA_CORRUPTED, NULL);
}

static const ProllyUnitTest tests[] = {
	{"bytes", test_bytes},
	{"malformed", test_malformed},
	{"problems", test_problems},
	{"map", test_map},
	{"statement", test_statement},
	{"statement_quoting", test_statement_quoting},
};

const ProllyUnitSuite unit_ws_fk_suite = {"ws_fk", tests, lengthof(tests)};
