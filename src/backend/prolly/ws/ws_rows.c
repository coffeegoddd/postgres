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
#include "utils/acl.h"
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

/*
 * Keyless rows the same are one entry with their count, with what the map has
 * of them from earlier batches.  How many entries there now are.
 */
static size_t
keyless_entries(CaptureBatch *b, ProllyEdit *edits, size_t n)
{
	size_t		out = 0;

	for (size_t i = 0; i < n;)
	{
		size_t		same = i + 1;
		uint64		count;
		const uint8 *held;
		size_t		heldlen;
		Buf			count_bytes = BUF_INIT;
		Buf			val = BUF_INIT;
		int			rc;

		while (same < n && edit_cmp(&edits[i], &edits[same]) == 0)
			same++;
		count = same - i;
		rc = prolly_get(b->ctx, &b->map, edits[i].key, edits[i].klen, &held, &heldlen);
		if (rc < 0)
			prolly_report(b->ctx);
		if (rc == PROLLY_OK)
			count += ws_rows_keyless_count(edits[i].key, edits[i].klen, held, heldlen);
		buf_append_uvarint(&count_bytes, count);
		WsRowCodec->keyless_value(edits[i].key, edits[i].klen, count_bytes.data, count_bytes.len, &val);
		edits[out] = edits[i];
		edits[out].val = val.data;
		edits[out].vlen = val.len;
		out++;
		buf_free(&count_bytes);
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
	if (n > 1)
		qsort(edits, n, sizeof(ProllyEdit), edit_cmp);
	if (s->nkey == 0)
		n = keyless_entries(b, edits, n);
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
} RestoreColumn;

/* A row's column by its base type's input: a domain's checks are user code. */
static void
restore_column_init(RestoreColumn *c, Form_pg_attribute att)
{
	int32		typmod = att->atttypmod;
	Oid			base = getBaseTypeAndTypmod(att->atttypid, &typmod);
	Oid			input;

	c->attnum = att->attnum;
	c->typmod = typmod;
	getTypeInputInfo(base, &input, &c->ioparam);
	fmgr_info(input, &c->input);
}

static void
direct_restore(ProllyCtx *ctx, const char *schema, const WsTableSchema *s, const PHash *map)
{
	char	   *table = ws_name_from_utf8(s->name);
	Oid			nsp = get_namespace_oid(schema, false);
	Oid			relid = get_relname_relid(table, nsp);
	Relation	rel;
	TupleDesc	desc;
	WsRowShape	shape;
	RestoreColumn *cols;
	int			live = 0;
	EState	   *estate;
	ResultRelInfo *rri;
	TupleTableSlot *slot;
	CommandId	cid;
	ProllyIter *it;
	const uint8 *k,
			   *v;
	size_t		kl,
				vl;
	WsField    *f;
	int			rc;
	int			level;

	if (!OidIsValid(relid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("table \"%s\" in schema \"%s\" does not exist", table, schema)));
	if (s->partition != WS_PARTITION_NONE)
	{
		uint64		count;

		if (prolly_count(ctx, map, &count) < 0)
			prolly_report(ctx);
		if (count != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("partitioned table \"%s\" holds rows of its own", table)));
		pfree(table);
		return;
	}
	rel = table_open(relid, RowExclusiveLock);
	if (!object_ownercheck(RelationRelationId, relid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLE, table);
	if (rel->rd_rel->relkind != RELKIND_RELATION)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table to hold rows", table)));
	if (rel->rd_rel->relispartition)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("rows cannot be restored into \"%s\" while it is attached", table),
				 errhint("Restore a partition's rows before attaching it, whose check of its bound stands behind them.")));

	/* each stored column of the record, and no other, by its tag */
	desc = RelationGetDescr(rel);
	ws_schema_row_shape(s, &shape);
	cols = palloc0_array(RestoreColumn, Max(shape.nstored, 1));
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);
		int			at;

		if (att->attisdropped || att->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL)
			continue;
		at = ws_row_shape_at(&shape, (uint64) att->attcoltag);
		if (at < 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("column \"%s\" of table \"%s\" is not one its rows hold",
							NameStr(att->attname), table)));
		restore_column_init(&cols[at], att);
		live++;
	}
	if (live != shape.nstored)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("table \"%s\" has %d of the %d columns its rows hold",
						table, live, shape.nstored)));

	estate = CreateExecutorState();
	rri = makeNode(ResultRelInfo);
	InitResultRelInfo(rri, rel, 1, NULL, 0);
	ExecOpenIndices(rri, false);
	slot = table_slot_create(rel, NULL);
	cid = GetCurrentCommandId(true);
	f = palloc_array(WsField, Max(shape.nstored, 1));

	level = WsSql->begin();
	if (prolly_iter_open(ctx, map, NULL, 0, &it) < 0)
		prolly_report(ctx);
	while ((rc = prolly_iter_next(it, &k, &kl, &v, &vl)) > 0)
	{
		MemoryContext old;
		uint64		count = 1;
		Buf			held = BUF_INIT;	/* (the tuple's, as all else of the row) */

		CHECK_FOR_INTERRUPTS();
		ResetPerTupleExprContext(estate);
		old = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
		ws_row_load(ctx, &shape, k, kl, v, vl, f, &held);
		if (shape.nkey == 0)
			count = ws_rows_keyless_count(k, kl, v, vl);
		ExecClearTuple(slot);
		memset(slot->tts_isnull, true, desc->natts * sizeof(bool));
		for (int i = 0; i < shape.nstored; i++)
			if (!f[i].null)
			{
				char	   *text = pnstrdup((const char *) f[i].p, f[i].len);
				int			att = cols[i].attnum - 1;

				slot->tts_values[att] = InputFunctionCall(&cols[i].input, ws_text_from_utf8(text),
														  cols[i].ioparam, cols[i].typmod);
				slot->tts_isnull[att] = false;
			}
		ExecStoreVirtualTuple(slot);
		for (uint64 i = 0; i < count; i++)
		{
			table_tuple_insert(rel, slot, cid, 0, NULL);
			if (rri->ri_NumIndices > 0)
				(void) ExecInsertIndexTuples(rri, estate, 0, slot, NIL, NULL);
		}
		MemoryContextSwitchTo(old);
	}
	prolly_iter_close(it);
	if (rc < 0)
		prolly_report(ctx);
	WsSql->end(level);

	ExecDropSingleTupleTableSlot(slot);
	ExecCloseIndices(rri);
	FreeExecutorState(estate);
	table_close(rel, NoLock);
	CommandCounterIncrement();
	ws_row_shape_free(&shape);
	pfree(f);
	pfree(cols);
	pfree(table);
}

const WsRowsRoutine DirectWsRows = {
	.name = "direct",
	.capture = direct_capture,
	.restore = direct_restore,
};

const WsRowsRoutine *WsRows = &DirectWsRows;
