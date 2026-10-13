/*-------------------------------------------------------------------------
 *
 * refs_stub.c
 *	  Stub implementation of the refs and commit graph interface.
 *
 * Every callback is NULL, so each call raises "not implemented"
 * (prolly_impl.h).  Configuration selects it until a real implementation
 * exists.  It never persists anything, so it has PROLLY_STUB_IMPL_ID.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/refs_stub.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"

const ProllyRefsRoutine prolly_refs_stub = {
	.info = {
		.iface = PROLLY_IFACE_REFS,
		.impl_id = PROLLY_STUB_IMPL_ID,
		.format_version = 0,
		.name = "stub",
		.description = "stub: every callback raises not implemented",
	},
};
