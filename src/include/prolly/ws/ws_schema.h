/*-------------------------------------------------------------------------
 *
 * ws_schema.h
 *	  The working-set model's table schemas: a table's definition as
 *	  structured data, the bytes of its schema chunk.
 *
 * A column has a tag, given when it first appears, which survives renames
 * and retypes: rows, and everything else that refers to a column, name it by
 * tag.  Its type is format_type's text under the settings of a statement's
 * scope (prolly/ws/ws_sql.h), which names no cluster-local object; its
 * default or generation expression is stored text (prolly/ws/ws_tags.h),
 * referring to columns by tag.  Names are as they are, not quoted.  Columns
 * are in attribute order; the primary key has its constraint's name, and
 * lists its columns' tags in key order.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/ws/ws_schema.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_WS_SCHEMA_H
#define PROLLY_WS_SCHEMA_H

#include "prolly/engine/util.h"
#include "prolly/ws/ws_row.h"

#define WS_COL_STORED 's'
#define WS_COL_GENERATED_STORED 'g'
#define WS_COL_GENERATED_VIRTUAL 'v'

#define WS_IDENTITY_NONE '\0'
#define WS_IDENTITY_ALWAYS 'a'
#define WS_IDENTITY_BY_DEFAULT 'd'

#define WS_PERSISTENCE_PERMANENT 'p'
#define WS_PERSISTENCE_UNLOGGED 'u'

typedef struct WsColumn
{
	uint64		tag;
	char	   *name;
	char	   *type;
	char		kind;			/* WS_COL_* */
	bool		not_null;
	char		identity;		/* WS_IDENTITY_* */
	char	   *def;			/* default or generation expression, or NULL */
	char	   *collation;		/* qualified and quoted, or NULL: the type's */
} WsColumn;

typedef struct WsTableSchema
{
	char	   *name;
	char		persistence;	/* WS_PERSISTENCE_* */
	int			ncols;
	WsColumn   *cols;			/* attribute order */
	char	   *key_name;		/* the primary key's constraint, or NULL: none */
	int			nkey;
	uint64	   *key;			/* the primary key's tags */
} WsTableSchema;

/*
 * What a schema must be, in order to be written and when it is read: named,
 * its columns' tags and names distinct, a generated column with its
 * expression and no identity, a key named if it has columns, its columns its
 * own, stored, not null and distinct.  NULL if it is so, or what is wrong.
 */
extern const char *ws_schema_problem(const WsTableSchema *s);

/*
 * The schema codec.  The same schema is always the same bytes.  encode raises
 * an error if the schema has a problem; decode raises
 * ERRCODE_DATA_CORRUPTED if the bytes are not a schema's, or the schema they
 * hold has one.  What decode gives is the caller's to ws_schema_free.
 */
typedef struct WsSchemaCodecRoutine
{
	const char *name;
	void		(*encode) (const WsTableSchema *s, Buf *out);
	void		(*decode) (const uint8 *p, size_t len, WsTableSchema *s);
} WsSchemaCodecRoutine;

extern PGDLLIMPORT const WsSchemaCodecRoutine BinaryWsSchemaCodec;

/* The codec the working-set model uses. */
extern PGDLLIMPORT const WsSchemaCodecRoutine *WsSchemaCodec;

extern void ws_schema_free(WsTableSchema *s);

/* A column by tag, or by name, or NULL. */
extern const WsColumn *ws_schema_column(const WsTableSchema *s, uint64 tag);
extern const WsColumn *ws_schema_column_named(const WsTableSchema *s, const char *name);

/*
 * The row codec's shape of the table: the key's columns, then the other
 * stored columns (generated or not) in attribute order, which is COPY's
 * order.  A virtual column has no value.
 */
extern void ws_schema_row_shape(const WsTableSchema *s, WsRowShape *shape);

/*
 * The CREATE TABLE statement for the table, in the named schema, appended to
 * out; each expression's references the names of its table's columns.  Names
 * are quoted as quote_identifier does, under the session's settings: a scope
 * (prolly/ws/ws_sql.h) makes the text the same whoever asks.  An expression
 * referring to a column the table does not have raises
 * ERRCODE_DATA_CORRUPTED.
 */
extern void ws_schema_create_table(const WsTableSchema *s, const char *schema, Buf *out);

#endif							/* PROLLY_WS_SCHEMA_H */
