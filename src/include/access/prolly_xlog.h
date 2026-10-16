/*-------------------------------------------------------------------------
 *
 * prolly_xlog.h
 *	  WAL resource managers for prolly storage.
 *
 * Two resource managers carry prolly records:
 *
 *	Prolly		records written by a log implementation (prolly_log.h)
 *	ProllyStore	records written by a node store implementation
 *				(prolly_nodestore.h) for its own structures, such as a
 *				hash -> block map
 *	ProllyCommit	records written by a commit pipeline implementation
 *				(prolly_commit.h): new branch heads
 *	ProllyChunks	records written by a chunk store implementation
 *				(prolly_chunkstore.h) for its files
 *
 * Every record's main data starts with xl_prolly_header, naming the
 * implementation (by impl_id within the interface) that wrote it; redo is
 * dispatched to that implementation through the registry.  The high four
 * bits of the record's info byte are the implementation's own record type.
 *
 * desc and identify here are generic, because they also run in pg_waldump,
 * which has no registry.
 *
 * This header is included by frontend code (pg_waldump).
 *
 * src/include/access/prolly_xlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_XLOG_H
#define PROLLY_XLOG_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/block.h"

/* The implementation's record type, in the info byte. */
#define PROLLY_XLOG_TYPE_MASK	0xF0

typedef struct xl_prolly_header
{
	uint16		impl_id;		/* within the rmgr's interface */
} xl_prolly_header;

#define SizeOfProllyHeader	(offsetof(xl_prolly_header, impl_id) + sizeof(uint16))

/* The Prolly rmgr (log interface) */
extern void prolly_redo(XLogReaderState *record);
extern void prolly_desc(StringInfo buf, XLogReaderState *record);
extern const char *prolly_identify(uint8 info);
extern void prolly_mask(char *pagedata, BlockNumber blkno);

/* The ProllyStore rmgr (node store interface) */
extern void prolly_store_redo(XLogReaderState *record);
extern void prolly_store_desc(StringInfo buf, XLogReaderState *record);
extern const char *prolly_store_identify(uint8 info);

/* The ProllyCommit rmgr (commit pipeline interface) */
extern void prolly_commit_redo(XLogReaderState *record);
extern void prolly_commit_desc(StringInfo buf, XLogReaderState *record);
extern const char *prolly_commit_identify(uint8 info);

/* The ProllyChunks rmgr (chunk store interface) */
extern void prolly_chunks_redo(XLogReaderState *record);
extern void prolly_chunks_desc(StringInfo buf, XLogReaderState *record);
extern const char *prolly_chunks_identify(uint8 info);

#endif							/* PROLLY_XLOG_H */
