/*-------------------------------------------------------------------------
 *
 * capture_tree.c
 *	  A CaptureSink keeping each table's rows as a row map.
 *
 * Decoding's callbacks run in a transaction it aborts, under a historic
 * snapshot, so nothing can be written from them.  So a change is held here as
 * its columns' output text, under the same settings as prolly/ws/ws_rows.h's
 * capture reads them, and apply writes what is held in a transaction of its own.
 * The row map of a table is then the one that capture would make of its rows.
 *
 * A table's row map is the reference capture/map/<relid>, as a relation's object
 * id is one branch's.  Each branch's position is a row of
 * pg_history.capture_positions, changed in the transaction that changes its row
 * maps: a transaction whose commit is at or before it has been applied, and if
 * told again is passed over.  That transaction is flushed before the slot moves,
 * whatever synchronous_commit says, or a crash would leave the slot past what it lost.
 *
 * A row map holds only what decoding was told, so only a table made since capture
 * began can have one.  The branch 0 row of pg_history.capture_positions says capture
 * began, and goes before any new slot it makes is kept.  A worker finding none forgets all it
 * holds and marks each table there now in pg_history.capture_tables, and a change to
 * one of those is an error.  So is a table's layout differing from the one recorded
 * there, which outlasts the worker, so capture stays stopped.
 *
 * What belongs to a table or a branch that's gone is let go of when a worker starts,
 * and a change decoded after that brings a table's back until the next start.  The
 * references and tables here don't go in a dump, so a restored store begins again.
 * The chunks only they hold are in a dump like any others, until they're freed.
 *
 * All that was decoded since the last apply is held in memory.
 *
 * What DDL does to a table's rows reaches here as all of its rows, in logical messages at the end
 * of its transaction (capture_rows.c), which void all that was told of the table before them.  So
 * nothing's judged until its transaction has ended: what's wrong with a change only matters if it
 * isn't voided.
 *
 * This is off by default (capture_to_trees), as there's more it doesn't follow.  A truncate is an
 * error, as are a table with no primary key and one whose replica identity doesn't give it, as an
 * update changing the key would look like one that didn't, and a value decoding wasn't given.  An
 * error stops capture of the whole database, as the worker starts again at the same row, and the
 * slot keeps its WAL.  Text that names an object, as a regclass column prints a table's name, isn't
 * followed when the object's renamed.  Nor is capture to trees being turned off for a while and on
 * again, nor an object id used again while its old row map is kept.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/capture_tree.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_type_d.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "prolly/capture.h"
#include "prolly/chunkstore.h"
#include "prolly/engine/prolly.h"
#include "prolly/engine/vc_refs.h"
#include "prolly/ws/ws_schema.h"
#include "prolly/ws/ws_sql.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/injection_point.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_lsn.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "varatt.h"

/* A table's layout, for as long as the worker runs. */
typedef struct TreeTable
{
	Oid			relid;
	WsRowShape *shape;			/* as of what was last told, in tree_tables_cxt */
	List	   *held;			/* its TreeChanges for apply, in tree_held_cxt */
} TreeTable;

/* One row's change, as text: the shape's fields, NULL for a null. */
typedef struct TreeChange
{
	const WsRowShape *shape;
	bool		replace;		/* all before this is void: no row of its own */
	const char *problem;		/* what's wrong with holding it, unless that's voided */
	int32		branch;
	XLogRecPtr	commit;
	char	  **old_row;		/* the key's fields alone: the row to go */
	char	  **new_row;		/* NULL if there is no new row */
} TreeChange;

/* How far a branch was applied before this apply, and how far it now is. */
typedef struct TreePosition
{
	int32		branch;
	XLogRecPtr	before;
	XLogRecPtr	now;
} TreePosition;

/* What is known of each table; lasts as long as the worker. */
static MemoryContext tree_tables_cxt = NULL;
static List *tree_tables = NIL;

/* What apply has yet to write; emptied by it. */
static MemoryContext tree_held_cxt = NULL;
static List *tree_held = NIL;

/* Reset for each row. */
static MemoryContext tree_row_cxt = NULL;

static XLogRecPtr tree_commit = InvalidXLogRecPtr;
static int	tree_scope = 0;

static void
tree_exec(const char *sql, int spi_ok)
{
	if (SPI_execute(sql, false, 0) != spi_ok)
		elog(ERROR, "could not run capture's \"%s\"", sql);
}

static void
tree_forget_ref(void *arg, const char *name, const PHash *hash)
{
	List	  **names = arg;

	*names = lappend(*names, pstrdup(name));
}

/*
 * Forgets all it holds, as what comes next is told from a new start, and marks the tables there
 * now.  One made and committed while this runs is marked too, and refused though its rows would
 * be told: an error, never a wrong map.
 */
static void
tree_begin_anew(VcRefs *refs, XLogRecPtr from)
{
	List	   *names = NIL;
	ListCell   *lc;
	Oid			types[1] = {PG_LSNOID};
	Datum		values[1] = {LSNGetDatum(from)};

	refs->routine->list(refs, "capture/map/", tree_forget_ref, &names);
	foreach(lc, names)
		refs->routine->remove(refs, lfirst(lc));
	tree_exec("DELETE FROM pg_history.capture_positions", SPI_OK_DELETE);
	tree_exec("DELETE FROM pg_history.capture_tables", SPI_OK_DELETE);
	tree_exec("INSERT INTO pg_history.capture_tables (relid) "
			  "SELECT c.oid FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
			  "WHERE n.nspbranch <> 0 AND c.relkind = 'r'", SPI_OK_INSERT);
	if (SPI_execute_with_args("INSERT INTO pg_history.capture_positions VALUES (0, $1)",
							  1, types, values, NULL, false, 0) != SPI_OK_INSERT)
		elog(ERROR, "could not save where capture began");
}

/* Lets go of what belongs to a table or a branch that's gone. */
static void
tree_let_go(VcRefs *refs)
{
	List	   *names = NIL;
	ListCell   *lc;

	if (SPI_execute("SELECT 'capture/map/' || t.relid FROM pg_history.capture_tables t "
					"WHERE NOT EXISTS (SELECT 1 FROM pg_catalog.pg_class c WHERE c.oid = t.relid)",
					true, 0) != SPI_OK_SELECT)
		elog(ERROR, "could not read capture's tables");
	for (uint64 i = 0; i < SPI_processed; i++)
		names = lappend(names, SPI_getvalue(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1));
	foreach(lc, names)
		(void) refs->routine->remove(refs, lfirst(lc));
	tree_exec("DELETE FROM pg_history.capture_tables t "
			  "WHERE NOT EXISTS (SELECT 1 FROM pg_catalog.pg_class c WHERE c.oid = t.relid)", SPI_OK_DELETE);
	tree_exec("DELETE FROM pg_history.capture_positions p WHERE p.branch <> 0 AND "
			  "NOT EXISTS (SELECT 1 FROM pg_catalog.pg_namespace n WHERE n.nspbranch = p.branch)", SPI_OK_DELETE);
}

/* (locally: what's kept doesn't wait on a standby, and the slot moves only once it's flushed) */
static void
tree_settings(void)
{
	SetConfigOption("synchronous_commit", "local", PGC_SUSET, PGC_S_OVERRIDE);
}

/* A new slot is to be made: there's no going on from what's kept. */
static void
tree_slot_to_be_made(void)
{
	tree_settings();
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect to SPI");
	tree_exec("DELETE FROM pg_history.capture_positions WHERE branch = 0", SPI_OK_DELETE);
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "could not finish with SPI");
	PopActiveSnapshot();
	CommitTransactionCommand();
}

static void
tree_start(XLogRecPtr from)
{
	ChunkStore *cs;
	VcRefs	   *refs;

	if (tree_tables_cxt == NULL)
	{
		tree_tables_cxt = AllocSetContextCreate(TopMemoryContext, "capture tree tables", ALLOCSET_DEFAULT_SIZES);
		tree_held_cxt = AllocSetContextCreate(TopMemoryContext, "capture tree held", ALLOCSET_DEFAULT_SIZES);
		tree_row_cxt = AllocSetContextCreate(TopMemoryContext, "capture tree row", ALLOCSET_DEFAULT_SIZES);
	}

	tree_settings();
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect to SPI");
	cs = ChunkStoreOpen("pages");
	refs = VcRefsOpen("pointers", cs);
	tree_exec("SELECT 1 FROM pg_history.capture_positions WHERE branch = 0", SPI_OK_SELECT);
	if (SPI_processed == 0)
		tree_begin_anew(refs, from);
	else
		tree_let_go(refs);
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "could not finish with SPI");
	PopActiveSnapshot();
	CommitTransactionCommand();
	elog(DEBUG1, "capture tree starts at %X/%X", LSN_FORMAT_ARGS(from));
}

static void
tree_begin(TransactionId xid, XLogRecPtr final_lsn)
{
	tree_commit = final_lsn;
	tree_scope = WsSql->begin();
}

static TreeTable *
tree_table_by_id(Oid relid)
{
	ListCell   *lc;

	foreach(lc, tree_tables)
	{
		TreeTable  *table = lfirst(lc);

		if (table->relid == relid)
			return table;
	}
	return NULL;
}

/* The table of this relation, its shape made what it was at this change. */
static TreeTable *
tree_table_of(Relation relation)
{
	WsRowShape	shape;
	TreeTable  *table = tree_table_by_id(RelationGetRelid(relation));
	MemoryContext old;

	CaptureRelationShape(relation, &shape);
	if (table != NULL && table->shape != NULL && table->shape->nkey == shape.nkey &&
		table->shape->nstored == shape.nstored &&
		memcmp(table->shape->tags, shape.tags, sizeof(uint64) * shape.nstored) == 0)
	{
		ws_row_shape_free(&shape);
		return table;
	}
	ws_row_shape_free(&shape);
	old = MemoryContextSwitchTo(tree_tables_cxt);
	if (table == NULL)
	{
		table = palloc0_object(TreeTable);
		table->relid = RelationGetRelid(relation);
		tree_tables = lappend(tree_tables, table);
	}

	/* (the one before is left, for changes held that were made in it) */
	table->shape = palloc_object(WsRowShape);
	CaptureRelationShape(relation, table->shape);
	MemoryContextSwitchTo(old);
	return table;
}

/* What's wrong with holding a change of this relation, or NULL. */
static const char *
tree_problem_of(Relation relation, const TreeTable *table)
{
	if (table->shape->nkey == 0)
		return psprintf("capture to trees does not hold \"%s\", which has no primary key",
						RelationGetRelationName(relation));
	if (relation->rd_rel->relreplident != REPLICA_IDENTITY_FULL &&
		(relation->rd_rel->relreplident != REPLICA_IDENTITY_DEFAULT ||
		 !OidIsValid(RelationGetPrimaryKeyIndex(relation, false))))
		return psprintf("capture to trees does not hold \"%s\", whose replica identity does not give its primary key",
						RelationGetRelationName(relation));
	return NULL;
}

/* A tuple's fields by the table's shape, each in the context apply empties. */
static char **
tree_row(Relation relation, const TreeTable *table, HeapTuple tuple, bool key_only, const char **problem)
{
	TupleDesc	desc = RelationGetDescr(relation);
	char	  **row = MemoryContextAllocZero(tree_held_cxt, sizeof(char *) * Max(table->shape->nstored, 1));
	MemoryContext old = MemoryContextSwitchTo(tree_row_cxt);

	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);
		int			at;
		bool		isnull;
		Datum		value;
		Oid			output;
		bool		varlena;

		if (att->attisdropped || att->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL)
			continue;
		at = ws_row_shape_at(table->shape, (uint64) att->attcoltag);
		Assert(at >= 0);
		if (key_only && at >= table->shape->nkey)
			continue;
		value = heap_getattr(tuple, att->attnum, desc, &isnull);
		if (isnull)
			continue;
		getTypeOutputInfo(att->atttypid, &output, &varlena);
		if (varlena && VARATT_IS_EXTERNAL_ONDISK(DatumGetPointer(value)))
		{
			MemoryContext was = MemoryContextSwitchTo(tree_held_cxt);

			*problem = psprintf("capture to trees was not given column \"%s\" of \"%s\", which an update left as it was",
								NameStr(att->attname), RelationGetRelationName(relation));
			MemoryContextSwitchTo(was);
			continue;
		}
		row[at] = MemoryContextStrdup(tree_held_cxt, ws_text_to_utf8(OidOutputFunctionCall(output, value)));
		MemoryContextReset(tree_row_cxt);
	}
	MemoryContextSwitchTo(old);
	return row;
}

static void
tree_change(const CaptureChange *change)
{
	TreeTable  *table = tree_table_of(change->relation);
	TreeChange *held = MemoryContextAllocZero(tree_held_cxt, sizeof(TreeChange));
	MemoryContext old = MemoryContextSwitchTo(tree_held_cxt);

	held->shape = table->shape;
	held->branch = change->branch;
	held->commit = tree_commit;
	held->problem = tree_problem_of(change->relation, table);
	MemoryContextSwitchTo(old);
	if (held->problem == NULL)
	{
		if (change->kind == CAPTURE_DELETE && change->old_tuple == NULL)
			elog(ERROR, "a row deleted from \"%s\" came with no key", RelationGetRelationName(change->relation));
		if (change->old_tuple != NULL)
			held->old_row = tree_row(change->relation, table, change->old_tuple, true, &held->problem);
		if (change->new_tuple != NULL)
			held->new_row = tree_row(change->relation, table, change->new_tuple, false, &held->problem);
	}
	old = MemoryContextSwitchTo(tree_held_cxt);
	table->held = lappend(table->held, held);
	tree_held = lappend(tree_held, held);
	MemoryContextSwitchTo(old);
}

static void
tree_truncate(int32 branch, Relation relation, bool restart_seqs, XLogRecPtr lsn)
{
	TreeTable  *table = tree_table_of(relation);
	MemoryContext old = MemoryContextSwitchTo(tree_held_cxt);
	TreeChange *held = palloc0_object(TreeChange);

	held->shape = table->shape;
	held->branch = branch;
	held->commit = tree_commit;
	held->problem = psprintf("capture to trees does not follow a truncate of \"%s\"", RelationGetRelationName(relation));
	table->held = lappend(table->held, held);
	tree_held = lappend(tree_held, held);
	MemoryContextSwitchTo(old);
}

/* Reading a message the server wrote: anything else is damage. */
static const char *
tree_take(const char **p, const char *end, Size n)
{
	const char *at = *p;

	if (n > (Size) (end - *p))
		elog(ERROR, "a message of a table's rows is shorter than it says");
	*p += n;
	return at;
}

static int32
read_int32(const char **p, const char *end)
{
	int32		v;

	memcpy(&v, tree_take(p, end, sizeof(v)), sizeof(v));
	return v;
}

/* All that's been told of the table is void, and its rows are those that follow, in this layout. */
static void
tree_rows_begin(const char *p, const char *end)
{
	int32		branch = read_int32(&p, end);
	Oid			relid = (Oid) read_int32(&p, end);
	int32		nkey = read_int32(&p, end);
	int32		nstored = read_int32(&p, end);
	TreeTable  *table = tree_table_by_id(relid);
	MemoryContext old;
	uint64	   *tags;
	TreeChange *held;

	if (nstored < 0 || nstored > WS_MAX_COLS || nkey < 0 || nkey > nstored)
		elog(ERROR, "a message of a table's rows has a layout of %d columns, %d of them the key", nstored, nkey);
	old = MemoryContextSwitchTo(tree_tables_cxt);
	if (table == NULL)
	{
		table = palloc0_object(TreeTable);
		table->relid = relid;
		tree_tables = lappend(tree_tables, table);
	}

	/* (the one before is left, for changes held that were made in it) */
	table->shape = palloc_object(WsRowShape);
	tags = palloc_array(uint64, Max(nstored, 1));
	memcpy(tags, tree_take(&p, end, sizeof(uint64) * nstored), sizeof(uint64) * nstored);
	ws_row_shape_init(table->shape, tags, nstored, nkey);
	pfree(tags);
	MemoryContextSwitchTo(tree_held_cxt);
	held = palloc0_object(TreeChange);
	held->shape = table->shape;
	held->replace = true;
	held->branch = branch;
	held->commit = tree_commit;
	table->held = lappend(table->held, held);
	tree_held = lappend(tree_held, held);
	MemoryContextSwitchTo(old);
}

static void
tree_rows_batch(const char *p, const char *end)
{
	int32		branch = read_int32(&p, end);
	Oid			relid = (Oid) read_int32(&p, end);
	int32		nrows = read_int32(&p, end);
	TreeTable  *table = tree_table_by_id(relid);
	MemoryContext old;

	if (table == NULL || table->shape == NULL)
		elog(ERROR, "rows of relation %u came with nothing before them", relid);
	old = MemoryContextSwitchTo(tree_held_cxt);
	for (int r = 0; r < nrows; r++)
	{
		TreeChange *held = palloc0_object(TreeChange);

		held->shape = table->shape;
		held->branch = branch;
		held->commit = tree_commit;
		held->new_row = palloc0_array(char *, Max(table->shape->nstored, 1));
		for (int c = 0; c < table->shape->nstored; c++)
		{
			int32		len = read_int32(&p, end);

			if (len >= 0)
				held->new_row[c] = pnstrdup(tree_take(&p, end, len), len);
		}
		table->held = lappend(table->held, held);
		tree_held = lappend(tree_held, held);
	}
	MemoryContextSwitchTo(old);
}

static void
tree_message(const char *prefix, const char *data, Size size, bool transactional, XLogRecPtr lsn)
{
	const char *end = data + size;

	if (!transactional || strcmp(prefix, CAPTURE_ROWS_PREFIX) != 0)
		return;
	if (size < 1)
		elog(ERROR, "a message of a table's rows is empty");
	if (data[0] == CAPTURE_ROWS_BEGIN)
		tree_rows_begin(data + 1, end);
	else if (data[0] == CAPTURE_ROWS_BATCH)
		tree_rows_batch(data + 1, end);
	else
		elog(ERROR, "a message of a table's rows of unknown kind %d", data[0]);
}

static void
tree_end(TransactionId xid, XLogRecPtr commit_lsn, XLogRecPtr end_lsn)
{
	WsSql->end(tree_scope);
}

/* How far a branch is applied; invalid if it never has been. */
static XLogRecPtr
tree_position(int32 branch)
{
	Oid			types[1] = {INT4OID};
	Datum		values[1] = {Int32GetDatum(branch)};
	XLogRecPtr	at = InvalidXLogRecPtr;

	if (SPI_execute_with_args("SELECT lsn FROM pg_history.capture_positions WHERE branch = $1",
							  1, types, values, NULL, true, 1) != SPI_OK_SELECT)
		elog(ERROR, "could not read a branch's capture position");
	if (SPI_processed == 1)
	{
		bool		isnull;

		at = DatumGetLSN(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
	}
	return at;
}

static void
tree_set_position(int32 branch, XLogRecPtr at)
{
	Oid			types[2] = {INT4OID, PG_LSNOID};
	Datum		values[2] = {Int32GetDatum(branch), LSNGetDatum(at)};

	if (SPI_execute_with_args("INSERT INTO pg_history.capture_positions VALUES ($1, $2) "
							  "ON CONFLICT (branch) DO UPDATE SET lsn = excluded.lsn",
							  2, types, values, NULL, false, 0) != SPI_OK_INSERT)
		elog(ERROR, "could not save a branch's capture position");
}

static TreePosition *
tree_position_of(List *positions, int32 branch)
{
	ListCell   *lc;

	foreach(lc, positions)
	{
		TreePosition *position = lfirst(lc);

		if (position->branch == branch)
			return position;
	}
	elog(ERROR, "no position read for branch %d", branch);
}

static void
tree_fields(const WsRowShape *shape, char **row, WsField *f)
{
	for (int c = 0; c < shape->nstored; c++)
	{
		f[c].p = (const uint8 *) row[c];
		f[c].len = row[c] != NULL ? strlen(row[c]) : 0;
		f[c].null = row[c] == NULL;
		f[c].large = false;
	}
}

/* Checks a table's layout against what is recorded of it, whichever worker it was that saw it first. */
static void
tree_check_table(const TreeTable *table, const WsRowShape *shape)
{
	Oid			types[3] = {OIDOID, INT4OID, INT8ARRAYOID};
	Datum		values[3];
	Datum	   *tags = palloc_array(Datum, Max(shape->nstored, 1));
	ArrayType  *array;

	for (int i = 0; i < shape->nstored; i++)
		tags[i] = Int64GetDatum((int64) shape->tags[i]);
	array = construct_array_builtin(tags, shape->nstored, INT8OID);
	values[0] = ObjectIdGetDatum(table->relid);
	values[1] = Int32GetDatum(shape->nkey);
	values[2] = PointerGetDatum(array);
	if (SPI_execute_with_args("SELECT nkey IS NULL, nkey = $2 AND tags = $3 FROM pg_history.capture_tables WHERE relid = $1",
							  3, types, values, NULL, true, 1) != SPI_OK_SELECT)
		elog(ERROR, "could not read capture's table %u", table->relid);
	if (SPI_processed == 1)
	{
		bool		isnull;

		if (DatumGetBool(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull)))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("capture to trees does not hold relation %u, which was there before it began", table->relid)));
		if (!DatumGetBool(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &isnull)))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("capture to trees does not follow the layout of relation %u changing", table->relid)));
	}
	pfree(array);
	pfree(tags);
}

static bool
tree_same_shape(const WsRowShape *a, const WsRowShape *b)
{
	return a == b || (a->nkey == b->nkey && a->nstored == b->nstored &&
					  memcmp(a->tags, b->tags, sizeof(uint64) * a->nstored) == 0);
}

/* Records a table's layout as of what's now applied. */
static void
tree_record_table(const TreeTable *table)
{
	Oid			types[3] = {OIDOID, INT4OID, INT8ARRAYOID};
	Datum		values[3];
	Datum	   *tags = palloc_array(Datum, Max(table->shape->nstored, 1));
	ArrayType  *array;

	for (int i = 0; i < table->shape->nstored; i++)
		tags[i] = Int64GetDatum((int64) table->shape->tags[i]);
	array = construct_array_builtin(tags, table->shape->nstored, INT8OID);
	values[0] = ObjectIdGetDatum(table->relid);
	values[1] = Int32GetDatum(table->shape->nkey);
	values[2] = PointerGetDatum(array);
	if (SPI_execute_with_args("INSERT INTO pg_history.capture_tables VALUES ($1, $2, $3) "
							  "ON CONFLICT (relid) DO UPDATE SET nkey = excluded.nkey, tags = excluded.tags",
							  3, types, values, NULL, false, 0) != SPI_OK_INSERT)
		elog(ERROR, "could not record capture's table %u", table->relid);
	pfree(array);
	pfree(tags);
}

/* The changes held of one table, not yet applied by its branch's position, on its row map. */
static void
tree_edits_free(ProllyEdit *edits, size_t n)
{
	for (size_t i = 0; i < n; i++)
	{
		px_free((void *) edits[i].key);
		if (edits[i].val != NULL)
			px_free((void *) edits[i].val);
	}
}

static void
tree_apply_table(ProllyCtx *ctx, VcRefs *refs, TreeTable *table, List *positions)
{
	int			most = 1;
	WsField    *f;
	ProllyEdit *edits;
	size_t		n = 0;
	int			from = -1;
	bool		anew = false;
	const WsRowShape *shape = NULL;
	char		name[64];
	PHash		map;
	ListCell   *lc;

	/* what counts starts at the last told whole, of what's not been applied */
	foreach(lc, table->held)
	{
		TreeChange *change = lfirst(lc);

		if (change->commit <= tree_position_of(positions, change->branch)->before)
		{
			elog(DEBUG1, "capture tree: passed over a change of %X/%X", LSN_FORMAT_ARGS(change->commit));
			continue;
		}
		if (from < 0 || change->replace)
			from = foreach_current_index(lc);
		most = Max(most, change->shape->nstored);
	}
	if (from < 0)
		return;
	for (int i = from; i < list_length(table->held); i++)
	{
		TreeChange *change = list_nth(table->held, i);

		if (change->problem != NULL)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("%s", change->problem)));
		if (shape != NULL && !tree_same_shape(shape, change->shape))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("capture to trees does not follow the layout of relation %u changing", table->relid)));
		shape = change->shape;
	}
	anew = ((TreeChange *) list_nth(table->held, from))->replace;
	if (!anew)
		tree_check_table(table, shape);

	snprintf(name, sizeof(name), "capture/map/%u", table->relid);
	if (shape->nkey == 0)
	{
		/* told whole, with no key: nothing can hold it, and what was held is void */
		(void) refs->routine->remove(refs, name);
		tree_record_table(table);
		return;
	}
	f = palloc_array(WsField, most);
	edits = palloc_array(ProllyEdit, 2 * (list_length(table->held) - from));
	for (int i = from; i < list_length(table->held); i++)
	{
		TreeChange *change = list_nth(table->held, i);
		Buf			k = BUF_INIT,
					v = BUF_INIT;

		CHECK_FOR_INTERRUPTS();
		if (change->old_row != NULL)
		{
			tree_fields(change->shape, change->old_row, f);
			ws_row_store(ctx, change->shape, f, &k, &v);
			buf_free(&v);
			edits[n++] = (ProllyEdit) {.key = k.data, .klen = k.len, .del = true};
			k = (Buf) BUF_INIT;
			v = (Buf) BUF_INIT;
		}
		if (change->new_row != NULL)
		{
			tree_fields(change->shape, change->new_row, f);
			ws_row_store(ctx, change->shape, f, &k, &v);
			edits[n++] = (ProllyEdit) {.key = k.data, .klen = k.len, .val = v.data, .vlen = v.len};
		}
	}
	if (n > 0 || anew)
	{
		if ((anew || !refs->routine->get(refs, name, VC_REF_READ, &map)) &&
			prolly_empty(ctx, WS_ROW_VREFS, &map) < 0)
			prolly_report(ctx);
		if (n > 0 && prolly_apply(ctx, &map, edits, n, &map) < 0)
			prolly_report(ctx);
		refs->routine->set(refs, name, &map);
		tree_record_table(table);
	}
	tree_edits_free(edits, n);
	pfree(edits);
	pfree(f);
}

static void
tree_apply(void)
{
	ChunkStore *cs;
	VcRefs	   *refs;
	ProllyCtx	ctx;
	List	   *positions = NIL;
	ListCell   *lc;

	if (tree_held == NIL)
		return;
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect to SPI");
	cs = ChunkStoreOpen("pages");
	refs = VcRefsOpen("pointers", cs);
	prolly_ctx_init(&ctx, cs, &prolly_default_policy);
	foreach(lc, tree_held)
	{
		TreeChange *change = lfirst(lc);
		bool		known = false;

		foreach_ptr(TreePosition, position, positions)
		{
			if (position->branch == change->branch)
			{
				position->now = Max(position->now, change->commit);
				known = true;
			}
		}
		if (!known)
		{
			TreePosition *position = palloc_object(TreePosition);

			position->branch = change->branch;
			position->before = tree_position(change->branch);
			position->now = Max(position->before, change->commit);
			positions = lappend(positions, position);
		}
	}
	foreach(lc, tree_tables)
	{
		TreeTable  *table = lfirst(lc);

		if (table->held != NIL)
			tree_apply_table(&ctx, refs, table, positions);
		table->held = NIL;
	}
	foreach_ptr(TreePosition, position, positions)
	{
		if (position->now > position->before)
			tree_set_position(position->branch, position->now);
	}
	elog(DEBUG1, "capture tree: applied %d changes", list_length(tree_held));
	prolly_ctx_free(&ctx);
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "could not finish with SPI");
	PopActiveSnapshot();
	CommitTransactionCommand();
	tree_held = NIL;
	MemoryContextReset(tree_held_cxt);
	INJECTION_POINT("capture-applied", NULL);
}

const CaptureSink TreeCaptureSink = {
	.name = "tree",
	.start = tree_start,
	.begin = tree_begin,
	.change = tree_change,
	.truncate = tree_truncate,
	.message = tree_message,
	.commit = tree_end,
	.apply = tree_apply,
	.slot_to_be_made = tree_slot_to_be_made,
};
