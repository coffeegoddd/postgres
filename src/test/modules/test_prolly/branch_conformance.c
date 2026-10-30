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
#include "prolly/branch_catalog.h"
#include "prolly/registry.h"
#include "test_prolly.h"
#include "utils/resowner.h"

#define BRANCH_ID_IFACE "branch_ids"

void
register_branch_implementations(void)
{
	ProllyRegisterImplementation(BRANCH_ID_IFACE, "sequence",
								 &SequenceBranchIds);
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
