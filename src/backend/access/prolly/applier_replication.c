/*-------------------------------------------------------------------------
 *
 * applier_replication.c
 *	  Row applier implementation "replication": rows written as logical
 *	  replication's apply worker writes them.
 *
 * The executor's simple-relation routines (execReplication.c) insert,
 * update and delete with index maintenance and constraint checks; rows to
 * change are found by the primary key's index, or by a scan comparing
 * every column for a table without one.  While writing, the session acts
 * as a replica (session_replication_role), so ordinary triggers, foreign
 * key checks among them, don't fire.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/applier_replication.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_relation.h"
#include "utils/acl.h"
#include "utils/guc.h"
#include "utils/rel.h"

struct ProllyApplier
{
	Relation	rel;
	Oid			key_index;		/* the primary key's; invalid: none */
	EState	   *estate;
	ResultRelInfo *rri;
	EPQState	epq;
	TupleTableSlot *found;
	int			guc_level;
};

static ProllyApplier *
replication_begin(Relation rel)
{
	ProllyApplier *a = palloc0(sizeof(ProllyApplier));
	RangeTblEntry *rte;
	List	   *perminfos = NIL;
	if (pg_class_aclmask(RelationGetRelid(rel), GetUserId(),
						 ACL_INSERT | ACL_UPDATE | ACL_DELETE, ACLMASK_ALL) !=
		(ACL_INSERT | ACL_UPDATE | ACL_DELETE))
		aclcheck_error(ACLCHECK_NO_PRIV,
					   get_relkind_objtype(rel->rd_rel->relkind),
					   RelationGetRelationName(rel));

	a->rel = rel;
	a->key_index = RelationGetPrimaryKeyIndex(rel, false);
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
	a->found = table_slot_create(rel, &a->estate->es_tupleTable);

	/* No user triggers or foreign key checks: as a replica applies. */
	a->guc_level = NewGUCNestLevel();
	(void) set_config_option("session_replication_role", "replica",
							 PGC_SUSET, PGC_S_SESSION, GUC_ACTION_SAVE, true,
							 0, false);
	AfterTriggerBeginQuery();
	return a;
}

/* The next write sees this one. */
static void
advance(ProllyApplier *a)
{
	CommandCounterIncrement();
	a->estate->es_output_cid = GetCurrentCommandId(true);
}

/* A row equal to old (by the key, or every column) into a->found. */
static bool
find_row(ProllyApplier *a, TupleTableSlot *old)
{
	slot_getallattrs(old);
	if (OidIsValid(a->key_index))
		return RelationFindReplTupleByIndex(a->rel, a->key_index, true,
											LockTupleExclusive, old, a->found);
	return RelationFindReplTupleSeq(a->rel, LockTupleExclusive, old, a->found);
}

static void
replication_insert(ProllyApplier *a, TupleTableSlot *row)
{
	ExecSimpleRelationInsert(a->rri, a->estate, row);
	advance(a);
}

static bool
replication_update(ProllyApplier *a, TupleTableSlot *old, TupleTableSlot *row)
{
	if (!find_row(a, old))
		return false;
	ExecSimpleRelationUpdate(a->rri, a->estate, &a->epq, a->found, row);
	advance(a);
	return true;
}

static bool
replication_remove(ProllyApplier *a, TupleTableSlot *old)
{
	if (!find_row(a, old))
		return false;
	ExecSimpleRelationDelete(a->rri, a->estate, &a->epq, a->found);
	advance(a);
	return true;
}

static void
replication_end(ProllyApplier *a)
{
	AfterTriggerEndQuery(a->estate);
	AtEOXact_GUC(true, a->guc_level);
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
	.end = replication_end,
};
