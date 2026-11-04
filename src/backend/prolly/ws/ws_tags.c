/*-------------------------------------------------------------------------
 *
 * ws_tags.c
 *	  Column references by tag in stored text (prolly/ws/ws_tags.h).
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/ws/ws_tags.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/int.h"
#include "common/md5.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "parser/parser.h"
#include "prolly/ws/ws_tags.h"
#include "utils/builtins.h"

uint64
ws_tag_derive(const char *table, const char *column, const char *type, int attempt)
{
	char	   *text = psprintf("%s/%s/%s/%d", table, column, type, attempt);
	char		hex[MD5_DIGEST_LENGTH * 2 + 1];
	const char *errstr = NULL;
	uint64		tag = 0;

	if (!pg_md5_hash(text, strlen(text), hex, &errstr))
		elog(ERROR, "could not derive a column's tag: %s", errstr);
	pfree(text);
	/* (its first 60 bits) */
	for (int i = 0; i < 15; i++)
		tag = tag << 4 | (uint64) (hex[i] <= '9' ? hex[i] - '0' : hex[i] - 'a' + 10);
	return tag;
}

static void
escape_span(const char *text, size_t n, Buf *out)
{
	for (size_t i = 0; i < n; i++)
	{
		if (text[i] == '@')
			buf_append_byte(out, '@');
		buf_append_byte(out, (uint8) text[i]);
	}
}

void
ws_tags_escape(const char *text, Buf *out)
{
	escape_span(text, strlen(text), out);
}

typedef struct RefList
{
	ColumnRef **refs;
	int			n;
	int			cap;
} RefList;

static bool
collect_refs(Node *node, void *arg)
{
	RefList    *l = arg;

	if (node == NULL)
		return false;
	/* (a type's modifiers are its own to read: geometry(point,4326)) */
	if (IsA(node, TypeName))
		return false;
	if (IsA(node, ColumnRef))
	{
		if (l->n == l->cap)
		{
			l->cap = l->cap ? l->cap * 2 : 8;
			l->refs = l->refs ? repalloc_array(l->refs, ColumnRef *, l->cap) :
				palloc_array(ColumnRef *, l->cap);
		}
		l->refs[l->n++] = (ColumnRef *) node;
		return false;
	}
	return raw_expression_tree_walker(node, collect_refs, arg);
}

static int
ref_location_cmp(const void *a, const void *b)
{
	return pg_cmp_s32((*(ColumnRef *const *) a)->location, (*(ColumnRef *const *) b)->location);
}

Node *
ws_one_expression(List *parsed)
{
	RawStmt    *raw;
	SelectStmt *select;
	ResTarget  *target;

	if (list_length(parsed) != 1)
		return NULL;
	raw = linitial_node(RawStmt, parsed);
	/* (a length only if a semicolon ended it) */
	if (raw->stmt_len != 0 || !IsA(raw->stmt, SelectStmt))
		return NULL;
	select = (SelectStmt *) raw->stmt;
	if (list_length(select->targetList) != 1 ||
		select->fromClause != NIL || select->whereClause != NULL ||
		select->groupClause != NIL || select->havingClause != NULL ||
		select->windowClause != NIL || select->sortClause != NIL ||
		select->limitOffset != NULL || select->limitCount != NULL ||
		select->lockingClause != NIL || select->withClause != NULL ||
		select->distinctClause != NIL || select->valuesLists != NIL ||
		select->intoClause != NULL || select->op != SETOP_NONE)
		return NULL;
	target = linitial_node(ResTarget, select->targetList);
	return target->name == NULL ? target->val : NULL;
}

void
ws_tags_template(const char *expression, WsTagOfFn tag_of, void *arg, Buf *out)
{
	static const char prefix[] = "SELECT (";
	char	   *sql = psprintf("%s%s)", prefix, expression);
	List	   *parsed = raw_parser(sql, RAW_PARSE_DEFAULT);
	RefList		refs = {0};
	const char *at = expression;

	if (ws_one_expression(parsed) == NULL)
		elog(ERROR, "not one expression: \"%s\"", expression);
	(void) collect_refs(linitial_node(RawStmt, parsed)->stmt, &refs);
	if (refs.n > 1)
		qsort(refs.refs, refs.n, sizeof(ColumnRef *), ref_location_cmp);
	for (int i = 0; i < refs.n; i++)
	{
		ColumnRef  *ref = refs.refs[i];
		const char *where = expression + (ref->location - (int) strlen(prefix));
		const char *name;
		const char *quoted;
		size_t		len;
		uint64		tag;
		char		num[32];

		if (list_length(ref->fields) != 1 || !IsA(linitial(ref->fields), String))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("an expression with a reference that is not one column's name cannot be read yet: \"%s\"",
							expression)));
		name = strVal(linitial(ref->fields));
		if (!tag_of(arg, name, &tag))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("an expression that refers to \"%s\", no column of its table, cannot be read yet: \"%s\"",
							name, expression)));
		quoted = quote_identifier(name);
		len = strlen(quoted);
		/* (as the catalogs print it, so that nothing else is replaced) */
		if (where < at || strncmp(where, quoted, len) != 0)
			elog(ERROR, "a column reference not as printed: \"%s\"", expression);
		escape_span(at, (size_t) (where - at), out);
		snprintf(num, sizeof(num), "@{" UINT64_FORMAT "}", tag);
		buf_append(out, num, strlen(num));
		at = where + len;
		if (quoted != name)
			pfree((char *) quoted);
	}
	ws_tags_escape(at, out);
	pfree(sql);
	if (refs.refs)
		pfree(refs.refs);
}

/*
 * The reference at p ("@{"), its tag in *tag and what follows it in *after:
 * false if it is not one, or not in the tag's one form.
 */
static bool
parse_reference(const char *p, uint64 *tag, const char **after)
{
	const char *q = p + 2;

	*tag = 0;
	if (!isdigit((unsigned char) *q) || (q[0] == '0' && isdigit((unsigned char) q[1])))
		return false;
	for (; isdigit((unsigned char) *q); q++)
	{
		uint64		d = (uint64) (*q - '0');

		if (*tag > (PG_UINT64_MAX - d) / 10)
			return false;
		*tag = *tag * 10 + d;
	}
	if (*q != '}')
		return false;
	*after = q + 1;
	return true;
}

bool
ws_tags_well_formed(const char *stored)
{
	for (const char *p = stored; *p;)
	{
		uint64		tag;

		if (*p != '@')
			p++;
		else if (p[1] == '@')
			p += 2;
		else if (p[1] == '{' && parse_reference(p, &tag, &p))
			continue;
		else
			return false;
	}
	return true;
}

pg_noreturn static void
malformed(const char *stored)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("malformed column references in stored text: \"%s\"", stored)));
}

bool
ws_tags_render(const char *stored, WsTagNameFn name, void *arg, Buf *out, uint64 *missing)
{
	for (const char *p = stored; *p;)
	{
		uint64		tag = 0;
		const char *after;
		const char *n;
		const char *quoted;

		if (*p != '@')
		{
			buf_append_byte(out, (uint8) *p++);
			continue;
		}
		if (p[1] == '@')
		{
			buf_append_byte(out, '@');
			p += 2;
			continue;
		}
		if (p[1] != '{' || !parse_reference(p, &tag, &after))
			malformed(stored);
		n = name(arg, tag);
		if (n == NULL)
		{
			*missing = tag;
			return false;
		}
		quoted = quote_identifier(n);
		buf_append(out, quoted, strlen(quoted));
		if (quoted != n)
			pfree((char *) quoted);
		p = after;
	}
	return true;
}
