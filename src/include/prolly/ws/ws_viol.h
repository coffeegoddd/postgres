/*-------------------------------------------------------------------------
 *
 * ws_viol.h
 *	  Constraint violations: rows of a merged root that its constraints refuse.
 *
 * A merge can make rows no side had, and rows a constraint one side added refuses.
 * Those are not its conflicts: it is finished, and the rows are for someone to fix.
 * Each is found by asking the rows themselves, with no constraint there to refuse them,
 * in a schema made for that and undone:
 *	foreign key		rows whose referenced row is not there
 *	check			rows for which its expression is false
 *	unique			rows sharing their key with another
 *	not null		rows with that column NULL
 * and an exclusion constraint, which only Postgres can apply, without its rows.  A check
 * or foreign key not valid has none, and a domain's constraints are not applied to rows
 * restored, so not here.  Unique keys compare by their collation and their type's
 * equality, and a foreign key by its referenced type's default operator class, whatever
 * an index's own operator class would say.
 *
 * They are held in the root's ~viol map, as keys, each to one byte that says nothing: its
 * table, kind and constraint, each after a zero byte but the first, and for a row a zero byte and its key
 * as a conflict prints it.  So one set is one map, a check and an index of one name are
 * two, and a violation of another kind is another.  Only the root's rows say what they
 * are, so a root is rechecked as it stands, whoever fixed what.
 *
 * A root holding them restores as it is: its rows in their tables, and each constraint
 * violated, and a foreign key on a unique one, set aside with what goes with it (its
 * comment, an index's settings, a replica identity or clustering through it), in
 * pg_ws_deferred, as real tables cannot hold what a unique index refuses and the planner
 * trusts what a check says.  Capture folds them back, so what is restored captures as the
 * root it came from, and rechecks to say which still are violated.  One clears only by
 * being applied again, with the rows fixed (resolve): nothing else does.  It can't yet be
 * dropped or changed while it waits, nor a column it names dropped, which capture then
 * refuses.  A partitioned or inheriting table's violations cannot be set aside yet:
 * restoring one is refused.
 *
 * What a root's rows cannot be, with no constraint on them, is a merge's conflict, and
 * not a violation: a value of no type it now has, two keys alike as 1.0 and 1.00 are, a
 * row outside its partition, an expression that raises for a row.  So is what Postgres
 * refuses of a root with no rows, where no constraint can be violated: a view of a column
 * dropped.  Those are one line, unapplied <what Postgres said>.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/ws/ws_viol.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_WS_VIOL_H
#define PROLLY_WS_VIOL_H

#include "prolly/engine/prolly.h"
#include "prolly/ws/ws_fk.h"
#include "prolly/ws/ws_merge.h"
#include "prolly/ws/ws_schema.h"

typedef struct WsViolation
{
	char	   *table;
	char	   *constraint;
	char	   *kind;			/* foreign key, check, unique, not null, exclusion */
	char	   *rowkey;			/* NULL for a constraint alone */
} WsViolation;

typedef struct WsViolRoutine
{
	const char *name;

	/*
	 * A root as its rows now are, its ~viol map what they violate, in *out; false, with
	 * what was wrong in *why, palloc'd, if it is one no constraint explains.
	 */
	bool		(*recheck) (ProllyCtx *ctx, const PHash *root, const char *schema, PHash *out, char **why);

	/* What a root holds of them, in key order. */
	void		(*list) (ProllyCtx *ctx, const PHash *root, WsViolation **out, int *n);

	/*
	 * Applies again each constraint set aside of a schema, as many as now hold going
	 * from pg_ws_deferred; how many still do not.  A foreign key waits for what it needs.
	 */
	int			(*resolve) (ProllyCtx *ctx, const char *schema);
} WsViolRoutine;

/* A ~viol key's parts, palloc'd; ERRCODE_DATA_CORRUPTED if it has too few. */
extern void ws_viol_key(const uint8 *key, size_t klen, WsViolation *out);

#define WS_DEFERRED_CHECK 'c'
#define WS_DEFERRED_NOT_NULL 'n'
#define WS_DEFERRED_UNIQUE 'u'
#define WS_DEFERRED_EXCLUSION 'x'
#define WS_DEFERRED_INDEX 'i'
#define WS_DEFERRED_FOREIGN_KEY 'f'

/* One set aside: of a record's bytes, or a foreign key's value and key. */
typedef struct WsDeferred
{
	char		kind;			/* WS_DEFERRED_* */
	const char *name;			/* UTF8 */
	Buf			record;
	Buf			key;
} WsDeferred;

typedef struct WsDeferredStore
{
	/* A table's, palloc'd, of *n: at what WsSql reads now. */
	WsDeferred *(*load) (Oid relid, int *n);
	void		(*add) (Oid relid, const WsDeferred *d);
	void		(*forget) (Oid relid, char kind, const char *name);
	/* All of a table's, as it is dropped. */
	void		(*forget_table) (Oid relid);
} WsDeferredStore;

extern const WsDeferredStore *const WsDeferreds;

typedef struct WsDeferredItem
{
	const char *table;
	WsDeferred	deferred;
} WsDeferredItem;

typedef struct WsDeferrals
{
	int			n;
	WsDeferredItem *items;
} WsDeferrals;

/*
 * For restoring a root: what it holds as violated, in *out, and these records of its
 * tables without that.  ERRCODE_FEATURE_NOT_SUPPORTED for a partitioned or inheriting
 * table's.  What it leaves of a record is the restore's to make, and _end remembers the
 * rest once its tables are.
 */
extern void ws_viol_defer(ProllyCtx *ctx, const PHash *root, WsTableSchema *recs, int nrecs, WsDeferrals *out);
extern bool ws_viol_key_deferred(const WsDeferrals *d, const WsForeignKey *fk);
extern void ws_viol_defer_end(const char *schema, const WsDeferrals *d);

/* For capturing a schema: a record of a table, or its keys, with what is set aside of it. */
extern void ws_viol_fold(const char *schema, WsTableSchema *s);
extern void ws_viol_fold_keys(const char *schema, const char *table, WsForeignKey **fks, int *nfks);
extern bool ws_viol_any_deferred(const char *schema);

extern PGDLLIMPORT const WsViolRoutine QueryWsViol;

/* The implementation in use. */
extern PGDLLIMPORT const WsViolRoutine *WsViol;

/*
 * A merge with no conflicts, checked: its root rechecked, or an unapplied conflict added
 * if it is refused.
 */
extern void ws_merge_check(ProllyCtx *ctx, const char *schema, WsMergeResult *result);

#endif							/* PROLLY_WS_VIOL_H */
