/*-------------------------------------------------------------------------
 *
 * engine_d1.c
 *	  D1 implementation of the engine interface.
 *
 * D1: immutable copy-on-write: hash-addressed nodes, full-chunk WAL.  Not
 * implemented yet: every callback is NULL, so each call raises "not
 * implemented" (prolly_impl.h).
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/engine_d1.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"

const ProllyEngineRoutine prolly_engine_d1 = {
	.info = {
		.iface = PROLLY_IFACE_ENGINE,
		.impl_id = 1,
		.format_version = 0,
		.name = "d1",
		.description = "D1: immutable copy-on-write: hash-addressed nodes, full-chunk WAL (not implemented)",
	},
};
