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

#include "access/multixact.h"
#include "access/prolly_config.h"
#include "access/relscan.h"
#include "access/skey.h"
#include "access/tableam.h"
#include "catalog/index.h"
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

	/* The current item: its row, and identical rows still to return. */
	StringInfoData row;
	ProllyHash	content;
	uint64		count;
	uint64		next_ordinal;
} ProllyScanDescData;

typedef ProllyScanDescData *ProllyScanDesc;

static const TupleTableSlotOps *
prolly_slot_callbacks(Relation rel)
{
	return &TTSOpsVirtual;
}

/* The command whose view a snapshot gives: all writes, if not MVCC. */
static CommandId
snapshot_cid(Snapshot snapshot)
{
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
		resetStringInfo(&scan->row);
		appendBinaryStringInfo(&scan->row, (const char *) row.data, row.len);
		row.data = (const uint8 *) scan->row.data;
		if (PROLLY_CALL(keying, keyless, scan->keying))
		{
			/* A keyless table's key is the content hash. */
			scan->content.len = key.len;
			memcpy(scan->content.bytes, key.data, key.len);
		}
		else
			PROLLY_CALL(keying, content_hash, scan->keying, &row, &scan->content);
		MemoryContextSwitchTo(old);
		scan->next_ordinal = 0;
	}

	row.data = (const uint8 *) scan->row.data;
	row.len = scan->row.len;
	PROLLY_CALL(keying, decode_row, scan->keying, &row, slot);
	(void) PROLLY_CALL(scan->c.row_identity, tid_find, scan->rowid,
					   &scan->content, scan->next_ordinal, &slot->tts_tid);
	slot->tts_tableOid = RelationGetRelid(sscan->rs_rd);
	scan->next_ordinal++;
	return true;
}

static void
prolly_scan_set_tidrange(TableScanDesc scan, ItemPointer mintid,
						 ItemPointer maxtid)
{
	not_implemented(scan->rs_rd, "scan_set_tidrange");
}

static bool
prolly_scan_getnextslot_tidrange(TableScanDesc scan, ScanDirection direction,
								 TupleTableSlot *slot)
{
	not_implemented(scan->rs_rd, "scan_getnextslot_tidrange");
}

/* ------------------------------------------------------------------------
 * Index scans and fetching rows by TID
 * ------------------------------------------------------------------------
 */

static void
prolly_index_scan_begin(IndexScanDesc scan, uint32 flags)
{
	not_implemented(scan->heapRelation, "index_scan_begin");
}

static void
prolly_index_scan_reset(IndexScanDesc scan)
{
	not_implemented(scan->heapRelation, "index_scan_reset");
}

static void
prolly_index_scan_end(IndexScanDesc scan)
{
	not_implemented(scan->heapRelation, "index_scan_end");
}

static bool
prolly_fetch_tid(Relation rel, ItemPointer tid, Snapshot snapshot,
				 bool *all_dead)
{
	not_implemented(rel, "fetch_tid");
}

static bool
prolly_tuple_fetch_row_version(Relation rel, ItemPointer tid,
							   Snapshot snapshot, TupleTableSlot *slot)
{
	not_implemented(rel, "tuple_fetch_row_version");
}

static bool
prolly_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
	not_implemented(scan->rs_rd, "tuple_tid_valid");
}

static void
prolly_tuple_get_latest_tid(TableScanDesc scan, ItemPointer tid)
{
	not_implemented(scan->rs_rd, "tuple_get_latest_tid");
}

static bool
prolly_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
								Snapshot snapshot)
{
	not_implemented(rel, "tuple_satisfies_snapshot");
}

static TransactionId
prolly_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
	not_implemented(rel, "index_delete_tuples");
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
	ik = hash_search(insert_keyings, &relid, HASH_ENTER, &found);
	if (found && ik->valid && ik->relnumber == rel->rd_locator.relNumber)
		return ik;
	if (found)
	{
		PROLLY_CALL(ik->routine, close, ik->keying);
		MemoryContextDelete(ik->mcxt);
	}
	ik->mcxt = AllocSetContextCreate(CacheMemoryContext, "prolly insert keying",
									 ALLOCSET_SMALL_SIZES);
	ik->routine = c->keying;
	ik->keying = PROLLY_CALL(c->keying, open, rel, NULL, ik->mcxt);
	ik->relnumber = rel->rd_locator.relNumber;
	ik->valid = true;
	ik->checked_lxid = InvalidLocalTransactionId;
	ik->checked_subxid = InvalidSubTransactionId;
	return ik;
}

static void insert_row(Relation rel, const ProllyComponents *c,
					   ProllyKeying *keying, ProllyRootSet *roots,
					   TupleTableSlot *slot, uint64 copies,
					   const ProllyHash *renamed_from, CommandId cid);

/*
 * Rebuild rel's data tree, written with the keying old describes, under
 * rel's keying now.  Rows are collected first: they are read from the
 * tree being replaced.  The rebuilt rows are tagged as written by the
 * transaction's first command, so every later command (and this one) sees
 * the table's rows as before.  A row whose bytes changed (a dropped
 * column's value is gone) keeps its TID.
 */
static void
rekey(Relation rel, const ProllyComponents *c, ProllyRootSet *roots,
	  ProllyKeying *old, ProllyKeying *now)
{
	MemoryContext tmp = AllocSetContextCreate(CurrentMemoryContext,
											  "prolly rekey", ALLOCSET_DEFAULT_SIZES);
	MemoryContext oldcxt = MemoryContextSwitchTo(tmp);
	ProllyCursor *cur;
	ProllyKey	key;
	ProllyValue value;
	List	   *rows = NIL;
	List	   *counts = NIL;
	TupleTableSlot *slot = MakeSingleTupleTableSlot(RelationGetDescr(rel),
													&TTSOpsVirtual);
	ListCell   *lr;
	ListCell   *lc;

	cur = PROLLY_CALL(c->engine, cursor_open, roots, rel, PROLLY_TREE_DATA,
					  InvalidCommandId);
	PROLLY_CALL(c->engine, cursor_seek, cur, NULL, ForwardScanDirection);
	while (PROLLY_CALL(c->engine, cursor_next, cur, &key, &value))
	{
		ProllyValue row;
		uint64		count;
		StringInfo	copy = makeStringInfo();
		uint64	   *n = palloc(sizeof(uint64));

		PROLLY_CALL(c->keying, parse_value, old, &value, &row, &count);
		appendBinaryStringInfo(copy, (const char *) row.data, row.len);
		*n = count;
		rows = lappend(rows, copy);
		counts = lappend(counts, n);
	}
	PROLLY_CALL(c->engine, cursor_close, cur);

	PROLLY_CALL(c->engine, tree_reset, rel, PROLLY_TREE_DATA);
	forboth(lr, rows, lc, counts)
	{
		StringInfo	r = lfirst(lr);
		ProllyValue row = {(const uint8 *) r->data, r->len};

		ProllyHash	old_content;

		PROLLY_CALL(c->keying, content_hash, old, &row, &old_content);
		PROLLY_CALL(c->keying, decode_row, old, &row, slot);
		insert_row(rel, c, now, roots, slot, *(uint64 *) lfirst(lc),
				   &old_content, FirstCommandId);
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
check_keying(Relation rel, const ProllyComponents *c, ProllyRootSet *roots,
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
	if (!recorded_keying(c, roots, rel, &recorded) ||
		recorded.len != now.len ||
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
					FirstCommandId);
	}
	pfree(recorded.data);
	pfree(now.data);
	ik->checked_lxid = MyProc->vxid.lxid;
	ik->checked_subxid = GetCurrentSubTransactionId();
}

/*
 * Write copies identical rows of slot's row, and set slot's TID to the
 * last one's.  renamed_from, if set, is the content hash the rows had
 * before they were re-encoded: they keep their TIDs.
 */
static void
insert_row(Relation rel, const ProllyComponents *c, ProllyKeying *keying,
		   ProllyRootSet *roots, TupleTableSlot *slot, uint64 copies,
		   const ProllyHash *renamed_from, CommandId cid)
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

	if (PROLLY_CALL(c->keying, keyless, keying))
	{
		StringInfoData old;

		/* More of these rows; see this command's own inserts too. */
		initStringInfo(&old);
		if (PROLLY_CALL(c->engine, get, roots, rel, PROLLY_TREE_DATA, &k,
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
	PROLLY_CALL(c->keying, make_value, keying, &row, count + copies, &value);
	v.data = (const uint8 *) value.data;
	v.len = value.len;
	PROLLY_CALL(c->engine, put, rel, PROLLY_TREE_DATA, &k, &v, cid);

	/* The new rows are ordinals count .. count + copies - 1. */
	for (uint64 i = 0; i < copies; i++)
	{
		if (renamed_from &&
			(renamed_from->len != content.len ||
			 memcmp(renamed_from->bytes, content.bytes, content.len) != 0 ||
			 count != 0))
			PROLLY_CALL(c->row_identity, tid_rename, roots, rel, renamed_from,
						i, &content, count + i, &k, cid);
		else
			PROLLY_CALL(c->row_identity, tid_assign, roots, rel, &content,
						count + i, &k, cid, &slot->tts_tid);
	}
	slot->tts_tableOid = RelationGetRelid(rel);

	pfree(key.data);
	pfree(rowbuf.data);
	pfree(value.data);
}

static void
insert_one(Relation rel, const ProllyComponents *c, ProllyRootSet *roots,
		   TupleTableSlot *slot, CommandId cid)
{
	InsertKeying *ik = insert_keying(rel, c);

	check_keying(rel, c, roots, ik);
	insert_row(rel, c, ik->keying, roots, slot, 1, NULL, cid);
}

static void
prolly_tuple_insert(Relation rel, TupleTableSlot *slot, CommandId cid,
					uint32 options, BulkInsertStateData *bistate)
{
	ProllyComponents c;
	ProllyRootSet *roots;

	prolly_components_for_relation(rel, &c);
	roots = PROLLY_CALL(c.engine, roots_acquire, NULL);
	insert_one(rel, &c, roots, slot, cid);
	PROLLY_CALL(c.engine, roots_release, roots);
}

static void
prolly_tuple_insert_speculative(Relation rel, TupleTableSlot *slot,
								CommandId cid, uint32 options,
								BulkInsertStateData *bistate, uint32 specToken)
{
	not_implemented(rel, "tuple_insert_speculative");
}

static void
prolly_tuple_complete_speculative(Relation rel, TupleTableSlot *slot,
								  uint32 specToken, bool succeeded)
{
	not_implemented(rel, "tuple_complete_speculative");
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
		insert_one(rel, &c, roots, slots[i], cid);
	PROLLY_CALL(c.engine, roots_release, roots);
}

static TM_Result
prolly_tuple_delete(Relation rel, ItemPointer tid, CommandId cid,
					uint32 options, Snapshot snapshot, Snapshot crosscheck,
					bool wait, TM_FailureData *tmfd)
{
	not_implemented(rel, "tuple_delete");
}

static TM_Result
prolly_tuple_update(Relation rel, ItemPointer otid, TupleTableSlot *slot,
					CommandId cid, uint32 options, Snapshot snapshot,
					Snapshot crosscheck, bool wait, TM_FailureData *tmfd,
					LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes)
{
	not_implemented(rel, "tuple_update");
}

static TM_Result
prolly_tuple_lock(Relation rel, ItemPointer tid, Snapshot snapshot,
				  TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
				  LockWaitPolicy wait_policy, uint8 flags,
				  TM_FailureData *tmfd)
{
	not_implemented(rel, "tuple_lock");
}

static void
prolly_finish_bulk_insert(Relation rel, uint32 options)
{
	/* Writes stay private until commit; nothing to flush. */
}

/* ------------------------------------------------------------------------
 * Relation storage: these map directly onto the engine interface.
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

	prolly_components_for_relation(rel, &components);
	engine = components.engine;
	PROLLY_CALL(engine, tree_copy, rel, newrlocator);
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

static void
prolly_index_validate_scan(Relation table_rel, Relation index_rel,
						   IndexInfo *index_info, Snapshot snapshot,
						   ValidateIndexState *state)
{
	not_implemented(table_rel, "index_validate_scan");
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
