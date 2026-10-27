/*-------------------------------------------------------------------------
 *
 * prolly_dolt.c
 *	  Doltgres's SQL interface to version control: the dolt_* functions
 *	  and the functions behind the dolt_log, dolt_branches, dolt_tags,
 *	  dolt_status and dolt_conflicts system views, the dolt.author and
 *	  dolt.allow_commit_conflicts settings, and "dbname/branch"
 *	  connections.
 *
 * The functions take Dolt's command-line arguments as text and return
 * text arrays, as Doltgres's do (dolt_commit('-m', 'message') returns the
 * new commit's id), so scripts written for Doltgres run here.  Each is an
 * adapter onto the refs and diff/merge interfaces (prolly_config.h): what
 * those can't do yet is refused, never approximated.
 *
 * There is no separate staging area: a commit records the branch's whole
 * working set, so every change is staged as it is made.  dolt_add has
 * nothing to do, dolt_commit accepts -a and -A and needs neither,
 * dolt_status shows every change as staged, and dolt_reset only does hard
 * resets.  WORKING
 * means the session branch's working set as the current transaction sees
 * it: dolt_status and dolt_diff see its own uncommitted writes, as a Dolt
 * session's working root has its own.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_dolt.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/prolly_autovacuum.h"
#include "access/prolly_chunkstore.h"
#include "access/prolly_function_scan.h"
#include "access/prolly_history.h"
#include "access/prolly_log.h"
#include "access/prolly_log_count.h"
#include "access/prolly_revision.h"
#include "access/prolly_snapshot.h"
#include "access/prolly_toast.h"
#include "access/prolly_canon.h"
#include "access/prolly_config.h"
#include "access/prolly_deps.h"
#include "access/prolly_window.h"
#include "access/prolly_xact.h"
#include "access/table.h"
#include "access/xact.h"
#include "access/tableam.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_proc.h"
#include "catalog/branch.h"
#include "catalog/branch_ops.h"
#include "catalog/namespace.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_authid_d.h"
#include "catalog/dependency.h"
#include "catalog/pg_branch.h"
#include "catalog/pg_database.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_type.h"
#include "catalog/versioned.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "lib/pairingheap.h"
#include "miscadmin.h"
#include "nodes/miscnodes.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "nodes/supportnodes.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "parser/parse_type.h"
#include "pgstat.h"
#include "storage/bmap.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/json.h"
#include "utils/jsonb.h"
#include "utils/regproc.h"
#include "utils/resowner.h"
#include "nodes/makefuncs.h"
#include "utils/datum.h"
#include "utils/varlena.h"
#include "utils/fmgrprotos.h"
#include "utils/guc.h"
#include "utils/injection_point.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

#include "refs_generations.h"

/* dolt.author's value; read through GetConfigOption so a placeholder works. */
static char *dolt_author = NULL;
static bool dolt_allow_commit_conflicts = false;

/*
 * dolt.transaction_commit: every SQL transaction that changes a branch makes
 * a Dolt commit of it too, as it commits, with dolt.transaction_commit_message
 * (as it is then).  Whether to is read at the transaction's first write.
 */
static bool dolt_transaction_commit = false;
static char *dolt_transaction_commit_message = NULL;
static LocalTransactionId txn_commit_lxid = InvalidLocalTransactionId;
static bool txn_commit_on = false;

/*
 * prolly.fold_on_commit: each transaction's own row changes folded into
 * its branch's baseline as it commits, as the transaction's first write
 * found the setting (txn_fold_on), dolt.transaction_commit's mode aside.
 */
static bool prolly_fold_on_commit = false;
static bool txn_fold_on = false;

/*
 * Its own row changes, as it makes them (the TIDs of the versions each
 * write made or replaced), by table; up to dolt.transaction_commit_max_changes,
 * past which it commits its working set instead.
 */
static int	dolt_transaction_commit_max_changes = 100000;

typedef struct OwnChanges
{
	Oid			relid;			/* hash key */
	ItemPointerData *tids;
	int			n;
	int			max;
} OwnChanges;

static HTAB *own_changes = NULL;	/* in TopTransactionContext */
static LocalTransactionId own_changes_lxid = InvalidLocalTransactionId;
static int	own_changes_n = 0;
static bool own_changes_over = false;

static void forget_own_changes(void);

/*
 * The last settle, by its subtransaction: one undone with it (a rollback to
 * a savepoint) undoes the commit or reset too, and the changes it had made
 * the transaction forget are the branch's again; the transaction then
 * commits its working set.
 */
static LocalTransactionId settled_lxid = InvalidLocalTransactionId;
static SubTransactionId settled_subid = InvalidSubTransactionId;
static bool settle_undone = false;

/*
 * a transaction found not to move from a branch: needn't look again there
 * while the writers' generation is the same (no mode or protection set, or
 * undone)
 */
static LocalTransactionId stays_lxid = InvalidLocalTransactionId;
static Oid	stays_branch = InvalidOid;
static uint64 stays_gen = 0;

static void transaction_commit_at_end(bool prepare);
static void note_own_change(Relation rel, const ItemPointerData *oldtid,
							const ItemPointerData *newtid);

/* (a commit's message isn't empty, as dolt_commit's isn't) */
static bool
check_commit_message(char **newval, void **extra, GucSource source)
{
	if (**newval == '\0')
	{
		GUC_check_errdetail("A commit needs a message.");
		return false;
	}
	return true;
}

/*
 * An author has a name, and an email when it has <...>, as Dolt's commits
 * need both (one without <...> is a name alone, as the session user is).
 * Its parts are as split_author finds them.
 */
static bool
author_ok(const char *author)
{
	const char *lt = strrchr(author, '<');
	const char *gt = lt ? strchr(lt, '>') : NULL;

	if (lt && gt)
	{
		const char *end = lt;

		while (end > author && end[-1] == ' ')
			end--;
		return end > author && gt > lt + 1;
	}
	return author[0] != '\0';
}

/* (empty, dolt.author is the session user's name) */
static bool
check_author(char **newval, void **extra, GucSource source)
{
	if (**newval != '\0' && !author_ok(*newval))
	{
		GUC_check_errdetail("An author is \"Name <email>\", or a name alone.");
		return false;
	}
	return true;
}

/* dolt_commit's --author, while its commit runs. */
static const char *author_override = NULL;

static void writers_note_write(Oid branch, void *arg);
static void writer_branch_hooks(void);
static bool names_tag(const char *name);
static void dolt_tag_change(char **names, int nnames, bool delete,
							const char *message, const char *author);

/* a branch to check out once the backend processes normally */
static char *connect_branch = NULL;

void
prolly_session_init(const char *branch)
{
	static bool defined = false;

	if (!defined)
	{
		DefineCustomStringVariable("dolt.author",
								   "Who prolly commits are by, as \"Name <email>\".",
								   "Empty means the session user.",
								   &dolt_author,
								   "",
								   PGC_USERSET,
								   0,
								   check_author, NULL, NULL);
		prolly_autovacuum_init();
		DefineCustomBoolVariable("dolt.transaction_commit",
								 "Makes a Dolt commit of every transaction that changes a branch, as it commits.",
								 "Read at the transaction's first write; the message, at its commit.",
								 &dolt_transaction_commit,
								 false,
								 PGC_USERSET,
								 0,
								 NULL, NULL, NULL);
		DefineCustomStringVariable("dolt.transaction_commit_message",
								   "The message of the Dolt commits dolt.transaction_commit makes.",
								   NULL,
								   &dolt_transaction_commit_message,
								   "Transaction commit",
								   PGC_USERSET,
								   0,
								   check_commit_message, NULL, NULL);
		DefineCustomIntVariable("dolt.transaction_commit_max_changes",
								"The most row versions a transaction's commit records by themselves (an update is two).",
								"Past them, dolt.transaction_commit commits the branch's working set.",
								&dolt_transaction_commit_max_changes,
								100000, 0, INT_MAX,
								PGC_USERSET,
								0,
								NULL, NULL, NULL);
		branch_tuple_hook = note_own_change;
		branch_storage_hook = branch_replaced_storage;
		prolly_refs_end_hook = transaction_commit_at_end;
		prolly_writers_init();
		RegisterBranchWriteCallback(writers_note_write, NULL);
		writer_branch_hooks();
		DefineCustomBoolVariable("prolly.fold_on_commit",
								 "Folds each transaction's own row changes into its branch's baseline as it commits.",
								 "Status, diffs and commits then fold only what other transactions left unfolded; each transaction that changes a versioned table pays its tree's writes as it commits.",
								 &prolly_fold_on_commit,
								 false,
								 PGC_USERSET,
								 0,
								 NULL, NULL, NULL);
		DefineCustomBoolVariable("dolt.allow_commit_conflicts",
								 "Lets a transaction commit with unresolved merge conflicts.",
								 "They stay to be resolved later; until then the branch can't commit.",
								 &dolt_allow_commit_conflicts,
								 false,
								 PGC_USERSET,
								 0,
								 NULL, NULL, NULL);
		defined = true;
	}
	if (branch)
		connect_branch = MemoryContextStrdup(TopMemoryContext, branch);
}

/*
 * The branch a connection asked for, checked out in a transaction of its
 * own now that the backend processes normally.  (An error here, before the
 * main loop, ends the connection: it isn't left on another branch.)  The
 * transaction reads nothing else, so it is read committed whatever the
 * default: a branch's first use moves its copies by a snapshot taken after
 * they were made, never one taken before.
 */
void
prolly_session_ready(void)
{
	ProllyDatabaseComponents dc;

	if (connect_branch == NULL)
		return;
	StartTransactionCommand();
	XactIsoLevel = XACT_READ_COMMITTED;
	PushActiveSnapshot(GetTransactionSnapshot());
	prolly_database_components(&dc);
	BRANCH_WRITES_HUSHED(PROLLY_CALL(dc.refs, checkout, connect_branch));
	PopActiveSnapshot();
	CommitTransactionCommand();
	pfree(connect_branch);
	connect_branch = NULL;
}

/* dolt_commit's -f, while its commit runs: violations don't stop it */
static bool commit_forced = false;

bool
prolly_allow_commit_conflicts(void)
{
	const char *v = GetConfigOption("dolt.allow_commit_conflicts", true, false);
	bool		result = false;

	return commit_forced || (v != NULL && parse_bool(v, &result) && result);
}

const char *
prolly_commit_author(void)
{
	const char *author;

	if (author_override)
		return author_override;
	author = GetConfigOption("dolt.author", true, false);
	if (author && author[0] != '\0')
		return author;
	return GetUserNameFromId(GetUserId(), false);
}

void
prolly_set_commit_author(const char *author)
{
	author_override = author;
}

/* the date a dolt_commit or dolt_merge was given (--date), while it runs */
static bool date_given = false;
static TimestampTz date_override = 0;

bool
prolly_commit_date(TimestampTz *at)
{
	if (!date_given)
		return false;
	*at = date_override;
	return true;
}

/* --date's value: a timestamptz, as Postgres reads one */
static TimestampTz
given_date(const char *fn, const char *value)
{
	TimestampTz at = DatumGetTimestampTz(DirectFunctionCall3(timestamptz_in,
															 CStringGetDatum(value),
															 ObjectIdGetDatum(InvalidOid),
															 Int32GetDatum(-1)));

	if (TIMESTAMP_NOT_FINITE(at))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s --date must be a finite timestamp", fn)));
	return at;
}

/* ------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------
 */

static char *
hash_hex(const ProllyHash *hash)
{
	return prolly_hash_hex(hash);
}

static ProllyHash *
copy_hash(const ProllyHash *h)
{
	ProllyHash *c = palloc(sizeof(ProllyHash));

	*c = *h;
	return c;
}

static bool
same_hash(const ProllyHash *a, const ProllyHash *b)
{
	return a->len == b->len && memcmp(a->bytes, b->bytes, a->len) == 0;
}

/* The VARIADIC text[] arguments, as C strings. */
static int
text_args(FunctionCallInfo fcinfo, char ***out)
{
	ArrayType  *arr;
	Datum	   *elems;
	bool	   *nulls;
	int			n;

	*out = NULL;
	if (PG_NARGS() == 0 || PG_ARGISNULL(0))
		return 0;
	arr = PG_GETARG_ARRAYTYPE_P(0);
	deconstruct_array_builtin(arr, TEXTOID, &elems, &nulls, &n);
	*out = palloc(sizeof(char *) * (n + 1));
	for (int i = 0; i < n; i++)
	{
		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("arguments must not be null")));
		(*out)[i] = TextDatumGetCString(elems[i]);
	}
	(*out)[n] = NULL;
	return n;
}

static Datum
text_array(int n, const char *const *vals)
{
	Datum	   *d = palloc(sizeof(Datum) * n);

	for (int i = 0; i < n; i++)
		d[i] = CStringGetTextDatum(vals[i]);
	return PointerGetDatum(construct_array_builtin(d, n, TEXTOID));
}

/*
 * The argument after option args[*i], whatever it is, "--" too (as Dolt's
 * argument parser and git's take it): an option's value is never read as
 * the "--" that ends the options.
 */
static char *
option_value(char **args, int nargs, int *i, const char *fn)
{
	if (*i + 1 >= nargs)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s: option \"%s\" needs a value", fn, args[*i])));
	return args[++(*i)];
}

/*
 * Is arg the option name, a long one or a short one, given with two
 * dashes or one ("--no-ff", "-no-ff"; "-m", "--m"), as Dolt's argument
 * parser takes either for either?  (Short flags together, "-am", are
 * each function's own to take apart.)
 */
static bool
is_option(const char *arg, const char *name)
{
	if (arg[0] != '-')
		return false;
	arg += arg[1] == '-' ? 2 : 1;
	return strcmp(arg, name) == 0;
}

/*
 * The short flags arg holds: one or several after one dash ("-am"), or
 * one after two ("--a", as Dolt's parser takes any name so); NULL if it
 * isn't such.
 */
static const char *
short_flags(const char *arg)
{
	if (arg[0] != '-' || arg[1] == '\0')
		return NULL;
	if (arg[1] != '-')
		return arg + 1;
	return arg[2] != '\0' && arg[2] != '-' && arg[3] == '\0' ? arg + 2 : NULL;
}

static void
unsupported(const char *fn, const char *arg)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("%s: \"%s\" is not supported", fn, arg)));
}

static void
unexpected(const char *fn, const char *arg)
{
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("%s: unexpected argument \"%s\"", fn, arg)));
}

/* Flags a and b (named an and bn) given together are an error. */
static void
exclusive_flags(int flags, int a, const char *an, int b, const char *bn)
{
	if ((flags & a) && (flags & b))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("flags \"%s\" and \"%s\" cannot be used together", an, bn)));
}

static const ProllyRefsRoutine *
refs(void)
{
	ProllyDatabaseComponents dc;

	prolly_database_components(&dc);
	return dc.refs;
}

static const ProllyMergeRoutine *
diff_merge(void)
{
	ProllyDatabaseComponents dc;

	prolly_database_components(&dc);
	return dc.diff_merge;
}

static const ProllyCanonRoutine *
canonicalizer(void)
{
	ProllyDatabaseComponents dc;

	prolly_database_components(&dc);
	return dc.canonicalizer;
}

static const ProllyWritersRoutine *
writers(void)
{
	ProllyDatabaseComponents dc;

	prolly_database_components(&dc);
	return dc.writers;
}

static const ProllyMatCacheRoutine *
matcache(void)
{
	ProllyDatabaseComponents dc;

	prolly_database_components(&dc);
	return dc.matcache;
}

/* while set, the session's branch is written by an operation that commits */
static bool integrating = false;

/*
 * A write to a branch is first its writers' to allow.  The transaction's
 * first takes dolt.transaction_commit as it is now, and opens the refs if
 * the transaction is to commit through them as it ends: their transaction
 * callback must be registered before COMMIT begins (one registered while
 * the callbacks run isn't called), and, callbacks running newest first,
 * the refs', registered after the chunk store's (which they open first),
 * runs before the chunk store's flush.
 */
static void
writers_note_write(Oid branch, void *arg)
{
	PROLLY_CALL(writers(), note_write, branch, integrating);
	if (MyProc->vxid.lxid != txn_commit_lxid)
	{
		txn_commit_lxid = MyProc->vxid.lxid;
		txn_commit_on = dolt_transaction_commit;
		txn_fold_on = !txn_commit_on && prolly_fold_on_commit;
		if (txn_commit_on || txn_fold_on)
			PROLLY_CALL(refs(), open);
	}
}

/*
 * The session's branch is about to be changed by an integration: an
 * operation that makes its commit in the same transaction (a protected
 * branch allows only these).
 */
static void
note_integration(void)
{
	integrating = true;
	PG_TRY();
	{
		branch_note_write(MyBranch);
	}
	PG_FINALLY();
	{
		integrating = false;
	}
	PG_END_TRY();
}

/*
 * An integration that stopped with conflicts or violations leaves a
 * protected branch with changes no commit holds: refused.
 */
static void
must_commit(int left, const char *what)
{
	const char *name = branch_ops()->current();

	if (left > 0 && PROLLY_CALL(writers(), protected, name))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("%s into protected branch \"%s\" would leave it uncommitted", what, name),
				 errdetail("It has conflicts or constraint violations to resolve."),
				 errhint("Merge \"%s\" into a branch of your own and resolve them there, then integrate that.",
						 name)));
}

/*
 * The session's branch may be clean again: committed, reset or merged.  Its
 * changes so far are a commit's (or gone): a later write is noted, and
 * allowed or not, again, and only it is the transaction's to commit at its
 * end.
 */
static void
settled(void)
{
	PROLLY_CALL(writers(), settled, MyBranch);
	branch_forget_written(MyBranch);
	forget_own_changes();
	settled_lxid = MyProc->vxid.lxid;
	settled_subid = GetCurrentSubTransactionId();
	settle_undone = false;		/* (one undone before is in this one) */
}

/* (a transaction's own changes: none yet, as it begins) */
static void
forget_own_changes(void)
{
	own_changes = NULL;
	own_changes_lxid = InvalidLocalTransactionId;
	own_changes_n = 0;
	own_changes_over = false;
}

static void
add_own_change(Relation rel, const ItemPointerData *tid)
{
	OwnChanges *oc;
	bool		found;
	Oid			relid = RelationGetRelid(rel);

	oc = hash_search(own_changes, &relid, HASH_ENTER, &found);
	if (!found)
	{
		oc->tids = NULL;
		oc->n = oc->max = 0;
	}
	if (oc->n == oc->max)
	{
		oc->max = Max(64, oc->max * 2);
		oc->tids = oc->tids ?
			repalloc_array(oc->tids, ItemPointerData, oc->max) :
			MemoryContextAlloc(TopTransactionContext,
							   sizeof(ItemPointerData) * oc->max);
	}
	oc->tids[oc->n++] = *tid;
	own_changes_n++;
}

/* A write's tuple versions, kept if the transaction is to commit by them. */
static void
note_own_change(Relation rel, const ItemPointerData *oldtid,
				const ItemPointerData *newtid)
{
	if (txn_commit_lxid != MyProc->vxid.lxid ||
		!(txn_commit_on || txn_fold_on) ||
		(own_changes_lxid == MyProc->vxid.lxid && own_changes_over))
		return;
	if (own_changes_lxid != MyProc->vxid.lxid)
	{
		HASHCTL		ctl = {0};

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(OwnChanges);
		ctl.hcxt = TopTransactionContext;
		own_changes = hash_create("own row changes", 16, &ctl,
								  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		own_changes_lxid = MyProc->vxid.lxid;
		own_changes_n = 0;
		own_changes_over = false;
	}
	/* (an update is two: the version it replaced, and the one it made) */
	if (own_changes_n + (oldtid != NULL) + (newtid != NULL) >
		dolt_transaction_commit_max_changes)
	{
		own_changes_over = true;
		return;
	}
	if (oldtid)
		add_own_change(rel, oldtid);
	if (newtid)
		add_own_change(rel, newtid);
}

/*
 * As a transaction that changed its branch commits in prolly.fold_on_commit's
 * mode: its own row changes folded into the branch's baseline, so the next
 * fold of the branch (a status, a diff, a commit) reads only what others
 * left unfolded.  Nothing is folded, the changes left to the next fold as
 * without the mode, where the transaction is prepared, changed definitions
 * or more rows than it keeps, changed another branch than the session's,
 * or a merge is in progress, or another transaction holds the refs (a
 * commit never waits for them for this).
 */
static void
fold_on_commit_at_end(bool prepare)
{
	List	   *branches;
	ProllyMergeState ms;
	ProllyRowChanges *changes;
	int			nchanges = 0;
	HASH_SEQ_STATUS hs;
	OwnChanges *oc;
	bool		pushed;

	if (prepare || own_changes_lxid != MyProc->vxid.lxid ||
		own_changes == NULL || own_changes_over || branch_catalog_written() ||
		(settled_lxid == MyProc->vxid.lxid && settle_undone))
		return;
	branches = branches_written();
	if (list_length(branches) != 1 || linitial_oid(branches) != MyBranch ||
		PROLLY_CALL(refs(), merge_state, &ms) ||
		!PROLLY_HAS(refs(), fold_own))
		return;
	changes = palloc_array(ProllyRowChanges,
						   Max(hash_get_num_entries(own_changes), 1));
	hash_seq_init(&hs, own_changes);
	while ((oc = hash_seq_search(&hs)) != NULL)
	{
		changes[nchanges].relid = oc->relid;
		changes[nchanges].tids = oc->tids;
		changes[nchanges].ntids = oc->n;
		nchanges++;
	}
	pushed = !ActiveSnapshotSet();
	if (pushed)
		PushActiveSnapshot(GetTransactionSnapshot());
	BRANCH_WRITES_HUSHED((void) refs()->fold_own(changes, nchanges));
	if (pushed)
		PopActiveSnapshot();
}

/*
 * As a transaction that changed a branch commits in dolt.transaction_commit's
 * mode: a Dolt commit of the branch, unless nothing changed (an insert
 * deleted again, a commit already made of it all).  Changes to two branches,
 * or to one the session has left, can't be committed so, and a transaction
 * that would commit can't be prepared.  With conflicts kept (allowed by
 * dolt.allow_commit_conflicts) the transaction commits without one, as the
 * conflicts are to be resolved before the branch can commit.
 */
static void
transaction_commit_at_end(bool prepare)
{
	List	   *branches;
	bool		undone;
	ProllyMergeState ms;
	ProllyHash	id;
	bool		pushed;

	if (txn_commit_lxid == MyProc->vxid.lxid && txn_fold_on)
	{
		fold_on_commit_at_end(prepare);
		return;
	}
	if (txn_commit_lxid != MyProc->vxid.lxid || !txn_commit_on)
		return;
	undone = settled_lxid == MyProc->vxid.lxid && settle_undone;
	settle_undone = false;
	branches = branches_written();
	if (branches == NIL && undone)
		branches = list_make1_oid(MyBranch);
	if (branches == NIL)
		return;
	if (prepare)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot prepare a transaction that dolt.transaction_commit would commit")));
	if (list_length(branches) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("dolt.transaction_commit can't commit a transaction that changed more than one branch"),
				 errhint("Change each branch in a transaction of its own.")));
	if (linitial_oid(branches) != MyBranch)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("dolt.transaction_commit can't commit changes to a branch the session has left"),
				 errhint("Check the changed branch out again before committing.")));
	/* (a Dolt commit moves the branch's head, as dolt_commit's does) */
	prolly_check_head_move(branch_ops()->current());
	if (PROLLY_CALL(refs(), merge_state, &ms) && ms.ntables > 0)
	{
		ereport(WARNING,
				(errmsg("transaction committed without a Dolt commit: it has unresolved merge conflicts"),
				 errhint("Resolve them with dolt_conflicts_resolve(), then commit.")));
		return;
	}
	pushed = !ActiveSnapshotSet();
	if (pushed)
		PushActiveSnapshot(GetTransactionSnapshot());

	/*
	 * Its own row changes, on the head as it is: unless it changed
	 * definitions, or more rows than it keeps, or they can't be applied so
	 * (then the working set, as its snapshot sees it).
	 */
	if (!undone && !branch_catalog_written() &&
		!(own_changes_lxid == MyProc->vxid.lxid && own_changes_over))
	{
		ProllyRowChanges *changes = NULL;
		int			nchanges = 0;
		List	   *seqs = branch_sequences_written();
		Oid		   *seqids = palloc_array(Oid, Max(list_length(seqs), 1));
		int			nseqs = 0;
		ListCell   *lc;
		int			r;

		if (own_changes_lxid == MyProc->vxid.lxid && own_changes)
		{
			HASH_SEQ_STATUS hs;
			OwnChanges *oc;

			changes = palloc_array(ProllyRowChanges,
								   Max(hash_get_num_entries(own_changes), 1));
			hash_seq_init(&hs, own_changes);
			while ((oc = hash_seq_search(&hs)) != NULL)
			{
				changes[nchanges].relid = oc->relid;
				changes[nchanges].tids = oc->tids;
				changes[nchanges].ntids = oc->n;
				nchanges++;
			}
		}
		foreach(lc, seqs)
			seqids[nseqs++] = lfirst_oid(lc);
		BRANCH_WRITES_HUSHED(r = PROLLY_CALL(refs(), commit_rows,
											 dolt_transaction_commit_message,
											 changes, nchanges, seqids, nseqs,
											 &id));
		if (r >= 0)
		{
			/* (committed or not, the branch may be clean: its owner given up) */
			settled();
			if (pushed)
				PopActiveSnapshot();
			return;
		}
	}
	BRANCH_WRITES_HUSHED((void) PROLLY_CALL(refs(), commit,
											dolt_transaction_commit_message,
											false, &id));
	/* (committed or not, the branch may be clean: its owner given up) */
	settled();
	if (pushed)
		PopActiveSnapshot();
}

/* Splits "Name <email>" into its parts; without <...>, all of it is a name. */
static void
split_author(const char *author, char **name, char **email)
{
	const char *lt = strrchr(author, '<');
	const char *gt = lt ? strchr(lt, '>') : NULL;

	if (lt && gt)
	{
		const char *end = lt;

		while (end > author && end[-1] == ' ')
			end--;
		*name = pnstrdup(author, end - author);
		*email = pnstrdup(lt + 1, gt - lt - 1);
	}
	else
	{
		*name = pstrdup(author);
		*email = pstrdup("");
	}
}

/* An --author given to fn, refused unless it is an author. */
static void
given_author(const char *fn, const char *author)
{
	if (author != NULL && !author_ok(author))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s: \"%s\" is not an author", fn, author),
				 errhint("An author is \"Name <email>\", or a name alone.")));
}

/* A -m given to fn, refused when empty: a commit needs a message. */
static void
given_message(const char *fn, const char *message)
{
	if (message != NULL && message[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s needs a message", fn),
				 errdetail("A commit's message can't be empty.")));
}

/* NULL for one dropped (as a table whose violations are kept may be) */
static char *
qualified_name(Oid relid)
{
	char	   *name = get_rel_name(relid);

	if (name == NULL)
		return NULL;
	return quote_qualified_identifier(get_namespace_name(get_rel_namespace(relid)),
									  name);
}

/* ------------------------------------------------------------------------
 * Procedures
 * ------------------------------------------------------------------------
 */

/* a rebase's own branch: dolt_rebase_<the branch rebased> */
#define REBASE_BRANCH_PREFIX "dolt_rebase_"


/*
 * Does the session's role write the version control's shared state at
 * large: own the database, have its CREATE privilege (which makes
 * branches), or have the privileges of pg_prolly_write?
 */
bool
prolly_writes_refs(void)
{
	Oid			user = GetUserId();

	return object_ownercheck(DatabaseRelationId, MyDatabaseId, user) ||
		has_privs_of_role(user, ROLE_PG_PROLLY_WRITE) ||
		object_aclcheck(DatabaseRelationId, MyDatabaseId, user,
						ACL_CREATE) == ACLCHECK_OK;
}

/* the database's owner */
static Oid
database_owner_role(void)
{
	HeapTuple	tup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
	Oid			owner;

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for database %u", MyDatabaseId);
	owner = ((Form_pg_database) GETSTRUCT(tup))->datdba;
	ReleaseSysCache(tup);
	return owner;
}

/*
 * Does role exist?  A writer branch's role is recorded by OID alone: a role
 * dropped leaves it where it was recorded.  (It grants nothing alone: a
 * writer's integration also moves its writer branch's head, which takes
 * the branch's owner, its maker, a shared dependency of the role.)
 */
static bool
role_exists(Oid role)
{
	return OidIsValid(role) &&
		SearchSysCacheExists1(AUTHOID, ObjectIdGetDatum(role));
}

/*
 * The role that owns branch name: the role that made it, or, for main or a
 * branch whose maker isn't recorded, the database's owner.  A maker is a
 * shared dependency of its role, so it is never a dropped role's.
 */
Oid
prolly_branch_owner(const char *name)
{
	Oid			creator;

	if (strcmp(name, "main") != 0 && PROLLY_HAS(refs(), branch_creator) &&
		PROLLY_CALL(refs(), branch_creator, name, &creator))
		return creator;
	return database_owner_role();
}

/*
 * The branch, tag or stash of kind classId whose maker record has id, by
 * its name (a stash's list's) and a stash's place in its list; false if
 * the refs have none.  To change the object found (lock), the refs are
 * held first, until the transaction ends: a name or a place found without
 * them can be another's by the time the change holds them (a stash pushed
 * onto its list, the branch renamed).
 */
static bool
made_object(Oid classId, Oid id, StringInfo name, int *n, bool lock)
{
	const ProllyRefsRoutine *r = refs();

	if (!PROLLY_HAS(r, made_object))
		return false;
	if (lock)
		PROLLY_CALL(r, lock);
	return PROLLY_CALL(r, made_object, classId, id, name, n);
}

bool
prolly_made_object_identity(Oid classId, Oid id, char **name, int *n)
{
	StringInfoData buf;

	initStringInfo(&buf);
	*n = 0;
	if (!made_object(classId, id, &buf, n, false))
	{
		pfree(buf.data);
		return false;
	}
	*name = buf.data;
	return true;
}

void
prolly_describe_made_object(StringInfo buf, Oid classId, Oid id)
{
	StringInfoData name;
	int			n = 0;

	initStringInfo(&name);
	if (!made_object(classId, id, &name, &n, false))
		appendStringInfo(buf, _("%s %u, no longer in the refs"),
						 classId == RefsBranchClassId ? "branch" :
						 classId == RefsTagClassId ? "tag" : "stash", id);
	else if (classId == RefsBranchClassId)
		appendStringInfo(buf, _("branch \"%s\""), name.data);
	else if (classId == RefsTagClassId)
		appendStringInfo(buf, _("tag \"%s\""), name.data);
	else
		appendStringInfo(buf, _("stash stash@{%d} of \"%s\""), n, name.data);
	pfree(name.data);
}

/*
 * REASSIGN OWNED: newrole is the object's maker, its dependency moved with
 * the record.
 */
void
prolly_reassign_made_object(Oid classId, Oid id, Oid newrole)
{
	const ProllyRefsRoutine *r = refs();
	StringInfoData name;
	int			n = 0;

	initStringInfo(&name);
	if (!made_object(classId, id, &name, &n, true))
		changeDependencyOnOwner(classId, id, newrole);
	else if (classId == RefsBranchClassId)
		PROLLY_CALL(r, branch_set_creator, name.data, newrole);
	else if (classId == RefsTagClassId)
		PROLLY_CALL(r, tag_set_creator, name.data, newrole);
	else
		PROLLY_CALL(r, stash_set_maker, name.data, n, newrole);
	pfree(name.data);
}

/*
 * DROP OWNED: a tag or stash the role made is deleted, as an object it owns
 * is.  A branch is given to the database's owner (its maker no longer
 * recorded, as main's isn't) rather than deleted: deleting it would lose
 * its commits that no other branch has, and its working set, which every
 * session on it shares, for a command about one role's objects.
 */
void
prolly_drop_made_object(Oid classId, Oid id)
{
	const ProllyRefsRoutine *r = refs();
	StringInfoData name;
	int			n = 0;

	initStringInfo(&name);
	if (!made_object(classId, id, &name, &n, true))
		deleteSharedDependencyRecordsFor(classId, id, 0);
	else if (classId == RefsBranchClassId)
		PROLLY_CALL(r, branch_set_creator, name.data, InvalidOid);
	else if (classId == RefsTagClassId)
		(void) PROLLY_CALL(r, tag_set, name.data, NULL, NULL);
	else
		(void) PROLLY_CALL(r, stash_drop, name.data, n);
	pfree(name.data);
}

/*
 * Refuse the session's role a move of branch name's head (a commit, reset,
 * merge, cherry-pick, revert or rebase onto it, its deletion or rename)
 * unless it has the privileges of the branch's owner (prolly_branch_owner())
 * or writes the shared state at large (prolly_writes_refs()).  A branch's
 * head is what every session on it reads and commits on, so moving it is a
 * write to the branch: privileges on its tables alone don't give it.
 */
bool
prolly_may_move_head(const char *name)
{
	return prolly_writes_refs() ||
		has_privs_of_role(GetUserId(), prolly_branch_owner(name));
}

/*
 * How many moves of the branch (merges, cherry-picks, reverts, stash
 * applies) this backend is running.  Nothing else that moves a head or
 * the working set (a reset, a checkout, a commit, another move) runs
 * inside one, as from a function the rows it writes call: it would change
 * the working set, merge state and privileges under it.
 */
static int	moves_running = 0;

void
prolly_check_not_moving(void)
{
	if (moves_running > 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a branch can't be moved inside a merge, cherry-pick, revert or stash apply"),
				 errdetail("It was called while one was running, as from a function the rows it wrote called.")));
}

void
prolly_move_begin(void)
{
	prolly_check_not_moving();
	moves_running++;
}

void
prolly_move_end(void)
{
	Assert(moves_running > 0);
	moves_running--;
}

void
prolly_check_head_move(const char *name)
{
	prolly_check_not_moving();
	if (!prolly_may_move_head(name))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to move the head of branch \"%s\"", name),
				 errdetail("Only the branch's owner, the database's owner, and roles with the database's CREATE privilege or the privileges of the \"pg_prolly_write\" role may.")));
}

/* Refuse the session's role a change to the shared refs: see prolly_writes_refs(). */
static void
check_writes_refs(const char *what)
{
	if (!prolly_writes_refs())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to %s", what),
				 errdetail("Only the database's owner, and roles with the database's CREATE privilege or the privileges of the \"pg_prolly_write\" role, may.")));
}

/*
 * Refuse a rename of a branch to newname that the session's role may not
 * make: a branch's name follows its rules, and taking a new one takes what
 * making a branch of it takes, or pg_prolly_write's privileges (a rename
 * would otherwise make a branch of any name from one made on the role's
 * behalf).
 */
void
prolly_check_branch_rename(const char *newname)
{
	if (!branch_ops()->name_ok(newname))
		branch_ops()->check_create(newname);	/* (which says how it breaks) */
	if (!prolly_writes_refs())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to rename a branch to \"%s\"", newname),
				 errdetail("A branch's new name takes the database's CREATE privilege, as making a branch does, or the privileges of the \"pg_prolly_write\" role.")));
}

/*
 * Run fn(arg) with prolly_move_provenance the commits from (n of them): the
 * commits a move takes versions from, whose records name the relations the
 * tables it makes are (a table keeps its relation, by OID).
 */
#define WITH_PROVENANCE(from_, n_, ...) \
	do { \
		List	   *prov_ = NIL; \
		for (int pi_ = 0; pi_ < (n_); pi_++) \
			prov_ = lappend(prov_, (void *) &(from_)[pi_]); \
		prolly_move_provenance = prov_; \
		PG_TRY(_provenance); \
		{ \
			__VA_ARGS__; \
		} \
		PG_FINALLY(_provenance); \
		{ \
			prolly_move_provenance = NIL; \
		} \
		PG_END_TRY(_provenance); \
	} while (0)

/* A move of versions into the session's branch (VERSION_MOVE()). */
typedef struct VersionMove
{
	const ProllyHash *from;		/* the commits it takes versions from */
	int			n;
	bool		folded;			/* before is the branch folded before it */
	ProllyNodeRef before;
	List	   *relids;			/* and its tables' relations then */
} VersionMove;

/*
 * Before the move: the branch folded, and its tables' relations, if a table
 * here may take a version of another relation of its name from the commits,
 * as their records alone tell (cheaply: else nothing is folded, nor
 * checked after).
 */
static void
version_move_begin(VersionMove *vm, const ProllyHash *from, int n)
{
	vm->from = from;
	vm->n = n;
	vm->relids = NIL;
	vm->folded = n > 0 && prolly_versions_may_mismatch(from, n) &&
		PROLLY_CALL(refs(), working_set, NULL, &vm->before);
	if (vm->folded)
		vm->relids = prolly_relations_of(&vm->before);
}

/*
 * After it: refused, before the transaction can commit (the error undoes
 * what it wrote), if it put into a table here a version of another relation
 * (prolly_refuse_mismatched_versions()).  A merge keeps such a table as the
 * branch has it, a table identity conflict, so this refuses only what it
 * didn't tell.
 */
static void
version_move_end(VersionMove *vm)
{
	if (vm->folded)
		prolly_refuse_mismatched_versions(&vm->before, vm->relids, vm->from,
										  vm->n, PROLLY_MISMATCH_MOVE);
}

/*
 * Every move that brings a commit's table versions into the session's
 * branch (a reset, a merge, a pick, a revert, a table's checkout, a
 * stash's apply, a conflict's resolution, an abort) runs as
 * VERSION_MOVE(), with the commits it takes them from (from_, n_ of them):
 * its writes hushed, the tables it makes the relations those commits'
 * records name (WITH_PROVENANCE()), each made by the owner the record
 * names (the canonicalizer's prolly_move_maker()), and refused if it put
 * into a table here a version of another relation (version_move_end()).
 * A branch's first use, and a push's move of a branch, are checked so in
 * the refs.
 */
#define VERSION_MOVE(from_, n_, ...) \
	do { \
		VersionMove vm_; \
		version_move_begin(&vm_, (from_), (n_)); \
		WITH_PROVENANCE(from_, n_, BRANCH_WRITES_HUSHED(__VA_ARGS__)); \
		version_move_end(&vm_); \
	} while (0)

/*
 * Hold the refs (to the transaction's end) before what reads the head and
 * then replaces commits from it, as an amend or a rebase does: a commit
 * another session made in between would be dropped from the branch, the
 * head moved back past it.  The head read before is checked against the
 * head once held: moved meanwhile, a serialization failure, to retry.
 * True if a newer snapshot was pushed (refs' hold()), for the caller to pop.
 */
static bool
hold_unmoved(const char *what)
{
	ProllyHash	seen;
	ProllyHash	now;
	bool		had = PROLLY_CALL(refs(), resolve, "HEAD", &seen);
	bool		pushed = PROLLY_CALL(refs(), hold);
	bool		has = PROLLY_CALL(refs(), resolve, "HEAD", &now);

	if (had != has || (had && !same_hash(&seen, &now)))
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("could not serialize access: branch \"%s\" moved while %s waited for it",
						branch_ops()->current(), what),
				 errhint("The transaction might succeed if retried.")));
	return pushed;
}

/*
 * dolt_commit(VARIADIC args text[]) returns text[]: {commit id}
 *
 * -m/--message message (required, not empty), --author "Name <email>" (or
 * a name alone), -a/-A/--all (every change is always committed),
 * --date timestamp (the commit's date, as given: else now, and never
 * before a parent's), --allow-empty (else a commit whose root equals its
 * parent's is refused),
 * --skip-empty (nothing to commit: no commit, and NULL), -f/--force
 * (constraint violations don't stop the commit, as in Dolt; the
 * transaction still can't commit with them unless dolt.allow_commit_conflicts
 * is on), --amend (the head commit replaced by one of the working set with
 * its parents, and its message unless -m gives one, as Dolt's: never
 * empty, and refused with a merge in progress).
 */
Datum
dolt_commit(PG_FUNCTION_ARGS)
{
	char	  **args;
	int			nargs = text_args(fcinfo, &args);
	char	   *message = NULL;
	char	   *author = NULL;
	bool		allow_empty = false;
	bool		skip_empty = false;
	bool		force = false;
	bool		dated = false;
	bool		amend = false;
	bool		options = true;	/* until "--" */
	ProllyHash	id = {0};
	bool		committed = false;
	ProllyMergeState ms;
	ProllyCommitInfo amended;
	bool		amend_pushed = false;
	const char *result[1];

	for (int i = 0; i < nargs; i++)
	{
		char	   *a = args[i];

		if (options && strcmp(a, "--") == 0)
			options = false;	/* (and nothing after it) */
		else if (!options || a[0] != '-')
			unexpected("dolt_commit", a);
		else if (is_option(a, "m") || is_option(a, "message"))
			message = option_value(args, nargs, &i, "dolt_commit");
		else if (is_option(a, "author"))
			author = option_value(args, nargs, &i, "dolt_commit");
		else if (is_option(a, "date"))
		{
			date_override = given_date("dolt_commit",
									   option_value(args, nargs, &i, "dolt_commit"));
			dated = true;
		}
		else if (is_option(a, "all") || is_option(a, "ALL"))
			continue;
		else if (is_option(a, "allow-empty"))
			allow_empty = true;
		else if (is_option(a, "skip-empty"))
			skip_empty = true;
		else if (is_option(a, "force"))
			force = true;
		else if (is_option(a, "amend"))
			amend = true;
		else if (short_flags(a) != NULL)
		{
			/* Short options together, as -Am message. */
			for (const char *c = short_flags(a); *c; c++)
			{
				if (*c == 'a' || *c == 'A')
					continue;
				if (*c == 'f')
				{
					force = true;
					continue;
				}
				if (*c == 'm' && c[1] == '\0')
					message = option_value(args, nargs, &i, "dolt_commit");
				else
					unsupported("dolt_commit", a);
			}
		}
		else
			unsupported("dolt_commit", a);
	}
	if (amend)
	{
		ProllyHash	head;

		amend_pushed = hold_unmoved("dolt_commit('--amend')");
		if (PROLLY_CALL(refs(), merge_state, &ms))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("you are in the middle of a merge -- cannot amend")));
		memset(&amended, 0, sizeof(amended));
		if (!PROLLY_CALL(refs(), resolve, "HEAD", &head) ||
			!PROLLY_CALL(refs(), commit_get, &head, &amended))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("nothing to amend"),
					 errdetail("The branch has no commit.")));
		if (message == NULL)
			message = amended.message;
	}
	if (message == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dolt_commit needs a message"),
				 errhint("Pass -m and a message.")));
	given_message("dolt_commit", message);
	given_author("dolt_commit", author);
	if (allow_empty && skip_empty)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dolt_commit can't --allow-empty and --skip-empty at once")));
	PreventCommandIfReadOnly("dolt_commit()");
	prolly_check_head_move(branch_ops()->current());
	author_override = author;
	commit_forced = force;
	date_given = dated;
	PG_TRY();
	{
		note_integration();
		if (amend)
		{
			if (!prolly_allow_commit_conflicts() &&
				PROLLY_CALL(refs(), violations) != NIL)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("cannot commit with constraint violations"),
						 errhint("Fix the rows and check them again with dolt_verify_constraints().")));
			/* (the head the new commit's parent, as a commit on it is made) */
			if (amended.nparents > 0)
				PROLLY_CALL(refs(), set_head, &amended.parents[0]);
			BRANCH_WRITES_HUSHED(PROLLY_CALL(refs(), commit_with, message,
											 amended.parents, amended.nparents,
											 NULL, &id));
			committed = true;
		}
		else
			BRANCH_WRITES_HUSHED(committed = PROLLY_CALL(refs(), commit, message,
														 allow_empty, &id));
	}
	PG_FINALLY();
	{
		author_override = NULL;
		commit_forced = false;
		date_given = false;
	}
	PG_END_TRY();
	if (amend_pushed)
		PopActiveSnapshot();

	/* Nothing changed: the root is its parent's, and nothing was recorded. */
	if (!committed && skip_empty && !PROLLY_CALL(refs(), merge_state, &ms))
	{
		settled();				/* (clean: as the head has it) */
		PG_RETURN_NULL();
	}
	if (!committed)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("nothing to commit"),
				 errhint("Pass --allow-empty to commit anyway.")));
	settled();
	result[0] = hash_hex(&id);
	PG_RETURN_DATUM(text_array(1, result));
}

static bool
detached_branch(const char *name)
{
	return strncmp(name, PROLLY_DETACHED_PREFIX, strlen(PROLLY_DETACHED_PREFIX)) == 0;
}

static bool
writer_branch_name(const char *name)
{
	return strncmp(name, PROLLY_WRITER_PREFIX, strlen(PROLLY_WRITER_PREFIX)) == 0;
}

static void
reserved_branch_name(const char *name)
{
	if (detached_branch(name))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("branch names beginning \"%s\" are reserved", PROLLY_DETACHED_PREFIX)));
	if (writer_branch_name(name))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("branch names beginning \"%s\" are reserved", PROLLY_WRITER_PREFIX)));
}

/*
 * A connection doesn't name a hidden branch, as dolt_checkout() doesn't:
 * on it, it would be writable, and its writes would move the branch off
 * the commit that the sessions detached there are at.  (A writer's branch
 * is connected to as any other.)
 */
void
prolly_check_connect_branch(const char *branch)
{
	if (detached_branch(branch))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("branch names beginning \"%s\" are reserved", PROLLY_DETACHED_PREFIX),
				 errhint("Check out the commit with dolt_checkout(), read-only.")));
}

/* Does what's checked out name a commit by its hash (a prefix of it)? */
static bool
names_commit(const char *ref)
{
	size_t		n = strlen(ref);

	return n >= 4 && strspn(ref, "0123456789abcdef") == n;
}

/*
 * Make a branch, though the transaction is read-only for the branch it
 * started on (a session on a commit makes branches there); not one that is
 * read-only otherwise (asked so, or in recovery).
 */
static void
create_branch(const char *name, const char *from)
{
	bool		was_read_only = XactReadOnly;

	if (XactReadOnlyFromBranch)
		XactReadOnly = false;
	PG_TRY();
	{
		BRANCH_WRITES_HUSHED(PROLLY_CALL(refs(), branch_create, name, from));
	}
	PG_FINALLY();
	{
		XactReadOnly = was_read_only;
	}
	PG_END_TRY();
}

/*
 * A forced create (or copy) of branch name, which exists: moved to from's
 * head commit (the session branch's, if NULL), its uncommitted changes
 * dropped, as Dolt moves it; not the session's own branch.  The start
 * point is resolved first, so it may name the branch itself.  The move is
 * a compare-and-swap: refused if another session moved or deleted the
 * branch meanwhile.
 */
static void
force_branch(const char *name, const char *from)
{
	ProllyHash	at;
	ProllyHash	head;
	bool		moved = false;
	bool		was_read_only = XactReadOnly;
	List	   *written;
	ListCell   *lc;

	if (strcmp(name, branch_ops()->current()) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("branch \"%s\" is checked out", name),
				 errhint("Check out another branch first.")));

	/*
	 * (made, as a create is, though the transaction is read-only for the
	 * branch it started on: the checks too, and another branch's rows
	 * written, as a create makes its copies)
	 */
	if (XactReadOnlyFromBranch)
		XactReadOnly = false;
	PG_TRY();
	{
		/* (the branch, which exists, moved: a head move's privileges) */
		PreventCommandIfReadOnly("dolt_branch()");
		prolly_check_head_move(name);
		written = branches_written();

		/*
		 * (no commit to move to, or from: a branch without commits, made
		 * again as a copy of the start point as it is)
		 */
		if (!PROLLY_CALL(refs(), resolve, from ? from : "HEAD", &head) ||
			!PROLLY_CALL(refs(), resolve, name, &at))
		{
			Oid			creator = InvalidOid;

			/*
			 * (the same branch, moved: made again on the session's behalf,
			 * its move checked already, and its owner kept)
			 */
			if (PROLLY_HAS(refs(), branch_creator) &&
				!PROLLY_CALL(refs(), branch_creator, name, &creator))
				creator = InvalidOid;
			PROLLY_CALL(refs(), branch_delete, name, true);
			BranchCreateOnBehalf = true;
			PG_TRY(_remade);
			{
				create_branch(name, from);
			}
			PG_FINALLY(_remade);
			{
				BranchCreateOnBehalf = false;
			}
			PG_END_TRY(_remade);
			if (PROLLY_HAS(refs(), branch_set_creator))
				PROLLY_CALL(refs(), branch_set_creator, name, creator);
			moved = true;
		}
		else
			moved = PROLLY_CALL(refs(), branch_move, name, &at, &head, true);

		/*
		 * The branch's rows, moved to the head, are the move's, as a
		 * reset's are, not the transaction's changes: not noted as its
		 * writes (once allowed, the writer modes having been told).
		 */
		foreach(lc, branches_written())
			if (lfirst_oid(lc) != MyBranch && !list_member_oid(written, lfirst_oid(lc)))
				branch_forget_written(lfirst_oid(lc));
	}
	PG_FINALLY();
	{
		XactReadOnly = was_read_only;
	}
	PG_END_TRY();
	if (!moved)
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("branch \"%s\" changed while it was moved", name),
				 errhint("Try again.")));
}

/*
 * Check out a branch, though the transaction is read-only for the branch it
 * started on: a branch first used gets its copies (writes), as one made
 * there does.
 */
static void
checkout_branch(const char *name)
{
	bool		was_read_only = XactReadOnly;

	if (XactReadOnlyFromBranch)
		XactReadOnly = false;
	PG_TRY();
	{
		BRANCH_WRITES_HUSHED(PROLLY_CALL(refs(), checkout, name));
	}
	PG_FINALLY();
	{
		XactReadOnly = was_read_only;
	}
	PG_END_TRY();
}

/* Does branch name have an owner (its changes a writer's, in an owning mode)? */
static bool
branch_owned(const char *name)
{
	HeapTuple	tup = SearchSysCache1(BRANCHNAME, CStringGetDatum(name));
	bool		owned = false;

	if (HeapTupleIsValid(tup))
	{
		bool		isnull;

		(void) SysCacheGetAttr(BRANCHNAME, tup, Anum_pg_branch_browner, &isnull);
		owned = !isnull;
		ReleaseSysCache(tup);
	}
	return owned;
}

/*
 * A writer branch done with: clean and merged into its base (its head an
 * ancestor of the base's, or the base's itself).  Clean is no owner, in an
 * owning mode; in the shared mode, where no branch has one, its working set
 * its head's.  (The cheap tests first: a sweep asks of every branch.)
 */
static bool
done_writer_branch(const char *name, const ProllyHash *head)
{
	char	   *writer;
	char	   *base;
	Oid			role;
	ProllyHash	base_head;
	ProllyHash	merge_base;

	if (!writer_branch_name(name) || head->len == 0 || branch_owned(name) ||
		!PROLLY_CALL(writers(), writer_branch, name, &writer, &base, &role) ||
		!PROLLY_CALL(refs(), resolve, base, &base_head))
		return false;
	if (PROLLY_CALL(writers(), mode) == PROLLY_WRITERS_SHARED)
	{
		ProllyNodeRef ws;
		ProllyNodeRef committed;
		List	   *left_out = NIL;

		if (!(PROLLY_CALL(refs(), read_working_set, name, InvalidOid, &ws, &left_out) &&
			  left_out == NIL &&
			  PROLLY_CALL(refs(), commit_working_set, head, &committed) &&
			  same_hash(&ws.hash, &committed.hash)))
			return false;
	}
	return same_hash(&base_head, head) ||
		(PROLLY_CALL(refs(), merge_base, head, &base_head, &merge_base) &&
		 same_hash(&merge_base, head));
}

typedef struct DetachedBranches
{
	List	   *names;
	char	   *current;
	const char *only;			/* (non-NULL: is just this one still one?) */
	bool		found;
} DetachedBranches;

static void
add_detached(const char *name, const ProllyHash *head, void *arg)
{
	DetachedBranches *db = arg;

	if (strcmp(name, db->current) == 0 ||
		(db->only != NULL && strcmp(name, db->only) != 0))
		return;
	if (!(detached_branch(name) || done_writer_branch(name, head)))
		return;
	if (db->only != NULL)
		db->found = true;
	else
		db->names = lappend(db->names, pstrdup(name));
}

/*
 * add_detached() each branch named with prefix, and with prefix2 (NULL:
 * none): those alone, if the refs can visit branches by their names, else
 * every branch.
 */
static void
visit_hidden(const char *prefix, const char *prefix2, DetachedBranches *db)
{
	if (!PROLLY_HAS(refs(), branches_named))
	{
		PROLLY_CALL(refs(), branches, add_detached, db);
		return;
	}
	PROLLY_CALL(refs(), branches_named, prefix, add_detached, db);
	if (prefix2 != NULL)
		PROLLY_CALL(refs(), branches_named, prefix2, add_detached, db);
}

/* What a sweep deletes its candidates with (sweep_one()). */
typedef struct Sweep
{
	char	   *current;
	MemoryContext cxt;
	ResourceOwner owner;
	bool		was_read_only;
} Sweep;

/*
 * Delete hidden branch name, in a subtransaction, if it is still one no
 * session is on: true if it is gone, deleted now or (a commit's hidden
 * branch) by another sweep meanwhile.  One a session is on, or that the
 * session may not delete, is left; any other error is the checkout's.
 *
 * Another checkout can delete a candidate, or a session write on a writer
 * branch and leave it, while this one waits for the refs.  Each is asked
 * again with the refs held, in its subtransaction: one no longer to be
 * deleted, or one that is left, lets the refs go with the subtransaction,
 * so a checkout that deletes nothing holds up no other mover.
 */
static bool
sweep_one(const char *name, void *arg)
{
	Sweep	   *sw = arg;
	DetachedBranches still = {NIL, sw->current, name, false};
	bool		deleted = false;

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(sw->cxt);
	PG_TRY();
	{
		PROLLY_CALL(refs(), lock);
		AcceptInvalidationMessages();
		visit_hidden(name, NULL, &still);
		if (still.found)
		{
			BRANCH_WRITES_HUSHED(PROLLY_CALL(refs(), branch_delete,
											 name, true));
			if (writer_branch_name(name))
				PROLLY_CALL(writers(), set_writer_branch, name,
							NULL, NULL);
			ReleaseCurrentSubTransaction();
			deleted = true;
			if (detached_branch(name))
				pgstat_count_prolly(PGSTAT_PROLLY_KEPT_CHECKOUTS_EVICTED, 1);
		}
		else
		{
			RollbackAndReleaseCurrentSubTransaction();
			/* (a commit's hidden branch no longer one is gone: another's sweep) */
			deleted = detached_branch(name);
		}
		MemoryContextSwitchTo(sw->cxt);
		CurrentResourceOwner = sw->owner;
	}
	PG_CATCH();
	{
		ErrorData  *ed;

		MemoryContextSwitchTo(sw->cxt);
		ed = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(sw->cxt);
		CurrentResourceOwner = sw->owner;
		/* (a cancel, or anything else, isn't the branch kept) */
		if (ed->sqlerrcode != ERRCODE_OBJECT_IN_USE &&
			ed->sqlerrcode != ERRCODE_INSUFFICIENT_PRIVILEGE)
		{
			XactReadOnly = sw->was_read_only;
			ReThrowError(ed);
		}
		FreeErrorData(ed);
	}
	PG_END_TRY();
	return deleted;
}

/*
 * The hidden branches no session is on any more (current is the session's
 * branch): writer branches done with are deleted; commits' hidden
 * branches are kept materializations, which the materialization cache
 * keeps up to its budget, deleting the rest (sweep_one() each); kept_only,
 * only those.  Only in a transaction that can write, or that is read-only
 * only for the branch it started on (one leaving a commit).
 */
static void
sweep_detached(char *current, bool kept_only)
{
	DetachedBranches db = {NIL, current, NULL, false};
	Sweep		sw = {current, CurrentMemoryContext, CurrentResourceOwner,
	XactReadOnly};
	List	   *kept = NIL;

	if ((XactReadOnly && !XactReadOnlyFromBranch) || RecoveryInProgress())
		return;
	/* (only hidden branches are candidates: those named so are read) */
	visit_hidden(PROLLY_DETACHED_PREFIX, PROLLY_WRITER_PREFIX, &db);
	if (db.names == NIL)
		return;
	XactReadOnly = false;
	foreach_ptr(char, name, db.names)
	{
		if (detached_branch(name))
			kept = lappend(kept, name);
		else if (!kept_only)
			(void) sweep_one(name, &sw);
	}
	if (kept != NIL)
		PROLLY_CALL(matcache(), evict, kept, sweep_one, &sw);
	XactReadOnly = sw.was_read_only;
}

/*
 * Does the database have a commit's hidden branch?  (Read without the
 * refs.)  The first name from the hidden branches' prefix on, in the name
 * index's order, answers it: one probe, however many branches there are.
 */
static bool
has_detached_branches(void)
{
	Relation	brel = table_open(BranchRelationId, AccessShareLock);
	Relation	irel = index_open(BranchNameIndexId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;
	NameData	start;
	bool		found;

	namestrcpy(&start, PROLLY_DETACHED_PREFIX);
	ScanKeyInit(&key, Anum_pg_branch_brname, BTGreaterEqualStrategyNumber,
				F_NAMEGE, NameGetDatum(&start));
	scan = systable_beginscan_ordered(brel, irel, NULL, 1, &key);
	tup = systable_getnext_ordered(scan, ForwardScanDirection);
	found = tup != NULL &&
		detached_branch(NameStr(((Form_pg_branch) GETSTRUCT(tup))->brname));
	systable_endscan_ordered(scan);
	index_close(irel, AccessShareLock);
	table_close(brel, AccessShareLock);
	return found;
}

bool
prolly_let_go_of_kept_states(void)
{
	int			budget = prolly_kept_checkouts;

	if (!IsTransactionState() || RecoveryInProgress() || !has_detached_branches())
		return false;
	prolly_kept_checkouts = 0;
	PG_TRY();
	{
		sweep_detached(branch_ops()->current(), true);
	}
	PG_FINALLY();
	{
		prolly_kept_checkouts = budget;
	}
	PG_END_TRY();
	return true;
}

/*
 * Objects of other databases may be branches, tags and stashes the role
 * made there, which only that database's DROP OWNED or REASSIGN OWNED
 * changes, or copies its kept states hold; a commit's hidden branch here
 * is one a session is on.
 */
const char *
prolly_drop_role_hint(const char *detail)
{
	if (detail == NULL)
		return NULL;
	if (strstr(detail, " in database ") != NULL)
		return _("In a database named, REASSIGN OWNED or DROP OWNED there changes the branches, tags and stashes the role made; its kept detached checkouts (prolly.kept_checkouts) may hold copies of its objects too: set prolly.kept_checkouts to 0 there and check out a branch to let them go.");
	if (strstr(detail, "branch \"" PROLLY_DETACHED_PREFIX) != NULL)
		return _("A database's kept detached checkouts (prolly.kept_checkouts) hold copies of its objects: set prolly.kept_checkouts to 0 and check out a branch to let them go.");
	return NULL;
}


/* A root's tables' qualified names, appended to *names (each once). */
static void
collect_table_name(const char *name, const ProllyNodeRef *data,
				   const ProllyValue *description, const char *unreadable,
				   void *arg)
{
	List	  **names = arg;

	*names = list_append_unique(*names, makeString(pstrdup(name)));
}

/*
 * A table named as a user names it, qualified as roots name tables: the
 * relation it names now, or (one gone since) the first of from's tables
 * of that name in the search path.  NULL if neither.
 */
static char *
checkout_table_name(const char *name, List *from_tables)
{
	List	   *parts = stringToQualifiedNameList(name, NULL);
	Oid			relid = RangeVarGetRelid(makeRangeVarFromNameList(parts), NoLock, true);
	ListCell   *lc;

	if (OidIsValid(relid))
		return quote_qualified_identifier(get_namespace_name(get_rel_namespace(relid)),
										  get_rel_name(relid));
	if (list_length(parts) == 2)
	{
		char	   *q = quote_qualified_identifier(strVal(linitial(parts)),
												   strVal(lsecond(parts)));

		return list_member(from_tables, makeString(q)) ? q : NULL;
	}
	foreach(lc, fetch_search_path(false))
	{
		char	   *nsp = get_namespace_name(lfirst_oid(lc));
		char	   *q;

		if (nsp == NULL)
			continue;
		q = quote_qualified_identifier(nsp, strVal(llast(parts)));
		if (list_member(from_tables, makeString(q)))
			return q;
	}
	return NULL;
}

/*
 * The tables named put back as ref's commit has them, the rest of the
 * working set as it is: their uncommitted changes discarded, as Dolt's
 * dolt_checkout -- tables.  ".": every table ref's commit or the head
 * has, as Dolt's (the head standing for its index): a table only the
 * working set has stays.
 */
static void
checkout_tables(const char *ref, char **names, int nnames)
{
	ProllyHash	commit;
	ProllyNodeRef from;
	ProllyNodeRef working;
	ProllyNodeRef target;
	List	   *from_tables = NIL;
	List	   *head_tables = NIL;
	List	   *tables = NIL;

	if (!PROLLY_HAS(canonicalizer(), splice))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("the database's version control can't check out tables")));
	PreventCommandIfReadOnly("dolt_checkout()");
	if (!PROLLY_CALL(refs(), resolve, ref, &commit) ||
		!PROLLY_CALL(refs(), commit_working_set, &commit, &from))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("prolly ref \"%s\" does not exist", ref)));
	if (!PROLLY_CALL(refs(), working_set, NULL, &working))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("the current branch has no working set")));
	PROLLY_CALL(canonicalizer(), tables, &from, NULL, collect_table_name, &from_tables);
	{
		ProllyHash	head;
		ProllyNodeRef head_root;

		if (PROLLY_CALL(refs(), resolve, "HEAD", &head) &&
			PROLLY_CALL(refs(), commit_working_set, &head, &head_root))
			PROLLY_CALL(canonicalizer(), tables, &head_root, NULL, collect_table_name,
						&head_tables);
	}
	for (int i = 0; i < nnames; i++)
	{
		char	   *q;

		if (strcmp(names[i], ".") == 0)
		{
			tables = list_concat_unique(tables, from_tables);
			tables = list_concat_unique(tables, head_tables);
			continue;
		}
		q = checkout_table_name(names[i], from_tables);
		if (q == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("table \"%s\" does not exist", names[i])));
		tables = list_append_unique(tables, makeString(q));
	}
	{
		List	   *strs = NIL;
		ListCell   *lc;

		foreach(lc, tables)
			strs = lappend(strs, strVal(lfirst(lc)));
		PROLLY_CALL(canonicalizer(), splice, &working, &from, strs, &target);
	}
	if (same_hash(&target.hash, &working.hash))
		return;

	/*
	 * (a write, as any: it makes no commit, so a protected branch refuses
	 * it; the branch may be its head's again, its changes all put back)
	 */
	branch_note_write(MyBranch);
	VERSION_MOVE(&commit, 1,
				   PROLLY_CALL(diff_merge(), move_to, &target, &working));
	{
		ProllyHash	head;
		ProllyNodeRef head_root;

		if (PROLLY_CALL(refs(), resolve, "HEAD", &head) &&
			PROLLY_CALL(refs(), commit_working_set, &head, &head_root) &&
			same_hash(&target.hash, &head_root.hash))
			settled();
	}
}

/*
 * dolt_checkout(VARIADIC args text[]) returns text[]: {status, message}
 *
 * branch, or -b branch [start point] to create it first; a commit's hash,
 * or a tag's name, that names no branch, a detached session there,
 * read-only.  Tables, as
 * Dolt's: -- tables (or "."), or a name that is no branch but a table
 * (with more after it), put back as the head has them; ref tables (ref
 * no table), or ref -- tables, as ref has them; a name before -- that is
 * no ref is a table.  After --, one name that is a branch and no table is
 * the branch; branch -- is the branch.
 */
Datum
dolt_checkout(PG_FUNCTION_ARGS)
{
	char	  **args;
	int			nargs = text_args(fcinfo, &args);
	bool		options = true;	/* until "--" */
	char	   *branch = NULL;
	char	   *start = NULL;
	char	   *named = NULL;	/* the one name given apart from -b's */
	bool		create = false;
	const char *result[2];
	char	  **names = palloc_array(char *, Max(nargs, 1));
	int			nnames = 0;
	int			dashes = -1;	/* names before "--" */
	bool		is_branch = false;	/* names[0] is a branch */
	bool		is_table = false;	/* names[0] is a table (or ".") */

	/* (it changes the working set, or the branch's, a move's under it) */
	prolly_check_not_moving();
	for (int i = 0; i < nargs; i++)
	{
		char	   *a = args[i];

		if (options && strcmp(a, "--") == 0)
		{
			options = false;	/* names after it, even ones like flags */
			dashes = nnames;
		}
		else if (!options || a[0] != '-')
		{
			if (named == NULL)
				named = a;
			names[nnames++] = a;
		}
		else if (is_option(a, "b"))
		{
			if (create)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("dolt_checkout: multiple values provided for \"%s\"", a)));
			create = true;
			branch = option_value(args, nargs, &i, "dolt_checkout");
		}
		else
			unsupported("dolt_checkout", a);
	}
	if (!create && nnames > 0)
	{
		is_branch = PROLLY_CALL(refs(), branch_lookup, names[0],
								&(ProllyBranchId) {0});
		is_table = strcmp(names[0], ".") == 0 ||
			OidIsValid(RangeVarGetRelid(makeRangeVarFromNameList(stringToQualifiedNameList(names[0], NULL)),
										NoLock, true));
	}

	/*
	 * Before "--", a ref, as Dolt's, where it names one: a branch alone is
	 * the branch (a table of its name not taken); with names after it, the
	 * commit the tables come from.  One that names no ref is a table, put
	 * back with the rest as the head has them.
	 */
	if (!create && dashes > 0)
	{
		ProllyHash	ref;

		if (dashes > 1)
			unexpected("dolt_checkout", names[1]);
		if (nnames > 1 &&
			(is_branch || PROLLY_CALL(refs(), resolve, names[0], &ref)))
		{
			checkout_tables(names[0], names + 1, nnames - 1);
			result[0] = "0";
			result[1] = "";
			PG_RETURN_DATUM(text_array(2, result));
		}
		if (!is_branch && (nnames > 1 || is_table))
		{
			checkout_tables("HEAD", names, nnames);
			result[0] = "0";
			result[1] = "";
			PG_RETURN_DATUM(text_array(2, result));
		}
	}
	/* tables: after "--", or names that aren't one branch */
	else if (!create && nnames > 0)
	{
		if (dashes == 0 && !(nnames == 1 && is_branch && !is_table))
		{
			checkout_tables("HEAD", names, nnames);
			result[0] = "0";
			result[1] = "";
			PG_RETURN_DATUM(text_array(2, result));
		}
		if (dashes < 0 && nnames > 1 && !is_table)
		{
			checkout_tables(names[0], names + 1, nnames - 1);
			result[0] = "0";
			result[1] = "";
			PG_RETURN_DATUM(text_array(2, result));
		}
		if (dashes < 0 && !is_branch && !names_commit(names[0]) &&
			(is_table || nnames > 1))
		{
			checkout_tables("HEAD", names, nnames);
			result[0] = "0";
			result[1] = "";
			PG_RETURN_DATUM(text_array(2, result));
		}
		if (nnames > 1)
			unexpected("dolt_checkout", names[1]);
	}
	else if (create && nnames > 1)
		unexpected("dolt_checkout", names[1]);
	/* (-b's start point, before or after it, as in Dolt and git) */
	if (create)
		start = named;
	else
		branch = named;
	if (branch == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dolt_checkout needs a branch")));
	/*
	 * (the hidden branches are checked out by their commits; a writer branch
	 * is checked out as any other, but made only on a writer's behalf)
	 */
	if (create || !writer_branch_name(branch))
		reserved_branch_name(branch);
	/* (the session's branch, as it still is: else checked out, to fail so) */
	AcceptInvalidationMessages();
	if (!create && strcmp(branch, branch_ops()->current()) == 0 &&
		PROLLY_CALL(refs(), branch_lookup, branch, &(ProllyBranchId) {0}))
	{
		sweep_detached(branch, false);
		result[0] = "0";
		result[1] = psprintf("Already on branch '%s'", branch);
		PG_RETURN_DATUM(text_array(2, result));
	}

	/*
	 * A commit's hash, or a tag, that names no branch: a detached session
	 * on the commit, read-only, on a hidden branch at the commit that every
	 * session detached there shares, as a revision database of a tag is
	 * read-only in Dolt.  Changing anything takes a branch made from it.
	 * (Its transactions are read-only from the next one; this one from
	 * now.)
	 */
	if (!create && (names_commit(branch) || names_tag(branch)) &&
		!PROLLY_CALL(refs(), branch_lookup, branch, &(ProllyBranchId) {0}))
	{
		ProllyHash	id;
		ProllyHash	head;
		char	   *hidden;

		if (!PROLLY_CALL(refs(), resolve, branch, &id))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("branch or commit \"%s\" does not exist", branch)));
		/* (a branch name has room for 40 digits: the whole hash) */
		hidden = psprintf("%s%.40s", PROLLY_DETACHED_PREFIX, hash_hex(&id));
		/*
		 * (rolled back, the checkout leaves the session where it was: as a
		 * hidden branch made by it goes, so does the session from one it
		 * found)
		 */
		branch_return_on_abort();
		if (!PROLLY_CALL(refs(), branch_lookup, hidden, &(ProllyBranchId) {0}))
			create_branch(hidden, hash_hex(&id));
		else if (!PROLLY_CALL(refs(), resolve, hidden, &head) || !same_hash(&head, &id))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("the hidden branch of commit %s has moved off it", hash_hex(&id))));
		else if (PROLLY_CALL(matcache(), kept, hidden))
		{
			ProllyCommitInfo info;

			/*
			 * (its copies, kept, are the commit's as a first use would make
			 * them: what the commit needs from outside is checked as a first
			 * use checks it)
			 */
			if (PROLLY_CALL(refs(), commit_get, &id, &info))
				prolly_check_commit_dependencies(&info, NULL, "checkout", false);
			pgstat_count_prolly(PGSTAT_PROLLY_KEPT_CHECKOUTS_REUSED, 1);
		}
		checkout_branch(hidden);
		PROLLY_CALL(matcache(), note_use, hidden);
		/* (the hidden branches past the cache's budget go: not this one) */
		sweep_detached(hidden, true);
		MyBranchReadOnly = true;
		if (!XactReadOnly)
			XactReadOnlyFromBranch = true;
		XactReadOnly = true;
		result[0] = "0";
		result[1] = psprintf("HEAD is now at %s (detached, read-only)", hash_hex(&id));
		PG_RETURN_DATUM(text_array(2, result));
	}

	/*
	 * A branch: writable from the next transaction (this one stays as it
	 * began); the hidden branches nobody is on any more go.
	 */
	if (create)
		create_branch(branch, start);
	checkout_branch(branch);
	sweep_detached(branch, false);
	result[0] = "0";
	result[1] = psprintf("Switched to branch '%s'", branch);
	PG_RETURN_DATUM(text_array(2, result));
}

/*
 * dolt_branch(VARIADIC args text[]) returns text[]: {status}
 *
 * branch [start point]; -c/--copy from to; -d/--delete branch...;
 * -D (or -d with -f/--force) branch....  Forced, a branch made (or
 * copied) takes the place of one of its name, as in Dolt, unless it is
 * the session's own.
 */
Datum
dolt_branch(PG_FUNCTION_ARGS)
{
	char	  **args;
	int			nargs = text_args(fcinfo, &args);
	bool		delete = false;
	bool		copy = false;
	bool		move = false;
	bool		force = false;
	bool		take = false;
	ProllyBranchProtection protection = PROLLY_PROTECT_DEFAULT;
	bool		options = true;
	bool		was_protected;
	bool		replaces_protected;
	List	   *names = NIL;
	const char *result[1] = {"0"};

	for (int i = 0; i < nargs; i++)
	{
		char	   *a = args[i];

		if (!options || a[0] != '-')
			names = lappend(names, a);
		else if (strcmp(a, "--") == 0)
			options = false;	/* names after it, even ones like flags */
		else if (is_option(a, "delete"))
			delete = true;
		else if (is_option(a, "force"))
			force = true;
		else if (is_option(a, "copy"))
			copy = true;
		else if (is_option(a, "take"))
			take = true;
		else if (is_option(a, "protect"))
			protection = PROLLY_PROTECT_ON;
		else if (is_option(a, "unprotect"))
			protection = PROLLY_PROTECT_OFF;
		else if (is_option(a, "move"))
			move = true;
		else if (short_flags(a) == NULL)
			unsupported("dolt_branch", a);
		else
		{
			/* short flags, one or several together ("-mf") */
			for (const char *f = short_flags(a); *f; f++)
			{
				if (*f == 'd')
					delete = true;
				else if (*f == 'D')
					delete = force = true;
				else if (*f == 'f')
					force = true;
				else if (*f == 'c')
					copy = true;
				else if (*f == 'm')
					move = true;
				else if (*f == 'M')
					move = force = true;
				else
					unsupported("dolt_branch", a);
			}
		}
	}
	if (names == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dolt_branch needs a branch")));

	/*
	 * (nor is a commit's hidden branch renamed: renamed, it would be an
	 * ordinary, writable branch where the sessions detached at the commit
	 * are; said before a detached session's read-only transaction is, as
	 * the more specific)
	 */
	if (move && detached_branch(linitial(names)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a commit's hidden branch can't be renamed"),
				 errhint("Make a branch at the commit with dolt_branch().")));

	/*
	 * (a rename, delete, take or protection makes no branch: not in a
	 * transaction read-only for the branch it started on either, unlike a
	 * create, forced or not, which a session on a commit makes to change
	 * anything)
	 */
	if (move || delete || take || protection != PROLLY_PROTECT_DEFAULT)
		PreventCommandIfReadOnly("dolt_branch()");
	if (protection != PROLLY_PROTECT_DEFAULT)
	{
		ListCell   *lc;

		if (delete || copy || move || take)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("dolt_branch --protect and --unprotect take no other option")));
		foreach(lc, names)
		{
			ProllyHash	head;

			if (!PROLLY_CALL(refs(), resolve, (char *) lfirst(lc), &head))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("branch \"%s\" does not exist", (char *) lfirst(lc))));
			PROLLY_CALL(writers(), set_protected, (char *) lfirst(lc), protection);
		}
		PG_RETURN_DATUM(text_array(1, result));
	}
	if (take)
	{
		ListCell   *lc;

		if (delete || copy || move)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("dolt_branch --take takes no other option")));
		foreach(lc, names)
		{
			HeapTuple	tup = SearchSysCache1(BRANCHNAME,
											  CStringGetDatum((char *) lfirst(lc)));
			Oid			branch;

			/* (a branch never used has no changes to take) */
			if (!HeapTupleIsValid(tup))
			{
				ProllyHash	head;

				if (!PROLLY_CALL(refs(), resolve, (char *) lfirst(lc), &head))
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("branch \"%s\" does not exist",
									(char *) lfirst(lc))));
				continue;
			}
			branch = ((Form_pg_branch) GETSTRUCT(tup))->oid;
			ReleaseSysCache(tup);
			PROLLY_CALL(writers(), take, branch);
		}
		PG_RETURN_DATUM(text_array(1, result));
	}
	if ((int) delete + (int) copy + (int) move > 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dolt_branch takes one of -d, -c and -m")));
	if (move)
	{
		if (list_length(names) != 2)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("dolt_branch -m needs the branch to rename and its new name")));
		reserved_branch_name(lsecond(names));

		/*
		 * (a writer's branch is named by its writer; a protected one is the
		 * database owner's to rename, and stays protected by its new name)
		 */
		if (writer_branch_name(linitial(names)) || writer_branch_name(lsecond(names)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("a writer's branch can't be renamed")));
		/* (nor a rebase's, made on the session's behalf for the rebase alone) */
		if (strncmp(linitial(names), REBASE_BRANCH_PREFIX,
					strlen(REBASE_BRANCH_PREFIX)) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("a rebase's branch can't be renamed"),
					 errhint("Finish the rebase with dolt_rebase('--continue'), or abort it.")));
		was_protected = PROLLY_CALL(writers(), protected, linitial(names));
		/* (the branch a forced rename replaces) */
		replaces_protected = force &&
			strcmp(linitial(names), lsecond(names)) != 0 &&
			PROLLY_CALL(refs(), resolve, lsecond(names), &(ProllyHash) {0}) &&
			PROLLY_CALL(writers(), protected, lsecond(names));
		for (int i = 0; i < 2; i++)
		{
			char	   *n = list_nth(names, i);

			if ((i == 0 ? was_protected : replaces_protected) &&
				!object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId()))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("branch \"%s\" is protected", n),
						 errhint("Only the database's owner renames or replaces a protected branch.")));
		}

		/*
		 * (the renamed branch is protected by its new name, as the one it
		 * replaces was: protected so first, which a branch with changes, or
		 * an owner, isn't)
		 */
		if (replaces_protected && !was_protected)
		{
			PROLLY_CALL(writers(), set_protected, linitial(names),
						PROLLY_PROTECT_ON);
			was_protected = true;
		}
		BRANCH_WRITES_HUSHED(PROLLY_CALL(refs(), branch_rename, linitial(names),
										 lsecond(names), force));
		if (was_protected)
		{
			PROLLY_CALL(writers(), set_protected, linitial(names),
						PROLLY_PROTECT_DEFAULT);
			PROLLY_CALL(writers(), set_protected, lsecond(names),
						PROLLY_PROTECT_ON);
		}
		PG_RETURN_DATUM(text_array(1, result));
	}
	if (delete)
	{
		ListCell   *lc;

		/* (a protected branch is the database owner's to delete) */
		foreach(lc, names)
		{
			if (PROLLY_CALL(writers(), protected, (char *) lfirst(lc)) &&
				!object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId()))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("branch \"%s\" is protected", (char *) lfirst(lc)),
						 errhint("Only the database's owner deletes a protected branch.")));
		}
		BRANCH_WRITES_HUSHED(
			foreach(lc, names)
				PROLLY_CALL(refs(), branch_delete, (char *) lfirst(lc), force);
		);
		foreach(lc, names)
		{
			if (writer_branch_name((char *) lfirst(lc)))
				PROLLY_CALL(writers(), set_writer_branch, (char *) lfirst(lc),
							NULL, NULL);
		}
		foreach(lc, names)
		{
			if (object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId()))
				PROLLY_CALL(writers(), set_protected, (char *) lfirst(lc),
							PROLLY_PROTECT_DEFAULT);
		}
	}
	else
	{
		char	   *name;
		char	   *from;

		if (copy && list_length(names) != 2)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("dolt_branch -c needs the branch to copy and a new name")));
		if (!copy && list_length(names) > 2)
			unexpected("dolt_branch", (char *) lthird(names));
		name = copy ? lsecond(names) : linitial(names);
		from = copy ? linitial(names) :
			list_length(names) > 1 ? (char *) lsecond(names) : NULL;
		reserved_branch_name(name);
		if (force && PROLLY_CALL(refs(), branch_lookup, name, &(ProllyBranchId) {0}))
		{
			/* (a protected branch is the database owner's to move so) */
			if (PROLLY_CALL(writers(), protected, name) &&
				!object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId()))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("branch \"%s\" is protected", name),
						 errhint("Only the database's owner moves a protected branch.")));
			force_branch(name, from);
		}
		else
			create_branch(name, from);
		/* (a copy is protected as its source is, where the owner may say so) */
		if (copy && from && strcmp(from, "main") != 0 &&
			PROLLY_CALL(writers(), protected, from) &&
			object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId()))
			PROLLY_CALL(writers(), set_protected, name, PROLLY_PROTECT_ON);
	}
	PG_RETURN_DATUM(text_array(1, result));
}

/*
 * End the merge (what: "merge", or "cherry-pick") in progress on the
 * session's branch: the branch as its head has it.  (The working set's
 * rows are reset, the transaction's writes as any others.)  A merge isn't
 * begun over uncommitted changes, so none are lost.
 */
static void
abort_in_progress(const char *what)
{
	ProllyHash	head = {0};
	ProllyMergeState ms;
	char	   *saved = NULL;

	/* (the privileges a resolution with theirs took from the branch's) */
	if (PROLLY_CALL(refs(), merge_state, &ms))
		saved = ms.identity_saved;
	branch_note_write(MyBranch);
	(void) PROLLY_CALL(refs(), resolve, "HEAD", &head);
	/* (a table its resolution made theirs: the head's relation again) */
	if (head.len > 0)
		BRANCH_WRITES_HUSHED(prolly_drop_mismatched_tables(&head,
														   psprintf("abort the %s", what),
														   true));
	VERSION_MOVE(&head, head.len > 0 ? 1 : 0,
				   PROLLY_CALL(refs(), reset, "HEAD"));
	BRANCH_WRITES_HUSHED(prolly_restore_privileges(saved, false));
	settled();
}

/*
 * A merge's, pick's or revert's conflicts (or violations) the session
 * can't keep, said at elevel: what they are, and how to keep them.  An
 * ERROR rolls back the transaction; a NOTICE is for a rebase aborted
 * instead.  between: what the rows were changed on ("branches", "sides").
 */
static void
unkept_conflicts(const char *what, const char *What, const char *between,
				 int conflicts, int nviolations, int elevel)
{
	ProllyMergeState ms;
	const char *outcome = elevel >= ERROR ? "transaction rolled back" : "rebase aborted";

	/* (a table that is another table on the other side: kept, not merged) */
	if (PROLLY_CALL(refs(), merge_state, &ms) && ms.nidentity > 0)
		ereport(elevel,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("%s conflict detected, %s", what, outcome),
				 errdetail("Table %s is another table on the other side, a relation of its own of that name: a table identity conflict.",
						   ms.identity[0].name),
				 errhint("%s in a transaction block to resolve it with dolt_conflicts_resolve('--ours') or ('--theirs') before committing, or set dolt.allow_commit_conflicts to keep it.", What)));
	else if (conflicts > 0)
		ereport(elevel,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("%s conflict detected, %s", what, outcome),
				 errdetail_plural("%d row was changed differently on both %s.",
								  "%d rows were changed differently on both %s.",
								  conflicts, conflicts, between),
				 errhint("%s in a transaction block to resolve the conflicts with dolt_conflicts_resolve() before committing, or set dolt.allow_commit_conflicts to keep them.", What)));
	else
		ereport(elevel,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("%s constraint violations detected, %s", what, outcome),
				 errdetail_plural("%d row violates a constraint.",
								  "%d rows violate constraints.",
								  nviolations, nviolations),
				 errhint("%s in a transaction block to see them in dolt_constraint_violations() and fix them before committing, or set dolt.allow_commit_conflicts to keep them.", What)));
}

/*
 * A merge's, pick's or revert's conflicts (or violations), found outside a
 * transaction block: an error unless they may be kept.
 */
static void
stopped_outside_block(const char *what, const char *What, const char *between,
					  int conflicts, int nviolations)
{
	if (IsTransactionBlock() || prolly_allow_commit_conflicts())
		return;
	unkept_conflicts(what, What, between, conflicts, nviolations, ERROR);
}

static void
count_violation(const ProllyViolation *pv, void *arg)
{
	(*(int *) arg)++;
}

/*
 * How many constraint violations the session's branch keeps, counted as
 * they are visited, not loaded: those of tables since dropped too, so the
 * difference of two counts is what a merge or pick added.
 */
static int
violations_kept(void)
{
	int			n = 0;

	PROLLY_CALL(refs(), visit_violations, count_violation, &n);
	return n;
}

/*
 * A pick or revert starts only on a branch without constraint violations,
 * as Dolt's only on a clean working set (which holds them): the ones it
 * leaves are its own.
 */
static void
refuse_violations(const char *what)
{
	if (PROLLY_CALL(refs(), violations) != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot %s with constraint violations on the branch", what),
				 errhint("Resolve them, or reset the branch, first.")));
}

/*
 * dolt_merge(VARIADIC args text[]) returns text[]:
 *	  {commit id, fast_forward, conflicts, message}
 *
 * branch; --no-ff; --ff-only; --squash; --no-commit; -m/--message
 * message; --date timestamp (the merge commit's, as dolt_commit's); --abort (each option, long or short, with one dash or two,
 * as Dolt takes them; so too every dolt_* function's).  Rows changed differently on both sides leave the
 * merge in progress, with conflicts (dolt_conflicts) to resolve before
 * dolt_commit makes the merge commit; --no-commit leaves it so without
 * them.  As in Dolt, a transaction can't commit with conflicts unless
 * dolt.allow_commit_conflicts is on: outside a transaction block, the
 * merge fails and changes nothing.  The commit id is empty when no commit
 * was made or moved to (nothing to merge, a merge left in progress).
 */
Datum
dolt_merge(PG_FUNCTION_ARGS)
{
	char	  **args;
	int			nargs = text_args(fcinfo, &args);
	bool		options = true;	/* until "--" */
	char	   *branch = NULL;
	char	   *message = NULL;
	int			flags = 0;
	ProllyHash	before = {0};
	ProllyHash	theirs = {0};
	ProllyHash	id = {0};
	bool		had_head;
	bool		abort_merge = false;
	bool		commit = false;
	bool		dated = false;
	int			conflicts;
	int			known;
	const char *result[4];

	for (int i = 0; i < nargs; i++)
	{
		char	   *a = args[i];

		if (options && strcmp(a, "--") == 0)
			options = false;	/* names after it, even ones like flags */
		else if (!options || a[0] != '-')
		{
			if (branch == NULL)
				branch = a;
			else
				unexpected("dolt_merge", a);
		}
		else if (is_option(a, "no-ff"))
			flags |= PROLLY_MERGE_NO_FF;
		else if (is_option(a, "ff-only"))
			flags |= PROLLY_MERGE_FF_ONLY;
		else if (is_option(a, "squash"))
			flags |= PROLLY_MERGE_SQUASH;
		else if (is_option(a, "no-commit"))
			flags |= PROLLY_MERGE_NO_COMMIT;
		else if (is_option(a, "commit"))
			commit = true;		/* (as without it) */
		else if (is_option(a, "abort"))
			abort_merge = true;
		else if (is_option(a, "m") || is_option(a, "message"))
			message = option_value(args, nargs, &i, "dolt_merge");
		else if (is_option(a, "date"))
		{
			date_override = given_date("dolt_merge",
									   option_value(args, nargs, &i, "dolt_merge"));
			dated = true;
		}
		else
			unsupported("dolt_merge", a);
	}
	/* (an abort makes no commit: its -m is ignored, as Dolt's is) */
	if (abort_merge)
	{
		ProllyMergeState ms;

		if (branch != NULL)
			unexpected("dolt_merge", branch);
		if (!PROLLY_CALL(refs(), merge_state, &ms))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("there is no merge to abort")));
		PreventCommandIfReadOnly("dolt_merge()");
		prolly_check_head_move(branch_ops()->current());
		abort_in_progress("merge");
		result[0] = "";
		result[1] = "0";
		result[2] = "0";
		result[3] = "merge aborted";
		PG_RETURN_DATUM(text_array(4, result));
	}
	given_message("dolt_merge", message);
	if (branch == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dolt_merge needs a branch")));
	if (commit && (flags & PROLLY_MERGE_NO_COMMIT))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("flags \"--commit\" and \"--no-commit\" cannot be used together")));
	exclusive_flags(flags, PROLLY_MERGE_SQUASH, "--squash", PROLLY_MERGE_NO_FF, "--no-ff");
	exclusive_flags(flags, PROLLY_MERGE_FF_ONLY, "--ff-only", PROLLY_MERGE_NO_FF, "--no-ff");
	exclusive_flags(flags, PROLLY_MERGE_FF_ONLY, "--ff-only", PROLLY_MERGE_SQUASH, "--squash");

	PreventCommandIfReadOnly("dolt_merge()");
	prolly_check_head_move(branch_ops()->current());
	/* (the branch held first: its head is read as the merge finds it) */
	note_integration();
	had_head = PROLLY_CALL(refs(), resolve, "HEAD", &before);
	(void) PROLLY_CALL(refs(), resolve, branch, &theirs);
	known = violations_kept();
	date_given = dated;
	PG_TRY();
	{
		VERSION_MOVE(&theirs, theirs.len > 0 ? 1 : 0,
					 conflicts = PROLLY_CALL(diff_merge(), merge, branch, message,
											 flags, &id));
	}
	PG_FINALLY();
	{
		date_given = false;
	}
	PG_END_TRY();
	must_commit(conflicts, "a merge");
	if (conflicts > 0)
	{
		/* (the violations it found, besides those the branch had) */
		int			nviolations = violations_kept() - known;

		ProllyMergeState ms;

		conflicts -= nviolations;
		/* (a table's definitions in conflict, as Dolt's message says) */
		if (!IsTransactionBlock() && !prolly_allow_commit_conflicts() &&
			PROLLY_CALL(refs(), merge_state, &ms) && ms.nschema > 0)
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("merge conflict detected, transaction rolled back"),
					 ms.nschema == 1 ?
					 errdetail("Table %s was changed differently on both branches: a schema conflict.",
							   ms.schema_tables[0]) :
					 errdetail("Tables %s and %d others were changed differently on both branches: schema conflicts.",
							   ms.schema_tables[0], ms.nschema - 1),
					 errhint("Merge in a transaction block to see the conflicts in dolt_schema_conflicts, or set dolt.allow_commit_conflicts to keep them.")));
		stopped_outside_block("merge", "Merge", "branches", conflicts, nviolations);
		result[0] = "";
		result[1] = "0";
		result[2] = "1";
		result[3] = "conflicts found";	/* (violations too, as in Dolt) */
		PG_RETURN_DATUM(text_array(4, result));
	}
	/* (Dolt's messages, for nothing to merge) */
	if (had_head && same_hash(&id, &before))
	{
		result[0] = "";
		result[1] = "0";
		result[2] = "0";
		result[3] = same_hash(&before, &theirs) ? "Everything up-to-date" :
			"cannot fast forward from a to b. a is ahead of b already";
		PG_RETURN_DATUM(text_array(4, result));
	}
	/* (a merge not committed, --no-commit's or --squash's, leaves it dirty) */
	if (id.len > 0)
		settled();
	result[0] = id.len > 0 ? hash_hex(&id) : "";
	result[1] = had_head && same_hash(&id, &theirs) ? "1" : "0";
	result[2] = "0";
	result[3] = "merge successful";
	PG_RETURN_DATUM(text_array(4, result));
}

/* ----------------------------------------------------------------
 * Writer branches: in the branch-auto mode, a statement that writes, on a
 * protected branch, first moves the session to its writer branch for it.
 * ----------------------------------------------------------------
 */

static ExecutorStart_hook_type prev_executor_start = NULL;
static ExecutorRun_hook_type prev_executor_run = NULL;
static ExecutorFinish_hook_type prev_executor_finish = NULL;
static ProcessUtility_hook_type prev_process_utility = NULL;

/* how deep in statements the session is: 0 at the top, a client's own */
static int	statement_depth = 0;

/* a transaction that moved, which can't be prepared, and the subtransaction */
static LocalTransactionId moved_lxid = InvalidLocalTransactionId;
static SubTransactionId moved_subid = InvalidSubTransactionId;

static post_parse_analyze_hook_type prev_post_parse_analyze = NULL;

static bool utility_may_write_branch(Node *stmt);

/*
 * Move the session to its writer branch, if the statement about to run
 * writes on a protected branch in the branch-auto mode (true if it moved).
 * A branch made now forks from the protected branch's head as the
 * transaction's snapshot saw it (one that can't see a newer head fails, to
 * be retried; read committed takes a newer snapshot).  The move is undone
 * if the (sub)transaction aborts.  A writer branch is moved to only while
 * its definitions are surely the base's (made now, or clean at its head),
 * so that what was analyzed, or planned, on the base runs on it (*same).
 */
static bool
move_to_writer_branch(bool *same)
{
	char	   *base;
	char	   *writer;
	char	   *name;
	char	   *w;
	char	   *b;
	Oid			role;
	ProllyHash	head;
	ProllyHash	at;

	*same = false;
	if (!IsTransactionState() || RecoveryInProgress() ||
		(MyProc->vxid.lxid == stays_lxid && MyBranch == stays_branch &&
		 PROLLY_CALL(writers(), generation) == stays_gen) ||
		!database_has_versioned_schemas() ||
		branch_written(MyBranch))
		return false;
	base = pstrdup(branch_ops()->current());
	if (detached_branch(base) || writer_branch_name(base) ||
		PROLLY_CALL(writers(), mode) != PROLLY_WRITERS_BRANCH_AUTO ||
		!PROLLY_CALL(writers(), protected, base))
	{
		stays_lxid = MyProc->vxid.lxid;
		stays_branch = MyBranch;
		stays_gen = PROLLY_CALL(writers(), generation);
		return false;
	}
	*same = true;
	writer = prolly_current_writer();
	name = psprintf("%s%s/%s", PROLLY_WRITER_PREFIX, writer, base);
	if (strlen(name) >= NAMEDATALEN)
		ereport(ERROR,
				(errcode(ERRCODE_NAME_TOO_LONG),
				 errmsg("writer branch \"%s\" would be too long a name", name),
				 errhint("Set a shorter dolt.writer.")));

	if (PROLLY_CALL(writers(), writer_branch, name, &w, &b, &role))
	{
		const char *rolname = GetUserNameFromId(role, true);

		/* (a writer's branch is its role's: another can't write as it) */
		if (!object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId()) &&
			(rolname == NULL || !has_privs_of_role(GetUserId(), role)))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 rolname ?
					 errmsg("writer branch \"%s\" is role \"%s\"'s", name, rolname) :
					 errmsg("writer branch \"%s\" is a dropped role's", name),
					 rolname ? 0 :
					 errhint("The database's owner can delete it with dolt_branch('-D', '%s').",
							 name)));
		/*
		 * (one with changes of its own may define things otherwise than the
		 * base the statement was analyzed on: written on checked out)
		 */
		if (!(PROLLY_CALL(refs(), resolve, name, &at) &&
			  PROLLY_CALL(refs(), resolve, base, &head) && same_hash(&at, &head) &&
			  !branch_owned(name)))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("writer branch \"%s\" has changes of its own", name),
					 errdetail("A statement moves to a writer branch only while it is the base's as it is."),
					 errhint("Write there with dolt_checkout('%s'), or integrate it with dolt_integrate() first.",
							 name)));
		branch_return_on_abort();
	}
	else if (PROLLY_CALL(refs(), resolve, base, &head))
	{
		bool		pushed = PROLLY_CALL(refs(), hold);

		branch_return_on_abort();
		BranchCreateOnBehalf = true;
		PG_TRY();
		{
			create_branch(name, base);
		}
		PG_FINALLY();
		{
			BranchCreateOnBehalf = false;
		}
		PG_END_TRY();
		PROLLY_CALL(writers(), set_writer_branch, name, writer, base);
		if (pushed)
			PopActiveSnapshot();
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("branch \"%s\" is protected and has no commit to fork a writer branch from",
						base),
				 errhint("Commit it, or unprotect it, first.")));
	checkout_branch(name);
	CommandCounterIncrement();
	moved_lxid = MyProc->vxid.lxid;
	moved_subid = GetCurrentSubTransactionId();
	if (prolly_may_move_head(base))
		ereport(NOTICE,
				(errmsg("writing on branch \"%s\"", name),
				 errdetail("Branch \"%s\" is protected.", base),
				 errhint("Integrate the writes with dolt_integrate('-m', ...).")));
	else
		ereport(NOTICE,
				(errmsg("writing on branch \"%s\"", name),
				 errdetail("Branch \"%s\" is protected.", base),
				 errhint("Integrate the writes with dolt_integrate('-m', ...): rows of tables this role may write as they changed integrate; a change to a definition, or to rows it may not write so, takes the privileges of the \"pg_prolly_write\" role or the database's CREATE privilege.")));
	return true;
}

/* Find DML, including a modifying CTE in an otherwise read-only SELECT. */
static bool
raw_query_writes(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, InsertStmt) || IsA(node, UpdateStmt) ||
		IsA(node, DeleteStmt) || IsA(node, MergeStmt))
		return true;
	return raw_expression_tree_walker(node, raw_query_writes, context);
}

/* Utility wrappers may contain other utility nodes, not raw expressions. */
static bool
raw_statement_writes(Node *stmt)
{
	if (stmt == NULL)
		return false;
	switch (nodeTag(stmt))
	{
		case T_InsertStmt:
		case T_UpdateStmt:
		case T_DeleteStmt:
		case T_MergeStmt:
			return true;
		case T_SelectStmt:
			return raw_query_writes(stmt, NULL);
		case T_ExplainStmt:
			{
				/* Only EXPLAIN ANALYZE runs the statement it explains. */
				ExplainStmt *es = (ExplainStmt *) stmt;
				bool		analyze = false;

				foreach_node(DefElem, opt, es->options)
				{
					if (strcmp(opt->defname, "analyze") == 0)
						analyze = defGetBoolean(opt);
				}
				return analyze && raw_statement_writes(es->query);
			}
		case T_CreateTableAsStmt:
			return raw_statement_writes(((CreateTableAsStmt *) stmt)->query);
		default:
			/* EXECUTE and other utilities are handled by ProcessUtility. */
			return false;
	}
}

/*
 * Analysis takes RowExclusiveLock on a write's target.  Waiting until the
 * post-analysis hook to fork lets several creators hold those locks on the
 * protected source while one holds refs and waits for ShareLock to copy it.
 * Route before analysis instead, and before cached queries reacquire locks.
 * A branch switch invalidates the plan caches; analysis/revalidation below
 * therefore uses the destination's definitions and locks throughout.
 *
 * Utility commands still use the ProcessUtility hook.  Nested statements
 * cannot move the session out from under their caller's executor.
 */
void
prolly_prepare_writer_statement(Node *stmt)
{
	static bool preparing = false;
	bool		snapshot_set = false;
	bool		same;

	if (statement_depth != 0 || preparing || stmt == NULL ||
		!IsTransactionState() || !raw_statement_writes(stmt))
		return;

	preparing = true;
	PG_TRY();
	{
		/* Extended-protocol revalidation need not have an active snapshot. */
		if (!ActiveSnapshotSet())
		{
			PushActiveSnapshot(GetTransactionSnapshot());
			snapshot_set = true;
		}
		(void) move_to_writer_branch(&same);
		if (snapshot_set)
			PopActiveSnapshot();
	}
	PG_FINALLY();
	{
		preparing = false;
	}
	PG_END_TRY();
}

/* Lock a statement's relations again, on the branch the session is on now. */
static void
relock(List *rtable)
{
	ListCell   *lc;

	foreach(lc, rtable)
	{
		RangeTblEntry *rte = lfirst(lc);

		if (rte->rtekind == RTE_RELATION && rte->rellockmode != NoLock)
			LockRelationOid(rte->relid, rte->rellockmode);
	}
}

/* Does an analyzed query write?  (A query's writes are inside functions.) */
static bool
query_writes(Query *query)
{
	return (query->commandType != CMD_SELECT &&
			query->commandType != CMD_UTILITY) || query->hasModifyingCTE;
}

/*
 * Fallback for writes introduced by an analysis hook: ordinary raw DML was
 * already routed before taking any analysis locks.
 */
static void
writer_post_parse_analyze(ParseState *pstate, Query *query, const JumbleState *jstate)
{
	bool		same;

	if (prev_post_parse_analyze)
		prev_post_parse_analyze(pstate, query, jstate);
	if (statement_depth == 0 && query_writes(query) && move_to_writer_branch(&same))
		relock(query->rtable);
}

/* Does a planned statement write?  (A query's writes are inside functions.) */
static bool
plan_writes(PlannedStmt *stmt)
{
	return stmt->commandType != CMD_SELECT || stmt->hasModifyingCTE;
}

/*
 * A statement planned before it could move (a cached plan, as the extended
 * protocol's) moves as it starts: run on the writer branch only if that
 * has the base's definitions, its relations locked there as the plan locked
 * them, and the copies just made seen.
 */
static void
writer_executor_start(QueryDesc *queryDesc, int eflags)
{
	bool		same;

	if (statement_depth == 0 && !(eflags & EXEC_FLAG_EXPLAIN_ONLY) &&
		plan_writes(queryDesc->plannedstmt) && move_to_writer_branch(&same))
	{
		if (!same)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("a statement planned before the session moved to writer branch \"%s\" can't run there: its definitions may differ",
							branch_ops()->current()),
					 errhint("Check the writer branch out with dolt_checkout() and prepare the statement again.")));
		relock(queryDesc->plannedstmt->rtable);
		/*
		 * (the statement's snapshot is its portal's own copy, registered by
		 * it: it sees the copies at the command they were made in)
		 */
		queryDesc->snapshot->curcid = GetCurrentCommandId(false);
	}
	if (prev_executor_start)
		prev_executor_start(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}

static void
writer_executor_run(QueryDesc *queryDesc, ScanDirection direction, uint64 count)
{
	statement_depth++;
	PG_TRY();
	{
		if (prev_executor_run)
			prev_executor_run(queryDesc, direction, count);
		else
			standard_ExecutorRun(queryDesc, direction, count);
	}
	PG_FINALLY();
	{
		statement_depth--;
	}
	PG_END_TRY();
}

static void
writer_executor_finish(QueryDesc *queryDesc)
{
	statement_depth++;
	PG_TRY();
	{
		if (prev_executor_finish)
			prev_executor_finish(queryDesc);
		else
			standard_ExecutorFinish(queryDesc);
	}
	PG_FINALLY();
	{
		statement_depth--;
	}
	PG_END_TRY();
}

static void
writer_process_utility(PlannedStmt *pstmt, const char *queryString,
					   bool readOnlyTree, ProcessUtilityContext context,
					   ParamListInfo params, QueryEnvironment *queryEnv,
					   DestReceiver *dest, QueryCompletion *qc)
{
	LogStmtLevel level = GetCommandLogLevel(pstmt->utilityStmt);
	bool		same;

	if (statement_depth == 0 && context == PROCESS_UTILITY_TOPLEVEL &&
		(level == LOGSTMT_DDL || level == LOGSTMT_MOD) &&
		utility_may_write_branch(pstmt->utilityStmt))
		(void) move_to_writer_branch(&same);
	statement_depth++;
	PG_TRY();
	{
		if (prev_process_utility)
			prev_process_utility(pstmt, queryString, readOnlyTree, context,
								 params, queryEnv, dest, qc);
		else
			standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
									params, queryEnv, dest, qc);
	}
	PG_FINALLY();
	{
		statement_depth--;
	}
	PG_END_TRY();
}

/*
 * Could a utility statement write a branch's objects?  Not one of the whole
 * cluster's (roles, databases, tablespaces, settings), nor a temporary
 * table, nor CREATE INDEX CONCURRENTLY (which commits as it goes): those
 * don't move the session.  (One that does write a protected branch after
 * all is refused there.)
 */
static bool
utility_may_write_branch(Node *stmt)
{
	switch (nodeTag(stmt))
	{
		case T_CreateRoleStmt:
		case T_AlterRoleStmt:
		case T_AlterRoleSetStmt:
		case T_DropRoleStmt:
		case T_GrantRoleStmt:
		case T_ReassignOwnedStmt:
		case T_CreatedbStmt:
		case T_AlterDatabaseStmt:
		case T_AlterDatabaseSetStmt:
		case T_AlterDatabaseRefreshCollStmt:
		case T_DropdbStmt:
		case T_CreateTableSpaceStmt:
		case T_DropTableSpaceStmt:
		case T_AlterTableSpaceOptionsStmt:
		case T_AlterSystemStmt:
		case T_CreateSubscriptionStmt:
		case T_AlterSubscriptionStmt:
		case T_DropSubscriptionStmt:
		case T_VariableSetStmt:
			return false;
		case T_CreateStmt:
			return ((CreateStmt *) stmt)->relation->relpersistence != RELPERSISTENCE_TEMP;
		case T_CreateTableAsStmt:
			return ((CreateTableAsStmt *) stmt)->into->rel->relpersistence != RELPERSISTENCE_TEMP;
		case T_IndexStmt:
			return !((IndexStmt *) stmt)->concurrent;
		default:
			return true;
	}
}

/* (a move is the session's: one in a prepared transaction couldn't be undone) */
static void
writer_xact_callback(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_PRE_PREPARE && moved_lxid == MyProc->vxid.lxid)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot prepare a transaction that moved the session to its writer branch")));
}

/* (a move undone with its subtransaction is no move) */
static void
writer_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
						SubTransactionId parentSubid, void *arg)
{
	if (settled_lxid == MyProc->vxid.lxid && settled_subid == mySubid)
	{
		if (event == SUBXACT_EVENT_ABORT_SUB)
			settle_undone = true;
		else if (event == SUBXACT_EVENT_COMMIT_SUB)
			settled_subid = parentSubid;
	}
	if (moved_lxid != MyProc->vxid.lxid || moved_subid != mySubid)
		return;
	if (event == SUBXACT_EVENT_ABORT_SUB)
		moved_lxid = InvalidLocalTransactionId;
	else if (event == SUBXACT_EVENT_COMMIT_SUB)
		moved_subid = parentSubid;
}

static void
writer_branch_hooks(void)
{
	prev_post_parse_analyze = post_parse_analyze_hook;
	post_parse_analyze_hook = writer_post_parse_analyze;
	RegisterSubXactCallback(writer_subxact_callback, NULL);
	prev_executor_start = ExecutorStart_hook;
	ExecutorStart_hook = writer_executor_start;
	prev_executor_run = ExecutorRun_hook;
	ExecutorRun_hook = writer_executor_run;
	prev_executor_finish = ExecutorFinish_hook;
	ExecutorFinish_hook = writer_executor_finish;
	prev_process_utility = ProcessUtility_hook;
	ProcessUtility_hook = writer_process_utility;
	RegisterXactCallback(writer_xact_callback, NULL);
}

static bool writer_may_integrate(const char *mine, const char *target,
								 const ProllyHash *head, List **needs);
static void check_integrated_rows(const char *target, List *needs);

/*
 * dolt_integrate(VARIADIC args text[]) returns text[]:
 *	  {commit id, fast_forward}
 *
 * [target] (default main); -m/--message message (required).  Run on a
 * branch of the session's own: in one transaction, commits it if it has
 * changes, merges it into target (fast-forward, or a merge commit), and
 * fast-forwards it to target's new head.  Target may be protected: the
 * merge commits as it changes it.  A merge that has conflicts is an
 * error, and the whole transaction is undone, the branch's commit too:
 * merge target into the branch to resolve them there, and integrate
 * again.
 */
Datum
dolt_integrate(PG_FUNCTION_ARGS)
{
	char	  **args;
	int			nargs = text_args(fcinfo, &args);
	char	   *target = NULL;
	char	   *message = NULL;
	bool		options = true;	/* until "--" */
	char	   *mine;
	ProllyHash	before = {0};
	ProllyHash	id = {0};
	ProllyHash	ignored;
	ProllyHash	mine_head = {0};
	int			returning;
	bool		had_head;
	bool		committed;
	int			conflicts;
	bool		rows_only = false;
	List	   *needs = NIL;
	const char *result[2];

	for (int i = 0; i < nargs; i++)
	{
		char	   *a = args[i];

		if (options && strcmp(a, "--") == 0)
			options = false;	/* names after it, even ones like flags */
		else if (options && a[0] == '-')
		{
			if (is_option(a, "m") || is_option(a, "message"))
				message = option_value(args, nargs, &i, "dolt_integrate");
			else
				unsupported("dolt_integrate", a);
		}
		else if (target == NULL)
			target = a;
		else
			unexpected("dolt_integrate", a);
	}
	if (message == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dolt_integrate needs a message"),
				 errhint("Pass -m and a message.")));
	given_message("dolt_integrate", message);
	mine = pstrdup(branch_ops()->current());
	if (target == NULL)
	{
		char	   *writer;
		Oid			role;

		/* (a writer branch's is its base) */
		if (!writer_branch_name(mine) ||
			!PROLLY_CALL(writers(), writer_branch, mine, &writer, &target, &role))
			target = "main";
	}
	if (strcmp(mine, target) == 0 || detached_branch(mine))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("dolt_integrate runs on a branch of your own, not \"%s\"", mine),
				 errhint("Check out a branch with dolt_checkout('-b', ...) first.")));

	PreventCommandIfReadOnly("dolt_integrate()");
	/* (it commits on the branch, and merges into target: checked below) */
	prolly_check_head_move(mine);
	/* (failing, the session stays on its branch, as everything else does) */
	returning = branch_return_on_abort();

	/* the branch's changes, committed */
	note_integration();
	BRANCH_WRITES_HUSHED(committed = PROLLY_CALL(refs(), commit, message,
												 false, &ignored));
	if (committed)
		settled();
	(void) PROLLY_CALL(refs(), resolve, "HEAD", &mine_head);
	if (!prolly_may_move_head(target))
	{
		needs = NIL;
		if (!writer_may_integrate(mine, target, &mine_head, &needs))
			prolly_check_head_move(target);
		rows_only = true;
	}

	/* merged into target, as it commits */
	checkout_branch(target);
	if (rows_only)
		check_integrated_rows(target, needs);
	note_integration();
	had_head = PROLLY_CALL(refs(), resolve, "HEAD", &before);
	/* (the tables it makes are the relations mine's head's record names) */
	VERSION_MOVE(&mine_head, mine_head.len > 0 ? 1 : 0,
				 conflicts = PROLLY_CALL(diff_merge(), merge, mine, message, 0, &id));
	if (conflicts > 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("branch \"%s\" can't be integrated into \"%s\": the merge has conflicts or constraint violations",
						mine, target),
				 errdetail("Nothing was committed or merged."),
				 errhint("Merge \"%s\" into \"%s\" with dolt_merge('%s'), resolve them there, and integrate again.",
						 target, mine, target)));
	settled();

	/* and the branch, fast-forwarded to target's new head */
	checkout_branch(mine);
	note_integration();
	VERSION_MOVE(&id, id.len > 0 ? 1 : 0,
				 (void) PROLLY_CALL(diff_merge(), merge, target, NULL, 0, &ignored));
	settled();

	branch_return_done(returning);
	result[0] = hash_hex(&id);
	result[1] = had_head && same_hash(&id, &mine_head) &&
		!same_hash(&before, &id) ? "1" : "0";
	PG_RETURN_DATUM(text_array(2, result));
}

/*
 * dolt_reset(VARIADIC args text[]) returns text[]: {status}
 *
 * --hard [ref]: the session branch's head commit and working set become
 * ref's (HEAD: drops the changes since the last commit).
 * --soft [ref], or [ref] alone: ref's commit becomes the head, the working
 * set as it is (with no staging area, Dolt's soft and mixed resets are
 * one): what differs from it shows as changes, which a commit takes.
 */
Datum
dolt_reset(PG_FUNCTION_ARGS)
{
	char	  **args;
	int			nargs = text_args(fcinfo, &args);
	bool		options = true;	/* until "--" */
	bool		hard = false;
	bool		soft = false;
	char	   *ref = NULL;
	const char *result[1] = {"0"};

	for (int i = 0; i < nargs; i++)
	{
		char	   *a = args[i];

		if (options && strcmp(a, "--") == 0)
			options = false;	/* names after it, even ones like flags */
		else if (!options || a[0] != '-')
		{
			if (ref == NULL)
				ref = a;
			else
				unexpected("dolt_reset", a);
		}
		else if (is_option(a, "hard"))
			hard = true;
		else if (is_option(a, "soft"))
			soft = true;
		else
			unsupported("dolt_reset", a);
	}
	if (hard && soft)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dolt_reset can't be --hard and --soft at once")));
	PreventCommandIfReadOnly("dolt_reset()");
	/* (a hard reset to the head drops the branch's changes: checked too) */
	prolly_check_head_move(branch_ops()->current());
	if (!hard)
	{
		ProllyMergeState ms;
		ProllyHash	target;

		if (PROLLY_CALL(refs(), merge_state, &ms))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("a merge is in progress on the current branch"),
					 errhint("Resolve its conflicts and commit, or reset it with --hard.")));
		if (!PROLLY_CALL(refs(), resolve, ref ? ref : "HEAD", &target))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("prolly ref \"%s\" does not exist", ref ? ref : "HEAD"),
					 errhint("A soft reset moves the head to a commit; there is no staging area to reset a table in.")));
	}
	/*
	 * What the target needs that the head doesn't.  Even a reset to HEAD
	 * must check ordering dependencies: an extension may have changed since
	 * its trees were built.  A soft reset reads no rows.
	 */
	if (hard)
	{
		ProllyHash	head;
		ProllyHash	target;
		ProllyCommitInfo info;
		ProllyCommitInfo before;
		bool		has_head = PROLLY_CALL(refs(), resolve, "HEAD", &head);

		if (PROLLY_CALL(refs(), resolve, ref ? ref : "HEAD", &target) &&
			PROLLY_CALL(refs(), commit_get, &target, &info))
			prolly_check_commit_dependencies(&info,
											 has_head &&
											 PROLLY_CALL(refs(), commit_get, &head, &before) ?
											 &before : NULL, "reset", false);
	}
	/*
	 * (a protected branch moved to another commit loses what it holds: the
	 * database owner's to do, as a push to it is)
	 */
	if (ref)
	{
		ProllyHash	head;
		ProllyHash	target;

		if (PROLLY_CALL(refs(), resolve, "HEAD", &head) &&
			PROLLY_CALL(refs(), resolve, ref, &target) &&
			!same_hash(&head, &target) &&
			PROLLY_CALL(writers(), protected, branch_ops()->current()) &&
			!object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId()))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("branch \"%s\" is protected", branch_ops()->current()),
					 errhint("Only the database's owner resets a protected branch to another commit.")));
	}
	/* (soft: the head moved, nothing of the working set) */
	if (!hard)
	{
		ProllyHash	target;
		ProllyNodeRef working;
		ProllyNodeRef target_root;
		const char *name = branch_ops()->current();

		(void) PROLLY_CALL(refs(), resolve, ref ? ref : "HEAD", &target);

		/*
		 * (a protected branch is left as its head has it: one whose working
		 * set the new head doesn't hold would have changes no commit does)
		 */
		if (PROLLY_CALL(writers(), protected, name) &&
			PROLLY_CALL(refs(), working_set, NULL, &working) &&
			(!PROLLY_CALL(refs(), commit_working_set, &target, &target_root) ||
			 !same_hash(&working.hash, &target_root.hash)))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("a soft reset of protected branch \"%s\" would leave it uncommitted", name),
					 errdetail("Its working set isn't the commit's it would be reset to."),
					 errhint("Reset it with --hard, or reset a branch of your own.")));
		note_integration();
		PROLLY_CALL(refs(), set_head, &target);
		settled();
		PG_RETURN_DATUM(text_array(1, result));
	}
	/* (the database's owner discards whoever's changes they are) */
	if (PROLLY_CALL(writers(), mode) != PROLLY_WRITERS_SHARED &&
		object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId()))
		PROLLY_CALL(writers(), take, MyBranch);
	note_integration();
	{
		ProllyHash	target = {0};

		ProllyHash	head = {0};

		(void) PROLLY_CALL(refs(), resolve, ref ? ref : "HEAD", &target);

		/*
		 * (to the head: discarding the branch's own work, a table made
		 * again since its commit is the head's relation again, as a
		 * stash's push makes it)
		 */
		if (target.len > 0 && PROLLY_CALL(refs(), resolve, "HEAD", &head) &&
			same_hash(&head, &target))
			BRANCH_WRITES_HUSHED(prolly_drop_mismatched_tables(&head,
															   "reset the branch",
															   true));
		VERSION_MOVE(&target, target.len > 0 ? 1 : 0,
					   PROLLY_CALL(refs(), reset, ref ? ref : "HEAD"));
	}
	settled();
	PG_RETURN_DATUM(text_array(1, result));
}

/* ------------------------------------------------------------------------
 * Stashes
 * ------------------------------------------------------------------------
 */

static int
stash_index(const char *arg)
{
	const char *p = arg;
	char	   *end;
	long		n;

	if (arg == NULL)
		return 0;
	if (strncmp(p, "stash@{", 7) == 0)
		p += 7;
	n = strtol(p, &end, 10);
	if (end == p || n < 0 || n > INT_MAX ||
		(p != arg && strcmp(end, "}") != 0) || (p == arg && *end != '\0'))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" is not a stash", arg),
				 errhint("Name one as stash@{n}, or by n.")));
	return (int) n;
}

static void
no_stashes(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("No stash entries found.")));
}

/*
 * May the session's role drop name's nth stash: the role that pushed it,
 * or one that writes the shared refs (prolly_writes_refs())?  A stash holds
 * changes set aside from a branch every session on it shares; dropping it
 * can't be undone.
 */
static bool
may_drop_stash(const char *name, int n)
{
	Oid			maker;

	return prolly_writes_refs() ||
		(PROLLY_HAS(refs(), stash_maker) &&
		 PROLLY_CALL(refs(), stash_maker, name, n, &maker) &&
		 has_privs_of_role(GetUserId(), maker));
}

static void
check_drop_stash(const char *name, int n)
{
	if (!may_drop_stash(name, n))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to drop stash@{%d} of \"%s\"", n, name),
				 errdetail("Only the role that pushed it, the database's owner, and roles with the database's CREATE privilege or the privileges of the \"pg_prolly_write\" role may.")));
}

/*
 * dolt_stash(VARIADIC args text[]) returns text[]: {status}
 *
 * As Dolt's, but for the working set alone (there is no staging area, so
 * every change is one a stash takes, new tables too: -u and -a are
 * accepted and change nothing):
 *	push name: the working set set aside as name's newest stash (a commit
 *		of it whose parent is the head), and the branch reset to its head;
 *	pop name [stash@{n}]: name's newest (or nth) stash applied to the
 *		working set and dropped;
 *	apply name [stash@{n}]: applied, and kept;
 *	drop name [stash@{n}]: dropped;
 *	clear name: every one of name's dropped.
 * A stash is applied three-way, its changes from the head it was pushed
 * on, as a cherry-pick without a commit: refused where it conflicts with
 * the branch or with changes not committed.
 */
Datum
dolt_stash(PG_FUNCTION_ARGS)
{
	char	  **args;
	int			nargs = text_args(fcinfo, &args);
	const char *cmd = NULL;
	const char *name = NULL;
	const char *which = NULL;
	const char *result[1] = {"0"};

	for (int i = 0; i < nargs; i++)
	{
		char	   *a = args[i];

		if (a[0] == '-')
		{
			if (is_option(a, "include-untracked") || strcmp(a, "-u") == 0 ||
				is_option(a, "all") || strcmp(a, "-a") == 0)
				continue;		/* (every change is staged) */
			unsupported("dolt_stash", a);
		}
		else if (cmd == NULL)
			cmd = a;
		else if (name == NULL)
			name = a;
		else if (which == NULL)
			which = a;
		else
			unexpected("dolt_stash", a);
	}
	if (cmd == NULL || name == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dolt_stash needs a subcommand and a stash name"),
				 errhint("As dolt_stash('push', 'name').")));
	if (!PROLLY_HAS(refs(), stash_push) || !PROLLY_HAS(diff_merge(), apply))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("the database's version control can't stash")));
	PreventCommandIfReadOnly("dolt_stash()");

	if (strcmp(cmd, "push") == 0)
	{
		ProllyNodeRef working;
		ProllyNodeRef head_root;
		ProllyHash	head;
		ProllyHash	stash;
		ProllyMergeState ms;

		if (which)
			unexpected("dolt_stash", which);
		if (PROLLY_CALL(refs(), merge_state, &ms))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("a merge is in progress on the current branch"),
					 errhint("Resolve its conflicts and commit, or abort it.")));
		if (!PROLLY_CALL(refs(), working_set, NULL, &working) ||
			!PROLLY_CALL(refs(), resolve, "HEAD", &head) ||
			!PROLLY_CALL(refs(), commit_working_set, &head, &head_root) ||
			same_hash(&working.hash, &head_root.hash))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("No local changes to save")));
		PROLLY_CALL(refs(), stash_push, name, &working, &stash);
		/* (as dolt_reset('--hard') does) */
		if (PROLLY_CALL(writers(), mode) != PROLLY_WRITERS_SHARED &&
			object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId()))
			PROLLY_CALL(writers(), take, MyBranch);
		note_integration();
		{
			ProllyHash	at = {0};

			(void) PROLLY_CALL(refs(), resolve, "HEAD", &at);
			/* (a table made again since: the head's relation again) */
			if (at.len > 0)
				BRANCH_WRITES_HUSHED(prolly_drop_mismatched_tables(&at,
																   "stash the changes",
																   true));
			VERSION_MOVE(&at, at.len > 0 ? 1 : 0,
						   PROLLY_CALL(refs(), reset, "HEAD"));
		}
		settled();
	}
	else if (strcmp(cmd, "pop") == 0 || strcmp(cmd, "apply") == 0)
	{
		int			n = stash_index(which);
		ProllyHash	stash;

		/*
		 * (chosen with the refs locked: no other session's push or drop
		 * moves it before it's applied and, popped, dropped)
		 */
		if (!PROLLY_CALL(refs(), stash_select, name, n, &stash))
			no_stashes();
		/* (a pop drops it) */
		if (strcmp(cmd, "pop") == 0)
			check_drop_stash(name, n);
		refuse_violations("stash");

		/*
		 * (a write, as any: it makes no commit, so a protected branch
		 * refuses it, and the branch is left with its changes)
		 */
		branch_note_write(MyBranch);
		VERSION_MOVE(&stash, 1,
					 PROLLY_CALL(diff_merge(), apply, hash_hex(&stash),
								 "dolt_stash"));
		if (strcmp(cmd, "pop") == 0)
			(void) PROLLY_CALL(refs(), stash_drop, name, n);
	}
	else if (strcmp(cmd, "drop") == 0)
	{
		ProllyHash	stash;
		int			n = stash_index(which);

		if (!PROLLY_CALL(refs(), stash_select, name, n, &stash))
			no_stashes();
		check_drop_stash(name, n);
		if (!PROLLY_CALL(refs(), stash_drop, name, n))
			no_stashes();
	}
	else if (strcmp(cmd, "clear") == 0)
	{
		if (which)
			unexpected("dolt_stash", which);
		/* (a role that may drop only its own stashes clears only those) */
		if (prolly_writes_refs())
			PROLLY_CALL(refs(), stash_clear, name);
		else
		{
			ProllyHash	stash;
			int			n = 0;

			while (PROLLY_CALL(refs(), stash_select, name, n, &stash))
			{
				if (may_drop_stash(name, n))
					(void) PROLLY_CALL(refs(), stash_drop, name, n);
				else
					n++;
			}
		}
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dolt_stash has no subcommand \"%s\"", cmd),
				 errhint("It takes push, pop, apply, drop and clear.")));
	PG_RETURN_DATUM(text_array(1, result));
}

/* dolt_stashes()'s rows */
static void
put_stash(const char *name, int n, const char *branch, const ProllyHash *stash,
		  void *arg)
{
	ReturnSetInfo *rsinfo = arg;
	ProllyCommitInfo info;
	ProllyCommitInfo head;
	Datum		values[5];
	bool		nulls[5] = {0};

	values[0] = CStringGetTextDatum(name);
	values[1] = CStringGetTextDatum(psprintf("stash@{%d}", n));
	values[2] = CStringGetTextDatum(branch);
	if (PROLLY_CALL(refs(), commit_get, stash, &info) && info.nparents > 0 &&
		PROLLY_CALL(refs(), commit_get, &info.parents[0], &head))
	{
		values[3] = CStringGetTextDatum(hash_hex(&info.parents[0]));
		values[4] = CStringGetTextDatum(head.message);
	}
	else
		nulls[3] = nulls[4] = true;
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

/*
 * dolt_stashes(OUT name text, OUT stash_id text, OUT branch text, OUT hash
 *				text, OUT commit_message text) returns setof record
 *
 * Every stash, by list name, then newest first, as Dolt's dolt_stashes:
 * its place (stash@{n}), the branch it was pushed from, and the head it
 * was pushed on, with that commit's message.
 */
Datum
dolt_stashes(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);
	if (PROLLY_HAS(refs(), stashes))
		PROLLY_CALL(refs(), stashes, put_stash, rsinfo);
	return (Datum) 0;
}

/* ----------------------------------------------------------------
 * Tags
 * ----------------------------------------------------------------
 */

/* The refs, if they keep tags: else an error. */
static const ProllyRefsRoutine *
tag_refs(void)
{
	const ProllyRefsRoutine *r = refs();

	if (!PROLLY_HAS(r, tags))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("refs implementation \"%s\" keeps no tags", r->info.name)));
	return r;
}

/* Is name a tag's, by its name or its full ref name (refs/tags/x)? */
static bool
names_tag(const char *name)
{
	const ProllyRefsRoutine *r = refs();

	if (strncmp(name, "refs/tags/", 10) == 0 && name[10] != '\0' &&
		PROLLY_HAS(r, tag_lookup) &&
		!PROLLY_CALL(r, tag_lookup, name, &(ProllyHash) {0}))
		name += 10;
	return PROLLY_HAS(r, tag_lookup) &&
		PROLLY_CALL(r, tag_lookup, name, &(ProllyHash) {0});
}

/*
 * dolt_tag(VARIADIC args text[]) returns text[]: {status}
 *
 * name [ref] (HEAD if none), with -m/--message message and --author
 * "Name <email>" (the session's author if none): a tag of ref's commit,
 * called name, as Dolt's; -d/--delete name...: those tags deleted.  A tag
 * that exists already isn't replaced.  The dolt_tags view lists them.
 */
Datum
dolt_tag(PG_FUNCTION_ARGS)
{
	char	  **args;
	int			nargs = text_args(fcinfo, &args);
	bool		options = true;	/* until "--" */
	bool		delete = false;
	bool		list = false;
	bool		was_read_only;
	char	   *message = NULL;
	char	   *author = NULL;
	char	  **names = palloc_array(char *, Max(nargs, 1));
	int			nnames = 0;
	const char *result[1] = {"0"};

	for (int i = 0; i < nargs; i++)
	{
		char	   *a = args[i];

		if (options && strcmp(a, "--") == 0)
			options = false;	/* names after it, even ones like flags */
		else if (!options || a[0] != '-')
			names[nnames++] = a;
		else if (is_option(a, "d") || is_option(a, "delete"))
			delete = true;
		else if (is_option(a, "m") || is_option(a, "message"))
			message = option_value(args, nargs, &i, "dolt_tag");
		else if (is_option(a, "author"))
			author = option_value(args, nargs, &i, "dolt_tag");
		else if (is_option(a, "v") || is_option(a, "verbose"))
			list = true;
		else
			unsupported("dolt_tag", a);
	}
	/* (a listing is the view's, as Dolt's is the dolt_tags table's) */
	if (nnames == 0 || list)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dolt_tag needs a tag's name"),
				 errhint("The dolt_tags view lists the tags.")));
	/*
	 * (a tag changes only the refs: made, as a branch is, in a session on a
	 * commit too, whose transactions are read-only only for that branch)
	 */
	was_read_only = XactReadOnly;
	if (XactReadOnlyFromBranch)
		XactReadOnly = false;
	PG_TRY();
	{
		dolt_tag_change(names, nnames, delete, message, author);
	}
	PG_FINALLY();
	{
		XactReadOnly = was_read_only;
	}
	PG_END_TRY();
	PG_RETURN_DATUM(text_array(1, result));
}

/* dolt_tag()'s change: names made a tag of (or deleted, if delete). */
static void
dolt_tag_change(char **names, int nnames, bool delete, const char *message,
				const char *author)
{
	PreventCommandIfReadOnly("dolt_tag()");
	if (delete)
	{
		if (message != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("dolt_tag: -d and -m cannot be used together")));
		/* (a tag is its maker's to delete, or a role's that writes the refs) */
		for (int i = 0; i < nnames; i++)
		{
			Oid			creator;

			if (!prolly_writes_refs() &&
				!(PROLLY_HAS(tag_refs(), tag_creator) &&
				  PROLLY_CALL(tag_refs(), tag_creator, names[i], &creator) &&
				  has_privs_of_role(GetUserId(), creator)))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to delete tag \"%s\"", names[i]),
						 errdetail("Only the tag's maker, the database's owner, and roles with the database's CREATE privilege or the privileges of the \"pg_prolly_write\" role may.")));
		}
		for (int i = 0; i < nnames; i++)
			if (!PROLLY_CALL(tag_refs(), tag_set, names[i], NULL, NULL))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("tag \"%s\" does not exist", names[i])));
	}
	else
	{
		const ProllyRefsRoutine *r = tag_refs();
		const char *ref = nnames > 1 ? names[1] : "HEAD";
		ProllyHash	commit;
		ProllyHash	tag;

		if (nnames > 2)
			unexpected("dolt_tag", names[2]);
		given_author("dolt_tag", author);
		check_writes_refs("make a tag");
		if (PROLLY_CALL(r, tag_lookup, names[0], &tag))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("tag \"%s\" already exists", names[0])));
		if (!PROLLY_CALL(r, resolve, ref, &commit))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("branch, tag or commit \"%s\" does not exist", ref)));
		PROLLY_CALL(r, tag_write, &commit, author ? author : prolly_commit_author(),
					message ? message : "", &tag);
		if (!PROLLY_CALL(r, tag_set, names[0], &(ProllyHash) {0}, &tag))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("tag \"%s\" already exists", names[0])));
		/* (a tag made here is its role's; one fetched or pushed, no one's) */
		if (PROLLY_HAS(r, tag_set_creator))
			PROLLY_CALL(r, tag_set_creator, names[0], GetUserId());
	}
}

/* dolt_tags()'s rows */
static void
put_tag(const char *name, const ProllyHash *tag, const ProllyHash *commit,
		void *arg)
{
	ReturnSetInfo *rsinfo = arg;
	ProllyTagInfo info;
	Datum		values[6];
	bool		nulls[6] = {0};

	values[0] = CStringGetTextDatum(name);
	values[1] = CStringGetTextDatum(hash_hex(commit));
	if (PROLLY_CALL(refs(), tag_get, tag, &info))
	{
		char	   *tagger;
		char	   *email;

		split_author(info.tagger, &tagger, &email);
		values[2] = CStringGetTextDatum(tagger);
		values[3] = CStringGetTextDatum(email);
		values[4] = TimestampTzGetDatum(info.tagged_at);
		values[5] = CStringGetTextDatum(info.message);
	}
	else
		nulls[2] = nulls[3] = nulls[4] = nulls[5] = true;
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

/*
 * dolt_tags(OUT tag_name text, OUT tag_hash text, OUT tagger text,
 *			 OUT email text, OUT date timestamptz, OUT message text)
 *			 returns setof record
 *
 * Every tag, by name, as Dolt's dolt_tags: the commit it names (tag_hash,
 * as Dolt's), who made it, when, and its message.
 */
Datum
dolt_tags(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);
	if (PROLLY_HAS(refs(), tags))
		PROLLY_CALL(refs(), tags, put_tag, rsinfo);
	return (Datum) 0;
}

/*
 * The tables with conflicts and the tables with constraint violations a
 * pick or revert in progress left, as Dolt counts them (tables, not rows):
 * the branch had no violations before it (refuse_violations()); and the
 * tables in table identity conflicts (identity, if wanted).
 */
static void
tables_left(int *conflicted, int *violated, int *identity)
{
	ProllyMergeState ms;
	List	   *relids = NIL;
	ListCell   *lc;
	bool		merging = PROLLY_CALL(refs(), merge_state, &ms);

	*conflicted = merging ? ms.ntables : 0;
	if (identity)
		*identity = merging ? ms.nidentity : 0;
	foreach(lc, PROLLY_CALL(refs(), violations))
		relids = list_append_unique_oid(relids, ((ProllyViolation *) lfirst(lc))->relid);
	*violated = list_length(relids);
}

/*
 * dolt_cherry_pick(VARIADIC args text[]) returns text[]:
 *	  {commit id, data conflicts, schema conflicts, constraint violations}
 *
 * [--allow-empty] commit: its changes, committed again on the current
 * branch, with its author.  Rows changed differently on both sides leave
 * the pick in progress, as a merge's conflicts do: resolved, dolt_commit
 * (or [--allow-empty] --continue, with the picked commit's message and
 * author) makes its commit, with one parent; --abort ends it.  The counts are of tables,
 * as Dolt's.
 */
Datum
dolt_cherry_pick(PG_FUNCTION_ARGS)
{
	char	  **args;
	int			nargs = text_args(fcinfo, &args);
	bool		options = true;	/* until "--" */
	const char *ref = NULL;
	bool		allow_empty = false;
	ProllyHash	id;
	const char *result[4] = {NULL, "0", "0", "0"};
	bool		abort_pick = false;
	bool		continue_pick = false;
	bool		committed;
	int			conflicts;

	for (int i = 0; i < nargs; i++)
	{
		if (options && strcmp(args[i], "--") == 0)
			options = false;	/* names after it, even ones like flags */
		else if (!options || args[i][0] != '-')
		{
			if (ref == NULL)
				ref = args[i];
			else
				unexpected("dolt_cherry_pick", args[i]);
		}
		else if (is_option(args[i], "allow-empty"))
			allow_empty = true;
		else if (is_option(args[i], "abort"))
			abort_pick = true;
		else if (is_option(args[i], "continue"))
			continue_pick = true;
		else
			unsupported("dolt_cherry_pick", args[i]);
	}
	if (abort_pick && continue_pick)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dolt_cherry_pick can't --abort and --continue at once")));
	if (abort_pick || continue_pick)
	{
		ProllyMergeState ms;
		ProllyCommitInfo picked;

		if (ref != NULL)
			unexpected("dolt_cherry_pick", ref);
		if (!PROLLY_CALL(refs(), merge_state, &ms) || !ms.pick || ms.revert)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("there is no cherry-pick to %s",
							abort_pick ? "abort" : "continue")));
		PreventCommandIfReadOnly("dolt_cherry_pick()");
		prolly_check_head_move(branch_ops()->current());
		if (abort_pick)
		{
			abort_in_progress("cherry-pick");
			result[0] = "";
			PG_RETURN_DATUM(text_array(4, result));
		}
		/*
		 * By the picked commit's author, as a pick that doesn't stop is
		 * (Dolt's ContinueCherryPick): the merge's theirs is that commit.
		 * A pick resolved to nothing: as a pick of nothing is.
		 */
		if (!PROLLY_CALL(refs(), commit_get, &ms.theirs, &picked))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("the cherry-pick's commit %s is missing",
							hash_hex(&ms.theirs))));
		note_integration();
		author_override = picked.author;
		PG_TRY();
		{
			BRANCH_WRITES_HUSHED(committed = PROLLY_CALL(refs(), commit,
														 ms.message,
														 allow_empty, &id));
		}
		PG_FINALLY();
		{
			author_override = NULL;
		}
		PG_END_TRY();
		if (!committed)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("dolt_cherry_pick changes nothing on the branch"),
					 errhint("Pass --allow-empty to commit it anyway.")));
		settled();
		result[0] = hash_hex(&id);
		PG_RETURN_DATUM(text_array(4, result));
	}
	if (ref == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dolt_cherry_pick needs a commit")));
	PreventCommandIfReadOnly("dolt_cherry_pick()");
	prolly_check_head_move(branch_ops()->current());
	refuse_violations("cherry-pick");
	note_integration();
	{
		ProllyHash	picked = {0};

		(void) PROLLY_CALL(refs(), resolve, ref, &picked);
		VERSION_MOVE(&picked, picked.len > 0 ? 1 : 0,
					 conflicts = PROLLY_CALL(diff_merge(), cherry_pick, &ref, 1,
											 allow_empty, &id));
	}
	must_commit(conflicts, "a cherry-pick");
	if (conflicts > 0)
	{
		int			nviolations = violations_kept();
		int			ctables;
		int			vtables;
		int			itables;

		stopped_outside_block("cherry-pick", "Cherry-pick", "sides",
							  conflicts - nviolations, nviolations);
		tables_left(&ctables, &vtables, &itables);
		result[0] = "";
		result[1] = psprintf("%d", ctables);
		result[2] = psprintf("%d", itables);
		result[3] = psprintf("%d", vtables);
		PG_RETURN_DATUM(text_array(4, result));
	}
	settled();
	result[0] = hash_hex(&id);
	PG_RETURN_DATUM(text_array(4, result));
}

/*
 * Revert commits (as hashes) from a revert series' merge state, the head
 * before the series kept, and fill result: the last commit made, or, if
 * one stopped, the tables it left with conflicts and violations.
 */
static void
revert_series(const char *const *ids, int n, const ProllyHash *series_head,
			  const char **result)
{
	ProllyHash	id;
	int			stopped;

	refuse_violations("revert");
	note_integration();
	{
		ProllyHash *from = palloc0(sizeof(ProllyHash) * Max(n, 1));
		int			nfrom = 0;

		/* (a revert brings back what each commit's parent had) */
		for (int i = 0; i < n; i++)
		{
			ProllyHash	c;
			ProllyCommitInfo info;

			if (PROLLY_CALL(refs(), resolve, ids[i], &c) &&
				PROLLY_CALL(refs(), commit_get, &c, &info) && info.nparents > 0)
				from[nfrom++] = info.parents[0];
		}
		VERSION_MOVE(from, nfrom,
					 stopped = PROLLY_CALL(diff_merge(), revert, ids, n,
										   series_head, &id));
	}
	must_commit(stopped, "a revert");
	if (stopped > 0)
	{
		int			nviolations = violations_kept();
		int			ctables;
		int			vtables;
		int			itables;

		stopped_outside_block("revert", "Revert", "sides",
							  stopped - nviolations, nviolations);
		tables_left(&ctables, &vtables, &itables);
		result[0] = "";
		result[1] = psprintf("%d", ctables);
		result[2] = psprintf("%d", itables);
		result[3] = psprintf("%d", vtables);
		return;
	}
	result[0] = id.len > 0 ? hash_hex(&id) : "";
}

/*
 * dolt_revert(VARIADIC args text[]) returns text[]:
 *	  {commit id, data conflicts, schema conflicts, constraint violations}
 *
 * [--author author] commit...: each one's changes undone, in a commit of
 * its own, in the order given (every one resolved first); the id is the
 * last commit's.  One that conflicts stops the series there, in progress
 * as a cherry-pick's: resolved, --continue commits it and goes on with the
 * rest; --abort puts the branch back where it was before the series.  The
 * counts are of tables, as Dolt's.
 */
Datum
dolt_revert(PG_FUNCTION_ARGS)
{
	char	  **args;
	int			nargs = text_args(fcinfo, &args);
	const char *result[4] = {"", "0", "0", "0"};
	const char **refs_given = palloc(sizeof(char *) * Max(nargs, 1));
	int			nrefs = 0;
	bool		options = true;	/* until "--" */
	bool		abort_revert = false;
	bool		continue_revert = false;
	char	   *author = NULL;

	for (int i = 0; i < nargs; i++)
	{
		if (options && strcmp(args[i], "--") == 0)
			options = false;	/* names after it, even ones like flags */
		else if (!options || args[i][0] != '-')
			refs_given[nrefs++] = args[i];
		else if (is_option(args[i], "abort"))
			abort_revert = true;
		else if (is_option(args[i], "continue"))
			continue_revert = true;
		else if (is_option(args[i], "author"))
			author = option_value(args, nargs, &i, "dolt_revert");
		else
			unsupported("dolt_revert", args[i]);
	}
	/* (an abort makes no commit: its --author is ignored) */
	if (!abort_revert)
		given_author("dolt_revert", author);
	if (abort_revert && continue_revert)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dolt_revert can't --abort and --continue at once")));
	PG_TRY();
	{
		author_override = author;
		if (abort_revert || continue_revert)
		{
			ProllyMergeState ms;

			if (nrefs > 0)
				unexpected("dolt_revert", refs_given[0]);
			if (!PROLLY_CALL(refs(), merge_state, &ms) || !ms.revert)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("there is no revert in progress")));
			PreventCommandIfReadOnly("dolt_revert()");
			prolly_check_head_move(branch_ops()->current());
			note_integration();
			if (abort_revert)
			{
				BRANCH_WRITES_HUSHED(prolly_drop_mismatched_tables(&ms.series_head,
																   "abort the revert",
																   true));
				VERSION_MOVE(&ms.series_head, 1,
							   PROLLY_CALL(refs(), reset, hash_hex(&ms.series_head)));
				BRANCH_WRITES_HUSHED(prolly_restore_privileges(ms.identity_saved, false));
				settled();
			}
			else if (ms.ntables > 0 || ms.nidentity > 0 ||
					 PROLLY_CALL(refs(), violations) != NIL)
			{
				/* (Dolt reports what is still to resolve, without error) */
				tables_left(&ms.ntables, &nrefs, &ms.nidentity);
				result[1] = psprintf("%d", ms.ntables);
				result[2] = psprintf("%d", ms.nidentity);
				result[3] = psprintf("%d", nrefs);
			}
			else
			{
				ProllyHash	id;
				const char **pending = palloc(sizeof(char *) * Max(ms.npending, 1));

				BRANCH_WRITES_HUSHED((void) PROLLY_CALL(refs(), commit, ms.message,
														true, &id));
				result[0] = hash_hex(&id);
				for (int i = 0; i < ms.npending; i++)
					pending[i] = hash_hex(&ms.pending[i]);
				if (ms.npending > 0)
					revert_series(pending, ms.npending, &ms.series_head, result);
			}
		}
		else
		{
			if (nrefs == 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("dolt_revert needs a commit")));
			PreventCommandIfReadOnly("dolt_revert()");
			prolly_check_head_move(branch_ops()->current());
			revert_series(refs_given, nrefs, NULL, result);
		}
	}
	PG_FINALLY();
	{
		author_override = NULL;
	}
	PG_END_TRY();
	if (result[0][0] != '\0')
		settled();
	PG_RETURN_DATUM(text_array(4, result));
}

/*
 * dolt_add(VARIADIC args text[]) returns text[]: {status}
 *
 * Tables or -A/--all/.: every change is staged already.
 */
Datum
dolt_add(PG_FUNCTION_ARGS)
{
	char	  **args;
	int			nargs = text_args(fcinfo, &args);
	const char *result[1] = {"0"};
	bool		options = true;	/* until "--" */

	for (int i = 0; i < nargs; i++)
	{
		char	   *a = args[i];

		if (options && strcmp(a, "--") == 0)
			options = false;	/* tables after it, even ones like flags */
		else if (options && a[0] == '-' &&
				 !is_option(a, "A") && !is_option(a, "all"))
			unsupported("dolt_add", a);
	}
	PG_RETURN_DATUM(text_array(1, result));
}

/* dolt_hashof(ref text) returns text: the commit id ref names */
Datum
dolt_hashof(PG_FUNCTION_ARGS)
{
	char	   *ref = text_to_cstring(PG_GETARG_TEXT_PP(0));
	ProllyHash	id;

	if (!PROLLY_CALL(refs(), resolve, ref, &id))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("prolly ref \"%s\" does not exist", ref)));
	PG_RETURN_TEXT_P(cstring_to_text(hash_hex(&id)));
}

/*
 * dolt_merge_base(ref1 text, ref2 text) returns text: the newest common
 * ancestor of the commits they name, as a merge of them finds it
 */
Datum
dolt_merge_base(PG_FUNCTION_ARGS)
{
	const char *names[2];
	ProllyHash	ids[2];
	ProllyHash	base;

	for (int i = 0; i < 2; i++)
	{
		names[i] = text_to_cstring(PG_GETARG_TEXT_PP(i));
		if (!PROLLY_CALL(refs(), resolve, names[i], &ids[i]))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("prolly ref \"%s\" does not exist", names[i])));
	}
	if (!PROLLY_CALL(refs(), merge_base, &ids[0], &ids[1], &base))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("\"%s\" and \"%s\" have no history in common",
						names[0], names[1])));
	PG_RETURN_TEXT_P(cstring_to_text(hash_hex(&base)));
}

/* active_branch() returns text: the session's branch */
Datum
active_branch(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(branch_ops()->current()));
}

/* ------------------------------------------------------------------------
 * System views
 * ------------------------------------------------------------------------
 */

typedef struct LogEntry
{
	ProllyHash	key;			/* hash key: info.id as prolly_hash_key() makes it */
	ProllyCommitInfo info;
	int64		order;			/* its height: 1 + the most of its parents' */
	pairingheap_node node;		/* (in a LogStream's heap until given) */
} LogEntry;

/*
 * A walk of the commits some starts reach, given one at a time in the log's
 * order: highest height first, then newest, then by id, as Dolt's log goes
 * by height and then date.  A
 * commit is always above its parents, so a commit not yet found is below
 * every one found and not yet given (each is a parent's parent of one of
 * them): the next to give is always the greatest of those found, kept in a
 * heap, and each one given is read with its parents, those then found.  So
 * the first n commits cost about n commits read, their parents with them,
 * however long the history.  A listing stops at the history horizon.
 */
struct ProllyLogStream
{
	HTAB	   *seen;			/* of LogEntry, by key */
	HTAB	   *heights;		/* those reckoned of commits without one */
	pairingheap *heap;			/* those found, not given */
	HTAB	   *horizon;		/* the history horizon as it began (NULL:
								 * the window's, asked) */
};
typedef ProllyLogStream LogStream;

static int
log_heap_cmp(const pairingheap_node *a, const pairingheap_node *b, void *arg)
{
	const LogEntry *x = pairingheap_const_container(LogEntry, node, a);
	const LogEntry *y = pairingheap_const_container(LogEntry, node, b);

	/* (the heap gives its greatest first: the log's first) */
	if (x->order != y->order)
		return x->order > y->order ? 1 : -1;
	if (x->info.committed_at != y->info.committed_at)
		return x->info.committed_at > y->info.committed_at ? 1 : -1;
	return -memcmp(x->info.id.bytes, y->info.id.bytes, x->info.id.len);
}

/* A commit of the stream, for prolly_history_height(): read as found. */
static bool
log_stream_lookup(const ProllyHash *id, ProllyCommitInfo *info, void *arg)
{
	LogStream  *ls = arg;
	ProllyHash	k = prolly_hash_key(id);
	LogEntry   *e = hash_search(ls->seen, &k, HASH_FIND, NULL);

	/* (one found already, read: entered before it is read, so asked) */
	if (e != NULL && e->info.id.len > 0)
	{
		*info = e->info;
		return true;
	}
	return PROLLY_CALL(refs(), commit_get, id, info);
}

/* Find commit id: read, with its height, and put in the heap; once. */
static void
log_stream_find(LogStream *ls, const ProllyHash *id)
{
	ProllyHash	key = prolly_hash_key(id);
	LogEntry   *e;
	bool		found;

	e = hash_search(ls->seen, &key, HASH_ENTER, &found);
	if (found)
		return;
	memset(&e->info, 0, sizeof(e->info));
	if (!PROLLY_CALL(refs(), commit_get, id, &e->info))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly commit %s is missing", hash_hex(id))));
	if (e->info.has_height && e->info.height > 0)
		e->order = e->info.height;
	else
	{
		if (ls->heights == NULL)
			ls->heights = prolly_history_heights();
		e->order = prolly_history_height(&e->info.id, ls->heights,
										 log_stream_lookup, ls);
	}
	pairingheap_add(ls->heap, &e->node);
}

/* A stream of the commits the starts (ProllyHash *) reach. */
static LogStream *
log_stream_begin(List *starts)
{
	LogStream  *ls = palloc0(sizeof(LogStream));
	HASHCTL		ctl = {0};

	ctl.keysize = sizeof(ProllyHash);
	ctl.entrysize = sizeof(LogEntry);
	ctl.hcxt = CurrentMemoryContext;
	ls->seen = hash_create("dolt_log commits", 256, &ctl,
						   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	ls->heap = pairingheap_allocate(log_heap_cmp, NULL);
	/* (held: a sever or a rollback meanwhile doesn't move where it stops) */
	ls->horizon = prolly_window_horizon();
	foreach_ptr(ProllyHash, start, starts)
		log_stream_find(ls, start);
	return ls;
}

/* The stream's next commit; NULL once every one is given. */
static LogEntry *
log_stream_next(LogStream *ls)
{
	LogEntry   *e;

	CHECK_FOR_INTERRUPTS();
	if (pairingheap_is_empty(ls->heap))
		return NULL;
	e = pairingheap_container(LogEntry, node, pairingheap_remove_first(ls->heap));
	for (int i = 0; i < e->info.nparents; i++)
	{
		/* (a listing stops at the history horizon) */
		if (prolly_window_held_stops_at(ls->horizon, &e->info.parents[i]))
			continue;
		log_stream_find(ls, &e->info.parents[i]);
	}
	return e;
}

/*
 * Every commit the starts (ProllyHash *) reach (LogEntry *), each once,
 * highest commit order (height) first, then newest: the stream drained.
 */
static List *
log_walk(List *starts)
{
	LogStream  *ls = log_stream_begin(starts);
	List	   *entries = NIL;
	LogEntry   *e;

	while ((e = log_stream_next(ls)) != NULL)
		entries = lappend(entries, e);
	return entries;
}

/*
 * Every commit ref reaches (LogEntry *), as log_walk() orders them; NIL for
 * HEAD before a first commit.
 */
static List *
log_entries(const char *ref)
{
	ProllyHash	start;

	if (!PROLLY_CALL(refs(), resolve, ref, &start))
	{
		if (pg_strcasecmp(ref, "HEAD") == 0)
			return NIL;			/* nothing committed yet */
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("prolly ref \"%s\" does not exist", ref)));
	}
	return log_walk(list_make1(&start));
}

int64
prolly_log_count(const char *ref)
{
	ProllyHash	start;

	if (!PROLLY_CALL(refs(), resolve, ref, &start))
	{
		if (pg_strcasecmp(ref, "HEAD") == 0)
			return 0;			/* nothing committed yet */
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("prolly ref \"%s\" does not exist", ref)));
	}
	/* (the count the refs keep, while the log reaches every commit counted) */
	if (PROLLY_HAS(refs(), side_get) && !prolly_window_severed())
	{
		ProllyKey	k = {(const uint8 *) start.bytes, start.len};
		StringInfoData v;
		bool		kept;

		initStringInfo(&v);
		kept = PROLLY_CALL(refs(), side_get, PROLLY_REFS_SPACE_COMMITS, &k, &v) &&
			v.len == 8;
		if (kept)
		{
			uint64		n = 0;

			for (int i = 0; i < 8; i++)
				n = (n << 8) | (uint8) v.data[i];
			pfree(v.data);
			return (int64) n;
		}
		pfree(v.data);
	}
	return list_length(log_walk(list_make1(&start)));
}

void
prolly_function_scan_check(Oid funcid)
{
	AclResult	aclresult = object_aclcheck(ProcedureRelationId, funcid,
											GetUserId(), ACL_EXECUTE);

	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION, get_func_name(funcid));
	InvokeFunctionExecuteHook(funcid);
}

static void
note_head(const char *name, const ProllyHash *head, void *arg)
{
	List	  **starts = arg;

	/* (not a session's hidden branch at a commit: Dolt's detached HEAD) */
	if (head->len == 0 || detached_branch(name))
		return;
	*starts = lappend(*starts, memcpy(palloc(sizeof(ProllyHash)), head,
									  sizeof(ProllyHash)));
}

/*
 * Every commit any branch reaches (LogEntry *), as Dolt's dolt_commits
 * reads them (#588); NIL before a first commit.
 */
static List *
all_log_entries(void)
{
	List	   *starts = NIL;

	PROLLY_CALL(refs(), branches, note_head, &starts);
	return log_walk(starts);
}

/* ----------------------------------------------------------------
 * Rebases
 * ----------------------------------------------------------------
 */

/*
 * The branch a session rebases on, as Dolt's: a branch of its own, made at
 * the commit replayed onto, which becomes the rebased branch's head once
 * every step is done.  (Git rebases on a detached HEAD; Dolt has none.)
 */

/* numeric(6,2)'s typmod: a rebase order's */
#define REBASE_ORDER_TYPMOD (((6 << 16) | 2) + VARHDRSZ)

static const char *const rebase_actions[] = {
	"pick", "reword", "squash", "fixup", "drop", "edit"
};

/* The refs, if they keep rebases: else an error. */
static const ProllyRefsRoutine *
rebase_refs(void)
{
	const ProllyRefsRoutine *r = refs();

	if (!PROLLY_HAS(r, rebase_state))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("refs implementation \"%s\" can't rebase", r->info.name)));
	return r;
}

/* A rebase order, as numeric(6,2) gives it ("1.00"): an error if it isn't one. */
static char *
rebase_order_text(Datum order)
{
	Datum		fitted = DirectFunctionCall2(numeric, order,
											 Int32GetDatum(REBASE_ORDER_TYPMOD));

	return DatumGetCString(DirectFunctionCall1(numeric_out, fitted));
}

static Datum
rebase_order_datum(const char *order)
{
	return DirectFunctionCall3(numeric_in, CStringGetDatum(order),
							   ObjectIdGetDatum(InvalidOid),
							   Int32GetDatum(REBASE_ORDER_TYPMOD));
}

static int
rebase_order_cmp(const char *a, const char *b)
{
	return DatumGetInt32(DirectFunctionCall2(numeric_cmp, rebase_order_datum(a),
											 rebase_order_datum(b)));
}

static int
rebase_step_cmp(const void *a, const void *b)
{
	return rebase_order_cmp(((const ProllyRebaseStep *) a)->order,
							((const ProllyRebaseStep *) b)->order);
}

static void
check_rebase_action(const char *action)
{
	for (int i = 0; i < lengthof(rebase_actions); i++)
		if (strcmp(action, rebase_actions[i]) == 0)
			return;
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid rebase action \"%s\"", action),
			 errhint("An action is pick, reword, squash, fixup, drop or edit.")));
}

static void
no_rebase(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("no rebase in progress")));
}

/* Does the session branch's working set differ from its head's? */
static bool
working_set_dirty(void)
{
	ProllyNodeRef working;
	ProllyNodeRef head_root;
	ProllyHash	head;

	if (!PROLLY_CALL(refs(), working_set, NULL, &working))
		return false;
	if (!PROLLY_CALL(refs(), resolve, "HEAD", &head))
		return true;
	return !PROLLY_CALL(refs(), commit_working_set, &head, &head_root) ||
		!same_hash(&working.hash, &head_root.hash);
}

/*
 * The plan Dolt's makes: every commit head reaches that onto doesn't, but
 * merge commits, oldest first, each picked, ordered 1, 2, ...
 */
static void
default_rebase_plan(const ProllyHash *head, const ProllyHash *onto,
					ProllyRebaseState *rs)
{
	List	   *theirs = log_walk(list_make1((void *) onto));
	List	   *ours = log_walk(list_make1((void *) head));
	HASHCTL		ctl = {0};
	HTAB	   *reached;
	ListCell   *lc;
	List	   *picked = NIL;
	int			n = 0;

	ctl.keysize = sizeof(ProllyHash);
	ctl.entrysize = sizeof(ProllyHash);
	ctl.hcxt = CurrentMemoryContext;
	reached = hash_create("dolt_rebase upstream", 256, &ctl,
						  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	foreach(lc, theirs)
		(void) hash_search(reached, &((LogEntry *) lfirst(lc))->key, HASH_ENTER, NULL);
	foreach(lc, ours)
	{
		LogEntry   *e = lfirst(lc);

		if (e->info.nparents == 1 &&
			hash_search(reached, &e->key, HASH_FIND, NULL) == NULL)
			picked = lcons(e, picked);	/* (oldest first) */
	}
	rs->nsteps = list_length(picked);
	rs->steps = palloc0(sizeof(ProllyRebaseStep) * Max(rs->nsteps, 1));
	foreach(lc, picked)
	{
		LogEntry   *e = lfirst(lc);
		ProllyRebaseStep *s = &rs->steps[n++];

		s->order = rebase_order_text(DirectFunctionCall1(int4_numeric,
														 Int32GetDatum(n)));
		s->action = "pick";
		s->commit = e->info.id;
		s->message = e->info.message;
	}
}

/*
 * Start rebasing the session's branch onto upstream: refused over
 * uncommitted changes, a merge, pick or revert in progress, or another
 * rebase, as Dolt's.  The session is left on the rebase's own branch.
 */
static void
rebase_start(const char *upstream, bool keep_empty)
{
	const ProllyRefsRoutine *r = rebase_refs();
	char	   *branch = pstrdup(branch_ops()->current());
	char	   *working = psprintf("%s%s", REBASE_BRANCH_PREFIX, branch);
	ProllyRebaseState rs = {0};
	ProllyMergeState ms;

	if (PROLLY_CALL(r, rebase_state, &rs))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("unable to start rebase while another rebase is in progress – abort the current rebase before proceeding")));
	if (PROLLY_CALL(r, merge_state, &ms))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("unable to start rebase while a merge is in progress – abort the current merge before proceeding")));
	/* (a branch with no commits has none to rebase: said below) */
	if (PROLLY_CALL(r, resolve, "HEAD", &rs.head) && working_set_dirty())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot start a rebase with uncommitted changes")));
	if (!PROLLY_CALL(r, resolve, upstream, &rs.onto))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("branch not found: %s", upstream)));
	if (!PROLLY_CALL(r, resolve, "HEAD", &rs.head))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("didn't identify any commits!")));
	default_rebase_plan(&rs.head, &rs.onto, &rs);
	if (rs.nsteps == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("didn't identify any commits!")));
	if (PROLLY_CALL(r, branch_lookup, working, &(ProllyBranchId) {0}))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("fatal: A branch named '%s' already exists.", working)));
	rs.branch = branch;
	rs.keep_empty = keep_empty;
	/*
	 * (the rebase's own branch, its machinery: made on the session's behalf,
	 * its head move on the branch rebased checked already, and the role's)
	 */
	BranchCreateOnBehalf = true;
	PG_TRY();
	{
		create_branch(working, hash_hex(&rs.onto));
	}
	PG_FINALLY();
	{
		BranchCreateOnBehalf = false;
	}
	PG_END_TRY();
	checkout_branch(working);
	PROLLY_CALL(r, set_rebase_state, &rs);
}

/*
 * Make the head commit just made (picked onto prev) one with prev's parent
 * instead, its working set, message and author: a squash or fixup into
 * prev, as Dolt's amend.  Or, with parent prev itself, a reword.
 */
static void
rebase_amend(const ProllyHash *parent, const char *message, const char *author)
{
	ProllyHash	head;
	ProllyHash	id;
	ProllyNodeRef ws;

	if (!PROLLY_CALL(refs(), resolve, "HEAD", &head) ||
		!PROLLY_CALL(refs(), commit_working_set, &head, &ws))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("the rebased commit's working set is missing")));
	/* (the head the new commit's parent, as a commit on it is made) */
	if (parent->len > 0)
		PROLLY_CALL(refs(), set_head, parent);
	author_override = author;
	PG_TRY();
	{
		BRANCH_WRITES_HUSHED(PROLLY_CALL(refs(), commit_with, message, parent,
										 parent->len > 0 ? 1 : 0, &ws, &id));
	}
	PG_FINALLY();
	{
		author_override = NULL;
	}
	PG_END_TRY();
}

/*
 * The head commit a step's pick just made (on prev, the head before it):
 * dropped if it changed nothing that its commit did change (unless kept),
 * else reworded, or squashed or fixed up into prev, as the step's action
 * says.
 */
static void
rebase_after_pick(const ProllyRebaseState *rs, const ProllyRebaseStep *s,
				  const ProllyHash *prev)
{
	ProllyHash	head;
	ProllyCommitInfo made;
	ProllyCommitInfo before;
	ProllyCommitInfo orig;
	ProllyCommitInfo orig_parent;

	if (!PROLLY_CALL(refs(), resolve, "HEAD", &head) ||
		!PROLLY_CALL(refs(), commit_get, &head, &made) ||
		!PROLLY_CALL(refs(), commit_get, prev, &before) ||
		!PROLLY_CALL(refs(), commit_get, &s->commit, &orig))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("a commit of the rebase is missing")));

	/*
	 * A commit that becomes empty, its changes there already: dropped
	 * unless --empty=keep.  One empty from the start is kept, as Dolt's.
	 */
	if (same_hash(&made.root.hash, &before.root.hash) && !rs->keep_empty &&
		!(orig.nparents == 1 &&
		  PROLLY_CALL(refs(), commit_get, &orig.parents[0], &orig_parent) &&
		  same_hash(&orig.root.hash, &orig_parent.root.hash)))
	{
		PROLLY_CALL(refs(), set_head, prev);
		return;
	}
	if (strcmp(s->action, "reword") == 0)
		rebase_amend(prev, s->message, orig.author);
	else if (strcmp(s->action, "squash") == 0)
		rebase_amend(before.nparents > 0 ? &before.parents[0] : &(ProllyHash) {0},
					 psprintf("%s\n\n%s", before.message, orig.message),
					 before.author);
	else if (strcmp(s->action, "fixup") == 0)
		rebase_amend(before.nparents > 0 ? &before.parents[0] : &(ProllyHash) {0},
					 before.message, before.author);
}

/*
 * Commit the working set of a step stopped at (its conflicts resolved, or
 * an edit's changes): a commit on the head, with the step's message, then
 * reworded, squashed or fixed up as a pick would be.
 */
static void
rebase_commit_stopped(const ProllyRebaseState *rs, const ProllyRebaseStep *s)
{
	ProllyHash	prev;
	ProllyHash	id;
	ProllyMergeState ms;
	ProllyCommitInfo orig;
	bool		picking = PROLLY_CALL(refs(), merge_state, &ms);

	if (!PROLLY_CALL(refs(), commit_get, &s->commit, &orig))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("rebased commit %s is missing", hash_hex(&s->commit))));
	/* (a pick stopped by conflicts: the head before it is the pick's base) */
	if (!PROLLY_CALL(refs(), resolve, "HEAD", &prev))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("the rebase branch has no head")));
	note_integration();
	author_override = picking ? orig.author : NULL;
	PG_TRY();
	{
		BRANCH_WRITES_HUSHED((void) PROLLY_CALL(refs(), commit,
												picking ? orig.message : s->message,
												true, &id));
	}
	PG_FINALLY();
	{
		author_override = NULL;
	}
	PG_END_TRY();
	/* (an edit's changes are a commit of their own, with its message) */
	if (picking)
		rebase_after_pick(rs, s, &prev);
	settled();
}

static void rebase_abort(void);

/* set by errschemaconflict(), for a rebase to tell a schema conflict apart */
bool		prolly_schema_conflict_raised = false;

/*
 * Among an error's ereport() arguments: the error is a schema conflict
 * (prolly_merge.h).
 */
int
errschemaconflict(void)
{
	prolly_schema_conflict_raised = true;
	return 0;
}

/*
 * Whether a caught error is a schema conflict: marked so as it was raised
 * (errschemaconflict()), and of a code those errors have, so that a mark
 * left by one swallowed on the way isn't taken for another error's.
 */
static bool
caught_schema_conflict(const ErrorData *ed)
{
	return prolly_schema_conflict_raised &&
		(ed->sqlerrcode == ERRCODE_T_R_SERIALIZATION_FAILURE ||
		 ed->sqlerrcode == ERRCODE_FEATURE_NOT_SUPPORTED);
}

/* How a rebase's steps ended: rebase_steps(). */
typedef enum RebaseSteps
{
	REBASE_STEPS_DONE,			/* every step done */
	REBASE_STEPS_STOPPED,		/* stopped at a step: a conflict, an edit */
	REBASE_STEPS_UNKEPT,		/* conflicts the session can't keep */
} RebaseSteps;

/*
 * The plan's steps after the last begun, each picked onto the rebase's
 * branch and made what its action says.  A step that conflicts, or an
 * edit, stops them (result: the status and why).  Conflicts the session
 * can't keep (keep false: outside a transaction block, without
 * dolt.allow_commit_conflicts) are said at unkept_elevel, what they are
 * (an ERROR, or a NOTICE for the caller to abort the rebase).
 */
static RebaseSteps
rebase_steps(ProllyRebaseState *rs, const char **result, bool keep,
			 int unkept_elevel)
{
	const ProllyRefsRoutine *r = rebase_refs();

	for (int i = 0; i < rs->nsteps; i++)
	{
		ProllyRebaseStep *s = &rs->steps[i];
		ProllyHash	prev;
		ProllyHash	id;
		ProllyHash	picked = {0};
		const char *ref = hash_hex(&s->commit);
		int			conflicts;

		CHECK_FOR_INTERRUPTS();
		if (rs->last_step != NULL && rebase_order_cmp(s->order, rs->last_step) <= 0)
			continue;
		rs->last_step = s->order;
		PROLLY_CALL(r, set_rebase_state, rs);
		if (strcmp(s->action, "drop") == 0)
			continue;
		if (!PROLLY_CALL(r, resolve, "HEAD", &prev))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("the rebase branch has no head")));
		INJECTION_POINT("prolly-rebase-step", NULL);
		note_integration();
		(void) PROLLY_CALL(r, resolve, ref, &picked);
		VERSION_MOVE(&picked, picked.len > 0 ? 1 : 0,
					 conflicts = PROLLY_CALL(diff_merge(), cherry_pick, &ref, 1,
											 true, &id));
		if (conflicts > 0)
		{
			int			nviolations = list_length(PROLLY_CALL(r, violations));

			if (!keep)
			{
				unkept_conflicts("rebase", "Rebase", "sides",
								 conflicts - nviolations, nviolations,
								 unkept_elevel);
				return REBASE_STEPS_UNKEPT;
			}
			result[0] = "1";
			result[1] = psprintf("data conflict detected while rebasing commit %s (%s). \n\n"
								 "Resolve the conflicts and remove them from the dolt_conflicts table, "
								 "then continue the rebase by calling dolt_rebase('--continue')",
								 ref, s->message);
			return REBASE_STEPS_STOPPED;
		}
		rebase_after_pick(rs, s, &prev);
		settled();
		if (strcmp(s->action, "edit") == 0)
		{
			result[0] = "0";
			result[1] = psprintf("edit action paused at commit %s (%s). \n\n"
								 "You can now modify the working directory and stage changes. "
								 "When ready, continue the rebase by calling dolt_rebase('--continue')",
								 ref, s->message);
			return REBASE_STEPS_STOPPED;
		}
	}
	return REBASE_STEPS_DONE;
}

/* Dolt's message for a schema conflict at a rebase's step */
static char *
rebase_schema_conflict_message(const ProllyRebaseState *rs)
{
	const char *ref = "";

	for (int i = 0; i < rs->nsteps; i++)
		if (rs->last_step != NULL &&
			rebase_order_cmp(rs->steps[i].order, rs->last_step) == 0)
			ref = hash_hex(&rs->steps[i].commit);
	return psprintf("schema conflict detected while rebasing commit %s. "
					"the rebase has been automatically aborted", ref);
}

/*
 * Run the rebase's steps where Dolt aborts it as a step fails it: a
 * schema conflict, or conflicts the session can't keep.
 *
 * A rebase the statement began (begun_here) is rolled back by an error,
 * which leaves what Dolt's abort does: a schema conflict fails with
 * Dolt's message, conflicts not kept with what they are.  For one begun
 * before, an error would roll the statement back to the rebase as it was
 * before it, so the steps run in a subtransaction, rolled back for the
 * rebase to be aborted, which the statement then commits: result is
 * status 1 and Dolt's message, and false is returned.  (One subtransaction
 * for every step: the abort drops them all anyway.)  Any other error is
 * the statement's, the rebase left as it was.
 */
static bool
rebase_steps_or_abort(ProllyRebaseState *rs, const char **result, bool begun_here)
{
	MemoryContext cxt = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	volatile RebaseSteps done = REBASE_STEPS_DONE;
	ErrorData  *volatile schema_conflict = NULL;

	/* (asked first: in the subtransaction, every statement is in a block) */
	bool		keep = IsTransactionBlock() || prolly_allow_commit_conflicts();

	prolly_schema_conflict_raised = false;
	if (begun_here)
	{
		PG_TRY();
		{
			done = rebase_steps(rs, result, keep, ERROR);
		}
		PG_CATCH();
		{
			ErrorData  *ed;

			MemoryContextSwitchTo(cxt);
			ed = CopyErrorData();
			if (!caught_schema_conflict(ed))
				PG_RE_THROW();
			prolly_schema_conflict_raised = false;
			FlushErrorState();
			/* (40001, as Dolt's merge.SchemaConflict is: the enum limits' too) */
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg_internal("%s", rebase_schema_conflict_message(rs)),
					 errdetail_internal("%s", ed->message)));
		}
		PG_END_TRY();
		return done != REBASE_STEPS_DONE;
	}

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(cxt);
	PG_TRY();
	{
		done = rebase_steps(rs, result, keep, NOTICE);
	}
	PG_CATCH();
	{
		ErrorData  *ed;
		bool		schema;

		MemoryContextSwitchTo(cxt);
		ed = CopyErrorData();
		schema = caught_schema_conflict(ed);
		prolly_schema_conflict_raised = false;
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(cxt);
		CurrentResourceOwner = owner;
		if (!schema)
			ReThrowError(ed);
		schema_conflict = ed;
	}
	PG_END_TRY();
	if (schema_conflict == NULL)
	{
		if (done == REBASE_STEPS_UNKEPT)
			RollbackAndReleaseCurrentSubTransaction();
		else
			ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(cxt);
		CurrentResourceOwner = owner;
		if (done != REBASE_STEPS_UNKEPT)
			return done != REBASE_STEPS_DONE;
	}
	result[0] = "1";
	if (schema_conflict != NULL)
		result[1] = rebase_schema_conflict_message(rs);
	else
		result[1] = "data conflicts from rebase, but session settings do not allow preserving conflicts, "
			"so they cannot be resolved. The rebase has been aborted. "
			"Run the rebase in a transaction block or set dolt.allow_commit_conflicts to on, "
			"and try the rebase again to resolve the conflicts.";
	rebase_abort();
	/* (Dolt's message doesn't say which; the notice does) */
	if (schema_conflict != NULL)
		ereport(NOTICE,
				(errmsg_internal("%s", schema_conflict->message),
				 schema_conflict->detail ?
				 errdetail_internal("%s", schema_conflict->detail) : 0));
	return true;
}

/*
 * Go on with the plan from the step after the last begun: each picked
 * onto the rebase's branch and made what its action says.  A step that
 * conflicts, or an edit, stops it (result: the status and why); the last
 * done, the rebased branch is moved to the rebase's head, checked out, and
 * the rebase's branch deleted.  Where Dolt aborts it, a rebase this
 * statement didn't begin (begun_here false) is aborted, and one it began
 * fails: see rebase_steps_or_abort().
 */
static void
rebase_continue(const char **result, bool begun_here)
{
	const ProllyRefsRoutine *r = rebase_refs();
	ProllyRebaseState rs;
	ProllyMergeState ms;
	char	   *working;
	bool		seen_pick = false;
	ProllyHash	head;

	if (!PROLLY_CALL(r, rebase_state, &rs))
		no_rebase();
	working = pstrdup(branch_ops()->current());

	/* (Dolt validates the plan as each --continue loads it) */
	qsort(rs.steps, rs.nsteps, sizeof(ProllyRebaseStep), rebase_step_cmp);
	for (int i = 0; i < rs.nsteps; i++)
	{
		ProllyRebaseStep *s = &rs.steps[i];
		ProllyCommitInfo info;

		check_rebase_action(s->action);
		if (strcmp(s->action, "pick") == 0 || strcmp(s->action, "reword") == 0)
			seen_pick = true;
		else if ((strcmp(s->action, "squash") == 0 ||
				  strcmp(s->action, "fixup") == 0) && !seen_pick)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid rebase plan: squash and fixup actions must appear after a pick or reword action")));
		if (!PROLLY_CALL(r, commit_get, &s->commit, &info))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("unable to resolve commit hash %s: target commit not found", hash_hex(&s->commit))));
	}

	if (PROLLY_CALL(r, merge_state, &ms))
	{
		int			ctables;
		int			vtables;
		int			itables;

		tables_left(&ctables, &vtables, &itables);
		if (ctables > 0 || vtables > 0 || itables > 0)
		{
			StringInfoData names;

			initStringInfo(&names);
			for (int i = 0; i < ms.ntables; i++)
			{
				char	   *n = ms.table_names && ms.table_names[i] ?
					ms.table_names[i] : get_rel_name(ms.tables[i]);

				appendStringInfo(&names, "%s%s", i ? ", " : "", n ? n : "?");
			}
			for (int i = 0; i < ms.nidentity; i++)
				appendStringInfo(&names, "%s%s", names.len ? ", " : "",
								 ms.identity[i].name);
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("conflicts detected in tables %s; resolve conflicts before continuing the rebase",
							names.data)));
		}
	}
	PreventCommandIfReadOnly("dolt_rebase()");
	/* (it moves the branch rebased once done) */
	prolly_check_head_move(rs.branch);

	/* a step stopped at: its resolution, or an edit's changes, committed */
	if (rs.last_step != NULL &&
		(PROLLY_CALL(r, merge_state, &ms) || working_set_dirty()))
	{
		bool		found = false;

		for (int i = 0; i < rs.nsteps; i++)
			if (rebase_order_cmp(rs.steps[i].order, rs.last_step) == 0)
			{
				rebase_commit_stopped(&rs, &rs.steps[i]);
				found = true;
			}

		/*
		 * (its step taken out of the plan, or moved: what it left would be
		 * lost, or a pick in progress carried into the steps after it)
		 */
		if (!found)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("the rebase stopped at step %s, which is no longer in the plan",
							rs.last_step),
					 errhint("Put the step back, or abort the rebase.")));
	}
	else if (rs.last_step == NULL && working_set_dirty())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot start a rebase with uncommitted changes")));

	if (rebase_steps_or_abort(&rs, result, begun_here))
		return;

	/*
	 * every step done: the rebased branch, if it hasn't changed, moved here.
	 * Changed is a new head, or uncommitted changes, which another session
	 * may have written to it meanwhile: its working set, clean as the rebase
	 * began, is no longer its head's, as Dolt compares its working root.
	 * The move would drop them.
	 */
	{
		ProllyNodeRef ws;
		ProllyNodeRef committed;
		List	   *left_out = NIL;

		if (!PROLLY_CALL(r, resolve, rs.branch, &head) || !same_hash(&head, &rs.head) ||
			(PROLLY_CALL(r, read_working_set, rs.branch, InvalidOid, &ws, &left_out) &&
			 (left_out != NIL ||
			  !PROLLY_CALL(r, commit_working_set, &head, &committed) ||
			  !same_hash(&ws.hash, &committed.hash))))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("rebase aborted due to changes in branch %s", rs.branch)));
	}
	/* (the rebased commits are the branch's, its relations theirs) */
	prolly_move_replaces = true;
	PG_TRY();
	{
		force_branch(rs.branch, working);
	}
	PG_FINALLY();
	{
		prolly_move_replaces = false;
	}
	PG_END_TRY();
	/* (rolled back, the session is back on the rebase's branch, as it is) */
	branch_return_on_abort();
	checkout_branch(rs.branch);
	BRANCH_WRITES_HUSHED(PROLLY_CALL(r, branch_delete, working, true));
	settled();
	result[0] = "0";
	result[1] = psprintf("Successfully rebased and updated refs/heads/%s", rs.branch);
}

/* Abort the rebase: back on the branch rebased, as it was; its branch gone. */
static void
rebase_abort(void)
{
	const ProllyRefsRoutine *r = rebase_refs();
	ProllyRebaseState rs;
	char	   *working;

	if (!PROLLY_CALL(r, rebase_state, &rs))
		no_rebase();
	PreventCommandIfReadOnly("dolt_rebase()");
	prolly_check_head_move(rs.branch);
	working = pstrdup(branch_ops()->current());
	/*
	 * (rolled back, the rebase's branch is back, its state with it: so is
	 * the session, on it)
	 */
	branch_return_on_abort();
	checkout_branch(rs.branch);
	BRANCH_WRITES_HUSHED(PROLLY_CALL(r, branch_delete, working, true));
	settled();
}

/*
 * dolt_rebase(VARIADIC args text[]) returns text[]: {status, message}
 *
 * [-i/--interactive] [--empty=drop|keep] upstream: the session branch's
 * commits that upstream doesn't reach (but merges), replayed onto it, as
 * Dolt's; interactively, the plan is left in the dolt_rebase view to edit
 * (actions pick, reword, squash, fixup, drop, edit, reordered by
 * rebase_order) until --continue runs it.  A step whose pick conflicts
 * stops the rebase, as a pick in progress: resolved, --continue commits it
 * and goes on.  An edit stops after its pick: changes made then are
 * committed by --continue.  --abort goes back to the branch as it was.
 * The rebase works on a branch of its own, dolt_rebase_<branch>, which is
 * deleted when it ends.
 */
Datum
dolt_rebase(PG_FUNCTION_ARGS)
{
	char	  **args;
	int			nargs = text_args(fcinfo, &args);
	bool		options = true;	/* until "--" */
	bool		interactive = false;
	bool		abort_rebase = false;
	bool		continue_rebase = false;
	bool		keep_empty = false;
	char	   *upstream = NULL;
	int			npositional = 0;
	const char *result[2] = {"0", ""};

	for (int i = 0; i < nargs; i++)
	{
		char	   *a = args[i];

		if (options && strcmp(a, "--") == 0)
			options = false;
		else if (!options || a[0] != '-')
		{
			if (npositional++ == 0)
				upstream = a;
		}
		else if (is_option(a, "i") || is_option(a, "interactive"))
			interactive = true;
		else if (is_option(a, "abort"))
			abort_rebase = true;
		else if (is_option(a, "continue"))
			continue_rebase = true;
		else if (strncmp(a, "--empty=", 8) == 0 || is_option(a, "empty"))
		{
			const char *v = strncmp(a, "--empty=", 8) == 0 ? a + 8 :
				option_value(args, nargs, &i, "dolt_rebase");

			if (pg_strcasecmp(v, "keep") == 0)
				keep_empty = true;
			else if (pg_strcasecmp(v, "drop") == 0)
				keep_empty = false;
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("unsupported option for the empty flag (%s); only 'keep' or 'drop' are allowed",
								v)));
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("error: unknown option `%s'", a + (a[1] == '-' ? 2 : 1))));
	}
	if (abort_rebase)
	{
		rebase_abort();
		result[1] = "Interactive rebase aborted";
		PG_RETURN_DATUM(text_array(2, result));
	}
	if (continue_rebase)
	{
		bool		pushed = hold_unmoved("dolt_rebase('--continue')");

		rebase_continue(result, false);
		if (pushed)
			PopActiveSnapshot();
		PG_RETURN_DATUM(text_array(2, result));
	}
	if (npositional == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("not enough args")));
	if (npositional > 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("rebase takes at most one positional argument.")));
	PreventCommandIfReadOnly("dolt_rebase()");
	prolly_check_head_move(branch_ops()->current());
	rebase_start(upstream, keep_empty);
	if (!interactive)
	{
		bool		pushed = hold_unmoved("dolt_rebase()");

		rebase_continue(result, true);
		if (pushed)
			PopActiveSnapshot();
		PG_RETURN_DATUM(text_array(2, result));
	}
	result[1] = psprintf("interactive rebase started on branch %s; "
						 "adjust the rebase plan in the dolt_rebase table, then "
						 "continue rebasing by calling dolt_rebase('--continue')",
						 branch_ops()->current());
	PG_RETURN_DATUM(text_array(2, result));
}

/*
 * dolt_rebase_plan(OUT rebase_order numeric, OUT action text,
 *					OUT commit_hash text, OUT commit_message text)
 *					returns setof record
 *
 * The session branch's rebase plan, in rebase order, behind the dolt_rebase
 * view; an error when no rebase is in progress, as Dolt's dolt_rebase table
 * is there only during one.
 */
Datum
dolt_rebase_plan(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	ProllyRebaseState rs;

	if (!PROLLY_HAS(refs(), rebase_state) || !PROLLY_CALL(refs(), rebase_state, &rs))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("table not found: dolt_rebase"),
				 errdetail("No rebase is in progress.")));
	InitMaterializedSRF(fcinfo, 0);
	qsort(rs.steps, rs.nsteps, sizeof(ProllyRebaseStep), rebase_step_cmp);
	for (int i = 0; i < rs.nsteps; i++)
	{
		Datum		values[4];
		bool		nulls[4] = {0};

		values[0] = rebase_order_datum(rs.steps[i].order);
		values[1] = CStringGetTextDatum(rs.steps[i].action);
		values[2] = CStringGetTextDatum(hash_hex(&rs.steps[i].commit));
		values[3] = CStringGetTextDatum(rs.steps[i].message);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

/* A row given to the dolt_rebase view, as a step. */
static void
rebase_step_of(HeapTuple tuple, TupleDesc desc, ProllyRebaseStep *s)
{
	bool		isnull[4];
	Datum		v[4];

	for (int i = 0; i < 4; i++)
	{
		v[i] = heap_getattr(tuple, i + 1, desc, &isnull[i]);
		if (isnull[i])
			ereport(ERROR,
					(errcode(ERRCODE_NOT_NULL_VIOLATION),
					 errmsg("a dolt_rebase step's %s can't be null",
							NameStr(TupleDescAttr(desc, i)->attname))));
	}
	s->order = rebase_order_text(v[0]);
	s->action = TextDatumGetCString(v[1]);
	check_rebase_action(s->action);
	if (!prolly_hash_from_hex(TextDatumGetCString(v[2]),
							  strlen(TextDatumGetCString(v[2])), &s->commit))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid commit hash: %s", TextDatumGetCString(v[2]))));
	s->message = TextDatumGetCString(v[3]);

	/*
	 * (a commit the store has: the refs name it as a chunk they reference,
	 * so one missing couldn't be stored, nor rebased)
	 */
	{
		ProllyCommitInfo info;

		if (!PROLLY_CALL(refs(), commit_get, &s->commit, &info))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("unable to resolve commit hash %s: target commit not found",
							hash_hex(&s->commit))));
	}
}

/*
 * dolt_rebase_plan_change() returns trigger
 *
 * The dolt_rebase view's INSTEAD OF trigger, row by row: an insert adds a
 * step, an update changes the step of the old row's rebase order, a delete
 * removes it, in the session branch's rebase state, with the transaction.
 * Rebase orders are unique, as the plan's key.
 */
Datum
dolt_rebase_plan_change(PG_FUNCTION_ARGS)
{
	TriggerData *td = (TriggerData *) fcinfo->context;
	TupleDesc	desc;
	ProllyRebaseState rs;
	ProllyRebaseStep oldstep;
	ProllyRebaseStep newstep;
	bool		has_old;
	bool		has_new;
	int			at = -1;
	HeapTuple	ret;

	if (!CALLED_AS_TRIGGER(fcinfo) || !TRIGGER_FIRED_INSTEAD(td->tg_event) ||
		!TRIGGER_FIRED_FOR_ROW(td->tg_event))
		elog(ERROR, "dolt_rebase_plan_change: not called as an INSTEAD OF row trigger");
	desc = RelationGetDescr(td->tg_relation);
	if (desc->natts != 4)
		elog(ERROR, "dolt_rebase_plan_change: the view has %d columns, not 4", desc->natts);
	if (!PROLLY_HAS(refs(), rebase_state) || !PROLLY_CALL(refs(), rebase_state, &rs))
		no_rebase();
	PreventCommandIfReadOnly("dolt_rebase");
	prolly_check_head_move(rs.branch);
	has_old = !TRIGGER_FIRED_BY_INSERT(td->tg_event);
	has_new = !TRIGGER_FIRED_BY_DELETE(td->tg_event);
	if (has_old)
	{
		rebase_step_of(td->tg_trigtuple, desc, &oldstep);
		for (int i = 0; i < rs.nsteps; i++)
			if (rebase_order_cmp(rs.steps[i].order, oldstep.order) == 0)
				at = i;
		if (at < 0)
			PG_RETURN_POINTER(NULL);	/* (gone already: nothing done) */
	}
	if (has_new)
	{
		rebase_step_of(TRIGGER_FIRED_BY_UPDATE(td->tg_event) ?
					   td->tg_newtuple : td->tg_trigtuple, desc, &newstep);
		for (int i = 0; i < rs.nsteps; i++)
			if (i != at && rebase_order_cmp(rs.steps[i].order, newstep.order) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_UNIQUE_VIOLATION),
						 errmsg("duplicate rebase_order %s in the rebase plan", newstep.order)));
	}
	if (has_old && has_new)
		rs.steps[at] = newstep;
	else if (has_new)
	{
		rs.steps = repalloc(rs.steps, sizeof(ProllyRebaseStep) * (rs.nsteps + 1));
		rs.steps[rs.nsteps++] = newstep;
	}
	else
	{
		memmove(&rs.steps[at], &rs.steps[at + 1],
				sizeof(ProllyRebaseStep) * (rs.nsteps - at - 1));
		rs.nsteps--;
	}
	qsort(rs.steps, rs.nsteps, sizeof(ProllyRebaseStep), rebase_step_cmp);
	PROLLY_CALL(refs(), set_rebase_state, &rs);
	ret = TRIGGER_FIRED_BY_UPDATE(td->tg_event) ? td->tg_newtuple : td->tg_trigtuple;
	PG_RETURN_POINTER(ret);
}

/*
 * A stream of dolt_log(ref)'s rows (see prolly_log.h), the commits read as
 * they are given; NULL for HEAD before a first commit.  The ref's commit is
 * kept from collection while the transaction runs (the chunk store's
 * keep()), and with it every commit it reaches.
 */
ProllyLogStream *
prolly_log_begin(const char *ref)
{
	ProllyHash	start;

	if (!PROLLY_CALL(refs(), resolve, ref, &start))
	{
		if (pg_strcasecmp(ref, "HEAD") == 0)
			return NULL;		/* nothing committed yet */
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("prolly ref \"%s\" does not exist", ref)));
	}
	/*
	 * (the start kept from collection for the transaction: a stream reads
	 * its commits as it goes, after a collection that began meanwhile, a
	 * history drop's, would otherwise have freed them)
	 */
	{
		ProllyTreeEnv env;

		prolly_folded_tree_env(&env);
		PROLLY_CALL(env.chunk_store, keep, env.chunks, &start);
	}
	return log_stream_begin(list_make1(&start));
}

/* dolt_log's next row into values (6 of them); false at the end. */
bool
prolly_log_next(ProllyLogStream *stream, Datum *values, bool *nulls)
{
	LogEntry   *e = stream ? log_stream_next(stream) : NULL;
	char	   *name;
	char	   *email;

	if (e == NULL)
		return false;
	split_author(e->info.author, &name, &email);
	memset(nulls, 0, sizeof(bool) * 6);
	values[0] = CStringGetTextDatum(hash_hex(&e->info.id));
	values[1] = CStringGetTextDatum(name);
	values[2] = CStringGetTextDatum(email);
	values[3] = TimestampTzGetDatum(e->info.committed_at);
	values[4] = CStringGetTextDatum(e->info.message);
	values[5] = Int64GetDatum(e->order);
	return true;
}

/*
 * dolt_log(ref text, OUT commit_hash text, OUT committer text,
 *			OUT email text, OUT date timestamptz, OUT message text,
 *			OUT commit_order int8) returns setof record
 *
 * Every commit ref reaches, highest commit order (height) first, then
 * newest.  (Planned, it is read by a scan of its own that gives each row as
 * it is found, so a LIMIT reads no more commits than it gives:
 * prolly_log_scan.c.)
 */
Datum
dolt_log(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	char	   *ref = text_to_cstring(PG_GETARG_TEXT_PP(0));
	ProllyLogStream *stream;
	Datum		values[6];
	bool		nulls[6];

	InitMaterializedSRF(fcinfo, 0);
	stream = prolly_log_begin(ref);
	while (prolly_log_next(stream, values, nulls))
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	return (Datum) 0;
}

/*
 * dolt_commits(OUT commit_hash text, OUT committer text, OUT email text,
 *				OUT date timestamptz, OUT message text) returns setof record
 *
 * Every commit any branch reaches, in dolt_log's order (#588).
 */
Datum
dolt_commits(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	ListCell   *lc;

	InitMaterializedSRF(fcinfo, 0);
	foreach(lc, all_log_entries())
	{
		LogEntry   *e = lfirst(lc);
		Datum		values[5];
		bool		nulls[5] = {0};
		char	   *name;
		char	   *email;

		split_author(e->info.author, &name, &email);
		values[0] = CStringGetTextDatum(hash_hex(&e->info.id));
		values[1] = CStringGetTextDatum(name);
		values[2] = CStringGetTextDatum(email);
		values[3] = TimestampTzGetDatum(e->info.committed_at);
		values[4] = CStringGetTextDatum(e->info.message);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

/*
 * dolt_commit_ancestors(OUT commit_hash text, OUT parent_hash text,
 *						 OUT parent_index int4) returns setof record
 *
 * Each commit dolt_commits() has, with its parents in order; a root commit
 * with a null parent, index 0, as Dolt's (#588).
 */
Datum
dolt_commit_ancestors(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	ListCell   *lc;

	InitMaterializedSRF(fcinfo, 0);
	foreach(lc, all_log_entries())
	{
		LogEntry   *e = lfirst(lc);
		Datum		values[3];
		bool		nulls[3] = {0};

		values[0] = CStringGetTextDatum(hash_hex(&e->info.id));
		if (e->info.nparents == 0)
		{
			nulls[1] = true;
			values[2] = Int32GetDatum(0);
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values,
								 nulls);
		}
		for (int i = 0; i < e->info.nparents; i++)
		{
			values[1] = CStringGetTextDatum(hash_hex(&e->info.parents[i]));
			values[2] = Int32GetDatum(i);
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values,
								 nulls);
		}
	}
	return (Datum) 0;
}

typedef struct BranchRows
{
	ReturnSetInfo *rsinfo;
} BranchRows;

static void
branch_row(const char *name, const ProllyHash *head, void *arg)
{
	BranchRows *br = arg;
	Datum		values[13];
	bool		nulls[13] = {0};
	char	   *writer;
	char	   *base;
	Oid			writer_role;
	HeapTuple	tup;
	ProllyCommitInfo info;
	ProllyNodeRef ws;
	ProllyNodeRef committed;

	/* (the hidden branches of sessions checked out at commits) */
	if (detached_branch(name))
		return;
	values[0] = CStringGetTextDatum(name);
	if (head->len > 0 && PROLLY_CALL(refs(), commit_get, head, &info))
	{
		char	   *who;
		char	   *email;

		split_author(info.author, &who, &email);
		values[1] = CStringGetTextDatum(hash_hex(head));
		values[2] = CStringGetTextDatum(who);
		values[3] = CStringGetTextDatum(email);
		values[4] = TimestampTzGetDatum(info.committed_at);
		values[5] = CStringGetTextDatum(info.message);
		{
			List	   *left_out = NIL;

			/* (objects a commit refuses are changes it can't hold) */
			values[6] = BoolGetDatum(PROLLY_CALL(refs(), read_working_set, name,
												 InvalidOid, &ws, &left_out) &&
									 (left_out != NIL ||
									  !(PROLLY_CALL(refs(), commit_working_set, head,
													&committed) &&
										same_hash(&ws.hash, &committed.hash))));
		}
	}
	else
	{
		for (int i = 1; i < 6; i++)
			nulls[i] = true;
		values[6] = BoolGetDatum(true);
	}

	/* (its owner: the writer whose changes it holds, in an owning mode) */
	nulls[7] = nulls[8] = true;
	tup = SearchSysCache1(BRANCHNAME, CStringGetDatum(name));
	if (HeapTupleIsValid(tup))
	{
		bool		isnull;
		Datum		owner = SysCacheGetAttr(BRANCHNAME, tup,
											Anum_pg_branch_browner, &isnull);
		Oid			role = ((Form_pg_branch) GETSTRUCT(tup))->brownerrole;

		if (!isnull)
		{
			values[7] = datumCopy(owner, false, -1);
			nulls[7] = false;
		}
		if (OidIsValid(role))
		{
			const char *rolname = GetUserNameFromId(role, true);

			values[8] = CStringGetTextDatum(rolname ? rolname : "?");
			nulls[8] = false;
		}
		ReleaseSysCache(tup);
	}
	values[9] = BoolGetDatum(PROLLY_CALL(writers(), protected, name));
	/* (a writer branch's base: the branch it writes for) */
	if (writer_branch_name(name) &&
		PROLLY_CALL(writers(), writer_branch, name, &writer, &base, &writer_role))
	{
		values[10] = CStringGetTextDatum("writer");
		values[11] = CStringGetTextDatum(base);
	}
	else
	{
		values[10] = CStringGetTextDatum("branch");
		nulls[11] = true;
	}
	/* (the role that owns it, whose head moves need nothing more) */
	{
		const char *creator = GetUserNameFromId(prolly_branch_owner(name), true);

		values[12] = CStringGetTextDatum(creator ? creator : "?");
	}
	tuplestore_putvalues(br->rsinfo->setResult, br->rsinfo->setDesc, values,
						 nulls);
}

/*
 * dolt_branches(OUT name text, OUT hash text, OUT latest_committer text,
 *				 OUT latest_committer_email text,
 *				 OUT latest_commit_date timestamptz,
 *				 OUT latest_commit_message text, OUT dirty bool)
 *	  returns setof record
 *
 * dirty: the branch's working set has changes since its head commit.
 * (Then owner and owner_role, the writer whose changes it holds; protected;
 * kind and base, a writer branch's; and creator, the role that owns the
 * branch: its maker, or the database's owner for main, a branch whose maker
 * isn't recorded, or one whose maker was dropped.)
 */
Datum
dolt_branches(PG_FUNCTION_ARGS)
{
	BranchRows	br;

	InitMaterializedSRF(fcinfo, 0);
	br.rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	PROLLY_CALL(refs(), branches, branch_row, &br);
	return (Datum) 0;
}

/*
 * dolt_writer_mode() returns text: the database's writer mode
 * (dolt_writer_mode_noargs).
 * dolt_writer_mode(mode text) returns text: sets it (the database's owner
 * only): "shared" (every session writes every branch), "owned" (a dirty
 * branch is one writer's), "branch-only" (owned, and main protected: it
 * changes only by integrations, as dolt_integrate makes), or "branch-auto"
 * (branch-only, a session writing on a protected branch moved to its writer
 * branch first).
 */
Datum
dolt_writer_mode(PG_FUNCTION_ARGS)
{
	static const char *const names[] = {PROLLY_WRITER_MODE_NAMES};

	if (PG_NARGS() > 0)
	{
		char	   *mode = text_to_cstring(PG_GETARG_TEXT_PP(0));
		int			m;

		for (m = 0; m < lengthof(names); m++)
		{
			if (strcmp(mode, names[m]) == 0)
				break;
		}
		if (m == lengthof(names))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unknown writer mode \"%s\"", mode),
					 errhint("The writer modes are \"shared\", \"owned\", \"branch-only\" and \"branch-auto\".")));
		PreventCommandIfReadOnly("dolt_writer_mode()");
		PROLLY_CALL(writers(), set_mode, (ProllyWriterMode) m);
	}
	PG_RETURN_TEXT_P(cstring_to_text(names[PROLLY_CALL(writers(), mode)]));
}

Datum
dolt_writer_mode_noargs(PG_FUNCTION_ARGS)
{
	return dolt_writer_mode(fcinfo);
}

/* The roots dolt_reflog() reads at most, the newest: as many as Dolt's. */
#define REFLOG_ROOTS	5000

typedef struct ReflogRoot
{
	ProllyHash	root;
	TimestampTz at;
} ReflogRoot;

typedef struct ReflogRoots
{
	ReflogRoot *roots;			/* a ring of the newest REFLOG_ROOTS */
	uint64		n;				/* roots seen */
} ReflogRoots;

static void
reflog_root(const ProllyHash *root, TimestampTz at, void *arg)
{
	ReflogRoots *rr = arg;
	ReflogRoot *r = &rr->roots[rr->n++ % REFLOG_ROOTS];

	r->root = *root;
	r->at = at;
}

/* A ref's last commit, as the roots are read oldest first. */
typedef struct ReflogRef
{
	char	   *ref;
	ProllyHash	commit;
} ReflogRef;

typedef struct ReflogScan
{
	TimestampTz at;				/* the root's */
	char	   *target;			/* the ref asked for, or NULL: all */
	List	   *last;			/* ReflogRef *, a ref's last commit */
	List	   *rows;			/* ReflogRow *, oldest first */
} ReflogScan;

typedef struct ReflogRow
{
	Datum		values[4];
	bool		nulls[4];
} ReflogRow;

/* Every ref a root names, in its order: ReflogRef *. */
static void
reflog_collect(const char *ref, const ProllyHash *commit, void *arg)
{
	List	  **named = arg;
	ReflogRef  *r = palloc(sizeof(ReflogRef));

	r->ref = pstrdup(ref);
	r->commit = *commit;
	*named = lappend(*named, r);
}

/* Does a root name the refs a collection left, each at its commit? */
static bool
reflog_same_refs(const ProllyHash *root, List *left)
{
	List	   *named = NIL;
	bool		same;

	(void) PROLLY_CALL(refs(), root_refs, root, reflog_collect, &named);
	same = list_length(named) == list_length(left);
	for (int i = 0; same && i < list_length(named); i++)
	{
		ReflogRef  *a = list_nth(named, i);
		ReflogRef  *b = list_nth(left, i);

		same = strcmp(a->ref, b->ref) == 0 && same_hash(&a->commit, &b->commit);
	}
	list_free_deep(named);
	return same;
}

static void
reflog_ref(const char *ref, const ProllyHash *commit, void *arg)
{
	ReflogScan *rs = arg;
	ReflogRef  *last = NULL;
	ListCell   *lc;
	ProllyCommitInfo info;
	ReflogRow  *row;

	/*
	 * As Dolt does: a branch's name alone stands for the first ref it ends,
	 * case aside; then only the ref matching it, case aside, is listed.
	 */
	if (rs->target != NULL)
	{
		size_t		rl = strlen(ref);
		size_t		tl = strlen(rs->target);

		if (rl > tl && ref[rl - tl - 1] == '/' &&
			pg_strcasecmp(ref + rl - tl, rs->target) == 0)
			rs->target = pstrdup(ref);
		if (pg_strcasecmp(ref, rs->target) != 0)
			return;
	}
	foreach(lc, rs->last)
	{
		if (strcmp(((ReflogRef *) lfirst(lc))->ref, ref) == 0)
		{
			last = lfirst(lc);
			break;
		}
	}
	if (last != NULL && same_hash(&last->commit, commit))
		return;
	if (last == NULL)
	{
		last = palloc(sizeof(ReflogRef));
		last->ref = pstrdup(ref);
		rs->last = lappend(rs->last, last);
	}
	last->commit = *commit;

	row = palloc0(sizeof(ReflogRow));
	row->values[0] = CStringGetTextDatum(ref);
	/* (a time the store didn't record is unknown) */
	row->values[1] = TimestampTzGetDatum(rs->at);
	row->nulls[1] = rs->at == 0;
	row->values[2] = CStringGetTextDatum(hash_hex(commit));
	memset(&info, 0, sizeof(info));
	if (PROLLY_CALL(refs(), commit_get, commit, &info))
		row->values[3] = CStringGetTextDatum(info.message);
	else
		row->nulls[3] = true;
	rs->rows = lappend(rs->rows, row);
}

/*
 * dolt_reflog(VARIADIC args text[], OUT ref text,
 *			   OUT ref_timestamp timestamptz, OUT commit_hash text,
 *			   OUT commit_message text) returns setof record
 *
 * Each change of a ref (a branch's head, a remote-tracking branch's
 * commit) the chunk store still records, newest first: as Dolt's reflog,
 * read from the roots the store was moved to, each with its time; a
 * garbage collection forgets the moves before it.  As in Dolt, whose roots
 * after a collection are each a ref's change, the first of them that
 * changed a ref lists every ref it names; those before it that changed
 * none (a working set's checkpoint, as the fold worker takes, which moves
 * the root and no ref) list nothing.  An argument names one
 * ref ("main", or "refs/heads/main"); "--all" lists every ref, which they
 * all are here already.
 */
Datum
dolt_reflog(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	const ProllyChunkStoreRoutine *cs = prolly_chunk_store();
	char	  **args;
	int			nargs = text_args(fcinfo, &args);
	bool		all = false;
	ReflogRoots rr = {0};
	ReflogScan	rs = {0};
	uint64		first;
	ProllyHash	before = {0};
	List	   *left = NIL;
	bool		unchanged = false;

	for (int i = 0; i < nargs; i++)
	{
		if (args[i][0] == '\0')
			continue;			/* (no ref, as Dolt takes it) */
		if (strcmp(args[i], "--all") == 0)
		{
			if (all)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("multiple values provided for \"--all\"")));
			all = true;
		}
		else if (rs.target != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("dolt_reflog takes at most one ref"),
					 errdetail("It was given \"%s\" and \"%s\".",
							   rs.target, args[i])));
		else
			rs.target = args[i];
	}

	InitMaterializedSRF(fcinfo, 0);
	if (!PROLLY_HAS(cs, roots))
		return (Datum) 0;
	rr.roots = palloc(sizeof(ReflogRoot) * REFLOG_ROOTS);
	PROLLY_CALL(cs, roots, PROLLY_CALL(cs, open, MyDatabaseId,
									   MyDatabaseTableSpace),
				&before, reflog_root, &rr);
	first = rr.n > REFLOG_ROOTS ? rr.n - REFLOG_ROOTS : 0;
	/* (with the oldest roots passed over, it isn't the one before them) */
	if (before.len > 0 && first == 0)
	{
		(void) PROLLY_CALL(refs(), root_refs, &before, reflog_collect, &left);
		unchanged = true;
	}
	for (uint64 i = first; i < rr.n; i++)
	{
		ReflogRoot *r = &rr.roots[i % REFLOG_ROOTS];

		CHECK_FOR_INTERRUPTS();
		if (unchanged)
		{
			if (same_hash(&r->root, &before) ||
				reflog_same_refs(&r->root, left))
				continue;
			unchanged = false;
		}
		rs.at = r->at;
		(void) PROLLY_CALL(refs(), root_refs, &r->root, reflog_ref, &rs);
	}

	/* newest first */
	for (int i = list_length(rs.rows) - 1; i >= 0; i--)
	{
		ReflogRow  *row = list_nth(rs.rows, i);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, row->values,
							 row->nulls);
	}
	return (Datum) 0;
}

Datum
dolt_reflog_noargs(PG_FUNCTION_ARGS)
{
	return dolt_reflog(fcinfo);
}

/* dolt_status's rows ("name\tstatus") by name, which may hold a tab. */
static int
status_cmp(const ListCell *a, const ListCell *b)
{
	const char *x = lfirst(a);
	const char *y = lfirst(b);
	size_t		xl = strrchr(x, '\t') - x;
	size_t		yl = strrchr(y, '\t') - y;
	int			c = memcmp(x, y, Min(xl, yl));

	return c != 0 ? c : xl < yl ? -1 : xl > yl ? 1 : strcmp(x + xl, y + yl);
}

typedef struct StatusSink
{
	ProllyDiffSink sink;		/* must be first */
	List	   *relids;
} StatusSink;

static void
status_change(ProllyDiffSink *self, Oid relid, ProllyDiffKind kind,
			  const ProllyKey *key, const ProllyValue *before,
			  const ProllyValue *after)
{
	StatusSink *s = (StatusSink *) self;

	s->relids = list_append_unique_oid(s->relids, relid);
}

/*
 * A table, or another object, changed: its name, a tab, and its status.
 * Other objects take the tables' statuses, as in Doltgres, but a schema
 * made or dropped ("new schema", "deleted schema"), as in Dolt.
 */
static void
status_table(const char *name, ProllyDiffKind kind, char object, void *arg)
{
	List	  **names = arg;
	static const char *const statuses[] = {
		[PROLLY_DIFF_ADDED] = "new table",
		[PROLLY_DIFF_REMOVED] = "deleted",
		[PROLLY_DIFF_NOT_COVERED] = "not covered",
		[PROLLY_DIFF_MODIFIED] = "modified",
	};
	const char *status = statuses[kind];

	if ((kind == PROLLY_DIFF_ADDED || kind == PROLLY_DIFF_REMOVED) &&
		object == PROLLY_OBJECT_SCHEMA)
		status = kind == PROLLY_DIFF_ADDED ? "new schema" : "deleted schema";
	*names = lappend(*names, psprintf("%s\t%s", name, status));
}

/*
 * A diff function's refs, as Dolt's take them: two refs; or one range,
 * "a..b" (from a to b) or "a...b" (from a's and b's merge base to b), the
 * argument after it then the object's name.  *from_ref, *to_ref and
 * *name are set from the arguments given (to_ref and name NULL if not).
 */
static void
diff_refs(const char *fn, char **from_ref, char **to_ref, char **name)
{
	char	   *dots = strstr(*from_ref, "..");
	bool		three;
	char	   *a;
	char	   *b;

	if (dots == NULL)
	{
		if (*to_ref == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("%s needs two refs, or a range of them (a..b, a...b)", fn)));
		return;
	}
	three = dots[2] == '.';
	a = pnstrdup(*from_ref, dots - *from_ref);
	b = pstrdup(dots + (three ? 3 : 2));
	if (*a == '\0' || *b == '\0' || strstr(b, "..") != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" isn't a range of refs", *from_ref),
				 errhint("A range is a..b, or a...b from their merge base.")));
	if (*to_ref != NULL)
	{
		if (*name != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("%s takes a range and a name, or two refs and a name", fn)));
		*name = *to_ref;
	}
	if (three)
	{
		ProllyHash	ha;
		ProllyHash	hb;
		ProllyHash	base;

		if (!PROLLY_CALL(refs(), resolve, a, &ha))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("prolly ref \"%s\" does not exist", a)));
		if (!PROLLY_CALL(refs(), resolve, b, &hb))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("prolly ref \"%s\" does not exist", b)));
		if (!PROLLY_CALL(refs(), merge_base, &ha, &hb, &base))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("refs \"%s\" and \"%s\" have no merge base", a, b)));
		a = hash_hex(&base);
	}
	*from_ref = a;
	*to_ref = b;
}

/* What dolt_diff_summary() and dolt_schema_diff() list: each change, copied. */
typedef struct SummaryRow
{
	char	   *from_name;		/* NULL: none */
	char	   *to_name;
	char		object;
	bool		data_change;
	bool		schema_change;
	bool		not_covered;
	char	   *from_item;		/* the items (NULL: none) */
	int			from_len;
	char	   *to_item;
	int			to_len;
} SummaryRow;

/*
 * A copy of an item's bytes: they begin with its data tree's hash, which
 * can hold zero bytes, so it isn't copied as a string.
 */
static char *
copy_item(const ProllyValue *v)
{
	char	   *c = palloc(v->len + 1);

	memcpy(c, v->data, v->len);
	c[v->len] = '\0';
	return c;
}

static void
collect_change(const ProllyObjectChange *c, void *arg)
{
	List	  **rows = arg;
	SummaryRow *r = palloc0(sizeof(SummaryRow));

	r->from_name = c->from_name ? pstrdup(c->from_name) : NULL;
	r->to_name = c->to_name ? pstrdup(c->to_name) : NULL;
	r->object = c->object;
	r->data_change = c->data_change;
	r->schema_change = c->schema_change;
	r->not_covered = c->not_covered;
	if (c->from_item)
	{
		r->from_item = copy_item(c->from_item);
		r->from_len = c->from_item->len;
	}
	if (c->to_item)
	{
		r->to_item = copy_item(c->to_item);
		r->to_len = c->to_item->len;
	}
	*rows = lappend(*rows, r);
}

/* Whether name is a change's, on either side. */
static bool
row_named(const SummaryRow *r, const char *name)
{
	return (r->from_name && strcmp(r->from_name, name) == 0) ||
		(r->to_name && strcmp(r->to_name, name) == 0);
}

/*
 * A name as an object's name is written in a summary: its identifiers
 * parsed as Postgres parses them (case folded, quotes honored) and quoted
 * as needed, a routine's argument list after them as given; *schema
 * whether it names its schema.  NULL if it isn't a name.
 */
static char *
normalized_name(const char *name, bool *schema)
{
	const char *paren = NULL;
	bool		quoted = false;
	char	   *ident;
	List	   *parts;
	StringInfoData out;
	ListCell   *lc;

	/* a routine's argument list begins at the first '(' outside quotes */
	for (const char *p = name; *p && paren == NULL; p++)
	{
		if (*p == '"')
			quoted = !quoted;
		else if (*p == '(' && !quoted)
			paren = p;
	}
	ident = paren ? pnstrdup(name, paren - name) : pstrdup(name);
	if (!SplitIdentifierString(ident, '.', &parts) || parts == NIL ||
		list_length(parts) > 2)
		return NULL;
	initStringInfo(&out);
	foreach(lc, parts)
	{
		if (foreach_current_index(lc) > 0)
			appendStringInfoChar(&out, '.');
		appendStringInfoString(&out, quote_identifier(lfirst(lc)));
	}
	if (paren)
		appendStringInfoString(&out, paren);
	*schema = list_length(parts) == 2;
	return out.data;
}

/*
 * The changes between two refs, every one, or (name not NULL) the object
 * name names in either version, its identifiers read as Postgres reads
 * them: as qualified, or, a name without its schema, in the first schema
 * of the search path that has one of that name changed (it need not exist
 * now).
 */
static char *row_name_wanted(List *rows, const char *name);

static List *
summary_rows(const char *from_ref, const char *to_ref, const char *name,
			 bool definitions)
{
	List	   *rows = NIL;
	List	   *kept = NIL;
	char	   *want;
	ListCell   *lc;

	if (!PROLLY_HAS(diff_merge(), summary))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("the database's version control can't summarize a diff")));
	PROLLY_CALL(diff_merge(), summary, from_ref, to_ref, definitions,
				collect_change, &rows);
	if (name == NULL)
		return rows;
	want = row_name_wanted(rows, name);
	if (want == NULL)
		return NIL;
	foreach(lc, rows)
		if (row_named(lfirst(lc), want))
			kept = lappend(kept, lfirst(lc));
	return kept;
}

/*
 * The name of rows' object name names, as summary_rows() reads it: as
 * qualified, or in the first schema of the search path that has one of
 * that name among rows.  NULL if none.
 */
static char *
row_name_wanted(List *rows, const char *name)
{
	char	   *want = NULL;
	ListCell   *lc;
	bool		qualified = false;
	char	   *normal;

	/* qualified, or found by the search path */
	normal = normalized_name(name, &qualified);
	if (normal == NULL)
		return NULL;
	name = normal;
	foreach(lc, rows)
		if (row_named(lfirst(lc), name))
			want = pstrdup(name);
	if (want == NULL)
	{
		if (!qualified)
		{
			List	   *path = fetch_search_path(false);
			ListCell   *nc;

			foreach(nc, path)
			{
				char	   *nsp = get_namespace_name(lfirst_oid(nc));
				char	   *q;

				if (nsp == NULL)
					continue;
				q = psprintf("%s.%s", quote_identifier(nsp), name);
				foreach(lc, rows)
					if (row_named(lfirst(lc), q))
						want = q;
				if (want)
					break;
			}
		}
	}
	return want;
}

/* Is one of rows a table's? */
static bool
rows_have_table(List *rows)
{
	ListCell   *lc;

	foreach(lc, rows)
		if (((SummaryRow *) lfirst(lc))->object == PROLLY_OBJECT_TABLE)
			return true;
	return false;
}

/*
 * Does name name a table ref has?  Its objects are what differs from the
 * empty root to it.
 */
static bool
version_has_table(const char *ref, const char *name)
{
	List	   *rows = NIL;
	char	   *want;
	ListCell   *lc;

	PROLLY_CALL(diff_merge(), summary, NULL, ref, false, collect_change, &rows);
	want = row_name_wanted(rows, name);
	if (want != NULL)
		foreach(lc, rows)
		{
			SummaryRow *r = lfirst(lc);

			if (row_named(r, want) && r->object == PROLLY_OBJECT_TABLE)
				return true;
		}
	return false;
}

/*
 * dolt_diff_summary(from_ref text, to_ref text [, name text], OUT
 * from_table_name text, OUT to_table_name text, OUT diff_type text, OUT
 * data_change bool, OUT schema_change bool) returns setof record
 *
 * Each object that differs between two refs (WORKING and STAGED as
 * dolt_diff reads them), tables and the rest, named as dolt_status names
 * them, by name: added, dropped, modified, or renamed (dropped and added
 * with the same definition and rows, alone of their content), '' for the
 * side it isn't on; whether its rows (a sequence's value) and its
 * definition differ (a new name is a change of definition; an empty table
 * added or dropped changes no rows, as in Dolt).  Objects a read of a working set left out are "not
 * covered", their flags NULL.  With name, that object's alone.
 */
Datum
dolt_diff_summary(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	char	   *from_ref;
	char	   *to_ref;
	char	   *name = NULL;
	ListCell   *lc;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("dolt_diff_summary needs two refs")));
	from_ref = text_to_cstring(PG_GETARG_TEXT_PP(0));
	to_ref = PG_ARGISNULL(1) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(1));
	if (PG_NARGS() > 2 && !PG_ARGISNULL(2))
		name = text_to_cstring(PG_GETARG_TEXT_PP(2));
	diff_refs("dolt_diff_summary", &from_ref, &to_ref, &name);
	InitMaterializedSRF(fcinfo, 0);
	foreach(lc, summary_rows(from_ref, to_ref, name, false))
	{
		SummaryRow *r = lfirst(lc);
		Datum		values[5];
		bool		nulls[5] = {0};
		const char *type;

		if (r->not_covered)
			type = "not covered";
		else if (r->from_name && r->to_name)
			type = strcmp(r->from_name, r->to_name) == 0 ? "modified" : "renamed";
		else
			type = r->to_name ? "added" : "dropped";
		values[0] = CStringGetTextDatum(r->from_name ? r->from_name : "");
		values[1] = CStringGetTextDatum(r->not_covered ? r->from_name :
										r->to_name ? r->to_name : "");
		values[2] = CStringGetTextDatum(type);
		values[3] = BoolGetDatum(r->data_change);
		values[4] = BoolGetDatum(r->schema_change);
		nulls[3] = nulls[4] = r->not_covered;
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

/* One dolt_diff() row: a commit's (or the working set's) object that changed. */
static void
put_commit_changes(ReturnSetInfo *rsinfo, const char *hash,
				   const ProllyCommitInfo *info, List *rows)
{
	ListCell   *lc;
	char	   *name = NULL;
	char	   *email = NULL;

	if (info)
		split_author(info->author, &name, &email);
	foreach(lc, rows)
	{
		SummaryRow *r = lfirst(lc);
		Datum		values[8];
		bool		nulls[8] = {0};

		if (r->not_covered)
			continue;
		values[0] = CStringGetTextDatum(hash);
		values[1] = CStringGetTextDatum(r->to_name ? r->to_name : r->from_name);
		if (info)
		{
			values[2] = CStringGetTextDatum(name);
			values[3] = CStringGetTextDatum(email);
			values[4] = TimestampTzGetDatum(info->committed_at);
			values[5] = CStringGetTextDatum(info->message);
		}
		else
			nulls[2] = nulls[3] = nulls[4] = nulls[5] = true;
		values[6] = BoolGetDatum(r->data_change);
		values[7] = BoolGetDatum(r->schema_change);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
}

/*
 * dolt_diff(OUT commit_hash text, OUT table_name text, OUT committer text,
 *			 OUT email text, OUT date timestamptz, OUT message text, OUT
 *			 data_change bool, OUT schema_change bool) returns setof record
 *
 * What each commit the head reaches changed, against its first parent (a
 * first commit, against nothing), object by object as dolt_diff_summary
 * names them, with the commit's metadata; the working set's changes first,
 * as commit WORKING (its metadata NULL).  As Dolt's dolt_diff system
 * table, but for every kind of object.
 */
Datum
dolt_diff_commits(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	List	   *entries = log_entries("HEAD");
	ListCell   *lc;

	InitMaterializedSRF(fcinfo, 0);
	put_commit_changes(rsinfo, "WORKING", NULL,
					   summary_rows(entries ? "HEAD" : NULL, "WORKING", NULL, false));
	foreach(lc, entries)
	{
		LogEntry   *e = lfirst(lc);
		char	   *to = hash_hex(&e->info.id);
		char	   *from = e->info.nparents > 0 ? hash_hex(&e->info.parents[0]) : NULL;

		CHECK_FOR_INTERRUPTS();
		put_commit_changes(rsinfo, to, &e->info, summary_rows(from, to, NULL, false));
	}
	return (Datum) 0;
}

/* The statements that make an object, from its item ('' for none). */
static char *
render_item(char object, const char *name, const char *item, int len)
{
	StringInfoData out;
	ProllyValue v;

	initStringInfo(&out);
	if (item == NULL)
		return out.data;
	v.data = (const uint8 *) item;
	v.len = len;
	PROLLY_CALL(canonicalizer(), render, object, name, &v, &out);
	return out.data;
}

/*
 * dolt_schema_diff(from_ref text, to_ref text [, name text], OUT
 * from_table_name text, OUT to_table_name text, OUT from_create_statement
 * text, OUT to_create_statement text) returns setof record
 *
 * Each object whose statements differ between two refs (its definition
 * changed, or it was added, dropped or renamed; a change of rows alone
 * gives none, as in Dolt), as dolt_diff_summary() lists them: each side's
 * statements, rendered from that version's description (never from the
 * catalogs: any two refs, whatever the session's branch holds), '' for
 * the side it isn't on.  A table's is its CREATE TABLE, then its
 * constraints, indexes, triggers, rules, statistics, row security, foreign
 * keys, policies and parents, a statement a line.
 */
Datum
dolt_schema_diff(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	char	   *from_ref;
	char	   *to_ref;
	char	   *name = NULL;
	ListCell   *lc;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("dolt_schema_diff needs two refs")));
	from_ref = text_to_cstring(PG_GETARG_TEXT_PP(0));
	to_ref = PG_ARGISNULL(1) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(1));
	if (PG_NARGS() > 2 && !PG_ARGISNULL(2))
		name = text_to_cstring(PG_GETARG_TEXT_PP(2));
	diff_refs("dolt_schema_diff", &from_ref, &to_ref, &name);
	if (!PROLLY_HAS(canonicalizer(), render))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("the database's canonical form can't be shown as statements")));
	InitMaterializedSRF(fcinfo, 0);
	foreach(lc, summary_rows(from_ref, to_ref, name, true))
	{
		SummaryRow *r = lfirst(lc);
		Datum		values[4];
		bool		nulls[4] = {0};
		char	   *from_sql;
		char	   *to_sql;

		if (r->not_covered)
			continue;
		from_sql = render_item(r->object, r->from_name, r->from_item, r->from_len);
		to_sql = render_item(r->object, r->to_name, r->to_item, r->to_len);
		if (strcmp(from_sql, to_sql) == 0)
			continue;
		values[0] = CStringGetTextDatum(r->from_name ? r->from_name : "");
		values[1] = CStringGetTextDatum(r->to_name ? r->to_name : "");
		values[2] = CStringGetTextDatum(from_sql);
		values[3] = CStringGetTextDatum(to_sql);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

/*
 * names ("name<tab>status") with table relid's status status, in place of
 * any it had (none for a table dropped)
 */
static List *
with_status_named(List *names, const char *name, const char *status)
{
	size_t		len = strlen(name);
	ListCell   *lc;

	foreach(lc, names)
	{
		char	   *n = lfirst(lc);

		/* (the status is after the last tab: a name may hold one) */
		if (strrchr(n, '\t') == n + len && strncmp(n, name, len) == 0)
			names = foreach_delete_current(names, lc);
	}
	return lappend(names, psprintf("%s\t%s", name, status));
}

static List *
with_status(List *names, Oid relid, const char *status)
{
	char	   *name = qualified_name(relid);

	return name ? with_status_named(names, name, status) : names;
}

/*
 * dolt_status(OUT table_name text, OUT staged bool, OUT status text)
 *	  returns setof record
 *
 * The tables changed since the session branch's head commit (before its
 * first, since the empty root: everything is new): rows changed
 * ("modified"), made ("new table"), dropped ("deleted") or, with a merge
 * in progress, with conflicts ("conflict", or "schema conflict", not
 * staged), or with constraint
 * violations ("constraint violation", not staged); the other objects
 * changed (schemas, routines with their argument types, views, types,
 * sequences), with the tables' statuses, as Doltgres lists them; and
 * objects a commit doesn't cover yet ("not covered", not staged).
 */
Datum
dolt_status(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	StatusSink	s = {{status_change}, NIL};
	List	   *names = NIL;
	ListCell   *lc;

	ProllyMergeState ms;
	List	   *conflicted = NIL;
	List	   *violated = NIL;

	InitMaterializedSRF(fcinfo, 0);
	foreach(lc, PROLLY_CALL(refs(), violations))
		violated = list_append_unique_oid(violated,
										  ((ProllyViolation *) lfirst(lc))->relid);

	/*
	 * Tables by name, so ones made or dropped since the head are too; a
	 * table with conflicts, as that.
	 */
	if (PROLLY_HAS(diff_merge(), tables))
	{
		PROLLY_CALL(diff_merge(), tables, "HEAD", "WORKING", status_table,
					&names);
		/* (a table with conflicts and violations shows its conflicts) */
		foreach(lc, violated)
			names = with_status(names, lfirst_oid(lc), "constraint violation");
		if (PROLLY_CALL(refs(), merge_state, &ms))
		{
			for (int i = 0; i < ms.ntables; i++)
				names = with_status(names, ms.tables[i], "conflict");
			for (int i = 0; i < ms.nschema; i++)
				names = with_status_named(names, ms.schema_tables[i],
										  "schema conflict");
			for (int i = 0; i < ms.nidentity; i++)
				names = with_status_named(names, ms.identity[i].name,
										  "identity conflict");
		}
	}
	else
	{
		PROLLY_CALL(diff_merge(), diff, "HEAD", "WORKING", InvalidOid, &s.sink);
		if (PROLLY_CALL(refs(), merge_state, &ms))
		{
			for (int i = 0; i < ms.ntables; i++)
			{
				conflicted = lappend_oid(conflicted, ms.tables[i]);
				s.relids = list_append_unique_oid(s.relids, ms.tables[i]);
			}
		}
		foreach(lc, violated)
			s.relids = list_append_unique_oid(s.relids, lfirst_oid(lc));
		foreach(lc, s.relids)
		{
			Oid			relid = lfirst_oid(lc);
			char	   *name = qualified_name(relid);

			if (name == NULL)
				continue;
			/* Name, then a tab and the status, to sort by name. */
			names = lappend(names,
							psprintf("%s\t%s", name,
									 list_member_oid(conflicted, relid) ? "conflict" :
									 list_member_oid(violated, relid) ?
									 "constraint violation" : "modified"));
		}
	}
	list_sort(names, status_cmp);
	foreach(lc, names)
	{
		Datum		values[3];
		bool		nulls[3] = {0};
		char	   *tab = strrchr(lfirst(lc), '\t');	/* (a name may hold one) */

		*tab = '\0';
		values[0] = CStringGetTextDatum(lfirst(lc));
		/* (a conflict, or an object a commit refuses, isn't committed) */
		values[1] = BoolGetDatum(strcmp(tab + 1, "conflict") != 0 &&
								 strcmp(tab + 1, "schema conflict") != 0 &&
								 strcmp(tab + 1, "constraint violation") != 0 &&
								 strcmp(tab + 1, "not covered") != 0);
		values[2] = CStringGetTextDatum(tab + 1);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

/* ------------------------------------------------------------------------
 * dolt_diff
 * ------------------------------------------------------------------------
 */

/* Decodes a table's tree values into rows of its row type. */
typedef struct Rows
{
	ProllyComponents c;
	ProllyKeying *keying;
	bool		keyless;
	TupleTableSlot *slot;
	ProllyTreeEnv env;			/* where values out of line are read */
	ProllyKeyingRefs refs;
	MemoryContext rowcxt;		/* a row's decoding, reset for the next */
	bool		described;		/* a version's, by its description: its
								 * values parsed as keyless says */
} Rows;

static void
rows_begin(Rows *r, Relation rel)
{
	prolly_components_for_relation(rel, &r->c);
	/* (a table's trees are its folds, rows as a rewrite has them) */
	r->keying = PROLLY_CALL(r->c.keying, open, rel, CurrentMemoryContext);
	r->keyless = PROLLY_CALL(r->c.keying, keyless, r->keying);
	prolly_folded_tree_env(&r->env);
	prolly_keying_use_env(&r->refs, r->c.keying, r->keying, r->c.tree, &r->env);
	r->slot = MakeSingleTupleTableSlot(RelationGetDescr(rel), &TTSOpsVirtual);
	r->rowcxt = AllocSetContextCreate(CurrentMemoryContext, "dolt rows",
									  ALLOCSET_DEFAULT_SIZES);
}

/* A canonical table item's data tree. */
static void
table_item_data(const char *item, int len, ProllyNodeRef *data)
{
	memset(data, 0, sizeof(*data));
	if (len < 1 || (uint8) item[0] > PROLLY_HASH_MAX_LEN || len < 1 + (uint8) item[0])
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("a canonical table item without its rows")));
	data->hash.len = (uint8) item[0];
	memcpy(data->hash.bytes, item + 1, data->hash.len);
}

/* The kinds of row changes a diff of a table's trees finds (bits by kind). */
static bool
note_change_kind(ProllyDiffKind kind, const ProllyKey *key,
				 const ProllyValue *before, const ProllyValue *after, void *arg)
{
	*(int *) arg |= 1 << kind;
	return true;
}

/* A table an integration changes rows of, and the privileges it needs. */
typedef struct IntegratedRows
{
	Oid			relid;
	AclMode		need;
	bool		any;			/* (one of need's privileges is enough) */
} IntegratedRows;

/*
 * In a writer mode, may the session's role integrate its own writer branch
 * mine (whose head is now head) into target, its base, without the
 * privileges of a head move there?  Only if every change the branch brings
 * since it and target last met is to the rows of a table the role may
 * write as the change needs: INSERT for rows added, DELETE for rows
 * removed, UPDATE for rows changed (of a table without a key, whose changed
 * rows are counts of copies, INSERT and DELETE).  A change to any
 * definition, or to another object, takes the head move's privileges; but
 * a sequence's value, which a write to a serial or identity column moves,
 * takes USAGE or UPDATE on it, as nextval() does.
 * *needs: the tables (and sequences) and privileges, checked on target's catalogs
 * (check_integrated_rows()), whose grants are the ones its rows take.
 */
static bool
writer_may_integrate(const char *mine, const char *target, const ProllyHash *head,
					 List **needs)
{
	char	   *writer;
	char	   *base;
	Oid			role;
	ProllyHash	at;
	ProllyHash	met;
	List	   *rows = NIL;
	ListCell   *lc;
	const ProllyTreeRoutine *tree = prolly_tree_lookup("cow");
	ProllyTreeEnv env;

	if (PROLLY_CALL(writers(), mode) == PROLLY_WRITERS_SHARED ||
		!writer_branch_name(mine) ||
		!PROLLY_CALL(writers(), writer_branch, mine, &writer, &base, &role) ||
		strcmp(base, target) != 0 || !role_exists(role) ||
		!has_privs_of_role(GetUserId(), role) ||
		!PROLLY_HAS(tree, diff_unordered))
		return false;
	if (!PROLLY_CALL(refs(), resolve, target, &at) ||
		!PROLLY_CALL(refs(), merge_base, head, &at, &met))
		return false;
	PROLLY_CALL(diff_merge(), summary, hash_hex(&met), hash_hex(head), false,
				collect_change, &rows);
	prolly_folded_tree_env(&env);
	foreach(lc, rows)
	{
		SummaryRow *r = lfirst(lc);
		ProllyNodeRef fd;
		ProllyNodeRef td;
		ProllyValue d;
		bool		keyed;
		int			kinds = 0;
		Oid			relid;
		AclMode		need = 0;

		if (r->object == 's' && !r->schema_change && !r->not_covered &&
			r->from_name != NULL && r->to_name != NULL &&
			strcmp(r->from_name, r->to_name) == 0)
		{
			IntegratedRows *ir;

			relid = RangeVarGetRelid(makeRangeVarFromNameList(stringToQualifiedNameList(r->to_name, NULL)),
									 NoLock, true);
			if (!OidIsValid(relid))
				return false;
			ir = palloc(sizeof(IntegratedRows));
			ir->relid = relid;
			ir->need = ACL_USAGE | ACL_UPDATE;
			ir->any = true;
			*needs = lappend(*needs, ir);
			continue;
		}
		if (r->object != PROLLY_OBJECT_TABLE || r->not_covered ||
			r->schema_change || r->from_name == NULL || r->to_name == NULL ||
			strcmp(r->from_name, r->to_name) != 0 ||
			r->from_item == NULL || r->to_item == NULL)
			return false;
		if (!r->data_change)
			continue;
		relid = RangeVarGetRelid(makeRangeVarFromNameList(stringToQualifiedNameList(r->to_name, NULL)),
								 NoLock, true);
		if (!OidIsValid(relid))
			return false;
		table_item_data(r->from_item, r->from_len, &fd);
		table_item_data(r->to_item, r->to_len, &td);
		PROLLY_CALL(tree, diff_unordered, &env, &fd, &td, note_change_kind, &kinds);
		d.data = (const uint8 *) r->to_item + 1 + td.hash.len;
		d.len = r->to_len - 1 - td.hash.len;
		(void) PROLLY_CALL(canonicalizer(), table_columns, &d, &keyed);
		if (kinds & (1 << PROLLY_DIFF_ADDED))
			need |= ACL_INSERT;
		if (kinds & (1 << PROLLY_DIFF_REMOVED))
			need |= ACL_DELETE;
		if (kinds & (1 << PROLLY_DIFF_MODIFIED))
			need |= keyed ? ACL_UPDATE : (ACL_INSERT | ACL_DELETE);
		if (kinds & (1 << PROLLY_DIFF_NOT_COVERED))
			return false;
		{
			IntegratedRows *ir = palloc(sizeof(IntegratedRows));

			ir->relid = relid;
			ir->need = need;
			ir->any = false;
			*needs = lappend(*needs, ir);
		}
	}
	return true;
}

/* writer_may_integrate()'s privileges, as target (the session's now) grants them */
static void
check_integrated_rows(const char *target, List *needs)
{
	ListCell   *lc;

	foreach(lc, needs)
	{
		IntegratedRows *ir = lfirst(lc);

		if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(ir->relid)) ||
			(ir->any ?
			 pg_class_aclmask(ir->relid, GetUserId(), ir->need, ACLMASK_ANY) == 0 :
			 pg_class_aclmask(ir->relid, GetUserId(), ir->need, ACLMASK_ALL) != ir->need))
			prolly_check_head_move(target);
	}
}

/*
 * rows_begin() of a version of a table only its commits describe (its
 * item, item bytes): its columns as described, read without a relation.
 * False if they can't be: a column's type isn't one this database has, or
 * the keying can't open rows so.
 */
static bool
rows_begin_described(Rows *r, const char *item, int len)
{
	ProllyValue v = {(const uint8 *) item, len};
	ProllyNodeRef data;
	ProllyValue d;
	List	   *cols;
	TupleDesc	desc;
	bool		keyed;
	int			i = 1;
	ListCell   *lc;

	memset(r, 0, sizeof(*r));
	prolly_history_components(&r->c);
	if (!PROLLY_HAS(r->c.keying, open_described) ||
		!PROLLY_HAS(r->c.keying, parse_value_as))
		return false;
	table_item_data(item, len, &data);
	d.data = v.data + 1 + data.hash.len;
	d.len = v.len - 1 - data.hash.len;
	cols = PROLLY_CALL(canonicalizer(), table_columns, &d, &keyed);
	desc = CreateTemplateTupleDesc(list_length(cols));
	foreach(lc, cols)
	{
		ProllyTableColumn *c = lfirst(lc);
		ErrorSaveContext escontext = {T_ErrorSaveContext};
		Oid			type;
		int32		typmod;

		if (!parseTypeString(c->type, &type, &typmod, (Node *) &escontext) ||
			!OidIsValid(type))
			return false;
		TupleDescInitEntry(desc, i++, c->name, type, typmod, 0);
	}
	TupleDescFinalize(desc);
	r->keying = PROLLY_CALL(r->c.keying, open_described, desc, CurrentMemoryContext);
	r->keyless = !keyed;
	r->described = true;
	prolly_folded_tree_env(&r->env);
	prolly_keying_use_env(&r->refs, r->c.keying, r->keying, r->c.tree, &r->env);
	r->slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	r->rowcxt = AllocSetContextCreate(CurrentMemoryContext, "dolt rows",
									  ALLOCSET_DEFAULT_SIZES);
	return true;
}

/* A tree value's row and copies, as the rows' version keyed them. */
static void
rows_parse(Rows *r, const ProllyValue *value, ProllyValue *row, uint64 *count)
{
	if (r->described)
		PROLLY_CALL(r->c.keying, parse_value_as, r->keying, r->keyless, value,
					row, count);
	else
		PROLLY_CALL(r->c.keying, parse_value, r->keying, value, row, count);
}

/*
 * Where a sink decodes the rows it's handed: what it decoded for the last
 * row is freed (the tuplestore has its own copies), so a diff of millions
 * of rows takes the memory of one.
 */
static MemoryContext
rows_next(Rows *r)
{
	ExecClearTuple(r->slot);
	MemoryContextReset(r->rowcxt);
	return MemoryContextSwitchTo(r->rowcxt);
}

static void
rows_end(Rows *r)
{
	ExecDropSingleTupleTableSlot(r->slot);
	PROLLY_CALL(r->c.keying, close, r->keying);
	MemoryContextDelete(r->rowcxt);
}

/*
 * The row a tree value holds, as a composite datum of its own (the slot
 * is reused for the next value); and its copies.
 */
/*
 * A version's row decoded into r->slot, its long values as the trees they
 * are where a hash pointer can name them (read only as a composite datum
 * of the row is made).
 */
static void
rows_slot(Rows *r, const ProllyValue *value, uint64 *count)
{
	ProllyValue row;

	rows_parse(r, value, &row, count);
	ExecClearTuple(r->slot);
	PROLLY_CALL(r->c.keying, decode_row, r->keying, &row, r->slot);
}

static Datum
rows_datum(Rows *r, const ProllyValue *value, uint64 *count)
{
	rows_slot(r, value, count);
	return ExecFetchSlotHeapTupleDatum(r->slot);
}

/*
 * rows_datum()'s row as a jsonb object instead: each column's value as
 * to_jsonb() has it, by the column's name in the rows' version (#612).
 */
static Datum
rows_jsonb(Rows *r, const ProllyValue *value, uint64 *count)
{
	TupleDesc	desc = r->slot->tts_tupleDescriptor;
	Datum	   *args = palloc_array(Datum, Max(2 * desc->natts, 1));
	bool	   *nulls = palloc_array(bool, Max(2 * desc->natts, 1));
	Oid		   *types = palloc_array(Oid, Max(2 * desc->natts, 1));
	int			n = 0;
	ProllyValue row;

	rows_parse(r, value, &row, count);
	ExecClearTuple(r->slot);
	PROLLY_CALL(r->c.keying, decode_row, r->keying, &row, r->slot);
	slot_getallattrs(r->slot);
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (att->attisdropped)
			continue;
		args[n] = CStringGetTextDatum(NameStr(att->attname));
		nulls[n] = false;
		types[n++] = TEXTOID;
		args[n] = r->slot->tts_values[i];
		nulls[n] = r->slot->tts_isnull[i];
		types[n++] = att->atttypid;
	}
	return jsonb_build_object_worker(n, args, nulls, types, false, false);
}

/* A row as a function returns it: of the table's row type, or as jsonb. */
static Datum
rows_value(Rows *r, bool json, const ProllyValue *value, uint64 *count)
{
	return json ? rows_jsonb(r, value, count) : rows_datum(r, value, count);
}

/*
 * Whether the user may read relid's history, or what refuses them: they
 * need mode on the table, and row security mustn't apply to them, as
 * history holds every row whatever the table's policies say.  noError as
 * check_enable_rls() takes it.  Asked before a commit's rows are read,
 * and before the planner estimates them from a commit's tree.
 */
typedef enum HistoryAccess
{
	HISTORY_READABLE,
	HISTORY_NO_PRIVILEGE,
	HISTORY_ROW_SECURITY,
} HistoryAccess;

static HistoryAccess
history_access(Oid relid, AclMode mode, bool noError)
{
	if (pg_class_aclcheck(relid, GetUserId(), mode) != ACLCHECK_OK)
		return HISTORY_NO_PRIVILEGE;
	if (check_enable_rls(relid, InvalidOid, noError) == RLS_ENABLED)
		return HISTORY_ROW_SECURITY;
	return HISTORY_READABLE;
}

/*
 * A reader of history whom row security applies to is refused, rather
 * than shown rows the policies hide.
 */
static void
history_rls_error(Oid relid, const char *fn)
{
	ereport(ERROR,
			(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
			 errmsg("%s can't read \"%s\": row security applies to you",
					fn, get_rel_name(relid)),
			 errdetail("Other commits' rows aren't filtered by its policies."),
			 errhint("The table's owner, or a role that bypasses row security, can read them.")));
}

/* row security's refusal of history, as an error */
static void
check_history_rls(Oid relid, const char *fn)
{
	if (check_enable_rls(relid, InvalidOid, false) == RLS_ENABLED)
		history_rls_error(relid, fn);
}

/* history_access(), its refusals as errors */
static void
check_history_access(Oid relid, const char *fn, AclMode mode)
{
	switch (history_access(relid, mode, false))
	{
		case HISTORY_READABLE:
			break;
		case HISTORY_NO_PRIVILEGE:
			aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE, get_rel_name(relid));
			break;
		case HISTORY_ROW_SECURITY:
			history_rls_error(relid, fn);
			break;
	}
}

/*
 * The prolly table whose row type is type, opened, if the user has mode
 * on it (and row security doesn't apply to them).
 */
static Relation
open_row_type_table(Oid relid, const char *fn, AclMode mode)
{
	Relation	rel;

	if (!OidIsValid(relid) || get_rel_relkind(relid) != RELKIND_RELATION)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s needs a table's row type, as in NULL::tablename", fn)));
	check_history_access(relid, fn, mode);
	rel = table_open(relid, AccessShareLock);
	if (rel->rd_rel->relam != PROLLY_TABLE_AM_OID)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a prolly table", RelationGetRelationName(rel))));
	return rel;
}

typedef struct RowDiffSink
{
	ProllyDiffSink sink;		/* must be first */
	ReturnSetInfo *rsinfo;
	Rows		rows;
	bool		json;			/* (a table by name) rows as jsonb, the to
								 * side's decoded by to_rows */
	Rows		to_rows;

	/*
	 * dolt_diff_table()'s: each row with the commits it is between, and
	 * their dates (to_commit NULL: dolt_diff()'s row, without them)
	 */
	const char *to_commit;
	const char *from_commit;
	TimestampTz to_date;
	TimestampTz from_date;
	bool		to_dated;
	bool		from_dated;
	List	   *lineages;		/* (a table by name) check_named_rows()' */
} RowDiffSink;

static void
put_diff(RowDiffSink *s, const char *kind, Datum from, bool from_null,
		 Datum to, bool to_null, uint64 times)
{
	Datum		values[7];
	bool		nulls[7] = {0};

	if (s->to_commit)
	{
		values[0] = to;
		nulls[0] = to_null;
		values[1] = from;
		nulls[1] = from_null;
		values[2] = CStringGetTextDatum(s->to_commit);
		values[3] = s->from_commit ? CStringGetTextDatum(s->from_commit) : (Datum) 0;
		nulls[3] = s->from_commit == NULL;
		values[4] = TimestampTzGetDatum(s->to_date);
		nulls[4] = !s->to_dated;
		values[5] = TimestampTzGetDatum(s->from_date);
		nulls[5] = !s->from_dated;
		values[6] = CStringGetTextDatum(kind);
	}
	else
	{
		values[0] = CStringGetTextDatum(kind);
		values[1] = from;
		nulls[1] = from_null;
		values[2] = to;
		nulls[2] = to_null;
	}
	for (uint64 i = 0; i < times; i++)
		tuplestore_putvalues(s->rsinfo->setResult, s->rsinfo->setDesc, values,
							 nulls);
}

static void
row_change(ProllyDiffSink *self, Oid relid, ProllyDiffKind kind,
		   const ProllyKey *key, const ProllyValue *before,
		   const ProllyValue *after)
{
	RowDiffSink *s = (RowDiffSink *) self;
	uint64		nbefore = 0;
	uint64		nafter = 0;
	Datum		from = (Datum) 0;
	Datum		to = (Datum) 0;
	Rows	   *to_rows = s->json ? &s->to_rows : &s->rows;
	MemoryContext old = rows_next(&s->rows);

	if (s->json)
		(void) rows_next(to_rows);
	if (before)
		from = rows_value(&s->rows, s->json, before, &nbefore);
	if (after)
		to = rows_value(to_rows, s->json, after, &nafter);

	/* A keyless row's copies: each one added or removed is a row. */
	if (s->rows.keyless || to_rows->keyless)
	{
		if (nafter > nbefore)
			put_diff(s, "added", (Datum) 0, true, to, false, nafter - nbefore);
		else
			put_diff(s, "removed", from, false, (Datum) 0, true,
					 nbefore - nafter);
	}
	else if (kind == PROLLY_DIFF_ADDED)
		put_diff(s, "added", (Datum) 0, true, to, false, 1);
	else if (kind == PROLLY_DIFF_REMOVED)
		put_diff(s, "removed", from, false, (Datum) 0, true, 1);
	else
		put_diff(s, "modified", from, false, to, false, 1);
	MemoryContextSwitchTo(old);
}

static void check_typed_diff(Oid relid, const char *from_ref,
							 const char *to_ref, List **lineages,
							 const char *fn);

/*
 * dolt_diff(from_ref text, to_ref text, tbl anyelement,
 *			 OUT diff_type text, OUT from_row anyelement,
 *			 OUT to_row anyelement) returns setof record
 *
 * The rows of tbl's table (its row type, as in NULL::t) that differ
 * between two refs (WORKING: the branch's working set), each as it was and
 * as it is.  Doltgres's dolt_diff() spreads the two rows into from_ and to_
 * columns; here they are values of the table's row type.
 */
Datum
dolt_diff(PG_FUNCTION_ARGS)
{
	char	   *from_ref;
	char	   *to_ref;
	Oid			type = get_fn_expr_argtype(fcinfo->flinfo, 2);
	Oid			relid = OidIsValid(type) ? get_typ_typrelid(type) : InvalidOid;
	Relation	rel;
	RowDiffSink s;

	/* (not strict, for tbl's NULL::t: its refs checked here) */
	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("dolt_diff's refs can't be null")));
	from_ref = text_to_cstring(PG_GETARG_TEXT_PP(0));
	to_ref = text_to_cstring(PG_GETARG_TEXT_PP(1));
	InitMaterializedSRF(fcinfo, 0);
	rel = open_row_type_table(relid, "dolt_diff", ACL_SELECT);
	memset(&s, 0, sizeof(s));
	s.sink.change = row_change;
	s.rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	check_typed_diff(relid, from_ref, to_ref, &s.lineages, "dolt_diff");
	rows_begin(&s.rows, rel);
	PROLLY_CALL(diff_merge(), diff, from_ref, to_ref, relid, &s.sink);
	rows_end(&s.rows);
	table_close(rel, AccessShareLock);
	return (Datum) 0;
}

/*
 * The version of the table name names that a summary's changes (rows) have,
 * name read as summary_rows() reads it: the table it names on the to side
 * first, then on the from side (a table renamed, and another made with its
 * old name).  NULL if none (its rows and definition the same on both, or
 * neither has it).
 */
static SummaryRow *
named_table(List *rows, const char *name)
{
	char	   *want = row_name_wanted(rows, name);
	SummaryRow *found = NULL;
	ListCell   *lc;

	if (want == NULL)
		return NULL;
	foreach(lc, rows)
	{
		SummaryRow *r = lfirst(lc);

		if (r->object != PROLLY_OBJECT_TABLE || r->not_covered)
			continue;
		if (r->to_name && strcmp(r->to_name, want) == 0)
			return r;
		if (r->from_name && strcmp(r->from_name, want) == 0)
			found = r;
	}
	return found;
}

/*
 * A table of the session's history: the versions that are it, as the
 * commits this database made record the relations they were made from
 * (refs' commit_relations()).  A commit holds tables by name and identity
 * alone, and a table made again keeps its name's identity, so its names
 * can't tell the table from another of its name; its record can.  From the
 * working set back through the head, each commit's parents, a step is
 * taken only where both commits were made from the table's relation (its
 * name at each, a rename followed, as the record has it).  A commit without
 * a record (made elsewhere: fetched, cloned, pushed here) is no version of
 * it.  Walked as far as a question needs (lineage_has()), each commit's
 * record read once.
 */
typedef struct LineageStep
{
	char		key[PROLLY_HASH_MAX_LEN * 2 + 1];	/* a commit's hex, or
													 * "WORKING" */
	char	   *name;			/* the table's name there */
} LineageStep;

typedef struct NameLineage
{
	char	   *now;			/* the table's name now */
	Oid			relid;			/* and its relation */
	HTAB	   *steps;			/* LineageStep, each version reached */
	List	   *queue;			/* (ProllyHash *, its name) pairs, to walk */
	HTAB	   *apart;			/* commits a walk back from found apart */
	bool		started;
} NameLineage;

/*
 * The name l's table had in commit, as its record has it; NULL if the
 * commit wasn't made from the table's relation, or has no record.
 */
static char *
made_from(NameLineage *l, const ProllyHash *commit)
{
	List	   *relations;
	ListCell   *lc;

	if (!OidIsValid(l->relid) || !PROLLY_HAS(refs(), commit_relations) ||
		!PROLLY_CALL(refs(), commit_relations, commit, &relations))
		return NULL;
	foreach(lc, relations)
	{
		ProllyCommitRelation *r = lfirst(lc);

		if (r->relid == l->relid &&
			(r->relkind == RELKIND_RELATION || r->relkind == RELKIND_PARTITIONED_TABLE))
			return r->name;
	}
	return NULL;
}

static void
lineage_enter(NameLineage *l, const char *key, const char *name)
{
	LineageStep *st;
	bool		found;

	st = hash_search(l->steps, key, HASH_ENTER, &found);
	if (!found)
		st->name = pstrdup(name);
}

/* commit's parents made from l's relation, each with its name there, to queue */
static List *
parents_made_from(NameLineage *l, const ProllyHash *commit, List *queue,
				  HTAB *seen)
{
	ProllyCommitInfo info;
	char	   *hex = hash_hex(commit);

	if (!PROLLY_CALL(refs(), commit_get, commit, &info))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly commit %s is missing", hex)));
	for (int i = 0; i < info.nparents; i++)
	{
		char	   *name;

		if (hash_search(seen, hash_hex(&info.parents[i]), HASH_FIND, NULL) != NULL)
			continue;
		if ((name = made_from(l, &info.parents[i])) != NULL)
			queue = lappend(lappend(queue, copy_hash(&info.parents[i])), name);
	}
	return queue;
}

/*
 * Is the version ref has (a commit, or the working set) of the table named
 * name l's table?
 */
static bool
lineage_has(NameLineage *l, const char *ref, const char *name)
{
	char		key[PROLLY_HASH_MAX_LEN * 2 + 1];
	ProllyHash	h;
	LineageStep *st;

	if (ref == NULL)
		return false;
	if (pg_strcasecmp(ref, "WORKING") == 0 || pg_strcasecmp(ref, "STAGED") == 0)
		strlcpy(key, "WORKING", sizeof(key));
	else if (PROLLY_CALL(refs(), resolve, ref, &h))
		strlcpy(key, hash_hex(&h), sizeof(key));
	else
		return false;

	if (!l->started)
	{
		char	   *at_name;

		l->started = true;
		lineage_enter(l, "WORKING", l->now);
		/* (the head's version is the table's if the head was made of it) */
		if (PROLLY_CALL(refs(), resolve, "HEAD", &h) &&
			(at_name = made_from(l, &h)) != NULL)
			l->queue = lappend(lappend(l->queue, copy_hash(&h)), at_name);
	}
	while ((st = hash_search(l->steps, key, HASH_FIND, NULL)) == NULL &&
		   l->queue != NIL)
	{
		ProllyHash *at = linitial(l->queue);
		char	   *at_name = lsecond(l->queue);
		char	   *hex = hash_hex(at);

		CHECK_FOR_INTERRUPTS();
		l->queue = list_delete_first(list_delete_first(l->queue));
		if (hash_search(l->steps, hex, HASH_FIND, NULL) != NULL)
			continue;
		lineage_enter(l, hex, at_name);
		l->queue = parents_made_from(l, at, l->queue, l->steps);
	}
	return st != NULL && strcmp(st->name, name) == 0;
}

/* a commit a walk back from another ref reached, and how */
typedef struct ReachStep
{
	char		key[PROLLY_HASH_MAX_LEN * 2 + 1];	/* the commit's hex */
	char		child[PROLLY_HASH_MAX_LEN * 2 + 1]; /* the one it was reached
													 * from ("": the ref) */
	char	   *name;			/* the table's name there */
} ReachStep;

/*
 * Is the version ref has of the table named name l's table, in ref's
 * history if not the session's?  One of the lineage's versions
 * (lineage_has()), or a commit made from the table's relation whose walk
 * back, by such commits, meets one of them: another branch's commit, made
 * from the history this table shares.  A walk that meets the lineage adds
 * the path it took to it, and one that meets none marks every commit it
 * saw apart, so no later walk takes either again: each commit's record is
 * read about once a lineage.
 */
static bool
lineage_reaches(NameLineage *l, const char *ref, const char *name)
{
	ProllyHash	h;
	List	   *queue = NIL;
	HTAB	   *seen;
	HASHCTL		ctl = {0};
	ReachStep  *found = NULL;
	ReachStep  *rs;
	char	   *at_name;
	HASH_SEQ_STATUS hs;

	if (lineage_has(l, ref, name))
		return true;
	if (ref == NULL || pg_strcasecmp(ref, "WORKING") == 0 ||
		pg_strcasecmp(ref, "STAGED") == 0 ||
		!PROLLY_CALL(refs(), resolve, ref, &h) ||
		(at_name = made_from(l, &h)) == NULL ||
		strcmp(at_name, name) != 0)
		return false;
	ctl.keysize = PROLLY_HASH_MAX_LEN * 2 + 1;
	ctl.entrysize = PROLLY_HASH_MAX_LEN * 2 + 1;
	ctl.hcxt = CurrentMemoryContext;
	if (l->apart == NULL)
		l->apart = hash_create("prolly lineage apart", 64, &ctl,
							   HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);
	ctl.entrysize = sizeof(ReachStep);
	seen = hash_create("prolly lineage reached", 64, &ctl,
					   HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);
	rs = hash_search(seen, hash_hex(&h), HASH_ENTER, NULL);
	rs->child[0] = '\0';
	rs->name = at_name;
	queue = lappend(queue, copy_hash(&h));
	while (queue != NIL && found == NULL)
	{
		ProllyHash *at = linitial(queue);
		char	   *hex = hash_hex(at);
		ProllyCommitInfo info;

		CHECK_FOR_INTERRUPTS();
		queue = list_delete_first(queue);
		rs = hash_search(seen, hex, HASH_FIND, NULL);
		if (lineage_has(l, hex, rs->name))
		{
			found = rs;
			break;
		}
		/* (a version of the lineage of another name: another table's) */
		if (hash_search(l->steps, hex, HASH_FIND, NULL) != NULL)
			continue;
		if (!PROLLY_CALL(refs(), commit_get, at, &info))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("prolly commit %s is missing", hex)));
		for (int i = 0; i < info.nparents; i++)
		{
			char	   *phex = hash_hex(&info.parents[i]);
			char	   *pname;
			bool		dup;
			ReachStep  *ps;

			if (hash_search(l->apart, phex, HASH_FIND, NULL) != NULL ||
				hash_search(seen, phex, HASH_FIND, NULL) != NULL ||
				(pname = made_from(l, &info.parents[i])) == NULL)
				continue;
			ps = hash_search(seen, phex, HASH_ENTER, &dup);
			strlcpy(ps->child, hex, sizeof(ps->child));
			ps->name = pname;
			queue = lappend(queue, copy_hash(&info.parents[i]));
		}
	}
	if (found != NULL)
	{
		/* (the path from the ref to the lineage joins it) */
		for (rs = found; rs->child[0] != '\0';)
		{
			rs = hash_search(seen, rs->child, HASH_FIND, NULL);
			lineage_enter(l, rs->key, rs->name);
		}
	}
	else
	{
		hash_seq_init(&hs, seen);
		while ((rs = hash_seq_search(&hs)) != NULL)
			(void) hash_search(l->apart, rs->key, HASH_ENTER, NULL);
	}
	hash_destroy(seen);
	return found != NULL;
}

/* The lineage of the table here named now: from lineages, or begun. */
static NameLineage *
name_lineage(List **lineages, const char *now, Oid relid)
{
	NameLineage *l;
	HASHCTL		ctl = {0};
	ListCell   *lc;

	foreach(lc, *lineages)
	{
		l = lfirst(lc);
		if (strcmp(l->now, now) == 0)
			return l;
	}
	l = palloc0(sizeof(NameLineage));
	l->now = pstrdup(now);
	l->relid = relid;
	ctl.keysize = PROLLY_HASH_MAX_LEN * 2 + 1;
	ctl.entrysize = sizeof(LineageStep);
	ctl.hcxt = CurrentMemoryContext;
	l->steps = hash_create("prolly table lineage", 64, &ctl,
						   HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);
	*lineages = lappend(*lineages, l);
	return l;
}

/* Does the user read every version, whatever its grants? */
static bool
reads_all_history(void)
{
	return object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId()) ||
		has_privs_of_role(GetUserId(), ROLE_PG_READ_ALL_DATA);
}

/* A history read the version control can't check, refused. */
static void
history_unchecked(const char *fn)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("%s can't check which versions are the table's", fn),
			 errdetail("The database's version control lists no changes to follow a table by."),
			 errhint("The database's owner, or a member of pg_read_all_data, can read it.")));
}

/*
 * May the user read a table's rows as its versions (at from_ref and
 * to_ref) have them, by name?  Where the session has a table of either
 * version's name, SELECT on it (row security not applying to them, as for
 * the table's row type), a grant that covers each version that is that
 * table (name_lineage()).  A version no table here is (dropped since, or
 * one of its name made after it) has no grant to check: only the
 * database's owner or a member of pg_read_all_data may read it.  Refused
 * as an error, or (noError) false.
 */
static bool
named_rows_allowed(const SummaryRow *r, const char *from_ref,
				   const char *to_ref, List **lineages, const char *fn,
				   bool noError)
{
	const char *names[2] = {r->to_name, r->from_name};
	bool		from_ok = r->from_item == NULL;
	bool		to_ok = r->to_item == NULL;
	bool		some = false;
	bool		reads_all;
	const char *name;

	/*
	 * (one who may read every version needs no lineage: walking it reads
	 * what the working set changed since the head, a fold of its rows,
	 * whatever the refs diffed)
	 */
	reads_all = reads_all_history();
	for (int i = 0; i < 2; i++)
	{
		Oid			relid;
		AclResult	aclresult;
		NameLineage *l;

		if (names[i] == NULL)
			continue;
		relid = RangeVarGetRelid(makeRangeVarFromNameList(stringToQualifiedNameList(names[i], NULL)),
								 NoLock, true);
		if (!OidIsValid(relid) || get_rel_relkind(relid) != RELKIND_RELATION)
			continue;
		aclresult = pg_class_aclcheck(relid, GetUserId(), ACL_SELECT);
		if (aclresult != ACLCHECK_OK)
		{
			if (noError)
				return false;
			aclcheck_error(aclresult, OBJECT_TABLE, get_rel_name(relid));
		}
		if (noError && check_enable_rls(relid, InvalidOid, true) == RLS_ENABLED)
			return false;
		check_history_rls(relid, fn);
		some = true;
		if (reads_all)
			continue;
		l = name_lineage(lineages, names[i], relid);
		if (!from_ok)
			from_ok = lineage_reaches(l, from_ref, r->from_name);
		if (!to_ok)
			to_ok = lineage_reaches(l, to_ref, r->to_name);
	}
	if ((from_ok && to_ok) || reads_all)
		return true;
	if (noError)
		return false;
	name = !from_ok ? r->from_name : r->to_name;
	if (some)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("%s can't read \"%s\": the table of that name here isn't the one a version holds",
						fn, name),
				 errdetail("The name was without a table since that version, or the branch's head was moved to it since its last commit, so the table here may be another."),
				 errhint("The database's owner, or a member of pg_read_all_data, can read it.")));
	ereport(ERROR,
			(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
			 errmsg("%s can't read \"%s\": no table of that name is here to grant it",
					fn, name),
			 errhint("The database's owner, or a member of pg_read_all_data, can read it.")));
	return false;				/* keep the compiler quiet */
}

static void
check_named_rows(const SummaryRow *r, const char *from_ref, const char *to_ref,
				 List **lineages, const char *fn)
{
	(void) named_rows_allowed(r, from_ref, to_ref, lineages, fn, false);
}

/*
 * A history read of relid's table by its row type, as by name: a diff of
 * it between two refs (from_ref NULL: from nothing) reads the versions
 * the refs' changes pair with it, which check_named_rows() allows by the
 * same rule as a read by name.  So a version of another table of its name
 * (one dropped, and this one made again with its name) isn't read on this
 * table's grants.
 */
static void
check_typed_diff(Oid relid, const char *from_ref, const char *to_ref,
				 List **lineages, const char *fn)
{
	List	   *rows = NIL;
	SummaryRow *r;
	char	   *name;

	if (reads_all_history())
		return;
	if (!PROLLY_HAS(diff_merge(), summary) || (name = qualified_name(relid)) == NULL)
		history_unchecked(fn);
	PROLLY_CALL(diff_merge(), summary, from_ref, to_ref, false, collect_change, &rows);
	r = named_table(rows, name);
	if (r != NULL)
		check_named_rows(r, from_ref, to_ref, lineages, fn);
}

/*
 * May the user read relid's table as ref has it (dolt_at(),
 * dolt_history_table(), which read ref's table of its name)?  Not if that
 * is another table's version (check_named_rows()'s rule), unless the user
 * reads every version: an error, or (noError) false.
 */
static bool
typed_at_allowed(Oid relid, const char *ref, List **lineages, const char *fn,
				 bool noError)
{
	List	   *rows = NIL;
	SummaryRow *r;
	char	   *name;

	if (reads_all_history())
		return true;
	if (!PROLLY_HAS(diff_merge(), summary) || (name = qualified_name(relid)) == NULL)
	{
		if (noError)
			return false;
		history_unchecked(fn);
	}
	if (lineage_reaches(name_lineage(lineages, name, relid), ref, name))
		return true;
	/* (no table of the name there: nothing of another's is read) */
	PROLLY_CALL(diff_merge(), summary, NULL, ref, false, collect_change, &rows);
	r = named_table(rows, name);
	if (r == NULL || r->to_item == NULL)
		return true;
	return named_rows_allowed(r, NULL, ref, lineages, fn, noError);
}

static void
check_typed_at(Oid relid, const char *ref, List **lineages, const char *fn)
{
	(void) typed_at_allowed(relid, ref, lineages, fn, false);
}

/*
 * The versions and tables lineage_has() found alike, this backend's, as
 * of a head: a lineage is walked back from the working set, so moving the
 * head (a reset to before the table was made) asks again.  Emptied when
 * full.
 */
#define LINEAGES_KNOWN_MAX	1024
static HTAB *lineages_known = NULL;

/*
 * check_named_rows() of one version, commit, of the table named name.
 */
void
prolly_check_history_by_name(const char *name, const char *commit,
							 bool lineage, const char *fn)
{
	Oid			relid;
	AclResult	aclresult;
	List	   *lineages = NIL;

	if (object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId()) ||
		has_privs_of_role(GetUserId(), ROLE_PG_READ_ALL_DATA))
		return;
	relid = RangeVarGetRelid(makeRangeVarFromNameList(stringToQualifiedNameList(name, NULL)),
							 NoLock, true);
	if (!OidIsValid(relid) || get_rel_relkind(relid) != RELKIND_RELATION)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("%s can't read \"%s\": no table of that name is here to grant it",
						fn, name),
				 errhint("The database's owner, or a member of pg_read_all_data, can read it.")));
	aclresult = pg_class_aclcheck(relid, GetUserId(), ACL_SELECT);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_TABLE, get_rel_name(relid));
	check_history_rls(relid, fn);
	if (!lineage)
		return;

	/*
	 * (a version and the table here it was found to be, remembered: the
	 * table dropped and made again, or another renamed to the name, is
	 * another relation, so asks again)
	 */
	{
		ProllyHash	head;
		char		key[NAMEDATALEN * 4 + 4 * PROLLY_HASH_MAX_LEN];
		bool		found;

		if (!PROLLY_CALL(refs(), resolve, "HEAD", &head))
			head.len = 0;
		snprintf(key, sizeof(key), "%u %s %s %s", relid, commit,
				 head.len ? hash_hex(&head) : "", name);
		if (lineages_known == NULL)
		{
			HASHCTL		ctl = {0};

			ctl.keysize = sizeof(key);
			ctl.entrysize = sizeof(key);
			lineages_known = hash_create("prolly known lineages",
										 LINEAGES_KNOWN_MAX, &ctl,
										 HASH_ELEM | HASH_STRINGS);
		}
		if (hash_search(lineages_known, key, HASH_FIND, NULL))
			return;
		if (!lineage_reaches(name_lineage(&lineages, name, relid), commit, name))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("%s can't read \"%s\": the table of that name here isn't the one a version holds",
							fn, name),
					 errdetail("The name was without a table since that version, so the table here is another."),
					 errhint("The database's owner, or a member of pg_read_all_data, can read it.")));
		if (hash_get_num_entries(lineages_known) >= LINEAGES_KNOWN_MAX)
		{
			hash_destroy(lineages_known);
			lineages_known = NULL;
			return;
		}
		(void) hash_search(lineages_known, key, HASH_ENTER, &found);
	}
}

/*
 * A reader of a table's history, version by version: who may read it
 * (checked once) and the table's lineage (walked once, as far as the
 * versions asked of need).
 */
struct ProllyHistoryReader
{
	char	   *name;			/* the table's name here */
	Oid			relid;			/* the table here */
	bool		reads_all;		/* the database's owner, or pg_read_all_data */
	MemoryContext cxt;			/* its lineage's, as long as the reader */
	List	   *lineages;
	const char *fn;
};

ProllyHistoryReader *
prolly_history_reader_begin(const char *name, const char *fn)
{
	ProllyHistoryReader *r = palloc0(sizeof(ProllyHistoryReader));
	Oid			relid;
	AclResult	aclresult;

	r->name = pstrdup(name);
	r->fn = fn;
	r->cxt = CurrentMemoryContext;
	r->reads_all = object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId()) ||
		has_privs_of_role(GetUserId(), ROLE_PG_READ_ALL_DATA);
	if (r->reads_all)
		return r;
	relid = RangeVarGetRelid(makeRangeVarFromNameList(stringToQualifiedNameList(name, NULL)),
							 NoLock, true);
	if (!OidIsValid(relid) || get_rel_relkind(relid) != RELKIND_RELATION)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("%s can't read \"%s\": no table of that name is here to grant it",
						fn, name),
				 errhint("The database's owner, or a member of pg_read_all_data, can read it.")));
	aclresult = pg_class_aclcheck(relid, GetUserId(), ACL_SELECT);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_TABLE, get_rel_name(relid));
	check_history_rls(relid, fn);
	r->relid = relid;
	return r;
}

/*
 * Refuse reading the version commit (hex) has of the reader's table, by
 * the name it has there, unless it is the table here (its lineage reaches
 * back to it), as dolt_history_table() by name refuses it.
 */
void
prolly_history_reader_check(ProllyHistoryReader *r, const char *commit,
							const char *version_name)
{
	MemoryContext old;
	bool		has;

	if (r->reads_all)
		return;
	/* (the lineage, walked as far as asked, kept for the next commit) */
	old = MemoryContextSwitchTo(r->cxt);
	has = lineage_reaches(name_lineage(&r->lineages, r->name, r->relid),
						  commit, version_name);
	MemoryContextSwitchTo(old);
	if (!has)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("%s can't read \"%s\": the table of that name here isn't the one a version holds",
						r->fn, version_name),
				 errdetail("The name was without a table since commit %s, so the table here is another.",
						   commit),
				 errhint("The database's owner, or a member of pg_read_all_data, can read it.")));
}

/* rows_begin_described() of a table's version, or refused. */
static void
rows_begin_named(Rows *r, const char *item, int len, const char *name,
				 const char *fn)
{
	if (!rows_begin_described(r, item, len))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("%s can't read \"%s\" as a version describes it", fn, name),
				 errdetail("One of its columns' types isn't one this database has now.")));
}

/* A change an unordered diff finds, to a RowDiffSink. */
static bool
row_tree_change(ProllyDiffKind kind, const ProllyKey *key,
				const ProllyValue *before, const ProllyValue *after, void *arg)
{
	row_change(arg, InvalidOid, kind, key, before, after);
	return true;
}

/*
 * The rows of the table name names (as named_table() finds it) that differ
 * between two refs (from_ref NULL: the empty root), to s as jsonb, each
 * decoded as its own version describes the table (#612): read from the
 * versions' trees, so a table the session hasn't (dropped since, renamed)
 * is read too; matched by their keys' bytes, as no relation orders them (a
 * row whose key changed form between the versions, a primary key made or
 * dropped, is removed and added).  False if neither version has it
 * changed.
 */
static bool
diff_by_name(RowDiffSink *s, const char *from_ref, const char *to_ref,
			 const char *name, const char *fn)
{
	const ProllyTreeRoutine *tree = prolly_tree_lookup("cow");
	List	   *rows = NIL;
	SummaryRow *r;
	const char *fitem;
	const char *titem;
	int			flen;
	int			tlen;
	ProllyNodeRef fd;
	ProllyNodeRef td;

	if (!PROLLY_HAS(diff_merge(), summary) || !PROLLY_HAS(tree, diff_unordered))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("the database's version control can't read a table's rows by its name")));
	PROLLY_CALL(diff_merge(), summary, from_ref, to_ref, false, collect_change, &rows);
	r = named_table(rows, name);
	if (r == NULL)
		return false;
	check_named_rows(r, from_ref, to_ref, &s->lineages, fn);

	/* (a side without the table: the empty tree, read as the other's) */
	fitem = r->from_item ? r->from_item : r->to_item;
	flen = r->from_item ? r->from_len : r->to_len;
	titem = r->to_item ? r->to_item : r->from_item;
	tlen = r->to_item ? r->to_len : r->from_len;
	rows_begin_named(&s->rows, fitem, flen, name, fn);
	rows_begin_named(&s->to_rows, titem, tlen, name, fn);
	table_item_data(fitem, flen, &fd);
	table_item_data(titem, tlen, &td);
	if (r->from_item == NULL || r->to_item == NULL)
	{
		ProllyNodeRef empty;

		PROLLY_CALL(tree, build_finish,
					PROLLY_CALL(tree, build_begin, &s->rows.env, CurrentMemoryContext),
					&empty);
		if (r->from_item == NULL)
			fd = empty;
		else
			td = empty;
	}
	PROLLY_CALL(tree, diff_unordered, &s->rows.env, &fd, &td, row_tree_change, s);
	rows_end(&s->rows);
	rows_end(&s->to_rows);
	return true;
}

/*
 * dolt_diff(from_ref text, to_ref text, table_name text,
 *			 OUT diff_type text, OUT from_row jsonb, OUT to_row jsonb)
 *	  returns setof record
 *
 * dolt_diff() of a table named by its name, as Dolt's takes it, rather
 * than by its row type: so a table the session hasn't, dropped since or
 * renamed (by the name a version has), can be read (#612).  Each row is a
 * jsonb object of its columns, decoded as its own version defines the
 * table; a name neither version has is refused (as dolt_diff_stat's).
 */
Datum
dolt_diff_by_name(PG_FUNCTION_ARGS)
{
	char	   *from_ref;
	char	   *to_ref;
	char	   *name;
	RowDiffSink s;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("dolt_diff's refs and table name can't be null")));
	from_ref = text_to_cstring(PG_GETARG_TEXT_PP(0));
	to_ref = text_to_cstring(PG_GETARG_TEXT_PP(1));
	name = text_to_cstring(PG_GETARG_TEXT_PP(2));
	InitMaterializedSRF(fcinfo, 0);
	memset(&s, 0, sizeof(s));
	s.sink.change = row_change;
	s.rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	s.json = true;
	if (!diff_by_name(&s, from_ref, to_ref, name, "dolt_diff") &&
		!version_has_table(from_ref, name) && !version_has_table(to_ref, name))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("table \"%s\" is in neither version", name)));
	return (Datum) 0;
}

/*
 * A step of a table's history: its rows that differ between two refs, by
 * its relation (relid), or by name.
 */
static void
diff_step(RowDiffSink *s, Oid relid, const char *name, const char *from_ref,
		  const char *to_ref)
{
	if (name)
		(void) diff_by_name(s, from_ref, to_ref, name, "dolt_diff_table");
	else
	{
		check_typed_diff(relid, from_ref, to_ref, &s->lineages, "dolt_diff_table");
		PROLLY_CALL(diff_merge(), diff, from_ref, to_ref, relid, &s->sink);
	}
}

/*
 * dolt_diff_table()'s walk: from to_ref (NULL: the working set's changes
 * first) back along first parents to from_ref, each step to diff_step().
 */
static void
diff_history(RowDiffSink *s, Oid relid, const char *name,
			 const char *from_ref, const char *to_ref)
{
	ProllyHash	stop;
	bool		stopping = false;
	ProllyHash	at;
	bool		more;

	if (from_ref)
	{
		if (!PROLLY_CALL(refs(), resolve, from_ref, &stop))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("prolly ref \"%s\" does not exist", from_ref)));
		stopping = true;
	}

	/* the working set's changes first, against the head (or nothing) */
	more = PROLLY_CALL(refs(), resolve, "HEAD", &at);
	if (to_ref == NULL || pg_strcasecmp(to_ref, "WORKING") == 0)
	{
		ProllyCommitInfo head;

		s->to_commit = "WORKING";
		s->from_commit = more ? hash_hex(&at) : NULL;
		s->to_dated = false;
		s->from_dated = more && PROLLY_CALL(refs(), commit_get, &at, &head);
		if (s->from_dated)
			s->from_date = head.committed_at;
		diff_step(s, relid, name, s->from_commit, "WORKING");
	}
	else
	{
		more = PROLLY_CALL(refs(), resolve, to_ref, &at);
		if (!more)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("prolly ref \"%s\" does not exist", to_ref)));
	}

	/* then each commit's, against its first parent */
	while (more && !(stopping && (at.len == stop.len && memcmp(at.bytes, stop.bytes, at.len) == 0)))
	{
		ProllyCommitInfo info;
		ProllyCommitInfo parent;
		char	   *to = hash_hex(&at);

		CHECK_FOR_INTERRUPTS();
		if (!PROLLY_CALL(refs(), commit_get, &at, &info))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("prolly commit %s is missing", to)));
		s->to_commit = to;
		s->to_date = info.committed_at;
		s->to_dated = true;
		s->from_commit = info.nparents > 0 ? hash_hex(&info.parents[0]) : NULL;
		s->from_dated = info.nparents > 0 &&
			PROLLY_CALL(refs(), commit_get, &info.parents[0], &parent);
		if (s->from_dated)
			s->from_date = parent.committed_at;
		diff_step(s, relid, name, s->from_commit, to);
		more = info.nparents > 0;
		if (more)
			at = info.parents[0];
	}
}

/*
 * dolt_diff_table(tbl anyelement, from_ref text DEFAULT NULL, to_ref text
 *				   DEFAULT NULL, OUT to_row anyelement, OUT from_row
 *				   anyelement, OUT to_commit text, OUT from_commit text, OUT
 *				   to_commit_date timestamptz, OUT from_commit_date
 *				   timestamptz, OUT diff_type text) returns setof record
 *
 * tbl's table's changes over its history, a commit at a time, as Dolt's
 * dolt_diff_<table> system tables have them: from to_ref (the working set,
 * if NULL: its changes against the head first, to_commit WORKING) back
 * along first parents to from_ref (not its changes; the first commit's,
 * against nothing, if NULL), each row as it was and is, decoded as the
 * table is defined now, with the commits it is between and their dates:
 * keyed as it is now (across a change of its primary key too), an old
 * value that doesn't convert to its column's type now shown null, with a
 * warning.  A commit that didn't change the table's rows costs a lookup
 * of it, no more.
 */
Datum
dolt_diff_table(PG_FUNCTION_ARGS)
{
	Oid			type = get_fn_expr_argtype(fcinfo->flinfo, 0);
	Oid			relid = OidIsValid(type) ? get_typ_typrelid(type) : InvalidOid;
	char	   *from_ref = PG_ARGISNULL(1) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(1));
	char	   *to_ref = PG_ARGISNULL(2) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(2));
	Relation	rel;
	RowDiffSink s;

	InitMaterializedSRF(fcinfo, 0);
	rel = open_row_type_table(relid, "dolt_diff_table", ACL_SELECT);
	memset(&s, 0, sizeof(s));
	s.sink.change = row_change;
	s.rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	rows_begin(&s.rows, rel);
	diff_history(&s, relid, NULL, from_ref, to_ref);
	rows_end(&s.rows);
	table_close(rel, AccessShareLock);
	return (Datum) 0;
}

/*
 * dolt_diff_table(table_name text, from_ref text DEFAULT NULL, to_ref text
 *				   DEFAULT NULL, OUT to_row jsonb, OUT from_row jsonb, ...)
 *	  returns setof record
 *
 * dolt_diff_table() of a table named by its name (#612): at each step, the
 * table the name names in the step's commits, its rows as jsonb, each
 * decoded as its own commit defines it (as dolt_diff() by name reads
 * them).  A step neither of whose commits has it changed has none.
 */
Datum
dolt_diff_table_by_name(PG_FUNCTION_ARGS)
{
	char	   *name;
	char	   *from_ref = PG_ARGISNULL(1) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(1));
	char	   *to_ref = PG_ARGISNULL(2) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(2));
	RowDiffSink s;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("dolt_diff_table's table name can't be null")));
	name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	InitMaterializedSRF(fcinfo, 0);
	memset(&s, 0, sizeof(s));
	s.sink.change = row_change;
	s.rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	s.json = true;
	diff_history(&s, InvalidOid, name, from_ref, to_ref);
	return (Datum) 0;
}

/* ------------------------------------------------------------------------
 * A commit's rows
 * ------------------------------------------------------------------------
 */

/* What dolt_at() and dolt_history_table() put of a version's rows. */
typedef struct AtSink
{
	ProllyDiffSink sink;		/* must be first */
	ReturnSetInfo *rsinfo;
	Rows		rows;
	bool		history;		/* each row a column, with the commit's */
	bool		json;			/* (history) a table by name: rows as jsonb */
	const ProllyCommitInfo *commit;
	char	   *hash;
	char	   *committer;
} AtSink;

static void
at_change(ProllyDiffSink *self, Oid relid, ProllyDiffKind kind,
		  const ProllyKey *key, const ProllyValue *before,
		  const ProllyValue *after)
{
	AtSink	   *s = (AtSink *) self;
	uint64		copies = 1;
	MemoryContext old;
	Datum		row;

	/* (a diff from nothing: every row added) */
	if (after == NULL)
		return;
	old = rows_next(&s->rows);
	if (s->history)
		row = rows_value(&s->rows, s->json, after, &copies);
	else
	{
		rows_slot(&s->rows, after, &copies);
		row = (Datum) 0;
	}
	if (!s->rows.keyless)
		copies = 1;
	for (uint64 i = 0; i < copies; i++)
	{
		if (s->history)
		{
			Datum		values[4];
			bool		nulls[4] = {0};

			values[0] = row;
			values[1] = CStringGetTextDatum(s->hash);
			values[2] = CStringGetTextDatum(s->committer);
			values[3] = TimestampTzGetDatum(s->commit->committed_at);
			tuplestore_putvalues(s->rsinfo->setResult, s->rsinfo->setDesc,
								 values, nulls);
		}
		else
			tuplestore_puttupleslot(s->rsinfo->setResult, s->rows.slot);
	}
	MemoryContextSwitchTo(old);
}

/*
 * dolt_at(ref text, tbl anyelement) returns setof anyelement
 *
 * tbl's table's rows (its row type, as in NULL::t) as ref has them (any
 * commit, branch or WORKING), read from ref's tree, never written to
 * pages: as Dolt's AS OF.  Decoded as the table is defined now, as
 * dolt_diff decodes them; a keyless row once a copy.
 */
Datum
dolt_at(PG_FUNCTION_ARGS)
{
	Oid			type = get_fn_expr_argtype(fcinfo->flinfo, 1);
	Oid			relid = OidIsValid(type) ? get_typ_typrelid(type) : InvalidOid;
	char	   *ref;
	Relation	rel;
	AtSink		s;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("dolt_at's ref can't be null")));
	ref = text_to_cstring(PG_GETARG_TEXT_PP(0));
	rel = open_row_type_table(relid, "dolt_at", ACL_SELECT);
	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	memset(&s, 0, sizeof(s));
	s.sink.change = at_change;
	s.rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	{
		List	   *lineages = NIL;

		check_typed_at(relid, ref, &lineages, "dolt_at");
	}
	rows_begin(&s.rows, rel);
	PROLLY_CALL(diff_merge(), diff, NULL, ref, relid, &s.sink);
	rows_end(&s.rows);
	table_close(rel, AccessShareLock);
	return (Datum) 0;
}

/*
 * relid's table's rows as ref has them, read as dolt_at(ref, NULL::t) reads
 * them (refused as it refuses them, decoded as the table is defined here),
 * put to store as rows of the table's descriptor.
 */
void
prolly_read_at(const char *ref, Oid relid, Tuplestorestate *store)
{
	Relation	rel;
	AtSink		s;
	ReturnSetInfo rsinfo;
	List	   *lineages = NIL;

	rel = open_row_type_table(relid, "a revision's table", ACL_SELECT);
	{
		List	   *rows = NIL;
		SummaryRow *r;
		char	   *name = qualified_name(relid);

		PROLLY_CALL(diff_merge(), summary, NULL, ref, false, collect_change,
					&rows);
		r = named_table(rows, name);
		if (r == NULL || r->to_item == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("relation \"%s\" does not exist in revision \"%s\"",
							name, ref)));
	}
	memset(&rsinfo, 0, sizeof(rsinfo));
	rsinfo.type = T_ReturnSetInfo;
	rsinfo.setResult = store;
	rsinfo.setDesc = RelationGetDescr(rel);
	memset(&s, 0, sizeof(s));
	s.sink.change = at_change;
	s.rsinfo = &rsinfo;
	check_typed_at(relid, ref, &lineages, "a revision's table");
	rows_begin(&s.rows, rel);
	PROLLY_CALL(diff_merge(), diff, NULL, ref, relid, &s.sink);
	rows_end(&s.rows);
	table_close(rel, AccessShareLock);
}

/*
 * dolt_at_support(internal) returns internal: dolt_at()'s planner support.
 * Its rows, estimated as the diff-merge's row_count() counts them from the
 * commit's tree, when the ref is a constant by planning (a parameter's
 * value too, in a custom plan); else the planner's default.  Only an
 * estimate is taken from the ref, never the commit it resolved to: a
 * plan kept for later reads whatever the ref names when it runs.
 *
 * A count is history too: a user dolt_at would refuse (no SELECT on the
 * table, or row security applying to them) gets the default, the tree
 * unread.  The estimate then depends on the role, so the plan is marked
 * so, and a plan kept for one role isn't used by another.
 */
Datum
dolt_at_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);
	Node	   *ret = NULL;

	if (IsA(rawreq, SupportRequestRows))
	{
		SupportRequestRows *req = (SupportRequestRows *) rawreq;

		if (is_funcclause(req->node) &&
			list_length(((FuncExpr *) req->node)->args) == 2 &&
			PROLLY_HAS(diff_merge(), row_count))
		{
			List	   *args = ((FuncExpr *) req->node)->args;
			Node	   *ref = estimate_expression_value(req->root, linitial(args));
			Oid			relid = get_typ_typrelid(exprType(lsecond(args)));
			double		rows;
			List	   *lineages = NIL;

			if (IsA(ref, Const) && !((Const *) ref)->constisnull &&
				OidIsValid(relid))
			{
				req->root->glob->dependsOnRole = true;
				if (history_access(relid, ACL_SELECT, true) == HISTORY_READABLE &&
					typed_at_allowed(relid, TextDatumGetCString(((Const *) ref)->constvalue),
									 &lineages, "dolt_at", true) &&
					PROLLY_CALL(diff_merge(), row_count,
								TextDatumGetCString(((Const *) ref)->constvalue),
								relid, &rows))
				{
					req->rows = rows;
					ret = (Node *) req;
				}
			}
		}
	}
	PG_RETURN_POINTER(ret);
}

/*
 * The rows a bounded history read found at the commit it read last, kept
 * for the commits after it whose table is the same tree: each key and row
 * as the sink was given them.
 */
typedef struct HistoryKept
{
	ProllyDiffSink sink;		/* must be first */
	AtSink	   *to;
	MemoryContext cxt;
	List	   *rows;			/* of HistoryRow */
} HistoryKept;

typedef struct HistoryRow
{
	ProllyKey	key;
	ProllyValue value;
} HistoryRow;

static void
history_keep(ProllyDiffSink *self, Oid relid, ProllyDiffKind kind,
			 const ProllyKey *key, const ProllyValue *before,
			 const ProllyValue *after)
{
	HistoryKept *hk = (HistoryKept *) self;
	MemoryContext old = MemoryContextSwitchTo(hk->cxt);
	HistoryRow *r = palloc(sizeof(HistoryRow));
	uint8	   *k = palloc(Max(key->len, 1));
	uint8	   *v = palloc(Max(after->len, 1));

	memcpy(k, key->data, key->len);
	memcpy(v, after->data, after->len);
	r->key.data = k;
	r->key.len = key->len;
	r->value.data = v;
	r->value.len = after->len;
	hk->rows = lappend(hk->rows, r);
	MemoryContextSwitchTo(old);
	at_change(&hk->to->sink, relid, kind, key, before, after);
}

/*
 * relid's table's rows at each commit the head reaches (that has the
 * table), with the commit, into rsinfo's store, as dolt_history_table()
 * gives them.  With bounds (nbounds of them: the rows whose primary keys
 * any bounds), only those: a commit whose version is keyed and defined as
 * the table is now is read by seeking its data tree for each bound
 * (O(depth) a bound), and one whose table is the tree the commit read
 * before it had is given that commit's rows again, reading nothing; any
 * other is read whole, the caller's filter keeping what the bounds would.
 */
void
prolly_history_rows(Oid relid, const ProllyKeyBounds *bounds, int nbounds,
					ReturnSetInfo *rsinfo)
{
	const ProllyMergeRoutine *dm = diff_merge();
	Relation	rel;
	AtSink		s;
	HistoryKept hk;
	ProllyHash	data;
	ListCell   *lc;
	List	   *lineages = NIL;

	rel = open_row_type_table(relid, "dolt_history_table", ACL_SELECT);
	memset(&s, 0, sizeof(s));
	s.sink.change = at_change;
	s.rsinfo = rsinfo;
	s.history = true;
	memset(&hk, 0, sizeof(hk));
	hk.sink.change = history_keep;
	hk.to = &s;
	hk.cxt = AllocSetContextCreate(CurrentMemoryContext, "history rows kept",
								   ALLOCSET_DEFAULT_SIZES);
	data.len = 0;
	if (nbounds > 0 && !PROLLY_HAS(dm, bounded_rows))
		nbounds = 0;
	rows_begin(&s.rows, rel);
	foreach(lc, log_entries("HEAD"))
	{
		LogEntry   *e = lfirst(lc);
		char	   *email;
		ProllyBoundedRead read = PROLLY_BOUNDED_NO;

		CHECK_FOR_INTERRUPTS();
		s.commit = &e->info;
		s.hash = hash_hex(&e->info.id);
		split_author(e->info.author, &s.committer, &email);
		check_typed_at(relid, s.hash, &lineages, "dolt_history_table");
		if (nbounds > 0)
		{
			/* (what this read finds kept apart from what the last one kept) */
			MemoryContext kept_cxt = hk.cxt;
			List	   *kept = hk.rows;

			hk.cxt = AllocSetContextCreate(CurrentMemoryContext,
										   "history rows kept",
										   ALLOCSET_DEFAULT_SIZES);
			hk.rows = NIL;
			read = dm->bounded_rows(s.hash, &e->info.root, relid, bounds,
									nbounds, &data, &hk.sink);
			if (read == PROLLY_BOUNDED_SAME)
			{
				/* (the same tree: the rows kept, given with this commit) */
				MemoryContextDelete(hk.cxt);
				hk.cxt = kept_cxt;
				hk.rows = kept;
				foreach_ptr(HistoryRow, r, hk.rows)
					at_change(&s.sink, relid, PROLLY_DIFF_ADDED, &r->key, NULL,
							  &r->value);
				continue;
			}
			MemoryContextDelete(kept_cxt);
			if (read == PROLLY_BOUNDED_READ)
				continue;
		}
		/* (read whole: nothing kept for the commits after it) */
		data.len = 0;
		MemoryContextReset(hk.cxt);
		hk.rows = NIL;
		PROLLY_CALL(dm, diff, NULL, s.hash, relid, &s.sink);
	}
	rows_end(&s.rows);
	MemoryContextDelete(hk.cxt);
	table_close(rel, AccessShareLock);
}

/*
 * dolt_history_table(tbl anyelement, OUT row anyelement, OUT commit_hash
 *					  text, OUT committer text, OUT commit_date timestamptz)
 *	  returns setof record
 *
 * tbl's table's rows at each commit the head reaches (that has the table),
 * with the commit, as Dolt's dolt_history_<table>: each read from the
 * commit's tree, as dolt_at() reads it.  (Planned with quals on the
 * primary key, it is read by a scan of its own that seeks each commit:
 * prolly_history_scan.c.)
 */
Datum
dolt_history_table(PG_FUNCTION_ARGS)
{
	Oid			type = get_fn_expr_argtype(fcinfo->flinfo, 0);
	Oid			relid = OidIsValid(type) ? get_typ_typrelid(type) : InvalidOid;

	InitMaterializedSRF(fcinfo, 0);
	prolly_history_rows(relid, NULL, 0, (ReturnSetInfo *) fcinfo->resultinfo);
	return (Datum) 0;
}

/* A change an unordered diff finds, to an AtSink. */
static bool
at_tree_change(ProllyDiffKind kind, const ProllyKey *key,
			   const ProllyValue *before, const ProllyValue *after, void *arg)
{
	at_change(arg, InvalidOid, kind, key, before, after);
	return true;
}

/*
 * dolt_history_table(table_name text, OUT row jsonb, OUT commit_hash text,
 *					  OUT committer text, OUT commit_date timestamptz)
 *	  returns setof record
 *
 * dolt_history_table() of a table named by its name (#612): at each commit
 * the head reaches, the rows of the table the name names in it (none
 * where it has none), as jsonb, decoded as the commit defines the table,
 * read from its tree.
 */
Datum
dolt_history_table_by_name(PG_FUNCTION_ARGS)
{
	const ProllyTreeRoutine *tree = prolly_tree_lookup("cow");
	char	   *name;
	AtSink		s;
	List	   *lineages = NIL;
	ListCell   *lc;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("dolt_history_table's table name can't be null")));
	name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	if (!PROLLY_HAS(diff_merge(), summary) || !PROLLY_HAS(tree, diff_unordered))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("the database's version control can't read a table's rows by its name")));
	InitMaterializedSRF(fcinfo, 0);
	memset(&s, 0, sizeof(s));
	s.sink.change = at_change;
	s.rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	s.history = true;
	s.json = true;
	foreach(lc, log_entries("HEAD"))
	{
		LogEntry   *e = lfirst(lc);
		List	   *rows = NIL;
		SummaryRow *r;
		char	   *email;
		ProllyNodeRef empty;
		ProllyNodeRef data;

		CHECK_FOR_INTERRUPTS();
		s.commit = &e->info;
		s.hash = hash_hex(&e->info.id);
		split_author(e->info.author, &s.committer, &email);

		/* (the commit's objects: what differs from the empty root to it) */
		PROLLY_CALL(diff_merge(), summary, NULL, s.hash, false, collect_change, &rows);
		r = named_table(rows, name);
		if (r == NULL || r->to_item == NULL)
			continue;
		check_named_rows(r, NULL, s.hash, &lineages, "dolt_history_table");
		rows_begin_named(&s.rows, r->to_item, r->to_len, name, "dolt_history_table");
		table_item_data(r->to_item, r->to_len, &data);
		PROLLY_CALL(tree, build_finish,
					PROLLY_CALL(tree, build_begin, &s.rows.env, CurrentMemoryContext),
					&empty);
		PROLLY_CALL(tree, diff_unordered, &s.rows.env, &empty, &data, at_tree_change, &s);
		rows_end(&s.rows);
	}
	return (Datum) 0;
}

/* ------------------------------------------------------------------------
 * Diff stats
 * ------------------------------------------------------------------------
 */

/* What dolt_diff_stat() counts of one table's rows. */
typedef struct StatSink
{
	ProllyDiffSink sink;		/* must be first */
	Rows		from;			/* (two, as a modified row decodes both) */
	Rows		to;
	bool		keyless;
	uint64		added;
	uint64		deleted;
	uint64		modified;
	uint64		cells_modified;
	bool		cells_unknown;	/* a modified row's cells aren't compared */
	int		   *from_of;		/* described versions: each of to's columns'
								 * of from's, by name (-1: none) */
} StatSink;

/* The columns of a modified row that differ, as the table is defined now. */
static uint64
cells_differing(StatSink *s, const ProllyValue *before, const ProllyValue *after)
{
	TupleDesc	desc = s->from.slot->tts_tupleDescriptor;
	uint64		count;
	uint64		n = 0;
	MemoryContext old = rows_next(&s->from);

	(void) rows_next(&s->to);
	(void) rows_datum(&s->from, before, &count);
	(void) rows_datum(&s->to, after, &count);
	slot_getallattrs(s->from.slot);
	slot_getallattrs(s->to.slot);
	if (s->from_of)
		desc = s->to.slot->tts_tupleDescriptor;
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);
		int			j = s->from_of ? s->from_of[i] : i;
		bool		fnull = j < 0 || s->from.slot->tts_isnull[j];
		bool		tnull = s->to.slot->tts_isnull[i];

		if (att->attisdropped)
			continue;
		if (j >= 0 && s->from_of &&
			TupleDescAttr(s->from.slot->tts_tupleDescriptor, j)->atttypid != att->atttypid)
		{
			/* (a column retyped between the versions: by its text) */
			Oid			fout;
			Oid			tout;
			bool		varlena;

			if (fnull != tnull)
				n++;
			else if (!fnull)
			{
				getTypeOutputInfo(TupleDescAttr(s->from.slot->tts_tupleDescriptor, j)->atttypid,
								  &fout, &varlena);
				getTypeOutputInfo(att->atttypid, &tout, &varlena);
				if (strcmp(OidOutputFunctionCall(fout, s->from.slot->tts_values[j]),
						   OidOutputFunctionCall(tout, s->to.slot->tts_values[i])) != 0)
					n++;
			}
			continue;
		}
		if (fnull != tnull ||
			(!fnull && !datum_image_eq(s->from.slot->tts_values[j],
									   s->to.slot->tts_values[i],
									   att->attbyval, att->attlen)))
			n++;
	}
	MemoryContextSwitchTo(old);
	return n;
}

static void
stat_change(ProllyDiffSink *self, Oid relid, ProllyDiffKind kind,
			const ProllyKey *key, const ProllyValue *before,
			const ProllyValue *after)
{
	StatSink   *s = (StatSink *) self;

	if (s->keyless)
	{
		ProllyValue row;
		uint64		was = 0;
		uint64		is = 0;

		/* (a keyless row's copies: those added or taken away) */
		if (before)
			rows_parse(&s->from, before, &row, &was);
		if (after)
			rows_parse(s->from_of ? &s->to : &s->from, after, &row, &is);
		if (is > was)
			s->added += is - was;
		else
			s->deleted += was - is;
		return;
	}
	if (kind == PROLLY_DIFF_ADDED)
		s->added++;
	else if (kind == PROLLY_DIFF_REMOVED)
		s->deleted++;
	else
	{
		s->modified++;
		if (!s->cells_unknown)
			s->cells_modified += cells_differing(s, before, after);
	}
}

/*
 * Whether relid's rows are keyed by a primary key now.  A diff through it
 * reads each version's rows keyed so, whatever key the version had: a
 * keyless version's copies of a row become one keyed row.
 */
static bool
keyed_now(Oid relid)
{
	Relation	rel = table_open(relid, AccessShareLock);
	ProllyComponents c;
	ProllyKeying *k;
	bool		keyless;

	prolly_components_for_relation(rel, &c);
	k = PROLLY_CALL(c.keying, open, rel, CurrentMemoryContext);
	keyless = PROLLY_CALL(c.keying, keyless, k);
	PROLLY_CALL(c.keying, close, k);
	table_close(rel, AccessShareLock);
	return !keyless;
}

/* A version of a table: its rows (a count of its tree's items), its columns. */
static void
table_version(const char *item, int len, uint64 *rows, int *columns, bool *keyed)
{
	ProllyValue v = {(const uint8 *) item, len};
	ProllyValue desc;
	ProllyNodeRef data;
	ProllyTreeEnv env;

	table_item_data(item, len, &data);
	desc.data = v.data + 1 + data.hash.len;
	desc.len = v.len - 1 - data.hash.len;
	*columns = list_length(PROLLY_CALL(canonicalizer(), table_columns, &desc, keyed));
	prolly_folded_tree_env(&env);
	*rows = PROLLY_CALL(prolly_tree_lookup("cow"), count, &env, &data);
}

/* A change an unordered diff finds, to the sink. */
static bool
stat_tree_change(ProllyDiffKind kind, const ProllyKey *key,
				 const ProllyValue *before, const ProllyValue *after, void *arg)
{
	stat_change(arg, InvalidOid, kind, key, before, after);
	return true;
}

/*
 * A table the session's branch hasn't, or has keyed otherwise than its
 * versions are: its rows diffed from the trees, by their keys' bytes (as
 * no relation orders them), each decoded by its version's description (a
 * side without the table, as the empty tree of the other's).  False if
 * they can't be read so.
 */
static bool
stat_described(StatSink *s, const SummaryRow *r)
{
	const ProllyTreeRoutine *tree = prolly_tree_lookup("cow");
	const char *fitem = r->from_item ? r->from_item : r->to_item;
	int			flen = r->from_item ? r->from_len : r->to_len;
	const char *titem = r->to_item ? r->to_item : r->from_item;
	int			tlen = r->to_item ? r->to_len : r->from_len;
	ProllyNodeRef fd;
	ProllyNodeRef td;
	TupleDesc	fdesc;
	TupleDesc	tdesc;

	if (!PROLLY_HAS(tree, diff_unordered) ||
		!rows_begin_described(&s->from, fitem, flen))
		return false;
	if (!rows_begin_described(&s->to, titem, tlen))
	{
		rows_end(&s->from);
		return false;
	}
	fdesc = s->from.slot->tts_tupleDescriptor;
	tdesc = s->to.slot->tts_tupleDescriptor;
	s->from_of = palloc_array(int, Max(tdesc->natts, 1));
	for (int i = 0; i < tdesc->natts; i++)
	{
		s->from_of[i] = -1;
		for (int j = 0; j < fdesc->natts; j++)
			if (strcmp(NameStr(TupleDescAttr(tdesc, i)->attname),
					   NameStr(TupleDescAttr(fdesc, j)->attname)) == 0)
				s->from_of[i] = j;
	}
	table_item_data(fitem, flen, &fd);
	table_item_data(titem, tlen, &td);
	if (r->from_item == NULL || r->to_item == NULL)
	{
		ProllyNodeRef empty;

		PROLLY_CALL(tree, build_finish,
					PROLLY_CALL(tree, build_begin, &s->from.env, CurrentMemoryContext),
					&empty);
		if (r->from_item == NULL)
			fd = empty;
		else
			td = empty;
	}
	PROLLY_CALL(tree, diff_unordered, &s->from.env, &fd, &td, stat_tree_change, s);
	rows_end(&s->from);
	rows_end(&s->to);
	return true;
}

/*
 * stat_described(), its modified rows' cells compared where its values
 * decode.  The columns' types are found by name as they're defined now, and
 * a version's value may not read as one (an enum's label renamed since):
 * then its rows are counted again, their cells modified unknown.
 */
static bool
stat_described_or_counted(StatSink *s, const SummaryRow *r)
{
	MemoryContext cxt = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	StatSink	start = *s;
	volatile bool read = false;
	volatile bool decoded = false;

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(cxt);
	PG_TRY();
	{
		read = stat_described(s, r);
		decoded = true;
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(cxt);
		CurrentResourceOwner = owner;
	}
	PG_CATCH();
	{
		ErrorData  *ed;

		MemoryContextSwitchTo(cxt);
		ed = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(cxt);
		CurrentResourceOwner = owner;
		/* (a cancel, or anything else, isn't a value that doesn't decode) */
		if (ERRCODE_TO_CATEGORY(ed->sqlerrcode) != ERRCODE_DATA_EXCEPTION)
			ReThrowError(ed);
		FreeErrorData(ed);
	}
	PG_END_TRY();
	if (decoded)
		return read;
	*s = start;
	s->cells_unknown = true;
	return stat_described(s, r);
}

/*
 * Dolt's cells added and deleted, from the rows added and deleted and the
 * cells before and after (its GetCellsAddedAndDeleted, as it counts them).
 */
static void
cells_added_deleted(uint64 added, uint64 deleted, int new_columns,
					uint64 old_cells, uint64 new_cells,
					uint64 *cells_added, uint64 *cells_deleted)
{
	double		inserts = (double) added * new_columns;
	double		deletes = (double) deleted * new_columns;
	double		diff = (double) new_cells - (double) old_cells;

	if (diff > 0)
	{
		*cells_added = (uint64) (diff + deletes);
		*cells_deleted = (uint64) deletes;
	}
	else if (diff < 0)
	{
		*cells_added = (uint64) inserts;
		*cells_deleted = (uint64) (-diff + inserts);
	}
	else
	{
		*cells_added = (uint64) Max(inserts, deletes);
		*cells_deleted = (uint64) Max(inserts, deletes);
	}
}

/*
 * dolt_diff_stat(from_ref text, to_ref text [, name text], OUT table_name
 * text, OUT rows_unmodified int8, OUT rows_added int8, OUT rows_deleted
 * int8, OUT rows_modified int8, OUT cells_added int8, OUT cells_deleted
 * int8, OUT cells_modified int8, OUT old_row_count int8, OUT new_row_count
 * int8, OUT old_cell_count int8, OUT new_cell_count int8) returns setof
 * record
 *
 * For each table whose rows (or whose cells' count) differ between two
 * refs, as Dolt's: the rows added, deleted and modified, found by the tree
 * diff of its rows (what both sides share is skipped), the cells of the
 * modified ones that differ (each decoded as the table is defined now),
 * and the rows and cells each side has; cells added and deleted as Dolt
 * counts them.  A keyless table's rows added and deleted are copies, the
 * rest NULL, as in Dolt.  A table the session's branch hasn't, or has
 * keyed otherwise than the versions are (a primary key made or dropped
 * since), changed on both sides, has its rows diffed from the trees, by
 * their keys' bytes (as no relation orders them), each version's decoded
 * by its own description, cells compared by column name (unknown, NULL,
 * where a version's values don't decode as its columns' types are defined
 * now).  With name, that table's alone: one neither version has is
 * refused (as Dolt's; dolt_diff_summary gives none).
 */
Datum
dolt_diff_stat(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	char	   *from_ref;
	char	   *to_ref;
	char	   *name = NULL;
	List	   *stats;
	ListCell   *lc;
	List	   *lineages = NIL;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("dolt_diff_stat needs two refs")));
	from_ref = text_to_cstring(PG_GETARG_TEXT_PP(0));
	to_ref = PG_ARGISNULL(1) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(1));
	if (PG_NARGS() > 2 && !PG_ARGISNULL(2))
		name = text_to_cstring(PG_GETARG_TEXT_PP(2));
	diff_refs("dolt_diff_stat", &from_ref, &to_ref, &name);
	stats = summary_rows(from_ref, to_ref, name, false);

	/*
	 * (as Dolt's: a table neither version has is refused, not none; the
	 * name's changes may be another object's, a view's or a routine's)
	 */
	if (name != NULL && !rows_have_table(stats) &&
		!version_has_table(from_ref, name) && !version_has_table(to_ref, name))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("table \"%s\" is in neither version", name)));
	InitMaterializedSRF(fcinfo, 0);
	foreach(lc, stats)
	{
		SummaryRow *r = lfirst(lc);
		const char *table = r->to_name ? r->to_name : r->from_name;
		uint64		old_rows = 0;
		uint64		new_rows = 0;
		int			old_columns = 0;
		int			new_columns = 0;
		bool		keyed = true;
		bool		counted = false;
		StatSink	s;
		Oid			relid;
		Datum		values[12];
		bool		nulls[12] = {0};

		if (r->not_covered || r->object != PROLLY_OBJECT_TABLE)
			continue;

		/*
		 * (counts are history too: a table's, as a read of its rows takes
		 * it; one the user may not read is left out of the list, and
		 * refused when named)
		 */
		if (!named_rows_allowed(r, from_ref, to_ref, &lineages, "dolt_diff_stat",
								name == NULL))
			continue;
		if (r->from_item)
			table_version(r->from_item, r->from_len, &old_rows, &old_columns, &keyed);
		if (r->to_item)
			table_version(r->to_item, r->to_len, &new_rows, &new_columns, &keyed);
		memset(&s, 0, sizeof(s));
		s.sink.change = stat_change;
		s.keyless = !keyed;

		/*
		 * (rows diffed where the branch has the table, keyed as the versions
		 * are: the rows are read keyed as it is now; one side's: counted)
		 */
		relid = RangeVarGetRelid(makeRangeVarFromNameList(stringToQualifiedNameList(table, NULL)),
								 NoLock, true);
		if (OidIsValid(relid) && get_rel_relam(relid) == PROLLY_TABLE_AM_OID &&
			(r->data_change || !keyed) && keyed_now(relid) == keyed)
		{
			Relation	rel = table_open(relid, AccessShareLock);

			check_history_rls(relid, "dolt_diff_stat");
			rows_begin(&s.from, rel);
			rows_begin(&s.to, rel);
			table_close(rel, AccessShareLock);
			PROLLY_CALL(diff_merge(), diff, from_ref, to_ref, relid, &s.sink);
			rows_end(&s.from);
			rows_end(&s.to);
			counted = true;
		}
		else if (keyed && (r->from_item == NULL || r->to_item == NULL))
		{
			s.added = new_rows;
			s.deleted = old_rows;
			counted = true;
		}
		else if ((r->data_change || !keyed) && stat_described_or_counted(&s, r))
			counted = true;
		else if (keyed && !r->data_change)
			counted = true;

		/* (as Dolt: a table whose rows and cells' count are the same, none) */
		if (counted && s.added + s.deleted + s.modified == 0 &&
			old_rows * old_columns == new_rows * new_columns)
			continue;

		values[0] = CStringGetTextDatum(table);
		for (int i = 1; i < 12; i++)
			nulls[i] = true;
		if (counted && keyed)
		{
			uint64		cells_added;
			uint64		cells_deleted;

			cells_added_deleted(s.added, s.deleted, new_columns,
								old_rows * old_columns, new_rows * new_columns,
								&cells_added, &cells_deleted);
			values[1] = Int64GetDatum((int64) (old_rows - s.modified - s.deleted));
			values[2] = Int64GetDatum((int64) s.added);
			values[3] = Int64GetDatum((int64) s.deleted);
			values[4] = Int64GetDatum((int64) s.modified);
			values[5] = Int64GetDatum((int64) cells_added);
			values[6] = Int64GetDatum((int64) cells_deleted);
			values[7] = Int64GetDatum((int64) s.cells_modified);
			for (int i = 1; i <= 7; i++)
				nulls[i] = false;
			nulls[7] = s.cells_unknown && s.modified > 0;
		}
		else if (counted)
		{
			values[2] = Int64GetDatum((int64) s.added);
			values[3] = Int64GetDatum((int64) s.deleted);
			nulls[2] = nulls[3] = false;
		}
		if (keyed)
		{
			values[8] = Int64GetDatum((int64) old_rows);
			values[9] = Int64GetDatum((int64) new_rows);
			values[10] = Int64GetDatum((int64) (old_rows * old_columns));
			values[11] = Int64GetDatum((int64) (new_rows * new_columns));
			nulls[8] = nulls[9] = nulls[10] = nulls[11] = false;
		}
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

/* ------------------------------------------------------------------------
 * Conflicts
 * ------------------------------------------------------------------------
 */

/*
 * dolt_conflicts(OUT table_name text, OUT num_conflicts int8)
 *	  returns setof record
 *
 * The tables with unresolved conflicts in the session branch's merge.
 */
typedef struct CountSink
{
	ProllyConflictSink sink;	/* must be first */
	int64		n;
	bool		unreadable;		/* the count isn't known */
} CountSink;

static void
count_conflict(ProllyConflictSink *self, const ProllyKey *key,
			   const ProllyValue *base, const ProllyValue *ours,
			   const ProllyValue *theirs)
{
	((CountSink *) self)->n++;
}

/* (a table whose conflicts can't be read is listed, its count unknown) */
static void
count_unreadable(ProllyConflictSink *self, const char *why)
{
	((CountSink *) self)->unreadable = true;
}

Datum
dolt_conflicts_summary(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	ProllyMergeState ms;

	InitMaterializedSRF(fcinfo, 0);
	if (!PROLLY_CALL(refs(), merge_state, &ms))
		return (Datum) 0;
	for (int i = 0; i < ms.ntables; i++)
	{
		CountSink	cs = {{count_conflict, count_unreadable}, 0, false};
		Datum		values[2];
		bool		nulls[2] = {0};

		PROLLY_CALL(diff_merge(), conflicts, ms.tables[i], &cs.sink);
		values[0] = CStringGetTextDatum(qualified_name(ms.tables[i]));
		values[1] = Int64GetDatum(cs.n);
		nulls[1] = cs.unreadable;
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	/* (a table in an identity conflict: one conflict, the table's own) */
	for (int i = 0; i < ms.nidentity; i++)
	{
		Datum		values[2];
		bool		nulls[2] = {0};

		values[0] = CStringGetTextDatum(ms.identity[i].name);
		values[1] = Int64GetDatum(1);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

/*
 * dolt_identity_conflicts(OUT table_name text, OUT our_relation oid, OUT
 * our_owner regrole, OUT their_table text, OUT their_relation oid, OUT
 * their_owner regrole, OUT tree int4) returns setof record
 *
 * The tables the merge in progress kept as the branch has them because
 * the other side's version of one (or of a table of its inheritance or
 * partition tree) is another relation of its name: the table here and
 * theirs, each its relation and owner (theirs': the local branch merged's,
 * if it was one's head; else NULL), and the tree resolved with it.  Never
 * rows: the two are different tables.
 */
Datum
dolt_identity_conflicts(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	ProllyMergeState ms;

	InitMaterializedSRF(fcinfo, 0);
	if (!PROLLY_CALL(refs(), merge_state, &ms))
		return (Datum) 0;
	for (int i = 0; i < ms.nidentity; i++)
	{
		ProllyIdentityConflict *c = &ms.identity[i];
		Datum		values[7];
		bool		nulls[7] = {0};
		HeapTuple	tup = OidIsValid(c->relid) ?
			SearchSysCache1(RELOID, ObjectIdGetDatum(c->relid)) : NULL;
		Oid			their_owner = InvalidOid;

		values[0] = CStringGetTextDatum(c->name);
		values[1] = ObjectIdGetDatum(c->relid);
		nulls[1] = !OidIsValid(c->relid);
		nulls[2] = !HeapTupleIsValid(tup);
		if (HeapTupleIsValid(tup))
		{
			values[2] = ObjectIdGetDatum(((Form_pg_class) GETSTRUCT(tup))->relowner);
			ReleaseSysCache(tup);
		}
		values[3] = CStringGetTextDatum(c->their_name);
		values[4] = ObjectIdGetDatum(c->their_relid);
		nulls[4] = !OidIsValid(c->their_relid);
		if (ms.identity_source != NULL && OidIsValid(c->their_relid) &&
			branch_ops()->relation_owner != NULL)
			their_owner = branch_ops()->relation_owner(ms.identity_source,
													   c->their_relid);
		values[5] = ObjectIdGetDatum(their_owner);
		nulls[5] = !OidIsValid(their_owner);
		values[6] = Int32GetDatum(c->tree);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

/* A version of a table in a schema conflict: its item, and statements. */
typedef struct ConflictSide
{
	bool		found;			/* listed in a diff from the base */
	char	   *item;			/* NULL: none (dropped) */
	int			len;
	char	   *sql;
} ConflictSide;

/*
 * A table in from and to (refs), named from_name in from and to_name in
 * to, if it changed between them, as dolt_schema_diff() lists it: each
 * side's item and statements (a side without it: "<deleted>").  Sides
 * already found are left as they are.
 */
static void
conflict_sides(const char *from, const char *to, const char *from_name,
			   const char *to_name, ConflictSide *from_side,
			   ConflictSide *to_side)
{
	ListCell   *lc;

	foreach(lc, summary_rows(from, to, NULL, true))
	{
		SummaryRow *r = lfirst(lc);

		if (r->object != PROLLY_OBJECT_TABLE ||
			!((r->from_name && strcmp(r->from_name, from_name) == 0) ||
			  (r->to_name && strcmp(r->to_name, to_name) == 0)))
			continue;
		if (!from_side->found)
		{
			from_side->found = true;
			from_side->item = r->from_item;
			from_side->len = r->from_len;
			from_side->sql = r->from_item ?
				render_item(r->object, r->from_name, r->from_item, r->from_len) :
				pstrdup("<deleted>");
		}
		to_side->found = true;
		to_side->item = r->to_item;
		to_side->len = r->to_len;
		to_side->sql = r->to_item ?
			render_item(r->object, r->to_name, r->to_item, r->to_len) :
			pstrdup("<deleted>");
	}
}

/* A table version's live columns (ProllyTableColumn *). */
static List *
side_columns(const ConflictSide *side)
{
	ProllyValue v = {(const uint8 *) side->item, side->len};
	ProllyValue desc;
	ProllyNodeRef data;
	bool		keyed;

	table_item_data(side->item, side->len, &data);
	desc.data = v.data + 1 + data.hash.len;
	desc.len = v.len - 1 - data.hash.len;
	return PROLLY_CALL(canonicalizer(), table_columns, &desc, &keyed);
}

static ProllyTableColumn *
column_named(List *columns, const char *name)
{
	ListCell   *lc;

	foreach(lc, columns)
		if (strcmp(((ProllyTableColumn *) lfirst(lc))->name, name) == 0)
			return lfirst(lc);
	return NULL;
}

/* A column's type as a description names it: built-in types unqualified. */
static const char *
shown_type(const char *type)
{
	return strncmp(type, "pg_catalog.", 11) == 0 ? type + 11 : type;
}

/*
 * What conflicts in a table both sides changed, in Dolt's words where it
 * has them: a column the sides give types that differ, one a side
 * dropped and the other changed, else the definitions at large.
 */
static char *
conflict_description(const ConflictSide *base, const ConflictSide *ours,
					 const ConflictSide *theirs)
{
	List	   *b;
	List	   *o;
	List	   *t;
	ListCell   *lc;

	if (ours->item == NULL || theirs->item == NULL)
	{
		const ConflictSide *kept = ours->item ? ours : theirs;

		return pstrdup(strcmp(kept->sql, base->sql) == 0 ?
					   "cannot merge a table deletion with data modification" :
					   "cannot merge a table deletion with schema modification");
	}
	if (base->item == NULL)
		return pstrdup("both branches made the table, defined differently");
	b = side_columns(base);
	o = side_columns(ours);
	t = side_columns(theirs);
	foreach(lc, b)
	{
		ProllyTableColumn *bc = lfirst(lc);
		ProllyTableColumn *oc = column_named(o, bc->name);
		ProllyTableColumn *tc = column_named(t, bc->name);

		if (oc && tc && strcmp(shown_type(oc->type), shown_type(tc->type)) != 0)
			return psprintf("incompatible column types for column '%s': %s and %s",
							bc->name, shown_type(oc->type), shown_type(tc->type));
		if ((oc == NULL) != (tc == NULL) &&
			strcmp((oc ? oc : tc)->type, bc->type) != 0)
			return psprintf("column '%s' was dropped on one side and changed on the other",
							bc->name);
	}
	return pstrdup("both branches changed the table's definition, and the changes don't merge");
}

/*
 * dolt_schema_conflicts(OUT table_name text, OUT base_schema text, OUT
 * our_schema text, OUT their_schema text, OUT description text) returns
 * setof record
 *
 * The tables the merge in progress kept as the branch has them, as Dolt's
 * dolt_schema_conflicts lists them: each one's statements in the merge
 * base, the branch's head and the merged commit (as dolt_schema_diff()
 * gives them; "<deleted>" where that side dropped it), and what conflicts.
 */
Datum
dolt_schema_conflicts(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	ProllyMergeState ms;
	ProllyHash	head;
	char	   *base;
	char	   *ours;
	char	   *theirs;

	InitMaterializedSRF(fcinfo, 0);
	if (!PROLLY_CALL(refs(), merge_state, &ms) || ms.nschema == 0 ||
		!PROLLY_CALL(refs(), resolve, "HEAD", &head))
		return (Datum) 0;
	if (!PROLLY_HAS(canonicalizer(), render))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("the database's canonical form can't be shown as statements")));
	base = hash_hex(&ms.base);
	ours = hash_hex(&head);
	theirs = hash_hex(&ms.theirs);
	for (int i = 0; i < ms.nschema; i++)
	{
		const char *name = ms.schema_tables[i];

		/* (each side by its own name: one may have renamed it) */
		const char *base_name = ms.schema_base_tables ? ms.schema_base_tables[i] : name;
		const char *their_name = ms.schema_their_tables ? ms.schema_their_tables[i] : name;
		ConflictSide b = {0};
		ConflictSide o = {0};
		ConflictSide t = {0};
		Datum		values[5];
		bool		nulls[5] = {0};

		conflict_sides(base, ours, base_name, name, &b, &o);
		conflict_sides(base, theirs, base_name, their_name, &b, &t);
		/* (a side that changed only the table's rows: as listed from base) */
		if (!b.found)
		{
			b.sql = pstrdup("<deleted>");
			b.found = true;
		}
		if (!o.found)
			o = b;
		if (!t.found)
			t = b;
		values[0] = CStringGetTextDatum(name);
		values[1] = CStringGetTextDatum(b.sql);
		values[2] = CStringGetTextDatum(o.sql);
		values[3] = CStringGetTextDatum(t.sql);
		values[4] = CStringGetTextDatum(conflict_description(&b, &o, &t));
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

/*
 * dolt_constraint_violations(OUT table_name text, OUT num_violations int8)
 *
 * The tables with constraint violations a merge (or cherry-pick) left on
 * the session's branch, as Dolt's dolt_constraint_violations table.
 */
Datum
dolt_constraint_violations_summary(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	List	   *vs;
	List	   *tables = NIL;
	ListCell   *lc;
	ListCell   *lt;

	InitMaterializedSRF(fcinfo, 0);
	vs = PROLLY_CALL(refs(), violations);
	foreach(lc, vs)
		tables = list_append_unique_oid(tables, ((ProllyViolation *) lfirst(lc))->relid);
	foreach(lt, tables)
	{
		Oid			relid = lfirst_oid(lt);
		int64		n = 0;
		Datum		values[2];
		bool		nulls[2] = {0};

		if (qualified_name(relid) == NULL)
			continue;			/* (dropped) */
		foreach(lc, vs)
			if (((ProllyViolation *) lfirst(lc))->relid == relid)
				n++;
		values[0] = CStringGetTextDatum(qualified_name(relid));
		values[1] = Int64GetDatum(n);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

/*
 * dolt_constraint_violations(tbl regclass, OUT violation_type text,
 *		OUT violating_row jsonb, OUT violation_info jsonb)
 *
 * A table's constraint violations, as Dolt's dolt_constraint_violations_
 * <table>: each row, and what it violates.
 */
Datum
dolt_constraint_violations(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Oid			relid = PG_GETARG_OID(0);
	ListCell   *lc;

	InitMaterializedSRF(fcinfo, 0);
	check_history_access(relid, "dolt_constraint_violations", ACL_SELECT);
	foreach(lc, PROLLY_CALL(refs(), violations))
	{
		ProllyViolation *pv = lfirst(lc);
		Datum		values[3];
		bool		nulls[3] = {0};

		if (pv->relid != relid)
			continue;
		values[0] = CStringGetTextDatum(pv->type);
		values[1] = DirectFunctionCall1(jsonb_in, CStringGetDatum(pv->row));
		values[2] = DirectFunctionCall1(jsonb_in, CStringGetDatum(pv->info));
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

/*
 * dolt_verify_constraints(VARIADIC tables text[]) returns text[]: {status}
 *
 * [--all] [--output-only] tables: check the tables (none: every versioned
 * one) again, as Dolt's dolt_verify_constraints: against their foreign
 * keys, and their check and not-null constraints not validated yet (a
 * table holds no row breaking one validated).  Their recorded violations
 * are replaced by what is found now, unless --output-only: rows a merge
 * didn't write for breaking a unique, check or not-null constraint are
 * dropped (and the rows a unique one's collided with), what to do with
 * them is the user's.  Only the rows changed
 * since the head commit are checked (and those referencing rows removed
 * or changed), and every row of a table with violations recorded, so one
 * still holding is kept, unless --all: every row.  Status 1 if any are found, else 0.
 */
Datum
dolt_verify_constraints(PG_FUNCTION_ARGS)
{
	char	  **args = NULL;
	int			nargs = PG_NARGS() > 0 ? text_args(fcinfo, &args) : 0;
	List	   *relids = NIL;
	List	   *kept = NIL;
	List	   *found;
	ListCell   *lc;
	const char *result[1];
	bool		output_only = false;
	bool		all = false;
	bool		options = true;	/* until "--" */

	for (int i = 0; i < nargs; i++)
	{
		if (options && strcmp(args[i], "--") == 0)
		{
			options = false;	/* tables after it, even ones like flags */
			continue;
		}
		if (options && args[i][0] == '-')
		{
			if (is_option(args[i], "all"))
				all = true;
			else if (is_option(args[i], "output-only"))
				output_only = true;
			/* short flags, together or apart: -a, -o, -ao */
			else if (short_flags(args[i]) != NULL &&
					 strspn(short_flags(args[i]), "ao") == strlen(short_flags(args[i])))
			{
				output_only |= strchr(short_flags(args[i]), 'o') != NULL;
				all |= strchr(short_flags(args[i]), 'a') != NULL;
			}
			else
				unsupported("dolt_verify_constraints", args[i]);
			continue;
		}
		relids = lappend_oid(relids,
							 RangeVarGetRelid(makeRangeVarFromNameList(stringToQualifiedNameList(args[i], NULL)),
											  AccessShareLock, false));
	}
	foreach(lc, PROLLY_CALL(refs(), violations))
	{
		ProllyViolation *pv = lfirst(lc);

		if (relids != NIL && !list_member_oid(relids, pv->relid))
			kept = lappend(kept, pv);
	}
	BRANCH_WRITES_HUSHED(found = PROLLY_CALL(diff_merge(), verify, relids, all));
	if (relids != NIL)
	{
		List	   *own = NIL;

		/* (a table's own foreign keys, not those of tables referencing it) */
		foreach(lc, found)
			if (list_member_oid(relids, ((ProllyViolation *) lfirst(lc))->relid))
				own = lappend(own, lfirst(lc));
		found = own;
		kept = list_concat(kept, own);
	}
	else
		kept = found;
	/* (--output-only writes nothing: a read-only transaction's to run) */
	if (!output_only)
	{
		PreventCommandIfReadOnly("dolt_verify_constraints()");
		PROLLY_CALL(refs(), set_violations, kept);
	}
	result[0] = found != NIL ? "1" : "0";
	PG_RETURN_DATUM(text_array(1, result));
}

/* dolt_verify_constraints() returns text[]: every versioned table */
Datum
dolt_verify_constraints_all(PG_FUNCTION_ARGS)
{
	return dolt_verify_constraints(fcinfo);
}

typedef struct RowConflictSink
{
	ProllyConflictSink sink;	/* must be first */
	ReturnSetInfo *rsinfo;
	Rows		rows;
} RowConflictSink;

static const char *
change_kind(const ProllyValue *base, const ProllyValue *side)
{
	if (base == NULL)
		return "added";
	if (side == NULL)
		return "removed";
	return "modified";
}

static void
row_conflict(ProllyConflictSink *self, const ProllyKey *key,
			 const ProllyValue *base, const ProllyValue *ours,
			 const ProllyValue *theirs)
{
	RowConflictSink *s = (RowConflictSink *) self;
	Datum		values[5];
	bool		nulls[5] = {base == NULL, ours == NULL, theirs == NULL, false, false};
	uint64		count;
	MemoryContext old = rows_next(&s->rows);

	if (base)
		values[0] = rows_datum(&s->rows, base, &count);
	if (ours)
		values[1] = rows_datum(&s->rows, ours, &count);
	if (theirs)
		values[2] = rows_datum(&s->rows, theirs, &count);
	values[3] = CStringGetTextDatum(change_kind(base, ours));
	values[4] = CStringGetTextDatum(change_kind(base, theirs));
	tuplestore_putvalues(s->rsinfo->setResult, s->rsinfo->setDesc, values,
						 nulls);
	MemoryContextSwitchTo(old);
}

/*
 * dolt_conflicts(tbl anyelement, OUT base_row anyelement,
 *				  OUT our_row anyelement, OUT their_row anyelement,
 *				  OUT our_diff_type text, OUT their_diff_type text)
 *	  returns setof record
 *
 * A table's unresolved conflicts, each row as the common ancestor, the
 * current branch and the merged branch have it: Doltgres's
 * dolt_conflicts_<table>, with rows of the table's row type for its
 * base_, our_ and their_ columns.
 */
Datum
dolt_conflicts(PG_FUNCTION_ARGS)
{
	Oid			type = get_fn_expr_argtype(fcinfo->flinfo, 0);
	Oid			relid = OidIsValid(type) ? get_typ_typrelid(type) : InvalidOid;
	Relation	rel;
	RowConflictSink s;

	InitMaterializedSRF(fcinfo, 0);
	rel = open_row_type_table(relid, "dolt_conflicts", ACL_SELECT);
	memset(&s, 0, sizeof(s));
	s.sink.conflict = row_conflict;
	s.rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	rows_begin(&s.rows, rel);
	PROLLY_CALL(diff_merge(), conflicts, relid, &s.sink);
	rows_end(&s.rows);
	table_close(rel, AccessShareLock);
	return (Datum) 0;
}

/*
 * dolt_conflicts_resolve(VARIADIC args text[]) returns text[]: {status}
 *
 * --ours or --theirs, then tables (or "." for every one with conflicts):
 * keep the current branch's rows, or take the merged branch's.
 */
Datum
dolt_conflicts_resolve(PG_FUNCTION_ARGS)
{
	char	  **args;
	int			nargs = text_args(fcinfo, &args);
	int			side = 0;		/* 1 ours, 2 theirs */
	bool		options = true;	/* until "--" */
	List	   *tables = NIL;
	ListCell   *lc;
	ProllyMergeState ms;
	const char *result[1] = {"0"};

	/*
	 * Schema conflicts aren't resolved so, as in Dolt: the tables in them
	 * hold the branch's rows, theirs not merged, so neither side's choice
	 * would be the merge.
	 */
	if (PROLLY_CALL(refs(), merge_state, &ms) && ms.nschema > 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("unable to automatically resolve schema conflicts, since data changes may not have been fully merged yet"),
				 errdetail("Table %s has a schema conflict.", ms.schema_tables[0]),
				 errhint("Abort the merge with dolt_merge('--abort'), change either side of the merge so the definitions agree, then merge again.")));
	for (int i = 0; i < nargs; i++)
	{
		char	   *a = args[i];

		if (options && strcmp(a, "--") == 0)
			options = false;	/* tables after it, even ones like flags */
		else if (options && a[0] == '-')
		{
			if (is_option(a, "ours"))
				side = 1;
			else if (is_option(a, "theirs"))
				side = 2;
			else
				unsupported("dolt_conflicts_resolve", a);
		}
		else if (strcmp(a, ".") == 0)
		{
			if (PROLLY_CALL(refs(), merge_state, &ms))
			{
				for (int j = 0; j < ms.ntables; j++)
					tables = list_append_unique_oid(tables, ms.tables[j]);
				for (int j = 0; j < ms.nidentity; j++)
					if (OidIsValid(ms.identity[j].relid))
						tables = list_append_unique_oid(tables, ms.identity[j].relid);
			}
		}
		else
		{
			Oid			relid = RangeVarGetRelid(makeRangeVarFromNameList(stringToQualifiedNameList(a, NULL)),
												 NoLock, false);
			AclResult	aclresult = pg_class_aclcheck(relid, GetUserId(),
													  ACL_INSERT | ACL_UPDATE | ACL_DELETE);

			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, OBJECT_TABLE, get_rel_name(relid));
			tables = list_append_unique_oid(tables, relid);
		}
	}
	if (side == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dolt_conflicts_resolve needs --ours or --theirs")));
	PreventCommandIfReadOnly("dolt_conflicts_resolve()");

	/*
	 * (an identity conflict's tree is resolved whole, through any of its
	 * tables: once)
	 */
	if (PROLLY_CALL(refs(), merge_state, &ms) && ms.nidentity > 0)
	{
		List	   *trees = NIL;
		List	   *once = NIL;

		foreach(lc, tables)
		{
			int			tree = -1;

			for (int j = 0; j < ms.nidentity; j++)
				if (ms.identity[j].relid == lfirst_oid(lc))
					tree = ms.identity[j].tree;
			if (tree >= 0 && list_member_int(trees, tree))
				continue;
			if (tree >= 0)
				trees = lappend_int(trees, tree);
			once = lappend_oid(once, lfirst_oid(lc));
		}
		tables = once;
	}
	branch_note_write(MyBranch);
	{
		/* (a side's rows are of the merge's other side's commit) */
		ProllyMergeState theirs;
		bool		merging = PROLLY_CALL(refs(), merge_state, &theirs);

		VERSION_MOVE(&theirs.theirs, merging && theirs.theirs.len > 0 ? 1 : 0,
					 foreach(lc, tables)
						 PROLLY_CALL(diff_merge(), resolve, lfirst_oid(lc), side == 2));
	}
	PG_RETURN_DATUM(text_array(1, result));
}

/* ----------------------------------------------------------------
 * Remotes
 * ----------------------------------------------------------------
 */

/* The refs remotes are kept in. */
static const ProllyRefsRoutine *
remote_refs(void)
{
	ProllyDatabaseComponents dc;

	prolly_database_components(&dc);
	return dc.refs;
}

typedef struct RemoteFind
{
	const char *name;
	char	   *url;
} RemoteFind;

static void
find_remote(const char *name, const char *url, void *arg)
{
	RemoteFind *rf = arg;

	if (strcmp(name, rf->name) == 0)
		rf->url = pstrdup(url);
}

/* A remote's URL; NULL if there is no remote of that name. */
static char *
remote_url_of(const char *name)
{
	RemoteFind	rf = {name, NULL};

	PROLLY_CALL(remote_refs(), remotes, find_remote, &rf);
	return rf.url;
}

static char *
need_remote(const char *name)
{
	char	   *url = remote_url_of(name);

	if (url == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("remote \"%s\" not found", name)));
	return url;
}

/* Add a remote: its name valid and not taken, its URL one a remote serves. */
static void
add_remote(const char *name, const char *url)
{
	if (name[0] == '\0' || strchr(name, '/') != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid remote name \"%s\"", name),
				 errdetail("A remote's name is not empty and has no \"/\".")));
	(void) prolly_remote_for_url(url);
	if (remote_url_of(name) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("remote \"%s\" already exists", name)));
	PROLLY_CALL(remote_refs(), remote_set, name, url);
}

/*
 * dolt_remote(VARIADIC args text[]) returns text[]: {status}
 *
 * add name url; remove (or rm) name: the remote, its remote-tracking
 * branches and the upstreams naming it.
 */
Datum
dolt_remote(PG_FUNCTION_ARGS)
{
	char	  **args;
	int			nargs = text_args(fcinfo, &args);
	const char *result[1] = {"0"};

	/* (no options: a "--" only lets a name begin with "-") */
	for (int i = 0; i < nargs; i++)
		if (strcmp(args[i], "--") == 0)
		{
			memmove(&args[i], &args[i + 1], sizeof(char *) * (nargs - i));
			nargs--;
			break;
		}
	if (nargs >= 1 && strcmp(args[0], "add") == 0)
	{
		if (nargs != 3)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("dolt_remote add needs a name and a URL")));
		PreventCommandIfReadOnly("dolt_remote()");
		check_writes_refs("add a remote");
		add_remote(args[1], args[2]);
	}
	else if (nargs >= 1 && (strcmp(args[0], "remove") == 0 ||
							strcmp(args[0], "rm") == 0))
	{
		if (nargs != 2)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("dolt_remote remove needs a name")));
		PreventCommandIfReadOnly("dolt_remote()");
		check_writes_refs("remove a remote");
		(void) need_remote(args[1]);
		PROLLY_CALL(remote_refs(), remote_set, args[1], NULL);
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dolt_remote needs add or remove")));
	PG_RETURN_DATUM(text_array(1, result));
}

typedef struct Heads
{
	List	   *branches;		/* char * */
	List	   *commits;		/* ProllyHash * */
	const char *remote;			/* the remote's name, for messages */
} Heads;

/* name, a remote's branch's, for a message: bytes not printable ASCII as \xNN */
static char *
escaped_branch_name(const char *name)
{
	StringInfoData out;

	initStringInfo(&out);
	for (const char *p = name; *p; p++)
	{
		unsigned char c = (unsigned char) *p;

		if (c < 0x20 || c >= 0x7f)
			appendStringInfo(&out, "\\x%02x", c);
		else
			appendStringInfoChar(&out, c);
	}
	return out.data;
}

/*
 * A remote's head, kept to track.  A branch here is named by stricter rules
 * than one made under older ones may meet ("odd..name", "head", one not ASCII
 * are tracked, and a clone doesn't make a branch of them); but no rules
 * named one "", "HEAD" or with a control character, so a remote sending
 * one is in error.  One not valid in this database's encoding isn't kept
 * as text here: that head alone isn't fetched.
 */
static void
collect_head(const char *branch, const ProllyHash *commit, void *arg)
{
	Heads	   *h = arg;
	bool		control = false;

	for (const char *p = branch; *p; p++)
	{
		if ((unsigned char) *p < 0x20 || (unsigned char) *p == 0x7f)
			control = true;
	}
	if (branch[0] == '\0' || strcmp(branch, "HEAD") == 0 || control)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("remote \"%s\" sent the branch name \"%s\", which no branch can have",
						h->remote, escaped_branch_name(branch))));
	if (!pg_verifymbstr(branch, strlen(branch), true))
	{
		ereport(WARNING,
				(errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
				 errmsg("remote \"%s\"'s branch \"%s\" isn't fetched",
						h->remote, escaped_branch_name(branch)),
				 errdetail("Its name isn't valid in this database's encoding.")));
		return;
	}
	h->branches = lappend(h->branches, pstrdup(branch));
	h->commits = lappend(h->commits, copy_hash(commit));
}

/* A remote's head of branch: false if it hasn't the branch. */
static bool
remote_head(const ProllyRemoteRoutine *rr, ProllyRemote *r, const char *remote,
			const char *branch, ProllyHash *head)
{
	Heads		h = {NIL, NIL, remote};
	ListCell   *b;
	ListCell   *c;

	PROLLY_CALL(rr, heads, r, collect_head, &h);
	forboth(b, h.branches, c, h.commits)
	{
		if (strcmp(lfirst(b), branch) == 0)
		{
			*head = *(ProllyHash *) lfirst(c);
			return true;
		}
	}
	memset(head, 0, sizeof(*head));
	return false;
}

static char *
build_format(void)
{
	ProllyDatabaseComponents dc;
	StringInfoData f;

	prolly_database_components(&dc);
	initStringInfo(&f);
	PROLLY_CALL(dc.canonicalizer, format, &f);
	return f.data;
}

/* Refuse a remote whose commits are of another format than this build's. */
static void
check_remote_format(const ProllyRemoteRoutine *rr, ProllyRemote *r,
					const char *name)
{
	StringInfoData f;
	char	   *mine = build_format();

	initStringInfo(&f);
	PROLLY_CALL(rr, format, r, &f);
	if (f.len > 0 && strcmp(f.data, mine) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("remote \"%s\" holds commits of canonical format %s, not %s",
						name, f.data, mine)));
}

/* A remote's tags: each one's name, tag object and commit. */
typedef struct RemoteTags
{
	List	   *names;			/* char * */
	List	   *tags;			/* ProllyHash * */
	List	   *commits;		/* ProllyHash * */
	const char *remote;			/* the remote's name, for messages */
} RemoteTags;

static void
collect_tag(const char *name, const ProllyHash *tag, const ProllyHash *commit,
			void *arg)
{
	RemoteTags *t = arg;

	if (!pg_verifymbstr(name, strlen(name), true))
	{
		ereport(WARNING,
				(errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
				 errmsg("remote \"%s\"'s tag \"%s\" isn't fetched",
						t->remote, escaped_branch_name(name)),
				 errdetail("Its name isn't valid in this database's encoding.")));
		return;
	}
	t->names = lappend(t->names, pstrdup(name));
	t->tags = lappend(t->tags, copy_hash(tag));
	t->commits = lappend(t->commits, copy_hash(commit));
}

/*
 * Receive the remote's tags of commits this database has, once its
 * branches are fetched, as Dolt's fetch follows tags (or, with all, as a
 * clone takes them, every one, with its commit's history): those whose tag
 * objects differ from the tags here of their names.  *got is what was
 * received, to name here.
 */
static void
receive_tags(const ProllyRemoteRoutine *rr, ProllyRemote *r, const char *name,
			 bool all_tags, RemoteTags *got)
{
	ProllyDatabaseComponents dc;
	RemoteTags	all = {NIL, NIL, NIL, name};
	ListCell   *n;
	ListCell   *t;
	ListCell   *c;
	ProllyHash *tags;
	int			ntags = 0;

	prolly_database_components(&dc);
	memset(got, 0, sizeof(*got));
	if (!PROLLY_HAS(rr, tags) || !PROLLY_HAS(dc.refs, tags))
		return;
	PROLLY_CALL(rr, tags, r, collect_tag, &all);
	forthree(n, all.names, t, all.tags, c, all.commits)
	{
		ProllyHash	mine;
		ProllyCommitInfo info;

		if (PROLLY_CALL(dc.refs, tag_lookup, lfirst(n), &mine) &&
			same_hash(&mine, lfirst(t)))
			continue;
		if (!all_tags && !PROLLY_CALL(dc.refs, commit_get, lfirst(c), &info))
			continue;			/* (a commit not fetched: its tag isn't) */
		got->names = lappend(got->names, lfirst(n));
		got->tags = lappend(got->tags, lfirst(t));
	}
	if (got->tags == NIL)
		return;
	tags = palloc(sizeof(ProllyHash) * list_length(got->tags));
	foreach(t, got->tags)
		tags[ntags++] = *(ProllyHash *) lfirst(t);
	(void) PROLLY_CALL(dc.transfer, receive, rr, r, tags, ntags);
}

/*
 * Fetch remote name's branches (NIL: all of them) into its remote-tracking
 * branches, the remote opened and its heads read once; with need, refuse
 * before receiving anything if the remote hasn't that branch.  *got (if
 * not NULL) is what was fetched: the branches and their commits.  A
 * clone's (into a database with nothing to walk against) copies the
 * remote's files whole, where the transfer can.  Then the remote's tags of
 * commits the database has are fetched too, each set here as the remote
 * has it.
 */
static void
fetch_remote(const char *name, List *only, const char *need, Heads *got,
			 bool clone)
{
	ProllyDatabaseComponents dc;
	char	   *url = need_remote(name);
	const ProllyRemoteRoutine *rr = prolly_remote_for_url(url);
	ProllyRemote *r;
	Heads		all = {NIL, NIL, name};
	Heads		want = {NIL, NIL, name};
	RemoteTags	tags = {0};
	ListCell   *b;
	ListCell   *c;

	prolly_database_components(&dc);
	r = PROLLY_CALL(rr, open, url, false);
	PG_TRY();
	{
		ProllyHash *commits;
		int			n = 0;
		List	   *tag_objects = NIL;

		check_remote_format(rr, r, name);
		PROLLY_CALL(rr, heads, r, collect_head, &all);
		forboth(b, all.branches, c, all.commits)
		{
			ListCell   *o;
			bool		wanted = (only == NIL);

			foreach(o, only)
			{
				if (strcmp(lfirst(o), lfirst(b)) == 0)
					wanted = true;
			}
			if (wanted)
			{
				want.branches = lappend(want.branches, lfirst(b));
				want.commits = lappend(want.commits, lfirst(c));
			}
		}
		if (need)
		{
			bool		has = false;

			foreach(b, all.branches)
				has |= (strcmp(lfirst(b), need) == 0);
			if (!has)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("the remote has no branch \"%s\"", need)));
		}
		foreach(b, only)
		{
			ListCell   *w;
			bool		found = false;

			foreach(w, want.branches)
			{
				if (strcmp(lfirst(w), lfirst(b)) == 0)
					found = true;
			}
			if (!found)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("remote \"%s\" has no branch \"%s\"", name,
								(char *) lfirst(b))));
		}
		/*
		 * (a clone's tags, every one: among what it checks, so the commits
		 * only a tag reaches are checked to be of this build's format, as a
		 * head's are, though the clone copies them with the files)
		 */
		if (clone && PROLLY_HAS(rr, tags) && PROLLY_HAS(dc.refs, tags))
		{
			RemoteTags	rt = {NIL, NIL, NIL, name};

			PROLLY_CALL(rr, tags, r, collect_tag, &rt);
			tag_objects = rt.tags;
		}
		commits = palloc(sizeof(ProllyHash) *
						 Max(list_length(want.commits) + list_length(tag_objects), 1));
		foreach(c, want.commits)
			commits[n++] = *(ProllyHash *) lfirst(c);
		foreach(c, tag_objects)
			commits[n++] = *(ProllyHash *) lfirst(c);
		{
			uint64		count;

			if (!clone || !PROLLY_HAS(dc.transfer, clone) ||
				!PROLLY_CALL(dc.transfer, clone, rr, r, commits, n, &count))
				(void) PROLLY_CALL(dc.transfer, receive, rr, r, commits, n);
		}
		receive_tags(rr, r, name, clone, &tags);
	}
	PG_FINALLY();
	{
		PROLLY_CALL(rr, close, r);
	}
	PG_END_TRY();

	forboth(b, want.branches, c, want.commits)
		PROLLY_CALL(dc.refs, tracking_set, name, lfirst(b), lfirst(c));
	/*
	 * (a tag here of a name the remote's has is set to the remote's, as
	 * Dolt's: tags are shared refs, so only by a role that writes them; for
	 * another, a fetch leaves them as they are.  A fetched tag has no maker
	 * here: its deletion takes a role that writes the refs.)
	 */
	if (tags.names != NIL && !prolly_writes_refs())
		ereport(NOTICE,
				(errmsg("the remote's tags were not fetched"),
				 errdetail("Setting tags takes the database's CREATE privilege or the privileges of the \"pg_prolly_write\" role.")));
	else
		forboth(b, tags.names, c, tags.tags)
			(void) PROLLY_CALL(dc.refs, tag_set, lfirst(b), NULL, lfirst(c));
	if (got)
		*got = want;
}

/*
 * dolt_fetch(VARIADIC args text[]) returns text[]: {status}
 *
 * [remote [branch...]] (origin if none): the remote's branches into its
 * remote-tracking branches (remote/branch), with the chunks they reach that
 * the database lacks.
 */
Datum
dolt_fetch(PG_FUNCTION_ARGS)
{
	char	  **args;
	int			nargs = text_args(fcinfo, &args);
	char	   *remote = NULL;
	List	   *branches = NIL;
	bool		options = true;	/* until "--" */
	const char *result[1] = {"0"};

	for (int i = 0; i < nargs; i++)
	{
		if (options && strcmp(args[i], "--") == 0)
			options = false;	/* names after it, even ones like flags */
		else if (options && args[i][0] == '-')
			unsupported("dolt_fetch", args[i]);
		else if (remote == NULL)
			remote = args[i];
		else
			branches = lappend(branches, args[i]);
	}
	PreventCommandIfReadOnly("dolt_fetch()");
	fetch_remote(remote ? remote : "origin", branches, NULL, NULL, false);
	PG_RETURN_DATUM(text_array(1, result));
}

/*
 * Does the user own the current database, or is a member of pg_maintain,
 * as VACUUM asks?
 */
static bool
may_maintain(void)
{
	return object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId()) ||
		has_privs_of_role(GetUserId(), ROLE_PG_MAINTAIN);
}

/*
 * Collect the current database's chunk store (full: all of it), as the
 * database's owner or a member of pg_maintain may, as VACUUM asks.
 */
static void
collect(bool full, const char *what, ProllyGcResult *gc)
{
	ProllyDatabaseComponents dc;

	if (!may_maintain())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to collect garbage in database \"%s\"",
						get_database_name(MyDatabaseId)),
				 errdetail("Only the database's owner or members of the \"%s\" role may.",
						   "pg_maintain")));
	PreventCommandIfReadOnly(what);
	prolly_database_components(&dc);
	(void) PROLLY_CALL(dc.reclamation, collect, full, false, gc);
	/* (then, a step of its own, the records of the commits it took) */
	if (PROLLY_HAS(dc.refs, forget_records))
		(void) PROLLY_CALL(dc.refs, forget_records);
	ereport(DEBUG1,
			(errmsg_internal("%s: %llu chunks kept, %llu freed, %llu bytes freed, appends held off %llu us",
							 what,
							 (unsigned long long) gc->chunks_kept,
							 (unsigned long long) gc->chunks_freed,
							 (unsigned long long) gc->bytes_freed,
							 (unsigned long long) gc->window_usec)));
}

/*
 * dolt_gc(VARIADIC args text[]) returns text[]: {status}
 *
 * --full: the database's chunk store collected (prolly_reclaim.h): what
 * was written since the last collection (--shallow, Dolt's cheaper
 * collection, too), or (full) all of it.  Commits go
 * on meanwhile but for a short final window; a transaction that read a
 * chunk collected meanwhile and then names it fails, to be retried.  The
 * database's owner, or a member of pg_maintain, as VACUUM asks.
 */
Datum
dolt_gc(PG_FUNCTION_ARGS)
{
	char	  **args;
	int			nargs = text_args(fcinfo, &args);
	bool		full = false;
	bool		shallow = false;
	bool		options = true;	/* until "--" */
	const char *result[1] = {"0"};
	ProllyGcResult gc;

	for (int i = 0; i < nargs; i++)
	{
		if (options && strcmp(args[i], "--") == 0)
			options = false;	/* (and nothing after it) */
		else if (!options)
			unexpected("dolt_gc", args[i]);
		else if (is_option(args[i], "full"))
			full = true;
		else if (is_option(args[i], "shallow"))
			shallow = true;
		else
			unsupported("dolt_gc", args[i]);
	}
	if (full && shallow)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dolt_gc can't --full and --shallow at once")));
	collect(full, "dolt_gc()", &gc);
	PG_RETURN_DATUM(text_array(1, result));
}

Datum
dolt_gc_noargs(PG_FUNCTION_ARGS)
{
	return dolt_gc(fcinfo);
}

/*
 * prolly_gc(full bool, OUT chunks_kept int8, OUT chunks_freed int8,
 *			 OUT bytes_freed int8, OUT window_usec int8) returns record
 *
 * A collection, as dolt_gc() makes, and what it did: the chunks it kept
 * and freed, the bytes freed, and how long appends were held off.
 * (dolt_gc() answers {0}, as Dolt's does.)
 */
Datum
prolly_gc(PG_FUNCTION_ARGS)
{
	bool		full = PG_GETARG_BOOL(0);
	ProllyGcResult gc;
	TupleDesc	desc;
	Datum		values[4];
	bool		nulls[4] = {0};

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	collect(full, "prolly_gc()", &gc);
	values[0] = Int64GetDatum((int64) gc.chunks_kept);
	values[1] = Int64GetDatum((int64) gc.chunks_freed);
	values[2] = Int64GetDatum((int64) gc.bytes_freed);
	values[3] = Int64GetDatum((int64) gc.window_usec);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc),
													  values, nulls)));
}

/* prolly_store_sizes()'s sums */
typedef struct StoreSizes
{
	int64		segments;
	int64		segment_bytes;
	int64		files;
	int64		file_bytes;
	int64		settled;
	int64		settled_bytes;
} StoreSizes;

static void
size_segment(const char *name, int nchunks, uint64 size, bool settled,
			 void *arg)
{
	StoreSizes *ss = arg;

	Assert(!settled);			/* (only table files are) */
	ss->segments++;
	ss->segment_bytes += size;
}

static void
size_file(const char *name, int nchunks, uint64 size, bool settled, void *arg)
{
	StoreSizes *ss = arg;

	ss->files++;
	ss->file_bytes += size;
	if (settled)
	{
		ss->settled++;
		ss->settled_bytes += size;
	}
}

/*
 * prolly_store_sizes(OUT journal_segments int8, OUT journal_bytes int8,
 *					  OUT table_files int8, OUT table_file_bytes int8,
 *					  OUT settled_files int8, OUT settled_bytes int8)
 *	 returns record
 *
 * The current database's chunk store as it stands, as Dolt's StoreSizes:
 * its journal (Dolt's journal), its table files, and of those the settled
 * ones (closed under what they name: Dolt's oldgen; the rest its newgen).
 * Only sizes are read.  Approximate while the store changes: the segments
 * and the table files are listed one after the other, so a move or a
 * collection between may count bytes in both, or neither; a segment whose
 * file a collection has just removed counts with no bytes.
 */
Datum
prolly_store_sizes(PG_FUNCTION_ARGS)
{
	const ProllyChunkStoreRoutine *cs = prolly_chunk_store();
	ProllyChunkStore *store = PROLLY_CALL(cs, open, MyDatabaseId,
										  MyDatabaseTableSpace);
	StoreSizes	ss = {0};
	TupleDesc	desc;
	Datum		values[6];
	bool		nulls[6] = {0};

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	PROLLY_CALL(cs, segments, store, false, size_segment, &ss);
	PROLLY_CALL(cs, table_files, store, size_file, &ss);
	values[0] = Int64GetDatum(ss.segments);
	values[1] = Int64GetDatum(ss.segment_bytes);
	values[2] = Int64GetDatum(ss.files);
	values[3] = Int64GetDatum(ss.file_bytes);
	values[4] = Int64GetDatum(ss.settled);
	values[5] = Int64GetDatum(ss.settled_bytes);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc),
													  values, nulls)));
}

/* A block-mapped relation's storage, on any branch, and what it is. */
typedef struct MappedFile
{
	Oid			spcOid;			/* (the key) */
	RelFileNumber relnumber;
	char		kind;			/* 'i': an index; 'l': a change log */
} MappedFile;

/* prolly_storage_breakdown()'s sums over one tablespace's page store */
typedef struct MappedSizes
{
	HTAB	   *files;			/* MappedFile, of indexes and change logs */
	Oid			spcOid;
	int64		heap_main;
	int64		index_main;
} MappedSizes;

static void
size_mapped(RelFileNumber relnumber, BlockNumber nblocks, void *arg)
{
	MappedSizes *ms = arg;
	MappedFile	key = {ms->spcOid, relnumber};
	MappedFile *found = hash_search(ms->files, &key, HASH_FIND, NULL);

	if (found == NULL)
		ms->heap_main += (int64) nblocks * BLCKSZ;
	else if (found->kind == 'i')
		ms->index_main += (int64) nblocks * BLCKSZ;
	/* (a change log's are change_log_bytes') */
}

/*
 * Every branch's indexes' and change logs' file numbers, in a set.  A log
 * is known by its pg_class row alone (prolly_is_change_log_class()): its
 * table may be on another branch only, where the session branch's name
 * lookup wouldn't find its log.
 */
static HTAB *
mapped_files(void)
{
	HASHCTL		ctl = {.keysize = offsetof(MappedFile, kind),
	.entrysize = sizeof(MappedFile),.hcxt = CurrentMemoryContext};
	HTAB	   *set = hash_create("mapped files", 256, &ctl,
								  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	Relation	classes = table_open(RelationRelationId, AccessShareLock);

	CatalogAllBranches = true;
	PG_TRY();
	{
		SysScanDesc scan = systable_beginscan(classes, InvalidOid, false,
											  NULL, 0, NULL);
		HeapTuple	tup;

		while ((tup = systable_getnext(scan)) != NULL)
		{
			Form_pg_class c = (Form_pg_class) GETSTRUCT(tup);
			MappedFile	key;
			MappedFile *entry;

			if (!RelFileNumberIsValid(c->relfilenode) ||
				(c->relkind != RELKIND_INDEX && !prolly_is_change_log_class(c)))
				continue;
			key.spcOid = OidIsValid(c->reltablespace) ? c->reltablespace :
				MyDatabaseTableSpace;
			key.relnumber = c->relfilenode;
			entry = hash_search(set, &key, HASH_ENTER, NULL);
			entry->kind = c->relkind == RELKIND_INDEX ? 'i' : 'l';
		}
		systable_endscan(scan);
	}
	PG_FINALLY();
	{
		CatalogAllBranches = false;
	}
	PG_END_TRY();
	table_close(classes, AccessShareLock);
	return set;
}

/*
 * prolly_storage_breakdown(OUT heap_main_bytes int8, OUT index_main_bytes int8,
 *						    OUT vm_bytes int8, OUT fsm_bytes int8,
 *						    OUT page_store_bytes int8,
 *						    OUT page_store_allocated_bytes int8,
 *						    OUT page_store_mapped_bytes int8,
 *						    OUT page_store_listed_bytes int8,
 *						    OUT block_map_bytes int8,
 *						    OUT change_log_bytes int8,
 *						    OUT change_log_live_bytes int8,
 *						    OUT change_log_dead_bytes int8,
 *						    OUT journal_bytes int8, OUT settled_bytes int8,
 *						    OUT unsettled_bytes int8,
 *						    OUT chunk_store_other_bytes int8)
 *	 returns record
 *
 * What the current database's prolly storage takes on disk, each component
 * reporting its own part (#500):
 *
 * - its block-mapped relations (prolly tables, their TOAST tables, indexes
 *   and change logs, on every branch), in every tablespace: their main
 *   forks' bytes as mapped, tables' (with their TOAST tables') and
 *   indexes' apart, logs' in neither (a page two branches share counts for
 *   each), and their visibility maps' and free space maps' files;
 * - the page stores: their files, and of their blocks those allocated,
 *   those a relation maps, and those held only otherwise (pages as of a
 *   fold a changes list keeps, entries past a relation's length);
 * - the block maps: the maps', changes lists', allocators' and reference
 *   counts' files;
 * - the session branch's tables' change logs: their relations' bytes, and
 *   their entries' live and dead (consumed, until vacuumed) by heap
 *   visibility; NULL if the change log can't say.  Their pages are the
 *   page store's;
 * - the chunk store: its journal, its table files, settled or not, and its
 *   other files (NULL if it can't say).
 *
 * The files of vm, fsm, page_store, block_map, journal, settled, unsettled
 * and chunk_store_other are apart, and with the database's other
 * relations (its catalogs) and the few files no relation owns (the
 * relation map, the relcache's init file) they are pg_database_size().
 * The other columns are parts of the page store.  Approximate while the
 * storage changes, as prolly_store_sizes() is.
 */
Datum
prolly_storage_breakdown(PG_FUNCTION_ARGS)
{
	const ProllyChunkStoreRoutine *cs = prolly_chunk_store();
	ProllyChunkStore *store = PROLLY_CALL(cs, open, MyDatabaseId,
										  MyDatabaseTableSpace);
	StoreSizes	ss = {0};
	BmapSizes	total = {0};
	MappedSizes ms = {0};
	ProllyChangeLogSizes logs = {0};
	bool		logs_known = true;
	List	   *spcs = NIL;
	List	   *tables = NIL;
	Relation	rel;
	SysScanDesc scan;
	HeapTuple	tup;
	TupleDesc	desc;
	Datum		values[16];
	bool		nulls[16] = {0};

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/* the page stores, in every tablespace */
	rel = table_open(TableSpaceRelationId, AccessShareLock);
	scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		Oid			spc = ((Form_pg_tablespace) GETSTRUCT(tup))->oid;

		if (spc != GLOBALTABLESPACE_OID)
			spcs = lappend_oid(spcs, spc);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	ms.files = mapped_files();
	foreach_oid(spc, spcs)
	{
		BmapSizes	bs;

		ms.spcOid = spc;
		bmap_sizes(spc, MyDatabaseId, size_mapped, &ms, &bs);
		total.store_bytes += bs.store_bytes;
		total.allocated_blocks += bs.allocated_blocks;
		total.mapped_blocks += bs.mapped_blocks;
		total.listed_blocks += bs.listed_blocks;
		total.map_bytes += bs.map_bytes;
		total.alloc_bytes += bs.alloc_bytes;
		total.vm_bytes += bs.vm_bytes;
		total.fsm_bytes += bs.fsm_bytes;
	}
	hash_destroy(ms.files);

	/* the change logs of the session branch's tables */
	rel = table_open(RelationRelationId, AccessShareLock);
	scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_class c = (Form_pg_class) GETSTRUCT(tup);

		if (c->relam == PROLLY_TABLE_AM_OID &&
			(c->relkind == RELKIND_RELATION || c->relkind == RELKIND_MATVIEW))
			tables = lappend_oid(tables, c->oid);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	foreach_oid(relid, tables)
	{
		Relation	table = try_table_open(relid, AccessShareLock);
		ProllyChangeLogSizes one;

		if (table == NULL)
			continue;			/* (dropped meanwhile) */
		if (prolly_change_log_sizes(table, &one))
		{
			logs.bytes += one.bytes;
			logs.live_bytes += one.live_bytes;
			logs.dead_bytes += one.dead_bytes;
		}
		else
			logs_known = false;
		table_close(table, AccessShareLock);
	}

	/* the chunk store */
	PROLLY_CALL(cs, segments, store, false, size_segment, &ss);
	PROLLY_CALL(cs, table_files, store, size_file, &ss);

	values[0] = Int64GetDatum(ms.heap_main);
	values[1] = Int64GetDatum(ms.index_main);
	values[2] = Int64GetDatum((int64) total.vm_bytes);
	values[3] = Int64GetDatum((int64) total.fsm_bytes);
	values[4] = Int64GetDatum((int64) total.store_bytes);
	values[5] = Int64GetDatum((int64) total.allocated_blocks * BLCKSZ);
	values[6] = Int64GetDatum((int64) total.mapped_blocks * BLCKSZ);
	values[7] = Int64GetDatum((int64) total.listed_blocks * BLCKSZ);
	values[8] = Int64GetDatum((int64) (total.map_bytes + total.alloc_bytes));
	values[9] = Int64GetDatum(logs.bytes);
	values[10] = Int64GetDatum(logs.live_bytes);
	values[11] = Int64GetDatum(logs.dead_bytes);
	nulls[9] = nulls[10] = nulls[11] = !logs_known;
	values[12] = Int64GetDatum(ss.segment_bytes);
	values[13] = Int64GetDatum(ss.settled_bytes);
	values[14] = Int64GetDatum(ss.file_bytes - ss.settled_bytes);
	if (cs->other_bytes != NULL)
		values[15] = Int64GetDatum(cs->other_bytes(store));
	else
		nulls[15] = true;
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc),
													  values, nulls)));
}

/*
 * prolly_make_change_logs() returns setof regclass
 *
 * The change logs the session branch's prolly tables lack, made: tables
 * made before logs were, or restored by pg_upgrade (which makes none),
 * fold in full on every commit until one is.  Each table is locked as an
 * ALTER that makes one locks it (ShareLock: its writers wait, so none
 * writes rows the new log misses), in turn.  The tables given one.  An
 * upgrade step, run on each branch.
 */
Datum
prolly_make_change_logs(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	classes;
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *relids = NIL;

	PreventCommandIfReadOnly("prolly_make_change_logs()");
	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	classes = table_open(RelationRelationId, AccessShareLock);
	scan = systable_beginscan(classes, InvalidOid, false, NULL, 0, NULL);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_class c = (Form_pg_class) GETSTRUCT(tup);

		if (c->relam == PROLLY_TABLE_AM_OID &&
			(c->relkind == RELKIND_RELATION || c->relkind == RELKIND_MATVIEW) &&
			!OidIsValid(prolly_change_log_relid(c->oid, c->relam)))
			relids = lappend_oid(relids, c->oid);
	}
	systable_endscan(scan);
	table_close(classes, AccessShareLock);

	foreach_oid(relid, relids)
	{
		Datum		value = ObjectIdGetDatum(relid);
		bool		null = false;

		if (!object_ownercheck(RelationRelationId, relid, GetUserId()))
			continue;			/* (another's: theirs to make) */
		prolly_make_change_log(relid, ShareLock);
		if (OidIsValid(prolly_change_log_relid(relid, PROLLY_TABLE_AM_OID)))
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, &value, &null);
	}
	return (Datum) 0;
}

/*
 * prolly_page_sweep(OUT freed int8, OUT recounted int8, OUT truncated int8,
 *				   OUT moved int8)
 *
 * The current database's page stores swept (bmap_sweep()), in every
 * tablespace that has one: blocks nothing holds freed, stuck reference
 * counts rewritten exactly, the blocks past what they hold moved down and
 * the stores shortened.  Its owner, or a member
 * of pg_maintain, as dolt_gc().
 */
Datum
prolly_page_sweep(PG_FUNCTION_ARGS)
{
	TupleDesc	desc;
	Datum		values[4];
	bool		nulls[4] = {0};
	BmapSweep	r;

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	if (!may_maintain())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to sweep the page store in database \"%s\"",
						get_database_name(MyDatabaseId)),
				 errdetail("Only the database's owner or members of the \"%s\" role may.",
						   "pg_maintain")));
	PreventCommandIfReadOnly("prolly_page_sweep()");
	prolly_sweep_page_stores(false, &r);
	values[0] = Int64GetDatum((int64) r.freed);
	values[1] = Int64GetDatum((int64) r.recounted);
	values[2] = Int64GetDatum((int64) r.truncated);
	values[3] = Int64GetDatum((int64) r.moved);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc),
													  values, nulls)));
}

/* The session branch's upstream, or an error naming how to set one. */
static void
need_upstream(const char *fn, char **remote, char **branch)
{
	const char *current = branch_ops()->current();

	if (!PROLLY_CALL(remote_refs(), upstream, current, remote, branch))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("the current branch %s has no upstream branch", current),
				 errhint("Name the remote, as %s('origin', '%s'), or set an upstream with dolt_push('--set-upstream', 'origin', '%s').",
						 fn, current, current)));
}

typedef struct TagFind
{
	const char *name;
	ProllyHash	tag;
	bool		found;
} TagFind;

static void
find_tag(const char *name, const ProllyHash *tag, const ProllyHash *commit,
		 void *arg)
{
	TagFind    *tf = arg;

	if (strcmp(name, tf->name) == 0)
	{
		tf->tag = *tag;
		tf->found = true;
	}
}

/*
 * Push tag name to remote, as Dolt does: its tag object and what that
 * reaches sent, then the remote's tag of that name made it, whatever tag
 * it was (a compare-and-swap with the one read, so a change meanwhile is
 * refused rather than lost).
 */
static Datum
push_tag(const char *remote, const char *name)
{
	ProllyDatabaseComponents dc;
	const char *url = need_remote(remote);
	const ProllyRemoteRoutine *rr = prolly_remote_for_url(url);
	ProllyRemote *r;
	ProllyHash	tag;
	ProllyTagInfo info;
	const char *result[2] = {"0", NULL};

	PreventCommandIfReadOnly("dolt_push()");
	prolly_database_components(&dc);
	if (!PROLLY_HAS(dc.refs, tag_lookup) ||
		!PROLLY_CALL(dc.refs, tag_lookup, name, &tag) ||
		!PROLLY_CALL(dc.refs, tag_get, &tag, &info))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tag \"%s\" not found", name)));
	if (!PROLLY_HAS(rr, move_tag))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("remote \"%s\" keeps no tags", remote)));
	r = PROLLY_CALL(rr, open, url, true);
	PG_TRY();
	{
		TagFind		tf = {name, {0}, false};

		check_remote_format(rr, r, remote);
		PROLLY_CALL(rr, tags, r, find_tag, &tf);
		if (tf.found && same_hash(&tf.tag, &tag))
			result[1] = "Everything up-to-date";
		else
		{
			(void) PROLLY_CALL(dc.transfer, send, rr, r, &tag, 1);
			if (!PROLLY_CALL(rr, move_tag, r, name, &tf.tag, &tag, &info.commit,
							 build_format()))
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("the push of tag %s to %s was rejected: the remote's tag moved meanwhile",
								name, remote),
						 errhint("Try again.")));
			result[1] = psprintf("* [new tag] %s -> %s", name, name);
		}
	}
	PG_FINALLY();
	{
		PROLLY_CALL(rr, close, r);
	}
	PG_END_TRY();
	return text_array(2, result);
}

/*
 * dolt_push(VARIADIC args text[]) returns text[]: {status, message}
 *
 * [remote [branch]], -f/--force, -u/--set-upstream: branch (the session's
 * if not named) pushed to the remote's branch of the same name; with no
 * remote, the session branch to its upstream (which names the remote and
 * the remote's branch); remote tag (a tag's name that is no branch's, or
 * refs/tags/tag): the tag, as push_tag() pushes it.  The chunks the branch's head reaches that the
 * remote lacks are sent, then the remote's branch moved to it, only as a
 * fast-forward unless forced.  The remote's branch
 * is moved as it is reached, not with the transaction: the remote-tracking
 * branch is, and a fetch finds the remote as it is.
 */
Datum
dolt_push(PG_FUNCTION_ARGS)
{
	ProllyDatabaseComponents dc;
	char	  **args;
	int			nargs = text_args(fcinfo, &args);
	char	   *remote = NULL;
	char	   *branch = NULL;
	char	   *remote_branch = NULL;
	bool		force = false;
	bool		set_upstream = false;
	bool		options = true;	/* until "--" */
	char	   *url;
	const ProllyRemoteRoutine *rr;
	ProllyRemote *r;
	ProllyHash	local;
	ProllyHash	theirs;
	bool		had;
	const char *result[2] = {"0", NULL};

	for (int i = 0; i < nargs; i++)
	{
		char	   *a = args[i];

		if (options && strcmp(a, "--") == 0)
			options = false;	/* names after it, even ones like flags */
		else if (options && a[0] == '-')
		{
			if (is_option(a, "f") || is_option(a, "force"))
				force = true;
			else if (is_option(a, "u") || is_option(a, "set-upstream"))
				set_upstream = true;
			/* short flags together: -fu */
			else if (short_flags(a) != NULL &&
					 strspn(short_flags(a), "fu") == strlen(short_flags(a)))
			{
				force |= strchr(short_flags(a), 'f') != NULL;
				set_upstream |= strchr(short_flags(a), 'u') != NULL;
			}
			else
				unsupported("dolt_push", a);
		}
		else if (remote == NULL)
			remote = a;
		else if (branch == NULL)
			branch = a;
		else
			unexpected("dolt_push", a);
	}
	/*
	 * A tag, named as one ("refs/tags/v1") or by a name that is a tag's
	 * and no branch's: pushed as Dolt pushes one.
	 */
	if (remote != NULL && branch != NULL)
	{
		const char *tag = NULL;

		if (strncmp(branch, "refs/tags/", 10) == 0 && branch[10] != '\0')
			tag = branch + 10;
		else if (!PROLLY_CALL(remote_refs(), branch_lookup, branch,
							  &(ProllyBranchId) {0}) && names_tag(branch))
			tag = branch;
		if (tag != NULL)
		{
			if (set_upstream)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("dolt_push: a tag has no upstream")));
			PG_RETURN_DATUM(push_tag(remote, tag));
		}
	}
	if (remote == NULL)
		need_upstream("dolt_push", &remote, &remote_branch);
	if (branch == NULL)
		branch = pstrdup(branch_ops()->current());
	if (remote_branch == NULL)
		remote_branch = branch;
	/* (a commit's hidden branch, a session's, isn't a remote's: as it serves) */
	if (detached_branch(remote_branch))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("branch names beginning \"%s\" are reserved", PROLLY_DETACHED_PREFIX),
				 errhint("Push a branch made at the commit with dolt_branch().")));
	PreventCommandIfReadOnly("dolt_push()");
	if (!PROLLY_CALL(remote_refs(), branch_lookup, branch, &(ProllyBranchId) {0}))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("branch \"%s\" not found", branch)));
	if (!PROLLY_CALL(remote_refs(), resolve, branch, &local))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("branch \"%s\" has no commits to push", branch)));
	url = need_remote(remote);
	rr = prolly_remote_for_url(url);
	prolly_database_components(&dc);
	r = PROLLY_CALL(rr, open, url, true);
	PG_TRY();
	{
		check_remote_format(rr, r, remote);
		had = remote_head(rr, r, remote, remote_branch, &theirs);
		if (had && same_hash(&theirs, &local))
			result[1] = "Everything up-to-date";
		else
		{
			if (had && !force)
			{
				ProllyHash	base;

				if (!PROLLY_CALL(prolly_chunk_store(), has,
								 PROLLY_CALL(prolly_chunk_store(), open,
											 MyDatabaseId, MyDatabaseTableSpace),
								 &theirs))
					ereport(ERROR,
							(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							 errmsg("the push to %s/%s was rejected: the remote has commits this database lacks",
									remote, remote_branch),
							 errhint("Fetch and merge them first, or push with --force.")));
				if (!PROLLY_CALL(remote_refs(), merge_base, &theirs, &local, &base) ||
					!same_hash(&base, &theirs))
					ereport(ERROR,
							(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							 errmsg("the push to %s/%s was rejected: it isn't a fast-forward",
									remote, remote_branch),
							 errhint("Merge the remote's branch first, or push with --force.")));
			}
			(void) PROLLY_CALL(dc.transfer, send, rr, r, &local, 1);
			if (!PROLLY_CALL(rr, move_head, r, remote_branch, &theirs, &local,
							 build_format(), force))
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("the push to %s/%s was rejected: the remote's branch moved meanwhile",
								remote, remote_branch),
						 errhint("Fetch and try again.")));
			result[1] = psprintf("%s -> %s/%s", branch, remote, remote_branch);
		}
	}
	PG_FINALLY();
	{
		PROLLY_CALL(rr, close, r);
	}
	PG_END_TRY();
	PROLLY_CALL(dc.refs, tracking_set, remote, remote_branch, &local);
	if (set_upstream)
		PROLLY_CALL(dc.refs, upstream_set, branch, remote, remote_branch);
	PG_RETURN_DATUM(text_array(2, result));
}

/*
 * dolt_pull(VARIADIC args text[]) returns text[]: as dolt_merge()
 *
 * [remote [branch]] (the session branch's upstream if none; the session
 * branch's name if only the remote): the remote's branch fetched, then
 * merged into the session branch as dolt_merge() merges, with its options
 * (--no-ff, -m).
 */
Datum
dolt_pull(PG_FUNCTION_ARGS)
{
	char	  **args;
	int			nargs = text_args(fcinfo, &args);
	char	   *remote = NULL;
	char	   *branch = NULL;
	List	   *merge_args = NIL;
	bool		options = true;	/* until "--" */
	Datum	   *d;
	int			n = 0;
	ListCell   *lc;

	for (int i = 0; i < nargs; i++)
	{
		char	   *a = args[i];

		if (options && strcmp(a, "--") == 0)
			options = false;	/* names after it, even ones like flags */
		else if (options && a[0] == '-')
		{
			if (is_option(a, "no-ff"))
				merge_args = lappend(merge_args, a);
			else if (is_option(a, "m") || is_option(a, "message"))
			{
				char	   *message = option_value(args, nargs, &i, "dolt_pull");

				/* (before the fetch, as dolt_merge would refuse it after) */
				given_message("dolt_pull", message);
				merge_args = lappend(merge_args, a);
				merge_args = lappend(merge_args, message);
			}
			else
				unsupported("dolt_pull", a);
		}
		else if (remote == NULL)
			remote = a;
		else if (branch == NULL)
			branch = a;
		else
			unexpected("dolt_pull", a);
	}
	if (remote == NULL)
		need_upstream("dolt_pull", &remote, &branch);
	if (branch == NULL)
		branch = pstrdup(branch_ops()->current());
	PreventCommandIfReadOnly("dolt_pull()");
	prolly_check_head_move(branch_ops()->current());
	fetch_remote(remote, list_make1(branch), NULL, NULL, false);

	/* (after "--": a remote's name may begin with "-") */
	merge_args = lappend(merge_args, "--");
	merge_args = lappend(merge_args, psprintf("%s/%s", remote, branch));
	d = palloc(sizeof(Datum) * list_length(merge_args));
	foreach(lc, merge_args)
		d[n++] = CStringGetTextDatum(lfirst(lc));
	return DirectFunctionCall1(dolt_merge,
							   PointerGetDatum(construct_array_builtin(d, n, TEXTOID)));
}

/* dolt_fetch(), dolt_push(), dolt_pull(): with no arguments */
Datum
dolt_fetch_noargs(PG_FUNCTION_ARGS)
{
	return dolt_fetch(fcinfo);
}

Datum
dolt_push_noargs(PG_FUNCTION_ARGS)
{
	return dolt_push(fcinfo);
}

Datum
dolt_pull_noargs(PG_FUNCTION_ARGS)
{
	return dolt_pull(fcinfo);
}

static void
remote_row(const char *name, const char *url, void *arg)
{
	ReturnSetInfo *rsinfo = arg;
	Datum		values[4];
	bool		nulls[4] = {0};
	StringInfoData spec;
	StringInfoData specs;

	values[0] = CStringGetTextDatum(name);
	values[1] = CStringGetTextDatum(url);

	/*
	 * Dolt's columns (#586): the one fetch spec a fetch follows (every
	 * branch to its remote-tracking branch), and no parameters (a remote is
	 * its URL).
	 */
	initStringInfo(&spec);
	appendStringInfo(&spec, "refs/heads/*:refs/remotes/%s/*", name);
	initStringInfo(&specs);
	appendStringInfoChar(&specs, '[');
	escape_json(&specs, spec.data);
	appendStringInfoChar(&specs, ']');
	values[2] = DirectFunctionCall1(jsonb_in, CStringGetDatum(specs.data));
	values[3] = DirectFunctionCall1(jsonb_in, CStringGetDatum("{}"));
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

/*
 * dolt_remotes(OUT name text, OUT url text, OUT fetch_specs jsonb,
 *				OUT params jsonb) returns setof record
 */
Datum
dolt_remotes(PG_FUNCTION_ARGS)
{
	InitMaterializedSRF(fcinfo, 0);
	PROLLY_CALL(remote_refs(), remotes, remote_row, fcinfo->resultinfo);
	return (Datum) 0;
}

static void
tracking_row(const char *remote, const char *branch, const ProllyHash *commit,
			 void *arg)
{
	ReturnSetInfo *rsinfo = arg;
	Datum		values[2];
	bool		nulls[2] = {0};

	values[0] = CStringGetTextDatum(psprintf("remotes/%s/%s", remote, branch));
	values[1] = CStringGetTextDatum(hash_hex(commit));
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

/*
 * dolt_remote_branches(OUT name text, OUT hash text) returns setof record:
 * the remote-tracking branches, as remotes/remote/branch.
 */
Datum
dolt_remote_branches(PG_FUNCTION_ARGS)
{
	InitMaterializedSRF(fcinfo, 0);
	PROLLY_CALL(remote_refs(), trackings, NULL, tracking_row, fcinfo->resultinfo);
	return (Datum) 0;
}

/* What a database has that a clone would run into, one string a kind. */
typedef struct InTheWay
{
	const char *current;
	StringInfoData branches;
	StringInfoData remotes;
	StringInfoData upstreams;
} InTheWay;

static void
list_name(StringInfo list, const char *name)
{
	if (list->len > 0)
		appendStringInfoString(list, ", ");
	appendStringInfoString(list, name);
}

static void
note_branch(const char *name, const ProllyHash *head, void *arg)
{
	InTheWay   *w = arg;
	char	   *remote;
	char	   *remote_branch;

	/* (the session's own branch is in the way only once it has commits) */
	if (head->len > 0 || strcmp(name, w->current) != 0)
		list_name(&w->branches, name);
	if (PROLLY_CALL(remote_refs(), upstream, name, &remote, &remote_branch))
		list_name(&w->upstreams, name);
}

static void
note_remote(const char *name, const char *url, void *arg)
{
	list_name(&((InTheWay *) arg)->remotes, name);
}

/*
 * Refuse a clone into a database that isn't empty: no versioned schemas,
 * no branch but the session's (with no commits), no remotes, no upstreams;
 * naming what is there.
 */
static void
check_clone_target(const char *current)
{
	InTheWay	w;
	StringInfoData what;

	w.current = current;
	initStringInfo(&w.branches);
	initStringInfo(&w.remotes);
	initStringInfo(&w.upstreams);
	initStringInfo(&what);
	PROLLY_CALL(remote_refs(), branches, note_branch, &w);
	PROLLY_CALL(remote_refs(), remotes, note_remote, &w);
	if (database_has_versioned_schemas())
		appendStringInfoString(&what, "versioned schemas");
	if (w.branches.len > 0)
		appendStringInfo(&what, "%sbranches %s", what.len ? "; " : "", w.branches.data);
	if (w.remotes.len > 0)
		appendStringInfo(&what, "%sremotes %s", what.len ? "; " : "", w.remotes.data);
	if (w.upstreams.len > 0)
		appendStringInfo(&what, "%supstreams of %s", what.len ? "; " : "", w.upstreams.data);
	if (what.len > 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("dolt_clone needs an empty database"),
				 errdetail("This one has %s.", what.data),
				 errhint("Clone into a new database.")));
}

/*
 * Is a remote's branch made a branch here by a clone?  Not one whose name
 * this database's rules refuse, made under older ones, nor one named as a
 * commit's hidden branch (which a served database doesn't list, but a file
 * remote may hold): made here, it would be where sessions detached at the
 * commit it names go, though at another commit.
 */
static bool
cloned_as_branch(const char *name)
{
	return branch_ops()->name_ok(name) && !detached_branch(name);
}

/*
 * dolt_clone(VARIADIC args text[]) returns text[]: {status}
 *
 * url, [--remote name] (origin), [--branch branch]: in an empty database
 * (no versioned schemas, no branch but the session's, which has no
 * commits, no remotes), the remote added and fetched, its heads read once,
 * each of its branches made a branch here (head-only until used) with the
 * remote's as its upstream, and the session checked out on branch (else
 * the remote's main, master, or first by name), which makes its copies.
 * The session's own branch, if the remote has one of its name, is moved to
 * it; if not, it stays, empty (main, the database's, can't be deleted).
 * Before any branch is made, what every branch's head needs from outside
 * the versioned schemas is checked (prolly_deps.h), and the extensions the
 * database lacks are made.  All in the transaction: a clone that fails
 * leaves nothing but the files it copied whole, which a clone tried again
 * doesn't copy again: out of what the store reads if it failed copying or
 * checking them, but once they are checked the store takes them at once,
 * so one that fails after (making its branches) leaves them named, its
 * garbage until the store's next collection.
 */
Datum
dolt_clone(PG_FUNCTION_ARGS)
{
	char	  **args;
	int			nargs = text_args(fcinfo, &args);
	char	   *url = NULL;
	char	   *remote = "origin";
	char	   *branch = NULL;
	bool		options = true;	/* until "--" */
	const char *current = branch_ops()->current();
	Heads		h = {NIL, NIL};
	ListCell   *lc;
	const char *result[1] = {"0"};

	for (int i = 0; i < nargs; i++)
	{
		char	   *a = args[i];

		if (options && strcmp(a, "--") == 0)
			options = false;	/* a URL after it */
		else if (options && a[0] == '-')
		{
			if (is_option(a, "remote"))
				remote = option_value(args, nargs, &i, "dolt_clone");
			else if (is_option(a, "b") || is_option(a, "branch"))
				branch = option_value(args, nargs, &i, "dolt_clone");
			else
				unsupported("dolt_clone", a);
		}
		else if (url == NULL)
			url = a;
		else
			unexpected("dolt_clone", a);
	}
	if (url == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dolt_clone needs a remote's URL")));
	if (current == NULL ||
		strncmp(current, PROLLY_DETACHED_PREFIX, strlen(PROLLY_DETACHED_PREFIX)) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("dolt_clone needs the session on a branch")));
	if (branch != NULL && detached_branch(branch))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("branch names beginning \"%s\" are reserved", PROLLY_DETACHED_PREFIX)));
	check_clone_target(current);

	/* the remote added as dolt_remote() adds one, and fetched */
	add_remote(remote, url);
	PreventCommandIfReadOnly("dolt_clone()");
	fetch_remote(remote, NIL, branch, &h, true);
	if (h.branches == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("the remote has no branches to clone")));
	if (branch == NULL)
	{
		foreach(lc, h.branches)
		{
			if (strcmp(lfirst(lc), "main") == 0)
				branch = "main";
			else if (strcmp(lfirst(lc), "master") == 0 && branch == NULL)
				branch = "master";
		}
		foreach(lc, h.branches)
		{
			if (branch == NULL && cloned_as_branch(lfirst(lc)))
				branch = lfirst(lc);
		}
		if (branch == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_NAME),
					 errmsg("the remote has no branches whose names are a branch's here")));
	}

	/*
	 * What every branch's head needs from outside the versioned schemas,
	 * checked before anything is made; then the extensions the database
	 * lacks, made.  (A head that didn't record them can't be checked: what
	 * it lacks fails when its copies are made, as before.)
	 */
	{
		List	   *trees = NIL;
		ListCell   *hc;
		ListCell   *bc;

		/* (the branch checked out first: its configuration rows win) */
		forboth(hc, h.commits, bc, h.branches)
		{
			ProllyCommitInfo info;

			if (!PROLLY_CALL(refs(), commit_get, lfirst(hc), &info))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("a fetched commit is missing")));
			if (info.has_dependencies)
			{
				ProllyNodeRef *tree = palloc(sizeof(ProllyNodeRef));

				*tree = info.dependencies;
				if (strcmp(lfirst(bc), branch) == 0)
					trees = lcons(tree, trees);
				else
					trees = lappend(trees, tree);
			}
		}
		prolly_check_dependencies(trees, true, "clone");
	}

	foreach(lc, h.branches)
	{
		char	   *b = lfirst(lc);
		char	   *tracking = psprintf("%s/%s", remote, b);

		/*
		 * (a remote branch whose name this database refuses: tracked, not
		 * made here)
		 */
		if (!cloned_as_branch(b))
		{
			ereport(WARNING,
					(errcode(ERRCODE_INVALID_NAME),
					 errmsg("remote branch \"%s\" isn't cloned as a branch", b),
					 detached_branch(b) ?
					 errdetail("Names beginning \"%s\" are reserved.", PROLLY_DETACHED_PREFIX) :
					 errdetail("Its name isn't a branch's by this database's rules."),
					 errhint("Its commits are fetched: dolt_branch(name, '%s') makes a branch of it under another name.",
							 tracking)));
			continue;
		}
		/* (the session's branch, empty, moved to the remote's) */
		if (strcmp(b, current) == 0)
		{
			ProllyHash	at = {0};

			(void) PROLLY_CALL(refs(), resolve, tracking, &at);
			VERSION_MOVE(&at, at.len > 0 ? 1 : 0,
						 PROLLY_CALL(remote_refs(), reset, tracking));
		}
		else
			create_branch(b, tracking);
		PROLLY_CALL(remote_refs(), upstream_set, b, remote, b);
	}
	if (strcmp(branch, current) != 0)
		checkout_branch(branch);
	PG_RETURN_DATUM(text_array(1, result));
}
