/*-------------------------------------------------------------------------
 *
 * ws_rows.c
 *	  A table's rows as its row map, and back.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/ws/ws_rows.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "prolly/ws/ws_row.h"
#include "prolly/ws/ws_rows.h"
#include "prolly/ws/ws_sql.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/* How many rows capture takes at a time. */
#define CAPTURE_BATCH 1000

typedef struct CaptureBatch
{
	ProllyCtx  *ctx;
	const WsRowShape *shape;
	PHash		map;
	uint64		seen;
} CaptureBatch;

static int
edit_cmp(const void *a, const void *b)
{
	const ProllyEdit *x = a;
	const ProllyEdit *y = b;
	int			c = memcmp(x->key, y->key, Min(x->klen, y->klen));

	if (c != 0)
		return c;
	return (x->klen > y->klen) - (x->klen < y->klen);
}

uint64
ws_rows_keyless_count(const uint8 *k, size_t kl, const uint8 *v, size_t vl)
{
	const uint8 *p;
	size_t		len;
	const uint8 *end;
	uint64		count = 0;

	WsRowCodec->keyless_payload(k, kl, v, vl, &p, &len);
	end = p + len;
	if (!ws_read_canonical_uvarint(&p, end, &count) || p != end || count == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("a malformed row: a keyless row's count is not one")));
	return count;
}

size_t
ws_rows_keyless_merge(ProllyCtx *ctx, const PHash *map, ProllyEdit *edits, size_t n)
{
	size_t		out = 0;

	if (n > 1)
		qsort(edits, n, sizeof(ProllyEdit), edit_cmp);
	for (size_t i = 0; i < n;)
	{
		size_t		same = i;
		uint64		count = 0;
		uint64		fewer = 0;
		const uint8 *held;
		size_t		heldlen;
		int			rc;
		bool		had;

		for (; same < n && edit_cmp(&edits[i], &edits[same]) == 0; same++)
		{
			if (edits[same].del)
				fewer++;
			else
				count++;
		}
		rc = prolly_get(ctx, map, edits[i].key, edits[i].klen, &held, &heldlen);
		if (rc < 0)
			prolly_report(ctx);
		had = rc == PROLLY_OK;
		if (had)
			count += ws_rows_keyless_count(edits[i].key, edits[i].klen, held, heldlen);
		if (fewer > count)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("a row map holds fewer of a keyless row than were removed"),
					 errhint("A rewrite of the table, such as VACUUM FULL, has capture told all of its rows, once capture reads that far and no barrier is waiting.  Turning capture_in_background off and on begins again.")));
		count -= fewer;
		if (count > 0)
		{
			Buf			count_bytes = BUF_INIT;
			Buf			val = BUF_INIT;

			buf_append_uvarint(&count_bytes, count);
			WsRowCodec->keyless_value(edits[i].key, edits[i].klen, count_bytes.data, count_bytes.len, &val);
			buf_free(&count_bytes);
			edits[out] = edits[i];
			edits[out].val = val.data;
			edits[out].vlen = val.len;
			edits[out].del = false;
			out++;
		}
		else if (had)
		{
			edits[out] = edits[i];
			edits[out].val = NULL;
			edits[out].vlen = 0;
			edits[out].del = true;
			out++;
		}
		i = same;
	}
	return out;
}

static void
capture_batch(void *arg, const WsSqlRows *rows)
{
	CaptureBatch *b = arg;
	const WsRowShape *s = b->shape;
	WsField    *f = palloc_array(WsField, Max(s->nstored, 1));
	ProllyEdit *edits = palloc_array(ProllyEdit, Max(rows->nrows, 1));
	size_t		n = (size_t) rows->nrows;

	b->seen += n;
	for (int r = 0; r < rows->nrows; r++)
	{
		Buf			k = BUF_INIT,
					v = BUF_INIT;

		CHECK_FOR_INTERRUPTS();
		for (int c = 0; c < s->nstored; c++)
		{
			const char *text = ws_sql_value(rows, r, c);

			if (text != NULL)
				text = ws_text_to_utf8(text);
			f[c].p = (const uint8 *) text;
			f[c].len = text != NULL ? strlen(text) : 0;
			f[c].null = text == NULL;
			f[c].large = false;
		}
		ws_row_store(b->ctx, s, f, &k, &v);
		edits[r].key = k.data;
		edits[r].klen = k.len;
		edits[r].val = v.data;
		edits[r].vlen = v.len;
		edits[r].del = false;
	}
	if (s->nkey == 0)
		n = ws_rows_keyless_merge(b->ctx, &b->map, edits, n);
	else if (n > 1)
		qsort(edits, n, sizeof(ProllyEdit), edit_cmp);
	if (n > 0 && prolly_apply(b->ctx, &b->map, edits, n, &b->map) < 0)
		prolly_report(b->ctx);
}

static void
direct_capture(ProllyCtx *ctx, const char *schema, const WsTableSchema *s, PHash *map)
{
	CaptureBatch b = {ctx};
	WsRowShape	shape;
	Buf			sql = BUF_INIT;
	int			level;
	char	   *table = ws_name_from_utf8(s->name);

	if (prolly_empty(ctx, WS_ROW_VREFS, &b.map) < 0)
		prolly_report(ctx);
	if (s->partition == WS_PARTITION_NONE)
	{
		ws_schema_row_shape(s, &shape);
		b.shape = &shape;
		ws_put_text(&sql, "SELECT ");
		for (int i = 0; i < shape.nstored; i++)
		{
			const WsColumn *col = ws_schema_column(s, shape.tags[i]);

			if (i > 0)
				ws_put_text(&sql, ", ");
			ws_put_identifier(&sql, ws_name_from_utf8(col->name));
		}
		ws_put_text(&sql, " FROM ONLY ");
		ws_put_identifier(&sql, schema);
		ws_put_text(&sql, ".");
		ws_put_identifier(&sql, table);
		buf_append_byte(&sql, 0);
		level = WsSql->begin();
		WsSql->scan((const char *) sql.data, 0, NULL, CAPTURE_BATCH, capture_batch, &b);
		WsSql->end(level);
		if (shape.nkey > 0)
		{
			uint64		held;

			if (prolly_count(ctx, &b.map, &held) < 0)
				prolly_report(ctx);
			if (held != b.seen)
				ereport(ERROR,
						(errcode(ERRCODE_UNIQUE_VIOLATION),
						 errmsg("table \"%s\" has rows whose keys print alike, " UINT64_FORMAT " rows to " UINT64_FORMAT " keys",
								table, b.seen, held),
						 errhint("A commit tells its rows apart by the text of their keys.")));
		}
		ws_row_shape_free(&shape);
	}
	*map = b.map;
	buf_free(&sql);
	pfree(table);
}

/* What reads and writes one column of a row. */
typedef struct RestoreColumn
{
	AttrNumber	attnum;
	FmgrInfo	input;
	Oid			ioparam;
	int32		typmod;
	FmgrInfo	output;
} RestoreColumn;

/* A row's column by its base type's input: a domain's checks are user code. */
static void
restore_column_init(RestoreColumn *c, Form_pg_attribute att)
{
	int32		typmod = att->atttypmod;
	Oid			base = getBaseTypeAndTypmod(att->atttypid, &typmod);
	Oid			input;
	Oid			output;
	bool		varlena;

	c->attnum = att->attnum;
	c->typmod = typmod;
	getTypeInputInfo(base, &input, &c->ioparam);
	fmgr_info(input, &c->input);
	getTypeOutputInfo(att->atttypid, &output, &varlena);
	fmgr_info(output, &c->output);
}

/*
 * What writes a table's rows directly, as logical replication's apply does, below the rewriter and
 * with no trigger: no rule, trigger or domain's check runs, and a stored generated column is
 * written as given.  Its indexes are kept, and their uniqueness checked as each row goes in.
 */
typedef struct RowsWriter
{
	ProllyCtx  *ctx;
	char	   *table;
	Relation	rel;
	TupleDesc	desc;
	WsRowShape	shape;
	RestoreColumn *cols;
	EState	   *estate;
	ResultRelInfo *rri;
	TupleTableSlot *slot;
	CommandId	cid;
	WsField    *f;
	MemoryContext rowcxt;
	Oid			key_index;		/* the primary key's, of a keyed table */
} RowsWriter;

/* The table the record names opened to write, or false if it's partitioned, with no rows of its own. */
static bool
writer_open(RowsWriter *w, ProllyCtx *ctx, const char *schema, const WsTableSchema *s, bool attached,
			const char *what)
{
	Oid			nsp = get_namespace_oid(schema, false);
	Oid			relid;
	int			live = 0;

	memset(w, 0, sizeof(*w));
	w->ctx = ctx;
	w->table = ws_name_from_utf8(s->name);
	relid = get_relname_relid(w->table, nsp);
	if (!OidIsValid(relid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("table \"%s\" in schema \"%s\" does not exist", w->table, schema)));
	if (s->partition != WS_PARTITION_NONE)
		return false;
	PreventCommandIfReadOnly(what);
	PreventCommandIfParallelMode(what);
	PreventCommandDuringRecovery(what);
	w->rel = table_open(relid, RowExclusiveLock);
	if (!object_ownercheck(RelationRelationId, relid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLE, w->table);
	if (w->rel->rd_rel->relkind != RELKIND_RELATION)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table to hold rows", w->table)));
	if (!attached && w->rel->rd_rel->relispartition)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("rows cannot be restored into \"%s\" while it is attached", w->table),
				 errhint("Restore a partition's rows before attaching it, whose check of its bound stands behind them.")));

	/* each stored column of the record, and no other, by its tag */
	w->desc = RelationGetDescr(w->rel);
	ws_schema_row_shape(s, &w->shape);
	w->cols = palloc0_array(RestoreColumn, Max(w->shape.nstored, 1));
	for (int i = 0; i < w->desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(w->desc, i);
		int			at;

		if (att->attisdropped || att->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL)
			continue;
		at = ws_row_shape_at(&w->shape, (uint64) att->attcoltag);
		if (at < 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("column \"%s\" of table \"%s\" is not one its rows hold",
							NameStr(att->attname), w->table)));
		restore_column_init(&w->cols[at], att);
		live++;
	}
	if (live != w->shape.nstored)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("table \"%s\" has %d of the %d columns its rows hold",
						w->table, live, w->shape.nstored)));

	w->estate = CreateExecutorState();
	w->rri = makeNode(ResultRelInfo);
	InitResultRelInfo(w->rri, w->rel, 1, NULL, 0);
	ExecOpenIndices(w->rri, false);
	w->key_index = InvalidOid;
	for (int i = 0; i < w->rri->ri_NumIndices; i++)
		if (w->rri->ri_IndexRelationDescs[i]->rd_index->indisprimary)
			w->key_index = RelationGetRelid(w->rri->ri_IndexRelationDescs[i]);
	w->slot = table_slot_create(w->rel, NULL);
	w->cid = GetCurrentCommandId(true);
	w->f = palloc_array(WsField, Max(w->shape.nstored, 1));
	w->rowcxt = AllocSetContextCreate(CurrentMemoryContext, "ws_rows row", ALLOCSET_DEFAULT_SIZES);
	return true;
}

static void
writer_close(RowsWriter *w)
{
	if (w->rel != NULL)
	{
		MemoryContextDelete(w->rowcxt);
		ExecDropSingleTupleTableSlot(w->slot);
		ExecCloseIndices(w->rri);
		FreeExecutorState(w->estate);
		table_close(w->rel, NoLock);
		CommandCounterIncrement();
		ws_row_shape_free(&w->shape);
		pfree(w->f);
		pfree(w->cols);
	}
	pfree(w->table);
}

/* Fields in the writer's slot, in its shape's order, read in the row's context. */
static void
writer_fill_fields(RowsWriter *w, const WsField *f)
{
	MemoryContext old = MemoryContextSwitchTo(w->rowcxt);

	ExecClearTuple(w->slot);
	memset(w->slot->tts_isnull, true, w->desc->natts * sizeof(bool));

	/* (the values' own only: an index's functions may rely on a domain) */
	domain_checks_suspended = true;
	PG_TRY();
	{
		for (int i = 0; i < w->shape.nstored; i++)
			if (!f[i].null)
			{
				char	   *text = pnstrdup((const char *) f[i].p, f[i].len);
				int			att = w->cols[i].attnum - 1;

				w->slot->tts_values[att] = InputFunctionCall(&w->cols[i].input, ws_text_from_utf8(text),
															 w->cols[i].ioparam, w->cols[i].typmod);
				w->slot->tts_isnull[att] = false;
			}
	}
	PG_FINALLY();
	{
		domain_checks_suspended = false;
	}
	PG_END_TRY();
	ExecStoreVirtualTuple(w->slot);
	MemoryContextSwitchTo(old);
}

/* A row of the map in the writer's slot, its fields in w->f; how many of it a keyless row is. */
static uint64
writer_fill(RowsWriter *w, const uint8 *k, size_t kl, const uint8 *v, size_t vl)
{
	MemoryContext old;
	uint64		count = 1;
	Buf			held = BUF_INIT;	/* (the row's, as all else of it) */

	MemoryContextReset(w->rowcxt);
	old = MemoryContextSwitchTo(w->rowcxt);
	ws_row_load(w->ctx, &w->shape, k, kl, v, vl, w->f, &held);
	if (w->shape.nkey == 0)
		count = ws_rows_keyless_count(k, kl, v, vl);
	MemoryContextSwitchTo(old);
	writer_fill_fields(w, w->f);
	return count;
}

/* The row in the writer's slot inserted, with its index entries. */
static void
writer_insert(RowsWriter *w)
{
	MemoryContext old;

	CHECK_FOR_INTERRUPTS();
	ResetPerTupleExprContext(w->estate);
	old = MemoryContextSwitchTo(GetPerTupleMemoryContext(w->estate));
	table_tuple_insert(w->rel, w->slot, w->cid, 0, NULL);

	/*
	 * (A deferrable key says what it would check at commit.  All of a
	 * state is written here, so that is now.)
	 */
	if (w->rri->ri_NumIndices > 0 &&
		ExecInsertIndexTuples(w->rri, w->estate, 0, w->slot, NIL, NULL) != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_UNIQUE_VIOLATION),
				 errmsg("a row of table \"%s\" has a key another has, under a deferrable constraint",
						w->table)));
	MemoryContextSwitchTo(old);
}

static void
direct_restore(ProllyCtx *ctx, const char *schema, const WsTableSchema *s, const PHash *map)
{
	RowsWriter	w;
	ProllyIter *it;
	const uint8 *k,
			   *v;
	size_t		kl,
				vl;
	int			rc;
	int			level;

	if (!writer_open(&w, ctx, schema, s, false, "restoring rows"))
	{
		uint64		count;

		if (prolly_count(ctx, map, &count) < 0)
			prolly_report(ctx);
		if (count != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("partitioned table \"%s\" holds rows of its own", w.table)));
		writer_close(&w);
		return;
	}

	level = WsSql->begin();
	if (prolly_iter_open(ctx, map, NULL, 0, &it) < 0)
		prolly_report(ctx);
	while ((rc = prolly_iter_next(it, &k, &kl, &v, &vl)) > 0)
	{
		uint64		count;

		CHECK_FOR_INTERRUPTS();
		count = writer_fill(&w, k, kl, v, vl);
		for (uint64 i = 0; i < count; i++)
			writer_insert(&w);
	}
	prolly_iter_close(it);
	if (rc < 0)
		prolly_report(ctx);
	WsSql->end(level);
	writer_close(&w);
}

/* A keyless row's fields as bytes to tell it by: each a length, -1 for a null, and its text. */
static void
row_print_bytes(const WsField *f, int n, StringInfo out)
{
	for (int i = 0; i < n; i++)
	{
		int32		len = f[i].null ? -1 : (int32) f[i].len;

		appendBinaryStringInfo(out, (const char *) &len, sizeof(len));
		if (!f[i].null)
			appendBinaryStringInfo(out, (const char *) f[i].p, f[i].len);
	}
}

/* Keyless rows to be taken away, by what they print as, and how many of each are left to find. */
typedef struct DoomedRow
{
	char	   *bytes;
	int			len;
	uint64		left;
} DoomedRow;

static int
doomed_cmp(const void *a, const void *b)
{
	const DoomedRow *x = a;
	const DoomedRow *y = b;
	int			c = memcmp(x->bytes, y->bytes, Min(x->len, y->len));

	if (c != 0)
		return c;
	return (x->len > y->len) - (x->len < y->len);
}

typedef struct RowsApply
{
	RowsWriter *w;
	const WsRowShape *was;		/* what from's rows are in, if not the table's now, or NULL */
	int		   *was_at;			/* each of the table's fields' place in it, or -1: null now */
	WsField    *was_f;
	WsField    *now_f;
	bool		inserting;
	uint64		deleted;
	uint64		inserted;
	uint64		kept;
	DoomedRow  *doomed;
	int			ndoomed;
	int			doomed_cap;
	TupleTableSlot *found;
} RowsApply;

/*
 * One key that differs: in the first pass, what from has of it taken away, and in the second, what to
 * has put in.  A keyed row is found by its key; a keyless one is only noted, to be found in one scan.
 */
static void
apply_diff(void *arg, const ProllyDiff *d)
{
	RowsApply  *a = arg;
	RowsWriter *w = a->w;
	uint64		had = 0;
	uint64		has = 0;

	CHECK_FOR_INTERRUPTS();
	if (w->shape.nkey == 0)
	{
		if (d->from != NULL)
			had = ws_rows_keyless_count(d->key, d->klen, d->from, d->flen);
		if (d->to != NULL)
			has = ws_rows_keyless_count(d->key, d->klen, d->to, d->tlen);
	}
	else
	{
		had = d->from != NULL;
		has = d->to != NULL;
	}

	/*
	 * (a row only the change of the table's definition rewrote is as the target has it already:
	 * from's fields where the table now has them, and null in those it added)
	 */
	if (a->was != NULL && had == 1 && has == 1)
	{
		Buf			held_was = BUF_INIT;
		Buf			held_to = BUF_INIT;
		bool		same = true;

		MemoryContextReset(w->rowcxt);
		{
			MemoryContext old = MemoryContextSwitchTo(w->rowcxt);

			ws_row_load(w->ctx, a->was, d->key, d->klen, d->from, d->flen, a->was_f, &held_was);
			ws_row_load(w->ctx, &w->shape, d->key, d->klen, d->to, d->tlen, w->f, &held_to);
			MemoryContextSwitchTo(old);
		}
		for (int i = 0; i < w->shape.nstored; i++)
		{
			const WsField *now = a->was_at[i] >= 0 ? &a->was_f[a->was_at[i]] : NULL;

			a->now_f[i] = now != NULL ? *now : (WsField) {.null = true};
			if (same && (a->now_f[i].null != w->f[i].null ||
						 (!w->f[i].null && (a->now_f[i].len != w->f[i].len ||
											memcmp(a->now_f[i].p, w->f[i].p, w->f[i].len) != 0))))
				same = false;
		}
		if (same)
		{
			if (!a->inserting)
				a->kept++;
			return;
		}
	}

	if (a->inserting)
	{
		/* (a keyed row changed is put in again whole, as it was taken away) */
		uint64		more = w->shape.nkey == 0 ? (has > had ? has - had : 0) : has;

		if (more > 0)
		{
			(void) writer_fill(w, d->key, d->klen, d->to, d->tlen);
			for (uint64 i = 0; i < more; i++)
				writer_insert(w);
			a->inserted += more;
		}
		return;
	}
	if (had == 0 || (w->shape.nkey == 0 && had <= has))
		return;
	if (a->was != NULL)
	{
		Buf			held = BUF_INIT;

		MemoryContextReset(w->rowcxt);
		{
			MemoryContext old = MemoryContextSwitchTo(w->rowcxt);

			ws_row_load(w->ctx, a->was, d->key, d->klen, d->from, d->flen, a->was_f, &held);
			MemoryContextSwitchTo(old);
		}
		for (int i = 0; i < w->shape.nstored; i++)
			a->now_f[i] = a->was_at[i] >= 0 ? a->was_f[a->was_at[i]] : (WsField) {.null = true};
		writer_fill_fields(w, a->now_f);
	}
	else
		(void) writer_fill(w, d->key, d->klen, d->from, d->flen);
	if (w->shape.nkey == 0)
	{
		StringInfoData bytes;
		DoomedRow  *row;

		if (a->ndoomed == a->doomed_cap)
		{
			a->doomed_cap = Max(16, a->doomed_cap * 2);
			a->doomed = a->doomed == NULL ? palloc_array(DoomedRow, a->doomed_cap) :
				repalloc_array(a->doomed, DoomedRow, a->doomed_cap);
		}
		initStringInfo(&bytes);
		row_print_bytes(w->f, w->shape.nstored, &bytes);
		row = &a->doomed[a->ndoomed++];
		row->bytes = bytes.data;
		row->len = bytes.len;
		row->left = had - has;
		return;
	}
	{
		/* (what finding it takes is the row's) */
		MemoryContext old = MemoryContextSwitchTo(w->rowcxt);
		bool		found = RelationFindReplTupleByIndex(w->rel, w->key_index, true, LockTupleExclusive,
														 w->slot, a->found);

		MemoryContextSwitchTo(old);
		if (!found)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("table \"%s\" has no row with a key its rows were said to have", w->table)));
	}
	simple_table_tuple_delete(w->rel, &a->found->tts_tid, GetActiveSnapshot());
	a->deleted++;
}

/* The keyless rows noted taken away, as many of each as there are to be, in one scan. */
static void
apply_keyless_deletes(RowsApply *a)
{
	RowsWriter *w = a->w;
	TableScanDesc scan;
	StringInfoData bytes;
	MemoryContext row_cxt;
	WsField    *f;
	int			level;
	uint64		left = 0;

	if (a->ndoomed == 0)
		return;

	/* (no two of them print alike, as one row's text is always one key: ws_row.h) */
	qsort(a->doomed, a->ndoomed, sizeof(DoomedRow), doomed_cmp);
	for (int i = 0; i < a->ndoomed; i++)
		left += a->doomed[i].left;
	row_cxt = AllocSetContextCreate(CurrentMemoryContext, "ws_rows printed row", ALLOCSET_DEFAULT_SIZES);
	f = palloc_array(WsField, Max(w->shape.nstored, 1));
	initStringInfo(&bytes);

	/* (each row as capture prints it, by which a keyless row is told apart) */
	level = WsSql->begin();
	scan = table_beginscan(w->rel, GetActiveSnapshot(), 0, NULL, SO_NONE);
	while (left > 0 && table_scan_getnextslot(scan, ForwardScanDirection, a->found))
	{
		MemoryContext old = MemoryContextSwitchTo(row_cxt);
		DoomedRow	key;
		DoomedRow  *hit;

		CHECK_FOR_INTERRUPTS();
		slot_getallattrs(a->found);
		for (int i = 0; i < w->shape.nstored; i++)
		{
			int			att = w->cols[i].attnum - 1;

			f[i].null = a->found->tts_isnull[att];
			if (!f[i].null)
			{
				char	   *text = ws_text_to_utf8(OutputFunctionCall(&w->cols[i].output, a->found->tts_values[att]));

				f[i].p = (const uint8 *) text;
				f[i].len = strlen(text);
			}
		}
		resetStringInfo(&bytes);
		row_print_bytes(f, w->shape.nstored, &bytes);
		key.bytes = bytes.data;
		key.len = bytes.len;
		hit = bsearch(&key, a->doomed, a->ndoomed, sizeof(DoomedRow), doomed_cmp);
		MemoryContextSwitchTo(old);
		if (hit != NULL && hit->left > 0)
		{
			simple_table_tuple_delete(w->rel, &a->found->tts_tid, GetActiveSnapshot());
			hit->left--;
			left--;
			a->deleted++;
		}
		MemoryContextReset(row_cxt);
	}
	table_endscan(scan);
	WsSql->end(level);
	if (left > 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("table \"%s\" has fewer of a row than its rows were said to have", w->table)));
	MemoryContextDelete(row_cxt);
	pfree(f);
	pfree(bytes.data);
}

static void
direct_apply(ProllyCtx *ctx, const char *schema, const WsTableSchema *was, const PHash *from,
			 const WsTableSchema *s, const PHash *to, uint64 *deleted, uint64 *inserted, uint64 *kept)
{
	RowsWriter	w;
	RowsApply	a = {&w};
	int			level;
	WsRowShape	was_shape;

	*deleted = 0;
	*inserted = 0;
	*kept = 0;
	if (!writer_open(&w, ctx, schema, s, true, "moving rows"))
	{
		uint64		count;

		if (prolly_count(ctx, to, &count) < 0)
			prolly_report(ctx);
		if (count != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("partitioned table \"%s\" holds rows of its own", w.table)));
		writer_close(&w);
		return;
	}
	if (w.shape.nkey > 0 && !OidIsValid(w.key_index))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("table \"%s\" has no primary key, and its rows are keyed", w.table)));
	a.found = table_slot_create(w.rel, NULL);

	/* (from's rows in its own record's shape, which only a table with a key can differ from) */
	ws_schema_row_shape(was, &was_shape);
	if (was_shape.nstored != w.shape.nstored ||
		memcmp(was_shape.tags, w.shape.tags, sizeof(uint64) * w.shape.nstored) != 0)
	{
		if (w.shape.nkey == 0 || was_shape.nkey != w.shape.nkey ||
			memcmp(was_shape.tags, w.shape.tags, sizeof(uint64) * w.shape.nkey) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("rows of table \"%s\" can be moved from another layout only by the same key", w.table)));
		a.was = &was_shape;
		a.was_at = palloc_array(int, Max(w.shape.nstored, 1));
		for (int i = 0; i < w.shape.nstored; i++)
			a.was_at[i] = ws_row_shape_at(&was_shape, w.shape.tags[i]);
		a.was_f = palloc_array(WsField, Max(was_shape.nstored, 1));
		a.now_f = palloc_array(WsField, Max(w.shape.nstored, 1));
	}

	/* (every row taken away before any is put in, so that one can have what another had) */
	level = WsSql->begin();
	if (prolly_diff(ctx, from, to, apply_diff, &a) < 0)
		prolly_report(ctx);
	apply_keyless_deletes(&a);
	CommandCounterIncrement();
	w.cid = GetCurrentCommandId(true);
	a.inserting = true;
	if (prolly_diff(ctx, from, to, apply_diff, &a) < 0)
		prolly_report(ctx);
	WsSql->end(level);

	ExecDropSingleTupleTableSlot(a.found);
	writer_close(&w);
	*deleted = a.deleted;
	*inserted = a.inserted;
	*kept = a.kept;
	if (a.was != NULL)
	{
		pfree(a.was_at);
		pfree(a.was_f);
		pfree(a.now_f);
	}
	ws_row_shape_free(&was_shape);
}

const WsRowsRoutine DirectWsRows = {
	.name = "direct",
	.capture = direct_capture,
	.restore = direct_restore,
	.apply = direct_apply,
};

const WsRowsRoutine *WsRows = &DirectWsRows;
