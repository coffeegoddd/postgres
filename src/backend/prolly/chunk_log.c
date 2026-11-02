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
#define CHUNK_LOG_VERSION	6

#define CHUNK_PAGE_META		1
#define CHUNK_PAGE_DATA		2
#define CHUNK_PAGE_MAP		3

#define META_BLOCK			0

/* the pages a backend claims at a time to append into */
#define REGION_PAGES		32

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

/*
 * Claims npages new data pages for this backend's appends, from the meta
 * page: the one place appends meet, for one short record per claim.  The
 * pages are this backend's region's (published) and marked open before
 * they are the log's: VACUUM finds none of them unheld and not open.
 * Answers the first.
 */
static uint64
claim_pages(Relation rel, uint64 npages)
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
	ChunkRegionPublish(rel->rd_locator, RelationGetRelid(rel), meta->generation,
					   DATA_BLOCK(first), DATA_BLOCK(first + npages - 1) + 1);
	map_mark(rel, first, npages, true);
	set_next_block(rel, mbuf, DATA_BLOCK(first + npages - 1) + 1, false);
	UnlockReleaseBuffer(mbuf);
	return first;
}

/* whether data page d is free (every byte written and dead), locked a moment */
static bool
probe_free(Relation rel, uint64 d, BlockNumber nblocks)
{
	BlockNumber blk = DATA_BLOCK(d);
	Buffer		buf;
	bool		free_page;

	if (blk >= nblocks)
		return false;
	buf = ReadBuffer(rel, blk);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	free_page = !PageIsNew(BufferGetPage(buf)) && page_free(BufferGetPage(buf));
	UnlockReleaseBuffer(buf);
	return free_page;
}

/*
 * npages data pages one after another the free space map has as free (every
 * byte dead), made afresh for this backend's appends; the first, or -1 if
 * none are found (a region of them grows a page at a time as its appends
 * reach its end: see extend_region).  A page the map has is looked at with
 * those before it, for where a run of free pages begins; the run's pages
 * are then locked in order, each checked free, and made afresh.  The map is a hint, not WAL-logged: a
 * page it has wrongly is passed by, and taken out of it.
 */
static int64
take_free_pages(Relation rel, uint32 generation, int npages)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);

	Assert(npages >= 1 && npages <= REGION_PAGES);
	for (int tries = 0; tries < 8; tries++)
	{
		/*
		 * (the map keeps space in steps, a whole page rounding past what a
		 * page can have: half of one finds the pages recorded free, which
		 * are then checked)
		 */
		BlockNumber blk = GetPageWithFreeSpace(rel, AREA / 2);
		Buffer		bufs[REGION_PAGES];
		uint64		first;
		int			locked = 0;
		bool		all_free = true;
		bool		blk_wrong = false;

		if (!BlockNumberIsValid(blk) || blk == META_BLOCK || blk >= nblocks)
			return -1;
		if (IS_MAP_BLOCK(blk))
		{
			RecordPageWithFreeSpace(rel, blk, 0);
			continue;
		}

		/* (the run's beginning: free pages before this one, as far as it needs) */
		first = pages_below(blk);
		while (npages > 1 && first > 0 && pages_below(blk) - first < (uint64) npages - 1 &&
			   probe_free(rel, first - 1, nblocks))
			first--;
		for (int j = 0; j < npages && all_free; j++)
		{
			BlockNumber b = DATA_BLOCK(first + j);
			Page		page;

			if (b >= nblocks)
			{
				all_free = false;
				break;
			}
			bufs[j] = ReadBuffer(rel, b);
			LockBuffer(bufs[j], BUFFER_LOCK_EXCLUSIVE);
			locked++;
			page = BufferGetPage(bufs[j]);
			if (PageIsNew(page) || !page_free(page))
			{
				all_free = false;
				blk_wrong = b == blk;
			}
		}
		if (!all_free)
		{
			for (int j = 0; j < locked; j++)
				UnlockReleaseBuffer(bufs[j]);
			/* (one the map has wrongly, out of it, once nothing is locked) */
			if (blk_wrong)
				RecordPageWithFreeSpace(rel, blk, 0);
			continue;
		}


		/* (this backend's region's, and open, before they are made to use) */
		ChunkRegionPublish(rel->rd_locator, RelationGetRelid(rel), generation,
						   DATA_BLOCK(first), DATA_BLOCK(first + npages - 1) + 1);
		map_mark(rel, first, npages, true);
		for (int j = 0; j < npages; j++)
			make_page(rel, bufs[j], false);
		for (int j = 0; j < npages; j++)
			UnlockReleaseBuffer(bufs[j]);
		/* (the map told, once nothing is locked: it may extend itself) */
		for (int j = 0; j < npages; j++)
			RecordPageWithFreeSpace(rel, DATA_BLOCK(first + j), 0);
		return (int64) first;
	}
	return -1;
}

static void page_mark_dead(Relation rel, uint64 loc, size_t len, uint32 generation);

/*
 * The region grown over the data pages just after it, as many as len bytes
 * need past its end, if they are free (every byte dead): locked in order,
 * each checked free, published as this backend's region's (from its page
 * now on) and marked open, then made afresh.  False, nothing done, if they
 * are not all free, or more than a region's.  (Its first page as before.)
 */
static bool
extend_region(Relation rel, ChunkLogRegion *region, size_t len)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);
	uint64		room = region->limit - region->next;
	uint64		first = region->limit / AREA;
	int			n = (int) Min((uint64) REGION_PAGES + 1, (len - room + AREA - 1) / AREA);
	Buffer		bufs[REGION_PAGES];
	int			locked = 0;

	BlockNumber from = ChunkRegionMineFirst(rel->rd_locator);

	if (n > REGION_PAGES || region->limit % AREA != 0 || !BlockNumberIsValid(from))
		return false;
	for (int j = 0; j < n; j++)
	{
		BlockNumber b = DATA_BLOCK(first + j);
		Page		page;

		if (b >= nblocks)
			break;
		bufs[j] = ReadBuffer(rel, b);
		LockBuffer(bufs[j], BUFFER_LOCK_EXCLUSIVE);
		locked++;
		page = BufferGetPage(bufs[j]);
		if (PageIsNew(page) || !page_free(page))
			break;
	}
	if (locked < n || PageIsNew(BufferGetPage(bufs[n - 1])) ||
		!page_free(BufferGetPage(bufs[n - 1])))
	{
		for (int j = 0; j < locked; j++)
			UnlockReleaseBuffer(bufs[j]);
		return false;
	}
	/*
	 * (published from the region's first page still: a pack's bytes reserved
	 * on its pages before may not be written yet, which VACUUM must not take
	 * for a region let go of)
	 */
	ChunkRegionPublish(rel->rd_locator, RelationGetRelid(rel), region->generation,
					   from, DATA_BLOCK(first + n - 1) + 1);
	map_mark(rel, first, n, true);
	for (int j = 0; j < n; j++)
		make_page(rel, bufs[j], false);
	for (int j = 0; j < n; j++)
		UnlockReleaseBuffer(bufs[j]);
	for (int j = 0; j < n; j++)
		RecordPageWithFreeSpace(rel, DATA_BLOCK(first + j), 0);
	region->limit += (uint64) n * AREA;
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
		bool		freed;

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
		freed = page_free(BufferGetPage(buf));
		UnlockReleaseBuffer(buf);
		if (freed)
		{
			RecordPageWithFreeSpace(rel, blk, AREA);
			FreeSpaceMapVacuumRange(rel, blk, blk + 1);
		}
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
		RecordPageWithFreeSpace(rel, blk, AREA);
		FreeSpaceMapVacuumRange(rel, blk, blk + 1);
	}
}

/* pages enough for len bytes, from the first */
static void
claim_run(Relation rel, uint64 len, uint64 npages, ChunkLogRegion *run)
{
	uint64		first;

	npages = Max(npages, (len + AREA - 1) / AREA);
	first = claim_pages(rel, npages);
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
 * Room for len bytes in this backend's region: what is left of it, or
 * else a new one (a run of pages freed, if the bytes fit no more than a
 * region's pages, or new pages, more if the bytes are longer than a
 * region), what was left of the old one
 * let go of.  The region, kept in the relation's rd_amcache too, in *run.
 */
static void
page_prepare(Relation rel, size_t len, ChunkLogRun *run)
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
	 * (a region grown over the free pages just after it, if they are free,
	 * as far as the bytes need: chunks go on across them as across pages
	 * new, where a region of a page alone would leave each page's rest the
	 * next chunk does not fit dead, and a region of many taken at once
	 * would keep from others pages its backend may never use)
	 */
	if (region.limit > 0 && region.limit - region.next < len &&
		extend_region(rel, &region, len))
		keep_region(rel, &region);
	if (region.limit - region.next < len)
	{
		uint64		npages;
		int64		first;
		uint32		generation;

		/*
		 * What is left of the region let go of, forgotten first: should what
		 * follows fail (a claim, an I/O), no put reserves in a region sealed
		 */
		if (cached != NULL && cached->relnumber == region.relnumber)
			cached->next = cached->limit = 0;
		if (region.limit > 0)
			seal_region(rel, region.next, region.limit);
		INJECTION_POINT("chunk-log-region-sealed", NULL);

		/*
		 * (its bytes the generation's: a tail cut, which makes the next,
		 * needs the relation locked as no put holds it, and its
		 * invalidation has every backend forget its region)
		 */
		generation = log_generation(rel, NULL);
		/*
		 * (pages freed first, as many as the bytes need, if no more than a
		 * region's: a run of them found where the map has one free; the
		 * region then grows over free pages after it as appends reach them)
		 */
		npages = (len + AREA - 1) / AREA;
		first = npages <= REGION_PAGES ?
			take_free_pages(rel, generation, Max(npages, 1)) : -1;
		if (first >= 0)
		{
			region.relnumber = rel->rd_locator.relNumber;
			region.next = (uint64) first * AREA;
			region.limit = region.next + (uint64) Max(npages, 1) * AREA;
		}
		else
			claim_run(rel, len, REGION_PAGES, &region);

		region.generation = generation;
		keep_region(rel, &region);
	}
	run->relnumber = region.relnumber;
	run->next = region.next;
	run->limit = region.limit;
	run->generation = region.generation;
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
	if (run->relnumber != rel->rd_locator.relNumber ||
		run->limit - run->next >= len ||
		!extend_region(rel, &region, len))
		return run->limit - run->next >= len;
	keep_region(rel, &region);
	run->limit = region.limit;
	INJECTION_POINT("chunk-log-extended", NULL);
	return true;
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
 * Counts len bytes from loc dead, a page at a time; a page all of whose
 * bytes are dead then goes in the free space map, to be used again.
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
		bool		freed;
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
		freed = page_free(page);
		UnlockReleaseBuffer(buf);
		if (freed)
		{
			RecordPageWithFreeSpace(rel, blk, AREA);
			FreeSpaceMapVacuumRange(rel, blk, blk + 1);
		}
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
		bool		done = false,
					freed = false;

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
		if (done)
			freed = page_free(page);
		sweep->candidate[k] = done;
		UnlockReleaseBuffer(buf);
		if (freed)
		{
			RecordPageWithFreeSpace(rel, blk, AREA);
			FreeSpaceMapVacuumRange(rel, blk, blk + 1);
		}
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
 * Puts every page free (written full, all dead) in the free space map:
 * the map is not WAL-logged, and a crash, a standby's promotion or a move
 * to another tablespace leaves it without pages freed since it was last
 * written.  O(log).
 */
void
ChunkLogRecordFree(Relation rel)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);

	for (BlockNumber blk = META_BLOCK + 1; blk < nblocks; blk++)
	{
		Buffer		buf;
		bool		freed;

		if (IS_MAP_BLOCK(blk))
			continue;
		buf = ReadBuffer(rel, blk);

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		freed = !PageIsNew(BufferGetPage(buf)) && page_free(BufferGetPage(buf));
		UnlockReleaseBuffer(buf);
		if (freed)
			RecordPageWithFreeSpace(rel, blk, AREA);
		CHECK_FOR_INTERRUPTS();
	}
	FreeSpaceMapVacuum(rel);
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
	.extend = page_extend,
	.bind_index = page_bind_index,
	.read = page_read,
	.mark_dead = page_mark_dead,
	.stamp = page_stamp,
	.end = page_end,
};

const ChunkLogRoutine *ChunkLog = &PageChunkLog;

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
		 * a page all dead is free, its upper levels told so, as the primary
		 * does, or a search from the root would not find it; one made afresh
		 * to append to is not.  With checksums, a standby's map pages are
		 * not written out for this, as with heap's, and VACUUM after
		 * promotion puts every free page in again.)
		 */
		bool		data_page = blk != META_BLOCK && !IS_MAP_BLOCK(blk);
		bool		freed = data_page && page_free(BufferGetPage(buf));

		UnlockReleaseBuffer(buf);
		if (freed)
		{
			Relation	rel = CreateFakeRelcacheEntry(locator);

			XLogRecordPageWithFreeSpace(locator, blk, AREA);
			FreeSpaceMapVacuumRange(rel, blk, blk + 1);
			FreeFakeRelcacheEntry(rel);
		}
		else if (data_page && info == XLOG_CHUNK_LOG_INIT_PAGE)
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
