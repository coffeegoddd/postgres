/*-------------------------------------------------------------------------
 *
 * registry.c
 *	  Named implementations of the engine's interfaces.
 *
 * The registry is a list in TopMemoryContext, kept in registration order so
 * that anything listing the implementations of an interface, such as a
 * conformance suite, sees them in an order that does not change from run to
 * run.  It holds a handful of entries per interface, so lookups scan it.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/prolly/registry.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "prolly/registry.h"
#include "utils/memutils.h"

static List *implementations = NIL;

/*
 * Names become SQL identifiers in the functions that report on the
 * registry, so they follow the same length limit.
 */
static void
check_name(const char *what, const char *name)
{
	if (name == NULL || name[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s name must not be empty", what)));
	if (strlen(name) >= NAMEDATALEN)
		ereport(ERROR,
				(errcode(ERRCODE_NAME_TOO_LONG),
				 errmsg("%s name \"%s\" is too long", what, name),
				 errdetail("Names must be shorter than %d bytes.", NAMEDATALEN)));
}

/*
 * Register an implementation of an interface under a name.
 *
 * The routine struct must outlive the process; it is normally a static
 * const struct.  The names are copied.  Registering a second implementation
 * under a name that its interface already has is an error, because a lookup
 * by name could then find either one.
 */
void
ProllyRegisterImplementation(const char *iface, const char *name,
							 const void *routine)
{
	ProllyImplementation *impl;
	MemoryContext oldcxt;

	check_name("interface", iface);
	check_name("implementation", name);
	if (routine == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("implementation \"%s\" of interface \"%s\" has no routine",
						name, iface)));
	if (ProllyLookupImplementation(iface, name) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("implementation \"%s\" of interface \"%s\" is already registered",
						name, iface)));

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	impl = palloc_object(ProllyImplementation);
	impl->iface = pstrdup(iface);
	impl->name = pstrdup(name);
	impl->routine = routine;
	implementations = lappend(implementations, impl);
	MemoryContextSwitchTo(oldcxt);
}

/*
 * Find an implementation by its interface and name, or return NULL if none
 * is registered.
 */
const ProllyImplementation *
ProllyLookupImplementation(const char *iface, const char *name)
{
	foreach_ptr(ProllyImplementation, impl, implementations)
	{
		if (strcmp(impl->iface, iface) == 0 && strcmp(impl->name, name) == 0)
			return impl;
	}
	return NULL;
}

/*
 * List the implementations of an interface, or of every interface if iface
 * is NULL, in registration order.  The list is allocated in the current
 * memory context; its elements belong to the registry.
 */
List *
ProllyListImplementations(const char *iface)
{
	List	   *result = NIL;

	foreach_ptr(ProllyImplementation, impl, implementations)
	{
		if (iface == NULL || strcmp(impl->iface, iface) == 0)
			result = lappend(result, impl);
	}
	return result;
}
