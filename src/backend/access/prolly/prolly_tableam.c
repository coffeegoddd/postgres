/*-------------------------------------------------------------------------
 *
 * prolly_tableam.c
 *	  Table access method adapter for prolly engines.
 *
 * One TableAmRoutine serves every prolly table AM (prolly_d1, prolly_d2,
 * ...); the AM a relation uses selects its components through
 * prolly_config.h.  This file translates table AM calls into calls on the
 * prolly interfaces and holds no engine logic.
 *
 * Rows are tree items made by the table keying (prolly_keying.h): a scan
 * walks the table's data tree through an engine cursor and decodes each
 * item, a keyless table's item once per identical row; an insert encodes
 * the row, writes its item, and gets the row's TID from row identity
 * (prolly_rowid.h).  Slots are virtual: rows have no heap tuple header.
 *
 * Every data tree records the description of the keying it was written
 * with, in its META tree.  Scans read with the recorded keying.  The first
 * write to a table in a (sub)transaction compares it with the table's
 * keying now, and if its rows no longer read correctly (a primary key was
 * added or dropped, Postgres rewrote the table through one without a key,
 * or a column was dropped without a rewrite), rebuilds the tree first.
 * TIDs don't depend on keying, so indexes stay valid.
 *
 * Callbacks not built yet raise ERRCODE_FEATURE_NOT_SUPPORTED naming the
 * table AM, the engine and the callback.  Parallel scans are left out
 * (NULL), so the planner never plans one: workers couldn't see the
 * leader's private writes.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_tableam.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "access/multixact.h"
#include "access/prolly_config.h"
#include "access/prolly_indexam.h"
#include "access/prolly_tableam.h"
#include "access/relation.h"
#include "access/relscan.h"
#include "access/skey.h"
#include "access/tableam.h"
#include "access/tableam_indexscan.h"
#include "access/tsmapi.h"
#include "common/pg_prng.h"
#include "nodes/tidbitmap.h"
#include "pgstat.h"
#include "catalog/index.h"
#include "catalog/storage.h"
#include "commands/defrem.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "storage/proc.h"
#include "utils/datum.h"
#include "utils/fmgrprotos.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"
#include "utils/tuplesort.h"

pg_noreturn static void
not_implemented(Relation rel, const char *callback)
{
	ProllyComponents components;

	prolly_components_for_relation(rel, &components);
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("table access method \"%s\" does not implement %s yet",
					get_am_name(rel->rd_rel->relam), callback),
			 errdetail("The table uses engine \"%s\".",
					   components.engine->info.name)));
	pg_unreachable();
}

/* ------------------------------------------------------------------------
 * Slots and scans
 * ------------------------------------------------------------------------
 */

/* Rebuilds of tables in another keying by this backend (rekey()). */
static uint64 prolly_rekeys = 0;

typedef struct ProllyScanDescData
{
	TableScanDescData base;
	ProllyComponents c;
	MemoryContext mcxt;
	ProllyRootSet *roots;
	ProllyCursor *cursor;
	ProllyKeying *keying;
	ProllyRowIdScan *rowid;
	CommandId	cid;

	/*
	 * The current item: its key and row, how many identical rows it holds,
	 * and which of them the scan last returned.  The cursor moves the way
	 * the scan last did (cursor_dir); have_item is false before the first
	 * item and after the scan ran off an end.
	 */
	StringInfoData key;
	StringInfoData row;
	uint64		count;
	uint64		ordinal;
	bool		have_item;
	ScanDirection cursor_dir;

	/* TID range scans: rows whose TIDs are in [mintid, maxtid]. */
	ItemPointerData mintid;
	ItemPointerData maxtid;

	/*
	 * Sample scans: pseudo-blocks of SAMPLE_BLOCK_ITEMS items in key order,
	 * and the rows of the current one.
	 */
	bool		sample_started;
	bool		sample_by_position; /* blocks found by position, else read */
	BlockNumber sample_nblocks;
	BlockNumber sample_block;
	BlockNumber sample_cursor_block;	/* read: the block the cursor is at */
	int			sample_nrows;
	int			sample_chunk;	/* first row of the chunk offsets name */
	int			sample_cap;
	StringInfoData *sample_rows;
	ItemPointerData *sample_tids;

	/* Bitmap scans: the current page's offsets, and which is next. */
	uint64		bm_rekeys;		/* prolly_rekeys when keying was opened */
	bool		bm_have_page;
	TBMIterateResult bm_page;
	OffsetNumber bm_offsets[TBM_MAX_TUPLES_PER_PAGE];
	int			bm_noffsets;
	int			bm_next;
	struct RowAt *bm_row;
} ProllyScanDescData;

typedef ProllyScanDescData *ProllyScanDesc;

/*
 * Rows have no tuple header, so slots are virtual slots that find what
 * Postgres asks of a header through row identity, when it asks: xmin (the
 * transaction that wrote the version) and whether that is the current
 * transaction.  A live version has no xmax.
 */
typedef struct ProllyTupleTableSlot
{
	VirtualTupleTableSlot base;
	bool		have_xmin;
	TransactionId xmin;
} ProllyTupleTableSlot;

static void
tts_prolly_init(TupleTableSlot *slot)
{
	TTSOpsVirtual.init(slot);
	((ProllyTupleTableSlot *) slot)->have_xmin = false;
}

static void
tts_prolly_clear(TupleTableSlot *slot)
{
	TTSOpsVirtual.clear(slot);
	((ProllyTupleTableSlot *) slot)->have_xmin = false;
}

static void tts_prolly_copyslot(TupleTableSlot *dstslot, TupleTableSlot *srcslot);

static TransactionId
slot_xmin(TupleTableSlot *slot)
{
	ProllyTupleTableSlot *pslot = (ProllyTupleTableSlot *) slot;

	if (!pslot->have_xmin)
	{
		Relation	rel = RelationIdGetRelation(slot->tts_tableOid);
		ProllyComponents c;
		ProllyRootSet *roots;
		StringInfoData key;
		ProllyHash	content;
		uint64		ordinal;
		ProllyTidInfo info;

		if (!RelationIsValid(rel))
			elog(ERROR, "could not open relation with OID %u", slot->tts_tableOid);
		prolly_components_for_relation(rel, &c);
		roots = PROLLY_CALL(c.engine, roots_acquire, NULL);
		initStringInfo(&key);
		if (PROLLY_CALL(c.row_identity, tid_resolve, roots, rel, &slot->tts_tid,
						InvalidCommandId, &key, &content, &ordinal,
						&info) == PROLLY_TID_UNKNOWN)
			info.xmin = InvalidTransactionId;
		pfree(key.data);
		PROLLY_CALL(c.engine, roots_release, roots);
		RelationClose(rel);
		pslot->xmin = info.xmin;
		pslot->have_xmin = true;
	}
	return pslot->xmin;
}

static Datum
tts_prolly_getsysattr(TupleTableSlot *slot, int attnum, bool *isnull)
{
	*isnull = false;
	switch (attnum)
	{
		case MinTransactionIdAttributeNumber:
			return TransactionIdGetDatum(slot_xmin(slot));
		case MaxTransactionIdAttributeNumber:
			return TransactionIdGetDatum(InvalidTransactionId);
		case MinCommandIdAttributeNumber:
		case MaxCommandIdAttributeNumber:
			return CommandIdGetDatum(FirstCommandId);
	}
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot retrieve a system column in this context")));
	return 0;					/* keep compiler quiet */
}

static bool
tts_prolly_is_current_xact_tuple(TupleTableSlot *slot)
{
	return TransactionIdIsCurrentTransactionId(slot_xmin(slot));
}

static void
tts_prolly_release(TupleTableSlot *slot)
{
	TTSOpsVirtual.release(slot);
}

static void
tts_prolly_getsomeattrs(TupleTableSlot *slot, int natts)
{
	TTSOpsVirtual.getsomeattrs(slot, natts);
}

static void
tts_prolly_materialize(TupleTableSlot *slot)
{
	TTSOpsVirtual.materialize(slot);
}

static HeapTuple
tts_prolly_copy_heap_tuple(TupleTableSlot *slot)
{
	return TTSOpsVirtual.copy_heap_tuple(slot);
}

static MinimalTuple
tts_prolly_copy_minimal_tuple(TupleTableSlot *slot, Size extra)
{
	return TTSOpsVirtual.copy_minimal_tuple(slot, extra);
}

static const TupleTableSlotOps TTSOpsProlly = {
	.base_slot_size = sizeof(ProllyTupleTableSlot),
	.init = tts_prolly_init,
	.release = tts_prolly_release,
	.clear = tts_prolly_clear,
	.getsomeattrs = tts_prolly_getsomeattrs,
	.getsysattr = tts_prolly_getsysattr,
	.is_current_xact_tuple = tts_prolly_is_current_xact_tuple,
	.materialize = tts_prolly_materialize,
	.copyslot = tts_prolly_copyslot,
	.get_heap_tuple = NULL,
	.get_minimal_tuple = NULL,
	.copy_heap_tuple = tts_prolly_copy_heap_tuple,
	.copy_minimal_tuple = tts_prolly_copy_minimal_tuple,
};

static void
tts_prolly_copyslot(TupleTableSlot *dstslot, TupleTableSlot *srcslot)
{
	ProllyTupleTableSlot *dst = (ProllyTupleTableSlot *) dstslot;

	TTSOpsVirtual.copyslot(dstslot, srcslot);
	dst->have_xmin = false;
	if (srcslot->tts_ops == &TTSOpsProlly &&
		((ProllyTupleTableSlot *) srcslot)->have_xmin)
	{
		dst->have_xmin = true;
		dst->xmin = ((ProllyTupleTableSlot *) srcslot)->xmin;
	}
}

static const TupleTableSlotOps *
prolly_slot_callbacks(Relation rel)
{
	return &TTSOpsProlly;
}

/*
 * The command whose view a snapshot gives: all writes, if not MVCC.
 *
 * A dirty snapshot also reports to its caller which transactions it saw
 * in progress, so callers can wait for them.  Other transactions' writes
 * are private until they commit, so there never are any.
 */
static CommandId
snapshot_cid(Snapshot snapshot)
{
	if (snapshot != NULL && snapshot->snapshot_type == SNAPSHOT_DIRTY)
	{
		snapshot->xmin = InvalidTransactionId;
		snapshot->xmax = InvalidTransactionId;
		snapshot->speculativeToken = 0;
	}
	if (snapshot != NULL && IsMVCCSnapshot(snapshot))
		return snapshot->curcid;
	return InvalidCommandId;
}

/* The keying description recorded for rel's data tree, if any. */
static bool
recorded_keying(const ProllyComponents *c, ProllyRootSet *roots, Relation rel,
				StringInfo desc)
{
	ProllyKey	key = {(const uint8 *) PROLLY_META_KEYING, PROLLY_META_KEYING_LEN};

	return PROLLY_CALL(c->engine, get, roots, rel, PROLLY_TREE_META, &key,
					   InvalidCommandId, desc);
}

/* The keying to read rel's data tree with: the recorded one, or rel's. */
static ProllyKeying *
open_read_keying(const ProllyComponents *c, ProllyRootSet *roots, Relation rel,
				 MemoryContext mcxt)
{
	StringInfoData desc;
	ProllyValue d;
	ProllyKeying *k;
	bool		recorded;

	initStringInfo(&desc);
	recorded = recorded_keying(c, roots, rel, &desc);
	d.data = (const uint8 *) desc.data;
	d.len = desc.len;
	k = PROLLY_CALL(c->keying, open, rel, recorded ? &d : NULL, mcxt);
	pfree(desc.data);
	return k;
}

static void
scan_start(ProllyScanDesc scan)
{
	scan->count = 0;
	scan->ordinal = 0;
	scan->have_item = false;
	scan->cursor_dir = ForwardScanDirection;
	PROLLY_CALL(scan->c.engine, cursor_seek, scan->cursor, NULL,
				ForwardScanDirection);
}

/*
 * Move to the next item in direction: the cursor goes on if it already
 * moves that way; else it seeks back to the current item (or the end the
 * scan ran off) in the new direction and steps past it.
 */
static bool
scan_next_item(ProllyScanDesc scan, ScanDirection direction)
{
	const ProllyEngineRoutine *engine = scan->c.engine;
	ProllyKey	key;
	ProllyValue value;
	ProllyValue row;
	MemoryContext old;

	if (direction != scan->cursor_dir)
	{
		if (scan->have_item)
		{
			ProllyKey	cur = {(const uint8 *) scan->key.data, scan->key.len};

			PROLLY_CALL(engine, cursor_seek, scan->cursor, &cur, direction);
			if (PROLLY_CALL(engine, cursor_next, scan->cursor, &key, &value) &&
				(key.len != cur.len || memcmp(key.data, cur.data, key.len) != 0))
			{
				/* The current item is gone: this is the next one. */
				scan->cursor_dir = direction;
				goto found;
			}
		}
		else
			PROLLY_CALL(engine, cursor_seek, scan->cursor, NULL, direction);
		scan->cursor_dir = direction;
	}
	if (!PROLLY_CALL(engine, cursor_next, scan->cursor, &key, &value))
	{
		scan->have_item = false;
		return false;
	}

found:
	old = MemoryContextSwitchTo(scan->mcxt);
	PROLLY_CALL(scan->c.keying, parse_value, scan->keying, &value, &row,
				&scan->count);
	resetStringInfo(&scan->key);
	appendBinaryStringInfo(&scan->key, (const char *) key.data, key.len);
	resetStringInfo(&scan->row);
	appendBinaryStringInfo(&scan->row, (const char *) row.data, row.len);
	MemoryContextSwitchTo(old);
	scan->have_item = true;
	return true;
}

static void serializable_read(Relation rel, const ProllyComponents *c,
							  Snapshot snapshot);

static TableScanDesc
prolly_scan_begin(Relation rel, Snapshot snapshot, int nkeys,
				  ScanKeyData *key, ParallelTableScanDesc pscan, uint32 flags)
{
	ProllyScanDesc scan;
	MemoryContext old;

	if (pscan != NULL)
		not_implemented(rel, "parallel scans");

	scan = palloc0(sizeof(ProllyScanDescData));
	scan->base.rs_rd = rel;
	scan->base.rs_snapshot = snapshot;
	scan->base.rs_nkeys = nkeys;
	scan->base.rs_key = key;
	scan->base.rs_flags = flags;
	scan->base.rs_parallel = NULL;

	prolly_components_for_relation(rel, &scan->c);
	scan->mcxt = AllocSetContextCreate(CurrentMemoryContext, "prolly scan",
									   ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(scan->mcxt);
	scan->cid = snapshot_cid(snapshot);
	scan->roots = PROLLY_CALL(scan->c.engine, roots_acquire, snapshot);
	serializable_read(rel, &scan->c, snapshot);
	scan->keying = open_read_keying(&scan->c, scan->roots, rel, scan->mcxt);
	scan->bm_rekeys = prolly_rekeys;
	scan->cursor = PROLLY_CALL(scan->c.engine, cursor_open, scan->roots, rel,
							   PROLLY_TREE_DATA, scan->cid);
	scan->rowid = PROLLY_CALL(scan->c.row_identity, scan_begin, scan->roots,
							  rel, scan->cid);
	initStringInfo(&scan->key);
	initStringInfo(&scan->row);
	MemoryContextSwitchTo(old);

	scan_start(scan);
	return &scan->base;
}

static void
prolly_scan_end(TableScanDesc sscan)
{
	ProllyScanDesc scan = (ProllyScanDesc) sscan;

	PROLLY_CALL(scan->c.row_identity, scan_end, scan->rowid);
	PROLLY_CALL(scan->c.engine, cursor_close, scan->cursor);
	PROLLY_CALL(scan->c.keying, close, scan->keying);
	PROLLY_CALL(scan->c.engine, roots_release, scan->roots);
	if (scan->base.rs_flags & SO_TEMP_SNAPSHOT)
		UnregisterSnapshot(scan->base.rs_snapshot);
	MemoryContextDelete(scan->mcxt);
	pfree(scan);
}

static void
prolly_scan_rescan(TableScanDesc sscan, ScanKeyData *key, bool set_params,
				   bool allow_strat, bool allow_sync, bool allow_pagemode)
{
	ProllyScanDesc scan = (ProllyScanDesc) sscan;

	if (key && sscan->rs_nkeys > 0)
		memcpy(sscan->rs_key, key, sizeof(ScanKeyData) * sscan->rs_nkeys);
	scan->sample_started = false;
	scan->bm_have_page = false;
	scan_start(scan);
}

static bool
prolly_scan_getnextslot(TableScanDesc sscan, ScanDirection direction,
						TupleTableSlot *slot)
{
	ProllyScanDesc scan = (ProllyScanDesc) sscan;
	const ProllyKeyingRoutine *keying = scan->c.keying;
	ProllyValue row;
	bool		backward = ScanDirectionIsBackward(direction);

	if (!backward)
		direction = ForwardScanDirection;

	/* The next of the current item's identical rows, else the next item. */
	if (scan->have_item &&
		(backward ? scan->ordinal > 0 : scan->ordinal + 1 < scan->count))
		scan->ordinal += backward ? -1 : 1;
	else if (scan_next_item(scan, direction))
		scan->ordinal = backward ? scan->count - 1 : 0;
	else
	{
		ExecClearTuple(slot);
		return false;
	}

	row.data = (const uint8 *) scan->row.data;
	row.len = scan->row.len;
	PROLLY_CALL(keying, decode_row, scan->keying, &row, slot);
	{
		ProllyKey	k = {(const uint8 *) scan->key.data, scan->key.len};

		if (!PROLLY_CALL(scan->c.row_identity, tid_find, scan->rowid, &k,
						 scan->ordinal, &slot->tts_tid))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("a row of \"%s\" has no TID",
							RelationGetRelationName(sscan->rs_rd))));
	}
	slot->tts_tableOid = RelationGetRelid(sscan->rs_rd);
	return true;
}

/*
 * TIDs follow row content, not storage order, so a TID range scan reads
 * every row and keeps those whose TIDs are in the range.
 */
static void
prolly_scan_set_tidrange(TableScanDesc sscan, ItemPointer mintid,
						 ItemPointer maxtid)
{
	ProllyScanDesc scan = (ProllyScanDesc) sscan;

	scan->mintid = *mintid;
	scan->maxtid = *maxtid;
	scan_start(scan);
}

static bool
prolly_scan_getnextslot_tidrange(TableScanDesc sscan, ScanDirection direction,
								 TupleTableSlot *slot)
{
	ProllyScanDesc scan = (ProllyScanDesc) sscan;

	while (prolly_scan_getnextslot(sscan, direction, slot))
	{
		if (ItemPointerCompare(&slot->tts_tid, &scan->mintid) >= 0 &&
			ItemPointerCompare(&slot->tts_tid, &scan->maxtid) <= 0)
			return true;
	}
	return false;
}

/* ------------------------------------------------------------------------
 * Row modification
 * ------------------------------------------------------------------------
 */

/*
 * The keying of rel for inserts, cached per relation and reopened after a
 * relcache invalidation.  An invalidated entry is only marked; it is
 * closed at the next lookup, so a keying in use is never freed under its
 * caller.
 */
typedef struct InsertKeying
{
	Oid			relid;			/* hash key */
	RelFileNumber relnumber;
	bool		valid;
	MemoryContext mcxt;
	ProllyKeying *keying;
	const ProllyKeyingRoutine *routine;
	StringInfoData desc;		/* keying's description */

	/* The (sub)transaction that last checked the recorded keying. */
	LocalTransactionId checked_lxid;
	SubTransactionId checked_subxid;
} InsertKeying;

static HTAB *insert_keyings = NULL;

static void
insert_keying_invalidate(Datum arg, Oid relid)
{
	HASH_SEQ_STATUS status;
	InsertKeying *ik;

	if (insert_keyings == NULL)
		return;
	if (OidIsValid(relid))
	{
		ik = hash_search(insert_keyings, &relid, HASH_FIND, NULL);
		if (ik)
			ik->valid = false;
		return;
	}
	hash_seq_init(&status, insert_keyings);
	while ((ik = hash_seq_search(&status)) != NULL)
		ik->valid = false;
}

static InsertKeying *
insert_keying(Relation rel, const ProllyComponents *c)
{
	Oid			relid = RelationGetRelid(rel);
	InsertKeying *ik;
	bool		found;
	MemoryContext mcxt;
	ProllyKeying *keying;
	StringInfoData desc;

	if (insert_keyings == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(InsertKeying);
		ctl.hcxt = CacheMemoryContext;
		insert_keyings = hash_create("prolly insert keyings", 64, &ctl,
									 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		CacheRegisterRelcacheCallback(insert_keying_invalidate, (Datum) 0);
	}
	ik = hash_search(insert_keyings, &relid, HASH_FIND, &found);
	if (found && ik->valid && ik->relnumber == rel->rd_locator.relNumber)
		return ik;

	/*
	 * Open the new keying before touching the entry, so an error leaves
	 * either the old entry or none; its memory moves under the cache only
	 * once it's complete.
	 */
	mcxt = AllocSetContextCreate(CurrentMemoryContext, "prolly insert keying",
								 ALLOCSET_SMALL_SIZES);
	keying = PROLLY_CALL(c->keying, open, rel, NULL, mcxt);
	{
		MemoryContext old = MemoryContextSwitchTo(mcxt);

		initStringInfo(&desc);
		PROLLY_CALL(c->keying, describe, keying, &desc);
		MemoryContextSwitchTo(old);
	}
	MemoryContextSetParent(mcxt, CacheMemoryContext);
	if (found)
	{
		PROLLY_CALL(ik->routine, close, ik->keying);
		MemoryContextDelete(ik->mcxt);
	}
	else
		ik = hash_search(insert_keyings, &relid, HASH_ENTER, NULL);
	ik->mcxt = mcxt;
	ik->routine = c->keying;
	ik->keying = keying;
	ik->desc = desc;
	ik->relnumber = rel->rd_locator.relNumber;
	ik->valid = true;
	ik->checked_lxid = InvalidLocalTransactionId;
	ik->checked_subxid = InvalidSubTransactionId;
	return ik;
}

/*
 * SERIALIZABLE: record a read of rel with predicate.c, and the read/write
 * conflicts with the transactions whose writes to rel the snapshot doesn't
 * see (committed since it, or still running), as heap finds them per tuple
 * from xmin and xmax.  Reads are recorded for the whole relation.
 */
static void
serializable_read(Relation rel, const ProllyComponents *c, Snapshot snapshot)
{
	TransactionId *xids;
	TransactionId running_buf[64];
	TransactionId *running = running_buf;
	int			n;

	if (snapshot == NULL || snapshot->snapshot_type != SNAPSHOT_MVCC ||
		!CheckForSerializableConflictOutNeeded(rel, snapshot))
		return;
	PROLLY_CALL(c->coord, predicate_lock_range, rel, snapshot, NULL, NULL);
	n = PROLLY_CALL(c->engine, unseen_writers, rel, snapshot, &xids);
	for (int i = 0; i < n; i++)
		CheckForSerializableConflictOut(rel, xids[i], snapshot);
	pfree(xids);
	n = PROLLY_CALL(c->coord, current_writers, rel, running_buf,
					lengthof(running_buf));
	if (n > lengthof(running_buf))
	{
		running = palloc(sizeof(TransactionId) * n);
		n = PROLLY_CALL(c->coord, current_writers, rel, running, n);
	}
	for (int i = 0; i < n; i++)
		CheckForSerializableConflictOut(rel, running[i], snapshot);
	if (running != running_buf)
		pfree(running);
}

/*
 * The key a row is locked by: its bytes, if they name it (keys equal by
 * the keying are the same bytes); else NULL, which locks all of rel.
 */
static const ProllyKey *
lock_key(Relation rel, const ProllyComponents *c, const ProllyKey *key)
{
	if (key == NULL || !PROLLY_CALL(c->keying, equal_bytes,
									insert_keying(rel, c)->keying))
		return NULL;
	return key;
}

/*
 * Get the right to write key of rel (NULL: all of rel) in mode before
 * writing it, from the coordination module.  Getting it may have meant
 * waiting for another writer to commit, so *roots is replaced by the
 * newest: whatever the write reads (an existing row, a count) must be what
 * it builds on.
 */
static void
lock_for_write_mode(Relation rel, const ProllyComponents *c,
					const ProllyKey *key, LockTupleMode mode,
					ProllyRootSet **roots)
{
	TM_FailureData tmfd;

	/* SERIALIZABLE readers of rel have a read/write conflict with this. */
	CheckForSerializableConflictIn(rel, NULL, InvalidBlockNumber);
	if (PROLLY_CALL(c->coord, lock_row, rel, lock_key(rel, c, key), mode,
					LockWaitBlock, &tmfd) != TM_Ok)
		elog(ERROR, "could not lock a row of \"%s\" for writing",
			 RelationGetRelationName(rel));
	{
		/* The new one lives where the old one did, for the caller to free. */
		MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(*roots));

		PROLLY_CALL(c->engine, roots_release, *roots);
		*roots = PROLLY_CALL(c->engine, roots_acquire, NULL);
		MemoryContextSwitchTo(old);
	}
}

static void
lock_for_write(Relation rel, const ProllyComponents *c, const ProllyKey *key,
			   ProllyRootSet **roots)
{
	lock_for_write_mode(rel, c, key, LockTupleExclusive, roots);
}

static void
newest_roots(const ProllyComponents *c, ProllyRootSet **roots)
{
	MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(*roots));

	PROLLY_CALL(c->engine, roots_release, *roots);
	*roots = PROLLY_CALL(c->engine, roots_acquire, NULL);
	MemoryContextSwitchTo(old);
}

static bool has_rows(Relation rel, const ProllyComponents *c,
					 ProllyRootSet *roots);
static void insert_row(Relation rel, const ProllyComponents *c,
					   ProllyKeying *keying, ProllyRootSet **roots,
					   TupleTableSlot *slot, uint64 copies,
					   const ItemPointerData *renamed, bool replaces,
					   CommandId cid);

/*
 * Rebuild rel's data tree, written with the keying old describes, under
 * rel's keying now, and its local metadata by row (kept in the data tree's
 * order) with it.  Rows are collected first: they are read from the
 * tree being replaced.  The rebuilt rows are tagged InvalidCommandId, so
 * every command, this one included, sees the table's rows as before.  A
 * row whose bytes changed (a dropped column's value is gone) keeps its
 * TID.
 */
static void
rekey(Relation rel, const ProllyComponents *c, ProllyRootSet **roots,
	  ProllyKeying *old, ProllyKeying *now)
{
	MemoryContext tmp = AllocSetContextCreate(CurrentMemoryContext,
											  "prolly rekey", ALLOCSET_DEFAULT_SIZES);
	MemoryContext oldcxt = MemoryContextSwitchTo(tmp);
	ProllyCursor *cur;
	ProllyKey	key;
	ProllyValue value;
	List	   *rows = NIL;
	TupleTableSlot *slot = MakeSingleTupleTableSlot(RelationGetDescr(rel),
													&TTSOpsVirtual);
	ListCell   *lr;
	ProllyRowIdScan *rowid;
	typedef struct RekeyRow
	{
		StringInfoData row;
		uint64		count;
		ItemPointerData *tids;	/* one per identical row */
	} RekeyRow;

	/*
	 * Scans that read rows one by one (index and bitmap scans) read the
	 * rebuilt rows from now on: they open the keying again.
	 */
	prolly_rekeys++;

	rowid = PROLLY_CALL(c->row_identity, scan_begin, *roots, rel,
						InvalidCommandId);
	cur = PROLLY_CALL(c->engine, cursor_open, *roots, rel, PROLLY_TREE_DATA,
					  InvalidCommandId);
	PROLLY_CALL(c->engine, cursor_seek, cur, NULL, ForwardScanDirection);
	while (PROLLY_CALL(c->engine, cursor_next, cur, &key, &value))
	{
		ProllyValue row;
		RekeyRow   *r = palloc(sizeof(RekeyRow));

		PROLLY_CALL(c->keying, parse_value, old, &value, &row, &r->count);
		initStringInfo(&r->row);
		appendBinaryStringInfo(&r->row, (const char *) row.data, row.len);
		r->tids = palloc(sizeof(ItemPointerData) * r->count);
		for (uint64 i = 0; i < r->count; i++)
		{
			if (!PROLLY_CALL(c->row_identity, tid_find, rowid, &key, i,
							 &r->tids[i]))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("a row of \"%s\" has no TID",
								RelationGetRelationName(rel))));
		}
		rows = lappend(rows, r);
	}
	PROLLY_CALL(c->engine, cursor_close, cur);
	PROLLY_CALL(c->row_identity, scan_end, rowid);

	PROLLY_CALL(c->engine, tree_reset, rel, PROLLY_TREE_DATA);
	PROLLY_CALL(c->engine, tree_reset, rel, PROLLY_TREE_ROWKEY);
	foreach(lr, rows)
	{
		RekeyRow   *r = lfirst(lr);
		ProllyValue row = {(const uint8 *) r->row.data, r->row.len};

		PROLLY_CALL(c->keying, decode_row, old, &row, slot);
		insert_row(rel, c, now, roots, slot, r->count, r->tids, false,
				   InvalidCommandId);
	}


	ExecDropSingleTupleTableSlot(slot);
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(tmp);
}

/*
 * Before the first write to rel in this (sub)transaction: record its
 * keying if its tree has none, or rebuild the tree if the keying changed.
 */
static void
check_keying(Relation rel, const ProllyComponents *c, ProllyRootSet **roots,
			 InsertKeying *ik)
{
	StringInfoData recorded;
	StringInfoData now;
	ProllyKey	key = {(const uint8 *) PROLLY_META_KEYING, PROLLY_META_KEYING_LEN};
	bool		rebuilt = false;

	if (ik->checked_lxid == MyProc->vxid.lxid &&
		ik->checked_subxid == GetCurrentSubTransactionId())
		return;

	initStringInfo(&recorded);
	initStringInfo(&now);
	PROLLY_CALL(c->keying, describe, ik->keying, &now);
	if ((!recorded_keying(c, *roots, rel, &recorded) ||
		 recorded.len != now.len ||
		 memcmp(recorded.data, now.data, now.len) != 0) &&
		has_rows(rel, c, *roots))
	{
		/*
		 * Writing the whole tree: lock it, and look again once locked.  A
		 * tree with no rows has nothing to rebuild, and writers that find
		 * it so all record the same keying, so they needn't wait for each
		 * other (a new table's first writers).
		 */
		lock_for_write(rel, c, NULL, roots);
		resetStringInfo(&recorded);
		if (recorded_keying(c, *roots, rel, &recorded) &&
			recorded.len == now.len &&
			memcmp(recorded.data, now.data, now.len) == 0)
			goto done;
	}
	if (recorded.len != now.len ||
		memcmp(recorded.data, now.data, now.len) != 0)
	{
		ProllyValue v = {(const uint8 *) now.data, now.len};
		ProllyValue r = {(const uint8 *) recorded.data, recorded.len};

		/*
		 * Rows that still read correctly (columns were only added) stay as
		 * they are; others are rebuilt.  Either way the tree is recorded as
		 * written now.
		 */
		if (recorded.len > 0 && !PROLLY_CALL(c->keying, can_read, ik->keying, &r) &&
			has_rows(rel, c, *roots))
		{
			ProllyValue d = {(const uint8 *) recorded.data, recorded.len};
			ProllyKeying *old = PROLLY_CALL(c->keying, open, rel, &d,
											CurrentMemoryContext);

			rekey(rel, c, roots, old, ik->keying);
			PROLLY_CALL(c->keying, close, old);
			rebuilt = true;
		}
		PROLLY_CALL(c->engine, put, rel, PROLLY_TREE_META, &key, &v,
					InvalidCommandId);

		/*
		 * A rebuild changed every row key, so the prolly indexes are
		 * rebuilt, reading the table as it is now recorded.
		 */
		if (rebuilt)
			prolly_index_rebuild_all(rel);
	}
done:
	pfree(recorded.data);
	pfree(now.data);
	ik->checked_lxid = MyProc->vxid.lxid;
	ik->checked_subxid = GetCurrentSubTransactionId();
}

/*
 * The key, in a table's META tree, of the keying description its rows were
 * last made canonical under (prolly_canonicalize_table).
 */
#define META_CANONICAL		"canonical"
#define META_CANONICAL_LEN	9

void
prolly_canonicalize_table(Relation rel)
{
	ProllyComponents c;
	InsertKeying *ik;
	ProllyRootSet *roots;
	StringInfoData now;
	StringInfoData mark;
	ProllyKey	key = {(const uint8 *) META_CANONICAL, META_CANONICAL_LEN};
	ProllyValue v;
	bool		missing = false;
	TupleDesc	desc = RelationGetDescr(rel);

	prolly_components_for_relation(rel, &c);
	ik = insert_keying(rel, &c);
	roots = PROLLY_CALL(c.engine, roots_acquire, NULL);
	initStringInfo(&now);
	initStringInfo(&mark);
	PROLLY_CALL(c.keying, describe, ik->keying, &now);
	if (PROLLY_CALL(c.engine, get, roots, rel, PROLLY_TREE_META, &key,
					InvalidCommandId, &mark) &&
		mark.len == now.len && memcmp(mark.data, now.data, now.len) == 0)
	{
		PROLLY_CALL(c.engine, roots_release, roots);
		return;
	}

	/*
	 * Rows written before a column was dropped are rebuilt by the keying
	 * check (they don't read as the table is now); rows written before a
	 * column was added with a default lack it, though they read with it,
	 * so they are written again with it.
	 */
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (!att->attisdropped && att->atthasmissing)
			missing = true;
	}
	if (has_rows(rel, &c, roots))
	{
		lock_for_write(rel, &c, NULL, &roots);
		check_keying(rel, &c, &roots, ik);
		if (missing)
			rekey(rel, &c, &roots, ik->keying, ik->keying);
	}
	v.data = (const uint8 *) now.data;
	v.len = now.len;
	PROLLY_CALL(c.engine, put, rel, PROLLY_TREE_META, &key, &v, InvalidCommandId);
	PROLLY_CALL(c.engine, roots_release, roots);
	pfree(now.data);
	pfree(mark.data);
}

/* Does rel's data tree hold any row, as roots and this transaction see it? */
static bool
has_rows(Relation rel, const ProllyComponents *c, ProllyRootSet *roots)
{
	ProllyCursor *cur = PROLLY_CALL(c->engine, cursor_open, roots, rel,
									PROLLY_TREE_DATA, InvalidCommandId);
	ProllyKey	key;
	ProllyValue value;
	bool		any;

	PROLLY_CALL(c->engine, cursor_seek, cur, NULL, ForwardScanDirection);
	any = PROLLY_CALL(c->engine, cursor_next, cur, &key, &value);
	PROLLY_CALL(c->engine, cursor_close, cur);
	return any;
}

/*
 * A table keyed by its primary key holds one row per key, so a second row
 * with a key is refused here, as the primary key's index would refuse it
 * (it can't, as the row it would find was replaced).
 */
static void
report_duplicate_key(Relation rel, TupleTableSlot *slot)
{
	Oid			pkoid = RelationGetPrimaryKeyIndex(rel, false);
	Relation	idx;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	char	   *desc;
	int			nkeys;

	if (!OidIsValid(pkoid))
		elog(ERROR, "a row of \"%s\" has the key of another, but it has no primary key",
			 RelationGetRelationName(rel));
	idx = index_open(pkoid, AccessShareLock);
	nkeys = IndexRelationGetNumberOfKeyAttributes(idx);
	slot_getallattrs(slot);
	for (int i = 0; i < nkeys; i++)
	{
		AttrNumber	att = idx->rd_index->indkey.values[i];

		values[i] = slot->tts_values[att - 1];
		isnull[i] = slot->tts_isnull[att - 1];
	}
	desc = BuildIndexValueDescription(idx, values, isnull);
	ereport(ERROR,
			(errcode(ERRCODE_UNIQUE_VIOLATION),
			 errmsg("duplicate key value violates unique constraint \"%s\"",
					RelationGetRelationName(idx)),
			 desc ? errdetail("Key %s already exists.", desc) : 0,
			 errtableconstraint(rel, RelationGetRelationName(idx))));
}

/*
 * Write copies identical rows of slot's row, and set slot's TID to the
 * last one's.  renamed, if set, holds the TIDs the rows had before they
 * were rebuilt (re-encoded or re-keyed): they keep them.  replaces says
 * the row takes the place of the row at its key (an update keeping its
 * key).
 */
static void
insert_row(Relation rel, const ProllyComponents *c, ProllyKeying *keying,
		   ProllyRootSet **roots, TupleTableSlot *slot, uint64 copies,
		   const ItemPointerData *renamed, bool replaces, CommandId cid)
{
	StringInfoData key;
	StringInfoData rowbuf;
	StringInfoData value;
	ProllyHash	content;
	ProllyKey	k;
	ProllyValue row;
	ProllyValue v;
	uint64		count = 0;

	initStringInfo(&key);
	initStringInfo(&rowbuf);
	initStringInfo(&value);
	PROLLY_CALL(c->keying, encode_row, keying, slot, &key, &rowbuf, &content);
	k.data = (const uint8 *) key.data;
	k.len = key.len;
	row.data = (const uint8 *) rowbuf.data;
	row.len = rowbuf.len;

	/*
	 * An update keeping its key holds the row's lock already, in the mode
	 * it needs (FOR NO KEY UPDATE if no key column changed): asking for
	 * that mode finds it held.
	 */
	lock_for_write_mode(rel, c, &k,
						replaces ? LockTupleNoKeyExclusive : LockTupleExclusive,
						roots);
	prolly_lock_unique_values(rel, slot);

	if (PROLLY_CALL(c->keying, keyless, keying))
	{
		StringInfoData old;

		/* More of these rows; see this command's own inserts too. */
		initStringInfo(&old);
		if (PROLLY_CALL(c->engine, get, *roots, rel, PROLLY_TREE_DATA, &k,
						InvalidCommandId, &old))
		{
			ProllyValue ov = {(const uint8 *) old.data, old.len};
			ProllyValue orow;

			PROLLY_CALL(c->keying, parse_value, keying, &ov, &orow, &count);
		}
		pfree(old.data);
	}
	else if (copies != 1)
		ereport(ERROR,
				(errcode(ERRCODE_UNIQUE_VIOLATION),
				 errmsg("table \"%s\" holds identical rows, so it can't be keyed by its primary key",
						RelationGetRelationName(rel))));
	else if (!renamed && !replaces)
	{
		StringInfoData old;
		bool		taken;

		initStringInfo(&old);
		taken = PROLLY_CALL(c->engine, get, *roots, rel, PROLLY_TREE_DATA, &k,
							InvalidCommandId, &old);
		pfree(old.data);
		if (taken)
			report_duplicate_key(rel, slot);
	}
	PROLLY_CALL(c->keying, make_value, keying, &row, count + copies, &value);
	v.data = (const uint8 *) value.data;
	v.len = value.len;
	PROLLY_CALL(c->engine, put, rel, PROLLY_TREE_DATA, &k, &v, cid);

	/* The new rows are ordinals count .. count + copies - 1. */
	for (uint64 i = 0; i < copies; i++)
	{
		if (renamed)
		{
			slot->tts_tid = renamed[i];
			PROLLY_CALL(c->row_identity, tid_rename, *roots, rel, &slot->tts_tid,
						&content, count + i, &k, cid);
		}
		else
			PROLLY_CALL(c->row_identity, tid_assign, *roots, rel, &content,
						count + i, &k, cid, &slot->tts_tid);
	}
	slot->tts_tableOid = RelationGetRelid(rel);

	pfree(key.data);
	pfree(rowbuf.data);
	pfree(value.data);
}

static void
insert_one(Relation rel, const ProllyComponents *c, ProllyRootSet **roots,
		   TupleTableSlot *slot, CommandId cid)
{
	InsertKeying *ik = insert_keying(rel, c);

	check_keying(rel, c, roots, ik);
	insert_row(rel, c, ik->keying, roots, slot, 1, NULL, false, cid);
}

static void
prolly_tuple_insert(Relation rel, TupleTableSlot *slot, CommandId cid,
					uint32 options, BulkInsertStateData *bistate)
{
	ProllyComponents c;
	ProllyRootSet *roots;

	prolly_components_for_relation(rel, &c);
	roots = PROLLY_CALL(c.engine, roots_acquire, NULL);
	insert_one(rel, &c, &roots, slot, cid);
	PROLLY_CALL(c.engine, roots_release, roots);
}

static void
prolly_multi_insert(Relation rel, TupleTableSlot **slots, int nslots,
					CommandId cid, uint32 options,
					BulkInsertStateData *bistate)
{
	ProllyComponents c;
	ProllyRootSet *roots;

	prolly_components_for_relation(rel, &c);
	roots = PROLLY_CALL(c.engine, roots_acquire, NULL);
	for (int i = 0; i < nslots; i++)
		insert_one(rel, &c, &roots, slots[i], cid);
	PROLLY_CALL(c.engine, roots_release, roots);
}

static void
prolly_finish_bulk_insert(Relation rel, uint32 options)
{
	/* Writes stay private until commit; nothing to flush. */
}

/* ------------------------------------------------------------------------
 * Rows by TID
 *
 * A TID names a row version through row identity.  The version is in the
 * table if the data tree holds its key with the same content (for a
 * keyless table, enough identical rows for its ordinal).  Visibility is
 * by command: a command sees the transaction's writes from earlier
 * commands (InvalidCommandId: all of them; FirstCommandId: none, which is
 * what other transactions committed), and every snapshot reads the newest
 * published head.
 *
 * Updating, deleting or locking a row first gets the right to write or
 * lock it from coordination, then looks at it as the command sees it and
 * with every write: present in both, it's live; only in the first, this
 * command (or a later one, from a trigger) changed it; in neither, an
 * earlier command did, or another transaction that committed while this
 * one waited (then the committed state has the TID retired, with a
 * successor if it was updated, which READ COMMITTED follows).
 * ------------------------------------------------------------------------
 */

/* A row version found by TID. */
typedef struct RowAt
{
	ProllyTidState state;
	ProllyTidInfo info;			/* xmin; successor and cid if gone */
	StringInfoData key;
	ProllyHash	content;
	uint64		ordinal;
	bool		present;		/* named, and in the data tree */
	StringInfoData value;
	ProllyValue row;			/* points into value */
	uint64		count;
} RowAt;

static void
row_at_init(RowAt *r)
{
	memset(r, 0, sizeof(*r));
	initStringInfo(&r->key);
	initStringInfo(&r->value);
}

static void
row_at_free(RowAt *r)
{
	pfree(r->key.data);
	pfree(r->value.data);
}

static bool
same_content(const ProllyHash *a, const ProllyHash *b)
{
	return a->len == b->len && memcmp(a->bytes, b->bytes, a->len) == 0;
}

/* What tid names, as command cid sees it. */
static void
row_at(const ProllyComponents *c, ProllyRootSet *roots, Relation rel,
	   ProllyKeying *keying, ItemPointer tid, CommandId cid, RowAt *r)
{
	ProllyKey	k;
	ProllyValue v;

	resetStringInfo(&r->key);
	resetStringInfo(&r->value);
	r->present = false;
	r->state = PROLLY_CALL(c->row_identity, tid_resolve, roots, rel, tid, cid,
						   &r->key, &r->content, &r->ordinal, &r->info);
	if (r->state == PROLLY_TID_GONE)
	{
		/* key holds the gone version's row */
		r->row.data = (const uint8 *) r->key.data;
		r->row.len = r->key.len;
		return;
	}
	if (r->state != PROLLY_TID_NAMED)
		return;
	k.data = (const uint8 *) r->key.data;
	k.len = r->key.len;
	if (!PROLLY_CALL(c->engine, get, roots, rel, PROLLY_TREE_DATA, &k, cid,
					 &r->value))
		return;
	v.data = (const uint8 *) r->value.data;
	v.len = r->value.len;
	PROLLY_CALL(c->keying, parse_value, keying, &v, &r->row, &r->count);
	if (PROLLY_CALL(c->keying, keyless, keying))
		r->present = r->ordinal < r->count;
	else
	{
		ProllyHash	h;

		PROLLY_CALL(c->keying, content_hash, keying, &r->row, &h);
		r->present = r->ordinal == 0 && same_content(&h, &r->content);
	}
}

/*
 * row_at() for a dirty snapshot (uniqueness and exclusion checks), which
 * must not decide while the row's writer is still running: a row that
 * exists may be being deleted or updated, so its key's writers matter, and
 * a TID nobody knows may be another writer's private row.  Returns whether
 * the caller should take the row as there.
 *
 * Without locks held (an executor's index scan), waits for each such
 * writer to end and looks again at what was committed.  With them (a
 * btree uniqueness check holds its leaf page) waiting could deadlock
 * unseen, so instead it reports the writer in the snapshot as heap reports
 * a deleter (xmax), and says the row is there: the caller lets go of its
 * page, waits, and checks again.
 */
static bool
row_at_dirty(const ProllyComponents *c, ProllyRootSet **roots, Relation rel,
			 ProllyKeying *keying, ItemPointer tid, Snapshot snapshot, RowAt *r)
{
	for (;;)
	{
		TransactionId writer;

		row_at(c, *roots, rel, keying, tid, InvalidCommandId, r);

		/*
		 * A TID nobody knows names no committed version.  In a btree
		 * uniqueness check (the caller holds its page), the caller already
		 * holds the lock on the row's unique value, so a writer still
		 * running with that value would have made it wait: it is a TID of
		 * a writer that is gone (its transaction aborted).  An executor's
		 * check (ON CONFLICT, exclusion) hasn't locked anything, so it
		 * waits for the table's writers.
		 */
		if (r->state == PROLLY_TID_UNKNOWN)
		{
			if (InterruptHoldoffCount > 0)
				return false;
			writer = PROLLY_CALL(c->coord, writer_of, rel, NULL);
			if (!TransactionIdIsValid(writer))
				return false;
			XactLockTableWait(writer, rel, tid, XLTW_None);
			newest_roots(c, roots);
			continue;
		}
		if (r->present)
		{
			ProllyKey	k = {(const uint8 *) r->key.data, r->key.len};

			writer = PROLLY_CALL(c->coord, writer_of, rel,
								 lock_key(rel, c, &k));
		}
		else
			return false;
		if (!TransactionIdIsValid(writer))
			return true;
		if (InterruptHoldoffCount > 0)
		{
			snapshot->xmax = writer;	/* its deleter, for the caller */
			return true;
		}
		XactLockTableWait(writer, rel, tid, XLTW_None);
		newest_roots(c, roots);
	}
}

/*
 * The version tid names, as a snapshot sees it.  SnapshotAny also finds a
 * gone version, kept with its TID as heap keeps a dead tuple (AFTER
 * triggers fetch the old row that way, and an update fetches the row it
 * is about to find was updated meanwhile).
 */
static bool
row_for_snapshot(const ProllyComponents *c, ProllyRootSet *roots, Relation rel,
				 ProllyKeying *keying, ItemPointer tid, Snapshot snapshot,
				 RowAt *r)
{
	row_at(c, roots, rel, keying, tid, snapshot_cid(snapshot), r);
	return r->present ||
		(r->state == PROLLY_TID_GONE &&
		 snapshot != NULL && snapshot->snapshot_type == SNAPSHOT_ANY);
}

static void
store_row(const ProllyComponents *c, Relation rel, ProllyKeying *keying,
		  RowAt *r, ItemPointer tid, TupleTableSlot *slot)
{
	PROLLY_CALL(c->keying, decode_row, keying, &r->row, slot);
	slot->tts_tid = *tid;
	slot->tts_tableOid = RelationGetRelid(rel);
}

/*
 * The keying rel's data tree is read with: the cached current one if the
 * tree records it, else one opened from the record (*owned: close it).
 */
static ProllyKeying *
read_keying(Relation rel, const ProllyComponents *c, ProllyRootSet *roots,
			bool *owned)
{
	InsertKeying *ik = insert_keying(rel, c);
	StringInfoData recorded;
	ProllyKeying *k;

	initStringInfo(&recorded);
	if (!recorded_keying(c, roots, rel, &recorded) ||
		(recorded.len == ik->desc.len &&
		 memcmp(recorded.data, ik->desc.data, recorded.len) == 0))
	{
		pfree(recorded.data);
		*owned = false;
		return ik->keying;
	}
	{
		ProllyValue d = {(const uint8 *) recorded.data, recorded.len};

		k = PROLLY_CALL(c->keying, open, rel, &d, CurrentMemoryContext);
	}
	pfree(recorded.data);
	*owned = true;
	return k;
}

static void
release_keying(const ProllyComponents *c, ProllyKeying *k, bool owned)
{
	if (owned)
		PROLLY_CALL(c->keying, close, k);
}

typedef enum RowFate
{
	ROW_LIVE,
	ROW_SELF_MODIFIED,			/* by this transaction */
	ROW_CONCURRENTLY_MODIFIED,	/* by one that committed meanwhile */
	ROW_INVISIBLE,				/* never seen by this command */
} RowFate;

/*
 * What became of the version at tid, for command cid about to change or
 * lock it; *now is the version with every write.  Fills tmfd unless live.
 */
static RowFate
row_fate(const ProllyComponents *c, ProllyRootSet *roots, Relation rel,
		 ProllyKeying *keying, ItemPointer tid, CommandId cid, RowAt *now,
		 TM_FailureData *tmfd)
{
	RowAt		cmd;
	RowFate		fate;

	row_at_init(&cmd);
	row_at(c, roots, rel, keying, tid, cid, &cmd);
	row_at(c, roots, rel, keying, tid, InvalidCommandId, now);

	tmfd->ctid = *tid;
	tmfd->xmax = InvalidTransactionId;
	tmfd->cmax = InvalidCommandId;
	tmfd->traversed = false;

	if (cmd.present && now->present)
		fate = ROW_LIVE;
	else if (cmd.present)
	{
		/* Changed by this command, or a later one */
		fate = ROW_SELF_MODIFIED;
		tmfd->xmax = GetCurrentTransactionId();
		tmfd->cmax = cid;
		if (now->state == PROLLY_TID_GONE)
		{
			tmfd->cmax = now->info.cid;
			if (ItemPointerIsValid(&now->info.successor))
				tmfd->ctid = now->info.successor;
		}
	}
	else if (cmd.state == PROLLY_TID_GONE)
	{
		RowAt		base;

		row_at_init(&base);
		row_at(c, roots, rel, keying, tid, FirstCommandId, &base);
		if (base.state == PROLLY_TID_GONE)
		{
			/* Retired by a transaction that committed while we waited */
			fate = ROW_CONCURRENTLY_MODIFIED;
			if (ItemPointerIsValid(&base.info.successor))
				tmfd->ctid = base.info.successor;
		}
		else
		{
			/* By an earlier command of this transaction */
			fate = ROW_SELF_MODIFIED;
			tmfd->xmax = GetCurrentTransactionId();
			tmfd->cmax = cmd.info.cid;
			if (ItemPointerIsValid(&cmd.info.successor))
				tmfd->ctid = cmd.info.successor;
		}
		row_at_free(&base);
	}
	else
		fate = ROW_INVISIBLE;
	row_at_free(&cmd);
	return fate;
}

static TM_Result
fate_result(RowFate fate, const TM_FailureData *tmfd, ItemPointer tid)
{
	switch (fate)
	{
		case ROW_LIVE:
			return TM_Ok;
		case ROW_SELF_MODIFIED:
			return TM_SelfModified;
		case ROW_CONCURRENTLY_MODIFIED:
			return ItemPointerEquals(&tmfd->ctid, tid) ? TM_Deleted : TM_Updated;
		case ROW_INVISIBLE:
			break;
	}
	return TM_Invisible;
}

/*
 * Remove the live version *r (at tid) from rel, retiring tid with
 * successor (NULL: deleted).  A keyless table's identical rows are
 * ordinals 0..count-1: the last one takes the removed one's ordinal.
 */
static void
remove_version(const ProllyComponents *c, ProllyRootSet *roots, Relation rel,
			   ProllyKeying *keying, ItemPointer tid, RowAt *r,
			   ItemPointer successor, bool keep_key, CommandId cid)
{
	ProllyKey	k = {(const uint8 *) r->key.data, r->key.len};

	/*
	 * Its prolly index entries go too: of identical rows, the one whose
	 * ordinal no longer exists (they have the same entries otherwise).
	 */
	if (rel->rd_rel->relhasindex)
	{
		TupleTableSlot *slot = MakeSingleTupleTableSlot(RelationGetDescr(rel),
														&TTSOpsVirtual);

		PROLLY_CALL(c->keying, decode_row, keying, &r->row, slot);
		prolly_lock_unique_values(rel, slot);
		prolly_index_remove_row(rel, slot, &k,
								PROLLY_CALL(c->keying, keyless, keying) ?
								r->count - 1 : 0,
								cid);
		ExecDropSingleTupleTableSlot(slot);
	}

	if (PROLLY_CALL(c->keying, keyless, keying) && r->count > 1)
	{
		StringInfoData value;
		ProllyValue v;

		initStringInfo(&value);
		PROLLY_CALL(c->keying, make_value, keying, &r->row, r->count - 1,
					&value);
		v.data = (const uint8 *) value.data;
		v.len = value.len;
		PROLLY_CALL(c->engine, put, rel, PROLLY_TREE_DATA, &k, &v, cid);
		pfree(value.data);
		if (r->ordinal != r->count - 1)
		{
			ProllyRowIdScan *rowid = PROLLY_CALL(c->row_identity, scan_begin,
												 roots, rel, InvalidCommandId);
			ItemPointerData last;

			if (!PROLLY_CALL(c->row_identity, tid_find, rowid, &k, r->count - 1,
							 &last))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("a row of \"%s\" has no TID",
								RelationGetRelationName(rel))));
			PROLLY_CALL(c->row_identity, scan_end, rowid);
			PROLLY_CALL(c->row_identity, tid_rename, roots, rel, &last,
						&r->content, r->ordinal, &k, cid);
		}
	}
	else if (!keep_key)
		PROLLY_CALL(c->engine, remove, rel, PROLLY_TREE_DATA, &k, cid);
	PROLLY_CALL(c->row_identity, tid_retire, roots, rel, tid, successor,
				&r->row, cid);
}

/* Lock the row tid names for writing in mode, then check rel's keying. */
static void
prepare_row_write(Relation rel, const ProllyComponents *c, ItemPointer tid,
				  LockTupleMode mode, InsertKeying **ik_out,
				  ProllyRootSet **roots_out)
{
	InsertKeying *ik = insert_keying(rel, c);
	ProllyRootSet *roots = PROLLY_CALL(c->engine, roots_acquire, NULL);
	StringInfoData key;
	ProllyHash	content;
	uint64		ordinal;
	ProllyTidInfo info;
	ProllyKey	k;

	/* The row's key, to lock; it is looked at again once locked. */
	initStringInfo(&key);
	if (PROLLY_CALL(c->row_identity, tid_resolve, roots, rel, tid,
					InvalidCommandId, &key, &content, &ordinal,
					&info) == PROLLY_TID_NAMED)
	{
		k.data = (const uint8 *) key.data;
		k.len = key.len;
		lock_for_write_mode(rel, c, &k, mode, &roots);
	}

	/*
	 * Else the row was updated or deleted meanwhile (or never was): there
	 * is nothing to lock, and the caller finds it so and doesn't write.
	 */
	pfree(key.data);
	check_keying(rel, c, &roots, ik);
	*ik_out = ik;
	*roots_out = roots;
}

static TM_Result
prolly_tuple_delete(Relation rel, ItemPointer tid, CommandId cid,
					uint32 options, Snapshot snapshot, Snapshot crosscheck,
					bool wait, TM_FailureData *tmfd)
{
	ProllyComponents c;
	InsertKeying *ik;
	ProllyRootSet *roots;
	RowAt		now;
	RowFate		fate;

	prolly_components_for_relation(rel, &c);
	prepare_row_write(rel, &c, tid, LockTupleExclusive, &ik, &roots);
	row_at_init(&now);
	fate = row_fate(&c, roots, rel, ik->keying, tid, cid, &now, tmfd);
	if (fate == ROW_LIVE)
	{
		/*
		 * A row moved to another partition by an update of its partition
		 * key is retired with heap's "moved" successor, so those that find
		 * it later learn where it went, as on heap.
		 */
		ItemPointerData moved;

		ItemPointerSetMovedPartitions(&moved);
		remove_version(&c, roots, rel, ik->keying, tid, &now,
					   (options & TABLE_DELETE_CHANGING_PARTITION) ? &moved : NULL,
					   false, cid);
	}
	row_at_free(&now);
	PROLLY_CALL(c.engine, roots_release, roots);
	return fate_result(fate, tmfd, tid);
}

/*
 * The row lock an update of the row otid names to slot's values needs, as
 * heap decides it: FOR NO KEY UPDATE if no column a foreign key can
 * reference (one of a unique index's) changes, else FOR UPDATE, so
 * foreign-key checks of other transactions (FOR KEY SHARE) aren't blocked
 * by updates of other columns.  Read from the newest commit before the
 * lock: if the row changes before it's locked, the update finds it
 * changed and goes no further.
 */
/*
 * Whether two rows of rel have the same values in the columns of its
 * unique indexes that foreign keys can reference (what FOR KEY SHARE
 * protects): an update keeping them is a no-key update.
 */
static bool
same_key_columns(Relation rel, const ProllyComponents *c, ProllyKeying *keying,
				 const ProllyValue *a, const ProllyValue *b)
{
	Bitmapset  *keycols = RelationGetIndexAttrBitmap(rel, INDEX_ATTR_BITMAP_KEY);
	TupleTableSlot *sa = MakeSingleTupleTableSlot(RelationGetDescr(rel), &TTSOpsVirtual);
	TupleTableSlot *sb = MakeSingleTupleTableSlot(RelationGetDescr(rel), &TTSOpsVirtual);
	bool		same = true;
	int			col = -1;

	PROLLY_CALL(c->keying, decode_row, keying, a, sa);
	PROLLY_CALL(c->keying, decode_row, keying, b, sb);
	slot_getallattrs(sa);
	slot_getallattrs(sb);
	while (same && (col = bms_next_member(keycols, col)) >= 0)
	{
		int			attno = col + FirstLowInvalidHeapAttributeNumber;
		Form_pg_attribute att;

		if (attno <= 0)
			continue;
		att = TupleDescAttr(RelationGetDescr(rel), attno - 1);
		same = sa->tts_isnull[attno - 1] == sb->tts_isnull[attno - 1] &&
			(sa->tts_isnull[attno - 1] ||
			 datumIsEqual(sa->tts_values[attno - 1], sb->tts_values[attno - 1],
						  att->attbyval, att->attlen));
	}
	ExecDropSingleTupleTableSlot(sa);
	ExecDropSingleTupleTableSlot(sb);
	bms_free(keycols);
	return same;
}

static LockTupleMode
update_lock_mode(Relation rel, const ProllyComponents *c, ItemPointer otid,
				 TupleTableSlot *slot)
{
	Bitmapset  *keycols = RelationGetIndexAttrBitmap(rel, INDEX_ATTR_BITMAP_KEY);
	LockTupleMode mode = LockTupleNoKeyExclusive;
	ProllyRootSet *roots;
	ProllyKeying *keying;
	bool		owned;
	RowAt		r;
	TupleTableSlot *old;
	int			col = -1;

	if (bms_is_empty(keycols))
		return mode;
	roots = PROLLY_CALL(c->engine, roots_acquire, NULL);
	keying = read_keying(rel, c, roots, &owned);
	row_at_init(&r);
	row_at(c, roots, rel, keying, otid, InvalidCommandId, &r);

	/*
	 * Compared with the version otid names, gone or not: an update whose
	 * version was updated meanwhile follows the newer one in the mode its
	 * own change needs, as heap's does (comparing with the old tuple).
	 */
	if (!r.present && r.state != PROLLY_TID_GONE)
		mode = LockTupleExclusive;
	else
	{
		old = MakeSingleTupleTableSlot(RelationGetDescr(rel), &TTSOpsVirtual);
		PROLLY_CALL(c->keying, decode_row, keying, &r.row, old);
		slot_getallattrs(old);
		slot_getallattrs(slot);
		while ((col = bms_next_member(keycols, col)) >= 0)
		{
			int			attno = col + FirstLowInvalidHeapAttributeNumber;
			Form_pg_attribute att;

			if (attno <= 0)
				continue;
			att = TupleDescAttr(RelationGetDescr(rel), attno - 1);
			if (old->tts_isnull[attno - 1] != slot->tts_isnull[attno - 1] ||
				(!old->tts_isnull[attno - 1] &&
				 !datumIsEqual(old->tts_values[attno - 1],
							   slot->tts_values[attno - 1],
							   att->attbyval, att->attlen)))
			{
				mode = LockTupleExclusive;
				break;
			}
		}
		ExecDropSingleTupleTableSlot(old);
	}
	row_at_free(&r);
	release_keying(c, keying, owned);
	PROLLY_CALL(c->engine, roots_release, roots);
	bms_free(keycols);
	return mode;
}

/*
 * An update writes the new version and retires the old TID with the new
 * one as its successor.  A version whose content doesn't change gets a new
 * TID all the same (row identity's tid_renew), as heap makes a new tuple.
 */
static TM_Result
prolly_tuple_update(Relation rel, ItemPointer otid, TupleTableSlot *slot,
					CommandId cid, uint32 options, Snapshot snapshot,
					Snapshot crosscheck, bool wait, TM_FailureData *tmfd,
					LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes)
{
	ProllyComponents c;
	InsertKeying *ik;
	ProllyRootSet *roots;
	RowAt		now;
	RowFate		fate;
	ProllyKeying *keying;

	prolly_components_for_relation(rel, &c);
	*lockmode = update_lock_mode(rel, &c, otid, slot);
	*update_indexes = TU_None;
	prepare_row_write(rel, &c, otid, *lockmode, &ik, &roots);
	keying = ik->keying;
	row_at_init(&now);
	fate = row_fate(&c, roots, rel, keying, otid, cid, &now, tmfd);
	if (fate == ROW_LIVE)
	{
		StringInfoData key;
		StringInfoData rowbuf;
		ProllyHash	content;

		initStringInfo(&key);
		initStringInfo(&rowbuf);
		PROLLY_CALL(c.keying, encode_row, keying, slot, &key, &rowbuf, &content);
		if (same_content(&content, &now.content) &&
			key.len == now.key.len &&
			memcmp(key.data, now.key.data, key.len) == 0)
		{
			/*
			 * The row stays as it was, but is a new version, under a new
			 * TID, as on heap: so this command, and writers waiting for
			 * it, see that it changed.
			 */
			PROLLY_CALL(c.row_identity, tid_renew, roots, rel, otid, &now.row,
						cid, &slot->tts_tid);
			slot->tts_tableOid = RelationGetRelid(rel);
			*update_indexes = TU_All;
		}
		else
		{
			ProllyKey	nk = {(const uint8 *) key.data, key.len};
			ProllyKey	ok = {(const uint8 *) now.key.data, now.key.len};

			/*
			 * Keys equal by the key's ordering are the same key even if
			 * their bytes differ ('a' and 'A' under a case-insensitive
			 * collation, numeric 1.0 and 1.00): the row is replaced, and
			 * takes the new bytes.
			 */
			bool		same_key = PROLLY_CALL(c.keying, can_compare, keying) ?
				PROLLY_CALL(c.keying, compare, keying, &nk, &ok) == 0 :
				(nk.len == ok.len && memcmp(nk.data, ok.data, nk.len) == 0);
			ItemPointerData ntid;

			/*
			 * A key respelled: remove it as it was written first, so the
			 * write names the committed key by its bytes too, which is how
			 * it is matched once the key's order is gone (its collation
			 * dropped later in the transaction).
			 */
			if (same_key && !PROLLY_CALL(c.keying, keyless, keying) &&
				(nk.len != ok.len || memcmp(nk.data, ok.data, nk.len) != 0))
				PROLLY_CALL(c.engine, remove, rel, PROLLY_TREE_DATA, &ok, cid);

			/* The new version first, then the old one goes. */
			insert_row(rel, &c, keying, &roots, slot, 1, NULL, same_key, cid);
			ntid = slot->tts_tid;
			remove_version(&c, roots, rel, keying, otid, &now, &ntid,
						   same_key && !PROLLY_CALL(c.keying, keyless, keying),
						   cid);
			*update_indexes = TU_All;
		}
		pfree(key.data);
		pfree(rowbuf.data);
	}
	row_at_free(&now);
	PROLLY_CALL(c.engine, roots_release, roots);
	return fate_result(fate, tmfd, otid);
}

/*
 * Row locks are the coordination module's.  With FIND_LAST_VERSION, a row
 * a committed transaction updated meanwhile is followed to its newest
 * version, which is locked and returned instead.
 */
static TM_Result
prolly_tuple_lock(Relation rel, ItemPointer tid, Snapshot snapshot,
				  TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
				  LockWaitPolicy wait_policy, uint8 flags,
				  TM_FailureData *tmfd)
{
	ProllyComponents c;
	ProllyRootSet *roots;
	ProllyKeying *keying;
	bool		owned;
	RowAt		now;
	RowFate		fate;
	TM_Result	result;
	StringInfoData key;
	ProllyHash	content;
	uint64		ordinal;
	ProllyTidInfo info;
	ProllyKey	k;
	bool		traversed = false;

	prolly_components_for_relation(rel, &c);
	for (;;)
	{
		/*
		 * Lock the row tid names now, if it names one: one updated or
		 * deleted meanwhile has nothing to lock, and is followed below.
		 */
		roots = PROLLY_CALL(c.engine, roots_acquire, NULL);
		initStringInfo(&key);
		result = TM_Ok;
		if (PROLLY_CALL(c.row_identity, tid_resolve, roots, rel, tid,
						InvalidCommandId, &key, &content, &ordinal,
						&info) == PROLLY_TID_NAMED)
		{
			k.data = (const uint8 *) key.data;
			k.len = key.len;
			result = PROLLY_CALL(c.coord, lock_row, rel, lock_key(rel, &c, &k),
								 mode, wait_policy, tmfd);
		}
		pfree(key.data);
		PROLLY_CALL(c.engine, roots_release, roots);
		if (result != TM_Ok)
		{
			tmfd->ctid = *tid;
			tmfd->traversed = traversed;
			return result;
		}

		roots = PROLLY_CALL(c.engine, roots_acquire, NULL);
		keying = read_keying(rel, &c, roots, &owned);
		row_at_init(&now);
		fate = row_fate(&c, roots, rel, keying, tid, cid, &now, tmfd);

		/*
		 * FOR KEY SHARE doesn't conflict with an update that kept the key
		 * columns: as heap does, it locks the row and returns the version
		 * it was given, whatever the isolation level, rather than follow
		 * the update.
		 */
		if (fate == ROW_CONCURRENTLY_MODIFIED && mode == LockTupleKeyShare &&
			!ItemPointerEquals(&tmfd->ctid, tid) &&
			!ItemPointerIndicatesMovedPartitions(&tmfd->ctid))
		{
			RowAt		gone;
			RowAt		last;
			ItemPointerData next = tmfd->ctid;

			row_at_init(&gone);
			row_at_init(&last);
			row_at(&c, roots, rel, keying, tid, InvalidCommandId, &gone);
			for (;;)
			{
				row_at(&c, roots, rel, keying, &next, InvalidCommandId, &last);
				if (last.present || last.state != PROLLY_TID_GONE ||
					!ItemPointerIsValid(&last.info.successor) ||
					ItemPointerIndicatesMovedPartitions(&last.info.successor))
					break;
				next = last.info.successor;
			}
			if (gone.state == PROLLY_TID_GONE && last.present &&
				same_key_columns(rel, &c, keying, &gone.row, &last.row))
			{
				ProllyKey	lk = {(const uint8 *) last.key.data, last.key.len};

				result = PROLLY_CALL(c.coord, lock_row, rel,
									 lock_key(rel, &c, &lk), mode,
									 wait_policy, tmfd);
				if (result == TM_Ok)
					store_row(&c, rel, keying, &gone, tid, slot);
				row_at_free(&gone);
				row_at_free(&last);
				row_at_free(&now);
				release_keying(&c, keying, owned);
				PROLLY_CALL(c.engine, roots_release, roots);
				if (result != TM_Ok)
				{
					tmfd->ctid = *tid;
					tmfd->traversed = traversed;
				}
				return result;
			}
			row_at_free(&gone);
			row_at_free(&last);
		}

		/* As heap's, following a row that moved partitions is an error. */
		if (fate == ROW_CONCURRENTLY_MODIFIED &&
			(flags & TUPLE_LOCK_FLAG_FIND_LAST_VERSION) &&
			ItemPointerIndicatesMovedPartitions(&tmfd->ctid))
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("tuple to be locked was already moved to another partition due to concurrent update")));
		if (fate == ROW_CONCURRENTLY_MODIFIED &&
			(flags & TUPLE_LOCK_FLAG_FIND_LAST_VERSION) &&
			!ItemPointerEquals(&tmfd->ctid, tid))
		{
			ItemPointerData next = tmfd->ctid;

			/* Follow the successors to the version that exists now. */
			for (;;)
			{
				row_at(&c, roots, rel, keying, &next, InvalidCommandId, &now);
				if (now.present || now.state != PROLLY_TID_GONE ||
					!ItemPointerIsValid(&now.info.successor) ||
					ItemPointerIndicatesMovedPartitions(&now.info.successor))
					break;
				next = now.info.successor;
			}
			if (now.present)
			{
				RowAt		seen;

				/*
				 * A version this transaction wrote, in this command or a
				 * later one, isn't the command's to lock: as heap does,
				 * report it modified by this transaction (when, in cmax,
				 * this command or a later one: the caller tells them apart).
				 */
				row_at_init(&seen);
				row_at(&c, roots, rel, keying, &next, cid, &seen);
				if (!seen.present)
				{
					RowAt		after;

					row_at_init(&after);
					row_at(&c, roots, rel, keying, &next, cid + 1, &after);
					tmfd->ctid = next;
					tmfd->xmax = GetCurrentTransactionId();
					tmfd->cmax = after.present ? cid : cid + 1;
					tmfd->traversed = true;
					row_at_free(&after);
					row_at_free(&seen);
					row_at_free(&now);
					release_keying(&c, keying, owned);
					PROLLY_CALL(c.engine, roots_release, roots);
					return TM_SelfModified;
				}
				row_at_free(&seen);

				/* Lock that version, and look at it again once locked. */
				*tid = next;
				traversed = true;
				row_at_free(&now);
				release_keying(&c, keying, owned);
				PROLLY_CALL(c.engine, roots_release, roots);
				continue;
			}
			tmfd->ctid = *tid;	/* deleted at the end of the chain */
		}
		break;
	}
	tmfd->traversed = traversed;
	/*
	 * The slot holds the row when it's live, and also when the command
	 * can't see it because it inserted it itself: ON CONFLICT reads its
	 * xmin to report a row affected a second time.
	 */
	if (fate == ROW_LIVE || (fate == ROW_INVISIBLE && now.present))
		store_row(&c, rel, keying, &now, tid, slot);
	row_at_free(&now);
	release_keying(&c, keying, owned);
	PROLLY_CALL(c.engine, roots_release, roots);
	if (fate == ROW_LIVE)
		return TM_Ok;
	return fate_result(fate, tmfd, tid);
}

/*
 * Speculative insertion: no other inserter can race this one (it holds
 * the right to write), so the row is inserted outright, and removed again
 * if the insertion is abandoned.
 */
static void
prolly_tuple_insert_speculative(Relation rel, TupleTableSlot *slot,
								CommandId cid, uint32 options,
								BulkInsertStateData *bistate, uint32 specToken)
{
	ProllyComponents c;
	TM_FailureData tmfd;

	prolly_components_for_relation(rel, &c);
	(void) PROLLY_CALL(c.coord, insert_intent, rel, NULL, specToken, &tmfd);
	prolly_tuple_insert(rel, slot, cid, options, bistate);
}

static void
prolly_tuple_complete_speculative(Relation rel, TupleTableSlot *slot,
								  uint32 specToken, bool succeeded)
{
	ProllyComponents c;
	InsertKeying *ik;
	ProllyRootSet *roots;
	RowAt		now;

	prolly_components_for_relation(rel, &c);
	PROLLY_CALL(c.coord, insert_intent_complete, rel, NULL, specToken,
				succeeded);
	if (succeeded)
		return;
	prepare_row_write(rel, &c, &slot->tts_tid, LockTupleExclusive, &ik, &roots);
	row_at_init(&now);
	row_at(&c, roots, rel, ik->keying, &slot->tts_tid, InvalidCommandId, &now);
	if (now.present)
		remove_version(&c, roots, rel, ik->keying, &slot->tts_tid, &now, NULL,
					   false, GetCurrentCommandId(true));
	row_at_free(&now);
	PROLLY_CALL(c.engine, roots_release, roots);
}

static bool
prolly_tuple_fetch_row_version(Relation rel, ItemPointer tid,
							   Snapshot snapshot, TupleTableSlot *slot)
{
	ProllyComponents c;
	ProllyRootSet *roots;
	ProllyKeying *keying;
	bool		owned;
	RowAt		r;
	bool		found;

	prolly_components_for_relation(rel, &c);
	roots = PROLLY_CALL(c.engine, roots_acquire, snapshot);
	serializable_read(rel, &c, snapshot);
	keying = read_keying(rel, &c, roots, &owned);
	row_at_init(&r);
	found = row_for_snapshot(&c, roots, rel, keying, tid, snapshot, &r);
	if (found)
		store_row(&c, rel, keying, &r, tid, slot);
	row_at_free(&r);
	release_keying(&c, keying, owned);
	PROLLY_CALL(c.engine, roots_release, roots);
	return found;
}

/* Every live version is reachable only from its own TID. */
static bool
prolly_fetch_tid(Relation rel, ItemPointer tid, Snapshot snapshot,
				 bool *all_dead)
{
	ProllyComponents c;
	ProllyRootSet *roots;
	ProllyKeying *keying;
	bool		owned;
	RowAt		r;
	bool		found;

	if (all_dead)
		*all_dead = false;
	prolly_components_for_relation(rel, &c);
	roots = PROLLY_CALL(c.engine, roots_acquire, snapshot);
	keying = read_keying(rel, &c, roots, &owned);
	row_at_init(&r);
	if (snapshot != NULL && snapshot->snapshot_type == SNAPSHOT_DIRTY)
	{
		(void) snapshot_cid(snapshot);
		found = row_at_dirty(&c, &roots, rel, keying, tid, snapshot, &r);
	}
	else
	{
		row_at(&c, roots, rel, keying, tid, snapshot_cid(snapshot), &r);
		found = r.present;
	}
	row_at_free(&r);
	release_keying(&c, keying, owned);
	PROLLY_CALL(c.engine, roots_release, roots);
	return found;
}

static bool
prolly_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
	return ItemPointerIsValid(tid) &&
		ItemPointerGetOffsetNumber(tid) <= MaxHeapTuplesPerPage;
}

static void
prolly_tuple_get_latest_tid(TableScanDesc sscan, ItemPointer tid)
{
	ProllyScanDesc scan = (ProllyScanDesc) sscan;
	RowAt		r;
	ItemPointerData at = *tid;

	row_at_init(&r);
	for (;;)
	{
		row_at(&scan->c, scan->roots, sscan->rs_rd, scan->keying, &at,
			   scan->cid, &r);
		if (r.present)
		{
			*tid = at;
			break;
		}
		if (r.state != PROLLY_TID_GONE ||
			!ItemPointerIsValid(&r.info.successor) ||
			ItemPointerIndicatesMovedPartitions(&r.info.successor))
			break;
		at = r.info.successor;
		CHECK_FOR_INTERRUPTS();
	}
	row_at_free(&r);
}

static bool
prolly_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
								Snapshot snapshot)
{
	ProllyComponents c;
	ProllyRootSet *roots;
	ProllyKeying *keying;
	bool		owned;
	RowAt		r;
	bool		found;

	prolly_components_for_relation(rel, &c);
	roots = PROLLY_CALL(c.engine, roots_acquire, snapshot);
	keying = read_keying(rel, &c, roots, &owned);
	row_at_init(&r);
	row_at(&c, roots, rel, keying, &slot->tts_tid, snapshot_cid(snapshot), &r);
	found = r.present;
	row_at_free(&r);
	release_keying(&c, keying, owned);
	PROLLY_CALL(c.engine, roots_release, roots);
	return found;
}

/*
 * Index entries the index AM already knows are dead stay deletable; no
 * others are found (bottom-up deletion gets none), as versions are
 * reclaimed by VACUUM.
 */
static TransactionId
prolly_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
	if (delstate->bottomup)
		delstate->ndeltids = 0;
	return InvalidTransactionId;
}

/* ------------------------------------------------------------------------
 * Index scans of prolly tables (index AMs that store TIDs)
 * ------------------------------------------------------------------------
 */

typedef struct ProllyIndexScan
{
	ProllyComponents c;
	MemoryContext mcxt;
	ProllyRootSet *roots;
	ProllyKeying *keying;
	bool		owned;
	uint64		rekeys;			/* prolly_rekeys when keying was opened */
	CommandId	cid;
	RowAt		row;
} ProllyIndexScan;

static bool
prolly_index_getnext_slot(IndexScanDesc scan, ScanDirection direction,
						  TupleTableSlot *slot)
{
	ProllyIndexScan *is = (ProllyIndexScan *) scan->xs_table_opaque;
	Relation	rel = scan->heapRelation;

	for (;;)
	{
		MemoryContext old;
		bool		found;

		if (!tableam_index_getnext_tid(scan, direction))
			return false;
		if (scan->xs_want_itup && scan->instrument)
			scan->instrument->ntabletuplefetches++;
		old = MemoryContextSwitchTo(is->mcxt);
		if (is->rekeys != prolly_rekeys)
		{
			/* The statement rebuilt the table: read the rebuilt rows. */
			release_keying(&is->c, is->keying, is->owned);
			is->keying = read_keying(rel, &is->c, is->roots, &is->owned);
			is->rekeys = prolly_rekeys;
		}
		(void) snapshot_cid(scan->xs_snapshot);
		if (scan->xs_snapshot->snapshot_type == SNAPSHOT_DIRTY)
			found = row_at_dirty(&is->c, &is->roots, rel, is->keying,
								 &scan->xs_heaptid, scan->xs_snapshot,
								 &is->row) && is->row.present;
		else
		{
			row_at(&is->c, is->roots, rel, is->keying, &scan->xs_heaptid,
				   is->cid, &is->row);
			found = is->row.present;
		}
		MemoryContextSwitchTo(old);
		if (!found)
			continue;			/* no visible version, try the next entry */
		pgstat_count_heap_fetch(scan->indexRelation);
		if (scan->xs_want_itup)
			tableam_index_fill_ios_slot(scan, slot);
		else
			store_row(&is->c, rel, is->keying, &is->row, &scan->xs_heaptid,
					  slot);
		return true;
	}
}

static void
prolly_index_scan_begin(IndexScanDesc scan, uint32 flags)
{
	ProllyIndexScan *is = palloc0(sizeof(ProllyIndexScan));
	MemoryContext old;

	prolly_components_for_relation(scan->heapRelation, &is->c);
	is->mcxt = AllocSetContextCreate(CurrentMemoryContext, "prolly index scan",
									 ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(is->mcxt);
	is->roots = PROLLY_CALL(is->c.engine, roots_acquire, scan->xs_snapshot);
	serializable_read(scan->heapRelation, &is->c, scan->xs_snapshot);
	is->keying = read_keying(scan->heapRelation, &is->c, is->roots, &is->owned);
	is->rekeys = prolly_rekeys;
	is->cid = snapshot_cid(scan->xs_snapshot);
	row_at_init(&is->row);
	MemoryContextSwitchTo(old);
	scan->xs_getnext_slot = prolly_index_getnext_slot;
	scan->xs_table_opaque = is;
}

static void
prolly_index_scan_reset(IndexScanDesc scan)
{
}

static void
prolly_index_scan_end(IndexScanDesc scan)
{
	ProllyIndexScan *is = (ProllyIndexScan *) scan->xs_table_opaque;

	release_keying(&is->c, is->keying, is->owned);
	PROLLY_CALL(is->c.engine, roots_release, is->roots);
	MemoryContextDelete(is->mcxt);
	pfree(is);
}

/* ------------------------------------------------------------------------
 * Relation storage: these map directly onto the engine interface.
 *
 * Rows live in the engine's store, but every relation also gets a main
 * fork, as Postgres expects of a relation with storage: CREATE DATABASE,
 * ALTER TABLE SET TABLESPACE and the size functions open it.  It holds
 * only per-table pages of components that need state outside
 * transactions (row identity's TID counter), and is copied with the
 * table.
 * ------------------------------------------------------------------------
 */

static void
prolly_relation_set_new_filelocator(Relation rel,
									const RelFileLocator *newrlocator,
									char persistence,
									TransactionId *freezeXid,
									MultiXactId *minmulti)
{
	ProllyComponents components;
	const ProllyEngineRoutine *engine;

	/* Rows carry no xids, so there is nothing to freeze. */
	*freezeXid = InvalidTransactionId;
	*minmulti = InvalidMultiXactId;

	prolly_components_for_relation(rel, &components);
	engine = components.engine;
	PROLLY_CALL(engine, tree_create, rel, newrlocator, persistence);
	smgrclose(RelationCreateStorage(*newrlocator, persistence, true));

	/* New storage for an existing table (TRUNCATE): TIDs go on. */
	if (!RelFileLocatorEquals(rel->rd_locator, *newrlocator) &&
		RelFileNumberIsValid(rel->rd_locator.relNumber) &&
		smgrexists(RelationGetSmgr(rel), MAIN_FORKNUM))
		PROLLY_CALL(components.row_identity, counter_carry, rel, newrlocator);
}

static void
prolly_relation_nontransactional_truncate(Relation rel)
{
	ProllyComponents components;
	const ProllyEngineRoutine *engine;

	prolly_components_for_relation(rel, &components);
	engine = components.engine;
	PROLLY_CALL(engine, tree_truncate, rel);
}

void
prolly_swap_relations(Oid r1, Oid relam1, Oid r2, Oid relam2)
{
	char	   *am1 = get_am_name(relam1);
	char	   *am2 = get_am_name(relam2);
	bool		prolly1 = prolly_is_table_am(am1);
	bool		prolly2 = prolly_is_table_am(am2);
	Relation	rel;
	Relation	other;
	ProllyComponents c;

	if (!prolly1 && !prolly2)
		return;

	/*
	 * The swap's prolly side orders both sides' trees (the other has its
	 * columns, or no trees).  The rewritten table's TIDs go on from the
	 * old storage's.
	 */
	rel = relation_open(prolly1 ? r1 : r2, NoLock);
	other = relation_open(prolly1 ? r2 : r1, NoLock);
	prolly_components_for_relation(rel, &c);
	PROLLY_CALL(c.engine, tree_swap, rel, RelationGetRelid(other));
	if (prolly1 && prolly2)
		PROLLY_CALL(c.row_identity, counter_carry, rel, &other->rd_locator);
	relation_close(other, NoLock);
	relation_close(rel, NoLock);
}

void
prolly_drop_relation(Relation rel)
{
	ProllyComponents c;
	char	   *amname = get_am_name(rel->rd_rel->relam);
	bool		prolly = rel->rd_rel->relkind == RELKIND_INDEX ?
		amname != NULL && strcmp(amname, "prolly_d1_index") == 0 :
		prolly_is_table_am(amname);

	if (!prolly)
		return;
	prolly_components_for_relation(rel, &c);
	PROLLY_CALL(c.engine, tree_drop, rel);
}

static void
prolly_relation_copy_data(Relation rel, const RelFileLocator *newrlocator)
{
	SMgrRelation dstrel;

	/*
	 * The trees stay where they are (they're found by relation, not by
	 * storage); the main fork holds the components' per-table pages (TID
	 * counter).
	 */
	FlushRelationBuffers(rel);
	dstrel = RelationCreateStorage(*newrlocator, rel->rd_rel->relpersistence,
								   true);
	RelationCopyStorage(RelationGetSmgr(rel), dstrel, MAIN_FORKNUM,
						rel->rd_rel->relpersistence);

	/* The old storage goes when the transaction commits, as heap's does. */
	RelationDropStorage(rel);
	smgrclose(dstrel);
}

/* Whether a live column of rel has a missing value (added with a default). */
static bool
has_missing_values(Relation rel)
{
	TupleDesc	desc = RelationGetDescr(rel);

	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (!att->attisdropped && att->atthasmissing)
			return true;
	}
	return false;
}

/*
 * CLUSTER and VACUUM FULL: rows are kept in key order whatever the index,
 * and carry no xids to freeze, so the new storage takes the table's trees
 * as they are (the transaction's own writes included), and its TID
 * counter: the rows, their keying and their TIDs are the same.  Nothing
 * else can write the table meanwhile (it is locked exclusively).
 *
 * Postgres clears the missing values of added columns once the table is
 * rewritten, so rows written before such a column was added (which don't
 * hold it) are rewritten in the new storage with its value, as rows of a
 * table without missing values are encoded; they keep their TIDs.
 */
static void
prolly_relation_copy_for_cluster(Relation OldTable, Relation NewTable,
								 Relation OldIndex, bool use_sort,
								 TransactionId OldestXmin, Snapshot snapshot,
								 TransactionId *xid_cutoff,
								 MultiXactId *multi_cutoff,
								 double *num_tuples, double *tups_vacuumed,
								 double *tups_recently_dead)
{
	ProllyComponents c;
	ProllyRootSet *roots;
	ProllyKeying *keying;
	bool		owned;

	prolly_components_for_relation(OldTable, &c);
	PROLLY_CALL(c.engine, tree_copy, OldTable, NewTable);
	FlushRelationBuffers(OldTable);
	RelationCopyStorage(RelationGetSmgr(OldTable), RelationGetSmgr(NewTable),
						MAIN_FORKNUM, NewTable->rd_rel->relpersistence);

	roots = PROLLY_CALL(c.engine, roots_acquire, NULL);
	if (has_missing_values(OldTable))
	{
		ProllyKeying *old = open_read_keying(&c, roots, OldTable,
											 CurrentMemoryContext);
		ProllyKeying *now = PROLLY_CALL(c.keying, open_rewritten, OldTable,
										CurrentMemoryContext);

		/*
		 * The copied trees record the keying, which describes itself the
		 * same rewritten or not.
		 */
		rekey(NewTable, &c, &roots, old, now);
		PROLLY_CALL(c.keying, close, now);
		PROLLY_CALL(c.keying, close, old);
	}

	/*
	 * The row count: the item count, but in a keyless table an item holds
	 * every copy of identical rows, so their counts are summed.
	 */
	keying = read_keying(OldTable, &c, roots, &owned);
	if (PROLLY_CALL(c.keying, keyless, keying))
	{
		ProllyCursor *cur = PROLLY_CALL(c.engine, cursor_open, roots, OldTable,
										PROLLY_TREE_DATA, InvalidCommandId);
		ProllyKey	key;
		ProllyValue value;

		*num_tuples = 0;
		PROLLY_CALL(c.engine, cursor_seek, cur, NULL, ForwardScanDirection);
		while (PROLLY_CALL(c.engine, cursor_next, cur, &key, &value))
		{
			ProllyValue row;
			uint64		count;

			CHECK_FOR_INTERRUPTS();
			PROLLY_CALL(c.keying, parse_value, keying, &value, &row, &count);
			*num_tuples += count;
		}
		PROLLY_CALL(c.engine, cursor_close, cur);
	}
	else
		*num_tuples = (double) PROLLY_CALL(c.engine, count_range, roots,
										   OldTable, PROLLY_TREE_DATA, NULL,
										   NULL);
	release_keying(&c, keying, owned);
	PROLLY_CALL(c.engine, roots_release, roots);
	*xid_cutoff = InvalidTransactionId;
	*multi_cutoff = InvalidMultiXactId;
	*tups_vacuumed = 0;
	*tups_recently_dead = 0;
}

typedef struct DeadTids
{
	ItemPointer tids;			/* sorted */
	int			n;
} DeadTids;

static bool
dead_tid(ItemPointer itemptr, void *state)
{
	DeadTids   *d = state;

	return bsearch(itemptr, d->tids, d->n, sizeof(ItemPointerData),
				   (int (*) (const void *, const void *)) ItemPointerCompare) != NULL;
}

/*
 * VACUUM rel's indexes other than prolly's: remove their entries for dead
 * TIDs (if any), then let each clean up, as heap's VACUUM does whether or
 * not it found dead tuples (GIN flushes its pending list, BRIN summarizes
 * new ranges), unless INDEX_CLEANUP is off.
 */
static void
vacuum_index_entries(Relation rel, ItemPointer dead, int ndead,
					 const VacuumParams *params, BufferAccessStrategy bstrategy)
{
	Relation   *indexes;
	int			nindexes;
	DeadTids	d = {dead, ndead};
	bool		cleanup = params->index_cleanup != VACOPTVALUE_DISABLED;

	if (ndead > 0)
		qsort(dead, ndead, sizeof(ItemPointerData),
			  (int (*) (const void *, const void *)) ItemPointerCompare);
	vac_open_indexes(rel, RowExclusiveLock, &nindexes, &indexes);
	for (int i = 0; i < nindexes; i++)
	{
		IndexVacuumInfo ivinfo = {0};
		IndexBulkDeleteResult *stats;

		if (prolly_index_is_prolly(indexes[i]))
			continue;
		ivinfo.index = indexes[i];
		ivinfo.heaprel = rel;
		ivinfo.analyze_only = false;
		ivinfo.report_progress = false;
		ivinfo.estimated_count = true;
		ivinfo.message_level = DEBUG2;
		ivinfo.num_heap_tuples = -1;
		ivinfo.strategy = bstrategy;
		stats = ndead > 0 ? index_bulk_delete(&ivinfo, NULL, dead_tid, &d) : NULL;
		if (cleanup)
			stats = index_vacuum_cleanup(&ivinfo, stats);
		if (stats)
			pfree(stats);
	}
	vac_close_indexes(nindexes, indexes, NoLock);
}

static void
prolly_relation_vacuum(Relation rel, const VacuumParams *params,
					   BufferAccessStrategy bstrategy)
{
	ProllyComponents c;
	ProllyRootSet *roots;
	ItemPointer dead;
	int			ndead;

	prolly_components_for_relation(rel, &c);
	PROLLY_CALL(c.engine, vacuum, rel, params, bstrategy);

	/*
	 * Versions retired by transactions no snapshot can still see as
	 * running are gone for everyone, like heap's dead tuples: drop other
	 * index AMs' entries for their TIDs (a prolly index's entries point at
	 * row keys, and go when a row does), then forget the TIDs, which frees
	 * them for reuse.
	 */
	roots = PROLLY_CALL(c.engine, roots_acquire, NULL);
	ndead = PROLLY_CALL(c.row_identity, dead_tids, roots, rel,
						GetOldestNonRemovableTransactionId(rel), &dead);
	PROLLY_CALL(c.engine, roots_release, roots);
	vacuum_index_entries(rel, dead, ndead, params, bstrategy);
	if (ndead > 0)
		PROLLY_CALL(c.row_identity, tid_forget, rel, dead, ndead,
					GetCurrentCommandId(true));
	ereport(params->log_vacuum_min_duration == 0 ? LOG : DEBUG2,
			(errmsg("prolly table \"%s\": %d gone row versions removed",
					RelationGetRelationName(rel), ndead)));
	pfree(dead);
}

/* ------------------------------------------------------------------------
 * ANALYZE, index builds, sizes and sampling
 * ------------------------------------------------------------------------
 */

/* ANALYZE samples prolly tables with prolly_acquire_sample_rows instead. */
static bool
prolly_scan_analyze_next_block(TableScanDesc scan, ReadStream *stream)
{
	not_implemented(scan->rs_rd, "scan_analyze_next_block");
}

static bool
prolly_scan_analyze_next_tuple(TableScanDesc scan, double *liverows,
							   double *deadrows, TupleTableSlot *slot)
{
	not_implemented(scan->rs_rd, "scan_analyze_next_tuple");
}

/*
 * Index every row (start_blockno and numblocks name heap block ranges,
 * which prolly tables don't have, so the whole table is scanned).  Rows
 * are read with every write of the transaction visible, like heap's
 * SnapshotAny build, minus dead rows: a prolly tree holds only live ones.
 */
static double
prolly_index_build_range_scan(Relation table_rel, Relation index_rel,
							  IndexInfo *index_info, bool allow_sync,
							  bool anyvisible, bool progress,
							  BlockNumber start_blockno, BlockNumber numblocks,
							  IndexBuildCallback callback,
							  void *callback_state, TableScanDesc scan)
{
	EState	   *estate;
	ExprContext *econtext;
	TupleTableSlot *slot;
	ExprState  *predicate;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	double		reltuples = 0;

	if (scan != NULL)
		not_implemented(table_rel, "parallel index builds");

	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	slot = table_slot_create(table_rel, NULL);
	econtext->ecxt_scantuple = slot;
	predicate = ExecPrepareQual(index_info->ii_Predicate, estate);

	scan = table_beginscan_strat(table_rel, SnapshotAny, 0, NULL, true,
								 allow_sync);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		CHECK_FOR_INTERRUPTS();
		MemoryContextReset(econtext->ecxt_per_tuple_memory);
		reltuples += 1;
		if (predicate != NULL && !ExecQual(predicate, econtext))
			continue;
		FormIndexDatum(index_info, slot, estate, values, isnull);
		callback(index_rel, &slot->tts_tid, values, isnull, true,
				 callback_state);
	}
	table_endscan(scan);

	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
	index_info->ii_ExpressionsState = NIL;
	index_info->ii_PredicateState = NULL;
	return reltuples;
}

static int
tid_cmp(const void *a, const void *b)
{
	return ItemPointerCompare((const ItemPointerData *) a,
							  (const ItemPointerData *) b);
}

static void
prolly_index_validate_scan(Relation table_rel, Relation index_rel,
						   IndexInfo *index_info, Snapshot snapshot,
						   ValidateIndexState *state)
{
	EState	   *estate;
	ExprContext *econtext;
	TupleTableSlot *slot;
	ExprState  *predicate;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	TableScanDesc scan;
	ItemPointerData *indexed;
	int64		nindexed = 0;
	int64		cap = 1024;
	Datum		ts_val;
	bool		ts_isnull;

	/*
	 * A prolly index's entries point at row keys, which an update keeps, so
	 * checking that every TID is indexed can't tell a stale entry from a
	 * right one.  Instead the index is rebuilt from the rows as they are,
	 * holding the right to write the table so no writer commits meanwhile;
	 * from then on, writers keep it.
	 */
	if (prolly_index_is_prolly(index_rel))
	{
		ProllyComponents c;
		ProllyRootSet *roots;

		prolly_components_for_relation(table_rel, &c);
		roots = PROLLY_CALL(c.engine, roots_acquire, NULL);
		lock_for_write(table_rel, &c, NULL, &roots);
		PROLLY_CALL(c.engine, roots_release, roots);
		prolly_index_rebuild(table_rel, index_rel);
		return;
	}

	/*
	 * The index's TIDs come sorted; rows come in key order, not TID order,
	 * so the TIDs are kept for lookups rather than merged.
	 */
	indexed = palloc(sizeof(ItemPointerData) * cap);
	while (tuplesort_getdatum(state->tuplesort, true, false, &ts_val,
							  &ts_isnull, NULL))
	{
		if (nindexed == cap)
		{
			cap *= 2;
			indexed = repalloc_huge(indexed, sizeof(ItemPointerData) * cap);
		}
		itemptr_decode(&indexed[nindexed++], DatumGetInt64(ts_val));
	}

	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	slot = table_slot_create(table_rel, NULL);
	econtext->ecxt_scantuple = slot;
	predicate = ExecPrepareQual(index_info->ii_Predicate, estate);

	/* Rows the snapshot sees that the index doesn't have yet. */
	scan = table_beginscan_strat(table_rel, snapshot, 0, NULL, true, false);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		CHECK_FOR_INTERRUPTS();
		state->htups += 1;
		if (bsearch(&slot->tts_tid, indexed, nindexed, sizeof(ItemPointerData),
					tid_cmp))
			continue;
		MemoryContextReset(econtext->ecxt_per_tuple_memory);
		if (predicate != NULL && !ExecQual(predicate, econtext))
			continue;
		FormIndexDatum(index_info, slot, estate, values, isnull);
		index_insert(index_rel, values, isnull, &slot->tts_tid, table_rel,
					 index_info->ii_Unique ? UNIQUE_CHECK_YES : UNIQUE_CHECK_NO,
					 false, index_info);
		state->tups_inserted += 1;
	}
	table_endscan(scan);

	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
	pfree(indexed);
	index_info->ii_ExpressionsState = NIL;
	index_info->ii_PredicateState = NULL;
}

/* Prolly tables have no files of their own: nodes live in the node store. */
static uint64
prolly_relation_size(Relation rel, ForkNumber forkNumber)
{
	return 0;
}

static bool
prolly_relation_needs_toast_table(Relation rel)
{
	/* Large values will go out of line in the node store, not TOAST. */
	return false;
}

static Oid
prolly_relation_toast_am(Relation rel)
{
	not_implemented(rel, "relation_toast_am");
}

static void
prolly_relation_fetch_toast_slice(Relation toastrel, Oid8 valueid,
								  int32 attrsize, int32 sliceoffset,
								  int32 slicelength, varlena *result)
{
	not_implemented(toastrel, "relation_fetch_toast_slice");
}

/*
 * Rows from the tree's subtree counts (items, so a keyless table's
 * identical rows count once); pages as if the rows were packed into heap
 * pages, so plans cost a prolly table like a heap table of the same size.
 */
/*
 * The width of a row by its columns' types only, so a table's size in
 * pages doesn't change when statistics are gathered: ANALYZE records rows
 * per page, which are estimated from it later.
 */
static int32
row_width_by_types(Relation rel)
{
	TupleDesc	desc = RelationGetDescr(rel);
	int32		width = 0;

	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (!att->attisdropped)
			width += get_typavgwidth(att->atttypid, att->atttypmod);
	}
	return width;
}

static void
prolly_relation_estimate_size(Relation rel, int32 *attr_widths,
							  BlockNumber *pages, double *tuples,
							  double *allvisfrac)
{
	ProllyComponents c;
	ProllyRootSet *roots;
	uint64		n;
	int32		width;

	prolly_components_for_relation(rel, &c);
	roots = PROLLY_CALL(c.engine, roots_acquire, NULL);
	n = PROLLY_CALL(c.engine, count_range, roots, rel, PROLLY_TREE_DATA,
					NULL, NULL);
	PROLLY_CALL(c.engine, roots_release, roots);

	/*
	 * Pages are the items' size in pages.  Rows: the rows per page ANALYZE
	 * (or VACUUM FULL) last found, times the pages now, as heap estimates;
	 * an item can hold several identical rows.  Never analyzed: a row per
	 * item.
	 */
	width = row_width_by_types(rel) + 8;
	*pages = n == 0 ? 0 :
		(BlockNumber) Max(1, ((double) n * width) / (BLCKSZ * 0.9));
	if (rel->rd_rel->relpages > 0 && rel->rd_rel->reltuples >= 0)
		*tuples = rint((rel->rd_rel->reltuples / rel->rd_rel->relpages) * *pages);
	else
		*tuples = (double) n;
	*allvisfrac = 0;
}

static int
uint64_cmp(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a;
	uint64		y = *(const uint64 *) b;

	return (x > y) - (x < y);
}

typedef struct SampledRow
{
	HeapTuple	tuple;
	uint64		seq;			/* among the rows read, in key order */
} SampledRow;

static int
sampled_row_cmp(const void *a, const void *b)
{
	return uint64_cmp(&((const SampledRow *) a)->seq,
					  &((const SampledRow *) b)->seq);
}

/* ANALYZE's sample so far: a reservoir over the rows read, in order. */
typedef struct SampleState
{
	const ProllyComponents *c;
	Relation	rel;
	ProllyKeying *keying;
	ProllyRowIdScan *rowid;
	TupleTableSlot *slot;
	MemoryContext caller;		/* the sample's tuples live here */
	pg_prng_state rng;
	SampledRow *kept;
	int			targrows;
	int			nrows;			/* rows kept */
	int			nitems;			/* items read */
	uint64		nread;			/* rows read */
} SampleState;

/* Offer every row of an item to the reservoir. */
static void
sample_item(SampleState *st, const ProllyKey *key, const ProllyValue *value)
{
	ProllyValue row;
	uint64		count;

	PROLLY_CALL(st->c->keying, parse_value, st->keying, value, &row, &count);
	PROLLY_CALL(st->c->keying, decode_row, st->keying, &row, st->slot);
	st->nitems++;
	for (uint64 o = 0; o < count; o++, st->nread++)
	{
		int			slotno;
		MemoryContext old;

		if (st->nrows < st->targrows)
			slotno = st->nrows++;
		else
		{
			uint64		j = pg_prng_uint64_range(&st->rng, 0, st->nread);

			if (j >= (uint64) st->targrows)
				continue;
			slotno = (int) j;
			heap_freetuple(st->kept[slotno].tuple);
		}
		old = MemoryContextSwitchTo(st->caller);
		st->kept[slotno].tuple = ExecCopySlotHeapTuple(st->slot);
		MemoryContextSwitchTo(old);
		st->kept[slotno].seq = st->nread;
		if (!PROLLY_CALL(st->c->row_identity, tid_find, st->rowid, key, o,
						 &st->kept[slotno].tuple->t_self))
			ItemPointerSetInvalid(&st->kept[slotno].tuple->t_self);
		st->kept[slotno].tuple->t_tableOid = RelationGetRelid(st->rel);
	}
}

/*
 * ANALYZE's sample, returned in key order.
 *
 * A table with a primary key that the transaction hasn't written: random
 * items of the data tree, found by position from subtree counts, so only
 * they are read (each item is a row).
 *
 * A keyless table, whose items hold identical rows (a value can be most of
 * the table in one item, so picking items would rarely see it), or one the
 * transaction has written (positions don't cover its writes): every row,
 * as the transaction sees the table, offered to a reservoir.
 */
static int
prolly_acquire_sample_rows(Relation rel, int elevel, HeapTuple *rows,
						   int targrows, double *totalrows,
						   double *totaldeadrows)
{
	ProllyComponents c;
	MemoryContext tmp = AllocSetContextCreate(CurrentMemoryContext,
											  "prolly analyze",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContext caller = MemoryContextSwitchTo(tmp);
	ProllyRootSet *roots;
	bool		owned;
	ProllyCursor *cur;
	SampleState st;
	ProllyKey	key;
	ProllyValue value;
	uint64		nitems;
	bool		positional;

	prolly_components_for_relation(rel, &c);
	memset(&st, 0, sizeof(st));
	st.c = &c;
	st.rel = rel;
	st.caller = caller;
	st.targrows = targrows;
	st.kept = palloc(sizeof(SampledRow) * Max(targrows, 1));
	st.slot = MakeSingleTupleTableSlot(RelationGetDescr(rel), &TTSOpsVirtual);
	pg_prng_seed(&st.rng, pg_prng_uint64(&pg_global_prng_state));

	roots = PROLLY_CALL(c.engine, roots_acquire, NULL);
	st.keying = read_keying(rel, &c, roots, &owned);
	positional = !PROLLY_CALL(c.keying, keyless, st.keying) &&
		!PROLLY_CALL(c.engine, has_writes, rel, PROLLY_TREE_DATA);
	cur = PROLLY_CALL(c.engine, cursor_open, roots, rel, PROLLY_TREE_DATA,
					  InvalidCommandId);
	st.rowid = PROLLY_CALL(c.row_identity, scan_begin, roots, rel,
						   InvalidCommandId);

	if (positional)
	{
		uint64	   *picks = palloc(sizeof(uint64) * Max(targrows, 1));
		int			npicks = 0;

		nitems = PROLLY_CALL(c.engine, count_range, roots, rel,
							 PROLLY_TREE_DATA, NULL, NULL);

		/*
		 * targrows positions (or all), distinct, sorted.  With fewer than
		 * twice as many items, choose each in turn with the probability that
		 * keeps every subset equally likely; else draw at random, and draw
		 * again for repeats (each draw is new at least half the time).
		 */
		if (nitems <= (uint64) targrows * 2)
		{
			for (uint64 i = 0; i < nitems && npicks < targrows; i++)
				if (pg_prng_double(&st.rng) * (nitems - i) < (targrows - npicks))
					picks[npicks++] = i;
		}
		else
		{
			while (npicks < targrows)
			{
				int			n = npicks;

				while (n < targrows)
					picks[n++] = pg_prng_uint64_range(&st.rng, 0, nitems - 1);
				qsort(picks, n, sizeof(uint64), uint64_cmp);
				npicks = 0;
				for (int i = 0; i < n; i++)
					if (npicks == 0 || picks[npicks - 1] != picks[i])
						picks[npicks++] = picks[i];
			}
		}

		for (int i = 0; i < npicks; i++)
		{
			StringInfoData at;
			ProllyKey	k;

			vacuum_delay_point(true);
			initStringInfo(&at);
			if (!PROLLY_CALL(c.engine, key_at, roots, rel, picks[i], &at))
				elog(ERROR, "prolly table \"%s\" has no item at position " UINT64_FORMAT,
					 RelationGetRelationName(rel), picks[i]);
			k.data = (const uint8 *) at.data;
			k.len = at.len;
			PROLLY_CALL(c.engine, cursor_seek, cur, &k, ForwardScanDirection);
			if (!PROLLY_CALL(c.engine, cursor_next, cur, &key, &value))
				elog(ERROR, "prolly table \"%s\" has no item at position " UINT64_FORMAT,
					 RelationGetRelationName(rel), picks[i]);
			sample_item(&st, &key, &value);
			pfree(at.data);
		}
	}
	else
	{
		PROLLY_CALL(c.engine, cursor_seek, cur, NULL, ForwardScanDirection);
		while (PROLLY_CALL(c.engine, cursor_next, cur, &key, &value))
		{
			if ((st.nitems & 63) == 0)
				vacuum_delay_point(true);
			sample_item(&st, &key, &value);
		}
		nitems = st.nitems;
	}

	qsort(st.kept, st.nrows, sizeof(SampledRow), sampled_row_cmp);
	for (int i = 0; i < st.nrows; i++)
		rows[i] = st.kept[i].tuple;
	PROLLY_CALL(c.row_identity, scan_end, st.rowid);
	PROLLY_CALL(c.engine, cursor_close, cur);
	ExecDropSingleTupleTableSlot(st.slot);
	release_keying(&c, st.keying, owned);
	PROLLY_CALL(c.engine, roots_release, roots);

	/* Every row, or (one row per item) the item count. */
	*totalrows = positional ? (double) nitems : (double) st.nread;
	*totaldeadrows = 0;
	ereport(elevel,
			(errmsg("\"%s\": read %d of " UINT64_FORMAT " items, " UINT64_FORMAT " rows; %d rows in sample, %.0f estimated total rows",
					RelationGetRelationName(rel), st.nitems, nitems, st.nread,
					st.nrows, *totalrows)));
	MemoryContextSwitchTo(caller);
	MemoryContextDelete(tmp);
	return st.nrows;
}

/*
 * Bitmap scans: TIDs follow row identity's counter, so a page of the
 * bitmap is a range of TIDs; each is looked up as the scan's command sees
 * the table.  A lossy page's every TID is looked up, and rechecked.
 */
static bool
prolly_scan_bitmap_next_tuple(TableScanDesc sscan, TupleTableSlot *slot,
							  bool *recheck, uint64 *lossy_pages,
							  uint64 *exact_pages)
{
	ProllyScanDesc scan = (ProllyScanDesc) sscan;
	MemoryContext old = MemoryContextSwitchTo(scan->mcxt);

	if (scan->bm_row == NULL)
	{
		scan->bm_row = palloc(sizeof(RowAt));
		row_at_init(scan->bm_row);
	}
	for (;;)
	{
		ItemPointerData tid;

		CHECK_FOR_INTERRUPTS();
		if (!scan->bm_have_page || scan->bm_next >= scan->bm_noffsets)
		{
			if (!tbm_iterate(&sscan->st.rs_tbmiterator, &scan->bm_page))
			{
				MemoryContextSwitchTo(old);
				ExecClearTuple(slot);
				return false;
			}
			if (scan->bm_page.lossy)
			{
				scan->bm_noffsets = MaxHeapTuplesPerPage;
				for (int i = 0; i < MaxHeapTuplesPerPage; i++)
					scan->bm_offsets[i] = i + 1;
				(*lossy_pages)++;
			}
			else
			{
				scan->bm_noffsets = tbm_extract_page_tuple(&scan->bm_page,
														   scan->bm_offsets,
														   TBM_MAX_TUPLES_PER_PAGE);
				(*exact_pages)++;
			}
			*recheck = scan->bm_page.recheck || scan->bm_page.lossy;
			scan->bm_next = 0;
			scan->bm_have_page = true;
		}
		ItemPointerSet(&tid, scan->bm_page.blockno,
					   scan->bm_offsets[scan->bm_next++]);
		if (scan->bm_rekeys != prolly_rekeys)
		{
			/* The statement rebuilt the table: read the rebuilt rows. */
			PROLLY_CALL(scan->c.keying, close, scan->keying);
			scan->keying = open_read_keying(&scan->c, scan->roots,
											sscan->rs_rd, scan->mcxt);
			scan->bm_rekeys = prolly_rekeys;
		}
		row_at(&scan->c, scan->roots, sscan->rs_rd, scan->keying, &tid,
			   scan->cid, scan->bm_row);
		if (scan->bm_row->present)
		{
			store_row(&scan->c, sscan->rs_rd, scan->keying, scan->bm_row, &tid,
					  slot);
			pgstat_count_heap_fetch(sscan->rs_rd);
			MemoryContextSwitchTo(old);
			return true;
		}
	}
}

/* Items per pseudo-block of a sample scan. */
#define SAMPLE_BLOCK_ITEMS	64

/*
 * Read pseudo-block block's rows and TIDs: SAMPLE_BLOCK_ITEMS items from the
 * one at the block's first position, or, reading the table (it has the
 * transaction's writes, which positions don't cover), from where the cursor
 * is (going back to the start for an earlier block).
 */
static void
load_sample_block(ProllyScanDesc scan, BlockNumber block)
{
	Relation	rel = scan->base.rs_rd;
	ProllyKey	key;
	ProllyValue value;

	scan->sample_nrows = 0;
	scan->sample_chunk = 0;
	if (scan->sample_by_position)
	{
		StringInfoData at;
		ProllyKey	k;
		bool		found;

		initStringInfo(&at);
		found = PROLLY_CALL(scan->c.engine, key_at, scan->roots, rel,
							(uint64) block * SAMPLE_BLOCK_ITEMS, &at);
		k.data = (const uint8 *) at.data;
		k.len = at.len;
		if (found)
			PROLLY_CALL(scan->c.engine, cursor_seek, scan->cursor, &k,
						ForwardScanDirection);
		pfree(at.data);
		if (!found)
			return;
	}
	else
	{
		if (!BlockNumberIsValid(scan->sample_cursor_block) ||
			block < scan->sample_cursor_block)
		{
			PROLLY_CALL(scan->c.engine, cursor_seek, scan->cursor, NULL,
						ForwardScanDirection);
			scan->sample_cursor_block = 0;
		}
		for (uint64 skip = (uint64) (block - scan->sample_cursor_block) *
			 SAMPLE_BLOCK_ITEMS; skip > 0; skip--)
		{
			CHECK_FOR_INTERRUPTS();
			if (!PROLLY_CALL(scan->c.engine, cursor_next, scan->cursor, &key,
							 &value))
				break;
		}
		scan->sample_cursor_block = block + 1;
	}
	for (int i = 0; i < SAMPLE_BLOCK_ITEMS; i++)
	{
		ProllyValue row;
		uint64		count;

		if (!PROLLY_CALL(scan->c.engine, cursor_next, scan->cursor, &key, &value))
			break;
		PROLLY_CALL(scan->c.keying, parse_value, scan->keying, &value, &row, &count);
		for (uint64 o = 0; o < count; o++)
		{
			if (scan->sample_nrows == scan->sample_cap)
			{
				int			cap = scan->sample_cap ? scan->sample_cap * 2 : 128;

				scan->sample_rows = scan->sample_rows ?
					repalloc(scan->sample_rows, sizeof(StringInfoData) * cap) :
					palloc(sizeof(StringInfoData) * cap);
				scan->sample_tids = scan->sample_tids ?
					repalloc(scan->sample_tids, sizeof(ItemPointerData) * cap) :
					palloc(sizeof(ItemPointerData) * cap);
				for (int j = scan->sample_cap; j < cap; j++)
					initStringInfo(&scan->sample_rows[j]);
				scan->sample_cap = cap;
			}
			resetStringInfo(&scan->sample_rows[scan->sample_nrows]);
			appendBinaryStringInfo(&scan->sample_rows[scan->sample_nrows],
								   (const char *) row.data, row.len);
			if (!PROLLY_CALL(scan->c.row_identity, tid_find, scan->rowid, &key,
							 o, &scan->sample_tids[scan->sample_nrows]))
				ItemPointerSetInvalid(&scan->sample_tids[scan->sample_nrows]);
			scan->sample_nrows++;
		}
	}
}

/*
 * TABLESAMPLE: the table's items in key order make pseudo-blocks of
 * SAMPLE_BLOCK_ITEMS, found by position, so a method that picks blocks
 * (SYSTEM) reads only those.  If the transaction has written the table,
 * the blocks are cut while reading it (there are at least as many as its
 * items fill: the count may include rows its writes replaced or deleted).
 */
static bool
prolly_scan_sample_next_block(TableScanDesc sscan, SampleScanState *scanstate)
{
	ProllyScanDesc scan = (ProllyScanDesc) sscan;
	TsmRoutine *tsm = scanstate->tsmroutine;
	MemoryContext old = MemoryContextSwitchTo(scan->mcxt);
	BlockNumber block;

	if (!scan->sample_started)
	{
		uint64		n = PROLLY_CALL(scan->c.engine, count_range, scan->roots,
									sscan->rs_rd, PROLLY_TREE_DATA, NULL, NULL);

		scan->sample_by_position = !PROLLY_CALL(scan->c.engine, has_writes,
												sscan->rs_rd, PROLLY_TREE_DATA);
		scan->sample_nblocks = (BlockNumber)
			Min((n + SAMPLE_BLOCK_ITEMS - 1) / SAMPLE_BLOCK_ITEMS,
				(uint64) MaxBlockNumber);
		scan->sample_block = InvalidBlockNumber;
		scan->sample_cursor_block = InvalidBlockNumber;
		scan->sample_started = true;
	}
	if (scan->sample_nblocks == 0)
		block = InvalidBlockNumber;
	else if (tsm->NextSampleBlock)
		block = tsm->NextSampleBlock(scanstate, scan->sample_nblocks);
	else if (scan->sample_block == InvalidBlockNumber)
		block = 0;
	else if (scan->sample_block + 1 < scan->sample_nblocks)
		block = scan->sample_block + 1;
	else
		block = InvalidBlockNumber;

	scan->sample_block = block;
	if (!BlockNumberIsValid(block))
	{
		scan->sample_started = false;
		MemoryContextSwitchTo(old);
		return false;
	}
	CHECK_FOR_INTERRUPTS();
	load_sample_block(scan, block);
	MemoryContextSwitchTo(old);
	return true;
}

static bool
prolly_scan_sample_next_tuple(TableScanDesc sscan, SampleScanState *scanstate,
							  TupleTableSlot *slot)
{
	ProllyScanDesc scan = (ProllyScanDesc) sscan;
	TsmRoutine *tsm = scanstate->tsmroutine;

	/*
	 * A block can hold more rows than a page has offsets (identical rows);
	 * the method is shown them a page's worth at a time, each as a block of
	 * its own past the table's blocks, so that a method choosing rows by
	 * their block and offset chooses each chunk's afresh.
	 */
	for (;;)
	{
		int			left = scan->sample_nrows - scan->sample_chunk;
		OffsetNumber maxoff = (OffsetNumber) Min(left, MaxOffsetNumber);
		uint64		chunkno = scan->sample_chunk / MaxOffsetNumber;
		BlockNumber blockno = (BlockNumber)
			((scan->sample_block + chunkno * scan->sample_nblocks) %
			 ((uint64) MaxBlockNumber + 1));
		OffsetNumber off;
		int			i;
		ProllyValue row;

		CHECK_FOR_INTERRUPTS();
		off = tsm->NextSampleTuple(scanstate, blockno, maxoff);
		if (!OffsetNumberIsValid(off))
		{
			if (left > MaxOffsetNumber)
			{
				scan->sample_chunk += MaxOffsetNumber;
				continue;
			}
			ExecClearTuple(slot);
			return false;
		}
		if (off > maxoff)
			continue;
		i = scan->sample_chunk + off - 1;
		row.data = (const uint8 *) scan->sample_rows[i].data;
		row.len = scan->sample_rows[i].len;
		PROLLY_CALL(scan->c.keying, decode_row, scan->keying, &row, slot);
		slot->tts_tid = scan->sample_tids[i];
		slot->tts_tableOid = RelationGetRelid(sscan->rs_rd);
		return true;
	}
}

/* ------------------------------------------------------------------------
 * The routine and the handlers
 * ------------------------------------------------------------------------
 */

static const TableAmRoutine prolly_methods = {
	.type = T_TableAmRoutine,

	.slot_callbacks = prolly_slot_callbacks,

	.scan_begin = prolly_scan_begin,
	.scan_end = prolly_scan_end,
	.scan_rescan = prolly_scan_rescan,
	.scan_getnextslot = prolly_scan_getnextslot,

	.scan_set_tidrange = prolly_scan_set_tidrange,
	.scan_getnextslot_tidrange = prolly_scan_getnextslot_tidrange,

	/* no parallel scans */

	.index_scan_begin = prolly_index_scan_begin,
	.index_scan_reset = prolly_index_scan_reset,
	.index_scan_end = prolly_index_scan_end,
	.fetch_tid = prolly_fetch_tid,

	.tuple_fetch_row_version = prolly_tuple_fetch_row_version,
	.tuple_tid_valid = prolly_tuple_tid_valid,
	.tuple_get_latest_tid = prolly_tuple_get_latest_tid,
	.tuple_satisfies_snapshot = prolly_tuple_satisfies_snapshot,
	.index_delete_tuples = prolly_index_delete_tuples,

	.tuple_insert = prolly_tuple_insert,
	.tuple_insert_speculative = prolly_tuple_insert_speculative,
	.tuple_complete_speculative = prolly_tuple_complete_speculative,
	.multi_insert = prolly_multi_insert,
	.tuple_delete = prolly_tuple_delete,
	.tuple_update = prolly_tuple_update,
	.tuple_lock = prolly_tuple_lock,
	.finish_bulk_insert = prolly_finish_bulk_insert,

	.relation_set_new_filelocator = prolly_relation_set_new_filelocator,
	.relation_nontransactional_truncate = prolly_relation_nontransactional_truncate,
	.relation_copy_data = prolly_relation_copy_data,
	.relation_copy_for_cluster = prolly_relation_copy_for_cluster,
	.relation_vacuum = prolly_relation_vacuum,
	.scan_analyze_next_block = prolly_scan_analyze_next_block,
	.scan_analyze_next_tuple = prolly_scan_analyze_next_tuple,
	.relation_acquire_sample_rows = prolly_acquire_sample_rows,
	.index_build_range_scan = prolly_index_build_range_scan,
	.index_validate_scan = prolly_index_validate_scan,

	.relation_size = prolly_relation_size,
	.relation_needs_toast_table = prolly_relation_needs_toast_table,
	.relation_toast_am = prolly_relation_toast_am,
	.relation_fetch_toast_slice = prolly_relation_fetch_toast_slice,

	.relation_estimate_size = prolly_relation_estimate_size,

	.scan_bitmap_next_tuple = prolly_scan_bitmap_next_tuple,
	.scan_sample_next_block = prolly_scan_sample_next_block,
	.scan_sample_next_tuple = prolly_scan_sample_next_tuple,
};

/*
 * Every prolly table AM returns the same routine; the relation's AM name
 * selects the engine (prolly_config.c).
 */
Datum
prolly_d1_tableam_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&prolly_methods);
}

Datum
prolly_d2_tableam_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&prolly_methods);
}

Datum
prolly_d3a_tableam_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&prolly_methods);
}

Datum
prolly_d3b_tableam_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&prolly_methods);
}

Datum
prolly_d4_tableam_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&prolly_methods);
}

/*
 * prolly_node_counters(access_method name, OUT nodes_read int8,
 * OUT nodes_written int8): nodes this backend has read and written for the
 * engine of a prolly table access method.
 */
Datum
prolly_node_counters(PG_FUNCTION_ARGS)
{
	ProllyComponents c;
	ProllyNodeCounters counters;
	TupleDesc	tupdesc;
	Datum		values[2];
	bool		nulls[2] = {false, false};

	prolly_components_for_am(NameStr(*PG_GETARG_NAME(0)), &c);

	/* (page-based prolly keeps its trees as chunks, not in a node store) */
	if (c.engine == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("access method \"%s\" keeps no node counters",
						NameStr(*PG_GETARG_NAME(0))),
				 errdetail("Its tables are pages; their history is chunks, which no node store counts.")));
	PROLLY_CALL(c.engine, node_counters, &counters);
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	values[0] = Int64GetDatum((int64) counters.nodes_read);
	values[1] = Int64GetDatum((int64) counters.nodes_written);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}
