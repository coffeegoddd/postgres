/*
 * ws_move.c
 *	  Moves a live working set from one root value to another in place: the
 *	  schema changes between them, then only the rows that differ.
 *
 * In order:
 *	1. enum values B adds to an enum A has, added in place (outside the
 *	   transaction: a new value cannot be used in the one that adds it);
 *	2. objects A has that B lacks or changed are dropped;
 *	3. tables: each table B drops is dropped and each it adds created; a
 *	   table both have is altered in place when the change allows it
 *	   (columns renamed, added at the end, dropped or retyped; defaults,
 *	   NOT NULL and table constraints changed) and recreated otherwise
 *	   (columns reordered, partitioning or other definition changes, a type
 *	   it uses dropped);
 *	4. tags as B has them;
 *	5. rows: a recreated or new table is loaded whole; for the others only
 *	   the rows that differ are deleted and inserted, by key, and rows whose
 *	   bytes differ only because of the schema change are left alone;
 *	6. constraints and NOT NULLs that need the rows, and sequences;
 *	7. then, until the database's objects are B's: what B has and the
 *	   database lacks (or has otherwise) is created, as in a restore, and
 *	   what the database has and B lacks is dropped;
 *	8. populated materialized views refreshed.
 * No user code runs: rows go through prolly_apply_insert and
 * prolly_apply_delete, which fire no triggers and sit below the rule
 * rewriter, and each transaction runs between prolly.quiet() and
 * prolly.unquiet(), which set session_replication_role = replica and turn
 * off, while it runs, the triggers, rules and event triggers enabled
 * ALWAYS or REPLICA.  What the commits hold already includes what those
 * did when the rows were first written.
 */
#include "ws.h"

static char *
qid(PGconn *c, const char *name)
{
	char	   *q = PQescapeIdentifier(c, name, strlen(name));
	char	   *out = strdup(q);

	PQfreemem(q);
	return out;
}

static char *
rendered(const char *t, size_t n, const ColMap *m)
{
	Buf			r = {0};
	uint64_t	missing = 0;

	if (!render(t, n, m, &r, &missing))
	{
		char		msg[64];

		snprintf(msg, sizeof(msg), "%" PRIu64, missing);
		die("an object names unknown column tag %s", msg);
	}
	buf_append(&r, "", 1);
	return (char *) r.data;
}

static void
load_objs(ProllyCtx *ctx, const PHash *rv, ObjList *l)
{
	VcTable		t;
	ProllyIter *it;
	const uint8_t *k,
			   *v;
	size_t		kl,
				vl;

	memset(l, 0, sizeof(*l));
	CHECK(vc_table_get(ctx, rv, "~obj", &t));
	it = t_iter(ctx, &t.data, NULL, 0);
	while (t_next(it, &k, &kl, &v, &vl))
		obj_add(l, (const char *) k, kl, (const char *) v, vl);
	prolly_iter_close(it);
}

static const ProllyKV *
obj_findn(const ObjList *l, const uint8_t *key, size_t kl)
{
	size_t		lo = 0,
				hi = l->n;

	while (lo < hi)
	{
		size_t		mid = (lo + hi) / 2;
		int			c = key_cmp(l->kv[mid].key, l->kv[mid].klen, key, kl);

		if (c == 0)
			return &l->kv[mid];
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return NULL;
}

/* Keys of object lists are not NUL-terminated; these are. */
static const ProllyKV *
obj_find(const ObjList *l, const char *key)
{
	return obj_findn(l, (const uint8_t *) key, strlen(key));
}

static bool
kv_same(const ProllyKV *a, const ProllyKV *b)
{
	return a && b && bytes_eq(a->val, a->vlen, b->val, b->vlen);
}

/* Section, type and name of an object's key. */
static void
key_parts(const ProllyKV *kv, char *type, size_t tc, char *name, size_t nc)
{
	char		key[1200];
	char	   *t,
			   *n;

	snprintf(key, sizeof(key), "%.*s", (int) kv->klen, (const char *) kv->key);
	t = key + 2;
	n = strchr(t, '\t');
	*n++ = '\0';
	snprintf(type, tc, "%.*s", (int) (tc - 1), t);
	snprintf(name, nc, "%.*s", (int) (nc - 1), n);
}

/* The first rt.<relation> a definition names, as written. */
static char *
first_rel(const char *ddl)
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

/*
 * How to take an object away: SQL, "" for nothing to do, or NULL if not
 * known (the caller reports it).  ddl is the object's rendered definition.
 */
static char *
drop_sql(PGconn *c, const char *type, const char *name, const char *ddl)
{
	char		buf[3000];
	char		a[600],
				b[600];
	char	   *qa = NULL,
			   *qb = NULL;
	const char *sp = strchr(name, ' ');

	if (sp)
	{
		snprintf(a, sizeof(a), "%.*s", (int) (sp - name), name);
		snprintf(b, sizeof(b), "%s", sp + 1);
	}
	else
	{
		snprintf(a, sizeof(a), "%s", name);
		b[0] = '\0';
	}
	qa = qid(c, a);
	qb = qid(c, b[0] ? b : a);
	buf[0] = '\0';
	if (strcmp(type, "INDEX") == 0)
		snprintf(buf, sizeof(buf), "DROP INDEX IF EXISTS " SCHEMA ".%s CASCADE", qa);
	else if (strcmp(type, "CONSTRAINT") == 0 || strcmp(type, "FK CONSTRAINT") == 0 ||
			 strcmp(type, "CHECK CONSTRAINT") == 0)
		snprintf(buf, sizeof(buf), "ALTER TABLE IF EXISTS " SCHEMA ".%s DROP CONSTRAINT IF EXISTS %s CASCADE",
				 qa, qb);
	else if (strcmp(type, "TRIGGER") == 0)
		snprintf(buf, sizeof(buf), "DROP TRIGGER IF EXISTS %s ON " SCHEMA ".%s", qb, qa);
	else if (strcmp(type, "POLICY") == 0)
		snprintf(buf, sizeof(buf), "DROP POLICY IF EXISTS %s ON " SCHEMA ".%s", qb, qa);
	else if (strcmp(type, "RULE") == 0)
		snprintf(buf, sizeof(buf), "DROP RULE IF EXISTS %s ON " SCHEMA ".%s", qb, qa);
	else if (strcmp(type, "ROW SECURITY") == 0)
		snprintf(buf, sizeof(buf), "ALTER TABLE IF EXISTS " SCHEMA ".%s DISABLE ROW LEVEL SECURITY", qa);
	else if (strcmp(type, "VIEW") == 0)
		snprintf(buf, sizeof(buf), "DROP VIEW IF EXISTS " SCHEMA ".%s CASCADE", qa);
	else if (strcmp(type, "MATERIALIZED VIEW") == 0)
		snprintf(buf, sizeof(buf), "DROP MATERIALIZED VIEW IF EXISTS " SCHEMA ".%s CASCADE", qa);
	else if (strcmp(type, "STATISTICS") == 0)
		snprintf(buf, sizeof(buf), "DROP STATISTICS IF EXISTS " SCHEMA ".%s", qa);
	else if (strcmp(type, "SEQUENCE") == 0)
		snprintf(buf, sizeof(buf), "DROP SEQUENCE IF EXISTS " SCHEMA ".%s CASCADE", qa);
	else if (strcmp(type, "TYPE") == 0)
		snprintf(buf, sizeof(buf), "DROP TYPE IF EXISTS " SCHEMA ".%s CASCADE", qa);
	else if (strcmp(type, "DOMAIN") == 0)
		snprintf(buf, sizeof(buf), "DROP DOMAIN IF EXISTS " SCHEMA ".%s CASCADE", qa);
	else if (strcmp(type, "COLLATION") == 0)
		snprintf(buf, sizeof(buf), "DROP COLLATION IF EXISTS " SCHEMA ".%s CASCADE", qa);
	else if (strcmp(type, "FUNCTION") == 0 || strcmp(type, "PROCEDURE") == 0)
		snprintf(buf, sizeof(buf), "DROP %s IF EXISTS " SCHEMA ".%s CASCADE", type, name);
	else if (strcmp(type, "DEFAULT") == 0)
		snprintf(buf, sizeof(buf), "ALTER TABLE IF EXISTS " SCHEMA ".%s ALTER COLUMN %s DROP DEFAULT",
				 qa, qb);
	else if (strcmp(type, "SEQUENCE OWNED BY") == 0)
		snprintf(buf, sizeof(buf), "ALTER SEQUENCE IF EXISTS " SCHEMA ".%s OWNED BY NONE", qa);
	else if (strcmp(type, "TABLE ATTACH") == 0)
	{
		char	   *parent = first_rel(ddl);

		if (parent)
			snprintf(buf, sizeof(buf), "ALTER TABLE IF EXISTS " SCHEMA ".%s DETACH PARTITION " SCHEMA ".%s",
					 parent, qa);
		free(parent);
	}
	else if (strcmp(type, "COMMENT") == 0)
	{
		const char *is = strstr(ddl, " IS ");

		if (is)
			snprintf(buf, sizeof(buf), "%.*s IS NULL", (int) (is - ddl), ddl);
	}
	else if (strcmp(type, "MATERIALIZED VIEW DATA") == 0 || strcmp(type, "ACL") == 0 ||
			 strcmp(type, "SCHEMA") == 0 || strcmp(type, "INDEX ATTACH") == 0)
		buf[0] = '\0';
	else
	{
		free(qa);
		free(qb);
		return NULL;
	}
	free(qa);
	free(qb);
	return strdup(buf);
}

static void
exec_or_report(PGconn *c, const char *sql, const char *what, WsReport *rep)
{
	PGresult   *res;

	bool		intx;

	if (sql == NULL || sql[0] == '\0')
		return;
	intx = PQtransactionStatus(c) == PQTRANS_INTRANS;
	if (intx)
		pg_cmd(c, "SAVEPOINT mv");
	res = PQexec(c, sql);
	if (PQresultStatus(res) == PGRES_COMMAND_OK || PQresultStatus(res) == PGRES_TUPLES_OK)
	{
		if (intx)
			pg_cmd(c, "RELEASE SAVEPOINT mv");
	}
	else
	{
		if (intx)
			pg_cmd(c, "ROLLBACK TO SAVEPOINT mv");
		report_add(rep, what, res);
	}
	PQclear(res);
}

/* ------------------------------------------------------------- enums */

/* The labels of "CREATE TYPE ... AS ENUM ( 'a', 'b' );", or 0 if not one. */
static int
enum_labels(const char *ddl, char **out, int max)
{
	const char *p = strstr(ddl, " AS ENUM (");
	int			n = 0;

	if (p == NULL)
		return 0;
	for (p += 10; *p && *p != ')' && n < max;)
	{
		if (*p == '\'')
		{
			Buf			b = {0};

			for (p++; *p; p++)
			{
				if (*p == '\'' && p[1] == '\'')
				{
					buf_append(&b, "'", 1);
					p++;
					continue;
				}
				if (*p == '\'')
					break;
				buf_append(&b, p, 1);
			}
			buf_append(&b, "", 1);
			out[n++] = (char *) b.data;
		}
		if (*p)
			p++;
	}
	return n;
}

/* Values B adds to an enum, in place; false if B is not A plus values. */
static bool
enum_add(PGconn *c, const char *name, const char *a, const char *b, WsReport *rep)
{
	char	   *la[256],
			   *lb[256];
	int			na = enum_labels(a, la, 256),
				nb = enum_labels(b, lb, 256),
				i = 0;
	bool		ok = na > 0 && nb > na;
	char	   *qn = qid(c, name);

	/* A's labels must be B's in order, with others between. */
	for (int j = 0; ok && j < nb; j++)
		if (i < na && strcmp(la[i], lb[j]) == 0)
			i++;
	ok &= i == na;
	for (int j = 0; ok && j < nb; j++)
	{
		bool		have = false;
		char	   *lit,
				   *nb_lit;
		char		sql[1200];

		for (int q = 0; q < na; q++)
			have |= strcmp(la[q], lb[j]) == 0;
		if (have)
			continue;
		lit = PQescapeLiteral(c, lb[j], strlen(lb[j]));
		if (j + 1 < nb)
		{
			nb_lit = PQescapeLiteral(c, lb[j + 1], strlen(lb[j + 1]));
			snprintf(sql, sizeof(sql), "ALTER TYPE " SCHEMA ".%s ADD VALUE %s BEFORE %s", qn, lit, nb_lit);
			PQfreemem(nb_lit);
		}
		else
			snprintf(sql, sizeof(sql), "ALTER TYPE " SCHEMA ".%s ADD VALUE %s", qn, lit);
		PQfreemem(lit);
		{
			PGresult   *res = PQexec(c, sql);

			if (PQresultStatus(res) != PGRES_COMMAND_OK)
				report_add(rep, "enum value", res);
			PQclear(res);
		}
	}
	for (int q = 0; q < na; q++)
		free(la[q]);
	for (int q = 0; q < nb; q++)
		free(lb[q]);
	free(qn);
	return ok;
}

/* ------------------------------------------------------------- tables */

typedef struct MoveTable
{
	char	   *name;			/* as in the object key (unquoted) */
	bool		fresh;			/* created or recreated: rows loaded whole */
	bool		recreate;
} MoveTable;

typedef struct Move
{
	ProllyCtx  *ctx;
	PGconn	   *c;
	const PHash *from,
			   *to;
	ColMap		ca,
				cb;
	ObjList		oa,
				ob;
	MoveTable  *t;
	size_t		nt,
				tcap;
	Buf			later;			/* statements that need the rows */
	WsMoveStats *st;
	WsReport   *rep;
} Move;

static MoveTable *
mt_get(Move *mv, const char *name)
{
	for (size_t i = 0; i < mv->nt; i++)
		if (strcmp(mv->t[i].name, name) == 0)
			return &mv->t[i];
	px_grow((void **) &mv->t, &mv->tcap, mv->nt + 1, sizeof(MoveTable));
	memset(&mv->t[mv->nt], 0, sizeof(MoveTable));
	mv->t[mv->nt].name = strdup(name);
	return &mv->t[mv->nt++];
}

static const ColRef *
col_by_tag(const ColMap *m, uint64_t tag)
{
	for (size_t i = 0; i < m->n; i++)
		if (m->c[i].tag == tag)
			return &m->c[i];
	return NULL;
}

/* The text inside the parentheses opening at p. */
static char *
paren_text(const char *p)
{
	int			depth = 0;

	for (const char *q = p; *q; q++)
	{
		if (*q == '(')
			depth++;
		else if (*q == ')' && --depth == 0)
			return strndup(p + 1, (size_t) (q - p - 1));
	}
	return strdup("");
}

/* The column line with the parts an ALTER changes taken out. */
static char *
line_rest(const char *line, const WsCol *col)
{
	char	   *s = strdup(line),
			   *p;
	char		def[4200];

	if (col)
	{
		if (col->def[0])
		{
			snprintf(def, sizeof(def), " DEFAULT %s", col->def);
			if ((p = strstr(s, def)))
				memmove(p, p + strlen(def), strlen(p + strlen(def)) + 1);
		}
		if ((p = strstr(s, col->type)) && p > s && p[-1] == ' ')
			memmove(p, p + strlen(col->type), strlen(p + strlen(col->type)) + 1);
	}
	if ((p = strstr(s, " NOT NULL")))
		memmove(p, p + 9, strlen(p + 9) + 1);
	return s;
}

static void
later(Move *mv, const char *sql)
{
	buf_append(&mv->later, sql, strlen(sql));
	buf_append(&mv->later, "\n\x01", 2);	/* one statement per separator */
}

/*
 * Alters a table B and A both have from A's definition to B's; false if
 * the change needs the table recreated.  Statements needing the rows (NOT
 * NULL, constraints) go to mv->later.
 */
static bool
alter_table(Move *mv, const char *name, const char *ta, const char *tb, const WsLayout *LA,
			const WsLayout *LB)
{
	Ddl			da,
				db;
	char	   *qt = qid(mv->c, name);
	Buf			sql = {0};
	bool		ok = true;
	int			last_kept = -1,
				first_added = INT32_MAX;
	const char *semi_a,
			   *semi_b;

	if (!ddl_parse(ta, LA, &da) || !ddl_parse(tb, LB, &db))
	{
		free(qt);
		return false;
	}
	/*
	 * The definition beyond its items must match, up to its first
	 * statement, but for storage parameters, which are set in place.
	 */
	semi_a = strchr(da.tail, ';');
	semi_b = strchr(db.tail, ';');
	if (strcmp(da.head, db.head) != 0 || semi_a == NULL || semi_b == NULL)
		ok = false;
	else
	{
		char	   *fa = strndup(da.tail, (size_t) (semi_a - da.tail)),
				   *fb = strndup(db.tail, (size_t) (semi_b - db.tail));
		char	   *wa = strstr(fa, "\nWITH ("),
				   *wb = strstr(fb, "\nWITH (");
		char	   *oa = wa ? paren_text(wa + 6) : strdup(""),
				   *ob = wb ? paren_text(wb + 6) : strdup("");

		if (wa)
			memmove(wa, wa + 7 + strlen(oa) + 1, strlen(wa + 7 + strlen(oa) + 1) + 1);
		if (wb)
			memmove(wb, wb + 7 + strlen(ob) + 1, strlen(wb + 7 + strlen(ob) + 1) + 1);
		if (strcmp(fa, fb) != 0)
			ok = false;
		else if (strcmp(oa, ob) != 0)
		{
			char		stmt[4400];

			/* Each of A's parameters reset, then B's set. */
			for (char *p = oa, *e; *p; p = *e ? e + 2 : e)
			{
				e = strstr(p, ", ");
				if (e == NULL)
					e = p + strlen(p);
				snprintf(stmt, sizeof(stmt), "ALTER TABLE " SCHEMA ".%s RESET (%.*s)", qt,
						 (int) strcspn(p, "="), p);
				exec_or_report(mv->c, stmt, "reset storage parameter", mv->rep);
			}
			if (ob[0])
			{
				snprintf(stmt, sizeof(stmt), "ALTER TABLE " SCHEMA ".%s SET (%s)", qt, ob);
				exec_or_report(mv->c, stmt, "set storage parameters", mv->rep);
			}
			mv->st->altered++;
		}
		free(fa);
		free(fb);
		free(oa);
		free(ob);
	}
	/* Postgres adds columns at the end: B's new columns must come last. */
	for (int i = 0; ok && i < db.n; i++)
	{
		if (db.item[i].key[0] != 't')
			continue;
		if (ddl_item(&da, db.item[i].key))
			last_kept = i;
		else if (i < first_added)
			first_added = i;
	}
	if (ok && first_added != INT32_MAX && first_added < last_kept)
		ok = false;
	/* A's columns must keep their order in B. */
	for (int i = 0, prev = -1; ok && i < da.n; i++)
	{
		int			at = -1;

		if (da.item[i].key[0] != 't')
			continue;
		for (int j = 0; j < db.n; j++)
			if (strcmp(db.item[j].key, da.item[i].key) == 0)
				at = j;
		if (at < 0)
			continue;
		if (at < prev)
			ok = false;
		prev = at;
	}
	if (!ok)
		goto out;

	/* Dropped columns and constraints, then changes, then additions. */
	for (int i = 0; i < da.n; i++)
	{
		const char *k = da.item[i].key;
		char		stmt[4200];

		if (ddl_item(&db, k))
			continue;
		if (k[0] == 't')
		{
			const ColRef *r = col_by_tag(&mv->ca, strtoull(k + 2, NULL, 10));

			if (r == NULL)
			{
				ok = false;
				goto out;
			}
			snprintf(stmt, sizeof(stmt), "ALTER TABLE " SCHEMA ".%s DROP COLUMN %s CASCADE", qt, r->name);
		}
		else
			snprintf(stmt, sizeof(stmt), "ALTER TABLE " SCHEMA ".%s DROP CONSTRAINT %s", qt,
					 strstr(da.item[i].line, "CONSTRAINT ") + 11);
		if (k[0] == 'c')
			*strchr(strstr(stmt, "DROP CONSTRAINT ") + 16, ' ') = '\0';
		exec_or_report(mv->c, stmt, "drop column or constraint", mv->rep);
	}
	for (int i = 0; i < db.n; i++)
	{
		const char *k = db.item[i].key;
		const char *la = ddl_item(&da, k);
		char	   *lb = rendered(db.item[i].line, strlen(db.item[i].line), &mv->cb);
		char		stmt[8400];

		if (la && strcmp(la, db.item[i].line) == 0)
		{
			free(lb);
			continue;
		}
		if (k[0] == 'c')
		{
			/* Constraints come back after the rows. */
			const char *cn = strstr(lb, "CONSTRAINT ");

			if (la)
			{
				snprintf(stmt, sizeof(stmt), "ALTER TABLE " SCHEMA ".%s DROP CONSTRAINT %.*s", qt,
						 (int) strcspn(cn + 11, " "), cn + 11);
				exec_or_report(mv->c, stmt, "drop constraint", mv->rep);
			}
			snprintf(stmt, sizeof(stmt), "ALTER TABLE " SCHEMA ".%s ADD %s", qt, cn);
			later(mv, stmt);
			free(lb);
			continue;
		}
		if (la == NULL)
		{
			/* A new column: NOT NULL without a default waits for the rows. */
			char	   *p = strstr(lb, " NOT NULL");
			uint64_t	tag = strtoull(k + 2, NULL, 10);
			const WsCol *cbcol = ws_layout_col(LB, tag);

			if (p && cbcol && cbcol->def[0] == '\0')
			{
				const ColRef *r = col_by_tag(&mv->cb, tag);

				memmove(p, p + 9, strlen(p + 9) + 1);
				snprintf(stmt, sizeof(stmt), "ALTER TABLE " SCHEMA ".%s ALTER COLUMN %s SET NOT NULL",
						 qt, r ? r->name : "?");
				later(mv, stmt);
			}
			while (*lb == ' ')
				memmove(lb, lb + 1, strlen(lb));
			snprintf(stmt, sizeof(stmt), "ALTER TABLE " SCHEMA ".%s ADD COLUMN %s", qt, lb);
			exec_or_report(mv->c, stmt, "add column", mv->rep);
			mv->st->altered++;
			free(lb);
			continue;
		}
		{
			/* A changed column: type, default, NOT NULL; anything else recreates. */
			uint64_t	tag = strtoull(k + 2, NULL, 10);
			const WsCol *ca = ws_layout_col(LA, tag),
					   *cb = ws_layout_col(LB, tag);
			char	   *ra = line_rest(la, ca),
					   *rb = line_rest(db.item[i].line, cb);
			const ColRef *r = col_by_tag(&mv->cb, tag);
			bool		nna = strstr(la, " NOT NULL") != NULL,
						nnb = strstr(db.item[i].line, " NOT NULL") != NULL;

			if (strcmp(ra, rb) != 0 || ca == NULL || cb == NULL || r == NULL ||
				ca->kind != cb->kind || ca->kind == 'g')
			{
				free(ra);
				free(rb);
				free(lb);
				ok = false;
				goto out;
			}
			if (strcmp(ca->type, cb->type) != 0)
			{
				snprintf(stmt, sizeof(stmt), "ALTER TABLE " SCHEMA ".%s ALTER COLUMN %s TYPE %s USING %s::%s",
						 qt, r->name, cb->type, r->name, cb->type);
				exec_or_report(mv->c, stmt, "retype column", mv->rep);
			}
			if (strcmp(ca->def, cb->def) != 0)
			{
				if (cb->def[0])
					snprintf(stmt, sizeof(stmt), "ALTER TABLE " SCHEMA ".%s ALTER COLUMN %s SET DEFAULT %s",
							 qt, r->name, cb->def);
				else
					snprintf(stmt, sizeof(stmt), "ALTER TABLE " SCHEMA ".%s ALTER COLUMN %s DROP DEFAULT",
							 qt, r->name);
				exec_or_report(mv->c, stmt, "column default", mv->rep);
			}
			if (nna != nnb)
			{
				snprintf(stmt, sizeof(stmt), "ALTER TABLE " SCHEMA ".%s ALTER COLUMN %s %s NOT NULL",
						 qt, r->name, nnb ? "SET" : "DROP");
				if (nnb)
					later(mv, stmt);
				else
					exec_or_report(mv->c, stmt, "drop not null", mv->rep);
			}
			mv->st->altered++;
			free(ra);
			free(rb);
			free(lb);
		}
	}
	/* B's statements after CREATE TABLE (owner, storage, statistics). */
	if (strcmp(semi_a, semi_b) != 0)
	{
		char	   *rest = rendered(semi_b + 1, strlen(semi_b + 1), &mv->cb);
		PGresult   *res;

		pg_cmd(mv->c, "SAVEPOINT mv");
		res = PQexec(mv->c, rest);
		if (PQresultStatus(res) == PGRES_COMMAND_OK || PQresultStatus(res) == PGRES_EMPTY_QUERY)
			pg_cmd(mv->c, "RELEASE SAVEPOINT mv");
		else
		{
			pg_cmd(mv->c, "ROLLBACK TO SAVEPOINT mv");
			report_add(mv->rep, "table statements", res);
		}
		PQclear(res);
		free(rest);
	}
out:
	ddl_free(&da);
	ddl_free(&db);
	free(qt);
	free(sql.data);
	return ok;
}

/* Renames to B's names the columns B and A both have, through spare names. */
static void
rename_columns(Move *mv)
{
	for (int pass = 0; pass < 2; pass++)
		for (size_t i = 0; i < mv->cb.n; i++)
		{
			const ColRef *b = &mv->cb.c[i],
					   *a = col_by_tag(&mv->ca, b->tag);
			char		sql[1400];

			if (a == NULL || strcmp(a->table, b->table) != 0 || strcmp(a->name, b->name) == 0)
				continue;
			if (pass == 0)
			{
				snprintf(sql, sizeof(sql), "ALTER TABLE IF EXISTS " SCHEMA ".%s RENAME COLUMN %s TO \"~%" PRIu64 "\"",
						 a->table, a->name, b->tag);
				mv->st->renamed++;
			}
			else
				snprintf(sql, sizeof(sql), "ALTER TABLE IF EXISTS " SCHEMA ".%s RENAME COLUMN \"~%" PRIu64 "\" TO %s",
						 b->table, b->tag, b->name);
			exec_or_report(mv->c, sql, "rename column", mv->rep);
		}
}

static bool
rel_exists(PGconn *c, const char *quoted)
{
	char		rel[600];
	const char *params[1] = {rel};
	PGresult   *res;
	bool		yes;

	snprintf(rel, sizeof(rel), SCHEMA ".%s", quoted);
	res = PQexecParams(c, "SELECT to_regclass($1) IS NOT NULL", 1, NULL, params, NULL, NULL, 0);
	yes = PQresultStatus(res) == PGRES_TUPLES_OK && PQgetvalue(res, 0, 0)[0] == 't';
	PQclear(res);
	return yes;
}

/* --------------------------------------------------------------- rows */

static void
literal(PGconn *c, Buf *b, const WsField *f, const char *type)
{
	char	   *lit;

	if (f->null)
	{
		buf_append(b, "NULL", 4);
		return;
	}
	lit = PQescapeLiteral(c, (const char *) f->p, f->len);
	buf_append(b, lit, strlen(lit));
	buf_append(b, "::", 2);
	buf_append(b, type, strlen(type));
	PQfreemem(lit);
}

static void
run_sql(Move *mv, Buf *sql)
{
	PGresult   *res;

	if (sql->len == 0)
		return;
	buf_append(sql, "", 1);
	res = PQexec(mv->c, (const char *) sql->data);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		die("move rows: %s", PQerrorMessage(mv->c));
	PQclear(res);
	sql->len = 0;
}

/*
 * Whether a row's new bytes leave the database as it is after the schema
 * change: the columns both layouts store agree, and each column only B
 * stores holds what the database now has there (its default, or NULL).
 */
static bool
same_in_db(Move *mv, const WsLayout *LA, const WsLayout *LB, const uint8_t *va, size_t al,
		   const uint8_t *vb, size_t bl, Buf *defs, bool *have)
{
	if (broken("keepall"))
		return true;

	for (int i = LB->nkey; i < LB->nstored; i++)
	{
		uint64_t	tag = LB->copy[i];
		const uint8_t *pa = NULL,
				   *pb = NULL;
		size_t		la = 0,
					lb = 0;
		bool		ha,
					hb = ws_cell_get(vb, bl, tag, &pb, &lb);

		if (ws_copy_index(LA, tag) >= LA->nkey)
		{
			ha = ws_cell_get(va, al, tag, &pa, &la);
			if (ha != hb || (ha && !bytes_eq(pa, la, pb, lb)))
				return false;
			continue;
		}
		/* Only B stores it: what the database filled it with. */
		if (!have[i])
		{
			const WsCol *col = ws_layout_col(LB, tag);
			const ColRef *r = col_by_tag(&mv->cb, tag);

			defs[i].len = 0;
			if (col && col->def[0] && r)
			{
				char	   *sql = px_malloc(strlen(col->def) + strlen(col->type) + 64);
				PGresult   *res;

				sprintf(sql, "SELECT format('%%s', (%s)::%s)", col->def, col->type);
				res = PQexec(mv->c, sql);
				if (PQresultStatus(res) == PGRES_TUPLES_OK && !PQgetisnull(res, 0, 0))
				{
					buf_append(&defs[i], PQgetvalue(res, 0, 0), strlen(PQgetvalue(res, 0, 0)));
					buf_append(&defs[i], "", 1);
					defs[i].len--;
				}
				else
					defs[i].len = (size_t) -1;	/* volatile or failing: never the same */
				PQclear(res);
				free(sql);
			}
			have[i] = true;
		}
		if (defs[i].len == (size_t) -1)
			return false;
		if (defs[i].data == NULL || (defs[i].len == 0 && ws_layout_col(LB, tag)->def[0] == '\0'))
		{
			if (hb)
				return false;
		}
		else if (!hb || !bytes_eq(pb, lb, defs[i].data, defs[i].len))
			return false;
	}
	return true;
}

/* A quoted identifier (as quote_ident gives it) as the name it stands for. */
static void
unquote_ident(const char *q, Buf *out)
{
	if (q[0] != '"')
	{
		buf_append(out, q, strlen(q));
		return;
	}
	for (const char *p = q + 1; *p && !(p[0] == '"' && p[1] != '"'); p++)
	{
		buf_append(out, p, 1);
		if (p[0] == '"')
			p++;
	}
}

/* A text[] literal of the columns with these tags. */
static void
col_array(const WsLayout *L, const uint64_t *tags, int n, Buf *out)
{
	out->len = 0;
	buf_append(out, "{", 1);
	for (int i = 0; i < n; i++)
	{
		Buf			name = {0};

		unquote_ident(ws_layout_col(L, tags[i])->name, &name);
		if (i)
			buf_append(out, ",", 1);
		buf_append(out, "\"", 1);
		for (size_t j = 0; j < name.len; j++)
		{
			if (name.data[j] == '"' || name.data[j] == '\\')
				buf_append(out, "\\", 1);
			buf_append(out, &name.data[j], 1);
		}
		buf_append(out, "\"", 1);
		free(name.data);
	}
	buf_append(out, "}", 2);
}

/*
 * Applies data, COPY text for the columns in cols, with prolly_apply_insert
 * or prolly_apply_delete: no user code runs.  Every line must take effect.
 */
static void
apply_rows(Move *mv, const char *fn, const WsLayout *L, Buf *cols, Buf *data, uint64_t lines)
{
	char		rel[600],
				sql[200];
	const char *vals[3];
	PGresult   *res;

	if (lines == 0)
		return;
	snprintf(rel, sizeof(rel), SCHEMA ".%s", L->rel);
	snprintf(sql, sizeof(sql), "SELECT public.%s($1::regclass, $2::text[], $3)", fn);
	buf_append(data, "", 1);
	vals[0] = rel;
	vals[1] = (const char *) cols->data;
	vals[2] = (const char *) data->data;
	res = PQexecParams(mv->c, sql, 3, NULL, vals, NULL, NULL, 0);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		char		what[800];

		snprintf(what, sizeof(what), "%s %s: %s", fn, rel, PQerrorMessage(mv->c));
		die("move %s", what);
	}
	if (strtoull(PQgetvalue(res, 0, 0), NULL, 10) != lines)
	{
		char		what[800];

		snprintf(what, sizeof(what), "%s %s: %s of %llu rows", fn, rel, PQgetvalue(res, 0, 0),
				 (unsigned long long) lines);
		die("move %s", what);
	}
	PQclear(res);
	data->len = 0;
}

#define APPLY_BATCH (4 << 20)

/* The rows of one table that differ between A and B. */
static void
move_rows(Move *mv, const char *name, const WsLayout *LA, const PHash *da, const WsLayout *LB,
		  const PHash *db)
{
	ProllyDiffList dl = {0};
	Buf			del = {0},
				line = {0},
				keys = {0},
				kcols = {0},
				cols = {0},
				copy_data = {0};
	uint64_t	nkeys = 0,
				nlines = 0;
	Buf		   *defs = calloc((size_t) LB->nstored + 1, sizeof(Buf));
	bool	   *have = calloc((size_t) LB->nstored + 1, sizeof(bool));
	WsField		f[WS_MAX_COLS];

	(void) name;
	if (phash_eq(da, db))
		goto done;
	CHECK(prolly_diff_collect(mv->ctx, da, db, &dl) == PROLLY_OK);
	/* Deletes first, so rows that move between keys never collide. */
	for (size_t i = 0; i < dl.n; i++)
	{
		const ProllyDiff *x = &dl.v[i];
		long		times = 1;

		if (x->from == NULL)
			continue;
		if (LB->nkey > 0 && x->to &&
			same_in_db(mv, LA, LB, x->from, x->flen, x->to, x->tlen, defs, have))
		{
			mv->st->skipped++;
			continue;
		}
		if (LB->nkey == 0)
		{
			char		cnt[24];
			long		from,
						to = 0;

			snprintf(cnt, sizeof(cnt), "%.*s", (int) x->flen, (const char *) x->from);
			from = atol(cnt);
			if (x->to)
			{
				snprintf(cnt, sizeof(cnt), "%.*s", (int) x->tlen, (const char *) x->to);
				to = atol(cnt);
			}
			if (to >= from)
				continue;
			times = from - to;
		}
		ws_decode(LA, x->key, x->klen, x->from, x->flen, f);
		if (LB->nkey > 0)
		{
			/* the key, a line of COPY text for prolly_apply_delete */
			if (nkeys == 0 && kcols.len == 0)
				col_array(LB, LB->key, LB->nkey, &kcols);
			for (int k = 0; k < LB->nkey; k++)
			{
				if (k)
					buf_append(&keys, "\t", 1);
				if (f[k].null)
					buf_append(&keys, "\\N", 2);
				else
					ws_copy_escape(&keys, f[k].p, f[k].len);
			}
			buf_append(&keys, "\n", 1);
			if (++nkeys, keys.len >= APPLY_BATCH)
			{
				apply_rows(mv, "prolly_apply_delete", LB, &kcols, &keys, nkeys);
				nkeys = 0;
			}
		}
		else
		{
			/* A keyless row: that many of its copies, matched on what B stores. */
			char		lim[32];

			buf_append(&del, "DELETE FROM ONLY " SCHEMA ".", strlen("DELETE FROM ONLY " SCHEMA "."));
			buf_append(&del, LB->rel, strlen(LB->rel));
			buf_append(&del, " WHERE ctid IN (SELECT ctid FROM ONLY " SCHEMA ".",
					   strlen(" WHERE ctid IN (SELECT ctid FROM ONLY " SCHEMA "."));
			buf_append(&del, LB->rel, strlen(LB->rel));
			buf_append(&del, " WHERE true", 11);
			for (int k = 0; k < LA->nstored; k++)
			{
				const WsCol *col = ws_layout_col(LB, LA->copy[k]);

				if (col == NULL || col->kind != 's')
					continue;
				buf_append(&del, " AND ", 5);
				buf_append(&del, col->name, strlen(col->name));
				buf_append(&del, " IS NOT DISTINCT FROM ", 22);
				literal(mv->c, &del, &f[k], col->type);
			}
			snprintf(lim, sizeof(lim), " LIMIT %ld)", times);
			buf_append(&del, lim, strlen(lim));
			run_sql(mv, &del);
		}
		mv->st->deleted += (uint64_t) times;
	}
	apply_rows(mv, "prolly_apply_delete", LB, &kcols, &keys, nkeys);
	/* Then the new versions. */
	for (size_t i = 0; i < dl.n; i++)
	{
		const ProllyDiff *x = &dl.v[i];
		long		times = 1;

		if (x->to == NULL)
			continue;
		if (LB->nkey > 0 && x->from &&
			same_in_db(mv, LA, LB, x->from, x->flen, x->to, x->tlen, defs, have))
			continue;
		if (LB->nkey == 0)
		{
			char		cnt[24];
			long		from = 0,
						to;

			snprintf(cnt, sizeof(cnt), "%.*s", (int) x->tlen, (const char *) x->to);
			to = atol(cnt);
			if (x->from)
			{
				snprintf(cnt, sizeof(cnt), "%.*s", (int) x->flen, (const char *) x->from);
				from = atol(cnt);
			}
			if (to <= from)
				continue;
			times = to - from;
		}
		if (cols.len == 0)
			col_array(LB, LB->copy, LB->nstored, &cols);
		ws_decode(LB, x->key, x->klen, x->to, x->tlen, f);
		ws_copy_line(LB, f, &line);
		for (long q = 0; q < times; q++)
			buf_append(&copy_data, line.data, line.len);
		nlines += (uint64_t) times;
		if (copy_data.len >= APPLY_BATCH)
		{
			apply_rows(mv, "prolly_apply_insert", LB, &cols, &copy_data, nlines);
			nlines = 0;
		}
		if (x->from)
			mv->st->updated++;
		else
			mv->st->inserted += (uint64_t) times;
	}
	apply_rows(mv, "prolly_apply_insert", LB, &cols, &copy_data, nlines);
done:
	for (int i = 0; i <= LB->nstored; i++)
		if (defs[i].len != (size_t) -1)
			free(defs[i].data);
	free(defs);
	free(have);
	free(del.data);
	free(line.data);
	free(keys.data);
	free(kcols.data);
	free(cols.data);
	free(copy_data.data);
	prolly_diff_list_free(&dl);
}

/* All of a table's rows, for a table created or recreated. */
static void
load_rows(Move *mv, const WsLayout *LB, const PHash *db)
{
	PHash		e = t_empty(mv->ctx);
	WsLayout	none = *LB;

	(void) none;
	move_rows(mv, "", LB, &e, LB, db);
}

/* ---------------------------------------------------------------- all */

static double
since(double t0)
{
	return now_sec() - t0;
}

/* Objects of B the database lacks or has otherwise, created; extras dropped. */
static int
converge(Move *mv, const char *db)
{
	int			left = 0;

	for (int round = 0; round < 4; round++)
	{
		ObjList		cur;
		Obj		   *todo = NULL;
		size_t		n = 0,
					cap = 0;
		bool		any = false;

		collect_objects(mv->c, db, &cur);
		pg_cmd(mv->c, "BEGIN");
		pg_cmd(mv->c, "SELECT prolly.quiet()");
		for (size_t i = 0; i < cur.n; i++)
		{
			const ProllyKV *b = obj_findn(&mv->ob, cur.kv[i].key, cur.kv[i].klen);
			char		type[200],
						name[1000];

			if (cur.kv[i].key[0] == '0' || b)
				continue;
			key_parts(&cur.kv[i], type, sizeof(type), name, sizeof(name));
			if (strcmp(type, "TABLE") == 0)
				continue;		/* tables were handled; a stray one shows in the capture */
			{
				char	   *r = rendered((const char *) cur.kv[i].val, cur.kv[i].vlen, &mv->cb);
				char	   *sql = drop_sql(mv->c, type, name, r);

				exec_or_report(mv->c, sql, "drop", mv->rep);
				any |= sql && sql[0];
				mv->st->dropped++;
				free(sql);
				free(r);
			}
		}
		for (size_t i = 0; i < mv->ob.n; i++)
		{
			const ProllyKV *kv = &mv->ob.kv[i];
			const ProllyKV *have = obj_findn(&cur, kv->key, kv->klen);
			char		type[200],
						name[1000];

			if (kv->key[0] == '0' || kv_same(kv, have))
				continue;
			key_parts(kv, type, sizeof(type), name, sizeof(name));
			if (have && strcmp(type, "TABLE") != 0)
			{
				char	   *r = rendered((const char *) have->val, have->vlen, &mv->cb);
				char	   *sql = drop_sql(mv->c, type, name, r);

				exec_or_report(mv->c, sql, "drop", mv->rep);
				free(sql);
				free(r);
			}
			if (strcmp(type, "TABLE") == 0)
				continue;
			px_grow((void **) &todo, &cap, n + 1, sizeof(Obj));
			todo[n].key = strndup((const char *) kv->key, kv->klen);
			todo[n].ddl = rendered((const char *) kv->val, kv->vlen, &mv->cb);
			todo[n].tddl = strndup((const char *) kv->val, kv->vlen);
			todo[n].prio = obj_prio(todo[n].key);
			n++;
		}
		qsort(todo, n, sizeof(Obj), obj_cmp);
		{
			WsReport	r = {0};
			double		t0 = now_sec();

			run_objects(mv->c, todo, n, &r);
			for (size_t i = 0; i < n; i++)
				if (strstr(todo[i].key, "\tINDEX\t") || strstr(todo[i].key, "\tCONSTRAINT\t"))
					mv->st->built++;
			mv->st->create_secs += since(t0);
			left = (int) r.n;
			if (round == 3)
				for (size_t i = 0; i < r.n; i++)
					report_add(mv->rep, r.p[i].what, NULL);
			report_free(&r);
		}
		mv->st->created += (int) n;
		pg_cmd(mv->c, "SELECT prolly.unquiet()");
		pg_cmd(mv->c, "COMMIT");
		for (size_t i = 0; i < n; i++)
		{
			free(todo[i].key);
			free(todo[i].ddl);
			free(todo[i].tddl);
		}
		free(todo);
		objlist_free(&cur);
		if (n == 0 && !any)
			break;
	}
	return left;
}

int
ws_move(ProllyCtx *ctx, const PHash *from, const PHash *to, const char *db, WsMoveStats *st,
		WsReport *rep)
{
	Move		mv = {0};
	double		t0 = now_sec(),
				t1;
	TableList	la,
				lb;
	VcTable		vt;

	memset(st, 0, sizeof(*st));
	mv.ctx = ctx;
	mv.from = from;
	mv.to = to;
	mv.st = st;
	mv.rep = rep;
	mv.c = pg_open(db);
	colmap_from_tree(ctx, from, &mv.ca);
	colmap_from_tree(ctx, to, &mv.cb);
	load_objs(ctx, from, &mv.oa);
	load_objs(ctx, to, &mv.ob);
	table_list(ctx, from, &la);
	table_list(ctx, to, &lb);
	pg_cmd(mv.c, "SET session_replication_role = replica");

	/* 1. Enum values added, in a transaction of their own. */
	pg_cmd(mv.c, "BEGIN");
	pg_cmd(mv.c, "SELECT prolly.quiet()");
	for (size_t i = 0; i < mv.oa.n; i++)
	{
		const ProllyKV *a = &mv.oa.kv[i],
				   *b = obj_findn(&mv.ob, a->key, a->klen);
		char		type[200],
					name[1000];

		key_parts(a, type, sizeof(type), name, sizeof(name));
		if (strcmp(type, "TYPE") != 0 || b == NULL || kv_same(a, b))
			continue;
		{
			char	   *ta = strndup((const char *) a->val, a->vlen),
					   *tb = strndup((const char *) b->val, b->vlen);

			if (enum_add(mv.c, name, ta, tb, rep))
			{
				/* Now the same as B's: not dropped below. */
				free((void *) a->val);
				mv.oa.kv[i].val = (const uint8_t *) strndup((const char *) b->val, b->vlen);
				mv.oa.kv[i].vlen = b->vlen;
			}
			free(ta);
			free(tb);
		}
	}

	pg_cmd(mv.c, "SELECT prolly.unquiet()");
	pg_cmd(mv.c, "COMMIT");

	pg_cmd(mv.c, "BEGIN");
	{
		const ProllyKV *pre = obj_find(&mv.ob, "0\tprelude\t");

		if (pre)
		{
			char	   *p = strndup((const char *) pre->val, pre->vlen);

			pg_cmd(mv.c, p);
			free(p);
		}
	}
	pg_cmd(mv.c, SESSION_SETTINGS);
	pg_cmd(mv.c, "SET session_replication_role = replica");
	pg_cmd(mv.c, "SELECT prolly.quiet()");

	/* 2. What B lacks or changed, dropped; a dropped type takes its tables. */
	for (int sec = 'b'; sec >= 'a'; sec--)
		for (size_t i = mv.oa.n; i-- > 0;)
		{
			const ProllyKV *a = &mv.oa.kv[i],
					   *b = obj_findn(&mv.ob, a->key, a->klen);
			char		type[200],
						name[1000];

			if (a->key[0] != sec || kv_same(a, b))
				continue;
			key_parts(a, type, sizeof(type), name, sizeof(name));
			if (strcmp(type, "TABLE") == 0)
				continue;
			{
				char	   *r = rendered((const char *) a->val, a->vlen, &mv.ca);
				char	   *sql;
				char	   *on = first_rel(r);
				char		tkey[1100];

				/* What belongs to a table B drops goes with the table. */
				if (on)
				{
					snprintf(tkey, sizeof(tkey), "a\tTABLE\t%s", on[0] == '"' ? on : on);
					if (obj_find(&mv.oa, tkey) && !obj_find(&mv.ob, tkey))
					{
						free(on);
						free(r);
						continue;
					}
					free(on);
				}
				sql = drop_sql(mv.c, type, name, r);

				if (sql == NULL)
				{
					char		what[1300];

					snprintf(what, sizeof(what), "cannot drop %s %s", type, name);
					report_add(rep, what, NULL);
				}
				if (getenv("MOVE_TRACE"))
					fprintf(stderr, "move: drop %s %s (in B: %s): %s\n", type, name, b ? "changed" : "absent", sql ? sql : "?");
				exec_or_report(mv.c, sql, "drop", rep);
				st->dropped++;
				if (strcmp(type, "TYPE") == 0 || strcmp(type, "DOMAIN") == 0)
				{
					char		ref[1100];

					snprintf(ref, sizeof(ref), SCHEMA ".%s", name);
					for (size_t t = 0; t < la.n; t++)
					{
						WsLayout	L;

						if (la.names[t][0] == '~' || !table_layout(ctx, from, la.names[t], &L, NULL))
							continue;
						for (int q = 0; q < L.ncols; q++)
							if (strstr(L.cols[q].type, ref))
								mt_get(&mv, la.names[t])->recreate = true;
						ws_layout_free(&L);
					}
				}
				free(sql);
				free(r);
			}
		}

	/*
	 * What tables are built from (types, domains, collations, functions,
	 * sequences) that B adds or changed, before the tables.
	 */
	{
		Obj		   *pre = NULL;
		size_t		n = 0,
					cap = 0;
		int			tprio = obj_prio("a\tTABLE\tx");

		for (size_t i = 0; i < mv.ob.n; i++)
		{
			const ProllyKV *b = &mv.ob.kv[i],
					   *a = obj_findn(&mv.oa, b->key, b->klen);
			char	   *key = strndup((const char *) b->key, b->klen);

			if (b->key[0] != 'a' || kv_same(a, b) || obj_prio(key) >= tprio)
			{
				free(key);
				continue;
			}
			px_grow((void **) &pre, &cap, n + 1, sizeof(Obj));
			pre[n].key = key;
			pre[n].ddl = rendered((const char *) b->val, b->vlen, &mv.cb);
			pre[n].tddl = strndup((const char *) b->val, b->vlen);
			pre[n].prio = obj_prio(key);
			n++;
		}
		qsort(pre, n, sizeof(Obj), obj_cmp);
		run_objects(mv.c, pre, n, rep);
		st->created += (int) n;
		for (size_t i = 0; i < n; i++)
		{
			free(pre[i].key);
			free(pre[i].ddl);
			free(pre[i].tddl);
		}
		free(pre);
	}

	/* 3. Tables: renames, then each table dropped, created, altered or recreated. */
	rename_columns(&mv);
	for (int pass = 0; pass < 2; pass++)
	{
		const ObjList *l = pass == 0 ? &mv.oa : &mv.ob;

		for (size_t i = 0; i < l->n; i++)
		{
			const ProllyKV *kv = &l->kv[i];
			const ProllyKV *a,
					   *b;
			char		type[200],
						name[1000];
			char		key[1100];
			MoveTable  *t;
			char	   *qt;
			char		sql[1400];

			key_parts(kv, type, sizeof(type), name, sizeof(name));
			if (strcmp(type, "TABLE") != 0)
				continue;
			snprintf(key, sizeof(key), "a\tTABLE\t%s", name);
			a = obj_find(&mv.oa, key);
			b = obj_find(&mv.ob, key);
			if (pass == 1 && a)
				continue;		/* done in the first pass */
			t = mt_get(&mv, name);
			qt = qid(mv.c, name);
			if (b == NULL)
			{
				snprintf(sql, sizeof(sql), "DROP TABLE IF EXISTS " SCHEMA ".%s CASCADE", qt);
				exec_or_report(mv.c, sql, "drop table", rep);
				st->dropped++;
				free(qt);
				continue;
			}
			if (a && !t->recreate && !kv_same(a, b))
			{
				WsLayout	LA,
							LB;
				bool		ha = table_layout(ctx, from, name, &LA, NULL),
							hb = table_layout(ctx, to, name, &LB, NULL);
				char	   *ta = strndup((const char *) a->val, a->vlen),
						   *tb = strndup((const char *) b->val, b->vlen);

				pg_cmd(mv.c, "SAVEPOINT alter_table");
				if (!alter_table(&mv, name, ta, tb, ha ? &LA : NULL, hb ? &LB : NULL))
				{
					pg_cmd(mv.c, "ROLLBACK TO SAVEPOINT alter_table");
					t->recreate = true;
					if (getenv("MOVE_TRACE"))
						fprintf(stderr, "move: recreate %s:\n--- A\n%s\n--- B\n%s\n", name, ta, tb);
				}
				else
					pg_cmd(mv.c, "RELEASE SAVEPOINT alter_table");
				if (ha)
					ws_layout_free(&LA);
				if (hb)
					ws_layout_free(&LB);
				free(ta);
				free(tb);
			}
			if (a == NULL || t->recreate)
			{
				char	   *r = rendered((const char *) b->val, b->vlen, &mv.cb);

				if (a)
				{
					snprintf(sql, sizeof(sql), "DROP TABLE IF EXISTS " SCHEMA ".%s CASCADE", qt);
					exec_or_report(mv.c, sql, "drop table", rep);
					st->recreated++;
				}
				exec_or_report(mv.c, r, "create table", rep);
				t->fresh = true;
				free(r);
			}
			free(qt);
		}
	}
	/* Tables a CASCADE took (partitions of a recreated parent) come back whole. */
	for (size_t i = 0; i < mv.ob.n; i++)
	{
		char		type[200],
					name[1000];
		char	   *qt;

		key_parts(&mv.ob.kv[i], type, sizeof(type), name, sizeof(name));
		if (strcmp(type, "TABLE") != 0)
			continue;
		qt = qid(mv.c, name);
		if (!rel_exists(mv.c, qt))
		{
			char	   *r = rendered((const char *) mv.ob.kv[i].val, mv.ob.kv[i].vlen, &mv.cb);

			exec_or_report(mv.c, r, "create table", rep);
			mt_get(&mv, name)->fresh = true;
			st->recreated++;
			free(r);
		}
		free(qt);
	}
	st->schema_secs = since(t0);

	/* 4. Tags as B has them. */
	pg_cmd(mv.c, "DELETE FROM prolly.tags");
	restore_tags(mv.c, &mv.cb);

	/* 5. Rows. */
	t1 = now_sec();
	for (size_t j = 0; j < lb.n; j++)
	{
		WsLayout	LA,
					LB;
		PHash		da,
					dbh;
		MoveTable  *t;

		if (lb.names[j][0] == '~')
			continue;
		CHECK(table_layout(ctx, to, lb.names[j], &LB, &dbh));
		t = mt_get(&mv, lb.names[j]);
		if (t->fresh || !table_layout(ctx, from, lb.names[j], &LA, &da))
			load_rows(&mv, &LB, &dbh);
		else
		{
			move_rows(&mv, lb.names[j], &LA, &da, &LB, &dbh);
			ws_layout_free(&LA);
		}
		ws_layout_free(&LB);
	}
	st->rows_secs = since(t1);

	/* 6. What needed the rows; sequences. */
	for (char *p = (char *) mv.later.data, *e; p && p < (char *) mv.later.data + mv.later.len; p = e + 2)
	{
		e = strstr(p, "\n\x01");
		*e = '\0';
		exec_or_report(mv.c, p, "constraint after rows", rep);
	}
	CHECK(vc_table_get(ctx, to, "~seq", &vt));
	restore_sequences(ctx, mv.c, &vt.data);
	pg_cmd(mv.c, "DELETE FROM prolly.violations");
	if (vc_table_get(ctx, to, "~viol", &vt))
		restore_violations(ctx, mv.c, &vt.data);
	pg_cmd(mv.c, "SELECT prolly.unquiet()");
	pg_cmd(mv.c, "COMMIT");

	/* 7. Every other object as B has it. */
	converge(&mv, db);

	/* 8. Materialized views from the rows now there. */
	t1 = now_sec();
	pg_cmd(mv.c, "BEGIN");
	pg_cmd(mv.c, "SELECT prolly.quiet()");
	{
		PGresult   *res = pg_exec(mv.c,
								  "SELECT format('REFRESH MATERIALIZED VIEW %s', c.oid::regclass) FROM pg_class c "
								  "JOIN pg_namespace n ON n.oid = c.relnamespace "
								  "WHERE n.nspname = '" SCHEMA "' AND c.relkind = 'm' AND c.relispopulated "
								  "ORDER BY c.oid");

		for (int i = 0; i < PQntuples(res); i++)
			exec_or_report(mv.c, PQgetvalue(res, i, 0), "refresh", NULL);
		PQclear(res);
	}
	pg_cmd(mv.c, "SELECT prolly.unquiet()");
	pg_cmd(mv.c, "COMMIT");
	st->refresh_secs = since(t1);
	st->secs = since(t0);

	PQfinish(mv.c);
	colmap_free(&mv.ca);
	colmap_free(&mv.cb);
	objlist_free(&mv.oa);
	objlist_free(&mv.ob);
	table_list_free(&la);
	table_list_free(&lb);
	for (size_t i = 0; i < mv.nt; i++)
		free(mv.t[i].name);
	free(mv.t);
	free(mv.later.data);
	return rep ? (int) rep->n : 0;
}
