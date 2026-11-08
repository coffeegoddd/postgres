/*-------------------------------------------------------------------------
 *
 * ws_dbroot.h
 *	  What a branch's root value holds: each of its versioned schemas' roots,
 *	  by the schema's name.
 *
 * A repository is a database's, and a branch holds every versioned schema
 * of it, so what a commit records is the branch's root, as a Dolt commit
 * records its database's.  A schema's own root (prolly/ws/ws_root.h) names
 * no schema, so the name is the branch root's key for it:
 *	s/<schema>	a schema: its data is the schema's root value, its schema
 *				chunk empty
 *
 * A branch root is a root value of the version control layer's table
 * entries, as a schema's is, so the layer's diff of two finds the schemas
 * that differ.  Reads raise ERRCODE_DATA_CORRUPTED for what no writer here
 * makes, as a commit may come from anywhere.  Holds are as
 * prolly/engine/prolly.h says.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/ws/ws_dbroot.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_WS_DBROOT_H
#define PROLLY_WS_DBROOT_H

#include "prolly/engine/prolly.h"

/* Each schema's name (UTF8, in name order) and root; ends early on false. */
typedef bool (*WsDbRootSchemaFn) (void *arg, const char *name, const PHash *root);

typedef struct WsDbRootRoutine
{
	const char *name;

	/* Names are UTF8, as a root's are.  Sets a schema's root: new, or in place of what it was. */
	void		(*put_schema) (ProllyCtx *ctx, const PHash *rv, const char *name,
							   const PHash *root, PHash *out);

	/* A schema's root; false if there is no such schema. */
	bool		(*get_schema) (ProllyCtx *ctx, const PHash *rv, const char *name, PHash *root);

	/* False, and out unset, if there is no such schema. */
	bool		(*drop_schema) (ProllyCtx *ctx, const PHash *rv, const char *name, PHash *out);

	/* Every schema; raises an error for an entry that is nothing here. */
	void		(*schemas) (ProllyCtx *ctx, const PHash *rv, WsDbRootSchemaFn fn, void *arg);
} WsDbRootRoutine;

extern PGDLLIMPORT const WsDbRootRoutine MapWsDbRoot;

/* The implementation in use. */
extern PGDLLIMPORT const WsDbRootRoutine *WsDbRoot;

/*
 * Each schema of a branch root made on branch, which has none of those names: as
 * prolly/ws/ws_restore.h's restore makes one.
 */
extern void ws_dbroot_restore(ProllyCtx *ctx, const PHash *rv, int32 branch);

#endif							/* PROLLY_WS_DBROOT_H */
