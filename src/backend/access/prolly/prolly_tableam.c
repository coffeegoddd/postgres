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

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "access/multixact.h"
#include "access/prolly_config.h"
#include "access/relscan.h"
#include "access/skey.h"
#include "access/tableam.h"
#include "access/tableam_indexscan.h"
#include "catalog/index.h"
#include "catalog/storage.h"
#include "commands/defrem.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "storage/proc.h"
#include "optimizer/plancat.h"
#include "utils/fmgrprotos.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"
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

	/* The current item: its key and row, and identical rows still to return. */
	StringInfoData key;
	StringInfoData row;
	uint64		count;
	uint64		next_ordinal;

	/* TID range scans: rows whose TIDs are in [mintid, maxtid]. */
	ItemPointerData mintid;
	ItemPointerData maxtid;
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
	scan->next_ordinal = 0;
	PROLLY_CALL(scan->c.engine, cursor_seek, scan->cursor, NULL,
				ForwardScanDirection);
}

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
	scan->keying = open_read_keying(&scan->c, scan->roots, rel, scan->mcxt);
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
	scan_start(scan);
}

static bool
prolly_scan_getnextslot(TableScanDesc sscan, ScanDirection direction,
						TupleTableSlot *slot)
{
	ProllyScanDesc scan = (ProllyScanDesc) sscan;
	const ProllyKeyingRoutine *keying = scan->c.keying;
	ProllyValue row;

	if (ScanDirectionIsBackward(direction))
		not_implemented(sscan->rs_rd, "backward scans");

	if (scan->next_ordinal >= scan->count)
	{
		ProllyKey	key;
		ProllyValue value;
		MemoryContext old;

		if (!PROLLY_CALL(scan->c.engine, cursor_next, scan->cursor, &key, &value))
		{
			ExecClearTuple(slot);
			return false;
		}
		old = MemoryContextSwitchTo(scan->mcxt);
		PROLLY_CALL(keying, parse_value, scan->keying, &value, &row, &scan->count);
		resetStringInfo(&scan->key);
		appendBinaryStringInfo(&scan->key, (const char *) key.data, key.len);
		resetStringInfo(&scan->row);
		appendBinaryStringInfo(&scan->row, (const char *) row.data, row.len);
		MemoryContextSwitchTo(old);
		scan->next_ordinal = 0;
	}

	row.data = (const uint8 *) scan->row.data;
	row.len = scan->row.len;
	PROLLY_CALL(keying, decode_row, scan->keying, &row, slot);
	{
		ProllyKey	k = {(const uint8 *) scan->key.data, scan->key.len};

		if (!PROLLY_CALL(scan->c.row_identity, tid_find, scan->rowid, &k,
						 scan->next_ordinal, &slot->tts_tid))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("a row of \"%s\" has no TID",
							RelationGetRelationName(sscan->rs_rd))));
	}
	slot->tts_tableOid = RelationGetRelid(sscan->rs_rd);
	scan->next_ordinal++;
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

	if (ScanDirectionIsBackward(direction))
		not_implemented(sscan->rs_rd, "backward TID range scans");
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
 * Get the right to write key of rel (NULL: all of rel) before writing it,
 * from the coordination module.  Getting it may have meant waiting for
 * another writer to commit, so *roots is replaced by the newest: whatever
 * the write reads (an existing row, a count) must be what it builds on.
 */
static void
lock_for_write(Relation rel, const ProllyComponents *c, const ProllyKey *key,
			   ProllyRootSet **roots)
{
	TM_FailureData tmfd;

	if (PROLLY_CALL(c->coord, lock_row, rel, key, LockTupleExclusive,
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

static void insert_row(Relation rel, const ProllyComponents *c,
					   ProllyKeying *keying, ProllyRootSet **roots,
					   TupleTableSlot *slot, uint64 copies,
					   const ItemPointerData *renamed, bool replaces,
					   CommandId cid);

/*
 * Rebuild rel's data tree, written with the keying old describes, under
 * rel's keying now.  Rows are collected first: they are read from the
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

	if (ik->checked_lxid == MyProc->vxid.lxid &&
		ik->checked_subxid == GetCurrentSubTransactionId())
		return;

	initStringInfo(&recorded);
	initStringInfo(&now);
	PROLLY_CALL(c->keying, describe, ik->keying, &now);
	if (!recorded_keying(c, *roots, rel, &recorded) ||
		recorded.len != now.len ||
		memcmp(recorded.data, now.data, now.len) != 0)
	{
		/* Writing the whole tree: lock it, and look again once locked. */
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
		if (recorded.len > 0 && !PROLLY_CALL(c->keying, can_read, ik->keying, &r))
		{
			ProllyValue d = {(const uint8 *) recorded.data, recorded.len};
			ProllyKeying *old = PROLLY_CALL(c->keying, open, rel, &d,
											CurrentMemoryContext);

			rekey(rel, c, roots, old, ik->keying);
			PROLLY_CALL(c->keying, close, old);
		}
		PROLLY_CALL(c->engine, put, rel, PROLLY_TREE_META, &key, &v,
					InvalidCommandId);
	}
done:
	pfree(recorded.data);
	pfree(now.data);
	ik->checked_lxid = MyProc->vxid.lxid;
	ik->checked_subxid = GetCurrentSubTransactionId();
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
	lock_for_write(rel, c, &k, roots);

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
 * row_at() for a dirty snapshot (uniqueness and exclusion checks): a TID
 * nobody knows may name another writer's row, private until it commits, so
 * wait for the right to write rel (which the caller, about to write, needs
 * anyway) and look again with what was committed meanwhile.  Heap reports
 * the inserter for its caller to wait for instead.
 */
static void
row_at_dirty(const ProllyComponents *c, ProllyRootSet **roots, Relation rel,
			 ProllyKeying *keying, ItemPointer tid, RowAt *r)
{
	row_at(c, *roots, rel, keying, tid, InvalidCommandId, r);
	if (r->state == PROLLY_TID_UNKNOWN)
	{
		lock_for_write(rel, c, NULL, roots);
		row_at(c, *roots, rel, keying, tid, InvalidCommandId, r);
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

/* Lock the row tid names for writing, then check rel's keying. */
static void
prepare_row_write(Relation rel, const ProllyComponents *c, ItemPointer tid,
				  InsertKeying **ik_out, ProllyRootSet **roots_out)
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
		lock_for_write(rel, c, &k, &roots);
	}
	else
		lock_for_write(rel, c, NULL, &roots);
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
	prepare_row_write(rel, &c, tid, &ik, &roots);
	row_at_init(&now);
	fate = row_fate(&c, roots, rel, ik->keying, tid, cid, &now, tmfd);
	if (fate == ROW_LIVE)
		remove_version(&c, roots, rel, ik->keying, tid, &now, NULL, false, cid);
	row_at_free(&now);
	PROLLY_CALL(c.engine, roots_release, roots);
	return fate_result(fate, tmfd, tid);
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
	*lockmode = LockTupleExclusive;
	*update_indexes = TU_None;
	prepare_row_write(rel, &c, otid, &ik, &roots);
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
			bool		same_key = key.len == now.key.len &&
				memcmp(key.data, now.key.data, key.len) == 0;
			ItemPointerData ntid;

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

	prolly_components_for_relation(rel, &c);
	roots = PROLLY_CALL(c.engine, roots_acquire, NULL);
	initStringInfo(&key);
	if (PROLLY_CALL(c.row_identity, tid_resolve, roots, rel, tid,
					InvalidCommandId, &key, &content, &ordinal,
					&info) == PROLLY_TID_NAMED)
	{
		k.data = (const uint8 *) key.data;
		k.len = key.len;
		result = PROLLY_CALL(c.coord, lock_row, rel, &k, mode, wait_policy, tmfd);
	}
	else
		result = PROLLY_CALL(c.coord, lock_row, rel, NULL, mode, wait_policy,
							 tmfd);
	pfree(key.data);
	PROLLY_CALL(c.engine, roots_release, roots);
	if (result != TM_Ok)
	{
		tmfd->ctid = *tid;
		tmfd->traversed = false;
		return result;
	}

	roots = PROLLY_CALL(c.engine, roots_acquire, NULL);
	keying = read_keying(rel, &c, roots, &owned);
	row_at_init(&now);
	fate = row_fate(&c, roots, rel, keying, tid, cid, &now, tmfd);
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
				!ItemPointerIsValid(&now.info.successor))
				break;
			next = now.info.successor;
		}
		if (now.present)
		{
			*tid = next;
			fate = ROW_LIVE;
			tmfd->traversed = true;
		}
		else
			tmfd->ctid = *tid;	/* deleted at the end of the chain */
	}
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
	prepare_row_write(rel, &c, &slot->tts_tid, &ik, &roots);
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
		row_at_dirty(&c, &roots, rel, keying, tid, &r);
	}
	else
		row_at(&c, roots, rel, keying, tid, snapshot_cid(snapshot), &r);
	found = r.present;
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
			!ItemPointerIsValid(&r.info.successor))
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
		(void) snapshot_cid(scan->xs_snapshot);
		if (scan->xs_snapshot->snapshot_type == SNAPSHOT_DIRTY)
			row_at_dirty(&is->c, &is->roots, rel, is->keying, &scan->xs_heaptid,
						 &is->row);
		else
			row_at(&is->c, is->roots, rel, is->keying, &scan->xs_heaptid,
				   is->cid, &is->row);
		found = is->row.present;
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
	is->keying = read_keying(scan->heapRelation, &is->c, is->roots, &is->owned);
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

static void
prolly_relation_copy_data(Relation rel, const RelFileLocator *newrlocator)
{
	ProllyComponents components;
	const ProllyEngineRoutine *engine;
	SMgrRelation dstrel;

	prolly_components_for_relation(rel, &components);
	engine = components.engine;
	PROLLY_CALL(engine, tree_copy, rel, newrlocator);

	/* The main fork holds the components' per-table pages (TID counter). */
	FlushRelationBuffers(rel);
	dstrel = RelationCreateStorage(*newrlocator, rel->rd_rel->relpersistence,
								   true);
	RelationCopyStorage(RelationGetSmgr(rel), dstrel, MAIN_FORKNUM,
						rel->rd_rel->relpersistence);
	smgrclose(dstrel);
}

static void
prolly_relation_copy_for_cluster(Relation OldTable, Relation NewTable,
								 Relation OldIndex, bool use_sort,
								 TransactionId OldestXmin, Snapshot snapshot,
								 TransactionId *xid_cutoff,
								 MultiXactId *multi_cutoff,
								 double *num_tuples, double *tups_vacuumed,
								 double *tups_recently_dead)
{
	not_implemented(OldTable, "relation_copy_for_cluster");
}

static void
prolly_relation_vacuum(Relation rel, const VacuumParams *params,
					   BufferAccessStrategy bstrategy)
{
	ProllyComponents components;
	const ProllyEngineRoutine *engine;

	prolly_components_for_relation(rel, &components);
	engine = components.engine;
	PROLLY_CALL(engine, vacuum, rel, params, bstrategy);
}

/* ------------------------------------------------------------------------
 * ANALYZE, index builds, sizes and sampling
 * ------------------------------------------------------------------------
 */

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

	width = get_rel_data_width(rel, attr_widths) + 8;
	*tuples = (double) n;
	*pages = n == 0 ? 0 :
		(BlockNumber) Max(1, ((double) n * width) / (BLCKSZ * 0.9));
	*allvisfrac = 0;
}

static bool
prolly_scan_bitmap_next_tuple(TableScanDesc scan, TupleTableSlot *slot,
							  bool *recheck, uint64 *lossy_pages,
							  uint64 *exact_pages)
{
	not_implemented(scan->rs_rd, "scan_bitmap_next_tuple");
}

static bool
prolly_scan_sample_next_block(TableScanDesc scan, SampleScanState *scanstate)
{
	not_implemented(scan->rs_rd, "scan_sample_next_block");
}

static bool
prolly_scan_sample_next_tuple(TableScanDesc scan, SampleScanState *scanstate,
							  TupleTableSlot *slot)
{
	not_implemented(scan->rs_rd, "scan_sample_next_tuple");
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
