/*-------------------------------------------------------------------------
 *
 * prolly_fold.c
 *	  What the change finders share about folding a table in full: why a
 *	  fold read every row, counted and logged (prolly_fold.h).
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_fold.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_fold.h"
#include "access/tableam.h"
#include "miscadmin.h"
#include "pgstat.h"
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

/* A row to sort: its key's and its encoded row's lengths, then both. */
typedef struct FoldTuple
{
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
			  const StringInfoData *row)
{
	TuplesortPublic *base = TuplesortstateGetPublic(state);
	MemoryContext old = MemoryContextSwitchTo(base->tuplecontext);
	Size		len = offsetof(FoldTuple, data) + key->len + row->len;
	FoldTuple  *t = palloc(len);
	SortTuple	stup;

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
	StringInfoData prev_key;
	StringInfoData prev_row;
	StringInfoData value;
	ProllyTreeBuilder *b;
	TuplesortInstrumentation stats;
	const FoldTuple *t;
	uint64		copies = 0;

	memset(done, 0, sizeof(*done));
	initStringInfo(&key);
	initStringInfo(&row);
	scan = table_beginscan(rel, snap, 0, NULL, SO_NONE);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		ProllyHash	content;

		resetStringInfo(&key);
		resetStringInfo(&row);
		PROLLY_CALL(kr, encode_row, k, slot, &key, &row, &content);
		fold_sort_put(sort, &key, &row);
		done->rows_read++;
		CHECK_FOR_INTERRUPTS();
	}
	table_endscan(scan);
	tuplesort_performsort(sort);

	/* in key order: a keyless row's copies, one after another, counted */
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
			ereport(ERROR,
					(errcode(ERRCODE_UNIQUE_VIOLATION),
					 errmsg("table \"%s\" holds rows with the same primary key",
							RelationGetRelationName(rel)),
					 errdetail("A commit can't hold them until the key is unique again.")));
		if (copies == 0)
		{
			resetStringInfo(&prev_key);
			appendBinaryStringInfo(&prev_key, t->data, t->keylen);
			resetStringInfo(&prev_row);
			appendBinaryStringInfo(&prev_row, t->data + t->keylen, t->rowlen);
		}
		copies++;
		CHECK_FOR_INTERRUPTS();
	}
	PROLLY_CALL(tree, build_finish, b, data);
	tuplesort_get_stats(sort, &stats);
	done->spilled = stats.spaceType == SORT_SPACE_TYPE_DISK;
	done->sort_kb = stats.spaceUsed;
	tuplesort_end(sort);
	if (done->spilled)
		pgstat_count_prolly(PGSTAT_PROLLY_FOLD_FULL_SPILLED, 1);
	prolly_last_full_fold = *done;
}
