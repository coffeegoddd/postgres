/*-------------------------------------------------------------------------
 *
 * rootfreeze_stub.c
 *	  Stub implementation of the root freezer interface.
 *
 * Every callback is NULL, so each call raises "not implemented"
 * (prolly_impl.h).  It never persists anything, so it has
 * PROLLY_STUB_IMPL_ID.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/rootfreeze_stub.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"

const ProllyRootFreezerRoutine prolly_rootfreeze_stub = {
	.info = {
		.iface = PROLLY_IFACE_ROOT_FREEZER,
		.impl_id = PROLLY_STUB_IMPL_ID,
		.format_version = 0,
		.name = "stub",
		.description = "stub: every callback raises not implemented",
	},
};
