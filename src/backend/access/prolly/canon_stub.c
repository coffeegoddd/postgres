/*-------------------------------------------------------------------------
 *
 * canon_stub.c
 *	  Stub implementation of the canonicalizer interface.
 *
 * Every callback is NULL, so each call raises "not implemented"
 * (prolly_impl.h).  It never persists anything, so it has
 * PROLLY_STUB_IMPL_ID.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/canon_stub.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"

const ProllyCanonRoutine prolly_canon_stub = {
	.info = {
		.iface = PROLLY_IFACE_CANONICALIZER,
		.impl_id = PROLLY_STUB_IMPL_ID,
		.format_version = 0,
		.name = "stub",
		.description = "stub: every callback raises not implemented",
	},
};
