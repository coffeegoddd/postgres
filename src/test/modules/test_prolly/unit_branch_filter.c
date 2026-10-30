/*-------------------------------------------------------------------------
 *
 * unit_branch_filter.c
 *	  Unit tests of the catalog filter's decisions on rows that name
 *	  objects by class and object id.
 *
 * Some of these catalogs only get rows from features a test cannot easily
 * use (security label providers, extension scripts granting privileges,
 * other databases' dependencies on shared objects), so the tests form
 * such rows themselves, naming a versioned table made on a branch, and ask
 * the filter whether a session on no branch, and one on the table's
 * branch, sees them.  Each test makes its objects in a subtransaction it
 * rolls back.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/test_prolly/unit_branch_filter.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_class.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_description.h"
#include "catalog/pg_init_privs.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_publication_namespace.h"
#include "catalog/pg_publication_rel.h"
#include "catalog/pg_seclabel.h"
#include "catalog/pg_shdepend.h"
#include "catalog/pg_subscription_rel.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "prolly/branch_catalog.h"
#include "prolly/branch_filter.h"
#include "test_prolly.h"
#include "utils/guc.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/resowner.h"

typedef struct Fixture
{
	int32		branch;
	Oid			table;			/* a table in a versioned schema */
	Oid			schema;			/* that schema, on that branch */
} Fixture;

static void
set_branch(int32 branch)
{
	char		buf[32];

	snprintf(buf, sizeof(buf), "%d", branch);
	SetConfigOption("catalog_branch", buf, PGC_USERSET, PGC_S_SESSION);
}

static Oid
query_oid(const char *sql)
{
	Oid			result;
	bool		isnull;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	if (SPI_execute(sql, false, 0) != SPI_OK_SELECT || SPI_processed != 1)
		elog(ERROR, "expected one row from: %s", sql);
	result = DatumGetObjectId(SPI_getbinval(SPI_tuptable->vals[0],
											SPI_tuptable->tupdesc, 1, &isnull));
	SPI_finish();
	return result;
}

static void
with_fixture(void (*fn) (const Fixture *f))
{
	MemoryContext cxt = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;

	BeginInternalSubTransaction(NULL);
	PG_TRY();
	{
		Fixture		f;

		f.branch = BranchIds->allocate();
		set_branch(f.branch);
		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "SPI_connect failed");
		if (SPI_execute("CREATE SCHEMA branch_filter_fixture VERSIONED;"
						"CREATE TABLE branch_filter_fixture.t (a int)",
						false, 0) < 0)
			elog(ERROR, "could not make the fixture");
		SPI_finish();
		CommandCounterIncrement();
		f.table = query_oid("SELECT 'branch_filter_fixture.t'::regclass::oid");
		f.schema = query_oid("SELECT 'branch_filter_fixture'::regnamespace::oid");
		fn(&f);
	}
	PG_FINALLY();
	{
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(cxt);
		CurrentResourceOwner = owner;
	}
	PG_END_TRY();
}

/*
 * Whether a session on no branch, and one on the fixture's branch, see a
 * row of catalog relid whose columns atts are vals and whose other columns
 * are zero or null.
 */
static void
check_row(const Fixture *f, Oid relid, int n, const AttrNumber *atts,
		  const Datum *vals, bool hidden_on_no_branch)
{
	Relation	rel = table_open(relid, AccessShareLock);
	TupleDesc	desc = RelationGetDescr(rel);
	Datum	   *values = palloc0_array(Datum, desc->natts);
	bool	   *nulls = palloc0_array(bool, desc->natts);
	HeapTuple	tup;

	for (int i = 0; i < desc->natts; i++)
		nulls[i] = TupleDescAttr(desc, i)->attlen < 0;
	for (int i = 0; i < n; i++)
	{
		values[atts[i] - 1] = vals[i];
		nulls[atts[i] - 1] = false;
	}
	tup = heap_form_tuple(desc, values, nulls);
	tup->t_tableOid = relid;

	set_branch(0);
	CHECK(CatalogBranchHides(rel, tup) == hidden_on_no_branch);
	set_branch(f->branch);
	CHECK(!CatalogBranchHides(rel, tup));

	table_close(rel, AccessShareLock);
}

static void
check_named_objects(const Fixture *f)
{
	Datum		table = ObjectIdGetDatum(f->table);
	Datum		pg_class_oid = ObjectIdGetDatum(RelationRelationId);
	Datum		shared_table = ObjectIdGetDatum(RelationRelationId);

	/* (objoid, classoid) catalogs: the fixture's table, then a catalog */
	{
		AttrNumber	atts[] = {Anum_pg_description_objoid, Anum_pg_description_classoid};
		Datum		vals[] = {table, pg_class_oid};
		Datum		shared[] = {shared_table, pg_class_oid};

		check_row(f, DescriptionRelationId, 2, atts, vals, true);
		check_row(f, DescriptionRelationId, 2, atts, shared, false);
	}
	{
		AttrNumber	atts[] = {Anum_pg_init_privs_objoid, Anum_pg_init_privs_classoid};
		Datum		vals[] = {table, pg_class_oid};
		Datum		shared[] = {shared_table, pg_class_oid};

		check_row(f, InitPrivsRelationId, 2, atts, vals, true);
		check_row(f, InitPrivsRelationId, 2, atts, shared, false);
	}
	{
		AttrNumber	atts[] = {Anum_pg_seclabel_objoid, Anum_pg_seclabel_classoid};
		Datum		vals[] = {table, pg_class_oid};
		Datum		shared[] = {shared_table, pg_class_oid};

		check_row(f, SecLabelRelationId, 2, atts, vals, true);
		check_row(f, SecLabelRelationId, 2, atts, shared, false);
	}

	/* pg_depend: either side */
	{
		AttrNumber	atts[] = {Anum_pg_depend_classid, Anum_pg_depend_objid,
		Anum_pg_depend_refclassid, Anum_pg_depend_refobjid};
		Datum		depender[] = {pg_class_oid, table, pg_class_oid, shared_table};
		Datum		referenced[] = {pg_class_oid, shared_table, pg_class_oid, table};
		Datum		neither[] = {pg_class_oid, shared_table, pg_class_oid, shared_table};

		check_row(f, DependRelationId, 4, atts, depender, true);
		check_row(f, DependRelationId, 4, atts, referenced, true);
		check_row(f, DependRelationId, 4, atts, neither, false);
	}

	/* pg_shdepend: this database's rows only */
	{
		AttrNumber	atts[] = {Anum_pg_shdepend_dbid, Anum_pg_shdepend_classid,
		Anum_pg_shdepend_objid, Anum_pg_shdepend_refclassid,
		Anum_pg_shdepend_refobjid};
		Datum		here[] = {ObjectIdGetDatum(MyDatabaseId), pg_class_oid, table,
		ObjectIdGetDatum(AuthIdRelationId), ObjectIdGetDatum(BOOTSTRAP_SUPERUSERID)};
		Datum		elsewhere[] = {ObjectIdGetDatum(MyDatabaseId + 1), pg_class_oid, table,
		ObjectIdGetDatum(AuthIdRelationId), ObjectIdGetDatum(BOOTSTRAP_SUPERUSERID)};

		check_row(f, SharedDependRelationId, 5, atts, here, true);
		check_row(f, SharedDependRelationId, 5, atts, elsewhere, false);
	}

	/* publications and subscriptions of the branch's table and schema */
	{
		AttrNumber	atts[] = {Anum_pg_publication_rel_prrelid};
		Datum		vals[] = {table};

		check_row(f, PublicationRelRelationId, 1, atts, vals, true);
	}
	{
		AttrNumber	atts[] = {Anum_pg_publication_namespace_pnnspid};
		Datum		vals[] = {ObjectIdGetDatum(f->schema)};
		Datum		shared[] = {ObjectIdGetDatum(PG_CATALOG_NAMESPACE)};

		check_row(f, PublicationNamespaceRelationId, 1, atts, vals, true);
		check_row(f, PublicationNamespaceRelationId, 1, atts, shared, false);
	}
	{
		AttrNumber	atts[] = {Anum_pg_subscription_rel_srrelid};
		Datum		vals[] = {table};

		check_row(f, SubscriptionRelRelationId, 1, atts, vals, true);
	}
}

static void
rows_naming_objects(void)
{
	with_fixture(check_named_objects);
}

static void
check_filtered_scans(const Fixture *f)
{
	Relation	rel = table_open(DescriptionRelationId, AccessShareLock);
	Snapshot	snap = GetTransactionSnapshot();

	/* queries' snapshots filter; catalog snapshots, and the filter off, don't */
	CHECK(CatalogBranchFilters(rel, snap));
	CHECK(!CatalogBranchFilters(rel, GetCatalogSnapshot(DescriptionRelationId)));
	SetConfigOption("catalog_branch_filter", "off", PGC_SUSET, PGC_S_SESSION);
	CHECK(!CatalogBranchFilters(rel, snap));
	SetConfigOption("catalog_branch_filter", "on", PGC_SUSET, PGC_S_SESSION);
	table_close(rel, AccessShareLock);

	/* nor are other tables filtered */
	rel = table_open(f->table, AccessShareLock);
	CHECK(!CatalogBranchFilters(rel, snap));
	table_close(rel, AccessShareLock);
}

static void
which_scans_filter(void)
{
	with_fixture(check_filtered_scans);
}

static const ProllyUnitTest tests[] = {
	{"rows_naming_objects", rows_naming_objects},
	{"which_scans_filter", which_scans_filter},
};

const ProllyUnitSuite unit_branch_filter_suite = {"branch_filter", tests, lengthof(tests)};
