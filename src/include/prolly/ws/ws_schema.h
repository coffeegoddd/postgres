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

/* (not temporary: a temporary table is in no versioned schema) */
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
 * in names Postgres would not shorten, its columns' tags and names distinct,
 * a generated column with its expression and no identity, an identity not
 * null, no empty expression or collation, an expression's references well
 * formed, a key named if it has columns, its columns its own, stored, not
 * null and distinct.  NULL if it is so, or what
 * is wrong.
 */
extern const char *ws_schema_problem(const WsTableSchema *s);

/*
 * The schema codec.  The same schema is always the same bytes, and only those
 * bytes are read as it, on any cluster: a schema's strings are UTF8. encode
 * raises an error if the schema has a problem; decode raises
 * ERRCODE_DATA_CORRUPTED if the bytes are not a schema's, or the schema they
 * hold has one, or a string is not UTF8.  What decode gives is the caller's to
 * ws_schema_free.
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
 * order.  A virtual column has no value.  A schema with a problem (one a merge
 * made, not yet checked) raises an error.
 */
extern void ws_schema_row_shape(const WsTableSchema *s, WsRowShape *shape);

/*
 * A column's tag, by the caller's table of them; attempt as ws_tag_derive's.
 * table, column and type are UTF8.
 */
typedef uint64 (*WsSchemaTagFn) (void *arg, const char *table, const char *column,
								 const char *type, int attnum, int attempt);

/* (the tag a new column gets, for callers that keep none) */
extern uint64 ws_schema_derived_tag(void *arg, const char *table, const char *column,
									const char *type, int attnum, int attempt);

/*
 * The schema of a table, from the catalogs, through WsSql: false if the
 * schema has no ordinary table of that name.  What the record cannot yet hold
 * and would lose (inheritance and partitions, storage settings, a key's
 * options, a sequence's, a default using one) raises
 * ERRCODE_FEATURE_NOT_SUPPORTED, and a name past a commit's limit
 * ERRCODE_NAME_TOO_LONG.  A not-null constraint's name and an identity
 * sequence's are not held, and are made again.
 *
 * It locks the table till the transaction ends, and a column renamed or
 * dropped since the transaction's snapshot raises
 * ERRCODE_T_R_SERIALIZATION_FAILURE.  Other objects' names (a type's, a
 * function's in a default) are printed as they are now, so a reader on an old
 * snapshot must hold those still itself.  A sequence a default names only by
 * text has no dependency to see, so the record holds that default as it is.
 * What it gives is the caller's to ws_schema_free.
 */
extern bool ws_schema_read(const char *schema, const char *table, WsSchemaTagFn tag, void *arg,
						   WsTableSchema *out);

/*
 * The CREATE TABLE statement for the table, in the named schema, appended to
 * out, in the server's encoding: text the encoding cannot hold raises an
 * error, as does a name it holds in too many bytes; each expression's
 * references the names of its table's columns.  Names are quoted as
 * quote_identifier does, under the session's settings: a scope
 * (prolly/ws/ws_sql.h) makes the text the same whoever asks.  A commit may
 * have come from anywhere: the whole statement is parsed and compared with the
 * record, and if it is anything but one CREATE TABLE of just these columns,
 * types, clauses, expressions and key, it raises ERRCODE_DATA_CORRUPTED and is
 * not given back.  A schema with a problem raises an error.  What an
 * expression does is not checked: a default runs as it is, on later inserts,
 * with the inserter's privileges.
 */
extern void ws_schema_create_table(const WsTableSchema *s, const char *schema, Buf *out);

#endif							/* PROLLY_WS_SCHEMA_H */
