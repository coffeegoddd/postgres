/*-------------------------------------------------------------------------
 *
 * move_alter.c
 *	  A table a move keeps, altered in place from one definition to another.
 *
 * Columns are matched by tag.  Renames go through a spare name each, so that two columns can
 * swap names.  A column added goes in with no default and no not-null constraint, so that each
 * row has it null until the move writes the rows the target has otherwise; its default is set
 * after, which changes no row, and its not-null constraint once the rows are in.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/move_alter.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "prolly/move_alter.h"
#include "prolly/ws/ws_sql.h"
#include "prolly/ws/ws_viol.h"
#include "utils/builtins.h"

static void
run_sql(Buf *sql)
{
	WsSqlRows	rows;

	buf_append_byte(sql, 0);
	WsSql->run((const char *) sql->data, 0, NULL, &rows);
	ws_sql_rows_free(&rows);
	sql->len = 0;
}

static bool
buf_same(const Buf *a, const Buf *b)
{
	return a->len == b->len && memcmp(a->data, b->data, a->len) == 0;
}

static bool
text_same(const char *a, const char *b)
{
	return (a == NULL && b == NULL) || (a != NULL && b != NULL && strcmp(a, b) == 0);
}

/* The index of a column in a record by its tag, or -1. */
static int
col_at(const WsTableSchema *s, uint64 tag)
{
	for (int i = 0; i < s->ncols; i++)
		if (s->cols[i].tag == tag)
			return i;
	return -1;
}

/* Whether the record has a constraint of that name, and it is the same. */
static bool
constraint_same(const WsTableSchema *s, const WsConstraint *con)
{
	for (int i = 0; i < s->nconstraints; i++)
		if (strcmp(s->constraints[i].name, con->name) == 0)
		{
			Buf			a = BUF_INIT,
						b = BUF_INIT;
			bool		same;

			ws_schema_constraint_bytes(&s->constraints[i], &a);
			ws_schema_constraint_bytes(con, &b);
			same = buf_same(&a, &b);
			buf_free(&a);
			buf_free(&b);
			return same;
		}
	return false;
}

static bool
index_same(const WsTableSchema *s, const WsIndex *ix)
{
	for (int i = 0; i < s->nindexes; i++)
		if (strcmp(s->indexes[i].name, ix->name) == 0)
		{
			Buf			a = BUF_INIT,
						b = BUF_INIT;
			bool		same;

			ws_schema_index_bytes(&s->indexes[i], &a);
			ws_schema_index_bytes(ix, &b);
			same = buf_same(&a, &b);
			buf_free(&a);
			buf_free(&b);
			return same;
		}
	return false;
}

/* All of a record but its columns, key, constraints, indexes and storage parameters, as bytes. */
static void
settings_but_options(const WsTableSchema *s, Buf *out)
{
	WsTableSchema t = *s;

	t.noptions = 0;
	t.options = NULL;
	ws_schema_settings_bytes(&t, out);
}

static bool
options_same(const WsTableSchema *a, const WsTableSchema *b)
{
	if (a->noptions != b->noptions)
		return false;
	for (int i = 0; i < a->noptions; i++)
		if (a->options[i].toast != b->options[i].toast || strcmp(a->options[i].name, b->options[i].name) != 0 ||
			strcmp(a->options[i].value, b->options[i].value) != 0)
			return false;
	return true;
}

bool
move_alterable(const WsTableSchema *from, const WsTableSchema *to, bool related)
{
	Buf			a = BUF_INIT,
				b = BUF_INIT;
	bool		same;
	int			last = -1;
	bool		added = false;

	/* (what's done to one of a tree is done to the rest, which an alter of one can't say) */
	if (related || from->partition != WS_PARTITION_NONE || from->partition_of != NULL || from->ninherits > 0)
		return false;

	settings_but_options(from, &a);
	settings_but_options(to, &b);
	same = buf_same(&a, &b);
	buf_free(&a);
	buf_free(&b);
	if (!same || from->nkey != to->nkey || from->key_deferrable != to->key_deferrable ||
		from->key_initdeferred != to->key_initdeferred)
		return false;
	for (int i = 0; i < to->nkey; i++)
		if (from->key[i] != to->key[i])
			return false;

	/* (a row with no key is told by all of it: one whose columns change is another row) */
	if (to->nkey == 0)
	{
		if (from->ncols != to->ncols)
			return false;
		for (int i = 0; i < to->ncols; i++)
		{
			const WsColumn *f = &from->cols[i];
			const WsColumn *t = &to->cols[i];

			if (f->tag != t->tag || strcmp(f->name, t->name) != 0 || strcmp(f->type, t->type) != 0 ||
				f->kind != t->kind || !text_same(f->not_null, t->not_null) || f->identity != t->identity ||
				!text_same(f->def, t->def) || !text_same(f->collation, t->collation))
				return false;
		}
		return true;
	}

	/* the columns both have in the same order, and those only the target has after them */
	for (int i = 0; i < to->ncols; i++)
	{
		const WsColumn *t = &to->cols[i];
		int			at = col_at(from, t->tag);

		if (at < 0)
		{
			if (t->kind != WS_COL_STORED || t->identity != WS_IDENTITY_NONE)
				return false;
			added = true;
			continue;
		}
		if (added || at < last)
			return false;
		last = at;
		{
			const WsColumn *f = &from->cols[at];

			if (strcmp(f->type, t->type) != 0 || !text_same(f->collation, t->collation) || f->kind != t->kind ||
				f->identity != t->identity || !text_same(f->identity_seq, t->identity_seq))
				return false;
			if (t->kind != WS_COL_STORED && !text_same(f->def, t->def))
				return false;
		}
	}
	return true;
}

static void
put_table(const WsTableSchema *s, const char *schema, Buf *out)
{
	char	   *name = ws_name_from_utf8(s->name);

	ws_put_text(out, "ALTER TABLE ONLY ");
	ws_put_identifier(out, schema);
	ws_put_text(out, ".");
	ws_put_identifier(out, name);
	ws_put_text(out, " ");
	pfree(name);
}

static void
put_name(const char *utf8, Buf *out)
{
	char	   *name = ws_name_from_utf8(utf8);

	ws_put_identifier(out, name);
	pfree(name);
}

void
move_alter_before(const WsTableSchema *from, const WsTableSchema *to, const char *schema)
{
	Buf			sql = BUF_INIT;

	/* what the target doesn't have as this has it, which the rows the target has may not meet */
	for (int i = 0; i < from->nconstraints; i++)
	{
		if (constraint_same(to, &from->constraints[i]))
			continue;
		put_table(from, schema, &sql);
		ws_put_text(&sql, "DROP CONSTRAINT ");
		put_name(from->constraints[i].name, &sql);
		run_sql(&sql);
	}
	for (int i = 0; i < from->nindexes; i++)
	{
		if (index_same(to, &from->indexes[i]))
			continue;
		ws_put_text(&sql, "DROP INDEX ");
		ws_put_identifier(&sql, schema);
		ws_put_text(&sql, ".");
		put_name(from->indexes[i].name, &sql);
		run_sql(&sql);
	}
	for (int i = 0; i < from->ncols; i++)
	{
		const WsColumn *f = &from->cols[i];
		int			at = col_at(to, f->tag);
		const WsColumn *t = at >= 0 ? &to->cols[at] : NULL;

		if (t == NULL)
		{
			put_table(from, schema, &sql);
			ws_put_text(&sql, "DROP COLUMN ");
			put_name(f->name, &sql);
			run_sql(&sql);
			continue;
		}
		if (f->not_null != NULL && !text_same(f->not_null, t->not_null))
		{
			put_table(from, schema, &sql);
			ws_put_text(&sql, "ALTER COLUMN ");
			put_name(f->name, &sql);
			ws_put_text(&sql, " DROP NOT NULL");
			run_sql(&sql);
		}
		if (f->kind == WS_COL_STORED && f->def != NULL && !text_same(f->def, t->def))
		{
			put_table(from, schema, &sql);
			ws_put_text(&sql, "ALTER COLUMN ");
			put_name(f->name, &sql);
			ws_put_text(&sql, " DROP DEFAULT");
			run_sql(&sql);
		}
	}

	/* names, each through a spare one, as two columns may swap theirs */
	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < from->ncols; i++)
		{
			const WsColumn *f = &from->cols[i];
			int			at = col_at(to, f->tag);
			char		spare[NAMEDATALEN];

			if (at < 0 || strcmp(f->name, to->cols[at].name) == 0)
				continue;
			snprintf(spare, sizeof(spare), "prolly move " UINT64_FORMAT, f->tag);
			put_table(from, schema, &sql);
			ws_put_text(&sql, "RENAME COLUMN ");
			if (pass == 0)
				put_name(f->name, &sql);
			else
				ws_put_identifier(&sql, spare);
			ws_put_text(&sql, " TO ");
			if (pass == 0)
				ws_put_identifier(&sql, spare);
			else
				put_name(to->cols[at].name, &sql);
			run_sql(&sql);
		}
	if (!text_same(from->key_name, to->key_name))
	{
		put_table(from, schema, &sql);
		ws_put_text(&sql, "RENAME CONSTRAINT ");
		put_name(from->key_name, &sql);
		ws_put_text(&sql, " TO ");
		put_name(to->key_name, &sql);
		run_sql(&sql);
	}

	/* the columns it adds, at the end, null in each row; then defaults, which change no row */
	for (int i = 0; i < to->ncols; i++)
	{
		const WsColumn *t = &to->cols[i];

		if (col_at(from, t->tag) >= 0)
			continue;
		put_table(to, schema, &sql);
		ws_put_text(&sql, "ADD COLUMN ");
		put_name(t->name, &sql);
		ws_put_text(&sql, " ");
		ws_put_text(&sql, t->type);
		if (t->collation != NULL)
		{
			ws_put_text(&sql, " COLLATE ");
			ws_put_text(&sql, t->collation);
		}
		run_sql(&sql);
	}
	for (int i = 0; i < to->ncols; i++)
	{
		const WsColumn *t = &to->cols[i];
		int			at = col_at(from, t->tag);

		if (t->kind != WS_COL_STORED || t->def == NULL || (at >= 0 && text_same(from->cols[at].def, t->def)))
			continue;
		put_table(to, schema, &sql);
		ws_put_text(&sql, "ALTER COLUMN ");
		put_name(t->name, &sql);
		ws_put_text(&sql, " SET DEFAULT ");
		ws_schema_render_expression(to, t->def, &sql);
		run_sql(&sql);
	}
	ws_schema_set_tags(schema, to);
	buf_free(&sql);
}

void
move_alter_after(const WsTableSchema *from, const WsTableSchema *to, const char *schema)
{
	Buf			sql = BUF_INIT;

	for (int i = 0; i < to->ncols; i++)
	{
		const WsColumn *t = &to->cols[i];
		int			at = col_at(from, t->tag);

		if (t->not_null == NULL || (at >= 0 && text_same(from->cols[at].not_null, t->not_null)))
			continue;
		ws_schema_add_not_null(to, t->tag, schema, &sql);
		run_sql(&sql);
	}
	for (int i = 0; i < to->nconstraints; i++)
	{
		if (constraint_same(from, &to->constraints[i]))
			continue;
		if (to->constraints[i].not_valid)
			ws_schema_add_check(to, i, schema, &sql);
		else
			ws_schema_add_constraint(to, i, schema, &sql);
		run_sql(&sql);
	}
	for (int i = 0; i < to->nindexes; i++)
	{
		if (index_same(from, &to->indexes[i]))
			continue;
		ws_schema_create_index(to, i, schema, &sql);
		run_sql(&sql);
	}
	if (!options_same(from, to))
	{
		for (int i = 0; i < from->noptions; i++)
		{
			bool		kept = false;

			for (int k = 0; !kept && k < to->noptions; k++)
				kept = from->options[i].toast == to->options[k].toast &&
					strcmp(from->options[i].name, to->options[k].name) == 0;
			if (kept)
				continue;
			put_table(to, schema, &sql);
			ws_put_text(&sql, "RESET (");
			if (from->options[i].toast)
				ws_put_text(&sql, "toast.");
			ws_put_identifier(&sql, from->options[i].name);
			ws_put_text(&sql, ")");
			run_sql(&sql);
		}
		if (to->noptions > 0)
		{
			put_table(to, schema, &sql);
			ws_put_text(&sql, "SET (");
			for (int i = 0; i < to->noptions; i++)
			{
				if (i > 0)
					ws_put_text(&sql, ", ");
				if (to->options[i].toast)
					ws_put_text(&sql, "toast.");
				ws_put_identifier(&sql, to->options[i].name);
				ws_put_text(&sql, " = ");
				ws_put_text(&sql, quote_literal_cstr(to->options[i].value));
			}
			ws_put_text(&sql, ")");
			run_sql(&sql);
		}
	}
	ws_schema_remember(to, schema);
	buf_free(&sql);
}

void
move_alter_check(const WsTableSchema *to, const char *schema)
{
	char	   *name = ws_name_from_utf8(to->name);
	WsTableSchema now;
	Buf			a = BUF_INIT,
				b = BUF_INIT;

	if (!ws_schema_read(schema, name, &now))
		elog(ERROR, "table \"%s\" of schema \"%s\" went as it was altered", name, schema);
	ws_viol_fold(schema, &now);
	WsSchemaCodec->encode(&now, &a);
	WsSchemaCodec->encode(to, &b);
	if (!buf_same(&a, &b))
		elog(ERROR, "table \"%s\" of schema \"%s\" was altered to other than its target", name, schema);
	buf_free(&a);
	buf_free(&b);
	ws_schema_free(&now);
	pfree(name);
}
