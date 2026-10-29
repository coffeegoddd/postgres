/*-------------------------------------------------------------------------
 *
 * prolly_pages.c
 *	  Write whole pages back into a relation, for restoring a working set
 *	  from a commit image.
 *
 * Pages are written through the buffer manager under an exclusive lock on
 * the relation and logged as full page images, so the result is crash safe
 * and replicated like any other change.  Reading pages back out is done
 * with pageinspect's get_raw_page.
 *
 * prolly_put_pages takes the pages as an argument; prolly_put_pages_file
 * reads them from a file on the server, as a restore reading its own chunk
 * store would, and can log them XLR_MAX_BLOCK_ID to a record.
 *
 * prolly_apply_insert and prolly_apply_delete write rows the way version
 * control must: through the executor's simple apply paths (as logical
 * replication applies rows), keeping indexes, constraints and stored
 * generated columns, but with no trigger descriptor and below the rule
 * rewriter, so no user code runs.  A checkout or merge reproduces rows a
 * commit already holds, trigger effects included; firing again would
 * apply them twice.
 *
 * prolly_get_pages_frozen reads pages for a keyframe with every heap tuple
 * resolved against the caller's snapshot: the visible ones frozen, the
 * rest gone.  A raw page names transactions, which the server forgets as
 * its freeze horizon moves; a frozen copy names none, so it can be put
 * back however long after.
 *
 * contrib/prolly_pages/prolly_pages.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <unistd.h>

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/tableam.h"
#include "commands/copy.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "parser/parse_node.h"
#include "parser/parse_relation.h"
#include "utils/lsyscache.h"
#include "access/xlog_internal.h"
#include "catalog/pg_control.h"
#include "access/xloginsert.h"
#include "catalog/pg_type.h"
#include "catalog/storage.h"
#include "common/relpath.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/smgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(prolly_put_pages);
PG_FUNCTION_INFO_V1(prolly_truncate);
PG_FUNCTION_INFO_V1(prolly_put_pages_file);
PG_FUNCTION_INFO_V1(prolly_get_pages_frozen);
PG_FUNCTION_INFO_V1(prolly_apply_insert);
PG_FUNCTION_INFO_V1(prolly_apply_delete);

static Relation
open_for_write(Oid relid)
{
	Relation	rel;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to write raw pages")));
	rel = relation_open(relid, AccessExclusiveLock);
	if (!RELKIND_HAS_STORAGE(rel->rd_rel->relkind))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" has no storage",
						RelationGetRelationName(rel))));
	return rel;
}

/*
 * prolly_put_pages(rel regclass, fork text, blocks int4[], pages bytea)
 *
 * pages holds one BLCKSZ page per block, back to back.  Writes each page at
 * its block, extending the fork (creating it if needed) when the block lies
 * past its end.  Returns the number written.
 */
Datum
prolly_put_pages(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	char	   *forkname = text_to_cstring(PG_GETARG_TEXT_PP(1));
	ArrayType  *barr = PG_GETARG_ARRAYTYPE_P(2);
	bytea	   *pages = PG_GETARG_BYTEA_PP(3);
	ForkNumber	fork = forkname_to_number(forkname);
	Relation	rel = open_for_write(relid);
	Datum	   *blocks;
	bool	   *bnulls;
	int			nb;

	deconstruct_array_builtin(barr, INT4OID, &blocks, &bnulls, &nb);
	if (VARSIZE_ANY_EXHDR(pages) != (size_t) nb * BLCKSZ)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%d blocks need %zu bytes of pages, got %zu", nb,
						(size_t) nb * BLCKSZ, VARSIZE_ANY_EXHDR(pages))));

	for (int i = 0; i < nb; i++)
	{
		BlockNumber blk;
		const char *page = VARDATA_ANY(pages) + (size_t) i * BLCKSZ;
		Buffer		buf;

		if (bnulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("null block number")));
		blk = (BlockNumber) DatumGetInt32(blocks[i]);

		if (smgrexists(RelationGetSmgr(rel), fork) &&
			blk < RelationGetNumberOfBlocksInFork(rel, fork))
			buf = ReadBufferExtended(rel, fork, blk, RBM_ZERO_AND_LOCK, NULL);
		else
			buf = ExtendBufferedRelTo(BMR_REL(rel), fork, NULL,
									  EB_CREATE_FORK_IF_NEEDED, blk + 1,
									  RBM_ZERO_AND_LOCK);

		START_CRIT_SECTION();
		memcpy(BufferGetPage(buf), page, BLCKSZ);
		MarkBufferDirty(buf);
		/* map forks keep their data in what a standard page calls the hole */
		if (RelationNeedsWAL(rel))
			log_newpage_buffer(buf, fork == MAIN_FORKNUM);
		END_CRIT_SECTION();
		UnlockReleaseBuffer(buf);
	}

	/* index access methods cache their metapages in the relcache */
	CacheInvalidateRelcache(rel);
	relation_close(rel, NoLock);
	PG_RETURN_INT32(nb);
}

/* The buffer for block blk, zeroed and locked, extending the fork to it. */
static Buffer
zeroed_buffer(Relation rel, ForkNumber fork, BlockNumber blk)
{
	if (smgrexists(RelationGetSmgr(rel), fork) &&
		blk < RelationGetNumberOfBlocksInFork(rel, fork))
		return ReadBufferExtended(rel, fork, blk, RBM_ZERO_AND_LOCK, NULL);
	return ExtendBufferedRelTo(BMR_REL(rel), fork, NULL, EB_CREATE_FORK_IF_NEEDED,
							   blk + 1, RBM_ZERO_AND_LOCK);
}

/*
 * prolly_put_pages_file(rel regclass, fork text, blocks int4[], path text,
 *						 batched bool)
 *
 * The file at path holds one BLCKSZ page per block, back to back.  Like
 * prolly_put_pages; with batched, up to XLR_MAX_BLOCK_ID pages share one
 * WAL record.  Returns the number written.
 */
Datum
prolly_put_pages_file(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	char	   *forkname = text_to_cstring(PG_GETARG_TEXT_PP(1));
	ArrayType  *barr = PG_GETARG_ARRAYTYPE_P(2);
	char	   *path = text_to_cstring(PG_GETARG_TEXT_PP(3));
	bool		batched = PG_GETARG_BOOL(4);
	ForkNumber	fork = forkname_to_number(forkname);
	Relation	rel = open_for_write(relid);
	bool		wal = RelationNeedsWAL(rel);
	bool		std = fork == MAIN_FORKNUM;
	int			per = batched ? XLR_MAX_BLOCK_ID : 1;
	Datum	   *blocks;
	bool	   *bnulls;
	int			nb,
				fd;
	char	   *pages = palloc((size_t) XLR_MAX_BLOCK_ID * BLCKSZ);

	deconstruct_array_builtin(barr, INT4OID, &blocks, &bnulls, &nb);
	for (int i = 0; i < nb; i++)
		if (bnulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("null block number")));
	fd = open(path, O_RDONLY, 0);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open \"%s\": %m", path)));
	if (wal && batched)
		XLogEnsureRecordSpace(XLR_MAX_BLOCK_ID - 1, 0);

	for (int a = 0; a < nb; a += per)
	{
		int			m = nb - a < per ? nb - a : per;
		Buffer		bufs[XLR_MAX_BLOCK_ID];
		ssize_t		got = read(fd, pages, (size_t) m * BLCKSZ);

		if (got != (ssize_t) m * BLCKSZ)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("\"%s\" ends before page %d of %d", path, a + m, nb)));
		CHECK_FOR_INTERRUPTS();
		for (int i = 0; i < m; i++)
			bufs[i] = zeroed_buffer(rel, fork, (BlockNumber) DatumGetInt32(blocks[a + i]));

		START_CRIT_SECTION();
		for (int i = 0; i < m; i++)
		{
			memcpy(BufferGetPage(bufs[i]), pages + (size_t) i * BLCKSZ, BLCKSZ);
			MarkBufferDirty(bufs[i]);
		}
		if (wal)
		{
			XLogRecPtr	recptr;

			XLogBeginInsert();
			for (int i = 0; i < m; i++)
				XLogRegisterBuffer(i, bufs[i],
								   REGBUF_FORCE_IMAGE | (std ? REGBUF_STANDARD : 0));
			recptr = XLogInsert(RM_XLOG_ID, XLOG_FPI);
			for (int i = 0; i < m; i++)
				if (!PageIsNew(BufferGetPage(bufs[i])))
					PageSetLSN(BufferGetPage(bufs[i]), recptr);
		}
		END_CRIT_SECTION();
		for (int i = 0; i < m; i++)
			UnlockReleaseBuffer(bufs[i]);
	}
	close(fd);
	pfree(pages);

	CacheInvalidateRelcache(rel);
	relation_close(rel, NoLock);
	PG_RETURN_INT32(nb);
}

/* A visible tuple, frozen: no transaction named, no update after it. */
static void
freeze_tuple(HeapTupleHeader t, BlockNumber blk, OffsetNumber off)
{
	t->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_COMBOCID | HEAP_MOVED);
	t->t_infomask |= HEAP_XMAX_INVALID;
	HeapTupleHeaderSetXminFrozen(t);
	HeapTupleHeaderSetXmax(t, InvalidTransactionId);
	t->t_infomask2 &= ~(HEAP_KEYS_UPDATED | HEAP_HOT_UPDATED);
	ItemPointerSet(&t->t_ctid, blk, off);
}

/*
 * Rewrites copy, a copy of the heap page in buf, as the snapshot sees it.
 * Each HOT chain (a root, or a redirect to one, and the heap-only tuples
 * its updates made) keeps at most its one visible member, frozen: a
 * redirect from the root reaches it if it is not the root, so index
 * entries, which point at roots, still find it.  A chain with nothing
 * visible leaves its root dead, where indexes may still point; dropped
 * heap-only tuples become unused, as no index points at them.  The live
 * page is not changed, beyond hint bits a visibility check may set.
 */
static void
freeze_heap_page(Relation rel, Buffer buf, Page copy, Snapshot snap)
{
	Page		live = BufferGetPage(buf);
	BlockNumber blk = BufferGetBlockNumber(buf);
	OffsetNumber maxoff = PageGetMaxOffsetNumber(live);
	bool		visible[MaxHeapTuplesPerPage + 1];
	bool		inchain[MaxHeapTuplesPerPage + 1];
	bool		freed = false;

	memset(visible, 0, sizeof(visible));
	memset(inchain, 0, sizeof(inchain));
	for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId		lp = PageGetItemId(live, off);
		HeapTupleData tup;

		if (!ItemIdIsNormal(lp))
			continue;
		tup.t_data = (HeapTupleHeader) PageGetItem(live, lp);
		tup.t_len = ItemIdGetLength(lp);
		tup.t_tableOid = RelationGetRelid(rel);
		ItemPointerSet(&tup.t_self, blk, off);
		visible[off] = HeapTupleSatisfiesVisibility(&tup, snap, buf);
	}

	for (OffsetNumber root = FirstOffsetNumber; root <= maxoff; root++)
	{
		ItemId		lp = PageGetItemId(live, root);
		bool		redirect = ItemIdIsRedirected(lp);
		OffsetNumber chain[MaxHeapTuplesPerPage];
		int			n = 0;
		OffsetNumber cur,
					v = InvalidOffsetNumber;
		TransactionId prior = InvalidTransactionId;

		if (redirect)
			cur = ItemIdGetRedirect(lp);
		else if (ItemIdIsNormal(lp) &&
				 !HeapTupleHeaderIsHeapOnly((HeapTupleHeader) PageGetItem(live, lp)))
			cur = root;
		else
			continue;

		while (cur >= FirstOffsetNumber && cur <= maxoff && !inchain[cur])
		{
			ItemId		clp = PageGetItemId(live, cur);
			HeapTupleHeader t;

			if (!ItemIdIsNormal(clp))
				break;
			t = (HeapTupleHeader) PageGetItem(live, clp);
			if (n > 0 && (!HeapTupleHeaderIsHeapOnly(t) ||
						  !TransactionIdEquals(HeapTupleHeaderGetXmin(t), prior)))
				break;
			chain[n++] = cur;
			inchain[cur] = true;
			if (!HeapTupleHeaderIsHotUpdated(t) ||
				ItemPointerGetBlockNumber(&t->t_ctid) != blk)
				break;
			prior = HeapTupleHeaderGetUpdateXid(t);
			cur = ItemPointerGetOffsetNumber(&t->t_ctid);
		}
		for (int i = 0; i < n && v == InvalidOffsetNumber; i++)
			if (visible[chain[i]])
				v = chain[i];

		for (int i = 0; i < n; i++)
		{
			ItemId		cp = PageGetItemId(copy, chain[i]);

			if (chain[i] == v)
				freeze_tuple((HeapTupleHeader) PageGetItem(copy, cp), blk, chain[i]);
			else if (chain[i] == root)
			{
				if (v != InvalidOffsetNumber)
					ItemIdSetRedirect(cp, v);
				else
					ItemIdSetDead(cp);
			}
			else
			{
				ItemIdSetUnused(cp);
				freed = true;
			}
		}
		if (redirect)
		{
			ItemId		cp = PageGetItemId(copy, root);

			if (v != InvalidOffsetNumber)
				ItemIdSetRedirect(cp, v);
			else
				ItemIdSetDead(cp);
		}
	}

	/* heap-only tuples no chain reached: nothing can find them */
	for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
		if (!inchain[off] && ItemIdIsNormal(PageGetItemId(live, off)))
		{
			ItemIdSetUnused(PageGetItemId(copy, off));
			freed = true;
		}
	((PageHeader) copy)->pd_prune_xid = InvalidTransactionId;
	if (freed)
		PageSetHasFreeLinePointers(copy);
}

/*
 * prolly_get_pages_frozen(rel regclass, fork text, blocks int4[])
 *
 * The pages at the blocks, back to back, as the caller's snapshot sees
 * them: for a table's (or TOAST table's) main fork, every tuple frozen or
 * gone (see freeze_heap_page); anything else as it is.  Run it in a
 * REPEATABLE READ transaction with the commit's exported snapshot to read
 * a keyframe as of that commit.
 */
Datum
prolly_get_pages_frozen(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	char	   *forkname = text_to_cstring(PG_GETARG_TEXT_PP(1));
	ArrayType  *barr = PG_GETARG_ARRAYTYPE_P(2);
	ForkNumber	fork = forkname_to_number(forkname);
	Relation	rel;
	Datum	   *blocks;
	bool	   *bnulls;
	int			nb;
	bytea	   *out;
	bool		heap;
	Snapshot	snap = GetActiveSnapshot();

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to read raw pages")));
	rel = relation_open(relid, AccessShareLock);
	heap = fork == MAIN_FORKNUM &&
		(rel->rd_rel->relkind == RELKIND_RELATION ||
		 rel->rd_rel->relkind == RELKIND_TOASTVALUE ||
		 rel->rd_rel->relkind == RELKIND_MATVIEW);
	deconstruct_array_builtin(barr, INT4OID, &blocks, &bnulls, &nb);
	out = palloc(VARHDRSZ + (Size) nb * BLCKSZ);
	SET_VARSIZE(out, VARHDRSZ + (Size) nb * BLCKSZ);

	for (int i = 0; i < nb; i++)
	{
		Page		copy = (Page) (VARDATA(out) + (Size) i * BLCKSZ);
		Buffer		buf;

		if (bnulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("null block number")));
		CHECK_FOR_INTERRUPTS();
		buf = ReadBufferExtended(rel, fork, (BlockNumber) DatumGetInt32(blocks[i]),
								 RBM_NORMAL, NULL);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		memcpy(copy, BufferGetPage(buf), BLCKSZ);
		if (heap && !PageIsNew(copy))
			freeze_heap_page(rel, buf, copy, snap);
		UnlockReleaseBuffer(buf);
	}
	relation_close(rel, AccessShareLock);
	PG_RETURN_BYTEA_P(out);
}

/* ------------------------------------------------------------ row apply */

/* COPY text handed to COPY's reader as its data source. */
static const char *apply_data;
static size_t apply_left;

static int
apply_read(void *outbuf, int minread, int maxread)
{
	int			n = apply_left < (size_t) maxread ? (int) apply_left : maxread;

	memcpy(outbuf, apply_data, n);
	apply_data += n;
	apply_left -= n;
	return n;
}

typedef struct Apply
{
	Relation	rel;
	EState	   *estate;
	ResultRelInfo *rri;
	CopyFromState cstate;
} Apply;

/*
 * Opens rel for applying rows: an executor state with rel as its result
 * relation, its indexes open, and no triggers; and COPY's reader over
 * data for the columns named.
 */
static void
apply_begin(Apply *a, Oid relid, ArrayType *cols, text *data)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	List	   *perminfos = NIL;
	List	   *names = NIL;
	Datum	   *cv;
	bool	   *cn;
	int			nc;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to apply rows")));
	a->rel = table_open(relid, RowExclusiveLock);
	if (a->rel->rd_rel->relkind != RELKIND_RELATION)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table: rows go to its partitions",
						RelationGetRelationName(a->rel))));
	a->estate = CreateExecutorState();
	rte->rtekind = RTE_RELATION;
	rte->relid = relid;
	rte->relkind = a->rel->rd_rel->relkind;
	rte->rellockmode = RowExclusiveLock;
	addRTEPermissionInfo(&perminfos, rte);
	ExecInitRangeTable(a->estate, list_make1(rte), perminfos, bms_make_singleton(1));
	a->rri = makeNode(ResultRelInfo);
	InitResultRelInfo(a->rri, a->rel, 1, NULL, 0);
	/* version control runs no user code: no triggers, and no rules below the rewriter */
	a->rri->ri_TrigDesc = NULL;
	a->estate->es_opened_result_relations = lappend(a->estate->es_opened_result_relations, a->rri);
	a->estate->es_output_cid = GetCurrentCommandId(true);
	ExecOpenIndices(a->rri, false);

	deconstruct_array_builtin(cols, TEXTOID, &cv, &cn, &nc);
	for (int i = 0; i < nc; i++)
		names = lappend(names, makeString(TextDatumGetCString(cv[i])));
	apply_data = VARDATA_ANY(data);
	apply_left = VARSIZE_ANY_EXHDR(data);
	a->cstate = BeginCopyFrom(make_parsestate(NULL), a->rel, NULL, NULL, false, apply_read,
							  names, NIL);
}

static void
apply_end(Apply *a)
{
	EndCopyFrom(a->cstate);
	ExecCloseIndices(a->rri);
	ExecResetTupleTable(a->estate->es_tupleTable, false);
	FreeExecutorState(a->estate);
	table_close(a->rel, NoLock);
}

/*
 * prolly_apply_insert(rel regclass, cols text[], data text) returns int8
 *
 * Inserts each line of data, COPY text for the columns named, as a row.
 * Columns not named get their defaults, as in COPY; identity columns take
 * the values given; stored generated columns are computed.
 */
Datum
prolly_apply_insert(PG_FUNCTION_ARGS)
{
	Apply		a;
	TupleTableSlot *slot;
	ExprContext *econtext;
	int64		n = 0;

	apply_begin(&a, PG_GETARG_OID(0), PG_GETARG_ARRAYTYPE_P(1), PG_GETARG_TEXT_PP(2));
	slot = table_slot_create(a.rel, &a.estate->es_tupleTable);
	econtext = GetPerTupleExprContext(a.estate);
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();
		ResetPerTupleExprContext(a.estate);
		ExecClearTuple(slot);
		if (!NextCopyFrom(a.cstate, econtext, slot->tts_values, slot->tts_isnull))
			break;
		ExecStoreVirtualTuple(slot);
		ExecSimpleRelationInsert(a.rri, a.estate, slot);
		n++;
	}
	apply_end(&a);
	PG_RETURN_INT64(n);
}

/*
 * prolly_apply_delete(rel regclass, cols text[], data text) returns int8
 *
 * Deletes, for each line of data (COPY text for the columns named), one
 * row that has those values: found through the table's replica identity
 * index when the columns are its key, or by comparing every column when
 * the table's identity is FULL (a keyless table, each line one copy).
 * Returns the rows deleted; a line that matches nothing deletes nothing.
 */
Datum
prolly_apply_delete(PG_FUNCTION_ARGS)
{
	Apply		a;
	TupleTableSlot *search,
			   *found;
	EPQState	epq;
	Oid			idx;
	TupleDesc	desc;
	List	   *attnums = NIL;
	int64		n = 0;
	ArrayType  *cols = PG_GETARG_ARRAYTYPE_P(1);

	apply_begin(&a, PG_GETARG_OID(0), cols, PG_GETARG_TEXT_PP(2));
	desc = RelationGetDescr(a.rel);
	idx = RelationGetReplicaIndex(a.rel);
	{
		Datum	   *cv;
		bool	   *cn;
		int			nc;

		deconstruct_array_builtin(cols, TEXTOID, &cv, &cn, &nc);
		for (int i = 0; i < nc; i++)
		{
			AttrNumber	at = attnameAttNum(a.rel, TextDatumGetCString(cv[i]), false);

			if (at == InvalidAttrNumber)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column \"%s\" does not exist", TextDatumGetCString(cv[i]))));
			attnums = lappend_int(attnums, at);
		}
	}
	search = table_slot_create(a.rel, &a.estate->es_tupleTable);
	found = table_slot_create(a.rel, &a.estate->es_tupleTable);
	EvalPlanQualInit(&epq, a.estate, NULL, NIL, -1, NIL);
	for (;;)
	{
		char	  **fields;
		int			nf;
		ListCell   *lc;
		int			i = 0;
		bool		hit;

		CHECK_FOR_INTERRUPTS();
		if (!NextCopyFromRawFields(a.cstate, &fields, &nf))
			break;
		if (nf != list_length(attnums))
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("%d fields for %d columns", nf, list_length(attnums))));
		/* the named columns from their text; nothing else is evaluated */
		ExecClearTuple(search);
		for (int c = 0; c < desc->natts; c++)
		{
			search->tts_values[c] = (Datum) 0;
			search->tts_isnull[c] = true;
		}
		foreach(lc, attnums)
		{
			AttrNumber	at = lfirst_int(lc);
			Form_pg_attribute att = TupleDescAttr(desc, at - 1);
			Oid			infunc,
						ioparam;

			if (fields[i] != NULL)
			{
				getTypeInputInfo(att->atttypid, &infunc, &ioparam);
				search->tts_values[at - 1] = OidInputFunctionCall(infunc, fields[i], ioparam,
																 att->atttypmod);
				search->tts_isnull[at - 1] = false;
			}
			i++;
		}
		ExecStoreVirtualTuple(search);
		/* the identity index is unique: its key alone names the row */
		if (OidIsValid(idx))
			hit = RelationFindReplTupleByIndex(a.rel, idx, true, LockTupleExclusive, search, found);
		else
			hit = RelationFindReplTupleSeq(a.rel, LockTupleExclusive, search, found);
		if (hit)
		{
			EvalPlanQualSetSlot(&epq, found);
			ExecSimpleRelationDelete(a.rri, a.estate, &epq, found);
			n++;
		}
		/* each delete must be seen by the next search: two copies are two rows */
		CommandCounterIncrement();
		a.estate->es_output_cid = GetCurrentCommandId(true);
	}
	EvalPlanQualEnd(&epq);
	apply_end(&a);
	PG_RETURN_INT64(n);
}

/*
 * prolly_truncate(rel regclass, nblocks int4)
 *
 * Shrinks the relation's main fork to nblocks, and its maps to match.
 */
Datum
prolly_truncate(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int32		nblocks = PG_GETARG_INT32(1);
	Relation	rel = open_for_write(relid);

	if (nblocks < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("negative block count")));
	if ((BlockNumber) nblocks < RelationGetNumberOfBlocks(rel))
		RelationTruncate(rel, (BlockNumber) nblocks);
	CacheInvalidateRelcache(rel);
	relation_close(rel, NoLock);
	PG_RETURN_VOID();
}
