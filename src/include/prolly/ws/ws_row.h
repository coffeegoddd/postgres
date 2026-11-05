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
 * A cell's value longer than WS_LARGE_CELL is in a tree of its own, which the
 * cell names with the value's length, so changing part of a large value leaves
 * the rest of it shared.  Whether a value is by the cell depends on nothing but
 * its length, so one value is always the same bytes.  A primary key's values
 * stay in the key, which is what the row is found by.  A row's value starts
 * with a uvarint and the hashes of the trees its cells name, in order of their
 * tags, which storage reads to keep them; a keyless row's start its value,
 * though its key names them.
 *
 * Row maps are of WS_ROW_VREFS.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/ws/ws_row.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_WS_ROW_H
#define PROLLY_WS_ROW_H

#include "prolly/engine/prolly.h"
#include "prolly/engine/util.h"

/* Postgres's limit on a table's columns */
#define WS_MAX_COLS 1600

/* The longest value a cell holds itself; one longer is in a tree. */
#define WS_LARGE_CELL 2048

/* What a row map's leaf values start with. */
#define WS_ROW_VREFS CHUNK_VREFS_COUNTED

/*
 * A uvarint read as read_uvarint does, and false too if it is not the
 * shortest form of its value: the same value is always the same bytes, so
 * that one row or schema does not have many hashes.
 */
static inline bool
ws_read_canonical_uvarint(const uint8 **p, const uint8 *end, uint64 *out)
{
	const uint8 *start = *p;
	size_t		len;

	if (!read_uvarint(p, end, out))
		return false;
	len = (size_t) (*p - start);
	if (len > 1 && start[len - 1] == 0)
		return false;			/* a last byte of no bits: too long */
	if (len == 10 && start[9] > 1)
		return false;			/* bits past the 64th */
	return len <= 10;
}

/*
 * A stored column's value: its output text, or NULL.  If large, p and len are
 * what names its tree: the codec's view, as ws_row_load gives the value.
 */
typedef struct WsField
{
	const uint8 *p;
	size_t		len;
	bool		null;
	bool		large;
} WsField;

/*
 * A cell of a row: a stored column's value that is not NULL.  If large, p and
 * len are what names its tree, equal if and only if the values are.
 */
typedef struct WsCell
{
	uint64		tag;
	const uint8 *p;
	size_t		len;
	bool		large;
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
 * end, a key with bytes left over, a value's cell for a key's column) raise
 * ERRCODE_DATA_CORRUPTED.
 */
typedef struct WsRowCodecRoutine
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
	/*
	 * A keyed row's key as its values (f[0] on, s->nkey of them), pointing
	 * into it.
	 */
	void		(*key_fields) (const WsRowShape *s, const uint8 *k, size_t kl, WsField *f);
	/* A keyless row's key as its cells, at most WS_MAX_COLS: how many. */
	int			(*key_cells) (const uint8 *k, size_t kl, WsCell *out);
	/*
	 * Cells appended to out, sorted by tag (c is sorted in place).  A tag
	 * given twice, or more cells than a table has columns, is an error: what
	 * cells would then refuse to read.
	 */
	void		(*cells_encode) (WsCell *c, int n, Buf *out);
	/* A value's cells, at most WS_MAX_COLS: how many. */
	int			(*cells) (const uint8 *v, size_t vl, WsCell *out);
	/* A value's cell of this tag, if it has one (read only as far as it). */
	bool		(*cell_get) (const uint8 *v, size_t vl, uint64 tag, WsCell *out);
	/* A keyless row's value (replacing it): what the key names, and payload. */
	void		(*keyless_value) (const uint8 *k, size_t kl, const uint8 *payload, size_t plen,
								  Buf *val);
	/* The caller's part of a keyless row's value, pointing into it. */
	void		(*keyless_payload) (const uint8 *k, size_t kl, const uint8 *v, size_t vl,
									const uint8 **payload, size_t *plen);
	/* What names a tree of a value of this length, appended to out. */
	void		(*large_encode) (const PHash *root, uint64 total, Buf *out);
	/* The tree a large cell or field names, and its value's length. */
	void		(*large_decode) (const uint8 *p, size_t len, PHash *root, uint64 *total);
} WsRowCodecRoutine;

extern PGDLLIMPORT const WsRowCodecRoutine CellsWsRowCodec;

/* The codec the working-set model uses. */
extern PGDLLIMPORT const WsRowCodecRoutine *WsRowCodec;

/*
 * Where a large value is.  A store's errors, and a missing chunk, are raised
 * as prolly_report does; a tree that is not this value's is ERRCODE_DATA_CORRUPTED.
 */
typedef struct WsLargeValueRoutine
{
	const char *name;
	void		(*put) (ProllyCtx *ctx, const uint8 *p, size_t len, PHash *root);
	/* The value, its length len, appended to out. */
	void		(*get) (ProllyCtx *ctx, const PHash *root, uint64 len, Buf *out);
} WsLargeValueRoutine;

extern PGDLLIMPORT const WsLargeValueRoutine TreeWsLargeValues;

/* The implementation in use. */
extern PGDLLIMPORT const WsLargeValueRoutine *WsLargeValues;

/* A row's key and value, its large values put in their trees. */
extern void ws_row_store(ProllyCtx *ctx, const WsRowShape *s, const WsField *f, Buf *key, Buf *val);

/*
 * A row's fields, as the values they were: its large values read into held,
 * which the fields then point into, as the others do into k and v.
 */
extern void ws_row_load(ProllyCtx *ctx, const WsRowShape *s, const uint8 *k, size_t kl,
						const uint8 *v, size_t vl, WsField *f, Buf *held);

/* COPY text: a value escaped, a line split, and a line from a row's fields. */
extern void ws_copy_escape(Buf *b, const uint8 *v, size_t len);

/*
 * A line (no newline) as n fields, unescaped into scratch, which they then
 * point into.  Any other number of fields is an error.  (A table with no
 * stored columns has empty lines, of no fields.)
 */
extern void ws_copy_split(const char *line, size_t len, WsField *f, int n, Buf *scratch);
/* The shape's fields as a line, with its newline (replacing line's). */
extern void ws_copy_line(const WsRowShape *s, const WsField *f, Buf *line);

#endif							/* PROLLY_WS_ROW_H */
