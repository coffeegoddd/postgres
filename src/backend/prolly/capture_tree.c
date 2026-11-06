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
 * told again is passed over.
 *
 * All that was decoded since the last apply is held in memory.
 *
 * This doesn't yet keep to the tables through DDL, so it is off by default
 * (capture_to_trees).  A table's layout changing while a worker runs is an
 * error, as are a truncate, a table with no primary key, a row with no old key
 * given, and a value decoding wasn't given.  An error stops capture of the whole
 * database, as the worker starts again at the same row.  DDL that leaves the
 * layout as it was, or changes it while no worker runs, isn't noticed.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/capture_tree.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
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
#include "utils/builtins.h"
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
	WsRowShape	shape;
} TreeTable;

/* One row's change, as text: the shape's fields, NULL for a null. */
typedef struct TreeChange
{
	const TreeTable *table;
	int32		branch;
	XLogRecPtr	commit;
	char	  **old_row;		/* the key's fields are its key, if it is to go */
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
tree_start(XLogRecPtr from)
{
	if (tree_tables_cxt == NULL)
	{
		tree_tables_cxt = AllocSetContextCreate(TopMemoryContext, "capture tree tables", ALLOCSET_DEFAULT_SIZES);
		tree_held_cxt = AllocSetContextCreate(TopMemoryContext, "capture tree held", ALLOCSET_DEFAULT_SIZES);
		tree_row_cxt = AllocSetContextCreate(TopMemoryContext, "capture tree row", ALLOCSET_DEFAULT_SIZES);
	}
	elog(DEBUG1, "capture tree starts at %X/%X", LSN_FORMAT_ARGS(from));
}

static void
tree_begin(TransactionId xid, XLogRecPtr final_lsn)
{
	tree_commit = final_lsn;
	tree_scope = WsSql->begin();
}

/* The table known by this relation, which must be as it was when first seen. */
static const TreeTable *
tree_table_of(Relation relation)
{
	WsRowShape	shape;
	ListCell   *lc;
	TreeTable  *table;
	MemoryContext old;

	CaptureRelationShape(relation, &shape);
	foreach(lc, tree_tables)
	{
		table = lfirst(lc);
		if (table->relid != RelationGetRelid(relation))
			continue;
		if (table->shape.nkey != shape.nkey || table->shape.nstored != shape.nstored ||
			memcmp(table->shape.tags, shape.tags, sizeof(uint64) * shape.nstored) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("capture to trees does not follow the layout of \"%s\" changing",
							RelationGetRelationName(relation))));
		ws_row_shape_free(&shape);
		return table;
	}
	if (shape.nkey == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("capture to trees does not hold \"%s\", which has no primary key",
						RelationGetRelationName(relation))));
	ws_row_shape_free(&shape);
	old = MemoryContextSwitchTo(tree_tables_cxt);
	table = palloc_object(TreeTable);
	table->relid = RelationGetRelid(relation);
	CaptureRelationShape(relation, &table->shape);
	tree_tables = lappend(tree_tables, table);
	MemoryContextSwitchTo(old);
	return table;
}

/* A tuple's fields by the table's shape, each in the context apply empties. */
static char **
tree_row(Relation relation, const TreeTable *table, HeapTuple tuple)
{
	TupleDesc	desc = RelationGetDescr(relation);
	char	  **row = MemoryContextAllocZero(tree_held_cxt, sizeof(char *) * Max(table->shape.nstored, 1));
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
		at = ws_row_shape_at(&table->shape, (uint64) att->attcoltag);
		Assert(at >= 0);
		value = heap_getattr(tuple, att->attnum, desc, &isnull);
		if (isnull)
			continue;
		getTypeOutputInfo(att->atttypid, &output, &varlena);
		if (varlena && VARATT_IS_EXTERNAL_ONDISK(DatumGetPointer(value)))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("capture to trees was not given column \"%s\" of \"%s\", which an update left as it was",
							NameStr(att->attname), RelationGetRelationName(relation))));
		row[at] = MemoryContextStrdup(tree_held_cxt, ws_text_to_utf8(OidOutputFunctionCall(output, value)));
		MemoryContextReset(tree_row_cxt);
	}
	MemoryContextSwitchTo(old);
	return row;
}

static void
tree_change(const CaptureChange *change)
{
	const TreeTable *table = tree_table_of(change->relation);
	TreeChange *held = MemoryContextAllocZero(tree_held_cxt, sizeof(TreeChange));
	MemoryContext old;

	if (change->kind == CAPTURE_DELETE && change->old_tuple == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("capture to trees was not given the key of a row deleted from \"%s\"",
						RelationGetRelationName(change->relation))));
	held->table = table;
	held->branch = change->branch;
	held->commit = tree_commit;
	if (change->old_tuple != NULL)
		held->old_row = tree_row(change->relation, table, change->old_tuple);
	if (change->new_tuple != NULL)
		held->new_row = tree_row(change->relation, table, change->new_tuple);
	old = MemoryContextSwitchTo(tree_held_cxt);
	tree_held = lappend(tree_held, held);
	MemoryContextSwitchTo(old);
}

static void
tree_truncate(int32 branch, Relation relation, bool restart_seqs, XLogRecPtr lsn)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("capture to trees does not follow a truncate of \"%s\"", RelationGetRelationName(relation))));
}

static void
tree_message(const char *prefix, const char *data, Size size, bool transactional, XLogRecPtr lsn)
{
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
tree_fields(const TreeTable *table, char **row, WsField *f)
{
	for (int c = 0; c < table->shape.nstored; c++)
	{
		f[c].p = (const uint8 *) row[c];
		f[c].len = row[c] != NULL ? strlen(row[c]) : 0;
		f[c].null = row[c] == NULL;
		f[c].large = false;
	}
}

/* The changes held of one table, not yet applied by its branch's position, on its row map. */
static void
tree_apply_table(ProllyCtx *ctx, VcRefs *refs, const TreeTable *table, List *positions)
{
	WsField    *f = palloc_array(WsField, Max(table->shape.nstored, 1));
	ProllyEdit *edits = palloc_array(ProllyEdit, 2 * list_length(tree_held));
	size_t		n = 0;
	char		name[64];
	PHash		map;
	ListCell   *lc;

	foreach(lc, tree_held)
	{
		TreeChange *change = lfirst(lc);
		Buf			k = BUF_INIT,
					v = BUF_INIT;

		if (change->table != table)
			continue;
		CHECK_FOR_INTERRUPTS();
		if (change->commit <= tree_position_of(positions, change->branch)->before)
		{
			elog(DEBUG1, "capture tree: passed over a change of %X/%X", LSN_FORMAT_ARGS(change->commit));
			continue;
		}
		if (change->old_row != NULL)
		{
			tree_fields(table, change->old_row, f);
			ws_row_store(ctx, &table->shape, f, &k, &v);
			edits[n++] = (ProllyEdit) {.key = k.data, .klen = k.len, .del = true};
			k = (Buf) BUF_INIT;
			v = (Buf) BUF_INIT;
		}
		if (change->new_row != NULL)
		{
			tree_fields(table, change->new_row, f);
			ws_row_store(ctx, &table->shape, f, &k, &v);
			edits[n++] = (ProllyEdit) {.key = k.data, .klen = k.len, .val = v.data, .vlen = v.len};
		}
	}
	if (n == 0)
		return;
	snprintf(name, sizeof(name), "capture/map/%u", table->relid);
	if (!refs->routine->get(refs, name, VC_REF_READ, &map) &&
		prolly_empty(ctx, WS_ROW_VREFS, &map) < 0)
		prolly_report(ctx);
	if (prolly_apply(ctx, &map, edits, n, &map) < 0)
		prolly_report(ctx);
	refs->routine->set(refs, name, &map);
}

static void
tree_apply(void)
{
	ChunkStore *cs;
	VcRefs	   *refs;
	ProllyCtx	ctx;
	List	   *positions = NIL;
	List	   *done = NIL;
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
			known |= position->branch == change->branch;
		if (!known)
		{
			TreePosition *position = palloc_object(TreePosition);

			position->branch = change->branch;
			position->before = position->now = tree_position(change->branch);
			positions = lappend(positions, position);
		}
	}
	foreach(lc, tree_held)
	{
		TreeChange *change = lfirst(lc);
		TreePosition *position = tree_position_of(positions, change->branch);

		if (!list_member_ptr(done, change->table))
		{
			tree_apply_table(&ctx, refs, change->table, positions);
			done = lappend(done, (void *) change->table);
		}
		position->now = Max(position->now, change->commit);
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
};
