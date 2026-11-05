/*-------------------------------------------------------------------------
 *
 * ws_viol.c
 *	  Constraint violations: rows of a merged root that its constraints refuse.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/ws/ws_viol.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/stratnum.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type_d.h"
#include "commands/defrem.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "parser/parse_type.h"
#include "prolly/ws/ws_fk.h"
#include "prolly/ws/ws_restore.h"
#include "prolly/ws/ws_root.h"
#include "prolly/ws/ws_schema.h"
#include "prolly/ws/ws_sql.h"
#include "prolly/ws/ws_viol.h"
#include "utils/builtins.h"
#include "utils/errcodes.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/ruleutils.h"
#include "utils/syscache.h"

/* What was found; in a context of its own, as what finds it is undone. */
typedef struct Found
{
	MemoryContext cxt;
	WsViolation *v;
	int			n;
	int			cap;
} Found;

static void
found_add(Found *f, const char *table, const char *constraint, const char *kind, const char *rowkey)
{
	MemoryContext old = MemoryContextSwitchTo(f->cxt);

	if (f->n == f->cap)
	{
		f->cap = Max(32, f->cap * 2);
		f->v = f->v == NULL ? palloc_array(WsViolation, f->cap) : repalloc_array(f->v, WsViolation, f->cap);
	}
	f->v[f->n].table = pstrdup(table);
	f->v[f->n].constraint = pstrdup(constraint);
	f->v[f->n].kind = pstrdup(kind);
	f->v[f->n].rowkey = rowkey != NULL ? pstrdup(rowkey) : NULL;
	f->n++;
	MemoryContextSwitchTo(old);
}

/* ---------------------------------------------------------------- the queries */

/* A column of a record, quoted, as a query in this database names it. */
static const char *
column_sql(const WsTableSchema *s, uint64 tag)
{
	const WsColumn *col = ws_schema_column(s, tag);

	if (col == NULL)
		elog(ERROR, "a constraint of table \"%s\" names a column it does not have", s->name);
	return quote_identifier(ws_name_from_utf8(col->name));
}

/*
 * What says a row of table x: its key's values as a conflict prints them, or all of it,
 * and no longer than a key can be, whatever its row holds.
 */
static char *
rowkey_sql(const WsTableSchema *s)
{
	StringInfoData text;
	char	   *out;

	initStringInfo(&text);
	if (s->nkey == 0)
		appendStringInfoString(&text, "(x.*)::text");
	else
	{
		appendStringInfoString(&text, "concat_ws(',', ");
		for (int i = 0; i < s->nkey; i++)
			appendStringInfo(&text, "%sx.%s::text", i > 0 ? ", " : "", column_sql(s, s->key[i]));
		appendStringInfoChar(&text, ')');
	}
	out = psprintf("CASE WHEN length(%s) <= 200 THEN %s "
				   "ELSE left(%s, 200) || ' ' || encode(sha256(convert_to(%s, 'UTF8')), 'hex') END",
				   text.data, text.data, text.data, text.data);
	pfree(text.data);
	return out;
}

/* A table as a query reads it: its own rows, or a partitioned table's partitions'. */
static char *
table_sql(const WsTableSchema *s, const char *schema)
{
	return psprintf("%s%s.%s", s->partition != WS_PARTITION_NONE ? "" : "ONLY ",
					quote_identifier(schema), quote_identifier(ws_name_from_utf8(s->name)));
}

static char *
expression_sql(const WsTableSchema *s, const char *stored)
{
	Buf			out = BUF_INIT;

	ws_schema_render_expression(s, stored, &out);
	buf_append_byte(&out, 0);
	return ws_text_from_utf8((const char *) out.data);
}

/* Each row a query of one column gives, a violation; the query's failure raised. */
static void
rows_violating(Found *f, const char *sql, const WsTableSchema *s, const char *constraint, const char *kind)
{
	WsSqlRows	rows;

	CHECK_FOR_INTERRUPTS();
	WsSql->run(sql, 0, NULL, &rows);
	for (int i = 0; i < rows.nrows; i++)
		found_add(f, s->name, constraint, kind, ws_text_to_utf8(ws_sql_value(&rows, i, 0)));
	ws_sql_rows_free(&rows);
}

/*
 * Rows sharing what a list of keys say, and those not null unless nulls are alike: a
 * composite value is one, whatever its fields, as an index holds it, and as IS NULL does
 * not say.
 */
static void
unique_violating(Found *f, const WsTableSchema *s, const char *schema, const char *constraint, const char *keys,
				 const char *not_null, const char *predicate)
{
	StringInfoData where;

	initStringInfo(&where);
	if (not_null != NULL)
		appendStringInfoString(&where, not_null);
	if (predicate != NULL)
		appendStringInfo(&where, "%s(%s)", where.len > 0 ? " AND " : "", predicate);
	rows_violating(f, psprintf("SELECT r FROM (SELECT %s AS r, count(*) OVER (PARTITION BY %s) AS c FROM %s x%s%s) s WHERE c > 1",
							   rowkey_sql(s), keys, table_sql(s, schema), where.len > 0 ? " WHERE " : "", where.data),
				   s, constraint, "unique");
	pfree(where.data);
}

/* An exclusion constraint, added: only Postgres can apply it, and it says no rows. */
static void
exclusion_violated(Found *f, const WsTableSchema *s, int n, const char *schema)
{
	Buf			sql = BUF_INIT;
	WsSqlRows	rows;
	WsSqlFailure failure;

	ws_schema_add_exclusion(s, n, schema, &sql);
	buf_append_byte(&sql, 0);
	if (WsSql->attempt((const char *) sql.data, 0, NULL, &rows, &failure))
		ws_sql_rows_free(&rows);
	else if (strcmp(failure.sqlstate, "23P01") == 0)
	{
		found_add(f, s->name, s->constraints[n].name, "exclusion", NULL);
		ws_sql_failure_free(&failure);
	}
	else
		ereport(ERROR,
				(errcode(MAKE_SQLSTATE(failure.sqlstate[0], failure.sqlstate[1], failure.sqlstate[2],
									   failure.sqlstate[3], failure.sqlstate[4])),
				 errmsg("%s", failure.message != NULL ? failure.message : "an exclusion constraint was refused")));
	buf_free(&sql);
}

/* What a table's own constraints refuse of its rows. */
static void
find_in_table(Found *f, const WsTableSchema *s, const char *schema)
{
	/* (a partitioned table holds no rows, and each partition these as its own) */
	if (s->partition != WS_PARTITION_NONE)
		return;
	for (int i = 0; i < s->ncols; i++)
		if (s->cols[i].not_null != NULL)
			rows_violating(f, psprintf("SELECT %s FROM %s x WHERE x.%s IS NOT DISTINCT FROM NULL", rowkey_sql(s), table_sql(s, schema),
									   column_sql(s, s->cols[i].tag)),
						   s, s->cols[i].not_null, "not null");
	for (int i = 0; i < s->nconstraints; i++)
	{
		const WsConstraint *con = &s->constraints[i];

		if (con->kind == WS_CONSTRAINT_CHECK)
		{
			if (!con->not_valid && !con->not_enforced)
				rows_violating(f, psprintf("SELECT %s FROM %s x WHERE (%s) IS FALSE", rowkey_sql(s), table_sql(s, schema),
										   expression_sql(s, con->expr)),
							   s, con->name, "check");
		}
		else if (con->kind == WS_CONSTRAINT_EXCLUSION)
			exclusion_violated(f, s, i, schema);
		else
		{
			StringInfoData keys;
			StringInfoData not_null;

			initStringInfo(&keys);
			initStringInfo(&not_null);
			for (int k = 0; k < con->ncols; k++)
			{
				appendStringInfo(&keys, "%s%s", k > 0 ? ", " : "", column_sql(s, con->cols[k]));
				appendStringInfo(&not_null, "%s%s IS DISTINCT FROM NULL", k > 0 ? " AND " : "", column_sql(s, con->cols[k]));
			}
			unique_violating(f, s, schema, con->name, keys.data, con->nulls_not_distinct ? NULL : not_null.data, NULL);
			pfree(keys.data);
			pfree(not_null.data);
		}
	}
	for (int i = 0; i < s->nindexes; i++)
	{
		const WsIndex *ix = &s->indexes[i];
		StringInfoData keys;
		StringInfoData not_null;

		if (!ix->unique)
			continue;
		initStringInfo(&keys);
		initStringInfo(&not_null);
		for (int k = 0; k < ix->nkeys; k++)
		{
			const char *key = ix->keys[k].tag != 0 ? column_sql(s, ix->keys[k].tag) :
				psprintf("(%s)", expression_sql(s, ix->keys[k].expr));

			appendStringInfo(&keys, "%s%s", k > 0 ? ", " : "", key);
			if (ix->keys[k].collation != NULL)
				appendStringInfo(&keys, " COLLATE %s", ws_text_from_utf8(ix->keys[k].collation));
			appendStringInfo(&not_null, "%s%s IS DISTINCT FROM NULL", k > 0 ? " AND " : "", key);
		}
		unique_violating(f, s, schema, ix->name, keys.data, ix->nulls_not_distinct ? NULL : not_null.data,
						 ix->predicate != NULL ? expression_sql(s, ix->predicate) : NULL);
		pfree(keys.data);
		pfree(not_null.data);
	}
}

/* An operator's name as a query says it, whatever is on its path. */
static char *
operator_sql(Oid op)
{
	HeapTuple	tup = SearchSysCache1(OPEROID, ObjectIdGetDatum(op));
	Form_pg_operator form;
	char	   *out;

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "no operator %u", op);
	form = (Form_pg_operator) GETSTRUCT(tup);
	out = psprintf("OPERATOR(%s.%s)", quote_identifier(get_namespace_name(form->oprnamespace)), NameStr(form->oprname));
	ReleaseSysCache(tup);
	return out;
}

/*
 * What a foreign key compares a referenced column and its own by, y's then x's, as adding
 * it finds that: the referenced type's equality, against this column's type if it has one
 * for that, else against this column cast to its own; in the referenced column's collation.
 */
static void
key_column_match(StringInfo out, const WsColumn *child, const WsColumn *parent, const char *c, const char *p)
{
	Oid			ptype;
	Oid			ctype;
	int32		mod;
	Oid			opclass;
	Oid			collation;
	const char *cast = "";
	const char *op = "=";
	const char *collate = "";

	(void) parseTypeString(ws_text_from_utf8(parent->type), &ptype, &mod, NULL);
	(void) parseTypeString(ws_text_from_utf8(child->type), &ctype, &mod, NULL);
	ptype = getBaseType(ptype);
	ctype = getBaseType(ctype);
	opclass = GetDefaultOpClass(ptype, BTREE_AM_OID);
	if (OidIsValid(opclass))
	{
		Oid			family = get_opclass_family(opclass);
		Oid			intype = get_opclass_input_type(opclass);
		Oid			eq = get_opfamily_member(family, intype, ctype, BTEqualStrategyNumber);

		if (OidIsValid(eq))
			op = operator_sql(eq);
		else if (get_typtype(intype) != TYPTYPE_PSEUDO)
		{
			op = operator_sql(get_opfamily_member(family, intype, intype, BTEqualStrategyNumber));
			cast = psprintf("::%s", format_type_be_qualified(intype));
		}
	}
	collation = get_typcollation(ptype);
	if (OidIsValid(collation) && !(parent->collation == NULL ? child->collation == NULL :
								   child->collation != NULL && strcmp(parent->collation, child->collation) == 0))
		collate = psprintf(" COLLATE %s", parent->collation != NULL ? ws_text_from_utf8(parent->collation) :
						   generate_collation_name(collation));
	appendStringInfo(out, "y.%s %s (x.%s%s)%s", p, op, c, cast, collate);
}

/* Rows of a foreign key's table whose referenced row is not there. */
static void
find_for_key(Found *f, const WsForeignKey *fk, const WsTableSchema *child, const WsTableSchema *parent, const char *schema)
{
	StringInfoData present;
	StringInfoData match;

	if (fk->not_valid)
		return;
	initStringInfo(&present);
	initStringInfo(&match);
	for (int k = 0; k < fk->ncols; k++)
	{
		const char *c = column_sql(child, fk->cols[k]);

		/* (MATCH FULL refuses any but all null, and SIMPLE looks only if none is) */
		appendStringInfo(&present, "%sx.%s IS NOT NULL", k > 0 ? (fk->match == 'f' ? " OR " : " AND ") : "", c);
		if (k > 0)
			appendStringInfoString(&match, " AND ");
		key_column_match(&match, ws_schema_column(child, fk->cols[k]), ws_schema_column(parent, fk->ref_cols[k]),
						 c, column_sql(parent, fk->ref_cols[k]));
	}
	rows_violating(f, psprintf("SELECT %s FROM %s x WHERE (%s) AND NOT EXISTS (SELECT 1 FROM %s y WHERE %s)",
							   rowkey_sql(child), table_sql(child, schema), present.data,
							   table_sql(parent, schema), match.data),
				   child, fk->name, "foreign key");
	pfree(present.data);
	pfree(match.data);
}

typedef struct Search
{
	ProllyCtx  *ctx;
	const PHash *root;
	Found	   *found;
	const char *schema;
} Search;

static bool
search_table(void *arg, const char *name, const VcTable *entry)
{
	Search	   *search = arg;
	WsTableSchema rec;
	PHash		rows;

	if (!WsRoot->get_table(search->ctx, search->root, name, &rec, &rows))
		elog(ERROR, "a table listed is not there");
	find_in_table(search->found, &rec, search->schema);
	ws_schema_free(&rec);
	return true;
}

/* What the rows violate, in a schema of them with nothing there to refuse them. */
static void
search_rows(void *arg, const char *schema)
{
	Search	   *search = arg;
	PHash		map;

	search->schema = schema;
	WsRoot->tables(search->ctx, search->root, search_table, search);
	if (WsRoot->get_map(search->ctx, search->root, WS_ROOT_FOREIGN_KEYS, &map))
	{
		WsForeignKey *fks;
		int			n;

		ws_fk_list(search->ctx, &map, &fks, &n);
		for (int i = 0; i < n; i++)
		{
			WsTableSchema child;
			WsTableSchema parent;
			PHash		rows;

			if (!WsRoot->get_table(search->ctx, search->root, fks[i].table, &child, &rows) ||
				!WsRoot->get_table(search->ctx, search->root, fks[i].ref_table, &parent, &rows))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("foreign key \"%s\" names a table not there", fks[i].name)));
			find_for_key(search->found, &fks[i], &child, &parent, schema);
			ws_schema_free(&child);
			ws_schema_free(&parent);
		}
	}
}

/* ------------------------------------------------------------------- the map */

static int
found_cmp(const void *a, const void *b)
{
	const WsViolation *x = a;
	const WsViolation *y = b;
	int			c = strcmp(x->table, y->table);

	if (c == 0)
		c = strcmp(x->kind, y->kind);
	if (c == 0)
		c = strcmp(x->constraint, y->constraint);
	if (c != 0)
		return c;
	if (x->rowkey == NULL || y->rowkey == NULL)
		return (x->rowkey != NULL) - (y->rowkey != NULL);
	return strcmp(x->rowkey, y->rowkey);
}

void
ws_viol_key(const uint8 *key, size_t klen, WsViolation *out)
{
	const char *parts[3];
	size_t		at = 0;

	for (int i = 0; i < 3; i++)
	{
		size_t		len = at < klen ? strnlen((const char *) key + at, klen - at) : 0;

		if (at > klen || (i < 2 && at + len >= klen))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("a violation's key has too few parts")));
		parts[i] = pnstrdup((const char *) key + at, len);
		at += len + 1;
	}
	out->table = (char *) parts[0];
	out->kind = (char *) parts[1];
	out->constraint = (char *) parts[2];
	out->rowkey = at <= klen ? pnstrdup((const char *) key + at, klen - at) : NULL;
}

/* What was found, each once (a keyless row's copies are one), as the root's map. */
static void
put_found(ProllyCtx *ctx, Found *f, const PHash *root, PHash *out)
{
	ProllyKV   *kvs = palloc_array(ProllyKV, Max(f->n, 1));
	int			n = 0;
	PHash		map;

	if (f->n > 1)
		qsort(f->v, f->n, sizeof(WsViolation), found_cmp);
	for (int i = 0; i < f->n; i++)
	{
		const WsViolation *v = &f->v[i];
		Buf			key = BUF_INIT;

		if (i > 0 && found_cmp(&f->v[i - 1], v) == 0)
			continue;
		buf_append(&key, (const uint8 *) v->table, strlen(v->table) + 1);
		buf_append(&key, (const uint8 *) v->kind, strlen(v->kind) + 1);
		buf_append(&key, (const uint8 *) v->constraint, strlen(v->constraint));
		if (v->rowkey != NULL)
		{
			buf_append_byte(&key, 0);
			buf_append(&key, (const uint8 *) v->rowkey, strlen(v->rowkey));
		}
		kvs[n].key = key.data;
		kvs[n].klen = key.len;
		kvs[n].val = (const uint8 *) "1";
		kvs[n].vlen = 1;
		n++;
	}
	if (prolly_build(ctx, 0, kvs, n, &map) < 0)
		prolly_report(ctx);
	WsRoot->put_map(ctx, root, WS_ROOT_VIOLATIONS, &map, out);
	for (int i = 0; i < n; i++)
		pfree((void *) kvs[i].key);
	pfree(kvs);
}

static void
query_list(ProllyCtx *ctx, const PHash *root, WsViolation **out, int *n)
{
	Found		f = {.cxt = CurrentMemoryContext};
	PHash		map;

	if (WsRoot->get_map(ctx, root, WS_ROOT_VIOLATIONS, &map))
	{
		ProllyIter *it;
		const uint8 *k;
		const uint8 *v;
		size_t		kl;
		size_t		vl;
		int			rc;

		if (prolly_iter_open(ctx, &map, NULL, 0, &it) < 0)
			prolly_report(ctx);
		while ((rc = prolly_iter_next(it, &k, &kl, &v, &vl)) > 0)
		{
			WsViolation one;

			ws_viol_key(k, kl, &one);
			found_add(&f, one.table, one.constraint, one.kind, one.rowkey);
		}
		prolly_iter_close(it);
		if (rc < 0)
			prolly_report(ctx);
	}
	*out = f.v;
	*n = f.n;
}

/* --------------------------------------------------------------- rechecking */

static bool
query_recheck(ProllyCtx *ctx, const PHash *root, PHash *out, char **why)
{
	Found		found = {.cxt = CurrentMemoryContext};
	Search		search = {ctx, root, &found};
	WsSqlFailure failure;

	if (ws_root_rows_refused(ctx, root, search_rows, &search, &failure) ||
		ws_root_statements_refused(ctx, root, &failure))
	{
		*why = psprintf("%s", failure.message != NULL ? failure.message : "it was refused");
		ws_sql_failure_free(&failure);
		return false;
	}
	put_found(ctx, &found, root, out);
	return true;
}

const WsViolRoutine QueryWsViol = {
	.name = "query",
	.recheck = query_recheck,
	.list = query_list,
};

const WsViolRoutine *WsViol = &QueryWsViol;

void
ws_merge_check(ProllyCtx *ctx, WsMergeResult *result)
{
	char	   *why = NULL;
	PHash		checked;

	if (result->nconflicts > 0)
		return;
	if (WsViol->recheck(ctx, &result->root, &checked, &why))
	{
		result->root = checked;
		return;
	}
	result->conflicts = palloc_array(char *, 1);
	result->conflicts[0] = psprintf("unapplied %s", why);
	result->nconflicts = 1;
}
