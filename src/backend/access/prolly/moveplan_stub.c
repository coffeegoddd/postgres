/*-------------------------------------------------------------------------
 *
 * moveplan_stub.c
 *	  Stub implementation of the move planner interface.
 *
 * Every callback is NULL, so each call raises "not implemented"
 * (prolly_impl.h).  It never persists anything, so it has
 * PROLLY_STUB_IMPL_ID.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/moveplan_stub.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"

const ProllyMovePlannerRoutine prolly_moveplan_stub = {
	.info = {
		.iface = PROLLY_IFACE_MOVE_PLANNER,
		.impl_id = PROLLY_STUB_IMPL_ID,
		.format_version = 0,
		.name = "stub",
		.description = "stub: every callback raises not implemented",
	},
};
