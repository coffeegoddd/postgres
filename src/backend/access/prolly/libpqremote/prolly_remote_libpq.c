/*-------------------------------------------------------------------------
 *
 * prolly_remote_libpq.c
 *	  A remote that is another database, over a libpq connection: what a
 *	  fetch, push or clone asks, asked of the functions it serves
 *	  (dolt_serve_*).
 *
 * The URL is a libpq connection string or URI (postgresql://host/db), so
 * pg_hba.conf, roles, passwords and TLS are the remote server's as for any
 * client.  A role that isn't a superuser must give a password, and the
 * connection must use it, as dblink asks: the server's own credentials
 * (a .pgpass file, trust or peer authentication) aren't lent to it.
 *
 * A remote opened is one transaction there: a push's chunks, sent in
 * batches, are kept there until its head is moved, which commits it.
 * Hashes and chunks travel as bytea, in binary.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/libpqremote/prolly_remote_libpq.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_chunkstore.h"
#include "access/prolly_remote.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "libpq-fe.h"
#include "libpq/libpq-be-fe-helpers.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

PG_MODULE_MAGIC_EXT(
					.name = "prolly_remote_libpq",
					.version = PG_VERSION
);

/* A batch of a push's chunks: at most this many, or this many bytes. */
#define BATCH_CHUNKS	4096
#define BATCH_BYTES		(8 * 1024 * 1024)

struct ProllyRemote
{
	PGconn	   *conn;
	char	   *where;			/* host and database, for messages */
	bool		in_transaction; /* a BEGIN sent, not ended */
	int			has_since;		/* dolt_serve_since() there: -1 not asked */
	char	   *format;			/* its canonical format, once read */
	bool		takes_lz4;		/* it decompresses what it is sent */
};

static void read_format(ProllyRemote *r);

/* A file being read: its rows, one at a time, as they come. */
struct ProllyRemoteFile
{
	ProllyRemote *r;
	bool		checked;		/* the rows' description */
	bool		done;			/* every row read, the query ended */
};

static uint32 we_connect;
static uint32 we_result;

static const ProllyRemoteRoutine libpq_routine;

void
_PG_init(void)
{
	if (prolly_remote_libpq_routine != NULL)
		elog(ERROR, "prolly_remote_libpq already loaded");
	prolly_remote_libpq_routine = &libpq_routine;
}

/* ------------------------------------------------------------------------
 * The connection
 * ------------------------------------------------------------------------
 */

static PQconninfoOption *
parse_url(const char *url)
{
	char	   *err = NULL;
	PQconninfoOption *opts = PQconninfoParse(url, &err);

	if (opts == NULL)
	{
		char	   *msg = err ? pchomp(err) : pstrdup("out of memory");

		if (err)
			PQfreemem(err);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid remote URL: %s", msg)));
	}
	return opts;
}

static void
libpq_check_url(const char *url)
{
	PQconninfoFree(parse_url(url));
}

/*
 * A role that isn't a superuser gives the password in the URL: the
 * server's .pgpass, or its trust or peer authentication, aren't its.
 */
static void
check_password_given(const char *url)
{
	PQconninfoOption *opts;
	bool		given = false;

	if (superuser())
		return;
	opts = parse_url(url);
	for (PQconninfoOption *o = opts; o->keyword; o++)
		if (strcmp(o->keyword, "password") == 0 && o->val && o->val[0] != '\0')
			given = true;
	PQconninfoFree(opts);
	if (!given)
		ereport(ERROR,
				(errcode(ERRCODE_S_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED),
				 errmsg("password is required"),
				 errdetail("Non-superusers must give a password in a remote's URL.")));
}

/* An error the remote raised, as it raised it (its SQLSTATE kept). */
static void
remote_error(ProllyRemote *r, PGresult *res, const char *what)
{
	char	   *sqlstate = res ? PQresultErrorField(res, PG_DIAG_SQLSTATE) : NULL;
	char	   *primary = res ? PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY) : NULL;
	char	   *detail = res ? PQresultErrorField(res, PG_DIAG_MESSAGE_DETAIL) : NULL;
	char	   *hint = res ? PQresultErrorField(res, PG_DIAG_MESSAGE_HINT) : NULL;
	int			code = ERRCODE_CONNECTION_FAILURE;
	char	   *message;

	if (sqlstate && strlen(sqlstate) == 5)
		code = MAKE_SQLSTATE(sqlstate[0], sqlstate[1], sqlstate[2],
							 sqlstate[3], sqlstate[4]);
	message = pstrdup(primary ? primary : pchomp(PQerrorMessage(r->conn)));
	detail = detail ? pstrdup(detail) : NULL;
	hint = hint ? pstrdup(hint) : NULL;
	PQclear(res);
	ereport(ERROR,
			(errcode(code),
			 errmsg("%s", message),
			 detail ? errdetail_internal("%s", detail) : 0,
			 hint ? errhint("%s", hint) : 0,
			 errcontext("%s on remote %s", what, r->where)));
}

/*
 * What a statement's result is to be: its columns' types, all in one
 * format, and between min_rows and max_rows rows (any number, if max_rows
 * is negative), none of them null.  A remote is any server the URL names,
 * so a result is checked against it before anything reads it: libpq's
 * value of a row or column a result hasn't is NULL, and of a null is "".
 */
typedef struct ResultShape
{
	int			min_rows;
	int			max_rows;
	int			format;			/* 0 text, 1 binary */
	int			ncols;
	Oid			types[4];
} ResultShape;

static const ResultShape one_text = {1, 1, 0, 1, {TEXTOID}};
static const ResultShape one_bool = {1, 1, 0, 1, {BOOLOID}};
static const ResultShape chunk_rows = {0, -1, 1, 2, {BYTEAOID, BYTEAOID}};

/* (a column's type and format, which libpq-be-fe.h doesn't wrap) */
static inline Oid
result_ftype(const PGresult *res, int col)
{
	return PQftype(res->res, col);
}

static inline int
result_fformat(const PGresult *res, int col)
{
	return PQfformat(res->res, col);
}

/*
 * An ERROR if res's columns aren't shape's; res the caller's to clear, on
 * an error too, as in the checks below.
 */
static void
result_columns_check(ProllyRemote *r, const PGresult *res,
					 const ResultShape *shape, const char *what)
{
	int			ncols = PQnfields(res);

	if (ncols != shape->ncols)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_plural("remote %s answered with %d column, not %d",
							   "remote %s answered with %d columns, not %d",
							   ncols, r->where, ncols, shape->ncols),
				 errcontext("%s", what)));
	for (int c = 0; c < ncols; c++)
	{
		if (result_ftype(res, c) != shape->types[c])
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("remote %s answered with type %u in column %d, not %u",
							r->where, result_ftype(res, c), c + 1, shape->types[c]),
					 errcontext("%s", what)));
		if (result_fformat(res, c) != shape->format)
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("remote %s answered with format %d in column %d, not %d",
							r->where, result_fformat(res, c), c + 1, shape->format),
					 errcontext("%s", what)));
	}
}

/* An ERROR if any of res's values is null. */
static void
result_nulls_check(ProllyRemote *r, const PGresult *res, const char *what)
{
	for (int i = 0; i < PQntuples(res); i++)
		for (int c = 0; c < PQnfields(res); c++)
			if (PQgetisnull(res, i, c))
				ereport(ERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("remote %s answered with a null in row %d, column %d",
								r->where, i + 1, c + 1),
						 errcontext("%s", what)));
}

/* An ERROR if res isn't shape. */
static void
result_check(ProllyRemote *r, const PGresult *res, const ResultShape *shape,
			 const char *what)
{
	int			nrows = PQntuples(res);

	result_columns_check(r, res, shape, what);
	if (nrows < shape->min_rows ||
		(shape->max_rows >= 0 && nrows > shape->max_rows))
	{
		if (shape->min_rows == shape->max_rows)
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg_plural("remote %s answered with %d row, not %d",
								   "remote %s answered with %d rows, not %d",
								   nrows, r->where, nrows, shape->min_rows),
					 errcontext("%s", what)));
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_plural("remote %s answered with %d row, not %d to %d",
							   "remote %s answered with %d rows, not %d to %d",
							   nrows, r->where, nrows, shape->min_rows,
							   shape->max_rows),
				 errcontext("%s", what)));
	}
	result_nulls_check(r, res, what);
}

static void run_command(ProllyRemote *r, const char *sql, const char *what);

/*
 * Run a statement there; its result, which the caller clears, checked to
 * be shape (a command's, if NULL: nothing in it is read).
 */
static PGresult *
run(ProllyRemote *r, const char *sql, int nparams, const Oid *types,
	const char *const *values, const int *lengths, const int *formats,
	const ResultShape *shape, const char *what)
{
	PGresult   *res;
	ExecStatusType status;

	/* (the transaction a push's commit ended, begun again when needed) */
	if (!r->in_transaction)
	{
		r->in_transaction = true;
		run_command(r, "BEGIN", "beginning");
	}
	res = libpqsrv_exec_params(r->conn, sql, nparams, types, values, lengths,
							   formats, shape ? shape->format : 0, we_result);
	status = PQresultStatus(res);
	if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK)
		remote_error(r, res, what);
	if (shape)
	{
		PG_TRY();
		{
			result_check(r, res, shape, what);
		}
		PG_CATCH();
		{
			PQclear(res);
			PG_RE_THROW();
		}
		PG_END_TRY();
	}
	return res;
}

static void
run_command(ProllyRemote *r, const char *sql, const char *what)
{
	PQclear(run(r, sql, 0, NULL, NULL, NULL, NULL, NULL, what));
}

static ProllyRemote *
libpq_open(const char *url, bool create)
{
	ProllyRemote *r;
	MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);

	check_password_given(url);
	if (we_connect == 0)
		we_connect = WaitEventExtensionNew("ProllyRemoteConnect");
	if (we_result == 0)
		we_result = WaitEventExtensionNew("ProllyRemoteResult");
	r = palloc0(sizeof(ProllyRemote));
	r->has_since = -1;
	MemoryContextSwitchTo(old);

	r->conn = libpqsrv_connect(url, we_connect);
	if (PQstatus(r->conn) != CONNECTION_OK)
	{
		char	   *msg = pchomp(PQerrorMessage(r->conn));

		libpqsrv_disconnect(r->conn);
		ereport(ERROR,
				(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
				 errmsg("could not connect to the remote"),
				 errdetail_internal("%s", msg)));
	}
	r->where = MemoryContextStrdup(TopTransactionContext,
								   psprintf("\"%s\" at %s", PQdb(r->conn),
											PQhost(r->conn)));
	if (!superuser() && !PQconnectionUsedPassword(r->conn))
	{
		libpqsrv_disconnect(r->conn);
		ereport(ERROR,
				(errcode(ERRCODE_S_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED),
				 errmsg("password is required"),
				 errdetail("Non-superusers must authenticate to a remote with a password.")));
	}
	PG_TRY();
	{
		r->in_transaction = true;
		run_command(r, "BEGIN", "beginning");
	}
	PG_CATCH();
	{
		libpqsrv_disconnect(r->conn);
		PG_RE_THROW();
	}
	PG_END_TRY();
	return r;
}

/* Closed: what wasn't committed there (a push's head moved) isn't. */
static void
libpq_close(ProllyRemote *r)
{
	libpqsrv_disconnect(r->conn);
	r->conn = NULL;
}

/* ------------------------------------------------------------------------
 * Values
 * ------------------------------------------------------------------------
 */

/* An array's header, in binary: one dimension of n bytea, no nulls. */
static void
array_header(StringInfo out, int n)
{
	pq_sendint32(out, 1);
	pq_sendint32(out, 0);
	pq_sendint32(out, BYTEAOID);
	pq_sendint32(out, n);
	pq_sendint32(out, 1);		/* lower bound */
}

/* hashes[n] as a bytea[] in binary. */
static void
hashes_array(StringInfo out, const ProllyHash *hashes, int n)
{
	array_header(out, n);
	for (int i = 0; i < n; i++)
	{
		pq_sendint32(out, hashes[i].len);
		pq_sendbytes(out, hashes[i].bytes, hashes[i].len);
	}
}

/* chunks[n] as a bytea[] in binary. */
static void
chunks_array(StringInfo out, StringInfoData *const *chunks, int n)
{
	array_header(out, n);
	for (int i = 0; i < n; i++)
	{
		pq_sendint32(out, chunks[i]->len);
		pq_sendbytes(out, chunks[i]->data, chunks[i]->len);
	}
}

/* Run a statement whose one parameter is hashes, as a bytea[]. */
static PGresult *
run_hashes(ProllyRemote *r, const char *sql, const ProllyHash *hashes, int n,
		   const ResultShape *shape, const char *what)
{
	StringInfoData param;
	const char *values[1];
	int			lengths[1];
	int			formats[1] = {1};
	Oid			types[1] = {BYTEAARRAYOID};
	PGresult   *res;

	initStringInfo(&param);
	hashes_array(&param, hashes, n);
	values[0] = param.data;
	lengths[0] = param.len;
	res = run(r, sql, 1, types, values, lengths, formats, shape, what);
	pfree(param.data);
	return res;
}

static void
result_hash(ProllyRemote *r, PGresult *res, int row, int col, ProllyHash *h)
{
	int			len = PQgetlength(res, row, col);

	/* (res the caller's to clear, on an error too) */
	if (len <= 0 || len > PROLLY_HASH_MAX_LEN)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("remote %s sent a hash of %d bytes", r->where, len)));
	memset(h, 0, sizeof(*h));
	h->len = len;
	memcpy(h->bytes, PQgetvalue(res, row, col), len);
}

/* ------------------------------------------------------------------------
 * The callbacks
 * ------------------------------------------------------------------------
 */

/*
 * The remote's format, read once, and how it sends and takes chunks (what
 * follows its canonical format): one that doesn't say it frames them is of
 * a build before, and its format is given so, that a client refuses it as
 * of another.  This build without LZ4 asks for frames it can decompress.
 */
static void
read_format(ProllyRemote *r)
{
	PGresult   *res;
	char	   *f;
	char	   *framed;

	if (r->format != NULL)
		return;
	res = run(r, "SELECT pg_catalog.dolt_serve_format()", 0, NULL,
			  NULL, NULL, NULL, &one_text, "reading the format");
	f = MemoryContextStrdup(TopTransactionContext, PQgetvalue(res, 0, 0));
	PQclear(res);
	framed = strstr(f, PROLLY_SERVE_FRAMED);
	if (framed == NULL)
	{
		r->format = MemoryContextStrdup(TopTransactionContext,
										psprintf("%s (chunks sent unframed)", f));
		return;
	}
	r->takes_lz4 = strcmp(framed + strlen(PROLLY_SERVE_FRAMED),
						  PROLLY_SERVE_LZ4) == 0;
	*framed = '\0';
	r->format = f;
#ifndef USE_LZ4
	run_command(r, "SET prolly_serve_uncompressed = on", "asking for chunks uncompressed");
#endif
}

static void
libpq_format(ProllyRemote *r, StringInfo out)
{
	read_format(r);
	appendStringInfoString(out, r->format);
}

static void
libpq_heads(ProllyRemote *r, ProllyRemoteHeadVisit visit, void *arg)
{
	static const ResultShape shape = {0, -1, 1, 2, {TEXTOID, BYTEAOID}};
	PGresult   *res = run(r, "SELECT branch, commit_id FROM pg_catalog.dolt_serve_heads() "
						  "ORDER BY branch COLLATE \"C\"",
						  0, NULL, NULL, NULL, NULL, &shape, "reading the heads");

	PG_TRY();
	{
		for (int i = 0; i < PQntuples(res); i++)
		{
			const char *name = PQgetvalue(res, i, 0);
			int			len = PQgetlength(res, i, 0);
			char	   *branch;
			ProllyHash	commit;

			/* (the rest of a name the fetch checks: a NUL would cut it short) */
			if (memchr(name, '\0', len) != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("remote %s sent a branch name with a NUL in it",
								r->where),
						 errcontext("reading the heads")));
			branch = pnstrdup(name, len);
			result_hash(r, res, i, 1, &commit);
			visit(branch, &commit, arg);
		}
	}
	PG_FINALLY();
	{
		PQclear(res);
	}
	PG_END_TRY();
}

static bool
libpq_get(ProllyRemote *r, const ProllyHash *hash, StringInfo out)
{
	static const ResultShape shape = {0, 1, 1, 1, {BYTEAOID}};
	PGresult   *res;
	bool		found;

	read_format(r);
	res = run_hashes(r, "SELECT chunk FROM pg_catalog.dolt_serve_get($1)", hash, 1,
					 &shape, "reading a chunk");
	found = PQntuples(res) > 0;
	if (found)
		appendBinaryStringInfo(out, PQgetvalue(res, 0, 0), PQgetlength(res, 0, 0));
	PQclear(res);
	return found;
}

static bool
libpq_has(ProllyRemote *r, const ProllyHash *hash)
{
	PGresult   *res;
	bool		has;

	res = run_hashes(r, "SELECT (pg_catalog.dolt_serve_has($1))[1]", hash, 1,
					 &one_bool, "asking for a chunk");
	has = strcmp(PQgetvalue(res, 0, 0), "t") == 0;
	PQclear(res);
	return has;
}

static void
libpq_get_many(ProllyRemote *r, const ProllyHash *hashes, int n,
			   ProllyRemoteChunkVisit visit, void *arg)
{
	PGresult   *res;

	read_format(r);
	res = run_hashes(r, "SELECT hash, chunk FROM pg_catalog.dolt_serve_get($1)",
					 hashes, n, &chunk_rows, "reading chunks");
	PG_TRY();
	{
		for (int i = 0; i < PQntuples(res); i++)
		{
			ProllyHash	h;

			result_hash(r, res, i, 0, &h);
			visit(&h, PQgetvalue(res, i, 1), PQgetlength(res, i, 1), arg);
		}
	}
	PG_FINALLY();
	{
		PQclear(res);
	}
	PG_END_TRY();
}

static void
libpq_has_many(ProllyRemote *r, const ProllyHash *hashes, int n, bool *has)
{
	ResultShape shape = {n, n, 0, 1, {BOOLOID}};
	PGresult   *res;

	res = run_hashes(r, "SELECT h FROM unnest(pg_catalog.dolt_serve_has($1)) WITH ORDINALITY AS a(h, i) "
					 "ORDER BY i", hashes, n, &shape, "asking for chunks");
	for (int i = 0; i < n; i++)
		has[i] = strcmp(PQgetvalue(res, i, 0), "t") == 0;
	PQclear(res);
}

/*
 * What a store with the known commits lacks of the wanted ones, as the
 * remote tells: streamed a row at a time, not held whole.
 */
static void
libpq_get_since(ProllyRemote *r, const ProllyHash *wanted, int nwanted,
				const ProllyHash *known, int nknown,
				ProllyRemoteChunkVisit visit, void *arg)
{
	StringInfoData w;
	StringInfoData k;
	const char *values[2];
	int			lengths[2];
	int			formats[2] = {1, 1};
	Oid			types[2] = {BYTEAARRAYOID, BYTEAARRAYOID};
	PGresult   *res;
	bool		checked = false;

	/*
	 * (a remote of a build before it has no dolt_serve_since(): it tells
	 * nothing, and the walk reads what is lacked)
	 */
	if (r->has_since < 0)
	{
		res = run(r, "SELECT pg_catalog.to_regprocedure('pg_catalog.dolt_serve_since(bytea[], bytea[])') IS NOT NULL",
				  0, NULL, NULL, NULL, NULL, &one_bool, "asking what it serves");
		r->has_since = strcmp(PQgetvalue(res, 0, 0), "t") == 0;
		PQclear(res);
	}
	if (!r->has_since)
		return;
	read_format(r);

	initStringInfo(&w);
	initStringInfo(&k);
	hashes_array(&w, wanted, nwanted);
	hashes_array(&k, known, nknown);
	values[0] = w.data;
	lengths[0] = w.len;
	values[1] = k.data;
	lengths[1] = k.len;
	if (!PQsendQueryParams(r->conn,
						   "SELECT hash, chunk FROM pg_catalog.dolt_serve_since($1, $2)",
						   2, types, values, lengths, formats, 1) ||
		!PQsetSingleRowMode(r->conn))
		remote_error(r, NULL, "reading what changed");
	pfree(w.data);
	pfree(k.data);
	for (;;)
	{
		ExecStatusType status;

		res = libpqsrv_get_result(r->conn, we_result);
		if (res == NULL)
			break;
		status = PQresultStatus(res);
		if (status == PGRES_SINGLE_TUPLE)
		{
			ProllyHash	h;

			PG_TRY();
			{
				/*
				 * (one description for all the rows, checked with the first;
				 * each row's values, as it comes)
				 */
				if (!checked)
					result_columns_check(r, res, &chunk_rows, "reading what changed");
				checked = true;
				result_nulls_check(r, res, "reading what changed");
				result_hash(r, res, 0, 0, &h);
				visit(&h, PQgetvalue(res, 0, 1), PQgetlength(res, 0, 1), arg);
			}
			PG_CATCH();
			{
				/* (rows still coming: the connection isn't usable, so it goes) */
				PQclear(res);
				libpqsrv_disconnect(r->conn);
				r->conn = NULL;
				PG_RE_THROW();
			}
			PG_END_TRY();
		}
		else if (status != PGRES_TUPLES_OK)
		{
			PGresult   *rest;

			/* (the query's end read first: the connection is free again) */
			while ((rest = libpqsrv_get_result(r->conn, we_result)) != NULL)
				PQclear(rest);
			remote_error(r, res, "reading what changed");
		}
		PQclear(res);
	}
}

/*
 * Its files, whole (false: a remote of a build before them, which can't
 * list them).
 */
static bool
libpq_files(ProllyRemote *r, ProllyRemoteFileVisit visit, void *arg)
{
	static const ResultShape shape = {0, -1, 0, 4, {TEXTOID, TEXTOID, TEXTOID, TEXTOID}};
	PGresult   *res;
	bool		has;

	res = run(r, "SELECT pg_catalog.to_regprocedure('pg_catalog.dolt_serve_file_chunks(text)') IS NOT NULL",
			  0, NULL, NULL, NULL, NULL, &one_bool, "asking what it serves");
	has = strcmp(PQgetvalue(res, 0, 0), "t") == 0;
	PQclear(res);
	if (!has)
		return false;
	res = run(r, "SELECT key, chunks::text, bytes::text, journal::text "
			  "FROM pg_catalog.dolt_serve_files()",
			  0, NULL, NULL, NULL, NULL, &shape, "reading its files");
	PG_TRY();
	{
		for (int i = 0; i < PQntuples(res); i++)
		{
			char	   *key = pnstrdup(PQgetvalue(res, i, 0), PQgetlength(res, i, 0));
			char	   *end;
			uint64		chunks = strtou64(PQgetvalue(res, i, 1), &end, 10);
			uint64		bytes;

			if (*end != '\0' || chunks > PG_UINT32_MAX)
				ereport(ERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("remote %s sent a file's chunk count that isn't one",
								r->where)));
			bytes = strtou64(PQgetvalue(res, i, 2), &end, 10);
			if (*end != '\0')
				ereport(ERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("remote %s sent a file's size that isn't one", r->where)));
			visit(key, (uint32) chunks, bytes,
				  strcmp(PQgetvalue(res, i, 3), "true") == 0, arg);
			pfree(key);
		}
	}
	PG_FINALLY();
	{
		PQclear(res);
	}
	PG_END_TRY();
	return true;
}

static ProllyRemoteFile *
libpq_file_open(ProllyRemote *r, const char *key)
{
	ProllyRemoteFile *f;
	const char *values[1] = {key};
	Oid			types[1] = {TEXTOID};

	/* (the transaction the reads are in, as run() begins it) */
	if (!r->in_transaction)
	{
		r->in_transaction = true;
		run_command(r, "BEGIN", "beginning");
	}
	if (!PQsendQueryParams(r->conn,
						   "SELECT hash, chunk FROM pg_catalog.dolt_serve_file_chunks($1)",
						   1, types, values, NULL, NULL, 1) ||
		!PQsetSingleRowMode(r->conn))
		remote_error(r, NULL, "reading a file");
	f = palloc0(sizeof(ProllyRemoteFile));
	f->r = r;
	return f;
}

/* A connection a file was being read on, given up: rows may still come. */
static void
file_abandon(ProllyRemoteFile *f)
{
	libpqsrv_disconnect(f->r->conn);
	f->r->conn = NULL;
	f->done = true;
}

static bool
libpq_file_next(ProllyRemoteFile *f, ProllyHash *hash, StringInfo frame)
{
	ProllyRemote *r = f->r;

	while (!f->done)
	{
		PGresult   *res = libpqsrv_get_result(r->conn, we_result);
		ExecStatusType status;

		if (res == NULL)
		{
			f->done = true;
			break;
		}
		status = PQresultStatus(res);
		if (status == PGRES_SINGLE_TUPLE)
		{
			PG_TRY();
			{
				if (!f->checked)
					result_columns_check(r, res, &chunk_rows, "reading a file");
				f->checked = true;
				result_nulls_check(r, res, "reading a file");
				result_hash(r, res, 0, 0, hash);
				appendBinaryStringInfo(frame, PQgetvalue(res, 0, 1),
									   PQgetlength(res, 0, 1));
			}
			PG_CATCH();
			{
				PQclear(res);
				file_abandon(f);
				PG_RE_THROW();
			}
			PG_END_TRY();
			PQclear(res);
			return true;
		}
		if (status != PGRES_TUPLES_OK)
		{
			PGresult   *rest;

			while ((rest = libpqsrv_get_result(r->conn, we_result)) != NULL)
				PQclear(rest);
			f->done = true;
			remote_error(r, res, "reading a file");
		}
		PQclear(res);
	}
	return false;
}

static void
libpq_file_close(ProllyRemoteFile *f)
{
	/* (closed before its end, on an error: the connection goes) */
	if (!f->done && f->r->conn != NULL)
		file_abandon(f);
	pfree(f);
}

/* Send a batch of a push's chunks. */
static void
send_batch(ProllyRemote *r, const ProllyHash *hashes, StringInfoData **chunks, int n)
{
	StringInfoData h;
	StringInfoData c;
	const char *values[2];
	int			lengths[2];
	int			formats[2] = {1, 1};
	Oid			types[2] = {BYTEAARRAYOID, BYTEAARRAYOID};

	/* (a remote that can't decompress is sent every chunk plain) */
	read_format(r);
	if (!r->takes_lz4)
	{
		for (int i = 0; i < n; i++)
		{
			StringInfoData plain;

			initStringInfo(&plain);
			prolly_chunk_frame_plain(chunks[i]->data, chunks[i]->len, &plain);
			pfree(chunks[i]->data);
			*chunks[i] = plain;
		}
	}
	initStringInfo(&h);
	initStringInfo(&c);
	hashes_array(&h, hashes, n);
	chunks_array(&c, chunks, n);
	values[0] = h.data;
	lengths[0] = h.len;
	values[1] = c.data;
	lengths[1] = c.len;
	PQclear(run(r, "SELECT pg_catalog.dolt_serve_receive($1, $2)", 2, types,
				values, lengths, formats, NULL, "sending chunks"));
	pfree(h.data);
	pfree(c.data);
}

static void
libpq_add_chunks(ProllyRemote *r, ProllyChunkNext next, void *arg)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext,
											  "prolly push batch",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	ProllyHash *hashes = palloc(sizeof(ProllyHash) * BATCH_CHUNKS);
	StringInfoData **chunks = palloc(sizeof(StringInfoData *) * BATCH_CHUNKS);
	int			n = 0;
	Size		bytes = 0;

	MemoryContextSwitchTo(old);
	for (;;)
	{
		ProllyHash *h;
		StringInfoData *c;
		bool		more;

		old = MemoryContextSwitchTo(cxt);
		h = palloc(sizeof(ProllyHash));
		c = palloc(sizeof(StringInfoData));
		initStringInfo(c);
		MemoryContextSwitchTo(old);
		more = next(arg, h, c);
		if (more)
		{
			hashes[n] = *h;
			chunks[n] = c;
			n++;
			bytes += c->len;
		}
		if (n > 0 && (!more || n == BATCH_CHUNKS || bytes >= BATCH_BYTES))
		{
			send_batch(r, hashes, chunks, n);
			n = 0;
			bytes = 0;
			MemoryContextReset(cxt);
			old = MemoryContextSwitchTo(cxt);
			hashes = palloc(sizeof(ProllyHash) * BATCH_CHUNKS);
			chunks = palloc(sizeof(StringInfoData *) * BATCH_CHUNKS);
			MemoryContextSwitchTo(old);
		}
		if (!more)
			break;
		CHECK_FOR_INTERRUPTS();
	}
	MemoryContextDelete(cxt);
}

/*
 * Move the branch there, receiving what was sent first, and commit there:
 * false (nothing committed) if the branch isn't at expected.
 */
static bool
libpq_move_head(ProllyRemote *r, const char *branch, const ProllyHash *expected,
				const ProllyHash *head, const char *format, bool force)
{
	const char *values[5];
	int			lengths[5] = {0};
	int			formats[5] = {0, 1, 1, 0, 0};
	Oid			types[5] = {TEXTOID, BYTEAOID, BYTEAOID, TEXTOID, BOOLOID};
	PGresult   *res;
	bool		moved;

	values[0] = branch;
	values[1] = (const char *) expected->bytes;
	lengths[1] = expected->len;
	values[2] = head ? (const char *) head->bytes : NULL;
	lengths[2] = head ? head->len : 0;
	values[3] = format;
	values[4] = force ? "t" : "f";
	res = run(r, "SELECT pg_catalog.dolt_serve_move_head($1, $2, $3, $4, $5)",
			  5, types, values, lengths, formats, &one_bool, "moving a branch");
	moved = strcmp(PQgetvalue(res, 0, 0), "t") == 0;
	PQclear(res);
	run_command(r, moved ? "COMMIT" : "ROLLBACK", "ending the push");
	/* (the next statement begins again: a push committed is pushed) */
	r->in_transaction = false;
	return moved;
}

static const ProllyRemoteRoutine libpq_routine = {
	.info = {
		.iface = PROLLY_IFACE_REMOTE,
		.impl_id = 2,
		.format_version = 1,
		.name = "postgresql",
		.description = "another database, over a libpq connection",
	},
	.check_url = libpq_check_url,
	.open = libpq_open,
	.close = libpq_close,
	.format = libpq_format,
	.heads = libpq_heads,
	.get = libpq_get,
	.has = libpq_has,
	.add_chunks = libpq_add_chunks,
	.move_head = libpq_move_head,
	.get_many = libpq_get_many,
	.has_many = libpq_has_many,
	.get_since = libpq_get_since,
	.files = libpq_files,
	.file_open = libpq_file_open,
	.file_next = libpq_file_next,
	.file_close = libpq_file_close,
};
