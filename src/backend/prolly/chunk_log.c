/*-------------------------------------------------------------------------
 *
 * chunk_log.c
 *	  The history store's bytes in pages of their own layout
 *	  (prolly/chunk_log.h), and the prolly_chunks table access method,
 *	  whose relations hold them.
 *
 * The access method exists so the pages are the log's: nothing reads them
 * as heap pages.  It creates, copies and truncates the relation's storage;
 * VACUUM and ANALYZE pass it by, a scan finds no tuples, and everything
 * else refuses, as does any rewrite of the relation (make_new_heap()),
 * which would copy no tuples and so leave the log empty.  pg_dump writes
 * the relation and none of its bytes, its table data being its tuples;
 * publications leave it out.  The log's changes are WAL-logged as records
 * of its own resource manager, ChunkLog (prolly/chunk_xlog.h).
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/chunk_log.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/bufmask.h"
#include "access/multixact.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "commands/vacuum.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "prolly/branch_catalog.h"
#include "prolly/chunk_index.h"
#include "prolly/chunk_log.h"
#include "prolly/chunk_region.h"
#include "prolly/chunk_xlog.h"
#include "storage/bufmgr.h"
#include "storage/bulk_write.h"
#include "storage/freespace.h"
#include "storage/lmgr.h"
#include "storage/sinval.h"
#include "storage/smgr.h"
#include "utils/fmgrprotos.h"
#include "utils/injection_point.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

/* ------------------------------------------------------------ the pages */

#define CHUNK_LOG_MAGIC		0x50434C47	/* "PCLG" */
#define CHUNK_LOG_VERSION	7

#define CHUNK_PAGE_META		1
#define CHUNK_PAGE_DATA		2
#define CHUNK_PAGE_MAP		3

#define META_BLOCK			0

/*
 * The log's space is handed out, taken back and compacted a segment at a
 * time: SEGMENT_PAGES data pages, aligned (data page d is in segment d /
 * SEGMENT_PAGES), a backend's region one segment or more.  Chunks written
 * in one go on across a segment's pages, so a segment filled is full
 * pages, its end alone part used; a segment is used again once every one
 * of its pages is free, and the free space map has segments alone (their
 * first pages), never a page by itself.  A map page's group is whole
 * segments.
 */
#define SEGMENT_PAGES		32
#define SEGMENT_BYTES		((uint64) SEGMENT_PAGES * AREA)

/* The special space of each of the log's pages. */
typedef struct ChunkPageOpaqueData
{
	uint32		magic;			/* CHUNK_LOG_MAGIC */
	uint16		kind;			/* CHUNK_PAGE_META, _DATA or _MAP */
	uint16		unused;
	uint32		dead;			/* a data page's bytes no chunk's any more */
} ChunkPageOpaqueData;

typedef ChunkPageOpaqueData *ChunkPageOpaque;

/* The meta page's contents. */
typedef struct ChunkLogMetaData
{
	uint32		version;		/* CHUNK_LOG_VERSION */
	BlockNumber next_block;		/* the first page no region has */
	uint64		stamp;			/* this storage's, made with it (an index
								 * of chunks in it names it) */
	uint32		generation;		/* one more at each tail cut */
	BlockNumber cut_from;		/* the last cut's first page cut, or
								 * InvalidBlockNumber */
	Oid			index;			/* the one chunk index naming its bytes, or
								 * InvalidOid */
} ChunkLogMetaData;

/*
 * The region a backend appends into, in the relation's rd_amcache, kept as
 * each page of an append is written (so an append an error stops leaves it
 * where its last page ends).  A relcache reset forgets it (the relation's
 * storage replaced, or any invalidation of the relation), as does a
 * truncation, and the rest of it stays unused.
 */
typedef struct ChunkLogRegion
{
	RelFileNumber relnumber;	/* the storage it is in */
	uint64		next;			/* where the next append begins */
	uint64		limit;			/* where the region ends */
	uint64		stamp;			/* the storage's stamp, if known, or 0 */
	uint32		generation;		/* the log's when the region was made */
	Oid			index;			/* the index the log is known bound to */
} ChunkLogRegion;

/* The bytes of the log a data page holds, after its header. */
#define AREA_START		SizeOfPageHeaderData
#define AREA			(BLCKSZ - SizeOfPageHeaderData - \
						 MAXALIGN(sizeof(ChunkPageOpaqueData)))

/*
 * The open-page map: a map page heads each group of MAP_BITS data pages,
 * a bit for each, set as the page is handed to a backend's region, and
 * cleared by VACUUM once it finds the page full (or makes it full: a page
 * no live backend's region has, which its backend, gone, or having
 * forgotten its region, will never let go of).  The pages from block 1
 * are a map page, its group's data pages, the next map page, and so on;
 * the log's bytes are on its data pages alone, in order.
 */
#define MAP_BITS		4096
#define MAP_GROUP		(MAP_BITS + 1)

/* data page d's block, the block of its group's map page, and its bit */
#define DATA_BLOCK64(d)	(1 + ((uint64) (d) / MAP_BITS) * MAP_GROUP + 1 + \
						 (uint64) (d) % MAP_BITS)
#define DATA_BLOCK(d)	((BlockNumber) DATA_BLOCK64(d))
#define MAP_BLOCK(d)	((BlockNumber) (1 + ((d) / MAP_BITS) * MAP_GROUP))
#define MAP_BIT(d)		((uint32) ((d) % MAP_BITS))
#define IS_MAP_BLOCK(blk) ((blk) != META_BLOCK && ((blk) - 1) % MAP_GROUP == 0)

#define LOC_BLOCK(loc)	DATA_BLOCK((loc) / AREA)
#define LOC_OFFSET(loc)	((size_t) ((loc) % AREA))

/* the data pages before block blk, and so where its bytes begin */
static inline uint64
pages_below(BlockNumber blk)
{
	uint64		b,
				in_group;

	if (blk <= META_BLOCK + 1)
		return 0;
	b = blk - 1;
	in_group = b % MAP_GROUP;
	return (b / MAP_GROUP) * MAP_BITS + (in_group > 0 ? in_group - 1 : 0);
}

#define BLOCK_LOC(blk)	(pages_below(blk) * AREA)

static void
init_page(Page page, uint16 kind)
{
	ChunkPageOpaque opaque;

	PageInit(page, BLCKSZ, sizeof(ChunkPageOpaqueData));
	opaque = (ChunkPageOpaque) PageGetSpecialPointer(page);
	opaque->magic = CHUNK_LOG_MAGIC;
	opaque->kind = kind;
	opaque->unused = 0;
	opaque->dead = 0;
}

/*
 * The changes the log makes to its pages, each WAL-logged as a record of
 * its own (prolly/chunk_xlog.h) and made again by its redo.
 */
static void
page_make(Page page, bool all_dead)
{
	init_page(page, CHUNK_PAGE_DATA);
	if (all_dead)
	{
		((PageHeader) page)->pd_lower = AREA_START + AREA;
		((ChunkPageOpaque) PageGetSpecialPointer(page))->dead = AREA;
	}
}

/*
 * A page part used, made full: the bytes from off on dead, those before it
 * not yet written reserved (an entry's, which counts them dead itself)
 */
static void
page_seal(Page page, bool init, size_t off)
{
	ChunkPageOpaque opaque;

	if (init)
		init_page(page, CHUNK_PAGE_DATA);
	opaque = (ChunkPageOpaque) PageGetSpecialPointer(page);
	opaque->dead += AREA - off;
	((PageHeader) page)->pd_lower = AREA_START + AREA;
}

/*
 * Bytes written at off: past where the page is written, the bytes between
 * are reserved and never written (an entry's, counting them dead itself)
 */
static void
page_write(Page page, bool init, size_t off, const char *bytes, size_t n)
{
	if (init)
		init_page(page, CHUNK_PAGE_DATA);
	memcpy((char *) page + AREA_START + off, bytes, n);
	((PageHeader) page)->pd_lower = AREA_START + off + n;
}

/*
 * Bytes at off counted dead: reserved and never written, the page is
 * written to their end
 */
static void
page_add_dead(Page page, bool init, size_t off, size_t n)
{
	if (init)
		init_page(page, CHUNK_PAGE_DATA);
	if (((PageHeader) page)->pd_lower < AREA_START + off + n)
		((PageHeader) page)->pd_lower = AREA_START + off + n;
	((ChunkPageOpaque) PageGetSpecialPointer(page))->dead += n;
}

/* Bits first..first+n-1 of a map page set, or cleared. */
static void
page_map_set(Page page, bool init, uint32 first, uint32 n, bool set)
{
	uint8	   *bits;

	if (init)
	{
		init_page(page, CHUNK_PAGE_MAP);
		((PageHeader) page)->pd_lower = MAXALIGN(SizeOfPageHeaderData) + MAP_BITS / 8;
		memset(PageGetContents(page), 0, MAP_BITS / 8);
	}
	bits = (uint8 *) PageGetContents(page);
	for (uint32 i = first; i < first + n; i++)
	{
		if (set)
			bits[i / 8] |= (uint8) (1 << (i % 8));
		else
			bits[i / 8] &= (uint8) ~(1 << (i % 8));
	}
}

/*
 * WAL-logs the change just made to buf's page, in the critical section
 * that made it: rec, the record's, and data, block 0's (or none)
 */
static void
log_page_change(Relation rel, Buffer buf, uint8 info, bool will_init,
				const void *rec, size_t reclen, const void *data, size_t len)
{
	XLogRecPtr	lsn;

	MarkBufferDirty(buf);
	if (!RelationNeedsWAL(rel))
		return;
	XLogBeginInsert();
	XLogRegisterData(rec, reclen);
	XLogRegisterBuffer(0, buf, REGBUF_STANDARD | (will_init ? REGBUF_WILL_INIT : 0));
	if (len > 0)
		XLogRegisterBufData(0, data, len);
	lsn = XLogInsert(RM_CHUNK_LOG_ID, info);
	PageSetLSN(BufferGetPage(buf), lsn);
}

/* a page whose every byte was written, and is dead: free to use again */
static inline bool
page_free(Page page)
{
	return ((ChunkPageOpaque) PageGetSpecialPointer(page))->kind == CHUNK_PAGE_DATA &&
		((PageHeader) page)->pd_lower == AREA_START + AREA &&
		((ChunkPageOpaque) PageGetSpecialPointer(page))->dead == AREA;
}

/* a page made afresh to use again, nothing on it yet */
static inline bool
page_fresh(Page page)
{
	return !PageIsNew(page) &&
		PageGetSpecialSize(page) == MAXALIGN(sizeof(ChunkPageOpaqueData)) &&
		((ChunkPageOpaque) PageGetSpecialPointer(page))->kind == CHUNK_PAGE_DATA &&
		((PageHeader) page)->pd_lower == AREA_START &&
		((ChunkPageOpaque) PageGetSpecialPointer(page))->dead == 0;
}

static void
check_page(Relation rel, BlockNumber blk, Page page, uint16 kind)
{
	ChunkPageOpaque opaque;

	if (PageIsNew(page) ||
		PageGetSpecialSize(page) != MAXALIGN(sizeof(ChunkPageOpaqueData)))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("block %u of \"%s\" is not a page of a chunk log",
						blk, RelationGetRelationName(rel))));
	opaque = (ChunkPageOpaque) PageGetSpecialPointer(page);
	if (opaque->magic != CHUNK_LOG_MAGIC || opaque->kind != kind)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("block %u of \"%s\" is not a %s page of a chunk log",
						blk, RelationGetRelationName(rel),
						kind == CHUNK_PAGE_META ? "meta" :
						kind == CHUNK_PAGE_MAP ? "map" : "data")));
}

static ChunkLogMetaData *
page_meta(Relation rel, Page page)
{
	ChunkLogMetaData *meta;

	check_page(rel, META_BLOCK, page, CHUNK_PAGE_META);
	meta = (ChunkLogMetaData *) PageGetContents(page);
	if (meta->version != CHUNK_LOG_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("chunk log \"%s\" has format version %u, not %u",
						RelationGetRelationName(rel), meta->version,
						CHUNK_LOG_VERSION)));
	return meta;
}

/*
 * A new log's meta page, in a page buffer: made with its storage, and after
 * a truncation, so that no append ever makes it (two backends doing so at
 * once could both claim the first region).
 */
static void
init_meta(Page page)
{
	ChunkLogMetaData *meta;

	init_page(page, CHUNK_PAGE_META);
	meta = (ChunkLogMetaData *) PageGetContents(page);
	meta->version = CHUNK_LOG_VERSION;
	meta->next_block = META_BLOCK + 1;
	meta->generation = 1;
	meta->cut_from = InvalidBlockNumber;
	meta->index = InvalidOid;
	if (!pg_strong_random(&meta->stamp, sizeof(meta->stamp)))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not make a chunk log's stamp")));
	((PageHeader) page)->pd_lower =
		(char *) meta + sizeof(ChunkLogMetaData) - (char *) page;
}

static void
write_meta(SMgrRelation srel, bool use_wal)
{
	BulkWriteState *bulk = smgr_bulk_start_smgr(srel, MAIN_FORKNUM, use_wal);
	BulkWriteBuffer buf = smgr_bulk_get_buf(bulk);

	init_meta((Page) buf);
	smgr_bulk_write(bulk, META_BLOCK, buf, true);
	smgr_bulk_finish(bulk);
}

/*
 * Data page blk, locked exclusively; the relation extended to it if needed
 * (another backend may extend the relation past blk meanwhile, with zero
 * pages, or make blk itself: a page of a region, written by its backend,
 * and by VACUUM where bytes reserved in it were never written).
 */
static Buffer
data_buffer(Relation rel, BlockNumber blk)
{
	Buffer		buf;

	Assert(blk != META_BLOCK);
	if (blk < RelationGetNumberOfBlocks(rel))
	{
		buf = ReadBuffer(rel, blk);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		return buf;
	}
	/*
	 * (read, not made zero, if another made it meanwhile: VACUUM counting
	 * bytes reserved and never written dead extends the log too, as their
	 * region's backend may at once)
	 */
	buf = ExtendBufferedRelTo(BMR_REL(rel), MAIN_FORKNUM, NULL, 0, blk + 1,
							  RBM_NORMAL);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	return buf;
}

/* ------------------------------------------------------------- the log */

/*
 * The meta page's next block (and, at a tail cut, its generation and where
 * the cut began), mbuf locked exclusively
 */
static void
meta_set(ChunkLogMetaData *meta, const xl_chunk_log_next_block *x)
{
	meta->next_block = x->next_block;
	meta->generation = x->generation;
	meta->cut_from = x->cut_from;
}

static void
set_next_block(Relation rel, Buffer mbuf, BlockNumber next_block, bool cut)
{
	ChunkLogMetaData *meta = (ChunkLogMetaData *) PageGetContents(BufferGetPage(mbuf));
	xl_chunk_log_next_block xlrec = {
		.next_block = next_block,
		.generation = meta->generation + (cut ? 1 : 0),
		.cut_from = cut ? next_block : meta->cut_from,
	};

	START_CRIT_SECTION();
	meta_set(meta, &xlrec);
	log_page_change(rel, mbuf, XLOG_CHUNK_LOG_NEXT_BLOCK, false,
					&xlrec, sizeof(xlrec), NULL, 0);
	END_CRIT_SECTION();
}

/* the log's generation, and where its last tail cut began */
static uint32
log_generation(Relation rel, BlockNumber *cut_from)
{
	Buffer		mbuf = ReadBuffer(rel, META_BLOCK);
	ChunkLogMetaData *meta;
	uint32		gen;

	LockBuffer(mbuf, BUFFER_LOCK_SHARE);
	meta = page_meta(rel, BufferGetPage(mbuf));
	gen = meta->generation;
	if (cut_from)
		*cut_from = meta->cut_from;
	UnlockReleaseBuffer(mbuf);
	return gen;
}

/* a data page made afresh: empty, or written full with every byte dead */
static void
make_page(Relation rel, Buffer buf, bool all_dead)
{
	xl_chunk_log_init_page xlrec = {.all_dead = all_dead};

	START_CRIT_SECTION();
	page_make(BufferGetPage(buf), all_dead);
	log_page_change(rel, buf, XLOG_CHUNK_LOG_INIT_PAGE, true,
					&xlrec, sizeof(xlrec), NULL, 0);
	END_CRIT_SECTION();
}

/*
 * Data pages first..first+n-1 marked open in their groups' map pages (each
 * made, if new); or, set false, page first marked so no more.
 */
static void
map_mark(Relation rel, uint64 first, uint64 n, bool set)
{
	while (n > 0)
	{
		uint32		bit = MAP_BIT(first);
		uint32		k = (uint32) Min(n, (uint64) (MAP_BITS - bit));
		Buffer		buf = data_buffer(rel, MAP_BLOCK(first));
		Page		page = BufferGetPage(buf);
		bool		init = PageIsNew(page);
		xl_chunk_log_map xlrec = {.first = bit, .n = k, .set = set, .init = init};

		if (!init)
			check_page(rel, MAP_BLOCK(first), page, CHUNK_PAGE_MAP);
		START_CRIT_SECTION();
		page_map_set(page, init, bit, k, set);
		log_page_change(rel, buf, XLOG_CHUNK_LOG_MAP, init,
						&xlrec, sizeof(xlrec), NULL, 0);
		END_CRIT_SECTION();
		UnlockReleaseBuffer(buf);
		first += k;
		n -= k;
	}
}

static BlockNumber region_from(const ChunkLogRegion *region);

/*
 * Claims npages new data pages for this backend's appends, from the meta
 * page: the one place appends meet, for one short record per claim.  The
 * pages are this backend's region's (published) and marked open before
 * they are the log's: VACUUM finds none of them unheld and not open.
 * Answers the first.  With after, the region they are to go on from: only
 * if it ends where the claim begins (published from region_from on), and
 * -1, nothing claimed, if it does not.
 */
static int64
claim_pages(Relation rel, uint64 npages, const ChunkLogRegion *after)
{
	Buffer		mbuf;
	ChunkLogMetaData *meta;
	uint64		first;

	/*
	 * (the map pages the claim's pages are in, in the file first: a map page
	 * new to it is made with the meta page locked, but the file is not
	 * extended so, which would stall every other claim)
	 */
	for (;;)
	{
		BlockNumber map_end;

		mbuf = ReadBuffer(rel, META_BLOCK);
		LockBuffer(mbuf, BUFFER_LOCK_EXCLUSIVE);
		meta = page_meta(rel, BufferGetPage(mbuf));
		first = pages_below(meta->next_block);
		Assert(first % SEGMENT_PAGES == 0);
		if (npages > (uint64) MaxBlockNumber ||
			DATA_BLOCK64(first + npages - 1) >= (uint64) MaxBlockNumber)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("chunk log \"%s\" is full", RelationGetRelationName(rel))));
		map_end = MAP_BLOCK(first + npages - 1) + 1;
		if (map_end <= RelationGetNumberOfBlocks(rel))
			break;
		UnlockReleaseBuffer(mbuf);
		ReleaseBuffer(ExtendBufferedRelTo(BMR_REL(rel), MAIN_FORKNUM, NULL, 0,
										  map_end, RBM_NORMAL));
	}
	if (after != NULL)
	{
		BlockNumber from = region_from(after);

		if (after->limit != first * AREA || after->generation != meta->generation)
		{
			UnlockReleaseBuffer(mbuf);
			return -1;
		}
		ChunkRegionPublish(rel->rd_locator, RelationGetRelid(rel), meta->generation,
						   from, DATA_BLOCK(first + npages - 1) + 1);
	}
	else
		ChunkRegionPublish(rel->rd_locator, RelationGetRelid(rel), meta->generation,
						   DATA_BLOCK(first), DATA_BLOCK(first + npages - 1) + 1);
	map_mark(rel, first, npages, true);
	set_next_block(rel, mbuf, DATA_BLOCK(first + npages - 1) + 1, false);
	UnlockReleaseBuffer(mbuf);
	return (int64) first;
}

/* the data pages the log's claims have reached (its meta page's next one) */
static uint64
claimed_pages(Relation rel)
{
	Buffer		buf = ReadBuffer(rel, META_BLOCK);
	uint64		d;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	d = pages_below(page_meta(rel, BufferGetPage(buf))->next_block);
	UnlockReleaseBuffer(buf);
	return d;
}

/* whether data page d's bit in the open-page map is set */
static bool
map_bit(Relation rel, uint64 d)
{
	BlockNumber blk = MAP_BLOCK(d);
	Buffer		buf;
	bool		set = false;

	if (blk >= RelationGetNumberOfBlocks(rel))
		return false;
	buf = ReadBuffer(rel, blk);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	if (!PageIsNew(BufferGetPage(buf)))
		set = (((uint8 *) PageGetContents(BufferGetPage(buf)))[MAP_BIT(d) / 8] &
			   (1 << (MAP_BIT(d) % 8))) != 0;
	UnlockReleaseBuffer(buf);
	return set;
}

/*
 * Whether data page d, its page given (locked), is unused: free (every
 * byte written and dead), or never written though below where the log's
 * claims have reached (claimed) and in no region, its open-page bit clear
 * (a page a crash left between a tail cut and the meta page's next one
 * set after it, never to be claimed again).
 */
static bool
page_unused(Relation rel, Page page, uint64 d, uint64 claimed)
{
	if (!PageIsNew(page))
		return page_free(page);
	return d < claimed && !map_bit(rel, d);
}

/*
 * A segment the free space map has, wholly before data page below if that
 * is not 0, for prepare to take: its first data page, or -1 if none is
 * found in a few tries.  (A hint: take_free_segment checks it.)
 */
static int64
free_segment_hint(Relation rel, uint64 below)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);
	BlockNumber passed = InvalidBlockNumber;
	int			npassed = 0;

	for (int tries = 0; tries < 8; tries++)
	{
		BlockNumber blk = GetPageWithFreeSpace(rel, AREA / 2);
		uint64		first;

		if (!BlockNumberIsValid(blk) || blk == META_BLOCK || blk >= nblocks)
			return -1;
		if (IS_MAP_BLOCK(blk))
		{
			RecordPageWithFreeSpace(rel, blk, 0);
			continue;
		}
		first = pages_below(blk) / SEGMENT_PAGES * SEGMENT_PAGES;
		/* (one past where room may be passed by, as take_free_segment does) */
		if (below > 0 && first + SEGMENT_PAGES > below)
		{
			if (blk == passed || ++npassed > 4096)
				return -1;
			if (!BlockNumberIsValid(passed))
				passed = blk;
			tries--;
			continue;
		}
		return (int64) first;
	}
	return -1;
}

/*
 * A segment the free space map has as free (every byte of every page
 * dead), made afresh for this backend's appends, and below the data page
 * below if that is not 0; its first data page, or -1 if none is found.
 * The map is a hint, not WAL-logged: a segment it has wrongly is passed by,
 * and taken out of it.  The segment's pages are locked in order, each
 * checked free, published as this backend's region's, marked open, and
 * made afresh.
 */
static int64
take_free_segment(Relation rel, uint32 generation, uint64 below, int64 hint)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);
	uint64		claimed = claimed_pages(rel);
	BlockNumber passed = InvalidBlockNumber;
	int			npassed = 0;

	for (int tries = 0; tries < 8; tries++)
	{
		Buffer		bufs[SEGMENT_PAGES];
		uint64		first;
		BlockNumber blk;
		int			locked = 0;
		bool		all_free = true;

		/* (the hint first, once, then what the map finds) */
		if (hint >= 0)
		{
			first = (uint64) hint;
			blk = DATA_BLOCK(first);
			hint = -1;
		}
		else
		{
			/*
			 * (the map keeps space in steps, a whole page rounding past what
			 * a page can have: half of one finds the segments recorded free)
			 */
			blk = GetPageWithFreeSpace(rel, AREA / 2);
			if (!BlockNumberIsValid(blk) || blk == META_BLOCK || blk >= nblocks)
				return -1;
			if (IS_MAP_BLOCK(blk))
			{
				RecordPageWithFreeSpace(rel, blk, 0);
				continue;
			}
			first = pages_below(blk) / SEGMENT_PAGES * SEGMENT_PAGES;
		}
		/*
		 * (one past where room may be passed by, not counted a try: others
		 * may be under it; until the map comes round to the first passed)
		 */
		if (below > 0 && first + SEGMENT_PAGES > below)
		{
			if (blk == passed || ++npassed > 4096)
				return -1;
			if (!BlockNumberIsValid(passed))
				passed = blk;
			tries--;
			continue;
		}
		for (int j = 0; j < SEGMENT_PAGES; j++)
		{
			BlockNumber b = DATA_BLOCK(first + j);

			if (b >= nblocks)
			{
				all_free = false;
				break;
			}
			bufs[j] = ReadBuffer(rel, b);
			LockBuffer(bufs[j], BUFFER_LOCK_EXCLUSIVE);
			locked++;
			if (!page_unused(rel, BufferGetPage(bufs[j]), first + j, claimed))
			{
				all_free = false;
				break;
			}
		}
		if (!all_free)
		{
			for (int j = 0; j < locked; j++)
				UnlockReleaseBuffer(bufs[j]);
			/* (the map wrong of it: out, once nothing is locked) */
			RecordPageWithFreeSpace(rel, blk, 0);
			if (blk != DATA_BLOCK(first))
				RecordPageWithFreeSpace(rel, DATA_BLOCK(first), 0);
			continue;
		}

		/* (this backend's region's, and open, before they are made to use) */
		ChunkRegionPublish(rel->rd_locator, RelationGetRelid(rel), generation,
						   DATA_BLOCK(first), DATA_BLOCK(first + SEGMENT_PAGES - 1) + 1);
		map_mark(rel, first, SEGMENT_PAGES, true);
		for (int j = 0; j < SEGMENT_PAGES; j++)
			make_page(rel, bufs[j], false);
		for (int j = 0; j < SEGMENT_PAGES; j++)
			UnlockReleaseBuffer(bufs[j]);
		/* (the map told, once nothing is locked: it may extend itself) */
		RecordPageWithFreeSpace(rel, DATA_BLOCK(first), 0);
		return (int64) first;
	}
	return -1;
}

/*
 * Where a region grown is published from: the segment its next append goes
 * in (every byte before it written: those segments VACUUM may take for
 * done with), or, for a pack's whose bytes reserved before may not be
 * written yet, which VACUUM must not take for a region let go of, the
 * region's first page still (ChunkRegionMineFirst).
 */
static BlockNumber
region_from(const ChunkLogRegion *region)
{
	uint64		d = (region->next < region->limit ? region->next : region->limit - 1) / AREA;

	return DATA_BLOCK(d / SEGMENT_PAGES * SEGMENT_PAGES);
}

/*
 * The region grown over the segment just after it, if every page of it is
 * unused (free, or a crash's hole): its pages locked in order, each
 * checked, published as this backend's region's (from the block from on:
 * see region_from) and marked open, then made afresh.  False, nothing
 * done, if not, or if the region would reach below (a data page, if not
 * 0).  (Called again as long as the bytes need, for a chunk longer than a
 * segment.)
 */
static bool
extend_region(Relation rel, ChunkLogRegion *region, uint64 below,
			  BlockNumber from)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);
	uint64		first = region->limit / AREA;
	uint64		claimed = claimed_pages(rel);
	Buffer		bufs[SEGMENT_PAGES];
	int			locked = 0;

	if (region->limit % SEGMENT_BYTES != 0 || !BlockNumberIsValid(from) ||
		(below > 0 && first + SEGMENT_PAGES > below))
		return false;
	for (int j = 0; j < SEGMENT_PAGES; j++)
	{
		BlockNumber b = DATA_BLOCK(first + j);

		if (b >= nblocks)
			break;
		bufs[j] = ReadBuffer(rel, b);
		LockBuffer(bufs[j], BUFFER_LOCK_EXCLUSIVE);
		locked++;
		if (!page_unused(rel, BufferGetPage(bufs[j]), first + j, claimed))
		{
			locked = -locked;
			break;
		}
	}
	if (locked != SEGMENT_PAGES)
	{
		for (int j = 0; j < (locked < 0 ? -locked : locked); j++)
			UnlockReleaseBuffer(bufs[j]);
		return false;
	}
	ChunkRegionPublish(rel->rd_locator, RelationGetRelid(rel), region->generation,
					   from, DATA_BLOCK(first + SEGMENT_PAGES - 1) + 1);
	map_mark(rel, first, SEGMENT_PAGES, true);
	for (int j = 0; j < SEGMENT_PAGES; j++)
		make_page(rel, bufs[j], false);
	for (int j = 0; j < SEGMENT_PAGES; j++)
		UnlockReleaseBuffer(bufs[j]);
	RecordPageWithFreeSpace(rel, DATA_BLOCK(first), 0);
	region->limit += SEGMENT_BYTES;
	return true;
}

/*
 * Lets go of what is left of a region, from loc to limit: the page loc is
 * on, written part way, is made full, its rest dead (a page part written
 * would never be free, and its backend appends to it no more); and each
 * page after it that the file has, never written, is made a page all
 * dead.  Either may then be used again.
 */
static void
seal_region(Relation rel, uint64 loc, uint64 limit)
{
	if (loc >= limit)
		return;
	if (LOC_OFFSET(loc) > 0)
	{
		BlockNumber blk = LOC_BLOCK(loc);
		size_t		off = LOC_OFFSET(loc);
		Buffer		buf = data_buffer(rel, blk);
		bool		init = PageIsNew(BufferGetPage(buf));
		xl_chunk_log_seal xlrec = {.off = off, .init = init};

		/*
		 * (written as far as the region's bytes, or short of it: those
		 * between reserved, never written; nothing past it)
		 */
		if (!init)
		{
			check_page(rel, blk, BufferGetPage(buf), CHUNK_PAGE_DATA);
			if (((PageHeader) BufferGetPage(buf))->pd_lower > AREA_START + off)
			{
				UnlockReleaseBuffer(buf);
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("block %u of chunk log \"%s\" is written past where its region's bytes end",
								blk, RelationGetRelationName(rel))));
			}
		}
		START_CRIT_SECTION();
		page_seal(BufferGetPage(buf), init, off);
		log_page_change(rel, buf, XLOG_CHUNK_LOG_SEAL_PAGE, init,
						&xlrec, sizeof(xlrec), NULL, 0);
		END_CRIT_SECTION();
		UnlockReleaseBuffer(buf);
		loc += AREA - off;
	}
	for (uint64 d = loc / AREA;
		 loc < limit && DATA_BLOCK(d) < RelationGetNumberOfBlocks(rel);
		 d++, loc += AREA)
	{
		BlockNumber blk = DATA_BLOCK(d);
		Buffer		buf = ReadBuffer(rel, blk);

		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

		/*
		 * (a page never written: new, or made afresh for this region, from
		 * pages freed; another, written, is not this region's to let go of)
		 */
		if (!PageIsNew(BufferGetPage(buf)) && !page_fresh(BufferGetPage(buf)))
		{
			UnlockReleaseBuffer(buf);
			continue;
		}
		make_page(rel, buf, true);
		UnlockReleaseBuffer(buf);
	}
}

/* pages enough for len bytes, from the first: whole segments */
static void
claim_run(Relation rel, uint64 len, uint64 npages, ChunkLogRegion *run)
{
	uint64		first;

	npages = Max(npages, (len + AREA - 1) / AREA);
	npages = (npages + SEGMENT_PAGES - 1) / SEGMENT_PAGES * SEGMENT_PAGES;
	first = (uint64) claim_pages(rel, npages, NULL);
	run->relnumber = rel->rd_locator.relNumber;
	run->next = first * AREA;
	run->limit = run->next + (uint64) npages * AREA;
}

static void
keep_region(Relation rel, const ChunkLogRegion *region)
{
	ChunkLogRegion *cached = rel->rd_amcache;
	uint64		stamp = 0;
	Oid			index = InvalidOid;

	if (cached == NULL)
		cached = rel->rd_amcache = MemoryContextAllocZero(CacheMemoryContext,
														  sizeof(ChunkLogRegion));
	else if (cached->relnumber == region->relnumber)
	{
		stamp = cached->stamp;
		index = cached->index;
	}
	*cached = *region;
	if (cached->stamp == 0)
		cached->stamp = stamp;
	if (!OidIsValid(cached->index))
		cached->index = index;
	/* (the region kept again, should an invalidation have forgotten it) */
	if (region->limit > 0)
		ChunkRegionKept(rel->rd_locator);
}

/*
 * Binds the log to the one chunk index naming its bytes, or checks it is
 * bound to it: a tail cut is safe only made by the VACUUM of every index
 * naming the log's bytes, which takes out every aborted entry it finds
 * before it cuts (prolly/chunk_log.h's mark_dead)
 */
static void
page_bind_index(Relation rel, Oid index)
{
	ChunkLogRegion *cached = rel->rd_amcache;
	Buffer		mbuf;
	ChunkLogMetaData *meta;

	if (cached != NULL && cached->relnumber == rel->rd_locator.relNumber &&
		cached->index == index)
		return;
	mbuf = ReadBuffer(rel, META_BLOCK);
	LockBuffer(mbuf, BUFFER_LOCK_EXCLUSIVE);
	meta = page_meta(rel, BufferGetPage(mbuf));
	if (OidIsValid(meta->index) && meta->index != index)
	{
		UnlockReleaseBuffer(mbuf);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("chunk log \"%s\" is another chunk index's",
						RelationGetRelationName(rel)),
				 errdetail("A chunk log's bytes are named by one chunk index.")));
	}
	if (!OidIsValid(meta->index))
	{
		xl_chunk_log_bind_index xlrec = {.index = index};

		START_CRIT_SECTION();
		meta->index = index;
		log_page_change(rel, mbuf, XLOG_CHUNK_LOG_BIND_INDEX, false,
						&xlrec, sizeof(xlrec), NULL, 0);
		END_CRIT_SECTION();
	}
	UnlockReleaseBuffer(mbuf);
	if (cached == NULL)
		cached = rel->rd_amcache = MemoryContextAllocZero(CacheMemoryContext,
														  sizeof(ChunkLogRegion));
	if (cached->relnumber != rel->rd_locator.relNumber)
	{
		memset(cached, 0, sizeof(ChunkLogRegion));
		cached->relnumber = rel->rd_locator.relNumber;
	}
	cached->index = index;
}

/*
 * The checks of every write to the log: by a transaction with an id (its
 * commit flushes the WAL of its bytes; one without would commit with no
 * record), before any buffer is touched, as getting one can fail; and
 * under RowExclusiveLock, which keeps VACUUM from cutting the log's tail
 * off meanwhile
 */
static void
check_writer(Relation rel)
{
	PreventCommandIfReadOnly("append to a chunk log");
	(void) GetCurrentTransactionId();
	if (!CheckRelationLockedByMe(rel, RowExclusiveLock, true))
		elog(ERROR, "chunk log \"%s\" appended to without RowExclusiveLock",
			 RelationGetRelationName(rel));
}

/*
 * Room for len bytes in this backend's region: what is left of it, grown
 * over the segments just after it if they are unused, or else a new one:
 * a segment free (and as many unused after it as the bytes need), or new
 * pages, whole segments (the region going on into them if it ends where
 * they begin, else what was left of it let go of).  Unless may_grow, no new
 * pages; with below (a data page, not 0), room wholly before it alone.
 * False, the region as it is in *run (for release), if there is no room
 * so.  The region, kept in the relation's rd_amcache too, in *run.
 */
static bool
prepare_run(Relation rel, size_t len, ChunkLogRun *run, bool may_grow,
			uint64 below)
{
	ChunkLogRegion *cached = rel->rd_amcache;
	ChunkLogRegion region;

	check_writer(rel);

	/*
	 * (until the region's release, a region an invalidation has this
	 * backend forget is let go of only if not kept again)
	 */
	ChunkRegionAppending(true);
	if (cached != NULL && cached->relnumber == rel->rd_locator.relNumber)
		region = *cached;
	else
	{
		memset(&region, 0, sizeof(region));
		region.relnumber = rel->rd_locator.relNumber;
	}

	/*
	 * (appends go only in the region this backend's entry names: one whose
	 * entry went, to make room or as a transaction ended once it was
	 * forgotten, is VACUUM's to let go of)
	 */
	if (region.limit > 0 &&
		!ChunkRegionMine(rel->rd_locator, LOC_BLOCK(region.limit - 1) + 1))
	{
		region.next = region.limit = 0;
		if (cached != NULL && cached->relnumber == region.relnumber)
			cached->next = cached->limit = 0;
	}

	/*
	 * (a region grown over the unused segments just after it, as far as the
	 * bytes need: chunks go on across them as across pages new, where a
	 * new region would leave what was left of this one dead)
	 */
	while (region.limit > 0 && region.limit - region.next < len &&
		   (below == 0 || region.limit <= below * AREA) &&
		   extend_region(rel, &region, below, region_from(&region)))
		keep_region(rel, &region);

	/*
	 * (a region reaching below is let go of, for one under it: what is
	 * moved there is to leave what is above)
	 */
	if (region.limit - region.next < len ||
		(below > 0 && region.limit > below * AREA))
	{
		uint64		npages = Max((len + AREA - 1) / AREA, 1);
		int64		hint = -1;
		int64		first;
		uint32		generation;

		/*
		 * (a segment free looked for first; for a chunk longer than a
		 * segment, only if the log is not to grow: one longer goes on pages
		 * new alone, unless segments free lie one after another for it)
		 */
		if (npages <= SEGMENT_PAGES || !may_grow || below > 0)
			hint = free_segment_hint(rel, below);
		if (hint < 0)
		{
			/* (no segment free and the log not to grow: no room) */
			if (!may_grow || below > 0)
			{
				run->relnumber = region.relnumber;
				run->next = region.next;
				run->limit = region.limit;
				run->generation = region.generation;
				return false;
			}

			/*
			 * (pages new: the region going on into them if it ends where
			 * they begin, nothing of it left dead)
			 */
			if (region.limit > 0 && region.limit - region.next < len)
			{
				uint64		more = ((len - (region.limit - region.next) + AREA - 1) / AREA +
									SEGMENT_PAGES - 1) / SEGMENT_PAGES * SEGMENT_PAGES;

				if (claim_pages(rel, more, &region) >= 0)
				{
					region.limit += more * AREA;
					keep_region(rel, &region);
					goto done;
				}
			}
		}

		/*
		 * What is left of the region let go of, forgotten first: should what
		 * follows fail (a claim, an I/O), no put reserves in a region sealed
		 */
		if (cached != NULL && cached->relnumber == region.relnumber)
			cached->next = cached->limit = 0;
		if (region.limit > 0)
			seal_region(rel, region.next, region.limit);
		region.next = region.limit = 0;
		INJECTION_POINT("chunk-log-region-sealed", NULL);

		/*
		 * (its bytes the generation's: a tail cut, which makes the next,
		 * needs the relation locked as no put holds it, and its
		 * invalidation has every backend forget its region)
		 */
		generation = log_generation(rel, NULL);

		/*
		 * (a hint the map has wrongly, and none free after all, leaves the
		 * region let go of here though it could have gone on into pages
		 * new: it is let go of before what replaces it is made the
		 * backend's, and the segment found is locked only then, in order)
		 */
		first = hint >= 0 ? take_free_segment(rel, generation, below, hint) : -1;
		if (first >= 0)
		{
			region.relnumber = rel->rd_locator.relNumber;
			region.next = (uint64) first * AREA;
			region.limit = region.next + SEGMENT_BYTES;
			region.generation = generation;
			keep_region(rel, &region);
			while (region.limit - region.next < len &&
				   extend_region(rel, &region, below, region_from(&region)))
				keep_region(rel, &region);
			/* (not room enough: the segments taken kept, for the next) */
			if (region.limit - region.next < len)
			{
				run->relnumber = region.relnumber;
				run->next = region.next;
				run->limit = region.limit;
				run->generation = region.generation;
				return false;
			}
		}
		else if (may_grow && below == 0)
		{
			claim_run(rel, len, SEGMENT_PAGES, &region);
			region.generation = generation;
			keep_region(rel, &region);
		}
		else
		{
			/* (none free after all, and the log not to grow: no room) */
			run->relnumber = region.relnumber;
			run->next = run->limit = 0;
			run->generation = generation;
			return false;
		}
	}
done:
	run->relnumber = region.relnumber;
	run->next = region.next;
	run->limit = region.limit;
	run->generation = region.generation;
	return true;
}

static void
page_prepare(Relation rel, size_t len, ChunkLogRun *run)
{
	(void) prepare_run(rel, len, run, true, 0);
}

static bool
page_prepare_free(Relation rel, size_t len, uint64 below, ChunkLogRun *run)
{
	return prepare_run(rel, len, run, false, below / AREA);
}

/*
 * len bytes of the room prepared, where they begin: the run, and the
 * region kept, past them.  Changes nothing but memory: it does not fail.
 */
static uint64
page_reserve(Relation rel, ChunkLogRun *run, size_t len)
{
	ChunkLogRegion *cached = rel->rd_amcache;
	uint64		loc = run->next;

	if (len > run->limit - run->next)
		elog(PANIC, "chunk log \"%s\": %zu bytes reserved past the room prepared",
			 RelationGetRelationName(rel), len);
	run->next += len;

	/*
	 * (the region, if still kept, past them: should the bytes never be
	 * written, the region goes on after them, the entry naming them
	 * counting them dead; one forgotten meanwhile, by an invalidation, is
	 * kept again once the bytes are written)
	 */
	if (cached != NULL && cached->relnumber == run->relnumber &&
		cached->limit == run->limit)
		cached->next = run->next;
	return loc;
}

/*
 * Writes len bytes reserved at loc, a page at a time, each page's change a
 * record of its own.  A page is written in its reserved bytes' order; one
 * written short of loc has the bytes between reserved, never written.
 */
static void
page_write_at(Relation rel, uint64 loc, const void *data, size_t len)
{
	const char *bytes = data;

	check_writer(rel);
	while (len > 0)
	{
		BlockNumber blk = LOC_BLOCK(loc);
		size_t		off = LOC_OFFSET(loc),
					n = Min(len, AREA - off);
		Buffer		buf;
		Page		page;
		bool		init;
		xl_chunk_log_append xlrec;

		CHECK_FOR_INTERRUPTS();
		buf = data_buffer(rel, blk);
		page = BufferGetPage(buf);
		init = PageIsNew(page);

		/* (a page made, or the log's, written no further than loc) */
		if (!init)
		{
			check_page(rel, blk, page, CHUNK_PAGE_DATA);
			if (((PageHeader) page)->pd_lower > AREA_START + off)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("block %u of chunk log \"%s\" is written past %llu, where bytes reserved go",
								blk, RelationGetRelationName(rel),
								(unsigned long long) loc)));
		}
		xlrec.off = off;
		xlrec.len = n;
		xlrec.init = init;
		START_CRIT_SECTION();
		page_write(page, init, off, bytes, n);
		log_page_change(rel, buf, XLOG_CHUNK_LOG_APPEND, init,
						&xlrec, sizeof(xlrec), bytes, n);
		END_CRIT_SECTION();
		UnlockReleaseBuffer(buf);
		loc += n;
		bytes += n;
		len -= n;

		INJECTION_POINT("chunk-log-append-page", NULL);
	}
}

static bool
page_extend(Relation rel, ChunkLogRun *run, size_t len)
{
	ChunkLogRegion region = {
		.relnumber = run->relnumber,
		.next = run->next,
		.limit = run->limit,
		.generation = run->generation,
	};

	check_writer(rel);
	if (run->relnumber != rel->rd_locator.relNumber)
		return run->limit - run->next >= len;
	while (region.limit - region.next < len &&
		   BlockNumberIsValid(ChunkRegionMineFirst(rel->rd_locator)) &&
		   extend_region(rel, &region, 0, ChunkRegionMineFirst(rel->rd_locator)))
	{
		keep_region(rel, &region);
		run->limit = region.limit;
		INJECTION_POINT("chunk-log-extended", NULL);
	}
	return run->limit - run->next >= len;
}

/* The region kept once more, should an invalidation have forgotten it. */
static void
page_release(Relation rel, const ChunkLogRun *run)
{
	ChunkLogRegion region = {
		.relnumber = run->relnumber,
		.next = run->next,
		.limit = run->limit,
		.generation = run->generation,
	};

	if (run->relnumber == rel->rd_locator.relNumber)
		keep_region(rel, &region);
	ChunkRegionAppending(false);
}

static uint64
page_append(Relation rel, const void *data, size_t len)
{
	ChunkLogRun run;
	uint64		loc;

	check_writer(rel);
	if (len == 0)
		return 0;
	page_prepare(rel, len, &run);
	loc = page_reserve(rel, &run, len);
	page_write_at(rel, loc, data, len);
	page_release(rel, &run);
	return loc;
}

static uint64
page_end(Relation rel)
{
	Buffer		buf;
	uint64		end;

	buf = ReadBuffer(rel, META_BLOCK);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	end = BLOCK_LOC(page_meta(rel, BufferGetPage(buf))->next_block);
	UnlockReleaseBuffer(buf);
	return end;
}

static void
page_read(Relation rel, uint64 loc, void *dst, size_t len)
{
	char	   *out = dst;
	uint64		end = page_end(rel);

	if (loc > end || len > end - loc)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("bytes %llu to %llu are past the end of chunk log \"%s\" (%llu)",
						(unsigned long long) loc,
						(unsigned long long) (loc + len),
						RelationGetRelationName(rel),
						(unsigned long long) end)));
	while (len > 0)
	{
		BlockNumber blk = LOC_BLOCK(loc);
		size_t		off = LOC_OFFSET(loc),
					n = Min(len, AREA - off);
		Buffer		buf;
		Page		page;

		/* (a region's pages are made as its appends reach them) */
		if (blk >= RelationGetNumberOfBlocks(rel))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("bytes %llu to %llu of chunk log \"%s\" were never written",
							(unsigned long long) loc,
							(unsigned long long) (loc + len),
							RelationGetRelationName(rel))));
		buf = ReadBuffer(rel, blk);
		page = BufferGetPage(buf);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		if (!PageIsNew(page))
			check_page(rel, blk, page, CHUNK_PAGE_DATA);
		if (PageIsNew(page) ||
			AREA_START + off + n > ((PageHeader) page)->pd_lower)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("bytes %llu to %llu of chunk log \"%s\" were never written",
							(unsigned long long) loc,
							(unsigned long long) (loc + len),
							RelationGetRelationName(rel))));
		memcpy(out, (char *) page + AREA_START + off, n);
		UnlockReleaseBuffer(buf);
		out += n;
		loc += n;
		len -= n;
	}
}

/*
 * Counts len bytes from loc dead, a page at a time; a segment all of whose
 * pages' bytes are dead then goes in the free space map, as VACUUM ends
 * (ChunkLogRecordFree), to be used again.
 */
static void
page_mark_dead(Relation rel, uint64 loc, size_t len, uint32 generation)
{
	BlockNumber cut_from;
	uint32		current = log_generation(rel, &cut_from);

	/*
	 * Bytes of a generation before the last tail cut's: those on pages the
	 * cut took are gone, nothing to count.  Only bytes reserved and never
	 * written can have been on them (a page with bytes written and not yet
	 * dead is no free tail), an aborted entry's; and that entry is out
	 * before another cut, VACUUM taking out every aborted entry it finds
	 * before it cuts, with no put running.  Bytes written, of any
	 * generation, are on pages before every cut since.
	 */
	if (generation > current)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("chunk log \"%s\" has no bytes of generation %u (it is at %u)",
						RelationGetRelationName(rel), generation, current)));
	while (len > 0)
	{
		BlockNumber blk = LOC_BLOCK(loc);
		size_t		off = LOC_OFFSET(loc),
					n = Min(len, AREA - off);
		Buffer		buf;
		Page		page;
		bool		init;
		xl_chunk_log_mark_dead xlrec;
		size_t		written;

		CHECK_FOR_INTERRUPTS();
		if (generation != current && BlockNumberIsValid(cut_from) && blk >= cut_from)
		{
			loc += n;
			len -= n;
			continue;
		}

		/*
		 * (bytes reserved and never written may be on a page not made yet,
		 * or past the file's end: an aborted entry's, its region's later
		 * bytes only after them)
		 */
		buf = data_buffer(rel, blk);
		page = BufferGetPage(buf);
		init = PageIsNew(page);
		if (!init)
			check_page(rel, blk, page, CHUNK_PAGE_DATA);
		written = init ? 0 : ((PageHeader) page)->pd_lower - AREA_START;
		if ((init ? 0 : ((ChunkPageOpaque) PageGetSpecialPointer(page))->dead) + n >
			Max(written, off + n))
		{
			UnlockReleaseBuffer(buf);
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("block %u of chunk log \"%s\" would have more bytes dead than written",
							blk, RelationGetRelationName(rel))));
		}
		xlrec.off = off;
		xlrec.len = n;
		xlrec.init = init;
		START_CRIT_SECTION();
		page_add_dead(page, init, off, n);
		log_page_change(rel, buf, XLOG_CHUNK_LOG_MARK_DEAD, init,
						&xlrec, sizeof(xlrec), NULL, 0);
		END_CRIT_SECTION();
		UnlockReleaseBuffer(buf);
		loc += n;
		len -= n;
	}
}

/*
 * VACUUM lets go of what is left of the regions no live backend holds: the
 * pages marked open that are not full, held by no live backend's region as
 * its pass over the index begins (ChunkLogSweepBegin), and not changed
 * from then to its end (ChunkLogSweepEnd), are made full (their rest dead,
 * or, never written, all dead).  Every byte reserved and never written on
 * such a page is an entry's put before the pass began, when the page's
 * region was held (no region but a page's own backend's reserves on it,
 * and a region begins on pages new or free alone), whose transaction ended
 * before the region went (an exit ends it first; a backend lets a region
 * go only once its bytes reserved there are written, or their put
 * failed): the pass finds it aborted, takes it out, and counts its bytes
 * dead, changing the page, which is then left for the next VACUUM.  A page
 * held as the pass began, or changed since, may have such bytes of an
 * entry the pass did not see, and is left too.  Pages marked open and
 * found full are marked so no more.
 */
typedef struct ChunkLogSweep
{
	XLogRecPtr	start;			/* a page changed since has an LSN past it */
	uint32		generation;
	int			n;
	int			cap;
	uint64	   *pages;			/* data pages to look at again */
	bool	   *candidate;		/* ... and whether to make it full */
} ChunkLogSweep;

static void
sweep_add(ChunkLogSweep *sweep, uint64 d, bool candidate)
{
	if (sweep->n == sweep->cap)
	{
		sweep->cap = Max(64, sweep->cap * 2);
		sweep->pages = sweep->pages ?
			repalloc(sweep->pages, sweep->cap * sizeof(uint64)) :
			palloc(sweep->cap * sizeof(uint64));
		sweep->candidate = sweep->candidate ?
			repalloc(sweep->candidate, sweep->cap * sizeof(bool)) :
			palloc(sweep->cap * sizeof(bool));
	}
	sweep->pages[sweep->n] = d;
	sweep->candidate[sweep->n] = candidate;
	sweep->n++;
}

/* the map pages' bits cleared a batch at a time, the pages pinned first */
#define CLEAR_BATCH		64

/*
 * Pages pages[0..n-1] of one group, those found full (done[i]) marked so no
 * more if still full: their map page locked once, and logged whole, the
 * pages pinned before it is locked and each, then, locked if it can be
 * without waiting (a backend taking a page free locks it, then the map
 * page: a page it is taking is left marked, as it is to be).
 */
static void
map_clear_full(Relation rel, const uint64 *pages, const bool *done, int n)
{
	Buffer		bufs[CLEAR_BATCH];
	bool		locked[CLEAR_BATCH];
	bool		any = false;
	Buffer		mbuf;
	uint8	   *bits;
	bool		changed = false;

	for (int i = 0; i < n; i++)
	{
		bufs[i] = done[i] ? ReadBuffer(rel, DATA_BLOCK(pages[i])) : InvalidBuffer;
		any |= done[i];
	}
	if (!any)
		return;
	mbuf = ReadBuffer(rel, MAP_BLOCK(pages[0]));
	LockBuffer(mbuf, BUFFER_LOCK_EXCLUSIVE);
	check_page(rel, MAP_BLOCK(pages[0]), BufferGetPage(mbuf), CHUNK_PAGE_MAP);
	bits = (uint8 *) PageGetContents(BufferGetPage(mbuf));

	/* (the pages locked, those that can be, before the change is made) */
	for (int i = 0; i < n; i++)
		locked[i] = BufferIsValid(bufs[i]) && ConditionalLockBufferShared(bufs[i]);
	START_CRIT_SECTION();
	for (int i = 0; i < n; i++)
	{
		uint32		bit = MAP_BIT(pages[i]);
		Page		page;

		if (!locked[i])
			continue;
		page = BufferGetPage(bufs[i]);
		if (!PageIsNew(page) && ((PageHeader) page)->pd_lower == AREA_START + AREA &&
			(bits[bit / 8] & (1 << (bit % 8))))
		{
			bits[bit / 8] &= (uint8) ~(1 << (bit % 8));
			changed = true;
		}
	}
	if (changed)
	{
		MarkBufferDirty(mbuf);
		if (RelationNeedsWAL(rel))
			log_newpage_buffer(mbuf, true);
	}
	END_CRIT_SECTION();
	for (int i = 0; i < n; i++)
		if (locked[i])
			LockBuffer(bufs[i], BUFFER_LOCK_UNLOCK);
	UnlockReleaseBuffer(mbuf);
	for (int i = 0; i < n; i++)
		if (BufferIsValid(bufs[i]))
			ReleaseBuffer(bufs[i]);
}

uint32
ChunkLogGeneration(Relation rel)
{
	return log_generation(rel, NULL);
}

ChunkLogSweep *
ChunkLogSweepBegin(Relation rel)
{
	ChunkLogSweep *sweep = palloc0(sizeof(ChunkLogSweep));
	BlockNumber nblocks;
	Buffer		mbuf;
	ChunkLogMetaData *meta;
	uint64		npages;
	ChunkRegionSet *live;

	/*
	 * (the WAL's end first: a change after it has a later LSN.  Then the
	 * log's pages, and then the regions held: a region published since has
	 * pages past those, or a page free, full)
	 */
	sweep->start = GetXLogInsertRecPtr();
	mbuf = ReadBuffer(rel, META_BLOCK);
	LockBuffer(mbuf, BUFFER_LOCK_SHARE);
	meta = page_meta(rel, BufferGetPage(mbuf));
	npages = pages_below(meta->next_block);
	sweep->generation = meta->generation;
	UnlockReleaseBuffer(mbuf);
	live = ChunkRegionLive(rel->rd_locator, sweep->generation);
	INJECTION_POINT("chunk-log-sweep-begin", NULL);
	nblocks = RelationGetNumberOfBlocks(rel);

	for (uint64 group = 0; group * MAP_BITS < npages; group++)
	{
		BlockNumber mblk = MAP_BLOCK(group * MAP_BITS);
		uint8		bits[MAP_BITS / 8];
		Buffer		buf;

		if (mblk >= nblocks)
			break;
		buf = ReadBuffer(rel, mblk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		if (PageIsNew(BufferGetPage(buf)))
		{
			UnlockReleaseBuffer(buf);
			continue;
		}
		check_page(rel, mblk, BufferGetPage(buf), CHUNK_PAGE_MAP);
		memcpy(bits, PageGetContents(BufferGetPage(buf)), sizeof(bits));
		UnlockReleaseBuffer(buf);

		for (uint32 bit = 0; bit < MAP_BITS; bit++)
		{
			uint64		d = group * MAP_BITS + bit;
			BlockNumber blk = DATA_BLOCK(d);
			Page		page;
			bool		full;

			if (d >= npages || blk >= nblocks)
				break;
			if (!(bits[bit / 8] & (1 << (bit % 8))))
				continue;
			CHECK_FOR_INTERRUPTS();
			buf = ReadBuffer(rel, blk);
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buf);
			if (!PageIsNew(page))
				check_page(rel, blk, page, CHUNK_PAGE_DATA);
			full = !PageIsNew(page) && ((PageHeader) page)->pd_lower == AREA_START + AREA;
			UnlockReleaseBuffer(buf);
			if (full)
				sweep_add(sweep, d, false);	/* (to mark so no more) */
			else if (!ChunkRegionSetHas(live, blk))
				sweep_add(sweep, d, true);
		}
	}
	pfree(live->first);
	pfree(live->end);
	pfree(live);
	return sweep;
}

void
ChunkLogSweepEnd(Relation rel, ChunkLogSweep *sweep)
{
	/*
	 * (the regions held now, once: one published since on a page found has
	 * made it afresh, a page free, its LSN then past the pass's beginning)
	 */
	ChunkRegionSet *live = ChunkRegionLive(rel->rd_locator, sweep->generation);

	for (int k = 0; k < sweep->n; k++)
	{
		uint64		d = sweep->pages[k];
		BlockNumber blk = DATA_BLOCK(d);
		Buffer		buf;
		Page		page;
		bool		done = false;

		CHECK_FOR_INTERRUPTS();
		buf = ReadBuffer(rel, blk);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
		if (!PageIsNew(page))
			check_page(rel, blk, page, CHUNK_PAGE_DATA);
		if (!PageIsNew(page) && ((PageHeader) page)->pd_lower == AREA_START + AREA)
			done = true;
		else if (sweep->candidate[k] && PageGetLSN(page) <= sweep->start &&
				 !ChunkRegionSetHas(live, blk))
		{
			if (PageIsNew(page))
				make_page(rel, buf, true);
			else
			{
				size_t		off = ((PageHeader) page)->pd_lower - AREA_START;
				xl_chunk_log_seal xlrec = {.off = off, .init = false};

				START_CRIT_SECTION();
				page_seal(page, false, off);
				log_page_change(rel, buf, XLOG_CHUNK_LOG_SEAL_PAGE, false,
								&xlrec, sizeof(xlrec), NULL, 0);
				END_CRIT_SECTION();
			}
			done = true;
		}
		sweep->candidate[k] = done;
		UnlockReleaseBuffer(buf);
	}

	pfree(live->first);
	pfree(live->end);
	pfree(live);

	/* the pages found full marked so no more, a batch at a time */
	for (int k = 0; k < sweep->n;)
	{
		uint64		group = sweep->pages[k] / MAP_BITS;
		int			to = k;

		while (to < sweep->n && to - k < CLEAR_BATCH &&
			   sweep->pages[to] / MAP_BITS == group)
			to++;
		map_clear_full(rel, sweep->pages + k, sweep->candidate + k, to - k);
		k = to;
	}
	if (sweep->pages)
	{
		pfree(sweep->pages);
		pfree(sweep->candidate);
	}
	pfree(sweep);
}

bool
ChunkLogCheckLive(Relation rel, int n, const uint64 *locs, const uint32 *lens,
				  char **why)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);
	uint64		npages = pages_below(nblocks);
	uint32	   *live = MemoryContextAllocHuge(CurrentMemoryContext,
											  Max(npages, 1) * sizeof(uint32));

	memset(live, 0, Max(npages, 1) * sizeof(uint32));
	*why = NULL;
	for (int i = 0; i < n; i++)
	{
		uint64		loc = locs[i];
		uint64		len = lens[i];

		while (len > 0)
		{
			uint64		d = loc / AREA;
			uint64		k = Min(len, AREA - LOC_OFFSET(loc));

			if (d >= npages)
			{
				*why = psprintf("bytes at %llu of chunk log \"%s\" are past its end",
								(unsigned long long) loc, RelationGetRelationName(rel));
				pfree(live);
				return false;
			}
			live[d] += (uint32) k;
			loc += k;
			len -= k;
		}
	}
	for (uint64 d = 0; d < npages && *why == NULL; d++)
	{
		BlockNumber blk = DATA_BLOCK(d);
		Buffer		buf = ReadBuffer(rel, blk);
		Page		page = BufferGetPage(buf);
		uint32		written = 0,
					dead = 0;

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		if (!PageIsNew(page))
		{
			check_page(rel, blk, page, CHUNK_PAGE_DATA);
			written = ((PageHeader) page)->pd_lower - AREA_START;
			dead = ((ChunkPageOpaque) PageGetSpecialPointer(page))->dead;
		}
		UnlockReleaseBuffer(buf);
		if ((uint64) live[d] + dead > written)
			*why = psprintf("block %u of chunk log \"%s\" has %u bytes of live chunks, %u dead, and %u written",
							blk, RelationGetRelationName(rel), live[d], dead, written);
		CHECK_FOR_INTERRUPTS();
	}
	pfree(live);
	return *why == NULL;
}

void
ChunkLogSpace(Relation rel, uint32 *pages, uint32 *full, uint64 *dead,
			  uint32 *free_pages)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);

	*pages = *full = *free_pages = 0;
	*dead = 0;
	for (BlockNumber blk = META_BLOCK + 1; blk < nblocks; blk++)
	{
		Buffer		buf;
		Page		page;

		if (IS_MAP_BLOCK(blk))
			continue;
		buf = ReadBuffer(rel, blk);
		page = BufferGetPage(buf);

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		if (!PageIsNew(page))
		{
			(*pages)++;
			if (((PageHeader) page)->pd_lower == AREA_START + AREA)
				(*full)++;
			*dead += ((ChunkPageOpaque) PageGetSpecialPointer(page))->dead;
			if (page_free(page))
				(*free_pages)++;
		}
		UnlockReleaseBuffer(buf);
	}
}

static uint64
page_stamp(Relation rel)
{
	ChunkLogRegion *cached = rel->rd_amcache;
	Buffer		buf;
	uint64		stamp;

	/* (kept with the backend's region: every put asks) */
	if (cached != NULL && cached->relnumber == rel->rd_locator.relNumber &&
		cached->stamp != 0)
		return cached->stamp;
	buf = ReadBuffer(rel, META_BLOCK);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	stamp = page_meta(rel, BufferGetPage(buf))->stamp;
	UnlockReleaseBuffer(buf);
	if (rel->rd_amcache == NULL)
	{
		ChunkLogRegion none = {0};

		none.relnumber = rel->rd_locator.relNumber;
		keep_region(rel, &none);
	}
	cached = rel->rd_amcache;
	if (cached->relnumber == rel->rd_locator.relNumber)
		cached->stamp = stamp;
	return stamp;
}

/*
 * What a segment of the log is: its pages' dead bytes; whether it is done
 * with, every page it has written full and none in a live backend's region
 * (the end of the last may be past the file's, never written); free; and
 * whether chunks freed with bytes on it are kept for snapshots still.
 */
typedef struct SegmentState
{
	uint64		dead;
	uint32		pages;			/* the file has */
	bool		written;
	bool		free;			/* every byte of every page dead */
	bool		pending;		/* bytes of chunks freed, dead only later */
} SegmentState;

/*
 * each segment's state, read a page at a time (O(log), through a ring of
 * buffers of its own), those with chunks freed kept for snapshots marked
 * (pending, a bit a segment, of the first npending); how many
 */
static uint32
scan_segments(Relation rel, const uint8 *pending, uint64 npending,
			  SegmentState **out)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);
	uint64		npages = pages_below(nblocks);
	uint64		claimed = claimed_pages(rel);
	uint32		nseg = (uint32) ((npages + SEGMENT_PAGES - 1) / SEGMENT_PAGES);
	SegmentState *st = palloc0_array(SegmentState, Max(nseg, 1));
	ChunkRegionSet *live = ChunkRegionLive(rel->rd_locator, ChunkLogGeneration(rel));
	BufferAccessStrategy strategy = GetAccessStrategy(BAS_BULKREAD);

	for (uint32 g = 0; g < nseg; g++)
	{
		st[g].written = st[g].free = true;
		/* (a region is whole segments: its first page tells) */
		if (ChunkRegionSetHas(live, DATA_BLOCK((uint64) g * SEGMENT_PAGES)))
			st[g].written = false;
		for (int j = 0; j < SEGMENT_PAGES; j++)
		{
			uint64		d = (uint64) g * SEGMENT_PAGES + j;
			Buffer		buf;
			Page		page;

			if (d >= npages || DATA_BLOCK(d) >= nblocks)
			{
				/* (past the file's end: never written, nor free) */
				st[g].free = false;
				break;
			}
			st[g].pages++;
			buf = ReadBufferExtended(rel, MAIN_FORKNUM, DATA_BLOCK(d), RBM_NORMAL,
									 strategy);
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buf);
			if (PageIsNew(page))
			{
				/* (a crash's hole: no region's, never to be: all dead) */
				if (st[g].written && page_unused(rel, page, d, claimed))
					st[g].dead += AREA;
				else
					st[g].written = st[g].free = false;
			}
			else
			{
				if (((PageHeader) page)->pd_lower != AREA_START + AREA)
					st[g].written = false;
				if (!page_free(page))
					st[g].free = false;
				st[g].dead += ((ChunkPageOpaque) PageGetSpecialPointer(page))->dead;
			}
			UnlockReleaseBuffer(buf);
		}
		CHECK_FOR_INTERRUPTS();
	}
	/*
	 * (segments past those marked, the log grown meanwhile, taken for
	 * pending: chunks put there and freed may be kept too)
	 */
	for (uint64 g = 0; pending != NULL && g < nseg; g++)
		if (g >= npending || (pending[g / 8] & (1 << (g % 8))))
			st[g].pending = true;
	FreeAccessStrategy(strategy);
	*out = st;
	return nseg;
}

/*
 * a segment to compact: done with, fraction of the bytes of the pages it
 * has dead, not all, and none of its chunks freed kept for snapshots still
 * (their bytes dead later; its chunks moved already, maybe, the entries
 * left for those snapshots)
 */
static inline bool
segment_compactable(const SegmentState *st, double fraction)
{
	return st->written && !st->free && !st->pending && st->dead > 0 &&
		st->dead >= fraction * st->pages * AREA;
}

/*
 * The segments at the log's end to evacuate, their live chunks moved into
 * the free segments below them (so that, once VACUUM takes out the entries
 * moved, the tail is free and cut off): from the last segment written down
 * (the free tail passed by), as long as their live bytes fit in nine
 * tenths of the free segments below the lowest (each one's end part
 * unused); the lowest, or nseg if none.  Stops at a segment not done with
 * (a region a backend appends to, an idle one's too, until it exits or
 * forgets it), or with chunks freed kept for snapshots: no end beyond it is
 * cut until they are gone.
 */
static uint32
evacuable_from(const SegmentState *st, uint32 nseg)
{
	uint32	   *free_below = palloc_array(uint32, nseg + 1);
	uint32		from = nseg;
	uint64		live = 0;
	int64		g;

	free_below[0] = 0;
	for (uint32 k = 0; k < nseg; k++)
		free_below[k + 1] = free_below[k] + (st[k].free ? 1 : 0);
	for (g = (int64) nseg - 1; g >= 0 && st[g].free; g--)
		;
	for (; g >= 0; g--)
	{
		if (st[g].free)
			continue;
		if (!st[g].written || st[g].pending)
			break;
		live += (uint64) st[g].pages * AREA - st[g].dead;
		if (live > (uint64) free_below[g] * SEGMENT_BYTES / 10 * 9)
			break;
		from = (uint32) g;
	}
	pfree(free_below);
	return from;
}

/* what is found of the segments, the free ones put in the map if record */
static void
find_segments(Relation rel, double fraction, const uint8 *pending,
			  uint64 npending, bool record, ChunkLogSegments *out)
{
	SegmentState *st;
	uint32		nseg = scan_segments(rel, pending, npending, &st);
	uint32		from = evacuable_from(st, nseg);

	memset(out, 0, sizeof(*out));
	out->segments = nseg;
	for (uint32 g = 0; g < nseg; g++)
	{
		if (st[g].free)
		{
			/* (a segment free: its first page alone in the map, for whole) */
			out->free++;
			if (record)
				RecordPageWithFreeSpace(rel, DATA_BLOCK((uint64) g * SEGMENT_PAGES), AREA);
		}
		if (st[g].pending && st[g].written && !st[g].free)
			out->pending++;
		if (segment_compactable(&st[g], fraction))
		{
			out->compactable++;
			out->compactable_dead += st[g].dead;
		}
		if (g >= from && !st[g].free)
			out->evacuable++;
	}
	if (record)
		FreeSpaceMapVacuum(rel);

	/*
	 * (to compact only if the dead bytes come to a segment's: fewer gain
	 * nothing, what is moved off them filling a segment part way; the log
	 * grown by a segment if none is free, to move into)
	 */
	out->worth = out->compactable_dead >= SEGMENT_BYTES;
	out->grow = out->free > 0 ? 0 : SEGMENT_PAGES;
	pfree(st);
}

void
ChunkLogRecordFree(Relation rel, double fraction, const uint8 *pending,
				   uint64 npending, ChunkLogSegments *out)
{
	find_segments(rel, fraction, pending, npending, true, out);
}

static void
page_find_segments(Relation rel, double fraction, ChunkLogSegments *out)
{
	find_segments(rel, fraction, NULL, 0, false, out);
}

static int
page_compactable_segments(Relation rel, double fraction, int max,
						  uint64 *starts, uint64 *ends)
{
	SegmentState *st;
	uint32		nseg = scan_segments(rel, NULL, 0, &st);
	int			n = 0;

	for (uint32 g = 0; g < nseg && n < max; g++)
		if (segment_compactable(&st[g], fraction))
		{
			starts[n] = (uint64) g * SEGMENT_BYTES;
			ends[n] = starts[n] + SEGMENT_BYTES;
			n++;
		}
	pfree(st);
	return n;
}

/*
 * (the highest up to max, from the end down, given ascending: the end
 * evacuated first, which is what lets the log be cut)
 */
static int
page_evacuable_segments(Relation rel, int max, uint64 *starts, uint64 *ends)
{
	SegmentState *st;
	uint32		nseg = scan_segments(rel, NULL, 0, &st);
	uint32		from = evacuable_from(st, nseg);
	int			n = 0;

	for (int64 g = (int64) nseg - 1; g >= (int64) from && n < max; g--)
		if (!st[g].free)
		{
			starts[n] = (uint64) g * SEGMENT_BYTES;
			ends[n] = starts[n] + SEGMENT_BYTES;
			n++;
		}
	for (int k = 0; k < n / 2; k++)
	{
		uint64		t = starts[k];

		starts[k] = starts[n - 1 - k];
		starts[n - 1 - k] = t;
		t = ends[k];
		ends[k] = ends[n - 1 - k];
		ends[n - 1 - k] = t;
	}
	pfree(st);
	return n;
}

static uint64
page_segment_bytes(Relation rel)
{
	return SEGMENT_BYTES;
}

/* a page the log's tail may be cut at: free, part written all dead, or new */
static bool
tail_page_cuttable(Relation rel, BlockNumber blk)
{
	Buffer		buf;
	Page		page;
	bool		cut;

	/* (a map page goes with the last of its group's pages) */
	if (IS_MAP_BLOCK(blk))
		return true;
	buf = ReadBuffer(rel, blk);
	page = BufferGetPage(buf);

	/*
	 * (a page part written all dead too: its backend, if any, can append
	 * to it no more, with no append running and its region forgotten.
	 * Bytes reserved past what is written, an aborted entry's, are not
	 * counted on it then: the cut makes a generation, and they are of the
	 * one before)
	 */
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	cut = PageIsNew(page) || page_free(page) ||
		(PageGetSpecialSize(page) == MAXALIGN(sizeof(ChunkPageOpaqueData)) &&
		 ((ChunkPageOpaque) PageGetSpecialPointer(page))->kind == CHUNK_PAGE_DATA &&
		 ((ChunkPageOpaque) PageGetSpecialPointer(page))->dead ==
		 ((PageHeader) page)->pd_lower - AREA_START);
	UnlockReleaseBuffer(buf);
	return cut;
}

/*
 * Cuts the log's free tail off: the pages at its end all free, part written
 * and all dead, or never written.  Only if its last page is such, and with
 * the relation's AccessExclusiveLock got without waiting (as VACUUM cuts a
 * heap's empty end), so never while an append runs; the lock is let go of
 * as soon as the tail is cut.  The relation is truncated first, and the
 * meta page's next region set after (a crash between leaves only holes
 * that extensions fill), and a relcache invalidation goes at once, whatever
 * becomes of this transaction: a backend whose region lay in the tail
 * forgets it before its next append.  Answers the pages cut.
 */
BlockNumber
ChunkLogTruncateTail(Relation rel)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);
	BlockNumber keep;

	if (nblocks <= META_BLOCK + 1 || !tail_page_cuttable(rel, nblocks - 1))
		return 0;
	if (!ConditionalLockRelation(rel, AccessExclusiveLock))
		return 0;
	nblocks = RelationGetNumberOfBlocks(rel);
	for (keep = nblocks; keep > META_BLOCK + 1; keep--)
		if (!tail_page_cuttable(rel, keep - 1))
			break;

	/*
	 * (whole segments cut: the pages kept rounded up to a segment's end, so
	 * that the next claim, from the cut, begins a segment)
	 */
	{
		uint64		d = pages_below(keep);
		BlockNumber aligned;

		d = (d + SEGMENT_PAGES - 1) / SEGMENT_PAGES * SEGMENT_PAGES;
		aligned = d == 0 ? META_BLOCK + 1 :
			d % MAP_BITS == 0 ? MAP_BLOCK(d) : DATA_BLOCK(d);
		keep = Max(keep, Min(aligned, nblocks));
	}
	if (keep < nblocks)
	{
		Buffer		mbuf;
		SharedInvalidationMessage msg;

		RelationTruncate(rel, keep);

		/* (no region from the cut on: the next claim starts there) */
		mbuf = ReadBuffer(rel, META_BLOCK);
		LockBuffer(mbuf, BUFFER_LOCK_EXCLUSIVE);
		(void) page_meta(rel, BufferGetPage(mbuf));
		set_next_block(rel, mbuf, keep, true);
		UnlockReleaseBuffer(mbuf);

		msg.rc.id = SHAREDINVALRELCACHE_ID;
		msg.rc.dbId = MyDatabaseId;
		msg.rc.relId = RelationGetRelid(rel);
		VALGRIND_MAKE_MEM_DEFINED(&msg, sizeof(msg));
		SendSharedInvalidMessages(&msg, 1);
	}
	UnlockRelation(rel, AccessExclusiveLock);
	return nblocks - keep;
}

const ChunkLogRoutine PageChunkLog = {
	.append = page_append,
	.prepare = page_prepare,
	.reserve = page_reserve,
	.write = page_write_at,
	.release = page_release,
	.prepare_free = page_prepare_free,
	.compactable = page_compactable_segments,
	.evacuable = page_evacuable_segments,
	.segment_bytes = page_segment_bytes,
	.find_segments = page_find_segments,
	.extend = page_extend,
	.bind_index = page_bind_index,
	.read = page_read,
	.mark_dead = page_mark_dead,
	.stamp = page_stamp,
	.end = page_end,
};

const ChunkLogRoutine *
ChunkLogOf(Relation rel)
{
	if (!RelationIsChunkLog(rel))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a chunk log relation", RelationGetRelationName(rel))));
	return &PageChunkLog;
}

/* ------------------------------------------------- the access method */

static void
refuse(const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("a history store relation does not support %s", what)));
}

static const TupleTableSlotOps *
cl_slot_callbacks(Relation relation)
{
	return &TTSOpsVirtual;
}

/* a scan of the log's relation finds no tuples (a SELECT, ANALYZE) */
static TableScanDesc
cl_scan_begin(Relation rel, Snapshot snapshot, int nkeys, ScanKeyData *key,
			  ParallelTableScanDesc pscan, uint32 flags)
{
	TableScanDesc scan = palloc0(sizeof(TableScanDescData));

	scan->rs_rd = rel;
	scan->rs_snapshot = snapshot;
	scan->rs_nkeys = nkeys;
	scan->rs_flags = flags;
	scan->rs_parallel = pscan;
	return scan;
}

static void
cl_scan_end(TableScanDesc scan)
{
	pfree(scan);
}

static void
cl_scan_rescan(TableScanDesc scan, ScanKeyData *key, bool set_params,
			   bool allow_strat, bool allow_sync, bool allow_pagemode)
{
}

static bool
cl_scan_getnextslot(TableScanDesc scan, ScanDirection direction,
					TupleTableSlot *slot)
{
	ExecClearTuple(slot);
	return false;
}

static void
cl_scan_set_tidrange(TableScanDesc scan, ItemPointer mintid,
					 ItemPointer maxtid)
{
	refuse("TID range scans");
}

static bool
cl_scan_getnextslot_tidrange(TableScanDesc scan, ScanDirection direction,
							 TupleTableSlot *slot)
{
	refuse("TID range scans");
	pg_unreachable();
}


static void
cl_index_scan_begin(IndexScanDesc scan, uint32 flags)
{
	refuse("indexes");
}

static void
cl_index_scan_reset(IndexScanDesc scan)
{
	refuse("indexes");
}

static void
cl_index_scan_end(IndexScanDesc scan)
{
	refuse("indexes");
}

static bool
cl_fetch_tid(Relation rel, ItemPointer tid, Snapshot snapshot, bool *all_dead)
{
	refuse("indexes");
	pg_unreachable();
}

/* (no tuple has a TID: a TID scan finds none) */
static bool
cl_tuple_fetch_row_version(Relation rel, ItemPointer tid, Snapshot snapshot,
						   TupleTableSlot *slot)
{
	return false;
}

static bool
cl_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
	return false;
}

static void
cl_tuple_get_latest_tid(TableScanDesc scan, ItemPointer tid)
{
	refuse("fetching tuples");
}

static bool
cl_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
							Snapshot snapshot)
{
	refuse("fetching tuples");
	pg_unreachable();
}

static TransactionId
cl_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
	refuse("indexes");
	pg_unreachable();
}

static void
cl_tuple_insert(Relation rel, TupleTableSlot *slot, CommandId cid,
				uint32 options, BulkInsertStateData *bistate)
{
	refuse("inserting tuples");
}

static void
cl_tuple_insert_speculative(Relation rel, TupleTableSlot *slot, CommandId cid,
							uint32 options, BulkInsertStateData *bistate,
							uint32 specToken)
{
	refuse("inserting tuples");
}

static void
cl_tuple_complete_speculative(Relation rel, TupleTableSlot *slot,
							  uint32 specToken, bool succeeded)
{
	refuse("inserting tuples");
}

static void
cl_multi_insert(Relation rel, TupleTableSlot **slots, int nslots,
				CommandId cid, uint32 options, BulkInsertStateData *bistate)
{
	refuse("inserting tuples");
}

static TM_Result
cl_tuple_delete(Relation rel, ItemPointer tid, CommandId cid, uint32 options,
				Snapshot snapshot, Snapshot crosscheck, bool wait,
				TM_FailureData *tmfd)
{
	refuse("deleting tuples");
	pg_unreachable();
}

static TM_Result
cl_tuple_update(Relation rel, ItemPointer otid, TupleTableSlot *slot,
				CommandId cid, uint32 options, Snapshot snapshot,
				Snapshot crosscheck, bool wait, TM_FailureData *tmfd,
				LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes)
{
	refuse("updating tuples");
	pg_unreachable();
}

static TM_Result
cl_tuple_lock(Relation rel, ItemPointer tid, Snapshot snapshot,
			  TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
			  LockWaitPolicy wait_policy, uint8 flags, TM_FailureData *tmfd)
{
	refuse("locking tuples");
	pg_unreachable();
}

static void
cl_finish_bulk_insert(Relation rel, uint32 options)
{
}

/*
 * The history store's relation is ordinary storage, WAL-logged: neither
 * unlogged nor temporary, and not a branch's (in a versioned schema).
 */
static SMgrRelation
create_store_storage(Relation rel, const RelFileLocator *newrlocator,
					 char persistence, TransactionId *freezeXid,
					 MultiXactId *minmulti)
{
	if (persistence != RELPERSISTENCE_PERMANENT)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a history store relation must be permanent")));
	if (NamespaceBranch(RelationGetNamespace(rel)) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a history store relation cannot be in a versioned schema")));

	*freezeXid = InvalidTransactionId;
	*minmulti = InvalidMultiXactId;
	return RelationCreateStorageWith(*newrlocator, persistence, true, SMGR_MD);
}

static void
cl_relation_set_new_filelocator(Relation rel,
								const RelFileLocator *newrlocator,
								char persistence,
								TransactionId *freezeXid,
								MultiXactId *minmulti)
{
	SMgrRelation srel = create_store_storage(rel, newrlocator, persistence,
											 freezeXid, minmulti);

	/*
	 * (logged unless WAL is minimal, when the new storage is synced at
	 * commit instead, as RelationCreateStorage arranged)
	 */
	write_meta(srel, XLogIsNeeded());
	smgrclose(srel);
}

/* a chunk index's storage: its own first pages (prolly/chunk_index.h) */
static void
ci_relation_set_new_filelocator(Relation rel,
								const RelFileLocator *newrlocator,
								char persistence,
								TransactionId *freezeXid,
								MultiXactId *minmulti)
{
	SMgrRelation srel = create_store_storage(rel, newrlocator, persistence,
											 freezeXid, minmulti);

	/* (its entries name the transactions that inserted them) */
	*freezeXid = RecentXmin;
	ChunkIndexInitStorage(srel, XLogIsNeeded());
	smgrclose(srel);
}

/* a chunk index's VACUUM: aborted transactions' entries out, the rest frozen */
static void
ci_relation_vacuum(Relation rel, const VacuumParams *params,
				   BufferAccessStrategy bstrategy)
{
	ChunkIndexVacuum(rel, params->truncate != VACOPTVALUE_DISABLED);
}

static void
ci_relation_nontransactional_truncate(Relation rel)
{
	RelationTruncate(rel, 0);
	ChunkIndexInitStorage(RelationGetSmgr(rel), RelationNeedsWAL(rel));
	if (rel->rd_amcache != NULL)
	{
		pfree(rel->rd_amcache);
		rel->rd_amcache = NULL;
	}
}

static void
cl_relation_nontransactional_truncate(Relation rel)
{
	RelationTruncate(rel, 0);
	write_meta(RelationGetSmgr(rel), RelationNeedsWAL(rel));

	/* (the region this backend had is gone with its pages) */
	if (rel->rd_amcache != NULL)
	{
		pfree(rel->rd_amcache);
		rel->rd_amcache = NULL;
	}
}

/* (ALTER TABLE ... SET TABLESPACE: the pages as they are) */
static void
cl_relation_copy_data(Relation rel, const RelFileLocator *newrlocator)
{
	SMgrRelation dstrel;

	FlushRelationBuffers(rel);
	dstrel = RelationCreateStorageWith(*newrlocator,
									   rel->rd_rel->relpersistence, true,
									   RelationGetSmgr(rel)->smgr_which);
	RelationCopyStorage(RelationGetSmgr(rel), dstrel, MAIN_FORKNUM,
						rel->rd_rel->relpersistence);
	RelationDropStorage(rel);
	smgrclose(dstrel);
}

static void
cl_relation_copy_for_cluster(Relation OldTable, Relation NewTable,
							 Relation OldIndex, bool use_sort,
							 TransactionId OldestXmin, Snapshot snapshot,
							 TransactionId *xid_cutoff,
							 MultiXactId *multi_cutoff, double *num_tuples,
							 double *tups_vacuumed, double *tups_recently_dead)
{
	refuse("VACUUM FULL or CLUSTER");
}

/* nothing to vacuum: the log holds no tuples */
static void
cl_relation_vacuum(Relation rel, const VacuumParams *params,
				   BufferAccessStrategy bstrategy)
{
}

static bool
cl_scan_analyze_next_block(TableScanDesc scan, ReadStream *stream)
{
	return false;
}

static bool
cl_scan_analyze_next_tuple(TableScanDesc scan, double *liverows,
						   double *deadrows, TupleTableSlot *slot)
{
	return false;
}

static double
cl_index_build_range_scan(Relation table_rel, Relation index_rel,
						  IndexInfo *index_info, bool allow_sync,
						  bool anyvisible, bool progress,
						  BlockNumber start_blockno, BlockNumber numblocks,
						  IndexBuildCallback callback, void *callback_state,
						  TableScanDesc scan)
{
	refuse("indexes");
	pg_unreachable();
}

static void
cl_index_validate_scan(Relation table_rel, Relation index_rel,
					   IndexInfo *index_info, Snapshot snapshot,
					   ValidateIndexState *state)
{
	refuse("indexes");
}

static uint64
cl_relation_size(Relation rel, ForkNumber forkNumber)
{
	return table_block_relation_size(rel, forkNumber);
}

static bool
cl_relation_needs_toast_table(Relation rel)
{
	return false;
}

static void
cl_relation_estimate_size(Relation rel, int32 *attr_widths,
						  BlockNumber *pages, double *tuples,
						  double *allvisfrac)
{
	*pages = 0;
	*tuples = 0;
	*allvisfrac = 0;
}

static bool
cl_scan_bitmap_next_tuple(TableScanDesc scan, TupleTableSlot *slot,
						  bool *recheck, uint64 *lossy_pages,
						  uint64 *exact_pages)
{
	refuse("bitmap scans");
	pg_unreachable();
}

static bool
cl_scan_sample_next_block(TableScanDesc scan, SampleScanState *scanstate)
{
	refuse("TABLESAMPLE");
	pg_unreachable();
}

static bool
cl_scan_sample_next_tuple(TableScanDesc scan, SampleScanState *scanstate,
						  TupleTableSlot *slot)
{
	refuse("TABLESAMPLE");
	pg_unreachable();
}

static const TableAmRoutine chunk_log_routine = {
	.type = T_TableAmRoutine,
	.slot_callbacks = cl_slot_callbacks,
	.scan_begin = cl_scan_begin,
	.scan_end = cl_scan_end,
	.scan_rescan = cl_scan_rescan,
	.scan_getnextslot = cl_scan_getnextslot,
	.scan_set_tidrange = cl_scan_set_tidrange,
	.scan_getnextslot_tidrange = cl_scan_getnextslot_tidrange,
	/* (a parallel scan, as a plain one, finds no tuples) */
	.parallelscan_estimate = table_block_parallelscan_estimate,
	.parallelscan_initialize = table_block_parallelscan_initialize,
	.parallelscan_reinitialize = table_block_parallelscan_reinitialize,
	.index_scan_begin = cl_index_scan_begin,
	.index_scan_reset = cl_index_scan_reset,
	.index_scan_end = cl_index_scan_end,
	.fetch_tid = cl_fetch_tid,
	.tuple_fetch_row_version = cl_tuple_fetch_row_version,
	.tuple_tid_valid = cl_tuple_tid_valid,
	.tuple_get_latest_tid = cl_tuple_get_latest_tid,
	.tuple_satisfies_snapshot = cl_tuple_satisfies_snapshot,
	.index_delete_tuples = cl_index_delete_tuples,
	.tuple_insert = cl_tuple_insert,
	.tuple_insert_speculative = cl_tuple_insert_speculative,
	.tuple_complete_speculative = cl_tuple_complete_speculative,
	.multi_insert = cl_multi_insert,
	.tuple_delete = cl_tuple_delete,
	.tuple_update = cl_tuple_update,
	.tuple_lock = cl_tuple_lock,
	.finish_bulk_insert = cl_finish_bulk_insert,
	.relation_set_new_filelocator = cl_relation_set_new_filelocator,
	.relation_nontransactional_truncate = cl_relation_nontransactional_truncate,
	.relation_copy_data = cl_relation_copy_data,
	.relation_copy_for_cluster = cl_relation_copy_for_cluster,
	.relation_vacuum = cl_relation_vacuum,
	.scan_analyze_next_block = cl_scan_analyze_next_block,
	.scan_analyze_next_tuple = cl_scan_analyze_next_tuple,
	.index_build_range_scan = cl_index_build_range_scan,
	.index_validate_scan = cl_index_validate_scan,
	.relation_size = cl_relation_size,
	.relation_needs_toast_table = cl_relation_needs_toast_table,
	.relation_estimate_size = cl_relation_estimate_size,
	.scan_bitmap_next_tuple = cl_scan_bitmap_next_tuple,
	.scan_sample_next_block = cl_scan_sample_next_block,
	.scan_sample_next_tuple = cl_scan_sample_next_tuple,
};

/*
 * A chunk index's relation is a chunk log's but for its pages: the same
 * callbacks, its storage made with the index's first pages.
 */
static TableAmRoutine chunk_index_routine;
static bool chunk_index_routine_made = false;

static const TableAmRoutine *
index_routine(void)
{
	if (!chunk_index_routine_made)
	{
		chunk_index_routine = chunk_log_routine;
		chunk_index_routine.relation_set_new_filelocator =
			ci_relation_set_new_filelocator;
		chunk_index_routine.relation_nontransactional_truncate =
			ci_relation_nontransactional_truncate;
		chunk_index_routine.relation_vacuum = ci_relation_vacuum;
		chunk_index_routine_made = true;
	}
	return &chunk_index_routine;
}

bool
RelationIsChunkLog(Relation rel)
{
	return rel->rd_tableam == &chunk_log_routine;
}

bool
RelationIsChunkIndex(Relation rel)
{
	return rel->rd_tableam == index_routine();
}

Datum
prolly_chunks_tableam_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&chunk_log_routine);
}

Datum
prolly_chunk_index_tableam_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(index_routine());
}

/* ------------------------------------------------------------- redo */

void
chunk_log_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	char	   *rec = XLogRecGetData(record);
	RelFileLocator locator;
	BlockNumber blk;
	Buffer		buf = InvalidBuffer;
	bool		applied = false;

	XLogRecGetBlockTag(record, 0, &locator, NULL, &blk);
	switch (info)
	{
		case XLOG_CHUNK_LOG_NEXT_BLOCK:
			if (XLogReadBufferForRedo(record, 0, &buf) == BLK_NEEDS_REDO)
			{
				meta_set((ChunkLogMetaData *) PageGetContents(BufferGetPage(buf)),
						 (xl_chunk_log_next_block *) rec);
				applied = true;
			}
			break;
		case XLOG_CHUNK_LOG_BIND_INDEX:
			if (XLogReadBufferForRedo(record, 0, &buf) == BLK_NEEDS_REDO)
			{
				((ChunkLogMetaData *) PageGetContents(BufferGetPage(buf)))->index =
					((xl_chunk_log_bind_index *) rec)->index;
				applied = true;
			}
			break;
		case XLOG_CHUNK_LOG_INIT_PAGE:
			buf = XLogInitBufferForRedo(record, 0);
			page_make(BufferGetPage(buf), ((xl_chunk_log_init_page *) rec)->all_dead);
			applied = true;
			break;
		case XLOG_CHUNK_LOG_SEAL_PAGE:
			{
				xl_chunk_log_seal *xlrec = (xl_chunk_log_seal *) rec;

				if (xlrec->init)
					buf = XLogInitBufferForRedo(record, 0);
				else if (XLogReadBufferForRedo(record, 0, &buf) != BLK_NEEDS_REDO)
					break;
				page_seal(BufferGetPage(buf), xlrec->init, xlrec->off);
				applied = true;
				break;
			}
		case XLOG_CHUNK_LOG_APPEND:
			{
				xl_chunk_log_append *xlrec = (xl_chunk_log_append *) rec;
				Size		len;
				char	   *bytes;

				if (xlrec->init)
					buf = XLogInitBufferForRedo(record, 0);
				else if (XLogReadBufferForRedo(record, 0, &buf) != BLK_NEEDS_REDO)
					break;
				bytes = XLogRecGetBlockData(record, 0, &len);
				if (len != xlrec->len || xlrec->off + len > AREA)
					elog(PANIC, "chunk_log_redo: an append of %u bytes at %u has %zu",
						 xlrec->len, xlrec->off, len);
				page_write(BufferGetPage(buf), xlrec->init, xlrec->off, bytes, len);
				applied = true;
				break;
			}
		case XLOG_CHUNK_LOG_MAP:
			{
				xl_chunk_log_map *xlrec = (xl_chunk_log_map *) rec;

				if (xlrec->init)
					buf = XLogInitBufferForRedo(record, 0);
				else if (XLogReadBufferForRedo(record, 0, &buf) != BLK_NEEDS_REDO)
					break;
				page_map_set(BufferGetPage(buf), xlrec->init, xlrec->first,
							 xlrec->n, xlrec->set);
				applied = true;
				break;
			}
		case XLOG_CHUNK_LOG_MARK_DEAD:
			{
				xl_chunk_log_mark_dead *xlrec = (xl_chunk_log_mark_dead *) rec;

				if (xlrec->init)
					buf = XLogInitBufferForRedo(record, 0);
				else if (XLogReadBufferForRedo(record, 0, &buf) != BLK_NEEDS_REDO)
					break;
				page_add_dead(BufferGetPage(buf), xlrec->init, xlrec->off, xlrec->len);
				applied = true;
				break;
			}
		default:
			elog(PANIC, "chunk_log_redo: unknown op code %u", info);
	}
	if (applied)
	{
		PageSetLSN(BufferGetPage(buf), record->EndRecPtr);
		MarkBufferDirty(buf);
	}
	if (BufferIsValid(buf))
	{
		/*
		 * (the free space map as the primary keeps it, for after promotion:
		 * a page made afresh to append to is not free.  Segments freed are
		 * put in by VACUUM, which after promotion puts every free one in
		 * again.)
		 */
		bool		data_page = blk != META_BLOCK && !IS_MAP_BLOCK(blk);

		UnlockReleaseBuffer(buf);
		if (data_page && info == XLOG_CHUNK_LOG_INIT_PAGE)
			XLogRecordPageWithFreeSpace(locator, blk, 0);
	}
}

/*
 * For wal_consistency_checking: a page as replayed is the primary's but for
 * its LSN, checksum, hint bits and the bytes past where it is written
 */
void
chunk_log_mask(char *pagedata, BlockNumber blkno)
{
	mask_page_lsn_and_checksum(pagedata);
	mask_page_hint_bits(pagedata);
	mask_unused_space(pagedata);
}
