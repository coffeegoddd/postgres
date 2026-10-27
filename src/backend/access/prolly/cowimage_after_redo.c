/*-------------------------------------------------------------------------
 *
 * cowimage_after_redo.c
 *	  Copy-on-write image "after_redo": a copy of a shared page leaves out
 *	  the page's image when the capture that shared it logged the page
 *	  after the redo pointer, and the page hasn't changed since.
 *
 * The capture's image of the page is then in every replay that reaches
 * the copy, before it, and its redo wrote the old block whole; nothing in
 * WAL writes the block between the two (it is shared, so held, and never
 * changes in place), so redo copies the page from it as the primary did.
 * A page changed since the capture logged it (its LSN another record's),
 * or captured before the redo pointer, keeps its image.  In steady state,
 * with checkpoints rarer than captures, a page is imaged once a cycle, by
 * the capture, instead of twice.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/cowimage_after_redo.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"

static bool
after_redo_skip_image(const ProllyCowImageCase *c)
{
	return !XLogRecPtrIsInvalid(c->shared_lsn) &&
		c->page_lsn == c->shared_lsn &&
		c->shared_since > c->redo;
}

const ProllyCowImageRoutine prolly_cowimage_after_redo = {
	.info = {
		.iface = PROLLY_IFACE_COW_IMAGE,
		.impl_id = 2,
		.format_version = 1,
		.name = "after_redo",
		.description = "a copy of a page captured since the redo pointer, unchanged since, logs no image: redo copies the old block",
	},
	.skip_image = after_redo_skip_image,
};
