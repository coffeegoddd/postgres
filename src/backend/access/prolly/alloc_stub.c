/*-------------------------------------------------------------------------
 *
 * alloc_stub.c
 *	  Stub implementation of the allocator interface.
 *
 * Every callback is NULL, so each call raises "not implemented"
 * (prolly_impl.h).  Configuration selects it until a real implementation
 * exists.  It never persists anything, so it has PROLLY_STUB_IMPL_ID.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/alloc_stub.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"

const ProllyAllocatorRoutine prolly_alloc_stub = {
	.info = {
		.iface = PROLLY_IFACE_ALLOCATOR,
		.impl_id = PROLLY_STUB_IMPL_ID,
		.format_version = 0,
		.name = "stub",
		.description = "stub: every callback raises not implemented",
	},
};
