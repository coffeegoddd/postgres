/*-------------------------------------------------------------------------
 *
 * prolly_vcs.c
 *	  SQL functions for version control beside the dolt_* ones: a commit's
 *	  canonical root hash, a working-set checkpoint, and the tables whose
 *	  next fold reads them in full; and the checkpoint a rewrite of a
 *	  versioned table makes.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_vcs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/prolly_config.h"
#include "access/prolly_fold.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/branch.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/versioned.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/resowner.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"

static text *
hash_to_text(const ProllyHash *hash)
{
	return cstring_to_text(prolly_hash_hex(hash));
}

/*
 * prolly_root_hash(ref text) returns text
 *
 * The canonical root hash of the commit ref names (a branch or a commit
 * id).  It depends only on the logical content (schema and data) of that
 * commit, never on how it was produced, so comparing it is how history
 * independence is tested.
 */
Datum
prolly_root_hash(PG_FUNCTION_ARGS)
{
	ProllyDatabaseComponents components;
	const ProllyRefsRoutine *refs;
	char	   *ref = text_to_cstring(PG_GETARG_TEXT_PP(0));
	ProllyHash	commit_id = {0};
	ProllyCommitInfo info;

	prolly_database_components(&components);
	refs = components.refs;
	if (!PROLLY_CALL(refs, resolve, ref, &commit_id))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("prolly ref \"%s\" does not exist", ref)));
	if (!PROLLY_CALL(refs, commit_get, &commit_id, &info))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("prolly commit for ref \"%s\" does not exist", ref)));
	PG_RETURN_TEXT_P(hash_to_text(&info.root.hash));
}

/*
 * prolly_checkpoint() returns text
 *
 * Take a working-set checkpoint of the session's branch: its working set
 * folded as a commit would fold it, without a commit, so later commits,
 * diffs and statuses start from it.  Returns the working set's canonical
 * root hash.
 */
Datum
prolly_checkpoint(PG_FUNCTION_ARGS)
{
	ProllyDatabaseComponents components;
	ProllyNodeRef root;

	prolly_database_components(&components);
	PROLLY_CALL(components.refs, checkpoint, &root);
	PG_RETURN_TEXT_P(hash_to_text(&root.hash));
}

/* Each table dolt_pending_full_folds() visits, a row of its result. */
static void
pending_full_row(const char *name, Oid relid, ProllyFullFoldReason reason,
				 void *arg)
{
	ReturnSetInfo *rsinfo = arg;
	Datum		values[3];
	bool		nulls[3] = {false, false, false};
	HeapTuple	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));

	values[0] = CStringGetTextDatum(name);
	values[1] = CStringGetTextDatum(prolly_full_fold_reason_name(reason));
	nulls[2] = true;
	if (HeapTupleIsValid(tup))
	{
		float4		reltuples = ((Form_pg_class) GETSTRUCT(tup))->reltuples;

		if (reltuples >= 0)
		{
			values[2] = Float8GetDatum(reltuples);
			nulls[2] = false;
		}
		ReleaseSysCache(tup);
	}
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

/*
 * dolt_pending_full_folds() returns setof (table_name text, reason text,
 * estimated_rows float8)
 *
 * The session branch's tables whose next fold (a commit's, a status's)
 * will read them in full, and why (pg_stat_prolly's fold_full_* reasons),
 * with the rows the table was last estimated to hold (NULL: never
 * estimated): what the next commit will cost, before it is made.  What
 * only reading a table's change log can tell (a truncation or rewrite
 * logged, more rows changed than the table holds) isn't shown.
 */
Datum
dolt_pending_full_folds(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	ProllyDatabaseComponents components;

	InitMaterializedSRF(fcinfo, 0);
	prolly_database_components(&components);
	PROLLY_CALL(components.refs, pending_full_folds, pending_full_row, rsinfo);
	return (Datum) 0;
}

/*
 * A rewrite of a table (ALTER TABLE's, REFRESH MATERIALIZED VIEW's) has
 * just read and written every row: if it is a versioned prolly table or
 * materialized view, the session's branch is folded now, as a checkpoint
 * (prolly.fold_on_rewrite), so the next commit or status folds only what
 * changed since rather than reading every row again.  The rewrite pays for
 * the fold.  Not during recovery, in a transaction that can't write, nor
 * for history work's own DDL (a move rewriting a table folds after it).
 *
 * The checkpoint never waits for a lock: the rewrite holds its table
 * exclusively, and a commit open on the branch, holding the branch, may
 * be about to read that table, so waiting for the branch (or for another
 * table a rewrite elsewhere holds) could deadlock, and would hold the
 * table from readers meanwhile.  It is made in a subtransaction with
 * lock_timeout at its least; a lock not free at once rolls it back, and
 * the table's next fold reads it in full, as without the checkpoint.
 * True if relid was such a table (folded or not): the branch's fold is
 * then tried, and needn't be for another table of the same rewrite.
 */
bool
prolly_fold_rewritten(Oid relid)
{
	ProllyDatabaseComponents components;
	ProllyNodeRef root;
	char		relkind = get_rel_relkind(relid);
	MemoryContext cxt = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	int			save_nestlevel;

	if (!prolly_fold_on_rewrite || RecoveryInProgress() || XactReadOnly ||
		branch_ddl_internal() ||
		(relkind != RELKIND_RELATION && relkind != RELKIND_MATVIEW) ||
		get_rel_relam(relid) != PROLLY_TABLE_AM_OID ||
		!namespace_is_versioned(get_rel_namespace(relid)))
		return false;
	prolly_database_components(&components);
	if (components.refs->checkpoint == NULL)
		return true;
	save_nestlevel = NewGUCNestLevel();
	(void) set_config_option("lock_timeout", "1", PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);
	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(cxt);
	PG_TRY();
	{
		BRANCH_WRITES_HUSHED(PROLLY_CALL(components.refs, checkpoint, &root));
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(cxt);
		CurrentResourceOwner = owner;
	}
	PG_CATCH();
	{
		ErrorData  *ed;

		MemoryContextSwitchTo(cxt);
		ed = CopyErrorData();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(cxt);
		CurrentResourceOwner = owner;
		if (ed->sqlerrcode != ERRCODE_LOCK_NOT_AVAILABLE)
			ReThrowError(ed);
		FlushErrorState();
		FreeErrorData(ed);
	}
	PG_END_TRY();
	AtEOXact_GUC(true, save_nestlevel);
	return true;
}
