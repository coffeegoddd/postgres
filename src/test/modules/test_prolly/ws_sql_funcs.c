/*-------------------------------------------------------------------------
 *
 * ws_sql_funcs.c
 *	  SQL functions over the working-set model, for tests that need a server
 *	  of their own: a table's schema read from the catalogs, its bytes, and a
 *	  table made from them.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/test/modules/test_prolly/ws_sql_funcs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "prolly/ws/ws_fk.h"
#include "prolly/ws/ws_schema.h"
#include "prolly/ws/ws_sql.h"
#include "utils/builtins.h"
#include "varatt.h"

PG_FUNCTION_INFO_V1(test_ws_schema_bytes);
PG_FUNCTION_INFO_V1(test_ws_schema_create);
PG_FUNCTION_INFO_V1(test_ws_schema_attach);
PG_FUNCTION_INFO_V1(test_ws_schema_comments);
PG_FUNCTION_INFO_V1(test_ws_schema_row_security);
PG_FUNCTION_INFO_V1(test_ws_schema_inherit);
PG_FUNCTION_INFO_V1(test_ws_schema_set_inherited);
PG_FUNCTION_INFO_V1(test_ws_schema_add_checks);
PG_FUNCTION_INFO_V1(test_ws_schema_set_tags);
PG_FUNCTION_INFO_V1(test_ws_fk_bytes);
PG_FUNCTION_INFO_V1(test_ws_fk_copy);

/*
 * test_ws_schema_bytes(schema text, table text) returns bytea: the table's
 * schema, read from the catalogs and encoded; NULL if there is no such table
 */
Datum
test_ws_schema_bytes(PG_FUNCTION_ARGS)
{
	char	   *schema = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *table = text_to_cstring(PG_GETARG_TEXT_PP(1));
	WsTableSchema s;
	Buf			b = BUF_INIT;
	bytea	   *out;

	if (!ws_schema_read(schema, table, &s))
		PG_RETURN_NULL();
	WsSchemaCodec->encode(&s, &b);
	out = palloc(VARHDRSZ + b.len);
	SET_VARSIZE(out, VARHDRSZ + b.len);
	memcpy(VARDATA(out), b.data, b.len);
	ws_schema_free(&s);
	buf_free(&b);
	PG_RETURN_BYTEA_P(out);
}

/*
 * test_ws_schema_set_tags(bytes bytea, schema text) returns void: a schema's
 * bytes' tags given to its table in the schema
 */
Datum
test_ws_schema_set_tags(PG_FUNCTION_ARGS)
{
	bytea	   *bytes = PG_GETARG_BYTEA_PP(0);
	char	   *schema = text_to_cstring(PG_GETARG_TEXT_PP(1));
	WsTableSchema s;

	WsSchemaCodec->decode((const uint8 *) VARDATA_ANY(bytes), VARSIZE_ANY_EXHDR(bytes), &s);
	ws_schema_set_tags(schema, &s);
	ws_schema_free(&s);
	PG_RETURN_VOID();
}

/*
 * test_ws_schema_create(bytes bytea, schema text) returns text: the table of
 * a schema's bytes made in the schema, its columns with the tags the bytes
 * say; the statement that made it
 */
Datum
test_ws_schema_create(PG_FUNCTION_ARGS)
{
	bytea	   *bytes = PG_GETARG_BYTEA_PP(0);
	char	   *schema = text_to_cstring(PG_GETARG_TEXT_PP(1));
	WsTableSchema s;
	WsSqlRows	rows;
	Buf			sql = BUF_INIT;
	int			level = WsSql->begin();
	text	   *out;

	WsSchemaCodec->decode((const uint8 *) VARDATA_ANY(bytes), VARSIZE_ANY_EXHDR(bytes), &s);
	ws_schema_create_table(&s, schema, &sql);
	buf_append_byte(&sql, 0);
	WsSql->run((char *) sql.data, 0, NULL, &rows);
	ws_sql_rows_free(&rows);
	{
		Buf			set = BUF_INIT;

		ws_schema_column_settings(&s, schema, &set);
		if (set.len > 0)
		{
			buf_append_byte(&set, 0);
			WsSql->run((char *) set.data, 0, NULL, &rows);
			ws_sql_rows_free(&rows);
		}
		buf_free(&set);
	}
	for (int i = 0; i < s.nindexes; i++)
	{
		Buf			ix = BUF_INIT;

		ws_schema_create_index(&s, i, schema, &ix);
		buf_append_byte(&ix, 0);
		WsSql->run((char *) ix.data, 0, NULL, &rows);
		ws_sql_rows_free(&rows);
		buf_free(&ix);
	}
	for (int i = 0; i <= s.nindex_settings; i++)
	{
		Buf			set = BUF_INIT;

		if (i < s.nindex_settings)
			ws_schema_index_stats(&s, i, schema, &set);
		else
			ws_schema_table_settings(&s, schema, &set);
		if (set.len > 0)
		{
			buf_append_byte(&set, 0);
			WsSql->run((char *) set.data, 0, NULL, &rows);
			ws_sql_rows_free(&rows);
		}
		buf_free(&set);
	}
	ws_schema_set_tags(schema, &s);
	CommandCounterIncrement();
	WsSql->end(level);
	out = cstring_to_text((char *) sql.data);
	ws_schema_free(&s);
	buf_free(&sql);
	PG_RETURN_TEXT_P(out);
}

/*
 * test_ws_schema_add_checks(bytes bytea, schema text) returns text: the checks
 * not valid of a schema's bytes added to its table in the schema, its rows and
 * any partitions' such checks already there; the statements, a line each
 */
Datum
test_ws_schema_add_checks(PG_FUNCTION_ARGS)
{
	bytea	   *bytes = PG_GETARG_BYTEA_PP(0);
	char	   *schema = text_to_cstring(PG_GETARG_TEXT_PP(1));
	WsTableSchema s;
	WsSqlRows	rows;
	StringInfoData out;
	int			level = WsSql->begin();

	initStringInfo(&out);
	WsSchemaCodec->decode((const uint8 *) VARDATA_ANY(bytes), VARSIZE_ANY_EXHDR(bytes), &s);
	for (int i = 0; i < s.nconstraints; i++)
	{
		Buf			sql = BUF_INIT;

		if (!s.constraints[i].not_valid)
			continue;
		ws_schema_add_check(&s, i, schema, &sql);
		buf_append_byte(&sql, 0);
		WsSql->run((char *) sql.data, 0, NULL, &rows);
		ws_sql_rows_free(&rows);
		appendStringInfo(&out, "%s\n", (char *) sql.data);
		buf_free(&sql);
	}
	WsSql->end(level);
	ws_schema_free(&s);
	PG_RETURN_TEXT_P(cstring_to_text(out.data));
}

/*
 * test_ws_schema_attach(bytes bytea, schema text) returns text: a partition of
 * a schema's bytes attached to its table in the schema; the statement, or NULL
 * if it is none
 */
Datum
test_ws_schema_attach(PG_FUNCTION_ARGS)
{
	bytea	   *bytes = PG_GETARG_BYTEA_PP(0);
	char	   *schema = text_to_cstring(PG_GETARG_TEXT_PP(1));
	WsTableSchema s;
	WsSqlRows	rows;
	Buf			sql = BUF_INIT;
	int			level;
	text	   *out;

	WsSchemaCodec->decode((const uint8 *) VARDATA_ANY(bytes), VARSIZE_ANY_EXHDR(bytes), &s);
	if (s.partition_of == NULL)
	{
		ws_schema_free(&s);
		PG_RETURN_NULL();
	}
	level = WsSql->begin();
	ws_schema_attach(&s, schema, &sql);
	buf_append_byte(&sql, 0);
	WsSql->run((char *) sql.data, 0, NULL, &rows);
	ws_sql_rows_free(&rows);
	WsSql->end(level);
	out = cstring_to_text((char *) sql.data);
	ws_schema_free(&s);
	buf_free(&sql);
	PG_RETURN_TEXT_P(out);
}

/*
 * test_ws_schema_comments(bytes bytea, schema text) returns text: the comments of
 * a schema's bytes put on what is in the schema; the statements, a line each
 */
Datum
test_ws_schema_comments(PG_FUNCTION_ARGS)
{
	bytea	   *bytes = PG_GETARG_BYTEA_PP(0);
	char	   *schema = text_to_cstring(PG_GETARG_TEXT_PP(1));
	WsTableSchema s;
	StringInfoData out;
	int			level = WsSql->begin();

	initStringInfo(&out);
	WsSchemaCodec->decode((const uint8 *) VARDATA_ANY(bytes), VARSIZE_ANY_EXHDR(bytes), &s);
	for (int i = 0; i < s.ncomments; i++)
	{
		Buf			sql = BUF_INIT;
		WsSqlRows	rows;

		ws_schema_comment(&s, i, schema, &sql);
		buf_append_byte(&sql, 0);
		WsSql->run((char *) sql.data, 0, NULL, &rows);
		ws_sql_rows_free(&rows);
		appendStringInfo(&out, "%s\n", (char *) sql.data);
		buf_free(&sql);
	}
	WsSql->end(level);
	ws_schema_free(&s);
	PG_RETURN_TEXT_P(cstring_to_text(out.data));
}

/*
 * test_ws_schema_row_security(bytes bytea, schema text) returns text: the table of
 * a schema's bytes given its row security, in the schema; the statement, or NULL
 */
Datum
test_ws_schema_row_security(PG_FUNCTION_ARGS)
{
	bytea	   *bytes = PG_GETARG_BYTEA_PP(0);
	char	   *schema = text_to_cstring(PG_GETARG_TEXT_PP(1));
	WsTableSchema s;
	Buf			sql = BUF_INIT;
	WsSqlRows	rows;
	text	   *out = NULL;
	int			level = WsSql->begin();

	WsSchemaCodec->decode((const uint8 *) VARDATA_ANY(bytes), VARSIZE_ANY_EXHDR(bytes), &s);
	ws_schema_row_security(&s, schema, &sql);
	if (sql.len > 0)
	{
		buf_append_byte(&sql, 0);
		WsSql->run((char *) sql.data, 0, NULL, &rows);
		ws_sql_rows_free(&rows);
		out = cstring_to_text((char *) sql.data);
	}
	WsSql->end(level);
	ws_schema_free(&s);
	buf_free(&sql);
	if (out == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(out);
}

/*
 * test_ws_schema_inherit(bytes bytea, schema text) returns text: the table of a
 * schema's bytes made to inherit, in the schema, from its parents; the
 * statements, a line each
 */
Datum
test_ws_schema_inherit(PG_FUNCTION_ARGS)
{
	bytea	   *bytes = PG_GETARG_BYTEA_PP(0);
	char	   *schema = text_to_cstring(PG_GETARG_TEXT_PP(1));
	WsTableSchema s;
	StringInfoData out;
	int			level = WsSql->begin();

	initStringInfo(&out);
	WsSchemaCodec->decode((const uint8 *) VARDATA_ANY(bytes), VARSIZE_ANY_EXHDR(bytes), &s);
	for (int i = 0; i < s.ninherits; i++)
	{
		Buf			sql = BUF_INIT;
		WsSqlRows	rows;

		ws_schema_inherit(&s, i, schema, &sql);
		buf_append_byte(&sql, 0);
		WsSql->run((char *) sql.data, 0, NULL, &rows);
		ws_sql_rows_free(&rows);
		appendStringInfo(&out, "%s\n", (char *) sql.data);
		buf_free(&sql);
	}
	WsSql->end(level);
	ws_schema_free(&s);
	PG_RETURN_TEXT_P(cstring_to_text(out.data));
}

/*
 * test_ws_schema_set_inherited(bytes bytea, schema text) returns void: what a
 * schema's bytes say its table has only by inheriting marked so
 */
Datum
test_ws_schema_set_inherited(PG_FUNCTION_ARGS)
{
	bytea	   *bytes = PG_GETARG_BYTEA_PP(0);
	char	   *schema = text_to_cstring(PG_GETARG_TEXT_PP(1));
	WsTableSchema s;

	WsSchemaCodec->decode((const uint8 *) VARDATA_ANY(bytes), VARSIZE_ANY_EXHDR(bytes), &s);
	ws_schema_set_inherited(schema, &s);
	CommandCounterIncrement();
	ws_schema_free(&s);
	PG_RETURN_VOID();
}

/*
 * test_ws_fk_bytes(schema text, tab text) returns text: the table's foreign
 * keys' bytes, in hex, a line each
 */
Datum
test_ws_fk_bytes(PG_FUNCTION_ARGS)
{
	char	   *schema = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *table = text_to_cstring(PG_GETARG_TEXT_PP(1));
	WsForeignKey *fks;
	int			n;
	StringInfoData out;

	initStringInfo(&out);
	ws_fk_read(schema, table, &fks, &n);
	for (int i = 0; i < n; i++)
	{
		Buf			k = BUF_INIT;
		Buf			v = BUF_INIT;

		WsFkCodec->encode_key(&fks[i], &k);
		WsFkCodec->encode_value(&fks[i], &v);
		for (size_t j = 0; j < k.len; j++)
			appendStringInfo(&out, "%02x", k.data[j]);
		appendStringInfoChar(&out, ':');
		for (size_t j = 0; j < v.len; j++)
			appendStringInfo(&out, "%02x", v.data[j]);
		appendStringInfoChar(&out, '\n');
		buf_free(&k);
		buf_free(&v);
		ws_fk_free(&fks[i]);
	}
	PG_RETURN_TEXT_P(cstring_to_text(out.data));
}

/*
 * test_ws_fk_copy(from_schema text, tab text, to_schema text) returns text: the
 * table's foreign keys, through their bytes, added to its copy in to_schema;
 * the statements
 */
Datum
test_ws_fk_copy(PG_FUNCTION_ARGS)
{
	char	   *from = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *table = text_to_cstring(PG_GETARG_TEXT_PP(1));
	char	   *to = text_to_cstring(PG_GETARG_TEXT_PP(2));
	WsForeignKey *fks;
	int			n;
	StringInfoData out;
	int			level = WsSql->begin();

	initStringInfo(&out);
	ws_fk_read(from, table, &fks, &n);
	for (int i = n - 1; i >= 0; i--)
	{
		Buf			k = BUF_INIT;
		Buf			v = BUF_INIT;
		Buf			sql = BUF_INIT;
		WsForeignKey fk;
		WsTableSchema child;
		WsTableSchema parent;
		WsSqlRows	rows;

		WsFkCodec->encode_key(&fks[i], &k);
		WsFkCodec->encode_value(&fks[i], &v);
		WsFkCodec->decode(k.data, k.len, v.data, v.len, &fk);
		if (!ws_schema_read(from, table, &child) ||
			!ws_schema_read(from, pg_any_to_server(fk.ref_table, (int) strlen(fk.ref_table), PG_UTF8), &parent))
			elog(ERROR, "no such table");
		ws_fk_add_statement(&fk, &child, &parent, to, &sql);
		buf_append_byte(&sql, 0);
		WsSql->run((char *) sql.data, 0, NULL, &rows);
		ws_sql_rows_free(&rows);
		appendStringInfo(&out, "%s\n", (char *) sql.data);
		ws_schema_free(&child);
		ws_schema_free(&parent);
		ws_fk_free(&fk);
		ws_fk_free(&fks[i]);
		buf_free(&k);
		buf_free(&v);
		buf_free(&sql);
	}
	WsSql->end(level);
	PG_RETURN_TEXT_P(cstring_to_text(out.data));
}
