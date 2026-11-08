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
#include "prolly/move_alter.h"
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
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

/* A root's tables, by name in UTF8's order. */
typedef struct MoveTables
{
	char	  **names;
	VcTable    *entries;
	bool	   *aside;			/* has constraints set aside, here or in the target */
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
		t->aside = t->aside == NULL ? palloc_array(bool, t->cap) : repalloc_array(t->aside, bool, t->cap);
	}
	t->names[t->n] = pstrdup(name);
	t->entries[t->n] = *entry;
	t->aside[t->n] = false;
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
	int		   *by_key;			/* their indexes, by their keys' bytes */
} MoveObjects;

static const Buf *key_sort_keys;

static int
buf_cmp(const Buf *a, const Buf *b)
{
	return key_cmp(a->data, a->len, b->data, b->len);
}

static int
by_key_cmp(const void *a, const void *b)
{
	return buf_cmp(&key_sort_keys[*(const int *) a], &key_sort_keys[*(const int *) b]);
}

static void
objects_index(MoveObjects *o)
{
	o->by_key = palloc_array(int, Max(o->n, 1));
	for (int i = 0; i < o->n; i++)
		o->by_key[i] = i;
	key_sort_keys = o->keys;
	qsort(o->by_key, o->n, sizeof(int), by_key_cmp);
}

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
	objects_index(o);
}

/* The same, of objects read from the catalogs. */
static void
objects_listed(WsObject *objs, int n, MoveObjects *o)
{
	o->objs = objs;
	o->n = n;
	o->keys = palloc0_array(Buf, Max(n, 1));
	o->values = palloc0_array(Buf, Max(n, 1));
	for (int i = 0; i < n; i++)
	{
		WsObjectCodec->encode_key(&objs[i], &o->keys[i]);
		WsObjectCodec->encode_value(&objs[i], &o->values[i]);
	}
	objects_index(o);
}

static void
objects_free(MoveObjects *o)
{
	for (int i = 0; i < o->n; i++)
	{
		buf_free(&o->keys[i]);
		buf_free(&o->values[i]);
		ws_obj_free(&o->objs[i]);
	}
	pfree(o->keys);
	pfree(o->values);
	pfree(o->by_key);
	if (o->objs != NULL)
		pfree(o->objs);
}

/*
 * Whether dropping an object is anything: a configuration's mapping of a token goes with the
 * configuration, and there is no saying IF EXISTS of a configuration to take one off it.
 */
static bool
drop_is_of_something(const WsObject *o, const char *schema)
{
	const char *params[2];
	WsSqlRows	rows;
	bool		there;

	if (o->kind != WS_OBJ_TSMAP)
		return true;
	params[0] = schema;
	params[1] = ws_name_from_utf8(o->name);
	WsSql->run("SELECT 1 FROM pg_ts_config c JOIN pg_namespace n ON n.oid = c.cfgnamespace "
			   "WHERE n.nspname = $1::name AND c.cfgname = $2::name", 2, params, &rows);
	there = rows.nrows > 0;
	ws_sql_rows_free(&rows);
	pfree((char *) params[1]);
	return there;
}

static void
drop_object(const WsObject *o, const char *schema)
{
	Buf			sql = BUF_INIT;

	if (!drop_is_of_something(o, schema))
		return;
	ws_obj_drop_statement(o, schema, &sql);
	run_sql(&sql);
	buf_free(&sql);
}

static bool
buf_same(const Buf *a, const Buf *b)
{
	return bytes_eq(a->data, a->len, b->data, b->len);
}

/* Whether the other has object i of one as it is. */
static bool
object_kept(const MoveObjects *one, int i, const MoveObjects *other)
{
	int			lo = 0;
	int			hi = other->n - 1;

	while (lo <= hi)
	{
		int			mid = (lo + hi) / 2;
		int			k = other->by_key[mid];
		int			c = buf_cmp(&one->keys[i], &other->keys[k]);

		if (c == 0)
			return buf_same(&one->values[i], &other->values[k]);
		if (c < 0)
			hi = mid - 1;
		else
			lo = mid + 1;
	}
	return false;
}

/*
 * Whether the other has table i of one, by name, with the same definition, and neither has
 * constraints set aside of it.
 */
static bool
table_kept(const MoveTables *one, int i, const MoveTables *other)
{
	for (int k = 0; k < other->n; k++)
		if (strcmp(one->names[i], other->names[k]) == 0)
			return memcmp(one->entries[i].schema.b, other->entries[k].schema.b, PHASH_LEN) == 0 &&
				!one->aside[i] && !other->aside[k];
	return false;
}

static void
drop_table(const char *schema, const char *name)
{
	Buf			sql = BUF_INIT;
	char	   *here = ws_name_from_utf8(name);

	ws_put_text(&sql, "DROP TABLE IF EXISTS ");
	ws_put_identifier(&sql, schema);
	ws_put_text(&sql, ".");
	ws_put_identifier(&sql, here);
	pfree(here);
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
				  const MoveTables *had, const MoveTables *has, const bool *altered_had)
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
				if (!table_kept(had, i, has) && !altered_had[i])
					drop_table(schema, had->names[i]);
			continue;
		}
		for (int i = oa.n - 1; i >= 0; i--)
		{
			if (ws_obj_before_tables(&oa.objs[i]) != (pass == 2) || object_kept(&oa, i, &ob))
				continue;
			drop_object(&oa.objs[i], schema);
		}
	}
	objects_free(&oa);
	objects_free(&ob);
	buf_free(&sql);
}

/* Each object here not as the target has it dropped, once; whether any was. */
static bool
drop_objects_once(const char *schema, const MoveObjects *ob)
{
	WsObject   *objs;
	int			nobjs;
	MoveObjects now;
	bool		any = false;

	ws_obj_read(schema, &objs, &nobjs);
	objects_listed(objs, nobjs, &now);
	for (int i = now.n - 1; i >= 0; i--)
	{
		if (object_kept(&now, i, ob))
			continue;
		drop_object(&now.objs[i], schema);
		any = true;
		CommandCounterIncrement();
	}
	objects_free(&now);
	return any;
}

/*
 * Each table and object left that is now other than the target has it, as dropping what it used
 * took a part of it, dropped too, until none is: a column, default, constraint or index of a table,
 * a domain's check, a composite type's attribute, a family's member.  As each was one the target
 * keeps, it's made again whole.  A table's name is set in gone.
 */
static void
drop_what_changed(ProllyCtx *ctx, const char *schema, const PHash *to, const MoveTables *has, bool *gone,
				  const WsTableSchema *was, bool *altered)
{
	MoveObjects ob;
	bool		again = true;

	objects_of(ctx, to, &ob);
	while (again)
	{
		again = false;
		for (int i = 0; i < has->n; i++)
		{
			WsTableSchema rec;
			WsTableSchema want;
			PHash		rows;
			Buf			a = BUF_INIT,
						b = BUF_INIT;
			bool		same;
			char	   *name;

			if (gone[i])
				continue;
			CHECK_FOR_INTERRUPTS();
			name = ws_name_from_utf8(has->names[i]);
			same = ws_schema_read(schema, name, &rec);
			pfree(name);
			if (!same)
			{
				gone[i] = true;
				continue;
			}
			ws_viol_fold(schema, &rec);

			/* (one to be altered is still as it was, until it's altered) */
			if (altered[i])
				want = was[i];
			else if (!WsRoot->get_table(ctx, to, has->names[i], &want, &rows))
				elog(ERROR, "a root's table \"%s\" is gone", has->names[i]);
			WsSchemaCodec->encode(&rec, &a);
			WsSchemaCodec->encode(&want, &b);
			same = buf_same(&a, &b);
			buf_free(&a);
			buf_free(&b);
			ws_schema_free(&rec);
			if (!altered[i])
				ws_schema_free(&want);
			if (same)
				continue;
			drop_table(schema, has->names[i]);
			gone[i] = true;
			altered[i] = false;
			again = true;
		}

		/* (what's here but not as the target has it, made after or before tables) */
		CommandCounterIncrement();
		again |= drop_objects_once(schema, &ob);
	}
	objects_free(&ob);
}

/*
 * The foreign keys of tables kept that are to a table altered with a unique constraint or index
 * dropped, which they may need, dropped: what makes what's missing makes them again.
 */
static void
drop_keys_to_altered(ProllyCtx *ctx, const char *schema, const PHash *from, const MoveTables *has,
					 const WsTableSchema *was, const WsTableSchema *want, const bool *altered)
{
	PHash		map;
	WsForeignKey *fks;
	int			n;
	Buf			sql = BUF_INIT;

	if (!WsRoot->get_map(ctx, from, WS_ROOT_FOREIGN_KEYS, &map))
		return;
	ws_fk_list(ctx, &map, &fks, &n);
	for (int f = 0; f < n; f++)
		for (int i = 0; i < has->n; i++)
		{
			char	   *table;
			char	   *name;

			if (!altered[i] || strcmp(has->names[i], fks[f].ref_table) != 0 ||
				!move_alter_drops_unique(&was[i], &want[i]))
				continue;
			table = ws_name_from_utf8(fks[f].table);
			name = ws_name_from_utf8(fks[f].name);
			ws_put_text(&sql, "ALTER TABLE IF EXISTS ");
			ws_put_identifier(&sql, schema);
			ws_put_text(&sql, ".");
			ws_put_identifier(&sql, table);
			ws_put_text(&sql, " DROP CONSTRAINT IF EXISTS ");
			ws_put_identifier(&sql, name);
			run_sql(&sql);
			pfree(table);
			pfree(name);
		}
	for (int f = 0; f < n; f++)
		ws_fk_free(&fks[f]);
	buf_free(&sql);
}

/* The schema's materialized views that hold rows, by name, to hold them again after. */
static void
populated_read(const char *schema, WsSqlRows *rows)
{
	const char *params[1] = {schema};

	WsSql->run("SELECT c.relname::text FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
			   "WHERE n.nspname = $1::name AND c.relkind = 'm' AND c.relispopulated", 1, params, rows);
}

/*
 * Each that held rows and the target has, holding the rows it's made of now: in the order the
 * target's objects are made, each after what it reads.
 */
static void
populated_refresh(ProllyCtx *ctx, const char *schema, const PHash *to, WsSqlRows *rows)
{
	MoveObjects ob;
	int		   *order;

	if (rows->nrows == 0)
	{
		ws_sql_rows_free(rows);
		return;
	}
	objects_of(ctx, to, &ob);
	order = palloc_array(int, Max(ob.n, 1));
	ws_obj_restore_order(ob.objs, ob.n, order);
	for (int i = 0; i < ob.n; i++)
	{
		const WsObject *o = &ob.objs[order[i]];
		char	   *name;
		bool		held = false;

		if (o->kind != WS_OBJ_MATVIEW)
			continue;
		name = ws_name_from_utf8(o->name);
		for (int r = 0; !held && r < rows->nrows; r++)
			held = strcmp(ws_sql_value(rows, r, 0), name) == 0;
		if (held)
		{
			Buf			sql = BUF_INIT;

			ws_put_text(&sql, "REFRESH MATERIALIZED VIEW ");
			ws_put_identifier(&sql, schema);
			ws_put_text(&sql, ".");
			ws_put_identifier(&sql, name);
			run_sql(&sql);
			buf_free(&sql);
		}
		pfree(name);
	}
	pfree(order);
	objects_free(&ob);
	ws_sql_rows_free(rows);
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

/*
 * The tables of a root that are of a tree, each a partition or child of another, or that another
 * is of, by name, appended.
 */
static void
find_related(ProllyCtx *ctx, const PHash *root, const MoveTables *tables, char ***names, int *n)
{
	for (int i = 0; i < tables->n; i++)
	{
		WsTableSchema rec;
		PHash		rows;

		if (!WsRoot->get_table(ctx, root, tables->names[i], &rec, &rows))
			elog(ERROR, "a root's table \"%s\" is gone", tables->names[i]);
		if (rec.partition_of != NULL || rec.ninherits > 0 || rec.partition != WS_PARTITION_NONE)
		{
			*names = *names == NULL ? palloc_array(char *, rec.ninherits + 2) :
				repalloc_array(*names, char *, *n + rec.ninherits + 2);
			(*names)[(*n)++] = pstrdup(rec.name);
			if (rec.partition_of != NULL)
				(*names)[(*n)++] = pstrdup(rec.partition_of);
			for (int p = 0; p < rec.ninherits; p++)
				(*names)[(*n)++] = pstrdup(rec.inherits[p]);
		}
		ws_schema_free(&rec);
	}
}

static void
aside_mark(MoveTables *t, const char *name)
{
	for (int i = 0; i < t->n; i++)
		if (strcmp(t->names[i], name) == 0)
			t->aside[i] = true;
}

/*
 * Each table that has constraints set aside here, or would have as the target's restored, marked
 * in both: what's set aside is said by the whole of the table's record and its rows, and a key to
 * it waits with it, so the table is made again, as restore sets aside of what it makes.  What the
 * target holds that restore couldn't set aside is refused here, as all of its records are asked.
 */
static void
mark_set_aside(ProllyCtx *ctx, const char *schema, const PHash *to, MoveTables *had, MoveTables *has)
{
	MemoryContext asking = AllocSetContextCreate(CurrentMemoryContext, "move set aside", ALLOCSET_DEFAULT_SIZES);
	MemoryContext was = MemoryContextSwitchTo(asking);
	const char *params[1] = {schema};
	WsSqlRows	here;
	PHash		map;

	WsSql->run("SELECT DISTINCT c.relname::text FROM pg_ws_deferred d JOIN pg_class c ON c.oid = d.defrelid "
			   "JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = $1::name", 1, params, &here);
	for (int i = 0; i < here.nrows; i++)
	{
		char	   *name = ws_name_to_utf8(ws_sql_value(&here, i, 0));

		aside_mark(had, name);
		aside_mark(has, name);
	}
	if (WsRoot->get_map(ctx, to, WS_ROOT_VIOLATIONS, &map))
	{
		WsTableSchema *recs = palloc_array(WsTableSchema, Max(has->n, 1));
		WsDeferrals deferred;
		PHash		rows;

		for (int i = 0; i < has->n; i++)
			if (!WsRoot->get_table(ctx, to, has->names[i], &recs[i], &rows))
				elog(ERROR, "a root's table \"%s\" is gone", has->names[i]);
		ws_viol_defer(ctx, to, recs, has->n, &deferred);
		for (int i = 0; i < deferred.n; i++)
		{
			aside_mark(had, deferred.items[i].table);
			aside_mark(has, deferred.items[i].table);
		}
	}
	MemoryContextSwitchTo(was);
	MemoryContextDelete(asking);
}

/*
 * The schema's tables, holding from, made to hold to; with tell, capture told each table written
 * has the target's rows.  In a transaction left out of capture, the schema's writers held off.
 */
static void
move_tables(ProllyCtx *ctx, const char *schema, Oid nsp, const PHash *from_root, const PHash *to,
			bool tell, WsMoveStats *stats)
{
	PHash		from = *from_root;
	MoveTables	had = {0};
	MoveTables	has = {0};
	bool	   *gone;
	WsSqlRows	populated;
	int			level;
	int			nest;
	bool		definitions;
	bool	   *altered;
	WsTableSchema *was;
	WsTableSchema *want;

	/* (a move to where it is writes nothing at all) */
	if (memcmp(from.b, to->b, PHASH_LEN) == 0)
		return;

	/* (what's read and written is as of now, with the writers held off, as the root is) */
	PushActiveSnapshot(GetLatestSnapshot());

	WsRoot->tables(ctx, &from, move_table_add, &had);
	WsRoot->tables(ctx, to, move_table_add, &has);

	level = WsSql->begin();
	mark_set_aside(ctx, schema, to, &had, &has);

	/* (with the same definitions, only rows and sequences differ) */
	definitions = same_map(ctx, &from, to, WS_ROOT_OBJECTS) && same_map(ctx, &from, to, WS_ROOT_FOREIGN_KEYS) &&
		same_map(ctx, &from, to, WS_ROOT_ROLES) && had.n == has.n;
	for (int i = 0; definitions && i < has.n; i++)
		definitions = strcmp(had.names[i], has.names[i]) == 0 && table_kept(&had, i, &has);
	definitions = !definitions;

	nest = ws_schema_path_begin(schema);
	populated_read(schema, &populated);

	/* 1. What differs dropped, and what that changed of what the target keeps; what can be, altered */
	gone = palloc0_array(bool, Max(has.n, 1));
	altered = palloc0_array(bool, Max(has.n, 1));
	was = palloc0_array(WsTableSchema, Max(has.n, 1));
	want = palloc0_array(WsTableSchema, Max(has.n, 1));
	if (definitions)
	{
		bool	   *altered_had = palloc0_array(bool, Max(had.n, 1));
		char	  **related = NULL;
		int			nrelated = 0;
		char	  **domains;
		int			ndomains = 0;
		MoveObjects ob;

		find_related(ctx, &from, &had, &related, &nrelated);
		find_related(ctx, to, &has, &related, &nrelated);
		objects_of(ctx, to, &ob);
		domains = palloc_array(char *, Max(ob.n, 1));
		for (int i = 0; i < ob.n; i++)
			if (ob.objs[i].kind == WS_OBJ_DOMAIN)
				domains[ndomains++] = pstrdup(ob.objs[i].name);
		objects_free(&ob);
		for (int i = 0; i < has.n; i++)
		{
			bool		kept = false;
			int			k;

			for (k = 0; k < had.n && strcmp(had.names[k], has.names[i]) != 0; k++)
				;
			kept = k < had.n && table_kept(&had, k, &has);
			gone[i] = !kept;
			if (k < had.n && !kept)
			{
				PHash		rows;
				bool		in_tree = false;

				for (int r = 0; !in_tree && r < nrelated; r++)
					in_tree = strcmp(related[r], has.names[i]) == 0;
				if (!WsRoot->get_table(ctx, &from, had.names[k], &was[i], &rows) ||
					!WsRoot->get_table(ctx, to, has.names[i], &want[i], &rows))
					elog(ERROR, "a root's table \"%s\" is gone", has.names[i]);
				altered[i] = !has.aside[i] &&
					move_alterable(&was[i], &want[i], in_tree, (const char *const *) domains, ndomains);
				altered_had[k] = altered[i];
				gone[i] = !altered[i];
			}
		}
		drop_what_differs(ctx, schema, &from, to, &had, &has, altered_had);
		drop_what_changed(ctx, schema, to, &has, gone, was, altered);
		CommandCounterIncrement();

		/* (what alters drop first, as what the target adds may take a name they free) */
		drop_keys_to_altered(ctx, schema, &from, &has, was, want, altered);
		for (int i = 0; i < has.n; i++)
			if (altered[i])
			{
				move_alter_drop(&was[i], &want[i], schema);
				stats->altered++;
			}
		CommandCounterIncrement();

		/* (what the target adds that tables use, before a table altered uses it) */
		WsRootIO->restore_missing(ctx, to, &from, schema, true);
		CommandCounterIncrement();
		for (int i = 0; i < has.n; i++)
			if (altered[i])
				move_alter_add(&was[i], &want[i], schema);
		CommandCounterIncrement();

		pfree(altered_had);
	}

	/* 2. The rows of each table kept that differ */
	for (int i = 0; i < has.n; i++)
	{
		WsTableSchema rec;
		PHash		rows;
		uint64		deleted;
		uint64		inserted;
		uint64		kept;
		Oid			relid;
		int			k;

		if (gone[i])
			continue;
		for (k = 0; strcmp(had.names[k], has.names[i]) != 0; k++)
			;

		/* (one altered has a layout capture is to be told of, whether its rows' bytes changed or not) */
		if (memcmp(had.entries[k].data.b, has.entries[i].data.b, PHASH_LEN) == 0 && !altered[i])
			continue;
		if (!WsRoot->get_table(ctx, to, has.names[i], &rec, &rows))
			elog(ERROR, "a root's table \"%s\" is gone", has.names[i]);
		WsRows->apply(ctx, schema, altered[i] ? &was[i] : &rec, &had.entries[k].data, &rec, &rows,
					  &deleted, &inserted, &kept);
		stats->tables++;
		stats->deleted += deleted;
		stats->inserted += inserted;
		stats->kept += kept;

		/* (a partitioned table has no rows of its own, nor a map of them) */
		{
			char	   *name = ws_name_from_utf8(rec.name);

			relid = get_relname_relid(name, nsp);
			pfree(name);
		}
		if (tell && get_rel_relkind(relid) == RELKIND_RELATION)
		{
			Relation	rel = relation_open(relid, NoLock);

			CaptureRowsAre(rel, &rows);
			relation_close(rel, NoLock);
		}
		ws_schema_free(&rec);
	}

	/* what an altered table's rows needed it to wait for, and then all of it as the target has it */
	for (int i = 0; i < has.n; i++)
		if (altered[i])
		{
			move_alter_after(&was[i], &want[i], schema);
			CommandCounterIncrement();
			move_alter_check(&want[i], schema);
		}

	/* 3. What the target has that isn't here, made, a table with all its rows */
	if (definitions)
		WsRootIO->restore_missing(ctx, to, &from, schema, false);
	CommandCounterIncrement();
	for (int i = 0; i < has.n; i++)
	{
		Oid			relid;
		char	   *name;

		if (!gone[i])
			continue;
		name = ws_name_from_utf8(has.names[i]);
		relid = get_relname_relid(name, nsp);
		pfree(name);
		if (!OidIsValid(relid))
			elog(ERROR, "table \"%s\" of schema \"%s\" was not made", has.names[i], schema);
		stats->made++;
		if (tell && get_rel_relkind(relid) == RELKIND_RELATION)
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
	populated_refresh(ctx, schema, to, &populated);

	ws_schema_path_end(nest);
	WsSql->end(level);
	PopActiveSnapshot();
	for (int i = 0; i < has.n; i++)
		if (altered[i] || was[i].name != NULL)
		{
			ws_schema_free(&was[i]);
			ws_schema_free(&want[i]);
		}
	for (int i = 0; i < had.n; i++)
		pfree(had.names[i]);
	for (int i = 0; i < has.n; i++)
		pfree(has.names[i]);
	if (had.n > 0)
	{
		pfree(had.names);
		pfree(had.entries);
		pfree(had.aside);
	}
	if (has.n > 0)
	{
		pfree(has.names);
		pfree(has.entries);
		pfree(has.aside);
	}
}

static void
inplace_move(ProllyCtx *ctx, const char *schema, const PHash *to, int timeout_ms, WsMoveStats *stats)
{
	Oid			nsp = get_namespace_oid(schema, false);
	List	   *locked;
	PHash		from;
	MoveTables	had = {0};
	int			nlocked = 0;
	MoveLocks	locks = {nsp, NIL};

	memset(stats, 0, sizeof(*stats));
	CaptureBarrierLeavingOut(ctx, schema, timeout_ms, lock_writers_out, &locks, &from);
	locked = locks.relations;

	/* (a table made after the lock was taken, and before the root, would have writers still) */
	WsRoot->tables(ctx, &from, move_table_add, &had);
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
	for (int i = 0; i < had.n; i++)
		pfree(had.names[i]);
	if (had.n > 0)
	{
		pfree(had.names);
		pfree(had.entries);
		pfree(had.aside);
	}

	move_tables(ctx, schema, nsp, &from, to, true, stats);
	list_free(locked);
	CaptureLeftOutSeal();
}

static void
inplace_move_from(ProllyCtx *ctx, const char *schema, const PHash *from, const PHash *to,
				  WsMoveStats *stats)
{
	memset(stats, 0, sizeof(*stats));
	move_tables(ctx, schema, get_namespace_oid(schema, false), from, to, false, stats);
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

	WsSql->run("SELECT 1 FROM pg_type t JOIN pg_namespace n ON n.oid = t.typnamespace "
			   "WHERE n.nspname = $1::name AND t.typname = $2::name AND t.typtype = 'e'", 2, params, &rows);
	if (rows.nrows == 0)
	{
		ws_sql_rows_free(&rows);
		return;
	}
	ws_sql_rows_free(&rows);
	WsSql->run("SELECT e.enumlabel::text FROM pg_enum e JOIN pg_type t ON t.oid = e.enumtypid "
			   "JOIN pg_namespace n ON n.oid = t.typnamespace WHERE n.nspname = $1::name AND t.typname = $2::name "
			   "ORDER BY e.enumsortorder", 2, params, &rows);

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
	objects_free(&ob);
	ws_schema_path_end(nest);
	WsSql->end(level);
}

const WsMoveRoutine InPlaceWsMove = {
	.name = "in place",
	.prepare = inplace_prepare,
	.move = inplace_move,
	.move_from = inplace_move_from,
};

const WsMoveRoutine *WsMove = &InPlaceWsMove;
