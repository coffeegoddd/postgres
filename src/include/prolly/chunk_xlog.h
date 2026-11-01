/*-------------------------------------------------------------------------
 *
 * chunk_xlog.h
 *	  The history store's WAL records: the chunk log's (resource manager
 *	  ChunkLog) and the chunk index's (ChunkIndex).
 *
 * Each of the log's records is of one page, and says what changed in it:
 * the meta page's next block, a data page made afresh, sealed, appended
 * to, or bytes of it counted dead, or a map page's bits of open pages.
 * Its redo makes the same change, by the same code.
 *
 * The index's records, too, say what changed in their pages, and are
 * replayed by the code that made the change: an entry added to a page (or
 * with a page of its own, at a chain's end); a change of a chunk's counts
 * changed again, or added into its chunk's entry; a chunk freed; a page's
 * entries kept in part (a purge, a split's old bucket, a removal), or one
 * taken out; a page made with its contents (a split's new pages, a
 * directory page); a split made public; the index bound to its log; and
 * VACUUM's fate of each entry of a page.  An entry is moved whole, flags
 * and all, as the standby has it, whose hints are its own.
 *
 * VACUUM's record of a page that takes out freed chunks' entries (their
 * bytes then dead, for the log to reuse) has the latest freeing
 * transaction's id: a hot standby's queries whose snapshots might have
 * found one of them before its freeing committed (their xmin no later than
 * it) conflict with it, as with heap pruning's.
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
#define XLOG_CHUNK_LOG_BIND_INDEX	0x50	/* the meta page's chunk index */
#define XLOG_CHUNK_LOG_MAP			0x60	/* a map page's bits */

typedef struct xl_chunk_log_next_block
{
	BlockNumber next_block;		/* the first page no region has */
	uint32		generation;		/* the log's, one more at each tail cut */
	BlockNumber cut_from;		/* the last tail cut's first page cut */
} xl_chunk_log_next_block;

typedef struct xl_chunk_log_init_page
{
	bool		all_dead;		/* written full, every byte dead (a page a
								 * region let go of), or empty (to use) */
} xl_chunk_log_init_page;

typedef struct xl_chunk_log_append
{
	uint32		off;			/* where on the page, after its header (past
								 * where it is written: the bytes between
								 * reserved, never written) */
	uint32		len;			/* the bytes', block 0's data */
	uint32		init;			/* the page made afresh first */
} xl_chunk_log_append;

typedef struct xl_chunk_log_seal
{
	uint32		off;			/* where the region's reserved bytes end on
								 * the page: dead from there */
	uint32		init;			/* the page made afresh first */
} xl_chunk_log_seal;

typedef struct xl_chunk_log_mark_dead
{
	uint32		off;			/* where on the page */
	uint32		len;			/* the bytes counted dead (written, or
								 * reserved and never written: the page
								 * then written to their end) */
	uint32		init;			/* the page made afresh first */
} xl_chunk_log_mark_dead;

typedef struct xl_chunk_log_map
{
	uint32		first;			/* the first bit */
	uint32		n;				/* the bits from it */
	uint16		set;			/* set (pages open), or cleared */
	uint16		init;			/* the page made afresh first */
} xl_chunk_log_map;

typedef struct xl_chunk_log_bind_index
{
	Oid			index;			/* the one naming the log's bytes */
} xl_chunk_log_bind_index;

extern void chunk_log_redo(XLogReaderState *record);
extern void chunk_log_desc(StringInfo buf, XLogReaderState *record);
extern const char *chunk_log_identify(uint8 info);
extern void chunk_log_mask(char *pagedata, BlockNumber blkno);

/*
 * ChunkIndex's.  (Their fields are laid out with no padding, every byte
 * of a record set.)
 */
#define XLOG_CHUNK_INDEX_VACUUM		0x00	/* 0: the page; VACUUM's fates */
#define XLOG_CHUNK_INDEX_ADD_ENTRY	0x10	/* 0: the page; the entry */
#define XLOG_CHUNK_INDEX_ADD_PAGE	0x20	/* 0: meta, 1: the chain's last,
											 * 2: the new page */
#define XLOG_CHUNK_INDEX_SET_COUNTS	0x30	/* 0: the page */
#define XLOG_CHUNK_INDEX_SET_XMAX	0x40	/* 0: the page */
#define XLOG_CHUNK_INDEX_KEEP		0x50	/* 0: the page; the slots kept */
#define XLOG_CHUNK_INDEX_REMOVE_SLOT 0x60	/* 0: the page */
#define XLOG_CHUNK_INDEX_FOLD		0x70	/* 0: the chunk's page, 1: the
											 * change's (if another) */
#define XLOG_CHUNK_INDEX_INIT_PAGE	0x80	/* 0: the page; its contents */
#define XLOG_CHUNK_INDEX_SPLIT		0x90	/* 0: meta, 1: the old bucket's
											 * first page, 2: the directory
											 * page (if not new) */
#define XLOG_CHUNK_INDEX_BIND_LOG	0xA0	/* 0: meta */

typedef struct xl_chunk_index_vacuum
{
	TransactionId snapshotConflictHorizon;	/* the latest freeing's, or 0 */
	uint16		nentries;		/* then a fate per entry, block 0's data */
	uint16		unused;
} xl_chunk_index_vacuum;

typedef struct xl_chunk_index_slot
{
	uint32		refs;			/* SET_COUNTS: the counts after */
	uint32		roots;
	TransactionId xmax;			/* SET_XMAX: the freeing transaction */
	uint16		slot;			/* SET_COUNTS, SET_XMAX, REMOVE_SLOT */
	uint16		unused;
} xl_chunk_index_slot;

typedef struct xl_chunk_index_add_page
{
	uint32		bucket;			/* the new page's (then the entry, its data) */
} xl_chunk_index_add_page;

typedef struct xl_chunk_index_keep
{
	uint16		nkept;			/* then the slots kept, in order, its data */
} xl_chunk_index_keep;

typedef struct xl_chunk_index_fold
{
	uint32		refs;			/* the chunk's counts after */
	uint32		roots;
	uint16		chunk_slot;		/* in block 0 */
	uint16		change_slot;	/* in block 1, or in block 0 if same_page */
	uint8		same_page;
	uint8		chunk_on_bucket_page;	/* block 0 is its bucket's first page */
	uint16		unused;
} xl_chunk_index_fold;

typedef struct xl_chunk_index_init_page
{
	uint32		bucket;
	uint32		split_at;
	BlockNumber next;
	uint32		len;			/* its contents' (block 0's data) */
	uint16		kind;			/* INDEX_PAGE_* */
	uint16		unused;
} xl_chunk_index_init_page;

typedef struct xl_chunk_index_split
{
	uint32		new_bucket;
	uint32		lowmask;
	uint32		highmask;
	BlockNumber first;			/* the new bucket's first page */
	BlockNumber next_block;		/* the first page not in use, after */
	BlockNumber dir_block;		/* the directory page of its slot */
	uint32		new_dir;		/* which is new, made before */
} xl_chunk_index_split;

typedef struct xl_chunk_index_bind_log
{
	uint64		stamp;
	Oid			log;
	uint32		unused;
} xl_chunk_index_bind_log;

extern void chunk_index_redo(XLogReaderState *record);
extern void chunk_index_desc(StringInfo buf, XLogReaderState *record);
extern const char *chunk_index_identify(uint8 info);
extern void chunk_index_mask(char *pagedata, BlockNumber blkno);

#endif							/* PROLLY_CHUNK_XLOG_H */
