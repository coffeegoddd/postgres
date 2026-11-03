/*-------------------------------------------------------------------------
 *
 * ws_row.c
 *	  The working-set model's rows: "cells", the row codec, and COPY text
 *	  (prolly/ws/ws_row.h).
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/ws/ws_row.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "prolly/ws/ws_row.h"

const WsRowCodecRoutine *WsRowCodec = &CellsWsRowCodec;

pg_noreturn static void
malformed(const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("a malformed row: %s", what)));
}

/* --------------------------------------------------------------- shapes */

static int
shape_tag_cmp(const void *a, const void *b)
{
	uint64		x = ((const WsShapeTag *) a)->tag,
				y = ((const WsShapeTag *) b)->tag;

	return (x > y) - (x < y);
}

void
ws_row_shape_init(WsRowShape *s, const uint64 *tags, int nstored, int nkey)
{
	if (nstored < 0 || nstored > WS_MAX_COLS || nkey < 0 || nkey > nstored)
		elog(ERROR, "a row of %d columns, %d of them its key", nstored, nkey);
	s->nkey = nkey;
	s->nstored = nstored;
	s->tags = px_malloc((size_t) nstored * sizeof(uint64));
	s->by_tag = px_malloc((size_t) nstored * sizeof(WsShapeTag));
	for (int i = 0; i < nstored; i++)
	{
		s->tags[i] = tags[i];
		s->by_tag[i].tag = tags[i];
		s->by_tag[i].at = i;
	}
	qsort(s->by_tag, (size_t) nstored, sizeof(WsShapeTag), shape_tag_cmp);
	for (int i = 1; i < nstored; i++)
		if (s->by_tag[i].tag == s->by_tag[i - 1].tag)
			elog(ERROR, "a row with two columns of tag " UINT64_FORMAT, s->by_tag[i].tag);
}

void
ws_row_shape_free(WsRowShape *s)
{
	px_free(s->tags);
	px_free(s->by_tag);
	memset(s, 0, sizeof(*s));
}

int
ws_row_shape_at(const WsRowShape *s, uint64 tag)
{
	WsShapeTag	key = {tag, 0};
	WsShapeTag *t = bsearch(&key, s->by_tag, (size_t) s->nstored, sizeof(WsShapeTag),
							shape_tag_cmp);

	return t ? t->at : -1;
}

/* ---------------------------------------------------------------- cells */

static int
cell_cmp(const void *a, const void *b)
{
	uint64		x = ((const WsCell *) a)->tag,
				y = ((const WsCell *) b)->tag;

	return (x > y) - (x < y);
}

static void
cells_encode(WsCell *c, int n, Buf *out)
{
	if (n > WS_MAX_COLS)
		elog(ERROR, "a row of %d cells", n);
	qsort(c, (size_t) n, sizeof(WsCell), cell_cmp);
	for (int i = 1; i < n; i++)
		if (c[i].tag == c[i - 1].tag)
			elog(ERROR, "a row with two cells of tag " UINT64_FORMAT, c[i].tag);
	for (int i = 0; i < n; i++)
	{
		buf_append_uvarint(out, c[i].tag);
		buf_append_uvarint(out, c[i].len);
		buf_append(out, c[i].p, c[i].len);
	}
}

/*
 * The next cell at *p: false at the end.  prev is the tag before it, if
 * has_prev.
 */
static bool
cell_next(const uint8 **p, const uint8 *end, bool has_prev, uint64 prev, WsCell *c)
{
	uint64		tag = 0,
				len = 0;

	if (*p >= end)
		return false;
	if (!read_uvarint(p, end, &tag) || !read_uvarint(p, end, &len) ||
		len > (uint64) (end - *p))
		malformed("a cell runs past its end");
	if (has_prev && tag <= prev)
		malformed("its cells are not in order of their tags");
	c->tag = tag;
	c->p = *p;
	c->len = (size_t) len;
	*p += len;
	return true;
}

static int
cells_parse(const uint8 *v, size_t vl, WsCell *out)
{
	const uint8 *p = v,
			   *end = v + vl;
	WsCell		c;
	int			n = 0;

	while (cell_next(&p, end, n > 0, n > 0 ? out[n - 1].tag : 0, &c))
	{
		if (n == WS_MAX_COLS)
			malformed("more cells than a table has columns");
		out[n++] = c;
	}
	return n;
}

static bool
cells_cell_get(const uint8 *v, size_t vl, uint64 tag, const uint8 **p, size_t *len)
{
	const uint8 *q = v,
			   *end = v + vl;
	WsCell		c;
	bool		has_prev = false;
	uint64		prev = 0;

	/* (in order of their tags: past it, it is not there) */
	while (cell_next(&q, end, has_prev, prev, &c) && c.tag <= tag)
	{
		if (c.tag == tag)
		{
			*p = c.p;
			*len = c.len;
			return true;
		}
		has_prev = true;
		prev = c.tag;
	}
	return false;
}

/* The non-NULL fields from from on, as cells appended to out. */
static void
put_cells(const WsRowShape *s, const WsField *f, int from, Buf *out)
{
	WsCell		cells[WS_MAX_COLS];
	int			n = 0;

	for (int i = from; i < s->nstored; i++)
		if (!f[i].null)
		{
			cells[n].tag = s->tags[i];
			cells[n].p = f[i].p;
			cells[n].len = f[i].len;
			n++;
		}
	cells_encode(cells, n, out);
}

static void
cells_row_encode(const WsRowShape *s, const WsField *f, Buf *key, Buf *val)
{
	key->len = 0;
	val->len = 0;
	if (s->nkey == 0)
	{
		buf_append(key, "r", 1);
		put_cells(s, f, 0, key);
		return;
	}
	for (int i = 0; i < s->nkey; i++)
	{
		if (f[i].null)
			elog(ERROR, "a NULL in a primary key");
		buf_append_uvarint(key, f[i].len);
		buf_append(key, f[i].p, f[i].len);
	}
	put_cells(s, f, s->nkey, val);
}

static void
cells_key_fields(const WsRowShape *s, const uint8 *k, size_t kl, WsField *f)
{
	const uint8 *p = k,
			   *end = k + kl;

	for (int i = 0; i < s->nkey; i++)
	{
		uint64		len = 0;

		if (!read_uvarint(&p, end, &len) || len > (uint64) (end - p))
			malformed("a key's value runs past its end");
		f[i].p = p;
		f[i].len = (size_t) len;
		f[i].null = false;
		p += len;
	}
	if (p != end)
		malformed("a key has bytes left over");
}

static int
cells_key_cells(const uint8 *k, size_t kl, WsCell *out)
{
	if (kl == 0 || k[0] != 'r')
		malformed("a keyless row's key has no mark");
	return cells_parse(k + 1, kl - 1, out);
}

static void
cells_row_decode(const WsRowShape *s, const uint8 *k, size_t kl, const uint8 *v, size_t vl,
				 WsField *f)
{
	WsCell		c[WS_MAX_COLS];
	int			n,
				t = 0;

	for (int i = 0; i < s->nstored; i++)
	{
		f[i].p = NULL;
		f[i].len = 0;
		f[i].null = true;
	}
	if (s->nkey == 0)
		n = cells_key_cells(k, kl, c);
	else
	{
		cells_key_fields(s, k, kl, f);
		n = cells_parse(v, vl, c);
	}

	/* (both in order of their tags: a cell the shape has no column for skipped) */
	for (int i = 0; i < n; i++)
	{
		while (t < s->nstored && s->by_tag[t].tag < c[i].tag)
			t++;
		if (t < s->nstored && s->by_tag[t].tag == c[i].tag)
		{
			WsField    *to = &f[s->by_tag[t].at];

			if (s->by_tag[t].at < s->nkey)
				malformed("a value has a cell for a key's column");
			to->p = c[i].p;
			to->len = c[i].len;
			to->null = false;
		}
	}
}

const WsRowCodecRoutine CellsWsRowCodec = {
	.name = "cells",
	.encode = cells_row_encode,
	.decode = cells_row_decode,
	.key_fields = cells_key_fields,
	.key_cells = cells_key_cells,
	.cells_encode = cells_encode,
	.cells = cells_parse,
	.cell_get = cells_cell_get,
};

/* ------------------------------------------------------------ COPY text */

void
ws_copy_escape(Buf *b, const uint8 *v, size_t len)
{
	for (size_t i = 0; i < len; i++)
	{
		char		c = (char) v[i],
					e = 0;

		switch (c)
		{
			case '\\':
				e = '\\';
				break;
			case '\n':
				e = 'n';
				break;
			case '\r':
				e = 'r';
				break;
			case '\t':
				e = 't';
				break;
			case '\b':
				e = 'b';
				break;
			case '\f':
				e = 'f';
				break;
			case '\v':
				e = 'v';
				break;
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

void
ws_copy_split(const char *line, size_t len, WsField *f, int n, Buf *scratch)
{
	int			i = 0;
	size_t		start = 0;

	if (n == 0 && len == 0)
		return;
	scratch->len = 0;
	/* (room for them all at once: the fields point into it as they are made) */
	buf_reserve(scratch, len + 1);
	for (size_t j = 0; j <= len; j++)
	{
		if (j < len && line[j] != '\t')
			continue;
		if (i == n)
			elog(ERROR, "a COPY line with more than %d fields", n);
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
				scratch->data[scratch->len++] = (uint8) c;
			}
			f[i].null = false;
			f[i].p = scratch->data + off;
			f[i].len = scratch->len - off;
		}
		i++;
		start = j + 1;
	}
	if (i != n)
		elog(ERROR, "a COPY line of %d fields, not %d", i, n);
}

void
ws_copy_line(const WsRowShape *s, const WsField *f, Buf *line)
{
	line->len = 0;
	for (int i = 0; i < s->nstored; i++)
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
