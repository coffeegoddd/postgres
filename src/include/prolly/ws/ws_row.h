/*-------------------------------------------------------------------------
 *
 * ws_row.h
 *	  The working-set model's rows: a table's row as a key and a value in
 *	  its prolly tree, and as COPY text.
 *
 * A row is keyed by its primary key's values, each length-prefixed; its
 * value holds the other stored columns that are not NULL as (tag, length,
 * bytes) cells sorted by tag.  NULLs take no space and columns are found by
 * tag, so renaming a column, adding a nullable column without a default,
 * or dropping one that is all NULL leaves every row as it was.  A table
 * without a primary key keys each row by all its cells, after the byte
 * 'r', and its value is the caller's to give (how many such rows there
 * are).  Values are each type's output text.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/ws/ws_row.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_WS_ROW_H
#define PROLLY_WS_ROW_H

#include "prolly/engine/util.h"

/* Postgres's limit on a table's columns */
#define WS_MAX_COLS 1600

/* A stored column's value: its output text, or NULL. */
typedef struct WsField
{
	const uint8 *p;
	size_t		len;
	bool		null;
} WsField;

/* A cell of a row: a stored column's value that is not NULL. */
typedef struct WsCell
{
	uint64		tag;
	const uint8 *p;
	size_t		len;
} WsCell;

typedef struct WsShapeTag
{
	uint64		tag;
	int			at;				/* its place in tags */
} WsShapeTag;

/*
 * What the codec needs of a table: its stored columns' tags, the primary
 * key's first, in key order.  A row's fields are in this order (COPY's).
 */
typedef struct WsRowShape
{
	int			nkey;
	int			nstored;
	uint64	   *tags;
	WsShapeTag *by_tag;			/* the same, sorted by tag */
} WsRowShape;

/*
 * The shape of nstored columns, nkey of them the key.  A tag given twice,
 * or more columns than a table can have, is an error.
 */
extern void ws_row_shape_init(WsRowShape *s, const uint64 *tags, int nstored, int nkey);
extern void ws_row_shape_free(WsRowShape *s);
/* A tag's place among the shape's fields, or -1. */
extern int	ws_row_shape_at(const WsRowShape *s, uint64 tag);

/*
 * The row codec.  Malformed bytes (cells out of order, a length past the
 * end, a key with bytes left over) raise ERRCODE_DATA_CORRUPTED.
 */
typedef struct WsRowCodec
{
	const char *name;
	/* A row's key and value (replacing theirs) from its fields. */
	void		(*encode) (const WsRowShape *s, const WsField *f, Buf *key, Buf *val);
	/*
	 * A row's fields from its key and value, pointing into them.  A cell of a
	 * column the shape no longer has is left out.
	 */
	void		(*decode) (const WsRowShape *s, const uint8 *k, size_t kl,
						   const uint8 *v, size_t vl, WsField *f);
	/* Cells appended to out, sorted by tag (c is sorted in place). */
	void		(*cells_encode) (WsCell *c, int n, Buf *out);
	/* A value's cells, at most WS_MAX_COLS: how many. */
	int			(*cells) (const uint8 *v, size_t vl, WsCell *out);
} WsRowCodec;

extern PGDLLIMPORT const WsRowCodec WsCellsRowCodec;

/* The codec the working-set model uses. */
extern PGDLLIMPORT const WsRowCodec *WsRows;

/* A value's cell of this tag, if it has one. */
extern bool ws_cell_get(const uint8 *v, size_t vl, uint64 tag, const uint8 **p, size_t *len);

/* COPY text: a value escaped, a line split, and a line from a row's fields. */
extern void ws_copy_escape(Buf *b, const uint8 *v, size_t len);

/*
 * A line (no newline) as n fields, unescaped into scratch, which they then
 * point into.  Any other number of fields is an error.
 */
extern void ws_copy_split(const char *line, size_t len, WsField *f, int n, Buf *scratch);
/* The shape's fields as a line, with its newline (replacing line's). */
extern void ws_copy_line(const WsRowShape *s, const WsField *f, Buf *line);

#endif							/* PROLLY_WS_ROW_H */
