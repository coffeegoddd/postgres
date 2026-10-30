/*-------------------------------------------------------------------------
 *
 * test_prolly.c
 *	  Runners for the unit tests and conformance suites in test_prolly.h.
 *
 * test_prolly_unit_tests() runs the unit tests, and
 * test_prolly_conformance() runs every conformance suite against every
 * implementation registered under its interface.  Each returns one row per
 * test, with its result ("ok" or "failed") and, for a failure, the error it
 * raised and where.  A suite whose interface has no implementations
 * registered reports one row saying so, rather than nothing, so a missing
 * registration cannot pass for a passing suite.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/test_prolly/test_prolly.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "test_prolly.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/tuplestore.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_prolly_unit_tests);
PG_FUNCTION_INFO_V1(test_prolly_conformance);
PG_FUNCTION_INFO_V1(test_prolly_loaded_at_preload);

/*
 * Whether this module was loaded through shared_preload_libraries.  A test
 * that means to check registration at preload needs to know, because the
 * first call of any of the module's functions loads it anyway, and then
 * the registrations happen in the calling backend instead.
 */
static bool loaded_at_preload = false;

static const ProllyUnitSuite *const unit_suites[] = {
	&unit_harness_suite,
	&unit_bufmgr_suite,
	&unit_registry_suite,
	&unit_smgr_suite,
};

static const ProllyConformanceSuite *const conformance_suites[] = {
	&conformance_test_checksum_suite,
	&conformance_branch_ids_suite,
	&conformance_branch_map_suite,
};

/* The outcome of one test. */
typedef struct TestOutcome
{
	bool		ok;
	char	   *message;		/* the error, if it failed */
	char	   *location;		/* where it was raised, if it failed */
} TestOutcome;

void
_PG_init(void)
{
	loaded_at_preload = process_shared_preload_libraries_in_progress;
	register_test_checksum_implementations();
	register_branch_implementations();
}

/*
 * test_prolly_loaded_at_preload() returns bool
 */
Datum
test_prolly_loaded_at_preload(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(loaded_at_preload);
}

/*
 * Call fn(arg) in a subtransaction and a memory context of its own, and
 * report whether it raised an error.  The subtransaction undoes whatever
 * database changes a failed test made, and the memory context frees what
 * any test allocated, so one test cannot affect the next.
 */
static TestOutcome
run_isolated(void (*fn) (const void *arg), const void *arg)
{
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;
	MemoryContext testcontext;
	TestOutcome outcome = {true, NULL, NULL};

	testcontext = AllocSetContextCreate(oldcontext, "test_prolly test",
										ALLOCSET_DEFAULT_SIZES);

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(testcontext);

	PG_TRY();
	{
		fn(arg);

		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();

		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;

		outcome.ok = false;
		outcome.message = edata->message ? pstrdup(edata->message) : pstrdup("(no message)");
		if (edata->filename)
			outcome.location = psprintf("%s:%d", edata->filename, edata->lineno);
		FreeErrorData(edata);
	}
	PG_END_TRY();

	MemoryContextDelete(testcontext);
	return outcome;
}

/*
 * Add a result row: the test's names (NULL for one that does not apply),
 * its result, and the error and location of a failure.
 */
static void
put_row(ReturnSetInfo *rsinfo, const char *const *names, int nnames,
		const char *result, const TestOutcome *outcome)
{
	Datum		values[8];
	bool		nulls[8] = {0};
	const char *rest[3];
	int			i = 0;

	Assert(nnames + 3 == rsinfo->setDesc->natts);
	rest[0] = result;
	rest[1] = outcome ? outcome->message : NULL;
	rest[2] = outcome ? outcome->location : NULL;

	for (int n = 0; n < nnames + 3; n++)
	{
		const char *v = n < nnames ? names[n] : rest[n - nnames];

		if (v)
			values[i] = CStringGetTextDatum(v);
		else
			nulls[i] = true;
		i++;
	}

	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

static void
call_unit_test(const void *arg)
{
	const ProllyUnitTest *test = arg;

	test->fn();
}

/*
 * test_prolly_unit_tests(suite_name text) returns setof
 *		(suite text, test text, result text, detail text, location text)
 *
 * Runs every unit test, or those of the suite named suite_name.
 */
Datum
test_prolly_unit_tests(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	char	   *suite_name = PG_ARGISNULL(0) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(0));
	bool		found = false;

	InitMaterializedSRF(fcinfo, 0);

	for (int s = 0; s < lengthof(unit_suites); s++)
	{
		const ProllyUnitSuite *suite = unit_suites[s];

		if (suite_name && strcmp(suite_name, suite->name) != 0)
			continue;
		found = true;
		for (int t = 0; t < suite->ntests; t++)
		{
			TestOutcome outcome = run_isolated(call_unit_test, &suite->tests[t]);

			const char *names[] = {suite->name, suite->tests[t].name};

			put_row(rsinfo, names, 2, outcome.ok ? "ok" : "failed", &outcome);
		}
	}

	if (suite_name && !found)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("unit test suite \"%s\" does not exist", suite_name)));

	return (Datum) 0;
}

typedef struct ConformanceCall
{
	const ProllyConformanceCase *test;
	const ProllyImplementation *impl;
} ConformanceCall;

static void
call_conformance_case(const void *arg)
{
	const ConformanceCall *call = arg;

	call->test->fn(call->impl);
}

/*
 * Every implementation, whatever its interface, must be the one a lookup by
 * its own names finds; otherwise code choosing an implementation by name
 * would get a different one than the suite checked.
 */
static void
check_registered(const void *arg)
{
	const ProllyImplementation *impl = arg;

	CHECK(ProllyLookupImplementation(impl->iface, impl->name) == impl);
	CHECK(impl->routine != NULL);
}

/*
 * test_prolly_conformance(iface_name text) returns setof
 *		(iface text, implementation text, test text, result text,
 *		 detail text, location text)
 *
 * Runs every conformance suite, or the one for the interface named iface_name,
 * against each implementation registered under its interface.  Every
 * implementation also gets the case "registered", which all interfaces
 * share.
 */
Datum
test_prolly_conformance(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	char	   *iface_name = PG_ARGISNULL(0) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(0));
	bool		found = false;

	InitMaterializedSRF(fcinfo, 0);

	for (int s = 0; s < lengthof(conformance_suites); s++)
	{
		const ProllyConformanceSuite *suite = conformance_suites[s];
		List	   *impls;

		if (iface_name && strcmp(iface_name, suite->iface) != 0)
			continue;
		found = true;

		impls = ProllyListImplementations(suite->iface);
		if (impls == NIL)
		{
			const char *names[] = {suite->iface, NULL, NULL};

			put_row(rsinfo, names, 3, "no implementations", NULL);
			continue;
		}

		foreach_ptr(const ProllyImplementation, impl, impls)
		{
			TestOutcome outcome = run_isolated(check_registered, impl);
			const char *names[] = {suite->iface, impl->name, "registered"};

			put_row(rsinfo, names, 3, outcome.ok ? "ok" : "failed", &outcome);

			for (int c = 0; c < suite->ncases; c++)
			{
				ConformanceCall call = {&suite->cases[c], impl};

				outcome = run_isolated(call_conformance_case, &call);
				names[2] = suite->cases[c].name;
				put_row(rsinfo, names, 3, outcome.ok ? "ok" : "failed", &outcome);
			}
		}
		list_free(impls);
	}

	if (iface_name && !found)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("no conformance suite for interface \"%s\"", iface_name)));

	return (Datum) 0;
}
