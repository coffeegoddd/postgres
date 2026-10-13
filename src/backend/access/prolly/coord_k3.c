/*-------------------------------------------------------------------------
 *
 * coord_k3.c
 *	  K3 implementation of the coordination interface.
 *
 * K3: locks in the page's unhashed section.  Not implemented yet: every
 * callback is NULL, so each call raises "not implemented" (prolly_impl.h).
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/coord_k3.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"

const ProllyCoordRoutine prolly_coord_k3 = {
	.info = {
		.iface = PROLLY_IFACE_COORDINATION,
		.impl_id = 3,
		.format_version = 0,
		.name = "k3",
		.description = "K3: locks in the page's unhashed section (not implemented)",
	},
};
