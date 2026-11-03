/*-------------------------------------------------------------------------
 *
 * ws_sql.h
 *	  How the working-set model reads catalogs and runs statements: text in
 *	  and text out, under settings that make the text the same whatever the
 *	  session's are.
 *
 * A scope (begin to end) holds the settings a statement's output depends on:
 * a value's output text, and a definition the catalogs' functions print,
 * does not depend on the session (DateStyle, IntervalStyle, TimeZone,
 * extra_float_digits, bytea_output, search_path, lc_monetary,
 * quote_all_identifiers), and what is read is this branch's whole
 * (catalog_branch_filter, and row_security: a row a policy would hide is an
 * error, not a row left out).  So a commit means the same thing whoever
 * captured it.  The search path is pg_catalog then the session's temporary
 * schema, which cannot shadow the catalogs: a statement names its schemas.
 * It holds no setting that only input depends on.  A scope's end puts back
 * what it set, not what a statement set: a statement's SET outlives it.
 *
 * A statement and its parameters are valid in the database's encoding; its
 * parameters are text, $1 and on: it casts them to what it needs.  Each statement reads with a
 * snapshot of its own, so it sees what those before it changed; and, in READ
 * COMMITTED, what other sessions committed between them: a caller needing
 * one point in time is in REPEATABLE READ already.
 *
 * This is for catalogs and statements, not a table's rows: all of a
 * statement's rows are held in memory as text, and more than an int counts
 * is an error.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/ws/ws_sql.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_WS_SQL_H
#define PROLLY_WS_SQL_H

/* A statement's rows, as text; a NULL is a null pointer. */
typedef struct WsSqlRows
{
	int			nrows;
	int			ncols;
	char	  **values;			/* nrows * ncols, a row at a time */
	uint64		processed;		/* the rows a statement changed or returned */
} WsSqlRows;

/* The value at row r, column c. */
static inline const char *
ws_sql_value(const WsSqlRows *rows, int r, int c)
{
	return rows->values[r * rows->ncols + c];
}

/* A statement's failure; a field it does not have is NULL. */
typedef struct WsSqlFailure
{
	char		sqlstate[6];
	char	   *message;
	char	   *constraint;
	char	   *column;
} WsSqlFailure;

typedef struct WsSqlRoutine
{
	const char *name;
	/* The settings taken, until end; the level is end's to give back. */
	int			(*begin) (void);
	void		(*end) (int level);
	/* A statement run; its error is raised. */
	void		(*run) (const char *sql, int nparams, const char *const *params,
						WsSqlRows *rows);
	/*
	 * A statement run; if it fails, false, with what it changed undone and
	 * its failure in *failure.  A failure is the statement's own: its data, a
	 * constraint, its syntax or what it names, a limit it passed, or an error
	 * a function it ran raised.  Any other error (a cancel, a lock's timeout
	 * or deadlock, no memory or disk, corruption) is raised, as run's: so too
	 * a file not found, a C function's library's or a relation's segment's
	 * alike, which only its message tells apart.  A deferred constraint is
	 * checked as the transaction commits, not here.
	 */
	bool		(*attempt) (const char *sql, int nparams, const char *const *params,
							WsSqlRows *rows, WsSqlFailure *failure);
	/* The same of what fn does: statements, and the work between them. */
	bool		(*attempt_fn) (void (*fn) (void *arg), void *arg, WsSqlFailure *failure);
} WsSqlRoutine;

extern PGDLLIMPORT const WsSqlRoutine SpiWsSql;

/* How the working-set model runs its statements. */
extern PGDLLIMPORT const WsSqlRoutine *WsSql;

extern void ws_sql_rows_free(WsSqlRows *rows);
extern void ws_sql_failure_free(WsSqlFailure *failure);

#endif							/* PROLLY_WS_SQL_H */
