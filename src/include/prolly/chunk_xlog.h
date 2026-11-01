/*-------------------------------------------------------------------------
 *
 * chunk_xlog.h
 *	  The history store's WAL records: the chunk log's (resource manager
 *	  ChunkLog) and the chunk index's (ChunkIndex).
 *
 * Each of the log's records is of one page, and says what changed in it:
 * the meta page's next block, a data page made afresh, sealed, appended
 * to, or bytes of it counted dead.  Its redo makes the same change, by the
 * same code.
 *
 * XLOG_CHUNK_INDEX_CLEANUP_INFO precedes VACUUM's taking freed chunks'
 * entries out of a chunk index: their bytes are then dead, for the log to
 * reuse.  A hot standby's queries whose snapshots might have found one of
 * them before its freeing committed (their xmin no later than the latest
 * freeing transaction's) conflict with it, as with heap pruning's.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/chunk_xlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_CHUNK_XLOG_H
#define PROLLY_CHUNK_XLOG_H

#include "access/transam.h"
#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/block.h"
#include "storage/relfilelocator.h"

/* ChunkLog's: each of block 0, the page changed */
#define XLOG_CHUNK_LOG_NEXT_BLOCK	0x00	/* the meta page's next block */
#define XLOG_CHUNK_LOG_INIT_PAGE	0x10	/* a data page made afresh */
#define XLOG_CHUNK_LOG_SEAL_PAGE	0x20	/* a page part written made full */
#define XLOG_CHUNK_LOG_APPEND		0x30	/* bytes appended to a page */
#define XLOG_CHUNK_LOG_MARK_DEAD	0x40	/* bytes of a page counted dead */

typedef struct xl_chunk_log_next_block
{
	BlockNumber next_block;		/* the first page no region has */
} xl_chunk_log_next_block;

typedef struct xl_chunk_log_init_page
{
	bool		all_dead;		/* written full, every byte dead (a page a
								 * region let go of), or empty (to use) */
} xl_chunk_log_init_page;

typedef struct xl_chunk_log_append
{
	uint32		off;			/* where on the page, after its header (at 0,
								 * the page is made afresh first) */
	uint32		len;			/* the bytes', block 0's data */
} xl_chunk_log_append;

typedef struct xl_chunk_log_mark_dead
{
	uint32		len;			/* the bytes counted dead */
} xl_chunk_log_mark_dead;

extern void chunk_log_redo(XLogReaderState *record);
extern void chunk_log_desc(StringInfo buf, XLogReaderState *record);
extern const char *chunk_log_identify(uint8 info);
extern void chunk_log_mask(char *pagedata, BlockNumber blkno);

/* ChunkIndex's */
#define XLOG_CHUNK_INDEX_CLEANUP_INFO	0x00

typedef struct xl_chunk_index_cleanup_info
{
	RelFileLocator locator;		/* the chunk index */
	TransactionId snapshotConflictHorizon;	/* the latest freeing's */
} xl_chunk_index_cleanup_info;

#define SizeOfChunkIndexCleanupInfo \
	(offsetof(xl_chunk_index_cleanup_info, snapshotConflictHorizon) + \
	 sizeof(TransactionId))

extern void chunk_index_redo(XLogReaderState *record);
extern void chunk_index_desc(StringInfo buf, XLogReaderState *record);
extern const char *chunk_index_identify(uint8 info);

#endif							/* PROLLY_CHUNK_XLOG_H */
