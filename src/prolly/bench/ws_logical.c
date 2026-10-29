/*
 * ws_logical.c
 *	  The logical working set: a schema captured into a root value, and
 *	  restored from one into an empty database.
 *
 * Entries of the root value:
 *	~obj		every object of the schema as pg_dump writes it, one entry
 *				per object: "<section>\t<type>\t<name>" -> its DDL, with the
 *				section a (pre-data) or b (post-data); "0\tprelude\t" holds
 *				the session settings the DDL assumes
 *	~seq		sequence name -> "last_value \t is_called"
 *	<relation>	one per table holding rows (leaf partitions included): its
 *				schema is its layout (ws_codec.c), its data its rows
 *
 * Restoring runs the objects in an order by type and retries those whose
 * dependencies are not there yet until nothing more succeeds, so objects
 * from two branches restore together; what still fails is reported.
 */
#include "ws.h"

/* ------------------------------------------------------------- capture */

static int
row_cmp(const void *a, const void *b)
{
	const Row  *x = a,
			   *y = b;

	return key_cmp(x->key, x->klen, y->key, y->klen);
}

/* A row from its COPY text line. */
void
row_from_line(const WsLayout *L, const char *line, size_t len, Row *r)
{
	static Buf	scratch,
				key,
				val;
	WsField		f[WS_MAX_COLS];

	ws_copy_split(line, len, f, L->nstored, &scratch);
	ws_encode(L, f, &key, &val);
	r->key = px_malloc(key.len + 1);
	if (key.len)
		memcpy(r->key, key.data, key.len);
	r->klen = key.len;
	r->val = px_malloc(val.len + 1);
	if (val.len)
		memcpy(r->val, val.data, val.len);
	r->vlen = val.len;
}

void
row_add(Row **rows, size_t *n, size_t *cap, const WsLayout *L, const char *line, size_t len)
{
	px_grow((void **) rows, cap, *n + 1, sizeof(Row));
	row_from_line(L, line, len, &(*rows)[*n]);
	(*n)++;
}

/* A table's row map from its rows (taken over and freed). */
PHash
rows_build(ProllyCtx *ctx, Row *rows, size_t n, const WsLayout *L)
{
	ProllyKV   *kvs = px_malloc((n ? n : 1) * sizeof(ProllyKV));
	char	   *counts = px_malloc((n ? n : 1) * 24);
	size_t		nkv = 0;
	PHash		out;

	qsort(rows, n, sizeof(Row), row_cmp);
	for (size_t i = 0; i < n;)
	{
		size_t		j = i + 1;

		while (j < n && row_cmp(&rows[i], &rows[j]) == 0)
			j++;
		if (L->nkey > 0 && j > i + 1)
			die("duplicate key in %s", L->rel);
		kvs[nkv].key = rows[i].key;
		kvs[nkv].klen = rows[i].klen;
		if (L->nkey == 0)
		{
			char	   *cnt = counts + nkv * 24;

			kvs[nkv].vlen = (size_t) sprintf(cnt, "%zu", j - i);
			kvs[nkv].val = (const uint8_t *) cnt;
		}
		else
		{
			kvs[nkv].val = rows[i].val;
			kvs[nkv].vlen = rows[i].vlen;
		}
		nkv++;
		i = j;
	}
	out = t_build(ctx, kvs, nkv);
	for (size_t i = 0; i < n; i++)
	{
		free(rows[i].key);
		free(rows[i].val);
	}
	free(rows);
	free(kvs);
	free(counts);
	return out;
}

PHash
capture_rows(ProllyCtx *ctx, PGconn *c, const WsLayout *L)
{
	Buf			sql = {0};
	PGresult   *res;
	Row		   *rows = NULL;
	size_t		n = 0,
				cap = 0;
	char	   *line;
	int			len;

	buf_append(&sql, "COPY (SELECT ", 13);
	ws_copy_list(L, &sql);
	buf_append(&sql, " FROM ONLY " SCHEMA ".", strlen(" FROM ONLY " SCHEMA "."));
	buf_append(&sql, L->rel, strlen(L->rel));
	buf_append(&sql, ") TO STDOUT", 12);	/* with the NUL */
	res = PQexec(c, (const char *) sql.data);
	if (PQresultStatus(res) != PGRES_COPY_OUT)
		die("copy out: %s", PQerrorMessage(c));
	PQclear(res);
	while ((len = PQgetCopyData(c, &line, 0)) > 0)
	{
		row_add(&rows, &n, &cap, L, line, (size_t) len - 1);	/* drop the newline */
		PQfreemem(line);
	}
	res = PQgetResult(c);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		die("copy out: %s", PQerrorMessage(c));
	PQclear(res);
	free(sql.data);
	return rows_build(ctx, rows, n, L);
}

PHash
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

PHash
put_table(ProllyCtx *ctx, PHash rv, const char *name, const char *schema, size_t slen,
		  const PHash *data)
{
	CHECK(vc_table_create(ctx, &rv, name, schema, slen, &rv) == 0);
	if (data)
		CHECK(vc_table_set_data(ctx, &rv, name, data, &rv) == 0);
	return rv;
}


void
obj_add(ObjList *l, const char *key, size_t kl, const char *body, size_t bl)
{
	px_grow((void **) &l->kv, &l->cap, l->n + 1, sizeof(ProllyKV));
	l->kv[l->n].key = px_memdup(key, kl);
	l->kv[l->n].klen = kl;
	l->kv[l->n].val = px_memdup(body, bl ? bl : 1);
	l->kv[l->n].vlen = bl;
	l->n++;
}

static size_t
trim_end(const char *p, size_t n)
{
	while (n > 0 && (p[n - 1] == '\n' || p[n - 1] == ' '))
		n--;
	return n;
}

/*
 * Splits a pg_dump script into its objects.  Each starts with a header
 *	--
 *	-- Name: <name>; Type: <type>; Schema: <schema>; Owner: <owner>
 *	--
 * and runs to the next.  What precedes the first is the prelude.
 */
void
split_dump(const char *text, char sec, ObjList *l)
{
	const char *hdr = "\n--\n-- Name: ";
	const char *p = strstr(text, hdr);
	const char *end = strstr(text, "\n--\n-- PostgreSQL database dump complete");

	if (end == NULL)
		end = text + strlen(text);
	if (sec == 'a')
		obj_add(l, "0\tprelude\t", 10, text, trim_end(text, (size_t) ((p ? p : end) - text)));
	while (p && p < end)
	{
		const char *name = p + strlen(hdr),
				   *semi = strstr(name, "; Type: "),
				   *type = semi + 8,
				   *semi2 = strstr(type, "; "),
				   *body = strstr(name, "\n--\n"),
				   *next;
		char		key[1024];
		int			kl;

		if (semi == NULL || semi2 == NULL || body == NULL)
			die("%s", "a pg_dump header not understood");
		body += 4;
		next = strstr(body, hdr);
		if (next == NULL || next > end)
			next = end;
		kl = snprintf(key, sizeof(key), "%c\t%.*s\t%.*s", sec, (int) (semi2 - type), type,
					  (int) (semi - name), name);
		if (kl >= (int) sizeof(key))
			die("%s", "an object name too long");
		while (body < next && *body == '\n')
			body++;
		obj_add(l, key, (size_t) kl, body, trim_end(body, (size_t) (next - body)));
		p = next < end ? next : NULL;
	}
}

int
kv_cmp(const void *a, const void *b)
{
	const ProllyKV *x = a,
			   *y = b;

	return key_cmp(x->key, x->klen, y->key, y->klen);
}

/*
 * The schema's objects in a database, keys and DDL with column references
 * by tag, sorted by key: what ~obj holds.
 */
void
collect_objects(PGconn *c, const char *db, ObjList *l)
{
	size_t		len;
	char	   *ddl;
	ColMap		cols;

	memset(l, 0, sizeof(*l));
	ddl = dump_section(db, "pre-data", &len);
	split_dump(ddl, 'a', l);
	free(ddl);
	ddl = dump_section(db, "post-data", &len);
	split_dump(ddl, 'b', l);
	free(ddl);

	/* Column references by tag, in keys (column comments) and DDL. */
	colmap_from_db(c, &cols);
	for (size_t i = 0; i < l->n; i++)
	{
		Buf			k = {0},
					v = {0};

		if (l->kv[i].key[0] == '0' || broken("notags"))
			continue;
		templ((const char *) l->kv[i].key, l->kv[i].klen, &cols, &k);
		templ((const char *) l->kv[i].val, l->kv[i].vlen, &cols, &v);
		free((void *) l->kv[i].key);
		free((void *) l->kv[i].val);
		buf_append(&k, "", 1);
		buf_append(&v, "", 1);
		l->kv[i].key = k.data;
		l->kv[i].klen = k.len - 1;
		l->kv[i].val = v.data;
		l->kv[i].vlen = v.len - 1;
	}
	colmap_free(&cols);
	qsort(l->kv, l->n, sizeof(ProllyKV), kv_cmp);
	for (size_t i = 1; i < l->n; i++)
		if (key_cmp(l->kv[i - 1].key, l->kv[i - 1].klen, l->kv[i].key, l->kv[i].klen) == 0)
			die("two objects named %s", (const char *) l->kv[i].key);
}

void
objlist_free(ObjList *l)
{
	for (size_t i = 0; i < l->n; i++)
	{
		free((void *) l->kv[i].key);
		free((void *) l->kv[i].val);
	}
	free(l->kv);
	memset(l, 0, sizeof(*l));
}

/* The definitions (~obj, ~cols, ~viol) and sequence positions (~seq). */
PHash
capture_defs(ProllyCtx *ctx, PHash rv, PGconn *c, const char *db)
{
	ObjList		l;
	ColMap		cols;
	PHash		objs,
				seq,
				colsmap,
				viol;

	collect_objects(c, db, &l);
	objs = t_build(ctx, l.kv, l.n);
	objlist_free(&l);
	colmap_from_db(c, &cols);
	colsmap = colmap_build(ctx, &cols);
	colmap_free(&cols);
	rv = put_table(ctx, rv, "~obj", "", 0, &objs);
	rv = put_table(ctx, rv, "~cols", "", 0, &colsmap);
	viol = capture_violations(ctx, c);
	rv = put_table(ctx, rv, "~viol", "", 0, &viol);
	seq = capture_sequences(ctx, c);
	return put_table(ctx, rv, "~seq", "", 0, &seq);
}

void
layout_must_parse(const char *text, WsLayout *L)
{
	if (!ws_layout_parse(text, strlen(text), L))
		die("a layout not understood (a column without a tag?):\n%s", text);
}

PHash
logical_capture(ProllyCtx *ctx, const char *db)
{
	PGconn	   *c = pg_open(db);
	PHash		rv = capture_defs(ctx, vc_root_empty(ctx), c, db);
	PGresult   *res = pg_exec(c, LAYOUT_SQL);

	for (int i = 0; i < PQntuples(res); i++)
	{
		const char *layout = PQgetvalue(res, i, 3);
		WsLayout	L;
		PHash		data;

		layout_must_parse(layout, &L);
		data = capture_rows(ctx, c, &L);
		rv = put_table(ctx, rv, PQgetvalue(res, i, 1), layout, strlen(layout), &data);
		ws_layout_free(&L);
	}
	PQclear(res);
	PQfinish(c);
	return rv;
}

/* ---------------------------------------------------------- root values */

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

void
table_list(ProllyCtx *ctx, const PHash *rv, TableList *l)
{
	PHash		empty = vc_root_empty(ctx);

	memset(l, 0, sizeof(*l));
	vc_diff_tables(ctx, &empty, rv, table_list_fn, l);
}

void
table_list_free(TableList *l)
{
	for (size_t i = 0; i < l->n; i++)
		free(l->names[i]);
	free(l->names);
	free(l->t);
}

/* The schema chunk's bytes as a NUL-terminated string. */
char *
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

bool
table_layout(ProllyCtx *ctx, const PHash *rv, const char *name, WsLayout *L, PHash *data)
{
	VcTable		t;
	size_t		len;
	char	   *text;

	if (!vc_table_get(ctx, rv, name, &t))
		return false;
	text = schema_text(ctx, &t.schema, &len);
	layout_must_parse(text, L);
	free(text);
	if (data)
		*data = t.data;
	return true;
}

/* ------------------------------------------------------------- restore */

void
report_add(WsReport *rep, const char *what, const PGresult *res)
{
	report_add_ddl(rep, what, res, "", "");
}

void
report_add_ddl(WsReport *rep, const char *what, const PGresult *res, const char *key,
			   const char *ddl)
{
	const char *st = res ? PQresultErrorField(res, PG_DIAG_SQLSTATE) : NULL;
	const char *con = res ? PQresultErrorField(res, PG_DIAG_CONSTRAINT_NAME) : NULL;
	const char *msg = res ? PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY) : NULL;
	const char *col = res ? PQresultErrorField(res, PG_DIAG_COLUMN_NAME) : NULL;
	WsProblem  *p;

	if (rep == NULL)
	{
		fprintf(stderr, "rebuild: %s failed: %s\n", what, msg ? msg : "");
		exit(1);
	}
	px_grow((void **) &rep->p, &rep->cap, rep->n + 1, sizeof(WsProblem));
	p = &rep->p[rep->n++];
	p->what = strdup(what);
	p->sqlstate = strdup(st ? st : "");
	p->constraint = strdup(con ? con : "");
	p->message = strdup(msg ? msg : "");
	p->column = strdup(col ? col : "");
	p->key = strdup(key);
	p->ddl = strdup(ddl);
}

void
report_free(WsReport *rep)
{
	for (size_t i = 0; i < rep->n; i++)
	{
		free(rep->p[i].what);
		free(rep->p[i].sqlstate);
		free(rep->p[i].constraint);
		free(rep->p[i].message);
		free(rep->p[i].column);
		free(rep->p[i].key);
		free(rep->p[i].ddl);
	}
	free(rep->p);
	memset(rep, 0, sizeof(*rep));
}


/* Where an object goes in its section; dependencies are left to retries. */
int
obj_prio(const char *key)
{
	static const struct
	{
		char		sec;
		const char *type;
		int			prio;
	}			order[] = {
		{'a', "SCHEMA", 0}, {'a', "EXTENSION", 1}, {'a', "COLLATION", 2},
		{'a', "TYPE", 3}, {'a', "SHELL TYPE", 3}, {'a', "DOMAIN", 4},
		{'a', "FUNCTION", 5}, {'a', "PROCEDURE", 5}, {'a', "AGGREGATE", 6},
		{'a', "OPERATOR", 6}, {'a', "SEQUENCE", 7}, {'a', "TABLE", 8},
		{'a', "SEQUENCE OWNED BY", 9}, {'a', "DEFAULT", 9}, {'a', "TABLE ATTACH", 9},
		{'a', "VIEW", 10}, {'a', "MATERIALIZED VIEW", 10}, {'a', "COMMENT", 20},
		{'a', "ACL", 21},
		{'b', "INDEX", 0}, {'b', "CONSTRAINT", 1}, {'b', "CHECK CONSTRAINT", 1},
		{'b', "INDEX ATTACH", 2}, {'b', "FK CONSTRAINT", 3}, {'b', "TRIGGER", 4},
		{'b', "RULE", 4}, {'b', "POLICY", 5}, {'b', "ROW SECURITY", 5},
		{'b', "STATISTICS", 5}, {'b', "MATERIALIZED VIEW DATA", 8}, {'b', "COMMENT", 20},
		{'b', "ACL", 21},
	};
	const char *type = key + 2;
	size_t		tl = strcspn(type, "\t");

	for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++)
		if (order[i].sec == key[0] && strlen(order[i].type) == tl &&
			strncmp(order[i].type, type, tl) == 0)
			return order[i].prio;
	return 15;
}

int
obj_cmp(const void *a, const void *b)
{
	const Obj  *x = a,
			   *y = b;

	if (x->key[0] != y->key[0])
		return x->key[0] - y->key[0];
	if (x->prio != y->prio)
		return x->prio - y->prio;
	return strcmp(x->key, y->key);
}

/*
 * Runs a section's objects, each in a savepoint, again and again while
 * some succeed; what never succeeds is reported with its last error.
 */
void
run_objects(PGconn *c, Obj *objs, size_t n, WsReport *rep)
{
	bool	   *done = calloc(n ? n : 1, sizeof(bool));
	PGresult  **last = calloc(n ? n : 1, sizeof(PGresult *));
	bool		progress = true;

	while (progress)
	{
		progress = false;
		for (size_t i = 0; i < n; i++)
		{
			PGresult   *res;

			if (done[i])
				continue;
			pg_cmd(c, "SAVEPOINT obj");
			res = PQexec(c, objs[i].ddl);
			if (PQresultStatus(res) == PGRES_COMMAND_OK || PQresultStatus(res) == PGRES_TUPLES_OK)
			{
				pg_cmd(c, "RELEASE SAVEPOINT obj");
				done[i] = progress = true;
				PQclear(res);
				continue;
			}
			pg_cmd(c, "ROLLBACK TO SAVEPOINT obj");
			if (last[i])
				PQclear(last[i]);
			last[i] = res;
		}
	}
	for (size_t i = 0; i < n; i++)
	{
		if (!done[i])
		{
			char		what[1100];

			snprintf(what, sizeof(what), "%s", objs[i].key + 2);
			for (char *p = what; *p; p++)
				if (*p == '\t')
					*p = ' ';
			report_add_ddl(rep, what, last[i], objs[i].key, objs[i].tddl);
		}
		if (last[i])
			PQclear(last[i]);
	}
	free(done);
	free(last);
}

/* One COPY of a table's rows; the result, or NULL if it loaded. */
static PGresult *
copy_rows(ProllyCtx *ctx, PGconn *c, const char *name, const WsLayout *L, const VcTable *t)
{
	Buf			sql = {0},
				line = {0};
	PGresult   *res;
	ProllyIter *it;
	const uint8_t *k,
			   *v;
	size_t		kl,
				vl;
	WsField		f[WS_MAX_COLS];
	bool		skip = broken("row") && strcmp(name, "kinds") == 0;

	buf_append(&sql, "COPY " SCHEMA ".", strlen("COPY " SCHEMA "."));
	buf_append(&sql, L->rel, strlen(L->rel));
	buf_append(&sql, " (", 2);
	ws_copy_list(L, &sql);
	buf_append(&sql, ") FROM STDIN", 13);
	res = PQexec(c, (const char *) sql.data);
	if (PQresultStatus(res) != PGRES_COPY_IN)
		die("copy in: %s", PQerrorMessage(c));
	PQclear(res);
	it = t_iter(ctx, &t->data, NULL, 0);
	while (t_next(it, &k, &kl, &v, &vl))
	{
		long		times = 1;

		if (skip)
		{
			skip = false;
			continue;
		}
		if (L->nkey == 0)
		{
			char		cnt[24];

			CHECK(vl < sizeof(cnt));
			memcpy(cnt, v, vl);
			cnt[vl] = '\0';
			times = atol(cnt);
		}
		ws_decode(L, k, kl, v, vl, f);
		ws_copy_line(L, f, &line);
		for (long i = 0; i < times; i++)
			if (PQputCopyData(c, (const char *) line.data, (int) line.len) != 1)
				die("copy in: %s", PQerrorMessage(c));
	}
	prolly_iter_close(it);
	if (PQputCopyEnd(c, NULL) != 1)
		die("copy in: %s", PQerrorMessage(c));
	res = PQgetResult(c);
	free(line.data);
	free(sql.data);
	if (PQresultStatus(res) == PGRES_COMMAND_OK)
	{
		PQclear(res);
		return NULL;
	}
	return res;
}

/*
 * Loads a table's rows.  When collecting problems, a row that breaks a
 * CHECK or NOT NULL in the table's definition does not keep the others
 * out: the constraint is reported and set aside, and the rows load.
 */
static void
restore_rows(ProllyCtx *ctx, PGconn *c, const char *name, const VcTable *t, WsReport *rep)
{
	size_t		slen;
	char	   *text = schema_text(ctx, &t->schema, &slen);
	WsLayout	L;

	layout_must_parse(text, &L);
	for (int tries = 0; tries < 64; tries++)
	{
		PGresult   *res;
		const char *st,
				   *con,
				   *col;
		char		what[600],
					sql[1400];

		pg_cmd(c, "SAVEPOINT copy");
		res = copy_rows(ctx, c, name, &L, t);
		if (res == NULL)
		{
			pg_cmd(c, "RELEASE SAVEPOINT copy");
			break;
		}
		pg_cmd(c, "ROLLBACK TO SAVEPOINT copy");
		st = PQresultErrorField(res, PG_DIAG_SQLSTATE);
		con = PQresultErrorField(res, PG_DIAG_CONSTRAINT_NAME);
		col = PQresultErrorField(res, PG_DIAG_COLUMN_NAME);
		snprintf(what, sizeof(what), "rows %s", name);
		sql[0] = '\0';
		if (st && strcmp(st, "23514") == 0 && con)
		{
			/* The check's definition, to apply again once resolved. */
			const char *params[2];
			char		rel[600],
						def[4096];
			PGresult   *r2;
			ColMap		cols;
			Buf			t = {0};

			snprintf(rel, sizeof(rel), SCHEMA ".%s", L.rel);
			params[0] = rel;
			params[1] = con;
			r2 = PQexecParams(c, "SELECT pg_get_constraintdef(oid) FROM pg_constraint "
							  "WHERE conrelid = $1::regclass AND conname = $2",
							  2, NULL, params, NULL, NULL, 0);
			snprintf(def, sizeof(def), "ALTER TABLE ONLY %s ADD CONSTRAINT %s %s;", rel,
					 con, PQresultStatus(r2) == PGRES_TUPLES_OK && PQntuples(r2) == 1
					 ? PQgetvalue(r2, 0, 0) : "");
			PQclear(r2);
			colmap_from_db(c, &cols);
			templ(def, strlen(def), &cols, &t);
			buf_append(&t, "", 1);
			colmap_free(&cols);
			report_add_ddl(rep, what, res, "", (const char *) t.data);
			free(t.data);
			snprintf(sql, sizeof(sql), "ALTER TABLE " SCHEMA ".%s DROP CONSTRAINT \"%s\"",
					 L.rel, con);
		}
		else if (st && strcmp(st, "23502") == 0 && col)
		{
			char		def[1400];
			ColMap		cols;
			Buf			t = {0};
			PGresult   *q = PQexecParams(c, "SELECT quote_ident($1)", 1, NULL, &col, NULL, NULL, 0);

			snprintf(def, sizeof(def), "ALTER TABLE ONLY " SCHEMA ".%s ALTER COLUMN %s SET NOT NULL;",
					 L.rel, PQgetvalue(q, 0, 0));
			PQclear(q);
			colmap_from_db(c, &cols);
			templ(def, strlen(def), &cols, &t);
			buf_append(&t, "", 1);
			colmap_free(&cols);
			report_add_ddl(rep, what, res, "", (const char *) t.data);
			free(t.data);
			if (rep && rep->n > 0)
			{
				/* The error does not name the constraint; the catalog does. */
				char		rel[600];
				const char *params[2] = {rel, col};
				PGresult   *r2;

				snprintf(rel, sizeof(rel), SCHEMA ".%s", L.rel);
				r2 = PQexecParams(c, "SELECT conname FROM pg_constraint c JOIN pg_attribute a "
								  "ON a.attrelid = c.conrelid AND a.attnum = ANY (c.conkey) "
								  "WHERE c.conrelid = $1::regclass AND c.contype = 'n' AND a.attname = $2",
								  2, NULL, params, NULL, NULL, 0);
				if (PQresultStatus(r2) == PGRES_TUPLES_OK && PQntuples(r2) == 1)
				{
					free(rep->p[rep->n - 1].constraint);
					rep->p[rep->n - 1].constraint = strdup(PQgetvalue(r2, 0, 0));
				}
				PQclear(r2);
			}
			snprintf(sql, sizeof(sql), "ALTER TABLE " SCHEMA ".%s ALTER COLUMN \"%s\" DROP NOT NULL",
					 L.rel, col);
		}
		else
			report_add(rep, what, res);
		PQclear(res);
		if (sql[0] == '\0')
			break;
		pg_cmd(c, sql);
	}
	free(text);
	ws_layout_free(&L);
}

/*
 * Gives the restored columns the tags the commit holds: every column in
 * ~cols, partitioned tables' too.
 */
void
restore_tags(PGconn *c, const ColMap *cols)
{
	for (size_t i = 0; i < cols->n; i++)
	{
		char		rel[600],
					tag[32];
		const char *params[3] = {rel, cols->c[i].name, tag};
		PGresult   *res;

		snprintf(rel, sizeof(rel), SCHEMA ".%s", cols->c[i].table);
		snprintf(tag, sizeof(tag), "%" PRIu64, cols->c[i].tag);
		res = PQexecParams(c,
						   "INSERT INTO prolly.tags SELECT a.attrelid, a.attnum, $3::int8 "
						   "FROM pg_attribute a WHERE a.attrelid = to_regclass($1) "
						   "AND quote_ident(a.attname) = $2 AND NOT a.attisdropped "
						   "ON CONFLICT (relid, attnum) DO UPDATE SET tag = excluded.tag",
						   3, NULL, params, NULL, NULL, 0);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
			die("tags: %s", PQerrorMessage(c));
		PQclear(res);
	}
}

void
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

/*
 * Builds the root value's schema into db, which holds the extensions and
 * prolly_tags.sql and nothing else.  Objects that never restore and rows
 * that do not load go to rep; with rep NULL they are fatal.
 */
void
ws_restore(ProllyCtx *ctx, const PHash *rv, const char *db, WsReport *rep)
{
	TableList	l;
	PGconn	   *c = pg_open(db);
	VcTable		t;
	ProllyIter *it;
	const uint8_t *k,
			   *v;
	size_t		kl,
				vl,
				nobj = 0,
				cap = 0,
				npre;
	Obj		   *objs = NULL;
	ColMap		cols;

	CHECK(vc_table_get(ctx, rv, "~obj", &t));
	colmap_from_tree(ctx, rv, &cols);
	it = t_iter(ctx, &t.data, NULL, 0);
	pg_cmd(c, "BEGIN");
	/*
	 * No user code runs while a commit is restored: the event triggers
	 * that would see this DDL are off until it commits.  Row triggers and
	 * rules come after the rows, with the objects that need them.
	 */
	pg_cmd(c, "SELECT prolly.quiet()");
	while (t_next(it, &k, &kl, &v, &vl))
	{
		Buf			r = {0};
		uint64_t	missing = 0;
		char	   *ddl;

		/* Column references as the names this commit gives them. */
		if (!render((const char *) v, vl, &cols, &r, &missing))
		{
			char		what[1100];

			snprintf(what, sizeof(what), "%.*s (column tag %" PRIu64 " unknown)",
					 (int) kl, (const char *) k, missing);
			report_add(rep, what, NULL);
			free(r.data);
			continue;
		}
		buf_append(&r, "", 1);
		ddl = (char *) r.data;
		if (kl > 0 && k[0] == '0')
		{
			PGresult   *res = PQexec(c, ddl);

			if (PQresultStatus(res) != PGRES_COMMAND_OK && PQresultStatus(res) != PGRES_TUPLES_OK)
				die("prelude: %s", PQerrorMessage(c));
			PQclear(res);
			free(ddl);
			continue;
		}
		px_grow((void **) &objs, &cap, nobj + 1, sizeof(Obj));
		objs[nobj].key = px_malloc(kl + 1);
		memcpy(objs[nobj].key, k, kl);
		objs[nobj].key[kl] = '\0';
		objs[nobj].ddl = ddl;
		objs[nobj].tddl = strndup((const char *) v, vl);
		objs[nobj].prio = obj_prio(objs[nobj].key);
		nobj++;
	}
	prolly_iter_close(it);
	qsort(objs, nobj, sizeof(Obj), obj_cmp);
	for (npre = 0; npre < nobj && objs[npre].key[0] == 'a'; npre++)
		;
	run_objects(c, objs, npre, rep);

	/* The session settings for rows, after the prelude's. */
	pg_cmd(c, SESSION_SETTINGS);
	table_list(ctx, rv, &l);
	restore_tags(c, &cols);
	for (size_t i = 0; i < l.n; i++)
		if (l.names[i][0] != '~')
			restore_rows(ctx, c, l.names[i], &l.t[i], rep);
	CHECK(vc_table_get(ctx, rv, "~seq", &t));
	if (!broken("sequence"))
		restore_sequences(ctx, c, &t.data);
	run_objects(c, objs + npre, nobj - npre, rep);
	if (vc_table_get(ctx, rv, "~viol", &t))
		restore_violations(ctx, c, &t.data);
	pg_cmd(c, "SELECT prolly.unquiet()");
	pg_cmd(c, "COMMIT");
	PQfinish(c);
	for (size_t i = 0; i < nobj; i++)
	{
		free(objs[i].key);
		free(objs[i].ddl);
		free(objs[i].tddl);
	}
	free(objs);
	table_list_free(&l);
	colmap_free(&cols);
}

static void
logical_restore(ProllyCtx *ctx, const PHash *rv, const char *db)
{
	ws_restore(ctx, rv, db, NULL);
}

const WorkingSetOps logical_ops = {"logical", logical_capture, logical_restore};

/* ------------------------------------------------------------- install */

/*
 * Installs prolly_tags.sql into db and, for a database whose working set
 * is captured as it changes, prolly_triggers.sql with the layout query in
 * place of @LAYOUT@ (and the faults cast and isnull applied to it).
 */
void
ws_install(const char *db, bool triggers)
{
	char		path[1024];

	snprintf(path, sizeof(path), "%s/prolly_tags.sql", o.sqldir);
	psql_file(db, path);
	if (triggers)
		ws_install_triggers(db);
}

/* prolly_triggers.sql alone, for a database that has prolly_tags.sql. */
void
ws_install_triggers(const char *db)
{
	char		path[1024];
	size_t		len;
	char	   *sql,
			   *p;
	Buf			b = {0};

	snprintf(path, sizeof(path), "%s/prolly_triggers.sql", o.sqldir);
	sql = slurp_file(path, &len);
	for (p = sql; *p;)
	{
		char	   *at = strstr(p, "@LAYOUT@");

		if (at == NULL)
		{
			buf_append(&b, p, strlen(p));
			break;
		}
		buf_append(&b, p, (size_t) (at - p));
		{
			const char *lay = LAYOUT_SQL;
			const char *from = broken("cast") ? "format(''%%s'', %I)" :
				broken("isnull") ? "num_nulls(%I) > 0" : NULL;
			const char *to = broken("cast") ? "%I::text" : "%I IS NULL";
			const char *hit = from ? strstr(lay, from) : NULL;

			if (from && hit == NULL)
				die("%s", "the fault's target is not in the layout query");
			if (hit)
			{
				buf_append(&b, lay, (size_t) (hit - lay));
				buf_append(&b, to, strlen(to));
				lay = hit + strlen(from);
			}
			buf_append(&b, lay, strlen(lay));
		}
		p = at + 8;
	}
	snprintf(path, sizeof(path), "%s/triggers.sql", o.work);
	write_file(path, b.data, b.len);
	psql_file(db, path);
	free(b.data);
	free(sql);
}
