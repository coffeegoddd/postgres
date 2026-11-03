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
#include "prolly/ws/ws_sql.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/resowner.h"

const WsSqlRoutine *WsSql = &SpiWsSql;

/*
 * The settings a commit's text is written under.  (lc_monetary: money's
 * output.  quote_all_identifiers: what format_type and the pg_get_*
 * functions print.)
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
	{"search_path", ""},
	{"lc_monetary", "C"},
	{"quote_all_identifiers", "off"},
};

static int
spi_begin_scope(void)
{
	int			level = NewGUCNestLevel();

	for (size_t i = 0; i < lengthof(ws_settings); i++)
		(void) set_config_option(ws_settings[i].name, ws_settings[i].value,
								 PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	return level;
}

static void
spi_end_scope(int level)
{
	AtEOXact_GUC(true, level);
}

/* The statement run under SPI, its rows copied into cxt. */
static void
spi_run_in(const char *sql, int nparams, const char *const *params, WsSqlRows *rows,
		   MemoryContext cxt)
{
	Oid		   *types = NULL;
	Datum	   *values = NULL;
	char	   *nulls = NULL;
	int			rc;

	memset(rows, 0, sizeof(*rows));
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	if (nparams > 0)
	{
		types = palloc_array(Oid, nparams);
		values = palloc_array(Datum, nparams);
		nulls = palloc_array(char, nparams);
		for (int i = 0; i < nparams; i++)
		{
			types[i] = TEXTOID;
			nulls[i] = params[i] ? ' ' : 'n';
			values[i] = params[i] ? CStringGetTextDatum(params[i]) : (Datum) 0;
		}
	}
	rc = SPI_execute_with_args(sql, nparams, types, values, nulls, false, 0);
	if (rc < 0)
		elog(ERROR, "\"%s\" failed: %s", sql, SPI_result_code_string(rc));
	rows->processed = SPI_processed;
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
	spi_run_in(sql, nparams, params, rows, CurrentMemoryContext);
}

/* (as PL/pgSQL's block with an exception clause: a subtransaction) */
static bool
spi_attempt(const char *sql, int nparams, const char *const *params, WsSqlRows *rows,
			WsSqlFailure *failure)
{
	MemoryContext cxt = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	volatile bool ok = true;

	memset(failure, 0, sizeof(*failure));
	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(cxt);
	PG_TRY();
	{
		spi_run_in(sql, nparams, params, rows, cxt);
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

		/* (what cannot be gone on from is not the statement's to report) */
		if (edata->sqlerrcode == ERRCODE_QUERY_CANCELED ||
			edata->sqlerrcode == ERRCODE_ADMIN_SHUTDOWN)
			ReThrowError(edata);
		memcpy(failure->sqlstate, unpack_sql_state(edata->sqlerrcode), 6);
		failure->message = edata->message ? pstrdup(edata->message) : NULL;
		failure->constraint = edata->constraint_name ? pstrdup(edata->constraint_name) : NULL;
		failure->column = edata->column_name ? pstrdup(edata->column_name) : NULL;
		FreeErrorData(edata);
		memset(rows, 0, sizeof(*rows));
		ok = false;
	}
	PG_END_TRY();
	return ok;
}

const WsSqlRoutine SpiWsSql = {
	.name = "spi",
	.begin = spi_begin_scope,
	.end = spi_end_scope,
	.run = spi_run,
	.attempt = spi_attempt,
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
