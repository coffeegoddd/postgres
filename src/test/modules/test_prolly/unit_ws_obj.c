/*-------------------------------------------------------------------------
 *
 * unit_ws_obj.c
 *	  Unit tests of a versioned schema's objects other than tables: their bytes,
 *	  what is wrong with one, their map, their order and their statements.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/test/modules/test_prolly/unit_ws_obj.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "engine_test_util.h"
#include "prolly/ws/ws_obj.h"
#include "prolly/ws/ws_root.h"
#include "test_prolly.h"

static char *one_dep[] = {"view/a"};
static WsObjectComment comments[] = {{.text = "on it"}, {.column = "c",.text = "on c"}};
static WsObjectGrant grants[] = {
	{.grantee = NULL,.privileges = "r"},
	{.grantee = "g",.grant_option = true,.privileges = "ar"},
	{.column = "c",.grantee = "g",.privileges = "r"},
};

static WsObject
example(void)
{
	WsObject	o = {.kind = WS_OBJ_VIEW,.name = "v",.definition = "AS SELECT 1 AS c",
		.ndeps = 1,.deps = one_dep,.owner = "g",.ncomments = 2,.comments = comments,
	.ngrants = 3,.grants = grants};

	return o;
}

static void
check_same(const WsObject *a, const WsObject *b)
{
	CHECK(a->kind == b->kind);
	CHECK(strcmp(a->name, b->name) == 0);
	CHECK(strcmp(a->definition, b->definition) == 0);
	CHECK_EQ(a->ndeps, b->ndeps);
	CHECK_EQ(a->ncomments, b->ncomments);
	CHECK_EQ(a->ngrants, b->ngrants);
}

ENGINE_TEST(test_bytes)
{
	WsObject	o = example();
	WsObject	back;
	Buf			k = BUF_INIT;
	Buf			v = BUF_INIT;
	Buf			again = BUF_INIT;

	WsObjectCodec->encode_key(&o, &k);
	WsObjectCodec->encode_value(&o, &v);
	CHECK(k.len == 6 && memcmp(k.data, "view/v", 6) == 0);
	WsObjectCodec->decode(k.data, k.len, v.data, v.len, &back);
	check_same(&o, &back);
	CHECK(strcmp(back.deps[0], "view/a") == 0);
	CHECK(strcmp(back.owner, "g") == 0);
	CHECK(back.comments[0].column == NULL && strcmp(back.comments[1].column, "c") == 0);
	CHECK(back.grants[0].grantee == NULL && back.grants[1].grant_option);
	CHECK(strcmp(back.grants[2].column, "c") == 0);
	WsObjectCodec->encode_value(&back, &again);
	CHECK(again.len == v.len && memcmp(again.data, v.data, v.len) == 0);
	ws_obj_free(&back);
	buf_free(&k);
	buf_free(&v);
	buf_free(&again);
}

typedef struct Bad
{
	const char *key;
	const uint8 *val;
	size_t		vlen;
} Bad;

static void
decode_bad(void *arg)
{
	Bad		   *b = arg;
	WsObject	o;

	WsObjectCodec->decode((const uint8 *) b->key, strlen(b->key), b->val, b->vlen, &o);
}

ENGINE_TEST(test_malformed)
{
#define D 1, 'x'				/* a definition */
	static const uint8 ok[] = {D, 0, 0, 0, 0};
	static const uint8 cut[] = {D, 0, 0, 0};
	static const uint8 left[] = {D, 0, 0, 0, 0, 0};
	static const uint8 long_num[] = {D, 0x80, 0x00, 0, 0, 0};
	static const uint8 huge[] = {D, 0xff, 0xff, 0xff, 0xff, 0x0f, 0, 0, 0};
	static const uint8 past[] = {9, 'x', 0, 0, 0, 0};
	static const uint8 nul[] = {2, 'x', 0, 0, 0, 0, 0};
	static const uint8 not_utf8[] = {1, 0xff, 0, 0, 0, 0};
	static const uint8 lone_at[] = {1, '@', 0, 0, 0, 0};
	static const uint8 dep_nothing[] = {D, 1, 1, 'q', 0, 0, 0};
	static const uint8 dep_kindless[] = {D, 1, 3, 'x', '/', 'a', 0, 0, 0};
	static const uint8 dep_self[] = {D, 1, 6, 'v', 'i', 'e', 'w', '/', 'v', 0, 0, 0};
	static const uint8 grant_option[] = {D, 0, 0, 0, 1, 0, 0, 2, 1, 'r'};
	static const uint8 privilege[] = {D, 0, 0, 0, 1, 0, 0, 0, 1, 'Z'};
	static const uint8 order[] = {D, 0, 0, 0, 1, 0, 0, 0, 2, 'r', 'a'};
	static const uint8 predefined[] = {D, 0, 9, 'p', 'g', '_', 'm', 'o', 'n', 'i', 't', 'o', 0, 0};
	Bad			bad[] = {
		{"view/v", cut, sizeof(cut)},
		{"view/v", left, sizeof(left)},
		{"view/v", long_num, sizeof(long_num)},
		{"view/v", huge, sizeof(huge)},
		{"view/v", past, sizeof(past)},
		{"view/v", nul, sizeof(nul)},
		{"view/v", not_utf8, sizeof(not_utf8)},
		{"view/v", lone_at, sizeof(lone_at)},
		{"view/v", dep_nothing, sizeof(dep_nothing)},
		{"view/v", dep_kindless, sizeof(dep_kindless)},
		{"view/v", dep_self, sizeof(dep_self)},
		{"view/v", grant_option, sizeof(grant_option)},
		{"view/v", privilege, sizeof(privilege)},
		{"view/v", order, sizeof(order)},
		{"view/v", predefined, sizeof(predefined)},
		{"v", ok, sizeof(ok)},
		{"table/v", ok, sizeof(ok)},
		{"view/", ok, sizeof(ok)},
		{"view/\xff", ok, sizeof(ok)},
	};
#undef D

	for (int i = 0; i < lengthof(bad); i++)
		expect_error(decode_bad, &bad[i], ERRCODE_DATA_CORRUPTED, NULL);
}

ENGINE_TEST(test_problems)
{
	WsObject	o = example();
	char	   *unordered[] = {"view/b", "view/a"};
	WsObjectComment two_mine[] = {{.text = "x"}, {.text = "y"}};
	WsObjectGrant twice[] = {{.grantee = "g",.privileges = "r"}, {.grantee = "g",.privileges = "a"}};

	CHECK(ws_obj_problem(&o) == NULL);
	o.definition = "a lone @";
	CHECK(ws_obj_problem(&o) != NULL);
	o = example();
	o.deps = unordered;
	o.ndeps = 2;
	CHECK(ws_obj_problem(&o) != NULL);
	o = example();
	o.comments = two_mine;
	CHECK(ws_obj_problem(&o) != NULL);
	o = example();
	o.grants = twice;
	o.ngrants = 2;
	CHECK(ws_obj_problem(&o) != NULL);
	o = example();
	o.owner = "pg_monitor";
	CHECK(ws_obj_problem(&o) != NULL);
}

static bool
count_table(void *arg, const char *name, const VcTable *entry)
{
	(*(int *) arg)++;
	return true;
}

/* The map on a root value: one set of objects, one map, in any order. */
ENGINE_TEST(test_map)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &prolly_default_policy);
	WsObject	a = {.kind = WS_OBJ_MATVIEW,.name = "m",.definition = "x"};
	WsObject	b = example();
	WsObject	in_order[] = {a, b};
	WsObject	reversed[] = {b, a};
	PHash		rv = vc_root_empty(&ctx);
	PHash		m1;
	PHash		m2;
	PHash		got;
	WsObject   *list;
	int			n;
	int			tables = 0;

	ws_obj_map(&ctx, in_order, 2, &m1);
	ws_obj_map(&ctx, reversed, 2, &m2);
	CHECK(memcmp(m1.b, m2.b, PHASH_LEN) == 0);
	WsRoot->put_map(&ctx, &rv, WS_ROOT_OBJECTS, &m1, &rv);
	CHECK(WsRoot->get_map(&ctx, &rv, WS_ROOT_OBJECTS, &got));
	ws_obj_list(&ctx, &got, &list, &n);
	CHECK_EQ(n, 2);
	check_same(&list[0], &a);
	check_same(&list[1], &b);
	ws_obj_free(&list[0]);
	ws_obj_free(&list[1]);
	WsRoot->tables(&ctx, &rv, count_table, &tables);
	CHECK_EQ(tables, 0);
}

static void
order_of(void *arg)
{
	WsObject   *objs = arg;
	int			order[3];

	ws_obj_restore_order(objs, 3, order);
}

ENGINE_TEST(test_restore_order)
{
	char	   *needs_c[] = {"view/c"};
	char	   *needs_a[] = {"view/a"};
	char	   *needs_b[] = {"view/b"};
	char	   *needs_none[] = {"view/z"};
	WsObject	chain[] = {
		{.kind = WS_OBJ_VIEW,.name = "a",.definition = "x",.ndeps = 1,.deps = needs_c},
		{.kind = WS_OBJ_VIEW,.name = "b",.definition = "x"},
		{.kind = WS_OBJ_VIEW,.name = "c",.definition = "x",.ndeps = 1,.deps = needs_b},
	};
	WsObject	cycle[] = {
		{.kind = WS_OBJ_VIEW,.name = "a",.definition = "x",.ndeps = 1,.deps = needs_c},
		{.kind = WS_OBJ_VIEW,.name = "b",.definition = "x",.ndeps = 1,.deps = needs_a},
		{.kind = WS_OBJ_VIEW,.name = "c",.definition = "x",.ndeps = 1,.deps = needs_b},
	};
	WsObject	missing[] = {
		{.kind = WS_OBJ_VIEW,.name = "a",.definition = "x"},
		{.kind = WS_OBJ_VIEW,.name = "b",.definition = "x",.ndeps = 1,.deps = needs_none},
		{.kind = WS_OBJ_VIEW,.name = "c",.definition = "x"},
	};
	int			order[3];

	ws_obj_restore_order(chain, 3, order);
	CHECK(order[0] == 1 && order[1] == 2 && order[2] == 0);
	expect_error(order_of, cycle, ERRCODE_DATA_CORRUPTED, NULL);
	expect_error(order_of, missing, ERRCODE_DATA_CORRUPTED, NULL);
}

ENGINE_TEST(test_statements)
{
	WsObject	v = example();
	WsObject	m = {.kind = WS_OBJ_MATVIEW,.name = "m",
		.definition = "AS SELECT 1 AS c WITH NO DATA"};
	WsObject	at = {.kind = WS_OBJ_VIEW,.name = "at",
		.definition = "AS SELECT 'a@@b'::text AS c"};
	Buf			b = BUF_INIT;
	static const char want[] = "CREATE VIEW s.at AS SELECT 'a@b'::text AS c";
	static const char want_m_made[] = "CREATE MATERIALIZED VIEW s.m AS SELECT 1 AS c WITH NO DATA";
	static const char want_comment[] = "COMMENT ON COLUMN s.v.c IS 'on c'";
	static const char want_grant[] = "GRANT SELECT (c) ON TABLE s.v TO g";
	static const char want_all[] = "GRANT INSERT, SELECT ON TABLE s.v TO g WITH GRANT OPTION";
	static const char want_owner[] = "ALTER VIEW s.v OWNER TO g";
	static const char want_m[] = "ALTER MATERIALIZED VIEW s.m OWNER TO g";

	ws_obj_create_statement(&m, "s", &b);
	CHECK(b.len == strlen(want_m_made) && memcmp(b.data, want_m_made, b.len) == 0);
	b.len = 0;
	ws_obj_create_statement(&at, "s", &b);
	CHECK(b.len == strlen(want) && memcmp(b.data, want, b.len) == 0);
	b.len = 0;
	ws_obj_comment_statement(&v, 1, "s", &b);
	CHECK(b.len == strlen(want_comment) && memcmp(b.data, want_comment, b.len) == 0);
	b.len = 0;
	ws_obj_grant_statement(&v, 2, "s", &b);
	CHECK(b.len == strlen(want_grant) && memcmp(b.data, want_grant, b.len) == 0);
	b.len = 0;
	ws_obj_grant_statement(&v, 1, "s", &b);
	CHECK(b.len == strlen(want_all) && memcmp(b.data, want_all, b.len) == 0);
	b.len = 0;
	ws_obj_owner_statement(&v, "s", &b);
	CHECK(b.len == strlen(want_owner) && memcmp(b.data, want_owner, b.len) == 0);
	b.len = 0;
	m.owner = "g";
	ws_obj_owner_statement(&m, "s", &b);
	CHECK(b.len == strlen(want_m) && memcmp(b.data, want_m, b.len) == 0);
	buf_free(&b);
}

static void
statement_of(void *arg)
{
	Buf			b = BUF_INIT;

	ws_obj_create_statement(arg, "s", &b);
}

/* A definition can make its object and nothing else, whatever a commit says. */
ENGINE_TEST(test_refused_statements)
{
	static const char *const views[] = {
		"not sql at all",
		"",
		"AS SELECT 1; DROP TABLE t",
		"(x) AS SELECT 1",
		"AS SELECT 1 WITH NO DATA",
		"(a int)",
	};
	static const char *const matviews[] = {
		"AS SELECT evil()",
		"AS SELECT evil() WITH DATA",
		"AS SELECT 1 WITH NO DATA; DROP TABLE t",
		"TABLESPACE t AS SELECT 1 WITH NO DATA",
		"USING a AS SELECT 1 WITH NO DATA",
		"(x) AS SELECT 1 WITH NO DATA",
	};

	for (int i = 0; i < lengthof(views); i++)
	{
		WsObject	o = {.kind = WS_OBJ_VIEW,.name = "v",.definition = (char *) views[i]};

		expect_error(statement_of, &o, ERRCODE_DATA_CORRUPTED, NULL);
	}
	for (int i = 0; i < lengthof(matviews); i++)
	{
		WsObject	o = {.kind = WS_OBJ_MATVIEW,.name = "v",.definition = (char *) matviews[i]};

		expect_error(statement_of, &o, ERRCODE_DATA_CORRUPTED, NULL);
	}
}

static const ProllyUnitTest tests[] = {
	{"bytes", test_bytes},
	{"malformed", test_malformed},
	{"problems", test_problems},
	{"map", test_map},
	{"restore_order", test_restore_order},
	{"statements", test_statements},
	{"refused_statements", test_refused_statements},
};

const ProllyUnitSuite unit_ws_obj_suite = {"ws_obj", tests, lengthof(tests)};
