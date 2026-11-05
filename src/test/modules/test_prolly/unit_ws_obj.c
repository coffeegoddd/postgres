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
	{.grantee = "h",.grant_option = true,.privileges = "ar"},
	{.column = "c",.grantee = "h",.privileges = "r"},
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
	for (int i = 0; i < a->ndeps; i++)
		CHECK(strcmp(a->deps[i], b->deps[i]) == 0);
	CHECK_EQ(a->ncomments, b->ncomments);
	for (int i = 0; i < a->ncomments; i++)
		CHECK(strcmp(a->comments[i].text, b->comments[i].text) == 0);
	CHECK_EQ(a->ngrants, b->ngrants);
	for (int i = 0; i < a->ngrants; i++)
		CHECK(strcmp(a->grants[i].privileges, b->grants[i].privileges) == 0);
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
	{
		WsObject	f = {.kind = WS_OBJ_FUNCTION,.name = "f",.args = "integer, mood",.definition = "x",
		.after_tables = true,.public_revoked = true};
		WsObject	f0 = {.kind = WS_OBJ_PROCEDURE,.name = "a(b",.args = "",.definition = "x"};
		WsObject	fback;
		Buf			fk = BUF_INIT;
		Buf			fv = BUF_INIT;

		WsObjectCodec->encode_key(&f, &fk);
		WsObjectCodec->encode_value(&f, &fv);
		CHECK(fk.len == strlen("function/f(integer, mood)"));
		WsObjectCodec->decode(fk.data, fk.len, fv.data, fv.len, &fback);
		CHECK(strcmp(fback.name, "f") == 0 && strcmp(fback.args, "integer, mood") == 0);
		CHECK(fback.after_tables && fback.public_revoked);
		ws_obj_free(&fback);
		fk.len = 0;
		fv.len = 0;
		WsObjectCodec->encode_key(&f0, &fk);
		WsObjectCodec->encode_value(&f0, &fv);
		WsObjectCodec->decode(fk.data, fk.len, fv.data, fv.len, &fback);
		CHECK(strcmp(fback.name, "a(b") == 0 && fback.args[0] == '\0');
		ws_obj_free(&fback);
		buf_free(&fk);
		buf_free(&fv);
	}
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
#define D 1, 'x', 0, 0, 0, 0		/* a definition, no flags, args or owning column */
#define O 1, 'g'				/* an owner */
	static const uint8 ok[] = {D, 0, O, 0, 0, 0, 0};
	static const uint8 cut[] = {D, 0, O, 0};
	static const uint8 left[] = {D, 0, O, 0, 0, 0, 0, 0};
	static const uint8 long_num[] = {D, 0x80, 0x00, O, 0, 0, 0, 0};
	static const uint8 huge[] = {D, 0xff, 0xff, 0xff, 0xff, 0x0f, O, 0, 0, 0, 0};
	static const uint8 past[] = {9, 'x', 0, O, 0, 0, 0, 0};
	static const uint8 nul[] = {2, 'x', 0, 0, O, 0, 0, 0, 0};
	static const uint8 not_utf8[] = {1, 0xff, 0, O, 0, 0, 0, 0};
	static const uint8 lone_at[] = {1, '@', 0, O, 0, 0, 0, 0};
	static const uint8 dep_nothing[] = {D, 1, 1, 'q', O, 0, 0, 0, 0};
	static const uint8 dep_kindless[] = {D, 1, 3, 'x', '/', 'a', O, 0, 0, 0, 0};
	static const uint8 dep_self[] = {D, 1, 6, 'v', 'i', 'e', 'w', '/', 'v', O, 0, 0, 0, 0};
	static const uint8 grant_option[] = {D, 0, O, 0, 1, 0, 0, 2, 1, 'r', 0, 0};
	static const uint8 privilege[] = {D, 0, O, 0, 1, 0, 0, 0, 1, 'Z', 0, 0};
	static const uint8 order[] = {D, 0, O, 0, 1, 0, 0, 0, 2, 'r', 'a', 0, 0};
	static const uint8 predefined[] = {D, 0, 9, 'p', 'g', '_', 'm', 'o', 'n', 'i', 't', 'o', 0, 0, 0, 0};
	static const uint8 no_owner[] = {D, 0, 0, 0, 0, 0, 0};
	static const uint8 using_cut[] = {D, 0, O, 0, 0, 1, 1, 't', 1, 'n'};
	static const uint8 using_no_tag[] = {D, 0, O, 0, 0, 1, 1, 't', 1, 'n', 0, 0};
	static const uint8 using_no_table[] = {D, 0, O, 0, 0, 1, 0, 1, 'n', 5, 0};
	static const uint8 using_order[] = {D, 0, O, 0, 0, 2, 1, 't', 1, 'n', 5, 1, 't', 1, 'n', 5, 0};
	static const uint8 place_no_columns[] = {D, 0, O, 0, 0, 0, 1, 1, 't', 0};
	static const uint8 place_order[] = {D, 0, O, 0, 0, 0, 2, 1, 't', 1, '1', 1, 't', 1, '1'};
	Bad			bad[] = {
		{"view/v", cut, sizeof(cut)},
		{"view/v", using_cut, sizeof(using_cut)},
		{"view/v", place_no_columns, sizeof(place_no_columns)},
		{"view/v", place_order, sizeof(place_order)},
		{"view/v", using_no_tag, sizeof(using_no_tag)},
		{"view/v", using_no_table, sizeof(using_no_table)},
		{"view/v", using_order, sizeof(using_order)},
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
		{"view/v", no_owner, sizeof(no_owner)},
		{"v", ok, sizeof(ok)},
		{"table/v", ok, sizeof(ok)},
		{"view/", ok, sizeof(ok)},
		{"view/\xff", ok, sizeof(ok)},
		{"function/f", ok, sizeof(ok)},
		{"function/f(x)", ok, sizeof(ok)},
	};
#undef D
#undef O

	for (int i = 0; i < lengthof(bad); i++)
		expect_error(decode_bad, &bad[i], ERRCODE_DATA_CORRUPTED, NULL);

	/* (what they are all wrong next to is right) */
	{
		WsObject	o;

		WsObjectCodec->decode((const uint8 *) "view/v", 6, ok, sizeof(ok), &o);
		CHECK(strcmp(o.definition, "x") == 0);
		ws_obj_free(&o);
	}
}

ENGINE_TEST(test_problems)
{
	WsObject	o = example();
	char	   *unordered[] = {"view/b", "view/a"};
	WsObjectComment two_mine[] = {{.text = "x"}, {.text = "y"}};
	WsObjectGrant twice[] = {{.grantee = "h",.privileges = "r"}, {.grantee = "h",.privileges = "a"}};

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
	o.owner = NULL;
	CHECK(ws_obj_problem(&o) != NULL);
	/* (a type reads nothing with its owner's rights, so may have none held) */
	o.kind = WS_OBJ_TYPE;
	o.ncomments = 0;
	o.ngrants = 0;
	o.ndeps = 0;
	CHECK(ws_obj_problem(&o) == NULL);
	{
		WsObjectComment empty[] = {{.text = ""}};
		WsObjectGrant to_owner[] = {{.grantee = "g",.privileges = "r"}};
		WsObjectGrant overlapping[] = {{.grantee = "h",.privileges = "r"},
		{.grantee = "h",.grant_option = true,.privileges = "ar"}};
		WsObjectGrant apart[] = {{.grantee = "h",.privileges = "r"},
		{.grantee = "h",.grant_option = true,.privileges = "a"}};

		o = example();
		o.comments = empty;
		o.ncomments = 1;
		CHECK(ws_obj_problem(&o) != NULL);
		o = example();
		o.grants = to_owner;
		o.ngrants = 1;
		CHECK(ws_obj_problem(&o) != NULL);
		o = example();
		o.grants = overlapping;
		o.ngrants = 2;
		CHECK(ws_obj_problem(&o) != NULL);
		o.grants = apart;
		CHECK(ws_obj_problem(&o) == NULL);
	}
}

static bool
count_table(void *arg, const char *name, const VcTable *entry)
{
	(*(int *) arg)++;
	return true;
}

ENGINE_TEST(test_roles)
{
	WsObject	v = example();
	WsObject	p = {.kind = WS_OBJ_POLICY,.name = "y",.args = "t",
	.definition = "CREATE POLICY y ON t AS PERMISSIVE FOR ALL TO a, \"B c\", PUBLIC USING (true)"};
	char	  **names = NULL;
	int			n = 0;

	ws_obj_roles(&v, &names, &n);
	CHECK_EQ(n, 3);				/* (its owner, and h twice, for the object and a column) */
	CHECK(strcmp(names[0], "g") == 0 && strcmp(names[1], "h") == 0);
	names = NULL;
	n = 0;
	ws_obj_roles(&p, &names, &n);
	CHECK_EQ(n, 2);
	CHECK(strcmp(names[0], "a") == 0 && strcmp(names[1], "B c") == 0);
}

ENGINE_TEST(test_positions)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &prolly_default_policy);
	WsSeqPosition a = {.name = "a",.last_value = -5,.is_called = true};
	WsSeqPosition b = {.name = "b",.last_value = 7,.is_called = false};
	WsSeqPosition in_order[] = {a, b};
	WsSeqPosition reversed[] = {b, a};
	PHash		m1;
	PHash		m2;
	PHash		rv = vc_root_empty(&ctx);
	PHash		got;
	WsSeqPosition *list;
	int			n;

	ws_seq_map(&ctx, in_order, 2, &m1);
	ws_seq_map(&ctx, reversed, 2, &m2);
	CHECK(memcmp(m1.b, m2.b, PHASH_LEN) == 0);
	WsRoot->put_map(&ctx, &rv, WS_ROOT_SEQUENCES, &m1, &rv);
	CHECK(WsRoot->get_map(&ctx, &rv, WS_ROOT_SEQUENCES, &got));
	ws_seq_list(&ctx, &got, &list, &n);
	CHECK_EQ(n, 2);
	CHECK(strcmp(list[0].name, "a") == 0 && list[0].last_value == -5 && list[0].is_called);
	CHECK(strcmp(list[1].name, "b") == 0 && list[1].last_value == 7 && !list[1].is_called);
}

/* The map on a root value: one set of objects, one map, in any order. */
ENGINE_TEST(test_map)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &prolly_default_policy);
	WsObject	a = {.kind = WS_OBJ_MATVIEW,.name = "m",.definition = "x",.owner = "g"};
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
		{.kind = WS_OBJ_VIEW,.name = "a",.definition = "x",.owner = "g",.ndeps = 1,.deps = needs_c},
		{.kind = WS_OBJ_VIEW,.name = "b",.definition = "x",.owner = "g"},
		{.kind = WS_OBJ_VIEW,.name = "c",.definition = "x",.owner = "g",.ndeps = 1,.deps = needs_b},
	};
	WsObject	cycle[] = {
		{.kind = WS_OBJ_VIEW,.name = "a",.definition = "x",.owner = "g",.ndeps = 1,.deps = needs_c},
		{.kind = WS_OBJ_VIEW,.name = "b",.definition = "x",.owner = "g",.ndeps = 1,.deps = needs_a},
		{.kind = WS_OBJ_VIEW,.name = "c",.definition = "x",.owner = "g",.ndeps = 1,.deps = needs_b},
	};
	WsObject	missing[] = {
		{.kind = WS_OBJ_VIEW,.name = "a",.definition = "x",.owner = "g"},
		{.kind = WS_OBJ_VIEW,.name = "b",.definition = "x",.owner = "g",.ndeps = 1,.deps = needs_none},
		{.kind = WS_OBJ_VIEW,.name = "c",.definition = "x",.owner = "g"},
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
		.definition = "AS SELECT 1 AS c WITH NO DATA",.owner = "g"};
	WsObject	at = {.kind = WS_OBJ_VIEW,.name = "at",
		.definition = "AS SELECT 'a@@b'::text AS c",.owner = "g"};
	Buf			b = BUF_INIT;
	WsObject	e = {.kind = WS_OBJ_TYPE,.name = "e",.definition = "AS ENUM ('a')"};
	WsObject	c = {.kind = WS_OBJ_TYPE,.name = "c",.definition = "AS (a integer)"};
	WsObject	r = {.kind = WS_OBJ_TYPE,.name = "r",.definition = "AS RANGE (SUBTYPE = integer)"};
	WsObject	d = {.kind = WS_OBJ_DOMAIN,.name = "d",.definition = "AS integer CONSTRAINT k CHECK ((VALUE > 0))"};
	static const char want[] = "CREATE VIEW s.at AS SELECT 'a@b'::text AS c";
	static const char want_m_made[] = "CREATE MATERIALIZED VIEW s.m AS SELECT 1 AS c WITH NO DATA";
	static const char want_comment[] = "COMMENT ON COLUMN s.v.c IS 'on c'";
	static const char want_grant[] = "GRANT SELECT (c) ON TABLE s.v TO h";
	static const char want_all[] = "GRANT INSERT, SELECT ON TABLE s.v TO h WITH GRANT OPTION";
	static const char want_owner[] = "ALTER VIEW s.v OWNER TO g";
	static const char want_m[] = "ALTER MATERIALIZED VIEW s.m OWNER TO g";

	ws_obj_create_statement(&e, "s", &b);
	CHECK(b.len == strlen("CREATE TYPE s.e AS ENUM ('a')"));
	b.len = 0;
	ws_obj_create_statement(&c, "s", &b);
	CHECK(b.len == strlen("CREATE TYPE s.c AS (a integer)"));
	b.len = 0;
	ws_obj_create_statement(&r, "s", &b);
	CHECK(b.len == strlen("CREATE TYPE s.r AS RANGE (SUBTYPE = integer)"));
	b.len = 0;
	ws_obj_create_statement(&d, "s", &b);
	CHECK(b.len == strlen("CREATE DOMAIN s.d AS integer CONSTRAINT k CHECK ((VALUE > 0))"));
	b.len = 0;
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

	{
		static const char *const of_table[] = {
			"not sql",
			"CREATE POLICY v ON other USING (true)",
			"CREATE POLICY other ON t USING (true)",
			"CREATE POLICY v ON s.t USING (true)",
			"CREATE POLICY v ON t USING (true); DROP TABLE t",
		};

		for (int i = 0; i < lengthof(of_table); i++)
		{
			WsObject	o = {.kind = WS_OBJ_POLICY,.name = "v",.args = "t",.definition = (char *) of_table[i]};

			expect_error(statement_of, &o, ERRCODE_DATA_CORRUPTED, NULL);
		}
		{
			WsObject	o = {.kind = WS_OBJ_TRIGGER,.name = "v",.args = "t",
			.definition = "CREATE OR REPLACE TRIGGER v BEFORE INSERT ON t FOR EACH ROW EXECUTE FUNCTION f()"};
			WsObject	r = {.kind = WS_OBJ_RULE,.name = "v",.args = "t",
			.definition = "CREATE OR REPLACE RULE v AS ON INSERT TO t DO INSTEAD NOTHING"};

			expect_error(statement_of, &o, ERRCODE_DATA_CORRUPTED, NULL);
			expect_error(statement_of, &r, ERRCODE_DATA_CORRUPTED, NULL);
		}
		{
			static const char *const indexes[] = {
				"CREATE INDEX v ON other (a)",
				"CREATE INDEX other ON t (a)",
				"CREATE INDEX v ON s.t (a)",
				"CREATE INDEX CONCURRENTLY v ON t (a)",
				"CREATE INDEX IF NOT EXISTS v ON t (a)",
				"CREATE INDEX v ON t (a) TABLESPACE p",
				"CREATE INDEX v ON t (a); DROP TABLE t",
				"CREATE INDEX ON t (a)",
				"CREATE INDEX v ON ONLY t (a)",
			};
			static const char *const defaults[] = {
				"ALTER VIEW other ALTER COLUMN v SET DEFAULT 1",
				"ALTER VIEW t ALTER COLUMN other SET DEFAULT 1",
				"ALTER VIEW t ALTER COLUMN v DROP DEFAULT",
				"ALTER VIEW IF EXISTS t ALTER COLUMN v SET DEFAULT 1",
				"ALTER VIEW t OWNER TO g",
				"ALTER TABLE t ALTER COLUMN v SET DEFAULT 1",
				"ALTER VIEW t ALTER COLUMN v SET DEFAULT 1, ALTER COLUMN x SET DEFAULT 2",
			};

			for (int i = 0; i < lengthof(indexes); i++)
			{
				WsObject	x = {.kind = WS_OBJ_INDEX,.name = "v",.args = "t",.definition = (char *) indexes[i]};

				expect_error(statement_of, &x, ERRCODE_DATA_CORRUPTED, NULL);
			}
			for (int i = 0; i < lengthof(defaults); i++)
			{
				WsObject	x = {.kind = WS_OBJ_DEFAULT,.name = "v",.args = "t",.definition = (char *) defaults[i]};

				expect_error(statement_of, &x, ERRCODE_DATA_CORRUPTED, NULL);
			}
		}
	}
	{
		static const char *const operators[] = {"(FUNCTION = f); DROP TABLE t", "AS ENUM ('a')", "not sql"};
		static const char *const classes[] = {"FOR TYPE a USING b AS FUNCTION 1 f(a); DROP TABLE t", "USING b", "not sql"};
		static const char *const links[] = {"ALTER OPERATOR <<< (a, a) OWNER TO x; DROP TABLE t", "DROP OPERATOR <<< (a, a)", "not sql"};
		static const char *const members[] = {"ALTER OPERATOR FAMILY v USING b DROP FUNCTION 1 (a, a)", "DROP OPERATOR FAMILY v USING b", "not sql"};

		static const char *const aggregates[] = {"(integer) (SFUNC = f, STYPE = integer); DROP TABLE t", "AS ENUM ('a')", "(integer, SFUNC = f, STYPE = integer)x", "not sql"};
		static const char *const dicts[] = {"(TEMPLATE = t); DROP TABLE t", "AS ENUM ('a')", "not sql"};
		static const char *const maps[] = {"ALTER TEXT SEARCH CONFIGURATION v DROP MAPPING FOR w", "DROP TEXT SEARCH CONFIGURATION v", "not sql"};
		static const char *const stats[] = {"ON a, b FROM t; DROP TABLE t", "AS ENUM ('a')", "not sql"};
		static const char *const families[] = {"USING b; DROP TABLE t", "AS ENUM ('a')", "not sql"};

		for (int i = 0; i < lengthof(aggregates); i++)
		{
			WsObject	o = {.kind = WS_OBJ_AGGREGATE,.name = "v",.args = "integer",.definition = (char *) aggregates[i]};

			expect_error(statement_of, &o, ERRCODE_DATA_CORRUPTED, NULL);
		}
		for (int i = 0; i < lengthof(dicts); i++)
		{
			WsObject	o = {.kind = WS_OBJ_TSDICT,.name = "v",.definition = (char *) dicts[i]};

			expect_error(statement_of, &o, ERRCODE_DATA_CORRUPTED, NULL);
		}
		for (int i = 0; i < lengthof(maps); i++)
		{
			WsObject	o = {.kind = WS_OBJ_TSMAP,.name = "v",.args = "w",.definition = (char *) maps[i]};

			expect_error(statement_of, &o, ERRCODE_DATA_CORRUPTED, NULL);
		}
		for (int i = 0; i < lengthof(stats); i++)
		{
			WsObject	o = {.kind = WS_OBJ_STATISTICS,.name = "v",.definition = (char *) stats[i]};

			expect_error(statement_of, &o, ERRCODE_DATA_CORRUPTED, NULL);
		}
		for (int i = 0; i < lengthof(families); i++)
		{
			WsObject	o = {.kind = WS_OBJ_OPFAMILY,.name = "v",.args = "b",.definition = (char *) families[i]};

			expect_error(statement_of, &o, ERRCODE_DATA_CORRUPTED, NULL);
		}
		for (int i = 0; i < lengthof(operators); i++)
		{
			WsObject	o = {.kind = WS_OBJ_OPERATOR,.name = "<<<",.args = "a, a",.definition = (char *) operators[i]};

			expect_error(statement_of, &o, ERRCODE_DATA_CORRUPTED, NULL);
		}
		for (int i = 0; i < lengthof(classes); i++)
		{
			WsObject	o = {.kind = WS_OBJ_OPCLASS,.name = "v",.args = "b",.definition = (char *) classes[i]};

			expect_error(statement_of, &o, ERRCODE_DATA_CORRUPTED, NULL);
		}
		for (int i = 0; i < lengthof(links); i++)
		{
			WsObject	o = {.kind = WS_OBJ_OPLINK,.name = "<<<",.args = "a, a",.definition = (char *) links[i]};

			expect_error(statement_of, &o, ERRCODE_DATA_CORRUPTED, NULL);
		}
		for (int i = 0; i < lengthof(members); i++)
		{
			WsObject	o = {.kind = WS_OBJ_OPMEMBERS,.name = "v",.args = "b",.definition = (char *) members[i]};

			expect_error(statement_of, &o, ERRCODE_DATA_CORRUPTED, NULL);
		}
	}
	{
		static const char *const collations[] = {"(LOCALE = 'C'); DROP TABLE t", "FROM \"C\"; DROP TABLE t", "AS ENUM ('a')"};
		static const char *const casts[] = {"CREATE CAST (a AS b) WITH INOUT; DROP TABLE t", "DROP CAST (a AS b)", "not sql"};
		static const char *const conversions[] = {"FOR 'a' TO 'b' FROM f; DROP TABLE t", "not sql"};

		for (int i = 0; i < lengthof(collations); i++)
		{
			WsObject	o = {.kind = WS_OBJ_COLLATION,.name = "v",.definition = (char *) collations[i]};

			expect_error(statement_of, &o, ERRCODE_DATA_CORRUPTED, NULL);
		}
		for (int i = 0; i < lengthof(casts); i++)
		{
			WsObject	o = {.kind = WS_OBJ_CAST,.name = "a",.args = "b",.definition = (char *) casts[i]};

			expect_error(statement_of, &o, ERRCODE_DATA_CORRUPTED, NULL);
		}
		for (int i = 0; i < lengthof(conversions); i++)
		{
			WsObject	o = {.kind = WS_OBJ_CONVERSION,.name = "v",.definition = (char *) conversions[i]};

			expect_error(statement_of, &o, ERRCODE_DATA_CORRUPTED, NULL);
		}
	}
	{
		static const char *const sequences[] = {
			"AS bigint; DROP TABLE t",
			"OWNED BY t.a",
			"AS (a int)",
		};

		for (int i = 0; i < lengthof(sequences); i++)
		{
			WsObject	o = {.kind = WS_OBJ_SEQUENCE,.name = "v",.definition = (char *) sequences[i]};

			expect_error(statement_of, &o, ERRCODE_DATA_CORRUPTED, NULL);
		}
	}
	{
		static const char *const functions[] = {
			"() RETURNS integer LANGUAGE sql AS 'SELECT 1'; DROP TABLE t",
			"AS 'SELECT 1'",
			"",
		};

		for (int i = 0; i < lengthof(functions); i++)
		{
			WsObject	o = {.kind = WS_OBJ_FUNCTION,.name = "v",.args = "",.definition = (char *) functions[i]};

			expect_error(statement_of, &o, ERRCODE_DATA_CORRUPTED, NULL);
		}
	}
	{
		static const char *const types[] = {
			"AS ENUM ('a'); DROP TABLE t",
			"AS (a int); DROP TABLE t",
			"(INPUT = i, OUTPUT = o)",
			"",
		};
		static const char *const domains[] = {
			"AS int; DROP TABLE t",
			"",
		};

		for (int i = 0; i < lengthof(types); i++)
		{
			WsObject	o = {.kind = WS_OBJ_TYPE,.name = "v",.definition = (char *) types[i]};

			expect_error(statement_of, &o, ERRCODE_DATA_CORRUPTED, NULL);
		}
		for (int i = 0; i < lengthof(domains); i++)
		{
			WsObject	o = {.kind = WS_OBJ_DOMAIN,.name = "v",.definition = (char *) domains[i]};

			expect_error(statement_of, &o, ERRCODE_DATA_CORRUPTED, NULL);
		}
	}
	for (int i = 0; i < lengthof(views); i++)
	{
		WsObject	o = {.kind = WS_OBJ_VIEW,.name = "v",.definition = (char *) views[i],.owner = "g"};

		expect_error(statement_of, &o, ERRCODE_DATA_CORRUPTED, NULL);
	}
	for (int i = 0; i < lengthof(matviews); i++)
	{
		WsObject	o = {.kind = WS_OBJ_MATVIEW,.name = "v",.definition = (char *) matviews[i],.owner = "g"};

		expect_error(statement_of, &o, ERRCODE_DATA_CORRUPTED, NULL);
	}
}

static const ProllyUnitTest tests[] = {
	{"bytes", test_bytes},
	{"malformed", test_malformed},
	{"problems", test_problems},
	{"map", test_map},
	{"positions", test_positions},
	{"roles", test_roles},
	{"restore_order", test_restore_order},
	{"statements", test_statements},
	{"refused_statements", test_refused_statements},
};

const ProllyUnitSuite unit_ws_obj_suite = {"ws_obj", tests, lengthof(tests)};
