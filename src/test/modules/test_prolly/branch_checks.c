/*--------------------------------------------------------------------------
 *
 * branch_checks.c
 *		Switching a session's branch, for tests of branch-aware catalogs
 *		before branches can be made and checked out for real; the branches a
 *		transaction has written; and the refs' commit, called as the engine
 *		calls it.
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
#include "catalog/catalog.h"
#include "catalog/pg_branch.h"
#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "funcapi.h"
#include "pgstat.h"
#include "storage/lock.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"

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

/* test_branches_written(): the names of the branches the transaction has written */
PG_FUNCTION_INFO_V1(test_branches_written);

Datum
test_branches_written(PG_FUNCTION_ARGS)
{
	List	   *branches = branches_written();
	Datum	   *names = palloc(sizeof(Datum) * (list_length(branches) + 1));
	int			n = 0;
	ListCell   *lc;

	foreach(lc, branches)
	{
		HeapTuple	tup = SearchSysCache1(BRANCHOID,
										  ObjectIdGetDatum(lfirst_oid(lc)));

		if (!HeapTupleIsValid(tup))
			names[n++] = CStringGetTextDatum(psprintf("%u", lfirst_oid(lc)));
		else
		{
			names[n++] = CStringGetTextDatum(NameStr(((Form_pg_branch) GETSTRUCT(tup))->brname));
			ReleaseSysCache(tup);
		}
	}
	PG_RETURN_ARRAYTYPE_P(construct_array_builtin(names, n, TEXTOID));
}

/*
 * Whether another backend waits for a lock this one holds, or for its
 * virtual transaction (AnyLockHasWaiters(), which a paced fold asks before
 * each nap).
 */
PG_FUNCTION_INFO_V1(test_any_lock_has_waiters);
Datum
test_any_lock_has_waiters(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(AnyLockHasWaiters());
}

/*
 * The engine's work counters as this backend alone has counted them, since
 * it began (pgstat_prolly_session_counts()): one (name, value) row each.
 */
PG_FUNCTION_INFO_V1(test_prolly_session_stats);
Datum
test_prolly_session_stats(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	PgStat_Counter counts[PGSTAT_PROLLY_NUM_COUNTERS];

	InitMaterializedSRF(fcinfo, 0);
	pgstat_prolly_session_counts(counts);
	for (int i = 0; i < PGSTAT_PROLLY_NUM_COUNTERS; i++)
	{
		Datum		values[2];
		bool		nulls[2] = {false, false};

		values[0] = CStringGetTextDatum(pgstat_prolly_counter_name(i));
		values[1] = Int64GetDatum(counts[i]);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

/*
 * What the caches let go of: an injection point callback, attached with
 * injection_points_attach('catcache-reset' or 'relcache-reset-entry',
 * 'test_prolly', 'test_count_cache_reset', NULL), noting the catalogs
 * whose caches are reset, and counting the system catalogs' relation
 * entries invalidated, in this backend.
 */
#define MAX_RESET_CATALOGS 128

static Oid	reset_catalogs[MAX_RESET_CATALOGS];
static int	nreset_catalogs = 0;
static int64 system_entries_reset = 0;

extern PGDLLEXPORT void test_count_cache_reset(const char *name,
											   const void *private_data,
											   void *arg);

void
test_count_cache_reset(const char *name, const void *private_data, void *arg)
{
	if (strcmp(name, "catcache-reset") == 0)
	{
		Oid			reloid = ((CatCache *) arg)->cc_reloid;

		for (int i = 0; i < nreset_catalogs; i++)
			if (reset_catalogs[i] == reloid)
				return;
		if (nreset_catalogs < MAX_RESET_CATALOGS)
			reset_catalogs[nreset_catalogs++] = reloid;
	}
	else if (IsCatalogRelationOid(RelationGetRelid((Relation) arg)))
		system_entries_reset++;
}

/*
 * test_cache_resets(): the catalogs whose caches were reset, and the
 * system catalogs' relation entries invalidated, since the last call
 */
PG_FUNCTION_INFO_V1(test_cache_resets);

Datum
test_cache_resets(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[2];
	bool		nulls[2] = {false, false};
	Datum	   *oids = palloc_array(Datum, Max(nreset_catalogs, 1));

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	for (int i = 0; i < nreset_catalogs; i++)
		oids[i] = ObjectIdGetDatum(reset_catalogs[i]);
	values[0] = PointerGetDatum(construct_array_builtin(oids, nreset_catalogs,
														OIDOID));
	values[1] = Int64GetDatum(system_entries_reset);
	nreset_catalogs = 0;
	system_entries_reset = 0;
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/* test_syscache_catalog(relid): whether a catalog cache is of relid */
PG_FUNCTION_INFO_V1(test_syscache_catalog);

Datum
test_syscache_catalog(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(RelationHasSysCache(PG_GETARG_OID(0)));
}
