/*-------------------------------------------------------------------------
 *
 * changes_log.c
 *	  Change finder implementation "log": a table's rows changed since a
 *	  fold, from the entries of its change log.
 *
 * The table's change log (prolly_changelog.h) has an entry for each tuple
 * version a write made or replaced, seen exactly when the write is, and
 * consumed once a fold that saw it is the table's baseline.  So the entries
 * a fold's snapshot sees are the changes since the baseline that the
 * snapshot sees, and those it doesn't see stay for the next fold.
 *
 * With a primary key, each entry's key is a candidate: its old row is the
 * baseline tree's, its new row the one the snapshot sees by the primary key
 * now, and the pairs that differ are the edits.  Without one, each entry
 * is a row's copy made (+1) or replaced (-1), counted against the
 * baseline's count of the row.
 *
 * A table is folded in full instead, every row read, when the baseline
 * isn't one this finder made with the table's log as it is now (none,
 * another finder's, or a log made since: the table had none then), when
 * the snapshot doesn't see the baseline's fold as committed (it may not see
 * what that fold saw), when the baseline's fold was such a fold itself
 * (unsettled: see below), when the table's catalog row was changed by a
 * transaction the snapshot doesn't see (a truncation or rewrite the
 * snapshot doesn't see the entry of, though it reads the new storage), when
 * an entry says to (a row the log couldn't hold, or the table truncated or
 * rewritten), when a keyless table has a key's entry, a row's count would
 * go below none (the keying changed meanwhile) or a key column of an
 * entry is null (it was written under another key), or when a keyless
 * table has more rows changed than it holds.  Many changes (past a tenth
 * of the baseline's rows) are merged with the baseline in one pass, not
 * applied one by one.  Changes past maintenance_work_mem are sorted,
 * spilling to disk (prolly_change_sort_begin()), and read back a part at a
 * time, each part's edits applied to the tree so far, or merged with it
 * once they pass that tenth: the parts follow each other in key order, and
 * a row's copies are never split between them, so the fold's work is still
 * that of the rows changed.  A hot standby folds from the baseline as the
 * primary does: its log and baseline are replayed, its snapshots real; a
 * fold there only reads.  A branch's copy of a table has its
 * source's baseline and a copy of its log, so it folds from them.  Its
 * pages are never kept as of a fold: only sharing between branches copies
 * them.
 *
 * A fold by a snapshot that doesn't see its baseline's fold (a repeatable
 * read transaction's, older than another's fold) starts from the baseline
 * too: the entries the baseline's fold held of transactions this snapshot
 * doesn't see finished are undone (their keys re-read by this snapshot,
 * the version it sees, or none; a keyless row's count taken back), and
 * those since applied as any fold's.  The log keeps them for it: rows are
 * trimmed only once every snapshot sees their transactions finished.  But
 * the baseline it begins, at its own snapshot's position, doesn't hold the
 * changes of the transactions it didn't see finished, and the rows the
 * newer fold consumed (its own transaction's, deleted) are kept only while
 * a snapshot may need them: VACUUM takes them once this one ends.  So when
 * the undoing found such a row, or such a fold read the table in full, its
 * baseline is marked unsettled: the next fold by a snapshot that sees more
 * finished than it held reads the table in full, by its own snapshot (one
 * that sees no more, as the old transaction's later statements, folds from
 * it, and keeps the mark).
 *
 * The entries a fold reads are kept once per row (a key's, or a keyless
 * row's with its count summed), so the memory a fold takes is of the rows
 * changed, not of the writes, and at most about maintenance_work_mem for
 * the sort and half as much for the part read back.  A transaction's own
 * changes (fold_rows) and a read of changes are wanted all at once: past
 * maintenance_work_mem, they give up instead (the working set committed,
 * or the trees compared).
 *
 * A baseline's state: a tag and version, flags, the log's identity, and the
 * fold's snapshot's xmax, the fold's own transaction (if it had written)
 * and the transactions it saw running (a count, then each), as
 * FullTransactionIds: the fold's position.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/changes_log.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/prolly_builtins.h"
#include "access/relscan.h"
#include "access/stratnum.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "common/int.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "portability/instr_time.h"
#include "storage/bmap.h"
#include "storage/bufmgr.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/* (a first byte no "pages" state has: neither reads the other's) */
#define STATE_TAG			'L'
#define STATE_VERSION		3		/* 2: flags; 3: full transaction ids */

#define STATE_UNSETTLED		0x01	/* folded by a snapshot older than its
									 * baseline's fold, which consumed rows
									 * this one's position doesn't hold:
									 * one that sees more folds in full */

/* Entries kept before they are first made one per row. */
#define COMPACT_AT			65536

/* edits past a tenth of the baseline's rows: merged with it, not applied */
#define MERGE_FRACTION		10

typedef struct State
{
	uint8		flags;
	uint64		log;			/* the log's identity then */
	TransactionId xmax;			/* the fold's snapshot's */
	uint32		nxip;			/* the transactions it saw running, sorted */
	TransactionId *xip;
	TransactionId own;			/* the fold's transaction, if it wrote */
	/* the same, whole: the fold's position in the log */
	FullTransactionId fxmax;
	FullTransactionId *fxip;
	FullTransactionId fown;
} State;

static ProllyChangeCounters counters;

/* A row's copies counted, or a key's candidate: its key, row and delta. */
typedef struct Copy
{
	char	   *key;
	uint32		keylen;
	char	   *row;			/* a candidate's: its entry's tuple */
	uint32		rowlen;
	int64		delta;
	ItemPointerData tid;		/* a candidate's: where its key's version is
								 * (invalid: not known), as the log says */
} Copy;

typedef struct Finder
{
	Relation	rel;
	Snapshot	snap;
	const ProllyTreeRoutine *tree;
	ProllyTreeEnv env;			/* the caller's, ordered by the keying */
	const ProllyKeyingRoutine *kr;
	ProllyKeying *k;
	ProllyKeyingRefs krefs;
	bool		keyless;
	TupleDesc	desc;
	Relation	pk;				/* NULL if keyless */
	int			nkeys;
	AttrNumber	keyattrs[INDEX_MAX_KEYS];
	RegProcedure eqproc[INDEX_MAX_KEYS];
	StrategyNumber eqstrat[INDEX_MAX_KEYS];
	BlockNumber nblocks;		/* the table's, as the finder opened */
	TupleTableSlot *keyslot;	/* virtual: the key columns, else nulls */
	TupleTableSlot *rowslot;
	Datum	   *values;
	bool	   *isnull;
	Copy	   *c;
	int64		n;
	int64		max;
	MemoryContext ccxt;			/* the copies' keys and rows */
	bool		compact;		/* entries: kept once per row */
	int64		limit;			/* more rows than this: merged, keyed;
								 * folded in full, keyless */
	bool		merge;			/* merge the changes with the baseline */
	Size		bytes;			/* what the copies take */
	bool		spillable;		/* past maintenance_work_mem: sorted, not
								 * given up on */
	ProllyChangeSort *spill;	/* the copies, once past it */
	/* reading the sorted copies back: a row's copies, made one */
	bool		pending;
	StringInfoData pkey;
	StringInfoData prow;
	int64		pdelta;
	ItemPointerData ptid;
	bool		have_last;		/* the last row read back's key */
	StringInfoData last;
	int64		rows_back;		/* rows read back */
	bool		give_up;
	bool		unsettled;		/* undid rows its position can't find again */
	bool		emptied;		/* an entry's transaction emptied the table */
	ProllyFullFoldReason reason;	/* why, once it gives up */
	StringInfoData key;			/* reused for each encoding */
	StringInfoData row;
	ProllyChangeCounters work;
} Finder;

/* ----------------------------------------------------------------
 * State
 * ----------------------------------------------------------------
 */

static void
put_u32(StringInfo out, uint32 v)
{
	for (int i = 0; i < 4; i++)
		appendStringInfoChar(out, (char) (v >> (8 * i)));
}

static void
put_u64(StringInfo out, uint64 v)
{
	put_u32(out, (uint32) v);
	put_u32(out, (uint32) (v >> 32));
}

static uint32
get_u32(const uint8 *p)
{
	return (uint32) p[0] | ((uint32) p[1] << 8) | ((uint32) p[2] << 16) |
		((uint32) p[3] << 24);
}

static uint64
get_u64(const uint8 *p)
{
	return (uint64) get_u32(p) | ((uint64) get_u32(p + 4) << 32);
}

static int	xid_cmp(const void *a, const void *b);

static void
put_state(StringInfo out, const State *st)
{
	appendStringInfoChar(out, STATE_TAG);
	appendStringInfoChar(out, (char) STATE_VERSION);
	appendStringInfoChar(out, (char) st->flags);
	put_u64(out, st->log);
	put_u64(out, U64FromFullTransactionId(st->fxmax));
	put_u64(out, U64FromFullTransactionId(st->fown));
	put_u32(out, st->nxip);
	for (uint32 i = 0; i < st->nxip; i++)
		put_u64(out, U64FromFullTransactionId(st->fxip[i]));
}

/*
 * A state for the snapshot's fold by this transaction: its xmax, those it
 * saw running (sorted) and the transaction itself, whole, as of now.
 */
static void
state_of_snapshot(State *st, Snapshot snap)
{
	FullTransactionId next = ReadNextFullTransactionId();

	st->fxmax = FullTransactionIdFromAllowableAt(next, snap->xmax);
	st->xmax = snap->xmax;
	st->nxip = snap->xcnt;
	st->xip = palloc_array(TransactionId, Max(st->nxip, 1));
	st->fxip = palloc_array(FullTransactionId, Max(st->nxip, 1));
	if (st->nxip > 0)
		memcpy(st->xip, snap->xip, st->nxip * sizeof(TransactionId));
	qsort(st->xip, st->nxip, sizeof(TransactionId), xid_cmp);
	for (uint32 i = 0; i < st->nxip; i++)
		st->fxip[i] = FullTransactionIdFromAllowableAt(next, st->xip[i]);
	st->fown = GetTopFullTransactionIdIfAny();
	st->own = XidFromFullTransactionId(st->fown);
}

#define STATE_FIXED_LEN	(3 + 8 + 8 + 8 + 4)

static bool
parse_state(const ProllyValue *state, State *st)
{
	const uint8 *p = state->data;

	memset(st, 0, sizeof(*st));
	if (state->len < STATE_FIXED_LEN || p[0] != STATE_TAG ||
		p[1] != STATE_VERSION)
		return false;
	st->flags = p[2];
	st->log = get_u64(p + 3);
	st->fxmax = FullTransactionIdFromU64(get_u64(p + 11));
	st->fown = FullTransactionIdFromU64(get_u64(p + 19));
	st->nxip = get_u32(p + 27);
	st->xmax = XidFromFullTransactionId(st->fxmax);
	st->own = XidFromFullTransactionId(st->fown);
	if ((uint64) state->len != STATE_FIXED_LEN + (uint64) st->nxip * 8)
		return false;
	p += STATE_FIXED_LEN;
	st->xip = palloc_array(TransactionId, Max(st->nxip, 1));
	st->fxip = palloc_array(FullTransactionId, Max(st->nxip, 1));
	for (uint32 i = 0; i < st->nxip; i++, p += 8)
	{
		st->fxip[i] = FullTransactionIdFromU64(get_u64(p));
		st->xip[i] = XidFromFullTransactionId(st->fxip[i]);
	}
	return true;
}

static int
xid_cmp(const void *a, const void *b)
{
	return pg_cmp_u32(*(const TransactionId *) a, *(const TransactionId *) b);
}

/* (sorted as xid_cmp sorts their xids: the state's xids are searched so) */
static int
full_xid_cmp(const void *a, const void *b)
{
	return pg_cmp_u32(XidFromFullTransactionId(*(const FullTransactionId *) a),
					  XidFromFullTransactionId(*(const FullTransactionId *) b));
}

/*
 * Does the snapshot see every transaction the baseline's fold saw as
 * committed?  Each the fold saw finished (below its xmax, not running
 * then) must be below the snapshot's xmax and not running for it; and the
 * fold's own transaction, whose writes it saw, must be this one or one the
 * snapshot sees finished.  (Aborted, it took the baseline with it: so
 * finished is committed.)  A hot standby's snapshot is as real (the same
 * transactions as the primary's, replayed), but holds those running in its
 * subxip, subtransactions too: one of those the fold didn't list running
 * fails the test, conservatively; and one whose subtransactions overflowed
 * isn't known exactly, so fails it too.
 */
static bool
sees_fold(Snapshot snap, const State *st)
{
	TransactionId *running = snap->takenDuringRecovery ? snap->subxip : snap->xip;
	uint32		nrunning = snap->takenDuringRecovery ? snap->subxcnt : snap->xcnt;

	if (!TransactionIdIsNormal(st->xmax) ||
		TransactionIdPrecedes(snap->xmax, st->xmax) ||
		(snap->takenDuringRecovery && snap->suboverflowed))
		return false;
	if (TransactionIdIsNormal(st->own) &&
		!TransactionIdIsCurrentTransactionId(st->own) &&
		XidInMVCCSnapshot(st->own, snap))
		return false;
	for (uint32 i = 0; i < nrunning; i++)
		if (TransactionIdPrecedes(running[i], st->xmax) &&
			bsearch(&running[i], st->xip, st->nxip, sizeof(TransactionId),
					xid_cmp) == NULL)
			return false;
	return true;
}

/*
 * Does the snapshot see no transaction finished that the fold's position
 * doesn't hold (one at or past its xmax, or one it saw running)?  So an
 * unsettled baseline's rows are all this snapshot needs: a later
 * statement of the transaction whose old snapshot began it, say.
 */
static bool
sees_no_more(Snapshot snap, const State *st)
{
	if (!TransactionIdIsNormal(st->xmax) ||
		TransactionIdPrecedes(st->xmax, snap->xmax))
		return false;
	for (uint32 i = 0; i < st->nxip; i++)
		if (!XidInMVCCSnapshot(st->xip[i], snap))
			return false;
	return true;
}

/*
 * Was the table's catalog row changed by a transaction the snapshot doesn't
 * see?  A truncation or rewrite is: the snapshot reads the table's new
 * storage, but not the entry saying its rows were replaced.
 */
static bool
storage_unseen(Relation rel, Snapshot snap)
{
	HeapTuple	tup = SearchSysCache1(RELOID,
									  ObjectIdGetDatum(RelationGetRelid(rel)));
	TransactionId xmin;
	bool		unseen;

	if (!HeapTupleIsValid(tup))
		return true;
	xmin = HeapTupleHeaderGetXmin(tup->t_data);
	unseen = TransactionIdIsNormal(xmin) &&
		!TransactionIdIsCurrentTransactionId(xmin) &&
		XidInMVCCSnapshot(xmin, snap);
	ReleaseSysCache(tup);
	return unseen;
}

/* ----------------------------------------------------------------
 * Rows
 * ----------------------------------------------------------------
 */

static int
keying_compare(const ProllyKey *a, const ProllyKey *b, void *arg)
{
	Finder	   *f = arg;

	return PROLLY_CALL(f->kr, compare, f->k, a, b);
}

static void
finder_open(Finder *f, Relation rel, const ProllyTreeEnv *env)
{
	ProllyComponents c;

	memset(f, 0, sizeof(*f));
	f->rel = rel;
	f->snap = GetActiveSnapshot();
	prolly_components_for_relation(rel, &c);
	f->tree = c.tree;
	f->kr = c.keying;
	f->k = PROLLY_CALL(f->kr, open, rel, CurrentMemoryContext);
	f->keyless = PROLLY_CALL(f->kr, keyless, f->k);
	f->env = *env;
	f->env.compare = keying_compare;
	f->env.compare_arg = f;
	prolly_keying_use_env(&f->krefs, f->kr, f->k, f->tree, &f->env);
	f->desc = RelationGetDescr(rel);
	f->nblocks = RelationGetNumberOfBlocks(rel);
	f->ccxt = AllocSetContextCreate(CurrentMemoryContext, "log fold copies",
									ALLOCSET_DEFAULT_SIZES);
	/* (more rows changed than the table holds: reading it all is less) */
	f->limit = rel->rd_rel->reltuples >= 0 ?
		Max((int64) rel->rd_rel->reltuples, COMPACT_AT) :
		Max((int64) RelationGetNumberOfBlocks(rel) * MaxHeapTuplesPerPage,
			COMPACT_AT);
	f->values = palloc_array(Datum, f->desc->natts);
	f->isnull = palloc_array(bool, f->desc->natts);
	f->keyslot = MakeSingleTupleTableSlot(f->desc, &TTSOpsVirtual);
	f->rowslot = table_slot_create(rel, NULL);
	initStringInfo(&f->key);
	initStringInfo(&f->row);
	initStringInfo(&f->pkey);
	initStringInfo(&f->prow);
	initStringInfo(&f->last);

	if (!f->keyless)
	{
		f->pk = index_open(RelationGetPrimaryKeyIndex(rel, false), AccessShareLock);
		f->nkeys = IndexRelationGetNumberOfKeyAttributes(f->pk);
		for (int i = 0; i < f->nkeys; i++)
		{
			Oid			opfamily = f->pk->rd_opfamily[i];
			Oid			optype = f->pk->rd_opcintype[i];
			Oid			op;

			f->keyattrs[i] = f->pk->rd_index->indkey.values[i];
			f->eqstrat[i] = IndexAmTranslateCompareType(COMPARE_EQ,
														f->pk->rd_rel->relam,
														opfamily, true);
			op = f->eqstrat[i] == InvalidStrategy ? InvalidOid :
				get_opfamily_member(opfamily, optype, optype, f->eqstrat[i]);
			/* a key we can't look up by: fold in full */
			if (!AttributeNumberIsValid(f->keyattrs[i]) || !OidIsValid(op))
			{
				f->give_up = true;
				f->reason = PROLLY_FULL_UNSUPPORTED_KEY;
			}
			else
				f->eqproc[i] = get_opcode(op);
		}
	}
}

/* The sort of the copies, if any, ended: its files removed. */
static void
spill_end(Finder *f)
{
	if (f->spill)
		prolly_change_sort_end(f->spill, NULL);
	f->spill = NULL;
}

static void
finder_close(Finder *f)
{
	spill_end(f);
	if (f->pk)
		index_close(f->pk, AccessShareLock);
	ExecDropSingleTupleTableSlot(f->keyslot);
	ExecDropSingleTupleTableSlot(f->rowslot);
	PROLLY_CALL(f->kr, close, f->k);
}

static int	copy_cmp(const void *a, const void *b, void *arg);
static bool same_bytes(const char *a, uint32 alen, const uint8 *b, uint32 blen);

/*
 * The entries kept once per row: sorted, a row's copies made one (a key's
 * first, or a keyless row's with its count summed; one summed to none is
 * no change).
 */
static void
compact_copies(Finder *f)
{
	int64		u = 0;

	if (f->n > 1)
		qsort_arg(f->c, f->n, sizeof(Copy), copy_cmp, f);
	for (int64 i = 0; i < f->n; i++)
	{
		Copy	   *c = &f->c[i];

		if (u > 0 && same_bytes(f->c[u - 1].key, f->c[u - 1].keylen,
								(const uint8 *) c->key, c->keylen))
		{
			f->c[u - 1].delta += c->delta;
			if (!ItemPointerIsValid(&f->c[u - 1].tid))
				f->c[u - 1].tid = c->tid;
			pfree(c->key);
			pfree(c->row);
			continue;
		}
		if (u > 0 && f->keyless && f->c[u - 1].delta == 0)
		{
			pfree(f->c[u - 1].key);
			pfree(f->c[u - 1].row);
			u--;
		}
		f->c[u++] = *c;
	}
	if (u > 0 && f->keyless && f->c[u - 1].delta == 0)
	{
		pfree(f->c[u - 1].key);
		pfree(f->c[u - 1].row);
		u--;
	}
	f->n = u;
}

/*
 * The copies held so far moved into a sort, which takes the rest: past
 * maintenance_work_mem, the changes are sorted there, spilling, and read
 * back a part at a time (next_part()).
 */
static void
start_spill(Finder *f)
{
	if (f->compact)
		compact_copies(f);
	f->spill = prolly_change_sort_begin(f->kr, f->k);
	for (int64 i = 0; i < f->n; i++)
	{
		ProllyKey	k = {(const uint8 *) f->c[i].key, f->c[i].keylen};

		prolly_change_sort_put(f->spill, &k, f->c[i].row, f->c[i].rowlen,
							   f->c[i].delta, &f->c[i].tid);
	}
	MemoryContextReset(f->ccxt);
	f->c = NULL;
	f->n = f->max = 0;
	f->bytes = 0;
	pgstat_count_prolly(PGSTAT_PROLLY_FOLD_CHANGES_SPILLED, 1);
}

static void
add_copy(Finder *f, const StringInfoData *key, const char *row, uint32 rowlen,
		 int64 delta, const ItemPointerData *tid)
{
	Copy	   *c;

	if (f->spill)
	{
		ProllyKey	k = {(const uint8 *) key->data, key->len};

		prolly_change_sort_put(f->spill, &k, row, rowlen, delta, tid);
		return;
	}
	if (f->n == f->max)
	{
		if (f->compact && f->n >= COMPACT_AT)
		{
			compact_copies(f);
			if (f->n > f->limit)
			{
				/* (keyed: merged with the baseline, a pass over each) */
				if (!f->keyless)
					f->merge = true;
				else
				{
					f->give_up = true;
					f->reason = PROLLY_FULL_TOO_MANY;
					return;
				}
			}
		}
		/* (grown only if compacting left it more than half full) */
		if (f->n * 2 > f->max || f->max == 0)
		{
			f->max = Max(64, f->max * 2);
			f->c = f->c ? repalloc_huge(f->c, f->max * sizeof(Copy)) :
				MemoryContextAllocHuge(f->ccxt, f->max * sizeof(Copy));
		}
	}
	/*
	 * (more than maintenance_work_mem: sorted, spilling, where the fold can
	 * read them back in parts; else, where the reader needs them all at
	 * once, given up on)
	 */
	f->bytes += sizeof(Copy) + key->len + rowlen;
	if (f->bytes > (Size) maintenance_work_mem * 1024)
	{
		if (!f->spillable)
		{
			f->give_up = true;
			f->reason = PROLLY_FULL_MEMORY_BUDGET;
			return;
		}
		start_spill(f);
		add_copy(f, key, row, rowlen, delta, tid);
		return;
	}
	c = &f->c[f->n++];
	c->keylen = key->len;
	c->key = MemoryContextAlloc(f->ccxt, Max(key->len, 1));
	memcpy(c->key, key->data, key->len);
	c->rowlen = rowlen;
	c->row = MemoryContextAlloc(f->ccxt, Max(rowlen, 1));
	memcpy(c->row, row, rowlen);
	c->delta = delta;
	if (tid != NULL)
		c->tid = *tid;
	else
		ItemPointerSetInvalid(&c->tid);
}

/* A whole row's copy: the tuple's key and encoded row, counted delta. */
static void
row_copy(Finder *f, HeapTuple tup, int64 delta)
{
	ExecForceStoreHeapTuple(tup, f->rowslot, false);
	resetStringInfo(&f->key);
	resetStringInfo(&f->row);
	PROLLY_CALL(f->kr, encode_row, f->k, f->rowslot, &f->key, &f->row, NULL);
	ExecClearTuple(f->rowslot);
	add_copy(f, &f->key, f->row.data, f->row.len, delta, NULL);
}

/*
 * An entry of the log: with a key, its key a candidate (its tuple kept, to
 * look the key up by); without, its row's copy counted.
 */
static bool
entry_visit(HeapTuple entry, int32 delta, void *arg)
{
	Finder	   *f = arg;

	/*
	 * (the table emptied: with a key, the fold starts from no rows, each key
	 * read a candidate, before or after, looked up in the heap; without,
	 * the counts can't be told apart, and it reads the table in full, as
	 * many rows as were written since)
	 */
	if (entry == NULL)
	{
		if (f->keyless)
		{
			f->give_up = true;
			f->reason = PROLLY_FULL_LOG_ENTRY;
			return false;
		}
		f->emptied = true;
		return true;
	}
	if (f->keyless)
	{
		/* (a key's entry: the table was keyed when it was written) */
		if (delta == 0)
		{
			f->give_up = true;
			f->reason = PROLLY_FULL_KEYING_CHANGED;
			return false;
		}
		row_copy(f, entry, delta);
	}
	else
	{
		heap_deform_tuple(entry, f->desc, f->values, f->isnull);
		ExecClearTuple(f->keyslot);
		for (int i = 0; i < f->desc->natts; i++)
			f->keyslot->tts_isnull[i] = true;
		for (int i = 0; i < f->nkeys; i++)
		{
			int			a = f->keyattrs[i] - 1;

			/* (a key column null: the entry is of another key's columns) */
			if (f->isnull[a])
			{
				f->give_up = true;
				f->reason = PROLLY_FULL_KEYING_CHANGED;
				return false;
			}
			f->keyslot->tts_values[a] = f->values[a];
			f->keyslot->tts_isnull[a] = f->isnull[a];
		}
		ExecStoreVirtualTuple(f->keyslot);
		resetStringInfo(&f->key);
		if (!PROLLY_CALL(f->kr, encode_key, f->k, f->keyslot, &f->key))
		{
			/*
			 * (the keying keys rows by content, though the table's read as
			 * keyed: keyless() said otherwise; never, as keyings are)
			 */
			f->give_up = true;
			f->reason = PROLLY_FULL_KEYING_CHANGED;
			return false;
		}
		add_copy(f, &f->key, (const char *) entry->t_data, entry->t_len, 0,
				 &entry->t_self);
	}
	CHECK_FOR_INTERRUPTS();
	return !f->give_up;
}

static bool
same_bytes(const char *a, uint32 alen, const uint8 *b, uint32 blen)
{
	return alen == blen && memcmp(a, b, alen) == 0;
}

/* By the keying's order, then by bytes (as a full fold orders rows). */
static int
copy_cmp(const void *a, const void *b, void *arg)
{
	const Copy *x = a;
	const Copy *y = b;
	ProllyKey	kx = {(const uint8 *) x->key, x->keylen};
	ProllyKey	ky = {(const uint8 *) y->key, y->keylen};
	int			r = keying_compare(&kx, &ky, arg);

	if (r == 0)
		r = memcmp(x->key, y->key, Min(x->keylen, y->keylen));
	return r != 0 ? r : (x->keylen > y->keylen) - (x->keylen < y->keylen);
}

/*
 * The row the snapshot sees by a candidate's key (its entry's tuple 'c'),
 * if any, encoded into f->key and f->row.
 */
static bool
lookup(Finder *f, IndexScanDesc scan, const Copy *c)
{
	HeapTupleData entry;
	ScanKeyData skey[INDEX_MAX_KEYS];

	/*
	 * (where the log says the key's version is: the row, if the snapshot
	 * sees the version there and it has the key, as a table with a key has
	 * one row by a key a snapshot sees; else, updated since, deleted, or
	 * its place reused, the key looked up)
	 */
	if (ItemPointerIsValid(&c->tid) &&
		ItemPointerGetBlockNumber(&c->tid) < f->nblocks &&
		table_tuple_fetch_row_version(f->rel, (ItemPointer) &c->tid, f->snap,
									  f->rowslot))
	{
		ProllyKey	ck = {(const uint8 *) c->key, c->keylen};
		ProllyKey	nk;

		resetStringInfo(&f->key);
		resetStringInfo(&f->row);
		PROLLY_CALL(f->kr, encode_row, f->k, f->rowslot, &f->key, &f->row, NULL);
		ExecClearTuple(f->rowslot);
		nk.data = (const uint8 *) f->key.data;
		nk.len = f->key.len;
		if (keying_compare(&nk, &ck, f) == 0)
		{
			pgstat_count_prolly(PGSTAT_PROLLY_FOLD_ROWS_LOCATED, 1);
			return true;
		}
	}

	entry.t_data = (HeapTupleHeader) c->row;
	entry.t_len = c->rowlen;
	heap_deform_tuple(&entry, f->desc, f->values, f->isnull);
	for (int i = 0; i < f->nkeys; i++)
	{
		int			a = f->keyattrs[i] - 1;

		ScanKeyInit(&skey[i], i + 1, f->eqstrat[i], f->eqproc[i], f->values[a]);
		skey[i].sk_collation = f->pk->rd_indcollation[i];
		if (f->isnull[a])
			skey[i].sk_flags |= (SK_ISNULL | SK_SEARCHNULL);
	}
	index_rescan(scan, skey, f->nkeys, NULL, 0);
	if (!table_index_getnext_slot(scan, ForwardScanDirection, f->rowslot))
		return false;
	resetStringInfo(&f->key);
	resetStringInfo(&f->row);
	PROLLY_CALL(f->kr, encode_row, f->k, f->rowslot, &f->key, &f->row, NULL);
	ExecClearTuple(f->rowslot);
	return true;
}

/*
 * The baseline's rows and the edits (in key order), merged into a new tree
 * in one pass over each: a row the edits name, as they have it (or not, a
 * delete), the rest as the baseline has them.  Many edits cost a pass over
 * the baseline, not a lookup into it each.  The edits can come in parts,
 * each after the last in key order (merge_add()).
 */
typedef struct Merge
{
	ProllyTreeBuilder *b;
	ProllyTreeCursor *cur;
	ProllyKey	k;
	ProllyValue v;
	bool		more;
} Merge;

static void
merge_begin(Finder *f, Merge *m, const ProllyNodeRef *from)
{
	m->b = PROLLY_CALL(f->tree, build_begin, &f->env, CurrentMemoryContext);
	m->cur = PROLLY_CALL(f->tree, cursor_open, &f->env, from,
						 CurrentMemoryContext);
	PROLLY_CALL(f->tree, cursor_seek, m->cur, NULL, ForwardScanDirection);
	m->more = PROLLY_CALL(f->tree, cursor_next, m->cur, &m->k, &m->v);
}

static void
merge_add(Finder *f, Merge *m, const ProllyEdit *edits, int nedits)
{
	int			e = 0;

	while (e < nedits)
	{
		int			c = !m->more ? 1 : keying_compare(&m->k, &edits[e].key, f);

		if (c < 0)
			PROLLY_CALL(f->tree, build_add, m->b, &m->k, &m->v);
		else
		{
			if (!edits[e].is_delete)
				PROLLY_CALL(f->tree, build_add, m->b, &edits[e].key,
							&edits[e].value);
			e++;
		}
		if (c <= 0)
			m->more = PROLLY_CALL(f->tree, cursor_next, m->cur, &m->k, &m->v);
		CHECK_FOR_INTERRUPTS();
	}
}

static void
merge_finish(Finder *f, Merge *m, ProllyNodeRef *data)
{
	while (m->more)
	{
		PROLLY_CALL(f->tree, build_add, m->b, &m->k, &m->v);
		m->more = PROLLY_CALL(f->tree, cursor_next, m->cur, &m->k, &m->v);
		CHECK_FOR_INTERRUPTS();
	}
	PROLLY_CALL(f->tree, cursor_close, m->cur);
	PROLLY_CALL(f->tree, build_finish, m->b, data);
	f->work.merges++;
}

static void
merge_build(Finder *f, const ProllyTableFold *baseline, const ProllyEdit *edits,
			int nedits, ProllyNodeRef *data)
{
	Merge		m;

	merge_begin(f, &m, &baseline->data);
	merge_add(f, &m, edits, nedits);
	merge_finish(f, &m, data);
}

/* The candidates kept once per key, as the keying tells keys apart. */
static void
unique_keys(Finder *f)
{
	int64		u = 0;

	if (f->n > 1)
		qsort_arg(f->c, f->n, sizeof(Copy), copy_cmp, f);
	for (int64 i = 0; i < f->n; i++)
	{
		ProllyKey	prev = {u > 0 ? (const uint8 *) f->c[u - 1].key : NULL,
		u > 0 ? f->c[u - 1].keylen : 0};
		ProllyKey	next = {(const uint8 *) f->c[i].key, f->c[i].keylen};

		if (u == 0 || keying_compare(&prev, &next, f) != 0)
			f->c[u++] = f->c[i];
	}
	f->n = u;
	f->work.candidates += f->n;
	f->work.rows_read += f->n;
}

/*
 * A candidate that differs: the row data has by its key (kind REMOVED or
 * MODIFIED: before), and the row the snapshot sees (ADDED or MODIFIED:
 * after, with its own key's bytes; REMOVED: the candidate's key).  What
 * they point to is valid during the call only, but a REMOVED one's key,
 * which is the candidate's.
 */
typedef bool (*ChangeEmit) (Finder *f, ProllyDiffKind kind,
							const ProllyKey *key, const ProllyValue *before,
							const ProllyValue *after, void *arg);

/*
 * Each candidate (unique_keys()'s, in key order) looked up in data and in
 * the heap, by the finder's snapshot; those that differ emitted, until an
 * emit returns false.
 */
static void
each_change(Finder *f, const ProllyNodeRef *data, ChangeEmit emit, void *arg)
{
	ProllyTreeCursor *cur;
	IndexScanDesc scan;
	StringInfoData value;
	bool		go = true;

	cur = PROLLY_CALL(f->tree, cursor_open, &f->env, data,
					  CurrentMemoryContext);
	scan = index_beginscan(f->rel, f->pk, false, f->snap, NULL, f->nkeys, 0,
						   SO_NONE);
	initStringInfo(&value);
	for (int64 i = 0; go && i < f->n; i++)
	{
		Copy	   *c = &f->c[i];
		ProllyKey	ck = {(const uint8 *) c->key, c->keylen};
		ProllyKey	k;
		ProllyValue v;
		bool		had;
		bool		has;

		PROLLY_CALL(f->tree, cursor_seek, cur, &ck, ForwardScanDirection);
		had = PROLLY_CALL(f->tree, cursor_next, cur, &k, &v) &&
			keying_compare(&k, &ck, f) == 0;
		has = lookup(f, scan, c);
		if (has)
		{
			ProllyValue r = {(const uint8 *) f->row.data, f->row.len};

			resetStringInfo(&value);
			PROLLY_CALL(f->kr, make_value, f->k, &r, 1, &value);
		}
		if (had && has && v.len == value.len &&
			memcmp(v.data, value.data, v.len) == 0 && k.len == f->key.len &&
			memcmp(k.data, f->key.data, k.len) == 0)
			;					/* the same row */
		else if (has)
		{
			ProllyKey	nk = {(const uint8 *) f->key.data, f->key.len};
			ProllyValue nv = {(const uint8 *) value.data, value.len};

			go = emit(f, had ? PROLLY_DIFF_MODIFIED : PROLLY_DIFF_ADDED, &nk,
					  had ? &v : NULL, &nv, arg);
		}
		else if (had)
			go = emit(f, PROLLY_DIFF_REMOVED, &ck, &v, NULL, arg);
		CHECK_FOR_INTERRUPTS();
	}
	index_endscan(scan);
	PROLLY_CALL(f->tree, cursor_close, cur);
}

typedef struct Edits
{
	ProllyEdit *edits;
	int			nedits;
} Edits;

static bool
edit_emit(Finder *f, ProllyDiffKind kind, const ProllyKey *key,
		  const ProllyValue *before, const ProllyValue *after, void *arg)
{
	Edits	   *ed = arg;
	ProllyEdit *e = &ed->edits[ed->nedits++];

	if (kind == PROLLY_DIFF_REMOVED)
	{
		e->key = *key;
		e->is_delete = true;
	}
	else
	{
		char	   *kb = palloc(Max(key->len, 1));
		char	   *vb = palloc(Max(after->len, 1));

		memcpy(kb, key->data, key->len);
		memcpy(vb, after->data, after->len);
		e->key.data = (const uint8 *) kb;
		e->key.len = key->len;
		e->value.data = (const uint8 *) vb;
		e->value.len = after->len;
		e->is_delete = false;
	}
	return true;
}

/* The row read back so far (f->pkey, its copies made one) put in the part. */
static void
part_put(Finder *f)
{
	ProllyKey	k = {(const uint8 *) f->pkey.data, f->pkey.len};
	Copy	   *c;

	f->pending = false;
	if (f->keyless && f->pdelta == 0)
		return;
	if (f->have_last)
	{
		ProllyKey	lk = {(const uint8 *) f->last.data, f->last.len};

		/*
		 * (keys the keying calls equal: with a key, the first kept, as
		 * unique_keys() keeps it; without, rows written under another
		 * keying, as apply_copies() finds them)
		 */
		if (keying_compare(&lk, &k, f) == 0)
		{
			if (f->keyless)
			{
				f->give_up = true;
				f->reason = PROLLY_FULL_KEYING_CHANGED;
			}
			return;
		}
	}
	/* (more rows changed than the table holds: reading it all is less) */
	if (f->keyless && ++f->rows_back > f->limit)
	{
		f->give_up = true;
		f->reason = PROLLY_FULL_TOO_MANY;
		return;
	}
	if (f->n == f->max)
	{
		f->max = Max(64, f->max * 2);
		f->c = f->c ? repalloc_huge(f->c, f->max * sizeof(Copy)) :
			MemoryContextAllocHuge(f->ccxt, f->max * sizeof(Copy));
	}
	c = &f->c[f->n++];
	c->keylen = f->pkey.len;
	c->key = MemoryContextAlloc(f->ccxt, Max(f->pkey.len, 1));
	memcpy(c->key, f->pkey.data, f->pkey.len);
	c->rowlen = f->prow.len;
	c->row = MemoryContextAlloc(f->ccxt, Max(f->prow.len, 1));
	memcpy(c->row, f->prow.data, f->prow.len);
	c->delta = f->pdelta;
	c->tid = f->ptid;
	f->bytes += sizeof(Copy) + c->keylen + c->rowlen;
	resetStringInfo(&f->last);
	appendBinaryStringInfo(&f->last, f->pkey.data, f->pkey.len);
	f->have_last = true;
}

/*
 * The next part of the sorted copies into f->c, in key order, kept once
 * per row as compact_copies() and unique_keys() keep them, a row's copies
 * never split between parts: up to half maintenance_work_mem, as the sort
 * holds about as much.  False once none are left, or on giving up.
 */
static bool
next_part(Finder *f)
{
	Size		budget = (Size) maintenance_work_mem * 1024 / 2;
	ProllySortedChange ch;

	MemoryContextReset(f->ccxt);
	f->c = NULL;
	f->n = f->max = 0;
	f->bytes = 0;
	while (!f->give_up && f->bytes < budget &&
		   prolly_change_sort_next(f->spill, &ch))
	{
		if (f->pending && same_bytes(f->pkey.data, f->pkey.len, ch.key.data,
									 ch.key.len))
		{
			f->pdelta += ch.delta;
			if (!ItemPointerIsValid(&f->ptid))
				f->ptid = ch.tid;
		}
		else
		{
			if (f->pending)
				part_put(f);
			resetStringInfo(&f->pkey);
			appendBinaryStringInfo(&f->pkey, (const char *) ch.key.data,
								   ch.key.len);
			resetStringInfo(&f->prow);
			appendBinaryStringInfo(&f->prow, ch.row, ch.rowlen);
			f->pdelta = ch.delta;
			f->ptid = ch.tid;
			f->pending = true;
		}
		CHECK_FOR_INTERRUPTS();
	}
	/* (the sort's end: the last row's copies are all read) */
	if (f->pending && !f->give_up && f->bytes < budget)
		part_put(f);
	return !f->give_up && (f->n > 0 || f->pending);
}

/*
 * fold_keys() for changes past maintenance_work_mem, read back from their
 * sort a part at a time: each part's edits applied to the tree so far, or,
 * once the edits pass a tenth of the baseline's rows, merged with it in
 * one pass, as fold_keys() chooses.  The parts follow each other in key
 * order, so each part's rows are the baseline's in the tree so far.
 */
static void
fold_keys_spilled(Finder *f, const ProllyTableFold *baseline,
				  ProllyNodeRef *data)
{
	uint64		rows = PROLLY_CALL(f->tree, count, &f->env, &baseline->data);
	ProllyNodeRef cur = baseline->data;
	MemoryContext pcxt = AllocSetContextCreate(CurrentMemoryContext,
											   "log fold part",
											   ALLOCSET_DEFAULT_SIZES);
	Merge		m = {0};
	bool		merging = false;
	uint64		total = 0;

	prolly_change_sort_done(f->spill);
	while (next_part(f))
	{
		MemoryContext old = MemoryContextSwitchTo(pcxt);
		Edits		ed = {NULL, 0};

		f->work.candidates += f->n;
		f->work.rows_read += f->n;
		ed.edits = MemoryContextAllocHuge(pcxt, Max(f->n, 1) * sizeof(ProllyEdit));
		each_change(f, &baseline->data, edit_emit, &ed);
		MemoryContextSwitchTo(old);
		f->work.rows_written += ed.nedits;
		total += ed.nedits;
		if (!merging && ed.nedits > 0 &&
			(f->merge || total * MERGE_FRACTION > rows))
		{
			merge_begin(f, &m, &cur);
			merging = true;
		}
		if (merging)
			merge_add(f, &m, ed.edits, ed.nedits);
		else if (ed.nedits > 0)
		{
			ProllyNodeRef next;

			PROLLY_CALL(f->tree, apply, &f->env, &cur, ed.edits, ed.nedits,
						&next);
			cur = next;
		}
		MemoryContextReset(pcxt);
	}
	if (merging)
		merge_finish(f, &m, &cur);
	MemoryContextDelete(pcxt);
	*data = cur;
}

/* Each candidate's old and new rows; the pairs that differ are edits. */
static void
fold_keys(Finder *f, const ProllyTableFold *baseline, ProllyNodeRef *data)
{
	Edits		ed = {NULL, 0};
	int			nedits;
	ProllyEdit *edits;

	if (f->spill)
	{
		fold_keys_spilled(f, baseline, data);
		return;
	}
	unique_keys(f);
	ed.edits = MemoryContextAllocHuge(CurrentMemoryContext,
									  Max(f->n, 1) * sizeof(ProllyEdit));
	each_change(f, &baseline->data, edit_emit, &ed);
	edits = ed.edits;
	nedits = ed.nedits;
	f->work.rows_written += nedits;
	/* (many edits: merged, a pass over the baseline; else each applied) */
	if (nedits > 0 &&
		(f->merge ||
		 (uint64) nedits * MERGE_FRACTION >
		 PROLLY_CALL(f->tree, count, &f->env, &baseline->data)))
		merge_build(f, baseline, edits, nedits, data);
	else if (nedits > 0)
		PROLLY_CALL(f->tree, apply, &f->env, &baseline->data, edits, nedits, data);
	else
		*data = baseline->data;
}

/*
 * Each row's copies, counted against data_in's count of it, applied into
 * *data; with 'made' (the transaction's own changes), a keyed row is put
 * as its copy made, or deleted if none was.  False if they can't be: keys
 * the keying can't tell apart but the bytes can, or a count below none.
 */
static bool
apply_copies(Finder *f, const ProllyNodeRef *data_in, ProllyNodeRef *data)
{
	ProllyEdit *edits;
	int			nedits = 0;
	ProllyTreeCursor *cur;
	bool		ok = true;

	if (f->n > 1)
		qsort_arg(f->c, f->n, sizeof(Copy), copy_cmp, f);
	edits = MemoryContextAllocHuge(CurrentMemoryContext,
								   Max(f->n, 1) * sizeof(ProllyEdit));
	cur = PROLLY_CALL(f->tree, cursor_open, &f->env, data_in,
					  CurrentMemoryContext);
	for (int64 i = 0; ok && i < f->n;)
	{
		Copy	   *c = &f->c[i];
		Copy	   *made = NULL;
		ProllyKey	ck = {(const uint8 *) c->key, c->keylen};
		ProllyKey	hk;
		ProllyValue hv;
		int64		delta = 0;
		bool		at_head;
		ProllyEdit *e;

		while (i < f->n && same_bytes(f->c[i].key, f->c[i].keylen, ck.data, ck.len))
		{
			if (f->c[i].delta > 0)
				made = &f->c[i];
			delta += f->c[i++].delta;
		}
		if (i < f->n)
		{
			ProllyKey	nk = {(const uint8 *) f->c[i].key, f->c[i].keylen};

			if (keying_compare(&ck, &nk, f) == 0)
			{
				ok = false;
				break;
			}
		}
		if (f->keyless && delta == 0)
			continue;
		f->work.candidates++;
		f->work.rows_read++;
		PROLLY_CALL(f->tree, cursor_seek, cur, &ck, ForwardScanDirection);
		at_head = PROLLY_CALL(f->tree, cursor_next, cur, &hk, &hv) &&
			keying_compare(&hk, &ck, f) == 0;
		if (at_head && !same_bytes(c->key, c->keylen, hk.data, hk.len))
		{
			ok = false;
			break;
		}
		e = &edits[nedits];
		e->key = ck;
		if (!f->keyless)
		{
			e->is_delete = made == NULL;
			if (made)
			{
				ProllyValue r = {(const uint8 *) made->row, made->rowlen};
				StringInfoData value;

				initStringInfo(&value);
				PROLLY_CALL(f->kr, make_value, f->k, &r, 1, &value);
				e->value.data = (const uint8 *) value.data;
				e->value.len = value.len;
			}
			nedits++;
		}
		else
		{
			int64		count = 0;

			if (at_head)
			{
				ProllyValue row;
				uint64		had;

				PROLLY_CALL(f->kr, parse_value, f->k, &hv, &row, &had);
				count = (int64) had;
			}
			count += delta;
			if (count < 0)
			{
				ok = false;
				break;
			}
			e->is_delete = count == 0;
			if (count > 0)
			{
				ProllyValue r = {(const uint8 *) c->row, c->rowlen};
				StringInfoData value;

				initStringInfo(&value);
				PROLLY_CALL(f->kr, make_value, f->k, &r, (uint64) count, &value);
				e->value.data = (const uint8 *) value.data;
				e->value.len = value.len;
			}
			nedits++;
		}
		CHECK_FOR_INTERRUPTS();
	}
	PROLLY_CALL(f->tree, cursor_close, cur);
	f->work.rows_written += nedits;
	if (ok && nedits > 0)
		PROLLY_CALL(f->tree, apply, &f->env, data_in, edits, nedits, data);
	else if (ok)
		*data = *data_in;
	return ok;
}

/*
 * apply_copies() for copies past maintenance_work_mem, read back from
 * their sort a part at a time, each part applied to the tree so far: a
 * row's copies are never split between parts, so each count is the
 * baseline's with every copy counted.
 */
static bool
apply_spilled(Finder *f, const ProllyNodeRef *data_in, ProllyNodeRef *data)
{
	ProllyNodeRef cur = *data_in;
	MemoryContext pcxt = AllocSetContextCreate(CurrentMemoryContext,
											   "log fold part",
											   ALLOCSET_DEFAULT_SIZES);
	bool		ok = true;

	prolly_change_sort_done(f->spill);
	while (ok && next_part(f))
	{
		MemoryContext old = MemoryContextSwitchTo(pcxt);
		ProllyNodeRef next;

		ok = apply_copies(f, &cur, &next);
		MemoryContextSwitchTo(old);
		cur = next;
		MemoryContextReset(pcxt);
	}
	MemoryContextDelete(pcxt);
	if (!ok || f->give_up)
		return false;
	*data = cur;
	return true;
}


/*
 * Every row the snapshot sees, folded into a data tree: each row's key and
 * canonical encoding, in key order, identical rows of a keyless table
 * counted once.
 */
static void
fold_all(Finder *f, ProllyNodeRef *data)
{
	ProllyFullFold done;

	f->work.full++;
	prolly_fold_all(f->rel, f->snap, f->rowslot, f->kr, f->k, f->keyless,
					f->tree, &f->env, data, &done);
	f->work.rows_read += done.rows_read;
	f->work.rows_written += done.rows_written;
}

/*
 * Fold from the baseline, its state 'st': false if the table must be read
 * in full instead.
 */
/* An entry the baseline holds and the snapshot doesn't see: undone. */
static bool
entry_unseen_visit(HeapTuple entry, int32 delta, void *arg)
{
	Finder	   *f = arg;

	/* (an emptying it doesn't see: the rows before aren't in the log) */
	if (entry == NULL)
	{
		f->give_up = true;
		f->reason = PROLLY_FULL_LOG_ENTRY;
		return false;
	}
	return entry_visit(entry, -delta, arg);
}

static bool
fold_changed(Finder *f, const ProllyTableFold *baseline, const State *st,
			 bool stale, const ProllyChangeLogRoutine *log,
			 ProllyNodeRef *data)
{
	ProllyLogPosition pos = {st->fxmax, st->fown, st->nxip, st->fxip};
	bool		consumed = false;
	ProllyTableFold from;

	f->compact = true;
	f->spillable = true;
	if (f->give_up)
		return false;

	/*
	 * (a snapshot that doesn't see the baseline's fold: the changes the
	 * fold held of transactions the snapshot doesn't see finished are
	 * undone, each a candidate re-read by the snapshot, a keyless row's
	 * count taken back; if one was consumed, the baseline this fold
	 * begins is unsettled: see the file's header)
	 */
	if (stale &&
		!PROLLY_CALL(log, read_unseen, f->rel, f->snap, &pos,
					 entry_unseen_visit, f, &consumed))
	{
		if (!f->give_up)
			f->reason = PROLLY_FULL_LOG_ENTRY;
		return false;
	}
	if (consumed)
		f->unsettled = true;
	if (f->give_up)
		return false;
	if (!PROLLY_CALL(log, read, f->rel, f->snap, &pos, entry_visit, f))
	{
		/* (an entry said to, unless the visit gave up) */
		if (!f->give_up)
			f->reason = PROLLY_FULL_LOG_ENTRY;
		return false;
	}
	if (f->give_up)
		return false;
	compact_copies(f);
	if (!f->keyless && f->emptied)
	{
		ProllyTableFold empty = *baseline;
		ProllyTreeBuilder *b = PROLLY_CALL(f->tree, build_begin, &f->env,
										   CurrentMemoryContext);

		PROLLY_CALL(f->tree, build_finish, b, &empty.data);
		fold_keys(f, &empty, data);
		return true;
	}

	/*
	 * (a baseline of another shape: its rows mapped to this one's now, only
	 * here, once the fold will be from it, as a map reads every row of it)
	 */
	from = *baseline;
	if (!prolly_table_fold_data(f->rel, &f->env, baseline, &from.data))
	{
		f->reason = PROLLY_FULL_SHAPE_CHANGED;
		return false;
	}
	if (!f->keyless)
	{
		fold_keys(f, &from, data);
		return true;
	}
	if (f->spill ? !apply_spilled(f, &from.data, data) :
		!apply_copies(f, &from.data, data))
	{
		/* (a row's count below none: written under another keying) */
		if (!f->give_up)
			f->reason = PROLLY_FULL_KEYING_CHANGED;
		return false;
	}
	return true;
}

/* ----------------------------------------------------------------
 * A transaction's own changes
 * ----------------------------------------------------------------
 */

static int
tid_cmp(const void *a, const void *b)
{
	return ItemPointerCompare((ItemPointer) a, (ItemPointer) b);
}

/*
 * The tuple versions tids names, read where they are (the transaction's
 * own versions aren't pruned while it runs): each it made and didn't
 * replace, a copy +1; each it replaced and hadn't made, -1.
 */
static void
own_copies(Finder *f, ItemPointerData *tids, int ntids)
{
	if (ntids > 1)
		qsort(tids, ntids, sizeof(ItemPointerData), tid_cmp);
	for (int i = 0; i < ntids; i++)
	{
		Buffer		buf;
		Page		page;
		OffsetNumber off;
		ItemId		lp;
		HeapTupleData tup;
		HeapTupleHeader t;
		bool		made;
		bool		replaced = false;
		HeapTuple	copy;

		if (i > 0 && ItemPointerEquals(&tids[i], &tids[i - 1]))
			continue;
		buf = ReadBufferExtended(f->rel, MAIN_FORKNUM,
								 ItemPointerGetBlockNumber(&tids[i]),
								 RBM_NORMAL, NULL);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		off = ItemPointerGetOffsetNumber(&tids[i]);
		lp = off <= PageGetMaxOffsetNumber(page) ? PageGetItemId(page, off) : NULL;
		if (lp == NULL || !ItemIdIsNormal(lp))
		{
			UnlockReleaseBuffer(buf);
			continue;
		}
		tup.t_data = (HeapTupleHeader) PageGetItem(page, lp);
		tup.t_len = ItemIdGetLength(lp);
		tup.t_tableOid = RelationGetRelid(f->rel);
		tup.t_self = tids[i];
		t = tup.t_data;
		made = !HeapTupleHeaderXminFrozen(t) &&
			!(t->t_infomask & HEAP_XMIN_INVALID) &&
			TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetRawXmin(t));
		if (!(t->t_infomask & HEAP_XMAX_INVALID) &&
			!HEAP_XMAX_IS_LOCKED_ONLY(t->t_infomask))
		{
			TransactionId updater = HeapTupleHeaderGetUpdateXid(t);

			replaced = TransactionIdIsValid(updater) &&
				TransactionIdIsCurrentTransactionId(updater);
		}
		copy = (made != replaced) ? heap_copytuple(&tup) : NULL;
		UnlockReleaseBuffer(buf);

		/* (the table's own versions: their values out of line are there) */
		if (copy)
		{
			row_copy(f, copy, made ? 1 : -1);
			heap_freetuple(copy);
			if (f->give_up)
				break;
		}
		CHECK_FOR_INTERRUPTS();
	}
}

/* What a fold or a read of changes did, counted. */
static void
count_work(const Finder *f)
{
	if (!prolly_fold_counted)
		return;
	counters.pages += f->work.pages;
	counters.candidates += f->work.candidates;
	counters.full += f->work.full;
	counters.rows_read += f->work.rows_read;
	counters.rows_written += f->work.rows_written;
	pgstat_count_prolly(PGSTAT_PROLLY_FOLD_CANDIDATES, f->work.candidates);
	pgstat_count_prolly(PGSTAT_PROLLY_FOLD_FULL, f->work.full);
	pgstat_count_prolly(PGSTAT_PROLLY_FOLD_ROWS_READ, f->work.rows_read);
	pgstat_count_prolly(PGSTAT_PROLLY_FOLD_ROWS_WRITTEN, f->work.rows_written);
	pgstat_count_prolly(PGSTAT_PROLLY_FOLD_MERGE_BUILDS, f->work.merges);
}

/* ----------------------------------------------------------------
 * Callbacks
 * ----------------------------------------------------------------
 */

static void
log_fold(Relation rel, const ProllyTreeEnv *env,
		 const ProllyTableFold *baseline, ProllyFullFoldReason no_baseline,
		 ProllyNodeRef *data, StringInfo next)
{
	Finder		f;
	State		st;
	ProllyComponents c;
	uint64		identity = 0;
	bool		have;
	bool		stale;
	MemoryContext cxt;
	MemoryContext old;

	/* (on a hot standby, a fold only reads: it advances no baseline) */
	Assert(!RecoveryInProgress() || next == NULL);

	/*
	 * (a table "pages" folded before keeps its fold, so each page's first
	 * change since copies it: no one reads those copies now, so a fold
	 * that begins a baseline forgets it)
	 */
	if (next != NULL && RelationGetSmgr(rel)->smgr_which == SMGR_BMAP &&
		bmap_folded(RelationGetSmgr(rel)))
		bmap_fold_forget(RelationGetSmgr(rel));

	/* what a table's fold makes is let go of when it's done */
	cxt = AllocSetContextCreate(CurrentMemoryContext, "log fold",
								ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cxt);
	finder_open(&f, rel, env);
	prolly_components_for_relation(rel, &c);
	if (c.change_log)
		identity = PROLLY_CALL(c.change_log, identity, rel);
	have = identity != 0 && baseline != NULL &&
		parse_state(&baseline->state, &st) && st.log == identity;
	if (prolly_full_fold_wanted(&f.reason))
		;
	else if (baseline == NULL)
		f.reason = no_baseline;
	else if (identity == 0)
		f.reason = PROLLY_FULL_NO_LOG;
	else if (!have)
		f.reason = PROLLY_FULL_LOG_CHANGED;
	/* (a snapshot older than the baseline's fold: see the file's header) */
	stale = have && !sees_fold(f.snap, &st);
	if (have && f.reason == PROLLY_FULL_NONE)
	{
		if ((st.flags & STATE_UNSETTLED) && !sees_no_more(f.snap, &st))
			f.reason = PROLLY_FULL_UNSETTLED;
		else if (stale && !PROLLY_HAS(c.change_log, read_unseen))
			f.reason = PROLLY_FULL_STALE_SNAPSHOT;
		else if (storage_unseen(rel, f.snap))
			f.reason = PROLLY_FULL_STORAGE_UNSEEN;
	}
	have = have && f.reason == PROLLY_FULL_NONE;
	/* (a baseline as unsettled, still not holding what it didn't) */
	if (have && (st.flags & STATE_UNSETTLED))
		f.unsettled = true;
	if (!have || !fold_changed(&f, baseline, &st, stale, c.change_log, data))
	{
		instr_time	start;
		instr_time	took;

		/* (what the fold from the baseline read is thrown away) */
		memset(&f.work, 0, sizeof(f.work));
		spill_end(&f);
		MemoryContextReset(f.ccxt);
		f.c = NULL;
		f.n = f.max = 0;
		/* (by a stale snapshot: what a newer fold consumed isn't read) */
		if (stale)
			f.unsettled = true;
		INSTR_TIME_SET_CURRENT(start);
		fold_all(&f, data);
		INSTR_TIME_SET_CURRENT(took);
		INSTR_TIME_SUBTRACT(took, start);
		prolly_full_fold_noted(rel, f.reason == PROLLY_FULL_NONE ?
							   PROLLY_FULL_TOO_MANY : f.reason,
							   (int64) f.work.rows_read,
							   INSTR_TIME_GET_MILLISEC(took));
	}
	count_work(&f);
	if (next)
	{
		State		ns = {0};

		ns.flags = f.unsettled ? STATE_UNSETTLED : 0;
		ns.log = identity;
		state_of_snapshot(&ns, f.snap);
		MemoryContextSwitchTo(old);
		put_state(next, &ns);
		MemoryContextSwitchTo(cxt);
	}
	finder_close(&f);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
}

/*
 * Leaving a branch folds nothing: the log keeps nothing a fold frees but
 * its entries, which the branch's next fold reads.
 */
static bool
log_fold_on_leave(void)
{
	return false;
}

typedef struct VisitChanges
{
	ProllyRowChangeVisit visit;
	void	   *arg;
} VisitChanges;

static bool
visit_emit(Finder *f, ProllyDiffKind kind, const ProllyKey *key,
		   const ProllyValue *before, const ProllyValue *after, void *arg)
{
	VisitChanges *vc = arg;

	return vc->visit(kind, key, before, after, vc->arg);
}

/*
 * The rows that differ from the baseline's, from the log's entries since,
 * as a fold from the baseline finds them, without the tree: only where that
 * fold would read nothing else (not in full, not for a snapshot that
 * doesn't see the baseline's fold, nor from a baseline unsettled, nor for a
 * keyless table, whose rows are counts, nor after the table was emptied).
 */
static bool
log_changes(Relation rel, const ProllyTreeEnv *env,
			const ProllyTableFold *baseline, ProllyRowChangeVisit visit,
			void *arg)
{
	Finder		f;
	State		st;
	ProllyComponents c;
	uint64		identity = 0;
	bool		ok;
	MemoryContext cxt;
	MemoryContext old;

	cxt = AllocSetContextCreate(CurrentMemoryContext, "log changes",
								ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cxt);
	finder_open(&f, rel, env);
	prolly_components_for_relation(rel, &c);
	if (c.change_log)
		identity = PROLLY_CALL(c.change_log, identity, rel);
	ok = !f.keyless && !f.give_up && !prolly_full_fold_wanted(&f.reason) &&
		identity != 0 && parse_state(&baseline->state, &st) &&
		st.log == identity && sees_fold(f.snap, &st) &&
		!(st.flags & STATE_UNSETTLED) && !storage_unseen(rel, f.snap);
	if (ok)
	{
		ProllyLogPosition pos = {st.fxmax, st.fown, st.nxip, st.fxip};

		f.compact = true;
		ok = PROLLY_CALL(c.change_log, read, f.rel, f.snap, &pos, entry_visit,
						 &f) &&
			!f.give_up && !f.emptied;
	}
	if (ok)
	{
		VisitChanges vc = {visit, arg};

		compact_copies(&f);
		unique_keys(&f);
		each_change(&f, &baseline->data, visit_emit, &vc);
		count_work(&f);
	}
	finder_close(&f);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
	return ok;
}

/* (log entries counted, to a limit; an emptying stops the count unknown) */
typedef struct EntryCount
{
	int			n;
	int			limit;
	bool		unknown;
} EntryCount;

static bool
count_entry(HeapTuple entry, int32 delta, void *arg)
{
	EntryCount *c = arg;

	if (entry == NULL)
	{
		c->unknown = true;
		return false;
	}
	return ++c->n < c->limit;
}

/*
 * Fewer than limit rows differ from the baseline's: log_changes()'s
 * checks, and fewer than limit log entries since the baseline (each
 * changes a row at most: the rows that differ are fewer still).  Only the
 * entries' count is read, stopping at limit; no candidate is made.
 */
static bool
log_fewer_changes(Relation rel, const ProllyTableFold *baseline, int limit)
{
	Snapshot	snap = GetActiveSnapshot();
	ProllyComponents c;
	ProllyFullFoldReason reason;
	uint64		identity = 0;
	State		st;
	EntryCount	count = {0, Max(limit, 1), false};
	ProllyLogPosition pos;

	prolly_components_for_relation(rel, &c);
	if (c.change_log)
		identity = PROLLY_CALL(c.change_log, identity, rel);
	if (prolly_full_fold_wanted(&reason) || identity == 0 ||
		!parse_state(&baseline->state, &st) || st.log != identity ||
		!sees_fold(snap, &st) || (st.flags & STATE_UNSETTLED) ||
		storage_unseen(rel, snap))
		return false;
	pos = (ProllyLogPosition) {st.fxmax, st.fown, st.nxip, st.fxip};
	(void) PROLLY_CALL(c.change_log, read, rel, snap, &pos, count_entry,
					   &count);
	return !count.unknown && count.n < count.limit;
}

/*
 * Why log_fold() from baseline would read rel in full, from the checks it
 * makes before reading the log (the log's entries can say so too: a
 * truncation, a rewrite, more rows than the table holds; those aren't
 * read here).
 */
static ProllyFullFoldReason
log_full_reason(Relation rel, const ProllyTableFold *baseline)
{
	Snapshot	snap = GetActiveSnapshot();
	ProllyComponents c;
	ProllyFullFoldReason reason = PROLLY_FULL_NONE;
	uint64		identity = 0;
	State		st;

	if (prolly_full_fold_wanted(&reason))
		return reason;
	if (baseline == NULL)
		return PROLLY_FULL_NO_BASELINE;
	prolly_components_for_relation(rel, &c);
	if (c.change_log)
		identity = PROLLY_CALL(c.change_log, identity, rel);
	if (identity == 0)
		return PROLLY_FULL_NO_LOG;
	if (!parse_state(&baseline->state, &st) || st.log != identity)
		return PROLLY_FULL_LOG_CHANGED;
	if ((st.flags & STATE_UNSETTLED) && !sees_no_more(snap, &st))
		return PROLLY_FULL_UNSETTLED;
	if (!sees_fold(snap, &st) && !PROLLY_HAS(c.change_log, read_unseen))
		return PROLLY_FULL_STALE_SNAPSHOT;
	if (storage_unseen(rel, snap))
		return PROLLY_FULL_STORAGE_UNSEEN;
	return PROLLY_FULL_NONE;
}

/* A state holds no nodes: the log's entries are rows, not chunks. */
static void
log_walk(const ProllyTreeEnv *env, const ProllyValue *state,
		 ProllyTreeVisitNode visit, void *arg)
{
}

/* (a log's state names no chunk) */
static void
log_state_refs(const ProllyValue *state, StringInfo refs)
{
}

static void
log_counters(ProllyChangeCounters *out, bool reset)
{
	*out = counters;
	if (reset)
		memset(&counters, 0, sizeof(counters));
}

static bool
log_fold_rows(Relation rel, const ProllyTreeEnv *env,
			  const ProllyNodeRef *data_in, ItemPointerData *tids, int ntids,
			  ProllyNodeRef *data)
{
	Finder		f;
	bool		ok;
	MemoryContext cxt;
	MemoryContext old;

	cxt = AllocSetContextCreate(CurrentMemoryContext, "own rows",
								ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cxt);
	finder_open(&f, rel, env);
	own_copies(&f, tids, ntids);

	/*
	 * (copies past maintenance_work_mem weren't kept: the transaction's
	 * working set is committed instead, folded from the baseline)
	 */
	ok = !f.give_up && apply_copies(&f, data_in, data);
	finder_close(&f);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
	return ok;
}

/* Would log_advance() take rel's state as it is?  (Into c, identity, st.) */
static bool
advance_holds(Relation rel, const ProllyValue *state, ProllyComponents *c,
			  uint64 *identity, State *st)
{
	Snapshot	snap = GetActiveSnapshot();

	prolly_components_for_relation(rel, c);
	if (c->change_log == NULL || !PROLLY_HAS(c->change_log, consume_own))
		return false;
	*identity = PROLLY_CALL(c->change_log, identity, rel);
	return *identity != 0 && parse_state(state, st) && st->log == *identity &&
		!(st->flags & STATE_UNSETTLED) && sees_fold(snap, st) &&
		!storage_unseen(rel, snap);
}

/* can_advance(): log_advance()'s checks, consuming nothing. */
static bool
log_can_advance(Relation rel, const ProllyValue *state)
{
	ProllyComponents c;
	uint64		identity = 0;
	State		st;

	return advance_holds(rel, state, &c, &identity, &st);
}

/*
 * The state of a baseline that holds rel's last baseline's rows with the
 * transaction's own changes applied (a commit of them alone), into next:
 * as of the active snapshot, its fold's transaction this one; or, where
 * others' entries since the last's position wait, at that position still.
 * The transaction's entries are consumed; the rest stay, the changes of
 * others the new baseline doesn't hold.  False, consuming nothing, where
 * the last baseline's state doesn't hold so: not this finder's with the
 * table's log as it is, unsettled, its fold one the snapshot doesn't see,
 * the table's storage changed unseen, or the log can't tell the
 * transaction's entries.
 */
static bool
log_advance(Relation rel, const ProllyValue *state, StringInfo next)
{
	ProllyComponents c;
	uint64		identity = 0;
	State		st;
	State		ns = {0};
	ProllyLogPosition rest = {0};
	Snapshot	snap = GetActiveSnapshot();

	if (!advance_holds(rel, state, &c, &identity, &st))
		return false;
	{
		ProllyLogPosition pos = {st.fxmax, st.fown, st.nxip, st.fxip};

		if (!PROLLY_CALL(c.change_log, consume_own, rel, &pos, &rest))
			return false;
	}
	ns.log = identity;
	if (!FullTransactionIdIsValid(rest.xmax))
		state_of_snapshot(&ns, snap);
	else
	{
		/*
		 * (others' entries the new baseline doesn't hold either: its
		 * position is the last's, which reads them, the transaction its
		 * own; not the snapshot's, which would skip them)
		 */
		ns.fxmax = rest.xmax;
		ns.xmax = XidFromFullTransactionId(rest.xmax);
		ns.fown = rest.own;
		ns.own = XidFromFullTransactionId(rest.own);
		ns.nxip = rest.nxip;
		ns.fxip = palloc_array(FullTransactionId, Max(ns.nxip, 1));
		ns.xip = palloc_array(TransactionId, Max(ns.nxip, 1));
		if (ns.nxip > 0)
			memcpy(ns.fxip, rest.xip, ns.nxip * sizeof(FullTransactionId));
		qsort(ns.fxip, ns.nxip, sizeof(FullTransactionId), full_xid_cmp);
		for (uint32 i = 0; i < ns.nxip; i++)
			ns.xip[i] = XidFromFullTransactionId(ns.fxip[i]);
	}
	put_state(next, &ns);
	return true;
}

const ProllyChangeFinderRoutine prolly_changes_log = {
	.info = {
		.iface = PROLLY_IFACE_CHANGE_FINDER,
		.impl_id = 2,
		.format_version = STATE_VERSION,
		.name = "log",
		.description = "rows the table's change log names as changed since "
		"its fold",
	},
	.fold = log_fold,
	.walk = log_walk,
	.counters = log_counters,
	.fold_rows = log_fold_rows,
	.state_refs = log_state_refs,
	.advance = log_advance,
	.can_advance = log_can_advance,
	.full_reason = log_full_reason,
	.fold_on_leave = log_fold_on_leave,
	.changes = log_changes,
	.fewer_changes = log_fewer_changes,
};
