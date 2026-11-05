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

#include "miscadmin.h"

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

/* What names a tree: its root, and its value's length, which only a large value has. */
static void
large_decode(const uint8 *p, size_t len, PHash *root, uint64 *total)
{
	const uint8 *q = p + PHASH_LEN;
	const uint8 *end = p + len;

	if (len <= PHASH_LEN || !ws_read_canonical_uvarint(&q, end, total) || q != end ||
		*total <= WS_LARGE_CELL)
		malformed("a large value's cell is not a tree and the length of a large value");
	memcpy(root->b, p, PHASH_LEN);
}

static void
large_encode(const PHash *root, uint64 total, Buf *out)
{
	buf_append(out, root->b, PHASH_LEN);
	buf_append_uvarint(out, total);
}

/* Sorted, each tag once, and what cell_next would read. */
static void
cells_ready(WsCell *c, int n)
{
	if (n > WS_MAX_COLS)
		elog(ERROR, "a row of %d cells", n);
	qsort(c, (size_t) n, sizeof(WsCell), cell_cmp);
	for (int i = 0; i < n; i++)
	{
		if (i > 0 && c[i].tag == c[i - 1].tag)
			elog(ERROR, "a row with two cells of tag " UINT64_FORMAT, c[i].tag);
		if (!c[i].large && c[i].len > WS_LARGE_CELL)
			elog(ERROR, "a value of %zu bytes in its cell", c[i].len);
		if (c[i].large)
		{
			PHash		root;
			uint64		total;

			large_decode(c[i].p, c[i].len, &root, &total);
		}
	}
}

static void
cells_put(const WsCell *c, int n, Buf *out)
{
	for (int i = 0; i < n; i++)
	{
		buf_append_uvarint(out, c[i].tag);
		buf_append_uvarint(out, ((uint64) c[i].len << 1) | (c[i].large ? 1 : 0));
		buf_append(out, c[i].p, c[i].len);
	}
}

/* The trees cells name, as what a value starts with. */
static void
refs_put(const WsCell *c, int n, Buf *out)
{
	uint64		nrefs = 0;

	for (int i = 0; i < n; i++)
		if (c[i].large)
			nrefs++;
	buf_append_uvarint(out, nrefs);
	for (int i = 0; i < n; i++)
		if (c[i].large)
			buf_append(out, c[i].p, PHASH_LEN);
}

static void
cells_encode(WsCell *c, int n, Buf *out)
{
	cells_ready(c, n);
	refs_put(c, n, out);
	cells_put(c, n, out);
}

/*
 * The next cell at *p: false at the end.  prev is the tag before it, if
 * has_prev.
 */
static bool
cell_next(const uint8 **p, const uint8 *end, bool has_prev, uint64 prev, WsCell *c)
{
	uint64		tag = 0,
				sized = 0,
				len;

	if (*p >= end)
		return false;
	if (!ws_read_canonical_uvarint(p, end, &tag) || !ws_read_canonical_uvarint(p, end, &sized) ||
		(sized >> 1) > (uint64) (end - *p))
		malformed("a cell runs past its end, or is not in its shortest form");
	if (has_prev && tag <= prev)
		malformed("its cells are not in order of their tags");
	len = sized >> 1;
	c->tag = tag;
	c->p = *p;
	c->len = (size_t) len;
	c->large = (sized & 1) != 0;
	if (c->large)
	{
		PHash		root;
		uint64		total;

		large_decode(c->p, c->len, &root, &total);
	}
	else if (len > WS_LARGE_CELL)
		malformed("a value in its cell that belongs in a tree");
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

/* The hashes a value starts with, and how many; *rest is what follows them. */
static const uint8 *
refs_parse(const uint8 *v, size_t vl, uint64 *nrefs, const uint8 **rest)
{
	const uint8 *p = v,
			   *end = v + vl;

	if (!ws_read_canonical_uvarint(&p, end, nrefs) || *nrefs > WS_MAX_COLS ||
		*nrefs * PHASH_LEN > (uint64) (end - p))
		malformed("a value's trees run past its end, or are not in their shortest form");
	*rest = p + *nrefs * PHASH_LEN;
	return p;
}

/* What cells name is what a value says it holds, or a tree could go uncounted. */
static void
refs_check(const uint8 *refs, uint64 nrefs, const WsCell *c, int n)
{
	uint64		at = 0;

	for (int i = 0; i < n; i++)
		if (c[i].large)
		{
			if (at == nrefs || memcmp(refs + at * PHASH_LEN, c[i].p, PHASH_LEN) != 0)
				malformed("a large value's tree is not one its value starts with");
			at++;
		}
	if (at != nrefs)
		malformed("a value starts with a tree no cell names");
}

static int
value_cells(const uint8 *v, size_t vl, WsCell *out)
{
	uint64		nrefs;
	const uint8 *rest;
	const uint8 *refs = refs_parse(v, vl, &nrefs, &rest);
	int			n = cells_parse(rest, (size_t) (v + vl - rest), out);

	refs_check(refs, nrefs, out, n);
	return n;
}

static bool
cells_cell_get(const uint8 *v, size_t vl, uint64 tag, WsCell *out)
{
	uint64		nrefs;
	const uint8 *q;
	const uint8 *end = v + vl;
	WsCell		c;
	bool		has_prev = false;
	uint64		prev = 0;

	(void) refs_parse(v, vl, &nrefs, &q);
	/* (in order of their tags: past it, it is not there) */
	while (cell_next(&q, end, has_prev, prev, &c) && c.tag <= tag)
	{
		if (c.tag == tag)
		{
			*out = c;
			return true;
		}
		has_prev = true;
		prev = c.tag;
	}
	return false;
}

/* The non-NULL fields from from on, as cells appended to out: as a value's, or a key's. */
static void
put_cells(const WsRowShape *s, const WsField *f, int from, bool as_value, Buf *out)
{
	WsCell		cells[WS_MAX_COLS];
	int			n = 0;

	for (int i = from; i < s->nstored; i++)
		if (!f[i].null)
		{
			cells[n].tag = s->tags[i];
			cells[n].p = f[i].p;
			cells[n].len = f[i].len;
			cells[n].large = f[i].large;
			n++;
		}
	if (as_value)
		cells_encode(cells, n, out);
	else
	{
		cells_ready(cells, n);
		cells_put(cells, n, out);
	}
}

static void
cells_row_encode(const WsRowShape *s, const WsField *f, Buf *key, Buf *val)
{
	key->len = 0;
	val->len = 0;
	if (s->nkey == 0)
	{
		buf_append(key, "r", 1);
		put_cells(s, f, 0, false, key);
		return;
	}
	for (int i = 0; i < s->nkey; i++)
	{
		if (f[i].null)
			elog(ERROR, "a NULL in a primary key");
		if (f[i].large)
			elog(ERROR, "a large value's tree in a primary key");
		buf_append_uvarint(key, f[i].len);
		buf_append(key, f[i].p, f[i].len);
	}
	put_cells(s, f, s->nkey, true, val);
}

static void
cells_key_fields(const WsRowShape *s, const uint8 *k, size_t kl, WsField *f)
{
	const uint8 *p = k,
			   *end = k + kl;

	for (int i = 0; i < s->nkey; i++)
	{
		uint64		len = 0;

		if (!ws_read_canonical_uvarint(&p, end, &len) || len > (uint64) (end - p))
			malformed("a key's value runs past its end, or is not in its shortest form");
		f[i].p = p;
		f[i].len = (size_t) len;
		f[i].null = false;
		f[i].large = false;
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
		f[i].large = false;
	}
	if (s->nkey == 0)
		n = cells_key_cells(k, kl, c);
	else
	{
		cells_key_fields(s, k, kl, f);
		n = value_cells(v, vl, c);
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
			to->large = c[i].large;
		}
	}
}

static void
cells_keyless_value(const uint8 *k, size_t kl, const uint8 *payload, size_t plen, Buf *val)
{
	WsCell		c[WS_MAX_COLS];
	int			n = cells_key_cells(k, kl, c);

	val->len = 0;
	refs_put(c, n, val);
	buf_append(val, payload, plen);
}

static void
cells_keyless_payload(const uint8 *k, size_t kl, const uint8 *v, size_t vl,
					  const uint8 **payload, size_t *plen)
{
	WsCell		c[WS_MAX_COLS];
	int			n = cells_key_cells(k, kl, c);
	uint64		nrefs;
	const uint8 *refs = refs_parse(v, vl, &nrefs, payload);

	refs_check(refs, nrefs, c, n);
	*plen = (size_t) (v + vl - *payload);
}

const WsRowCodecRoutine CellsWsRowCodec = {
	.name = "cells",
	.encode = cells_row_encode,
	.decode = cells_row_decode,
	.key_fields = cells_key_fields,
	.key_cells = cells_key_cells,
	.cells_encode = cells_encode,
	.cells = value_cells,
	.cell_get = cells_cell_get,
	.keyless_value = cells_keyless_value,
	.keyless_payload = cells_keyless_payload,
	.large_encode = large_encode,
	.large_decode = large_decode,
};

/* --------------------------------------------------------- large values */

/* A tree's pieces, by the offset each starts at. */
#define LARGE_PIECE 4000

static void
offset_key(uint64 at, uint8 *key)
{
	for (int i = 7; i >= 0; i--)
	{
		key[i] = (uint8) at;
		at >>= 8;
	}
}

static void
tree_put(ProllyCtx *ctx, const uint8 *p, size_t len, PHash *root)
{
	size_t		n = (len + LARGE_PIECE - 1) / LARGE_PIECE;
	ProllyKV   *kvs = palloc_array(ProllyKV, n);
	uint8	   *keys = palloc_array(uint8, n * 8);

	for (size_t i = 0; i < n; i++)
	{
		size_t		at = i * LARGE_PIECE;

		offset_key(at, keys + i * 8);
		kvs[i].key = keys + i * 8;
		kvs[i].klen = 8;
		kvs[i].val = p + at;
		kvs[i].vlen = Min(len - at, LARGE_PIECE);
	}
	if (prolly_build(ctx, 0, kvs, n, root) < 0)
		prolly_report(ctx);
	pfree(keys);
	pfree(kvs);
}

static void
not_its_tree(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("a malformed row: a large value's tree does not hold the value its cell says")));
}

static void
tree_get(ProllyCtx *ctx, const PHash *root, uint64 len, Buf *out)
{
	ProllyIter *it;
	const uint8 *k,
			   *v;
	size_t		kl,
				vl;
	uint64		at = 0;
	int			rc;

	if (prolly_iter_open(ctx, root, NULL, 0, &it) < 0)
		prolly_report(ctx);
	while ((rc = prolly_iter_next(it, &k, &kl, &v, &vl)) > 0)
	{
		uint8		want[8];

		CHECK_FOR_INTERRUPTS();
		offset_key(at, want);
		if (kl != 8 || memcmp(k, want, 8) != 0 || at >= len || vl != Min(len - at, LARGE_PIECE))
			not_its_tree();
		buf_append(out, v, vl);
		at += vl;
	}
	prolly_iter_close(it);
	if (rc < 0)
		prolly_report(ctx);
	if (at != len)
		not_its_tree();
}

const WsLargeValueRoutine TreeWsLargeValues = {
	.name = "tree",
	.put = tree_put,
	.get = tree_get,
};

const WsLargeValueRoutine *WsLargeValues = &TreeWsLargeValues;

void
ws_row_store(ProllyCtx *ctx, const WsRowShape *s, const WsField *f, Buf *key, Buf *val)
{
	WsField    *fields = palloc_array(WsField, Max(s->nstored, 1));
	Buf			named = BUF_INIT;
	size_t	   *at = palloc_array(size_t, Max(s->nstored, 1));

	for (int i = 0; i < s->nstored; i++)
	{
		if (f[i].large)
			elog(ERROR, "a large value's tree given as a value to store");
		fields[i] = f[i];
		if (i >= s->nkey && !f[i].null && f[i].len > WS_LARGE_CELL)
		{
			PHash		root;

			WsLargeValues->put(ctx, f[i].p, f[i].len, &root);
			at[i] = named.len;
			WsRowCodec->large_encode(&root, f[i].len, &named);
			fields[i].len = named.len - at[i];
			fields[i].large = true;
		}
	}
	/* (named has stopped moving) */
	for (int i = 0; i < s->nstored; i++)
		if (fields[i].large)
			fields[i].p = named.data + at[i];
	WsRowCodec->encode(s, fields, key, val);
	buf_free(&named);
	pfree(at);
	pfree(fields);
}

void
ws_row_load(ProllyCtx *ctx, const WsRowShape *s, const uint8 *k, size_t kl,
			const uint8 *v, size_t vl, WsField *f, Buf *held)
{
	size_t	   *at = palloc_array(size_t, Max(s->nstored, 1));

	WsRowCodec->decode(s, k, kl, v, vl, f);
	if (s->nkey == 0)
	{
		const uint8 *payload;
		size_t		plen;

		/* (decoding reads its key alone) */
		WsRowCodec->keyless_payload(k, kl, v, vl, &payload, &plen);
	}
	held->len = 0;
	for (int i = 0; i < s->nstored; i++)
		if (f[i].large)
		{
			PHash		root;
			uint64		total;

			WsRowCodec->large_decode(f[i].p, f[i].len, &root, &total);
			at[i] = held->len;
			WsLargeValues->get(ctx, &root, total, held);
			f[i].len = (size_t) total;
		}
	/* (held has stopped moving) */
	for (int i = 0; i < s->nstored; i++)
		if (f[i].large)
		{
			f[i].p = held->data + at[i];
			f[i].large = false;
		}
	pfree(at);
}

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
			f[i].large = false;
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
			f[i].large = false;
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
