/*-------------------------------------------------------------------------
 *
 * vc_refs.c
 *	  The implementations of a repository's references, registered
 *	  (prolly/engine/vc_refs.h).
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/engine/vc_refs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "prolly/engine/vc_refs.h"
#include "prolly/registry.h"

static void
register_refs(void)
{
	static bool registered = false;

	if (registered)
		return;
	ProllyRegisterImplementation(VC_REFS_IFACE, "pointers", &PointersVcRefs);
	ProllyRegisterImplementation(VC_REFS_IFACE, "memory", &MemoryVcRefs);
	registered = true;
}

VcRefs *
VcRefsOpen(const char *name, ChunkStore *cs)
{
	const ProllyImplementation *impl;

	register_refs();
	impl = ProllyLookupImplementation(VC_REFS_IFACE, name);
	if (impl == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("there are no references \"%s\"", name)));
	return ((const VcRefsRoutine *) impl->routine)->open(cs);
}
