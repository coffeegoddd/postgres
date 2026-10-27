/*-------------------------------------------------------------------------
 *
 * cowimage_always.c
 *	  Copy-on-write image "always": every copy of a shared page carries
 *	  the page's image.
 *
 * Redo never reads an old block for a copy: the record that gives the
 * page its new block holds the page.  It costs an image for every page's
 * first change after each capture, besides the capture's own image of it.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/cowimage_always.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"

static bool
always_skip_image(const ProllyCowImageCase *c)
{
	return false;
}

const ProllyCowImageRoutine prolly_cowimage_always = {
	.info = {
		.iface = PROLLY_IFACE_COW_IMAGE,
		.impl_id = 1,
		.format_version = 1,
		.name = "always",
		.description = "every copy of a shared page logs the page's image",
	},
	.skip_image = always_skip_image,
};
