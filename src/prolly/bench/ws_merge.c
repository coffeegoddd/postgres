/*
 * ws_merge.c
 *	  Three-way merge of root values: the schema by object and, within a
 *	  table, by column tag; then rows by key and, where both sides changed
 *	  a row, by column.
 *
 * Conflicts, as "<kind> <where>":
 *	object <type> <name>		both changed an object differently
 *	table-ddl <table>			both changed a table's definition outside its
 *								columns and constraints
 *	column <table>.<name>		both changed a column differently, or one
 *								dropped it and the other changed it
 *	column-name <table>.<name>	two different columns took the same name
 *	key <table>					the primary keys differ
 *	table <table>				one dropped a table the other changed
 *	row <table> <key>			both changed a value differently, or one
 *								deleted a row the other changed
 *
 * With no conflicts, rows that only one side changed are moved to the
 * merged schema: values of columns the merge dropped are removed, and
 * columns the other side added with a default get it.  What the merged
 * schema then rejects (constraints) is found by restoring the result.
 */
#include "ws.h"

static void
conflict(WsMerge *m, const char *kind, const char *where)
{
	char		line[1200];

	snprintf(line, sizeof(line), "%s %s", kind, where);
	for (size_t i = 0; i < m->nconflicts; i++)
		if (strcmp(m->conflict[i], line) == 0)
			return;
	px_grow((void **) &m->conflict, &m->cap, m->nconflicts + 1, sizeof(char *));
	m->conflict[m->nconflicts++] = strdup(line);
}

void
ws_merge_free(WsMerge *m)
{
	for (size_t i = 0; i < m->nconflicts; i++)
		free(m->conflict[i]);
	free(m->conflict);
	memset(m, 0, sizeof(*m));
}

static bool
str_eq(const char *a, const char *b)
{
	if (a == NULL || b == NULL)
		return a == b;
	return strcmp(a, b) == 0;
}

/*
 * The three-way choice: the side that changed, or either if both made the
 * same change; NULL if both changed differently (absent counts as a value).
 */
static const char *
pick(const char *b, const char *o, const char *t, bool *clash)
{
	*clash = false;
	if (str_eq(o, t))
		return o;
	if (str_eq(o, b))
		return t;
	if (str_eq(t, b))
		return o;
	*clash = true;
	return NULL;
}

/* ------------------------------------------------- a table's definition */

typedef struct DdlItem
{
	char	   *key;			/* t:<tag>, n:<name> or c:<constraint> */
	char	   *line;			/* without its trailing comma */
} DdlItem;

typedef struct Ddl
{
	char	   *head;			/* through "CREATE ... (" and its newline */
	DdlItem    *item;
	int			n;
	size_t		cap;
	char	   *tail;			/* from the closing ")" */
} Ddl;

/* A quoted or plain identifier at p, as written. */
static char *
ident_at(const char *p)
{
	const char *e = p;

	if (*e == '"')
	{
		for (e++; *e; e++)
			if (*e == '"')
			{
				if (e[1] != '"')
					break;
				e++;
			}
		e++;
	}
	else
		while (*e && *e != ' ' && *e != ',')
			e++;
	return strndup(p, (size_t) (e - p));
}

static bool
ddl_parse(const char *text, const WsLayout *L, Ddl *d)
{
	const char *open = strstr(text, " (\n"),
			   *p,
			   *close;

	memset(d, 0, sizeof(*d));
	if (text == NULL || strncmp(text, "CREATE ", 7) != 0 || open == NULL)
		return false;
	d->head = strndup(text, (size_t) (open + 3 - text));
	close = strstr(open, "\n)");
	if (close == NULL)
		return false;
	d->tail = strdup(close + 1);
	for (p = open + 3; p < close;)
	{
		const char *nl = memchr(p, '\n', (size_t) (close - p));
		size_t		ll = nl ? (size_t) (nl - p) : (size_t) (close - p);
		const char *s = p;
		char	   *line,
				   *name;
		char		key[1100];

		while (*s == ' ')
			s++;
		line = strndup(p, ll - (p[ll - 1] == ',' ? 1 : 0));
		if (strncmp(s, "CONSTRAINT ", 11) == 0)
		{
			name = ident_at(s + 11);
			snprintf(key, sizeof(key), "c:%s", name);
		}
		else
		{
			const WsCol *c;

			name = ident_at(s);
			c = L ? ws_layout_col_named(L, name) : NULL;
			if (c)
				snprintf(key, sizeof(key), "t:%" PRIu64, c->tag);
			else
				snprintf(key, sizeof(key), "n:%s", name);
		}
		free(name);
		px_grow((void **) &d->item, &d->cap, (size_t) d->n + 1, sizeof(DdlItem));
		d->item[d->n].key = strdup(key);
		d->item[d->n].line = line;
		d->n++;
		p += ll + 1;
	}
	return true;
}

static void
ddl_free(Ddl *d)
{
	for (int i = 0; i < d->n; i++)
	{
		free(d->item[i].key);
		free(d->item[i].line);
	}
	free(d->item);
	free(d->head);
	free(d->tail);
}

static const char *
ddl_item(const Ddl *d, const char *key)
{
	for (int i = 0; d && i < d->n; i++)
		if (strcmp(d->item[i].key, key) == 0)
			return d->item[i].line;
	return NULL;
}

/*
 * Merges a CREATE TABLE object item by item (columns by tag, constraints
 * by name); the rest of the object must be changed by one side only.
 */
static bool
merge_table_ddl(const char *table, const char *b, const char *o, const char *t,
				const WsLayout *Lb, const WsLayout *Lo, const WsLayout *Lt, Buf *out,
				WsMerge *m)
{
	Ddl			db,
				dO,
				dt;
	bool		hb = b && ddl_parse(b, Lb, &db),
				ok = true,
				clash;
	const char *head,
			   *tail;
	char	  **keys = NULL;
	int			nk = 0;
	size_t		kcap = 0;
	bool		first = true;

	if (!(ddl_parse(o, Lo, &dO) && ddl_parse(t, Lt, &dt)))
	{
		conflict(m, "table-ddl", table);
		return false;
	}
	head = pick(hb ? db.head : NULL, dO.head, dt.head, &clash);
	ok &= !clash;
	tail = pick(hb ? db.tail : NULL, dO.tail, dt.tail, &clash);
	ok &= !clash;
	if (!ok)
		conflict(m, "table-ddl", table);

	/*
	 * Ours' columns, then the other columns theirs has, then the same for
	 * constraints; pick decides each (one dropped, both kept, a clash).
	 */
	for (int pass = 0; pass < 4; pass++)
	{
		const Ddl  *src = pass % 2 == 0 ? &dO : &dt;
		bool		cons = pass >= 2;

		for (int i = 0; i < src->n; i++)
		{
			const char *k = src->item[i].key;
			bool		seen = false;

			if ((k[0] == 'c') != cons)
				continue;
			for (int j = 0; j < nk; j++)
				seen |= strcmp(keys[j], k) == 0;
			if (seen)
				continue;
			px_grow((void **) &keys, &kcap, (size_t) nk + 1, sizeof(char *));
			keys[nk++] = (char *) k;
		}
	}
	if (ok)
		buf_append(out, head, strlen(head));
	for (int i = 0; i < nk; i++)
	{
		const char *bl = hb ? ddl_item(&db, keys[i]) : NULL,
				   *ol = ddl_item(&dO, keys[i]),
				   *tl = ddl_item(&dt, keys[i]),
				   *line = pick(bl, ol, tl, &clash);

		if (clash)
		{
			char		where[1200];
			const char *nm = ol ? ol : tl;

			while (*nm == ' ')
				nm++;
			snprintf(where, sizeof(where), "%s.%.*s", table, (int) strcspn(nm, " "), nm);
			conflict(m, "column", where);
			ok = false;
			continue;
		}
		if (line == NULL || !ok)
			continue;
		if (!first)
			buf_append(out, ",\n", 2);
		buf_append(out, line, strlen(line));
		first = false;
	}
	if (ok)
	{
		buf_append(out, "\n", 1);
		buf_append(out, tail, strlen(tail));
	}
	free(keys);
	if (hb)
		ddl_free(&db);
	ddl_free(&dO);
	ddl_free(&dt);
	return ok;
}

/* ------------------------------------------------------------- objects */

static char *
dup_val(const uint8_t *v, size_t len)
{
	return v ? strndup((const char *) v, len) : NULL;
}

static bool
layout_of(ProllyCtx *ctx, const PHash *rv, const char *name, WsLayout *L)
{
	return rv && table_layout(ctx, rv, name, L, NULL);
}

static PHash
merge_objects(ProllyCtx *ctx, const PHash *base, const PHash *ours, const PHash *theirs,
			  const PHash *ob, const PHash *oo, const PHash *ot, WsMerge *m)
{
	ProllyMergeResult res = {0};
	ProllyEdit *ed;
	size_t		ne = 0;
	Buf		   *bufs;
	PHash		out;

	CHECK(prolly_merge(ctx, ob, oo, ot, &res) == PROLLY_OK);
	ed = px_malloc((res.nconflicts ? res.nconflicts : 1) * sizeof(ProllyEdit));
	bufs = calloc(res.nconflicts ? res.nconflicts : 1, sizeof(Buf));
	for (size_t i = 0; i < res.nconflicts; i++)
	{
		const ProllyConflict *c = &res.conflicts[i];
		char		key[1100],
				   *type,
				   *name;

		snprintf(key, sizeof(key), "%.*s", (int) c->klen, (const char *) c->key);
		type = key + 2;
		name = strchr(type, '\t');
		*name++ = '\0';
		if (strcmp(type, "TABLE") == 0 && c->ours && c->theirs)
		{
			WsLayout	Lb,
						Lo,
						Lt;
			bool		hb = layout_of(ctx, base, name, &Lb),
						ho = layout_of(ctx, ours, name, &Lo),
						ht = layout_of(ctx, theirs, name, &Lt);
			char	   *b = dup_val(c->base, c->blen),
					   *o = dup_val(c->ours, c->olen),
					   *t = dup_val(c->theirs, c->tlen);

			if (merge_table_ddl(name, b, o, t, hb ? &Lb : NULL, ho ? &Lo : NULL,
								ht ? &Lt : NULL, &bufs[i], m))
			{
				ed[ne].key = c->key;
				ed[ne].klen = c->klen;
				ed[ne].val = bufs[i].data;
				ed[ne].vlen = bufs[i].len;
				ed[ne].del = false;
				ne++;
			}
			if (hb)
				ws_layout_free(&Lb);
			if (ho)
				ws_layout_free(&Lo);
			if (ht)
				ws_layout_free(&Lt);
			free(b);
			free(o);
			free(t);
			continue;
		}
		{
			char		where[1200];

			snprintf(where, sizeof(where), "%s %s", type, name);
			conflict(m, "object", where);
		}
	}
	out = ne ? t_apply(ctx, &res.root, ed, ne) : res.root;
	for (size_t i = 0; i < res.nconflicts; i++)
		free(bufs[i].data);
	free(bufs);
	free(ed);
	prolly_merge_result_free(&res);
	return out;
}

/* Sequences advance on both sides; the merge keeps the further one. */
static PHash
merge_sequences(ProllyCtx *ctx, const PHash *sb, const PHash *so, const PHash *st)
{
	ProllyMergeResult res = {0};
	ProllyEdit *ed;
	size_t		ne = 0;
	PHash		out;

	CHECK(prolly_merge(ctx, sb, so, st, &res) == PROLLY_OK);
	ed = px_malloc((res.nconflicts ? res.nconflicts : 1) * sizeof(ProllyEdit));
	for (size_t i = 0; i < res.nconflicts; i++)
	{
		const ProllyConflict *c = &res.conflicts[i];

		if (c->ours == NULL || c->theirs == NULL)
			continue;
		ed[ne].key = c->key;
		ed[ne].klen = c->klen;
		if (strtoll((const char *) c->theirs, NULL, 10) > strtoll((const char *) c->ours, NULL, 10))
		{
			ed[ne].val = c->theirs;
			ed[ne].vlen = c->tlen;
		}
		else
		{
			ed[ne].val = c->ours;
			ed[ne].vlen = c->olen;
		}
		ed[ne].del = false;
		ne++;
	}
	out = ne ? t_apply(ctx, &res.root, ed, ne) : res.root;
	free(ed);
	prolly_merge_result_free(&res);
	return out;
}

/* -------------------------------------------------------------- layouts */

static bool
col_same(const WsCol *a, const WsCol *b)
{
	if (a == NULL || b == NULL)
		return a == b;
	return a->kind == b->kind && strcmp(a->type, b->type) == 0 &&
		strcmp(a->name, b->name) == 0 && strcmp(a->def, b->def) == 0;
}

static void
col_copy(WsCol *dst, const WsCol *src)
{
	*dst = *src;
	dst->type = strdup(src->type);
	dst->name = strdup(src->name);
	dst->def = strdup(src->def);
}

/* The merged layout (as text, which ws_layout_parse then reads) or false. */
static bool
merge_layout(const char *table, const WsLayout *Lb, const WsLayout *Lo, const WsLayout *Lt,
			 Buf *out, WsMerge *m)
{
	WsLayout	M = {0};
	size_t		ccap = 0;
	bool		ok = true;
	int			pos = 0;

	if (Lo->nkey != Lt->nkey ||
		(Lo->nkey && memcmp(Lo->key, Lt->key, (size_t) Lo->nkey * sizeof(uint64_t)) != 0))
	{
		conflict(m, "key", table);
		return false;
	}
	M.rel = Lo->rel;
	M.nkey = Lo->nkey;
	M.key = Lo->key;
	for (int pass = 0; pass < 2; pass++)
	{
		const WsLayout *S = pass == 0 ? Lo : Lt;

		for (int i = 0; i < S->ncols; i++)
		{
			uint64_t	tag = S->cols[i].tag;
			const WsCol *b = Lb ? ws_layout_col(Lb, tag) : NULL,
					   *o = ws_layout_col(Lo, tag),
					   *t = ws_layout_col(Lt, tag),
					   *keep;

			if (pass == 1 && o)
				continue;		/* placed in the first pass */
			if (col_same(o, t))
				keep = o;
			else if (col_same(o, b))
				keep = t;
			else if (col_same(t, b))
				keep = o;
			else
			{
				char		where[1200];

				snprintf(where, sizeof(where), "%s.%s", table, (o ? o : t)->name);
				conflict(m, "column", where);
				ok = false;
				continue;
			}
			if (keep == NULL)
				continue;		/* dropped by one side */
			px_grow((void **) &M.cols, &ccap, (size_t) M.ncols + 1, sizeof(WsCol));
			col_copy(&M.cols[M.ncols++], keep);
		}
	}
	for (int i = 0; i < M.ncols; i++)
	{
		for (int j = 0; j < i; j++)
			if (strcmp(M.cols[i].name, M.cols[j].name) == 0)
			{
				char		where[1200];

				snprintf(where, sizeof(where), "%s.%s", table, M.cols[i].name);
				conflict(m, "column-name", where);
				ok = false;
			}
		M.cols[i].pos = M.cols[i].kind == 's' ? pos++ : -1;
	}
	if (ok)
		ws_layout_write(&M, out);
	for (int i = 0; i < M.ncols; i++)
	{
		free(M.cols[i].type);
		free(M.cols[i].name);
		free(M.cols[i].def);
	}
	free(M.cols);
	return ok;
}

/* ----------------------------------------------------------------- rows */

typedef struct TableMerge
{
	ProllyCtx  *ctx;
	const char *name;
	const WsLayout *Lb,
			   *Lo,
			   *Lt,
			   *Lm;
	WsDefaultFn def;
	void	   *defarg;
	WsMerge    *m;
	/* defaults of added columns, evaluated once */
	uint64_t   *dtag;
	Buf		   *dval;
	bool	   *dhas;
	int			nd;
} TableMerge;

static bool
cell_of(const WsCell *c, int n, uint64_t tag, const WsCell **out)
{
	for (int i = 0; i < n; i++)
		if (c[i].tag == tag)
		{
			*out = &c[i];
			return true;
		}
	*out = NULL;
	return false;
}

static bool
cells_eq(const WsCell *a, const WsCell *b)
{
	if (a == NULL || b == NULL)
		return a == b;
	return bytes_eq(a->p, a->len, b->p, b->len);
}

/* A key as its values, comma separated. */
static void
key_text(const WsLayout *L, const uint8_t *k, size_t kl, char *out, size_t cap)
{
	const uint8_t *p = k,
			   *end = k + kl;
	size_t		n = 0;

	out[0] = '\0';
	for (int i = 0; i < L->nkey && n + 2 < cap; i++)
	{
		uint64_t	len = 0;

		if (!read_uvarint(&p, end, &len) || len > (uint64_t) (end - p))
			break;
		n += (size_t) snprintf(out + n, cap - n, "%s%.*s", i ? "," : "", (int) len, p);
		p += len;
	}
}

/*
 * Merges a row both sides changed, column by column, into out (the
 * value's cells in the merged layout's non-key columns).
 */
static bool
merge_cells(TableMerge *tm, const uint8_t *k, size_t kl, const ProllyConflict *c, Buf *out)
{
	WsCell		cb[WS_MAX_COLS],
				co[WS_MAX_COLS],
				ct[WS_MAX_COLS],
				cm[WS_MAX_COLS];
	int			nb = c->base ? ws_cells(c->base, c->blen, cb, WS_MAX_COLS) : 0,
				no = ws_cells(c->ours, c->olen, co, WS_MAX_COLS),
				nt = ws_cells(c->theirs, c->tlen, ct, WS_MAX_COLS),
				nm = 0;

	for (int i = tm->Lm->nkey; i < tm->Lm->nstored; i++)
	{
		uint64_t	tag = tm->Lm->copy[i];
		const WsCell *b,
				   *o,
				   *t,
				   *keep;

		cell_of(cb, nb, tag, &b);
		cell_of(co, no, tag, &o);
		cell_of(ct, nt, tag, &t);
		if (cells_eq(o, t))
			keep = o;
		else if (cells_eq(o, b))
			keep = t;
		else if (cells_eq(t, b))
			keep = o;
		else
		{
			char		where[1300],
						kt[200];

			key_text(tm->Lm, k, kl, kt, sizeof(kt));
			snprintf(where, sizeof(where), "%s %s", tm->name, kt);
			conflict(tm->m, "row", where);
			return false;
		}
		if (keep)
			cm[nm++] = *keep;
	}
	out->len = 0;
	ws_cells_encode(cm, nm, out);
	tm->m->cells_merged++;
	return true;
}

static long
count_of(const uint8_t *v, size_t len)
{
	char		cnt[24];

	if (v == NULL)
		return 0;
	snprintf(cnt, sizeof(cnt), "%.*s", (int) len, (const char *) v);
	return atol(cnt);
}

/* The default value of a column the other side added, if it has one. */
static bool
added_default(TableMerge *tm, const WsLayout *side, uint64_t tag, const uint8_t **p,
			  size_t *len)
{
	const WsCol *c = ws_layout_col(tm->Lm, tag);

	if (c == NULL || c->kind != 's' || c->def[0] == '\0' || ws_layout_col(side, tag) ||
		(tm->Lb && ws_layout_col(tm->Lb, tag)))
		return false;
	for (int i = 0; i < tm->nd; i++)
		if (tm->dtag[i] == tag)
		{
			*p = tm->dval[i].data;
			*len = tm->dval[i].len;
			return tm->dhas[i];
		}
	tm->dtag = px_realloc(tm->dtag, (size_t) (tm->nd + 1) * sizeof(uint64_t));
	tm->dval = px_realloc(tm->dval, (size_t) (tm->nd + 1) * sizeof(Buf));
	tm->dhas = px_realloc(tm->dhas, (size_t) (tm->nd + 1) * sizeof(bool));
	tm->dtag[tm->nd] = tag;
	memset(&tm->dval[tm->nd], 0, sizeof(Buf));
	tm->dhas[tm->nd] = tm->def && tm->def(tm->defarg, c->def, c->type, &tm->dval[tm->nd]);
	tm->nd++;
	return added_default(tm, side, tag, p, len);
}

/*
 * A row one side changed, in the merged layout: values of dropped columns
 * removed, columns the other side added with a default filled in.  For a
 * keyless table the row is its key.
 */
static void
migrate(TableMerge *tm, const WsLayout *side, const uint8_t *v, size_t vl, Buf *out)
{
	WsCell		c[WS_MAX_COLS],
				keep[WS_MAX_COLS];
	int			n = ws_cells(v, vl, c, WS_MAX_COLS),
				nk = 0;
	int			from = tm->Lm->nkey;

	for (int i = 0; i < n; i++)
		if (ws_copy_index(tm->Lm, c[i].tag) >= from)
			keep[nk++] = c[i];
	for (int i = from; i < tm->Lm->nstored; i++)
	{
		const WsCell *have;
		const uint8_t *p;
		size_t		len;

		if (cell_of(keep, nk, tm->Lm->copy[i], &have))
			continue;
		if (added_default(tm, side, tm->Lm->copy[i], &p, &len))
		{
			keep[nk].tag = tm->Lm->copy[i];
			keep[nk].p = p;
			keep[nk].len = len;
			nk++;
		}
	}
	out->len = 0;
	ws_cells_encode(keep, nk, out);
}

typedef struct EditList
{
	ProllyEdit *e;
	Buf		   *b;				/* owned key and value bytes */
	size_t		n,
				cap,
				bcap;
} EditList;

static void
edit_add(EditList *l, const uint8_t *k, size_t kl, const uint8_t *v, size_t vl, bool del)
{
	px_grow((void **) &l->e, &l->cap, l->n + 1, sizeof(ProllyEdit));
	px_grow((void **) &l->b, &l->bcap, l->n + 1, sizeof(Buf));
	memset(&l->b[l->n], 0, sizeof(Buf));
	buf_append(&l->b[l->n], k, kl);
	buf_append(&l->b[l->n], v ? v : (const uint8_t *) "", vl);
	l->e[l->n].klen = kl;
	l->e[l->n].vlen = vl;
	l->e[l->n].del = del;
	l->n++;
}

static PHash
edit_apply(ProllyCtx *ctx, const PHash *root, EditList *l)
{
	PHash		out = *root;

	for (size_t i = 0; i < l->n; i++)
	{
		l->e[i].key = l->b[i].data;
		l->e[i].val = l->b[i].data ? l->b[i].data + l->e[i].klen : NULL;
	}
	if (l->n)
		out = t_apply(ctx, root, l->e, l->n);
	for (size_t i = 0; i < l->n; i++)
		free(l->b[i].data);
	free(l->b);
	free(l->e);
	memset(l, 0, sizeof(*l));
	return out;
}

/* Moves the rows one side changed into the merged layout. */
static PHash
migrate_side(TableMerge *tm, const PHash *merged, const PHash *db, const PHash *dside,
			 const WsLayout *side)
{
	ProllyDiffList dl = {0};
	EditList	el = {0};
	Buf			nv = {0},
				nk = {0};
	PHash		cur = *merged;

	CHECK(prolly_diff_collect(tm->ctx, db, dside, &dl) == PROLLY_OK);
	for (size_t i = 0; i < dl.n; i++)
	{
		const uint8_t *v;
		size_t		vl;

		if (dl.v[i].to == NULL || !t_get(tm->ctx, &cur, dl.v[i].key, dl.v[i].klen, &v, &vl))
			continue;
		if (tm->Lm->nkey > 0)
		{
			migrate(tm, side, v, vl, &nv);
			if (!bytes_eq(nv.data, nv.len, v, vl))
				edit_add(&el, dl.v[i].key, dl.v[i].klen, nv.data, nv.len, false);
		}
		else
		{
			const uint8_t *k = dl.v[i].key,
					   *ov;
			size_t		ovl;
			long		n = count_of(v, vl),
						have;
			char		cnt[24];
			int			cl;

			migrate(tm, side, k + 1, dl.v[i].klen - 1, &nv);
			nk.len = 0;
			buf_append(&nk, "r", 1);
			buf_append(&nk, nv.data, nv.len);
			if (bytes_eq(nk.data, nk.len, k, dl.v[i].klen))
				continue;
			have = t_get(tm->ctx, &cur, nk.data, nk.len, &ov, &ovl) ? count_of(ov, ovl) : 0;
			cl = snprintf(cnt, sizeof(cnt), "%ld", have + n);
			edit_add(&el, k, dl.v[i].klen, NULL, 0, true);
			edit_add(&el, nk.data, nk.len, (const uint8_t *) cnt, (size_t) cl, false);
			cur = edit_apply(tm->ctx, &cur, &el);
		}
	}
	cur = edit_apply(tm->ctx, &cur, &el);
	free(nv.data);
	free(nk.data);
	prolly_diff_list_free(&dl);
	return cur;
}

static PHash
merge_rows(TableMerge *tm, const PHash *db, const PHash *dO, const PHash *dt)
{
	ProllyMergeResult res = {0};
	EditList	el = {0};
	Buf			v = {0};
	PHash		out;

	/*
	 * A keyless table is a multiset: theirs' change in each row's count is
	 * added to ours, so rows both sides inserted are both kept (a merge of
	 * values would see the same new count on both sides and keep one).
	 */
	if (tm->Lm->nkey == 0)
	{
		ProllyDiffList dl = {0};

		CHECK(prolly_diff_collect(tm->ctx, db, dt, &dl) == PROLLY_OK);
		for (size_t i = 0; i < dl.n; i++)
		{
			const uint8_t *ov;
			size_t		ovl;
			long		n = (t_get(tm->ctx, dO, dl.v[i].key, dl.v[i].klen, &ov, &ovl)
							 ? count_of(ov, ovl) : 0) +
				count_of(dl.v[i].to, dl.v[i].tlen) - count_of(dl.v[i].from, dl.v[i].flen);
			char		cnt[24];
			int			cl = snprintf(cnt, sizeof(cnt), "%ld", n);

			edit_add(&el, dl.v[i].key, dl.v[i].klen, (const uint8_t *) cnt, (size_t) cl, n <= 0);
		}
		prolly_diff_list_free(&dl);
		out = edit_apply(tm->ctx, dO, &el);
		out = migrate_side(tm, &out, db, dO, tm->Lo);
		return migrate_side(tm, &out, db, dt, tm->Lt);
	}
	CHECK(prolly_merge(tm->ctx, db, dO, dt, &res) == PROLLY_OK);
	for (size_t i = 0; i < res.nconflicts; i++)
	{
		const ProllyConflict *c = &res.conflicts[i];

		if (c->ours == NULL || c->theirs == NULL)
		{
			char		where[1300],
						kt[200];

			key_text(tm->Lm, c->key, c->klen, kt, sizeof(kt));
			snprintf(where, sizeof(where), "%s %s", tm->name, kt);
			conflict(tm->m, "row", where);
			continue;
		}
		if (merge_cells(tm, c->key, c->klen, c, &v))
			edit_add(&el, c->key, c->klen, v.data, v.len, false);
	}
	out = edit_apply(tm->ctx, &res.root, &el);
	prolly_merge_result_free(&res);
	free(v.data);
	if (tm->m->nconflicts)
		return out;
	out = migrate_side(tm, &out, db, dO, tm->Lo);
	return migrate_side(tm, &out, db, dt, tm->Lt);
}

/* ------------------------------------------------------------------ all */

static int
name_index(const TableList *l, const char *name)
{
	for (size_t i = 0; i < l->n; i++)
		if (strcmp(l->names[i], name) == 0)
			return (int) i;
	return -1;
}

static bool
vt_eq(const VcTable *a, const VcTable *b)
{
	if (a == NULL || b == NULL)
		return a == b;
	return phash_eq(&a->schema, &b->schema) && phash_eq(&a->data, &b->data);
}

void
ws_merge(ProllyCtx *ctx, const PHash *base, const PHash *ours, const PHash *theirs,
		 WsDefaultFn def, void *defarg, WsMerge *m)
{
	TableList	lb,
				lo,
				lt;
	char	  **names = NULL;
	size_t		nn = 0,
				ncap = 0;
	PHash		rv = vc_root_empty(ctx),
				e = t_empty(ctx);

	memset(m, 0, sizeof(*m));
	table_list(ctx, base, &lb);
	table_list(ctx, ours, &lo);
	table_list(ctx, theirs, &lt);
	for (int s = 0; s < 3; s++)
	{
		const TableList *l = s == 0 ? &lo : s == 1 ? &lt : &lb;

		for (size_t i = 0; i < l->n; i++)
		{
			bool		seen = false;

			for (size_t j = 0; j < nn; j++)
				seen |= strcmp(names[j], l->names[i]) == 0;
			if (!seen)
			{
				px_grow((void **) &names, &ncap, nn + 1, sizeof(char *));
				names[nn++] = l->names[i];
			}
		}
	}
	for (size_t i = 0; i < nn; i++)
	{
		const char *name = names[i];
		int			ib = name_index(&lb, name),
					io = name_index(&lo, name),
					it = name_index(&lt, name);
		const VcTable *b = ib >= 0 ? &lb.t[ib] : NULL,
				   *o = io >= 0 ? &lo.t[io] : NULL,
				   *t = it >= 0 ? &lt.t[it] : NULL;
		const VcTable *keep = NULL;
		bool		full = false;

		if (strcmp(name, "~obj") == 0 || strcmp(name, "~seq") == 0)
		{
			PHash		d = strcmp(name, "~obj") == 0
				? merge_objects(ctx, base, ours, theirs, b ? &b->data : &e, o ? &o->data : &e,
								t ? &t->data : &e, m)
				: merge_sequences(ctx, b ? &b->data : &e, o ? &o->data : &e, t ? &t->data : &e);

			rv = put_table(ctx, rv, name, "", 0, &d);
			continue;
		}
		if (vt_eq(o, t))
			keep = o;
		else if (vt_eq(o, b))
			keep = t;
		else if (vt_eq(t, b))
			keep = o;
		else if (o == NULL || t == NULL)
		{
			conflict(m, "table", name);
			continue;
		}
		else
			full = true;
		if (!full)
		{
			if (keep)
			{
				size_t		len;
				char	   *text = schema_text(ctx, &keep->schema, &len);

				rv = put_table(ctx, rv, name, text, len, &keep->data);
				free(text);
			}
			continue;
		}
		{
			WsLayout	Lb,
						Lo,
						Lt,
						Lm;
			bool		hb = b && table_layout(ctx, base, name, &Lb, NULL);
			Buf			lay = {0};
			TableMerge	tm = {0};
			size_t		before = m->nconflicts;
			PHash		d;

			CHECK(table_layout(ctx, ours, name, &Lo, NULL));
			CHECK(table_layout(ctx, theirs, name, &Lt, NULL));
			if (merge_layout(name, hb ? &Lb : NULL, &Lo, &Lt, &lay, m))
			{
				char	   *text = strndup((const char *) lay.data, lay.len);

				layout_must_parse(text, &Lm);
				free(text);
				tm.ctx = ctx;
				tm.name = name;
				tm.Lb = hb ? &Lb : NULL;
				tm.Lo = &Lo;
				tm.Lt = &Lt;
				tm.Lm = &Lm;
				tm.def = def;
				tm.defarg = defarg;
				tm.m = m;
				d = merge_rows(&tm, b ? &b->data : &e, &o->data, &t->data);
				if (m->nconflicts == before)
					rv = put_table(ctx, rv, name, (const char *) lay.data, lay.len, &d);
				for (int q = 0; q < tm.nd; q++)
					free(tm.dval[q].data);
				free(tm.dtag);
				free(tm.dval);
				free(tm.dhas);
				ws_layout_free(&Lm);
			}
			free(lay.data);
			if (hb)
				ws_layout_free(&Lb);
			ws_layout_free(&Lo);
			ws_layout_free(&Lt);
		}
	}
	m->root = rv;
	free(names);
	table_list_free(&lb);
	table_list_free(&lo);
	table_list_free(&lt);
}
