/*-------------------------------------------------------------------------
 *
 * engine_d2.c
 *	  D2 implementation of the engine interface.
 *
 * D2: copy-on-write with logical WAL and deferred hashing.  Not
 * implemented yet: every callback is NULL, so each call raises "not
 * implemented" (prolly_impl.h).
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/engine_d2.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"

const ProllyEngineRoutine prolly_engine_d2 = {
	.info = {
		.iface = PROLLY_IFACE_ENGINE,
		.impl_id = 2,
		.format_version = 0,
		.name = "d2",
		.description = "D2: copy-on-write with logical WAL and deferred hashing (not implemented)",
	},
};
