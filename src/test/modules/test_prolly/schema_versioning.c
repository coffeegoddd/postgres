/*--------------------------------------------------------------------------
 *
 * schema_versioning.c
 *		A test-only setting that makes every new schema versioned.
 *
 * With test_prolly.version_new_schemas on, every schema made is made
 * VERSIONED, as if the statement had said so: CREATE SCHEMA, however it
 * runs (typed, from a function, through EXECUTE, from an extension's
 * script), and the schema CREATE EXTENSION makes for an extension whose
 * control file names one that doesn't exist yet.  It lets a suite written
 * for stock Postgres, which makes schemas of its own, run with all its
 * tables versioned without rewriting it.
 *
 * An extension is shared by every branch, so CREATE EXTENSION refuses a
 * versioned schema some branch doesn't have: with the setting on, an
 * extension whose schema is made for it installs only while the database
 * has no other branch (as when a suite starts).
 *
 * It is a ProcessUtility hook, so it takes effect in backends that have
 * loaded test_prolly: from shared_preload_libraries for a whole server, or
 * after LOAD in one session.  It is a testing aid only: Postgres has no
 * versioned-by-default schema setting, and nothing outside tests should
 * rely on this one.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_prolly/schema_versioning.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "commands/schemacmds.h"
#include "executor/spi.h"
#include "nodes/makefuncs.h"
#include "parser/parse_node.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "test_prolly.h"

static bool version_new_schemas = false;
static ProcessUtility_hook_type prev_ProcessUtility = NULL;

/*
 * The schema the control file of the extension stmt installs names, if
 * any: the one for the version asked for, or the default version.
 */
static char *
extension_control_schema(CreateExtensionStmt *stmt)
{
	const char *version = NULL;
	char	   *schema = NULL;
	Oid			argtypes[2] = {TEXTOID, TEXTOID};
	Datum		values[2];
	char		nulls[2] = {' ', ' '};
	MemoryContext cxt = CurrentMemoryContext;
	ListCell   *lc;

	foreach(lc, stmt->options)
	{
		DefElem    *d = lfirst_node(DefElem, lc);

		if (strcmp(d->defname, "new_version") == 0)
			version = defGetString(d);
	}
	values[0] = CStringGetTextDatum(stmt->extname);
	if (version)
		values[1] = CStringGetTextDatum(version);
	else
		nulls[1] = 'n';

	SPI_connect();
	if (SPI_execute_with_args("SELECT v.schema FROM pg_catalog.pg_available_extension_versions v"
							  " JOIN pg_catalog.pg_available_extensions e USING (name)"
							  " WHERE v.name = $1"
							  " AND v.version = coalesce($2, e.default_version)",
							  2, argtypes, values, nulls, true, 1) != SPI_OK_SELECT)
		elog(ERROR, "could not look up extension \"%s\"", stmt->extname);
	if (SPI_processed == 1)
	{
		char	   *s = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);

		if (s)
			schema = MemoryContextStrdup(cxt, s);
	}
	SPI_finish();
	return schema;
}

/*
 * CREATE EXTENSION makes the schema its control file names, if missing, as
 * the current user and outside the extension (so DROP EXTENSION leaves
 * it).  It is made here first, also as the current user and outside the
 * extension, but versioned.  Unlike CREATE EXTENSION's, it is made before
 * the command starts, so event triggers aren't told of it: ddl_command_end
 * reports only the extension.
 */
static void
make_extension_schema(CreateExtensionStmt *stmt)
{
	char	   *schema;
	CreateSchemaStmt *csstmt;
	ParseState *pstate;

	if (OidIsValid(get_extension_oid(stmt->extname, true)))
		return;
	schema = extension_control_schema(stmt);
	if (schema == NULL || OidIsValid(get_namespace_oid(schema, true)))
		return;

	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = "(generated CREATE SCHEMA command)";
	csstmt = makeNode(CreateSchemaStmt);
	csstmt->schemaname = schema;
	csstmt->versioned = true;
	CreateSchemaCommand(pstate, csstmt, -1, -1);
}

static void
versioning_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
						  bool readOnlyTree, ProcessUtilityContext context,
						  ParamListInfo params, QueryEnvironment *queryEnv,
						  DestReceiver *dest, QueryCompletion *qc)
{
	if (version_new_schemas)
	{
		Node	   *parsetree = pstmt->utilityStmt;

		if (IsA(parsetree, CreateSchemaStmt) &&
			!((CreateSchemaStmt *) parsetree)->versioned)
		{
			/* (a cached plan's tree is shared: change a copy) */
			if (readOnlyTree)
			{
				pstmt = copyObject(pstmt);
				readOnlyTree = false;
			}
			((CreateSchemaStmt *) pstmt->utilityStmt)->versioned = true;
		}
		else if (IsA(parsetree, CreateExtensionStmt))
			make_extension_schema((CreateExtensionStmt *) parsetree);
	}

	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
}

void
schema_versioning_init(void)
{
	DefineCustomBoolVariable("test_prolly.version_new_schemas",
							 "Makes every new schema versioned (testing only).",
							 NULL,
							 &version_new_schemas,
							 false,
							 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);
	MarkGUCPrefixReserved("test_prolly");

	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = versioning_ProcessUtility;
}
