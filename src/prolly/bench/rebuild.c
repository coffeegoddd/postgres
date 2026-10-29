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
#include "ws.h"

#include <getopt.h>
#include <sys/wait.h>

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
	NULL,
	NULL,
	NULL,
	NULL,
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
	bool		not_moved;		/* Q: skipped after in-place moves */
} Check;

static Check checks[MAX_CHECKS];
static int	nchecks;
static bool checking_move;		/* the database was moved in place, not rebuilt */

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
		if (nchecks == MAX_CHECKS || p[1] != ' ' || strchr("qQsv", p[0]) == NULL)
			die("bad check line: %s", p);
		ch = &checks[nchecks++];
		/*
		 * Q: a q check skipped after in-place moves.  For the SP-GiST inet
		 * index: a move writes rows one at a time, and in the order that
		 * leaves them in, a Postgres bug makes SP-GiST inet_ops miss rows
		 * (bench/spgist_inet_repro.sql reproduces it without us).  A
		 * rebuild's COPY order does not hit it, so the check still runs there.
		 */
		ch->not_moved = p[0] == 'Q';
		ch->kind = ch->not_moved ? 'q' : p[0];
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
	/*
	 * A fixture whose event triggers log DDL into public.vc_noise: version
	 * control ran no user code here, so nothing logged.
	 */
	{
		PGresult   *res = pg_exec(c, "SELECT to_regclass('public.vc_noise') IS NOT NULL");
		const char *noise = "";

		if (strcmp(PQgetvalue(res, 0, 0), "t") == 0)
		{
			PQclear(res);
			res = pg_exec(c, "SELECT coalesce(string_agg(what, ', '), '') FROM public.vc_noise");
			noise = PQgetvalue(res, 0, 0);
		}
		if (noise[0])
		{
			char		msg[1200];

			snprintf(msg, sizeof(msg), "user event triggers ran: %.1100s", noise);
			fail(label, msg);
		}
		PQclear(res);
	}
	/*
	 * A fixture whose generation expressions count their calls in
	 * public.vc_gen_calls: version control copied the values it holds and
	 * computed none, so the count never moved here.
	 */
	{
		PGresult   *res = pg_exec(c, "SELECT to_regclass('public.vc_gen_calls') IS NOT NULL");

		if (strcmp(PQgetvalue(res, 0, 0), "t") == 0)
		{
			PQclear(res);
			res = pg_exec(c, "SELECT CASE WHEN is_called THEN last_value ELSE 0 END "
						  "FROM public.vc_gen_calls");
			if (strcmp(PQgetvalue(res, 0, 0), "0") != 0)
			{
				char		msg[200];

				snprintf(msg, sizeof(msg), "generation expressions ran %s times",
						 PQgetvalue(res, 0, 0));
				fail(label, msg);
			}
		}
		PQclear(res);
	}
	load_indexes(c);
	pg_cmd(c, "SET search_path = public");
	pg_cmd(c, "SET enable_seqscan = off");
	for (int i = 0; i < nchecks; i++)
	{
		Check	   *ch = &checks[i];
		char	   *got;
		char		msg[2048];

		if (ch->not_moved && checking_move)
			continue;			/* a Postgres bug (see load_checks) */
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
	static const char *names[] = {"base", "step", "expect", "ours", "theirs", "resolve", "after",
	NULL};

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

/*
 * A stored generated column's value for a row a merge made: its expression
 * over the row's stored columns, each given as its output text and read
 * back as its type, formatted as the capture formats it.
 */
static bool
eval_generated(void *arg, const WsLayout *L, const WsCol *col, const WsCell *cells, int n,
			   Buf *out, bool *null)
{
	PGconn	  **conns = arg;
	Buf			sql = {0};
	const char **vals = px_malloc(sizeof(char *) * (size_t) (L->ncols + 1));
	char	  **owned = px_malloc(sizeof(char *) * (size_t) (L->ncols + 1));
	int			np = 0;
	bool		ok = false;

	buf_append(&sql, "SELECT v IS NULL, format('%s', v) FROM (SELECT (", strlen("SELECT v IS NULL, format('%s', v) FROM (SELECT ("));
	buf_append(&sql, col->def, strlen(col->def));
	buf_append(&sql, ")::", 3);
	buf_append(&sql, col->type, strlen(col->type));
	buf_append(&sql, " AS v FROM (SELECT", strlen(" AS v FROM (SELECT"));
	for (int i = 0; i < L->ncols; i++)
	{
		const WsCol *c = &L->cols[i];
		char		ref[32];
		int			rl;

		if (c->kind != 's')
			continue;
		owned[np] = NULL;
		vals[np] = NULL;
		for (int j = 0; j < n; j++)
			if (cells[j].tag == c->tag)
			{
				owned[np] = strndup((const char *) cells[j].p, cells[j].len);
				vals[np] = owned[np];
			}
		rl = snprintf(ref, sizeof(ref), "%s $%d::", np ? "," : "", np + 1);
		buf_append(&sql, ref, (size_t) rl);
		buf_append(&sql, c->type, strlen(c->type));
		buf_append(&sql, " AS ", 4);
		buf_append(&sql, c->name, strlen(c->name));
		np++;
	}
	buf_append(&sql, ") s) q", 7);	/* with the NUL */
	for (int i = 0; conns[i] && !ok; i++)
	{
		PGresult   *res = PQexecParams(conns[i], (const char *) sql.data, np, NULL, vals,
									   NULL, NULL, 0);

		if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1)
		{
			*null = PQgetvalue(res, 0, 0)[0] == 't';
			out->len = 0;
			if (!*null)
				buf_append(out, PQgetvalue(res, 0, 1), strlen(PQgetvalue(res, 0, 1)));
			ok = true;
		}
		PQclear(res);
	}
	for (int i = 0; i < np; i++)
		free(owned[i]);
	free(owned);
	free(vals);
	free(sql.data);
	return ok;
}

static int
str_cmp(const void *a, const void *b)
{
	return strcmp(*(char *const *) a, *(char *const *) b);
}

/* The "check <sql> => <answer>" lines on a database, after amcheck. */
static bool
run_checks(const char *db, const char *file, char **want, int nw)
{
	PGconn	   *c = pg_open(db);
	bool		ok = true;

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
	return ok;
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
			   *theirs = NULL,
			   *resolve = NULL;
	char	  **want = NULL,
			  **got = NULL,
			  **after = NULL;
	int			nw = 0,
				ng = 0,
				na = 0;
	bool		stored = false,
				inplace = false;
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
		else if (strcmp(sec[i].name, "resolve") == 0)
			resolve = &sec[i].text;
		else if (strcmp(sec[i].name, "after") == 0)
			na = expect_lines(&sec[i], &after);
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
	ws_merge(ctx, &B, &O, &T, &(WsEval) {eval_default, eval_generated, conns}, &m);
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
		{
			WsLines		viol = {0},
						unapplied = {0};

			ws_record_violations("rt_mm", &rep, &viol, &unapplied);
			for (size_t i = 0; i < viol.n; i++)
			{
				px_grow((void **) &got, &gcap, (size_t) ng + 1, sizeof(char *));
				got[ng++] = strdup(viol.line[i]);
			}
			for (size_t i = 0; i < unapplied.n; i++)
			{
				px_grow((void **) &got, &gcap, (size_t) ng + 1, sizeof(char *));
				got[ng++] = strdup(unapplied.line[i]);
			}

			/*
			 * Violations are stored: the working set with them captures to
			 * a root value holding them, which rebuilds to itself.
			 */
			if (viol.n > 0 && unapplied.n == 0)
			{
				PHash		W = logical_capture(ctx, "rt_mm");
				VcTable		vt;
				uint64_t	nv = 0;

				CHECK(vc_table_get(ctx, &W, "~viol", &vt));
				CHECK(prolly_count(ctx, &vt.data, &nv) == PROLLY_OK);
				if (nv == 0)
					fail(file, "the violations were not stored");
				stored = round_trip(ctx, &W, "rt_mm2", file);
				if (resolve)
				{
					WsLines		left = {0};
					PHash		R;
					VcTable		ro,
								mo;

					apply_sql("rt_mm", resolve);
					if (ws_resolve("rt_mm", &left) != 0)
						for (size_t i = 0; i < left.n; i++)
							fail(file, left.line[i]);
					R = logical_capture(ctx, "rt_mm");
					CHECK(vc_table_get(ctx, &R, "~viol", &vt));
					CHECK(prolly_count(ctx, &vt.data, &nv) == PROLLY_OK);
					if (nv != 0)
						fail(file, "violations remain after resolving");
					CHECK(vc_table_get(ctx, &R, "~obj", &ro));
					CHECK(vc_table_get(ctx, &m.root, "~obj", &mo));
					if (!phash_eq(&ro.data, &mo.data))
					{
						fail(file, "resolving did not give back the merged schema");
						explain_mismatch(ctx, &m.root, &R, file);
					}
					vc_commit_write(ctx, &R, (PHash[]) {co, ct}, 2, 4, "merge, resolved");
					lines_free(&left);
				}
			}
			lines_free(&viol);
			lines_free(&unapplied);
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

			/* The merge applied in place to ours' working set. */
			{
				WsMoveStats mst;
				WsReport	mrep = {0};
				PHash		P;

				ws_move(ctx, &O, &m.root, "rt_mb", &mst, &mrep);
				for (size_t i = 0; i < mrep.n; i++)
				{
					char		msg[1400];

					snprintf(msg, sizeof(msg), "in place: %s: %s", mrep.p[i].what, mrep.p[i].message);
					fail(file, msg);
				}
				report_free(&mrep);
				P = logical_capture(ctx, "rt_mb");
				if (!phash_eq(&P, &m.root))
				{
					fail(file, "the merge applied in place captures to a different root value");
					explain_mismatch(ctx, &m.root, &P, file);
				}
				else
					inplace = true;
			}
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
		ok &= run_checks("rt_mm", file, want, nw);
	else if (resolve && stored)
		ok &= run_checks("rt_mm", file, after, na);
	printf("%-36s %s:", file, ok ? "as expected" : "NOT as expected");
	if (ng == 0)
		printf(" clean, %" PRIu64 " rows merged by column", m.cells_merged);
	for (int g = 0; g < ng; g++)
		printf("%s %s", g ? ";" : "", got[g]);
	if (stored)
		printf("; stored%s", resolve ? ", resolved" : "");
	if (inplace)
		printf("; applied in place");
	printf("\n");
	for (int a2 = 0; a2 < na; a2++)
		free(after[a2]);
	free(after);
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
		if (o.only)
		{
			char		num[16];
			char		list[256];
			bool		hit = false;

			snprintf(num, sizeof(num), "%d", atoi(files[i]));
			snprintf(list, sizeof(list), "%s", o.only);
			for (char *t = strtok(list, ","); t; t = strtok(NULL, ","))
				hit |= atoi(t) == atoi(num);
			if (!hit)
			{
				free(files[i]);
				continue;
			}
		}
		run_merge_scenario(&ctx, files[i]);
		free(files[i]);
	}
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
}

/* ---------------------------------------------- durable commits, crashes */

static const char *crash_at;	/* "<step>:<point>" */
static const char *self_path;

/*
 * Dies at a named point of a step's commit, as a crash would: the process
 * goes without cleaning up ("<step>:<point>"), or Postgres goes down hard
 * first ("<step>:pg-<point>").
 */
static void
maybe_crash(int step, const char *point)
{
	char		want[64],
				pgwant[64];

	if (crash_at == NULL)
		return;
	snprintf(want, sizeof(want), "%d:%s", step, point);
	snprintf(pgwant, sizeof(pgwant), "%d:pg-%s", step, point);
	if (strcmp(crash_at, pgwant) == 0)
	{
		char		cmd[1400];

		snprintf(cmd, sizeof(cmd), "%s/pg_ctl -D '%s' stop -m immediate >/dev/null 2>&1",
				 o.bindir, o.pgdata);
		if (system(cmd) != 0)
			fprintf(stderr, "rebuild: could not crash the server\n");
	}
	if (strcmp(crash_at, want) == 0 || strcmp(crash_at, pgwant) == 0)
	{
		printf("# crashing at %s\n", crash_at);
		fflush(stdout);
		_exit(70);
	}
}

static void
hash_hexval(const PHash *h, char *out)
{
	out[0] = '\\';
	out[1] = 'x';
	phash_hex(h, out + 2);
}

static bool
hex_hash(const char *hex, PHash *out)
{
	if (strlen(hex) != 2 * PHASH_LEN)
		return false;
	for (int i = 0; i < PHASH_LEN; i++)
	{
		unsigned	v;

		if (sscanf(hex + 2 * i, "%2x", &v) != 1)
			return false;
		out->b[i] = (uint8_t) v;
	}
	return true;
}

static uint64_t
wal_lsn(const char *db)
{
	PGconn	   *c = pg_open(db);
	PGresult   *res = pg_exec(c, "SELECT (pg_current_wal_lsn() - '0/0')::text");
	uint64_t	v = strtoull(PQgetvalue(res, 0, 0), NULL, 10);

	PQclear(res);
	PQfinish(c);
	return v;
}

/* The commit of record: a row in prolly.roots, in the source database. */
static void
record_root(const char *db, int seq, const PHash *state, const PHash *rv, uint64_t barrier,
			const char *tables, uint64_t jend)
{
	PGconn	   *c = pg_open(db);
	char		sq[16],
				sh[64],
				rh[64],
				lsn[40],
				je[24];
	const char *params[6] = {sq, sh, rh, lsn, je, tables};
	PGresult   *res;

	snprintf(sq, sizeof(sq), "%d", seq);
	hash_hexval(state, sh);
	hash_hexval(rv, rh);
	snprintf(lsn, sizeof(lsn), "%X/%X", (unsigned) (barrier >> 32), (unsigned) barrier);
	snprintf(je, sizeof(je), "%" PRIu64, jend);
	res = PQexecParams(c, "INSERT INTO prolly.roots VALUES ($1::int, $2::bytea, $3::bytea, "
					   "$4::pg_lsn, $5::int8, $6)", 6, NULL, params, NULL, NULL, 0);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		die("record root: %s", PQerrorMessage(c));
	PQclear(res);
	PQfinish(c);
}

typedef struct RootRow
{
	int			seq;
	PHash		state,
				rv;
	uint64_t	barrier,
				jend;
	char	   *tables;
} RootRow;

/* Every recorded commit, in order; how many. */
static int
read_roots(const char *db, RootRow **out)
{
	PGconn	   *c = pg_open(db);
	PGresult   *res = pg_exec(c, "SELECT seq, encode(state, 'hex'), encode(rv, 'hex'), "
							  "(barrier - '0/0')::text, journal_end, tables FROM prolly.roots ORDER BY seq");
	int			n = PQntuples(res);
	RootRow    *r = calloc((size_t) (n ? n : 1), sizeof(RootRow));

	for (int i = 0; i < n; i++)
	{
		r[i].seq = atoi(PQgetvalue(res, i, 0));
		CHECK(hex_hash(PQgetvalue(res, i, 1), &r[i].state));
		CHECK(hex_hash(PQgetvalue(res, i, 2), &r[i].rv));
		r[i].barrier = strtoull(PQgetvalue(res, i, 3), NULL, 10);
		r[i].jend = strtoull(PQgetvalue(res, i, 4), NULL, 10);
		r[i].tables = strdup(PQgetvalue(res, i, 5));
	}
	PQclear(res);
	PQfinish(c);
	*out = r;
	return n;
}

static void
applied_path(char *out, size_t n)
{
	snprintf(out, n, "%s.applied", o.store);
}

typedef struct RootList
{
	PHash		root[MAX_STEPS];
	size_t		n;
} RootList;

static void
roots_fn(void *arg, const VcCommit *c)
{
	RootList   *l = arg;

	if (l->n < MAX_STEPS)
		l->root[l->n++] = c->root;
}

/*
 * The streamed fixture with every commit made durable in order: the
 * journal synced, the root recorded in Postgres, then the slot told.  With
 * resume, picks up after a crash from the last recorded commit.  Then
 * checks: one recorded commit per step, each the commit the store's log
 * holds, the store consistent, and (unless quick) the last commit
 * rebuilding to its root value.
 */
static void
run_durable(char **names, int nsteps, bool resume, bool quick)
{
	ChunkStore *cs;
	VcRepo		r;
	char		err[256],
				ap[1100];
	int			start = 0,
				applied = -1;
	RootRow    *rows;
	int			nrows;
	PHash		state;
	RootList	log = {0};
	ChunkStore *bcs = NULL;
	VcRepo		b;
	bool		bfresh = false;

	if (o.store == NULL)
		die("%s", "--durable needs --store");
	applied_path(ap, sizeof(ap));
	if (!resume)
	{
		cs = dirstore_open(o.store, err, sizeof(err));
		if (cs == NULL)
			die("store: %s", err);
		if (cs->ops->count(cs) != 0)
			die("%s must start empty", o.store);
		vc_repo_init(&r, cs, &prolly_default_policy);
		drop_slot();
		fresh_database(o.src_db);
		ws_install(o.src_db, true);
		stream_setup(&r.ctx, o.src_db);
		stream.seq = getpid() * 1000;	/* barrier tokens unique across runs */
		printf("# durable commits: %d steps\n", nsteps);
	}
	else
	{
		RootRow    *last;
		FILE	   *f = fopen(ap, "r");

		nrows = read_roots(o.src_db, &rows);
		if (nrows == 0)
			die("%s", "nothing recorded to resume from");
		last = &rows[nrows - 1];
		cs = dirstore_open_at(o.store, &last->state, err, sizeof(err));
		if (cs == NULL)
			die("store: %s", err);
		CHECK(vc_repo_open(&r, cs, &prolly_default_policy, &last->state) == VC_OK);
		stream_resume(&r.ctx, o.src_db, &last->rv, last->tables, last->barrier, getpid() * 1000);
		start = last->seq + 1;
		applied = last->seq;
		state = last->state;	/* if every step is already recorded */
		if (f)
		{
			if (fscanf(f, "%d", &applied) != 1)
				applied = last->seq;
			fclose(f);
		}
		printf("# resuming after step %d (applied through %d), journal cut to %" PRIu64 "\n",
			   last->seq, applied, dirstore_journal_end(cs));
	}
	if (o.backup)
	{
		PHash		bst;

		bcs = dirstore_open(o.backup, err, sizeof(err));
		if (bcs == NULL)
			die("backup: %s", err);
		if (dirstore_get_state(bcs, &bst))
			CHECK(vc_repo_open(&b, bcs, &prolly_default_policy, &bst) == VC_OK);
		else
			bfresh = true;
	}
	for (int i = start; i < nsteps; i++)
	{
		PHash		rv,
					fresh;
		char	   *tables;
		double		t0 = now_sec(),
					t1,
					t2;
		uint64_t	wal0 = wal_lsn(o.src_db),
					j0 = dirstore_journal_end(cs);

		if (i > applied)
		{
			char		file[1024];
			FILE	   *f;

			snprintf(file, sizeof(file), "%s/%s", o.fixture, names[i]);
			psql_file(o.src_db, file);
			f = fopen(ap, "w");
			fprintf(f, "%d\n", i);
			fclose(f);
			applied = i;
		}
		rv = stream_capture(&r.ctx, o.src_db);
		maybe_crash(i, "apply");
		fresh = logical_capture(&r.ctx, o.src_db);
		if (!phash_eq(&fresh, &rv))
		{
			fail(names[i], "the stream's root value differs from a fresh capture's");
			explain_mismatch(&r.ctx, &fresh, &rv, names[i]);
		}
		vc_set_working(&r, &rv);
		vc_commit(&r, names[i]);
		state = vc_repo_save(&r);
		dirstore_set_state(cs, &state);
		t1 = now_sec();
		CHECK_OK(dirstore_flush(cs));
		t2 = now_sec();
		maybe_crash(i, "synced");
		tables = stream_tables();
		record_root(o.src_db, i, &state, &rv, stream.barrier_lsn, tables,
					dirstore_journal_end(cs));
		free(tables);
		maybe_crash(i, "recorded");
		stream_ack();
		{
			/* Where the WAL is now: a point-in-time target just after this commit. */
			PGconn	   *c = pg_open(o.src_db);
			PGresult   *res = pg_exec(c, "SELECT pg_current_wal_lsn()");
			char		lp[1200];
			FILE	   *f;

			snprintf(lp, sizeof(lp), "%s.lsn", o.store);
			f = fopen(lp, "a");
			fprintf(f, "%d %s\n", i, PQgetvalue(res, 0, 0));
			fclose(f);
			PQclear(res);
			PQfinish(c);
		}
		printf("%s: committed in %.2f s (journal sync %.3f s, root recorded %.3f s)\n", names[i],
			   now_sec() - t0, t2 - t1, now_sec() - t2);
		{
			/*
			 * The source's WAL for the step and commit, against the bytes the
			 * commit added to the journal: what logging chunks through the
			 * WAL would add to it.
			 */
			uint64_t	wal = wal_lsn(o.src_db) - wal0,
						jb = dirstore_journal_end(cs) - j0;

			printf("  WAL %.1f MB for the step; journal +%.1f MB (%.2fx the WAL)\n",
				   wal / 1048576.0, jb / 1048576.0, wal ? (double) jb / (double) wal : 0);
		}
		if (bcs)
		{
			/* The backup takes only what it lacks. */
			uint64_t	before = bcs->ops->bytes(bcs);
			size_t		cbefore = bcs->ops->count(bcs);
			PHash		bst;
			double		t3 = now_sec();

			if (bfresh)
			{
				vc_repo_init(&b, bcs, &prolly_default_policy);
				CHECK(vc_repo_clone(&b, bcs, &r, 0) == VC_OK);
				bfresh = false;
			}
			else
				CHECK(vc_push(&r, &b, vc_head(&r)->name) == VC_OK);
			bst = vc_repo_save(&b);
			dirstore_set_state(bcs, &bst);
			CHECK_OK(dirstore_flush(bcs));
			printf("  backup: +%zu chunks, +%.1f KB in %.3f s (backup holds %.1f MB; store %.1f MB)\n",
				   bcs->ops->count(bcs) - cbefore, (bcs->ops->bytes(bcs) - before) / 1024.0,
				   now_sec() - t3, bcs->ops->bytes(bcs) / 1048576.0, cs->ops->bytes(cs) / 1048576.0);
		}
	}
	pgo_close(stream.pgo);
	for (size_t i = 0; i < stream.n; i++)
		st_free(&stream.t[i]);
	free(stream.t);

	/* One recorded commit per step, each the store's. */
	nrows = read_roots(o.src_db, &rows);
	if (nrows != nsteps)
		fail("roots", "not one recorded commit per step");
	CHECK(vc_log(&r, &vc_head(&r)->head, MAX_STEPS, 0, roots_fn, &log) == VC_OK);
	for (int i = 0; i < nrows && i < nsteps; i++)
	{
		if (rows[i].seq != i)
			fail("roots", "a step recorded out of order");
		if ((size_t) (nsteps - 1 - i) >= log.n || !phash_eq(&rows[i].rv, &log.root[nsteps - 1 - i]))
			fail(names[i], "the recorded commit is not the store's");
	}
	if (nrows > 0 && !phash_eq(&rows[nrows - 1].state, &state))
		fail("roots", "the last recorded state is not the store's");
	{
		PHash		bad;

		if (!dirstore_verify(cs, &bad))
			fail("store", "a chunk's bytes do not match its hash");
		check_store(cs, 0);
	}
	if (!quick && nrows > 0)
	{
		PHash		got;

		fresh_database(o.dst_db);
		ws_install(o.dst_db, false);
		ws_restore(&r.ctx, &rows[nrows - 1].rv, o.dst_db, NULL);
		got = logical_capture(&r.ctx, o.dst_db);
		if (!phash_eq(&got, &rows[nrows - 1].rv))
		{
			fail("rebuild", "the last commit rebuilds to a different root value");
			explain_mismatch(&r.ctx, &rows[nrows - 1].rv, &got, "rebuild");
		}
		else
			printf("# the last commit rebuilds to its root value\n");
	}
	printf("# %d commits recorded, the store holds them all\n", nrows);
	if (bcs)
	{
		/* Restored from the backup alone: the same last commit. */
		PHash		bst;
		VcRepo		rb;

		vc_repo_free(&b);
		cs_destroy(bcs);
		bcs = dirstore_open(o.backup, err, sizeof(err));
		if (bcs == NULL)
			die("backup reopen: %s", err);
		CHECK(dirstore_get_state(bcs, &bst));
		CHECK(vc_repo_open(&rb, bcs, &prolly_default_policy, &bst) == VC_OK);
		{
			VcCommit	hc;

			CHECK(vc_commit_load(&rb.ctx, &vc_head(&rb)->head, &hc));
			if (nrows == 0 || !phash_eq(&hc.root, &rows[nrows - 1].rv))
				fail("backup", "the backup's head is not the last recorded commit");
			else if (!quick)
			{
				PHash		got;

				fresh_database(o.dst_db);
				ws_install(o.dst_db, false);
				ws_restore(&rb.ctx, &hc.root, o.dst_db, NULL);
				got = logical_capture(&rb.ctx, o.dst_db);
				if (!phash_eq(&got, &hc.root))
					fail("backup", "the backup's head rebuilds to a different root value");
				else
					printf("# restored from the backup alone, the last commit rebuilds\n");
			}
			else
				printf("# the backup's head is the last recorded commit\n");
		}
		{
			PHash		bad;

			if (!dirstore_verify(bcs, &bad))
				fail("backup", "a chunk's bytes do not match its hash");
		}
		vc_repo_free(&rb);
		cs_destroy(bcs);
	}
	vc_repo_free(&r);
	cs_destroy(cs);
}

/* ------------------------------------------------------------ crash suite */

typedef struct CrashCase
{
	int			step;
	const char *point;
	bool		power_cut;		/* lose the journal past the last recorded root */
	const char *what;
} CrashCase;

static int
run_child(const char *args, const char *log)
{
	char		cmd[4096];
	int			rc;

	snprintf(cmd, sizeof(cmd),
			 "'%s' --conninfo '%s' --fixture '%s' --work '%s' --bindir '%s' --pgdata '%s' %s > '%s' 2>&1",
			 self_path, o.conninfo, o.fixture, o.work, o.bindir, o.pgdata, args, log);
	rc = system(cmd);
	return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

static void
cluster(const char *how)
{
	char		cmd[2048];

	if (strcmp(how, "start") == 0)
		snprintf(cmd, sizeof(cmd), "%s/pg_ctl -D '%s' -l '%s/server.log' -w status >/dev/null 2>&1 || "
				 "%s/pg_ctl -D '%s' -l '%s/server.log' -w start >/dev/null", o.bindir, o.pgdata,
				 o.pgdata, o.bindir, o.pgdata, o.pgdata);
	else
		snprintf(cmd, sizeof(cmd), "%s/pg_ctl -D '%s' -m fast -w stop >/dev/null", o.bindir, o.pgdata);
	if (!run(cmd))
		die("cluster %s failed", how);
}

/* Cuts the journal at a point between the last recorded root's end and its end. */
static void
power_cut(const char *store, uint64_t from, unsigned seed)
{
	char		cmd[1400],
				path[2900];
	FILE	   *ls;
	struct stat st;
	uint64_t	at;

	snprintf(cmd, sizeof(cmd), "ls '%s' | grep '^j-'", store);
	ls = popen(cmd, "r");
	CHECK(fgets(cmd, sizeof(cmd), ls) != NULL);
	pclose(ls);
	cmd[strcspn(cmd, "\n")] = '\0';
	snprintf(path, sizeof(path), "%s/%s", store, cmd);
	CHECK(stat(path, &st) == 0);
	at = from + (uint64_t) seed % ((uint64_t) st.st_size - from + 1);
	CHECK(truncate(path, (off_t) at) == 0);
	printf("    journal cut at %" PRIu64 " (last recorded root ends at %" PRIu64 ", file %lld)\n",
		   at, from, (long long) st.st_size);
}

/*
 * Crashes a durable run at each point of a commit, in a cluster of its own
 * (made on first use), then resumes it: the resumed run must finish with
 * every step recorded once, as the store holds it.
 */
static void
run_crash_suite(void)
{
	static const CrashCase cases[] = {
		{1, "apply", false, "streamed, nothing durable"},
		{1, "synced", false, "journal synced, root not recorded"},
		{1, "recorded", false, "root recorded, slot not told"},
		{2, "apply", false, "across DDL: streamed, nothing durable"},
		{2, "synced", false, "across DDL: journal synced, root not recorded"},
		{2, "recorded", false, "across DDL: root recorded, slot not told"},
		{2, "synced", true, "journal synced, then a power cut"},
		{3, "recorded", true, "root recorded, then a power cut"},
		{1, "pg-apply", false, "Postgres down hard mid-commit"},
		{2, "pg-recorded", false, "Postgres down hard after the root was recorded"},
	};
	char		cmd[2048];
	struct stat st;
	int			bad = 0;

	if (o.pgdata == NULL)
		die("%s", "--crash-suite needs --pgdata (a directory for its own cluster)");
	snprintf(cmd, sizeof(cmd), "%s/PG_VERSION", o.pgdata);
	if (stat(cmd, &st) != 0)
	{
		char		conf[1200];
		FILE	   *f;
		const char *port = strstr(o.conninfo, "port=");

		snprintf(cmd, sizeof(cmd), "%s/initdb -D '%s' -U postgres --auth=trust >/dev/null", o.bindir,
				 o.pgdata);
		if (!run(cmd))
			die("%s", "initdb failed");
		snprintf(conf, sizeof(conf), "%s/postgresql.conf", o.pgdata);
		f = fopen(conf, "a");
		fprintf(f, "\nport = %d\nunix_socket_directories = '/tmp'\nwal_level = logical\n"
				"max_wal_senders = 10\nmax_replication_slots = 10\nshared_buffers = 128MB\n"
				"fsync = on\n", port ? atoi(port + 5) : 5498);
		fclose(f);
	}
	cluster("start");
	printf("# crash suite (cluster %s)\n", o.pgdata);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
	{
		const CrashCase *c = &cases[i];
		char		store[1100],
					args[2400],
					log1[1200],
					log2[1200];
		int			rc;

		snprintf(store, sizeof(store), "%s/crash-XXXXXX", o.work);
		if (mkdtemp(store) == NULL)
			die("%s", "cannot make a store");
		snprintf(log1, sizeof(log1), "%s.crash.log", store);
		snprintf(log2, sizeof(log2), "%s.resume.log", store);
		printf("%-48s ", c->what);
		fflush(stdout);
		snprintf(args, sizeof(args), "--durable --quick --store '%s' --crash %d:%s", store, c->step,
				 c->point);
		rc = run_child(args, log1);
		if (rc != 70)
		{
			printf("the run did not crash as asked (exit %d; see %s)\n", rc, log1);
			bad++;
			continue;
		}
		if (strncmp(c->point, "pg-", 3) == 0)
			cluster("start");	/* Postgres recovers from its WAL */
		if (c->power_cut)
		{
			RootRow    *rows;
			int			n = read_roots(o.src_db, &rows);

			printf("\n");
			power_cut(store, n ? rows[n - 1].jend : 0, (unsigned) (i * 7919 + 13));
			printf("%-48s ", "");
		}
		snprintf(args, sizeof(args), "--durable --resume %s --store '%s'", i == 0 ? "" : "--quick",
				 store);
		rc = run_child(args, log2);
		if (rc != 0)
		{
			printf("RESUME FAILED (exit %d; see %s)\n", rc, log2);
			bad++;
			continue;
		}
		printf("recovered\n");
	}
	drop_slot();
	cluster("stop");
	if (bad)
	{
		char		msg[64];

		snprintf(msg, sizeof(msg), "%d cases", bad);
		fail("crash suite", msg);
	}
}


/* ----------------------------------------------------------- backup suite */

static bool
sh(const char *fmt, const char *a, const char *b, const char *c, const char *d)
{
	char		cmd[4096];

	snprintf(cmd, sizeof(cmd), fmt, a, b, c, d);
	return run(cmd);
}

/*
 * A physical backup and recovery to a point in time: commits 0-1, then
 * pg_basebackup and a copy of the store; commits 2-3, then the store
 * copied again (the journal last, so it holds every earlier root's
 * chunks).  The base backup recovered (in a cluster of its own) to just
 * after commit 2 must name commit 2 as its last commit; the first copy of
 * the store cannot serve it, the later one opens as of it and rebuilds
 * it, and the recovered source's tables are commit 2's.
 */
static void
run_backup_suite(void)
{
	char		archive[1100],
				base[1100],
				store[1100],
				copy1[1200],
				copy2[1200],
				args[2400],
				log[1300],
				lp[1300],
				target[64] = "",
				conf[1300],
				err[256];
	const char *saved_conninfo = o.conninfo;
	char		restored_ci[600];
	PGconn	   *c;
	PGresult   *res;
	FILE	   *f;
	int			rc;
	RootRow    *rows;
	int			n;

	if (o.pgdata == NULL)
		die("%s", "--backup-suite needs --pgdata (the crash suite's cluster)");
	snprintf(archive, sizeof(archive), "%s/backup-archive-XXXXXX", o.work);
	snprintf(base, sizeof(base), "%s/backup-base-XXXXXX", o.work);
	snprintf(store, sizeof(store), "%s/backup-store-XXXXXX", o.work);
	if (!mkdtemp(archive) || !mkdtemp(base) || !mkdtemp(store))
		die("%s", "cannot make backup directories");
	snprintf(copy1, sizeof(copy1), "%s.copy1", store);
	snprintf(copy2, sizeof(copy2), "%s.copy2", store);

	/* WAL archiving on the cluster. */
	cluster("start");
	snprintf(conf, sizeof(conf), "%s/postgresql.auto.conf", o.pgdata);
	f = fopen(conf, "w");
	fprintf(f, "# Do not edit this file manually!\n"
			"# It will be overwritten by the ALTER SYSTEM command.\n"
			"archive_mode = on\narchive_command = 'cp %%p %s/%%f'\n", archive);
	fclose(f);
	cluster("stop");
	cluster("start");
	printf("# backup suite (cluster %s, archive %s)\n", o.pgdata, archive);

	snprintf(log, sizeof(log), "%s.run1.log", store);
	snprintf(args, sizeof(args), "--durable --quick --steps 2 --store '%s'", store);
	if ((rc = run_child(args, log)) != 0)
		die("commits 0-1 failed; see %s", log);
	{
		const char *port = strstr(o.conninfo, "port=");
		char		pn[16];

		snprintf(pn, sizeof(pn), "%d", port ? atoi(port + 5) : 5498);
		if (!sh("%s/pg_basebackup -h /tmp -p %s -U postgres -D '%s' -X stream -c fast >/dev/null 2>&1",
				o.bindir, pn, base, ""))
			die("%s", "pg_basebackup failed");
	}
	if (!sh("cp -a '%s' '%s'", store, copy1, "", ""))
		die("%s", "copying the store failed");
	printf("base backup taken after commit 1; store copied\n");

	snprintf(log, sizeof(log), "%s.run2.log", store);
	snprintf(args, sizeof(args), "--durable --quick --resume --store '%s'", store);
	if ((rc = run_child(args, log)) != 0)
		die("commits 2-3 failed; see %s", log);
	c = pg_open(o.src_db);
	pg_cmd(c, "SELECT pg_switch_wal()");
	PQfinish(c);
	usleep(1500000);			/* the archiver copies the last segment */
	if (!sh("cp -a '%s' '%s'", store, copy2, "", ""))
		die("%s", "copying the store failed");
	snprintf(lp, sizeof(lp), "%s.lsn", store);
	f = fopen(lp, "r");
	{
		int			seq;
		char		lsn[64];

		while (f && fscanf(f, "%d %63s", &seq, lsn) == 2)
			if (seq == 2)
				snprintf(target, sizeof(target), "%s", lsn);
		if (f)
			fclose(f);
	}
	if (target[0] == '\0')
		die("%s", "no WAL position recorded after commit 2");
	printf("commits 2-3 made; store copied again; recovering to %s (just after commit 2)\n", target);

	/* The base backup, recovered to just after commit 2, on port 5497. */
	snprintf(conf, sizeof(conf), "%s/postgresql.auto.conf", base);
	f = fopen(conf, "a");
	fprintf(f, "port = 5497\narchive_mode = off\nrestore_command = 'cp %s/%%f %%p'\n"
			"recovery_target_lsn = '%s'\nrecovery_target_action = 'promote'\n", archive, target);
	fclose(f);
	snprintf(conf, sizeof(conf), "%s/recovery.signal", base);
	f = fopen(conf, "w");
	fclose(f);
	if (!sh("%s/pg_ctl -D '%s' -l '%s/server.log' -w start >/dev/null", o.bindir, base, base, ""))
		die("%s", "the recovered cluster did not start");
	snprintf(restored_ci, sizeof(restored_ci), "host=/tmp port=5497 user=postgres");
	o.conninfo = restored_ci;
	for (int i = 0; i < 100; i++)
	{
		bool		done;

		c = pg_open("postgres");
		res = pg_exec(c, "SELECT NOT pg_is_in_recovery()");
		done = PQgetvalue(res, 0, 0)[0] == 't';
		PQclear(res);
		PQfinish(c);
		if (done)
			break;
		usleep(100000);
	}

	n = read_roots(o.src_db, &rows);
	printf("recovered: %d commits recorded, the last %d\n", n, n ? rows[n - 1].seq : -1);
	if (n != 3 || rows[2].seq != 2)
		fail("backup suite", "the recovered database does not end at commit 2");
	else
	{
		ChunkStore *cs = dirstore_open_at(copy1, &rows[2].state, err, sizeof(err));
		VcRepo		r;
		PHash		got,
					src;

		if (cs != NULL)
		{
			fail("backup suite", "the store copied before commit 2 claims to hold it");
			dirstore_crash(cs);
		}
		else
			printf("the store copied before commit 2 cannot serve it (%s)\n", err);
		cs = dirstore_open_at(copy2, &rows[2].state, err, sizeof(err));
		if (cs == NULL)
			fail("backup suite", err);
		else
		{
			CHECK(vc_repo_open(&r, cs, &prolly_default_policy, &rows[2].state) == VC_OK);
			fresh_database(o.dst_db);
			ws_install(o.dst_db, false);
			ws_restore(&r.ctx, &rows[2].rv, o.dst_db, NULL);
			got = logical_capture(&r.ctx, o.dst_db);
			if (!phash_eq(&got, &rows[2].rv))
				fail("backup suite", "commit 2 does not rebuild from the later store copy");
			else
				printf("the later store copy opens as of commit 2, which rebuilds\n");

			/* The recovered source holds commit 2's rows (not unlogged ones). */
			src = logical_capture(&r.ctx, o.src_db);
			{
				WsDiff		d;
				int			off = 0;

				ws_diff(&r.ctx, &rows[2].rv, &src, &d);
				for (size_t q = 0; q < d.n; q++)
					if (strncmp(d.line[q], "data ", 5) == 0 && strstr(d.line[q], " changed") &&
						strstr(d.line[q], "scratch") == NULL)
					{
						printf("    %s\n", d.line[q]);
						off++;
					}
				if (off)
					fail("backup suite", "the recovered source's rows are not commit 2's");
				else
					printf("the recovered source's tables are commit 2's (the unlogged one empty)\n");
				ws_diff_free(&d);
			}
			vc_repo_free(&r);
			dirstore_crash(cs);
		}
	}
	o.conninfo = saved_conninfo;
	sh("%s/pg_ctl -D '%s' -m fast -w stop >/dev/null", o.bindir, base, "", "");

	/* The cluster without archiving again. */
	snprintf(conf, sizeof(conf), "%s/postgresql.auto.conf", o.pgdata);
	f = fopen(conf, "w");
	fprintf(f, "# Do not edit this file manually!\n# It will be overwritten by the ALTER SYSTEM command.\n");
	fclose(f);
	drop_slot();
	cluster("stop");
}

/* ------------------------------------------------------- moving in place */

/* Row keys that differ between two root values, over every table. */
static uint64_t
row_changes(ProllyCtx *ctx, const PHash *a, const PHash *b)
{
	TableList	lb;
	uint64_t	n = 0;

	table_list(ctx, b, &lb);
	for (size_t i = 0; i < lb.n; i++)
	{
		VcTable		ta;
		ProllyDiffList dl = {0};

		if (lb.names[i][0] == '~')
			continue;
		if (!vc_table_get(ctx, a, lb.names[i], &ta))
		{
			uint64_t	c = 0;

			CHECK(prolly_count(ctx, &lb.t[i].data, &c) == PROLLY_OK);
			n += c;
			continue;
		}
		CHECK(prolly_diff_collect(ctx, &ta.data, &lb.t[i].data, &dl) == PROLLY_OK);
		n += dl.n;
		prolly_diff_list_free(&dl);
	}
	table_list_free(&lb);
	return n;
}

static void
vacuum_analyze(const char *db)
{
	PGconn	   *c = pg_open(db);

	pg_cmd(c, "VACUUM ANALYZE");
	PQfinish(c);
}

/*
 * The working set moved in place between every ordered pair of the
 * fixture's commits, through one database: each move must leave it
 * capturing to the target's root value and answering the target's checks.
 */
static void
run_moves(char **names, int nsteps)
{
	ChunkStore *cs = memstore_create();
	VcRepo		r;
	PHash		root[MAX_STEPS];
	char	  **expect[MAX_STEPS];
	double		rebuild_s[MAX_STEPS];
	static const int chain[] = {1, 2, 3, 2, 1, 0, 2, 0, 3, 1, 3, 0};
	int			cur = 0;

	vc_repo_init(&r, cs, &prolly_default_policy);
	fresh_database(o.src_db);
	ws_install(o.src_db, true);
	for (int i = 0; i < nsteps; i++)
	{
		char		file[1024];

		snprintf(file, sizeof(file), "%s/%s", o.fixture, names[i]);
		psql_file(o.src_db, file);
		root[i] = logical_capture(&r.ctx, o.src_db);
		vc_set_working(&r, &root[i]);
		vc_commit(&r, names[i]);
		expect[i] = source_answers(o.src_db);
	}
	printf("# full rebuilds, for comparison\n");
	for (int i = 0; i < nsteps; i++)
	{
		double		t0 = now_sec();

		fresh_database("rt_move");
		ws_install("rt_move", false);
		ws_restore(&r.ctx, &root[i], "rt_move", NULL);
		vacuum_analyze("rt_move");
		rebuild_s[i] = now_sec() - t0;
		printf("%-14s rebuilt in %.2f s\n", names[i], rebuild_s[i]);
	}
	fresh_database("rt_move");
	ws_install("rt_move", false);
	ws_restore(&r.ctx, &root[0], "rt_move", NULL);
	vacuum_analyze("rt_move");
	printf("# moves in place (one database, starting at %s)\n", names[0]);
	printf("%-28s %7s %7s %6s %6s %6s %6s %6s %5s %5s %5s\n", "move", "secs", "rebuild",
		   "diff", "del", "ins", "upd", "kept", "built", "alter", "recr");
	for (size_t m = 0; m < sizeof(chain) / sizeof(chain[0]); m++)
	{
		int			next = chain[m];
		WsMoveStats st;
		WsReport	rep = {0};
		char		label[256];
		PHash		got;
		double		t0;

		if (next >= nsteps || cur >= nsteps)
			continue;
		snprintf(label, sizeof(label), "%.12s -> %.12s", names[cur], names[next]);
		t0 = now_sec();
		ws_move(&r.ctx, &root[cur], &root[next], "rt_move", &st, &rep);
		vacuum_analyze("rt_move");
		st.secs = now_sec() - t0;
		for (size_t i = 0; i < rep.n; i++)
		{
			char		msg[1400];

			snprintf(msg, sizeof(msg), "%s: %s", rep.p[i].what, rep.p[i].message);
			fail(label, msg);
		}
		report_free(&rep);
		printf("%-28s %7.2f %7.2f %6" PRIu64 " %6" PRIu64 " %6" PRIu64 " %6" PRIu64 " %6" PRIu64
			   " %5d %5d %5d\n", label, st.secs, rebuild_s[next],
			   row_changes(&r.ctx, &root[cur], &root[next]), st.deleted, st.inserted, st.updated,
			   st.skipped, st.built, st.altered, st.recreated);
		printf("%28s schema %.2f s, rows %.2f s, objects %.2f s, refresh %.2f s\n", "",
			   st.schema_secs, st.rows_secs, st.create_secs, st.refresh_secs);
		got = logical_capture(&r.ctx, "rt_move");
		if (!phash_eq(&got, &root[next]))
		{
			fail(label, "the moved working set captures to a different root value");
			explain_mismatch(&r.ctx, &root[next], &got, label);
		}
		checking_move = true;
		check_rebuilt("rt_move", expect[next], label, false);
		checking_move = false;
		cur = next;
	}
	vc_repo_free(&r);
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
		{"only", 1, 0, 'O'}, {"move", 0, 0, 'V'},
		{"durable", 0, 0, 'U'}, {"store", 1, 0, 'T'}, {"crash", 1, 0, 'C'},
		{"resume", 0, 0, 'R'}, {"quick", 0, 0, 'Q'}, {"crash-suite", 0, 0, 'Z'},
		{"pgdata", 1, 0, 'P'}, {"backup", 1, 0, 'B'}, {"backup-suite", 0, 0, 'Y'},
		{0, 0, 0, 0}
	};
	const WorkingSetOps *ws = &logical_ops,
			   *src_ws;
	Step		steps[MAX_STEPS];
	int			nsteps = 0,
				ch;
	bool		whole;
	char		mode = 0;
	bool		resume = false,
				quick = false;

	self_path = argv[0];
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
			case 'O': o.only = optarg; break;
			case 'V': mode = 'V'; break;
			case 'U': mode = 'U'; break;
			case 'Z': mode = 'Z'; break;
			case 'T': o.store = optarg; break;
			case 'C': crash_at = optarg; break;
			case 'R': resume = true; break;
			case 'Q': quick = true; break;
			case 'P': o.pgdata = optarg; break;
			case 'B': o.backup = optarg; break;
			case 'Y': mode = 'Y'; break;
			default:
				fprintf(stderr, "rebuild [--conninfo C] [--fixture DIR] [--work DIR] [--bindir DIR]\n"
						"        [--steps N] [--stream] [--ddl] [--merge [--only N,N...]] [--move]\n"
						"        [--durable --store DIR [--resume] [--quick] [--crash STEP:POINT]]\n"
						"        [--backup DIR] [--crash-suite --pgdata DIR] [--backup-suite --pgdata DIR]\n"
						"        [--break row|sequence|matview|nohide|cast|isnull|toast|noreemit|noflush|\n"
						"                 notags|noviol|keepall]\n");
				return 2;
		}
	mkdir(o.work, 0700);
	if (mode == 'D' || mode == 'M')
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
	if (mode == 'U' || mode == 'Z' || mode == 'Y')
	{
		if (mode == 'U')
			run_durable(names, nsteps, resume, quick);
		else if (mode == 'Z')
			run_crash_suite();
		else
			run_backup_suite();
		if (failures)
			printf("# %d FAILURES\n", failures);
		else
			printf("# %s\n", mode == 'U' ? "durable run as expected" :
				   mode == 'Z' ? "every crash recovered" : "backups restore as expected");
		return failures ? 1 : 0;
	}
	if (mode == 'V')
	{
		run_moves(names, nsteps);
		if (failures)
			printf("# %d FAILURES\n", failures);
		else
			printf("# every move as expected\n");
		return failures ? 1 : 0;
	}

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
			if (stream.unflushed)
				fail(s->file, "the barrier was not flushed when written");
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
		if (o.stream)
			stream_ack();		/* this run keeps its commits in memory until the end */
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
