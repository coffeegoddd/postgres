/*-------------------------------------------------------------------------
 *
 * ws_sql.c
 *	  "spi", the working-set model's statements run in the server
 *	  (prolly/ws/ws_sql.h).
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/ws/ws_sql.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "mb/pg_wchar.h"
#include "prolly/ws/ws_sql.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/resowner.h"

const WsSqlRoutine *WsSql = &SpiWsSql;

/*
 * The settings a commit is captured under.  (lc_monetary: money's output.
 * quote_all_identifiers: what format_type and the pg_get_* functions print.
 * catalog_branch_filter: other branches' objects stay out of a commit.
 * row_security: a policy does not quietly leave rows out.
 * default_tablespace, default_table_access_method: what a statement does not
 * say is what a record holds.
 * xmloption: a value is read back whichever form it is in.)
 */
static const struct
{
	const char *name;
	const char *value;
}			ws_settings[] = {
	{"DateStyle", "ISO"},
	{"IntervalStyle", "postgres"},
	{"TimeZone", "UTC"},
	{"extra_float_digits", "1"},
	{"bytea_output", "hex"},
	{"search_path", "pg_catalog, pg_temp"},
	{"lc_monetary", "C"},
	{"quote_all_identifiers", "off"},
	{"catalog_branch_filter", "on"},
	{"row_security", "off"},
	{"default_tablespace", ""},
	{"default_table_access_method", "heap"},
	{"xmloption", "content"},
};

/* (as PGC_SUSET: catalog_branch_filter is, and this is no user's choosing) */
static int
spi_begin_scope(void)
{
	int			level = NewGUCNestLevel();

	for (size_t i = 0; i < lengthof(ws_settings); i++)
		(void) set_config_option(ws_settings[i].name, ws_settings[i].value,
								 PGC_SUSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	return level;
}

static void
spi_end_scope(int level)
{
	AtEOXact_GUC(true, level);
}

/*
 * Whether an error is the statement's own to report: what it was given or
 * asked for, or what a function it ran raised, and not the server's state.
 * (All else is raised: the next statement could not rightly go on.)
 */
static bool
statements_own_error(int code)
{
	if (code == ERRCODE_LOCK_NOT_AVAILABLE || code == ERRCODE_ASSERT_FAILURE)
		return false;
	switch (ERRCODE_TO_CATEGORY(code))
	{
		case ERRCODE_CARDINALITY_VIOLATION:
		case ERRCODE_CASE_NOT_FOUND:
		case ERRCODE_CONFIG_FILE_ERROR:
		case ERRCODE_DATA_EXCEPTION:
		case ERRCODE_DEPENDENT_PRIVILEGE_DESCRIPTORS_STILL_EXIST:
		case ERRCODE_EXTERNAL_ROUTINE_EXCEPTION:
		case ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION:
		case ERRCODE_FEATURE_NOT_SUPPORTED:
		case ERRCODE_INTEGRITY_CONSTRAINT_VIOLATION:
		case ERRCODE_INVALID_GRANTOR:
		case ERRCODE_INVALID_SCHEMA_NAME:
		case ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE:
		case ERRCODE_PLPGSQL_ERROR:
		case ERRCODE_PROGRAM_LIMIT_EXCEEDED:
		case ERRCODE_SQL_ROUTINE_EXCEPTION:
		case ERRCODE_SYNTAX_ERROR_OR_ACCESS_RULE_VIOLATION:
		case ERRCODE_WITH_CHECK_OPTION_VIOLATION:
			return true;
		default:
			return false;
	}
}

/* The statement run under SPI, its rows copied into cxt. */
static void
spi_run_in(const char *sql, int nparams, const char *const *params, WsSqlRows *rows,
		   MemoryContext cxt, bool read_only)
{
	Oid		   *types = NULL;
	Datum	   *values = NULL;
	char	   *nulls = NULL;
	int			rc;

	memset(rows, 0, sizeof(*rows));
	(void) pg_verifymbstr(sql, strlen(sql), false);
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	if (nparams > 0)
	{
		types = palloc_array(Oid, nparams);
		values = palloc_array(Datum, nparams);
		nulls = palloc_array(char, nparams);
		for (int i = 0; i < nparams; i++)
		{
			if (params[i])
				(void) pg_verifymbstr(params[i], strlen(params[i]), false);
			types[i] = TEXTOID;
			nulls[i] = params[i] ? ' ' : 'n';
			values[i] = params[i] ? CStringGetTextDatum(params[i]) : (Datum) 0;
		}
	}
	rc = SPI_execute_with_args(sql, nparams, types, values, nulls, read_only, 0);
	if (rc < 0)
		elog(ERROR, "\"%s\" failed: %s", sql, SPI_result_code_string(rc));
	rows->processed = SPI_processed;
	if (SPI_tuptable != NULL && SPI_processed > (uint64) INT_MAX)
		elog(ERROR, "\"%s\" returned too many rows", sql);
	if (SPI_tuptable != NULL)
	{
		TupleDesc	desc = SPI_tuptable->tupdesc;

		rows->nrows = (int) SPI_processed;
		rows->ncols = desc->natts;
		rows->values = MemoryContextAllocZero(cxt,
											  Max((Size) rows->nrows * rows->ncols, 1) *
											  sizeof(char *));
		for (int r = 0; r < rows->nrows; r++)
			for (int c = 0; c < rows->ncols; c++)
			{
				/* (each type's output function, as COPY's) */
				char	   *v = SPI_getvalue(SPI_tuptable->vals[r], desc, c + 1);

				if (v != NULL)
					rows->values[r * rows->ncols + c] = MemoryContextStrdup(cxt, v);
			}
	}
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");
}

static void
spi_run(const char *sql, int nparams, const char *const *params, WsSqlRows *rows)
{
	spi_run_in(sql, nparams, params, rows, CurrentMemoryContext, false);
}

static void
spi_scan(const char *sql, int nparams, const char *const *params, int batch,
		 void (*fn) (void *arg, const WsSqlRows *rows), void *arg)
{
	Oid		   *types = NULL;
	Datum	   *values = NULL;
	char	   *nulls = NULL;
	Portal		portal;
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "ws_sql scan",
											  ALLOCSET_DEFAULT_SIZES);

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	if (nparams > 0)
	{
		types = palloc_array(Oid, nparams);
		values = palloc_array(Datum, nparams);
		nulls = palloc_array(char, nparams);
		for (int i = 0; i < nparams; i++)
		{
			if (params[i])
				(void) pg_verifymbstr(params[i], strlen(params[i]), false);
			types[i] = TEXTOID;
			nulls[i] = params[i] ? ' ' : 'n';
			values[i] = params[i] ? CStringGetTextDatum(params[i]) : (Datum) 0;
		}
	}
	portal = SPI_cursor_open_with_args(NULL, sql, nparams, types, values, nulls, true, 0);
	if (portal == NULL)
		elog(ERROR, "\"%s\" failed: %s", sql, SPI_result_code_string(SPI_result));
	for (;;)
	{
		WsSqlRows	rows = {0};
		MemoryContext old;

		SPI_cursor_fetch(portal, true, batch);
		if (SPI_processed == 0)
			break;
		old = MemoryContextSwitchTo(cxt);
		rows.nrows = (int) SPI_processed;
		rows.ncols = SPI_tuptable->tupdesc->natts;
		rows.processed = SPI_processed;
		rows.values = palloc0(Max((Size) rows.nrows * rows.ncols, 1) * sizeof(char *));
		for (int r = 0; r < rows.nrows; r++)
			for (int c = 0; c < rows.ncols; c++)
				rows.values[r * rows.ncols + c] = SPI_getvalue(SPI_tuptable->vals[r],
															   SPI_tuptable->tupdesc, c + 1);
		fn(arg, &rows);
		MemoryContextSwitchTo(old);
		SPI_freetuptable(SPI_tuptable);
		MemoryContextReset(cxt);
	}
	SPI_cursor_close(portal);
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");
	MemoryContextDelete(cxt);
}

/* (as PL/pgSQL's block with an exception clause: a subtransaction) */
static bool
spi_attempt_fn(void (*fn) (void *arg), void *arg, WsSqlFailure *failure)
{
	MemoryContext cxt = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	volatile bool ok = true;

	memset(failure, 0, sizeof(*failure));
	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(cxt);
	PG_TRY();
	{
		fn(arg);
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(cxt);
		CurrentResourceOwner = owner;
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(cxt);
		edata = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(cxt);
		CurrentResourceOwner = owner;
		if (!statements_own_error(edata->sqlerrcode))
			ReThrowError(edata);
		memcpy(failure->sqlstate, unpack_sql_state(edata->sqlerrcode), 6);
		failure->message = edata->message ? pstrdup(edata->message) : NULL;
		failure->constraint = edata->constraint_name ? pstrdup(edata->constraint_name) : NULL;
		failure->column = edata->column_name ? pstrdup(edata->column_name) : NULL;
		FreeErrorData(edata);
		ok = false;
	}
	PG_END_TRY();
	return ok;
}

typedef struct AttemptArgs
{
	const char *sql;
	int			nparams;
	const char *const *params;
	WsSqlRows  *rows;
	MemoryContext cxt;
} AttemptArgs;

static void
attempt_statement(void *arg)
{
	AttemptArgs *a = arg;

	spi_run_in(a->sql, a->nparams, a->params, a->rows, a->cxt, false);
}

static bool
spi_attempt(const char *sql, int nparams, const char *const *params, WsSqlRows *rows,
			WsSqlFailure *failure)
{
	AttemptArgs a = {sql, nparams, params, rows, CurrentMemoryContext};

	if (spi_attempt_fn(attempt_statement, &a, failure))
		return true;
	memset(rows, 0, sizeof(*rows));
	return false;
}

static void
spi_read(const char *sql, int nparams, const char *const *params, WsSqlRows *rows)
{
	spi_run_in(sql, nparams, params, rows, CurrentMemoryContext, true);
}

const WsSqlRoutine SpiWsSql = {
	.name = "spi",
	.begin = spi_begin_scope,
	.end = spi_end_scope,
	.run = spi_run,
	.read = spi_read,
	.scan = spi_scan,
	.attempt = spi_attempt,
	.attempt_fn = spi_attempt_fn,
};

void
ws_sql_rows_free(WsSqlRows *rows)
{
	for (int i = 0; i < rows->nrows * rows->ncols; i++)
		if (rows->values[i] != NULL)
			pfree(rows->values[i]);
	if (rows->values != NULL)
		pfree(rows->values);
	memset(rows, 0, sizeof(*rows));
}

void
ws_sql_failure_free(WsSqlFailure *failure)
{
	if (failure->message != NULL)
		pfree(failure->message);
	if (failure->constraint != NULL)
		pfree(failure->constraint);
	if (failure->column != NULL)
		pfree(failure->column);
	memset(failure, 0, sizeof(*failure));
}
