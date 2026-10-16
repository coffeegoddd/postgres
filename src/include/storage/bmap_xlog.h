/*-------------------------------------------------------------------------
 *
 * bmap_xlog.h
 *	  WAL records of the block-mapped storage manager.
 *
 * Every change to a block-mapped relation's map and to its database's
 * allocator is logged, so both are the same on a primary and its standbys
 * and are restored after a crash.  The pages are registered buffers; what
 * changes on each is described by its block data, in one of the delta
 * formats below.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/bmap_xlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BMAP_XLOG_H
#define BMAP_XLOG_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/block.h"
#include "storage/relfilelocator.h"

/*
 * Record types.
 *
 * XLOG_BMAP_INIT		a database's allocator begins: block 0 is its metapage
 * XLOG_BMAP_CREATE		a relation's map: block 0 is its header
 * XLOG_BMAP_EXTEND		blocks are allocated: block 0 is the map's header,
 *						block 1 the map page, block 2 the allocator's metapage,
 *						then bitmap pages
 * XLOG_BMAP_TRUNCATE	the relation's length: block 0 is the map's header,
 *						then, if the relation was folded, its changes list's
 *						header and page
 * XLOG_BMAP_FREE		blocks are freed: block 0 is the allocator's metapage,
 *						then bitmap pages
 * XLOG_BMAP_SHARE		part of a map copied into another relation's, sharing
 *						its blocks: block 0 is the copy's map page, then the
 *						reference-count pages of the blocks
 * XLOG_BMAP_SHARED		a map's sharing ends: block 0 is the source's header
 *						(its epoch raised), block 1 the copy's (its length)
 * XLOG_BMAP_UNREF		references dropped: reference-count pages, after a
 *						map page whose entries change (BMAP_UNREF_MAP)
 * XLOG_BMAP_REMAP		a shared page is copied before it changes: block 0 is
 *						the map page, then the allocator's metapage and bitmap
 *						pages, the old block's reference-count page if its
 *						count changes, the changes list's header and page if
 *						the old block is listed (BMAP_REMAP_LISTED), and last
 *						the page itself, as an image
 * XLOG_BMAP_FOLD		a fold begins: block 0 is the map's header (its epoch
 *						raised), block 1 the changes list's header
 * XLOG_BMAP_CHANGES	the changes list rewritten: its pages, then its header
 * XLOG_BMAP_SHARE_CHANGES	a changes list copied into another relation's:
 *						its pages or its header, then the reference-count
 *						pages of the blocks it holds
 */
#define XLOG_BMAP_INIT			0x00
#define XLOG_BMAP_CREATE		0x10
#define XLOG_BMAP_EXTEND		0x30
#define XLOG_BMAP_TRUNCATE		0x40
#define XLOG_BMAP_FREE			0x50
#define XLOG_BMAP_SHARE			0x60
#define XLOG_BMAP_SHARED		0x70
#define XLOG_BMAP_UNREF			0x80
#define XLOG_BMAP_REMAP			0x90
#define XLOG_BMAP_FOLD			0xA0
#define XLOG_BMAP_CHANGES		0xB0
#define XLOG_BMAP_SHARE_CHANGES	0xC0

/* main data of CREATE, TRUNCATE and FREE */
typedef struct xl_bmap_rel
{
	RelFileLocator locator;
} xl_bmap_rel;

/* SHARE's and SHARED's main data: the relation shared and its copy. */
typedef struct xl_bmap_share
{
	RelFileLocator src;
	RelFileLocator dst;
} xl_bmap_share;

/*
 * UNREF's main data.  With BMAP_UNREF_MAP, block 0 is a map page of
 * 'locator' whose entries change; the reference-count pages follow.
 */
#define BMAP_UNREF_MAP			0x01

typedef struct xl_bmap_unref
{
	RelFileLocator locator;
	uint32		flags;
} xl_bmap_unref;

/*
 * REMAP's main data: the relation, the logical block copied, the block it
 * leaves and the one it takes, and the page store's high-water mark before
 * and after.  The page's image comes as its last block, unless it is all
 * zeroes (BMAP_REMAP_ZERO): then redo zeroes the new block, as extending
 * does.  A block left for the changes list (BMAP_REMAP_LISTED) gets the
 * image (or zeroes) in redo too: it is the page as it was before the
 * change, which a crash may have lost from the block.
 */
#define BMAP_REMAP_ZERO		0x01
#define BMAP_REMAP_LISTED	0x02	/* the old block stays, on the list */

typedef struct xl_bmap_remap
{
	RelFileLocator locator;
	BlockNumber blkno;
	BlockNumber old_block;
	BlockNumber new_block;
	BlockNumber old_hw;
	BlockNumber new_hw;
	uint32		flags;
} xl_bmap_remap;

/*
 * EXTEND's main data: the relation, the page store's high-water mark before
 * and after, and the reused physical blocks (below the old mark).  Redo
 * zeroes the reused blocks again and extends the page store, unless the
 * relation's data skips WAL (BMAP_EXTEND_SKIP_ZERO): then its pages were
 * written and synced at commit, and zeroing them would lose them.
 */
#define BMAP_EXTEND_SKIP_ZERO	0x01

typedef struct xl_bmap_extend
{
	RelFileLocator locator;
	BlockNumber old_hw;
	BlockNumber new_hw;
	uint32		flags;
	uint32		nreused;
	/* BlockNumber reused[nreused] follows */
} xl_bmap_extend;

#define SizeOfBmapExtend	(offsetof(xl_bmap_extend, nreused) + sizeof(uint32))

/*
 * Every block's data begins with a kind, saying which delta follows, and
 * flags.
 */
#define BMAP_DELTA_HEADER	1
#define BMAP_DELTA_NODE		2
#define BMAP_DELTA_BITS		3
#define BMAP_DELTA_META		4
#define BMAP_DELTA_REFS		5
#define BMAP_DELTA_CHANGES_HEAD	6
#define BMAP_DELTA_CHANGES	7

#define BMAP_DELTA_INIT		0x01	/* initialize the page first */

/*
 * Block data of a map's header: its relation's length, and its epoch: the
 * entries born before it may be shared with another map.
 */
typedef struct xl_bmap_header_delta
{
	uint16		kind;			/* BMAP_DELTA_HEADER */
	uint16		flags;
	BlockNumber nblocks;
	uint32		epoch;
} xl_bmap_header_delta;

/*
 * Block data of a map page: entries set.  An entry whose block changes
 * takes 'since', a WAL position no later than the record's start and after
 * every earlier record of the logical block.
 */
typedef struct xl_bmap_node_delta
{
	uint16		kind;			/* BMAP_DELTA_NODE */
	uint16		flags;
	uint32		nentries;
	XLogRecPtr	since;
	/* xl_bmap_entry_delta entries[nentries] follow */
} xl_bmap_node_delta;

typedef struct xl_bmap_entry_delta
{
	uint32		slot;
	BlockNumber block;
	uint32		epoch;
} xl_bmap_entry_delta;

/* Block data of a bitmap page: bits set to 'value'. */
typedef struct xl_bmap_bits_delta
{
	uint16		kind;			/* BMAP_DELTA_BITS */
	uint16		flags;
	uint32		value;
	uint32		nbits;
	/* uint32 bits[nbits] follow */
} xl_bmap_bits_delta;

/*
 * Block data of a reference-count page: counts set.  A block's count is the
 * number of maps holding it besides the first.
 */
typedef struct xl_bmap_refs_delta
{
	uint16		kind;			/* BMAP_DELTA_REFS */
	uint16		flags;
	uint32		nrefs;
	/* xl_bmap_ref_delta refs[nrefs] follow */
} xl_bmap_refs_delta;

typedef struct xl_bmap_ref_delta
{
	uint32		slot;
	uint32		count;
} xl_bmap_ref_delta;

/* Block data of the allocator's metapage: new counters, bitmap pages added. */
typedef struct xl_bmap_meta_delta
{
	uint16		kind;			/* BMAP_DELTA_META */
	uint16		flags;
	BlockNumber hw;
	uint32		nfree;
	uint32		hint;
	uint32		nbitmap_added;
	/* BlockNumber bitmap_added[nbitmap_added] follow */
} xl_bmap_meta_delta;

/*
 * Block data of a changes list's header: the epoch of the last fold, the
 * list's lineage (made with the list, kept by its copies), the relation's
 * length at the last fold, and the list's length.
 */
typedef struct xl_bmap_changes_head_delta
{
	uint16		kind;			/* BMAP_DELTA_CHANGES_HEAD */
	uint16		flags;
	uint32		fold_epoch;
	uint64		lineage;
	BlockNumber fold_nblocks;
	uint32		nentries;
} xl_bmap_changes_head_delta;

/* Block data of a changes list's page: entries set from slot 'first'. */
typedef struct xl_bmap_changes_delta
{
	uint16		kind;			/* BMAP_DELTA_CHANGES */
	uint16		flags;
	uint32		first;
	uint32		n;
	/* BmapChange entries[n] follow */
} xl_bmap_changes_delta;

/*
 * An entry of a changes list: a logical block copied in an epoch, and the
 * block its page had before, which the list holds (BMAP_CHANGE_COPY); or
 * the relation's length cut to 'blkno' (BMAP_CHANGE_TRUNCATE).
 */
#define BMAP_CHANGE_COPY		1
#define BMAP_CHANGE_TRUNCATE	2

typedef struct BmapChange
{
	uint32		epoch;
	BlockNumber blkno;
	BlockNumber old_block;
	uint32		kind;
} BmapChange;

extern void bmap_redo(XLogReaderState *record);
extern void bmap_desc(StringInfo buf, XLogReaderState *record);
extern const char *bmap_identify(uint8 info);
extern void bmap_mask(char *pagedata, BlockNumber blkno);

#endif							/* BMAP_XLOG_H */
