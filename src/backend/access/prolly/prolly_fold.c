/*-------------------------------------------------------------------------
 *
 * prolly_fold.c
 *	  What the change finders share about folding a table in full: why a
 *	  fold read every row, counted and logged; and a baseline mapped to its
 *	  table's shape now (prolly_fold.h).
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_fold.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/prolly_changes.h"
#include "access/prolly_config.h"
#include "access/prolly_fold.h"
#include "access/tableam.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_coerce.h"
#include "pgstat.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tuplesort.h"

bool		debug_prolly_force_full_fold = false;
int			prolly_log_full_folds = -1;
ProllyFullFold prolly_last_full_fold;

static const char *const reason_names[PROLLY_FULL_REASONS] = {
	[PROLLY_FULL_NONE] = "none",
	[PROLLY_FULL_NO_BASELINE] = "no_baseline",
	[PROLLY_FULL_SHAPE_CHANGED] = "shape_changed",
	[PROLLY_FULL_NO_LOG] = "no_log",
	[PROLLY_FULL_LOG_CHANGED] = "log_changed",
	[PROLLY_FULL_STALE_SNAPSHOT] = "stale_snapshot",
	[PROLLY_FULL_UNSETTLED] = "unsettled",
	[PROLLY_FULL_STORAGE_UNSEEN] = "storage_unseen",
	[PROLLY_FULL_RECOVERY] = "recovery",
	[PROLLY_FULL_LOG_ENTRY] = "log_entry",
	[PROLLY_FULL_TOO_MANY] = "too_many",
	[PROLLY_FULL_KEYING_CHANGED] = "keying_changed",
	[PROLLY_FULL_UNSUPPORTED_KEY] = "unsupported_key",
	[PROLLY_FULL_MEMORY_BUDGET] = "memory_budget",
	[PROLLY_FULL_FORCED] = "forced",
	[PROLLY_FULL_VERIFY] = "verify",
};

StaticAssertDecl(PGSTAT_PROLLY_FOLD_FULL_VERIFY - PGSTAT_PROLLY_FOLD_FULL_NO_BASELINE ==
				 PROLLY_FULL_VERIFY - PROLLY_FULL_NO_BASELINE,
				 "a counter for each full fold's reason, in the reasons' order");

const char *
prolly_full_fold_reason_name(ProllyFullFoldReason reason)
{
	if ((int) reason < 0 || reason >= PROLLY_FULL_REASONS)
		return "unknown";
	return reason_names[reason];
}

void
prolly_full_fold_noted(Relation rel, ProllyFullFoldReason reason, int64 rows,
					   double ms)
{
	if (reason == PROLLY_FULL_NONE)
		return;
	pgstat_count_prolly(PGSTAT_PROLLY_FOLD_FULL_NO_BASELINE +
						(reason - PROLLY_FULL_NO_BASELINE), 1);
	if (prolly_log_full_folds >= 0 && rows >= prolly_log_full_folds)
		ereport(LOG,
				(errmsg("prolly folded table \"%s.%s\" in full: %s, %lld rows read in %.0f ms",
						get_namespace_name(RelationGetNamespace(rel)),
						RelationGetRelationName(rel),
						prolly_full_fold_reason_name(reason),
						(long long) rows, ms)));
}

/* ----------------------------------------------------------------
 * Sorting a table's rows by key: a tuplesort of its own
 * ----------------------------------------------------------------
 */

/*
 * A row to sort: its copies (a keyless row's count), its key's and its
 * encoded row's lengths, then both.
 */
typedef struct FoldTuple
{
	uint64		copies;
	uint32		keylen;
	uint32		rowlen;
	char		data[FLEXIBLE_ARRAY_MEMBER];
} FoldTuple;

typedef struct FoldSortArg
{
	const ProllyKeyingRoutine *kr;
	ProllyKeying *k;
} FoldSortArg;

static int
comparetup_fold(const SortTuple *a, const SortTuple *b, Tuplesortstate *state)
{
	FoldSortArg *arg = TuplesortstateGetPublic(state)->arg;
	const FoldTuple *x = a->tuple;
	const FoldTuple *y = b->tuple;
	ProllyKey	kx = {(const uint8 *) x->data, x->keylen};
	ProllyKey	ky = {(const uint8 *) y->data, y->keylen};

	return PROLLY_CALL(arg->kr, compare, arg->k, &kx, &ky);
}

static void
writetup_fold(Tuplesortstate *state, LogicalTape *tape, SortTuple *stup)
{
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	const FoldTuple *t = stup->tuple;
	unsigned int tuplen = offsetof(FoldTuple, data) + t->keylen + t->rowlen;
	unsigned int written = tuplen + sizeof(unsigned int);

	LogicalTapeWrite(tape, &written, sizeof(written));
	LogicalTapeWrite(tape, t, tuplen);
	if (base->sortopt & TUPLESORT_RANDOMACCESS)
		LogicalTapeWrite(tape, &written, sizeof(written));
}

static void
readtup_fold(Tuplesortstate *state, SortTuple *stup, LogicalTape *tape,
			 unsigned int len)
{
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	unsigned int tuplen = len - sizeof(unsigned int);
	FoldTuple  *t = tuplesort_readtup_alloc(state, tuplen);

	LogicalTapeReadExact(tape, t, tuplen);
	stup->tuple = t;
	stup->datum1 = (Datum) 0;
	stup->isnull1 = true;
	if (base->sortopt & TUPLESORT_RANDOMACCESS)
		LogicalTapeReadExact(tape, &tuplen, sizeof(tuplen));
}

static void
removeabbrev_fold(Tuplesortstate *state, SortTuple *stups, int count)
{
}

static Tuplesortstate *
fold_sort_begin(FoldSortArg *arg)
{
	Tuplesortstate *state = tuplesort_begin_common(maintenance_work_mem, NULL,
												   TUPLESORT_NONE);
	TuplesortPublic *base = TuplesortstateGetPublic(state);

	base->nKeys = 1;
	base->removeabbrev = removeabbrev_fold;
	base->comparetup = comparetup_fold;
	base->comparetup_tiebreak = comparetup_fold;
	base->writetup = writetup_fold;
	base->readtup = readtup_fold;
	base->haveDatum1 = false;
	base->tuples = true;
	base->arg = arg;
	return state;
}

static void
fold_sort_put(Tuplesortstate *state, const StringInfoData *key,
			  const StringInfoData *row, uint64 copies)
{
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	MemoryContext old = MemoryContextSwitchTo(base->tuplecontext);
	Size		len = offsetof(FoldTuple, data) + key->len + row->len;
	FoldTuple  *t = palloc(len);
	SortTuple	stup;

	t->copies = copies;
	t->keylen = key->len;
	t->rowlen = row->len;
	memcpy(t->data, key->data, key->len);
	memcpy(t->data + key->len, row->data, row->len);
	stup.tuple = t;
	stup.datum1 = (Datum) 0;
	stup.isnull1 = true;
	/* (the tuple context is a bump allocator, which knows no chunk sizes) */
	tuplesort_puttuple_common(state, &stup, false,
							  TupleSortUseBumpTupleCxt(base->sortopt) ?
							  MAXALIGN(len) : GetMemoryChunkSpace(t));
	MemoryContextSwitchTo(old);
}

static const FoldTuple *
fold_sort_next(Tuplesortstate *state)
{
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	MemoryContext old = MemoryContextSwitchTo(base->sortcontext);
	SortTuple	stup;
	bool		got = tuplesort_gettuple_common(state, true, &stup);

	MemoryContextSwitchTo(old);
	return got ? stup.tuple : NULL;
}

/*
 * Rebuild a keyed historical tree under today's comparator.  Walk from the
 * beginning, without seeking by key in the old order, and preserve encoded
 * rows and their long-value references.  The sort spills at maintenance_work_mem.
 */
bool
prolly_fold_reorder(const ProllyKeyingRoutine *kr, ProllyKeying *k,
					const ProllyTreeRoutine *tree, const ProllyTreeEnv *env,
					const ProllyNodeRef *from, ProllyNodeRef *data)
{
	FoldSortArg arg = {kr, k};
	Tuplesortstate *sort = fold_sort_begin(&arg);
	ProllyTreeCursor *cur;
	ProllyTreeBuilder *builder;
	ProllyKey	key;
	ProllyValue value;
	StringInfoData prev;
	const FoldTuple *t;
	bool		unique = true;
	ProllyNodeRef reordered;

	cur = PROLLY_CALL(tree, cursor_open, env, from, CurrentMemoryContext);
	PROLLY_CALL(tree, cursor_seek, cur, NULL, ForwardScanDirection);
	while (PROLLY_CALL(tree, cursor_next, cur, &key, &value))
	{
		StringInfoData kb = {(char *) key.data, key.len, key.len, 0};
		StringInfoData vb = {(char *) value.data, value.len, value.len, 0};

		fold_sort_put(sort, &kb, &vb, 1);
		CHECK_FOR_INTERRUPTS();
	}
	PROLLY_CALL(tree, cursor_close, cur);
	tuplesort_performsort(sort);
	initStringInfo(&prev);
	builder = PROLLY_CALL(tree, build_begin, env, CurrentMemoryContext);
	while ((t = fold_sort_next(sort)) != NULL)
	{
		ProllyKey	before = {(const uint8 *) prev.data, prev.len};

		key.data = (const uint8 *) t->data;
		key.len = t->keylen;
		value.data = (const uint8 *) t->data + t->keylen;
		value.len = t->rowlen;
		if (prev.len && PROLLY_CALL(kr, compare, k, &before, &key) == 0)
		{
			unique = false;
			break;
		}
		PROLLY_CALL(tree, build_add, builder, &key, &value);
		resetStringInfo(&prev);
		appendBinaryStringInfo(&prev, (const char *) key.data, key.len);
		CHECK_FOR_INTERRUPTS();
	}
	PROLLY_CALL(tree, build_finish, builder, &reordered);
	if (unique)
		*data = reordered;
	pfree(prev.data);
	tuplesort_end(sort);
	return unique;
}

/*
 * The sorted rows built into *data, in key order: a keyless row's copies,
 * one after another, counted.  False at a keyed table's second row of a
 * key, *data then the part built before it, not to be used.  done gets
 * the rows written and what the sort took.
 */
static bool
fold_sort_build(Tuplesortstate *sort, const ProllyKeyingRoutine *kr,
				ProllyKeying *k, bool keyless, const ProllyTreeRoutine *tree,
				const ProllyTreeEnv *env, ProllyNodeRef *data,
				ProllyFullFold *done)
{
	StringInfoData prev_key;
	StringInfoData prev_row;
	StringInfoData value;
	ProllyTreeBuilder *b;
	TuplesortInstrumentation stats;
	const FoldTuple *t;
	uint64		copies = 0;
	bool		unique = true;

	tuplesort_performsort(sort);
	initStringInfo(&prev_key);
	initStringInfo(&prev_row);
	initStringInfo(&value);
	b = PROLLY_CALL(tree, build_begin, env, CurrentMemoryContext);
	for (;;)
	{
		t = fold_sort_next(sort);
		if (copies > 0 &&
			(t == NULL || t->keylen != (uint32) prev_key.len ||
			 memcmp(t->data, prev_key.data, prev_key.len) != 0))
		{
			ProllyKey	pk = {(const uint8 *) prev_key.data, prev_key.len};
			ProllyValue pr = {(const uint8 *) prev_row.data, prev_row.len};
			ProllyValue v;

			resetStringInfo(&value);
			PROLLY_CALL(kr, make_value, k, &pr, copies, &value);
			v.data = (const uint8 *) value.data;
			v.len = value.len;
			PROLLY_CALL(tree, build_add, b, &pk, &v);
			done->rows_written++;
			copies = 0;
		}
		if (t == NULL)
			break;
		if (copies > 0 && !keyless)
		{
			unique = false;
			break;
		}
		if (copies == 0)
		{
			resetStringInfo(&prev_key);
			appendBinaryStringInfo(&prev_key, t->data, t->keylen);
			resetStringInfo(&prev_row);
			appendBinaryStringInfo(&prev_row, t->data + t->keylen, t->rowlen);
		}
		copies += t->copies;
		CHECK_FOR_INTERRUPTS();
	}
	/* (a tree begun is finished, used or not) */
	PROLLY_CALL(tree, build_finish, b, data);
	tuplesort_get_stats(sort, &stats);
	done->spilled = stats.spaceType == SORT_SPACE_TYPE_DISK;
	done->sort_kb = stats.spaceUsed;
	pfree(prev_key.data);
	pfree(prev_row.data);
	pfree(value.data);
	return unique;
}

void
prolly_fold_all(Relation rel, Snapshot snap, TupleTableSlot *slot,
				const ProllyKeyingRoutine *kr, ProllyKeying *k, bool keyless,
				const ProllyTreeRoutine *tree, const ProllyTreeEnv *env,
				ProllyNodeRef *data, ProllyFullFold *done)
{
	FoldSortArg arg = {kr, k};
	Tuplesortstate *sort = fold_sort_begin(&arg);
	TableScanDesc scan;
	StringInfoData key;
	StringInfoData row;

	memset(done, 0, sizeof(*done));
	initStringInfo(&key);
	initStringInfo(&row);
	scan = table_beginscan(rel, snap, 0, NULL, SO_NONE);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		resetStringInfo(&key);
		resetStringInfo(&row);
		PROLLY_CALL(kr, encode_row, k, slot, &key, &row, NULL);
		fold_sort_put(sort, &key, &row, 1);
		done->rows_read++;
		CHECK_FOR_INTERRUPTS();
	}
	table_endscan(scan);
	if (!fold_sort_build(sort, kr, k, keyless, tree, env, data, done))
		ereport(ERROR,
				(errcode(ERRCODE_UNIQUE_VIOLATION),
				 errmsg("table \"%s\" holds rows with the same primary key",
						RelationGetRelationName(rel)),
				 errdetail("A commit can't hold them until the key is unique again.")));
	tuplesort_end(sort);
	if (done->spilled)
		pgstat_count_prolly(PGSTAT_PROLLY_FOLD_FULL_SPILLED, 1);
	prolly_last_full_fold = *done;
}

/* ----------------------------------------------------------------
 * A baseline's rows mapped to the table's shape now
 * ----------------------------------------------------------------
 */


/* How a column of rel's rows now is had from a row as it was folded. */
typedef enum MapKind
{
	MAP_NONE,					/* dropped: none */
	MAP_SAME,					/* the column as it was */
	MAP_MISSING,				/* added since: its missing value, or NULL */
} MapKind;

typedef struct MapColumn
{
	MapKind		kind;
	Datum		missing;
	bool		isnull;
} MapColumn;

/* A map in progress: the keyings both ways, and what each pass reuses. */
typedef struct Map
{
	Relation	rel;
	const ProllyKeyingRoutine *kr;
	const ProllyTreeRoutine *tree;
	ProllyKeying *ok;			/* the rows as folded */
	ProllyKeying *k;			/* the rows now */
	bool		keyless;
	MapColumn  *cols;
	ProllyTreeEnv oenv;
	ProllyTreeEnv nenv;
	ProllyKeyingRefs orefs;
	ProllyKeyingRefs nrefs;
	TupleTableSlot *oslot;
	TupleTableSlot *nslot;
	MemoryContext rowcxt;
	int64		rows;
} Map;

/* How a pass ended. */
typedef enum MapEnd
{
	MAP_DONE,
	MAP_UNORDERED,				/* keys came out of order: sort them */
	MAP_FAILED,					/* a row can't be mapped */
} MapEnd;

static int
map_compare(const ProllyKey *a, const ProllyKey *b, void *arg)
{
	Map		   *m = arg;

	return PROLLY_CALL(m->kr, compare, m->k, a, b);
}

/*
 * Does a value of type was keep its datum as a value of type now, as
 * ALTER COLUMN TYPE casts it by default?  When its cast reduces to a
 * relabeling (a binary-coercible type, a domain over it, a length or
 * precision only widened, whose support function says so): Postgres
 * rewrites no row then, so the rows hold the old datums.  (A cast that
 * computes is a rewrite, whose log entry reads the table in full; and a
 * USING expression makes one, so the cast here needn't be the one used.
 * Between timestamp and timestamptz whether a rewrite is made depends on
 * the session's time zone: not mapped.)
 */
static bool
cast_keeps_datum(Oid was, int32 wastypmod, Oid now, int32 nowtypmod)
{
	Node	   *e = (Node *) makeVar(1, 1, was, wastypmod, InvalidOid, 0);

	e = coerce_to_target_type(NULL, e, was, now, nowtypmod,
							  COERCION_ASSIGNMENT, COERCE_IMPLICIT_CAST, -1);
	if (e == NULL)
		return false;
	e = (Node *) expression_planner((Expr *) e);
	for (;;)
	{
		if (IsA(e, Var))
			return true;
		else if (IsA(e, RelabelType))
			e = (Node *) ((RelabelType *) e)->arg;
		else if (IsA(e, CoerceToDomain))
			e = (Node *) ((CoerceToDomain *) e)->arg;
		else
			return false;
	}
}

/*
 * How each of rel's columns now is had from a row of desc's columns (a
 * row as folded, prolly_keying open_shape()): false if one can't be.
 */
static bool
map_columns(Relation rel, TupleDesc desc, MapColumn *cols)
{
	TupleDesc	rd = RelationGetDescr(rel);

	for (int i = 0; i < rd->natts; i++)
	{
		Form_pg_attribute now = TupleDescAttr(rd, i);
		Form_pg_attribute was = TupleDescAttr(desc, i);

		memset(&cols[i], 0, sizeof(MapColumn));
		cols[i].isnull = true;
		if (now->attisdropped)
			cols[i].kind = MAP_NONE;
		else if (was->attisdropped)
		{
			/* (rows read it as Postgres reads a row from before it) */
			cols[i].kind = MAP_MISSING;
			if (now->atthasmissing)
			{
				Datum		d = getmissingattr(rd, i + 1, &cols[i].isnull);

				if (!cols[i].isnull)
					cols[i].missing = datumCopy(d, now->attbyval, now->attlen);
			}
		}
		else if ((was->atttypid == now->atttypid &&
				  was->atttypmod == now->atttypmod) ||
				 cast_keeps_datum(was->atttypid, was->atttypmod,
								  now->atttypid, now->atttypmod))
			cols[i].kind = MAP_SAME;
		else
			return false;
	}
	return true;
}

/*
 * The keyings to read rows folded under old by (*ok) and to encode them as
 * rel's now (*k), and how each column of rel's is had from one of the old
 * rows (*cols, of *odesc's): false if they can't be, as for
 * prolly_fold_map().  Opened in cxt.
 */
static bool
map_open(Relation rel, const ProllyKeyingRoutine *kr, const ProllyValue *old,
		 MemoryContext cxt, ProllyKeying **ok, ProllyKeying **k,
		 TupleDesc *odesc, MapColumn **cols)
{
	if (!PROLLY_HAS(kr, open_shape))
		return false;
	*ok = PROLLY_CALL(kr, open_shape, rel, old, cxt, odesc);
	if (*ok == NULL)
		return false;
	*k = PROLLY_CALL(kr, open, rel, cxt);
	*cols = MemoryContextAlloc(cxt, sizeof(MapColumn) *
							   Max(RelationGetDescr(rel)->natts, 1));
	if (!map_columns(rel, *odesc, *cols))
	{
		PROLLY_CALL(kr, close, *ok);
		PROLLY_CALL(kr, close, *k);
		return false;
	}
	return true;
}

bool
prolly_fold_mappable(Relation rel, const ProllyValue *old)
{
	ProllyComponents c;
	MemoryContext cxt;
	MemoryContext caller;
	ProllyKeying *ok;
	ProllyKeying *k;
	TupleDesc	odesc;
	MapColumn  *cols;
	bool		mappable;

	prolly_components_for_relation(rel, &c);
	cxt = AllocSetContextCreate(CurrentMemoryContext, "fold mappable",
								ALLOCSET_DEFAULT_SIZES);
	caller = MemoryContextSwitchTo(cxt);
	mappable = map_open(rel, c.keying, old, cxt, &ok, &k, &odesc, &cols);
	if (mappable)
	{
		PROLLY_CALL(c.keying, close, ok);
		PROLLY_CALL(c.keying, close, k);
	}
	MemoryContextSwitchTo(caller);
	MemoryContextDelete(cxt);
	return mappable;
}

/*
 * One pass over the baseline's tree from: each row mapped, and built into
 * *data as it comes (sorted: false), or sorted first (true).  Unsorted, a
 * key out of order ends it (MAP_UNORDERED); sorted, a keyed table's key
 * twice fails it, as does a keyless row's copies for a keyed table (the
 * rows aren't the table's: it can't hold them).
 */
static MapEnd
map_pass(Map *m, const ProllyNodeRef *from, bool sorted, ProllyNodeRef *data)
{
	TupleDesc	rd = RelationGetDescr(m->rel);
	ProllyTreeCursor *cur;
	ProllyTreeBuilder *b = NULL;
	Tuplesortstate *sort = NULL;
	FoldSortArg arg = {m->kr, m->k};
	ProllyFullFold done = {0};
	StringInfoData key;
	StringInfoData row;
	StringInfoData value;
	StringInfoData prev;
	ProllyKey	k_in;
	ProllyValue v_in;
	MapEnd		end = MAP_DONE;

	initStringInfo(&key);
	initStringInfo(&row);
	initStringInfo(&value);
	initStringInfo(&prev);
	m->rows = 0;
	if (sorted)
		sort = fold_sort_begin(&arg);
	else
		b = PROLLY_CALL(m->tree, build_begin, &m->nenv, CurrentMemoryContext);
	cur = PROLLY_CALL(m->tree, cursor_open, &m->oenv, from, CurrentMemoryContext);
	PROLLY_CALL(m->tree, cursor_seek, cur, NULL, ForwardScanDirection);
	while (end == MAP_DONE &&
		   PROLLY_CALL(m->tree, cursor_next, cur, &k_in, &v_in))
	{
		MemoryContext old;
		ProllyValue r;
		uint64		count;

		MemoryContextReset(m->rowcxt);
		old = MemoryContextSwitchTo(m->rowcxt);
		PROLLY_CALL(m->kr, parse_value, m->ok, &v_in, &r, &count);
		PROLLY_CALL(m->kr, decode_row, m->ok, &r, m->oslot);
		slot_getallattrs(m->oslot);
		ExecClearTuple(m->nslot);
		for (int i = 0; i < rd->natts; i++)
		{
			m->nslot->tts_values[i] = (Datum) 0;
			m->nslot->tts_isnull[i] = true;
			if (m->cols[i].kind == MAP_SAME)
			{
				m->nslot->tts_values[i] = m->oslot->tts_values[i];
				m->nslot->tts_isnull[i] = m->oslot->tts_isnull[i];
			}
			else if (m->cols[i].kind == MAP_MISSING)
			{
				m->nslot->tts_values[i] = m->cols[i].missing;
				m->nslot->tts_isnull[i] = m->cols[i].isnull;
			}
		}
		ExecStoreVirtualTuple(m->nslot);
		resetStringInfo(&key);
		resetStringInfo(&row);
		PROLLY_CALL(m->kr, encode_row, m->k, m->nslot, &key, &row, NULL);
		MemoryContextSwitchTo(old);
		if (count > 1 && !m->keyless)
			end = MAP_FAILED;
		else if (sorted)
			fold_sort_put(sort, &key, &row, count);
		else
		{
			ProllyKey	nk = {(const uint8 *) key.data, key.len};
			ProllyKey	pk = {(const uint8 *) prev.data, prev.len};
			ProllyValue nv;
			ProllyValue nr = {(const uint8 *) row.data, row.len};

			if (m->rows > 0 && PROLLY_CALL(m->kr, compare, m->k, &pk, &nk) >= 0)
				end = MAP_UNORDERED;
			else
			{
				resetStringInfo(&value);
				PROLLY_CALL(m->kr, make_value, m->k, &nr, 1, &value);
				nv.data = (const uint8 *) value.data;
				nv.len = value.len;
				PROLLY_CALL(m->tree, build_add, b, &nk, &nv);
				resetStringInfo(&prev);
				appendBinaryStringInfo(&prev, key.data, key.len);
			}
		}
		m->rows++;
		CHECK_FOR_INTERRUPTS();
	}
	PROLLY_CALL(m->tree, cursor_close, cur);
	if (sorted)
	{
		if (end == MAP_DONE &&
			!fold_sort_build(sort, m->kr, m->k, m->keyless, m->tree, &m->nenv,
							 data, &done))
			end = MAP_FAILED;
		tuplesort_end(sort);
	}
	else
		PROLLY_CALL(m->tree, build_finish, b, data);
	pfree(key.data);
	pfree(row.data);
	pfree(value.data);
	pfree(prev.data);
	return end;
}

bool
prolly_fold_map(Relation rel, const ProllyTreeEnv *env,
				const ProllyNodeRef *from, const ProllyValue *old,
				ProllyNodeRef *data, int64 *rows)
{
	ProllyComponents c;
	MemoryContext cxt;
	MemoryContext caller;
	TupleDesc	odesc;
	TupleDesc	rd = RelationGetDescr(rel);
	Map			m = {0};
	MapEnd		end;
	ProllyNodeRef built;

	prolly_components_for_relation(rel, &c);
	*rows = 0;

	/* what the map makes is let go of when it's done, but the tree */
	cxt = AllocSetContextCreate(CurrentMemoryContext, "fold map",
								ALLOCSET_DEFAULT_SIZES);
	caller = MemoryContextSwitchTo(cxt);
	m.rel = rel;
	m.kr = c.keying;
	m.tree = c.tree;
	if (!map_open(rel, m.kr, old, cxt, &m.ok, &m.k, &odesc, &m.cols))
	{
		MemoryContextSwitchTo(caller);
		MemoryContextDelete(cxt);
		return false;
	}
	m.keyless = PROLLY_CALL(m.kr, keyless, m.k);

	/*
	 * Rows read as folded, their long values as the trees they are (hash
	 * pointers, not read), and encoded as the keying encodes them now.
	 */
	m.oenv = *env;
	prolly_keying_use_env(&m.orefs, m.kr, m.ok, m.tree, &m.oenv);
	m.orefs.values.pointers = true;
	m.nenv = *env;
	m.nenv.compare = map_compare;
	m.nenv.compare_arg = &m;
	prolly_keying_use_env(&m.nrefs, m.kr, m.k, m.tree, &m.nenv);
	m.oslot = MakeSingleTupleTableSlot(odesc, &TTSOpsVirtual);
	m.nslot = MakeSingleTupleTableSlot(rd, &TTSOpsVirtual);
	m.rowcxt = AllocSetContextCreate(cxt, "fold map row", ALLOCSET_DEFAULT_SIZES);

	/*
	 * A keyless row's key is its bytes, and a table keyed otherwise than
	 * its rows were folded orders them otherwise: sorted again.  Else in the
	 * baseline's order, unless a key comes out of order (a key column's
	 * collation changed, say): then sorted.  (A pass that fails has built
	 * a part of the tree: *data gets only a whole one.)
	 */
	end = map_pass(&m, from,
				   m.keyless || PROLLY_CALL(m.kr, keyless, m.ok) != m.keyless,
				   &built);
	if (end == MAP_UNORDERED)
		end = map_pass(&m, from, true, &built);
	if (end == MAP_DONE)
		*data = built;
	*rows = m.rows;
	ExecDropSingleTupleTableSlot(m.oslot);
	ExecDropSingleTupleTableSlot(m.nslot);
	PROLLY_CALL(m.kr, close, m.ok);
	PROLLY_CALL(m.kr, close, m.k);
	MemoryContextSwitchTo(caller);
	MemoryContextDelete(cxt);
	return end == MAP_DONE;
}

bool
prolly_table_fold_data(Relation rel, const ProllyTreeEnv *env,
					   const ProllyTableFold *baseline, ProllyNodeRef *data)
{
	int64		rows;

	if (!baseline->reshaped)
	{
		*data = baseline->data;
		return true;
	}
	if (!prolly_fold_map(rel, env, &baseline->data, &baseline->shape, data,
						 &rows))
		return false;
	pgstat_count_prolly(PGSTAT_PROLLY_FOLD_MAPPED, 1);
	return true;
}
