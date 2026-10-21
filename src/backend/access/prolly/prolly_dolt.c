/*-------------------------------------------------------------------------
 *
 * prolly_dolt.c
 *	  Doltgres's SQL interface to version control: the dolt_* functions
 *	  and the functions behind the dolt_log, dolt_branches, dolt_status
 *	  and dolt_conflicts system views, the dolt.author and
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
#include "access/prolly_config.h"
#include "access/prolly_deps.h"
#include "access/prolly_xact.h"
#include "access/table.h"
#include "access/xact.h"
#include "access/tableam.h"
#include "catalog/branch.h"
#include "catalog/branch_ops.h"
#include "catalog/namespace.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_authid_d.h"
#include "catalog/pg_branch.h"
#include "catalog/pg_database_d.h"
#include "catalog/pg_type.h"
#include "catalog/versioned.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/miscnodes.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "nodes/supportnodes.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "parser/parse_type.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/json.h"
#include "utils/jsonb.h"
#include "utils/regproc.h"
#include "utils/resowner.h"
#include "nodes/makefuncs.h"
#include "utils/datum.h"
#include "utils/varlena.h"
#include "utils/fmgrprotos.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

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
		prolly_refs_end_hook = transaction_commit_at_end;
		prolly_writers_init();
		RegisterBranchWriteCallback(writers_note_write, NULL);
		writer_branch_hooks();
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
		if (txn_commit_on)
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
	if (txn_commit_lxid != MyProc->vxid.lxid || !txn_commit_on ||
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
 * is on).
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
	bool		options = true;	/* until "--" */
	ProllyHash	id = {0};
	bool		committed = false;
	ProllyMergeState ms;
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
	author_override = author;
	commit_forced = force;
	date_given = dated;
	PG_TRY();
	{
		note_integration();
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
		branch_ops()->check_create(name);
		written = branches_written();

		/*
		 * (no commit to move to, or from: a branch without commits, made
		 * again as a copy of the start point as it is)
		 */
		if (!PROLLY_CALL(refs(), resolve, from ? from : "HEAD", &head) ||
			!PROLLY_CALL(refs(), resolve, name, &at))
		{
			PROLLY_CALL(refs(), branch_delete, name, true);
			create_branch(name, from);
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
} DetachedBranches;

static void
add_detached(const char *name, const ProllyHash *head, void *arg)
{
	DetachedBranches *db = arg;

	if (strcmp(name, db->current) == 0)
		return;
	if (detached_branch(name) || done_writer_branch(name, head))
		db->names = lappend(db->names, pstrdup(name));
}

/*
 * The hidden branches no session is on any more (current is the session's
 * branch), deleted, each in a subtransaction: one a session is on, or
 * that the session may not delete, is left; any other error is the
 * checkout's.  Only in a transaction that can
 * write, or that is read-only only for the branch it started on (one
 * leaving a commit).
 */
static void
sweep_detached(char *current)
{
	DetachedBranches db = {NIL, current};
	MemoryContext cxt = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	ListCell   *lc;
	bool		was_read_only = XactReadOnly;

	if ((XactReadOnly && !XactReadOnlyFromBranch) || RecoveryInProgress())
		return;
	PROLLY_CALL(refs(), branches, add_detached, &db);
	XactReadOnly = false;
	foreach(lc, db.names)
	{
		BeginInternalSubTransaction(NULL);
		MemoryContextSwitchTo(cxt);
		PG_TRY();
		{
			BRANCH_WRITES_HUSHED(PROLLY_CALL(refs(), branch_delete,
											 (char *) lfirst(lc), true));
			if (writer_branch_name((char *) lfirst(lc)))
				PROLLY_CALL(writers(), set_writer_branch, (char *) lfirst(lc),
							NULL, NULL);
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
			/* (a cancel, or anything else, isn't the branch kept) */
			if (ed->sqlerrcode != ERRCODE_OBJECT_IN_USE &&
				ed->sqlerrcode != ERRCODE_INSUFFICIENT_PRIVILEGE)
			{
				XactReadOnly = was_read_only;
				ReThrowError(ed);
			}
			FreeErrorData(ed);
		}
		PG_END_TRY();
	}
	XactReadOnly = was_read_only;
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
 * The tables named (".": every one) put back as ref's commit has them,
 * the rest of the working set as it is: their uncommitted changes
 * discarded, as Dolt's dolt_checkout -- tables.
 */
static void
checkout_tables(const char *ref, char **names, int nnames)
{
	ProllyHash	commit;
	ProllyNodeRef from;
	ProllyNodeRef working;
	ProllyNodeRef target;
	List	   *from_tables = NIL;
	List	   *working_tables = NIL;
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
	PROLLY_CALL(canonicalizer(), tables, &from, collect_table_name, &from_tables);
	PROLLY_CALL(canonicalizer(), tables, &working, collect_table_name, &working_tables);
	for (int i = 0; i < nnames; i++)
	{
		char	   *q;

		if (strcmp(names[i], ".") == 0)
		{
			tables = list_concat_unique(tables, from_tables);
			tables = list_concat_unique(tables, working_tables);
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
	BRANCH_WRITES_HUSHED(PROLLY_CALL(diff_merge(), move_to, &target, &working));
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
 * branch, or -b branch [start point] to create it first.  Tables, as
 * Dolt's: -- tables (or "."), or a name that is no branch but a table
 * (with more after it), put back as the head has them; ref tables (ref
 * no table), as ref has them.  After --, one name that is a branch and no
 * table is the branch.
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
	/* tables: after "--", or names that aren't one branch */
	if (!create && nnames > 0)
	{
		bool		is_branch = PROLLY_CALL(refs(), branch_lookup, names[0],
											&(ProllyBranchId) {0});
		bool		is_table = strcmp(names[0], ".") == 0 ||
			OidIsValid(RangeVarGetRelid(makeRangeVarFromNameList(stringToQualifiedNameList(names[0], NULL)),
										NoLock, true));

		if (dashes == 0 && !(nnames == 1 && is_branch && !is_table))
		{
			checkout_tables("HEAD", names, nnames);
			result[0] = "0";
			result[1] = "";
			PG_RETURN_DATUM(text_array(2, result));
		}
		if (dashes != 0 && nnames > 1 && !is_table)
		{
			checkout_tables(names[0], names + 1, nnames - 1);
			result[0] = "0";
			result[1] = "";
			PG_RETURN_DATUM(text_array(2, result));
		}
		if (dashes != 0 && !is_branch && !names_commit(names[0]) &&
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
		sweep_detached(branch);
		result[0] = "0";
		result[1] = psprintf("Already on branch '%s'", branch);
		PG_RETURN_DATUM(text_array(2, result));
	}

	/*
	 * A commit's hash that names no branch: a detached session on it,
	 * read-only, on a hidden branch at the commit that every session
	 * detached there shares.  Changing anything takes a branch made from
	 * it.  (Its transactions are read-only from the next one; this one from
	 * now.)
	 */
	if (!create && names_commit(branch) &&
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
		if (!PROLLY_CALL(refs(), branch_lookup, hidden, &(ProllyBranchId) {0}))
			create_branch(hidden, hash_hex(&id));
		else if (!PROLLY_CALL(refs(), resolve, hidden, &head) || !same_hash(&head, &id))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("the hidden branch of commit %s has moved off it", hash_hex(&id))));
		checkout_branch(hidden);
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
	sweep_detached(branch);
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
	branch_note_write(MyBranch);
	BRANCH_WRITES_HUSHED(PROLLY_CALL(refs(), reset, "HEAD"));
	settled();
}

/*
 * A merge's, pick's or revert's conflicts (or violations), found outside a
 * transaction block: an error unless they may be kept.  between: what the
 * rows were changed on ("branches", "sides").
 */
static void
stopped_outside_block(const char *what, const char *What, const char *between,
					  int conflicts, int nviolations)
{
	if (IsTransactionBlock() || prolly_allow_commit_conflicts())
		return;
	if (conflicts > 0)
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("%s conflict detected, transaction rolled back", what),
				 errdetail_plural("%d row was changed differently on both %s.",
								  "%d rows were changed differently on both %s.",
								  conflicts, conflicts, between),
				 errhint("%s in a transaction block to resolve the conflicts with dolt_conflicts_resolve() before committing, or set dolt.allow_commit_conflicts to keep them.", What)));
	ereport(ERROR,
			(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
			 errmsg("%s constraint violations detected, transaction rolled back", what),
			 errdetail_plural("%d row violates a constraint.",
							  "%d rows violate constraints.",
							  nviolations, nviolations),
			 errhint("%s in a transaction block to see them in dolt_constraint_violations() and fix them before committing, or set dolt.allow_commit_conflicts to keep them.", What)));
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
	/* (the branch held first: its head is read as the merge finds it) */
	note_integration();
	had_head = PROLLY_CALL(refs(), resolve, "HEAD", &before);
	(void) PROLLY_CALL(refs(), resolve, branch, &theirs);
	known = list_length(PROLLY_CALL(refs(), violations));
	date_given = dated;
	PG_TRY();
	{
		BRANCH_WRITES_HUSHED(conflicts = PROLLY_CALL(diff_merge(), merge, branch,
													 message, flags, &id));
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
		int			nviolations = list_length(PROLLY_CALL(refs(), violations)) - known;

		conflicts -= nviolations;
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
	ereport(NOTICE,
			(errmsg("writing on branch \"%s\"", name),
			 errdetail("Branch \"%s\" is protected.", base),
			 errhint("Integrate the writes with dolt_integrate('-m', ...).")));
	return true;
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
 * A top-level statement that writes moves as it is analyzed, before it is
 * planned: its plan is then the writer branch's.
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
	/* (failing, the session stays on its branch, as everything else does) */
	returning = branch_return_on_abort();

	/* the branch's changes, committed */
	note_integration();
	BRANCH_WRITES_HUSHED(committed = PROLLY_CALL(refs(), commit, message,
												 false, &ignored));
	if (committed)
		settled();
	(void) PROLLY_CALL(refs(), resolve, "HEAD", &mine_head);

	/* merged into target, as it commits */
	checkout_branch(target);
	note_integration();
	had_head = PROLLY_CALL(refs(), resolve, "HEAD", &before);
	BRANCH_WRITES_HUSHED(conflicts = PROLLY_CALL(diff_merge(), merge, mine,
												 message, 0, &id));
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
	BRANCH_WRITES_HUSHED((void) PROLLY_CALL(diff_merge(), merge, target, NULL,
											0, &ignored));
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
	 * What the target needs that the head doesn't, checked (a reset to the
	 * head moves nothing new in, as an abort's doesn't; a soft one, none).
	 */
	if (ref && hard)
	{
		ProllyHash	head;
		ProllyHash	target;
		ProllyCommitInfo info;
		ProllyCommitInfo before;
		bool		has_head = PROLLY_CALL(refs(), resolve, "HEAD", &head);

		if (PROLLY_CALL(refs(), resolve, ref, &target) &&
			(!has_head || !same_hash(&head, &target)) &&
			PROLLY_CALL(refs(), commit_get, &target, &info))
			prolly_check_commit_dependencies(&info,
											 has_head &&
											 PROLLY_CALL(refs(), commit_get, &head, &before) ?
											 &before : NULL, "reset");
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
	BRANCH_WRITES_HUSHED(PROLLY_CALL(refs(), reset, ref ? ref : "HEAD"));
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
		BRANCH_WRITES_HUSHED(PROLLY_CALL(refs(), reset, "HEAD"));
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
		refuse_violations("stash");

		/*
		 * (a write, as any: it makes no commit, so a protected branch
		 * refuses it, and the branch is left with its changes)
		 */
		branch_note_write(MyBranch);
		BRANCH_WRITES_HUSHED(PROLLY_CALL(diff_merge(), apply, hash_hex(&stash),
										 "dolt_stash"));
		if (strcmp(cmd, "pop") == 0)
			(void) PROLLY_CALL(refs(), stash_drop, name, n);
	}
	else if (strcmp(cmd, "drop") == 0)
	{
		if (!PROLLY_CALL(refs(), stash_drop, name, stash_index(which)))
			no_stashes();
	}
	else if (strcmp(cmd, "clear") == 0)
	{
		if (which)
			unexpected("dolt_stash", which);
		PROLLY_CALL(refs(), stash_clear, name);
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

/*
 * The tables with conflicts and the tables with constraint violations a
 * pick or revert in progress left, as Dolt counts them (tables, not rows):
 * the branch had no violations before it (refuse_violations()).
 */
static void
tables_left(int *conflicted, int *violated)
{
	ProllyMergeState ms;
	List	   *relids = NIL;
	ListCell   *lc;

	*conflicted = PROLLY_CALL(refs(), merge_state, &ms) ? ms.ntables : 0;
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
	refuse_violations("cherry-pick");
	note_integration();
	BRANCH_WRITES_HUSHED(conflicts = PROLLY_CALL(diff_merge(), cherry_pick,
												 &ref, 1, allow_empty, &id));
	must_commit(conflicts, "a cherry-pick");
	if (conflicts > 0)
	{
		int			nviolations = list_length(PROLLY_CALL(refs(), violations));
		int			ctables;
		int			vtables;

		stopped_outside_block("cherry-pick", "Cherry-pick", "sides",
							  conflicts - nviolations, nviolations);
		tables_left(&ctables, &vtables);
		result[0] = "";
		result[1] = psprintf("%d", ctables);
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
	BRANCH_WRITES_HUSHED(stopped = PROLLY_CALL(diff_merge(), revert, ids, n,
											   series_head, &id));
	must_commit(stopped, "a revert");
	if (stopped > 0)
	{
		int			nviolations = list_length(PROLLY_CALL(refs(), violations));
		int			ctables;
		int			vtables;

		stopped_outside_block("revert", "Revert", "sides",
							  stopped - nviolations, nviolations);
		tables_left(&ctables, &vtables);
		result[0] = "";
		result[1] = psprintf("%d", ctables);
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
			note_integration();
			if (abort_revert)
			{
				BRANCH_WRITES_HUSHED(PROLLY_CALL(refs(), reset,
												 hash_hex(&ms.series_head)));
				settled();
			}
			else if (ms.ntables > 0 || PROLLY_CALL(refs(), violations) != NIL)
			{
				/* (Dolt reports what is still to resolve, without error) */
				tables_left(&ms.ntables, &nrefs);
				result[1] = psprintf("%d", ms.ntables);
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
} LogEntry;

static LogEntry *
log_find(HTAB *entries, const ProllyHash *id)
{
	ProllyHash	k = prolly_hash_key(id);

	return hash_search(entries, &k, HASH_FIND, NULL);
}

/*
 * Highest order first, then newest, as Dolt's log goes by height and then
 * date: a commit is always above its parents.
 */
static int
log_cmp(const ListCell *a, const ListCell *b)
{
	LogEntry   *x = lfirst(a);
	LogEntry   *y = lfirst(b);

	if (x->order != y->order)
		return x->order > y->order ? -1 : 1;
	if (x->info.committed_at != y->info.committed_at)
		return x->info.committed_at > y->info.committed_at ? -1 : 1;
	return memcmp(x->info.id.bytes, y->info.id.bytes, x->info.id.len);
}

/*
 * A commit of the log, for prolly_history_height(): read already, so none
 * is read twice.
 */
static bool
log_lookup(const ProllyHash *id, ProllyCommitInfo *info, void *arg)
{
	LogEntry   *e = log_find(arg, id);

	if (e == NULL)
		return false;
	*info = e->info;
	return true;
}

/*
 * Every commit the starts (ProllyHash *) reach (LogEntry *), each once,
 * highest commit order (height) first, then newest.
 */
static List *
log_walk(List *starts)
{
	HTAB	   *seen;
	HASHCTL		ctl = {0};
	HTAB	   *heights;
	List	   *entries = NIL;
	ListCell   *lc;
	ProllyHash	key;
	bool		found;

	ctl.keysize = sizeof(ProllyHash);
	ctl.entrysize = sizeof(LogEntry);
	ctl.hcxt = CurrentMemoryContext;
	seen = hash_create("dolt_log commits", 256, &ctl,
					   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/*
	 * Every commit from the starts, each entered once, as it is found:
	 * entries is the queue too, walked by an index as it grows.
	 */
	foreach(lc, starts)
	{
		LogEntry   *e;

		key = prolly_hash_key(lfirst(lc));
		e = hash_search(seen, &key, HASH_ENTER, &found);
		if (!found)
			entries = lappend(entries, e);
	}
	for (int n = 0; n < list_length(entries); n++)
	{
		LogEntry   *e = list_nth(entries, n);
		ProllyHash	id = e->key;

		CHECK_FOR_INTERRUPTS();
		memset(&e->info, 0, sizeof(e->info));
		e->order = 0;
		if (!PROLLY_CALL(refs(), commit_get, &id, &e->info))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("prolly commit %s is missing", hash_hex(&id))));
		for (int i = 0; i < e->info.nparents; i++)
		{
			LogEntry   *p;

			key = prolly_hash_key(&e->info.parents[i]);
			p = hash_search(seen, &key, HASH_ENTER, &found);
			if (!found)
				entries = lappend(entries, p);
		}
	}
	heights = prolly_history_heights();
	foreach(lc, entries)
	{
		LogEntry   *e = lfirst(lc);

		e->order = prolly_history_height(&e->info.id, heights, log_lookup, seen);
	}
	hash_destroy(heights);
	list_sort(entries, log_cmp);
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

/*
 * dolt_log(ref text, OUT commit_hash text, OUT committer text,
 *			OUT email text, OUT date timestamptz, OUT message text,
 *			OUT commit_order int8) returns setof record
 *
 * Every commit ref reaches, highest commit order (height) first, then
 * newest.
 */
Datum
dolt_log(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	char	   *ref = text_to_cstring(PG_GETARG_TEXT_PP(0));
	ListCell   *lc;

	InitMaterializedSRF(fcinfo, 0);
	foreach(lc, log_entries(ref))
	{
		LogEntry   *e = lfirst(lc);
		Datum		values[6];
		bool		nulls[6] = {0};
		char	   *name;
		char	   *email;

		split_author(e->info.author, &name, &email);
		values[0] = CStringGetTextDatum(hash_hex(&e->info.id));
		values[1] = CStringGetTextDatum(name);
		values[2] = CStringGetTextDatum(email);
		values[3] = TimestampTzGetDatum(e->info.committed_at);
		values[4] = CStringGetTextDatum(e->info.message);
		values[5] = Int64GetDatum(e->order);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
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
	Datum		values[12];
	bool		nulls[12] = {0};
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
 * garbage collection forgets the moves before it.  An argument names one
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
				reflog_root, &rr);
	first = rr.n > REFLOG_ROOTS ? rr.n - REFLOG_ROOTS : 0;
	for (uint64 i = first; i < rr.n; i++)
	{
		ReflogRoot *r = &rr.roots[i % REFLOG_ROOTS];

		CHECK_FOR_INTERRUPTS();
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
with_status(List *names, Oid relid, const char *status)
{
	char	   *name = qualified_name(relid);
	size_t		len;
	ListCell   *lc;

	if (name == NULL)
		return names;
	len = strlen(name);

	foreach(lc, names)
	{
		char	   *n = lfirst(lc);

		/* (the status is after the last tab: a name may hold one) */
		if (strrchr(n, '\t') == n + len && strncmp(n, name, len) == 0)
			names = foreach_delete_current(names, lc);
	}
	return lappend(names, psprintf("%s\t%s", name, status));
}

/*
 * dolt_status(OUT table_name text, OUT staged bool, OUT status text)
 *	  returns setof record
 *
 * The tables changed since the session branch's head commit (before its
 * first, since the empty root: everything is new): rows changed
 * ("modified"), made ("new table"), dropped ("deleted") or, with a merge
 * in progress, with conflicts ("conflict", not staged), or with constraint
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
static Datum
rows_datum(Rows *r, const ProllyValue *value, uint64 *count)
{
	ProllyValue row;

	rows_parse(r, value, &row, count);
	ExecClearTuple(r->slot);
	PROLLY_CALL(r->c.keying, decode_row, r->keying, &row, r->slot);
	return ExecFetchSlotHeapTupleDatum(r->slot);
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
	MemoryContext old = rows_next(&s->rows);

	if (before)
		from = rows_datum(&s->rows, before, &nbefore);
	if (after)
		to = rows_datum(&s->rows, after, &nafter);

	/* A keyless row's copies: each one added or removed is a row. */
	if (s->rows.keyless)
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
	rows_begin(&s.rows, rel);
	PROLLY_CALL(diff_merge(), diff, from_ref, to_ref, relid, &s.sink);
	rows_end(&s.rows);
	table_close(rel, AccessShareLock);
	return (Datum) 0;
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
	ProllyHash	stop;
	bool		stopping = false;
	ProllyHash	at;
	bool		more;
	Relation	rel;
	RowDiffSink s;

	InitMaterializedSRF(fcinfo, 0);
	rel = open_row_type_table(relid, "dolt_diff_table", ACL_SELECT);
	if (from_ref)
	{
		if (!PROLLY_CALL(refs(), resolve, from_ref, &stop))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("prolly ref \"%s\" does not exist", from_ref)));
		stopping = true;
	}
	memset(&s, 0, sizeof(s));
	s.sink.change = row_change;
	s.rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	rows_begin(&s.rows, rel);

	/* the working set's changes first, against the head (or nothing) */
	more = PROLLY_CALL(refs(), resolve, "HEAD", &at);
	if (to_ref == NULL || pg_strcasecmp(to_ref, "WORKING") == 0)
	{
		ProllyCommitInfo head;

		s.to_commit = "WORKING";
		s.from_commit = more ? hash_hex(&at) : NULL;
		s.to_dated = false;
		s.from_dated = more && PROLLY_CALL(refs(), commit_get, &at, &head);
		if (s.from_dated)
			s.from_date = head.committed_at;
		PROLLY_CALL(diff_merge(), diff, s.from_commit, "WORKING", relid, &s.sink);
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
		s.to_commit = to;
		s.to_date = info.committed_at;
		s.to_dated = true;
		s.from_commit = info.nparents > 0 ? hash_hex(&info.parents[0]) : NULL;
		s.from_dated = info.nparents > 0 &&
			PROLLY_CALL(refs(), commit_get, &info.parents[0], &parent);
		if (s.from_dated)
			s.from_date = parent.committed_at;
		PROLLY_CALL(diff_merge(), diff, s.from_commit, to, relid, &s.sink);
		more = info.nparents > 0;
		if (more)
			at = info.parents[0];
	}
	rows_end(&s.rows);
	table_close(rel, AccessShareLock);
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
	row = rows_datum(&s->rows, after, &copies);
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
	rows_begin(&s.rows, rel);
	PROLLY_CALL(diff_merge(), diff, NULL, ref, relid, &s.sink);
	rows_end(&s.rows);
	table_close(rel, AccessShareLock);
	return (Datum) 0;
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

			if (IsA(ref, Const) && !((Const *) ref)->constisnull &&
				OidIsValid(relid))
			{
				req->root->glob->dependsOnRole = true;
				if (history_access(relid, ACL_SELECT, true) == HISTORY_READABLE &&
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
 * dolt_history_table(tbl anyelement, OUT row anyelement, OUT commit_hash
 *					  text, OUT committer text, OUT commit_date timestamptz)
 *	  returns setof record
 *
 * tbl's table's rows at each commit the head reaches (that has the table),
 * with the commit, as Dolt's dolt_history_<table>: each read from the
 * commit's tree, as dolt_at() reads it.
 */
Datum
dolt_history_table(PG_FUNCTION_ARGS)
{
	Oid			type = get_fn_expr_argtype(fcinfo->flinfo, 0);
	Oid			relid = OidIsValid(type) ? get_typ_typrelid(type) : InvalidOid;
	Relation	rel;
	AtSink		s;
	ListCell   *lc;

	rel = open_row_type_table(relid, "dolt_history_table", ACL_SELECT);
	InitMaterializedSRF(fcinfo, 0);
	memset(&s, 0, sizeof(s));
	s.sink.change = at_change;
	s.rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	s.history = true;
	rows_begin(&s.rows, rel);
	foreach(lc, log_entries("HEAD"))
	{
		LogEntry   *e = lfirst(lc);
		char	   *email;

		CHECK_FOR_INTERRUPTS();
		s.commit = &e->info;
		s.hash = hash_hex(&e->info.id);
		split_author(e->info.author, &s.committer, &email);
		PROLLY_CALL(diff_merge(), diff, NULL, s.hash, relid, &s.sink);
	}
	rows_end(&s.rows);
	table_close(rel, AccessShareLock);
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
static void
stat_tree_change(ProllyDiffKind kind, const ProllyKey *key,
				 const ProllyValue *before, const ProllyValue *after, void *arg)
{
	stat_change(arg, InvalidOid, kind, key, before, after);
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
 * dropped, what to do with them is the user's.  Only the rows changed
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
				for (int j = 0; j < ms.ntables; j++)
					tables = list_append_unique_oid(tables, ms.tables[j]);
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
	branch_note_write(MyBranch);
	BRANCH_WRITES_HUSHED(
		foreach(lc, tables)
			PROLLY_CALL(diff_merge(), resolve, lfirst_oid(lc), side == 2);
	);
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

/*
 * Fetch remote name's branches (NIL: all of them) into its remote-tracking
 * branches, the remote opened and its heads read once; with need, refuse
 * before receiving anything if the remote hasn't that branch.  *got (if
 * not NULL) is what was fetched: the branches and their commits.  A
 * clone's (into a database with nothing to walk against) copies the
 * remote's files whole, where the transfer can.
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
	ListCell   *b;
	ListCell   *c;

	prolly_database_components(&dc);
	r = PROLLY_CALL(rr, open, url, false);
	PG_TRY();
	{
		ProllyHash *commits;
		int			n = 0;

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
		commits = palloc(sizeof(ProllyHash) * Max(list_length(want.commits), 1));
		foreach(c, want.commits)
			commits[n++] = *(ProllyHash *) lfirst(c);
		{
			uint64		count;

			if (!clone || !PROLLY_HAS(dc.transfer, clone) ||
				!PROLLY_CALL(dc.transfer, clone, rr, r, commits, n, &count))
				(void) PROLLY_CALL(dc.transfer, receive, rr, r, commits, n);
		}
	}
	PG_FINALLY();
	{
		PROLLY_CALL(rr, close, r);
	}
	PG_END_TRY();

	forboth(b, want.branches, c, want.commits)
		PROLLY_CALL(dc.refs, tracking_set, name, lfirst(b), lfirst(c));
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
	PROLLY_CALL(dc.reclamation, collect, full, gc);
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
 * prolly_page_sweep(OUT freed int8, OUT recounted int8, OUT truncated int8)
 *
 * The current database's page stores swept (bmap_sweep()), in every
 * tablespace that has one: blocks nothing holds freed, stuck reference
 * counts rewritten exactly, the stores shortened.  Its owner, or a member
 * of pg_maintain, as dolt_gc().
 */
Datum
prolly_page_sweep(PG_FUNCTION_ARGS)
{
	TupleDesc	desc;
	Datum		values[3];
	bool		nulls[3] = {0};
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

/*
 * dolt_push(VARIADIC args text[]) returns text[]: {status, message}
 *
 * [remote [branch]], -f/--force, -u/--set-upstream: branch (the session's
 * if not named) pushed to the remote's branch of the same name; with no
 * remote, the session branch to its upstream (which names the remote and
 * the remote's branch).  The chunks the branch's head reaches that the
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

		foreach(hc, h.commits)
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
			BRANCH_WRITES_HUSHED(PROLLY_CALL(remote_refs(), reset, tracking));
		}
		else
			create_branch(b, tracking);
		PROLLY_CALL(remote_refs(), upstream_set, b, remote, b);
	}
	if (strcmp(branch, current) != 0)
		checkout_branch(branch);
	PG_RETURN_DATUM(text_array(1, result));
}
