/*-------------------------------------------------------------------------
 *
 * prolly_deps.c
 *	  Checking what commits need from outside the versioned schemas, and
 *	  creating the extensions a clone needs.
 *
 * The dependencies come from commits' trees of them, read by the
 * canonicalizer (dependencies()): an extension, named "extension <name>"
 * and valued by its version and schema, or another object, valued by its
 * address (its type, names and arguments, as
 * pg_identify_object_as_address() gives them), each a text[] literal.  An
 * object is looked for with pg_get_object_address(), in a subtransaction:
 * it raises an error for one it doesn't find.  Extensions are checked
 * through the catalog views pg_extension, pg_available_extensions,
 * pg_available_extension_versions and pg_extension_update_paths(), as an
 * administrator would.
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
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "catalog/versioned.h"
#include "executor/spi.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/varlena.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "varatt.h"

#define EXTENSION_PREFIX	"extension "

/* An extension the commits need, at the versions and schemas they recorded. */
typedef struct NeededExtension
{
	char	   *name;			/* unquoted */
	List	   *versions;		/* String, distinct; none for "any" */
	List	   *schemas;		/* String, distinct */
	bool		versioned;		/* its schema is a versioned one */
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
	List	   *except;			/* "name\nvalue": already needed, not checked */
} Needs;

/* An extension to create. */
typedef struct Install
{
	char	   *name;
	char	   *version;
	char	   *schema;
	bool		versioned;		/* the schema is to be made versioned first */
	bool		started;		/* (what it requires is being created) */
} Install;

/* One update an extension's scripts can make, from one version to another. */
typedef struct UpdatePath
{
	char	   *from;
	char	   *to;
} UpdatePath;

/*
 * The elements of a text[] literal; an error unless there are n, or, with
 * more, up to more past n (how many in *got).
 */
static Datum *
text_array_elements_upto(const char *literal, int n, int more, int *got,
						 const char *what)
{
	Datum	   *elems;
	bool	   *nulls;
	int			nelems;

	deconstruct_array_builtin(DatumGetArrayTypeP(OidInputFunctionCall(F_ARRAY_IN,
																	  (char *) literal,
																	  TEXTOID, -1)),
							  TEXTOID, &elems, &nulls, &nelems);
	if (nelems < n || nelems > n + more)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid %s dependency \"%s\"", what, literal)));
	if (got)
		*got = nelems;
	for (int i = 0; i < nelems; i++)
		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid %s dependency \"%s\"", what, literal)));
	return elems;
}

/* The elements of a text[] literal; an error unless there are n. */
static Datum *
text_array_elements(const char *literal, int n, const char *what)
{
	return text_array_elements_upto(literal, n, 0, NULL, what);
}

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
note_except(const char *name, const char *value, void *arg)
{
	Needs	   *needs = arg;

	needs->except = lappend(needs->except, psprintf("%s\n%s", name, value));
}

static void
note_need(const char *name, const char *value, void *arg)
{
	Needs	   *needs = arg;
	ListCell   *lc;

	foreach(lc, needs->except)
	{
		const char *e = lfirst(lc);
		size_t		len = strlen(name);

		if (strncmp(e, name, len) == 0 && e[len] == '\n' && strcmp(e + len + 1, value) == 0)
			return;
	}

	if (strncmp(name, EXTENSION_PREFIX, strlen(EXTENSION_PREFIX)) == 0)
	{
		char	   *quoted = pstrdup(name + strlen(EXTENSION_PREFIX));
		int			nparts;
		Datum	   *parts = text_array_elements_upto(value, 2, 1, &nparts,
													 "extension");
		char	   *version = TextDatumGetCString(parts[0]);
		char	   *schema = TextDatumGetCString(parts[1]);
		bool		versioned = false;
		List	   *names;
		NeededExtension *e;

		if (!SplitIdentifierString(quoted, '.', &names) || list_length(names) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid extension dependency \"%s\"", name)));
		if (nparts == 3)
		{
			if (strcmp(TextDatumGetCString(parts[2]), "versioned") != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("invalid extension dependency \"%s\"", name)));
			versioned = true;
		}
		e = find_extension(needs, linitial(names));
		if (e == NULL)
		{
			e = palloc0(sizeof(NeededExtension));
			e->name = pstrdup(linitial(names));
			needs->extensions = lappend(needs->extensions, e);
		}
		if (version[0] != '\0' && !list_member(e->versions, makeString(version)))
			e->versions = lappend(e->versions, makeString(version));
		if (!list_member(e->schemas, makeString(schema)))
			e->schemas = lappend(e->schemas, makeString(schema));
		e->versioned = e->versioned || versioned;
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

/* Run a query with text parameters; the rows, in SPI_tuptable. */
static void
query(const char *sql, int nparams, const char **params)
{
	Oid		   *types = palloc(sizeof(Oid) * Max(nparams, 1));
	Datum	   *values = palloc(sizeof(Datum) * Max(nparams, 1));

	for (int i = 0; i < nparams; i++)
	{
		types[i] = TEXTOID;
		values[i] = CStringGetTextDatum(params[i]);
	}
	if (SPI_execute_with_args(sql, nparams, types, values, NULL, true, 0) != SPI_OK_SELECT)
		elog(ERROR, "SPI_execute_with_args failed: %s", sql);
}

static char *
column(int row, int col)
{
	return SPI_getvalue(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, col);
}

/* The versions of an extension this server can install (none: no control file). */
static List *
available_versions(const char *name)
{
	List	   *out = NIL;

	query("SELECT version FROM pg_catalog.pg_available_extension_versions "
		  "WHERE name = $1", 1, &name);
	for (uint64 i = 0; i < SPI_processed; i++)
		out = lappend(out, makeString(column(i, 1)));
	return out;
}

/* The version CREATE EXTENSION makes when it isn't given one. */
static char *
default_version(const char *name)
{
	query("SELECT default_version FROM pg_catalog.pg_available_extensions "
		  "WHERE name = $1", 1, &name);
	return SPI_processed > 0 ? column(0, 1) : NULL;
}

/*
 * The updates an extension's scripts can make (UpdatePath *), read once:
 * the view builds the whole version graph from the control and script
 * files at each call.  Only for an available extension.
 */
static List *
update_paths(const char *name)
{
	List	   *out = NIL;

	query("SELECT source, target FROM pg_catalog.pg_extension_update_paths($1) "
		  "WHERE path IS NOT NULL", 1, &name);
	for (uint64 i = 0; i < SPI_processed; i++)
	{
		UpdatePath *p = palloc(sizeof(UpdatePath));

		p->from = column(i, 1);
		p->to = column(i, 2);
		out = lappend(out, p);
	}
	return out;
}

/* Whether the paths update an extension from one version to another. */
static bool
updates_to(List *paths, const char *from, const char *to)
{
	ListCell   *lc;

	if (strcmp(from, to) == 0)
		return true;
	foreach(lc, paths)
	{
		UpdatePath *p = lfirst(lc);

		if (strcmp(p->from, from) == 0 && strcmp(p->to, to) == 0)
			return true;
	}
	return false;
}

/*
 * The version and schema of an extension the database has; false if it
 * hasn't it.
 */
static bool
installed(const char *name, char **version, char **schema)
{
	query("SELECT e.extversion, n.nspname FROM pg_catalog.pg_extension e "
		  "JOIN pg_catalog.pg_namespace n ON n.oid = e.extnamespace "
		  "WHERE e.extname = $1", 1, &name);
	if (SPI_processed == 0)
		return false;
	*version = column(0, 1);
	*schema = column(0, 2);
	return true;
}

/*
 * The newest of the versions, by the update paths: the one every other can
 * be updated to.  NULL if none is.
 */
static char *
newest_version(List *versions, List *paths)
{
	ListCell   *a;
	ListCell   *b;

	foreach(a, versions)
	{
		char	   *v = strVal(lfirst(a));
		bool		newest = true;

		foreach(b, versions)
			if (!updates_to(paths, strVal(lfirst(b)), v))
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
string_list(List *strings)
{
	StringInfoData s;
	ListCell   *lc;

	initStringInfo(&s);
	foreach(lc, strings)
		appendStringInfo(&s, "%s%s", s.len > 0 ? ", " : "", strVal(lfirst(lc)));
	return s.data;
}

/* Whether all the versions are one: the given one. */
static bool
only_version(List *versions, const char *version)
{
	ListCell   *lc;

	foreach(lc, versions)
		if (strcmp(strVal(lfirst(lc)), version) != 0)
			return false;
	return true;
}

/*
 * Check one extension the commits need; what is wrong goes in missing.
 * The version to create it at, if the database lacks it and can make it;
 * NULL otherwise.
 */
static char *
check_extension(NeededExtension *e, StringInfo missing, bool *any_extension,
				bool install)
{
	char	   *schema = strVal(linitial(e->schemas));
	char	   *have;
	char	   *have_schema;
	List	   *available;
	List	   *paths;
	char	   *newest;

	if (list_length(e->schemas) > 1)
	{
		appendStringInfo(missing, "\nextension %s: in schemas %s, which differ",
						 e->name, string_list(e->schemas));
		return NULL;
	}
	if (installed(e->name, &have, &have_schema))
	{
		if (strcmp(have_schema, schema) != 0)
		{
			appendStringInfo(missing, "\nextension %s: in schema %s, and the database has it in %s",
							 e->name, schema, have_schema);
		}
		if (only_version(e->versions, have))
			return NULL;
	}
	else
		have = NULL;

	/* (the update paths need the control file: without it, nothing to order) */
	available = available_versions(e->name);
	if (available == NIL)
	{
		appendStringInfo(missing, "\nextension %s: %s%s, not available on this server",
						 e->name,
						 e->versions == NIL ? "any version" :
						 list_length(e->versions) > 1 ? "versions " : "version ",
						 e->versions ? string_list(e->versions) : "");
		if (have)
			*any_extension = true;
		return NULL;
	}
	paths = update_paths(e->name);
	if (e->versions == NIL)
		newest = have ? have : default_version(e->name);
	else if ((newest = newest_version(e->versions, paths)) == NULL)
	{
		appendStringInfo(missing, "\nextension %s: versions %s, which no update path orders",
						 e->name, string_list(e->versions));
		return NULL;
	}

	if (have)
	{
		if (!updates_to(paths, newest, have))
		{
			appendStringInfo(missing, "\nextension %s: version %s, and the database has %s",
							 e->name, newest, have);
			*any_extension = true;
		}
		return NULL;
	}
	if (!list_member(available, makeString(newest)))
	{
		appendStringInfo(missing, "\nextension %s: version %s, not available on this server",
						 e->name, newest);
		return NULL;
	}
	if (e->versioned && install)
		return newest;			/* (the schema made, versioned, first) */
	if (!OidIsValid(get_namespace_oid(schema, true)))
	{
		appendStringInfo(missing, "\nextension %s: schema %s, not in this database",
						 e->name, schema);
		return NULL;
	}
	if (e->versioned && !namespace_is_versioned(get_namespace_oid(schema, false)))
	{
		appendStringInfo(missing, "\nextension %s: schema %s, which isn't versioned in this database",
						 e->name, schema);
		return NULL;
	}
	return newest;
}

static Install *
find_install(List *installs, const char *name)
{
	ListCell   *lc;

	foreach(lc, installs)
	{
		Install    *i = lfirst(lc);

		if (strcmp(i->name, name) == 0)
			return i;
	}
	return NULL;
}

/*
 * Create an extension, after those it requires that are to be created too:
 * CASCADE would make them at their default versions and in this one's
 * schema.  One made already (by another's CASCADE) must be as needed.
 */
static void
create_extension(List *installs, Install *e)
{
	const char *params[2] = {e->name, e->version};
	List	   *requires = NIL;
	ListCell   *lc;
	char	   *have;
	char	   *have_schema;

	if (e->started)
		return;
	e->started = true;
	query("SELECT r FROM pg_catalog.pg_available_extension_versions v, "
		  "unnest(v.requires) r WHERE v.name = $1 AND v.version = $2",
		  2, params);
	for (uint64 i = 0; i < SPI_processed; i++)
		requires = lappend(requires, column(i, 1));
	foreach(lc, requires)
	{
		Install    *r = find_install(installs, lfirst(lc));

		if (r)
			create_extension(installs, r);
	}

	if (installed(e->name, &have, &have_schema))
	{
		if (strcmp(have, e->version) != 0 || strcmp(have_schema, e->schema) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("extension \"%s\" was made at version %s in schema %s, not %s in %s",
							e->name, have, have_schema, e->version, e->schema)));
		return;
	}
	{
		char	   *sql = psprintf("CREATE EXTENSION %s VERSION %s SCHEMA %s CASCADE",
								   quote_identifier(e->name),
								   quote_literal_cstr(e->version),
								   quote_identifier(e->schema));

		if (SPI_execute(sql, false, 0) != SPI_OK_UTILITY)
			elog(ERROR, "SPI_execute failed: %s", sql);
	}
}

/* Whether an error says what pg_get_object_address() looked for isn't there. */
static bool
not_found(int sqlerrcode)
{
	return sqlerrcode == ERRCODE_UNDEFINED_OBJECT ||
		sqlerrcode == ERRCODE_UNDEFINED_FUNCTION ||
		sqlerrcode == ERRCODE_UNDEFINED_TABLE ||
		sqlerrcode == ERRCODE_UNDEFINED_COLUMN ||
		sqlerrcode == ERRCODE_INVALID_SCHEMA_NAME;
}

/*
 * Whether the database has an object, found by its address.  Only "not
 * found" means it hasn't: any other error (a cancel, a bad address) is
 * raised.
 */
static bool
object_exists(const char *address)
{
	MemoryContext cxt = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	Datum	   *parts = text_array_elements(address, 3, "object");
	bool		found;

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(cxt);
	PG_TRY();
	{
		Oid			types[3] = {TEXTOID, TEXTOID, TEXTOID};

		if (SPI_execute_with_args("SELECT pg_catalog.pg_get_object_address($1, $2::text[], $3::text[])",
								  3, types, parts, NULL, true, 1) != SPI_OK_SELECT)
			elog(ERROR, "SPI_execute_with_args failed");
		found = true;
		ReleaseCurrentSubTransaction();
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(cxt);
		edata = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(cxt);
		CurrentResourceOwner = owner;
		if (!not_found(edata->sqlerrcode))
			ReThrowError(edata);
		FreeErrorData(edata);
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

/* Check the needs, as prolly_check_dependencies() says. */
static void
check_needs(Needs *needs, bool install, const char *what)
{
	StringInfoData missing;
	List	   *installs = NIL;
	ListCell   *lc;
	bool		any_extension = false;

	if (needs->extensions == NIL && needs->objects == NIL)
		return;

	initStringInfo(&missing);
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	foreach(lc, needs->extensions)
	{
		NeededExtension *e = lfirst(lc);
		char	   *version = check_extension(e, &missing, &any_extension,
											  install);

		if (version == NULL)
			continue;
		if (!install)
		{
			appendStringInfo(&missing, "\nextension %s: version %s, not installed",
							 e->name, version);
			any_extension = true;
			continue;
		}
		{
			Install    *add = palloc0(sizeof(Install));

			add->name = e->name;
			add->version = version;
			add->schema = strVal(linitial(e->schemas));
			add->versioned = e->versioned;
			installs = lappend(installs, add);
		}
	}
	foreach(lc, needs->objects)
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

	/*
	 * A versioned schema an extension is to be made in is made first, as
	 * the move that makes the rest would make it (in place of an ordinary
	 * one of its name, such as a new database's public).
	 */
	foreach(lc, installs)
	{
		Install    *i = lfirst(lc);
		Oid			nsp = get_namespace_oid(i->schema, true);

		if (i->versioned && (!OidIsValid(nsp) || !namespace_is_versioned(nsp)))
		{
			ProllyDatabaseComponents dc;

			prolly_database_components(&dc);
			if (dc.canonicalizer->make_versioned_schema == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("the %s needs versioned schema %s for extension %s, which this canonicalizer can't make first",
								what, i->schema, i->name)));
			PROLLY_CALL(dc.canonicalizer, make_versioned_schema,
						quote_identifier(i->schema));
		}
	}
	foreach(lc, installs)
		create_extension(installs, lfirst(lc));
	SPI_finish();
}

void
prolly_check_dependencies(List *trees, bool install, const char *what)
{
	Needs		needs = {NIL, NIL, NIL};

	collect_trees(trees, &needs);
	check_needs(&needs, install, what);
}

void
prolly_check_commit_dependencies(const ProllyCommitInfo *commit,
								 const ProllyCommitInfo *since, const char *what)
{
	ProllyDatabaseComponents dc;
	Needs		needs = {NIL, NIL, NIL};

	if (!commit->has_dependencies)
		return;
	prolly_database_components(&dc);
	if (since && since->has_dependencies)
		PROLLY_CALL(dc.canonicalizer, dependencies, &since->dependencies,
					note_except, &needs);
	PROLLY_CALL(dc.canonicalizer, dependencies, &commit->dependencies,
				note_need, &needs);
	check_needs(&needs, false, what);
}
