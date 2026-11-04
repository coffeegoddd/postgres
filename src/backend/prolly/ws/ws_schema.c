/*-------------------------------------------------------------------------
 *
 * ws_schema.c
 *	  The working-set model's table schemas (prolly/ws/ws_schema.h), and
 *	  "binary", their codec.
 *
 * version byte, name, uvarint ncols, then each column: uvarint tag, name, type,
 * kind byte, not-null constraint's name, identity byte and if it has one its
 * sequence's name, default, collation; then the key's name, uvarint nkey and the
 * key's tags; then uvarint nconstraints and each constraint: name, kind byte,
 * flags byte, and a check's expression, a unique constraint's uvarint ncols and
 * tags, or an exclusion's method and index body; then uvarint nindexes and each
 * index: name, method, flags byte and index body.  An index body is uvarint nkeys
 * and each key (uvarint tag, 0 for an expression's, expression if so, collation,
 * opclass, order byte, nulls byte, and an exclusion's operator), uvarint ninclude
 * and tags, predicate.  A string is its uvarint length and bytes, in its shortest
 * form; a default, collation, not-null constraint's or key's name none is empty,
 * which none is if it is there.  A partitioned table goes on: its kind byte,
 * uvarint nkeys and each key (uvarint tag, 0 for an expression's, expression if
 * so, collation, opclass); a partition with 'o', its table's name and bound; and
 * a table inheriting with 'i', uvarint nparents and names, uvarint ncols and tags,
 * uvarint nconstraints and names, of what it has only by inheriting; any other
 * ends there.
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
#include "catalog/pg_constraint.h"
#include "catalog/pg_partitioned_table.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/miscnodes.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "parser/parse_type.h"
#include "parser/parser.h"
#include "prolly/branch_catalog.h"
#include "prolly/ws/ws_fk.h"
#include "prolly/ws/ws_schema.h"
#include "prolly/ws/ws_sql.h"
#include "prolly/ws/ws_tags.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/regproc.h"
#include "utils/ruleutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#define WS_SCHEMA_VERSION 1

const WsSchemaCodecRoutine *WsSchemaCodec = &BinaryWsSchemaCodec;

/* (a commit's bytes are the record's, whatever the parser calls these) */
StaticAssertDecl(WS_PARTITION_RANGE == PARTITION_STRATEGY_RANGE, "partition kinds differ");
StaticAssertDecl(WS_PARTITION_LIST == PARTITION_STRATEGY_LIST, "partition kinds differ");
StaticAssertDecl(WS_PARTITION_HASH == PARTITION_STRATEGY_HASH, "partition kinds differ");

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
	return na == nb && (na == 0 || memcmp(a, b, (size_t) na * sizeof(uint64)) == 0);
}

static bool
same_text(const char *a, const char *b)
{
	return a == NULL || b == NULL ? a == b : strcmp(a, b) == 0;
}

/* Whether CREATE TABLE would keep one index of the two (both exclusions). */
static bool
exclusions_alike(const WsConstraint *a, const WsConstraint *b)
{
	if (a->deferrable != b->deferrable || a->initdeferred != b->initdeferred ||
		strcmp(a->excl.method, b->excl.method) != 0 || a->excl.nkeys != b->excl.nkeys ||
		!same_tags(a->excl.include, a->excl.ninclude, b->excl.include, b->excl.ninclude) ||
		!same_text(a->excl.predicate, b->excl.predicate))
		return false;
	for (int k = 0; k < a->excl.nkeys; k++)
	{
		const WsIndexKey *x = &a->excl.keys[k];
		const WsIndexKey *y = &b->excl.keys[k];

		if (x->tag != y->tag || x->order != y->order || x->nulls != y->nulls ||
			!same_text(x->expr, y->expr) || !same_text(x->collation, y->collation) ||
			!same_text(x->opclass, y->opclass) || !same_text(x->op, y->op))
			return false;
	}
	return true;
}

/* What any key of an index or a partitioning has to be. */
static const char *
index_key_problem(const WsTableSchema *s, const WsIndexKey *key)
{
	if ((key->tag != 0) == (key->expr != NULL))
		return "a key neither a column nor an expression, or both";
	if (key->tag != 0)
	{
		if (ws_schema_column(s, key->tag) == NULL)
			return "a key with a column the table does not have";
	}
	else
	{
		if (key->expr[0] == '\0' || !ws_tags_well_formed(key->expr))
			return "an expression with malformed column references";
		if (!refers_to_own_columns(s, key->expr))
			return "an expression referring to a column the table does not have";
	}
	if ((key->collation != NULL && key->collation[0] == '\0') ||
		(key->opclass != NULL && key->opclass[0] == '\0'))
		return "a key with an empty collation or opclass";
	return NULL;
}

static const char *
inheritance_problem(const WsTableSchema *s)
{
	int			last_col = -1;

	if (s->ninherits < 0 || s->ninherited_cols < 0 || s->ninherited_cons < 0)
		return "a negative number of parents";
	if (s->ninherits == 0 && (s->ninherited_cols > 0 || s->ninherited_cons > 0))
		return "something inherited with no parents";
	if (s->ninherits > WS_MAX_PARENTS)
		return "more parents than a commit holds";
	if (s->ninherits > 0 && s->partition_of != NULL)
		return "a partition that inherits as well";
	if (s->ninherits > 0 && s->partition != WS_PARTITION_NONE)
		return "a partitioned table that inherits as well";
	for (int i = 0; i < s->ninherits; i++)
	{
		if (s->inherits[i] == NULL || s->inherits[i][0] == '\0' || strlen(s->inherits[i]) >= NAMEDATALEN)
			return "a parent of no name, or too long a one";
		if (strcmp(s->inherits[i], s->name) == 0)
			return "a table that inherits from itself";
		for (int j = 0; j < i; j++)
			if (strcmp(s->inherits[j], s->inherits[i]) == 0)
				return "a parent twice";
	}
	for (int i = 0; i < s->ninherited_cols; i++)
	{
		int			at = -1;

		for (int c = 0; c < s->ncols && at < 0; c++)
			if (s->cols[c].tag == s->inherited_cols[i])
				at = c;
		if (at < 0)
			return "an inherited column the table does not have";
		if (at <= last_col)
			return "inherited columns not in attribute order";
		last_col = at;
	}
	for (int i = 0; i < s->ninherited_cons; i++)
	{
		const char *name = s->inherited_cons[i];
		bool		found = false;

		if (name == NULL || name[0] == '\0')
			return "an inherited constraint of no name";
		if (i > 0 && strcmp(s->inherited_cons[i - 1], name) >= 0)
			return "inherited constraints not in order by name";
		/* (the table's constraints are in order by name, already checked) */
		{
			int			lo = 0,
						hi = s->nconstraints - 1;

			while (lo <= hi && !found)
			{
				int			mid = lo + (hi - lo) / 2;
				int			cmp = strcmp(name, s->constraints[mid].name);

				if (cmp == 0)
					found = s->constraints[mid].kind == WS_CONSTRAINT_CHECK;
				if (cmp == 0)
					break;
				if (cmp < 0)
					hi = mid - 1;
				else
					lo = mid + 1;
			}
		}
		for (int c = 0; c < s->ncols && !found; c++)
			found = s->cols[c].not_null != NULL && strcmp(s->cols[c].not_null, name) == 0;
		if (!found)
			return "an inherited constraint the table does not have";
	}
	return NULL;
}

static const char *
partitioning_problem(const WsTableSchema *s)
{
	if ((s->partition_of != NULL) != (s->partition_bound != NULL))
		return "a partition with no table or no bound";
	if (s->partition_of != NULL)
	{
		if (s->partition_of[0] == '\0' || strlen(s->partition_of) >= NAMEDATALEN)
			return "a partition of a table of no name, or too long a one";
		if (strcmp(s->partition_of, s->name) == 0)
			return "a partition of itself";
		if (s->partition_bound[0] == '\0')
			return "a partition with an empty bound";
		for (int i = 0; i < s->ncols; i++)
			if (s->cols[i].identity != WS_IDENTITY_NONE)
				return "a partition with an identity, which attaching gives it";
	}
	if (s->partition == WS_PARTITION_NONE)
		return s->npartition == 0 ? NULL : "partition keys with no partitioning";
	if (s->partition != WS_PARTITION_RANGE && s->partition != WS_PARTITION_LIST &&
		s->partition != WS_PARTITION_HASH)
		return "a partitioning of no known kind";
	if (s->npartition <= 0 || s->npartition > PARTITION_MAX_KEYS)
		return "a partitioning of no keys, or too many";
	if (s->partition == WS_PARTITION_LIST && s->npartition != 1)
		return "a list partitioning of more than one key";
	for (int k = 0; k < s->npartition; k++)
	{
		const WsIndexKey *key = &s->partition_keys[k];
		const char *problem = index_key_problem(s, key);

		if (problem != NULL)
			return problem;
		if (key->order != WS_INDEX_ORDER_DEFAULT || key->nulls != WS_INDEX_NULLS_DEFAULT ||
			key->op != NULL)
			return "a partition key with an order or an operator";
	}
	return NULL;
}

/* What an index and an exclusion's index have to be alike. */
static const char *
index_body_problem(const WsTableSchema *s, const WsIndex *ix, bool exclusion)
{
	if (ix->method == NULL || ix->method[0] == '\0' || strlen(ix->method) >= NAMEDATALEN)
		return "an index with no method";
	if (ix->nulls_not_distinct && !ix->unique)
		return "NULLS NOT DISTINCT on an index not unique";
	if (ix->nkeys <= 0 || ix->nkeys > INDEX_MAX_KEYS)
		return "an index of no keys, or too many";
	if (ix->ninclude < 0 || ix->nkeys + ix->ninclude > INDEX_MAX_KEYS)
		return "an index of too many columns";
	for (int k = 0; k < ix->nkeys; k++)
	{
		const WsIndexKey *key = &ix->keys[k];
		const char *problem;

		if ((key->op != NULL) != exclusion)
			return exclusion ? "an exclusion's key with no operator" : "an index's key with an operator";
		if (exclusion && key->op[0] == '\0')
			return "an exclusion's key with no operator";
		problem = index_key_problem(s, key);
		if (problem != NULL)
			return problem;
		if (key->order != WS_INDEX_ORDER_DEFAULT && key->order != WS_INDEX_ORDER_DESC)
			return "an index's key of no known order";
		if (key->nulls != WS_INDEX_NULLS_DEFAULT && key->nulls != WS_INDEX_NULLS_FIRST &&
			key->nulls != WS_INDEX_NULLS_LAST)
			return "an index's key of no known order of nulls";
		/* (one index, one set of bytes: its default order of nulls is not said) */
		if (key->nulls == (key->order == WS_INDEX_ORDER_DESC ? WS_INDEX_NULLS_FIRST :
						   WS_INDEX_NULLS_LAST))
			return "an index's key saying its default order of nulls";
	}
	for (int k = 0; k < ix->ninclude; k++)
		if (ws_schema_column(s, ix->include[k]) == NULL)
			return "an index including a column the table does not have";
	if (ix->predicate != NULL)
	{
		if (ix->predicate[0] == '\0' || !ws_tags_well_formed(ix->predicate))
			return "an expression with malformed column references";
		if (!refers_to_own_columns(s, ix->predicate))
			return "an expression referring to a column the table does not have";
	}
	return NULL;
}

/*
 * A name a column's not-null constraint or sequence has, taken by another of
 * its kind: they could not stand together.  (All else already well formed.)
 */
static const char *
column_names_problem(const WsTableSchema *s)
{
	for (int i = 0; i < s->ncols; i++)
	{
		const char *not_null = s->cols[i].not_null;
		const char *seq = s->cols[i].identity_seq;

		CHECK_FOR_INTERRUPTS();

		if (not_null != NULL && s->key_name != NULL && strcmp(not_null, s->key_name) == 0)
			return "a not-null constraint named as the key is";
		if (seq != NULL && (strcmp(seq, s->name) == 0 ||
							(s->key_name != NULL && strcmp(seq, s->key_name) == 0)))
			return "a sequence named as the table or its key is";
		for (int c = 0; c < s->nconstraints; c++)
		{
			if (not_null != NULL && strcmp(not_null, s->constraints[c].name) == 0)
				return "a not-null constraint named as another constraint is";
			if (seq != NULL && s->constraints[c].kind != WS_CONSTRAINT_CHECK &&
				strcmp(seq, s->constraints[c].name) == 0)
				return "a sequence named as a constraint with an index is";
		}
		for (int x = 0; x < s->nindexes && seq != NULL; x++)
			if (strcmp(seq, s->indexes[x].name) == 0)
				return "a sequence named as an index is";
	}
	return NULL;
}

static const char *
index_problem(const WsTableSchema *s, const WsIndex *ix)
{
	if (ix->name == NULL || ix->name[0] == '\0')
		return "an index with no name";
	if (strlen(ix->name) >= NAMEDATALEN)
		return "an index's name too long";
	if (s->key_name != NULL && strcmp(ix->name, s->key_name) == 0)
		return "an index named as the key is";
	/* (a constraint's index has its name; a check has no relation) */
	for (int c = 0; c < s->nconstraints; c++)
		if (s->constraints[c].kind != WS_CONSTRAINT_CHECK &&
			strcmp(ix->name, s->constraints[c].name) == 0)
			return "an index named as a constraint with an index is";
	return index_body_problem(s, ix, false);
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
		if (c->not_null != NULL && (c->not_null[0] == '\0' || strlen(c->not_null) >= NAMEDATALEN))
			return "a not-null constraint with no name, or too long a one";
		if (c->kind != WS_COL_STORED && (c->def == NULL || c->def[0] == '\0'))
			return "a generated column with no expression";
		if (c->kind != WS_COL_STORED && c->identity != WS_IDENTITY_NONE)
			return "a generated column that is an identity";
		if (c->identity != WS_IDENTITY_NONE && c->def != NULL)
			return "an identity column with a default";
		if (c->identity != WS_IDENTITY_NONE && !c->not_null)
			return "an identity column that may be null";
		if ((c->identity != WS_IDENTITY_NONE) != (c->identity_seq != NULL))
			return "an identity with no sequence, or a sequence with no identity";
		if (c->identity_seq != NULL &&
			(c->identity_seq[0] == '\0' || strlen(c->identity_seq) >= NAMEDATALEN))
			return "an identity's sequence with no name, or too long a one";
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
			if (c->not_null != NULL && s->cols[j].not_null != NULL &&
				strcmp(s->cols[j].not_null, c->not_null) == 0)
				return "two not-null constraints with one name";
			if (c->identity_seq != NULL && s->cols[j].identity_seq != NULL &&
				strcmp(s->cols[j].identity_seq, c->identity_seq) == 0)
				return "two sequences with one name";
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

		/* (their pairs below: a record from anywhere can hold a great many) */
		CHECK_FOR_INTERRUPTS();
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
		if (con->kind != WS_CONSTRAINT_CHECK &&
			(con->not_valid || con->no_inherit || con->not_enforced))
			return "a constraint with a check's flags";
		if (con->kind != WS_CONSTRAINT_EXCLUSION &&
			(con->excl.name != NULL || con->excl.method != NULL || con->excl.nkeys != 0 ||
			 con->excl.ninclude != 0 || con->excl.predicate != NULL ||
			 con->excl.unique || con->excl.nulls_not_distinct))
			return "a constraint with an exclusion's fields";
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
			/* (one check, one set of bytes: not enforced says the other) */
			if (con->not_valid && con->not_enforced)
				return "a check both not valid and not enforced";
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
		else if (con->kind == WS_CONSTRAINT_EXCLUSION)
		{
			const char *problem;

			if (con->expr != NULL || con->ncols != 0 || con->nulls_not_distinct)
				return "an exclusion with another constraint's fields";
			if (con->excl.name != NULL || con->excl.unique || con->excl.nulls_not_distinct)
				return "an exclusion's index with a name, or unique";
			problem = index_body_problem(s, &con->excl, true);
			if (problem != NULL)
				return problem;
			for (int j = 0; j < i; j++)
				if (s->constraints[j].kind == WS_CONSTRAINT_EXCLUSION &&
					exclusions_alike(con, &s->constraints[j]))
					return "two exclusions alike";
		}
		else
			return "a constraint of no known kind";
	}
	if (s->nindexes < 0)
		return "a negative number of indexes";
	for (int i = 0; i < s->nindexes; i++)
	{
		const char *problem;

		CHECK_FOR_INTERRUPTS();
		problem = index_problem(s, &s->indexes[i]);
		if (problem != NULL)
			return problem;
		if (i > 0 && strcmp(s->indexes[i - 1].name, s->indexes[i].name) >= 0)
			return "indexes not in order by name";
	}
	{
		const char *problem = partitioning_problem(s);

		if (problem != NULL)
			return problem;
		problem = inheritance_problem(s);
		if (problem != NULL)
			return problem;
	}
	return column_names_problem(s);
}

#define WS_PARTITION_OF 'o'
#define WS_INHERITS 'i'
#define CON_NULLS_NOT_DISTINCT 0x01
#define CON_DEFERRABLE 0x02
#define CON_INITDEFERRED 0x04
#define CON_NOT_VALID 0x08
#define CON_NO_INHERIT 0x10
#define CON_NOT_ENFORCED 0x20
#define IDX_UNIQUE 0x01
#define IDX_NULLS_NOT_DISTINCT 0x02

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
put_index_body(Buf *out, const WsIndex *ix, bool exclusion)
{
	buf_append_uvarint(out, (uint64) ix->nkeys);
	for (int k = 0; k < ix->nkeys; k++)
	{
		const WsIndexKey *key = &ix->keys[k];

		buf_append_uvarint(out, key->tag);
		if (key->tag == 0)
			put_string(out, key->expr);
		put_optional(out, key->collation);
		put_optional(out, key->opclass);
		buf_append_byte(out, (uint8) key->order);
		buf_append_byte(out, (uint8) key->nulls);
		if (exclusion)
			put_string(out, key->op);
	}
	buf_append_uvarint(out, (uint64) ix->ninclude);
	for (int k = 0; k < ix->ninclude; k++)
		buf_append_uvarint(out, ix->include[k]);
	put_optional(out, ix->predicate);
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
		put_optional(out, c->not_null);
		buf_append_byte(out, (uint8) c->identity);
		if (c->identity != WS_IDENTITY_NONE)
			put_string(out, c->identity_seq);
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
						(con->initdeferred ? CON_INITDEFERRED : 0) |
						(con->not_valid ? CON_NOT_VALID : 0) |
						(con->no_inherit ? CON_NO_INHERIT : 0) |
						(con->not_enforced ? CON_NOT_ENFORCED : 0));
		if (con->kind == WS_CONSTRAINT_CHECK)
			put_string(out, con->expr);
		else if (con->kind == WS_CONSTRAINT_EXCLUSION)
		{
			put_string(out, con->excl.method);
			put_index_body(out, &con->excl, true);
		}
		else
		{
			buf_append_uvarint(out, (uint64) con->ncols);
			for (int k = 0; k < con->ncols; k++)
				buf_append_uvarint(out, con->cols[k]);
		}
	}
	buf_append_uvarint(out, (uint64) s->nindexes);
	for (int i = 0; i < s->nindexes; i++)
	{
		const WsIndex *ix = &s->indexes[i];

		put_string(out, ix->name);
		put_string(out, ix->method);
		buf_append_byte(out, (ix->unique ? IDX_UNIQUE : 0) |
						(ix->nulls_not_distinct ? IDX_NULLS_NOT_DISTINCT : 0));
		put_index_body(out, ix, false);
	}
	if (s->partition != WS_PARTITION_NONE)
	{
		buf_append_byte(out, (uint8) s->partition);
		buf_append_uvarint(out, (uint64) s->npartition);
		for (int k = 0; k < s->npartition; k++)
		{
			const WsIndexKey *key = &s->partition_keys[k];

			buf_append_uvarint(out, key->tag);
			if (key->tag == 0)
				put_string(out, key->expr);
			put_optional(out, key->collation);
			put_optional(out, key->opclass);
		}
	}
	if (s->partition_of != NULL)
	{
		buf_append_byte(out, WS_PARTITION_OF);
		put_string(out, s->partition_of);
		put_string(out, s->partition_bound);
	}
	if (s->ninherits > 0)
	{
		buf_append_byte(out, WS_INHERITS);
		buf_append_uvarint(out, (uint64) s->ninherits);
		for (int i = 0; i < s->ninherits; i++)
			put_string(out, s->inherits[i]);
		buf_append_uvarint(out, (uint64) s->ninherited_cols);
		for (int i = 0; i < s->ninherited_cols; i++)
			buf_append_uvarint(out, s->inherited_cols[i]);
		buf_append_uvarint(out, (uint64) s->ninherited_cons);
		for (int i = 0; i < s->ninherited_cons; i++)
			put_string(out, s->inherited_cons[i]);
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
get_index_body(const uint8 **p, const uint8 *end, WsIndex *ix, bool exclusion)
{
	uint64		n = get_uvarint(p, end);

	if (n > INDEX_MAX_KEYS)
		malformed("an index of too many keys");
	ix->nkeys = (int) n;
	ix->keys = px_malloc((size_t) Max(n, 1) * sizeof(WsIndexKey));
	memset(ix->keys, 0, (size_t) Max(n, 1) * sizeof(WsIndexKey));
	for (int k = 0; k < ix->nkeys; k++)
	{
		WsIndexKey *key = &ix->keys[k];

		key->tag = get_uvarint(p, end);
		if (key->tag == 0)
			key->expr = get_string(p, end);
		key->collation = get_optional(p, end);
		key->opclass = get_optional(p, end);
		key->order = (char) get_byte(p, end);
		key->nulls = (char) get_byte(p, end);
		if (exclusion)
			key->op = get_string(p, end);
	}
	n = get_uvarint(p, end);
	if (n > INDEX_MAX_KEYS)
		malformed("an index of too many columns");
	ix->ninclude = (int) n;
	ix->include = px_malloc((size_t) Max(n, 1) * sizeof(uint64));
	for (int k = 0; k < ix->ninclude; k++)
		ix->include[k] = get_uvarint(p, end);
	ix->predicate = get_optional(p, end);
}

static void
schema_decode(const uint8 *bytes, size_t len, WsTableSchema *s)
{
	const uint8 *p = bytes,
			   *end = bytes + len;
	uint64		n;
	uint64		nconstraints;
	uint64		capacity;
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

		c->tag = get_uvarint(&p, end);
		c->name = get_string(&p, end);
		c->type = get_string(&p, end);
		c->kind = (char) get_byte(&p, end);
		c->not_null = get_optional(&p, end);
		c->identity = (char) get_byte(&p, end);
		if (c->identity != WS_IDENTITY_NONE)
			c->identity_seq = get_string(&p, end);
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
	nconstraints = n;
	/* (grown as they are read: each is far bigger here than its four bytes there) */
	capacity = Max(Min(nconstraints, 16), 1);
	s->constraints = px_malloc((size_t) capacity * sizeof(WsConstraint));
	memset(s->constraints, 0, (size_t) capacity * sizeof(WsConstraint));
	for (uint64 i = 0; i < nconstraints; i++)
	{
		WsConstraint *con;
		uint8		flags;

		if (i == capacity)
		{
			uint64		grown = Min(capacity * 2, nconstraints);

			s->constraints = px_realloc(s->constraints, (size_t) grown * sizeof(WsConstraint));
			memset(s->constraints + capacity, 0, (size_t) (grown - capacity) * sizeof(WsConstraint));
			capacity = grown;
		}
		s->nconstraints = (int) i + 1;
		con = &s->constraints[i];

		con->name = get_string(&p, end);
		con->kind = (char) get_byte(&p, end);
		flags = get_byte(&p, end);
		if (flags & ~(CON_NULLS_NOT_DISTINCT | CON_DEFERRABLE | CON_INITDEFERRED |
					  CON_NOT_VALID | CON_NO_INHERIT | CON_NOT_ENFORCED))
			malformed("a constraint's flags not known");
		con->nulls_not_distinct = (flags & CON_NULLS_NOT_DISTINCT) != 0;
		con->deferrable = (flags & CON_DEFERRABLE) != 0;
		con->initdeferred = (flags & CON_INITDEFERRED) != 0;
		con->not_valid = (flags & CON_NOT_VALID) != 0;
		con->no_inherit = (flags & CON_NO_INHERIT) != 0;
		con->not_enforced = (flags & CON_NOT_ENFORCED) != 0;
		if (con->kind == WS_CONSTRAINT_CHECK)
			con->expr = get_string(&p, end);
		else if (con->kind == WS_CONSTRAINT_EXCLUSION)
		{
			con->excl.method = get_string(&p, end);
			get_index_body(&p, end, &con->excl, true);
		}
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
	n = get_uvarint(&p, end);
	/* (each is at least six bytes: its name, method, flags and counts) */
	if (n > (uint64) (end - p) / 6)
		malformed("more indexes than bytes");
	s->nindexes = (int) n;
	s->indexes = px_malloc((size_t) Max(n, 1) * sizeof(WsIndex));
	memset(s->indexes, 0, (size_t) Max(n, 1) * sizeof(WsIndex));
	for (int i = 0; i < s->nindexes; i++)
	{
		WsIndex    *ix = &s->indexes[i];
		uint8		flags;

		ix->name = get_string(&p, end);
		ix->method = get_string(&p, end);
		flags = get_byte(&p, end);
		if (flags & ~(IDX_UNIQUE | IDX_NULLS_NOT_DISTINCT))
			malformed("an index's flags not known");
		ix->unique = (flags & IDX_UNIQUE) != 0;
		ix->nulls_not_distinct = (flags & IDX_NULLS_NOT_DISTINCT) != 0;
		get_index_body(&p, end, ix, false);
	}
	if (p != end && *p != WS_PARTITION_OF && *p != WS_INHERITS)
	{
		s->partition = (char) get_byte(&p, end);
		if (s->partition == WS_PARTITION_NONE)
			malformed("it has bytes left over");
		n = get_uvarint(&p, end);
		if (n > PARTITION_MAX_KEYS)
			malformed("a partitioning of too many keys");
		s->npartition = (int) n;
		s->partition_keys = px_malloc((size_t) Max(n, 1) * sizeof(WsIndexKey));
		memset(s->partition_keys, 0, (size_t) Max(n, 1) * sizeof(WsIndexKey));
		for (int k = 0; k < s->npartition; k++)
		{
			WsIndexKey *key = &s->partition_keys[k];

			key->tag = get_uvarint(&p, end);
			if (key->tag == 0)
				key->expr = get_string(&p, end);
			key->collation = get_optional(&p, end);
			key->opclass = get_optional(&p, end);
			key->order = WS_INDEX_ORDER_DEFAULT;
			key->nulls = WS_INDEX_NULLS_DEFAULT;
		}
	}
	if (p != end && *p == WS_PARTITION_OF)
	{
		p++;
		s->partition_of = get_string(&p, end);
		s->partition_bound = get_string(&p, end);
	}
	if (p != end && *p == WS_INHERITS)
	{
		p++;
		n = get_uvarint(&p, end);
		if (n == 0)
			malformed("it inherits from nothing, at length");
		if (n > WS_MAX_PARENTS)
			malformed("more parents than a commit holds");
		/* (each is at least a byte, its length) */
		if (n > (uint64) (end - p))
			malformed("more parents than bytes");
		s->inherits = px_malloc((size_t) n * sizeof(char *));
		memset(s->inherits, 0, (size_t) n * sizeof(char *));
		s->ninherits = (int) n;
		for (int i = 0; i < s->ninherits; i++)
			s->inherits[i] = get_string(&p, end);
		n = get_uvarint(&p, end);
		if (n > (uint64) s->ncols)
			malformed("more inherited columns than the table has");
		s->inherited_cols = px_malloc((size_t) Max(n, 1) * sizeof(uint64));
		s->ninherited_cols = (int) n;
		for (int i = 0; i < s->ninherited_cols; i++)
			s->inherited_cols[i] = get_uvarint(&p, end);
		n = get_uvarint(&p, end);
		if (n > (uint64) s->ncols + (uint64) s->nconstraints)
			malformed("more inherited constraints than the table has");
		if (n > (uint64) (end - p))
			malformed("more inherited constraints than bytes");
		s->inherited_cons = px_malloc((size_t) Max(n, 1) * sizeof(char *));
		memset(s->inherited_cons, 0, (size_t) Max(n, 1) * sizeof(char *));
		s->ninherited_cons = (int) n;
		for (int i = 0; i < s->ninherited_cons; i++)
			s->inherited_cons[i] = get_string(&p, end);
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

static void
index_free(WsIndex *ix)
{
	px_free(ix->name);
	px_free(ix->method);
	for (int k = 0; k < ix->nkeys; k++)
	{
		px_free(ix->keys[k].expr);
		px_free(ix->keys[k].collation);
		px_free(ix->keys[k].opclass);
		px_free(ix->keys[k].op);
	}
	px_free(ix->keys);
	px_free(ix->include);
	px_free(ix->predicate);
}

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
		px_free(s->cols[i].not_null);
		px_free(s->cols[i].identity_seq);
	}
	px_free(s->cols);
	px_free(s->key_name);
	px_free(s->key);
	for (int i = 0; i < s->nconstraints; i++)
	{
		px_free(s->constraints[i].name);
		px_free(s->constraints[i].expr);
		px_free(s->constraints[i].cols);
		index_free(&s->constraints[i].excl);
	}
	px_free(s->constraints);
	for (int i = 0; i < s->nindexes; i++)
		index_free(&s->indexes[i]);
	px_free(s->indexes);
	for (int k = 0; k < s->npartition; k++)
	{
		px_free(s->partition_keys[k].expr);
		px_free(s->partition_keys[k].collation);
		px_free(s->partition_keys[k].opclass);
	}
	px_free(s->partition_keys);
	px_free(s->partition_of);
	px_free(s->partition_bound);
	for (int i = 0; i < s->ninherits; i++)
		px_free(s->inherits[i]);
	px_free(s->inherits);
	px_free(s->inherited_cols);
	for (int i = 0; i < s->ninherited_cons; i++)
		px_free(s->inherited_cons[i]);
	px_free(s->inherited_cons);
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

void
ws_put_text(Buf *out, const char *text)
{
	buf_append(out, text, strlen(text));
}

void
ws_put_identifier(Buf *out, const char *name)
{
	const char *quoted = quote_identifier(name);

	ws_put_text(out, quoted);
	if (quoted != name)
		pfree((char *) quoted);
}

/* An index's keys and INCLUDE, in the order both statements have them. */
static void
put_index_columns(const WsTableSchema *s, const WsIndex *ix, Buf *out)
{
	ws_put_text(out, " (");
	for (int k = 0; k < ix->nkeys; k++)
	{
		const WsIndexKey *key = &ix->keys[k];

		if (k > 0)
			ws_put_text(out, ", ");
		if (key->tag != 0)
			ws_put_identifier(out, ws_schema_column(s, key->tag)->name);
		else
		{
			ws_put_text(out, "(");
			put_expression(s, key->expr, out);
			ws_put_text(out, ")");
		}
		if (key->collation != NULL)
		{
			ws_put_text(out, " COLLATE ");
			ws_put_text(out, key->collation);
		}
		if (key->opclass != NULL)
		{
			ws_put_text(out, " ");
			ws_put_text(out, key->opclass);
		}
		if (key->order != WS_INDEX_ORDER_DEFAULT)
			ws_put_text(out, " DESC");
		if (key->nulls != WS_INDEX_NULLS_DEFAULT)
			ws_put_text(out, key->nulls == WS_INDEX_NULLS_FIRST ? " NULLS FIRST" : " NULLS LAST");
		if (key->op != NULL)
		{
			ws_put_text(out, " WITH OPERATOR(");
			ws_put_text(out, key->op);
			ws_put_text(out, ")");
		}
	}
	ws_put_text(out, ")");
	if (ix->ninclude > 0)
	{
		ws_put_text(out, " INCLUDE (");
		for (int k = 0; k < ix->ninclude; k++)
		{
			if (k > 0)
				ws_put_text(out, ", ");
			ws_put_identifier(out, ws_schema_column(s, ix->include[k])->name);
		}
		ws_put_text(out, ")");
	}
}

static void
put_index_predicate(const WsTableSchema *s, const WsIndex *ix, Buf *out)
{
	if (ix->predicate != NULL)
	{
		ws_put_text(out, " WHERE (");
		put_expression(s, ix->predicate, out);
		ws_put_text(out, ")");
	}
}

/* A check's clause, but for NOT VALID. */
static void
put_check(const WsTableSchema *s, const WsConstraint *con, Buf *out)
{
	ws_put_text(out, " CHECK (");
	put_expression(s, con->expr, out);
	ws_put_text(out, ")");
	if (con->no_inherit)
		ws_put_text(out, " NO INHERIT");
	if (con->not_enforced)
		ws_put_text(out, " NOT ENFORCED");
}

/* The statement, as the record has it. */
static void
write_create_table(const WsTableSchema *s, const char *schema, Buf *out)
{
	bool		any = false;

	ws_put_text(out, "CREATE TABLE ");
	ws_put_identifier(out, schema);
	ws_put_text(out, ".");
	ws_put_identifier(out, s->name);
	ws_put_text(out, " (");
	for (int i = 0; i < s->ncols; i++)
	{
		const WsColumn *c = &s->cols[i];

		ws_put_text(out, any ? ",\n    " : "\n    ");
		any = true;
		ws_put_identifier(out, c->name);
		ws_put_text(out, " ");
		ws_put_text(out, c->type);
		if (c->collation != NULL)
		{
			ws_put_text(out, " COLLATE ");
			ws_put_text(out, c->collation);
		}
		if (c->kind != WS_COL_STORED)
		{
			ws_put_text(out, " GENERATED ALWAYS AS (");
			put_expression(s, c->def, out);
			ws_put_text(out, c->kind == WS_COL_GENERATED_STORED ? ") STORED" : ") VIRTUAL");
		}
		else if (c->identity != WS_IDENTITY_NONE)
		{
			ws_put_text(out, c->identity == WS_IDENTITY_ALWAYS ? " GENERATED ALWAYS AS IDENTITY (SEQUENCE NAME " :
						" GENERATED BY DEFAULT AS IDENTITY (SEQUENCE NAME ");
			ws_put_identifier(out, schema);
			ws_put_text(out, ".");
			ws_put_identifier(out, c->identity_seq);
			ws_put_text(out, ")");
		}
		else if (c->def != NULL)
		{
			ws_put_text(out, " DEFAULT (");
			put_expression(s, c->def, out);
			ws_put_text(out, ")");
		}
		if (c->not_null != NULL)
		{
			ws_put_text(out, " CONSTRAINT ");
			ws_put_identifier(out, c->not_null);
			ws_put_text(out, " NOT NULL");
		}
	}
	if (s->nkey > 0)
	{
		ws_put_text(out, any ? ",\n    CONSTRAINT " : "\n    CONSTRAINT ");
		any = true;
		ws_put_identifier(out, s->key_name);
		ws_put_text(out, " PRIMARY KEY (");
		for (int i = 0; i < s->nkey; i++)
		{
			if (i > 0)
				ws_put_text(out, ", ");
			ws_put_identifier(out, ws_schema_column(s, s->key[i])->name);
		}
		ws_put_text(out, ")");
	}
	for (int i = 0; i < s->nconstraints; i++)
	{
		const WsConstraint *con = &s->constraints[i];

		if (con->not_valid)
			continue;
		ws_put_text(out, any ? ",\n    CONSTRAINT " : "\n    CONSTRAINT ");
		any = true;
		ws_put_identifier(out, con->name);
		if (con->kind == WS_CONSTRAINT_CHECK)
		{
			put_check(s, con, out);
			continue;
		}
		if (con->kind == WS_CONSTRAINT_EXCLUSION)
		{
			ws_put_text(out, " EXCLUDE USING ");
			ws_put_identifier(out, con->excl.method);
			put_index_columns(s, &con->excl, out);
			put_index_predicate(s, &con->excl, out);
		}
		else
		{
			ws_put_text(out, con->nulls_not_distinct ? " UNIQUE NULLS NOT DISTINCT (" : " UNIQUE (");
			for (int k = 0; k < con->ncols; k++)
			{
				if (k > 0)
					ws_put_text(out, ", ");
				ws_put_identifier(out, ws_schema_column(s, con->cols[k])->name);
			}
			ws_put_text(out, ")");
		}
		if (con->deferrable)
			ws_put_text(out, con->initdeferred ? " DEFERRABLE INITIALLY DEFERRED" : " DEFERRABLE");
	}
	ws_put_text(out, any ? "\n)" : ")");
	if (s->partition != WS_PARTITION_NONE)
	{
		ws_put_text(out, s->partition == WS_PARTITION_RANGE ? " PARTITION BY RANGE (" :
					s->partition == WS_PARTITION_LIST ? " PARTITION BY LIST (" : " PARTITION BY HASH (");
		for (int k = 0; k < s->npartition; k++)
		{
			const WsIndexKey *key = &s->partition_keys[k];

			if (k > 0)
				ws_put_text(out, ", ");
			if (key->tag != 0)
				ws_put_identifier(out, ws_schema_column(s, key->tag)->name);
			else
			{
				ws_put_text(out, "(");
				put_expression(s, key->expr, out);
				ws_put_text(out, ")");
			}
			if (key->collation != NULL)
			{
				ws_put_text(out, " COLLATE ");
				ws_put_text(out, key->collation);
			}
			if (key->opclass != NULL)
			{
				ws_put_text(out, " ");
				ws_put_text(out, key->opclass);
			}
		}
		ws_put_text(out, ")");
	}
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

/* Names as they print with no setting asking for more quotes. */
static char *
canonical_names(List *names)
{
	char	   *canonical;
	bool		quote_all = quote_all_identifiers;

	quote_all_identifiers = false;
	PG_TRY();
	{
		canonical = NameListToQuotedString(names);
	}
	PG_FINALLY();
	{
		quote_all_identifiers = quote_all;
	}
	PG_END_TRY();
	return canonical;
}

/*
 * Whether a collation's or opclass's text names these names, in its one form:
 * as they print with no setting asking for more quotes, so no spaces, nor a
 * name the grammar would cut short.
 */
static bool
same_names(const char *text, List *names)
{
	ErrorSaveContext escontext = {T_ErrorSaveContext};
	List	   *want;

	if (text == NULL)
		return names == NIL;
	want = stringToQualifiedNameList(text, (Node *) &escontext);
	if (want == NIL || !equal(want, names))
		return false;
	return strcmp(canonical_names(want), text) == 0;
}

/* An exclusion's operator in its one form: its schema's name quoted, itself bare. */
static char *
operator_text(List *names)
{
	char	   *op = strVal(llast(names));

	if (list_length(names) == 1)
		return pstrdup(op);
	return psprintf("%s.%s", canonical_names(list_copy_head(names, list_length(names) - 1)), op);
}

/* An index's key as parsed is the record's; false if not. */
static bool
same_index_key(const WsTableSchema *s, const WsIndexKey *key, const IndexElem *elem)
{
	if (elem->indexcolname != NULL || elem->opclassopts != NIL ||
		!same_names(key->collation, elem->collation) ||
		!same_names(key->opclass, elem->opclass) ||
		elem->ordering != (key->order == WS_INDEX_ORDER_DESC ? SORTBY_DESC : SORTBY_DEFAULT) ||
		elem->nulls_ordering != (key->nulls == WS_INDEX_NULLS_FIRST ? SORTBY_NULLS_FIRST :
								 key->nulls == WS_INDEX_NULLS_LAST ? SORTBY_NULLS_LAST :
								 SORTBY_NULLS_DEFAULT))
		return false;
	if (key->tag != 0)
		return elem->name != NULL && elem->expr == NULL &&
			strcmp(elem->name, ws_schema_column(s, key->tag)->name) == 0;
	if (elem->name != NULL || elem->expr == NULL)
		return false;
	check_expression(s, key->expr, elem->expr);
	return true;
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

/* Whether an identity's options are its sequence's name, in this schema, alone. */
static bool
is_sequence_named(List *options, const char *schema, const char *seq)
{
	DefElem    *only;
	List	   *names;

	if (list_length(options) != 1)
		return false;
	only = linitial_node(DefElem, options);
	if (strcmp(only->defname, "sequence_name") != 0 || !IsA(only->arg, List))
		return false;
	names = (List *) only->arg;
	return list_length(names) == 2 && strcmp(strVal(linitial(names)), schema) == 0 &&
		strcmp(strVal(lsecond(names)), seq) == 0;
}

/* A column as parsed is the record's: its name, type, collation and clauses. */
static void
check_column(const WsTableSchema *s, const WsColumn *c, const ColumnDef *def, const char *schema)
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
		if (def->collClause->arg != NULL || !same_names(c->collation, def->collClause->collname))
			not_the_record(s, "a column's collation");
	}
	if (def->storage_name != NULL || def->compression != NULL || def->fdwoptions != NIL)
		not_the_record(s, "a column's clause");
	foreach(lc, def->constraints)
	{
		Constraint *con = lfirst_node(Constraint, lc);

		if ((con->conname != NULL) != (con->contype == CONSTR_NOTNULL) ||
			con->is_no_inherit || con->deferrable || con->initdeferred)
			not_the_record(s, "a column's clause");
		switch (con->contype)
		{
			case CONSTR_NOTNULL:
				not_null++;
				if (c->not_null == NULL || strcmp(con->conname, c->not_null) != 0)
					not_the_record(s, "a column's not-null constraint");
				break;
			case CONSTR_DEFAULT:
				deflt++;
				if (c->kind != WS_COL_STORED || c->identity != WS_IDENTITY_NONE || c->def == NULL)
					not_the_record(s, "a default the column does not have");
				check_expression(s, c->def, con->raw_expr);
				break;
			case CONSTR_IDENTITY:
				identity++;
				if (c->identity == WS_IDENTITY_NONE || !is_sequence_named(con->options, schema, c->identity_seq) ||
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
		in_statement->deferrable != con->deferrable ||
		in_statement->initdeferred != con->initdeferred ||
		in_statement->options != NIL ||
		in_statement->indexspace != NULL || in_statement->indexname != NULL ||
		in_statement->without_overlaps || in_statement->pktable != NULL)
		not_the_record(s, "another constraint");
	if (con->kind != WS_CONSTRAINT_EXCLUSION &&
		(in_statement->including != NIL || in_statement->exclusions != NIL ||
		 in_statement->where_clause != NULL))
		not_the_record(s, "another constraint");
	if (con->kind == WS_CONSTRAINT_CHECK)
	{
		/* (the grammar says these of a check alone) */
		if (in_statement->contype != CONSTR_CHECK || in_statement->keys != NIL ||
			in_statement->is_no_inherit != con->no_inherit ||
			in_statement->is_enforced == con->not_enforced ||
			in_statement->skip_validation != (con->not_valid || con->not_enforced) ||
			in_statement->initially_valid == (con->not_valid || con->not_enforced))
			not_the_record(s, "another constraint");
		check_expression(s, con->expr, in_statement->raw_expr);
	}
	else if (con->kind == WS_CONSTRAINT_EXCLUSION)
	{
		const WsIndex *ix = &con->excl;
		int			k = 0;
		ListCell   *kc;

		if (in_statement->contype != CONSTR_EXCLUSION || in_statement->raw_expr != NULL ||
			in_statement->is_no_inherit || in_statement->skip_validation ||
			in_statement->keys != NIL || in_statement->nulls_not_distinct ||
			strcmp(in_statement->access_method, ix->method) != 0 ||
			list_length(in_statement->exclusions) != ix->nkeys ||
			list_length(in_statement->including) != ix->ninclude ||
			(in_statement->where_clause != NULL) != (ix->predicate != NULL))
			not_the_record(s, "another constraint");
		foreach(kc, in_statement->exclusions)
		{
			List	   *pair = lfirst_node(List, kc);
			const WsIndexKey *key = &ix->keys[k++];

			if (!same_index_key(s, key, linitial_node(IndexElem, pair)) ||
				strcmp(operator_text(lsecond_node(List, pair)), key->op) != 0)
				not_the_record(s, "another constraint");
		}
		k = 0;
		foreach(kc, in_statement->including)
			if (strcmp(strVal(lfirst(kc)), ws_schema_column(s, ix->include[k++])->name) != 0)
				not_the_record(s, "another constraint");
		if (ix->predicate != NULL)
			check_expression(s, ix->predicate, in_statement->where_clause);
	}
	else
	{
		int			k = 0;
		ListCell   *kc;

		if (in_statement->contype != CONSTR_UNIQUE || in_statement->raw_expr != NULL ||
			in_statement->is_no_inherit || in_statement->skip_validation ||
			in_statement->nulls_not_distinct != con->nulls_not_distinct ||
			list_length(in_statement->keys) != con->ncols)
			not_the_record(s, "another constraint");
		foreach(kc, in_statement->keys)
			if (strcmp(strVal(lfirst(kc)), ws_schema_column(s, con->cols[k++])->name) != 0)
				not_the_record(s, "another constraint");
	}
}

/* The statement's PARTITION BY is the record's, or there is none in either. */
static void
check_partitioning(const WsTableSchema *s, const PartitionSpec *spec)
{
	int			k = 0;
	ListCell   *lc;

	if (s->partition == WS_PARTITION_NONE)
	{
		if (spec != NULL)
			not_the_record(s, "a clause the record does not have");
		return;
	}
	if (spec == NULL || spec->strategy != (PartitionStrategy) s->partition ||
		list_length(spec->partParams) != s->npartition)
		not_the_record(s, "another partitioning");
	foreach(lc, spec->partParams)
	{
		PartitionElem *elem = lfirst_node(PartitionElem, lc);
		const WsIndexKey *key = &s->partition_keys[k++];

		if (!same_names(key->collation, elem->collation) ||
			!same_names(key->opclass, elem->opclass))
			not_the_record(s, "another partitioning");
		if (key->tag != 0)
		{
			if (elem->name == NULL || elem->expr != NULL ||
				strcmp(elem->name, ws_schema_column(s, key->tag)->name) != 0)
				not_the_record(s, "another partitioning");
		}
		else
		{
			if (elem->name != NULL || elem->expr == NULL)
				not_the_record(s, "another partitioning");
			check_expression(s, key->expr, elem->expr);
		}
	}
}

/*
 * The statement as written is the one the record says, by the grammar: one
 * CREATE TABLE, of this name, these columns, key, constraints and partitioning,
 * and nothing else.
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
	int			next = 0;
	int			nmade = 0;

	for (int c = 0; c < s->nconstraints; c++)
		nmade += s->constraints[c].not_valid ? 0 : 1;
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
	check_partitioning(s, create->partspec);
	if (create->inhRelations != NIL || create->partbound != NULL ||
		create->ofTypename != NULL || create->constraints != NIL || create->nnconstraints != NIL ||
		create->options != NIL || create->oncommit != ONCOMMIT_NOOP ||
		create->tablespacename != NULL || create->accessMethod != NULL || create->if_not_exists)
		not_the_record(s, "a clause the record does not have");
	if (list_length(create->tableElts) != s->ncols + (s->nkey > 0 ? 1 : 0) + nmade)
		not_the_record(s, "another number of columns or constraints");
	foreach(lc, create->tableElts)
	{
		Node	   *element = lfirst(lc);

		if (i < s->ncols)
		{
			if (!IsA(element, ColumnDef))
				not_the_record(s, "a clause where a column should be");
			check_column(s, &s->cols[i], (ColumnDef *) element, schema);
		}
		else if (i >= s->ncols + (s->nkey > 0 ? 1 : 0))
		{
			if (!IsA(element, Constraint))
				not_the_record(s, "a clause where a constraint should be");
			while (s->constraints[next].not_valid)
				next++;
			check_constraint(s, &s->constraints[next++], (Constraint *) element);
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

	/*
	 * CREATE TABLE keeps one index of those that parse alike, which two
	 * exclusions may though the record's text for them differs.
	 */
	for (int x = s->ncols + (s->nkey > 0 ? 1 : 0); x < list_length(create->tableElts); x++)
		for (int y = x + 1; y < list_length(create->tableElts); y++)
		{
			Constraint *a = list_nth_node(Constraint, create->tableElts, x);
			Constraint *b = list_nth_node(Constraint, create->tableElts, y);

			if (a->contype == CONSTR_EXCLUSION && b->contype == CONSTR_EXCLUSION &&
				strcmp(a->access_method, b->access_method) == 0 &&
				a->deferrable == b->deferrable && a->initdeferred == b->initdeferred &&
				equal(a->exclusions, b->exclusions) && equal(a->including, b->including) &&
				equal(a->where_clause, b->where_clause))
				not_the_record(s, "two exclusions alike");
		}
}

/* (strings of a schema, a new copy, each from UTF8 into the server's encoding) */
static char *
from_utf8(const char *text)
{
	return text ? pstrdup(pg_any_to_server(text, (int) strlen(text), PG_UTF8)) : NULL;
}

/* (a name may be longer here: the grammar would cut it short, and quietly) */
char *
ws_name_from_utf8(const char *name)
{
	char	   *here = from_utf8(name);

	if (here != NULL && strlen(here) >= NAMEDATALEN)
		ereport(ERROR,
				(errcode(ERRCODE_NAME_TOO_LONG),
				 errmsg("name \"%s\" is too long in this database's encoding", here)));
	return here;
}

static void
write_create_index(const WsTableSchema *s, const WsIndex *ix, const char *schema, Buf *out)
{
	ws_put_text(out, ix->unique ? "CREATE UNIQUE INDEX " : "CREATE INDEX ");
	ws_put_identifier(out, ix->name);
	ws_put_text(out, " ON ");
	ws_put_identifier(out, schema);
	ws_put_text(out, ".");
	ws_put_identifier(out, s->name);
	ws_put_text(out, " USING ");
	ws_put_identifier(out, ix->method);
	put_index_columns(s, ix, out);
	if (ix->nulls_not_distinct)
		ws_put_text(out, " NULLS NOT DISTINCT");
	put_index_predicate(s, ix, out);
}

/*
 * The statement as written is the one the record says, by the grammar: one
 * CREATE INDEX of this name, on this table, of just these keys, and no more.
 */
static void
check_index_statement(const WsTableSchema *s, const WsIndex *ix, const char *schema, const char *sql)
{
	List	   *parsed = parse_text(s, sql);
	IndexStmt  *stmt;
	int			k = 0;
	ListCell   *lc;

	if (list_length(parsed) != 1 || !IsA(linitial_node(RawStmt, parsed)->stmt, IndexStmt))
		not_the_record(s, "it is not one CREATE INDEX");
	stmt = (IndexStmt *) linitial_node(RawStmt, parsed)->stmt;
	if (stmt->idxname == NULL || strcmp(stmt->idxname, ix->name) != 0 ||
		stmt->relation->catalogname != NULL || stmt->relation->schemaname == NULL ||
		strcmp(stmt->relation->schemaname, schema) != 0 ||
		strcmp(stmt->relation->relname, s->name) != 0 || !stmt->relation->inh ||
		strcmp(stmt->accessMethod, ix->method) != 0 ||
		stmt->tableSpace != NULL || stmt->options != NIL || stmt->excludeOpNames != NIL ||
		stmt->idxcomment != NULL || stmt->unique != ix->unique ||
		stmt->nulls_not_distinct != ix->nulls_not_distinct || stmt->primary ||
		stmt->isconstraint || stmt->iswithoutoverlaps || stmt->deferrable ||
		stmt->initdeferred || stmt->concurrent || stmt->if_not_exists ||
		list_length(stmt->indexParams) != ix->nkeys ||
		list_length(stmt->indexIncludingParams) != ix->ninclude ||
		(stmt->whereClause != NULL) != (ix->predicate != NULL))
		not_the_record(s, "another index");
	foreach(lc, stmt->indexParams)
	{
		if (!same_index_key(s, &ix->keys[k++], lfirst_node(IndexElem, lc)))
			not_the_record(s, "another index");
	}
	k = 0;
	foreach(lc, stmt->indexIncludingParams)
	{
		IndexElem  *elem = lfirst_node(IndexElem, lc);

		if (elem->name == NULL || elem->expr != NULL || elem->indexcolname != NULL ||
			elem->collation != NIL || elem->opclass != NIL || elem->opclassopts != NIL ||
			elem->ordering != SORTBY_DEFAULT || elem->nulls_ordering != SORTBY_NULLS_DEFAULT ||
			strcmp(elem->name, ws_schema_column(s, ix->include[k++])->name) != 0)
			not_the_record(s, "another index");
	}
	if (ix->predicate != NULL)
		check_expression(s, ix->predicate, stmt->whereClause);
}

static void
write_attach(const WsTableSchema *s, const char *schema, Buf *out)
{
	ws_put_text(out, "ALTER TABLE ");
	ws_put_identifier(out, schema);
	ws_put_text(out, ".");
	ws_put_identifier(out, s->partition_of);
	ws_put_text(out, " ATTACH PARTITION ");
	ws_put_identifier(out, schema);
	ws_put_text(out, ".");
	ws_put_identifier(out, s->name);
	ws_put_text(out, " ");
	ws_put_text(out, s->partition_bound);
}

/*
 * Whether a parsed bound holds nothing but constants, and so runs nothing when it
 * is attached: the server prints no other, and a commit may have come from anywhere.
 */
static bool
bound_is_constants(const PartitionBoundSpec *bound)
{
	ListCell   *lc;

	foreach(lc, bound->listdatums)
		if (!IsA(lfirst(lc), A_Const))
			return false;
	for (int side = 0; side < 2; side++)
		foreach(lc, side == 0 ? bound->lowerdatums : bound->upperdatums)
		{
			Node	   *datum = lfirst(lc);

			if (IsA(datum, ColumnRef))
			{
				List	   *fields = ((ColumnRef *) datum)->fields;

				if (list_length(fields) != 1 || !IsA(linitial(fields), String) ||
					(strcmp(strVal(linitial(fields)), "minvalue") != 0 &&
					 strcmp(strVal(linitial(fields)), "maxvalue") != 0))
					return false;
			}
			else if (!IsA(datum, A_Const))
				return false;
		}
	return true;
}

/* One ALTER TABLE, of this table, attaching this partition by this bound, parsed alone. */
static void
check_attach_statement(const WsTableSchema *s, const char *schema, const char *sql)
{
	List	   *parsed = parse_text(s, sql);
	List	   *alone = parse_text(s, psprintf("CREATE TABLE t PARTITION OF p %s", s->partition_bound));
	AlterTableStmt *stmt;
	AlterTableCmd *cmd;
	PartitionCmd *part;
	CreateStmt *create;

	if (list_length(parsed) != 1 || !IsA(linitial_node(RawStmt, parsed)->stmt, AlterTableStmt))
		not_the_record(s, "it is not one ALTER TABLE");
	stmt = (AlterTableStmt *) linitial_node(RawStmt, parsed)->stmt;
	if (stmt->relation->catalogname != NULL || stmt->relation->schemaname == NULL ||
		strcmp(stmt->relation->schemaname, schema) != 0 ||
		strcmp(stmt->relation->relname, s->partition_of) != 0 || !stmt->relation->inh ||
		stmt->objtype != OBJECT_TABLE || stmt->missing_ok || list_length(stmt->cmds) != 1)
		not_the_record(s, "another table");
	cmd = linitial_node(AlterTableCmd, stmt->cmds);
	if (cmd->subtype != AT_AttachPartition || !IsA(cmd->def, PartitionCmd))
		not_the_record(s, "another change");
	part = (PartitionCmd *) cmd->def;
	if (part->concurrent || part->name->catalogname != NULL || part->name->schemaname == NULL ||
		strcmp(part->name->schemaname, schema) != 0 || strcmp(part->name->relname, s->name) != 0)
		not_the_record(s, "another partition");
	if (list_length(alone) != 1 || !IsA(linitial_node(RawStmt, alone)->stmt, CreateStmt))
		not_the_record(s, "a bound that is no bound");
	create = (CreateStmt *) linitial_node(RawStmt, alone)->stmt;
	if (create->partbound == NULL || create->partspec != NULL || create->tableElts != NIL ||
		create->options != NIL || create->tablespacename != NULL || create->accessMethod != NULL ||
		create->oncommit != ONCOMMIT_NOOP || !equal(create->partbound, part->bound))
		not_the_record(s, "a bound that is no bound");
	if (!bound_is_constants(create->partbound))
		not_the_record(s, "a bound of more than constants");
}

/* One ALTER TABLE, of this table, inheriting from this parent, and nothing else. */
static void
check_inherit_statement(const WsTableSchema *s, const char *parent, const char *schema, const char *sql)
{
	List	   *parsed = parse_text(s, sql);
	AlterTableStmt *stmt;
	AlterTableCmd *cmd;
	RangeVar   *of;

	if (list_length(parsed) != 1 || !IsA(linitial_node(RawStmt, parsed)->stmt, AlterTableStmt))
		not_the_record(s, "it is not one ALTER TABLE");
	stmt = (AlterTableStmt *) linitial_node(RawStmt, parsed)->stmt;
	if (stmt->relation->catalogname != NULL || stmt->relation->schemaname == NULL ||
		strcmp(stmt->relation->schemaname, schema) != 0 ||
		strcmp(stmt->relation->relname, s->name) != 0 || !stmt->relation->inh ||
		stmt->objtype != OBJECT_TABLE || stmt->missing_ok || list_length(stmt->cmds) != 1)
		not_the_record(s, "another table");
	cmd = linitial_node(AlterTableCmd, stmt->cmds);
	if (cmd->subtype != AT_AddInherit || cmd->name != NULL || cmd->newowner != NULL ||
		cmd->missing_ok || !IsA(cmd->def, RangeVar))
		not_the_record(s, "another change");
	of = (RangeVar *) cmd->def;
	if (of->catalogname != NULL || of->schemaname == NULL || strcmp(of->schemaname, schema) != 0 ||
		strcmp(of->relname, parent) != 0)
		not_the_record(s, "another parent");
}

static void
index_in_server_encoding(const WsIndex *ix, WsIndex *here)
{
	*here = *ix;
	here->name = ws_name_from_utf8(ix->name);
	here->method = ws_name_from_utf8(ix->method);
	here->predicate = from_utf8(ix->predicate);
	here->keys = palloc_array(WsIndexKey, Max(ix->nkeys, 1));
	for (int k = 0; k < ix->nkeys; k++)
	{
		here->keys[k] = ix->keys[k];
		here->keys[k].expr = from_utf8(ix->keys[k].expr);
		here->keys[k].collation = from_utf8(ix->keys[k].collation);
		here->keys[k].opclass = from_utf8(ix->keys[k].opclass);
		here->keys[k].op = from_utf8(ix->keys[k].op);
	}
}

static void
write_add_check(const WsTableSchema *s, const WsConstraint *con, const char *schema, Buf *out)
{
	ws_put_text(out, "ALTER TABLE ");
	ws_put_identifier(out, schema);
	ws_put_text(out, ".");
	ws_put_identifier(out, s->name);
	ws_put_text(out, " ADD CONSTRAINT ");
	ws_put_identifier(out, con->name);
	put_check(s, con, out);
	ws_put_text(out, " NOT VALID");
}

/* One ALTER TABLE, of this table, adding this check, and nothing else. */
static void
check_add_check_statement(const WsTableSchema *s, const WsConstraint *con, const char *schema,
						  const char *sql)
{
	List	   *parsed = parse_text(s, sql);
	AlterTableStmt *stmt;
	AlterTableCmd *cmd;

	if (list_length(parsed) != 1 || !IsA(linitial_node(RawStmt, parsed)->stmt, AlterTableStmt))
		not_the_record(s, "it is not one ALTER TABLE");
	stmt = (AlterTableStmt *) linitial_node(RawStmt, parsed)->stmt;
	if (stmt->relation->catalogname != NULL || stmt->relation->schemaname == NULL ||
		strcmp(stmt->relation->schemaname, schema) != 0 ||
		strcmp(stmt->relation->relname, s->name) != 0 || !stmt->relation->inh ||
		stmt->objtype != OBJECT_TABLE || stmt->missing_ok || list_length(stmt->cmds) != 1)
		not_the_record(s, "another table");
	cmd = linitial_node(AlterTableCmd, stmt->cmds);
	if (cmd->subtype != AT_AddConstraint || cmd->name != NULL || cmd->newowner != NULL ||
		cmd->missing_ok || !IsA(cmd->def, Constraint))
		not_the_record(s, "another change");
	check_constraint(s, con, (Constraint *) cmd->def);
}

/* A copy of a schema with its strings in the server's encoding. */
static void
schema_in_server_encoding(const WsTableSchema *s, WsTableSchema *here_out)
{
	WsTableSchema here = *s;

	here.name = ws_name_from_utf8(s->name);
	here.key_name = ws_name_from_utf8(s->key_name);
	here.cols = palloc_array(WsColumn, Max(s->ncols, 1));
	for (int i = 0; i < s->ncols; i++)
	{
		here.cols[i] = s->cols[i];
		here.cols[i].name = ws_name_from_utf8(s->cols[i].name);
		here.cols[i].type = from_utf8(s->cols[i].type);
		here.cols[i].def = from_utf8(s->cols[i].def);
		here.cols[i].collation = from_utf8(s->cols[i].collation);
		here.cols[i].not_null = ws_name_from_utf8(s->cols[i].not_null);
		here.cols[i].identity_seq = ws_name_from_utf8(s->cols[i].identity_seq);
	}
	here.constraints = palloc_array(WsConstraint, Max(s->nconstraints, 1));
	for (int i = 0; i < s->nconstraints; i++)
	{
		here.constraints[i] = s->constraints[i];
		here.constraints[i].name = ws_name_from_utf8(s->constraints[i].name);
		here.constraints[i].expr = from_utf8(s->constraints[i].expr);
		if (s->constraints[i].kind == WS_CONSTRAINT_EXCLUSION)
			index_in_server_encoding(&s->constraints[i].excl, &here.constraints[i].excl);
	}
	here.indexes = palloc_array(WsIndex, Max(s->nindexes, 1));
	for (int i = 0; i < s->nindexes; i++)
		index_in_server_encoding(&s->indexes[i], &here.indexes[i]);
	here.partition_keys = palloc_array(WsIndexKey, Max(s->npartition, 1));
	for (int k = 0; k < s->npartition; k++)
	{
		here.partition_keys[k] = s->partition_keys[k];
		here.partition_keys[k].expr = from_utf8(s->partition_keys[k].expr);
		here.partition_keys[k].collation = from_utf8(s->partition_keys[k].collation);
		here.partition_keys[k].opclass = from_utf8(s->partition_keys[k].opclass);
	}
	here.partition_of = ws_name_from_utf8(s->partition_of);
	here.partition_bound = from_utf8(s->partition_bound);
	here.inherits = palloc_array(char *, Max(s->ninherits, 1));
	for (int i = 0; i < s->ninherits; i++)
		here.inherits[i] = ws_name_from_utf8(s->inherits[i]);
	here.inherited_cons = palloc_array(char *, Max(s->ninherited_cons, 1));
	for (int i = 0; i < s->ninherited_cons; i++)
		here.inherited_cons[i] = ws_name_from_utf8(s->inherited_cons[i]);
	*here_out = here;
}

void
ws_schema_create_table(const WsTableSchema *s, const char *schema, Buf *out)
{
	const char *problem = ws_schema_problem(s);
	WsTableSchema here;
	Buf			sql = BUF_INIT;

	if (problem != NULL)
		elog(ERROR, "%s", problem);
	schema_in_server_encoding(s, &here);
	write_create_table(&here, schema, &sql);
	buf_append_byte(&sql, 0);
	check_statement(&here, schema, (char *) sql.data);
	buf_append(out, sql.data, sql.len - 1);
	buf_free(&sql);
}

void
ws_schema_create_index(const WsTableSchema *s, int n, const char *schema, Buf *out)
{
	const char *problem = ws_schema_problem(s);
	WsTableSchema here;
	Buf			sql = BUF_INIT;

	if (problem != NULL)
		elog(ERROR, "%s", problem);
	if (n < 0 || n >= s->nindexes)
		elog(ERROR, "no index %d", n);
	schema_in_server_encoding(s, &here);
	write_create_index(&here, &here.indexes[n], schema, &sql);
	buf_append_byte(&sql, 0);
	check_index_statement(&here, &here.indexes[n], schema, (char *) sql.data);
	buf_append(out, sql.data, sql.len - 1);
	buf_free(&sql);
}

void
ws_schema_add_check(const WsTableSchema *s, int n, const char *schema, Buf *out)
{
	const char *problem = ws_schema_problem(s);
	WsTableSchema here;
	Buf			sql = BUF_INIT;

	if (problem != NULL)
		elog(ERROR, "%s", problem);
	if (n < 0 || n >= s->nconstraints || !s->constraints[n].not_valid)
		elog(ERROR, "no check not valid at %d", n);
	schema_in_server_encoding(s, &here);
	write_add_check(&here, &here.constraints[n], schema, &sql);
	buf_append_byte(&sql, 0);
	check_add_check_statement(&here, &here.constraints[n], schema, (char *) sql.data);
	buf_append(out, sql.data, sql.len - 1);
	buf_free(&sql);
}

void
ws_schema_attach(const WsTableSchema *s, const char *schema, Buf *out)
{
	const char *problem = ws_schema_problem(s);
	WsTableSchema here;
	Buf			sql = BUF_INIT;

	if (problem != NULL)
		elog(ERROR, "%s", problem);
	if (s->partition_of == NULL)
		elog(ERROR, "table \"%s\" is no partition", s->name);
	schema_in_server_encoding(s, &here);
	write_attach(&here, schema, &sql);
	buf_append_byte(&sql, 0);
	check_attach_statement(&here, schema, (char *) sql.data);
	buf_append(out, sql.data, sql.len - 1);
	buf_free(&sql);
}

void
ws_schema_inherit(const WsTableSchema *s, int n, const char *schema, Buf *out)
{
	const char *problem = ws_schema_problem(s);
	WsTableSchema here;
	Buf			sql = BUF_INIT;

	if (problem != NULL)
		elog(ERROR, "%s", problem);
	if (n < 0 || n >= s->ninherits)
		elog(ERROR, "no parent %d", n);
	schema_in_server_encoding(s, &here);
	ws_put_text(&sql, "ALTER TABLE ");
	ws_put_identifier(&sql, schema);
	ws_put_text(&sql, ".");
	ws_put_identifier(&sql, here.name);
	ws_put_text(&sql, " INHERIT ");
	ws_put_identifier(&sql, schema);
	ws_put_text(&sql, ".");
	ws_put_identifier(&sql, here.inherits[n]);
	buf_append_byte(&sql, 0);
	check_inherit_statement(&here, here.inherits[n], schema, (char *) sql.data);
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
char *
ws_name_to_utf8(const char *name)
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
 * of it: a refusal, not a smaller table.
 */
static const char *const unsupported_sql =
"WITH own(o) AS (SELECT $1::oid "
"  UNION ALL SELECT x.indexrelid FROM pg_index x WHERE x.indrelid = $1::oid "
"  UNION ALL SELECT p.objid FROM pg_depend p WHERE p.classid = 'pg_class'::regclass "
"    AND p.refclassid = 'pg_class'::regclass AND p.refobjid = $1::oid AND p.deptype = 'i' "
"    AND p.refobjsubid > 0) "
"SELECT CASE "
"  WHEN EXISTS (SELECT FROM pg_inherits i JOIN pg_class q ON q.oid = i.inhparent "
"               WHERE c.relispartition AND i.inhrelid = c.oid AND "
"                 (q.relnamespace <> c.relnamespace OR i.inhdetachpending)) "
"    THEN 'it is a partition being detached, or of a table in another schema' "
"  WHEN EXISTS (SELECT FROM pg_inherits i JOIN pg_attribute pa ON pa.attrelid = i.inhparent "
"               JOIN pg_attribute ca ON ca.attrelid = c.oid AND ca.attname = pa.attname "
"               WHERE c.relispartition AND i.inhrelid = c.oid AND pa.attnum > 0 "
"                 AND NOT pa.attisdropped AND NOT ca.attisdropped AND pa.attidentity <> ca.attidentity) "
"    THEN 'its identity is not that of its partitioned table' "
"  WHEN c.relispartition AND EXISTS (SELECT FROM pg_index x JOIN pg_class xc ON xc.oid = x.indexrelid "
"               JOIN pg_index y ON y.indrelid = x.indrelid AND y.indexrelid > x.indexrelid "
"               JOIN pg_class yc ON yc.oid = y.indexrelid "
"               LEFT JOIN pg_inherits xp ON xp.inhrelid = x.indexrelid "
"               LEFT JOIN pg_inherits yp ON yp.inhrelid = y.indexrelid "
"               WHERE x.indrelid = c.oid AND xc.relam = yc.relam AND x.indisunique = y.indisunique "
"                 AND (xp.inhparent IS NOT NULL OR yp.inhparent IS NOT NULL) "
"                 AND NOT (yp.inhparent IS NULL AND "
"                   EXISTS (SELECT FROM pg_constraint k WHERE k.conindid = xp.inhparent AND k.contype IN ('p', 'u', 'x')) AND "
"                   NOT EXISTS (SELECT FROM pg_constraint k WHERE k.conindid = y.indexrelid AND k.contype IN ('p', 'u', 'x'))) "
"                 AND NOT (xp.inhparent IS NULL AND "
"                   EXISTS (SELECT FROM pg_constraint k WHERE k.conindid = yp.inhparent AND k.contype IN ('p', 'u', 'x')) AND "
"                   NOT EXISTS (SELECT FROM pg_constraint k WHERE k.conindid = x.indexrelid AND k.contype IN ('p', 'u', 'x'))) "
"                 AND x.indnullsnotdistinct = y.indnullsnotdistinct AND x.indnatts = y.indnatts "
"                 AND x.indnkeyatts = y.indnkeyatts AND x.indkey::text = y.indkey::text "
"                 AND x.indcollation::text = y.indcollation::text "
"                 AND (SELECT k.conexclop FROM pg_constraint k WHERE k.conindid = x.indexrelid AND k.contype = 'x') "
"                   IS NOT DISTINCT FROM "
"                     (SELECT k.conexclop FROM pg_constraint k WHERE k.conindid = y.indexrelid AND k.contype = 'x') "
"                 AND pg_get_expr(x.indexprs, x.indrelid) IS NOT DISTINCT FROM pg_get_expr(y.indexprs, y.indrelid) "
"                 AND pg_get_expr(x.indpred, x.indrelid) IS NOT DISTINCT FROM pg_get_expr(y.indpred, y.indrelid) "
"                 AND (SELECT array_agg(o.opcfamily ORDER BY n.ord) FROM unnest(x.indclass::oid[]) "
"                      WITH ORDINALITY n(oc, ord) JOIN pg_opclass o ON o.oid = n.oc) = "
"                     (SELECT array_agg(o.opcfamily ORDER BY n.ord) FROM unnest(y.indclass::oid[]) "
"                      WITH ORDINALITY n(oc, ord) JOIN pg_opclass o ON o.oid = n.oc)) "
"    THEN 'it has two indexes attaching could take one for the other' "
"  WHEN EXISTS (SELECT FROM pg_inherits i JOIN pg_class q ON q.oid = i.inhrelid "
"               WHERE c.relkind = 'p' AND i.inhparent = c.oid AND "
"                 (q.relnamespace <> c.relnamespace OR q.relkind NOT IN ('r', 'p') OR i.inhdetachpending)) "
"    THEN 'it has a partition being detached, in another schema, or not a table' "
"  WHEN c.reloftype <> 0 THEN 'it is a typed table' "
"  WHEN EXISTS (SELECT FROM pg_inherits i JOIN pg_class q ON q.oid = i.inhparent "
"               WHERE NOT c.relispartition AND i.inhrelid = c.oid AND "
"                 (q.relnamespace <> c.relnamespace OR q.relkind <> 'r')) "
"    THEN 'it inherits from a table in another schema, or from no table' "
"  WHEN (SELECT count(*) FROM pg_inherits i WHERE NOT c.relispartition AND i.inhrelid = c.oid) > " CppAsString2(WS_MAX_PARENTS) " "
"    THEN 'it inherits from more tables than a commit holds' "
"  WHEN EXISTS (SELECT FROM pg_inherits i JOIN pg_class q ON q.oid = i.inhrelid "
"               WHERE c.relkind <> 'p' AND i.inhparent = c.oid AND q.relpersistence <> 't' AND "
"                 (q.relnamespace <> c.relnamespace OR q.relkind <> 'r')) "
"    THEN 'a table in another schema, or no table, inherits from it' "
"  WHEN am.amname <> 'heap' OR (c.relkind = 'p' AND c.relam <> 0) "
"    THEN 'it has another access method' "
"  WHEN c.reltablespace <> 0 THEN 'it has a tablespace' "
"  WHEN c.reloptions IS NOT NULL OR tc.reloptions IS NOT NULL "
"    THEN 'it has storage parameters' "
"  WHEN c.relreplident <> 'd' THEN 'it has a replica identity' "
"  WHEN c.relrowsecurity OR c.relforcerowsecurity OR "
"       EXISTS (SELECT FROM pg_policy o WHERE o.polrelid = c.oid) THEN 'it has row security' "
"  WHEN EXISTS (SELECT FROM pg_constraint k WHERE k.conrelid = c.oid AND "
"               k.contype NOT IN ('p', 'u', 'c', 'n', 'f', 'x')) "
"    THEN 'it has a constraint trigger' "
"  WHEN EXISTS (SELECT FROM pg_constraint k JOIN pg_index x ON x.indexrelid = k.conindid "
"               JOIN pg_class ic ON ic.oid = x.indexrelid "
"               WHERE k.conrelid = c.oid AND k.contype IN ('p', 'u') AND "
"                 (NOT x.indisvalid OR (k.contype = 'p' AND k.condeferrable) OR k.conperiod OR x.indisclustered OR "
"                  x.indnkeyatts <> x.indnatts OR "
"                  ic.reloptions IS NOT NULL OR ic.reltablespace <> 0)) "
"    THEN 'a key or unique constraint has more than columns, or is not valid' "
"  WHEN EXISTS (SELECT FROM pg_constraint k JOIN pg_index x ON x.indexrelid = k.conindid "
"               JOIN pg_constraint k2 ON k2.conrelid = k.conrelid AND k2.oid > k.oid "
"               JOIN pg_index x2 ON x2.indexrelid = k2.conindid "
"               WHERE k.conrelid = c.oid AND k.contype IN ('p', 'u') AND k2.contype IN ('p', 'u') "
"                 AND x.indkey::text = x2.indkey::text "
"                 AND x.indnullsnotdistinct = x2.indnullsnotdistinct "
"                 AND k.condeferrable = k2.condeferrable AND k.condeferred = k2.condeferred) "
"    THEN 'it has two constraints alike, of which CREATE TABLE makes one' "
"  WHEN EXISTS (SELECT FROM pg_index x JOIN pg_class ic ON ic.oid = x.indexrelid "
"               WHERE x.indrelid = c.oid AND NOT EXISTS "
"               (SELECT FROM pg_constraint k WHERE k.conindid = x.indexrelid AND k.conrelid = c.oid "
"                  AND k.contype NOT IN ('x', 'f')) AND "
"                 (NOT x.indisvalid OR NOT x.indisready OR x.indisclustered OR "
"                  ic.reloptions IS NOT NULL OR ic.reltablespace <> 0 OR "
"                  EXISTS (SELECT FROM pg_attribute a WHERE a.attrelid = x.indexrelid AND "
"                          (a.attstattarget IS NOT NULL OR a.attoptions IS NOT NULL)))) "
"    THEN 'an index has options, or is not valid' "
"  WHEN EXISTS (SELECT FROM pg_depend p WHERE p.classid = 'pg_class'::regclass "
"               AND p.objid IN (SELECT o FROM own) AND p.deptype = 'x') "
"    THEN 'it depends on an extension' "
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
"  WHEN NOT c.relispartition AND EXISTS (SELECT FROM pg_attribute a WHERE a.attrelid = c.oid AND a.attidentity <> '' "
"               AND NOT a.attisdropped AND NOT EXISTS (SELECT FROM pg_depend p "
"                 JOIN pg_class q ON q.oid = p.objid WHERE p.classid = 'pg_class'::regclass "
"                 AND p.refclassid = 'pg_class'::regclass AND p.refobjid = c.oid "
"                 AND p.refobjsubid = a.attnum AND p.deptype = 'i' AND q.relkind = 'S')) "
"    THEN 'an identity has its sequence in another schema' "
"  WHEN EXISTS (SELECT FROM pg_depend p JOIN pg_class q ON q.oid = p.objid "
"               JOIN pg_attribute a ON a.attrelid = p.refobjid AND a.attnum = p.refobjsubid "
"               WHERE p.classid = 'pg_class'::regclass AND p.refclassid = 'pg_class'::regclass "
"                 AND p.refobjid = c.oid AND p.deptype = 'i' AND q.relkind = 'S' "
"                 AND a.attidentity = '') "
"    THEN 'a column owns the identity sequence of another table' "
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
						  ws_sql_value(cols, i, 1)) == 0 &&
				((Form_pg_attribute) GETSTRUCT(tup))->atttypid == atooid(ws_sql_value(cols, i, 11));
			ReleaseSysCache(tup);
		}
		if (!same)
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("a column of table \"%s\" was renamed, retyped or dropped since this transaction's snapshot",
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

static uint64
index_col_tag(const char *table, ReadCols *rc, const char *name)
{
	uint64		tag;

	if (!read_tag_of(rc, name, &tag))
		elog(ERROR, "an index of table \"%s\" on column \"%s\", which it does not have", table, name);
	return tag;
}

static char *
stored_expression(const char *expression, ReadCols *rc)
{
	Buf			stored = BUF_INIT;
	char	   *utf8;

	ws_tags_template(expression, read_tag_of, rc, &stored);
	buf_append_byte(&stored, 0);
	utf8 = to_utf8((char *) stored.data);
	buf_free(&stored);
	return utf8;
}

/* (printed from the caches, not the snapshot its row was read under) */
pg_noreturn static void
index_gone(const char *table)
{
	ereport(ERROR,
			(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
			 errmsg("an index of table \"%s\" was dropped since this transaction's snapshot", table)));
}

/* (and so, what the snapshot's rows passed as having no options) */
pg_noreturn static void
index_changed(const char *table)
{
	ereport(ERROR,
			(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
			 errmsg("an index of table \"%s\" was given options since this transaction's snapshot", table)));
}

/* Each part of a name a commit holds as text, within a commit's limit for a name. */
static void
names_fit(List *names)
{
	ListCell   *lc;

	foreach(lc, names)
		px_free(ws_name_to_utf8(strVal(lfirst(lc))));
}

/* A key as printed; its expression, if it is one, as printed alone. */
static void
read_index_key(const char *table, const char *index_oid, int k, const IndexElem *elem,
			   ReadCols *rc, WsIndexKey *key)
{
	if (elem->opclassopts != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("the schema of table \"%s\" cannot be read yet: an index has options",
						table)));
	if (elem->name != NULL)
		key->tag = index_col_tag(table, rc, elem->name);
	else
	{
		char		colno[16];
		const char *params[2] = {index_oid, colno};
		WsSqlRows	one;

		snprintf(colno, sizeof(colno), "%d", k + 1);
		WsSql->run("SELECT pg_get_indexdef($1::oid, $2::int, false)", 2, params, &one);
		if (ws_sql_value(&one, 0, 0) == NULL)
			index_gone(table);
		key->expr = stored_expression(ws_sql_value(&one, 0, 0), rc);
		ws_sql_rows_free(&one);
	}
	names_fit(elem->collation);
	names_fit(elem->opclass);
	key->collation = elem->collation ? to_utf8(NameListToQuotedString(elem->collation)) : NULL;
	key->opclass = elem->opclass ? to_utf8(NameListToQuotedString(elem->opclass)) : NULL;
	key->order = elem->ordering == SORTBY_DESC ? WS_INDEX_ORDER_DESC : WS_INDEX_ORDER_DEFAULT;
	key->nulls = elem->nulls_ordering == SORTBY_NULLS_FIRST ? WS_INDEX_NULLS_FIRST :
		elem->nulls_ordering == SORTBY_NULLS_LAST ? WS_INDEX_NULLS_LAST : WS_INDEX_NULLS_DEFAULT;
}

/*
 * An exclusion, from the clause the server prints for it, as an index is below.
 * The constraint's row is the snapshot's, and its index's text the caches', which
 * the server cannot print with that index gone; under the lock, it stays.
 */
static void
read_exclusion(const char *table, const char *con_oid, const char *index_oid, const char *predicate,
			   ReadCols *rc, WsIndex *ix)
{
	const char *params[1] = {con_oid};
	WsSqlRows	printed;
	List	   *parsed;
	List	   *elts;
	Constraint *con;
	int			k = 0;
	ListCell   *lc;

	if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(atooid(index_oid))))
		index_gone(table);
	WsSql->run("SELECT pg_get_constraintdef($1::oid)", 1, params, &printed);
	if (ws_sql_value(&printed, 0, 0) == NULL)
		index_gone(table);
	parsed = raw_parser(psprintf("CREATE TABLE t (CONSTRAINT c %s)", ws_sql_value(&printed, 0, 0)),
						RAW_PARSE_DEFAULT);
	ws_sql_rows_free(&printed);
	if (list_length(parsed) != 1 || !IsA(linitial_node(RawStmt, parsed)->stmt, CreateStmt))
		elog(ERROR, "an exclusion of table \"%s\" printed as something else", table);
	elts = ((CreateStmt *) linitial_node(RawStmt, parsed)->stmt)->tableElts;
	if (list_length(elts) != 1 || !IsA(linitial(elts), Constraint))
		elog(ERROR, "an exclusion of table \"%s\" printed as something else", table);
	con = linitial_node(Constraint, elts);
	if (con->contype != CONSTR_EXCLUSION)
		elog(ERROR, "an exclusion of table \"%s\" printed as something else", table);
	if (con->options != NIL || con->indexspace != NULL)
		index_changed(table);
	ix->method = ws_name_to_utf8(con->access_method);
	ix->nkeys = list_length(con->exclusions);
	ix->keys = px_malloc((size_t) Max(ix->nkeys, 1) * sizeof(WsIndexKey));
	memset(ix->keys, 0, (size_t) Max(ix->nkeys, 1) * sizeof(WsIndexKey));
	foreach(lc, con->exclusions)
	{
		List	   *pair = lfirst_node(List, lc);

		read_index_key(table, index_oid, k, linitial_node(IndexElem, pair), rc, &ix->keys[k]);
		names_fit(lsecond_node(List, pair));
		ix->keys[k].op = to_utf8(operator_text(lsecond_node(List, pair)));
		k++;
	}
	ix->ninclude = list_length(con->including);
	ix->include = px_malloc((size_t) Max(ix->ninclude, 1) * sizeof(uint64));
	k = 0;
	foreach(lc, con->including)
		ix->include[k++] = index_col_tag(table, rc, strVal(lfirst(lc)));
	ix->predicate = predicate ? stored_expression(predicate, rc) : NULL;
}

/*
 * An index, from the statement the server prints for it: which collations,
 * opclasses and orders are other than the default is its decision, so the
 * grammar reads them from what it decided, and each expression comes from the
 * form of it printed alone.
 */
static void
read_index(const char *table, const WsSqlRows *idxs, int row, ReadCols *rc, WsIndex *ix)
{
	const char *def = ws_sql_value(idxs, row, 2);
	const char *predicate = ws_sql_value(idxs, row, 3);
	List	   *parsed;
	IndexStmt  *stmt;
	int			k = 0;
	ListCell   *lc;

	if (def == NULL)
		index_gone(table);
	parsed = raw_parser(def, RAW_PARSE_DEFAULT);
	if (list_length(parsed) != 1 || !IsA(linitial_node(RawStmt, parsed)->stmt, IndexStmt))
		elog(ERROR, "an index of table \"%s\" printed as something else", table);
	stmt = (IndexStmt *) linitial_node(RawStmt, parsed)->stmt;
	if (stmt->options != NIL || stmt->tableSpace != NULL)
		index_changed(table);
	ix->name = ws_name_to_utf8(ws_sql_value(idxs, row, 0));
	ix->method = ws_name_to_utf8(stmt->accessMethod);
	ix->unique = stmt->unique;
	ix->nulls_not_distinct = stmt->nulls_not_distinct;
	ix->nkeys = list_length(stmt->indexParams);
	ix->keys = px_malloc((size_t) Max(ix->nkeys, 1) * sizeof(WsIndexKey));
	memset(ix->keys, 0, (size_t) Max(ix->nkeys, 1) * sizeof(WsIndexKey));
	foreach(lc, stmt->indexParams)
	{
		read_index_key(table, ws_sql_value(idxs, row, 1), k, lfirst_node(IndexElem, lc), rc, &ix->keys[k]);
		k++;
	}
	ix->ninclude = list_length(stmt->indexIncludingParams);
	ix->include = px_malloc((size_t) Max(ix->ninclude, 1) * sizeof(uint64));
	k = 0;
	foreach(lc, stmt->indexIncludingParams)
		ix->include[k++] = index_col_tag(table, rc, lfirst_node(IndexElem, lc)->name);
	ix->predicate = predicate ? stored_expression(predicate, rc) : NULL;
}

/*
 * How a partitioned table's rows divide, from the clause the server prints for
 * it, as an index is above; but its expressions are one list in the catalog, with
 * no way to print the nth but this.
 */
static void
read_partitioning(const char *table, const char *oid, ReadCols *rc, WsTableSchema *out)
{
	const char *params[1] = {oid};
	WsSqlRows	printed;
	List	   *parsed;
	PartitionSpec *spec;
	HeapTuple	tup = SearchSysCache1(PARTRELID, ObjectIdGetDatum(atooid(oid)));
	List	   *exprs = NIL;
	List	   *context = deparse_context_for(table, atooid(oid));
	int			nexpr = 0;
	int			k = 0;
	ListCell   *lc;

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "partitioned table \"%s\" has no partitioning", table);
	{
		bool		isnull;
		Datum		datum = SysCacheGetAttr(PARTRELID, tup, Anum_pg_partitioned_table_partexprs, &isnull);

		if (!isnull)
			exprs = (List *) stringToNode(TextDatumGetCString(datum));
	}
	ReleaseSysCache(tup);
	WsSql->run("SELECT pg_get_partkeydef($1::oid)", 1, params, &printed);
	if (ws_sql_value(&printed, 0, 0) == NULL)
		elog(ERROR, "partitioned table \"%s\" printed no partitioning", table);
	parsed = raw_parser(psprintf("CREATE TABLE t () PARTITION BY %s", ws_sql_value(&printed, 0, 0)),
						RAW_PARSE_DEFAULT);
	ws_sql_rows_free(&printed);
	if (list_length(parsed) != 1 || !IsA(linitial_node(RawStmt, parsed)->stmt, CreateStmt))
		elog(ERROR, "partitioned table \"%s\" printed as something else", table);
	spec = ((CreateStmt *) linitial_node(RawStmt, parsed)->stmt)->partspec;
	if (spec == NULL)
		elog(ERROR, "partitioned table \"%s\" printed as something else", table);
	out->partition = (char) spec->strategy;
	out->npartition = list_length(spec->partParams);
	out->partition_keys = px_malloc((size_t) Max(out->npartition, 1) * sizeof(WsIndexKey));
	memset(out->partition_keys, 0, (size_t) Max(out->npartition, 1) * sizeof(WsIndexKey));
	foreach(lc, spec->partParams)
	{
		PartitionElem *elem = lfirst_node(PartitionElem, lc);
		WsIndexKey *key = &out->partition_keys[k++];

		if (elem->name != NULL)
			key->tag = index_col_tag(table, rc, elem->name);
		else
			key->expr = stored_expression(deparse_expression(list_nth(exprs, nexpr++), context, false, false), rc);
		names_fit(elem->collation);
		names_fit(elem->opclass);
		key->collation = elem->collation ? to_utf8(NameListToQuotedString(elem->collation)) : NULL;
		key->opclass = elem->opclass ? to_utf8(NameListToQuotedString(elem->opclass)) : NULL;
		key->order = WS_INDEX_ORDER_DEFAULT;
		key->nulls = WS_INDEX_NULLS_DEFAULT;
	}
}

static int
name_cmp(const void *a, const void *b)
{
	return strcmp(*(char *const *) a, *(char *const *) b);
}

/* The tables a table inherits from, and what it has only by doing so. */
static void
read_inheritance(const char *oid, const WsSqlRows *cols, const WsSqlRows *others, WsTableSchema *out)
{
	const char *params[1] = {oid};
	WsSqlRows	parents;
	int			n = 0;

	WsSql->run("SELECT q.relname::text FROM pg_inherits i JOIN pg_class q ON q.oid = i.inhparent "
			   "WHERE i.inhrelid = $1::oid ORDER BY i.inhseqno", 1, params, &parents);
	if (parents.nrows > 0)
	{
		out->ninherits = parents.nrows;
		out->inherits = px_malloc((size_t) parents.nrows * sizeof(char *));
		memset(out->inherits, 0, (size_t) parents.nrows * sizeof(char *));
		for (int i = 0; i < parents.nrows; i++)
			out->inherits[i] = ws_name_to_utf8(ws_sql_value(&parents, i, 0));
		out->inherited_cols = px_malloc((size_t) Max(cols->nrows, 1) * sizeof(uint64));
		for (int i = 0; i < cols->nrows; i++)
			if (ws_sql_value(cols, i, 12)[0] != 't')
				out->inherited_cols[out->ninherited_cols++] = out->cols[i].tag;
		out->inherited_cons = px_malloc((size_t) (cols->nrows + others->nrows + 1) * sizeof(char *));
		memset(out->inherited_cons, 0, (size_t) (cols->nrows + others->nrows + 1) * sizeof(char *));
		for (int i = 0; i < cols->nrows; i++)
			if (out->cols[i].not_null != NULL && ws_sql_value(cols, i, 13)[0] != 't')
				out->inherited_cons[n++] = px_memdup(out->cols[i].not_null, strlen(out->cols[i].not_null) + 1);
		for (int i = 0; i < others->nrows; i++)
			if (ws_sql_value(others, i, 1)[0] == WS_CONSTRAINT_CHECK && ws_sql_value(others, i, 13)[0] != 't')
				out->inherited_cons[n++] = ws_name_to_utf8(ws_sql_value(others, i, 0));
		if (n > 1)
			qsort(out->inherited_cons, n, sizeof(char *), name_cmp);
		out->ninherited_cons = n;
	}
	ws_sql_rows_free(&parents);
}

static int
index_cmp(const void *a, const void *b)
{
	return strcmp(((const WsIndex *) a)->name, ((const WsIndex *) b)->name);
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
				others,
				idxs;
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

		WsSql->run("SELECT c.oid::text, c.relkind::text, c.relpersistence::text, n.nspbranch::text, "
				   "  c.relispartition::text, pg_get_expr(c.relpartbound, c.oid), "
				   "  (SELECT q.relname::text FROM pg_inherits i JOIN pg_class q ON q.oid = i.inhparent "
				   "    WHERE c.relispartition AND i.inhrelid = c.oid) "
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
		ws_fk_refuse(oid[0], table);
	}
	out->name = ws_name_to_utf8(table);
	{
		const char *oid[1] = {ws_sql_value(&rel, 0, 0)};

		WsSql->run("SELECT a.attnum::text, a.attname::text, "
				   "  format_type(a.atttypid, a.atttypmod), a.attnotnull::text, "
				   "  a.attidentity::text, a.attgenerated::text, "
				   "  pg_get_expr(d.adbin, d.adrelid), "
				   "  CASE WHEN a.attcollation <> t.typcollation THEN "
				   "    quote_ident(cn.nspname) || '.' || quote_ident(co.collname) END, "
				   "  a.attcoltag::text, "
				   "  (SELECT k.conname::text FROM pg_constraint k WHERE k.conrelid = a.attrelid "
				   "    AND k.contype = 'n' AND k.conkey[1] = a.attnum), "
				   "  (SELECT q.relname::text FROM pg_depend p JOIN pg_class q ON q.oid = p.objid "
				   "    WHERE p.classid = 'pg_class'::regclass AND p.refclassid = 'pg_class'::regclass "
				   "      AND p.refobjid = a.attrelid AND p.refobjsubid = a.attnum "
				   "      AND p.deptype = 'i' AND q.relkind = 'S'), "
				   "  a.atttypid::text, a.attislocal::text, "
				   "  (SELECT k.conislocal::text FROM pg_constraint k WHERE k.conrelid = a.attrelid "
				   "    AND k.contype = 'n' AND k.conkey[1] = a.attnum) "
				   "FROM pg_attribute a "
				   "JOIN pg_type t ON t.oid = a.atttypid "
				   "LEFT JOIN pg_attrdef d ON d.adrelid = a.attrelid AND d.adnum = a.attnum "
				   "LEFT JOIN pg_collation co ON co.oid = a.attcollation "
				   "LEFT JOIN pg_namespace cn ON cn.oid = co.collnamespace "
				   "WHERE a.attrelid = $1::oid AND a.attnum > 0 AND NOT a.attisdropped "
				   "ORDER BY a.attnum", 1, oid, &cols);
		WsSql->run("SELECT con.conname::text, con.contype::text, pg_get_expr(con.conbin, con.conrelid), "
				   "  array_to_string((i.indkey::int2[])[0:i.indnkeyatts - 1], ' '), "
				   "  i.indnullsnotdistinct::text, con.condeferrable::text, con.condeferred::text, "
				   "  con.oid::text, "
				   "  i.indexrelid::text, pg_get_expr(i.indpred, i.indrelid), "
				   "  con.convalidated::text, con.connoinherit::text, con.conenforced::text, "
				   "  con.conislocal::text "
				   "FROM pg_constraint con LEFT JOIN pg_index i ON i.indexrelid = con.conindid "
				   "WHERE con.conrelid = $1::oid AND con.contype IN ('c', 'u', 'x')", 1, oid, &others);
		WsSql->run("SELECT ic.relname::text, i.indexrelid::text, pg_get_indexdef(i.indexrelid), "
				   "  pg_get_expr(i.indpred, i.indrelid) "
				   "FROM pg_index i JOIN pg_class ic ON ic.oid = i.indexrelid "
				   "WHERE i.indrelid = $1::oid AND NOT EXISTS (SELECT FROM pg_constraint k "
				   "  WHERE k.conindid = i.indexrelid AND k.conrelid = i.indrelid "
				   "    AND k.contype <> 'f')",
				   1, oid, &idxs);
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
		c->name = ws_name_to_utf8(rc.names[i]);
		names_fit(typeStringToTypeName(ws_sql_value(&cols, i, 2), NULL)->names);
		c->type = to_utf8(ws_sql_value(&cols, i, 2));
		if (ws_sql_value(&cols, i, 3)[0] == 't')
		{
			if (ws_sql_value(&cols, i, 9) == NULL)
				elog(ERROR, "column \"%s\" of table \"%s\" is not null by no constraint", rc.names[i], table);
			c->not_null = ws_name_to_utf8(ws_sql_value(&cols, i, 9));
		}
		/* (a partition's is its table's, which attaching gives it) */
		c->identity = ws_sql_value(&rel, 0, 4)[0] == 't' ? WS_IDENTITY_NONE : identity[0];
		if (c->identity != WS_IDENTITY_NONE)
		{
			if (ws_sql_value(&cols, i, 10) == NULL)
				elog(ERROR, "identity column \"%s\" of table \"%s\" has no sequence", rc.names[i], table);
			c->identity_seq = ws_name_to_utf8(ws_sql_value(&cols, i, 10));
		}
		c->kind = generated[0] == ATTRIBUTE_GENERATED_STORED ? WS_COL_GENERATED_STORED :
			generated[0] == ATTRIBUTE_GENERATED_VIRTUAL ? WS_COL_GENERATED_VIRTUAL : WS_COL_STORED;
		if (collation != NULL)
			names_fit(stringToQualifiedNameList(collation, NULL));
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

		out->key_name = ws_name_to_utf8(ws_sql_value(&key, 0, 0));
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

		con->name = ws_name_to_utf8(ws_sql_value(&others, i, 0));
		con->kind = ws_sql_value(&others, i, 1)[0];
		if (con->kind == WS_CONSTRAINT_CHECK)
		{
			Buf			stored = BUF_INIT;

			ws_tags_template(ws_sql_value(&others, i, 2), read_tag_of, &rc, &stored);
			buf_append_byte(&stored, 0);
			con->expr = to_utf8((char *) stored.data);
			buf_free(&stored);
			con->no_inherit = ws_sql_value(&others, i, 11)[0] == 't';
			con->not_enforced = ws_sql_value(&others, i, 12)[0] != 't';
			con->not_valid = !con->not_enforced && ws_sql_value(&others, i, 10)[0] != 't';
		}
		else if (con->kind == WS_CONSTRAINT_EXCLUSION)
		{
			read_exclusion(table, ws_sql_value(&others, i, 7), ws_sql_value(&others, i, 8),
						   ws_sql_value(&others, i, 9), &rc, &con->excl);
			con->deferrable = ws_sql_value(&others, i, 5)[0] == 't';
			con->initdeferred = ws_sql_value(&others, i, 6)[0] == 't';
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
	for (int i = 0; i < out->nconstraints; i++)
		for (int j = 0; j < i; j++)
			if (out->constraints[i].kind == WS_CONSTRAINT_EXCLUSION &&
				out->constraints[j].kind == WS_CONSTRAINT_EXCLUSION &&
				exclusions_alike(&out->constraints[i], &out->constraints[j]))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("the schema of table \"%s\" cannot be read yet: it has two constraints alike, of which CREATE TABLE makes one",
								table)));
	out->nindexes = idxs.nrows;
	out->indexes = px_malloc((size_t) Max(idxs.nrows, 1) * sizeof(WsIndex));
	memset(out->indexes, 0, (size_t) Max(idxs.nrows, 1) * sizeof(WsIndex));
	for (int i = 0; i < idxs.nrows; i++)
		read_index(table, &idxs, i, &rc, &out->indexes[i]);
	if (out->nindexes > 1)
		qsort(out->indexes, out->nindexes, sizeof(WsIndex), index_cmp);
	if (relkind[0] == RELKIND_PARTITIONED_TABLE)
		read_partitioning(table, ws_sql_value(&rel, 0, 0), &rc, out);
	if (ws_sql_value(&rel, 0, 4)[0] != 't')
		read_inheritance(ws_sql_value(&rel, 0, 0), &cols, &others, out);
	if (ws_sql_value(&rel, 0, 4)[0] == 't')
	{
		if (ws_sql_value(&rel, 0, 5) == NULL || ws_sql_value(&rel, 0, 6) == NULL)
			elog(ERROR, "partition \"%s\" has no bound, or no table", table);
		out->partition_of = ws_name_to_utf8(ws_sql_value(&rel, 0, 6));
		out->partition_bound = to_utf8(ws_sql_value(&rel, 0, 5));
	}
	pfree(rc.names);
	pfree(rc.tags);
	ws_sql_rows_free(&rel);
	ws_sql_rows_free(&cols);
	ws_sql_rows_free(&key);
	ws_sql_rows_free(&others);
	ws_sql_rows_free(&idxs);
	WsSql->end(level);
	return true;
}

/* (both names in the server's encoding) */
pg_noreturn static void
not_inherited(const char *table, const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("the schema of table \"%s\" says \"%s\" is inherited, and it is not", table, what)));
}

void
ws_schema_set_inherited(const char *schema, const WsTableSchema *s)
{
	const char *problem = ws_schema_problem(s);
	Oid			nsp = get_namespace_oid(schema, false);
	char	   *table;
	Oid			relid;
	Relation	rel;
	Relation	catalog;

	if (problem != NULL)
		elog(ERROR, "%s", problem);
	if (NamespaceBranch(nsp) == 0)
		elog(ERROR, "schema \"%s\" is not versioned", schema);
	table = ws_name_from_utf8(s->name);
	relid = get_relname_relid(table, nsp);
	if (!OidIsValid(relid))
		elog(ERROR, "no table \"%s\" in schema \"%s\"", table, schema);
	rel = relation_open(relid, AccessExclusiveLock);
	relation_close(rel, NoLock);
	catalog = table_open(AttributeRelationId, RowExclusiveLock);
	for (int i = 0; i < s->ninherited_cols; i++)
	{
		const WsColumn *col = ws_schema_column(s, s->inherited_cols[i]);
		char	   *name = ws_name_from_utf8(col->name);
		HeapTuple	tup = SearchSysCacheCopyAttName(relid, name);

		if (!HeapTupleIsValid(tup))
			elog(ERROR, "no column \"%s\" in table \"%s\"", name, table);
		if (((Form_pg_attribute) GETSTRUCT(tup))->attinhcount == 0)
			not_inherited(table, name);
		((Form_pg_attribute) GETSTRUCT(tup))->attislocal = false;
		CatalogTupleUpdate(catalog, &tup->t_self, tup);
		heap_freetuple(tup);
		pfree(name);
	}
	table_close(catalog, RowExclusiveLock);
	catalog = table_open(ConstraintRelationId, RowExclusiveLock);
	for (int i = 0; i < s->ninherited_cons; i++)
	{
		char	   *name = ws_name_from_utf8(s->inherited_cons[i]);
		Oid			con = get_relation_constraint_oid(relid, name, true);
		HeapTuple	tup;

		if (!OidIsValid(con))
			not_inherited(table, name);
		tup = SearchSysCacheCopy1(CONSTROID, ObjectIdGetDatum(con));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "no constraint \"%s\" on table \"%s\"", name, table);
		if (((Form_pg_constraint) GETSTRUCT(tup))->coninhcount == 0)
			not_inherited(table, name);
		((Form_pg_constraint) GETSTRUCT(tup))->conislocal = false;
		CatalogTupleUpdate(catalog, &tup->t_self, tup);
		heap_freetuple(tup);
		pfree(name);
	}
	table_close(catalog, RowExclusiveLock);
	pfree(table);
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
	table = ws_name_from_utf8(s->name);
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
		char	   *name = ws_name_from_utf8(s->cols[i].name);
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
