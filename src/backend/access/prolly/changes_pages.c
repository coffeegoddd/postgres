/*-------------------------------------------------------------------------
 *
 * changes_pages.c
 *	  Change finder implementation "pages": a table's rows changed since a
 *	  fold, found from the pages its storage lists as changed.
 *
 * A fold of a block-mapped table (bmap_fold_begin()) keeps its pages as
 * they are: each page changed since is listed, with the block holding it as
 * it was.  Every tuple on either version of a listed page, visible or not,
 * and on every page past the table's shortest length since, names a
 * candidate: its primary key.  A candidate's old row is the baseline tree's;
 * its new row is the one the snapshot sees by the primary key now; the
 * pairs that differ are the edits that make the baseline's tree the
 * table's tree now.  Visibility on the old pages is never consulted, so a
 * row an update moved to another page, being on two listed pages, is paired
 * up by its key, and VACUUM may prune old versions freely.
 *
 * A fold's snapshot doesn't see transactions running when it was taken, or
 * committed since; their rows stay as they were in the tree, and the keys
 * of the tuples they wrote or deleted are carried into the next baseline,
 * to be candidates again.  (A table without a primary key carries the
 * blocks that hold them.)
 *
 * A table without a primary key keys a row by its content and counts its
 * copies: of each row on a listed page, or a carried one, the copies the
 * fold saw on the page's old version (by the fold's snapshot) are taken
 * from the baseline's count, and those the snapshot sees on its version
 * now added.  A carried page not listed is as the fold saw it.
 *
 * A table is folded in full instead, every row read, when the baseline
 * isn't of its changes list's lineage (it was never folded, or rewritten
 * since, or the list was pruned past it), when a candidate's key column
 * (any column, without a primary key) is stored out of line on a page's
 * old version (its value may be gone), when whether the fold saw a tuple
 * of an old version can't be told (its transaction's status is gone, or
 * it was deleted by a multixact), when the baseline's fold left rows
 * unsettled whose keys it couldn't carry, when the snapshot is older than
 * the baseline's fold's (which may have seen transactions the snapshot
 * doesn't), and during recovery (a standby replays prunes without waiting
 * for its readers).
 *
 * A baseline's state: a version byte, the list's lineage, the fold's epoch,
 * the table's length then, the fold's snapshot's xmax and the transactions
 * it saw running (a count, then each), flags, the fold's own transaction
 * (if it had written), and the tree of carried keys
 * (each key with the key columns' encoded row, to look it up by; an empty
 * hash for none), then the blocks carried (a count, then each).
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/changes_pages.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/prolly_builtins.h"
#include "access/prolly_toast.h"
#include "access/relscan.h"
#include "access/stratnum.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/pg_am_d.h"
#include "common/int.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "portability/instr_time.h"
#include "storage/bmap.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/smgr.h"
#include "utils/lsyscache.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#define STATE_VERSION		8		/* 8: rows encoded with NaNs canonical */

/* carried pages a fold keeps, whatever the table's size (past an eighth of it) */
#define CARRIED_BLOCKS_MIN	1024
#define STATE_UNSETTLED		0x01	/* rows the fold didn't see as settled,
									 * whose keys it couldn't carry */

/*
 * A lock per relation file: folds that begin one exclusive, readers of the
 * changes list shared.  An object lock's tag, its class the tablespace and
 * its sub-ID one no column has.
 */
#define FOLD_LOCK_SUBID		0xFFFF

/* A baseline's state, parsed. */
typedef struct State
{
	BmapFold	fold;
	TransactionId xmax;			/* the fold's snapshot's */
	uint32		nxip;			/* the transactions it saw running, sorted */
	TransactionId *xip;
	uint8		flags;
	TransactionId own;			/* the fold's transaction, if it wrote */
	ProllyNodeRef carried;
	uint32		nblocks_carried;	/* without a key: blocks to read again */
	BlockNumber *blocks_carried;	/* sorted */
} State;

static ProllyChangeCounters counters;

/* A row to look at: its key, and its key columns' encoded row. */
typedef struct Candidate
{
	char	   *key;
	uint32		keylen;
	char	   *keyrow;
	uint32		keyrowlen;
} Candidate;

typedef struct Finder
{
	Relation	rel;
	Snapshot	snap;
	const ProllyTreeRoutine *tree;
	ProllyTreeEnv env;			/* the caller's, ordered by the keying */
	ProllyTreeEnv memenv;		/* the same, ordered by bytes */
	const ProllyKeyingRoutine *kr;
	ProllyKeying *k;
	ProllyKeyingRefs krefs;		/* k's values out of line, in env's store */
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
	Candidate  *cands;
	int			ncands;
	int			maxcands;
	Candidate  *carried;
	int			ncarried;
	int			maxcarried;
	bool		unsettled;		/* some rows weren't, and their keys aren't
								 * carried */
	bool		give_up;
	const State *base;			/* the baseline's fold, folding from it */
	BlockNumber *cblocks;		/* without a key: the blocks carried */
	int			ncblocks;
	int			maxcblocks;
	MemoryContext foldcxt;		/* the fold's: what lasts all of it */
	ProllyChangeCounters work;	/* what this fold read, counted once it's done */
	ProllyFullFoldReason reason;	/* why it reads in full, once known */
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
	appendStringInfoChar(out, (char) STATE_VERSION);
	put_u64(out, st->fold.lineage);
	put_u32(out, st->fold.epoch);
	put_u32(out, st->fold.nblocks);
	put_u32(out, st->xmax);
	put_u32(out, st->nxip);
	for (uint32 i = 0; i < st->nxip; i++)
		put_u32(out, st->xip[i]);
	appendStringInfoChar(out, (char) st->flags);
	put_u32(out, st->own);
	appendStringInfoChar(out, (char) st->carried.hash.len);
	appendBinaryStringInfo(out, st->carried.hash.bytes, st->carried.hash.len);
	put_u32(out, st->nblocks_carried);
	for (uint32 i = 0; i < st->nblocks_carried; i++)
		put_u32(out, st->blocks_carried[i]);
}

static bool
parse_state(const ProllyValue *state, State *st)
{
	const uint8 *p = state->data;
	const uint8 *end = p + state->len;
	uint8		hlen;

	uint8		version = state->len > 0 ? p[0] : 0;

	memset(st, 0, sizeof(*st));
	/*
	 * (a baseline of another version, its data trees in a format before
	 * this one's, is folded again in full: its unchanged rows aren't kept)
	 */
	if (state->len < 1 + 8 + 4 + 4 + 4 + 4 || version != STATE_VERSION)
		return false;
	st->fold.lineage = get_u64(p + 1);
	st->fold.epoch = get_u32(p + 9);
	st->fold.nblocks = get_u32(p + 13);
	st->xmax = get_u32(p + 17);
	st->nxip = get_u32(p + 21);
	p += 25;
	if ((uint64) (end - p) < (uint64) st->nxip * 4 + 2 + 4)
		return false;
	st->xip = palloc_array(TransactionId, Max(st->nxip, 1));
	for (uint32 i = 0; i < st->nxip; i++, p += 4)
		st->xip[i] = get_u32(p);
	st->flags = *p++;
	st->own = get_u32(p);
	p += 4;
	hlen = *p++;
	if (hlen > PROLLY_HASH_MAX_LEN || end - p < hlen + 4)
		return false;
	st->carried.hash.len = hlen;
	memcpy(st->carried.hash.bytes, p, hlen);
	p += hlen;
	st->nblocks_carried = get_u32(p);
	p += 4;
	if ((uint64) (end - p) != (uint64) st->nblocks_carried * 4)
		return false;
	st->blocks_carried = palloc_array(BlockNumber, Max(st->nblocks_carried, 1));
	for (uint32 i = 0; i < st->nblocks_carried; i++, p += 4)
		st->blocks_carried[i] = get_u32(p);
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
 * finished is committed, and no transaction's status need be read.)
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
	memset(f, 0, sizeof(*f));
	f->foldcxt = CurrentMemoryContext;
	f->rel = rel;
	f->snap = GetActiveSnapshot();
	f->tree = prolly_tree_lookup("cow");
	{
		ProllyComponents c;

		prolly_components_for_relation(rel, &c);
		f->kr = c.keying;
	}
	/*
	 * Rows as a rewrite leaves them: a column added with a default is in
	 * every row (the scan gives its missing value), so whether the table was
	 * rewritten since doesn't change the encoding.
	 */
	f->k = PROLLY_CALL(f->kr, open, rel, CurrentMemoryContext);
	f->keyless = PROLLY_CALL(f->kr, keyless, f->k);
	f->env = *env;
	f->env.compare = keying_compare;
	f->env.compare_arg = f;
	f->memenv = *env;
	f->memenv.compare = prolly_bytes_compare;
	f->memenv.compare_arg = NULL;
	prolly_keying_use_env(&f->krefs, f->kr, f->k, f->tree, &f->env);
	/* (the carried keys' rows hold references too, if a key is long) */
	prolly_keying_use_env_rows(&f->krefs, &f->memenv);
	f->desc = RelationGetDescr(rel);
	f->values = palloc_array(Datum, f->desc->natts);
	f->isnull = palloc_array(bool, f->desc->natts);
	f->keyslot = MakeSingleTupleTableSlot(f->desc, &TTSOpsVirtual);
	f->rowslot = table_slot_create(rel, NULL);

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

static void
finder_close(Finder *f)
{
	if (f->pk)
		index_close(f->pk, AccessShareLock);
	ExecDropSingleTupleTableSlot(f->keyslot);
	ExecDropSingleTupleTableSlot(f->rowslot);
	PROLLY_CALL(f->kr, close, f->k);
}

/*
 * Is xid's transaction one the snapshot doesn't see as committed or not?
 * The fold's own transaction's writes the snapshot sees (cid, the write's
 * command, before the snapshot's) are settled for it: they go with the
 * fold if the transaction (or a subtransaction both are in) rolls back,
 * and what it writes after changes pages since.  Writes the snapshot
 * doesn't see yet aren't.
 */
static bool
xid_unsettled(Finder *f, HeapTupleHeader t, bool updater)
{
	TransactionId xid = updater ? HeapTupleHeaderGetUpdateXid(t) :
		HeapTupleHeaderGetRawXmin(t);

	if (!TransactionIdIsNormal(xid))
		return false;
	if (TransactionIdIsCurrentTransactionId(xid))
		return (updater ? HeapTupleHeaderGetCmax(t) :
				HeapTupleHeaderGetCmin(t)) >= f->snap->curcid;
	return XidInMVCCSnapshot(xid, f->snap);
}

static bool
tuple_unsettled(Finder *f, HeapTupleHeader t)
{
	uint16		mask = t->t_infomask;

	/* (one hinted as aborted is settled, without asking) */
	if (!HeapTupleHeaderXminFrozen(t) && !(mask & HEAP_XMIN_INVALID) &&
		xid_unsettled(f, t, false))
		return true;
	if (!(mask & HEAP_XMAX_INVALID) && !HEAP_XMAX_IS_LOCKED_ONLY(mask))
		return xid_unsettled(f, t, true);
	return false;
}

static void
add_candidate(Candidate **arr, int *n, int *max, const StringInfoData *key,
			  const StringInfoData *keyrow)
{
	Candidate  *c;

	if (*n == *max)
	{
		*max = Max(64, *max * 2);
		*arr = *arr ? repalloc_array(*arr, Candidate, *max) :
			palloc_array(Candidate, *max);
	}
	c = &(*arr)[(*n)++];
	c->keylen = key->len;
	c->key = palloc(Max(key->len, 1));
	memcpy(c->key, key->data, key->len);
	c->keyrowlen = keyrow->len;
	c->keyrow = palloc(Max(keyrow->len, 1));
	memcpy(c->keyrow, keyrow->data, keyrow->len);
}

/*
 * A table without a key carries the blocks of tuples the snapshot doesn't
 * see as settled, to read again next time: unless the tuple is the fold's
 * own transaction's (written in this command or since, or by a
 * subtransaction its snapshot is older than), which the next fold, judging
 * by the snapshot, couldn't tell from one it saw; then the table is read
 * in full next time.
 */
static void
carry_tuple(Finder *f, HeapTuple tup)
{
	HeapTupleHeader t = tup->t_data;
	BlockNumber blkno = ItemPointerGetBlockNumber(&tup->t_self);
	MemoryContext old;

	if ((!HeapTupleHeaderXminFrozen(t) && !(t->t_infomask & HEAP_XMIN_INVALID) &&
		 TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetRawXmin(t))) ||
		(!(t->t_infomask & HEAP_XMAX_INVALID) &&
		 !HEAP_XMAX_IS_LOCKED_ONLY(t->t_infomask) &&
		 TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetUpdateXid(t))))
	{
		f->unsettled = true;
		return;
	}
	if (f->ncblocks > 0 && f->cblocks[f->ncblocks - 1] == blkno)
		return;
	old = MemoryContextSwitchTo(f->foldcxt);
	if (f->ncblocks == f->maxcblocks)
	{
		f->maxcblocks = Max(64, f->maxcblocks * 2);
		f->cblocks = f->cblocks ? repalloc_array(f->cblocks, BlockNumber, f->maxcblocks) :
			palloc_array(BlockNumber, f->maxcblocks);
	}
	f->cblocks[f->ncblocks++] = blkno;
	MemoryContextSwitchTo(old);
}

/*
 * Is the value of column i (0-based) out of line where it may not be read?
 * One in its TOAST relation: its row's old version may outlive it there.
 * A hash pointer's value doesn't go with its row, but one a transaction in
 * progress stored isn't anyone else's to read yet.
 */
static bool
out_of_line(TupleDesc desc, int i, Datum value, bool isnull)
{
	varlena    *v = (varlena *) DatumGetPointer(value);

	if (isnull || TupleDescAttr(desc, i)->attlen != -1)
		return false;
	return VARATT_IS_EXTERNAL_ONDISK(v) ||
		(VARATT_IS_EXTERNAL_ONDISK_HASH(v) && !prolly_toast_readable(v));
}

/* Visits a tuple of a page version; false stops the walk. */
typedef bool (*TupleVisit) (Finder *f, HeapTuple tup, void *arg);

/* Visit each tuple of a version of page blkno; false if a visit stopped. */
static bool
walk_page(Finder *f, Page page, BlockNumber blkno, TupleVisit visit, void *arg)
{
	OffsetNumber maxoff;

	f->work.pages++;
	if (PageIsNew(page))
		return true;
	maxoff = PageGetMaxOffsetNumber(page);
	for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId		lp = PageGetItemId(page, off);
		HeapTupleData tup;

		if (!ItemIdIsNormal(lp))
			continue;
		tup.t_data = (HeapTupleHeader) PageGetItem(page, lp);
		tup.t_len = ItemIdGetLength(lp);
		tup.t_tableOid = RelationGetRelid(f->rel);
		ItemPointerSet(&tup.t_self, blkno, off);
		if (!visit(f, &tup, arg))
			return false;
	}
	return true;
}

/*
 * A tuple's key and its key columns' encoded row; false if a key column is
 * stored out of line, which an old version may no longer have.
 */
static bool
tuple_key(Finder *f, HeapTuple tup, StringInfo key, StringInfo keyrow)
{
	ProllyHash	content;

	heap_deform_tuple(tup, f->desc, f->values, f->isnull);
	ExecClearTuple(f->keyslot);
	for (int i = 0; i < f->desc->natts; i++)
		f->keyslot->tts_isnull[i] = true;
	for (int i = 0; i < f->nkeys; i++)
	{
		int			a = f->keyattrs[i] - 1;

		if (out_of_line(f->desc, a, f->values[a], f->isnull[a]))
			return false;
		f->keyslot->tts_values[a] = f->values[a];
		f->keyslot->tts_isnull[a] = f->isnull[a];
	}
	ExecStoreVirtualTuple(f->keyslot);
	resetStringInfo(key);
	resetStringInfo(keyrow);
	PROLLY_CALL(f->kr, encode_row, f->k, f->keyslot, key, keyrow, &content);
	return true;
}

typedef struct PageVisit
{
	bool		current;
	bool		candidates;
	StringInfoData key;
	StringInfoData keyrow;
} PageVisit;

static bool
page_tuple(Finder *f, HeapTuple tup, void *arg)
{
	PageVisit  *pv = arg;
	bool		unsettled = pv->current && tuple_unsettled(f, tup->t_data);

	if (!pv->candidates && !unsettled)
		return true;
	if (f->keyless)
	{
		if (unsettled)
			carry_tuple(f, tup);
		return true;
	}
	if (!tuple_key(f, tup, &pv->key, &pv->keyrow))
	{
		if (pv->candidates)
		{
			f->give_up = true;
			f->reason = PROLLY_FULL_KEYING_CHANGED;
			return false;
		}
		f->unsettled = true;
		return true;
	}
	if (pv->candidates)
		add_candidate(&f->cands, &f->ncands, &f->maxcands, &pv->key, &pv->keyrow);
	if (unsettled)
		add_candidate(&f->carried, &f->ncarried, &f->maxcarried, &pv->key, &pv->keyrow);
	return true;
}

/*
 * The tuples on a version of page blkno.  With 'candidates', each names a
 * candidate, visible or not (a key out of line gives up).  Of the version
 * the table has now ('current'), keys of tuples the snapshot doesn't see
 * as settled are carried (a key out of line marks the table unsettled
 * instead; a keyless table's tuple carries its block).
 */
static void
page_tuples(Finder *f, Page page, BlockNumber blkno, bool current,
			bool candidates)
{
	PageVisit	pv = {current, candidates};

	initStringInfo(&pv.key);
	initStringInfo(&pv.keyrow);
	(void) walk_page(f, page, blkno, page_tuple, &pv);
	pfree(pv.key.data);
	pfree(pv.keyrow.data);
}

/* The current version of page blkno, read under a share lock. */
static void
current_page(Finder *f, BlockNumber blkno, bool candidates)
{
	Buffer		buf = ReadBufferExtended(f->rel, MAIN_FORKNUM, blkno,
										 RBM_NORMAL, NULL);

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page_tuples(f, BufferGetPage(buf), blkno, true, candidates);
	UnlockReleaseBuffer(buf);
}

/*
 * Look a candidate up by its key columns, with 'scan' (of the primary key,
 * by f->snap, or by SnapshotAny with 'carry'): the row the snapshot sees,
 * if any, encoded into 'key' and 'row'.  With 'carry', every version is
 * visited, and *unsettled set if one is of a transaction the snapshot
 * doesn't see as settled.
 */
static bool
lookup(Finder *f, IndexScanDesc scan, const Candidate *c, bool carry,
	   StringInfo key, StringInfo row, bool *unsettled)
{
	ProllyValue kr = {(const uint8 *) c->keyrow, c->keyrowlen};
	ScanKeyData skey[INDEX_MAX_KEYS];
	bool		found = false;
	ProllyHash	content;

	PROLLY_CALL(f->kr, decode_row, f->k, &kr, f->keyslot);
	slot_getallattrs(f->keyslot);
	for (int i = 0; i < f->nkeys; i++)
	{
		int			a = f->keyattrs[i] - 1;

		ScanKeyInit(&skey[i], i + 1, f->eqstrat[i], f->eqproc[i],
					f->keyslot->tts_values[a]);
		skey[i].sk_collation = f->pk->rd_indcollation[i];
		if (f->keyslot->tts_isnull[a])
			skey[i].sk_flags |= (SK_ISNULL | SK_SEARCHNULL);
	}
	*unsettled = false;
	index_rescan(scan, skey, f->nkeys, NULL, 0);
	while (table_index_getnext_slot(scan, ForwardScanDirection, f->rowslot))
	{
		if (carry && !*unsettled &&
			tuple_unsettled(f, ((HeapTupleTableSlot *) f->rowslot)->tuple->t_data))
			*unsettled = true;
		if (!found &&
			(!carry || table_tuple_satisfies_snapshot(f->rel, f->rowslot, f->snap)))
		{
			found = true;
			resetStringInfo(key);
			resetStringInfo(row);
			PROLLY_CALL(f->kr, encode_row, f->k, f->rowslot, key, row, &content);
		}
		if (found && (!carry || *unsettled))
			break;
	}
	return found;
}

static int
candidate_cmp(const void *a, const void *b, void *arg)
{
	const Candidate *x = a;
	const Candidate *y = b;
	ProllyKey	kx = {(const uint8 *) x->key, x->keylen};
	ProllyKey	ky = {(const uint8 *) y->key, y->keylen};

	return keying_compare(&kx, &ky, arg);
}

/* Sort candidates by the table's key order, each key once. */
static int
unique_candidates(Finder *f, Candidate *c, int n)
{
	int			u = 0;

	if (n > 1)
		qsort_arg(c, n, sizeof(Candidate), candidate_cmp, f);
	for (int i = 0; i < n; i++)
		if (u == 0 || candidate_cmp(&c[u - 1], &c[i], f) != 0)
			c[u++] = c[i];
	return u;
}

/* ----------------------------------------------------------------
 * Folding
 * ----------------------------------------------------------------
 */

typedef struct Listed
{
	BmapChange *changes;
	int			n;
	int			max;
} Listed;

static void
listed_change(const BmapChange *change, void *arg)
{
	Listed	   *l = arg;

	if (l->n == l->max)
	{
		l->max = Max(16, l->max * 2);
		l->changes = l->changes ? repalloc_array(l->changes, BmapChange, l->max) :
			palloc_array(BmapChange, l->max);
	}
	l->changes[l->n++] = *change;
}

static int
block_cmp(const void *a, const void *b)
{
	return pg_cmp_u32(*(const BlockNumber *) a, *(const BlockNumber *) b);
}

/* ----------------------------------------------------------------
 * Tables without a primary key
 *
 * A page the baseline's fold saw tuples on that weren't settled for it
 * (in flight then) is carried, to be read again; whether the fold saw a
 * tuple of an old version is told by the fold's snapshot (base_saw()), not
 * by whether its transaction committed since.  A page not listed holds
 * what it held then.  A page's first
 * listed old version is the page as the fold saw it: it was copied at the
 * page's first change since, and nothing changed it before.  (A later one,
 * from a fold that began an epoch and never became a baseline, can hold
 * rows written since.)
 * ----------------------------------------------------------------------
 */

/* A row's copies counted on some page versions: its key, row and count. */
typedef struct Copy
{
	char	   *key;
	uint32		keylen;
	char	   *row;
	uint32		rowlen;
	int64		delta;
} Copy;

typedef struct Copies
{
	Copy	   *c;
	int			n;
	int			max;
	MemoryContext cxt;			/* holds the keys and rows */
	StringInfoData key;			/* reused for each encoding */
	StringInfoData row;
} Copies;

/* A page version's tuples to count: copies, or pointers into the page. */
typedef struct Counted
{
	HeapTupleData *t;
	int			n;
} Counted;

/* What block_copies() reuses from block to block. */
typedef struct BlockScratch
{
	PGIOAlignedBlock oldpage;
	Counted		was;
	Counted		is;
	MemoryContext cxt;			/* the version now's tuples, copied */
} BlockScratch;

/*
 * Did xid commit?  *known set false if its status may be gone (older than
 * the oldest transaction whose status is kept).  The caller holds
 * XactTruncationLock.
 */
static bool
xid_committed(TransactionId xid, bool *known)
{
	if (!TransactionIdIsNormal(xid))
		return xid == FrozenTransactionId || xid == BootstrapTransactionId;
	/* (the fold's transaction, again: what it wrote then, it saw) */
	if (TransactionIdIsCurrentTransactionId(xid))
		return true;
	if (TransactionIdPrecedes(xid, TransamVariables->oldestClogXid))
	{
		*known = false;
		return false;
	}
	return TransactionIdDidCommit(xid);
}

/*
 * Did the baseline's fold see xid's writes?  Those of its own transaction,
 * and of any its snapshot saw finished that committed (hinted: known to
 * have).  *known set false if that can't be told.
 */
static bool
base_saw(Finder *f, TransactionId xid, bool hinted, bool *known)
{
	const State *st = f->base;

	if (!TransactionIdIsNormal(xid))
		return xid == FrozenTransactionId || xid == BootstrapTransactionId;
	if (TransactionIdIsNormal(st->own) && TransactionIdEquals(xid, st->own))
		return true;
	if (!TransactionIdPrecedes(xid, st->xmax) ||
		bsearch(&xid, st->xip, st->nxip, sizeof(TransactionId), xid_cmp) != NULL)
		return false;
	return hinted || xid_committed(xid, known);
}

/*
 * Was this tuple of an old page version visible to the baseline's fold?
 * *known set false if that can't be told.  The caller holds
 * XactTruncationLock and MultiXactTruncationLock.
 */
static bool
fold_saw(Finder *f, HeapTupleHeader t, bool *known)
{
	uint16		mask = t->t_infomask;
	TransactionId updater;

	if (!HeapTupleHeaderXminFrozen(t))
	{
		if (mask & HEAP_XMIN_INVALID)
			return false;
		if (!base_saw(f, HeapTupleHeaderGetRawXmin(t),
					  (mask & HEAP_XMIN_COMMITTED) != 0, known))
			return false;
	}
	if ((mask & HEAP_XMAX_INVALID) || HEAP_XMAX_IS_LOCKED_ONLY(mask))
		return true;
	if (mask & HEAP_XMAX_IS_MULTI)
	{
		MultiXactId oldest;
		MultiXactId next;

		/* (its members, while they are kept) */
		ReadMultiXactIdRange(&oldest, &next);
		if (MultiXactIdPrecedes(HeapTupleHeaderGetRawXmax(t), oldest))
		{
			*known = false;
			return false;
		}
		updater = HeapTupleHeaderGetUpdateXid(t);
		if (!TransactionIdIsValid(updater))
			return true;		/* (lockers alone) */
		return !base_saw(f, updater, false, known);
	}
	return !base_saw(f, HeapTupleHeaderGetRawXmax(t),
					 (mask & HEAP_XMAX_COMMITTED) != 0, known);
}

/*
 * Count a copy of the tuple's row, delta; false if its row can't be read
 * (a value out of line of an old page version's tuple, 'of_old', may be
 * gone).
 */
static bool
add_copy(Finder *f, Copies *cs, HeapTuple tup, int64 delta, bool of_old)
{
	ProllyHash	content;
	MemoryContext old;
	Copy	   *c;

	ExecForceStoreHeapTuple(tup, f->rowslot, false);
	slot_getallattrs(f->rowslot);
	if (of_old)
	{
		for (int i = 0; i < f->desc->natts; i++)
			if (out_of_line(f->desc, i, f->rowslot->tts_values[i],
							f->rowslot->tts_isnull[i]))
			{
				ExecClearTuple(f->rowslot);
				return false;
			}
	}
	resetStringInfo(&cs->key);
	resetStringInfo(&cs->row);
	PROLLY_CALL(f->kr, encode_row, f->k, f->rowslot, &cs->key, &cs->row, &content);
	ExecClearTuple(f->rowslot);

	old = MemoryContextSwitchTo(cs->cxt);
	if (cs->n == cs->max)
	{
		cs->max = Max(64, cs->max * 2);
		cs->c = cs->c ? repalloc_array(cs->c, Copy, cs->max) :
			palloc_array(Copy, cs->max);
	}
	c = &cs->c[cs->n++];
	c->keylen = cs->key.len;
	c->key = palloc(Max(c->keylen, 1));
	memcpy(c->key, cs->key.data, c->keylen);
	c->rowlen = cs->row.len;
	c->row = palloc(Max(c->rowlen, 1));
	memcpy(c->row, cs->row.data, c->rowlen);
	c->delta = delta;
	MemoryContextSwitchTo(old);
	return true;
}

typedef struct OldVisit
{
	Counted    *ct;
	bool		known;
} OldVisit;

/* An old version's tuple: noted (in the page) if the fold saw it. */
static bool
old_tuple(Finder *f, HeapTuple tup, void *arg)
{
	OldVisit   *ov = arg;

	if (fold_saw(f, tup->t_data, &ov->known))
		ov->ct->t[ov->ct->n++] = *tup;
	return ov->known;
}

typedef struct NowVisit
{
	Counted    *ct;
	Buffer		buf;
} NowVisit;

/*
 * The version now's tuple (its page share-locked): copied if the snapshot
 * sees it, so it is read after the page is let go of; its settledness
 * noted.
 */
static bool
now_tuple(Finder *f, HeapTuple tup, void *arg)
{
	NowVisit   *nv = arg;

	if (tuple_unsettled(f, tup->t_data))
		carry_tuple(f, tup);
	if (HeapTupleSatisfiesVisibility(tup, f->snap, nv->buf))
		nv->ct->t[nv->ct->n++] = *heap_copytuple(tup);
	return true;
}

/* The same row data, as stored? */
static bool
same_data(const HeapTupleData *a, const HeapTupleData *b)
{
	return a->t_len == b->t_len && a->t_data->t_hoff == b->t_data->t_hoff &&
		HeapTupleHeaderGetNatts(a->t_data) == HeapTupleHeaderGetNatts(b->t_data) &&
		(a->t_data->t_infomask & HEAP_HASNULL) == (b->t_data->t_infomask & HEAP_HASNULL) &&
		memcmp((char *) a->t_data + offsetof(HeapTupleHeaderData, t_bits),
			   (char *) b->t_data + offsetof(HeapTupleHeaderData, t_bits),
			   a->t_len - offsetof(HeapTupleHeaderData, t_bits)) == 0;
}

/*
 * The copies on page blkno: those the fold saw on its old version (if
 * old_block is valid), counted -1, and those the snapshot sees on its
 * version now (if 'now'), counted +1.  A tuple on both, the same row,
 * cancels unread.  False to fold in full instead.
 */
static bool
block_copies(Finder *f, Copies *cs, BlockScratch *bs, BlockNumber blkno,
			 BlockNumber old_block, bool now)
{
	Counted    *was = &bs->was;
	Counted    *is = &bs->is;
	bool		ok = true;
	MemoryContext old;

	was->n = is->n = 0;
	MemoryContextReset(bs->cxt);
	if (BlockNumberIsValid(old_block))
	{
		OldVisit	ov = {was, true};

		bmap_read_old(RelationGetSmgr(f->rel), blkno, old_block,
					  bs->oldpage.data);

		/* (the statuses of its transactions, while they are kept) */
		LWLockAcquire(XactTruncationLock, LW_SHARED);
		LWLockAcquire(MultiXactTruncationLock, LW_SHARED);
		(void) walk_page(f, (Page) bs->oldpage.data, blkno, old_tuple, &ov);
		LWLockRelease(MultiXactTruncationLock);
		LWLockRelease(XactTruncationLock);
		ok = ov.known;
	}
	if (ok && now)
	{
		Buffer		buf = ReadBufferExtended(f->rel, MAIN_FORKNUM, blkno,
											 RBM_NORMAL, NULL);
		NowVisit	nv = {is, buf};

		old = MemoryContextSwitchTo(bs->cxt);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		(void) walk_page(f, BufferGetPage(buf), blkno, now_tuple, &nv);
		UnlockReleaseBuffer(buf);
		MemoryContextSwitchTo(old);
	}

	/* rows on both versions, as they were, cancel (most of a page) */
	for (int i = 0, j = 0; ok && i < was->n; i++)
	{
		bool		kept = false;

		/* (both in line pointer order) */
		while (j < is->n &&
			   ItemPointerGetOffsetNumber(&is->t[j].t_self) <
			   ItemPointerGetOffsetNumber(&was->t[i].t_self))
			j++;
		if (j < is->n && ItemPointerEquals(&was->t[i].t_self, &is->t[j].t_self) &&
			same_data(&was->t[i], &is->t[j]))
		{
			is->t[j].t_data = NULL;
			kept = true;
		}
		if (!kept)
			ok = add_copy(f, cs, &was->t[i], -1, true);
	}
	for (int j = 0; ok && j < is->n; j++)
		if (is->t[j].t_data != NULL)
			ok = add_copy(f, cs, &is->t[j], 1, false);
	return ok;
}

/*
 * The copies on a carried page unchanged since the fold: it is as the fold
 * saw it, so each tuple counts once, by whether the fold saw it and whether
 * the snapshot does: -1 if only the fold, +1 if only the snapshot.  (One
 * read of the page, under its share lock; the tuples counted are copied,
 * and encoded after.)
 */
static bool
unchanged_copies(Finder *f, Copies *cs, BlockScratch *bs, BlockNumber blkno)
{
	Buffer		buf = ReadBufferExtended(f->rel, MAIN_FORKNUM, blkno,
										 RBM_NORMAL, NULL);
	Page		page;
	OffsetNumber maxoff;
	bool		known = true;
	bool		ok = true;
	MemoryContext old;
	int64	   *deltas = palloc_array(int64, MaxHeapTuplesPerPage);
	int			n = 0;

	MemoryContextReset(bs->cxt);
	old = MemoryContextSwitchTo(bs->cxt);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	LWLockAcquire(XactTruncationLock, LW_SHARED);
	LWLockAcquire(MultiXactTruncationLock, LW_SHARED);
	page = BufferGetPage(buf);
	f->work.pages++;
	maxoff = PageIsNew(page) ? InvalidOffsetNumber : PageGetMaxOffsetNumber(page);
	for (OffsetNumber off = FirstOffsetNumber; off <= maxoff && known; off++)
	{
		ItemId		lp = PageGetItemId(page, off);
		HeapTupleData tup;
		bool		saw;
		bool		sees;

		if (!ItemIdIsNormal(lp))
			continue;
		tup.t_data = (HeapTupleHeader) PageGetItem(page, lp);
		tup.t_len = ItemIdGetLength(lp);
		tup.t_tableOid = RelationGetRelid(f->rel);
		ItemPointerSet(&tup.t_self, blkno, off);
		if (tuple_unsettled(f, tup.t_data))
			carry_tuple(f, &tup);
		saw = fold_saw(f, tup.t_data, &known);
		sees = HeapTupleSatisfiesVisibility(&tup, f->snap, buf);
		if (saw != sees)
		{
			bs->is.t[n] = *heap_copytuple(&tup);
			deltas[n++] = sees ? 1 : -1;
		}
	}
	LWLockRelease(MultiXactTruncationLock);
	LWLockRelease(XactTruncationLock);
	UnlockReleaseBuffer(buf);
	MemoryContextSwitchTo(old);
	for (int i = 0; ok && known && i < n; i++)
		ok = add_copy(f, cs, &bs->is.t[i], deltas[i], deltas[i] < 0);
	pfree(deltas);
	return ok && known;
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

typedef struct OldPage
{
	BlockNumber blkno;
	BlockNumber old_block;
	int			listed;			/* its place on the list */
} OldPage;

/* By block, then by when listed. */
static int
old_page_cmp(const void *a, const void *b)
{
	const OldPage *x = a;
	const OldPage *y = b;

	return x->blkno != y->blkno ? pg_cmp_u32(x->blkno, y->blkno) :
		pg_cmp_s32(x->listed, y->listed);
}

/*
 * Fold a table without a key from the baseline: of each row on a listed
 * page, the copies the fold saw on the page's old version taken from the
 * baseline's count, those the snapshot sees on its version now added.
 * False if it must be read in full instead.
 */
static bool
fold_keyless(Finder *f, const ProllyTableFold *baseline, const State *st,
			 const Listed *l, BlockNumber low, BlockNumber nblocks,
			 ProllyNodeRef *data)
{
	Copies		cs = {0};
	BlockScratch *bs = palloc_aligned(sizeof(BlockScratch), PG_IO_ALIGN_SIZE, 0);
	OldPage    *pages = palloc_array(OldPage, Max(l->n, 1));
	int			npages = 0;
	ProllyEdit *edits;
	int			nedits = 0;
	ProllyTreeCursor *cur;
	bool		ok = true;
	int			p = 0;

	cs.cxt = AllocSetContextCreate(CurrentMemoryContext, "keyless copies",
								   ALLOCSET_DEFAULT_SIZES);
	initStringInfo(&cs.key);
	initStringInfo(&cs.row);
	bs->was.t = palloc_array(HeapTupleData, MaxHeapTuplesPerPage);
	bs->is.t = palloc_array(HeapTupleData, MaxHeapTuplesPerPage);
	bs->cxt = AllocSetContextCreate(CurrentMemoryContext, "keyless page",
									ALLOCSET_DEFAULT_SIZES);

	/* each listed page's first old version (the page as the fold saw it) */
	for (int i = 0; i < l->n; i++)
		if (l->changes[i].kind == BMAP_CHANGE_COPY)
		{
			pages[npages].blkno = l->changes[i].blkno;
			pages[npages].old_block = l->changes[i].old_block;
			pages[npages].listed = i;
			npages++;
		}

	/*
	 * And the pages whose tuples the fold carried, unsettled for it: one
	 * not listed is as the fold saw it, and whether it saw each tuple is
	 * told by its snapshot.  (One past the table's shortest length since:
	 * in full.)
	 */
	if (st->nblocks_carried > 0)
	{
		pages = repalloc_array(pages, OldPage, npages + st->nblocks_carried);
		for (uint32 i = 0; i < st->nblocks_carried; i++)
		{
			/*
			 * (one added to the table after the fold began held none of its
			 * rows: it is read again as a page past the length then)
			 */
			if (st->blocks_carried[i] >= st->fold.nblocks)
				continue;
			if (st->blocks_carried[i] >= Min(low, nblocks))
				return false;
			pages[npages].blkno = st->blocks_carried[i];
			pages[npages].old_block = InvalidBlockNumber;
			pages[npages].listed = l->n;	/* (after any listed) */
			npages++;
		}
	}
	if (npages > 1)
	{
		int			u = 1;

		qsort(pages, npages, sizeof(OldPage), old_page_cmp);
		for (int i = 1; i < npages; i++)
			if (pages[i].blkno != pages[u - 1].blkno)
				pages[u++] = pages[i];
		npages = u;
	}

	/*
	 * Those pages as they were, and as they are if the table still has
	 * them; then the pages past its shortest length since not listed, as
	 * they are.
	 */
	for (int i = 0; ok && i < npages; i++)
	{
		ok = BlockNumberIsValid(pages[i].old_block) ?
			block_copies(f, &cs, bs, pages[i].blkno, pages[i].old_block,
						 pages[i].blkno < nblocks) :
			unchanged_copies(f, &cs, bs, pages[i].blkno);
		CHECK_FOR_INTERRUPTS();
	}
	for (BlockNumber blk = low; ok && blk < nblocks; blk++)
	{
		while (p < npages && pages[p].blkno < blk)
			p++;
		if (p < npages && pages[p].blkno == blk)
			continue;
		ok = block_copies(f, &cs, bs, blk, InvalidBlockNumber, true);
		CHECK_FOR_INTERRUPTS();
	}
	if (!ok)
		return false;

	/* each row's count, the baseline's changed by its copies' */
	if (cs.n > 1)
		qsort_arg(cs.c, cs.n, sizeof(Copy), copy_cmp, f);
	edits = palloc_array(ProllyEdit, Max(cs.n, 1));
	cur = PROLLY_CALL(f->tree, cursor_open, &f->env, &baseline->data,
					  CurrentMemoryContext);
	for (int i = 0; i < cs.n && ok;)
	{
		Copy	   *c = &cs.c[i];
		ProllyKey	ck = {(const uint8 *) c->key, c->keylen};
		ProllyKey	k;
		ProllyValue v;
		int64		count = 0;
		int64		delta = 0;

		while (i < cs.n && same_bytes(cs.c[i].key, cs.c[i].keylen, ck.data, ck.len))
			delta += cs.c[i++].delta;

		/* (rows the keying can't tell apart, but the bytes can: in full) */
		if (i < cs.n)
		{
			ProllyKey	nk = {(const uint8 *) cs.c[i].key, cs.c[i].keylen};

			if (keying_compare(&ck, &nk, f) == 0)
				ok = false;
		}
		if (!ok || delta == 0)
			continue;
		f->work.candidates++;
		f->work.rows_read++;
		PROLLY_CALL(f->tree, cursor_seek, cur, &ck, ForwardScanDirection);
		if (PROLLY_CALL(f->tree, cursor_next, cur, &k, &v) &&
			keying_compare(&k, &ck, f) == 0)
		{
			ProllyValue row;
			uint64		had;

			if (!same_bytes(c->key, c->keylen, k.data, k.len))
			{
				ok = false;
				continue;
			}
			PROLLY_CALL(f->kr, parse_value, f->k, &v, &row, &had);
			count = (int64) had;
		}
		count += delta;

		/* (fewer copies than none: not what the fold saw, so in full) */
		if (count < 0)
			ok = false;
		else
		{
			ProllyEdit *e = &edits[nedits++];

			e->key = ck;
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
		}
		CHECK_FOR_INTERRUPTS();
	}
	PROLLY_CALL(f->tree, cursor_close, cur);
	f->work.rows_written += nedits;
	if (ok && nedits > 0)
		PROLLY_CALL(f->tree, apply, &f->env, &baseline->data, edits, nedits, data);
	else if (ok)
		*data = baseline->data;
	MemoryContextDelete(cs.cxt);
	return ok;
}

/*
 * Fold from the baseline, its state 'st': false if the table must be read
 * in full instead.  With 'carry', keys to look at again next time are
 * carried.
 */
static bool
fold_changed(Finder *f, const ProllyTableFold *baseline, const State *st,
			 bool carry, ProllyNodeRef *data)
{
	Listed		l = {0};
	BlockNumber low = st->fold.nblocks;
	BlockNumber nblocks;
	BlockNumber *current;
	int			ncurrent = 0;
	PGIOAlignedBlock old;
	ProllyEdit *edits;
	int			nedits = 0;
	ProllyTreeCursor *cur;
	StringInfoData key;
	StringInfoData row;
	StringInfoData value;
	IndexScanDesc scan;

	/*
	 * Rows the baseline's fold left unsettled without carrying their keys
	 * may be anywhere; a snapshot older than the fold's may not see all it
	 * saw.
	 */
	if (f->give_up)
		return false;
	if (st->flags & STATE_UNSETTLED)
	{
		f->reason = PROLLY_FULL_UNSETTLED;
		return false;
	}
	if (!sees_fold(f->snap, st))
	{
		f->reason = PROLLY_FULL_STALE_SNAPSHOT;
		return false;
	}
	if (!bmap_fold_changes(RelationGetSmgr(f->rel), &st->fold, listed_change, &l))
	{
		f->reason = PROLLY_FULL_STORAGE_UNSEEN;
		return false;
	}
	for (int i = 0; i < l.n; i++)
		if (l.changes[i].kind == BMAP_CHANGE_TRUNCATE)
			low = Min(low, l.changes[i].blkno);
	nblocks = RelationGetNumberOfBlocks(f->rel);

	/* a table without a key: unchanged, or its rows' copies counted again */
	if (f->keyless)
	{
		f->base = st;
		if (l.n == 0 && low >= nblocks && st->nblocks_carried == 0)
		{
			*data = baseline->data;
			return true;
		}
		return fold_keyless(f, baseline, st, &l, low, nblocks, data);
	}

	/* the pages listed, as they were and as they are */
	current = palloc_array(BlockNumber, Max(l.n, 1));
	for (int i = 0; i < l.n && !f->give_up; i++)
	{
		BmapChange *c = &l.changes[i];

		if (c->kind != BMAP_CHANGE_COPY)
			continue;
		bmap_read_old(RelationGetSmgr(f->rel), c->blkno, c->old_block, old.data);
		page_tuples(f, (Page) old.data, c->blkno, false, true);
		if (c->blkno < Min(low, nblocks))
			current[ncurrent++] = c->blkno;
	}
	if (ncurrent > 1)
		qsort(current, ncurrent, sizeof(BlockNumber), block_cmp);
	for (int i = 0; i < ncurrent && !f->give_up; i++)
		if (i == 0 || current[i] != current[i - 1])
			current_page(f, current[i], true);
	/* and those past the shortest length since, as they are */
	for (BlockNumber blk = low; blk < nblocks && !f->give_up; blk++)
		current_page(f, blk, true);
	if (f->give_up)
		return false;

	/* the keys carried from the last fold */
	if (st->carried.hash.len > 0)
	{
		ProllyKey	k;
		ProllyValue v;

		cur = PROLLY_CALL(f->tree, cursor_open, &f->memenv, &st->carried,
						  CurrentMemoryContext);
		PROLLY_CALL(f->tree, cursor_seek, cur, NULL, ForwardScanDirection);
		initStringInfo(&key);
		initStringInfo(&row);
		while (PROLLY_CALL(f->tree, cursor_next, cur, &k, &v))
		{
			resetStringInfo(&key);
			resetStringInfo(&row);
			appendBinaryStringInfo(&key, k.data, k.len);
			appendBinaryStringInfo(&row, v.data, v.len);
			add_candidate(&f->cands, &f->ncands, &f->maxcands, &key, &row);
		}
		PROLLY_CALL(f->tree, cursor_close, cur);
	}

	/* each candidate's old and new rows; the pairs that differ are edits */
	f->ncands = unique_candidates(f, f->cands, f->ncands);
	f->work.candidates += f->ncands;
	f->work.rows_read += f->ncands;
	edits = palloc_array(ProllyEdit, Max(f->ncands, 1));
	cur = PROLLY_CALL(f->tree, cursor_open, &f->env, &baseline->data,
					  CurrentMemoryContext);
	/* one scan, every version if keys are carried */
	scan = index_beginscan(f->rel, f->pk, false, carry ? SnapshotAny : f->snap,
						   NULL, f->nkeys, 0, SO_NONE);
	initStringInfo(&key);
	initStringInfo(&row);
	initStringInfo(&value);
	for (int i = 0; i < f->ncands; i++)
	{
		Candidate  *c = &f->cands[i];
		ProllyKey	ck = {(const uint8 *) c->key, c->keylen};
		ProllyKey	k;
		ProllyValue v;
		bool		had;
		bool		has;
		bool		unsettled;

		PROLLY_CALL(f->tree, cursor_seek, cur, &ck, ForwardScanDirection);
		had = PROLLY_CALL(f->tree, cursor_next, cur, &k, &v) &&
			keying_compare(&k, &ck, f) == 0;
		has = lookup(f, scan, c, carry, &key, &row, &unsettled);
		if (has)
		{
			ProllyValue r;

			resetStringInfo(&value);
			r.data = (const uint8 *) row.data;
			r.len = row.len;
			PROLLY_CALL(f->kr, make_value, f->k, &r, 1, &value);
		}
		if (had && has && v.len == value.len &&
			memcmp(v.data, value.data, v.len) == 0 && k.len == key.len &&
			memcmp(k.data, key.data, k.len) == 0)
			;					/* the same row */
		else if (has)
		{
			ProllyEdit *e = &edits[nedits++];
			char	   *kb = palloc(Max(key.len, 1));
			char	   *vb = palloc(Max(value.len, 1));

			memcpy(kb, key.data, key.len);
			memcpy(vb, value.data, value.len);
			e->key.data = (const uint8 *) kb;
			e->key.len = key.len;
			e->value.data = (const uint8 *) vb;
			e->value.len = value.len;
			e->is_delete = false;
		}
		else if (had)
		{
			ProllyEdit *e = &edits[nedits++];

			e->key.data = (const uint8 *) c->key;
			e->key.len = c->keylen;
			e->is_delete = true;
		}
		if (unsettled)
		{
			StringInfoData ks = {c->key, (int) c->keylen, (int) c->keylen, 0};
			StringInfoData rs = {c->keyrow, (int) c->keyrowlen, (int) c->keyrowlen, 0};

			add_candidate(&f->carried, &f->ncarried, &f->maxcarried, &ks, &rs);
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
	return true;
}


/*
 * Every row the snapshot sees, folded into a data tree: each row's key and
 * canonical encoding, in key order, identical rows of a keyless table
 * counted once.  With 'carry', the keys of tuples the snapshot doesn't see
 * as settled are carried too.
 */
static void
fold_all(Finder *f, ProllyNodeRef *data, bool carry)
{
	ProllyFullFold done;

	f->work.full++;
	prolly_fold_all(f->rel, f->snap, f->rowslot, f->kr, f->k, f->keyless,
					f->tree, &f->env, data, &done);
	f->work.rows_read += done.rows_read;
	f->work.rows_written += done.rows_written;

	/* the tuples of transactions the snapshot doesn't see as settled */
	if (carry)
	{
		BlockNumber nblocks = RelationGetNumberOfBlocks(f->rel);

		for (BlockNumber blk = 0; blk < nblocks; blk++)
		{
			current_page(f, blk, false);
			CHECK_FOR_INTERRUPTS();
		}
	}
}

static int
carried_cmp(const void *a, const void *b)
{
	const Candidate *x = a;
	const Candidate *y = b;
	ProllyKey	kx = {(const uint8 *) x->key, x->keylen};
	ProllyKey	ky = {(const uint8 *) y->key, y->keylen};

	return prolly_bytes_compare(&kx, &ky, NULL);
}

/* The carried keys as a tree (by bytes); an empty hash for none. */
static void
carried_tree(Finder *f, ProllyNodeRef *root)
{
	ProllyTreeBuilder *b;
	int			u = 0;

	memset(root, 0, sizeof(*root));
	if (f->ncarried == 0)
		return;
	qsort(f->carried, f->ncarried, sizeof(Candidate), carried_cmp);
	for (int i = 0; i < f->ncarried; i++)
		if (u == 0 || carried_cmp(&f->carried[u - 1], &f->carried[i]) != 0)
			f->carried[u++] = f->carried[i];
	b = PROLLY_CALL(f->tree, build_begin, &f->memenv, CurrentMemoryContext);
	for (int i = 0; i < u; i++)
	{
		ProllyKey	k = {(const uint8 *) f->carried[i].key, f->carried[i].keylen};
		ProllyValue v = {(const uint8 *) f->carried[i].keyrow, f->carried[i].keyrowlen};

		PROLLY_CALL(f->tree, build_add, b, &k, &v);
	}
	PROLLY_CALL(f->tree, build_finish, b, root);
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
static bool
own_copies(Finder *f, Copies *cs, ItemPointerData *tids, int ntids)
{
	bool		ok = true;

	if (ntids > 1)
		qsort(tids, ntids, sizeof(ItemPointerData), tid_cmp);
	for (int i = 0; i < ntids && ok; i++)
	{
		BlockNumber blkno = ItemPointerGetBlockNumber(&tids[i]);
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
		buf = ReadBufferExtended(f->rel, MAIN_FORKNUM, blkno, RBM_NORMAL, NULL);
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
			ok = add_copy(f, cs, copy, made ? 1 : -1, false);
			heap_freetuple(copy);
		}
		CHECK_FOR_INTERRUPTS();
	}
	return ok;
}

static bool
pages_fold_rows(Relation rel, const ProllyTreeEnv *env,
				const ProllyNodeRef *data_in, ItemPointerData *tids, int ntids,
				ProllyNodeRef *data)
{
	Finder		f;
	Copies		cs = {0};
	ProllyEdit *edits;
	int			nedits = 0;
	ProllyTreeCursor *cur;
	bool		ok;
	MemoryContext cxt;
	MemoryContext old;

	cxt = AllocSetContextCreate(CurrentMemoryContext, "own rows",
								ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cxt);
	finder_open(&f, rel, env);
	cs.cxt = cxt;
	initStringInfo(&cs.key);
	initStringInfo(&cs.row);
	ok = own_copies(&f, &cs, tids, ntids);
	if (ok && cs.n > 1)
		qsort_arg(cs.c, cs.n, sizeof(Copy), copy_cmp, &f);

	/*
	 * A key's copies: with a key, the row it has now if the transaction
	 * made one (it replaced the rest), else none; without, each row's
	 * copies counted (a row's key is its content).
	 */
	edits = palloc_array(ProllyEdit, Max(cs.n, 1));
	cur = PROLLY_CALL(f.tree, cursor_open, &f.env, data_in, CurrentMemoryContext);
	for (int i = 0; ok && i < cs.n;)
	{
		Copy	   *c = &cs.c[i];
		Copy	   *made = NULL;
		int64		delta = 0;
		ProllyKey	ck = {(const uint8 *) c->key, c->keylen};
		ProllyEdit *e;
		ProllyKey	hk;
		ProllyValue hv;
		bool		at_head;

		while (i < cs.n && same_bytes(cs.c[i].key, cs.c[i].keylen, ck.data, ck.len))
		{
			if (cs.c[i].delta > 0)
				made = &cs.c[i];
			delta += cs.c[i++].delta;
		}

		/*
		 * (keys the keying can't tell apart but the bytes can, among the
		 * changes or against the head: the working set instead, as a fold
		 * from the baseline would)
		 */
		if (i < cs.n)
		{
			ProllyKey	nk = {(const uint8 *) cs.c[i].key, cs.c[i].keylen};

			if (keying_compare(&ck, &nk, &f) == 0)
			{
				ok = false;
				break;
			}
		}
		PROLLY_CALL(f.tree, cursor_seek, cur, &ck, ForwardScanDirection);
		at_head = PROLLY_CALL(f.tree, cursor_next, cur, &hk, &hv) &&
			keying_compare(&hk, &ck, &f) == 0;
		if (at_head && !same_bytes(c->key, c->keylen, hk.data, hk.len))
		{
			ok = false;
			break;
		}
		e = &edits[nedits];
		e->key = ck;
		if (!f.keyless)
		{
			e->is_delete = made == NULL;
			if (made)
			{
				ProllyValue r = {(const uint8 *) made->row, made->rowlen};
				StringInfoData value;

				initStringInfo(&value);
				PROLLY_CALL(f.kr, make_value, f.k, &r, 1, &value);
				e->value.data = (const uint8 *) value.data;
				e->value.len = value.len;
			}
			nedits++;
		}
		else if (delta != 0)
		{
			int64		count = 0;

			if (at_head)
			{
				ProllyValue row;
				uint64		had;

				PROLLY_CALL(f.kr, parse_value, f.k, &hv, &row, &had);
				count = (int64) had;
			}
			count += delta;
			if (count < 0)
				ok = false;
			else
			{
				e->is_delete = count == 0;
				if (count > 0)
				{
					ProllyValue r = {(const uint8 *) c->row, c->rowlen};
					StringInfoData value;

					initStringInfo(&value);
					PROLLY_CALL(f.kr, make_value, f.k, &r, (uint64) count, &value);
					e->value.data = (const uint8 *) value.data;
					e->value.len = value.len;
				}
				nedits++;
			}
		}
	}
	PROLLY_CALL(f.tree, cursor_close, cur);
	if (ok && nedits > 0)
		PROLLY_CALL(f.tree, apply, &f.env, data_in, edits, nedits, data);
	else if (ok)
		*data = *data_in;
	finder_close(&f);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
	return ok;
}

/* ----------------------------------------------------------------
 * Callbacks
 * ----------------------------------------------------------------
 */

static void
pages_fold(Relation rel, const ProllyTreeEnv *env,
		   const ProllyTableFold *baseline, ProllyFullFoldReason no_baseline,
		   ProllyNodeRef *data, StringInfo next)
{
	Finder		f;
	State		st;
	BmapFold	fold;
	bool		have;
	bool		mapped;
	bool		locked;
	LOCKTAG		tag;
	LOCKMODE	mode = next ? ExclusiveLock : ShareLock;
	MemoryContext cxt;
	MemoryContext old;

	/* what a table's fold makes is let go of when it's done */
	cxt = AllocSetContextCreate(CurrentMemoryContext, "pages fold",
								ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cxt);
	finder_open(&f, rel, env);
	mapped = RelationGetSmgr(rel)->smgr_which == SMGR_BMAP &&
		!RecoveryInProgress();
	have = mapped && baseline != NULL && parse_state(&baseline->state, &st);
	if (debug_prolly_force_full_fold)
		f.reason = PROLLY_FULL_FORCED;
	else if (!mapped)
		f.reason = RecoveryInProgress() ? PROLLY_FULL_RECOVERY : PROLLY_FULL_NO_LOG;
	else if (baseline == NULL)
		f.reason = no_baseline;
	else if (!have)
		f.reason = PROLLY_FULL_LOG_CHANGED;
	have = have && f.reason == PROLLY_FULL_NONE;

	/*
	 * A fold's beginning lets go of what changed before the baseline, which
	 * readers of it don't need; it waits for those reading the blocks.
	 */
	locked = mapped && (next != NULL || have);
	if (locked)
	{
		SET_LOCKTAG_OBJECT(tag, MyDatabaseId, rel->rd_locator.spcOid,
						   rel->rd_locator.relNumber, FOLD_LOCK_SUBID);
		(void) LockAcquire(&tag, mode, false, false);
	}
	PG_TRY();
	{
		if (next && mapped)
		{
			if (have)
				bmap_fold_prune(RelationGetSmgr(rel), &st.fold);
			bmap_fold_begin(RelationGetSmgr(rel), &fold);
			INJECTION_POINT("prolly-fold-begun", NULL);
		}
		if (!have || !fold_changed(&f, baseline, &st, next != NULL, data))
		{
			instr_time	start;
			instr_time	took;

			/* (what the fold from the baseline read is thrown away) */
			memset(&f.work, 0, sizeof(f.work));
			f.ncarried = 0;
			f.ncblocks = 0;
			f.unsettled = false;
			INSTR_TIME_SET_CURRENT(start);
			fold_all(&f, data, next != NULL);
			INSTR_TIME_SET_CURRENT(took);
			INSTR_TIME_SUBTRACT(took, start);
			/* (giving up otherwise: what changed costs more than reading it all) */
			prolly_full_fold_noted(rel, f.reason == PROLLY_FULL_NONE ?
								   PROLLY_FULL_TOO_MANY : f.reason,
								   (int64) f.work.rows_read,
								   INSTR_TIME_GET_MILLISEC(took));
		}
		counters.pages += f.work.pages;
		counters.candidates += f.work.candidates;
		counters.full += f.work.full;
		counters.rows_read += f.work.rows_read;
		counters.rows_written += f.work.rows_written;
		pgstat_count_prolly(PGSTAT_PROLLY_FOLD_PAGES, f.work.pages);
		pgstat_count_prolly(PGSTAT_PROLLY_FOLD_CANDIDATES, f.work.candidates);
		pgstat_count_prolly(PGSTAT_PROLLY_FOLD_FULL, f.work.full);
		pgstat_count_prolly(PGSTAT_PROLLY_FOLD_ROWS_READ, f.work.rows_read);
		pgstat_count_prolly(PGSTAT_PROLLY_FOLD_ROWS_WRITTEN, f.work.rows_written);
		if (next)
		{
			State		ns = {0};

			if (mapped)
				ns.fold = fold;
			ns.xmax = f.snap->xmax;
			ns.nxip = f.snap->xcnt;
			ns.xip = palloc_array(TransactionId, Max(ns.nxip, 1));
			if (ns.nxip > 0)
				memcpy(ns.xip, f.snap->xip, ns.nxip * sizeof(TransactionId));
			qsort(ns.xip, ns.nxip, sizeof(TransactionId), xid_cmp);
			ns.flags = f.unsettled ? STATE_UNSETTLED : 0;
			ns.own = GetTopTransactionIdIfAny();
			carried_tree(&f, &ns.carried);
			if (f.ncblocks > 1)
			{
				int			u = 1;

				qsort(f.cblocks, f.ncblocks, sizeof(BlockNumber), block_cmp);
				for (int i = 1; i < f.ncblocks; i++)
					if (f.cblocks[i] != f.cblocks[u - 1])
						f.cblocks[u++] = f.cblocks[i];
				f.ncblocks = u;
			}

			/*
			 * (more pages carried than reading the table in full would cost
			 * much more than: the table is read in full next time)
			 */
			if (f.ncblocks > CARRIED_BLOCKS_MIN &&
				f.ncblocks > RelationGetNumberOfBlocks(rel) / 8)
			{
				ns.flags |= STATE_UNSETTLED;
				f.ncblocks = 0;
			}
			ns.nblocks_carried = f.ncblocks;
			ns.blocks_carried = f.cblocks;
			MemoryContextSwitchTo(old);
			put_state(next, &ns);
			MemoryContextSwitchTo(cxt);
		}
	}
	PG_FINALLY();
	{
		if (locked)
			LockRelease(&tag, mode, false);
	}
	PG_END_TRY();
	finder_close(&f);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
}

static void
pages_walk(const ProllyTreeEnv *env, const ProllyValue *state,
		   ProllyTreeVisitNode visit, void *arg)
{
	State		st;
	ProllyTreeEnv memenv = *env;

	if (!parse_state(state, &st) || st.carried.hash.len == 0)
		return;
	memenv.compare = prolly_bytes_compare;
	PROLLY_CALL(prolly_tree_lookup("cow"), walk, &memenv, &st.carried, visit, arg);
}

/* A state's tree of carried keys. */
static void
pages_state_refs(const ProllyValue *state, StringInfo refs)
{
	State		st;

	memset(&st, 0, sizeof(st));
	if (parse_state(state, &st) && st.carried.hash.len > 0)
		PROLLY_CALL(prolly_tree_lookup("cow"), value_reference, &st.carried.hash,
					0, refs);
	/* (once per baseline item as it is built: what parsing made, freed) */
	if (st.xip)
		pfree(st.xip);
	if (st.blocks_carried)
		pfree(st.blocks_carried);
}

static void
pages_counters(ProllyChangeCounters *out, bool reset)
{
	*out = counters;
	if (reset)
		memset(&counters, 0, sizeof(counters));
}

const ProllyChangeFinderRoutine prolly_changes_pages = {
	.info = {
		.iface = PROLLY_IFACE_CHANGE_FINDER,
		.impl_id = 1,
		.format_version = STATE_VERSION,
		.name = "pages",
		.description = "rows on the pages a block-mapped table lists as "
		"changed since its fold",
	},
	.fold = pages_fold,
	.walk = pages_walk,
	.counters = pages_counters,
	.fold_rows = pages_fold_rows,
	.state_refs = pages_state_refs,
};
