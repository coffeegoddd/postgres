/*-------------------------------------------------------------------------
 *
 * ws_merge.c
 *	  Three-way merge of root values: objects by key, tables field by field.
 *
 * A merged record is made of what the three records hold, not copied, for as long
 * as it takes to write it.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/ws/ws_merge.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "prolly/ws/ws_fk.h"
#include "prolly/ws/ws_merge.h"
#include "prolly/ws/ws_obj.h"
#include "prolly/ws/ws_root.h"
#include "prolly/ws/ws_row.h"
#include "prolly/ws/ws_schema.h"

typedef struct Merger
{
	ProllyCtx  *ctx;
	char	  **conflicts;
	int			nconflicts;
	int			cap;
} Merger;

static void
conflict(Merger *m, const char *kind, const char *where)
{
	char	   *line = psprintf("%s %s", kind, where);

	for (int i = 0; i < m->nconflicts; i++)
		if (strcmp(m->conflicts[i], line) == 0)
		{
			pfree(line);
			return;
		}
	if (m->nconflicts == m->cap)
	{
		m->cap = Max(16, m->cap * 2);
		m->conflicts = m->conflicts == NULL ? palloc_array(char *, m->cap) :
			repalloc_array(m->conflicts, char *, m->cap);
	}
	m->conflicts[m->nconflicts++] = line;
}

/* ------------------------------------------------- the three-way choice */

/* An item of a record's list: what it is of, and all it holds, as bytes. */
typedef struct Item
{
	char	   *key;
	size_t		keylen;
	uint8	   *bytes;
	size_t		len;
	void	   *what;
} Item;

typedef struct Items
{
	Item	   *v;
	int			n;
} Items;

static const Item *
item_of(const Items *l, const Item *like)
{
	if (l == NULL)
		return NULL;
	for (int i = 0; i < l->n; i++)
		if (l->v[i].keylen == like->keylen && memcmp(l->v[i].key, like->key, like->keylen) == 0)
			return &l->v[i];
	return NULL;
}

static bool
item_same(const Item *a, const Item *b)
{
	if (a == NULL || b == NULL)
		return a == b;
	return a->len == b->len && (a->len == 0 || memcmp(a->bytes, b->bytes, a->len) == 0);
}

/*
 * The one to keep, NULL if none; *clash if both changed it differently, a drop being
 * a change.
 */
static const Item *
pick(const Item *b, const Item *o, const Item *t, bool *clash)
{
	*clash = false;
	if (item_same(o, t))
		return o;
	if (item_same(o, b))
		return t;
	if (item_same(t, b))
		return o;
	*clash = true;
	return NULL;
}

/*
 * Each item of ours and each new one of theirs, as pick says, in what, in that
 * order; on a clash, clashed is told of it.  How many.
 */
static int
merge_items(const Items *b, const Items *o, const Items *t, void **what,
			void (*clashed) (void *arg, const Item *ours, const Item *theirs), void *arg)
{
	int			n = 0;

	for (int pass = 0; pass < 2; pass++)
	{
		const Items *from = pass == 0 ? o : t;

		for (int i = 0; i < from->n; i++)
		{
			const Item *like = &from->v[i];
			const Item *io = item_of(o, like);
			const Item *it = item_of(t, like);
			const Item *keep;
			bool		clash;

			if (pass == 1 && io != NULL)
				continue;		/* (seen in the first) */
			keep = pick(item_of(b, like), io, it, &clash);
			if (clash)
				clashed(arg, io, it);
			else if (keep != NULL)
				what[n++] = keep->what;
		}
	}
	return n;
}

static void
item_set(Item *it, const char *key, size_t keylen, Buf *bytes, void *what)
{
	it->key = palloc(Max(keylen, 1));
	memcpy(it->key, key, keylen);
	it->keylen = keylen;
	it->bytes = bytes->data;
	it->len = bytes->len;
	it->what = what;
}

/* ---------------------------------------------------- a record's lists */

static void
put_text(Buf *out, const char *s)
{
	if (s == NULL)
		buf_append_byte(out, 0);
	else
	{
		buf_append_byte(out, 1);
		buf_append(out, s, strlen(s) + 1);
	}
}

static Items
column_items(const WsTableSchema *s)
{
	Items		l = {palloc0_array(Item, Max(s->ncols, 1)), s->ncols};

	for (int i = 0; i < s->ncols; i++)
	{
		WsColumn   *c = &s->cols[i];
		Buf			bytes = BUF_INIT;
		WsTableSchema one = {.ncols = 1, .cols = c, .replica_identity = WS_REPLICA_DEFAULT};

		put_text(&bytes, c->name);
		put_text(&bytes, c->type);
		buf_append_byte(&bytes, (uint8) c->kind);
		put_text(&bytes, c->not_null);
		buf_append_byte(&bytes, (uint8) c->identity);
		put_text(&bytes, c->identity_seq);
		put_text(&bytes, c->def);
		put_text(&bytes, c->collation);
		ws_schema_settings_bytes(&one, &bytes);
		item_set(&l.v[i], (const char *) &c->tag, sizeof(c->tag), &bytes, c);
	}
	return l;
}

static Items
constraint_items(const WsTableSchema *s)
{
	Items		l = {palloc0_array(Item, Max(s->nconstraints, 1)), s->nconstraints};

	for (int i = 0; i < s->nconstraints; i++)
	{
		Buf			bytes = BUF_INIT;

		ws_schema_constraint_bytes(&s->constraints[i], &bytes);
		item_set(&l.v[i], s->constraints[i].name, strlen(s->constraints[i].name), &bytes, &s->constraints[i]);
	}
	return l;
}

static Items
index_items(const WsTableSchema *s)
{
	Items		l = {palloc0_array(Item, Max(s->nindexes, 1)), s->nindexes};

	for (int i = 0; i < s->nindexes; i++)
	{
		Buf			bytes = BUF_INIT;

		ws_schema_index_bytes(&s->indexes[i], &bytes);
		item_set(&l.v[i], s->indexes[i].name, strlen(s->indexes[i].name), &bytes, &s->indexes[i]);
	}
	return l;
}

static Items
index_settings_items(const WsTableSchema *s)
{
	Items		l = {palloc0_array(Item, Max(s->nindex_settings, 1)), s->nindex_settings};

	for (int i = 0; i < s->nindex_settings; i++)
	{
		Buf			bytes = BUF_INIT;
		WsTableSchema one = {.nindex_settings = 1, .index_settings = &s->index_settings[i], .replica_identity = WS_REPLICA_DEFAULT};

		ws_schema_settings_bytes(&one, &bytes);
		item_set(&l.v[i], s->index_settings[i].index, strlen(s->index_settings[i].index), &bytes, &s->index_settings[i]);
	}
	return l;
}

static Items
option_items(const WsTableSchema *s)
{
	Items		l = {palloc0_array(Item, Max(s->noptions, 1)), s->noptions};

	for (int i = 0; i < s->noptions; i++)
	{
		Buf			bytes = BUF_INIT;
		char	   *key = psprintf("%d%s", s->options[i].toast ? 1 : 0, s->options[i].name);

		put_text(&bytes, s->options[i].value);
		item_set(&l.v[i], key, strlen(key), &bytes, &s->options[i]);
	}
	return l;
}

static Items
comment_items(const WsTableSchema *s)
{
	Items		l = {palloc0_array(Item, Max(s->ncomments, 1)), s->ncomments};

	for (int i = 0; i < s->ncomments; i++)
	{
		const WsComment *c = &s->comments[i];
		Buf			bytes = BUF_INIT;
		char	   *key = psprintf("%c" UINT64_FORMAT "/%s", c->kind, c->tag, c->name != NULL ? c->name : "");

		put_text(&bytes, c->text);
		item_set(&l.v[i], key, strlen(key), &bytes, &s->comments[i]);
	}
	return l;
}

static Items
grant_items(const WsTableSchema *s)
{
	Items		l = {palloc0_array(Item, Max(s->ngrants, 1)), s->ngrants};

	for (int i = 0; i < s->ngrants; i++)
	{
		const WsGrant *g = &s->grants[i];
		Buf			bytes = BUF_INIT;
		char	   *key = psprintf("%c" UINT64_FORMAT "/%d/%s", g->target, g->tag, g->grant_option ? 1 : 0,
								   g->grantee != NULL ? g->grantee : "");

		put_text(&bytes, g->privileges);
		item_set(&l.v[i], key, strlen(key), &bytes, &s->grants[i]);
	}
	return l;
}

static Items
inherited_col_items(const WsTableSchema *s)
{
	Items		l = {palloc0_array(Item, Max(s->ninherited_cols, 1)), s->ninherited_cols};

	for (int i = 0; i < s->ninherited_cols; i++)
	{
		Buf			bytes = BUF_INIT;

		item_set(&l.v[i], (const char *) &s->inherited_cols[i], sizeof(uint64), &bytes, &s->inherited_cols[i]);
	}
	return l;
}

static Items
inherited_con_items(const WsTableSchema *s)
{
	Items		l = {palloc0_array(Item, Max(s->ninherited_cons, 1)), s->ninherited_cons};

	for (int i = 0; i < s->ninherited_cons; i++)
	{
		Buf			bytes = BUF_INIT;

		item_set(&l.v[i], s->inherited_cons[i], strlen(s->inherited_cons[i]), &bytes, &s->inherited_cons[i]);
	}
	return l;
}

/* ------------------------------------------------------ a merged record */

typedef struct TableMerge
{
	Merger	   *m;
	const char *table;
	const WsTableSchema *o;
	const WsTableSchema *t;
	bool		ok;
} TableMerge;

static void
column_clashed(void *arg, const Item *ours, const Item *theirs)
{
	TableMerge *tm = arg;
	const WsColumn *c = ours != NULL ? ours->what : theirs->what;
	char	   *where = psprintf("%s.%s", tm->table, c->name);

	conflict(tm->m, "column", where);
	pfree(where);
	tm->ok = false;
}

static void
other_clashed(void *arg, const Item *ours, const Item *theirs)
{
	TableMerge *tm = arg;

	conflict(tm->m, "table-ddl", tm->table);
	tm->ok = false;
}

static int
constraint_cmp(const void *a, const void *b)
{
	return strcmp(((const WsConstraint *) a)->name, ((const WsConstraint *) b)->name);
}

static int
index_cmp(const void *a, const void *b)
{
	return strcmp(((const WsIndex *) a)->name, ((const WsIndex *) b)->name);
}

static int
index_settings_cmp(const void *a, const void *b)
{
	return strcmp(((const WsIndexSettings *) a)->index, ((const WsIndexSettings *) b)->index);
}

static int
name_cmp(const void *a, const void *b)
{
	return strcmp(*(char *const *) a, *(char *const *) b);
}

static int
option_cmp(const void *a, const void *b)
{
	const WsOption *x = a;
	const WsOption *y = b;

	if (x->toast != y->toast)
		return x->toast ? 1 : -1;
	return strcmp(x->name, y->name);
}

static int
comment_cmp(const void *a, const void *b)
{
	const WsComment *x = a;
	const WsComment *y = b;

	if (x->kind != y->kind)
		return (x->kind > y->kind) - (x->kind < y->kind);
	if (x->tag != y->tag)
		return (x->tag > y->tag) - (x->tag < y->tag);
	if (x->name == NULL || y->name == NULL)
		return (x->name != NULL) - (y->name != NULL);
	return strcmp(x->name, y->name);
}

static int
grant_cmp(const void *a, const void *b)
{
	const WsGrant *x = a;
	const WsGrant *y = b;

	if (x->target != y->target)
		return (x->target > y->target) - (x->target < y->target);
	if (x->tag != y->tag)
		return (x->tag > y->tag) - (x->tag < y->tag);
	if (x->grantee == NULL || y->grantee == NULL)
	{
		if (x->grantee != y->grantee)
			return x->grantee == NULL ? -1 : 1;
	}
	else if (strcmp(x->grantee, y->grantee) != 0)
		return strcmp(x->grantee, y->grantee);
	return (x->grant_option > y->grant_option) - (x->grant_option < y->grant_option);
}

/* What merge_items chose, as an array of its own, sorted if cmp. */
static void *
chosen(void **what, int n, size_t size, int (*cmp) (const void *, const void *))
{
	char	   *out = palloc0(Max((size_t) n, 1) * size);

	for (int i = 0; i < n; i++)
		memcpy(out + (size_t) i * size, what[i], size);
	if (cmp != NULL && n > 1)
		qsort(out, (size_t) n, size, cmp);
	return out;
}

#define MERGE_LIST(items_of, count, list, type, cmp) \
	do { \
		Items		lo = items_of(o); \
		Items		lt = items_of(t); \
		Items		lb = b != NULL ? items_of(b) : (Items) {NULL, 0}; \
		void	  **what = palloc_array(void *, lo.n + lt.n + 1); \
		\
		out->count = merge_items(&lb, &lo, &lt, what, other_clashed, tm); \
		out->list = chosen(what, out->count, sizeof(type), cmp); \
	} while (0)

/* One setting or set of them, in a record of nothing else, as bytes. */
static void
group_bytes(const WsTableSchema *s, void (*copy) (WsTableSchema *to, const WsTableSchema *from), Buf *out)
{
	WsTableSchema one = {.replica_identity = WS_REPLICA_DEFAULT};

	copy(&one, s);
	ws_schema_settings_bytes(&one, out);
}

/* Each is a setting or set that only makes sense whole. */
static void
copy_partitioning(WsTableSchema *to, const WsTableSchema *from)
{
	to->partition = from->partition;
	to->npartition = from->npartition;
	to->partition_keys = from->partition_keys;
}

static void
copy_partition_of(WsTableSchema *to, const WsTableSchema *from)
{
	to->partition_of = from->partition_of;
	to->partition_bound = from->partition_bound;
}

static void
copy_inherits(WsTableSchema *to, const WsTableSchema *from)
{
	to->ninherits = from->ninherits;
	to->inherits = from->inherits;
}

static void
copy_key_timing(WsTableSchema *to, const WsTableSchema *from)
{
	to->key_deferrable = from->key_deferrable;
	to->key_initdeferred = from->key_initdeferred;
}

static void
copy_replica(WsTableSchema *to, const WsTableSchema *from)
{
	to->replica_identity = from->replica_identity;
	to->replica_index = from->replica_index;
}

static void
copy_clustered(WsTableSchema *to, const WsTableSchema *from)
{
	to->clustered = from->clustered;
}

static void
copy_row_security(WsTableSchema *to, const WsTableSchema *from)
{
	to->row_security = from->row_security;
	to->force_row_security = from->force_row_security;
}

static void
copy_owner(WsTableSchema *to, const WsTableSchema *from)
{
	to->owner = from->owner;
}

static void
merge_group(TableMerge *tm, const WsTableSchema *b, const WsTableSchema *o, const WsTableSchema *t,
			void (*copy) (WsTableSchema *to, const WsTableSchema *from), WsTableSchema *out)
{
	Buf			bb = BUF_INIT,
				bo = BUF_INIT,
				bt = BUF_INIT;
	Item		ib = {0},
				io = {0},
				it = {0};
	const Item *keep;
	bool		clash;

	if (b != NULL)
		group_bytes(b, copy, &bb);
	group_bytes(o, copy, &bo);
	group_bytes(t, copy, &bt);
	ib.bytes = bb.data;
	ib.len = bb.len;
	ib.what = (void *) b;
	io.bytes = bo.data;
	io.len = bo.len;
	io.what = (void *) o;
	it.bytes = bt.data;
	it.len = bt.len;
	it.what = (void *) t;
	keep = pick(b != NULL ? &ib : NULL, &io, &it, &clash);
	if (clash)
	{
		conflict(tm->m, "table-ddl", tm->table);
		tm->ok = false;
	}
	else
		copy(out, keep->what);
	buf_free(&bb);
	buf_free(&bo);
	buf_free(&bt);
}

static bool
same_key_columns(const WsTableSchema *a, const WsTableSchema *b)
{
	if (a->nkey != b->nkey)
		return false;
	for (int i = 0; i < a->nkey; i++)
		if (a->key[i] != b->key[i])
			return false;
	return true;
}

static void
merge_key_name(TableMerge *tm, const WsTableSchema *b, const WsTableSchema *o, const WsTableSchema *t,
			   WsTableSchema *out)
{
	const char *base = b != NULL ? b->key_name : NULL;

	if (o->key_name == NULL || t->key_name == NULL || strcmp(o->key_name, t->key_name) == 0)
		out->key_name = o->key_name;
	else if (base != NULL && strcmp(o->key_name, base) == 0)
		out->key_name = t->key_name;
	else if (base != NULL && strcmp(t->key_name, base) == 0)
		out->key_name = o->key_name;
	else
	{
		conflict(tm->m, "table-ddl", tm->table);
		tm->ok = false;
	}
}

/* Those inherited that are still columns, in the order of the columns. */
static void
order_inherited_cols(WsTableSchema *out)
{
	uint64	   *in_order = palloc_array(uint64, Max(out->ninherited_cols, 1));
	int			n = 0;

	for (int c = 0; c < out->ncols; c++)
		for (int i = 0; i < out->ninherited_cols; i++)
			if (out->inherited_cols[i] == out->cols[c].tag)
				in_order[n++] = out->cols[c].tag;
	out->inherited_cols = in_order;
	out->ninherited_cols = n;
}

/*
 * An index on a column dropped goes with it, with what is said of it: a record of
 * the merged columns and that index alone says so.
 */
static void
drop_indexes_of_dropped(WsTableSchema *out)
{
	WsTableSchema columns_only = {.name = out->name, .ncols = out->ncols, .cols = out->cols,
		.key_name = out->key_name, .nkey = out->nkey, .key = out->key,
	.replica_identity = WS_REPLICA_DEFAULT};
	int			kept = 0;

	if (ws_schema_problem(&columns_only) != NULL)
		return;
	for (int i = 0; i < out->nindexes; i++)
	{
		WsTableSchema one = columns_only;

		one.indexes = &out->indexes[i];
		one.nindexes = 1;
		if (ws_schema_problem(&one) == NULL)
		{
			out->indexes[kept++] = out->indexes[i];
			continue;
		}
		for (int s = 0; s < out->nindex_settings; s++)
			if (strcmp(out->index_settings[s].index, out->indexes[i].name) == 0)
			{
				out->index_settings[s] = out->index_settings[--out->nindex_settings];
				qsort(out->index_settings, out->nindex_settings, sizeof(WsIndexSettings), index_settings_cmp);
				break;
			}
		for (int c = 0; c < out->ncomments; c++)
			if (out->comments[c].name != NULL && strcmp(out->comments[c].name, out->indexes[i].name) == 0 &&
				out->comments[c].kind != WS_COMMENT_COLUMN && out->comments[c].kind != WS_COMMENT_SEQUENCE)
			{
				memmove(&out->comments[c], &out->comments[c + 1], (out->ncomments - c - 1) * sizeof(WsComment));
				out->ncomments--;
				break;
			}
	}
	out->nindexes = kept;
}

/* A merged record of two sides' and their base's, if there is one; false, in conflict. */
static bool
merge_record(Merger *m, const WsTableSchema *b, const WsTableSchema *o, const WsTableSchema *t,
			 WsTableSchema *out)
{
	TableMerge	state = {m, o->name, o, t, true};
	TableMerge *tm = &state;
	const char *problem;

	memset(out, 0, sizeof(*out));
	out->name = o->name;
	out->replica_identity = WS_REPLICA_DEFAULT;
	if (!same_key_columns(o, t) || (b != NULL && !same_key_columns(o, b)))
	{
		conflict(m, "key", o->name);
		return false;
	}
	out->nkey = o->nkey;
	out->key = o->key;
	merge_key_name(tm, b, o, t, out);

	/* columns: ours' order, then those only theirs has */
	{
		Items		lo = column_items(o);
		Items		lt = column_items(t);
		Items		lb = b != NULL ? column_items(b) : (Items) {NULL, 0};
		void	  **what = palloc_array(void *, lo.n + lt.n + 1);

		out->ncols = merge_items(&lb, &lo, &lt, what, column_clashed, tm);
		out->cols = chosen(what, out->ncols, sizeof(WsColumn), NULL);
	}
	for (int i = 0; i < out->ncols; i++)
		for (int j = 0; j < i; j++)
			if (strcmp(out->cols[i].name, out->cols[j].name) == 0)
			{
				char	   *where = psprintf("%s.%s", o->name, out->cols[i].name);

				conflict(m, "column-name", where);
				pfree(where);
				tm->ok = false;
			}
	MERGE_LIST(constraint_items, nconstraints, constraints, WsConstraint, constraint_cmp);
	MERGE_LIST(index_items, nindexes, indexes, WsIndex, index_cmp);
	MERGE_LIST(index_settings_items, nindex_settings, index_settings, WsIndexSettings, index_settings_cmp);
	MERGE_LIST(option_items, noptions, options, WsOption, option_cmp);
	MERGE_LIST(comment_items, ncomments, comments, WsComment, comment_cmp);
	MERGE_LIST(grant_items, ngrants, grants, WsGrant, grant_cmp);
	merge_group(tm, b, o, t, copy_partitioning, out);
	merge_group(tm, b, o, t, copy_partition_of, out);
	merge_group(tm, b, o, t, copy_inherits, out);
	merge_group(tm, b, o, t, copy_key_timing, out);
	merge_group(tm, b, o, t, copy_replica, out);
	merge_group(tm, b, o, t, copy_clustered, out);
	merge_group(tm, b, o, t, copy_row_security, out);
	merge_group(tm, b, o, t, copy_owner, out);
	{
		Items		lo = inherited_col_items(o);
		Items		lt = inherited_col_items(t);
		Items		lb = b != NULL ? inherited_col_items(b) : (Items) {NULL, 0};
		void	  **what = palloc_array(void *, lo.n + lt.n + 1);

		out->ninherited_cols = merge_items(&lb, &lo, &lt, what, other_clashed, tm);
		out->inherited_cols = chosen(what, out->ninherited_cols, sizeof(uint64), NULL);
	}
	MERGE_LIST(inherited_con_items, ninherited_cons, inherited_cons, char *, name_cmp);
	if (!tm->ok)
		return false;
	order_inherited_cols(out);
	drop_indexes_of_dropped(out);

	/* what each side's change leaves of the other's: a check of a column dropped */
	problem = ws_schema_problem(out);
	if (problem != NULL)
	{
		conflict(m, "table-ddl", o->name);
		return false;
	}
	return true;
}

/* -------------------------------------------------------------- the maps */

static void
key_line(Merger *m, const char *what, const uint8 *key, size_t klen)
{
	char	   *where = palloc(klen + strlen(what) + 2);
	char	   *at = where;
	bool		split = false;

	if (what[0] != '\0')
	{
		strcpy(at, what);
		at += strlen(what);
		*at++ = ' ';
	}
	for (size_t i = 0; i < klen; i++)
	{
		/* (an object's kind apart from its name; a foreign key's table too) */
		if (key[i] == '\0')
			*at++ = '/';
		else if (key[i] == '/' && what[0] == '\0' && !split)
		{
			*at++ = ' ';
			split = true;
		}
		else
			*at++ = (char) key[i];
	}
	*at = '\0';
	conflict(m, "object", where);
	pfree(where);
}

/* A map by key; kinds is how a conflict names an entry, or "" for an object's own key. */
static void
merge_map(Merger *m, const PHash *base, const PHash *ours, const PHash *theirs, WsRootMap which,
		  const char *kinds, PHash *rv)
{
	PHash		mb,
				mo,
				mt,
				empty;
	bool		hb = WsRoot->get_map(m->ctx, base, which, &mb);
	bool		ho = WsRoot->get_map(m->ctx, ours, which, &mo);
	bool		ht = WsRoot->get_map(m->ctx, theirs, which, &mt);
	ProllyMergeResult res = {0};

	if (!hb && !ho && !ht)
		return;
	if (prolly_empty(m->ctx, 0, &empty) < 0)
		prolly_report(m->ctx);
	if (prolly_merge(m->ctx, hb ? &mb : &empty, ho ? &mo : &empty, ht ? &mt : &empty, &res) < 0)
		prolly_report(m->ctx);
	for (size_t i = 0; i < res.nconflicts; i++)
		key_line(m, kinds, res.conflicts[i].key, res.conflicts[i].klen);
	WsRoot->put_map(m->ctx, rv, which, &res.root, rv);
	prolly_merge_result_free(&res);
}

/* Violations either side has stay, for whoever resolves them. */
static void
merge_violations(Merger *m, const PHash *base, const PHash *ours, const PHash *theirs, PHash *rv)
{
	PHash		mb,
				mo,
				mt,
				empty;
	bool		hb = WsRoot->get_map(m->ctx, base, WS_ROOT_VIOLATIONS, &mb);
	bool		ho = WsRoot->get_map(m->ctx, ours, WS_ROOT_VIOLATIONS, &mo);
	bool		ht = WsRoot->get_map(m->ctx, theirs, WS_ROOT_VIOLATIONS, &mt);
	ProllyMergeResult res = {0};

	if (!hb && !ho && !ht)
		return;
	if (prolly_empty(m->ctx, 0, &empty) < 0)
		prolly_report(m->ctx);
	if (prolly_merge(m->ctx, hb ? &mb : &empty, ho ? &mo : &empty, ht ? &mt : &empty, &res) < 0)
		prolly_report(m->ctx);
	WsRoot->put_map(m->ctx, rv, WS_ROOT_VIOLATIONS, &res.root, rv);
	prolly_merge_result_free(&res);
}

/* Whether a sequence, as the merged objects hold it, counts down; one with none does not. */
static bool
sequence_descends(Merger *m, const PHash *rv, const uint8 *name, size_t nlen)
{
	PHash		objects;
	Buf			key = BUF_INIT;
	char	   *text = pnstrdup((const char *) name, nlen);
	const uint8 *v;
	size_t		vl;
	WsObject	o;
	bool		descends = false;
	int			rc;

	if (!WsRoot->get_map(m->ctx, rv, WS_ROOT_OBJECTS, &objects))
		return false;
	ws_obj_key(WS_OBJ_SEQUENCE, text, NULL, &key);
	rc = prolly_get(m->ctx, &objects, key.data, key.len, &v, &vl);
	if (rc < 0)
		prolly_report(m->ctx);
	if (rc == PROLLY_OK)
	{
		WsObjectCodec->decode(key.data, key.len, v, vl, &o);
		descends = strstr(o.definition, " INCREMENT BY -") != NULL;
		ws_obj_free(&o);
	}
	buf_free(&key);
	pfree(text);
	return descends;
}

/* A sequence that both moved keeps the further; one either dropped is gone with its object. */
static void
merge_sequences(Merger *m, const PHash *base, const PHash *ours, const PHash *theirs, PHash *rv)
{
	PHash		mb,
				mo,
				mt,
				empty,
				merged;
	bool		hb = WsRoot->get_map(m->ctx, base, WS_ROOT_SEQUENCES, &mb);
	bool		ho = WsRoot->get_map(m->ctx, ours, WS_ROOT_SEQUENCES, &mo);
	bool		ht = WsRoot->get_map(m->ctx, theirs, WS_ROOT_SEQUENCES, &mt);
	ProllyMergeResult res = {0};
	ProllyEdit *edits;

	if (!hb && !ho && !ht)
		return;
	if (prolly_empty(m->ctx, 0, &empty) < 0)
		prolly_report(m->ctx);
	if (prolly_merge(m->ctx, hb ? &mb : &empty, ho ? &mo : &empty, ht ? &mt : &empty, &res) < 0)
		prolly_report(m->ctx);
	edits = palloc0_array(ProllyEdit, Max(res.nconflicts, 1));
	for (size_t i = 0; i < res.nconflicts; i++)
	{
		const ProllyConflict *c = &res.conflicts[i];

		edits[i].key = c->key;
		edits[i].klen = c->klen;
		if (c->ours == NULL || c->theirs == NULL)
			edits[i].del = true;
		else
		{
			int64		lo,
						lt;
			bool		co,
						ct;
			bool		theirs_further;

			ws_seq_decode(c->ours, c->olen, &lo, &co);
			ws_seq_decode(c->theirs, c->tlen, &lt, &ct);
			theirs_further = sequence_descends(m, rv, c->key, c->klen) ? lt < lo : lt > lo;

			if (theirs_further || (lt == lo && ct && !co))
			{
				edits[i].val = c->theirs;
				edits[i].vlen = c->tlen;
			}
			else
			{
				edits[i].val = c->ours;
				edits[i].vlen = c->olen;
			}
		}
	}
	merged = res.root;
	if (res.nconflicts > 0 && prolly_apply(m->ctx, &res.root, edits, res.nconflicts, &merged) < 0)
		prolly_report(m->ctx);
	WsRoot->put_map(m->ctx, rv, WS_ROOT_SEQUENCES, &merged, rv);
	pfree(edits);
	prolly_merge_result_free(&res);
}

/* ------------------------------------------------------------------ rows */

/* Whether a side's column holds text as the merged one reads it. */
static bool
column_reads_alike(const WsColumn *side, const WsColumn *merged)
{
	if (strcmp(side->type, merged->type) != 0 || side->kind != merged->kind)
		return false;
	if (merged->kind == WS_COL_GENERATED_STORED)
		return strcmp(side->def, merged->def) == 0;
	return true;
}

/*
 * Whether the rows of a map the holders each hold are as the merged record would have
 * them: each column it stores held by a side alike, or it empty in every row, and none
 * stored but it.  A side's own rows meet its own type, so one that holds alike vouches
 * for that column, though another's retype left the same text unchanged.
 */
static bool
rows_fit(ProllyCtx *ctx, const PHash *rows, const WsTableSchema **holders, int nholders,
		 const WsTableSchema *merged)
{
	WsRowShape	shape;
	uint64		count;
	bool		fits = true;

	if (prolly_count(ctx, rows, &count) < 0)
		prolly_report(ctx);
	if (count == 0)
		return true;
	ws_schema_row_shape(merged, &shape);
	for (int i = 0; fits && i < shape.nstored; i++)
	{
		const WsColumn *col = ws_schema_column(merged, shape.tags[i]);
		bool		vouched = false;
		bool		held = false;

		for (int h = 0; h < nholders; h++)
		{
			const WsColumn *side = ws_schema_column(holders[h], col->tag);

			held |= side != NULL && side->kind != WS_COL_GENERATED_VIRTUAL;
			vouched |= side != NULL && column_reads_alike(side, col);
		}
		/* (one no side holds has no cell: if so it is null, it reads as that) */
		fits = vouched || (!held && col->def == NULL && col->kind == WS_COL_STORED);
	}
	ws_row_shape_free(&shape);

	/* a cell of a column gone, unless a side holding these same rows has none */
	for (int h = 0; fits && h < nholders; h++)
	{
		ws_schema_row_shape(holders[h], &shape);
		for (int i = 0; fits && i < shape.nstored; i++)
			if (ws_schema_column(merged, shape.tags[i]) == NULL)
			{
				bool		lacks = false;

				for (int g = 0; g < nholders; g++)
					lacks |= ws_schema_column(holders[g], shape.tags[i]) == NULL;
				fits = lacks;
			}
		ws_row_shape_free(&shape);
	}
	return fits;
}

static void
merge_rows(Merger *m, const WsTableSchema *merged, const WsTableSchema *b, const PHash *rb,
		   const WsTableSchema *o, const PHash *ro, const WsTableSchema *t, const PHash *rt, PHash *out)
{
	const WsTableSchema *holders[3];
	int			n = 0;
	const PHash *keep;

	if (phash_eq(ro, rt))
		keep = ro;
	else if (rb != NULL && phash_eq(ro, rb))
		keep = rt;
	else if (rb != NULL && phash_eq(rt, rb))
		keep = ro;
	else
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("merging rows both sides of table \"%s\" changed is not done yet", merged->name)));
	if (phash_eq(ro, keep))
		holders[n++] = o;
	if (phash_eq(rt, keep))
		holders[n++] = t;
	if (!rows_fit(m->ctx, keep, holders, n, merged))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("moving rows of table \"%s\" into its merged columns is not done yet", merged->name)));
	*out = *keep;
}

/* --------------------------------------------------------------- tables */

typedef struct TableEntry
{
	char	   *name;
	VcTable		entry;
} TableEntry;

typedef struct TableList
{
	TableEntry *v;
	int			n;
	int			cap;
} TableList;

static bool
list_table(void *arg, const char *name, const VcTable *entry)
{
	TableList  *l = arg;

	if (l->n == l->cap)
	{
		l->cap = Max(16, l->cap * 2);
		l->v = l->v == NULL ? palloc_array(TableEntry, l->cap) : repalloc_array(l->v, TableEntry, l->cap);
	}
	l->v[l->n].name = pstrdup(name);
	l->v[l->n].entry = *entry;
	l->n++;
	return true;
}

static const VcTable *
entry_of(const TableList *l, const char *name)
{
	for (int i = 0; i < l->n; i++)
		if (strcmp(l->v[i].name, name) == 0)
			return &l->v[i].entry;
	return NULL;
}

static bool
same_entry(const VcTable *a, const VcTable *b)
{
	if (a == NULL || b == NULL)
		return a == b;
	return phash_eq(&a->schema, &b->schema) && phash_eq(&a->data, &b->data);
}

static void
keep_table(Merger *m, const PHash *from, const char *name, PHash *rv)
{
	WsTableSchema rec;
	PHash		rows;

	if (!WsRoot->get_table(m->ctx, from, name, &rec, &rows))
		elog(ERROR, "a table listed is not there");
	WsRoot->put_table(m->ctx, rv, &rec, &rows, rv);
	ws_schema_free(&rec);
}

static void
merge_table(Merger *m, const PHash *base, const PHash *ours, const PHash *theirs, const char *name,
			const VcTable *b, const VcTable *o, const VcTable *t, PHash *rv)
{
	WsTableSchema sb,
				so,
				st,
				merged;
	PHash		rb,
				ro,
				rt,
				rows;
	bool		has_base = b != NULL;

	if (same_entry(o, t))
	{
		if (o != NULL)
			keep_table(m, ours, name, rv);
		return;
	}
	if (same_entry(o, b))
	{
		if (t != NULL)
			keep_table(m, theirs, name, rv);
		return;
	}
	if (same_entry(t, b))
	{
		if (o != NULL)
			keep_table(m, ours, name, rv);
		return;
	}
	if (o == NULL || t == NULL)
	{
		conflict(m, "table", name);
		return;
	}
	if (!has_base && !phash_eq(&o->schema, &t->schema))
	{
		conflict(m, "table", name);
		return;
	}
	if (has_base && !WsRoot->get_table(m->ctx, base, name, &sb, &rb))
		elog(ERROR, "a table listed is not there");
	if (!WsRoot->get_table(m->ctx, ours, name, &so, &ro) ||
		!WsRoot->get_table(m->ctx, theirs, name, &st, &rt))
		elog(ERROR, "a table listed is not there");
	if (merge_record(m, has_base ? &sb : NULL, &so, &st, &merged))
	{
		merge_rows(m, &merged, has_base ? &sb : NULL, has_base ? &rb : NULL, &so, &ro, &st, &rt, &rows);
		WsRoot->put_table(m->ctx, rv, &merged, &rows, rv);
	}
	if (has_base)
		ws_schema_free(&sb);
	ws_schema_free(&so);
	ws_schema_free(&st);
}

/* ------------------------------------------- what no one record can say */

/* As Dolt: a foreign key on or to what is gone goes too. */
static void
drop_foreign_keys_of_dropped(Merger *m, PHash *rv)
{
	PHash		map;
	WsForeignKey *fks;
	int			n;
	ProllyEdit *edits;
	int			nedits = 0;

	if (!WsRoot->get_map(m->ctx, rv, WS_ROOT_FOREIGN_KEYS, &map))
		return;
	ws_fk_list(m->ctx, &map, &fks, &n);
	edits = palloc0_array(ProllyEdit, Max(n, 1));
	for (int i = 0; i < n; i++)
	{
		WsTableSchema on,
					to;
		PHash		rows;
		bool		has_on = WsRoot->get_table(m->ctx, rv, fks[i].table, &on, &rows);
		bool		has_to = WsRoot->get_table(m->ctx, rv, fks[i].ref_table, &to, &rows);
		bool		whole = has_on && has_to;

		for (int k = 0; whole && k < fks[i].ncols; k++)
			whole = ws_schema_column(&on, fks[i].cols[k]) != NULL &&
				ws_schema_column(&to, fks[i].ref_cols[k]) != NULL;
		if (has_on)
			ws_schema_free(&on);
		if (has_to)
			ws_schema_free(&to);
		if (!whole)
		{
			Buf			key = BUF_INIT;

			WsFkCodec->encode_key(&fks[i], &key);
			edits[nedits].key = key.data;
			edits[nedits].klen = key.len;
			edits[nedits].del = true;
			nedits++;
		}
	}
	if (nedits > 0)
	{
		if (prolly_apply(m->ctx, &map, edits, nedits, &map) < 0)
			prolly_report(m->ctx);
		WsRoot->put_map(m->ctx, rv, WS_ROOT_FOREIGN_KEYS, &map, rv);
	}
}

/* A partition or child has each column its table has, and that table is there. */
static void
check_has_its_tables_columns(Merger *m, const PHash *rv, const WsTableSchema *child, const char *parent_name)
{
	WsTableSchema parent;
	PHash		rows;

	if (!WsRoot->get_table(m->ctx, rv, parent_name, &parent, &rows))
	{
		conflict(m, "table-ddl", child->name);
		return;
	}
	for (int i = 0; i < parent.ncols; i++)
	{
		const WsColumn *c = ws_schema_column_named(child, parent.cols[i].name);

		if (c == NULL || strcmp(c->type, parent.cols[i].type) != 0)
			conflict(m, "table-ddl", child->name);
	}
	ws_schema_free(&parent);
}

typedef struct NameSeen
{
	Merger	   *m;
	char	  **names;
	int			n;
	int			cap;
} NameSeen;

/* A relation's or type's name, of the one namespace they share. */
static void
name_taken(NameSeen *seen, const char *name)
{
	if (name == NULL)
		return;
	for (int i = 0; i < seen->n; i++)
		if (strcmp(seen->names[i], name) == 0)
		{
			conflict(seen->m, "table-ddl", name);
			return;
		}
	if (seen->n == seen->cap)
	{
		seen->cap = Max(64, seen->cap * 2);
		seen->names = seen->names == NULL ? palloc_array(char *, seen->cap) :
			repalloc_array(seen->names, char *, seen->cap);
	}
	seen->names[seen->n++] = pstrdup(name);
}

/* What only the merged root as a whole can show: Postgres would refuse to make it. */
static void
check_merged_root(Merger *m, const PHash *rv)
{
	TableList	tables = {0};
	NameSeen	seen = {m};
	PHash		objects;

	WsRoot->tables(m->ctx, rv, list_table, &tables);
	for (int i = 0; i < tables.n; i++)
	{
		WsTableSchema rec;
		PHash		rows;

		if (!WsRoot->get_table(m->ctx, rv, tables.v[i].name, &rec, &rows))
			elog(ERROR, "a table listed is not there");
		if (rec.partition_of != NULL)
			check_has_its_tables_columns(m, rv, &rec, rec.partition_of);
		for (int p = 0; p < rec.ninherits; p++)
			check_has_its_tables_columns(m, rv, &rec, rec.inherits[p]);
		name_taken(&seen, rec.name);
		name_taken(&seen, rec.key_name);
		for (int k = 0; k < rec.nconstraints; k++)
			if (rec.constraints[k].kind == WS_CONSTRAINT_UNIQUE || rec.constraints[k].kind == WS_CONSTRAINT_EXCLUSION)
				name_taken(&seen, rec.constraints[k].name);
		for (int k = 0; k < rec.nindexes; k++)
			name_taken(&seen, rec.indexes[k].name);
		for (int c = 0; c < rec.ncols; c++)
			name_taken(&seen, rec.cols[c].identity_seq);
		ws_schema_free(&rec);
	}
	if (WsRoot->get_map(m->ctx, rv, WS_ROOT_OBJECTS, &objects))
	{
		WsObject   *objs;
		int			n;

		ws_obj_list(m->ctx, &objects, &objs, &n);
		for (int i = 0; i < n; i++)
		{
			if (objs[i].kind == WS_OBJ_VIEW || objs[i].kind == WS_OBJ_MATVIEW ||
				objs[i].kind == WS_OBJ_SEQUENCE || objs[i].kind == WS_OBJ_INDEX ||
				objs[i].kind == WS_OBJ_TYPE || objs[i].kind == WS_OBJ_DOMAIN)
				name_taken(&seen, objs[i].name);
			ws_obj_free(&objs[i]);
		}
	}
}

static void
tags_merge(ProllyCtx *ctx, const PHash *base, const PHash *ours, const PHash *theirs, WsMergeResult *out)
{
	Merger		m = {ctx};
	TableList	lb = {0},
				lo = {0},
				lt = {0};
	PHash		rv = vc_root_empty(ctx);

	merge_map(&m, base, ours, theirs, WS_ROOT_OBJECTS, "", &rv);
	merge_map(&m, base, ours, theirs, WS_ROOT_FOREIGN_KEYS, "foreign-key", &rv);
	merge_map(&m, base, ours, theirs, WS_ROOT_ROLES, "role", &rv);
	merge_sequences(&m, base, ours, theirs, &rv);
	merge_violations(&m, base, ours, theirs, &rv);
	WsRoot->tables(ctx, base, list_table, &lb);
	WsRoot->tables(ctx, ours, list_table, &lo);
	WsRoot->tables(ctx, theirs, list_table, &lt);
	for (int s = 0; s < 3; s++)
	{
		const TableList *from = s == 0 ? &lo : s == 1 ? &lt : &lb;

		for (int i = 0; i < from->n; i++)
		{
			const char *name = from->v[i].name;

			CHECK_FOR_INTERRUPTS();

			/* (each once: where it is first of ours, theirs, base) */
			if ((s > 0 && entry_of(&lo, name) != NULL) || (s > 1 && entry_of(&lt, name) != NULL))
				continue;
			merge_table(&m, base, ours, theirs, name, entry_of(&lb, name), entry_of(&lo, name),
						entry_of(&lt, name), &rv);
		}
	}
	drop_foreign_keys_of_dropped(&m, &rv);
	check_merged_root(&m, &rv);
	out->root = rv;
	out->nconflicts = m.nconflicts;
	out->conflicts = m.conflicts;
}

const WsMergeRoutine TagsWsMerge = {
	.name = "tags",
	.merge = tags_merge,
};

const WsMergeRoutine *WsMerge = &TagsWsMerge;
