/*-------------------------------------------------------------------------
 *
 * rowid_stub.c
 *	  Stub implementation of the row identity interface.
 *
 * Every callback is NULL, so each call raises "not implemented"
 * (prolly_impl.h).  Configuration selects it until a real implementation
 * exists.  It never persists anything, so it has PROLLY_STUB_IMPL_ID.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/rowid_stub.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"

const ProllyRowIdRoutine prolly_rowid_stub = {
	.info = {
		.iface = PROLLY_IFACE_ROW_IDENTITY,
		.impl_id = PROLLY_STUB_IMPL_ID,
		.format_version = 0,
		.name = "stub",
		.description = "stub: every callback raises not implemented",
	},
};
