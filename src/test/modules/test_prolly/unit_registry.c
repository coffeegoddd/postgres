/*-------------------------------------------------------------------------
 *
 * unit_registry.c
 *	  Unit tests of the registry of interface implementations.
 *
 * The registry lasts as long as the process and has no way to remove an
 * entry, so every test registers under an interface name no other test or
 * run uses.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/test_prolly/unit_registry.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "prolly/registry.h"
#include "test_prolly.h"
#include "utils/memutils.h"

static const int routine_a = 1;
static const int routine_b = 2;

/* An interface name not used before in this process. */
static char *
fresh_iface(void)
{
	static int	counter = 0;

	return psprintf("unit_registry_%d", ++counter);
}

/* The message of the error that a registration raises, or NULL if none. */
static char *
register_error(const char *iface, const char *name, const void *routine)
{
	MemoryContext cxt = CurrentMemoryContext;
	char	   *msg = NULL;

	PG_TRY();
	{
		ProllyRegisterImplementation(iface, name, routine);
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(cxt);
		edata = CopyErrorData();
		FlushErrorState();
		msg = edata->message;
	}
	PG_END_TRY();
	return msg;
}

static void
register_and_look_up(void)
{
	char	   *iface = fresh_iface();
	const ProllyImplementation *impl;

	CHECK(ProllyLookupImplementation(iface, "a") == NULL);
	ProllyRegisterImplementation(iface, "a", &routine_a);

	impl = ProllyLookupImplementation(iface, "a");
	CHECK(impl != NULL);
	CHECK_STR_EQ(impl->iface, iface);
	CHECK_STR_EQ(impl->name, "a");
	CHECK(impl->routine == &routine_a);
	CHECK(ProllyLookupImplementation(iface, "b") == NULL);
}

/*
 * The registry copies the names, so a caller may pass names it builds and
 * frees, and the entries outlive the memory context that registered them.
 */
static void
names_are_copied(void)
{
	char	   *iface = fresh_iface();
	char	   *name = pstrdup("copied");
	const ProllyImplementation *impl;

	ProllyRegisterImplementation(iface, name, &routine_a);
	name[0] = 'X';
	impl = ProllyLookupImplementation(iface, "copied");
	CHECK(impl != NULL);
	CHECK(impl->name != name);
	CHECK(GetMemoryChunkContext((void *) impl->name) == TopMemoryContext);
	CHECK(GetMemoryChunkContext((void *) impl) == TopMemoryContext);
}

static void
lists_in_registration_order(void)
{
	char	   *iface = fresh_iface();
	char	   *other = fresh_iface();
	List	   *impls;

	ProllyRegisterImplementation(iface, "second_name_first", &routine_b);
	ProllyRegisterImplementation(other, "elsewhere", &routine_a);
	ProllyRegisterImplementation(iface, "a_later", &routine_a);

	impls = ProllyListImplementations(iface);
	CHECK_EQ(list_length(impls), 2);
	CHECK_STR_EQ(((ProllyImplementation *) linitial(impls))->name, "second_name_first");
	CHECK_STR_EQ(((ProllyImplementation *) lsecond(impls))->name, "a_later");

	CHECK(ProllyListImplementations(fresh_iface()) == NIL);

	/* every interface's, with NULL */
	impls = ProllyListImplementations(NULL);
	CHECK_LE(3, list_length(impls));
}

/*
 * The same name may be used by different interfaces, but not twice by one,
 * where a lookup by name could find either implementation.
 */
static void
rejects_duplicates(void)
{
	char	   *iface = fresh_iface();
	char	   *other = fresh_iface();
	char	   *msg;

	ProllyRegisterImplementation(iface, "dup", &routine_a);
	ProllyRegisterImplementation(other, "dup", &routine_b);

	msg = register_error(iface, "dup", &routine_b);
	CHECK(msg != NULL);
	CHECK(strstr(msg, "is already registered") != NULL);
	CHECK(ProllyLookupImplementation(iface, "dup")->routine == &routine_a);
	CHECK_EQ(list_length(ProllyListImplementations(iface)), 1);
}

static void
rejects_bad_names_and_routines(void)
{
	char	   *iface = fresh_iface();
	char		longname[NAMEDATALEN + 1];
	char	   *msg;

	memset(longname, 'x', NAMEDATALEN);
	longname[NAMEDATALEN] = '\0';

	msg = register_error(iface, "", &routine_a);
	CHECK(msg != NULL && strstr(msg, "must not be empty") != NULL);
	msg = register_error("", "a", &routine_a);
	CHECK(msg != NULL && strstr(msg, "must not be empty") != NULL);
	msg = register_error(iface, NULL, &routine_a);
	CHECK(msg != NULL && strstr(msg, "must not be empty") != NULL);
	msg = register_error(iface, longname, &routine_a);
	CHECK(msg != NULL && strstr(msg, "is too long") != NULL);
	msg = register_error(iface, "a", NULL);
	CHECK(msg != NULL && strstr(msg, "has no routine") != NULL);

	/* a name one byte shorter fits */
	longname[NAMEDATALEN - 1] = '\0';
	CHECK(register_error(iface, longname, &routine_a) == NULL);

	CHECK_EQ(list_length(ProllyListImplementations(iface)), 1);
}

static const ProllyUnitTest tests[] = {
	{"register_and_look_up", register_and_look_up},
	{"names_are_copied", names_are_copied},
	{"lists_in_registration_order", lists_in_registration_order},
	{"rejects_duplicates", rejects_duplicates},
	{"rejects_bad_names_and_routines", rejects_bad_names_and_routines},
};

const ProllyUnitSuite unit_registry_suite = {"registry", tests, lengthof(tests)};
