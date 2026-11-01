/*-------------------------------------------------------------------------
 *
 * chunk_xlog.h
 *	  The history store's WAL records (resource manager ChunkStore).
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
#include "storage/relfilelocator.h"

#define XLOG_CHUNK_INDEX_CLEANUP_INFO	0x00

typedef struct xl_chunk_index_cleanup_info
{
	RelFileLocator locator;		/* the chunk index */
	TransactionId snapshotConflictHorizon;	/* the latest freeing's */
} xl_chunk_index_cleanup_info;

#define SizeOfChunkIndexCleanupInfo \
	(offsetof(xl_chunk_index_cleanup_info, snapshotConflictHorizon) + \
	 sizeof(TransactionId))

extern void chunk_redo(XLogReaderState *record);
extern void chunk_desc(StringInfo buf, XLogReaderState *record);
extern const char *chunk_identify(uint8 info);

#endif							/* PROLLY_CHUNK_XLOG_H */
