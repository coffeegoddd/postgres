/*-------------------------------------------------------------------------
 *
 * ws_diff.c
 *	  What changed between two root values: schema first, then rows, with columns
 *	  matched by tag.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/ws/ws_diff.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "prolly/ws/ws_diff.h"
#include "prolly/ws/ws_root.h"
#include "prolly/ws/ws_row.h"
#include "prolly/ws/ws_rows.h"
#include "prolly/ws/ws_schema.h"

typedef struct Differ
{
	ProllyCtx  *ctx;
	WsDiffFn	fn;
	void	   *arg;
} Differ;

static void
emit(Differ *d, const WsDiffEvent *e)
{
	d->fn(d->arg, e);
}

static bool
same_text(const char *a, const char *b)
{
	if (a == NULL || b == NULL)
		return a == b;
	return strcmp(a, b) == 0;
}

/* ------------------------------------------------------------ the maps */

typedef struct MapDiff
{
	Differ	   *d;
	WsDiffWhat	what;
} MapDiff;

static void
map_entry(void *arg, const ProllyDiff *x)
{
	MapDiff    *m = arg;
	char	   *key = palloc(x->klen + 1);
	WsDiffEvent e = {.what = m->what, .name = key};

	/* (a foreign key's is its table and its name, apart by a zero byte) */
	for (size_t i = 0; i < x->klen; i++)
		key[i] = x->key[i] == '\0' ? '/' : (char) x->key[i];
	key[x->klen] = '\0';

	e.action = x->from == NULL ? WS_DIFF_ADDED : x->to == NULL ? WS_DIFF_DROPPED : WS_DIFF_CHANGED;
	if (m->what == WS_DIFF_OBJECT)
	{
		char	   *slash = strchr(key, '/');

		if (slash == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("an object's key has no kind: \"%s\"", key)));
		*slash = '\0';
		e.kind = key;
		e.name = slash + 1;
	}
	emit(m->d, &e);
	pfree(key);
}

static void
diff_map(Differ *d, const PHash *from, const PHash *to, WsRootMap which, WsDiffWhat what)
{
	PHash		a,
				b,
				empty;
	bool		has_a = WsRoot->get_map(d->ctx, from, which, &a);
	bool		has_b = WsRoot->get_map(d->ctx, to, which, &b);
	MapDiff		m = {d, what};

	if (!has_a && !has_b)
		return;
	if (prolly_empty(d->ctx, 0, &empty) < 0)
		prolly_report(d->ctx);
	if (prolly_diff(d->ctx, has_a ? &a : &empty, has_b ? &b : &empty, map_entry, &m) < 0)
		prolly_report(d->ctx);
}

/* --------------------------------------------------------- a table's schema */

static void
diff_columns(Differ *d, const WsTableSchema *a, const WsTableSchema *b)
{
	const char *table = b->name;
	int			last = -1;
	bool		moved = false;

	for (int i = 0; i < a->ncols; i++)
	{
		const WsColumn *ca = &a->cols[i];
		const WsColumn *cb = ws_schema_column(b, ca->tag);
		const char *name = cb != NULL ? cb->name : ca->name;

		if (cb == NULL)
		{
			WsDiffEvent e = {.what = WS_DIFF_COLUMN, .action = WS_DIFF_DROPPED, .table = table, .name = name};

			emit(d, &e);
			continue;
		}
		if (strcmp(ca->name, cb->name) != 0)
		{
			WsDiffEvent e = {.what = WS_DIFF_COLUMN_RENAMED, .table = table, .name = ca->name, .to = cb->name};

			emit(d, &e);
		}
		if (strcmp(ca->type, cb->type) != 0)
		{
			WsDiffEvent e = {.what = WS_DIFF_COLUMN_RETYPED, .table = table, .name = name, .from = ca->type, .to = cb->type};

			emit(d, &e);
		}
		if (!same_text(ca->def, cb->def) || ca->kind != cb->kind)
		{
			WsDiffEvent e = {.what = WS_DIFF_COLUMN_DEFAULT, .table = table, .name = name};

			emit(d, &e);
		}
		if (!same_text(ca->not_null, cb->not_null) || ca->identity != cb->identity ||
			!same_text(ca->identity_seq, cb->identity_seq) || !same_text(ca->collation, cb->collation))
		{
			WsDiffEvent e = {.what = WS_DIFF_COLUMN_CHANGED, .table = table, .name = name};

			emit(d, &e);
		}

		/* (in b's order, those both have) */
		{
			int			at = (int) (cb - b->cols);

			moved |= at < last;
			last = at;
		}
	}
	for (int i = 0; i < b->ncols; i++)
		if (ws_schema_column(a, b->cols[i].tag) == NULL)
		{
			WsDiffEvent e = {.what = WS_DIFF_COLUMN, .action = WS_DIFF_ADDED, .table = table, .name = b->cols[i].name};

			emit(d, &e);
		}
	if (moved)
	{
		WsDiffEvent e = {.what = WS_DIFF_COLUMNS_MOVED, .table = table};

		emit(d, &e);
	}
}

static bool
same_key(const WsTableSchema *a, const WsTableSchema *b)
{
	if (a->nkey != b->nkey)
		return false;
	for (int i = 0; i < a->nkey; i++)
		if (a->key[i] != b->key[i])
			return false;
	return true;
}

static bool
same_bytes(const Buf *a, const Buf *b)
{
	return a->len == b->len && (a->len == 0 || memcmp(a->data, b->data, a->len) == 0);
}

/* Two lists by name, in UTF8's order: of constraints or indexes. */
static void
diff_named(Differ *d, WsDiffWhat what, const char *table, int na, int nb,
		   const char *(*name_of) (const WsTableSchema *s, int i),
		   void (*bytes_of) (const WsTableSchema *s, int i, Buf *out),
		   const WsTableSchema *a, const WsTableSchema *b)
{
	int			i = 0,
				j = 0;

	while (i < na || j < nb)
	{
		int			c = i == na ? 1 : j == nb ? -1 : strcmp(name_of(a, i), name_of(b, j));
		WsDiffEvent e = {.what = what, .table = table};

		if (c < 0)
		{
			e.action = WS_DIFF_DROPPED;
			e.name = name_of(a, i++);
			emit(d, &e);
		}
		else if (c > 0)
		{
			e.action = WS_DIFF_ADDED;
			e.name = name_of(b, j++);
			emit(d, &e);
		}
		else
		{
			Buf			ba = BUF_INIT,
						bb = BUF_INIT;

			bytes_of(a, i, &ba);
			bytes_of(b, j, &bb);
			if (!same_bytes(&ba, &bb))
			{
				e.action = WS_DIFF_CHANGED;
				e.name = name_of(b, j);
				emit(d, &e);
			}
			buf_free(&ba);
			buf_free(&bb);
			i++;
			j++;
		}
	}
}

static const char *
constraint_name(const WsTableSchema *s, int i)
{
	return s->constraints[i].name;
}

static void
constraint_bytes(const WsTableSchema *s, int i, Buf *out)
{
	ws_schema_constraint_bytes(&s->constraints[i], out);
}

static const char *
index_name(const WsTableSchema *s, int i)
{
	return s->indexes[i].name;
}

static void
index_bytes(const WsTableSchema *s, int i, Buf *out)
{
	ws_schema_index_bytes(&s->indexes[i], out);
}

static void
diff_schema(Differ *d, const WsTableSchema *a, const WsTableSchema *b)
{
	Buf			sa = BUF_INIT,
				sb = BUF_INIT;

	diff_columns(d, a, b);
	if (!same_key(a, b))
	{
		WsDiffEvent e = {.what = WS_DIFF_KEY, .table = b->name};

		emit(d, &e);
	}
	diff_named(d, WS_DIFF_CONSTRAINT, b->name, a->nconstraints, b->nconstraints,
			   constraint_name, constraint_bytes, a, b);
	diff_named(d, WS_DIFF_INDEX, b->name, a->nindexes, b->nindexes, index_name, index_bytes, a, b);
	ws_schema_settings_bytes(a, &sa);
	ws_schema_settings_bytes(b, &sb);
	if (!same_bytes(&sa, &sb))
	{
		WsDiffEvent e = {.what = WS_DIFF_TABLE, .action = WS_DIFF_CHANGED, .table = b->name};

		emit(d, &e);
	}
	buf_free(&sa);
	buf_free(&sb);
}

/* --------------------------------------------------------- a table's rows */

typedef struct RowCounts
{
	const WsRowShape *a;
	const WsRowShape *b;
	bool		same_columns;	/* both shapes store the same columns */
	uint64		inserted;
	uint64		deleted;
	uint64		updated;
	uint64		rewritten;
	/* a keyless table's whose columns changed: each changed row's change */
	struct Projected *projected;
	size_t		nprojected;
	size_t		capprojected;
} RowCounts;

typedef struct Projected
{
	uint8	   *cells;
	size_t		len;
	int64		change;
} Projected;

/* Whether a keyed row's values, in the columns both sides have, are the same. */
static bool
same_values(const RowCounts *c, const ProllyDiff *x)
{
	const WsRowShape *a = c->a;

	for (int i = a->nkey; i < a->nstored; i++)
	{
		uint64		tag = a->tags[i];
		WsCell		ca,
					cb;
		bool		ha,
					hb;

		if (ws_row_shape_at(c->b, tag) < 0)
			continue;
		ha = WsRowCodec->cell_get(x->from, x->flen, tag, &ca);
		hb = WsRowCodec->cell_get(x->to, x->tlen, tag, &cb);
		if (ha != hb)
			return false;
		if (ha && (ca.large != cb.large || ca.len != cb.len || memcmp(ca.p, cb.p, ca.len) != 0))
			return false;
	}
	return true;
}

static void
keyed_change(void *arg, const ProllyDiff *x)
{
	RowCounts  *c = arg;

	CHECK_FOR_INTERRUPTS();
	if (x->from == NULL)
		c->inserted++;
	else if (x->to == NULL)
		c->deleted++;
	else if (same_values(c, x))
		c->rewritten++;
	else
		c->updated++;
}

/* A keyless row's cells limited to the columns both sides store. */
static void
project(const RowCounts *c, const uint8 *k, size_t kl, Buf *out)
{
	WsCell		cells[WS_MAX_COLS];
	int			n = WsRowCodec->key_cells(k, kl, cells);

	for (int i = 0; i < n; i++)
		if (ws_row_shape_at(c->a, cells[i].tag) >= 0 && ws_row_shape_at(c->b, cells[i].tag) >= 0)
		{
			buf_append_uvarint(out, cells[i].tag);
			buf_append_byte(out, cells[i].large ? 1 : 0);
			buf_append_uvarint(out, cells[i].len);
			buf_append(out, cells[i].p, cells[i].len);
		}
}

static void
keyless_change(void *arg, const ProllyDiff *x)
{
	RowCounts  *c = arg;
	uint64		from = x->from != NULL ? ws_rows_keyless_count(x->key, x->klen, x->from, x->flen) : 0;
	uint64		to = x->to != NULL ? ws_rows_keyless_count(x->key, x->klen, x->to, x->tlen) : 0;

	CHECK_FOR_INTERRUPTS();
	if (c->same_columns)
	{
		if (to > from)
			c->inserted += to - from;
		else
			c->deleted += from - to;
	}
	else
	{
		Projected  *p;
		Buf			cells = BUF_INIT;

		project(c, x->key, x->klen, &cells);
		px_grow((void **) &c->projected, &c->capprojected, c->nprojected + 1, sizeof(Projected));
		p = &c->projected[c->nprojected++];
		p->cells = cells.data;
		p->len = cells.len;
		p->change = (int64) to - (int64) from;
	}
}

static int
projected_cmp(const void *a, const void *b)
{
	const Projected *x = a;
	const Projected *y = b;
	int			c = memcmp(x->cells, y->cells, Min(x->len, y->len));

	if (c != 0)
		return c;
	return (x->len > y->len) - (x->len < y->len);
}

/* Rows that are the same in the columns both sides store are the same rows. */
static void
count_projected(RowCounts *c)
{
	if (c->nprojected > 1)
		qsort(c->projected, c->nprojected, sizeof(Projected), projected_cmp);
	for (size_t i = 0; i < c->nprojected;)
	{
		size_t		j = i;
		int64		net = 0;
		uint64		moved = 0;

		while (j < c->nprojected && projected_cmp(&c->projected[i], &c->projected[j]) == 0)
		{
			net += c->projected[j].change;
			moved += (uint64) (c->projected[j].change < 0 ? -c->projected[j].change : c->projected[j].change);
			j++;
		}
		if (net > 0)
			c->inserted += (uint64) net;
		else
			c->deleted += (uint64) -net;
		c->rewritten += (moved - (uint64) (net < 0 ? -net : net)) / 2;
		i = j;
	}
}

/* How many rows a map holds, a keyless one's each. */
static uint64
rows_in(ProllyCtx *ctx, const PHash *map, bool keyless)
{
	ProllyIter *it;
	const uint8 *k,
			   *v;
	size_t		kl,
				vl;
	uint64		n = 0;
	int			rc;

	if (!keyless)
	{
		if (prolly_count(ctx, map, &n) < 0)
			prolly_report(ctx);
		return n;
	}
	if (prolly_iter_open(ctx, map, NULL, 0, &it) < 0)
		prolly_report(ctx);
	while ((rc = prolly_iter_next(it, &k, &kl, &v, &vl)) > 0)
	{
		CHECK_FOR_INTERRUPTS();
		n += ws_rows_keyless_count(k, kl, v, vl);
	}
	prolly_iter_close(it);
	if (rc < 0)
		prolly_report(ctx);
	return n;
}

static bool
same_stored(const WsRowShape *a, const WsRowShape *b)
{
	if (a->nstored != b->nstored)
		return false;
	for (int i = 0; i < a->nstored; i++)
		if (ws_row_shape_at(b, a->tags[i]) < 0)
			return false;
	return true;
}

static void
diff_rows(Differ *d, const WsTableSchema *a, const PHash *ra, const WsTableSchema *b, const PHash *rb)
{
	WsRowShape	sa,
				sb;
	RowCounts	c = {0};
	const char *table = b->name;
	bool		same = phash_eq(ra, rb);
	WsDiffEvent data = {.what = WS_DIFF_DATA, .table = table, .same = same};
	WsDiffEvent rows = {.what = WS_DIFF_ROWS, .table = table};

	emit(d, &data);
	ws_schema_row_shape(a, &sa);
	ws_schema_row_shape(b, &sb);
	c.a = &sa;
	c.b = &sb;
	c.same_columns = same_stored(&sa, &sb);
	if (same)
		;
	else if (!same_key(a, b))
	{
		c.inserted = rows_in(d->ctx, rb, b->nkey == 0);
		c.deleted = rows_in(d->ctx, ra, a->nkey == 0);
	}
	else if (prolly_diff(d->ctx, ra, rb, a->nkey > 0 ? keyed_change : keyless_change, &c) < 0)
		prolly_report(d->ctx);
	else
		count_projected(&c);
	rows.inserted = c.inserted;
	rows.deleted = c.deleted;
	rows.updated = c.updated;
	emit(d, &rows);
	if (c.rewritten > 0)
	{
		WsDiffEvent e = {.what = WS_DIFF_REWRITTEN, .table = table, .rewritten = c.rewritten};

		emit(d, &e);
	}
	for (size_t i = 0; i < c.nprojected; i++)
		px_free(c.projected[i].cells);
	px_free(c.projected);
	ws_row_shape_free(&sa);
	ws_row_shape_free(&sb);
}

/* --------------------------------------------------------------- tables */

typedef struct TableNames
{
	char	  **names;
	int			n;
	size_t		cap;
} TableNames;

static bool
table_name(void *arg, const char *name, const VcTable *entry)
{
	TableNames *t = arg;

	px_grow((void **) &t->names, &t->cap, (size_t) t->n + 1, sizeof(char *));
	t->names[t->n++] = pstrdup(name);
	return true;
}

static void
tags_diff(ProllyCtx *ctx, const PHash *from, const PHash *to, WsDiffFn fn, void *arg)
{
	Differ		d = {ctx, fn, arg};
	TableNames	a = {0},
				b = {0};
	int			i = 0,
				j = 0;

	diff_map(&d, from, to, WS_ROOT_OBJECTS, WS_DIFF_OBJECT);
	diff_map(&d, from, to, WS_ROOT_ROLES, WS_DIFF_ROLE);
	diff_map(&d, from, to, WS_ROOT_FOREIGN_KEYS, WS_DIFF_FOREIGN_KEY);
	diff_map(&d, from, to, WS_ROOT_SEQUENCES, WS_DIFF_SEQUENCE);
	WsRoot->tables(ctx, from, table_name, &a);
	WsRoot->tables(ctx, to, table_name, &b);
	while (i < a.n || j < b.n)
	{
		int			c = i == a.n ? 1 : j == b.n ? -1 : strcmp(a.names[i], b.names[j]);
		WsDiffEvent e = {.what = WS_DIFF_TABLE};

		CHECK_FOR_INTERRUPTS();
		if (c < 0)
		{
			e.action = WS_DIFF_DROPPED;
			e.table = a.names[i++];
			fn(arg, &e);
		}
		else if (c > 0)
		{
			e.action = WS_DIFF_ADDED;
			e.table = b.names[j++];
			fn(arg, &e);
		}
		else
		{
			WsTableSchema sa,
						sb;
			PHash		ra,
						rb;

			if (!WsRoot->get_table(ctx, from, a.names[i], &sa, &ra) ||
				!WsRoot->get_table(ctx, to, b.names[j], &sb, &rb))
				elog(ERROR, "a table listed is not there");
			diff_schema(&d, &sa, &sb);
			diff_rows(&d, &sa, &ra, &sb, &rb);
			ws_schema_free(&sa);
			ws_schema_free(&sb);
			i++;
			j++;
		}
	}
}

const WsDiffRoutine TagsWsDiff = {
	.name = "tags",
	.diff = tags_diff,
};

const WsDiffRoutine *WsDiff = &TagsWsDiff;

/* ---------------------------------------------------------------- lines */

static const char *
action_word(WsDiffAction action)
{
	return action == WS_DIFF_ADDED ? "added" : action == WS_DIFF_DROPPED ? "dropped" : "changed";
}

void
ws_diff_line(const WsDiffEvent *e, Buf *out)
{
	StringInfoData s;

	initStringInfo(&s);
	switch (e->what)
	{
		case WS_DIFF_OBJECT:
			appendStringInfo(&s, "object %s %s %s", action_word(e->action), e->kind, e->name);
			break;
		case WS_DIFF_FOREIGN_KEY:
			appendStringInfo(&s, "foreign key %s %s", action_word(e->action), e->name);
			break;
		case WS_DIFF_SEQUENCE:
			appendStringInfo(&s, "sequence %s %s", action_word(e->action), e->name);
			break;
		case WS_DIFF_ROLE:
			appendStringInfo(&s, "role %s %s", action_word(e->action), e->name);
			break;
		case WS_DIFF_TABLE:
			appendStringInfo(&s, "table %s %s", action_word(e->action), e->table);
			break;
		case WS_DIFF_COLUMN:
			appendStringInfo(&s, "column %s %s.%s", action_word(e->action), e->table, e->name);
			break;
		case WS_DIFF_COLUMN_RENAMED:
			appendStringInfo(&s, "column renamed %s.%s %s", e->table, e->name, e->to);
			break;
		case WS_DIFF_COLUMN_RETYPED:
			appendStringInfo(&s, "column retyped %s.%s %s -> %s", e->table, e->name, e->from, e->to);
			break;
		case WS_DIFF_COLUMN_DEFAULT:
			appendStringInfo(&s, "column default %s.%s", e->table, e->name);
			break;
		case WS_DIFF_COLUMN_CHANGED:
			appendStringInfo(&s, "column changed %s.%s", e->table, e->name);
			break;
		case WS_DIFF_COLUMNS_MOVED:
			appendStringInfo(&s, "column moved %s", e->table);
			break;
		case WS_DIFF_KEY:
			appendStringInfo(&s, "key changed %s", e->table);
			break;
		case WS_DIFF_CONSTRAINT:
			appendStringInfo(&s, "constraint %s %s %s", action_word(e->action), e->table, e->name);
			break;
		case WS_DIFF_INDEX:
			appendStringInfo(&s, "index %s %s %s", action_word(e->action), e->table, e->name);
			break;
		case WS_DIFF_DATA:
			appendStringInfo(&s, "data %s %s", e->table, e->same ? "same" : "changed");
			break;
		case WS_DIFF_ROWS:
			appendStringInfo(&s, "rows %s +" UINT64_FORMAT " -" UINT64_FORMAT " ~" UINT64_FORMAT,
							 e->table, e->inserted, e->deleted, e->updated);
			break;
		case WS_DIFF_REWRITTEN:
			appendStringInfo(&s, "rewritten %s " UINT64_FORMAT, e->table, e->rewritten);
			break;
	}
	buf_append(out, s.data, s.len);
	pfree(s.data);
}
