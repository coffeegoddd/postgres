/*-------------------------------------------------------------------------
 *
 * unit_ws_tags.c
 *	  Unit tests of column references by tag in stored text: what is written
 *	  comes back as it was, nothing in it is taken for a reference, and what
 *	  is malformed is refused.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/test/modules/test_prolly/unit_ws_tags.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "engine_test_util.h"
#include "access/tupdesc.h"
#include "catalog/pg_type_d.h"
#include "prolly/ws/ws_tags.h"
#include "test_prolly.h"

static const char *
names(void *arg, uint64 tag)
{
	return tag == 7 ? "seven" : tag == 8 ? "Eight Too" : tag == PG_UINT64_MAX ? "big" : NULL;
}

static char *
rendered(const char *stored)
{
	Buf			b = BUF_INIT;
	uint64		missing = 0;

	CHECK(ws_tags_render(stored, names, NULL, &b, &missing));
	buf_append_byte(&b, 0);
	return (char *) b.data;
}

static void
test_escape_and_render(void)
{
	static const char *const written[] = {
		"", "plain", "@", "@@", "a @> b", "'@{7}'", "x @{", "@{7}", "@ @ @",
	};

	for (size_t i = 0; i < lengthof(written); i++)
	{
		Buf			b = BUF_INIT;

		ws_tags_escape(written[i], &b);
		buf_append_byte(&b, 0);
		/* what is written comes back as it was: none of it a reference */
		CHECK(strcmp(rendered((char *) b.data), written[i]) == 0);
		buf_free(&b);
	}
	CHECK(strcmp(rendered("@{7} + @{8}"), "seven + \"Eight Too\"") == 0);
	CHECK(strcmp(rendered("@{7}@{7}"), "sevenseven") == 0);
	CHECK(strcmp(rendered("@@{7}"), "@{7}") == 0);
	CHECK(strcmp(rendered("@@@{7}"), "@seven") == 0);
	CHECK(strcmp(rendered("@{18446744073709551615}"), "big") == 0);
}

static void
test_render_missing(void)
{
	Buf			b = BUF_INIT;
	uint64		missing = 0;

	CHECK(!ws_tags_render("a @{9} b", names, NULL, &b, &missing));
	CHECK_EQ(missing, 9);
	buf_free(&b);
}

static void
render_it(void *arg)
{
	Buf			b = BUF_INIT;
	uint64		missing;

	(void) ws_tags_render(arg, names, NULL, &b, &missing);
}

static void
test_render_malformed(void)
{
	static const char *const bad[] = {
		"@", "a @ b", "@x", "@{", "@{}", "@{x}", "@{7", "@{7x}",
		"@{18446744073709551616}", "@{07}", "@{00}",
	};

	for (size_t i = 0; i < lengthof(bad); i++)
	{
		expect_error(render_it, (void *) bad[i], ERRCODE_DATA_CORRUPTED, NULL);
		CHECK(!ws_tags_well_formed(bad[i]));
	}
	CHECK(ws_tags_well_formed("") && ws_tags_well_formed("a @@ @{0} @{7}b"));
}

static bool
tag_of(void *arg, const char *name, uint64 *tag)
{
	if (strcmp(name, "a") == 0)
		*tag = 7;
	else if (strcmp(name, "Odd Name") == 0)
		*tag = 8;
	else if (strcmp(name, "select") == 0)
		*tag = 9;
	else
		return false;
	return true;
}

static char *
templated(const char *expression)
{
	Buf			b = BUF_INIT;

	ws_tags_template(expression, tag_of, NULL, &b);
	buf_append_byte(&b, 0);
	return (char *) b.data;
}

/*
 * References are found by the grammar: a column's name in a string, a
 * comment, a function's name or a field's is none, and an @ of any of them
 * is an @.
 */
static void
test_template(void)
{
	static const struct
	{
		const char *expression;
		const char *stored;
	}			cases[] = {
		{"(a + 1)", "(@{7} + 1)"},
		{"(a + a)", "(@{7} + @{7})"},
		{"(\"Odd Name\" || a)", "(@{8} || @{7})"},
		{"\"select\"", "@{9}"},
		{"'a'::text", "'a'::text"},
		{"(a || 'a @{7} a')", "(@{7} || 'a @@{7} a')"},
		{"(a @> a)", "(@{7} @@> @{7})"},
		{"a(1)", "a(1)"},
		{"(a).a", "(@{7}).a"},
		{"'x'::geometry(point, 4326)", "'x'::geometry(point, 4326)"},
		{"a::numeric(a, 2)", "@{7}::numeric(a, 2)"},
		{"$$a$$", "$$a$$"},
		{"1 /* a */", "1 /* a */"},
		{"(a * CASE WHEN a > 0 THEN 1 ELSE a END)", "(@{7} * CASE WHEN @{7} > 0 THEN 1 ELSE @{7} END)"},
		{"'caf\xc3\xa9'::text || a", "'caf\xc3\xa9'::text || @{7}"},
	};

	for (size_t i = 0; i < lengthof(cases); i++)
	{
		char	   *got = templated(cases[i].expression);

		if (strcmp(got, cases[i].stored) != 0)
			elog(ERROR, "\"%s\" stored as \"%s\", not \"%s\"", cases[i].expression, got,
				 cases[i].stored);
		CHECK(ws_tags_well_formed(got));
	}
}

static void
template_unknown(void *arg)
{
	(void) templated("(nosuch + 1)");
}

static void
template_qualified(void *arg)
{
	(void) templated("(t.a + 1)");
}

static void
template_not_one(void *arg)
{
	(void) templated("1) FROM y WHERE (a");
}

static void
test_template_refused(void)
{
	expect_error(template_unknown, NULL, ERRCODE_FEATURE_NOT_SUPPORTED, NULL);
	expect_error(template_qualified, NULL, ERRCODE_FEATURE_NOT_SUPPORTED, NULL);
	expect_error(template_not_one, NULL, ERRCODE_INTERNAL_ERROR, NULL);
}

/*
 * The tag a new column gets: this value on every cluster, or two branches'
 * clones would disagree; different for anything else it is derived from, and
 * no parts run together.
 */
static void
test_derive(void)
{
	uint64		tag = ws_tag_derive("t", "a", "pg_catalog.int4(-1)", 0);

	CHECK_EQ(tag, UINT64CONST(254872713275789205));
	CHECK(tag < (UINT64CONST(1) << 60));
	CHECK(tag != ws_tag_derive("t", "a", "pg_catalog.int4(-1)", 1));
	CHECK(tag != ws_tag_derive("t", "b", "pg_catalog.int4(-1)", 0));
	CHECK(tag != ws_tag_derive("u", "a", "pg_catalog.int4(-1)", 0));
	CHECK(tag != ws_tag_derive("t", "a", "pg_catalog.int8(-1)", 0));
	CHECK(ws_tag_derive("ab", "c", "t", 0) != ws_tag_derive("a", "bc", "t", 0));
}

/* A descriptor's entry starts with no tag, and a tag makes two different. */
static void
test_tupdesc(void)
{
	TupleDesc	a = CreateTemplateTupleDesc(2);
	TupleDesc	b;

	TupleDescInitEntry(a, 1, "x", INT4OID, -1, 0);
	TupleDescInitBuiltinEntry(a, 2, "y", INT4OID, -1, 0);
	TupleDescFinalize(a);
	CHECK_EQ(TupleDescAttr(a, 0)->attcoltag, 0);
	CHECK_EQ(TupleDescAttr(a, 1)->attcoltag, 0);
	b = CreateTupleDescCopy(a);
	CHECK(equalTupleDescs(a, b));
	TupleDescAttr(b, 0)->attcoltag = 7;
	CHECK(!equalTupleDescs(a, b));
	FreeTupleDesc(a);
	FreeTupleDesc(b);
}

static const ProllyUnitTest tests[] = {
	{"escape_and_render", test_escape_and_render},
	{"render_missing", test_render_missing},
	{"render_malformed", test_render_malformed},
	{"template", test_template},
	{"template_refused", test_template_refused},
	{"derive", test_derive},
	{"tupdesc", test_tupdesc},
};

const ProllyUnitSuite unit_ws_tags_suite = {"ws_tags", tests, lengthof(tests)};
