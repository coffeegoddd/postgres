/*-------------------------------------------------------------------------
 *
 * prolly_table.c
 *	  Table access method "prolly": heap's implementation, with the table,
 *	  its indexes and its TOAST table in block-mapped storage.
 *
 * A prolly table is versioned: each branch will have its own copy of it,
 * sharing pages copy-on-write, and commits will snapshot it.  Its pages
 * are heap's, read and written by heap's code, so the handler returns
 * heap's own routines: code that checks for heap by its routines (rather
 * than its OID) treats a prolly table as heap.  What differs is where the
 * pages live, which heap decides by the relation's access method when it
 * creates storage (heapam_handler.c).  Prolly tables are permanent:
 * temporary and unlogged tables are heap.
 *
 * Its rows' keys are bounded as they are written.  A commit folds every
 * row into a data tree, keyed as the table keying keys it, and a tree
 * holds keys of at most its key_max() bytes (about half a node); a row
 * with a longer key would make every commit of its branch fail, so it is
 * refused when it is written instead, as btree refuses an index row too
 * large for its page.  What is bounded is the key the tree would hold:
 * the key columns encoded, from their values detoasted and decompressed
 * (a btree holds them compressed, so takes keys a tree can't).  Rows keyed
 * by content have keys of a few bytes and a hash.  Heap checks each row
 * it writes (heap_insert(), heap_multi_insert(), heap_update()) and each
 * live row a primary key's index is built over; a primary key made of an
 * index built before checks the table's rows then
 * (prolly_check_table_keys()).
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_table.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/prolly_builtins.h"
#include "access/tableam.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

PG_FUNCTION_INFO_V1(prolly_tableam_handler);

Datum
prolly_tableam_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(GetHeapamTableAmRoutine());
}

/* ------------------------------------------------------------------------
 * Keys bounded
 * ------------------------------------------------------------------------
 */

/* A table's keying, its rows' keys encoded by: kept until invalidated. */
typedef struct KeyBound
{
	Oid			relid;			/* hash key */
	bool		valid;
	MemoryContext cxt;			/* all of it but the entry */
	const ProllyKeyingRoutine *kr;
	ProllyKeying *k;
	bool		keyless;
	TupleTableSlot *slot;		/* a row as written */
	MemoryContext scratch;		/* a key's encoding */
	Size		max;			/* the longest key a data tree holds */
} KeyBound;

static HTAB *key_bounds = NULL;
static uint64 key_bound_invals = 0;

static void
key_bounds_inval(Datum arg, Oid relid)
{
	HASH_SEQ_STATUS seq;
	KeyBound   *kb;

	key_bound_invals++;
	if (OidIsValid(relid))
	{
		kb = hash_search(key_bounds, &relid, HASH_FIND, NULL);
		if (kb)
			kb->valid = false;
		return;
	}
	hash_seq_init(&seq, key_bounds);
	while ((kb = hash_seq_search(&seq)) != NULL)
		kb->valid = false;
}

/*
 * rel's keying, opened anew if rel's relcache entry was invalidated since
 * (its primary key may be another).  An entry invalidated while in use is
 * freed at the next lookup, not then.
 */
static KeyBound *
key_bound(Relation rel)
{
	Oid			relid = RelationGetRelid(rel);
	KeyBound   *kb;
	KeyBound	made;
	ProllyComponents c;
	TupleDesc	desc;
	MemoryContext old;
	uint64		invals = key_bound_invals;
	bool		found;

	if (key_bounds == NULL)
	{
		HASHCTL		ctl = {0};

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(KeyBound);
		key_bounds = hash_create("prolly key bounds", 16, &ctl,
								 HASH_ELEM | HASH_BLOBS);
		CacheRegisterRelcacheCallback(key_bounds_inval, (Datum) 0);
	}
	kb = hash_search(key_bounds, &relid, HASH_FIND, NULL);
	if (kb && kb->valid)
		return kb;

	/*
	 * Made in the caller's memory and kept only once made, so an error on
	 * the way leaves nothing behind.
	 */
	memset(&made, 0, sizeof(made));
	made.relid = relid;
	made.cxt = AllocSetContextCreate(CurrentMemoryContext, "prolly key bound",
									 ALLOCSET_SMALL_SIZES);
	old = MemoryContextSwitchTo(made.cxt);
	prolly_components_for_relation(rel, &c);
	made.kr = c.keying;
	made.k = PROLLY_CALL(made.kr, open, rel, made.cxt);
	made.keyless = PROLLY_CALL(made.kr, keyless, made.k);

	/*
	 * (the relation's columns with their missing values: a row written
	 * before a column was added lacks it, and has its default)
	 */
	desc = CreateTupleDescCopyConstr(RelationGetDescr(rel));
	made.slot = MakeSingleTupleTableSlot(desc, &TTSOpsHeapTuple);
	made.scratch = AllocSetContextCreate(made.cxt, "prolly key",
										 ALLOCSET_DEFAULT_SIZES);
	/* (in the chunk format of folded trees, prolly_folded_tree_env()) */
	made.max = PROLLY_CALL(c.tree, key_max, prolly_chunkformat_lookup("basic"));
	MemoryContextSwitchTo(old);

	MemoryContextSetParent(made.cxt, CacheMemoryContext);
	kb = hash_search(key_bounds, &relid, HASH_ENTER, &found);
	if (found)
		MemoryContextDelete(kb->cxt);
	*kb = made;
	/* (an invalidation while it was made may have come too late for it) */
	kb->valid = (key_bound_invals == invals);
	return kb;
}

void
prolly_check_row_key(Relation rel, HeapTuple tuple)
{
	KeyBound   *kb = key_bound(rel);
	MemoryContext old;
	StringInfoData key;
	Size		len;

	if (kb->keyless)
		return;
	MemoryContextReset(kb->scratch);
	old = MemoryContextSwitchTo(kb->scratch);
	ExecStoreHeapTuple(tuple, kb->slot, false);
	initStringInfo(&key);
	(void) PROLLY_CALL(kb->kr, encode_key, kb->k, kb->slot, &key);
	ExecClearTuple(kb->slot);
	MemoryContextSwitchTo(old);
	len = key.len;
	MemoryContextReset(kb->scratch);

	if (len > kb->max)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("primary key of %zu bytes exceeds the maximum %zu for prolly table \"%s\"",
						len, kb->max, RelationGetRelationName(rel)),
				 errdetail("A commit keys each row by its primary key, encoded and uncompressed, and holds keys of at most %zu bytes.",
						   kb->max),
				 errhint("Key the table by a shorter value, such as a hash of this one.")));
}

void
prolly_check_table_keys(Relation rel)
{
	Snapshot	snap = RegisterSnapshot(GetLatestSnapshot());
	TableScanDesc scan = table_beginscan(rel, snap, 0, NULL, SO_NONE);
	TupleTableSlot *slot = table_slot_create(rel, NULL);

	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		bool		should_free;
		HeapTuple	tuple = ExecFetchSlotHeapTuple(slot, false, &should_free);

		prolly_check_row_key(rel, tuple);
		if (should_free)
			heap_freetuple(tuple);
	}
	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
	UnregisterSnapshot(snap);
}
