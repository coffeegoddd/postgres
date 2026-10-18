/*--------------------------------------------------------------------------
 *
 * branch_checks.c
 *		Switching a session's branch, for tests of branch-aware catalogs
 *		before branches can be made and checked out for real; and the refs'
 *		commit, called as the engine calls it.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_prolly/branch_checks.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/prolly_config.h"
#include "access/prolly_impl.h"
#include "catalog/branch.h"
#include "catalog/pg_branch.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/syscache.h"

/*
 * test_branch_switch(name): make the pg_branch row called name the session's
 * branch.
 */
PG_FUNCTION_INFO_V1(test_branch_switch);

Datum
test_branch_switch(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	HeapTuple	tup;
	Oid			branch;

	tup = SearchSysCache1(BRANCHNAME, CStringGetDatum(name));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("branch \"%s\" does not exist", name)));
	branch = ((Form_pg_branch) GETSTRUCT(tup))->oid;
	ReleaseSysCache(tup);
	set_session_branch(branch);
	PG_RETURN_VOID();
}

/* test_branch_current(): the session's branch */
PG_FUNCTION_INFO_V1(test_branch_current);

Datum
test_branch_current(PG_FUNCTION_ARGS)
{
	PG_RETURN_OID(MyBranch);
}

/*
 * test_refs_commit(message, allow_empty): the refs' commit of the session
 * branch, as the engine calls it; its id, or NULL when it was refused as
 * empty (and recorded nothing).
 */
PG_FUNCTION_INFO_V1(test_refs_commit);

Datum
test_refs_commit(PG_FUNCTION_ARGS)
{
	char	   *message = text_to_cstring(PG_GETARG_TEXT_PP(0));
	bool		allow_empty = PG_GETARG_BOOL(1);
	ProllyDatabaseComponents db;
	ProllyHash	id;

	prolly_database_components(&db);
	if (!PROLLY_CALL(db.refs, commit, message, allow_empty, &id))
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(cstring_to_text(prolly_hash_hex(&id)));
}
