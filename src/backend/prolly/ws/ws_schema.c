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
 * uvarint nconstraints and names, of what it has only by inheriting; storage
 * parameters with 'w', uvarint n and each one's toast byte, name and value; and
 * columns' settings with 'c', uvarint n, and each column that has any: uvarint tag,
 * storage byte, compression byte, statistics target plus one or 0, uvarint n and
 * each option's name and value; indexes' settings with 'j', uvarint n, and each
 * index that has any: name, uvarint ninclude and tags, uvarint noptions, names and
 * values, uvarint nstats, columns and targets, uvarint nkeyoptions and each column,
 * uvarint noptions, names and values; and with 'x', a flags byte for the key and
 * row security, a replica identity byte, its index's name if by index, and the clustered index's
 * name or nothing; and comments with 'm', uvarint n, and each one's kind byte, tag
 * or name as its kind has, and text; and who may do what with 'g', its owner's name
 * or nothing, uvarint n, and each grant's target byte, tag if not the table's,
 * grantee or nothing, grant option byte and privileges.  Any other ends there.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/ws/ws_schema.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/toast_compression.h"
#include "catalog/indexing.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_default_acl.h"
#include "catalog/pg_partitioned_table.h"
#include "catalog/pg_type.h"
#include "commands/vacuum.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/miscnodes.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "parser/parse_type.h"
#include "parser/parser.h"
#include "prolly/branch_catalog.h"
#include "prolly/ws/ws_fk.h"
#include "prolly/ws/ws_obj.h"
#include "prolly/ws/ws_role.h"
#include "prolly/ws/ws_schema.h"
#include "prolly/ws/ws_sql.h"
#include "prolly/ws/ws_tags.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/injection_point.h"
#include "utils/lsyscache.h"
#include "utils/regproc.h"
#include "utils/ruleutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
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

/* An index's settings in a schema in any encoding, whose order may be another's. */
static const WsIndexSettings *
index_settings_of(const WsTableSchema *s, const char *index)
{
	for (int i = 0; i < s->nindex_settings; i++)
		if (strcmp(s->index_settings[i].index, index) == 0)
			return &s->index_settings[i];
	return NULL;
}

/* The same, in a commit's order, already checked: a record may hold a great many. */
static const WsIndexSettings *
held_index_settings(const WsTableSchema *s, const char *index)
{
	int			lo = 0,
				hi = s->nindex_settings - 1;

	while (lo <= hi)
	{
		int			mid = lo + (hi - lo) / 2;
		int			cmp = strcmp(index, s->index_settings[mid].index);

		if (cmp == 0)
			return &s->index_settings[mid];
		if (cmp < 0)
			hi = mid - 1;
		else
			lo = mid + 1;
	}
	return NULL;
}

/* The INCLUDE of a key's or unique constraint's index. */
static bool
same_include(const WsTableSchema *s, const char *a, const char *b)
{
	const WsIndexSettings *x = held_index_settings(s, a);
	const WsIndexSettings *y = held_index_settings(s, b);

	return same_tags(x ? x->include : NULL, x ? x->ninclude : 0, y ? y->include : NULL, y ? y->ninclude : 0);
}

/* Whether two exclusions' keys have the same operator class parameters. */
static bool
same_key_options(const WsTableSchema *s, const char *a, const char *b)
{
	const WsIndexSettings *x = held_index_settings(s, a);
	const WsIndexSettings *y = held_index_settings(s, b);
	int			nx = x ? x->nkeyoptions : 0;

	if (nx != (y ? y->nkeyoptions : 0))
		return false;
	for (int i = 0; i < nx; i++)
	{
		if (x->keyoptions[i].column != y->keyoptions[i].column ||
			x->keyoptions[i].noptions != y->keyoptions[i].noptions)
			return false;
		for (int k = 0; k < x->keyoptions[i].noptions; k++)
			if (strcmp(x->keyoptions[i].options[k].name, y->keyoptions[i].options[k].name) != 0 ||
				strcmp(x->keyoptions[i].options[k].value, y->keyoptions[i].options[k].value) != 0)
				return false;
	}
	return true;
}

/* Whether CREATE TABLE would keep one index of the two (both exclusions). */
static bool
exclusions_alike(const WsTableSchema *s, const WsConstraint *a, const WsConstraint *b)
{
	if (!same_key_options(s, a->name, b->name))
		return false;
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
options_problem(const WsOption *options, int n)
{
	if (n < 0 || n > WS_MAX_OPTIONS)
		return "a number of options no table has";
	for (int i = 0; i < n; i++)
	{
		const WsOption *o = &options[i];

		if (o->name == NULL || o->name[0] == '\0' || strlen(o->name) >= NAMEDATALEN)
			return "an option of no name, or too long a one";
		if (o->value == NULL)
			return "an option with no value";
		/* (one order, so one set of bytes) */
		if (i > 0 && (options[i - 1].toast > o->toast ||
					  (options[i - 1].toast == o->toast &&
					   strcmp(options[i - 1].name, o->name) >= 0)))
			return "options not in order";
	}
	return NULL;
}

static const char *
column_settings_problem(const WsColumn *c)
{
	if (c->storage != WS_STORAGE_DEFAULT && c->storage != TYPSTORAGE_PLAIN &&
		c->storage != TYPSTORAGE_EXTERNAL && c->storage != TYPSTORAGE_EXTENDED &&
		c->storage != TYPSTORAGE_MAIN)
		return "a column of no known storage";
	if (c->compression != WS_COMPRESSION_DEFAULT && c->compression != TOAST_PGLZ_COMPRESSION &&
		c->compression != TOAST_LZ4_COMPRESSION)
		return "a column of no known compression";
	if (c->has_stattarget ? (c->stattarget < 0 || c->stattarget > MAX_STATISTICS_TARGET) :
		c->stattarget != 0)
		return "a column's statistics target out of range";
	for (int i = 0; i < c->noptions; i++)
		if (c->options[i].toast)
			return "a column's option of a TOAST table";
	return options_problem(c->options, c->noptions);
}

/* An index, by name, that the table has: what it holds, or false if none. */
static bool
find_index(const WsTableSchema *s, const char *name, bool *takes_include, const WsIndex **body)
{
	*takes_include = false;
	*body = NULL;
	if (s->key_name != NULL && strcmp(s->key_name, name) == 0)
	{
		*takes_include = true;
		return true;
	}
	/* (both in order by name by now, already checked: a record may hold a great many) */
	{
		int			lo = 0,
					hi = s->nconstraints - 1;

		while (lo <= hi)
		{
			int			mid = lo + (hi - lo) / 2;
			int			cmp = strcmp(name, s->constraints[mid].name);

			if (cmp == 0)
			{
				/* (a check has no relation, and an index may have its name) */
				if (s->constraints[mid].kind == WS_CONSTRAINT_CHECK)
					break;
				*takes_include = s->constraints[mid].kind == WS_CONSTRAINT_UNIQUE;
				*body = s->constraints[mid].kind == WS_CONSTRAINT_EXCLUSION ? &s->constraints[mid].excl : NULL;
				return true;
			}
			if (cmp < 0)
				hi = mid - 1;
			else
				lo = mid + 1;
		}
		lo = 0;
		hi = s->nindexes - 1;
		while (lo <= hi)
		{
			int			mid = lo + (hi - lo) / 2;
			int			cmp = strcmp(name, s->indexes[mid].name);

			if (cmp == 0)
			{
				*body = &s->indexes[mid];
				return true;
			}
			if (cmp < 0)
				hi = mid - 1;
			else
				lo = mid + 1;
		}
	}
	return false;
}

static const char *
index_settings_problem(const WsTableSchema *s)
{
	for (int i = 0; i < s->nindex_settings; i++)
	{
		const WsIndexSettings *set = &s->index_settings[i];
		bool		takes_include;
		const WsIndex *body;
		const char *problem;

		CHECK_FOR_INTERRUPTS();
		if (!find_index(s, set->index, &takes_include, &body))
			return "settings for an index the table does not have";
		if (set->ninclude < 0 || set->noptions < 0 || set->nstats < 0 || set->nkeyoptions < 0)
			return "a negative number of an index's settings";
		if (set->ninclude == 0 && set->noptions == 0 && set->nstats == 0 && set->nkeyoptions == 0)
			return "settings for an index saying it has none";
		if (set->ninclude > 0 && !takes_include)
			return "INCLUDE in settings for an index that holds its own, or is an exclusion's";
		if (set->ninclude > INDEX_MAX_KEYS)
			return "an index of too many columns";
		for (int k = 0; k < set->ninclude; k++)
		{
			if (ws_schema_column(s, set->include[k]) == NULL)
				return "an index including a column the table does not have";
		}
		for (int k = 0; k < set->noptions; k++)
			if (set->options[k].toast)
				return "an index's option of a TOAST table";
		problem = options_problem(set->options, set->noptions);
		if (problem != NULL)
			return problem;
		if (set->nstats > INDEX_MAX_KEYS || set->nkeyoptions > INDEX_MAX_KEYS)
			return "settings for more columns than an index has";
		for (int k = 0; k < set->nstats; k++)
		{
			if (set->stats[k].column < 1 || set->stats[k].column > INDEX_MAX_KEYS)
				return "a statistics target on no column of an index";
			if (k > 0 && set->stats[k - 1].column >= set->stats[k].column)
				return "an index's statistics targets not in order";
			if (set->stats[k].target < 0 || set->stats[k].target > MAX_STATISTICS_TARGET)
				return "an index's statistics target out of range";
			if (body == NULL || set->stats[k].column > body->nkeys ||
				body->keys[set->stats[k].column - 1].expr == NULL)
				return "a statistics target on a column of an index that is no expression";
		}
		for (int k = 0; k < set->nkeyoptions; k++)
		{
			const WsKeyOptions *ko = &set->keyoptions[k];

			if (k > 0 && set->keyoptions[k - 1].column >= ko->column)
				return "an index's keys' options not in order";
			if (body == NULL || ko->column < 1 || ko->column > body->nkeys ||
				body->keys[ko->column - 1].opclass == NULL)
				return "options on a key with no operator class";
			if (ko->noptions <= 0)
				return "options on a key saying it has none";
			for (int j = 0; j < ko->noptions; j++)
				if (ko->options[j].toast)
					return "an index's option of a TOAST table";
			problem = options_problem(ko->options, ko->noptions);
			if (problem != NULL)
				return problem;
		}
	}
	return NULL;
}

static const char *
table_settings_problem(const WsTableSchema *s)
{
	bool		takes_include;
	const WsIndex *body;

	if (s->key_initdeferred && !s->key_deferrable)
		return "a key initially deferred that is not deferrable";
	if (s->key_deferrable && s->nkey == 0)
		return "a key deferrable with no key";
	if (s->replica_identity != WS_REPLICA_DEFAULT && s->replica_identity != WS_REPLICA_FULL &&
		s->replica_identity != WS_REPLICA_NOTHING && s->replica_identity != WS_REPLICA_INDEX)
		return "a replica identity of no known kind";
	if ((s->replica_identity == WS_REPLICA_INDEX) != (s->replica_index != NULL))
		return "a replica identity by index with no index, or an index with no such identity";
	if (s->replica_index != NULL)
	{
		if (!find_index(s, s->replica_index, &takes_include, &body))
			return "a replica identity by an index the table does not have";
		if (s->key_name != NULL && strcmp(s->replica_index, s->key_name) == 0 && s->key_deferrable)
			return "a replica identity by a deferrable index";
		for (int i = 0; i < s->nconstraints; i++)
			if (s->constraints[i].kind != WS_CONSTRAINT_CHECK &&
				strcmp(s->constraints[i].name, s->replica_index) == 0 &&
				(s->constraints[i].kind != WS_CONSTRAINT_UNIQUE || s->constraints[i].deferrable))
				return "a replica identity by a deferrable index, or not a unique one";
		for (int i = 0; i < s->nindexes; i++)
			if (strcmp(s->indexes[i].name, s->replica_index) == 0 &&
				(!s->indexes[i].unique || s->indexes[i].predicate != NULL))
				return "a replica identity by a partial index, or not a unique one";
	}
	if (s->clustered != NULL && !find_index(s, s->clustered, &takes_include, &body))
		return "clustered on an index the table does not have";
	return NULL;
}

static const char *
comments_problem(const WsTableSchema *s)
{
	if (s->ncomments < 0)
		return "a negative number of comments";
	for (int i = 0; i < s->ncomments; i++)
	{
		const WsComment *m = &s->comments[i];
		bool		takes_include;
		const WsIndex *body;

		CHECK_FOR_INTERRUPTS();
		if (m->text == NULL || m->text[0] == '\0')
			return "a comment with no text";
		switch (m->kind)
		{
			case WS_COMMENT_TABLE:
				if (m->tag != 0 || m->name != NULL)
					return "a table's comment on something else";
				break;
			case WS_COMMENT_COLUMN:
			case WS_COMMENT_SEQUENCE:
				{
					const WsColumn *col = ws_schema_column(s, m->tag);

					if (m->name != NULL || col == NULL)
						return "a comment on a column the table does not have";
					if (m->kind == WS_COMMENT_SEQUENCE && col->identity_seq == NULL)
						return "a comment on a sequence the table does not have";
					break;
				}
			case WS_COMMENT_INDEX:
				if (m->tag != 0 || m->name == NULL || !find_index(s, m->name, &takes_include, &body))
					return "a comment on an index the table does not have";
				break;
			case WS_COMMENT_CONSTRAINT:
				{
					bool		found = false;

					if (m->tag != 0 || m->name == NULL)
						return "a comment on a constraint the table does not have";
					found = s->key_name != NULL && strcmp(s->key_name, m->name) == 0;
					/* (in order by name, already checked: a record may hold a great many) */
					{
						int			lo = 0,
									hi = s->nconstraints - 1;

						while (lo <= hi && !found)
						{
							int			mid = lo + (hi - lo) / 2;
							int			cmp = strcmp(m->name, s->constraints[mid].name);

							found = cmp == 0;
							if (cmp < 0)
								hi = mid - 1;
							else
								lo = mid + 1;
						}
					}
					for (int k = 0; k < s->ncols && !found; k++)
						found = s->cols[k].not_null != NULL && strcmp(s->cols[k].not_null, m->name) == 0;
					if (!found)
						return "a comment on a constraint the table does not have";
					break;
				}
			default:
				return "a comment on no known kind of thing";
		}
		/* (one order, so one set of bytes) */
		if (i > 0)
		{
			const WsComment *prior = &s->comments[i - 1];
			int			cmp = (unsigned char) prior->kind - (unsigned char) m->kind;

			if (cmp == 0 && prior->tag != m->tag)
				cmp = prior->tag < m->tag ? -1 : 1;
			if (cmp == 0 && m->name != NULL)
				cmp = strcmp(prior->name, m->name);
			if (cmp >= 0)
				return "comments not in order";
		}
	}
	return NULL;
}

/* The privileges each target can have. */
static const char *
grantable_letters(char target)
{
	switch (target)
	{
		case WS_GRANT_TABLE:
			return "Ddamrtwx";
		case WS_GRANT_COLUMN:
			return "arwx";
		case WS_GRANT_SEQUENCE:
			return "Urw";
	}
	return NULL;
}

static const char *
grants_problem(const WsTableSchema *s)
{
	if (s->owner != NULL && (s->owner[0] == '\0' || strlen(s->owner) >= NAMEDATALEN))
		return "an owner of no name, or too long a one";
	if (s->ngrants < 0)
		return "a negative number of grants";
	for (int i = 0; i < s->ngrants; i++)
	{
		const WsGrant *g = &s->grants[i];
		const char *allowed = grantable_letters(g->target);

		CHECK_FOR_INTERRUPTS();
		if (allowed == NULL)
			return "a grant on no known kind of thing";
		if (g->target == WS_GRANT_TABLE)
		{
			if (g->tag != 0)
				return "a table's grant on something else";
		}
		else
		{
			const WsColumn *col = ws_schema_column(s, g->tag);

			if (col == NULL || (g->target == WS_GRANT_SEQUENCE && col->identity_seq == NULL))
				return "a grant on what the table does not have";
		}
		if (g->grantee != NULL && (g->grantee[0] == '\0' || strlen(g->grantee) >= NAMEDATALEN))
			return "a grantee of no name, or too long a one";
		if (g->grantee != NULL && s->owner != NULL && strcmp(g->grantee, s->owner) == 0)
			return "a grant to the table's owner";
		if (g->grantee == NULL && g->grant_option)
			return "a grant option to PUBLIC, who cannot have one";
		if (g->privileges == NULL || g->privileges[0] == '\0')
			return "a grant of no privileges";
		for (const char *l = g->privileges; *l; l++)
		{
			if (strchr(allowed, *l) == NULL)
				return "a privilege that target cannot have";
			if (l > g->privileges && (unsigned char) l[-1] >= (unsigned char) *l)
				return "privileges not in order";
		}
		if (i > 0)
		{
			const WsGrant *prior = &s->grants[i - 1];
			int			cmp = (unsigned char) prior->target - (unsigned char) g->target;

			if (cmp == 0 && prior->tag != g->tag)
				cmp = prior->tag < g->tag ? -1 : 1;
			if (cmp == 0)
			{
				if ((prior->grantee == NULL) != (g->grantee == NULL))
					cmp = prior->grantee == NULL ? -1 : 1;
				else if (g->grantee != NULL)
					cmp = strcmp(prior->grantee, g->grantee);
			}
			if (cmp == 0)
			{
				if (prior->grant_option == g->grant_option)
					cmp = 1;
				else
				{
					cmp = prior->grant_option ? 1 : -1;
					for (const char *l = g->privileges; *l; l++)
						if (strchr(prior->privileges, *l) != NULL)
							return "a privilege granted twice";
				}
			}
			if (cmp >= 0)
				return "grants not in order";
		}
	}
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
		{
			const char *problem = column_settings_problem(c);

			if (problem != NULL)
				return problem;
		}
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
	/* (before the constraints' pairs, which look their settings up by this order) */
	if (s->nindex_settings < 0)
		return "a negative number of index settings";
	for (int i = 0; i < s->nindex_settings; i++)
	{
		const char *index = s->index_settings[i].index;

		if (index == NULL || index[0] == '\0')
			return "settings for an index of no name";
		if (i > 0 && strcmp(s->index_settings[i - 1].index, index) >= 0)
			return "index settings not in order";
	}
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
			if (s->nkey > 0 && !con->nulls_not_distinct && con->deferrable == s->key_deferrable &&
				con->initdeferred == s->key_initdeferred &&
				same_tags(con->cols, con->ncols, s->key, s->nkey) &&
				same_include(s, con->name, s->key_name))
				return "a unique constraint alike the key";
			for (int j = 0; j < i; j++)
			{
				const WsConstraint *prior = &s->constraints[j];

				if (prior->kind == WS_CONSTRAINT_UNIQUE &&
					prior->nulls_not_distinct == con->nulls_not_distinct &&
					prior->deferrable == con->deferrable &&
					prior->initdeferred == con->initdeferred &&
					same_tags(con->cols, con->ncols, prior->cols, prior->ncols) &&
					same_include(s, con->name, prior->name))
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
					exclusions_alike(s, con, &s->constraints[j]))
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
		problem = options_problem(s->options, s->noptions);
		if (problem != NULL)
			return problem;
		problem = index_settings_problem(s);
		if (problem != NULL)
			return problem;
		problem = table_settings_problem(s);
		if (problem != NULL)
			return problem;
		problem = comments_problem(s);
		if (problem != NULL)
			return problem;
		problem = grants_problem(s);
		if (problem != NULL)
			return problem;
	}
	return column_names_problem(s);
}

#define WS_PARTITION_OF 'o'
#define WS_INHERITS 'i'
#define WS_OPTIONS 'w'
#define WS_COLUMN_SETTINGS 'c'
#define WS_INDEX_SETTINGS 'j'
#define WS_TABLE_SETTINGS 'x'
#define WS_COMMENTS 'm'
#define WS_GRANTS 'g'
#define KEY_DEFERRABLE 0x01
#define KEY_INITDEFERRED 0x02
#define TABLE_ROW_SECURITY 0x04
#define TABLE_FORCE_ROW_SECURITY 0x08
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
put_named_options(Buf *out, const WsOption *options, int n)
{
	buf_append_uvarint(out, (uint64) n);
	for (int i = 0; i < n; i++)
	{
		put_string(out, options[i].name);
		put_string(out, options[i].value);
	}
}

static bool
has_settings(const WsColumn *c)
{
	return c->storage != WS_STORAGE_DEFAULT || c->compression != WS_COMPRESSION_DEFAULT ||
		c->has_stattarget || c->noptions > 0;
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
	if (s->noptions > 0)
	{
		buf_append_byte(out, WS_OPTIONS);
		buf_append_uvarint(out, (uint64) s->noptions);
		for (int i = 0; i < s->noptions; i++)
		{
			buf_append_byte(out, s->options[i].toast ? 1 : 0);
			put_string(out, s->options[i].name);
			put_string(out, s->options[i].value);
		}
	}
	{
		int			n = 0;

		for (int i = 0; i < s->ncols; i++)
			n += has_settings(&s->cols[i]) ? 1 : 0;
		if (n > 0)
		{
			buf_append_byte(out, WS_COLUMN_SETTINGS);
			buf_append_uvarint(out, (uint64) n);
			for (int i = 0; i < s->ncols; i++)
			{
				const WsColumn *col = &s->cols[i];

				if (!has_settings(col))
					continue;
				buf_append_uvarint(out, col->tag);
				buf_append_byte(out, (uint8) col->storage);
				buf_append_byte(out, (uint8) col->compression);
				buf_append_uvarint(out, col->has_stattarget ? (uint64) col->stattarget + 1 : 0);
				buf_append_uvarint(out, (uint64) col->noptions);
				for (int k = 0; k < col->noptions; k++)
				{
					put_string(out, col->options[k].name);
					put_string(out, col->options[k].value);
				}
			}
		}
	}
	if (s->nindex_settings > 0)
	{
		buf_append_byte(out, WS_INDEX_SETTINGS);
		buf_append_uvarint(out, (uint64) s->nindex_settings);
		for (int i = 0; i < s->nindex_settings; i++)
		{
			const WsIndexSettings *set = &s->index_settings[i];

			put_string(out, set->index);
			buf_append_uvarint(out, (uint64) set->ninclude);
			for (int k = 0; k < set->ninclude; k++)
				buf_append_uvarint(out, set->include[k]);
			put_named_options(out, set->options, set->noptions);
			buf_append_uvarint(out, (uint64) set->nstats);
			for (int k = 0; k < set->nstats; k++)
			{
				buf_append_uvarint(out, (uint64) set->stats[k].column);
				buf_append_uvarint(out, (uint64) set->stats[k].target);
			}
			buf_append_uvarint(out, (uint64) set->nkeyoptions);
			for (int k = 0; k < set->nkeyoptions; k++)
			{
				buf_append_uvarint(out, (uint64) set->keyoptions[k].column);
				put_named_options(out, set->keyoptions[k].options, set->keyoptions[k].noptions);
			}
		}
	}
	if (s->key_deferrable || s->replica_identity != WS_REPLICA_DEFAULT || s->clustered != NULL ||
		s->row_security || s->force_row_security)
	{
		buf_append_byte(out, WS_TABLE_SETTINGS);
		buf_append_byte(out, (s->key_deferrable ? KEY_DEFERRABLE : 0) |
						(s->key_initdeferred ? KEY_INITDEFERRED : 0) |
						(s->row_security ? TABLE_ROW_SECURITY : 0) |
						(s->force_row_security ? TABLE_FORCE_ROW_SECURITY : 0));
		buf_append_byte(out, (uint8) s->replica_identity);
		if (s->replica_identity == WS_REPLICA_INDEX)
			put_string(out, s->replica_index);
		put_optional(out, s->clustered);
	}
	if (s->ncomments > 0)
	{
		buf_append_byte(out, WS_COMMENTS);
		buf_append_uvarint(out, (uint64) s->ncomments);
		for (int i = 0; i < s->ncomments; i++)
		{
			const WsComment *m = &s->comments[i];

			buf_append_byte(out, (uint8) m->kind);
			if (m->kind == WS_COMMENT_COLUMN || m->kind == WS_COMMENT_SEQUENCE)
				buf_append_uvarint(out, m->tag);
			else if (m->kind != WS_COMMENT_TABLE)
				put_string(out, m->name);
			put_string(out, m->text);
		}
	}
	if (s->owner != NULL || s->ngrants > 0)
	{
		buf_append_byte(out, WS_GRANTS);
		put_optional(out, s->owner);
		buf_append_uvarint(out, (uint64) s->ngrants);
		for (int i = 0; i < s->ngrants; i++)
		{
			const WsGrant *g = &s->grants[i];

			buf_append_byte(out, (uint8) g->target);
			if (g->target != WS_GRANT_TABLE)
				buf_append_uvarint(out, g->tag);
			put_optional(out, g->grantee);
			buf_append_byte(out, g->grant_option ? 1 : 0);
			put_string(out, g->privileges);
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
get_named_options(const uint8 **p, const uint8 *end, WsOption **options, int *n)
{
	uint64		count = get_uvarint(p, end);

	if (count > WS_MAX_OPTIONS)
		malformed("more options than a table has");
	/* (each is at least two bytes: two lengths) */
	if (count > (uint64) (end - *p) / 2)
		malformed("more options than bytes");
	*options = px_malloc((size_t) Max(count, 1) * sizeof(WsOption));
	memset(*options, 0, (size_t) Max(count, 1) * sizeof(WsOption));
	*n = (int) count;
	for (int i = 0; i < *n; i++)
	{
		(*options)[i].name = get_string(p, end);
		(*options)[i].value = get_string(p, end);
	}
}

static void
get_index_settings(const uint8 **p, const uint8 *end, WsIndexSettings *set)
{
	uint64		n;

	set->index = get_string(p, end);
	n = get_uvarint(p, end);
	if (n > INDEX_MAX_KEYS)
		malformed("an index of too many columns");
	set->include = px_malloc((size_t) Max(n, 1) * sizeof(uint64));
	set->ninclude = (int) n;
	for (int k = 0; k < set->ninclude; k++)
		set->include[k] = get_uvarint(p, end);
	get_named_options(p, end, &set->options, &set->noptions);
	n = get_uvarint(p, end);
	if (n > INDEX_MAX_KEYS)
		malformed("settings for more columns than an index has");
	set->stats = px_malloc((size_t) Max(n, 1) * sizeof(WsIndexStat));
	set->nstats = (int) n;
	for (int k = 0; k < set->nstats; k++)
	{
		uint64		column = get_uvarint(p, end);
		uint64		target = get_uvarint(p, end);

		if (column > INDEX_MAX_KEYS || target > MAX_STATISTICS_TARGET)
			malformed("an index's statistics target out of range");
		set->stats[k].column = (int) column;
		set->stats[k].target = (int) target;
	}
	n = get_uvarint(p, end);
	if (n > INDEX_MAX_KEYS)
		malformed("settings for more columns than an index has");
	set->keyoptions = px_malloc((size_t) Max(n, 1) * sizeof(WsKeyOptions));
	memset(set->keyoptions, 0, (size_t) Max(n, 1) * sizeof(WsKeyOptions));
	set->nkeyoptions = (int) n;
	for (int k = 0; k < set->nkeyoptions; k++)
	{
		uint64		column = get_uvarint(p, end);

		if (column > INDEX_MAX_KEYS)
			malformed("an index's keys' options out of range");
		set->keyoptions[k].column = (int) column;
		get_named_options(p, end, &set->keyoptions[k].options, &set->keyoptions[k].noptions);
	}
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
	if (p != end && *p != WS_PARTITION_OF && *p != WS_INHERITS && *p != WS_OPTIONS &&
		*p != WS_COLUMN_SETTINGS && *p != WS_INDEX_SETTINGS && *p != WS_TABLE_SETTINGS &&
		*p != WS_COMMENTS && *p != WS_GRANTS)
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
	if (p != end && *p == WS_OPTIONS)
	{
		p++;
		n = get_uvarint(&p, end);
		if (n == 0)
			malformed("it has no options, at length");
		if (n > WS_MAX_OPTIONS)
			malformed("more options than a table has");
		/* (each is at least three bytes: its toast byte and two lengths) */
		if (n > (uint64) (end - p) / 3)
			malformed("more options than bytes");
		s->options = px_malloc((size_t) n * sizeof(WsOption));
		memset(s->options, 0, (size_t) n * sizeof(WsOption));
		s->noptions = (int) n;
		for (int i = 0; i < s->noptions; i++)
		{
			uint8		toast = get_byte(&p, end);

			if (toast > 1)
				malformed("an option's toast is neither");
			s->options[i].toast = toast == 1;
			s->options[i].name = get_string(&p, end);
			s->options[i].value = get_string(&p, end);
		}
	}
	if (p != end && *p == WS_COLUMN_SETTINGS)
	{
		int			last = -1;

		p++;
		n = get_uvarint(&p, end);
		if (n == 0)
			malformed("it has no column settings, at length");
		if (n > (uint64) s->ncols)
			malformed("settings for more columns than the table has");
		for (uint64 i = 0; i < n; i++)
		{
			uint64		tag = get_uvarint(&p, end);
			int			at = -1;
			WsColumn   *col;
			uint64		target;

			for (int k = 0; k < s->ncols && at < 0; k++)
				if (s->cols[k].tag == tag)
					at = k;
			if (at < 0)
				malformed("settings for a column the table does not have");
			/* (one order, so one set of bytes) */
			if (at <= last)
				malformed("column settings not in attribute order");
			last = at;
			col = &s->cols[at];
			col->storage = (char) get_byte(&p, end);
			col->compression = (char) get_byte(&p, end);
			target = get_uvarint(&p, end);
			if (target > (uint64) MAX_STATISTICS_TARGET + 1)
				malformed("a column's statistics target out of range");
			col->has_stattarget = target > 0;
			col->stattarget = target > 0 ? (int) target - 1 : 0;
			target = get_uvarint(&p, end);
			if (target > WS_MAX_OPTIONS)
				malformed("more options than a table has");
			/* (each is at least two bytes: two lengths) */
			if (target > (uint64) (end - p) / 2)
				malformed("more options than bytes");
			if (target > 0)
			{
				col->options = px_malloc((size_t) target * sizeof(WsOption));
				memset(col->options, 0, (size_t) target * sizeof(WsOption));
				col->noptions = (int) target;
				for (int k = 0; k < col->noptions; k++)
				{
					col->options[k].name = get_string(&p, end);
					col->options[k].value = get_string(&p, end);
				}
			}
			/* (a column with none is not listed) */
			if (!has_settings(col))
				malformed("settings for a column saying it has none");
		}
	}
	if (p != end && *p == WS_INDEX_SETTINGS)
	{
		p++;
		n = get_uvarint(&p, end);
		if (n == 0)
			malformed("it has no index settings, at length");
		/* (each is at least five bytes: its name and four counts) */
		if (n > (uint64) (end - p) / 5)
			malformed("more index settings than bytes");
		s->index_settings = px_malloc((size_t) Min(n, 16) * sizeof(WsIndexSettings));
		memset(s->index_settings, 0, (size_t) Min(n, 16) * sizeof(WsIndexSettings));
		for (uint64 i = 0; i < n; i++)
		{
			/* (grown as they are read: each is far bigger here than there) */
			if (i >= 16 && (i & (i - 1)) == 0)
			{
				s->index_settings = px_realloc(s->index_settings, (size_t) Min(i * 2, n) * sizeof(WsIndexSettings));
				memset(s->index_settings + i, 0, (size_t) (Min(i * 2, n) - i) * sizeof(WsIndexSettings));
			}
			s->nindex_settings = (int) i + 1;
			get_index_settings(&p, end, &s->index_settings[i]);
		}
	}
	if (p != end && *p == WS_TABLE_SETTINGS)
	{
		uint8		flags;

		p++;
		flags = get_byte(&p, end);
		if (flags & ~(KEY_DEFERRABLE | KEY_INITDEFERRED | TABLE_ROW_SECURITY | TABLE_FORCE_ROW_SECURITY))
			malformed("a table's settings' flags not known");
		s->key_deferrable = (flags & KEY_DEFERRABLE) != 0;
		s->key_initdeferred = (flags & KEY_INITDEFERRED) != 0;
		s->row_security = (flags & TABLE_ROW_SECURITY) != 0;
		s->force_row_security = (flags & TABLE_FORCE_ROW_SECURITY) != 0;
		s->replica_identity = (char) get_byte(&p, end);
		if (s->replica_identity == WS_REPLICA_INDEX)
			s->replica_index = get_string(&p, end);
		s->clustered = get_optional(&p, end);
		if (!s->key_deferrable && s->replica_identity == WS_REPLICA_DEFAULT && s->clustered == NULL &&
			!s->row_security && !s->force_row_security)
			malformed("a table's settings saying it has none");
	}
	if (p != end && *p == WS_COMMENTS)
	{
		p++;
		n = get_uvarint(&p, end);
		if (n == 0)
			malformed("it has no comments, at length");
		/* (each is at least two bytes: its kind and its text's length) */
		if (n > (uint64) (end - p) / 2)
			malformed("more comments than bytes");
		s->comments = px_malloc((size_t) Min(n, 16) * sizeof(WsComment));
		memset(s->comments, 0, (size_t) Min(n, 16) * sizeof(WsComment));
		for (uint64 i = 0; i < n; i++)
		{
			WsComment  *m;

			/* (grown as they are read: each is bigger here than there) */
			if (i >= 16 && (i & (i - 1)) == 0)
			{
				s->comments = px_realloc(s->comments, (size_t) Min(i * 2, n) * sizeof(WsComment));
				memset(s->comments + i, 0, (size_t) (Min(i * 2, n) - i) * sizeof(WsComment));
			}
			s->ncomments = (int) i + 1;
			m = &s->comments[i];
			m->kind = (char) get_byte(&p, end);
			if (m->kind == WS_COMMENT_COLUMN || m->kind == WS_COMMENT_SEQUENCE)
				m->tag = get_uvarint(&p, end);
			else if (m->kind != WS_COMMENT_TABLE)
				m->name = get_string(&p, end);
			m->text = get_string(&p, end);
		}
	}
	if (p != end && *p == WS_GRANTS)
	{
		p++;
		s->owner = get_optional(&p, end);
		n = get_uvarint(&p, end);
		if (n == 0 && s->owner == NULL)
			malformed("it says it has no owner and no grants, at length");
		/* (each is at least four bytes: target, grantee's length, option, privileges' length) */
		if (n > (uint64) (end - p) / 4)
			malformed("more grants than bytes");
		s->grants = px_malloc((size_t) Max(Min(n, 16), 1) * sizeof(WsGrant));
		memset(s->grants, 0, (size_t) Max(Min(n, 16), 1) * sizeof(WsGrant));
		for (uint64 i = 0; i < n; i++)
		{
			WsGrant    *g;
			uint8		option;

			/* (grown as they are read: each is bigger here than there) */
			if (i >= 16 && (i & (i - 1)) == 0)
			{
				s->grants = px_realloc(s->grants, (size_t) Min(i * 2, n) * sizeof(WsGrant));
				memset(s->grants + i, 0, (size_t) (Min(i * 2, n) - i) * sizeof(WsGrant));
			}
			s->ngrants = (int) i + 1;
			g = &s->grants[i];
			g->target = (char) get_byte(&p, end);
			if (g->target != WS_GRANT_TABLE)
				g->tag = get_uvarint(&p, end);
			g->grantee = get_optional(&p, end);
			option = get_byte(&p, end);
			if (option > 1)
				malformed("a grant's option is neither");
			g->grant_option = option == 1;
			g->privileges = get_string(&p, end);
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

static void
options_free(WsOption *options, int n)
{
	for (int i = 0; i < n; i++)
	{
		px_free(options[i].name);
		px_free(options[i].value);
	}
	px_free(options);
}

void
ws_schema_free(WsTableSchema *s)
{
	for (int i = 0; i < s->nindex_settings; i++)
	{
		WsIndexSettings *set = &s->index_settings[i];

		px_free(set->index);
		px_free(set->include);
		options_free(set->options, set->noptions);
		px_free(set->stats);
		for (int k = 0; k < set->nkeyoptions; k++)
			options_free(set->keyoptions[k].options, set->keyoptions[k].noptions);
		px_free(set->keyoptions);
	}
	for (int i = 0; i < s->ncomments; i++)
	{
		px_free(s->comments[i].name);
		px_free(s->comments[i].text);
	}
	px_free(s->comments);
	px_free(s->owner);
	for (int i = 0; i < s->ngrants; i++)
	{
		px_free(s->grants[i].grantee);
		px_free(s->grants[i].privileges);
	}
	px_free(s->grants);
	px_free(s->index_settings);
	px_free(s->replica_index);
	px_free(s->clustered);
	options_free(s->options, s->noptions);
	for (int i = 0; i < s->ncols; i++)
		options_free(s->cols[i].options, s->cols[i].noptions);
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

/*
 * The catalogs come first and temporary objects last, so that nothing of the schema's
 * can be taken for theirs or the other way about: what the schema shadows is printed
 * qualified, which a record refuses.
 */
int
ws_schema_path_begin(const char *schema)
{
	int			nest = NewGUCNestLevel();
	char	   *path = psprintf("pg_catalog, %s, pg_temp", quote_identifier(schema));

	(void) set_config_option("search_path", path, PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);
	pfree(path);
	return nest;
}

void
ws_schema_path_end(int nest)
{
	AtEOXact_GUC(true, nest);
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

static void put_options(const WsOption *options, int n, Buf *out);

/* The options on an index's kth key (from 0), or none. */
static const WsKeyOptions *
key_options_of(const WsIndexSettings *set, int k)
{
	for (int i = 0; set != NULL && i < set->nkeyoptions; i++)
		if (set->keyoptions[i].column == k + 1)
			return &set->keyoptions[i];
	return NULL;
}

/* An index's keys and INCLUDE, in the order both statements have them. */
static void
put_index_columns(const WsTableSchema *s, const WsIndex *ix, const WsIndexSettings *set, Buf *out)
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
			const WsKeyOptions *ko = key_options_of(set, k);

			ws_put_text(out, " ");
			ws_put_text(out, key->opclass);
			if (ko != NULL)
			{
				ws_put_text(out, " ");
				put_options(ko->options, ko->noptions, out);
			}
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

/* Whether a list of parameters as parsed is the record's: names, values and all. */
static bool
same_options(const WsOption *options, int n, List *parsed)
{
	int			i = 0;
	ListCell   *lc;

	if (list_length(parsed) != n)
		return false;
	foreach(lc, parsed)
	{
		DefElem    *d = lfirst_node(DefElem, lc);
		const WsOption *o = &options[i++];

		if (d->defaction != DEFELEM_UNSPEC || strcmp(d->defname, o->name) != 0 ||
			(d->defnamespace != NULL) != o->toast ||
			(d->defnamespace != NULL && strcmp(d->defnamespace, "toast") != 0) ||
			d->arg == NULL || !IsA(d->arg, String) || strcmp(strVal(d->arg), o->value) != 0)
			return false;
	}
	return true;
}

static const char *
storage_word(char storage)
{
	switch (storage)
	{
		case TYPSTORAGE_PLAIN:
			return "plain";
		case TYPSTORAGE_EXTERNAL:
			return "external";
		case TYPSTORAGE_MAIN:
			return "main";
	}
	return "extended";
}

/* A list of parameters' name = 'value', each value a string, for WITH or SET. */
static void
put_options(const WsOption *options, int n, Buf *out)
{
	ws_put_text(out, "(");
	for (int i = 0; i < n; i++)
	{
		if (i > 0)
			ws_put_text(out, ", ");
		if (options[i].toast)
			ws_put_text(out, "toast.");
		ws_put_identifier(out, options[i].name);
		ws_put_text(out, " = ");
		ws_put_text(out, quote_literal_cstr(options[i].value));
	}
	ws_put_text(out, ")");
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

/* A constraint's index's INCLUDE and WITH. */
static void
put_constraint_settings(const WsTableSchema *s, const WsIndexSettings *set, Buf *out)
{
	if (set == NULL)
		return;
	if (set->ninclude > 0)
	{
		ws_put_text(out, " INCLUDE (");
		for (int k = 0; k < set->ninclude; k++)
		{
			if (k > 0)
				ws_put_text(out, ", ");
			ws_put_identifier(out, ws_schema_column(s, set->include[k])->name);
		}
		ws_put_text(out, ")");
	}
	if (set->noptions > 0)
	{
		ws_put_text(out, " WITH ");
		put_options(set->options, set->noptions, out);
	}
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
		if (c->storage != WS_STORAGE_DEFAULT)
		{
			ws_put_text(out, " STORAGE ");
			ws_put_text(out, storage_word(c->storage));
		}
		if (c->compression != WS_COMPRESSION_DEFAULT)
			ws_put_text(out, c->compression == TOAST_LZ4_COMPRESSION ? " COMPRESSION lz4" : " COMPRESSION pglz");
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
		put_constraint_settings(s, index_settings_of(s, s->key_name), out);
		if (s->key_deferrable)
			ws_put_text(out, s->key_initdeferred ? " DEFERRABLE INITIALLY DEFERRED" : " DEFERRABLE");
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
			put_index_columns(s, &con->excl, index_settings_of(s, con->name), out);
			put_constraint_settings(s, index_settings_of(s, con->name), out);
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
			put_constraint_settings(s, index_settings_of(s, con->name), out);
		}
		if (con->deferrable)
			ws_put_text(out, con->initdeferred ? " DEFERRABLE INITIALLY DEFERRED" : " DEFERRABLE");
	}
	ws_put_text(out, any ? "\n)" : ")");
	if (s->noptions > 0 && s->partition == WS_PARTITION_NONE)
	{
		ws_put_text(out, " WITH ");
		put_options(s->options, s->noptions, out);
	}
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

static bool same_options(const WsOption *options, int n, List *parsed);

/* An INCLUDE and a WITH as parsed are the settings', of a constraint's index. */
static bool
same_constraint_settings(const WsTableSchema *s, const WsIndexSettings *set, List *including, List *options)
{
	int			k = 0;
	ListCell   *lc;

	if (list_length(including) != (set ? set->ninclude : 0) ||
		!same_options(set ? set->options : NULL, set ? set->noptions : 0, options))
		return false;
	foreach(lc, including)
		if (strcmp(strVal(lfirst(lc)), ws_schema_column(s, set->include[k++])->name) != 0)
			return false;
	return true;
}

/* An index's key as parsed is the record's; false if not. */
static bool
same_index_key(const WsTableSchema *s, const WsIndexKey *key, const WsKeyOptions *ko, const IndexElem *elem)
{
	if (elem->indexcolname != NULL ||
		!same_options(ko ? ko->options : NULL, ko ? ko->noptions : 0, elem->opclassopts) ||
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
	if (def->fdwoptions != NIL ||
		(def->storage_name != NULL) != (c->storage != WS_STORAGE_DEFAULT) ||
		(def->compression != NULL) != (c->compression != WS_COMPRESSION_DEFAULT) ||
		(def->storage_name != NULL && strcmp(def->storage_name, storage_word(c->storage)) != 0) ||
		(def->compression != NULL &&
		 strcmp(def->compression, c->compression == TOAST_LZ4_COMPRESSION ? "lz4" : "pglz") != 0))
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
		in_statement->indexspace != NULL || in_statement->indexname != NULL ||
		in_statement->without_overlaps || in_statement->pktable != NULL)
		not_the_record(s, "another constraint");
	if (con->kind != WS_CONSTRAINT_EXCLUSION &&
		(in_statement->exclusions != NIL || in_statement->where_clause != NULL))
		not_the_record(s, "another constraint");
	if (con->kind == WS_CONSTRAINT_CHECK && (in_statement->including != NIL || in_statement->options != NIL))
		not_the_record(s, "another constraint");
	if (con->kind == WS_CONSTRAINT_UNIQUE &&
		!same_constraint_settings(s, index_settings_of(s, con->name), in_statement->including, in_statement->options))
		not_the_record(s, "another constraint");
	if (con->kind == WS_CONSTRAINT_EXCLUSION)
	{
		const WsIndexSettings *set = index_settings_of(s, con->name);

		if (!same_options(set ? set->options : NULL, set ? set->noptions : 0, in_statement->options))
			not_the_record(s, "another constraint");
	}
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

			if (!same_index_key(s, key, key_options_of(index_settings_of(s, con->name), k - 1),
								linitial_node(IndexElem, pair)) ||
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
	if (!same_options(s->options, s->partition == WS_PARTITION_NONE ? s->noptions : 0, create->options))
		not_the_record(s, "other storage parameters");
	if (create->inhRelations != NIL || create->partbound != NULL ||
		create->ofTypename != NULL || create->constraints != NIL || create->nnconstraints != NIL ||
		create->oncommit != ONCOMMIT_NOOP ||
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
				list_length(key->keys) != s->nkey ||
				!same_constraint_settings(s, index_settings_of(s, s->key_name), key->including, key->options) ||
				key->indexspace != NULL || key->deferrable != s->key_deferrable ||
				key->initdeferred != s->key_initdeferred || key->without_overlaps)
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
	put_index_columns(s, ix, index_settings_of(s, ix->name), out);
	if (ix->nulls_not_distinct)
		ws_put_text(out, " NULLS NOT DISTINCT");
	put_constraint_settings(s, index_settings_of(s, ix->name), out);
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
	const WsIndexSettings *set = index_settings_of(s, ix->name);
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
		stmt->tableSpace != NULL || stmt->excludeOpNames != NIL ||
		!same_options(set ? set->options : NULL, set ? set->noptions : 0, stmt->options) ||
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
		if (!same_index_key(s, &ix->keys[k], key_options_of(index_settings_of(s, ix->name), k),
							lfirst_node(IndexElem, lc)))
			not_the_record(s, "another index");
		k++;
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

static void
write_column_settings(const WsTableSchema *s, const char *schema, Buf *out)
{
	bool		any = false;

	ws_put_text(out, "ALTER TABLE ONLY ");
	ws_put_identifier(out, schema);
	ws_put_text(out, ".");
	ws_put_identifier(out, s->name);
	for (int i = 0; i < s->ncols; i++)
	{
		const WsColumn *col = &s->cols[i];

		if (col->has_stattarget)
		{
			char		target[16];

			snprintf(target, sizeof(target), "%d", col->stattarget);
			ws_put_text(out, any ? ", ALTER " : " ALTER ");
			any = true;
			ws_put_identifier(out, col->name);
			ws_put_text(out, " SET STATISTICS ");
			ws_put_text(out, target);
		}
		if (col->noptions > 0)
		{
			ws_put_text(out, any ? ", ALTER " : " ALTER ");
			any = true;
			ws_put_identifier(out, col->name);
			ws_put_text(out, " SET ");
			put_options(col->options, col->noptions, out);
		}
	}
}

/* One ALTER TABLE ONLY of this table, of just these columns' settings. */
static void
check_column_settings_statement(const WsTableSchema *s, const char *schema, const char *sql)
{
	List	   *parsed = parse_text(s, sql);
	AlterTableStmt *stmt;
	ListCell   *lc;

	if (list_length(parsed) != 1 || !IsA(linitial_node(RawStmt, parsed)->stmt, AlterTableStmt))
		not_the_record(s, "it is not one ALTER TABLE");
	stmt = (AlterTableStmt *) linitial_node(RawStmt, parsed)->stmt;
	if (stmt->relation->catalogname != NULL || stmt->relation->schemaname == NULL ||
		strcmp(stmt->relation->schemaname, schema) != 0 ||
		strcmp(stmt->relation->relname, s->name) != 0 || stmt->relation->inh ||
		stmt->objtype != OBJECT_TABLE || stmt->missing_ok)
		not_the_record(s, "another table");
	lc = list_head(stmt->cmds);
	for (int i = 0; i < s->ncols; i++)
	{
		const WsColumn *col = &s->cols[i];

		for (int step = 0; step < 2; step++)
		{
			AlterTableCmd *cmd;

			if (step == 0 ? !col->has_stattarget : col->noptions == 0)
				continue;
			if (lc == NULL)
				not_the_record(s, "too few changes");
			cmd = lfirst_node(AlterTableCmd, lc);
			lc = lnext(stmt->cmds, lc);
			if (cmd->name == NULL || strcmp(cmd->name, col->name) != 0 || cmd->num != 0 ||
				cmd->newowner != NULL || cmd->missing_ok ||
				cmd->subtype != (step == 0 ? AT_SetStatistics : AT_SetOptions))
				not_the_record(s, "another change");
			if (step == 0)
			{
				if (cmd->def == NULL || !IsA(cmd->def, Integer) ||
					intVal(cmd->def) != col->stattarget)
					not_the_record(s, "another statistics target");
			}
			else if (cmd->def == NULL || !IsA(cmd->def, List) ||
					 !same_options(col->options, col->noptions, (List *) cmd->def))
				not_the_record(s, "other storage parameters");
		}
	}
	if (lc != NULL)
		not_the_record(s, "too many changes");
}

static void
write_table_settings(const WsTableSchema *s, const char *schema, Buf *out)
{
	ws_put_text(out, "ALTER TABLE ONLY ");
	ws_put_identifier(out, schema);
	ws_put_text(out, ".");
	ws_put_identifier(out, s->name);
	if (s->replica_identity != WS_REPLICA_DEFAULT)
	{
		ws_put_text(out, " REPLICA IDENTITY ");
		if (s->replica_identity == WS_REPLICA_FULL)
			ws_put_text(out, "FULL");
		else if (s->replica_identity == WS_REPLICA_NOTHING)
			ws_put_text(out, "NOTHING");
		else
		{
			ws_put_text(out, "USING INDEX ");
			ws_put_identifier(out, s->replica_index);
		}
	}
	if (s->clustered != NULL)
	{
		ws_put_text(out, s->replica_identity != WS_REPLICA_DEFAULT ? ", CLUSTER ON " : " CLUSTER ON ");
		ws_put_identifier(out, s->clustered);
	}
}

/* One ALTER TABLE ONLY of this table, of just its replica identity and clustered index. */
static void
check_table_settings_statement(const WsTableSchema *s, const char *schema, const char *sql)
{
	List	   *parsed = parse_text(s, sql);
	AlterTableStmt *stmt;
	int			want = (s->replica_identity != WS_REPLICA_DEFAULT ? 1 : 0) + (s->clustered != NULL ? 1 : 0);
	int			at = 0;

	if (list_length(parsed) != 1 || !IsA(linitial_node(RawStmt, parsed)->stmt, AlterTableStmt))
		not_the_record(s, "it is not one ALTER TABLE");
	stmt = (AlterTableStmt *) linitial_node(RawStmt, parsed)->stmt;
	if (stmt->relation->catalogname != NULL || stmt->relation->schemaname == NULL ||
		strcmp(stmt->relation->schemaname, schema) != 0 ||
		strcmp(stmt->relation->relname, s->name) != 0 || stmt->relation->inh ||
		stmt->objtype != OBJECT_TABLE || stmt->missing_ok || list_length(stmt->cmds) != want)
		not_the_record(s, "another table");
	if (s->replica_identity != WS_REPLICA_DEFAULT)
	{
		AlterTableCmd *cmd = list_nth_node(AlterTableCmd, stmt->cmds, at++);
		ReplicaIdentityStmt *ri;

		if (cmd->subtype != AT_ReplicaIdentity || !IsA(cmd->def, ReplicaIdentityStmt))
			not_the_record(s, "another change");
		ri = (ReplicaIdentityStmt *) cmd->def;
		if (ri->identity_type != (s->replica_identity == WS_REPLICA_FULL ? REPLICA_IDENTITY_FULL :
								  s->replica_identity == WS_REPLICA_NOTHING ? REPLICA_IDENTITY_NOTHING :
								  REPLICA_IDENTITY_INDEX) ||
			(ri->name != NULL) != (s->replica_index != NULL) ||
			(ri->name != NULL && strcmp(ri->name, s->replica_index) != 0))
			not_the_record(s, "another replica identity");
	}
	if (s->clustered != NULL)
	{
		AlterTableCmd *cmd = list_nth_node(AlterTableCmd, stmt->cmds, at);

		if (cmd->subtype != AT_ClusterOn || cmd->name == NULL || strcmp(cmd->name, s->clustered) != 0)
			not_the_record(s, "another clustered index");
	}
}

static void
write_comment(const WsTableSchema *s, const WsComment *m, const char *schema, Buf *out)
{
	const WsColumn *col = m->tag != 0 ? ws_schema_column(s, m->tag) : NULL;

	switch (m->kind)
	{
		case WS_COMMENT_TABLE:
			ws_put_text(out, "COMMENT ON TABLE ");
			break;
		case WS_COMMENT_COLUMN:
			ws_put_text(out, "COMMENT ON COLUMN ");
			break;
		case WS_COMMENT_SEQUENCE:
			ws_put_text(out, "COMMENT ON SEQUENCE ");
			break;
		case WS_COMMENT_INDEX:
			ws_put_text(out, "COMMENT ON INDEX ");
			break;
		default:
			ws_put_text(out, "COMMENT ON CONSTRAINT ");
			ws_put_identifier(out, m->name);
			ws_put_text(out, " ON ");
			break;
	}
	ws_put_identifier(out, schema);
	ws_put_text(out, ".");
	if (m->kind == WS_COMMENT_SEQUENCE)
		ws_put_identifier(out, col->identity_seq);
	else if (m->kind == WS_COMMENT_INDEX)
		ws_put_identifier(out, m->name);
	else
		ws_put_identifier(out, s->name);
	if (m->kind == WS_COMMENT_COLUMN)
	{
		ws_put_text(out, ".");
		ws_put_identifier(out, col->name);
	}
	ws_put_text(out, " IS ");
	ws_put_text(out, quote_literal_cstr(m->text));
}

/* One COMMENT, on just this, saying just this. */
static void
check_comment_statement(const WsTableSchema *s, const WsComment *m, const char *schema, const char *sql)
{
	List	   *parsed = parse_text(s, sql);
	const WsColumn *col = m->tag != 0 ? ws_schema_column(s, m->tag) : NULL;
	CommentStmt *stmt;
	List	   *want = list_make1(makeString((char *) schema));
	ObjectType	type;

	switch (m->kind)
	{
		case WS_COMMENT_TABLE:
			type = OBJECT_TABLE;
			want = lappend(want, makeString(s->name));
			break;
		case WS_COMMENT_COLUMN:
			type = OBJECT_COLUMN;
			want = lappend(want, makeString(s->name));
			want = lappend(want, makeString(col->name));
			break;
		case WS_COMMENT_SEQUENCE:
			type = OBJECT_SEQUENCE;
			want = lappend(want, makeString(col->identity_seq));
			break;
		case WS_COMMENT_INDEX:
			type = OBJECT_INDEX;
			want = lappend(want, makeString(m->name));
			break;
		default:
			type = OBJECT_TABCONSTRAINT;
			want = lappend(want, makeString(s->name));
			want = lappend(want, makeString(m->name));
			break;
	}
	if (list_length(parsed) != 1 || !IsA(linitial_node(RawStmt, parsed)->stmt, CommentStmt))
		not_the_record(s, "it is not one COMMENT");
	stmt = (CommentStmt *) linitial_node(RawStmt, parsed)->stmt;
	if (stmt->objtype != type || !equal(stmt->object, want) || stmt->comment == NULL ||
		strcmp(stmt->comment, m->text) != 0)
		not_the_record(s, "another comment");
}

const char *
ws_privilege_word(char letter)
{
	switch (letter)
	{
		case 'a':
			return "INSERT";
		case 'r':
			return "SELECT";
		case 'w':
			return "UPDATE";
		case 'd':
			return "DELETE";
		case 'D':
			return "TRUNCATE";
		case 'x':
			return "REFERENCES";
		case 't':
			return "TRIGGER";
		case 'm':
			return "MAINTAIN";
	}
	return "USAGE";
}

static void
write_grant(const WsTableSchema *s, const WsGrant *g, const char *schema, Buf *out)
{
	const WsColumn *col = g->tag != 0 ? ws_schema_column(s, g->tag) : NULL;

	ws_put_text(out, "GRANT ");
	for (const char *l = g->privileges; *l; l++)
	{
		if (l > g->privileges)
			ws_put_text(out, ", ");
		ws_put_text(out, ws_privilege_word(*l));
		if (g->target == WS_GRANT_COLUMN)
		{
			ws_put_text(out, " (");
			ws_put_identifier(out, col->name);
			ws_put_text(out, ")");
		}
	}
	ws_put_text(out, g->target == WS_GRANT_SEQUENCE ? " ON SEQUENCE " : " ON TABLE ");
	ws_put_identifier(out, schema);
	ws_put_text(out, ".");
	ws_put_identifier(out, g->target == WS_GRANT_SEQUENCE ? col->identity_seq : s->name);
	ws_put_text(out, " TO ");
	if (g->grantee == NULL)
		ws_put_text(out, "PUBLIC");
	else
		ws_put_identifier(out, g->grantee);
	if (g->grant_option)
		ws_put_text(out, " WITH GRANT OPTION");
}

/* One GRANT, of just these, on just this, to just this role. */
static void
check_grant_statement(const WsTableSchema *s, const WsGrant *g, const char *schema, const char *sql)
{
	List	   *parsed = parse_text(s, sql);
	const WsColumn *col = g->tag != 0 ? ws_schema_column(s, g->tag) : NULL;
	GrantStmt  *stmt;
	RangeVar   *on;
	RoleSpec   *to;
	ListCell   *lc;
	const char *l = g->privileges;

	if (list_length(parsed) != 1 || !IsA(linitial_node(RawStmt, parsed)->stmt, GrantStmt))
		not_the_record(s, "it is not one GRANT");
	stmt = (GrantStmt *) linitial_node(RawStmt, parsed)->stmt;
	if (!stmt->is_grant || stmt->targtype != ACL_TARGET_OBJECT || stmt->grantor != NULL ||
		stmt->objtype != (g->target == WS_GRANT_SEQUENCE ? OBJECT_SEQUENCE : OBJECT_TABLE) ||
		stmt->grant_option != g->grant_option ||
		list_length(stmt->objects) != 1 || list_length(stmt->grantees) != 1 ||
		list_length(stmt->privileges) != (int) strlen(g->privileges))
		not_the_record(s, "another grant");
	on = linitial_node(RangeVar, stmt->objects);
	if (on->catalogname != NULL || on->schemaname == NULL || strcmp(on->schemaname, schema) != 0 ||
		strcmp(on->relname, g->target == WS_GRANT_SEQUENCE ? col->identity_seq : s->name) != 0)
		not_the_record(s, "a grant on something else");
	to = linitial_node(RoleSpec, stmt->grantees);
	if (g->grantee == NULL ? to->roletype != ROLESPEC_PUBLIC :
		(to->roletype != ROLESPEC_CSTRING || strcmp(to->rolename, g->grantee) != 0))
		not_the_record(s, "a grant to someone else");
	foreach(lc, stmt->privileges)
	{
		AccessPriv *priv = lfirst_node(AccessPriv, lc);

		if (pg_strcasecmp(priv->priv_name, ws_privilege_word(*l)) != 0)
			not_the_record(s, "another grant");
		l++;
		if (g->target == WS_GRANT_COLUMN)
		{
			if (list_length(priv->cols) != 1 || strcmp(strVal(linitial(priv->cols)), col->name) != 0)
				not_the_record(s, "another grant");
		}
		else if (priv->cols != NIL)
			not_the_record(s, "another grant");
	}
}

/* One ALTER TABLE ONLY of this table, of just its owner. */
static void
check_owner_statement(const WsTableSchema *s, const char *schema, const char *sql)
{
	List	   *parsed = parse_text(s, sql);
	AlterTableStmt *stmt;
	AlterTableCmd *cmd;

	if (list_length(parsed) != 1 || !IsA(linitial_node(RawStmt, parsed)->stmt, AlterTableStmt))
		not_the_record(s, "it is not one ALTER TABLE");
	stmt = (AlterTableStmt *) linitial_node(RawStmt, parsed)->stmt;
	if (stmt->relation->catalogname != NULL || stmt->relation->schemaname == NULL ||
		strcmp(stmt->relation->schemaname, schema) != 0 ||
		strcmp(stmt->relation->relname, s->name) != 0 || stmt->relation->inh ||
		stmt->objtype != OBJECT_TABLE || stmt->missing_ok || list_length(stmt->cmds) != 1)
		not_the_record(s, "another table");
	cmd = linitial_node(AlterTableCmd, stmt->cmds);
	if (cmd->subtype != AT_ChangeOwner || cmd->newowner == NULL ||
		cmd->newowner->roletype != ROLESPEC_CSTRING || strcmp(cmd->newowner->rolename, s->owner) != 0)
		not_the_record(s, "another owner");
}

/* One ALTER TABLE ONLY of this table, of just its row security. */
static void
check_row_security_statement(const WsTableSchema *s, const char *schema, const char *sql)
{
	List	   *parsed = parse_text(s, sql);
	AlterTableStmt *stmt;
	int			want = (s->row_security ? 1 : 0) + (s->force_row_security ? 1 : 0);
	int			at = 0;

	if (list_length(parsed) != 1 || !IsA(linitial_node(RawStmt, parsed)->stmt, AlterTableStmt))
		not_the_record(s, "it is not one ALTER TABLE");
	stmt = (AlterTableStmt *) linitial_node(RawStmt, parsed)->stmt;
	if (stmt->relation->catalogname != NULL || stmt->relation->schemaname == NULL ||
		strcmp(stmt->relation->schemaname, schema) != 0 ||
		strcmp(stmt->relation->relname, s->name) != 0 || stmt->relation->inh ||
		stmt->objtype != OBJECT_TABLE || stmt->missing_ok || list_length(stmt->cmds) != want)
		not_the_record(s, "another table");
	if (s->row_security && list_nth_node(AlterTableCmd, stmt->cmds, at++)->subtype != AT_EnableRowSecurity)
		not_the_record(s, "another change");
	if (s->force_row_security && list_nth_node(AlterTableCmd, stmt->cmds, at)->subtype != AT_ForceRowSecurity)
		not_the_record(s, "another change");
}

static void
write_index_stats(const WsIndexSettings *set, const char *schema, Buf *out)
{
	ws_put_text(out, "ALTER INDEX ");
	ws_put_identifier(out, schema);
	ws_put_text(out, ".");
	ws_put_identifier(out, set->index);
	for (int k = 0; k < set->nstats; k++)
	{
		char		text[48];

		snprintf(text, sizeof(text), "%s ALTER COLUMN %d SET STATISTICS %d", k > 0 ? "," : "",
				 set->stats[k].column, set->stats[k].target);
		ws_put_text(out, text);
	}
}

/* One ALTER INDEX, of this index, of just these targets. */
static void
check_index_stats_statement(const WsTableSchema *s, const WsIndexSettings *set, const char *schema, const char *sql)
{
	List	   *parsed = parse_text(s, sql);
	AlterTableStmt *stmt;
	int			k = 0;
	ListCell   *lc;

	if (list_length(parsed) != 1 || !IsA(linitial_node(RawStmt, parsed)->stmt, AlterTableStmt))
		not_the_record(s, "it is not one ALTER INDEX");
	stmt = (AlterTableStmt *) linitial_node(RawStmt, parsed)->stmt;
	if (stmt->relation->catalogname != NULL || stmt->relation->schemaname == NULL ||
		strcmp(stmt->relation->schemaname, schema) != 0 ||
		strcmp(stmt->relation->relname, set->index) != 0 ||
		stmt->objtype != OBJECT_INDEX || stmt->missing_ok || list_length(stmt->cmds) != set->nstats)
		not_the_record(s, "another index");
	foreach(lc, stmt->cmds)
	{
		AlterTableCmd *cmd = lfirst_node(AlterTableCmd, lc);

		if (cmd->subtype != AT_SetStatistics || cmd->name != NULL || cmd->num != set->stats[k].column ||
			cmd->def == NULL || !IsA(cmd->def, Integer) || intVal(cmd->def) != set->stats[k].target)
			not_the_record(s, "another statistics target");
		k++;
	}
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

static WsOption *
options_in_server_encoding(const WsOption *options, int n)
{
	WsOption   *here = palloc_array(WsOption, Max(n, 1));

	for (int i = 0; i < n; i++)
	{
		here[i] = options[i];
		here[i].name = ws_name_from_utf8(options[i].name);
		here[i].value = from_utf8(options[i].value);
	}
	return here;
}

static WsIndexSettings *
index_settings_in_server_encoding(const WsTableSchema *s)
{
	WsIndexSettings *here = palloc_array(WsIndexSettings, Max(s->nindex_settings, 1));

	for (int i = 0; i < s->nindex_settings; i++)
	{
		const WsIndexSettings *set = &s->index_settings[i];

		here[i] = *set;
		here[i].index = ws_name_from_utf8(set->index);
		here[i].options = options_in_server_encoding(set->options, set->noptions);
		here[i].keyoptions = palloc_array(WsKeyOptions, Max(set->nkeyoptions, 1));
		for (int k = 0; k < set->nkeyoptions; k++)
		{
			here[i].keyoptions[k] = set->keyoptions[k];
			here[i].keyoptions[k].options = options_in_server_encoding(set->keyoptions[k].options,
																	   set->keyoptions[k].noptions);
		}
	}
	return here;
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
		here.cols[i].options = options_in_server_encoding(s->cols[i].options, s->cols[i].noptions);
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
	here.options = options_in_server_encoding(s->options, s->noptions);
	here.index_settings = index_settings_in_server_encoding(s);
	here.replica_index = ws_name_from_utf8(s->replica_index);
	here.clustered = ws_name_from_utf8(s->clustered);
	here.comments = palloc_array(WsComment, Max(s->ncomments, 1));
	for (int i = 0; i < s->ncomments; i++)
	{
		here.comments[i] = s->comments[i];
		here.comments[i].name = ws_name_from_utf8(s->comments[i].name);
		here.comments[i].text = from_utf8(s->comments[i].text);
	}
	here.owner = ws_name_from_utf8(s->owner);
	here.grants = palloc_array(WsGrant, Max(s->ngrants, 1));
	for (int i = 0; i < s->ngrants; i++)
	{
		here.grants[i] = s->grants[i];
		here.grants[i].grantee = ws_name_from_utf8(s->grants[i].grantee);
	}
	here.inherits = palloc_array(char *, Max(s->ninherits, 1));
	for (int i = 0; i < s->ninherits; i++)
		here.inherits[i] = ws_name_from_utf8(s->inherits[i]);
	here.inherited_cons = palloc_array(char *, Max(s->ninherited_cons, 1));
	for (int i = 0; i < s->ninherited_cons; i++)
		here.inherited_cons[i] = ws_name_from_utf8(s->inherited_cons[i]);
	*here_out = here;
}

/*
 * A statement's copy of the schema, and what parsing it takes, in a context of its
 * own: each is as big as the record, and a table has a statement per index.
 */
static MemoryContext
statement_begin(void)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "ws statement", ALLOCSET_DEFAULT_SIZES);

	MemoryContextSwitchTo(cxt);
	return cxt;
}

static void
statement_end(MemoryContext cxt, Buf *sql, Buf *out)
{
	MemoryContextSwitchTo(MemoryContextGetParent(cxt));
	buf_append(out, sql->data, sql->len - 1);
	MemoryContextDelete(cxt);
}

void
ws_schema_check_relation_defaults(const char *schema)
{
	Oid			nsp = get_namespace_oid(schema, true);
	Oid			role = GetUserId();

	if (SearchSysCacheExists3(DEFACLROLENSPOBJ, ObjectIdGetDatum(role), ObjectIdGetDatum(InvalidOid),
							  CharGetDatum(DEFACLOBJ_RELATION)) ||
		(OidIsValid(nsp) &&
		 SearchSysCacheExists3(DEFACLROLENSPOBJ, ObjectIdGetDatum(role), ObjectIdGetDatum(nsp),
							   CharGetDatum(DEFACLOBJ_RELATION))))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("default privileges would change what is made in schema \"%s\"", schema)));
}

/*
 * Whether default privileges would make what is made here other than the record
 * says, by the role about to make it.  What applies to a table always does, and to
 * sequences if it has an identity.  Looked up as making it will look them up, so
 * the two agree whatever snapshot this transaction holds.
 */
static void
check_destination(const WsTableSchema *s, const char *schema)
{
	Oid			nsp = get_namespace_oid(schema, true);
	Oid			role = GetUserId();
	bool		sequences = false;

	ws_schema_check_relation_defaults(schema);
	for (int i = 0; i < s->ncols; i++)
		sequences |= s->cols[i].identity != WS_IDENTITY_NONE;
	if (sequences &&
		(SearchSysCacheExists3(DEFACLROLENSPOBJ, ObjectIdGetDatum(role), ObjectIdGetDatum(InvalidOid),
							   CharGetDatum(DEFACLOBJ_SEQUENCE)) ||
		 (OidIsValid(nsp) &&
		  SearchSysCacheExists3(DEFACLROLENSPOBJ, ObjectIdGetDatum(role), ObjectIdGetDatum(nsp),
								CharGetDatum(DEFACLOBJ_SEQUENCE)))))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("default privileges would change what is made in schema \"%s\"", schema)));
}

void
ws_schema_create_table(const WsTableSchema *s, const char *schema, Buf *out)
{
	const char *problem = ws_schema_problem(s);
	WsTableSchema here;
	Buf			sql = BUF_INIT;
	MemoryContext cxt;

	if (problem != NULL)
		elog(ERROR, "%s", problem);
	check_destination(s, schema);
	cxt = statement_begin();
	schema_in_server_encoding(s, &here);
	write_create_table(&here, schema, &sql);
	buf_append_byte(&sql, 0);
	check_statement(&here, schema, (char *) sql.data);
	statement_end(cxt, &sql, out);
}

void
ws_schema_create_index(const WsTableSchema *s, int n, const char *schema, Buf *out)
{
	const char *problem = ws_schema_problem(s);
	WsTableSchema here;
	Buf			sql = BUF_INIT;
	MemoryContext cxt;

	if (problem != NULL)
		elog(ERROR, "%s", problem);
	if (n < 0 || n >= s->nindexes)
		elog(ERROR, "no index %d", n);
	cxt = statement_begin();
	schema_in_server_encoding(s, &here);
	write_create_index(&here, &here.indexes[n], schema, &sql);
	buf_append_byte(&sql, 0);
	check_index_statement(&here, &here.indexes[n], schema, (char *) sql.data);
	statement_end(cxt, &sql, out);
}

void
ws_schema_add_check(const WsTableSchema *s, int n, const char *schema, Buf *out)
{
	const char *problem = ws_schema_problem(s);
	WsTableSchema here;
	Buf			sql = BUF_INIT;
	MemoryContext cxt;

	if (problem != NULL)
		elog(ERROR, "%s", problem);
	if (n < 0 || n >= s->nconstraints || !s->constraints[n].not_valid)
		elog(ERROR, "no check not valid at %d", n);
	cxt = statement_begin();
	schema_in_server_encoding(s, &here);
	write_add_check(&here, &here.constraints[n], schema, &sql);
	buf_append_byte(&sql, 0);
	check_add_check_statement(&here, &here.constraints[n], schema, (char *) sql.data);
	statement_end(cxt, &sql, out);
}

void
ws_schema_attach(const WsTableSchema *s, const char *schema, Buf *out)
{
	const char *problem = ws_schema_problem(s);
	WsTableSchema here;
	Buf			sql = BUF_INIT;
	MemoryContext cxt;

	if (problem != NULL)
		elog(ERROR, "%s", problem);
	if (s->partition_of == NULL)
		elog(ERROR, "table \"%s\" is no partition", s->name);
	cxt = statement_begin();
	schema_in_server_encoding(s, &here);
	write_attach(&here, schema, &sql);
	buf_append_byte(&sql, 0);
	check_attach_statement(&here, schema, (char *) sql.data);
	statement_end(cxt, &sql, out);
}

void
ws_schema_column_settings(const WsTableSchema *s, const char *schema, Buf *out)
{
	const char *problem = ws_schema_problem(s);
	WsTableSchema here;
	Buf			sql = BUF_INIT;
	MemoryContext cxt;
	bool		any = false;

	if (problem != NULL)
		elog(ERROR, "%s", problem);
	for (int i = 0; i < s->ncols; i++)
		any |= s->cols[i].has_stattarget || s->cols[i].noptions > 0;
	if (!any)
		return;
	cxt = statement_begin();
	schema_in_server_encoding(s, &here);
	write_column_settings(&here, schema, &sql);
	buf_append_byte(&sql, 0);
	check_column_settings_statement(&here, schema, (char *) sql.data);
	statement_end(cxt, &sql, out);
}

void
ws_schema_table_settings(const WsTableSchema *s, const char *schema, Buf *out)
{
	const char *problem = ws_schema_problem(s);
	WsTableSchema here;
	Buf			sql = BUF_INIT;
	MemoryContext cxt;

	if (problem != NULL)
		elog(ERROR, "%s", problem);
	if (s->replica_identity == WS_REPLICA_DEFAULT && s->clustered == NULL)
		return;
	cxt = statement_begin();
	schema_in_server_encoding(s, &here);
	write_table_settings(&here, schema, &sql);
	buf_append_byte(&sql, 0);
	check_table_settings_statement(&here, schema, (char *) sql.data);
	statement_end(cxt, &sql, out);
}

void
ws_schema_comment(const WsTableSchema *s, int n, const char *schema, Buf *out)
{
	const char *problem = ws_schema_problem(s);
	WsTableSchema here;
	Buf			sql = BUF_INIT;
	MemoryContext cxt;

	if (problem != NULL)
		elog(ERROR, "%s", problem);
	if (n < 0 || n >= s->ncomments)
		elog(ERROR, "no comment %d", n);
	cxt = statement_begin();
	schema_in_server_encoding(s, &here);
	write_comment(&here, &here.comments[n], schema, &sql);
	buf_append_byte(&sql, 0);
	check_comment_statement(&here, &here.comments[n], schema, (char *) sql.data);
	statement_end(cxt, &sql, out);
}

/*
 * A record may name any role here.  One that cannot travel it could not have been
 * read with, so it came from somewhere it should not.
 */
static void
check_role_may_travel(const WsTableSchema *s, const char *role)
{
	char	   *what;

	if (IsReservedName(role))
		not_the_record(s, "a predefined role");
	what = psprintf("table \"%s\"", s->name);
	ws_role_check_name(what, role);
	pfree(what);
	if (ws_role_travel_problem(role) != NULL)
		not_the_record(s, "a role that can log in, or with powers no commit holds");
}

void
ws_schema_owner(const WsTableSchema *s, const char *schema, Buf *out)
{
	const char *problem = ws_schema_problem(s);
	WsTableSchema here;
	Buf			sql = BUF_INIT;
	MemoryContext cxt;

	if (problem != NULL)
		elog(ERROR, "%s", problem);
	if (s->owner == NULL)
		return;
	cxt = statement_begin();
	schema_in_server_encoding(s, &here);
	ws_put_text(&sql, "ALTER TABLE ONLY ");
	ws_put_identifier(&sql, schema);
	ws_put_text(&sql, ".");
	ws_put_identifier(&sql, here.name);
	check_role_may_travel(&here, here.owner);
	ws_put_text(&sql, " OWNER TO ");
	ws_put_identifier(&sql, here.owner);
	buf_append_byte(&sql, 0);
	check_owner_statement(&here, schema, (char *) sql.data);
	statement_end(cxt, &sql, out);
}

void
ws_schema_grant(const WsTableSchema *s, int n, const char *schema, Buf *out)
{
	const char *problem = ws_schema_problem(s);
	WsTableSchema here;
	Buf			sql = BUF_INIT;
	MemoryContext cxt;

	if (problem != NULL)
		elog(ERROR, "%s", problem);
	if (n < 0 || n >= s->ngrants)
		elog(ERROR, "no grant %d", n);
	cxt = statement_begin();
	schema_in_server_encoding(s, &here);
	if (here.grants[n].grantee != NULL)
	{
		check_role_may_travel(&here, here.grants[n].grantee);

		/*
		 * With no owner in the record, who restores owns it, and a grant to that
		 * role is one to its owner, which a read would not see again.
		 */
		if (here.owner == NULL &&
			strcmp(here.grants[n].grantee, GetUserNameFromId(GetUserId(), false)) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("table \"%s\" has a grant to role \"%s\", who would own it",
							here.name, here.grants[n].grantee)));
	}
	write_grant(&here, &here.grants[n], schema, &sql);
	buf_append_byte(&sql, 0);
	check_grant_statement(&here, &here.grants[n], schema, (char *) sql.data);
	statement_end(cxt, &sql, out);
}

void
ws_schema_row_security(const WsTableSchema *s, const char *schema, Buf *out)
{
	const char *problem = ws_schema_problem(s);
	WsTableSchema here;
	Buf			sql = BUF_INIT;
	MemoryContext cxt;

	if (problem != NULL)
		elog(ERROR, "%s", problem);
	if (!s->row_security && !s->force_row_security)
		return;
	cxt = statement_begin();
	schema_in_server_encoding(s, &here);
	ws_put_text(&sql, "ALTER TABLE ONLY ");
	ws_put_identifier(&sql, schema);
	ws_put_text(&sql, ".");
	ws_put_identifier(&sql, here.name);
	if (s->row_security)
		ws_put_text(&sql, " ENABLE ROW LEVEL SECURITY");
	if (s->force_row_security)
		ws_put_text(&sql, s->row_security ? ", FORCE ROW LEVEL SECURITY" : " FORCE ROW LEVEL SECURITY");
	buf_append_byte(&sql, 0);
	check_row_security_statement(&here, schema, (char *) sql.data);
	statement_end(cxt, &sql, out);
}

void
ws_schema_index_stats(const WsTableSchema *s, int n, const char *schema, Buf *out)
{
	const char *problem;
	WsTableSchema here;
	Buf			sql = BUF_INIT;
	MemoryContext cxt;

	/* (one of these per index: most have nothing to say, so ask that first) */
	if (n < 0 || n >= s->nindex_settings)
		elog(ERROR, "no index settings %d", n);
	if (s->index_settings[n].nstats == 0)
		return;
	problem = ws_schema_problem(s);
	if (problem != NULL)
		elog(ERROR, "%s", problem);
	cxt = statement_begin();
	schema_in_server_encoding(s, &here);
	write_index_stats(&here.index_settings[n], schema, &sql);
	buf_append_byte(&sql, 0);
	check_index_stats_statement(&here, &here.index_settings[n], schema, (char *) sql.data);
	statement_end(cxt, &sql, out);
}

void
ws_schema_inherit(const WsTableSchema *s, int n, const char *schema, Buf *out)
{
	const char *problem = ws_schema_problem(s);
	WsTableSchema here;
	Buf			sql = BUF_INIT;
	MemoryContext cxt;

	if (problem != NULL)
		elog(ERROR, "%s", problem);
	if (n < 0 || n >= s->ninherits)
		elog(ERROR, "no parent %d", n);
	cxt = statement_begin();
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
	statement_end(cxt, &sql, out);
}

static char *
to_utf8(const char *text)
{
	const char *u = pg_server_to_any(text, (int) strlen(text), PG_UTF8);

	return px_memdup(u, strlen(u) + 1);
}

char *
ws_text_to_utf8(const char *text)
{
	return to_utf8(text);
}

char *
ws_text_from_utf8(const char *text)
{
	return from_utf8(text);
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
"    AND p.refobjsubid > 0), "
"acls(grantor, grantee, kind, owner, priv) AS ( "
"  SELECT e.grantor, e.grantee, 't', q.relowner, e.privilege_type FROM pg_class q, aclexplode(q.relacl) e "
"    WHERE q.oid = $1::oid "
"  UNION ALL SELECT e.grantor, e.grantee, 'c', q.relowner, e.privilege_type FROM pg_class q "
"    JOIN pg_attribute a ON a.attrelid = q.oid, aclexplode(a.attacl) e "
"    WHERE q.oid = $1::oid AND a.attnum > 0 AND NOT a.attisdropped "
"  UNION ALL SELECT e.grantor, e.grantee, 's', q.relowner, e.privilege_type FROM pg_class q, aclexplode(q.relacl) e "
"    WHERE q.oid IN (SELECT o FROM own) AND q.relkind = 'S') "
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
"  WHEN c.relreplident = 'i' AND NOT EXISTS (SELECT FROM pg_index x WHERE x.indrelid = c.oid AND x.indisreplident) "
"    THEN 'its replica identity is by an index it no longer has' "
"  WHEN EXISTS (SELECT FROM pg_policy o WHERE o.polrelid = c.oid) THEN 'it has a policy' "
"  WHEN EXISTS (SELECT FROM pg_constraint k WHERE k.conrelid = c.oid AND "
"               k.contype NOT IN ('p', 'u', 'c', 'n', 'f', 'x')) "
"    THEN 'it has a constraint trigger' "
"  WHEN EXISTS (SELECT FROM pg_constraint k JOIN pg_index x ON x.indexrelid = k.conindid "
"               JOIN pg_class ic ON ic.oid = x.indexrelid "
"               WHERE k.conrelid = c.oid AND k.contype IN ('p', 'u') AND "
"                 (NOT x.indisvalid OR k.conperiod OR ic.reltablespace <> 0)) "
"    THEN 'a key or unique constraint is over a period, has a tablespace, or is not valid' "
"  WHEN EXISTS (SELECT FROM pg_constraint k JOIN pg_index x ON x.indexrelid = k.conindid "
"               JOIN pg_constraint k2 ON k2.conrelid = k.conrelid AND k2.oid > k.oid "
"               JOIN pg_index x2 ON x2.indexrelid = k2.conindid "
"               WHERE k.conrelid = c.oid AND k.contype IN ('p', 'u') AND k2.contype IN ('p', 'u') "
"                 AND x.indkey::text = x2.indkey::text AND x.indnkeyatts = x2.indnkeyatts "
"                 AND x.indnullsnotdistinct = x2.indnullsnotdistinct "
"                 AND k.condeferrable = k2.condeferrable AND k.condeferred = k2.condeferred) "
"    THEN 'it has two constraints alike, of which CREATE TABLE makes one' "
"  WHEN EXISTS (SELECT FROM pg_index x JOIN pg_class ic ON ic.oid = x.indexrelid "
"               WHERE x.indrelid = c.oid AND NOT EXISTS "
"               (SELECT FROM pg_constraint k WHERE k.conindid = x.indexrelid AND k.conrelid = c.oid "
"                  AND k.contype NOT IN ('x', 'f')) AND "
"                 (NOT x.indisvalid OR NOT x.indisready OR ic.reltablespace <> 0)) "
"    THEN 'an index has a tablespace, or is not valid' "
"  WHEN EXISTS (SELECT FROM pg_depend p WHERE p.classid = 'pg_class'::regclass "
"               AND p.objid IN (SELECT o FROM own) AND p.deptype = 'x') "
"    THEN 'it depends on an extension' "
"  WHEN EXISTS (SELECT FROM pg_description d WHERE d.classoid = 'pg_constraint'::regclass AND d.objoid IN "
"                (SELECT k.oid FROM pg_constraint k WHERE k.conrelid = c.oid AND k.contype = 'f')) "
"    THEN 'a foreign key has a comment' "
"  WHEN EXISTS (SELECT FROM pg_description d WHERE d.classoid = 'pg_type'::regclass AND "
"               (d.objoid = c.reltype OR d.objoid = (SELECT t.typarray FROM pg_type t WHERE t.oid = c.reltype))) "
"    THEN 'its row type has a comment' "
"  WHEN EXISTS (SELECT FROM pg_description d JOIN pg_index x ON x.indexrelid = d.objoid "
"               WHERE d.classoid = 'pg_class'::regclass AND x.indrelid = c.reltoastrelid) "
"    THEN 'its TOAST table has a comment' "
"  WHEN EXISTS (SELECT FROM pg_seclabel l WHERE l.classoid = 'pg_class'::regclass "
"               AND l.objoid IN (SELECT o FROM own)) THEN 'it has a security label' "
"  WHEN EXISTS (SELECT FROM pg_attribute a WHERE a.attrelid = c.oid AND a.attnum < 0 AND a.attacl IS NOT NULL) "
"    THEN 'a system column has a grant' "
"  WHEN EXISTS (SELECT FROM pg_class t WHERE t.oid = c.reltoastrelid AND t.relacl IS NOT NULL) "
"    THEN 'its TOAST table has a grant' "
"  WHEN EXISTS (SELECT FROM acls g WHERE g.grantee <> 0 AND NOT EXISTS (SELECT FROM pg_roles r WHERE r.oid = g.grantee)) "
"    THEN 'it has a grant to a role that does not exist' "
"  WHEN EXISTS (SELECT FROM acls g WHERE NOT ("
"      (g.kind = 't' AND g.priv IN ('INSERT', 'SELECT', 'UPDATE', 'DELETE', 'TRUNCATE', 'REFERENCES', 'TRIGGER', 'MAINTAIN')) OR "
"      (g.kind = 'c' AND g.priv IN ('INSERT', 'SELECT', 'UPDATE', 'REFERENCES')) OR "
"      (g.kind = 's' AND g.priv IN ('USAGE', 'SELECT', 'UPDATE')))) "
"    THEN 'it has a grant of a privilege a commit does not know' "
"  WHEN EXISTS (SELECT FROM acls g WHERE g.grantor <> g.owner) "
"    THEN 'it has a grant by someone other than its owner' "
"  WHEN EXISTS (SELECT FROM acls g JOIN pg_roles r ON r.oid = g.grantee WHERE g.grantee <> g.owner AND "
"               (r.rolcanlogin OR r.rolsuper OR r.rolcreaterole OR r.rolcreatedb OR r.rolreplication OR r.rolbypassrls "
"                OR r.rolname::text LIKE 'pg\\_%')) "
"    THEN 'it has a grant to a role that can log in, or with powers no commit holds: use a group role' "
"  WHEN EXISTS (SELECT FROM pg_roles r WHERE r.oid = c.relowner AND NOT r.rolcanlogin AND "
"               (r.rolsuper OR r.rolcreaterole OR r.rolcreatedb OR r.rolreplication OR r.rolbypassrls "
"                OR r.rolname::text LIKE 'pg\\_%')) "
"    THEN 'its owner has powers no commit holds' "
"  WHEN EXISTS (SELECT FROM pg_class q WHERE (q.oid = c.oid OR (q.oid IN (SELECT o FROM own) AND q.relkind = 'S')) "
"               AND q.relacl IS NOT NULL AND "
"               (SELECT array_agg(e.privilege_type ORDER BY e.privilege_type) "
"                  FROM aclexplode(q.relacl) e WHERE e.grantee = q.relowner) IS DISTINCT FROM "
"               (SELECT array_agg(e.privilege_type ORDER BY e.privilege_type) "
"                  FROM aclexplode(acldefault(CASE q.relkind WHEN 'S' THEN 's' ELSE 'r' END::\"char\", q.relowner)) e)) "
"    THEN 'its owner has other rights than owning gives' "
"  WHEN EXISTS (SELECT FROM acls g WHERE g.kind = 'c' AND g.grantee = g.owner) "
"    THEN 'its owner has other rights than owning gives' "
"  WHEN EXISTS (SELECT FROM pg_constraint k WHERE k.conrelid = c.oid AND k.contype = 'n' AND "
"               (NOT k.convalidated OR k.connoinherit)) "
"    THEN 'it has a not-null constraint not valid or no inherit' "
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
"WHERE c.oid = $1::oid";

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
	const char *schema;
	const char *table;
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

pg_noreturn static void
says_its_schema(const char *schema, const char *table)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("table \"%s\" in schema \"%s\" cannot be held in a commit: it must say its schema's name",
					table, schema),
			 errdetail("What it uses in its schema has a name the catalogs also have, which a record would have to tell apart by naming the schema.")));
}

/* (a commit does not name the schema it is of, so it can be restored into any) */
static void
check_says_no_schema(const ReadCols *rc, const char *expression)
{
	if (ws_expression_names_schema(expression, rc->schema))
		says_its_schema(rc->schema, rc->table);
}

/* Whether a name as the catalogs print it is qualified by a schema's name. */
static bool
name_says_schema(const char *name, const char *schema)
{
	const char *q = quote_identifier(schema);
	size_t		n = strlen(q);

	return name != NULL && (strncmp(name, q, n) == 0 && name[n] == '.');
}

static void
check_key_says_no_schema(const WsIndexKey *key, const char *schema, const char *table)
{
	char	   *op = key->op != NULL ? psprintf("%s", key->op) : NULL;

	if (name_says_schema(key->collation, schema) || name_says_schema(key->opclass, schema))
		says_its_schema(schema, table);
	if (op != NULL && strncmp(op, "OPERATOR(", 9) == 0 && name_says_schema(op + 9, schema))
		says_its_schema(schema, table);
	if (name_says_schema(op, schema))
		says_its_schema(schema, table);
}

/* The record's names that are not in an expression, which what read those has checked. */
static void
check_record_says_no_schema(const WsTableSchema *s, const char *schema, const char *table)
{
	for (int i = 0; i < s->ncols; i++)
		if (name_says_schema(s->cols[i].type, schema) || name_says_schema(s->cols[i].collation, schema))
			says_its_schema(schema, table);
	for (int i = 0; i < s->nconstraints; i++)
		for (int k = 0; k < s->constraints[i].excl.nkeys; k++)
			check_key_says_no_schema(&s->constraints[i].excl.keys[k], schema, table);
	for (int i = 0; i < s->nindexes; i++)
		for (int k = 0; k < s->indexes[i].nkeys; k++)
			check_key_says_no_schema(&s->indexes[i].keys[k], schema, table);
	for (int k = 0; k < s->npartition; k++)
		check_key_says_no_schema(&s->partition_keys[k], schema, table);
}

static char *
stored_expression(const char *expression, ReadCols *rc)
{
	Buf			stored = BUF_INIT;
	char	   *utf8;

	check_says_no_schema(rc, expression);
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
	if (elem->name != NULL)
		key->tag = index_col_tag(table, rc, elem->name);
	else
	{
		char		colno[16];
		const char *params[2] = {index_oid, colno};
		WsSqlRows	one;

		snprintf(colno, sizeof(colno), "%d", k + 1);
		WsSql->read("SELECT pg_get_indexdef($1::oid, $2::int, false)", 2, params, &one);
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
	WsSql->read("SELECT pg_get_constraintdef($1::oid)", 1, params, &printed);
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
	WsSql->read("SELECT pg_get_partkeydef($1::oid)", 1, params, &printed);
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
option_cmp(const void *a, const void *b)
{
	const WsOption *x = a;
	const WsOption *y = b;

	if (x->toast != y->toast)
		return x->toast ? 1 : -1;
	return strcmp(x->name, y->name);
}

/* name=value, as a catalog's options array holds each, each part in a commit's encoding. */
static void
read_option(const char *held, bool toast, WsOption *o)
{
	const char *eq = strchr(held, '=');

	if (eq == NULL)
		elog(ERROR, "an option with no value: \"%s\"", held);
	o->toast = toast;
	o->name = ws_name_to_utf8(pnstrdup(held, eq - held));
	o->value = to_utf8(eq + 1);
}

/* The table's storage parameters, its TOAST table's, and its columns'. */
static void
read_options(const char *table, const char *oid, const WsSqlRows *cols, WsTableSchema *out)
{
	const char *params[1] = {oid};
	WsSqlRows	rows;

	WsSql->read("SELECT o, false FROM pg_class c, unnest(c.reloptions) o WHERE c.oid = $1::oid "
			    "UNION ALL SELECT o, true FROM pg_class c JOIN pg_class t ON t.oid = c.reltoastrelid, "
			    "  unnest(t.reloptions) o WHERE c.oid = $1::oid", 1, params, &rows);
	/*
	 * A TOAST table outlives the need for one, and its parameters with it, where
	 * CREATE TABLE would make none and lose them: what decides is what it would.
	 * That looks at the table as it is now, so the columns must be as this
	 * snapshot saw them, under the lock, for its answer to be about them.
	 */
	for (int i = 0; i < rows.nrows; i++)
		if (ws_sql_value(&rows, i, 1)[0] == 't')
		{
			Relation	r = relation_open(atooid(oid), NoLock);
			TupleDesc	desc = RelationGetDescr(r);
			int			live = 0;
			bool		same = true;
			bool		needs;

			for (int a = 0; a < desc->natts; a++)
				live += TupleDescAttr(desc, a)->attisdropped ? 0 : 1;
			same = live == cols->nrows;
			for (int k = 0; k < cols->nrows && same; k++)
			{
				int			attnum = atoi(ws_sql_value(cols, k, 0));

				same = attnum <= desc->natts &&
					TupleDescAttr(desc, attnum - 1)->attstorage == ws_sql_value(cols, k, 17)[0] &&
					TupleDescAttr(desc, attnum - 1)->atttypmod == atoi(ws_sql_value(cols, k, 18));
			}
			needs = table_relation_needs_toast_table(r);
			relation_close(r, NoLock);
			if (!same)
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("a column of table \"%s\" was added, or its size or storage changed, since this transaction's snapshot",
								table)));
			if (!needs)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("the schema of table \"%s\" cannot be read yet: its TOAST table has parameters, and it no longer needs one",
								table)));
			break;
		}
	if (rows.nrows > 0)
	{
		out->options = px_malloc((size_t) rows.nrows * sizeof(WsOption));
		memset(out->options, 0, (size_t) rows.nrows * sizeof(WsOption));
		out->noptions = rows.nrows;
		for (int i = 0; i < rows.nrows; i++)
			read_option(ws_sql_value(&rows, i, 0), ws_sql_value(&rows, i, 1)[0] == 't', &out->options[i]);
		if (out->noptions > 1)
			qsort(out->options, out->noptions, sizeof(WsOption), option_cmp);
	}
	ws_sql_rows_free(&rows);
	WsSql->read("SELECT a.attnum::text, o FROM pg_attribute a, unnest(a.attoptions) o "
			    "WHERE a.attrelid = $1::oid AND a.attnum > 0 AND NOT a.attisdropped ORDER BY 1", 1, params, &rows);
	for (int r = 0; r < rows.nrows;)
	{
		int			same = 1;
		int			at = -1;

		while (r + same < rows.nrows &&
			   strcmp(ws_sql_value(&rows, r, 0), ws_sql_value(&rows, r + same, 0)) == 0)
			same++;
		for (int i = 0; i < cols->nrows && at < 0; i++)
			if (strcmp(ws_sql_value(cols, i, 0), ws_sql_value(&rows, r, 0)) == 0)
				at = i;
		if (at < 0)
			elog(ERROR, "options of a column the table does not have");
		out->cols[at].options = px_malloc((size_t) same * sizeof(WsOption));
		memset(out->cols[at].options, 0, (size_t) same * sizeof(WsOption));
		out->cols[at].noptions = same;
		for (int k = 0; k < same; k++)
			read_option(ws_sql_value(&rows, r + k, 1), false, &out->cols[at].options[k]);
		if (same > 1)
			qsort(out->cols[at].options, same, sizeof(WsOption), option_cmp);
		r += same;
	}
	ws_sql_rows_free(&rows);
}

static int
name_cmp(const void *a, const void *b)
{
	return strcmp(*(char *const *) a, *(char *const *) b);
}

static int
index_settings_cmp(const void *a, const void *b)
{
	return strcmp(((const WsIndexSettings *) a)->index, ((const WsIndexSettings *) b)->index);
}

/* An array of name=value, in rows: the first column's, from row at, n of them. */
static WsOption *
read_option_rows(const WsSqlRows *rows, int column, int at, int n)
{
	WsOption   *options = px_malloc((size_t) Max(n, 1) * sizeof(WsOption));

	memset(options, 0, (size_t) Max(n, 1) * sizeof(WsOption));
	for (int i = 0; i < n; i++)
		read_option(ws_sql_value(rows, at + i, column), false, &options[i]);
	if (n > 1)
		qsort(options, n, sizeof(WsOption), option_cmp);
	return options;
}

/*
 * Each index's settings, of any kind: a key's or unique constraint's INCLUDE, all
 * of their options, and their columns'.  A constraint's index has the constraint's
 * name, so the name is the one the record has it by.
 */
static void
read_index_settings(const char *table, const char *oid, const WsSqlRows *cols, WsTableSchema *out)
{
	const char *params[1] = {oid};
	WsSqlRows	idx;
	int			n = 0;

	WsSql->read("SELECT ic.relname::text, ic.oid::text, "
			    "  CASE WHEN EXISTS (SELECT FROM pg_constraint k WHERE k.conindid = i.indexrelid "
			    "      AND k.conrelid = i.indrelid AND k.contype IN ('p', 'u')) "
			    "    THEN i.indnatts - i.indnkeyatts ELSE 0 END::text, "
			    "  array_to_string((i.indkey::int2[])[i.indnkeyatts:i.indnatts - 1], ' ') "
			    "FROM pg_index i JOIN pg_class ic ON ic.oid = i.indexrelid "
			    "WHERE i.indrelid = $1::oid", 1, params, &idx);
	out->index_settings = px_malloc((size_t) Max(idx.nrows, 1) * sizeof(WsIndexSettings));
	memset(out->index_settings, 0, (size_t) Max(idx.nrows, 1) * sizeof(WsIndexSettings));
	for (int r = 0; r < idx.nrows; r++)
	{
		WsIndexSettings *set = &out->index_settings[n];
		const char *iparams[1] = {ws_sql_value(&idx, r, 1)};
		WsSqlRows	opts;
		WsSqlRows	atts;
		WsSqlRows	keyopts;

		set->ninclude = atoi(ws_sql_value(&idx, r, 2));
		if (set->ninclude > 0)
		{
			set->include = px_malloc((size_t) set->ninclude * sizeof(uint64));
			read_col_tags(table, cols, out, ws_sql_value(&idx, r, 3), set->ninclude, set->include);
		}
		WsSql->read("SELECT o FROM pg_class c, unnest(c.reloptions) o WHERE c.oid = $1::oid", 1, iparams, &opts);
		if (opts.nrows > 0)
		{
			set->options = read_option_rows(&opts, 0, 0, opts.nrows);
			set->noptions = opts.nrows;
		}
		ws_sql_rows_free(&opts);
		WsSql->read("SELECT a.attnum::text, a.attstattarget::text FROM pg_attribute a "
				    "WHERE a.attrelid = $1::oid AND a.attnum > 0 AND a.attstattarget IS NOT NULL "
				    "ORDER BY a.attnum", 1, iparams, &atts);
		if (atts.nrows > 0)
		{
			set->stats = px_malloc((size_t) atts.nrows * sizeof(WsIndexStat));
			set->nstats = atts.nrows;
			for (int k = 0; k < atts.nrows; k++)
			{
				set->stats[k].column = atoi(ws_sql_value(&atts, k, 0));
				set->stats[k].target = atoi(ws_sql_value(&atts, k, 1));
			}
		}
		ws_sql_rows_free(&atts);
		WsSql->read("SELECT a.attnum, o FROM pg_attribute a, unnest(a.attoptions) o "
				    "WHERE a.attrelid = $1::oid AND a.attnum > 0 ORDER BY a.attnum", 1, iparams, &keyopts);
		if (keyopts.nrows > 0)
		{
			set->keyoptions = px_malloc((size_t) keyopts.nrows * sizeof(WsKeyOptions));
			memset(set->keyoptions, 0, (size_t) keyopts.nrows * sizeof(WsKeyOptions));
			for (int at = 0; at < keyopts.nrows;)
			{
				int			same = 1;

				while (at + same < keyopts.nrows &&
					   strcmp(ws_sql_value(&keyopts, at, 0), ws_sql_value(&keyopts, at + same, 0)) == 0)
					same++;
				set->keyoptions[set->nkeyoptions].column = atoi(ws_sql_value(&keyopts, at, 0));
				set->keyoptions[set->nkeyoptions].options = read_option_rows(&keyopts, 1, at, same);
				set->keyoptions[set->nkeyoptions].noptions = same;
				set->nkeyoptions++;
				at += same;
			}
		}
		ws_sql_rows_free(&keyopts);
		if (set->ninclude > 0 || set->noptions > 0 || set->nstats > 0 || set->nkeyoptions > 0)
		{
			set->index = ws_name_to_utf8(ws_sql_value(&idx, r, 0));
			n++;
		}
		else
			memset(set, 0, sizeof(*set));
	}
	out->nindex_settings = n;
	if (n > 1)
		qsort(out->index_settings, n, sizeof(WsIndexSettings), index_settings_cmp);
	ws_sql_rows_free(&idx);
}

static int
comment_cmp(const void *a, const void *b)
{
	const WsComment *x = a;
	const WsComment *y = b;

	if (x->kind != y->kind)
		return (unsigned char) x->kind < (unsigned char) y->kind ? -1 : 1;
	if (x->tag != y->tag)
		return x->tag < y->tag ? -1 : 1;
	return x->name != NULL && y->name != NULL ? strcmp(x->name, y->name) : 0;
}

/*
 * The comments on what the record holds: those on a foreign key, a row type or a
 * TOAST table are refused above, and those on triggers and the like go with those
 * objects, which are not the table's.
 */
static void
read_comments(const char *oid, const WsSqlRows *cols, WsTableSchema *out)
{
	const char *params[1] = {oid};
	WsSqlRows	rows;

	WsSql->read("SELECT 't', NULL::text, d.description FROM pg_description d "
			    "  WHERE d.classoid = 'pg_class'::regclass AND d.objoid = $1::oid AND d.objsubid = 0 "
			    "UNION ALL SELECT 'c', d.objsubid::text, d.description FROM pg_description d "
			    "  WHERE d.classoid = 'pg_class'::regclass AND d.objoid = $1::oid AND d.objsubid > 0 "
			    "UNION ALL SELECT 'i', q.relname::text, d.description FROM pg_description d "
			    "  JOIN pg_index x ON x.indexrelid = d.objoid JOIN pg_class q ON q.oid = x.indexrelid "
			    "  WHERE d.classoid = 'pg_class'::regclass AND x.indrelid = $1::oid "
			    "UNION ALL SELECT 's', p.refobjsubid::text, d.description FROM pg_description d "
			    "  JOIN pg_depend p ON p.objid = d.objoid AND p.classid = 'pg_class'::regclass "
			    "    AND p.refclassid = 'pg_class'::regclass AND p.deptype = 'i' "
			    "  JOIN pg_class q ON q.oid = p.objid AND q.relkind = 'S' "
			    "  WHERE d.classoid = 'pg_class'::regclass AND p.refobjid = $1::oid "
			    "UNION ALL SELECT 'k', k.conname::text, d.description FROM pg_description d "
			    "  JOIN pg_constraint k ON k.oid = d.objoid "
			    "  WHERE d.classoid = 'pg_constraint'::regclass AND k.conrelid = $1::oid", 1, params, &rows);
	if (rows.nrows > 0)
	{
		out->comments = px_malloc((size_t) rows.nrows * sizeof(WsComment));
		memset(out->comments, 0, (size_t) rows.nrows * sizeof(WsComment));
		out->ncomments = rows.nrows;
		for (int i = 0; i < rows.nrows; i++)
		{
			WsComment  *m = &out->comments[i];
			const char *target = ws_sql_value(&rows, i, 1);

			m->kind = ws_sql_value(&rows, i, 0)[0];
			m->text = to_utf8(ws_sql_value(&rows, i, 2));
			if (m->kind == WS_COMMENT_COLUMN || m->kind == WS_COMMENT_SEQUENCE)
			{
				for (int k = 0; k < cols->nrows && m->tag == 0; k++)
					if (strcmp(ws_sql_value(cols, k, 0), target) == 0)
						m->tag = out->cols[k].tag;
				if (m->tag == 0)
					elog(ERROR, "a comment on a column the table does not have");
			}
			else if (m->kind != WS_COMMENT_TABLE)
				m->name = ws_name_to_utf8(target);
		}
		if (out->ncomments > 1)
			qsort(out->comments, out->ncomments, sizeof(WsComment), comment_cmp);
	}
	ws_sql_rows_free(&rows);
}

/* A role's name as a commit holds it. */
static char *
role_name_to_utf8(const char *table, const char *role)
{
	char	   *what = psprintf("table \"%s\"", table);

	ws_role_check_name(what, role);
	pfree(what);
	return ws_name_to_utf8(role);
}

static int
grant_cmp(const void *a, const void *b)
{
	const WsGrant *x = a;
	const WsGrant *y = b;

	if (x->target != y->target)
		return (unsigned char) x->target < (unsigned char) y->target ? -1 : 1;
	if (x->tag != y->tag)
		return x->tag < y->tag ? -1 : 1;
	if ((x->grantee == NULL) != (y->grantee == NULL))
		return x->grantee == NULL ? -1 : 1;
	if (x->grantee != NULL && strcmp(x->grantee, y->grantee) != 0)
		return strcmp(x->grantee, y->grantee);
	return (int) x->grant_option - (int) y->grant_option;
}

/* Its owner, if the record holds it, and what it has granted, all refused above if not by it. */
static void
read_grants(const char *table, const char *oid, const WsSqlRows *cols, WsTableSchema *out)
{
	const char *params[1] = {oid};
	WsSqlRows	rows;

	WsSql->read("SELECT r.rolname::text FROM pg_class c JOIN pg_roles r ON r.oid = c.relowner "
			    "WHERE c.oid = $1::oid AND NOT r.rolcanlogin", 1, params, &rows);
	if (rows.nrows == 1)
		out->owner = role_name_to_utf8(table, ws_sql_value(&rows, 0, 0));
	ws_sql_rows_free(&rows);
	WsSql->read("WITH g(target, attnum, grantee, owner, is_grantable, privilege_type) AS ( "
			    "  SELECT 't', NULL::int2, e.grantee, c.relowner, e.is_grantable, e.privilege_type "
			    "    FROM pg_class c, aclexplode(c.relacl) e WHERE c.oid = $1::oid "
			    "  UNION ALL SELECT 'c', a.attnum, e.grantee, c.relowner, e.is_grantable, e.privilege_type "
			    "    FROM pg_class c JOIN pg_attribute a ON a.attrelid = c.oid, aclexplode(a.attacl) e "
			    "    WHERE c.oid = $1::oid AND a.attnum > 0 AND NOT a.attisdropped "
			    "  UNION ALL SELECT 's', p.refobjsubid::int2, e.grantee, q.relowner, e.is_grantable, e.privilege_type "
			    "    FROM pg_depend p JOIN pg_class q ON q.oid = p.objid, aclexplode(q.relacl) e "
			    "    WHERE p.classid = 'pg_class'::regclass AND p.refclassid = 'pg_class'::regclass "
			    "      AND p.refobjid = $1::oid AND p.deptype = 'i' AND q.relkind = 'S') "
			    "SELECT g.target, g.attnum::text, (SELECT r.rolname::text FROM pg_roles r WHERE r.oid = g.grantee), "
			    "  g.is_grantable::text, string_agg(CASE g.privilege_type WHEN 'INSERT' THEN 'a' WHEN 'SELECT' THEN 'r' "
			    "    WHEN 'UPDATE' THEN 'w' WHEN 'DELETE' THEN 'd' WHEN 'TRUNCATE' THEN 'D' WHEN 'REFERENCES' THEN 'x' "
			    "    WHEN 'TRIGGER' THEN 't' WHEN 'MAINTAIN' THEN 'm' WHEN 'USAGE' THEN 'U' END, '') "
			    "FROM g WHERE g.grantee <> g.owner GROUP BY 1, 2, 3, 4", 1, params, &rows);
	if (rows.nrows > 0)
	{
		out->grants = px_malloc((size_t) rows.nrows * sizeof(WsGrant));
		memset(out->grants, 0, (size_t) rows.nrows * sizeof(WsGrant));
		out->ngrants = rows.nrows;
		for (int i = 0; i < rows.nrows; i++)
		{
			WsGrant    *g = &out->grants[i];
			char	   *letters = px_memdup(ws_sql_value(&rows, i, 4), strlen(ws_sql_value(&rows, i, 4)) + 1);
			int			n = (int) strlen(letters);

			g->target = ws_sql_value(&rows, i, 0)[0];
			if (g->target != WS_GRANT_TABLE)
			{
				for (int k = 0; k < cols->nrows && g->tag == 0; k++)
					if (strcmp(ws_sql_value(cols, k, 0), ws_sql_value(&rows, i, 1)) == 0)
						g->tag = out->cols[k].tag;
				if (g->tag == 0)
					elog(ERROR, "a grant on a column the table does not have");
			}
			g->grantee = ws_sql_value(&rows, i, 2) ? role_name_to_utf8(table, ws_sql_value(&rows, i, 2)) : NULL;
			g->grant_option = ws_sql_value(&rows, i, 3)[0] == 't';
			/* (in order by byte, whatever order aclexplode gave them in) */
			for (int a = 1; a < n; a++)
				for (int b = a; b > 0 && (unsigned char) letters[b - 1] > (unsigned char) letters[b]; b--)
				{
					char		swap = letters[b];

					letters[b] = letters[b - 1];
					letters[b - 1] = swap;
				}
			g->privileges = letters;
		}
		if (out->ngrants > 1)
			qsort(out->grants, out->ngrants, sizeof(WsGrant), grant_cmp);
	}
	ws_sql_rows_free(&rows);
}

/* Its row security, replica identity and clustered index. */
static void
read_table_settings(const char *table, const char *oid, WsTableSchema *out)
{
	const char *params[1] = {oid};
	WsSqlRows	rows;

	WsSql->read("SELECT c.relreplident::text, "
			    "  c.relrowsecurity::text, c.relforcerowsecurity::text, "
			    "  (SELECT ic.relname::text FROM pg_index i JOIN pg_class ic ON ic.oid = i.indexrelid "
			    "    WHERE i.indrelid = c.oid AND i.indisreplident), "
			    "  (SELECT ic.relname::text FROM pg_index i JOIN pg_class ic ON ic.oid = i.indexrelid "
			    "    WHERE i.indrelid = c.oid AND i.indisclustered) "
			    "FROM pg_class c WHERE c.oid = $1::oid", 1, params, &rows);
	out->row_security = ws_sql_value(&rows, 0, 1)[0] == 't';
	out->force_row_security = ws_sql_value(&rows, 0, 2)[0] == 't';
	switch (ws_sql_value(&rows, 0, 0)[0])
	{
		case REPLICA_IDENTITY_DEFAULT:
			break;
		case REPLICA_IDENTITY_FULL:
			out->replica_identity = WS_REPLICA_FULL;
			break;
		case REPLICA_IDENTITY_NOTHING:
			out->replica_identity = WS_REPLICA_NOTHING;
			break;
		case REPLICA_IDENTITY_INDEX:
			if (ws_sql_value(&rows, 0, 3) == NULL)
				elog(ERROR, "table \"%s\" has a replica identity by no index", table);
			out->replica_identity = WS_REPLICA_INDEX;
			out->replica_index = ws_name_to_utf8(ws_sql_value(&rows, 0, 3));
			break;
		default:
			elog(ERROR, "table \"%s\" has a replica identity of no known kind", table);
	}
	if (ws_sql_value(&rows, 0, 4) != NULL)
		out->clustered = ws_name_to_utf8(ws_sql_value(&rows, 0, 4));
	ws_sql_rows_free(&rows);
}

/* The tables a table inherits from, and what it has only by doing so. */
static void
read_inheritance(const char *oid, const WsSqlRows *cols, const WsSqlRows *others, WsTableSchema *out)
{
	const char *params[1] = {oid};
	WsSqlRows	parents;
	int			n = 0;

	WsSql->read("SELECT q.relname::text FROM pg_inherits i JOIN pg_class q ON q.oid = i.inhparent "
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
	int			nest = ws_schema_path_begin(schema);
	WsSqlRows	rel,
				cols,
				key,
				others,
				idxs;
	ReadCols	rc = {.schema = schema,.table = table};
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
		ws_schema_path_end(nest);
		WsSql->end(level);
		return false;
	}
	relkind = ws_sql_value(&rel, 0, 1);
	persistence = ws_sql_value(&rel, 0, 2);
	if (relkind[0] != RELKIND_RELATION && relkind[0] != RELKIND_PARTITIONED_TABLE)
	{
		ws_sql_rows_free(&rel);
		ws_schema_path_end(nest);
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

	/*
	 * All the rest under one snapshot, taken now the lock is held, as that lock
	 * keeps out only some of what can change a table's record: another
	 * transaction's comment, or an index renamed, between one read and the next.
	 */
	PushCopiedSnapshot(GetTransactionSnapshot());
	UpdateActiveSnapshotCommandId();
	{
		const char *oid[1] = {ws_sql_value(&rel, 0, 0)};
		WsSqlRows	why;

		WsSql->read(unsupported_sql, 1, oid, &why);
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

		WsSql->read("SELECT a.attnum::text, a.attname::text, "
				    "  format_type(a.atttypid, a.atttypmod), a.attnotnull::text, "
				    "  a.attidentity::text, a.attgenerated::text, "
				    "  pg_get_expr(d.adbin, d.adrelid), "
				    "  CASE WHEN a.attcollation <> t.typcollation THEN "
				    "    CASE WHEN co.collnamespace = (SELECT relnamespace FROM pg_class WHERE oid = a.attrelid) "
				    "        AND pg_collation_is_visible(co.oid) THEN quote_ident(co.collname) "
				    "      ELSE quote_ident(cn.nspname) || '.' || quote_ident(co.collname) END END, "
				    "  a.attcoltag::text, "
				    "  (SELECT k.conname::text FROM pg_constraint k WHERE k.conrelid = a.attrelid "
				    "    AND k.contype = 'n' AND k.conkey[1] = a.attnum), "
				    "  (SELECT q.relname::text FROM pg_depend p JOIN pg_class q ON q.oid = p.objid "
				    "    WHERE p.classid = 'pg_class'::regclass AND p.refclassid = 'pg_class'::regclass "
				    "      AND p.refobjid = a.attrelid AND p.refobjsubid = a.attnum "
				    "      AND p.deptype = 'i' AND q.relkind = 'S'), "
				    "  a.atttypid::text, a.attislocal::text, "
				    "  (SELECT k.conislocal::text FROM pg_constraint k WHERE k.conrelid = a.attrelid "
				    "    AND k.contype = 'n' AND k.conkey[1] = a.attnum), "
				    "  CASE WHEN a.attstorage <> t.typstorage THEN a.attstorage::text END, "
				    "  a.attcompression::text, a.attstattarget::text, a.attstorage::text, "
				    "  a.atttypmod::text "
				    "FROM pg_attribute a "
				    "JOIN pg_type t ON t.oid = a.atttypid "
				    "LEFT JOIN pg_attrdef d ON d.adrelid = a.attrelid AND d.adnum = a.attnum "
				    "LEFT JOIN pg_collation co ON co.oid = a.attcollation "
				    "LEFT JOIN pg_namespace cn ON cn.oid = co.collnamespace "
				    "WHERE a.attrelid = $1::oid AND a.attnum > 0 AND NOT a.attisdropped "
				    "ORDER BY a.attnum", 1, oid, &cols);
		WsSql->read("SELECT con.conname::text, con.contype::text, pg_get_expr(con.conbin, con.conrelid), "
				    "  array_to_string((i.indkey::int2[])[0:i.indnkeyatts - 1], ' '), "
				    "  i.indnullsnotdistinct::text, con.condeferrable::text, con.condeferred::text, "
				    "  con.oid::text, "
				    "  i.indexrelid::text, pg_get_expr(i.indpred, i.indrelid), "
				    "  con.convalidated::text, con.connoinherit::text, con.conenforced::text, "
				    "  con.conislocal::text "
				    "FROM pg_constraint con LEFT JOIN pg_index i ON i.indexrelid = con.conindid "
				    "WHERE con.conrelid = $1::oid AND con.contype IN ('c', 'u', 'x')", 1, oid, &others);
		WsSql->read("SELECT ic.relname::text, i.indexrelid::text, pg_get_indexdef(i.indexrelid), "
				    "  pg_get_expr(i.indpred, i.indrelid) "
				    "FROM pg_index i JOIN pg_class ic ON ic.oid = i.indexrelid "
				    "WHERE i.indrelid = $1::oid AND NOT EXISTS (SELECT FROM pg_constraint k "
				    "  WHERE k.conindid = i.indexrelid AND k.conrelid = i.indrelid "
				    "    AND k.contype <> 'f')",
				    1, oid, &idxs);
		WsSql->read("SELECT con.conname::text, i.indnkeyatts::text, i.indnatts::text, "
				    "  con.condeferrable::text, "
				    "  array_to_string((i.indkey::int2[])[0:i.indnkeyatts - 1], ' '), "
				    "  con.condeferred::text "
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
		c->storage = ws_sql_value(&cols, i, 14) ? ws_sql_value(&cols, i, 14)[0] : WS_STORAGE_DEFAULT;
		c->compression = ws_sql_value(&cols, i, 15)[0];
		if (ws_sql_value(&cols, i, 16) != NULL)
		{
			c->has_stattarget = true;
			c->stattarget = atoi(ws_sql_value(&cols, i, 16));
		}
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

			check_says_no_schema(&rc, expr);
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
		out->key_deferrable = ws_sql_value(&key, 0, 3)[0] == 't';
		out->key_initdeferred = ws_sql_value(&key, 0, 5)[0] == 't';
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

			check_says_no_schema(&rc, ws_sql_value(&others, i, 2));
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
	out->nindexes = idxs.nrows;
	out->indexes = px_malloc((size_t) Max(idxs.nrows, 1) * sizeof(WsIndex));
	memset(out->indexes, 0, (size_t) Max(idxs.nrows, 1) * sizeof(WsIndex));
	for (int i = 0; i < idxs.nrows; i++)
		read_index(table, &idxs, i, &rc, &out->indexes[i]);
	if (out->nindexes > 1)
		qsort(out->indexes, out->nindexes, sizeof(WsIndex), index_cmp);
	if (relkind[0] == RELKIND_PARTITIONED_TABLE)
		read_partitioning(table, ws_sql_value(&rel, 0, 0), &rc, out);
	INJECTION_POINT("ws-schema-read-between", NULL);
	read_options(table, ws_sql_value(&rel, 0, 0), &cols, out);
	read_index_settings(table, ws_sql_value(&rel, 0, 0), &cols, out);
	read_table_settings(table, ws_sql_value(&rel, 0, 0), out);
	read_comments(ws_sql_value(&rel, 0, 0), &cols, out);
	read_grants(table, ws_sql_value(&rel, 0, 0), &cols, out);
	/* (their keys' parameters, in the settings, tell them apart) */
	for (int i = 0; i < out->nconstraints; i++)
		for (int j = 0; j < i; j++)
			if (out->constraints[i].kind == WS_CONSTRAINT_EXCLUSION &&
				out->constraints[j].kind == WS_CONSTRAINT_EXCLUSION &&
				exclusions_alike(out, &out->constraints[i], &out->constraints[j]))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("the schema of table \"%s\" cannot be read yet: it has two constraints alike, of which CREATE TABLE makes one",
								table)));
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
	PopActiveSnapshot();
	check_record_says_no_schema(out, schema, table);
	ws_schema_path_end(nest);
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
