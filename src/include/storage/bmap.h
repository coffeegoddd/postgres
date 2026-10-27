/*-------------------------------------------------------------------------
 *
 * bmap.h
 *	  Block-mapped storage manager: relations whose main fork's logical
 *	  blocks are mapped to physical blocks of a shared page store.
 *
 * See src/backend/storage/smgr/bmap.c.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/bmap.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BMAP_H
#define BMAP_H

#include "datatype/timestamp.h"
#include "storage/bmap_files.h"
#include "storage/bmap_xlog.h"
#include "storage/locktag.h"
#include "storage/smgr.h"
#include "storage/sync.h"

/* the smgr implementation's number in smgrsw[] */
#define SMGR_BMAP		1

/* developer setting: heap tables made in block-mapped storage */
extern PGDLLIMPORT bool debug_block_mapped_heap;

/* logical blocks recovery remembers a later remap of, at most */
extern PGDLLIMPORT int block_map_replay_entries;

/* storage manager callbacks */
extern void bmapinit(void);
extern bool bmapclaims(RelFileLocatorBackend rlocator);
extern void bmapopen(SMgrRelation reln);
extern void bmapclose(SMgrRelation reln, ForkNumber forknum);
extern void bmapcreate(SMgrRelation reln, ForkNumber forknum, bool isRedo);
extern bool bmapexists(SMgrRelation reln, ForkNumber forknum);
extern void bmapshare(SMgrRelation src, SMgrRelation dst);
extern void bmapsyncshares(bool isCommit);
extern bool bmapreplacesblocks(SMgrRelation reln, ForkNumber forknum);
extern bool bmapreplacedafter(SMgrRelation reln, ForkNumber forknum,
							  BlockNumber blocknum, XLogRecPtr lsn);
extern void bmapprerestore(SMgrRelation reln, ForkNumber forknum,
						   BlockNumber blocknum, XLogRecPtr lsn);
extern bool bmappreparesforchanges(SMgrRelation reln, ForkNumber forknum);
extern void bmapprepareforchange(SMgrRelation reln, ForkNumber forknum,
								 BlockNumber blocknum, Buffer buffer,
								 bool neutral);
extern bool bmappageisshared(SMgrRelation reln, ForkNumber forknum,
							 BlockNumber blocknum, bool *copies);
extern void bmappredrop(SMgrRelation reln, bool isRedo);
extern bool bmappagesharedelsewhere(SMgrRelation reln, ForkNumber forknum,
									BlockNumber blocknum);
extern void bmapprecommitdrop(SMgrRelation reln);
extern void bmapenddrops(void);
extern void bmapbeforeextend(SMgrRelation reln, ForkNumber forknum,
							 BlockNumber nblocks);
extern uint32 bmapreleasestep(SMgrRelation reln);
extern bool bmapwritesread(ForkNumber forknum);
extern void bmapunlink(RelFileLocatorBackend rlocator, ForkNumber forknum,
					   bool isRedo);
extern void bmap_drop_database_recovery(Oid dbid, Oid spcid);
extern void bmapextend(SMgrRelation reln, ForkNumber forknum,
					   BlockNumber blocknum, const void *buffer, bool skipFsync);
extern void bmapzeroextend(SMgrRelation reln, ForkNumber forknum,
						   BlockNumber blocknum, int nblocks, bool skipFsync);
extern void bmappreextend(SMgrRelation reln, ForkNumber forknum,
						  BlockNumber blocknum, int nblocks);
extern bool bmapprefetch(SMgrRelation reln, ForkNumber forknum,
						 BlockNumber blocknum, int nblocks);
extern uint32 bmapmaxcombine(SMgrRelation reln, ForkNumber forknum,
							 BlockNumber blocknum);
extern void bmapreadv(SMgrRelation reln, ForkNumber forknum,
					  BlockNumber blocknum, void **buffers,
					  BlockNumber nblocks);
extern void bmapprepareread(SMgrRelation reln, ForkNumber forknum,
							BlockNumber blocknum, BlockNumber nblocks);
extern void bmapstartreadv(PgAioHandle *ioh, SMgrRelation reln,
						   ForkNumber forknum, BlockNumber blocknum,
						   void **buffers, BlockNumber nblocks);
extern void bmapwritev(SMgrRelation reln, ForkNumber forknum,
					   BlockNumber blocknum, const void **buffers,
					   BlockNumber nblocks, bool skipFsync);
extern void bmapwriteback(SMgrRelation reln, ForkNumber forknum,
						  BlockNumber blocknum, BlockNumber nblocks);
extern BlockNumber bmapnblocks(SMgrRelation reln, ForkNumber forknum);
extern void bmappretruncate(SMgrRelation reln, ForkNumber forknum);
extern void bmaptruncate(SMgrRelation reln, ForkNumber forknum,
						 BlockNumber curnblk, BlockNumber nblocks);
extern void bmapposttruncate(SMgrRelation reln, ForkNumber forknum);
extern void bmapimmedsync(SMgrRelation reln, ForkNumber forknum);
extern void bmapregistersync(SMgrRelation reln, ForkNumber forknum);
extern int	bmapfd(SMgrRelation reln, ForkNumber forknum,
				   BlockNumber blocknum, uint32 *off);

/* remove a database's unused block map from a tablespace's directory */
extern void bmap_drop_unused(Oid spcOid, Oid dbOid, const char *path);

/*
 * Folds: a fold of a relation is a moment its pages are kept as they are,
 * each page as of then held by the relation's changes list once it first
 * changes, so what changed since can be found without reading the rest.
 */
typedef struct BmapFold
{
	uint64		lineage;		/* the changes list's; its copies keep it */
	uint32		epoch;			/* the fold's */
	BlockNumber nblocks;		/* the relation's length then */
} BmapFold;

typedef void (*BmapChangeVisit) (const BmapChange *change, void *arg);

extern void bmap_fold_begin(SMgrRelation reln, BmapFold *fold);
extern bool bmap_folded(SMgrRelation reln);
extern bool bmap_fold_changes(SMgrRelation reln, const BmapFold *since,
							  BmapChangeVisit visit, void *arg);
extern void bmap_fold_prune(SMgrRelation reln, const BmapFold *keep);
extern void bmap_fold_forget(SMgrRelation reln);
extern void bmap_read_old(SMgrRelation reln, BlockNumber blkno,
						  BlockNumber old_block, char *page);

/*
 * Snapshots of a tree map: a relation's pages as they were, kept by its root
 * page, each a reference to the subtrees it had then, taken and let go of in
 * O(1) of the relation's size; a new relation can be made of one.  At most
 * BMAP_SNAPSHOTS_MAX of them, gone with the relation's storage: a commit's
 * pages are kept by captured roots instead (below).  The
 * caller of bmap_snapshot_take() keeps the relation from changing, as for a
 * share.  A snapshot is kept only once the (sub)transaction taking it
 * commits: one whose taker aborted, or was ended by a crash, is listed by
 * nobody and let go of by the relation's next snapshot or its page store's
 * next sweep.
 */
#define BMAP_SNAPSHOTS_MAX	16

typedef struct BmapSnapshot
{
	uint64		id;				/* names it until it is let go of */
	BlockNumber nblocks;		/* the relation's length then */
} BmapSnapshot;

extern uint64 bmap_snapshot_take(SMgrRelation reln);
extern void bmap_snapshot_take_many(SMgrRelation *rels, int n, uint64 *ids);
extern bool bmap_snapshot_release(SMgrRelation reln, uint64 id);
extern int	bmap_snapshot_list(SMgrRelation reln, BmapSnapshot *snaps, int max);
extern bool bmap_snapshot_unchanged(SMgrRelation reln, uint64 id);
extern void bmap_share_snapshot(SMgrRelation src, uint64 id, SMgrRelation dst,
								bool replacing);
extern uint64 bmap_snapshot_carry(SMgrRelation src, uint64 id,
								  SMgrRelation dst);

/*
 * Captured roots: a relation's tree as it was at a moment, kept by its
 * page store (not by the relation), so any number of them, each outliving
 * the relation's storage until it is let go of, and any relation of the
 * store may be made of it, another branch's included.  Capturing one is a
 * snapshot's work (O(1) in the relation's size, but for the pages changed
 * since its last capture or snapshot, logged as images and written out),
 * and a relation can be made of one in O(1) (bmap_share_root()).  A frozen
 * root (bmap_root_capture_logged()) is captured of temporary storage
 * holding frozen copies of a relation's pages changed since its last root,
 * the rest shared with that root, and records the relation and its epoch
 * then, so the next one's changed pages are found by their epochs
 * (bmap_root_base(), bmap_changed_pages()).  bmap_root_copy() holds a root
 * again by a root of its own, the same subtrees, in O(1).  A root
 * is held once its capturing (sub)transaction commits, and let go of once
 * the (sub)transaction that lets go of it commits: one whose capturer
 * aborted, or was ended by a crash, holds nothing, and neither does one
 * whose releaser committed; the page store's next captures or sweep reuse
 * their slots.  A root is named by an id (0: none) unique in its store.
 */
typedef uint64 BmapRootId;

extern BmapRootId bmap_root_capture(SMgrRelation reln);
extern void bmap_root_capture_many(SMgrRelation *rels, int n, BmapRootId *ids);
extern BmapRootId bmap_root_capture_logged(SMgrRelation reln, RelFileNumber of,
										   uint32 live_epoch);
extern BmapRootId bmap_root_copy(RelFileLocator store, BmapRootId id,
								 RelFileLocator holder);
extern bool bmap_root_base(SMgrRelation live, BmapRootId id, uint32 *since);
extern BmapRootId bmap_made_of(SMgrRelation reln);
extern uint32 bmap_changed_pages(SMgrRelation reln, uint32 since,
								 BlockNumber **blknos, BlockNumber *nblocks);
extern uint32 bmap_raise_epoch(SMgrRelation reln);
extern bool bmap_root_release(RelFileLocator store, BmapRootId id);
extern bool bmap_root_held(RelFileLocator store, BmapRootId id,
						   BlockNumber *nblocks);
extern bool bmap_root_unchanged(SMgrRelation reln, BmapRootId id);
extern void bmap_share_root(BmapRootId id, SMgrRelation dst,
							SMgrRelation replacing, SMgrRelation sharer);
extern uint32 bmap_roots_reclaim(RelFileLocator store, bool settle);

/* inspection, for tests */
extern void bmap_inspect_roots(RelFileLocator store, uint64 *held,
							   uint64 *slots, uint64 *naming);
extern void bmap_inspect_block(SMgrRelation reln, BlockNumber blkno,
							   BlockNumber *phys, uint32 *epoch, uint32 *refs);
extern uint64 bmap_inspect_map_blocks(SMgrRelation reln);
extern uint32 bmap_inspect_epoch(SMgrRelation reln);
typedef void (*BmapNodeInspect) (uint16 level, BlockNumber first,
								 BlockNumber block, uint32 refs, void *arg);
extern void bmap_inspect_tree(SMgrRelation reln, BmapNodeInspect visit,
							  void *arg);
extern void bmap_inspect_allocator(RelFileLocator rel, BlockNumber *hw,
								   uint32 *nfree);
extern bool bmap_inspect_store_sync(RelFileLocator rel, uint64 *written,
									uint32 *flushing);

/*
 * The sweep: what a database's page store in a tablespace holds counted
 * exactly, blocks nothing holds freed, stuck counts rewritten, the store
 * shortened.
 */
typedef struct BmapSweep
{
	uint64		relations;		/* maps read */
	uint64		freed;			/* blocks nothing held */
	uint64		recounted;		/* stuck counts rewritten */
	uint64		truncated;		/* blocks the store was shortened by */
	uint64		moved;			/* blocks moved down for it */
	uint64		stuck;			/* counts left stuck: past what one holds */
} BmapSweep;

extern void bmap_sweep(Oid spcOid, Oid dbOid, BmapSweep *result);

/*
 * A page sweep holds each relation it compacts, or lowers the shared_below
 * of, against its drop (AccessShareLock on it and on its branch), and a
 * compaction stops its moves once another waits for that hold.  One who
 * only tries a lock the hold conflicts with isn't waiting: it asks instead.
 * If a sweep holds 'tag', bmap_compaction_let_go() asks it to let go (a
 * compaction after the move under way, a lowering once done) and waits
 * until that hold has ended: true then, false if none held it, or if
 * lock_timeout passed since *start (set by the first call that waits, if
 * 0: one deadline for all the tries of an operation).  Asked, the sweep
 * passes over the branch (or the relation of none) for the rest of its
 * run.  Either way the lock is to be tried again.
 */
extern bool bmap_compaction_let_go(const LOCKTAG *tag, TimestampTz *start);

/*
 * The release queue: a dropped map's subtrees and blocks, queued as it is
 * dropped (in O(1) of the relation's size: its root page's pointers, and
 * its changes list's blocks), each holding what it names until a drain
 * lets go of it as a drop of the map would have: a count one less, or the
 * block freed, a node's children queued in its place.  A step is one
 * record: some entries popped, what they let go of, the children pushed.
 * Steps are taken by one backend of a page store at a time; another, or a
 * sweep counting holders, makes a step take none.  A relation dropped at a
 * commit is queued before the commit record, its entries naming the
 * transaction: they hold nothing until it commits (a step stops at them),
 * and are popped with nothing let go of if it doesn't.  With autovacuum
 * off, a relation's extension takes steps first when the store would grow.
 */
typedef struct BmapRelease
{
	uint64		entries;		/* entries popped */
	uint64		freed;			/* blocks freed */
	uint64		unrefs;			/* counts made one less (or more) */
	uint64		discarded;		/* of those popped, of drops that didn't commit */
} BmapRelease;

extern bool bmap_release_step(Oid spcOid, Oid dbOid, bool wait,
							  BmapRelease *done);
extern void bmap_release_drain(Oid spcOid, Oid dbOid, uint64 max_entries,
							   BmapRelease *done);
extern uint64 bmap_release_pending(Oid spcOid, Oid dbOid);
extern bool bmap_sweep_due(Oid spcOid, Oid dbOid);

/*
 * Visit the blocks of a database's page store in a tablespace that are
 * allocated as the scan begins, each once whatever maps hold it, as read
 * from the store (pages in shared buffers aren't seen until written).  A
 * block written meanwhile is visited until two reads of it agree, so no
 * version of it is seen only torn.  A sweep (which may free blocks and
 * shorten the store) waits for it, and so does another scan.
 *
 * The store is visited as ranges of blocks, in order, each told to 'range'
 * first: changed, its allocated blocks then visited; or not, and none is
 * read, because no block of it was written or freed since the scan whose
 * token is 'since' began, and what that scan visited of the range holds
 * still (its caller keeps that).  A 'since' of 0, or of a scan whose
 * watching was lost (a restart, a crash, the store dropped or watched no
 * more, or grown past its bits), has every range changed; and the ranges
 * may differ from that scan's then.  This scan's token is set in *token
 * (0: the store has none).  Returns the blocks read.
 */
typedef void (*BmapBlockVisit) (const char *page, void *arg);
typedef void (*BmapRangeVisit) (uint32 range, bool changed, void *arg);
extern uint64 bmap_scan_store(Oid spcOid, Oid dbOid, uint64 since,
							  uint64 *token, BmapRangeVisit range,
							  BmapBlockVisit visit, void *arg);

/*
 * What a database's page store in a tablespace takes, as it stands: its
 * files, and of the store's blocks those allocated, those a map holds under
 * its relation's length (each once, however many maps hold it), and those
 * held otherwise (by a changes list, or an entry past a relation's length).
 * Approximate while the store changes; a sweep waits for it.  visit, if
 * given, is told each relation's file number and logical length.
 */
typedef struct BmapSizes
{
	uint64		relations;		/* maps read */
	uint64		store_bytes;	/* the page store's files */
	uint64		allocated_blocks;	/* the allocator's: in use */
	uint64		mapped_blocks;	/* held by a map under its length */
	uint64		listed_blocks;	/* held otherwise, not mapped */
	uint64		map_bytes;		/* the maps' and changes lists' files */
	uint64		alloc_bytes;	/* the allocator's, the counts', the release
								 * queue's and the captured roots' files */
	uint64		vm_bytes;		/* the relations' other forks (md.c) */
	uint64		fsm_bytes;
} BmapSizes;

typedef void (*BmapRelationVisit) (RelFileNumber relnumber,
								   BlockNumber nblocks, void *arg);
extern void bmap_sizes(Oid spcOid, Oid dbOid, BmapRelationVisit visit,
					   void *arg, BmapSizes *sizes);

/* does a relation file number name a block-mapped relation? */
extern bool bmap_relation_exists(RelFileLocator rlocator);

#endif							/* BMAP_H */
