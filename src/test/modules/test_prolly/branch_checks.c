/*--------------------------------------------------------------------------
 *
 * branch_checks.c
 *		Switching a session's branch, for tests of branch-aware catalogs
 *		before branches can be made and checked out for real.
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
