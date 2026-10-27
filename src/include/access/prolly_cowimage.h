/*-------------------------------------------------------------------------
 *
 * prolly_cowimage.h
 *	  Copy-on-write image interface: whether a block-mapped page's copy,
 *	  made as it first changes after its block came to be shared, may be
 *	  logged without the page's image.
 *
 * A page whose block is shared never changes in place: its first change
 * copies it to a new block (bmapprepareforchange()), and the record that
 * gives the page its new block also says what the page holds there.  It
 * can carry the page as an image; or, if every replay that reaches it has
 * just written the old block whole, it can say the page is the old
 * block's, and redo copies that block.  A capture writes each of the
 * relation's own blocks whole in WAL (a PRESERVE image of it) after the
 * point its walk began (the map's shared_since), and stamps each page it
 * logged that hasn't changed since with the capture record's position
 * (the map's shared_lsn).  So a page whose LSN is the capture's, captured
 * after the redo pointer, is still as its image has it, and every replay
 * from a checkpoint before the copy replays that image first.
 *
 * Contract:
 * - skip_image() says whether the copy of a page described by 'c' may be
 *   logged without the page's image.  It must say false unless the page's
 *   LSN is the map's shared_lsn (valid), and the map's shared_since is past
 *   the redo pointer: a copy logged without its image when either fails
 *   would leave redo to copy an old block that a replay may not have
 *   written (torn in a base backup, lost before a restartpoint, or another
 *   relation's page since).  It may say false always (every copy carries
 *   its image).  It is asked twice for an image-less copy: as the copy is
 *   prepared, and again with the redo pointer read after its record is
 *   inserted, a checkpoint having begun meanwhile; a false then logs the
 *   page's image right after.  It never errors, allocates or waits: it is
 *   asked with buffers locked, the second time in a critical section.
 *
 * src/include/access/prolly_cowimage.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_COWIMAGE_H
#define PROLLY_COWIMAGE_H

#include "access/prolly_registry.h"
#include "access/xlogdefs.h"

/* A shared page about to be copied, and its map's last capture. */
typedef struct ProllyCowImageCase
{
	XLogRecPtr	page_lsn;		/* the page's LSN */
	XLogRecPtr	shared_lsn;		/* the capture record's end (invalid: none) */
	XLogRecPtr	shared_since;	/* where the capture's walk began */
	XLogRecPtr	redo;			/* the redo pointer */
} ProllyCowImageCase;

typedef struct ProllyCowImageRoutine
{
	ProllyImplInfo info;

	bool		(*skip_image) (const ProllyCowImageCase *c);
} ProllyCowImageRoutine;

#define PROLLY_COWIMAGE_CALLBACK_NAMES \
	"skip_image"
#define PROLLY_COWIMAGE_FIRST_CALLBACK skip_image
#define PROLLY_COWIMAGE_LAST_CALLBACK skip_image

#endif							/* PROLLY_COWIMAGE_H */
