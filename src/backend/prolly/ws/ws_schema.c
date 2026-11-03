/*-------------------------------------------------------------------------
 *
 * ws_schema.c
 *	  The working-set model's table schemas (prolly/ws/ws_schema.h), and
 *	  "binary", their codec.
 *
 * version byte, name, persistence byte, uvarint ncols, then each column:
 * uvarint tag, name, type, kind byte, not null byte (0 or 1), identity byte,
 * default, collation; then the key's name, uvarint nkey and the key's tags.  A
 * string is its uvarint length and bytes, in its shortest form; a default,
 * collation or key's name none is empty, which none is if it is there.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/ws/ws_schema.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "mb/pg_wchar.h"
#include "nodes/miscnodes.h"
#include "nodes/parsenodes.h"
#include "parser/parser.h"
#include "parser/parse_type.h"
#include "prolly/ws/ws_schema.h"
#include "prolly/ws/ws_tags.h"
#include "utils/builtins.h"
#include "utils/regproc.h"

#define WS_SCHEMA_VERSION 1

const WsSchemaCodecRoutine *WsSchemaCodec = &BinaryWsSchemaCodec;

const char *
ws_schema_problem(const WsTableSchema *s)
{
	if (s->name == NULL || s->name[0] == '\0')
		return "a table with no name";
	if (strlen(s->name) >= NAMEDATALEN)
		return "a table's name too long";
	if (s->persistence != WS_PERSISTENCE_PERMANENT && s->persistence != WS_PERSISTENCE_UNLOGGED)
		return "a table of no known persistence";
	if (s->ncols < 0 || s->ncols > WS_MAX_COLS)
		return "more columns than a table can have";
	for (int i = 0; i < s->ncols; i++)
	{
		const WsColumn *c = &s->cols[i];

		if (c->name == NULL || c->name[0] == '\0')
			return "a column with no name";
		if (strlen(c->name) >= NAMEDATALEN)
			return "a column's name too long";
		if (c->type == NULL || c->type[0] == '\0')
			return "a column with no type";
		if (c->kind != WS_COL_STORED && c->kind != WS_COL_GENERATED_STORED &&
			c->kind != WS_COL_GENERATED_VIRTUAL)
			return "a column of no known kind";
		if (c->identity != WS_IDENTITY_NONE && c->identity != WS_IDENTITY_ALWAYS &&
			c->identity != WS_IDENTITY_BY_DEFAULT)
			return "a column of no known identity";
		if (c->kind != WS_COL_STORED && (c->def == NULL || c->def[0] == '\0'))
			return "a generated column with no expression";
		if (c->kind != WS_COL_STORED && c->identity != WS_IDENTITY_NONE)
			return "a generated column that is an identity";
		if (c->identity != WS_IDENTITY_NONE && c->def != NULL)
			return "an identity column with a default";
		if (c->identity != WS_IDENTITY_NONE && !c->not_null)
			return "an identity column that may be null";
		if (c->def != NULL && c->def[0] == '\0')
			return "an empty default";
		if (c->collation != NULL && c->collation[0] == '\0')
			return "an empty collation";
		for (int j = 0; j < i; j++)
		{
			if (s->cols[j].tag == c->tag)
				return "two columns with one tag";
			if (strcmp(s->cols[j].name, c->name) == 0)
				return "two columns with one name";
		}
	}
	if (s->nkey < 0 || s->nkey > s->ncols)
		return "a key of more columns than the table has";
	if ((s->nkey > 0) != (s->key_name != NULL && s->key_name[0] != '\0'))
		return s->nkey > 0 ? "a key with no name" : "a key's name with no key";
	if (s->key_name != NULL && strlen(s->key_name) >= NAMEDATALEN)
		return "a key's name too long";
	for (int i = 0; i < s->nkey; i++)
	{
		const WsColumn *c = ws_schema_column(s, s->key[i]);

		if (c == NULL)
			return "a key with a column the table does not have";
		if (c->kind == WS_COL_GENERATED_VIRTUAL)
			return "a key with a virtual column";
		if (!c->not_null)
			return "a key with a column that may be null";
		for (int j = 0; j < i; j++)
			if (s->key[j] == s->key[i])
				return "a key with a column twice";
	}
	return NULL;
}

static void
put_string(Buf *out, const char *s)
{
	size_t		n = strlen(s);

	buf_append_uvarint(out, n);
	buf_append(out, s, n);
}

static void
put_optional(Buf *out, const char *s)
{
	put_string(out, s ? s : "");
}

static void
schema_encode(const WsTableSchema *s, Buf *out)
{
	const char *problem = ws_schema_problem(s);

	if (problem != NULL)
		elog(ERROR, "%s", problem);
	buf_append_byte(out, WS_SCHEMA_VERSION);
	put_string(out, s->name);
	buf_append_byte(out, (uint8) s->persistence);
	buf_append_uvarint(out, (uint64) s->ncols);
	for (int i = 0; i < s->ncols; i++)
	{
		const WsColumn *c = &s->cols[i];

		buf_append_uvarint(out, c->tag);
		put_string(out, c->name);
		put_string(out, c->type);
		buf_append_byte(out, (uint8) c->kind);
		buf_append_byte(out, c->not_null ? 1 : 0);
		buf_append_byte(out, (uint8) c->identity);
		put_optional(out, c->def);
		put_optional(out, c->collation);
	}
	put_optional(out, s->key_name);
	buf_append_uvarint(out, (uint64) s->nkey);
	for (int i = 0; i < s->nkey; i++)
		buf_append_uvarint(out, s->key[i]);
}

pg_noreturn static void
malformed(const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("a malformed table schema: %s", what)));
}

static uint64
get_uvarint(const uint8 **p, const uint8 *end)
{
	uint64		v = 0;

	if (!ws_read_canonical_uvarint(p, end, &v))
		malformed("it ends early, or a number is not in its shortest form");
	return v;
}

static uint8
get_byte(const uint8 **p, const uint8 *end)
{
	if (*p >= end)
		malformed("it ends early");
	return *(*p)++;
}

/* n bytes as a string of their own: a string has no NUL in it. */
static char *
get_bytes(const uint8 **p, const uint8 *end, uint64 n)
{
	char	   *s;

	if (n > (uint64) (end - *p))
		malformed("a string runs past its end");
	if (memchr(*p, '\0', (size_t) n) != NULL)
		malformed("a string with a NUL in it");
	if (!pg_verifymbstr((const char *) *p, (int) n, true))
		malformed("a string not in the database's encoding");
	s = px_malloc((size_t) n + 1);
	memcpy(s, *p, (size_t) n);
	s[n] = '\0';
	*p += n;
	return s;
}

static char *
get_string(const uint8 **p, const uint8 *end)
{
	uint64		n = get_uvarint(p, end);

	return get_bytes(p, end, n);
}

static char *
get_optional(const uint8 **p, const uint8 *end)
{
	uint64		n = get_uvarint(p, end);

	return n == 0 ? NULL : get_bytes(p, end, n);
}

static void
schema_decode(const uint8 *bytes, size_t len, WsTableSchema *s)
{
	const uint8 *p = bytes,
			   *end = bytes + len;
	uint64		n;
	const char *problem;

	memset(s, 0, sizeof(*s));
	if (get_byte(&p, end) != WS_SCHEMA_VERSION)
		malformed("a version not known");
	s->name = get_string(&p, end);
	s->persistence = (char) get_byte(&p, end);
	n = get_uvarint(&p, end);
	if (n > WS_MAX_COLS)
		malformed("more columns than a table can have");
	s->cols = px_malloc((size_t) n * sizeof(WsColumn));
	memset(s->cols, 0, (size_t) n * sizeof(WsColumn));
	s->ncols = (int) n;
	for (int i = 0; i < s->ncols; i++)
	{
		WsColumn   *c = &s->cols[i];
		uint8		not_null;

		c->tag = get_uvarint(&p, end);
		c->name = get_string(&p, end);
		c->type = get_string(&p, end);
		c->kind = (char) get_byte(&p, end);
		not_null = get_byte(&p, end);
		if (not_null > 1)
			malformed("a column's not null is neither");
		c->not_null = not_null == 1;
		c->identity = (char) get_byte(&p, end);
		c->def = get_optional(&p, end);
		c->collation = get_optional(&p, end);
	}
	s->key_name = get_optional(&p, end);
	n = get_uvarint(&p, end);
	if (n > (uint64) s->ncols)
		malformed("a key of more columns than the table has");
	s->key = px_malloc((size_t) n * sizeof(uint64));
	s->nkey = (int) n;
	for (int i = 0; i < s->nkey; i++)
		s->key[i] = get_uvarint(&p, end);
	if (p != end)
		malformed("it has bytes left over");
	problem = ws_schema_problem(s);
	if (problem != NULL)
		malformed(problem);
}

const WsSchemaCodecRoutine BinaryWsSchemaCodec = {
	.name = "binary",
	.encode = schema_encode,
	.decode = schema_decode,
};

void
ws_schema_free(WsTableSchema *s)
{
	px_free(s->name);
	for (int i = 0; i < s->ncols; i++)
	{
		px_free(s->cols[i].name);
		px_free(s->cols[i].type);
		px_free(s->cols[i].def);
		px_free(s->cols[i].collation);
	}
	px_free(s->cols);
	px_free(s->key_name);
	px_free(s->key);
	memset(s, 0, sizeof(*s));
}

const WsColumn *
ws_schema_column(const WsTableSchema *s, uint64 tag)
{
	for (int i = 0; i < s->ncols; i++)
		if (s->cols[i].tag == tag)
			return &s->cols[i];
	return NULL;
}

const WsColumn *
ws_schema_column_named(const WsTableSchema *s, const char *name)
{
	for (int i = 0; i < s->ncols; i++)
		if (strcmp(s->cols[i].name, name) == 0)
			return &s->cols[i];
	return NULL;
}

void
ws_schema_row_shape(const WsTableSchema *s, WsRowShape *shape)
{
	uint64	   *tags = px_malloc((size_t) (s->nkey + s->ncols) * sizeof(uint64));
	int			n = 0;

	/* (a schema not yet checked, a merge's: ws_row_shape_init refuses too many) */
	for (int i = 0; i < s->nkey; i++)
		tags[n++] = s->key[i];
	for (int i = 0; i < s->ncols; i++)
	{
		bool		in_key = false;

		for (int k = 0; k < s->nkey; k++)
			in_key |= s->key[k] == s->cols[i].tag;
		if (s->cols[i].kind != WS_COL_GENERATED_VIRTUAL && !in_key)
			tags[n++] = s->cols[i].tag;
	}
	ws_row_shape_init(shape, tags, n, s->nkey);
	px_free(tags);
}

static const char *
own_column_name(void *arg, uint64 tag)
{
	const WsColumn *c = ws_schema_column(arg, tag);

	return c ? c->name : NULL;
}

pg_noreturn static void
not_sql(const WsTableSchema *s, const char *what, const char *text)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("table \"%s\" has %s that is not one: \"%s\"", s->name, what, text)));
}

/*
 * Whether text is one expression alone, by the grammar: as SELECT (text), the
 * one statement, its one target, and nothing else, so that closing a
 * parenthesis in it gets it no further.
 */
static bool
is_one_expression(const char *text)
{
	char	   *sql = psprintf("SELECT (%s)", text);
	List	   *parsed = raw_parser(sql, RAW_PARSE_DEFAULT);
	SelectStmt *select;

	pfree(sql);
	if (list_length(parsed) != 1)
		return false;
	select = (SelectStmt *) linitial_node(RawStmt, parsed)->stmt;
	return IsA(select, SelectStmt) && list_length(select->targetList) == 1 &&
		select->fromClause == NIL && select->whereClause == NULL &&
		select->groupClause == NIL && select->havingClause == NULL &&
		select->windowClause == NIL && select->sortClause == NIL &&
		select->limitOffset == NULL && select->limitCount == NULL &&
		select->lockingClause == NIL && select->withClause == NULL &&
		select->distinctClause == NIL && select->valuesLists == NIL &&
		select->intoClause == NULL && select->op == SETOP_NONE;
}

/*
 * A stored expression rendered with the table's own columns' names.  What a
 * commit holds may have come from anywhere: it must be an expression, and no
 * more of a statement, before it goes in one.
 */
static void
put_expression(const WsTableSchema *s, const char *stored, Buf *out)
{
	Buf			text = BUF_INIT;
	uint64		missing = 0;

	if (!ws_tags_render(stored, own_column_name, (void *) s, &text, &missing))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("an expression of table \"%s\" refers to column tag " UINT64_FORMAT ", which it does not have",
						s->name, missing)));
	buf_append_byte(&text, 0);
	if (!is_one_expression((char *) text.data))
		not_sql(s, "an expression", (char *) text.data);
	buf_append(out, text.data, text.len - 1);
	buf_free(&text);
}

/* A type's text, which must be a type's name and modifiers by the grammar. */
static void
put_type(const WsTableSchema *s, const char *type, Buf *out)
{
	ErrorSaveContext escontext = {T_ErrorSaveContext};

	if (typeStringToTypeName(type, (Node *) &escontext) == NULL)
		not_sql(s, "a type", type);
	buf_append(out, type, strlen(type));
}

/* A collation's text, which must be a qualified name. */
static void
put_collation(const WsTableSchema *s, const char *collation, Buf *out)
{
	ErrorSaveContext escontext = {T_ErrorSaveContext};

	if (stringToQualifiedNameList(collation, (Node *) &escontext) == NIL)
		not_sql(s, "a collation", collation);
	buf_append(out, collation, strlen(collation));
}

static void
put_text(Buf *out, const char *text)
{
	buf_append(out, text, strlen(text));
}

static void
put_identifier(Buf *out, const char *name)
{
	const char *quoted = quote_identifier(name);

	put_text(out, quoted);
	if (quoted != name)
		pfree((char *) quoted);
}

void
ws_schema_create_table(const WsTableSchema *s, const char *schema, Buf *out)
{
	bool		any = false;

	put_text(out, s->persistence == WS_PERSISTENCE_UNLOGGED ? "CREATE UNLOGGED TABLE " : "CREATE TABLE ");
	put_identifier(out, schema);
	put_text(out, ".");
	put_identifier(out, s->name);
	put_text(out, " (");
	for (int i = 0; i < s->ncols; i++)
	{
		const WsColumn *c = &s->cols[i];

		put_text(out, any ? ",\n    " : "\n    ");
		any = true;
		put_identifier(out, c->name);
		put_text(out, " ");
		put_type(s, c->type, out);
		if (c->collation != NULL)
		{
			put_text(out, " COLLATE ");
			put_collation(s, c->collation, out);
		}
		if (c->kind != WS_COL_STORED)
		{
			put_text(out, " GENERATED ALWAYS AS (");
			put_expression(s, c->def, out);
			put_text(out, c->kind == WS_COL_GENERATED_STORED ? ") STORED" : ") VIRTUAL");
		}
		else if (c->identity == WS_IDENTITY_ALWAYS)
			put_text(out, " GENERATED ALWAYS AS IDENTITY");
		else if (c->identity == WS_IDENTITY_BY_DEFAULT)
			put_text(out, " GENERATED BY DEFAULT AS IDENTITY");
		else if (c->def != NULL)
		{
			put_text(out, " DEFAULT ");
			put_expression(s, c->def, out);
		}
		if (c->not_null)
			put_text(out, " NOT NULL");
	}
	if (s->nkey > 0)
	{
		put_text(out, any ? ",\n    CONSTRAINT " : "\n    CONSTRAINT ");
		any = true;
		put_identifier(out, s->key_name);
		put_text(out, " PRIMARY KEY (");
		for (int i = 0; i < s->nkey; i++)
		{
			if (i > 0)
				put_text(out, ", ");
			put_identifier(out, ws_schema_column(s, s->key[i])->name);
		}
		put_text(out, ")");
	}
	put_text(out, any ? "\n)" : ")");
}
