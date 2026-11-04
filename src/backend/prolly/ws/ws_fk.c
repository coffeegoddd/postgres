/*-------------------------------------------------------------------------
 *
 * ws_fk.c
 *	  A versioned schema's foreign keys (prolly/ws/ws_fk.h), and "binary",
 *	  their codec.
 *
 * Its key: table, a NUL, name.  Its value: ref_table (uvarint length and
 * bytes), match, on_update and on_delete bytes, flags byte, uvarint ncols and
 * that many tags and ref_cols, uvarint nset and that many tags.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/ws/ws_fk.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "mb/pg_wchar.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "parser/parser.h"
#include "prolly/ws/ws_fk.h"
#include "prolly/ws/ws_row.h"
#include "prolly/ws/ws_sql.h"
#include "utils/builtins.h"

#define FK_DEFERRABLE 0x01
#define FK_INITDEFERRED 0x02
#define FK_NOT_VALID 0x04

static bool
known_action(char a)
{
	return a == FKCONSTR_ACTION_NOACTION || a == FKCONSTR_ACTION_RESTRICT ||
		a == FKCONSTR_ACTION_CASCADE || a == FKCONSTR_ACTION_SETNULL ||
		a == FKCONSTR_ACTION_SETDEFAULT;
}

const char *
ws_fk_problem(const WsForeignKey *fk)
{
	if (fk->table == NULL || fk->table[0] == '\0' || strlen(fk->table) >= NAMEDATALEN)
		return "a foreign key on no table, or one of a name too long";
	if (fk->name == NULL || fk->name[0] == '\0' || strlen(fk->name) >= NAMEDATALEN)
		return "a foreign key of no name, or one too long";
	if (fk->ref_table == NULL || fk->ref_table[0] == '\0' || strlen(fk->ref_table) >= NAMEDATALEN)
		return "a foreign key to no table, or one of a name too long";
	if (fk->ncols <= 0 || fk->ncols > INDEX_MAX_KEYS)
		return "a foreign key of no columns, or too many";
	for (int i = 0; i < fk->ncols; i++)
	{
		if (fk->cols[i] == 0 || fk->ref_cols[i] == 0)
			return "a foreign key with a column of no tag";
		for (int j = 0; j < i; j++)
			if (fk->ref_cols[j] == fk->ref_cols[i])
				return "a foreign key to a column twice";
	}
	if (fk->match != FKCONSTR_MATCH_FULL && fk->match != FKCONSTR_MATCH_SIMPLE)
		return "a foreign key of no known match";
	if (!known_action(fk->on_update) || !known_action(fk->on_delete))
		return "a foreign key of no known action";
	if (fk->nset < 0 || fk->nset > fk->ncols)
		return "a foreign key setting more columns than it has";
	if (fk->nset > 0 && fk->on_delete != FKCONSTR_ACTION_SETNULL &&
		fk->on_delete != FKCONSTR_ACTION_SETDEFAULT)
		return "a foreign key with columns to set and no action setting them";
	for (int i = 0; i < fk->nset; i++)
	{
		bool		of_key = false;

		for (int j = 0; j < fk->ncols; j++)
			of_key |= fk->cols[j] == fk->set_cols[i];
		if (!of_key)
			return "a foreign key setting a column not of it";
		for (int j = 0; j < i; j++)
			if (fk->set_cols[j] == fk->set_cols[i])
				return "a foreign key setting a column twice";
	}
	if (fk->initdeferred && !fk->deferrable)
		return "a foreign key initially deferred that is not deferrable";
	return NULL;
}

static void
fk_encode_key(const WsForeignKey *fk, Buf *out)
{
	const char *problem = ws_fk_problem(fk);

	if (problem != NULL)
		elog(ERROR, "%s", problem);
	buf_append(out, fk->table, strlen(fk->table));
	buf_append_byte(out, 0);
	buf_append(out, fk->name, strlen(fk->name));
}

static void
fk_encode_value(const WsForeignKey *fk, Buf *out)
{
	const char *problem = ws_fk_problem(fk);

	if (problem != NULL)
		elog(ERROR, "%s", problem);
	buf_append_uvarint(out, strlen(fk->ref_table));
	buf_append(out, fk->ref_table, strlen(fk->ref_table));
	buf_append_byte(out, (uint8) fk->match);
	buf_append_byte(out, (uint8) fk->on_update);
	buf_append_byte(out, (uint8) fk->on_delete);
	buf_append_byte(out, (fk->deferrable ? FK_DEFERRABLE : 0) |
					(fk->initdeferred ? FK_INITDEFERRED : 0) |
					(fk->not_valid ? FK_NOT_VALID : 0));
	buf_append_uvarint(out, (uint64) fk->ncols);
	for (int i = 0; i < fk->ncols; i++)
		buf_append_uvarint(out, fk->cols[i]);
	for (int i = 0; i < fk->ncols; i++)
		buf_append_uvarint(out, fk->ref_cols[i]);
	buf_append_uvarint(out, (uint64) fk->nset);
	for (int i = 0; i < fk->nset; i++)
		buf_append_uvarint(out, fk->set_cols[i]);
}

pg_noreturn static void
malformed(const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("a malformed foreign key: %s", what)));
}

static uint64
get_uvarint(const uint8 **p, const uint8 *end)
{
	uint64		v;

	if (!ws_read_canonical_uvarint(p, end, &v))
		malformed("a number runs past its end, or is not in its shortest form");
	return v;
}

static uint8
get_byte(const uint8 **p, const uint8 *end)
{
	if (*p >= end)
		malformed("it runs past its end");
	return *(*p)++;
}

static char *
get_name(const uint8 *p, size_t n)
{
	char	   *s;

	if (memchr(p, 0, n) != NULL || !pg_verify_mbstr(PG_UTF8, (const char *) p, (int) n, true))
		malformed("a name not UTF8");
	s = px_malloc(n + 1);
	memcpy(s, p, n);
	s[n] = '\0';
	return s;
}

static uint64 *
get_tags(const uint8 **p, const uint8 *end, uint64 n)
{
	uint64	   *tags = px_malloc((size_t) Max(n, 1) * sizeof(uint64));

	for (uint64 i = 0; i < n; i++)
		tags[i] = get_uvarint(p, end);
	return tags;
}

static void
fk_decode(const uint8 *key, size_t klen, const uint8 *val, size_t vlen, WsForeignKey *out)
{
	const uint8 *sep = memchr(key, 0, klen);
	const uint8 *p = val;
	const uint8 *end = val + vlen;
	uint64		n;
	uint8		flags;
	const char *problem;

	memset(out, 0, sizeof(*out));
	if (sep == NULL)
		malformed("a key of no name");
	out->table = get_name(key, (size_t) (sep - key));
	out->name = get_name(sep + 1, klen - (size_t) (sep - key) - 1);
	n = get_uvarint(&p, end);
	if (n >= NAMEDATALEN || n > (uint64) (end - p))
		malformed("a table's name too long");
	out->ref_table = get_name(p, (size_t) n);
	p += n;
	out->match = (char) get_byte(&p, end);
	out->on_update = (char) get_byte(&p, end);
	out->on_delete = (char) get_byte(&p, end);
	flags = get_byte(&p, end);
	if (flags & ~(FK_DEFERRABLE | FK_INITDEFERRED | FK_NOT_VALID))
		malformed("flags not known");
	out->deferrable = (flags & FK_DEFERRABLE) != 0;
	out->initdeferred = (flags & FK_INITDEFERRED) != 0;
	out->not_valid = (flags & FK_NOT_VALID) != 0;
	n = get_uvarint(&p, end);
	if (n > INDEX_MAX_KEYS)
		malformed("too many columns");
	out->ncols = (int) n;
	out->cols = get_tags(&p, end, n);
	out->ref_cols = get_tags(&p, end, n);
	n = get_uvarint(&p, end);
	if (n > INDEX_MAX_KEYS)
		malformed("too many columns to set");
	out->nset = (int) n;
	out->set_cols = get_tags(&p, end, n);
	if (p != end)
		malformed("it has bytes left over");
	problem = ws_fk_problem(out);
	if (problem != NULL)
		malformed(problem);
}

const WsFkCodecRoutine BinaryWsFkCodec = {
	.name = "binary",
	.encode_key = fk_encode_key,
	.encode_value = fk_encode_value,
	.decode = fk_decode,
};

const WsFkCodecRoutine *WsFkCodec = &BinaryWsFkCodec;

void
ws_fk_free(WsForeignKey *fk)
{
	px_free(fk->table);
	px_free(fk->name);
	px_free(fk->ref_table);
	px_free(fk->cols);
	px_free(fk->ref_cols);
	px_free(fk->set_cols);
	memset(fk, 0, sizeof(*fk));
}

/* ---------------------------------------------------------- the catalogs */

/* (a list of tags, signed as the catalog holds them, as space-separated text) */
static uint64 *
tags_from_text(const char *text, int *n)
{
	uint64	   *tags;
	int			count = 0;
	const char *p;

	if (text == NULL || text[0] == '\0')
	{
		*n = 0;
		return px_malloc(sizeof(uint64));
	}
	count = 1;
	for (p = text; *p; p++)
		count += *p == ' ';
	tags = px_malloc((size_t) count * sizeof(uint64));
	p = text;
	for (int i = 0; i < count; i++)
	{
		char	   *end;

		tags[i] = (uint64) strtoi64(p, &end, 10);
		p = end;
	}
	*n = count;
	return tags;
}

static int
fk_cmp(const void *a, const void *b)
{
	return strcmp(((const WsForeignKey *) a)->name, ((const WsForeignKey *) b)->name);
}

#define TAGS_OF(arr, rel) \
	"(SELECT string_agg(a.attcoltag::text, ' ' ORDER BY u.o) FROM unnest(" arr ") WITH ORDINALITY u(n, o) " \
	"JOIN pg_attribute a ON a.attrelid = " rel " AND a.attnum = u.n)"

/*
 * What no commit can hold of a table's foreign keys, the first of it.  One that
 * more than one unique index could serve is among them: the server picks by
 * object id, which a restore would not repeat, so it could choose an index of
 * another equality, or leave another droppable.
 */
static const char *const refusal_sql =
"SELECT k.conname::text, CASE "
"  WHEN p.relnamespace <> c.relnamespace "
"    THEN 'is to a table outside its schema, which no commit can hold' "
"  WHEN NOT k.conenforced OR k.conperiod THEN 'cannot be read yet: not enforced, or over a period' "
"  WHEN k.conparentid <> 0 AND EXISTS (SELECT FROM pg_constraint u WHERE u.oid = k.conparentid "
"                                       AND u.conrelid <> k.conrelid AND u.conname <> k.conname) "
"    THEN 'cannot be read yet: it is named otherwise than its partitioned table''s' "
"  WHEN k.conparentid <> 0 AND EXISTS (SELECT FROM pg_constraint u WHERE u.oid = k.conparentid "
"                                       AND u.conrelid <> k.conrelid AND u.convalidated <> k.convalidated) "
"    THEN 'cannot be read yet: it is validated otherwise than its partitioned table''s' "
"  WHEN EXISTS (SELECT FROM pg_trigger t WHERE t.tgconstraint = k.oid AND t.tgenabled <> 'O') "
"    THEN 'cannot be read yet: its triggers are disabled' "
"  WHEN EXISTS (SELECT FROM pg_trigger t JOIN pg_description d ON d.objoid = t.oid "
"               AND d.classoid = 'pg_trigger'::regclass WHERE t.tgconstraint = k.oid) "
"    THEN 'cannot be read yet: its triggers have comments' "
"  WHEN k.conparentid = 0 AND p.relkind = 'p' AND EXISTS (SELECT FROM pg_constraint o "
"               WHERE o.conrelid = k.conrelid AND o.contype = 'f' AND o.conparentid = 0 AND o.oid <> k.oid "
"                 AND o.conname::text ~ '_[0-9]+$' "
"                 AND starts_with(k.conname::text, regexp_replace(o.conname::text, '_[0-9]+$', '')) "
"                 AND octet_length(k.conname::text) > octet_length(regexp_replace(o.conname::text, '_[0-9]+$', '')) "
"                 AND octet_length(k.conname::text) + octet_length(o.conname::text) "
"                     - octet_length(regexp_replace(o.conname::text, '_[0-9]+$', '')) >= 64) "
"    THEN 'cannot be read yet: the numbered rows of so long a name could take another key''s' "
"  WHEN k.conparentid = 0 AND (SELECT count(*) FROM pg_index x WHERE x.indrelid = k.confrelid AND x.indisunique "
"        AND x.indisvalid AND x.indimmediate AND x.indpred IS NULL AND x.indexprs IS NULL "
"        AND x.indnkeyatts = cardinality(k.confkey) AND "
"          (SELECT array_agg(a ORDER BY a) FROM unnest((x.indkey::int2[])[0:x.indnkeyatts - 1]) a) = "
"          (SELECT array_agg(a ORDER BY a) FROM unnest(k.confkey) a)) > 1 "
"    THEN 'cannot be read yet: more than one unique index could serve it' "
"  END "
"FROM pg_constraint k JOIN pg_class c ON c.oid = k.conrelid JOIN pg_class p ON p.oid = k.confrelid "
"WHERE k.conrelid = $1::oid AND k.contype = 'f'";

void
ws_fk_refuse(const char *oid, const char *table)
{
	const char *params[1] = {oid};
	WsSqlRows	rows;

	WsSql->run(refusal_sql, 1, params, &rows);
	for (int i = 0; i < rows.nrows; i++)
		if (ws_sql_value(&rows, i, 1) != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("foreign key \"%s\" of table \"%s\" %s",
							ws_sql_value(&rows, i, 0), table, ws_sql_value(&rows, i, 1))));
	ws_sql_rows_free(&rows);
}

void
ws_fk_read(const char *schema, const char *table, WsForeignKey **out, int *n)
{
	const char *params[2] = {schema, table};
	int			level = WsSql->begin();
	WsSqlRows	rows;
	WsSqlRows	rel;

	WsSql->run("SELECT c.oid::text, ns.nspbranch::text FROM pg_class c "
			   "JOIN pg_namespace ns ON ns.oid = c.relnamespace "
			   "WHERE ns.nspname = $1 AND c.relname = $2", 2, params, &rel);
	if (rel.nrows == 1)
	{
		if (atoi(ws_sql_value(&rel, 0, 1)) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("table \"%s\" is not in a versioned schema", table)));
		ws_fk_refuse(ws_sql_value(&rel, 0, 0), table);
	}
	ws_sql_rows_free(&rel);

	WsSql->run("SELECT k.conname::text, p.relname::text, k.confmatchtype::text, "
			   "  k.confupdtype::text, k.confdeltype::text, k.condeferrable::text, "
			   "  k.condeferred::text, k.convalidated::text, "
			   TAGS_OF("k.conkey", "k.conrelid") ", "
			   TAGS_OF("k.confkey", "k.confrelid") ", "
			   TAGS_OF("k.confdelsetcols", "k.conrelid") " "
			   "FROM pg_constraint k JOIN pg_class c ON c.oid = k.conrelid "
			   "JOIN pg_namespace ns ON ns.oid = c.relnamespace "
			   "JOIN pg_class p ON p.oid = k.confrelid "
			   "WHERE ns.nspname = $1 AND c.relname = $2 AND k.contype = 'f' AND k.conparentid = 0",
			   2, params, &rows);
	*n = rows.nrows;
	*out = palloc0_array(WsForeignKey, Max(rows.nrows, 1));
	for (int i = 0; i < rows.nrows; i++)
	{
		WsForeignKey *fk = &(*out)[i];
		int			nref;

		fk->table = ws_name_to_utf8(table);
		fk->name = ws_name_to_utf8(ws_sql_value(&rows, i, 0));
		fk->ref_table = ws_name_to_utf8(ws_sql_value(&rows, i, 1));
		fk->match = ws_sql_value(&rows, i, 2)[0];
		fk->on_update = ws_sql_value(&rows, i, 3)[0];
		fk->on_delete = ws_sql_value(&rows, i, 4)[0];
		fk->deferrable = ws_sql_value(&rows, i, 5)[0] == 't';
		fk->initdeferred = ws_sql_value(&rows, i, 6)[0] == 't';
		fk->not_valid = ws_sql_value(&rows, i, 7)[0] != 't';
		fk->cols = tags_from_text(ws_sql_value(&rows, i, 8), &fk->ncols);
		fk->ref_cols = tags_from_text(ws_sql_value(&rows, i, 9), &nref);
		fk->set_cols = tags_from_text(ws_sql_value(&rows, i, 10), &fk->nset);
		if (nref != fk->ncols)
			elog(ERROR, "foreign key \"%s\" of %d columns and %d", fk->name, fk->ncols, nref);
	}
	if (*n > 1)
		qsort(*out, *n, sizeof(WsForeignKey), fk_cmp);
	ws_sql_rows_free(&rows);
	WsSql->end(level);
}

/* ---------------------------------------------------------------- the map */

static int
kv_cmp(const void *a, const void *b)
{
	const ProllyKV *x = a;
	const ProllyKV *y = b;
	int			c = memcmp(x->key, y->key, Min(x->klen, y->klen));

	return c != 0 ? c : (x->klen < y->klen ? -1 : x->klen > y->klen);
}

void
ws_fk_map(ProllyCtx *ctx, const WsForeignKey *fks, int n, PHash *map)
{
	ProllyKV   *kvs = palloc_array(ProllyKV, Max(n, 1));
	Buf		   *keys = palloc0_array(Buf, Max(n, 1));
	Buf		   *vals = palloc0_array(Buf, Max(n, 1));
	int			rc;

	for (int i = 0; i < n; i++)
	{
		WsFkCodec->encode_key(&fks[i], &keys[i]);
		WsFkCodec->encode_value(&fks[i], &vals[i]);
		kvs[i].key = keys[i].data;
		kvs[i].klen = keys[i].len;
		kvs[i].val = vals[i].data;
		kvs[i].vlen = vals[i].len;
	}
	if (n > 1)
		qsort(kvs, n, sizeof(ProllyKV), kv_cmp);
	/* (a tree of two entries of one key is no tree a read would take) */
	for (int i = 1; i < n; i++)
		if (kv_cmp(&kvs[i - 1], &kvs[i]) == 0)
			elog(ERROR, "two foreign keys of one name on one table");
	rc = prolly_build(ctx, 0, kvs, n, map);
	if (rc < 0)
		prolly_report(ctx);
	for (int i = 0; i < n; i++)
	{
		buf_free(&keys[i]);
		buf_free(&vals[i]);
	}
	pfree(kvs);
	pfree(keys);
	pfree(vals);
}

void
ws_fk_list(ProllyCtx *ctx, const PHash *map, WsForeignKey **out, int *n)
{
	ProllyIter *it;
	const uint8 *key;
	const uint8 *val;
	size_t		klen;
	size_t		vlen;
	uint64		count;
	int			rc;
	int			i = 0;

	rc = prolly_count(ctx, map, &count);
	if (rc < 0)
		prolly_report(ctx);
	*out = palloc0_array(WsForeignKey, Max(count, 1));
	rc = prolly_iter_open(ctx, map, NULL, 0, &it);
	if (rc < 0)
		prolly_report(ctx);
	while ((rc = prolly_iter_next(it, &key, &klen, &val, &vlen)) > 0)
	{
		/* (a root's count is as trusted as its entries, which is not at all) */
		if ((uint64) i >= count)
			malformed("a map of more keys than it says");
		WsFkCodec->decode(key, klen, val, vlen, &(*out)[i++]);
	}
	if (rc < 0)
		prolly_report(ctx);
	prolly_iter_close(it);
	*n = i;
}

/* ---------------------------------------------------------- the statement */

pg_noreturn static void
not_the_key(const WsForeignKey *fk, const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("foreign key \"%s\" of table \"%s\" does not make that key: %s",
					fk->name, fk->table, what)));
}

static char *
column_here(const WsForeignKey *fk, const WsTableSchema *s, uint64 tag)
{
	const WsColumn *c = ws_schema_column(s, tag);

	if (c == NULL)
		not_the_key(fk, "a column its table does not have");
	return ws_name_from_utf8(c->name);
}

static void
put_columns(Buf *out, const WsForeignKey *fk, const WsTableSchema *s, const uint64 *tags, int n)
{
	ws_put_text(out, "(");
	for (int i = 0; i < n; i++)
	{
		if (i > 0)
			ws_put_text(out, ", ");
		ws_put_identifier(out, column_here(fk, s, tags[i]));
	}
	ws_put_text(out, ")");
}

static const char *
action_text(char a)
{
	switch (a)
	{
		case FKCONSTR_ACTION_RESTRICT:
			return "RESTRICT";
		case FKCONSTR_ACTION_CASCADE:
			return "CASCADE";
		case FKCONSTR_ACTION_SETNULL:
			return "SET NULL";
		case FKCONSTR_ACTION_SETDEFAULT:
			return "SET DEFAULT";
	}
	return "NO ACTION";
}

static bool
same_names(const WsForeignKey *fk, const WsTableSchema *s, const uint64 *tags, int n, List *names)
{
	int			i = 0;
	ListCell   *lc;

	if (list_length(names) != n)
		return false;
	foreach(lc, names)
		if (strcmp(strVal(lfirst(lc)), column_here(fk, s, tags[i++])) != 0)
			return false;
	return true;
}

static void
check_statement(const WsForeignKey *fk, const WsTableSchema *table, const WsTableSchema *ref,
				const char *schema, const char *sql)
{
	List	   *parsed = raw_parser(sql, RAW_PARSE_DEFAULT);
	AlterTableStmt *stmt;
	AlterTableCmd *cmd;
	Constraint *con;

	if (list_length(parsed) != 1 || !IsA(linitial_node(RawStmt, parsed)->stmt, AlterTableStmt))
		not_the_key(fk, "it is not one ALTER TABLE");
	stmt = (AlterTableStmt *) linitial_node(RawStmt, parsed)->stmt;
	if (stmt->relation->catalogname != NULL || stmt->relation->schemaname == NULL ||
		strcmp(stmt->relation->schemaname, schema) != 0 || !stmt->relation->inh ||
		strcmp(stmt->relation->relname, ws_name_from_utf8(fk->table)) != 0 ||
		stmt->objtype != OBJECT_TABLE || stmt->missing_ok || list_length(stmt->cmds) != 1)
		not_the_key(fk, "another table");
	cmd = linitial_node(AlterTableCmd, stmt->cmds);
	if (cmd->subtype != AT_AddConstraint || cmd->name != NULL || cmd->newowner != NULL ||
		cmd->missing_ok || !IsA(cmd->def, Constraint))
		not_the_key(fk, "another change");
	con = (Constraint *) cmd->def;
	if (con->contype != CONSTR_FOREIGN || con->conname == NULL ||
		strcmp(con->conname, ws_name_from_utf8(fk->name)) != 0 ||
		con->deferrable != fk->deferrable || con->initdeferred != fk->initdeferred ||
		!con->is_enforced || con->skip_validation != fk->not_valid ||
		con->initially_valid == fk->not_valid || con->is_no_inherit ||
		con->fk_with_period || con->pk_with_period ||
		con->fk_matchtype != fk->match || con->fk_upd_action != fk->on_update ||
		con->fk_del_action != fk->on_delete ||
		con->pktable == NULL || con->pktable->catalogname != NULL ||
		con->pktable->schemaname == NULL || strcmp(con->pktable->schemaname, schema) != 0 ||
		strcmp(con->pktable->relname, ws_name_from_utf8(fk->ref_table)) != 0 ||
		!same_names(fk, table, fk->cols, fk->ncols, con->fk_attrs) ||
		!same_names(fk, ref, fk->ref_cols, fk->ncols, con->pk_attrs) ||
		!same_names(fk, table, fk->set_cols, fk->nset, con->fk_del_set_cols))
		not_the_key(fk, "another key");
}

void
ws_fk_add_statement(const WsForeignKey *fk, const WsTableSchema *table,
					const WsTableSchema *ref_table, const char *schema, Buf *out)
{
	const char *problem = ws_fk_problem(fk);
	Buf			sql = BUF_INIT;

	if (problem != NULL)
		elog(ERROR, "%s", problem);
	if (strcmp(table->name, fk->table) != 0 || strcmp(ref_table->name, fk->ref_table) != 0)
		elog(ERROR, "records of other tables than foreign key \"%s\" names", fk->name);
	ws_put_text(&sql, "ALTER TABLE ");
	ws_put_identifier(&sql, schema);
	ws_put_text(&sql, ".");
	ws_put_identifier(&sql, ws_name_from_utf8(fk->table));
	ws_put_text(&sql, " ADD CONSTRAINT ");
	ws_put_identifier(&sql, ws_name_from_utf8(fk->name));
	ws_put_text(&sql, " FOREIGN KEY ");
	put_columns(&sql, fk, table, fk->cols, fk->ncols);
	ws_put_text(&sql, " REFERENCES ");
	ws_put_identifier(&sql, schema);
	ws_put_text(&sql, ".");
	ws_put_identifier(&sql, ws_name_from_utf8(fk->ref_table));
	ws_put_text(&sql, " ");
	put_columns(&sql, fk, ref_table, fk->ref_cols, fk->ncols);
	if (fk->match == FKCONSTR_MATCH_FULL)
		ws_put_text(&sql, " MATCH FULL");
	if (fk->on_update != FKCONSTR_ACTION_NOACTION)
	{
		ws_put_text(&sql, " ON UPDATE ");
		ws_put_text(&sql, action_text(fk->on_update));
	}
	if (fk->on_delete != FKCONSTR_ACTION_NOACTION)
	{
		ws_put_text(&sql, " ON DELETE ");
		ws_put_text(&sql, action_text(fk->on_delete));
		if (fk->nset > 0)
		{
			ws_put_text(&sql, " ");
			put_columns(&sql, fk, table, fk->set_cols, fk->nset);
		}
	}
	if (fk->deferrable)
		ws_put_text(&sql, fk->initdeferred ? " DEFERRABLE INITIALLY DEFERRED" : " DEFERRABLE");
	if (fk->not_valid)
		ws_put_text(&sql, " NOT VALID");
	buf_append_byte(&sql, 0);
	check_statement(fk, table, ref_table, schema, (char *) sql.data);
	buf_append(out, sql.data, sql.len - 1);
	buf_free(&sql);
}
