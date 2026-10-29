/*
 * rebuild.c
 *	  Whether the prolly tree holds everything Postgres needs to rebuild a
 *	  schema, every index included, at every commit.
 *
 * The fixture (bench/rebuild_fixture/) is a series of SQL files, each applied to
 * a source database and then committed: the schema's state is captured
 * into a root value and committed to a repo in a durable store.  After the
 * last commit the store is closed and reopened, and every commit in the
 * log is rebuilt from the store alone into an empty database.  Each
 * rebuild must:
 *
 *	- capture back to the same root value (the schema's definitions and
 *	  every row are identical, since root values are history independent);
 *	- pass amcheck: every heap and its TOAST, every B-tree (with the heap
 *	  and uniqueness checks) and every GIN index;
 *	- answer every check in checks.txt as the source did at that commit:
 *	  queries the plan must answer through a named index (the source
 *	  answers them with indexes off), queries on sequences and views, and
 *	  statements a constraint must reject;
 *
 * and across the rebuilds every index that amcheck cannot verify must have
 * answered some query.
 *
 * The working set reaches the prolly tree through a WorkingSetOps; the one
 * here is logical: definitions as pg_dump's pre-data and post-data
 * sections, rows as COPY text keyed by the primary key (the whole row, with
 * a count, for tables without one), and sequence positions.  Indexes are
 * not stored: Postgres builds them from the rows and their definitions.
 */
#define _DEFAULT_SOURCE
#include <ctype.h>
#include <inttypes.h>
#include <getopt.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libpq-fe.h>

#include "../test/test_util.h"

#ifndef PGBIN
#define PGBIN "/home/dustin/cursor_src/pg-v2-build/inst/bin"
#endif

#define SCHEMA "rt"
#define MAX_STEPS 64
#define MAX_CHECKS 512
#define MAX_INDEXES 1024

/* Settings under which row text is written and read. */
#define SESSION_SETTINGS \
	"SET DateStyle = ISO; SET IntervalStyle = postgres; SET TimeZone = 'UTC'; " \
	"SET extra_float_digits = 1; SET bytea_output = hex; SET search_path = ''"

typedef struct Opts
{
	const char *conninfo;
	const char *fixture;
	const char *work;
	const char *bindir;
	const char *src_db;
	const char *dst_db;
	const char *brk;			/* a deliberate fault, to show the checks catch it */
	int			steps;			/* 0: all */
} Opts;

static Opts o = {
	"host=/tmp port=5499 user=postgres",
	"bench/rebuild_fixture",
	"/home/dustin/pgv2-bench/rebuild",
	PGBIN,
	"rt_src",
	"rt_dst",
	NULL,
	0,
};

/*
 * Faults the checks must catch.  Each names a way a rebuild could go wrong:
 *	row			a table loses a row
 *	sequence	sequence positions are not restored
 *	matview		post-data is dumped schema-only, which drops the REFRESH of
 *				populated materialized views
 *	nohide		queries are planned with every index visible, so the plan
 *				may answer through some other index than the one named
 */
static bool
broken(const char *what)
{
	return o.brk && strcmp(o.brk, what) == 0;
}

static int	failures = 0;

static void
die(const char *fmt, const char *a)
{
	fprintf(stderr, "rebuild: ");
	fprintf(stderr, fmt, a);
	fprintf(stderr, "\n");
	exit(1);
}

static void
fail(const char *what, const char *detail)
{
	failures++;
	printf("FAIL %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
}

/* ------------------------------------------------------------ the server */

static PGconn *
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

static PGresult *
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

static void
pg_cmd(PGconn *c, const char *sql)
{
	PQclear(pg_exec(c, sql));
}

/* Runs a command line; true when it exits 0. */
static bool
run(const char *cmd)
{
	int			rc = system(cmd);

	return rc == 0;
}

static void
psql_file(const char *db, const char *file)
{
	char		cmd[2048];

	snprintf(cmd, sizeof(cmd),
			 "%s/psql -X -q -v ON_ERROR_STOP=1 -d '%s dbname=%s' -f '%s' >/dev/null",
			 o.bindir, o.conninfo, db, file);
	if (!run(cmd))
		die("psql failed on %s", file);
}

static void
fresh_database(const char *db)
{
	PGconn	   *c = pg_open("postgres");
	char		sql[256],
				file[1024];

	snprintf(sql, sizeof(sql), "DROP DATABASE IF EXISTS %s", db);
	pg_cmd(c, sql);
	snprintf(sql, sizeof(sql), "CREATE DATABASE %s TEMPLATE template0", db);
	pg_cmd(c, sql);
	PQfinish(c);
	snprintf(file, sizeof(file), "%s/setup.sql", o.fixture);
	psql_file(db, file);
}

/* Reads a command's whole output. */
static char *
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

static char *
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

static void
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
static char *
dump_section(const char *db, const char *section, size_t *len)
{
	char		cmd[2048];
	char	   *raw,
			   *out,
			   *p;
	size_t		rawlen,
				n = 0;

	snprintf(cmd, sizeof(cmd),
			 "%s/pg_dump %s --section=%s -n %s -d '%s dbname=%s'",
			 o.bindir,
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

/* ------------------------------------------------- working set <-> prolly */

/*
 * How a schema's working set becomes a root value and back.  capture reads
 * the schema in a database into a root value; restore builds it, from the
 * root value alone, into a database holding nothing but the extensions.
 */
typedef struct WorkingSetOps
{
	const char *name;
	PHash		(*capture) (ProllyCtx *ctx, const char *db);
	void		(*restore) (ProllyCtx *ctx, const PHash *rv, const char *db);
} WorkingSetOps;

/*
 * The logical working set.  Entries of the root value:
 *	~pre, ~post		pg_dump's pre-data and post-data sections, as the schema
 *	~seq			sequence name -> "last_value \t is_called"
 *	<relation>		one per table holding rows (leaf partitions included);
 *					its schema is the codec layout, its data the rows
 * The codec layout: "rel <quoted name>\nkey <n>\ncols <m>\nlist <columns>\n".
 * Rows are COPY text with the key columns first: with a primary key the key
 * is the first n fields and the value the rest; without one (n = 0) the
 * key is the whole row and the value how many times it occurs.  Stored and
 * virtual generated columns are not kept; Postgres computes them again.
 */

typedef struct Row
{
	char	   *line;
	size_t		len;
	size_t		klen;			/* key bytes; the value starts after a tab */
} Row;

static int
row_cmp(const void *a, const void *b)
{
	const Row  *x = a,
			   *y = b;

	return key_cmp((const uint8_t *) x->line, x->klen, (const uint8_t *) y->line, y->klen);
}

/* The byte length of the first n tab-separated fields of a COPY text line. */
static size_t
fields_len(const char *line, size_t len, int n)
{
	for (size_t i = 0; i < len; i++)
		if (line[i] == '\t' && --n == 0)
			return i;
	return len;
}

static PHash
capture_rows(ProllyCtx *ctx, PGconn *c, const char *rel, const char *list, int nkey)
{
	char	   *sql = px_malloc(strlen(list) + strlen(rel) + 128);
	PGresult   *res;
	Row		   *rows = NULL;
	size_t		n = 0,
				cap = 0,
				nkv = 0;
	char	   *line;
	int			len;
	ProllyKV   *kvs;
	char	   *counts;
	PHash		out;

	sprintf(sql, "COPY (SELECT %s FROM ONLY %s.%s) TO STDOUT", list, SCHEMA, rel);
	res = PQexec(c, sql);
	if (PQresultStatus(res) != PGRES_COPY_OUT)
		die("copy out: %s", PQerrorMessage(c));
	PQclear(res);
	while ((len = PQgetCopyData(c, &line, 0)) > 0)
	{
		px_grow((void **) &rows, &cap, n + 1, sizeof(Row));
		rows[n].len = (size_t) len - 1;	/* drop the newline */
		rows[n].line = px_memdup(line, rows[n].len);
		rows[n].klen = nkey ? fields_len(line, rows[n].len, nkey) : rows[n].len;
		PQfreemem(line);
		n++;
	}
	res = PQgetResult(c);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		die("copy out: %s", PQerrorMessage(c));
	PQclear(res);
	qsort(rows, n, sizeof(Row), row_cmp);
	kvs = px_malloc((n ? n : 1) * sizeof(ProllyKV));
	counts = px_malloc((n ? n : 1) * 24);
	for (size_t i = 0; i < n;)
	{
		size_t		j = i + 1;

		if (nkey == 0)
			while (j < n && row_cmp(&rows[i], &rows[j]) == 0)
				j++;
		else if (j < n && row_cmp(&rows[i], &rows[j]) == 0)
			die("duplicate key in %s", rel);
		kvs[nkv].key = (const uint8_t *) rows[i].line;
		kvs[nkv].klen = rows[i].klen;
		if (nkey == 0)
		{
			char	   *cnt = counts + nkv * 24;

			kvs[nkv].vlen = (size_t) sprintf(cnt, "%zu", j - i);
			kvs[nkv].val = (const uint8_t *) cnt;
		}
		else
		{
			size_t		vs = rows[i].klen < rows[i].len ? rows[i].klen + 1 : rows[i].len;

			kvs[nkv].val = (const uint8_t *) rows[i].line + vs;
			kvs[nkv].vlen = rows[i].len - vs;
		}
		nkv++;
		i = j;
	}
	out = t_build(ctx, kvs, nkv);
	for (size_t i = 0; i < n; i++)
		free(rows[i].line);
	free(rows);
	free(kvs);
	free(counts);
	free(sql);
	return out;
}

static PHash
capture_sequences(ProllyCtx *ctx, PGconn *c)
{
	PGresult   *res = pg_exec(c,
							  "SELECT c.relname FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
							  "WHERE n.nspname = '" SCHEMA "' AND c.relkind = 'S' ORDER BY c.relname COLLATE \"C\"");
	int			n = PQntuples(res);
	ProllyKV   *kvs = px_malloc((n ? n : 1) * sizeof(ProllyKV));
	char	  **vals = px_malloc((n ? n : 1) * sizeof(char *));
	PHash		out;

	for (int i = 0; i < n; i++)
	{
		const char *name = PQgetvalue(res, i, 0);
		PGresult   *r3;
		char		sql[512];

		snprintf(sql, sizeof(sql), "SELECT last_value, is_called FROM %s.\"%s\"", SCHEMA, name);
		r3 = pg_exec(c, sql);
		vals[i] = px_malloc(64);
		snprintf(vals[i], 64, "%s\t%s", PQgetvalue(r3, 0, 0), PQgetvalue(r3, 0, 1));
		PQclear(r3);
		kvs[i].key = (const uint8_t *) name;
		kvs[i].klen = strlen(name);
		kvs[i].val = (const uint8_t *) vals[i];
		kvs[i].vlen = strlen(vals[i]);
	}
	out = t_build(ctx, kvs, (size_t) n);
	for (int i = 0; i < n; i++)
		free(vals[i]);
	free(vals);
	free(kvs);
	PQclear(res);
	return out;
}

static PHash
put_table(ProllyCtx *ctx, PHash rv, const char *name, const char *schema, size_t slen,
		  const PHash *data)
{
	CHECK(vc_table_create(ctx, &rv, name, schema, slen, &rv) == 0);
	if (data)
		CHECK(vc_table_set_data(ctx, &rv, name, data, &rv) == 0);
	return rv;
}

static PHash
logical_capture(ProllyCtx *ctx, const char *db)
{
	PGconn	   *c = pg_open(db);
	PHash		rv = vc_root_empty(ctx),
				seq;
	size_t		len;
	char	   *ddl;
	PGresult   *res;

	ddl = dump_section(db, "pre-data", &len);
	rv = put_table(ctx, rv, "~pre", ddl, len, NULL);
	free(ddl);
	ddl = dump_section(db, "post-data", &len);
	rv = put_table(ctx, rv, "~post", ddl, len, NULL);
	free(ddl);
	seq = capture_sequences(ctx, c);
	rv = put_table(ctx, rv, "~seq", "", 0, &seq);

	/* Key columns first (the primary key's, in its order), then the rest. */
	res = pg_exec(c,
				  "WITH rel AS (SELECT c.oid, c.relname FROM pg_class c "
				  "  JOIN pg_namespace n ON n.oid = c.relnamespace "
				  "  WHERE n.nspname = '" SCHEMA "' AND c.relkind = 'r'), "
				  "pk AS (SELECT i.indrelid, k.attnum, k.ord FROM pg_index i, "
				  "  unnest(i.indkey::int2[]) WITH ORDINALITY k(attnum, ord) "
				  "  WHERE i.indisprimary AND k.ord <= i.indnkeyatts), "
				  "col AS (SELECT r.oid, a.attname, a.attnum, p.ord FROM rel r "
				  "  JOIN pg_attribute a ON a.attrelid = r.oid AND a.attnum > 0 "
				  "    AND NOT a.attisdropped AND a.attgenerated = '' "
				  "  LEFT JOIN pk p ON p.indrelid = r.oid AND p.attnum = a.attnum) "
				  "SELECT r.relname, quote_ident(r.relname), "
				  "  (SELECT count(*) FROM col WHERE col.oid = r.oid AND ord IS NOT NULL), "
				  "  (SELECT count(*) FROM col WHERE col.oid = r.oid), "
				  "  (SELECT string_agg(quote_ident(attname), ', ' ORDER BY ord NULLS LAST, attnum) "
				  "     FROM col WHERE col.oid = r.oid) "
				  "FROM rel r ORDER BY r.relname COLLATE \"C\"");
	for (int i = 0; i < PQntuples(res); i++)
	{
		const char *name = PQgetvalue(res, i, 0),
				   *quoted = PQgetvalue(res, i, 1),
				   *list = PQgetvalue(res, i, 4);
		int			nkey = atoi(PQgetvalue(res, i, 2));
		char	   *layout = px_malloc(strlen(quoted) + strlen(list) + 128);
		PHash		data;
		int			ll;

		ll = sprintf(layout, "rel %s\nkey %d\ncols %s\nlist %s\n", quoted, nkey,
					 PQgetvalue(res, i, 3), list);
		data = capture_rows(ctx, c, quoted, list, nkey);
		rv = put_table(ctx, rv, name, layout, (size_t) ll, &data);
		free(layout);
	}
	PQclear(res);
	PQfinish(c);
	return rv;
}

typedef struct TableList
{
	char	  **names;
	VcTable    *t;
	size_t		n,
				cap,
				tcap;
} TableList;

static void
table_list_fn(void *arg, const VcTableChange *ch)
{
	TableList  *l = arg;

	if (!ch->has_to)
		return;
	px_grow((void **) &l->names, &l->cap, l->n + 1, sizeof(char *));
	px_grow((void **) &l->t, &l->tcap, l->n + 1, sizeof(VcTable));
	l->names[l->n] = px_malloc(ch->nlen + 1);
	memcpy(l->names[l->n], ch->name, ch->nlen);
	l->names[l->n][ch->nlen] = '\0';
	l->t[l->n++] = ch->to;
}

static void
table_list(ProllyCtx *ctx, const PHash *rv, TableList *l)
{
	PHash		empty = vc_root_empty(ctx);

	memset(l, 0, sizeof(*l));
	vc_diff_tables(ctx, &empty, rv, table_list_fn, l);
}

static void
table_list_free(TableList *l)
{
	for (size_t i = 0; i < l->n; i++)
		free(l->names[i]);
	free(l->names);
	free(l->t);
}

/* The schema chunk's bytes as a NUL-terminated string. */
static char *
schema_text(ProllyCtx *ctx, const PHash *h, size_t *len)
{
	const uint8_t *s = vc_schema_get(ctx, h, len);
	char	   *out;

	CHECK(s != NULL);
	out = px_malloc(*len + 1);
	memcpy(out, s, *len);
	out[*len] = '\0';
	return out;
}

static void
restore_ddl(ProllyCtx *ctx, const PHash *schema, const char *db, const char *what)
{
	char		path[1024];
	size_t		len;
	char	   *ddl = schema_text(ctx, schema, &len);

	snprintf(path, sizeof(path), "%s/%s.sql", o.work, what);
	write_file(path, ddl, len);
	psql_file(db, path);
	free(ddl);
}

static void
restore_rows(ProllyCtx *ctx, PGconn *c, const VcTable *t)
{
	size_t		slen;
	char	   *layout = schema_text(ctx, &t->schema, &slen),
			   *rel = layout + 4,
			   *p,
			   *list;
	int			nkey,
				ncols;
	char	   *sql;
	PGresult   *res;
	ProllyIter *it;
	const uint8_t *k,
			   *v;
	size_t		kl,
				vl;
	Buf			line = {0};

	p = strchr(rel, '\n');
	*p++ = '\0';
	CHECK(sscanf(p, "key %d\ncols %d\n", &nkey, &ncols) == 2);
	list = strstr(p, "list ") + 5;
	*strchr(list, '\n') = '\0';
	sql = px_malloc(strlen(rel) + strlen(list) + 64);
	sprintf(sql, "COPY %s.%s (%s) FROM STDIN", SCHEMA, rel, list);
	res = PQexec(c, sql);
	if (PQresultStatus(res) != PGRES_COPY_IN)
		die("copy in: %s", PQerrorMessage(c));
	PQclear(res);
	it = t_iter(ctx, &t->data, NULL, 0);
	if (broken("row") && strcmp(rel, "kinds") == 0)
		t_next(it, &k, &kl, &v, &vl);
	while (t_next(it, &k, &kl, &v, &vl))
	{
		long		times = 1;

		line.len = 0;
		buf_append(&line, k, kl);
		if (nkey == 0)
		{
			char		cnt[24];

			CHECK(vl < sizeof(cnt));
			memcpy(cnt, v, vl);
			cnt[vl] = '\0';
			times = atol(cnt);
		}
		else if (ncols > nkey)
		{
			buf_append(&line, "\t", 1);
			buf_append(&line, v, vl);
		}
		buf_append(&line, "\n", 1);
		for (long i = 0; i < times; i++)
			if (PQputCopyData(c, (const char *) line.data, (int) line.len) != 1)
				die("copy in: %s", PQerrorMessage(c));
	}
	prolly_iter_close(it);
	if (PQputCopyEnd(c, NULL) != 1)
		die("copy in: %s", PQerrorMessage(c));
	res = PQgetResult(c);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		die("copy in: %s", PQerrorMessage(c));
	PQclear(res);
	free(line.data);
	free(sql);
	free(layout);
}

static void
restore_sequences(ProllyCtx *ctx, PGconn *c, const PHash *data)
{
	ProllyIter *it = t_iter(ctx, data, NULL, 0);
	const uint8_t *k,
			   *v;
	size_t		kl,
				vl;

	while (t_next(it, &k, &kl, &v, &vl))
	{
		char		name[256],
					val[64],
				   *tab;
		const char *params[3];
		PGresult   *res;

		CHECK(kl < sizeof(name) && vl < sizeof(val));
		memcpy(name, k, kl);
		name[kl] = '\0';
		memcpy(val, v, vl);
		val[vl] = '\0';
		tab = strchr(val, '\t');
		*tab++ = '\0';
		params[0] = name;
		params[1] = val;
		params[2] = tab;
		res = PQexecParams(c, "SELECT setval(format('" SCHEMA ".%I', $1::text)::regclass, "
						   "$2::int8, $3::bool)", 3, NULL, params, NULL, NULL, 0);
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
			die("setval: %s", PQerrorMessage(c));
		PQclear(res);
	}
	prolly_iter_close(it);
}

static void
logical_restore(ProllyCtx *ctx, const PHash *rv, const char *db)
{
	TableList	l;
	PGconn	   *c;
	VcTable		t;

	table_list(ctx, rv, &l);
	CHECK(vc_table_get(ctx, rv, "~pre", &t));
	restore_ddl(ctx, &t.schema, db, "pre");
	c = pg_open(db);
	for (size_t i = 0; i < l.n; i++)
		if (l.names[i][0] != '~')
			restore_rows(ctx, c, &l.t[i]);
	CHECK(vc_table_get(ctx, rv, "~seq", &t));
	if (!broken("sequence"))
		restore_sequences(ctx, c, &t.data);
	PQfinish(c);
	CHECK(vc_table_get(ctx, rv, "~post", &t));
	restore_ddl(ctx, &t.schema, db, "post");
	table_list_free(&l);
}

static const WorkingSetOps logical_ops = {"logical", logical_capture, logical_restore};

/* --------------------------------------------------------------- checks */

typedef struct Check
{
	char		kind;			/* q, s or v */
	char	   *arg;			/* index name or sqlstate */
	char	   *sql;
	bool		used;			/* q: its index answered it in some rebuild */
	bool		nonempty;		/* q: some commit gave rows */
} Check;

static Check checks[MAX_CHECKS];
static int	nchecks;

static void
load_checks(void)
{
	char		path[1024];
	size_t		len;
	char	   *text,
			   *p;

	snprintf(path, sizeof(path), "%s/checks.txt", o.fixture);
	text = slurp_file(path, &len);
	for (p = strtok(text, "\n"); p; p = strtok(NULL, "\n"))
	{
		Check	   *ch;
		char	   *bar;

		if (p[0] == '#' || p[0] == '\0')
			continue;
		if (nchecks == MAX_CHECKS || p[1] != ' ' || strchr("qsv", p[0]) == NULL)
			die("bad check line: %s", p);
		ch = &checks[nchecks++];
		ch->kind = p[0];
		p += 2;
		if (ch->kind == 's')
		{
			ch->sql = strdup(p);
			continue;
		}
		bar = strchr(p, '|');
		if (bar == NULL)
			die("bad check line: %s", p);
		*bar = '\0';
		ch->arg = strdup(p);
		ch->sql = strdup(bar + 1);
	}
	free(text);
}

/* A query's rows, canonical: their count and a digest independent of order. */
static char *
answer(PGconn *c, const char *sql)
{
	char	   *w = px_malloc(strlen(sql) + 256);
	PGresult   *res;
	char	   *out;

	sprintf(w, "SELECT count(*), coalesce(md5(string_agg(x::text, E'\\n' ORDER BY x::text "
			"COLLATE \"C\")), '') FROM (%s) x", sql);
	res = PQexec(c, w);
	out = px_malloc(128);
	if (PQresultStatus(res) == PGRES_TUPLES_OK)
		snprintf(out, 128, "%s %s", PQgetvalue(res, 0, 0), PQgetvalue(res, 0, 1));
	else
		snprintf(out, 128, "error %s", PQresultErrorField(res, PG_DIAG_SQLSTATE));
	PQclear(res);
	free(w);
	return out;
}

/* A statement's outcome with every constraint checked at once, undone. */
static char *
outcome(PGconn *c, const char *sql)
{
	PGresult   *res;
	char	   *out = px_malloc(32);

	pg_cmd(c, "BEGIN");
	pg_cmd(c, "SET CONSTRAINTS ALL IMMEDIATE");
	res = PQexec(c, sql);
	if (PQresultStatus(res) == PGRES_COMMAND_OK || PQresultStatus(res) == PGRES_TUPLES_OK)
		snprintf(out, 32, "ok");
	else
		snprintf(out, 32, "%s", PQresultErrorField(res, PG_DIAG_SQLSTATE));
	PQclear(res);
	pg_cmd(c, "ROLLBACK");
	return out;
}

/* Every field of every row, in order. */
static char *
plain_answer(PGconn *c, const char *sql)
{
	PGresult   *res = PQexec(c, sql);
	Buf			b = {0};

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		const char *st = PQresultErrorField(res, PG_DIAG_SQLSTATE);

		buf_append(&b, "error ", 6);
		buf_append(&b, st, strlen(st));
	}
	else
		for (int i = 0; i < PQntuples(res); i++)
			for (int j = 0; j < PQnfields(res); j++)
			{
				buf_append(&b, PQgetvalue(res, i, j), strlen(PQgetvalue(res, i, j)));
				buf_append(&b, j + 1 < PQnfields(res) ? "|" : "\n", 1);
			}
	buf_append(&b, "", 1);
	PQclear(res);
	return (char *) b.data;
}

static bool
word_in(const char *text, const char *word)
{
	size_t		wl = strlen(word);

	for (const char *p = strstr(text, word); p; p = strstr(p + 1, word))
	{
		bool		before = p == text || !(isalnum((unsigned char) p[-1]) || p[-1] == '_');
		bool		after = !(isalnum((unsigned char) p[wl]) || p[wl] == '_');

		if (before && after)
			return true;
	}
	return false;
}

typedef struct IndexInfo
{
	char	   *name;
	char	   *am;
	bool		used;
} IndexInfo;

static IndexInfo idx[MAX_INDEXES];
static int	nidx;

/* The schema's indexes in a database (partitioned parents have no storage). */
static void
load_indexes(PGconn *c)
{
	PGresult   *res = pg_exec(c,
							  "SELECT c.relname, am.amname FROM pg_class c "
							  "JOIN pg_namespace n ON n.oid = c.relnamespace "
							  "JOIN pg_am am ON am.oid = c.relam "
							  "WHERE n.nspname = '" SCHEMA "' AND c.relkind = 'i' "
							  "ORDER BY c.relname COLLATE \"C\"");

	for (int i = 0; i < PQntuples(res); i++)
	{
		int			j;

		for (j = 0; j < nidx; j++)
			if (strcmp(idx[j].name, PQgetvalue(res, i, 0)) == 0)
				break;
		if (j == nidx)
		{
			if (nidx == MAX_INDEXES)
				die("%s", "too many indexes");
			idx[nidx].name = strdup(PQgetvalue(res, i, 0));
			idx[nidx].am = strdup(PQgetvalue(res, i, 1));
			nidx++;
		}
	}
	PQclear(res);
}

static bool
index_exists(PGconn *c, const char *name)
{
	const char *params[1] = {name};
	PGresult   *res = PQexecParams(c,
								   "SELECT 1 FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
								   "WHERE n.nspname = '" SCHEMA "' AND c.relname = $1",
								   1, NULL, params, NULL, NULL, 0);
	bool		yes = PQntuples(res) == 1;

	PQclear(res);
	return yes;
}

/* What the source says at this commit, one answer per check. */
static char **
source_answers(const char *db)
{
	PGconn	   *c = pg_open(db);
	char	  **out = px_malloc(nchecks * sizeof(char *));

	/* Extension operators live in public. */
	pg_cmd(c, "SET search_path = public");
	pg_cmd(c, "SET enable_indexscan = off; SET enable_bitmapscan = off; "
		   "SET enable_indexonlyscan = off");
	for (int i = 0; i < nchecks; i++)
	{
		Check	   *ch = &checks[i];

		if (ch->kind == 'q')
			out[i] = answer(c, ch->sql);
		else if (ch->kind == 's')
			out[i] = plain_answer(c, ch->sql);
		else
			out[i] = outcome(c, ch->sql);
	}
	PQfinish(c);
	return out;
}

/* amcheck over the schema: heaps with TOAST, B-trees, GIN. */
static void
amcheck(PGconn *c, const char *label)
{
	PGresult   *res = pg_exec(c,
							  "SELECT c.relname, c.relkind, coalesce(am.amname, '') FROM pg_class c "
							  "JOIN pg_namespace n ON n.oid = c.relnamespace "
							  "LEFT JOIN pg_am am ON am.oid = c.relam "
							  "WHERE n.nspname = '" SCHEMA "' AND c.relkind IN ('r', 'm', 'i') "
							  "ORDER BY 1");
	int			heaps = 0,
				btrees = 0,
				gins = 0;

	for (int i = 0; i < PQntuples(res); i++)
	{
		const char *name = PQgetvalue(res, i, 0);
		char		kind = PQgetvalue(res, i, 1)[0];
		const char *am = PQgetvalue(res, i, 2);
		char		sql[512];
		PGresult   *r2;

		if (kind != 'i')
		{
			snprintf(sql, sizeof(sql),
					 "SELECT count(*) FROM public.verify_heapam('" SCHEMA ".\"%s\"', check_toast => true)", name);
			heaps++;
		}
		else if (strcmp(am, "btree") == 0)
		{
			snprintf(sql, sizeof(sql),
					 "SELECT public.bt_index_parent_check('" SCHEMA ".\"%s\"', heapallindexed => true, "
					 "rootdescend => true, checkunique => true), 0", name);
			btrees++;
		}
		else if (strcmp(am, "gin") == 0)
		{
			snprintf(sql, sizeof(sql), "SELECT public.gin_index_check('" SCHEMA ".\"%s\"'), 0", name);
			gins++;
		}
		else
			continue;
		r2 = PQexec(c, sql);
		if (PQresultStatus(r2) != PGRES_TUPLES_OK)
			fail(label, PQerrorMessage(c));
		else if (strcmp(PQgetvalue(r2, 0, PQnfields(r2) - 1), "0") != 0)
		{
			char		msg[600];

			snprintf(msg, sizeof(msg), "verify_heapam reports corruption in %s", name);
			fail(label, msg);
		}
		PQclear(r2);
	}
	PQclear(res);
	printf("  amcheck: %d heaps, %d btree, %d gin\n", heaps, btrees, gins);
}

/* Every check on a rebuilt database against the source's answers. */
static void
check_rebuilt(const char *db, char **expect, const char *label, bool last)
{
	PGconn	   *c = pg_open(db);
	int			planned = 0;

	amcheck(c, label);
	load_indexes(c);
	pg_cmd(c, "SET search_path = public");
	pg_cmd(c, "SET enable_seqscan = off");
	for (int i = 0; i < nchecks; i++)
	{
		Check	   *ch = &checks[i];
		char	   *got;
		char		msg[2048];

		if (ch->kind == 'q')
		{
			if (index_exists(c, ch->arg))
			{
				char	   *ex = px_malloc(strlen(ch->sql) + 300);
				char	   *plan;
				char		hide[1024];

				/*
				 * The planner may prefer another index that answers the same
				 * query, so the table's other indexes are hidden from it
				 * until the rollback.
				 */
				pg_cmd(c, "BEGIN");
				snprintf(hide, sizeof(hide),
						 "UPDATE pg_index SET indisvalid = false WHERE indrelid = "
						 "(SELECT indrelid FROM pg_index WHERE indexrelid = '" SCHEMA ".\"%s\"'::regclass) "
						 "AND indexrelid <> '" SCHEMA ".\"%s\"'::regclass", ch->arg, ch->arg);
				if (!broken("nohide"))
					pg_cmd(c, hide);
				sprintf(ex, "EXPLAIN (COSTS OFF) SELECT count(*), md5(string_agg(x::text, ''"
						" ORDER BY x::text COLLATE \"C\")) FROM (%s) x", ch->sql);
				plan = plain_answer(c, ex);
				for (int j = 0; j < nidx; j++)
					if (word_in(plan, idx[j].name))
						idx[j].used = true;
				if (!word_in(plan, ch->arg))
				{
					snprintf(msg, sizeof(msg), "%s not used by: %s\n%s", ch->arg, ch->sql, plan);
					fail(label, msg);
				}
				else
				{
					ch->used = true;
					planned++;
				}
				got = answer(c, ch->sql);
				pg_cmd(c, "ROLLBACK");
				free(plan);
				free(ex);
			}
			else
				got = answer(c, ch->sql);
			if (strncmp(got, "0 ", 2) != 0 && strncmp(got, "error", 5) != 0)
				ch->nonempty = true;
		}
		else if (ch->kind == 's')
			got = plain_answer(c, ch->sql);
		else
		{
			got = outcome(c, ch->sql);
			if (last && strcmp(got, ch->arg) != 0)
			{
				snprintf(msg, sizeof(msg), "expected %s, got %s: %s", ch->arg, got, ch->sql);
				fail(label, msg);
			}
		}
		if (strcmp(got, expect[i]) != 0)
		{
			snprintf(msg, sizeof(msg), "%s\n  source:  %.200s\n  rebuilt: %.200s",
					 ch->sql, expect[i], got);
			fail(label, msg);
		}
		free(got);
	}
	printf("  checks: %d, %d answered through their index\n", nchecks, planned);
	PQfinish(c);
}

/* Which entries differ, and for definitions how. */
static void
explain_mismatch(ProllyCtx *ctx, const PHash *want, const PHash *got, const char *label)
{
	TableList	a,
				b;

	table_list(ctx, want, &a);
	table_list(ctx, got, &b);
	for (size_t i = 0; i < a.n; i++)
	{
		size_t		j;

		for (j = 0; j < b.n; j++)
			if (strcmp(a.names[i], b.names[j]) == 0)
				break;
		if (j == b.n)
		{
			fail(label, a.names[i]);
			printf("  missing from the rebuild: %s\n", a.names[i]);
			continue;
		}
		if (phash_eq(&a.t[i].schema, &b.t[j].schema) && phash_eq(&a.t[i].data, &b.t[j].data))
			continue;
		printf("  %s differs (%s)\n", a.names[i],
			   phash_eq(&a.t[i].schema, &b.t[j].schema) ? "rows" : "definition");
		if (a.names[i][0] == '~' && !phash_eq(&a.t[i].schema, &b.t[j].schema))
		{
			char		pa[1024],
						pb[1024],
						cmd[2200];
			size_t		la,
						lb;
			char	   *ta = schema_text(ctx, &a.t[i].schema, &la),
					   *tb = schema_text(ctx, &b.t[j].schema, &lb);

			snprintf(pa, sizeof(pa), "%s/want%s.sql", o.work, a.names[i] + 1);
			snprintf(pb, sizeof(pb), "%s/got%s.sql", o.work, a.names[i] + 1);
			write_file(pa, ta, la);
			write_file(pb, tb, lb);
			snprintf(cmd, sizeof(cmd), "diff '%s' '%s' | head -40", pa, pb);
			if (system(cmd) < 0)
				printf("  (diff failed)\n");
			free(ta);
			free(tb);
		}
	}
	for (size_t j = 0; j < b.n; j++)
	{
		size_t		i;

		for (i = 0; i < a.n; i++)
			if (strcmp(a.names[i], b.names[j]) == 0)
				break;
		if (i == a.n)
			printf("  extra in the rebuild: %s\n", b.names[j]);
	}
	table_list_free(&a);
	table_list_free(&b);
}

/* ----------------------------------------------------------------- main */

typedef struct Step
{
	char	   *file;
	PHash		commit;
	PHash		root;
	char	  **expect;
} Step;

static int
step_cmp(const void *a, const void *b)
{
	return strcmp(*(char *const *) a, *(char *const *) b);
}

typedef struct LogList
{
	PHash		c[MAX_STEPS];
	PHash		root[MAX_STEPS];
	size_t		n;
} LogList;

static void
log_fn(void *arg, const VcCommit *c)
{
	LogList    *l = arg;

	if (l->n < MAX_STEPS)
	{
		l->root[l->n] = c->root;
		l->c[l->n++] = c->hash;
	}
}

static void
stats(ProllyCtx *ctx, const PHash *rv)
{
	TableList	l;
	uint64_t	rows = 0;

	table_list(ctx, rv, &l);
	for (size_t i = 0; i < l.n; i++)
		if (l.names[i][0] != '~')
		{
			uint64_t	n;

			CHECK(prolly_count(ctx, &l.t[i].data, &n) == PROLLY_OK);
			rows += n;
		}
	printf("  %zu entries, %" PRIu64 " row keys\n", l.n, rows);
	table_list_free(&l);
}

static void
largest_fn(void *arg, const PHash *h)
{
	void	  **a = arg;
	ChunkStore *cs = a[0];
	size_t	   *max = a[1],
				len;

	if (cs->ops->get(cs, h, &len) && len > *max)
		*max = len;
}

static size_t
largest_chunk(ChunkStore *cs)
{
	size_t		max = 0;
	void	   *a[2] = {cs, &max};

	cs->ops->foreach(cs, largest_fn, a);
	return max;
}

int
main(int argc, char **argv)
{
	static struct option opts[] = {
		{"conninfo", 1, 0, 'c'}, {"fixture", 1, 0, 'f'}, {"work", 1, 0, 'w'},
		{"bindir", 1, 0, 'b'}, {"break", 1, 0, 'x'}, {"steps", 1, 0, 's'},
		{0, 0, 0, 0}
	};
	const WorkingSetOps *ws = &logical_ops;
	Step		steps[MAX_STEPS];
	int			nsteps = 0,
				ch;
	bool		whole;
	char		path[1024],
				err[256];
	char	   *names[MAX_STEPS];
	ChunkStore *cs;
	VcRepo		r;
	PHash		state;
	LogList		log = {0};
	FILE	   *ls;

	while ((ch = getopt_long(argc, argv, "", opts, NULL)) != -1)
		switch (ch)
		{
			case 'c': o.conninfo = optarg; break;
			case 'f': o.fixture = optarg; break;
			case 'w': o.work = optarg; break;
			case 'b': o.bindir = optarg; break;
			case 'x': o.brk = optarg; break;
			case 's': o.steps = atoi(optarg); break;
			default:
				fprintf(stderr, "rebuild [--conninfo C] [--fixture DIR] [--work DIR] [--bindir DIR]\n"
						"        [--steps N] [--break row|sequence|matview|nohide]\n");
				return 2;
		}
	load_checks();

	/* The steps are the fixture's numbered SQL files, in order. */
	snprintf(path, sizeof(path), "ls '%s' | grep -E '^[0-9]+-.*\\.sql$'", o.fixture);
	ls = popen(path, "r");
	while (nsteps < MAX_STEPS && fgets(path, sizeof(path), ls))
	{
		path[strcspn(path, "\n")] = '\0';
		names[nsteps++] = strdup(path);
	}
	pclose(ls);
	qsort(names, nsteps, sizeof(char *), step_cmp);
	whole = o.steps <= 0 || o.steps >= nsteps;
	if (!whole)
		nsteps = o.steps;
	if (nsteps == 0)
		die("no steps in %s", o.fixture);

	mkdir(o.work, 0700);
	/* Each run gets its own store beside the earlier ones. */
	snprintf(path, sizeof(path), "%s/store-XXXXXX", o.work);
	if (mkdtemp(path) == NULL)
		die("cannot make a store under %s", o.work);
	cs = dirstore_open(path, err, sizeof(err));
	if (cs == NULL)
		die("store: %s", err);
	if (cs->ops->count(cs) != 0)
		die("%s must start empty", path);
	vc_repo_init(&r, cs, &prolly_default_policy);

	printf("# capture (%s working set): %d steps, %d checks\n", ws->name, nsteps, nchecks);
	fresh_database(o.src_db);
	for (int i = 0; i < nsteps; i++)
	{
		Step	   *s = &steps[i];
		char		file[1024];
		double		t0 = now_sec(),
					t1;

		s->file = names[i];
		snprintf(file, sizeof(file), "%s/%s", o.fixture, s->file);
		psql_file(o.src_db, file);
		t1 = now_sec();
		s->root = ws->capture(&r.ctx, o.src_db);
		vc_set_working(&r, &s->root);
		s->commit = vc_commit(&r, s->file);
		printf("%s: applied %.2f s, captured %.2f s\n", s->file, t1 - t0, now_sec() - t1);
		stats(&r.ctx, &s->root);
		s->expect = source_answers(o.src_db);
	}
	state = vc_repo_save(&r);
	dirstore_set_state(cs, &state);
	CHECK_OK(dirstore_flush(cs));
	printf("# store %s: %zu chunks, %.1f MB live, %.1f MB on disk, largest chunk %zu bytes\n",
		   path, cs->ops->count(cs), cs->ops->bytes(cs) / 1048576.0,
		   dirstore_disk_bytes(cs) / 1048576.0, largest_chunk(cs));
	vc_repo_free(&r);
	cs_destroy(cs);

	/* Everything after this point comes from the store alone. */
	cs = dirstore_open(path, err, sizeof(err));
	if (cs == NULL)
		die("reopen: %s", err);
	CHECK(dirstore_get_state(cs, &state));
	CHECK(vc_repo_open(&r, cs, &prolly_default_policy, &state) == VC_OK);
	CHECK(vc_log(&r, &vc_head(&r)->head, MAX_STEPS, 0, log_fn, &log) == VC_OK);
	/* Every step's commit, then the repo's initial commit. */
	if ((int) log.n != nsteps + 1)
		die("%s", "the log does not hold every commit");

	printf("# rebuild every commit from the store\n");
	for (size_t li = log.n - 1; li-- > 0;)
	{
		Step	   *s = NULL;
		PHash		got;
		double		t0 = now_sec(),
					t1;
		char		label[256];

		for (int i = 0; i < nsteps; i++)
			if (phash_eq(&steps[i].commit, &log.c[li]))
				s = &steps[i];
		CHECK(s != NULL && phash_eq(&s->root, &log.root[li]));
		snprintf(label, sizeof(label), "%s", s->file);
		fresh_database(o.dst_db);
		ws->restore(&r.ctx, &log.root[li], o.dst_db);
		{
			PGconn	   *c = pg_open(o.dst_db);

			pg_cmd(c, "VACUUM ANALYZE");
			PQfinish(c);
		}
		t1 = now_sec();
		got = ws->capture(&r.ctx, o.dst_db);
		printf("%s: rebuilt %.2f s\n", s->file, t1 - t0);
		if (phash_eq(&got, &log.root[li]))
			printf("  captures back to the same root value\n");
		else
		{
			fail(label, "the rebuild captures to a different root value");
			explain_mismatch(&r.ctx, &log.root[li], &got, label);
		}
		check_rebuilt(o.dst_db, s->expect, label, whole && s == &steps[nsteps - 1]);
	}

	/*
	 * Coverage, over the whole fixture: indexes amcheck cannot verify must
	 * have answered queries.
	 */
	for (int i = 0; whole && i < nchecks; i++)
		if (checks[i].kind == 'q' && !checks[i].nonempty)
		{
			fail("coverage", "a query gave no rows at any commit");
			printf("  %s\n", checks[i].sql);
		}
	{
		int			ams = 0;

		for (int j = 0; j < nidx; j++)
		{
			if (strcmp(idx[j].am, "btree") == 0 || strcmp(idx[j].am, "gin") == 0)
				continue;
			ams++;
			if (whole && !idx[j].used)
			{
				char		msg[300];

				snprintf(msg, sizeof(msg), "%s (%s) answered no query", idx[j].name, idx[j].am);
				fail("coverage", msg);
			}
		}
		printf("# %d indexes seen, %d beyond amcheck's reach, all of those queried unless listed\n",
			   nidx, ams);
	}
	vc_repo_free(&r);
	cs_destroy(cs);
	if (failures)
		printf("# %d FAILURES\n", failures);
	else
		printf("# every rebuild matches\n");
	return failures ? 1 : 0;
}
