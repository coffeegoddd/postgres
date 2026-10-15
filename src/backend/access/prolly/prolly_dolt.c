/*-------------------------------------------------------------------------
 *
 * prolly_dolt.c
 *	  Doltgres's SQL interface to version control: the dolt_* functions
 *	  and the functions behind the dolt_log, dolt_branches and dolt_status
 *	  system views, the dolt.author setting, and "dbname/branch"
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
#include "access/tableam.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "executor/tuptable.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

/* dolt.author's value; read through GetConfigOption so a placeholder works. */
static char *dolt_author = NULL;

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
		defined = true;
	}
	if (branch)
	{
		ProllyDatabaseComponents dc;

		prolly_database_components(&dc);
		PROLLY_CALL(dc.refs, checkout, branch);
	}
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
		info.nparents > 0)
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
	if (create)
		PROLLY_CALL(refs(), branch_create, branch, start);
	PROLLY_CALL(refs(), checkout, branch);
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
	else if (copy)
	{
		if (list_length(names) != 2)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("dolt_branch -c needs the branch to copy and a new name")));
		PROLLY_CALL(refs(), branch_create, (char *) lsecond(names),
					(char *) linitial(names));
	}
	else
	{
		if (list_length(names) > 2)
			unexpected("dolt_branch", (char *) lthird(names));
		PROLLY_CALL(refs(), branch_create, (char *) linitial(names),
					list_length(names) > 1 ? (char *) lsecond(names) : NULL);
	}
	PG_RETURN_DATUM(text_array(1, result));
}

/*
 * dolt_merge(VARIADIC args text[]) returns text[]:
 *	  {commit id, fast_forward, conflicts, message}
 *
 * branch; --no-ff; -m/--message message.  Conflicts raise an error and
 * change nothing (Dolt's conflict tables aren't there yet).
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
	const char *result[4];

	for (int i = 0; i < nargs; i++)
	{
		char	   *a = args[i];

		if (strcmp(a, "--no-ff") == 0)
			flags |= PROLLY_MERGE_NO_FF;
		else if (strcmp(a, "-m") == 0 || strcmp(a, "--message") == 0)
			message = option_value(args, nargs, &i, "dolt_merge");
		else if (a[0] == '-')
			unsupported("dolt_merge", a);
		else if (branch == NULL)
			branch = a;
		else
			unexpected("dolt_merge", a);
	}
	if (branch == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dolt_merge needs a branch")));
	had_head = PROLLY_CALL(refs(), resolve, "HEAD", &before);
	(void) PROLLY_CALL(refs(), resolve, branch, &theirs);
	PROLLY_CALL(diff_merge(), merge, branch, message, flags, &id);
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

/*
 * dolt_status(OUT table_name text, OUT staged bool, OUT status text)
 *	  returns setof record
 *
 * The tables whose rows changed since the session branch's head commit
 * (until catalogs are versioned, schema changes don't show).
 */
Datum
dolt_status(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	StatusSink	s = {{status_change}, NIL};
	ProllyHash	head;
	List	   *names = NIL;
	ListCell   *lc;

	InitMaterializedSRF(fcinfo, 0);
	if (!PROLLY_CALL(refs(), resolve, "HEAD", &head))
		return (Datum) 0;
	PROLLY_CALL(diff_merge(), diff, "HEAD", "WORKING", InvalidOid, &s.sink);
	foreach(lc, s.relids)
	{
		Oid			relid = lfirst_oid(lc);

		names = lappend(names,
						quote_qualified_identifier(get_namespace_name(get_rel_namespace(relid)),
												   get_rel_name(relid)));
	}
	list_sort(names, name_cmp);
	foreach(lc, names)
	{
		Datum		values[3];
		bool		nulls[3] = {0};

		values[0] = CStringGetTextDatum(lfirst(lc));
		values[1] = BoolGetDatum(true);
		values[2] = CStringGetTextDatum("modified");
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

/* ------------------------------------------------------------------------
 * dolt_diff
 * ------------------------------------------------------------------------
 */

typedef struct RowDiffSink
{
	ProllyDiffSink sink;		/* must be first */
	ReturnSetInfo *rsinfo;
	ProllyComponents c;
	ProllyKeying *keying;
	bool		keyless;
	TupleTableSlot *slot;
} RowDiffSink;

/* The row a tree value holds, as a composite datum; and its copies. */
static Datum
row_datum(RowDiffSink *s, const ProllyValue *value, uint64 *count)
{
	ProllyValue row;

	PROLLY_CALL(s->c.keying, parse_value, s->keying, value, &row, count);
	ExecClearTuple(s->slot);
	PROLLY_CALL(s->c.keying, decode_row, s->keying, &row, s->slot);
	return ExecFetchSlotHeapTupleDatum(s->slot);
}

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
		from = row_datum(s, before, &nbefore);
	if (after)
		to = row_datum(s, after, &nafter);

	/* A keyless row's copies: each one added or removed is a row. */
	if (s->keyless)
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
	AclResult	aclresult;

	if (!OidIsValid(relid) || get_rel_relkind(relid) != RELKIND_RELATION)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("dolt_diff needs a table's row type, as in NULL::tablename")));
	aclresult = pg_class_aclcheck(relid, GetUserId(), ACL_SELECT);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_TABLE, get_rel_name(relid));

	InitMaterializedSRF(fcinfo, 0);
	rel = table_open(relid, AccessShareLock);
	if (!prolly_is_table_am(get_am_name(rel->rd_rel->relam)))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a prolly table", RelationGetRelationName(rel))));
	memset(&s, 0, sizeof(s));
	s.sink.change = row_change;
	s.rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	prolly_components_for_relation(rel, &s.c);
	s.keying = PROLLY_CALL(s.c.keying, open, rel, NULL, CurrentMemoryContext);
	s.keyless = PROLLY_CALL(s.c.keying, keyless, s.keying);
	s.slot = MakeSingleTupleTableSlot(RelationGetDescr(rel), &TTSOpsVirtual);
	PROLLY_CALL(diff_merge(), diff, from_ref, to_ref, relid, &s.sink);
	ExecDropSingleTupleTableSlot(s.slot);
	PROLLY_CALL(s.c.keying, close, s.keying);
	table_close(rel, AccessShareLock);
	return (Datum) 0;
}
