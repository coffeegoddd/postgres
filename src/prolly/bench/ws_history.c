/*
 * ws_history.c
 *	  Reading a table as of a commit, under today's security.
 *
 * A commit's rows are history, and whoever reads history reads it as who
 * they are now: the table's current row security policies and current
 * SELECT grants (table and column) decide what they see, not the policies
 * the commit was made under, which may have been looser.  So a table as of
 * a commit is materialized into a scratch table with the commit's columns
 * and rows, and given the current table's policies and grants; queries
 * against it, run as the reader, are filtered as queries against the
 * table itself would be.  When a current policy cannot apply to the
 * commit's columns (it names one the commit does not have), the read is
 * refused: nothing is shown unprotected.
 */
#include "ws.h"

/* Runs sql; on failure, its message in err (if given) and false. */
static bool
try_cmd(PGconn *c, const char *sql, char *err, size_t errlen)
{
	PGresult   *res = PQexec(c, sql);
	bool		ok = PQresultStatus(res) == PGRES_COMMAND_OK ||
		PQresultStatus(res) == PGRES_TUPLES_OK;

	if (!ok && err)
		snprintf(err, errlen, "%s", PQresultErrorMessage(res));
	PQclear(res);
	return ok;
}

/*
 * Materializes table as of commit root into SCRATCH_SCHEMA.scratch on
 * connection c (a superuser), with the current table's policies and SELECT
 * grants.  False, with the reason in err, if it cannot be read safely.
 */
bool
ws_history_table(ProllyCtx *ctx, const PHash *root, const char *table, PGconn *c,
				 const char *scratch, char *err, size_t errlen)
{
	WsLayout	L;
	PHash		data;
	Buf			sql = {0},
				line = {0};
	char		cur[600],
				dst[600],
				buf[2000];
	PGresult   *res;
	ProllyIter *it;
	const uint8_t *k,
			   *v;
	size_t		kl,
				vl;
	WsField		f[WS_MAX_COLS];
	bool		ok = true;

	if (!table_layout(ctx, root, table, &L, &data))
	{
		snprintf(err, errlen, "the commit has no table %s", table);
		return false;
	}
	snprintf(cur, sizeof(cur), SCHEMA ".%s", L.rel);
	snprintf(dst, sizeof(dst), SCRATCH_SCHEMA ".%s", scratch);
	pg_cmd(c, "BEGIN");
	pg_cmd(c, "CREATE SCHEMA IF NOT EXISTS " SCRATCH_SCHEMA);
	snprintf(buf, sizeof(buf), "DROP TABLE IF EXISTS %s", dst);
	pg_cmd(c, buf);

	/* the commit's columns; generated ones hold the values the commit holds */
	buf_append(&sql, "CREATE TABLE ", 13);
	buf_append(&sql, dst, strlen(dst));
	buf_append(&sql, " (", 2);
	for (int i = 0, n = 0; i < L.ncols; i++)
	{
		if (L.cols[i].kind == 'v')
			continue;
		if (n++)
			buf_append(&sql, ", ", 2);
		buf_append(&sql, L.cols[i].name, strlen(L.cols[i].name));
		buf_append(&sql, " ", 1);
		buf_append(&sql, L.cols[i].type, strlen(L.cols[i].type));
	}
	buf_append(&sql, ")", 2);	/* with the NUL */
	pg_cmd(c, (const char *) sql.data);

	/* the commit's rows */
	sql.len = 0;
	buf_append(&sql, "COPY ", 5);
	buf_append(&sql, dst, strlen(dst));
	buf_append(&sql, " (", 2);
	ws_copy_list(&L, &sql);
	buf_append(&sql, ") FROM STDIN", 13);
	res = PQexec(c, (const char *) sql.data);
	if (PQresultStatus(res) != PGRES_COPY_IN)
		die("history copy: %s", PQerrorMessage(c));
	PQclear(res);
	it = t_iter(ctx, &data, NULL, 0);
	while (t_next(it, &k, &kl, &v, &vl))
	{
		long		times = 1;

		if (L.nkey == 0)
		{
			char		cnt[24];

			snprintf(cnt, sizeof(cnt), "%.*s", (int) vl, (const char *) v);
			times = atol(cnt);
		}
		ws_decode(&L, k, kl, v, vl, f);
		ws_copy_line(&L, f, &line);
		for (long i = 0; i < times; i++)
			if (PQputCopyData(c, (const char *) line.data, (int) line.len) != 1)
				die("history copy: %s", PQerrorMessage(c));
	}
	prolly_iter_close(it);
	if (PQputCopyEnd(c, NULL) != 1)
		die("history copy: %s", PQerrorMessage(c));
	res = PQgetResult(c);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		die("history copy: %s", PQerrorMessage(c));
	PQclear(res);

	/* today's security: the current table's row security and policies */
	snprintf(buf, sizeof(buf),
			 "SELECT c.relrowsecurity, c.relforcerowsecurity FROM pg_class c "
			 "WHERE c.oid = to_regclass('%s')", cur);
	res = pg_exec(c, buf);
	if (PQntuples(res) == 0)
	{
		/* no table today: nobody but a superuser reads it */
		PQclear(res);
		goto done;
	}
	if (PQgetvalue(res, 0, 0)[0] == 't' && !broken("oldpolicies"))
	{
		snprintf(buf, sizeof(buf), "ALTER TABLE %s ENABLE ROW LEVEL SECURITY", dst);
		pg_cmd(c, buf);
	}
	if (PQgetvalue(res, 0, 1)[0] == 't')
	{
		snprintf(buf, sizeof(buf), "ALTER TABLE %s FORCE ROW LEVEL SECURITY", dst);
		pg_cmd(c, buf);
	}
	PQclear(res);
	snprintf(buf, sizeof(buf),
			 "SELECT format('CREATE POLICY %%I ON %s AS %%s FOR %%s TO %%s%%s%%s', p.polname, "
			 "  CASE WHEN p.polpermissive THEN 'PERMISSIVE' ELSE 'RESTRICTIVE' END, "
			 "  CASE p.polcmd WHEN 'r' THEN 'SELECT' WHEN 'a' THEN 'INSERT' WHEN 'w' THEN 'UPDATE' "
			 "    WHEN 'd' THEN 'DELETE' ELSE 'ALL' END, "
			 "  CASE WHEN p.polroles = '{0}' THEN 'PUBLIC' ELSE "
			 "    (SELECT string_agg(quote_ident(rolname), ', ') FROM pg_roles WHERE oid = ANY (p.polroles)) END, "
			 "  coalesce(' USING (' || pg_get_expr(p.polqual, p.polrelid) || ')', ''), "
			 "  coalesce(' WITH CHECK (' || pg_get_expr(p.polwithcheck, p.polrelid) || ')', '')) "
			 "FROM pg_policy p WHERE p.polrelid = '%s'::regclass", dst, cur);
	res = pg_exec(c, buf);
	for (int i = 0; i < PQntuples(res) && ok && !broken("oldpolicies"); i++)
	{
		pg_cmd(c, "SAVEPOINT policy");
		if (!try_cmd(c, PQgetvalue(res, i, 0), err, errlen))
		{
			/* a policy today's table has that the commit cannot carry */
			pg_cmd(c, "ROLLBACK TO SAVEPOINT policy");
			ok = false;
		}
	}
	PQclear(res);

	/* today's SELECT grants, on the table and on each column the commit has */
	snprintf(buf, sizeof(buf),
			 "SELECT format('GRANT SELECT ON %s TO %%s', "
			 "  CASE WHEN a.grantee = 0 THEN 'PUBLIC' ELSE quote_ident(a.grantee::regrole::text) END) "
			 "FROM pg_class c, aclexplode(c.relacl) a "
			 "WHERE c.oid = '%s'::regclass AND a.privilege_type = 'SELECT'", dst, cur);
	res = pg_exec(c, buf);
	for (int i = 0; i < PQntuples(res); i++)
		pg_cmd(c, PQgetvalue(res, i, 0));
	PQclear(res);
	snprintf(buf, sizeof(buf),
			 "SELECT format('GRANT SELECT (%%I) ON %s TO %%s', t.attname, "
			 "  CASE WHEN a.grantee = 0 THEN 'PUBLIC' ELSE quote_ident(a.grantee::regrole::text) END) "
			 "FROM pg_attribute t, aclexplode(t.attacl) a "
			 "WHERE t.attrelid = '%s'::regclass AND a.privilege_type = 'SELECT' "
			 "  AND EXISTS (SELECT FROM pg_attribute d WHERE d.attrelid = '%s'::regclass "
			 "    AND d.attname = t.attname AND NOT d.attisdropped)", dst, cur, dst);
	res = pg_exec(c, buf);
	for (int i = 0; i < PQntuples(res); i++)
		pg_cmd(c, PQgetvalue(res, i, 0));
	PQclear(res);
	snprintf(buf, sizeof(buf), "GRANT USAGE ON SCHEMA " SCRATCH_SCHEMA " TO PUBLIC");
	pg_cmd(c, buf);

done:
	if (ok)
		pg_cmd(c, "COMMIT");
	else
		pg_cmd(c, "ROLLBACK");
	ws_layout_free(&L);
	free(sql.data);
	free(line.data);
	return ok;
}
