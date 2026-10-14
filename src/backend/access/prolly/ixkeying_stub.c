/*-------------------------------------------------------------------------
 *
 * ixkeying_stub.c
 *	  Stub implementation of the index keying interface.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/ixkeying_stub.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"

const ProllyIndexKeyingRoutine prolly_ixkeying_stub = {
	.info = {
		.iface = PROLLY_IFACE_INDEX_KEYING,
		.impl_id = PROLLY_STUB_IMPL_ID,
		.format_version = 0,
		.name = "stub",
		.description = "stub: every callback raises not implemented",
	},
};
