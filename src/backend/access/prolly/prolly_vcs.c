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
#include "pgstat.h"
#include "catalog/pg_type_d.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/resowner.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
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
 * the table's next fold reads it in full, as without the checkpoint.  Any
 * other error does the same, logged, but for a cancel or an internal
 * error (class XX): the checkpoint is an optimization, never a reason for
 * the rewrite to fail.  Each skipped checkpoint is counted
 * (pg_stat_prolly's rewrite_checkpoints_skipped).  True if relid was such
 * a table (folded or not): the branch's fold is then tried, and needn't be
 * for another table of the same rewrite.
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
		/*
		 * Nor does any other error the fold meets fail the rewrite: the
		 * checkpoint folds every table of the branch, and one the next
		 * commit refuses (a value its type's text form no longer reads
		 * back as, say) is that commit's to report, not this table's
		 * ALTER's.  A cancel, a timeout or a shutdown (class 57), running
		 * out of memory or disk (53), a system error (58), an internal
		 * error or corruption (XX), or a deadlock or serialization failure
		 * (40) is never set aside.  Another error is logged as it was
		 * raised, its fields kept, under a message naming the table.
		 */
		switch (ERRCODE_TO_CATEGORY(ed->sqlerrcode))
		{
			case ERRCODE_OPERATOR_INTERVENTION:
			case ERRCODE_INSUFFICIENT_RESOURCES:
			case ERRCODE_SYSTEM_ERROR:
			case ERRCODE_INTERNAL_ERROR:
			case ERRCODE_TRANSACTION_ROLLBACK:
				ReThrowError(ed);
		}
		FlushErrorState();
		pgstat_count_prolly(PGSTAT_PROLLY_REWRITE_CHECKPOINTS_SKIPPED, 1);
		if (ed->sqlerrcode != ERRCODE_LOCK_NOT_AVAILABLE)
		{
			char	   *nsp = get_namespace_name(get_rel_namespace(relid));
			char	   *name = quote_qualified_identifier(nsp, get_rel_name(relid));

			ed->elevel = LOG;
			ed->output_to_client = false;
			ed->message = psprintf("branch not checkpointed after rewriting %s: %s",
								   name, ed->message);
			ThrowErrorData(ed);
		}
		FreeErrorData(ed);
	}
	PG_END_TRY();
	AtEOXact_GUC(true, save_nestlevel);
	return true;
}

/* A log's name: a branch, or "remotes/<remote>/<branch>" (its remote's). */
typedef struct LogName
{
	const char *ref;
	char	   *remote;			/* the longest remote ref begins with */
} LogName;

static void
match_remote(const char *name, const char *url, void *arg)
{
	LogName    *ln = arg;
	size_t		len = strlen(name);

	if (strncmp(ln->ref, name, len) == 0 && ln->ref[len] == '/' &&
		(ln->remote == NULL || strlen(ln->remote) < len))
		ln->remote = pstrdup(name);
}

static void
log_name(const ProllyRefsRoutine *refs, const char *ref, const char **name,
		 const char **branch)
{
	if (!PROLLY_HAS(refs, head_at))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("refs implementation \"%s\" keeps no head logs",
						refs->info.name)));
	*name = ref;
	*branch = NULL;

	/*
	 * ("remotes/" names a remote-tracking branch, of a remote that must
	 * exist, as a branch of that name would be one no fetch makes; it may
	 * have no log yet)
	 */
	if (strncmp(ref, "remotes/", 8) == 0)
	{
		LogName		ln = {ref + 8, NULL};

		PROLLY_CALL(refs, remotes, match_remote, &ln);
		if (ln.remote == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("no remote is named by \"%s\"", ref),
					 errhint("A remote-tracking branch is \"remotes/<remote>/<branch>\".")));
		*name = ln.remote;
		*branch = ref + 8 + strlen(ln.remote) + 1;
		return;
	}
	{
		ProllyBranchId id;

		if (!PROLLY_CALL(refs, branch_lookup, ref, &id))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("branch \"%s\" does not exist", ref)));
	}
}

static bool
head_move_row(const ProllyHeadMove *m, void *arg)
{
	ReturnSetInfo *rsinfo = arg;
	Datum		values[3];
	bool		nulls[3] = {false, false, false};

	values[0] = Int64GetDatum(m->seq);
	values[1] = TimestampTzGetDatum(m->moved_at);
	values[2] = PointerGetDatum(hash_to_text(&m->commit));
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	return true;
}

static Datum
head_log(FunctionCallInfo fcinfo, TimestampTz from, TimestampTz to,
		 bool backward)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	ProllyDatabaseComponents dc;
	char	   *ref;
	const char *name;
	const char *branch;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("a head log's ref can't be null")));
	ref = text_to_cstring(PG_GETARG_TEXT_PP(0));
	prolly_database_components(&dc);
	log_name(dc.refs, ref, &name, &branch);
	InitMaterializedSRF(fcinfo, 0);
	PROLLY_CALL(dc.refs, head_moves, name, branch, from, to,
				backward ? BackwardScanDirection : ForwardScanDirection,
				head_move_row, rsinfo);
	return (Datum) 0;
}

/*
 * prolly_head_log(ref text)
 *	returns setof (seq bigint, moved_at timestamptz, commit_hash text)
 *
 * A branch's head log (ref "remotes/<remote>/<branch>": a remote-tracking
 * branch's): each move of its head, in order, with when it was made and
 * the commit it moved to.  A move's date is when it was made in its
 * transaction, not when that committed.  An unknown branch or remote is
 * an error.
 */
Datum
prolly_head_log(PG_FUNCTION_ARGS)
{
	return head_log(fcinfo, DT_NOBEGIN, DT_NOEND, false);
}

/*
 * prolly_head_log_range(ref text, from_time timestamptz,
 *						 to_time timestamptz, backward bool)
 *	returns setof (seq bigint, moved_at timestamptz, commit_hash text)
 *
 * The moves of ref's head log dated from_time..to_time (NULL: unbounded),
 * newest first if backward (NULL: not).  ref can't be NULL.
 */
Datum
prolly_head_log_range(PG_FUNCTION_ARGS)
{
	return head_log(fcinfo,
					PG_ARGISNULL(1) ? DT_NOBEGIN : PG_GETARG_TIMESTAMPTZ(1),
					PG_ARGISNULL(2) ? DT_NOEND : PG_GETARG_TIMESTAMPTZ(2),
					!PG_ARGISNULL(3) && PG_GETARG_BOOL(3));
}

/*
 * prolly_head_at(ref text, at timestamptz) returns text
 *
 * The commit ref's head was at at time at (its log's move last made at or
 * before it; a move dated when it was made in its transaction); NULL if it
 * had none then.  An unknown branch or remote is an error.
 */
Datum
prolly_head_at(PG_FUNCTION_ARGS)
{
	ProllyDatabaseComponents dc;
	char	   *ref = text_to_cstring(PG_GETARG_TEXT_PP(0));
	TimestampTz at = PG_GETARG_TIMESTAMPTZ(1);
	const char *name;
	const char *branch;
	ProllyHeadMove m;

	prolly_database_components(&dc);
	log_name(dc.refs, ref, &name, &branch);
	if (!PROLLY_CALL(dc.refs, head_at, name, branch, at, &m))
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(hash_to_text(&m.commit));
}

static const ProllyWindowRoutine *
database_window(void)
{
	ProllyDatabaseComponents dc;

	prolly_database_components(&dc);
	if (dc.history_window == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("this database's configuration has no history window")));
	return dc.history_window;
}

static Datum
hashes_datum(const ProllyHash *hashes, int n)
{
	Datum	   *d = palloc_array(Datum, Max(n, 1));

	for (int i = 0; i < n; i++)
		d[i] = PointerGetDatum(hash_to_text(&hashes[i]));
	return PointerGetDatum(construct_array_builtin(d, n, TEXTOID));
}

/*
 * dolt_history_drop(before timestamptz)
 *	returns record (fragment bigint, commits bigint, boundary text[],
 *					chunks bigint, bytes bigint)
 *
 * Sever the history before before and let it go, archiving none of it:
 * every branch keeps its commits dated at or after it, its head, and the
 * commits down to its merge bases with the other branches; every commit a
 * ref names is kept.  Hashes don't change: the boundary commits still name
 * the parents severed, and the horizon records the fragment, deleted.  The
 * next collection frees what only the fragment reached; repair can bring
 * it back from any remote or clone that has it.  The database owner's.
 */
Datum
dolt_history_drop(PG_FUNCTION_ARGS)
{
	const ProllyWindowRoutine *w = database_window();
	TimestampTz before = PG_GETARG_TIMESTAMPTZ(0);
	ProllyWindowResult r;
	TupleDesc	tupdesc;
	Datum		values[5];
	bool		nulls[5] = {false, false, false, false, false};
	HeapTuple	tuple;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	PROLLY_CALL(w, sever, PROLLY_WINDOW_DELETE, &before, &r);
	values[2] = hashes_datum(r.boundary, r.nboundary);
	values[0] = Int64GetDatum(r.fragment);
	nulls[0] = r.fragment == 0;
	values[1] = Int64GetDatum(r.commits);
	values[3] = Int64GetDatum(r.chunks);
	values[4] = Int64GetDatum(r.bytes);
	tuple = heap_form_tuple(BlessTupleDesc(tupdesc), values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

static void
horizon_row(const ProllyWindowFragment *f, void *arg)
{
	ReturnSetInfo *rsinfo = arg;
	Datum		values[12];
	bool		nulls[12] = {0};

	values[0] = Int64GetDatum(f->number);
	values[1] = CStringGetTextDatum(f->deleted ? "deleted" : "archived");
	values[2] = TimestampTzGetDatum(f->severed_at);
	values[3] = TimestampTzGetDatum(f->oldest);
	values[4] = TimestampTzGetDatum(f->newest);
	values[5] = Int64GetDatum(f->commits);
	values[6] = Int64GetDatum(f->chunks);
	values[7] = Int64GetDatum(f->bytes);
	values[8] = PointerGetDatum(hash_to_text(&f->root));
	values[9] = PointerGetDatum(hash_to_text(&f->digest));
	if (f->location)
		values[10] = CStringGetTextDatum(f->location);
	else
		nulls[10] = true;
	values[11] = hashes_datum(f->tops, f->ntops);
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

/*
 * prolly_history_horizon()
 *	returns setof (fragment bigint, state text, severed_at timestamptz,
 *				   oldest timestamptz, newest timestamptz, commits bigint,
 *				   chunks bigint, bytes bigint, root text, digest text,
 *				   location text, tops text[])
 *
 * The history horizon's fragments, newest first.
 */
Datum
prolly_history_horizon(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	const ProllyWindowRoutine *w = database_window();

	InitMaterializedSRF(fcinfo, 0);
	PROLLY_CALL(w, fragments, horizon_row, rsinfo);
	return (Datum) 0;
}

static void
plan_row(ProllyWindowItemKind kind, const ProllyHash *commit,
		 const ProllyHash *other, const char *why, void *arg)
{
	ReturnSetInfo *rsinfo = arg;
	Datum		values[4];
	bool		nulls[4] = {0};
	static const char *const kinds[] = {"severed", "boundary", "pinned"};

	values[0] = CStringGetTextDatum(kinds[kind]);
	values[1] = PointerGetDatum(hash_to_text(commit));
	if (other)
		values[2] = PointerGetDatum(hash_to_text(other));
	else
		nulls[2] = true;
	if (why)
		values[3] = CStringGetTextDatum(why);
	else
		nulls[3] = true;
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

/*
 * prolly_history_plan(before timestamptz)
 *	returns setof (kind text, commit_hash text, parent_hash text, why text)
 *
 * What a sever before before (NULL: as the database's window has it)
 * would do, doing none of it: each commit it
 * would sever, each boundary commit with the parent it would no longer
 * keep, and each commit kept past the window and why (a ref's, a merge
 * base's).
 */
Datum
prolly_history_plan(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	const ProllyWindowRoutine *w = database_window();
	TimestampTz before = PG_ARGISNULL(0) ? 0 : PG_GETARG_TIMESTAMPTZ(0);

	InitMaterializedSRF(fcinfo, 0);
	PROLLY_CALL(w, plan, PG_ARGISNULL(0) ? NULL : &before, plan_row, rsinfo);
	return (Datum) 0;
}

/*
 * dolt_history_repair(source text, fragments int) returns int
 *
 * Reattach the history horizon's newest fragments (fragments NULL: all of
 * them), newest first, from source (a remote's name or URL; NULL: each
 * fragment's archive), each verified against its identity and digest
 * before the horizon lets go of it: the history reads as it did.  The
 * count repaired.  The database owner's.
 */
Datum
dolt_history_repair(PG_FUNCTION_ARGS)
{
	const ProllyWindowRoutine *w = database_window();
	char	   *source = PG_ARGISNULL(0) ? NULL :
		text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			n = PG_ARGISNULL(1) ? -1 : PG_GETARG_INT32(1);

	if (!PG_ARGISNULL(1) && n < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("fragments must not be negative")));
	PG_RETURN_INT32(PROLLY_CALL(w, repair, source, n));
}

static Datum
sever_row(FunctionCallInfo fcinfo, const ProllyWindowResult *r)
{
	TupleDesc	tupdesc;
	Datum		values[6];
	bool		nulls[6] = {false, false, false, false, false, false};
	static const char *const actions[] = {"archive", "delete", "none"};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	values[0] = CStringGetTextDatum(actions[r->action]);
	values[1] = Int64GetDatum(r->fragment);
	nulls[1] = r->fragment == 0;
	values[2] = Int64GetDatum(r->commits);
	values[3] = hashes_datum(r->boundary, r->nboundary);
	values[4] = Int64GetDatum(r->chunks);
	values[5] = Int64GetDatum(r->sent);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(tupdesc),
													  values, nulls)));
}

/*
 * dolt_history_sever()
 *	returns record (action text, fragment bigint, commits bigint,
 *					boundary text[], chunks bigint, sent bigint)
 *
 * Sever the history past the database's window, as its settings have it
 * (prolly.history_window_action, prolly.history_window and the rest, as the
 * database has them, never the session): archived to
 * prolly.history_archive and found whole there, deleted, or nothing.  The
 * database owner's.
 */
Datum
dolt_history_sever(PG_FUNCTION_ARGS)
{
	const ProllyWindowRoutine *w = database_window();
	ProllyWindowPolicy p;
	ProllyWindowResult r;

	PROLLY_CALL(w, policy, NULL, &p);
	PROLLY_CALL(w, sever, p.action, NULL, &r);
	return sever_row(fcinfo, &r);
}

/*
 * dolt_history_window(branch text, setting text, value text) returns void
 *
 * Set branch's lineage's own window setting (window, min_commits or
 * batch; value NULL: the database's again).  The database owner's.
 */
Datum
dolt_history_window(PG_FUNCTION_ARGS)
{
	const ProllyWindowRoutine *w = database_window();

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("a branch and a setting are needed")));
	PROLLY_CALL(w, set_policy, text_to_cstring(PG_GETARG_TEXT_PP(0)),
				text_to_cstring(PG_GETARG_TEXT_PP(1)),
				PG_ARGISNULL(2) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(2)));
	PG_RETURN_VOID();
}

/*
 * prolly_history_in_window(branch text, commit_hash text) returns bool
 *
 * Whether the commit is in the history window now: in branch's lineage's
 * (NULL: any branch's), kept for its age or count as a sever would keep
 * it.  A commit kept only for a ref or a merge base isn't.
 */
Datum
prolly_history_in_window(PG_FUNCTION_ARGS)
{
	const ProllyWindowRoutine *w = database_window();
	char	   *hex;
	ProllyHash	commit;

	if (PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("a commit is needed")));
	hex = text_to_cstring(PG_GETARG_TEXT_PP(1));
	if (!prolly_hash_from_hex(hex, strlen(hex), &commit))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" isn't a commit hash", hex)));
	PG_RETURN_BOOL(PROLLY_CALL(w, in_window,
							   PG_ARGISNULL(0) ? NULL :
							   text_to_cstring(PG_GETARG_TEXT_PP(0)),
							   &commit));
}

/*
 * prolly_history_policy(branch text)
 *	returns record (action text, time_window interval, min_commits bigint,
 *					batch interval, max_size bigint, archive text)
 *
 * The window branch's lineage keeps (NULL: the database's), as the
 * database's settings and the branch's own have it.
 */
Datum
prolly_history_policy(PG_FUNCTION_ARGS)
{
	const ProllyWindowRoutine *w = database_window();
	ProllyWindowPolicy p;
	TupleDesc	tupdesc;
	Datum		values[6];
	bool		nulls[6] = {false, false, false, false, false, false};
	static const char *const actions[] = {"archive", "delete", "none"};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	PROLLY_CALL(w, policy,
				PG_ARGISNULL(0) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(0)),
				&p);
	values[0] = CStringGetTextDatum(actions[p.action]);
	if (p.keep_time > 0)
	{
		Interval   *iv = palloc0(sizeof(Interval));

		iv->time = p.keep_time;
		values[1] = IntervalPGetDatum(iv);
	}
	else
		nulls[1] = true;
	values[2] = Int64GetDatum(p.keep_commits);
	if (p.batch > 0)
	{
		Interval   *iv = palloc0(sizeof(Interval));

		iv->time = p.batch;
		values[3] = IntervalPGetDatum(iv);
	}
	else
		nulls[3] = true;
	values[4] = Int64GetDatum(p.max_bytes);
	nulls[4] = p.max_bytes == 0;
	if (p.archive)
		values[5] = CStringGetTextDatum(p.archive);
	else
		nulls[5] = true;
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(tupdesc),
													  values, nulls)));
}
