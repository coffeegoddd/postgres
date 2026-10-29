/*
 * ws_diff.c
 *	  What changed between two root values, schema first and then rows,
 *	  with columns matched by tag.
 *
 * Lines, so tests can name what they expect:
 *	object added|dropped|changed <type> <name>
 *	table added|dropped <table>
 *	column added|dropped <table>.<name>
 *	column renamed <table>.<old> <new>
 *	column retyped <table>.<name> <old type> -> <new type>
 *	column default <table>.<name>
 *	key changed <table>
 *	data <table> same|changed				whether the row map is the same tree
 *	rows <table> +<inserted> -<deleted> ~<updated>
 *	rewritten <table> <n>					rows whose bytes changed but whose values,
 *											in the columns both sides have, did not
 * A row counts as updated only if a value in a column both sides have
 * changed: renaming a column changes no row.
 */
#include "ws.h"

static void
add_line(WsDiff *d, const char *fmt, const char *a, const char *b, const char *c,
		 const char *e)
{
	char		line[2048];

	snprintf(line, sizeof(line), fmt, a, b, c, e);
	px_grow((void **) &d->line, &d->cap, d->n + 1, sizeof(char *));
	d->line[d->n++] = strdup(line);
}

bool
ws_diff_has(const WsDiff *d, const char *line)
{
	for (size_t i = 0; i < d->n; i++)
		if (strcmp(d->line[i], line) == 0)
			return true;
	return false;
}

void
ws_diff_free(WsDiff *d)
{
	for (size_t i = 0; i < d->n; i++)
		free(d->line[i]);
	free(d->line);
	memset(d, 0, sizeof(*d));
}

static int
find(const TableList *l, const char *name)
{
	for (size_t i = 0; i < l->n; i++)
		if (strcmp(l->names[i], name) == 0)
			return (int) i;
	return -1;
}

static void
diff_objects(ProllyCtx *ctx, const PHash *a, const PHash *b, WsDiff *d)
{
	ProllyDiffList dl = {0};

	CHECK(prolly_diff_collect(ctx, a, b, &dl) == PROLLY_OK);
	for (size_t i = 0; i < dl.n; i++)
	{
		char		key[1100],
				   *type,
				   *name;

		snprintf(key, sizeof(key), "%.*s", (int) dl.v[i].klen, (const char *) dl.v[i].key);
		if (key[0] == '0')
			continue;			/* the prelude */
		type = key + 2;
		name = strchr(type, '\t');
		*name++ = '\0';
		add_line(d, "object %s %s %s%s", dl.v[i].from == NULL ? "added" :
				 dl.v[i].to == NULL ? "dropped" : "changed", type, name, "");
	}
	prolly_diff_list_free(&dl);
}

static bool
same_cells(const WsLayout *A, const WsLayout *B, const uint8_t *a, size_t al,
		   const uint8_t *b, size_t bl)
{
	/* Values in the stored columns both layouts have. */
	for (int i = A->nkey; i < A->nstored; i++)
	{
		uint64_t	tag = A->copy[i];
		const uint8_t *pa = NULL,
				   *pb = NULL;
		size_t		la = 0,
					lb = 0;
		bool		ha,
					hb;

		if (ws_copy_index(B, tag) < B->nkey)
			continue;
		ha = ws_cell_get(a, al, tag, &pa, &la);
		hb = ws_cell_get(b, bl, tag, &pb, &lb);
		if (ha != hb || (ha && !bytes_eq(pa, la, pb, lb)))
			return false;
	}
	return true;
}

/* A keyless row's cells limited to the columns both layouts store. */
static void
project(const WsLayout *A, const WsLayout *B, const uint8_t *k, size_t kl, Buf *out)
{
	WsCell		c[WS_MAX_COLS];
	int			n = ws_cells(k + 1, kl - 1, c, WS_MAX_COLS),
				m = 0;

	for (int i = 0; i < n; i++)
		if (ws_copy_index(A, c[i].tag) >= 0 && ws_copy_index(B, c[i].tag) >= 0)
			c[m++] = c[i];
	out->len = 0;
	ws_cells_encode(c, m, out);
}

typedef struct Proj
{
	uint8_t    *b;
	size_t		len;
	long		n;
} Proj;

static int
proj_cmp(const void *a, const void *b)
{
	const Proj *x = a,
			   *y = b;

	return key_cmp(x->b, x->len, y->b, y->len);
}

static void
diff_rows(ProllyCtx *ctx, const char *name, const WsLayout *A, const PHash *da,
		  const WsLayout *B, const PHash *db, WsDiff *d)
{
	ProllyDiffList dl = {0};
	uint64_t	ins = 0,
				del = 0,
				upd = 0,
				rew = 0;
	char		num[4][32];

	add_line(d, "data %s %s%s%s", name, phash_eq(da, db) ? "same" : "changed", "", "");
	if (phash_eq(da, db))
	{
		add_line(d, "rows %s +0 -0 ~0%s%s%s", name, "", "", "");
		return;
	}
	CHECK(prolly_diff_collect(ctx, da, db, &dl) == PROLLY_OK);
	if (A->nkey > 0)
	{
		for (size_t i = 0; i < dl.n; i++)
		{
			const ProllyDiff *x = &dl.v[i];

			if (x->from == NULL)
				ins++;
			else if (x->to == NULL)
				del++;
			else if (same_cells(A, B, x->from, x->flen, x->to, x->tlen))
				rew++;
			else
				upd++;
		}
	}
	else
	{
		/* Keyless rows: cancel what both sides have once projected. */
		Proj	   *p = px_malloc((dl.n ? dl.n : 1) * sizeof(Proj));
		Buf			b = {0};
		size_t		np = 0;

		for (size_t i = 0; i < dl.n; i++)
		{
			const ProllyDiff *x = &dl.v[i];
			char		cnt[24];
			long		from = 0,
						to = 0;

			if (x->from)
			{
				snprintf(cnt, sizeof(cnt), "%.*s", (int) x->flen, (const char *) x->from);
				from = atol(cnt);
			}
			if (x->to)
			{
				snprintf(cnt, sizeof(cnt), "%.*s", (int) x->tlen, (const char *) x->to);
				to = atol(cnt);
			}
			project(A, B, x->key, x->klen, &b);
			p[np].b = px_memdup(b.data ? b.data : (uint8_t *) "", b.len + 1);
			p[np].len = b.len;
			p[np].n = to - from;
			np++;
		}
		qsort(p, np, sizeof(Proj), proj_cmp);
		for (size_t i = 0; i < np;)
		{
			size_t		j = i;
			long		net = 0;
			uint64_t	moved = 0;

			while (j < np && proj_cmp(&p[i], &p[j]) == 0)
			{
				net += p[j].n;
				moved += (uint64_t) labs(p[j].n);
				j++;
			}
			if (net > 0)
				ins += (uint64_t) net;
			else
				del += (uint64_t) -net;
			rew += (moved - (uint64_t) labs(net)) / 2;
			for (size_t q = i; q < j; q++)
				free(p[q].b);
			i = j;
		}
		free(p);
		free(b.data);
	}
	snprintf(num[0], 32, "%" PRIu64, ins);
	snprintf(num[1], 32, "%" PRIu64, del);
	snprintf(num[2], 32, "%" PRIu64, upd);
	add_line(d, "rows %s +%s -%s ~%s", name, num[0], num[1], num[2]);
	if (rew)
	{
		snprintf(num[3], 32, "%" PRIu64, rew);
		add_line(d, "rewritten %s %s%s%s", name, num[3], "", "");
	}
	prolly_diff_list_free(&dl);
}

static void
diff_columns(const char *name, const WsLayout *A, const WsLayout *B, WsDiff *d)
{
	char		who[1200];

	for (int i = 0; i < A->ncols; i++)
	{
		const WsCol *a = &A->cols[i],
				   *b = ws_layout_col(B, a->tag);

		snprintf(who, sizeof(who), "%s.%s", name, a->name);
		if (b == NULL)
		{
			add_line(d, "column dropped %s%s%s%s", who, "", "", "");
			continue;
		}
		if (strcmp(a->name, b->name) != 0)
			add_line(d, "column renamed %s %s%s%s", who, b->name, "", "");
		snprintf(who, sizeof(who), "%s.%s", name, b->name);
		if (strcmp(a->type, b->type) != 0)
			add_line(d, "column retyped %s %s -> %s%s", who, a->type, b->type, "");
		if (strcmp(a->def, b->def) != 0 || a->kind != b->kind)
			add_line(d, "column default %s%s%s%s", who, "", "", "");
	}
	for (int i = 0; i < B->ncols; i++)
		if (ws_layout_col(A, B->cols[i].tag) == NULL)
		{
			snprintf(who, sizeof(who), "%s.%s", name, B->cols[i].name);
			add_line(d, "column added %s%s%s%s", who, "", "", "");
		}
	if (A->nkey != B->nkey || (A->nkey && memcmp(A->key, B->key, (size_t) A->nkey * sizeof(uint64_t))))
		add_line(d, "key changed %s%s%s%s", name, "", "", "");
}

void
ws_diff(ProllyCtx *ctx, const PHash *from, const PHash *to, WsDiff *d)
{
	TableList	a,
				b;

	memset(d, 0, sizeof(*d));
	table_list(ctx, from, &a);
	table_list(ctx, to, &b);
	{
		int			ia = find(&a, "~obj"),
					ib = find(&b, "~obj");
		PHash		e = t_empty(ctx);

		diff_objects(ctx, ia >= 0 ? &a.t[ia].data : &e, ib >= 0 ? &b.t[ib].data : &e, d);
	}
	for (size_t i = 0; i < a.n; i++)
		if (a.names[i][0] != '~' && find(&b, a.names[i]) < 0)
			add_line(d, "table dropped %s%s%s%s", a.names[i], "", "", "");
	for (size_t j = 0; j < b.n; j++)
	{
		int			i = find(&a, b.names[j]);
		WsLayout	A,
					B;
		PHash		da,
					db;

		if (b.names[j][0] == '~')
			continue;
		if (i < 0)
		{
			add_line(d, "table added %s%s%s%s", b.names[j], "", "", "");
			continue;
		}
		CHECK(table_layout(ctx, from, a.names[i], &A, &da));
		CHECK(table_layout(ctx, to, b.names[j], &B, &db));
		diff_columns(b.names[j], &A, &B, d);
		diff_rows(ctx, b.names[j], &A, &da, &B, &db, d);
		ws_layout_free(&A);
		ws_layout_free(&B);
	}
	table_list_free(&a);
	table_list_free(&b);
}
