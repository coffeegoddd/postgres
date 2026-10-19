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
 * entry is null (it was written under another key), when the entries name
 * more rows than the table has, and during recovery.  A branch's copy of a
 * table has its
 * source's baseline and a copy of its log, so it folds from them.  Its
 * pages are never kept as of a fold: only sharing between branches copies
 * them.
 *
 * A fold by a snapshot that doesn't see its baseline's fold reads the
 * table in full, but by an old snapshot: rows written meanwhile whose
 * entries another fold consumed are neither in what it reads nor left in
 * the log.  Its baseline is marked unsettled, so the next fold reads the
 * table in full again, by its own snapshot.
 *
 * The entries a fold reads are kept once per row (a key's, or a keyless
 * row's with its count summed), so the memory a fold takes is of the rows
 * changed, not of the writes.
 *
 * A baseline's state: a tag and version, flags, the log's identity, and the
 * fold's snapshot's xmax, the fold's own transaction (if it had written)
 * and the transactions it saw running (a count, then each).
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
#include "storage/bufmgr.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/* (a first byte no "pages" state has: neither reads the other's) */
#define STATE_TAG			'L'
#define STATE_VERSION		2		/* 2: flags */

#define STATE_UNSETTLED		0x01	/* folded by a snapshot older than its
									 * baseline's fold: fold in full next */

/* Entries kept before they are first made one per row. */
#define COMPACT_AT			65536

typedef struct State
{
	uint8		flags;
	uint64		log;			/* the log's identity then */
	TransactionId xmax;			/* the fold's snapshot's */
	uint32		nxip;			/* the transactions it saw running, sorted */
	TransactionId *xip;
	TransactionId own;			/* the fold's transaction, if it wrote */
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
	TupleTableSlot *keyslot;	/* virtual: the key columns, else nulls */
	TupleTableSlot *rowslot;
	Datum	   *values;
	bool	   *isnull;
	Copy	   *c;
	int64		n;
	int64		max;
	MemoryContext ccxt;			/* the copies' keys and rows */
	bool		compact;		/* entries: kept once per row */
	int64		limit;			/* more rows than this: fold in full */
	bool		give_up;
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

static void
put_state(StringInfo out, const State *st)
{
	appendStringInfoChar(out, STATE_TAG);
	appendStringInfoChar(out, (char) STATE_VERSION);
	appendStringInfoChar(out, (char) st->flags);
	put_u64(out, st->log);
	put_u32(out, st->xmax);
	put_u32(out, st->own);
	put_u32(out, st->nxip);
	for (uint32 i = 0; i < st->nxip; i++)
		put_u32(out, st->xip[i]);
}

#define STATE_FIXED_LEN	(3 + 8 + 4 + 4 + 4)

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
	st->xmax = get_u32(p + 11);
	st->own = get_u32(p + 15);
	st->nxip = get_u32(p + 19);
	if ((uint64) state->len != STATE_FIXED_LEN + (uint64) st->nxip * 4)
		return false;
	p += STATE_FIXED_LEN;
	st->xip = palloc_array(TransactionId, Max(st->nxip, 1));
	for (uint32 i = 0; i < st->nxip; i++, p += 4)
		st->xip[i] = get_u32(p);
	return true;
}

static int
xid_cmp(const void *a, const void *b)
{
	return pg_cmp_u32(*(const TransactionId *) a, *(const TransactionId *) b);
}

/*
 * Does the snapshot see every transaction the baseline's fold saw as
 * committed?  Each the fold saw finished (below its xmax, not running
 * then) must be below the snapshot's xmax and not running for it; and the
 * fold's own transaction, whose writes it saw, must be this one or one the
 * snapshot sees finished.  (Aborted, it took the baseline with it: so
 * finished is committed.)
 */
static bool
sees_fold(Snapshot snap, const State *st)
{
	if (snap->takenDuringRecovery || !TransactionIdIsNormal(st->xmax) ||
		TransactionIdPrecedes(snap->xmax, st->xmax))
		return false;
	if (TransactionIdIsNormal(st->own) &&
		!TransactionIdIsCurrentTransactionId(st->own) &&
		XidInMVCCSnapshot(st->own, snap))
		return false;
	for (uint32 i = 0; i < snap->xcnt; i++)
		if (TransactionIdPrecedes(snap->xip[i], st->xmax) &&
			bsearch(&snap->xip[i], st->xip, st->nxip, sizeof(TransactionId),
					xid_cmp) == NULL)
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
				f->give_up = true;
			else
				f->eqproc[i] = get_opcode(op);
		}
	}
}

static void
finder_close(Finder *f)
{
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

static void
add_copy(Finder *f, const StringInfoData *key, const char *row, uint32 rowlen,
		 int64 delta)
{
	Copy	   *c;

	if (f->n == f->max)
	{
		if (f->compact && f->n >= COMPACT_AT)
		{
			compact_copies(f);
			if (f->n > f->limit)
			{
				f->give_up = true;
				return;
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
	c = &f->c[f->n++];
	c->keylen = key->len;
	c->key = MemoryContextAlloc(f->ccxt, Max(key->len, 1));
	memcpy(c->key, key->data, key->len);
	c->rowlen = rowlen;
	c->row = MemoryContextAlloc(f->ccxt, Max(rowlen, 1));
	memcpy(c->row, row, rowlen);
	c->delta = delta;
}

/* A whole row's copy: the tuple's key and encoded row, counted delta. */
static void
row_copy(Finder *f, HeapTuple tup, int64 delta)
{
	ProllyHash	content;

	ExecForceStoreHeapTuple(tup, f->rowslot, false);
	resetStringInfo(&f->key);
	resetStringInfo(&f->row);
	PROLLY_CALL(f->kr, encode_row, f->k, f->rowslot, &f->key, &f->row, &content);
	ExecClearTuple(f->rowslot);
	add_copy(f, &f->key, f->row.data, f->row.len, delta);
}

/*
 * An entry of the log: with a key, its key a candidate (its tuple kept, to
 * look the key up by); without, its row's copy counted.
 */
static bool
entry_visit(HeapTuple entry, int32 delta, void *arg)
{
	Finder	   *f = arg;

	if (f->keyless)
	{
		/* (a key's entry: the table was keyed when it was written) */
		if (delta == 0)
		{
			f->give_up = true;
			return false;
		}
		row_copy(f, entry, delta);
	}
	else
	{
		ProllyHash	content;

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
				return false;
			}
			f->keyslot->tts_values[a] = f->values[a];
			f->keyslot->tts_isnull[a] = f->isnull[a];
		}
		ExecStoreVirtualTuple(f->keyslot);
		resetStringInfo(&f->key);
		resetStringInfo(&f->row);
		PROLLY_CALL(f->kr, encode_row, f->k, f->keyslot, &f->key, &f->row, &content);
		add_copy(f, &f->key, (const char *) entry->t_data, entry->t_len, 0);
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
	ProllyHash	content;

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
	PROLLY_CALL(f->kr, encode_row, f->k, f->rowslot, &f->key, &f->row, &content);
	ExecClearTuple(f->rowslot);
	return true;
}

/* Each candidate's old and new rows; the pairs that differ are edits. */
static void
fold_keys(Finder *f, const ProllyTableFold *baseline, ProllyNodeRef *data)
{
	ProllyEdit *edits;
	int			nedits = 0;
	int			u = 0;
	ProllyTreeCursor *cur;
	IndexScanDesc scan;
	StringInfoData value;

	/* (each key once, as the keying tells keys apart) */
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

	edits = MemoryContextAllocHuge(CurrentMemoryContext,
								   Max(f->n, 1) * sizeof(ProllyEdit));
	cur = PROLLY_CALL(f->tree, cursor_open, &f->env, &baseline->data,
					  CurrentMemoryContext);
	scan = index_beginscan(f->rel, f->pk, false, f->snap, NULL, f->nkeys, 0,
						   SO_NONE);
	initStringInfo(&value);
	for (int64 i = 0; i < f->n; i++)
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
			ProllyEdit *e = &edits[nedits++];
			char	   *kb = palloc(Max(f->key.len, 1));
			char	   *vb = palloc(Max(value.len, 1));

			memcpy(kb, f->key.data, f->key.len);
			memcpy(vb, value.data, value.len);
			e->key.data = (const uint8 *) kb;
			e->key.len = f->key.len;
			e->value.data = (const uint8 *) vb;
			e->value.len = value.len;
			e->is_delete = false;
		}
		else if (had)
		{
			ProllyEdit *e = &edits[nedits++];

			e->key = ck;
			e->is_delete = true;
		}
		CHECK_FOR_INTERRUPTS();
	}
	index_endscan(scan);
	PROLLY_CALL(f->tree, cursor_close, cur);
	f->work.rows_written += nedits;
	if (nedits > 0)
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

/* A row read: its key and encoding, each allocated to its length. */
typedef struct FoldRow
{
	char	   *key;
	uint32		keylen;
	char	   *row;
	uint32		rowlen;
} FoldRow;

static int
fold_row_cmp(const void *a, const void *b, void *arg)
{
	const FoldRow *ra = *(FoldRow *const *) a;
	const FoldRow *rb = *(FoldRow *const *) b;
	ProllyKey	ka = {(const uint8 *) ra->key, ra->keylen};
	ProllyKey	kb = {(const uint8 *) rb->key, rb->keylen};
	int			r = keying_compare(&ka, &kb, arg);

	/* equal keys of a keyless table: the same row; order by bytes anyway */
	if (r == 0)
		r = memcmp(ra->key, rb->key, Min(ra->keylen, rb->keylen));
	return r != 0 ? r : (ra->keylen > rb->keylen) - (ra->keylen < rb->keylen);
}

/*
 * Every row the snapshot sees, folded into a data tree: each row's key and
 * canonical encoding, in key order, identical rows of a keyless table
 * counted once.
 */
static void
fold_all(Finder *f, ProllyNodeRef *data)
{
	TableScanDesc scan;
	FoldRow   **rows = NULL;
	int64		nrows = 0;
	int64		maxrows = 0;
	ProllyTreeBuilder *b;

	f->work.full++;
	scan = table_beginscan(f->rel, f->snap, 0, NULL, SO_NONE);
	while (table_scan_getnextslot(scan, ForwardScanDirection, f->rowslot))
	{
		FoldRow    *r = palloc(sizeof(FoldRow));
		ProllyHash	content;

		resetStringInfo(&f->key);
		resetStringInfo(&f->row);
		PROLLY_CALL(f->kr, encode_row, f->k, f->rowslot, &f->key, &f->row, &content);
		r->keylen = f->key.len;
		r->key = palloc(Max(r->keylen, 1));
		memcpy(r->key, f->key.data, r->keylen);
		r->rowlen = f->row.len;
		r->row = palloc(Max(r->rowlen, 1));
		memcpy(r->row, f->row.data, r->rowlen);
		if (nrows == maxrows)
		{
			maxrows = Max(64, maxrows * 2);
			rows = rows ? repalloc_huge(rows, maxrows * sizeof(FoldRow *)) :
				MemoryContextAllocHuge(CurrentMemoryContext,
									   maxrows * sizeof(FoldRow *));
		}
		rows[nrows++] = r;
		CHECK_FOR_INTERRUPTS();
	}
	table_endscan(scan);
	f->work.rows_read += nrows;

	if (nrows > 1)
		qsort_arg(rows, nrows, sizeof(FoldRow *), fold_row_cmp, f);

	b = PROLLY_CALL(f->tree, build_begin, &f->env, CurrentMemoryContext);
	for (int64 i = 0; i < nrows;)
	{
		int64		j = i + 1;
		ProllyKey	k = {(const uint8 *) rows[i]->key, rows[i]->keylen};
		ProllyValue row = {(const uint8 *) rows[i]->row, rows[i]->rowlen};
		StringInfoData value;
		ProllyValue v;

		while (j < nrows && rows[j]->keylen == rows[i]->keylen &&
			   memcmp(rows[j]->key, rows[i]->key, k.len) == 0)
			j++;
		if (!f->keyless && j > i + 1)
			ereport(ERROR,
					(errcode(ERRCODE_UNIQUE_VIOLATION),
					 errmsg("table \"%s\" holds rows with the same primary key",
							RelationGetRelationName(f->rel)),
					 errdetail("A commit can't hold them until the key is unique again.")));
		initStringInfo(&value);
		PROLLY_CALL(f->kr, make_value, f->k, &row, (uint64) (j - i), &value);
		v.data = (const uint8 *) value.data;
		v.len = value.len;
		PROLLY_CALL(f->tree, build_add, b, &k, &v);
		pfree(value.data);
		f->work.rows_written++;
		i = j;
	}
	PROLLY_CALL(f->tree, build_finish, b, data);
}

/*
 * Fold from the baseline, its state 'st': false if the table must be read
 * in full instead.
 */
static bool
fold_changed(Finder *f, const ProllyTableFold *baseline, const State *st,
			 const ProllyChangeLogRoutine *log, ProllyNodeRef *data)
{
	f->compact = true;
	if (f->give_up ||
		!PROLLY_CALL(log, read, f->rel, f->snap, entry_visit, f) ||
		f->give_up)
		return false;
	compact_copies(f);
	if (!f->keyless)
	{
		fold_keys(f, baseline, data);
		return true;
	}
	return apply_copies(f, &baseline->data, data);
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
		}
		CHECK_FOR_INTERRUPTS();
	}
}

/* ----------------------------------------------------------------
 * Callbacks
 * ----------------------------------------------------------------
 */

static void
log_fold(Relation rel, const ProllyTreeEnv *env,
		 const ProllyTableFold *baseline, ProllyNodeRef *data,
		 StringInfo next)
{
	Finder		f;
	State		st;
	ProllyComponents c;
	uint64		identity = 0;
	bool		have;
	bool		stale;
	MemoryContext cxt;
	MemoryContext old;

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
	/* (a snapshot older than the baseline's fold: see the file's header) */
	stale = have && !sees_fold(f.snap, &st);
	have = have && !stale && !RecoveryInProgress() &&
		!(st.flags & STATE_UNSETTLED) && !storage_unseen(rel, f.snap);
	if (!have || !fold_changed(&f, baseline, &st, c.change_log, data))
	{
		/* (what the fold from the baseline read is thrown away) */
		memset(&f.work, 0, sizeof(f.work));
		MemoryContextReset(f.ccxt);
		f.c = NULL;
		f.n = f.max = 0;
		fold_all(&f, data);
	}
	counters.pages += f.work.pages;
	counters.candidates += f.work.candidates;
	counters.full += f.work.full;
	counters.rows_read += f.work.rows_read;
	counters.rows_written += f.work.rows_written;
	pgstat_count_prolly(PGSTAT_PROLLY_FOLD_CANDIDATES, f.work.candidates);
	pgstat_count_prolly(PGSTAT_PROLLY_FOLD_FULL, f.work.full);
	pgstat_count_prolly(PGSTAT_PROLLY_FOLD_ROWS_READ, f.work.rows_read);
	pgstat_count_prolly(PGSTAT_PROLLY_FOLD_ROWS_WRITTEN, f.work.rows_written);
	if (next)
	{
		State		ns = {0};

		ns.flags = stale ? STATE_UNSETTLED : 0;
		ns.log = identity;
		ns.xmax = f.snap->xmax;
		ns.nxip = f.snap->xcnt;
		ns.xip = palloc_array(TransactionId, Max(ns.nxip, 1));
		if (ns.nxip > 0)
			memcpy(ns.xip, f.snap->xip, ns.nxip * sizeof(TransactionId));
		qsort(ns.xip, ns.nxip, sizeof(TransactionId), xid_cmp);
		ns.own = GetTopTransactionIdIfAny();
		MemoryContextSwitchTo(old);
		put_state(next, &ns);
		MemoryContextSwitchTo(cxt);
	}
	finder_close(&f);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
}

/* A state holds no nodes: the log's entries are rows, not chunks. */
static void
log_walk(const ProllyTreeEnv *env, const ProllyValue *state,
		 ProllyTreeVisitNode visit, void *arg)
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
	ok = apply_copies(&f, data_in, data);
	finder_close(&f);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
	return ok;
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
};
