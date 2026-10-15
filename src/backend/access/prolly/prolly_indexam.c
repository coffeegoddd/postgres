/*-------------------------------------------------------------------------
 *
 * prolly_indexam.c
 *	  Index access method adapter for prolly index trees.
 *
 * A prolly index is a second tree from the same root set as its table,
 * made by the index keying (prolly_ixkeying.h): one entry per row, the
 * index columns then the row's key in its table and its ordinal, with no
 * TIDs, so the tree depends only on the table's rows.  Its operator
 * classes follow the btree operator class contract exactly, since key
 * encodings must agree with btree ordering; so validation reuses
 * btvalidate().  Prolly indexes exist only on prolly tables.
 *
 * Postgres calls an index AM to insert entries; the prolly table AM also
 * has it remove the entries of a row version that goes, and rebuild when
 * the table is re-keyed (prolly_indexam.h), so the index always holds
 * exactly the table's rows.
 *
 * Scans walk the tree through an engine cursor, from a bound made of the
 * scan's equality keys on leading columns (and a range key on the next),
 * to a bound past them; every scan key is checked on the decoded entry.
 * A row key and ordinal become a TID through row identity.  Entries are
 * visible as the scan's command sees the table, so the table AM's check
 * of each row agrees.
 *
 * Not yet: bitmap scans, INCLUDE columns, parallel scans and builds.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_indexam.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/genam.h"
#include "access/nbtree.h"
#include "access/prolly_config.h"
#include "access/prolly_indexam.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/pg_am_d.h"
#include "commands/defrem.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "executor/executor.h"
#include "nodes/tidbitmap.h"
#include "utils/fmgrprotos.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"

pg_noreturn static void
not_implemented(const char *callback)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("index access method \"%s\" does not implement %s yet",
					"prolly", callback)));
	pg_unreachable();
}

static IndexBuildResult *prolly_ambuild(Relation heapRelation,
										Relation indexRelation,
										IndexInfo *indexInfo);

/* ------------------------------------------------------------------------
 * Keyings, cached per index
 * ------------------------------------------------------------------------
 */

typedef struct IndexKeyingEntry
{
	Oid			indexoid;		/* hash key */
	RelFileNumber relnumber;
	bool		valid;
	MemoryContext mcxt;
	const ProllyIndexKeyingRoutine *routine;
	ProllyIndexKeying *keying;
} IndexKeyingEntry;

static HTAB *index_keyings = NULL;

static void
index_keying_invalidate(Datum arg, Oid relid)
{
	HASH_SEQ_STATUS status;
	IndexKeyingEntry *e;

	if (index_keyings == NULL)
		return;
	if (OidIsValid(relid))
	{
		e = hash_search(index_keyings, &relid, HASH_FIND, NULL);
		if (e)
			e->valid = false;
		return;
	}
	hash_seq_init(&status, index_keyings);
	while ((e = hash_seq_search(&status)) != NULL)
		e->valid = false;
}

/*
 * The keying of index as it is now.  An invalidated entry is only marked
 * and reopened at the next lookup, so a keying in use isn't freed.
 */
static ProllyIndexKeying *
index_keying(Relation index, const ProllyComponents *c)
{
	Oid			oid = RelationGetRelid(index);
	IndexKeyingEntry *e;
	bool		found;
	MemoryContext mcxt;
	ProllyIndexKeying *k;

	if (index_keyings == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(IndexKeyingEntry);
		ctl.hcxt = CacheMemoryContext;
		index_keyings = hash_create("prolly index keyings", 32, &ctl,
									HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		CacheRegisterRelcacheCallback(index_keying_invalidate, (Datum) 0);
	}
	e = hash_search(index_keyings, &oid, HASH_FIND, &found);
	if (found && e->valid && e->relnumber == index->rd_locator.relNumber)
		return e->keying;

	mcxt = AllocSetContextCreate(CurrentMemoryContext, "prolly index keying",
								 ALLOCSET_SMALL_SIZES);
	k = PROLLY_CALL(c->index_keying, open, index, NULL, mcxt);
	MemoryContextSetParent(mcxt, CacheMemoryContext);
	if (found)
	{
		PROLLY_CALL(e->routine, close, e->keying);
		MemoryContextDelete(e->mcxt);
	}
	else
		e = hash_search(index_keyings, &oid, HASH_ENTER, NULL);
	e->mcxt = mcxt;
	e->routine = c->index_keying;
	e->keying = k;
	e->relnumber = index->rd_locator.relNumber;
	e->valid = true;
	return k;
}

/* ------------------------------------------------------------------------
 * Entries
 * ------------------------------------------------------------------------
 */

static bool
is_prolly_table(Relation heap)
{
	return prolly_is_table_am(get_am_name(heap->rd_rel->relam));
}

bool
prolly_index_is_prolly(Relation index)
{
	return index->rd_indam != NULL &&
		index->rd_indam->ambuild == prolly_ambuild;
}

/* The row key and ordinal of the row version tid names. */
static void
row_of_tid(const ProllyComponents *c, ProllyRootSet *roots, Relation heap,
		   ItemPointer tid, StringInfo rowkey, uint64 *ordinal)
{
	ProllyHash	content;
	ProllyTidInfo info;

	if (PROLLY_CALL(c->row_identity, tid_resolve, roots, heap, tid,
					InvalidCommandId, rowkey, &content, ordinal,
					&info) != PROLLY_TID_NAMED)
		elog(ERROR, "TID (%u,%u) of \"%s\" names no row to index",
			 ItemPointerGetBlockNumber(tid), ItemPointerGetOffsetNumber(tid),
			 RelationGetRelationName(heap));
}

static void
record_keying(const ProllyComponents *c, Relation index, ProllyIndexKeying *k)
{
	StringInfoData desc;
	ProllyKey	key = {(const uint8 *) PROLLY_META_KEYING, PROLLY_META_KEYING_LEN};
	ProllyValue v;

	initStringInfo(&desc);
	PROLLY_CALL(c->index_keying, describe, k, &desc);
	v.data = (const uint8 *) desc.data;
	v.len = desc.len;
	PROLLY_CALL(c->engine, put, index, PROLLY_TREE_META, &key, &v,
				InvalidCommandId);
	pfree(desc.data);
}

/* Does a row with these key values take part in uniqueness? */
static bool
checks_unique(Relation index, int nkeys, const bool *isnull)
{
	if (index->rd_index->indnullsnotdistinct)
		return true;
	for (int i = 0; i < nkeys; i++)
		if (isnull[i])
			return false;
	return true;
}

static void
report_duplicate(Relation index, Relation heap, const Datum *values,
				 const bool *isnull, bool building)
{
	char	   *desc = BuildIndexValueDescription(index, (Datum *) values,
												  (bool *) isnull);

	if (building)
		ereport(ERROR,
				(errcode(ERRCODE_UNIQUE_VIOLATION),
				 errmsg("could not create unique index \"%s\"",
						RelationGetRelationName(index)),
				 desc ? errdetail("Key %s is duplicated.", desc) :
				 errdetail("Duplicate keys exist."),
				 errtableconstraint(heap, RelationGetRelationName(index))));
	ereport(ERROR,
			(errcode(ERRCODE_UNIQUE_VIOLATION),
			 errmsg("duplicate key value violates unique constraint \"%s\"",
					RelationGetRelationName(index)),
			 desc ? errdetail("Key %s already exists.", desc) : 0,
			 errtableconstraint(heap, RelationGetRelationName(index))));
}

/*
 * Is there an entry with the key columns of entry for another row, as
 * every write so far sees it?
 */
static bool
has_other_entry(const ProllyComponents *c, ProllyRootSet *roots,
				Relation index, ProllyIndexKeying *k, const Datum *values,
				const bool *isnull, const ProllyKey *entry)
{
	int			nkeys = PROLLY_CALL(c->index_keying, nkeys, k);
	StringInfoData bound;
	ProllyKey	b;
	ProllyCursor *cur;
	ProllyKey	key;
	ProllyValue value;
	bool		found = false;

	initStringInfo(&bound);
	PROLLY_CALL(c->index_keying, encode_bound, k, nkeys, values, isnull, false,
				&bound);
	b.data = (const uint8 *) bound.data;
	b.len = bound.len;
	cur = PROLLY_CALL(c->engine, cursor_open, roots, index, PROLLY_TREE_DATA,
					  InvalidCommandId);
	PROLLY_CALL(c->engine, cursor_seek, cur, &b, ForwardScanDirection);
	while (PROLLY_CALL(c->engine, cursor_next, cur, &key, &value))
	{
		if (!PROLLY_CALL(c->index_keying, columns_equal, k, &key, entry, nkeys))
			break;
		if (key.len != entry->len || memcmp(key.data, entry->data, key.len) != 0)
		{
			found = true;
			break;
		}
	}
	PROLLY_CALL(c->engine, cursor_close, cur);
	pfree(bound.data);
	return found;
}

/*
 * Lock a unique index's key value for writing, so writers of the same
 * value (inserting it, or removing it) take turns: one inserting it waits
 * for another that inserted or removed it to end, and then sees what that
 * one committed, as heap's uniqueness check waits for the other row's
 * inserter or deleter.  The value's bytes name it if equal values encode
 * alike; else the whole index is locked.
 */
static void
lock_unique_value(const ProllyComponents *c, Relation index,
				  ProllyIndexKeying *k, const Datum *values,
				  const bool *isnull)
{
	int			nkeys = PROLLY_CALL(c->index_keying, nkeys, k);
	StringInfoData bound;
	ProllyKey	b;
	TM_FailureData tmfd;

	if (!index->rd_index->indisunique || !checks_unique(index, nkeys, isnull))
		return;
	initStringInfo(&bound);
	PROLLY_CALL(c->index_keying, encode_bound, k, nkeys, values, isnull, false,
				&bound);
	b.data = (const uint8 *) bound.data;
	b.len = bound.len;
	(void) PROLLY_CALL(c->coord, lock_row, index,
					   PROLLY_CALL(c->index_keying, equal_bytes, k) ? &b : NULL,
					   LockTupleExclusive, LockWaitBlock, &tmfd);
	pfree(bound.data);
}

/* ------------------------------------------------------------------------
 * Building
 * ------------------------------------------------------------------------
 */

typedef struct BuildState
{
	const ProllyComponents *c;
	ProllyRootSet *roots;
	Relation	heap;
	Relation	index;
	ProllyIndexKeying *k;
	MemoryContext mcxt;
	StringInfo *entries;
	int64		n;
	int64		cap;
} BuildState;

static void
build_callback(Relation index, ItemPointer tid, Datum *values, bool *isnull,
			   bool tupleIsAlive, void *arg)
{
	BuildState *bs = arg;
	MemoryContext old = MemoryContextSwitchTo(bs->mcxt);
	StringInfoData rowkey;
	uint64		ordinal;
	StringInfo	entry = makeStringInfo();
	ProllyKey	rk;

	initStringInfo(&rowkey);
	row_of_tid(bs->c, bs->roots, bs->heap, tid, &rowkey, &ordinal);
	rk.data = (const uint8 *) rowkey.data;
	rk.len = rowkey.len;
	PROLLY_CALL(bs->c->index_keying, encode_entry, bs->k, values, isnull, &rk,
				ordinal, entry);
	pfree(rowkey.data);
	if (bs->n == bs->cap)
	{
		bs->cap = bs->cap ? bs->cap * 2 : 1024;
		bs->entries = bs->entries ?
			repalloc_huge(bs->entries, sizeof(StringInfo) * bs->cap) :
			palloc(sizeof(StringInfo) * bs->cap);
	}
	bs->entries[bs->n++] = entry;
	MemoryContextSwitchTo(old);
}

static BuildState *sort_state;

static int
entry_cmp(const void *a, const void *b)
{
	StringInfo	ea = *(StringInfo const *) a;
	StringInfo	eb = *(StringInfo const *) b;
	ProllyKey	ka = {(const uint8 *) ea->data, ea->len};
	ProllyKey	kb = {(const uint8 *) eb->data, eb->len};

	return PROLLY_CALL(sort_state->c->index_keying, compare, sort_state->k,
					   &ka, &kb);
}

/*
 * Fill index's tree from heap's rows (it is empty, or was just reset),
 * refusing duplicates if it is unique.  Returns the number of rows.
 */
static double
build_entries(const ProllyComponents *c, Relation heap, Relation index,
			  IndexInfo *indexInfo)
{
	BuildState	bs;
	double		reltuples;
	int			nkeys;
	Datum	   *values;
	bool	   *isnull;
	ProllyValue empty = {(const uint8 *) "", 0};

	memset(&bs, 0, sizeof(bs));
	bs.c = c;
	bs.heap = heap;
	bs.index = index;
	bs.k = index_keying(index, c);
	bs.mcxt = AllocSetContextCreate(CurrentMemoryContext, "prolly index build",
									ALLOCSET_DEFAULT_SIZES);
	bs.roots = PROLLY_CALL(c->engine, roots_acquire, NULL);
	reltuples = table_index_build_scan(heap, index, indexInfo, true, false,
									   build_callback, &bs, NULL);

	sort_state = &bs;
	qsort(bs.entries, bs.n, sizeof(StringInfo), entry_cmp);
	sort_state = NULL;

	nkeys = PROLLY_CALL(c->index_keying, nkeys, bs.k);
	values = palloc(sizeof(Datum) * nkeys);
	isnull = palloc(sizeof(bool) * nkeys);
	for (int64 i = 0; i < bs.n; i++)
	{
		ProllyKey	key = {(const uint8 *) bs.entries[i]->data, bs.entries[i]->len};

		if (indexInfo->ii_Unique && i > 0)
		{
			ProllyKey	prev = {(const uint8 *) bs.entries[i - 1]->data,
			bs.entries[i - 1]->len};

			if (PROLLY_CALL(c->index_keying, columns_equal, bs.k, &prev, &key,
							nkeys))
			{
				PROLLY_CALL(c->index_keying, decode_entry, bs.k, &key, values,
							isnull, NULL, NULL);
				if (checks_unique(index, nkeys, isnull))
					report_duplicate(index, heap, values, isnull, true);
			}
		}
		PROLLY_CALL(c->engine, put, index, PROLLY_TREE_DATA, &key, &empty,
					InvalidCommandId);
	}
	record_keying(c, index, bs.k);
	PROLLY_CALL(c->engine, roots_release, bs.roots);
	MemoryContextDelete(bs.mcxt);
	pfree(values);
	pfree(isnull);
	return reltuples;
}

static IndexBuildResult *
prolly_ambuild(Relation heapRelation, Relation indexRelation,
			   IndexInfo *indexInfo)
{
	ProllyComponents c;
	IndexBuildResult *result = palloc0(sizeof(IndexBuildResult));

	if (!is_prolly_table(heapRelation))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("prolly indexes exist only on prolly tables"),
				 errdetail("Table \"%s\" uses access method \"%s\".",
						   RelationGetRelationName(heapRelation),
						   get_am_name(heapRelation->rd_rel->relam))));
	if (indexInfo->ii_NumIndexAttrs != indexInfo->ii_NumIndexKeyAttrs)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("prolly indexes do not support INCLUDE columns yet")));
	prolly_components_for_relation(indexRelation, &c);
	result->heap_tuples = build_entries(&c, heapRelation, indexRelation, indexInfo);
	result->index_tuples = result->heap_tuples;
	return result;
}

static void
prolly_ambuildempty(Relation indexRelation)
{
	/* Only for unlogged indexes, which prolly storage doesn't have. */
	not_implemented("ambuildempty");
}

static bool
prolly_aminsert(Relation indexRelation, Datum *values, bool *isnull,
				ItemPointer heap_tid, Relation heapRelation,
				IndexUniqueCheck checkUnique, bool indexUnchanged,
				IndexInfo *indexInfo)
{
	ProllyComponents c;
	ProllyIndexKeying *k;
	ProllyRootSet *roots;
	StringInfoData rowkey;
	StringInfoData entry;
	uint64		ordinal;
	ProllyKey	rk;
	ProllyKey	e;
	bool		unique = true;
	ProllyValue empty = {(const uint8 *) "", 0};

	prolly_components_for_relation(indexRelation, &c);
	k = index_keying(indexRelation, &c);
	roots = PROLLY_CALL(c.engine, roots_acquire, NULL);
	initStringInfo(&rowkey);
	row_of_tid(&c, roots, heapRelation, heap_tid, &rowkey, &ordinal);
	rk.data = (const uint8 *) rowkey.data;
	rk.len = rowkey.len;
	initStringInfo(&entry);
	PROLLY_CALL(c.index_keying, encode_entry, k, values, isnull, &rk, ordinal,
				&entry);
	e.data = (const uint8 *) entry.data;
	e.len = entry.len;

	/*
	 * Once the value is locked, another row with it is either committed or
	 * this transaction's: look at the newest commit.
	 */
	if (checkUnique != UNIQUE_CHECK_NO)
	{
		lock_unique_value(&c, indexRelation, k, values, isnull);
		PROLLY_CALL(c.engine, roots_release, roots);
		roots = PROLLY_CALL(c.engine, roots_acquire, NULL);
	}
	if (checkUnique != UNIQUE_CHECK_NO &&
		checks_unique(indexRelation, PROLLY_CALL(c.index_keying, nkeys, k), isnull) &&
		has_other_entry(&c, roots, indexRelation, k, values, isnull, &e))
	{
		if (checkUnique == UNIQUE_CHECK_PARTIAL)
			unique = false;
		else
			report_duplicate(indexRelation, heapRelation, values, isnull, false);
	}
	if (checkUnique != UNIQUE_CHECK_EXISTING)
		PROLLY_CALL(c.engine, put, indexRelation, PROLLY_TREE_DATA, &e, &empty,
					GetCurrentCommandId(true));
	PROLLY_CALL(c.engine, roots_release, roots);
	pfree(rowkey.data);
	pfree(entry.data);
	return unique;
}

/* ------------------------------------------------------------------------
 * What the table AM asks
 * ------------------------------------------------------------------------
 */

void
prolly_index_remove_row(Relation heap, TupleTableSlot *slot,
						const ProllyKey *rowkey, uint64 ordinal, CommandId cid)
{
	List	   *indexes;
	ListCell   *lc;
	EState	   *estate = NULL;

	if (!heap->rd_rel->relhasindex)
		return;
	indexes = RelationGetIndexList(heap);
	foreach(lc, indexes)
	{
		Relation	index = index_open(lfirst_oid(lc), RowExclusiveLock);
		ProllyComponents c;
		ProllyIndexKeying *k;
		IndexInfo  *ii;
		Datum		values[INDEX_MAX_KEYS];
		bool		isnull[INDEX_MAX_KEYS];
		StringInfoData entry;
		ProllyKey	e;

		/* An index still being built concurrently is rebuilt when validated. */
		if (!prolly_index_is_prolly(index))
		{
			index_close(index, NoLock);
			continue;
		}
		prolly_components_for_relation(index, &c);
		k = index_keying(index, &c);
		ii = BuildIndexInfo(index);
		if (ii->ii_Expressions != NIL || ii->ii_Predicate != NIL)
		{
			if (estate == NULL)
				estate = CreateExecutorState();
			GetPerTupleExprContext(estate)->ecxt_scantuple = slot;
		}
		if (ii->ii_Predicate != NIL &&
			!ExecQual(ExecPrepareQual(ii->ii_Predicate, estate),
					  GetPerTupleExprContext(estate)))
		{
			index_close(index, NoLock);
			continue;
		}
		FormIndexDatum(ii, slot, estate, values, isnull);
		lock_unique_value(&c, index, k, values, isnull);
		initStringInfo(&entry);
		PROLLY_CALL(c.index_keying, encode_entry, k, values, isnull, rowkey,
					ordinal, &entry);
		e.data = (const uint8 *) entry.data;
		e.len = entry.len;
		PROLLY_CALL(c.engine, remove, index, PROLLY_TREE_DATA, &e, cid);
		pfree(entry.data);
		index_close(index, NoLock);
	}
	if (estate)
		FreeExecutorState(estate);
	list_free(indexes);
}

void
prolly_lock_unique_values(Relation heap, TupleTableSlot *slot)
{
	List	   *indexes;
	ListCell   *lc;
	EState	   *estate = NULL;
	ProllyComponents c;

	if (!heap->rd_rel->relhasindex)
		return;
	prolly_components_for_relation(heap, &c);
	indexes = RelationGetIndexList(heap);
	foreach(lc, indexes)
	{
		Relation	index = index_open(lfirst_oid(lc), RowExclusiveLock);
		IndexInfo  *ii;
		Datum		values[INDEX_MAX_KEYS];
		bool		isnull[INDEX_MAX_KEYS];
		int			nkeys = IndexRelationGetNumberOfKeyAttributes(index);
		ProllyKeyDesc desc;
		Oid			types[INDEX_MAX_KEYS];
		bool		encodable = true;
		StringInfoData bound;
		ProllyKey	b;
		bool		named = false;
		TM_FailureData tmfd;

		if (!index->rd_index->indisunique || !index->rd_index->indisready ||
			prolly_index_is_prolly(index))
		{
			index_close(index, NoLock);
			continue;
		}
		ii = BuildIndexInfo(index);
		if (ii->ii_Expressions != NIL || ii->ii_Predicate != NIL)
		{
			if (estate == NULL)
				estate = CreateExecutorState();
			GetPerTupleExprContext(estate)->ecxt_scantuple = slot;
		}
		if ((ii->ii_Predicate != NIL &&
			 !ExecQual(ExecPrepareQual(ii->ii_Predicate, estate),
					   GetPerTupleExprContext(estate))))
		{
			index_close(index, NoLock);
			continue;
		}
		FormIndexDatum(ii, slot, estate, values, isnull);
		if (!checks_unique(index, nkeys, isnull))
		{
			index_close(index, NoLock);
			continue;
		}

		/* The value's bytes, if equal values encode alike; else the index. */
		for (int i = 0; i < nkeys; i++)
		{
			types[i] = index->rd_opcintype[i];
			encodable &= PROLLY_CALL(c.key_encoder, supports,
									 index->rd_opfamily[i], types[i],
									 index->rd_indcollation[i]);
		}
		initStringInfo(&bound);
		if (encodable)
		{
			ProllyKeyEncoder *enc;

			desc.nkeys = nkeys;
			desc.types = types;
			desc.opfamilies = index->rd_opfamily;
			desc.collations = index->rd_indcollation;
			desc.options = index->rd_indoption;
			enc = PROLLY_CALL(c.key_encoder, begin, &desc, CurrentMemoryContext);
			PROLLY_CALL(c.key_encoder, encode, enc, values, isnull, &bound);
			named = PROLLY_CALL(c.key_encoder, equal_bytes, enc);
			PROLLY_CALL(c.key_encoder, end, enc);
		}
		b.data = (const uint8 *) bound.data;
		b.len = bound.len;
		(void) PROLLY_CALL(c.coord, lock_row, index, named ? &b : NULL,
						   LockTupleExclusive, LockWaitBlock, &tmfd);
		pfree(bound.data);
		index_close(index, NoLock);
	}
	if (estate)
		FreeExecutorState(estate);
	list_free(indexes);
}

void
prolly_index_rebuild(Relation heap, Relation index)
{
	ProllyComponents c;

	prolly_components_for_relation(index, &c);
	PROLLY_CALL(c.engine, tree_reset, index, PROLLY_TREE_DATA);
	(void) build_entries(&c, heap, index, BuildIndexInfo(index));
}

void
prolly_index_rebuild_all(Relation heap)
{
	List	   *indexes;
	ListCell   *lc;

	if (!heap->rd_rel->relhasindex)
		return;
	indexes = RelationGetIndexList(heap);
	foreach(lc, indexes)
	{
		Relation	index = index_open(lfirst_oid(lc), RowExclusiveLock);

		if (prolly_index_is_prolly(index))
			prolly_index_rebuild(heap, index);
		index_close(index, NoLock);
	}
	list_free(indexes);
}

/* ------------------------------------------------------------------------
 * Scans
 * ------------------------------------------------------------------------
 */

typedef struct ProllyIndexScanOpaque
{
	ProllyComponents c;
	MemoryContext mcxt;
	MemoryContext tuplecxt;
	ProllyRootSet *roots;
	ProllyIndexKeying *k;
	int			nkeys;
	CommandId	cid;
	ProllyCursor *cursor;
	Relation	table;			/* opened here for a bitmap scan, else NULL */
	ProllyRowIdScan *rowid;
	Datum	   *values;
	bool	   *isnull;

	/* Where the scan starts and stops, in each direction (len 0: none). */
	StringInfoData lower;
	StringInfoData upper;

	bool		positioned;
	ScanDirection dir;
	bool		exhausted;		/* ran off the end in direction dir */
	StringInfoData current;		/* the last entry returned */
	StringInfoData mark;		/* ammarkpos's entry */
	bool		have_mark;
	StringInfoData skip;		/* repositioned here: don't return it again */
	bool		skipping;
} ProllyIndexScanOpaque;

static IndexScanDesc
prolly_ambeginscan(Relation indexRelation, int nkeys, int norderbys)
{
	IndexScanDesc scan = RelationGetIndexScan(indexRelation, nkeys, norderbys);
	ProllyIndexScanOpaque *so = palloc0(sizeof(ProllyIndexScanOpaque));
	MemoryContext old;

	prolly_components_for_relation(indexRelation, &so->c);
	so->mcxt = AllocSetContextCreate(CurrentMemoryContext, "prolly index scan",
									 ALLOCSET_DEFAULT_SIZES);
	so->tuplecxt = AllocSetContextCreate(so->mcxt, "prolly index tuple",
										 ALLOCSET_SMALL_SIZES);
	old = MemoryContextSwitchTo(so->mcxt);
	so->k = index_keying(indexRelation, &so->c);
	so->nkeys = PROLLY_CALL(so->c.index_keying, nkeys, so->k);
	so->values = palloc0(sizeof(Datum) * Max(so->nkeys, 1));
	so->isnull = palloc0(sizeof(bool) * Max(so->nkeys, 1));
	initStringInfo(&so->lower);
	initStringInfo(&so->upper);
	initStringInfo(&so->current);
	initStringInfo(&so->mark);
	initStringInfo(&so->skip);
	MemoryContextSwitchTo(old);
	scan->opaque = so;
	scan->xs_itupdesc = RelationGetDescr(indexRelation);
	return scan;
}

/* The scan's command, as its snapshot gives it. */
static CommandId
scan_cid(Snapshot snapshot)
{
	if (snapshot != NULL && IsMVCCSnapshot(snapshot))
		return snapshot->curcid;
	return InvalidCommandId;
}

static void
close_cursor(ProllyIndexScanOpaque *so)
{
	if (so->cursor)
	{
		PROLLY_CALL(so->c.engine, cursor_close, so->cursor);
		so->cursor = NULL;
	}
	if (so->rowid)
	{
		PROLLY_CALL(so->c.row_identity, scan_end, so->rowid);
		so->rowid = NULL;
	}
	if (so->roots)
	{
		PROLLY_CALL(so->c.engine, roots_release, so->roots);
		so->roots = NULL;
	}
}

/* Can key bound column attno's values (same type, simple key)? */
static bool
usable_bound(Relation index, ScanKey key)
{
	if (key->sk_flags & (SK_ROW_HEADER | SK_ROW_MEMBER | SK_ORDER_BY))
		return false;
	if (key->sk_flags & SK_ISNULL)
		return (key->sk_flags & SK_SEARCHNULL) != 0;
	return !OidIsValid(key->sk_subtype) ||
		key->sk_subtype == index->rd_opcintype[key->sk_attno - 1];
}

/*
 * The bounds of the scan in index order: the leading columns with
 * equality keys, then on the next column the first range key bounding
 * each end (others are checked on each entry).  On a DESC column, a ">"
 * key bounds the end of the index order and a "<" key its start.
 */
static void
make_bounds(IndexScanDesc scan)
{
	ProllyIndexScanOpaque *so = (ProllyIndexScanOpaque *) scan->opaque;
	Relation	index = scan->indexRelation;
	Datum		lo[INDEX_MAX_KEYS];
	bool		lonull[INDEX_MAX_KEYS];
	Datum		hi[INDEX_MAX_KEYS];
	bool		hinull[INDEX_MAX_KEYS];
	int			neq = 0;
	bool		have_lo = false;
	bool		have_hi = false;

	resetStringInfo(&so->lower);
	resetStringInfo(&so->upper);
	for (int col = 1; col <= so->nkeys; col++)
	{
		bool		eq = false;
		bool		desc = (index->rd_indoption[col - 1] & INDOPTION_DESC) != 0;

		for (int i = 0; i < scan->numberOfKeys && !eq; i++)
		{
			ScanKey		key = &scan->keyData[i];

			if (key->sk_attno != col || !usable_bound(index, key))
				continue;
			if ((key->sk_flags & SK_SEARCHNULL) ||
				key->sk_strategy == BTEqualStrategyNumber)
			{
				lo[col - 1] = hi[col - 1] = key->sk_argument;
				lonull[col - 1] = hinull[col - 1] = (key->sk_flags & SK_ISNULL) != 0;
				eq = true;
			}
		}
		if (eq)
		{
			neq++;
			continue;
		}

		/* A range on this column, after the equal ones. */
		for (int i = 0; i < scan->numberOfKeys; i++)
		{
			ScanKey		key = &scan->keyData[i];
			bool		greater;

			if (key->sk_attno != col || !usable_bound(index, key) ||
				(key->sk_flags & SK_ISNULL))
				continue;
			if (key->sk_strategy == BTGreaterStrategyNumber ||
				key->sk_strategy == BTGreaterEqualStrategyNumber)
				greater = true;
			else if (key->sk_strategy == BTLessStrategyNumber ||
					 key->sk_strategy == BTLessEqualStrategyNumber)
				greater = false;
			else
				continue;
			if (greater != desc && !have_lo)
			{
				lo[col - 1] = key->sk_argument;
				lonull[col - 1] = false;
				have_lo = true;
			}
			else if (greater == desc && !have_hi)
			{
				hi[col - 1] = key->sk_argument;
				hinull[col - 1] = false;
				have_hi = true;
			}
		}
		break;
	}

	if (neq > 0 || have_lo)
		PROLLY_CALL(so->c.index_keying, encode_bound, so->k,
					neq + (have_lo ? 1 : 0), lo, lonull, false, &so->lower);
	if (neq > 0 || have_hi)
		PROLLY_CALL(so->c.index_keying, encode_bound, so->k,
					neq + (have_hi ? 1 : 0), hi, hinull, true, &so->upper);
}

static void
prolly_amrescan(IndexScanDesc scan, ScanKey keys, int nkeys,
				ScanKey orderbys, int norderbys)
{
	ProllyIndexScanOpaque *so = (ProllyIndexScanOpaque *) scan->opaque;
	MemoryContext old;

	if (keys && scan->numberOfKeys > 0)
		memmove(scan->keyData, keys, scan->numberOfKeys * sizeof(ScanKeyData));
	for (int i = 0; i < scan->numberOfKeys; i++)
		if (scan->keyData[i].sk_flags & SK_ROW_HEADER)
			not_implemented("row comparison scan keys");
	close_cursor(so);
	old = MemoryContextSwitchTo(so->mcxt);
	make_bounds(scan);
	so->cid = scan_cid(scan->xs_snapshot);
	so->roots = PROLLY_CALL(so->c.engine, roots_acquire, scan->xs_snapshot);
	so->cursor = PROLLY_CALL(so->c.engine, cursor_open, so->roots,
							 scan->indexRelation, PROLLY_TREE_DATA, so->cid);
	/* A bitmap scan isn't given the table; the executor has it locked. */
	if (scan->heapRelation == NULL && so->table == NULL)
		so->table = table_open(scan->indexRelation->rd_index->indrelid, NoLock);
	so->rowid = PROLLY_CALL(so->c.row_identity, scan_begin, so->roots,
							scan->heapRelation ? scan->heapRelation : so->table,
							so->cid);
	MemoryContextSwitchTo(old);
	so->positioned = false;
	so->exhausted = false;
	so->have_mark = false;
	so->skipping = false;
	resetStringInfo(&so->current);
}

/* Does the decoded entry pass every scan key? */
static bool
entry_matches(IndexScanDesc scan, ProllyIndexScanOpaque *so)
{
	for (int i = 0; i < scan->numberOfKeys; i++)
	{
		ScanKey		key = &scan->keyData[i];
		int			att = key->sk_attno - 1;

		if (key->sk_flags & SK_ISNULL)
		{
			if (key->sk_flags & SK_SEARCHNULL)
			{
				if (!so->isnull[att])
					return false;
			}
			else if (key->sk_flags & SK_SEARCHNOTNULL)
			{
				if (so->isnull[att])
					return false;
			}
			else
				return false;	/* a NULL argument matches nothing */
			continue;
		}
		if (so->isnull[att])
			return false;
		if (!DatumGetBool(FunctionCall2Coll(&key->sk_func, key->sk_collation,
											so->values[att], key->sk_argument)))
			return false;
	}
	return true;
}

/*
 * Position the cursor for direction dir: at at (whose entry isn't
 * returned again), or at the scan's bound in that direction.
 */
static void
position(ProllyIndexScanOpaque *so, ScanDirection dir, const StringInfoData *at)
{
	StringInfo	start = ScanDirectionIsForward(dir) ? &so->lower : &so->upper;
	ProllyKey	k;

	so->skipping = (at != NULL);
	if (at)
	{
		resetStringInfo(&so->skip);
		appendBinaryStringInfo(&so->skip, at->data, at->len);
		k.data = (const uint8 *) so->skip.data;
		k.len = so->skip.len;
	}
	else
	{
		k.data = (const uint8 *) start->data;
		k.len = start->len;
	}
	PROLLY_CALL(so->c.engine, cursor_seek, so->cursor, k.len > 0 ? &k : NULL, dir);
	so->dir = dir;
	so->positioned = true;
}

static bool
prolly_amgettuple(IndexScanDesc scan, ScanDirection direction)
{
	ProllyIndexScanOpaque *so = (ProllyIndexScanOpaque *) scan->opaque;
	bool		forward = ScanDirectionIsForward(direction);
	StringInfo	stop = forward ? &so->upper : &so->lower;
	ProllyKey	stopkey = {(const uint8 *) stop->data, stop->len};

	/*
	 * Start; or turn around: at the last entry returned, or, past the end,
	 * at the scan's bound in the new direction (whose first entry is the
	 * one returned last).
	 */
	if (!so->positioned)
		position(so, direction, NULL);
	else if (so->dir != direction)
		position(so, direction,
				 !so->exhausted && so->current.len > 0 ? &so->current : NULL);
	so->exhausted = false;

	for (;;)
	{
		ProllyKey	key;
		ProllyValue value;
		ProllyKey	rowkey;
		uint64		ordinal;
		MemoryContext old;

		CHECK_FOR_INTERRUPTS();
		if (!PROLLY_CALL(so->c.engine, cursor_next, so->cursor, &key, &value))
		{
			so->exhausted = true;
			return false;
		}
		if (so->skipping)
		{
			so->skipping = false;
			if (key.len == so->skip.len &&
				memcmp(key.data, so->skip.data, key.len) == 0)
				continue;
		}
		if (stopkey.len > 0)
		{
			int			r = PROLLY_CALL(so->c.index_keying, compare, so->k,
										&key, &stopkey);

			if (forward ? r > 0 : r < 0)
			{
				so->exhausted = true;
				return false;
			}
		}
		MemoryContextReset(so->tuplecxt);
		old = MemoryContextSwitchTo(so->tuplecxt);
		PROLLY_CALL(so->c.index_keying, decode_entry, so->k, &key, so->values,
					so->isnull, &rowkey, &ordinal);
		if (!entry_matches(scan, so))
		{
			MemoryContextSwitchTo(old);
			continue;
		}
		if (!PROLLY_CALL(so->c.row_identity, tid_find, so->rowid, &rowkey,
						 ordinal, &scan->xs_heaptid))
			elog(ERROR, "a row in prolly index \"%s\" has no TID",
				 RelationGetRelationName(scan->indexRelation));
		scan->xs_recheck = false;
		if (scan->xs_want_itup)
			scan->xs_itup = index_form_tuple(RelationGetDescr(scan->indexRelation),
											 so->values, so->isnull);
		MemoryContextSwitchTo(old);
		resetStringInfo(&so->current);
		appendBinaryStringInfo(&so->current, (const char *) key.data, key.len);
		return true;
	}
}

/* Every matching entry's TID, into tbm; entries are exact, so no recheck. */
static int64
prolly_amgetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	int64		ntids = 0;

	while (prolly_amgettuple(scan, ForwardScanDirection))
	{
		tbm_add_tuples(tbm, &scan->xs_heaptid, 1, false);
		ntids++;
	}
	return ntids;
}

static void
prolly_amendscan(IndexScanDesc scan)
{
	ProllyIndexScanOpaque *so = (ProllyIndexScanOpaque *) scan->opaque;

	close_cursor(so);
	if (so->table)
		table_close(so->table, NoLock);
	MemoryContextDelete(so->mcxt);
	pfree(so);
}

static void
prolly_ammarkpos(IndexScanDesc scan)
{
	ProllyIndexScanOpaque *so = (ProllyIndexScanOpaque *) scan->opaque;

	resetStringInfo(&so->mark);
	appendBinaryStringInfo(&so->mark, so->current.data, so->current.len);
	so->have_mark = so->current.len > 0;
}

/* The next entry returned is the one after the marked one. */
static void
prolly_amrestrpos(IndexScanDesc scan)
{
	ProllyIndexScanOpaque *so = (ProllyIndexScanOpaque *) scan->opaque;
	ScanDirection dir = so->positioned ? so->dir : ForwardScanDirection;

	if (!so->have_mark)
	{
		so->positioned = false;
		resetStringInfo(&so->current);
		return;
	}
	resetStringInfo(&so->current);
	appendBinaryStringInfo(&so->current, so->mark.data, so->mark.len);
	position(so, dir, &so->mark);
}

/* ------------------------------------------------------------------------
 * VACUUM, costs, options
 * ------------------------------------------------------------------------
 */

/*
 * Entries are removed with their rows, so VACUUM finds none to delete;
 * the callback still sees every entry's TID (CREATE INDEX CONCURRENTLY
 * collects them this way).
 */
static IndexBulkDeleteResult *
prolly_ambulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
					IndexBulkDeleteCallback callback, void *callback_state)
{
	Relation	index = info->index;
	ProllyComponents c;
	ProllyIndexKeying *k;
	ProllyRootSet *roots;
	ProllyCursor *cur;
	ProllyRowIdScan *rowid;
	ProllyKey	key;
	ProllyValue value;

	if (stats == NULL)
		stats = palloc0(sizeof(IndexBulkDeleteResult));
	prolly_components_for_relation(index, &c);
	k = index_keying(index, &c);
	roots = PROLLY_CALL(c.engine, roots_acquire, NULL);
	cur = PROLLY_CALL(c.engine, cursor_open, roots, index, PROLLY_TREE_DATA,
					  InvalidCommandId);
	rowid = PROLLY_CALL(c.row_identity, scan_begin, roots, info->heaprel,
						InvalidCommandId);
	PROLLY_CALL(c.engine, cursor_seek, cur, NULL, ForwardScanDirection);
	while (PROLLY_CALL(c.engine, cursor_next, cur, &key, &value))
	{
		ProllyKey	rowkey;
		uint64		ordinal;
		ItemPointerData tid;

		PROLLY_CALL(c.index_keying, decode_entry, k, &key, NULL, NULL, &rowkey,
					&ordinal);
		if (!PROLLY_CALL(c.row_identity, tid_find, rowid, &rowkey, ordinal, &tid))
			continue;
		if (callback && callback(&tid, callback_state))
			stats->tuples_removed += 1;
		else
			stats->num_index_tuples += 1;
	}
	PROLLY_CALL(c.row_identity, scan_end, rowid);
	PROLLY_CALL(c.engine, cursor_close, cur);
	PROLLY_CALL(c.engine, roots_release, roots);
	return stats;
}

static IndexBulkDeleteResult *
prolly_amvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	if (stats == NULL)
		stats = prolly_ambulkdelete(info, NULL, NULL, NULL);
	return stats;
}

static bool
prolly_amcanreturn(Relation indexRelation, int attno)
{
	return attno <= IndexRelationGetNumberOfKeyAttributes(indexRelation);
}

static void
prolly_amcostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
					  Cost *indexStartupCost, Cost *indexTotalCost,
					  Selectivity *indexSelectivity, double *indexCorrelation,
					  double *indexPages)
{
	GenericCosts costs = {0};

	genericcostestimate(root, path, loop_count, &costs);
	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = costs.indexCorrelation;
	*indexPages = costs.numIndexPages;
}

static bytea *
prolly_amoptions(Datum reloptions, bool validate)
{
	return btoptions(reloptions, validate);
}

Datum
prolly_indexam_handler(PG_FUNCTION_ARGS)
{
	static const IndexAmRoutine amroutine = {
		.type = T_IndexAmRoutine,
		/* operator classes follow the btree contract */
		.amstrategies = BTMaxStrategyNumber,
		.amsupport = BTNProcs,
		.amoptsprocnum = BTOPTIONS_PROC,
		.amcanorder = true,
		.amcanorderbyop = false,
		.amcanhash = false,
		.amconsistentequality = true,
		.amconsistentordering = true,
		.amcanbackward = true,
		.amcanunique = true,
		.amcanmulticol = true,
		.amoptionalkey = true,
		.amsearcharray = false,
		.amsearchnulls = true,
		.amstorage = false,
		.amclusterable = true,
		.ampredlocks = false,
		.amcanparallel = false,
		.amcanbuildparallel = false,
		.amcaninclude = false,
		.amusemaintenanceworkmem = false,
		.amsummarizing = false,
		.amparallelvacuumoptions = VACUUM_OPTION_PARALLEL_BULKDEL,
		.amkeytype = InvalidOid,

		.ambuild = prolly_ambuild,
		.ambuildempty = prolly_ambuildempty,
		.aminsert = prolly_aminsert,
		.aminsertcleanup = NULL,
		.ambulkdelete = prolly_ambulkdelete,
		.amvacuumcleanup = prolly_amvacuumcleanup,
		.amcanreturn = prolly_amcanreturn,
		.amcostestimate = prolly_amcostestimate,
		.amgettreeheight = NULL,
		.amoptions = prolly_amoptions,
		.amproperty = NULL,
		.ambuildphasename = NULL,
		.amvalidate = btvalidate,
		.amadjustmembers = NULL,
		.ambeginscan = prolly_ambeginscan,
		.amrescan = prolly_amrescan,
		.amgettuple = prolly_amgettuple,
		.amgetbitmap = prolly_amgetbitmap,
		.amendscan = prolly_amendscan,
		.ammarkpos = prolly_ammarkpos,
		.amrestrpos = prolly_amrestrpos,
		.amestimateparallelscan = NULL,
		.aminitparallelscan = NULL,
		.amparallelrescan = NULL,
		/*
		 * btree's strategies.  Only compare type -> strategy: with the other
		 * direction the planner would count prolly's operator families as
		 * merge-join and equivalence families too, changing plans for every
		 * table; sort orders still map to btree's families through "<".
		 */
		.amtranslatestrategy = NULL,
		.amtranslatecmptype = bttranslatecmptype,
	};

	PG_RETURN_POINTER(&amroutine);
}
