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

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "access/stratnum.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type_d.h"
#include "catalog/pg_ws_deferred.h"
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
#include "utils/fmgroids.h"
#include "utils/rel.h"
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

	ws_schema_add_constraint(s, n, schema, &sql);
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
 * for that, else against this column cast to its own; in the referenced column's collation,
 * which its type may say.
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
	collation = get_typcollation(ptype);
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
			/* (of no length: the name alone of char or bit is of one) */
			cast = psprintf("::%s", format_type_extended(intype, -1, FORMAT_TYPE_FORCE_QUALIFY | FORMAT_TYPE_TYPEMOD_GIVEN));
		}
	}
	if (OidIsValid(collation))
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
query_recheck(ProllyCtx *ctx, const PHash *root, const char *schema, PHash *out, char **why)
{
	Found		found = {.cxt = CurrentMemoryContext};
	Search		search = {ctx, root, &found};
	WsSqlFailure failure;

	if (ws_root_rows_refused(ctx, root, schema, search_rows, &search, &failure) ||
		ws_root_statements_refused(ctx, root, schema, &failure))
	{
		*why = psprintf("%s", failure.message != NULL ? failure.message : "it was refused");
		ws_sql_failure_free(&failure);
		return false;
	}
	put_found(ctx, &found, root, out);
	return true;
}

/* -------------------------------------------------- what the catalog holds */

/* What is deferred of a table, read now, palloc'd, of *n. */
static WsDeferred *
catalog_load(Oid relid, int *n)
{
	const char *params[1] = {psprintf("%u", relid)};
	WsSqlRows	rows;
	WsDeferred *out;

	WsSql->run("SELECT defkind::text, defname::text, encode(defrecord, 'hex'), encode(defkey, 'hex') "
			   "FROM pg_ws_deferred WHERE defrelid = $1::oid ORDER BY defkind, defname COLLATE \"C\"",
			   1, params, &rows);
	out = palloc0_array(WsDeferred, Max(rows.nrows, 1));
	for (int i = 0; i < rows.nrows; i++)
	{
		const char *record = ws_sql_value(&rows, i, 2);
		const char *key = ws_sql_value(&rows, i, 3);

		out[i].kind = ws_sql_value(&rows, i, 0)[0];
		out[i].name = ws_name_to_utf8(ws_sql_value(&rows, i, 1));
		out[i].record.data = palloc(strlen(record) / 2 + 1);
		out[i].record.len = hex_decode(record, strlen(record), (char *) out[i].record.data);
		out[i].key.data = palloc(strlen(key) / 2 + 1);
		out[i].key.len = hex_decode(key, strlen(key), (char *) out[i].key.data);
	}
	*n = rows.nrows;
	ws_sql_rows_free(&rows);
	return out;
}

static bytea *
bytes_of(const Buf *b)
{
	bytea	   *out = palloc(VARHDRSZ + b->len);

	SET_VARSIZE(out, VARHDRSZ + b->len);
	if (b->len > 0)
		memcpy(VARDATA(out), b->data, b->len);
	return out;
}

static void
catalog_add(Oid relid, const WsDeferred *d)
{
	Relation	rel = table_open(WsDeferredRelationId, RowExclusiveLock);
	Datum		values[Natts_pg_ws_deferred];
	bool		nulls[Natts_pg_ws_deferred] = {false};
	NameData	name;
	HeapTuple	tup;

	namestrcpy(&name, ws_name_from_utf8(d->name));
	values[Anum_pg_ws_deferred_defrelid - 1] = ObjectIdGetDatum(relid);
	values[Anum_pg_ws_deferred_defkind - 1] = CharGetDatum(d->kind);
	values[Anum_pg_ws_deferred_defname - 1] = NameGetDatum(&name);
	values[Anum_pg_ws_deferred_defrecord - 1] = PointerGetDatum(bytes_of(&d->record));
	values[Anum_pg_ws_deferred_defkey - 1] = PointerGetDatum(bytes_of(&d->key));
	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);
	table_close(rel, RowExclusiveLock);
}

static void
catalog_forget(Oid relid, char kind, const char *name)
{
	Relation	rel = table_open(WsDeferredRelationId, RowExclusiveLock);
	ScanKeyData keys[1];
	SysScanDesc scan;
	HeapTuple	tup;
	char	   *here = ws_name_from_utf8(name);

	ScanKeyInit(&keys[0], Anum_pg_ws_deferred_defrelid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(relid));
	scan = systable_beginscan(rel, WsDeferredRelidKindNameIndexId, true, NULL, 1, keys);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_ws_deferred form = (Form_pg_ws_deferred) GETSTRUCT(tup);

		if (form->defkind == kind && strcmp(NameStr(form->defname), here) == 0)
			CatalogTupleDelete(rel, &tup->t_self);
	}
	systable_endscan(scan);
	table_close(rel, RowExclusiveLock);
}

static void
catalog_forget_table(Oid relid)
{
	Relation	rel = table_open(WsDeferredRelationId, RowExclusiveLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;

	ScanKeyInit(&key, Anum_pg_ws_deferred_defrelid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(relid));
	scan = systable_beginscan(rel, WsDeferredRelidKindNameIndexId, true, NULL, 1, &key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
		CatalogTupleDelete(rel, &tup->t_self);
	systable_endscan(scan);
	table_close(rel, RowExclusiveLock);
}

static const WsDeferredStore catalog_store = {
	.load = catalog_load,
	.add = catalog_add,
	.forget = catalog_forget,
	.forget_table = catalog_forget_table,
};

const WsDeferredStore *const WsDeferreds = &catalog_store;

/* ------------------------------------------------- restoring: setting aside */

static const WsDeferredItem *
deferred_item(const WsDeferrals *d, const char *table, char kind, const char *name)
{
	for (int i = 0; i < d->n; i++)
		if (d->items[i].deferred.kind == kind && strcmp(d->items[i].table, table) == 0 &&
			strcmp(d->items[i].deferred.name, name) == 0)
			return &d->items[i];
	return NULL;
}

static void
item_add(WsDeferrals *d, const char *table, char kind, const char *name, const Buf *record, const Buf *key)
{
	static const Buf none = BUF_INIT;
	WsDeferredItem *item;

	if (deferred_item(d, table, kind, name) != NULL)
		return;
	d->items = d->items == NULL ? palloc_array(WsDeferredItem, 8) : repalloc_array(d->items, WsDeferredItem, d->n + 8);
	item = &d->items[d->n++];
	item->table = pstrdup(table);
	item->deferred.kind = kind;
	item->deferred.name = pstrdup(name);
	item->deferred.record = *record;
	item->deferred.key = key != NULL ? *key : none;
}

static bool
same_columns(const uint64 *a, int na, const uint64 *b, int nb)
{
	if (na != nb)
		return false;
	for (int i = 0; i < na; i++)
	{
		bool		found = false;

		for (int k = 0; !found && k < nb; k++)
			found = a[i] == b[k];
		if (!found)
			return false;
	}
	return true;
}

/* Whether a table's deferred unique constraint or index is what a key to it would need. */
static bool
key_needs_deferred(const WsDeferrals *d, const WsTableSchema *to, const WsForeignKey *fk)
{
	for (int i = 0; i < to->nconstraints; i++)
		if (to->constraints[i].kind == WS_CONSTRAINT_UNIQUE &&
			deferred_item(d, to->name, WS_DEFERRED_UNIQUE, to->constraints[i].name) != NULL &&
			same_columns(fk->ref_cols, fk->ncols, to->constraints[i].cols, to->constraints[i].ncols))
			return true;
	for (int i = 0; i < to->nindexes; i++)
	{
		const WsIndex *ix = &to->indexes[i];
		uint64	   *tags;

		if (deferred_item(d, to->name, WS_DEFERRED_INDEX, ix->name) == NULL || ix->nkeys != fk->ncols)
			continue;
		tags = palloc_array(uint64, Max(ix->nkeys, 1));
		for (int k = 0; k < ix->nkeys; k++)
			tags[k] = ix->keys[k].tag;
		if (same_columns(fk->ref_cols, fk->ncols, tags, ix->nkeys))
			return true;
	}
	return false;
}

static void
strip_comments(WsTableSchema *s, const WsDeferrals *d)
{
	int			kept = 0;

	for (int i = 0; i < s->ncomments; i++)
	{
		const WsComment *c = &s->comments[i];
		bool		named = c->kind == WS_COMMENT_CONSTRAINT || c->kind == WS_COMMENT_INDEX;

		if (!named || (deferred_item(d, s->name, WS_DEFERRED_CHECK, c->name) == NULL &&
					   deferred_item(d, s->name, WS_DEFERRED_NOT_NULL, c->name) == NULL &&
					   deferred_item(d, s->name, WS_DEFERRED_UNIQUE, c->name) == NULL &&
					   deferred_item(d, s->name, WS_DEFERRED_EXCLUSION, c->name) == NULL &&
					   deferred_item(d, s->name, WS_DEFERRED_INDEX, c->name) == NULL))
			s->comments[kept++] = *c;
	}
	s->ncomments = kept;
}

static void
strip_record(WsTableSchema *s, const WsDeferrals *d)
{
	int			kept = 0;

	for (int i = 0; i < s->ncols; i++)
		if (s->cols[i].not_null != NULL && deferred_item(d, s->name, WS_DEFERRED_NOT_NULL, s->cols[i].not_null) != NULL)
			s->cols[i].not_null = NULL;
	for (int i = 0; i < s->nconstraints; i++)
	{
		const WsConstraint *con = &s->constraints[i];
		char		kind = con->kind == WS_CONSTRAINT_CHECK ? WS_DEFERRED_CHECK :
			con->kind == WS_CONSTRAINT_UNIQUE ? WS_DEFERRED_UNIQUE : WS_DEFERRED_EXCLUSION;

		if (deferred_item(d, s->name, kind, con->name) == NULL)
			s->constraints[kept++] = *con;
	}
	s->nconstraints = kept;
	kept = 0;
	for (int i = 0; i < s->nindexes; i++)
		if (deferred_item(d, s->name, WS_DEFERRED_INDEX, s->indexes[i].name) == NULL)
			s->indexes[kept++] = s->indexes[i];
	s->nindexes = kept;
	kept = 0;
	for (int i = 0; i < s->nindex_settings; i++)
	{
		const char *index = s->index_settings[i].index;

		if (deferred_item(d, s->name, WS_DEFERRED_INDEX, index) == NULL &&
			deferred_item(d, s->name, WS_DEFERRED_UNIQUE, index) == NULL &&
			deferred_item(d, s->name, WS_DEFERRED_EXCLUSION, index) == NULL)
			s->index_settings[kept++] = s->index_settings[i];
	}
	s->nindex_settings = kept;
	strip_comments(s, d);
	if (s->replica_index != NULL && (deferred_item(d, s->name, WS_DEFERRED_INDEX, s->replica_index) != NULL ||
									 deferred_item(d, s->name, WS_DEFERRED_UNIQUE, s->replica_index) != NULL))
	{
		s->replica_identity = WS_REPLICA_DEFAULT;
		s->replica_index = NULL;
	}
	if (s->clustered != NULL && (deferred_item(d, s->name, WS_DEFERRED_INDEX, s->clustered) != NULL ||
								 deferred_item(d, s->name, WS_DEFERRED_UNIQUE, s->clustered) != NULL ||
								 deferred_item(d, s->name, WS_DEFERRED_EXCLUSION, s->clustered) != NULL))
		s->clustered = NULL;
}

void
ws_viol_defer(ProllyCtx *ctx, const PHash *root, WsTableSchema *recs, int nrecs, WsDeferrals *out)
{
	WsViolation *v;
	int			n;
	PHash		map;
	Buf		   *encoded = palloc0_array(Buf, Max(nrecs, 1));
	WsForeignKey *fks = NULL;
	int			nfks = 0;

	memset(out, 0, sizeof(*out));
	WsViol->list(ctx, root, &v, &n);
	if (n == 0)
		return;
	if (WsRoot->get_map(ctx, root, WS_ROOT_FOREIGN_KEYS, &map))
		ws_fk_list(ctx, &map, &fks, &nfks);
	for (int i = 0; i < nrecs; i++)
		WsSchemaCodec->encode(&recs[i], &encoded[i]);
	for (int i = 0; i < n; i++)
	{
		WsTableSchema *rec = NULL;
		const Buf  *bytes = NULL;
		char		kind;

		for (int r = 0; r < nrecs && rec == NULL; r++)
			if (strcmp(recs[r].name, v[i].table) == 0)
			{
				rec = &recs[r];
				bytes = &encoded[r];
			}
		if (rec == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("a violation is of table \"%s\", not there", v[i].table)));
		if (rec->partition != WS_PARTITION_NONE || rec->partition_of != NULL || rec->ninherits > 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("table \"%s\" has violations and is partitioned or inherits, which a restore cannot yet set them aside of",
							v[i].table)));
		for (int r = 0; r < nrecs; r++)
			for (int p = 0; p < recs[r].ninherits; p++)
				if (strcmp(recs[r].inherits[p], rec->name) == 0)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("table \"%s\" has violations and is partitioned or inherits, which a restore cannot yet set them aside of",
									v[i].table)));
		if (strcmp(v[i].kind, "foreign key") == 0)
		{
			for (int f = 0; f < nfks; f++)
				if (strcmp(fks[f].table, v[i].table) == 0 && strcmp(fks[f].name, v[i].constraint) == 0)
				{
					Buf			key = BUF_INIT;
					Buf			value = BUF_INIT;

					WsFkCodec->encode_key(&fks[f], &key);
					WsFkCodec->encode_value(&fks[f], &value);
					item_add(out, v[i].table, WS_DEFERRED_FOREIGN_KEY, v[i].constraint, &value, &key);
				}
			continue;
		}
		kind = strcmp(v[i].kind, "check") == 0 ? WS_DEFERRED_CHECK :
			strcmp(v[i].kind, "not null") == 0 ? WS_DEFERRED_NOT_NULL :
			strcmp(v[i].kind, "exclusion") == 0 ? WS_DEFERRED_EXCLUSION : WS_DEFERRED_INDEX;
		if (kind == WS_DEFERRED_INDEX)
			for (int c = 0; c < rec->nconstraints; c++)
				if (rec->constraints[c].kind == WS_CONSTRAINT_UNIQUE && strcmp(rec->constraints[c].name, v[i].constraint) == 0)
					kind = WS_DEFERRED_UNIQUE;
		item_add(out, v[i].table, kind, v[i].constraint, bytes, NULL);
	}

	/* (a key to what is set aside has nothing to be made on: it waits with it) */
	for (int f = 0; f < nfks; f++)
		for (int r = 0; r < nrecs; r++)
			if (strcmp(recs[r].name, fks[f].ref_table) == 0 && key_needs_deferred(out, &recs[r], &fks[f]))
			{
				Buf			key = BUF_INIT;
				Buf			value = BUF_INIT;

				WsFkCodec->encode_key(&fks[f], &key);
				WsFkCodec->encode_value(&fks[f], &value);
				item_add(out, fks[f].table, WS_DEFERRED_FOREIGN_KEY, fks[f].name, &value, &key);
			}
	for (int r = 0; r < nrecs; r++)
		strip_record(&recs[r], out);
}

bool
ws_viol_key_deferred(const WsDeferrals *d, const WsForeignKey *fk)
{
	return deferred_item(d, fk->table, WS_DEFERRED_FOREIGN_KEY, fk->name) != NULL;
}

void
ws_viol_defer_end(const char *schema, const WsDeferrals *d)
{
	Oid			nsp = get_namespace_oid(schema, false);

	for (int i = 0; i < d->n; i++)
		WsDeferreds->add(get_relname_relid(ws_name_from_utf8(d->items[i].table), nsp), &d->items[i].deferred);
	if (d->n > 0)
		CommandCounterIncrement();
}

/* ------------------------------------------------- capturing: folding back */

static int
constraint_name_cmp(const void *a, const void *b)
{
	return strcmp(((const WsConstraint *) a)->name, ((const WsConstraint *) b)->name);
}

static int
index_name_cmp(const void *a, const void *b)
{
	return strcmp(((const WsIndex *) a)->name, ((const WsIndex *) b)->name);
}

static int
settings_name_cmp(const void *a, const void *b)
{
	return strcmp(((const WsIndexSettings *) a)->index, ((const WsIndexSettings *) b)->index);
}

static int
comment_order_cmp(const void *a, const void *b)
{
	const WsComment *x = a;
	const WsComment *y = b;

	if (x->kind != y->kind)
		return (unsigned char) x->kind < (unsigned char) y->kind ? -1 : 1;
	if (x->name != NULL && y->name != NULL)
		return strcmp(x->name, y->name);
	return x->tag < y->tag ? -1 : x->tag > y->tag;
}

/* What of a name a record held comes back: its comment, settings, and what it serves. */
static void
fold_named(WsTableSchema *s, const WsTableSchema *held, const char *name)
{
	for (int i = 0; i < held->nindex_settings; i++)
		if (strcmp(held->index_settings[i].index, name) == 0)
		{
			s->index_settings = s->index_settings == NULL ? palloc_array(WsIndexSettings, 1) :
				repalloc_array(s->index_settings, WsIndexSettings, s->nindex_settings + 1);
			s->index_settings[s->nindex_settings++] = held->index_settings[i];
		}
	for (int i = 0; i < held->ncomments; i++)
		if ((held->comments[i].kind == WS_COMMENT_CONSTRAINT || held->comments[i].kind == WS_COMMENT_INDEX) &&
			strcmp(held->comments[i].name, name) == 0)
		{
			s->comments = s->comments == NULL ? palloc_array(WsComment, 1) :
				repalloc_array(s->comments, WsComment, s->ncomments + 1);
			s->comments[s->ncomments++] = held->comments[i];
		}
	if (held->replica_index != NULL && strcmp(held->replica_index, name) == 0)
	{
		s->replica_identity = held->replica_identity;
		s->replica_index = held->replica_index;
	}
	if (held->clustered != NULL && strcmp(held->clustered, name) == 0)
		s->clustered = held->clustered;
}

void
ws_viol_fold(const char *schema, WsTableSchema *s)
{
	Oid			relid = get_relname_relid(ws_name_from_utf8(s->name), get_namespace_oid(schema, false));
	int			n;
	WsDeferred *d = WsDeferreds->load(relid, &n);

	for (int i = 0; i < n; i++)
	{
		WsTableSchema held;

		if (d[i].kind == WS_DEFERRED_FOREIGN_KEY)
			continue;
		WsSchemaCodec->decode(d[i].record.data, d[i].record.len, &held);
		if (d[i].kind == WS_DEFERRED_NOT_NULL)
		{
			for (int c = 0; c < held.ncols; c++)
				if (held.cols[c].not_null != NULL && strcmp(held.cols[c].not_null, d[i].name) == 0)
					for (int k = 0; k < s->ncols; k++)
						if (s->cols[k].tag == held.cols[c].tag)
							s->cols[k].not_null = held.cols[c].not_null;
		}
		else if (d[i].kind == WS_DEFERRED_INDEX)
		{
			for (int k = 0; k < held.nindexes; k++)
				if (strcmp(held.indexes[k].name, d[i].name) == 0)
				{
					s->indexes = s->indexes == NULL ? palloc_array(WsIndex, 1) :
						repalloc_array(s->indexes, WsIndex, s->nindexes + 1);
					s->indexes[s->nindexes++] = held.indexes[k];
				}
		}
		else
		{
			for (int k = 0; k < held.nconstraints; k++)
				if (strcmp(held.constraints[k].name, d[i].name) == 0)
				{
					s->constraints = s->constraints == NULL ? palloc_array(WsConstraint, 1) :
						repalloc_array(s->constraints, WsConstraint, s->nconstraints + 1);
					s->constraints[s->nconstraints++] = held.constraints[k];
				}
		}
		fold_named(s, &held, d[i].name);
	}
	if (n > 0)
	{
		qsort(s->constraints, s->nconstraints, sizeof(WsConstraint), constraint_name_cmp);
		qsort(s->indexes, s->nindexes, sizeof(WsIndex), index_name_cmp);
		qsort(s->index_settings, s->nindex_settings, sizeof(WsIndexSettings), settings_name_cmp);
		qsort(s->comments, s->ncomments, sizeof(WsComment), comment_order_cmp);
	}
}

void
ws_viol_fold_keys(const char *schema, const char *table, WsForeignKey **fks, int *nfks)
{
	Oid			relid = get_relname_relid(table, get_namespace_oid(schema, false));
	int			n;
	WsDeferred *d = WsDeferreds->load(relid, &n);

	for (int i = 0; i < n; i++)
		if (d[i].kind == WS_DEFERRED_FOREIGN_KEY)
		{
			*fks = *fks == NULL ? palloc_array(WsForeignKey, 1) : repalloc_array(*fks, WsForeignKey, *nfks + 1);
			WsFkCodec->decode(d[i].key.data, d[i].key.len, d[i].record.data, d[i].record.len, &(*fks)[(*nfks)++]);
		}
}

bool
ws_viol_any_deferred(const char *schema)
{
	const char *params[1] = {schema};
	WsSqlRows	rows;
	bool		any;

	WsSql->run("SELECT EXISTS (SELECT FROM pg_ws_deferred d JOIN pg_class c ON c.oid = d.defrelid "
			   "JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = $1::name)", 1, params, &rows);
	any = ws_sql_value(&rows, 0, 0)[0] == 't';
	ws_sql_rows_free(&rows);
	return any;
}

/* ---------------------------------------------------------------- resolving */

/* A table's whole record, what is deferred of it included. */
static void
read_whole(const char *schema, const char *table, WsTableSchema *s)
{
	if (!ws_schema_read(schema, table, s))
		elog(ERROR, "table \"%s\" of schema \"%s\" is gone", table, schema);
	ws_viol_fold(schema, s);
}

/* The statement making what is deferred, in *sql. */
static void
deferred_statement(const WsTableSchema *s, const WsDeferred *d, const char *schema, const char *table, Buf *sql)
{
	switch (d->kind)
	{
		case WS_DEFERRED_NOT_NULL:
			for (int c = 0; c < s->ncols; c++)
				if (s->cols[c].not_null != NULL && strcmp(s->cols[c].not_null, d->name) == 0)
					ws_schema_add_not_null(s, s->cols[c].tag, schema, sql);
			return;
		case WS_DEFERRED_INDEX:
			for (int i = 0; i < s->nindexes; i++)
				if (strcmp(s->indexes[i].name, d->name) == 0)
					ws_schema_create_index(s, i, schema, sql);
			return;
		case WS_DEFERRED_FOREIGN_KEY:
			{
				WsForeignKey fk;
				WsTableSchema to;

				WsFkCodec->decode(d->key.data, d->key.len, d->record.data, d->record.len, &fk);
				read_whole(schema, ws_name_from_utf8(fk.ref_table), &to);
				ws_fk_add_statement(&fk, s, &to, schema, sql);
				ws_schema_free(&to);
				return;
			}
		default:
			for (int i = 0; i < s->nconstraints; i++)
				if (strcmp(s->constraints[i].name, d->name) == 0)
					ws_schema_add_constraint(s, i, schema, sql);
	}
}

/* What goes with what was just made: its comment, settings and what it serves. */
static void
made_named(const WsTableSchema *s, const char *name, const char *schema)
{
	Buf			sql = BUF_INIT;
	WsSqlRows	rows;

	for (int i = 0; i < s->ncomments; i++)
		if ((s->comments[i].kind == WS_COMMENT_CONSTRAINT || s->comments[i].kind == WS_COMMENT_INDEX) &&
			strcmp(s->comments[i].name, name) == 0)
			ws_schema_comment(s, i, schema, &sql);
	if (sql.len > 0)
	{
		buf_append_byte(&sql, 0);
		WsSql->run((const char *) sql.data, 0, NULL, &rows);
		ws_sql_rows_free(&rows);
		sql.len = 0;
	}
	for (int i = 0; i < s->nindex_settings; i++)
		if (strcmp(s->index_settings[i].index, name) == 0)
			ws_schema_index_stats(s, i, schema, &sql);
	if (sql.len > 0)
	{
		buf_append_byte(&sql, 0);
		WsSql->run((const char *) sql.data, 0, NULL, &rows);
		ws_sql_rows_free(&rows);
		sql.len = 0;
	}
	if ((s->replica_index != NULL && strcmp(s->replica_index, name) == 0) ||
		(s->clustered != NULL && strcmp(s->clustered, name) == 0))
	{
		ws_schema_table_settings(s, schema, &sql);
		buf_append_byte(&sql, 0);
		WsSql->run((const char *) sql.data, 0, NULL, &rows);
		ws_sql_rows_free(&rows);
	}
	buf_free(&sql);
}

static int
query_resolve(ProllyCtx *ctx, const char *schema)
{
	const char *params[1] = {schema};
	int			level = WsSql->begin();
	int			nest = ws_schema_path_begin(schema);
	WsSqlRows	tables;
	int			left = 0;

	/* (foreign keys last, as one may need a unique constraint that was waiting too) */
	for (int pass = 0; pass < 2; pass++)
	{
		WsSql->run("SELECT DISTINCT c.oid::text, c.relname::text FROM pg_ws_deferred d JOIN pg_class c ON c.oid = d.defrelid "
				   "JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = $1::name ORDER BY 2", 1, params, &tables);
		for (int t = 0; t < tables.nrows; t++)
		{
			Oid			relid = atooid(ws_sql_value(&tables, t, 0));
			const char *table = ws_sql_value(&tables, t, 1);
			int			n;
			WsDeferred *d = WsDeferreds->load(relid, &n);

			for (int i = 0; i < n; i++)
			{
				WsTableSchema s;
				Buf			sql = BUF_INIT;
				WsSqlRows	rows;
				WsSqlFailure failure;

				CHECK_FOR_INTERRUPTS();
				if ((d[i].kind == WS_DEFERRED_FOREIGN_KEY) != (pass == 1))
					continue;
				read_whole(schema, table, &s);
				deferred_statement(&s, &d[i], schema, table, &sql);
				buf_append_byte(&sql, 0);
				if (WsSql->attempt((const char *) sql.data, 0, NULL, &rows, &failure))
				{
					ws_sql_rows_free(&rows);
					WsDeferreds->forget(relid, d[i].kind, d[i].name);
					CommandCounterIncrement();
					made_named(&s, d[i].name, schema);
				}
				else if (strncmp(failure.sqlstate, "23", 2) == 0 || strcmp(failure.sqlstate, "42830") == 0)
				{
					left++;
					ws_sql_failure_free(&failure);
				}
				else
					ereport(ERROR,
							(errcode(MAKE_SQLSTATE(failure.sqlstate[0], failure.sqlstate[1], failure.sqlstate[2],
												   failure.sqlstate[3], failure.sqlstate[4])),
							 errmsg("%s", failure.message != NULL ? failure.message : "a deferred constraint was refused")));
				ws_schema_free(&s);
				buf_free(&sql);
			}
		}
		ws_sql_rows_free(&tables);
	}
	ws_schema_path_end(nest);
	WsSql->end(level);
	return left;
}

const WsViolRoutine QueryWsViol = {
	.name = "query",
	.recheck = query_recheck,
	.list = query_list,
	.resolve = query_resolve,
};

const WsViolRoutine *WsViol = &QueryWsViol;

void
ws_merge_check(ProllyCtx *ctx, const char *schema, WsMergeResult *result)
{
	char	   *why = NULL;
	PHash		checked;

	if (result->nconflicts > 0)
		return;
	if (WsViol->recheck(ctx, &result->root, schema, &checked, &why))
	{
		result->root = checked;
		return;
	}
	result->conflicts = palloc_array(char *, 1);
	result->conflicts[0] = psprintf("unapplied %s", why);
	result->nconflicts = 1;
}
