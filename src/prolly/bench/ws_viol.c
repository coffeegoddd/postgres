/*
 * ws_viol.c
 *	  Constraint violations: found row by row after a merge is restored,
 *	  kept in the working set (prolly.violations) and in commits (~viol),
 *	  and resolved by applying the constraints once the rows are fixed.
 *
 * A constraint the merged rows break is deferred: a foreign key or check
 * is added NOT VALID, so rows written meanwhile are checked; a unique
 * constraint or index, an exclusion constraint or a NOT NULL waits.  Each
 * violating row is found by query:
 *	foreign key		child rows whose referenced row is missing
 *	check			rows for which the expression is false
 *	unique			rows sharing their key columns' values with another row
 *	not null		rows with the column NULL
 * and an exclusion constraint is recorded without its rows.
 *
 * ~viol maps "<table>\t<constraint>\t<rowkey>" (rowkey empty for the
 * constraint's own entry) to "<kind>\t<detail>".
 */
#include "ws.h"

void
lines_add(WsLines *l, const char *line)
{
	px_grow((void **) &l->line, &l->cap, l->n + 1, sizeof(char *));
	l->line[l->n++] = strdup(line);
}

void
lines_free(WsLines *l)
{
	for (size_t i = 0; i < l->n; i++)
		free(l->line[i]);
	free(l->line);
	memset(l, 0, sizeof(*l));
}

/* The first rt.<table> a definition names, quoted as written. */
static char *
table_of(const char *ddl)
{
	const char *p = strstr(ddl, SCHEMA ".");
	size_t		n;

	if (p == NULL)
		return NULL;
	p += strlen(SCHEMA) + 1;
	if (*p == '"')
	{
		const char *e = p + 1;

		while (*e && !(*e == '"' && e[1] != '"'))
			e += *e == '"' ? 2 : 1;
		n = (size_t) (e + 1 - p);
	}
	else
		n = strspn(p, "abcdefghijklmnopqrstuvwxyz0123456789_$");
	return strndup(p, n);
}

/* An expression giving a row's key text: its primary key values, or the row. */
static char *
rowkey_expr(PGconn *c, const char *table, const char *alias)
{
	char		rel[600];
	const char *params[2] = {rel, alias};
	PGresult   *res;
	char	   *out;

	snprintf(rel, sizeof(rel), SCHEMA ".%s", table);
	res = PQexecParams(c,
					   "SELECT coalesce('concat_ws('','', ' || string_agg(format('format(''%%s'', %s.%I)', "
					   "  $2::text, a.attname), ', ' ORDER BY k.ord) || ')', format('format(''%%s'', %s.*)', $2::text)) "
					   "FROM pg_index i CROSS JOIN LATERAL unnest(i.indkey::int2[]) WITH ORDINALITY k(attnum, ord) "
					   "JOIN pg_attribute a ON a.attrelid = i.indrelid AND a.attnum = k.attnum "
					   "WHERE i.indrelid = $1::regclass AND i.indisprimary AND k.ord <= i.indnkeyatts",
					   2, NULL, params, NULL, NULL, 0);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		die("rowkey: %s", PQerrorMessage(c));
	out = strdup(PQgetvalue(res, 0, 0));
	PQclear(res);
	return out;
}

/* The text inside the parentheses that open at p. */
static char *
paren_body(const char *p, const char **end)
{
	int			depth = 0;
	const char *s = p;

	for (; *p; p++)
	{
		if (*p == '\'')
		{
			for (p++; *p && *p != '\''; p++)
				;
			continue;
		}
		if (*p == '(')
			depth++;
		else if (*p == ')' && --depth == 0)
		{
			if (end)
				*end = p + 1;
			return strndup(s + 1, (size_t) (p - s - 1));
		}
	}
	return NULL;
}

/*
 * A unique constraint's or index's key list as plain expressions: sort
 * order, NULLS placement, collations and operator classes dropped.
 */
static char *
key_list(const char *list)
{
	Buf			out = {0};
	int			depth = 0;
	const char *item = list;

	for (const char *p = list;; p++)
	{
		if (*p == '(')
			depth++;
		else if (*p == ')')
			depth--;
		if (*p == '\0' || (*p == ',' && depth == 0))
		{
			char	   *it = strndup(item, (size_t) (p - item));
			char	   *cut;

			while ((cut = strstr(it, " DESC")) || (cut = strstr(it, " ASC")) ||
				   (cut = strstr(it, " NULLS ")) || (cut = strstr(it, " COLLATE ")))
				*cut = '\0';
			/* "col opclass": a trailing word after the expression. */
			if ((cut = strrchr(it, ' ')) && strstr(cut, "_ops"))
				*cut = '\0';
			if (out.len)
				buf_append(&out, ", ", 2);
			buf_append(&out, it, strlen(it));
			free(it);
			if (*p == '\0')
				break;
			item = p + 1;
		}
	}
	buf_append(&out, "", 1);
	return (char *) out.data;
}

/* Runs a query of row keys and records each as a violation. */
static int
record_rows(PGconn *c, const char *sql, const char *table, const char *cons, const char *kind,
			WsLines *out)
{
	PGresult   *res = PQexec(c, sql);
	int			n = 0;

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		PQclear(res);
		return -1;
	}
	for (int i = 0; i < PQntuples(res); i++)
	{
		const char *params[4] = {table, cons, kind, PQgetvalue(res, i, 0)};
		PGresult   *r2 = PQexecParams(c, "INSERT INTO prolly.violations VALUES ($1, $2, $3, $4, '')",
									  4, NULL, params, NULL, NULL, 0);
		char		line[1400];

		if (PQresultStatus(r2) != PGRES_COMMAND_OK)
			die("violations: %s", PQerrorMessage(c));
		PQclear(r2);
		snprintf(line, sizeof(line), "violation %s %s %s", cons, table, PQgetvalue(res, i, 0));
		lines_add(out, line);
		n++;
	}
	PQclear(res);
	return n;
}

static char *
strip_semicolon(const char *ddl)
{
	size_t		n = strlen(ddl);

	while (n > 0 && (ddl[n - 1] == ';' || isspace((unsigned char) ddl[n - 1])))
		n--;
	return strndup(ddl, n);
}

/* One violated constraint: defer it, find its rows, keep its definition. */
static void
record_one(PGconn *c, const WsProblem *p, WsLines *out)
{
	ColMap		cols;
	Buf			r = {0};
	char	   *ddl,
			   *table,
			   *rk,
			   *sql = NULL;
	const char *cons = p->constraint[0] ? p->constraint : strrchr(p->what, ' ') + 1;
	const char *kind;
	bool		is_fk = strstr(p->ddl, "FOREIGN KEY") != NULL,
				is_check = strstr(p->ddl, " CHECK ") != NULL,
				is_unique = strstr(p->ddl, " UNIQUE ") != NULL || strstr(p->ddl, "UNIQUE INDEX") != NULL,
				is_notnull = strcmp(p->sqlstate, "23502") == 0;
	int			n = -1;

	colmap_from_db(c, &cols);
	if (p->ddl[0] && !render(p->ddl, strlen(p->ddl), &cols, &r, NULL))
		die("%s", "a violated constraint's definition names an unknown column");
	buf_append(&r, "", 1);
	ddl = (char *) r.data;
	table = p->ddl[0] ? table_of(ddl) : strdup(strchr(p->what, ' ') + 1);
	if (table == NULL)
		die("no table in %s", ddl);
	rk = rowkey_expr(c, table, "x");
	kind = is_fk ? "foreign key" : is_check ? "check" : is_unique ? "unique" :
		is_notnull ? "not null" : "other";

	if (is_fk || is_check)
	{
		/* Deferred: the constraint holds for new rows but not old ones. */
		char	   *base = strip_semicolon(ddl);
		char	   *nv = px_malloc(strlen(base) + 16);
		PGresult   *res;

		sprintf(nv, "%s NOT VALID", base);
		res = PQexec(c, nv);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
			die("defer: %s", PQerrorMessage(c));
		PQclear(res);
		free(nv);
		free(base);
	}
	if (is_fk)
	{
		const char *params[1] = {cons};
		PGresult   *res = PQexecParams(c,
									   "SELECT format('SELECT %s FROM ONLY %s x WHERE %s AND NOT EXISTS "
									   "(SELECT 1 FROM ONLY %s y WHERE %s)', $2::text, conrelid::regclass, "
									   "  (SELECT string_agg(format('x.%I IS NOT NULL', a.attname), ' AND ') "
									   "     FROM unnest(conkey) k JOIN pg_attribute a ON a.attrelid = conrelid AND a.attnum = k), "
									   "  confrelid::regclass, "
									   "  (SELECT string_agg(format('y.%I = x.%I', pa.attname, ca.attname), ' AND ') "
									   "     FROM unnest(conkey, confkey) AS k(c, p) "
									   "     JOIN pg_attribute ca ON ca.attrelid = conrelid AND ca.attnum = k.c "
									   "     JOIN pg_attribute pa ON pa.attrelid = confrelid AND pa.attnum = k.p)) "
									   "FROM pg_constraint WHERE conname = $1 AND contype = 'f' "
									   "AND connamespace = '" SCHEMA "'::regnamespace",
									   2, NULL, (const char *[]) {cons, rk}, NULL, NULL, 0);

		(void) params;
		if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1)
			sql = strdup(PQgetvalue(res, 0, 0));
		PQclear(res);
	}
	else if (is_check)
	{
		PGresult   *res = PQexecParams(c,
									   "SELECT format('SELECT %s FROM ONLY %s x WHERE (%s) IS FALSE', $2::text, "
									   "  conrelid::regclass, pg_get_expr(conbin, conrelid)) "
									   "FROM pg_constraint WHERE conname = $1 AND contype = 'c' "
									   "AND connamespace = '" SCHEMA "'::regnamespace",
									   2, NULL, (const char *[]) {cons, rk}, NULL, NULL, 0);

		if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1)
			sql = strdup(PQgetvalue(res, 0, 0));
		PQclear(res);
	}
	else if (is_unique)
	{
		const char *open = strstr(ddl, " UNIQUE ");
		const char *after = NULL;
		char	   *list,
				   *keys,
				   *pred = NULL;

		if (strstr(ddl, "UNIQUE INDEX"))
		{
			open = strstr(ddl, " USING ");
			open = open ? strchr(open + 7, '(') : NULL;
		}
		else
			open = open ? strchr(open, '(') : NULL;
		list = open ? paren_body(open, &after) : NULL;
		if (list)
		{
			const char *w = strstr(after, " WHERE ");

			keys = key_list(list);
			if (w)
				pred = strip_semicolon(w + 7);
			sql = px_malloc(strlen(keys) * 3 + strlen(table) * 2 + strlen(rk) +
							(pred ? 2 * strlen(pred) : 0) + 256);
			sprintf(sql, "SELECT %s FROM ONLY " SCHEMA ".%s x WHERE (%s) IN (SELECT %s FROM ONLY "
					SCHEMA ".%s%s%s GROUP BY %s HAVING count(*) > 1)%s%s",
					rk, table, keys, keys, table, pred ? " WHERE " : "", pred ? pred : "", keys,
					pred ? " AND " : "", pred ? pred : "");
			free(keys);
			free(list);
			free(pred);
		}
	}
	else if (is_notnull && p->column[0])
	{
		sql = px_malloc(strlen(rk) + strlen(table) + strlen(p->column) + 128);
		sprintf(sql, "SELECT %s FROM ONLY " SCHEMA ".%s x WHERE x.\"%s\" IS NULL", rk, table,
				p->column);
	}
	if (sql)
		n = record_rows(c, sql, table, cons, kind, out);
	if (n <= 0)
	{
		char		line[1400];

		snprintf(line, sizeof(line), "violation %s %s *", cons, table);
		lines_add(out, line);
	}
	{
		/* The constraint's own entry: its key and definition by tag. */
		char	   *detail = px_malloc(strlen(p->key) + strlen(p->ddl) + 2);
		const char *params[4] = {table, cons, kind, detail};
		PGresult   *res;

		sprintf(detail, "%s\n%s", p->key, p->ddl);
		res = PQexecParams(c, "INSERT INTO prolly.violations VALUES ($1, $2, $3, NULL, $4)",
						   4, NULL, params, NULL, NULL, 0);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
			die("violations: %s", PQerrorMessage(c));
		PQclear(res);
		free(detail);
	}
	free(sql);
	free(rk);
	free(table);
	free(ddl);
	colmap_free(&cols);
}

/*
 * Records what a restore reported: violations (SQLSTATE class 23) in
 * prolly.violations, the rest in unapplied.
 */
void
ws_record_violations(const char *db, const WsReport *rep, WsLines *out, WsLines *unapplied)
{
	PGconn	   *c = pg_open(db);

	pg_cmd(c, "BEGIN");
	for (size_t i = 0; i < rep->n; i++)
	{
		const WsProblem *p = &rep->p[i];

		if (strncmp(p->sqlstate, "23", 2) == 0)
			record_one(c, p, out);
		else
		{
			char		line[1400];

			snprintf(line, sizeof(line), "unapplied %s (%s)", p->what, p->message);
			lines_add(unapplied, line);
		}
	}
	pg_cmd(c, "COMMIT");
	PQfinish(c);
}

/*
 * Applies each deferred constraint again; those that now hold are cleared
 * with their rows.  Returns how many still fail (listed in unresolved).
 */
int
ws_resolve(const char *db, WsLines *unresolved)
{
	PGconn	   *c = pg_open(db);
	PGresult   *res = pg_exec(c, "SELECT tbl, cons, kind, detail FROM prolly.violations "
							  "WHERE rowkey IS NULL ORDER BY tbl, cons");
	ColMap		cols;
	int			left = 0;

	colmap_from_db(c, &cols);
	for (int i = 0; i < PQntuples(res); i++)
	{
		const char *tbl = PQgetvalue(res, i, 0),
				   *cons = PQgetvalue(res, i, 1),
				   *kind = PQgetvalue(res, i, 2),
				   *def = strchr(PQgetvalue(res, i, 3), '\n') + 1;
		const char *params[2] = {tbl, cons};
		Buf			r = {0};
		PGresult   *r2;
		char		sql[1400];

		if (!render(def, strlen(def), &cols, &r, NULL))
			die("%s", "a deferred constraint names an unknown column");
		buf_append(&r, "", 1);
		pg_cmd(c, "BEGIN");
		if (strcmp(kind, "foreign key") == 0 || strcmp(kind, "check") == 0)
		{
			snprintf(sql, sizeof(sql), "ALTER TABLE " SCHEMA ".%s DROP CONSTRAINT IF EXISTS \"%s\"",
					 tbl, cons);
			pg_cmd(c, sql);
		}
		r2 = PQexec(c, (const char *) r.data);
		if (PQresultStatus(r2) == PGRES_COMMAND_OK)
		{
			PQclear(r2);
			r2 = PQexecParams(c, "DELETE FROM prolly.violations WHERE tbl = $1 AND cons = $2",
							  2, NULL, params, NULL, NULL, 0);
			PQclear(r2);
			pg_cmd(c, "COMMIT");
		}
		else
		{
			snprintf(sql, sizeof(sql), "%s %s: %s", cons, tbl,
					 PQresultErrorField(r2, PG_DIAG_MESSAGE_PRIMARY));
			lines_add(unresolved, sql);
			PQclear(r2);
			pg_cmd(c, "ROLLBACK");
			left++;
		}
		free(r.data);
	}
	PQclear(res);
	colmap_free(&cols);
	PQfinish(c);
	return left;
}

PHash
capture_violations(ProllyCtx *ctx, PGconn *c)
{
	PGresult   *res = pg_exec(c,
							  "SELECT k, v FROM (SELECT tbl || E'\\t' || cons || E'\\t' || "
							  "  coalesce(rowkey, '') AS k, kind || E'\\t' || detail AS v "
							  "  FROM prolly.violations) s ORDER BY k COLLATE \"C\"");
	int			n = broken("noviol") ? 0 : PQntuples(res);
	ProllyKV   *kv = px_malloc((size_t) (n ? n : 1) * sizeof(ProllyKV));
	size_t		m = 0;
	PHash		out;

	for (int i = 0; i < n; i++)
	{
		const char *k = PQgetvalue(res, i, 0);

		if (m > 0 && strcmp((const char *) kv[m - 1].key, k) == 0)
			continue;			/* the same row twice */
		kv[m].key = (const uint8_t *) k;
		kv[m].klen = strlen(k);
		kv[m].val = (const uint8_t *) PQgetvalue(res, i, 1);
		kv[m].vlen = strlen(PQgetvalue(res, i, 1));
		m++;
	}
	out = t_build(ctx, kv, m);
	free(kv);
	PQclear(res);
	return out;
}

void
restore_violations(ProllyCtx *ctx, PGconn *c, const PHash *data)
{
	ProllyIter *it = t_iter(ctx, data, NULL, 0);
	const uint8_t *k,
			   *v;
	size_t		kl,
				vl;

	while (t_next(it, &k, &kl, &v, &vl))
	{
		char	   *key = strndup((const char *) k, kl),
				   *val = strndup((const char *) v, vl),
				   *c2 = strchr(key, '\t'),
				   *c3,
				   *d;
		const char *params[5];
		PGresult   *res;

		CHECK(c2 != NULL);
		*c2++ = '\0';
		c3 = strchr(c2, '\t');
		*c3++ = '\0';
		d = strchr(val, '\t');
		*d++ = '\0';
		params[0] = key;
		params[1] = c2;
		params[2] = val;
		params[3] = *c3 ? c3 : NULL;
		params[4] = d;
		res = PQexecParams(c, "INSERT INTO prolly.violations VALUES ($1, $2, $3, $4, $5)",
						   5, NULL, params, NULL, NULL, 0);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
			die("violations: %s", PQerrorMessage(c));
		PQclear(res);
		free(key);
		free(val);
	}
	prolly_iter_close(it);
}
