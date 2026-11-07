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
 * A new slot begins with a row map of every table, read under the snapshot of the slot's
 * first position, so that map and what's told after it are the whole of the table.  That's one
 * transaction, which ends with the branch 0 row of pg_history.capture_positions saying capture began,
 * and that row goes before any new slot is made.  So a slot is kept only once it's all there; a slot
 * with no such row was made by another sink, and is made again.  A table's layout differing from the
 * one recorded in pg_history.capture_tables, unless told of whole, is an error, which outlasts the
 * worker, so capture stays stopped.
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
 * A table with no key holds its rows by the whole row, with how many there are of it, so a change
 * is one fewer of the old row and one more of the new; CaptureLoggedIdentity has that come whole.
 *
 * An update doesn't send a value it left out of line.  With no key it's in the old row; with one,
 * it's what the row has now, from what's being applied or else the map, in the tree it's already in.
 *
 * This is off by default (capture_to_trees), as there's more it doesn't follow.  An error stops capture
 * of the whole database, as the worker starts again at the same row, and the slot keeps its WAL.
 * What capture_rows.c doesn't find out isn't followed.  Nor is capture to trees being turned off for
 * a while and on again, nor an object id used again while its old row map is kept.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/capture_tree.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "access/tupdesc.h"
#include "access/xact.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "common/hashfn.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "prolly/branch_catalog.h"
#include "prolly/capture.h"
#include "prolly/chunkstore.h"
#include "prolly/engine/prolly.h"
#include "prolly/engine/vc_refs.h"
#include "prolly/ws/ws_schema.h"
#include "prolly/ws/ws_sql.h"
#include "prolly/ws/ws_rows.h"
#include "storage/lmgr.h"
#include "utils/hsearch.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/injection_point.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_lsn.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tuplesort.h"
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
	char	   *as_of;			/* in a transaction that changed catalogs, its table's fingerprint */
	Oid			relid;
	Oid			filenode;
	int32		branch;
	XLogRecPtr	commit;
	char	  **old_row;		/* the key's fields, or with no key all of them: the row to go */
	char	  **new_row;		/* NULL if there is no new row */
	bool		unchanged;		/* new_row has columns an update left where they were */
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

/* Those of the transaction being told whose fingerprints are to be checked at its commit. */
static List *tree_as_of = NIL;

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

/* In a new row, in place of a value an update left where it was. */
static char tree_unchanged_value[1];

/*
 * A tuple's fields by the table's shape, each in the context apply empties.  A value an update
 * left where it was is what old_row has of it, if it has it, or else, if unchanged is given, marked.
 */
static char **
tree_row(Relation relation, const TreeTable *table, HeapTuple tuple, bool key_only, char **old_row, bool *unchanged,
		 const char **problem)
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
			MemoryContext was;

			if (old_row != NULL && old_row[at] != NULL)
			{
				row[at] = old_row[at];
				continue;
			}
			if (unchanged != NULL)
			{
				row[at] = tree_unchanged_value;
				*unchanged = true;
				continue;
			}
			was = MemoryContextSwitchTo(tree_held_cxt);
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
	if (change->catalogs_changed)
	{
		held->relid = RelationGetRelid(change->relation);
		held->filenode = change->relation->rd_rel->relfilenode;
		held->as_of = CaptureRowsFingerprint(held->relid, held->filenode);
		tree_as_of = lappend(tree_as_of, held);
	}
	MemoryContextSwitchTo(old);
	if (change->old_tuple == NULL &&
		(change->kind == CAPTURE_DELETE || (change->kind == CAPTURE_UPDATE && table->shape->nkey == 0)))
		elog(ERROR, "a change of \"%s\" came with no old row", RelationGetRelationName(change->relation));
	if (change->old_tuple != NULL)
		held->old_row = tree_row(change->relation, table, change->old_tuple, table->shape->nkey > 0, NULL, NULL, &held->problem);
	if (change->new_tuple != NULL)
		held->new_row = tree_row(change->relation, table, change->new_tuple, false, held->old_row,
								 table->shape->nkey > 0 ? &held->unchanged : NULL, &held->problem);
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

/* A barrier's marker just told, to be seen to before the next record is read. */
static int32 tree_barrier_branch = 0;
static XLogRecPtr tree_barrier_lsn = InvalidXLogRecPtr;

static void
tree_message(const char *prefix, const char *data, Size size, bool transactional, XLogRecPtr lsn)
{
	const char *end = data + size;

	if (!transactional && strcmp(prefix, CAPTURE_BARRIER_PREFIX) == 0)
	{
		tree_barrier_branch = read_int32(&data, end);
		tree_barrier_lsn = lsn;
		return;
	}
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

/*
 * A row written with catalogs as they weren't at the commit, and no change of a table's fingerprint
 * by then to have its rows told: an enum's value renamed and renamed back, with a row between.  It
 * prints as it did then, and is an error, unless voided.
 */
static void
tree_check_as_of_commit(void)
{
	ListCell   *lc;

	foreach(lc, tree_as_of)
	{
		TreeChange *change = lfirst(lc);

		if (change->problem == NULL)
		{
			HeapTuple	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(change->relid));

			if (HeapTupleIsValid(tup))
			{
				char	   *now = CaptureRowsFingerprint(change->relid, ((Form_pg_class) GETSTRUCT(tup))->relfilenode);

				ReleaseSysCache(tup);

				/* (one it's still different from at the commit has its rows told, voiding this) */
				if (strcmp(now, change->as_of) != 0)
					change->problem = MemoryContextStrdup(tree_held_cxt,
														  psprintf("capture to trees has a row of relation %u written as catalogs were during its transaction and aren't now",
																   change->relid));
				pfree(now);
			}
		}
	}
	tree_as_of = NIL;
}

static void
tree_end(TransactionId xid, XLogRecPtr commit_lsn, XLogRecPtr end_lsn)
{
	tree_check_as_of_commit();
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
		bool		none = row[c] == NULL || row[c] == tree_unchanged_value;

		f[c].p = (const uint8 *) row[c];
		f[c].len = none ? 0 : strlen(row[c]);
		f[c].null = none;
		f[c].large = false;
	}
}

/* What's being applied to a table, by key: each key's latest edit. */
typedef struct TreeEditKey
{
	const uint8 *p;
	size_t		len;
} TreeEditKey;

typedef struct TreeLatestEdit
{
	TreeEditKey key;
	size_t		edit;
} TreeLatestEdit;

static uint32
tree_edit_key_hash(const void *key, Size keysize)
{
	const TreeEditKey *k = key;

	return hash_bytes(k->p, (int) k->len);
}

static int
tree_edit_key_match(const void *a, const void *b, Size keysize)
{
	const TreeEditKey *x = a;
	const TreeEditKey *y = b;

	return x->len == y->len && memcmp(x->p, y->p, x->len) == 0 ? 0 : 1;
}

static HTAB *
tree_latest_edits(void)
{
	HASHCTL		ctl = {
		.keysize = sizeof(TreeEditKey),
		.entrysize = sizeof(TreeLatestEdit),
		.hash = tree_edit_key_hash,
		.match = tree_edit_key_match,
		.hcxt = CurrentMemoryContext,
	};

	return hash_create("capture edits by key", 256, &ctl, HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_CONTEXT);
}

/* A new row's fields, with what an update left where it was from the row it has now, under key. */
static void
tree_fill_unchanged(ProllyCtx *ctx, const TreeTable *table, const TreeChange *change, const PHash *map,
					HTAB *latest, const ProllyEdit *edits, const uint8 *key, size_t klen, WsField *f)
{
	TreeEditKey look = {key, klen};
	TreeLatestEdit *found = hash_search(latest, &look, HASH_FIND, NULL);
	const uint8 *now = NULL;
	size_t		nowlen = 0;
	uint8	   *copy;
	WsField    *had = palloc_array(WsField, Max(change->shape->nstored, 1));

	if (found != NULL)
	{
		if (!edits[found->edit].del)
		{
			now = edits[found->edit].val;
			nowlen = edits[found->edit].vlen;
		}
	}
	else
	{
		int			rc = prolly_get(ctx, map, key, klen, &now, &nowlen);

		if (rc < 0)
			prolly_report(ctx);
		if (rc != PROLLY_OK)
			now = NULL;
	}
	if (now == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("the row map of relation %u has no row for an update that left a value as it was", table->relid)));

	/* (what prolly_get gives lasts until the next it's asked, and these go on to be stored) */
	copy = palloc(Max(nowlen, 1));
	memcpy(copy, now, nowlen);
	WsRowCodec->decode(change->shape, key, klen, copy, nowlen, had);
	for (int c = 0; c < change->shape->nstored; c++)
	{
		if (change->new_row[c] != tree_unchanged_value)
			continue;
		if (had[c].null)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("the row map of relation %u has no value for one an update left as it was", table->relid)));
		f[c] = had[c];
	}
	pfree(had);
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
	if (SPI_execute_with_args("SELECT nkey = $2 AND tags = $3 FROM pg_history.capture_tables WHERE relid = $1",
							  3, types, values, NULL, true, 1) != SPI_OK_SELECT)
		elog(ERROR, "could not read capture's table %u", table->relid);
	if (SPI_processed == 1)
	{
		bool		isnull;

		if (!DatumGetBool(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull)))
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
tree_record_shape(Oid relid, const WsRowShape *shape)
{
	Oid			types[3] = {OIDOID, INT4OID, INT8ARRAYOID};
	Datum		values[3];
	Datum	   *tags = palloc_array(Datum, Max(shape->nstored, 1));
	ArrayType  *array;

	for (int i = 0; i < shape->nstored; i++)
		tags[i] = Int64GetDatum((int64) shape->tags[i]);
	array = construct_array_builtin(tags, shape->nstored, INT8OID);
	values[0] = ObjectIdGetDatum(relid);
	values[1] = Int32GetDatum(shape->nkey);
	values[2] = PointerGetDatum(array);
	if (SPI_execute_with_args("INSERT INTO pg_history.capture_tables VALUES ($1, $2, $3) "
							  "ON CONFLICT (relid) DO UPDATE SET nkey = excluded.nkey, tags = excluded.tags",
							  3, types, values, NULL, false, 0) != SPI_OK_INSERT)
		elog(ERROR, "could not record capture's table %u", relid);
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
tree_apply_context(void *arg)
{
	errcontext("capture applying changes to table %s", (const char *) arg);
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
	HTAB	   *latest = NULL;

	/* (one that's gone has nothing to hold, whatever was wrong with holding it) */
	if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(table->relid)))
		return;

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
	if ((anew || !refs->routine->get(refs, name, VC_REF_READ, &map)) &&
		prolly_empty(ctx, WS_ROW_VREFS, &map) < 0)
		prolly_report(ctx);
	for (int i = from; i < list_length(table->held); i++)
	{
		if (((TreeChange *) list_nth(table->held, i))->unchanged)
		{
			latest = tree_latest_edits();
			break;
		}
	}
	f = palloc_array(WsField, most);
	edits = palloc_array(ProllyEdit, 2 * (list_length(table->held) - from));
	for (int i = from; i < list_length(table->held); i++)
	{
		TreeChange *change = list_nth(table->held, i);
		Buf			k = BUF_INIT,
					v = BUF_INIT;
		size_t		first = n;

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
			if (change->unchanged)
			{
				/* (the row is found by the key it had, which a key that changed isn't) */
				if (change->old_row != NULL)
					tree_fill_unchanged(ctx, table, change, &map, latest, edits, edits[n - 1].key, edits[n - 1].klen, f);
				else
				{
					ws_row_store(ctx, change->shape, f, &k, &v);
					tree_fill_unchanged(ctx, table, change, &map, latest, edits, k.data, k.len, f);
				}
			}
			ws_row_store_kept(ctx, change->shape, f, &k, &v);
			edits[n++] = (ProllyEdit) {.key = k.data, .klen = k.len, .val = v.data, .vlen = v.len};
		}
		if (latest != NULL)
		{
			for (size_t e = first; e < n; e++)
			{
				TreeEditKey key = {edits[e].key, edits[e].klen};
				TreeLatestEdit *entry = hash_search(latest, &key, HASH_ENTER, NULL);

				entry->edit = e;
			}
		}
	}
	if (latest != NULL)
		hash_destroy(latest);
	if (n > 0 || anew)
	{
		if (shape->nkey == 0)
		{
			char	   *relname = get_rel_name(table->relid);
			ErrorContextCallback context = {
				.previous = error_context_stack,
				.callback = tree_apply_context,
				.arg = psprintf("%s of relation %u", relname != NULL ? relname : "(gone)", table->relid),
			};

			error_context_stack = &context;
			n = ws_rows_keyless_merge(ctx, &map, edits, n);
			error_context_stack = context.previous;
		}
		if (n > 0 && prolly_apply(ctx, &map, edits, n, &map) < 0)
			prolly_report(ctx);
		refs->routine->set(refs, name, &map);
		tree_record_shape(table->relid, table->shape);
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

/*
 * A table's rows being made into its map: sorted by key first, as written in a batch at a time as
 * they lie in the table, every tree between would be written too.
 */
typedef struct TreeRebuild
{
	ProllyCtx  *ctx;
	const WsRowShape *shape;
	WsField    *f;
	Tuplesortstate *sort;
	TupleTableSlot *slot;
} TreeRebuild;

#define TREE_REBUILD_BATCH_ROWS 1000
#define TREE_REBUILD_BATCH_BYTES (8 * 1024 * 1024)

static bytea *
tree_bytea(const uint8 *p, size_t len)
{
	bytea	   *b = palloc(VARHDRSZ + len);

	SET_VARSIZE(b, VARHDRSZ + len);
	memcpy(VARDATA(b), p, len);
	return b;
}

static void
tree_rebuild_row(void *arg, const char **fields)
{
	TreeRebuild *b = arg;
	Buf			k = BUF_INIT,
				v = BUF_INIT;
	bytea	   *key;
	bytea	   *val;

	for (int c = 0; c < b->shape->nstored; c++)
	{
		b->f[c].p = (const uint8 *) fields[c];
		b->f[c].len = fields[c] != NULL ? strlen(fields[c]) : 0;
		b->f[c].null = fields[c] == NULL;
		b->f[c].large = false;
	}
	ws_row_store(b->ctx, b->shape, b->f, &k, &v);
	key = tree_bytea(k.data, k.len);
	val = tree_bytea(v.data, v.len);
	buf_free(&k);
	buf_free(&v);
	ExecClearTuple(b->slot);
	b->slot->tts_values[0] = PointerGetDatum(key);
	b->slot->tts_values[1] = PointerGetDatum(val);
	b->slot->tts_isnull[0] = b->slot->tts_isnull[1] = false;
	ExecStoreVirtualTuple(b->slot);
	tuplesort_puttupleslot(b->sort, b->slot);
	pfree(key);
	pfree(val);
}

/* What's sorted, in key order, on the map, a bounded batch at a time. */
static void
tree_rebuild_write(ProllyCtx *ctx, TreeRebuild *b, PHash *map)
{
	MemoryContext batch_cxt = AllocSetContextCreate(CurrentMemoryContext, "capture rebuild batch", ALLOCSET_DEFAULT_SIZES);
	ProllyEdit *edits = palloc_array(ProllyEdit, TREE_REBUILD_BATCH_ROWS);
	int			n = 0;
	Size		bytes = 0;
	bool		more;

	tuplesort_performsort(b->sort);
	do
	{
		more = tuplesort_gettupleslot(b->sort, true, false, b->slot, NULL);
		if (more)
		{
			bool		isnull;
			bytea	   *key = DatumGetByteaPP(slot_getattr(b->slot, 1, &isnull));
			bytea	   *val = DatumGetByteaPP(slot_getattr(b->slot, 2, &isnull));
			size_t		klen = VARSIZE_ANY_EXHDR(key);
			size_t		vlen = VARSIZE_ANY_EXHDR(val);
			uint8	   *kc = MemoryContextAlloc(batch_cxt, Max(klen, 1));
			uint8	   *vc = MemoryContextAlloc(batch_cxt, Max(vlen, 1));

			memcpy(kc, VARDATA_ANY(key), klen);
			memcpy(vc, VARDATA_ANY(val), vlen);
			edits[n++] = (ProllyEdit) {.key = kc, .klen = klen, .val = vc, .vlen = vlen};
			bytes += klen + vlen;
		}
		if (n > 0 && (!more || n == TREE_REBUILD_BATCH_ROWS || bytes >= TREE_REBUILD_BATCH_BYTES))
		{
			if (b->shape->nkey == 0)
			{
				MemoryContext old = MemoryContextSwitchTo(batch_cxt);

				n = ws_rows_keyless_merge(ctx, map, edits, n);
				MemoryContextSwitchTo(old);
			}
			if (n > 0 && prolly_apply(ctx, map, edits, n, map) < 0)
				prolly_report(ctx);
			n = 0;
			bytes = 0;
			MemoryContextReset(batch_cxt);
		}
		CHECK_FOR_INTERRUPTS();
	} while (more);
	pfree(edits);
	MemoryContextDelete(batch_cxt);
}

/* A table's row map, of its rows in the transaction's snapshot. */
static void
tree_rebuild_table(ProllyCtx *ctx, VcRefs *refs, Oid relid)
{
	Relation	rel;
	WsRowShape	shape;
	char	   *fingerprint;
	Oid			types[2] = {OIDOID, TEXTOID};
	Datum		values[2];
	TreeRebuild b = {.ctx = ctx,.shape = &shape};
	TupleDesc	desc = CreateTemplateTupleDesc(2);
	AttrNumber	sort_col = 1;
	Oid			sort_op = ByteaLessOperator;
	Oid			collation = InvalidOid;
	bool		nulls_first = false;
	char		name[64];
	PHash		map;

	rel = table_open(relid, NoLock);
	CaptureRelationShape(rel, &shape);
	TupleDescInitEntry(desc, 1, "key", BYTEAOID, -1, 0);
	TupleDescInitEntry(desc, 2, "value", BYTEAOID, -1, 0);
	TupleDescFinalize(desc);
	b.f = palloc_array(WsField, Max(shape.nstored, 1));
	b.slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	b.sort = tuplesort_begin_heap(desc, 1, &sort_col, &sort_op, &collation, &nulls_first,
								  maintenance_work_mem, NULL, TUPLESORT_NONE);
	CaptureTableRowsRead(rel, &shape, GetActiveSnapshot(), tree_rebuild_row, &b);
	ExecDropSingleTupleTableSlot(b.slot);
	b.slot = MakeSingleTupleTableSlot(desc, &TTSOpsMinimalTuple);
	if (prolly_empty(ctx, WS_ROW_VREFS, &map) < 0)
		prolly_report(ctx);
	tree_rebuild_write(ctx, &b, &map);
	tuplesort_end(b.sort);
	ExecDropSingleTupleTableSlot(b.slot);
	snprintf(name, sizeof(name), "capture/map/%u", relid);
	refs->routine->set(refs, name, &map);
	tree_record_shape(relid, &shape);
	pfree(b.f);

	/*
	 * (or the first catalog change would tell all of every table.  One that changed between the
	 * snapshot and the lock taken before this recorded its own, which this can't see, and it's a
	 * serialization failure: the worker starts again, with a later snapshot.)
	 */
	fingerprint = CaptureRowsFingerprint(relid, rel->rd_rel->relfilenode);
	values[0] = ObjectIdGetDatum(relid);
	values[1] = CStringGetTextDatum(fingerprint);
	if (SPI_execute_with_args("INSERT INTO pg_history.capture_fingerprints VALUES ($1, $2) ON CONFLICT (relid) DO NOTHING",
							  2, types, values, NULL, false, 0) != SPI_OK_INSERT)
		elog(ERROR, "could not record a table's fingerprint");
	pfree(fingerprint);
	ws_row_shape_free(&shape);
	table_close(rel, NoLock);
}

static void
tree_slot_made(XLogRecPtr from)
{
	ChunkStore *cs;
	VcRefs	   *refs;
	ProllyCtx	ctx;
	List	   *names = NIL;
	List	   *tables = NIL;
	ListCell   *lc;
	Oid			types[1] = {PG_LSNOID};
	Datum		values[1] = {LSNGetDatum(from)};

	/* (what changes a table's rows, untold, takes turns with this, as it does with its own kind) */
	LockSharedObject(DatabaseRelationId, MyDatabaseId, CAPTURE_ROWS_LOCK, ExclusiveLock);
	PushActiveSnapshot(GetTransactionSnapshot());
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect to SPI");
	cs = ChunkStoreOpen("pages");
	refs = VcRefsOpen("pointers", cs);
	prolly_ctx_init(&ctx, cs, &prolly_default_policy);

	refs->routine->list(refs, "capture/map/", tree_forget_ref, &names);
	foreach(lc, names)
		refs->routine->remove(refs, lfirst(lc));
	tree_exec("DELETE FROM pg_history.capture_positions", SPI_OK_DELETE);
	tree_exec("DELETE FROM pg_history.capture_tables", SPI_OK_DELETE);
	tree_exec("DELETE FROM pg_history.capture_fingerprints", SPI_OK_DELETE);
	tree_exec("SELECT c.oid FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
			  "WHERE n.nspbranch <> 0 AND c.relkind = 'r' AND c.relpersistence = 'p'", SPI_OK_SELECT);
	for (uint64 i = 0; i < SPI_processed; i++)
	{
		bool		isnull;

		tables = lappend_oid(tables, DatumGetObjectId(SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1, &isnull)));
	}

	/*
	 * Every table locked before any is read, so that DDL starting during the read waits for it,
	 * rather than have the read give way when it gets there.  (A table locked by a transaction
	 * waiting to commit would wait for the lock this holds: so this gives way, and begins again,
	 * rather than have a user's commit be the one a deadlock fails.)
	 */
	foreach(lc, tables)
	{
		if (!ConditionalLockRelationOid(lfirst_oid(lc), AccessShareLock))
			ereport(ERROR,
					(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					 errmsg("capture can't begin yet, as relation %u is locked", lfirst_oid(lc))));
		if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(lfirst_oid(lc))))
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("capture can't begin yet, as relation %u has gone", lfirst_oid(lc))));
	}
	INJECTION_POINT("capture-slot-locked", NULL);
	foreach(lc, tables)
		tree_rebuild_table(&ctx, refs, lfirst_oid(lc));

	if (SPI_execute_with_args("INSERT INTO pg_history.capture_positions VALUES (0, $1)",
							  1, types, values, NULL, false, 0) != SPI_OK_INSERT)
		elog(ERROR, "could not save where capture began");
	elog(DEBUG1, "capture tree: began with %d tables", list_length(tables));
	prolly_ctx_free(&ctx);
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "could not finish with SPI");
	PopActiveSnapshot();
}

static bool
tree_slot_will_do(void)
{
	bool		began;

	tree_settings();
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect to SPI");
	tree_exec("SELECT 1 FROM pg_history.capture_positions WHERE branch = 0", SPI_OK_SELECT);
	began = SPI_processed > 0;
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "could not finish with SPI");
	PopActiveSnapshot();
	CommitTransactionCommand();
	return began;
}

/*
 * A barrier's marker: all that committed before it has been told, and what's held is applied, with
 * each of the branch's row maps saved as of it, for whoever's waiting.
 */
static void
tree_after_record(void)
{
	int32		branch = tree_barrier_branch;
	XLogRecPtr	lsn = tree_barrier_lsn;
	ChunkStore *cs;
	VcRefs	   *refs;
	List	   *names = NIL;
	Oid			types[1] = {PG_LSNOID};
	Datum		values[1] = {LSNGetDatum(lsn)};

	if (XLogRecPtrIsInvalid(lsn))
		return;
	tree_barrier_lsn = InvalidXLogRecPtr;

	/*
	 * A barrier holds this lock from before its marker until it has its root.  If it's free, no one's
	 * waiting for this marker, so it's passed over, rather than apply here what a change later in
	 * the WAL may be what gets past.  (Someone else having it only means this may be needless.)
	 */
	StartTransactionCommand();
	if (ConditionalLockSharedObject(DatabaseRelationId, MyDatabaseId, CAPTURE_ROWS_LOCK, ExclusiveLock))
	{
		CommitTransactionCommand();
		return;
	}
	CommitTransactionCommand();

	tree_apply();
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect to SPI");
	cs = ChunkStoreOpen("pages");
	refs = VcRefsOpen("pointers", cs);

	/*
	 * Told again after a crash, with the maps past it: whoever was waiting has what was saved the
	 * first time, which these would overwrite.  (What's saved commits before anything past it is
	 * applied.  A position is where a commit starts, and lsn where the marker ends: a commit before
	 * it starts below that, and one right after it there.)
	 */
	if (tree_position(branch) >= lsn)
	{
		if (SPI_finish() != SPI_OK_FINISH)
			elog(ERROR, "could not finish with SPI");
		PopActiveSnapshot();
		CommitTransactionCommand();
		return;
	}

	/*
	 * What's saved for an earlier one has been used, or given up on, as they take turns.  (One
	 * later can only be there if this is being told again, and may be in use.)
	 */
	refs->routine->list(refs, CAPTURE_BARRIER_REF_PREFIX, tree_forget_ref, &names);
	foreach_ptr(char, name, names)
	{
		uint32		hi,
					lo;

		if (sscanf(name + strlen(CAPTURE_BARRIER_REF_PREFIX), "%X/%X/", &hi, &lo) != 2)
			elog(ERROR, "a barrier's reference is named \"%s\"", name);
		if (((XLogRecPtr) hi << 32 | lo) < lsn)
			(void) refs->routine->remove(refs, name);
	}
	if (SPI_execute_with_args("DELETE FROM pg_history.capture_barriers WHERE lsn < $1",
							  1, types, values, NULL, false, 0) != SPI_OK_DELETE)
		elog(ERROR, "could not forget a barrier");
	names = NIL;

	refs->routine->list(refs, "capture/map/", tree_forget_ref, &names);
	foreach_ptr(char, name, names)
	{
		Oid			relid = (Oid) strtoul(name + strlen("capture/map/"), NULL, 10);
		char		saved[128];
		PHash		map;

		if (NamespaceBranch(get_rel_namespace(relid)) != branch)
			continue;
		if (!refs->routine->get(refs, name, VC_REF_READ, &map))
			elog(ERROR, "a row map went while a barrier was saving it");
		snprintf(saved, sizeof(saved), CAPTURE_BARRIER_REF_PREFIX "%X/%X/%u", LSN_FORMAT_ARGS(lsn), relid);
		refs->routine->set(refs, saved, &map);
	}

	/* (it may be told again, after a crash before anything past it was applied) */
	if (SPI_execute_with_args("INSERT INTO pg_history.capture_barriers VALUES ($1) ON CONFLICT (lsn) DO NOTHING",
							  1, types, values, NULL, false, 0) != SPI_OK_INSERT)
		elog(ERROR, "could not save a barrier");
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "could not finish with SPI");
	PopActiveSnapshot();
	CommitTransactionCommand();
}

const CaptureSink TreeCaptureSink = {
	.name = "tree",
	.reads_at_start = true,
	.start = tree_start,
	.begin = tree_begin,
	.change = tree_change,
	.truncate = tree_truncate,
	.message = tree_message,
	.commit = tree_end,
	.apply = tree_apply,
	.after_record = tree_after_record,
	.slot_to_be_made = tree_slot_to_be_made,
	.slot_made = tree_slot_made,
	.slot_will_do = tree_slot_will_do,
};
