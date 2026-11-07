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
 * What's been told since the last apply is held: each table's changes as bytes, which go to a file
 * of its own once all of them are more than logical_decoding_work_mem.
 *
 * What DDL does to a table's rows reaches here as all of its rows, in logical messages at the end
 * of its transaction (capture_rows.c), which void all that was told of the table before them.  So
 * nothing's judged until its transaction has ended: what's wrong with a change only matters if it
 * isn't voided.
 *
 * A table with no key holds its rows by the whole row, with how many there are of it, so a change
 * is one fewer of the old row and one more of the new; CaptureLoggedIdentity has that come whole.
 *
 * A deferrable key can be held by two rows for a while, in the middle of a transaction, so edits made
 * one at a time could take off what another's just put.  Its whole old rows are logged, so what a
 * transaction did comes down to what's left of each row: see tree_net_edits.
 *
 * An update doesn't send a value it left out of line.  With no key, or a deferrable one, it's in the
 * old row; with any other key, it's what the row has now, from what's being applied or else the map, in the tree it's already in.
 *
 * What's held between applying is transactions already committed, so it's applied once it's more
 * than logical_decoding_work_mem, and each table's map is written a bounded batch at a time.  One
 * transaction is told in one call, in which nothing can be written, so what's held is bytes, which go
 * to a file past that same size, with what's asked of them all kept apart.
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
#include "storage/buffile.h"
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

/* A table's layout, for as long as the worker runs; and what's held of it for apply. */
typedef struct TreeTable
{
	Oid			relid;
	WsRowShape *shape;			/* as of what was last told, in tree_tables_cxt */
	XLogRecPtr	by_key;			/* the commit of the last change with its key's fields alone */
	XLogRecPtr	by_row;			/* and of the last with its whole old row, as of a deferrable key */

	/*
	 * Its changes since the last told whole, as tree_held_add writes them, in order: in file, then in
	 * buf, in tree_held_cxt.  The rest is what's asked of them all, in no more room however many they are.
	 */
	BufFile    *file;
	StringInfo	buf;
	uint64		nheld;
	bool		replaced;		/* it was told whole, at replaced_at: what's held is all there is */
	XLogRecPtr	replaced_at;
	int			problems;		/* how many have something wrong with them */
	const char *problem;		/* the last, of the commit problem_at */
	XLogRecPtr	problem_at;
	XLogRecPtr	layout_until;	/* the last commit in a layout other than the one now */
	XLogRecPtr	last_at;		/* the last commit held */
	WsRowShape *last_shape;		/* the layout of the last change held */
	int32		branch;
	bool		any_unchanged;
	Size		held_bytes;		/* of tree_held_bytes */
} TreeTable;

/* One row's change, as text: the shape's fields, NULL for a null. */
typedef struct TreeChange
{
	const WsRowShape *shape;
	int32		branch;
	XLogRecPtr	commit;
	char	  **old_row;		/* the key's fields, or all of them if there's none or it's deferrable */
	char	  **new_row;		/* NULL if there is no new row */
	bool		unchanged;		/* new_row has columns an update left where they were */
	bool		whole_old_row;	/* of a deferrable key: see tree_net_edits */
} TreeChange;

/* A fingerprint of a table's as a row of a transaction that changed catalogs was told. */
typedef struct TreeAsOf
{
	Oid			relid;
	char	   *as_of;
} TreeAsOf;

/* The last commit held of a branch. */
typedef struct TreeHeldBranch
{
	int32		branch;
	XLogRecPtr	commit;
} TreeHeldBranch;

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
static List *tree_held_branches = NIL;
static uint64 tree_held_changes = 0;
static Size tree_held_bytes = 0;	/* all of it, on file or not */
static Size tree_held_in_memory = 0;

/* TreeAsOfs of the transaction being told, to be checked at its commit. */
static List *tree_as_of = NIL;

/* Reset for each change. */
static MemoryContext tree_change_cxt = NULL;

/* Reset for each column. */
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
		tree_change_cxt = AllocSetContextCreate(TopMemoryContext, "capture tree change", ALLOCSET_DEFAULT_SIZES);
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
 * A tuple's fields by the table's shape, each in tree_change_cxt, reset for each change.  A value an update
 * left where it was is what old_row has of it, if it has it, or else, if unchanged is given, marked.
 */
static char **
tree_row(Relation relation, const TreeTable *table, HeapTuple tuple, bool key_only, char **old_row, bool *unchanged,
		 const char **problem)
{
	TupleDesc	desc = RelationGetDescr(relation);
	char	  **row = MemoryContextAllocZero(tree_change_cxt, sizeof(char *) * Max(table->shape->nstored, 1));
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
			was = MemoryContextSwitchTo(tree_change_cxt);
			*problem = psprintf("capture to trees was not given column \"%s\" of \"%s\", which an update left as it was",
								NameStr(att->attname), RelationGetRelationName(relation));
			MemoryContextSwitchTo(was);
			continue;
		}
		row[at] = MemoryContextStrdup(tree_change_cxt, ws_text_to_utf8(OidOutputFunctionCall(output, value)));
		MemoryContextReset(tree_row_cxt);
	}
	MemoryContextSwitchTo(old);
	return row;
}

static bool tree_same_shape(const WsRowShape *a, const WsRowShape *b);

/* What's in memory of every table's changes held goes to its file. */
static void
tree_held_spill(void)
{
	MemoryContext old = MemoryContextSwitchTo(tree_held_cxt);

	foreach_ptr(TreeTable, table, tree_tables)
	{
		if (table->buf == NULL || table->buf->len == 0)
			continue;

		/* (across transactions, as decoding's own only ever aborts) */
		if (table->file == NULL)
			table->file = BufFileCreateTemp(true);
		BufFileWrite(table->file, table->buf->data, table->buf->len);

		/* (its room too, which would otherwise stay what it grew to) */
		pfree(table->buf->data);
		pfree(table->buf);
		table->buf = NULL;
	}
	MemoryContextSwitchTo(old);
	elog(DEBUG1, "capture tree: spilled %zu bytes", tree_held_in_memory);
	tree_held_in_memory = 0;
}

static void
tree_held_note(TreeTable *table, int32 branch, XLogRecPtr commit)
{
	MemoryContext old = MemoryContextSwitchTo(tree_held_cxt);
	bool		known = false;

	foreach_ptr(TreeHeldBranch, held, tree_held_branches)
	{
		if (held->branch == branch)
		{
			held->commit = commit;
			known = true;
		}
	}
	if (!known)
	{
		TreeHeldBranch *held = palloc_object(TreeHeldBranch);

		held->branch = branch;
		held->commit = commit;
		tree_held_branches = lappend(tree_held_branches, held);
	}
	MemoryContextSwitchTo(old);
	table->branch = branch;
	table->last_at = commit;
	tree_held_changes++;
}

static void
tree_held_write_row(StringInfo out, char **row, int n)
{
	for (int c = 0; c < n; c++)
	{
		/* (-2 for a value left where it was, -1 for a null; with its end, so it's read in place) */
		int32		len = row[c] == tree_unchanged_value ? -2 : row[c] == NULL ? -1 : (int32) strlen(row[c]);

		appendBinaryStringInfo(out, &len, sizeof(len));
		if (len >= 0)
			appendBinaryStringInfo(out, row[c], len + 1);
	}
}

/*
 * A change held for apply: a uint32 of the length of the rest, then its commit, whether each row's
 * there and how it's to be taken, how many fields, and the rows.
 */
static void
tree_held_add(TreeTable *table, const TreeChange *change, const char *problem)
{
	MemoryContext old = MemoryContextSwitchTo(tree_held_cxt);
	int			start;
	uint32		len;
	uint8		flags = (change->old_row != NULL ? 1 : 0) | (change->new_row != NULL ? 2 : 0) |
		(change->unchanged ? 4 : 0) | (change->whole_old_row ? 8 : 0);
	int32		nstored = change->shape->nstored;

	if (table->buf == NULL)
		table->buf = makeStringInfo();
	if (table->last_shape != NULL && !tree_same_shape(table->last_shape, change->shape))
		table->layout_until = table->last_at;
	table->last_shape = (WsRowShape *) change->shape;
	if (problem != NULL)
	{
		table->problems++;
		if (table->problem != NULL)
			pfree((char *) table->problem);
		table->problem = pstrdup(problem);
		table->problem_at = change->commit;
	}
	table->any_unchanged |= change->unchanged;
	table->nheld++;

	/* (written where it's held, its length filled in once that's known) */
	start = table->buf->len;
	len = 0;
	appendBinaryStringInfo(table->buf, &len, sizeof(len));
	appendBinaryStringInfo(table->buf, &change->commit, sizeof(change->commit));
	appendBinaryStringInfo(table->buf, &flags, sizeof(flags));
	appendBinaryStringInfo(table->buf, &nstored, sizeof(nstored));
	if (change->old_row != NULL)
		tree_held_write_row(table->buf, change->old_row, nstored);
	if (change->new_row != NULL)
		tree_held_write_row(table->buf, change->new_row, nstored);
	len = table->buf->len - start - sizeof(len);
	memcpy(table->buf->data + start, &len, sizeof(len));
	tree_held_bytes += sizeof(len) + len;
	tree_held_in_memory += sizeof(len) + len;
	table->held_bytes += sizeof(len) + len;
	MemoryContextSwitchTo(old);
	tree_held_note(table, change->branch, change->commit);
	if (tree_held_in_memory > (Size) logical_decoding_work_mem * 1024)
		tree_held_spill();
}

static void
tree_held_forget(TreeTable *table)
{
	if (table->file != NULL)
		BufFileClose(table->file);
	table->file = NULL;
	if (table->buf != NULL)
	{
		tree_held_in_memory -= table->buf->len;
		pfree(table->buf->data);
		pfree(table->buf);
	}
	table->buf = NULL;
	tree_held_bytes -= table->held_bytes;
	table->held_bytes = 0;
	table->nheld = 0;
	table->replaced = false;
	table->replaced_at = InvalidXLogRecPtr;
	table->problems = 0;
	if (table->problem != NULL)
		pfree((char *) table->problem);
	table->problem = NULL;
	table->problem_at = InvalidXLogRecPtr;
	table->layout_until = InvalidXLogRecPtr;
	table->last_at = InvalidXLogRecPtr;
	table->last_shape = NULL;
	table->any_unchanged = false;
}

/* Reading a table's changes held back, in order, from its file and then what's still in memory. */
typedef struct TreeHeldReader
{
	TreeTable  *table;
	bool		in_file;
	int			at;
} TreeHeldReader;

static void
tree_held_read_begin(TreeHeldReader *r, TreeTable *table)
{
	r->table = table;
	r->at = 0;
	r->in_file = table->file != NULL;
	if (r->in_file && BufFileSeek(table->file, 0, 0, SEEK_SET) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rewind what capture spilled: %m")));
}

/* The next change, in the current memory context; false at the end. */
static bool
tree_held_read(TreeHeldReader *r, TreeChange *change)
{
	uint32		len;
	char	   *rec;
	char	   *p;
	uint8		flags;
	int32		nstored;

	if (r->in_file)
	{
		if (BufFileReadMaybeEOF(r->table->file, &len, sizeof(len), true) == 0)
			r->in_file = false;
		else
		{
			rec = palloc_extended(len, MCXT_ALLOC_HUGE);
			BufFileReadExact(r->table->file, rec, len);
		}
	}
	if (!r->in_file)
	{
		if (r->table->buf == NULL || r->at >= r->table->buf->len)
			return false;
		memcpy(&len, r->table->buf->data + r->at, sizeof(len));
		r->at += sizeof(len);
		rec = r->table->buf->data + r->at;
		r->at += len;
	}
	p = rec;
	memset(change, 0, sizeof(TreeChange));
	memcpy(&change->commit, p, sizeof(change->commit));
	p += sizeof(change->commit);
	flags = *(uint8 *) p;
	p += sizeof(flags);
	memcpy(&nstored, p, sizeof(nstored));
	p += sizeof(nstored);
	change->unchanged = (flags & 4) != 0;
	change->whole_old_row = (flags & 8) != 0;
	for (int side = 0; side < 2; side++)
	{
		char	  **row;

		if (!(flags & (side == 0 ? 1 : 2)))
			continue;
		row = palloc0_array(char *, Max(nstored, 1));
		for (int c = 0; c < nstored; c++)
		{
			int32		cell;

			memcpy(&cell, p, sizeof(cell));
			p += sizeof(cell);
			if (cell == -2)
				row[c] = tree_unchanged_value;
			else if (cell >= 0)
			{
				row[c] = p;
				p += cell + 1;
			}
		}
		if (side == 0)
			change->old_row = row;
		else
			change->new_row = row;
	}
	return true;
}

static void
tree_change(const CaptureChange *change)
{
	TreeTable  *table = tree_table_of(change->relation);
	TreeChange	held = {0};
	const char *problem = NULL;
	MemoryContext old;

	held.shape = table->shape;
	held.branch = change->branch;
	held.commit = tree_commit;
	if (change->catalogs_changed)
	{
		char	   *as_of;
		bool		known = false;

		old = MemoryContextSwitchTo(tree_held_cxt);
		as_of = CaptureRowsFingerprint(RelationGetRelid(change->relation), change->relation->rd_rel->relfilenode);
		foreach_ptr(TreeAsOf, seen, tree_as_of)
		{
			if (seen->relid == RelationGetRelid(change->relation) && strcmp(seen->as_of, as_of) == 0)
				known = true;
		}
		if (known)
			pfree(as_of);
		else
		{
			TreeAsOf   *seen = palloc_object(TreeAsOf);

			seen->relid = RelationGetRelid(change->relation);
			seen->as_of = as_of;
			tree_as_of = lappend(tree_as_of, seen);
		}
		MemoryContextSwitchTo(old);
	}
	held.whole_old_row = table->shape->nkey > 0 &&
		!OidIsValid(RelationGetPrimaryKeyIndex(change->relation, false)) &&
		OidIsValid(RelationGetPrimaryKeyIndex(change->relation, true));
	if (change->old_tuple == NULL &&
		(change->kind == CAPTURE_DELETE ||
		 (change->kind == CAPTURE_UPDATE && (table->shape->nkey == 0 || held.whole_old_row))))
		elog(ERROR, "a change of \"%s\" came with no old row", RelationGetRelationName(change->relation));
	if (change->old_tuple != NULL)
		held.old_row = tree_row(change->relation, table, change->old_tuple,
								table->shape->nkey > 0 && !held.whole_old_row, NULL, NULL, &problem);
	if (change->new_tuple != NULL)
		held.new_row = tree_row(change->relation, table, change->new_tuple, false, held.old_row,
								table->shape->nkey > 0 && !held.whole_old_row && change->kind == CAPTURE_UPDATE ?
								&held.unchanged : NULL, &problem);
	if (problem == NULL && table->shape->nkey > 0)
	{
		/* (both in a transaction can't be worked out, whichever came first) */
		if (held.whole_old_row)
			table->by_row = tree_commit;
		else
			table->by_key = tree_commit;
		if (table->by_row == tree_commit && table->by_key == tree_commit)
		{
			old = MemoryContextSwitchTo(tree_change_cxt);
			problem = psprintf("capture to trees does not follow relation %u having its deferrable key changed in a transaction that writes it",
							   table->relid);
			MemoryContextSwitchTo(old);
		}
	}
	tree_held_add(table, &held, problem);
	MemoryContextReset(tree_change_cxt);
}

static void
tree_truncate(int32 branch, Relation relation, bool restart_seqs, XLogRecPtr lsn)
{
	TreeTable  *table = tree_table_of(relation);
	TreeChange	held = {0};
	char	   *problem = psprintf("capture to trees does not follow a truncate of \"%s\"", RelationGetRelationName(relation));

	held.shape = table->shape;
	held.branch = branch;
	held.commit = tree_commit;
	tree_held_add(table, &held, problem);
	pfree(problem);
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
	MemoryContextSwitchTo(old);
	tree_held_forget(table);

	/* (what it's replacing, and so what's to be said of that at the commit, is void) */
	foreach_ptr(TreeAsOf, seen, tree_as_of)
	{
		if (seen->relid == relid)
			tree_as_of = foreach_delete_current(tree_as_of, seen);
	}
	table->replaced = true;
	table->replaced_at = tree_commit;
	table->last_shape = table->shape;
	tree_held_note(table, branch, tree_commit);
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
	old = MemoryContextSwitchTo(tree_change_cxt);
	for (int r = 0; r < nrows; r++)
	{
		TreeChange	held = {0};

		held.shape = table->shape;
		held.branch = branch;
		held.commit = tree_commit;
		held.new_row = palloc0_array(char *, Max(table->shape->nstored, 1));
		for (int c = 0; c < table->shape->nstored; c++)
		{
			int32		len = read_int32(&p, end);

			if (len >= 0)
				held.new_row[c] = pnstrdup(tree_take(&p, end, len), len);
		}
		tree_held_add(table, &held, NULL);
		MemoryContextReset(tree_change_cxt);
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
	foreach_ptr(TreeAsOf, seen, tree_as_of)
	{
		TreeTable  *table = tree_table_by_id(seen->relid);
		HeapTuple	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(seen->relid));

		/* (one that's gone has nothing held that this could be of) */
		if (HeapTupleIsValid(tup))
		{
			char	   *now = CaptureRowsFingerprint(seen->relid, ((Form_pg_class) GETSTRUCT(tup))->relfilenode);

			ReleaseSysCache(tup);

			/* (one it's still different from at the commit has its rows told, voiding this) */
			if (strcmp(now, seen->as_of) != 0)
			{
				MemoryContext old = MemoryContextSwitchTo(tree_held_cxt);

				if (table->problem != NULL)
					pfree((char *) table->problem);
				table->problem = psprintf("capture to trees has a row of relation %u written as catalogs were during its transaction and aren't now", seen->relid);
				table->problem_at = tree_commit;
				table->problems++;
				MemoryContextSwitchTo(old);
			}
			pfree(now);
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

/* A table's map is written this much at a time. */
#define TREE_BATCH_ROWS 1000
#define TREE_BATCH_BYTES (8 * 1024 * 1024)

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

/*
 * A new row's fields, with what an update left where it was from the row it has now, under key.
 * They point into that, which is the batch's.
 */
static void
tree_fill_unchanged(ProllyCtx *ctx, const TreeTable *table, const TreeChange *change, const PHash *map,
					HTAB *latest, const ProllyEdit *edits, const uint8 *key, size_t klen, WsField *f)
{
	TreeEditKey look = {key, klen};
	TreeLatestEdit *found = hash_search(latest, &look, HASH_FIND, NULL);
	const uint8 *now = NULL;
	size_t		nowlen = 0;
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

	WsRowCodec->decode(change->shape, key, klen, now, nowlen, had);
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

static int
tree_edit_row_cmp(const void *a, const void *b)
{
	const ProllyEdit *x = a;
	const ProllyEdit *y = b;
	int			c = memcmp(x->key, y->key, Min(x->klen, y->klen));

	if (c == 0)
		c = (x->klen > y->klen) - (x->klen < y->klen);
	if (c == 0)
		c = memcmp(x->val, y->val, Min(x->vlen, y->vlen));
	if (c == 0)
		c = (x->vlen > y->vlen) - (x->vlen < y->vlen);
	return c;
}

/*
 * One transaction's edits of a table with a deferrable key, each with its row's value, as what they
 * come to: how many of each row, whatever order they came in.  By the transaction's end a key has at
 * most one, which is the edit; or none, and if it lost one, it's gone.  How many edits there now are.
 */
static size_t
tree_net_edits(Oid relid, ProllyEdit *edits, size_t n)
{
	size_t		out = 0;

	if (n > 1)
		qsort(edits, n, sizeof(ProllyEdit), tree_edit_row_cmp);
	for (size_t i = 0; i < n;)
	{
		ProllyEdit	kept = edits[i];
		bool		have = false;
		bool		lost = false;
		size_t		k = i;

		while (k < n && edits[k].klen == edits[i].klen && memcmp(edits[k].key, edits[i].key, edits[i].klen) == 0)
		{
			size_t		r = k;
			int64		net = 0;

			for (; r < n && tree_edit_row_cmp(&edits[k], &edits[r]) == 0; r++)
				net += edits[r].del ? -1 : 1;
			if (net < 0)
				lost = true;
			if (net > 0)
			{
				if (have || net > 1)
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("a transaction left a deferrable key of relation %u on more than one row", relid)));
				have = true;
				kept = edits[k];
			}
			k = r;
		}

		/* (all but the row that's left go now, as what's kept is all that's freed later) */
		for (size_t e = i; e < k; e++)
		{
			if (have && edits[e].key == kept.key)
				continue;
			if (!have && e == i)
				continue;
			px_free((void *) edits[e].key);
			px_free((void *) edits[e].val);
		}
		if (have || lost)
		{
			if (!have)
			{
				px_free((void *) kept.val);
				kept.val = NULL;
				kept.vlen = 0;
			}
			edits[out] = kept;
			edits[out].del = !have;
			out++;
		}
		else
		{
			px_free((void *) kept.key);
			px_free((void *) kept.val);
		}
		i = k;
	}
	return out;
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

static void
tree_apply_context(void *arg)
{
	errcontext("capture applying changes to table %s", (const char *) arg);
}

/*
 * Edits written on a table's map, so that what's looked for from here is found there.  All of what
 * they point to, and what's found by key, goes with batch_cxt, which is current.
 */
static void
tree_write_edits(ProllyCtx *ctx, const TreeTable *table, const WsRowShape *shape, PHash *map, ProllyEdit *edits, size_t *n,
				 HTAB **latest, bool has_unchanged, MemoryContext batch_cxt)
{
	size_t		count = *n;

	if (shape->nkey == 0)
	{
		char	   *relname = get_rel_name(table->relid);
		ErrorContextCallback context = {
			.previous = error_context_stack,
			.callback = tree_apply_context,
			.arg = psprintf("%s of relation %u", relname != NULL ? relname : "(gone)", table->relid),
		};

		error_context_stack = &context;
		count = ws_rows_keyless_merge(ctx, map, edits, count);
		error_context_stack = context.previous;
	}
	if (count > 0 && prolly_apply(ctx, map, edits, count, map) < 0)
		prolly_report(ctx);
	elog(DEBUG1, "capture tree: wrote %zu edits of relation %u", count, table->relid);
	MemoryContextReset(batch_cxt);
	*latest = has_unchanged ? tree_latest_edits() : NULL;
	*n = 0;
}

/* The changes held of one table, not yet applied by its branch's position, on its row map. */
static void
tree_apply_table(ProllyCtx *ctx, VcRefs *refs, TreeTable *table, List *positions)
{
	WsField    *f;
	ProllyEdit *edits;
	size_t		n = 0;
	bool		anew = false;
	const WsRowShape *shape = NULL;
	char		name[64];
	PHash		map;
	HTAB	   *latest = NULL;
	bool		has_unchanged = false;
	bool		netting = false;
	bool		wrote = false;
	Size		bytes = 0;
	MemoryContext batch_cxt;
	MemoryContext read_cxt;
	MemoryContext was;
	XLogRecPtr	netted = InvalidXLogRecPtr;
	XLogRecPtr	before;
	TreeHeldReader reader;
	size_t		room;

	/* (one that's gone has nothing to hold, whatever was wrong with holding it) */
	if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(table->relid)))
		return;

	/* what's held starts at the last told whole; what's been applied before this is passed over */
	before = tree_position_of(positions, table->branch)->before;
	if (table->last_at <= before)
	{
		elog(DEBUG1, "capture tree: passed over a change of %X/%X", LSN_FORMAT_ARGS(table->last_at));
		return;
	}
	if (table->problem_at > before)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("%s", table->problem)));
	if (table->layout_until > before)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("capture to trees does not follow the layout of relation %u changing", table->relid)));
	shape = table->last_shape;
	anew = table->replaced && table->replaced_at > before;
	if (!anew)
		tree_check_table(table, shape);

	snprintf(name, sizeof(name), "capture/map/%u", table->relid);
	if ((anew || !refs->routine->get(refs, name, VC_REF_READ, &map)) &&
		prolly_empty(ctx, WS_ROW_VREFS, &map) < 0)
		prolly_report(ctx);
	batch_cxt = AllocSetContextCreate(CurrentMemoryContext, "capture apply batch", ALLOCSET_DEFAULT_SIZES);
	read_cxt = AllocSetContextCreate(CurrentMemoryContext, "capture apply read", ALLOCSET_DEFAULT_SIZES);
	was = MemoryContextSwitchTo(batch_cxt);
	has_unchanged = table->any_unchanged;
	if (has_unchanged)
		latest = tree_latest_edits();
	f = MemoryContextAlloc(was, sizeof(WsField) * Max(shape->nstored, 1));

	/* (a deferrable key's transaction is written whole, so it's more if it needs to be) */
	room = 2 * TREE_BATCH_ROWS + 2;
	edits = MemoryContextAlloc(was, sizeof(ProllyEdit) * room);
	tree_held_read_begin(&reader, table);
	for (;;)
	{
		TreeChange	read;
		TreeChange *change = &read;
		Buf			k = BUF_INIT,
					v = BUF_INIT;
		size_t		first;
		bool		more;

		MemoryContextReset(read_cxt);
		MemoryContextSwitchTo(read_cxt);
		more = tree_held_read(&reader, &read);
		MemoryContextSwitchTo(batch_cxt);
		if (!more)
			break;
		if (read.commit <= before)
		{
			elog(DEBUG1, "capture tree: passed over a change of %X/%X", LSN_FORMAT_ARGS(read.commit));
			continue;
		}
		read.shape = shape;
		read.branch = table->branch;
		if (n + 2 > room)
		{
			room *= 2;
			edits = repalloc_huge(edits, sizeof(ProllyEdit) * room);
		}
		first = n;

		CHECK_FOR_INTERRUPTS();

		/*
		 * A deferrable key's edits are of their transaction, on their own: they're worked out and
		 * written when it ends, and what came before is written first, as they don't go through latest.
		 */
		if (netting && (!change->whole_old_row || change->commit != netted))
		{
			n = tree_net_edits(table->relid, edits, n);
			netting = false;
			tree_write_edits(ctx, table, shape, &map, edits, &n, &latest, has_unchanged, batch_cxt);
			wrote = true;
			bytes = 0;
			first = 0;
		}
		if (change->whole_old_row && !netting)
		{
			if (n > 0)
			{
				tree_write_edits(ctx, table, shape, &map, edits, &n, &latest, has_unchanged, batch_cxt);
				wrote = true;
				bytes = 0;
			}
			first = 0;
			netting = true;
			netted = change->commit;
		}
		if (change->old_row != NULL)
		{
			tree_fields(change->shape, change->old_row, f);
			ws_row_store(ctx, change->shape, f, &k, &v);
			bytes += k.len;
			if (change->whole_old_row)
				edits[n++] = (ProllyEdit) {.key = k.data, .klen = k.len, .val = v.data, .vlen = v.len, .del = true};
			else
			{
				buf_free(&v);
				edits[n++] = (ProllyEdit) {.key = k.data, .klen = k.len, .del = true};
			}
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
					/* (its key alone, so no value's put in a tree for it) */
					WsField    *key_alone = palloc_array(WsField, change->shape->nstored);

					for (int c = 0; c < change->shape->nstored; c++)
						key_alone[c] = c < change->shape->nkey ? f[c] : (WsField) {.null = true};
					WsRowCodec->encode(change->shape, key_alone, &k, &v);
					pfree(key_alone);
					tree_fill_unchanged(ctx, table, change, &map, latest, edits, k.data, k.len, f);
				}
			}
			ws_row_store_kept(ctx, change->shape, f, &k, &v);
			bytes += k.len + v.len;
			edits[n++] = (ProllyEdit) {.key = k.data, .klen = k.len, .val = v.data, .vlen = v.len};
		}
		if (latest != NULL && !change->whole_old_row)
		{
			for (size_t e = first; e < n; e++)
			{
				TreeEditKey key = {edits[e].key, edits[e].klen};
				TreeLatestEdit *entry = hash_search(latest, &key, HASH_ENTER, NULL);

				entry->edit = e;
			}
		}
		if (!netting && (n >= TREE_BATCH_ROWS || bytes >= TREE_BATCH_BYTES))
		{
			tree_write_edits(ctx, table, shape, &map, edits, &n, &latest, has_unchanged, batch_cxt);
			wrote = true;
			bytes = 0;
		}
	}
	if (netting)
		n = tree_net_edits(table->relid, edits, n);
	if (n > 0)
	{
		tree_write_edits(ctx, table, shape, &map, edits, &n, &latest, has_unchanged, batch_cxt);
		wrote = true;
	}
	if (wrote || anew)
	{
		refs->routine->set(refs, name, &map);
		tree_record_shape(table->relid, table->shape);
	}
	MemoryContextSwitchTo(was);
	MemoryContextDelete(batch_cxt);
	MemoryContextDelete(read_cxt);
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

	if (tree_held_changes == 0)
		return;
	INJECTION_POINT("capture-apply-begin", NULL);
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect to SPI");
	cs = ChunkStoreOpen("pages");
	refs = VcRefsOpen("pointers", cs);
	prolly_ctx_init(&ctx, cs, &prolly_default_policy);
	foreach_ptr(TreeHeldBranch, held, tree_held_branches)
	{
		TreePosition *position = palloc_object(TreePosition);

		position->branch = held->branch;
		position->before = tree_position(held->branch);
		position->now = Max(position->before, held->commit);
		positions = lappend(positions, position);
	}
	foreach(lc, tree_tables)
	{
		TreeTable  *table = lfirst(lc);

		if (table->nheld > 0 || table->replaced)
			tree_apply_table(&ctx, refs, table, positions);
		tree_held_forget(table);
	}
	foreach_ptr(TreePosition, position, positions)
	{
		if (position->now > position->before)
			tree_set_position(position->branch, position->now);
	}
	elog(DEBUG1, "capture tree: applied " UINT64_FORMAT " changes", tree_held_changes);
	prolly_ctx_free(&ctx);
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "could not finish with SPI");
	PopActiveSnapshot();
	INJECTION_POINT("capture-applying", NULL);
	CommitTransactionCommand();
	tree_held_branches = NIL;
	tree_held_changes = 0;
	tree_held_bytes = 0;
	tree_held_in_memory = 0;
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
	ProllyEdit *edits = palloc_array(ProllyEdit, TREE_BATCH_ROWS);
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
		if (n > 0 && (!more || n == TREE_BATCH_ROWS || bytes >= TREE_BATCH_BYTES))
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
tree_marker(void)
{
	int32		branch = tree_barrier_branch;
	XLogRecPtr	lsn = tree_barrier_lsn;
	ChunkStore *cs;
	VcRefs	   *refs;
	List	   *names = NIL;
	Oid			types[1] = {PG_LSNOID};
	Datum		values[1] = {LSNGetDatum(lsn)};

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

/*
 * Whether applying what's held would stop at what's wrong with a change.  That may be what a change
 * still to come gets past, so what's held isn't applied for its size then, or a worker would stop at
 * the same place each time it started, and never read that far.
 */
static bool
tree_held_has_problem(void)
{
	foreach_ptr(TreeTable, table, tree_tables)
	{
		if (table->problems > 0)
			return true;
	}
	return false;
}

/*
 * A marker told is seen to; and what's held, being all of the transactions told so far, is
 * applied once it's more than logical_decoding_work_mem, as the worker may be far behind.
 */
static bool
tree_after_record(void)
{
	if (!XLogRecPtrIsInvalid(tree_barrier_lsn))
		tree_marker();
	if (tree_held_changes == 0 ||
		tree_held_bytes <= (Size) logical_decoding_work_mem * 1024 ||
		tree_held_has_problem())
		return false;
	tree_apply();
	return true;
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
