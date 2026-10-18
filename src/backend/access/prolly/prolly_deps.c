/*-------------------------------------------------------------------------
 *
 * prolly_deps.c
 *	  Checking what commits need from outside the versioned schemas, and
 *	  creating the extensions a clone needs.
 *
 * The dependencies come from commits' trees of them, read by the
 * canonicalizer (dependencies()): an extension, named "extension <name>"
 * and valued by its version, or another object, valued by its address (a
 * text[] of its type and of its names' and arguments' text[] literals, as
 * pg_identify_object_as_address() gives them).  An object is looked for
 * with pg_get_object_address(), in a subtransaction: it raises an error for
 * one it doesn't find.  Extensions are checked through the catalog views
 * pg_extension, pg_available_extension_versions and
 * pg_extension_update_paths(), as an administrator would.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_deps.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_config.h"
#include "access/prolly_deps.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/varlena.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "varatt.h"

#define EXTENSION_PREFIX	"extension "

/* An extension the commits need, at the versions they recorded. */
typedef struct NeededExtension
{
	char	   *name;			/* unquoted */
	List	   *versions;		/* char *, distinct */
} NeededExtension;

/* An object the commits need. */
typedef struct NeededObject
{
	char	   *name;			/* kind and identity, for messages */
	char	   *address;		/* text[]: type, names, arguments */
} NeededObject;

typedef struct Needs
{
	List	   *extensions;
	List	   *objects;
} Needs;

static NeededExtension *
find_extension(Needs *needs, const char *name)
{
	ListCell   *lc;

	foreach(lc, needs->extensions)
	{
		NeededExtension *e = lfirst(lc);

		if (strcmp(e->name, name) == 0)
			return e;
	}
	return NULL;
}

static void
note_need(const char *name, const char *value, void *arg)
{
	Needs	   *needs = arg;
	ListCell   *lc;

	if (strncmp(name, EXTENSION_PREFIX, strlen(EXTENSION_PREFIX)) == 0)
	{
		char	   *quoted = pstrdup(name + strlen(EXTENSION_PREFIX));
		List	   *parts;
		NeededExtension *e;

		if (!SplitIdentifierString(quoted, '.', &parts) || list_length(parts) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid extension dependency \"%s\"", name)));
		e = find_extension(needs, linitial(parts));
		if (e == NULL)
		{
			e = palloc0(sizeof(NeededExtension));
			e->name = pstrdup(linitial(parts));
			needs->extensions = lappend(needs->extensions, e);
		}
		if (!list_member(e->versions, makeString((char *) value)))
			e->versions = lappend(e->versions, makeString(pstrdup(value)));
		return;
	}
	foreach(lc, needs->objects)
	{
		if (strcmp(((NeededObject *) lfirst(lc))->name, name) == 0)
			return;
	}
	{
		NeededObject *o = palloc(sizeof(NeededObject));

		o->name = pstrdup(name);
		o->address = pstrdup(value);
		needs->objects = lappend(needs->objects, o);
	}
}

/* Run a query with up to two text parameters; the rows, in SPI_tuptable. */
static void
query(const char *sql, const char *p1, const char *p2)
{
	Oid			types[2] = {TEXTOID, TEXTOID};
	Datum		values[2];
	int			n = 0;

	if (p1)
		values[n++] = CStringGetTextDatum(p1);
	if (p2)
		values[n++] = CStringGetTextDatum(p2);
	if (SPI_execute_with_args(sql, n, types, values, NULL, true, 0) != SPI_OK_SELECT)
		elog(ERROR, "SPI_execute_with_args failed: %s", sql);
}

static char *
column(int row, int col)
{
	return SPI_getvalue(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, col);
}

/* The versions of an extension this server can install. */
static List *
available_versions(const char *name)
{
	List	   *out = NIL;

	query("SELECT version FROM pg_catalog.pg_available_extension_versions "
		  "WHERE name = $1", name, NULL);
	for (uint64 i = 0; i < SPI_processed; i++)
		out = lappend(out, makeString(column(i, 1)));
	return out;
}

/* Whether an extension can be updated from one version to another. */
static bool
update_path(const char *name, const char *from, const char *to)
{
	Oid			types[3] = {TEXTOID, TEXTOID, TEXTOID};
	Datum		values[3];

	if (strcmp(from, to) == 0)
		return true;
	values[0] = CStringGetTextDatum(name);
	values[1] = CStringGetTextDatum(from);
	values[2] = CStringGetTextDatum(to);
	if (SPI_execute_with_args("SELECT 1 FROM pg_catalog.pg_extension_update_paths($1) "
							  "WHERE source = $2 AND target = $3 AND path IS NOT NULL",
							  3, types, values, NULL, true, 1) != SPI_OK_SELECT)
		elog(ERROR, "SPI_execute_with_args failed");
	return SPI_processed > 0;
}

/* The version of an extension the database has, or NULL. */
static char *
installed_version(const char *name)
{
	query("SELECT extversion FROM pg_catalog.pg_extension WHERE extname = $1",
		  name, NULL);
	return SPI_processed > 0 ? column(0, 1) : NULL;
}

/*
 * The newest of the versions, by the extension's update paths: the one
 * every other can be updated to.  NULL if none is.
 */
static char *
newest_version(const char *name, List *versions)
{
	ListCell   *a;
	ListCell   *b;

	foreach(a, versions)
	{
		char	   *v = strVal(lfirst(a));
		bool		newest = true;

		foreach(b, versions)
			if (!update_path(name, strVal(lfirst(b)), v))
			{
				newest = false;
				break;
			}
		if (newest)
			return v;
	}
	return NULL;
}

static char *
version_list(List *versions)
{
	StringInfoData s;
	ListCell   *lc;

	initStringInfo(&s);
	foreach(lc, versions)
		appendStringInfo(&s, "%s%s", s.len > 0 ? ", " : "", strVal(lfirst(lc)));
	return s.data;
}

/* Whether the database has an object, found by its address. */
static bool
object_exists(const char *address)
{
	MemoryContext cxt = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	Datum	   *parts;
	int			nparts;
	bool		found;

	/* (a text[] of the type and the names' and arguments' text[]s) */
	deconstruct_array_builtin(DatumGetArrayTypeP(OidInputFunctionCall(F_ARRAY_IN,
																	  (char *) address,
																	  TEXTOID, -1)),
							  TEXTOID, &parts, NULL, &nparts);
	if (nparts != 3)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid object dependency \"%s\"", address)));

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(cxt);
	PG_TRY();
	{
		Oid			types[3] = {TEXTOID, TEXTOID, TEXTOID};
		Datum		values[3] = {parts[0], parts[1], parts[2]};

		if (SPI_execute_with_args("SELECT pg_catalog.pg_get_object_address($1, $2::text[], $3::text[])",
								  3, types, values, NULL, true, 1) != SPI_OK_SELECT)
			elog(ERROR, "SPI_execute_with_args failed");
		found = true;
		ReleaseCurrentSubTransaction();
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(cxt);
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		found = false;
	}
	PG_END_TRY();
	MemoryContextSwitchTo(cxt);
	CurrentResourceOwner = owner;
	return found;
}

static void
collect_trees(List *trees, Needs *needs)
{
	ProllyDatabaseComponents dc;
	ListCell   *lc;

	prolly_database_components(&dc);
	foreach(lc, trees)
		PROLLY_CALL(dc.canonicalizer, dependencies, lfirst(lc), note_need, needs);
}

void
prolly_check_dependencies(List *trees, bool install, const char *what)
{
	Needs		needs = {NIL, NIL};
	StringInfoData missing;
	List	   *to_install = NIL;	/* NeededExtension *, versions: the one */
	ListCell   *lc;
	bool		any_extension = false;

	collect_trees(trees, &needs);
	if (needs.extensions == NIL && needs.objects == NIL)
		return;

	initStringInfo(&missing);
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	foreach(lc, needs.extensions)
	{
		NeededExtension *e = lfirst(lc);
		char	   *newest = newest_version(e->name, e->versions);
		char	   *have = installed_version(e->name);

		if (newest == NULL)
		{
			appendStringInfo(&missing, "\nextension %s: versions %s, which no update path orders",
							 e->name, version_list(e->versions));
			continue;
		}
		if (have != NULL)
		{
			if (!update_path(e->name, newest, have))
			{
				appendStringInfo(&missing, "\nextension %s: version %s, and the database has %s",
								 e->name, newest, have);
				any_extension = true;
			}
			continue;
		}
		if (!list_member(available_versions(e->name), makeString(newest)))
		{
			appendStringInfo(&missing, "\nextension %s: version %s, not available on this server",
							 e->name, newest);
			continue;
		}
		if (!install)
		{
			appendStringInfo(&missing, "\nextension %s: version %s, not installed",
							 e->name, newest);
			any_extension = true;
			continue;
		}
		{
			NeededExtension *add = palloc(sizeof(NeededExtension));

			add->name = e->name;
			add->versions = list_make1(makeString(newest));
			to_install = lappend(to_install, add);
		}
	}
	foreach(lc, needs.objects)
	{
		NeededObject *o = lfirst(lc);

		if (!object_exists(o->address))
			appendStringInfo(&missing, "\n%s: not in this database", o->name);
	}
	if (missing.len > 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("the %s needs what this database lacks", what),
				 errdetail_internal("Needed from outside the versioned schemas:%s",
									missing.data),
				 any_extension ?
				 errhint("Install or update the extensions (CREATE EXTENSION, ALTER EXTENSION ... UPDATE), then try again.") :
				 errhint("Make what is missing, then try again.")));

	foreach(lc, to_install)
	{
		NeededExtension *e = lfirst(lc);
		char	   *sql = psprintf("CREATE EXTENSION %s VERSION %s CASCADE",
								   quote_identifier(e->name),
								   quote_literal_cstr(strVal(linitial(e->versions))));

		if (SPI_execute(sql, false, 0) != SPI_OK_UTILITY)
			elog(ERROR, "SPI_execute failed: %s", sql);
	}
	SPI_finish();
}
