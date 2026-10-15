/*-------------------------------------------------------------------------
 *
 * bmap.c
 *	  Block-mapped storage manager.
 *
 * A block-mapped relation's main fork is a sequence of logical blocks, each
 * mapped to a physical block of the page store its database keeps in each
 * tablespace.  The map is the relation's map fork (BMAP_FORKNUM), an
 * ordinary md.c file read through the buffer manager, so its pages are
 * WAL-logged, checkpointed and protected by full-page images like any
 * other.  The page store is read and written with md.c directly, never
 * through the buffer manager: the buffer manager caches the relation's
 * logical blocks, and this module translates their I/O.
 *
 * Other forks (the free space map and visibility map) are md.c files of the
 * relation.  They are extended during recovery without WAL records of their
 * own, so a standby couldn't follow the primary's allocations for them; and
 * they are hints or rebuildable, so they need no sharing.
 *
 * The map fork.  Block 0 is a header holding the relation's logical length.
 * Logical block L's entry is at slot L % BMAP_FANOUT of map page
 * 1 + L / BMAP_FANOUT: found without following any pointer.  That matters in
 * recovery, where map pages on disk can be from different moments (one
 * written after the point redo has reached, another not yet written): an
 * entry never changes once set, so any version of its page that has it
 * gives the same answer, and a page that redo needs has it.  An entry is a
 * physical block and a birth epoch, which nothing uses yet: it is there so
 * that sharing pages between relations can be added without rewriting
 * maps.  The map fork's existence is the relation's; smgropen() chooses
 * this storage manager by it.
 *
 * The allocator.  Per tablespace and database, BMAP_ALLOC_RELNUMBER is a
 * metapage (the page store's high-water mark, the count of free blocks, and
 * the bitmap pages) and bitmap pages saying which physical blocks are used.
 * Freed blocks are reused before the page store grows.  A reused block is
 * zeroed before it is mapped, so a crash can't expose its old contents at a
 * new logical block.  Truncation only shortens the logical length: blocks
 * past it are reused by a later extension, or freed with the relation.
 *
 * Locking.  Lookups read the header and a map page under share locks, one
 * at a time.  Extension takes the header's exclusive lock (serializing the
 * relation's extensions and its map fork's growth), then the map page's,
 * then the allocator's metapage and bitmap pages.  Truncation takes the
 * header's.  Writing a page looks it up without loading map pages into
 * shared buffers (see read_node()), so a write never waits for these locks
 * and an extension may read buffers while holding them.
 *
 * WAL.  Every map and allocator change is logged (bmap_xlog.h), and the
 * page store is changed (zeroed, extended) before the record that maps the
 * blocks, so a data record for a new block always follows the record that
 * allocated it.  Callers that log a page's image before writing it call
 * smgrpreextend() first.  Redo never allocates.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/smgr/bmap.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/bufmask.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "catalog/pg_tablespace.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "common/int.h"
#include "common/relpath.h"
#include "miscadmin.h"
#include "storage/bmap.h"
#include "storage/bmap_xlog.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/md.h"
#include "utils/memutils.h"
#include "utils/resowner.h"

bool		debug_block_mapped_heap = false;

/* ----------------------------------------------------------------
 * Page layouts
 * ----------------------------------------------------------------
 */

#define BMAP_MAGIC			0x424D4150	/* "BMAP" */
#define BMAP_VERSION		1

#define BMAP_PAGE_META		1
#define BMAP_PAGE_HEADER	2
#define BMAP_PAGE_NODE		3
#define BMAP_PAGE_BITMAP	4

typedef struct BmapPageOpaqueData
{
	uint16		type;			/* BMAP_PAGE_* */
	uint16		unused;
	uint32		magic;
} BmapPageOpaqueData;

typedef struct BmapEntry
{
	BlockNumber block;			/* physical block */
	uint32		epoch;
} BmapEntry;

typedef struct BmapHeader
{
	uint32		version;
	BlockNumber nblocks;		/* the relation's logical length */
} BmapHeader;

typedef struct BmapMeta
{
	uint32		version;
	BlockNumber hw;				/* page store blocks in use or freed */
	uint32		nfree;			/* freed blocks below hw */
	uint32		hint;			/* bitmap page to search first */
	uint32		nbitmap;
	uint32		epoch;
	BlockNumber bitmap[FLEXIBLE_ARRAY_MEMBER];
} BmapMeta;

#define BMAP_CONTENT_SIZE \
	(BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - MAXALIGN(sizeof(BmapPageOpaqueData)))
#define BMAP_FANOUT \
	((uint32) (BMAP_CONTENT_SIZE / sizeof(BmapEntry)))
#define BMAP_MAX_BITMAPS \
	((uint32) ((BMAP_CONTENT_SIZE - offsetof(BmapMeta, bitmap)) / sizeof(BlockNumber)))
#define BMAP_BITS_PER_PAGE \
	((uint32) ((BMAP_CONTENT_SIZE / sizeof(uint64)) * 64))

/* the map page and slot of logical block 'blk' */
#define MAP_PAGE(blk)		((BlockNumber) (1 + (blk) / BMAP_FANOUT))
#define MAP_SLOT(blk)		((uint32) ((blk) % BMAP_FANOUT))

/* blocks allocated by one record at most */
#define BMAP_PIECE			64
/* bitmap pages one allocation takes free blocks from at most */
#define BMAP_SCAN_PAGES		2

#define PageGetBmapOpaque(page) \
	((BmapPageOpaqueData *) PageGetSpecialPointer(page))
#define PageGetBmapHeader(page)	((BmapHeader *) PageGetContents(page))
/* a map page's contents are BMAP_FANOUT entries */
#define PageGetBmapEntries(page)	((BmapEntry *) PageGetContents(page))
#define PageGetBmapMeta(page)	((BmapMeta *) PageGetContents(page))
#define PageGetBmapBits(page)	((uint64 *) PageGetContents(page))

static const PGIOAlignedBlock zero_block;

/* ----------------------------------------------------------------
 * Files
 * ----------------------------------------------------------------
 */

static RelFileLocator
with_relnumber(RelFileLocator rel, RelFileNumber relnumber)
{
	RelFileLocator loc = rel;

	loc.relNumber = relnumber;
	return loc;
}

static SMgrRelation
store_smgr(RelFileLocator rel)
{
	return smgropen(with_relnumber(rel, BMAP_STORE_RELNUMBER),
					INVALID_PROC_NUMBER);
}

static SMgrRelation
alloc_smgr(RelFileLocator rel)
{
	return smgropen(with_relnumber(rel, BMAP_ALLOC_RELNUMBER),
					INVALID_PROC_NUMBER);
}

/*
 * Does a relation file number name a block-mapped relation?  It does if its
 * map fork exists.
 */
bool
bmap_relation_exists(RelFileLocator rel)
{
	RelPathStr	path = relpathperm(rel, BMAP_FORKNUM);
	struct stat st;

	return stat(path.str, &st) == 0;
}

/* ----------------------------------------------------------------
 * Pages
 * ----------------------------------------------------------------
 */

static void
check_page(Page page, uint16 type, RelFileLocator rel, ForkNumber fork,
		   BlockNumber blkno)
{
	BmapPageOpaqueData *opaque;

	if (PageIsNew(page) ||
		((PageHeader) page)->pd_special != BLCKSZ - MAXALIGN(sizeof(BmapPageOpaqueData)))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("block %u of \"%s\" is not an initialized block map page",
						blkno, relpathperm(rel, fork).str)));
	opaque = PageGetBmapOpaque(page);
	if (opaque->magic != BMAP_MAGIC || opaque->type != type)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("block %u of \"%s\" is not a block map page of the expected kind",
						blkno, relpathperm(rel, fork).str)));
}

static Buffer
map_read(RelFileLocator rel, BlockNumber blkno)
{
	return ReadBufferWithoutRelcache(rel, BMAP_FORKNUM, blkno, RBM_NORMAL,
									 NULL, true);
}

static Buffer
alloc_read(RelFileLocator rel, BlockNumber blkno)
{
	return ReadBufferWithoutRelcache(with_relnumber(rel, BMAP_ALLOC_RELNUMBER),
									 MAIN_FORKNUM, blkno, RBM_NORMAL, NULL,
									 true);
}

/*
 * Add a page to a relation's map fork, returned zeroed and locked
 * exclusively.  The caller holds the map's header exclusively, which
 * serializes this; it may also hold the relation's extension lock, so it
 * may take no other heavyweight lock.
 */
static Buffer
map_new_page(SMgrRelation reln)
{
	return ExtendBufferedRel(BMR_SMGR(reln, RELPERSISTENCE_PERMANENT),
							 BMAP_FORKNUM, NULL,
							 EB_LOCK_FIRST | EB_SKIP_EXTENSION_LOCK);
}

/*
 * Add a page to an allocator, returned zeroed and locked exclusively.  The
 * caller holds the allocator's metapage exclusively, which serializes this.
 */
static Buffer
alloc_new_page(RelFileLocator rel)
{
	return ExtendBufferedRel(BMR_SMGR(alloc_smgr(rel), RELPERSISTENCE_PERMANENT),
							 MAIN_FORKNUM, NULL,
							 EB_LOCK_FIRST | EB_SKIP_EXTENSION_LOCK);
}

static void
init_page(Page page, uint16 type)
{
	BmapPageOpaqueData *opaque;

	PageInit(page, BLCKSZ, sizeof(BmapPageOpaqueData));
	opaque = PageGetBmapOpaque(page);
	opaque->type = type;
	opaque->unused = 0;
	opaque->magic = BMAP_MAGIC;
	memset(PageGetContents(page), 0, BMAP_CONTENT_SIZE);
	/* the contents fill the page; nothing for a full-page image to skip */
	((PageHeader) page)->pd_lower = ((PageHeader) page)->pd_upper;
}

static void
init_node(Page page)
{
	BmapEntry  *entries;

	init_page(page, BMAP_PAGE_NODE);
	entries = PageGetBmapEntries(page);
	for (uint32 i = 0; i < BMAP_FANOUT; i++)
		entries[i].block = InvalidBlockNumber;
}

static void
init_header(Page page)
{
	init_page(page, BMAP_PAGE_HEADER);
	PageGetBmapHeader(page)->version = BMAP_VERSION;
}

static void
init_meta(Page page)
{
	init_page(page, BMAP_PAGE_META);
	PageGetBmapMeta(page)->version = BMAP_VERSION;
}

/*
 * The deltas: the one way these pages change, applied the same in normal
 * running and in redo.
 */
static void
apply_header_delta(Page page, const xl_bmap_header_delta *delta)
{
	if (delta->flags & BMAP_DELTA_INIT)
		init_header(page);
	PageGetBmapHeader(page)->nblocks = delta->nblocks;
}

static void
apply_node_delta(Page page, const xl_bmap_node_delta *delta)
{
	const xl_bmap_entry_delta *ent = (const xl_bmap_entry_delta *) (delta + 1);
	BmapEntry  *entries;

	if (delta->flags & BMAP_DELTA_INIT)
		init_node(page);
	entries = PageGetBmapEntries(page);
	for (uint32 i = 0; i < delta->nentries; i++)
	{
		Assert(ent[i].slot < BMAP_FANOUT);
		entries[ent[i].slot].block = ent[i].block;
		entries[ent[i].slot].epoch = ent[i].epoch;
	}
}

static void
apply_bits_delta(Page page, const xl_bmap_bits_delta *delta)
{
	const uint32 *bits = (const uint32 *) (delta + 1);
	uint64	   *words;

	if (delta->flags & BMAP_DELTA_INIT)
		init_page(page, BMAP_PAGE_BITMAP);
	words = PageGetBmapBits(page);
	for (uint32 i = 0; i < delta->nbits; i++)
	{
		uint64		mask = UINT64CONST(1) << (bits[i] % 64);

		Assert(bits[i] < BMAP_BITS_PER_PAGE);
		if (delta->value)
			words[bits[i] / 64] |= mask;
		else
			words[bits[i] / 64] &= ~mask;
	}
}

static void
apply_meta_delta(Page page, const xl_bmap_meta_delta *delta)
{
	const BlockNumber *added = (const BlockNumber *) (delta + 1);
	BmapMeta   *meta;

	if (delta->flags & BMAP_DELTA_INIT)
		init_meta(page);
	meta = PageGetBmapMeta(page);
	meta->hw = delta->hw;
	meta->nfree = delta->nfree;
	meta->hint = delta->hint;
	for (uint32 i = 0; i < delta->nbitmap_added; i++)
	{
		if (meta->nbitmap >= BMAP_MAX_BITMAPS)
			elog(PANIC, "block map allocator has too many bitmap pages");
		meta->bitmap[meta->nbitmap++] = added[i];
	}
}

static Size
node_delta_size(const xl_bmap_node_delta *d)
{
	return sizeof(xl_bmap_node_delta) + d->nentries * sizeof(xl_bmap_entry_delta);
}

static Size
bits_delta_size(const xl_bmap_bits_delta *d)
{
	return sizeof(xl_bmap_bits_delta) + d->nbits * sizeof(uint32);
}

static Size
meta_delta_size(const xl_bmap_meta_delta *d)
{
	return sizeof(xl_bmap_meta_delta) + d->nbitmap_added * sizeof(BlockNumber);
}

/* ----------------------------------------------------------------
 * Creating a database's allocator
 * ----------------------------------------------------------------
 */

static bool
alloc_exists(RelFileLocator rel)
{
	SMgrRelation srel = alloc_smgr(rel);

	return smgrexists(srel, MAIN_FORKNUM) && smgrnblocks(srel, MAIN_FORKNUM) > 0;
}

/*
 * Make sure the tablespace and database of 'rel' have an allocator and a
 * page store.
 */
static void
ensure_alloc(RelFileLocator rel)
{
	LOCKTAG		tag;

	if (alloc_exists(rel))
		return;

	SET_LOCKTAG_RELATION(tag, rel.dbOid, BMAP_ALLOC_RELNUMBER);
	LockAcquire(&tag, AccessExclusiveLock, false, false);
	if (!alloc_exists(rel))
	{
		RelFileLocator aloc = with_relnumber(rel, BMAP_ALLOC_RELNUMBER);
		RelFileLocator sloc = with_relnumber(rel, BMAP_STORE_RELNUMBER);
		SMgrRelation asrel = alloc_smgr(rel);
		SMgrRelation ssrel = store_smgr(rel);
		Buffer		buf;
		Page		page;

		if (!smgrexists(ssrel, MAIN_FORKNUM))
		{
			smgrcreate(ssrel, MAIN_FORKNUM, false);
			log_smgrcreate(&sloc, MAIN_FORKNUM);
		}
		if (!smgrexists(asrel, MAIN_FORKNUM))
		{
			smgrcreate(asrel, MAIN_FORKNUM, false);
			log_smgrcreate(&aloc, MAIN_FORKNUM);
		}

		buf = alloc_new_page(rel);
		if (BufferGetBlockNumber(buf) != 0)
			elog(ERROR, "block map allocator of database %u begins at block %u",
				 rel.dbOid, BufferGetBlockNumber(buf));
		page = BufferGetPage(buf);

		START_CRIT_SECTION();
		init_meta(page);
		MarkBufferDirty(buf);
		XLogBeginInsert();
		XLogRegisterBuffer(0, buf, REGBUF_WILL_INIT | REGBUF_STANDARD);
		PageSetLSN(page, XLogInsert(RM_BMAP_ID, XLOG_BMAP_INIT));
		END_CRIT_SECTION();
		UnlockReleaseBuffer(buf);
	}
	LockRelease(&tag, AccessExclusiveLock, false);
}

/* ----------------------------------------------------------------
 * Lookups
 * ----------------------------------------------------------------
 */

/*
 * Copy map page 'blk' of 'reln' into 'dst' (I/O-aligned).
 *
 * Writing a block-mapped page looks the page up, and a write may be the
 * eviction a buffer read needs; if the lookup read the map through the
 * buffer manager, it could need an eviction of its own, which could need a
 * lookup, and so on until no buffer is left.  So a lookup 'for_write' never
 * loads a map page into shared buffers: it uses the page if it is cached,
 * and otherwise reads it from disk, where an uncached page has every entry
 * a write can need (entries never change once set).  A torn read fails
 * verification and is read again.
 */
static void
read_node(SMgrRelation reln, BlockNumber blk, bool for_write, char *dst,
		  uint16 type)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		buf;

	if (for_write)
	{
		PrefetchBufferResult r = PrefetchSharedBuffer(reln, BMAP_FORKNUM, blk);

		if (!BufferIsValid(r.recent_buffer) ||
			!ReadRecentBuffer(rel, BMAP_FORKNUM, blk, r.recent_buffer))
		{
			for (int tries = 0;; tries++)
			{
				void	   *page = dst;

				mdreadv(reln, BMAP_FORKNUM, blk, &page, 1);
				if (PageIsVerified(dst, blk, 0, NULL))
					break;
				if (tries >= 3)
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("invalid page in block %u of \"%s\"",
									blk, relpathperm(rel, BMAP_FORKNUM).str)));
			}
			check_page(dst, type, rel, BMAP_FORKNUM, blk);
			return;
		}
		buf = r.recent_buffer;
	}
	else
		buf = map_read(rel, blk);

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(buf), type, rel, BMAP_FORKNUM, blk);
	memcpy(dst, BufferGetPage(buf), BLCKSZ);
	UnlockReleaseBuffer(buf);
}

static BlockNumber
header_nblocks(SMgrRelation reln)
{
	PGIOAlignedBlock copy;

	read_node(reln, 0, false, copy.data, BMAP_PAGE_HEADER);
	return PageGetBmapHeader(copy.data)->nblocks;
}

/*
 * The physical block of logical block 'blkno', and in *run how many logical
 * blocks from it (at most 'maxrun') are physically consecutive.  If the
 * block has no entry, returns InvalidBlockNumber when 'missing_ok', else
 * fails.  See read_node() for 'for_write'.
 */
static BlockNumber
lookup_ext(SMgrRelation reln, BlockNumber blkno, BlockNumber maxrun,
		   BlockNumber *run, bool missing_ok, bool for_write)
{
	PGIOAlignedBlock copy;
	BmapEntry  *entries = PageGetBmapEntries(copy.data);
	uint32		slot = MAP_SLOT(blkno);
	BlockNumber phys;
	BlockNumber n;

	if (MAP_PAGE(blkno) >= mdnblocks(reln, BMAP_FORKNUM))
		phys = InvalidBlockNumber;
	else
	{
		read_node(reln, MAP_PAGE(blkno), for_write, copy.data, BMAP_PAGE_NODE);
		phys = entries[slot].block;
	}
	if (!BlockNumberIsValid(phys))
	{
		if (missing_ok)
			return InvalidBlockNumber;
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("block map of \"%s\" has no page for block %u",
						relpath(reln->smgr_rlocator, MAIN_FORKNUM).str,
						blkno)));
	}
	n = 1;
	while (n < maxrun && slot + n < BMAP_FANOUT &&
		   entries[slot + n].block == phys + n)
		n++;

	if (run)
		*run = n;
	return phys;
}

static BlockNumber
lookup(SMgrRelation reln, BlockNumber blkno, BlockNumber maxrun,
	   BlockNumber *run)
{
	return lookup_ext(reln, blkno, maxrun, run, false, false);
}

/* ----------------------------------------------------------------
 * Allocation
 * ----------------------------------------------------------------
 */

/*
 * Make the page store hold the blocks below 'new_hw', zeroing the blocks
 * from 'old_hw', and zero the reused blocks.  Done before the record that
 * maps them, and again by its redo unless 'skip_existing': then blocks the
 * page store already has are left as they are.
 */
static void
prepare_store(RelFileLocator rel, BlockNumber old_hw, BlockNumber new_hw,
			  const BlockNumber *reused, uint32 nreused, bool skip_existing)
{
	SMgrRelation ssrel = store_smgr(rel);
	BlockNumber store_cur = mdnblocks(ssrel, MAIN_FORKNUM);
	BlockNumber blk;

	for (uint32 i = 0; i < nreused; i++)
	{
		const void *page = zero_block.data;

		if (skip_existing && reused[i] < store_cur)
			continue;
		mdwritev(ssrel, MAIN_FORKNUM, reused[i], &page, 1, false);
	}
	for (blk = old_hw; blk < new_hw && blk < store_cur; blk++)
	{
		const void *page = zero_block.data;

		if (skip_existing)
			continue;
		mdwritev(ssrel, MAIN_FORKNUM, blk, &page, 1, false);
	}
	if (blk < new_hw)
		mdzeroextend(ssrel, MAIN_FORKNUM, blk, new_hw - blk, false);
}

typedef struct BitsChange
{
	Buffer		buf;
	uint32		pageno;			/* which bitmap page */
	xl_bmap_bits_delta *delta;	/* followed by room for the bits */
} BitsChange;

static BitsChange *
bits_change(BitsChange *bits, int *nbits, Buffer buf, uint32 pageno,
			uint32 value, bool isnew, uint32 room)
{
	BitsChange *bc = &bits[(*nbits)++];

	bc->buf = buf;
	bc->pageno = pageno;
	bc->delta = palloc0(sizeof(xl_bmap_bits_delta) + room * sizeof(uint32));
	bc->delta->kind = BMAP_DELTA_BITS;
	bc->delta->value = value;
	if (isnew)
		bc->delta->flags |= BMAP_DELTA_INIT;
	return bc;
}

/*
 * Extend 'reln' by the blocks [from, to), all on one map page, with
 * 'content' written to block to - 1 if given.  The header is locked
 * exclusively, and the relation is 'from' blocks long.
 */
static void
extend_piece(SMgrRelation reln, Buffer headerbuf, BlockNumber from,
			 BlockNumber to, const void *content, bool skipFsync)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	uint32		n = to - from;
	BlockNumber mapblk = MAP_PAGE(from);
	BlockNumber mapnblocks;
	Buffer		mapbuf;
	bool		mapnew;
	Buffer		metabuf;
	BmapMeta   *meta;
	BlockNumber phys[BMAP_PIECE];
	bool		need[BMAP_PIECE];
	uint32		nneed = 0;
	BlockNumber reused[BMAP_PIECE];
	uint32		nreused = 0;
	BlockNumber taken[BMAP_PIECE];
	uint32		ntaken = 0;
	BitsChange	bits[BMAP_SCAN_PAGES + 2];
	int			nbits = 0;
	BlockNumber added[2];
	int			nadded = 0;
	BlockNumber old_hw;
	BlockNumber new_hw;
	uint32		new_nfree;
	uint32		new_hint;
	xl_bmap_header_delta hdelta;
	xl_bmap_node_delta *ndelta;
	xl_bmap_meta_delta *mdelta;
	xl_bmap_extend xlrec;
	XLogRecPtr	lsn;
	int			blockid;

	Assert(n > 0 && n <= BMAP_PIECE);
	Assert(MAP_PAGE(to - 1) == mapblk);

	/*
	 * The map page, added if the map fork doesn't reach it yet.  An existing
	 * page is read now and locked only after the allocator's pages have been
	 * read: a read may write out one of this relation's pages, which looks
	 * it up on this map page.  Its entries in [from, to) can't change
	 * meanwhile, since the header serializes the relation's extensions.  A
	 * new page is referenced by nothing yet, so holding it locked is
	 * harmless.
	 */
	mapnblocks = mdnblocks(reln, BMAP_FORKNUM);
	mapnew = mapnblocks <= mapblk;
	if (mapnew)
	{
		/* blocks are added in order, so the map fork ends just before */
		if (mapnblocks != mapblk)
			elog(ERROR, "map of \"%s\" has %u pages, and none for block %u",
				 relpath(reln->smgr_rlocator, MAIN_FORKNUM).str,
				 mapnblocks, from);
		mapbuf = map_new_page(reln);
		if (BufferGetBlockNumber(mapbuf) != mapblk)
			elog(ERROR, "map of \"%s\" extended to block %u, expected %u",
				 relpath(reln->smgr_rlocator, MAIN_FORKNUM).str,
				 BufferGetBlockNumber(mapbuf), mapblk);
	}
	else
	{
		mapbuf = map_read(rel, mapblk);

		/*
		 * A page added to the map fork by an extension whose record a crash
		 * lost is all zeroes; nothing refers to it, so it is used as new.
		 */
		LockBuffer(mapbuf, BUFFER_LOCK_EXCLUSIVE);
		if (PageIsNew(BufferGetPage(mapbuf)))
			mapnew = true;
		else
		{
			LockBuffer(mapbuf, BUFFER_LOCK_UNLOCK);
			LockBuffer(mapbuf, BUFFER_LOCK_SHARE);
			check_page(BufferGetPage(mapbuf), BMAP_PAGE_NODE, rel,
					   BMAP_FORKNUM, mapblk);
		}
	}

	/* which blocks need a physical block: the map may hold stale ones */
	for (uint32 i = 0; i < n; i++)
	{
		phys[i] = mapnew ? InvalidBlockNumber :
			PageGetBmapEntries(BufferGetPage(mapbuf))[MAP_SLOT(from + i)].block;
		need[i] = !BlockNumberIsValid(phys[i]);
		if (need[i])
			nneed++;
		else
			reused[nreused++] = phys[i];
	}
	if (!mapnew)
		LockBuffer(mapbuf, BUFFER_LOCK_UNLOCK);

	/* allocate under the allocator's metapage lock */
	metabuf = alloc_read(rel, 0);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(metabuf), BMAP_PAGE_META, rel, MAIN_FORKNUM, 0);
	meta = PageGetBmapMeta(BufferGetPage(metabuf));
	old_hw = new_hw = meta->hw;
	new_nfree = meta->nfree;
	new_hint = meta->hint;

	/*
	 * Freed blocks first, except for a relation whose data skips WAL (made
	 * in this transaction at wal_level minimal): its pages are synced at
	 * commit, not logged, so nothing in redo would restore them if replaying
	 * an earlier record of the block's previous owner zeroed them.  Blocks
	 * above the high-water mark were never in an earlier record.
	 */
	if (nneed > 0 && meta->nfree > 0 && !RelFileLocatorSkippingWAL(rel))
	{
		for (uint32 k = 0; k < meta->nbitmap && ntaken < nneed &&
			 nbits < BMAP_SCAN_PAGES; k++)
		{
			uint32		pageno = (meta->hint + k) % meta->nbitmap;
			Buffer		buf = alloc_read(rel, meta->bitmap[pageno]);
			uint64	   *words;
			BitsChange *bc = NULL;
			BlockNumber base = pageno * BMAP_BITS_PER_PAGE;

			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
			check_page(BufferGetPage(buf), BMAP_PAGE_BITMAP, rel,
					   MAIN_FORKNUM, meta->bitmap[pageno]);
			words = PageGetBmapBits(BufferGetPage(buf));
			for (uint32 b = 0; b < BMAP_BITS_PER_PAGE && ntaken < nneed &&
				 base + b < old_hw; b++)
			{
				if (b % 64 == 0 && words[b / 64] == PG_UINT64_MAX)
				{
					b += 63;
					continue;
				}
				if ((words[b / 64] & (UINT64CONST(1) << (b % 64))) == 0)
				{
					if (bc == NULL)
						bc = bits_change(bits, &nbits, buf, pageno, 1, false,
										 BMAP_PIECE);
					((uint32 *) (bc->delta + 1))[bc->delta->nbits++] = b;
					taken[ntaken++] = base + b;
				}
			}
			if (bc == NULL)
				UnlockReleaseBuffer(buf);
			if (ntaken < nneed)
				new_hint = (pageno + 1) % meta->nbitmap;
		}
		new_nfree -= ntaken;
	}

	/* then the high-water mark */
	while (ntaken < nneed)
	{
		BlockNumber blk = new_hw++;
		uint32		pageno = blk / BMAP_BITS_PER_PAGE;
		BitsChange *bc = NULL;

		for (int k = 0; k < nbits; k++)
			if (bits[k].pageno == pageno)
				bc = &bits[k];
		if (bc == NULL)
		{
			if (pageno < meta->nbitmap)
			{
				Buffer		buf = alloc_read(rel, meta->bitmap[pageno]);

				LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
				check_page(BufferGetPage(buf), BMAP_PAGE_BITMAP, rel,
						   MAIN_FORKNUM, meta->bitmap[pageno]);
				bc = bits_change(bits, &nbits, buf, pageno, 1, false,
								 BMAP_PIECE);
			}
			else
			{
				Buffer		buf;

				/* a new bitmap page, the next one in order */
				if (pageno != meta->nbitmap + nadded ||
					meta->nbitmap + nadded >= BMAP_MAX_BITMAPS)
					ereport(ERROR,
							(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							 errmsg("block-mapped storage of database %u is full",
									rel.dbOid)));
				buf = alloc_new_page(rel);
				added[nadded++] = BufferGetBlockNumber(buf);
				bc = bits_change(bits, &nbits, buf, pageno, 1, true,
								 BMAP_PIECE);
			}
		}
		((uint32 *) (bc->delta + 1))[bc->delta->nbits++] = blk % BMAP_BITS_PER_PAGE;
		taken[ntaken++] = blk;
	}

	/* hand the blocks out in logical order */
	for (uint32 i = 0, t = 0; i < n; i++)
	{
		if (need[i])
		{
			phys[i] = taken[t++];
			if (phys[i] < old_hw)
				reused[nreused++] = phys[i];
		}
	}

	/* every read is done; now the map page can be locked for the update */
	if (!mapnew)
		LockBuffer(mapbuf, BUFFER_LOCK_EXCLUSIVE);

	/* the page store before the record */
	prepare_store(rel, old_hw, new_hw, reused, nreused, false);
	if (content)
	{
		const void *page = content;

		mdwritev(store_smgr(rel), MAIN_FORKNUM, phys[n - 1], &page, 1,
				 skipFsync);
	}

	/* deltas */
	hdelta.kind = BMAP_DELTA_HEADER;
	hdelta.flags = 0;
	hdelta.nblocks = to;
	ndelta = palloc0(sizeof(xl_bmap_node_delta) + n * sizeof(xl_bmap_entry_delta));
	ndelta->kind = BMAP_DELTA_NODE;
	if (mapnew)
		ndelta->flags |= BMAP_DELTA_INIT;
	for (uint32 i = 0; i < n; i++)
	{
		xl_bmap_entry_delta *ent = (xl_bmap_entry_delta *) (ndelta + 1);

		if (!need[i])
			continue;
		ent[ndelta->nentries].slot = MAP_SLOT(from + i);
		ent[ndelta->nentries].block = phys[i];
		ent[ndelta->nentries].epoch = 0;
		ndelta->nentries++;
	}
	mdelta = palloc0(sizeof(xl_bmap_meta_delta) + 2 * sizeof(BlockNumber));
	mdelta->kind = BMAP_DELTA_META;
	mdelta->hw = new_hw;
	mdelta->nfree = new_nfree;
	mdelta->hint = new_hint;
	mdelta->nbitmap_added = nadded;
	for (int i = 0; i < nadded; i++)
		((BlockNumber *) (mdelta + 1))[i] = added[i];

	xlrec.locator = rel;
	xlrec.old_hw = old_hw;
	xlrec.new_hw = new_hw;
	xlrec.flags = RelFileLocatorSkippingWAL(rel) ? BMAP_EXTEND_SKIP_ZERO : 0;
	xlrec.nreused = nreused;

	XLogEnsureRecordSpace(3 + nbits, 0);

	START_CRIT_SECTION();
	apply_header_delta(BufferGetPage(headerbuf), &hdelta);
	MarkBufferDirty(headerbuf);
	apply_node_delta(BufferGetPage(mapbuf), ndelta);
	MarkBufferDirty(mapbuf);
	apply_meta_delta(BufferGetPage(metabuf), mdelta);
	MarkBufferDirty(metabuf);
	for (int i = 0; i < nbits; i++)
	{
		apply_bits_delta(BufferGetPage(bits[i].buf), bits[i].delta);
		MarkBufferDirty(bits[i].buf);
	}

	XLogBeginInsert();
	XLogRegisterData(&xlrec, SizeOfBmapExtend);
	if (nreused > 0)
		XLogRegisterData(reused, nreused * sizeof(BlockNumber));
	XLogRegisterBuffer(0, headerbuf, REGBUF_STANDARD);
	XLogRegisterBufData(0, &hdelta, sizeof(hdelta));
	XLogRegisterBuffer(1, mapbuf, REGBUF_STANDARD |
					   (mapnew ? REGBUF_WILL_INIT : 0));
	XLogRegisterBufData(1, ndelta, node_delta_size(ndelta));
	XLogRegisterBuffer(2, metabuf, REGBUF_STANDARD);
	XLogRegisterBufData(2, mdelta, meta_delta_size(mdelta));
	blockid = 3;
	for (int i = 0; i < nbits; i++)
	{
		XLogRegisterBuffer(blockid, bits[i].buf, REGBUF_STANDARD |
						   ((bits[i].delta->flags & BMAP_DELTA_INIT) ?
							REGBUF_WILL_INIT : 0));
		XLogRegisterBufData(blockid, bits[i].delta,
							bits_delta_size(bits[i].delta));
		blockid++;
	}
	lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_EXTEND);

	PageSetLSN(BufferGetPage(headerbuf), lsn);
	PageSetLSN(BufferGetPage(mapbuf), lsn);
	PageSetLSN(BufferGetPage(metabuf), lsn);
	for (int i = 0; i < nbits; i++)
		PageSetLSN(BufferGetPage(bits[i].buf), lsn);
	END_CRIT_SECTION();

	for (int i = 0; i < nbits; i++)
	{
		UnlockReleaseBuffer(bits[i].buf);
		pfree(bits[i].delta);
	}
	UnlockReleaseBuffer(metabuf);
	UnlockReleaseBuffer(mapbuf);
	pfree(ndelta);
	pfree(mdelta);
}

/*
 * Extend 'reln' to 'to' blocks, writing 'content' to block to - 1 if given.
 */
static void
extend_to(SMgrRelation reln, BlockNumber to, const void *content,
		  bool skipFsync)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		headerbuf;

	if (InRecovery)
		elog(ERROR, "block-mapped relation %s extended during recovery without its allocation",
			 relpath(reln->smgr_rlocator, MAIN_FORKNUM).str);

	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);

	for (;;)
	{
		BlockNumber nblocks = PageGetBmapHeader(BufferGetPage(headerbuf))->nblocks;
		BlockNumber end;

		if (nblocks >= to)
		{
			/* long enough; just write the content */
			if (content)
			{
				const void *page = content;
				BlockNumber phys = lookup_ext(reln, to - 1, 1, NULL, false, true);

				mdwritev(store_smgr(rel), MAIN_FORKNUM, phys, &page, 1,
						 skipFsync);
			}
			break;
		}

		end = Min(to, nblocks + BMAP_PIECE);
		end = Min(end, (nblocks / BMAP_FANOUT + 1) * BMAP_FANOUT);
		extend_piece(reln, headerbuf, nblocks, end,
					 end == to ? content : NULL, skipFsync);
	}
	UnlockReleaseBuffer(headerbuf);
}

/* ----------------------------------------------------------------
 * Freeing
 * ----------------------------------------------------------------
 */

static int
cmp_block(const void *a, const void *b)
{
	BlockNumber x = *(const BlockNumber *) a;
	BlockNumber y = *(const BlockNumber *) b;

	return pg_cmp_u32(x, y);
}

/*
 * Clear the allocator's bits of the physical blocks blocks[0 .. n), sorted,
 * in records of a few bitmap pages each.
 */
static void
free_blocks(RelFileLocator rel, BlockNumber *blocks, uint32 n)
{
	uint32		i = 0;

	while (i < n)
	{
		Buffer		metabuf;
		BmapMeta   *meta;
		BitsChange	bits[BMAP_SCAN_PAGES];
		int			nbits = 0;
		xl_bmap_meta_delta mdelta;
		uint32		freed = 0;
		uint32		hint;
		XLogRecPtr	lsn;

		metabuf = alloc_read(rel, 0);
		LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
		check_page(BufferGetPage(metabuf), BMAP_PAGE_META, rel, MAIN_FORKNUM, 0);
		meta = PageGetBmapMeta(BufferGetPage(metabuf));
		hint = meta->hint;

		while (i < n && nbits < BMAP_SCAN_PAGES)
		{
			uint32		pageno = blocks[i] / BMAP_BITS_PER_PAGE;
			BitsChange *bc;
			uint64	   *words;
			Buffer		buf;
			uint32		end = i;

			if (pageno >= meta->nbitmap || blocks[i] >= meta->hw)
				elog(ERROR, "block-mapped storage of database %u frees block %u past its end",
					 rel.dbOid, blocks[i]);
			while (end < n && blocks[end] / BMAP_BITS_PER_PAGE == pageno)
				end++;
			buf = alloc_read(rel, meta->bitmap[pageno]);
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
			check_page(BufferGetPage(buf), BMAP_PAGE_BITMAP, rel, MAIN_FORKNUM,
					   meta->bitmap[pageno]);
			bc = bits_change(bits, &nbits, buf, pageno, 0, false, end - i);
			words = PageGetBmapBits(BufferGetPage(buf));
			for (; i < end; i++)
			{
				uint32		b = blocks[i] % BMAP_BITS_PER_PAGE;

				/* count only bits that are set, so a block is freed once */
				if (words[b / 64] & (UINT64CONST(1) << (b % 64)))
				{
					((uint32 *) (bc->delta + 1))[bc->delta->nbits++] = b;
					freed++;
				}
			}
			hint = Min(hint, pageno);
		}

		mdelta.kind = BMAP_DELTA_META;
		mdelta.flags = 0;
		mdelta.hw = meta->hw;
		mdelta.nfree = meta->nfree + freed;
		mdelta.hint = hint;
		mdelta.nbitmap_added = 0;

		XLogEnsureRecordSpace(1 + nbits, 0);

		START_CRIT_SECTION();
		apply_meta_delta(BufferGetPage(metabuf), &mdelta);
		MarkBufferDirty(metabuf);
		for (int k = 0; k < nbits; k++)
		{
			apply_bits_delta(BufferGetPage(bits[k].buf), bits[k].delta);
			MarkBufferDirty(bits[k].buf);
		}
		XLogBeginInsert();
		XLogRegisterData(&rel, sizeof(xl_bmap_rel));
		XLogRegisterBuffer(0, metabuf, REGBUF_STANDARD);
		XLogRegisterBufData(0, &mdelta, sizeof(mdelta));
		for (int k = 0; k < nbits; k++)
		{
			XLogRegisterBuffer(1 + k, bits[k].buf, REGBUF_STANDARD);
			XLogRegisterBufData(1 + k, bits[k].delta,
								bits_delta_size(bits[k].delta));
		}
		lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_FREE);
		PageSetLSN(BufferGetPage(metabuf), lsn);
		for (int k = 0; k < nbits; k++)
			PageSetLSN(BufferGetPage(bits[k].buf), lsn);
		END_CRIT_SECTION();

		for (int k = 0; k < nbits; k++)
		{
			UnlockReleaseBuffer(bits[k].buf);
			pfree(bits[k].delta);
		}
		UnlockReleaseBuffer(metabuf);
	}
}

/* Set a relation's length; the header is pinned and not locked. */
static void
set_length(RelFileLocator rel, Buffer headerbuf, BlockNumber nblocks)
{
	xl_bmap_header_delta delta;
	XLogRecPtr	lsn;

	delta.kind = BMAP_DELTA_HEADER;
	delta.flags = 0;
	delta.nblocks = nblocks;

	LockBuffer(headerbuf, BUFFER_LOCK_EXCLUSIVE);
	START_CRIT_SECTION();
	apply_header_delta(BufferGetPage(headerbuf), &delta);
	MarkBufferDirty(headerbuf);
	XLogBeginInsert();
	XLogRegisterData(&rel, sizeof(xl_bmap_rel));
	XLogRegisterBuffer(0, headerbuf, REGBUF_STANDARD);
	XLogRegisterBufData(0, &delta, sizeof(delta));
	lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_TRUNCATE);
	PageSetLSN(BufferGetPage(headerbuf), lsn);
	END_CRIT_SECTION();
	LockBuffer(headerbuf, BUFFER_LOCK_UNLOCK);
}

/*
 * Free every physical block a dropped relation's map holds.  The records
 * name only the allocator's pages: a standby has removed the relation's map
 * by the time it replays them.  Runs after commit or abort, so it must not
 * fail: a problem is reported and the blocks are left allocated.
 */
static void
drop_blocks(SMgrRelation reln)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	ResourceOwner save = CurrentResourceOwner;
	ResourceOwner owner = ResourceOwnerCreate(NULL, "block map drop");
	MemoryContext oldcxt = CurrentMemoryContext;
	uint32		holdoff = InterruptHoldoffCount;

	CurrentResourceOwner = owner;
	PG_TRY();
	{
		BlockNumber mapnblocks = mdnblocks(reln, BMAP_FORKNUM);
		BlockNumber *blocks = palloc(BMAP_FANOUT * sizeof(BlockNumber));

		for (BlockNumber mapblk = 1; mapblk < mapnblocks; mapblk++)
		{
			Buffer		buf = map_read(rel, mapblk);
			BmapEntry  *entries;
			uint32		n = 0;

			LockBuffer(buf, BUFFER_LOCK_SHARE);
			check_page(BufferGetPage(buf), BMAP_PAGE_NODE, rel, BMAP_FORKNUM,
					   mapblk);
			entries = PageGetBmapEntries(BufferGetPage(buf));
			for (uint32 i = 0; i < BMAP_FANOUT; i++)
				if (BlockNumberIsValid(entries[i].block))
					blocks[n++] = entries[i].block;
			UnlockReleaseBuffer(buf);

#ifdef USE_ASSERT_CHECKING
			/* the relation's pages have left shared buffers (bmappredrop) */
			for (uint32 i = 0; i < BMAP_FANOUT; i++)
			{
				BlockNumber blk = (mapblk - 1) * BMAP_FANOUT + i;
				PrefetchBufferResult r = PrefetchSharedBuffer(reln, MAIN_FORKNUM, blk);

				Assert(!BufferIsValid(r.recent_buffer));
			}
#endif

			qsort(blocks, n, sizeof(BlockNumber), cmp_block);
			free_blocks(rel, blocks, n);
		}
		pfree(blocks);
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		/* an error resets the hold-off our caller set up */
		InterruptHoldoffCount = holdoff;
		MemoryContextSwitchTo(oldcxt);
		edata = CopyErrorData();
		FlushErrorState();
		ereport(WARNING,
				(errmsg("could not free the blocks of relation %s: %s",
						relpathperm(rel, MAIN_FORKNUM).str, edata->message)));
		FreeErrorData(edata);
		UnlockBuffers();
	}
	PG_END_TRY();
	ResourceOwnerRelease(owner, RESOURCE_RELEASE_BEFORE_LOCKS, false, true);
	ResourceOwnerRelease(owner, RESOURCE_RELEASE_LOCKS, false, true);
	ResourceOwnerRelease(owner, RESOURCE_RELEASE_AFTER_LOCKS, false, true);
	CurrentResourceOwner = save;
	ResourceOwnerDelete(owner);
}

/*
 * Remove a database's allocator and page store from the directory 'path'
 * of tablespace 'spcOid', if no block-mapped relation remains there (no map
 * forks).  Used before a tablespace is dropped, where the files would
 * otherwise keep the directory from being empty.
 */
void
bmap_drop_unused(Oid spcOid, Oid dbOid, const char *path)
{
	DIR		   *dir;
	struct dirent *de;
	bool		maps = false;
	char		suffix[16];
	size_t		suffixlen;
	RelFileLocator rel;
	SMgrRelation srels[2];

	snprintf(suffix, sizeof(suffix), "_%s", forkNames[BMAP_FORKNUM]);
	suffixlen = strlen(suffix);
	dir = AllocateDir(path);
	if (dir == NULL)
		return;
	while ((de = ReadDirExtended(dir, path, LOG)) != NULL)
	{
		const char *fork = strchr(de->d_name, '_');

		if (fork && strncmp(fork, suffix, suffixlen) == 0 &&
			(fork[suffixlen] == '\0' || fork[suffixlen] == '.'))
		{
			maps = true;
			break;
		}
	}
	FreeDir(dir);
	if (maps)
		return;

	rel.spcOid = spcOid;
	rel.dbOid = dbOid;
	rel.relNumber = InvalidRelFileNumber;
	srels[0] = alloc_smgr(rel);
	srels[1] = store_smgr(rel);
	if (!smgrexists(srels[0], MAIN_FORKNUM) &&
		!smgrexists(srels[1], MAIN_FORKNUM))
		return;
	/* at once, as in redo: nothing may use the tablespace any more */
	smgrdounlinkall(srels, 2, true);
}

/* ----------------------------------------------------------------
 * Storage manager callbacks
 * ----------------------------------------------------------------
 */

void
bmapinit(void)
{
}

bool
bmapclaims(RelFileLocatorBackend rlocator)
{
	if (rlocator.backend != INVALID_PROC_NUMBER)
		return false;
	if (rlocator.locator.dbOid == InvalidOid ||
		rlocator.locator.spcOid == GLOBALTABLESPACE_OID)
		return false;
	if (rlocator.locator.relNumber == BMAP_ALLOC_RELNUMBER ||
		rlocator.locator.relNumber == BMAP_STORE_RELNUMBER)
		return false;
	return bmap_relation_exists(rlocator.locator);
}

void
bmapopen(SMgrRelation reln)
{
	mdopen(reln);
	reln->bmap_pretrunc = InvalidBuffer;
	reln->bmap_read_blkno = InvalidBlockNumber;
}

void
bmapclose(SMgrRelation reln, ForkNumber forknum)
{
	if (forknum == MAIN_FORKNUM)
		reln->bmap_read_blkno = InvalidBlockNumber;
	mdclose(reln, forknum);
}

void
bmapcreate(SMgrRelation reln, ForkNumber forknum, bool isRedo)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		buf;
	Page		page;
	xl_bmap_header_delta delta;

	if (forknum != MAIN_FORKNUM)
	{
		mdcreate(reln, forknum, isRedo);
		return;
	}
	if (reln->smgr_rlocator.backend != INVALID_PROC_NUMBER)
		elog(ERROR, "block-mapped storage holds no temporary relations");

	/* in redo, the record that made the map came first */
	if (isRedo)
	{
		if (!mdexists(reln, BMAP_FORKNUM))
			elog(ERROR, "block-mapped relation %s has no map in recovery",
				 relpath(reln->smgr_rlocator, MAIN_FORKNUM).str);
		return;
	}
	if (mdexists(reln, BMAP_FORKNUM))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %s",
						relpath(reln->smgr_rlocator, BMAP_FORKNUM).str,
						"File exists")));

	ensure_alloc(rel);

	mdcreate(reln, BMAP_FORKNUM, false);
	buf = map_new_page(reln);
	if (BufferGetBlockNumber(buf) != 0)
		elog(ERROR, "map of \"%s\" begins at block %u",
			 relpath(reln->smgr_rlocator, MAIN_FORKNUM).str,
			 BufferGetBlockNumber(buf));
	page = BufferGetPage(buf);

	delta.kind = BMAP_DELTA_HEADER;
	delta.flags = BMAP_DELTA_INIT;
	delta.nblocks = 0;

	START_CRIT_SECTION();
	apply_header_delta(page, &delta);
	MarkBufferDirty(buf);
	XLogBeginInsert();
	XLogRegisterData(&rel, sizeof(xl_bmap_rel));
	XLogRegisterBuffer(0, buf, REGBUF_WILL_INIT | REGBUF_STANDARD);
	XLogRegisterBufData(0, &delta, sizeof(delta));
	PageSetLSN(page, XLogInsert(RM_BMAP_ID, XLOG_BMAP_CREATE));
	END_CRIT_SECTION();

	UnlockReleaseBuffer(buf);
}

/*
 * The map fork is the storage manager's own, so it is hidden from callers
 * that go through every fork (copying a relation, measuring it).
 */
bool
bmapexists(SMgrRelation reln, ForkNumber forknum)
{
	if (forknum == MAIN_FORKNUM)
		return mdexists(reln, BMAP_FORKNUM);
	if (forknum == BMAP_FORKNUM)
		return false;
	return mdexists(reln, forknum);
}

void
bmapunlink(RelFileLocatorBackend rlocator, ForkNumber forknum, bool isRedo)
{
	SMgrRelation reln;

	if (forknum == InvalidForkNumber)
	{
		for (ForkNumber f = MAIN_FORKNUM + 1; f <= MAX_FORKNUM; f++)
			if (f != BMAP_FORKNUM)
				mdunlink(rlocator, f, isRedo);
		bmapunlink(rlocator, MAIN_FORKNUM, isRedo);
		return;
	}
	if (forknum != MAIN_FORKNUM)
	{
		mdunlink(rlocator, forknum, isRedo);
		return;
	}

	if (isRedo)
	{
		/* the primary logged the frees; in recovery nothing reuses the name */
		mdunlink(rlocator, BMAP_FORKNUM, true);
		return;
	}

	reln = smgropen(rlocator.locator, INVALID_PROC_NUMBER);
	if (!mdexists(reln, BMAP_FORKNUM))
		return;

	/*
	 * Keep the map until after the next checkpoint, as md.c keeps a main
	 * fork's first segment: until then, its relation file number must not
	 * be used again, since crash recovery may still replay records for it.
	 */
	{
		FileTag		tag;

		memset(&tag, 0, sizeof(tag));
		tag.handler = SYNC_HANDLER_BMAP;
		tag.forknum = BMAP_FORKNUM;
		tag.rlocator = rlocator.locator;
		tag.segno = 0;
		RegisterSyncRequest(&tag, SYNC_UNLINK_REQUEST, true);
	}
}

/*
 * Before a relation's buffers are dropped to unlink it, free its blocks:
 * its map may be only in those buffers.  In redo, the primary's own records
 * do this.
 */
void
bmappredrop(SMgrRelation reln, bool isRedo)
{
	ForkNumber	fork = MAIN_FORKNUM;
	BlockNumber first = 0;

	if (isRedo || !mdexists(reln, BMAP_FORKNUM))
		return;

	/*
	 * The relation's pages leave shared buffers first: once a block is free,
	 * another relation may be given it, and a dirty page still cached here
	 * would be written onto it through this relation's map.  The map's own
	 * pages stay until the blocks are freed.
	 */
	DropRelationBuffers(reln, &fork, 1, &first);
	drop_blocks(reln);
}

void
bmapextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		   const void *buffer, bool skipFsync)
{
	if (forknum != MAIN_FORKNUM)
	{
		mdextend(reln, forknum, blocknum, buffer, skipFsync);
		return;
	}
	extend_to(reln, blocknum + 1, buffer, skipFsync);
}

void
bmapzeroextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			   int nblocks, bool skipFsync)
{
	if (forknum != MAIN_FORKNUM)
	{
		mdzeroextend(reln, forknum, blocknum, nblocks, skipFsync);
		return;
	}
	extend_to(reln, blocknum + nblocks, NULL, skipFsync);
}

/*
 * Allocate blocks before their first WAL record: callers that log a page's
 * image before writing it call this first, so that redo finds the block
 * already mapped.
 */
void
bmappreextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			  int nblocks)
{
	if (forknum != MAIN_FORKNUM)
		return;
	extend_to(reln, blocknum + nblocks, NULL, true);
}

bool
bmapprefetch(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			 int nblocks)
{
	SMgrRelation ssrel;

	if (forknum != MAIN_FORKNUM)
		return mdprefetch(reln, forknum, blocknum, nblocks);

	ssrel = store_smgr(reln->smgr_rlocator.locator);
	while (nblocks > 0)
	{
		BlockNumber run;
		BlockNumber phys = lookup_ext(reln, blocknum, nblocks, &run, true, false);

		if (!BlockNumberIsValid(phys))
			return false;
		if (!mdprefetch(ssrel, MAIN_FORKNUM, phys, run))
			return false;
		blocknum += run;
		nblocks -= run;
	}
	return true;
}

uint32
bmapmaxcombine(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum)
{
	BlockNumber run;
	BlockNumber phys;

	if (forknum != MAIN_FORKNUM)
		return mdmaxcombine(reln, forknum, blocknum);

	phys = lookup_ext(reln, blocknum, PG_UINT32_MAX, &run, true, false);
	if (!BlockNumberIsValid(phys))
		return 1;
	return Min(run, mdmaxcombine(store_smgr(reln->smgr_rlocator.locator),
								 MAIN_FORKNUM, phys));
}

void
bmapreadv(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		  void **buffers, BlockNumber nblocks)
{
	SMgrRelation ssrel;

	if (forknum != MAIN_FORKNUM)
	{
		mdreadv(reln, forknum, blocknum, buffers, nblocks);
		return;
	}
	ssrel = store_smgr(reln->smgr_rlocator.locator);
	while (nblocks > 0)
	{
		BlockNumber run;
		BlockNumber phys = lookup(reln, blocknum, nblocks, &run);

		mdreadv(ssrel, MAIN_FORKNUM, phys, buffers, run);
		blocknum += run;
		buffers += run;
		nblocks -= run;
	}
}

/*
 * Look up an asynchronous read's blocks before its I/O handle is acquired:
 * reading the map may need a handle of its own.
 */
void
bmapprepareread(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
				BlockNumber nblocks)
{
	if (forknum != MAIN_FORKNUM)
		return;
	reln->bmap_read_phys = lookup(reln, blocknum, nblocks,
								  &reln->bmap_read_run);
	reln->bmap_read_blkno = blocknum;
}

void
bmapstartreadv(PgAioHandle *ioh, SMgrRelation reln, ForkNumber forknum,
			   BlockNumber blocknum, void **buffers, BlockNumber nblocks)
{
	BlockNumber phys;

	if (forknum != MAIN_FORKNUM)
	{
		mdstartreadv(ioh, reln, forknum, blocknum, buffers, nblocks);
		return;
	}

	/*
	 * smgrprepareread() looked the blocks up, and smgrmaxcombine() limited
	 * the read to a physically consecutive run.  The read is started on the
	 * page store itself, so the I/O's target needs no map to be reopened
	 * elsewhere.
	 */
	if (reln->bmap_read_blkno != blocknum)
		elog(ERROR, "read of block %u of %s was not prepared",
			 blocknum, relpath(reln->smgr_rlocator, MAIN_FORKNUM).str);
	phys = reln->bmap_read_phys;
	reln->bmap_read_blkno = InvalidBlockNumber;
	if (reln->bmap_read_run < nblocks)
		elog(ERROR, "read of %u blocks at block %u of %s crosses a gap in its block map",
			 nblocks, blocknum, relpath(reln->smgr_rlocator, MAIN_FORKNUM).str);
	mdstartreadv(ioh, store_smgr(reln->smgr_rlocator.locator), MAIN_FORKNUM,
				 phys, buffers, nblocks);
}

void
bmapwritev(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		   const void **buffers, BlockNumber nblocks, bool skipFsync)
{
	SMgrRelation ssrel;

	if (forknum != MAIN_FORKNUM)
	{
		mdwritev(reln, forknum, blocknum, buffers, nblocks, skipFsync);
		return;
	}
	ssrel = store_smgr(reln->smgr_rlocator.locator);
	while (nblocks > 0)
	{
		BlockNumber run;
		BlockNumber phys = lookup_ext(reln, blocknum, nblocks, &run, false,
									  true);

		mdwritev(ssrel, MAIN_FORKNUM, phys, buffers, run, skipFsync);
		blocknum += run;
		buffers += run;
		nblocks -= run;
	}
}

void
bmapwriteback(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			  BlockNumber nblocks)
{
	SMgrRelation ssrel;

	if (forknum != MAIN_FORKNUM)
	{
		mdwriteback(reln, forknum, blocknum, nblocks);
		return;
	}
	ssrel = store_smgr(reln->smgr_rlocator.locator);
	while (nblocks > 0)
	{
		BlockNumber run;
		BlockNumber phys = lookup_ext(reln, blocknum, nblocks, &run, true,
									  true);

		/* writeback is a hint; blocks without an entry are skipped */
		if (!BlockNumberIsValid(phys))
			return;
		mdwriteback(ssrel, MAIN_FORKNUM, phys, run);
		blocknum += run;
		nblocks -= run;
	}
}

BlockNumber
bmapnblocks(SMgrRelation reln, ForkNumber forknum)
{
	if (forknum != MAIN_FORKNUM)
		return mdnblocks(reln, forknum);
	return header_nblocks(reln);
}

/*
 * Truncation happens inside a critical section, where reading a buffer
 * isn't safe, so the header is pinned beforehand.
 */
void
bmappretruncate(SMgrRelation reln, ForkNumber forknum)
{
	if (forknum != MAIN_FORKNUM || InRecovery)
		return;
	if (BufferIsValid(reln->bmap_pretrunc))
		ReleaseBuffer(reln->bmap_pretrunc);
	reln->bmap_pretrunc = map_read(reln->smgr_rlocator.locator, 0);
}

void
bmaptruncate(SMgrRelation reln, ForkNumber forknum, BlockNumber curnblk,
			 BlockNumber nblocks)
{
	Buffer		headerbuf;

	if (forknum != MAIN_FORKNUM)
	{
		mdtruncate(reln, forknum, curnblk, nblocks);
		return;
	}

	/* in recovery, the primary's own truncation record follows this one */
	if (InRecovery)
		return;

	headerbuf = reln->bmap_pretrunc;
	if (!BufferIsValid(headerbuf))
		elog(ERROR, "block-mapped relation %s truncated without being prepared",
			 relpath(reln->smgr_rlocator, MAIN_FORKNUM).str);
	reln->bmap_pretrunc = InvalidBuffer;

	/*
	 * Blocks past the new length stay mapped; extending the relation again
	 * reuses them, and dropping it frees them.
	 */
	if (nblocks < curnblk)
		set_length(reln->smgr_rlocator.locator, headerbuf, nblocks);
	ReleaseBuffer(headerbuf);
}

void
bmapimmedsync(SMgrRelation reln, ForkNumber forknum)
{
	if (forknum != MAIN_FORKNUM)
	{
		mdimmedsync(reln, forknum);
		return;
	}
	mdimmedsync(store_smgr(reln->smgr_rlocator.locator), MAIN_FORKNUM);
	mdimmedsync(reln, BMAP_FORKNUM);
}

void
bmapregistersync(SMgrRelation reln, ForkNumber forknum)
{
	if (forknum != MAIN_FORKNUM)
	{
		mdregistersync(reln, forknum);
		return;
	}
	mdregistersync(store_smgr(reln->smgr_rlocator.locator), MAIN_FORKNUM);
	mdregistersync(reln, BMAP_FORKNUM);
}

int
bmapfd(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
	   uint32 *off)
{
	BlockNumber phys;

	if (forknum != MAIN_FORKNUM)
		return mdfd(reln, forknum, blocknum, off);
	phys = lookup(reln, blocknum, 1, NULL);
	return mdfd(store_smgr(reln->smgr_rlocator.locator), MAIN_FORKNUM, phys,
				off);
}

/* ----------------------------------------------------------------
 * Sync handler: maps removed after a checkpoint
 * ----------------------------------------------------------------
 */

int
bmapsyncfiletag(const FileTag *ftag, char *path)
{
	/* only unlink requests are queued with this handler */
	strlcpy(path, relpathperm(ftag->rlocator, BMAP_FORKNUM).str, MAXPGPATH);
	return 0;
}

int
bmapunlinkfiletag(const FileTag *ftag, char *path)
{
	RelPathStr	base = relpathperm(ftag->rlocator, BMAP_FORKNUM);
	int			ret;

	/* the first segment's result is the request's; later ones follow it */
	strlcpy(path, base.str, MAXPGPATH);
	ret = unlink(path);
	for (int segno = 1; ret == 0; segno++)
	{
		char		segpath[MAXPGPATH];

		snprintf(segpath, sizeof(segpath), "%s.%d", base.str, segno);
		if (unlink(segpath) < 0)
			break;
	}
	return ret;
}

bool
bmapfiletagmatches(const FileTag *ftag, const FileTag *candidate)
{
	/* used to forget a dropped database's requests */
	return ftag->rlocator.dbOid == candidate->rlocator.dbOid;
}

/* ----------------------------------------------------------------
 * Redo
 * ----------------------------------------------------------------
 */

static void
redo_block(XLogReaderState *record, uint8 block_id)
{
	Size		len;
	char	   *data = XLogRecGetBlockData(record, block_id, &len);
	char	   *copy;
	uint16		kind;
	uint16		flags;
	Buffer		buf;
	XLogRedoAction action;

	/* a full-page image leaves out the block's data, and needs none */
	if (data == NULL)
	{
		if (XLogReadBufferForRedo(record, block_id, &buf) != BLK_RESTORED)
			elog(PANIC, "block map record without its delta");
		UnlockReleaseBuffer(buf);
		return;
	}
	if (len < 2 * sizeof(uint16))
		elog(PANIC, "block map record with a short delta");
	/* the data isn't aligned */
	copy = palloc(len);
	memcpy(copy, data, len);
	memcpy(&kind, copy, sizeof(uint16));
	memcpy(&flags, copy + sizeof(uint16), sizeof(uint16));

	if (flags & BMAP_DELTA_INIT)
	{
		buf = XLogInitBufferForRedo(record, block_id);
		action = BLK_NEEDS_REDO;
	}
	else
		action = XLogReadBufferForRedo(record, block_id, &buf);

	if (action == BLK_NEEDS_REDO)
	{
		Page		page = BufferGetPage(buf);

		switch (kind)
		{
			case BMAP_DELTA_HEADER:
				apply_header_delta(page, (xl_bmap_header_delta *) copy);
				break;
			case BMAP_DELTA_NODE:
				apply_node_delta(page, (xl_bmap_node_delta *) copy);
				break;
			case BMAP_DELTA_BITS:
				apply_bits_delta(page, (xl_bmap_bits_delta *) copy);
				break;
			case BMAP_DELTA_META:
				apply_meta_delta(page, (xl_bmap_meta_delta *) copy);
				break;
			default:
				elog(PANIC, "unknown block map delta %u", kind);
		}
		PageSetLSN(page, record->EndRecPtr);
		MarkBufferDirty(buf);
	}
	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
	pfree(copy);
}

static void
forget_cached_size(RelFileLocator rel)
{
	SMgrRelation reln = smgropen(rel, INVALID_PROC_NUMBER);

	reln->smgr_cached_nblocks[MAIN_FORKNUM] = InvalidBlockNumber;
}

void
bmap_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_BMAP_INIT:
			{
				Buffer		buf = XLogInitBufferForRedo(record, 0);
				Page		page = BufferGetPage(buf);

				init_meta(page);
				PageSetLSN(page, record->EndRecPtr);
				MarkBufferDirty(buf);
				UnlockReleaseBuffer(buf);
				break;
			}
		case XLOG_BMAP_CREATE:
			{
				xl_bmap_rel *xlrec = (xl_bmap_rel *) XLogRecGetData(record);
				SMgrRelation reln;

				/* making the header creates the map fork */
				redo_block(record, 0);

				/* an earlier open of this relation may have chosen md.c */
				reln = smgropen(xlrec->locator, INVALID_PROC_NUMBER);
				smgrcreatewith(reln, SMGR_BMAP, MAIN_FORKNUM, true);
				break;
			}
		case XLOG_BMAP_EXTEND:
			{
				xl_bmap_extend *xlrec = (xl_bmap_extend *) XLogRecGetData(record);
				BlockNumber *reused = (BlockNumber *) ((char *) xlrec + SizeOfBmapExtend);

				prepare_store(xlrec->locator, xlrec->old_hw, xlrec->new_hw,
							  reused, xlrec->nreused,
							  (xlrec->flags & BMAP_EXTEND_SKIP_ZERO) != 0);
				for (uint8 id = 0; id <= XLogRecMaxBlockId(record); id++)
					redo_block(record, id);
				forget_cached_size(xlrec->locator);
				break;
			}
		case XLOG_BMAP_TRUNCATE:
			{
				xl_bmap_rel *xlrec = (xl_bmap_rel *) XLogRecGetData(record);

				redo_block(record, 0);
				forget_cached_size(xlrec->locator);
				break;
			}
		case XLOG_BMAP_FREE:
			for (uint8 id = 0; id <= XLogRecMaxBlockId(record); id++)
				redo_block(record, id);
			break;
		default:
			elog(PANIC, "bmap_redo: unknown op code %u", info);
	}
}

void
bmap_mask(char *pagedata, BlockNumber blkno)
{
	mask_page_lsn_and_checksum(pagedata);
}
