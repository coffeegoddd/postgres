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
 * own, so a standby couldn't follow the primary's allocations for them, so
 * they aren't shared: a relation made as a share of another gets copies of
 * them (RelationCreateStorageShared()), as they are small.
 *
 * The map.  Block 0 of the map fork is a header holding the relation's
 * logical length and epoch, and block 1 its root page (BmapRoot), the
 * relation's alone, pointing to four subtrees of fixed range.  A subtree's
 * nodes are pages of the page store, read through shared buffers under the
 * store's own file number: a node points to nodes a level down, and a leaf
 * holds the entries of BMAP_TREE_FANOUT consecutive logical blocks.  An
 * entry is a physical block, the epoch it was born in (one born before its
 * map's epoch may be shared with another map), and the WAL position since
 * which it has that block.  Its block changes only while nothing else can
 * change the logical block: when a shared page is copied before it changes
 * (its buffer locked exclusively), or when an entry left past the
 * relation's end by a truncation is let go because another map shares its
 * block.  The map fork's existence is the relation's; smgropen() chooses
 * this storage manager by it.
 *
 * Sharing with other maps (a share, a copy of a snapshot) raises the
 * header's shared_below to the map's epoch: an entry born since can be
 * shared only with the map's own snapshots (bmappagesharedelsewhere()),
 * which VACUUM changes as if unshared.
 *
 * Sharing.  A node is held by every page that points to it, and a data
 * block by every leaf (and changes list) holding it, the reference counts
 * saying how many besides the first.  A share copies the root page alone,
 * each subtree gaining a holder; the first change under a shared node copies
 * the path down to the leaf (tree_copy_node()), each copy's children
 * gaining a holder, and the data block is then copied as any shared block
 * is.  A drop queues its subtrees, and its changes list's blocks, on its
 * page store's release queue, in O(1) of its size; a background drain
 * lets go of each: a count less where another holds it, else freed, its
 * children let go of the same way (see "The release queue").  A node is an
 * incarnation of its block, which the pointer to it names by the record
 * that made it: a lookup that finds another there (a node freed and made
 * something else under it; in recovery, a version written before a crash)
 * looks again, or in recovery takes the subtree for replaced after the
 * record (tree_find()).  A map, or page store, of an older format (the flat
 * maps of version 1) is refused, not read (refuse_version()).
 *
 * Recovery reads map pages that can be from different moments: one written
 * after the point redo has reached, another not yet written.  A version
 * older than the record being replayed gives it the block it was made for.
 * A newer one may give a block set after the record: redo skips the record
 * (smgrreplacedafter()), since the logical block's contents from before
 * the change aren't needed there, and doesn't write pages it made to that
 * block, which may still hold another relation's page at this point of the
 * replay.  An older image of a map page, restored over a newer version,
 * doesn't take that back: recovery remembers what the newer one said
 * (remember_ahead()).
 *
 * The allocator.  Per tablespace and database, BMAP_ALLOC_RELNUMBER is a
 * metapage (the page store's high-water mark, the count of free blocks, and
 * the bitmap pages) and bitmap pages saying which physical blocks are used.
 * Freed blocks are reused before the page store grows.  A reused block is
 * zeroed before it is mapped, so a crash can't expose its old contents at a
 * new logical block.  Truncation shortens the logical length, and once
 * it is done (outside its critical section) lets go of the blocks past it,
 * freeing those no other map or changes list holds (#638).  A crash in
 * between leaves them mapped past the length, where a later truncation or
 * extension lets go of them, or dropping the relation frees them.
 *
 * Locking.  Lookups read the root page and the nodes down to a leaf under
 * share locks, one at a time.  Extension holds the header exclusively
 * (serializing the relation's extensions and the tree's growth): it reads
 * the leaf, takes blocks under the allocator's metapage and bitmap pages'
 * locks, and then locks the leaf exclusively.  Truncation takes the header's.
 * Writing a page out looks it up without loading map pages into shared
 * buffers (see read_node()), but share-locks a map page it finds cached,
 * so a write may wait for a change of that page.  Any buffer read may be
 * the eviction that writes a page out, so a backend holding a map page
 * locked for a change reads no buffer: a change reads every buffer it
 * needs (the changes list's, the reference counts', the allocator's)
 * first and locks the map page last (map_lock(), which assert-enabled
 * builds check at every read).  The one exception is a node just made: it
 * maps no page a write could look up until the page that points to it is
 * locked, last.  Nodes are made and copied under the header held
 * exclusively (a change prepared under it held shared lets go and takes it
 * so to copy a path); a write looks its page up from the root page, never
 * the header, which the backend writing it out may hold exclusively.
 *
 * The sweep (bmap_sweep()).  Freeing is by reference count, which leaks in
 * four cases: a crash between a drop's commit and the records queuing what
 * it didn't queue before its commit record (snapshots whose takers hadn't
 * committed, a prepared transaction's drops), a crash part way through a prune, a crash (or an error, not an
 * interrupt, which the header's lock holds off) between the records
 * letting go of a truncation's blocks and those freeing them, and a count
 * stuck at 255 maps.
 * A sweep lets go of the snapshots their takers never committed (a crash
 * between a snapshot's taking and its transaction's commit leaves one,
 * whose slot names that transaction: snapshot_abandoned()), drains the
 * release queue, then counts exactly what every
 * map (and changes list) of a database's page store holds, each
 * relation's read under its header's lock, and what the queue still holds
 * (drains held off meanwhile), and
 * frees the blocks allocated as it began that nothing holds, rewrites the
 * stuck counts that now fit, moves the blocks past the allocated count down
 * into free ones (compact_store()), and shortens the store to its last
 * allocated block (SHRINK, which redo truncates by too).  Branch creation (a share,
 * the one thing that adds holders) waits for it while it counts, and the
 * allocator reuses no freed block of the store while a sweep of it runs:
 * one freed and taken again meanwhile could be held by a map the sweep
 * read before.  A dropped
 * relation's map counts until it is removed, once its blocks are queued
 * (a tree's root page is written out with no subtrees once they are
 * queued, as the nodes it named may be freed); an empty main fork file
 * then keeps its relation file number until the next checkpoint, as
 * md.c's does.  A tree's node counts once for each page pointing to
 * it, its children once however many reach it.  A path copy (not held off)
 * adds holders only to blocks held already; one adding a holder to a stuck
 * count makes a sweep under way leave the stuck counts as they are.
 *
 * Scans (bmap_scan_store()).  A scan of a page store reads every block
 * allocated, or only the ranges of blocks written or freed since an
 * earlier scan, which its caller names by the token it was given: from
 * the first scan on, every write of the store's blocks sets its range's
 * bit in shared memory (store_written()), a scan taking and clearing them
 * as it begins.  Nothing of this is on disk: a token from before the
 * server (or its shared memory) started again names no scan, and neither
 * does one whose store was watched no more (too many stores watched, the
 * store dropped) or outgrew its bits, so each such scan reads every block.
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
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "catalog/pg_tablespace.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "common/hashfn.h"
#include "common/int.h"
#include "common/relpath.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "port/pg_bitutils.h"
#include "storage/bmap.h"
#include "storage/bmap_xlog.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/ipc.h"
#include "storage/md.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/spin.h"
#include "storage/subsystems.h"
#include "utils/hsearch.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"
#include "utils/relfilenumbermap.h"
#include "utils/resowner.h"
#include "utils/timestamp.h"

bool		debug_block_mapped_heap = false;
int			block_map_replay_entries = 65536;

/* ----------------------------------------------------------------
 * Page layouts
 * ----------------------------------------------------------------
 */

#define BMAP_MAGIC			0x424D4150	/* "BMAP" */
/*
 * The format's version, which headers, allocators' metapages and changes
 * lists' headers carry.  Version 1 maps were flat, an entry per logical
 * block in the relation's own map fork; version 2's were trees of pages of
 * the page store, whose snapshots didn't say who took them; version 3's
 * release queue entries didn't say which drop's transaction they wait for;
 * version 4's headers didn't say which entries may be shared with another
 * map.  This server reads only version 5 and refuses anything older
 * (refuse_version()).
 */
#define BMAP_VERSION		5

#define BMAP_PAGE_META		1
#define BMAP_PAGE_HEADER	2
#define BMAP_PAGE_BITMAP	4
#define BMAP_PAGE_REFS		5
#define BMAP_PAGE_CHANGES_HEAD	6
#define BMAP_PAGE_CHANGES	7
#define BMAP_PAGE_ROOT		8		/* a tree's root page: block 1 */
#define BMAP_PAGE_TREE		9		/* a tree's node, in the page store */
#define BMAP_PAGE_QUEUE_META	10	/* the release queue's metapage */
#define BMAP_PAGE_QUEUE		11		/* and its pages of entries */

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
	uint32		shared_below;	/* entries born before it may be shared
								 * with another map; those born since, only
								 * with the map's own snapshots */
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
#define BMAP_MAX_BITMAPS \
	((uint32) ((BMAP_CONTENT_SIZE - offsetof(BmapMeta, bitmap)) / sizeof(BlockNumber)))
#define BMAP_BITS_PER_PAGE \
	((uint32) ((BMAP_CONTENT_SIZE / sizeof(uint64)) * 64))

/* the release queue's metapage: how many entries are queued */
typedef struct BmapQueueMeta
{
	uint32		version;
	uint32		nentries;
} BmapQueueMeta;

#define BMAP_REFS_PER_PAGE	((uint32) BMAP_CONTENT_SIZE)
#define BMAP_CHANGES_PER_PAGE \
	((uint32) (BMAP_CONTENT_SIZE / sizeof(BmapChange)))

/* the changes list's page and slot of entry 'i' */
#define CHANGE_PAGE(i)		((BlockNumber) (1 + (i) / BMAP_CHANGES_PER_PAGE))
#define CHANGE_SLOT(i)		((uint32) ((i) % BMAP_CHANGES_PER_PAGE))

/* the release queue's page and slot of entry 'i' */
#define BMAP_QUEUE_PER_PAGE \
	((uint32) (BMAP_CONTENT_SIZE / sizeof(BmapQueued)))
#define QUEUE_PAGE(i)		((BlockNumber) (1 + (i) / BMAP_QUEUE_PER_PAGE))
#define QUEUE_SLOT(i)		((uint32) ((i) % BMAP_QUEUE_PER_PAGE))

/* a count that reaches this stays, until a sweep counts it exactly */
#define BMAP_REFS_STUCK		255

/* the lock a sweep holds off branch creation with: an object no catalog has */
#define BMAP_SWEEP_CLASSID	((Oid) 0x424D5357)

/*
 * The lock a transaction that queued its drops before its commit record
 * holds, by its id, until they are done (see bmapprecommitdrop())
 */
#define BMAP_DROP_CLASSID	((Oid) 0x424D4452)
#define DROP_LOCK(tag, xid) \
	SET_LOCKTAG_OBJECT(tag, MyDatabaseId, BMAP_DROP_CLASSID, (xid), 0)

/* reference-count pages one record changes at most */
#define BMAP_REFS_PER_RECORD	24

/* the reference-count page and slot of physical block 'phys' */
#define REF_PAGE(phys)		((BlockNumber) ((phys) / BMAP_REFS_PER_PAGE))
#define REF_SLOT(phys)		((uint32) ((phys) % BMAP_REFS_PER_PAGE))


/*
 * A tree's node: a page of the page store, read through shared buffers under
 * the store's file number (data pages never are: theirs are logical), whose
 * special space says what it is.  A node is an incarnation of its block:
 * 'born' is the WAL position of the record that made it, which the pointer
 * to it carries as its 'since', and no other node of the block has (a block
 * is made a node again only after it was freed).  Level 0 is a leaf, whose
 * entries map logical blocks to physical ones; a node of level k > 0
 * points to nodes of level k - 1, with entries of the same shape (the child's block, the epoch
 * the pointer was set in, the child's birth).
 */
typedef struct BmapTreeOpaqueData
{
	uint16		type;			/* BMAP_PAGE_TREE */
	uint16		level;
	uint32		magic;
	XLogRecPtr	born;
	BlockNumber first;			/* the first logical block it maps */
	uint32		unused;
} BmapTreeOpaqueData;

#define BMAP_TREE_SPECIAL	MAXALIGN(sizeof(BmapTreeOpaqueData))
#define BMAP_TREE_FANOUT \
	((uint32) ((BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - BMAP_TREE_SPECIAL) / \
			   sizeof(BmapEntry)))

#define PageGetBmapTreeOpaque(page) \
	((BmapTreeOpaqueData *) PageGetSpecialPointer(page))

/*
 * A tree's root page, block 1 of its map fork, the relation's own: a pointer
 * to each of its subtrees.  Subtree k, of level k, maps BMAP_TREE_FANOUT^(k+1)
 * logical blocks from where subtree k - 1's end: a relation of fewer blocks
 * than a leaf maps has one level, and the four together map every block a
 * relation can have.  No subtree ever gets a level above it, so a pointer's
 * node is replaced only by its copy, by a node made where there was none,
 * or by nothing (see tree_find()).
 */
#define BMAP_TREE_TOPS		4

/*
 * The root page keeps snapshots too (bmap_snapshot_take()): each the
 * pointers to the subtrees the relation had when it was taken, which it
 * holds as a page pointing to them does, and the relation's length then.
 */
#define BMAP_TREE_SNAPSHOTS	BMAP_SNAPSHOTS_MAX

typedef struct BmapRoot
{
	BmapEntry	top[BMAP_TREE_TOPS];
	xl_bmap_snapshot snap[BMAP_TREE_SNAPSHOTS];
} BmapRoot;

StaticAssertDecl(sizeof(xl_bmap_top) == sizeof(BmapEntry),
				 "a snapshot's pointer is a map entry");
StaticAssertDecl(BMAP_DELTA_ROOT_TOPS == BMAP_TREE_TOPS,
				 "a root page's record has each of its pointers");

#define PageGetBmapRoot(page)	((BmapRoot *) PageGetContents(page))

/* a map fork's pages, a tree's: its header and its root page */
#define BMAP_TREE_MAP_PAGES	2

/* blocks allocated by one record at most */
#define BMAP_PIECE			64
/* bitmap pages one allocation takes free blocks from at most */
#define BMAP_SCAN_PAGES		2

#define PageGetBmapOpaque(page) \
	((BmapPageOpaqueData *) PageGetSpecialPointer(page))
#define PageGetBmapHeader(page)	((BmapHeader *) PageGetContents(page))
/* a node's or a root page's contents are entries */
#define PageGetBmapEntries(page)	((BmapEntry *) PageGetContents(page))
#define PageGetBmapMeta(page)	((BmapMeta *) PageGetContents(page))
#define PageGetBmapBits(page)	((uint64 *) PageGetContents(page))
#define PageGetBmapRefs(page)	((uint8 *) PageGetContents(page))
#define PageGetBmapChangesHead(page)	((BmapChangesHead *) PageGetContents(page))
#define PageGetBmapChanges(page)	((BmapChange *) PageGetContents(page))
#define PageGetBmapQueueMeta(page)	((BmapQueueMeta *) PageGetContents(page))
#define PageGetBmapQueued(page)	((BmapQueued *) PageGetContents(page))

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

/* page stores whose blocks written since their last scan are watched, at most */
#define BMAP_WATCHED_MAX	64

/* a watched store's bits, each for a range of its blocks */
#define BMAP_WATCH_BITS		32768

/*
 * A page store watched since a scan of it (bmap_scan_store()): a bit a range
 * of 1 << shift blocks, set once a block of the range is written or freed
 * after the scan began, and the scan's token.  Writers set bits without a
 * lock; the rest is the mutex's.
 */
typedef struct BmapWatch
{
	pg_atomic_uint32 shift;
	uint64		token;			/* of the store's last scan */
	uint64		used;			/* when it last began, as watch_clock was */
	pg_atomic_uint32 bits[BMAP_WATCH_BITS / 32];
} BmapWatch;

/*
 * Shared by every backend: the sweeps running, during which the allocator
 * of a page store being swept reuses no freed block (one freed and taken
 * again meanwhile could be held by a map the sweep read before); the page
 * stores swept since the server started; and the page stores watched for
 * scans, each named by its watch_key[] (tablespace and database, 0: none),
 * which every write of a page store reads without a lock.
 */

/* page stores whose queue was last found empty, remembered at most */
#define BMAP_QUEUE_HINTS	64

typedef struct BmapShared
{
	pg_atomic_uint32 sweeps;	/* running, anywhere */
	pg_atomic_uint64 stuck_held;	/* holders a stuck count gained, ever */
	pg_atomic_uint64 watch_key[BMAP_WATCHED_MAX];
	slock_t		mutex;			/* protects what follows */
	int			nlisted;
	Oid			listed[BMAP_SWEEPS_LISTED][2];	/* tablespace, database */
	int			unlisted;		/* past the most listed: every store is */
	int			nswept;
	BmapSwept	swept[BMAP_SWEPT_MAX];

	/* the remaps ahead of replay (bmap_ahead), and how many there are */
	LWLock		ahead_lock;
	pg_atomic_uint32 nahead;

	/*
	 * Release queue pushes, counted, and for some page stores (by their
	 * watch_key(), in the slot it hashes to) the count when their queue was
	 * last found empty: equal, no queue has had an entry pushed since.  A
	 * hint, for bmapbeforeextend()'s cheap test: two stores taking one slot
	 * at once may leave it wrong, which only costs or skips a drain.
	 */
	pg_atomic_uint64 queue_pushes;
	pg_atomic_uint64 queue_empty_key[BMAP_QUEUE_HINTS];
	pg_atomic_uint64 queue_empty_at[BMAP_QUEUE_HINTS];

	uint64		next_token;		/* a scan's, unique to this shared memory */
	uint64		watch_clock;
	BmapWatch	watch[BMAP_WATCHED_MAX];
} BmapShared;

static BmapShared *bmap_shared = NULL;

/*
 * A logical block whose map page recovery replaced with an older image, and
 * the WAL position since which the version replaced gave it its physical
 * block (see remember_ahead()), named by its page store, the block of its
 * leaf (which relations may share) and its slot there.
 */
typedef struct BmapAheadKey
{
	RelFileLocator rel;
	BlockNumber blkno;
	uint32		slot;
} BmapAheadKey;

typedef struct BmapAhead
{
	BmapAheadKey key;
	XLogRecPtr	since;
} BmapAhead;

static HTAB *bmap_ahead = NULL;

static void
BmapShmemRequest(void *arg)
{
	ShmemRequestStruct(.name = "block map",
					   .size = sizeof(BmapShared),
					   .ptr = (void **) &bmap_shared,
		);
	ShmemRequestHash(.name = "block map remaps ahead of replay",
					 .nelems = block_map_replay_entries,
					 .ptr = &bmap_ahead,
					 .hash_info.keysize = sizeof(BmapAheadKey),
					 .hash_info.entrysize = sizeof(BmapAhead),
					 .hash_flags = HASH_ELEM | HASH_BLOBS | HASH_FIXED_SIZE,
		);
}

static void
BmapShmemInit(void *arg)
{
	pg_atomic_init_u32(&bmap_shared->sweeps, 0);
	pg_atomic_init_u64(&bmap_shared->stuck_held, 0);
	SpinLockInit(&bmap_shared->mutex);
	bmap_shared->nlisted = 0;
	bmap_shared->unlisted = 0;
	bmap_shared->nswept = 0;
	LWLockInitialize(&bmap_shared->ahead_lock,
					 LWLockNewTrancheId("block map replay"));
	pg_atomic_init_u32(&bmap_shared->nahead, 0);
	pg_atomic_init_u64(&bmap_shared->queue_pushes, 0);
	for (int i = 0; i < BMAP_QUEUE_HINTS; i++)
	{
		pg_atomic_init_u64(&bmap_shared->queue_empty_key[i], 0);
		pg_atomic_init_u64(&bmap_shared->queue_empty_at[i], PG_UINT64_MAX);
	}

	/*
	 * Scans' tokens begin anywhere: one remembered from before the server
	 * (or its shared memory, after a crash) began again names no scan now.
	 */
	if (!pg_strong_random(&bmap_shared->next_token,
						  sizeof(bmap_shared->next_token)))
		bmap_shared->next_token = (uint64) GetCurrentTimestamp() << 16;
	bmap_shared->watch_clock = 0;
	for (int i = 0; i < BMAP_WATCHED_MAX; i++)
	{
		pg_atomic_init_u64(&bmap_shared->watch_key[i], 0);
		pg_atomic_init_u32(&bmap_shared->watch[i].shift, 0);
		bmap_shared->watch[i].token = 0;
		bmap_shared->watch[i].used = 0;
		for (int w = 0; w < BMAP_WATCH_BITS / 32; w++)
			pg_atomic_init_u32(&bmap_shared->watch[i].bits[w], 0);
	}
}

/* a page store's name among those watched */
static inline uint64
watch_key(RelFileLocator rel)
{
	return ((uint64) rel.spcOid << 32) | rel.dbOid;
}

/*
 * Physical blocks [blk, blk + n) of rel's page store were just written, or
 * freed: a scan watching the store reads their ranges again.  Noted after
 * the write, so a scan that begins watching after this looks finds what
 * was written, and one that began before finds the bits set.
 */
static void
store_written(RelFileLocator rel, BlockNumber blk, BlockNumber n)
{
	uint64		key = watch_key(rel);

	if (n == 0)
		return;
	pg_memory_barrier();
	for (int i = 0; i < BMAP_WATCHED_MAX; i++)
	{
		BmapWatch  *w;
		uint32		shift;
		uint64		last;

		if (pg_atomic_read_u64(&bmap_shared->watch_key[i]) != key)
			continue;
		w = &bmap_shared->watch[i];
		shift = pg_atomic_read_u32(&w->shift);
		last = Min(((uint64) blk + n - 1) >> shift, BMAP_WATCH_BITS - 1);
		for (uint64 r = (uint64) blk >> shift; r <= last; r++)
			pg_atomic_fetch_or_u32(&w->bits[r / 32], UINT32_C(1) << (r % 32));
		break;
	}
}

/* rel's page store is gone: what a scan of it found holds no more */
static void
store_unwatch(RelFileLocator rel)
{
	uint64		key = watch_key(rel);

	SpinLockAcquire(&bmap_shared->mutex);
	for (int i = 0; i < BMAP_WATCHED_MAX; i++)
	{
		if (pg_atomic_read_u64(&bmap_shared->watch_key[i]) != key)
			continue;
		pg_atomic_write_u64(&bmap_shared->watch_key[i], 0);
		bmap_shared->watch[i].token = 0;
	}
	SpinLockRelease(&bmap_shared->mutex);
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

struct MapPage;
static void list_stale(SMgrRelation reln, const struct MapPage *mp,
					   Buffer mapbuf, BlockNumber blkno,
					   BlockNumber old_block, uint32 epoch);
static void unshare_stale(SMgrRelation reln, const struct MapPage *leaf,
						  uint32 epoch, uint32 fold_epoch,
						  BlockNumber from, BlockNumber to,
						  bool release);
static void share_changes(SMgrRelation src, SMgrRelation dst);
static void create_map(SMgrRelation reln);
struct BmapEntry;
static void tree_release_tops(SMgrRelation reln, const struct BmapEntry *tops);

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

static void sync_store_later(RelFileLocator rel);

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

static SMgrRelation
queue_smgr(RelFileLocator rel)
{
	return smgropen(with_relnumber(rel, BMAP_QUEUE_RELNUMBER),
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

/*
 * A block map, or a page store, made by a server of an older format, which
 * this one doesn't read: its pages would be taken for something else.
 */
static void
refuse_version(uint32 version, RelFileLocator rel, ForkNumber fork)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("\"%s\" is block-mapped storage of format version %u, which this server does not read",
					relpathperm(rel, fork).str, version),
			 errdetail("This server reads format version %u only.", BMAP_VERSION),
			 errhint("Recreate the database from a dump made by the server that wrote it.")));
}

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

	/* (the pages that say their version: the first word of their contents) */
	if ((type == BMAP_PAGE_HEADER || type == BMAP_PAGE_META ||
		 type == BMAP_PAGE_CHANGES_HEAD || type == BMAP_PAGE_QUEUE_META) &&
		*(uint32 *) PageGetContents(page) != BMAP_VERSION)
		refuse_version(*(uint32 *) PageGetContents(page), rel, fork);
}

/*
 * A map page locked for a change, and the rule that comes with it: while a
 * backend holds one locked it reads no buffer, as any read may evict a page
 * the map page maps, whose write would look it up there (read_node()).  A
 * map page just added, or found all zeroes, maps nothing, and is exempt:
 * so is a tree's node just made, which nothing points to yet, and which may
 * stay locked while the page that is to point to it is locked last.
 * Assert-enabled builds check the rule at each of this file's buffer reads,
 * whether or not a read happens to evict such a page; the resource owner
 * forgets the lock if an error releases it.
 */
#ifdef USE_ASSERT_CHECKING
#define MAP_LOCKED_MAX	2

static Buffer map_locked[MAP_LOCKED_MAX] = {InvalidBuffer, InvalidBuffer};
static bool map_locked_new[MAP_LOCKED_MAX];

static void
map_locked_forget(Buffer buf)
{
	for (int i = 0; i < MAP_LOCKED_MAX; i++)
		if (map_locked[i] == buf)
		{
			map_locked[i] = InvalidBuffer;
			return;
		}
}

static void
map_locked_release(Datum res)
{
	map_locked_forget(DatumGetInt32(res));
}

static const ResourceOwnerDesc map_locked_desc = {
	.name = "block map page locked for a change",
	.release_phase = RESOURCE_RELEASE_BEFORE_LOCKS,
	.release_priority = RELEASE_PRIO_FIRST,
	.ReleaseResource = map_locked_release,
	.DebugPrint = NULL
};

static void
map_locked_note(Buffer buf, bool isnew)
{
	int			free = -1;

	if (!CurrentResourceOwner)
		return;
	for (int i = 0; i < MAP_LOCKED_MAX; i++)
	{
		/* (at most one changing page, beside new ones) */
		Assert(!BufferIsValid(map_locked[i]) || map_locked_new[i] || isnew);
		if (!BufferIsValid(map_locked[i]) && free < 0)
			free = i;
	}
	Assert(free >= 0);
	ResourceOwnerEnlarge(CurrentResourceOwner);
	ResourceOwnerRemember(CurrentResourceOwner, Int32GetDatum(buf),
						  &map_locked_desc);
	map_locked[free] = buf;
	map_locked_new[free] = isnew;
}
#endif

static void
map_lock(Buffer buf, bool isnew)
{
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
#ifdef USE_ASSERT_CHECKING
	map_locked_note(buf, isnew);
#endif
}

/* (a page map_new_page() or tree_new_page() returned, locked already) */
static void
map_locked_as_new(Buffer buf)
{
#ifdef USE_ASSERT_CHECKING
	map_locked_note(buf, true);
#endif
}

static void
map_unlock(Buffer buf)
{
#ifdef USE_ASSERT_CHECKING
	for (int i = 0; i < MAP_LOCKED_MAX; i++)
		if (map_locked[i] == buf)
		{
			ResourceOwnerForget(CurrentResourceOwner, Int32GetDatum(buf),
								&map_locked_desc);
			map_locked[i] = InvalidBuffer;
			break;
		}
#endif
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
}

/* a buffer read: no map page may be locked for a change (map_lock()) */
static inline void
check_may_read(void)
{
#ifdef USE_ASSERT_CHECKING
	for (int i = 0; i < MAP_LOCKED_MAX; i++)
		Assert(!BufferIsValid(map_locked[i]) || map_locked_new[i]);
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
init_header(Page page)
{
	init_page(page, BMAP_PAGE_HEADER);
	PageGetBmapHeader(page)->version = BMAP_VERSION;
}

static void
init_root(Page page)
{
	BmapRoot   *root;

	init_page(page, BMAP_PAGE_ROOT);
	root = PageGetBmapRoot(page);
	for (int k = 0; k < BMAP_TREE_TOPS; k++)
		root->top[k].block = InvalidBlockNumber;
}

/* a tree's node of 'level', mapping from logical block 'first', no entries */
static void
init_tree_node(Page page, uint16 level, BlockNumber first, XLogRecPtr born)
{
	BmapTreeOpaqueData *opaque;
	BmapEntry  *entries;

	PageInit(page, BLCKSZ, sizeof(BmapTreeOpaqueData));
	opaque = PageGetBmapTreeOpaque(page);
	opaque->type = BMAP_PAGE_TREE;
	opaque->level = level;
	opaque->magic = BMAP_MAGIC;
	opaque->born = born;
	opaque->first = first;
	opaque->unused = 0;
	entries = PageGetBmapEntries(page);
	memset(entries, 0, BMAP_TREE_FANOUT * sizeof(BmapEntry));
	for (uint32 i = 0; i < BMAP_TREE_FANOUT; i++)
		entries[i].block = InvalidBlockNumber;
	/* the contents fill the page; nothing for a full-page image to skip */
	((PageHeader) page)->pd_lower = ((PageHeader) page)->pd_upper;
}

/*
 * Is 'page' a tree's node of 'level' mapping from 'first', born at 'born':
 * the incarnation of its block a pointer to it names?  A page of zeroes, one
 * that fails verification, another kind of page, or another incarnation (a
 * block freed and made something else since the pointer was read; in
 * recovery, a version written before a crash) is not.
 */
static bool
tree_node_is(const PageData *page, uint16 level, BlockNumber first,
			 XLogRecPtr born)
{
	const BmapTreeOpaqueData *opaque;

	if (PageIsNew(page) ||
		((const PageHeaderData *) page)->pd_special != BLCKSZ - BMAP_TREE_SPECIAL)
		return false;
	opaque = (const BmapTreeOpaqueData *) PageGetSpecialPointer(page);
	return opaque->magic == BMAP_MAGIC && opaque->type == BMAP_PAGE_TREE &&
		opaque->level == level && opaque->first == first &&
		opaque->born == born;
}

/* is 'page' a tree's node, of any incarnation? */
static bool
tree_node_any(const PageData *page)
{
	const BmapTreeOpaqueData *opaque;

	if (PageIsNew(page) ||
		((const PageHeaderData *) page)->pd_special != BLCKSZ - BMAP_TREE_SPECIAL)
		return false;
	opaque = (const BmapTreeOpaqueData *) PageGetSpecialPointer(page);
	return opaque->magic == BMAP_MAGIC && opaque->type == BMAP_PAGE_TREE;
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
	if (delta->flags & BMAP_DELTA_SHARED_BELOW)
		PageGetBmapHeader(page)->shared_below = delta->shared_below;
}

/* (whether it changed the page) */
static bool
apply_node_delta(Page page, const xl_bmap_node_delta *delta)
{
	const xl_bmap_entry_delta *ent = (const xl_bmap_entry_delta *) (delta + 1);
	BmapEntry  *entries;
	bool		changed = false;

	entries = PageGetBmapEntries(page);
	for (uint32 i = 0; i < delta->nentries; i++)
	{
		BmapEntry  *e = &entries[ent[i].slot];

		Assert(ent[i].slot < BMAP_TREE_FANOUT);
		/* (in redo, an entry kept from a newer page: see redo_block()) */
		if (e->since > delta->since)
			continue;
		if (e->block != ent[i].block)
			e->since = delta->since;
		else if (e->epoch == ent[i].epoch)
			continue;
		e->block = ent[i].block;
		e->epoch = ent[i].epoch;
		changed = true;
	}
	return changed;
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

static void
apply_root_delta(Page page, const xl_bmap_root_delta *delta)
{
	BmapRoot   *root;

	init_root(page);
	root = PageGetBmapRoot(page);
	Assert(delta->ntops <= BMAP_TREE_TOPS);
	for (uint32 k = 0; k < delta->ntops; k++)
	{
		root->top[k].block = delta->tops[k].block;
		root->top[k].epoch = delta->tops[k].epoch;
		root->top[k].since = delta->tops[k].since;
	}
}

static void
apply_snapshot_delta(Page page, const xl_bmap_snapshot_delta *delta)
{
	Assert(delta->slot < BMAP_TREE_SNAPSHOTS);
	PageGetBmapRoot(page)->snap[delta->slot] = delta->snap;
}

/* a snapshot's pointers, as map entries */
static inline const BmapEntry *
snapshot_tops(const xl_bmap_snapshot *snap)
{
	return (const BmapEntry *) snap->tops;
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
 * An allocator's first page can exist without its initialization record:
 * extension writes zeroes before INIT is logged, and a crash or error can
 * leave them there.  File length alone does not mean it is ready to use.
 * Read under the buffer lock so a concurrent initializer can finish first.
 */
static bool
alloc_initialized(RelFileLocator rel)
{
	Buffer		buf;
	Page		page;
	bool		initialized;

	if (!alloc_exists(rel))
		return false;
	buf = alloc_read(rel, 0);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	initialized = !PageIsNew(page) || !pg_memory_is_all_zeros(page, BLCKSZ);
	if (initialized)
		check_page(page, BMAP_PAGE_META,
				   with_relnumber(rel, BMAP_ALLOC_RELNUMBER), MAIN_FORKNUM, 0);
	UnlockReleaseBuffer(buf);
	return initialized;
}

static void ensure_queue(RelFileLocator rel);

/*
 * Make sure the tablespace and database of 'rel' have an allocator, a page
 * store and a release queue.
 */
static void
ensure_alloc(RelFileLocator rel)
{
	LOCKTAG		tag;

	ensure_queue(rel);
	if (alloc_initialized(rel))
		return;

	SET_LOCKTAG_RELATION(tag, rel.dbOid, BMAP_ALLOC_RELNUMBER);
	LockAcquire(&tag, AccessExclusiveLock, false, false);
	if (!alloc_initialized(rel))
	{
		RelFileLocator aloc = with_relnumber(rel, BMAP_ALLOC_RELNUMBER);
		RelFileLocator sloc = with_relnumber(rel, BMAP_STORE_RELNUMBER);
		RelFileLocator rloc = with_relnumber(rel, BMAP_REFS_RELNUMBER);
		SMgrRelation asrel = alloc_smgr(rel);
		SMgrRelation ssrel = store_smgr(rel);
		SMgrRelation rsrel = refs_smgr(rel);
		BlockNumber nalloc;
		BlockNumber nstore;
		BlockNumber nrefs;
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

		/*
		 * Reuse only an interrupted first initialization.  An allocator
		 * with bitmap pages or a nonempty store/count file must not be
		 * reset: doing so could allocate blocks that still hold rows.
		 */
		nalloc = smgrnblocks(asrel, MAIN_FORKNUM);
		nstore = smgrnblocks(ssrel, MAIN_FORKNUM);
		nrefs = smgrnblocks(rsrel, MAIN_FORKNUM);
		if (nalloc > 1 || nstore != 0 || nrefs != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("cannot initialize block map allocator \"%s\" with existing storage",
							relpathperm(aloc, MAIN_FORKNUM).str),
					 errdetail("The allocator header is missing or zeroed, but storage already exists (allocator: %u blocks, page store: %u blocks, reference counts: %u blocks).",
							   nalloc, nstore, nrefs),
					 errhint("Restore the affected database from a known-good backup.")));
		if (nalloc == 0)
			buf = alloc_new_page(rel);
		else
		{
			buf = alloc_read(rel, 0);
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		}
		if (BufferGetBlockNumber(buf) != 0)
			elog(ERROR, "block map allocator of database %u begins at block %u",
				 rel.dbOid, BufferGetBlockNumber(buf));
		page = BufferGetPage(buf);
		INJECTION_POINT("bmap-alloc-extended", NULL);

		START_CRIT_SECTION();
		init_meta(page);
		MarkBufferDirty(buf);
		XLogBeginInsert();
		XLogRegisterBuffer(0, buf, REGBUF_WILL_INIT | REGBUF_STANDARD);
		PageSetLSN(page, XLogInsert(RM_BMAP_ID, XLOG_BMAP_INIT));
		/*
		 * Before anyone can extend the allocator or page store, make INIT
		 * durable.  Otherwise a crash could lose INIT after those files grew,
		 * leaving more than the empty first page we can safely retry above.
		 * This is paid once per database/tablespace, not per relation.
		 */
		XLogFlush(PageGetLSN(page));
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

/*
 * The release queue's file (see "The release queue" below), made with the
 * allocator, of its metapage at least: the page itself is initialized by
 * the first record that pushes an entry.  The file is logged as made; its
 * pages aren't, as reference-count pages aren't (refs_cover()), so a
 * standby, or a server restored from a backup, may have the file without
 * its metapage, which it then gets here.
 */
static void
ensure_queue(RelFileLocator rel)
{
	SMgrRelation qsrel = queue_smgr(rel);
	LOCKTAG		tag;

	if (smgrexists(qsrel, MAIN_FORKNUM) && smgrnblocks(qsrel, MAIN_FORKNUM) > 0)
		return;
	SET_LOCKTAG_RELATION(tag, rel.dbOid, BMAP_QUEUE_RELNUMBER);
	(void) LockAcquire(&tag, AccessExclusiveLock, false, false);
	if (!smgrexists(qsrel, MAIN_FORKNUM))
	{
		RelFileLocator qloc = with_relnumber(rel, BMAP_QUEUE_RELNUMBER);

		smgrcreate(qsrel, MAIN_FORKNUM, false);
		log_smgrcreate(&qloc, MAIN_FORKNUM);
	}
	check_may_read();
	if (smgrnblocks(qsrel, MAIN_FORKNUM) == 0)
		ReleaseBuffer(ExtendBufferedRelTo(BMR_SMGR(qsrel, RELPERSISTENCE_PERMANENT),
										  MAIN_FORKNUM, NULL,
										  EB_SKIP_EXTENSION_LOCK, 1,
										  RBM_NORMAL));
	LockRelease(&tag, AccessExclusiveLock, false);
}

/* does rel's store have a release queue? */
static bool
queue_exists(RelFileLocator rel)
{
	SMgrRelation qsrel = queue_smgr(rel);

	return smgrexists(qsrel, MAIN_FORKNUM) && smgrnblocks(qsrel, MAIN_FORKNUM) > 0;
}

/* ----------------------------------------------------------------
 * Lookups
 * ----------------------------------------------------------------
 */

/*
 * Tree lookups.  A tree map's root page is block 1 of its map fork (see
 * BmapRoot), so a map fork of two pages whose block 1 is a root page is a
 * tree's.
 * Lookups read the root page and the nodes down to the leaf one at a time,
 * each under a share lock, with no lock held while the next is read: a
 * node a lookup can reach is either the relation's own, changed only under
 * its buffer lock, or shared and so unchanging.  A node freed and made
 * something else under a lookup that had read the pointer to it isn't the
 * incarnation the pointer names (tree_node_is()), and the lookup begins
 * again from the root page.  In recovery the map read is as it stands,
 * pages of it maybe newer than the replay; a pointer whose node isn't there
 * (made after the replay point, or made something else since) means the
 * subtree was replaced or let go of after the record being replayed: see
 * tree_find().
 */

/* the path from the root page to logical block 'blkno''s leaf entry */
typedef struct TreePath
{
	int			top;			/* the root page's pointer */
	uint32		slot[BMAP_TREE_TOPS];	/* the entry at each level */
	BlockNumber first[BMAP_TREE_TOPS];	/* the first block each node maps */
} TreePath;

/* the logical blocks a node of 'level' maps */
static inline uint64
tree_span(int level)
{
	uint64		span = BMAP_TREE_FANOUT;

	for (int l = 0; l < level; l++)
		span *= BMAP_TREE_FANOUT;
	return span;
}

static void
tree_path(BlockNumber blkno, TreePath *path)
{
	uint64		base = 0;
	uint64		off;
	int			k = 0;

	while (k < BMAP_TREE_TOPS - 1 && blkno >= base + tree_span(k))
	{
		base += tree_span(k);
		k++;
	}
	path->top = k;
	off = blkno - base;
	for (int l = k; l >= 0; l--)
	{
		uint64		unit = l == 0 ? 1 : tree_span(l - 1);

		path->slot[l] = (uint32) ((off / unit) % BMAP_TREE_FANOUT);
		path->first[l] = (BlockNumber) (base + off / (unit * BMAP_TREE_FANOUT) *
										(unit * BMAP_TREE_FANOUT));
	}
}

/* the page store's buffer of a tree's node, read */
static Buffer
tree_read(RelFileLocator rel, BlockNumber node, ReadBufferMode mode)
{
	check_may_read();
	return ReadBufferWithoutRelcache(with_relnumber(rel, BMAP_STORE_RELNUMBER),
									 MAIN_FORKNUM, node, mode, NULL, true);
}

/* What read_entries() found. */
typedef enum PageRead
{
	PAGE_READ,					/* the page, read */
	PAGE_NOT_CACHED,			/* not in shared buffers (cached_only) */
	PAGE_TORN,					/* read from disk, it failed verification */
} PageRead;

/*
 * Read a page of a map: block 'blk' of reln's map fork if !node, else of
 * its page store (a tree's node).  Into *page_out a copy of the page's
 * header and special space (enough for the checks above), and into dst its
 * entries [slot, slot + n).  See read_node() for 'for_write': with it, or
 * 'cached_only', a page not cached isn't loaded into shared buffers; with
 * 'cached_only' it isn't read at all.  A node that fails verification reads
 * as zeroes (it may be a block made a data page since, whose checksum is of
 * a logical block number).
 */
static PageRead
read_entries(SMgrRelation reln, bool node, BlockNumber blk, bool for_write,
			 bool cached_only, uint32 slot, uint32 n, BmapEntry *dst,
			 PGIOAlignedBlock *page_out)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	SMgrRelation fsrel = node ? store_smgr(rel) : reln;
	ForkNumber	fork = node ? MAIN_FORKNUM : BMAP_FORKNUM;
	RelFileLocator floc = node ? with_relnumber(rel, BMAP_STORE_RELNUMBER) : rel;
	Buffer		buf = InvalidBuffer;
	char	   *page;

	if (for_write || cached_only)
	{
		PrefetchBufferResult r = PrefetchSharedBuffer(fsrel, fork, blk);

		if (BufferIsValid(r.recent_buffer) &&
			ReadRecentBuffer(floc, fork, blk, r.recent_buffer))
		{
			buf = r.recent_buffer;
			/* a change keeping the page locked (map_lock()) can't be waited for */
			if (BufferIsLockedByMe(buf))
			{
				ReleaseBuffer(buf);
				if (cached_only)
					return PAGE_NOT_CACHED;
				elog(ERROR, "map page %u of \"%s\" is locked by the backend writing a page it maps",
					 blk, relpathperm(floc, fork).str);
			}
		}
		else if (cached_only)
			return PAGE_NOT_CACHED;
		else
		{
			void	   *dstpage = page_out->data;

			mdreadv(fsrel, fork, blk, &dstpage, 1);
			if (!PageIsVerified(page_out->data, blk, 0, NULL))
			{
				if (!node)
					return PAGE_TORN;
				memset(page_out->data, 0, BLCKSZ);
			}
			memcpy(dst, PageGetBmapEntries(page_out->data) + slot,
				   n * sizeof(BmapEntry));
			return PAGE_READ;
		}
	}
	else if (node)
		buf = tree_read(rel, blk, RBM_ZERO_ON_ERROR);
	else
		buf = map_read(rel, blk);

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	memcpy(page_out->data, page, MAXALIGN(SizeOfPageHeaderData));
	memcpy(page_out->data + BLCKSZ - BMAP_TREE_SPECIAL,
		   page + BLCKSZ - BMAP_TREE_SPECIAL, BMAP_TREE_SPECIAL);
	memcpy(dst, PageGetBmapEntries(page) + slot, n * sizeof(BmapEntry));
	UnlockReleaseBuffer(buf);
	return PAGE_READ;
}

/* Does a page read_entries() read say it is of 'type' (a map fork's)? */
static bool
map_page_is(const PGIOAlignedBlock *page, uint16 type)
{
	const BmapPageOpaqueData *opaque;

	if (PageIsNew(page->data) ||
		((const PageHeaderData *) page->data)->pd_special !=
		BLCKSZ - MAXALIGN(sizeof(BmapPageOpaqueData)))
		return false;
	opaque = (const BmapPageOpaqueData *) PageGetSpecialPointer(page->data);
	return opaque->magic == BMAP_MAGIC && opaque->type == type;
}

/* What tree_find() found. */
typedef enum TreeFound
{
	TREE_LEAF,					/* the leaf, its entries read */
	TREE_NO_LEAF,				/* no leaf maps the block */
	TREE_NOT_CACHED,			/* a page wasn't cached (cached_only) */
	TREE_UNAVAILABLE,			/* in recovery: see tree_find() */
} TreeFound;

/* the leaf tree_find() found */
typedef struct TreeLeaf
{
	BlockNumber node;
	XLogRecPtr	born;
	BlockNumber first;			/* the first block it maps */
	uint32		slot;			/* the block's */
} TreeLeaf;

/* tries of a lookup that finds a node changed under it, outside recovery */
#define TREE_FIND_TRIES		8

/*
 * Look up logical block 'blkno' in reln's map, if it is a tree: its leaf
 * into *leaf, and the leaf's entries from the block's on, at most n (and
 * none past the leaf), into entries[].  See read_entries() for 'for_write'
 * and 'cached_only'.
 *
 * In recovery a node may not be the incarnation the pointer to it names,
 * however often it is read: the pointer is of a version of its page newer
 * than the replay, and the node it names was made after the replay point
 * (and not yet written out before a crash), or made something else after
 * it was let go of.  Either way the subtree that held the block when the
 * record being replayed was made was replaced or let go of since: copied
 * because it was shared, and then its relation's pages were written and
 * synced before the share committed, so its blocks have what the records
 * before then changed; or let go of by a truncation, a drop or a root
 * swap, after which nothing reads its pages.  (A subtree never gets a level
 * above it, which would replace a pointer with one to a node that still
 * needs the old.)  So the block's records from before then are skipped,
 * and its pages go nowhere: TREE_UNAVAILABLE.
 */
static TreeFound
tree_find(SMgrRelation reln, BlockNumber blkno, bool for_write,
		  bool cached_only, uint32 n, BmapEntry *entries, TreeLeaf *leaf)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	TreePath	path;
	int			mismatches = 0;

	tree_path(blkno, &path);
	n = Min(n, BMAP_TREE_FANOUT - path.slot[0]);
	Assert(n > 0);
	for (;;)
	{
		PGIOAlignedBlock page;
		BmapEntry	ptr;
		PageRead	r;
		int			level;

		r = read_entries(reln, false, 1, for_write, cached_only, path.top, 1,
						 &ptr, &page);
		if (r == PAGE_NOT_CACHED)
			return TREE_NOT_CACHED;
		if (r == PAGE_TORN)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid page in block %u of \"%s\"",
							1, relpathperm(rel, BMAP_FORKNUM).str)));
		/* (a map of an older format has something else there) */
		if (!map_page_is(&page, BMAP_PAGE_ROOT))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("block 1 of \"%s\" is not the root page of a block map of format version %u, which this server reads only",
							relpathperm(rel, BMAP_FORKNUM).str, BMAP_VERSION),
					 errhint("Recreate the database from a dump made by the server that wrote it.")));

		for (level = path.top; level >= 0; level--)
		{
			BmapEntry	next;

			if (!BlockNumberIsValid(ptr.block))
				return TREE_NO_LEAF;
			/* (in recovery, a pointer of a newer page may be past the store) */
			if (RecoveryInProgress() && !cached_only &&
				ptr.block >= mdnblocks(store_smgr(rel), MAIN_FORKNUM))
				break;
			r = read_entries(reln, true, ptr.block, for_write, cached_only,
							 path.slot[level], level == 0 ? n : 1,
							 level == 0 ? entries : &next, &page);
			if (r == PAGE_NOT_CACHED)
				return TREE_NOT_CACHED;
			if (!tree_node_is(page.data, level, path.first[level], ptr.since))
				break;
			if (level == 0)
			{
				leaf->node = ptr.block;
				leaf->born = ptr.since;
				leaf->first = path.first[0];
				leaf->slot = path.slot[0];
				return TREE_LEAF;
			}
			ptr = next;
		}

		/* a node not the one its pointer names */
		mismatches++;
		if (RecoveryInProgress() && mismatches > 1)
			return TREE_UNAVAILABLE;
		if (mismatches >= TREE_FIND_TRIES)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("block map of \"%s\" points to block %u of its page store, which is not the node it names",
							relpath(reln->smgr_rlocator, MAIN_FORKNUM).str,
							ptr.block)));
		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * The leaf a logical block's entry is on.  What changes an entry reads and
 * locks it through this.
 */
typedef struct MapPage
{
	BlockNumber blk;			/* the leaf's block */
	BlockNumber first;			/* the logical block of its first entry */
	uint32		nslots;
	XLogRecPtr	born;			/* a leaf's */
} MapPage;

#define MP_SLOT(mp, blkno)	((uint32) ((blkno) - (mp)->first))

static void
mp_leaf(const TreeLeaf *leaf, MapPage *mp)
{
	mp->blk = leaf->node;
	mp->first = leaf->first;
	mp->nslots = BMAP_TREE_FANOUT;
	mp->born = leaf->born;
}

static Buffer
mp_read(SMgrRelation reln, const MapPage *mp)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;

	return tree_read(rel, mp->blk, RBM_NORMAL);
}

/* Verify a locked leaf, which is what it was found to be. */
static void
mp_check(SMgrRelation reln, const MapPage *mp, Page page)
{
	if (!tree_node_is(page, 0, mp->first, mp->born))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("block %u of the page store of \"%s\" is not the leaf of its block map it was",
						mp->blk, relpath(reln->smgr_rlocator, MAIN_FORKNUM).str)));
}

/*
 * Logical block 'blkno''s entry, read under a share lock, and in *mp (if
 * given) the leaf it is on.  False if it has none: no leaf maps it, or its
 * entry maps no block; in recovery, also if *unavailable is set (see
 * tree_find()).  'entry' gets the entry found on a leaf even if it maps no
 * block (a cleared entry keeps its position).
 */
static bool
lookup_entry_at(SMgrRelation reln, BlockNumber blkno, BmapEntry *entry,
				MapPage *mp, bool *unavailable)
{
	TreeLeaf	leaf;

	if (unavailable)
		*unavailable = false;
	switch (tree_find(reln, blkno, false, false, 1, entry, &leaf))
	{
		case TREE_LEAF:
			if (mp)
				mp_leaf(&leaf, mp);
			return BlockNumberIsValid(entry->block);
		case TREE_UNAVAILABLE:
			if (unavailable)
				*unavailable = true;
			pg_fallthrough;
		default:
			entry->block = InvalidBlockNumber;
			entry->since = InvalidXLogRecPtr;
			return false;
	}
}
static bool
lookup_entry(SMgrRelation reln, BlockNumber blkno, BmapEntry *entry)
{
	return lookup_entry_at(reln, blkno, entry, NULL, NULL);
}

/*
 * The logical blocks a tree's leaves map: up to the end of its last leaf
 * (past its relation's length too).  The rightmost pointer of each node,
 * from the last subtree that has one.
 */
static uint64
tree_map_blocks(SMgrRelation reln)
{
	PGIOAlignedBlock page;
	BmapEntry	tops[BMAP_TREE_TOPS];
	uint64		base = 0;
	uint64		end = 0;

	(void) read_entries(reln, false, 1, false, false, 0, BMAP_TREE_TOPS, tops,
						&page);
	for (int k = 0; k < BMAP_TREE_TOPS; k++)
	{
		BmapEntry	ptr = tops[k];
		uint64		first = base;

		base += tree_span(k);
		if (!BlockNumberIsValid(ptr.block))
			continue;
		for (int level = k; level >= 0; level--)
		{
			BmapEntry	entries[BMAP_TREE_FANOUT];
			int			last = -1;

			(void) read_entries(reln, true, ptr.block, false, false, 0,
								BMAP_TREE_FANOUT, entries, &page);
			if (!tree_node_is(page.data, level, (BlockNumber) first, ptr.since))
				break;
			if (level == 0)
			{
				end = first + BMAP_TREE_FANOUT;
				break;
			}
			for (int i = BMAP_TREE_FANOUT - 1; i >= 0 && last < 0; i--)
				if (BlockNumberIsValid(entries[i].block))
					last = i;
			if (last < 0)
				break;
			first += (uint64) last * tree_span(level - 1);
			ptr = entries[last];
		}
	}
	return Min(end, (uint64) MaxBlockNumber + 1);
}

/* the logical blocks the map's leaves have entries for, past its length too */
static uint64
map_blocks(SMgrRelation reln)
{
	return tree_map_blocks(reln);
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
 * Verify map page 'blk' of 'rel'.  A page of zeroes (an extension's whose
 * record a crash lost) passes if 'new_ok' (returning false), else fails.
 */
static bool
check_node(char *page, uint16 type, RelFileLocator rel,
		   BlockNumber blk, bool new_ok)
{
	if (new_ok && PageIsNew(page))
		return false;
	check_page(page, type, rel, BMAP_FORKNUM, blk);
	return true;
}

/*
 * Copy map page 'blk' of 'rel', pinned in 'buf', into 'dst' under a share
 * lock, verified as in check_node().
 */
static bool
copy_node(RelFileLocator rel, Buffer buf, BlockNumber blk, char *dst,
		  uint16 type, bool new_ok)
{
	bool		found;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	found = check_node(BufferGetPage(buf), type, rel, blk, new_ok);
	memcpy(dst, BufferGetPage(buf), BLCKSZ);
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	return found;
}

/*
 * Map page 'blk' without loading it into shared buffers: in *buf the buffer
 * holding it, pinned, if it is cached; otherwise InvalidBuffer, and the page
 * read from disk into 'dst'.  A read that fails verification (torn by a
 * concurrent write) is made 'tries' times in all; false if each one failed.
 */
static bool
read_map_uncached(SMgrRelation reln, BlockNumber blk, char *dst, int tries,
				  Buffer *buf)
{
	PrefetchBufferResult r = PrefetchSharedBuffer(reln, BMAP_FORKNUM, blk);

	if (BufferIsValid(r.recent_buffer) &&
		ReadRecentBuffer(reln->smgr_rlocator.locator, BMAP_FORKNUM, blk,
						 r.recent_buffer))
	{
		*buf = r.recent_buffer;
		return true;
	}
	*buf = InvalidBuffer;
	for (int i = 0; i < tries; i++)
	{
		void	   *page = dst;

		mdreadv(reln, BMAP_FORKNUM, blk, &page, 1);
		if (PageIsVerified(dst, blk, 0, NULL))
			return true;
	}
	return false;
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
 *
 * The page is verified as in check_node(); see read_node_ext().
 */
static bool
read_node_ext(SMgrRelation reln, BlockNumber blk, bool for_write, char *dst,
			  uint16 type, bool new_ok)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		buf;
	bool		found;

	if (for_write)
	{
		if (!read_map_uncached(reln, blk, dst, 4, &buf))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid page in block %u of \"%s\"",
							blk, relpathperm(rel, BMAP_FORKNUM).str)));
		if (!BufferIsValid(buf))
			return check_node(dst, type, rel, blk, new_ok);

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

	found = copy_node(rel, buf, blk, dst, type, new_ok);
	ReleaseBuffer(buf);
	return found;
}

/* read_node_ext() of a page that must be initialized */
static void
read_node(SMgrRelation reln, BlockNumber blk, bool for_write, char *dst,
		  uint16 type)
{
	(void) read_node_ext(reln, blk, for_write, dst, type, false);
}

/*
 * The relation's length in its map's header, and in *lsn (if given) the
 * header's LSN.  See read_node() for 'for_write'.
 */
static BlockNumber
header_nblocks(SMgrRelation reln, bool for_write, XLogRecPtr *lsn)
{
	PGIOAlignedBlock copy;

	read_node(reln, 0, for_write, copy.data, BMAP_PAGE_HEADER);
	if (lsn)
		*lsn = PageGetLSN(copy.data);
	return PageGetBmapHeader(copy.data)->nblocks;
}

/*
 * Has logical block 'blkno' no entry, let go of after 'lsn'?  (A cleared
 * entry keeps the position of the record that cleared it.)  In recovery, a
 * block whose subtree was replaced or let go of after the record being
 * replayed was let go of too (see tree_find()).  See read_node() for
 * 'for_write'.
 */
static bool
let_go_after(SMgrRelation reln, BlockNumber blkno, bool for_write,
			 XLogRecPtr lsn)
{
	BmapEntry	entry;
	TreeLeaf	leaf;

	switch (tree_find(reln, blkno, for_write, false, 1, &entry, &leaf))
	{
		case TREE_LEAF:
			return !BlockNumberIsValid(entry.block) && entry.since > lsn;
		case TREE_UNAVAILABLE:
			return true;
		default:
			return false;
	}
}
/*
 * The physical block of the first of entries[0 .. avail), the entries of
 * consecutive logical blocks from 'blkno' on one map page, and in *run how
 * many of them are physically consecutive; if it has none, how many from
 * it have none.  'entries' NULL: the page has no entries (or there is none).
 */
static BlockNumber
entries_run(SMgrRelation reln, BlockNumber blkno, const BmapEntry *entries,
			uint32 avail, BlockNumber *run, bool missing_ok)
{
	BlockNumber phys = entries ? entries[0].block : InvalidBlockNumber;
	BlockNumber n;

	if (!BlockNumberIsValid(phys))
	{
		n = 1;
		if (entries == NULL)
			n = avail;
		else
			while (n < avail && !BlockNumberIsValid(entries[n].block))
				n++;
		if (missing_ok)
		{
			if (run)
				*run = n;
			return InvalidBlockNumber;
		}
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("block map of \"%s\" has no page for block %u",
						relpath(reln->smgr_rlocator, MAIN_FORKNUM).str,
						blkno)));
	}
	n = 1;
	while (n < avail && entries[n].block == phys + n)
		n++;
	if (run)
		*run = n;
	return phys;
}

/*
 * The physical block of logical block 'blkno', and in *run how many logical
 * blocks from it (at most 'maxrun') are physically consecutive.  If the
 * block has no entry, returns InvalidBlockNumber when 'missing_ok', with
 * *run how many blocks from it have none on its leaf, else fails.  See
 * read_node() for 'for_write'.  In recovery, a block whose subtree isn't
 * there (see tree_find()) has no entry.
 */
static BlockNumber
lookup_ext(SMgrRelation reln, BlockNumber blkno, BlockNumber maxrun,
		   BlockNumber *run, bool missing_ok, bool for_write)
{
	BmapEntry	found[BMAP_TREE_FANOUT];
	TreeLeaf	leaf;
	TreePath	path;
	uint32		avail;
	TreeFound	f;

	tree_path(blkno, &path);
	avail = Min(maxrun, BMAP_TREE_FANOUT - path.slot[0]);
	f = tree_find(reln, blkno, for_write, false, avail, found, &leaf);
	return entries_run(reln, blkno, f == TREE_LEAF ? found : NULL, avail, run,
					   missing_ok);
}
static BlockNumber
lookup(SMgrRelation reln, BlockNumber blkno, BlockNumber maxrun,
	   BlockNumber *run)
{
	return lookup_ext(reln, blkno, maxrun, run, false, false);
}

/*
 * As lookup_ext() with 'missing_ok', but only from a path every page of
 * which is cached: InvalidBlockNumber if it isn't.  For hints, which may
 * come after the map's buffers were dropped without being written (a
 * dropped database's), when the pages on disk may be stale or all zeroes.
 */
static BlockNumber
lookup_cached(SMgrRelation reln, BlockNumber blkno, BlockNumber maxrun,
			  BlockNumber *run)
{
	BmapEntry	found[BMAP_TREE_FANOUT];
	TreeLeaf	leaf;
	TreePath	path;
	uint32		avail;

	tree_path(blkno, &path);
	avail = Min(maxrun, BMAP_TREE_FANOUT - path.slot[0]);
	if (tree_find(reln, blkno, false, true, avail, found, &leaf) != TREE_LEAF)
		return InvalidBlockNumber;
	return entries_run(reln, blkno, found, avail, run, true);
}
/* ----------------------------------------------------------------
 * Allocation
 * ----------------------------------------------------------------
 */

/*
 * Before a block the allocator handed out again is given its new contents:
 * it may have been a tree's node, whose page may still be in a buffer under
 * the page store's own tag, dirty.  A node is freed with no change to its
 * buffer, and the block's new page may be written through another buffer
 * (a relation's own, for a data page), so a stale node page written out
 * later would overwrite it.  The buffer is dropped.  The node's incarnation
 * was let go of, so nothing reads it as a node again (tree_find() checks
 * the birth of what it reads).
 */
static void
drop_node_buffer(RelFileLocator rel, BlockNumber blk)
{
	DropBufferBlock(with_relnumber(rel, BMAP_STORE_RELNUMBER),
					MAIN_FORKNUM, blk);
}

/*
 * In redo of the record at 'lsn': does block 'blk' of rel's page store hold
 * a tree's node born after the record?  Then the record's writes to the
 * block by its address (zeroes, a node's image or entries, a preserved
 * page) are of an incarnation of the block let go of since, which nothing
 * reads any more, and the block is left alone: the node on it may be one a
 * map points to that no record before its birth can make again (a node
 * moved down, written and synced before the record pointing to it; see
 * compact_nodes()).
 */
static bool
redo_newer_node(RelFileLocator rel, BlockNumber blk, XLogRecPtr lsn)
{
	SMgrRelation ssrel = store_smgr(rel);
	PGIOAlignedBlock page;
	void	   *pages[1] = {page.data};

	/*
	 * Read from disk as it is, not through a buffer: the block may hold any
	 * relation's page, whose checksum is of its logical block (and a node
	 * born since was written out before the crash, or no record would be
	 * replayed from before it).
	 */
	if (blk >= mdnblocks(ssrel, MAIN_FORKNUM))
		return false;
	mdreadv(ssrel, MAIN_FORKNUM, blk, pages, 1);
	return tree_node_any(page.data) &&
		PageGetBmapTreeOpaque(page.data)->born > lsn;
}

/*
 * Make the page store hold the blocks below 'new_hw', zeroing the blocks
 * from 'old_hw', and zero the reused blocks.  Done before the record that
 * maps them, and again by its redo unless 'skip_existing': then blocks the
 * page store already has are left as they are.  The bytes count in
 * 'counter': the block map's own page-store writes, shared buffers' extends
 * (which pg_stat_io counts as extends too) or bulk writes' (which nothing
 * else counts) by what the blocks are for.
 *
 * A reused block may have been a tree's node, whose page may still be in a
 * buffer, dirty: it is dropped first (drop_node_buffer()).  In redo (of the
 * record at 'redo_lsn', if valid) a block holding a node born after the
 * record is left alone (redo_newer_node()).
 */
static void
prepare_store(RelFileLocator rel, BlockNumber old_hw, BlockNumber new_hw,
			  const BlockNumber *reused, uint32 nreused, bool skip_existing,
			  PgStat_ProllyCounter counter, XLogRecPtr redo_lsn)
{
	SMgrRelation ssrel = store_smgr(rel);
	BlockNumber store_cur = mdnblocks(ssrel, MAIN_FORKNUM);
	BlockNumber blk;

	for (uint32 i = 0; i < nreused; i++)
	{
		const void *page = zero_block.data;

		if (skip_existing && reused[i] < store_cur)
		{
			drop_node_buffer(rel, reused[i]);
			continue;
		}
		if (!XLogRecPtrIsInvalid(redo_lsn) &&
			redo_newer_node(rel, reused[i], redo_lsn))
			continue;
		drop_node_buffer(rel, reused[i]);
		mdwritev(ssrel, MAIN_FORKNUM, reused[i], &page, 1, false);
		store_written(rel, reused[i], 1);
		pgstat_count_prolly(counter, BLCKSZ);
	}
	for (blk = old_hw; blk < new_hw && blk < store_cur; blk++)
	{
		const void *page = zero_block.data;

		if (skip_existing)
			continue;
		if (!XLogRecPtrIsInvalid(redo_lsn) &&
			redo_newer_node(rel, blk, redo_lsn))
			continue;
		drop_node_buffer(rel, blk);
		mdwritev(ssrel, MAIN_FORKNUM, blk, &page, 1, false);
		store_written(rel, blk, 1);
		pgstat_count_prolly(counter, BLCKSZ);
	}
	if (blk < new_hw)
	{
		mdzeroextend(ssrel, MAIN_FORKNUM, blk, new_hw - blk, false);
		/* (a node's buffer is read with the store's length, in recovery) */
		ssrel->smgr_cached_nblocks[MAIN_FORKNUM] = InvalidBlockNumber;
		store_written(rel, blk, new_hw - blk);
		pgstat_count_prolly(counter,
							(uint64) (new_hw - blk) * BLCKSZ);
	}
}

/*
 * Make the page store reach 'node', a block just allocated to be a tree's
 * node, the high-water mark going from 'old_hw' to 'new_hw': extended, but
 * nothing written over a block it has.  The node is made through its
 * buffer, from the record's image, in redo too, where the version on disk
 * may be newer than the record (see redo_block()).
 */
static void
prepare_node_store(RelFileLocator rel, BlockNumber node, BlockNumber old_hw,
				   BlockNumber new_hw)
{
	SMgrRelation ssrel = store_smgr(rel);
	BlockNumber store_cur = mdnblocks(ssrel, MAIN_FORKNUM);

	if (store_cur < new_hw)
	{
		mdzeroextend(ssrel, MAIN_FORKNUM, store_cur, new_hw - store_cur, false);
		ssrel->smgr_cached_nblocks[MAIN_FORKNUM] = InvalidBlockNumber;
		store_written(rel, store_cur, new_hw - store_cur);
	}
	store_written(rel, node, 1);
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
 * A page whose entry is to point to a tree's node: the root page (block 1
 * of the map fork), or a node, the relation's own, known to be 'level',
 * mapping from 'first', born at 'born'; and the entry, 'slot'.
 */
typedef struct TreeParent
{
	bool		root;
	BlockNumber blk;			/* the node's (!root) */
	uint16		level;
	BlockNumber first;
	XLogRecPtr	born;
	uint32		slot;
} TreeParent;

static Buffer
parent_read(SMgrRelation reln, const TreeParent *parent)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;

	return parent->root ? map_read(rel, 1) :
		tree_read(rel, parent->blk, RBM_NORMAL);
}

/* Verify a parent, locked, which the header's lock keeps as it was found. */
static void
parent_check(SMgrRelation reln, const TreeParent *parent, Page page)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;

	if (parent->root)
		check_page(page, BMAP_PAGE_ROOT, rel, BMAP_FORKNUM, 1);
	else if (!tree_node_is(page, parent->level, parent->first, parent->born))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("block %u of the page store of \"%s\" is not the node of its block map it was",
						parent->blk,
						relpath(reln->smgr_rlocator, MAIN_FORKNUM).str)));
}

/*
 * Make a tree's node of 'level', mapping from logical block 'first', its
 * entries 'entries' (NULL: none), and point 'parent''s entry to it, if
 * given; the pointer, set in 'epoch', is returned.  One record allocates the
 * block, makes the node (as an image) and points the parent to it, so the
 * node is never allocated and unreachable but by a crash before the record.
 * The header is held exclusively: the relation's tree changes no other way
 * meanwhile.  The node's buffer is locked as it is read, before the parent
 * is: nothing points to it yet (map_lock()).  A parent's lock comes last,
 * after the allocator's pages are read.
 */
static BmapEntry
tree_make_node(SMgrRelation reln, const TreeParent *parent, uint32 epoch,
			   uint16 level, BlockNumber first, const BmapEntry *entries)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		parentbuf = InvalidBuffer;
	Buffer		nodebuf;
	BlockNumber node;
	Allocation	alloc;
	xl_bmap_tree xlrec;
	struct
	{
		xl_bmap_node_delta d;
		xl_bmap_entry_delta e;
	}			pdelta;
	BmapEntry	ptr;
	Page		page;
	XLogRecPtr	lsn;
	int			id = 0;

	if (parent)
		parentbuf = parent_read(reln, parent);

	alloc_take(rel, 1, &node, &alloc);
	prepare_node_store(rel, node, alloc.old_hw, alloc.new_hw);
	nodebuf = tree_read(rel, node, RBM_ZERO_AND_LOCK);
	map_locked_as_new(nodebuf);
	if (parent)
	{
		map_lock(parentbuf, false);
		parent_check(reln, parent, BufferGetPage(parentbuf));
	}

	xlrec.locator = rel;
	xlrec.op = BMAP_TREE_NODE;
	xlrec.flags = parent ? BMAP_TREE_INSTALL : 0;
	xlrec.node = node;
	xlrec.from = InvalidBlockNumber;
	xlrec.old_hw = alloc.old_hw;
	xlrec.new_hw = alloc.new_hw;

	XLogEnsureRecordSpace(2 + BMAP_ALLOC_MAX_PAGES, 0);
	ptr.block = node;
	ptr.epoch = epoch;
	ptr.since = GetXLogInsertRecPtr();
	pdelta.d.kind = BMAP_DELTA_NODE;
	pdelta.d.flags = 0;
	pdelta.d.nentries = 1;
	pdelta.d.since = ptr.since;
	pdelta.e.slot = parent ? parent->slot : 0;
	pdelta.e.block = node;
	pdelta.e.epoch = epoch;

	START_CRIT_SECTION();
	page = BufferGetPage(nodebuf);
	init_tree_node(page, level, first, ptr.since);
	if (entries)
		memcpy(PageGetBmapEntries(page), entries,
			   BMAP_TREE_FANOUT * sizeof(BmapEntry));
	MarkBufferDirty(nodebuf);
	if (parent)
	{
		apply_node_delta(BufferGetPage(parentbuf), &pdelta.d);
		MarkBufferDirty(parentbuf);
	}
	XLogBeginInsert();
	XLogRegisterData(&xlrec, sizeof(xlrec));
	if (parent)
	{
		XLogRegisterBuffer(id, parentbuf, REGBUF_STANDARD);
		XLogRegisterBufData(id, &pdelta, sizeof(pdelta));
		id++;
	}
	XLogRegisterBuffer(id, nodebuf, REGBUF_FORCE_IMAGE);
	id++;
	alloc_apply_register(&alloc, id);
	lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_TREE);
	PageSetLSN(page, lsn);
	if (parent)
		PageSetLSN(BufferGetPage(parentbuf), lsn);
	alloc_set_lsn(&alloc, lsn);
	END_CRIT_SECTION();

	alloc_release(&alloc);
	if (parent)
	{
		map_unlock(parentbuf);
		ReleaseBuffer(parentbuf);
	}
	map_unlock(nodebuf);
	ReleaseBuffer(nodebuf);
	INJECTION_POINT("bmap-tree-node-made", NULL);
	return ptr;
}

static BmapEntry tree_copy_node(SMgrRelation reln, const TreeParent *parent,
								uint32 epoch, uint16 level, BlockNumber first,
								const BmapEntry *ptr);

/*
 * Is the node 'ptr' points to the relation's own, its parent being so: set
 * in the map's epoch (no share since, every one raising it), or held by its
 * parent alone?  A node under one another holds is shared through it.
 */
static bool
tree_node_own(RelFileLocator rel, const BmapEntry *ptr, uint32 epoch,
			  bool parent_own)
{
	return parent_own &&
		(ptr->epoch >= epoch || lookup_ref(rel, ptr->block) == 0);
}

/*
 * The leaf of logical block 'blkno' in reln's tree, into *mp, the nodes on
 * its path made if 'make' and there are none (false if there is none and
 * not 'make'), and, with 'own', those shared copied (tree_copy_node()), from
 * the top down, so that the leaf and every node above it is the relation's
 * own.  The header 'headerbuf' is locked exclusively, so the path stays as
 * found.
 */
static bool
tree_leaf_for(SMgrRelation reln, Buffer headerbuf, BlockNumber blkno,
			  bool make, bool own, MapPage *mp)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	uint32		epoch = PageGetBmapHeader(BufferGetPage(headerbuf))->epoch;
	TreePath	path;
	TreeParent	parent;
	BmapEntry	ptr;
	Buffer		buf;
	bool		owned = true;

	Assert(own || !make);
	tree_path(blkno, &path);
	buf = map_read(rel, 1);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(buf), BMAP_PAGE_ROOT, rel, BMAP_FORKNUM, 1);
	ptr = PageGetBmapRoot(BufferGetPage(buf))->top[path.top];
	UnlockReleaseBuffer(buf);
	parent.root = true;
	parent.slot = path.top;

	for (int level = path.top;; level--)
	{
		if (!BlockNumberIsValid(ptr.block))
		{
			if (!make)
				return false;
			ptr = tree_make_node(reln, &parent, epoch, level,
								 path.first[level], NULL);
		}
		else if (own)
		{
			if (!tree_node_own(rel, &ptr, epoch, owned))
				ptr = tree_copy_node(reln, &parent, epoch, level,
									 path.first[level], &ptr);
			owned = true;
		}
		parent.root = false;
		parent.blk = ptr.block;
		parent.level = level;
		parent.first = path.first[level];
		parent.born = ptr.since;
		buf = tree_read(rel, ptr.block, RBM_NORMAL);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		parent_check(reln, &parent, BufferGetPage(buf));
		if (level == 0)
		{
			UnlockReleaseBuffer(buf);
			mp->blk = ptr.block;
			mp->first = path.first[0];
			mp->nslots = BMAP_TREE_FANOUT;
			mp->born = ptr.since;
			return true;
		}
		parent.slot = path.slot[level];
		ptr = PageGetBmapEntries(BufferGetPage(buf))[parent.slot];
		UnlockReleaseBuffer(buf);
	}
}

/*
 * Is the path to logical block 'blkno''s leaf in reln's tree the relation's
 * own, every node on it (see tree_node_own())?  Read under share locks; the
 * caller holds the header, at least shared, so no share or copy of the
 * relation's begins meanwhile.
 */
static bool
tree_path_own(SMgrRelation reln, BlockNumber blkno, uint32 epoch)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	TreePath	path;
	BmapEntry	ptr;
	Buffer		buf;

	tree_path(blkno, &path);
	buf = map_read(rel, 1);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(buf), BMAP_PAGE_ROOT, rel, BMAP_FORKNUM, 1);
	ptr = PageGetBmapRoot(BufferGetPage(buf))->top[path.top];
	UnlockReleaseBuffer(buf);
	for (int level = path.top;; level--)
	{
		if (!BlockNumberIsValid(ptr.block))
			return true;
		if (!tree_node_own(rel, &ptr, epoch, true))
			return false;
		if (level == 0)
			return true;
		buf = tree_read(rel, ptr.block, RBM_NORMAL);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		if (!tree_node_is(BufferGetPage(buf), level, path.first[level], ptr.since))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("block %u of the page store of \"%s\" is not the node of its block map it was",
							ptr.block, relpath(reln->smgr_rlocator, MAIN_FORKNUM).str)));
		ptr = PageGetBmapEntries(BufferGetPage(buf))[path.slot[level]];
		UnlockReleaseBuffer(buf);
	}
}

/*
 * Extend 'reln' by the blocks [from, to), all on one leaf, 'leaf', the
 * relation's own (tree_leaf_for()), with 'content' written to block to - 1
 * if given.  The header is locked exclusively, and the relation is 'from'
 * blocks long.
 */
static void
extend_piece(SMgrRelation reln, Buffer headerbuf, const MapPage *leaf,
			 BlockNumber from, BlockNumber to, const void *content,
			 bool skipFsync, PgStat_ProllyCounter counter)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	uint32		n = to - from;
	Buffer		mapbuf;
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
	const MapPage *mp = leaf;

	Assert(n > 0 && n <= BMAP_PIECE);
	Assert(to - 1 - mp->first < mp->nslots);

	/*
	 * The leaf is read now and locked only after the allocator's pages have
	 * been read: a read may write out one of this relation's pages, which
	 * looks it up on this leaf.  Its entries in [from, to) can't change
	 * meanwhile, since the header serializes the relation's extensions.
	 */
	mapbuf = mp_read(reln, mp);
	LockBuffer(mapbuf, BUFFER_LOCK_SHARE);
	mp_check(reln, mp, BufferGetPage(mapbuf));

	/* which blocks need a physical block: the map may hold stale ones */
	for (uint32 i = 0; i < n; i++)
	{
		phys[i] = PageGetBmapEntries(BufferGetPage(mapbuf))[MP_SLOT(mp, from + i)].block;
		need[i] = !BlockNumberIsValid(phys[i]);
		if (need[i])
			nneed++;
		else
			reused[nreused++] = phys[i];
	}
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

	/* every read is done; now the leaf can be locked for the update */
	map_lock(mapbuf, false);

	/* the page store before the record */
	prepare_store(rel, old_hw, new_hw, reused, nreused, false, counter,
				  InvalidXLogRecPtr);
	if (content)
	{
		const void *page = content;

		mdwritev(store_smgr(rel), MAIN_FORKNUM, phys[n - 1], &page, 1,
				 skipFsync);
		store_written(rel, phys[n - 1], 1);
		pgstat_count_prolly(counter, BLCKSZ);
	}

	/* deltas */
	hdelta.kind = BMAP_DELTA_HEADER;
	hdelta.flags = 0;
	hdelta.nblocks = to;
	hdelta.epoch = epoch;
	hdelta.fold_epoch = PageGetBmapHeader(BufferGetPage(headerbuf))->fold_epoch;
	hdelta.shared_below = 0;
	ndelta = palloc0(sizeof(xl_bmap_node_delta) + n * sizeof(xl_bmap_entry_delta));
	ndelta->kind = BMAP_DELTA_NODE;
	for (uint32 i = 0; i < n; i++)
	{
		xl_bmap_entry_delta *ent = (xl_bmap_entry_delta *) (ndelta + 1);

		if (!need[i])
			continue;
		ent[ndelta->nentries].slot = MP_SLOT(mp, from + i);
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
	XLogRegisterBuffer(1, mapbuf, REGBUF_STANDARD);
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
 * Lengths in recovery.  The map's header recovery reads may be newer than
 * the record being replayed (written before a crash, or copied by a base
 * backup), and so shorter, if a truncation came between: a record from
 * before the truncation may restore or initialize a page past the length
 * the header has.  md.c's file, truncated before the crash, is extended
 * then, and the truncation's replay cuts it again.  So is a map's length,
 * though not in the header, whose record the truncation's would then find
 * already replayed: the startup process keeps it until the truncation's
 * replay (bmaptruncate()).  Redo never allocates; the blocks keep the
 * entries the truncation left them, or none, one let go of since
 * (unshare_stale(), or once a truncation is done): records before the
 * truncation are skipped for such a block (bmapreplacedafter()), and pages
 * made for it go nowhere (bmapwritev()).
 *
 * The truncation's replay runs in a critical section, so it only finds,
 * changes and removes entries, which allocates nothing: it removes the
 * relation's once the length it cuts to is no longer past the header's.
 * Unlinking the relation in redo, its XLOG_BMAP_CREATE and its database's
 * drop from its tablespace (bmap_drop_database_recovery()) forget it too.
 */
typedef struct RecoveryLength
{
	RelFileLocator rel;
	BlockNumber nblocks;		/* the length kept */
	BlockNumber header;			/* the header's length then */
} RecoveryLength;

static HTAB *recovery_lengths = NULL;

/* the length recovery keeps for 'rel' past its header's, or 0 */
static BlockNumber
recovery_length(RelFileLocator rel)
{
	RecoveryLength *rl;

	if (recovery_lengths == NULL)
		return 0;
	rl = hash_search(recovery_lengths, &rel, HASH_FIND, NULL);
	return rl ? rl->nblocks : 0;
}

/* keep 'rel' at least 'nblocks' long, past its header's length 'header' */
static void
keep_recovery_length(RelFileLocator rel, BlockNumber nblocks,
					 BlockNumber header)
{
	RecoveryLength *rl;
	bool		found;

	if (recovery_lengths == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(RelFileLocator);
		ctl.entrysize = sizeof(RecoveryLength);
		recovery_lengths = hash_create("block map lengths in recovery", 16,
									   &ctl, HASH_ELEM | HASH_BLOBS);
	}
	rl = hash_search(recovery_lengths, &rel, HASH_ENTER, &found);
	rl->nblocks = Max(found ? rl->nblocks : 0, nblocks);
	rl->header = header;
}

/* a truncation's replay cuts the length kept (in a critical section) */
static void
cut_recovery_length(RelFileLocator rel, BlockNumber nblocks)
{
	RecoveryLength *rl;

	if (recovery_lengths == NULL)
		return;
	rl = hash_search(recovery_lengths, &rel, HASH_FIND, NULL);
	if (rl == NULL)
		return;
	if (nblocks <= rl->header)
		(void) hash_search(recovery_lengths, &rel, HASH_REMOVE, NULL);
	else if (nblocks < rl->nblocks)
		rl->nblocks = nblocks;
}

static void
forget_recovery_length(RelFileLocator rel)
{
	if (recovery_lengths != NULL)
		(void) hash_search(recovery_lengths, &rel, HASH_REMOVE, NULL);
}

/*
 * In redo of a database's drop from tablespace 'spcid' (all of it, or the
 * old tablespace of a move), forget the lengths kept for its relations
 * there: their files are removed without unlinking each, and a relation
 * file number may be used again later in the replay.
 */
void
bmap_drop_database_recovery(Oid dbid, Oid spcid)
{
	HASH_SEQ_STATUS status;
	RecoveryLength *rl;

	if (recovery_lengths == NULL)
		return;
	hash_seq_init(&status, recovery_lengths);
	while ((rl = hash_seq_search(&status)) != NULL)
	{
		if (rl->rel.dbOid == dbid && rl->rel.spcOid == spcid)
			(void) hash_search(recovery_lengths, &rl->rel, HASH_REMOVE, NULL);
	}
}

/*
 * In recovery, extend 'reln' to 'to' blocks past its header's length (see
 * above), writing 'content' to block to - 1 if given.  A header no newer
 * than the record being replayed has every block the record can have.
 */
static void
extend_in_recovery(SMgrRelation reln, BlockNumber to, const void *content,
				   bool skipFsync)
{
	XLogRecPtr	lsn;
	BlockNumber nblocks = header_nblocks(reln, false, &lsn);

	if (nblocks < to)
	{
		if (lsn <= GetCurrentReplayRecPtr(NULL))
			elog(ERROR, "block-mapped relation %s extended during recovery without its allocation",
				 relpath(reln->smgr_rlocator, MAIN_FORKNUM).str);
		keep_recovery_length(reln->smgr_rlocator.locator, to, nblocks);
	}
	if (content)
		bmapwritev(reln, MAIN_FORKNUM, to - 1, &content, 1, skipFsync);
}

/*
 * Extend 'reln' to 'to' blocks, writing 'content' to block to - 1 if given;
 * the page-store bytes count in 'counter' (prepare_store).
 */
static void
extend_to(SMgrRelation reln, BlockNumber to, const void *content,
		  bool skipFsync, PgStat_ProllyCounter counter)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		headerbuf;

	if (InRecovery)
	{
		extend_in_recovery(reln, to, content, skipFsync);
		return;
	}

	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);

	for (;;)
	{
		BlockNumber nblocks = PageGetBmapHeader(BufferGetPage(headerbuf))->nblocks;
		BlockNumber end;
		MapPage		leaf;

		if (nblocks >= to)
		{
			/* long enough; just write the content */
			if (content)
			{
				const void *page = content;
				BlockNumber phys = lookup_ext(reln, to - 1, 1, NULL, false, true);

				mdwritev(store_smgr(rel), MAIN_FORKNUM, phys, &page, 1,
						 skipFsync);
				store_written(rel, phys, 1);
				pgstat_count_prolly(counter, BLCKSZ);
			}
			break;
		}

		/* the leaf of the blocks, the relation's own */
		(void) tree_leaf_for(reln, headerbuf, nblocks, true, true, &leaf);
		end = Min(to, nblocks + BMAP_PIECE);
		end = Min(end, leaf.first + leaf.nslots);
		/* a map never shared has no entry born before its epoch */
		if (PageGetBmapHeader(BufferGetPage(headerbuf))->epoch > 0)
		{
			unshare_stale(reln, &leaf,
						  PageGetBmapHeader(BufferGetPage(headerbuf))->epoch,
						  PageGetBmapHeader(BufferGetPage(headerbuf))->fold_epoch,
						  nblocks, end, false);
			INJECTION_POINT("bmap-unshared", NULL);
		}
		extend_piece(reln, headerbuf, &leaf, nblocks, end,
					 end == to ? content : NULL, skipFsync, counter);
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
		pgstat_count_prolly(PGSTAT_PROLLY_BMAP_BLOCKS_FREED, freed);
	}

	/* (what a scan found in them no longer holds) */
	for (i = 0; i < n; i++)
		store_written(rel, blocks[i], 1);
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

/* set a block's count when the record is made, counted as 'counter' */
static void
refs_set_counted(RefsPages *rp, BlockNumber phys, uint8 count,
				 PgStat_ProllyCounter counter)
{
	xl_bmap_refs_delta *delta = rp->delta[refs_find(rp, phys)];
	xl_bmap_ref_delta *ref = (xl_bmap_ref_delta *) (delta + 1);

	ref[delta->nrefs].slot = REF_SLOT(phys);
	ref[delta->nrefs].count = count;
	delta->nrefs++;
	pgstat_count_prolly(counter, 1);
}

static void
refs_set(RefsPages *rp, BlockNumber phys, uint8 count)
{
	refs_set_counted(rp, phys, count, PGSTAT_PROLLY_BMAP_REFCOUNT_UPDATES);
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

/*
 * One holder more of each of the blocks phys[0 .. n), on the pages rp
 * locked; a stuck count stays, and says so: a sweep counting holders
 * meanwhile can't rewrite stuck counts from what it counted.
 */
static void
refs_add_holders(RefsPages *rp, const BlockNumber *phys, uint32 n)
{
	for (uint32 k = 0; k < n; k++)
	{
		uint8		c = refs_get(rp, phys[k]);

		if (c != BMAP_REFS_STUCK)
			refs_set(rp, phys[k], c + 1);
		else
			pg_atomic_fetch_add_u64(&bmap_shared->stuck_held, 1);
	}
}

/* how many reference-count pages the sorted blocks phys[0 .. n) are on */
static uint32
refs_pages_of(const BlockNumber *phys, uint32 n)
{
	uint32		pages = 0;

	for (uint32 k = 0; k < n; k++)
		if (k == 0 || REF_PAGE(phys[k]) != REF_PAGE(phys[k - 1]))
			pages++;
	return pages;
}

/*
 * Copy a tree's shared node, which 'ptr' in 'parent' (the relation's own)
 * points to, of 'level', mapping from 'first', into a new node that is the
 * relation's own, and point 'parent' to it; the new pointer, set in
 * 'epoch', is returned.  The copy holds each of the node's children, which
 * gain a holder; the node loses the parent's.  The header is held
 * exclusively, so the relation's tree changes no other way meanwhile, and a
 * sweep reads it before the copy or after (sweep_relation()).
 *
 * The children's counts go first, in records of as many count pages as one
 * holds (HOLD), and the last of them with the copy itself (COPY): the
 * copy's allocation, its image, the parent's pointer, the node's count.  A
 * crash before the COPY leaves children counted once too often, held by
 * nothing more: they are freed a sweep later than they would be, and never
 * too early.  Locks: the count pages, the allocator's, the copy's new
 * buffer, then the parent last (map_lock()).
 */
static BmapEntry
tree_copy_node(SMgrRelation reln, const TreeParent *parent, uint32 epoch,
			   uint16 level, BlockNumber first, const BmapEntry *ptr)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	BmapEntry  *entries = palloc(BMAP_TREE_FANOUT * sizeof(BmapEntry));
	BlockNumber *children = palloc((BMAP_TREE_FANOUT + 1) * sizeof(BlockNumber));
	uint32		n = 0;
	uint32		i = 0;
	Buffer		buf;
	Buffer		parentbuf;
	Buffer		nodebuf;
	BlockNumber node;
	Allocation	alloc;
	RefsPages	rp;
	xl_bmap_tree xlrec;
	struct
	{
		xl_bmap_node_delta d;
		xl_bmap_entry_delta e;
	}			pdelta;
	BmapEntry	copy;
	Page		page;
	XLogRecPtr	lsn;
	int			id;
	uint8		c;
	uint32		hold_pages;

	/* the node, as it stays while shared */
	buf = tree_read(rel, ptr->block, RBM_NORMAL);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	if (!tree_node_is(BufferGetPage(buf), level, first, ptr->since))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("block %u of the page store of \"%s\" is not the node of its block map it was",
						ptr->block, relpath(reln->smgr_rlocator, MAIN_FORKNUM).str)));
	memcpy(entries, PageGetBmapEntries(BufferGetPage(buf)),
		   BMAP_TREE_FANOUT * sizeof(BmapEntry));
	UnlockReleaseBuffer(buf);
	for (uint32 k = 0; k < BMAP_TREE_FANOUT; k++)
		if (BlockNumberIsValid(entries[k].block))
			children[n++] = entries[k].block;
	qsort(children, n, sizeof(BlockNumber), cmp_block);
	/* (a leaf may map one block twice, a child is pointed to once) */
	{
		uint32		u = 0;

		for (uint32 k = 0; k < n; k++)
			if (u == 0 || children[u - 1] != children[k])
				children[u++] = children[k];
		n = u;
	}

	xlrec.locator = rel;
	xlrec.flags = 0;
	xlrec.node = InvalidBlockNumber;
	xlrec.from = ptr->block;
	xlrec.old_hw = xlrec.new_hw = InvalidBlockNumber;

	/*
	 * The children's counts, all but the last pages'.  (A test makes every
	 * child's count go first, which a node of children close together, as
	 * a test's are, otherwise never needs.)
	 */
	hold_pages = IS_INJECTION_POINT_ATTACHED("bmap-tree-hold-first") ?
		1 : BMAP_REFS_PER_RECORD;
	while (i < n && refs_pages_of(children + i, n - i) >= hold_pages)
	{
		uint32		covered = refs_lock(rel, children + i, n - i, &rp);

		refs_add_holders(&rp, children + i, covered);
		xlrec.op = BMAP_TREE_HOLD;
		log_refs_record(XLOG_BMAP_TREE, &xlrec, sizeof(xlrec), InvalidBuffer,
						NULL, 0, &rp);
		refs_release(&rp);
		i += covered;
		INJECTION_POINT("bmap-tree-held", NULL);
	}

	/* the last of them, and the node's own, with the copy */
	parentbuf = parent_read(reln, parent);
	{
		uint32		rest = n - i;
		uint32		k = 0;
		uint32		j = 0;
		BlockNumber *all = palloc((rest + 1) * sizeof(BlockNumber));
		bool		placed = false;

		/* (the node in order among them, for the count pages' order) */
		while (j < rest || !placed)
		{
			if (!placed && (j == rest || ptr->block < children[i + j]))
			{
				all[k++] = ptr->block;
				placed = true;
			}
			else
				all[k++] = children[i + j++];
		}
		if (refs_lock(rel, all, rest + 1, &rp) != rest + 1)
			elog(ERROR, "children of a node of \"%s\" span too many count pages",
				 relpath(reln->smgr_rlocator, MAIN_FORKNUM).str);
		pfree(all);
	}
	refs_add_holders(&rp, children + i, n - i);
	c = refs_get(&rp, ptr->block);
	if (c == 0)
		elog(ERROR, "node %u of the page store of \"%s\" is copied but held once",
			 ptr->block, relpath(reln->smgr_rlocator, MAIN_FORKNUM).str);
	if (c != BMAP_REFS_STUCK)
		refs_set(&rp, ptr->block, c - 1);
	else
		stuck_released(rel);

	alloc_take(rel, 1, &node, &alloc);
	prepare_node_store(rel, node, alloc.old_hw, alloc.new_hw);
	nodebuf = tree_read(rel, node, RBM_ZERO_AND_LOCK);
	map_locked_as_new(nodebuf);
	map_lock(parentbuf, false);
	parent_check(reln, parent, BufferGetPage(parentbuf));
	if (PageGetBmapEntries(BufferGetPage(parentbuf))[parent->slot].block != ptr->block)
		elog(ERROR, "the pointer to node %u of \"%s\" changed while it was copied",
			 ptr->block, relpath(reln->smgr_rlocator, MAIN_FORKNUM).str);

	xlrec.op = BMAP_TREE_COPY;
	xlrec.flags = BMAP_TREE_INSTALL;
	xlrec.node = node;
	xlrec.old_hw = alloc.old_hw;
	xlrec.new_hw = alloc.new_hw;

	XLogEnsureRecordSpace(2 + BMAP_ALLOC_MAX_PAGES + BMAP_REFS_PER_RECORD,
						  3 + BMAP_ALLOC_MAX_PAGES + BMAP_REFS_PER_RECORD);
	copy.block = node;
	copy.epoch = epoch;
	copy.since = GetXLogInsertRecPtr();
	pdelta.d.kind = BMAP_DELTA_NODE;
	pdelta.d.flags = 0;
	pdelta.d.nentries = 1;
	pdelta.d.since = copy.since;
	pdelta.e.slot = parent->slot;
	pdelta.e.block = node;
	pdelta.e.epoch = epoch;

	START_CRIT_SECTION();
	page = BufferGetPage(nodebuf);
	init_tree_node(page, level, first, copy.since);
	memcpy(PageGetBmapEntries(page), entries,
		   BMAP_TREE_FANOUT * sizeof(BmapEntry));
	MarkBufferDirty(nodebuf);
	apply_node_delta(BufferGetPage(parentbuf), &pdelta.d);
	MarkBufferDirty(parentbuf);
	XLogBeginInsert();
	XLogRegisterData(&xlrec, sizeof(xlrec));
	XLogRegisterBuffer(0, parentbuf, REGBUF_STANDARD);
	XLogRegisterBufData(0, &pdelta, sizeof(pdelta));
	XLogRegisterBuffer(1, nodebuf, REGBUF_FORCE_IMAGE);
	id = alloc_apply_register(&alloc, 2);
	refs_apply_register(&rp, id);
	lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_TREE);
	PageSetLSN(page, lsn);
	PageSetLSN(BufferGetPage(parentbuf), lsn);
	alloc_set_lsn(&alloc, lsn);
	refs_set_lsn(&rp, lsn);
	END_CRIT_SECTION();

	refs_release(&rp);
	alloc_release(&alloc);
	map_unlock(parentbuf);
	ReleaseBuffer(parentbuf);
	map_unlock(nodebuf);
	ReleaseBuffer(nodebuf);
	pgstat_count_prolly(PGSTAT_PROLLY_BMAP_MAP_PAGES_COPIED, 1);
	pfree(entries);
	pfree(children);
	INJECTION_POINT("bmap-tree-copied", NULL);
	return copy;
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
 * Before an extension reuses the entries of blocks [from, to), on one leaf
 * ('leaf', the relation's own), that a truncation left past the relation's
 * end: those born before
 * the map's epoch may be shared.  One no other map holds is claimed (born
 * again in this epoch); one another map holds is let go (its count drops)
 * and cleared, to be allocated anew.  With 'release', once a truncation is
 * done (bmapposttruncate()), every entry is let go of, and a block no other
 * map holds is freed.  The header is locked exclusively, so those entries
 * can't change meanwhile; the leaf is locked only after each record's
 * other pages, since reading them may write out one of this relation's
 * pages, which looks it up on the leaf.
 */
static void
unshare_stale(SMgrRelation reln, const MapPage *leaf, uint32 epoch,
			  uint32 fold_epoch, BlockNumber from, BlockNumber to,
			  bool release)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	const MapPage *mp = leaf;
	Buffer		mapbuf;
	PGIOAlignedBlock copy;
	BmapEntry	stale[BMAP_PIECE];
	SlotEntry	items[BMAP_PIECE];
	BlockNumber phys[BMAP_PIECE];
	BlockNumber tofree[BMAP_PIECE];
	uint32		nitems = 0;
	uint32		nfree = 0;
	BlockNumber nrefpages;
	uint32		i = 0;

	Assert(to - from <= BMAP_PIECE);
	mapbuf = mp_read(reln, mp);
	LockBuffer(mapbuf, BUFFER_LOCK_SHARE);
	mp_check(reln, mp, BufferGetPage(mapbuf));
	memcpy(copy.data, BufferGetPage(mapbuf), BLCKSZ);
	LockBuffer(mapbuf, BUFFER_LOCK_UNLOCK);
	Assert(to - 1 - mp->first < mp->nslots);
	memcpy(stale, &PageGetBmapEntries(copy.data)[MP_SLOT(mp, from)],
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
			list_stale(reln, mp, mapbuf, blk, e->block, epoch);
		else if (e->epoch < epoch || release)
		{
			items[nitems].slot = MP_SLOT(mp, blk);
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
			if (c == 0 && !release)
			{
				/* no other map holds it: it is ours */
				ent[ndelta->nentries].block = items[k].block;
				ent[ndelta->nentries].epoch = epoch;
			}
			else
			{
				if (c == 0)
					tofree[nfree++] = phys[k];
				else if (c != BMAP_REFS_STUCK)
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

	/*
	 * (sorted, as phys[]; a crash before this leaves them to a sweep)  A
	 * released block no other map holds is held by nothing until it is
	 * freed, and free_blocks()'s reads may wait; but the caller's lock of
	 * the header holds off interrupts, so a cancelled truncation (an
	 * autovacuum's, waited on by a query) frees them first.
	 */
	if (nfree > 0)
	{
		INJECTION_POINT("bmap-truncation-let-go", NULL);
		free_blocks(rel, tofree, nfree);
	}
}

/* a main fork's logical blocks can be given other physical blocks */
bool
bmapreplacesblocks(SMgrRelation reln, ForkNumber forknum)
{
	return forknum == MAIN_FORKNUM;
}

/* ----------------------------------------------------------------
 * Recovery: remaps ahead of replay
 * ----------------------------------------------------------------
 */

/*
 * Redo skips a block's records from before the WAL position since which
 * its entry has its physical block (bmapreplacedafter()), and doesn't write
 * pages made by them (page_predates_entry()), when the map page read is
 * newer than the record: the record that gave the block its physical block
 * carries the page's image, and every record from before it is of another
 * block.  That holds only if every record up to then is skipped.  The map
 * page is read as it stands at each point of the replay, and a full-page
 * image of it, always restored, can be older than the version it replaces:
 * it gives the entry the position it had at the image, and the block's
 * records from between then and the newer position would be applied to a
 * page missing the ones skipped before them, or written to a physical block
 * that may still hold another relation's page.
 *
 * So before an image of a map page is restored (bmapprerestore()), each
 * entry of the version it replaces whose position is ahead of the replay is
 * remembered here, in shared memory, since the checkpointer and the
 * background writer write pages out during recovery too.  Restoring an
 * image reads nothing of the page it replaces, so the version replaced is
 * read first: from its buffer, or from disk, where a version newer than the
 * replay can be.  Both answers take the later of the map page's position
 * and the one remembered, so neither goes backwards.  An entry is let go of
 * once the replay passes its position: by then the record at it has given
 * the map page that position or a later one.  A dropped relation's entries
 * are let go of that way too; until then they skip records of a relation
 * that is to be dropped, or of a later one with the same file number, whose
 * map page on disk gave the position.
 *
 * The table holds block_map_replay_entries entries.  If more are needed at
 * once, there's no safe answer for a block that didn't fit: taking its
 * record to be of another block skips a change its page needs, and taking
 * it to be of the block applies the record to a page missing the ones
 * skipped before it (or writes the page over another relation's).  So
 * recovery stops before restoring the image, with nothing wrong written,
 * and can be run again with a larger table.
 */

/*
 * The latest position since which the logical block whose entry is 'slot'
 * of leaf 'leaf' (a block of the page store 'store') has its physical
 * block, as a version of the leaf that an older image replaced in recovery
 * said: InvalidXLogRecPtr if none is remembered.
 */
static XLogRecPtr
remap_ahead(RelFileLocator store, BlockNumber leaf, uint32 slot)
{
	BmapAheadKey key;
	BmapAhead  *a;
	XLogRecPtr	since = InvalidXLogRecPtr;

	/*
	 * An entry is remembered before the image is restored under the map
	 * page's buffer lock, and read after a lookup of the map page, which
	 * takes that lock or reads the version on disk: a lookup that finds the
	 * image finds the entry.
	 */
	if (pg_atomic_read_u32(&bmap_shared->nahead) == 0)
		return InvalidXLogRecPtr;
	memset(&key, 0, sizeof(key));
	key.rel = store;
	key.blkno = leaf;
	key.slot = slot;
	LWLockAcquire(&bmap_shared->ahead_lock, LW_SHARED);
	a = hash_search(bmap_ahead, &key, HASH_FIND, NULL);
	if (a != NULL)
		since = a->since;
	LWLockRelease(&bmap_shared->ahead_lock);
	return since;
}

/* let go of the remaps the replay, at 'lsn', has passed; the lock held */
static void
forget_passed(XLogRecPtr lsn)
{
	HASH_SEQ_STATUS status;
	BmapAhead  *a;

	hash_seq_init(&status, bmap_ahead);
	while ((a = hash_seq_search(&status)) != NULL)
	{
		if (a->since < lsn)
		{
			(void) hash_search(bmap_ahead, &a->key, HASH_REMOVE, NULL);
			pg_atomic_fetch_sub_u32(&bmap_shared->nahead, 1);
		}
	}
}

/*
 * Remember the entries of leaf 'leaf' (a block of the page store 'store'),
 * in a version that an image from the record at 'lsn' is about to replace,
 * whose positions are ahead of the record.
 */
static void
remember_ahead(RelFileLocator store, BlockNumber leaf,
			   const BmapEntry *entries, XLogRecPtr lsn)
{
	bool		passed = false;

	LWLockAcquire(&bmap_shared->ahead_lock, LW_EXCLUSIVE);
	for (uint32 slot = 0; slot < BMAP_TREE_FANOUT; slot++)
	{
		BmapAheadKey key;
		BmapAhead  *a;
		bool		found;

		if (!BlockNumberIsValid(entries[slot].block) ||
			entries[slot].since <= lsn)
			continue;
		memset(&key, 0, sizeof(key));
		key.rel = store;
		key.blkno = leaf;
		key.slot = slot;
		a = hash_search(bmap_ahead, &key, HASH_ENTER_NULL, &found);
		if (a == NULL && !passed)
		{
			forget_passed(lsn);
			passed = true;
			a = hash_search(bmap_ahead, &key, HASH_ENTER_NULL, &found);
		}
		if (a == NULL)
		{
			LWLockRelease(&bmap_shared->ahead_lock);
			ereport(FATAL,
					(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
					 errmsg("recovery needs to remember more remapped blocks of block-mapped relations than \"block_map_replay_entries\" (%d) holds",
							block_map_replay_entries),
					 errdetail("A full-page image of block map leaf %u of \"%s\" is older than the version it replaces.",
							   leaf, relpathperm(store, MAIN_FORKNUM).str),
					 errhint("Increase \"block_map_replay_entries\" and restart the server.")));
		}
		if (!found)
		{
			a->since = entries[slot].since;
			pg_atomic_fetch_add_u32(&bmap_shared->nahead, 1);
		}
		else
			a->since = Max(a->since, entries[slot].since);
	}
	LWLockRelease(&bmap_shared->ahead_lock);
}

/*
 * In recovery, before a full-page image from the record at 'lsn' of block
 * 'blocknum' of the map fork is restored.  The map fork holds the header and
 * the root page, whose pointers redo_block() keeps from a newer version;
 * the leaves, whose entries are remembered (remember_ahead()), are the page
 * store's, and redo_block() remembers them before restoring an image.
 */
void
bmapprerestore(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			   XLogRecPtr lsn)
{
}

/*
 * In recovery: was logical block 'blocknum' given its physical block after
 * WAL position 'lsn'?  The map page read may be newer than the record being
 * replayed (written before a crash, or copied by a base backup), or not yet
 * initialized, or older than a version an image of it replaced earlier in
 * the replay, whose position counts too (see remember_ahead()).
 */
bool
bmapreplacedafter(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
				  XLogRecPtr lsn)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	BmapEntry	entry;
	MapPage		mp;
	bool		unavailable;

	if (lookup_entry_at(reln, blocknum, &entry, &mp, &unavailable))
	{
		if (remap_ahead(with_relnumber(rel, BMAP_STORE_RELNUMBER), mp.blk,
						MP_SLOT(&mp, blocknum)) > lsn)
			return true;
		return entry.since > lsn;
	}

	/*
	 * Its subtree replaced or let go of since (see tree_find()): the block
	 * on disk has every change the records before then made, written out and
	 * synced before its subtree came to be shared.  Its buffer, though, may
	 * hold an image an earlier record restored, to which this record (and
	 * those like it) would never be applied: it is dropped, so that the
	 * page is read from the block again once the subtree is there.
	 */
	if (unavailable)
	{
		DropBufferBlock(rel, forknum, blocknum);
		return true;
	}

	/* (its entry let go of after the record, whatever the header says) */
	if (InRecovery && let_go_after(reln, blocknum, false, lsn))
		return true;

	/*
	 * A block recovery keeps past its header's length whose entry was let
	 * go of after the truncation the header has (extend_in_recovery()): its
	 * pages go nowhere (bmapwritev()), so one read back would find no
	 * block.  The truncation's replay drops it, as a block given other
	 * storage is skipped until the change (this record is from before
	 * both).
	 */
	return InRecovery &&
		blocknum < recovery_length(reln->smgr_rlocator.locator) &&
		blocknum >= header_nblocks(reln, false, NULL);
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
 * Move logical block 'blkno''s entry, block 'old_block' on map page 'mp'
 * (pinned in 'mapbuf'), to reln's changes list: a copy of the page, which
 * the list holds as it was.  The map's entry is left empty.  The map page
 * is locked after the list's pages.
 */
static void
list_stale(SMgrRelation reln, const MapPage *mp, Buffer mapbuf,
		   BlockNumber blkno, BlockNumber old_block, uint32 epoch)
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
	ent->slot = MP_SLOT(mp, blkno);
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
	hdelta.shared_below = 0;

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
 * Forget the relation's folds: the changes listed let go of (their blocks
 * freed), and no page copied before a change any more, as if it had never
 * been folded.  For a relation whose folds no one reads any more (its
 * change finder now keeps no pages): each first change since its last
 * fold would otherwise copy its page.  A crash between the two leaves the
 * list empty and pages still copied, until the next call.
 */
void
bmap_fold_forget(SMgrRelation reln)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		headerbuf;
	Buffer		chgbuf;
	BmapHeader *header;
	BmapChangesHead *head;
	BmapFold	all;
	xl_bmap_header_delta hdelta;
	xl_bmap_changes_head_delta cdelta;
	XLogRecPtr	lsn;

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("cannot forget a relation's folds during recovery")));
	if (!bmap_folded(reln) || !chg_exists(reln) ||
		mdnblocks(reln, BMAP_CHANGES_FORKNUM) == 0)
		return;

	/* every change listed, made before any fold to come */
	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_SHARE);
	all.epoch = PageGetBmapHeader(BufferGetPage(headerbuf))->epoch + 1;
	UnlockReleaseBuffer(headerbuf);
	chgbuf = chg_read(rel, 0);
	LockBuffer(chgbuf, BUFFER_LOCK_SHARE);
	if (PageIsNew(BufferGetPage(chgbuf)))
	{
		UnlockReleaseBuffer(chgbuf);
		return;
	}
	all.lineage = PageGetBmapChangesHead(BufferGetPage(chgbuf))->lineage;
	UnlockReleaseBuffer(chgbuf);
	all.nblocks = 0;
	bmap_fold_prune(reln, &all);

	/* then no fold: the header's epoch and the list's, none */
	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	header = PageGetBmapHeader(BufferGetPage(headerbuf));
	chgbuf = chg_read(rel, 0);
	LockBuffer(chgbuf, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(chgbuf), BMAP_PAGE_CHANGES_HEAD, rel,
			   BMAP_CHANGES_FORKNUM, 0);
	head = PageGetBmapChangesHead(BufferGetPage(chgbuf));

	hdelta.kind = BMAP_DELTA_HEADER;
	hdelta.flags = 0;
	hdelta.nblocks = header->nblocks;
	hdelta.epoch = header->epoch;
	hdelta.fold_epoch = 0;
	hdelta.shared_below = 0;

	cdelta.kind = BMAP_DELTA_CHANGES_HEAD;
	cdelta.flags = 0;
	cdelta.nentries = head->nentries;
	cdelta.lineage = head->lineage;
	cdelta.pruned_epoch = Max(head->pruned_epoch, all.epoch);
	cdelta.fold_epoch = 0;
	cdelta.fold_nblocks = head->fold_nblocks;

	START_CRIT_SECTION();
	apply_header_delta(BufferGetPage(headerbuf), &hdelta);
	MarkBufferDirty(headerbuf);
	apply_changes_head_delta(BufferGetPage(chgbuf), &cdelta);
	MarkBufferDirty(chgbuf);
	XLogBeginInsert();
	XLogRegisterData(&rel, sizeof(xl_bmap_rel));
	XLogRegisterBuffer(0, headerbuf, REGBUF_STANDARD);
	XLogRegisterBufData(0, &hdelta, sizeof(hdelta));
	XLogRegisterBuffer(1, chgbuf, REGBUF_STANDARD);
	XLogRegisterBufData(1, &cdelta, sizeof(cdelta));
	lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_FOLD);
	PageSetLSN(BufferGetPage(headerbuf), lsn);
	PageSetLSN(BufferGetPage(chgbuf), lsn);
	END_CRIT_SECTION();

	UnlockReleaseBuffer(chgbuf);
	UnlockReleaseBuffer(headerbuf);
}

/*
 * Whether a fold of the relation has begun: its changes since are listed
 * from then on.
 */
bool
bmap_folded(SMgrRelation reln)
{
	return changes_fold_epoch(reln) > 0;
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
	/* (verified in the aligned copy: 'page' needn't be aligned) */
	if (!PageIsVerified((Page) copy.data, blkno, PIV_LOG_WARNING, NULL))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid page in block %u of the page store of \"%s\", kept for block %u",
						old_block, relpathperm(reln->smgr_rlocator.locator, MAIN_FORKNUM).str,
						blkno)));
	memcpy(page, copy.data, BLCKSZ);
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
 * Before a private block is shared, log its contents with its physical
 * address.  A base backup may have read the page store before our flush,
 * and the source's map after a later remap.  Redo then skips the source's
 * older changes, so flushing alone cannot preserve the copy's rows.
 *
 * At replica/logical WAL levels, preserve even when no backup is active:
 * a backup could start between such a check and the share's flush.  Skipping
 * images safely would require an interlock with backup startup.
 *
 * Blocks already held by another map or changes list have been preserved
 * by their first share or REMAP, and cannot change in place.  Do not read
 * or log them again.  The caller holds the source's header exclusively
 * and keeps its writers out, but has not locked the destination map page.
 */
static void
preserve_shared_page(SMgrRelation src, BlockNumber blkno, BlockNumber phys)
{
	RelFileLocator rel = src->smgr_rlocator.locator;
	PGIOAlignedBlock copy;
	Buffer		buf;

	/* Without online backups or standbys, the share's flush is sufficient. */
	if (!XLogIsNeeded() || lookup_ref(rel, phys) > 0)
		return;

	buf = ReadBufferWithoutRelcache(rel, MAIN_FORKNUM, blkno,
								   RBM_NORMAL, NULL, true);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	memcpy(copy.data, BufferGetPage(buf), BLCKSZ);
	UnlockReleaseBuffer(buf);

	XLogBeginInsert();
	XLogRegisterData(&phys, sizeof(phys));
	XLogRegisterBlock(0, &rel, MAIN_FORKNUM, blkno, copy.data,
					 REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
	XLogInsert(RM_BMAP_ID, XLOG_BMAP_PRESERVE);
	pgstat_count_prolly(PGSTAT_PROLLY_BMAP_PAGES_PRESERVED, 1);
}

/*
 * Before a tree is shared, preserve (preserve_shared_page()) the blocks of
 * its own leaves, under its length: those of shared ones were preserved when
 * they came to be shared, and can't change in place since.  So a share that
 * follows another walks only what changed in between.  The header is held
 * exclusively.
 */
static void
tree_preserve_node(SMgrRelation src, const BmapEntry *ptr, uint16 level,
				   BlockNumber first, BlockNumber nblocks, uint32 epoch)
{
	RelFileLocator rel = src->smgr_rlocator.locator;
	BmapEntry  *entries;
	Buffer		buf;

	if (first >= nblocks || !tree_node_own(rel, ptr, epoch, true))
		return;
	entries = palloc(BMAP_TREE_FANOUT * sizeof(BmapEntry));
	buf = tree_read(rel, ptr->block, RBM_NORMAL);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	if (!tree_node_is(BufferGetPage(buf), level, first, ptr->since))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("block %u of the page store of \"%s\" is not the node of its block map it was",
						ptr->block, relpath(src->smgr_rlocator, MAIN_FORKNUM).str)));
	memcpy(entries, PageGetBmapEntries(BufferGetPage(buf)),
		   BMAP_TREE_FANOUT * sizeof(BmapEntry));
	UnlockReleaseBuffer(buf);
	for (uint32 i = 0; i < BMAP_TREE_FANOUT; i++)
	{
		uint64		child = (uint64) first + (level == 0 ? i :
											   i * tree_span(level - 1));

		if (!BlockNumberIsValid(entries[i].block) || child >= nblocks)
			continue;
		if (level == 0)
			preserve_shared_page(src, (BlockNumber) child, entries[i].block);
		else
			tree_preserve_node(src, &entries[i], level - 1,
							   (BlockNumber) child, nblocks, epoch);
	}
	pfree(entries);
}

static void
tree_preserve(SMgrRelation src, BlockNumber nblocks, uint32 epoch)
{
	RelFileLocator rel = src->smgr_rlocator.locator;
	BmapRoot	root;
	Buffer		buf;
	uint64		first = 0;

	/* (preserve_shared_page() logs nothing without online backups) */
	if (!XLogIsNeeded())
		return;
	buf = map_read(rel, 1);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(buf), BMAP_PAGE_ROOT, rel, BMAP_FORKNUM, 1);
	root = *PageGetBmapRoot(BufferGetPage(buf));
	UnlockReleaseBuffer(buf);
	for (int k = 0; k < BMAP_TREE_TOPS && first < nblocks; k++)
	{
		if (BlockNumberIsValid(root.top[k].block))
			tree_preserve_node(src, &root.top[k], k, (BlockNumber) first,
							   nblocks, epoch);
		first += tree_span(k);
	}
}

/*
 * A tree shared: dst's root page (made with no subtrees) gets src's
 * pointers, and each subtree a holder more, in one record.  That is all the
 * map a share copies: the subtrees stay shared until either relation
 * changes something under one, which copies the path to it.  With 'from',
 * the pointers are those of a snapshot of src's, and dst's header gets
 * 'hdelta' in the same record, and src's header (held exclusively by the
 * caller in 'sheader', if valid) 'shdelta'.
 */
static void
tree_share_root(SMgrRelation src, SMgrRelation dst, const BmapEntry *from,
				const xl_bmap_header_delta *hdelta, Buffer sheader,
				const xl_bmap_header_delta *shdelta)
{
	RelFileLocator srel = src->smgr_rlocator.locator;
	RelFileLocator drel = dst->smgr_rlocator.locator;
	BmapRoot	root;
	BlockNumber tops[BMAP_TREE_TOPS];
	uint32		ntops = 0;
	Buffer		buf;
	RefsPages	rp;
	xl_bmap_root_delta rdelta;
	xl_bmap_tree xlrec;
	XLogRecPtr	lsn;
	int			id;
	Buffer		headerbuf = InvalidBuffer;

	buf = map_read(srel, 1);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(buf), BMAP_PAGE_ROOT, srel, BMAP_FORKNUM, 1);
	root = *PageGetBmapRoot(BufferGetPage(buf));
	UnlockReleaseBuffer(buf);
	if (from)
		memcpy(root.top, from, sizeof(root.top));

	memset(&rdelta, 0, sizeof(rdelta));
	rdelta.kind = BMAP_DELTA_ROOT;
	rdelta.flags = BMAP_DELTA_INIT;
	rdelta.ntops = BMAP_TREE_TOPS;
	for (int k = 0; k < BMAP_TREE_TOPS; k++)
	{
		rdelta.tops[k].block = root.top[k].block;
		rdelta.tops[k].epoch = root.top[k].epoch;
		rdelta.tops[k].since = root.top[k].since;
		if (BlockNumberIsValid(root.top[k].block))
			tops[ntops++] = root.top[k].block;
	}
	qsort(tops, ntops, sizeof(BlockNumber), cmp_block);
	rp.n = 0;
	if (ntops > 0 && refs_lock(srel, tops, ntops, &rp) != ntops)
		elog(ERROR, "the subtrees of \"%s\" span too many count pages",
			 relpath(src->smgr_rlocator, MAIN_FORKNUM).str);
	refs_add_holders(&rp, tops, ntops);

	if (hdelta)
	{
		headerbuf = map_read(drel, 0);
		LockBuffer(headerbuf, BUFFER_LOCK_EXCLUSIVE);
		check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, drel,
				   BMAP_FORKNUM, 0);
	}
	buf = map_read(drel, 1);
	map_lock(buf, false);
	check_page(BufferGetPage(buf), BMAP_PAGE_ROOT, drel, BMAP_FORKNUM, 1);

	xlrec.locator = drel;
	xlrec.op = BMAP_TREE_SHARE;
	xlrec.flags = hdelta ? BMAP_TREE_OF_SNAPSHOT : 0;
	xlrec.node = InvalidBlockNumber;
	xlrec.from = srel.relNumber;
	xlrec.old_hw = xlrec.new_hw = InvalidBlockNumber;

	XLogEnsureRecordSpace(3 + BMAP_TREE_TOPS, 4 + BMAP_TREE_TOPS);
	START_CRIT_SECTION();
	apply_root_delta(BufferGetPage(buf), &rdelta);
	MarkBufferDirty(buf);
	if (hdelta)
	{
		apply_header_delta(BufferGetPage(headerbuf), hdelta);
		MarkBufferDirty(headerbuf);
	}
	if (BufferIsValid(sheader))
	{
		apply_header_delta(BufferGetPage(sheader), shdelta);
		MarkBufferDirty(sheader);
	}
	XLogBeginInsert();
	XLogRegisterData(&xlrec, sizeof(xlrec));
	XLogRegisterBuffer(0, buf, REGBUF_WILL_INIT | REGBUF_STANDARD);
	XLogRegisterBufData(0, &rdelta, sizeof(rdelta));
	id = refs_apply_register(&rp, 1);
	if (hdelta)
	{
		XLogRegisterBuffer(id, headerbuf, REGBUF_STANDARD);
		XLogRegisterBufData(id, hdelta, sizeof(*hdelta));
		id++;
	}
	if (BufferIsValid(sheader))
	{
		XLogRegisterBuffer(id, sheader, REGBUF_STANDARD);
		XLogRegisterBufData(id, shdelta, sizeof(*shdelta));
	}
	lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_TREE);
	PageSetLSN(BufferGetPage(buf), lsn);
	if (hdelta)
		PageSetLSN(BufferGetPage(headerbuf), lsn);
	if (BufferIsValid(sheader))
		PageSetLSN(BufferGetPage(sheader), lsn);
	refs_set_lsn(&rp, lsn);
	END_CRIT_SECTION();

	refs_release(&rp);
	map_unlock(buf);
	ReleaseBuffer(buf);
	if (hdelta)
		UnlockReleaseBuffer(headerbuf);
	pgstat_count_prolly(PGSTAT_PROLLY_BMAP_MAP_PAGES_COPIED, 1);
}

/*
 * Make 'dst', a new relation file number in the same tablespace and
 * database, a copy of 'src' that shares its blocks: dst's map holds src's
 * entries, and its changes list src's, each block's count grows by one,
 * and both maps' epochs pass the entries' births.  A tree's copy holds its
 * subtrees instead, each of which gains a holder (tree_share_root()).
 * src's dirty buffers are
 * written, so the blocks hold what both copies start from; the page store is
 * synced by bmapsyncshares(), at the latest when the transaction commits.
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

	create_map(dst);

	/* src's header, held until the epochs are raised: no change meanwhile */
	sheader = map_read(srel, 0);
	LockBuffer(sheader, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(sheader), BMAP_PAGE_HEADER, srel, BMAP_FORKNUM, 0);
	nblocks = PageGetBmapHeader(BufferGetPage(sheader))->nblocks;
	epoch = PageGetBmapHeader(BufferGetPage(sheader))->epoch;

	/* src's own blocks preserved, its subtrees a holder more */
	tree_preserve(src, nblocks, epoch);
	tree_share_root(src, dst, NULL, NULL, InvalidBuffer, NULL);
	INJECTION_POINT("bmap-share-map-page", NULL);

	/* the copy has the same changes since src's last fold */
	share_changes(src, dst);

	dheader = map_read(drel, 0);
	LockBuffer(dheader, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(dheader), BMAP_PAGE_HEADER, drel, BMAP_FORKNUM, 0);

	sdelta.kind = ddelta.kind = BMAP_DELTA_HEADER;
	sdelta.flags = ddelta.flags = BMAP_DELTA_SHARED_BELOW;
	sdelta.nblocks = ddelta.nblocks = nblocks;
	sdelta.epoch = ddelta.epoch = epoch + 1;
	/* (every entry either has is the other's too) */
	sdelta.shared_below = ddelta.shared_below = epoch + 1;
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
	 * The shared blocks get src's latest pages, durably.  PRESERVE records
	 * can reconstruct them even if a base backup has older store pages and
	 * newer maps, which make redo skip the source's earlier changes.  A page
	 * src changes from here on is copied first, its old contents written to
	 * the shared block if they were only in its buffer.  So only the sync
	 * waits for src to be let change again: the blocks hold what dst starts
	 * from either way, and it only has to
	 * be durable by the time dst is, at commit (bmapsyncshares()).
	 */
	FlushRelationsAllBuffers(&src, 1);
	sync_store_later(srel);
}

/*
 * The page stores (tablespace and database) that shares made since the last
 * bmapsyncshares() wrote blocks of, to sync: each once, however many
 * relations of it were shared.
 */
static RelFileLocator *unsynced_stores = NULL;
static int	n_unsynced_stores = 0;
static int	max_unsynced_stores = 0;

static void
sync_store_later(RelFileLocator rel)
{
	for (int i = 0; i < n_unsynced_stores; i++)
		if (unsynced_stores[i].spcOid == rel.spcOid &&
			unsynced_stores[i].dbOid == rel.dbOid)
			return;
	if (n_unsynced_stores == max_unsynced_stores)
	{
		max_unsynced_stores = max_unsynced_stores ? max_unsynced_stores * 2 : 4;
		unsynced_stores = unsynced_stores
			? repalloc_array(unsynced_stores, RelFileLocator, max_unsynced_stores)
			: MemoryContextAlloc(TopMemoryContext,
								 max_unsynced_stores * sizeof(RelFileLocator));
	}
	unsynced_stores[n_unsynced_stores++] = rel;
}

/*
 * Sync the page stores this transaction's shares wrote blocks of (or, at
 * abort, forget them: the copies are dropped, their blocks src's alone).
 */
void
bmapsyncshares(bool isCommit)
{
	if (isCommit)
		while (n_unsynced_stores > 0)
		{
			INJECTION_POINT("bmap-sync-shares", NULL);
			mdimmedsync(store_smgr(unsynced_stores[n_unsynced_stores - 1]),
						MAIN_FORKNUM);
			n_unsynced_stores--;
		}
	n_unsynced_stores = 0;
}

/* ----------------------------------------------------------------
 * Snapshots of a tree
 * ----------------------------------------------------------------
 */

/*
 * The slot of reln's root page that keeps snapshot 'id' (-1: none), and a
 * copy of the root page into *root; the header is held.
 */
static int
snapshot_slot(SMgrRelation reln, uint64 id, BmapRoot *root)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		buf = map_read(rel, 1);

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(buf), BMAP_PAGE_ROOT, rel, BMAP_FORKNUM, 1);
	*root = *PageGetBmapRoot(BufferGetPage(buf));
	UnlockReleaseBuffer(buf);
	for (int s = 0; s < BMAP_TREE_SNAPSHOTS; s++)
		if (root->snap[s].id == id)
			return s;
	return -1;
}

/*
 * What became of the (sub)transaction 'owner' that wrote something into
 * the storage before it committed (a snapshot's slot, a drop's release
 * queue entries): OWNER_NONE if there is none, OWNER_RUNNING while it runs
 * (or is prepared, or is this backend's), OWNER_COMMITTED, OWNER_ENDED if
 * it aborted or a crash ended it, and OWNER_UNKNOWN if it is too old for
 * the commit log to say, which each caller takes the safe way.  Owners are
 * FullTransactionIds, never mistaken for a later transaction of the same
 * 32-bit id.  As for a tuple, the transaction is asked whether it is in
 * progress before the commit log is: a commit sets the log first.  Not in
 * recovery.
 */
typedef enum OwnerState
{
	OWNER_NONE,
	OWNER_RUNNING,
	OWNER_COMMITTED,
	OWNER_ENDED,
	OWNER_UNKNOWN,
} OwnerState;

static OwnerState
owner_state(FullTransactionId owner)
{
	TransactionId xid = XidFromFullTransactionId(owner);
	FullTransactionId now;
	OwnerState	state;

	Assert(!RecoveryInProgress());
	if (!TransactionIdIsNormal(xid))
		return OWNER_NONE;
	if (TransactionIdIsCurrentTransactionId(xid))
		return OWNER_RUNNING;
	now = ReadNextFullTransactionId();
	if (!FullTransactionIdPrecedes(owner, now))
		return OWNER_UNKNOWN;
	LWLockAcquire(XactTruncationLock, LW_SHARED);
	if (FullTransactionIdPrecedes(owner,
								  FullTransactionIdFromAllowableAt(now,
																   TransamVariables->oldestClogXid)))
		state = OWNER_UNKNOWN;
	else if (TransactionIdIsInProgress(xid))
		state = OWNER_RUNNING;
	else if (TransactionIdDidCommit(xid))
		state = OWNER_COMMITTED;
	else
		state = OWNER_ENDED;
	LWLockRelease(XactTruncationLock);
	return state;
}

/*
 * Is snapshot 'snap' kept by nobody, the (sub)transaction that took it
 * having ended without committing (owner_state())?  A snapshot is in the
 * storage, not the transaction: one taken by a transaction that aborts, or
 * that a crash ends, would otherwise hold its subtrees for as long as the
 * relation lasts.  One whose taker is too old for the commit log to say is
 * taken as committed (kept), as is one still running or prepared.  During
 * recovery, none.
 */
static bool
snapshot_abandoned(const xl_bmap_snapshot *snap)
{
	if (snap->id == 0 || RecoveryInProgress())
		return false;
	return owner_state(FullTransactionIdFromU64(snap->owner)) == OWNER_ENDED;
}

/*
 * Let go of the snapshots of reln that nobody keeps (snapshot_abandoned()),
 * as bmap_snapshot_release() does: how many.  Not under any of its pages'
 * locks.  A map not made yet (its header page, or its root page, all
 * zeroes, as a crash part way through making one leaves it) keeps none.
 */
static int
snapshot_reclaim(SMgrRelation reln)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		headerbuf;
	Buffer		rootbuf;
	BmapRoot	root;
	int			n = 0;

	if (RecoveryInProgress())
		return 0;
	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_SHARE);
	if (PageIsNew(BufferGetPage(headerbuf)))
	{
		UnlockReleaseBuffer(headerbuf);
		return 0;
	}
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	rootbuf = map_read(rel, 1);
	LockBuffer(rootbuf, BUFFER_LOCK_SHARE);
	if (PageIsNew(BufferGetPage(rootbuf)))
	{
		UnlockReleaseBuffer(rootbuf);
		UnlockReleaseBuffer(headerbuf);
		return 0;
	}
	UnlockReleaseBuffer(rootbuf);
	(void) snapshot_slot(reln, 0, &root);
	UnlockReleaseBuffer(headerbuf);
	for (int s = 0; s < BMAP_TREE_SNAPSHOTS; s++)
	{
		/* (an abandoned snapshot stays so: its slot is let go of once) */
		if (snapshot_abandoned(&root.snap[s]) &&
			bmap_snapshot_release(reln, root.snap[s].id))
			n++;
	}
	return n;
}

/*
 * Keep a snapshot of reln's pages as they are: a slot of its root page holds
 * the subtrees it has now, and its length and last fold's epoch, each
 * subtree gaining a holder, and the map's epoch is raised, as by a share,
 * so that a page changed from now on is copied first.  The slot names the
 * (sub)transaction taking it: if that doesn't commit, the snapshot is kept
 * by nobody, and the next snapshot taken of reln, or the next sweep of its
 * page store, lets go of it (snapshot_reclaim()).  O(1) in the
 * relation's size, but for the blocks of its own leaves a base backup may
 * need preserved (as a share does) and the page store's sync.  Returns its
 * id, which names it until bmap_snapshot_release().  Only a tree keeps
 * snapshots, at most BMAP_SNAPSHOTS_MAX.
 *
 * As for a share, the caller keeps reln from changing meanwhile; the pages
 * are written out, and the page store synced, before this returns: the
 * blocks then hold what the snapshot holds, which redo skipping a page's
 * records from before a copy of its path relies on (tree_find()).
 */
static uint64 snapshot_take_slot(SMgrRelation reln);

uint64
bmap_snapshot_take(SMgrRelation reln)
{
	uint64		id = snapshot_take_slot(reln);

	/* as after a share (bmapshare()), but synced before it is relied on */
	MarkBufferedRelationShared(reln, MAIN_FORKNUM);
	FlushRelationsAllBuffers(&reln, 1);
	mdimmedsync(store_smgr(reln->smgr_rlocator.locator), MAIN_FORKNUM);
	INJECTION_POINT("bmap-snapshot-taken", NULL);
	return id;
}

/*
 * bmap_snapshot_take() of each of rels[] (n of them), their ids into ids[]:
 * their pages written out in one pass over the buffers, and their page
 * stores synced once each, as the transaction commits (bmapsyncshares()),
 * as a share's are.  For a caller that relies on the snapshots only once
 * its transaction has committed: until then a crash leaves them unsynced,
 * kept by nobody.  Each id is set as its snapshot is taken (the others 0),
 * so that, if one fails, the caller knows of those taken already.
 */
void
bmap_snapshot_take_many(SMgrRelation *rels, int n, uint64 *ids)
{
	for (int i = 0; i < n; i++)
		ids[i] = 0;
	for (int i = 0; i < n; i++)
	{
		ids[i] = snapshot_take_slot(rels[i]);
		INJECTION_POINT("bmap-snapshot-slot-taken", NULL);
	}
	MarkBufferedRelationsShared(rels, n, MAIN_FORKNUM);
	if (n > 0)
		FlushRelationsAllBuffers(rels, n);
	for (int i = 0; i < n; i++)
		sync_store_later(rels[i]->smgr_rlocator.locator);
	if (n > 0)
		INJECTION_POINT("bmap-snapshot-taken", NULL);
}

/* A snapshot's slot of reln set, the map's epoch raised: its id. */
static uint64
snapshot_take_slot(SMgrRelation reln)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	LOCKTAG		sweeptag;
	Buffer		metabuf;
	Buffer		headerbuf;
	Buffer		rootbuf;
	BmapHeader *header;
	BmapRoot	root;
	BlockNumber tops[BMAP_TREE_TOPS];
	uint32		ntops = 0;
	int			slot;
	RefsPages	rp;
	xl_bmap_header_delta hdelta;
	xl_bmap_snapshot_delta sdelta;
	xl_bmap_tree xlrec;
	XLogRecPtr	lsn;
	uint64		id;
	FullTransactionId owner;

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("cannot keep a snapshot of a relation during recovery")));

	/* the snapshots nobody keeps let go of, and this one's owner named */
	(void) snapshot_reclaim(reln);
	owner = GetCurrentFullTransactionId();

	/* not while a sweep counts what the maps hold */
	SET_LOCKTAG_OBJECT(sweeptag, rel.dbOid, BMAP_SWEEP_CLASSID, rel.spcOid, 0);
	(void) LockAcquire(&sweeptag, ShareLock, false, false);
	metabuf = alloc_read(rel, 0);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(metabuf), BMAP_PAGE_META, rel, MAIN_FORKNUM, 0);
	refs_cover(rel, PageGetBmapMeta(BufferGetPage(metabuf))->hw);
	UnlockReleaseBuffer(metabuf);

	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	header = PageGetBmapHeader(BufferGetPage(headerbuf));
	tree_preserve(reln, header->nblocks, header->epoch);

	slot = snapshot_slot(reln, 0, &root);
	if (slot < 0)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("block-mapped relation %s keeps %d snapshots already",
						relpath(reln->smgr_rlocator, MAIN_FORKNUM).str,
						BMAP_TREE_SNAPSHOTS)));
	for (int k = 0; k < BMAP_TREE_TOPS; k++)
		if (BlockNumberIsValid(root.top[k].block))
			tops[ntops++] = root.top[k].block;
	qsort(tops, ntops, sizeof(BlockNumber), cmp_block);
	rp.n = 0;
	if (ntops > 0 && refs_lock(rel, tops, ntops, &rp) != ntops)
		elog(ERROR, "the subtrees of \"%s\" span too many count pages",
			 relpath(reln->smgr_rlocator, MAIN_FORKNUM).str);
	refs_add_holders(&rp, tops, ntops);

	rootbuf = map_read(rel, 1);
	map_lock(rootbuf, false);
	check_page(BufferGetPage(rootbuf), BMAP_PAGE_ROOT, rel, BMAP_FORKNUM, 1);

	hdelta.kind = BMAP_DELTA_HEADER;
	hdelta.flags = 0;
	hdelta.nblocks = header->nblocks;
	hdelta.epoch = header->epoch + 1;
	hdelta.fold_epoch = header->fold_epoch;
	hdelta.shared_below = 0;
	memset(&sdelta, 0, sizeof(sdelta));
	sdelta.kind = BMAP_DELTA_SNAPSHOT;
	sdelta.slot = slot;
	sdelta.snap.owner = U64FromFullTransactionId(owner);
	sdelta.snap.nblocks = header->nblocks;
	sdelta.snap.fold_epoch = header->fold_epoch;
	sdelta.snap.epoch = hdelta.epoch;
	for (int k = 0; k < BMAP_TREE_TOPS; k++)
	{
		sdelta.snap.tops[k].block = root.top[k].block;
		sdelta.snap.tops[k].epoch = root.top[k].epoch;
		sdelta.snap.tops[k].since = root.top[k].since;
	}
	xlrec.locator = rel;
	xlrec.op = BMAP_TREE_SNAPSHOT;
	xlrec.flags = 0;
	xlrec.node = xlrec.from = InvalidBlockNumber;
	xlrec.old_hw = xlrec.new_hw = InvalidBlockNumber;

	XLogEnsureRecordSpace(2 + BMAP_TREE_TOPS, 3 + BMAP_TREE_TOPS);
	/* (a position no other record has, as the snapshot's id) */
	id = sdelta.snap.id = GetXLogInsertRecPtr();
	START_CRIT_SECTION();
	apply_snapshot_delta(BufferGetPage(rootbuf), &sdelta);
	MarkBufferDirty(rootbuf);
	apply_header_delta(BufferGetPage(headerbuf), &hdelta);
	MarkBufferDirty(headerbuf);
	XLogBeginInsert();
	XLogRegisterData(&xlrec, sizeof(xlrec));
	XLogRegisterBuffer(0, rootbuf, REGBUF_STANDARD);
	XLogRegisterBufData(0, &sdelta, sizeof(sdelta));
	XLogRegisterBuffer(1, headerbuf, REGBUF_STANDARD);
	XLogRegisterBufData(1, &hdelta, sizeof(hdelta));
	refs_apply_register(&rp, 2);
	lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_TREE);
	PageSetLSN(BufferGetPage(rootbuf), lsn);
	PageSetLSN(BufferGetPage(headerbuf), lsn);
	refs_set_lsn(&rp, lsn);
	END_CRIT_SECTION();

	refs_release(&rp);
	map_unlock(rootbuf);
	ReleaseBuffer(rootbuf);
	UnlockReleaseBuffer(headerbuf);
	LockRelease(&sweeptag, ShareLock, false);
	return id;
}

/*
 * Let go of snapshot 'id' of reln: its slot cleared, then its subtrees let
 * go of as a drop's are, so that what only it held is freed.  False if reln
 * keeps no such snapshot.  A crash between the two leaves the subtrees'
 * counts a holder too high, for a sweep.
 */
bool
bmap_snapshot_release(SMgrRelation reln, uint64 id)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		headerbuf;
	Buffer		rootbuf;
	BmapRoot	root;
	BmapEntry	tops[BMAP_TREE_TOPS];
	int			slot;
	xl_bmap_snapshot_delta sdelta;
	xl_bmap_tree xlrec;
	XLogRecPtr	lsn;

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("cannot let go of a relation's snapshot during recovery")));
	if (id == 0)
		return false;
	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	slot = snapshot_slot(reln, id, &root);
	if (slot < 0)
	{
		UnlockReleaseBuffer(headerbuf);
		return false;
	}
	memcpy(tops, snapshot_tops(&root.snap[slot]), sizeof(tops));

	rootbuf = map_read(rel, 1);
	map_lock(rootbuf, false);
	check_page(BufferGetPage(rootbuf), BMAP_PAGE_ROOT, rel, BMAP_FORKNUM, 1);
	memset(&sdelta, 0, sizeof(sdelta));
	sdelta.kind = BMAP_DELTA_SNAPSHOT;
	sdelta.slot = slot;
	xlrec.locator = rel;
	xlrec.op = BMAP_TREE_SNAPSHOT;
	xlrec.flags = 0;
	xlrec.node = xlrec.from = InvalidBlockNumber;
	xlrec.old_hw = xlrec.new_hw = InvalidBlockNumber;

	START_CRIT_SECTION();
	apply_snapshot_delta(BufferGetPage(rootbuf), &sdelta);
	MarkBufferDirty(rootbuf);
	XLogBeginInsert();
	XLogRegisterData(&xlrec, sizeof(xlrec));
	XLogRegisterBuffer(0, rootbuf, REGBUF_STANDARD);
	XLogRegisterBufData(0, &sdelta, sizeof(sdelta));
	lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_TREE);
	PageSetLSN(BufferGetPage(rootbuf), lsn);
	END_CRIT_SECTION();
	map_unlock(rootbuf);
	ReleaseBuffer(rootbuf);
	INJECTION_POINT("bmap-snapshot-cleared", NULL);

	tree_release_tops(reln, tops);
	UnlockReleaseBuffer(headerbuf);
	return true;
}

/*
 * The snapshots reln keeps, at most 'max' of them into snaps[]: how many it
 * keeps.  Not those nobody keeps (snapshot_abandoned()), whose slots the
 * next snapshot taken lets go of.
 */
int
bmap_snapshot_list(SMgrRelation reln, BmapSnapshot *snaps, int max)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		headerbuf;
	BmapRoot	root;
	int			n = 0;

	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_SHARE);
	(void) snapshot_slot(reln, 0, &root);
	UnlockReleaseBuffer(headerbuf);
	for (int s = 0; s < BMAP_TREE_SNAPSHOTS; s++)
	{
		if (root.snap[s].id == 0 || snapshot_abandoned(&root.snap[s]))
			continue;
		if (n < max)
		{
			snaps[n].id = root.snap[s].id;
			snaps[n].nblocks = root.snap[s].nblocks;
		}
		n++;
	}
	return n;
}

/*
 * Are reln's pages still those of its snapshot 'id': its root page pointing
 * to the subtrees the snapshot holds, and its length the snapshot's?  O(1):
 * a page changed since the snapshot was taken was copied first, its path
 * with it, so the root page points to a copy of a subtree the snapshot
 * holds where anything under it changed.  False if reln keeps no snapshot
 * 'id'.  The caller keeps reln from changing, or takes the answer as of
 * when it was asked.
 */
bool
bmap_snapshot_unchanged(SMgrRelation reln, uint64 id)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		headerbuf;
	BmapRoot	root;
	BlockNumber nblocks;
	int			slot;
	bool		same;

	if (id == 0)
		return false;
	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	nblocks = PageGetBmapHeader(BufferGetPage(headerbuf))->nblocks;
	slot = snapshot_slot(reln, id, &root);
	UnlockReleaseBuffer(headerbuf);
	if (slot < 0)
		return false;
	same = root.snap[slot].nblocks == nblocks;
	for (int k = 0; same && k < BMAP_TREE_TOPS; k++)
	{
		const BmapEntry *top = &snapshot_tops(&root.snap[slot])[k];

		same = root.top[k].block == top->block &&
			(!BlockNumberIsValid(top->block) || root.top[k].since == top->since);
	}
	return same;
}

/*
 * Make 'dst', a new relation file number in src's tablespace and database,
 * a copy of src's snapshot 'id': its root page gets the snapshot's
 * subtrees, each gaining a holder, and its header the snapshot's length, in
 * one record.  O(1) in the relation's size.  The copy has no changes list
 * and has never been folded.  The snapshot's blocks were preserved and
 * synced when it was taken.  Only the main fork: the caller makes the
 * other forks (src's visibility map says nothing of the snapshot's pages).
 * With 'replacing', dst is to take src's place, src dropped as the
 * transaction commits (a restore): dst's entries may then be shared with
 * another map only where src's might, and src's header is left as it is.
 */
void
bmap_share_snapshot(SMgrRelation src, uint64 id, SMgrRelation dst,
					bool replacing)
{
	RelFileLocator srel = src->smgr_rlocator.locator;
	RelFileLocator drel = dst->smgr_rlocator.locator;
	LOCKTAG		sweeptag;
	Buffer		headerbuf;
	BmapRoot	root;
	int			slot;
	xl_bmap_header_delta hdelta;
	xl_bmap_header_delta shdelta;

	if (srel.spcOid != drel.spcOid || srel.dbOid != drel.dbOid)
		elog(ERROR, "block-mapped relation %s can't share the blocks of %s, in another page store",
			 relpath(dst->smgr_rlocator, MAIN_FORKNUM).str,
			 relpath(src->smgr_rlocator, MAIN_FORKNUM).str);
	smgrusewhich(dst, SMGR_BMAP);
	SET_LOCKTAG_OBJECT(sweeptag, srel.dbOid, BMAP_SWEEP_CLASSID, srel.spcOid, 0);
	(void) LockAcquire(&sweeptag, ShareLock, false, false);

	headerbuf = map_read(srel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, srel, BMAP_FORKNUM, 0);
	slot = snapshot_slot(src, id, &root);
	if (id == 0 || slot < 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("block-mapped relation %s keeps no snapshot %llu",
						relpath(src->smgr_rlocator, MAIN_FORKNUM).str,
						(unsigned long long) id)));
	create_map(dst);

	/*
	 * (every entry of the snapshot was born before src's epoch, and is
	 * dst's: src's born before the snapshot's epoch may now be shared with
	 * another map, and all of dst's are)
	 */
	hdelta.kind = BMAP_DELTA_HEADER;
	hdelta.flags = BMAP_DELTA_SHARED_BELOW;
	hdelta.nblocks = root.snap[slot].nblocks;
	hdelta.epoch = PageGetBmapHeader(BufferGetPage(headerbuf))->epoch;
	hdelta.fold_epoch = 0;
	hdelta.shared_below = replacing ?
		PageGetBmapHeader(BufferGetPage(headerbuf))->shared_below : hdelta.epoch;
	{
		BmapHeader *sh = PageGetBmapHeader(BufferGetPage(headerbuf));

		shdelta.kind = BMAP_DELTA_HEADER;
		shdelta.flags = BMAP_DELTA_SHARED_BELOW;
		shdelta.nblocks = sh->nblocks;
		shdelta.epoch = sh->epoch;
		shdelta.fold_epoch = sh->fold_epoch;
		shdelta.shared_below = Max(sh->shared_below, root.snap[slot].epoch);
	}
	tree_share_root(src, dst, snapshot_tops(&root.snap[slot]), &hdelta,
					!replacing &&
					shdelta.shared_below != PageGetBmapHeader(BufferGetPage(headerbuf))->shared_below ?
					headerbuf : InvalidBuffer, &shdelta);
	UnlockReleaseBuffer(headerbuf);
	LockRelease(&sweeptag, ShareLock, false);
	dst->smgr_cached_nblocks[MAIN_FORKNUM] = InvalidBlockNumber;
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
	MapPage		mp;
	bool		shared;

	*copies = false;
	if (forknum != MAIN_FORKNUM)
		return false;
	epoch = lookup_epoch(reln);
	if (!lookup_entry_at(reln, blkno, &entry, &mp, NULL) || entry.epoch >= epoch)
		return false;
	/* (a tree's block is shared too if its leaf is, through any node) */
	shared = lookup_ref(reln->smgr_rlocator.locator, entry.block) > 0 ||
		!tree_path_own(reln, blkno, epoch);
	*copies = shared || entry.epoch < changes_fold_epoch(reln);
	return shared;
}

/*
 * Is logical block 'blkno''s block possibly held by another map, not only
 * by this map's own snapshots?  Its entry born before the map's
 * shared_below (raised by a share, and by a copy made of one of its
 * snapshots) may be; one born since only by its snapshots, which keep the
 * relation's pages as they were for its own branch.  A hint for callers
 * weighing a copy's cost: preparing the page for a change copies it either
 * way.  shared_below only rises (a restore's new storage takes its old
 * storage's, which goes): once the other map is gone (a branch deleted),
 * the entries born before it still count as shared elsewhere until they
 * change, which costs VACUUM's work on them, never their contents.
 */
bool
bmappagesharedelsewhere(SMgrRelation reln, ForkNumber forknum,
						BlockNumber blkno)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		buf;
	uint32		epoch;
	uint32		shared_below;
	BmapEntry	entry;
	MapPage		mp;

	if (forknum != MAIN_FORKNUM)
		return false;
	buf = map_read(rel, 0);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(buf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	epoch = PageGetBmapHeader(BufferGetPage(buf))->epoch;
	shared_below = PageGetBmapHeader(BufferGetPage(buf))->shared_below;
	UnlockReleaseBuffer(buf);
	if (!lookup_entry_at(reln, blkno, &entry, &mp, NULL) || entry.epoch >= epoch)
		return false;
	return entry.epoch < shared_below;
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
 * A tree's entry is changed on a leaf that is the relation's own: a leaf
 * another map shares, or one under a shared node, has its path copied first
 * (tree_leaf_for()), which gives each of the leaf's blocks a holder more, so
 * a block on a leaf just copied is copied in turn as one another map holds.
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
	MapPage		mp;

	if (forknum != MAIN_FORKNUM)
		return;

	/* the common case, with share locks alone */
	epoch = lookup_epoch(reln);
	if (!lookup_entry(reln, blkno, &entry) || entry.epoch >= epoch)
		return;

	headerbuf = map_read(rel, 0);
	for (;;)
	{
		LockBuffer(headerbuf, BUFFER_LOCK_SHARE);
		check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
		epoch = PageGetBmapHeader(BufferGetPage(headerbuf))->epoch;
		if (!lookup_entry_at(reln, blkno, &entry, &mp, NULL) || entry.epoch >= epoch)
		{
			UnlockReleaseBuffer(headerbuf);
			return;
		}
		if (tree_path_own(reln, blkno, epoch))
			break;

		/*
		 * A tree's leaf shared, or a node above it: the path is copied
		 * first, under the header held exclusively (tree_leaf_for()), and
		 * the block looked up again.
		 */
		LockBuffer(headerbuf, BUFFER_LOCK_UNLOCK);
		LockBuffer(headerbuf, BUFFER_LOCK_EXCLUSIVE);
		(void) tree_leaf_for(reln, headerbuf, blkno, false, true, &mp);
		LockBuffer(headerbuf, BUFFER_LOCK_UNLOCK);
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
	ent->slot = MP_SLOT(&mp, blkno);
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
		mapbuf = mp_read(reln, &mp);
		map_lock(mapbuf, false);
		mp_check(reln, &mp, BufferGetPage(mapbuf));

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

		/*
		 * This record restores the page before the caller's change.  If
		 * that change skips WAL, commit must log the final page contents;
		 * an fsync alone would let redo overwrite them with this image.
		 */
		RelFileLocatorForceWALAtCommit(rel);

		/* the old block gets what other maps should read, if only buffered */
		if (BufferIsDirty(buffer))
			FlushOneBuffer(buffer);

		alloc_take(rel, 1, &new_block, &alloc);
		ent->block = new_block;
		reused = new_block < alloc.old_hw;

		/*
		 * The page store reaches the block.  The page's contents come with
		 * the record, as an image; a page of zeroes (one being initialized)
		 * is zeroed in the new block instead, here and in redo.  An image's
		 * block is written only from the page's buffer, later, so a node's
		 * stale buffer for it is dropped now, here and in redo.
		 */
		if (reused && !zeroed)
			drop_node_buffer(rel, new_block);
		prepare_store(rel, alloc.old_hw, alloc.new_hw,
					  &new_block, zeroed && reused ? 1 : 0, false,
					  PGSTAT_PROLLY_BMAP_STORE_BYTES_WRITTEN, InvalidXLogRecPtr);

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
			pgstat_count_prolly(PGSTAT_PROLLY_BMAP_REFCOUNT_UPDATES, 1);
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

		mapbuf = mp_read(reln, &mp);
		map_lock(mapbuf, false);
		mp_check(reln, &mp, BufferGetPage(mapbuf));

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

		/* (copied for another map's sake, or else only to keep the fold's) */
		pgstat_count_prolly(count > 0 ? PGSTAT_PROLLY_PAGE_COPIES_SHARED :
							PGSTAT_PROLLY_PAGE_COPIES_FOLD, 1);

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
	delta.shared_below = 0;
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
 * Walking a tree: every node reached from the root page told to
 * visit_node(), whose children (if it says to go on) are visited too, and
 * every entry of a leaf so visited told to visit_entry(), with its logical
 * block (InvalidBlockNumber under a snapshot's subtree, which maps no block
 * of the relation as it is).  Each node is read under a share lock, and
 * must be the incarnation
 * its pointer names: the caller holds the header, so none is replaced or
 * let go of meanwhile.
 */
typedef bool (*TreeNodeVisit) (BlockNumber node, uint16 level,
							   BlockNumber first, void *arg);
typedef void (*TreeEntryVisit) (const BmapEntry *entry, BlockNumber blkno,
								void *arg);

static void
tree_walk_node(SMgrRelation reln, const BmapEntry *ptr, uint16 level,
			   BlockNumber first, bool held, TreeNodeVisit visit_node,
			   TreeEntryVisit visit_entry, void *arg)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	BmapEntry  *entries;
	Buffer		buf;

	if (!visit_node(ptr->block, level, first, arg))
		return;
	entries = palloc(BMAP_TREE_FANOUT * sizeof(BmapEntry));
	buf = tree_read(rel, ptr->block, RBM_NORMAL);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	if (!tree_node_is(BufferGetPage(buf), level, first, ptr->since))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("block %u of the page store of \"%s\" is not the node of its block map it was",
						ptr->block, relpath(reln->smgr_rlocator, MAIN_FORKNUM).str)));
	memcpy(entries, PageGetBmapEntries(BufferGetPage(buf)),
		   BMAP_TREE_FANOUT * sizeof(BmapEntry));
	UnlockReleaseBuffer(buf);
	CHECK_FOR_INTERRUPTS();

	for (uint32 i = 0; i < BMAP_TREE_FANOUT; i++)
	{
		if (!BlockNumberIsValid(entries[i].block))
			continue;
		if (level == 0)
			visit_entry(&entries[i], held ? InvalidBlockNumber : first + i,
						arg);
		else
			tree_walk_node(reln, &entries[i], level - 1,
						   (BlockNumber) Min((uint64) first +
											 i * tree_span(level - 1),
											 (uint64) MaxBlockNumber),
						   held, visit_node, visit_entry, arg);
	}
	pfree(entries);
}

static void
tree_walk(SMgrRelation reln, TreeNodeVisit visit_node,
		  TreeEntryVisit visit_entry, void *arg)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	BmapRoot	root;
	Buffer		buf = map_read(rel, 1);
	uint64		first = 0;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(buf), BMAP_PAGE_ROOT, rel, BMAP_FORKNUM, 1);
	root = *PageGetBmapRoot(BufferGetPage(buf));
	UnlockReleaseBuffer(buf);
	for (int k = 0; k < BMAP_TREE_TOPS; k++)
	{
		if (BlockNumberIsValid(root.top[k].block))
			tree_walk_node(reln, &root.top[k], k, (BlockNumber) first,
						   false, visit_node, visit_entry, arg);
		first += tree_span(k);
	}

	/* and the subtrees its snapshots hold */
	for (int s = 0; s < BMAP_TREE_SNAPSHOTS; s++)
	{
		const BmapEntry *tops = snapshot_tops(&root.snap[s]);

		if (root.snap[s].id == 0)
			continue;
		first = 0;
		for (int k = 0; k < BMAP_TREE_TOPS; k++)
		{
			if (BlockNumberIsValid(tops[k].block))
				tree_walk_node(reln, &tops[k], k, (BlockNumber) first,
							   true, visit_node, visit_entry, arg);
			first += tree_span(k);
		}
	}
}

static int
cmp_entry_block(const void *a, const void *b)
{
	return pg_cmp_u32(((const BmapEntry *) a)->block,
					  ((const BmapEntry *) b)->block);
}

/*
 * Let go of a tree's node that nothing else holds (its count was zero when
 * its holder let go of it): each child loses this node's reference, a node
 * child no other holds let go of the same way, a leaf's blocks no other map
 * holds freed; then the node is freed.  A crash partway leaves blocks
 * allocated that nothing holds, for a sweep.
 */
static void
tree_release_own(SMgrRelation reln, BlockNumber node, uint16 level,
				 BlockNumber first, XLogRecPtr born)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	BmapEntry  *entries = palloc(BMAP_TREE_FANOUT * sizeof(BmapEntry));
	BlockNumber *blocks = palloc(BMAP_TREE_FANOUT * sizeof(BlockNumber));
	BlockNumber *tofree = palloc(BMAP_TREE_FANOUT * sizeof(BlockNumber));
	uint32		n = 0;
	uint32		nfree;
	Buffer		buf;

	buf = tree_read(rel, node, RBM_NORMAL);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	if (!tree_node_is(BufferGetPage(buf), level, first, born))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("block %u of the page store of \"%s\" is not the node of its block map it was",
						node, relpath(reln->smgr_rlocator, MAIN_FORKNUM).str)));
	for (uint32 i = 0; i < BMAP_TREE_FANOUT; i++)
	{
		BmapEntry  *e = &PageGetBmapEntries(BufferGetPage(buf))[i];

		if (!BlockNumberIsValid(e->block))
			continue;
		entries[n] = *e;
		/* (a child's first block, kept in its epoch's place while sorted) */
		entries[n].epoch = (uint32) i;
		n++;
	}
	UnlockReleaseBuffer(buf);

	/* the children, each once: those no other holds are this node's to let go */
	qsort(entries, n, sizeof(BmapEntry), cmp_entry_block);
	for (uint32 i = 0; i < n; i++)
		blocks[i] = entries[i].block;
	nfree = unref_blocks(rel, blocks, n, tofree);
	if (level == 0)
		free_blocks(rel, tofree, nfree);
	else
	{
		for (uint32 i = 0; i < nfree; i++)
		{
			BmapEntry	key;
			BmapEntry  *child;

			key.block = tofree[i];
			child = bsearch(&key, entries, n, sizeof(BmapEntry), cmp_entry_block);
			Assert(child != NULL);
			tree_release_own(reln, child->block, level - 1,
							 (BlockNumber) Min((uint64) first + child->epoch *
											   tree_span(level - 1),
											   (uint64) MaxBlockNumber),
							 child->since);
		}
	}
	free_blocks(rel, &node, 1);
	pfree(entries);
	pfree(blocks);
	pfree(tofree);
}

/*
 * Let go of the subtree 'ptr' points to, of 'level' from 'first': one
 * reference less if another holds it, else all of it that no other holds.
 */
static void
tree_release(SMgrRelation reln, const BmapEntry *ptr, uint16 level,
			 BlockNumber first)
{
	BlockNumber node = ptr->block;
	BlockNumber own;

	if (unref_blocks(reln->smgr_rlocator.locator, &node, 1, &own) == 0)
		return;
	tree_release_own(reln, node, level, first, ptr->since);
}

/* Let go of the subtrees 'tops' points to (a root page's, or a snapshot's) */
static void
tree_release_tops(SMgrRelation reln, const BmapEntry *tops)
{
	uint64		first = 0;

	for (int k = 0; k < BMAP_TREE_TOPS; k++)
	{
		if (BlockNumberIsValid(tops[k].block))
			tree_release(reln, &tops[k], k, (BlockNumber) first);
		first += tree_span(k);
	}
}

/* ----------------------------------------------------------------
 * The release queue
 * ----------------------------------------------------------------
 */

/*
 * A database's page store in a tablespace has a release queue
 * (BMAP_QUEUE_RELNUMBER): a stack of entries (BmapQueued), each holding the
 * node or data block it names as the map it came from held it.  Block 0 is
 * its metapage, saying how many entries there are; entry i is slot
 * QUEUE_SLOT(i) of block QUEUE_PAGE(i).  Dropping a relation pushes its
 * root page's pointers (its snapshots' too) and its changes list's blocks,
 * so letting go of a relation's storage costs its root page and its
 * changes list, not its size.  A drain pops entries from the top, a step
 * at a time (bmap_release_step()), and lets go of each as tree_release()
 * would: one holder less, or, if nothing else held the block, the block
 * freed, a node's children pushed in its place.  A node another map still
 * holds is only counted down; its subtree isn't read.
 *
 * A step is one record: the entries popped, the counts they change, the
 * blocks they free and the children they push.  A crash replays it whole
 * or not at all, so no entry is let go of twice and none is lost; what the
 * queue holds counts as held after a crash as before it.  Steps are
 * serialized per store by a lock (DRAIN_LOCK), and held off while a sweep
 * counts holders, which counts the queue's entries as holders too
 * (sweep_queue()).
 *
 * Drains run in autovacuum workers (prolly_autovacuum()), in a sweep before
 * it counts, and before a VACUUM of a relation of the store
 * (bmapreleasestep()): a page a dropped map still holds counts as shared,
 * which VACUUM would otherwise leave as it is or copy to change.  With
 * autovacuum off, an extension that would grow the store drains first
 * (bmapbeforeextend()).
 *
 * A relation a top-level transaction drops at commit is queued before the
 * commit record (bmapprecommitdrop()), each entry naming the transaction
 * (queued_owner()), so that a crash after the commit finds it queued.  Such
 * an entry holds nothing until its transaction has committed and finished
 * its drops (queued_state()): a step stops at it before, and pops it with
 * nothing let go of if the transaction didn't commit.
 *
 * Locks: the metapage, exclusively, for a push or a step; a push then
 * locks the pages it writes.  A step reads the pages it pops from under
 * share locks, then locks the reference-count pages, the allocator's, and
 * the pages it pushes children onto, in that order.  A drop pushes under
 * its relation's header, held exclusively; nothing that holds the queue's
 * metapage waits for a map's header.  The file grows to the deepest the
 * stack has been, under the metapage's lock, and never shrinks.
 */

/* a step's locks: one store's drain at a time, and a sweep's counting */
#define DRAIN_LOCK(tag, rel) \
	SET_LOCKTAG_OBJECT(tag, (rel).dbOid, BMAP_SWEEP_CLASSID, (rel).spcOid, 2)
#define SWEEP_LOCK(tag, rel) \
	SET_LOCKTAG_OBJECT(tag, (rel).dbOid, BMAP_SWEEP_CLASSID, (rel).spcOid, 0)

/* queue pages one record changes at most */
#define BMAP_QUEUE_PAGES_PER_RECORD	8

/*
 * Entries one step pops at most, and the reference-count and bitmap pages
 * their blocks may lie on: with the metapage, the pages a node's children
 * are pushed onto and the allocator's metapage, all one record.
 */
#define BMAP_RELEASE_STEP		128
#define BMAP_RELEASE_REFS_PAGES	16
#define BMAP_RELEASE_BITS_PAGES	4

StaticAssertDecl(BMAP_RELEASE_REFS_PAGES <= BMAP_REFS_PER_RECORD,
				 "a step's count pages are locked at once");
StaticAssertDecl(1 + 3 + BMAP_RELEASE_REFS_PAGES + 1 + BMAP_RELEASE_BITS_PAGES <=
				 XLR_MAX_BLOCK_ID + 1,
				 "a step's pages fit one record");
StaticAssertDecl(BMAP_TREE_FANOUT <= 2 * BMAP_QUEUE_PER_PAGE,
				 "a node's children are pushed onto three pages at most");

static Buffer
queue_read(RelFileLocator rel, BlockNumber blkno)
{
	check_may_read();
	return ReadBufferWithoutRelcache(with_relnumber(rel, BMAP_QUEUE_RELNUMBER),
									 MAIN_FORKNUM, blkno, RBM_NORMAL, NULL,
									 true);
}

/*
 * Give the queue 'need' pages, all zeroes until a record initializes one.
 * Under the metapage's exclusive lock, which serializes the file's growth.
 */
static void
queue_cover(RelFileLocator rel, BlockNumber need)
{
	SMgrRelation qsrel = queue_smgr(rel);

	check_may_read();
	if (smgrnblocks(qsrel, MAIN_FORKNUM) < need)
		ReleaseBuffer(ExtendBufferedRelTo(BMR_SMGR(qsrel, RELPERSISTENCE_PERMANENT),
										  MAIN_FORKNUM, NULL,
										  EB_SKIP_EXTENSION_LOCK, need,
										  RBM_NORMAL));
}

static void
apply_queue_meta_delta(Page page, const xl_bmap_queue_meta_delta *delta)
{
	if (delta->flags & BMAP_DELTA_INIT)
	{
		init_page(page, BMAP_PAGE_QUEUE_META);
		PageGetBmapQueueMeta(page)->version = BMAP_VERSION;
	}
	PageGetBmapQueueMeta(page)->nentries = delta->nentries;
}

static void
apply_queue_delta(Page page, const xl_bmap_queue_delta *delta)
{
	if (delta->flags & BMAP_DELTA_INIT)
		init_page(page, BMAP_PAGE_QUEUE);
	Assert(delta->first + delta->n <= BMAP_QUEUE_PER_PAGE);
	memcpy(PageGetBmapQueued(page) + delta->first, delta + 1,
		   delta->n * sizeof(BmapQueued));
}

static Size
queue_delta_size(const xl_bmap_queue_delta *d)
{
	return sizeof(xl_bmap_queue_delta) + d->n * sizeof(BmapQueued);
}

/*
 * The queue's metapage, read and locked exclusively; *nentries gets the
 * count of entries, and *isnew whether the page is yet to be initialized.
 */
static Buffer
queue_lock_meta(RelFileLocator rel, uint32 *nentries, bool *isnew)
{
	Buffer		buf = queue_read(rel, 0);
	Page		page = BufferGetPage(buf);

	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	*isnew = PageIsNew(page);
	if (*isnew)
		*nentries = 0;
	else
	{
		check_page(page, BMAP_PAGE_QUEUE_META,
				   with_relnumber(rel, BMAP_QUEUE_RELNUMBER), MAIN_FORKNUM, 0);
		*nentries = PageGetBmapQueueMeta(page)->nentries;
	}
	return buf;
}

/* Queue pages locked for one record, each with the delta that will change it. */
typedef struct QueuePages
{
	int			n;
	Buffer		buf[BMAP_QUEUE_PAGES_PER_RECORD];
	xl_bmap_queue_delta *delta[BMAP_QUEUE_PAGES_PER_RECORD];
} QueuePages;

/*
 * Lock the pages of entries [pos, pos + n), which the file has, and make the
 * deltas setting them to items[].  A page the stack hasn't reached yet is
 * all zeroes, and reached from its first slot: its delta initializes it.
 */
static void
queue_write(RelFileLocator rel, QueuePages *qp, uint32 pos,
			const BmapQueued *items, uint32 n)
{
	RelFileLocator qloc = with_relnumber(rel, BMAP_QUEUE_RELNUMBER);
	uint32		i = 0;

	qp->n = 0;
	while (i < n)
	{
		BlockNumber blk = QUEUE_PAGE(pos + i);
		uint32		slot = QUEUE_SLOT(pos + i);
		uint32		take = Min(n - i, BMAP_QUEUE_PER_PAGE - slot);
		Buffer		buf;
		Page		page;
		xl_bmap_queue_delta *delta;

		if (qp->n == BMAP_QUEUE_PAGES_PER_RECORD)
			elog(ERROR, "release queue entries span too many pages for one record");
		buf = queue_read(rel, blk);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
		delta = palloc0(sizeof(xl_bmap_queue_delta) + take * sizeof(BmapQueued));
		delta->kind = BMAP_DELTA_QUEUE;
		if (PageIsNew(page))
		{
			if (slot != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("block %u of \"%s\" is not an initialized block map page",
								blk, relpathperm(qloc, MAIN_FORKNUM).str)));
			delta->flags |= BMAP_DELTA_INIT;
		}
		else
			check_page(page, BMAP_PAGE_QUEUE, qloc, MAIN_FORKNUM, blk);
		delta->first = slot;
		delta->n = take;
		memcpy(delta + 1, items + i, take * sizeof(BmapQueued));
		qp->buf[qp->n] = buf;
		qp->delta[qp->n] = delta;
		qp->n++;
		i += take;
	}
}

/* in the record's critical section, as refs_apply_register() */
static int
queue_apply_register(QueuePages *qp, int first)
{
	int			id = first;

	for (int k = 0; k < qp->n; k++)
	{
		apply_queue_delta(BufferGetPage(qp->buf[k]), qp->delta[k]);
		MarkBufferDirty(qp->buf[k]);
		XLogRegisterBuffer(id, qp->buf[k], REGBUF_STANDARD |
						   ((qp->delta[k]->flags & BMAP_DELTA_INIT) ?
							REGBUF_WILL_INIT : 0));
		XLogRegisterBufData(id, qp->delta[k], queue_delta_size(qp->delta[k]));
		id++;
	}
	return id;
}

static void
queue_set_lsn(QueuePages *qp, XLogRecPtr lsn)
{
	for (int k = 0; k < qp->n; k++)
		PageSetLSN(BufferGetPage(qp->buf[k]), lsn);
}

static void
queue_release(QueuePages *qp)
{
	for (int k = 0; k < qp->n; k++)
	{
		UnlockReleaseBuffer(qp->buf[k]);
		pfree(qp->delta[k]);
	}
	qp->n = 0;
}

/* in the record's critical section: the metapage's count set, as block 0 */
static void
register_queue_meta(Buffer metabuf, bool isnew,
					xl_bmap_queue_meta_delta *mdelta, uint32 nentries)
{
	mdelta->kind = BMAP_DELTA_QUEUE_META;
	mdelta->flags = isnew ? BMAP_DELTA_INIT : 0;
	mdelta->nentries = nentries;
	apply_queue_meta_delta(BufferGetPage(metabuf), mdelta);
	MarkBufferDirty(metabuf);
	XLogRegisterBuffer(0, metabuf, REGBUF_STANDARD |
					   (isnew ? REGBUF_WILL_INIT : 0));
	XLogRegisterBufData(0, mdelta, sizeof(*mdelta));
}

/*
 * Push items[0 .. n) onto rel's store's queue, in records of a few pages
 * each; returns the end of the last.  A crash between them leaves those not
 * pushed allocated, held by nothing, for a sweep.
 */
static XLogRecPtr
queue_push(RelFileLocator rel, const BmapQueued *items, uint32 n)
{
	uint32		i = 0;
	XLogRecPtr	end = InvalidXLogRecPtr;

	if (n == 0)
		return end;
	ensure_queue(rel);
	while (i < n)
	{
		uint32		nentries;
		bool		isnew;
		Buffer		metabuf = queue_lock_meta(rel, &nentries, &isnew);
		uint32		take;
		QueuePages	qp;
		xl_bmap_queue_meta_delta mdelta;
		xl_bmap_release xlrec;
		XLogRecPtr	lsn;

		take = Min(n - i, BMAP_QUEUE_PAGES_PER_RECORD * BMAP_QUEUE_PER_PAGE -
				   QUEUE_SLOT(nentries));
		if ((uint64) nentries + take > PG_UINT32_MAX)
			elog(ERROR, "release queue of database %u is full", rel.dbOid);
		queue_cover(rel, QUEUE_PAGE(nentries + take - 1) + 1);
		queue_write(rel, &qp, nentries, items + i, take);

		xlrec.locator = with_relnumber(rel, BMAP_QUEUE_RELNUMBER);
		xlrec.op = BMAP_RELEASE_QUEUE;
		xlrec.npopped = 0;
		XLogEnsureRecordSpace(qp.n, 2 + qp.n);
		START_CRIT_SECTION();
		XLogBeginInsert();
		XLogRegisterData(&xlrec, sizeof(xlrec));
		register_queue_meta(metabuf, isnew, &mdelta, nentries + take);
		queue_apply_register(&qp, 1);
		lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_RELEASE);
		PageSetLSN(BufferGetPage(metabuf), lsn);
		queue_set_lsn(&qp, lsn);
		END_CRIT_SECTION();

		queue_release(&qp);
		/* (counted before the metapage is let go: see bmapbeforeextend()) */
		pg_atomic_fetch_add_u64(&bmap_shared->queue_pushes, 1);
		UnlockReleaseBuffer(metabuf);
		pgstat_count_prolly(PGSTAT_PROLLY_BMAP_RELEASE_QUEUED, take);
		end = XactLastRecEnd;
		i += take;
	}
	return end;
}

static inline BmapQueued
queued(BlockNumber block, uint16 level, BlockNumber first, XLogRecPtr since)
{
	BmapQueued	q;

	memset(&q, 0, sizeof(q));
	q.block = block;
	q.level = level;
	q.first = first;
	q.since = since;
	return q;
}

/* (an entry's owner keeps a FullTransactionId's low 48 bits) */
#define QUEUED_OWNER_BITS	48
#define QUEUED_OWNER_MASK	((UINT64CONST(1) << QUEUED_OWNER_BITS) - 1)

static inline void
queued_set_owner(BmapQueued *q, FullTransactionId owner)
{
	uint64		v = U64FromFullTransactionId(owner) & QUEUED_OWNER_MASK;

	q->owner_hi = (uint16) (v >> 32);
	q->owner_lo = (uint32) v;
}

/*
 * The transaction an entry names (InvalidFullTransactionId: none), the
 * latest one whose low 48 bits it keeps that isn't in the future: one of
 * 2^48 transactions ago would be taken for a later one, which no queue
 * entry lasts for.
 */
static FullTransactionId
queued_owner(const BmapQueued *q)
{
	uint64		v = ((uint64) q->owner_hi << 32) | q->owner_lo;
	uint64		now;
	uint64		full;

	if (v == 0)
		return InvalidFullTransactionId;
	now = U64FromFullTransactionId(ReadNextFullTransactionId());
	full = (now & ~QUEUED_OWNER_MASK) | v;
	if (full > now && full > QUEUED_OWNER_MASK)
		full -= UINT64CONST(1) << QUEUED_OWNER_BITS;
	return FullTransactionIdFromU64(full);
}

/*
 * Is the transaction 'owner', committed, still to finish the drops it
 * queued before its commit record?  Until its drop of a relation has
 * dropped the relation's buffers, a dirty one written out would go through
 * its map to a block the queue may have freed and given another relation:
 * so it holds the lock DROP_LOCK names from before it queues them until
 * they are done, at commit or abort (bmapenddrops()), or the backend
 * exits.  Not waited for.  This backend's own count as in flight.
 */
static bool
drop_in_flight(FullTransactionId owner)
{
	LOCKTAG		tag;

	DROP_LOCK(tag, XidFromFullTransactionId(owner));
	if (LockHeldByMe(&tag, ExclusiveLock, false))
		return true;
	if (LockAcquire(&tag, ShareLock, false, true) == LOCKACQUIRE_NOT_AVAIL)
		return true;
	LockRelease(&tag, ShareLock, false);
	return false;
}

/*
 * What an entry holds by its owner (owner_state()): OWNER_NONE or
 * OWNER_COMMITTED, the block as a map holds it; OWNER_RUNNING, nothing yet
 * (its map still holds what it names, its drop not yet done: a committed
 * owner whose drops are in flight counts so too); OWNER_ENDED, nothing
 * (the drop didn't happen); OWNER_UNKNOWN, taken as nothing, the safe
 * way: if the drop did commit, the blocks are left to a sweep.
 */
static OwnerState
queued_state(const BmapQueued *q)
{
	FullTransactionId owner = queued_owner(q);
	OwnerState	state;

	if (!FullTransactionIdIsValid(owner))
		return OWNER_NONE;
	state = owner_state(owner);
	if (state == OWNER_COMMITTED && drop_in_flight(owner))
		state = OWNER_RUNNING;
	return state;
}

static inline bool
queued_holds(OwnerState state)
{
	return state == OWNER_NONE || state == OWNER_COMMITTED;
}

/* the subtrees 'tops' points to (a root page's, or a snapshot's), as entries */
static uint32
queued_tops(const BmapEntry *tops, BmapQueued *items)
{
	uint64		first = 0;
	uint32		n = 0;

	for (int k = 0; k < BMAP_TREE_TOPS; k++)
	{
		if (BlockNumberIsValid(tops[k].block))
			items[n++] = queued(tops[k].block, (uint16) k, (BlockNumber) first,
								tops[k].since);
		first += tree_span(k);
	}
	return n;
}

/*
 * A relation this transaction drops at commit whose storage was queued
 * before the commit record (bmapprecommitdrop()), and the snapshots that
 * were queued with it: its drop then queues only what was left, the
 * snapshots whose takers hadn't committed (those may be let go of by their
 * takers meanwhile, or come to be kept, so they wait for the drop itself).
 */
typedef struct PreQueuedDrop
{
	RelFileLocator rel;
	int			nsnaps;
	uint64		snaps[BMAP_TREE_SNAPSHOTS];
} PreQueuedDrop;

static List *prequeued = NIL;	/* PreQueuedDrop *, in TopMemoryContext */
static FullTransactionId prequeued_xid;	/* the transaction they are of */
static bool drop_locked = false;	/* DROP_LOCK held, for drop_locked_xid */
static TransactionId drop_locked_xid;

/*
 * The relation's drop queued before this transaction's commit record, if
 * it was and that transaction has committed: an abort's drops (of storage
 * made in it) queue everything, as the entries queued before are let go of
 * with nothing released.
 */
static PreQueuedDrop *
prequeued_find(RelFileLocator rel)
{
	FullTransactionId top = GetTopFullTransactionIdIfAny();

	if (prequeued == NIL || !FullTransactionIdIsValid(top) ||
		!FullTransactionIdEquals(top, prequeued_xid) ||
		!TransactionIdDidCommit(XidFromFullTransactionId(top)))
		return NULL;
	foreach_ptr(PreQueuedDrop, d, prequeued)
	{
		if (RelFileLocatorEquals(d->rel, rel))
			return d;
	}
	return NULL;
}

static bool
prequeued_has(const PreQueuedDrop *d, uint64 id)
{
	for (int i = 0; i < d->nsnaps; i++)
		if (d->snaps[i] == id)
			return true;
	return false;
}

/*
 * Queue what a map holds (root's subtrees and its snapshots', and its
 * changes list's blocks), under its header's lock, the entries naming
 * 'owner' if valid.  With 'done', only the snapshots that one didn't
 * queue; with 'record' (and an owner), only the snapshots whose takers
 * have committed (or are too old to say), recorded there.  The changes
 * list's blocks first, in as many records as they take, and the subtrees
 * last, in one record of their own: a failure before it leaves nothing
 * queued that the root page still reaches (the blocks queued are listed by
 * the changes list too, so counted twice, which keeps them).  Returns the
 * end of the last record, if any.
 */
static XLogRecPtr
queue_map(SMgrRelation reln, const BmapRoot *root, FullTransactionId owner,
		  const PreQueuedDrop *done, PreQueuedDrop *record)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	BmapQueued *items;
	uint32		n = 0;
	BmapChangesHead head;
	uint32		nchanges = 0;
	BmapChange *changes = NULL;
	XLogRecPtr	end;

	if (done == NULL)
		changes = changes_read(reln, InvalidBuffer, &head, &nchanges);
	items = palloc_array(BmapQueued, Max(BMAP_TREE_TOPS * (1 + BMAP_TREE_SNAPSHOTS),
										 changes ? nchanges : 0));
	if (changes != NULL)
	{
		BlockNumber *held = palloc_array(BlockNumber, Max(nchanges, 1));
		uint32		nheld = changes_blocks(changes, nchanges, held);

		for (uint32 i = 0; i < nheld; i++)
			items[n++] = queued(held[i], BMAP_QUEUED_BLOCK, 0, InvalidXLogRecPtr);
		pfree(held);
		pfree(changes);
	}
	for (uint32 i = 0; i < n && FullTransactionIdIsValid(owner); i++)
		queued_set_owner(&items[i], owner);
	(void) queue_push(rel, items, n);
	INJECTION_POINT("bmap-drop-changes-queued", NULL);

	n = done == NULL ? queued_tops(root->top, items) : 0;
	if (record != NULL)
		record->nsnaps = 0;
	for (int s = 0; s < BMAP_TREE_SNAPSHOTS; s++)
	{
		OwnerState	state;

		if (root->snap[s].id == 0 ||
			(done != NULL && prequeued_has(done, root->snap[s].id)))
			continue;
		if (record != NULL)
		{
			state = owner_state(FullTransactionIdFromU64(root->snap[s].owner));
			if (state == OWNER_RUNNING || state == OWNER_ENDED)
				continue;
			record->snaps[record->nsnaps++] = root->snap[s].id;
		}
		n += queued_tops(snapshot_tops(&root->snap[s]), items + n);
	}
	StaticAssertStmt(BMAP_TREE_TOPS * (1 + BMAP_TREE_SNAPSHOTS) <=
					 (BMAP_QUEUE_PAGES_PER_RECORD - 1) * BMAP_QUEUE_PER_PAGE,
					 "a map's subtrees are queued in one record");
	for (uint32 i = 0; i < n && FullTransactionIdIsValid(owner); i++)
		queued_set_owner(&items[i], owner);
	end = queue_push(rel, items, n);
	pfree(items);
	return end;
}

/*
 * drop_blocks(): the map's subtrees, its snapshots' and its changes list's
 * blocks queued, to be let go of in the background.  The map stays until
 * the relation is unlinked, and a sweep reads it until then (see
 * sweep_relation()), but once queued the nodes it points to may be freed,
 * and made something else: so its root page is written out with no
 * subtrees, under the header's lock (which a sweep reads it under), and
 * with no record, the relation's drop being logged already (redo removes
 * the map).  The queue's records come first, under the same lock: a sweep
 * that reads the map with its subtrees gone reads the queue after, with
 * them.  A relation dropped at a commit had them queued before the commit
 * record, as the transaction's (bmapprecommitdrop()): then only the
 * snapshots left out then are queued.
 */
static void
tree_drop(SMgrRelation reln)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	BmapRoot	root;
	Buffer		headerbuf = map_read(rel, 0);
	Buffer		buf;
	PGIOAlignedBlock copy;
	void	   *pages[1] = {copy.data};
	PreQueuedDrop *done = prequeued_find(rel);

	/* (a queue to make takes a lock, not to be waited for under the header) */
	ensure_queue(rel);
	LockBuffer(headerbuf, BUFFER_LOCK_EXCLUSIVE);
	buf = map_read(rel, 1);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(buf), BMAP_PAGE_ROOT, rel, BMAP_FORKNUM, 1);
	root = *PageGetBmapRoot(BufferGetPage(buf));
	UnlockReleaseBuffer(buf);

	/*
	 * The drop's commit is flushed, these records maybe not: the WAL writer
	 * is asked to flush them soon, not waited for.  A crash first leaves
	 * the blocks held by nothing, as a crash before the push does, for the
	 * sweep a restart makes due (bmap_sweep_due()).  That is only what a
	 * commit's drop didn't queue before its commit record, or an abort's.
	 */
	{
		XLogRecPtr	end = queue_map(reln, &root, InvalidFullTransactionId,
									done, NULL);

		if (!XLogRecPtrIsInvalid(end))
			XLogSetAsyncXactLSN(end);
		else if (!XLogRecPtrIsInvalid(XactLastRecEnd))
			XLogSetAsyncXactLSN(XactLastRecEnd);
	}
	if (done != NULL)
	{
		prequeued = list_delete_ptr(prequeued, done);
		pfree(done);
	}

	buf = map_read(rel, 1);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	for (int k = 0; k < BMAP_TREE_TOPS; k++)
		PageGetBmapRoot(BufferGetPage(buf))->top[k].block = InvalidBlockNumber;
	for (int s = 0; s < BMAP_TREE_SNAPSHOTS; s++)
		PageGetBmapRoot(BufferGetPage(buf))->snap[s].id = 0;
	memcpy(copy.data, BufferGetPage(buf), BLCKSZ);
	PageSetChecksum((Page) copy.data, 1);
	mdwritev(reln, BMAP_FORKNUM, 1, (const void **) pages, 1, false);
	UnlockReleaseBuffer(buf);
	UnlockReleaseBuffer(headerbuf);
}

/*
 * Before the commit record of a transaction that drops reln at commit:
 * queue what its map holds, each entry naming the transaction, so that a
 * crash after the commit record finds the entries there to drain, not the
 * blocks held by nothing.  Until the transaction commits the entries hold
 * nothing (the map still does), and if it doesn't, they are let go of with
 * nothing released (release_step()); a sweep counts them only once it has
 * committed.  The snapshots whose takers haven't committed are left to the
 * drop.  Nothing for a transaction with no id, which wrote no catalog.
 */
void
bmapprecommitdrop(SMgrRelation reln)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	FullTransactionId top = GetTopFullTransactionIdIfAny();
	Buffer		headerbuf;
	Buffer		buf;
	BmapRoot	root;
	PreQueuedDrop *d;
	MemoryContext old;

	if (!FullTransactionIdIsValid(top) || RecoveryInProgress() ||
		!mdexists(reln, BMAP_FORKNUM) || mdnblocks(reln, BMAP_FORKNUM) == 0)
		return;
	/* (what an earlier transaction left, had its end not let go of it) */
	if (!FullTransactionIdEquals(top, prequeued_xid))
	{
		bmapenddrops();
		prequeued_xid = top;
	}
	foreach_ptr(PreQueuedDrop, q, prequeued)
	{
		if (RelFileLocatorEquals(q->rel, rel))
			return;
	}

	/* (held past the commit, until the drops are done) */
	if (!drop_locked)
	{
		LOCKTAG		tag;

		DROP_LOCK(tag, XidFromFullTransactionId(top));
		(void) LockAcquire(&tag, ExclusiveLock, true, false);
		drop_locked = true;
		drop_locked_xid = XidFromFullTransactionId(top);
	}

	ensure_queue(rel);
	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_EXCLUSIVE);
	if (PageIsNew(BufferGetPage(headerbuf)))
	{
		UnlockReleaseBuffer(headerbuf);
		return;
	}
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	buf = map_read(rel, 1);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(buf), BMAP_PAGE_ROOT, rel, BMAP_FORKNUM, 1);
	root = *PageGetBmapRoot(BufferGetPage(buf));
	UnlockReleaseBuffer(buf);

	old = MemoryContextSwitchTo(TopMemoryContext);
	d = palloc0(sizeof(PreQueuedDrop));
	MemoryContextSwitchTo(old);
	d->rel = rel;
	PG_TRY();
	{
		(void) queue_map(reln, &root, top, NULL, d);
	}
	PG_CATCH();
	{
		pfree(d);
		PG_RE_THROW();
	}
	PG_END_TRY();
	old = MemoryContextSwitchTo(TopMemoryContext);
	prequeued = lappend(prequeued, d);
	MemoryContextSwitchTo(old);
	UnlockReleaseBuffer(headerbuf);
	INJECTION_POINT("bmap-drop-prequeued", NULL);
}

/*
 * As a top-level transaction's drops are done (their relations' buffers
 * dropped, before their files are unlinked), at commit or abort: the
 * entries it queued before its commit record (bmapprecommitdrop()) are
 * drained from now on, if it committed.  Called again as the transaction's
 * deletes end, which finds nothing left to do.
 */
void
bmapenddrops(void)
{
	LOCKTAG		tag;

	list_free_deep(prequeued);
	prequeued = NIL;
	if (!drop_locked)
		return;
	DROP_LOCK(tag, drop_locked_xid);

	/* (an abort lets go of session locks too, before its drops) */
	if (LockHeldByMe(&tag, ExclusiveLock, false))
		LockRelease(&tag, ExclusiveLock, true);
	drop_locked = false;
	INJECTION_POINT("bmap-drops-done", NULL);
}

/*
 * Let go of every physical block a dropped relation's map or changes list
 * holds: queued, for a drain to free those no other map holds and count
 * the others down.  The records name only the queue's pages: a standby has
 * removed the relation's map by the time it replays them.  Runs after
 * commit or abort, so it must not fail: a problem is reported and the
 * blocks are left allocated, for a sweep.
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
		tree_drop(reln);
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

/* a block popped, and how many of the entries popped hold it */
typedef struct Popped
{
	BlockNumber block;
	uint32		holders;
} Popped;

static int
cmp_popped(const void *a, const void *b)
{
	return pg_cmp_u32(((const Popped *) a)->block, ((const Popped *) b)->block);
}

/* is 'page' one of pages[0 .. *n), or is there room to add it? */
static bool
page_fits(BlockNumber page, BlockNumber *pages, int *n, int max)
{
	for (int i = 0; i < *n; i++)
		if (pages[i] == page)
			return true;
	if (*n == max)
		return false;
	pages[(*n)++] = page;
	return true;
}

static int
cmp_queued_block(const void *a, const void *b)
{
	return pg_cmp_u32(((const BmapQueued *) a)->block,
					  ((const BmapQueued *) b)->block);
}

/*
 * A node popped: its children, as entries, into children[]; their count,
 * which the caller pushes if the node is freed.  Shared, the node changes
 * no more; held by the queue alone, nothing else reaches it.  One that
 * isn't the incarnation its entry names (-1) is reported, and its entry
 * dropped with nothing let go of: what it held is left for a sweep.
 */
static int
queued_children(RelFileLocator rel, const BmapQueued *node,
				BmapQueued *children)
{
	Buffer		buf = tree_read(rel, node->block, RBM_NORMAL);
	const BmapEntry *entries;
	int			n = 0;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	if (!tree_node_is(BufferGetPage(buf), node->level, node->first, node->since))
	{
		UnlockReleaseBuffer(buf);
		ereport(WARNING,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("block %u of the page store of database %u is not the node its release queue entry names",
						node->block, rel.dbOid),
				 errdetail("What the node held is left allocated, for a page sweep.")));
		return -1;
	}
	entries = PageGetBmapEntries(BufferGetPage(buf));
	for (uint32 i = 0; i < BMAP_TREE_FANOUT; i++)
	{
		if (!BlockNumberIsValid(entries[i].block))
			continue;
		if (node->level == 0)
			children[n++] = queued(entries[i].block, BMAP_QUEUED_BLOCK, 0,
								   InvalidXLogRecPtr);
		else
			children[n++] = queued(entries[i].block, node->level - 1,
								   (BlockNumber) Min((uint64) node->first +
													 i * tree_span(node->level - 1),
													 (uint64) MaxBlockNumber),
								   entries[i].since);
	}
	UnlockReleaseBuffer(buf);

	/*
	 * A leaf holds each block it maps once, however many of its entries map
	 * it (as tree_copy_node() counts it): each is queued once.
	 */
	if (node->level == 0 && n > 1)
	{
		int			u = 0;

		qsort(children, n, sizeof(BmapQueued), cmp_queued_block);
		for (int i = 0; i < n; i++)
			if (u == 0 || children[u - 1].block != children[i].block)
				children[u++] = children[i];
		n = u;
	}
	return n;
}

/*
 * One step of a drain: entries popped from the top of the queue, as many as
 * one record lets go of (up to and including the first node), and let go
 * of, all in one record.  An entry whose drop didn't commit (queued_state())
 * is popped with nothing let go of; one whose drop is still to commit stops
 * the step there.  False if the queue is empty, or its top waits for its
 * drop to commit.
 */
static bool
release_step(RelFileLocator rel, BmapRelease *done)
{
	uint32		n;
	bool		isnew;
	Buffer		metabuf;
	BmapQueued	popped[BMAP_RELEASE_STEP];
	Popped		blocks[BMAP_RELEASE_STEP];
	BlockNumber sorted[BMAP_RELEASE_STEP];
	BlockNumber tofree[BMAP_RELEASE_STEP];
	uint32		k = 0;
	uint32		ndiscard = 0;
	uint32		npop;
	uint32		nblocks = 0;
	uint32		nfree = 0;
	uint32		freed = 0;
	uint32		unrefs = 0;
	BmapQueued *children = NULL;
	int			nchildren = 0;
	uint32		npushed = 0;
	uint32		nlet = 0;
	const BmapQueued *node = NULL;
	BlockNumber nrefpages;
	RefsPages	rp;
	QueuePages	qp;
	Buffer		allocbuf = InvalidBuffer;
	BitsChange	bits[BMAP_RELEASE_BITS_PAGES];
	int			nbits = 0;
	int			nblockids;
	xl_bmap_meta_delta adelta;
	xl_bmap_queue_meta_delta mdelta;
	xl_bmap_release xlrec;
	XLogRecPtr	lsn;
	int			id;

	metabuf = queue_lock_meta(rel, &n, &isnew);
	if (n == 0)
	{
		UnlockReleaseBuffer(metabuf);
		return false;
	}

	/*
	 * The entries from the top, until one is a node (whose children may
	 * take its place) or their blocks would lie on more count or bitmap
	 * pages than a record changes.
	 */
	{
		BlockNumber refpages[BMAP_RELEASE_REFS_PAGES];
		BlockNumber bitpages[BMAP_RELEASE_BITS_PAGES];
		int			nref = 0;
		int			nbit = 0;
		Buffer		buf = InvalidBuffer;
		BlockNumber cur = InvalidBlockNumber;
		FullTransactionId last_owner = InvalidFullTransactionId;
		OwnerState	last_state = OWNER_NONE;

		while (k + ndiscard < BMAP_RELEASE_STEP && k + ndiscard < n)
		{
			uint32		pos = n - 1 - k - ndiscard;
			BmapQueued	e;
			int			saveref = nref;
			FullTransactionId owner;
			OwnerState	state = OWNER_NONE;

			if (QUEUE_PAGE(pos) != cur)
			{
				if (BufferIsValid(buf))
					UnlockReleaseBuffer(buf);
				cur = QUEUE_PAGE(pos);
				buf = queue_read(rel, cur);
				LockBuffer(buf, BUFFER_LOCK_SHARE);
				check_page(BufferGetPage(buf), BMAP_PAGE_QUEUE,
						   with_relnumber(rel, BMAP_QUEUE_RELNUMBER),
						   MAIN_FORKNUM, cur);
			}
			e = PageGetBmapQueued(BufferGetPage(buf))[QUEUE_SLOT(pos)];

			/* (a drop's entries are together, of one owner) */
			owner = queued_owner(&e);
			if (FullTransactionIdIsValid(owner))
			{
				if (!FullTransactionIdEquals(owner, last_owner))
				{
					last_owner = owner;
					last_state = queued_state(&e);
				}
				state = last_state;
			}
			if (state == OWNER_RUNNING)
				break;
			if (!queued_holds(state))
			{
				ndiscard++;
				continue;
			}
			if (!page_fits(REF_PAGE(e.block), refpages, &nref,
						   BMAP_RELEASE_REFS_PAGES))
				break;
			if (!page_fits(e.block / BMAP_BITS_PER_PAGE, bitpages, &nbit,
						   BMAP_RELEASE_BITS_PAGES))
			{
				nref = saveref;
				break;
			}
			popped[k++] = e;
			if (e.level != BMAP_QUEUED_BLOCK)
			{
				node = &popped[k - 1];
				break;
			}
		}
		if (BufferIsValid(buf))
			UnlockReleaseBuffer(buf);
	}
	npop = k + ndiscard;
	if (npop == 0)
	{
		UnlockReleaseBuffer(metabuf);
		return false;
	}

	/* a node's children, read first (an entry not naming its node: none) */
	nlet = k;
	if (node != NULL)
	{
		children = palloc_array(BmapQueued, BMAP_TREE_FANOUT);
		nchildren = queued_children(rel, node, children);
		if (nchildren < 0)
		{
			nlet = k - 1;
			node = NULL;
		}
	}

	/* the blocks, each with how many of the entries popped hold it */
	for (uint32 i = 0; i < nlet; i++)
	{
		blocks[i].block = popped[i].block;
		blocks[i].holders = 1;
	}
	qsort(blocks, nlet, sizeof(Popped), cmp_popped);
	for (uint32 i = 0; i < nlet; i++)
	{
		if (nblocks > 0 && blocks[nblocks - 1].block == blocks[i].block)
			blocks[nblocks - 1].holders++;
		else
			blocks[nblocks++] = blocks[i];
	}

	/*
	 * Their counts, those holders fewer; a block they leave held by nothing
	 * is freed (blocks past the counts' pages count zero).
	 */
	nrefpages = refs_nblocks(rel);
	rp.n = 0;
	{
		uint32		ncounted = 0;

		for (uint32 i = 0; i < nblocks; i++)
			if (REF_PAGE(blocks[i].block) < nrefpages)
				sorted[ncounted++] = blocks[i].block;
		if (ncounted > 0 && refs_lock(rel, sorted, ncounted, &rp) != ncounted)
			elog(ERROR, "a release queue step locks too many count pages");
	}
	for (uint32 i = 0; i < nblocks; i++)
	{
		BlockNumber b = blocks[i].block;
		uint32		h = blocks[i].holders;
		uint8		c = REF_PAGE(b) < nrefpages ? refs_get(&rp, b) : 0;

		if (c == BMAP_REFS_STUCK)
		{
			for (uint32 j = 0; j < h; j++)
				stuck_released(rel);
			continue;
		}
		if (c >= h)
		{
			refs_set_counted(&rp, b, c - h,
							 PGSTAT_PROLLY_BMAP_RELEASE_REFCOUNT_UPDATES);
			unrefs++;
			continue;
		}
		/* (the entries were all the holders it had) */
		Assert(c + 1 == h);
		if (c > 0)
			refs_set_counted(&rp, b, 0,
							 PGSTAT_PROLLY_BMAP_RELEASE_REFCOUNT_UPDATES);
		tofree[nfree++] = b;
		if (node != NULL && b == node->block)
			npushed = (uint32) nchildren;
	}

	/* the frees: a bit cleared for each, if set (a block is freed once) */
	if (nfree > 0)
	{
		BmapMeta   *meta;
		uint32		hint;

		allocbuf = alloc_read(rel, 0);
		LockBuffer(allocbuf, BUFFER_LOCK_EXCLUSIVE);
		check_page(BufferGetPage(allocbuf), BMAP_PAGE_META, rel, MAIN_FORKNUM, 0);
		meta = PageGetBmapMeta(BufferGetPage(allocbuf));
		hint = meta->hint;
		for (uint32 i = 0; i < nfree; i++)
		{
			uint32		pageno = tofree[i] / BMAP_BITS_PER_PAGE;
			uint32		bit = tofree[i] % BMAP_BITS_PER_PAGE;
			BitsChange *bc = NULL;
			uint64	   *words;

			if (pageno >= meta->nbitmap || tofree[i] >= meta->hw)
				elog(ERROR, "block-mapped storage of database %u frees block %u past its end",
					 rel.dbOid, tofree[i]);
			for (int j = 0; j < nbits; j++)
				if (bits[j].pageno == pageno)
					bc = &bits[j];
			if (bc == NULL)
			{
				Buffer		buf;

				Assert(nbits < BMAP_RELEASE_BITS_PAGES);
				buf = alloc_read(rel, meta->bitmap[pageno]);
				LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
				check_page(BufferGetPage(buf), BMAP_PAGE_BITMAP, rel,
						   MAIN_FORKNUM, meta->bitmap[pageno]);
				bc = bits_change(bits, &nbits, buf, pageno, 0, false, nfree);
			}
			words = PageGetBmapBits(BufferGetPage(bc->buf));
			if (words[bit / 64] & (UINT64CONST(1) << (bit % 64)))
			{
				((uint32 *) (bc->delta + 1))[bc->delta->nbits++] = bit;
				freed++;
			}
			hint = Min(hint, pageno);
		}
		adelta.kind = BMAP_DELTA_META;
		adelta.flags = 0;
		adelta.hw = meta->hw;
		adelta.nfree = meta->nfree + freed;
		adelta.hint = hint;
		adelta.nbitmap_added = 0;
	}

	/* a node freed: its children, in the place of the entries popped */
	qp.n = 0;
	if (npushed > 0)
	{
		queue_cover(rel, QUEUE_PAGE(n - npop + npushed - 1) + 1);
		queue_write(rel, &qp, n - npop, children, npushed);
	}

	xlrec.locator = with_relnumber(rel, BMAP_QUEUE_RELNUMBER);
	xlrec.op = BMAP_RELEASE_DRAIN;
	xlrec.npopped = npop;
	nblockids = 1 + qp.n + rp.n + (nfree > 0 ? 1 + nbits : 0);
	XLogEnsureRecordSpace(nblockids - 1, 1 + nblockids);
	INJECTION_POINT("bmap-release-step", NULL);
	START_CRIT_SECTION();
	XLogBeginInsert();
	XLogRegisterData(&xlrec, sizeof(xlrec));
	register_queue_meta(metabuf, isnew, &mdelta, n - npop + npushed);
	id = queue_apply_register(&qp, 1);
	id = refs_apply_register(&rp, id);
	if (nfree > 0)
	{
		apply_meta_delta(BufferGetPage(allocbuf), &adelta);
		MarkBufferDirty(allocbuf);
		XLogRegisterBuffer(id, allocbuf, REGBUF_STANDARD);
		XLogRegisterBufData(id, &adelta, sizeof(adelta));
		id++;
		for (int j = 0; j < nbits; j++)
		{
			apply_bits_delta(BufferGetPage(bits[j].buf), bits[j].delta);
			MarkBufferDirty(bits[j].buf);
			XLogRegisterBuffer(id, bits[j].buf, REGBUF_STANDARD);
			XLogRegisterBufData(id, bits[j].delta, bits_delta_size(bits[j].delta));
			id++;
		}
	}
	lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_RELEASE);
	PageSetLSN(BufferGetPage(metabuf), lsn);
	queue_set_lsn(&qp, lsn);
	refs_set_lsn(&rp, lsn);
	if (nfree > 0)
	{
		PageSetLSN(BufferGetPage(allocbuf), lsn);
		for (int j = 0; j < nbits; j++)
			PageSetLSN(BufferGetPage(bits[j].buf), lsn);
	}
	END_CRIT_SECTION();

	for (int j = 0; j < nbits; j++)
	{
		UnlockReleaseBuffer(bits[j].buf);
		pfree(bits[j].delta);
	}
	if (BufferIsValid(allocbuf))
		UnlockReleaseBuffer(allocbuf);
	queue_release(&qp);
	refs_release(&rp);
	UnlockReleaseBuffer(metabuf);
	if (children)
		pfree(children);

	/* (what a scan found in them no longer holds) */
	for (uint32 i = 0; i < nfree; i++)
		store_written(rel, tofree[i], 1);

	pgstat_count_prolly(PGSTAT_PROLLY_BMAP_RELEASE_DRAINED, npop);
	pgstat_count_prolly(PGSTAT_PROLLY_BMAP_RELEASE_BLOCKS_FREED, freed);
	pgstat_count_prolly(PGSTAT_PROLLY_BMAP_RELEASE_QUEUED, npushed);
	done->entries += npop;
	done->discarded += ndiscard;
	done->freed += freed;
	done->unrefs += unrefs;
	return true;
}

/*
 * Take one step of the drain of a database's release queue in a
 * tablespace, adding what it let go of to *done.  False if it took none:
 * the queue is empty, or (unless 'wait') another backend drains it or a
 * sweep counts what it holds.  In a transaction.
 */
bool
bmap_release_step(Oid spcOid, Oid dbOid, bool wait, BmapRelease *done)
{
	RelFileLocator rel = {spcOid, dbOid, InvalidRelFileNumber};
	LOCKTAG		draintag;
	LOCKTAG		sweeptag;
	bool		stepped;

	if (RecoveryInProgress() || !alloc_exists(rel) || !queue_exists(rel))
		return false;
	DRAIN_LOCK(draintag, rel);
	if (LockAcquire(&draintag, ExclusiveLock, false, !wait) == LOCKACQUIRE_NOT_AVAIL)
		return false;
	SWEEP_LOCK(sweeptag, rel);
	if (LockAcquire(&sweeptag, ShareLock, false, !wait) == LOCKACQUIRE_NOT_AVAIL)
	{
		LockRelease(&draintag, ExclusiveLock, false);
		return false;
	}
	stepped = release_step(rel, done);
	LockRelease(&sweeptag, ShareLock, false);
	LockRelease(&draintag, ExclusiveLock, false);
	return stepped;
}

/*
 * Drain the release queue, waiting for another drain or a sweep, until it
 * is empty or (max_entries > 0) that many entries were popped.
 */
void
bmap_release_drain(Oid spcOid, Oid dbOid, uint64 max_entries,
				   BmapRelease *done)
{
	while ((max_entries == 0 || done->entries < max_entries) &&
		   bmap_release_step(spcOid, dbOid, true, done))
		CHECK_FOR_INTERRUPTS();
}

/* How many entries the release queue holds. */
uint64
bmap_release_pending(Oid spcOid, Oid dbOid)
{
	RelFileLocator rel = {spcOid, dbOid, InvalidRelFileNumber};
	Buffer		buf;
	uint64		n = 0;

	if (!queue_exists(rel))
		return 0;
	buf = queue_read(rel, 0);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	if (!PageIsNew(BufferGetPage(buf)))
	{
		check_page(BufferGetPage(buf), BMAP_PAGE_QUEUE_META,
				   with_relnumber(rel, BMAP_QUEUE_RELNUMBER), MAIN_FORKNUM, 0);
		n = PageGetBmapQueueMeta(BufferGetPage(buf))->nentries;
	}
	UnlockReleaseBuffer(buf);
	return n;
}

/*
 * A sweep counts what the queue holds, as it would a map's: each entry a
 * holder of its block, a node's subtree counted as tree_walk() counts it.
 * The sweep holds drains off, so entries are only pushed meanwhile, and
 * those pushed after this read are of maps the sweep read before they were
 * dropped (a drop pushes before it lets go of its root page, under its
 * header's lock).  An entry of a drop yet to commit holds nothing (its map,
 * read before while the drop was yet to commit, still held what it names),
 * nor does one of a drop that didn't, or is too old to say of.  One of a
 * committed drop holds, its drop done or not: the drop may have emptied
 * its map's root page before the map was read (counted twice, if not,
 * which keeps them).
 */
static void
sweep_queue(RelFileLocator rel, TreeNodeVisit visit_node,
			TreeEntryVisit visit_entry, void *arg)
{
	Buffer		metabuf;
	uint32		n = 0;
	BmapQueued *entries;
	SMgrRelation store = store_smgr(rel);
	FullTransactionId last_owner = InvalidFullTransactionId;
	OwnerState	last_state = OWNER_NONE;

	if (!queue_exists(rel))
		return;
	metabuf = queue_read(rel, 0);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	if (!PageIsNew(BufferGetPage(metabuf)))
	{
		check_page(BufferGetPage(metabuf), BMAP_PAGE_QUEUE_META,
				   with_relnumber(rel, BMAP_QUEUE_RELNUMBER), MAIN_FORKNUM, 0);
		n = PageGetBmapQueueMeta(BufferGetPage(metabuf))->nentries;
	}
	entries = MemoryContextAllocHuge(CurrentMemoryContext,
									 Max(n, 1) * sizeof(BmapQueued));
	for (uint32 i = 0; i < n;)
	{
		Buffer		buf = queue_read(rel, QUEUE_PAGE(i));
		uint32		take = Min(n - i, BMAP_QUEUE_PER_PAGE - QUEUE_SLOT(i));

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		check_page(BufferGetPage(buf), BMAP_PAGE_QUEUE,
				   with_relnumber(rel, BMAP_QUEUE_RELNUMBER), MAIN_FORKNUM,
				   QUEUE_PAGE(i));
		memcpy(entries + i, PageGetBmapQueued(BufferGetPage(buf)) + QUEUE_SLOT(i),
			   take * sizeof(BmapQueued));
		UnlockReleaseBuffer(buf);
		i += take;
	}
	UnlockReleaseBuffer(metabuf);

	for (uint32 i = 0; i < n; i++)
	{
		BmapEntry	ptr;
		FullTransactionId owner = queued_owner(&entries[i]);

		/* (a drop's entries are together, of one owner) */
		if (FullTransactionIdIsValid(owner) &&
			!FullTransactionIdEquals(owner, last_owner))
		{
			last_owner = owner;
			last_state = owner_state(owner);
		}
		if (FullTransactionIdIsValid(owner) && last_state != OWNER_COMMITTED)
			continue;
		ptr.block = entries[i].block;
		ptr.epoch = 0;
		ptr.since = entries[i].since;
		if (entries[i].level == BMAP_QUEUED_BLOCK)
			visit_entry(&ptr, InvalidBlockNumber, arg);
		else
			tree_walk_node(store, &ptr, entries[i].level, entries[i].first,
						   true, visit_node, visit_entry, arg);
	}
	pfree(entries);
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

typedef struct SweepHolders
{
	uint16	   *holders;
	uint64	   *reached;		/* the nodes reached, a bit each */
	BlockNumber hw;
} SweepHolders;

/*
 * A tree's node, one holder more: one for each page that points to it.  Its
 * children are counted only the first time it is reached, as it holds each
 * once, whichever relations reach it.  That it was reached is kept apart
 * from its holders: a dropped relation's changes list, read until its map
 * is unlinked, may still list the block, freed and made a node since, and
 * the node's children are counted all the same.  (A node past the blocks
 * the sweep may free is read again each time: counting more holders is
 * safe.)
 */
static bool
sweep_node(BlockNumber node, uint16 level, BlockNumber first, void *arg)
{
	SweepHolders *sh = arg;
	uint64		bit = UINT64CONST(1) << (node % 64);

	if (node >= sh->hw)
		return true;
	hold(sh->holders, node);
	if (sh->reached[node / 64] & bit)
		return false;
	sh->reached[node / 64] |= bit;
	return true;
}

static void
sweep_entry(const BmapEntry *entry, BlockNumber blkno, void *arg)
{
	SweepHolders *sh = arg;

	if (entry->block < sh->hw)
		hold(sh->holders, entry->block);
}

/*
 * Count, in holders[], each physical block a relation holds: its map's
 * entries (those past its length too; a tree's nodes too) and its changes list's old blocks.
 * Its header is held exclusively meanwhile, so no extension, remap,
 * truncation or prune of it runs: a block they allocated is mapped or
 * listed by the time it is read.
 */
static void
sweep_relation(SMgrRelation reln, uint16 *holders, uint64 *reached,
			   BlockNumber hw)
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
	{
		SweepHolders sh = {holders, reached, hw};

		tree_walk(reln, sweep_node, sweep_entry, &sh);
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
 * crash after a drop's commit or part way through a prune), what the
 * release queue holds let go of first and the rest counted, rewrite the
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

/* ----------------------------------------------------------------
 * Compaction: the blocks past where the page store would end moved down
 * ----------------------------------------------------------------
 */

/*
 * Freeing leaves holes where the freed blocks were (a vacuumed change
 * log's, among its table's).  The store keeps them until later allocations
 * fill them: a sweep can shorten it only to its last allocated block.  So
 * after counting, a sweep moves the blocks past the allocated count down,
 * each to the lowest free block, and then shortens the store past them
 * (sweep_shrink()).  It moves only data pages one relation alone holds, its
 * leaf and every node above it the relation's own, each as a copy before a
 * change would move it, which redo replays the same way: copied to the new
 * block by a REMAP (the page's image, the leaf's entry, the allocation)
 * under its buffer's cleanup lock, then the old block freed.  Its entry
 * keeps its epoch, so a page as of a fold stays one.  The image restores
 * the page whatever records redo skips for it from before the move.
 *
 * A tree's node the relation owns, its path too, moves after them, as a new
 * incarnation at the lower block, its parent pointed to it
 * (compact_nodes()): recovery skips the records of every page under a
 * pointer it can't follow (tree_find()), so the copy is written and synced
 * before the record that points to it, and the parents are written and
 * synced before the old blocks are freed, so that redo from any point
 * follows a pointer to a version of the node.  Not while an online backup
 * runs, which could copy a parent after the move and the copy's block
 * before it was written; one starting meanwhile fails at its stop.
 * Anything shared, a snapshot's, on a changes list, or busy (a pinned
 * buffer) stays where it is.  A crash between a move and the freeing of
 * its old block leaves that block allocated and held by nothing, for the
 * next sweep.  At wal_level minimal, a relation whose data skips WAL (made
 * in a transaction not yet committed) isn't moved: its later changes reach
 * its blocks without records, and replaying the move's image would undo
 * them.
 */

/* a data page to move */
typedef struct CompactMove
{
	BlockNumber phys;
	BlockNumber blkno;
} CompactMove;

/* a tree's node to move */
typedef struct CompactNode
{
	BlockNumber phys;
	uint16		level;
	BlockNumber first;
} CompactNode;

typedef struct CompactWalk
{
	BlockNumber target;			/* move the blocks from here */
	BlockNumber nblocks;		/* the relation's length */
	CompactMove *moves;
	uint32		n;
	uint32		cap;
	CompactNode *nodes;
	uint32		nnodes;
	uint32		capnodes;
} CompactWalk;

static void
compact_note(CompactWalk *cw, BlockNumber phys, BlockNumber blkno)
{
	if (cw->n == cw->cap)
	{
		cw->cap = Max(cw->cap * 2, 64);
		cw->moves = cw->moves ?
			repalloc_array(cw->moves, CompactMove, cw->cap) :
			palloc_array(CompactMove, cw->cap);
	}
	cw->moves[cw->n].phys = phys;
	cw->moves[cw->n].blkno = blkno;
	cw->n++;
}

static bool
compact_visit_node(BlockNumber node, uint16 level, BlockNumber first, void *arg)
{
	CompactWalk *cw = arg;

	if (node >= cw->target)
	{
		if (cw->nnodes == cw->capnodes)
		{
			cw->capnodes = Max(cw->capnodes * 2, 16);
			cw->nodes = cw->nodes ?
				repalloc_array(cw->nodes, CompactNode, cw->capnodes) :
				palloc_array(CompactNode, cw->capnodes);
		}
		cw->nodes[cw->nnodes].phys = node;
		cw->nodes[cw->nnodes].level = level;
		cw->nodes[cw->nnodes].first = first;
		cw->nnodes++;
	}
	return true;
}

static void
compact_visit_entry(const BmapEntry *entry, BlockNumber blkno, void *arg)
{
	CompactWalk *cw = arg;

	/* (not a snapshot's, nor one past the relation's length) */
	if (entry->block >= cw->target && BlockNumberIsValid(blkno) &&
		blkno < cw->nblocks)
		compact_note(cw, entry->block, blkno);
}

/*
 * Has rel's page store a free block below 'below'?  Read under the
 * allocator's share locks: an allocation may take it meanwhile, which the
 * move then finds (alloc_take() gives it another).
 */
static bool
compact_has_room(RelFileLocator rel, BlockNumber below)
{
	Buffer		metabuf = alloc_read(rel, 0);
	BmapMeta   *meta;
	bool		room = false;

	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(metabuf), BMAP_PAGE_META, rel, MAIN_FORKNUM, 0);
	meta = PageGetBmapMeta(BufferGetPage(metabuf));
	if (meta->nfree > 0)
	{
		for (uint32 pageno = meta->hint; pageno < meta->nbitmap && !room &&
			 (BlockNumber) pageno * BMAP_BITS_PER_PAGE < below; pageno++)
		{
			Buffer		buf = alloc_read(rel, meta->bitmap[pageno]);
			uint64	   *words;
			BlockNumber base = pageno * BMAP_BITS_PER_PAGE;

			LockBuffer(buf, BUFFER_LOCK_SHARE);
			check_page(BufferGetPage(buf), BMAP_PAGE_BITMAP, rel,
					   MAIN_FORKNUM, meta->bitmap[pageno]);
			words = PageGetBmapBits(BufferGetPage(buf));
			for (uint32 b = 0; b < BMAP_BITS_PER_PAGE && base + b < below; b++)
			{
				if (b % 64 == 0 && words[b / 64] == PG_UINT64_MAX)
				{
					b += 63;
					continue;
				}
				if ((words[b / 64] & (UINT64CONST(1) << (b % 64))) == 0)
				{
					room = true;
					break;
				}
			}
			UnlockReleaseBuffer(buf);
		}
	}
	UnlockReleaseBuffer(metabuf);
	return room;
}

/*
 * May data page 'blkno' of reln, at physical block 'phys', move?  Its entry
 * still names the block, which nothing else holds, on a path the relation
 * owns.  The header is held, at least shared.
 */
static bool
compact_page_movable(SMgrRelation reln, Buffer headerbuf, BlockNumber blkno,
					 BlockNumber phys, BmapEntry *entry, MapPage *mp)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	BmapHeader *header = PageGetBmapHeader(BufferGetPage(headerbuf));

	return blkno < header->nblocks &&
		lookup_entry_at(reln, blkno, entry, mp, NULL) &&
		entry->block == phys &&
		lookup_ref(rel, phys) == 0 &&
		tree_path_own(reln, blkno, header->epoch);
}

/*
 * Move data page 'blkno' of reln from physical block 'phys' to a lower free
 * block.  True if it moved (and 'phys' is to be freed).  Locks as
 * bmapprepareforchange()'s copy: the page's buffer, the header shared, the
 * allocator's pages, then the leaf last.
 */
static bool
compact_page(SMgrRelation reln, BlockNumber blkno, BlockNumber phys)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		buf;
	Buffer		headerbuf;
	Buffer		mapbuf;
	BmapEntry	entry;
	MapPage		mp;
	Allocation	alloc;
	BlockNumber new_block;
	xl_bmap_remap xlrec;
	xl_bmap_node_delta *ndelta;
	xl_bmap_entry_delta *ent;
	Page		page;
	bool		zeroed;
	int			id;
	XLogRecPtr	lsn;

	if (!compact_has_room(rel, phys))
		return false;
	INJECTION_POINT("bmap-compact-room", NULL);

	/*
	 * Asked first without the page: preparing its buffer for the change
	 * below would copy a page another relation shares, which isn't to be
	 * moved at all.
	 */
	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	if (!compact_page_movable(reln, headerbuf, blkno, phys, &entry, &mp))
	{
		UnlockReleaseBuffer(headerbuf);
		return false;
	}
	LockBuffer(headerbuf, BUFFER_LOCK_UNLOCK);

	/* the page, which nothing else may hold meanwhile */
	buf = ReadBufferWithoutRelcache(rel, MAIN_FORKNUM, blkno, RBM_NORMAL,
									NULL, true);
	if (!ConditionalLockBufferForCleanupUnprepared(buf))
	{
		ReleaseBuffer(buf);
		ReleaseBuffer(headerbuf);
		return false;
	}
	/* (as a change of no row, which claims an entry nothing else holds) */
	BufferPrepareForChange(buf);

	LockBuffer(headerbuf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	if (!compact_page_movable(reln, headerbuf, blkno, phys, &entry, &mp))
	{
		UnlockReleaseBuffer(headerbuf);
		UnlockReleaseBuffer(buf);
		return false;
	}

	alloc_take(rel, 1, &new_block, &alloc);
	if (new_block >= phys)
	{
		/*
		 * The room was taken meanwhile: nothing is changed yet (a page store
		 * grown for it by a block too many is shortened as it ends).
		 */
		alloc_release(&alloc);
		UnlockReleaseBuffer(headerbuf);
		UnlockReleaseBuffer(buf);
		return false;
	}

	page = BufferGetPage(buf);
	zeroed = pg_memory_is_all_zeros(page, BLCKSZ);
	if (new_block < alloc.old_hw && !zeroed)
		drop_node_buffer(rel, new_block);
	prepare_store(rel, alloc.old_hw, alloc.new_hw, &new_block,
				  zeroed && new_block < alloc.old_hw ? 1 : 0, false,
				  PGSTAT_PROLLY_BMAP_STORE_BYTES_WRITTEN, InvalidXLogRecPtr);

	ndelta = palloc0(sizeof(xl_bmap_node_delta) + sizeof(xl_bmap_entry_delta));
	ndelta->kind = BMAP_DELTA_NODE;
	ndelta->nentries = 1;
	ent = (xl_bmap_entry_delta *) (ndelta + 1);
	ent->slot = MP_SLOT(&mp, blkno);
	ent->block = new_block;
	ent->epoch = entry.epoch;

	mapbuf = mp_read(reln, &mp);
	map_lock(mapbuf, false);
	mp_check(reln, &mp, BufferGetPage(mapbuf));

	xlrec.locator = rel;
	xlrec.blkno = blkno;
	xlrec.old_block = phys;
	xlrec.new_block = new_block;
	xlrec.old_hw = alloc.old_hw;
	xlrec.new_hw = alloc.new_hw;
	xlrec.flags = zeroed ? BMAP_REMAP_ZERO : 0;

	XLogEnsureRecordSpace(5 + BMAP_ALLOC_MAX_PAGES, 0);
	ndelta->since = GetXLogInsertRecPtr();
	START_CRIT_SECTION();
	apply_node_delta(BufferGetPage(mapbuf), ndelta);
	MarkBufferDirty(mapbuf);
	/* the page is written to its new block from here on */
	MarkBufferDirty(buf);
	XLogBeginInsert();
	XLogRegisterData(&xlrec, sizeof(xlrec));
	XLogRegisterBuffer(0, mapbuf, REGBUF_STANDARD);
	XLogRegisterBufData(0, ndelta, node_delta_size(ndelta));
	id = alloc_apply_register(&alloc, 1);
	if (zeroed)
		XLogRegisterBuffer(id, buf, REGBUF_WILL_INIT);
	else
		XLogRegisterBuffer(id, buf, REGBUF_FORCE_IMAGE |
						   (page_hole_is_zero(page) ? REGBUF_STANDARD : 0));
	lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_REMAP);
	PageSetLSN(BufferGetPage(mapbuf), lsn);
	alloc_set_lsn(&alloc, lsn);
	if (!zeroed)
		PageSetLSN(page, lsn);
	END_CRIT_SECTION();

	alloc_release(&alloc);
	map_unlock(mapbuf);
	ReleaseBuffer(mapbuf);
	UnlockReleaseBuffer(headerbuf);
	UnlockReleaseBuffer(buf);
	pfree(ndelta);
	return true;
}

/*
 * A tree's node being moved down (compact_relation()): where it is, the
 * copy made of it, and what the copy was made of, to check that the node
 * didn't change before the parent is pointed to the copy.
 */
typedef struct NodeMove
{
	uint16		level;
	BlockNumber first;
	BlockNumber phys;			/* the node's block */
	XLogRecPtr	born;			/* its birth, as its pointer names it */
	XLogRecPtr	lsn;			/* its page's LSN as it was copied */
	BmapEntry	copy;			/* the copy's pointer (block invalid: none) */
	bool		installed;		/* the parent points to the copy */
	TreeParent	parent;			/* the page that points to it, as installed */
} NodeMove;

/*
 * The path down to node 'm' of reln, every node on it the relation's own:
 * the pointer to it into *ptr, the page holding that pointer into *parent.
 * False if it isn't there any more, or isn't the relation's alone.  The
 * header is held exclusively.
 */
static bool
node_move_path(SMgrRelation reln, uint32 epoch, const NodeMove *m,
			   BmapEntry *ptr, TreeParent *parent)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	TreePath	path;
	Buffer		buf;

	tree_path(m->first, &path);
	buf = map_read(rel, 1);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(buf), BMAP_PAGE_ROOT, rel, BMAP_FORKNUM, 1);
	*ptr = PageGetBmapRoot(BufferGetPage(buf))->top[path.top];
	UnlockReleaseBuffer(buf);
	parent->root = true;
	parent->slot = path.top;
	for (int l = path.top; l >= m->level && BlockNumberIsValid(ptr->block); l--)
	{
		if (!tree_node_own(rel, ptr, epoch, true))
			return false;
		if (l == m->level)
			return ptr->block == m->phys && path.first[l] == m->first &&
				lookup_ref(rel, m->phys) == 0;
		parent->root = false;
		parent->blk = ptr->block;
		parent->level = l;
		parent->first = path.first[l];
		parent->born = ptr->since;
		parent->slot = path.slot[l];
		buf = tree_read(rel, ptr->block, RBM_NORMAL);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		parent_check(reln, parent, BufferGetPage(buf));
		*ptr = PageGetBmapEntries(BufferGetPage(buf))[parent->slot];
		UnlockReleaseBuffer(buf);
	}
	return false;
}

/*
 * The first step of moving node 'm' down: a copy of it made at another
 * block, unreachable (tree_make_node() with no parent, in one record), its
 * entries and the node's page LSN noted.  The copy's block is any the
 * allocator gives; one not below the node's is not used (m->copy keeps it,
 * to be freed).  Under the header held exclusively.  False if the node
 * isn't one to move.
 */
static bool
node_move_copy(SMgrRelation reln, NodeMove *m)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		headerbuf;
	Buffer		buf;
	uint32		epoch;
	BmapEntry	ptr;
	TreeParent	parent;
	BmapEntry  *entries;
	bool		ok;

	m->copy.block = InvalidBlockNumber;
	m->installed = false;
	if (!compact_has_room(rel, m->phys))
		return false;

	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	epoch = PageGetBmapHeader(BufferGetPage(headerbuf))->epoch;
	ok = node_move_path(reln, epoch, m, &ptr, &parent);
	if (ok)
	{
		entries = palloc(BMAP_TREE_FANOUT * sizeof(BmapEntry));
		buf = tree_read(rel, m->phys, RBM_NORMAL);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		if (!tree_node_is(BufferGetPage(buf), m->level, m->first, ptr.since))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("block %u of the page store of \"%s\" is not the node of its block map it was",
							m->phys, relpath(reln->smgr_rlocator, MAIN_FORKNUM).str)));
		memcpy(entries, PageGetBmapEntries(BufferGetPage(buf)),
			   BMAP_TREE_FANOUT * sizeof(BmapEntry));
		m->born = ptr.since;
		m->lsn = PageGetLSN(BufferGetPage(buf));
		UnlockReleaseBuffer(buf);
		m->copy = tree_make_node(reln, NULL, epoch, m->level, m->first, entries);
		pfree(entries);
		ok = m->copy.block < m->phys;
	}
	UnlockReleaseBuffer(headerbuf);
	return ok;
}

/*
 * The last step of moving node 'm' down: its parent pointed to the copy, in
 * one record (BMAP_TREE_MOVE), if the node is still where it was, the
 * relation's own, and unchanged since it was copied (its page LSN the
 * same).  The copy was written out and synced before (compact_relation()).
 * Under the header held exclusively.  False if the node changed: the copy
 * is then to be freed.
 */
static bool
node_move_install(SMgrRelation reln, NodeMove *m)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		headerbuf;
	Buffer		buf;
	Buffer		parentbuf;
	uint32		epoch;
	BmapEntry	ptr;
	TreeParent	parent;
	bool		same;
	xl_bmap_tree xlrec;
	struct
	{
		xl_bmap_node_delta d;
		xl_bmap_entry_delta e;
	}			pdelta;
	XLogRecPtr	lsn;

	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	epoch = PageGetBmapHeader(BufferGetPage(headerbuf))->epoch;
	same = node_move_path(reln, epoch, m, &ptr, &parent) && ptr.since == m->born;
	if (same)
	{
		buf = tree_read(rel, m->phys, RBM_NORMAL);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		same = tree_node_is(BufferGetPage(buf), m->level, m->first, m->born) &&
			PageGetLSN(BufferGetPage(buf)) == m->lsn;
		UnlockReleaseBuffer(buf);
	}
	if (!same)
	{
		UnlockReleaseBuffer(headerbuf);
		return false;
	}

	parentbuf = parent_read(reln, &parent);
	map_lock(parentbuf, false);
	parent_check(reln, &parent, BufferGetPage(parentbuf));

	xlrec.locator = rel;
	xlrec.op = BMAP_TREE_MOVE;
	xlrec.flags = 0;
	xlrec.node = m->copy.block;
	xlrec.from = m->phys;
	xlrec.old_hw = xlrec.new_hw = InvalidBlockNumber;
	pdelta.d.kind = BMAP_DELTA_NODE;
	pdelta.d.flags = 0;
	pdelta.d.nentries = 1;
	pdelta.d.since = m->copy.since;
	pdelta.e.slot = parent.slot;
	pdelta.e.block = m->copy.block;
	pdelta.e.epoch = epoch;

	/* (a backup that began before the copy was made can't be used) */
	XLogNoteBackupUnsafe(GetXLogInsertRecPtr());

	START_CRIT_SECTION();
	apply_node_delta(BufferGetPage(parentbuf), &pdelta.d);
	MarkBufferDirty(parentbuf);
	XLogBeginInsert();
	XLogRegisterData(&xlrec, sizeof(xlrec));
	XLogRegisterBuffer(0, parentbuf, REGBUF_STANDARD);
	XLogRegisterBufData(0, &pdelta, sizeof(pdelta));
	lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_TREE);
	PageSetLSN(BufferGetPage(parentbuf), lsn);
	END_CRIT_SECTION();

	map_unlock(parentbuf);
	ReleaseBuffer(parentbuf);
	UnlockReleaseBuffer(headerbuf);
	m->parent = parent;
	m->installed = true;
	INJECTION_POINT("bmap-compact-node-moved", NULL);
	return true;
}

/*
 * Write out the pages that point to nodes moved, and sync the files they
 * are in: the relation's map fork for its root page, the page store for a
 * node.  A parent made something else since (moved itself, its copy then
 * durable) is written as it is, which does no harm.
 */
static void
compact_sync_parents(SMgrRelation reln, const NodeMove *moves, uint32 n)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	bool		root = false;
	bool		nodes = false;

	for (uint32 i = 0; i < n; i++)
	{
		const TreeParent *p = &moves[i].parent;
		Buffer		buf;

		if (!moves[i].installed)
			continue;
		buf = p->root ? map_read(rel, 1) : tree_read(rel, p->blk, RBM_NORMAL);
		LockBuffer(buf, BUFFER_LOCK_SHARE_EXCLUSIVE);
		FlushOneBuffer(buf);
		UnlockReleaseBuffer(buf);
		if (p->root)
			root = true;
		else
			nodes = true;
	}
	if (root)
		mdimmedsync(reln, BMAP_FORKNUM);
	if (nodes)
		mdimmedsync(store_smgr(rel), MAIN_FORKNUM);
}

static int
cmp_move_desc(const void *a, const void *b)
{
	const CompactMove *x = (const CompactMove *) a;
	const CompactMove *y = (const CompactMove *) b;

	return pg_cmp_u32(y->phys, x->phys);
}

/*
 * Move a relation's tree nodes past 'target' down (see "Compaction"), the
 * blocks given back added to *freed.  In three steps, each node's own
 * under the header held exclusively:
 *
 * 1. a copy of each made at a lower free block, unreachable, in a record
 *    of its own (node_move_copy());
 * 2. the copies written out and the page store synced, with no lock held,
 *    once for them all;
 * 3. each parent pointed to its copy, if the node didn't change since
 *    (node_move_install()), then the parents written out and synced.
 *
 * Redo from any point follows a parent's pointer to a version of the node:
 * a parent on disk naming a copy was written after the record installing
 * it, and the copy was on disk before.  No earlier record writes over the
 * copy in redo either: redo leaves alone a block that holds a node born
 * after the record (redo_newer_node()), and the copy was written with its
 * record's LSN.  The old blocks are freed only once the parents are
 * durable, so no redo, however early it begins, reads an old parent's
 * pointer to one taken again (or cut off by the store's shortening).  An
 * error, or a cancel, between the first parent's change and their sync
 * would break that: interrupts are held off then, and an error is a PANIC,
 * whose recovery replays each install and syncs its parent as it goes.
 * Copies not installed are freed with the old blocks.
 */
static uint64
compact_nodes(SMgrRelation reln, CompactWalk *cw, BlockList *freed)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	NodeMove   *moves = palloc0_array(NodeMove, cw->nnodes);
	uint32		n = 0;
	uint32		ninstalled = 0;
	bool		copied = false;

	INJECTION_POINT("bmap-compact-nodes", NULL);
	for (uint32 i = 0; i < cw->nnodes; i++)
	{
		NodeMove   *m = &moves[n];

		m->level = cw->nodes[i].level;
		m->first = cw->nodes[i].first;
		m->phys = cw->nodes[i].phys;
		if (node_move_copy(reln, m))
			copied = true;
		if (BlockNumberIsValid(m->copy.block))
			n++;
		CHECK_FOR_INTERRUPTS();
	}
	if (n == 0)
	{
		pfree(moves);
		return 0;
	}

	/* the copies durable, with no lock held */
	if (copied)
	{
		for (uint32 i = 0; i < n; i++)
		{
			Buffer		buf;

			if (moves[i].copy.block >= moves[i].phys)
				continue;
			buf = tree_read(rel, moves[i].copy.block, RBM_NORMAL);
			LockBuffer(buf, BUFFER_LOCK_SHARE_EXCLUSIVE);
			FlushOneBuffer(buf);
			UnlockReleaseBuffer(buf);
		}
		mdimmedsync(store_smgr(rel), MAIN_FORKNUM);
	}
	INJECTION_POINT("bmap-compact-node-written", NULL);

	HOLD_INTERRUPTS();
	PG_TRY();
	{
		for (uint32 i = 0; i < n; i++)
		{
			if (moves[i].copy.block < moves[i].phys &&
				node_move_install(reln, &moves[i]))
				ninstalled++;
		}
		INJECTION_POINT("bmap-compact-nodes-installed", NULL);
		if (ninstalled > 0)
			compact_sync_parents(reln, moves, n);
		INJECTION_POINT("bmap-compact-parents-synced", NULL);
	}
	PG_CATCH();
	{
		ereport(PANIC,
				(errmsg("could not finish moving the tree nodes of \"%s\"",
						relpath(reln->smgr_rlocator, MAIN_FORKNUM).str)));
	}
	PG_END_TRY();
	RESUME_INTERRUPTS();

	for (uint32 i = 0; i < n; i++)
		block_list_add(freed, moves[i].installed ? moves[i].phys :
					   moves[i].copy.block);
	pfree(moves);
	return ninstalled;
}

/*
 * Move one relation's blocks from 'target' on, the highest first; the old
 * blocks go back to the allocator.
 */
static uint64
compact_relation(SMgrRelation reln, BlockNumber target)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	CompactWalk cw = {target, 0, NULL, 0, 0, NULL, 0, 0};
	Buffer		headerbuf;
	BlockList	freed = {NULL, 0, 0};
	uint64		moved = 0;
	uint64		nodes = 0;

	if (!mdexists(reln, BMAP_FORKNUM) || mdnblocks(reln, BMAP_FORKNUM) == 0)
		return 0;
	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_SHARE);
	if (PageIsNew(BufferGetPage(headerbuf)))
	{
		UnlockReleaseBuffer(headerbuf);
		return 0;
	}
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	cw.nblocks = PageGetBmapHeader(BufferGetPage(headerbuf))->nblocks;
	tree_walk(reln, compact_visit_node, compact_visit_entry, &cw);
	UnlockReleaseBuffer(headerbuf);
	if (cw.n == 0 && cw.nnodes == 0)
		return 0;

	if (cw.n > 0)
		qsort(cw.moves, cw.n, sizeof(CompactMove), cmp_move_desc);
	for (uint32 i = 0; i < cw.n; i++)
	{
		const CompactMove *m = &cw.moves[i];

		if (compact_page(reln, m->blkno, m->phys))
		{
			block_list_add(&freed, m->phys);
			moved++;
		}
		CHECK_FOR_INTERRUPTS();
	}
	INJECTION_POINT("bmap-compact-moved", NULL);

	/*
	 * The nodes, not while an online backup runs: one copying a parent after
	 * a move and the copy's block before it was written couldn't be
	 * recovered from.  Every move notes the position (XLogNoteBackupUnsafe()),
	 * so that a backup that began before it fails as it stops.
	 */
	if (cw.nnodes > 0 && !XLogBackupInProgress())
		nodes = compact_nodes(reln, &cw, &freed);
	moved += nodes;

	if (freed.n > 0)
	{
		qsort(freed.blocks, freed.n, sizeof(BlockNumber), cmp_block);
		free_blocks(rel, freed.blocks, freed.n);
		pfree(freed.blocks);
	}
	if (nodes > 0)
	{
		/* (a backup that began before the frees can't be used) */
		XLogNoteBackupUnsafe(GetXLogInsertRecPtr());
	}
	if (cw.moves)
		pfree(cw.moves);
	if (cw.nodes)
		pfree(cw.nodes);
	pgstat_count_prolly(PGSTAT_PROLLY_BMAP_BLOCKS_MOVED, moved);
	return moved;
}

/*
 * Move the blocks of a database's page store in a tablespace (directory
 * 'path') that lie past its allocated count: how many moved.  Each relation
 * is moved in a subtransaction of its own, so one dropped or truncated
 * meanwhile is passed over.
 */
static uint64
compact_store(RelFileLocator rel, const char *path)
{
	Buffer		metabuf = alloc_read(rel, 0);
	BmapMeta   *meta;
	BlockNumber hw;
	BlockNumber target;
	DIR		   *dir;
	struct dirent *de;
	uint64		moved = 0;
	Oid			spc = rel.spcOid == MyDatabaseTableSpace ? InvalidOid : rel.spcOid;

	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(metabuf), BMAP_PAGE_META, rel, MAIN_FORKNUM, 0);
	meta = PageGetBmapMeta(BufferGetPage(metabuf));
	hw = meta->hw;
	target = meta->hw - meta->nfree;
	UnlockReleaseBuffer(metabuf);
	if (target == hw)
		return 0;

	dir = AllocateDir(path);
	while ((de = ReadDir(dir, path)) != NULL)
	{
		RelFileNumber relnumber;
		char	   *end;
		SMgrRelation reln;
		MemoryContext oldcxt = CurrentMemoryContext;
		ResourceOwner oldowner = CurrentResourceOwner;

		relnumber = (RelFileNumber) strtoul(de->d_name, &end, 10);
		if (end == de->d_name || strcmp(end, "_bmap") != 0)
			continue;
		/* (one whose data may skip WAL: see above) */
		if (!XLogIsNeeded() &&
			!OidIsValid(RelidByRelfilenumber(spc, relnumber)))
			continue;
		reln = smgropen(with_relnumber(rel, relnumber), INVALID_PROC_NUMBER);

		BeginInternalSubTransaction(NULL);
		MemoryContextSwitchTo(oldcxt);
		PG_TRY();
		{
			moved += compact_relation(reln, target);
			ReleaseCurrentSubTransaction();
			MemoryContextSwitchTo(oldcxt);
			CurrentResourceOwner = oldowner;
		}
		PG_CATCH();
		{
			ErrorData  *edata;

			MemoryContextSwitchTo(oldcxt);
			edata = CopyErrorData();
			FlushErrorState();
			RollbackAndReleaseCurrentSubTransaction();
			MemoryContextSwitchTo(oldcxt);
			CurrentResourceOwner = oldowner;
			/* (a cancel cancels the sweep: only other errors pass a relation over) */
			if (edata->sqlerrcode == ERRCODE_QUERY_CANCELED)
				ReThrowError(edata);
			ereport(DEBUG1,
					(errmsg_internal("blocks of relation %u not moved: %s",
									 relnumber, edata->message)));
			FreeErrorData(edata);
		}
		PG_END_TRY();
	}
	FreeDir(dir);
	return moved;
}

/*
 * The snapshots nobody keeps (snapshot_abandoned()) of every map of rel's
 * page store, let go of: how many.  As sweep_relation() reads them: a map
 * dropped since the directory was read is passed over (its snapshots queued
 * by its drop), as is one with no pages, or whose header or root page is
 * all zeroes (a crash part way through making it), and a map dropped while
 * it was read leaves none of its pages in shared buffers.
 */
static uint64
reclaim_store_snapshots(RelFileLocator rel, const char *path)
{
	DIR		   *dir;
	struct dirent *de;
	uint64		n = 0;

	dir = AllocateDir(path);
	while ((de = ReadDir(dir, path)) != NULL)
	{
		RelFileNumber relnumber;
		char	   *end;
		RelFileLocator loc;
		SMgrRelation reln;

		relnumber = (RelFileNumber) strtoul(de->d_name, &end, 10);
		if (end == de->d_name || strcmp(end, "_bmap") != 0)
			continue;
		loc = with_relnumber(rel, relnumber);
		reln = smgropen(loc, INVALID_PROC_NUMBER);
		if (!mdexists(reln, BMAP_FORKNUM) ||
			mdnblocks(reln, BMAP_FORKNUM) < BMAP_TREE_MAP_PAGES)
			continue;
		n += snapshot_reclaim(reln);
		if (!bmap_relation_exists(loc))
		{
			ForkNumber	forks[2] = {BMAP_FORKNUM, BMAP_CHANGES_FORKNUM};
			BlockNumber firsts[2] = {0, 0};

			DropRelationBuffers(reln, forks, 2, firsts);
		}
		CHECK_FOR_INTERRUPTS();
	}
	FreeDir(dir);
	return n;
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

	/* the snapshots nobody keeps, let go of before the queue is drained */
	(void) reclaim_store_snapshots(rel, path);

	/* what dropped maps held, let go of first: the sweep counts the rest */
	{
		BmapRelease done = {0};

		bmap_release_drain(spcOid, dbOid, 0, &done);
	}
	INJECTION_POINT("bmap-sweep-drained", NULL);
	swept_note(rel, true, &released);
	sweep_note(rel, true);
	PG_ENSURE_ERROR_CLEANUP(sweep_done, PointerGetDatum(&rel));
	{
		sweep_store(rel, path, result);
	}
	PG_END_ENSURE_ERROR_CLEANUP(sweep_done, PointerGetDatum(&rel));
	sweep_done(0, PointerGetDatum(&rel));
	result->moved = compact_store(rel, path);
	result->truncated = sweep_shrink(rel);
	swept_note(rel, false, &released);
	pfree(path);
}

/*
 * A scan of rel's page store begins watching it: its token, and the shift
 * of its ranges, into *token and *shift.  True if what a scan whose token
 * is 'since' found still holds for every range whose bit is clear in
 * changed[] (the bits set since then, each cleared); false if nothing it
 * found does (never watched since, or the store outgrew its bits): every
 * range is read.
 */
static bool
watch_begin(RelFileLocator rel, uint64 since, BlockNumber hw,
			uint32 *changed, uint64 *token, uint32 *shift)
{
	uint64		key = watch_key(rel);
	BmapWatch  *w = NULL;
	int			slot = -1;
	bool		valid;

	SpinLockAcquire(&bmap_shared->mutex);
	for (int i = 0; i < BMAP_WATCHED_MAX && slot < 0; i++)
		if (pg_atomic_read_u64(&bmap_shared->watch_key[i]) == key)
			slot = i;
	if (slot >= 0)
		w = &bmap_shared->watch[slot];
	valid = w != NULL && since != 0 && w->token == since &&
		(uint64) hw <= ((uint64) BMAP_WATCH_BITS << pg_atomic_read_u32(&w->shift));
	if (!valid)
	{
		uint32		s = 0;

		/* a store not watched takes the place watched longest unscanned */
		if (w == NULL)
		{
			slot = 0;
			for (int i = 0; i < BMAP_WATCHED_MAX; i++)
			{
				if (pg_atomic_read_u64(&bmap_shared->watch_key[i]) == 0)
				{
					slot = i;
					break;
				}
				if (bmap_shared->watch[i].used < bmap_shared->watch[slot].used)
					slot = i;
			}
			w = &bmap_shared->watch[slot];
			pg_atomic_write_u64(&bmap_shared->watch_key[slot], 0);
		}

		/* bits enough for twice the store */
		while (((uint64) BMAP_WATCH_BITS << s) < 2 * (uint64) hw)
			s++;
		pg_atomic_write_u32(&w->shift, s);
	}
	w->token = ++bmap_shared->next_token;
	if (w->token == 0)
		w->token = ++bmap_shared->next_token;
	w->used = ++bmap_shared->watch_clock;
	*token = w->token;
	*shift = pg_atomic_read_u32(&w->shift);

	/* the bits set since, cleared: a write from now on sets them again */
	for (int i = 0; i < BMAP_WATCH_BITS / 32; i++)
		changed[i] = pg_atomic_exchange_u32(&w->bits[i], 0);
	pg_atomic_write_u64(&bmap_shared->watch_key[slot], key);
	SpinLockRelease(&bmap_shared->mutex);

	/* (the writes a cleared bit stood for, before the blocks are read) */
	pg_memory_barrier();
	return valid;
}

uint64
bmap_scan_store(Oid spcOid, Oid dbOid, uint64 since, uint64 *token,
				BmapRangeVisit range, BmapBlockVisit visit, void *arg)
{
	RelFileLocator rel = {spcOid, dbOid, InvalidRelFileNumber};
	SMgrRelation store;
	Buffer		metabuf;
	BmapMeta   *meta;
	BlockNumber hw;
	uint64	   *allocated;
	uint32	   *changed;
	bool		valid;
	uint32		shift;
	uint64		nranges;
	PGIOAlignedBlock a;
	PGIOAlignedBlock b;
	uint64		nread = 0;
	LOCKTAG		sweeptag;
	LOCKTAG		scantag;

	*token = 0;
	if (!alloc_exists(rel))
		return 0;

	/* not while a sweep frees blocks or shortens the store, nor another scan */
	SET_LOCKTAG_OBJECT(sweeptag, dbOid, BMAP_SWEEP_CLASSID, spcOid, 0);
	(void) LockAcquire(&sweeptag, ShareLock, false, false);
	SET_LOCKTAG_OBJECT(scantag, dbOid, BMAP_SWEEP_CLASSID, spcOid, 1);
	(void) LockAcquire(&scantag, ExclusiveLock, false, false);

	/*
	 * Watching before the blocks allocated are read: a block allocated
	 * since is written after its bit (and is read, or has its bit set).
	 */
	metabuf = alloc_read(rel, 0);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(metabuf), BMAP_PAGE_META, rel, MAIN_FORKNUM, 0);
	hw = PageGetBmapMeta(BufferGetPage(metabuf))->hw;
	UnlockReleaseBuffer(metabuf);
	changed = palloc(sizeof(uint32) * (BMAP_WATCH_BITS / 32));
	valid = watch_begin(rel, since, hw, changed, token, &shift);

	/* the blocks allocated as it begins */
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
	INJECTION_POINT("bmap-scan-store", NULL);

	store = store_smgr(rel);
	nranges = ((uint64) hw + (UINT64CONST(1) << shift) - 1) >> shift;
	for (uint64 r = 0; r < nranges; r++)
	{
		BlockNumber first = (BlockNumber) (r << shift);
		BlockNumber end = (BlockNumber) Min((uint64) hw, (r + 1) << shift);
		bool		read;

		/* (past the bits: the store grew since the scan began watching) */
		read = !valid || r >= BMAP_WATCH_BITS ||
			(changed[r / 32] & (UINT32_C(1) << (r % 32))) != 0;
		range((uint32) r, read, arg);
		if (!read)
			continue;
		for (BlockNumber blk = first; blk < end; blk++)
		{
			if ((allocated[blk / 64] & (UINT64CONST(1) << (blk % 64))) == 0)
				continue;
			CHECK_FOR_INTERRUPTS();

			/* read until two reads agree, visiting each version read */
			smgrread(store, MAIN_FORKNUM, blk, a.data);
			visit(a.data, arg);
			nread++;
			for (;;)
			{
				smgrread(store, MAIN_FORKNUM, blk, b.data);
				if (memcmp(a.data, b.data, BLCKSZ) == 0)
					break;
				visit(b.data, arg);
				memcpy(a.data, b.data, BLCKSZ);
			}
		}
	}
	pfree(allocated);
	pfree(changed);
	LockRelease(&scantag, ExclusiveLock, false);
	LockRelease(&sweeptag, ShareLock, false);
	return nread;
}

typedef struct SizeBits
{
	uint64	   *mapped;
	uint64	   *listed;
	BlockNumber hw;
	BlockNumber nblocks;
} SizeBits;

/* a tree's node is held otherwise, not mapped; its children are read once */
static bool
size_node(BlockNumber node, uint16 level, BlockNumber first, void *arg)
{
	SizeBits   *sb = arg;
	uint64		bit = UINT64CONST(1) << (node % 64);

	if (node >= sb->hw)
		return true;
	if (sb->listed[node / 64] & bit)
		return false;
	sb->listed[node / 64] |= bit;
	return true;
}

static void
size_entry(const BmapEntry *entry, BlockNumber blkno, void *arg)
{
	SizeBits   *sb = arg;
	BlockNumber b = entry->block;

	if (b >= sb->hw)
		return;
	if (blkno < sb->nblocks)
		sb->mapped[b / 64] |= UINT64CONST(1) << (b % 64);
	else
		sb->listed[b / 64] |= UINT64CONST(1) << (b % 64);
}

/*
 * Mark, in mapped[] and listed[] (bits of the store's blocks below hw), the
 * blocks a relation holds: its map's entries under its length, and the
 * rest (entries past it, its changes list's old blocks, a tree's nodes).
 * Its header is
 * held shared meanwhile, so no extension, truncation or prune of it runs.
 * Its length, or 0 if it was dropped since the directory was read.
 */
static BlockNumber
size_relation(SMgrRelation reln, uint64 *mapped, uint64 *listed,
			  BlockNumber hw)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	BlockNumber nblocks;
	Buffer		headerbuf;
	BmapChangesHead head;
	uint32		nchanges;
	BmapChange *changes;

	if (!mdexists(reln, BMAP_FORKNUM) || mdnblocks(reln, BMAP_FORKNUM) == 0)
		return 0;
	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_SHARE);
	if (PageIsNew(BufferGetPage(headerbuf)))
	{
		UnlockReleaseBuffer(headerbuf);
		return 0;
	}
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	nblocks = PageGetBmapHeader(BufferGetPage(headerbuf))->nblocks;
	{
		SizeBits	sb = {mapped, listed, hw, nblocks};

		tree_walk(reln, size_node, size_entry, &sb);
	}
	changes = changes_read(reln, InvalidBuffer, &head, &nchanges);
	if (changes != NULL)
	{
		BlockNumber *held = palloc_array(BlockNumber, Max(nchanges, 1));
		uint32		nheld = changes_blocks(changes, nchanges, held);

		for (uint32 i = 0; i < nheld; i++)
			if (held[i] < hw)
				listed[held[i] / 64] |= UINT64CONST(1) << (held[i] % 64);
		pfree(held);
		pfree(changes);
	}
	UnlockReleaseBuffer(headerbuf);
	return nblocks;
}

static int
cmp_relnumber(const void *a, const void *b)
{
	return pg_cmp_u32(*(const RelFileNumber *) a, *(const RelFileNumber *) b);
}

void
bmap_sizes(Oid spcOid, Oid dbOid, BmapRelationVisit visit, void *arg,
		   BmapSizes *sizes)
{
	RelFileLocator rel = {spcOid, dbOid, InvalidRelFileNumber};
	char	   *path;
	LOCKTAG		sweeptag;
	Buffer		metabuf;
	BmapMeta   *meta;
	BlockNumber hw;
	uint64	   *mapped;
	uint64	   *listed;
	RelFileNumber *maps = NULL;
	int			nmaps = 0;
	int			maxmaps = 0;
	DIR		   *dir;
	struct dirent *de;

	memset(sizes, 0, sizeof(*sizes));
	if (!alloc_exists(rel))
		return;
	path = GetDatabasePath(dbOid, spcOid);

	/* not while a sweep frees blocks or shortens the store */
	SET_LOCKTAG_OBJECT(sweeptag, dbOid, BMAP_SWEEP_CLASSID, spcOid, 0);
	(void) LockAcquire(&sweeptag, ShareLock, false, false);

	metabuf = alloc_read(rel, 0);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(metabuf), BMAP_PAGE_META, rel, MAIN_FORKNUM, 0);
	meta = PageGetBmapMeta(BufferGetPage(metabuf));
	hw = meta->hw;
	sizes->allocated_blocks = hw - meta->nfree;
	UnlockReleaseBuffer(metabuf);
	mapped = MemoryContextAllocHuge(CurrentMemoryContext,
									(hw / 64 + 1) * sizeof(uint64));
	listed = MemoryContextAllocHuge(CurrentMemoryContext,
									(hw / 64 + 1) * sizeof(uint64));
	memset(mapped, 0, (hw / 64 + 1) * sizeof(uint64));
	memset(listed, 0, (hw / 64 + 1) * sizeof(uint64));

	/* the maps, and what each holds */
	dir = AllocateDir(path);
	while ((de = ReadDir(dir, path)) != NULL)
	{
		RelFileNumber relnumber;
		ForkNumber	fork;
		unsigned	segno;

		if (!parse_filename_for_nontemp_relation(de->d_name, &relnumber,
												 &fork, &segno) ||
			fork != BMAP_FORKNUM || segno != 0)
			continue;
		if (nmaps == maxmaps)
		{
			maxmaps = Max(maxmaps * 2, 64);
			maps = maps ? repalloc_array(maps, RelFileNumber, maxmaps) :
				palloc_array(RelFileNumber, maxmaps);
		}
		maps[nmaps++] = relnumber;
	}
	FreeDir(dir);
	qsort(maps, nmaps, sizeof(RelFileNumber), cmp_relnumber);
	for (int i = 0; i < nmaps; i++)
	{
		SMgrRelation reln = smgropen(with_relnumber(rel, maps[i]),
									 INVALID_PROC_NUMBER);
		BlockNumber nblocks = size_relation(reln, mapped, listed, hw);

		sizes->relations++;
		if (visit)
			visit(maps[i], nblocks, arg);
	}
	for (BlockNumber w = 0; w <= hw / 64; w++)
	{
		sizes->mapped_blocks += pg_popcount64(mapped[w]);
		sizes->listed_blocks += pg_popcount64(listed[w] & ~mapped[w]);
	}
	LockRelease(&sweeptag, ShareLock, false);

	/* the files: the store's and the allocator's, and the maps' forks */
	dir = AllocateDir(path);
	while ((de = ReadDir(dir, path)) != NULL)
	{
		RelFileNumber relnumber;
		ForkNumber	fork;
		unsigned	segno;
		char		file[MAXPGPATH];
		struct stat st;

		if (!parse_filename_for_nontemp_relation(de->d_name, &relnumber,
												 &fork, &segno))
			continue;
		snprintf(file, sizeof(file), "%s/%s", path, de->d_name);
		if (stat(file, &st) < 0)
			continue;			/* (unlinked meanwhile) */
		if (relnumber == BMAP_STORE_RELNUMBER)
			sizes->store_bytes += st.st_size;
		else if (relnumber == BMAP_ALLOC_RELNUMBER ||
				 relnumber == BMAP_REFS_RELNUMBER)
			sizes->alloc_bytes += st.st_size;
		else if (bsearch(&relnumber, maps, nmaps, sizeof(RelFileNumber),
						 cmp_relnumber) == NULL)
			continue;			/* (not block-mapped) */
		else if (fork == BMAP_FORKNUM || fork == BMAP_CHANGES_FORKNUM)
			sizes->map_bytes += st.st_size;
		else if (fork == VISIBILITYMAP_FORKNUM)
			sizes->vm_bytes += st.st_size;
		else if (fork == FSM_FORKNUM)
			sizes->fsm_bytes += st.st_size;
	}
	FreeDir(dir);

	pfree(mapped);
	pfree(listed);
	if (maps)
		pfree(maps);
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
	uint64	   *reached;
	DIR		   *dir;
	struct dirent *de;
	BlockList	tofree = {0};
	BlockList	stuck = {0};
	BlockNumber nrefpages;
	uint64		stuck_held;

	SET_LOCKTAG_OBJECT(tag, dbOid, BMAP_SWEEP_CLASSID, spcOid, 0);
	(void) LockAcquire(&tag, ExclusiveLock, false, false);
	stuck_held = pg_atomic_read_u64(&bmap_shared->stuck_held);

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
	reached = MemoryContextAllocHuge(CurrentMemoryContext,
									 (hw / 64 + 1) * sizeof(uint64));
	memset(reached, 0, (hw / 64 + 1) * sizeof(uint64));
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
		sweep_relation(reln, holders, reached, hw);
		result->relations++;
	}
	FreeDir(dir);

	/* and what the release queue holds, read after them */
	{
		SweepHolders sh = {holders, reached, hw};

		sweep_queue(rel, sweep_node, sweep_entry, &sh);
	}

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

	/*
	 * A tree's node copied meanwhile (which this lock doesn't hold off) gave
	 * its children a holder each, after their relation was counted; one whose
	 * count was stuck stayed so, but a count rewritten from what was counted
	 * would be a holder short.  Then the stuck counts are left as they are
	 * (those of blocks nothing holds, which no copy reaches, still go).
	 */
	if (pg_atomic_read_u64(&bmap_shared->stuck_held) != stuck_held)
	{
		uint32		kept = 0;

		for (uint32 k = 0; k < stuck.n; k++)
		{
			if (holders[stuck.blocks[k]] == 0)
				stuck.blocks[kept++] = stuck.blocks[k];
			else
				result->stuck++;
		}
		stuck.n = kept;
	}
	result->recounted = sweep_recount(rel, stuck.blocks, stuck.n, holders);
	LockRelease(&tag, ExclusiveLock, false);

	/* (a block nothing holds gets no new holder: freed after the lock) */
	free_blocks(rel, tofree.blocks, tofree.n);
	result->freed = tofree.n;

	pfree(allocated);
	pfree(holders);
	pfree(reached);
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
	SMgrRelation srels[4];

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
	srels[3] = queue_smgr(rel);
	if (!smgrexists(srels[0], MAIN_FORKNUM) &&
		!smgrexists(srels[1], MAIN_FORKNUM) &&
		!smgrexists(srels[2], MAIN_FORKNUM) &&
		!smgrexists(srels[3], MAIN_FORKNUM))
		return;
	/* at once, as in redo: nothing may use the tablespace any more */
	smgrdounlinkall(srels, 4, true);
	store_unwatch(rel);
}

/* ----------------------------------------------------------------
 * Inspection, for tests
 * ----------------------------------------------------------------
 */

/*
 * Logical block 'blkno' of a block-mapped relation: its physical block
 * (InvalidBlockNumber if unmapped), the entry's birth epoch, and how many
 * other maps reach the block: its own count and the counts of the nodes on
 * its path, added up (a node shared by k maps more shares every block under
 * it with them; at most BMAP_REFS_STUCK).
 */
void
bmap_inspect_block(SMgrRelation reln, BlockNumber blkno, BlockNumber *phys,
				   uint32 *epoch, uint32 *refs)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	BmapEntry	entry;
	TreePath	path;
	BmapEntry	ptr;
	Buffer		buf;
	uint32		n;

	*phys = InvalidBlockNumber;
	*epoch = 0;
	*refs = 0;
	if (!lookup_entry(reln, blkno, &entry))
		return;
	*phys = entry.block;
	*epoch = entry.epoch;
	n = lookup_ref(rel, entry.block);

	tree_path(blkno, &path);
	buf = map_read(rel, 1);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	ptr = PageGetBmapRoot(BufferGetPage(buf))->top[path.top];
	UnlockReleaseBuffer(buf);
	for (int level = path.top; level >= 0 && BlockNumberIsValid(ptr.block); level--)
	{
		n += lookup_ref(rel, ptr.block);
		if (level == 0)
			break;
		buf = tree_read(rel, ptr.block, RBM_NORMAL);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		ptr = PageGetBmapEntries(BufferGetPage(buf))[path.slot[level]];
		UnlockReleaseBuffer(buf);
	}
	*refs = Min(n, BMAP_REFS_STUCK);
}
typedef struct InspectTree
{
	RelFileLocator rel;
	BmapNodeInspect visit;
	void	   *arg;
} InspectTree;

static bool
inspect_node(BlockNumber node, uint16 level, BlockNumber first, void *arg)
{
	InspectTree *it = arg;

	it->visit(level, first, node, lookup_ref(it->rel, node), it->arg);
	return true;
}

static void
inspect_entry(const BmapEntry *entry, BlockNumber blkno, void *arg)
{
}

/*
 * Every node of a block-mapped relation's map, told to 'visit' with its
 * level, the first logical block it maps, its block and its reference count,
 * each time it is reached.  The header is held meanwhile, so the tree
 * changes no other way.
 */
void
bmap_inspect_tree(SMgrRelation reln, BmapNodeInspect visit, void *arg)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	InspectTree it = {rel, visit, arg};
	Buffer		headerbuf;

	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	tree_walk(reln, inspect_node, inspect_entry, &it);
	UnlockReleaseBuffer(headerbuf);
}

/* the logical blocks a block-mapped relation's map pages have entries for */
uint64
bmap_inspect_map_blocks(SMgrRelation reln)
{
	return map_blocks(reln);
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
		rlocator.locator.relNumber == BMAP_REFS_RELNUMBER ||
		rlocator.locator.relNumber == BMAP_QUEUE_RELNUMBER)
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
	create_map(reln);
}

/* Make reln's map: its header and its root page, of no subtrees, in one record */
static void
create_map(SMgrRelation reln)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		buf;
	Buffer		rootbuf;
	Page		page;
	xl_bmap_header_delta delta;
	xl_bmap_root_delta rdelta;
	XLogRecPtr	lsn;

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
	rootbuf = map_new_page(reln);
	if (BufferGetBlockNumber(rootbuf) != 1)
		elog(ERROR, "map of \"%s\" has its root page at block %u",
			 relpath(reln->smgr_rlocator, MAIN_FORKNUM).str,
			 BufferGetBlockNumber(rootbuf));

	delta.kind = BMAP_DELTA_HEADER;
	delta.flags = BMAP_DELTA_INIT;
	delta.nblocks = 0;
	delta.epoch = 0;
	delta.fold_epoch = 0;
	delta.shared_below = 0;
	memset(&rdelta, 0, sizeof(rdelta));
	rdelta.kind = BMAP_DELTA_ROOT;
	rdelta.flags = BMAP_DELTA_INIT;
	rdelta.ntops = BMAP_TREE_TOPS;
	for (int k = 0; k < BMAP_TREE_TOPS; k++)
		rdelta.tops[k].block = InvalidBlockNumber;

	START_CRIT_SECTION();
	apply_header_delta(page, &delta);
	MarkBufferDirty(buf);
	apply_root_delta(BufferGetPage(rootbuf), &rdelta);
	MarkBufferDirty(rootbuf);
	XLogBeginInsert();
	XLogRegisterData(&rel, sizeof(xl_bmap_rel));
	XLogRegisterBuffer(0, buf, REGBUF_WILL_INIT | REGBUF_STANDARD);
	XLogRegisterBufData(0, &delta, sizeof(delta));
	XLogRegisterBuffer(1, rootbuf, REGBUF_WILL_INIT | REGBUF_STANDARD);
	XLogRegisterBufData(1, &rdelta, sizeof(rdelta));
	lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_CREATE);
	PageSetLSN(page, lsn);
	PageSetLSN(BufferGetPage(rootbuf), lsn);
	END_CRIT_SECTION();

	UnlockReleaseBuffer(buf);
	UnlockReleaseBuffer(rootbuf);
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
		/* (and the empty main fork file the drop kept, if redo made the map again) */
		mdunlink(rlocator, MAIN_FORKNUM, true);
		forget_recovery_length(rlocator.locator);
		return;
	}

	reln = smgropen(rlocator.locator, INVALID_PROC_NUMBER);
	if (!mdexists(reln, BMAP_FORKNUM))
		return;

	/*
	 * The map goes at once, but the relation file number must not be used
	 * again until after the next checkpoint, since crash recovery may still
	 * replay records for it.  An empty main fork file keeps it, as md.c's
	 * truncated first segment keeps a dropped relation's, and md.c removes
	 * it after the checkpoint.  Redo, finding no map, replays the
	 * relation's records onto that file as md.c's, until the drop's record
	 * removes it.  (A binary upgrade gives out relation file numbers
	 * itself, and md.c keeps nothing then.)
	 */
	if (!IsBinaryUpgrade)
	{
		RelPathStr	path = relpath(rlocator, MAIN_FORKNUM);
		int			fd = OpenTransientFile(path.str, O_RDWR | O_CREAT | PG_BINARY);

		if (fd < 0)
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not create file \"%s\": %m", path.str)));
		else
			CloseTransientFile(fd);
	}
	mdunlink(rlocator, BMAP_FORKNUM, false);
	if (!IsBinaryUpgrade)
		mdunlink(rlocator, MAIN_FORKNUM, false);
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
	INJECTION_POINT("bmap-dropped", NULL);
}

/* release steps an extension takes at most before the store grows */
#define BMAP_GROWTH_RELEASE_STEPS	64

/* the free blocks of rel's page store, read under a share lock */
static uint32
store_free(RelFileLocator rel)
{
	Buffer		buf = alloc_read(rel, 0);
	uint32		nfree;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(buf), BMAP_PAGE_META, rel, MAIN_FORKNUM, 0);
	nfree = PageGetBmapMeta(BufferGetPage(buf))->nfree;
	UnlockReleaseBuffer(buf);
	return nfree;
}

/*
 * Before reln's main fork is extended by nblocks through shared buffers: if
 * its page store has fewer free blocks than that and its release queue
 * isn't empty, take release steps first (not waiting for another drain or
 * a sweep), until enough are free, the queue is empty or its top waits for
 * a drop, or BMAP_GROWTH_RELEASE_STEPS were taken, so that dropped storage
 * is reused before the store grows.  Only with autovacuum off, whose
 * workers otherwise drain the queue in the background, and only in a
 * transaction.  The caller holds no extension lock (a step takes locks of
 * its own), but may hold a relation's page locked, as a B-tree split does:
 * a step locks the allocator's pages and counts, as the extension's
 * allocation does, and the queue's pages, none of which a backend waits
 * for while holding a relation's page.
 */
void
bmapbeforeextend(SMgrRelation reln, ForkNumber forknum, BlockNumber nblocks)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	BmapRelease done = {0};
	uint64		pushes;
	uint64		key = watch_key(rel);
	int			hint = (int) (hash_bytes_uint32((uint32) (key ^ (key >> 32))) %
							  BMAP_QUEUE_HINTS);

	if (forknum != MAIN_FORKNUM || nblocks == 0 || AutoVacuumingActive() ||
		!IsTransactionState() || CritSectionCount > 0 ||
		RecoveryInProgress())
		return;

	/*
	 * Cheap tests first: free blocks enough, or no queue has had an entry
	 * pushed since every one was last found empty.  A queue found empty
	 * under its metapage's lock had no push counted after 'pushes' was
	 * read, as a push counts itself before it lets the metapage go.
	 */
	if (store_free(rel) >= nblocks)
		return;
	pushes = pg_atomic_read_u64(&bmap_shared->queue_pushes);
	if (pg_atomic_read_u64(&bmap_shared->queue_empty_key[hint]) == key &&
		pg_atomic_read_u64(&bmap_shared->queue_empty_at[hint]) == pushes)
		return;
	pg_memory_barrier();
	if (!queue_exists(rel) ||
		bmap_release_pending(rel.spcOid, rel.dbOid) == 0)
	{
		pg_atomic_write_u64(&bmap_shared->queue_empty_key[hint], key);
		pg_atomic_write_u64(&bmap_shared->queue_empty_at[hint], pushes);
		return;
	}

	for (int step = 0; step < BMAP_GROWTH_RELEASE_STEPS; step++)
	{
		if (!bmap_release_step(rel.spcOid, rel.dbOid, false, &done) ||
			store_free(rel) >= nblocks)
			break;
	}
	if (done.entries > 0)
		pgstat_count_prolly(PGSTAT_PROLLY_BMAP_RELEASE_GROWTH_DRAINS, 1);
}

/*
 * One step of the drain of the release queue of reln's page store, not
 * waiting for another drain or a sweep: the entries it let go of.
 */
uint32
bmapreleasestep(SMgrRelation reln)
{
	BmapRelease done = {0};

	(void) bmap_release_step(reln->smgr_rlocator.locator.spcOid,
							 reln->smgr_rlocator.locator.dbOid, false, &done);
	return (uint32) done.entries;
}

/*
 * Writing a page out looks it up on its map page, cached or read from disk
 * (read_node()).  A map page whose buffer is dropped without being written
 * may be stale or all zeroes on disk, so its buffers go after the pages
 * they map.
 */
bool
bmapwritesread(ForkNumber forknum)
{
	return forknum == BMAP_FORKNUM;
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
	/* (bulk writes and copies: no shared buffer, nothing else counts it) */
	extend_to(reln, blocknum + 1, buffer, skipFsync,
			  PGSTAT_PROLLY_BMAP_BULK_BYTES_WRITTEN);
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
	/* (shared buffers extending: pg_stat_io counts these as extends) */
	extend_to(reln, blocknum + nblocks, NULL, skipFsync,
			  PGSTAT_PROLLY_BMAP_EXTEND_BYTES_WRITTEN);
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
	extend_to(reln, blocknum + nblocks, NULL, true,
			  PGSTAT_PROLLY_BMAP_BULK_BYTES_WRITTEN);
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
		 * the entry, so a block below the length may have none yet, and its
		 * map page may still be zeroes (one the same extension added, whose
		 * buffer wasn't written out before a crash); a prefetch is only a
		 * hint.  The blocks after it without one on its map page are
		 * skipped with it.
		 */
		if (BlockNumberIsValid(phys) &&
			!mdprefetch(ssrel, MAIN_FORKNUM, phys, run))
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
 * relation's.  So were pages of a block whose subtree was replaced or let
 * go of since (see tree_find()).
 */
static bool
page_predates_entry(SMgrRelation reln, BlockNumber blkno, const void *page)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	BmapEntry	found;
	TreeLeaf	leaf;
	XLogRecPtr	since;

	if (tree_find(reln, blkno, true, false, 1, &found, &leaf) != TREE_LEAF)
		return true;

	/* (or a later position, of a version an older image replaced) */
	since = Max(found.since,
				remap_ahead(with_relnumber(rel, BMAP_STORE_RELNUMBER),
							leaf.node, leaf.slot));
	return PageGetLSN((const PageData *) page) < since;
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
	INJECTION_POINT("bmap-write", NULL);
	ssrel = store_smgr(reln->smgr_rlocator.locator);
	while (nblocks > 0)
	{
		BlockNumber run;
		BlockNumber phys = lookup_ext(reln, blocknum, recovery ? 1 : nblocks,
									  &run, recovery, true);

		if (!BlockNumberIsValid(phys))
		{
			XLogRecPtr	lsn;

			/*
			 * In recovery, a page past the header's length that is older
			 * than the header was cut off by a truncation the header has,
			 * whose replay drops it: its entry may have been let go of
			 * since (see extend_in_recovery()), and it goes nowhere.  Any
			 * process may be writing it out, not only the startup process
			 * that kept the length.  So does one whose entry was let go of
			 * after the page's last change, whatever length a header's
			 * image replayed since gives (redo never moves an entry back,
			 * but a header's length may).  Anywhere else, the lookup fails
			 * as without 'missing_ok'.
			 */
			if (!let_go_after(reln, blocknum, true,
							  PageGetLSN((const PageData *) buffers[0])) &&
				(blocknum < header_nblocks(reln, true, &lsn) ||
				 PageGetLSN((const PageData *) buffers[0]) >= lsn))
				(void) lookup_ext(reln, blocknum, 1, NULL, false, true);
			elog(DEBUG1, "block %u of %s, past its map's length and without an entry, not written",
				 blocknum, relpath(reln->smgr_rlocator, MAIN_FORKNUM).str);
			run = 1;
		}
		else if (!recovery || !page_predates_entry(reln, blocknum, buffers[0]))
		{
			mdwritev(ssrel, MAIN_FORKNUM, phys, buffers, run, skipFsync);
			store_written(reln->smgr_rlocator.locator, phys, run);
		}
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
	INJECTION_POINT("bmap-writeback", NULL);
	ssrel = store_smgr(reln->smgr_rlocator.locator);
	while (nblocks > 0)
	{
		BlockNumber run;
		BlockNumber phys = lookup_cached(reln, blocknum, nblocks, &run);

		/*
		 * Writeback is a hint; blocks without an entry, or whose map page
		 * isn't cached, are skipped.
		 */
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
	BlockNumber nblocks;

	if (forknum != MAIN_FORKNUM)
		return mdnblocks(reln, forknum);
	nblocks = header_nblocks(reln, false, NULL);
	/* (a length past the header's, see extend_in_recovery()) */
	if (InRecovery)
		nblocks = Max(nblocks, recovery_length(reln->smgr_rlocator.locator));
	return nblocks;
}

/*
 * The number of the changes list entry the truncation being replayed adds,
 * from the list's header: one less if an earlier replay of the record
 * counted it (see pretruncate_in_recovery()).
 */
static uint32
truncation_entry(Page head)
{
	uint32		n = PageGetBmapChangesHead(head)->nentries;

	return PageGetLSN(head) == GetCurrentReplayRecPtr(NULL) ? n - 1 : n;
}

/*
 * A truncation writes two records: XLOG_SMGR_TRUNCATE, flushed before any
 * buffer is dropped, then the header's own (set_length()), which nothing
 * flushes.  A crash in between leaves only the first durable, whose replay
 * drops the buffers past the new length: were the header left long, those
 * pages would be read again from disk, older than the records replayed
 * onto them (rows deleted before the truncation would come back).  So the
 * first record's replay cuts the header itself, and lists the truncation
 * on a folded relation's changes list, as the second record does; that
 * one's replay, if it comes, applies the same again.  A page newer than
 * the first record's end means the second is durable (the header is held
 * from one record to the other, and the list changes only under it): then
 * the pages are left to it.  A page at the record's end was changed by an
 * earlier replay of it, which a crash interrupted with only some of the
 * pages written: each page still older is changed, and none twice.
 *
 * In redo, before the truncation's critical section: the header, held
 * exclusively, and a folded relation's changes list's header and the page
 * its next entry goes on, pinned (the page added to the fork if need be).
 */
static void
pretruncate_in_recovery(SMgrRelation reln)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		headerbuf;

	reln->bmap_pretrunc = InvalidBuffer;
	reln->bmap_pretrunc_chg[0] = reln->bmap_pretrunc_chg[1] = InvalidBuffer;
	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	if (PageGetBmapHeader(BufferGetPage(headerbuf))->fold_epoch > 0)
	{
		Buffer		head = chg_read(rel, 0);
		Buffer		page;
		BlockNumber pageno;

		LockBuffer(head, BUFFER_LOCK_SHARE);
		check_page(BufferGetPage(head), BMAP_PAGE_CHANGES_HEAD, rel,
				   BMAP_CHANGES_FORKNUM, 0);
		pageno = CHANGE_PAGE(truncation_entry(BufferGetPage(head)));
		LockBuffer(head, BUFFER_LOCK_UNLOCK);
		if (pageno < mdnblocks(reln, BMAP_CHANGES_FORKNUM))
			page = chg_read(rel, pageno);
		else
		{
			page = XLogReadBufferExtended(rel, BMAP_CHANGES_FORKNUM, pageno,
										  RBM_ZERO_AND_LOCK, InvalidBuffer);
			LockBuffer(page, BUFFER_LOCK_UNLOCK);
		}
		reln->bmap_pretrunc_chg[0] = head;
		reln->bmap_pretrunc_chg[1] = page;
	}
	reln->bmap_pretrunc = headerbuf;
	/* (run in the truncation's critical section, which can't load it) */
	INJECTION_POINT_LOAD("bmap-truncated-in-recovery");
}

/*
 * In redo, in XLOG_SMGR_TRUNCATE's critical section: cut the header to
 * 'nblocks', and list the truncation, unless the header's own record is
 * durable (see pretruncate_in_recovery()).  The pages take this record's
 * end as their LSN, so that the header's record still replays onto them.
 */
static void
truncate_in_recovery(SMgrRelation reln, BlockNumber nblocks)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		headerbuf = reln->bmap_pretrunc;
	Buffer	   *chg = reln->bmap_pretrunc_chg;
	XLogRecPtr	end = GetCurrentReplayRecPtr(NULL);
	Page		hpage;
	BmapHeader *header;
	bool		newer;

	if (!BufferIsValid(headerbuf))
		elog(PANIC, "block-mapped relation %s truncated in recovery without being prepared",
			 relpath(reln->smgr_rlocator, MAIN_FORKNUM).str);
	reln->bmap_pretrunc = InvalidBuffer;
	hpage = BufferGetPage(headerbuf);
	header = PageGetBmapHeader(hpage);
	if (BufferIsValid(chg[0]))
	{
		LockBuffer(chg[0], BUFFER_LOCK_EXCLUSIVE);
		LockBuffer(chg[1], BUFFER_LOCK_EXCLUSIVE);
	}
	newer = PageGetLSN(hpage) > end ||
		(BufferIsValid(chg[0]) &&
		 (PageGetLSN(BufferGetPage(chg[0])) > end ||
		  PageGetLSN(BufferGetPage(chg[1])) > end));

	/* (an earlier replay changed the header only if it cut it) */
	if (!newer && (PageGetLSN(hpage) == end || nblocks < header->nblocks))
	{
		if (BufferIsValid(chg[0]))
		{
			Page		hp = BufferGetPage(chg[0]);
			Page		pp = BufferGetPage(chg[1]);
			BmapChangesHead *head = PageGetBmapChangesHead(hp);
			uint32		n = truncation_entry(hp);

			if (BufferGetBlockNumber(chg[1]) != CHANGE_PAGE(n))
				elog(PANIC, "changes list of \"%s\" grew while its truncation was prepared",
					 relpathperm(rel, MAIN_FORKNUM).str);
			if (PageGetLSN(hp) < end)
			{
				xl_bmap_changes_head_delta hdelta;

				hdelta.kind = BMAP_DELTA_CHANGES_HEAD;
				hdelta.flags = 0;
				hdelta.fold_epoch = head->fold_epoch;
				hdelta.lineage = head->lineage;
				hdelta.fold_nblocks = head->fold_nblocks;
				hdelta.nentries = n + 1;
				hdelta.pruned_epoch = head->pruned_epoch;
				apply_changes_head_delta(hp, &hdelta);
				PageSetLSN(hp, end);
				MarkBufferDirty(chg[0]);
			}
			if (PageGetLSN(pp) < end)
			{
				struct
				{
					xl_bmap_changes_delta d;
					BmapChange	c;
				}			pdelta;

				pdelta.d.kind = BMAP_DELTA_CHANGES;
				pdelta.d.flags = CHANGE_SLOT(n) == 0 ? BMAP_DELTA_INIT : 0;
				pdelta.d.first = CHANGE_SLOT(n);
				pdelta.d.n = 1;
				memset(&pdelta.c, 0, sizeof(pdelta.c));
				pdelta.c.epoch = header->epoch;
				pdelta.c.blkno = nblocks;
				pdelta.c.old_block = InvalidBlockNumber;
				pdelta.c.kind = BMAP_CHANGE_TRUNCATE;
				apply_changes_delta(pp, &pdelta.d);
				PageSetLSN(pp, end);
				MarkBufferDirty(chg[1]);
			}
		}
		if (PageGetLSN(hpage) < end)
		{
			xl_bmap_header_delta delta;

			delta.kind = BMAP_DELTA_HEADER;
			delta.flags = 0;
			delta.nblocks = nblocks;
			delta.epoch = header->epoch;
			delta.fold_epoch = header->fold_epoch;
			delta.shared_below = 0;
			apply_header_delta(hpage, &delta);
			PageSetLSN(hpage, end);
			MarkBufferDirty(headerbuf);
		}
		{
			/* (the pages changed, still locked: header, list's header, page) */
			Buffer		bufs[3] = {headerbuf, chg[0], chg[1]};

			INJECTION_POINT_CACHED("bmap-truncated-in-recovery", bufs);
		}
	}
	UnlockReleaseBuffer(headerbuf);
	for (int i = 0; i < 2; i++)
	{
		if (BufferIsValid(chg[i]))
			UnlockReleaseBuffer(chg[i]);
		chg[i] = InvalidBuffer;
	}
	reln->smgr_cached_nblocks[MAIN_FORKNUM] = InvalidBlockNumber;
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

	if (forknum != MAIN_FORKNUM)
		return;
	if (InRecovery)
	{
		pretruncate_in_recovery(reln);
		return;
	}

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
	/* (run in the truncation's critical section, which can't load it) */
	INJECTION_POINT_LOAD("bmap-truncating");
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

	/*
	 * In recovery, the header is cut here, its own record maybe lost (see
	 * pretruncate_in_recovery()); a length kept past the header's is cut
	 * too, the header's record maybe already replayed.
	 */
	if (InRecovery)
	{
		truncate_in_recovery(reln, nblocks);
		cut_recovery_length(reln->smgr_rlocator.locator, nblocks);
		return;
	}

	headerbuf = reln->bmap_pretrunc;
	if (!BufferIsValid(headerbuf))
		elog(ERROR, "block-mapped relation %s truncated without being prepared",
			 relpath(reln->smgr_rlocator, MAIN_FORKNUM).str);
	reln->bmap_pretrunc = InvalidBuffer;
	INJECTION_POINT_CACHED("bmap-truncating", NULL);

	/*
	 * Blocks past the new length stay mapped until the truncation is done
	 * (bmapposttruncate()).  A folded relation lists the truncation.
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

/*
 * The first of blocks [from, to), on one leaf 'leaf', with an entry, and in
 * *last the one after the last; false if none has one.
 */
static bool
mapped_span(SMgrRelation reln, const MapPage *leaf, uint64 from, uint64 to,
			uint64 *first, uint64 *last)
{
	PGIOAlignedBlock copy;
	const BmapEntry *entries;
	bool		found = false;
	const MapPage *mp = leaf;
	Buffer		buf = mp_read(reln, mp);

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	mp_check(reln, mp, BufferGetPage(buf));
	memcpy(copy.data, BufferGetPage(buf), BLCKSZ);
	UnlockReleaseBuffer(buf);
	entries = PageGetBmapEntries(copy.data);
	for (uint64 blk = from; blk < to; blk++)
	{
		if (!BlockNumberIsValid(entries[MP_SLOT(mp, blk)].block))
			continue;
		if (!found)
			*first = blk;
		*last = blk + 1;
		found = true;
	}
	return found;
}

/*
 * Once a truncation is done, outside its critical section: let go of the
 * entries past the relation's length, this truncation's and any an earlier
 * one left (a crash before this), freeing the blocks no other map holds
 * (#638).  Pages as of a fold go on its changes list, which holds them
 * until pruned.  Kept, they would be held until the relation grew again
 * or was dropped, and counted by every sweep.  The records follow the
 * truncation's, so their replay finds the length already cut, and the
 * relation's pages past it have left shared buffers: none is written onto
 * a block freed here and given to another relation.  Records from before
 * the truncation that a crash replays for such a page are skipped, or the
 * page goes nowhere (bmapreplacedafter(), bmapwritev()), as for an entry
 * let go of on extension.
 */
void
bmapposttruncate(SMgrRelation reln, ForkNumber forknum)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		headerbuf;
	BmapHeader *header;
	uint64		end;

	if (forknum != MAIN_FORKNUM || InRecovery)
		return;
	INJECTION_POINT("bmap-truncated", NULL);
	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	header = PageGetBmapHeader(BufferGetPage(headerbuf));

	end = map_blocks(reln);
	for (uint64 page = header->nblocks; page < end;)
	{
		uint64		pageend;
		uint64		from;
		uint64		last;
		MapPage		leaf;
		TreePath	path;

		tree_path((BlockNumber) page, &path);
		pageend = Min(end, (uint64) path.first[0] + BMAP_TREE_FANOUT);

		/*
		 * The map never shrinks, so most of its leaves past the length have
		 * nothing left after the first truncation: each is read once, not
		 * copied for every piece, and a shared one is copied (the path to it,
		 * the relation's own from then on) only to let go of entries on it.
		 */
		if (tree_leaf_for(reln, headerbuf, (BlockNumber) page, false, false,
						  &leaf) &&
			mapped_span(reln, &leaf, page, pageend, &from, &last))
		{
			(void) tree_leaf_for(reln, headerbuf, (BlockNumber) page, false,
								 true, &leaf);
			while (from < last)
			{
				uint64		to = Min(last, from + BMAP_PIECE);

				unshare_stale(reln, &leaf, header->epoch, header->fold_epoch,
							  (BlockNumber) from, (BlockNumber) to, true);
				from = to;
			}
		}
		page = pageend;
	}
	UnlockReleaseBuffer(headerbuf);
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
 * Redo
 * ----------------------------------------------------------------
 */

/*
 * Redo's view of a map entry never goes backwards (#647).  A map page read
 * in recovery may be newer than the record being replayed (written out
 * before a crash, copied by a base backup), and an entry it changed since
 * (given another block, or let go of) makes redo skip the logical block's
 * records from before the change, or send its pages nowhere
 * (bmapreplacedafter(), bmapwritev()).  A full-page image of the map page
 * logged before the change (an extension of another block on it) would
 * bring the old entry back partway through replay, and the block's later
 * records would be replayed onto a page that missed the ones skipped.  So
 * an image restored over a newer page keeps each entry the newer page
 * changed later (its 'since' is later), and a delta leaves such an entry
 * alone (apply_node_delta()).  The page's LSN is then the newer page's: a
 * record before it changes the entries it changes anyway (a torn newer
 * page may have older ones), and wal_consistency_checking compares the
 * page only once replay has caught up with it.
 */

/*
 * Map page 'blkno' of 'rlocator' as it is before redo restores an image of
 * it, into 'dst', if it is a map page newer than the record; read from
 * shared buffers or disk, and passed over if it fails verification (an
 * image is restored over a torn page).
 *
 * This costs an image what images otherwise save redo: a read of the page
 * it replaces, synchronous if the page isn't cached, so one random read per
 * map page the first time recovery restores it after a checkpoint.  Whether
 * the page on disk is newer than the record can't be known without it.
 */
static bool
newer_map_page(XLogReaderState *record, RelFileLocator rlocator,
			   BlockNumber blkno, char *dst)
{
	SMgrRelation reln = smgropen(rlocator, INVALID_PROC_NUMBER);
	Buffer		buf;
	BmapPageOpaqueData *opaque;

	if (!mdexists(reln, BMAP_FORKNUM) || blkno >= mdnblocks(reln, BMAP_FORKNUM))
		return false;
	/* (only redo dirties map pages, so a read off disk isn't torn by one) */
	if (!read_map_uncached(reln, blkno, dst, 1, &buf))
		return false;
	if (BufferIsValid(buf))
	{
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		memcpy(dst, BufferGetPage(buf), BLCKSZ);
		UnlockReleaseBuffer(buf);
	}
	if (PageIsNew(dst) || PageGetLSN(dst) <= record->ReadRecPtr ||
		PageGetSpecialSize(dst) != MAXALIGN(sizeof(BmapPageOpaqueData)))
		return false;
	opaque = PageGetBmapOpaque(dst);
	return opaque->magic == BMAP_MAGIC && opaque->type == BMAP_PAGE_ROOT;
}

/*
 * The same for a tree's node, block 'blkno' of the page store 'rlocator':
 * true if there is a version of a node there newer than the record.  Of
 * the node the record names, or of another incarnation of its block (made
 * since it was let go of), which the caller tells apart by its birth.
 */
static bool
newer_tree_node(XLogReaderState *record, RelFileLocator rlocator,
				BlockNumber blkno, char *dst)
{
	SMgrRelation ssrel = smgropen(rlocator, INVALID_PROC_NUMBER);
	PrefetchBufferResult r;

	if (!smgrexists(ssrel, MAIN_FORKNUM) ||
		blkno >= mdnblocks(ssrel, MAIN_FORKNUM))
		return false;
	r = PrefetchSharedBuffer(ssrel, MAIN_FORKNUM, blkno);
	if (BufferIsValid(r.recent_buffer) &&
		ReadRecentBuffer(rlocator, MAIN_FORKNUM, blkno, r.recent_buffer))
	{
		LockBuffer(r.recent_buffer, BUFFER_LOCK_SHARE);
		memcpy(dst, BufferGetPage(r.recent_buffer), BLCKSZ);
		UnlockReleaseBuffer(r.recent_buffer);
	}
	else
	{
		void	   *page = dst;

		mdreadv(ssrel, MAIN_FORKNUM, blkno, &page, 1);
		if (!PageIsVerified(dst, blkno, 0, NULL))
			return false;
	}
	return tree_node_any(dst) && PageGetLSN(dst) > record->ReadRecPtr;
}

/*
 * Keep the entries of 'newer' changed after those of the restored page.
 * The page then takes the newer page's LSN, so the records between the two
 * aren't replayed on it: whatever else of the page they change must be the
 * newer page's too.  On a root page that is its snapshot slots, which no
 * birth orders: the newer page's are taken whole, as of its LSN, so that a
 * snapshot taken in between is kept and one released in between stays
 * released.
 */
static void
keep_newer_entries(Buffer buf, char *newer)
{
	Page		page = BufferGetPage(buf);
	BmapEntry  *entries = PageGetBmapEntries(page);
	const BmapEntry *kept = PageGetBmapEntries(newer);
	bool		any = false;
	bool		root = false;
	uint32		nslots = BMAP_TREE_FANOUT;

	/* (a root page has its pointers, a node its entries) */
	if (map_page_is((const PGIOAlignedBlock *) newer, BMAP_PAGE_ROOT))
	{
		nslots = BMAP_TREE_TOPS;
		root = true;
	}
	for (uint32 i = 0; i < nslots; i++)
	{
		if (kept[i].since > entries[i].since)
		{
			entries[i] = kept[i];
			any = true;
		}
	}
	if (any)
	{
		if (root && PageGetLSN(newer) > PageGetLSN(page))
			memcpy(PageGetBmapRoot(page)->snap, PageGetBmapRoot(newer)->snap,
				   sizeof(PageGetBmapRoot(page)->snap));
		PageSetLSN(page, Max(PageGetLSN(page), PageGetLSN(newer)));
		MarkBufferDirty(buf);
	}
}

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
	RelFileLocator rlocator;
	ForkNumber	forknum;
	BlockNumber blkno;
	PGIOAlignedBlock newer;
	bool		have_newer = false;
	bool		node;

	/* a map page's image restored: the page it replaces, if newer */
	XLogRecGetBlockTag(record, block_id, &rlocator, &forknum, &blkno);
	node = forknum == MAIN_FORKNUM && rlocator.relNumber == BMAP_STORE_RELNUMBER;

	/* (a node born since on the block: this record's writes are of no use) */
	if (node && redo_newer_node(rlocator, blkno, record->ReadRecPtr))
		return;
	if (forknum == BMAP_FORKNUM && blkno > 0 &&
		XLogRecBlockImageApply(record, block_id))
		have_newer = newer_map_page(record, rlocator, blkno, newer.data);

	/*
	 * A tree's node's image restored over a newer version of the same node
	 * (written out before a crash, copied by a base backup): the newer one's
	 * later entries are kept (keep_newer_entries()), and a leaf's are
	 * remembered first (remember_ahead()).  A newer version of another
	 * incarnation of the block, made after this one was let go of, keeps
	 * nothing: this record's node is the one replay needs until then.
	 */
	if (node && XLogRecBlockImageApply(record, block_id) &&
		newer_tree_node(record, rlocator, blkno, newer.data))
	{
		PGIOAlignedBlock image;

		if (!RestoreBlockImage(record, block_id, image.data))
			elog(PANIC, "%s", record->errormsg_buf);
		have_newer = tree_node_any(image.data) &&
			PageGetBmapTreeOpaque(image.data)->born ==
			PageGetBmapTreeOpaque(newer.data)->born;
		if (have_newer && PageGetBmapTreeOpaque(newer.data)->level == 0)
			remember_ahead(rlocator, blkno,
						   PageGetBmapEntries(newer.data), record->ReadRecPtr);
	}

	/* a full-page image leaves out the block's data, and needs none */
	if (data == NULL)
	{
		if (XLogReadBufferForRedo(record, block_id, &buf) != BLK_RESTORED)
			elog(PANIC, "block map record without its delta");
		if (have_newer)
			keep_newer_entries(buf, newer.data);
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
	if (action == BLK_RESTORED && have_newer)
		keep_newer_entries(buf, newer.data);

	/*
	 * A map page newer than the record still takes its entry changes, which
	 * leave alone those changed later: one whose newer entries were kept
	 * over an image may have older ones too (above).  It is dirtied only if
	 * an entry changed: written out, a page whose LSN is past the replay
	 * point asks for the minimum recovery point past it, which recovery
	 * from a base backup warns of, and most such deltas change nothing.
	 */
	if (action == BLK_DONE && kind == BMAP_DELTA_NODE &&
		forknum == BMAP_FORKNUM &&
		map_page_is((const PGIOAlignedBlock *) BufferGetPage(buf),
					BMAP_PAGE_ROOT))
	{
		if (apply_node_delta(BufferGetPage(buf), (xl_bmap_node_delta *) copy))
			MarkBufferDirty(buf);
	}

	/*
	 * So does a tree's node, if it is the node the record changes: a newer
	 * page of another incarnation of its block (born after the record) is
	 * left as it is.
	 */
	else if (action == BLK_DONE && kind == BMAP_DELTA_NODE && node)
	{
		Page		page = BufferGetPage(buf);

		if (tree_node_any(page) &&
			PageGetBmapTreeOpaque(page)->born <= record->ReadRecPtr &&
			apply_node_delta(page, (xl_bmap_node_delta *) copy))
			MarkBufferDirty(buf);
	}
	else if (action == BLK_NEEDS_REDO)
	{
		Page		page = BufferGetPage(buf);

		switch (kind)
		{
			case BMAP_DELTA_HEADER:
				apply_header_delta(page, (xl_bmap_header_delta *) copy);
				break;
			case BMAP_DELTA_NODE:
				if (node && !tree_node_any(page))
					elog(PANIC, "block %u of page store %u/%u, changed by a record, is no block map node",
						 blkno, rlocator.spcOid, rlocator.dbOid);
				apply_node_delta(page, (xl_bmap_node_delta *) copy);
				break;
			case BMAP_DELTA_ROOT:
				apply_root_delta(page, (xl_bmap_root_delta *) copy);
				break;
			case BMAP_DELTA_SNAPSHOT:
				apply_snapshot_delta(page, (xl_bmap_snapshot_delta *) copy);
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
			case BMAP_DELTA_QUEUE_META:
				apply_queue_meta_delta(page, (xl_bmap_queue_meta_delta *) copy);
				break;
			case BMAP_DELTA_QUEUE:
				apply_queue_delta(page, (xl_bmap_queue_delta *) copy);
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

/*
 * In redo: block 'block_id' of the record, as replayed, written out and its
 * file synced.
 */
static void
redo_flush_block(XLogReaderState *record, uint8 block_id)
{
	RelFileLocator rlocator;
	ForkNumber	forknum;
	BlockNumber blkno;
	Buffer		buf;

	XLogRecGetBlockTag(record, block_id, &rlocator, &forknum, &blkno);
	buf = ReadBufferWithoutRelcache(rlocator, forknum, blkno, RBM_NORMAL,
									NULL, true);
	LockBuffer(buf, BUFFER_LOCK_SHARE_EXCLUSIVE);
	FlushOneBuffer(buf);
	UnlockReleaseBuffer(buf);
	mdimmedsync(smgropen(rlocator, INVALID_PROC_NUMBER), forknum);
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

				/* making the header creates the map fork (a tree's root too) */
				for (uint8 id = 0; id <= XLogRecMaxBlockId(record); id++)
					redo_block(record, id);
				forget_recovery_length(xlrec->locator);

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
							  (xlrec->flags & BMAP_EXTEND_SKIP_ZERO) != 0,
							  PGSTAT_PROLLY_BMAP_STORE_BYTES_WRITTEN,
							  record->ReadRecPtr);
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
		case XLOG_BMAP_TREE:
			{
				xl_bmap_tree *xlrec = (xl_bmap_tree *) XLogRecGetData(record);

				/* the store reaches a node made, which its image makes */
				if (xlrec->op == BMAP_TREE_NODE || xlrec->op == BMAP_TREE_COPY)
					prepare_node_store(xlrec->locator, xlrec->node,
									   xlrec->old_hw, xlrec->new_hw);

				/*
				 * A node moved down: as the primary did, the copy (made by
				 * an earlier record) durable before the parent takes it, and
				 * the parent durable after, before a later record can take
				 * the old block again (see compact_nodes()).  A backup taken
				 * here meanwhile can't be used.
				 */
				if (xlrec->op == BMAP_TREE_MOVE)
				{
					Buffer		buf = tree_read(xlrec->locator, xlrec->node,
												RBM_NORMAL);

					LockBuffer(buf, BUFFER_LOCK_SHARE_EXCLUSIVE);
					FlushOneBuffer(buf);
					UnlockReleaseBuffer(buf);
					mdimmedsync(store_smgr(xlrec->locator), MAIN_FORKNUM);
					redo_block(record, 0);
					redo_flush_block(record, 0);
					XLogNoteBackupUnsafe(record->ReadRecPtr);
					break;
				}
				for (uint8 id = 0; id <= XLogRecMaxBlockId(record); id++)
					redo_block(record, id);

				/*
				 * A snapshot kept (its header changed too, which a let-go
				 * leaves): as the primary did, the relation's pages as of
				 * this record written to the blocks the snapshot holds, and
				 * durably.  Without online backups no image of them is
				 * logged, and the pages redo made of the records before
				 * this go elsewhere once a remap moves the relation off
				 * them: the snapshot's blocks would keep what they had
				 * when redo began.
				 */
				if (xlrec->op == BMAP_TREE_SNAPSHOT &&
					XLogRecHasBlockRef(record, 1))
				{
					SMgrRelation reln = smgropen(xlrec->locator,
												 INVALID_PROC_NUMBER);

					FlushRelationsAllBuffers(&reln, 1);
					mdimmedsync(store_smgr(xlrec->locator), MAIN_FORKNUM);
				}
				break;
			}
		case XLOG_BMAP_FREE:
		case XLOG_BMAP_UNREF:
		case XLOG_BMAP_RELEASE:
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
				if (!zeroed && xlrec->new_block < xlrec->old_hw)
					drop_node_buffer(xlrec->locator, xlrec->new_block);
				prepare_store(xlrec->locator, xlrec->old_hw, xlrec->new_hw,
							  &xlrec->new_block,
							  zeroed && xlrec->new_block < xlrec->old_hw ? 1 : 0,
							  !zeroed, PGSTAT_PROLLY_BMAP_STORE_BYTES_WRITTEN,
							  record->ReadRecPtr);
				for (uint8 id = 0; id < last; id++)
					redo_block(record, id);
				if (zeroed)
				{
					/*
					 * zeroes in its buffer too, without an LSN, as a new
					 * page.  A buffer already valid (the old block's page,
					 * read before) is only locked, not zeroed: zeroed here,
					 * under a cleanup lock, as a standby's scan may hold a
					 * pin on it and read its tuples without a content lock.
					 * A buffer of zeroes stays clean: the block has zeroes.
					 */
					Buffer		buf;

					(void) XLogReadBufferForRedoExtended(record, last,
														 RBM_ZERO_AND_CLEANUP_LOCK,
														 true, &buf);
					if (!pg_memory_is_all_zeros(BufferGetPage(buf), BLCKSZ))
					{
						memset(BufferGetPage(buf), 0, BLCKSZ);
						MarkBufferDirty(buf);
					}
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
					(BMAP_REMAP_LISTED | BMAP_REMAP_OWN) &&
					!redo_newer_node(xlrec->locator, xlrec->old_block,
									 record->ReadRecPtr))
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
					store_written(xlrec->locator, xlrec->old_block, 1);
				}
				XLogReplaceBlock(xlrec->locator, MAIN_FORKNUM, xlrec->blkno);
				break;
			}
		case XLOG_BMAP_PRESERVE:
			{
				BlockNumber phys;
				RelFileLocator rel;
				ForkNumber	forknum;
				BlockNumber blkno;
				Buffer		buf;
				PGIOAlignedBlock copy;

				memcpy(&phys, XLogRecGetData(record), sizeof(phys));
				XLogRecGetBlockTag(record, 0, &rel, &forknum, &blkno);
				Assert(forknum == MAIN_FORKNUM);

				/*
				 * Restore the source buffer too: SHARED flushes it, and must
				 * not overwrite the preserved block with incomplete redo.
				 * A newer map sends this buffer nowhere until its remap.
				 */
				buf = XLogReadBufferExtended(rel, forknum, blkno,
											RBM_ZERO_AND_LOCK, InvalidBuffer);
				if (!RestoreBlockImage(record, 0, BufferGetPage(buf)))
					elog(PANIC, "%s", record->errormsg_buf);
				/* Sharing changes no page contents, including its LSN. */
				MarkBufferDirty(buf);
				memcpy(copy.data, BufferGetPage(buf), BLCKSZ);
				UnlockReleaseBuffer(buf);

				/* The physical address is independent of every map's age. */
				if (redo_newer_node(rel, phys, record->ReadRecPtr))
					break;
				PageSetChecksum((Page) copy.data, blkno);
				smgrwrite(store_smgr(rel), MAIN_FORKNUM, phys, copy.data, false);
				store_written(rel, phys, 1);
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
