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
 * means the session branch's working set as last committed by any
 * transaction, so dolt_status and dolt_diff don't see the current
 * transaction's own uncommitted writes.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_dolt.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/prolly_config.h"
#include "access/prolly_xact.h"
#include "access/table.h"
#include "access/xact.h"
#include "access/tableam.h"
#include "catalog/branch.h"
#include "catalog/branch_ops.h"
#include "catalog/namespace.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "executor/tuptable.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/regproc.h"
#include "utils/resowner.h"
#include "nodes/makefuncs.h"
#include "utils/datum.h"
#include "utils/fmgrprotos.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

/* dolt.author's value; read through GetConfigOption so a placeholder works. */
static char *dolt_author = NULL;
static bool dolt_allow_commit_conflicts = false;

/* dolt_commit's --author, while its commit runs. */
static const char *author_override = NULL;

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
	/*
	 * A versioned database's tables are prolly tables by default: below any
	 * setting made for the server, database, role or session.
	 */
	if (MyDatabaseVersioned)
		SetConfigOption("default_table_access_method", "prolly_d1",
						PGC_USERSET, PGC_S_DYNAMIC_DEFAULT);
	if (branch)
	{
		ProllyDatabaseComponents dc;

		prolly_database_components(&dc);
		PROLLY_CALL(dc.refs, checkout, branch);
	}
}

bool
prolly_allow_commit_conflicts(void)
{
	const char *v = GetConfigOption("dolt.allow_commit_conflicts", true, false);
	bool		result = false;

	return v != NULL && parse_bool(v, &result) && result;
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

/* ------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------
 */

static char *
hash_hex(const ProllyHash *hash)
{
	char	   *hex = palloc(hash->len * 2 + 1);

	hex_encode((const char *) hash->bytes, hash->len, hex);
	hex[hash->len * 2] = '\0';
	return hex;
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

/* The argument after option args[*i]. */
static char *
option_value(char **args, int nargs, int *i, const char *fn)
{
	if (*i + 1 >= nargs)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s: option \"%s\" needs a value", fn, args[*i])));
	return args[++(*i)];
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

static char *
qualified_name(Oid relid)
{
	return quote_qualified_identifier(get_namespace_name(get_rel_namespace(relid)),
									  get_rel_name(relid));
}

/* The session branch's name. */
typedef struct FindBranch
{
	ProllyBranchId id;
	char	   *name;
} FindBranch;

static void
find_branch(const char *name, const ProllyHash *head, void *arg)
{
	FindBranch *f = arg;
	ProllyBranchId id;

	if (f->name == NULL && PROLLY_CALL(refs(), branch_lookup, name, &id) &&
		id == f->id)
		f->name = pstrdup(name);
}

static char *
session_branch_name(void)
{
	FindBranch	f = {prolly_session_branch(), NULL};

	PROLLY_CALL(refs(), branches, find_branch, &f);
	if (f.name == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("the session's prolly branch no longer exists")));
	return f.name;
}

/* ------------------------------------------------------------------------
 * Procedures
 * ------------------------------------------------------------------------
 */

/*
 * dolt_commit(VARIADIC args text[]) returns text[]: {commit id}
 *
 * -m/--message message (required), --author "Name <email>", -a/-A/--all
 * (every change is always committed), --allow-empty (else a commit whose
 * root equals its parent's is refused).
 */
Datum
dolt_commit(PG_FUNCTION_ARGS)
{
	char	  **args;
	int			nargs = text_args(fcinfo, &args);
	char	   *message = NULL;
	char	   *author = NULL;
	bool		allow_empty = false;
	ProllyHash	id = {0};
	ProllyCommitInfo info;
	const char *result[1];

	for (int i = 0; i < nargs; i++)
	{
		char	   *a = args[i];

		if (strcmp(a, "--message") == 0)
			message = option_value(args, nargs, &i, "dolt_commit");
		else if (strcmp(a, "--author") == 0)
			author = option_value(args, nargs, &i, "dolt_commit");
		else if (strcmp(a, "--all") == 0 || strcmp(a, "--ALL") == 0)
			continue;
		else if (strcmp(a, "--allow-empty") == 0)
			allow_empty = true;
		else if (a[0] == '-' && a[1] != '-' && a[1] != '\0')
		{
			/* Short options together, as -Am message. */
			for (const char *c = a + 1; *c; c++)
			{
				if (*c == 'a' || *c == 'A')
					continue;
				if (*c == 'm' && c[1] == '\0')
					message = option_value(args, nargs, &i, "dolt_commit");
				else
					unsupported("dolt_commit", a);
			}
		}
		else if (a[0] == '-')
			unsupported("dolt_commit", a);
		else
			unexpected("dolt_commit", a);
	}
	if (message == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dolt_commit needs a message"),
				 errhint("Pass -m and a message.")));

	author_override = author;
	PG_TRY();
	{
		PROLLY_CALL(refs(), commit, message, &id);
	}
	PG_FINALLY();
	{
		author_override = NULL;
	}
	PG_END_TRY();

	/* Nothing changed: the root is its parent's. */
	if (!allow_empty && PROLLY_CALL(refs(), commit_get, &id, &info) &&
		info.nparents == 1)
	{
		ProllyCommitInfo parent;

		if (PROLLY_CALL(refs(), commit_get, &info.parents[0], &parent) &&
			same_hash(&parent.root.hash, &info.root.hash))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("nothing to commit"),
					 errhint("Pass --allow-empty to commit anyway.")));
	}
	result[0] = hash_hex(&id);
	PG_RETURN_DATUM(text_array(1, result));
}

static bool
detached_branch(const char *name)
{
	return strncmp(name, PROLLY_DETACHED_PREFIX, strlen(PROLLY_DETACHED_PREFIX)) == 0;
}

static void
reserved_branch_name(const char *name)
{
	if (detached_branch(name))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("branch names beginning \"%s\" are reserved", PROLLY_DETACHED_PREFIX)));
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
		PROLLY_CALL(refs(), branch_create, name, from);
	}
	PG_FINALLY();
	{
		XactReadOnly = was_read_only;
	}
	PG_END_TRY();
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
		PROLLY_CALL(refs(), checkout, name);
	}
	PG_FINALLY();
	{
		XactReadOnly = was_read_only;
	}
	PG_END_TRY();
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

	if (detached_branch(name) && strcmp(name, db->current) != 0)
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
			PROLLY_CALL(refs(), branch_delete, (char *) lfirst(lc), true);
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


/*
 * dolt_checkout(VARIADIC args text[]) returns text[]: {status, message}
 *
 * branch, or -b branch [start point] to create it first.
 */
Datum
dolt_checkout(PG_FUNCTION_ARGS)
{
	char	  **args;
	int			nargs = text_args(fcinfo, &args);
	char	   *branch = NULL;
	char	   *start = NULL;
	bool		create = false;
	const char *result[2];

	for (int i = 0; i < nargs; i++)
	{
		char	   *a = args[i];

		if (strcmp(a, "-b") == 0)
		{
			create = true;
			branch = option_value(args, nargs, &i, "dolt_checkout");
		}
		else if (a[0] == '-')
			unsupported("dolt_checkout", a);
		else if (branch == NULL)
			branch = a;
		else if (create && start == NULL)
			start = a;
		else
			unexpected("dolt_checkout", a);
	}
	if (branch == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dolt_checkout needs a branch")));
	/* (the hidden branches are checked out by their commits) */
	reserved_branch_name(branch);

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
		/* (a branch name has room for 40 of the hash's 64 digits) */
		hidden = psprintf("%s%.40s", PROLLY_DETACHED_PREFIX, hash_hex(&id));
		if (!PROLLY_CALL(refs(), branch_lookup, hidden, &(ProllyBranchId) {0}))
			create_branch(hidden, hash_hex(&id));
		else if (!PROLLY_CALL(refs(), resolve, hidden, &head) || !same_hash(&head, &id))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("the hidden branch of commit %s has moved off it", hash_hex(&id))));
		checkout_branch(hidden);
		MyBranchReadOnly = true;
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
 * -D (or -d with -f/--force) branch....
 */
Datum
dolt_branch(PG_FUNCTION_ARGS)
{
	char	  **args;
	int			nargs = text_args(fcinfo, &args);
	bool		delete = false;
	bool		copy = false;
	bool		force = false;
	List	   *names = NIL;
	const char *result[1] = {"0"};

	for (int i = 0; i < nargs; i++)
	{
		char	   *a = args[i];

		if (strcmp(a, "-d") == 0 || strcmp(a, "--delete") == 0)
			delete = true;
		else if (strcmp(a, "-D") == 0)
			delete = force = true;
		else if (strcmp(a, "-f") == 0 || strcmp(a, "--force") == 0)
			force = true;
		else if (strcmp(a, "-c") == 0 || strcmp(a, "--copy") == 0)
			copy = true;
		else if (a[0] == '-')
			unsupported("dolt_branch", a);
		else
			names = lappend(names, a);
	}
	if (names == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dolt_branch needs a branch")));
	if (delete)
	{
		ListCell   *lc;

		foreach(lc, names)
			PROLLY_CALL(refs(), branch_delete, (char *) lfirst(lc), force);
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
		create_branch(name, from);
	}
	PG_RETURN_DATUM(text_array(1, result));
}

/*
 * End the merge (what: "merge", or "cherry-pick") in progress on the
 * session's branch: the branch as its head has it.  (Branch copies reset
 * their working set's rows, the transaction's writes as any others; the
 * rows engine can't undo its own.)  A merge isn't begun over uncommitted
 * changes, so none are lost.
 */
static void
abort_in_progress(const char *what)
{
	if (!prolly_database_uses_copies() && prolly_xact_has_writes())
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("cannot abort a %s in a transaction that has written to prolly tables",
						what),
				 errhint("Roll the transaction back instead.")));
	PROLLY_CALL(refs(), reset, "HEAD");
}

/* Is the session branch's working set its head's? */
static bool
working_is_head(void)
{
	ProllyHash	head;
	ProllyNodeRef working;
	ProllyNodeRef committed;

	return PROLLY_CALL(refs(), resolve, "HEAD", &head) &&
		PROLLY_CALL(refs(), working_set, NULL, &working) &&
		PROLLY_CALL(refs(), commit_working_set, &head, &committed) &&
		same_hash(&working.hash, &committed.hash);
}

/*
 * dolt_merge(VARIADIC args text[]) returns text[]:
 *	  {commit id, fast_forward, conflicts, message}
 *
 * branch; --no-ff; -m/--message message; --abort.  Rows changed
 * differently on both sides leave the merge in progress, with conflicts
 * (dolt_conflicts) to resolve before dolt_commit makes the merge commit.
 * As in Dolt, a transaction can't commit with conflicts unless
 * dolt.allow_commit_conflicts is on: outside a transaction block, the
 * merge fails and changes nothing.
 */
Datum
dolt_merge(PG_FUNCTION_ARGS)
{
	char	  **args;
	int			nargs = text_args(fcinfo, &args);
	char	   *branch = NULL;
	char	   *message = NULL;
	int			flags = 0;
	ProllyHash	before = {0};
	ProllyHash	theirs = {0};
	ProllyHash	id = {0};
	bool		had_head;
	bool		abort_merge = false;
	int			conflicts;
	int			known;
	const char *result[4];

	for (int i = 0; i < nargs; i++)
	{
		char	   *a = args[i];

		if (strcmp(a, "--no-ff") == 0)
			flags |= PROLLY_MERGE_NO_FF;
		else if (strcmp(a, "--abort") == 0)
			abort_merge = true;
		else if (strcmp(a, "-m") == 0 || strcmp(a, "--message") == 0)
			message = option_value(args, nargs, &i, "dolt_merge");
		else if (a[0] == '-')
			unsupported("dolt_merge", a);
		else if (branch == NULL)
			branch = a;
		else
			unexpected("dolt_merge", a);
	}
	if (abort_merge)
	{
		ProllyMergeState ms;

		if (branch != NULL)
			unexpected("dolt_merge", branch);
		if (!PROLLY_CALL(refs(), merge_state, &ms))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("there is no merge to abort")));
		abort_in_progress("merge");
		result[0] = "";
		result[1] = "0";
		result[2] = "0";
		result[3] = "merge aborted";
		PG_RETURN_DATUM(text_array(4, result));
	}
	if (branch == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dolt_merge needs a branch")));
	had_head = PROLLY_CALL(refs(), resolve, "HEAD", &before);
	(void) PROLLY_CALL(refs(), resolve, branch, &theirs);
	known = list_length(PROLLY_CALL(refs(), violations));
	conflicts = PROLLY_CALL(diff_merge(), merge, branch, message, flags, &id);
	if (conflicts > 0)
	{
		/* (the violations it found, besides those the branch had) */
		int			nviolations = list_length(PROLLY_CALL(refs(), violations)) - known;

		conflicts -= nviolations;
		if (!IsTransactionBlock() && !prolly_allow_commit_conflicts())
		{
			if (conflicts > 0)
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("merge conflict detected, transaction rolled back"),
						 errdetail_plural("%d row was changed differently on both branches.",
										  "%d rows were changed differently on both branches.",
										  conflicts, conflicts),
						 errhint("Merge in a transaction block to resolve the conflicts with dolt_conflicts_resolve() before committing, or set dolt.allow_commit_conflicts to keep them.")));
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("merge constraint violations detected, transaction rolled back"),
					 errdetail_plural("%d row violates a constraint.",
									  "%d rows violate constraints.",
									  nviolations, nviolations),
					 errhint("Merge in a transaction block to see them in dolt_constraint_violations() and fix them before committing, or set dolt.allow_commit_conflicts to keep them.")));
		}
		result[0] = "";
		result[1] = "0";
		result[2] = "1";
		result[3] = conflicts > 0 ? "conflicts found" : "constraint violations found";
		PG_RETURN_DATUM(text_array(4, result));
	}
	result[0] = hash_hex(&id);
	result[1] = had_head && same_hash(&id, &theirs) &&
		!same_hash(&before, &theirs) ? "1" : "0";
	result[2] = "0";
	result[3] = had_head && same_hash(&id, &before) ?
		"Everything up-to-date" : "merge successful";
	PG_RETURN_DATUM(text_array(4, result));
}

/*
 * dolt_reset(VARIADIC args text[]) returns text[]: {status}
 *
 * --hard [ref]: the session branch's head commit and working set become
 * ref's (HEAD: drops the changes since the last commit).
 */
Datum
dolt_reset(PG_FUNCTION_ARGS)
{
	char	  **args;
	int			nargs = text_args(fcinfo, &args);
	bool		hard = false;
	char	   *ref = NULL;
	const char *result[1] = {"0"};

	for (int i = 0; i < nargs; i++)
	{
		char	   *a = args[i];

		if (strcmp(a, "--hard") == 0)
			hard = true;
		else if (a[0] == '-')
			unsupported("dolt_reset", a);
		else if (ref == NULL)
			ref = a;
		else
			unexpected("dolt_reset", a);
	}
	if (!hard)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("dolt_reset only does hard resets"),
				 errdetail("Prolly branches have no staging area to reset."),
				 errhint("Pass --hard.")));
	PROLLY_CALL(refs(), reset, ref ? ref : "HEAD");
	PG_RETURN_DATUM(text_array(1, result));
}

/*
 * dolt_cherry_pick(VARIADIC args text[]) returns text[]:
 *	  {commit id, data conflicts, schema conflicts, constraint violations}
 *
 * [--allow-empty] commit: its changes, committed again on the current
 * branch, with its author.  Rows changed differently on both sides leave
 * the pick in progress, as a merge's conflicts do: resolved, dolt_commit
 * (or [--allow-empty] --continue, with the picked commit's message) makes
 * its commit, with one parent; --abort ends it.
 */
Datum
dolt_cherry_pick(PG_FUNCTION_ARGS)
{
	char	  **args;
	int			nargs = text_args(fcinfo, &args);
	const char *ref = NULL;
	bool		allow_empty = false;
	ProllyHash	id;
	const char *result[4] = {NULL, "0", "0", "0"};
	bool		abort_pick = false;
	bool		continue_pick = false;
	int			conflicts;
	int			known;

	for (int i = 0; i < nargs; i++)
	{
		if (strcmp(args[i], "--allow-empty") == 0)
			allow_empty = true;
		else if (strcmp(args[i], "--abort") == 0)
			abort_pick = true;
		else if (strcmp(args[i], "--continue") == 0)
			continue_pick = true;
		else if (args[i][0] == '-')
			unsupported("dolt_cherry_pick", args[i]);
		else if (ref == NULL)
			ref = args[i];
		else
			unexpected("dolt_cherry_pick", args[i]);
	}
	if (abort_pick && continue_pick)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dolt_cherry_pick can't --abort and --continue at once")));
	if (abort_pick || continue_pick)
	{
		ProllyMergeState ms;

		if (ref != NULL)
			unexpected("dolt_cherry_pick", ref);
		if (!PROLLY_CALL(refs(), merge_state, &ms) || !ms.pick)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("there is no cherry-pick to %s",
							abort_pick ? "abort" : "continue")));
		if (abort_pick)
		{
			abort_in_progress("cherry-pick");
			result[0] = "";
			PG_RETURN_DATUM(text_array(4, result));
		}
		/* a pick resolved to nothing: as a pick of nothing is */
		if (!allow_empty && working_is_head())
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("dolt_cherry_pick changes nothing on the branch"),
					 errhint("Pass --allow-empty to commit it anyway.")));
		PROLLY_CALL(refs(), commit, ms.message, &id);
		result[0] = hash_hex(&id);
		PG_RETURN_DATUM(text_array(4, result));
	}
	if (ref == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dolt_cherry_pick needs a commit")));
	known = list_length(PROLLY_CALL(refs(), violations));
	conflicts = PROLLY_CALL(diff_merge(), cherry_pick, &ref, 1, false, allow_empty,
							&id);
	if (conflicts > 0)
	{
		int			nviolations = list_length(PROLLY_CALL(refs(), violations)) - known;

		conflicts -= nviolations;
		if (!IsTransactionBlock() && !prolly_allow_commit_conflicts())
		{
			if (conflicts > 0)
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("cherry-pick conflict detected, transaction rolled back"),
						 errdetail_plural("%d row was changed differently on both sides.",
										  "%d rows were changed differently on both sides.",
										  conflicts, conflicts),
						 errhint("Cherry-pick in a transaction block to resolve the conflicts with dolt_conflicts_resolve() before committing, or set dolt.allow_commit_conflicts to keep them.")));
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("cherry-pick constraint violations detected, transaction rolled back"),
					 errdetail_plural("%d row violates a constraint.",
									  "%d rows violate constraints.",
									  nviolations, nviolations),
					 errhint("Cherry-pick in a transaction block to see them in dolt_constraint_violations() and fix them before committing, or set dolt.allow_commit_conflicts to keep them.")));
		}
		result[0] = "";
		result[1] = psprintf("%d", conflicts);
		result[3] = psprintf("%d", nviolations);
		PG_RETURN_DATUM(text_array(4, result));
	}
	result[0] = hash_hex(&id);
	PG_RETURN_DATUM(text_array(4, result));
}

/*
 * dolt_revert(VARIADIC args text[]) returns text[]: {status}
 *
 * commit...: their changes undone, in one commit; each named as the branch
 * was before any is.  A row the undoing can't write (or leaves breaking a
 * foreign key) is a constraint violation, as for a cherry-pick: status 1,
 * the revert in progress until they're fixed and it's committed.
 */
Datum
dolt_revert(PG_FUNCTION_ARGS)
{
	char	  **args;
	int			nargs = text_args(fcinfo, &args);
	const char *result[1] = {"0"};
	ProllyHash	id;
	int			known;
	int			nviolations;

	for (int i = 0; i < nargs; i++)
		if (args[i][0] == '-')
			unsupported("dolt_revert", args[i]);
	if (nargs == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dolt_revert needs a commit")));
	known = list_length(PROLLY_CALL(refs(), violations));
	nviolations = PROLLY_CALL(diff_merge(), cherry_pick,
							  (const char *const *) args, nargs, true, false, &id);

	/*
	 * Rows it couldn't write, or left breaking a foreign key: the revert in
	 * progress, as a cherry-pick's, until they're fixed and committed (its
	 * conflicts are an error).
	 */
	if (nviolations > 0)
	{
		nviolations = list_length(PROLLY_CALL(refs(), violations)) - known;
		if (!IsTransactionBlock() && !prolly_allow_commit_conflicts())
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("revert constraint violations detected, transaction rolled back"),
					 errdetail_plural("%d row violates a constraint.",
									  "%d rows violate constraints.",
									  nviolations, nviolations),
					 errhint("Revert in a transaction block to see them in dolt_constraint_violations() and fix them before committing, or set dolt.allow_commit_conflicts to keep them.")));
		result[0] = "1";
	}
	PG_RETURN_DATUM(text_array(1, result));
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

	for (int i = 0; i < nargs; i++)
	{
		char	   *a = args[i];

		if (a[0] == '-' && strcmp(a, "-A") != 0 && strcmp(a, "--all") != 0)
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

/* active_branch() returns text: the session's branch */
Datum
active_branch(PG_FUNCTION_ARGS)
{
	if (prolly_database_uses_copies())
		PG_RETURN_TEXT_P(cstring_to_text(branch_ops()->current()));
	PG_RETURN_TEXT_P(cstring_to_text(session_branch_name()));
}

/* ------------------------------------------------------------------------
 * System views
 * ------------------------------------------------------------------------
 */

typedef struct LogEntry
{
	ProllyCommitInfo info;
	int64		order;			/* 1 + the most of its parents' */
} LogEntry;

static LogEntry *
log_find(List *entries, const ProllyHash *id)
{
	ListCell   *lc;

	foreach(lc, entries)
	{
		LogEntry   *e = lfirst(lc);

		if (same_hash(&e->info.id, id))
			return e;
	}
	return NULL;
}

static int
log_cmp(const ListCell *a, const ListCell *b)
{
	LogEntry   *x = lfirst(a);
	LogEntry   *y = lfirst(b);

	if (x->info.committed_at != y->info.committed_at)
		return x->info.committed_at > y->info.committed_at ? -1 : 1;
	if (x->order != y->order)
		return x->order > y->order ? -1 : 1;
	return memcmp(x->info.id.bytes, y->info.id.bytes, x->info.id.len);
}

/* The order of e: its parents' first (they are all in entries). */
static int64
log_order(List *entries, LogEntry *e)
{
	if (e->order == 0)
	{
		int64		most = 0;

		check_stack_depth();
		for (int i = 0; i < e->info.nparents; i++)
		{
			LogEntry   *p = log_find(entries, &e->info.parents[i]);

			if (p)
				most = Max(most, log_order(entries, p));
		}
		e->order = most + 1;
	}
	return e->order;
}

/*
 * dolt_log(ref text, OUT commit_hash text, OUT committer text,
 *			OUT email text, OUT date timestamptz, OUT message text,
 *			OUT commit_order int8) returns setof record
 *
 * Every commit ref reaches, newest first.
 */
Datum
dolt_log(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	char	   *ref = text_to_cstring(PG_GETARG_TEXT_PP(0));
	ProllyHash	start;
	List	   *entries = NIL;
	List	   *todo = NIL;
	ListCell   *lc;

	InitMaterializedSRF(fcinfo, 0);
	if (!PROLLY_CALL(refs(), resolve, ref, &start))
	{
		if (strcmp(ref, "HEAD") == 0)
			return (Datum) 0;	/* nothing committed yet */
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("prolly ref \"%s\" does not exist", ref)));
	}
	todo = lappend(todo, copy_hash(&start));
	while (todo != NIL)
	{
		ProllyHash *id = linitial(todo);
		LogEntry   *e;

		todo = list_delete_first(todo);
		CHECK_FOR_INTERRUPTS();
		if (log_find(entries, id))
			continue;
		e = palloc0(sizeof(LogEntry));
		if (!PROLLY_CALL(refs(), commit_get, id, &e->info))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("prolly commit %s is missing", hash_hex(id))));
		entries = lappend(entries, e);
		for (int i = 0; i < e->info.nparents; i++)
			todo = lappend(todo, copy_hash(&e->info.parents[i]));
	}
	foreach(lc, entries)
		(void) log_order(entries, lfirst(lc));
	list_sort(entries, log_cmp);
	foreach(lc, entries)
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

typedef struct BranchRows
{
	ReturnSetInfo *rsinfo;
} BranchRows;

static void
branch_row(const char *name, const ProllyHash *head, void *arg)
{
	BranchRows *br = arg;
	Datum		values[7];
	bool		nulls[7] = {0};
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
		values[6] = BoolGetDatum(PROLLY_CALL(refs(), working_set, name, &ws) &&
								 PROLLY_CALL(refs(), commit_working_set, head,
											 &committed) &&
								 !same_hash(&ws.hash, &committed.hash));
	}
	else
	{
		for (int i = 1; i < 6; i++)
			nulls[i] = true;
		values[6] = BoolGetDatum(true);
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

static int
name_cmp(const ListCell *a, const ListCell *b)
{
	return strcmp(lfirst(a), lfirst(b));
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

/* A table changed: its name, a tab, and its status. */
static void
status_table(const char *name, ProllyDiffKind kind, void *arg)
{
	List	  **names = arg;
	static const char *const statuses[] = {
		[PROLLY_DIFF_ADDED] = "new table",
		[PROLLY_DIFF_REMOVED] = "deleted",
		[PROLLY_DIFF_MODIFIED] = "modified",
	};

	*names = lappend(*names, psprintf("%s\t%s", name, statuses[kind]));
}

/*
 * dolt_status(OUT table_name text, OUT staged bool, OUT status text)
 *	  returns setof record
 *
 * The tables changed since the session branch's head commit: rows changed
 * ("modified"), made ("new table"), dropped ("deleted") or, with a merge
 * in progress, with conflicts ("conflict", not staged).
 */
Datum
dolt_status(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	StatusSink	s = {{status_change}, NIL};
	ProllyHash	head;
	List	   *names = NIL;
	ListCell   *lc;

	ProllyMergeState ms;
	List	   *conflicted = NIL;

	InitMaterializedSRF(fcinfo, 0);
	if (!PROLLY_CALL(refs(), resolve, "HEAD", &head))
		return (Datum) 0;

	/*
	 * Tables by name, so ones made or dropped since the head are too; a
	 * table with conflicts, as that.
	 */
	if (PROLLY_HAS(diff_merge(), tables))
	{
		PROLLY_CALL(diff_merge(), tables, "HEAD", "WORKING", status_table,
					&names);
		if (PROLLY_CALL(refs(), merge_state, &ms))
		{
			for (int i = 0; i < ms.ntables; i++)
			{
				char	   *name = qualified_name(ms.tables[i]);
				size_t		len = strlen(name);

				foreach(lc, names)
				{
					char	   *n = lfirst(lc);

					if (strncmp(n, name, len) == 0 && n[len] == '\t')
						names = foreach_delete_current(names, lc);
				}
				names = lappend(names, psprintf("%s\tconflict", name));
			}
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
		foreach(lc, s.relids)
		{
			Oid			relid = lfirst_oid(lc);

			/* Name, then a tab and the status, to sort by name. */
			names = lappend(names,
							psprintf("%s\t%s",
									 qualified_name(relid),
									 list_member_oid(conflicted, relid) ?
									 "conflict" : "modified"));
		}
	}
	list_sort(names, name_cmp);
	foreach(lc, names)
	{
		Datum		values[3];
		bool		nulls[3] = {0};
		char	   *tab = strchr(lfirst(lc), '\t');

		*tab = '\0';
		values[0] = CStringGetTextDatum(lfirst(lc));
		values[1] = BoolGetDatum(strcmp(tab + 1, "conflict") != 0);
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
} Rows;

static void
rows_begin(Rows *r, Relation rel)
{
	prolly_components_for_relation(rel, &r->c);
	/* (a block-mapped table's trees are its folds, rows as a rewrite has them) */
	r->keying = rel->rd_rel->relam == PROLLY_TABLE_AM_OID ?
		PROLLY_CALL(r->c.keying, open_rewritten, rel, CurrentMemoryContext) :
		PROLLY_CALL(r->c.keying, open, rel, NULL, CurrentMemoryContext);
	r->keyless = PROLLY_CALL(r->c.keying, keyless, r->keying);
	r->slot = MakeSingleTupleTableSlot(RelationGetDescr(rel), &TTSOpsVirtual);
}

static void
rows_end(Rows *r)
{
	ExecDropSingleTupleTableSlot(r->slot);
	PROLLY_CALL(r->c.keying, close, r->keying);
}

/* The row a tree value holds, as a composite datum; and its copies. */
static Datum
rows_datum(Rows *r, const ProllyValue *value, uint64 *count)
{
	ProllyValue row;

	PROLLY_CALL(r->c.keying, parse_value, r->keying, value, &row, count);
	ExecClearTuple(r->slot);
	PROLLY_CALL(r->c.keying, decode_row, r->keying, &row, r->slot);
	return ExecFetchSlotHeapTupleDatum(r->slot);
}

/*
 * The prolly table whose row type is type, opened, if the user has mode
 * on it.
 */
static Relation
open_row_type_table(Oid relid, const char *fn, AclMode mode)
{
	Relation	rel;
	AclResult	aclresult;

	if (!OidIsValid(relid) || get_rel_relkind(relid) != RELKIND_RELATION)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s needs a table's row type, as in NULL::tablename", fn)));
	aclresult = pg_class_aclcheck(relid, GetUserId(), mode);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_TABLE, get_rel_name(relid));
	rel = table_open(relid, AccessShareLock);
	if (!prolly_is_table_am(get_am_name(rel->rd_rel->relam)) &&
		rel->rd_rel->relam != PROLLY_TABLE_AM_OID)
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
} RowDiffSink;

static void
put_diff(RowDiffSink *s, const char *kind, Datum from, bool from_null,
		 Datum to, bool to_null, uint64 times)
{
	Datum		values[3];
	bool		nulls[3] = {false, from_null, to_null};

	values[0] = CStringGetTextDatum(kind);
	values[1] = from;
	values[2] = to;
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
		return;
	}
	if (kind == PROLLY_DIFF_ADDED)
		put_diff(s, "added", (Datum) 0, true, to, false, 1);
	else if (kind == PROLLY_DIFF_REMOVED)
		put_diff(s, "removed", from, false, (Datum) 0, true, 1);
	else
		put_diff(s, "modified", from, false, to, false, 1);
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
	char	   *from_ref = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *to_ref = text_to_cstring(PG_GETARG_TEXT_PP(1));
	Oid			type = get_fn_expr_argtype(fcinfo->flinfo, 2);
	Oid			relid = OidIsValid(type) ? get_typ_typrelid(type) : InvalidOid;
	Relation	rel;
	RowDiffSink s;

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
} CountSink;

static void
count_conflict(ProllyConflictSink *self, const ProllyKey *key,
			   const ProllyValue *base, const ProllyValue *ours,
			   const ProllyValue *theirs)
{
	((CountSink *) self)->n++;
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
		CountSink	cs = {{count_conflict}, 0};
		Datum		values[2];
		bool		nulls[2] = {0};

		PROLLY_CALL(diff_merge(), conflicts, ms.tables[i], &cs.sink);
		values[0] = CStringGetTextDatum(qualified_name(ms.tables[i]));
		values[1] = Int64GetDatum(cs.n);
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
	if (pg_class_aclcheck(relid, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE, get_rel_name(relid));
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
 * Check the tables (none: every versioned one) against their foreign keys
 * again, as Dolt's dolt_verify_constraints: their recorded violations are
 * replaced by what is found now (rows a merge didn't write for breaking a
 * unique, check or not-null constraint are dropped: what to do with them
 * is the user's).  Status 1 if any are left, else 0.
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

	for (int i = 0; i < nargs; i++)
	{
		if (args[i][0] == '-')
			unsupported("dolt_verify_constraints", args[i]);
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
	found = PROLLY_CALL(diff_merge(), verify, relids);
	if (relids != NIL)
	{
		/* (a table's own foreign keys, not those of tables referencing it) */
		foreach(lc, found)
			if (list_member_oid(relids, ((ProllyViolation *) lfirst(lc))->relid))
				kept = lappend(kept, lfirst(lc));
	}
	else
		kept = found;
	PROLLY_CALL(refs(), set_violations, kept);
	result[0] = kept != NIL ? "1" : "0";
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

	if (base)
		values[0] = datumCopy(rows_datum(&s->rows, base, &count), false, -1);
	if (ours)
		values[1] = datumCopy(rows_datum(&s->rows, ours, &count), false, -1);
	if (theirs)
		values[2] = rows_datum(&s->rows, theirs, &count);
	values[3] = CStringGetTextDatum(change_kind(base, ours));
	values[4] = CStringGetTextDatum(change_kind(base, theirs));
	tuplestore_putvalues(s->rsinfo->setResult, s->rsinfo->setDesc, values,
						 nulls);
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
	List	   *tables = NIL;
	ListCell   *lc;
	ProllyMergeState ms;
	const char *result[1] = {"0"};

	for (int i = 0; i < nargs; i++)
	{
		char	   *a = args[i];

		if (strcmp(a, "--ours") == 0)
			side = 1;
		else if (strcmp(a, "--theirs") == 0)
			side = 2;
		else if (a[0] == '-')
			unsupported("dolt_conflicts_resolve", a);
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
	foreach(lc, tables)
		PROLLY_CALL(diff_merge(), resolve, lfirst_oid(lc), side == 2);
	PG_RETURN_DATUM(text_array(1, result));
}
