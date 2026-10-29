/*
 * ws_codec.c
 *	  Table layouts and the row codec.
 *
 * A layout (a table's schema chunk) names the table, its primary key and
 * its columns by tag:
 *
 *	rel <quoted name>
 *	key <tag>,<tag>...				the primary key, in its order
 *	col <tag> <kind> <pos> <type> <quoted name> <default>	(tab separated)
 *
 * one col line per column in attribute order; kind is s (stored) or g
 * (generated, never stored); pos is the column's place among the stored
 * columns, which is where a replication stream sends it (-1 if generated).
 *
 * A row is keyed by its primary key's values, each length-prefixed; its
 * value holds the other stored columns that are not NULL as (tag, length,
 * bytes) cells sorted by tag.  NULLs take no space and columns are found by
 * tag, so renaming a column, adding a nullable column without a default,
 * or dropping one that is all NULL leaves every row as it was.  A table
 * without a primary key keys each row by all its cells (after the byte
 * 'r') and counts it.  Values are each type's output text.
 */
#include "ws.h"

static char *
dup_span(const char *p, size_t n)
{
	char	   *s = px_malloc(n + 1);

	memcpy(s, p, n);
	s[n] = '\0';
	return s;
}

static int
tag_cmp(const void *a, const void *b)
{
	uint64_t	x = *(const uint64_t *) a,
				y = *(const uint64_t *) b;

	return (x > y) - (x < y);
}

bool
ws_layout_parse(const char *text, size_t len, WsLayout *L)
{
	const char *p = text,
			   *end = text + len;
	size_t		ccap = 0,
				kcap = 0;

	memset(L, 0, sizeof(*L));
	while (p < end)
	{
		const char *nl = memchr(p, '\n', (size_t) (end - p));
		size_t		ll = nl ? (size_t) (nl - p) : (size_t) (end - p);

		if (ll > 4 && strncmp(p, "rel ", 4) == 0)
			L->rel = dup_span(p + 4, ll - 4);
		else if (ll >= 4 && strncmp(p, "key ", 4) == 0)
		{
			const char *q = p + 4,
					   *le = p + ll;

			while (q < le)
			{
				char	   *e;

				px_grow((void **) &L->key, &kcap, (size_t) L->nkey + 1, sizeof(uint64_t));
				L->key[L->nkey++] = strtoull(q, &e, 10);
				if (e == q)
					return false;
				q = e + (e < le && *e == ',');
			}
		}
		else if (ll > 4 && strncmp(p, "col ", 4) == 0)
		{
			const char *f[6];
			size_t		fl[6];
			const char *q = p + 4,
					   *le = p + ll;
			WsCol	   *c;

			for (int i = 0; i < 6; i++)
			{
				const char *t = i < 5 ? memchr(q, '\t', (size_t) (le - q)) : NULL;

				if (i < 5 && t == NULL)
					return false;
				f[i] = q;
				fl[i] = (size_t) ((t ? t : le) - q);
				q = t ? t + 1 : le;
			}
			px_grow((void **) &L->cols, &ccap, (size_t) L->ncols + 1, sizeof(WsCol));
			c = &L->cols[L->ncols++];
			if (fl[0] == 0)
				return false;	/* a column with no tag */
			c->tag = strtoull(f[0], NULL, 10);
			c->kind = f[1][0];
			c->pos = atoi(f[2]);
			c->type = dup_span(f[3], fl[3]);
			c->name = dup_span(f[4], fl[4]);
			c->def = dup_span(f[5], fl[5]);
		}
		else if (ll > 0)
			return false;
		p += ll + 1;
	}
	if (L->rel == NULL)
		return false;

	/* COPY order: the key, then the other stored columns by attribute. */
	L->copy = px_malloc((size_t) (L->ncols ? L->ncols : 1) * sizeof(uint64_t));
	L->stream = px_malloc((size_t) (L->ncols ? L->ncols : 1) * sizeof(int));
	for (int i = 0; i < L->nkey; i++)
		L->copy[L->nstored++] = L->key[i];
	for (int i = 0; i < L->ncols; i++)
	{
		bool		inkey = false;

		for (int k = 0; k < L->nkey; k++)
			inkey |= L->key[k] == L->cols[i].tag;
		if (L->cols[i].kind == 's' && !inkey)
			L->copy[L->nstored++] = L->cols[i].tag;
	}
	for (int i = 0; i < L->ncols; i++)
		if (L->cols[i].kind == 's')
		{
			if (L->cols[i].pos < 0 || L->cols[i].pos >= L->nstored)
				return false;
			L->stream[L->cols[i].pos] = ws_copy_index(L, L->cols[i].tag);
		}
	return true;
}

void
ws_layout_free(WsLayout *L)
{
	for (int i = 0; i < L->ncols; i++)
	{
		free(L->cols[i].type);
		free(L->cols[i].name);
		free(L->cols[i].def);
	}
	free(L->cols);
	free(L->key);
	free(L->copy);
	free(L->stream);
	free(L->rel);
	memset(L, 0, sizeof(*L));
}

/* The layout as the layout query writes it. */
void
ws_layout_write(const WsLayout *L, Buf *out)
{
	char		num[32];

	buf_append(out, "rel ", 4);
	buf_append(out, L->rel, strlen(L->rel));
	buf_append(out, "\nkey ", 5);
	for (int i = 0; i < L->nkey; i++)
	{
		int			n = snprintf(num, sizeof(num), "%s%" PRIu64, i ? "," : "", L->key[i]);

		buf_append(out, num, (size_t) n);
	}
	buf_append(out, "\n", 1);
	for (int i = 0; i < L->ncols; i++)
	{
		const WsCol *c = &L->cols[i];
		int			n = snprintf(num, sizeof(num), "col %" PRIu64 "\t%c\t", c->tag, c->kind);

		buf_append(out, num, (size_t) n);
		n = snprintf(num, sizeof(num), "%d\t", c->pos);
		buf_append(out, num, (size_t) n);
		buf_append(out, c->type, strlen(c->type));
		buf_append(out, "\t", 1);
		buf_append(out, c->name, strlen(c->name));
		buf_append(out, "\t", 1);
		buf_append(out, c->def, strlen(c->def));
		buf_append(out, "\n", 1);
	}
}

const WsCol *
ws_layout_col(const WsLayout *L, uint64_t tag)
{
	for (int i = 0; i < L->ncols; i++)
		if (L->cols[i].tag == tag)
			return &L->cols[i];
	return NULL;
}

const WsCol *
ws_layout_col_named(const WsLayout *L, const char *quoted)
{
	for (int i = 0; i < L->ncols; i++)
		if (strcmp(L->cols[i].name, quoted) == 0)
			return &L->cols[i];
	return NULL;
}

int
ws_copy_index(const WsLayout *L, uint64_t tag)
{
	for (int i = 0; i < L->nstored; i++)
		if (L->copy[i] == tag)
			return i;
	return -1;
}

/* The stored columns' quoted names in COPY order. */
void
ws_copy_list(const WsLayout *L, Buf *out)
{
	for (int i = 0; i < L->nstored; i++)
	{
		const WsCol *c = ws_layout_col(L, L->copy[i]);

		if (i)
			buf_append(out, ", ", 2);
		buf_append(out, c->name, strlen(c->name));
	}
}

/* ----------------------------------------------------------------- rows */

static void
put_cells(const WsLayout *L, const WsField *f, int from, Buf *out)
{
	WsCell		cells[WS_MAX_COLS];
	int			n = 0;

	for (int i = from; i < L->nstored; i++)
		if (!f[i].null)
		{
			cells[n].tag = L->copy[i];
			cells[n].p = f[i].p;
			cells[n].len = f[i].len;
			n++;
		}
	ws_cells_encode(cells, n, out);
}

static int
cell_cmp(const void *a, const void *b)
{
	return tag_cmp(&((const WsCell *) a)->tag, &((const WsCell *) b)->tag);
}

void
ws_cells_encode(WsCell *c, int n, Buf *out)
{
	qsort(c, (size_t) n, sizeof(WsCell), cell_cmp);
	for (int i = 0; i < n; i++)
	{
		buf_append_uvarint(out, c[i].tag);
		buf_append_uvarint(out, c[i].len);
		buf_append(out, c[i].p, c[i].len);
	}
}

int
ws_cells(const uint8_t *v, size_t vl, WsCell *out, int max)
{
	const uint8_t *p = v,
			   *end = v + vl;
	int			n = 0;

	while (p < end)
	{
		uint64_t	tag = 0,
					len = 0;

		if (n == max || !read_uvarint(&p, end, &tag) || !read_uvarint(&p, end, &len) ||
			len > (uint64_t) (end - p))
			die("%s", "a malformed row");
		out[n].tag = tag;
		out[n].p = p;
		out[n].len = (size_t) len;
		p += len;
		n++;
	}
	return n;
}

bool
ws_cell_get(const uint8_t *v, size_t vl, uint64_t tag, const uint8_t **p, size_t *len)
{
	WsCell		c[WS_MAX_COLS];
	int			n = ws_cells(v, vl, c, WS_MAX_COLS);

	for (int i = 0; i < n; i++)
		if (c[i].tag == tag)
		{
			*p = c[i].p;
			*len = c[i].len;
			return true;
		}
	return false;
}

/* A row's key and value from its stored fields in COPY order. */
void
ws_encode(const WsLayout *L, const WsField *f, Buf *key, Buf *val)
{
	key->len = 0;
	val->len = 0;
	if (L->nkey == 0)
	{
		buf_append(key, "r", 1);
		put_cells(L, f, 0, key);
		return;
	}
	for (int i = 0; i < L->nkey; i++)
	{
		if (f[i].null)
			die("a NULL in the primary key of %s", L->rel);
		buf_append_uvarint(key, f[i].len);
		buf_append(key, f[i].p, f[i].len);
	}
	put_cells(L, f, L->nkey, val);
}

/* A row's stored fields in COPY order from its key and value. */
void
ws_decode(const WsLayout *L, const uint8_t *k, size_t kl, const uint8_t *v, size_t vl,
		  WsField *f)
{
	WsCell		c[WS_MAX_COLS];
	int			n;

	for (int i = 0; i < L->nstored; i++)
		f[i].null = true;
	if (L->nkey == 0)
	{
		if (kl == 0 || k[0] != 'r')
			die("a malformed keyless row in %s", L->rel);
		n = ws_cells(k + 1, kl - 1, c, WS_MAX_COLS);
	}
	else
	{
		const uint8_t *p = k,
				   *end = k + kl;

		for (int i = 0; i < L->nkey; i++)
		{
			uint64_t	len = 0;

			if (!read_uvarint(&p, end, &len) || len > (uint64_t) (end - p))
				die("a malformed key in %s", L->rel);
			f[i].p = p;
			f[i].len = (size_t) len;
			f[i].null = false;
			p += len;
		}
		n = ws_cells(v, vl, c, WS_MAX_COLS);
	}
	for (int i = 0; i < n; i++)
	{
		int			ix = ws_copy_index(L, c[i].tag);

		/* A cell of a column the layout no longer has is not restored. */
		if (ix < 0)
			continue;
		f[ix].p = c[i].p;
		f[ix].len = c[i].len;
		f[ix].null = false;
	}
}

/* ------------------------------------------------------------ COPY text */

void
ws_copy_escape(Buf *b, const uint8_t *v, size_t len)
{
	for (size_t i = 0; i < len; i++)
	{
		char		c = (char) v[i],
					e = 0;

		switch (c)
		{
			case '\\': e = '\\'; break;
			case '\n': e = 'n'; break;
			case '\r': e = 'r'; break;
			case '\t': e = 't'; break;
			case '\b': e = 'b'; break;
			case '\f': e = 'f'; break;
			case '\v': e = 'v'; break;
		}
		if (e)
		{
			char		pair[2] = {'\\', e};

			buf_append(b, pair, 2);
		}
		else
			buf_append(b, &c, 1);
	}
}

/*
 * Splits a COPY text line into n fields, unescaped into scratch (which the
 * fields then point into).
 */
void
ws_copy_split(const char *line, size_t len, WsField *f, int n, Buf *scratch)
{
	int			i = 0;
	size_t		start;

	scratch->len = 0;
	buf_reserve(scratch, len + 1);
	start = 0;
	for (size_t j = 0; j <= len && i < n; j++)
	{
		if (j < len && line[j] != '\t')
			continue;
		if (j - start == 2 && line[start] == '\\' && line[start + 1] == 'N')
		{
			f[i].null = true;
			f[i].p = NULL;
			f[i].len = 0;
		}
		else
		{
			size_t		off = scratch->len;

			for (size_t q = start; q < j; q++)
			{
				char		c = line[q];

				if (c == '\\' && q + 1 < j)
				{
					char		e = line[++q];

					c = e == 'n' ? '\n' : e == 'r' ? '\r' : e == 't' ? '\t' :
						e == 'b' ? '\b' : e == 'f' ? '\f' : e == 'v' ? '\v' : e;
				}
				scratch->data[scratch->len++] = (uint8_t) c;
			}
			f[i].null = false;
			f[i].p = scratch->data + off;
			f[i].len = scratch->len - off;
		}
		i++;
		start = j + 1;
	}
	if (i != n)
		die("%s", "a COPY line with too few fields");
}

/* A COPY text line (with its newline) from stored fields in COPY order. */
void
ws_copy_line(const WsLayout *L, const WsField *f, Buf *line)
{
	line->len = 0;
	for (int i = 0; i < L->nstored; i++)
	{
		if (i)
			buf_append(line, "\t", 1);
		if (f[i].null)
			buf_append(line, "\\N", 2);
		else
			ws_copy_escape(line, f[i].p, f[i].len);
	}
	buf_append(line, "\n", 1);
}
