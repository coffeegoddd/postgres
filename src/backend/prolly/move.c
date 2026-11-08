/*-------------------------------------------------------------------------
 *
 * move.c
 *	  A branch's versioned schema moved to a root value in place.
 *
 * Writers are held off first, by a lock on each of the schema's tables and sequences that only
 * readers get past.  Then capture's root as of now is what the tables hold, as nothing can change
 * them until this ends, and what differs between it and the target is what's written.  A table is
 * written as its rows' differences (prolly/ws/ws_rows.h's apply), and capture told its rows are
 * the target's map.  A sequence's position is set on a new file of it, as setting it otherwise
 * isn't undone by an error.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/move.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "commands/sequence.h"
#include "miscadmin.h"
#include "prolly/capture.h"
#include "prolly/move.h"
#include "prolly/ws/ws_obj.h"
#include "prolly/ws/ws_root.h"
#include "prolly/ws/ws_rows.h"
#include "prolly/ws/ws_schema.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

/* A root's tables, by name in UTF8's order. */
typedef struct MoveTables
{
	char	  **names;
	VcTable    *entries;
	int			n;
	int			cap;
} MoveTables;

static bool
move_table_add(void *arg, const char *name, const VcTable *entry)
{
	MoveTables *t = arg;

	if (t->n == t->cap)
	{
		t->cap = Max(16, t->cap * 2);
		t->names = t->names == NULL ? palloc_array(char *, t->cap) : repalloc_array(t->names, char *, t->cap);
		t->entries = t->entries == NULL ? palloc_array(VcTable, t->cap) : repalloc_array(t->entries, VcTable, t->cap);
	}
	t->names[t->n] = pstrdup(name);
	t->entries[t->n] = *entry;
	t->n++;
	return true;
}

/* The schema's tables and sequences, locked against writers; the tables' and sequences' ids. */
static List *
lock_writers_out(Oid nsp)
{
	List	   *relations = NIL;
	Relation	rel = table_open(RelationRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;

	ScanKeyInit(&key, Anum_pg_class_relnamespace, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(nsp));
	scan = systable_beginscan(rel, InvalidOid, false, NULL, 1, &key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_class c = (Form_pg_class) GETSTRUCT(tup);

		if (c->relkind == RELKIND_RELATION || c->relkind == RELKIND_PARTITIONED_TABLE ||
			c->relkind == RELKIND_SEQUENCE)
			relations = lappend_oid(relations, c->oid);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	foreach_oid(relid, relations)
		LockRelationOid(relid, ExclusiveLock);
	return relations;
}

/* Whether two of a root's maps are the same: both absent, or the same tree. */
static bool
same_map(ProllyCtx *ctx, const PHash *a, const PHash *b, WsRootMap which)
{
	PHash		ma;
	PHash		mb;
	bool		has_a = WsRoot->get_map(ctx, a, which, &ma);
	bool		has_b = WsRoot->get_map(ctx, b, which, &mb);

	return has_a == has_b && (!has_a || memcmp(ma.b, mb.b, PHASH_LEN) == 0);
}

static void
refuse_unbuilt(const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("a move in place that changes %s is not supported", what)));
}

/* A sequence's position as the target has it, on a new file of it, which an error throws away. */
static void
move_sequence(const char *schema, const WsSeqPosition *pos)
{
	char	   *name = ws_name_from_utf8(pos->name);
	Oid			relid = get_relname_relid(name, get_namespace_oid(schema, false));

	if (!OidIsValid(relid) || get_rel_relkind(relid) != RELKIND_SEQUENCE)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("sequence \"%s\" in schema \"%s\" does not exist", name, schema)));
	if (!object_ownercheck(RelationRelationId, relid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_SEQUENCE, name);
	LockRelationOid(relid, AccessExclusiveLock);
	ResetSequence(relid);
	(void) DirectFunctionCall3(setval3_oid, ObjectIdGetDatum(relid), Int64GetDatum(pos->last_value),
							   BoolGetDatum(pos->is_called));
	pfree(name);
}

static int
seq_name_cmp(const WsSeqPosition *a, const WsSeqPosition *b)
{
	return strcmp(a->name, b->name);
}

static void
move_sequences(ProllyCtx *ctx, const char *schema, const PHash *from, const PHash *to, WsMoveStats *stats)
{
	PHash		map;
	WsSeqPosition *had = NULL;
	WsSeqPosition *has = NULL;
	int			nhad = 0;
	int			nhas = 0;

	if (WsRoot->get_map(ctx, from, WS_ROOT_SEQUENCES, &map))
		ws_seq_list(ctx, &map, &had, &nhad);
	if (WsRoot->get_map(ctx, to, WS_ROOT_SEQUENCES, &map))
		ws_seq_list(ctx, &map, &has, &nhas);

	/* (both by name in UTF8's order, and the same sequences, as the objects are the same) */
	if (nhad != nhas)
		refuse_unbuilt("its sequences");
	for (int i = 0; i < nhas; i++)
	{
		if (seq_name_cmp(&had[i], &has[i]) != 0)
			refuse_unbuilt("its sequences");
		if (had[i].last_value == has[i].last_value && had[i].is_called == has[i].is_called)
			continue;
		move_sequence(schema, &has[i]);
		stats->sequences++;
	}
}

static void
inplace_move(ProllyCtx *ctx, const char *schema, const PHash *to, int timeout_ms, WsMoveStats *stats)
{
	Oid			nsp = get_namespace_oid(schema, false);
	List	   *locked;
	PHash		from;
	MoveTables	had = {0};
	MoveTables	has = {0};
	int			nlocked = 0;

	memset(stats, 0, sizeof(*stats));
	locked = lock_writers_out(nsp);
	CaptureBarrierLeavingOut(ctx, schema, timeout_ms, &from);

	WsRoot->tables(ctx, &from, move_table_add, &had);
	WsRoot->tables(ctx, to, move_table_add, &has);

	/* (a table made after the lock was taken, and before the root, would have writers still) */
	foreach_oid(relid, locked)
	{
		if (get_rel_relkind(relid) != RELKIND_SEQUENCE)
			nlocked++;
	}
	for (int i = 0; i < had.n && nlocked == had.n; i++)
	{
		if (!list_member_oid(locked, get_relname_relid(ws_name_from_utf8(had.names[i]), nsp)))
			nlocked = -1;
	}
	if (nlocked != had.n)
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("a table of schema \"%s\" was made or dropped as it was being moved", schema),
				 errhint("Try the move again.")));

	if (!same_map(ctx, &from, to, WS_ROOT_OBJECTS))
		refuse_unbuilt("its objects");
	if (!same_map(ctx, &from, to, WS_ROOT_FOREIGN_KEYS))
		refuse_unbuilt("its foreign keys");
	if (!same_map(ctx, &from, to, WS_ROOT_ROLES))
		refuse_unbuilt("its roles");
	if (!same_map(ctx, &from, to, WS_ROOT_VIOLATIONS))
		refuse_unbuilt("its violations");
	if (had.n != has.n)
		refuse_unbuilt("its tables");
	for (int i = 0; i < has.n; i++)
	{
		if (strcmp(had.names[i], has.names[i]) != 0 ||
			memcmp(had.entries[i].schema.b, has.entries[i].schema.b, PHASH_LEN) != 0)
			refuse_unbuilt("its tables");
	}

	for (int i = 0; i < has.n; i++)
	{
		WsTableSchema rec;
		PHash		rows;
		uint64		deleted;
		uint64		inserted;
		Oid			relid;

		if (memcmp(had.entries[i].data.b, has.entries[i].data.b, PHASH_LEN) == 0)
			continue;
		if (!WsRoot->get_table(ctx, to, has.names[i], &rec, &rows))
			elog(ERROR, "a root's table \"%s\" is gone", has.names[i]);
		WsRows->apply(ctx, schema, &rec, &had.entries[i].data, &rows, &deleted, &inserted);
		stats->tables++;
		stats->deleted += deleted;
		stats->inserted += inserted;

		/* (a partitioned table has no rows of its own, nor a map of them) */
		relid = get_relname_relid(ws_name_from_utf8(rec.name), nsp);
		if (get_rel_relkind(relid) == RELKIND_RELATION)
		{
			Relation	rel = relation_open(relid, NoLock);

			CaptureRowsAre(rel, &rows);
			relation_close(rel, NoLock);
		}
		ws_schema_free(&rec);
	}

	move_sequences(ctx, schema, &from, to, stats);
	list_free(locked);
}

const WsMoveRoutine InPlaceWsMove = {
	.name = "in place",
	.move = inplace_move,
};

const WsMoveRoutine *WsMove = &InPlaceWsMove;
