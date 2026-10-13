/*-------------------------------------------------------------------------
 *
 * engine_d3b.c
 *	  D3b implementation of the engine interface.
 *
 * D3b: location-addressed, in place when unpinned (lazy reference counts).
 * See specs/04-candidate-designs.md.  Not implemented yet: every callback
 * is NULL, so each call raises "not implemented" (prolly_impl.h).
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/engine_d3b.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"

const ProllyEngineRoutine prolly_engine_d3b = {
	.info = {
		.iface = PROLLY_IFACE_ENGINE,
		.impl_id = 4,
		.format_version = 0,
		.name = "d3b",
		.description = "D3b: location-addressed, in place when unpinned (lazy reference counts) (not implemented)",
	},
};
