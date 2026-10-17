/*-------------------------------------------------------------------------
 *
 * prolly_vcs.c
 *	  SQL functions for version-control operations.
 *
 * Each function is a thin adapter onto one call of the refs, diff/merge or
 * transfer interface (prolly_config.h).  Names and signatures are
 * provisional: they are what the version-control benchmarks call.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_vcs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_config.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/tuplestore.h"

static text *
hash_to_text(const ProllyHash *hash)
{
	char	   *hex = palloc(hash->len * 2 + 1);

	hex_encode((const char *) hash->bytes, hash->len, hex);
	hex[hash->len * 2] = '\0';
	return cstring_to_text(hex);
}

/* prolly_branch_create(branch text, start_point text) returns void (null) */
Datum
prolly_branch_create(PG_FUNCTION_ARGS)
{
	ProllyDatabaseComponents components;
	const ProllyRefsRoutine *refs;
	char	   *branch;
	char	   *start_point = NULL;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("branch name must not be null")));
	branch = text_to_cstring(PG_GETARG_TEXT_PP(0));
	if (!PG_ARGISNULL(1))
		start_point = text_to_cstring(PG_GETARG_TEXT_PP(1));

	prolly_database_components(&components);
	refs = components.refs;
	PROLLY_CALL(refs, branch_create, branch, start_point);
	PG_RETURN_NULL();
}

/* prolly_checkout(branch text) returns void (null) */
Datum
prolly_checkout(PG_FUNCTION_ARGS)
{
	ProllyDatabaseComponents components;
	const ProllyRefsRoutine *refs;
	char	   *branch = text_to_cstring(PG_GETARG_TEXT_PP(0));

	prolly_database_components(&components);
	refs = components.refs;
	PROLLY_CALL(refs, checkout, branch);
	PG_RETURN_NULL();
}

/* prolly_commit(message text) returns text (the commit id) */
Datum
prolly_commit(PG_FUNCTION_ARGS)
{
	ProllyDatabaseComponents components;
	const ProllyRefsRoutine *refs;
	char	   *message = text_to_cstring(PG_GETARG_TEXT_PP(0));
	ProllyHash	commit_id = {0};

	prolly_database_components(&components);
	refs = components.refs;
	PROLLY_CALL(refs, commit, message, &commit_id);
	PG_RETURN_TEXT_P(hash_to_text(&commit_id));
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

/* Collects diff rows into a tuplestore. */
typedef struct DiffCollector
{
	ProllyDiffSink sink;		/* must be first */
	ReturnSetInfo *rsinfo;
} DiffCollector;

static void
diff_collect(ProllyDiffSink *self, Oid relid, ProllyDiffKind kind,
			 const ProllyKey *key, const ProllyValue *before,
			 const ProllyValue *after)
{
	DiffCollector *collector = (DiffCollector *) self;
	Datum		values[3];
	bool		nulls[3] = {0};
	bytea	   *keybytes = palloc(VARHDRSZ + key->len);
	static const char *const kinds[] = {
		[PROLLY_DIFF_ADDED] = "added",
		[PROLLY_DIFF_REMOVED] = "removed",
		[PROLLY_DIFF_MODIFIED] = "modified",
	};

	SET_VARSIZE(keybytes, VARHDRSZ + key->len);
	memcpy(VARDATA(keybytes), key->data, key->len);

	values[0] = ObjectIdGetDatum(relid);
	values[1] = CStringGetTextDatum(kinds[kind]);
	values[2] = PointerGetDatum(keybytes);
	tuplestore_putvalues(collector->rsinfo->setResult,
						 collector->rsinfo->setDesc, values, nulls);
}

/*
 * prolly_diff(from_ref text, to_ref text,
 *			   OUT relation regclass, OUT change text, OUT key bytea)
 *	  returns setof record
 */
Datum
prolly_diff(PG_FUNCTION_ARGS)
{
	ProllyDatabaseComponents components;
	const ProllyMergeRoutine *diff_merge;
	char	   *from_ref = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *to_ref = text_to_cstring(PG_GETARG_TEXT_PP(1));
	DiffCollector collector;

	InitMaterializedSRF(fcinfo, 0);
	collector.sink.change = diff_collect;
	collector.rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	prolly_database_components(&components);
	diff_merge = components.diff_merge;
	PROLLY_CALL(diff_merge, diff, from_ref, to_ref, InvalidOid,
				&collector.sink);
	return (Datum) 0;
}

/* prolly_merge(branch text) returns text (the merge commit id) */
Datum
prolly_merge(PG_FUNCTION_ARGS)
{
	ProllyDatabaseComponents components;
	const ProllyMergeRoutine *diff_merge;
	char	   *branch = text_to_cstring(PG_GETARG_TEXT_PP(0));
	ProllyHash	commit_id = {0};

	prolly_database_components(&components);
	diff_merge = components.diff_merge;
	(void) PROLLY_CALL(diff_merge, merge, branch, NULL,
					   PROLLY_MERGE_FAIL_ON_CONFLICT, &commit_id);
	PG_RETURN_TEXT_P(hash_to_text(&commit_id));
}

