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
 * to be candidates again.  (A table without a primary key notes only that
 * it has some.)
 *
 * A table is folded in full instead, every row read, when the baseline
 * isn't of its changes list's lineage (it was never folded, or rewritten
 * since), when it has no primary key (its rows are keyed by content, and
 * the count of a row's copies can't be known from some of them) and
 * anything changed, and when a candidate's key column is stored out of
 * line (its old version may be gone).
 *
 * A baseline's state: a version byte, the list's lineage, the fold's epoch,
 * the table's length then, flags, and the tree of carried keys (each key
 * with the key columns' encoded row, to look it up by; an empty hash for
 * none).
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
#include "access/prolly_builtins.h"
#include "access/relscan.h"
#include "access/stratnum.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/pg_am_d.h"
#include "common/int.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "storage/bmap.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/smgr.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#define STATE_VERSION		1
#define STATE_UNSETTLED		0x01	/* rows the fold didn't see as settled */

/* a lock per relation file number: folds exclusive, readers shared */
#define FOLD_LOCK_CLASSID	((Oid) 0x50464C44)

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
	bool		unsettled;		/* a keyless table's: some rows weren't */
	bool		give_up;
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
put_state(StringInfo out, const BmapFold *fold, uint8 flags,
		  const ProllyNodeRef *carried)
{
	appendStringInfoChar(out, (char) STATE_VERSION);
	put_u64(out, fold->lineage);
	put_u32(out, fold->epoch);
	put_u32(out, fold->nblocks);
	appendStringInfoChar(out, (char) flags);
	appendStringInfoChar(out, (char) carried->hash.len);
	appendBinaryStringInfo(out, carried->hash.bytes, carried->hash.len);
}

static bool
parse_state(const ProllyValue *state, BmapFold *fold, uint8 *flags,
			ProllyNodeRef *carried)
{
	const uint8 *p = state->data;
	uint8		hlen;

	if (state->len < 1 + 8 + 4 + 4 + 1 + 1 || p[0] != STATE_VERSION)
		return false;
	fold->lineage = get_u64(p + 1);
	fold->epoch = get_u32(p + 9);
	fold->nblocks = get_u32(p + 13);
	*flags = p[17];
	hlen = p[18];
	if (hlen > PROLLY_HASH_MAX_LEN || state->len != 19 + hlen)
		return false;
	memset(carried, 0, sizeof(*carried));
	carried->hash.len = hlen;
	memcpy(carried->hash.bytes, p + 19, hlen);
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

static int
memcmp_keys(const ProllyKey *a, const ProllyKey *b, void *arg)
{
	int			r = memcmp(a->data, b->data, Min(a->len, b->len));

	return r != 0 ? r : (a->len > b->len) - (a->len < b->len);
}

static void
finder_open(Finder *f, Relation rel, const ProllyTreeEnv *env)
{
	memset(f, 0, sizeof(*f));
	f->rel = rel;
	f->snap = GetActiveSnapshot();
	f->tree = prolly_tree_lookup("cow");
	f->kr = (const ProllyKeyingRoutine *)
		prolly_get_impl(PROLLY_IFACE_TABLE_KEYING, "pk_or_content");
	/*
	 * Rows as a rewrite leaves them: a column added with a default is in
	 * every row (the scan gives its missing value), so whether the table was
	 * rewritten since doesn't change the encoding.
	 */
	f->k = PROLLY_CALL(f->kr, open_rewritten, rel, CurrentMemoryContext);
	f->keyless = PROLLY_CALL(f->kr, keyless, f->k);
	f->env = *env;
	f->env.compare = keying_compare;
	f->env.compare_arg = f;
	f->memenv = *env;
	f->memenv.compare = memcmp_keys;
	f->memenv.compare_arg = NULL;
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

/* Is xid's transaction one the snapshot doesn't see as committed or not? */
static bool
xid_unsettled(Finder *f, TransactionId xid)
{
	if (!TransactionIdIsNormal(xid))
		return false;
	return TransactionIdIsCurrentTransactionId(xid) ||
		XidInMVCCSnapshot(xid, f->snap);
}

static bool
tuple_unsettled(Finder *f, HeapTupleHeader t)
{
	if (!HeapTupleHeaderXminFrozen(t) &&
		xid_unsettled(f, HeapTupleHeaderGetRawXmin(t)))
		return true;
	if (!(t->t_infomask & HEAP_XMAX_INVALID) &&
		!HEAP_XMAX_IS_LOCKED_ONLY(t->t_infomask))
		return xid_unsettled(f, HeapTupleHeaderGetUpdateXid(t));
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

		if (!f->isnull[a] && TupleDescAttr(f->desc, a)->attlen == -1 &&
			VARATT_IS_EXTERNAL_ONDISK(DatumGetPointer(f->values[a])))
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

/*
 * The candidates on a version of page blkno: every tuple, visible or not.
 * Of the version the table has now ('current'), keys of tuples the
 * snapshot doesn't see as settled are carried.
 */
static void
page_candidates(Finder *f, Page page, BlockNumber blkno, bool current)
{
	OffsetNumber maxoff;
	StringInfoData key;
	StringInfoData keyrow;

	counters.pages++;
	if (PageIsNew(page))
		return;
	initStringInfo(&key);
	initStringInfo(&keyrow);
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
		if (f->keyless)
		{
			if (current && tuple_unsettled(f, tup.t_data))
				f->unsettled = true;
			continue;
		}
		if (!tuple_key(f, &tup, &key, &keyrow))
		{
			f->give_up = true;
			break;
		}
		add_candidate(&f->cands, &f->ncands, &f->maxcands, &key, &keyrow);
		if (current && tuple_unsettled(f, tup.t_data))
			add_candidate(&f->carried, &f->ncarried, &f->maxcarried, &key, &keyrow);
	}
	pfree(key.data);
	pfree(keyrow.data);
}

/* The current version of page blkno, read under a share lock. */
static void
current_page(Finder *f, BlockNumber blkno)
{
	Buffer		buf = ReadBufferExtended(f->rel, MAIN_FORKNUM, blkno,
										 RBM_NORMAL, NULL);

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page_candidates(f, BufferGetPage(buf), blkno, true);
	UnlockReleaseBuffer(buf);
}

/*
 * Look a candidate up by its key columns with 'snap', into f->rowslot;
 * with SnapshotAny, every version is visited and true returned if one is
 * unsettled.
 */
static bool
lookup(Finder *f, const Candidate *c, Snapshot snap)
{
	ProllyValue kr = {(const uint8 *) c->keyrow, c->keyrowlen};
	ScanKeyData skey[INDEX_MAX_KEYS];
	IndexScanDesc scan;
	bool		found = false;

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
	scan = index_beginscan(f->rel, f->pk, false, snap, NULL, f->nkeys, 0,
						   SO_NONE);
	index_rescan(scan, skey, f->nkeys, NULL, 0);
	while (table_index_getnext_slot(scan, ForwardScanDirection, f->rowslot))
	{
		if (snap != SnapshotAny)
		{
			found = true;
			break;
		}
		if (tuple_unsettled(f, ((HeapTupleTableSlot *) f->rowslot)->tuple->t_data))
		{
			found = true;
			break;
		}
	}
	index_endscan(scan);
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

/*
 * Fold from the baseline: false if the table must be read in full instead.
 */
static bool
fold_changed(Finder *f, const ProllyTableFold *baseline, const BmapFold *since,
			 uint8 flags, const ProllyNodeRef *carried, ProllyNodeRef *data)
{
	Listed		l = {0};
	BlockNumber low = since->nblocks;
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

	if (f->give_up ||
		!bmap_fold_changes(RelationGetSmgr(f->rel), since, listed_change, &l))
		return false;
	for (int i = 0; i < l.n; i++)
		if (l.changes[i].kind == BMAP_CHANGE_TRUNCATE)
			low = Min(low, l.changes[i].blkno);
	nblocks = RelationGetNumberOfBlocks(f->rel);

	/* a table without a key: unchanged, or read in full */
	if (f->keyless)
	{
		if (l.n > 0 || low < nblocks || (flags & STATE_UNSETTLED))
			return false;
		*data = baseline->data;
		return true;
	}

	/* the pages listed, as they were and as they are */
	current = palloc_array(BlockNumber, Max(l.n, 1));
	for (int i = 0; i < l.n && !f->give_up; i++)
	{
		BmapChange *c = &l.changes[i];

		if (c->kind != BMAP_CHANGE_COPY)
			continue;
		bmap_read_old(RelationGetSmgr(f->rel), c->blkno, c->old_block, old.data);
		page_candidates(f, (Page) old.data, c->blkno, false);
		if (c->blkno < Min(low, nblocks))
			current[ncurrent++] = c->blkno;
	}
	if (ncurrent > 1)
		qsort(current, ncurrent, sizeof(BlockNumber), block_cmp);
	for (int i = 0; i < ncurrent && !f->give_up; i++)
		if (i == 0 || current[i] != current[i - 1])
			current_page(f, current[i]);
	/* and those past the shortest length since, as they are */
	for (BlockNumber blk = low; blk < nblocks && !f->give_up; blk++)
		current_page(f, blk);
	if (f->give_up)
		return false;

	/* the keys carried from the last fold */
	if (carried->hash.len > 0)
	{
		ProllyKey	k;
		ProllyValue v;

		cur = PROLLY_CALL(f->tree, cursor_open, &f->memenv, carried,
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
	counters.candidates += f->ncands;
	edits = palloc_array(ProllyEdit, Max(f->ncands, 1));
	cur = PROLLY_CALL(f->tree, cursor_open, &f->env, &baseline->data,
					  CurrentMemoryContext);
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

		PROLLY_CALL(f->tree, cursor_seek, cur, &ck, ForwardScanDirection);
		had = PROLLY_CALL(f->tree, cursor_next, cur, &k, &v) &&
			keying_compare(&k, &ck, f) == 0;
		has = lookup(f, c, f->snap);
		if (has)
		{
			ProllyHash	content;
			ProllyValue r;

			resetStringInfo(&key);
			resetStringInfo(&row);
			resetStringInfo(&value);
			PROLLY_CALL(f->kr, encode_row, f->k, f->rowslot, &key, &row, &content);
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
		if (lookup(f, c, SnapshotAny))
		{
			StringInfoData ks = {c->key, (int) c->keylen, (int) c->keylen, 0};
			StringInfoData rs = {c->keyrow, (int) c->keyrowlen, (int) c->keyrowlen, 0};

			add_candidate(&f->carried, &f->ncarried, &f->maxcarried, &ks, &rs);
		}
		CHECK_FOR_INTERRUPTS();
	}
	PROLLY_CALL(f->tree, cursor_close, cur);
	if (nedits > 0)
		PROLLY_CALL(f->tree, apply, &f->env, &baseline->data, edits, nedits, data);
	else
		*data = baseline->data;
	return true;
}

typedef struct FoldRow
{
	StringInfoData key;
	StringInfoData row;
} FoldRow;

static int
fold_row_cmp(const void *a, const void *b, void *arg)
{
	const FoldRow *ra = *(FoldRow *const *) a;
	const FoldRow *rb = *(FoldRow *const *) b;
	ProllyKey	ka = {(const uint8 *) ra->key.data, ra->key.len};
	ProllyKey	kb = {(const uint8 *) rb->key.data, rb->key.len};
	int			r = keying_compare(&ka, &kb, arg);

	/* equal keys of a keyless table: the same row; order by bytes anyway */
	if (r == 0)
		r = memcmp(ra->key.data, rb->key.data, Min(ra->key.len, rb->key.len));
	return r != 0 ? r : (ra->key.len > rb->key.len) - (ra->key.len < rb->key.len);
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
	TableScanDesc scan;
	FoldRow   **rows = NULL;
	int			nrows = 0;
	int			maxrows = 0;
	ProllyTreeBuilder *b;

	counters.full++;
	scan = table_beginscan(f->rel, f->snap, 0, NULL, SO_NONE);
	while (table_scan_getnextslot(scan, ForwardScanDirection, f->rowslot))
	{
		FoldRow    *r = palloc(sizeof(FoldRow));
		ProllyHash	content;

		initStringInfo(&r->key);
		initStringInfo(&r->row);
		PROLLY_CALL(f->kr, encode_row, f->k, f->rowslot, &r->key, &r->row, &content);
		if (nrows == maxrows)
		{
			maxrows = Max(64, maxrows * 2);
			rows = rows ? repalloc_array(rows, FoldRow *, maxrows) :
				palloc_array(FoldRow *, maxrows);
		}
		rows[nrows++] = r;
		CHECK_FOR_INTERRUPTS();
	}
	table_endscan(scan);

	if (nrows > 1)
		qsort_arg(rows, nrows, sizeof(FoldRow *), fold_row_cmp, f);

	b = PROLLY_CALL(f->tree, build_begin, &f->env, CurrentMemoryContext);
	for (int i = 0; i < nrows;)
	{
		int			j = i + 1;
		ProllyKey	k = {(const uint8 *) rows[i]->key.data, rows[i]->key.len};
		ProllyValue row = {(const uint8 *) rows[i]->row.data, rows[i]->row.len};
		StringInfoData value;
		ProllyValue v;

		while (j < nrows && rows[j]->key.len == rows[i]->key.len &&
			   memcmp(rows[j]->key.data, rows[i]->key.data, k.len) == 0)
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
		i = j;
	}
	PROLLY_CALL(f->tree, build_finish, b, data);

	/* the tuples of transactions the snapshot doesn't see as settled */
	if (carry)
	{
		BlockNumber nblocks = RelationGetNumberOfBlocks(f->rel);

		f->give_up = false;
		for (BlockNumber blk = 0; blk < nblocks; blk++)
		{
			Buffer		buf = ReadBufferExtended(f->rel, MAIN_FORKNUM, blk,
												 RBM_NORMAL, NULL);
			Page		page;
			OffsetNumber maxoff;
			StringInfoData key;
			StringInfoData keyrow;

			LockBuffer(buf, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buf);
			initStringInfo(&key);
			initStringInfo(&keyrow);
			maxoff = PageIsNew(page) ? InvalidOffsetNumber : PageGetMaxOffsetNumber(page);
			for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
			{
				ItemId		lp = PageGetItemId(page, off);
				HeapTupleData tup;

				if (!ItemIdIsNormal(lp))
					continue;
				tup.t_data = (HeapTupleHeader) PageGetItem(page, lp);
				tup.t_len = ItemIdGetLength(lp);
				tup.t_tableOid = RelationGetRelid(f->rel);
				ItemPointerSet(&tup.t_self, blk, off);
				if (!tuple_unsettled(f, tup.t_data))
					continue;
				/* (an unreadable key: the next fold reads everything too) */
				if (f->keyless || !tuple_key(f, &tup, &key, &keyrow))
					f->unsettled = true;
				else
					add_candidate(&f->carried, &f->ncarried, &f->maxcarried,
								  &key, &keyrow);
			}
			UnlockReleaseBuffer(buf);
			pfree(key.data);
			pfree(keyrow.data);
			CHECK_FOR_INTERRUPTS();
		}
	}
}

static int
carried_cmp(const void *a, const void *b)
{
	const Candidate *x = a;
	const Candidate *y = b;
	int			r = memcmp(x->key, y->key, Min(x->keylen, y->keylen));

	return r != 0 ? r : (x->keylen > y->keylen) - (x->keylen < y->keylen);
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
 * Callbacks
 * ----------------------------------------------------------------
 */

static void
pages_fold(Relation rel, const ProllyTreeEnv *env,
		   const ProllyTableFold *baseline, ProllyNodeRef *data,
		   StringInfo next)
{
	Finder		f;
	BmapFold	since;
	BmapFold	fold;
	uint8		flags = 0;
	ProllyNodeRef carried;
	bool		have;
	bool		mapped;
	LOCKTAG		tag;
	LOCKMODE	mode = next ? ExclusiveLock : ShareLock;

	finder_open(&f, rel, env);
	mapped = RelationGetSmgr(rel)->smgr_which == SMGR_BMAP;
	have = mapped && baseline != NULL &&
		parse_state(&baseline->state, &since, &flags, &carried);

	/*
	 * A fold's beginning lets go of what changed before the baseline, which
	 * readers of it don't need; it waits for those reading the blocks.
	 */
	SET_LOCKTAG_OBJECT(tag, MyDatabaseId, FOLD_LOCK_CLASSID,
					   rel->rd_locator.relNumber, 0);
	(void) LockAcquire(&tag, mode, false, false);
	PG_TRY();
	{
		if (next && mapped)
		{
			if (have)
				bmap_fold_prune(RelationGetSmgr(rel), &since);
			bmap_fold_begin(RelationGetSmgr(rel), &fold);
		}
		if (!have || !fold_changed(&f, baseline, &since, flags, &carried, data))
		{
			f.ncarried = 0;
			f.unsettled = false;
			fold_all(&f, data, next != NULL);
		}
		if (next)
		{
			ProllyNodeRef ct;

			if (!mapped)
				memset(&fold, 0, sizeof(fold));
			carried_tree(&f, &ct);
			put_state(next, &fold, f.unsettled ? STATE_UNSETTLED : 0, &ct);
		}
	}
	PG_FINALLY();
	{
		LockRelease(&tag, mode, false);
	}
	PG_END_TRY();
	finder_close(&f);
}

static void
pages_walk(const ProllyTreeEnv *env, const ProllyValue *state,
		   ProllyTreeVisitNode visit, void *arg)
{
	BmapFold	fold;
	uint8		flags;
	ProllyNodeRef carried;
	ProllyTreeEnv memenv = *env;

	if (!parse_state(state, &fold, &flags, &carried) || carried.hash.len == 0)
		return;
	memenv.compare = memcmp_keys;
	PROLLY_CALL(prolly_tree_lookup("cow"), walk, &memenv, &carried, visit, arg);
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
};
