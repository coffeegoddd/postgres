/*-------------------------------------------------------------------------
 *
 * ws_root.h
 *	  What a root value holds of a versioned schema: each table's structured
 *	  schema and rows, and maps of everything else.
 *
 * A root value's key says what its entry is, so that no table's name can be
 * taken for anything else:
 *	t/<table>	a table: its schema chunk holds its record (ws_schema.h), its
 *				data is its row map
 *	~obj		object key -> the object's stored text (ws_tags.h)
 *	~seq		sequence name -> its position (ws_seq_encode)
 *	~viol		a merge's constraint violations
 *	~fk			the foreign keys (ws_fk.h)
 *	~roles		the group roles that travel with it (ws_role.h)
 *
 * The maps are in the data of an entry with an empty schema, and a map with
 * nothing in it has no entry, so one state is one root value however it was
 * reached.
 *
 * The columns' tags and names are in the tables' records and nowhere else, so
 * no second copy can disagree with them.  A tag is unique within its table, not
 * across them.
 *
 * Row maps are of WS_ROW_VREFS and maps of vrefs 0.  Merging these roots is this library's, not
 * vc_merge_roots's, which would leave a map's entry with nothing in it.
 *
 * Reads raise ERRCODE_DATA_CORRUPTED for what no writer here makes, as a
 * commit may come from anywhere.  Holds are as prolly/engine/prolly.h says.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/ws/ws_root.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_WS_ROOT_H
#define PROLLY_WS_ROOT_H

#include "prolly/engine/prolly.h"
#include "prolly/engine/vc.h"
#include "prolly/ws/ws_schema.h"

typedef enum WsRootMap
{
	WS_ROOT_OBJECTS,
	WS_ROOT_SEQUENCES,
	WS_ROOT_VIOLATIONS,
	WS_ROOT_FOREIGN_KEYS,
	WS_ROOT_ROLES,
} WsRootMap;

/* Each table's name (UTF8, in name order) and entry; ends early on false. */
typedef bool (*WsRootTableFn) (void *arg, const char *name, const VcTable *entry);

typedef struct WsRootRoutine
{
	const char *name;

	/*
	 * Sets a table's entry, by the name in its record: new, or in place of
	 * what it was.  Raises an error if the record has a problem.
	 */
	void		(*put_table) (ProllyCtx *ctx, const PHash *rv, const WsTableSchema *s,
							  const PHash *rows, PHash *out);

	/* A table's record, the caller's to ws_schema_free; false if it has none. */
	bool		(*get_table) (ProllyCtx *ctx, const PHash *rv, const char *name,
							  WsTableSchema *s, PHash *rows);

	/* False, and out unset, if there is no such table. */
	bool		(*drop_table) (ProllyCtx *ctx, const PHash *rv, const char *name, PHash *out);

	/* Every table; raises an error for an entry that is nothing here. */
	void		(*tables) (ProllyCtx *ctx, const PHash *rv, WsRootTableFn fn, void *arg);

	/* A map's root; false if it holds nothing.  Writes nothing, as a read must. */
	bool		(*get_map) (ProllyCtx *ctx, const PHash *rv, WsRootMap which, PHash *map);

	/* Sets a map, removing its entry if it holds nothing. */
	void		(*put_map) (ProllyCtx *ctx, const PHash *rv, WsRootMap which,
							const PHash *map, PHash *out);
} WsRootRoutine;

extern PGDLLIMPORT const WsRootRoutine MapWsRoot;

/* The implementation in use. */
extern PGDLLIMPORT const WsRootRoutine *WsRoot;

/*
 * A sequence's position as ~seq holds it, one set of bytes for one position:
 * decode raises ERRCODE_DATA_CORRUPTED for any other bytes.
 */
extern void ws_seq_encode(int64 last_value, bool is_called, Buf *out);
extern void ws_seq_decode(const uint8 *p, size_t len, int64 *last_value, bool *is_called);

#endif							/* PROLLY_WS_ROOT_H */
