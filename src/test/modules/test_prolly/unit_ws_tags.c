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

static const ProllyUnitTest tests[] = {
	{"escape_and_render", test_escape_and_render},
	{"render_missing", test_render_missing},
	{"render_malformed", test_render_malformed},
};

const ProllyUnitSuite unit_ws_tags_suite = {"ws_tags", tests, lengthof(tests)};
