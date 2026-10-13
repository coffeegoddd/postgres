/*-------------------------------------------------------------------------
 *
 * engine_d3a.c
 *	  D3a implementation of the engine interface.
 *
 * D3a: location-addressed, in place when unpinned (birth epoch).  See
 * specs/04-candidate-designs.md.  Not implemented yet: every callback is
 * NULL, so each call raises "not implemented" (prolly_impl.h).
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/engine_d3a.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"

const ProllyEngineRoutine prolly_engine_d3a = {
	.info = {
		.iface = PROLLY_IFACE_ENGINE,
		.impl_id = 3,
		.format_version = 0,
		.name = "d3a",
		.description = "D3a: location-addressed, in place when unpinned (birth epoch) (not implemented)",
	},
};
