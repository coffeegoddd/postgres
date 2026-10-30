/*-------------------------------------------------------------------------
 *
 * branch_conformance.c
 *	  Conformance suites for the interfaces of branched catalogs.
 *
 * branch_ids allocates branch identifiers.  Its contract: every identifier
 * is positive and handed out once, later ones are larger, allocated()
 * knows exactly which have been handed out, and an identifier allocated
 * by a transaction that rolled back stays handed out.
 *
 * branch_map answers which branch a catalog object belongs to.  Its
 * contract: 0 for shared objects and for objects that do not exist, the
 * branch of its versioned schema for an object in one, its owner's branch
 * for an object that belongs to another (a column default, a trigger, a
 * TOAST table and its index), the same answer on whatever branch the
 * session is, and nothing kept past reset().  Its cases make their objects
 * in a subtransaction they roll back.
 *
 * test_branch_map_stats() reports what the map in use did, so that SQL
 * tests can show what a query cost it.
 *
 * The server's implementations are registered here, under the interfaces'
 * names, so the runner checks them; a new implementation is checked by
 * adding it to the registration below.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/test_prolly/branch_conformance.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "prolly/branch_catalog.h"
#include "prolly/branch_filter.h"
#include "prolly/registry.h"
#include "test_prolly.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/resowner.h"

#define BRANCH_ID_IFACE "branch_ids"
#define BRANCH_MAP_IFACE "branch_map"

void
register_branch_implementations(void)
{
	ProllyRegisterImplementation(BRANCH_ID_IFACE, "sequence",
								 &SequenceBranchIds);
	ProllyRegisterImplementation(BRANCH_MAP_IFACE, "cached",
								 &CachedBranchMap);
}

static const BranchIdRoutine *
ids(const ProllyImplementation *impl)
{
	return (const BranchIdRoutine *) impl->routine;
}

static void
allocates_increasing(const ProllyImplementation *impl)
{
	int32		prev = ids(impl)->allocate();

	CHECK_LT(0, prev);
	for (int i = 0; i < 20; i++)
	{
		int32		next = ids(impl)->allocate();

		CHECK_LT(prev, next);
		prev = next;
	}
}

static void
knows_what_it_allocated(const ProllyImplementation *impl)
{
	int32		id = ids(impl)->allocate();

	CHECK(ids(impl)->allocated(id));
	CHECK(ids(impl)->allocated(1));
	CHECK(!ids(impl)->allocated(id + 1));
	CHECK(!ids(impl)->allocated(0));
	CHECK(!ids(impl)->allocated(-1));
	CHECK(!ids(impl)->allocated(PG_INT32_MAX));
}

/*
 * An identifier a rolled-back transaction allocated may have been kept
 * somewhere outside the transaction (a session's setting), so it is never
 * handed out again.
 */
static void
rollback_keeps_identifiers(const ProllyImplementation *impl)
{
	MemoryContext cxt = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	int32		rolled_back;
	int32		next;

	BeginInternalSubTransaction(NULL);
	rolled_back = ids(impl)->allocate();
	RollbackAndReleaseCurrentSubTransaction();
	MemoryContextSwitchTo(cxt);
	CurrentResourceOwner = owner;

	next = ids(impl)->allocate();
	CHECK_LT(rolled_back, next);
	CHECK(ids(impl)->allocated(rolled_back));
}

static const ProllyConformanceCase branch_id_cases[] = {
	{"allocates_increasing", allocates_increasing},
	{"knows_what_it_allocated", knows_what_it_allocated},
	{"rollback_keeps_identifiers", rollback_keeps_identifiers},
};

const ProllyConformanceSuite conformance_branch_ids_suite = {
	BRANCH_ID_IFACE, branch_id_cases, lengthof(branch_id_cases)
};

static const BranchMapRoutine *
map(const ProllyImplementation *impl)
{
	return (const BranchMapRoutine *) impl->routine;
}

static void
run_sql(const char *sql)
{
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	if (SPI_execute(sql, false, 0) < 0)
		elog(ERROR, "could not run: %s", sql);
	SPI_finish();
	CommandCounterIncrement();
}

static void
set_branch(int32 branch)
{
	char		buf[32];

	snprintf(buf, sizeof(buf), "%d", branch);
	SetConfigOption("catalog_branch", buf, PGC_USERSET, PGC_S_SESSION);
}

/*
 * Runs fn in a subtransaction that is rolled back, so the objects it made
 * go, and the session's branch with them.
 */
static void
with_fixture(void (*fn) (const ProllyImplementation *impl, int32 branch),
			 const ProllyImplementation *impl)
{
	MemoryContext cxt = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;

	BeginInternalSubTransaction(NULL);
	PG_TRY();
	{
		int32		branch = BranchIds->allocate();

		set_branch(branch);
		run_sql("CREATE SCHEMA branch_map_fixture VERSIONED;"
				"CREATE TABLE branch_map_fixture.t (id int PRIMARY KEY, v text DEFAULT 'x');"
				"CREATE FUNCTION branch_map_fixture.trig() RETURNS trigger"
				" LANGUAGE plpgsql AS 'BEGIN RETURN NEW; END';"
				"CREATE TRIGGER tr BEFORE INSERT ON branch_map_fixture.t"
				" FOR EACH ROW EXECUTE FUNCTION branch_map_fixture.trig()");
		fn(impl, branch);
	}
	PG_FINALLY();
	{
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(cxt);
		CurrentResourceOwner = owner;
	}
	PG_END_TRY();
}

/* The one object id a query of the fixture returns. */
static Oid
oid_of(const char *sql)
{
	Oid			result;
	bool		isnull;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	if (SPI_execute(sql, false, 0) != SPI_OK_SELECT || SPI_processed != 1)
		elog(ERROR, "expected one row from: %s (%d rows)", sql, (int) SPI_processed);
	result = DatumGetObjectId(SPI_getbinval(SPI_tuptable->vals[0],
											SPI_tuptable->tupdesc, 1, &isnull));
	SPI_finish();
	CHECK(!isnull);
	return result;
}

static Oid
fixture_table(void)
{
	return oid_of("SELECT 'branch_map_fixture.t'::regclass::oid");
}

static void
shared_and_missing_objects(const ProllyImplementation *impl)
{
	CHECK_EQ(map(impl)->object_branch(NamespaceRelationId, PG_CATALOG_NAMESPACE), 0);
	CHECK_EQ(map(impl)->object_branch(RelationRelationId, RelationRelationId), 0);
	CHECK_EQ(map(impl)->object_branch(TypeRelationId, INT4OID), 0);
	/* the TOAST table of a catalog */
	CHECK_EQ(map(impl)->object_branch(RelationRelationId, 4163), 0);
	/* object ids no object has */
	CHECK_EQ(map(impl)->object_branch(RelationRelationId, InvalidOid), 0);
	CHECK_EQ(map(impl)->object_branch(RelationRelationId, PG_UINT32_MAX - 1), 0);
}

static void
check_versioned_objects(const ProllyImplementation *impl, int32 branch)
{
	Oid			rel = fixture_table();
	Oid			trigger = oid_of("SELECT oid FROM pg_trigger WHERE tgname = 'tr'");

	CHECK_EQ(map(impl)->object_branch(NamespaceRelationId,
									  oid_of("SELECT 'branch_map_fixture'::regnamespace::oid")),
			 branch);
	CHECK_EQ(map(impl)->object_branch(RelationRelationId, rel), branch);
	CHECK_EQ(map(impl)->object_branch(TypeRelationId,
									  oid_of("SELECT 'branch_map_fixture.t'::regtype::oid")),
			 branch);
	CHECK_EQ(map(impl)->object_branch(ProcedureRelationId,
									  oid_of("SELECT 'branch_map_fixture.trig'::regproc::oid")),
			 branch);

	/* objects that belong to the table */
	CHECK_EQ(map(impl)->object_branch(RelationRelationId,
									  oid_of("SELECT reltoastrelid FROM pg_class"
											 " WHERE oid = 'branch_map_fixture.t'::regclass")),
			 branch);
	CHECK_EQ(map(impl)->object_branch(RelationRelationId,
									  oid_of("SELECT i.indexrelid FROM pg_index i JOIN pg_class c"
											 " ON c.reltoastrelid = i.indrelid"
											 " WHERE c.oid = 'branch_map_fixture.t'::regclass")),
			 branch);
	CHECK_EQ(map(impl)->object_branch(TriggerRelationId, trigger), branch);
	CHECK_EQ(map(impl)->object_branch(AttrDefaultRelationId,
									  oid_of("SELECT oid FROM pg_attrdef"
											 " WHERE adrelid = 'branch_map_fixture.t'::regclass")),
			 branch);

	/* the same answers on no branch */
	set_branch(0);
	CHECK_EQ(map(impl)->object_branch(RelationRelationId, rel), branch);
	CHECK_EQ(map(impl)->object_branch(TriggerRelationId, trigger), branch);
}

static void
versioned_objects(const ProllyImplementation *impl)
{
	with_fixture(check_versioned_objects, impl);
}

static void
check_reset(const ProllyImplementation *impl, int32 branch)
{
	Oid			rel = fixture_table();
	BranchMapStats before;
	BranchMapStats after;

	CHECK_EQ(map(impl)->object_branch(RelationRelationId, rel), branch);
	map(impl)->stats(&before);
	CHECK_LT(0, before.entries);

	/* asked again, nothing is read */
	CHECK_EQ(map(impl)->object_branch(RelationRelationId, rel), branch);
	map(impl)->stats(&after);
	CHECK_EQ(after.reads, before.reads);

	map(impl)->reset();
	map(impl)->stats(&after);
	CHECK_EQ(after.entries, 0);
	CHECK_EQ(map(impl)->object_branch(RelationRelationId, rel), branch);
	map(impl)->stats(&after);
	CHECK_LT(before.reads, after.reads);
}

static void
reset_forgets(const ProllyImplementation *impl)
{
	with_fixture(check_reset, impl);
}

static void
check_dropped(const ProllyImplementation *impl, int32 branch)
{
	Oid			rel = fixture_table();
	BranchMapStats before;
	BranchMapStats after;

	CHECK_EQ(map(impl)->object_branch(RelationRelationId, rel), branch);
	map(impl)->stats(&before);
	run_sql("DROP TABLE branch_map_fixture.t");
	map(impl)->stats(&after);
	CHECK_LT(before.forgotten, after.forgotten);
}

static void
forgets_dropped_objects(const ProllyImplementation *impl)
{
	with_fixture(check_dropped, impl);
}

static const ProllyConformanceCase branch_map_cases[] = {
	{"shared_and_missing_objects", shared_and_missing_objects},
	{"versioned_objects", versioned_objects},
	{"reset_forgets", reset_forgets},
	{"forgets_dropped_objects", forgets_dropped_objects},
};

const ProllyConformanceSuite conformance_branch_map_suite = {
	BRANCH_MAP_IFACE, branch_map_cases, lengthof(branch_map_cases)
};

PG_FUNCTION_INFO_V1(test_branch_map_stats);

/*
 * test_branch_map_stats() returns (probes, reads, forgotten, entries)
 */
Datum
test_branch_map_stats(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	BranchMapStats stats;
	Datum		values[4];
	bool		nulls[4] = {0};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	BranchMap->stats(&stats);
	values[0] = Int64GetDatum(stats.probes);
	values[1] = Int64GetDatum(stats.reads);
	values[2] = Int64GetDatum(stats.forgotten);
	values[3] = Int64GetDatum(stats.entries);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

PG_FUNCTION_INFO_V1(test_branch_map_reset);

Datum
test_branch_map_reset(PG_FUNCTION_ARGS)
{
	BranchMap->reset();
	PG_RETURN_VOID();
}
