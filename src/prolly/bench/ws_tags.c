/*
 * ws_tags.c
 *	  Column references by tag in the schema's objects.
 *
 * An object's DDL is stored with each reference to a column replaced by
 * @{<tag>}, and is rendered back with the column's name in the commit
 * being restored.  So an index, view, constraint or trigger made on one
 * branch follows a column the other branch renamed.
 *
 * pg_dump writes relations schema-qualified (rt.<table>), so the tables an
 * object names are known.  An identifier is a column reference when it
 * names a column of such a table and is not:
 *	- a function (followed by "("), a qualifier (followed by "."), a type
 *	  (after "::"), a field of a composite value (after ")."), or a name
 *	  that follows a keyword introducing one (AS, INDEX, CONSTRAINT, ...);
 *	- inside a string, a dollar-quoted body or a comment.
 * A qualified reference (table.column) belongs to that table; an
 * unqualified one to the nearest table named before it that has such a
 * column, or else to the one table named anywhere that has it.  Renders
 * of what is captured always give back the captured text, so getting a
 * reference wrong costs only a rename's reach, never a rebuild.
 *
 * The commit's columns are the ~cols entry: tag -> "<table>\t<column>",
 * both quoted, for every column of every table (partitioned ones too).
 */
#define _GNU_SOURCE
#include <strings.h>

#include "ws.h"

void
colmap_free(ColMap *m)
{
	for (size_t i = 0; i < m->n; i++)
	{
		free(m->c[i].table);
		free(m->c[i].name);
	}
	free(m->c);
	memset(m, 0, sizeof(*m));
}

static void
colmap_add(ColMap *m, const char *table, size_t tl, const char *name, size_t nl, uint64_t tag)
{
	px_grow((void **) &m->c, &m->cap, m->n + 1, sizeof(ColRef));
	m->c[m->n].table = strndup(table, tl);
	m->c[m->n].name = strndup(name, nl);
	m->c[m->n].tag = tag;
	m->n++;
}

void
colmap_from_db(PGconn *c, ColMap *m)
{
	PGresult   *res = pg_exec(c,
							  "SELECT quote_ident(cl.relname), quote_ident(a.attname), g.tag "
							  "FROM prolly.tags g "
							  "JOIN pg_attribute a ON a.attrelid = g.relid AND a.attnum = g.attnum "
							  "JOIN pg_class cl ON cl.oid = g.relid "
							  "JOIN pg_namespace ns ON ns.oid = cl.relnamespace "
							  "WHERE ns.nspname = '" SCHEMA "' AND NOT a.attisdropped "
							  "ORDER BY g.tag");

	memset(m, 0, sizeof(*m));
	for (int i = 0; i < PQntuples(res); i++)
	{
		const char *t = PQgetvalue(res, i, 0),
				   *n = PQgetvalue(res, i, 1);

		if (i > 0 && strcmp(PQgetvalue(res, i - 1, 2), PQgetvalue(res, i, 2)) == 0)
			die("two columns share tag %s", PQgetvalue(res, i, 2));
		colmap_add(m, t, strlen(t), n, strlen(n), strtoull(PQgetvalue(res, i, 2), NULL, 10));
	}
	PQclear(res);
}

/* The ~cols map from a database's tags; keys are tags as decimal text. */
PHash
colmap_build(ProllyCtx *ctx, const ColMap *m)
{
	ProllyKV   *kv = px_malloc((m->n ? m->n : 1) * sizeof(ProllyKV));
	char	  **keys = px_malloc((m->n ? m->n : 1) * sizeof(char *));
	Buf		   *vals = calloc(m->n ? m->n : 1, sizeof(Buf));
	PHash		out;

	for (size_t i = 0; i < m->n; i++)
	{
		keys[i] = px_malloc(24);
		snprintf(keys[i], 24, "%" PRIu64, m->c[i].tag);
		buf_append(&vals[i], m->c[i].table, strlen(m->c[i].table));
		buf_append(&vals[i], "\t", 1);
		buf_append(&vals[i], m->c[i].name, strlen(m->c[i].name));
	}
	for (size_t i = 0; i < m->n; i++)
	{
		kv[i].key = (const uint8_t *) keys[i];
		kv[i].klen = strlen(keys[i]);
		kv[i].val = vals[i].data;
		kv[i].vlen = vals[i].len;
	}
	/* Decimal text keys sort as text. */
	for (size_t i = 1; i < m->n; i++)
		for (size_t j = i; j > 0 && key_cmp(kv[j - 1].key, kv[j - 1].klen, kv[j].key, kv[j].klen) > 0; j--)
		{
			ProllyKV	x = kv[j];

			kv[j] = kv[j - 1];
			kv[j - 1] = x;
		}
	out = t_build(ctx, kv, m->n);
	for (size_t i = 0; i < m->n; i++)
	{
		free(keys[i]);
		free(vals[i].data);
	}
	free(keys);
	free(vals);
	free(kv);
	return out;
}

void
colmap_from_tree(ProllyCtx *ctx, const PHash *rv, ColMap *m)
{
	VcTable		t;
	ProllyIter *it;
	const uint8_t *k,
			   *v;
	size_t		kl,
				vl;

	memset(m, 0, sizeof(*m));
	if (!vc_table_get(ctx, rv, "~cols", &t))
		return;
	it = t_iter(ctx, &t.data, NULL, 0);
	while (t_next(it, &k, &kl, &v, &vl))
	{
		const uint8_t *tab = memchr(v, '\t', vl);
		char		tag[24];

		CHECK(tab != NULL && kl < sizeof(tag));
		memcpy(tag, k, kl);
		tag[kl] = '\0';
		colmap_add(m, (const char *) v, (size_t) (tab - v), (const char *) tab + 1,
				   vl - (size_t) (tab - v) - 1, strtoull(tag, NULL, 10));
	}
	prolly_iter_close(it);
}

static const ColRef *
col_of(const ColMap *m, const char *table, size_t tl, const char *name, size_t nl)
{
	for (size_t i = 0; i < m->n; i++)
		if (strlen(m->c[i].table) == tl && strncmp(m->c[i].table, table, tl) == 0 &&
			strlen(m->c[i].name) == nl && strncmp(m->c[i].name, name, nl) == 0)
			return &m->c[i];
	return NULL;
}

static bool
is_table(const ColMap *m, const char *t, size_t tl)
{
	for (size_t i = 0; i < m->n; i++)
		if (strlen(m->c[i].table) == tl && strncmp(m->c[i].table, t, tl) == 0)
			return true;
	return false;
}

/* ------------------------------------------------------------ the lexer */

typedef struct Tok
{
	size_t		off,
				len;
	bool		ident;
} Tok;

static size_t
skip_quoted(const char *s, size_t n, size_t i, char q)
{
	for (i++; i < n; i++)
		if (s[i] == q)
		{
			if (i + 1 < n && s[i + 1] == q)
				i++;
			else
				return i + 1;
		}
	return n;
}

/* The tokens of s: identifiers and everything else between them. */
static Tok *
lex(const char *s, size_t n, size_t *ntok)
{
	Tok		   *t = NULL;
	size_t		cap = 0,
				nt = 0;

	for (size_t i = 0; i < n;)
	{
		size_t		start = i;
		bool		ident = false;
		char		c = s[i];

		if (c == '-' && i + 1 < n && s[i + 1] == '-')
		{
			while (i < n && s[i] != '\n')
				i++;
		}
		else if (c == '/' && i + 1 < n && s[i + 1] == '*')
		{
			const char *e = strstr(s + i + 2, "*/");

			i = e ? (size_t) (e - s) + 2 : n;
		}
		else if (c == '\'')
		{
			bool		esc = i > 0 && (s[i - 1] == 'E' || s[i - 1] == 'e');

			if (esc)
			{
				for (i++; i < n && s[i] != '\''; i++)
					if (s[i] == '\\')
						i++;
				i = i < n ? i + 1 : n;
			}
			else
				i = skip_quoted(s, n, i, '\'');
		}
		else if (c == '$' && i + 1 < n && (s[i + 1] == '$' || isalpha((unsigned char) s[i + 1]) || s[i + 1] == '_'))
		{
			size_t		j = i + 1;

			while (j < n && (isalnum((unsigned char) s[j]) || s[j] == '_'))
				j++;
			if (j < n && s[j] == '$')
			{
				size_t		dl = j - i + 1;
				const char *e = NULL;

				for (size_t q = j + 1; q + dl <= n; q++)
					if (strncmp(s + q, s + i, dl) == 0)
					{
						e = s + q;
						break;
					}
				i = e ? (size_t) (e - s) + dl : n;
			}
			else
				i++;
		}
		else if (c == '"')
		{
			i = skip_quoted(s, n, i, '"');
			ident = true;
		}
		else if (isalpha((unsigned char) c) || c == '_' || (c == '@' && i + 1 < n && s[i + 1] == '{'))
		{
			if (c == '@')
			{
				while (i < n && s[i] != '}')
					i++;
				i = i < n ? i + 1 : n;
			}
			else
			{
				while (i < n && (isalnum((unsigned char) s[i]) || s[i] == '_' || s[i] == '$'))
					i++;
				/* E'...' starts a string, not an identifier. */
				if (i - start == 1 && (c == 'E' || c == 'e') && i < n && s[i] == '\'')
					continue;
				ident = true;
			}
		}
		else
			i++;
		if (nt > 0 && !ident && !t[nt - 1].ident)
		{
			t[nt - 1].len = i - t[nt - 1].off;
			continue;
		}
		px_grow((void **) &t, &cap, nt + 1, sizeof(Tok));
		t[nt].off = start;
		t[nt].len = i - start;
		t[nt].ident = ident;
		nt++;
	}
	*ntok = nt;
	return t;
}

/* The non-identifier text between tokens, trimmed of spaces. */
static void
gap(const char *s, const Tok *t, const char **p, size_t *len)
{
	size_t		a = t->off,
				b = t->off + t->len;

	while (a < b && isspace((unsigned char) s[a]))
		a++;
	while (b > a && isspace((unsigned char) s[b - 1]))
		b--;
	*p = s + a;
	*len = b - a;
}

static bool
kw_eq(const char *s, size_t len, const char *kw)
{
	return strlen(kw) == len && strncasecmp(s, kw, len) == 0;
}

/* Keywords after which an identifier names something other than a column. */
static bool
names_other(const char *s, size_t len)
{
	static const char *const kw[] = {
		"AS", "COLLATE", "INDEX", "CONSTRAINT", "VIEW", "TABLE", "SEQUENCE", "TRIGGER",
		"POLICY", "STATISTICS", "FUNCTION", "PROCEDURE", "TYPE", "SCHEMA", "RULE",
		"EXTENSION", "DOMAIN", "TO", "OPERATOR", "CLASS", "FAMILY", "USING", "SERVER",
		"OWNER", "ROLE", "EXECUTE", NULL
	};

	for (int i = 0; kw[i]; i++)
		if (kw_eq(s, len, kw[i]))
			return true;
	return false;
}

/*
 * The DDL with its column references as @{tag}.  Identifiers in the map
 * are quoted as quote_ident quotes them, which is how pg_dump writes them.
 */
void
templ(const char *s, size_t n, const ColMap *m, Buf *out)
{
	size_t		nt;
	Tok		   *t = lex(s, n, &nt);
	size_t	   *ment = px_malloc((nt ? nt : 1) * sizeof(size_t));	/* token index of rt.<table> */
	size_t		nm = 0;

	/* Every table the text names, by where. */
	for (size_t i = 2; i < nt; i++)
		if (t[i].ident && t[i - 2].ident && t[i - 2].len == strlen(SCHEMA) &&
			strncmp(s + t[i - 2].off, SCHEMA, t[i - 2].len) == 0)
		{
			const char *g;
			size_t		gl;

			gap(s, &t[i - 1], &g, &gl);
			if (gl == 1 && *g == '.' && is_table(m, s + t[i].off, t[i].len))
				ment[nm++] = i;
		}
	out->len = 0;
	for (size_t i = 0; i < nt; i++)
	{
		const char *x = s + t[i].off,
				   *g;
		size_t		xl = t[i].len,
					gl;
		const ColRef *ref = NULL;

		if (!t[i].ident || x[0] == '@')
		{
			buf_append(out, x, xl);
			continue;
		}
		if (i + 1 < nt)
		{
			gap(s, &t[i + 1], &g, &gl);
			if (gl > 0 && (g[0] == '(' || g[0] == '.'))
			{
				buf_append(out, x, xl);
				continue;
			}
		}
		/* Identifiers are always separated by a token of other text. */
		if (i > 0)
		{
			gap(s, &t[i - 1], &g, &gl);
			if (gl >= 2 && (strncmp(g + gl - 2, "::", 2) == 0 || strncmp(g + gl - 2, ").", 2) == 0))
				goto keep;
			if (gl == 1 && g[0] == '.' && i >= 2 && t[i - 2].ident)
			{
				const char *p = s + t[i - 2].off;
				size_t		pl = t[i - 2].len;

				if (pl == strlen(SCHEMA) && strncmp(p, SCHEMA, pl) == 0)
					goto keep;	/* a relation */
				if (is_table(m, p, pl))
				{
					ref = col_of(m, p, pl, x, xl);
					goto done;
				}
				/* An alias: resolved as an unqualified name is. */
			}
			else if (gl == 0 && i >= 2 && t[i - 2].ident &&
					 names_other(s + t[i - 2].off, t[i - 2].len))
				goto keep;
		}
		/* The nearest table named before, else the only one named at all. */
		for (size_t q = nm; q > 0 && ref == NULL; q--)
			if (ment[q - 1] < i)
				ref = col_of(m, s + t[ment[q - 1]].off, t[ment[q - 1]].len, x, xl);
		if (ref == NULL)
		{
			const ColRef *only = NULL;
			int			hits = 0;

			for (size_t q = 0; q < nm; q++)
			{
				const ColRef *r = col_of(m, s + t[ment[q]].off, t[ment[q]].len, x, xl);

				if (r && r != only)
				{
					only = r;
					hits++;
				}
			}
			if (hits == 1)
				ref = only;
		}
done:
		if (ref)
		{
			char		ph[32];
			int			pl = snprintf(ph, sizeof(ph), "@{%" PRIu64 "}", ref->tag);

			buf_append(out, ph, (size_t) pl);
			continue;
		}
keep:
		buf_append(out, x, xl);
	}
	free(ment);
	free(t);
}

/* The DDL with each @{tag} as its column's name; false if a tag is unknown. */
bool
render(const char *s, size_t n, const ColMap *m, Buf *out, uint64_t *missing)
{
	out->len = 0;
	for (size_t i = 0; i < n;)
	{
		const char *at = memmem(s + i, n - i, "@{", 2);
		uint64_t	tag;
		const ColRef *ref = NULL;
		char	   *e;

		if (at == NULL)
		{
			buf_append(out, s + i, n - i);
			break;
		}
		buf_append(out, s + i, (size_t) (at - s - (ptrdiff_t) i));
		tag = strtoull(at + 2, &e, 10);
		if (*e != '}')
		{
			buf_append(out, "@{", 2);
			i = (size_t) (at - s) + 2;
			continue;
		}
		for (size_t q = 0; q < m->n && ref == NULL; q++)
			if (m->c[q].tag == tag)
				ref = &m->c[q];
		if (ref == NULL)
		{
			if (missing)
				*missing = tag;
			return false;
		}
		buf_append(out, ref->name, strlen(ref->name));
		i = (size_t) (e + 1 - s);
	}
	return true;
}
