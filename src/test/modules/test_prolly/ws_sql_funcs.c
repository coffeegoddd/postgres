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

#include "fmgr.h"
#include "prolly/ws/ws_schema.h"
#include "prolly/ws/ws_sql.h"
#include "utils/builtins.h"
#include "varatt.h"

PG_FUNCTION_INFO_V1(test_ws_schema_bytes);
PG_FUNCTION_INFO_V1(test_ws_schema_create);

/*
 * test_ws_schema_bytes(schema text, table text) returns bytea: the table's
 * schema, read from the catalogs and encoded, its new columns' tags derived;
 * NULL if there is no such table
 */
Datum
test_ws_schema_bytes(PG_FUNCTION_ARGS)
{
	char	   *schema = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *table = text_to_cstring(PG_GETARG_TEXT_PP(1));
	WsTableSchema s;
	Buf			b = BUF_INIT;
	bytea	   *out;

	if (!ws_schema_read(schema, table, ws_schema_derived_tag, NULL, &s))
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
 * test_ws_schema_create(bytes bytea, schema text) returns text: the table of
 * a schema's bytes made in the schema; the statement that made it
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
	WsSql->end(level);
	out = cstring_to_text((char *) sql.data);
	ws_schema_free(&s);
	buf_free(&sql);
	PG_RETURN_TEXT_P(out);
}
