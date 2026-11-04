/*-------------------------------------------------------------------------
 *
 * unit_ws_role.c
 *	  Unit tests of the roles that travel with a versioned schema: their bytes,
 *	  what is wrong with one, their map on a root value and their statements.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/test/modules/test_prolly/unit_ws_role.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "engine_test_util.h"
#include "prolly/ws/ws_role.h"
#include "prolly/ws/ws_root.h"
#include "test_prolly.h"

static WsRoleMembership two[] = {
	{.role = "a",.admin = true,.inherit = false,.set = true},
	{.role = "b",.admin = false,.inherit = true,.set = false},
};

static WsRole
example(void)
{
	WsRole		r = {.name = "g",.no_inherit = true,.nmember_of = 2,.member_of = two};

	return r;
}

ENGINE_TEST(test_bytes)
{
	WsRole		r = example();
	WsRole		back;
	Buf			k = BUF_INIT;
	Buf			v = BUF_INIT;
	Buf			again = BUF_INIT;
	static const uint8 want[] = {0x01, 2, 1, 'a', 0x05, 1, 'b', 0x02};

	WsRoleCodec->encode_key(&r, &k);
	WsRoleCodec->encode_value(&r, &v);
	CHECK(k.len == 1 && k.data[0] == 'g');
	CHECK(v.len == sizeof(want) && memcmp(v.data, want, v.len) == 0);
	WsRoleCodec->decode(k.data, k.len, v.data, v.len, &back);
	CHECK(strcmp(back.name, "g") == 0);
	CHECK(back.no_inherit);
	CHECK(back.nmember_of == 2);
	CHECK(strcmp(back.member_of[1].role, "b") == 0);
	CHECK(back.member_of[0].admin && !back.member_of[0].inherit && back.member_of[0].set);
	CHECK(!back.member_of[1].admin && back.member_of[1].inherit && !back.member_of[1].set);
	WsRoleCodec->encode_value(&back, &again);
	CHECK(again.len == v.len && memcmp(again.data, v.data, v.len) == 0);
	ws_role_free(&back);
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
	WsRole		r;

	WsRoleCodec->decode((const uint8 *) b->key, strlen(b->key), b->val, b->vlen, &r);
}

ENGINE_TEST(test_malformed)
{
	/* (each is refused, as nothing here writes it) */
	static const uint8 empty[] = {0};
	static const uint8 flags[] = {0x02, 0};
	static const uint8 no_count[] = {0};
	static const uint8 long_count[] = {0, 0x80, 0x00};
	static const uint8 huge_count[] = {0, 0xff, 0xff, 0xff, 0xff, 0x0f};
	static const uint8 past[] = {0, 1, 9, 'a', 0};
	static const uint8 no_options[] = {0, 1, 1, 'a'};
	static const uint8 options[] = {0, 1, 1, 'a', 0x08};
	static const uint8 left[] = {0, 0, 0};
	static const uint8 nul[] = {0, 1, 2, 'a', 0, 0};
	static const uint8 not_utf8[] = {0, 1, 1, 0xff, 0};
	static const uint8 itself[] = {0, 1, 1, 'g', 0};
	static const uint8 order[] = {0, 2, 1, 'b', 0, 1, 'a', 0};
	static const uint8 twice[] = {0, 2, 1, 'a', 0, 1, 'a', 0};
	static const uint8 predefined[] = {0, 1, 9, 'p', 'g', '_', 'm', 'o', 'n', 'i', 't', 'o', 0};
	static const uint8 ok[] = {0, 0};
	Bad			bad[] = {
		{"g", empty, 0},
		{"g", flags, sizeof(flags)},
		{"g", no_count, sizeof(no_count)},
		{"g", long_count, sizeof(long_count)},
		{"g", huge_count, sizeof(huge_count)},
		{"g", past, sizeof(past)},
		{"g", no_options, sizeof(no_options)},
		{"g", options, sizeof(options)},
		{"g", left, sizeof(left)},
		{"g", nul, sizeof(nul)},
		{"g", not_utf8, sizeof(not_utf8)},
		{"g", itself, sizeof(itself)},
		{"g", order, sizeof(order)},
		{"g", twice, sizeof(twice)},
		{"g", predefined, sizeof(predefined)},
		{"", ok, sizeof(ok)},
		{"pg_monitor", ok, sizeof(ok)},
		{"public", ok, sizeof(ok)},
		{"none", ok, sizeof(ok)},
		{"\xff", ok, sizeof(ok)},
	};

	for (int i = 0; i < lengthof(bad); i++)
		expect_error(decode_bad, &bad[i], ERRCODE_DATA_CORRUPTED, NULL);
}

ENGINE_TEST(test_problems)
{
	char		toolong[NAMEDATALEN + 1];
	WsRole		r = example();
	WsRoleMembership reversed[] = {{.role = "b"}, {.role = "a"}};
	WsRoleMembership self[] = {{.role = "g"}};

	CHECK(ws_role_problem(&r) == NULL);
	memset(toolong, 'x', NAMEDATALEN);
	toolong[NAMEDATALEN] = '\0';
	r.name = toolong;
	CHECK(ws_role_problem(&r) != NULL);
	r.name = "pg_foo";
	CHECK(ws_role_problem(&r) != NULL);
	r = example();
	r.member_of = reversed;
	CHECK(ws_role_problem(&r) != NULL);
	r.member_of = self;
	r.nmember_of = 1;
	CHECK(ws_role_problem(&r) != NULL);
	/* (a name only like a predefined one's is not one) */
	r = example();
	r.name = "pgfoo";
	CHECK(ws_role_problem(&r) == NULL);
}

static bool
count_table(void *arg, const char *name, const VcTable *entry)
{
	(*(int *) arg)++;
	return true;
}

typedef struct TwoRoles
{
	ProllyCtx  *ctx;
	WsRole		roles[2];
} TwoRoles;

static void
map_twice(void *arg)
{
	TwoRoles   *t = arg;
	PHash		map;

	ws_role_map(t->ctx, t->roles, 2, &map);
}

/* The map on a root value: one set of roles, one map, in any order. */
ENGINE_TEST(test_map)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &prolly_default_policy);
	WsRole		a = {.name = "a"};
	WsRole		g = example();
	WsRole		in_order[] = {a, g};
	WsRole		reversed[] = {g, a};
	PHash		rv = vc_root_empty(&ctx);
	PHash		empty_rv = rv;
	PHash		m1;
	PHash		m2;
	PHash		got;
	PHash		none;
	PHash		out;
	WsRole	   *list;
	int			n;

	ws_role_map(&ctx, in_order, 2, &m1);
	ws_role_map(&ctx, reversed, 2, &m2);
	CHECK(memcmp(m1.b, m2.b, PHASH_LEN) == 0);
	WsRoot->put_map(&ctx, &rv, WS_ROOT_ROLES, &m1, &rv);
	CHECK(WsRoot->get_map(&ctx, &rv, WS_ROOT_ROLES, &got));
	CHECK(memcmp(got.b, m1.b, PHASH_LEN) == 0);
	ws_role_list(&ctx, &got, &list, &n);
	CHECK(n == 2);
	CHECK(strcmp(list[0].name, "a") == 0);
	CHECK(strcmp(list[1].name, "g") == 0);
	CHECK(list[1].nmember_of == 2);
	ws_role_free(&list[0]);
	ws_role_free(&list[1]);
	pfree(list);

	/* (no roles, no entry: one state, one root value) */
	ws_role_map(&ctx, NULL, 0, &none);
	WsRoot->put_map(&ctx, &empty_rv, WS_ROOT_ROLES, &none, &out);
	CHECK(memcmp(out.b, empty_rv.b, PHASH_LEN) == 0);
	CHECK(!WsRoot->get_map(&ctx, &out, WS_ROOT_ROLES, &got));

	/* (a table's listing passes the map by) */
	{
		int			tables = 0;

		WsRoot->tables(&ctx, &rv, count_table, &tables);
		CHECK_EQ(tables, 0);
	}
	{
		TwoRoles	twice = {&ctx, {a, a}};

		expect_error(map_twice, &twice, ERRCODE_INTERNAL_ERROR, NULL);
	}
}

ENGINE_TEST(test_statements)
{
	WsRole		r = {.name = "ws role no such one"};
	WsRole		no_inherit = {.name = "ws role no such one",.no_inherit = true};
	WsRole		g = example();
	Buf			b = BUF_INIT;

	ws_role_make_statement(&r, &b);
	CHECK(b.len == strlen("CREATE ROLE \"ws role no such one\" NOLOGIN"));
	CHECK(memcmp(b.data, "CREATE ROLE \"ws role no such one\" NOLOGIN", b.len) == 0);
	b.len = 0;
	ws_role_make_statement(&no_inherit, &b);
	CHECK(b.len == strlen("CREATE ROLE \"ws role no such one\" NOLOGIN NOINHERIT"));
	CHECK(memcmp(b.data, "CREATE ROLE \"ws role no such one\" NOLOGIN NOINHERIT", b.len) == 0);
	b.len = 0;
	ws_role_grant_statement(&g, 0, &b);
	CHECK(b.len == strlen("GRANT a TO g WITH ADMIN TRUE, INHERIT FALSE, SET TRUE"));
	CHECK(memcmp(b.data, "GRANT a TO g WITH ADMIN TRUE, INHERIT FALSE, SET TRUE", b.len) == 0);
	b.len = 0;
	ws_role_grant_statement(&g, 1, &b);
	CHECK(b.len == strlen("GRANT b TO g WITH ADMIN FALSE, INHERIT TRUE, SET FALSE"));
	CHECK(memcmp(b.data, "GRANT b TO g WITH ADMIN FALSE, INHERIT TRUE, SET FALSE", b.len) == 0);
	buf_free(&b);
}

/* Names no text can get out of. */
ENGINE_TEST(test_statement_quoting)
{
	WsRole		r = {.name = "a\"; DROP ROLE x; --"};
	WsRoleMembership of[] = {{.role = "b\" TO c; --"}};
	Buf			b = BUF_INIT;
	static const char want[] = "CREATE ROLE \"a\"\"; DROP ROLE x; --\" NOLOGIN";
	static const char want_grant[] =
		"GRANT \"b\"\" TO c; --\" TO \"a\"\"; DROP ROLE x; --\" WITH ADMIN FALSE, INHERIT FALSE, SET FALSE";

	ws_role_make_statement(&r, &b);
	CHECK(b.len == strlen(want) && memcmp(b.data, want, b.len) == 0);
	b.len = 0;
	r.member_of = of;
	r.nmember_of = 1;
	ws_role_grant_statement(&r, 0, &b);
	CHECK(b.len == strlen(want_grant) && memcmp(b.data, want_grant, b.len) == 0);
	buf_free(&b);
}

static const ProllyUnitTest tests[] = {
	{"bytes", test_bytes},
	{"malformed", test_malformed},
	{"problems", test_problems},
	{"map", test_map},
	{"statements", test_statements},
	{"statement_quoting", test_statement_quoting},
};

const ProllyUnitSuite unit_ws_role_suite = {"ws_role", tests, lengthof(tests)};
