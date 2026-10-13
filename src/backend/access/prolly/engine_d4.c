/*-------------------------------------------------------------------------
 *
 * engine_d4.c
 *	  D4 implementation of the engine interface.
 *
 * D4: location-addressed, row versions in the page.  See
 * specs/04-candidate-designs.md.  Not implemented yet: every callback is
 * NULL, so each call raises "not implemented" (prolly_impl.h).
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/engine_d4.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"

const ProllyEngineRoutine prolly_engine_d4 = {
	.info = {
		.iface = PROLLY_IFACE_ENGINE,
		.impl_id = 5,
		.format_version = 0,
		.name = "d4",
		.description = "D4: location-addressed, row versions in the page (not implemented)",
	},
};
