/*-------------------------------------------------------------------------
 *
 * unit_ws_sql.c
 *	  Unit tests of how the working-set model runs statements: text in and
 *	  out, a failure reported and undone, and text the same whatever the
 *	  session's settings.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/test/modules/test_prolly/unit_ws_sql.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "engine_test_util.h"
#include "miscadmin.h"
#include "prolly/ws/ws_sql.h"
#include "test_prolly.h"
#include "utils/guc.h"

static void
run(const char *sql)
{
	WsSqlRows	rows;

	WsSql->run(sql, 0, NULL, &rows);
	ws_sql_rows_free(&rows);
}

/* One value of a one-row result. */
static char *
one(const char *sql)
{
	WsSqlRows	rows;
	char	   *v;

	WsSql->run(sql, 0, NULL, &rows);
	CHECK_EQ(rows.nrows, 1);
	CHECK_EQ(rows.ncols, 1);
	v = ws_sql_value(&rows, 0, 0) ? pstrdup(ws_sql_value(&rows, 0, 0)) : NULL;
	ws_sql_rows_free(&rows);
	return v;
}

static bool
is(const char *got, const char *want)
{
	if (strcmp(got, want) == 0)
		return true;
	elog(WARNING, "got \"%s\", wanted \"%s\"", got, want);
	return false;
}

static void
test_rows(void)
{
	const char *params[] = {"7", NULL, "seven"};
	WsSqlRows	rows;
	int			level = WsSql->begin();

	WsSql->run("SELECT $1::int8 + g, $2, $3 FROM pg_catalog.generate_series(1, 2) g", 3, params,
			   &rows);
	CHECK_EQ(rows.nrows, 2);
	CHECK_EQ(rows.ncols, 3);
	CHECK(is(ws_sql_value(&rows, 0, 0), "8"));
	CHECK(is(ws_sql_value(&rows, 1, 0), "9"));
	CHECK(ws_sql_value(&rows, 1, 1) == NULL);
	CHECK(is(ws_sql_value(&rows, 1, 2), "seven"));
	CHECK_EQ(rows.processed, 2);
	ws_sql_rows_free(&rows);

	/* no rows, and rows changed with none returned */
	WsSql->run("SELECT 1 WHERE false", 0, NULL, &rows);
	CHECK(rows.nrows == 0 && rows.ncols == 1);
	ws_sql_rows_free(&rows);
	run("CREATE TEMP TABLE ws_r (a int)");
	WsSql->run("INSERT INTO pg_temp.ws_r SELECT g FROM pg_catalog.generate_series(1, 3) g", 0, NULL,
			   &rows);
	CHECK_EQ(rows.processed, 3);
	CHECK_EQ(rows.nrows, 0);
	ws_sql_rows_free(&rows);

	/* a statement sees what those before it changed */
	CHECK(is(one("SELECT count(*) FROM pg_temp.ws_r"), "3"));
	run("DROP TABLE pg_temp.ws_r");
	WsSql->end(level);
}

/* A failure is reported with its fields, what it changed undone. */
static void
test_attempt(void)
{
	WsSqlRows	rows;
	WsSqlFailure f;
	int			level = WsSql->begin();

	run("CREATE TEMP TABLE ws_a (id int CONSTRAINT ws_a_pk PRIMARY KEY, v int NOT NULL)");
	CHECK(WsSql->attempt("INSERT INTO pg_temp.ws_a VALUES (1, 1)", 0, NULL, &rows, &f));
	CHECK_EQ(rows.processed, 1);
	ws_sql_rows_free(&rows);

	/* a unique violation, by constraint; a statement's other rows undone */
	CHECK(!WsSql->attempt("INSERT INTO pg_temp.ws_a VALUES (2, 2), (1, 3)", 0, NULL, &rows, &f));
	CHECK(is(f.sqlstate, "23505"));
	CHECK(is(f.constraint, "ws_a_pk"));
	CHECK(f.message != NULL);
	CHECK(rows.nrows == 0 && rows.values == NULL);
	ws_sql_failure_free(&f);
	CHECK(is(one("SELECT count(*) FROM pg_temp.ws_a"), "1"));

	/* a NULL, by column */
	CHECK(!WsSql->attempt("INSERT INTO pg_temp.ws_a VALUES (3, NULL)", 0, NULL, &rows, &f));
	CHECK(is(f.sqlstate, "23502"));
	CHECK(is(f.column, "v"));
	ws_sql_failure_free(&f);

	/* a statement that does not parse is a failure too, and work goes on */
	CHECK(!WsSql->attempt("SELEC 1", 0, NULL, &rows, &f));
	CHECK(is(f.sqlstate, "42601"));
	ws_sql_failure_free(&f);
	CHECK(is(one("SELECT 1"), "1"));
	run("DROP TABLE pg_temp.ws_a");
	WsSql->end(level);
}

static void
cancelled_attempt(void *arg)
{
	WsSqlRows	rows;
	WsSqlFailure f;

	QueryCancelPending = true;
	InterruptPending = true;
	(void) WsSql->attempt("SELECT count(*) FROM pg_catalog.generate_series(1, 1000)", 0, NULL,
						  &rows, &f);
}

/* A cancel is no statement's failure: it goes on up. */
static void
test_attempt_cancelled(void)
{
	expect_error(cancelled_attempt, NULL, ERRCODE_QUERY_CANCELED, NULL);
	CHECK(is(one("SELECT 1"), "1"));
}

/* A session set as unlike the settings as it can be. */
static const struct
{
	const char *name;
	const char *value;
}			session_settings[] = {
	{"DateStyle", "SQL, DMY"},
	{"IntervalStyle", "sql_standard"},
	{"TimeZone", "Asia/Tokyo"},
	{"extra_float_digits", "-5"},
	{"bytea_output", "escape"},
	{"search_path", "pg_temp, public"},
	{"quote_all_identifiers", "on"},
};

/* The session's settings changed, until the level's end. */
static int
odd_session(void)
{
	int			level = NewGUCNestLevel();

	for (size_t i = 0; i < lengthof(session_settings); i++)
		(void) set_config_option(session_settings[i].name, session_settings[i].value,
								 PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	/* (money's output otherwise, where the system has the locale: quietly if not) */
	(void) set_config_option("lc_monetary", "es_ES.utf8", PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, DEBUG5, false);
	return level;
}

static void
check_settings_taken(void)
{
	CHECK(is(one("SELECT '2026-11-03 12:30:00+00'::timestamptz"), "2026-11-03 12:30:00+00"));
	CHECK(is(one("SELECT '2026-11-03'::date"), "2026-11-03"));
	CHECK(is(one("SELECT '1 day 2 hours'::interval"), "1 day 02:00:00"));
	CHECK(is(one("SELECT 1::float8 / 3"), "0.3333333333333333"));
	CHECK(is(one("SELECT '\\x01ff'::bytea"), "\\x01ff"));
	CHECK(is(one("SELECT pg_catalog.current_setting('search_path')"), ""));
	CHECK(is(one("SELECT 1.5::money"), "$1.50"));
	CHECK(is(one("SELECT pg_catalog.format_type('pg_catalog.int4'::pg_catalog.regtype, NULL)"),
			 "integer"));
	CHECK(is(one("SELECT pg_catalog.quote_ident('abc')"), "abc"));
}

/*
 * Text is the same whatever the session's settings: the same values under
 * the session as it is and set otherwise; the session's own settings back
 * when the scope ends.
 */
static void
test_settings(void)
{
	int			level;
	int			odd;

	level = WsSql->begin();
	check_settings_taken();
	WsSql->end(level);

	odd = odd_session();
	level = WsSql->begin();
	check_settings_taken();
	WsSql->end(level);
	for (size_t i = 0; i < lengthof(session_settings); i++)
		CHECK(is(GetConfigOption(session_settings[i].name, false, false),
				 session_settings[i].value) ||
			  strcmp(session_settings[i].name, "DateStyle") == 0);
	CHECK(is(GetConfigOption("DateStyle", false, false), "SQL, DMY"));
	AtEOXact_GUC(true, odd);
}

/* (a failure in a scope, raised) */
static void
scope_fails(void *arg)
{
	(void) WsSql->begin();
	run("SELECT 1 / 0");
}

/* An error in a scope gives the session its settings back; an attempt's stays in. */
static void
test_settings_after_errors(void)
{
	int			odd = odd_session();
	int			level;
	WsSqlRows	rows;
	WsSqlFailure f;

	expect_error(scope_fails, NULL, ERRCODE_DIVISION_BY_ZERO, NULL);
	CHECK(is(GetConfigOption("IntervalStyle", false, false), "sql_standard"));

	level = WsSql->begin();
	CHECK(!WsSql->attempt("SELECT 1 / 0", 0, NULL, &rows, &f));
	ws_sql_failure_free(&f);
	check_settings_taken();
	WsSql->end(level);
	CHECK(is(GetConfigOption("IntervalStyle", false, false), "sql_standard"));
	AtEOXact_GUC(true, odd);
}

static const ProllyUnitTest tests[] = {
	{"rows", test_rows},
	{"attempt", test_attempt},
	{"attempt_cancelled", test_attempt_cancelled},
	{"settings", test_settings},
	{"settings_after_errors", test_settings_after_errors},
};

const ProllyUnitSuite unit_ws_sql_suite = {"ws_sql", tests, lengthof(tests)};
