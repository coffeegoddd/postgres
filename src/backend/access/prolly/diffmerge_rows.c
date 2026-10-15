/*-------------------------------------------------------------------------
 *
 * diffmerge_rows.c
 *	  Diff and merge implementation "rows": compare the working sets
 *	  commits recorded, table by table, and merge by applying the other
 *	  side's row changes.
 *
 * Diff resolves both refs to commits and compares, for every prolly table,
 * its data tree in the two working sets those commits recorded (the
 * engine's diff, which skips equal subtrees).
 *
 * Merge is three-way, from the newest common ancestor of the current
 * branch's head commit and the other one: for each table, the changes the
 * other side made since the ancestor are applied to the current branch as
 * row writes through the executor (as logical replication applies
 * changes), so row identity, every index and constraint, and triggers see
 * an ordinary insert, update or delete.  A key both sides changed, to
 * different rows, is a conflict, and the merge fails; a key both changed
 * alike is left alone.  A keyless table's rows have no identity, so the
 * two sides' changes to how many copies of a row there are add up.  The
 * merge then records a commit with both heads as parents, of the working
 * set this transaction will commit: it holds the branch's commit lock from
 * the start, so nothing else changes the branch meanwhile.  The branch
 * must have no changes since its head commit.
 *
 * Until catalogs are versioned, every branch has the same tables, and a
 * table created on one branch has an empty tree on the others.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/diffmerge_rows.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/prolly_builtins.h"
#include "access/prolly_config.h"
#include "access/prolly_xact.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_class.h"
#include "commands/defrem.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "parser/parse_relation.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

/* One row changed between two working sets. */
typedef struct RowChange
{
	ProllyDiffKind kind;
	StringInfoData key;
	StringInfoData before;		/* empty if added */
	StringInfoData after;		/* empty if removed */
} RowChange;

typedef struct Changes
{
	List	   *list;			/* RowChange *, in key order */
} Changes;

static void
collect_change(ProllyDiffKind kind, const ProllyKey *key,
			   const ProllyValue *before, const ProllyValue *after, void *arg)
{
	Changes    *c = arg;
	RowChange  *rc = palloc0(sizeof(RowChange));

	rc->kind = kind;
	initStringInfo(&rc->key);
	appendBinaryStringInfo(&rc->key, (const char *) key->data, key->len);
	initStringInfo(&rc->before);
	if (before)
		appendBinaryStringInfo(&rc->before, (const char *) before->data,
							   before->len);
	initStringInfo(&rc->after);
	if (after)
		appendBinaryStringInfo(&rc->after, (const char *) after->data,
							   after->len);
	c->list = lappend(c->list, rc);
}

/* The prolly tables of the database, by OID. */
static List *
prolly_tables(void)
{
	Relation	pg_class = table_open(RelationRelationId, AccessShareLock);
	SysScanDesc scan = systable_beginscan(pg_class, InvalidOid, false, NULL,
										  0, NULL);
	HeapTuple	tup;
	List	   *tables = NIL;

	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_class form = (Form_pg_class) GETSTRUCT(tup);
		char	   *amname;

		if (form->relkind != RELKIND_RELATION || !OidIsValid(form->relam))
			continue;
		amname = get_am_name(form->relam);
		if (prolly_is_table_am(amname))
			tables = lappend_oid(tables, form->oid);
		if (amname)
			pfree(amname);
	}
	systable_endscan(scan);
	table_close(pg_class, AccessShareLock);
	return tables;
}

static void
resolve_working_set(const ProllyRefsRoutine *refs, const char *ref,
					ProllyHash *commit, ProllyNodeRef *ws)
{
	if (!PROLLY_CALL(refs, resolve, ref, commit))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("prolly ref \"%s\" does not exist", ref)));
	if (!PROLLY_CALL(refs, commit_working_set, commit, ws))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly commit of ref \"%s\" has no working set", ref)));
}

/* ------------------------------------------------------------------------
 * Diff
 * ------------------------------------------------------------------------
 */

typedef struct SinkArg
{
	ProllyDiffSink *sink;
	Oid			relid;
} SinkArg;

static void
to_sink(ProllyDiffKind kind, const ProllyKey *key, const ProllyValue *before,
		const ProllyValue *after, void *arg)
{
	SinkArg    *s = arg;

	s->sink->change(s->sink, s->relid, kind, key, before, after);
}

static void
rows_diff(const char *from_ref, const char *to_ref, ProllyDiffSink *sink)
{
	ProllyDatabaseComponents dc;
	ProllyComponents c;
	ProllyHash	from_commit;
	ProllyHash	to_commit;
	ProllyNodeRef from_ws;
	ProllyNodeRef to_ws;
	ProllyRootSet *from;
	ProllyRootSet *to;
	ListCell   *lc;
	List	   *tables;

	prolly_database_components(&dc);
	prolly_components_for_engine(&prolly_engine_d1.info, &c);
	resolve_working_set(dc.refs, from_ref, &from_commit, &from_ws);
	resolve_working_set(dc.refs, to_ref, &to_commit, &to_ws);
	from = PROLLY_CALL(c.engine, roots_at, &from_ws);
	to = PROLLY_CALL(c.engine, roots_at, &to_ws);
	tables = prolly_tables();
	foreach(lc, tables)
	{
		Relation	rel = table_open(lfirst_oid(lc), AccessShareLock);
		ProllyComponents rc;
		SinkArg		s = {sink, RelationGetRelid(rel)};

		prolly_components_for_relation(rel, &rc);
		PROLLY_CALL(rc.engine, diff, from, to, rel, to_sink, &s);
		table_close(rel, AccessShareLock);
	}
	PROLLY_CALL(c.engine, roots_release, from);
	PROLLY_CALL(c.engine, roots_release, to);
}

/* ------------------------------------------------------------------------
 * Merge
 * ------------------------------------------------------------------------
 */

/* Executor state for writing one table's rows, as logical replication's. */
typedef struct Apply
{
	Relation	rel;
	ProllyComponents c;
	ProllyKeying *keying;
	EState	   *estate;
	ResultRelInfo *rri;
	EPQState	epq;
	TupleTableSlot *slot;
	TupleTableSlot *search;
	ProllyRootSet *roots;
	ProllyRowIdScan *rowids;
} Apply;

static void
apply_begin(Apply *a, Relation rel)
{
	RangeTblEntry *rte;
	List	   *perminfos = NIL;

	memset(a, 0, sizeof(*a));
	a->rel = rel;
	prolly_components_for_relation(rel, &a->c);
	a->keying = PROLLY_CALL(a->c.keying, open, rel, NULL, CurrentMemoryContext);
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
	a->estate->es_opened_result_relations =
		lappend(a->estate->es_opened_result_relations, a->rri);
	a->estate->es_output_cid = GetCurrentCommandId(true);
	ExecOpenIndices(a->rri, false);
	EvalPlanQualInit(&a->epq, a->estate, NULL, NIL, -1, NIL);
	a->slot = table_slot_create(rel, &a->estate->es_tupleTable);
	a->search = table_slot_create(rel, &a->estate->es_tupleTable);
	AfterTriggerBeginQuery();
}

static void
apply_end(Apply *a)
{
	if (a->rowids)
		PROLLY_CALL(a->c.row_identity, scan_end, a->rowids);
	if (a->roots)
		PROLLY_CALL(a->c.engine, roots_release, a->roots);
	AfterTriggerEndQuery(a->estate);
	EvalPlanQualEnd(&a->epq);
	ExecCloseIndices(a->rri);
	ExecCloseTrigTargetRelations(a->estate);
	ExecResetTupleTable(a->estate->es_tupleTable, false);
	FreeExecutorState(a->estate);
	PROLLY_CALL(a->c.keying, close, a->keying);
}

/* The row a tree value holds, and how many copies. */
static void
parse_row(Apply *a, const StringInfoData *value, ProllyValue *row,
		  uint64 *count)
{
	ProllyValue v = {(const uint8 *) value->data, value->len};

	PROLLY_CALL(a->c.keying, parse_value, a->keying, &v, row, count);
}

static void
insert_copies(Apply *a, const StringInfoData *value, uint64 n)
{
	ProllyValue row;
	uint64		count;

	parse_row(a, value, &row, &count);
	for (uint64 i = 0; i < n; i++)
	{
		ExecClearTuple(a->slot);
		PROLLY_CALL(a->c.keying, decode_row, a->keying, &row, a->slot);
		ExecSimpleRelationInsert(a->rri, a->estate, a->slot);
		CommandCounterIncrement();
		a->estate->es_output_cid = GetCurrentCommandId(true);
	}
}

/* The current version of the row at key (copy ordinal) into a->search. */
static void
find_current(Apply *a, const StringInfoData *key, uint64 ordinal)
{
	ProllyKey	k = {(const uint8 *) key->data, key->len};
	ItemPointerData tid;

	if (a->rowids)
		PROLLY_CALL(a->c.row_identity, scan_end, a->rowids);
	if (a->roots)
		PROLLY_CALL(a->c.engine, roots_release, a->roots);
	a->roots = PROLLY_CALL(a->c.engine, roots_acquire, NULL);
	a->rowids = PROLLY_CALL(a->c.row_identity, scan_begin, a->roots, a->rel,
							InvalidCommandId);
	if (!PROLLY_CALL(a->c.row_identity, tid_find, a->rowids, &k, ordinal, &tid) ||
		!table_tuple_fetch_row_version(a->rel, &tid, SnapshotAny, a->search))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("a row of \"%s\" to merge is missing",
						RelationGetRelationName(a->rel))));
}

static void
delete_copies(Apply *a, const StringInfoData *key, uint64 have, uint64 n)
{
	for (uint64 i = 0; i < n; i++)
	{
		find_current(a, key, have - 1 - i);
		ExecSimpleRelationDelete(a->rri, a->estate, &a->epq, a->search);
		CommandCounterIncrement();
		a->estate->es_output_cid = GetCurrentCommandId(true);
	}
}

static void
update_row(Apply *a, const StringInfoData *key, const StringInfoData *value)
{
	ProllyValue row;
	uint64		count;

	find_current(a, key, 0);
	parse_row(a, value, &row, &count);
	ExecClearTuple(a->slot);
	PROLLY_CALL(a->c.keying, decode_row, a->keying, &row, a->slot);
	ExecSimpleRelationUpdate(a->rri, a->estate, &a->epq, a->search, a->slot);
	CommandCounterIncrement();
	a->estate->es_output_cid = GetCurrentCommandId(true);
}

static bool
same_bytes(const StringInfoData *a, const StringInfoData *b)
{
	return a->len == b->len && memcmp(a->data, b->data, a->len) == 0;
}

static uint64
copies(Apply *a, const StringInfoData *value)
{
	ProllyValue row;
	uint64		count;

	if (value->len == 0)
		return 0;
	parse_row(a, value, &row, &count);
	return count;
}

/*
 * Apply theirs (changes since the ancestor) to rel, which has ours since
 * the ancestor already; both in key order.  Returns how many keys
 * conflict.
 */
static int
merge_table(Relation rel, List *ours, List *theirs)
{
	Apply		a;
	ListCell   *lo = list_head(ours);
	ListCell   *lt;
	int			conflicts = 0;
	bool		keyless;

	apply_begin(&a, rel);
	keyless = PROLLY_CALL(a.c.keying, keyless, a.keying);
	foreach(lt, theirs)
	{
		RowChange  *t = lfirst(lt);
		RowChange  *o = NULL;
		ProllyKey	tk = {(const uint8 *) t->key.data, t->key.len};

		while (lo)
		{
			RowChange  *cand = lfirst(lo);
			ProllyKey	ok = {(const uint8 *) cand->key.data, cand->key.len};
			int			r = PROLLY_CALL(a.c.keying, compare, a.keying, &ok, &tk);

			if (r < 0)
				lo = lnext(ours, lo);
			else
			{
				if (r == 0)
					o = cand;
				break;
			}
		}

		if (keyless)
		{
			/* Copies of a row add up: apply their change in the count. */
			int64		have = (int64) copies(&a, o ? &o->after : &t->before);
			int64		delta = (int64) copies(&a, &t->after) -
				(int64) copies(&a, &t->before);

			if (have + delta < 0)
				conflicts++;
			else if (delta > 0)
				insert_copies(&a, &t->after, delta);
			else if (delta < 0)
				delete_copies(&a, &t->key, have, -delta);
			continue;
		}

		/* Both sides changed the key: alike, or a conflict. */
		if (o != NULL)
		{
			if (!same_bytes(&o->after, &t->after))
				conflicts++;
			continue;
		}
		if (t->kind == PROLLY_DIFF_ADDED)
			insert_copies(&a, &t->after, 1);
		else if (t->kind == PROLLY_DIFF_REMOVED)
			delete_copies(&a, &t->key, 1, 1);
		else
			update_row(&a, &t->key, &t->after);
	}
	apply_end(&a);
	return conflicts;
}

static bool
same_hash(const ProllyHash *a, const ProllyHash *b)
{
	return a->len == b->len && memcmp(a->bytes, b->bytes, a->len) == 0;
}

static void
rows_merge(const char *branch, ProllyHash *commit_id)
{
	ProllyDatabaseComponents dc;
	ProllyComponents c;
	ProllyHash	ours_c;
	ProllyHash	theirs_c;
	ProllyHash	base_c;
	ProllyNodeRef ours_ws;
	ProllyNodeRef theirs_ws;
	ProllyNodeRef base_ws;
	ProllyNodeRef now;
	ProllyRootSet *base;
	ProllyRootSet *ours;
	ProllyRootSet *theirs;
	ProllyHash	parents[2];
	ProllyNodeRef merged;
	List	   *tables;
	ListCell   *lc;
	char	   *message;

	prolly_database_components(&dc);
	prolly_components_for_engine(&prolly_engine_d1.info, &c);
	if (prolly_xact_has_writes())
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("cannot merge in a transaction that has written to prolly tables")));
	if (!PROLLY_CALL(dc.refs, resolve, "HEAD", &ours_c))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("the current prolly branch has no commit to merge into")));
	resolve_working_set(dc.refs, branch, &theirs_c, &theirs_ws);
	if (!PROLLY_CALL(dc.refs, merge_base, &ours_c, &theirs_c, &base_c))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("prolly branch \"%s\" has no history in common with the current one",
						branch)));
	if (same_hash(&base_c, &theirs_c))
	{
		*commit_id = ours_c;	/* already merged */
		return;
	}

	/*
	 * Nothing else commits to the branch until this transaction ends, and
	 * the merge starts from the branch's head commit.
	 */
	PROLLY_CALL(c.commit, branch_lock, prolly_session_branch());
	(void) PROLLY_CALL(dc.refs, commit_working_set, &ours_c, &ours_ws);
	if (!PROLLY_CALL(c.commit, tip, prolly_session_branch(), &now) ||
		!same_hash(&now.hash, &ours_ws.hash))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("the current prolly branch has changes since its last commit"),
				 errhint("Commit them before merging.")));
	(void) PROLLY_CALL(dc.refs, commit_working_set, &base_c, &base_ws);
	base = PROLLY_CALL(c.engine, roots_at, &base_ws);
	ours = PROLLY_CALL(c.engine, roots_at, &ours_ws);
	theirs = PROLLY_CALL(c.engine, roots_at, &theirs_ws);

	PushActiveSnapshot(GetTransactionSnapshot());
	tables = prolly_tables();
	foreach(lc, tables)
	{
		Relation	rel = table_open(lfirst_oid(lc), RowExclusiveLock);
		ProllyComponents rc;
		Changes		ours_changes = {NIL};
		Changes		theirs_changes = {NIL};
		int			n;

		prolly_components_for_relation(rel, &rc);
		PROLLY_CALL(rc.engine, diff, base, theirs, rel, collect_change,
					&theirs_changes);
		if (theirs_changes.list != NIL)
		{
			PROLLY_CALL(rc.engine, diff, base, ours, rel, collect_change,
						&ours_changes);
			n = merge_table(rel, ours_changes.list, theirs_changes.list);
			if (n > 0)
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("prolly merge of branch \"%s\" conflicts in table \"%s\"",
								branch, RelationGetRelationName(rel)),
						 errdetail_plural("%d row was changed differently on both branches.",
										  "%d rows were changed differently on both branches.",
										  n, n)));
		}
		table_close(rel, NoLock);
	}
	PopActiveSnapshot();

	/* The merge commit: both heads, and the working set this commits. */
	PROLLY_CALL(c.engine, pending_root, &merged);
	parents[0] = ours_c;
	parents[1] = theirs_c;
	message = psprintf("Merge branch '%s'", branch);
	PROLLY_CALL(dc.refs, commit_with, message, parents, 2, &merged, commit_id);
	PROLLY_CALL(c.engine, roots_release, base);
	PROLLY_CALL(c.engine, roots_release, ours);
	PROLLY_CALL(c.engine, roots_release, theirs);
}

const ProllyMergeRoutine prolly_merge_rows = {
	.info = {
		.iface = PROLLY_IFACE_DIFF_MERGE,
		.impl_id = 1,
		.format_version = 1,
		.name = "rows",
		.description = "diff committed working sets table by table; merge by "
		"applying the other side's row changes",
	},
	.diff = rows_diff,
	.merge = rows_merge,
};
