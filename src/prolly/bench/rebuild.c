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
#include <getopt.h>

#include "ws.h"

#define MAX_STEPS 64
#define MAX_CHECKS 512
#define MAX_INDEXES 1024

Opts		o = {
	"host=/tmp port=5499 user=postgres",
	"bench/rebuild_fixture",
	"/home/dustin/pgv2-bench/rebuild",
	PGBIN,
	"rt_src",
	"rt_dst",
	NULL,
	0,
	false,
	"bench",
};

static int	failures = 0;

static void
fail(const char *what, const char *detail)
{
	failures++;
	printf("FAIL %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
}

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

/* A row (or an object, for ~ entries) as text; "(none)" if absent. */
static void
show_row(ProllyCtx *ctx, const PHash *schema, const char *name, const uint8_t *k, size_t kl,
		 const uint8_t *v, size_t vl, Buf *out)
{
	out->len = 0;
	if (v == NULL)
		buf_append(out, "(none)", 6);
	else if (name[0] == '~')
	{
		buf_append(out, k, kl);
		buf_append(out, " => ", 4);
		buf_append(out, v, vl);
	}
	else
	{
		size_t		len;
		char	   *text = schema_text(ctx, schema, &len);
		WsLayout	L;
		WsField		f[WS_MAX_COLS];

		layout_must_parse(text, &L);
		ws_decode(&L, k, kl, v, vl, f);
		ws_copy_line(&L, f, out);
		if (L.nkey == 0)
		{
			out->len--;
			buf_append(out, " x", 2);
			buf_append(out, v, vl);
		}
		ws_layout_free(&L);
		free(text);
	}
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
		if (!phash_eq(&a.t[i].schema, &b.t[j].schema))
		{
			size_t		la,
						lb;
			char	   *ta = schema_text(ctx, &a.t[i].schema, &la),
					   *tb = schema_text(ctx, &b.t[j].schema, &lb);

			printf("    want layout:\n%s    got layout:\n%s", ta, tb);
			free(ta);
			free(tb);
		}
		if (!phash_eq(&a.t[i].data, &b.t[j].data))
		{
			ProllyDiffList dl = {0};
			Buf			wa = {0},
						gb = {0};

			CHECK(prolly_diff_collect(ctx, &a.t[i].data, &b.t[j].data, &dl) == PROLLY_OK);
			printf("    %zu keys differ; the first:\n", dl.n);
			for (size_t d = 0; d < dl.n && d < 3; d++)
			{
				show_row(ctx, &a.t[i].schema, a.names[i], dl.v[d].key, dl.v[d].klen,
						 dl.v[d].from, dl.v[d].flen, &wa);
				show_row(ctx, &b.t[j].schema, b.names[j], dl.v[d].key, dl.v[d].klen,
						 dl.v[d].to, dl.v[d].tlen, &gb);
				printf("    want %.*s\n    got  %.*s\n", (int) (wa.len < 600 ? wa.len : 600),
					   (const char *) wa.data, (int) (gb.len < 600 ? gb.len : 600),
					   (const char *) gb.data);
			}
			free(wa.data);
			free(gb.data);
			prolly_diff_list_free(&dl);
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

/* ------------------------------------------------- DDL and merge runners */

/* A fixture file's sections: lines "-- <name> [title]" start one. */
typedef struct Section
{
	char	   *name;
	char	   *title;
	Buf			text;
} Section;

static int
read_sections(const char *path, Section **out)
{
	size_t		len;
	char	   *text = slurp_file(path, &len),
			   *p = text;
	Section    *s = NULL;
	size_t		cap = 0;
	int			n = 0;
	static const char *names[] = {"base", "step", "expect", "ours", "theirs", NULL};

	while (*p)
	{
		char	   *nl = strchr(p, '\n');
		size_t		ll = nl ? (size_t) (nl - p) : strlen(p);
		bool		hdr = false;

		for (int i = 0; names[i] && !hdr; i++)
		{
			size_t		nlen = strlen(names[i]);

			if (strncmp(p, "-- ", 3) == 0 && strncmp(p + 3, names[i], nlen) == 0 &&
				(p[3 + nlen] == '\n' || p[3 + nlen] == ' ' || p[3 + nlen] == '\0'))
			{
				px_grow((void **) &s, &cap, (size_t) n + 1, sizeof(Section));
				memset(&s[n], 0, sizeof(Section));
				s[n].name = strdup(names[i]);
				s[n].title = strndup(p + 3 + nlen + (p[3 + nlen] == ' '),
									 ll > 3 + nlen ? ll - 3 - nlen - (p[3 + nlen] == ' ') : 0);
				n++;
				hdr = true;
			}
		}
		if (!hdr && n > 0)
		{
			buf_append(&s[n - 1].text, p, ll);
			buf_append(&s[n - 1].text, "\n", 1);
		}
		p += ll + (nl ? 1 : 0);
	}
	for (int i = 0; i < n; i++)
		buf_append(&s[i].text, "", 1);
	free(text);
	*out = s;
	return n;
}

static void
apply_sql(const char *db, const Buf *sql)
{
	char		path[1024];

	snprintf(path, sizeof(path), "%s/apply.sql", o.work);
	write_file(path, sql->data, sql->len ? sql->len - 1 : 0);
	psql_file(db, path);
}

/* The expect section's lines, without blanks and comments. */
static int
expect_lines(const Section *s, char ***out)
{
	char	   *copy = strdup((const char *) s->text.data);
	char	  **l = NULL;
	size_t		cap = 0;
	int			n = 0;

	for (char *p = strtok(copy, "\n"); p; p = strtok(NULL, "\n"))
	{
		if (p[0] == '\0' || p[0] == '#')
			continue;
		px_grow((void **) &l, &cap, (size_t) n + 1, sizeof(char *));
		l[n++] = strdup(p);
	}
	free(copy);
	*out = l;
	return n;
}

/* Rebuilds a root value into dst and captures it again: it must not change. */
static bool
round_trip(ProllyCtx *ctx, const PHash *rv, const char *dst, const char *label)
{
	PHash		got;

	fresh_database(dst);
	ws_install(dst, false);
	ws_restore(ctx, rv, dst, NULL);
	got = logical_capture(ctx, dst);
	if (phash_eq(&got, rv))
		return true;
	fail(label, "the rebuild captures to a different root value");
	explain_mismatch(ctx, rv, &got, label);
	return false;
}

/*
 * The DDL fixture: a base, then steps each with the diff lines it must
 * produce against the step before.  Every step's commit must also rebuild
 * to itself.
 */
static void
run_ddl(void)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	Section    *sec;
	char		path[1024];
	int			n;
	PHash		prev;
	const char *db = "rt_ddl";

	snprintf(path, sizeof(path), "%s/ddl.sql", o.fixture);
	n = read_sections(path, &sec);
	vc_repo_init(&r, cs, &prolly_default_policy);
	fresh_database(db);
	ws_install(db, true);
	CHECK(n > 0 && strcmp(sec[0].name, "base") == 0);
	apply_sql(db, &sec[0].text);
	prev = logical_capture(&r.ctx, db);
	vc_set_working(&r, &prev);
	vc_commit(&r, "base");
	printf("# DDL steps (%s/ddl.sql)\n", o.fixture);
	for (int i = 1; i < n; i++)
	{
		WsDiff		d;
		PHash		cur;
		char	  **want = NULL;
		int			nw = 0;
		bool		ok = true;

		if (strcmp(sec[i].name, "step") != 0)
			continue;
		if (i + 1 < n && strcmp(sec[i + 1].name, "expect") == 0)
			nw = expect_lines(&sec[i + 1], &want);
		apply_sql(db, &sec[i].text);
		cur = logical_capture(&r.ctx, db);
		vc_set_working(&r, &cur);
		vc_commit(&r, sec[i].title);
		ws_diff(&r.ctx, &prev, &cur, &d);
		for (int w = 0; w < nw; w++)
			if (!ws_diff_has(&d, want[w]))
			{
				fail(sec[i].title, want[w]);
				ok = false;
			}
		printf("%-40s %s\n", sec[i].title, ok ? "as expected" : "NOT as expected; the diff:");
		if (!ok)
			for (size_t q = 0; q < d.n; q++)
				printf("    %s\n", d.line[q]);
		round_trip(&r.ctx, &cur, "rt_ddl_dst", sec[i].title);
		ws_diff_free(&d);
		for (int w = 0; w < nw; w++)
			free(want[w]);
		free(want);
		prev = cur;
	}
	vc_repo_free(&r);
	cs_destroy(cs);
}

static bool
eval_default(void *arg, const char *expr, const char *type, Buf *out)
{
	PGconn	  **conns = arg;

	for (int i = 0; conns[i]; i++)
	{
		char	   *sql = px_malloc(strlen(expr) + strlen(type) + 64);
		PGresult   *res;

		sprintf(sql, "SELECT format('%%s', (%s)::%s)", expr, type);
		res = PQexec(conns[i], sql);
		free(sql);
		if (PQresultStatus(res) == PGRES_TUPLES_OK && !PQgetisnull(res, 0, 0))
		{
			out->len = 0;
			buf_append(out, PQgetvalue(res, 0, 0), strlen(PQgetvalue(res, 0, 0)));
			PQclear(res);
			return true;
		}
		PQclear(res);
	}
	return false;
}

static int
str_cmp(const void *a, const void *b)
{
	return strcmp(*(char *const *) a, *(char *const *) b);
}

/*
 * One merge scenario: base.sql, then ours and theirs applied on two
 * branches (theirs in a database rebuilt from the base commit), merged,
 * and the result restored.  The outcomes (conflicts, constraint violations,
 * objects that do not apply, normalization) must be exactly those expected,
 * and the merged database must pass amcheck and answer its checks.
 */
static void
run_merge_scenario(ProllyCtx *ctx, const char *file)
{
	Section    *sec;
	char		path[1024];
	int			n;
	const Buf  *ours = NULL,
			   *theirs = NULL;
	char	  **want = NULL,
			  **got = NULL;
	int			nw = 0,
				ng = 0;
	size_t		gcap = 0;
	PHash		B,
				O,
				T,
				cb,
				co,
				ct,
				mb;
	WsMerge		m;
	PGconn	   *conns[3];
	bool		ok = true;

	snprintf(path, sizeof(path), "%s/merge/%s", o.fixture, file);
	n = read_sections(path, &sec);
	for (int i = 0; i < n; i++)
	{
		if (strcmp(sec[i].name, "ours") == 0)
			ours = &sec[i].text;
		else if (strcmp(sec[i].name, "theirs") == 0)
			theirs = &sec[i].text;
		else if (strcmp(sec[i].name, "expect") == 0)
			nw = expect_lines(&sec[i], &want);
	}
	if (ours == NULL || theirs == NULL)
		die("%s needs -- ours and -- theirs", file);

	fresh_database("rt_mb");
	ws_install("rt_mb", true);
	snprintf(path, sizeof(path), "%s/merge/base.sql", o.fixture);
	psql_file("rt_mb", path);
	B = logical_capture(ctx, "rt_mb");
	cb = vc_commit_write(ctx, &B, NULL, 0, 1, "base");

	/* Their branch starts from the base commit, rebuilt. */
	fresh_database("rt_mt");
	ws_install("rt_mt", false);
	ws_restore(ctx, &B, "rt_mt", NULL);
	ws_install_triggers("rt_mt");
	apply_sql("rt_mt", theirs);
	T = logical_capture(ctx, "rt_mt");
	ct = vc_commit_write(ctx, &T, &cb, 1, 2, "theirs");
	apply_sql("rt_mb", ours);
	O = logical_capture(ctx, "rt_mb");
	co = vc_commit_write(ctx, &O, &cb, 1, 3, "ours");
	CHECK(vc_merge_base(ctx, &co, &ct, &mb) == 1 && phash_eq(&mb, &cb));

	conns[0] = pg_open("rt_mt");
	conns[1] = pg_open("rt_mb");
	conns[2] = NULL;
	ws_merge(ctx, &B, &O, &T, eval_default, conns, &m);
	PQfinish(conns[0]);
	PQfinish(conns[1]);

	for (size_t i = 0; i < m.nconflicts; i++)
	{
		char		line[1300];

		snprintf(line, sizeof(line), "conflict %s", m.conflict[i]);
		px_grow((void **) &got, &gcap, (size_t) ng + 1, sizeof(char *));
		got[ng++] = strdup(line);
	}
	if (m.nconflicts == 0)
	{
		WsReport	rep = {0};

		fresh_database("rt_mm");
		ws_install("rt_mm", false);
		ws_restore(ctx, &m.root, "rt_mm", &rep);
		for (size_t i = 0; i < rep.n; i++)
		{
			char		line[1300];
			const char *w = rep.p[i].what;

			if (strncmp(rep.p[i].sqlstate, "23", 2) == 0)
				snprintf(line, sizeof(line), "violation %s",
						 rep.p[i].constraint[0] ? rep.p[i].constraint : strrchr(w, ' ') + 1);
			else
				snprintf(line, sizeof(line), "unapplied %s (%s)", w, rep.p[i].message);
			px_grow((void **) &got, &gcap, (size_t) ng + 1, sizeof(char *));
			got[ng++] = strdup(line);
		}
		if (rep.n == 0)
		{
			PHash		N = logical_capture(ctx, "rt_mm");
			bool		expect_norm = false;

			for (int w = 0; w < nw; w++)
				expect_norm |= strcmp(want[w], "normalized") == 0;
			if (!phash_eq(&N, &m.root))
			{
				px_grow((void **) &got, &gcap, (size_t) ng + 1, sizeof(char *));
				got[ng++] = strdup("normalized");
				if (!expect_norm)
					explain_mismatch(ctx, &N, &m.root, file);
			}
			vc_commit_write(ctx, &m.root, (PHash[]) {co, ct}, 2, 4, "merge");
		}
		report_free(&rep);
	}

	/* Outcomes must be exactly those expected; checks run on the result. */
	qsort(got, (size_t) ng, sizeof(char *), str_cmp);
	for (int w = 0; w < nw; w++)
	{
		bool		have = false;

		if (strncmp(want[w], "check ", 6) == 0 || strcmp(want[w], "clean") == 0)
			continue;
		for (int g = 0; g < ng; g++)
			have |= strcmp(got[g], want[w]) == 0 ||
				(strncmp(want[w], "unapplied ", 10) == 0 &&
				 strncmp(got[g], want[w], strlen(want[w])) == 0);
		if (!have)
		{
			fail(file, want[w]);
			ok = false;
		}
	}
	for (int g = 0; g < ng; g++)
	{
		bool		have = false;

		for (int w = 0; w < nw; w++)
			have |= strcmp(got[g], want[w]) == 0 ||
				(strncmp(want[w], "unapplied ", 10) == 0 &&
				 strncmp(got[g], want[w], strlen(want[w])) == 0);
		if (!have)
		{
			fail(file, got[g]);
			ok = false;
		}
	}
	if (ng == 0)
	{
		PGconn	   *c = pg_open("rt_mm");

		amcheck(c, file);
		pg_cmd(c, "SET search_path = public");
		for (int w = 0; w < nw; w++)
		{
			char	   *bar,
					   *ans,
					   *q;

			if (strncmp(want[w], "check ", 6) != 0)
				continue;
			q = strdup(want[w] + 6);
			bar = strstr(q, " => ");
			if (bar == NULL)
				die("a check needs <sql> => <answer>: %s", want[w]);
			*bar = '\0';
			ans = plain_answer(c, q);
			if (ans[0] && ans[strlen(ans) - 1] == '\n')
				ans[strlen(ans) - 1] = '\0';
			if (strcmp(ans, bar + 4) != 0)
			{
				char		msg[1200];

				snprintf(msg, sizeof(msg), "%s gave %s, not %s", q, ans, bar + 4);
				fail(file, msg);
				ok = false;
			}
			free(ans);
			free(q);
		}
		PQfinish(c);
	}
	printf("%-36s %s:", file, ok ? "as expected" : "NOT as expected");
	if (ng == 0)
		printf(" clean, %" PRIu64 " rows merged by column", m.cells_merged);
	for (int g = 0; g < ng; g++)
		printf("%s %s", g ? ";" : "", got[g]);
	printf("\n");
	ws_merge_free(&m);
	for (int g = 0; g < ng; g++)
		free(got[g]);
	free(got);
	for (int w = 0; w < nw; w++)
		free(want[w]);
	free(want);
}

static void
run_merges(void)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx;
	char		cmd[1100];
	char		line[512];
	FILE	   *ls;
	char	   *files[256];
	int			nf = 0;

	prolly_ctx_init(&ctx, cs, &prolly_default_policy);
	snprintf(cmd, sizeof(cmd), "ls '%s/merge' | grep -E '^[0-9]+-.*\\.sql$'", o.fixture);
	ls = popen(cmd, "r");
	while (nf < 256 && fgets(line, sizeof(line), ls))
	{
		line[strcspn(line, "\n")] = '\0';
		files[nf++] = strdup(line);
	}
	pclose(ls);
	qsort(files, (size_t) nf, sizeof(char *), str_cmp);
	printf("# merge scenarios (%s/merge)\n", o.fixture);
	for (int i = 0; i < nf; i++)
	{
		if (o.steps > 0 && i >= o.steps)
			break;
		run_merge_scenario(&ctx, files[i]);
		free(files[i]);
	}
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
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
		{"stream", 0, 0, 'S'}, {"ddl", 0, 0, 'D'}, {"merge", 0, 0, 'M'},
		{0, 0, 0, 0}
	};
	const WorkingSetOps *ws = &logical_ops,
			   *src_ws;
	Step		steps[MAX_STEPS];
	int			nsteps = 0,
				ch;
	bool		whole;
	char		mode = 0;
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
			case 'S': o.stream = true; break;
			case 'D': mode = 'D'; break;
			case 'M': mode = 'M'; break;
			default:
				fprintf(stderr, "rebuild [--conninfo C] [--fixture DIR] [--work DIR] [--bindir DIR]\n"
						"        [--steps N] [--stream] [--ddl] [--merge]\n"
						"        [--break row|sequence|matview|nohide|cast|isnull|toast|noreemit|noflush]\n");
				return 2;
		}
	mkdir(o.work, 0700);
	if (mode)
	{
		if (mode == 'D')
			run_ddl();
		else
			run_merges();
		if (failures)
			printf("# %d FAILURES\n", failures);
		else
			printf("# every %s as expected\n", mode == 'D' ? "DDL step" : "merge");
		return failures ? 1 : 0;
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

	src_ws = o.stream ? &stream_ops : &logical_ops;
	printf("# capture (%s working set): %d steps, %d checks\n", src_ws->name, nsteps, nchecks);
	if (o.stream)
		drop_slot();
	fresh_database(o.src_db);
	ws_install(o.src_db, true);
	if (o.stream)
		stream_setup(&r.ctx, o.src_db);
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
		s->root = src_ws->capture(&r.ctx, o.src_db);
		printf("%s: applied %.2f s, captured %.2f s\n", s->file, t1 - t0, now_sec() - t1);
		if (o.stream)
		{
			PHash		fresh = logical_capture(&r.ctx, o.src_db);

			printf("  stream %.2f s to the barrier, definitions %.2f s\n", stream.wait_s,
				   stream.defs_s);
			if (stream.wait_s > BARRIER_DEADLINE)
				fail(s->file, "the stream took too long to reach the barrier");
			printf("  streamed so far: %" PRIu64 " changes, %" PRIu64 " layout and %" PRIu64
				   " rows messages (%" PRIu64 " rows), %" PRIu64 " unchanged values filled\n",
				   stream.changes, stream.ddl_msgs, stream.rows_msgs, stream.rows_reemitted,
				   stream.toast_filled);
			if (phash_eq(&fresh, &s->root))
				printf("  the stream's root value equals a fresh capture's\n");
			else
			{
				fail(s->file, "the stream's root value differs from a fresh capture's");
				explain_mismatch(&r.ctx, &fresh, &s->root, s->file);
			}
		}
		vc_set_working(&r, &s->root);
		s->commit = vc_commit(&r, s->file);
		stats(&r.ctx, &s->root);
		s->expect = source_answers(o.src_db);
	}
	if (o.stream)
	{
		pgo_close(stream.pgo);
		for (size_t i = 0; i < stream.n; i++)
			st_free(&stream.t[i]);
		free(stream.t);
		drop_slot();
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
		ws_install(o.dst_db, false);
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
