/*-------------------------------------------------------------------------
 *
 * ws_schema.c
 *	  The working-set model's table schemas (prolly/ws/ws_schema.h), and
 *	  "binary", their codec.
 *
 * version byte, name, uvarint ncols, then each column:
 * uvarint tag, name, type, kind byte, not null byte (0 or 1), identity byte,
 * default, collation; then the key's name, uvarint nkey and the key's tags;
 * then uvarint nconstraints and each constraint: name, kind byte, flags byte,
 * and a check's expression or a unique constraint's uvarint ncols and tags.  A
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

#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "mb/pg_wchar.h"
#include "nodes/miscnodes.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "parser/parse_type.h"
#include "parser/parser.h"
#include "prolly/branch_catalog.h"
#include "prolly/ws/ws_schema.h"
#include "prolly/ws/ws_sql.h"
#include "prolly/ws/ws_tags.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#define WS_SCHEMA_VERSION 1

const WsSchemaCodecRoutine *WsSchemaCodec = &BinaryWsSchemaCodec;

static const char *
own_column_name(void *arg, uint64 tag)
{
	const WsColumn *c = ws_schema_column(arg, tag);

	return c ? c->name : NULL;
}

/* (well formed already) */
static bool
refers_to_own_columns(const WsTableSchema *s, const char *stored)
{
	Buf			rendered = BUF_INIT;
	uint64		missing = 0;
	bool		ok = ws_tags_render(stored, own_column_name, (void *) s, &rendered, &missing);

	buf_free(&rendered);
	return ok;
}

static bool
same_tags(const uint64 *a, int na, const uint64 *b, int nb)
{
	return na == nb && memcmp(a, b, (size_t) na * sizeof(uint64)) == 0;
}

const char *
ws_schema_problem(const WsTableSchema *s)
{
	if (s->name == NULL || s->name[0] == '\0')
		return "a table with no name";
	if (strlen(s->name) >= NAMEDATALEN)
		return "a table's name too long";
	if (s->ncols < 0 || s->ncols > WS_MAX_COLS)
		return "more columns than a table can have";
	for (int i = 0; i < s->ncols; i++)
	{
		const WsColumn *c = &s->cols[i];

		if (c->name == NULL || c->name[0] == '\0')
			return "a column with no name";
		if (c->tag == 0)
			return "a column with no tag";
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
		if (c->def != NULL && !ws_tags_well_formed(c->def))
			return "an expression with malformed column references";
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
	/* (now that every column is known, whichever order they refer to each other) */
	for (int i = 0; i < s->ncols; i++)
		if (s->cols[i].def != NULL && !refers_to_own_columns(s, s->cols[i].def))
			return "an expression referring to a column the table does not have";
	if (s->nconstraints < 0)
		return "a negative number of constraints";
	for (int i = 0; i < s->nconstraints; i++)
	{
		const WsConstraint *con = &s->constraints[i];

		if (con->name == NULL || con->name[0] == '\0')
			return "a constraint with no name";
		if (strlen(con->name) >= NAMEDATALEN)
			return "a constraint's name too long";
		if (s->key_name != NULL && strcmp(con->name, s->key_name) == 0)
			return "a constraint named as the key is";
		/* (one order, so one set of bytes, on any cluster) */
		if (i > 0 && strcmp(s->constraints[i - 1].name, con->name) >= 0)
			return "constraints not in order by name";
		if (con->initdeferred && !con->deferrable)
			return "a constraint initially deferred that is not deferrable";
		if (con->kind == WS_CONSTRAINT_CHECK)
		{
			if (con->expr == NULL || con->expr[0] == '\0')
				return "a check with no expression";
			if (!ws_tags_well_formed(con->expr))
				return "an expression with malformed column references";
			if (!refers_to_own_columns(s, con->expr))
				return "an expression referring to a column the table does not have";
			if (con->ncols != 0 || con->nulls_not_distinct || con->deferrable)
				return "a check with a unique constraint's fields";
		}
		else if (con->kind == WS_CONSTRAINT_UNIQUE)
		{
			if (con->expr != NULL)
				return "a unique constraint with an expression";
			if (con->ncols <= 0 || con->ncols > s->ncols)
				return "a unique constraint of no columns, or more than the table has";
			for (int k = 0; k < con->ncols; k++)
			{
				const WsColumn *c = ws_schema_column(s, con->cols[k]);

				if (c == NULL)
					return "a unique constraint with a column the table does not have";
				if (c->kind == WS_COL_GENERATED_VIRTUAL)
					return "a unique constraint with a virtual column";
				for (int j = 0; j < k; j++)
					if (con->cols[j] == con->cols[k])
						return "a unique constraint with a column twice";
			}

			/*
			 * (CREATE TABLE keeps one index of those it would make alike, so
			 * no statement makes both)
			 */
			if (!con->nulls_not_distinct && !con->deferrable &&
				same_tags(con->cols, con->ncols, s->key, s->nkey))
				return "a unique constraint alike the key";
			for (int j = 0; j < i; j++)
			{
				const WsConstraint *prior = &s->constraints[j];

				if (prior->kind == WS_CONSTRAINT_UNIQUE &&
					prior->nulls_not_distinct == con->nulls_not_distinct &&
					prior->deferrable == con->deferrable &&
					prior->initdeferred == con->initdeferred &&
					same_tags(con->cols, con->ncols, prior->cols, prior->ncols))
					return "two unique constraints alike";
			}
		}
		else
			return "a constraint of no known kind";
	}
	return NULL;
}

#define CON_NULLS_NOT_DISTINCT 0x01
#define CON_DEFERRABLE 0x02
#define CON_INITDEFERRED 0x04

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
	buf_append_uvarint(out, (uint64) s->nconstraints);
	for (int i = 0; i < s->nconstraints; i++)
	{
		const WsConstraint *con = &s->constraints[i];

		put_string(out, con->name);
		buf_append_byte(out, (uint8) con->kind);
		buf_append_byte(out, (con->nulls_not_distinct ? CON_NULLS_NOT_DISTINCT : 0) |
						(con->deferrable ? CON_DEFERRABLE : 0) |
						(con->initdeferred ? CON_INITDEFERRED : 0));
		if (con->kind == WS_CONSTRAINT_CHECK)
			put_string(out, con->expr);
		else
		{
			buf_append_uvarint(out, (uint64) con->ncols);
			for (int k = 0; k < con->ncols; k++)
				buf_append_uvarint(out, con->cols[k]);
		}
	}
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

	const uint8 *start = *p;

	if (!read_uvarint(p, end, &v))
		malformed("it ends early");
	*p = start;
	if (!ws_read_canonical_uvarint(p, end, &v))
		malformed("a number is not in its shortest form");
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
	if (n > PG_INT32_MAX)
		malformed("a string too long to be one");
	if (memchr(*p, '\0', (size_t) n) != NULL)
		malformed("a string with a NUL in it");
	if (!pg_verify_mbstr(PG_UTF8, (const char *) *p, (int) n, true))
		malformed("a string not UTF8");
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
	n = get_uvarint(&p, end);
	/* (each is at least four bytes: its name, kind, flags and what follows) */
	if (n > (uint64) (end - p) / 4)
		malformed("more constraints than bytes");
	s->nconstraints = (int) n;
	s->constraints = px_malloc((size_t) Max(n, 1) * sizeof(WsConstraint));
	memset(s->constraints, 0, (size_t) Max(n, 1) * sizeof(WsConstraint));
	for (int i = 0; i < s->nconstraints; i++)
	{
		WsConstraint *con = &s->constraints[i];
		uint8		flags;

		con->name = get_string(&p, end);
		con->kind = (char) get_byte(&p, end);
		flags = get_byte(&p, end);
		if (flags & ~(CON_NULLS_NOT_DISTINCT | CON_DEFERRABLE | CON_INITDEFERRED))
			malformed("a constraint's flags not known");
		con->nulls_not_distinct = (flags & CON_NULLS_NOT_DISTINCT) != 0;
		con->deferrable = (flags & CON_DEFERRABLE) != 0;
		con->initdeferred = (flags & CON_INITDEFERRED) != 0;
		if (con->kind == WS_CONSTRAINT_CHECK)
			con->expr = get_string(&p, end);
		else
		{
			n = get_uvarint(&p, end);
			if (n > (uint64) s->ncols)
				malformed("a unique constraint of more columns than the table has");
			con->ncols = (int) n;
			con->cols = px_malloc((size_t) Max(n, 1) * sizeof(uint64));
			for (int k = 0; k < con->ncols; k++)
				con->cols[k] = get_uvarint(&p, end);
		}
	}
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
	for (int i = 0; i < s->nconstraints; i++)
	{
		px_free(s->constraints[i].name);
		px_free(s->constraints[i].expr);
		px_free(s->constraints[i].cols);
	}
	px_free(s->constraints);
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
	const char *problem = ws_schema_problem(s);
	uint64	   *tags;
	int			n = 0;

	if (problem != NULL)
		elog(ERROR, "%s", problem);
	tags = px_malloc((size_t) (s->nkey + s->ncols) * sizeof(uint64));
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

/* A stored expression rendered with the table's own columns' names. */
static void
put_expression(const WsTableSchema *s, const char *stored, Buf *out)
{
	uint64		missing = 0;

	if (!ws_tags_render(stored, own_column_name, (void *) s, out, &missing))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("an expression of table \"%s\" refers to column tag " UINT64_FORMAT ", which it does not have",
						s->name, missing)));
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

/* The statement, as the record has it. */
static void
write_create_table(const WsTableSchema *s, const char *schema, Buf *out)
{
	bool		any = false;

	put_text(out, "CREATE TABLE ");
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
		put_text(out, c->type);
		if (c->collation != NULL)
		{
			put_text(out, " COLLATE ");
			put_text(out, c->collation);
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
			put_text(out, " DEFAULT (");
			put_expression(s, c->def, out);
			put_text(out, ")");
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
	for (int i = 0; i < s->nconstraints; i++)
	{
		const WsConstraint *con = &s->constraints[i];

		put_text(out, any ? ",\n    CONSTRAINT " : "\n    CONSTRAINT ");
		any = true;
		put_identifier(out, con->name);
		if (con->kind == WS_CONSTRAINT_CHECK)
		{
			put_text(out, " CHECK (");
			put_expression(s, con->expr, out);
			put_text(out, ")");
			continue;
		}
		put_text(out, con->nulls_not_distinct ? " UNIQUE NULLS NOT DISTINCT (" : " UNIQUE (");
		for (int k = 0; k < con->ncols; k++)
		{
			if (k > 0)
				put_text(out, ", ");
			put_identifier(out, ws_schema_column(s, con->cols[k])->name);
		}
		put_text(out, ")");
		if (con->deferrable)
			put_text(out, con->initdeferred ? " DEFERRABLE INITIALLY DEFERRED" : " DEFERRABLE");
	}
	put_text(out, any ? "\n)" : ")");
}

pg_noreturn static void
not_the_record(const WsTableSchema *s, const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("the schema of table \"%s\" does not make that table: %s", s->name, what)));
}

/* Whether a parser's error is the text's fault, and so the record's. */
static bool
is_texts_fault(int code)
{
	int			class = ERRCODE_TO_CATEGORY(code);

	return class == ERRCODE_SYNTAX_ERROR_OR_ACCESS_RULE_VIOLATION ||
		class == ERRCODE_DATA_EXCEPTION || class == ERRCODE_FEATURE_NOT_SUPPORTED;
}

/* The text as parse trees, or a refusal: it is the record's fault. */
static List *
parse_text(const WsTableSchema *s, const char *sql)
{
	MemoryContext cxt = CurrentMemoryContext;
	List	   *volatile parsed = NIL;

	PG_TRY();
	{
		parsed = raw_parser(sql, RAW_PARSE_DEFAULT);
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(cxt);
		edata = CopyErrorData();
		if (!is_texts_fault(edata->sqlerrcode))
			PG_RE_THROW();
		FlushErrorState();
		not_the_record(s, edata->message);
	}
	PG_END_TRY();
	return parsed;
}

/* A column's type alone as a parse tree, or a refusal: it is the record's fault. */
static TypeName *
parse_type(const WsTableSchema *s, const char *type)
{
	MemoryContext cxt = CurrentMemoryContext;
	ErrorSaveContext escontext = {T_ErrorSaveContext};
	TypeName   *volatile parsed = NULL;

	PG_TRY();
	{
		parsed = typeStringToTypeName(type, (Node *) &escontext);
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(cxt);
		edata = CopyErrorData();
		if (!is_texts_fault(edata->sqlerrcode))
			PG_RE_THROW();
		FlushErrorState();
		not_the_record(s, edata->message);
	}
	PG_END_TRY();
	if (parsed == NULL)
		not_the_record(s, "a column's type");
	return parsed;
}

/*
 * An expression as written, parsed alone, is the expression the statement
 * holds: what a comment elsewhere in the statement could otherwise
 * put in its place, while the record reads as something else.
 */
static void
check_expression(const WsTableSchema *s, const char *stored, Node *in_statement)
{
	Buf			text = BUF_INIT;
	char	   *sql;
	List	   *parsed;
	Node	   *alone;

	put_expression(s, stored, &text);
	buf_append_byte(&text, 0);
	sql = psprintf("SELECT (%s)", (char *) text.data);
	parsed = parse_text(s, sql);
	alone = ws_one_expression(parsed);
	if (alone == NULL || !equal(alone, in_statement))
		not_the_record(s, "an expression");
	pfree(sql);
	buf_free(&text);
}

/* Whether a type name would create a sequence too: CREATE TABLE's serial types. */
static bool
is_serial(const TypeName *t)
{
	const char *n;

	if (list_length(t->names) > 2 ||
		(list_length(t->names) == 2 && strcmp(strVal(linitial(t->names)), "pg_catalog") != 0))
		return false;
	n = strVal(llast(t->names));
	return strcmp(n, "smallserial") == 0 || strcmp(n, "serial2") == 0 ||
		strcmp(n, "serial") == 0 || strcmp(n, "serial4") == 0 ||
		strcmp(n, "bigserial") == 0 || strcmp(n, "serial8") == 0;
}

/* A column as parsed is the record's: its name, type, collation and clauses. */
static void
check_column(const WsTableSchema *s, const WsColumn *c, const ColumnDef *def)
{
	TypeName   *type = parse_type(s, c->type);
	int			not_null = 0,
				deflt = 0,
				identity = 0,
				generated = 0;
	ListCell   *lc;

	if (!equal(type, def->typeName))
		not_the_record(s, "a column's type");
	if (is_serial(def->typeName))
		not_the_record(s, "a serial type, which is no type a catalog names");
	if (strcmp(def->colname, c->name) != 0)
		not_the_record(s, "a column's name");
	if ((def->collClause != NULL) != (c->collation != NULL))
		not_the_record(s, "a column's collation");
	if (c->collation != NULL)
	{
		ErrorSaveContext escontext = {T_ErrorSaveContext};
		List	   *names = stringToQualifiedNameList(c->collation, (Node *) &escontext);

		/* (as lists of names: their quoting follows quote_all_identifiers) */
		if (names == NIL || def->collClause->arg != NULL ||
			!equal(names, def->collClause->collname))
			not_the_record(s, "a column's collation");
	}
	if (def->storage_name != NULL || def->compression != NULL || def->fdwoptions != NIL)
		not_the_record(s, "a column's clause");
	foreach(lc, def->constraints)
	{
		Constraint *con = lfirst_node(Constraint, lc);

		if (con->conname != NULL || con->is_no_inherit || con->deferrable || con->initdeferred)
			not_the_record(s, "a column's clause");
		switch (con->contype)
		{
			case CONSTR_NOTNULL:
				not_null++;
				break;
			case CONSTR_DEFAULT:
				deflt++;
				if (c->kind != WS_COL_STORED || c->identity != WS_IDENTITY_NONE || c->def == NULL)
					not_the_record(s, "a default the column does not have");
				check_expression(s, c->def, con->raw_expr);
				break;
			case CONSTR_IDENTITY:
				identity++;
				if (con->options != NIL ||
					con->generated_when != (c->identity == WS_IDENTITY_ALWAYS ?
											ATTRIBUTE_IDENTITY_ALWAYS : ATTRIBUTE_IDENTITY_BY_DEFAULT))
					not_the_record(s, "a column's identity");
				break;
			case CONSTR_GENERATED:
				generated++;
				if (c->kind == WS_COL_STORED ||
					con->generated_kind != (c->kind == WS_COL_GENERATED_STORED ?
											ATTRIBUTE_GENERATED_STORED : ATTRIBUTE_GENERATED_VIRTUAL))
					not_the_record(s, "a column's generation");
				check_expression(s, c->def, con->raw_expr);
				break;
			default:
				not_the_record(s, "a column's clause");
		}
	}
	/* (exactly one of each kind the record has, and none other: no standing in) */
	if (not_null != (c->not_null ? 1 : 0) ||
		identity != (c->identity != WS_IDENTITY_NONE ? 1 : 0) ||
		generated != (c->kind != WS_COL_STORED ? 1 : 0) ||
		deflt != (c->kind == WS_COL_STORED && c->identity == WS_IDENTITY_NONE && c->def != NULL ? 1 : 0))
		not_the_record(s, "a column's clauses");
}

/* A table's constraint in the statement is the record's, and nothing more. */
static void
check_constraint(const WsTableSchema *s, const WsConstraint *con, Constraint *in_statement)
{
	if (in_statement->conname == NULL || strcmp(in_statement->conname, con->name) != 0 ||
		in_statement->is_no_inherit || in_statement->skip_validation ||
		in_statement->deferrable != con->deferrable ||
		in_statement->initdeferred != con->initdeferred ||
		in_statement->including != NIL || in_statement->options != NIL ||
		in_statement->indexspace != NULL || in_statement->indexname != NULL ||
		in_statement->without_overlaps || in_statement->pktable != NULL ||
		in_statement->exclusions != NIL || in_statement->where_clause != NULL)
		not_the_record(s, "another constraint");
	if (con->kind == WS_CONSTRAINT_CHECK)
	{
		/* (the grammar says these of a check alone) */
		if (in_statement->contype != CONSTR_CHECK || in_statement->keys != NIL ||
			!in_statement->is_enforced || !in_statement->initially_valid)
			not_the_record(s, "another constraint");
		check_expression(s, con->expr, in_statement->raw_expr);
	}
	else
	{
		int			k = 0;
		ListCell   *kc;

		if (in_statement->contype != CONSTR_UNIQUE || in_statement->raw_expr != NULL ||
			in_statement->nulls_not_distinct != con->nulls_not_distinct ||
			list_length(in_statement->keys) != con->ncols)
			not_the_record(s, "another constraint");
		foreach(kc, in_statement->keys)
			if (strcmp(strVal(lfirst(kc)), ws_schema_column(s, con->cols[k++])->name) != 0)
				not_the_record(s, "another constraint");
	}
}

/*
 * The statement as written is the one the record says, by the grammar: one
 * CREATE TABLE, of this name, these columns, key and constraints, and nothing else.
 * Text in a commit may have come from anywhere, and checking each piece alone
 * says nothing of what the pieces make together: a comment in one hides a
 * quote in the next.  So the whole is what is checked.
 */
static void
check_statement(const WsTableSchema *s, const char *schema, const char *sql)
{
	List	   *parsed = parse_text(s, sql);
	CreateStmt *create;
	ListCell   *lc;
	int			i = 0;

	if (list_length(parsed) != 1 || !IsA(linitial_node(RawStmt, parsed)->stmt, CreateStmt))
		not_the_record(s, "it is not one CREATE TABLE");
	create = (CreateStmt *) linitial_node(RawStmt, parsed)->stmt;
	if (create->relation->catalogname != NULL ||
		create->relation->schemaname == NULL ||
		strcmp(create->relation->schemaname, schema) != 0 ||
		strcmp(create->relation->relname, s->name) != 0)
		not_the_record(s, "another table's name");
	if (create->relation->relpersistence != RELPERSISTENCE_PERMANENT)
		not_the_record(s, "another persistence");
	if (create->inhRelations != NIL || create->partbound != NULL || create->partspec != NULL ||
		create->ofTypename != NULL || create->constraints != NIL || create->nnconstraints != NIL ||
		create->options != NIL || create->oncommit != ONCOMMIT_NOOP ||
		create->tablespacename != NULL || create->accessMethod != NULL || create->if_not_exists)
		not_the_record(s, "a clause the record does not have");
	if (list_length(create->tableElts) != s->ncols + (s->nkey > 0 ? 1 : 0) + s->nconstraints)
		not_the_record(s, "another number of columns or constraints");
	foreach(lc, create->tableElts)
	{
		Node	   *element = lfirst(lc);

		if (i < s->ncols)
		{
			if (!IsA(element, ColumnDef))
				not_the_record(s, "a clause where a column should be");
			check_column(s, &s->cols[i], (ColumnDef *) element);
		}
		else if (i >= s->ncols + (s->nkey > 0 ? 1 : 0))
		{
			if (!IsA(element, Constraint))
				not_the_record(s, "a clause where a constraint should be");
			check_constraint(s, &s->constraints[i - s->ncols - (s->nkey > 0 ? 1 : 0)],
							 (Constraint *) element);
		}
		else
		{
			Constraint *key = (Constraint *) element;
			int			k = 0;
			ListCell   *kc;

			if (!IsA(element, Constraint) || key->contype != CONSTR_PRIMARY ||
				key->conname == NULL || strcmp(key->conname, s->key_name) != 0 ||
				list_length(key->keys) != s->nkey || key->including != NIL ||
				key->options != NIL || key->indexspace != NULL || key->deferrable ||
				key->initdeferred || key->without_overlaps)
				not_the_record(s, "another key");
			foreach(kc, key->keys)
				if (strcmp(strVal(lfirst(kc)), ws_schema_column(s, s->key[k++])->name) != 0)
					not_the_record(s, "another key");
		}
		i++;
	}
}

/* (strings of a schema, a new copy, each from UTF8 into the server's encoding) */
static char *
from_utf8(const char *text)
{
	return text ? pstrdup(pg_any_to_server(text, (int) strlen(text), PG_UTF8)) : NULL;
}

/* (a name may be longer here: the grammar would cut it short, and quietly) */
static char *
name_from_utf8(const char *name)
{
	char	   *here = from_utf8(name);

	if (here != NULL && strlen(here) >= NAMEDATALEN)
		ereport(ERROR,
				(errcode(ERRCODE_NAME_TOO_LONG),
				 errmsg("name \"%s\" is too long in this database's encoding", here)));
	return here;
}

void
ws_schema_create_table(const WsTableSchema *s, const char *schema, Buf *out)
{
	const char *problem = ws_schema_problem(s);
	WsTableSchema here = *s;
	Buf			sql = BUF_INIT;

	if (problem != NULL)
		elog(ERROR, "%s", problem);
	here.name = name_from_utf8(s->name);
	here.key_name = name_from_utf8(s->key_name);
	here.cols = palloc_array(WsColumn, Max(s->ncols, 1));
	for (int i = 0; i < s->ncols; i++)
	{
		here.cols[i] = s->cols[i];
		here.cols[i].name = name_from_utf8(s->cols[i].name);
		here.cols[i].type = from_utf8(s->cols[i].type);
		here.cols[i].def = from_utf8(s->cols[i].def);
		here.cols[i].collation = from_utf8(s->cols[i].collation);
	}
	here.constraints = palloc_array(WsConstraint, Max(s->nconstraints, 1));
	for (int i = 0; i < s->nconstraints; i++)
	{
		here.constraints[i] = s->constraints[i];
		here.constraints[i].name = name_from_utf8(s->constraints[i].name);
		here.constraints[i].expr = from_utf8(s->constraints[i].expr);
	}
	write_create_table(&here, schema, &sql);
	buf_append_byte(&sql, 0);
	check_statement(&here, schema, (char *) sql.data);
	buf_append(out, sql.data, sql.len - 1);
	buf_free(&sql);
}

static char *
to_utf8(const char *text)
{
	const char *u = pg_server_to_any(text, (int) strlen(text), PG_UTF8);

	return px_memdup(u, strlen(u) + 1);
}

/* (a name's limit in a commit is in UTF8's bytes, on any cluster) */
static char *
name_to_utf8(const char *name)
{
	char	   *u = to_utf8(name);

	if (strlen(u) >= NAMEDATALEN)
		ereport(ERROR,
				(errcode(ERRCODE_NAME_TOO_LONG),
				 errmsg("name \"%s\" is too long for version control", name),
				 errdetail("A commit holds names of up to %d bytes of UTF8.", NAMEDATALEN - 1)));
	return u;
}

/*
 * What the record cannot yet hold and a table of the catalogs has, the first
 * of it: a refusal, not a smaller table.  (The names of a column's not-null
 * constraint and its identity sequence are not in the list: they are made
 * again, and a rename leaves them behind anyway.)
 */
static const char *const unsupported_sql =
"WITH own(o) AS (SELECT $1::oid "
"  UNION ALL SELECT x.indexrelid FROM pg_index x WHERE x.indrelid = $1::oid "
"  UNION ALL SELECT p.objid FROM pg_depend p WHERE p.classid = 'pg_class'::regclass "
"    AND p.refclassid = 'pg_class'::regclass AND p.refobjid = $1::oid AND p.deptype = 'i' "
"    AND p.refobjsubid > 0) "
"SELECT CASE "
"  WHEN c.relispartition OR c.relkind = 'p' THEN 'it is partitioned' "
"  WHEN c.reloftype <> 0 THEN 'it is a typed table' "
"  WHEN EXISTS (SELECT FROM pg_inherits i WHERE i.inhrelid = c.oid OR i.inhparent = c.oid) "
"    THEN 'it has inheritance' "
"  WHEN am.amname <> 'heap' THEN 'it has another access method' "
"  WHEN c.reltablespace <> 0 THEN 'it has a tablespace' "
"  WHEN c.reloptions IS NOT NULL OR tc.reloptions IS NOT NULL "
"    THEN 'it has storage parameters' "
"  WHEN c.relreplident <> 'd' THEN 'it has a replica identity' "
"  WHEN c.relrowsecurity OR c.relforcerowsecurity OR "
"       EXISTS (SELECT FROM pg_policy o WHERE o.polrelid = c.oid) THEN 'it has row security' "
"  WHEN EXISTS (SELECT FROM pg_constraint k WHERE k.conrelid = c.oid AND "
"               k.contype NOT IN ('p', 'u', 'c', 'n')) "
"    THEN 'it has a foreign key, an exclusion or a constraint trigger' "
"  WHEN EXISTS (SELECT FROM pg_constraint k JOIN pg_index x ON x.indexrelid = k.conindid "
"               JOIN pg_class ic ON ic.oid = x.indexrelid "
"               WHERE k.conrelid = c.oid AND k.contype IN ('p', 'u') AND "
"                 ((k.contype = 'p' AND k.condeferrable) OR k.conperiod OR x.indisclustered OR "
"                  x.indnkeyatts <> x.indnatts OR "
"                  ic.reloptions IS NOT NULL OR ic.reltablespace <> 0)) "
"    THEN 'a key or unique constraint has more than columns' "
"  WHEN EXISTS (SELECT FROM pg_constraint k JOIN pg_index x ON x.indexrelid = k.conindid "
"               JOIN pg_constraint k2 ON k2.conrelid = k.conrelid AND k2.oid > k.oid "
"               JOIN pg_index x2 ON x2.indexrelid = k2.conindid "
"               WHERE k.conrelid = c.oid AND k.contype IN ('p', 'u') AND k2.contype IN ('p', 'u') "
"                 AND x.indkey::text = x2.indkey::text "
"                 AND x.indnullsnotdistinct = x2.indnullsnotdistinct "
"                 AND k.condeferrable = k2.condeferrable AND k.condeferred = k2.condeferred) "
"    THEN 'it has two constraints alike, of which CREATE TABLE makes one' "
"  WHEN EXISTS (SELECT FROM pg_constraint k WHERE k.conrelid = c.oid AND k.contype = 'c' AND "
"               (NOT k.convalidated OR k.connoinherit OR NOT k.conenforced)) "
"    THEN 'it has a check not valid, no inherit or not enforced' "
"  WHEN EXISTS (SELECT FROM pg_index x WHERE x.indrelid = c.oid AND NOT EXISTS "
"               (SELECT FROM pg_constraint k WHERE k.conindid = x.indexrelid AND k.conrelid = c.oid)) "
"    THEN 'it has an index no constraint has' "
"  WHEN EXISTS (SELECT FROM pg_description d WHERE "
"               (d.classoid = 'pg_class'::regclass AND d.objoid IN (SELECT o FROM own)) OR "
"               (d.classoid = 'pg_constraint'::regclass AND d.objoid IN "
"                (SELECT k.oid FROM pg_constraint k WHERE k.conrelid = c.oid))) "
"    THEN 'it has a comment' "
"  WHEN EXISTS (SELECT FROM pg_seclabel l WHERE l.classoid = 'pg_class'::regclass "
"               AND l.objoid IN (SELECT o FROM own)) THEN 'it has a security label' "
"  WHEN (c.relacl IS NOT NULL AND c.relacl <> acldefault('r', c.relowner)) OR "
"       EXISTS (SELECT FROM pg_attribute a WHERE a.attrelid = c.oid "
"               AND a.attacl IS NOT NULL) OR "
"       EXISTS (SELECT FROM pg_class q WHERE q.oid IN (SELECT o FROM own) AND q.relkind = 'S' "
"               AND q.relacl IS NOT NULL AND q.relacl <> acldefault('s', q.relowner)) "
"    THEN 'it has grants' "
"  WHEN EXISTS (SELECT FROM pg_constraint k WHERE k.conrelid = c.oid AND k.contype = 'n' AND "
"               (NOT k.convalidated OR k.connoinherit)) "
"    THEN 'it has a not-null constraint not valid or no inherit' "
"  WHEN EXISTS (SELECT FROM pg_attribute a JOIN pg_type t ON t.oid = a.atttypid "
"               WHERE a.attrelid = c.oid AND a.attnum > 0 AND NOT a.attisdropped AND "
"                 (a.attstorage <> t.typstorage OR a.attcompression <> '' OR "
"                  a.attstattarget IS NOT NULL OR a.attoptions IS NOT NULL)) "
"    THEN 'a column has storage, compression, statistics or options set' "
"  WHEN EXISTS (SELECT FROM pg_attrdef d JOIN pg_depend p ON p.classid = 'pg_attrdef'::regclass "
"                 AND p.objid = d.oid AND p.refclassid = 'pg_class'::regclass "
"               JOIN pg_class q ON q.oid = p.refobjid "
"               WHERE d.adrelid = c.oid AND q.relkind = 'S') "
"    THEN 'a default uses a sequence' "
"  WHEN EXISTS (SELECT FROM pg_depend p JOIN pg_sequence q ON q.seqrelid = p.objid "
"               WHERE p.classid = 'pg_class'::regclass AND p.refclassid = 'pg_class'::regclass "
"                 AND p.refobjid = c.oid AND p.deptype = 'a') "
"    THEN 'it owns a sequence' "
"  WHEN EXISTS (SELECT FROM pg_depend p JOIN pg_sequence q ON q.seqrelid = p.objid "
"               JOIN pg_attribute a ON a.attrelid = p.refobjid AND a.attnum = p.refobjsubid "
"               WHERE p.classid = 'pg_class'::regclass AND p.refclassid = 'pg_class'::regclass "
"                 AND p.refobjid = c.oid AND p.deptype = 'i' AND a.attidentity <> '' AND "
"                 (q.seqstart <> 1 OR q.seqincrement <> 1 OR q.seqmin <> 1 OR q.seqcache <> 1 "
"                  OR q.seqcycle OR q.seqmax <> CASE a.atttypid WHEN 'int2'::regtype THEN 32767 "
"                    WHEN 'int4'::regtype THEN 2147483647 ELSE 9223372036854775807 END)) "
"    THEN 'an identity has sequence options set' "
"  END "
"FROM pg_class c LEFT JOIN pg_am am ON am.oid = c.relam "
"LEFT JOIN pg_class tc ON tc.oid = c.reltoastrelid WHERE c.oid = $1::oid";

/*
 * The columns as this snapshot saw them have the names they have now: names
 * are printed from the caches, not the snapshot, so an older snapshot would
 * put another column's name in an expression.  Under the lock, it holds till
 * the transaction ends.
 */
static void
check_snapshot_current(const char *table, Oid relid, const WsSqlRows *cols)
{
	for (int i = 0; i < cols->nrows; i++)
	{
		HeapTuple	tup = SearchSysCache2(ATTNUM, ObjectIdGetDatum(relid),
										  Int16GetDatum((int16) atoi(ws_sql_value(cols, i, 0))));
		bool		same;

		if (!HeapTupleIsValid(tup))
			same = false;
		else
		{
			same = strcmp(NameStr(((Form_pg_attribute) GETSTRUCT(tup))->attname),
						  ws_sql_value(cols, i, 1)) == 0;
			ReleaseSysCache(tup);
		}
		if (!same)
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("a column of table \"%s\" was renamed or dropped since this transaction's snapshot",
							table)));
	}
}

/* (the columns' names in the server's encoding, as an expression prints them) */
typedef struct ReadCols
{
	int			n;
	const char **names;
	uint64	   *tags;
} ReadCols;

static bool
read_tag_of(void *arg, const char *name, uint64 *tag)
{
	ReadCols   *c = arg;

	for (int i = 0; i < c->n; i++)
		if (strcmp(c->names[i], name) == 0)
		{
			*tag = c->tags[i];
			return true;
		}
	return false;
}

/* A list of attribute numbers, as the record's columns' tags. */
static void
read_col_tags(const char *table, const WsSqlRows *cols, const WsTableSchema *out,
			  const char *attnums, int n, uint64 *tags)
{
	for (int k = 0; k < n; k++)
	{
		char	   *end;
		long		attnum = strtol(attnums, &end, 10);
		bool		found = false;

		attnums = end;
		for (int i = 0; i < cols->nrows && !found; i++)
			if (atol(ws_sql_value(cols, i, 0)) == attnum)
			{
				tags[k] = out->cols[i].tag;
				found = true;
			}
		if (!found)
			elog(ERROR, "a constraint of table \"%s\" on column %ld, which it does not have",
				 table, attnum);
	}
}

static int
constraint_cmp(const void *a, const void *b)
{
	return strcmp(((const WsConstraint *) a)->name, ((const WsConstraint *) b)->name);
}

bool
ws_schema_read(const char *schema, const char *table, WsTableSchema *out)
{
	const char *params[2] = {schema, table};
	int			level = WsSql->begin();
	WsSqlRows	rel,
				cols,
				key,
				others;
	ReadCols	rc = {0};
	const char *relkind,
			   *persistence;

	memset(out, 0, sizeof(*out));

	/*
	 * The name's table, locked: while waiting for the lock the name may have
	 * come to mean another table, or none, so it is looked up again after.
	 */
	for (Oid locked = InvalidOid;;)
	{
		Oid			now;

		WsSql->run("SELECT c.oid::text, c.relkind::text, c.relpersistence::text, n.nspbranch::text "
				   "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
				   "WHERE n.nspname = $1 AND c.relname = $2", 2, params, &rel);
		now = rel.nrows == 0 ? InvalidOid : atooid(ws_sql_value(&rel, 0, 0));
		if (now == locked)
			break;
		if (OidIsValid(locked))
			UnlockRelationOid(locked, AccessShareLock);
		locked = now;
		if (OidIsValid(locked))
		{
			ws_sql_rows_free(&rel);
			LockRelationOid(locked, AccessShareLock);
		}
		else
			break;
	}
	if (rel.nrows == 0)
	{
		ws_sql_rows_free(&rel);
		WsSql->end(level);
		return false;
	}
	relkind = ws_sql_value(&rel, 0, 1);
	persistence = ws_sql_value(&rel, 0, 2);
	if (relkind[0] != RELKIND_RELATION && relkind[0] != RELKIND_PARTITIONED_TABLE)
	{
		ws_sql_rows_free(&rel);
		WsSql->end(level);
		return false;
	}
	if (persistence[0] == RELPERSISTENCE_TEMP)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("the schema of temporary table \"%s\" cannot be read yet", table)));
	if (atoi(ws_sql_value(&rel, 0, 3)) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("table \"%s\" is not in a versioned schema", table)));
	{
		const char *oid[1] = {ws_sql_value(&rel, 0, 0)};
		WsSqlRows	why;

		WsSql->run(unsupported_sql, 1, oid, &why);
		if (why.nrows == 1 && ws_sql_value(&why, 0, 0) != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("the schema of table \"%s\" cannot be read yet: %s",
							table, ws_sql_value(&why, 0, 0))));
		ws_sql_rows_free(&why);
	}
	out->name = name_to_utf8(table);
	{
		const char *oid[1] = {ws_sql_value(&rel, 0, 0)};

		WsSql->run("SELECT a.attnum::text, a.attname::text, "
				   "  format_type(a.atttypid, a.atttypmod), a.attnotnull::text, "
				   "  a.attidentity::text, a.attgenerated::text, "
				   "  pg_get_expr(d.adbin, d.adrelid), "
				   "  CASE WHEN a.attcollation <> t.typcollation THEN "
				   "    quote_ident(cn.nspname) || '.' || quote_ident(co.collname) END, "
				   "  a.attcoltag::text "
				   "FROM pg_attribute a "
				   "JOIN pg_type t ON t.oid = a.atttypid "
				   "LEFT JOIN pg_attrdef d ON d.adrelid = a.attrelid AND d.adnum = a.attnum "
				   "LEFT JOIN pg_collation co ON co.oid = a.attcollation "
				   "LEFT JOIN pg_namespace cn ON cn.oid = co.collnamespace "
				   "WHERE a.attrelid = $1::oid AND a.attnum > 0 AND NOT a.attisdropped "
				   "ORDER BY a.attnum", 1, oid, &cols);
		WsSql->run("SELECT con.conname::text, con.contype::text, pg_get_expr(con.conbin, con.conrelid), "
				   "  array_to_string((i.indkey::int2[])[0:i.indnkeyatts - 1], ' '), "
				   "  i.indnullsnotdistinct::text, con.condeferrable::text, con.condeferred::text "
				   "FROM pg_constraint con LEFT JOIN pg_index i ON i.indexrelid = con.conindid "
				   "WHERE con.conrelid = $1::oid AND con.contype IN ('c', 'u')", 1, oid, &others);
		WsSql->run("SELECT con.conname::text, i.indnkeyatts::text, i.indnatts::text, "
				   "  con.condeferrable::text, "
				   "  array_to_string((i.indkey::int2[])[0:i.indnkeyatts - 1], ' ') "
				   "FROM pg_constraint con JOIN pg_index i ON i.indexrelid = con.conindid "
				   "WHERE con.conrelid = $1::oid AND con.contype = 'p'", 1, oid, &key);
	}

	check_snapshot_current(table, atooid(ws_sql_value(&rel, 0, 0)), &cols);
	out->ncols = cols.nrows;
	out->cols = px_malloc((size_t) Max(cols.nrows, 1) * sizeof(WsColumn));
	memset(out->cols, 0, (size_t) Max(cols.nrows, 1) * sizeof(WsColumn));
	rc.n = cols.nrows;
	rc.names = palloc_array(const char *, Max(cols.nrows, 1));
	rc.tags = palloc_array(uint64, Max(cols.nrows, 1));
	for (int i = 0; i < cols.nrows; i++)
	{
		WsColumn   *c = &out->cols[i];
		const char *identity = ws_sql_value(&cols, i, 4);
		const char *generated = ws_sql_value(&cols, i, 5);
		const char *collation = ws_sql_value(&cols, i, 7);

		rc.names[i] = ws_sql_value(&cols, i, 1);
		c->name = name_to_utf8(rc.names[i]);
		c->type = to_utf8(ws_sql_value(&cols, i, 2));
		c->not_null = ws_sql_value(&cols, i, 3)[0] == 't';
		c->identity = identity[0];
		c->kind = generated[0] == ATTRIBUTE_GENERATED_STORED ? WS_COL_GENERATED_STORED :
			generated[0] == ATTRIBUTE_GENERATED_VIRTUAL ? WS_COL_GENERATED_VIRTUAL : WS_COL_STORED;
		c->collation = collation ? to_utf8(collation) : NULL;
		c->tag = (uint64) strtoi64(ws_sql_value(&cols, i, 8), NULL, 10);
		rc.tags[i] = c->tag;
	}
	/* (once every column has its tag: an expression may refer to any) */
	for (int i = 0; i < cols.nrows; i++)
	{
		const char *expr = ws_sql_value(&cols, i, 6);

		if (expr != NULL)
		{
			Buf			stored = BUF_INIT;

			ws_tags_template(expr, read_tag_of, &rc, &stored);
			buf_append_byte(&stored, 0);
			out->cols[i].def = to_utf8((char *) stored.data);
			buf_free(&stored);
		}
	}
	if (key.nrows > 0)
	{
		const char *keys = ws_sql_value(&key, 0, 4);

		out->key_name = name_to_utf8(ws_sql_value(&key, 0, 0));
		out->nkey = atoi(ws_sql_value(&key, 0, 1));
		out->key = px_malloc((size_t) out->nkey * sizeof(uint64));
		read_col_tags(table, &cols, out, keys, out->nkey, out->key);
	}
	out->nconstraints = others.nrows;
	out->constraints = px_malloc((size_t) Max(others.nrows, 1) * sizeof(WsConstraint));
	memset(out->constraints, 0, (size_t) Max(others.nrows, 1) * sizeof(WsConstraint));
	for (int i = 0; i < others.nrows; i++)
	{
		WsConstraint *con = &out->constraints[i];

		con->name = name_to_utf8(ws_sql_value(&others, i, 0));
		con->kind = ws_sql_value(&others, i, 1)[0];
		if (con->kind == WS_CONSTRAINT_CHECK)
		{
			Buf			stored = BUF_INIT;

			ws_tags_template(ws_sql_value(&others, i, 2), read_tag_of, &rc, &stored);
			buf_append_byte(&stored, 0);
			con->expr = to_utf8((char *) stored.data);
			buf_free(&stored);
		}
		else
		{
			const char *keys = ws_sql_value(&others, i, 3);

			con->ncols = 1;
			for (const char *p = keys; *p; p++)
				con->ncols += *p == ' ';
			con->cols = px_malloc((size_t) con->ncols * sizeof(uint64));
			read_col_tags(table, &cols, out, keys, con->ncols, con->cols);
			con->nulls_not_distinct = ws_sql_value(&others, i, 4)[0] == 't';
			con->deferrable = ws_sql_value(&others, i, 5)[0] == 't';
			con->initdeferred = ws_sql_value(&others, i, 6)[0] == 't';
		}
	}
	/* (by name in UTF8's order: what a commit holds, whatever this database sorts by) */
	if (out->nconstraints > 1)
		qsort(out->constraints, out->nconstraints, sizeof(WsConstraint), constraint_cmp);
	pfree(rc.names);
	pfree(rc.tags);
	ws_sql_rows_free(&rel);
	ws_sql_rows_free(&cols);
	ws_sql_rows_free(&key);
	ws_sql_rows_free(&others);
	WsSql->end(level);
	return true;
}

void
ws_schema_set_tags(const char *schema, const WsTableSchema *s)
{
	const char *problem = ws_schema_problem(s);
	char	   *table;
	Oid			nsp = get_namespace_oid(schema, false);
	Oid			relid;
	Relation	rel;
	Relation	attrs;
	int			live = 0;

	if (problem != NULL)
		elog(ERROR, "%s", problem);
	if (NamespaceBranch(nsp) == 0)
		elog(ERROR, "schema \"%s\" is not versioned, and its columns have no tags", schema);
	table = name_from_utf8(s->name);
	relid = get_relname_relid(table, nsp);
	if (!OidIsValid(relid))
		elog(ERROR, "no table \"%s\" in schema \"%s\"", table, schema);
	rel = relation_open(relid, AccessExclusiveLock);
	for (int i = 0; i < RelationGetNumberOfAttributes(rel); i++)
		if (!TupleDescAttr(RelationGetDescr(rel), i)->attisdropped)
			live++;
	/* (every column the record's, or the table could hold a tag twice) */
	if (live != s->ncols)
		elog(ERROR, "table \"%s\" has %d columns, its record %d", table, live, s->ncols);
	relation_close(rel, NoLock);
	attrs = table_open(AttributeRelationId, RowExclusiveLock);
	for (int i = 0; i < s->ncols; i++)
	{
		char	   *name = name_from_utf8(s->cols[i].name);
		HeapTuple	tup = SearchSysCacheCopyAttName(relid, name);

		if (!HeapTupleIsValid(tup))
			elog(ERROR, "no column \"%s\" in table \"%s\"", name, table);
		((Form_pg_attribute) GETSTRUCT(tup))->attcoltag = (int64) s->cols[i].tag;
		CatalogTupleUpdate(attrs, &tup->t_self, tup);
		heap_freetuple(tup);
		pfree(name);
	}
	table_close(attrs, RowExclusiveLock);
	pfree(table);
}
