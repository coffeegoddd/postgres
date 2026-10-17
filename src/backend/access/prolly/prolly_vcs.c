/*-------------------------------------------------------------------------
 *
 * prolly_vcs.c
 *	  SQL functions for version control beside the dolt_* ones: a commit's
 *	  canonical root hash, and a working-set checkpoint.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_vcs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_config.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"

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
