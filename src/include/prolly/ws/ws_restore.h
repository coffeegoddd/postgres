/*-------------------------------------------------------------------------
 *
 * ws_restore.h
 *	  A versioned schema as a root value, and a root value as a schema.
 *
 * Capture holds all a schema has: its tables' records and rows, its objects, foreign
 * keys, sequences' positions, and the roles what it has names.  A caller wanting one
 * point in time is in REPEATABLE READ, as each read of the catalogs says.  It marks no
 * role: whoever commits does that (ws_role_mark), and a root naming one unmarked is
 * refused in restoring it here.  A column of a view's row type is refused, as views
 * are made after tables.
 *
 * Restore makes all of it in a schema with nothing in it, in the one order each part
 * needs: the roles; the objects tables use; each table with its tags, then its rows,
 * then its owner, then its indexes; partitions attached, each table before its own; checks not valid,
 * a partition's before its table's; inheriting, and what is only inherited marked so;
 * foreign keys, each table's last name first; comments; each table's grants; the
 * objects that use tables; row security, as forcing
 * it would refuse a role its own rows; the sequences' positions; and what each now
 * prints remembered.  A
 * restored schema captures as the root it came from.
 *
 * A root is trusted, as a remote is.  Restoring one runs its code, with the rights of
 * whoever restores it, as pg_restore does: an index on a table with no owner a commit
 * can hold, a login role's, what ATTACH checks, what planning a foreign key's check
 * folds.  An index on a table with a role for its owner runs as that role, which takes
 * having its rights.  So restoring or merging a branch one does not trust is for a role
 * with rights one does not mind it having, not a superuser.
 *
 * Whether Postgres would have a root at all is more than anything can say of it but
 * Postgres: ws_root_refused restores it and rolls that back.  It is what a merge's
 * result has to pass, and what it refuses is its conflict.  It needs no right to
 * make schemas, and is refused what the role running it could not do of a real one.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/ws/ws_restore.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_WS_RESTORE_H
#define PROLLY_WS_RESTORE_H

#include "prolly/engine/prolly.h"
#include "prolly/ws/ws_sql.h"

typedef struct WsRootIORoutine
{
	const char *name;
	void		(*capture) (ProllyCtx *ctx, const char *schema, PHash *root);

	/*
	 * Makes a root in schema, which must hold nothing: ERRCODE_DUPLICATE_SCHEMA
	 * otherwise.  previous is the root this database was on, whose roles a root's
	 * may have changed, or NULL.
	 */
	void		(*restore) (ProllyCtx *ctx, const PHash *root, const PHash *previous, const char *schema);
} WsRootIORoutine;

extern PGDLLIMPORT const WsRootIORoutine SchemaWsRootIO;

/* The implementation in use. */
extern PGDLLIMPORT const WsRootIORoutine *WsRootIO;

/*
 * Restores a root in a schema made for it, and undoes all of that.  False if that went
 * through; true, with what was wrong in *failure, if the root was refused for
 * something its statements or rows did.  Any other error is raised, as ever.
 */
extern bool ws_root_refused(ProllyCtx *ctx, const PHash *root, WsSqlFailure *failure);

#endif							/* PROLLY_WS_RESTORE_H */
