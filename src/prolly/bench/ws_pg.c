/*
 * ws_pg.c
 *	  The server: connections, psql and pg_dump, and files.
 */
#include "ws.h"

/*
 * Faults the checks must catch.  Each names a way a rebuild could go wrong:
 *	row			a table loses a row
 *	sequence	sequence positions are not restored
 *	matview		post-data is dumped schema-only, which drops the REFRESH of
 *				populated materialized views
 *	nohide		queries are planned with every index visible, so the plan
 *				may answer through some other index than the one named
 * and with --stream:
 *	cast		rows the event trigger sends are formatted with casts to text,
 *				which differ from output functions (bool, char(n), inet)
 *	isnull		they test NULL with IS NULL, true of a composite value whose
 *				fields are all NULL
 *	toast		unchanged TOASTed values are not taken from the current row
 *	noreemit	rows sent after a rewriting DDL statement are ignored
 *	noflush		the barrier is not flushed, so it waits in the WAL until
 *				something else flushes it (caught by checking the WAL is
 *				flushed past it)
 *	nogenpub	the stream is not sent stored generated columns
 * and with --merge:
 *	notags		objects keep column names, so an index or view one branch adds
 *				does not follow a rename on the other
 *	noviol		constraint violations are found but not stored
 *	noregen		stored generated columns are copied from a side, never
 *				computed for a row the merge made
 * and with --move:
 *	keepall		every changed row is taken to be as the schema change left it,
 *				so none is rewritten
 */
bool
broken(const char *what)
{
	return o.brk && strcmp(o.brk, what) == 0;
}

void
die(const char *fmt, const char *a)
{
	fprintf(stderr, "rebuild: ");
	fprintf(stderr, fmt, a);
	fprintf(stderr, "\n");
	exit(1);
}

/* ------------------------------------------------------------ the server */

PGconn *
pg_open(const char *db)
{
	char		ci[512];
	PGconn	   *c;
	PGresult   *res;

	snprintf(ci, sizeof(ci), "%s dbname=%s", o.conninfo, db);
	c = PQconnectdb(ci);
	if (PQstatus(c) != CONNECTION_OK)
		die("connect: %s", PQerrorMessage(c));
	PQsetNoticeProcessor(c, NULL, NULL);
	res = PQexec(c, SESSION_SETTINGS);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		die("settings: %s", PQerrorMessage(c));
	PQclear(res);
	return c;
}

PGresult *
pg_exec(PGconn *c, const char *sql)
{
	PGresult   *res = PQexec(c, sql);
	ExecStatusType st = PQresultStatus(res);

	if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK)
	{
		fprintf(stderr, "rebuild: %s\n", sql);
		die("%s", PQerrorMessage(c));
	}
	return res;
}

void
pg_cmd(PGconn *c, const char *sql)
{
	PQclear(pg_exec(c, sql));
}

/* Runs a command line; true when it exits 0. */
bool
run(const char *cmd)
{
	int			rc = system(cmd);

	return rc == 0;
}

void
psql_file(const char *db, const char *file)
{
	char		cmd[2048];

	snprintf(cmd, sizeof(cmd),
			 "%s/psql -X -q -v ON_ERROR_STOP=1 -d '%s dbname=%s' -f '%s' >/dev/null",
			 o.bindir, o.conninfo, db, file);
	if (!run(cmd))
		die("psql failed on %s", file);
}

void
fresh_database(const char *db)
{
	static bool made = false;
	PGconn	   *c = pg_open("postgres");
	char		sql[256],
				file[1024];

	/*
	 * A template with the extensions, made once per run: copying it is far
	 * quicker than installing them in every database.
	 */
	if (!made)
	{
		pg_cmd(c, "DROP DATABASE IF EXISTS " TEMPLATE_DB);
		pg_cmd(c, "CREATE DATABASE " TEMPLATE_DB " TEMPLATE template0");
		snprintf(file, sizeof(file), "%s/setup.sql", o.fixture);
		psql_file(TEMPLATE_DB, file);
		made = true;
	}
	snprintf(sql, sizeof(sql), "DROP DATABASE IF EXISTS %s", db);
	pg_cmd(c, sql);
	snprintf(sql, sizeof(sql), "CREATE DATABASE %s TEMPLATE " TEMPLATE_DB, db);
	pg_cmd(c, sql);
	PQfinish(c);
}

/* Reads a command's whole output. */
char *
slurp_cmd(const char *cmd, size_t *len)
{
	FILE	   *f = popen(cmd, "r");
	Buf			b = {0};
	char		tmp[65536];
	size_t		n;

	if (f == NULL)
		die("cannot run %s", cmd);
	while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0)
		buf_append(&b, tmp, n);
	if (pclose(f) != 0)
		die("failed: %s", cmd);
	buf_append(&b, "", 1);
	*len = b.len - 1;
	return (char *) b.data;
}

char *
slurp_file(const char *path, size_t *len)
{
	FILE	   *f = fopen(path, "r");
	Buf			b = {0};
	char		tmp[65536];
	size_t		n;

	if (f == NULL)
		die("cannot read %s", path);
	while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0)
		buf_append(&b, tmp, n);
	fclose(f);
	buf_append(&b, "", 1);
	*len = b.len - 1;
	return (char *) b.data;
}

void
write_file(const char *path, const void *data, size_t len)
{
	FILE	   *f = fopen(path, "w");

	if (f == NULL || fwrite(data, 1, len, f) != len || fclose(f) != 0)
		die("cannot write %s", path);
}

/*
 * One pg_dump section of the schema, without the per-dump random
 * \restrict key lines, so equal schemas give equal text.  Post-data is
 * dumped with data on: that section holds no rows, but it is where a
 * populated materialized view's REFRESH lives, which schema-only drops.
 */
/* When set, dumps read the database as of this exported snapshot. */
char		dump_snapshot[64];

char *
dump_section(const char *db, const char *section, size_t *len)
{
	char		cmd[2048],
				snap[96] = "";
	char	   *raw,
			   *out,
			   *p;
	size_t		rawlen,
				n = 0;

	if (dump_snapshot[0])
		snprintf(snap, sizeof(snap), "--snapshot=%s", dump_snapshot);
	snprintf(cmd, sizeof(cmd),
			 "%s/pg_dump %s %s --section=%s -n %s -d '%s dbname=%s'",
			 o.bindir, snap,
			 strcmp(section, "pre-data") == 0 || broken("matview") ? "--schema-only" : "",
			 section, SCHEMA, o.conninfo, db);
	raw = slurp_cmd(cmd, &rawlen);
	out = px_malloc(rawlen + 1);
	for (p = raw; *p;)
	{
		char	   *nl = strchr(p, '\n');
		size_t		ll = nl ? (size_t) (nl - p + 1) : strlen(p);

		if (strncmp(p, "\\restrict ", 10) != 0 && strncmp(p, "\\unrestrict ", 12) != 0)
		{
			memcpy(out + n, p, ll);
			n += ll;
		}
		p += ll;
	}
	out[n] = '\0';
	free(raw);
	*len = n;
	return out;
}

