/*-------------------------------------------------------------------------
 *
 * move.c
 *	  A branch's versioned schema moved to a root value in place.
 *
 * Writers are held off first, by a lock on each of the schema's tables that only readers get past,
 * and on each sequence, which a reader waits for too.  Then capture's root as of now is what the
 * tables hold, as nothing can change them until this ends, and what differs between it and the
 * target is what's written.  A table kept is written as its rows' differences
 * (prolly/ws/ws_rows.h's apply), a table made with all of its rows, and capture told either's rows
 * are the target's map.  What dropping did to a table is found by reading its definition after,
 * not by working out what depends on what.  A sequence's position is set on a new file of it, as
 * setting it otherwise isn't undone by an error.
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
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_class.h"
#include "common/int.h"
#include "commands/sequence.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "parser/parser.h"
#include "prolly/capture.h"
#include "prolly/move.h"
#include "prolly/ws/ws_fk.h"
#include "prolly/ws/ws_obj.h"
#include "prolly/ws/ws_restore.h"
#include "prolly/ws/ws_root.h"
#include "prolly/ws/ws_rows.h"
#include "prolly/ws/ws_schema.h"
#include "prolly/ws/ws_sql.h"
#include "prolly/ws/ws_viol.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

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

static int
relid_cmp(const ListCell *a, const ListCell *b)
{
	return pg_cmp_u32(lfirst_oid(a), lfirst_oid(b));
}

/*
 * The schema's tables and sequences, each its owner's to move, locked against writers, in order of
 * their ids, as what else locks more than one does: a sequence against its readers too, as one is
 * set on a new file of it.  The tables' and sequences' ids.
 */
typedef struct MoveLocks
{
	Oid			nsp;
	List	   *relations;
} MoveLocks;

static void
lock_writers_out(void *arg)
{
	MoveLocks  *locks = arg;
	Oid			nsp = locks->nsp;
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
		{
			if (!object_ownercheck(RelationRelationId, c->oid, GetUserId()))
				aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(c->relkind), NameStr(c->relname));
			relations = lappend_oid(relations, c->oid);
		}
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	list_sort(relations, relid_cmp);
	foreach_oid(relid, relations)
		LockRelationOid(relid, get_rel_relkind(relid) == RELKIND_SEQUENCE ? AccessExclusiveLock : ExclusiveLock);
	locks->relations = relations;
}

static void
refuse_unbuilt(const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("a move in place that changes %s is not supported", what)));
}

static void
run_sql(Buf *sql)
{
	WsSqlRows	rows;

	buf_append_byte(sql, 0);
	WsSql->run((const char *) sql->data, 0, NULL, &rows);
	ws_sql_rows_free(&rows);
	sql->len = 0;
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
	ResetSequence(relid);
	(void) DirectFunctionCall3(setval3_oid, ObjectIdGetDatum(relid), Int64GetDatum(pos->last_value),
							   BoolGetDatum(pos->is_called));
	pfree(name);
}

/* Each sequence where the target has it, as what's there now has it otherwise. */
static void
move_sequences(ProllyCtx *ctx, const char *schema, const PHash *to, WsMoveStats *stats)
{
	PHash		map;
	WsSeqPosition *now = NULL;
	WsSeqPosition *has = NULL;
	int			nnow = 0;
	int			nhas = 0;
	int			k = 0;

	ws_seq_read(schema, &now, &nnow);
	if (WsRoot->get_map(ctx, to, WS_ROOT_SEQUENCES, &map))
		ws_seq_list(ctx, &map, &has, &nhas);

	/* (both by name in UTF8's order; what the target has and isn't here, its objects made) */
	for (int i = 0; i < nhas; i++)
	{
		while (k < nnow && strcmp(now[k].name, has[i].name) < 0)
			k++;
		if (k < nnow && strcmp(now[k].name, has[i].name) == 0 &&
			now[k].last_value == has[i].last_value && now[k].is_called == has[i].is_called)
			continue;
		move_sequence(schema, &has[i]);
		stats->sequences++;
	}
}

/* A root's objects, each with its bytes, by which one is the same as another. */
typedef struct MoveObjects
{
	WsObject   *objs;
	Buf		   *keys;
	Buf		   *values;
	int			n;
} MoveObjects;

static void
objects_of(ProllyCtx *ctx, const PHash *root, MoveObjects *o)
{
	PHash		map;

	memset(o, 0, sizeof(*o));
	if (WsRoot->get_map(ctx, root, WS_ROOT_OBJECTS, &map))
		ws_obj_list(ctx, &map, &o->objs, &o->n);
	o->keys = palloc0_array(Buf, Max(o->n, 1));
	o->values = palloc0_array(Buf, Max(o->n, 1));
	for (int i = 0; i < o->n; i++)
	{
		WsObjectCodec->encode_key(&o->objs[i], &o->keys[i]);
		WsObjectCodec->encode_value(&o->objs[i], &o->values[i]);
	}
}

static bool
buf_same(const Buf *a, const Buf *b)
{
	return a->len == b->len && memcmp(a->data, b->data, a->len) == 0;
}

/* Whether the other has object i of one as it is. */
static bool
object_kept(const MoveObjects *one, int i, const MoveObjects *other)
{
	for (int k = 0; k < other->n; k++)
		if (buf_same(&one->keys[i], &other->keys[k]))
			return buf_same(&one->values[i], &other->values[k]);
	return false;
}

/* Whether the other has table i of one, by name, with the same definition. */
static bool
table_kept(const MoveTables *one, int i, const MoveTables *other)
{
	for (int k = 0; k < other->n; k++)
		if (strcmp(one->names[i], other->names[k]) == 0)
			return memcmp(one->entries[i].schema.b, other->entries[k].schema.b, PHASH_LEN) == 0;
	return false;
}

static void
drop_table(const char *schema, const char *name)
{
	Buf			sql = BUF_INIT;

	ws_put_text(&sql, "DROP TABLE IF EXISTS ");
	ws_put_identifier(&sql, schema);
	ws_put_text(&sql, ".");
	ws_put_identifier(&sql, ws_name_from_utf8(name));
	ws_put_text(&sql, " CASCADE");
	run_sql(&sql);
	buf_free(&sql);
}

/*
 * What the target doesn't have as the schema has it, dropped, with what depends on it there: its
 * foreign keys, then what's made after tables, then tables, then what's made before them.
 */
static void
drop_what_differs(ProllyCtx *ctx, const char *schema, const PHash *from, const PHash *to,
				  const MoveTables *had, const MoveTables *has)
{
	MoveObjects oa;
	MoveObjects ob;
	PHash		map;
	WsForeignKey *fa = NULL;
	WsForeignKey *fb = NULL;
	int			nfa = 0;
	int			nfb = 0;
	Buf			sql = BUF_INIT;

	if (WsRoot->get_map(ctx, from, WS_ROOT_FOREIGN_KEYS, &map))
		ws_fk_list(ctx, &map, &fa, &nfa);
	if (WsRoot->get_map(ctx, to, WS_ROOT_FOREIGN_KEYS, &map))
		ws_fk_list(ctx, &map, &fb, &nfb);
	for (int i = 0; i < nfa; i++)
	{
		Buf			ka = BUF_INIT,
					va = BUF_INIT;
		bool		kept = false;

		WsFkCodec->encode_key(&fa[i], &ka);
		WsFkCodec->encode_value(&fa[i], &va);
		for (int k = 0; !kept && k < nfb; k++)
		{
			Buf			kb = BUF_INIT,
						vb = BUF_INIT;

			WsFkCodec->encode_key(&fb[k], &kb);
			WsFkCodec->encode_value(&fb[k], &vb);
			kept = buf_same(&ka, &kb) && buf_same(&va, &vb);
			buf_free(&kb);
			buf_free(&vb);
		}
		buf_free(&ka);
		buf_free(&va);
		if (kept)
			continue;
		ws_put_text(&sql, "ALTER TABLE IF EXISTS ");
		ws_put_identifier(&sql, schema);
		ws_put_text(&sql, ".");
		ws_put_identifier(&sql, ws_name_from_utf8(fa[i].table));
		ws_put_text(&sql, " DROP CONSTRAINT IF EXISTS ");
		ws_put_identifier(&sql, ws_name_from_utf8(fa[i].name));
		run_sql(&sql);
	}

	objects_of(ctx, from, &oa);
	objects_of(ctx, to, &ob);
	for (int pass = 0; pass < 3; pass++)
	{
		if (pass == 1)
		{
			for (int i = 0; i < had->n; i++)
				if (!table_kept(had, i, has))
					drop_table(schema, had->names[i]);
			continue;
		}
		for (int i = oa.n - 1; i >= 0; i--)
		{
			if (ws_obj_before_tables(&oa.objs[i]) != (pass == 2) || object_kept(&oa, i, &ob))
				continue;
			ws_obj_drop_statement(&oa.objs[i], schema, &sql);
			run_sql(&sql);
		}
	}
	buf_free(&sql);
}

/*
 * Each table left whose definition is now other than the target's, as dropping what it used took
 * a column, default, constraint or index of it, dropped too, until none is: as it was a table the
 * target keeps, it's made again whole.  Its name is set in gone.
 */
static void
drop_what_changed(ProllyCtx *ctx, const char *schema, const PHash *to, const MoveTables *has, bool *gone)
{
	bool		again = true;

	while (again)
	{
		again = false;
		for (int i = 0; i < has->n; i++)
		{
			WsTableSchema now;
			WsTableSchema want;
			PHash		rows;
			Buf			a = BUF_INIT,
						b = BUF_INIT;
			bool		same;

			if (gone[i])
				continue;
			CHECK_FOR_INTERRUPTS();
			if (!ws_schema_read(schema, ws_name_from_utf8(has->names[i]), &now))
			{
				gone[i] = true;
				continue;
			}
			ws_viol_fold(schema, &now);
			if (!WsRoot->get_table(ctx, to, has->names[i], &want, &rows))
				elog(ERROR, "a root's table \"%s\" is gone", has->names[i]);
			WsSchemaCodec->encode(&now, &a);
			WsSchemaCodec->encode(&want, &b);
			same = buf_same(&a, &b);
			buf_free(&a);
			buf_free(&b);
			ws_schema_free(&now);
			ws_schema_free(&want);
			if (same)
				continue;
			drop_table(schema, has->names[i]);
			gone[i] = true;
			again = true;
		}
	}
}

/* The schema's materialized views that hold rows, by name, to hold them again after. */
static void
populated_read(const char *schema, WsSqlRows *rows)
{
	const char *params[1] = {schema};

	WsSql->run("SELECT format('%I.%I', n.nspname, c.relname) FROM pg_class c "
			   "JOIN pg_namespace n ON n.oid = c.relnamespace "
			   "WHERE n.nspname = $1::name AND c.relkind = 'm' AND c.relispopulated "
			   "ORDER BY c.relname COLLATE \"C\"", 1, params, rows);
}

static void
populated_refresh(WsSqlRows *rows)
{
	for (int r = 0; r < rows->nrows; r++)
	{
		const char *params[1] = {ws_sql_value(rows, r, 0)};
		WsSqlRows	there;

		WsSql->run("SELECT 1 FROM pg_class WHERE oid = to_regclass($1) AND relkind = 'm'", 1, params, &there);
		if (there.nrows > 0)
		{
			Buf			sql = BUF_INIT;

			ws_put_text(&sql, "REFRESH MATERIALIZED VIEW ");
			ws_put_text(&sql, ws_sql_value(rows, r, 0));
			run_sql(&sql);
			buf_free(&sql);
		}
		ws_sql_rows_free(&there);
	}
	ws_sql_rows_free(rows);
}

static bool
has_violations(ProllyCtx *ctx, const PHash *root)
{
	PHash		map;

	return WsRoot->get_map(ctx, root, WS_ROOT_VIOLATIONS, &map);
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
	bool	   *gone;
	WsSqlRows	populated;
	int			level;
	int			nest;
	MoveLocks	locks = {nsp, NIL};

	memset(stats, 0, sizeof(*stats));
	CaptureBarrierCheck(schema);
	CaptureBarrierLeavingOut(ctx, schema, timeout_ms, lock_writers_out, &locks, &from);
	locked = locks.relations;

	/* (what's read and written is as of now, with the writers held off, as the root is) */
	PushActiveSnapshot(GetLatestSnapshot());

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
		char	   *name = ws_name_from_utf8(had.names[i]);

		if (!list_member_oid(locked, get_relname_relid(name, nsp)))
			nlocked = -1;
		pfree(name);
	}
	if (nlocked != had.n)
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("a table of schema \"%s\" was made or dropped as it was being moved", schema),
				 errhint("Try the move again.")));
	if (has_violations(ctx, &from) || has_violations(ctx, to))
		refuse_unbuilt("constraint violations");

	level = WsSql->begin();
	nest = ws_schema_path_begin(schema);
	populated_read(schema, &populated);

	/* 1. What differs dropped, and what that changed of the tables the target keeps */
	drop_what_differs(ctx, schema, &from, to, &had, &has);
	gone = palloc0_array(bool, Max(has.n, 1));
	for (int i = 0; i < has.n; i++)
	{
		bool		kept = false;

		for (int k = 0; !kept && k < had.n; k++)
			kept = strcmp(had.names[k], has.names[i]) == 0 && table_kept(&had, k, &has);
		gone[i] = !kept;
	}
	drop_what_changed(ctx, schema, to, &has, gone);
	CommandCounterIncrement();

	/* 2. The rows of each table kept that differ */
	for (int i = 0; i < has.n; i++)
	{
		WsTableSchema rec;
		PHash		rows;
		uint64		deleted;
		uint64		inserted;
		Oid			relid;
		int			k;

		if (gone[i])
			continue;
		for (k = 0; strcmp(had.names[k], has.names[i]) != 0; k++)
			;
		if (memcmp(had.entries[k].data.b, has.entries[i].data.b, PHASH_LEN) == 0)
			continue;
		if (!WsRoot->get_table(ctx, to, has.names[i], &rec, &rows))
			elog(ERROR, "a root's table \"%s\" is gone", has.names[i]);
		WsRows->apply(ctx, schema, &rec, &had.entries[k].data, &rows, &deleted, &inserted);
		stats->tables++;
		stats->deleted += deleted;
		stats->inserted += inserted;

		/* (a partitioned table has no rows of its own, nor a map of them) */
		{
			char	   *name = ws_name_from_utf8(rec.name);

			relid = get_relname_relid(name, nsp);
			pfree(name);
		}
		if (get_rel_relkind(relid) == RELKIND_RELATION)
		{
			Relation	rel = relation_open(relid, NoLock);

			CaptureRowsAre(rel, &rows);
			relation_close(rel, NoLock);
		}
		ws_schema_free(&rec);
	}

	/* 3. What the target has that isn't here, made, a table with all its rows */
	WsRootIO->restore_missing(ctx, to, &from, schema);
	CommandCounterIncrement();
	for (int i = 0; i < has.n; i++)
	{
		Oid			relid;

		if (!gone[i])
			continue;
		relid = get_relname_relid(ws_name_from_utf8(has.names[i]), nsp);
		if (!OidIsValid(relid))
			elog(ERROR, "table \"%s\" of schema \"%s\" was not made", has.names[i], schema);
		stats->made++;
		if (get_rel_relkind(relid) == RELKIND_RELATION)
		{
			Relation	rel = relation_open(relid, NoLock);

			CaptureRowsAre(rel, &has.entries[i].data);
			relation_close(rel, NoLock);
		}
	}
	for (int i = 0; i < had.n; i++)
	{
		bool		kept = false;

		for (int k = 0; !kept && k < has.n; k++)
			kept = strcmp(had.names[i], has.names[k]) == 0;
		if (!kept)
			stats->dropped++;
	}

	/* 4. Sequences where the target has them, and views that held rows holding the new ones */
	move_sequences(ctx, schema, to, stats);
	populated_refresh(&populated);

	ws_schema_path_end(nest);
	WsSql->end(level);
	PopActiveSnapshot();
	list_free(locked);
	for (int i = 0; i < had.n; i++)
		pfree(had.names[i]);
	for (int i = 0; i < has.n; i++)
		pfree(has.names[i]);
	if (had.n > 0)
	{
		pfree(had.names);
		pfree(had.entries);
	}
	if (has.n > 0)
	{
		pfree(has.names);
		pfree(has.entries);
	}
}

/* An enum's labels as the target makes it, or NIL if it isn't one. */
static List *
enum_labels(const WsObject *o, const char *schema)
{
	Buf			sql = BUF_INIT;
	List	   *parsed;
	Node	   *stmt;

	if (o->kind != WS_OBJ_TYPE)
		return NIL;
	ws_obj_create_statement(o, schema, &sql);
	buf_append_byte(&sql, 0);
	parsed = raw_parser((const char *) sql.data, RAW_PARSE_DEFAULT);
	buf_free(&sql);
	if (list_length(parsed) != 1)
		return NIL;
	stmt = linitial_node(RawStmt, parsed)->stmt;
	if (!IsA(stmt, CreateEnumStmt))
		return NIL;
	return ((CreateEnumStmt *) stmt)->vals;
}

/*
 * Each label the target's enum has and the schema's of its name lacks, added where it goes, if
 * those it has are in the target's order: otherwise it's another type, made again by the move.
 */
static void
add_enum_labels(const WsObject *o, List *want, const char *schema)
{
	const char *params[2] = {schema, ws_name_from_utf8(o->name)};
	WsSqlRows	rows;
	int			k = 0;
	bool		in_order;
	char	   *after = NULL;

	WsSql->run("SELECT e.enumlabel::text FROM pg_enum e JOIN pg_type t ON t.oid = e.enumtypid "
			   "JOIN pg_namespace n ON n.oid = t.typnamespace WHERE n.nspname = $1::name AND t.typname = $2::name "
			   "ORDER BY e.enumsortorder", 2, params, &rows);
	if (rows.nrows == 0)
	{
		ws_sql_rows_free(&rows);
		return;
	}

	/* (what's here, in the target's order: each found in it after the one before) */
	foreach_node(String, label, want)
	{
		if (k < rows.nrows && strcmp(strVal(label), ws_sql_value(&rows, k, 0)) == 0)
			k++;
	}
	in_order = k == rows.nrows;
	k = 0;
	foreach_node(String, label, want)
	{
		Buf			sql = BUF_INIT;

		if (!in_order)
			break;
		if (k < rows.nrows && strcmp(strVal(label), ws_sql_value(&rows, k, 0)) == 0)
		{
			after = strVal(label);
			k++;
			continue;
		}
		ws_put_text(&sql, "ALTER TYPE ");
		ws_put_identifier(&sql, schema);
		ws_put_text(&sql, ".");
		ws_put_identifier(&sql, params[1]);
		ws_put_text(&sql, " ADD VALUE ");
		ws_put_text(&sql, quote_literal_cstr(strVal(label)));
		if (after != NULL)
		{
			ws_put_text(&sql, " AFTER ");
			ws_put_text(&sql, quote_literal_cstr(after));
		}
		else if (k < rows.nrows)
		{
			ws_put_text(&sql, " BEFORE ");
			ws_put_text(&sql, quote_literal_cstr(ws_sql_value(&rows, k, 0)));
		}
		run_sql(&sql);
		buf_free(&sql);
		after = strVal(label);
	}
	ws_sql_rows_free(&rows);
}

static void
inplace_prepare(ProllyCtx *ctx, const char *schema, const PHash *to)
{
	MoveObjects ob;
	int			level = WsSql->begin();
	int			nest = ws_schema_path_begin(schema);

	(void) get_namespace_oid(schema, false);
	objects_of(ctx, to, &ob);
	for (int i = 0; i < ob.n; i++)
	{
		List	   *want = enum_labels(&ob.objs[i], schema);

		if (want != NIL)
			add_enum_labels(&ob.objs[i], want, schema);
	}
	ws_schema_path_end(nest);
	WsSql->end(level);
}

const WsMoveRoutine InPlaceWsMove = {
	.name = "in place",
	.prepare = inplace_prepare,
	.move = inplace_move,
};

const WsMoveRoutine *WsMove = &InPlaceWsMove;
