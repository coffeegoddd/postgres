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

static void run_command(ProllyRemote *r, const char *sql, const char *what);

/* Run a statement there; its result, which the caller clears. */
static PGresult *
run(ProllyRemote *r, const char *sql, int nparams, const Oid *types,
	const char *const *values, const int *lengths, const int *formats,
	int result_format, const char *what)
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
							   formats, result_format, we_result);
	status = PQresultStatus(res);
	if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK)
		remote_error(r, res, what);
	return res;
}

static void
run_command(ProllyRemote *r, const char *sql, const char *what)
{
	PQclear(run(r, sql, 0, NULL, NULL, NULL, NULL, 0, what));
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

/* hashes[n] as a bytea[] in binary. */
static void
bytea_array(StringInfo out, const ProllyHash *const *hashes,
			const StringInfoData *const *chunks, int n)
{
	pq_sendint32(out, 1);		/* dimensions */
	pq_sendint32(out, 0);		/* no nulls */
	pq_sendint32(out, BYTEAOID);
	pq_sendint32(out, n);
	pq_sendint32(out, 1);		/* lower bound */
	for (int i = 0; i < n; i++)
	{
		if (hashes)
		{
			pq_sendint32(out, hashes[i]->len);
			pq_sendbytes(out, hashes[i]->bytes, hashes[i]->len);
		}
		else
		{
			pq_sendint32(out, chunks[i]->len);
			pq_sendbytes(out, chunks[i]->data, chunks[i]->len);
		}
	}
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

static void
libpq_format(ProllyRemote *r, StringInfo out)
{
	PGresult   *res = run(r, "SELECT pg_catalog.dolt_serve_format()", 0, NULL,
						  NULL, NULL, NULL, 0, "reading the format");

	appendStringInfoString(out, PQgetvalue(res, 0, 0));
	PQclear(res);
}

static void
libpq_heads(ProllyRemote *r, ProllyRemoteHeadVisit visit, void *arg)
{
	PGresult   *res = run(r, "SELECT branch, commit_id FROM pg_catalog.dolt_serve_heads() "
						  "ORDER BY branch COLLATE \"C\"",
						  0, NULL, NULL, NULL, NULL, 1, "reading the heads");

	PG_TRY();
	{
		for (int i = 0; i < PQntuples(res); i++)
		{
			char	   *branch = pnstrdup(PQgetvalue(res, i, 0),
										  PQgetlength(res, i, 0));
			ProllyHash	commit;

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
	StringInfoData param;
	const char *values[1];
	int			lengths[1];
	int			formats[1] = {1};
	Oid			types[1] = {BYTEAARRAYOID};
	PGresult   *res;
	bool		found;

	initStringInfo(&param);
	bytea_array(&param, &hash, NULL, 1);
	values[0] = param.data;
	lengths[0] = param.len;
	res = run(r, "SELECT chunk FROM pg_catalog.dolt_serve_get($1)", 1, types,
			  values, lengths, formats, 1, "reading a chunk");
	found = PQntuples(res) > 0;
	if (found)
		appendBinaryStringInfo(out, PQgetvalue(res, 0, 0), PQgetlength(res, 0, 0));
	PQclear(res);
	return found;
}

static bool
libpq_has(ProllyRemote *r, const ProllyHash *hash)
{
	StringInfoData param;
	const char *values[1];
	int			lengths[1];
	int			formats[1] = {1};
	Oid			types[1] = {BYTEAARRAYOID};
	PGresult   *res;
	bool		has;

	initStringInfo(&param);
	bytea_array(&param, &hash, NULL, 1);
	values[0] = param.data;
	lengths[0] = param.len;
	res = run(r, "SELECT (pg_catalog.dolt_serve_has($1))[1]", 1, types, values,
			  lengths, formats, 0, "asking for a chunk");
	has = strcmp(PQgetvalue(res, 0, 0), "t") == 0;
	PQclear(res);
	return has;
}

/* hashes as a bytea[] parameter, in binary */
static void
hashes_param(StringInfo out, const ProllyHash *hashes, int n)
{
	const ProllyHash **ptrs = palloc(sizeof(ProllyHash *) * Max(n, 1));

	for (int i = 0; i < n; i++)
		ptrs[i] = &hashes[i];
	bytea_array(out, ptrs, NULL, n);
	pfree(ptrs);
}

static void
libpq_get_many(ProllyRemote *r, const ProllyHash *hashes, int n,
			   ProllyRemoteChunkVisit visit, void *arg)
{
	StringInfoData param;
	const char *values[1];
	int			lengths[1];
	int			formats[1] = {1};
	Oid			types[1] = {BYTEAARRAYOID};
	PGresult   *res;

	initStringInfo(&param);
	hashes_param(&param, hashes, n);
	values[0] = param.data;
	lengths[0] = param.len;
	res = run(r, "SELECT hash, chunk FROM pg_catalog.dolt_serve_get($1)", 1,
			  types, values, lengths, formats, 1, "reading chunks");
	pfree(param.data);
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
	StringInfoData param;
	const char *values[1];
	int			lengths[1];
	int			formats[1] = {1};
	Oid			types[1] = {BYTEAARRAYOID};
	PGresult   *res;

	initStringInfo(&param);
	hashes_param(&param, hashes, n);
	values[0] = param.data;
	lengths[0] = param.len;
	res = run(r, "SELECT h FROM unnest(pg_catalog.dolt_serve_has($1)) WITH ORDINALITY AS a(h, i) "
			  "ORDER BY i", 1, types, values, lengths, formats, 0,
			  "asking for chunks");
	pfree(param.data);
	if (PQntuples(res) != n)
	{
		PQclear(res);
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("remote %s answered for %d chunks of %d", r->where,
						PQntuples(res), n)));
	}
	for (int i = 0; i < n; i++)
		has[i] = strcmp(PQgetvalue(res, i, 0), "t") == 0;
	PQclear(res);
}

/* Send a batch of a push's chunks. */
static void
send_batch(ProllyRemote *r, ProllyHash **hashes, StringInfoData **chunks, int n)
{
	StringInfoData h;
	StringInfoData c;
	const char *values[2];
	int			lengths[2];
	int			formats[2] = {1, 1};
	Oid			types[2] = {BYTEAARRAYOID, BYTEAARRAYOID};

	initStringInfo(&h);
	initStringInfo(&c);
	bytea_array(&h, (const ProllyHash *const *) hashes, NULL, n);
	bytea_array(&c, NULL, (const StringInfoData *const *) chunks, n);
	values[0] = h.data;
	lengths[0] = h.len;
	values[1] = c.data;
	lengths[1] = c.len;
	PQclear(run(r, "SELECT pg_catalog.dolt_serve_receive($1, $2)", 2, types,
				values, lengths, formats, 0, "sending chunks"));
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
	ProllyHash **hashes = palloc(sizeof(ProllyHash *) * BATCH_CHUNKS);
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
			hashes[n] = h;
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
			hashes = palloc(sizeof(ProllyHash *) * BATCH_CHUNKS);
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
			  5, types, values, lengths, formats, 0, "moving a branch");
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
};
