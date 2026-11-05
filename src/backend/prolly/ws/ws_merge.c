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
#include "nodes/parsenodes.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "prolly/ws/ws_fk.h"
#include "prolly/ws/ws_merge.h"
#include "prolly/ws/ws_obj.h"
#include "prolly/ws/ws_root.h"
#include "prolly/ws/ws_row.h"
#include "prolly/ws/ws_rows.h"
#include "prolly/ws/ws_schema.h"
#include "prolly/ws/ws_sql.h"
#include "prolly/ws/ws_tags.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

/* The keys of the types and domains that differ between a side's objects and the merged. */
typedef struct TypesDiffering
{
	char	  **keys;
	int			n;
} TypesDiffering;

typedef struct Merger
{
	ProllyCtx  *ctx;
	const WsEvalRoutine *eval;
	void	   *eval_arg;
	TypesDiffering ours_types;	/* its types and domains otherwise than merged */
	TypesDiffering theirs_types;
	TypesDiffering base_types;
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

/* A key is its values' text, so a key column of another type is other rows. */
static bool
key_types_differ(const WsTableSchema *a, const WsTableSchema *b)
{
	for (int i = 0; i < a->nkey; i++)
		if (strcmp(ws_schema_column(a, a->key[i])->type, ws_schema_column(b, a->key[i])->type) != 0)
			return true;
	return false;
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

static const char *
name_of_column(void *arg, uint64 tag)
{
	const WsColumn *col = ws_schema_column((const WsTableSchema *) arg, tag);

	return col != NULL ? col->name : NULL;
}

/* Whether a stored expression names a column the record does not have. */
static bool
expression_lost_column(const WsTableSchema *s, const char *stored)
{
	Buf			rendered = BUF_INIT;
	uint64		missing = 0;
	bool		lost;

	if (stored == NULL)
		return false;
	lost = !ws_tags_render(stored, name_of_column, (void *) s, &rendered, &missing);
	buf_free(&rendered);
	return lost;
}

/* Whether an index is on a column the record does not have. */
static bool
index_lost_column(const WsTableSchema *s, const WsIndex *ix)
{
	for (int k = 0; k < ix->nkeys; k++)
		if (ix->keys[k].tag != 0 ? ws_schema_column(s, ix->keys[k].tag) == NULL :
			expression_lost_column(s, ix->keys[k].expr))
			return true;
	for (int c = 0; c < ix->ninclude; c++)
		if (ws_schema_column(s, ix->include[c]) == NULL)
			return true;
	return expression_lost_column(s, ix->predicate);
}

/* An index on a column dropped goes with it, with what is said of it. */
static void
drop_indexes_of_dropped(WsTableSchema *out)
{
	int			kept = 0;

	for (int i = 0; i < out->nindexes; i++)
	{
		if (!index_lost_column(out, &out->indexes[i]))
		{
			out->indexes[kept++] = out->indexes[i];
			continue;
		}
		for (int s = 0; s < out->nindex_settings; s++)
			if (strcmp(out->index_settings[s].index, out->indexes[i].name) == 0)
			{
				memmove(&out->index_settings[s], &out->index_settings[s + 1],
						(out->nindex_settings - s - 1) * sizeof(WsIndexSettings));
				out->nindex_settings--;
				break;
			}
		for (int c = 0; c < out->ncomments; c++)
			if (out->comments[c].kind == WS_COMMENT_INDEX &&
				strcmp(out->comments[c].name, out->indexes[i].name) == 0)
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
	if (!same_key_columns(o, t) || (b != NULL && !same_key_columns(o, b)) ||
		key_types_differ(o, t) || (b != NULL && key_types_differ(o, b)))
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

/*
 * A keyed row has an identity, so rows merge as each side wrote them and only what
 * is kept is put in the merged record's form: a row or a cell no one touched is never
 * read, and a value the other side has since retyped is never asked to read as it.  A
 * keyless row is all its cells: all three maps must be in one form to say which row
 * is which, and that takes reshaping that follows from the row alone.
 */

typedef struct DefaultValue
{
	uint64		tag;
	char	   *text;			/* NULL if it is null */
	bool		computed;		/* false if it could not be */
	bool		varies;			/* no one value, which a merge cannot give */
} DefaultValue;

typedef struct RowMerge
{
	Merger	   *m;
	const WsTableSchema *merged;
	WsRowShape	shape;
	DefaultValue *defaults;
	int			ndefaults;
	MemoryContext cxt;			/* what is for all its rows */
} RowMerge;

typedef enum MoveResult
{
	MOVED,
	MOVE_NOT_READ,
	MOVE_NOT_COMPUTED,
} MoveResult;

/* What a row is being moved from: a side's record, or base's. */
typedef struct MoveMap
{
	RowMerge   *rm;
	const WsTableSchema *side;
	const bool *type_differs;	/* of each of shape's: its type here is not the merged one */
} MoveMap;

typedef struct EditList
{
	ProllyEdit *v;
	int			n;
	int			cap;
} EditList;

static void
edit_add(EditList *l, const uint8 *k, size_t kl, const uint8 *v, size_t vl, bool del)
{
	ProllyEdit *e;

	if (l->n == l->cap)
	{
		l->cap = Max(64, l->cap * 2);
		l->v = l->v == NULL ? palloc_array(ProllyEdit, l->cap) : repalloc_array(l->v, ProllyEdit, l->cap);
	}
	e = &l->v[l->n++];
	e->key = memcpy(palloc(Max(kl, 1)), k, kl);
	e->klen = kl;
	e->val = v != NULL ? memcpy(palloc(Max(vl, 1)), v, vl) : NULL;
	e->vlen = v != NULL ? vl : 0;
	e->del = del;
}

static int
edit_cmp(const void *a, const void *b)
{
	const ProllyEdit *x = a;
	const ProllyEdit *y = b;
	int			c = memcmp(x->key, y->key, Min(x->klen, y->klen));

	if (c != 0)
		return c;
	return (x->klen > y->klen) - (x->klen < y->klen);
}

static void
edits_apply(Merger *m, EditList *l, PHash *root)
{
	if (l->n == 0)
		return;
	qsort(l->v, l->n, sizeof(ProllyEdit), edit_cmp);
	if (prolly_apply(m->ctx, root, l->v, l->n, root) < 0)
		prolly_report(m->ctx);
	for (int i = 0; i < l->n; i++)
	{
		pfree((void *) l->v[i].key);
		if (l->v[i].val != NULL)
			pfree((void *) l->v[i].val);
	}
	l->n = 0;
}

/* A row's key as its values, for a line. */
static void
row_conflict(RowMerge *rm, const char *kind, const uint8 *k, size_t kl)
{
	StringInfoData where;

	initStringInfo(&where);
	appendStringInfo(&where, "%s ", rm->merged->name);
	if (rm->shape.nkey == 0)
		appendStringInfoString(&where, "(keyless)");
	else
	{
		WsField		keys[INDEX_MAX_KEYS];

		WsRowCodec->key_fields(&rm->shape, k, kl, keys);
		for (int i = 0; i < rm->shape.nkey; i++)
			appendStringInfo(&where, "%s%.*s", i > 0 ? "," : "", (int) Min(keys[i].len, 200), keys[i].p);
	}
	conflict(rm->m, kind, where.data);
	pfree(where.data);
}

/* A field's value as text, read from its tree if it has one. */
static char *
field_text(RowMerge *rm, const WsField *f)
{
	Buf			held = BUF_INIT;

	if (f->large)
	{
		PHash		root;
		uint64		total;

		WsRowCodec->large_decode(f->p, f->len, &root, &total);
		WsLargeValues->get(rm->m->ctx, &root, total, &held);
		buf_append_byte(&held, 0);
		return (char *) held.data;
	}
	return pnstrdup((const char *) f->p, f->len);
}

/* A field of a computed value: in a tree if it is that long. */
static void
field_set(RowMerge *rm, WsField *f, const char *text)
{
	size_t		len = text != NULL ? strlen(text) : 0;

	memset(f, 0, sizeof(*f));
	f->null = text == NULL;
	if (text == NULL)
		return;
	if (len > WS_LARGE_CELL)
	{
		PHash		root;
		Buf			name = BUF_INIT;

		WsLargeValues->put(rm->m->ctx, (const uint8 *) text, len, &root);
		WsRowCodec->large_encode(&root, len, &name);
		f->p = name.data;
		f->len = name.len;
		f->large = true;
	}
	else
	{
		f->p = (const uint8 *) text;
		f->len = len;
	}
}

/* A column's default, computed once for a merge's rows. */
static const DefaultValue *
default_value(RowMerge *rm, const WsColumn *col)
{
	DefaultValue *d = NULL;
	MemoryContext old;

	for (int i = 0; i < rm->ndefaults; i++)
		if (rm->defaults[i].tag == col->tag)
			return &rm->defaults[i];
	d = &rm->defaults[rm->ndefaults++];
	memset(d, 0, sizeof(*d));
	d->tag = col->tag;
	old = MemoryContextSwitchTo(rm->cxt);	/* (not the row's: it is for them all) */
	if (col->identity != WS_IDENTITY_NONE)
		d->varies = true;
	else
	{
		Buf			expr = BUF_INIT;
		char	   *raw = NULL;

		ws_schema_render_expression(rm->merged, col->def, &expr);
		buf_append_byte(&expr, 0);
		d->computed = rm->m->eval != NULL &&
			rm->m->eval->column_default(rm->m->eval_arg, (const char *) expr.data, &raw, &d->varies);
		if (d->computed && !d->varies && raw != NULL)
			d->computed = rm->m->eval->reads_as(rm->m->eval_arg, col->type, raw, &d->text);
		buf_free(&expr);
	}
	MemoryContextSwitchTo(old);
	return d;
}

static bool
written_otherwise(const WsColumn *was, const WsColumn *col)
{
	return was == NULL || was->kind != WS_COL_GENERATED_STORED || strcmp(was->def, col->def) != 0 ||
		strcmp(was->type, col->type) != 0;
}

/*
 * A row's generated columns from its merged values: all of them if side is NULL, else
 * those it wrote under another expression or did not have.  False if one could not be.
 */
static bool
regenerate(RowMerge *rm, const WsTableSchema *side, WsField *f)
{
	const WsRowShape *s = &rm->shape;
	const char **names = NULL;
	const char **types = NULL;
	const char **values = NULL;
	int			n = 0;

	for (int i = s->nkey; i < s->nstored; i++)
	{
		const WsColumn *col = ws_schema_column(rm->merged, s->tags[i]);
		Buf			expr = BUF_INIT;
		char	   *raw = NULL;
		char	   *out = NULL;

		if (col->kind != WS_COL_GENERATED_STORED ||
			(side != NULL && !written_otherwise(ws_schema_column(side, col->tag), col)))
			continue;
		if (names == NULL)
		{
			/* (the row as its expressions see it: all else that is stored) */
			names = palloc_array(const char *, s->nstored);
			types = palloc_array(const char *, s->nstored);
			values = palloc_array(const char *, s->nstored);
			for (int c = 0; c < s->nstored; c++)
			{
				const WsColumn *other = ws_schema_column(rm->merged, s->tags[c]);

				if (other->kind != WS_COL_STORED)
					continue;
				names[n] = other->name;
				types[n] = other->type;
				values[n] = f[c].null ? NULL : field_text(rm, &f[c]);
				n++;
			}
		}
		ws_schema_render_expression(rm->merged, col->def, &expr);
		buf_append_byte(&expr, 0);
		if (rm->m->eval == NULL ||
			!rm->m->eval->generated(rm->m->eval_arg, (const char *) expr.data, n, names, types, values, &raw) ||
			(raw != NULL && !rm->m->eval->reads_as(rm->m->eval_arg, col->type, raw, &out)))
			return false;
		field_set(rm, &f[i], out);
		buf_free(&expr);
	}
	return true;
}

/* Whether a side's value in a column must be read to be the merged column's. */
static bool
must_read(const MoveMap *mm, int i, const WsColumn *was, const WsColumn *col)
{
	return strcmp(was->type, col->type) != 0 || mm->type_differs[i];
}

/*
 * A row, as fields of the merged record: what it no longer has is left by decoding, a
 * value under another type or a type changed under one name is read as the merged one,
 * and, if it is new, it is given the default of a column its side did not have.
 */
static MoveResult
move_row(MoveMap *mm, const uint8 *k, size_t kl, const uint8 *v, size_t vl, bool is_new, WsField *f)
{
	RowMerge   *rm = mm->rm;
	const WsRowShape *s = &rm->shape;

	WsRowCodec->decode(s, k, kl, v, vl, f);
	for (int i = 0; i < s->nstored; i++)
	{
		const WsColumn *col = ws_schema_column(rm->merged, s->tags[i]);
		const WsColumn *was = ws_schema_column(mm->side, col->tag);
		char	   *as = NULL;

		if (f[i].null || was == NULL || col->kind == WS_COL_GENERATED_STORED || !must_read(mm, i, was, col))
			continue;
		if (rm->m->eval == NULL ||
			!rm->m->eval->reads_as(rm->m->eval_arg, col->type, field_text(rm, &f[i]), &as))
			return MOVE_NOT_READ;

		/* (a key's values stay its own: its type is not changed by merging) */
		if (i >= s->nkey)
			field_set(rm, &f[i], as);
	}
	for (int i = s->nkey; is_new && i < s->nstored; i++)
	{
		const WsColumn *col = ws_schema_column(rm->merged, s->tags[i]);
		const DefaultValue *d;

		if (ws_schema_column(mm->side, col->tag) != NULL || col->kind != WS_COL_STORED ||
			(col->def == NULL && col->identity == WS_IDENTITY_NONE))
			continue;
		d = default_value(rm, col);
		if (d->varies || !d->computed)
			return MOVE_NOT_COMPUTED;
		field_set(rm, &f[i], d->text);
	}
	return regenerate(rm, mm->side, f) ? MOVED : MOVE_NOT_COMPUTED;
}

/* Keyless rows' counts to change in a map, by key, a batch at a time. */
typedef struct KeylessBatch
{
	uint8	  **keys;
	size_t	   *lens;
	int64	   *changes;
	int			n;
	int			cap;
} KeylessBatch;

static void
keyless_batch_add(KeylessBatch *kb, const uint8 *k, size_t kl, int64 change)
{
	if (kb->n == kb->cap)
	{
		kb->cap = Max(256, kb->cap * 2);
		kb->keys = kb->keys == NULL ? palloc_array(uint8 *, kb->cap) : repalloc_array(kb->keys, uint8 *, kb->cap);
		kb->lens = kb->lens == NULL ? palloc_array(size_t, kb->cap) : repalloc_array(kb->lens, size_t, kb->cap);
		kb->changes = kb->changes == NULL ? palloc_array(int64, kb->cap) : repalloc_array(kb->changes, int64, kb->cap);
	}
	kb->keys[kb->n] = memcpy(palloc(Max(kl, 1)), k, kl);
	kb->lens[kb->n] = kl;
	kb->changes[kb->n] = change;
	kb->n++;
}

/* Each key's changes added up, and what that leaves of it written. */
static void
keyless_batch_apply(RowMerge *rm, KeylessBatch *kb, PHash *cur)
{
	ProllyCtx  *ctx = rm->m->ctx;
	EditList	edits = {0};

	if (kb->n == 0)
		return;

	/* (a batch is small: a key is found by a pass over those seen) */
	for (int i = 0; i < kb->n;)
	{
		int64		change = 0;
		const uint8 *held;
		size_t		heldlen;
		int			rc;
		int64		have = 0;
		const uint8 *key = kb->keys[i];
		size_t		kl = kb->lens[i];
		bool		seen = false;

		for (int e = 0; e < edits.n && !seen; e++)
			seen = edits.v[e].klen == kl && memcmp(edits.v[e].key, key, kl) == 0;
		if (seen)
		{
			i++;
			continue;
		}
		for (int j = i; j < kb->n; j++)
			if (kb->lens[j] == kl && memcmp(kb->keys[j], key, kl) == 0)
				change += kb->changes[j];
		rc = prolly_get(ctx, cur, key, kl, &held, &heldlen);
		if (rc < 0)
			prolly_report(ctx);
		if (rc == PROLLY_OK)
			have = (int64) ws_rows_keyless_count(key, kl, held, heldlen);
		if (have + change <= 0)
		{
			if (rc == PROLLY_OK)
				edit_add(&edits, key, kl, NULL, 0, true);
		}
		else
		{
			Buf			c = BUF_INIT,
						v = BUF_INIT;

			buf_append_uvarint(&c, (uint64) (have + change));
			WsRowCodec->keyless_value(key, kl, c.data, c.len, &v);
			edit_add(&edits, key, kl, v.data, v.len, false);
			buf_free(&c);
			buf_free(&v);
		}
		i++;
	}
	edits_apply(rm->m, &edits, cur);
	for (int i = 0; i < kb->n; i++)
		pfree(kb->keys[i]);
	kb->n = 0;
}

/* Whether a side's rows could hold anything not as the merged record would have it. */
static bool
needs_moving(const MoveMap *mm)
{
	RowMerge   *rm = mm->rm;
	WsRowShape	side;
	bool		needs = false;

	ws_schema_row_shape(mm->side, &side);
	for (int i = 0; !needs && i < side.nstored; i++)
		needs = ws_row_shape_at(&rm->shape, side.tags[i]) < 0;
	ws_row_shape_free(&side);
	for (int i = 0; !needs && i < rm->shape.nstored; i++)
	{
		const WsColumn *col = ws_schema_column(rm->merged, rm->shape.tags[i]);
		const WsColumn *was = ws_schema_column(mm->side, col->tag);

		if (was == NULL)
			needs = col->kind != WS_COL_STORED || col->def != NULL || col->identity != WS_IDENTITY_NONE;
		else if (col->kind == WS_COL_GENERATED_STORED)
			needs = written_otherwise(was, col);
		else
			needs = must_read(mm, i, was, col);
	}
	return needs;
}

/*
 * Whether moving a side's rows follows from each row alone, so that a row in one form
 * is the same row in the other: not a retype, whose USING is its own, nor a value for
 * each row, nor a value changed under its type's name.
 */
static bool
moves_from_the_row_alone(const MoveMap *mm)
{
	RowMerge   *rm = mm->rm;

	for (int i = 0; i < rm->shape.nstored; i++)
	{
		const WsColumn *col = ws_schema_column(rm->merged, rm->shape.tags[i]);
		const WsColumn *was = ws_schema_column(mm->side, col->tag);

		if (was == NULL && col->kind == WS_COL_STORED && (col->def != NULL || col->identity != WS_IDENTITY_NONE))
		{
			const DefaultValue *d = default_value(rm, col);

			if (d->varies || !d->computed)
				return false;
		}
		if (was != NULL && col->kind != WS_COL_GENERATED_STORED && must_read(mm, i, was, col))
			return false;
	}
	return true;
}

#define MOVE_BATCH 1000

/*
 * A whole map in the merged record's form: a keyless table's, or one only to read.  Its
 * rows take the default of a column their side lacks if they are_new.
 */
static void
move_map(MoveMap *mm, const PHash *map, bool are_new, PHash *out)
{
	RowMerge   *rm = mm->rm;
	Merger	   *m = rm->m;
	ProllyIter *it;
	const uint8 *k,
			   *v;
	size_t		kl,
				vl;
	int			rc;
	PHash		cur = *map;
	EditList	edits = {0};
	KeylessBatch moves = {0};
	WsField    *f;
	MemoryContext rowcxt;

	*out = *map;
	if (!needs_moving(mm))
		return;
	f = palloc_array(WsField, Max(rm->shape.nstored, 1));
	rowcxt = AllocSetContextCreate(CurrentMemoryContext, "ws_merge row", ALLOCSET_DEFAULT_SIZES);
	if (prolly_iter_open(m->ctx, map, NULL, 0, &it) < 0)
		prolly_report(m->ctx);
	while ((rc = prolly_iter_next(it, &k, &kl, &v, &vl)) > 0)
	{
		MemoryContext old = MemoryContextSwitchTo(rowcxt);
		MoveResult	moved = move_row(mm, k, kl, v, vl, are_new, f);

		CHECK_FOR_INTERRUPTS();
		if (moved == MOVED)
		{
			Buf			nk = BUF_INIT,
						nv = BUF_INIT;

			WsRowCodec->encode(&rm->shape, f, &nk, &nv);
			MemoryContextSwitchTo(old);
			if (rm->shape.nkey == 0)
			{
				if (nk.len != kl || memcmp(nk.data, k, kl) != 0)
				{
					int64		count = (int64) ws_rows_keyless_count(k, kl, v, vl);

					keyless_batch_add(&moves, k, kl, -count);
					keyless_batch_add(&moves, nk.data, nk.len, count);
				}
			}
			else if (nv.len != vl || memcmp(nv.data, v, vl) != 0)
				edit_add(&edits, k, kl, nv.data, nv.len, false);
		}
		else
		{
			/* (its line outlasts the row) */
			MemoryContextSwitchTo(old);
			row_conflict(rm, moved == MOVE_NOT_READ ? "row" : "generated", k, kl);
		}
		MemoryContextSwitchTo(old);
		MemoryContextReset(rowcxt);
		if (edits.n >= MOVE_BATCH)
			edits_apply(m, &edits, &cur);
		if (moves.n >= MOVE_BATCH)
			keyless_batch_apply(rm, &moves, &cur);
	}
	prolly_iter_close(it);
	if (rc < 0)
		prolly_report(m->ctx);
	edits_apply(m, &edits, &cur);
	keyless_batch_apply(rm, &moves, &cur);
	MemoryContextDelete(rowcxt);
	pfree(f);
	*out = cur;
}

#define SAME_CELL(ha, a, hx, x) \
	((ha) == (hx) && (!(ha) || ((a).large == (x).large && (a).len == (x).len && memcmp((a).p, (x).p, (a).len) == 0)))

/* What a row's three sides are merged from. */
typedef struct RowSides
{
	MoveMap		ours;
	MoveMap		theirs;
	const WsTableSchema *base;
	const bool *base_type_differs;
} RowSides;

/* Whether both sides added a column their base has not. */
static bool
both_added_columns(const RowSides *sides)
{
	const WsTableSchema *o = sides->ours.side;

	if (sides->base == NULL)
		return false;
	for (int i = 0; i < o->ncols; i++)
		if (o->cols[i].kind == WS_COL_STORED && ws_schema_column(sides->theirs.side, o->cols[i].tag) != NULL &&
			ws_schema_column(sides->base, o->cols[i].tag) == NULL)
			return true;
	return false;
}

static bool
same_type_in(const WsTableSchema *a, const WsTableSchema *b, uint64 tag)
{
	const WsColumn *ca = a != NULL ? ws_schema_column(a, tag) : NULL;
	const WsColumn *cb = b != NULL ? ws_schema_column(b, tag) : NULL;

	return ca != NULL && cb != NULL && strcmp(ca->type, cb->type) == 0;
}

/*
 * Whether a side left a cell as base has it: it has no such column, or holds what base's
 * value is, as its own type prints it.
 */
static bool
cell_left(RowMerge *rm, const WsTableSchema *side, const WsTableSchema *b, uint64 tag,
		  bool hs, const WsCell *cs, bool hb, const WsCell *cb)
{
	const WsColumn *now = ws_schema_column(side, tag);
	WsField		fb;
	WsField		fs;
	char	   *as = NULL;

	if (now == NULL || (b != NULL && same_type_in(side, b, tag) && SAME_CELL(hs, *cs, hb, *cb)))
		return true;
	if (b == NULL || ws_schema_column(b, tag) == NULL || same_type_in(side, b, tag) || hs != hb)
		return false;
	if (!hs)
		return true;
	fb = (WsField) {cb->p, cb->len, false, cb->large};
	fs = (WsField) {cs->p, cs->len, false, cs->large};
	return rm->m->eval != NULL &&
		rm->m->eval->reads_as(rm->m->eval_arg, now->type, field_text(rm, &fb), &as) &&
		as != NULL && strcmp(as, field_text(rm, &fs)) == 0;
}

/* A kept cell as a field of the merged record: read, if its side's column reads otherwise. */
static bool
keep_cell(RowMerge *rm, const MoveMap *from, int i, const WsCell *cell, WsField *f)
{
	const WsColumn *col = ws_schema_column(rm->merged, rm->shape.tags[i]);
	const WsColumn *was = ws_schema_column(from->side, col->tag);
	WsField		raw = {cell->p, cell->len, false, cell->large};
	char	   *as = NULL;

	f[i] = raw;
	if (was == NULL || !must_read(from, i, was, col))
		return true;
	if (rm->m->eval == NULL ||
		!rm->m->eval->reads_as(rm->m->eval_arg, col->type, field_text(rm, &raw), &as))
		return false;
	field_set(rm, &f[i], as);
	return true;
}

/*
 * A row both sides changed, cell by cell as each wrote it, into fields of the merged
 * record; false, in conflict.  A cell one side left is the other's, and only a cell
 * kept is read.  With no row of base each cell must be alike, as one side's NULL is no
 * less a value it inserted, and so too of a column both added.  A value in a column
 * one side dropped, changed by the other, would be lost: that is a conflict, as in
 * Dolt.
 */
static bool
merge_cells(RowMerge *rm, const RowSides *sides, const ProllyConflict *c, WsField *f)
{
	const WsRowShape *s = &rm->shape;
	const WsTableSchema *o = sides->ours.side;
	const WsTableSchema *t = sides->theirs.side;
	const WsTableSchema *b = sides->base;

	WsRowCodec->key_fields(s, c->key, c->klen, f);
	for (int i = s->nkey; i < s->nstored; i++)
	{
		const WsColumn *col = ws_schema_column(rm->merged, s->tags[i]);
		WsCell		cb,
					co,
					ct;
		bool		hb,
					ho,
					ht;
		bool		base_has = b != NULL && c->base != NULL && ws_schema_column(b, col->tag) != NULL;
		const MoveMap *from = NULL;
		const WsCell *keep = NULL;

		f[i].null = true;
		f[i].large = false;
		f[i].p = NULL;
		f[i].len = 0;

		/* (computed below, from the values merged: so no conflict of its own) */
		if (col->kind == WS_COL_GENERATED_STORED)
			continue;
		hb = c->base != NULL && WsRowCodec->cell_get(c->base, c->blen, col->tag, &cb);
		ho = WsRowCodec->cell_get(c->ours, c->olen, col->tag, &co);
		ht = WsRowCodec->cell_get(c->theirs, c->tlen, col->tag, &ct);

		if (same_type_in(o, t, col->tag) && SAME_CELL(ho, co, ht, ct))
		{
			from = &sides->ours;
			keep = ho ? &co : NULL;
		}
		else if (base_has || (c->base != NULL && (ws_schema_column(o, col->tag) == NULL || ws_schema_column(t, col->tag) == NULL)))
		{
			/* (a side without the column left it, as a side with it unchanged did) */
			bool		theirs_left = cell_left(rm, t, b, col->tag, ht, &ct, hb, &cb);
			bool		ours_left = cell_left(rm, o, b, col->tag, ho, &co, hb, &cb);

			if (theirs_left)
			{
				from = &sides->ours;
				keep = ho ? &co : NULL;
			}
			else if (ours_left)
			{
				from = &sides->theirs;
				keep = ht ? &ct : NULL;
			}
			else
				return false;
		}
		else if (ws_schema_column(o, col->tag) == NULL)
		{
			/* (both inserted the row, and one has no such column: the other's value) */
			from = &sides->theirs;
			keep = ht ? &ct : NULL;
		}
		else if (ws_schema_column(t, col->tag) == NULL)
		{
			from = &sides->ours;
			keep = ho ? &co : NULL;
		}
		else
			return false;
		if (keep != NULL && !keep_cell(rm, from, i, keep, f))
			return false;
	}

	/* a value of a column gone from the merged, changed by a side that still has it */
	if (c->base != NULL && b != NULL)
		for (int i = 0; i < b->ncols; i++)
		{
			const WsColumn *col = &b->cols[i];
			WsCell		cb,
						cs;
			bool		hb,
						hs;

			if (ws_schema_column(rm->merged, col->tag) != NULL || col->kind != WS_COL_STORED)
				continue;
			hb = WsRowCodec->cell_get(c->base, c->blen, col->tag, &cb);
			if (ws_schema_column(o, col->tag) != NULL)
			{
				hs = WsRowCodec->cell_get(c->ours, c->olen, col->tag, &cs);
				if (!SAME_CELL(hb, cb, hs, cs))
					return false;
			}
			if (ws_schema_column(t, col->tag) != NULL)
			{
				hs = WsRowCodec->cell_get(c->theirs, c->tlen, col->tag, &cs);
				if (!SAME_CELL(hb, cb, hs, cs))
					return false;
			}
		}
	return true;
}

/*
 * Whether a row a side still has, and the other deleted, is only reshaped: each value
 * of base's columns it still has is what base's was, as its type prints it.  Then the
 * delete stands.
 */
static bool
only_reshaped(RowMerge *rm, const WsTableSchema *side, const WsTableSchema *b, const uint8 *base_row,
			  size_t blen, const uint8 *row, size_t len)
{
	for (int i = 0; i < b->ncols; i++)
	{
		const WsColumn *was = &b->cols[i];
		const WsColumn *now = ws_schema_column(side, was->tag);
		WsCell		cb,
					cs;
		bool		hb,
					hs;
		bool		in_key = false;

		for (int k = 0; k < b->nkey; k++)
			in_key |= b->key[k] == was->tag;
		if (now == NULL || in_key || was->kind != WS_COL_STORED || now->kind != WS_COL_STORED)
			continue;
		hb = WsRowCodec->cell_get(base_row, blen, was->tag, &cb);
		hs = WsRowCodec->cell_get(row, len, was->tag, &cs);
		if (!cell_left(rm, side, b, was->tag, hs, &cs, hb, &cb))
			return false;
	}
	return true;
}

/* A side's row taken as it wrote it, put in the merged form if it is still there as that. */
typedef struct MoveTaken
{
	MoveMap    *from;
	const WsTableSchema *base;
	const PHash *other;			/* the other side's map */
	PHash	   *cur;
	EditList	edits;
	MemoryContext rowcxt;
} MoveTaken;

/*
 * Whether a side changed a value of a column gone from the merged: the other dropped it,
 * and that row's bytes did not change there if it held nothing.
 */
static bool
changed_a_dropped_column(const MoveTaken *mt, const ProllyDiff *d)
{
	RowMerge   *rm = mt->from->rm;

	if (mt->base == NULL || d->from == NULL)
		return false;
	for (int i = 0; i < mt->base->ncols; i++)
	{
		const WsColumn *col = &mt->base->cols[i];
		WsCell		cb,
					cs;
		bool		hb,
					hs;

		if (col->kind != WS_COL_STORED || ws_schema_column(rm->merged, col->tag) != NULL ||
			ws_schema_column(mt->from->side, col->tag) == NULL)
			continue;
		hb = WsRowCodec->cell_get(d->from, d->flen, col->tag, &cb);
		hs = WsRowCodec->cell_get(d->to, d->tlen, col->tag, &cs);
		if (!SAME_CELL(hb, cb, hs, cs))
			return true;
	}
	return false;
}

static void
move_taken_row(void *arg, const ProllyDiff *d)
{
	MoveTaken  *mt = arg;
	RowMerge   *rm = mt->from->rm;
	const uint8 *v;
	size_t		vl;
	int			rc;
	bool		is_new = d->from == NULL;
	WsField    *f;
	MoveResult	moved;
	MemoryContext old;

	CHECK_FOR_INTERRUPTS();
	if (d->to == NULL)
		return;
	rc = prolly_get(rm->m->ctx, mt->cur, d->key, d->klen, &v, &vl);
	if (rc < 0)
		prolly_report(rm->m->ctx);
	if (rc != PROLLY_OK || vl != d->tlen || memcmp(v, d->to, vl) != 0)
		return;					/* (merged from both, or the other's) */
	if (changed_a_dropped_column(mt, d))
	{
		row_conflict(rm, "row", d->key, d->klen);
		return;
	}
	if (is_new)
	{
		/* (one the other side holds too isn't new to it: what it holds is its own) */
		rc = prolly_get(rm->m->ctx, mt->other, d->key, d->klen, &v, &vl);
		if (rc < 0)
			prolly_report(rm->m->ctx);
		is_new = rc != PROLLY_OK;
	}
	old = MemoryContextSwitchTo(mt->rowcxt);
	f = palloc_array(WsField, Max(rm->shape.nstored, 1));
	moved = move_row(mt->from, d->key, d->klen, d->to, d->tlen, is_new, f);
	if (moved == MOVED)
	{
		Buf			nk = BUF_INIT,
					nv = BUF_INIT;

		WsRowCodec->encode(&rm->shape, f, &nk, &nv);
		MemoryContextSwitchTo(old);
		if (nv.len != d->tlen || memcmp(nv.data, d->to, d->tlen) != 0)
			edit_add(&mt->edits, d->key, d->klen, nv.data, nv.len, false);
	}
	else
	{
		MemoryContextSwitchTo(old);
		row_conflict(rm, moved == MOVE_NOT_READ ? "row" : "generated", d->key, d->klen);
	}
	MemoryContextReset(mt->rowcxt);
	if (mt->edits.n >= MOVE_BATCH)
		edits_apply(rm->m, &mt->edits, mt->cur);
}

static void
move_taken_rows(MoveMap *from, const WsTableSchema *b, const PHash *base, const PHash *side, const PHash *other, PHash *cur)
{
	MoveTaken	mt = {from, b, other, cur};
	Merger	   *m = from->rm->m;

	/* (dropped columns: a side with one still has something to check of its rows) */
	if (!needs_moving(from))
		return;
	mt.rowcxt = AllocSetContextCreate(CurrentMemoryContext, "ws_merge row", ALLOCSET_DEFAULT_SIZES);
	if (prolly_diff(m->ctx, base, side, move_taken_row, &mt) < 0)
		prolly_report(m->ctx);
	edits_apply(m, &mt.edits, cur);
	MemoryContextDelete(mt.rowcxt);
}

typedef struct KeylessMerge
{
	RowMerge   *rm;
	const PHash *ours;
	bool		reshaped;		/* so a row theirs removed must be one ours has */
	EditList	edits;
} KeylessMerge;

/* Theirs' change in a row's count, added to ours'. */
static void
keyless_change(void *arg, const ProllyDiff *d)
{
	KeylessMerge *km = arg;
	ProllyCtx  *ctx = km->rm->m->ctx;
	uint64		from = d->from != NULL ? ws_rows_keyless_count(d->key, d->klen, d->from, d->flen) : 0;
	uint64		to = d->to != NULL ? ws_rows_keyless_count(d->key, d->klen, d->to, d->tlen) : 0;
	const uint8 *held;
	size_t		heldlen;
	uint64		ours = 0;
	int			rc;

	CHECK_FOR_INTERRUPTS();
	rc = prolly_get(ctx, km->ours, d->key, d->klen, &held, &heldlen);
	if (rc < 0)
		prolly_report(ctx);
	if (rc == PROLLY_OK)
		ours = ws_rows_keyless_count(d->key, d->klen, held, heldlen);
	if (km->reshaped && ours + to < from)
		row_conflict(km->rm, "row", d->key, d->klen);
	else if (ours + to <= from)
	{
		if (rc == PROLLY_OK)
			edit_add(&km->edits, d->key, d->klen, NULL, 0, true);
	}
	else
	{
		Buf			c = BUF_INIT,
					v = BUF_INIT;

		buf_append_uvarint(&c, ours + to - from);
		WsRowCodec->keyless_value(d->key, d->klen, c.data, c.len, &v);
		edit_add(&km->edits, d->key, d->klen, v.data, v.len, false);
		buf_free(&c);
		buf_free(&v);
	}
}

/* A type's name, without an array's brackets or a modifier's, unquoted. */
static char *
type_base_name(const char *type)
{
	char	   *name = pstrdup(type);

	if (name[0] == '"')
	{
		const char *p = name + 1;
		char	   *to = name;

		/* (what is in quotes is the name, whatever it holds) */
		for (; *p != '\0' && !(p[0] == '"' && p[1] != '"'); p++)
		{
			*to++ = *p;
			if (p[0] == '"')
				p++;
		}
		*to = '\0';
	}
	else
	{
		char	   *cut = strpbrk(name, "[(");

		if (cut != NULL)
			*cut = '\0';
	}
	return name;
}

static void
type_object_differs(void *arg, const ProllyDiff *d)
{
	TypesDiffering *td = arg;

	if ((d->klen > 5 && memcmp(d->key, "type/", 5) == 0) ||
		(d->klen > 7 && memcmp(d->key, "domain/", 7) == 0))
	{
		td->keys = td->keys == NULL ? palloc_array(char *, 1) : repalloc_array(td->keys, char *, td->n + 1);
		td->keys[td->n++] = pnstrdup((const char *) d->key, d->klen);
	}
}

static TypesDiffering
types_differing(Merger *m, const PHash *side, const PHash *rv)
{
	TypesDiffering td = {NULL, 0};
	PHash		a,
				b,
				empty;
	bool		ha = WsRoot->get_map(m->ctx, side, WS_ROOT_OBJECTS, &a);
	bool		hb = WsRoot->get_map(m->ctx, rv, WS_ROOT_OBJECTS, &b);

	if (!ha && !hb)
		return td;
	if (prolly_empty(m->ctx, 0, &empty) < 0)
		prolly_report(m->ctx);
	if (prolly_diff(m->ctx, ha ? &a : &empty, hb ? &b : &empty, type_object_differs, &td) < 0)
		prolly_report(m->ctx);
	return td;
}

/* Whether an object is one of these, or needs one: a composite of an enum that changed. */
static bool
object_needs_differing(Merger *m, const PHash *objects, const char *key, const TypesDiffering *td, int depth)
{
	const uint8 *v;
	size_t		vl;
	WsObject	o;
	bool		needs = false;
	int			rc;

	for (int i = 0; i < td->n; i++)
		if (strcmp(td->keys[i], key) == 0)
			return true;
	if (depth > 64)
		return true;
	rc = prolly_get(m->ctx, objects, (const uint8 *) key, strlen(key), &v, &vl);
	if (rc < 0)
		prolly_report(m->ctx);
	if (rc != PROLLY_OK)
		return false;
	WsObjectCodec->decode((const uint8 *) key, strlen(key), v, vl, &o);
	for (int i = 0; !needs && i < o.ndeps; i++)
		needs = object_needs_differing(m, objects, o.deps[i], td, depth + 1);
	ws_obj_free(&o);
	return needs;
}

/* For each of a shape's columns, whether what its type is differs here from the merged. */
static const bool *
columns_of_types_differing(RowMerge *rm, const PHash *rv, const TypesDiffering *td)
{
	Merger	   *m = rm->m;
	bool	   *differs = palloc0_array(bool, Max(rm->shape.nstored, 1));
	PHash		objects;

	if (td->n == 0 || !WsRoot->get_map(m->ctx, rv, WS_ROOT_OBJECTS, &objects))
		return differs;
	for (int i = 0; i < rm->shape.nstored; i++)
	{
		char	   *name = type_base_name(ws_schema_column(rm->merged, rm->shape.tags[i])->type);
		char	   *ranges[2] = {NULL, NULL};
		char	   *multi = strstr(name, "multirange");

		/*
		 * A multirange is no object of its own: it is its range's, named by that as
		 * Postgres names it, range for multirange in it or _multirange after it.
		 */
		if (multi != NULL)
		{
			ranges[0] = psprintf("%.*srange%s", (int) (multi - name), name, multi + strlen("multirange"));
			if (strlen(name) > 11 && strcmp(name + strlen(name) - 11, "_multirange") == 0)
				ranges[1] = pnstrdup(name, strlen(name) - 11);
		}
		for (int k = 0; !differs[i] && k < 4; k++)
		{
			Buf			key = BUF_INIT;

			if (k >= 2 && ranges[k - 2] == NULL)
				continue;
			ws_obj_key(k == 1 ? WS_OBJ_DOMAIN : WS_OBJ_TYPE, k >= 2 ? ranges[k - 2] : name, NULL, &key);
			buf_append_byte(&key, 0);
			differs[i] = object_needs_differing(m, &objects, (const char *) key.data, td, 0);
			buf_free(&key);
		}
		pfree(name);
	}
	return differs;
}

static void
row_merge_init(RowMerge *rm, Merger *m, const WsTableSchema *merged)
{
	memset(rm, 0, sizeof(*rm));
	rm->m = m;
	rm->merged = merged;
	rm->cxt = CurrentMemoryContext;
	ws_schema_row_shape(merged, &rm->shape);
	rm->defaults = palloc_array(DefaultValue, Max(merged->ncols, 1));
}

/* Rows one side alone has or changed: only to read, if its types are other than merged. */
static void
read_one_side(Merger *m, const WsTableSchema *rec, const PHash *rows, const PHash *rv,
			  const TypesDiffering *td, PHash *out)
{
	RowMerge	rm;
	MoveMap		mm;

	row_merge_init(&rm, m, rec);
	mm = (MoveMap) {&rm, rec, columns_of_types_differing(&rm, rv, td)};
	move_map(&mm, rows, false, out);
	ws_row_shape_free(&rm.shape);
}

typedef struct AddedValues
{
	const RowSides *sides;
	bool		found;
} AddedValues;

static void
added_value_row(void *arg, const ProllyDiff *d)
{
	AddedValues *av = arg;
	WsCell		cells[WS_MAX_COLS];
	int			n;

	if (d->to == NULL || av->found)
		return;
	n = WsRowCodec->key_cells(d->key, d->klen, cells);
	for (int i = 0; i < n; i++)
		if (ws_schema_column(av->sides->base, cells[i].tag) == NULL &&
			ws_schema_column(av->sides->ours.side, cells[i].tag) != NULL &&
			ws_schema_column(av->sides->theirs.side, cells[i].tag) != NULL)
			av->found = true;
}

/* Whether a side's changed rows hold a value in a column both added: else they are null alike. */
static bool
keyless_values_in_added(RowMerge *rm, const RowSides *sides, const PHash *base, const PHash *side)
{
	AddedValues av = {sides, false};

	if (prolly_diff(rm->m->ctx, base, side, added_value_row, &av) < 0)
		prolly_report(rm->m->ctx);
	return av.found;
}

/* A keyless table's rows, all in one form if that follows from each row alone. */
static void
merge_keyless_rows(RowMerge *rm, const RowSides *sides, const PHash *base, const PHash *ro, const PHash *rt, PHash *out)
{
	Merger	   *m = rm->m;
	MoveMap		from_base = {rm, sides->base, sides->base_type_differs};
	MoveMap		ours = sides->ours;
	MoveMap		theirs = sides->theirs;
	bool		base_moves = sides->base != NULL && needs_moving(&from_base);
	bool		ours_moves = needs_moving(&ours);
	bool		theirs_moves = needs_moving(&theirs);
	KeylessMerge km = {rm, NULL, base_moves || ours_moves || theirs_moves};
	PHash		mb = *base,
				mo = *ro,
				mt = *rt,
				cur;

	/*
	 * Both adding a column and reaching one set of rows is one change, not two to add
	 * up, and those rows are as both have them: a default of theirs is for no row here.
	 */
	if (phash_eq(ro, rt) && both_added_columns(sides))
	{
		move_map(&ours, ro, false, out);
		return;
	}

	/* a column both added has no base to say which side's rows are the merge's */
	if (!phash_eq(ro, rt) && both_added_columns(sides) &&
		(keyless_values_in_added(rm, sides, base, ro) || keyless_values_in_added(rm, sides, base, rt)))
	{
		row_conflict(rm, "row", NULL, 0);
		*out = *ro;
		return;
	}

	/* one side only, already as merged: its rows are the merge's */
	if (phash_eq(ro, base) && !theirs_moves)
	{
		*out = *rt;
		return;
	}
	if (phash_eq(rt, base) && !ours_moves)
	{
		*out = *ro;
		return;
	}
	if ((base_moves && !moves_from_the_row_alone(&from_base)) ||
		(ours_moves && !moves_from_the_row_alone(&ours)) ||
		(theirs_moves && !moves_from_the_row_alone(&theirs)))
	{
		row_conflict(rm, "row", NULL, 0);
		*out = *ro;
		return;
	}
	if (base_moves)
		move_map(&from_base, base, true, &mb);
	move_map(&ours, ro, true, &mo);
	move_map(&theirs, rt, true, &mt);
	cur = mo;
	km.ours = &mo;
	if (prolly_diff(m->ctx, &mb, &mt, keyless_change, &km) < 0)
		prolly_report(m->ctx);
	edits_apply(m, &km.edits, &cur);
	*out = cur;
}

/*
 * A column both sides added has no base to say which side left a row's value, even in a
 * row only one changed: where they differ is a conflict.
 */
typedef struct BothAdded
{
	RowMerge   *rm;
	uint64	   *tags;
	int			n;
} BothAdded;

static void
both_added_row(void *arg, const ProllyDiff *d)
{
	BothAdded  *ba = arg;

	CHECK_FOR_INTERRUPTS();
	if (d->from == NULL || d->to == NULL)
		return;
	for (int i = 0; i < ba->n; i++)
	{
		WsCell		ca,
					cb;
		bool		ha = WsRowCodec->cell_get(d->from, d->flen, ba->tags[i], &ca);
		bool		hb = WsRowCodec->cell_get(d->to, d->tlen, ba->tags[i], &cb);

		if (!SAME_CELL(ha, ca, hb, cb))
		{
			row_conflict(ba->rm, "row", d->key, d->klen);
			return;
		}
	}
}

static void
check_both_added(RowMerge *rm, const RowSides *sides, const PHash *ro, const PHash *rt)
{
	const WsTableSchema *o = sides->ours.side;
	const WsTableSchema *t = sides->theirs.side;
	const WsTableSchema *b = sides->base;
	BothAdded	ba = {rm, palloc_array(uint64, Max(o->ncols, 1)), 0};

	if (b == NULL)
		return;					/* (a row with no base is compared cell by cell) */
	for (int i = 0; i < o->ncols; i++)
		if (o->cols[i].kind == WS_COL_STORED && ws_schema_column(t, o->cols[i].tag) != NULL &&
			ws_schema_column(b, o->cols[i].tag) == NULL &&
			ws_schema_column(rm->merged, o->cols[i].tag) != NULL)
			ba.tags[ba.n++] = o->cols[i].tag;
	if (ba.n > 0 && !phash_eq(ro, rt) && prolly_diff(rm->m->ctx, ro, rt, both_added_row, &ba) < 0)
		prolly_report(rm->m->ctx);
	pfree(ba.tags);
}

static void
merge_keyed_rows(RowMerge *rm, const RowSides *sides, const PHash *base, const PHash *ro, const PHash *rt, PHash *out)
{
	Merger	   *m = rm->m;
	MoveMap		ours = sides->ours;
	MoveMap		theirs = sides->theirs;
	ProllyMergeResult res = {0};
	EditList	edits = {0};
	WsField    *f = palloc_array(WsField, Max(rm->shape.nstored, 1));
	PHash		cur;

	check_both_added(rm, sides, ro, rt);
	if (prolly_merge(m->ctx, base, ro, rt, &res) < 0)
		prolly_report(m->ctx);
	cur = res.root;
	for (size_t i = 0; i < res.nconflicts; i++)
	{
		const ProllyConflict *c = &res.conflicts[i];

		CHECK_FOR_INTERRUPTS();
		if (c->ours == NULL || c->theirs == NULL)
		{
			/* a delete: it stands if the other side only reshaped the row */
			const WsTableSchema *still = c->ours != NULL ? ours.side : theirs.side;
			const uint8 *row = c->ours != NULL ? c->ours : c->theirs;
			size_t		len = c->ours != NULL ? c->olen : c->tlen;

			if (c->base != NULL && sides->base != NULL &&
				only_reshaped(rm, still, sides->base, c->base, c->blen, row, len))
				edit_add(&edits, c->key, c->klen, NULL, 0, true);
			else
				row_conflict(rm, "row", c->key, c->klen);
		}
		else if (!merge_cells(rm, sides, c, f))
			row_conflict(rm, "row", c->key, c->klen);
		else if (!regenerate(rm, NULL, f))
			row_conflict(rm, "generated", c->key, c->klen);
		else
		{
			Buf			nk = BUF_INIT,
						nv = BUF_INIT;

			WsRowCodec->encode(&rm->shape, f, &nk, &nv);
			edit_add(&edits, c->key, c->klen, nv.data, nv.len, false);
			buf_free(&nk);
			buf_free(&nv);
		}
	}
	edits_apply(m, &edits, &cur);
	prolly_merge_result_free(&res);
	pfree(f);
	move_taken_rows(&ours, sides->base, base, ro, rt, &cur);
	move_taken_rows(&theirs, sides->base, base, rt, ro, &cur);
	*out = cur;
}

static void
merge_rows(Merger *m, const WsTableSchema *merged, const WsTableSchema *b, const PHash *rb,
		   const WsTableSchema *o, const PHash *ro, const WsTableSchema *t, const PHash *rt,
		   const PHash *rv, PHash *out)
{
	RowMerge	rm;
	RowSides	sides;
	PHash		empty;
	const PHash *base = rb;

	if (prolly_empty(m->ctx, WS_ROW_VREFS, &empty) < 0)
		prolly_report(m->ctx);
	if (base == NULL)
		base = &empty;
	row_merge_init(&rm, m, merged);
	sides.ours = (MoveMap) {&rm, o, columns_of_types_differing(&rm, rv, &m->ours_types)};
	sides.theirs = (MoveMap) {&rm, t, columns_of_types_differing(&rm, rv, &m->theirs_types)};
	sides.base = b;
	sides.base_type_differs = columns_of_types_differing(&rm, rv, &m->base_types);
	if (merged->nkey == 0)
		merge_keyless_rows(&rm, &sides, base, ro, rt, out);
	else
		merge_keyed_rows(&rm, &sides, base, ro, rt, out);
	ws_row_shape_free(&rm.shape);
}

/* ------------------------------------------------- computing, through SQL */

/* Tries a statement in each schema in turn; its one value, or NULL for a null. */
static bool
eval_statement(const WsEvalSchemas *in, const char *sql, int nparams, const char *const *params, char **out)
{
	int			level = WsSql->begin();
	bool		ok = false;

	for (int i = 0; !ok && i < in->n; i++)
	{
		int			nest = ws_schema_path_begin(in->schemas[i]);
		WsSqlRows	rows;
		WsSqlFailure failure;

		ok = WsSql->attempt(sql, nparams, params, &rows, &failure);
		if (ok && rows.nrows == 1 && rows.ncols == 1)
		{
			const char *value = ws_sql_value(&rows, 0, 0);

			*out = value != NULL ? pstrdup(value) : NULL;
		}
		else if (ok)
			ok = false;
		ws_schema_path_end(nest);
	}
	WsSql->end(level);
	return ok;
}

typedef struct VariesCheck
{
	const char *sql;
	bool		varies;
} VariesCheck;

static void
check_varies(void *arg)
{
	VariesCheck *vc = arg;
	List	   *parsed = pg_parse_query(vc->sql);
	Query	   *query = parse_analyze_fixedparams(linitial_node(RawStmt, parsed), vc->sql, NULL, 0, NULL);

	vc->varies = contain_mutable_functions((Node *) query->targetList);
}

static bool
sql_column_default(void *arg, const char *expr, char **out, bool *varies)
{
	const WsEvalSchemas *in = arg;
	char	   *sql = psprintf("SELECT (%s)", ws_text_from_utf8(expr));
	VariesCheck vc = {sql, false};
	int			level = WsSql->begin();
	bool		parsed = false;

	for (int i = 0; !parsed && i < in->n; i++)
	{
		int			nest = ws_schema_path_begin(in->schemas[i]);
		WsSqlFailure failure;

		parsed = WsSql->attempt_fn(check_varies, &vc, &failure);
		ws_schema_path_end(nest);
	}
	WsSql->end(level);
	*varies = vc.varies;
	if (!parsed)
		return false;
	if (vc.varies)
		return true;
	if (!eval_statement(in, sql, 0, NULL, out))
		return false;
	if (*out != NULL)
		*out = pstrdup(ws_text_to_utf8(*out));
	return true;
}

static bool
sql_generated(void *arg, const char *expr, int n, const char *const *names, const char *const *types,
			  const char *const *values, char **out)
{
	StringInfoData sql;
	const char **params = palloc_array(const char *, Max(n, 1));
	bool		ok;

	initStringInfo(&sql);
	appendStringInfo(&sql, "SELECT (%s) FROM (SELECT ", ws_text_from_utf8(expr));
	for (int i = 0; i < n; i++)
	{
		appendStringInfo(&sql, "%s$%d::%s AS %s", i > 0 ? ", " : "", i + 1, ws_text_from_utf8(types[i]),
						 quote_identifier(ws_text_from_utf8(names[i])));
		params[i] = values[i] != NULL ? ws_text_from_utf8(values[i]) : NULL;
	}
	appendStringInfoString(&sql, ") x");
	ok = eval_statement(arg, sql.data, n, params, out);
	if (ok && *out != NULL)
		*out = pstrdup(ws_text_to_utf8(*out));
	pfree(sql.data);
	return ok;
}

static bool
sql_reads_as(void *arg, const char *type, const char *text, char **as)
{
	const char *type_text = ws_text_from_utf8(type);
	const char *params[2] = {ws_text_from_utf8(text), type_text};
	char	   *valid = NULL;
	char	   *sql;
	bool		ok;

	/* (asked first, as a cast alone would cut a value down to fit) */
	if (!eval_statement(arg, "SELECT pg_input_is_valid($1, $2)", 2, params, &valid) ||
		valid == NULL || valid[0] != 't')
		return false;
	sql = psprintf("SELECT $1::%s", type_text);
	ok = eval_statement(arg, sql, 1, params, as);
	pfree(sql);
	if (ok && *as != NULL)
		*as = pstrdup(ws_text_to_utf8(*as));
	return ok;
}

const WsEvalRoutine SqlWsEval = {
	.name = "sql",
	.column_default = sql_column_default,
	.generated = sql_generated,
	.reads_as = sql_reads_as,
};

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

/* A table only one side changed, as it is; its rows read, if its types are not the merged. */
static void
keep_one_side(Merger *m, const PHash *from, const char *name, const TypesDiffering *td, PHash *rv)
{
	WsTableSchema rec;
	PHash		rows,
				read;
	int			before = m->nconflicts;

	if (!WsRoot->get_table(m->ctx, from, name, &rec, &rows))
		elog(ERROR, "a table listed is not there");
	read = rows;
	if (td->n > 0)
		read_one_side(m, &rec, &rows, rv, td, &read);
	if (m->nconflicts == before)
		WsRoot->put_table(m->ctx, rv, &rec, &read, rv);
	ws_schema_free(&rec);
}

/*
 * Whether both sides reached one set of rows of a keyless table from another: each
 * inserted one row the same, and so there are two of it more, not one.
 */
static bool
keyless_rows_both_changed(Merger *m, const PHash *ours, const char *name, const VcTable *b, const VcTable *o,
						  const PHash *rv)
{
	WsTableSchema rec;
	PHash		rows;
	bool		keyless;

	/* (a schema changed alike is one change: there is no adding it up) */
	if (b == NULL || o == NULL || phash_eq(&b->data, &o->data) || !phash_eq(&b->schema, &o->schema))
		return false;
	if (!WsRoot->get_table(m->ctx, ours, name, &rec, &rows))
		elog(ERROR, "a table listed is not there");
	keyless = rec.nkey == 0;
	if (keyless && m->base_types.n > 0)
	{
		/*
		 * A type changed alike, its values with it, is one change too.  Changed on
		 * one side, it is no such thing: that side's differs from the merged.
		 */
		RowMerge	rm;
		const bool *from_base;
		const bool *from_ours;
		const bool *from_theirs;

		row_merge_init(&rm, m, &rec);
		from_base = columns_of_types_differing(&rm, rv, &m->base_types);
		from_ours = columns_of_types_differing(&rm, rv, &m->ours_types);
		from_theirs = columns_of_types_differing(&rm, rv, &m->theirs_types);
		for (int i = 0; keyless && i < rm.shape.nstored; i++)
			keyless = !(from_base[i] && !from_ours[i] && !from_theirs[i]);
		ws_row_shape_free(&rm.shape);
	}
	ws_schema_free(&rec);
	return keyless;
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

	if (same_entry(o, t) && !keyless_rows_both_changed(m, ours, name, b, o, rv))
	{
		if (o != NULL)
			keep_table(m, ours, name, rv);
		return;
	}

	if (same_entry(o, b))
	{
		if (t != NULL)
			keep_one_side(m, theirs, name, &m->theirs_types, rv);
		return;
	}
	if (same_entry(t, b))
	{
		if (o != NULL)
			keep_one_side(m, ours, name, &m->ours_types, rv);
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
		int			before = m->nconflicts;

		merge_rows(m, &merged, has_base ? &sb : NULL, has_base ? &rb : NULL, &so, &ro, &st, &rt,
				   rv, &rows);
		if (m->nconflicts == before)
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

/* A name, of a namespace of those a relation's or type's are in. */
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

/*
 * What no record alone says, plainly: a partition's or child's table and columns, and
 * a name two of a namespace take.  A relation has a name among relations, and all but
 * an index a type of that name among types; a type is only that.  What Postgres
 * alone can tell, restoring the root says.
 */
static void
check_merged_root(Merger *m, const PHash *rv)
{
	TableList	tables = {0};
	NameSeen	relations = {m};
	NameSeen	types = {m};
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
		name_taken(&relations, rec.name);
		name_taken(&types, rec.name);
		name_taken(&relations, rec.key_name);
		for (int k = 0; k < rec.nconstraints; k++)
			if (rec.constraints[k].kind == WS_CONSTRAINT_UNIQUE || rec.constraints[k].kind == WS_CONSTRAINT_EXCLUSION)
				name_taken(&relations, rec.constraints[k].name);
		for (int k = 0; k < rec.nindexes; k++)
			name_taken(&relations, rec.indexes[k].name);
		for (int c = 0; c < rec.ncols; c++)
		{
			name_taken(&relations, rec.cols[c].identity_seq);
			name_taken(&types, rec.cols[c].identity_seq);
		}
		ws_schema_free(&rec);
	}
	if (WsRoot->get_map(m->ctx, rv, WS_ROOT_OBJECTS, &objects))
	{
		WsObject   *objs;
		int			n;

		ws_obj_list(m->ctx, &objects, &objs, &n);
		for (int i = 0; i < n; i++)
		{
			WsObjectKind kind = objs[i].kind;

			if (kind == WS_OBJ_VIEW || kind == WS_OBJ_MATVIEW || kind == WS_OBJ_SEQUENCE || kind == WS_OBJ_INDEX)
				name_taken(&relations, objs[i].name);
			if (kind == WS_OBJ_VIEW || kind == WS_OBJ_MATVIEW || kind == WS_OBJ_SEQUENCE ||
				kind == WS_OBJ_TYPE || kind == WS_OBJ_DOMAIN)
				name_taken(&types, objs[i].name);
			ws_obj_free(&objs[i]);
		}
	}
}

static void
tags_merge(ProllyCtx *ctx, const PHash *base, const PHash *ours, const PHash *theirs,
		   const WsEvalRoutine *eval, void *eval_arg, WsMergeResult *out)
{
	Merger		m = {ctx, eval, eval_arg};
	TableList	lb = {0},
				lo = {0},
				lt = {0};
	PHash		rv = vc_root_empty(ctx);

	merge_map(&m, base, ours, theirs, WS_ROOT_OBJECTS, "", &rv);
	m.ours_types = types_differing(&m, ours, &rv);
	m.theirs_types = types_differing(&m, theirs, &rv);
	m.base_types = types_differing(&m, base, &rv);
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
