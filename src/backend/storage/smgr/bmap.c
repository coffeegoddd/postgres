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
 * The map fork.  Block 0 is a header holding the relation's logical length
 * and epoch.  Logical block L's entry is at slot L % BMAP_FANOUT of map page
 * 1 + L / BMAP_FANOUT: found without following any pointer.  An entry is a
 * physical block, the epoch it was born in (one born before its map's epoch
 * may be shared with another map), and the WAL position since which it has
 * that block.  Its block changes only while nothing else can change the
 * logical block: when a shared page is copied before it changes (its
 * buffer locked exclusively), or when an entry left past the relation's end
 * by a truncation is let go because another map shares its block.  The map
 * fork's existence is the relation's; smgropen() chooses this storage
 * manager by it.
 *
 * Recovery reads map pages that can be from different moments: one written
 * after the point redo has reached, another not yet written.  A version
 * older than the record being replayed gives it the block it was made for.
 * A newer one may give a block set after the record: redo skips the record
 * (smgrreplacedafter()), since the logical block's contents from before
 * the change aren't needed there, and doesn't write pages it made to that
 * block, which may still hold another relation's page at this point of the
 * replay.
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
 * at a time.  Extension holds the header exclusively (serializing the
 * relation's extensions and its map fork's growth): it reads the map page,
 * takes blocks under the allocator's metapage and bitmap pages' locks, and
 * then locks the map page exclusively.  Truncation takes the header's.
 * Writing a page out looks it up without loading map pages into shared
 * buffers (see read_node()), but share-locks a map page it finds cached,
 * so a write may wait for a change of that page.  Any buffer read may be
 * the eviction that writes a page out, so a backend holding a map page
 * locked for a change reads no buffer: a change reads every buffer it
 * needs (the changes list's, the reference counts', the allocator's)
 * first and locks the map page last (map_lock(), which assert-enabled
 * builds check at every read).  The one exception is a map page just
 * added, or found all zeroes: it maps no page a write could look up, so an
 * extension keeps it locked while it allocates.
 *
 * The sweep (bmap_sweep()).  Freeing is by reference count, which leaks in
 * three cases: a crash between a drop's commit and the records freeing its
 * blocks, a crash part way through a prune, and a count stuck at 255 maps.
 * A sweep counts exactly what every map (and changes list) of a database's
 * page store holds, each relation's read under its header's lock, and
 * frees the blocks allocated as it began that nothing holds, rewrites the
 * stuck counts that now fit, and shortens the store to its last allocated
 * block (SHRINK, which redo truncates by too).  Branch creation (a share,
 * the one thing that adds holders) waits for it while it counts, and the
 * allocator reuses no freed block of the store while a sweep of it runs:
 * one freed and taken again meanwhile could be held by a map the sweep
 * read before.  A dropped
 * relation's map stays until the next checkpoint, and counts until then.
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
#include "storage/ipc.h"
#include "storage/md.h"
#include "storage/proc.h"
#include "storage/spin.h"
#include "storage/subsystems.h"
#include "utils/injection_point.h"
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
#define BMAP_PAGE_REFS		5
#define BMAP_PAGE_CHANGES_HEAD	6
#define BMAP_PAGE_CHANGES	7

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
	XLogRecPtr	since;			/* records before it are of another block */
} BmapEntry;

typedef struct BmapHeader
{
	uint32		version;
	BlockNumber nblocks;		/* the relation's logical length */
	uint32		epoch;			/* entries born before it may be shared */
	uint32		fold_epoch;		/* entries born before it are as of a fold
								 * (0: never folded), as the changes list's
								 * header says, here where each change
								 * reads it */
} BmapHeader;

/*
 * A changes list's header (block 0 of the changes fork): the epoch of the
 * relation's last fold, the list's lineage, the relation's length then, and
 * how many entries follow, BMAP_CHANGES_PER_PAGE a page from block 1.
 */
typedef struct BmapChangesHead
{
	uint32		version;
	uint32		fold_epoch;		/* entries born before it are as of a fold */
	uint64		lineage;
	BlockNumber fold_nblocks;
	uint32		nentries;
	uint32		pruned_epoch;	/* changes before it let go of (0: none) */
} BmapChangesHead;

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

#define BMAP_REFS_PER_PAGE	((uint32) BMAP_CONTENT_SIZE)
#define BMAP_CHANGES_PER_PAGE \
	((uint32) (BMAP_CONTENT_SIZE / sizeof(BmapChange)))

/* the changes list's page and slot of entry 'i' */
#define CHANGE_PAGE(i)		((BlockNumber) (1 + (i) / BMAP_CHANGES_PER_PAGE))
#define CHANGE_SLOT(i)		((uint32) ((i) % BMAP_CHANGES_PER_PAGE))

/* a count that reaches this stays, until a sweep counts it exactly */
#define BMAP_REFS_STUCK		255

/* the lock a sweep holds off branch creation with: an object no catalog has */
#define BMAP_SWEEP_CLASSID	((Oid) 0x424D5357)

/* reference-count pages one record changes at most */
#define BMAP_REFS_PER_RECORD	24

/* the reference-count page and slot of physical block 'phys' */
#define REF_PAGE(phys)		((BlockNumber) ((phys) / BMAP_REFS_PER_PAGE))
#define REF_SLOT(phys)		((uint32) ((phys) % BMAP_REFS_PER_PAGE))

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
#define PageGetBmapRefs(page)	((uint8 *) PageGetContents(page))
#define PageGetBmapChangesHead(page)	((BmapChangesHead *) PageGetContents(page))
#define PageGetBmapChanges(page)	((BmapChange *) PageGetContents(page))

static const PGIOAlignedBlock zero_block;

/* sweeps running whose page stores are listed, at most */
#define BMAP_SWEEPS_LISTED	64

/* page stores swept since the server started, remembered at most */
#define BMAP_SWEPT_MAX		1024

/*
 * A page store swept since the server started: the times a stuck count of
 * it lost a holder (which it keeps, stuck), and how many times that had
 * happened as its last sweep began.
 */
typedef struct BmapSwept
{
	Oid			spcOid;
	Oid			dbOid;
	bool		swept;			/* a sweep of it ended */
	uint64		released;
	uint64		swept_at;
} BmapSwept;

/*
 * Shared by every backend: the sweeps running, during which the allocator
 * of a page store being swept reuses no freed block (one freed and taken
 * again meanwhile could be held by a map the sweep read before); and the
 * page stores swept since the server started.
 */

typedef struct BmapShared
{
	pg_atomic_uint32 sweeps;	/* running, anywhere */
	slock_t		mutex;			/* protects what follows */
	int			nlisted;
	Oid			listed[BMAP_SWEEPS_LISTED][2];	/* tablespace, database */
	int			unlisted;		/* past the most listed: every store is */
	int			nswept;
	BmapSwept	swept[BMAP_SWEPT_MAX];
} BmapShared;

static BmapShared *bmap_shared = NULL;

static void
BmapShmemRequest(void *arg)
{
	ShmemRequestStruct(.name = "block map",
					   .size = sizeof(BmapShared),
					   .ptr = (void **) &bmap_shared,
		);
}

static void
BmapShmemInit(void *arg)
{
	pg_atomic_init_u32(&bmap_shared->sweeps, 0);
	SpinLockInit(&bmap_shared->mutex);
	bmap_shared->nlisted = 0;
	bmap_shared->unlisted = 0;
	bmap_shared->nswept = 0;
}

/* Is a sweep of rel's page store running? */
static bool
sweep_running(RelFileLocator rel)
{
	bool		running = false;

	if (pg_atomic_read_u32(&bmap_shared->sweeps) == 0)
		return false;
	SpinLockAcquire(&bmap_shared->mutex);
	running = bmap_shared->unlisted > 0;
	for (int i = 0; i < bmap_shared->nlisted && !running; i++)
		running = bmap_shared->listed[i][0] == rel.spcOid &&
			bmap_shared->listed[i][1] == rel.dbOid;
	SpinLockRelease(&bmap_shared->mutex);
	return running;
}

/* Note a sweep of rel's page store running (begin), or no longer. */
static void
sweep_note(RelFileLocator rel, bool begin)
{
	SpinLockAcquire(&bmap_shared->mutex);
	if (begin)
	{
		if (bmap_shared->nlisted < BMAP_SWEEPS_LISTED)
		{
			bmap_shared->listed[bmap_shared->nlisted][0] = rel.spcOid;
			bmap_shared->listed[bmap_shared->nlisted][1] = rel.dbOid;
			bmap_shared->nlisted++;
		}
		else
			bmap_shared->unlisted++;
	}
	else
	{
		int			i = 0;

		while (i < bmap_shared->nlisted &&
			   (bmap_shared->listed[i][0] != rel.spcOid ||
				bmap_shared->listed[i][1] != rel.dbOid))
			i++;
		if (i < bmap_shared->nlisted)
		{
			bmap_shared->nlisted--;
			bmap_shared->listed[i][0] = bmap_shared->listed[bmap_shared->nlisted][0];
			bmap_shared->listed[i][1] = bmap_shared->listed[bmap_shared->nlisted][1];
		}
		else
			bmap_shared->unlisted--;
	}
	SpinLockRelease(&bmap_shared->mutex);
	if (begin)
		pg_atomic_fetch_add_u32(&bmap_shared->sweeps, 1);
	else
		pg_atomic_fetch_sub_u32(&bmap_shared->sweeps, 1);
}

/* rel's page store's entry among those swept, or NULL; the mutex held */
static BmapSwept *
swept_find(RelFileLocator rel)
{
	for (int i = 0; i < bmap_shared->nswept; i++)
		if (bmap_shared->swept[i].spcOid == rel.spcOid &&
			bmap_shared->swept[i].dbOid == rel.dbOid)
			return &bmap_shared->swept[i];
	return NULL;
}

/*
 * A sweep of rel's page store begins (*at: the releases so far, to note
 * when it ends), or ends.  Past the most remembered, a store is not.
 */
static void
swept_note(RelFileLocator rel, bool begin, uint64 *at)
{
	BmapSwept  *found;

	SpinLockAcquire(&bmap_shared->mutex);
	found = swept_find(rel);
	if (found == NULL && begin && bmap_shared->nswept < BMAP_SWEPT_MAX)
	{
		found = &bmap_shared->swept[bmap_shared->nswept++];
		memset(found, 0, sizeof(*found));
		found->spcOid = rel.spcOid;
		found->dbOid = rel.dbOid;
	}
	if (found != NULL && begin)
		*at = found->released;
	else if (found != NULL)
	{
		found->swept = true;
		found->swept_at = *at;
	}
	SpinLockRelease(&bmap_shared->mutex);
}

/*
 * A stuck count of rel's page store lost a holder, which it keeps: a sweep
 * may now free the block, or fit its count.
 */
static void
stuck_released(RelFileLocator rel)
{
	BmapSwept  *found;

	SpinLockAcquire(&bmap_shared->mutex);
	found = swept_find(rel);
	if (found != NULL)
		found->released++;
	SpinLockRelease(&bmap_shared->mutex);
}

const ShmemCallbacks BmapShmemCallbacks = {
	.request_fn = BmapShmemRequest,
	.init_fn = BmapShmemInit,
};

static void list_stale(SMgrRelation reln, Buffer mapbuf, BlockNumber blkno,
					   BlockNumber old_block,
					   uint32 epoch);
static void unshare_stale(SMgrRelation reln, uint32 epoch, uint32 fold_epoch,
						  BlockNumber from,
						  BlockNumber to);
static void share_changes(SMgrRelation src, SMgrRelation dst);

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

static SMgrRelation
refs_smgr(RelFileLocator rel)
{
	return smgropen(with_relnumber(rel, BMAP_REFS_RELNUMBER),
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

/*
 * A map page locked for a change, and the rule that comes with it: while a
 * backend holds one locked it reads no buffer, as any read may evict a page
 * the map page maps, whose write would look it up there (read_node()).  A
 * map page just added, or found all zeroes, maps nothing, and is exempt.
 * Assert-enabled builds check the rule at each of this file's buffer reads,
 * whether or not a read happens to evict such a page; the resource owner
 * forgets the lock if an error releases it.
 */
#ifdef USE_ASSERT_CHECKING
static Buffer map_locked = InvalidBuffer;
static bool map_locked_new = false;

static void
map_locked_release(Datum res)
{
	map_locked = InvalidBuffer;
}

static const ResourceOwnerDesc map_locked_desc = {
	.name = "block map page locked for a change",
	.release_phase = RESOURCE_RELEASE_BEFORE_LOCKS,
	.release_priority = RELEASE_PRIO_FIRST,
	.ReleaseResource = map_locked_release,
	.DebugPrint = NULL
};
#endif

static void
map_lock(Buffer buf, bool isnew)
{
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
#ifdef USE_ASSERT_CHECKING
	Assert(!BufferIsValid(map_locked));
	if (CurrentResourceOwner)
	{
		ResourceOwnerEnlarge(CurrentResourceOwner);
		ResourceOwnerRemember(CurrentResourceOwner, Int32GetDatum(buf),
							  &map_locked_desc);
		map_locked = buf;
		map_locked_new = isnew;
	}
#endif
}

/* (a page map_new_page() returned, locked already) */
static void
map_locked_as_new(Buffer buf)
{
#ifdef USE_ASSERT_CHECKING
	Assert(!BufferIsValid(map_locked));
	if (CurrentResourceOwner)
	{
		ResourceOwnerEnlarge(CurrentResourceOwner);
		ResourceOwnerRemember(CurrentResourceOwner, Int32GetDatum(buf),
							  &map_locked_desc);
		map_locked = buf;
		map_locked_new = true;
	}
#endif
}

static void
map_unlock(Buffer buf)
{
#ifdef USE_ASSERT_CHECKING
	if (BufferIsValid(map_locked))
	{
		Assert(map_locked == buf);
		ResourceOwnerForget(CurrentResourceOwner, Int32GetDatum(buf),
							&map_locked_desc);
		map_locked = InvalidBuffer;
	}
#endif
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
}

/* a buffer read: no map page may be locked for a change (map_lock()) */
static inline void
check_may_read(void)
{
#ifdef USE_ASSERT_CHECKING
	Assert(!BufferIsValid(map_locked) || map_locked_new);
#endif
}

static Buffer
map_read(RelFileLocator rel, BlockNumber blkno)
{
	check_may_read();
	return ReadBufferWithoutRelcache(rel, BMAP_FORKNUM, blkno, RBM_NORMAL,
									 NULL, true);
}

static Buffer
alloc_read(RelFileLocator rel, BlockNumber blkno)
{
	check_may_read();
	return ReadBufferWithoutRelcache(with_relnumber(rel, BMAP_ALLOC_RELNUMBER),
									 MAIN_FORKNUM, blkno, RBM_NORMAL, NULL,
									 true);
}

/*
 * A reference-count page.  Every physical block below the page store's
 * high-water mark has one (refs_cover()), though it may still be all zeroes:
 * a new page counts zero for every block.
 */
static Buffer
refs_read(RelFileLocator rel, BlockNumber blkno)
{
	check_may_read();
	return ReadBufferWithoutRelcache(with_relnumber(rel, BMAP_REFS_RELNUMBER),
									 MAIN_FORKNUM, blkno, RBM_NORMAL, NULL,
									 true);
}

/* the count of 'phys' on its locked reference-count page */
static uint8
page_ref(Page page, BlockNumber phys)
{
	return PageIsNew(page) ? 0 : PageGetBmapRefs(page)[REF_SLOT(phys)];
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
	check_may_read();
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
	check_may_read();
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

static void
check_refs_page(Page page, RelFileLocator rel, BlockNumber blkno)
{
	if (!PageIsNew(page))
		check_page(page, BMAP_PAGE_REFS, with_relnumber(rel, BMAP_REFS_RELNUMBER),
				   MAIN_FORKNUM, blkno);
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
	PageGetBmapHeader(page)->epoch = delta->epoch;
	PageGetBmapHeader(page)->fold_epoch = delta->fold_epoch;
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
		BmapEntry  *e = &entries[ent[i].slot];

		Assert(ent[i].slot < BMAP_FANOUT);
		if (e->block != ent[i].block)
			e->since = delta->since;
		e->block = ent[i].block;
		e->epoch = ent[i].epoch;
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
apply_refs_delta(Page page, const xl_bmap_refs_delta *delta)
{
	const xl_bmap_ref_delta *ref = (const xl_bmap_ref_delta *) (delta + 1);
	uint8	   *counts;

	if (delta->flags & BMAP_DELTA_INIT)
		init_page(page, BMAP_PAGE_REFS);
	counts = PageGetBmapRefs(page);
	for (uint32 i = 0; i < delta->nrefs; i++)
	{
		Assert(ref[i].slot < BMAP_REFS_PER_PAGE && ref[i].count <= BMAP_REFS_STUCK);
		counts[ref[i].slot] = (uint8) ref[i].count;
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
refs_delta_size(const xl_bmap_refs_delta *d)
{
	return sizeof(xl_bmap_refs_delta) + d->nrefs * sizeof(xl_bmap_ref_delta);
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
		RelFileLocator rloc = with_relnumber(rel, BMAP_REFS_RELNUMBER);
		SMgrRelation asrel = alloc_smgr(rel);
		SMgrRelation ssrel = store_smgr(rel);
		SMgrRelation rsrel = refs_smgr(rel);
		Buffer		buf;
		Page		page;

		if (!smgrexists(ssrel, MAIN_FORKNUM))
		{
			smgrcreate(ssrel, MAIN_FORKNUM, false);
			log_smgrcreate(&sloc, MAIN_FORKNUM);
		}
		if (!smgrexists(rsrel, MAIN_FORKNUM))
		{
			smgrcreate(rsrel, MAIN_FORKNUM, false);
			log_smgrcreate(&rloc, MAIN_FORKNUM);
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

/*
 * Give every physical block below 'hw' a reference-count page, made (all
 * zeroes, counting zero) before any block it covers is allocated.  Called
 * with the allocator's metapage locked exclusively, which serializes the
 * file's growth.  The new pages are not logged: a record that changes one
 * initializes it, and redo extends the file to reach it.  A database whose
 * allocator predates reference counts gets the file here.
 */
static void
refs_cover(RelFileLocator rel, BlockNumber hw)
{
	SMgrRelation rsrel = refs_smgr(rel);
	BlockNumber need = hw == 0 ? 0 : REF_PAGE(hw - 1) + 1;

	if (!smgrexists(rsrel, MAIN_FORKNUM))
	{
		RelFileLocator rloc = with_relnumber(rel, BMAP_REFS_RELNUMBER);

		smgrcreate(rsrel, MAIN_FORKNUM, false);
		log_smgrcreate(&rloc, MAIN_FORKNUM);
	}
	check_may_read();
	if (need > 0 && smgrnblocks(rsrel, MAIN_FORKNUM) < need)
		ReleaseBuffer(ExtendBufferedRelTo(BMR_SMGR(rsrel, RELPERSISTENCE_PERMANENT),
										  MAIN_FORKNUM, NULL,
										  EB_SKIP_EXTENSION_LOCK, need,
										  RBM_NORMAL));
}

/*
 * How many reference-count pages exist: blocks whose page is past them
 * count zero (a database whose allocator predates reference counts).
 */
static BlockNumber
refs_nblocks(RelFileLocator rel)
{
	SMgrRelation rsrel = refs_smgr(rel);

	if (!smgrexists(rsrel, MAIN_FORKNUM))
		return 0;
	return smgrnblocks(rsrel, MAIN_FORKNUM);
}

/* ----------------------------------------------------------------
 * Lookups
 * ----------------------------------------------------------------
 */

/*
 * Logical block 'blkno''s entry, read under a share lock.  False if it has
 * none: past the map, or on a map page left blank (added to the map fork
 * by an extension that failed or whose record a crash lost).
 */
static bool
lookup_entry(SMgrRelation reln, BlockNumber blkno, BmapEntry *entry)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		buf;
	bool		found = false;

	if (MAP_PAGE(blkno) >= mdnblocks(reln, BMAP_FORKNUM))
		return false;
	buf = map_read(rel, MAP_PAGE(blkno));
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	if (!PageIsNew(BufferGetPage(buf)))
	{
		check_page(BufferGetPage(buf), BMAP_PAGE_NODE, rel, BMAP_FORKNUM,
				   MAP_PAGE(blkno));
		*entry = PageGetBmapEntries(BufferGetPage(buf))[MAP_SLOT(blkno)];
		found = BlockNumberIsValid(entry->block);
	}
	UnlockReleaseBuffer(buf);
	return found;
}

/* the map's epoch, read under a share lock */
static uint32
lookup_epoch(SMgrRelation reln)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		buf = map_read(rel, 0);
	uint32		epoch;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(buf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	epoch = PageGetBmapHeader(BufferGetPage(buf))->epoch;
	UnlockReleaseBuffer(buf);
	return epoch;
}

/* the reference count of 'phys', read under a share lock */
static uint8
lookup_ref(RelFileLocator rel, BlockNumber phys)
{
	Buffer		buf;
	uint8		count;

	if (REF_PAGE(phys) >= refs_nblocks(rel))
		return 0;
	buf = refs_read(rel, REF_PAGE(phys));
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	check_refs_page(BufferGetPage(buf), rel, REF_PAGE(phys));
	count = page_ref(BufferGetPage(buf), phys);
	UnlockReleaseBuffer(buf);
	return count;
}

/*
 * Copy map page 'blk' of 'rel', pinned in 'buf', into 'dst' under a share
 * lock.  A page of zeroes (an extension's whose record a crash lost) is
 * copied as such if 'new_ok' (returning false), else fails verification.
 */
static bool
copy_node(RelFileLocator rel, Buffer buf, BlockNumber blk, char *dst,
		  uint16 type, bool new_ok)
{
	bool		isnew;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	isnew = new_ok && PageIsNew(BufferGetPage(buf));
	if (!isnew)
		check_page(BufferGetPage(buf), type, rel, BMAP_FORKNUM, blk);
	memcpy(dst, BufferGetPage(buf), BLCKSZ);
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	return !isnew;
}

/*
 * Copy map page 'blk' of 'reln' into 'dst' (I/O-aligned).
 *
 * Writing a block-mapped page looks the page up, and a write may be the
 * eviction a buffer read needs; if the lookup read the map through the
 * buffer manager, it could need an eviction of its own, which could need a
 * lookup, and so on until no buffer is left.  So a lookup 'for_write' never
 * loads a map page into shared buffers: it uses the page if it is cached,
 * and otherwise reads it from disk, where an uncached page has every entry
 * a write can need: an entry's block changes only while its logical block
 * can't be written, so a write after the change finds the page cached or
 * written out.  A torn read fails verification and is read again.
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

		/*
		 * A change locks a map page only after its reads (map_lock()); one
		 * that didn't would wait here on its own lock, forever.
		 */
		if (BufferIsLockedByMe(buf))
			elog(ERROR, "map page %u of \"%s\" is locked by the backend writing a page it maps",
				 blk, relpathperm(rel, MAIN_FORKNUM).str);
	}
	else
		buf = map_read(rel, blk);

	(void) copy_node(rel, buf, blk, dst, type, false);
	ReleaseBuffer(buf);
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
 * Blocks taken from the allocator: its metapage and the bitmap pages that
 * record them, locked exclusively, with their deltas, for one record.
 */
#define BMAP_ALLOC_MAX_PAGES	(1 + BMAP_SCAN_PAGES + 2)

typedef struct Allocation
{
	Buffer		metabuf;
	BlockNumber old_hw;
	BlockNumber new_hw;
	BitsChange	bits[BMAP_SCAN_PAGES + 2];
	int			nbits;
	xl_bmap_meta_delta *mdelta;
} Allocation;

/*
 * Take 'nneed' physical blocks (at most BMAP_PIECE) into taken[]: freed
 * blocks first, then the page store's high-water mark.  Freed blocks are
 * not taken for a relation whose data skips WAL (made in this transaction
 * at wal_level minimal): its pages are synced at commit, not logged, so
 * nothing in redo would restore them if replaying an earlier record of the
 * block's previous owner zeroed them.  Blocks above the high-water mark
 * were never in an earlier record.  New blocks get their reference-count
 * pages here, before they are mapped.
 */
static void
alloc_take(RelFileLocator rel, uint32 nneed, BlockNumber *taken,
		   Allocation *a)
{
	BmapMeta   *meta;
	uint32		ntaken = 0;
	BlockNumber added[2];
	int			nadded = 0;
	BlockNumber new_hw;
	uint32		new_nfree;
	uint32		new_hint;

	Assert(nneed <= BMAP_PIECE);
	a->nbits = 0;
	a->metabuf = alloc_read(rel, 0);
	LockBuffer(a->metabuf, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(a->metabuf), BMAP_PAGE_META, rel, MAIN_FORKNUM, 0);
	meta = PageGetBmapMeta(BufferGetPage(a->metabuf));
	a->old_hw = new_hw = meta->hw;
	new_nfree = meta->nfree;
	new_hint = meta->hint;

	if (nneed > 0 && meta->nfree > 0 && !RelFileLocatorSkippingWAL(rel) &&
		!sweep_running(rel))
	{
		for (uint32 k = 0; k < meta->nbitmap && ntaken < nneed &&
			 a->nbits < BMAP_SCAN_PAGES; k++)
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
				 base + b < a->old_hw; b++)
			{
				if (b % 64 == 0 && words[b / 64] == PG_UINT64_MAX)
				{
					b += 63;
					continue;
				}
				if ((words[b / 64] & (UINT64CONST(1) << (b % 64))) == 0)
				{
					if (bc == NULL)
						bc = bits_change(a->bits, &a->nbits, buf, pageno, 1,
										 false, BMAP_PIECE);
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

		for (int k = 0; k < a->nbits; k++)
			if (a->bits[k].pageno == pageno)
				bc = &a->bits[k];
		if (bc == NULL)
		{
			if (pageno < meta->nbitmap)
			{
				Buffer		buf = alloc_read(rel, meta->bitmap[pageno]);

				LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
				check_page(BufferGetPage(buf), BMAP_PAGE_BITMAP, rel,
						   MAIN_FORKNUM, meta->bitmap[pageno]);
				bc = bits_change(a->bits, &a->nbits, buf, pageno, 1, false,
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
				bc = bits_change(a->bits, &a->nbits, buf, pageno, 1, true,
								 BMAP_PIECE);
			}
		}
		((uint32 *) (bc->delta + 1))[bc->delta->nbits++] = blk % BMAP_BITS_PER_PAGE;
		taken[ntaken++] = blk;
	}
	a->new_hw = new_hw;

	if (new_hw > a->old_hw)
		refs_cover(rel, new_hw);

	a->mdelta = palloc0(sizeof(xl_bmap_meta_delta) + 2 * sizeof(BlockNumber));
	a->mdelta->kind = BMAP_DELTA_META;
	a->mdelta->hw = new_hw;
	a->mdelta->nfree = new_nfree;
	a->mdelta->hint = new_hint;
	a->mdelta->nbitmap_added = nadded;
	for (int i = 0; i < nadded; i++)
		((BlockNumber *) (a->mdelta + 1))[i] = added[i];
}

/*
 * In the record's critical section: apply the allocation's deltas and
 * register its pages from block id 'first'.  Returns the next id.
 */
static int
alloc_apply_register(Allocation *a, int first)
{
	int			id = first;

	apply_meta_delta(BufferGetPage(a->metabuf), a->mdelta);
	MarkBufferDirty(a->metabuf);
	XLogRegisterBuffer(id, a->metabuf, REGBUF_STANDARD);
	XLogRegisterBufData(id, a->mdelta, meta_delta_size(a->mdelta));
	id++;
	for (int i = 0; i < a->nbits; i++)
	{
		apply_bits_delta(BufferGetPage(a->bits[i].buf), a->bits[i].delta);
		MarkBufferDirty(a->bits[i].buf);
		XLogRegisterBuffer(id, a->bits[i].buf, REGBUF_STANDARD |
						   ((a->bits[i].delta->flags & BMAP_DELTA_INIT) ?
							REGBUF_WILL_INIT : 0));
		XLogRegisterBufData(id, a->bits[i].delta,
							bits_delta_size(a->bits[i].delta));
		id++;
	}
	return id;
}

static void
alloc_set_lsn(Allocation *a, XLogRecPtr lsn)
{
	PageSetLSN(BufferGetPage(a->metabuf), lsn);
	for (int i = 0; i < a->nbits; i++)
		PageSetLSN(BufferGetPage(a->bits[i].buf), lsn);
}

static void
alloc_release(Allocation *a)
{
	for (int i = 0; i < a->nbits; i++)
	{
		UnlockReleaseBuffer(a->bits[i].buf);
		pfree(a->bits[i].delta);
	}
	UnlockReleaseBuffer(a->metabuf);
	pfree(a->mdelta);
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
	BlockNumber phys[BMAP_PIECE];
	bool		need[BMAP_PIECE];
	uint32		nneed = 0;
	BlockNumber reused[BMAP_PIECE];
	uint32		nreused = 0;
	BlockNumber taken[BMAP_PIECE];
	Allocation	alloc;
	BlockNumber old_hw;
	BlockNumber new_hw;
	xl_bmap_header_delta hdelta;
	xl_bmap_node_delta *ndelta;
	xl_bmap_extend xlrec;
	XLogRecPtr	lsn;
	uint32		epoch = PageGetBmapHeader(BufferGetPage(headerbuf))->epoch;

	Assert(n > 0 && n <= BMAP_PIECE);
	Assert(MAP_PAGE(to - 1) == mapblk);

	/*
	 * The map page, added if the map fork doesn't reach it yet.  An existing
	 * page is read now and locked only after the allocator's pages have been
	 * read: a read may write out one of this relation's pages, which looks
	 * it up on this map page.  Its entries in [from, to) can't change
	 * meanwhile, since the header serializes the relation's extensions.  A
	 * new page (or one of zeroes) maps no page a write could look up, so it
	 * stays locked while the allocator's pages are read: the exception
	 * map_lock() allows.
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
		map_locked_as_new(mapbuf);
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
		map_lock(mapbuf, true);
		if (PageIsNew(BufferGetPage(mapbuf)))
			mapnew = true;
		else
		{
			map_unlock(mapbuf);
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
	alloc_take(rel, nneed, taken, &alloc);
	old_hw = alloc.old_hw;
	new_hw = alloc.new_hw;

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
		map_lock(mapbuf, false);

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
	hdelta.epoch = epoch;
	hdelta.fold_epoch = PageGetBmapHeader(BufferGetPage(headerbuf))->fold_epoch;
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
		ent[ndelta->nentries].epoch = epoch;
		ndelta->nentries++;
	}
	xlrec.locator = rel;
	xlrec.old_hw = old_hw;
	xlrec.new_hw = new_hw;
	xlrec.flags = RelFileLocatorSkippingWAL(rel) ? BMAP_EXTEND_SKIP_ZERO : 0;
	xlrec.nreused = nreused;

	XLogEnsureRecordSpace(2 + BMAP_ALLOC_MAX_PAGES, 0);
	ndelta->since = GetXLogInsertRecPtr();

	START_CRIT_SECTION();
	apply_header_delta(BufferGetPage(headerbuf), &hdelta);
	MarkBufferDirty(headerbuf);
	apply_node_delta(BufferGetPage(mapbuf), ndelta);
	MarkBufferDirty(mapbuf);

	XLogBeginInsert();
	XLogRegisterData(&xlrec, SizeOfBmapExtend);
	if (nreused > 0)
		XLogRegisterData(reused, nreused * sizeof(BlockNumber));
	XLogRegisterBuffer(0, headerbuf, REGBUF_STANDARD);
	XLogRegisterBufData(0, &hdelta, sizeof(hdelta));
	XLogRegisterBuffer(1, mapbuf, REGBUF_STANDARD |
					   (mapnew ? REGBUF_WILL_INIT : 0));
	XLogRegisterBufData(1, ndelta, node_delta_size(ndelta));
	alloc_apply_register(&alloc, 2);
	lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_EXTEND);

	PageSetLSN(BufferGetPage(headerbuf), lsn);
	PageSetLSN(BufferGetPage(mapbuf), lsn);
	alloc_set_lsn(&alloc, lsn);
	END_CRIT_SECTION();

	alloc_release(&alloc);
	map_unlock(mapbuf);
	ReleaseBuffer(mapbuf);
	pfree(ndelta);
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
		/* a map never shared has no entry born before its epoch */
		if (PageGetBmapHeader(BufferGetPage(headerbuf))->epoch > 0)
			unshare_stale(reln, PageGetBmapHeader(BufferGetPage(headerbuf))->epoch,
						  PageGetBmapHeader(BufferGetPage(headerbuf))->fold_epoch,
						  nblocks, end);
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
 * At most this many of one bitmap page's bits are freed in one record: a
 * record holds no more than UINT16_MAX bytes of one page's data, and a
 * page's bits are many more.
 */
#define BMAP_FREE_BITS_MAX	8192

StaticAssertDecl(sizeof(xl_bmap_bits_delta) + BMAP_FREE_BITS_MAX * sizeof(uint32) <= UINT16_MAX,
				 "a bitmap page's freed bits must fit one record's data for the page");

/*
 * Clear the allocator's bits of the physical blocks blocks[0 .. n), sorted,
 * in records of a few bitmap pages each (a page with many, in several).
 * The records aren't one: a crash between them leaves the blocks not yet
 * freed allocated, held by nothing, until a page sweep (bmap_sweep())
 * frees what no map holds.
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
			bool		full = false;

			if (pageno >= meta->nbitmap || blocks[i] >= meta->hw)
				elog(ERROR, "block-mapped storage of database %u frees block %u past its end",
					 rel.dbOid, blocks[i]);
			while (end < n && blocks[end] / BMAP_BITS_PER_PAGE == pageno)
			{
				if (end - i == BMAP_FREE_BITS_MAX)
				{
					full = true;
					break;
				}
				end++;
			}
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
			if (full)
				break;			/* (the rest of this page, in the next record) */
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

/* ----------------------------------------------------------------
 * Reference counts
 * ----------------------------------------------------------------
 */

/*
 * Reference-count pages locked for one record, each with the delta that
 * will change it.
 */
typedef struct RefsPages
{
	int			n;
	BlockNumber blk[BMAP_REFS_PER_RECORD];
	Buffer		buf[BMAP_REFS_PER_RECORD];
	xl_bmap_refs_delta *delta[BMAP_REFS_PER_RECORD];
} RefsPages;

/*
 * Lock, exclusively and in order, the reference-count pages of the physical
 * blocks phys[0 .. n), which are sorted: as many as one record can change.
 * Returns how many of the blocks they cover.
 */
static uint32
refs_lock(RelFileLocator rel, const BlockNumber *phys, uint32 n, RefsPages *rp)
{
	uint32		i;
	BlockNumber last = InvalidBlockNumber;

	rp->n = 0;
	for (i = 0; i < n; i++)
	{
		BlockNumber blk = REF_PAGE(phys[i]);
		xl_bmap_refs_delta *delta;

		if (blk == last)
			continue;
		Assert(!BlockNumberIsValid(last) || last < blk);
		if (rp->n == BMAP_REFS_PER_RECORD)
			break;
		last = blk;
		rp->blk[rp->n] = blk;
		rp->buf[rp->n] = refs_read(rel, blk);
		LockBuffer(rp->buf[rp->n], BUFFER_LOCK_EXCLUSIVE);
		check_refs_page(BufferGetPage(rp->buf[rp->n]), rel, blk);
		delta = palloc0(sizeof(xl_bmap_refs_delta) + n * sizeof(xl_bmap_ref_delta));
		delta->kind = BMAP_DELTA_REFS;
		if (PageIsNew(BufferGetPage(rp->buf[rp->n])))
			delta->flags |= BMAP_DELTA_INIT;
		rp->delta[rp->n] = delta;
		rp->n++;
	}
	return i;
}

static int
refs_find(RefsPages *rp, BlockNumber phys)
{
	for (int k = 0; k < rp->n; k++)
		if (rp->blk[k] == REF_PAGE(phys))
			return k;
	elog(ERROR, "reference count of block %u is not locked", phys);
	return -1;					/* keep compiler quiet */
}

/* the count of a block on the locked pages */
static uint8
refs_get(RefsPages *rp, BlockNumber phys)
{
	return page_ref(BufferGetPage(rp->buf[refs_find(rp, phys)]), phys);
}

/* set a block's count when the record is made */
static void
refs_set(RefsPages *rp, BlockNumber phys, uint8 count)
{
	xl_bmap_refs_delta *delta = rp->delta[refs_find(rp, phys)];
	xl_bmap_ref_delta *ref = (xl_bmap_ref_delta *) (delta + 1);

	ref[delta->nrefs].slot = REF_SLOT(phys);
	ref[delta->nrefs].count = count;
	delta->nrefs++;
}

/*
 * In the record's critical section: apply the deltas of the pages that
 * change, and register them from block id 'first'.  Returns the next id.
 */
static int
refs_apply_register(RefsPages *rp, int first)
{
	int			id = first;

	for (int k = 0; k < rp->n; k++)
	{
		if (rp->delta[k]->nrefs == 0)
			continue;
		apply_refs_delta(BufferGetPage(rp->buf[k]), rp->delta[k]);
		MarkBufferDirty(rp->buf[k]);
		XLogRegisterBuffer(id, rp->buf[k], REGBUF_STANDARD |
						   ((rp->delta[k]->flags & BMAP_DELTA_INIT) ?
							REGBUF_WILL_INIT : 0));
		XLogRegisterBufData(id, rp->delta[k], refs_delta_size(rp->delta[k]));
		id++;
	}
	return id;
}

static void
refs_set_lsn(RefsPages *rp, XLogRecPtr lsn)
{
	for (int k = 0; k < rp->n; k++)
		if (rp->delta[k]->nrefs > 0)
			PageSetLSN(BufferGetPage(rp->buf[k]), lsn);
}

static void
refs_release(RefsPages *rp)
{
	for (int k = 0; k < rp->n; k++)
	{
		UnlockReleaseBuffer(rp->buf[k]);
		pfree(rp->delta[k]);
	}
	rp->n = 0;
}

/*
 * Log a record that changes the reference counts of the pages rp locked
 * and, if mapbuf is valid, a map page by ndelta (registered with
 * REGBUF_STANDARD | mapflags): apply the changes, register the pages,
 * insert, and set the pages' LSNs.
 */
static void
log_refs_record(uint8 info, const void *xlrec, uint32 xlrec_size,
				Buffer mapbuf, xl_bmap_node_delta *ndelta, int mapflags,
				RefsPages *rp)
{
	int			first = BufferIsValid(mapbuf) ? 1 : 0;
	XLogRecPtr	lsn;

	/* the main data, the map page's delta and one delta per count page */
	XLogEnsureRecordSpace(first + BMAP_REFS_PER_RECORD,
						  1 + first + BMAP_REFS_PER_RECORD);
	if (first)
		ndelta->since = GetXLogInsertRecPtr();
	START_CRIT_SECTION();
	if (first)
	{
		apply_node_delta(BufferGetPage(mapbuf), ndelta);
		MarkBufferDirty(mapbuf);
	}
	XLogBeginInsert();
	XLogRegisterData(xlrec, xlrec_size);
	if (first)
	{
		XLogRegisterBuffer(0, mapbuf, REGBUF_STANDARD | mapflags);
		XLogRegisterBufData(0, ndelta, node_delta_size(ndelta));
	}
	refs_apply_register(rp, first);
	lsn = XLogInsert(RM_BMAP_ID, info);
	if (first)
		PageSetLSN(BufferGetPage(mapbuf), lsn);
	refs_set_lsn(rp, lsn);
	END_CRIT_SECTION();
}

/*
 * Drop one reference to each of the physical blocks blocks[0 .. n), sorted,
 * held by a map that is going away.  A block no other map holds (count
 * zero) goes into tofree[] for the caller to free; a stuck count stays.
 * Returns how many blocks are to be freed.
 */
static uint32
unref_blocks(RelFileLocator rel, const BlockNumber *blocks, uint32 n,
			 BlockNumber *tofree)
{
	BlockNumber nrefpages = refs_nblocks(rel);
	uint32		nfree = 0;
	uint32		i = 0;

	while (i < n)
	{
		RefsPages	rp;
		uint32		end = i;
		uint32		covered;
		bool		changed = false;

		/* blocks past the counts' pages count zero */
		if (REF_PAGE(blocks[i]) >= nrefpages)
		{
			tofree[nfree++] = blocks[i++];
			continue;
		}
		while (end < n && REF_PAGE(blocks[end]) < nrefpages)
			end++;

		covered = refs_lock(rel, blocks + i, end - i, &rp);
		for (uint32 k = i; k < i + covered; k++)
		{
			uint8		c = refs_get(&rp, blocks[k]);

			if (c == BMAP_REFS_STUCK)
			{
				stuck_released(rel);
				continue;
			}
			if (c > 0)
			{
				refs_set(&rp, blocks[k], c - 1);
				changed = true;
			}
			else
				tofree[nfree++] = blocks[k];
		}
		if (changed)
		{
			xl_bmap_unref xlrec;

			xlrec.locator = rel;
			xlrec.flags = 0;
			log_refs_record(XLOG_BMAP_UNREF, &xlrec, sizeof(xlrec),
							InvalidBuffer, NULL, 0, &rp);
		}
		refs_release(&rp);
		i += covered;
	}
	return nfree;
}

/* an entry of a map page, with its slot, for sorting by physical block */
typedef struct SlotEntry
{
	uint32		slot;
	BlockNumber block;
	uint32		epoch;
} SlotEntry;

static int
cmp_slot_entry(const void *a, const void *b)
{
	const SlotEntry *x = (const SlotEntry *) a;
	const SlotEntry *y = (const SlotEntry *) b;

	return pg_cmp_u32(x->block, y->block);
}

/*
 * Before an extension reuses the entries of blocks [from, to), on one map
 * page, that a truncation left past the relation's end: those born before
 * the map's epoch may be shared.  One no other map holds is claimed (born
 * again in this epoch); one another map holds is let go (its count drops)
 * and cleared, to be allocated anew.  The header is locked exclusively, so
 * those entries can't change meanwhile; the map page is locked only after
 * each record's other pages, since reading them may write out one of this
 * relation's pages, which looks it up on the map page.
 */
static void
unshare_stale(SMgrRelation reln, uint32 epoch, uint32 fold_epoch,
			  BlockNumber from, BlockNumber to)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	BlockNumber mapblk = MAP_PAGE(from);
	Buffer		mapbuf;
	PGIOAlignedBlock copy;
	BmapEntry	stale[BMAP_PIECE];
	SlotEntry	items[BMAP_PIECE];
	BlockNumber phys[BMAP_PIECE];
	uint32		nitems = 0;
	BlockNumber nrefpages;
	uint32		i = 0;

	if (mapblk >= mdnblocks(reln, BMAP_FORKNUM))
		return;
	mapbuf = map_read(rel, mapblk);
	if (!copy_node(rel, mapbuf, mapblk, copy.data, BMAP_PAGE_NODE, true))
	{
		ReleaseBuffer(mapbuf);
		return;
	}
	memcpy(stale, &PageGetBmapEntries(copy.data)[MAP_SLOT(from)],
		   (to - from) * sizeof(BmapEntry));
	for (BlockNumber blk = from; blk < to; blk++)
	{
		BmapEntry  *e = &stale[blk - from];

		if (!BlockNumberIsValid(e->block))
			continue;

		/*
		 * A page as of the relation's fold goes on its changes list, which
		 * takes the map's reference: the fold's state still has it.
		 */
		if (e->epoch < fold_epoch)
			list_stale(reln, mapbuf, blk, e->block, epoch);
		else if (e->epoch < epoch)
		{
			items[nitems].slot = MAP_SLOT(blk);
			items[nitems].block = e->block;
			items[nitems].epoch = e->epoch;
			nitems++;
		}
	}
	if (nitems == 0)
	{
		ReleaseBuffer(mapbuf);
		return;
	}
	qsort(items, nitems, sizeof(SlotEntry), cmp_slot_entry);
	for (uint32 k = 0; k < nitems; k++)
		phys[k] = items[k].block;
	nrefpages = refs_nblocks(rel);

	while (i < nitems)
	{
		RefsPages	rp;
		xl_bmap_node_delta *ndelta;
		xl_bmap_entry_delta *ent;
		xl_bmap_unref xlrec;
		uint32		end = i;
		uint32		covered = 0;

		ndelta = palloc0(sizeof(xl_bmap_node_delta) + nitems * sizeof(xl_bmap_entry_delta));
		ndelta->kind = BMAP_DELTA_NODE;
		ent = (xl_bmap_entry_delta *) (ndelta + 1);

		while (end < nitems && REF_PAGE(phys[end]) < nrefpages)
			end++;
		rp.n = 0;
		if (end > i)
			covered = refs_lock(rel, phys + i, end - i, &rp);
		else
			/* blocks past the counts' pages count zero: all claimed */
			covered = nitems - i;
		map_lock(mapbuf, false);

		for (uint32 k = i; k < i + covered; k++)
		{
			uint8		c = end > i ? refs_get(&rp, phys[k]) : 0;

			ent[ndelta->nentries].slot = items[k].slot;
			if (c == 0)
			{
				/* no other map holds it: it is ours */
				ent[ndelta->nentries].block = items[k].block;
				ent[ndelta->nentries].epoch = epoch;
			}
			else
			{
				if (c != BMAP_REFS_STUCK)
					refs_set(&rp, phys[k], c - 1);
				else
					stuck_released(rel);
				ent[ndelta->nentries].block = InvalidBlockNumber;
				ent[ndelta->nentries].epoch = 0;
			}
			ndelta->nentries++;
		}

		xlrec.locator = rel;
		xlrec.flags = BMAP_UNREF_MAP;
		log_refs_record(XLOG_BMAP_UNREF, &xlrec, sizeof(xlrec),
						mapbuf, ndelta, 0, &rp);

		map_unlock(mapbuf);
		refs_release(&rp);
		pfree(ndelta);
		i += covered;
	}
	ReleaseBuffer(mapbuf);
}

/* a main fork's logical blocks can be given other physical blocks */
bool
bmapreplacesblocks(SMgrRelation reln, ForkNumber forknum)
{
	return forknum == MAIN_FORKNUM;
}

/*
 * In recovery: was logical block 'blocknum' given its physical block after
 * WAL position 'lsn'?  The map page read may be newer than the record being
 * replayed (written before a crash, or copied by a base backup), or not yet
 * initialized.
 */
bool
bmapreplacedafter(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
				  XLogRecPtr lsn)
{
	BmapEntry	entry;

	return lookup_entry(reln, blocknum, &entry) && entry.since > lsn;
}

/* ----------------------------------------------------------------
 * Folds: the changes list
 * ----------------------------------------------------------------
 */

static Buffer
chg_read(RelFileLocator rel, BlockNumber blkno)
{
	check_may_read();
	return ReadBufferWithoutRelcache(rel, BMAP_CHANGES_FORKNUM, blkno,
									 RBM_NORMAL, NULL, true);
}

static bool
chg_exists(SMgrRelation reln)
{
	return mdexists(reln, BMAP_CHANGES_FORKNUM);
}

/* Add a page to a relation's changes fork, returned zeroed and locked. */
static Buffer
chg_new_page(SMgrRelation reln)
{
	check_may_read();
	return ExtendBufferedRel(BMR_SMGR(reln, RELPERSISTENCE_PERMANENT),
							 BMAP_CHANGES_FORKNUM, NULL,
							 EB_LOCK_FIRST | EB_SKIP_EXTENSION_LOCK);
}

static void
init_changes_head(Page page)
{
	init_page(page, BMAP_PAGE_CHANGES_HEAD);
	PageGetBmapChangesHead(page)->version = BMAP_VERSION;
}

static void
apply_changes_head_delta(Page page, const xl_bmap_changes_head_delta *delta)
{
	BmapChangesHead *head;

	if (delta->flags & BMAP_DELTA_INIT)
		init_changes_head(page);
	head = PageGetBmapChangesHead(page);
	head->fold_epoch = delta->fold_epoch;
	head->lineage = delta->lineage;
	head->fold_nblocks = delta->fold_nblocks;
	head->nentries = delta->nentries;
	head->pruned_epoch = delta->pruned_epoch;
}

static void
apply_changes_delta(Page page, const xl_bmap_changes_delta *delta)
{
	const BmapChange *changes = (const BmapChange *) (delta + 1);

	if (delta->flags & BMAP_DELTA_INIT)
		init_page(page, BMAP_PAGE_CHANGES);
	Assert(delta->first + delta->n <= BMAP_CHANGES_PER_PAGE);
	memcpy(PageGetBmapChanges(page) + delta->first, changes,
		   delta->n * sizeof(BmapChange));
}

static Size
changes_delta_size(const xl_bmap_changes_delta *d)
{
	return sizeof(xl_bmap_changes_delta) + d->n * sizeof(BmapChange);
}

/*
 * The epoch of the relation's last fold (0: none), from the map's header,
 * a buffer each change reads anyway.
 */
static uint32
changes_fold_epoch(SMgrRelation reln)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		buf = map_read(rel, 0);
	uint32		epoch;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(buf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	epoch = PageGetBmapHeader(BufferGetPage(buf))->fold_epoch;
	UnlockReleaseBuffer(buf);
	return epoch;
}

/*
 * An entry being added to a relation's changes list, with the pages its
 * record changes: the list's header and the page the entry goes on, locked
 * exclusively (after the caller's other pages, but before a map page), and
 * their deltas.
 */
typedef struct ChangesAppend
{
	Buffer		headbuf;
	Buffer		pagebuf;
	xl_bmap_changes_head_delta hdelta;
	struct
	{
		xl_bmap_changes_delta d;
		BmapChange	c;
	}			pdelta;
} ChangesAppend;

static void set_length_listed(SMgrRelation reln, RelFileLocator rel,
							  Buffer headerbuf, BlockNumber nblocks,
							  ChangesAppend *ca);

/*
 * Page 'pageno' of reln's changes list, where its next entry goes, locked
 * exclusively: the list's fork is extended to it if it isn't there yet.
 */
static Buffer
changes_next_page(SMgrRelation reln, BlockNumber pageno)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		buf;

	if (pageno < mdnblocks(reln, BMAP_CHANGES_FORKNUM))
	{
		buf = chg_read(rel, pageno);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		return buf;
	}
	buf = chg_new_page(reln);
	if (BufferGetBlockNumber(buf) != pageno)
		elog(ERROR, "changes list of \"%s\" extended to block %u, expected %u",
			 relpathperm(rel, MAIN_FORKNUM).str, BufferGetBlockNumber(buf), pageno);
	return buf;
}

/*
 * Lock the list's header and the page entry number 'n' goes on, which the
 * header says is next: the page is added to the fork if it hasn't been, and
 * made anew if the entry is its first.  'headbuf' and 'pagebuf' may be
 * pinned already (for a truncation, in a critical section).
 */
static void
changes_append_lock(SMgrRelation reln, ChangesAppend *ca, const BmapChange *c)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	BmapChangesHead *head;
	BlockNumber pageno;

	if (!BufferIsValid(ca->headbuf))
		ca->headbuf = chg_read(rel, 0);
	LockBuffer(ca->headbuf, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(ca->headbuf), BMAP_PAGE_CHANGES_HEAD, rel,
			   BMAP_CHANGES_FORKNUM, 0);
	head = PageGetBmapChangesHead(BufferGetPage(ca->headbuf));
	pageno = CHANGE_PAGE(head->nentries);
	if (BufferIsValid(ca->pagebuf) && BufferGetBlockNumber(ca->pagebuf) != pageno)
		elog(PANIC, "changes list of \"%s\" grew while its truncation was prepared",
			 relpathperm(rel, MAIN_FORKNUM).str);
	if (!BufferIsValid(ca->pagebuf))
		ca->pagebuf = changes_next_page(reln, pageno);
	else
		LockBuffer(ca->pagebuf, BUFFER_LOCK_EXCLUSIVE);

	ca->hdelta.kind = BMAP_DELTA_CHANGES_HEAD;
	ca->hdelta.flags = 0;
	ca->hdelta.fold_epoch = head->fold_epoch;
	ca->hdelta.lineage = head->lineage;
	ca->hdelta.fold_nblocks = head->fold_nblocks;
	ca->hdelta.nentries = head->nentries + 1;
	ca->hdelta.pruned_epoch = head->pruned_epoch;
	ca->pdelta.d.kind = BMAP_DELTA_CHANGES;
	ca->pdelta.d.flags = CHANGE_SLOT(head->nentries) == 0 ? BMAP_DELTA_INIT : 0;
	ca->pdelta.d.first = CHANGE_SLOT(head->nentries);
	ca->pdelta.d.n = 1;
	ca->pdelta.c = *c;
}

/* In the record's critical section: apply and register, from block 'id'. */
static int
changes_append_register(ChangesAppend *ca, int id)
{
	apply_changes_head_delta(BufferGetPage(ca->headbuf), &ca->hdelta);
	MarkBufferDirty(ca->headbuf);
	XLogRegisterBuffer(id, ca->headbuf, REGBUF_STANDARD);
	XLogRegisterBufData(id, &ca->hdelta, sizeof(ca->hdelta));
	apply_changes_delta(BufferGetPage(ca->pagebuf), &ca->pdelta.d);
	MarkBufferDirty(ca->pagebuf);
	XLogRegisterBuffer(id + 1, ca->pagebuf, REGBUF_STANDARD |
					   ((ca->pdelta.d.flags & BMAP_DELTA_INIT) ? REGBUF_WILL_INIT : 0));
	XLogRegisterBufData(id + 1, &ca->pdelta, sizeof(ca->pdelta));
	return id + 2;
}

static void
changes_append_finish(ChangesAppend *ca, XLogRecPtr lsn)
{
	PageSetLSN(BufferGetPage(ca->headbuf), lsn);
	PageSetLSN(BufferGetPage(ca->pagebuf), lsn);
	UnlockReleaseBuffer(ca->pagebuf);
	UnlockReleaseBuffer(ca->headbuf);
	ca->headbuf = ca->pagebuf = InvalidBuffer;
}

/*
 * Move logical block 'blkno''s entry, block 'old_block' in map page 'mapbuf'
 * (pinned), to reln's changes list: a copy of the page, which the list
 * holds as it was.  The map's entry is left empty.  The map page is locked
 * after the list's pages.
 */
static void
list_stale(SMgrRelation reln, Buffer mapbuf, BlockNumber blkno,
		   BlockNumber old_block, uint32 epoch)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	ChangesAppend ca = {InvalidBuffer, InvalidBuffer};
	BmapChange	c;
	xl_bmap_node_delta *ndelta;
	xl_bmap_entry_delta *ent;
	xl_bmap_unref xlrec;
	XLogRecPtr	lsn;

	c.epoch = epoch;
	c.blkno = blkno;
	c.old_block = old_block;
	c.kind = BMAP_CHANGE_COPY;
	changes_append_lock(reln, &ca, &c);
	map_lock(mapbuf, false);

	ndelta = palloc0(sizeof(xl_bmap_node_delta) + sizeof(xl_bmap_entry_delta));
	ndelta->kind = BMAP_DELTA_NODE;
	ndelta->nentries = 1;
	ent = (xl_bmap_entry_delta *) (ndelta + 1);
	ent->slot = MAP_SLOT(blkno);
	ent->block = InvalidBlockNumber;
	ent->epoch = 0;
	xlrec.locator = rel;
	xlrec.flags = BMAP_UNREF_MAP;

	XLogEnsureRecordSpace(3, 0);
	ndelta->since = GetXLogInsertRecPtr();
	START_CRIT_SECTION();
	apply_node_delta(BufferGetPage(mapbuf), ndelta);
	MarkBufferDirty(mapbuf);
	XLogBeginInsert();
	XLogRegisterData(&xlrec, sizeof(xlrec));
	XLogRegisterBuffer(0, mapbuf, REGBUF_STANDARD);
	XLogRegisterBufData(0, ndelta, node_delta_size(ndelta));
	(void) changes_append_register(&ca, 1);
	lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_UNREF);
	PageSetLSN(BufferGetPage(mapbuf), lsn);
	END_CRIT_SECTION();
	map_unlock(mapbuf);
	changes_append_finish(&ca, lsn);
	pfree(ndelta);
}

/*
 * Every entry of a relation's changes list, and its header, read under
 * share locks, or under the caller's lock of the header if 'headbuf' is
 * valid; NULL (and *n zero) if it has none.
 */
static BmapChange *
changes_read(SMgrRelation reln, Buffer headbuf, BmapChangesHead *head,
			 uint32 *n)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	bool		own = !BufferIsValid(headbuf);
	BmapChange *all = NULL;

	*n = 0;
	memset(head, 0, sizeof(*head));
	if (own)
	{
		if (!chg_exists(reln) || mdnblocks(reln, BMAP_CHANGES_FORKNUM) == 0)
			return NULL;
		headbuf = chg_read(rel, 0);
		LockBuffer(headbuf, BUFFER_LOCK_SHARE);
	}
	/* (a header whose record a crash lost: no list) */
	if (PageIsNew(BufferGetPage(headbuf)))
	{
		if (own)
			UnlockReleaseBuffer(headbuf);
		return NULL;
	}
	check_page(BufferGetPage(headbuf), BMAP_PAGE_CHANGES_HEAD, rel,
			   BMAP_CHANGES_FORKNUM, 0);
	*head = *PageGetBmapChangesHead(BufferGetPage(headbuf));
	all = palloc_array(BmapChange, Max(head->nentries, 1));
	for (uint32 i = 0; i < head->nentries; i += BMAP_CHANGES_PER_PAGE)
	{
		Buffer		buf = chg_read(rel, CHANGE_PAGE(i));
		uint32		k = Min(BMAP_CHANGES_PER_PAGE, head->nentries - i);

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		check_page(BufferGetPage(buf), BMAP_PAGE_CHANGES, rel,
				   BMAP_CHANGES_FORKNUM, CHANGE_PAGE(i));
		memcpy(all + i, PageGetBmapChanges(BufferGetPage(buf)),
			   k * sizeof(BmapChange));
		UnlockReleaseBuffer(buf);
	}
	*n = head->nentries;
	if (own)
		UnlockReleaseBuffer(headbuf);
	return all;
}

/* The distinct blocks the listed copies hold, sorted; their count. */
static uint32
changes_blocks(const BmapChange *changes, uint32 n, BlockNumber *blocks)
{
	uint32		k = 0;
	uint32		u = 0;

	for (uint32 i = 0; i < n; i++)
		if (changes[i].kind == BMAP_CHANGE_COPY &&
			BlockNumberIsValid(changes[i].old_block))
			blocks[k++] = changes[i].old_block;
	if (k == 0)
		return 0;
	qsort(blocks, k, sizeof(BlockNumber), cmp_block);
	for (uint32 i = 0; i < k; i++)
		if (u == 0 || blocks[u - 1] != blocks[i])
			blocks[u++] = blocks[i];
	return u;
}

/*
 * Begin a fold of the relation: from here on, the first change to each page
 * copies it and lists the block it leaves, which keeps the page as it is
 * now; the map's epoch is raised past every entry's birth for it.  Makes
 * the changes list if the relation has none.  *fold gets the list's lineage,
 * the fold's epoch and the relation's length.
 *
 * Pages cached and already prepared are marked to be prepared again.  A
 * change of one under way (its buffer already locked) completes in place;
 * whoever reads the page after this, with a lock, sees it.
 */
void
bmap_fold_begin(SMgrRelation reln, BmapFold *fold)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		headerbuf;
	Buffer		chgbuf;
	BmapHeader *header;
	xl_bmap_header_delta hdelta;
	xl_bmap_changes_head_delta cdelta;
	bool		fresh;
	XLogRecPtr	lsn;

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("cannot fold a relation during recovery")));

	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	header = PageGetBmapHeader(BufferGetPage(headerbuf));

	if (!chg_exists(reln))
		mdcreate(reln, BMAP_CHANGES_FORKNUM, false);
	if (mdnblocks(reln, BMAP_CHANGES_FORKNUM) == 0)
	{
		chgbuf = chg_new_page(reln);
		if (BufferGetBlockNumber(chgbuf) != 0)
			elog(ERROR, "changes list of \"%s\" begins at block %u",
				 relpathperm(rel, MAIN_FORKNUM).str, BufferGetBlockNumber(chgbuf));
	}
	else
	{
		chgbuf = chg_read(rel, 0);
		LockBuffer(chgbuf, BUFFER_LOCK_EXCLUSIVE);
	}
	/* (a header whose record a crash lost is made anew) */
	fresh = PageIsNew(BufferGetPage(chgbuf));
	if (fresh)
	{
		cdelta.flags = BMAP_DELTA_INIT;
		cdelta.nentries = 0;
		cdelta.pruned_epoch = 0;
		if (!pg_strong_random(&cdelta.lineage, sizeof(cdelta.lineage)))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("could not generate random values")));
	}
	else
	{
		BmapChangesHead *head;

		check_page(BufferGetPage(chgbuf), BMAP_PAGE_CHANGES_HEAD, rel,
				   BMAP_CHANGES_FORKNUM, 0);
		head = PageGetBmapChangesHead(BufferGetPage(chgbuf));
		cdelta.flags = 0;
		cdelta.nentries = head->nentries;
		cdelta.lineage = head->lineage;
		cdelta.pruned_epoch = head->pruned_epoch;
	}
	cdelta.kind = BMAP_DELTA_CHANGES_HEAD;
	cdelta.fold_epoch = header->epoch + 1;
	cdelta.fold_nblocks = header->nblocks;

	hdelta.kind = BMAP_DELTA_HEADER;
	hdelta.flags = 0;
	hdelta.nblocks = header->nblocks;
	hdelta.epoch = header->epoch + 1;
	hdelta.fold_epoch = header->epoch + 1;

	START_CRIT_SECTION();
	apply_header_delta(BufferGetPage(headerbuf), &hdelta);
	MarkBufferDirty(headerbuf);
	apply_changes_head_delta(BufferGetPage(chgbuf), &cdelta);
	MarkBufferDirty(chgbuf);
	XLogBeginInsert();
	XLogRegisterData(&rel, sizeof(xl_bmap_rel));
	XLogRegisterBuffer(0, headerbuf, REGBUF_STANDARD);
	XLogRegisterBufData(0, &hdelta, sizeof(hdelta));
	XLogRegisterBuffer(1, chgbuf, REGBUF_STANDARD | (fresh ? REGBUF_WILL_INIT : 0));
	XLogRegisterBufData(1, &cdelta, sizeof(cdelta));
	lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_FOLD);
	PageSetLSN(BufferGetPage(headerbuf), lsn);
	PageSetLSN(BufferGetPage(chgbuf), lsn);
	END_CRIT_SECTION();

	fold->lineage = cdelta.lineage;
	fold->epoch = cdelta.fold_epoch;
	fold->nblocks = cdelta.fold_nblocks;
	UnlockReleaseBuffer(chgbuf);
	UnlockReleaseBuffer(headerbuf);

	MarkBufferedRelationShared(reln, MAIN_FORKNUM);
}

/*
 * Write 'k' entries into 'buf', a page of a changes list, locked
 * exclusively, as a page of them alone: one record 'info' with main data
 * 'xlrec'.  The buffer is unlocked and released.
 */
static void
write_changes_page(Buffer buf, const BmapChange *entries, uint32 k,
				   uint8 info, const void *xlrec, uint32 xlrec_size)
{
	xl_bmap_changes_delta *delta;
	XLogRecPtr	lsn;

	delta = palloc(sizeof(xl_bmap_changes_delta) + k * sizeof(BmapChange));
	delta->kind = BMAP_DELTA_CHANGES;
	delta->flags = BMAP_DELTA_INIT;
	delta->first = 0;
	delta->n = k;
	memcpy(delta + 1, entries, k * sizeof(BmapChange));

	START_CRIT_SECTION();
	apply_changes_delta(BufferGetPage(buf), delta);
	MarkBufferDirty(buf);
	XLogBeginInsert();
	XLogRegisterData(xlrec, xlrec_size);
	XLogRegisterBuffer(0, buf, REGBUF_STANDARD | REGBUF_WILL_INIT);
	XLogRegisterBufData(0, delta, changes_delta_size(delta));
	lsn = XLogInsert(RM_BMAP_ID, info);
	PageSetLSN(BufferGetPage(buf), lsn);
	END_CRIT_SECTION();
	UnlockReleaseBuffer(buf);
	pfree(delta);
}

/*
 * Visit what changed in the relation's pages since the fold 'since' began:
 * each page copied (its logical block, and the block holding it as it was
 * before), and each truncation (the length it cut to), in the order made.
 * A page may be listed more than once.  False if the relation's list isn't
 * of since's lineage (the relation was rewritten, or never folded), or if
 * changes made since it were let go of (bmap_fold_prune()): then nothing
 * is known.
 */
bool
bmap_fold_changes(SMgrRelation reln, const BmapFold *since,
				  BmapChangeVisit visit, void *arg)
{
	BmapChangesHead head;
	uint32		n;
	BmapChange *all = changes_read(reln, InvalidBuffer, &head, &n);

	if (all == NULL || head.lineage != since->lineage ||
		head.fold_epoch < since->epoch || head.pruned_epoch > since->epoch)
	{
		if (all)
			pfree(all);
		return false;
	}
	for (uint32 i = 0; i < n; i++)
		if (all[i].kind != 0 && all[i].epoch >= since->epoch)
			visit(&all[i], arg);
	pfree(all);
	return true;
}

/*
 * Let go of the listed changes made before the fold 'keep' began: the
 * blocks they hold are freed (or lose a reference), and the list keeps the
 * rest.  The caller makes sure nobody reads those blocks any more.
 *
 * The list is rewritten first, its entries moving down, page by page, then
 * its length; a crash in between leaves every entry kept on the list, some
 * perhaps twice, and blocks it let go of allocated (lost to reclamation).
 */
void
bmap_fold_prune(SMgrRelation reln, const BmapFold *keep)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	BmapChangesHead head;
	uint32		n;
	BmapChange *all;
	BmapChange *kept;
	BmapChange *dropped;
	uint32		nkept = 0;
	uint32		ndropped = 0;
	Buffer		headerbuf;
	Buffer		headbuf;
	BlockNumber *blocks;
	BlockNumber *keptblocks;
	BlockNumber *tofree;
	uint32		nblocks;
	uint32		nk;
	uint32		u = 0;

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("cannot prune a relation's changes during recovery")));
	if (!chg_exists(reln) || mdnblocks(reln, BMAP_CHANGES_FORKNUM) == 0)
		return;

	/*
	 * The map's header, held until the blocks are let go of: no copy of the
	 * relation is made meanwhile (one counts each listed block once more,
	 * as bmapshare() holds the header), and no fold begins or truncation is
	 * listed.  Then the list's: no entry is added.
	 */
	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	headbuf = chg_read(rel, 0);
	LockBuffer(headbuf, BUFFER_LOCK_EXCLUSIVE);
	all = changes_read(reln, headbuf, &head, &n);
	if (all == NULL || head.lineage != keep->lineage)
	{
		UnlockReleaseBuffer(headbuf);
		UnlockReleaseBuffer(headerbuf);
		if (all)
			pfree(all);
		return;
	}
	kept = palloc_array(BmapChange, Max(n, 1));
	dropped = palloc_array(BmapChange, Max(n, 1));
	for (uint32 i = 0; i < n; i++)
	{
		if (all[i].kind == 0)
			continue;
		if (all[i].epoch >= keep->epoch)
			kept[nkept++] = all[i];
		else
			dropped[ndropped++] = all[i];
	}
	if (ndropped == 0 && nkept == n)
	{
		UnlockReleaseBuffer(headbuf);
		UnlockReleaseBuffer(headerbuf);
		pfree(all);
		pfree(kept);
		pfree(dropped);
		return;
	}

	for (uint32 i = 0; i < nkept; i += BMAP_CHANGES_PER_PAGE)
	{
		xl_bmap_rel xlrec;
		Buffer		buf = chg_read(rel, CHANGE_PAGE(i));

		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		xlrec.locator = rel;
		write_changes_page(buf, kept + i, Min(BMAP_CHANGES_PER_PAGE, nkept - i),
						   XLOG_BMAP_CHANGES, &xlrec, sizeof(xlrec));
	}

	{
		xl_bmap_changes_head_delta hdelta;
		xl_bmap_rel xlrec;
		XLogRecPtr	lsn;

		hdelta.kind = BMAP_DELTA_CHANGES_HEAD;
		hdelta.flags = 0;
		hdelta.fold_epoch = head.fold_epoch;
		hdelta.lineage = head.lineage;
		hdelta.fold_nblocks = head.fold_nblocks;
		hdelta.nentries = nkept;
		/* (what changed since an earlier fold isn't all here now) */
		hdelta.pruned_epoch = Max(head.pruned_epoch, keep->epoch);
		xlrec.locator = rel;

		START_CRIT_SECTION();
		apply_changes_head_delta(BufferGetPage(headbuf), &hdelta);
		MarkBufferDirty(headbuf);
		XLogBeginInsert();
		XLogRegisterData(&xlrec, sizeof(xlrec));
		XLogRegisterBuffer(0, headbuf, REGBUF_STANDARD);
		XLogRegisterBufData(0, &hdelta, sizeof(hdelta));
		lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_CHANGES);
		PageSetLSN(BufferGetPage(headbuf), lsn);
		END_CRIT_SECTION();
	}
	UnlockReleaseBuffer(headbuf);
	INJECTION_POINT("bmap-pruned", NULL);

	/* the blocks let go of, each once; one a kept entry holds too stays */
	blocks = palloc_array(BlockNumber, Max(ndropped, 1));
	keptblocks = palloc_array(BlockNumber, Max(nkept, 1));
	tofree = palloc_array(BlockNumber, Max(ndropped, 1));
	nblocks = changes_blocks(dropped, ndropped, blocks);
	nk = changes_blocks(kept, nkept, keptblocks);
	for (uint32 i = 0; i < nblocks; i++)
		if (bsearch(&blocks[i], keptblocks, nk, sizeof(BlockNumber),
					cmp_block) == NULL)
			blocks[u++] = blocks[i];
	nblocks = unref_blocks(rel, blocks, u, tofree);
	free_blocks(rel, tofree, nblocks);
	UnlockReleaseBuffer(headerbuf);

	pfree(blocks);
	pfree(keptblocks);
	pfree(tofree);
	pfree(all);
	pfree(kept);
	pfree(dropped);
}

/*
 * Read a block the changes list holds into 'page': logical block 'blkno'
 * as it was before it was copied.
 */
void
bmap_read_old(SMgrRelation reln, BlockNumber blkno, BlockNumber old_block,
			  char *page)
{
	PGIOAlignedBlock copy;
	void	   *pages[1] = {copy.data};

	mdreadv(store_smgr(reln->smgr_rlocator.locator), MAIN_FORKNUM, old_block,
			pages, 1);
	memcpy(page, copy.data, BLCKSZ);
	if (!PageIsVerified((Page) page, blkno, PIV_LOG_WARNING, NULL))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid page in block %u of the page store of \"%s\", kept for block %u",
						old_block, relpathperm(reln->smgr_rlocator.locator, MAIN_FORKNUM).str,
						blkno)));
}

/*
 * Copy src's changes list into dst, a copy of src being made: dst holds
 * each listed block too, so each block's count grows by one.  src's map
 * header is held exclusively, so its list gets no entry meanwhile.
 *
 * dst's list is made empty first; its entries' pages follow, and then its
 * entries are taken in, a run at a time, each run's blocks counted in the
 * same record.  A crash partway leaves dst holding just the blocks it
 * counted, which dropping it gives back.
 */
static void
share_changes(SMgrRelation src, SMgrRelation dst)
{
	RelFileLocator srel = src->smgr_rlocator.locator;
	RelFileLocator drel = dst->smgr_rlocator.locator;
	BmapChangesHead head;
	uint32		n;
	BmapChange *all = changes_read(src, InvalidBuffer, &head, &n);
	BlockNumber *blocks;
	bool	   *counted;
	BlockNumber *run;
	uint32		nblocks;
	uint32		i;
	xl_bmap_share xlrec;
	Buffer		headbuf;
	xl_bmap_changes_head_delta hdelta;
	XLogRecPtr	lsn;

	if (all == NULL)
		return;
	xlrec.src = srel;
	xlrec.dst = drel;
	mdcreate(dst, BMAP_CHANGES_FORKNUM, false);

	/* the header, of no entries yet */
	headbuf = chg_new_page(dst);
	if (BufferGetBlockNumber(headbuf) != 0)
		elog(ERROR, "changes list of \"%s\" begins at block %u",
			 relpathperm(drel, MAIN_FORKNUM).str, BufferGetBlockNumber(headbuf));
	hdelta.kind = BMAP_DELTA_CHANGES_HEAD;
	hdelta.flags = BMAP_DELTA_INIT;
	hdelta.fold_epoch = head.fold_epoch;
	hdelta.lineage = head.lineage;
	hdelta.fold_nblocks = head.fold_nblocks;
	hdelta.nentries = 0;
	hdelta.pruned_epoch = head.pruned_epoch;
	START_CRIT_SECTION();
	apply_changes_head_delta(BufferGetPage(headbuf), &hdelta);
	MarkBufferDirty(headbuf);
	XLogBeginInsert();
	XLogRegisterData(&xlrec, sizeof(xlrec));
	XLogRegisterBuffer(0, headbuf, REGBUF_STANDARD | REGBUF_WILL_INIT);
	XLogRegisterBufData(0, &hdelta, sizeof(hdelta));
	lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_SHARE_CHANGES);
	PageSetLSN(BufferGetPage(headbuf), lsn);
	END_CRIT_SECTION();

	/* the entries' pages, which the header doesn't reach yet */
	for (i = 0; i < n; i += BMAP_CHANGES_PER_PAGE)
		write_changes_page(changes_next_page(dst, CHANGE_PAGE(i)), all + i,
						   Min(BMAP_CHANGES_PER_PAGE, n - i),
						   XLOG_BMAP_SHARE_CHANGES, &xlrec, sizeof(xlrec));

	/* the blocks the list holds, each counted once */
	blocks = palloc_array(BlockNumber, Max(n, 1));
	counted = palloc0_array(bool, Max(n, 1));
	run = palloc_array(BlockNumber, Max(n, 1));
	nblocks = changes_blocks(all, n, blocks);
	for (i = 0; i < nblocks; i++)
		if (REF_PAGE(blocks[i]) >= refs_nblocks(srel))
			elog(ERROR, "block %u of the page store of database %u has no reference count",
				 blocks[i], srel.dbOid);

	/*
	 * Runs of entries whose blocks not counted yet lie on few enough count
	 * pages for one record; the header takes each run in as its blocks are
	 * counted.
	 */
	i = 0;
	while (i < n)
	{
		BlockNumber pages[BMAP_REFS_PER_RECORD];
		int			npages = 0;
		uint32		nrun = 0;
		uint32		end = i;
		RefsPages	rp;

		for (; end < n; end++)
		{
			BlockNumber *at;
			bool		known = false;

			if (all[end].kind != BMAP_CHANGE_COPY ||
				!BlockNumberIsValid(all[end].old_block))
				continue;
			at = bsearch(&all[end].old_block, blocks, nblocks,
						 sizeof(BlockNumber), cmp_block);
			Assert(at != NULL);
			if (counted[at - blocks])
				continue;
			for (int p = 0; p < npages; p++)
				known |= pages[p] == REF_PAGE(*at);
			if (!known)
			{
				if (npages == BMAP_REFS_PER_RECORD)
					break;
				pages[npages++] = REF_PAGE(*at);
			}
			counted[at - blocks] = true;
			run[nrun++] = *at;
		}

		rp.n = 0;
		if (nrun > 0)
		{
			qsort(run, nrun, sizeof(BlockNumber), cmp_block);
			if (refs_lock(srel, run, nrun, &rp) != nrun)
				elog(ERROR, "blocks of a changes list of \"%s\" span too many count pages",
					 relpathperm(drel, MAIN_FORKNUM).str);
			for (uint32 k = 0; k < nrun; k++)
			{
				uint8		c = refs_get(&rp, run[k]);

				if (c != BMAP_REFS_STUCK)
					refs_set(&rp, run[k], c + 1);
			}
		}

		hdelta.flags = 0;
		hdelta.nentries = end;
		XLogEnsureRecordSpace(1 + BMAP_REFS_PER_RECORD, 2 + BMAP_REFS_PER_RECORD);
		START_CRIT_SECTION();
		apply_changes_head_delta(BufferGetPage(headbuf), &hdelta);
		MarkBufferDirty(headbuf);
		XLogBeginInsert();
		XLogRegisterData(&xlrec, sizeof(xlrec));
		XLogRegisterBuffer(0, headbuf, REGBUF_STANDARD);
		XLogRegisterBufData(0, &hdelta, sizeof(hdelta));
		refs_apply_register(&rp, 1);
		lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_SHARE_CHANGES);
		PageSetLSN(BufferGetPage(headbuf), lsn);
		refs_set_lsn(&rp, lsn);
		END_CRIT_SECTION();
		refs_release(&rp);
		i = end;
		INJECTION_POINT("bmap-share-changes-counted", NULL);
	}

	UnlockReleaseBuffer(headbuf);
	pfree(blocks);
	pfree(counted);
	pfree(run);
	pfree(all);
}

/* ----------------------------------------------------------------
 * Sharing
 * ----------------------------------------------------------------
 */

/*
 * Copy map page 'mapblk' of 'src' into a new map page of 'dst', for the
 * blocks below 'nblocks', counting one more reference to each block.
 * src's header is locked exclusively, so its entries can't change.
 */
static void
share_map_page(SMgrRelation src, SMgrRelation dst, BlockNumber mapblk,
			   BlockNumber nblocks)
{
	RelFileLocator srel = src->smgr_rlocator.locator;
	RelFileLocator drel = dst->smgr_rlocator.locator;
	PGIOAlignedBlock copy;
	BmapEntry  *entries = PageGetBmapEntries(copy.data);
	SlotEntry  *items = palloc(BMAP_FANOUT * sizeof(SlotEntry));
	BlockNumber *phys = palloc(BMAP_FANOUT * sizeof(BlockNumber));
	uint32		nitems = 0;
	Buffer		mapbuf;
	BlockNumber nrefpages = refs_nblocks(srel);
	uint32		i = 0;
	bool		first = true;

	read_node(src, mapblk, false, copy.data, BMAP_PAGE_NODE);
	for (uint32 slot = 0; slot < BMAP_FANOUT; slot++)
	{
		BlockNumber blk = (mapblk - 1) * BMAP_FANOUT + slot;

		if (blk >= nblocks || !BlockNumberIsValid(entries[slot].block))
			continue;
		items[nitems].slot = slot;
		items[nitems].block = entries[slot].block;
		items[nitems].epoch = entries[slot].epoch;
		nitems++;
	}
	qsort(items, nitems, sizeof(SlotEntry), cmp_slot_entry);
	for (uint32 k = 0; k < nitems; k++)
	{
		phys[k] = items[k].block;
		/* refs_cover() reached the high-water mark first */
		if (REF_PAGE(phys[k]) >= nrefpages)
			elog(ERROR, "block %u of the page store of database %u has no reference count",
				 phys[k], srel.dbOid);
	}

	/* the copy's maps are added in order, so this is its next page */
	mapbuf = map_new_page(dst);
	map_locked_as_new(mapbuf);
	if (BufferGetBlockNumber(mapbuf) != mapblk)
		elog(ERROR, "map of \"%s\" extended to block %u, expected %u",
			 relpath(dst->smgr_rlocator, MAIN_FORKNUM).str,
			 BufferGetBlockNumber(mapbuf), mapblk);

	/* a page with no entries is still made, in one record */
	while (first || i < nitems)
	{
		RefsPages	rp;
		xl_bmap_node_delta *ndelta;
		xl_bmap_entry_delta *ent;
		xl_bmap_share xlrec;
		uint32		covered = 0;

		ndelta = palloc0(sizeof(xl_bmap_node_delta) + nitems * sizeof(xl_bmap_entry_delta));
		ndelta->kind = BMAP_DELTA_NODE;
		if (first)
			ndelta->flags |= BMAP_DELTA_INIT;
		ent = (xl_bmap_entry_delta *) (ndelta + 1);

		rp.n = 0;
		if (i < nitems)
			covered = refs_lock(srel, phys + i, nitems - i, &rp);
		for (uint32 k = i; k < i + covered; k++)
		{
			uint8		c = refs_get(&rp, phys[k]);

			if (c != BMAP_REFS_STUCK)
				refs_set(&rp, phys[k], c + 1);
			ent[ndelta->nentries].slot = items[k].slot;
			ent[ndelta->nentries].block = items[k].block;
			ent[ndelta->nentries].epoch = items[k].epoch;
			ndelta->nentries++;
		}

		xlrec.src = srel;
		xlrec.dst = drel;
		log_refs_record(XLOG_BMAP_SHARE, &xlrec, sizeof(xlrec), mapbuf, ndelta,
						first ? REGBUF_WILL_INIT : 0, &rp);

		refs_release(&rp);
		pfree(ndelta);
		i += covered;
		first = false;
	}
	map_unlock(mapbuf);
	ReleaseBuffer(mapbuf);
	pfree(items);
	pfree(phys);
}

/*
 * Make 'dst', a new relation file number in the same tablespace and
 * database, a copy of 'src' that shares its blocks: dst's map holds src's
 * entries, and its changes list src's, each block's count grows by one,
 * and both maps' epochs pass the entries' births.  src's dirty buffers are written and the page store
 * synced, so the blocks hold what both copies start from.
 *
 * The caller keeps src from changing meanwhile (a lock that waits for its
 * writers): a change made to one of src's buffers before the epochs are
 * raised would be written to a block dst shares.  An error partway leaves
 * dst with some of src's blocks counted; dropping dst gives them back.
 */
void
bmapshare(SMgrRelation src, SMgrRelation dst)
{
	RelFileLocator srel = src->smgr_rlocator.locator;
	RelFileLocator drel = dst->smgr_rlocator.locator;
	Buffer		metabuf;
	Buffer		sheader;
	Buffer		dheader;
	BlockNumber nblocks;
	uint32		epoch;
	xl_bmap_header_delta sdelta;
	xl_bmap_header_delta ddelta;
	xl_bmap_share xlrec;
	XLogRecPtr	lsn;
	LOCKTAG		sweeptag;

	if (srel.spcOid != drel.spcOid || srel.dbOid != drel.dbOid)
		elog(ERROR, "block-mapped relation %s can't share the blocks of %s, in another page store",
			 relpath(dst->smgr_rlocator, MAIN_FORKNUM).str,
			 relpath(src->smgr_rlocator, MAIN_FORKNUM).str);

	/* not while a sweep counts what the maps hold */
	SET_LOCKTAG_OBJECT(sweeptag, srel.dbOid, BMAP_SWEEP_CLASSID, srel.spcOid, 0);
	(void) LockAcquire(&sweeptag, ShareLock, false, false);

	/* every allocated block has a reference count */
	metabuf = alloc_read(srel, 0);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(metabuf), BMAP_PAGE_META, srel, MAIN_FORKNUM, 0);
	refs_cover(srel, PageGetBmapMeta(BufferGetPage(metabuf))->hw);
	UnlockReleaseBuffer(metabuf);

	bmapcreate(dst, MAIN_FORKNUM, false);

	/* src's header, held until the epochs are raised: no change meanwhile */
	sheader = map_read(srel, 0);
	LockBuffer(sheader, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(sheader), BMAP_PAGE_HEADER, srel, BMAP_FORKNUM, 0);
	nblocks = PageGetBmapHeader(BufferGetPage(sheader))->nblocks;
	epoch = PageGetBmapHeader(BufferGetPage(sheader))->epoch;

	if (nblocks > 0)
		for (BlockNumber mapblk = 1; mapblk <= MAP_PAGE(nblocks - 1); mapblk++)
		{
			share_map_page(src, dst, mapblk, nblocks);
			INJECTION_POINT("bmap-share-map-page", NULL);
		}

	/* the copy has the same changes since src's last fold */
	share_changes(src, dst);

	dheader = map_read(drel, 0);
	LockBuffer(dheader, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(dheader), BMAP_PAGE_HEADER, drel, BMAP_FORKNUM, 0);

	sdelta.kind = ddelta.kind = BMAP_DELTA_HEADER;
	sdelta.flags = ddelta.flags = 0;
	sdelta.nblocks = ddelta.nblocks = nblocks;
	sdelta.epoch = ddelta.epoch = epoch + 1;
	/* (the copy has src's list, as of src's fold) */
	sdelta.fold_epoch = ddelta.fold_epoch =
		PageGetBmapHeader(BufferGetPage(sheader))->fold_epoch;
	xlrec.src = srel;
	xlrec.dst = drel;

	START_CRIT_SECTION();
	apply_header_delta(BufferGetPage(sheader), &sdelta);
	MarkBufferDirty(sheader);
	apply_header_delta(BufferGetPage(dheader), &ddelta);
	MarkBufferDirty(dheader);
	XLogBeginInsert();
	XLogRegisterData(&xlrec, sizeof(xlrec));
	XLogRegisterBuffer(0, sheader, REGBUF_STANDARD);
	XLogRegisterBufData(0, &sdelta, sizeof(sdelta));
	XLogRegisterBuffer(1, dheader, REGBUF_STANDARD);
	XLogRegisterBufData(1, &ddelta, sizeof(ddelta));
	lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_SHARED);
	PageSetLSN(BufferGetPage(sheader), lsn);
	PageSetLSN(BufferGetPage(dheader), lsn);
	END_CRIT_SECTION();

	UnlockReleaseBuffer(dheader);
	UnlockReleaseBuffer(sheader);
	LockRelease(&sweeptag, ShareLock, false);
	dst->smgr_cached_nblocks[MAIN_FORKNUM] = InvalidBlockNumber;

	/*
	 * src's cached pages are checked again before they next change, as pages
	 * read from now on are.  Done once src's map is unlocked, since it visits
	 * every buffer.  A page found src's own before the epoch was raised may
	 * change in place until it is marked; with src kept from changing, only
	 * a hint or pruning can change it, and either holds for dst too, whose
	 * page is the same (and gets it when flushed, below).
	 */
	MarkBufferedRelationShared(src, MAIN_FORKNUM);

	/*
	 * The shared blocks get src's latest pages, durably: recovery can't make
	 * them again, since replaying src's changes writes its pages through
	 * whatever map pages are on disk, which may already send them to the
	 * blocks src moved to later.  A page src changes from here on is copied
	 * first, its old contents written to the shared block if they were only
	 * in its buffer.
	 */
	FlushRelationsAllBuffers(&src, 1);
	mdimmedsync(store_smgr(srel), MAIN_FORKNUM);
}

/* ----------------------------------------------------------------
 * Copying shared pages before they change
 * ----------------------------------------------------------------
 */

/*
 * Is a page's hole (between pd_lower and pd_upper, as a standard page has)
 * all zeroes?  Then an image may leave it out and lose nothing.
 */
static bool
page_hole_is_zero(Page page)
{
	PageHeader	ph = (PageHeader) page;

	return ph->pd_lower >= SizeOfPageHeaderData &&
		ph->pd_upper > ph->pd_lower && ph->pd_upper <= BLCKSZ &&
		pg_memory_is_all_zeros((char *) page + ph->pd_lower,
							   ph->pd_upper - ph->pd_lower);
}

bool
bmappreparesforchanges(SMgrRelation reln, ForkNumber forknum)
{
	return forknum == MAIN_FORKNUM;
}

/*
 * Is logical block 'blkno''s block held by another map too, so that
 * preparing it for a change copies it?  *copies says whether preparing it
 * copies it, for that reason or because it is as of the relation's last
 * fold.
 */
bool
bmappageisshared(SMgrRelation reln, ForkNumber forknum, BlockNumber blkno,
				 bool *copies)
{
	uint32		epoch;
	BmapEntry	entry;
	bool		shared;

	*copies = false;
	if (forknum != MAIN_FORKNUM)
		return false;
	epoch = lookup_epoch(reln);
	if (!lookup_entry(reln, blkno, &entry) || entry.epoch >= epoch)
		return false;
	shared = lookup_ref(reln->smgr_rlocator.locator, entry.block) > 0;
	*copies = shared || entry.epoch < changes_fold_epoch(reln);
	return shared;
}

/*
 * Prepare logical block 'blkno', locked exclusively in 'buffer', for a
 * change.  An entry born in the map's epoch is the map's own.  An older one
 * may be shared: if no other map holds its block any more, it is claimed
 * (born again in this epoch); otherwise the page is copied to a new block,
 * which the map takes, and the old block loses a reference.  An entry born
 * before the relation's last fold is copied whether shared or not, and its
 * old block, the page as of the fold, goes on the changes list, which holds
 * the map's reference to it.  The copy's
 * record carries the page's image (without its hole, when that is all
 * zeroes), so redo makes the new block without reading it; a page of
 * zeroes, which its caller is about to initialize, needs no image.  Before
 * the copy, contents only in the buffer are written to the old block,
 * which other maps still read.
 *
 * Locks: the header shared (so the relation's extensions and sharing wait),
 * the old block's reference-count page, the allocator's pages, the changes
 * list's pages, then the map page last: reading any of those may write out
 * one of this relation's pages, which looks it up on that page.  No other
 * change of this entry can come meanwhile, since the buffer is locked.
 */
void
bmapprepareforchange(SMgrRelation reln, ForkNumber forknum, BlockNumber blkno,
					 Buffer buffer, bool neutral)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	uint32		epoch;
	BmapEntry	entry;
	Buffer		headerbuf;
	Buffer		refbuf = InvalidBuffer;
	Buffer		mapbuf;
	uint8		count = 0;
	BlockNumber old_block;
	xl_bmap_node_delta *ndelta;
	xl_bmap_entry_delta *ent;
	XLogRecPtr	lsn;
	bool		as_of_fold;
	bool		listed;

	if (forknum != MAIN_FORKNUM)
		return;

	/* the common case, with share locks alone */
	epoch = lookup_epoch(reln);
	if (!lookup_entry(reln, blkno, &entry) || entry.epoch >= epoch)
		return;

	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	epoch = PageGetBmapHeader(BufferGetPage(headerbuf))->epoch;
	if (!lookup_entry(reln, blkno, &entry) || entry.epoch >= epoch)
	{
		UnlockReleaseBuffer(headerbuf);
		return;
	}
	old_block = entry.block;
	as_of_fold = entry.epoch <
		PageGetBmapHeader(BufferGetPage(headerbuf))->fold_epoch;
	listed = as_of_fold && !neutral;

	if (REF_PAGE(old_block) < refs_nblocks(rel))
	{
		refbuf = refs_read(rel, REF_PAGE(old_block));
		LockBuffer(refbuf, BUFFER_LOCK_EXCLUSIVE);
		check_refs_page(BufferGetPage(refbuf), rel, REF_PAGE(old_block));
		count = page_ref(BufferGetPage(refbuf), old_block);
	}

	ndelta = palloc0(sizeof(xl_bmap_node_delta) + sizeof(xl_bmap_entry_delta));
	ndelta->kind = BMAP_DELTA_NODE;
	ndelta->nentries = 1;
	ent = (xl_bmap_entry_delta *) (ndelta + 1);
	ent->slot = MAP_SLOT(blkno);
	ent->epoch = epoch;

	/*
	 * A change of no row leaves a page as of the fold as it is: in place if
	 * nothing shares it, else copied (as the map's own) but still as of the
	 * fold, so its next change copies and lists it.
	 */
	if (as_of_fold && neutral)
	{
		if (count == 0)
		{
			UnlockReleaseBuffer(headerbuf);
			if (BufferIsValid(refbuf))
				UnlockReleaseBuffer(refbuf);
			pfree(ndelta);
			return;
		}
		ent->epoch = entry.epoch;
	}

	if (count == 0 && !listed)
	{
		xl_bmap_unref xlrec;
		RefsPages	norefs;

		/* no other map holds it: claim it */
		ent->block = old_block;
		mapbuf = map_read(rel, MAP_PAGE(blkno));
		map_lock(mapbuf, false);
		check_page(BufferGetPage(mapbuf), BMAP_PAGE_NODE, rel, BMAP_FORKNUM,
				   MAP_PAGE(blkno));

		/* its count is zero already: only the map page changes */
		norefs.n = 0;
		xlrec.locator = rel;
		xlrec.flags = BMAP_UNREF_MAP;
		log_refs_record(XLOG_BMAP_UNREF, &xlrec, sizeof(xlrec),
						mapbuf, ndelta, 0, &norefs);
	}
	else
	{
		Allocation	alloc;
		BlockNumber new_block;
		xl_bmap_refs_delta *rdelta = NULL;
		xl_bmap_remap xlrec;
		int			id;
		Page		page = BufferGetPage(buffer);
		bool		zeroed = pg_memory_is_all_zeros(page, BLCKSZ);
		bool		reused;
		ChangesAppend ca = {InvalidBuffer, InvalidBuffer};

		/* the old block gets what other maps should read, if only buffered */
		if (BufferIsDirty(buffer))
			FlushOneBuffer(buffer);

		alloc_take(rel, 1, &new_block, &alloc);
		ent->block = new_block;
		reused = new_block < alloc.old_hw;

		/*
		 * The page store reaches the block.  The page's contents come with
		 * the record, as an image; a page of zeroes (one being initialized)
		 * is zeroed in the new block instead, here and in redo.
		 */
		prepare_store(rel, alloc.old_hw, alloc.new_hw,
					  &new_block, zeroed && reused ? 1 : 0, false);

		/* a listed block keeps the reference, which the list holds */
		if (count == BMAP_REFS_STUCK && !listed)
			stuck_released(rel);
		if (count != BMAP_REFS_STUCK && !listed)
		{
			xl_bmap_ref_delta *ref;

			rdelta = palloc0(sizeof(xl_bmap_refs_delta) + sizeof(xl_bmap_ref_delta));
			rdelta->kind = BMAP_DELTA_REFS;
			rdelta->nrefs = 1;
			ref = (xl_bmap_ref_delta *) (rdelta + 1);
			ref->slot = REF_SLOT(old_block);
			ref->count = count - 1;
		}

		/* the list's pages are read before the map page is locked */
		if (listed)
		{
			BmapChange	c;

			c.epoch = epoch;
			c.blkno = blkno;
			c.old_block = old_block;
			c.kind = BMAP_CHANGE_COPY;
			changes_append_lock(reln, &ca, &c);
		}

		mapbuf = map_read(rel, MAP_PAGE(blkno));
		map_lock(mapbuf, false);
		check_page(BufferGetPage(mapbuf), BMAP_PAGE_NODE, rel, BMAP_FORKNUM,
				   MAP_PAGE(blkno));

		xlrec.locator = rel;
		xlrec.blkno = blkno;
		xlrec.old_block = old_block;
		xlrec.new_block = new_block;
		xlrec.old_hw = alloc.old_hw;
		xlrec.new_hw = alloc.new_hw;
		xlrec.flags = (zeroed ? BMAP_REMAP_ZERO : 0) |
			(listed ? BMAP_REMAP_LISTED : 0) |
			(listed && count == 0 ? BMAP_REMAP_OWN : 0);

		XLogEnsureRecordSpace(5 + BMAP_ALLOC_MAX_PAGES, 0);
		ndelta->since = GetXLogInsertRecPtr();
		START_CRIT_SECTION();
		apply_node_delta(BufferGetPage(mapbuf), ndelta);
		MarkBufferDirty(mapbuf);
		/* the page is written to its new block from here on */
		MarkBufferDirty(buffer);
		XLogBeginInsert();
		XLogRegisterData(&xlrec, sizeof(xlrec));
		XLogRegisterBuffer(0, mapbuf, REGBUF_STANDARD);
		XLogRegisterBufData(0, ndelta, node_delta_size(ndelta));
		id = alloc_apply_register(&alloc, 1);
		if (rdelta)
		{
			apply_refs_delta(BufferGetPage(refbuf), rdelta);
			MarkBufferDirty(refbuf);
			XLogRegisterBuffer(id, refbuf, REGBUF_STANDARD);
			XLogRegisterBufData(id, rdelta, refs_delta_size(rdelta));
			id++;
		}
		if (listed)
			id = changes_append_register(&ca, id);
		if (zeroed)
			XLogRegisterBuffer(id, buffer, REGBUF_WILL_INIT);
		else
			XLogRegisterBuffer(id, buffer, REGBUF_FORCE_IMAGE |
							   (page_hole_is_zero(page) ? REGBUF_STANDARD : 0));
		lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_REMAP);
		PageSetLSN(BufferGetPage(mapbuf), lsn);
		alloc_set_lsn(&alloc, lsn);
		if (rdelta)
			PageSetLSN(BufferGetPage(refbuf), lsn);
		/* a page of zeroes stays one: a new page has no LSN */
		if (!zeroed)
			PageSetLSN(page, lsn);
		END_CRIT_SECTION();
		if (listed)
			changes_append_finish(&ca, lsn);

		alloc_release(&alloc);
		if (rdelta)
			pfree(rdelta);
	}

	map_unlock(mapbuf);
	ReleaseBuffer(mapbuf);
	if (BufferIsValid(refbuf))
		UnlockReleaseBuffer(refbuf);
	UnlockReleaseBuffer(headerbuf);
	pfree(ndelta);
}

/* Set a relation's length; the header is locked exclusively. */
static void
set_length(RelFileLocator rel, Buffer headerbuf, BlockNumber nblocks)
{
	set_length_listed(NULL, rel, headerbuf, nblocks, NULL);
}

/*
 * The same, and with 'ca' (its buffers pinned), an entry on reln's changes
 * list saying the relation was cut to 'nblocks', in the same record.
 */
static void
set_length_listed(SMgrRelation reln, RelFileLocator rel, Buffer headerbuf,
				  BlockNumber nblocks, ChangesAppend *ca)
{
	xl_bmap_header_delta delta;
	XLogRecPtr	lsn;

	delta.kind = BMAP_DELTA_HEADER;
	delta.flags = 0;
	delta.nblocks = nblocks;
	delta.epoch = PageGetBmapHeader(BufferGetPage(headerbuf))->epoch;
	delta.fold_epoch = PageGetBmapHeader(BufferGetPage(headerbuf))->fold_epoch;
	if (ca)
	{
		BmapChange	c;

		c.epoch = delta.epoch;
		c.blkno = nblocks;
		c.old_block = InvalidBlockNumber;
		c.kind = BMAP_CHANGE_TRUNCATE;
		changes_append_lock(reln, ca, &c);
	}
	START_CRIT_SECTION();
	apply_header_delta(BufferGetPage(headerbuf), &delta);
	MarkBufferDirty(headerbuf);
	XLogBeginInsert();
	XLogRegisterData(&rel, sizeof(xl_bmap_rel));
	XLogRegisterBuffer(0, headerbuf, REGBUF_STANDARD);
	XLogRegisterBufData(0, &delta, sizeof(delta));
	if (ca)
		(void) changes_append_register(ca, 1);
	lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_TRUNCATE);
	PageSetLSN(BufferGetPage(headerbuf), lsn);
	END_CRIT_SECTION();
	if (ca)
	{
		/* the pins are the caller's */
		PageSetLSN(BufferGetPage(ca->headbuf), lsn);
		PageSetLSN(BufferGetPage(ca->pagebuf), lsn);
		LockBuffer(ca->pagebuf, BUFFER_LOCK_UNLOCK);
		LockBuffer(ca->headbuf, BUFFER_LOCK_UNLOCK);
	}
}

/*
 * Free every physical block a dropped relation's map or changes list holds
 * that no other map holds too; the others lose a reference.  The records
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
		BlockNumber *tofree = palloc(BMAP_FANOUT * sizeof(BlockNumber));

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

			/* a block another map still holds only loses a reference */
			qsort(blocks, n, sizeof(BlockNumber), cmp_block);
			n = unref_blocks(rel, blocks, n, tofree);
			free_blocks(rel, tofree, n);
		}
		pfree(blocks);
		pfree(tofree);

		/* and those the changes list holds */
		{
			BmapChangesHead head;
			uint32		nchanges;
			BmapChange *changes = changes_read(reln, InvalidBuffer, &head,
											   &nchanges);

			if (changes != NULL)
			{
				BlockNumber *held = palloc_array(BlockNumber, Max(nchanges, 1));
				BlockNumber *free_held = palloc_array(BlockNumber, Max(nchanges, 1));
				uint32		nheld = changes_blocks(changes, nchanges, held);

				nheld = unref_blocks(rel, held, nheld, free_held);
				free_blocks(rel, free_held, nheld);
				pfree(held);
				pfree(free_held);
				pfree(changes);
			}
		}
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

/* ----------------------------------------------------------------
 * The sweep
 * ----------------------------------------------------------------
 */

/* One more holder of block b (a count past what a count holds stays). */
static inline void
hold(uint16 *holders, BlockNumber b)
{
	if (holders[b] < PG_UINT16_MAX)
		holders[b]++;
}

/*
 * Count, in holders[], each physical block a relation holds: its map's
 * entries (those past its length too) and its changes list's old blocks.
 * Its header is held exclusively meanwhile, so no extension, remap,
 * truncation or prune of it runs: a block they allocated is mapped or
 * listed by the time it is read.
 */
static void
sweep_relation(SMgrRelation reln, uint16 *holders, BlockNumber hw)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	BlockNumber mapnblocks;
	Buffer		headerbuf;
	BmapChangesHead head;
	uint32		nchanges;
	BmapChange *changes;

	/*
	 * A relation dropped since the directory was read is passed over; once
	 * its map is open it stays readable.
	 */
	if (!mdexists(reln, BMAP_FORKNUM))
		return;
	mapnblocks = mdnblocks(reln, BMAP_FORKNUM);
	if (mapnblocks == 0)
		return;
	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_EXCLUSIVE);
	if (PageIsNew(BufferGetPage(headerbuf)))
	{
		UnlockReleaseBuffer(headerbuf);
		return;
	}
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	mapnblocks = mdnblocks(reln, BMAP_FORKNUM);
	for (BlockNumber mapblk = 1; mapblk < mapnblocks; mapblk++)
	{
		Buffer		buf = map_read(rel, mapblk);
		BmapEntry  *entries;

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		if (!PageIsNew(BufferGetPage(buf)))
		{
			check_page(BufferGetPage(buf), BMAP_PAGE_NODE, rel, BMAP_FORKNUM,
					   mapblk);
			entries = PageGetBmapEntries(BufferGetPage(buf));
			for (uint32 i = 0; i < BMAP_FANOUT; i++)
				if (BlockNumberIsValid(entries[i].block) && entries[i].block < hw)
					hold(holders, entries[i].block);
		}
		UnlockReleaseBuffer(buf);
		CHECK_FOR_INTERRUPTS();
	}
	changes = changes_read(reln, InvalidBuffer, &head, &nchanges);
	if (changes != NULL)
	{
		BlockNumber *held = palloc_array(BlockNumber, Max(nchanges, 1));
		uint32		nheld = changes_blocks(changes, nchanges, held);

		for (uint32 i = 0; i < nheld; i++)
			if (held[i] < hw)
				hold(holders, held[i]);
		pfree(held);
		pfree(changes);
	}
	UnlockReleaseBuffer(headerbuf);

	/*
	 * A relation dropped while it was read (its buffers dropped before
	 * these were read): none of its pages stays in shared buffers, where a
	 * relation given its file number later would find them.
	 */
	if (!bmap_relation_exists(rel))
	{
		ForkNumber	forks[2] = {BMAP_FORKNUM, BMAP_CHANGES_FORKNUM};
		BlockNumber firsts[2] = {0, 0};

		DropRelationBuffers(reln, forks, 2, firsts);
	}
}

/*
 * Rewrite the counts of the blocks stuck[0 .. n), sorted: a stuck count
 * whose holders fit a count now, and the count of a block nothing holds
 * (zero, before it is freed).  A stuck count is changed only by a sweep,
 * and nothing adds holders meanwhile, so the holders counted are the
 * block's, or more (a map dropped since).
 */
static uint32
sweep_recount(RelFileLocator rel, const BlockNumber *stuck, uint32 n,
			  const uint16 *holders)
{
	uint32		i = 0;
	uint32		recounted = 0;

	while (i < n)
	{
		RefsPages	rp;
		uint32		covered = refs_lock(rel, stuck + i, n - i, &rp);
		bool		changed = false;

		for (uint32 k = i; k < i + covered; k++)
		{
			uint32		h = holders[stuck[k]];

			if (h > 0 && refs_get(&rp, stuck[k]) != BMAP_REFS_STUCK)
				continue;
			refs_set(&rp, stuck[k], (uint8) (h > 0 ? h - 1 : 0));
			changed = true;
			recounted += h > 0;
		}
		if (changed)
		{
			xl_bmap_unref xlrec;

			xlrec.locator = rel;
			xlrec.flags = 0;
			log_refs_record(XLOG_BMAP_UNREF, &xlrec, sizeof(xlrec),
							InvalidBuffer, NULL, 0, &rp);
		}
		refs_release(&rp);
		i += covered;
	}
	return recounted;
}

/*
 * Shorten the page store to its last allocated block: the high-water mark
 * lowered and the file truncated, the record first.  Under the metapage's
 * lock, so nothing is allocated meanwhile.
 */
static BlockNumber
sweep_shrink(RelFileLocator rel)
{
	Buffer		metabuf = alloc_read(rel, 0);
	BmapMeta   *meta;
	BlockNumber hw;
	BlockNumber new_hw;
	xl_bmap_meta_delta mdelta;
	xl_bmap_shrink xlrec;
	XLogRecPtr	lsn;
	SMgrRelation store = store_smgr(rel);
	BlockNumber nblocks;

	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(metabuf), BMAP_PAGE_META, rel, MAIN_FORKNUM, 0);
	meta = PageGetBmapMeta(BufferGetPage(metabuf));
	hw = new_hw = meta->hw;

	/* each bitmap page read once, from the end, a word of free blocks at once */
	while (new_hw > 0)
	{
		uint32		pageno = (new_hw - 1) / BMAP_BITS_PER_PAGE;
		BlockNumber first = (BlockNumber) pageno * BMAP_BITS_PER_PAGE;
		Buffer		buf;
		uint64	   *bits;
		bool		used = false;

		if (pageno >= meta->nbitmap)
			break;
		buf = alloc_read(rel, meta->bitmap[pageno]);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		check_page(BufferGetPage(buf), BMAP_PAGE_BITMAP, rel, MAIN_FORKNUM,
				   meta->bitmap[pageno]);
		bits = PageGetBmapBits(BufferGetPage(buf));
		while (new_hw > first && !used)
		{
			uint32		bit = new_hw - 1 - first;

			if (bit % 64 == 63 && bits[bit / 64] == 0)
				new_hw -= 64;
			else if ((bits[bit / 64] & (UINT64CONST(1) << (bit % 64))) != 0)
				used = true;
			else
				new_hw--;
		}
		UnlockReleaseBuffer(buf);
		if (used)
			break;
		CHECK_FOR_INTERRUPTS();
	}
	if (new_hw == hw)
	{
		UnlockReleaseBuffer(metabuf);
		return 0;
	}

	mdelta.kind = BMAP_DELTA_META;
	mdelta.flags = 0;
	mdelta.hw = new_hw;
	mdelta.nfree = meta->nfree - (hw - new_hw);
	mdelta.hint = Min(meta->hint, new_hw / BMAP_BITS_PER_PAGE);
	mdelta.nbitmap_added = 0;
	xlrec.locator = rel;
	xlrec.old_hw = hw;
	xlrec.new_hw = new_hw;
	nblocks = smgrnblocks(store, MAIN_FORKNUM);

	/*
	 * The record and the truncation together, as RelationTruncate() does:
	 * no checkpoint begins between them (one whose redo point follows the
	 * record would leave a crash's store longer than its metapage says), nor
	 * completes before the file is shorter; and a failure to truncate is a
	 * PANIC, so redo truncates it.
	 */
	Assert((MyProc->delayChkptFlags & (DELAY_CHKPT_START | DELAY_CHKPT_COMPLETE)) == 0);
	MyProc->delayChkptFlags |= DELAY_CHKPT_START | DELAY_CHKPT_COMPLETE;
	START_CRIT_SECTION();
	apply_meta_delta(BufferGetPage(metabuf), &mdelta);
	MarkBufferDirty(metabuf);
	XLogBeginInsert();
	XLogRegisterData(&xlrec, sizeof(xlrec));
	XLogRegisterBuffer(0, metabuf, REGBUF_STANDARD);
	XLogRegisterBufData(0, &mdelta, sizeof(mdelta));
	lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_SHRINK);
	PageSetLSN(BufferGetPage(metabuf), lsn);
	XLogFlush(lsn);
	if (nblocks > new_hw)
		smgrtruncate(store, &(ForkNumber) {MAIN_FORKNUM}, 1, &nblocks, &new_hw);
	END_CRIT_SECTION();
	MyProc->delayChkptFlags &= ~(DELAY_CHKPT_START | DELAY_CHKPT_COMPLETE);
	UnlockReleaseBuffer(metabuf);
	return hw - new_hw;
}

/*
 * Sweep a database's page store in a tablespace: count exactly what every
 * block-mapped relation there holds (every branch's copies, so every
 * catalog row of every branch: their maps are files), then free the
 * blocks allocated when the sweep began that nothing holds (left by a
 * crash after a drop's commit or part way through a prune), rewrite the
 * stuck counts whose holders now fit one, and shorten the store to its
 * last allocated block.  Branch creation (bmapshare()) is held off while
 * the maps are read and the counts rewritten; extensions and remaps go on,
 * allocating blocks the sweep never frees.
 */
static void
sweep_done(int code, Datum arg)
{
	sweep_note(*(RelFileLocator *) DatumGetPointer(arg), false);
}

static void sweep_store(RelFileLocator rel, const char *path,
						BmapSweep *result);

/* blocks a sweep found, as many as it finds */
typedef struct BlockList
{
	BlockNumber *blocks;
	uint32		n;
	uint32		cap;
} BlockList;

static void
block_list_add(BlockList *l, BlockNumber b)
{
	if (l->n == l->cap)
	{
		l->cap = Max(l->cap * 2, 1024);
		l->blocks = l->blocks ?
			repalloc_huge(l->blocks, l->cap * sizeof(BlockNumber)) :
			MemoryContextAllocHuge(CurrentMemoryContext,
								   l->cap * sizeof(BlockNumber));
	}
	l->blocks[l->n++] = b;
}

void
bmap_sweep(Oid spcOid, Oid dbOid, BmapSweep *result)
{
	RelFileLocator rel = {spcOid, dbOid, InvalidRelFileNumber};
	char	   *path = GetDatabasePath(dbOid, spcOid);
	uint64		released = 0;

	memset(result, 0, sizeof(*result));
	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("cannot sweep a page store during recovery")));
	if (!alloc_exists(rel))
		return;
	swept_note(rel, true, &released);
	sweep_note(rel, true);
	PG_ENSURE_ERROR_CLEANUP(sweep_done, PointerGetDatum(&rel));
	{
		sweep_store(rel, path, result);
	}
	PG_END_ENSURE_ERROR_CLEANUP(sweep_done, PointerGetDatum(&rel));
	sweep_done(0, PointerGetDatum(&rel));
	result->truncated = sweep_shrink(rel);
	swept_note(rel, false, &released);
	pfree(path);
}

/*
 * Is a sweep of a database's page store in a tablespace due?  Once after
 * the server starts (a crash before may have leaked blocks: a drop's or a
 * prune's), and whenever a stuck count has lost a holder since the last
 * sweep began (it keeps it, stuck: the sweep may free the block or fit the
 * count).  A count left stuck by a sweep, its holders all still there, is
 * no reason for another.  Past the most stores remembered, a store not
 * among them isn't due.
 */
bool
bmap_sweep_due(Oid spcOid, Oid dbOid)
{
	RelFileLocator rel = {spcOid, dbOid, InvalidRelFileNumber};
	BmapSwept  *found;
	bool		due;

	if (!alloc_exists(rel))
		return false;
	SpinLockAcquire(&bmap_shared->mutex);
	found = swept_find(rel);
	if (found == NULL)
		due = bmap_shared->nswept < BMAP_SWEPT_MAX;
	else
		due = !found->swept || found->released != found->swept_at;
	SpinLockRelease(&bmap_shared->mutex);
	return due;
}

static void
sweep_store(RelFileLocator rel, const char *path, BmapSweep *result)
{
	Oid			spcOid = rel.spcOid;
	Oid			dbOid = rel.dbOid;
	LOCKTAG		tag;
	Buffer		metabuf;
	BmapMeta   *meta;
	BlockNumber hw;
	uint64	   *allocated;
	uint16	   *holders;
	DIR		   *dir;
	struct dirent *de;
	BlockList	tofree = {0};
	BlockList	stuck = {0};
	BlockNumber nrefpages;

	SET_LOCKTAG_OBJECT(tag, dbOid, BMAP_SWEEP_CLASSID, spcOid, 0);
	(void) LockAcquire(&tag, ExclusiveLock, false, false);

	/* the blocks allocated as it begins: the only ones it may free */
	metabuf = alloc_read(rel, 0);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(metabuf), BMAP_PAGE_META, rel, MAIN_FORKNUM, 0);
	meta = PageGetBmapMeta(BufferGetPage(metabuf));
	hw = meta->hw;
	allocated = MemoryContextAllocHuge(CurrentMemoryContext,
									   (hw / 64 + 1) * sizeof(uint64));
	for (uint32 pageno = 0; pageno * BMAP_BITS_PER_PAGE < hw; pageno++)
	{
		Buffer		buf = alloc_read(rel, meta->bitmap[pageno]);
		BlockNumber first = pageno * BMAP_BITS_PER_PAGE;
		uint32		nbits = Min(BMAP_BITS_PER_PAGE, hw - first);

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		check_page(BufferGetPage(buf), BMAP_PAGE_BITMAP, rel, MAIN_FORKNUM,
				   meta->bitmap[pageno]);
		memcpy(allocated + first / 64, PageGetBmapBits(BufferGetPage(buf)),
			   (nbits + 63) / 64 * sizeof(uint64));
		UnlockReleaseBuffer(buf);
	}
	UnlockReleaseBuffer(metabuf);

	/* what every relation holds */
	holders = MemoryContextAllocHuge(CurrentMemoryContext,
									 (hw + 1) * sizeof(uint16));
	memset(holders, 0, (hw + 1) * sizeof(uint16));
	dir = AllocateDir(path);
	while ((de = ReadDir(dir, path)) != NULL)
	{
		RelFileNumber relnumber;
		char	   *end;
		SMgrRelation reln;

		relnumber = (RelFileNumber) strtoul(de->d_name, &end, 10);
		if (end == de->d_name || strcmp(end, "_bmap") != 0)
			continue;
		reln = smgropen(with_relnumber(rel, relnumber), INVALID_PROC_NUMBER);
		sweep_relation(reln, holders, hw);
		result->relations++;
	}
	FreeDir(dir);

	/* nothing holds them: freed; stuck counts that fit: rewritten */
	nrefpages = refs_nblocks(rel);
	for (BlockNumber b = 0; b < hw; b++)
	{
		if ((allocated[b / 64] & (UINT64CONST(1) << (b % 64))) == 0)
			continue;
		if (holders[b] == 0)
		{
			block_list_add(&tofree, b);
			if (REF_PAGE(b) < nrefpages && lookup_ref(rel, b) != 0)
				block_list_add(&stuck, b);
		}
		else if (REF_PAGE(b) < nrefpages && lookup_ref(rel, b) == BMAP_REFS_STUCK)
		{
			if (holders[b] - 1 < BMAP_REFS_STUCK)
				block_list_add(&stuck, b);
			else
				result->stuck++;
		}
	}
	INJECTION_POINT("bmap-sweep-counted", NULL);
	result->recounted = sweep_recount(rel, stuck.blocks, stuck.n, holders);
	LockRelease(&tag, ExclusiveLock, false);

	/* (a block nothing holds gets no new holder: freed after the lock) */
	free_blocks(rel, tofree.blocks, tofree.n);
	result->freed = tofree.n;

	pfree(allocated);
	pfree(holders);
	if (tofree.blocks)
		pfree(tofree.blocks);
	if (stuck.blocks)
		pfree(stuck.blocks);
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
	SMgrRelation srels[3];

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
	srels[2] = refs_smgr(rel);
	if (!smgrexists(srels[0], MAIN_FORKNUM) &&
		!smgrexists(srels[1], MAIN_FORKNUM) &&
		!smgrexists(srels[2], MAIN_FORKNUM))
		return;
	/* at once, as in redo: nothing may use the tablespace any more */
	smgrdounlinkall(srels, 3, true);
}

/* ----------------------------------------------------------------
 * Inspection, for tests
 * ----------------------------------------------------------------
 */

/*
 * Logical block 'blkno' of a block-mapped relation: its physical block
 * (InvalidBlockNumber if unmapped), the entry's birth epoch and the
 * block's reference count.
 */
void
bmap_inspect_block(SMgrRelation reln, BlockNumber blkno, BlockNumber *phys,
				   uint32 *epoch, uint32 *refs)
{
	BmapEntry	entry;

	*phys = InvalidBlockNumber;
	*epoch = 0;
	*refs = 0;
	if (!lookup_entry(reln, blkno, &entry))
		return;
	*phys = entry.block;
	*epoch = entry.epoch;
	*refs = lookup_ref(reln->smgr_rlocator.locator, entry.block);
}

/* a block-mapped relation's epoch */
uint32
bmap_inspect_epoch(SMgrRelation reln)
{
	return lookup_epoch(reln);
}

/* the allocator's high-water mark and free count, of rel's page store */
void
bmap_inspect_allocator(RelFileLocator rel, BlockNumber *hw, uint32 *nfree)
{
	Buffer		buf = alloc_read(rel, 0);

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(buf), BMAP_PAGE_META, rel, MAIN_FORKNUM, 0);
	*hw = PageGetBmapMeta(BufferGetPage(buf))->hw;
	*nfree = PageGetBmapMeta(BufferGetPage(buf))->nfree;
	UnlockReleaseBuffer(buf);
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
		rlocator.locator.relNumber == BMAP_STORE_RELNUMBER ||
		rlocator.locator.relNumber == BMAP_REFS_RELNUMBER)
		return false;
	return bmap_relation_exists(rlocator.locator);
}

void
bmapopen(SMgrRelation reln)
{
	mdopen(reln);
	reln->bmap_pretrunc = InvalidBuffer;
	reln->bmap_pretrunc_chg[0] = reln->bmap_pretrunc_chg[1] = InvalidBuffer;
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
	delta.epoch = 0;
	delta.fold_epoch = 0;

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
 * The map and changes forks are the storage manager's own, so they are
 * hidden from callers that go through every fork (copying a relation,
 * measuring it).
 */
bool
bmapexists(SMgrRelation reln, ForkNumber forknum)
{
	if (forknum == MAIN_FORKNUM)
		return mdexists(reln, BMAP_FORKNUM);
	if (forknum == BMAP_FORKNUM || forknum == BMAP_CHANGES_FORKNUM)
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
		/*
		 * The primary logged the frees; in recovery nothing reuses the name.
		 * The map's and changes list's buffers go first: dropping the
		 * relation's buffers left them, taking the forks smgrexists() hides
		 * for absent, and one written out later would find no file.
		 */
		ForkNumber	forks[2] = {BMAP_FORKNUM, BMAP_CHANGES_FORKNUM};
		BlockNumber firsts[2] = {0, 0};

		DropRelationBuffers(smgropen(rlocator.locator, INVALID_PROC_NUMBER),
							forks, 2, firsts);
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
	INJECTION_POINT("bmap-predrop", NULL);
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

		/*
		 * A block without an entry has nothing to prefetch.  In recovery the
		 * map's header on disk may be newer than the map page that will have
		 * the entry, so a block below the length may have none yet; a
		 * prefetch is only a hint.
		 */
		if (!BlockNumberIsValid(phys))
		{
			blocknum++;
			nblocks--;
			continue;
		}
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

/*
 * In recovery, the version of logical block 'blkno' in 'page' was made by
 * records from before its entry's block was set: it is of the block it had
 * then, which needs nothing from it (what it should keep was written out
 * before the change), while the block it has now may still be another
 * relation's.
 */
static bool
page_predates_entry(SMgrRelation reln, BlockNumber blkno, const void *page)
{
	PGIOAlignedBlock copy;
	BmapEntry  *entry = &PageGetBmapEntries(copy.data)[MAP_SLOT(blkno)];

	read_node(reln, MAP_PAGE(blkno), true, copy.data, BMAP_PAGE_NODE);
	return PageGetLSN((const PageData *) page) < entry->since;
}

void
bmapwritev(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		   const void **buffers, BlockNumber nblocks, bool skipFsync)
{
	SMgrRelation ssrel;
	bool		recovery = RecoveryInProgress();

	if (forknum != MAIN_FORKNUM)
	{
		mdwritev(reln, forknum, blocknum, buffers, nblocks, skipFsync);
		return;
	}
	ssrel = store_smgr(reln->smgr_rlocator.locator);
	while (nblocks > 0)
	{
		BlockNumber run;
		BlockNumber phys = lookup_ext(reln, blocknum, recovery ? 1 : nblocks,
									  &run, false, true);

		if (!recovery || !page_predates_entry(reln, blocknum, buffers[0]))
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
 * isn't safe, so the header is read beforehand, and held exclusively until
 * the truncation: no fold begins, and the changes list isn't pruned,
 * meanwhile.
 */
void
bmappretruncate(SMgrRelation reln, ForkNumber forknum)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		headerbuf;
	Buffer		chg[2] = {InvalidBuffer, InvalidBuffer};
	bool		flushed = false;

	if (forknum != MAIN_FORKNUM || InRecovery)
		return;

	/* (pins a failed truncation left are gone with its resource owner) */
	reln->bmap_pretrunc = InvalidBuffer;
	reln->bmap_pretrunc_chg[0] = reln->bmap_pretrunc_chg[1] = InvalidBuffer;

	/*
	 * A folded relation's pages past the new length stay as of the fold
	 * until it is extended again (which lists them), but truncating drops
	 * their buffers, written or not: the pages are written first.  (Writes
	 * read the header, so before it is held.)
	 */
	for (;;)
	{
		if (!flushed && changes_fold_epoch(reln) > 0)
		{
			FlushRelationsAllBuffers(&reln, 1);
			flushed = true;
		}
		headerbuf = map_read(rel, 0);
		LockBuffer(headerbuf, BUFFER_LOCK_EXCLUSIVE);
		check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
		/* (a fold that began meanwhile) */
		if (flushed || PageGetBmapHeader(BufferGetPage(headerbuf))->fold_epoch == 0)
			break;
		UnlockReleaseBuffer(headerbuf);
	}

	/*
	 * A relation folded lists its truncations: its changes list's header and
	 * the page its next entry goes on, which stays next while the header is
	 * held.
	 */
	if (PageGetBmapHeader(BufferGetPage(headerbuf))->fold_epoch > 0)
	{
		BlockNumber pageno;

		chg[0] = chg_read(rel, 0);
		LockBuffer(chg[0], BUFFER_LOCK_SHARE);
		check_page(BufferGetPage(chg[0]), BMAP_PAGE_CHANGES_HEAD, rel,
				   BMAP_CHANGES_FORKNUM, 0);
		pageno = CHANGE_PAGE(PageGetBmapChangesHead(BufferGetPage(chg[0]))->nentries);
		LockBuffer(chg[0], BUFFER_LOCK_UNLOCK);
		chg[1] = changes_next_page(reln, pageno);
		LockBuffer(chg[1], BUFFER_LOCK_UNLOCK);
	}
	reln->bmap_pretrunc = headerbuf;
	reln->bmap_pretrunc_chg[0] = chg[0];
	reln->bmap_pretrunc_chg[1] = chg[1];
	INJECTION_POINT("bmap-pretruncated", NULL);
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
	 * reuses them, and dropping it frees them.  A folded relation lists the
	 * truncation.
	 */
	if (nblocks < curnblk)
	{
		if (BufferIsValid(reln->bmap_pretrunc_chg[0]))
		{
			ChangesAppend ca = {reln->bmap_pretrunc_chg[0],
			reln->bmap_pretrunc_chg[1]};

			set_length_listed(reln, reln->smgr_rlocator.locator, headerbuf,
							  nblocks, &ca);
		}
		else
			set_length(reln->smgr_rlocator.locator, headerbuf, nblocks);
	}
	UnlockReleaseBuffer(headerbuf);
	for (int i = 0; i < 2; i++)
	{
		if (BufferIsValid(reln->bmap_pretrunc_chg[i]))
			ReleaseBuffer(reln->bmap_pretrunc_chg[i]);
		reln->bmap_pretrunc_chg[i] = InvalidBuffer;
	}
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
	if (chg_exists(reln))
		mdimmedsync(reln, BMAP_CHANGES_FORKNUM);
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
	if (chg_exists(reln))
		mdregistersync(reln, BMAP_CHANGES_FORKNUM);
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
			case BMAP_DELTA_REFS:
				apply_refs_delta(page, (xl_bmap_refs_delta *) copy);
				break;
			case BMAP_DELTA_CHANGES_HEAD:
				apply_changes_head_delta(page, (xl_bmap_changes_head_delta *) copy);
				break;
			case BMAP_DELTA_CHANGES:
				apply_changes_delta(page, (xl_bmap_changes_delta *) copy);
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

				for (uint8 id = 0; id <= XLogRecMaxBlockId(record); id++)
					redo_block(record, id);
				forget_cached_size(xlrec->locator);
				break;
			}
		case XLOG_BMAP_SHRINK:
			{
				xl_bmap_shrink *xlrec = (xl_bmap_shrink *) XLogRecGetData(record);
				SMgrRelation store = store_smgr(xlrec->locator);

				redo_block(record, 0);

				/*
				 * The minimum recovery point past the record first, as
				 * smgr_redo() does for a truncation: once the file is
				 * shorter, a crash must not end recovery before it.
				 */
				XLogFlush(record->EndRecPtr);
				if (smgrexists(store, MAIN_FORKNUM) &&
					smgrnblocks(store, MAIN_FORKNUM) > xlrec->new_hw)
					smgrtruncate(store, &(ForkNumber) {MAIN_FORKNUM}, 1,
								 &(BlockNumber) {smgrnblocks(store, MAIN_FORKNUM)},
								 &xlrec->new_hw);
				break;
			}
		case XLOG_BMAP_FREE:
		case XLOG_BMAP_SHARE:
		case XLOG_BMAP_UNREF:
		case XLOG_BMAP_FOLD:
		case XLOG_BMAP_CHANGES:
		case XLOG_BMAP_SHARE_CHANGES:
			for (uint8 id = 0; id <= XLogRecMaxBlockId(record); id++)
				redo_block(record, id);
			break;
		case XLOG_BMAP_REMAP:
			{
				xl_bmap_remap *xlrec = (xl_bmap_remap *) XLogRecGetData(record);
				uint8		last = XLogRecMaxBlockId(record);

				bool		zeroed = (xlrec->flags & BMAP_REMAP_ZERO) != 0;

				/*
				 * The page store reaches the new block; the page itself is
				 * the last block, an image, so redo never reads the block,
				 * or zeroes, which redo writes there.  The old block keeps
				 * what it had: shared blocks were synced when shared.
				 */
				prepare_store(xlrec->locator, xlrec->old_hw, xlrec->new_hw,
							  &xlrec->new_block,
							  zeroed && xlrec->new_block < xlrec->old_hw ? 1 : 0,
							  !zeroed);
				for (uint8 id = 0; id < last; id++)
					redo_block(record, id);
				if (zeroed)
				{
					/* zeroes in its buffer too, without an LSN, as a new page */
					Buffer		buf = XLogInitBufferForRedo(record, last);

					UnlockReleaseBuffer(buf);
				}
				else
					redo_block(record, last);

				/*
				 * A listed block no other map holds keeps the page as it was
				 * before the change: the image, or zeroes.  What the primary
				 * wrote there before the change may not have reached it
				 * before a crash.  One another map holds keeps what it has,
				 * as on the primary: the image may have hints only this
				 * map's buffer had.
				 *
				 * Nothing written since can be lost here: a block let go of
				 * later is taken again only by a relation whose writes are
				 * logged (alloc_take()), and those records follow this one.
				 */
				if ((xlrec->flags & (BMAP_REMAP_LISTED | BMAP_REMAP_OWN)) ==
					(BMAP_REMAP_LISTED | BMAP_REMAP_OWN))
				{
					PGIOAlignedBlock old;

					if (zeroed)
						memset(old.data, 0, BLCKSZ);
					else
					{
						if (!RestoreBlockImage(record, last, old.data))
							elog(PANIC, "%s", record->errormsg_buf);
						PageSetChecksum((Page) old.data, xlrec->blkno);
					}
					smgrwrite(store_smgr(xlrec->locator), MAIN_FORKNUM,
							  xlrec->old_block, old.data, false);
				}
				XLogReplaceBlock(xlrec->locator, MAIN_FORKNUM, xlrec->blkno);
				break;
			}
		case XLOG_BMAP_SHARED:
			{
				xl_bmap_share *xlrec = (xl_bmap_share *) XLogRecGetData(record);
				SMgrRelation src = smgropen(xlrec->src, INVALID_PROC_NUMBER);

				redo_block(record, 0);
				redo_block(record, 1);
				forget_cached_size(xlrec->dst);

				/*
				 * As the primary did: the shared blocks get the source's
				 * pages as of this record, durably.
				 */
				FlushRelationsAllBuffers(&src, 1);
				mdimmedsync(store_smgr(xlrec->src), MAIN_FORKNUM);
				break;
			}
		default:
			elog(PANIC, "bmap_redo: unknown op code %u", info);
	}
}

void
bmap_mask(char *pagedata, BlockNumber blkno)
{
	mask_page_lsn_and_checksum(pagedata);
}
