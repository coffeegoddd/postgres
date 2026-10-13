/*-------------------------------------------------------------------------
 *
 * coord_k1.c
 *	  K1 implementation of the coordination interface.
 *
 * K1: separate lock table on the Postgres lock manager.  Not implemented
 * yet: every callback is NULL, so each call raises "not implemented"
 * (prolly_impl.h).
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/coord_k1.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"

const ProllyCoordRoutine prolly_coord_k1 = {
	.info = {
		.iface = PROLLY_IFACE_COORDINATION,
		.impl_id = 1,
		.format_version = 0,
		.name = "k1",
		.description = "K1: separate lock table on the Postgres lock manager (not implemented)",
	},
};
