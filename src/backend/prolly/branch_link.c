/*-------------------------------------------------------------------------
 *
 * branch_link.c
 *	  Which catalog branch each branch of the repository is, as rows of
 *	  pg_history.branches.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/prolly/branch_link.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "prolly/branch_catalog.h"
#include "prolly/branch_link.h"
#include "utils/builtins.h"

static void
link_connect(void)
{
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect to SPI");
}

static void
link_finish(void)
{
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "could not finish with SPI");
}

static int32
table_branch_of(const char *name)
{
	Oid			types[1] = {TEXTOID};
	Datum		values[1] = {CStringGetTextDatum(name)};
	int32		branch = 0;

	link_connect();
	if (SPI_execute_with_args("SELECT id FROM pg_history.branches WHERE name OPERATOR(pg_catalog.=) $1",
							  1, types, values, NULL, true, 1) != SPI_OK_SELECT)
		elog(ERROR, "could not read a branch's link");
	if (SPI_processed == 1)
	{
		bool		isnull;

		branch = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
	}
	link_finish();
	return branch;
}

static char *
table_name_of(int32 branch)
{
	Oid			types[1] = {INT4OID};
	Datum		values[1] = {Int32GetDatum(branch)};
	char	   *name = NULL;
	MemoryContext caller = CurrentMemoryContext;

	link_connect();
	if (SPI_execute_with_args("SELECT name FROM pg_history.branches WHERE id OPERATOR(pg_catalog.=) $1",
							  1, types, values, NULL, true, 1) != SPI_OK_SELECT)
		elog(ERROR, "could not read a branch's link");
	if (SPI_processed == 1)
	{
		char	   *found = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);

		name = MemoryContextStrdup(caller, found);
	}
	link_finish();
	return name;
}

static void
table_link(const char *name, int32 branch)
{
	Oid			types[2] = {TEXTOID, INT4OID};
	Datum		values[2] = {CStringGetTextDatum(name), Int32GetDatum(branch)};

	if (name[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a branch's name can't be empty")));
	if (branch <= 0 || !BranchIds->allocated(branch))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("branch %d has not been allocated", branch)));

	/* (one linking either at once waits for the other, and then finds it) */
	link_connect();
	if (SPI_execute_with_args("INSERT INTO pg_history.branches VALUES ($1, $2) ON CONFLICT DO NOTHING",
							  2, types, values, NULL, false, 0) != SPI_OK_INSERT)
		elog(ERROR, "could not write a branch's link");
	if (SPI_processed != 1)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("branch \"%s\" or catalog branch %d is linked already", name, branch)));
	link_finish();
}

static bool
table_unlink(const char *name)
{
	Oid			types[1] = {TEXTOID};
	Datum		values[1] = {CStringGetTextDatum(name)};
	bool		found;

	link_connect();
	if (SPI_execute_with_args("DELETE FROM pg_history.branches WHERE name OPERATOR(pg_catalog.=) $1",
							  1, types, values, NULL, false, 0) != SPI_OK_DELETE)
		elog(ERROR, "could not remove a branch's link");
	found = SPI_processed == 1;
	link_finish();
	return found;
}

const BranchLinkRoutine TableBranchLink = {
	.name = "table",
	.branch_of = table_branch_of,
	.name_of = table_name_of,
	.link = table_link,
	.unlink = table_unlink,
};

const BranchLinkRoutine *BranchLink = &TableBranchLink;
