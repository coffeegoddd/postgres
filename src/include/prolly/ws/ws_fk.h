/*-------------------------------------------------------------------------
 *
 * ws_fk.h
 *	  A versioned schema's foreign keys, as a root value holds them.
 *
 * A key names two tables, and may make a cycle of them, so no one table's
 * record is its home: the root value's ~fk map holds it, by its table and its
 * name.  Its columns are tags, each side's of its own table's, as a tag is
 * unique only within a table.  Restoring adds each with ALTER TABLE, after
 * every table and index.
 *
 * A key to a table outside its schema is the live database's alone: no commit
 * holds it, so reading one, or its table's record, raises
 * ERRCODE_FEATURE_NOT_SUPPORTED.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/ws/ws_fk.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_WS_FK_H
#define PROLLY_WS_FK_H

#include "prolly/engine/prolly.h"
#include "prolly/ws/ws_schema.h"

typedef struct WsForeignKey
{
	char	   *table;			/* the table it is on */
	char	   *name;
	char	   *ref_table;
	int			ncols;
	uint64	   *cols;			/* its table's tags */
	uint64	   *ref_cols;		/* ref_table's tags, as many */
	char		match;			/* FKCONSTR_MATCH_FULL or _SIMPLE */
	char		on_update;		/* FKCONSTR_ACTION_* */
	char		on_delete;		/* FKCONSTR_ACTION_* */
	int			nset;
	uint64	   *set_cols;		/* ON DELETE SET's own list, or none: all of cols */
	bool		deferrable;
	bool		initdeferred;
	bool		not_valid;
} WsForeignKey;

/*
 * What a key must be: its names ones Postgres would not shorten, its columns
 * as many on each side and no more than a key can have, ref_cols distinct, its
 * actions known, a list for ON DELETE SET only with a SET and of cols,
 * initially deferred only if deferrable.  NULL if it is so, or what is wrong.
 */
extern const char *ws_fk_problem(const WsForeignKey *fk);

/*
 * The codec: key is its table and its name, value the rest.  The same key is
 * always the same bytes, and only those are read as it; decode raises
 * ERRCODE_DATA_CORRUPTED otherwise, encode an error if it has a problem.  What
 * decode gives is the caller's to ws_fk_free.
 */
typedef struct WsFkCodecRoutine
{
	const char *name;
	void		(*encode_key) (const WsForeignKey *fk, Buf *out);
	void		(*encode_value) (const WsForeignKey *fk, Buf *out);
	void		(*decode) (const uint8 *key, size_t klen, const uint8 *val, size_t vlen,
						   WsForeignKey *out);
} WsFkCodecRoutine;

extern PGDLLIMPORT const WsFkCodecRoutine BinaryWsFkCodec;

/* The implementation in use. */
extern PGDLLIMPORT const WsFkCodecRoutine *WsFkCodec;

extern void ws_fk_free(WsForeignKey *fk);

/*
 * A table's foreign keys, from the catalogs, through WsSql, by name in UTF8's
 * order, in *out, palloc'd, each the caller's to ws_fk_free.  Raises
 * ERRCODE_FEATURE_NOT_SUPPORTED as ws_fk_refuse does, and if the table is in no
 * versioned schema.
 */
extern void ws_fk_read(const char *schema, const char *table, WsForeignKey **out, int *n);

/*
 * Raises ERRCODE_FEATURE_NOT_SUPPORTED if a table (its object id as text, in a
 * WsSql scope) has a key no commit can hold or the record cannot yet: to a table
 * outside its schema, not enforced, over a period, from a partitioned table's,
 * with its triggers disabled or commented on, or that more than one unique index
 * could serve.
 */
extern void ws_fk_refuse(const char *oid, const char *table);

/* The ~fk map of these, its root in *map: their order does not matter. */
extern void ws_fk_map(ProllyCtx *ctx, const WsForeignKey *fks, int n, PHash *map);

/* The keys of a ~fk map, in *out, palloc'd, each the caller's to ws_fk_free. */
extern void ws_fk_list(ProllyCtx *ctx, const PHash *map, WsForeignKey **out, int *n);

/*
 * The ALTER TABLE adding a key, in the named schema, to a table and from a
 * table whose records are these, appended to out, in the server's encoding.  A
 * column neither has raises ERRCODE_DATA_CORRUPTED, as does anything of the
 * statement, parsed whole, but this one key on this one table.
 */
extern void ws_fk_add_statement(const WsForeignKey *fk, const WsTableSchema *table,
								const WsTableSchema *ref_table, const char *schema, Buf *out);

#endif							/* PROLLY_WS_FK_H */
