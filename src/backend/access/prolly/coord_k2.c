/*-------------------------------------------------------------------------
 *
 * coord_k2.c
 *	  K2 implementation of the coordination interface.
 *
 * K2: coordination plane on live nodes, with commit markers.  See
 * specs/04-candidate-designs.md.  Not implemented yet: every callback is
 * NULL, so each call raises "not implemented" (prolly_impl.h).
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/coord_k2.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"

const ProllyCoordRoutine prolly_coord_k2 = {
	.info = {
		.iface = PROLLY_IFACE_COORDINATION,
		.impl_id = 2,
		.format_version = 0,
		.name = "k2",
		.description = "K2: coordination plane on live nodes, with commit markers (not implemented)",
	},
};
