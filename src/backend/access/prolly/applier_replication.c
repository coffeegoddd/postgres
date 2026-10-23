/*-------------------------------------------------------------------------
 *
 * applier_replication.c
 *	  Row applier implementation "replication": rows written as logical
 *	  replication's apply worker writes them.
 *
 * The executor's simple-relation routines (execReplication.c) insert,
 * update and delete with index maintenance and constraint checks.  A row
 * to change is found by the primary key's index, or, for a table without
 * one, by a row whose every column has the same bytes, through an index
 * on its columns if there is one, or else through the rows' hashes, taken
 * in one scan of the table when a row is first looked for.
 *
 * The apply worker turns triggers off with session_replication_role, but
 * that also skips the recheck of deferrable unique and exclusion
 * constraints, and only a superuser may set it.  Here the result
 * relation instead keeps only those rechecks among its triggers, so no
 * user trigger fires and no foreign key is checked, and nothing else in
 * the session changes.  After immediate(), the rechecks it keeps are
 * immediate ones, and each write fires its own in a query level of its
 * own: an error a recheck raises is the write's, and a subtransaction the
 * write was made in takes that level with it as it rolls back.
 *
 * Many rows inserted together are written as COPY writes them: a page's
 * rows at once (heap_multi_insert()), then each row's index entries.  A
 * table filled from nothing that is new in the transaction gets no index
 * entries as its rows are written: its indexes are built over them once
 * they are in, as CREATE INDEX builds them (#490).  Every row removed at
 * once is removed as TRUNCATE removes them, the table given new storage
 * (#492).
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/applier_replication.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/prolly_builtins.h"
#include "access/prolly_config.h"
#include "access/prolly_keying.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/pg_trigger.h"
#include "commands/trigger.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "executor/nodeModifyTable.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_relation.h"
#include "pgstat.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "utils/acl.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"
#include "utils/injection_point.h"
#include "utils/syscache.h"

#define WRITE_PRIVS		(ACL_SELECT | ACL_INSERT | ACL_UPDATE | ACL_DELETE)

struct ProllyApplier
{
	Relation	rel;
	Oid			key_index;		/* the primary key's; invalid: none */
	Oid			match_index;	/* without a key, one to find rows by */
	EState	   *estate;
	ResultRelInfo *rri;
	EPQState	epq;
	TupleTableSlot *found;
	bool		matview;		/* rows written as REFRESH writes them */
	bool		immediate;		/* each write's rechecks fired as it ends */
	bool		loading;		/* rows written without index entries,
								 * the indexes built after (load_end()) */
	bool		failed;			/* a load's end raised: unusable */
	int			load_level;		/* the nest level of the transaction
								 * loading */
	uint64		load_rows;		/* rows the load wrote, its builds' entries
								 * counted once they are made */
	ResourceOwner owner;		/* begin()'s: the indexes open under it */
	BulkInsertState bistate;	/* insert_many()'s (no pin held between) */
	/* without a key or match_index: its rows by the hash of their values */
	HTAB	   *rows;
	int			lookups;		/* of a keyless table's rows, so far */
};

/* The rows of a hash: where they are (some perhaps gone since). */
typedef struct RowsEntry
{
	uint32		hash;			/* (the key) */
	int			n;
	int			cap;
	ItemPointerData *tids;
} RowsEntry;

/* RLS would decide which rows the user may write; it isn't applied here. */
static void
check_no_rls(Oid relid)
{
	if (check_enable_rls(relid, InvalidOid, false) == RLS_ENABLED)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("user \"%s\" cannot apply rows to relation with row-level security enabled: \"%s\"",
						GetUserNameFromId(GetUserId(), true),
						get_rel_name(relid))));
}

/*
 * The user may write rel, and each table whose foreign keys reference it:
 * skipping those keys' checks can leave that table's rows referencing
 * nothing, which the user could otherwise do only by writing it.
 */
static void
check_privileges(Relation rel)
{
	TriggerDesc *td = rel->trigdesc;

	if (pg_class_aclmask(RelationGetRelid(rel), GetUserId(), WRITE_PRIVS,
						 ACLMASK_ALL) != WRITE_PRIVS)
		aclcheck_error(ACLCHECK_NO_PRIV,
					   get_relkind_objtype(rel->rd_rel->relkind),
					   RelationGetRelationName(rel));
	check_no_rls(RelationGetRelid(rel));

	for (int i = 0; td && i < td->numtriggers; i++)
	{
		Oid			other = td->triggers[i].tgconstrrelid;

		if (RI_FKey_trigger_type(td->triggers[i].tgfoid) != RI_TRIGGER_PK ||
			other == RelationGetRelid(rel))
			continue;
		if (pg_class_aclmask(other, GetUserId(), ACL_UPDATE | ACL_DELETE,
							 ACLMASK_ALL) != (ACL_UPDATE | ACL_DELETE))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied for table %s",
							get_rel_name(other)),
					 errdetail("Applying rows to \"%s\" skips the checks of foreign keys referencing it, which takes UPDATE and DELETE on \"%s\".",
							   RelationGetRelationName(rel),
							   get_rel_name(other))));
		check_no_rls(other);
	}
}

/*
 * Of td's triggers, only the rechecks of deferrable unique and exclusion
 * constraints (NULL if there are none).  The arrays InitResultRelInfo
 * sized for every trigger are big enough for these.
 */
static TriggerDesc *
kept_triggers(TriggerDesc *td)
{
	TriggerDesc *kept;

	if (td == NULL)
		return NULL;
	kept = palloc0(sizeof(TriggerDesc));
	kept->triggers = palloc0(td->numtriggers * sizeof(Trigger));
	for (int i = 0; i < td->numtriggers; i++)
	{
		Trigger    *t = &td->triggers[i];

		if (t->tgfoid != F_UNIQUE_KEY_RECHECK)
			continue;
		/* AFTER ROW INSERT OR UPDATE, as index_constraint_create makes it */
		kept->triggers[kept->numtriggers++] = *t;
		kept->trig_insert_after_row |= TRIGGER_FOR_INSERT(t->tgtype);
		kept->trig_update_after_row |= TRIGGER_FOR_UPDATE(t->tgtype);
	}
	return kept->numtriggers > 0 ? kept : NULL;
}

/*
 * Without a key, an index to find a row by, as the apply worker's
 * FindUsableIndexForReplicaIdentityFull picks one: valid, not partial,
 * scannable a row at a time, an equality operator for each key column and
 * a column (not an expression) first.  Its matches are rechecked, so any
 * such index will do.
 */
static Oid
usable_index(Relation rel)
{
	Oid			chosen = InvalidOid;

	foreach_oid(idxoid, RelationGetIndexList(rel))
	{
		Relation	idx = index_open(idxoid, AccessShareLock);
		Form_pg_index form = idx->rd_index;
		oidvector  *indclass;
		bool		ok;

		indclass = (oidvector *)
			DatumGetPointer(SysCacheGetAttrNotNull(INDEXRELID,
												   idx->rd_indextuple,
												   Anum_pg_index_indclass));
		ok = form->indisvalid &&
			heap_attisnull(idx->rd_indextuple, Anum_pg_index_indpred, NULL) &&
			AttributeNumberIsValid(form->indkey.values[0]) &&
			idx->rd_indam->amgettuple != NULL;
		for (int i = 0; ok && i < form->indnkeyatts; i++)
			ok = IndexAmTranslateCompareType(COMPARE_EQ, idx->rd_rel->relam,
											 get_opclass_family(indclass->values[i]),
											 true) != InvalidStrategy;
		index_close(idx, AccessShareLock);
		if (ok)
		{
			chosen = idxoid;
			break;
		}
	}
	return chosen;
}

/*
 * Keys of an index scan for rows whose columns equal row's, one for each
 * key column that is a column; -1 if a column is NULL and the index can't
 * search for NULLs.
 */
static int
match_keys(ScanKey skey, Relation idx, TupleTableSlot *row)
{
	oidvector  *indclass;
	int			n = 0;

	indclass = (oidvector *)
		DatumGetPointer(SysCacheGetAttrNotNull(INDEXRELID, idx->rd_indextuple,
											   Anum_pg_index_indclass));
	for (int i = 0; i < IndexRelationGetNumberOfKeyAttributes(idx); i++)
	{
		AttrNumber	attno = idx->rd_index->indkey.values[i];
		Oid			opfamily = get_opclass_family(indclass->values[i]);
		Oid			optype = get_opclass_input_type(indclass->values[i]);
		StrategyNumber eq;
		Oid			op;

		if (!AttributeNumberIsValid(attno))
			continue;
		if (row->tts_isnull[attno - 1] && !idx->rd_indam->amsearchnulls)
			return -1;
		eq = IndexAmTranslateCompareType(COMPARE_EQ, idx->rd_rel->relam,
										 opfamily, false);
		op = get_opfamily_member(opfamily, optype, optype, eq);
		if (!OidIsValid(op))
			elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
				 eq, optype, optype, opfamily);
		ScanKeyInit(&skey[n], i + 1, eq, get_opcode(op),
					row->tts_values[attno - 1]);
		skey[n].sk_collation = idx->rd_indcollation[i];
		if (row->tts_isnull[attno - 1])
			skey[n].sk_flags |= (SK_ISNULL | SK_SEARCHNULL);
		n++;
	}
	return n;
}

/*
 * Every column of a and b has the same bytes, not merely equal values:
 * the rows that differ were found by their bytes, and 'a' and 'A' are
 * different rows even where a collation says they are equal.
 */
static bool
same_row(TupleTableSlot *a, TupleTableSlot *b)
{
	TupleDesc	desc = a->tts_tupleDescriptor;

	slot_getallattrs(a);
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (att->attisdropped)
			continue;
		if (a->tts_isnull[i] != b->tts_isnull[i])
			return false;
		if (!a->tts_isnull[i] &&
			!datum_image_eq(a->tts_values[i], b->tts_values[i],
							att->attbyval, att->attlen))
			return false;
	}
	return true;
}

/*
 * A copy of old in a keyless table into a->found, locked, by match_index
 * or a scan of every row.  As RelationFindReplTupleSeq: a row another
 * transaction is changing is waited for, and looked for again if it
 * changed.
 */
static bool
find_copy(ProllyApplier *a, TupleTableSlot *old)
{
	ScanKeyData skey[INDEX_MAX_KEYS];
	int			nkeys = -1;
	Relation	idx = NULL;
	IndexScanDesc iscan = NULL;
	TableScanDesc scan = NULL;
	SnapshotData snap;
	TupleTableSlot *slot = a->found;
	bool		found;

	InitDirtySnapshot(snap);
	if (OidIsValid(a->match_index))
	{
		idx = index_open(a->match_index, RowExclusiveLock);
		nkeys = match_keys(skey, idx, old);
	}
	if (nkeys > 0)
		iscan = index_beginscan(a->rel, idx, false, &snap, NULL, nkeys, 0,
								SO_NONE);
	else
		scan = table_beginscan(a->rel, &snap, 0, NULL, SO_NONE);

retry:
	found = false;
	if (iscan)
		index_rescan(iscan, skey, nkeys, NULL, 0);
	else
		table_rescan(scan, NULL);
	while (iscan ? table_index_getnext_slot(iscan, ForwardScanDirection, slot) :
		   table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		TransactionId xwait;

		if (!same_row(slot, old))
			continue;
		ExecMaterializeSlot(slot);
		xwait = TransactionIdIsValid(snap.xmin) ? snap.xmin : snap.xmax;
		if (TransactionIdIsValid(xwait))
		{
			XactLockTableWait(xwait, NULL, NULL, XLTW_None);
			goto retry;
		}
		found = true;
		break;
	}
	if (found)
	{
		TM_FailureData tmfd;
		TM_Result	res;

		PushActiveSnapshot(GetLatestSnapshot());
		res = table_tuple_lock(a->rel, &slot->tts_tid, GetActiveSnapshot(),
							   slot, GetCurrentCommandId(false),
							   LockTupleExclusive, LockWaitBlock, 0, &tmfd);
		PopActiveSnapshot();
		if (res == TM_Updated || res == TM_Deleted)
			goto retry;
		if (res != TM_Ok)
			elog(ERROR, "unexpected table_tuple_lock status: %u", res);
	}

	if (iscan)
		index_endscan(iscan);
	else
		table_endscan(scan);
	if (idx)
		index_close(idx, NoLock);
	return found;
}

static ProllyApplier *
replication_begin(Relation rel)
{
	ProllyApplier *a = palloc0(sizeof(ProllyApplier));
	RangeTblEntry *rte;
	List	   *perminfos = NIL;

	/*
	 * A partition's rows are applied to it, not to its parent.  A
	 * materialized view's are written as its refresh writes them, which
	 * takes the privilege to refresh it.
	 */
	if (rel->rd_rel->relkind == RELKIND_MATVIEW)
	{
		AclResult	r = pg_class_aclcheck(RelationGetRelid(rel), GetUserId(),
										  ACL_MAINTAIN);

		if (r != ACLCHECK_OK)
			aclcheck_error(r, OBJECT_MATVIEW, RelationGetRelationName(rel));
		a->matview = true;
	}
	else if (rel->rd_rel->relkind != RELKIND_RELATION)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot apply rows to relation \"%s\"",
						RelationGetRelationName(rel)),
				 errdetail_relkind_not_supported(rel->rd_rel->relkind)));
	else
		check_privileges(rel);

	a->rel = rel;
	a->key_index = RelationGetPrimaryKeyIndex(rel, true);
	if (!OidIsValid(a->key_index))
		a->match_index = usable_index(rel);
	a->estate = CreateExecutorState();
	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RELATION;
	rte->relid = RelationGetRelid(rel);
	rte->relkind = rel->rd_rel->relkind;
	rte->rellockmode = RowExclusiveLock;
	addRTEPermissionInfo(&perminfos, rte);
	ExecInitRangeTable(a->estate, list_make1(rte), perminfos,
					   bms_make_singleton(1));
	a->rri = makeNode(ResultRelInfo);
	InitResultRelInfo(a->rri, rel, 1, NULL, 0);
	/* No user triggers or foreign key checks: as a replica applies. */
	a->rri->ri_TrigDesc = kept_triggers(a->rri->ri_TrigDesc);
	a->estate->es_opened_result_relations =
		lappend(a->estate->es_opened_result_relations, a->rri);
	a->estate->es_output_cid = GetCurrentCommandId(true);
	ExecOpenIndices(a->rri, false);
	a->owner = CurrentResourceOwner;
	EvalPlanQualInit(&a->epq, a->estate, NULL, NIL, -1, NIL);
	a->found = table_slot_create(rel, &a->estate->es_tupleTable);

	/*
	 * The slots after-row triggers' events would take are made now, not on
	 * the first write: a write made in a subtransaction would otherwise own
	 * them.
	 */
	if (a->rri->ri_TrigDesc)
	{
		(void) ExecGetTriggerOldSlot(a->estate, a->rri);
		(void) ExecGetTriggerNewSlot(a->estate, a->rri);
	}
	AfterTriggerBeginQuery();
	return a;
}

/*
 * The rows a write read are let go: the one found for it, and those its
 * after-row triggers' events read (the old row an update's event keeps,
 * or the row a recheck that raised read).  So no buffer stays pinned
 * between writes: a caller may write each in a subtransaction of its own,
 * and update in the next a row the last read.  A write that failed lets
 * them go before the error reaches the caller, whose subtransaction (if
 * it wrote in one) pinned their buffers and lets the pins go as it rolls
 * back.
 */
static void
let_go(ProllyApplier *a)
{
	ExecClearTuple(a->found);
	if (a->rri->ri_TrigOldSlot)
		ExecClearTuple(a->rri->ri_TrigOldSlot);
	if (a->rri->ri_TrigNewSlot)
		ExecClearTuple(a->rri->ri_TrigNewSlot);
}

/* The next write sees this one, and its rows are let go (let_go()). */
static void
advance(ProllyApplier *a)
{
	let_go(a);
	CommandCounterIncrement();
	a->estate->es_output_cid = GetCurrentCommandId(true);
}

/* The hash of a row's values (their images: equal rows' are alike). */
static uint32
row_hash(TupleTableSlot *slot)
{
	TupleDesc	desc = slot->tts_tupleDescriptor;
	uint32		h = 0;

	slot_getallattrs(slot);
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (att->attisdropped)
			continue;
		h = hash_combine(h, slot->tts_isnull[i] ? 0x9e3779b9 :
						 datum_image_hash(slot->tts_values[i], att->attbyval,
										  att->attlen));
	}
	return h;
}

static void
rows_add(ProllyApplier *a, TupleTableSlot *row)
{
	uint32		h = row_hash(row);
	bool		found;
	RowsEntry  *e = hash_search(a->rows, &h, HASH_ENTER, &found);

	if (!found)
	{
		e->n = 0;
		e->cap = 1;
		e->tids = MemoryContextAlloc(a->estate->es_query_cxt, sizeof(ItemPointerData));
	}
	else if (e->n == e->cap)
	{
		e->cap *= 2;
		e->tids = repalloc_array(e->tids, ItemPointerData, e->cap);
	}
	e->tids[e->n++] = row->tts_tid;
}

/* Index the rows of a table without a key: one scan. */
static void
rows_build(ProllyApplier *a)
{
	HASHCTL		ctl;
	TableScanDesc scan;
	TupleTableSlot *slot = table_slot_create(a->rel, NULL);
	Snapshot	snap = RegisterSnapshot(GetLatestSnapshot());

	ctl.keysize = sizeof(uint32);
	ctl.entrysize = sizeof(RowsEntry);
	ctl.hcxt = a->estate->es_query_cxt;
	/* (as many as the table is thought to have) */
	a->rows = hash_create("prolly applier rows",
						  (long) Min(Max(a->rel->rd_rel->reltuples, 1024), 1 << 24),
						  &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	scan = table_beginscan(a->rel, snap, 0, NULL, SO_NONE);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
		rows_add(a, slot);
	table_endscan(scan);
	UnregisterSnapshot(snap);
	ExecDropSingleTupleTableSlot(slot);
}

/*
 * A row of a table without a key, alike to old, found through the index and
 * locked, into a->found; it leaves the index (it is about to change), as
 * does a row gone since.  False if none is: the index may have missed it (a
 * row whose values are equal but not alike, or one written by another
 * session), so the caller scans for it.  The index is made only once a
 * few rows were looked for: a move that changes few rows scans for each,
 * rather than reading the whole table into the index.
 */
#define ROWS_INDEX_AFTER	8

static bool
rows_find(ProllyApplier *a, TupleTableSlot *old)
{
	uint32		h;
	RowsEntry  *e;
	bool		found = false;

	if (a->rows == NULL && ++a->lookups <= ROWS_INDEX_AFTER)
		return false;
	if (a->rows == NULL)
		rows_build(a);
	h = row_hash(old);
	e = hash_search(a->rows, &h, HASH_FIND, NULL);
	PushActiveSnapshot(GetLatestSnapshot());
	for (int i = e ? e->n - 1 : -1; i >= 0 && !found; i--)
	{
		ItemPointerData tid = e->tids[i];
		TM_FailureData tmfd;

		if (!table_tuple_fetch_row_version(a->rel, &tid, GetActiveSnapshot(),
										   a->found))
			e->tids[i] = e->tids[--e->n];	/* (gone) */
		else if (same_row(a->found, old) &&
				 table_tuple_lock(a->rel, &tid, GetActiveSnapshot(), a->found,
								  GetCurrentCommandId(false), LockTupleExclusive,
								  LockWaitBlock, 0, &tmfd) == TM_Ok)
		{
			e->tids[i] = e->tids[--e->n];
			found = true;
		}
	}
	PopActiveSnapshot();
	if (!found)
		ExecClearTuple(a->found);
	return found;
}

/* A row equal to old (by the key, or every column) into a->found. */
static bool
find_row(ProllyApplier *a, TupleTableSlot *old)
{
	slot_getallattrs(old);
	if (OidIsValid(a->key_index))
		return RelationFindReplTupleByIndex(a->rel, a->key_index, true,
											LockTupleExclusive, old, a->found);
	if (OidIsValid(a->match_index))
		return find_copy(a, old);
	if (rows_find(a, old))
		return true;
	return find_copy(a, old);
}

/*
 * The heap-only updates heap has counted for the table so far, or -1 when
 * it counts none (track_counts off): the index entries an update made are
 * none for a heap-only one, else one per index, which apply_index_tuples
 * counts.  (Without heap's count every update counts its indexes.)
 */
static PgStat_Counter
hot_updates(Relation rel)
{
	if (!pgstat_should_count_relation(rel))
		return -1;
	return rel->pgstat_info->tab.counts_xact.tuples_hot_updated;
}

/* Count n rows applied, each with an entry in every index. */
static void
count_applied(ProllyApplier *a, uint64 n, bool indexed)
{
	pgstat_count_prolly(PGSTAT_PROLLY_APPLY_ROWS, n);
	if (indexed)
		pgstat_count_prolly(PGSTAT_PROLLY_APPLY_INDEX_TUPLES,
							n * a->rri->ri_NumIndices);
}

/*
 * A materialized view's rows, as a refresh writes them: no constraints or
 * triggers to check (it has none), its indexes maintained.  (The
 * executor's simple-relation routines take tables only.)
 */
static void
matview_insert(ProllyApplier *a, TupleTableSlot *row)
{
	simple_table_tuple_insert(a->rel, row);
	if (a->rri->ri_NumIndices > 0)
		list_free(ExecInsertIndexTuples(a->rri, a->estate, 0, row, NIL, NULL));
}

static void
matview_delete(ProllyApplier *a)
{
	simple_table_tuple_delete(a->rel, &a->found->tts_tid,
							  a->estate->es_snapshot);
}

/*
 * A write's rechecks of deferrable unique and exclusion constraints, once
 * immediate(): queued in a query level begun for the write, and fired as
 * it ends (only those of the indexes the write's entries may break, as
 * the executor queues them).
 */
static void
rechecks_begin(ProllyApplier *a)
{
	if (a->immediate)
		AfterTriggerBeginQuery();
}

static void
rechecks_fire(ProllyApplier *a)
{
	if (!a->immediate)
		return;
	PG_TRY();
	{
		AfterTriggerEndQuery(a->estate);
	}
	PG_CATCH();
	{
		/* (a recheck that raised left its row in the trigger slots) */
		let_go(a);
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * Is a table's filling from nothing to be a load, its indexes built once
 * its rows are in rather than kept a row at a time?  Only when no other
 * transaction can see it: made in this one, or given new storage in it
 * under the ACCESS EXCLUSIVE lock that takes; and only for enough rows to
 * be worth the build.  Not with deferrable rechecks to queue (the build
 * checks every unique and exclusion constraint at once, deferrable or
 * not), nor for a materialized view.
 */
#define LOAD_MIN_ROWS	100

static bool
loads(ProllyApplier *a, int n)
{
	Relation	rel = a->rel;

	if (n < LOAD_MIN_ROWS || a->matview || a->rri->ri_TrigDesc != NULL ||
		a->rri->ri_NumIndices == 0)
		return false;
	if (rel->rd_createSubid == InvalidSubTransactionId &&
		(rel->rd_firstRelfilelocatorSubid == InvalidSubTransactionId ||
		 !CheckRelationLockedByMe(rel, AccessExclusiveLock, false)))
		return false;
	return RelationGetNumberOfBlocks(rel) == 0;
}

/*
 * A load whose end raised leaves the applier with the rows loaded and no
 * index entries for them: its indexes closed if the build raised, and a
 * subtransaction rolled back gives them back their storage from before
 * it, which has none.  A write after it would make no index entries, or
 * find no row by an index.  So none is made: the load's failure ends the
 * applier, but for end().
 */
static void
check_usable(ProllyApplier *a)
{
	if (a->failed)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot apply rows to relation \"%s\" after its load failed",
						RelationGetRelationName(a->rel))));
}

/*
 * The load's end: every index built over the rows written, its unique and
 * exclusion constraints checked as it is (a row breaking one is refused
 * here), and the primary key's not checking the rows' keys' length (see
 * replication_insert()).  The indexes are closed meanwhile: an index open
 * is in use, and isn't rebuilt.  Before any write of another kind, which
 * may look for a row by an index.
 */
static void
load_end(ProllyApplier *a)
{
	ReindexParams params = {0};
	bool		held;
	ResourceOwner owner = CurrentResourceOwner;

	check_usable(a);
	if (!a->loading)
		return;
	a->loading = false;
	/* (until the indexes are built and open again: see check_usable()) */
	a->failed = true;

	/*
	 * Not in a subtransaction begun since the load: rolled back, it would
	 * give the indexes back their storage from before the build, under
	 * rows it doesn't take away.
	 */
	if (GetCurrentTransactionNestLevel() > a->load_level)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot finish loading relation \"%s\" in another subtransaction",
						RelationGetRelationName(a->rel))));
	/* (closed and opened again under begin()'s owner, which holds them) */
	CurrentResourceOwner = a->owner;
	ExecCloseIndices(a->rri);
	CurrentResourceOwner = owner;
	a->rri->ri_NumIndices = 0;
	a->rri->ri_IndexRelationDescs = NULL;
	a->rri->ri_IndexRelationInfo = NULL;
	CommandCounterIncrement();
	held = prolly_writing_held_rows(true);
	PG_TRY();
	{
		(void) reindex_relation(NULL, RelationGetRelid(a->rel),
								REINDEX_REL_CHECK_CONSTRAINTS, &params);
	}
	PG_FINALLY();
	{
		prolly_writing_held_rows(held);
	}
	PG_END_TRY();
	CurrentResourceOwner = a->owner;
	ExecOpenIndices(a->rri, false);
	CurrentResourceOwner = owner;
	a->failed = false;
	pgstat_count_prolly(PGSTAT_PROLLY_APPLY_LOADS, 1);
	/* (the builds made an entry in each index for each row loaded) */
	pgstat_count_prolly(PGSTAT_PROLLY_APPLY_INDEX_TUPLES,
						a->load_rows * a->rri->ri_NumIndices);
	a->load_rows = 0;
}

/*
 * The rows written are from a tree a commit held, so their keys fit one:
 * they aren't refused for their keys' length (prolly_table.c), whose bound
 * a tree written before it may exceed a little.  (Restored however the
 * write ends, its rechecks raising too.)
 */
static void
replication_insert(ProllyApplier *a, TupleTableSlot *row)
{
	bool		held;

	load_end(a);
	held = prolly_writing_held_rows(true);
	PG_TRY();
	{
		if (a->matview)
			matview_insert(a, row);
		else
		{
			rechecks_begin(a);
			ExecSimpleRelationInsert(a->rri, a->estate, row);
			rechecks_fire(a);
		}
	}
	PG_FINALLY();
	{
		prolly_writing_held_rows(held);
	}
	PG_END_TRY();
	count_applied(a, 1, true);
	if (a->rows)
		rows_add(a, row);
	advance(a);
}

/*
 * Rows inserted together, as COPY inserts them: each one's generated
 * columns computed and constraints checked, then all written into the
 * table's pages at once, then each one's index entries made (none while
 * loading: see loads()) and its rechecks queued.  As insert(), their keys
 * aren't refused for their length.  The bulk-insert state lets go of its
 * buffer once they are in: nothing stays pinned between writes.
 */
static void
replication_insert_many(ProllyApplier *a, TupleTableSlot **rows, int n)
{
	Relation	rel = a->rel;
	TupleConstr *constr = RelationGetDescr(rel)->constr;
	MemoryContext cxt;
	bool		held;

	check_usable(a);
	if (n <= 0)
		return;
	if (a->matview || a->immediate)
	{
		/* (written as a refresh writes them; or each checked as it ends) */
		for (int i = 0; i < n; i++)
			replication_insert(a, rows[i]);
		return;
	}
	if (!a->loading && loads(a, n))
	{
		a->loading = true;
		a->load_level = GetCurrentTransactionNestLevel();
	}
	if (a->bistate == NULL)
		a->bistate = GetBulkInsertState();
	CheckCmdReplicaIdentity(rel, CMD_INSERT);
	held = prolly_writing_held_rows(true);
	PG_TRY();
	{
		for (int i = 0; i < n; i++)
		{
			if (constr && constr->has_generated_stored)
				ExecComputeStoredGenerated(a->rri, a->estate, rows[i],
										   CMD_INSERT);
			if (constr)
				ExecConstraints(a->rri, rows[i], a->estate);
			if (rel->rd_rel->relispartition)
				ExecPartitionCheck(a->rri, rows[i], a->estate, true);
		}
		/* (it may leak: in the per-row memory, reset once they are in) */
		cxt = MemoryContextSwitchTo(GetPerTupleMemoryContext(a->estate));
		table_multi_insert(rel, rows, n, a->estate->es_output_cid, 0,
						   a->bistate);
		MemoryContextSwitchTo(cxt);
		for (int i = 0; i < n; i++)
		{
			List	   *recheck = NIL;

			if (a->rri->ri_NumIndices > 0 && !a->loading)
				recheck = ExecInsertIndexTuples(a->rri, a->estate, 0, rows[i],
												NIL, NULL);
			ExecARInsertTriggers(a->estate, a->rri, rows[i], recheck, NULL);
			list_free(recheck);
			if (a->rows)
				rows_add(a, rows[i]);
		}
	}
	PG_FINALLY();
	{
		prolly_writing_held_rows(held);
		ReleaseBulkInsertStatePin(a->bistate);
	}
	PG_END_TRY();
	ResetPerTupleExprContext(a->estate);
	pgstat_count_prolly(PGSTAT_PROLLY_APPLY_ROWS_BATCHED, n);
	count_applied(a, n, !a->loading);
	if (a->loading)
		a->load_rows += n;
	INJECTION_POINT("prolly-insert-many", NULL);
	advance(a);
}

/*
 * Every row removed at once, as TRUNCATE removes them (#492): the table,
 * its TOAST table and its indexes given new storage, the old dropped as
 * the transaction commits, and the change log told every row may have
 * changed.  No other transaction reads or writes the table meanwhile: it
 * is locked ACCESS EXCLUSIVE.  Not a table the session reads otherwise (an
 * open cursor's scan reads its storage) or has trigger events pending
 * for, which TRUNCATE refuses; nor a materialized view (REFRESH writes
 * its rows).
 */
static bool
replication_remove_all(ProllyApplier *a)
{
	Relation	rel = a->rel;
	Oid			relid = RelationGetRelid(rel);
	Oid			toastid = rel->rd_rel->reltoastrelid;
	ReindexParams params = {0};
	ResourceOwner owner = CurrentResourceOwner;

	load_end(a);
	if (a->matview)
		return false;
	LockRelationOid(relid, AccessExclusiveLock);
	if (rel->rd_refcnt != 1 || AfterTriggerPendingOnRel(relid))
		return false;
	CheckTableForSerializableConflictIn(rel);
	/* (closed and opened again under begin()'s owner: see load_end()) */
	CurrentResourceOwner = a->owner;
	ExecCloseIndices(a->rri);
	CurrentResourceOwner = owner;
	a->rri->ri_NumIndices = 0;
	a->rri->ri_IndexRelationDescs = NULL;
	a->rri->ri_IndexRelationInfo = NULL;
	RelationSetNewRelfilenumber(rel, rel->rd_rel->relpersistence);
	if (OidIsValid(toastid))
	{
		Relation	toast = relation_open(toastid, AccessExclusiveLock);

		RelationSetNewRelfilenumber(toast, toast->rd_rel->relpersistence);
		table_close(toast, NoLock);
	}
	(void) reindex_relation(NULL, relid, REINDEX_REL_PROCESS_TOAST, &params);
	CurrentResourceOwner = a->owner;
	ExecOpenIndices(a->rri, false);
	CurrentResourceOwner = owner;
	/* (its change log told it was emptied, as TRUNCATE tells it) */
	prolly_note_emptied(rel);
	pgstat_count_truncate(rel);
	/* (the rows by their hashes are gone with them) */
	a->rows = NULL;
	a->lookups = 0;
	advance(a);
	return true;
}

/*
 * Whether the row found is as the fold before the change read it: not
 * written since by a transaction the active snapshot doesn't see.  (The
 * row found by its key is its newest version.)
 */
static bool
found_as_read(ProllyApplier *a)
{
	bool		isnull;
	TransactionId xmin = DatumGetTransactionId(slot_getsysattr(a->found,
															   MinTransactionIdAttributeNumber,
															   &isnull));

	return TransactionIdIsCurrentTransactionId(xmin) ||
		!XidInMVCCSnapshot(xmin, GetActiveSnapshot());
}

/*
 * The columns keep marks, from the row found: its stored values.  (Never a
 * materialized view's: it has no primary key to find its rows by.)
 */
static void
keep_found(ProllyApplier *a, TupleTableSlot *row, const bool *keep)
{
	TupleDesc	desc = RelationGetDescr(a->rel);
	Datum	   *values = palloc_array(Datum, desc->natts);
	bool	   *isnull = palloc_array(bool, desc->natts);

	Assert(!a->matview);
	slot_getallattrs(a->found);
	slot_getallattrs(row);
	for (int i = 0; i < desc->natts; i++)
	{
		TupleTableSlot *from = keep[i] ? a->found : row;

		values[i] = from->tts_values[i];
		isnull[i] = from->tts_isnull[i];
	}
	/* (formed anew: the found row's values are its buffer's) */
	ExecForceStoreHeapTuple(heap_form_tuple(desc, values, isnull), row, true);
	pfree(values);
	pfree(isnull);
}

static bool
replication_update(ProllyApplier *a, TupleTableSlot *old, TupleTableSlot *row,
				   const bool *keep)
{
	bool		keeping = false;
	bool		held;
	PgStat_Counter hot;

	load_end(a);
	for (int i = 0; keep && i < RelationGetDescr(a->rel)->natts; i++)
		keeping |= keep[i];
	if (keeping)
		INJECTION_POINT("prolly-update-keeping", NULL);

	/*
	 * A row another transaction changed since the fold read it isn't old,
	 * whatever its key: its values aren't the ones to keep.
	 */
	if (!find_row(a, old) || (keeping && !found_as_read(a)))
	{
		ExecClearTuple(a->found);
		return false;
	}
	/* (its key the row found's, which a tree held: see replication_insert) */
	hot = hot_updates(a->rel);
	held = prolly_writing_held_rows(true);
	PG_TRY();
	{
		if (keeping)
			keep_found(a, row, keep);
		if (a->matview)
		{
			matview_delete(a);
			matview_insert(a, row);
		}
		else
		{
			rechecks_begin(a);
			ExecSimpleRelationUpdate(a->rri, a->estate, &a->epq, a->found,
									 row);
			rechecks_fire(a);
		}
	}
	PG_CATCH();
	{
		prolly_writing_held_rows(held);
		let_go(a);
		PG_RE_THROW();
	}
	PG_END_TRY();
	prolly_writing_held_rows(held);
	/* (a heap-only update made no index entries; a view's insert did) */
	count_applied(a, 1, a->matview || hot < 0 || hot_updates(a->rel) == hot);
	if (a->rows)
		rows_add(a, row);
	advance(a);
	return true;
}

static bool
replication_remove(ProllyApplier *a, TupleTableSlot *old)
{
	load_end(a);
	if (!find_row(a, old))
	{
		ExecClearTuple(a->found);
		return false;
	}
	PG_TRY();
	{
		if (a->matview)
			matview_delete(a);
		else
			ExecSimpleRelationDelete(a->rri, a->estate, &a->epq, a->found);
	}
	PG_CATCH();
	{
		let_go(a);
		PG_RE_THROW();
	}
	PG_END_TRY();
	count_applied(a, 1, false);
	advance(a);
	return true;
}

/*
 * The rechecks kept, made immediate ones (not deferrable, so neither
 * INITIALLY DEFERRED nor SET CONSTRAINTS defers their events), are fired
 * as each write ends.  (The trigger descriptions are kept_triggers()'
 * copies.)
 */
static void
replication_immediate(ProllyApplier *a)
{
	TriggerDesc *td = a->rri->ri_TrigDesc;

	load_end(a);
	for (int i = 0; td && i < td->numtriggers; i++)
	{
		td->triggers[i].tgdeferrable = false;
		td->triggers[i].tginitdeferred = false;
	}
	a->immediate = (td != NULL);
}

static void
replication_end(ProllyApplier *a)
{
	if (!a->failed)
		load_end(a);
	if (a->bistate)
		FreeBulkInsertState(a->bistate);
	AfterTriggerEndQuery(a->estate);
	EvalPlanQualEnd(&a->epq);
	ExecCloseIndices(a->rri);
	ExecCloseTrigTargetRelations(a->estate);
	ExecResetTupleTable(a->estate->es_tupleTable, false);
	FreeExecutorState(a->estate);
	pfree(a);
}

const ProllyApplierRoutine prolly_applier_replication = {
	.info = {
		.iface = PROLLY_IFACE_ROW_APPLIER,
		.impl_id = 1,
		.format_version = 1,
		.name = "replication",
		.description = "rows written as logical replication's apply writes "
		"them: no user triggers, no foreign key checks",
	},
	.begin = replication_begin,
	.insert = replication_insert,
	.update = replication_update,
	.remove = replication_remove,
	.immediate = replication_immediate,
	.end = replication_end,
	.insert_many = replication_insert_many,
	.remove_all = replication_remove_all,
};
