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
 * XLOG_BMAP_CREATE		a relation's map: block 0 is its header, block 1 its
 *						root page
 * XLOG_BMAP_EXTEND		blocks are allocated: block 0 is the map's header,
 *						block 1 the leaf, block 2 the allocator's metapage,
 *						then bitmap pages
 * XLOG_BMAP_TRUNCATE	the relation's length: block 0 is the map's header;
 *						the XLOG_SMGR_TRUNCATE before it, flushed alone,
 *						makes the same change in its replay
 * XLOG_BMAP_FREE		blocks are freed: block 0 is the allocator's metapage,
 *						then bitmap pages
 * XLOG_BMAP_PRESERVE	before a private block is shared: block 0 is the
 *						source's page image, main data its physical BlockNumber;
 *						redo restores both the source buffer and that store block
 * XLOG_BMAP_SHARED		a map's sharing ends: block 0 is the source's header
 *						(its epoch raised), block 1 the copy's (its length);
 *						PRESERVE records may follow it
 * XLOG_BMAP_UNREF		references dropped: reference-count pages, after a
 *						leaf whose entries change (BMAP_UNREF_MAP); or a
 *						sweep's roots' counts (BMAP_UNREF_ROOTS)
 * XLOG_BMAP_REMAP		a shared page is copied before it changes: block 0 is
 *						the leaf, then the allocator's metapage and bitmap
 *						pages, the old block's reference-count page if its
 *						count changes, and last the page itself, as an
 *						image unless BMAP_REMAP_COPIED
 * XLOG_BMAP_PASSED		blocks copied without an image (BMAP_REMAP_COPIED)
 *						up to a position may change: no block
 * XLOG_BMAP_TREE		a map's nodes (xl_bmap_tree says how)
 * XLOG_BMAP_RELEASE	the release queue (xl_bmap_release says how): block 0
 *						is its metapage, then its pages; a drain's record
 *						then has reference-count pages, and, if it frees
 *						blocks, the allocator's metapage and bitmap pages
 *
 * A leaf registered (EXTEND, REMAP, UNREF) is a page of the page store
 * (BMAP_STORE_RELNUMBER's main fork).
 */
#define XLOG_BMAP_INIT			0x00
#define XLOG_BMAP_CREATE		0x10
#define XLOG_BMAP_RELEASE		0x20
#define XLOG_BMAP_EXTEND		0x30
#define XLOG_BMAP_TRUNCATE		0x40
#define XLOG_BMAP_FREE			0x50
#define XLOG_BMAP_PASSED		0x60
#define XLOG_BMAP_SHARED		0x70
#define XLOG_BMAP_UNREF			0x80
#define XLOG_BMAP_REMAP			0x90
#define XLOG_BMAP_SHRINK		0xD0
#define XLOG_BMAP_PRESERVE		0xE0
#define XLOG_BMAP_TREE			0xF0

/* main data of CREATE, TRUNCATE and FREE */
typedef struct xl_bmap_rel
{
	RelFileLocator locator;
} xl_bmap_rel;

/*
 * SHARE's and SHARED's main data: the relation shared and its copy.  With
 * BMAP_PRESERVE_LOGGED (SHARED), the source's own blocks given theirs
 * since the redo pointer of the checkpoint last completed as the share
 * began were logged as PRESERVE images before the record, whose redo
 * wrote them to their blocks; with BMAP_SHARE_SYNCED, the others, the
 * newest of which got its block at 'synced_since', were written out and
 * synced by the share after the record instead (and maybe logged as
 * PRESERVE images after it, if a backup began meanwhile).  Redo of the
 * share writes the source's buffers out and, with BMAP_SHARE_SYNCED,
 * syncs the page store and makes sure no later recovery of this server
 * begins at or before 'synced_since'.
 */
#define BMAP_PRESERVE_LOGGED	0x01
#define BMAP_SHARE_SYNCED		0x02

typedef struct xl_bmap_share
{
	RelFileLocator src;
	RelFileLocator dst;
	uint8		flags;
	XLogRecPtr	synced_since;
} xl_bmap_share;

/*
 * UNREF's main data.  With BMAP_UNREF_MAP, block 0 is a map page of
 * 'locator' whose entries change; the reference-count pages follow.  With
 * BMAP_UNREF_ROOTS, the pages are a sweep's rewrite of the roots' counts
 * on them, 'locator' the page store.
 */
#define BMAP_UNREF_MAP			0x01
#define BMAP_UNREF_ROOTS		0x02

typedef struct xl_bmap_unref
{
	RelFileLocator locator;
	uint32		flags;
} xl_bmap_unref;

/*
 * SHRINK's main data: the page store, its high-water mark before and after
 * (the blocks between free).  The metapage's delta comes as block 0; redo
 * truncates the store too.
 */
typedef struct xl_bmap_shrink
{
	RelFileLocator locator;
	BlockNumber old_hw;
	BlockNumber new_hw;
} xl_bmap_shrink;

/*
 * REMAP's main data: the relation, the logical block copied, the block it
 * leaves and the one it takes, and the page store's high-water mark before
 * and after.  The page's image comes as its last block, unless it is all
 * zeroes (BMAP_REMAP_ZERO): then redo zeroes the new block, as extending
 * does; or unless the page's image or a change since the redo pointer is
 * in the WAL already, as a full-page write leaves it out
 * (BMAP_REMAP_COPIED): then redo copies the page as replay has it in its
 * buffer, or else from the old block, and the old block's reference-count
 * page says the block was copied from and where (its delta's
 * BMAP_DELTA_COPIED), so that the old block keeps that page until no
 * recovery begins before the copy.  (The record may carry the image all
 * the same, if a checkpoint began as it was inserted; redo restores it.)
 * The old block, which another map still holds, keeps what it has:
 * the primary doesn't write there (the image may have hints only this
 * map's buffer had).  A hole given its first block has no old block
 * (InvalidBlockNumber).
 */
#define BMAP_REMAP_ZERO		0x01
#define BMAP_REMAP_COPIED	0x02

/*
 * PASSED's main data: the redo pointer of the checkpoint last completed,
 * which every copy made without an image of a block now to be changed in
 * place or freed precedes, and the newest such copy's position (its
 * record's end).  Redo makes sure its server's restart point is no older
 * than the first, and keeps the second in pg_control (see bmap.c's
 * "Copies without an image").
 */
typedef struct xl_bmap_passed
{
	XLogRecPtr	horizon;
	XLogRecPtr	copied;
} xl_bmap_passed;

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
 * zeroes the reused blocks and the new ones again and extends the page
 * store.  A relation whose data skips WAL (made in the transaction at
 * wal_level minimal) has its pages logged as images at commit, which redo
 * restores after.  No flags yet.
 */

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
 * TREE's main data: the relation, what is done to its tree, and the page
 * store's high-water mark before and after.
 *
 * BMAP_TREE_NODE		a node is made: with BMAP_TREE_INSTALL, block 0 is the
 *						page whose pointer takes it (the root page, or a node);
 *						then the node, as an image; then the allocator's
 *						metapage and bitmap pages.  Redo extends the store to
 *						reach it, writing nothing over a block it had.
 * BMAP_TREE_HOLD		a node is to be copied: reference-count pages of
 *						children of it, each holder more (the copy's), set
 *						before the copy holds them
 * BMAP_TREE_COPY		a shared node, 'from', is copied to 'node', which the
 *						relation's page whose pointer named 'from' now names:
 *						as BMAP_TREE_NODE (block 0 the page, then the copy
 *						as an image, then the allocator's pages), then
 *						reference-count pages: of the last of the children
 *						(a holder more) and of 'from' (one less)
 * BMAP_TREE_SHARE		a tree shared: 'locator' is the copy, made with the
 *						source's (relation 'from''s) pointers; block 0 is
 *						its root page, then the reference-count pages of the
 *						subtrees, each a holder more; a copy of a captured
 *						root (BMAP_TREE_OF_ROOT) has its header next, and
 *						then, if its shared_below rose, the source's header
 * BMAP_TREE_CAPTURE	a root of the tree captured into its page store's
 *						roots (BMAP_ROOTS_RELNUMBER): block 0 is the roots
 *						page whose slot is set (its delta kept with an image
 *						too), block 1 the relation's header (its epoch
 *						raised), block 2 the roots' metapage, then the
 *						reference-count pages of the subtrees, each a holder
 *						more; always with BMAP_TREE_PRESERVE_LOGGED
 * BMAP_TREE_ROOT_SLOT	a captured root's slot changes: named as let go of
 *						by a (sub)transaction, or cleared (block 0, the
 *						roots page, alone: the metapage's hint is lowered
 *						as a hint, unlogged); or set to another root's
 *						subtrees (block 0 the roots page, block 1 the roots'
 *						metapage, then the reference-count pages of the
 *						subtrees, each a holder more).  'locator' is the
 *						roots file
 * BMAP_TREE_EPOCH		the map's epoch raised, so that each page's next change
 *						claims or copies it: block 0 is its header
 * BMAP_TREE_STAMP		a pointer to the relation's own node 'node' set in the
 *						map's epoch, the first change under it in the epoch:
 *						block 0 is the parent (a node, or the tree's root
 *						page), its delta
 */
#define BMAP_TREE_NODE			1
#define BMAP_TREE_HOLD			3
#define BMAP_TREE_COPY			4
#define BMAP_TREE_SHARE			5
#define BMAP_TREE_CAPTURE		7
#define BMAP_TREE_ROOT_SLOT		8
#define BMAP_TREE_EPOCH			9
#define BMAP_TREE_STAMP			10

#define BMAP_TREE_INSTALL		0x01
#define BMAP_TREE_OF_ROOT		0x02
/* a root captured whose own blocks were logged as PRESERVE images first */
#define BMAP_TREE_PRESERVE_LOGGED	0x04

typedef struct xl_bmap_tree
{
	RelFileLocator locator;
	uint16		op;
	uint16		flags;
	BlockNumber node;			/* the node made (NODE, COPY) */
	BlockNumber from;			/* the node copied (COPY), relation (SHARE) */
	BlockNumber old_hw;
	BlockNumber new_hw;
} xl_bmap_tree;

/*
 * RELEASE's main data: the page store, and what is done to its queue.
 *
 * BMAP_RELEASE_QUEUE	entries pushed, each a holder of what it names: a
 *						dropped map's subtrees and blocks
 * BMAP_RELEASE_DRAIN	entries popped, each a holder let go of: a block's
 *						count one less, or, if nothing else held it, the
 *						block freed (a node's children pushed first)
 */
#define BMAP_RELEASE_QUEUE		1
#define BMAP_RELEASE_DRAIN		2

typedef struct xl_bmap_release
{
	RelFileLocator locator;
	uint32		op;
	uint32		npopped;		/* (DRAIN) entries popped */
} xl_bmap_release;

/*
 * An entry of the release queue: a tree's node of 'level', mapping from
 * 'first', born at 'since' (the pointer to it, as a map has it); or a data
 * block (level BMAP_QUEUED_BLOCK).  It holds the block as a map's pointer
 * or entry does.  An entry pushed by a drop before its transaction commits
 * names that transaction (owner_hi and owner_lo, the low 48 bits of its
 * FullTransactionId; zero: none): it holds the block only once that
 * commits, and is let go of with nothing released if it doesn't.
 *
 * The roots' counts (the holders the captured roots give each block, kept
 * beside its reference count) follow the entries too.  One with
 * BMAP_QUEUED_COUNTED is a pointer those counts include, a counted root's
 * let go of or a counted node's child: letting go of it takes one off the
 * block's roots' count as well.  One with BMAP_QUEUED_UNCOUNT holds
 * nothing: it is a pointer of a node that has left what the roots hold
 * while it stays, which its roots' count is only to lose.
 */
#define BMAP_QUEUED_BLOCK		0xFF

#define BMAP_QUEUED_COUNTED		0x01
#define BMAP_QUEUED_UNCOUNT		0x02

typedef struct BmapQueued
{
	BlockNumber block;
	uint8		level;
	uint8		flags;
	uint16		owner_hi;
	BlockNumber first;
	uint32		owner_lo;
	XLogRecPtr	since;
} BmapQueued;

/*
 * Every block's data begins with a kind, saying which delta follows, and
 * flags.
 */
#define BMAP_DELTA_HEADER	1
#define BMAP_DELTA_NODE		2
#define BMAP_DELTA_BITS		3
#define BMAP_DELTA_META		4
#define BMAP_DELTA_REFS		5
#define BMAP_DELTA_ROOT		8
#define BMAP_DELTA_QUEUE_META	10
#define BMAP_DELTA_QUEUE	11
#define BMAP_DELTA_ROOTS	12
#define BMAP_DELTA_ROOTS_META	13

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
	uint32		shared_below;	/* (BMAP_DELTA_SHARED_BELOW) */
} xl_bmap_header_delta;

/* a header delta that sets the header's shared_below too */
#define BMAP_DELTA_SHARED_BELOW	0x02

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

/*
 * Block data of a tree's root page, always made anew: its pointers to its
 * subtrees, each a node's block, the epoch it was set in and the node's
 * birth (InvalidBlockNumber: none).
 */
typedef struct xl_bmap_top
{
	BlockNumber block;
	uint32		epoch;
	XLogRecPtr	since;
} xl_bmap_top;

/*
 * The subtrees a root page points to: as many as it takes for the last to
 * map past MaxBlockNumber with the block size's fanout (four of 509 entries
 * a node at 8 kB; smaller blocks fan out less and need more).
 */
#if BLCKSZ >= 8192
#define BMAP_DELTA_ROOT_TOPS	4
#elif BLCKSZ >= 2048
#define BMAP_DELTA_ROOT_TOPS	5
#else
#define BMAP_DELTA_ROOT_TOPS	6
#endif

typedef struct xl_bmap_root_delta
{
	uint16		kind;			/* BMAP_DELTA_ROOT */
	uint16		flags;			/* BMAP_DELTA_INIT */
	uint32		ntops;
	xl_bmap_top tops[BMAP_DELTA_ROOT_TOPS];
	uint64		made_of;		/* the captured root it is a copy of (0: none) */
	uint64		lineage;		/* the lineage it carries on (0: its own) */
} xl_bmap_root_delta;

/*
 * A tree as a captured root keeps it (xl_bmap_root): its id (0: the slot
 * is free), the (sub)transaction that captured it (a FullTransactionId),
 * the relation's length then, the epoch it raised
 * the map's to (every entry it holds was born before), and the subtrees it
 * holds.
 */
typedef struct xl_bmap_snapshot
{
	uint64		id;
	uint64		owner;
	BlockNumber nblocks;
	uint32		epoch;			/* the map's epoch it raised the map's to */
	xl_bmap_top tops[BMAP_DELTA_ROOT_TOPS];
} xl_bmap_snapshot;

/*
 * A root captured into a page store's roots: the tree as it holds it
 * (snap.id 0: the slot is free; snap.owner the capturing
 * (sub)transaction), the (sub)transaction that let go of it (0: none),
 * and the map it was captured of: its identity (which no other map has,
 * whatever its file number), lineage and file number (or the relation of whose
 * pages it holds frozen copies, and the epoch of that relation since which
 * they changed); and the database's datfrozenxid and datminmxid as it was
 * captured, older than any transaction or multixact its pages may name
 * unfrozen.  It holds its subtrees
 * while its capturer has committed (or is too old to say) and nobody who
 * let go of it has.
 */
typedef struct xl_bmap_root
{
	xl_bmap_snapshot snap;
	uint64		released_by;
	uint64		map_id;			/* of the map it was captured of */
	uint32		relnumber;
	uint32		live_epoch;		/* of relnumber, its changes since told by
								 * (0: snap.epoch) */
	uint64		lineage;		/* of the map it was captured of (or of
								 * relnumber's) */
	uint64		live_map;		/* a frozen root's: relnumber's map's
								 * identity (0: none) */
	TransactionId frozenxid;	/* the database's datfrozenxid then */
	MultiXactId minmxid;		/* the database's datminmxid then */
	uint32		counted;		/* in the roots' counts (set by a sweep) */
	uint32		unused;
} xl_bmap_root;

/* Block data of a roots page: slot 'index' of the page set. */
typedef struct xl_bmap_roots_delta
{
	uint16		kind;			/* BMAP_DELTA_ROOTS */
	uint16		flags;			/* BMAP_DELTA_INIT */
	uint32		index;
	xl_bmap_root root;
} xl_bmap_roots_delta;

/*
 * Block data of the roots' metapage: how many pages of slots follow it,
 * the first that may have a free slot, and the last root's sequence number;
 * and, if 'naming' isn't 0, that page of slots (its block number) marked as
 * one whose slots may name a transaction and may hold a root the roots'
 * counts don't count, or with BMAP_DELTA_NAMING_CLEAR, as one whose slots
 * name none, or with BMAP_DELTA_UNCOUNTED_CLEAR, as one whose roots are all
 * counted.  With BMAP_DELTA_COUNTED, 'counted' says whether the roots'
 * counts are whole.
 */
typedef struct xl_bmap_roots_meta_delta
{
	uint16		kind;			/* BMAP_DELTA_ROOTS_META */
	uint16		flags;			/* BMAP_DELTA_INIT, BMAP_DELTA_NAMING_CLEAR,
								 * BMAP_DELTA_UNCOUNTED_CLEAR,
								 * BMAP_DELTA_COUNTED */
	uint32		npages;
	uint32		hint;
	uint32		naming;
	uint64		seq;
	uint32		counted;
	uint32		unused;
} xl_bmap_roots_meta_delta;

/* a roots' metapage delta that unmarks its page of slots 'naming' */
#define BMAP_DELTA_NAMING_CLEAR	0x02
/* one that marks page 'naming' as holding only roots counted */
#define BMAP_DELTA_UNCOUNTED_CLEAR	0x04
/* one that sets whether the roots' counts are whole */
#define BMAP_DELTA_COUNTED		0x08

/* Block data of the release queue's metapage: how many entries it has. */
typedef struct xl_bmap_queue_meta_delta
{
	uint16		kind;			/* BMAP_DELTA_QUEUE_META */
	uint16		flags;
	uint32		nentries;
} xl_bmap_queue_meta_delta;

/* Block data of a release queue's page: entries set from slot 'first'. */
typedef struct xl_bmap_queue_delta
{
	uint16		kind;			/* BMAP_DELTA_QUEUE */
	uint16		flags;
	uint32		first;
	uint32		n;
	/* BmapQueued entries[n] follow */
} xl_bmap_queue_delta;

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
 * number of maps holding it besides the first, the whole uint32 of its slot
 * on the page.  Its roots' count, another uint32, follows the counts on the
 * page: how many of the pages holding it are the captured roots' (a root's
 * slot, or a node one reaches), in 31 bits, with a flag saying whether it
 * is such a node itself in the top bit; 'nroots' of them set, after the
 * counts.
 */
typedef struct xl_bmap_refs_delta
{
	uint16		kind;			/* BMAP_DELTA_REFS */
	uint16		flags;
	uint32		nrefs;
	uint32		nroots;
	/* xl_bmap_ref_delta refs[nrefs + nroots] follow */
} xl_bmap_refs_delta;

/*
 * Flags of a reference-count page's delta: BMAP_DELTA_COPIED, the first
 * count set is of a block copied without an image, whose copied bit is
 * set, and the page's copy position becomes the record's end; with
 * BMAP_DELTA_COPIED_RESET, the page's copied bits are all cleared first
 * (the copies they told of all precede the restart point).
 */
#define BMAP_DELTA_COPIED		0x02
#define BMAP_DELTA_COPIED_RESET	0x04

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

extern void bmap_redo(XLogReaderState *record);
extern void bmap_desc(StringInfo buf, XLogReaderState *record);
extern const char *bmap_identify(uint8 info);
extern void bmap_mask(char *pagedata, BlockNumber blkno);

#endif							/* BMAP_XLOG_H */
