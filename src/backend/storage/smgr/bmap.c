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
 * The visibility map is mapped too, in the same tree past the main fork's
 * blocks (fork_key()), so that a share or a capture shares it with the
 * pages it describes, in O(1), and its pages are copied before they change
 * as the relation's are: a bit set or cleared is set or cleared in the
 * page's copy, under the page's lock taken before the change's critical
 * section, which is where the buffer manager prepares it.  A bit can be
 * set only by a change that holds the heap page too, so a share or a
 * capture, which finds every page of the relation it hasn't shared before
 * idle at once, keeps the map's bits as they hold for the pages it keeps.
 * Recovery replays the primary's copies; it never allocates, and a page of
 * the map it can't place (no block yet in a map newer than the replay, or a
 * length it extended past the header) goes nowhere, which loses only bits,
 * a map being right to claim less.  The free space map stays an md.c file
 * of the relation: its pages are written in recovery without WAL (a
 * standby records free space as it replays), where nothing could be
 * allocated or copied for them.  A share leaves the copy none
 * (RelationCreateStorageShared()), as a hint the copy builds again as it is
 * vacuumed: a copy's page with free space is shared, and filling it would
 * copy it first anyway.  But VACUUM skips the pages the visibility map says
 * are all-visible, and a copy's map is its source's, so the free space of
 * the pages shared all-visible would never be found: the header says how
 * many of the relation's first pages the free space map may know nothing
 * of (free_space_unknown, set by a share or a root's), and VACUUM reads
 * those it skips, only to record their free space, then has the header
 * say none (bmapfreespaceknown()).  Reading is all it does to them: a page
 * read isn't prepared, so nothing is copied.
 *
 * The map.  Block 0 of the map fork is a header holding the relation's
 * logical length and epoch, and block 1 its root page (BmapRoot), the
 * relation's alone, pointing to subtrees of fixed range (four, with 8 kB
 * blocks), which together reach MaxBlockNumber.  A subtree's
 * nodes are pages of the page store, read through shared buffers under the
 * store's own file number: a node points to nodes a level down, and a leaf
 * holds the entries of BMAP_TREE_FANOUT consecutive logical blocks.  An
 * entry is a physical block, the epoch it was born in (one born before its
 * map's epoch may be shared with another map), and the WAL position since
 * which it has that block.  Its block changes only while nothing else can
 * change the logical block: when a shared page is copied before it changes
 * (its buffer locked exclusively), when a hole is given its first block
 * (see below), or when an entry left past the
 * relation's end by a truncation is let go because another map shares its
 * block.  The map fork's existence is the relation's; smgropen() chooses
 * this storage manager by it.
 *
 * Holes.  A block below the length may have no entry: a hole, which reads
 * as zeroes, as a hole of md.c's file does where a write past its end
 * skipped blocks.  Writing past the end (smgrextend(), smgrpreextend())
 * maps the blocks written alone, so a relation reaching the last block a
 * relation can have costs the blocks it writes and the nodes on their
 * paths: a node is made only where a block under it is mapped, and walks
 * of the tree pass over subtrees it has none of.  A hole is given a block
 * when its page is first prepared for a change (bmapprepareforchange()),
 * which copies nothing, and that record comes before any record of the
 * change, as a copy's does, so redo finds the block mapped.  Zero-extension
 * through shared buffers (smgrzeroextend()) maps every block it adds, as
 * md.c allocates them: the buffer manager hands those pages out to be
 * initialized at once, settled as needing no preparing.  A read of a hole
 * starts no I/O (bmapprepareread()), and a page of zeroes written to one
 * is written nowhere.
 *
 * Sharing with other maps (a share, a copy of a captured root) raises the
 * header's shared_below to the map's epoch: an entry born since can be
 * shared only with the roots captured of the map (bmappagesharedelsewhere()),
 * which VACUUM changes as if unshared.  A sweep, having counted every
 * holder, lowers it to just past the newest entry another map still
 * holds (sweep_shared_below()), so pages the other map let go of are
 * vacuumed as unshared again.
 *
 * Sharing.  A node is held by every page that points to it, and a data
 * block by every leaf holding it, the reference counts
 * saying how many besides the first.  A count is four bytes, exact however
 * many maps share the block (branches of a table by the thousand), so the
 * last holder's going frees the block without a sweep; a holder that would
 * take a count past BMAP_REFS_MAX fails the change adding it, before its
 * record, rather than wrap the count or let it stick.  A share copies the root page alone,
 * each subtree gaining a holder; the first change under a shared node copies
 * the path down to the leaf (tree_copy_node()), each copy's children
 * gaining a holder, and the data block is then copied as any shared block
 * is.  A share makes the blocks it shares durable by syncing them, logging
 * images only of those the source got since the last checkpoint, which a
 * recovery may replay from before (bmapshare()).  A drop queues its
 * subtrees on its page store's release queue, in O(1) of its size; a
 * background drain lets go of each: a count less where another holds it, else freed, its
 * children let go of the same way (see "The release queue").  A node is an
 * incarnation of its block, which the pointer to it names by the record
 * that made it: a lookup that finds another there (a node freed and made
 * something else under it; in recovery, a version written before a crash)
 * looks again, or in recovery takes the subtree for replaced after the
 * record (tree_find()).  A map, or page store, of an older format (the flat
 * maps of version 1) is refused, not read (refuse_version()).
 *
 * Copies without an image.  With block_map_copy_images off, a shared page
 * copied before it changes is logged without its image when a full-page
 * write would leave the image out: the page
 * has changed since the redo pointer, so its image is in the WAL since.  The
 * copy's redo takes the page as replay rebuilt it in its buffer, or, where
 * replay may have skipped the page's records (its map on disk newer), from the
 * old block, which holds the page as the copy saw it (redo_remap_page()).  So
 * the old block keeps that page until no recovery of any server begins before
 * the copy.  Its reference-count page says it was copied from (a bit a block)
 * and where (the page's copy position, the newest copy from its blocks).
 * Until the redo pointer of the checkpoint last completed passes that position
 * (page_copy_holds()), the block isn't claimed in place by its other holder,
 * which copies the page instead and lets the block go; nor freed, its free
 * waiting on the release queue, whose drains pass over it (writing it back
 * below what they pop, so that it holds up nothing else); nor moved by a
 * compaction; and no hint is set on its page, which a recovery would copy.
 * Its first change in place or free after that is preceded by a PASSED record
 * (copies_passed()), unless one was logged for a later copy already (as
 * pg_control keeps the newest, across restarts).  The
 * record's redo moves the restart point of a standby or an archive recovery
 * past the copies it replayed, asking for a restartpoint if need be, so that a
 * crash of that server never replays a copy after the block changed
 * (redo_copies_passed()); a backup taken on a standby meanwhile fails as it
 * stops.  A restartpoint can't always be made (none is while recovery holds
 * references to pages it couldn't find): the redo then warns and goes on,
 * and so does a share's whose synced blocks the restart point isn't past
 * (redo_share_synced()).  Until a restartpoint past the position kept in
 * pg_control is made (at a later checkpoint record replayed, once the
 * cause is gone), a crash of that server is one it can't recover from: its
 * recovery would begin at or before the position, which it refuses
 * (FATAL) at every start, so it has to be made again from a new base
 * backup.  That failing safe, rather than replaying a copy from a changed
 * block, is the price of the images left out, paid only by a server whose
 * restartpoints fail.  Crash recovery of the primary begins at that
 * checkpoint, past the copies.  Both the record and its redo keep the
 * copy's position in
 * pg_control, as a share's synced blocks are kept, where it keeps pg_rewind
 * from rewinding a server from this one (which would replay from before the
 * copy over a page store copied after the change), and any recovery from
 * beginning before it.  A copy logs the image all the same while a base backup
 * runs (which replays from before the copy, and may copy the block after it
 * changes: one begun meanwhile begins at a checkpoint begun after the copy
 * did, which makes XLogInsert() add the image), without full-page writes,
 * for a relation whose data skips WAL, and for a visibility map page (whose
 * LSN doesn't say whether its changes are all in the WAL).  A later copy
 * that finds the page's position passed clears its bits.  What this costs: a
 * copy where a claim would do, and blocks held, until the next checkpoint.
 *
 * The standby hold.  A standby's recovery may begin before the primary's
 * checkpoint: at its restart point, which lags, or at the start of an
 * online backup taken on it.  Each standby reports where (GetRecoveryHold(),
 * the earliest of its own and of the standbys it serves) and whether a
 * backup runs on it, in its status replies; its physical slot keeps the
 * position while it is away, and across a restart of this server
 * (WalSndComputeRecoveryHold(), which publishes the earliest over the
 * standbys connected and the slots kept: bmap_set_standby_hold()).  The
 * copies' horizon is then the earliest of this server's restart point and
 * that (copy_horizon()): a block copied from without an image after it is
 * kept as it is, as above, until every standby known is past the copy, so
 * no PASSED record's redo needs a restartpoint of a standby known; and a
 * share syncs only the blocks its source got before it, so neither does a
 * share's.  While a backup runs on a standby, copies and shares log the
 * images they would leave out (backup_state()), as for one of this
 * server's, so its replay relies on nothing synced and the backup stops as
 * a good one.  A hold more than block_map_standby_hold_max_size of WAL
 * behind is let go of, as a slot past max_slot_wal_keep_size is; then, and
 * for a standby nobody knows of (one reading WAL from an archive), and for
 * what was copied before a report arrived, the restartpoints and the
 * backup's failure above remain, failing safe.
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
 * new logical block, unless the record mapping it carries the page's image
 * or redo takes the page from elsewhere (a copy's: see bmapprepareforchange()),
 * never reading the block.  Copies grow the store by several blocks at
 * once, which md.c allocates without writing them: the blocks past the
 * high-water mark are zeroes the allocator gives out next, as a crash
 * leaves those of an extension whose record it lost.  Truncation shortens the logical length, and once
 * it is done (outside its critical section) lets go of the blocks past it,
 * freeing those no other map holds.  A crash in
 * between leaves them mapped past the length, where a later truncation or
 * extension lets go of them (an extension past them, which leaves them in
 * a hole, first), or dropping the relation frees them.
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
 * needs (the reference counts', the allocator's)
 * first and locks the map page last (map_lock(), which assert-enabled
 * builds check at every read).  The one exception is a node just made: it
 * maps no page a write could look up until the page that points to it is
 * locked, last.  Nodes are made and copied under the header held
 * exclusively (a change prepared under it held shared lets go and takes it
 * so to copy a path); a write looks its page up from the root page, never
 * the header, which the backend writing it out may hold exclusively.
 * A relation's pages come before its map: preparing a page for a change
 * (bmapprepareforchange()), or asking whether it is shared, reads the
 * header and the map with the page's buffer locked, exclusively for a
 * change, so nothing waits for a page's content lock while holding a map
 * page of the relation.  A share or a capture, which reads the
 * relation's own pages to log them with the header held exclusively, only
 * tries their locks; it lets go of the header to wait for one
 * (preserve_header_lock()).
 *
 * The sweep (bmap_sweep()).  Freeing is by reference count, which leaks in
 * two cases: a crash between a drop's commit and the records queuing what it
 * didn't queue before its commit record (a prepared transaction's drops), and
 * a crash (or an error, not an interrupt, which the header's lock holds off)
 * between the records letting go of a truncation's blocks and those freeing
 * them.  (A path copy's crash or error between its records leaves children
 * counted once too often, likewise freed by a sweep once nothing holds
 * them.)  A sweep drains the release queue, then
 * counts exactly what every map of a database's page store holds, each
 * relation's read under its header's lock, and what the queue still holds
 * (drains held off meanwhile), and frees the blocks allocated as it began that
 * nothing holds (their counts made zero first), moves the blocks past
 * the allocated count down into free ones (compact_store()), and shortens the
 * store to its last allocated block (SHRINK, which redo truncates by too),
 * unless another sweep of it runs, which may still free blocks past that.
 * Branch creation (a share, the one thing that adds holders) waits for it
 * while it counts, and the allocator reuses no freed block of the store while
 * a sweep of it runs: one freed and taken again meanwhile could be held by a
 * map the sweep read before.  A dropped relation's map counts until it is
 * removed, once its blocks are queued (a tree's root page is written out with
 * no subtrees once they are queued, as the nodes it named may be freed); an
 * empty main fork file then keeps its relation file number until the next
 * checkpoint, as md.c's does.  A tree's node counts once for each page
 * pointing to it, its children once however many reach it.  A path copy (not
 * held off) adds holders only to blocks held already, so it never gives one
 * to a block the sweep found held by nothing; the sweep rewrites no count of
 * a block something holds, which a copy under way could make a holder short.
 * What the captured roots hold isn't counted anew by each sweep: it is kept
 * counted, beside the reference counts, a sweep adding the roots captured
 * since the last and a drain taking off what the roots let go of as it lets
 * go of it, so that a sweep's cost doesn't grow with the roots held; counts
 * a failed or crashed sweep left half written are counted anew, all roots
 * then walked (see "The roots' counts").
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
 * Diffs (bmap_diff()).  Two maps of a page store, relations' or captured
 * roots', differ only under the pointers that differ between them, since a
 * change under a node another map holds copies the path to it first.  A
 * diff compares the pointers and reads only the nodes whose pointers
 * differ, so it costs what differs, not the maps' sizes.
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
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xloginsert.h"
#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_tablespace.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "common/hashfn.h"
#include "common/int.h"
#include "common/relpath.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/pg_bitutils.h"
#include "postmaster/bgwriter.h"
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
#include "storage/reinit.h"
#include "storage/spin.h"
#include "storage/subsystems.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"
#include "utils/relfilenumbermap.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

bool		debug_block_mapped_heap = false;
int			block_map_replay_entries = 65536;
bool		block_map_copy_images = true;
int			block_map_standby_hold_max_size = -1;

/* ----------------------------------------------------------------
 * Page layouts
 * ----------------------------------------------------------------
 */

#define BMAP_MAGIC			0x424D4150	/* "BMAP" */
/*
 * The format's version, which headers and allocators' metapages carry.
 * Version 1 maps were flat, an entry per logical
 * block in the relation's own map fork; version 2's were trees of pages of
 * the page store, whose snapshots didn't say who took them; version 3's
 * release queue entries didn't say which drop's transaction they wait for;
 * version 4's headers didn't say which entries may be shared with another
 * map; version 5's root pages didn't name the map, or the captured root it
 * was made a copy of; version 6's captured roots didn't say of whose pages
 * they hold frozen copies; version 8's roots' metapages didn't say which
 * pages of slots may name a transaction; version 9's root pages kept
 * snapshots of the relation's tree; version 10's root pages and captured
 * roots didn't say of which lineage they are (a restore's storage
 * carrying the replaced storage's on), nor a frozen root of which map of
 * its relation; version 11's headers held another epoch before
 * shared_below; version 12's captured roots didn't keep the database's
 * oldest unfrozen transaction and multixact as they were captured; version
 * 13's reference counts were a byte a block, stuck at 255; version 14's
 * reference-count pages didn't keep the roots' counts, nor its release
 * queue entries whether those counted them; version 15's maps had an entry
 * for every block below the relation's length, which a server reading
 * them could count on (a map now has holes); version 16's reference-count
 * pages didn't say which blocks were copied without an image, nor where;
 * version 17's maps held the main fork alone, the visibility map a file of
 * the relation's own.  This server reads only version 18 and refuses
 * anything older (refuse_version()).
 */
#define BMAP_VERSION		19

#define BMAP_PAGE_META		1
#define BMAP_PAGE_HEADER	2
#define BMAP_PAGE_BITMAP	4
#define BMAP_PAGE_REFS		5
#define BMAP_PAGE_ROOT		8		/* a tree's root page: block 1 */
#define BMAP_PAGE_TREE		9		/* a tree's node, in the page store */
#define BMAP_PAGE_QUEUE_META	10	/* the release queue's metapage */
#define BMAP_PAGE_QUEUE		11		/* and its pages of entries */
#define BMAP_PAGE_ROOTS_META	12	/* the captured roots' metapage */
#define BMAP_PAGE_ROOTS		13		/* and their pages of slots */

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
	uint32		shared_below;	/* entries born before it may be shared
								 * with another map; those born since, only
								 * with the roots captured of the map */
	BlockNumber vm_nblocks;		/* its visibility map's length */
	BlockNumber free_space_unknown; /* pages from 0 whose free space the
									 * free space map may not know */
	FullTransactionId creator;	/* the transaction that made it, which
								 * drops it if it aborts; invalid for one
								 * made outside any (see bmap_creating()) */
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

/*
 * A reference-count page's blocks: a count each, a uint32, then a roots'
 * count each, another uint32 (see "The roots' counts"), then a copied
 * bit each (see "Copies without an image").  A roots' count is as wide as
 * a count, so that the roots of a long version history (one for each
 * commit, every one of them pointing to the same pages of a table that
 * doesn't change) never take it to its limit.  The page's copy position,
 * an XLogRecPtr, ends its contents.  The copied bits follow the uint32s,
 * as bytes, and the position is read and written with memcpy(), so
 * nothing on the page is misaligned.
 */
#define BMAP_REFS_PER_PAGE \
	((uint32) (((BMAP_CONTENT_SIZE - sizeof(XLogRecPtr)) * 8) / \
			   ((sizeof(uint32) + sizeof(uint32)) * 8 + 1)))
#define BMAP_REFS_COPIED_OFFSET \
	(BMAP_REFS_PER_PAGE * (sizeof(uint32) + sizeof(uint32)))
#define BMAP_REFS_COPY_LSN_OFFSET	(BMAP_CONTENT_SIZE - sizeof(XLogRecPtr))

StaticAssertDecl(BMAP_REFS_COPIED_OFFSET + (BMAP_REFS_PER_PAGE + 7) / 8 <=
				 BMAP_REFS_COPY_LSN_OFFSET,
				 "a reference-count page's copied bits fit before its copy position");

/* the release queue's page and slot of entry 'i' */
#define BMAP_QUEUE_PER_PAGE \
	((uint32) (BMAP_CONTENT_SIZE / sizeof(BmapQueued)))
#define QUEUE_PAGE(i)		((BlockNumber) (1 + (i) / BMAP_QUEUE_PER_PAGE))
#define QUEUE_SLOT(i)		((uint32) ((i) % BMAP_QUEUE_PER_PAGE))

/*
 * The most a count says: a block held by this many maps besides the first
 * takes no other holder (refs_add_holders() fails the change instead).  One
 * short of a uint32's largest value, so that a block's holders, the count
 * and one, fit the uint32 the sweep counts them in.  A test lowers it to
 * BMAP_REFS_TEST_MAX with an injection point.
 */
#define BMAP_REFS_MAX		((uint32) (PG_UINT32_MAX - 1))
#define BMAP_REFS_TEST_MAX	1000

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
 * than a leaf maps has one level, and together they map every block a
 * relation can have: the last alone maps past MaxBlockNumber.  No subtree
 * ever gets a level above it, so a pointer's
 * node is replaced only by its copy, by a node made where there was none,
 * or by nothing (see tree_find()).
 */
#define BMAP_TREE_TOPS		BMAP_DELTA_ROOT_TOPS

StaticAssertDecl(BMAP_TREE_TOPS >= 4 && BMAP_TREE_TOPS <= 6 &&
				 (uint64) BMAP_TREE_FANOUT * BMAP_TREE_FANOUT *
				 BMAP_TREE_FANOUT * BMAP_TREE_FANOUT *
				 (BMAP_TREE_TOPS > 4 ? BMAP_TREE_FANOUT : 1) *
				 (BMAP_TREE_TOPS > 5 ? BMAP_TREE_FANOUT : 1) >
				 (uint64) MaxBlockNumber,
				 "a block map's subtrees must reach MaxBlockNumber");

/*
 * The visibility map's blocks are the tree's too, past the main fork's:
 * block b of the fork is the tree's logical block BMAP_VM_BASE + b, so a
 * share, a capture, a copy before a change and every other change of the
 * tree treat the map's pages as they treat the relation's.  BMAP_VM_SPAN is
 * as many blocks as the map of the longest main fork (BMAP_VM_BASE blocks)
 * has, at two bits a block, so the main fork stops short of BMAP_VM_BASE,
 * which begins a leaf (every subtree's first block is a multiple of a
 * leaf's span): no leaf holds both forks' entries, so a run of a leaf's
 * entries is of one fork.
 *
 * So a block-mapped relation's main fork holds fewer blocks than md.c's
 * can: BMAP_VM_BASE, which with 8 kB blocks is 4,294,835,619, 131,676
 * blocks (about 1 GB) short of md.c's limit of InvalidBlockNumber blocks.  An
 * extension past it fails as md.c's past its own does
 * (check_extend_limit()), saying the lower limit.
 *
 * Within this module a "key" is a block of the tree, of either fork; a
 * relation's buffer names the fork and the block within it (key_fork(),
 * key_block()).
 */
#define BMAP_VM_BLOCKS_PER_PAGE \
	((uint32) ((BLCKSZ - MAXALIGN(SizeOfPageHeaderData)) * 4))
#define BMAP_VM_SPAN \
	((BlockNumber) ((uint64) MaxBlockNumber / (BMAP_VM_BLOCKS_PER_PAGE + 1) + 2))
#define BMAP_VM_BASE \
	((BlockNumber) ((((uint64) MaxBlockNumber + 1 - BMAP_VM_SPAN) / BMAP_TREE_FANOUT) * \
					BMAP_TREE_FANOUT))

StaticAssertDecl((uint64) BMAP_VM_SPAN * BMAP_VM_BLOCKS_PER_PAGE >= (uint64) BMAP_VM_BASE,
				 "the visibility map's span must hold the map of the longest main fork");

/* the forks the tree holds */
static inline bool
fork_mapped(ForkNumber forknum)
{
	return forknum == MAIN_FORKNUM || forknum == VISIBILITYMAP_FORKNUM;
}

/* the tree's block of block 'blkno' of fork 'forknum' */
static inline BlockNumber
fork_key(ForkNumber forknum, BlockNumber blkno)
{
	Assert(fork_mapped(forknum));
	if (forknum == VISIBILITYMAP_FORKNUM)
	{
		Assert(blkno < BMAP_VM_SPAN);
		return BMAP_VM_BASE + blkno;
	}
	return blkno;
}

/* the fork of the tree's block 'key', and its block within the fork */
static inline ForkNumber
key_fork(uint64 key)
{
	return key >= BMAP_VM_BASE ? VISIBILITYMAP_FORKNUM : MAIN_FORKNUM;
}

static inline BlockNumber
key_block(uint64 key)
{
	return (BlockNumber) (key >= BMAP_VM_BASE ? key - BMAP_VM_BASE : key);
}

/* the first key of the fork of 'key', and the key past its length */
static inline uint64
key_fork_start(uint64 key)
{
	return key >= BMAP_VM_BASE ? BMAP_VM_BASE : 0;
}

static inline uint64
key_fork_end(BlockNumber nblocks, BlockNumber vm_nblocks, uint64 key)
{
	return key >= BMAP_VM_BASE ? (uint64) BMAP_VM_BASE + vm_nblocks : nblocks;
}

/*
 * How an image of the page of 'key' is logged: a visibility map's page keeps
 * its bits where a standard page has its hole, so it is logged whole.
 */
static inline uint8
key_image_flags(uint64 key)
{
	return key >= BMAP_VM_BASE ? 0 : REGBUF_STANDARD;
}

/* is 'key' below its fork's length? */
static inline bool
key_in_length(BlockNumber nblocks, BlockNumber vm_nblocks, uint64 key)
{
	return key < key_fork_end(nblocks, vm_nblocks, key);
}

/* does [first, end) hold a key below its fork's length? */
static inline bool
range_in_length(BlockNumber nblocks, BlockNumber vm_nblocks, uint64 first,
				uint64 end)
{
	return (first < nblocks) ||
		(end > BMAP_VM_BASE && vm_nblocks > 0 &&
		 Max(first, (uint64) BMAP_VM_BASE) < (uint64) BMAP_VM_BASE + vm_nblocks);
}

/*
 * A frozen root's copy's live_epoch (bmap_root_copy()): frozen, the holder's
 * changes since it told by its own epoch.
 */
#define BMAP_LIVE_EPOCH_COPY	PG_UINT32_MAX

typedef struct BmapRoot
{
	BmapEntry	top[BMAP_TREE_TOPS];
	uint64		map_id;			/* the map's identity: where its record began */
	uint64		made_of;		/* the captured root it was made a copy of */
	uint64		lineage;		/* the map's lineage (0: map_id, its own) */
} BmapRoot;

/*
 * A map's lineage: the identity its storage's pages are told by across
 * new storage.  A map begins its own (its map_id, which no other map has),
 * but one made of a captured root to take a relation's storage's place
 * (a restore) carries on that storage's, so that the roots captured of
 * either are of the relation still and nothing names the storage itself.
 */
static inline uint64
root_lineage(const BmapRoot *root)
{
	return root->lineage != 0 ? root->lineage : root->map_id;
}

StaticAssertDecl(sizeof(xl_bmap_top) == sizeof(BmapEntry),
				 "a captured root's pointer is a map entry");
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
#define PageGetBmapRefs(page)	((uint32 *) PageGetContents(page))
#define PageGetBmapRootRefs(page) \
	((uint32 *) (PageGetContents(page) + BMAP_REFS_PER_PAGE * sizeof(uint32)))
#define PageGetBmapCopiedBits(page) \
	((uint8 *) (PageGetContents(page) + BMAP_REFS_COPIED_OFFSET))

/*
 * A block's roots' count: how many of the captured roots' pages hold it,
 * in 31 bits, and whether it is a node they reach (whose own pointers they
 * count), in the top bit.  A count at ROOTREF_MAX says at least that many.
 */
#define ROOTREF_IN			((uint32) 0x80000000)
#define ROOTREF_MAX			((uint32) 0x7FFFFFFF)
#define ROOTREF_COUNT(v)	((v) & ROOTREF_MAX)
#define PageGetBmapQueueMeta(page)	((BmapQueueMeta *) PageGetContents(page))
#define PageGetBmapQueued(page)	((BmapQueued *) PageGetContents(page))

static const PGIOAlignedBlock zero_block;

/* sweeps running whose page stores are listed, at most */
#define BMAP_SWEEPS_LISTED	64

/* page stores swept since the server started, remembered at most */
#define BMAP_SWEPT_MAX		1024

/* A page store whose sweep began since the server started */
typedef struct BmapSwept
{
	Oid			spcOid;
	Oid			dbOid;
	bool		swept;			/* a sweep of it ended */
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

/* page stores whose durability is followed (BmapStoreSync), at most */
#define BMAP_SYNCS_FOLLOWED	64

/*
 * What a page store's shares and captures wrote out, named by its
 * watch_key() (0: a free slot): the flushes of a share's or capture's own
 * blocks begun and not yet written, and those written.  A share holding no
 * block of its own needs no flush of its own when none is in flight
 * (store_written_out()): each block it holds was written by the share or
 * capture that first shared it.  A flush given up part way (its
 * transaction ended by an error before or during its write) may have left
 * its pages only in their buffers: it poisons the slot instead, which is
 * written out again only once a checkpoint that began after it has
 * completed (written every page dirty when it began), poison_lsn below
 * that checkpoint's redo pointer.  Letting go of a slot is safe but for
 * one with a flush in flight, whose end must find it, or one poisoned
 * that no checkpoint covers yet; the least recently used of the others is
 * let go of for a store that needs a slot.  A flush finding none to be had
 * is counted in BmapShared instead (unfollowed_flushing, and once given up
 * unfollowed_poison_lsn), and no store is written out while it is in
 * flight, or while its poison lasts.  Durability is WAL's: each own block
 * is logged as an image before the share's or capture's record.
 */
typedef struct BmapStoreSync
{
	uint64		key;
	uint32		flushing;
	uint64		written;
	XLogRecPtr	poison_lsn;		/* 0: none */
	uint64		used;			/* as sync_clock was, when last used */
} BmapStoreSync;

typedef struct BmapShared
{
	pg_atomic_uint32 sweeps;	/* running, anywhere */
	pg_atomic_uint64 watch_key[BMAP_WATCHED_MAX];
	slock_t		mutex;			/* protects what follows */
	int			nlisted;
	Oid			listed[BMAP_SWEEPS_LISTED][2];	/* tablespace, database */
	int			unlisted;		/* past the most listed: every store is */
	int			nswept;
	BmapSwept	swept[BMAP_SWEPT_MAX];

	/*
	 * The newest copy made without an image a PASSED record has said is
	 * passed (see "Copies without an image")
	 */
	pg_atomic_uint64 copies_passed;

	/*
	 * The standby hold (see "The standby hold"): the earliest position a
	 * recovery of a standby this server knows of may begin at (0: none),
	 * and whether one runs an online backup.
	 */
	pg_atomic_uint64 standby_hold;
	pg_atomic_uint32 standby_backups;
	pg_atomic_uint64 run_since_xid;	/* the first transaction since recovery ended */

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

	/* the page stores whose syncs are followed (also the mutex's) */
	int			nsyncs_followed;
	uint64		sync_clock;
	uint32		unfollowed_flushing;	/* flushes no slot follows, in flight */
	XLogRecPtr	unfollowed_poison_lsn;	/* and given up, as of (0: none) */
	BmapStoreSync syncs_followed[BMAP_SYNCS_FOLLOWED];
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

/*
 * What each backend's page sweep holds (a compaction's, compact_store(), or
 * a lowering of shared_below's, sweep_shared_below()), in its slot: the
 * relation, held against its drop, the hold's generation (raised as each
 * hold begins and ends), and whether another session asked it to let go
 * of it.  A session that only tries its lock isn't in the lock's queue, which is all
 * LockHasWaiters() sees, so it asks here instead (bmap_compaction_let_go()),
 * and waits for the generation it asked of to pass.  The mutex protects
 * the slot but 'asked', which a compaction reads at each move.
 */
typedef struct BmapCompacting
{
	slock_t		mutex;
	bool		holding;
	uint64		generation;
	LOCKTAG		rel;
	pg_atomic_uint32 asked;
} BmapCompacting;

static BmapCompacting *bmap_compacting = NULL;

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
	ShmemRequestStruct(.name = "block map compactions",
					   .size = mul_size(MaxBackends, sizeof(BmapCompacting)),
					   .ptr = (void **) &bmap_compacting,
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
	LWLockInitialize(&bmap_shared->ahead_lock, LWTRANCHE_BMAP_REPLAY_AHEAD);
	pg_atomic_init_u32(&bmap_shared->nahead, 0);
	pg_atomic_init_u64(&bmap_shared->copies_passed, 0);
	pg_atomic_init_u64(&bmap_shared->standby_hold, InvalidXLogRecPtr);
	pg_atomic_init_u32(&bmap_shared->standby_backups, 0);
	pg_atomic_init_u64(&bmap_shared->run_since_xid, 0);
	pg_atomic_init_u64(&bmap_shared->queue_pushes, 0);
	for (int i = 0; i < MaxBackends; i++)
	{
		SpinLockInit(&bmap_compacting[i].mutex);
		bmap_compacting[i].holding = false;
		bmap_compacting[i].generation = 0;
		pg_atomic_init_u32(&bmap_compacting[i].asked, 0);
	}
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
	bmap_shared->nsyncs_followed = 0;
	bmap_shared->sync_clock = 0;
	bmap_shared->unfollowed_flushing = 0;
	bmap_shared->unfollowed_poison_lsn = InvalidXLogRecPtr;
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
	/* (nor are its syncs followed: no flush of it can be in flight) */
	for (int i = 0; i < bmap_shared->nsyncs_followed; i++)
		if (bmap_shared->syncs_followed[i].key == key &&
			bmap_shared->syncs_followed[i].flushing == 0)
			bmap_shared->syncs_followed[i].used = 0;
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

/* none of a dropped relation's map in shared buffers */
static void
drop_gone_buffers(SMgrRelation reln)
{
	ForkNumber	forks[1] = {BMAP_FORKNUM};
	BlockNumber firsts[1] = {0};

	if (!bmap_relation_exists(reln->smgr_rlocator.locator))
		DropRelationBuffers(reln, forks, 1, firsts);
}

typedef void (*DroppedRead) (SMgrRelation reln, void *arg);

/* the size of reln's fork's file, or -1 if it has none */
static off_t
fork_size(SMgrRelation reln, ForkNumber fork)
{
	RelPathStr	path = relpathperm(reln->smgr_rlocator.locator, fork);
	struct stat st;

	if (stat(path.str, &st) < 0)
		return errno == ENOENT ? -1 : 1;
	return st.st_size;
}

/*
 * Has reln's map gone, or is it going: its file unlinked, or emptied, as
 * md.c empties a fork's file just before unlinking it?  A map is never
 * empty otherwise, except as it is made, before its first pages.
 */
static bool
map_going(SMgrRelation reln)
{
	return fork_size(reln, BMAP_FORKNUM) <= 0;
}

/*
 * Read reln's map, which may be a dropped relation's, by read(): every map
 * of a page store is read so (a sweep's, a compaction's, a size
 * count's), a dropped relation's too until its files
 * are unlinked.  In a subtransaction, so that whether it returns or fails,
 * its pins are let go of and, if the relation is gone by then, none of its
 * pages stays in shared buffers, where a relation given its file number
 * later would find them.  (One read before the unlink, the files still
 * there as it ends, is the drop's to drop: smgrdounlinkall().)
 *
 * A read that fails because the files went meanwhile (emptied, then
 * unlinked, as the drop's commit removes them, or closed by its
 * invalidation and gone when opened again) is passed over: false, and what
 * read() left in *arg is as far as it got.  A dropped relation's map is
 * passed over by every reader once it is gone: what it held was queued
 * for release before its files went, so a sweep counts it from the queue.
 * Any other failure, and a cancel, a shutdown or running out of memory or
 * disk whatever the map's state, is thrown again.
 */
static bool
read_maybe_dropped(SMgrRelation reln, DroppedRead read, void *arg)
{
	MemoryContext oldcxt = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;
	ErrorData  *edata = NULL;

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(oldcxt);
	PG_TRY();
	{
		read(reln, arg);
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcxt);
		CurrentResourceOwner = oldowner;
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(oldcxt);
		edata = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcxt);
		CurrentResourceOwner = oldowner;
	}
	PG_END_TRY();
	drop_gone_buffers(reln);
	if (edata == NULL)
		return true;
	if (ERRCODE_TO_CATEGORY(edata->sqlerrcode) != ERRCODE_OPERATOR_INTERVENTION &&
		ERRCODE_TO_CATEGORY(edata->sqlerrcode) != ERRCODE_INSUFFICIENT_RESOURCES &&
		map_going(reln))
	{
		ereport(DEBUG1,
				(errmsg_internal("map of relation %u passed over, dropped as it was read: %s",
								 reln->smgr_rlocator.locator.relNumber,
								 edata->message)));
		FreeErrorData(edata);
		return false;
	}
	ReThrowError(edata);
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
 * A sweep of rel's page store begins, or ends.  Past the most remembered, a
 * store is not.
 */
static void
swept_note(RelFileLocator rel, bool begin)
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
	if (found != NULL && !begin)
		found->swept = true;
	SpinLockRelease(&bmap_shared->mutex);
}

const ShmemCallbacks BmapShmemCallbacks = {
	.request_fn = BmapShmemRequest,
	.init_fn = BmapShmemInit,
};

struct MapPage;
static void unshare_stale(SMgrRelation reln, const struct MapPage *leaf,
						  uint32 epoch, BlockNumber from, BlockNumber to,
						  bool release);
static void create_map(SMgrRelation reln);
static void let_go_range(SMgrRelation reln, Buffer headerbuf, uint64 from,
						 uint64 end);

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

static void store_flush_begin(RelFileLocator rel);
static void store_flush_end(RelFileLocator rel, bool given_up);
static bool store_written_out(RelFileLocator rel);
static XLogRecPtr last_checkpoint_redo(void);
static XLogRecPtr copy_horizon(void);
static XLogRecPtr queue_push(RelFileLocator rel, const BmapQueued *items,
							 uint32 n);

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
 * Whether the reference counts' file exists, and its length, asked of the
 * file system: md.c closes and opens the file again to answer the first,
 * and seeks for the second.  Every such question of the file goes through
 * these, which count them (bmap_refs_file_asks()), so a test can tell that
 * a change asks from the length known instead.
 */
static uint64 refs_file_asks = 0;

static bool
refs_file_exists(SMgrRelation rsrel)
{
	refs_file_asks++;
	return smgrexists(rsrel, MAIN_FORKNUM);
}

static BlockNumber
refs_file_nblocks(SMgrRelation rsrel)
{
	refs_file_asks++;
	return smgrnblocks(rsrel, MAIN_FORKNUM);
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
		 type == BMAP_PAGE_QUEUE_META) &&
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
static uint32
page_ref(Page page, BlockNumber phys)
{
	return PageIsNew(page) ? 0 : PageGetBmapRefs(page)[REF_SLOT(phys)];
}

/* a locked reference-count page's copy position (see "Copies without an image") */
static XLogRecPtr
page_copy_lsn(Page page)
{
	XLogRecPtr	lsn;

	if (PageIsNew(page))
		return InvalidXLogRecPtr;
	memcpy(&lsn, PageGetContents(page) + BMAP_REFS_COPY_LSN_OFFSET, sizeof(lsn));
	return lsn;
}

static void
page_set_copy_lsn(Page page, XLogRecPtr lsn)
{
	memcpy(PageGetContents(page) + BMAP_REFS_COPY_LSN_OFFSET, &lsn, sizeof(lsn));
}

/*
 * Was 'phys' copied from without an image, at or before its locked
 * reference-count page's copy position (*copy_lsn)?
 */
static bool
page_copied(Page page, BlockNumber phys, XLogRecPtr *copy_lsn)
{
	uint32		slot = REF_SLOT(phys);

	*copy_lsn = page_copy_lsn(page);
	return !PageIsNew(page) &&
		(PageGetBmapCopiedBits(page)[slot / 8] & (1 << (slot % 8))) != 0;
}

/*
 * May 'phys' not yet change in place, nor be freed?  It was copied from
 * without an image, perhaps after the redo pointer of the checkpoint last
 * completed ('horizon'), so that a recovery from there would copy it
 * again, and needs it as it is.  Otherwise, if it was copied from at all,
 * *passed says where at the latest (else it is left alone), for
 * copies_passed() before the change's record.
 */
static bool
page_copy_holds(Page page, BlockNumber phys, XLogRecPtr horizon,
				XLogRecPtr *passed)
{
	XLogRecPtr	copy_lsn;

	if (!page_copied(page, phys, &copy_lsn))
		return false;
	if (copy_lsn > horizon)
		return true;
	*passed = Max(*passed, copy_lsn);
	return false;
}

/*
 * Before a record that changes in place, or frees, a block copied from
 * without an image at or before 'passed' (InvalidXLogRecPtr: none), which
 * the restart point is past: a PASSED record, unless one said a copy no
 * older is passed already.  Its redo moves the restart point of a server
 * replaying it (a standby, a recovery from archives) past the copy before
 * the change, which a recovery from before the copy would read in place of
 * what the copy saw.  Its WAL precedes the record's, the shared position
 * set only once it is inserted.
 *
 * The same holds of any recovery from before the copy over files copied
 * after the change: a server pg_rewind made of this one, replaying from
 * the last checkpoint the two had in common.  So the copy's position is
 * kept in pg_control (XLogNoteSyncedPages()) before the change, as a
 * share's synced blocks are, where pg_rewind refuses such a source (and
 * one that changes such a block while it copies files), and recovery
 * refuses to begin at or before it.  The checkpoint the copy is past
 * begins this server's own recovery past it, and a base backup of it
 * begins past every copy logged without an image (one running makes
 * copies log their images).  The positions noted only grow, a record and
 * a note for each newer copy passed: few, as each is the newest copy from
 * a reference-count page's blocks.
 */
static void
copies_passed(XLogRecPtr passed)
{
	xl_bmap_passed xlrec;
	XLogRecPtr	lsn;

	if (XLogRecPtrIsInvalid(passed) ||
		passed <= pg_atomic_read_u64(&bmap_shared->copies_passed))
		return;

	/*
	 * Noted already, before a restart that forgot it: pg_control keeps the
	 * newest position noted, and every one noted was so by a record (this
	 * one, or a share's) whose redo moves a standby's restart point past it.
	 * Logging again would raise pg_control's count of such notes, by which a
	 * pg_rewind copying files meanwhile refuses its source.
	 */
	{
		XLogRecPtr	since = XLogSyncedSince();

		if (XLogRecPtrIsValid(since) && passed - 1 <= since)
		{
			pg_atomic_monotonic_advance_u64(&bmap_shared->copies_passed, since + 1);
			return;
		}
	}
	xlrec.horizon = Max(copy_horizon(), passed);
	xlrec.copied = passed;
	Assert(passed <= xlrec.horizon);
	XLogBeginInsert();
	XLogRegisterData(&xlrec, sizeof(xlrec));
	lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_PASSED);

	/*
	 * The record durable before the note: a note pg_control keeps stands
	 * for its record, which skips a later PASSED (above), so a crash
	 * between them mustn't leave the note alone.  (The copy's position is
	 * its record's end: a recovery beginning there doesn't replay it.)
	 */
	XLogFlush(lsn);
	XLogNoteSyncedPages(passed - 1);
	INJECTION_POINT("bmap-copies-passed-noted", NULL);
	pg_atomic_monotonic_advance_u64(&bmap_shared->copies_passed, passed);
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
	PageGetBmapHeader(page)->vm_nblocks = delta->vm_nblocks;
	PageGetBmapHeader(page)->epoch = delta->epoch;
	if (delta->flags & BMAP_DELTA_SHARED_BELOW)
		PageGetBmapHeader(page)->shared_below = delta->shared_below;
	if (delta->flags & BMAP_DELTA_FREE_SPACE)
		PageGetBmapHeader(page)->free_space_unknown = delta->free_space_unknown;
	if (delta->flags & BMAP_DELTA_CREATOR)
		PageGetBmapHeader(page)->creator = delta->creator;
}

/*
 * A delta that sets a header as it is: the caller changes what changes.
 */
static void
header_delta_of(Page page, xl_bmap_header_delta *delta)
{
	const BmapHeader *header = PageGetBmapHeader(page);

	memset(delta, 0, sizeof(*delta));
	delta->kind = BMAP_DELTA_HEADER;
	delta->flags = 0;
	delta->nblocks = header->nblocks;
	delta->vm_nblocks = header->vm_nblocks;
	delta->epoch = header->epoch;
	delta->shared_below = 0;
	delta->free_space_unknown = 0;
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
		/* (an entry already as the delta sets it changes nothing) */
		if (e->block == ent[i].block && e->epoch == ent[i].epoch &&
			(!(delta->flags & BMAP_DELTA_SINCE) || e->since == delta->since))
			continue;
		if (e->block != ent[i].block || (delta->flags & BMAP_DELTA_SINCE))
			e->since = delta->since;
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
	uint32	   *counts;

	if (delta->flags & BMAP_DELTA_INIT)
		init_page(page, BMAP_PAGE_REFS);
	if (delta->flags & BMAP_DELTA_COPIED_RESET)
		memset(PageGetBmapCopiedBits(page), 0, (BMAP_REFS_PER_PAGE + 7) / 8);
	if (delta->flags & BMAP_DELTA_COPIED)
	{
		Assert(delta->nrefs > 0 && ref[0].slot < BMAP_REFS_PER_PAGE);
		PageGetBmapCopiedBits(page)[ref[0].slot / 8] |=
			(uint8) (1 << (ref[0].slot % 8));
	}
	counts = PageGetBmapRefs(page);
	for (uint32 i = 0; i < delta->nrefs; i++)
	{
		Assert(ref[i].slot < BMAP_REFS_PER_PAGE && ref[i].count <= BMAP_REFS_MAX);
		counts[ref[i].slot] = ref[i].count;
	}
	/* (then the roots' counts) */
	for (uint32 i = delta->nrefs; i < delta->nrefs + delta->nroots; i++)
	{
		Assert(ref[i].slot < BMAP_REFS_PER_PAGE);
		PageGetBmapRootRefs(page)[ref[i].slot] = ref[i].count;
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
	root->made_of = delta->made_of;
	root->lineage = delta->lineage;
}

/* a captured root's pointers, as map entries */
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
	return sizeof(xl_bmap_refs_delta) +
		(d->nrefs + d->nroots) * sizeof(xl_bmap_ref_delta);
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
		if (!refs_file_exists(rsrel))
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
		nrefs = refs_file_nblocks(rsrel);
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
 * The length of the reference-count file last learned by this backend, or
 * InvalidBlockNumber: its storage manager's, which the backend sets as it
 * asks for the file's length or extends it, and forgets as the file is
 * closed for good (an invalidation, a drop).  The file only grows while it
 * exists (a sweep shortens the page store, never its counts), and goes
 * only with the page store, so the length it says is the file's, or less,
 * and every page below it exists.
 */
static inline BlockNumber
refs_known(SMgrRelation rsrel)
{
	return rsrel->smgr_cached_nblocks[MAIN_FORKNUM];
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

	/* (the file known to reach that far already: see refs_known()) */
	if (refs_known(rsrel) != InvalidBlockNumber && refs_known(rsrel) >= need)
		return;
	if (!refs_file_exists(rsrel))
	{
		RelFileLocator rloc = with_relnumber(rel, BMAP_REFS_RELNUMBER);

		smgrcreate(rsrel, MAIN_FORKNUM, false);
		log_smgrcreate(&rloc, MAIN_FORKNUM);
	}
	check_may_read();
	if (need > 0 && refs_file_nblocks(rsrel) < need)
		ReleaseBuffer(ExtendBufferedRelTo(BMR_SMGR(rsrel, RELPERSISTENCE_PERMANENT),
										  MAIN_FORKNUM, NULL,
										  EB_SKIP_EXTENSION_LOCK, need,
										  RBM_NORMAL));
}

/*
 * How many reference-count pages exist: blocks whose page is past them
 * count zero (pages a crash or a standby never extended the file to, as
 * they aren't logged).  A length known (refs_known()) says the file
 * exists, so it isn't looked for again, which md.c does by closing and
 * opening it: only its length is asked.
 */
static BlockNumber
refs_nblocks(RelFileLocator rel)
{
	SMgrRelation rsrel = refs_smgr(rel);

	if (refs_known(rsrel) == InvalidBlockNumber &&
		!refs_file_exists(rsrel))
		return 0;
	return refs_file_nblocks(rsrel);
}

/*
 * How many times this backend asked the file system whether a reference
 * counts' file exists, or its length (refs_file_exists(),
 * refs_file_nblocks()): a test's measure that a change asks from the
 * length known instead.
 */
uint64
bmap_refs_file_asks(void)
{
	return refs_file_asks;
}

/*
 * Does reference-count page 'page' exist?  Asked of the block a change
 * prepares, every time: answered from the length last known when it is
 * below that, without a system call.
 */
static bool
refs_has_page(RelFileLocator rel, BlockNumber page)
{
	BlockNumber known = refs_known(refs_smgr(rel));

	if (known != InvalidBlockNumber && page < known)
		return true;
	return page < refs_nblocks(rel);
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
		/*
		 * (looked up only: a hint's lookup comes after the writes it hints
		 * at, by when the relation may be dropped and its map's file gone,
		 * and a page not cached is read, if at all, at once)
		 */
		Buffer		recent = LookupSharedBuffer(floc, fork, blk);

		if (BufferIsValid(recent) &&
			ReadRecentBuffer(floc, fork, blk, recent))
		{
			buf = recent;
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
map_page_is(const char *page, uint16 type)
{
	const BmapPageOpaqueData *opaque;

	if (PageIsNew(page) ||
		((const PageHeaderData *) page)->pd_special !=
		BLCKSZ - MAXALIGN(sizeof(BmapPageOpaqueData)))
		return false;
	opaque = (const BmapPageOpaqueData *) PageGetSpecialPointer(page);
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
	uint32		path_epoch;		/* the least epoch of the pointers to it */
} TreeLeaf;

/* tries of a lookup that finds a node changed under it, outside recovery */
#define TREE_FIND_TRIES		8

/*
 * In recovery: has reln's map no root page, neither in a buffer nor on disk
 * (one made in this replay has it in a buffer until it is written out)?
 * Then it is a map dropped later in the WAL, whose header an earlier
 * record's image made again (redo makes a missing file to restore an image
 * in), or one that lost its pages.  The root's buffer is asked first, as it
 * is nearly always cached; the file only when it isn't.
 */
static bool
map_root_lost(SMgrRelation reln)
{
	PrefetchBufferResult r = PrefetchSharedBuffer(reln, BMAP_FORKNUM, 1);

	if (BufferIsValid(r.recent_buffer))
		return false;
	return mdexists(reln, BMAP_FORKNUM) &&
		mdnblocks(reln, BMAP_FORKNUM) < BMAP_TREE_MAP_PAGES;
}

/*
 * Look up logical block 'blkno' in reln's map, if it is a tree: its leaf
 * into *leaf, and the leaf's entries from the block's on, at most n (and
 * none past the leaf), into entries[].  See read_entries() for 'for_write'
 * and 'cached_only'.
 *
 * In recovery a node may not be the incarnation the pointer to it names,
 * however often it is read: the pointer is of a version of its page newer than
 * the replay, and the node it names was made after the replay point (and not
 * yet written out before a crash), or made something else after it was let go
 * of.  Either way the subtree that held the block when the record being
 * replayed was made was replaced or let go of since: copied because it was
 * shared, and then its relation's own pages were logged as images before the
 * share, whose replay writes them to their blocks, so its blocks have what the
 * records before then changed; or let go of by a truncation, a drop or a root
 * swap, after which nothing reads its pages.  (A subtree never gets a level
 * above it, which would replace a pointer with one to a node that still needs
 * the old.)  So the block's records from before then are skipped, and its
 * pages go nowhere: TREE_UNAVAILABLE.
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

	/* (a map with no root in recovery: the relation's pages go nowhere) */
	if (RecoveryInProgress() && !cached_only && map_root_lost(reln))
		return TREE_UNAVAILABLE;

	for (;;)
	{
		PGIOAlignedBlock page;
		BmapEntry	ptr;
		PageRead	r;
		int			level;
		uint32		path_epoch = PG_UINT32_MAX;

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
		if (!map_page_is(page.data, BMAP_PAGE_ROOT))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("block 1 of \"%s\" is not the root page of a block map of format version %u, which this server reads only",
							relpathperm(rel, BMAP_FORKNUM).str, BMAP_VERSION),
					 errhint("Recreate the database from a dump made by the server that wrote it.")));
		INJECTION_POINT("bmap-tree-find-root", NULL);

		for (level = path.top; level >= 0; level--)
		{
			BmapEntry	next;

			if (!BlockNumberIsValid(ptr.block))
				return TREE_NO_LEAF;
			path_epoch = Min(path_epoch, ptr.epoch);
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
				leaf->path_epoch = path_epoch;
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
 * As lookup_entry_at(), outside recovery, and into *path_epoch the least
 * epoch of the pointers on the path to the leaf (PG_UINT32_MAX without a
 * leaf).  A path every pointer of which was set in the map's epoch is the
 * relation's own and stamped (tree_path_own()), which this tells without
 * reading the path again.
 */
static bool
lookup_entry_path(SMgrRelation reln, BlockNumber blkno, BmapEntry *entry,
				  MapPage *mp, uint32 *path_epoch)
{
	TreeLeaf	leaf;

	Assert(!RecoveryInProgress());
	*path_epoch = PG_UINT32_MAX;
	if (tree_find(reln, blkno, false, false, 1, entry, &leaf) != TREE_LEAF)
	{
		entry->block = InvalidBlockNumber;
		entry->since = InvalidXLogRecPtr;
		return false;
	}
	mp_leaf(&leaf, mp);
	*path_epoch = leaf.path_epoch;
	return BlockNumberIsValid(entry->block);
}

/*
 * (tree_map_blocks()) the end of the last leaf under 'ptr', of 'level',
 * mapping from 'first', that begins below 'limit'; 0 if it has none.
 */
static uint64
last_leaf_end_under(SMgrRelation reln, const BmapEntry *ptr, uint16 level,
					uint64 first, uint64 limit)
{
	PGIOAlignedBlock page;
	BmapEntry  *entries;
	uint64		end = 0;

	if (first >= limit)
		return 0;
	entries = palloc(BMAP_TREE_FANOUT * sizeof(BmapEntry));
	(void) read_entries(reln, true, ptr->block, false, false, 0,
						BMAP_TREE_FANOUT, entries, &page);
	if (!tree_node_is(page.data, level, (BlockNumber) first, ptr->since))
		end = 0;
	else if (level == 0)
		end = first + BMAP_TREE_FANOUT;
	else
	{
		for (int i = BMAP_TREE_FANOUT - 1; i >= 0 && end == 0; i--)
			if (BlockNumberIsValid(entries[i].block))
				end = last_leaf_end_under(reln, &entries[i], level - 1,
										  first + (uint64) i * tree_span(level - 1),
										  limit);
	}
	pfree(entries);
	return end;
}

/*
 * The logical blocks a tree's leaves map, of those that begin below
 * 'limit': up to the end of the last such leaf (past its relation's length
 * too).
 */
static uint64
tree_map_blocks(SMgrRelation reln, uint64 limit)
{
	PGIOAlignedBlock page;
	BmapEntry	tops[BMAP_TREE_TOPS];
	uint64		base = 0;
	uint64		end = 0;

	(void) read_entries(reln, false, 1, false, false, 0, BMAP_TREE_TOPS, tops,
						&page);
	for (int k = 0; k < BMAP_TREE_TOPS; k++)
	{
		uint64		first = base;
		uint64		e;

		base += tree_span(k);
		if (!BlockNumberIsValid(tops[k].block))
			continue;
		e = last_leaf_end_under(reln, &tops[k], k, first, limit);
		if (e > 0)
			end = e;
	}
	return Min(end, (uint64) MaxBlockNumber + 1);
}

/* the logical blocks the map's leaves have entries for, past its length too */
static uint64
map_blocks(SMgrRelation reln)
{
	return tree_map_blocks(reln, (uint64) MaxBlockNumber + 1);
}

/* the same, of the main fork's blocks alone */
static uint64
map_blocks_below(SMgrRelation reln, uint64 limit)
{
	return tree_map_blocks(reln, limit);
}

/* (tree_next_leaf()) the first leaf under 'ptr' mapping blocks from 'from' on */
static bool
next_leaf_under(SMgrRelation reln, const BmapEntry *ptr, uint16 level,
				uint64 first, uint64 from, uint64 *found)
{
	PGIOAlignedBlock page;
	BmapEntry  *entries;
	uint64		unit;
	uint32		start = 0;
	bool		any = false;

	if (level == 0)
	{
		*found = first;
		return true;
	}
	entries = palloc(BMAP_TREE_FANOUT * sizeof(BmapEntry));
	(void) read_entries(reln, true, ptr->block, false, false, 0,
						BMAP_TREE_FANOUT, entries, &page);
	if (!tree_node_is(page.data, level, (BlockNumber) first, ptr->since))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("block %u of the page store of \"%s\" is not the node of its block map it was",
						ptr->block, relpath(reln->smgr_rlocator, MAIN_FORKNUM).str)));
	unit = tree_span(level - 1);
	if (from > first)
		start = (uint32) ((from - first) / unit);
	for (uint32 i = start; i < BMAP_TREE_FANOUT && !any; i++)
		if (BlockNumberIsValid(entries[i].block))
			any = next_leaf_under(reln, &entries[i], level - 1,
								  first + i * unit, from, found);
	pfree(entries);
	return any;
}

/*
 * The first logical block of the first leaf of reln's tree that maps blocks
 * from 'from' on (the leaf of 'from' itself, if it has one), or past
 * MaxBlockNumber if there is none.  Subtrees without nodes are passed over
 * without reading anything, so the cost is of the nodes the map has, not of
 * the blocks between them: a map with holes may have a leaf near each end
 * of the blocks a relation can have, up to MaxBlockNumber.  The caller holds the
 * header, so the tree changes no other way meanwhile.
 */
static uint64
tree_next_leaf(SMgrRelation reln, uint64 from)
{
	PGIOAlignedBlock page;
	BmapEntry	tops[BMAP_TREE_TOPS];
	uint64		base = 0;

	(void) read_entries(reln, false, 1, false, false, 0, BMAP_TREE_TOPS, tops,
						&page);
	for (int k = 0; k < BMAP_TREE_TOPS; k++)
	{
		uint64		first = base;
		uint64		found;

		base += tree_span(k);
		if (base <= from || !BlockNumberIsValid(tops[k].block))
			continue;
		if (next_leaf_under(reln, &tops[k], k, first, from, &found))
			return found;
	}
	return (uint64) MaxBlockNumber + 1;
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

/*
 * The map's epoch, and into *end the key past the end of the fork of 'key',
 * as above
 */
static uint32
lookup_epoch_end(SMgrRelation reln, uint64 key, uint64 *end)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		buf = map_read(rel, 0);
	uint32		epoch;
	BmapHeader *header;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(buf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	header = PageGetBmapHeader(BufferGetPage(buf));
	epoch = header->epoch;
	*end = key_fork_end(header->nblocks, header->vm_nblocks, key);
	UnlockReleaseBuffer(buf);
	return epoch;
}

/* the reference count of 'phys', read under a share lock */
static uint32
lookup_ref(RelFileLocator rel, BlockNumber phys)
{
	Buffer		buf;
	uint32		count;

	if (!refs_has_page(rel, REF_PAGE(phys)))
		return 0;
	buf = refs_read(rel, REF_PAGE(phys));
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	check_refs_page(BufferGetPage(buf), rel, REF_PAGE(phys));
	count = page_ref(BufferGetPage(buf), phys);
	UnlockReleaseBuffer(buf);
	return count;
}

/*
 * Does a copy made without an image hold 'phys' as it is (page_copy_holds()),
 * read under a share lock?  For hints: the change itself asks again.
 */
static bool
lookup_copy_holds(RelFileLocator rel, BlockNumber phys)
{
	Buffer		buf;
	bool		holds;
	XLogRecPtr	passed = InvalidXLogRecPtr;

	if (!refs_has_page(rel, REF_PAGE(phys)))
		return false;
	buf = refs_read(rel, REF_PAGE(phys));
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	check_refs_page(BufferGetPage(buf), rel, REF_PAGE(phys));
	holds = page_copy_holds(BufferGetPage(buf), phys, copy_horizon(),
							&passed);
	UnlockReleaseBuffer(buf);
	return holds;
}

/* a block whose count to look up, with where the count goes */
typedef struct RefWanted
{
	BlockNumber phys;
	uint32		i;
} RefWanted;

static int
cmp_ref_wanted(const void *a, const void *b)
{
	BlockNumber pa = ((const RefWanted *) a)->phys;
	BlockNumber pb = ((const RefWanted *) b)->phys;

	return pa < pb ? -1 : pa > pb ? 1 : 0;
}

/*
 * lookup_ref() of each of phys[0..n-1], into counts[]: each count page read
 * once, under a share lock held alone, and the file's length asked once.
 * (A walk asks for every entry of a map page, whose blocks are near one
 * another: one or two count pages, where a lookup each would read its page
 * and ask the file's length every time.)
 */
static void
lookup_refs(RelFileLocator rel, const BlockNumber *phys, uint32 n,
			uint32 *counts)
{
	RefWanted  *wanted;
	BlockNumber nrefs;
	BlockNumber held = InvalidBlockNumber;
	Buffer		buf = InvalidBuffer;

	if (n == 0)
		return;
	nrefs = refs_nblocks(rel);
	wanted = palloc(n * sizeof(RefWanted));
	for (uint32 i = 0; i < n; i++)
	{
		wanted[i].phys = phys[i];
		wanted[i].i = i;
	}
	qsort(wanted, n, sizeof(RefWanted), cmp_ref_wanted);
	for (uint32 k = 0; k < n; k++)
	{
		BlockNumber page = REF_PAGE(wanted[k].phys);

		if (page >= nrefs)
		{
			counts[wanted[k].i] = 0;
			continue;
		}
		if (page != held)
		{
			if (BufferIsValid(buf))
				UnlockReleaseBuffer(buf);
			buf = refs_read(rel, page);
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			check_refs_page(BufferGetPage(buf), rel, page);
			held = page;
		}
		counts[wanted[k].i] = page_ref(BufferGetPage(buf), wanted[k].phys);
	}
	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
	pfree(wanted);
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
 * The length of the relation's fork 'forknum' (its main fork or its
 * visibility map) in its map's header, and in *lsn (if given) the header's
 * LSN.  See read_node() for 'for_write'.
 */
static BlockNumber
header_length(SMgrRelation reln, ForkNumber forknum, bool for_write,
			  XLogRecPtr *lsn)
{
	PGIOAlignedBlock copy;

	read_node(reln, 0, for_write, copy.data, BMAP_PAGE_HEADER);
	if (lsn)
		*lsn = PageGetLSN(copy.data);
	if (forknum == VISIBILITYMAP_FORKNUM)
		return PageGetBmapHeader(copy.data)->vm_nblocks;
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
/*
 * As lookup(), for a read: InvalidBlockNumber if logical block 'blkno' is a
 * hole, a block below the relation's length without an entry, which reads
 * as zeroes (as a hole of md.c's file does), with *run how many blocks from
 * it on its leaf are holes too.  A block past the length without an entry
 * fails, as lookup() does.
 */
static BlockNumber
hole_or_lookup(SMgrRelation reln, BlockNumber blkno, BlockNumber maxrun,
			   BlockNumber *run)
{
	BlockNumber phys = lookup_ext(reln, blkno, maxrun, run, true, false);

	if (!BlockNumberIsValid(phys) &&
		key_block(blkno) >= bmapnblocks(reln, key_fork(blkno)))
		(void) lookup(reln, blkno, 1, NULL);
	return phys;
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
 * The blocks a page store grows by at once for copies (prepare_store()):
 * enough that md.c allocates them without writing them (more than eight).
 */
#define BMAP_STORE_AHEAD	64

/*
 * Make the page store hold the blocks below 'new_hw', zeroing the blocks
 * from 'old_hw', and zero the reused blocks.  Done before the record that
 * maps them, and again by its redo unless 'skip_existing': then blocks the
 * page store already has are left as they are.
 *
 * A store that must grow grows to a multiple of 'ahead' blocks (if not 0),
 * the blocks past 'new_hw' zeroes the allocator hasn't given out yet, as a
 * crash can leave them: the caller skips existing blocks, so that the
 * copies taking them next find them there and write nothing, where growing
 * a block at a time would write each block's zeroes first.
 *
 * A reused block may have been a tree's node, whose page may still be in a
 * buffer, dirty: it is dropped first (drop_node_buffer()).
 */
static void
prepare_store(RelFileLocator rel, BlockNumber old_hw, BlockNumber new_hw,
			  const BlockNumber *reused, uint32 nreused, bool skip_existing,
			  BlockNumber ahead)
{
	SMgrRelation ssrel = store_smgr(rel);
	BlockNumber store_cur = mdnblocks(ssrel, MAIN_FORKNUM);
	BlockNumber blk;

	for (uint32 i = 0; i < nreused; i++)
	{
		const void *page = zero_block.data;

		drop_node_buffer(rel, reused[i]);
		if (skip_existing && reused[i] < store_cur)
			continue;
		mdwritev(ssrel, MAIN_FORKNUM, reused[i], &page, 1, false);
		store_written(rel, reused[i], 1);
	}
	for (blk = old_hw; blk < new_hw && blk < store_cur; blk++)
	{
		const void *page = zero_block.data;

		if (skip_existing)
			continue;
		mdwritev(ssrel, MAIN_FORKNUM, blk, &page, 1, false);
		store_written(rel, blk, 1);
	}
	if (blk < new_hw)
	{
		Assert(ahead == 0 || skip_existing);
		if (ahead > 0 && (uint64) new_hw + ahead <= MaxBlockNumber)
			new_hw = (new_hw + ahead - 1) / ahead * ahead;
		mdzeroextend(ssrel, MAIN_FORKNUM, blk, new_hw - blk, false);
		/* (a node's buffer is read with the store's length, in recovery) */
		ssrel->smgr_cached_nblocks[MAIN_FORKNUM] = InvalidBlockNumber;
		store_written(rel, blk, new_hw - blk);
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
 * blocks first, then the page store's high-water mark.  A relation whose
 * data skips WAL (made in this transaction at wal_level minimal) takes
 * freed blocks too: its pages are logged as images at commit, which redo
 * restores after whatever an earlier holder's records put there.  New
 * blocks get their reference-count pages here, before they are mapped.
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

	if (nneed > 0 && meta->nfree > 0 && !sweep_running(rel))
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

static uint32 free_blocks(RelFileLocator rel, BlockNumber *blocks, uint32 n);
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
 * Set the pointer 'ptr' in 'parent' (the relation's own) to the relation's
 * own node, set before the map's epoch, in 'epoch' instead: so every node
 * above a leaf changed in an epoch is pointed to as of that epoch at least,
 * and the pages changed since an epoch are found by walking only the
 * subtrees set since (bmap_changed_pages()).  One record, the parent's
 * delta, made once a node for each epoch: the first change under it.  The
 * header is held exclusively.  Returns the pointer as set.
 */
static BmapEntry
tree_stamp_node(SMgrRelation reln, const TreeParent *parent, uint32 epoch,
				const BmapEntry *ptr)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		parentbuf;
	xl_bmap_tree xlrec;
	struct
	{
		xl_bmap_node_delta d;
		xl_bmap_entry_delta e;
	}			pdelta;
	BmapEntry	stamped = *ptr;
	XLogRecPtr	lsn;

	parentbuf = parent_read(reln, parent);
	map_lock(parentbuf, false);
	parent_check(reln, parent, BufferGetPage(parentbuf));
	if (PageGetBmapEntries(BufferGetPage(parentbuf))[parent->slot].block != ptr->block)
		elog(ERROR, "the pointer to node %u of \"%s\" changed while it was set",
			 ptr->block, relpath(reln->smgr_rlocator, MAIN_FORKNUM).str);

	xlrec.locator = rel;
	xlrec.op = BMAP_TREE_STAMP;
	xlrec.flags = 0;
	xlrec.node = ptr->block;
	xlrec.from = InvalidBlockNumber;
	xlrec.old_hw = xlrec.new_hw = InvalidBlockNumber;
	stamped.epoch = epoch;
	pdelta.d.kind = BMAP_DELTA_NODE;
	pdelta.d.flags = 0;
	pdelta.d.nentries = 1;
	pdelta.d.since = GetXLogInsertRecPtr();
	pdelta.e.slot = parent->slot;
	pdelta.e.block = ptr->block;
	pdelta.e.epoch = epoch;

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
	return stamped;
}

/*
 * Move the relation's own node that 'ptr' in 'parent' points to, of 'level'
 * mapping from 'first', to a new block pointed to in 'epoch', and free its
 * old block.  The node, pointed to from before the map's shared_below, may
 * have been shared with another map that has since copied it, and a lookup
 * of that map (tree_find(), under no lock of this relation's header) may
 * hold a pointer to it still.  Changed in place, the node would be read as
 * the one that map shared; moved, its old block keeps what it had until it
 * is made something else, which the lookup finds is not the node it named,
 * and begins again.  Recovery's rules stay those of a copy: the new node is
 * another incarnation, and the relation's own pages under the old one were
 * logged as images at the share.
 *
 * The children keep their holders, the new node holding them in the old
 * one's place.  One record (MOVE): the new node's allocation and image and
 * the parent's pointer; then the old block freed, in records of its own (a
 * crash between leaves it held by nothing, to a sweep).  The header is held
 * exclusively.  Locks as tree_copy_node()'s.  Returns the new pointer.
 */
static BmapEntry
tree_move_node(SMgrRelation reln, const TreeParent *parent, uint32 epoch,
			   uint16 level, BlockNumber first, const BmapEntry *ptr)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	BmapEntry  *entries = palloc(BMAP_TREE_FANOUT * sizeof(BmapEntry));
	BlockNumber old = ptr->block;
	Buffer		buf;
	Buffer		parentbuf;
	Buffer		nodebuf;
	BlockNumber node;
	Allocation	alloc;
	xl_bmap_tree xlrec;
	struct
	{
		xl_bmap_node_delta d;
		xl_bmap_entry_delta e;
	}			pdelta;
	BmapEntry	moved;
	Page		page;
	XLogRecPtr	lsn;

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

	parentbuf = parent_read(reln, parent);
	alloc_take(rel, 1, &node, &alloc);
	prepare_node_store(rel, node, alloc.old_hw, alloc.new_hw);
	nodebuf = tree_read(rel, node, RBM_ZERO_AND_LOCK);
	map_locked_as_new(nodebuf);
	map_lock(parentbuf, false);
	parent_check(reln, parent, BufferGetPage(parentbuf));
	if (PageGetBmapEntries(BufferGetPage(parentbuf))[parent->slot].block != ptr->block)
		elog(ERROR, "the pointer to node %u of \"%s\" changed while it was moved",
			 ptr->block, relpath(reln->smgr_rlocator, MAIN_FORKNUM).str);

	xlrec.locator = rel;
	xlrec.op = BMAP_TREE_MOVE;
	xlrec.flags = BMAP_TREE_INSTALL;
	xlrec.node = node;
	xlrec.from = old;
	xlrec.old_hw = alloc.old_hw;
	xlrec.new_hw = alloc.new_hw;

	XLogEnsureRecordSpace(2 + BMAP_ALLOC_MAX_PAGES, 3 + BMAP_ALLOC_MAX_PAGES);
	moved.block = node;
	moved.epoch = epoch;
	moved.since = GetXLogInsertRecPtr();
	pdelta.d.kind = BMAP_DELTA_NODE;
	pdelta.d.flags = 0;
	pdelta.d.nentries = 1;
	pdelta.d.since = moved.since;
	pdelta.e.slot = parent->slot;
	pdelta.e.block = node;
	pdelta.e.epoch = epoch;

	START_CRIT_SECTION();
	page = BufferGetPage(nodebuf);
	init_tree_node(page, level, first, moved.since);
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
	(void) alloc_apply_register(&alloc, 2);
	lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_TREE);
	PageSetLSN(page, lsn);
	PageSetLSN(BufferGetPage(parentbuf), lsn);
	alloc_set_lsn(&alloc, lsn);
	END_CRIT_SECTION();

	alloc_release(&alloc);
	map_unlock(parentbuf);
	ReleaseBuffer(parentbuf);
	map_unlock(nodebuf);
	ReleaseBuffer(nodebuf);
	pfree(entries);
	(void) free_blocks(rel, &old, 1);
	return moved;
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
	uint32		shared_below = PageGetBmapHeader(BufferGetPage(headerbuf))->shared_below;
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
			else if (ptr.epoch < epoch && ptr.epoch < shared_below)
				ptr = tree_move_node(reln, &parent, epoch, level,
									 path.first[level], &ptr);
			else if (ptr.epoch < epoch)
				ptr = tree_stamp_node(reln, &parent, epoch, &ptr);
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
 * own, every node on it (see tree_node_own())?  With 'stamped', each
 * pointer on it set in the map's epoch too (tree_leaf_for() stamps one
 * that isn't), as a change to the leaf needs.  Read under share locks; the
 * caller holds the header, at least shared, so no share or copy of the
 * relation's begins meanwhile.
 */
static bool
tree_path_own(SMgrRelation reln, BlockNumber blkno, uint32 epoch, bool stamped)
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
		if (!tree_node_own(rel, &ptr, epoch, true) ||
			(stamped && ptr.epoch < epoch))
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
 * Map the tree's blocks [from, to), all on one leaf, 'leaf', the
 * relation's own (tree_leaf_for()), and make their fork at least as long
 * as 'to' says, with 'content' written to block to - 1 if given.  The
 * header is locked exclusively, and the fork ends at the tree's block
 * 'length': a block below that with an entry keeps it (its page), one past
 * it with an entry (a truncation's, left by a crash) is given the block
 * again, zeroed, and one without is given a block.
 */
static void
extend_piece(SMgrRelation reln, Buffer headerbuf, const MapPage *leaf,
			 BlockNumber from, BlockNumber to, BlockNumber length,
			 const void *content, bool skipFsync)
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

	/*
	 * Which blocks need a physical block.  The map may hold stale ones past
	 * the length, which are zeroed; those below it have pages.
	 */
	for (uint32 i = 0; i < n; i++)
	{
		phys[i] = PageGetBmapEntries(BufferGetPage(mapbuf))[MP_SLOT(mp, from + i)].block;
		need[i] = !BlockNumberIsValid(phys[i]);
		if (need[i])
			nneed++;
		else if (from + i >= length)
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
	prepare_store(rel, old_hw, new_hw, reused, nreused, false, 0);
	if (content)
	{
		const void *page = content;

		mdwritev(store_smgr(rel), MAIN_FORKNUM, phys[n - 1], &page, 1,
				 skipFsync);
		store_written(rel, phys[n - 1], 1);
	}

	/* deltas */
	header_delta_of(BufferGetPage(headerbuf), &hdelta);
	if (key_fork(from) == VISIBILITYMAP_FORKNUM)
		hdelta.vm_nblocks = (BlockNumber) (Max(to, length) - BMAP_VM_BASE);
	else
		hdelta.nblocks = Max(to, length);
	/*
	 * The entries given blocks, and those past the length reused: their
	 * blocks are zeroed, and hold pages made from now on, so they take the
	 * record's 'since' too (BMAP_DELTA_SINCE), as a block got anew does.  A
	 * share tells by it whether a block got its page before the checkpoint
	 * (bmapshare()).
	 */
	ndelta = palloc0(sizeof(xl_bmap_node_delta) + n * sizeof(xl_bmap_entry_delta));
	ndelta->kind = BMAP_DELTA_NODE;
	ndelta->flags = BMAP_DELTA_SINCE;
	for (uint32 i = 0; i < n; i++)
	{
		xl_bmap_entry_delta *ent = (xl_bmap_entry_delta *) (ndelta + 1);

		if (!need[i] && from + i < length)
			continue;
		ent[ndelta->nentries].slot = MP_SLOT(mp, from + i);
		ent[ndelta->nentries].block = phys[i];
		ent[ndelta->nentries].epoch = epoch;
		ndelta->nentries++;
	}
	xlrec.locator = rel;
	xlrec.old_hw = old_hw;
	xlrec.new_hw = new_hw;
	xlrec.flags = 0;
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
typedef struct RecoveryLengthKey
{
	RelFileLocator rel;
	ForkNumber	forknum;		/* its main fork or its visibility map */
} RecoveryLengthKey;

typedef struct RecoveryLength
{
	RecoveryLengthKey key;
	BlockNumber nblocks;		/* the length kept */
	BlockNumber header;			/* the header's length then */
} RecoveryLength;

static HTAB *recovery_lengths = NULL;

static RecoveryLengthKey
recovery_length_key(RelFileLocator rel, ForkNumber forknum)
{
	RecoveryLengthKey key;

	memset(&key, 0, sizeof(key));
	key.rel = rel;
	key.forknum = forknum;
	return key;
}

/* the length recovery keeps for fork 'forknum' of 'rel' past its header's, or 0 */
static BlockNumber
recovery_length(RelFileLocator rel, ForkNumber forknum)
{
	RecoveryLengthKey key = recovery_length_key(rel, forknum);
	RecoveryLength *rl;

	if (recovery_lengths == NULL)
		return 0;
	rl = hash_search(recovery_lengths, &key, HASH_FIND, NULL);
	return rl ? rl->nblocks : 0;
}

/*
 * keep fork 'forknum' of 'rel' at least 'nblocks' long, past its header's
 * length 'header'
 */
static void
keep_recovery_length(RelFileLocator rel, ForkNumber forknum,
					 BlockNumber nblocks, BlockNumber header)
{
	RecoveryLengthKey key = recovery_length_key(rel, forknum);
	RecoveryLength *rl;
	bool		found;

	if (recovery_lengths == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(RecoveryLengthKey);
		ctl.entrysize = sizeof(RecoveryLength);
		recovery_lengths = hash_create("block map lengths in recovery", 16,
									   &ctl, HASH_ELEM | HASH_BLOBS);
	}
	rl = hash_search(recovery_lengths, &key, HASH_ENTER, &found);
	rl->nblocks = Max(found ? rl->nblocks : 0, nblocks);
	rl->header = header;
}

/* a truncation's replay cuts the length kept (in a critical section) */
static void
cut_recovery_length(RelFileLocator rel, ForkNumber forknum,
					BlockNumber nblocks)
{
	RecoveryLengthKey key = recovery_length_key(rel, forknum);
	RecoveryLength *rl;

	if (recovery_lengths == NULL)
		return;
	rl = hash_search(recovery_lengths, &key, HASH_FIND, NULL);
	if (rl == NULL)
		return;
	if (nblocks <= rl->header)
		(void) hash_search(recovery_lengths, &key, HASH_REMOVE, NULL);
	else if (nblocks < rl->nblocks)
		rl->nblocks = nblocks;
}

static void
forget_recovery_length(RelFileLocator rel)
{
	RecoveryLengthKey key;

	if (recovery_lengths == NULL)
		return;
	key = recovery_length_key(rel, MAIN_FORKNUM);
	(void) hash_search(recovery_lengths, &key, HASH_REMOVE, NULL);
	key = recovery_length_key(rel, VISIBILITYMAP_FORKNUM);
	(void) hash_search(recovery_lengths, &key, HASH_REMOVE, NULL);
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
		if (rl->key.rel.dbOid == dbid && rl->key.rel.spcOid == spcid)
			(void) hash_search(recovery_lengths, &rl->key, HASH_REMOVE, NULL);
	}
}

/*
 * In recovery, extend 'reln''s fork of key 'to' - 1 to that key past its
 * header's length (see above), writing 'content' to that block if given.
 * A header no newer than the record being replayed has every block of the
 * main fork the record can have.  The visibility map is extended in redo
 * without a record of the primary's (a cleared bit's page pinned by
 * visibilitymap_pin()), and its blocks past the header's length are
 * holes, which redo never gives a block: a page written to one goes
 * nowhere (bmapwritev()), losing at most bits set, which says less of the
 * heap's pages than is true, never more.
 */
static void
extend_in_recovery(SMgrRelation reln, uint64 to, const void *content,
				   bool skipFsync)
{
	ForkNumber	forknum = key_fork(to - 1);
	XLogRecPtr	lsn;
	BlockNumber nblocks = header_length(reln, forknum, false, &lsn);
	BlockNumber want = key_block(to - 1) + 1;

	if (nblocks < want)
	{
		if (forknum == MAIN_FORKNUM && lsn <= GetCurrentReplayRecPtr(NULL))
			elog(ERROR, "block-mapped relation %s extended during recovery without its allocation",
				 relpath(reln->smgr_rlocator, MAIN_FORKNUM).str);
		keep_recovery_length(reln->smgr_rlocator.locator, forknum, want,
							 nblocks);
	}
	if (content)
		bmapwritev(reln, forknum, want - 1, &content, 1, skipFsync);
}

/*
 * Map the tree's blocks [from, to) of 'reln', all of one fork, making the
 * fork at least as long as 'to' says, and write 'content' to block to - 1
 * if given.  Blocks between the fork's end and 'from' get no entry: they
 * are holes, which read as zeroes and are given a block when they first
 * change (bmapprepareforchange()), as md.c's file has a hole where a write
 * past its end leaves one.  Entries a truncation left there (a crash
 * before it let go of them) are let go of first, or the hole would read
 * their stale pages.  Blocks below the length that have a block keep it.
 */
static void
extend_to(SMgrRelation reln, uint64 from, uint64 to,
		  const void *content, bool skipFsync)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		headerbuf;
	BmapHeader *header;
	uint64		blk = from;
	uint64		fend;

	Assert(from < to);
	Assert(key_fork(from) == key_fork(to - 1));
	if (InRecovery)
	{
		extend_in_recovery(reln, to, content, skipFsync);
		return;
	}

	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	header = PageGetBmapHeader(BufferGetPage(headerbuf));

	fend = key_fork_end(header->nblocks, header->vm_nblocks, from);
	if (from > fend)
		let_go_range(reln, headerbuf, fend, from);

	while (blk < to)
	{
		uint64		nblocks = key_fork_end(header->nblocks,
										   header->vm_nblocks, from);
		uint64		end;
		MapPage		leaf;

		/* blocks below the length that have a block: just the content */
		if (blk < nblocks)
		{
			BlockNumber run;
			BlockNumber phys = lookup_ext(reln, (BlockNumber) blk,
										  (BlockNumber) (Min(to, nblocks) - blk),
										  &run, true, false);

			if (BlockNumberIsValid(phys))
			{
				if (content && blk + run == to)
				{
					const void *page = content;

					mdwritev(store_smgr(rel), MAIN_FORKNUM, phys + run - 1,
							 &page, 1, skipFsync);
					store_written(rel, phys + run - 1, 1);
				}
				blk += run;
				continue;
			}
		}

		/* the leaf of the blocks, the relation's own */
		(void) tree_leaf_for(reln, headerbuf, (BlockNumber) blk, true, true,
							 &leaf);
		end = Min(to, blk + BMAP_PIECE);
		end = Min(end, (uint64) leaf.first + leaf.nslots);
		/* a map never shared has no entry born before its epoch */
		if (header->epoch > 0 && end > nblocks)
		{
			unshare_stale(reln, &leaf, header->epoch,
						  (BlockNumber) Max(blk, nblocks),
						  (BlockNumber) end, false);
			INJECTION_POINT("bmap-unshared", NULL);
		}
		extend_piece(reln, headerbuf, &leaf, (BlockNumber) blk,
					 (BlockNumber) end, (BlockNumber) nblocks,
					 end == to ? content : NULL, skipFsync);
		blk = end;
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
 *
 * A block a copy made without an image may still read (see "Copies without
 * an image") isn't freed yet: it is pushed onto the release queue instead,
 * whose entry holds it until a drain finds the copy past the restart point.
 * Returns how many were (blocks[] is left reordered); a crash before the
 * push leaves them to a sweep too.
 */
static uint32
free_blocks(RelFileLocator rel, BlockNumber *blocks, uint32 n)
{
	uint32		i = 0;
	uint32		nheld = 0;
	BmapQueued *held = NULL;

	/* those a copy holds, first (on pages of counts read one at a time) */
	{
		XLogRecPtr	horizon = copy_horizon();
		XLogRecPtr	passed = InvalidXLogRecPtr;
		BlockNumber nrefpages = refs_nblocks(rel);
		Buffer		buf = InvalidBuffer;
		uint32		nfree = 0;

		for (uint32 k = 0; k < n; k++)
		{
			BlockNumber b = blocks[k];

			if (REF_PAGE(b) >= nrefpages)
			{
				blocks[nfree++] = b;
				continue;
			}
			if (!BufferIsValid(buf) || BufferGetBlockNumber(buf) != REF_PAGE(b))
			{
				if (BufferIsValid(buf))
					UnlockReleaseBuffer(buf);
				buf = refs_read(rel, REF_PAGE(b));
				LockBuffer(buf, BUFFER_LOCK_SHARE);
				check_refs_page(BufferGetPage(buf), rel, REF_PAGE(b));
			}
			if (page_copy_holds(BufferGetPage(buf), b, horizon, &passed))
			{
				if (held == NULL)
					held = palloc0_array(BmapQueued, n);
				held[nheld].block = b;
				held[nheld].level = BMAP_QUEUED_BLOCK;
				nheld++;
			}
			else
				blocks[nfree++] = b;
		}
		if (BufferIsValid(buf))
			UnlockReleaseBuffer(buf);
		n = nfree;
		copies_passed(passed);
	}

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

	/* (what a scan found in them no longer holds) */
	for (i = 0; i < n; i++)
		store_written(rel, blocks[i], 1);

	if (nheld > 0)
	{
		(void) queue_push(rel, held, nheld);
		elog(DEBUG1, "%u blocks of the page store of database %u, copied from without an image since the restart point, are queued, not freed",
			 nheld, rel.dbOid);
		pfree(held);
	}
	return nheld;
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
	/* the roots' counts set, each page's, joined to its delta as it's made */
	xl_bmap_ref_delta *roots[BMAP_REFS_PER_RECORD];
	uint32		nroots[BMAP_REFS_PER_RECORD];
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
		delta = palloc0(sizeof(xl_bmap_refs_delta) + 2 * n * sizeof(xl_bmap_ref_delta));
		delta->kind = BMAP_DELTA_REFS;
		if (PageIsNew(BufferGetPage(rp->buf[rp->n])))
			delta->flags |= BMAP_DELTA_INIT;
		rp->delta[rp->n] = delta;
		rp->roots[rp->n] = palloc(n * sizeof(xl_bmap_ref_delta));
		rp->nroots[rp->n] = 0;
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
static uint32
refs_get(RefsPages *rp, BlockNumber phys)
{
	return page_ref(BufferGetPage(rp->buf[refs_find(rp, phys)]), phys);
}

/* set a block's count when the record is made */
static void
refs_set(RefsPages *rp, BlockNumber phys, uint32 count)
{
	xl_bmap_refs_delta *delta = rp->delta[refs_find(rp, phys)];
	xl_bmap_ref_delta *ref = (xl_bmap_ref_delta *) (delta + 1);

	ref[delta->nrefs].slot = REF_SLOT(phys);
	ref[delta->nrefs].count = count;
	delta->nrefs++;
}

/* a block's roots' count on the locked pages */
static uint32
refs_get_root(RefsPages *rp, BlockNumber phys)
{
	Page		page = BufferGetPage(rp->buf[refs_find(rp, phys)]);

	return PageIsNew(page) ? 0 : PageGetBmapRootRefs(page)[REF_SLOT(phys)];
}

/* set a block's roots' count when the record is made (once a record) */
static void
refs_set_root(RefsPages *rp, BlockNumber phys, uint32 value)
{
	int			k = refs_find(rp, phys);

	rp->roots[k][rp->nroots[k]].slot = REF_SLOT(phys);
	rp->roots[k][rp->nroots[k]].count = value;
	rp->nroots[k]++;
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
		/* (the roots' counts after the counts) */
		if (rp->nroots[k] > 0 && rp->delta[k]->nroots == 0)
		{
			memcpy((xl_bmap_ref_delta *) (rp->delta[k] + 1) + rp->delta[k]->nrefs,
				   rp->roots[k], rp->nroots[k] * sizeof(xl_bmap_ref_delta));
			rp->delta[k]->nroots = rp->nroots[k];
		}
		if (rp->delta[k]->nrefs + rp->delta[k]->nroots == 0)
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
		if (rp->delta[k]->nrefs + rp->delta[k]->nroots > 0)
			PageSetLSN(BufferGetPage(rp->buf[k]), lsn);
}

static void
refs_release(RefsPages *rp)
{
	for (int k = 0; k < rp->n; k++)
	{
		UnlockReleaseBuffer(rp->buf[k]);
		pfree(rp->delta[k]);
		pfree(rp->roots[k]);
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
 * One holder more of each of the blocks phys[0 .. n), on the pages rp
 * locked, before the record's critical section.  A count at the most it
 * says fails the change here, with nothing of this record changed: a count
 * that wrapped would free a block maps still hold, and one that stuck would
 * never reach zero as its holders went.  (A path copy's earlier records,
 * if any, gave children holders already: freed a sweep later, as after a
 * crash between them.)
 */
static void
refs_add_holders(RefsPages *rp, RelFileLocator rel, const BlockNumber *phys,
				 uint32 n)
{
	for (uint32 k = 0; k < n; k++)
	{
		uint32		c = refs_get(rp, phys[k]);

		if (c >= BMAP_REFS_MAX ||
			(c >= BMAP_REFS_TEST_MAX &&
			 IS_INJECTION_POINT_ATTACHED("bmap-refs-test-max")))
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("block %u of the page store of database %u in tablespace %u is shared by too many maps",
							phys[k], rel.dbOid, rel.spcOid),
					 errdetail("A block can be held by at most %u maps.",
							   (c >= BMAP_REFS_MAX ? BMAP_REFS_MAX :
								BMAP_REFS_TEST_MAX) + 1)));
		refs_set(rp, phys[k], c + 1);
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
	uint32		c;
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

		refs_add_holders(&rp, rel, children + i, covered);
		xlrec.op = BMAP_TREE_HOLD;
		log_refs_record(XLOG_BMAP_TREE, &xlrec, sizeof(xlrec), InvalidBuffer,
						NULL, 0, &rp);
		refs_release(&rp);
		i += covered;
		INJECTION_POINT("bmap-tree-held", NULL);
	}

	/* the last of them, and the node's own, with the copy */
	INJECTION_POINT("bmap-tree-copy-counts", NULL);
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
	c = refs_get(&rp, ptr->block);
	if (c == 0)
	{
		/*
		 * The node's other holder copied it, or let it go, since it was
		 * found shared: tree_node_own() looks before the counts are locked,
		 * and another relation sharing it changes its own path under its
		 * own header only.  The node is this relation's alone now, so it is
		 * moved rather than copied (tree_move_node()): a lookup of the
		 * other map may hold a pointer to it still.  Its children gain no
		 * holder from this record, and those the earlier HOLD records gave
		 * them are taken back (UNHOLD): else each would stay counted once
		 * too often, copied rather than claimed at its next change, its
		 * block never freed.
		 */
		refs_release(&rp);
		ReleaseBuffer(parentbuf);
		for (uint32 k = 0; k < i;)
		{
			uint32		covered = refs_lock(rel, children + k, i - k, &rp);

			for (uint32 m = 0; m < covered; m++)
			{
				uint32		held = refs_get(&rp, children[k + m]);

				Assert(held > 0);
				refs_set(&rp, children[k + m], held - 1);
			}
			xlrec.op = BMAP_TREE_UNHOLD;
			log_refs_record(XLOG_BMAP_TREE, &xlrec, sizeof(xlrec), InvalidBuffer,
							NULL, 0, &rp);
			refs_release(&rp);
			k += covered;
		}
		pfree(entries);
		pfree(children);
		return tree_move_node(reln, parent, epoch, level, first, ptr);
	}
	refs_add_holders(&rp, rel, children + i, n - i);
	refs_set(&rp, ptr->block, c - 1);

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
	pfree(entries);
	pfree(children);
	pgstat_count_block_map(rel.spcOid, rel.dbOid, rel.relNumber,
						   PGSTAT_BLOCK_MAP_NODE_COPIED);
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
			  BlockNumber from, BlockNumber to, bool release)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	const MapPage *mp = leaf;
	Buffer		mapbuf;
	PGIOAlignedBlock copy;
	BmapEntry	stale[BMAP_PIECE];
	SlotEntry	items[BMAP_PIECE];
	BlockNumber phys[BMAP_PIECE];
	BlockNumber tofree[BMAP_PIECE];
	bool		holds[BMAP_PIECE] = {0};
	uint32		nitems = 0;
	uint32		nfree = 0;
	BlockNumber nrefpages;
	uint32		i = 0;
	XLogRecPtr	horizon = copy_horizon();
	XLogRecPtr	passed = InvalidXLogRecPtr;

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
		if (e->epoch < epoch || release)
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
		/*
		 * A block a copy made without an image may still read can't be
		 * claimed, to be written over in place: it is let go of instead
		 * (freed, which queues it until the copy is past the restart
		 * point).
		 */
		for (uint32 k = i; k < i + covered && end > i && !release; k++)
		{
			Page		page = BufferGetPage(rp.buf[refs_find(&rp, phys[k])]);

			holds[k] = page_copy_holds(page, phys[k], horizon, &passed);
		}
		copies_passed(passed);
		map_lock(mapbuf, false);

		for (uint32 k = i; k < i + covered; k++)
		{
			uint32		c = end > i ? refs_get(&rp, phys[k]) : 0;

			ent[ndelta->nentries].slot = items[k].slot;
			if (c == 0 && !release && !holds[k])
			{
				/* no other map holds it: it is ours */
				ent[ndelta->nentries].block = items[k].block;
				ent[ndelta->nentries].epoch = epoch;
			}
			else
			{
				if (c == 0)
					tofree[nfree++] = phys[k];
				else
					refs_set(&rp, phys[k], c - 1);
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
	return fork_mapped(forknum);
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
	BlockNumber key = fork_key(forknum, blocknum);
	BmapEntry	entry;
	MapPage		mp;
	bool		unavailable;

	/*
	 * A map with no root page (map_root_lost()): the record is skipped, and
	 * its page noted invalid as md.c notes a page of a missing file.  The
	 * drop's replay forgets it; an archive recovery that reaches
	 * consistency without one fails.  (An image of the page is restored to
	 * its buffer, which goes nowhere when written out: tree_find().)
	 */
	if (map_root_lost(reln))
	{
		XLogNoteInvalidPage(rel, forknum, blocknum, false);
		DropBufferBlock(rel, forknum, blocknum);
		return true;
	}

	if (lookup_entry_at(reln, key, &entry, &mp, &unavailable))
	{
		if (remap_ahead(with_relnumber(rel, BMAP_STORE_RELNUMBER), mp.blk,
						MP_SLOT(&mp, key)) > lsn)
			return true;
		return entry.since > lsn;
	}

	/*
	 * Its subtree replaced or let go of since (see tree_find()): the block
	 * gets every change the records before then made from the image logged
	 * before its subtree came to be shared.  Its buffer, though, may
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
	if (InRecovery && let_go_after(reln, key, false, lsn))
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
		blocknum < recovery_length(reln->smgr_rlocator.locator, forknum) &&
		blocknum >= header_length(reln, forknum, false, NULL);
}

/* ----------------------------------------------------------------
 * Sharing
 * ----------------------------------------------------------------
 */

/* blocks found, as many as there are (a sweep's, a capture's own) */
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

/*
 * Write out the pages of 'own', keys of reln's tree, through their forks'
 * buffers.
 */
static void
flush_own_blocks(SMgrRelation reln, const BlockList *own)
{
	BlockNumber *blocks[2];
	uint32		n[2] = {0, 0};

	if (own->n == 0)
		return;
	blocks[0] = palloc(own->n * sizeof(BlockNumber));
	blocks[1] = palloc(own->n * sizeof(BlockNumber));
	for (uint32 i = 0; i < own->n; i++)
	{
		int			f = key_fork(own->blocks[i]) == VISIBILITYMAP_FORKNUM;

		blocks[f][n[f]++] = key_block(own->blocks[i]);
	}
	FlushRelationBlocks(reln, MAIN_FORKNUM, blocks[0], n[0]);
	FlushRelationBlocks(reln, VISIBILITYMAP_FORKNUM, blocks[1], n[1]);
	pfree(blocks[0]);
	pfree(blocks[1]);
}

/*
 * A walk preserving a relation's own blocks (tree_preserve()).  It holds
 * the relation's header exclusively, so it never waits for a page's content
 * lock: a backend holding a page exclusively may be preparing it for a
 * change, which reads the header (bmapprepareforchange()).  A page the walk
 * can't lock at once is left 'busy', and the walk given up; its caller lets
 * go of the header, waits for the page holding no map page, and walks again
 * with the page locked (shared) in 'held' (preserve_header_lock()), let go
 * of once the walk logs it.  'logged' has the physical blocks the walks
 * logged, each with its logical block, which a walk again doesn't log
 * again while the relation's map still names them so.  It is made only
 * once a walk is given up, so a walk that isn't costs no memory for it:
 * the first walk's are the relation's own blocks below the one it gave up
 * on, as the map names them then, which a pass recording them
 * ('record_below') finds before the header is let go.
 */
typedef struct PreserveWalk
{
	BlockList  *own;			/* the relation's own blocks, if collected */
	BlockNumber busy;			/* the page the walk gave up on */
	BlockNumber held_block;		/* the page locked before the walk, if any */
	Buffer		held;
	HTAB	   *logged;			/* PreserveLogged, by physical block */
	BlockNumber record_below;	/* recording 'logged' up to it, if valid */
	XLogRecPtr	began;			/* the WAL's insert position as this walk began */
	bool		concurrent;		/* writers at work: a page logged checked too */
	struct SyncedBlocks *synced;	/* own blocks synced, not logged, if given */
} PreserveWalk;

/* a leaf holding blocks a share synced (SyncedBlocks) */
typedef struct SyncedLeaf
{
	BlockNumber node;			/* its block of the page store */
	BlockNumber first;			/* the first logical block it maps */
	XLogRecPtr	born;			/* its incarnation (tree_node_is()) */
} SyncedLeaf;

/*
 * The own blocks a share writes out and syncs instead of logging their
 * images (preserve_shared_page()): those that got their blocks before
 * 'image_from'.  Counted, not listed, with the newest such position (or a
 * position past it: a leaf's LSN, for a leaf all of whose blocks are
 * synced, tree_preserve_node()); 'own' counts the blocks found the
 * relation's own, logged or synced, or that may be.
 */
typedef struct SyncedBlocks
{
	XLogRecPtr	image_from;
	uint64		n;
	uint64		own;
	XLogRecPtr	newest;
	/*
	 * The leaves that hold them, as the walk found them: shared by the
	 * share, so unchanged since (a change copies a shared leaf first), and
	 * the source's length then.  A backup begun during the share reads its
	 * blocks' images from them (preserve_synced()).
	 */
	SyncedLeaf *leaves;
	uint32		nleaves;
	uint32		leafcap;
	BlockNumber nblocks;
	BlockNumber vm_nblocks;
	/* what the walk did, for the share's DEBUG1 summary */
	uint64		leaves_whole;	/* leaves counted whole, not looked into */
	uint64		entries;		/* own entries looked at */
	uint64		asked;			/* counts looked up */
} SyncedBlocks;

typedef struct PreserveLogged
{
	BlockNumber phys;			/* hash key */
	BlockNumber blkno;
	XLogRecPtr	since;			/* logged by a walk begun here */
} PreserveLogged;

/*
 * The LSN of page 'blkno' of 'rel', without its content lock (the walk's
 * held one if it holds it): a pin, and the buffer header's.
 */
static XLogRecPtr
preserve_page_lsn(RelFileLocator rel, BlockNumber blkno, PreserveWalk *walk)
{
	Buffer		buf;
	XLogRecPtr	lsn;

	if (walk->held_block == blkno)
		return PageGetLSN(BufferGetPage(walk->held));
	buf = ReadBufferWithoutRelcache(rel, key_fork(blkno), key_block(blkno),
									RBM_NORMAL,
									NULL, true);
	lsn = BufferGetLSNAtomic(buf);
	ReleaseBuffer(buf);
	return lsn;
}

/* let go of the page the walk holds, if it does */
static void
preserve_release_held(PreserveWalk *walk)
{
	if (!BlockNumberIsValid(walk->held_block))
		return;
	UnlockReleaseBuffer(walk->held);
	walk->held_block = InvalidBlockNumber;
}

/*
 * Copy page 'blkno' of 'rel' into 'dst', under its content lock: the one
 * the walk holds (let go of then), or one taken without waiting.  False, the
 * walk's 'busy' set, if it would have to wait.
 */
static bool
preserve_read_page(RelFileLocator rel, BlockNumber blkno, PreserveWalk *walk,
				   char *dst)
{
	Buffer		buf;

	if (walk->held_block == blkno)
	{
		memcpy(dst, BufferGetPage(walk->held), BLCKSZ);
		preserve_release_held(walk);
		return true;
	}
	buf = ReadBufferWithoutRelcache(rel, key_fork(blkno), key_block(blkno),
									RBM_NORMAL,
									NULL, true);
	if (!ConditionalLockBufferShared(buf))
	{
		ReleaseBuffer(buf);
		walk->busy = blkno;
		return false;
	}
	memcpy(dst, BufferGetPage(buf), BLCKSZ);
	UnlockReleaseBuffer(buf);
	return true;
}

/*
 * Is page 'blkno' of 'rel' changed by no one now: its share lock had
 * without waiting (or held by the walk)?  A walk among writers asks it of
 * a page it logged before, as of one it logs (preserve_read_page()): a
 * writer holding the page may change it in place yet.  False, the walk's
 * 'busy' set, if not.
 */
static bool
preserve_page_idle(RelFileLocator rel, BlockNumber blkno, PreserveWalk *walk)
{
	Buffer		buf;

	if (walk->held_block == blkno)
		return true;
	buf = ReadBufferWithoutRelcache(rel, key_fork(blkno), key_block(blkno),
									RBM_NORMAL,
									NULL, true);
	if (!ConditionalLockBufferShared(buf))
	{
		ReleaseBuffer(buf);
		walk->busy = blkno;
		return false;
	}
	UnlockReleaseBuffer(buf);
	return true;
}

/* note the leaf the walk is in as holding synced blocks, once */
static void
synced_note_leaf(SyncedBlocks *synced, BlockNumber node, BlockNumber first,
				 XLogRecPtr born)
{
	if (synced->nleaves > 0 && synced->leaves[synced->nleaves - 1].node == node)
		return;
	if (synced->nleaves == synced->leafcap)
	{
		synced->leafcap = Max(16, synced->leafcap * 2);
		synced->leaves = synced->leaves ?
			repalloc_array(synced->leaves, SyncedLeaf, synced->leafcap) :
			palloc_array(SyncedLeaf, synced->leafcap);
	}
	synced->leaves[synced->nleaves].node = node;
	synced->leaves[synced->nleaves].first = first;
	synced->leaves[synced->nleaves].born = born;
	synced->nleaves++;
}

/*
 * Before a private block is shared, log its contents with its physical
 * address, unless the walk's 'synced' says the share syncs it instead (it
 * got the block before 'image_from': see bmapshare()).  A base backup may
 * have read the page store before our flush, and the source's map after a
 * later remap.  Redo then skips the source's older changes, so flushing
 * alone cannot preserve the copy's rows.
 *
 * At every WAL level, wal_level minimal too: crash recovery may skip the
 * source's changes as well (its map dropped, or the block moved off), and
 * may first zero the block again, replaying the record that gave it to the
 * source, or overwrite it with an earlier holder's image.  Only an image
 * logged after them all restores it.  (At replica and logical, a backup
 * could start between a check for one and the share's flush, too.)
 *
 * Blocks already held by another map have been preserved
 * by their first share or REMAP, and cannot change in place: the caller
 * passes only those whose count is zero.  The caller holds the source's
 * header exclusively and keeps its writers out, but has not locked the
 * destination map page.
 *
 * With the walk's 'own', the block is added to it whether logged or not: a
 * page that may be only in its buffer, to be written out (see
 * bmap_root_capture()).
 *
 * The page is read under its content lock, the walk's held one or one
 * taken only if that needn't wait (PreserveWalk): false, with the walk's
 * 'busy' set to the page, if it would have to.  A block an earlier walk of
 * the same share logged is not logged again while the same entry names it.
 */
static bool
preserve_shared_page(SMgrRelation src, BlockNumber blkno, BlockNumber phys,
					 XLogRecPtr since, PreserveWalk *walk)
{
	RelFileLocator rel = src->smgr_rlocator.locator;
	PGIOAlignedBlock copy;
	PreserveLogged *logged;
	bool		found;
	bool		synced = walk->synced && since < walk->synced->image_from;

	/* (the walks' logical order: below the busy page, every block logged) */
	if (BlockNumberIsValid(walk->record_below))
	{
		if (blkno >= walk->record_below)
			return false;
		if (synced)
			return true;
		logged = hash_search(walk->logged, &phys, HASH_ENTER, &found);
		logged->blkno = blkno;
		logged->since = walk->began;
		return true;
	}

	if (walk->own)
		block_list_add(walk->own, blkno);
	if (walk->synced)
	{
		walk->synced->own++;
		walk->synced->entries++;
	}

	/*
	 * A block the relation got before the checkpoint last completed (as
	 * the share began) isn't logged: the share writes it out and syncs it
	 * (bmapshare()), and no recovery begins before the checkpoint.  The
	 * page isn't read, so it needs no lock.
	 */
	if (synced)
	{
		walk->synced->n++;
		walk->synced->newest = Max(walk->synced->newest, since);
		return true;
	}

	/*
	 * Logged by an earlier walk, still named by the same entry, and not
	 * changed since: not logged again.  Its image is in the WAL before the
	 * share's record, and is the page as it is: the relation's writers are
	 * kept out, and a change that reached the page in place since (a prune,
	 * a SELECT FOR UPDATE's lock, a hint logged) set its LSN past the
	 * walk's beginning, so the page is logged again, as redo of the share
	 * takes the images for the pages as of the share and writes nothing
	 * else to the blocks.  A change that moves the page (a copy, a
	 * compaction's move) gives it another block, which is logged.  The
	 * block can't have held another page meanwhile: the map naming it held
	 * it all along.  So recovery, a standby or a base
	 * backup's redo gets the block the copy shares from the earlier image
	 * as from a new one.
	 */
	if (walk->logged)
	{
		logged = hash_search(walk->logged, &phys, HASH_FIND, NULL);
		if (logged && logged->blkno == blkno &&
			preserve_page_lsn(rel, blkno, walk) < logged->since &&
			(!walk->concurrent || preserve_page_idle(rel, blkno, walk)))
		{
			if (walk->held_block == blkno)
				preserve_release_held(walk);
			return true;
		}
		if (walk->concurrent && BlockNumberIsValid(walk->busy))
			return false;
	}

	INJECTION_POINT("bmap-preserve-page", NULL);
	if (!preserve_read_page(rel, blkno, walk, copy.data))
		return false;

	XLogBeginInsert();
	XLogRegisterData(&phys, sizeof(phys));
	XLogRegisterBlock(0, &rel, key_fork(blkno), key_block(blkno), copy.data,
					 REGBUF_FORCE_IMAGE | key_image_flags(blkno));
	XLogInsert(RM_BMAP_ID, XLOG_BMAP_PRESERVE);

	if (walk->logged)
	{
		logged = hash_search(walk->logged, &phys, HASH_ENTER, &found);
		logged->blkno = blkno;
		logged->since = walk->began;
	}
	return true;
}

/*
 * Before a tree is shared, preserve (preserve_shared_page()) the blocks of
 * its own leaves, under its length: those of shared ones were preserved when
 * they came to be shared, and can't change in place since.  So a share that
 * follows another walks only what changed in between.  With the walk's
 * 'own', the blocks are collected into it too.  The header is held
 * exclusively.  False if the walk was given up on a busy page
 * (PreserveWalk).
 *
 * 'ptr' is the relation's own node (tree_node_own()).  Its children's
 * counts are looked up together (lookup_refs()), but for those set in the
 * map's epoch, which are its own: a node copied for one change has a child
 * for each leaf under it, up to a node's fanout, and a leaf an entry for
 * each block.
 */
static bool
tree_preserve_node(SMgrRelation src, const BmapEntry *ptr, uint16 level,
				   BlockNumber first, BlockNumber nblocks,
				   BlockNumber vm_nblocks, uint32 epoch, PreserveWalk *walk)
{
	RelFileLocator rel = src->smgr_rlocator.locator;
	BmapEntry  *entries;
	BlockNumber *ask;
	uint32	   *slot;
	uint32	   *counts;
	uint32		nask = 0;
	Buffer		buf;
	XLogRecPtr	lsn;
	bool		done = true;

	if (!range_in_length(nblocks, vm_nblocks, first,
						 (uint64) first + tree_span(level)))
		return true;
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
	lsn = PageGetLSN(BufferGetPage(buf));
	UnlockReleaseBuffer(buf);

	/*
	 * A leaf last changed before 'image_from' got every block it names
	 * before it too, a block's entry changing only by a record on its leaf:
	 * its own blocks are all synced, none logged nor read
	 * (preserve_shared_page()), so they are counted without asking which
	 * are its own.  Its LSN stands for their newest position.  (Not while a
	 * walk records what it logged below a busy page: that walk stops at
	 * the page's block.)
	 */
	if (level == 0 && walk->synced && !walk->own &&
		!BlockNumberIsValid(walk->record_below) &&
		!XLogRecPtrIsInvalid(lsn) && lsn < walk->synced->image_from)
	{
		for (uint32 i = 0; i < BMAP_TREE_FANOUT; i++)
		{
			uint64		child = (uint64) first + i;

			if (BlockNumberIsValid(entries[i].block) &&
				range_in_length(nblocks, vm_nblocks, child, child + 1))
			{
				walk->synced->n++;
				walk->synced->own++;
			}
		}
		walk->synced->newest = Max(walk->synced->newest, lsn);
		walk->synced->leaves_whole++;
		synced_note_leaf(walk->synced, ptr->block, first, ptr->since);
		pfree(entries);
		return true;
	}

	/*
	 * The children whose counts say whether they are the relation's own:
	 * those set before the map's epoch.  One set in it, a node or a leaf's
	 * block, is its own whatever its count: nothing has shared or captured
	 * the map since (bmappageisshared()).  So a relation never shared asks
	 * for no count at all.
	 */
	ask = palloc(BMAP_TREE_FANOUT * sizeof(BlockNumber));
	slot = palloc(BMAP_TREE_FANOUT * sizeof(uint32));
	counts = palloc(BMAP_TREE_FANOUT * sizeof(uint32));
	for (uint32 i = 0; i < BMAP_TREE_FANOUT; i++)
	{
		uint64		child = (uint64) first + (level == 0 ? i :
											   i * tree_span(level - 1));

		if (!BlockNumberIsValid(entries[i].block) ||
			!range_in_length(nblocks, vm_nblocks, child,
							 child + (level == 0 ? 1 : tree_span(level - 1))))
			continue;
		if (entries[i].epoch >= epoch)
			continue;
		ask[nask] = entries[i].block;
		slot[nask++] = i;
	}
	lookup_refs(rel, ask, nask, counts);
	if (walk->synced)
		walk->synced->asked += nask;
	for (uint32 k = 0, i = 0; i < BMAP_TREE_FANOUT && done; i++)
	{
		uint64		child = (uint64) first + (level == 0 ? i :
											   i * tree_span(level - 1));
		bool		held_elsewhere = false;

		if (!BlockNumberIsValid(entries[i].block) ||
			!range_in_length(nblocks, vm_nblocks, child,
							 child + (level == 0 ? 1 : tree_span(level - 1))))
			continue;
		if (k < nask && slot[k] == i)
			held_elsewhere = counts[k++] > 0;
		if (held_elsewhere)
			continue;
		if (level == 0)
		{
			uint64		before = walk->synced ? walk->synced->n : 0;

			done = preserve_shared_page(src, (BlockNumber) child,
										entries[i].block, entries[i].since,
										walk);
			if (walk->synced && walk->synced->n > before)
				synced_note_leaf(walk->synced, ptr->block, first, ptr->since);
		}
		else
			done = tree_preserve_node(src, &entries[i], level - 1,
									  (BlockNumber) child, nblocks,
									  vm_nblocks, epoch, walk);
	}
	pfree(counts);
	pfree(slot);
	pfree(ask);
	pfree(entries);
	return done;
}

static bool
tree_preserve(SMgrRelation src, BlockNumber nblocks, BlockNumber vm_nblocks,
			  uint32 epoch, PreserveWalk *walk)
{
	RelFileLocator rel = src->smgr_rlocator.locator;
	BmapRoot	root;
	Buffer		buf;
	uint64		first = 0;

	buf = map_read(rel, 1);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(buf), BMAP_PAGE_ROOT, rel, BMAP_FORKNUM, 1);
	root = *PageGetBmapRoot(BufferGetPage(buf));
	UnlockReleaseBuffer(buf);
	for (int k = 0; k < BMAP_TREE_TOPS; k++)
	{
		if (BlockNumberIsValid(root.top[k].block) &&
			tree_node_own(rel, &root.top[k], epoch, true) &&
			!tree_preserve_node(src, &root.top[k], k, (BlockNumber) first,
								nblocks, vm_nblocks, epoch, walk))
			return false;
		first += tree_span(k);
	}
	return true;
}

/*
 * The relation's header, locked exclusively, its own blocks preserved
 * (tree_preserve()), and collected into 'own' if that is given.
 *
 * A walk given up on a busy page (PreserveWalk) lets go of the header,
 * waits for the page's share lock holding no map page, and walks again with
 * the page held until it is logged.  What may change while the header is let
 * go: the caller keeps the relation's writers and VACUUM out, and a sweep's
 * counting (the sweep lock, held), so only a change of no row or a row
 * lock's (a prune, a SELECT FOR UPDATE's lock) prepares a page meanwhile,
 * which may claim or copy it and change its entry and the counts; a
 * compaction under way may move a page to another block; and another share,
 * or a capture, may raise the epoch and make blocks shared.  The walk again
 * starts from the header as it is then, its length and epoch, and so finds
 * the blocks as they are mapped then, logging those not logged already
 * (preserve_shared_page()).  A walk gives up only on a block it hasn't
 * logged (one logged is skipped without its lock), and the next walk holds
 * that page and logs it; so the walks given up are no more than the blocks
 * logged, which are the relation's own pages and those that a copy or a
 * compaction gave another block while the header was let go.  The walks
 * end however many backends lock the pages meanwhile, each page logged
 * once unless it moved, and only one page held at a time.
 */
static Buffer preserve_header_lock_ext(SMgrRelation src, BlockList *own,
										bool concurrent, SyncedBlocks *synced);

static Buffer
preserve_header_lock(SMgrRelation src, BlockList *own, SyncedBlocks *synced)
{
	return preserve_header_lock_ext(src, own, false, synced);
}

/*
 * preserve_header_lock(), with 'concurrent': the relation's writers aren't
 * kept out.  Then each walk, its header held, first has each of the
 * relation's cached pages prepared again before it next changes
 * (MarkRelationPagesShared(), only its pages): a
 * writer's next change of one of the relation's pages waits for the
 * header (its preparation reads the map's epoch under it) and, once the
 * caller has raised the epoch, copies it.  What may still change a page
 * the walk sees is a writer that locked it before; so every page of the
 * walk's, those logged by an earlier walk too, must have no one holding
 * it then (preserve_page_idle()), and a page busy gives the walk up, as
 * above.  A walk that ends, the header still held, finds the relation's
 * own pages each as its last change left it, none of them changing: the
 * relation as at one moment, between any two of its changes, as a crash
 * would leave it once its WAL was replayed (a change of several pages,
 * as a B-tree page's split, is made under all their locks).  The walks
 * given up are bounded (PRESERVE_CONCURRENT_WALKS): InvalidBuffer, the
 * header not held, if they run out.
 */
#define PRESERVE_CONCURRENT_WALKS	32

static Buffer
preserve_header_lock_ext(SMgrRelation src, BlockList *own, bool concurrent,
						 SyncedBlocks *synced)
{
	RelFileLocator rel = src->smgr_rlocator.locator;
	Buffer		headerbuf = map_read(rel, 0);
	PreserveWalk walk = {0};
	int			walks = 0;

	walk.own = own;
	walk.held_block = InvalidBlockNumber;
	walk.record_below = InvalidBlockNumber;
	walk.concurrent = concurrent;
	walk.synced = synced;
	for (;;)
	{
		BmapHeader *header;
		bool		done;

		LockBuffer(headerbuf, BUFFER_LOCK_EXCLUSIVE);
		check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel,
				   BMAP_FORKNUM, 0);
		if (concurrent)
			MarkRelationPagesShared(rel);
		header = PageGetBmapHeader(BufferGetPage(headerbuf));
		if (own)
			own->n = 0;
		if (synced)
		{
			synced->n = synced->own = 0;
			synced->leaves_whole = synced->entries = synced->asked = 0;
			synced->nleaves = 0;
			synced->nblocks = header->nblocks;
			synced->vm_nblocks = header->vm_nblocks;
			synced->newest = InvalidXLogRecPtr;
		}
		walk.busy = InvalidBlockNumber;
		walk.began = GetXLogInsertRecPtr();
		done = tree_preserve(src, header->nblocks, header->vm_nblocks,
							 header->epoch, &walk);
		/* (a page held that the walk found no longer the relation's own) */
		preserve_release_held(&walk);
		if (done)
			break;

		/* the first walk given up: the blocks it logged, recorded */
		if (walk.logged == NULL)
		{
			HASHCTL		ctl;

			ctl.keysize = sizeof(BlockNumber);
			ctl.entrysize = sizeof(PreserveLogged);
			ctl.hcxt = CurrentMemoryContext;
			walk.logged = hash_create("bmap preserved blocks", 256, &ctl,
									  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
			walk.record_below = walk.busy;
			(void) tree_preserve(src, header->nblocks, header->vm_nblocks,
							 header->epoch, &walk);
			walk.record_below = InvalidBlockNumber;
		}

		LockBuffer(headerbuf, BUFFER_LOCK_UNLOCK);
		INJECTION_POINT("bmap-preserve-restart", NULL);
		if (concurrent && ++walks >= PRESERVE_CONCURRENT_WALKS)
		{
			if (walk.logged)
				hash_destroy(walk.logged);
			ReleaseBuffer(headerbuf);
			return InvalidBuffer;
		}
		walk.held = ReadBufferWithoutRelcache(rel, key_fork(walk.busy),
											  key_block(walk.busy),
											  RBM_NORMAL, NULL, true);
		LockBuffer(walk.held, BUFFER_LOCK_SHARE);
		walk.held_block = walk.busy;
	}
	if (walk.logged)
		hash_destroy(walk.logged);
	return headerbuf;
}

/*
 * In redo's walk of a capture (redo_own_blocks()), page 'blk' of 'rel' (the
 * page store's file or its counts') into dst: as replay holds it in a
 * buffer, if it does, else as it is on disk, if its checksum verifies it.
 * Never through a buffer read for it: the block may hold what the replay
 * hasn't reached, a relation's page (whose checksum is of its logical
 * block) in a node freed since, or a page torn by the crash whose image
 * comes later in the WAL.  False if it is past the file, or not verified.
 */
static bool
redo_walk_read(RelFileLocator rel, BlockNumber blk, char *dst)
{
	SMgrRelation srel = smgropen(rel, INVALID_PROC_NUMBER);
	PrefetchBufferResult r;
	void	   *page = dst;

	if (!smgrexists(srel, MAIN_FORKNUM) || blk >= mdnblocks(srel, MAIN_FORKNUM))
		return false;
	r = PrefetchSharedBuffer(srel, MAIN_FORKNUM, blk);
	if (BufferIsValid(r.recent_buffer) &&
		ReadRecentBuffer(rel, MAIN_FORKNUM, blk, r.recent_buffer))
	{
		LockBuffer(r.recent_buffer, BUFFER_LOCK_SHARE);
		memcpy(dst, BufferGetPage(r.recent_buffer), BLCKSZ);
		UnlockReleaseBuffer(r.recent_buffer);
		return true;
	}
	mdreadv(srel, MAIN_FORKNUM, blk, &page, 1);
	return PageIsVerified(dst, blk, 0, NULL);
}

/* the nodes, and count pages, redo's walk of a capture or a share read */
static uint32 redo_walk_nodes = 0;
static uint32 redo_walk_ref_pages = 0;

/*
 * The count page redo's walk read last, and the count file's length, for
 * the lookups after it (redo_lookup_ref()).  A walk's blocks are mostly on
 * few count pages, a page counting BMAP_REFS_PER_PAGE blocks, so each is
 * read once, not once a block.  Nothing changes a count page during a walk,
 * which only reads.
 */
typedef struct RedoRefsPage
{
	BlockNumber pageno;			/* InvalidBlockNumber: none read */
	BlockNumber nblocks;		/* InvalidBlockNumber: not asked */
	bool		usable;			/* read as one replay has reached */
	PGIOAlignedBlock page;
} RedoRefsPage;

/*
 * In redo, the count of 'phys' into *count, as lookup_ref() reads it; false
 * if its page can't be read as one replay has reached (redo_walk_read()),
 * isn't a count page, or is newer than 'lsn' (see redo_own_blocks()).  The
 * page read is kept in 'rp' for the next lookup.
 */
static bool
redo_lookup_ref(RelFileLocator rel, BlockNumber phys, XLogRecPtr lsn,
				RedoRefsPage *rp, uint32 *count)
{
	*count = 0;
	if (rp->nblocks == InvalidBlockNumber)
		rp->nblocks = refs_nblocks(rel);
	if (REF_PAGE(phys) >= rp->nblocks)
		return true;
	if (rp->pageno != REF_PAGE(phys))
	{
		rp->pageno = REF_PAGE(phys);
		rp->usable = redo_walk_read(with_relnumber(rel, BMAP_REFS_RELNUMBER),
									rp->pageno, rp->page.data);
		redo_walk_ref_pages++;
	}
	if (!rp->usable)
		return false;
	/* (a page extended and never written counts none) */
	if (PageIsNew(rp->page.data))
		return true;
	if (!map_page_is(rp->page.data, BMAP_PAGE_REFS) ||
		PageGetLSN(rp->page.data) > lsn)
		return false;
	*count = page_ref(rp->page.data, phys);
	return true;
}

/*
 * The shares redo has seen the tree's record of and not yet the record
 * that raises the source's epoch (XLOG_BMAP_SHARED): each copy's pointers,
 * the source's tops as of the share.  The source's header is held between
 * the two records, so its tree is the same at the second.  Shares are made
 * one at a time per source, and few at once: a share whose tree record
 * was replayed before recovery began, or pushed out by others, is redone
 * without its tops, visiting every buffer.
 */
#define REDO_SHARES		8

typedef struct RedoShare
{
	RelFileLocator dst;
	xl_bmap_top tops[BMAP_DELTA_ROOT_TOPS];
	bool		valid;
} RedoShare;

static RedoShare redo_shares[REDO_SHARES];
static int	redo_shares_next = 0;

static void
redo_share_begun(RelFileLocator dst, const xl_bmap_top *tops)
{
	RedoShare  *rs;

	/*
	 * A share to 'dst' begun before and never finished (an error between
	 * its records) is forgotten: its tops aren't this one's.
	 */
	for (int i = 0; i < REDO_SHARES; i++)
		if (redo_shares[i].valid && RelFileLocatorEquals(redo_shares[i].dst, dst))
			redo_shares[i].valid = false;
	rs = &redo_shares[redo_shares_next];
	redo_shares_next = (redo_shares_next + 1) % REDO_SHARES;
	rs->dst = dst;
	memcpy(rs->tops, tops, sizeof(rs->tops));
	rs->valid = true;
}

/* the share to 'dst' begun (redo_share_begun()), forgotten; false if none */
static bool
redo_share_tops(RelFileLocator dst, xl_bmap_top *tops)
{
	for (int i = 0; i < REDO_SHARES; i++)
	{
		if (redo_shares[i].valid && RelFileLocatorEquals(redo_shares[i].dst, dst))
		{
			memcpy(tops, redo_shares[i].tops, sizeof(redo_shares[i].tops));
			redo_shares[i].valid = false;
			return true;
		}
	}
	return false;
}

/*
 * 'held_more' is the holders the node's count has that the walk's record
 * gave it already: the copy's, for a top of a share's source.
 */
static bool
redo_own_node(RelFileLocator rel, const BmapEntry *ptr, uint16 level,
			  BlockNumber first, BlockNumber nblocks, BlockNumber vm_nblocks,
			  uint32 epoch, XLogRecPtr lsn, uint32 held_more, RedoRefsPage *rp,
			  BlockList *own)
{
	PGIOAlignedBlock page;
	BmapEntry  *entries;
	uint32		count;
	bool		ok = true;

	if (!range_in_length(nblocks, vm_nblocks, first,
						 (uint64) first + tree_span(level)))
		return true;
	if (ptr->epoch < epoch)
	{
		if (!redo_lookup_ref(rel, ptr->block, lsn, rp, &count))
			return false;
		if (count < held_more)
			return false;
		if (count > held_more)
			return true;
	}
	if (!redo_walk_read(with_relnumber(rel, BMAP_STORE_RELNUMBER), ptr->block,
						page.data) ||
		PageGetLSN(page.data) > lsn ||
		!tree_node_is(page.data, level, first, ptr->since))
		return false;
	redo_walk_nodes++;
	entries = palloc(BMAP_TREE_FANOUT * sizeof(BmapEntry));
	memcpy(entries, PageGetBmapEntries(page.data),
		   BMAP_TREE_FANOUT * sizeof(BmapEntry));
	for (uint32 i = 0; i < BMAP_TREE_FANOUT && ok; i++)
	{
		uint64		child = (uint64) first + (level == 0 ? i :
											   i * tree_span(level - 1));

		if (!BlockNumberIsValid(entries[i].block) ||
			!range_in_length(nblocks, vm_nblocks, child,
							 child + (level == 0 ? 1 : tree_span(level - 1))))
			continue;
		if (level > 0)
			ok = redo_own_node(rel, &entries[i], level - 1,
							   (BlockNumber) child, nblocks, vm_nblocks, epoch,
							   lsn, 0, rp, own);
		else if (!redo_lookup_ref(rel, entries[i].block, lsn, rp, &count))
			ok = false;
		else if (count == 0)
			block_list_add(own, (BlockNumber) child);
	}
	pfree(entries);
	return ok;
}

/*
 * In redo of a root captured ('snap', the record's, which begins at
 * 'lsn'), its own blocks into 'own', as tree_preserve() collects them where
 * it was captured: the only pages of the relation that may be dirty in their
 * buffers only, redo having written out at the relation's last capture or
 * share (or begun after it) what changed before.  Called before the
 * record's own changes are redone, so that the counts are those the
 * primary read.
 *
 * Recovery may read a map page, or a count page, newer than the replay: a
 * version written past this point before a crash, or a base backup's.  Its
 * pointers and counts may name as shared a subtree that changed since the
 * last capture, and the walk would miss a page redo dirtied there.  The
 * block may also hold another page by now (a node freed since and taken
 * as a relation's page), or one the crash tore.  So the walk gives up
 * (false), and the caller writes out every buffer of the relation instead,
 * at a page it reads (redo_walk_read()) that is past its file, isn't
 * verified, is newer than the record, or isn't the node (or count page)
 * the pointer to it names.  It relies on data checksums to find a torn
 * page: without them the caller doesn't walk.
 *
 * Redo of a share walks its source the same way, at the record that raises
 * the source's epoch ('snap' made of what the share's records hold), after
 * the share's tops each got a holder more for the copy: 'tops_held' says
 * so.
 */
static bool
redo_own_blocks(RelFileLocator rel, const xl_bmap_snapshot *snap,
				XLogRecPtr lsn, bool tops_held, BlockList *own)
{
	uint64		first = 0;

	/* (the map's epoch before the capture raised it) */
	uint32		epoch = snap->epoch - 1;

	/* (on the stack: its page is read into, I/O-aligned) */
	RedoRefsPage rp;

	rp.pageno = InvalidBlockNumber;
	rp.nblocks = InvalidBlockNumber;
	redo_walk_nodes = 0;
	redo_walk_ref_pages = 0;

	for (int k = 0; k < BMAP_TREE_TOPS; k++)
	{
		if (BlockNumberIsValid(snap->tops[k].block))
		{
			BmapEntry	top;

			top.block = snap->tops[k].block;
			top.epoch = snap->tops[k].epoch;
			top.since = snap->tops[k].since;
			if (!redo_own_node(rel, &top, k, (BlockNumber) first,
							   snap->nblocks, snap->vm_nblocks, epoch, lsn,
							   tops_held ? 1 : 0, &rp, own))
				return false;
		}
		first += tree_span(k);
	}
	return true;
}

/*
 * A tree shared: dst's root page (made with no subtrees) gets src's
 * pointers, and each subtree a holder more, in one record.  That is all the
 * map a share copies: the subtrees stay shared until either relation
 * changes something under one, which copies the path to it.  With 'from',
 * the pointers are those of a root captured of src, and dst's header gets
 * 'hdelta' in the same record, and src's header (held exclusively by the
 * caller in 'sheader', if valid) 'shdelta'.  dst begins a lineage of its
 * own, or with 'lineage' not 0 carries that one on.
 */
static void
tree_share_root(SMgrRelation src, SMgrRelation dst, const BmapEntry *from,
				const xl_bmap_header_delta *hdelta, Buffer sheader,
				const xl_bmap_header_delta *shdelta, uint64 made_of,
				uint64 lineage)
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
	rdelta.made_of = made_of;
	rdelta.lineage = lineage;
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
	refs_add_holders(&rp, srel, tops, ntops);

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
	xlrec.flags = hdelta ? BMAP_TREE_OF_ROOT : 0;
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
	/* (the new map's identity: where its record began) */
	PageGetBmapRoot(BufferGetPage(buf))->map_id = ProcLastRecPtr;
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
}

/*
 * The redo pointer of the last checkpoint completed (in recovery, of the
 * last restartpoint): where a recovery of this server would begin
 */
static XLogRecPtr
last_checkpoint_redo(void)
{
	XLogRecPtr	redo;
	TimeLineID	tli;

	GetOldestRestartPoint(&redo, &tli);
	return redo;
}

/*
 * Where a recovery that reads blocks copied without an image may begin, at
 * the earliest: this server's own (last_checkpoint_redo()), or, on a
 * primary, a standby's it knows of (the standby hold), if earlier.  A block
 * copied from without an image after it is kept as it is.
 */
static XLogRecPtr
copy_horizon(void)
{
	XLogRecPtr	redo = last_checkpoint_redo();
	XLogRecPtr	hold = pg_atomic_read_u64(&bmap_shared->standby_hold);

	if (XLogRecPtrIsValid(hold) && hold < redo && !RecoveryInProgress())
		return hold;
	return redo;
}

/*
 * The online backups running (GetBackupState()), and one more if a standby
 * this server knows of runs one: a copy or a share then logs the images it
 * would leave out, as for one of this server's.
 */
static void
backup_state(int *running, XLogRecPtr *last_start)
{
	GetBackupState(running, last_start);
	if (pg_atomic_read_u32(&bmap_shared->standby_backups) != 0)
		(*running)++;
}

/*
 * The standby hold (WalSndComputeRecoveryHold()), published: 'hold' the
 * earliest position a recovery of a standby known may begin at
 * (InvalidXLogRecPtr: none), and whether one runs an online backup.
 */
void
bmap_set_standby_hold(XLogRecPtr hold, bool backups)
{
	if (bmap_shared == NULL)
		return;
	pg_atomic_write_u64(&bmap_shared->standby_hold, hold);
	pg_atomic_write_u32(&bmap_shared->standby_backups, backups ? 1 : 0);
}

/* The standby hold as published, and whether a standby runs a backup. */
XLogRecPtr
bmap_standby_hold(bool *backups)
{
	if (bmap_shared == NULL)
	{
		*backups = false;
		return InvalidXLogRecPtr;
	}
	*backups = pg_atomic_read_u32(&bmap_shared->standby_backups) != 0;
	return pg_atomic_read_u64(&bmap_shared->standby_hold);
}

/*
 * Log images of the own blocks a share synced (SyncedBlocks), read from the
 * page store, after the share's record: for a base backup begun while the
 * share ran, which may have copied a block before it was written.  They
 * are found on the leaves the share's walk noted, read as they were then:
 * shared by the share, a leaf doesn't change in place (a change copies it
 * first), as the source may since, in a prune or a row lock.  Each entry
 * got before 'image_from' is logged, which takes in those of a leaf the
 * share found shared already too.  The images name the copy, 'dst', whose
 * map names the same blocks as the leaves, and whose buffer their redo
 * restores (the source's may name another block by then).  Every block is
 * shared, so nothing changes it in place: each holds its page as at the
 * share, which its image's redo writes there again, as
 * preserve_shared_page()'s does.
 */
static void
preserve_synced(SMgrRelation src, SMgrRelation dst, SyncedBlocks *synced)
{
	RelFileLocator srel = src->smgr_rlocator.locator;
	RelFileLocator drel = dst->smgr_rlocator.locator;
	BmapEntry  *entries = palloc(BMAP_TREE_FANOUT * sizeof(BmapEntry));
	uint64		n = 0;

	for (uint32 l = 0; l < synced->nleaves; l++)
	{
		const SyncedLeaf *leaf = &synced->leaves[l];
		Buffer		buf = tree_read(srel, leaf->node, RBM_NORMAL);

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		if (!tree_node_is(BufferGetPage(buf), 0, leaf->first, leaf->born))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("block %u of the page store of \"%s\" is not the node of its block map it was",
							leaf->node, relpath(src->smgr_rlocator, MAIN_FORKNUM).str)));
		memcpy(entries, PageGetBmapEntries(BufferGetPage(buf)),
			   BMAP_TREE_FANOUT * sizeof(BmapEntry));
		UnlockReleaseBuffer(buf);
		for (uint32 i = 0; i < BMAP_TREE_FANOUT; i++)
		{
			uint64		key = (uint64) leaf->first + i;
			PGIOAlignedBlock copy;
			void	   *page = copy.data;

			if (!BlockNumberIsValid(entries[i].block) ||
				entries[i].since >= synced->image_from ||
				!range_in_length(synced->nblocks, synced->vm_nblocks, key, key + 1))
				continue;
			mdreadv(store_smgr(srel), MAIN_FORKNUM, entries[i].block, &page, 1);
			XLogBeginInsert();
			XLogRegisterData(&entries[i].block, sizeof(BlockNumber));
			XLogRegisterBlock(0, &drel, key_fork(key), key_block(key), copy.data,
							  REGBUF_FORCE_IMAGE | key_image_flags(key));
			XLogInsert(RM_BMAP_ID, XLOG_BMAP_PRESERVE);
			n++;
			CHECK_FOR_INTERRUPTS();
		}
	}
	pfree(entries);
	elog(DEBUG1, "a share of %s logged images of " UINT64_FORMAT " blocks on the %u leaves it synced, a backup having begun",
		 relpathperm(srel, MAIN_FORKNUM).str, n, synced->nleaves);
}

/*
 * Make 'dst', a new relation file number in the same tablespace and
 * database, a copy of 'src' that shares its blocks: dst's map holds src's
 * entries, each block's count grows by one, and both maps' epochs pass the
 * entries' births.  A tree's copy holds its subtrees instead, each of which
 * gains a holder (tree_share_root()).  src's dirty buffers are written, so
 * the blocks hold what both copies start from.
 *
 * What the share logs is of a size that doesn't grow with src's: src's own
 * blocks (those no other map holds, which become shared) are made durable
 * by writing them out and syncing the page store, not by logging their
 * images, but for those src got since the checkpoint last completed as the
 * share began, which are logged as images (PRESERVE) as a full-page write
 * logs a page's first change since a checkpoint.  Why images are needed
 * at all: redo may skip src's records from before the share (src's map on
 * disk newer than the replay, a path above the block copied or the block
 * itself copied after the share, src dropped or truncated since), and the
 * block shared must hold src's page then all the same.  Before the share,
 * though, replay may also write something else over the block: the
 * extension or copy that gave the block to src zeroes it again, and a
 * node's or an earlier holder's page may be written out there.  Every such
 * write comes from a record from before src got the block (its entry's
 * 'since'), and after that only src's records reach the block, and nothing
 * after the share (a change copies a shared page first, and no hint is set
 * on one).  So a recovery that begins after src got the block finds it as
 * the share's sync left it, whatever it skips.  Crash recovery begins at a
 * checkpoint no older than the one the share began after; the redo of the
 * share (on a standby, or in archive recovery) syncs the blocks it writes
 * and moves its server's restart point past the newest of them
 * ('synced_since'), asking for a restartpoint if it must; and an image is
 * logged of each block src got since the checkpoint, which a recovery
 * from before it replays after all such writes.  Both the share and its
 * redo keep the position in pg_control once the blocks are durable
 * (XLogNoteSyncedPages()), where it keeps any recovery from beginning at or
 * before it: a standby's that couldn't make the restartpoint, and a
 * server's that pg_rewind made of this one, which replays from the last
 * checkpoint it had in common with this one over the page store copied as
 * it is now (pg_rewind refuses such a source, and one that syncs blocks so
 * while it copies files).
 *
 * A base backup copies the page store while the share may write it: one
 * running as the share begins gets an image of every own block, as does a
 * relation whose data skips WAL (made in this transaction at wal_level
 * minimal), whose own records aren't in WAL; one begun during the share
 * (GetBackupState()) has the images logged after the record, read from the
 * blocks once they are synced, which a recovery from the backup replays.
 * A backup taken on a standby while it replays the share fails as it
 * stops (XLogNoteSyncedReplay()).
 *
 * The write of src's blocks is needed for another reason too: a relation
 * made of the share reads them.  For no own block, it is needed only while
 * another share's is in flight (store_written_out()).
 *
 * The caller keeps src from changing meanwhile (a lock that waits for its
 * writers): a change made to one of src's buffers before the epochs are
 * raised would be written to a block dst shares, and none of src's or
 * dst's maps may change before the blocks are synced (the sync makes up
 * for records redo skips only because a map changed since).  An error
 * partway leaves dst with some of src's blocks counted; dropping dst gives
 * them back.
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
	BlockNumber vm_nblocks;
	uint32		epoch;
	xl_bmap_header_delta sdelta;
	xl_bmap_header_delta ddelta;
	xl_bmap_share xlrec;
	XLogRecPtr	lsn;
	LOCKTAG		sweeptag;
	SyncedBlocks synced = {0};
	XLogRecPtr	checkpointed;
	int			backups;
	XLogRecPtr	backup_start;

	if (srel.spcOid != drel.spcOid || srel.dbOid != drel.dbOid)
		elog(ERROR, "block-mapped relation %s can't share the blocks of %s, in another page store",
			 relpath(dst->smgr_rlocator, MAIN_FORKNUM).str,
			 relpath(src->smgr_rlocator, MAIN_FORKNUM).str);

	/*
	 * Not while a sweep counts what the maps hold, nor while it moves blocks:
	 * held until the own blocks are synced, and logged if they must be.
	 */
	SET_LOCKTAG_OBJECT(sweeptag, srel.dbOid, BMAP_SWEEP_CLASSID, srel.spcOid, 0);
	(void) LockAcquire(&sweeptag, ShareLock, false, false);

	/* every allocated block has a reference count */
	metabuf = alloc_read(srel, 0);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(metabuf), BMAP_PAGE_META, srel, MAIN_FORKNUM, 0);
	refs_cover(srel, PageGetBmapMeta(BufferGetPage(metabuf))->hw);
	UnlockReleaseBuffer(metabuf);

	create_map(dst);

	/*
	 * Which own blocks are synced instead of logged: those src got before
	 * the checkpoint last completed, unless a backup runs or src's data
	 * skips WAL (then none).
	 */
	checkpointed = copy_horizon();
	backup_state(&backups, &backup_start);
	synced.image_from = (backups > 0 || RelFileLocatorSkippingWAL(srel)) ?
		InvalidXLogRecPtr : checkpointed;

	/*
	 * src's header, held until the epochs are raised: no change meanwhile.
	 * src's own blocks preserved, its subtrees a holder more.
	 */
	sheader = preserve_header_lock(src, NULL, &synced);
	elog(DEBUG1, "a share of %s found " UINT64_FORMAT " own blocks, " UINT64_FORMAT " of them synced: " UINT64_FORMAT " leaves counted whole, " UINT64_FORMAT " entries looked at, " UINT64_FORMAT " counts asked",
		 relpathperm(srel, MAIN_FORKNUM).str, synced.own, synced.n,
		 synced.leaves_whole, synced.entries, synced.asked);
	nblocks = PageGetBmapHeader(BufferGetPage(sheader))->nblocks;
	vm_nblocks = PageGetBmapHeader(BufferGetPage(sheader))->vm_nblocks;
	epoch = PageGetBmapHeader(BufferGetPage(sheader))->epoch;
	if (synced.own > 0)
		store_flush_begin(srel);
	tree_share_root(src, dst, NULL, NULL, InvalidBuffer, NULL, 0, 0);
	INJECTION_POINT("bmap-share-map-page", NULL);

	dheader = map_read(drel, 0);
	LockBuffer(dheader, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(dheader), BMAP_PAGE_HEADER, drel, BMAP_FORKNUM, 0);

	sdelta.kind = ddelta.kind = BMAP_DELTA_HEADER;
	sdelta.flags = ddelta.flags = BMAP_DELTA_SHARED_BELOW;
	sdelta.nblocks = ddelta.nblocks = nblocks;
	sdelta.vm_nblocks = ddelta.vm_nblocks = vm_nblocks;
	sdelta.free_space_unknown = ddelta.free_space_unknown = 0;
	/* (the copy has no free space map: none of its pages' is known) */
	ddelta.flags |= BMAP_DELTA_FREE_SPACE;
	ddelta.free_space_unknown = nblocks;
	sdelta.epoch = ddelta.epoch = epoch + 1;
	/* (every entry either has is the other's too) */
	sdelta.shared_below = ddelta.shared_below = epoch + 1;
	/* (its padding too, which goes to WAL) */
	memset(&xlrec, 0, sizeof(xlrec));
	xlrec.src = srel;
	xlrec.dst = drel;
	/* (tree_preserve() logged images of the own blocks not synced) */
	xlrec.flags = BMAP_PRESERVE_LOGGED;
	if (synced.n > 0)
	{
		xlrec.flags |= BMAP_SHARE_SYNCED;
		xlrec.synced_since = synced.newest;
	}

	START_CRIT_SECTION();
	apply_header_delta(BufferGetPage(sheader), &sdelta);
	MarkBufferDirty(sheader);
	apply_header_delta(BufferGetPage(dheader), &ddelta);
	MarkBufferDirty(dheader);
	XLogBeginInsert();
	XLogRegisterData(&xlrec, sizeof(xlrec));
	/*
	 * (the delta kept with an image too: redo finds the source's own blocks
	 * from its length and epoch, and without them writes out every buffer)
	 */
	XLogRegisterBuffer(0, sheader, REGBUF_STANDARD | REGBUF_KEEP_DATA);
	XLogRegisterBufData(0, &sdelta, sizeof(sdelta));
	XLogRegisterBuffer(1, dheader, REGBUF_STANDARD);
	XLogRegisterBufData(1, &ddelta, sizeof(ddelta));
	lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_SHARED);
	PageSetLSN(BufferGetPage(sheader), lsn);
	PageSetLSN(BufferGetPage(dheader), lsn);
	END_CRIT_SECTION();

	UnlockReleaseBuffer(dheader);
	UnlockReleaseBuffer(sheader);
	dst->smgr_cached_nblocks[MAIN_FORKNUM] = InvalidBlockNumber;
	dst->smgr_cached_nblocks[VISIBILITYMAP_FORKNUM] = InvalidBlockNumber;

	/*
	 * src's cached pages are checked again before they next change, as pages
	 * read from now on are: the generation of sharing is raised, visiting
	 * no buffer.  A page found src's own before the epoch was raised may
	 * change in place until then; with src kept from changing, only a hint
	 * or pruning can change it, and either holds for dst too, whose page is
	 * the same (and gets it when flushed, below).
	 */
	MarkRelationPagesShared(src->smgr_rlocator.locator);
	MarkRelationPagesShared(dst->smgr_rlocator.locator);

	/*
	 * The shared blocks get src's latest pages.  The PRESERVE images logged
	 * before the record make those they are of durable, as WAL makes any
	 * page durable: recovery from a checkpoint before an image replays it to
	 * the block, after whatever earlier records it replays for the block,
	 * and one from a later checkpoint found the block written and synced by
	 * it (a page written out registers its segment for the checkpointer's
	 * sync, and one dirty as the checkpoint began is written by it).  The
	 * others are synced here, before src or dst can change their maps (the
	 * caller keeps them from it until the share is done): so a recovery
	 * that skips src's records for a block because a map changed finds the
	 * block synced, and one from before the sync finds every map as at the
	 * share, and replays src's records to the block.
	 *
	 * With no block of src's own, every block dst gets was shared before,
	 * written out by the share or capture that first shared it, and
	 * unchanged in place since (a change copies a shared page first, and
	 * only a hint can reach its buffer): the flush is needed only while
	 * another share's is in flight (store_written_out()).
	 */
	if (synced.own > 0 || !store_written_out(srel))
		FlushRelationsAllBuffers(&src, 1);
	if (synced.n > 0)
	{
		int			backups_now;
		XLogRecPtr	backup_start_now;

		INJECTION_POINT("bmap-share-sync", NULL);
		mdimmedsync(store_smgr(srel), MAIN_FORKNUM);

		/*
		 * A backup begun since the share did may have copied a block before
		 * its write: the blocks' images, as they are synced, are logged
		 * after all, for a recovery from the backup to replay.
		 */
		backup_state(&backups_now, &backup_start_now);
		if (backups_now > 0 || backup_start_now != backup_start)
			preserve_synced(src, dst, &synced);

		/*
		 * Said in pg_control, once the blocks are durable and the share's
		 * record is (the note stands for it: copies_passed()): no recovery
		 * may begin at or before the newest of them was given to src, and a
		 * rewind copying files meanwhile can't rely on what it copied.
		 */
		XLogFlush(lsn);
		XLogNoteSyncedPages(synced.newest);
	}
	if (synced.own > 0)
		store_flush_end(srel, false);
	LockRelease(&sweeptag, ShareLock, false);
	if (synced.leaves)
		pfree(synced.leaves);
}

/*
 * Is a slot's poison covered, a checkpoint begun after it ('checkpointed',
 * the redo pointer of the last one completed) having written and synced
 * every page dirty then?  Then it is let go of.  The mutex is held.
 */
static bool
slot_poisoned(BmapStoreSync *slot, XLogRecPtr checkpointed)
{
	if (slot->poison_lsn != InvalidXLogRecPtr && checkpointed > slot->poison_lsn)
		slot->poison_lsn = InvalidXLogRecPtr;
	return slot->poison_lsn != InvalidXLogRecPtr;
}

/*
 * The slot following rel's page store's flushes (BmapStoreSync), taken if
 * 'take' and there is room, or one may be let go of (idle, its poison
 * covered as of 'checkpointed'); NULL if it has none.  The mutex is held.
 */
static BmapStoreSync *
store_sync_slot(RelFileLocator rel, bool take, XLogRecPtr checkpointed)
{
	uint64		key = watch_key(rel);
	BmapStoreSync *slot = NULL;

	for (int i = 0; i < bmap_shared->nsyncs_followed; i++)
		if (bmap_shared->syncs_followed[i].key == key)
		{
			slot = &bmap_shared->syncs_followed[i];
			slot->used = ++bmap_shared->sync_clock;
			return slot;
		}
	if (!take)
		return NULL;
	if (bmap_shared->nsyncs_followed < BMAP_SYNCS_FOLLOWED)
		slot = &bmap_shared->syncs_followed[bmap_shared->nsyncs_followed++];
	else
	{
		/* the least recently used slot that may be let go of, if any */
		for (int i = 0; i < BMAP_SYNCS_FOLLOWED; i++)
		{
			BmapStoreSync *s = &bmap_shared->syncs_followed[i];

			if (s->flushing == 0 && !slot_poisoned(s, checkpointed) &&
				(slot == NULL || s->used < slot->used))
				slot = s;
		}
		if (slot == NULL)
			return NULL;
	}
	slot->key = key;
	slot->flushing = 0;
	slot->written = 0;
	slot->poison_lsn = InvalidXLogRecPtr;
	slot->used = ++bmap_shared->sync_clock;
	return slot;
}

/*
 * The flushes this backend began of a share's or capture's own blocks
 * (store_flush_begin()) and hasn't ended, once for each, with whether the
 * store's slot follows it; any left as the transaction ends (an error part
 * way) are ended then as given up, which poisons the store's slot, or the
 * flushes no slot follows (BmapShared), until a checkpoint begun after.
 */
typedef struct FlushingStore
{
	RelFileLocator rel;
	bool		followed;
} FlushingStore;

static FlushingStore *flushing_stores = NULL;
static int	n_flushing_stores = 0;
static int	max_flushing_stores = 0;

/*
 * A share or capture of a relation of rel's page store, holding blocks of
 * its own, is about to make them shared and write them out: the store is
 * not written out for one holding no block of its own until the write
 * completes.  Called before the blocks' counts are raised, with the
 * relation's header held exclusively, so that whoever finds them held
 * elsewhere finds this flush in flight or written.  With no slot to be had
 * (every one in flight or poisoned), the flush is counted among those no
 * slot follows, while which no store is written out.
 */
static void
store_flush_begin(RelFileLocator rel)
{
	BmapStoreSync *slot;
	XLogRecPtr	checkpointed = last_checkpoint_redo();

	if (n_flushing_stores == max_flushing_stores)
	{
		max_flushing_stores = max_flushing_stores ? max_flushing_stores * 2 : 4;
		flushing_stores = flushing_stores
			? repalloc_array(flushing_stores, FlushingStore, max_flushing_stores)
			: MemoryContextAlloc(TopMemoryContext,
								 max_flushing_stores * sizeof(FlushingStore));
	}
	SpinLockAcquire(&bmap_shared->mutex);
	slot = store_sync_slot(rel, true, checkpointed);
	if (slot)
		slot->flushing++;
	else
		bmap_shared->unfollowed_flushing++;
	SpinLockRelease(&bmap_shared->mutex);
	flushing_stores[n_flushing_stores].rel = rel;
	flushing_stores[n_flushing_stores++].followed = slot != NULL;
}

/*
 * The flush store_flush_begin() began is written; or, if 'given_up' (its
 * transaction ending before it was), may not have been, which poisons the
 * store's slot (BmapStoreSync).  One no slot followed counts in the
 * store's slot now if it has one (else a slot taken later begins as a
 * flush written); given up, it poisons every store (BmapShared).
 */
static void
store_flush_end(RelFileLocator rel, bool given_up)
{
	uint64		key = watch_key(rel);
	BmapStoreSync *slot;
	XLogRecPtr	poison;
	bool		followed;
	int			i;

	for (i = n_flushing_stores - 1; i >= 0; i--)
		if (watch_key(flushing_stores[i].rel) == key)
			break;
	if (i < 0)
		return;
	followed = flushing_stores[i].followed;
	flushing_stores[i] = flushing_stores[--n_flushing_stores];
	/* (past every record the flush's relations may have, as it ends) */
	poison = given_up ? GetXLogInsertRecPtr() : InvalidXLogRecPtr;
	SpinLockAcquire(&bmap_shared->mutex);
	slot = store_sync_slot(rel, false, InvalidXLogRecPtr);
	if (followed)
	{
		Assert(slot && slot->flushing > 0);
		slot->flushing--;
	}
	else
	{
		Assert(bmap_shared->unfollowed_flushing > 0);
		bmap_shared->unfollowed_flushing--;
	}
	if (given_up && followed)
		slot->poison_lsn = Max(slot->poison_lsn, poison);
	else if (given_up)
		bmap_shared->unfollowed_poison_lsn =
			Max(bmap_shared->unfollowed_poison_lsn, poison);
	else if (slot)
		slot->written++;
	SpinLockRelease(&bmap_shared->mutex);
}

/*
 * Is a flush no slot follows in flight, or one given up whose poison no
 * checkpoint covers yet?  Then no store is written out.  The
 * mutex is held.
 */
static bool
unfollowed_pending(XLogRecPtr checkpointed)
{
	if (bmap_shared->unfollowed_poison_lsn != InvalidXLogRecPtr &&
		checkpointed > bmap_shared->unfollowed_poison_lsn)
		bmap_shared->unfollowed_poison_lsn = InvalidXLogRecPtr;
	return bmap_shared->unfollowed_flushing > 0 ||
		bmap_shared->unfollowed_poison_lsn != InvalidXLogRecPtr;
}

/*
 * Is every block a share of a relation of rel's page store, holding no
 * block of its own, holds written out already?  Each of its blocks is held
 * by another map or root, and so was written out by the share or
 * capture that first made it shared (its own block then), and has not
 * changed in place since (a change copies a shared page first): so when
 * none of those flushes is in flight, and none given up part way (a
 * poisoned slot) since the last checkpoint began.  A store with no slot
 * had none in flight (a slot is let go of only with none, and unpoisoned),
 * but for the flushes no slot follows, while which no store is written out
 * (unfollowed_pending()).  The share made the blocks durable (bmapshare(),
 * syncing them or logging their images): the write is needed because a
 * relation made of the share reads the blocks, which must hold the pages
 * by then.
 */
static bool
store_written_out(RelFileLocator rel)
{
	BmapStoreSync *slot;
	bool		written;
	XLogRecPtr	checkpointed = last_checkpoint_redo();

	SpinLockAcquire(&bmap_shared->mutex);
	slot = store_sync_slot(rel, false, checkpointed);
	written = !unfollowed_pending(checkpointed) &&
		(slot == NULL || (slot->flushing == 0 && !slot_poisoned(slot, checkpointed)));
	SpinLockRelease(&bmap_shared->mutex);
	return written;
}

/*
 * As the transaction ends: the flushes of its shares' and captures' own
 * blocks still in flight (an error part way) are given up.  Nothing is
 * synced here: a share that completes makes its blocks durable itself
 * (bmapshare(), syncing them or logging their images), and one given up
 * part way left no copy that reads them.
 */
void
bmapsyncshares(bool isCommit)
{
	while (n_flushing_stores > 0)
		store_flush_end(flushing_stores[n_flushing_stores - 1].rel, true);
}

/* ----------------------------------------------------------------
 * Owners of what is written into the storage before a commit
 * ----------------------------------------------------------------
 */

/*
 * What became of the (sub)transaction 'owner' that wrote something into
 * the storage before it committed (a captured root's slot, a drop's release
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
	return fork_mapped(forknum);
}

/*
 * Is logical block 'blkno''s block held by another map too, so that
 * preparing it for a change copies it?  *copies says whether its next
 * change is to be prepared at all: to copy it, for that reason, or to
 * claim it, its entry born before the map's epoch.
 */
bool
bmappageisshared(SMgrRelation reln, ForkNumber forknum, BlockNumber blkno,
				 bool *copies)
{
	uint32		epoch;
	uint64		end;
	BmapEntry	entry;
	MapPage		mp;
	bool		shared;

	*copies = false;
	if (!fork_mapped(forknum))
		return false;
	blkno = fork_key(forknum, blkno);
	epoch = lookup_epoch_end(reln, blkno, &end);
	if (!lookup_entry_at(reln, blkno, &entry, &mp, NULL))
	{
		/*
		 * A hole is given a block when it is prepared for its first change,
		 * which copies nothing: there is nothing to copy.
		 */
		*copies = blkno < end;
		return false;
	}
	if (entry.epoch >= epoch)
		return false;
	/*
	 * (a tree's block is shared too if its leaf is, through any node; and
	 * one a copy made without an image may still read is copied as if
	 * shared, so no hint is set on it either, which the copy would take)
	 */
	shared = lookup_ref(reln->smgr_rlocator.locator, entry.block) > 0 ||
		!tree_path_own(reln, blkno, epoch, false) ||
		lookup_copy_holds(reln->smgr_rlocator.locator, entry.block);

	/*
	 * Its next change prepares it all the same, born before the map's
	 * epoch: shared, it is copied; else claimed, its entry taking the epoch,
	 * which is how a frozen root's next capture finds the pages changed
	 * since (bmap_changed_pages())
	 */
	*copies = true;
	return shared;
}

/*
 * Is logical block 'blkno''s block possibly held by another map, not only
 * by the roots captured of this map?  Its entry born before the map's
 * shared_below (raised by a share, and by a copy made of a root) may be;
 * one born since only by its captured roots, which keep the relation's
 * pages as they were.  A hint for callers
 * weighing a copy's cost: preparing the page for a change copies it either
 * way.  shared_below rises with a share (storage made of a root to take
 * another's place takes that storage's, which goes); once the other map is
 * gone (dropped), the entries born before it count as shared elsewhere until
 * they change or a page sweep lowers it (sweep_shared_below()), which
 * costs VACUUM's work on them meanwhile, never their contents.
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

	if (!fork_mapped(forknum))
		return false;
	blkno = fork_key(forknum, blkno);
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
 * (born again in this epoch), unless a copy made without an image may
 * still read it; otherwise the page is copied to a new block, which the
 * map takes, and the old block loses a reference (or, held by nothing
 * now, is freed, which queues it).  The copy's record carries the page's
 * image (without its hole, when that is all zeroes), so redo makes the new
 * block without reading it, unless a full-page write would leave the image
 * out (see "Copies without an image"); a page of zeroes, which its caller
 * is about to initialize, needs no image.  Before the copy, contents only
 * in the buffer are written to the old block, which other maps still
 * read.
 *
 * A tree's entry is changed on a leaf that is the relation's own: a leaf
 * another map shares, or one under a shared node, has its path copied first
 * (tree_leaf_for()), which gives each of the leaf's blocks a holder more, so
 * a block on a leaf just copied is copied in turn as one another map holds.
 *
 * A hole (a block below the length without an entry) is given a block, as
 * a shared page is copied to one, but with nothing to copy: the page is
 * zeroes, or what the caller made of it in its buffer, which the record
 * carries as an image.  The path to its leaf is made where the map has
 * none (tree_leaf_for()), so the map has nodes only where blocks are.
 *
 * Locks: the header shared (so the relation's extensions and sharing wait;
 * exclusively, to make or copy the path to a leaf),
 * the old block's reference-count page, the allocator's pages, then the
 * map page last: reading any of those may write out
 * one of this relation's pages, which looks it up on that page.  No other
 * change of this entry can come meanwhile, since the buffer is locked.
 */
void
bmapprepareforchange(SMgrRelation reln, ForkNumber forknum, BlockNumber blkno,
					 Buffer buffer)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	uint32		epoch;
	BmapEntry	entry;
	Buffer		headerbuf;
	Buffer		refbuf = InvalidBuffer;
	Buffer		mapbuf;
	uint32		count = 0;
	BlockNumber old_block;
	uint64		end;
	xl_bmap_node_delta *ndelta;
	xl_bmap_entry_delta *ent;
	XLogRecPtr	lsn;
	MapPage		mp;
	bool		hole = false;
	bool		held = false;
	bool		let_go = false;
	bool		claimed;
	XLogRecPtr	horizon;
	XLogRecPtr	copy_lsn = InvalidXLogRecPtr;
	XLogRecPtr	passed = InvalidXLogRecPtr;

	/*
	 * A copy allocates, writes and logs, and may fail: never in a critical
	 * section, where a caller that locked the page there (a page of a map
	 * fork, say, locked only once its change began) would PANIC instead.
	 */
	Assert(CritSectionCount == 0);
	if (!fork_mapped(forknum))
		return;
	blkno = fork_key(forknum, blkno);

	/*
	 * The entry is looked up once, under the header held shared: the common
	 * case (an entry the map's own, or past the fork's end) ends there, and
	 * the others go on with what it found, the header held as they need.
	 */
	headerbuf = map_read(rel, 0);
	for (;;)
	{
		uint32		path_epoch;

		LockBuffer(headerbuf, BUFFER_LOCK_SHARE);
		check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
		epoch = PageGetBmapHeader(BufferGetPage(headerbuf))->epoch;
		end = key_fork_end(PageGetBmapHeader(BufferGetPage(headerbuf))->nblocks,
						   PageGetBmapHeader(BufferGetPage(headerbuf))->vm_nblocks,
						   blkno);
		if (!lookup_entry_path(reln, blkno, &entry, &mp, &path_epoch))
		{
			if (blkno >= end)
			{
				UnlockReleaseBuffer(headerbuf);
				return;
			}

			/*
			 * A hole: its leaf, the relation's own, made where there is
			 * none, under the header held exclusively, which this keeps
			 * from here on (the length can't change meanwhile).
			 */
			LockBuffer(headerbuf, BUFFER_LOCK_UNLOCK);
			LockBuffer(headerbuf, BUFFER_LOCK_EXCLUSIVE);
			end = key_fork_end(PageGetBmapHeader(BufferGetPage(headerbuf))->nblocks,
							   PageGetBmapHeader(BufferGetPage(headerbuf))->vm_nblocks,
							   blkno);
			if (lookup_entry_at(reln, blkno, &entry, &mp, NULL) ||
				blkno >= end)
			{
				/* (changed meanwhile: looked at again) */
				LockBuffer(headerbuf, BUFFER_LOCK_UNLOCK);
				continue;
			}
			(void) tree_leaf_for(reln, headerbuf, blkno, true, true, &mp);
			hole = true;
			break;
		}
		if (entry.epoch >= epoch)
		{
			UnlockReleaseBuffer(headerbuf);
			return;
		}
		/*
		 * (every pointer set in the map's epoch: the path is its own, as
		 * tree_path_own() would find, which assert-enabled builds ask, so
		 * that the two rules can't drift apart; the buffers it reads aren't
		 * counted as the change's, which tests measure)
		 */
		if (path_epoch >= epoch)
		{
#ifdef USE_ASSERT_CHECKING
			BufferUsage usage = pgBufferUsage;

			Assert(tree_path_own(reln, blkno, epoch, true));
			pgBufferUsage = usage;
#endif
			break;
		}
		if (tree_path_own(reln, blkno, epoch, true))
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
	old_block = hole ? InvalidBlockNumber : entry.block;
	horizon = copy_horizon();

	if (!hole && refs_has_page(rel, REF_PAGE(old_block)))
	{
		refbuf = refs_read(rel, REF_PAGE(old_block));
		LockBuffer(refbuf, BUFFER_LOCK_EXCLUSIVE);
		check_refs_page(BufferGetPage(refbuf), rel, REF_PAGE(old_block));
		count = page_ref(BufferGetPage(refbuf), old_block);
		copy_lsn = page_copy_lsn(BufferGetPage(refbuf));

		/*
		 * A block a copy made without an image may still read isn't
		 * claimed: the page is copied instead, and the block let go of
		 * (see "Copies without an image").
		 */
		held = page_copy_holds(BufferGetPage(refbuf), old_block, horizon,
							   &passed);
	}

	ndelta = palloc0(sizeof(xl_bmap_node_delta) + sizeof(xl_bmap_entry_delta));
	ndelta->kind = BMAP_DELTA_NODE;
	ndelta->nentries = 1;
	ent = (xl_bmap_entry_delta *) (ndelta + 1);
	ent->slot = MP_SLOT(&mp, blkno);
	ent->epoch = epoch;

	claimed = count == 0 && !hole && !held;
	if (claimed)
	{
		xl_bmap_unref xlrec;
		RefsPages	norefs;

		/* no other map holds it: claim it */
		copies_passed(passed);
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
		xl_bmap_ref_delta *ref;
		xl_bmap_remap xlrec;
		int			id;
		Page		page = BufferGetPage(buffer);
		bool		zeroed = pg_memory_is_all_zeros(page, BLCKSZ);
		bool		reused;
		bool		copied = false;
		bool		reset = false;

		/*
		 * This record restores the page before the caller's change.  If
		 * that change skips WAL, commit must log the final page contents;
		 * an fsync alone would let redo overwrite them with this image.
		 */
		RelFileLocatorForceWALAtCommit(rel);

		/*
		 * The old block gets what other maps should read, if only buffered.
		 * (A hole has none, and its page can't be dirty: it was never
		 * prepared.)
		 */
		Assert(!hole || !BufferIsDirty(buffer));

		/*
		 * The page is copied without its image when one is needed no more
		 * than a full-page write would log one: the page's image or a
		 * change since the redo pointer is in the WAL, which replay rebuilds
		 * the page from (or skips, the map on disk newer, the old block
		 * holding the page as the copy saw it).  Not if the old block is
		 * written now (its write isn't logged), nor while a base backup
		 * runs (which may copy the old block after it changes, and replays
		 * from before the copy), nor for a relation whose data skips WAL,
		 * nor without full-page writes (which log no image to make this
		 * one redundant).  A backup begun meanwhile begins at a checkpoint
		 * begun after this, which makes XLogInsert() add the image after
		 * all.  Nor for a visibility map page: a bit cleared with a heap
		 * change is replayed with that change, which neither registers the
		 * map page nor sets its LSN, so its LSN says nothing of whether its
		 * changes since the redo pointer are all in the WAL, nor of whether
		 * replay skipped some of them (redo_remap_page()).  Its pages are
		 * few, one for many thousands of the relation's.  Nor with
		 * block_map_copy_images on (the default), which keeps every recovery
		 * from needing the old block for a copy: a standby's backup across
		 * copies and pg_rewind after them then work as they do for any
		 * relation (a share's synced blocks still limit both: bmapshare()).
		 */
		if (!block_map_copy_images && !hole && !zeroed && BufferIsValid(refbuf) &&
			forknum == MAIN_FORKNUM &&
			!BufferIsDirty(buffer) && !RelFileLocatorSkippingWAL(rel))
		{
			XLogRecPtr	redo;
			bool		page_writes;
			int			backups;
			XLogRecPtr	backup_start;

			GetFullPageWriteInfo(&redo, &page_writes);
			backup_state(&backups, &backup_start);
			copied = page_writes && backups == 0 &&
				!XLogCheckBufferNeedsBackup(buffer);
		}

		/*
		 * The copied bits told of copies all past the restart point: cleared
		 * (once standbys know: copies_passed())
		 */
		if (copied && !XLogRecPtrIsInvalid(copy_lsn) && copy_lsn <= horizon)
		{
			reset = true;
			copies_passed(copy_lsn);
		}
		if (!hole && BufferIsDirty(buffer))
			FlushOneBuffer(buffer);

		alloc_take(rel, 1, &new_block, &alloc);
		ent->block = new_block;
		reused = new_block < alloc.old_hw;

		/*
		 * The page store reaches the block, grown ahead for the copies to
		 * come (prepare_store()).  The page's contents come with the record,
		 * as an image, or redo takes them from the page as replay has it or
		 * from the old block (redo_remap_page()), so the block is written
		 * only from the page's buffer, later, and a block the store has is
		 * left as it is, as redo leaves it; a node's stale buffer for it is
		 * dropped now, here and in redo.  A page of zeroes (one being
		 * initialized) is zeroed in the new block instead, here and in redo.
		 */
		if (reused && !zeroed)
			drop_node_buffer(rel, new_block);
		prepare_store(rel, alloc.old_hw, alloc.new_hw,
					  &new_block, zeroed && reused ? 1 : 0, !zeroed,
					  zeroed ? 0 : BMAP_STORE_AHEAD);

		/* the old block, a holder fewer (a hole has none) */
		if (!hole)
		{
			rdelta = palloc0(sizeof(xl_bmap_refs_delta) + sizeof(xl_bmap_ref_delta));
			rdelta->kind = BMAP_DELTA_REFS;
			rdelta->nrefs = 1;
			ref = (xl_bmap_ref_delta *) (rdelta + 1);
			ref->slot = REF_SLOT(old_block);

			/*
			 * (a block a copy holds, with no other holder: let go of, which
			 * queues it, below)
			 */
			ref->count = count > 0 ? count - 1 : 0;
			let_go = count == 0;
			if (copied)
				rdelta->flags |= BMAP_DELTA_COPIED;
			if (reset)
				rdelta->flags |= BMAP_DELTA_COPIED_RESET;
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
		xlrec.flags = zeroed ? BMAP_REMAP_ZERO : 0;
		if (copied)
			xlrec.flags |= BMAP_REMAP_COPIED;

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
		if (zeroed)
			XLogRegisterBuffer(id, buffer, REGBUF_WILL_INIT);
		else
			XLogRegisterBuffer(id, buffer, (copied ? 0 : REGBUF_FORCE_IMAGE) |
							   (page_hole_is_zero(page) ? REGBUF_STANDARD : 0));
		lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_REMAP);
		PageSetLSN(BufferGetPage(mapbuf), lsn);
		alloc_set_lsn(&alloc, lsn);
		if (rdelta)
			PageSetLSN(BufferGetPage(refbuf), lsn);
		/* (set once inserted, as redo sets it: the image is taken before) */
		if (copied)
			page_set_copy_lsn(BufferGetPage(refbuf), lsn);
		/* a page of zeroes stays one: a new page has no LSN */
		if (!zeroed)
			PageSetLSN(page, lsn);
		END_CRIT_SECTION();

		alloc_release(&alloc);
		if (rdelta)
			pfree(rdelta);
	}

	map_unlock(mapbuf);
	ReleaseBuffer(mapbuf);
	if (BufferIsValid(refbuf))
		UnlockReleaseBuffer(refbuf);

	/*
	 * The old block, held by nothing now, is freed, which queues it while a
	 * copy may read it (a crash before leaves it to a sweep).
	 */
	if (let_go)
		(void) free_blocks(rel, &old_block, 1);
	UnlockReleaseBuffer(headerbuf);
	pfree(ndelta);

	/* (a hole given its first block copied nothing, and claimed nothing) */
	if (!hole)
		pgstat_count_block_map(rel.spcOid, rel.dbOid, rel.relNumber,
							   claimed ? PGSTAT_BLOCK_MAP_PAGE_CLAIMED :
							   PGSTAT_BLOCK_MAP_PAGE_COPIED);
}

/*
 * Set the length of a relation's fork 'forknum' (its main fork or its
 * visibility map); the header is locked exclusively.
 */
static void
set_length(RelFileLocator rel, Buffer headerbuf, ForkNumber forknum,
		   BlockNumber nblocks)
{
	xl_bmap_header_delta delta;
	XLogRecPtr	lsn;

	header_delta_of(BufferGetPage(headerbuf), &delta);
	if (forknum == VISIBILITYMAP_FORKNUM)
		delta.vm_nblocks = nblocks;
	else
		delta.nblocks = nblocks;
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
}

/*
 * Walking a tree: every node reached from the root page told to
 * visit_node(), whose children (if it says to go on) are visited too, and
 * every entry of a leaf so visited told to visit_entry(), with its logical
 * block (InvalidBlockNumber under a held subtree, which maps no block
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
 * root page's pointers, so letting go of a relation's storage costs its
 * root page, not its size.  A drain pops entries from the top, a step
 * at a time (bmap_release_step()), and lets go of each: one holder less,
 * or, if nothing else held the block, the block freed, a node's children
 * pushed in its place.  A node another map still
 * holds is only counted down; its subtree isn't read.
 *
 * An entry may carry a pointer the roots' counts count, and a node they
 * reach may push entries that take its pointers off them, below which its
 * hold stays (see "The roots' counts"): a step changes the roots' counts
 * of the blocks it pops as it changes their reference counts.
 *
 * A step is one record: the entries popped, the counts they change, the
 * blocks they free and the children they push.  A crash replays it whole
 * or not at all, so no entry is let go of twice and none is lost; what the
 * queue holds counts as held after a crash as before it.  Steps are
 * serialized per store by a lock (DRAIN_LOCK), and held off while a sweep
 * counts holders, which counts the queue's entries as holders too
 * (sweep_queue()).
 *
 * Drains are run by callers of bmap_release_step(), by a sweep before it
 * counts, and before a VACUUM of a relation of the store
 * (bmapreleasestep()): a page a dropped map still holds counts as shared,
 * which VACUUM would otherwise leave as it is or copy to change.  An
 * extension that would grow the store drains first (bmapbeforeextend()).
 * Autovacuum drains each database's queues in the background, after its
 * tables, a bounded number of entries a visit (bmapmaintain()).
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

/*
 * Entries a step passes over at most, held by copies without an image
 * (release_step()), and writes back under what it pushes.
 */
#define BMAP_RELEASE_HELD		(3 * BMAP_QUEUE_PER_PAGE)

StaticAssertDecl(BMAP_RELEASE_REFS_PAGES <= BMAP_REFS_PER_RECORD,
				 "a step's count pages are locked at once");
StaticAssertDecl(1 + BMAP_QUEUE_PAGES_PER_RECORD + BMAP_RELEASE_REFS_PAGES + 1 +
				 BMAP_RELEASE_BITS_PAGES <= XLR_MAX_BLOCK_ID + 1,
				 "a step's pages fit one record");
StaticAssertDecl((BMAP_RELEASE_HELD + BMAP_TREE_FANOUT + 1 + BMAP_QUEUE_PER_PAGE - 1) /
				 BMAP_QUEUE_PER_PAGE + 1 <= BMAP_QUEUE_PAGES_PER_RECORD,
				 "the entries a step writes back and pushes fit one record's queue pages");
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
	q.level = (uint8) level;
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

	/*
	 * (a test takes a committed owner for one too old to say of, as one
	 * past the commit log's truncation would be)
	 */
	if (state == OWNER_COMMITTED &&
		IS_INJECTION_POINT_ATTACHED("bmap-owner-unknown"))
		state = OWNER_UNKNOWN;
	return state;
}

static inline bool
queued_holds(OwnerState state)
{
	return state == OWNER_NONE || state == OWNER_COMMITTED;
}

/* the subtrees 'tops' points to (a root page's, or a captured root's), as entries */
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
 * before the commit record (bmapprecommitdrop()): its drop then queues
 * nothing more.
 */
typedef struct PreQueuedDrop
{
	RelFileLocator rel;
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

/*
 * Queue what a map holds (root's subtrees), under its header's lock, the
 * entries naming 'owner' if valid, in one record.  Returns the end of the
 * record, if any.
 */
static XLogRecPtr
queue_map(SMgrRelation reln, const BmapRoot *root, FullTransactionId owner)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	BmapQueued	items[BMAP_TREE_TOPS];
	uint32		n;
	XLogRecPtr	end;

	n = queued_tops(root->top, items);
	StaticAssertStmt(BMAP_TREE_TOPS <=
					 (BMAP_QUEUE_PAGES_PER_RECORD - 1) * BMAP_QUEUE_PER_PAGE,
					 "a map's subtrees are queued in one record");
	for (uint32 i = 0; i < n && FullTransactionIdIsValid(owner); i++)
		queued_set_owner(&items[i], owner);
	end = queue_push(rel, items, n);
	return end;
}

/*
 * drop_blocks(): the map's subtrees queued, to be let go of in the
 * background.  The map stays until
 * the relation is unlinked, and a sweep reads it until then (see
 * sweep_relation()), but once queued the nodes it points to may be freed,
 * and made something else: so its root page is written out with no
 * subtrees, under the header's lock (which a sweep reads it under), and
 * with no record, the relation's drop being logged already (redo removes
 * the map).  The queue's records come first, under the same lock: a sweep
 * that reads the map with its subtrees gone reads the queue after, with
 * them.  A relation dropped at a commit had them queued before the commit
 * record, as the transaction's (bmapprecommitdrop()): then nothing more
 * is queued.
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
		XLogRecPtr	end = done == NULL ?
			queue_map(reln, &root, InvalidFullTransactionId) : InvalidXLogRecPtr;

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
 * committed.  Nothing for a transaction with no id, which wrote no
 * catalog.
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
		(void) queue_map(reln, &root, top);
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
 * Let go of every physical block a dropped relation's map holds: queued, for a
 * drain to free those no other map holds and count the others down.  The
 * records name only the queue's pages: a standby has removed the relation's
 * map by the time it replays them.  Runs after commit or abort, so it must not
 * fail: a problem is reported and the blocks are left allocated, for a sweep.
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

static bool roots_exist(RelFileLocator rel);
static void roots_meta_change(RelFileLocator rel, uint16 flags, uint32 naming,
							  bool counted);

/*
 * A block popped, how many of the entries popped hold it, and how many of
 * them its roots' count counted (BMAP_QUEUED_COUNTED, BMAP_QUEUED_UNCOUNT)
 */
typedef struct Popped
{
	BlockNumber block;
	uint32		holders;
	uint32		uncounts;
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
 *
 * An entry of a block that a copy made without an image may still read
 * (see "Copies without an image") is passed over, not let go of: the step
 * writes it back below what it pushes, in place of the entries it popped
 * (the same record).  Such entries are pushed on top as copies free blocks,
 * the newest of the queue, and the copies they wait for pass only at the
 * next checkpoint; stopping at them would keep every entry under them,
 * dropped relations that have nothing to do with any copy, until the
 * copies stopped.  Passed over, they sink beneath what the step pops, and
 * a later drain past the checkpoint frees them.  The order of the other
 * entries stays as it was.  An entry that takes a count off a node's (its
 * flags say it counted) stays above the node's entry, which keeps the
 * node's hold until what is pushed over it is done, so the step stops at
 * a node once it passed over one of those.  At most BMAP_RELEASE_HELD are
 * passed over; more on top of the queue keep it waiting for the next
 * checkpoint, as they would all be freed by then.
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
	BmapQueued *held = NULL;
	uint32		nheld = 0;
	bool		held_counts = false;
	uint32		nlet = 0;
	bool		uncertain = false;
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
	XLogRecPtr	horizon;
	XLogRecPtr	passed = InvalidXLogRecPtr;

	metabuf = queue_lock_meta(rel, &n, &isnew);
	if (n == 0)
	{
		UnlockReleaseBuffer(metabuf);
		return false;
	}
	horizon = copy_horizon();

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

		while (k + ndiscard < BMAP_RELEASE_STEP && k + ndiscard + nheld < n)
		{
			uint32		pos = n - 1 - k - ndiscard - nheld;
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
				/*
				 * (one too old to say of may have been let go of by a
				 * commit: what it counted can't be taken off for sure)
				 */
				if (state == OWNER_UNKNOWN && (e.flags & BMAP_QUEUED_COUNTED))
					uncertain = true;
				ndiscard++;
				continue;
			}

			/*
			 * A block a copy made without an image may still read stays
			 * held until the copy is past the restart point: the step
			 * passes over it, to write it back below (asked again below,
			 * under the count's lock, for the blocks the step frees).
			 */
			if (e.level == BMAP_QUEUED_BLOCK && lookup_copy_holds(rel, e.block))
			{
				if (nheld == BMAP_RELEASE_HELD)
					break;
				if (held == NULL)
					held = palloc_array(BmapQueued, BMAP_RELEASE_HELD);
				held[nheld++] = e;
				if (e.flags & (BMAP_QUEUED_COUNTED | BMAP_QUEUED_UNCOUNT))
					held_counts = true;
				continue;
			}
			if (e.level != BMAP_QUEUED_BLOCK && held_counts)
				break;
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
	if (k + ndiscard == 0)
	{
		if (held)
			pfree(held);
		UnlockReleaseBuffer(metabuf);
		return false;
	}
	npop = k + ndiscard + nheld;

	/* the roots' counts said not to be whole first, for a sweep to count */
	if (uncertain && roots_exist(rel))
		roots_meta_change(rel, BMAP_DELTA_COUNTED, 0, false);

	/* a node's children, read first (an entry not naming its node: none) */
	nlet = k;
	if (node != NULL)
	{
		/* (and room for an entry that keeps the node's hold) */
		children = palloc_array(BmapQueued, BMAP_TREE_FANOUT + 1);
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
		blocks[i].holders = (popped[i].flags & BMAP_QUEUED_UNCOUNT) ? 0 : 1;
		blocks[i].uncounts =
			(popped[i].flags & (BMAP_QUEUED_COUNTED | BMAP_QUEUED_UNCOUNT)) ? 1 : 0;
	}
	qsort(blocks, nlet, sizeof(Popped), cmp_popped);
	for (uint32 i = 0; i < nlet; i++)
	{
		if (nblocks > 0 && blocks[nblocks - 1].block == blocks[i].block)
		{
			blocks[nblocks - 1].holders += blocks[i].holders;
			blocks[nblocks - 1].uncounts += blocks[i].uncounts;
		}
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
		uint32		c = REF_PAGE(b) < nrefpages ? refs_get(&rp, b) : 0;
		uint32		rv = REF_PAGE(b) < nrefpages ? refs_get_root(&rp, b) : 0;
		uint32		rc = ROOTREF_COUNT(rv);
		bool		is_node = node != NULL && b == node->block;
		bool		frees = false;
		bool		leaves = false;

		/*
		 * Its roots' count, those of the entries it counted fewer.  A node
		 * that the roots no longer reach leaves what they hold (see "The
		 * roots' counts").
		 */
		rc = rc >= blocks[i].uncounts ? rc - blocks[i].uncounts : 0;
		if (is_node && (rv & ROOTREF_IN) && rc == 0 && blocks[i].uncounts > 0)
			leaves = true;

		if (h > 0 && c >= h)
		{
			/* (a node leaving keeps its entry's hold: see below) */
			uint32		let = leaves ? h - 1 : h;

			if (let > 0)
			{
				refs_set(&rp, b, c - let);
				unrefs++;
			}
		}
		else if (h > 0)
		{
			/* (the entries were all the holders it had) */
			Assert(c + 1 == h);
			if (c > 0)
				refs_set(&rp, b, 0);
			tofree[nfree++] = b;
			frees = true;

			/*
			 * (a block copied from since it was looked at above: the step
			 * is taken again, which ends before it; a node is never copied
			 * from as a page, its bits a block's before it, long passed)
			 */
			if (!is_node && REF_PAGE(b) < nrefpages &&
				page_copy_holds(BufferGetPage(rp.buf[refs_find(&rp, b)]), b,
								horizon, &passed))
			{
				refs_release(&rp);
				if (children)
					pfree(children);
				if (held)
					pfree(held);
				UnlockReleaseBuffer(metabuf);
				return release_step(rel, done);
			}
		}

		/*
		 * A node freed lets go of its children, counted ones if the roots
		 * reached it.  One that the roots no longer reach but that another
		 * map still holds keeps its children: the counts of its pointers
		 * are to be taken off, from entries pushed over one that keeps
		 * its hold (as an entry of a counted root's did), so that nothing
		 * under it changes before they are (a change under a held node
		 * copies it first).  An entry that held nothing, under such a
		 * node, needs no hold of its own: the hold is the one below.
		 */
		if (is_node && frees)
		{
			for (int j = 0; j < nchildren; j++)
				if (rv & ROOTREF_IN)
					children[j].flags |= BMAP_QUEUED_COUNTED;
			npushed = (uint32) nchildren;
		}
		else if (leaves)
		{
			bool		hold = (node->flags & BMAP_QUEUED_UNCOUNT) == 0;

			if (hold)
			{
				memmove(children + 1, children, nchildren * sizeof(BmapQueued));
				children[0] = queued(node->block, node->level, node->first,
									 node->since);
			}
			for (int j = hold ? 1 : 0; j < nchildren + (hold ? 1 : 0); j++)
				children[j].flags |= BMAP_QUEUED_UNCOUNT;
			npushed = (uint32) nchildren + (hold ? 1 : 0);
		}
		if (REF_PAGE(b) < nrefpages)
		{
			/* (a block freed keeps no roots' count, whatever it said) */
			uint32		nv = frees ? 0 :
				(rc | ((rv & ROOTREF_IN) && !leaves ? ROOTREF_IN : 0));

			if (nv != rv)
				refs_set_root(&rp, b, nv);
		}
	}

	/*
	 * (before the allocator's metapage is locked, which every allocation of
	 * the database waits for: its note in pg_control is synced)
	 */
	copies_passed(passed);

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

	/*
	 * The entries passed over, and a node's children over them, in the
	 * place of the entries popped
	 */
	qp.n = 0;
	if (nheld > 0 && npushed > 0)
	{
		held = repalloc_array(held, BmapQueued, nheld + npushed);
		memcpy(held + nheld, children, npushed * sizeof(BmapQueued));
	}
	else if (npushed > 0)
		held = children;
	npushed += nheld;
	if (npushed > 0)
	{
		queue_cover(rel, QUEUE_PAGE(n - npop + npushed - 1) + 1);
		queue_write(rel, &qp, n - npop, held, npushed);
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
	if (held && held != children)
		pfree(held);
	if (children)
		pfree(children);

	/* (what a scan found in them no longer holds) */
	for (uint32 i = 0; i < nfree; i++)
		store_written(rel, tofree[i], 1);

	done->entries += npop - nheld;
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
 * Every entry of rel's store's queue, read under its metapage's lock, into
 * a palloc'd array; how many in *n.
 */
static BmapQueued *
queue_entries(RelFileLocator rel, uint32 *n)
{
	Buffer		metabuf;
	BmapQueued *entries;

	*n = 0;
	if (!queue_exists(rel))
		return palloc(sizeof(BmapQueued));
	metabuf = queue_read(rel, 0);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	if (!PageIsNew(BufferGetPage(metabuf)))
	{
		check_page(BufferGetPage(metabuf), BMAP_PAGE_QUEUE_META,
				   with_relnumber(rel, BMAP_QUEUE_RELNUMBER), MAIN_FORKNUM, 0);
		*n = PageGetBmapQueueMeta(BufferGetPage(metabuf))->nentries;
	}
	entries = MemoryContextAllocHuge(CurrentMemoryContext,
									 Max(*n, 1) * sizeof(BmapQueued));
	for (uint32 i = 0; i < *n;)
	{
		Buffer		buf = queue_read(rel, QUEUE_PAGE(i));
		uint32		take = Min(*n - i, BMAP_QUEUE_PER_PAGE - QUEUE_SLOT(i));

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
	return entries;
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
 *
 * The roots' counts, which the sweep begins from, count already what an
 * entry pointing to a block they count holds (BMAP_QUEUED_COUNTED), and
 * one more for each pointer still to be taken off them
 * (BMAP_QUEUED_UNCOUNT), which holds nothing: 'visit_uncount' takes it
 * off.
 */
static void
sweep_queue(RelFileLocator rel, TreeNodeVisit visit_node,
			TreeEntryVisit visit_entry, TreeEntryVisit visit_uncount,
			void *arg)
{
	uint32		n;
	BmapQueued *entries = queue_entries(rel, &n);
	SMgrRelation store = store_smgr(rel);
	FullTransactionId last_owner = InvalidFullTransactionId;
	OwnerState	last_state = OWNER_NONE;

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
		if (entries[i].flags & BMAP_QUEUED_UNCOUNT)
			visit_uncount(&ptr, InvalidBlockNumber, arg);
		else if (entries[i].flags & BMAP_QUEUED_COUNTED)
			continue;
		else if (entries[i].level == BMAP_QUEUED_BLOCK)
			visit_entry(&ptr, InvalidBlockNumber, arg);
		else
			tree_walk_node(store, &ptr, entries[i].level, entries[i].first,
						   true, visit_node, visit_entry, arg);
	}
	pfree(entries);
}

/* ----------------------------------------------------------------
 * Captured roots
 * ----------------------------------------------------------------
 */

/*
 * A page store's captured roots (BMAP_ROOTS_RELNUMBER): block 0 is a
 * metapage (BmapRootsMeta), and blocks 1 on hold slots of roots
 * (xl_bmap_root), BMAP_ROOTS_PER_PAGE a page.  A root holds its subtrees
 * as a page pointing to them does, but the store keeps it, not the
 * relation's root page: there may be any number of them, and dropping the relation,
 * or giving it new storage, leaves them as they are.  A root's id is its
 * slot and a sequence number the metapage hands out, so a slot reused
 * names another root.
 *
 * Holding is transactional both ways.  A slot names the (sub)transaction
 * that captured it: until that commits the root holds nothing, and if it
 * doesn't, the slot is reclaimed (its subtrees let go of).  Letting go of
 * a root names the (sub)transaction that does so in the slot and pushes
 * its subtrees onto the release queue as entries naming that transaction
 * too (carrying its pointers' roots' counts, if a sweep counted it): if
 * it commits, the queue lets go of them and the slot is free; if it
 * doesn't, the entries are popped with nothing let go of and the slot
 * holds as before.  So a root's holders change with the commit that stops
 * naming it, whatever crashes in between.
 *
 * The metapage marks each page of slots that may have a slot naming a
 * transaction (a capturer or a releaser, not yet settled), set in the
 * record that names it: so settling and reclaiming (bmap_roots_reclaim())
 * read only the pages marked, as many as the roots captured or let go of
 * since they last ran, not every page of slots.  A page is unmarked once
 * its slots name none, under the metapage's lock and the page's.  A page
 * marked so is marked too as one that may hold a root the roots' counts
 * don't count yet (see "The roots' counts"), until a sweep has counted
 * every root on it: so a sweep reads the pages of the roots captured
 * since the last, not every page of slots.
 *
 * Locks: the metapage exclusively while a slot is chosen and set (a
 * capture holds its relation's header, then the metapage, then the slot's
 * page, then the reference-count pages), or let go of (the metapage, then
 * the slot's page); a slot's page alone to settle it.  Nothing holding a
 * roots page waits for a map's header.
 */
typedef struct BmapRootsMeta
{
	uint32		version;
	uint32		npages;			/* pages of slots after the metapage */
	uint32		hint;			/* the first that may have a free slot */
	uint32		counted;		/* are the roots' counts whole */
	uint64		seq;			/* the last root's sequence number */
	uint8		naming[FLEXIBLE_ARRAY_MEMBER];	/* a bit a page of slots:
												 * may it name a transaction;
												 * then as many: may it hold
												 * a root not counted */
} BmapRootsMeta;

/* the pages of slots a metapage's marks cover, at most */
#define BMAP_ROOTS_NAMING_PAGES \
	((uint32) ((BMAP_CONTENT_SIZE - offsetof(BmapRootsMeta, naming)) / 2 * 8))
#define PageGetBmapRootsUncounted(page) \
	(PageGetBmapRootsMeta(page)->naming + BMAP_ROOTS_NAMING_PAGES / 8)

#define BMAP_ROOTS_PER_PAGE \
	((uint32) (BMAP_CONTENT_SIZE / sizeof(xl_bmap_root)))
#define PageGetBmapRootsMeta(page)	((BmapRootsMeta *) PageGetContents(page))
#define PageGetBmapRoots(page)	((xl_bmap_root *) PageGetContents(page))

/* a root's id: its slot in the high bits, its sequence number below */
#define ROOT_SEQ_BITS		40
#define ROOT_ID(slot, seq) \
	(((uint64) (slot) << ROOT_SEQ_BITS) | ((seq) & ((UINT64CONST(1) << ROOT_SEQ_BITS) - 1)))
#define ROOT_SLOT(id)		((uint32) ((id) >> ROOT_SEQ_BITS))
#define ROOT_PAGE(slot)		((BlockNumber) (1 + (slot) / BMAP_ROOTS_PER_PAGE))
#define ROOT_INDEX(slot)	((uint32) ((slot) % BMAP_ROOTS_PER_PAGE))

static bool root_read_under(RelFileLocator store, BmapRootId id, int mode,
							Buffer *buf, xl_bmap_root *r, Buffer heldmeta);

static SMgrRelation
roots_smgr(RelFileLocator rel)
{
	return smgropen(with_relnumber(rel, BMAP_ROOTS_RELNUMBER),
					INVALID_PROC_NUMBER);
}

static Buffer
roots_read(RelFileLocator rel, BlockNumber blkno)
{
	check_may_read();
	return ReadBufferWithoutRelcache(with_relnumber(rel, BMAP_ROOTS_RELNUMBER),
									 MAIN_FORKNUM, blkno, RBM_NORMAL, NULL,
									 true);
}

/* does rel's store keep roots (a file with its metapage, at least)? */
static bool
roots_exist(RelFileLocator rel)
{
	SMgrRelation rsrel = roots_smgr(rel);

	return smgrexists(rsrel, MAIN_FORKNUM) && smgrnblocks(rsrel, MAIN_FORKNUM) > 0;
}

/*
 * Give rel's store its roots file, as ensure_queue() its queue: the file is
 * logged as made, its pages initialized by the records that first set
 * them, all zeroes until then.
 */
static void
ensure_roots(RelFileLocator rel)
{
	SMgrRelation rsrel = roots_smgr(rel);
	LOCKTAG		tag;

	if (roots_exist(rel))
		return;
	SET_LOCKTAG_RELATION(tag, rel.dbOid, BMAP_ROOTS_RELNUMBER);
	(void) LockAcquire(&tag, AccessExclusiveLock, false, false);
	if (!smgrexists(rsrel, MAIN_FORKNUM))
	{
		RelFileLocator rloc = with_relnumber(rel, BMAP_ROOTS_RELNUMBER);

		smgrcreate(rsrel, MAIN_FORKNUM, false);
		log_smgrcreate(&rloc, MAIN_FORKNUM);
	}
	check_may_read();
	if (smgrnblocks(rsrel, MAIN_FORKNUM) == 0)
		ReleaseBuffer(ExtendBufferedRelTo(BMR_SMGR(rsrel, RELPERSISTENCE_PERMANENT),
										  MAIN_FORKNUM, NULL,
										  EB_SKIP_EXTENSION_LOCK, 1,
										  RBM_NORMAL));
	LockRelease(&tag, AccessExclusiveLock, false);
}

/* the roots file has 'need' pages; under the metapage's exclusive lock */
static void
roots_cover(RelFileLocator rel, BlockNumber need)
{
	SMgrRelation rsrel = roots_smgr(rel);

	check_may_read();
	if (smgrnblocks(rsrel, MAIN_FORKNUM) < need)
		ReleaseBuffer(ExtendBufferedRelTo(BMR_SMGR(rsrel, RELPERSISTENCE_PERMANENT),
										  MAIN_FORKNUM, NULL,
										  EB_SKIP_EXTENSION_LOCK, need,
										  RBM_NORMAL));
}

static void
apply_roots_meta_delta(Page page, const xl_bmap_roots_meta_delta *delta)
{
	BmapRootsMeta *meta;

	if (delta->flags & BMAP_DELTA_INIT)
	{
		init_page(page, BMAP_PAGE_ROOTS_META);
		PageGetBmapRootsMeta(page)->version = BMAP_VERSION;
	}
	meta = PageGetBmapRootsMeta(page);
	meta->npages = delta->npages;
	meta->hint = delta->hint;
	meta->seq = delta->seq;
	if (delta->flags & BMAP_DELTA_COUNTED)
		meta->counted = delta->counted;
	if (delta->naming != 0)
	{
		uint32		p = delta->naming - 1;
		uint8	   *uncounted = PageGetBmapRootsUncounted(page);

		Assert(p < BMAP_ROOTS_NAMING_PAGES);
		if (delta->flags & BMAP_DELTA_NAMING_CLEAR)
			meta->naming[p / 8] &= ~(1 << (p % 8));
		else if (delta->flags & BMAP_DELTA_UNCOUNTED_CLEAR)
			uncounted[p / 8] &= ~(1 << (p % 8));
		else
		{
			/* (a slot to name a transaction is a root captured, or let go of) */
			meta->naming[p / 8] |= 1 << (p % 8);
			uncounted[p / 8] |= 1 << (p % 8);
		}
	}
}

/* is page 'p' of slots (0-based) marked as one that may name a transaction? */
static inline bool
roots_page_naming(Page metapage, uint32 p)
{
	return (PageGetBmapRootsMeta(metapage)->naming[p / 8] & (1 << (p % 8))) != 0;
}

/* does slot *r name a transaction, a capturer or a releaser? */
static inline bool
root_names_xact(const xl_bmap_root *r)
{
	return r->snap.id != 0 && (r->snap.owner != 0 || r->released_by != 0);
}

static void
apply_roots_delta(Page page, const xl_bmap_roots_delta *delta)
{
	if (delta->flags & BMAP_DELTA_INIT)
		init_page(page, BMAP_PAGE_ROOTS);
	Assert(delta->index < BMAP_ROOTS_PER_PAGE);
	PageGetBmapRoots(page)[delta->index] = delta->root;
}

/* the metapage locked exclusively, its contents (zeroes if new) into *meta */
static Buffer
roots_lock_meta(RelFileLocator rel, BmapRootsMeta *meta, bool *isnew)
{
	Buffer		buf = roots_read(rel, 0);
	Page		page = BufferGetPage(buf);

	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	*isnew = PageIsNew(page);
	if (*isnew)
		memset(meta, 0, sizeof(*meta));
	else
	{
		check_page(page, BMAP_PAGE_ROOTS_META,
				   with_relnumber(rel, BMAP_ROOTS_RELNUMBER), MAIN_FORKNUM, 0);
		*meta = *PageGetBmapRootsMeta(page);
		if (meta->version != BMAP_VERSION)
			refuse_version(meta->version,
						   with_relnumber(rel, BMAP_ROOTS_RELNUMBER), MAIN_FORKNUM);
	}
	return buf;
}

/*
 * What a slot holds by its capturer and its releaser (owner_state()).
 * ROOT_FREE: nothing (no root, or one let go of by a committed releaser,
 * or one too old to say of, taken as let go of, as the queue takes its
 * entries); ROOT_HELD: its subtrees; ROOT_PENDING: nothing yet, or
 * nothing any more once a releaser commits, either way its slot not to be
 * reused; ROOT_ABANDONED: its capturer ended without committing, so its
 * subtrees' holders it gained are to be let go of.
 */
typedef enum RootState
{
	ROOT_FREE,
	ROOT_HELD,
	ROOT_PENDING,
	ROOT_ABANDONED,
} RootState;

static RootState
root_state(const xl_bmap_root *r)
{
	OwnerState	state;

	if (r->snap.id == 0)
		return ROOT_FREE;
	/* (in recovery no transaction is judged: one let go of is taken as so) */
	if (RecoveryInProgress())
		return r->released_by != 0 ? ROOT_FREE : ROOT_HELD;
	state = owner_state(FullTransactionIdFromU64(r->snap.owner));
	if (state == OWNER_ENDED)
		return ROOT_ABANDONED;
	if (state == OWNER_RUNNING)
		return ROOT_PENDING;
	if (r->released_by == 0)
		return ROOT_HELD;
	state = owner_state(FullTransactionIdFromU64(r->released_by));
	if (state == OWNER_RUNNING)
		return ROOT_PENDING;
	if (state == OWNER_ENDED)
		return ROOT_HELD;
	return ROOT_FREE;
}

/*
 * Change a root's slot (its page held exclusively in 'buf') to *r: one
 * record of the page alone, or, with 'metabuf' (the metapage, held
 * exclusively, locked before the slot's page as a capture locks it), of the
 * metapage too if the slot's page is to be marked as naming a transaction.
 */
static void
roots_set_slot(RelFileLocator rel, Buffer buf, uint32 slot,
			   const xl_bmap_root *r, Buffer metabuf)
{
	xl_bmap_roots_delta delta;
	xl_bmap_roots_meta_delta mdelta;
	bool		mark = false;
	xl_bmap_tree xlrec;
	XLogRecPtr	lsn;

	if (BufferIsValid(metabuf) && root_names_xact(r) &&
		!roots_page_naming(BufferGetPage(metabuf), ROOT_PAGE(slot) - 1))
	{
		BmapRootsMeta *meta = PageGetBmapRootsMeta(BufferGetPage(metabuf));

		memset(&mdelta, 0, sizeof(mdelta));
		mdelta.kind = BMAP_DELTA_ROOTS_META;
		mdelta.npages = meta->npages;
		mdelta.hint = meta->hint;
		mdelta.seq = meta->seq;
		mdelta.naming = ROOT_PAGE(slot);
		mark = true;
	}

	memset(&delta, 0, sizeof(delta));
	delta.kind = BMAP_DELTA_ROOTS;
	delta.index = ROOT_INDEX(slot);
	delta.root = *r;
	xlrec.locator = with_relnumber(rel, BMAP_ROOTS_RELNUMBER);
	xlrec.op = BMAP_TREE_ROOT_SLOT;
	xlrec.flags = 0;
	xlrec.node = xlrec.from = InvalidBlockNumber;
	xlrec.old_hw = xlrec.new_hw = InvalidBlockNumber;

	START_CRIT_SECTION();
	apply_roots_delta(BufferGetPage(buf), &delta);
	MarkBufferDirty(buf);
	if (mark)
	{
		apply_roots_meta_delta(BufferGetPage(metabuf), &mdelta);
		MarkBufferDirty(metabuf);
	}
	XLogBeginInsert();
	XLogRegisterData(&xlrec, sizeof(xlrec));
	XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
	XLogRegisterBufData(0, &delta, sizeof(delta));
	if (mark)
	{
		XLogRegisterBuffer(1, metabuf, REGBUF_STANDARD);
		XLogRegisterBufData(1, &mdelta, sizeof(mdelta));
	}
	lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_TREE);
	PageSetLSN(BufferGetPage(buf), lsn);
	if (mark)
		PageSetLSN(BufferGetPage(metabuf), lsn);
	END_CRIT_SECTION();
}

/*
 * Unmark page 'p' of slots (0-based) if none of its slots names a
 * transaction: the metapage, then the page, locked, one record of the
 * metapage.  A slot comes to name one only under the metapage's lock
 * (roots_set_slot()'s, a capture's or a copy's), so none can meanwhile.
 */
static void
roots_unmark_page(RelFileLocator rel, uint32 p)
{
	Buffer		metabuf = roots_read(rel, 0);
	Buffer		buf;
	bool		names = false;

	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	if (!roots_page_naming(BufferGetPage(metabuf), p))
	{
		UnlockReleaseBuffer(metabuf);
		return;
	}
	buf = roots_read(rel, 1 + p);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	for (uint32 i = 0; i < BMAP_ROOTS_PER_PAGE && !names; i++)
		names = root_names_xact(&PageGetBmapRoots(BufferGetPage(buf))[i]);
	UnlockReleaseBuffer(buf);
	if (!names)
	{
		BmapRootsMeta *meta = PageGetBmapRootsMeta(BufferGetPage(metabuf));
		xl_bmap_roots_meta_delta mdelta;
		xl_bmap_tree xlrec;
		XLogRecPtr	lsn;

		memset(&mdelta, 0, sizeof(mdelta));
		mdelta.kind = BMAP_DELTA_ROOTS_META;
		mdelta.flags = BMAP_DELTA_NAMING_CLEAR;
		mdelta.npages = meta->npages;
		mdelta.hint = meta->hint;
		mdelta.seq = meta->seq;
		mdelta.naming = 1 + p;
		xlrec.locator = with_relnumber(rel, BMAP_ROOTS_RELNUMBER);
		xlrec.op = BMAP_TREE_ROOT_SLOT;
		xlrec.flags = 0;
		xlrec.node = xlrec.from = InvalidBlockNumber;
		xlrec.old_hw = xlrec.new_hw = InvalidBlockNumber;

		START_CRIT_SECTION();
		apply_roots_meta_delta(BufferGetPage(metabuf), &mdelta);
		MarkBufferDirty(metabuf);
		XLogBeginInsert();
		XLogRegisterData(&xlrec, sizeof(xlrec));
		XLogRegisterBuffer(0, metabuf, REGBUF_STANDARD);
		XLogRegisterBufData(0, &mdelta, sizeof(mdelta));
		lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_TREE);
		PageSetLSN(BufferGetPage(metabuf), lsn);
		END_CRIT_SECTION();
	}
	UnlockReleaseBuffer(metabuf);
}

/*
 * The metapage's hint lowered to page 'p' of slots (0-based), which may
 * have a free slot soon: as a hint, not logged.  A crash may lose it,
 * leaving free slots below the hint unused until a sweep lowers it again.
 */
static void
roots_lower_hint(RelFileLocator rel, uint32 p)
{
	Buffer		buf = roots_read(rel, 0);

	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	if (!PageIsNew(BufferGetPage(buf)) &&
		PageGetBmapRootsMeta(BufferGetPage(buf))->hint > p)
	{
		PageGetBmapRootsMeta(BufferGetPage(buf))->hint = p;
		MarkBufferDirtyHint(buf, true);
	}
	UnlockReleaseBuffer(buf);
}

/*
 * Settle slot 'slot' of the store, *r, held or let go of, on its page held
 * exclusively in 'buf': its capturer, if committed, named no more, and its
 * releaser, if it didn't commit, either; cleared if let go of by a
 * committed releaser (its subtrees queued already): true then.  One still
 * running is left as it is: VACUUM took its horizon first, which a
 * transaction running now is newer than.  (bmap_roots_reclaim(), under
 * the sweep lock.)
 */
static bool
root_settle(RelFileLocator store, Buffer buf, uint32 slot,
			const xl_bmap_root *r)
{
	xl_bmap_root settled = *r;
	OwnerState	cap = owner_state(FullTransactionIdFromU64(r->snap.owner));
	OwnerState	rel = r->released_by != 0 ?
		owner_state(FullTransactionIdFromU64(r->released_by)) : OWNER_NONE;
	bool		cleared = false;

	if (cap == OWNER_RUNNING || rel == OWNER_RUNNING)
		return false;
	if (rel == OWNER_COMMITTED || rel == OWNER_UNKNOWN)
	{
		memset(&settled, 0, sizeof(settled));
		cleared = true;
	}
	else
	{
		settled.snap.owner = 0;
		settled.released_by = 0;
	}
	if (memcmp(&settled, r, sizeof(settled)) != 0)
		roots_set_slot(store, buf, slot, &settled, InvalidBuffer);
	return cleared;
}

/*
 * The tablespaces the current database's page stores may be in: every one
 * but the global tablespace, which holds no block-mapped storage.
 */
static List *
store_tablespaces(void)
{
	Relation	rel = table_open(TableSpaceRelationId, AccessShareLock);
	TableScanDesc scan = table_beginscan_catalog(rel, 0, NULL);
	HeapTuple	tup;
	List	   *spcs = NIL;

	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Oid			spc = ((Form_pg_tablespace) GETSTRUCT(tup))->oid;

		if (spc != GLOBALTABLESPACE_OID)
			spcs = lappend_oid(spcs, spc);
	}
	table_endscan(scan);
	table_close(rel, AccessShareLock);

	/*
	 * The scan's catalog snapshot would stay registered, keeping an xmin
	 * for as long as the caller's transaction runs: autovacuum's storage
	 * maintenance would hold back every backend's horizon while it drains
	 * and sweeps.
	 */
	InvalidateCatalogSnapshot();
	return spcs;
}

/*
 * Settle the captured roots of the current database's page stores, in
 * every tablespace, so that none needs the commit log to say what became
 * of a transaction older than any running now (bmap_roots_reclaim()), as
 * the database's oldest transaction id is about to advance.  A tablespace
 * with no page store of the database's has nothing to settle.
 */
void
bmapsettlexids(void)
{
	List	   *spcs = store_tablespaces();

	foreach_oid(spc, spcs)
	{
		RelFileLocator store = {spc, MyDatabaseId, InvalidRelFileNumber};

		(void) bmap_roots_reclaim(store, true);
	}
	list_free(spcs);
}

/*
 * Reclaim the slots of rel's store whose capturers ended without
 * committing: each cleared, then its subtrees let go of (queued).  A crash
 * between the two leaves their counts a holder too high, for a sweep.
 * Slots let go of by a committed releaser need nothing: their subtrees
 * were queued, and a capture reuses the slot as it is.  How many were
 * reclaimed.  Not in recovery; under no map page's lock.
 *
 * With 'settle', every slot is made to need the commit log no more, before
 * the log forgets its transactions (VACUUM does this before the database's
 * oldest transaction id may advance, vac_update_datfrozenxid()): one held
 * names no capturer (a committed one's slot is as if captured before every
 * transaction the log knows), nor a releaser that didn't commit; one let
 * go of by a committed releaser is cleared.  Else a capture whose
 * capturer a crash ended, never reclaimed, would be held for good once the
 * log can't say it didn't commit.
 */
uint32
bmap_roots_reclaim(RelFileLocator store, bool settle)
{
	BmapRootsMeta meta;
	bool		isnew;
	Buffer		metabuf;
	uint32		n = 0;
	LOCKTAG		sweeptag;
	uint8	   *marks;

	if (RecoveryInProgress() || !roots_exist(store))
		return 0;
	ensure_queue(store);
	metabuf = roots_lock_meta(store, &meta, &isnew);
	if (isnew)
	{
		UnlockReleaseBuffer(metabuf);
		return 0;
	}
	/* (only the pages marked as naming a transaction: the others need none) */
	marks = palloc((meta.npages + 7) / 8);
	memcpy(marks, PageGetBmapRootsMeta(BufferGetPage(metabuf))->naming,
		   (meta.npages + 7) / 8);
	UnlockReleaseBuffer(metabuf);
	/* not while a sweep counts: the slot and the queue change apart */
	SET_LOCKTAG_OBJECT(sweeptag, store.dbOid, BMAP_SWEEP_CLASSID, store.spcOid, 0);
	(void) LockAcquire(&sweeptag, ShareLock, false, false);
	for (uint32 p = 0; p < meta.npages; p++)
	{
		Buffer		buf;
		xl_bmap_root abandoned[BMAP_ROOTS_PER_PAGE];
		uint32		nabandoned = 0;
		bool		freed = false;

		if ((marks[p / 8] & (1 << (p % 8))) == 0)
			continue;
		buf = roots_read(store, 1 + p);

		/* (each page once: its abandoned slots cleared, then let go of) */
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		check_page(BufferGetPage(buf), BMAP_PAGE_ROOTS,
				   with_relnumber(store, BMAP_ROOTS_RELNUMBER),
				   MAIN_FORKNUM, 1 + p);
		for (uint32 i = 0; i < BMAP_ROOTS_PER_PAGE; i++)
		{
			xl_bmap_root r = PageGetBmapRoots(BufferGetPage(buf))[i];
			uint32		slot = p * BMAP_ROOTS_PER_PAGE + i;

			if (r.snap.id == 0)
				continue;
			if (root_state(&r) == ROOT_ABANDONED)
			{
				xl_bmap_root cleared;

				memset(&cleared, 0, sizeof(cleared));
				roots_set_slot(store, buf, slot, &cleared, InvalidBuffer);
				abandoned[nabandoned++] = r;
			}
			else if (settle)
				freed |= root_settle(store, buf, slot, &r);
		}
		UnlockReleaseBuffer(buf);
		for (uint32 a = 0; a < nabandoned; a++)
		{
			BmapQueued	items[BMAP_TREE_TOPS];
			uint32		nq = queued_tops(snapshot_tops(&abandoned[a].snap), items);

			/* (a sweep counts no root its capturer may still abandon) */
			Assert(!abandoned[a].counted);
			(void) queue_push(store, items, nq);
		}
		if (nabandoned > 0 || freed)
			roots_lower_hint(store, p);
		roots_unmark_page(store, p);
		n += nabandoned;
	}
	LockRelease(&sweeptag, ShareLock, false);
	pfree(marks);
	return n;
}

/*
 * A free slot of rel's store's roots (one never used, or let go of by a
 * committed releaser), from the first page that may have one, else the
 * first of a page more: its page locked exclusively in *slotbuf (*slotnew
 * if the page is to be initialized), the metapage locked exclusively and
 * returned, its contents in *meta as the slot's record is to set them
 * (*isnew if it is to be initialized).
 */
static Buffer
roots_take_slot(RelFileLocator rel, BmapRootsMeta *metap, bool *isnewp,
				Buffer *slotbufp, bool *slotnewp, uint32 *slotp)
{
	Buffer		rootsmeta;
	BmapRootsMeta meta;
	bool		isnew;
	Buffer		slotbuf = InvalidBuffer;
	bool		slotnew = false;
	uint32		slot = 0;
	bool		found = false;

	rootsmeta = roots_lock_meta(rel, &meta, &isnew);
	for (uint32 p = meta.hint; p < meta.npages && !found; p++)
	{
		slotbuf = roots_read(rel, 1 + p);
		LockBuffer(slotbuf, BUFFER_LOCK_EXCLUSIVE);
		check_page(BufferGetPage(slotbuf), BMAP_PAGE_ROOTS,
				   with_relnumber(rel, BMAP_ROOTS_RELNUMBER), MAIN_FORKNUM, 1 + p);
		for (uint32 i = 0; i < BMAP_ROOTS_PER_PAGE; i++)
		{
			if (root_state(&PageGetBmapRoots(BufferGetPage(slotbuf))[i]) == ROOT_FREE)
			{
				slot = p * BMAP_ROOTS_PER_PAGE + i;
				found = true;
				break;
			}
		}
		if (!found)
		{
			UnlockReleaseBuffer(slotbuf);
			slotbuf = InvalidBuffer;
			meta.hint = p + 1;
		}
	}
	if (!found)
	{
		/* a page more, its first slot */
		if ((uint64) (meta.npages + 1) * BMAP_ROOTS_PER_PAGE >= (UINT64CONST(1) << (64 - ROOT_SEQ_BITS)) ||
			meta.npages + 1 > BMAP_ROOTS_NAMING_PAGES)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("page store of database %u keeps too many captured roots",
							rel.dbOid)));
		roots_cover(rel, 1 + meta.npages + 1);
		slotbuf = roots_read(rel, 1 + meta.npages);
		LockBuffer(slotbuf, BUFFER_LOCK_EXCLUSIVE);
		slotnew = PageIsNew(BufferGetPage(slotbuf));
		if (!slotnew)
			memset(BufferGetPage(slotbuf), 0, BLCKSZ);
		slotnew = true;
		slot = meta.npages * BMAP_ROOTS_PER_PAGE;
		meta.hint = meta.npages;
		meta.npages++;
	}
	*metap = meta;
	*isnewp = isnew;
	*slotbufp = slotbuf;
	*slotnewp = slotnew;
	*slotp = slot;
	return rootsmeta;
}

/*
 * The database's oldest unfrozen transaction and multixact (datfrozenxid,
 * datminmxid), which a root captured now keeps: every one its pages name
 * unfrozen is no older, so storage made of the root later may be refused
 * once the database's have passed them (bmap_root_horizon()).  Read before
 * any buffer lock is taken, from the catalog cache, which may lag behind
 * an advance: an older horizon is only more cautious.
 */
static void
database_horizon(Oid dbOid, TransactionId *frozenxid, MultiXactId *minmxid)
{
	HeapTuple	tup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(dbOid));
	Form_pg_database db;

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for database %u", dbOid);
	db = (Form_pg_database) GETSTRUCT(tup);
	*frozenxid = db->datfrozenxid;
	*minmxid = db->datminmxid;
	ReleaseSysCache(tup);
}

/*
 * A root of reln captured into its store's roots: a free slot (one never
 * used, or let go of by a committed releaser) set to the subtrees reln has
 * now and its length, each subtree gaining a holder, and
 * the map's epoch raised, as by a share (its walk of its own blocks, which
 * go into 'own', logged as images).
 * The slot names the capturing (sub)transaction.  Returns the root's id.
 */
static BmapRootId
root_capture_slot(SMgrRelation reln, BlockList *own, bool concurrent,
				  RelFileNumber of, uint32 live_epoch, const BmapRoot *of_root)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	LOCKTAG		sweeptag;
	Buffer		metabuf;
	Buffer		headerbuf;
	Buffer		rootsmeta;
	Buffer		slotbuf = InvalidBuffer;
	Buffer		rootbuf;
	BmapHeader *header;
	BmapRoot	root;
	BmapRootsMeta meta;
	bool		isnew;
	bool		slotnew = false;
	BlockNumber tops[BMAP_TREE_TOPS];
	uint32		ntops = 0;
	uint32		slot = 0;
	RefsPages	rp;
	xl_bmap_header_delta hdelta;
	xl_bmap_roots_delta rdelta;
	xl_bmap_roots_meta_delta mdelta;
	xl_bmap_tree xlrec;
	XLogRecPtr	lsn;
	BmapRootId	id;
	TransactionId frozenxid;
	MultiXactId minmxid;

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("cannot capture a root of a relation during recovery")));
	ensure_roots(rel);
	database_horizon(rel.dbOid, &frozenxid, &minmxid);

	/* not while a sweep counts what the maps hold */
	SET_LOCKTAG_OBJECT(sweeptag, rel.dbOid, BMAP_SWEEP_CLASSID, rel.spcOid, 0);
	(void) LockAcquire(&sweeptag, ShareLock, false, false);
	metabuf = alloc_read(rel, 0);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(metabuf), BMAP_PAGE_META, rel, MAIN_FORKNUM, 0);
	refs_cover(rel, PageGetBmapMeta(BufferGetPage(metabuf))->hw);
	UnlockReleaseBuffer(metabuf);

	headerbuf = preserve_header_lock_ext(reln, own, concurrent, NULL);
	if (!BufferIsValid(headerbuf))
	{
		LockRelease(&sweeptag, ShareLock, false);
		return 0;
	}
	header = PageGetBmapHeader(BufferGetPage(headerbuf));
	if (own->n > 0)
		store_flush_begin(rel);
	INJECTION_POINT("bmap-root-preserved", NULL);

	/* the root page's pointers, which the root takes */
	rootbuf = map_read(rel, 1);
	LockBuffer(rootbuf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(rootbuf), BMAP_PAGE_ROOT, rel, BMAP_FORKNUM, 1);
	root = *PageGetBmapRoot(BufferGetPage(rootbuf));
	UnlockReleaseBuffer(rootbuf);

	/* a free slot, from the first page that may have one */
	rootsmeta = roots_take_slot(rel, &meta, &isnew, &slotbuf, &slotnew, &slot);

	for (int k = 0; k < BMAP_TREE_TOPS; k++)
		if (BlockNumberIsValid(root.top[k].block))
			tops[ntops++] = root.top[k].block;
	qsort(tops, ntops, sizeof(BlockNumber), cmp_block);
	rp.n = 0;
	if (ntops > 0 && refs_lock(rel, tops, ntops, &rp) != ntops)
		elog(ERROR, "the subtrees of \"%s\" span too many count pages",
			 relpath(reln->smgr_rlocator, MAIN_FORKNUM).str);
	refs_add_holders(&rp, rel, tops, ntops);

	hdelta.kind = BMAP_DELTA_HEADER;
	hdelta.flags = 0;
	hdelta.nblocks = header->nblocks;
	hdelta.vm_nblocks = header->vm_nblocks;
	hdelta.free_space_unknown = 0;
	hdelta.epoch = header->epoch + 1;
	hdelta.shared_below = 0;
	memset(&rdelta, 0, sizeof(rdelta));
	rdelta.kind = BMAP_DELTA_ROOTS;
	rdelta.flags = slotnew ? BMAP_DELTA_INIT : 0;
	rdelta.index = ROOT_INDEX(slot);
	rdelta.root.snap.owner = U64FromFullTransactionId(GetCurrentFullTransactionId());
	rdelta.root.snap.nblocks = header->nblocks;
	/*
	 * A root keeps the relation's visibility map, whose bits hold for the
	 * pages it keeps: the walk above found every own page, the map's
	 * included, idle at once, and a change of a page and its map's bit is
	 * made under both pages' locks.  A frozen root keeps none: its copy's
	 * pages were frozen into it below the map, which may still say what
	 * held of the pages they replaced, so storage made of it starts with
	 * an empty map (the entries it holds past that length are let go of
	 * when the map is extended).
	 */
	rdelta.root.snap.vm_nblocks = RelFileNumberIsValid(of) ? 0 :
		header->vm_nblocks;
	rdelta.root.snap.epoch = hdelta.epoch;
	rdelta.root.relnumber = RelFileNumberIsValid(of) ? of : rel.relNumber;
	rdelta.root.map_id = root.map_id;
	/* (a frozen root's are the relation's whose pages it froze) */
	rdelta.root.lineage = RelFileNumberIsValid(of) ? root_lineage(of_root) :
		root_lineage(&root);
	rdelta.root.live_map = RelFileNumberIsValid(of) ? of_root->map_id : 0;
	rdelta.root.live_epoch = live_epoch;
	rdelta.root.frozenxid = frozenxid;
	rdelta.root.minmxid = minmxid;
	for (int k = 0; k < BMAP_TREE_TOPS; k++)
	{
		rdelta.root.snap.tops[k].block = root.top[k].block;
		rdelta.root.snap.tops[k].epoch = root.top[k].epoch;
		rdelta.root.snap.tops[k].since = root.top[k].since;
	}
	memset(&mdelta, 0, sizeof(mdelta));
	mdelta.kind = BMAP_DELTA_ROOTS_META;
	mdelta.flags = isnew ? BMAP_DELTA_INIT : 0;
	mdelta.npages = meta.npages;
	mdelta.hint = meta.hint;
	mdelta.seq = meta.seq + 1;
	mdelta.naming = ROOT_PAGE(slot);
	id = rdelta.root.snap.id = ROOT_ID(slot, mdelta.seq);
	xlrec.locator = rel;
	xlrec.op = BMAP_TREE_CAPTURE;
	/* (tree_preserve() logged images of the own blocks) */
	xlrec.flags = BMAP_TREE_PRESERVE_LOGGED;
	xlrec.node = xlrec.from = InvalidBlockNumber;
	xlrec.old_hw = xlrec.new_hw = InvalidBlockNumber;

	XLogEnsureRecordSpace(3 + BMAP_TREE_TOPS, 4 + BMAP_TREE_TOPS);
	START_CRIT_SECTION();
	apply_roots_delta(BufferGetPage(slotbuf), &rdelta);
	MarkBufferDirty(slotbuf);
	apply_header_delta(BufferGetPage(headerbuf), &hdelta);
	MarkBufferDirty(headerbuf);
	apply_roots_meta_delta(BufferGetPage(rootsmeta), &mdelta);
	MarkBufferDirty(rootsmeta);
	XLogBeginInsert();
	XLogRegisterData(&xlrec, sizeof(xlrec));
	/* (the slot's delta kept with an image too: redo walks what it names) */
	XLogRegisterBuffer(0, slotbuf, REGBUF_STANDARD | REGBUF_KEEP_DATA |
					   (slotnew ? REGBUF_WILL_INIT : 0));
	XLogRegisterBufData(0, &rdelta, sizeof(rdelta));
	XLogRegisterBuffer(1, headerbuf, REGBUF_STANDARD);
	XLogRegisterBufData(1, &hdelta, sizeof(hdelta));
	XLogRegisterBuffer(2, rootsmeta, REGBUF_STANDARD |
					   (isnew ? REGBUF_WILL_INIT : 0));
	XLogRegisterBufData(2, &mdelta, sizeof(mdelta));
	refs_apply_register(&rp, 3);
	lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_TREE);
	PageSetLSN(BufferGetPage(slotbuf), lsn);
	PageSetLSN(BufferGetPage(headerbuf), lsn);
	PageSetLSN(BufferGetPage(rootsmeta), lsn);
	refs_set_lsn(&rp, lsn);
	END_CRIT_SECTION();

	refs_release(&rp);
	UnlockReleaseBuffer(slotbuf);
	UnlockReleaseBuffer(rootsmeta);
	UnlockReleaseBuffer(headerbuf);
	LockRelease(&sweeptag, ShareLock, false);
	return id;
}

/*
 * Capture a root of reln: its pages as they are, kept by its page store
 * until bmap_root_release() (see bmap.h).  As for a share, the caller
 * keeps reln from changing meanwhile, and its own pages are written out
 * before this returns, their images logged first; nothing is synced.
 */
BmapRootId
bmap_root_capture(SMgrRelation reln)
{
	BmapRootId	id;

	bmap_root_capture_many(&reln, 1, &id, NULL);
	return id;
}

/*
 * bmap_root_capture() of each of rels[] (n of them), their ids into ids[],
 * the generation of sharing raised once for them all.  Each id is set as
 * its root is captured (the others 0), so that, if one fails, the caller
 * knows of those captured already, which an abort lets go of.  owned (if
 * not NULL) gets each one's own pages then, those no root held before
 * (changed since the last capture, or new): what a later change of them
 * keeps for the root alone.
 */
void
bmap_root_capture_many(SMgrRelation *rels, int n, BmapRootId *ids,
					   uint32 *owned)
{
	BlockList  *own = palloc0_array(BlockList, Max(n, 1));

	for (int i = 0; i < n; i++)
		ids[i] = 0;
	for (int i = 0; i < n; i++)
	{
		ids[i] = root_capture_slot(rels[i], &own[i], false,
								   InvalidRelFileNumber, 0, NULL);
		if (owned)
			owned[i] = own[i].n;
		INJECTION_POINT("bmap-root-slot-captured", NULL);
	}
	for (int i = 0; i < n; i++)
		MarkRelationPagesShared(rels[i]->smgr_rlocator.locator);
	for (int i = 0; i < n; i++)
	{
		flush_own_blocks(rels[i], &own[i]);
		if (own[i].n > 0)
			store_flush_end(rels[i]->smgr_rlocator.locator, false);
		if (own[i].blocks)
			pfree(own[i].blocks);
	}
	pfree(own);
	if (n > 0)
		INJECTION_POINT("bmap-roots-captured", NULL);
}

/*
 * bmap_root_capture_many(), but with the relations' writers at work: each
 * root captured at a moment no writer is changing one of the relation's
 * pages (preserve_header_lock_ext()'s concurrent walk), so that its pages
 * are the relation's as at that moment, each as its last change left it.
 * The relations are captured one after another, in the order given, each
 * at its own moment.  False if one's writers never left it so (its walks
 * ran out): the roots captured before it are in ids[], the rest 0.
 */
bool
bmap_root_capture_concurrent(SMgrRelation *rels, int n, BmapRootId *ids)
{
	bool		ok = true;
	int			done = 0;
	BlockList  *own = palloc0_array(BlockList, Max(n, 1));

	for (int i = 0; i < n; i++)
		ids[i] = 0;
	for (int i = 0; i < n && ok; i++)
	{
		ids[i] = root_capture_slot(rels[i], &own[i], true,
								   InvalidRelFileNumber, 0, NULL);
		ok = ids[i] != 0;
		if (ok)
			done++;
	}

	/*
	 * (no raise of every page's generation: each relation's walk raised its
	 * own under its header, its writers' next changes waiting for the header
	 * and then seeing the raised epoch)
	 */
	for (int i = 0; i < done; i++)
	{
		flush_own_blocks(rels[i], &own[i]);
		if (own[i].n > 0)
			store_flush_end(rels[i]->smgr_rlocator.locator, false);
	}
	for (int i = 0; i < n; i++)
		if (own[i].blocks)
			pfree(own[i].blocks);
	pfree(own);
	if (done > 0)
		INJECTION_POINT("bmap-roots-captured", NULL);
	return ok;
}

/*
 * A root of reln (a copy of relation 'of' whose pages were frozen into it),
 * captured as bmap_root_capture() does, recorded as of relation 'of', its
 * pages changed since told by epoch 'live_epoch' there (bmap_root_since()),
 * and of its map (its identity, which bmap_root_base() asks, and lineage).
 *
 * The copy is temporary storage, dropped once the root is captured, so its
 * pages are logged as bmap_root_capture() logs a relation's own (the
 * preserve walk's images, which name their physical blocks), not as the
 * copy's logical blocks: crash recovery whose redo begins after the copy
 * was made, and so finds no map for it, replays its records onto nothing
 * the root holds, while still zeroing the page store's blocks the copy was
 * extended by; only an image of the physical block, logged after, restores
 * them.
 */
BmapRootId
bmap_root_capture_of(SMgrRelation reln, SMgrRelation of, uint32 live_epoch)
{
	RelFileLocator live = of->smgr_rlocator.locator;
	BlockList	own = {0};
	Buffer		buf = map_read(live, 1);
	BmapRoot	of_root;
	BmapRootId	id;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(buf), BMAP_PAGE_ROOT, live, BMAP_FORKNUM, 1);
	of_root = *PageGetBmapRoot(BufferGetPage(buf));
	UnlockReleaseBuffer(buf);
	id = root_capture_slot(reln, &own, false, live.relNumber, live_epoch,
						   &of_root);

	MarkRelationPagesShared(reln->smgr_rlocator.locator);
	flush_own_blocks(reln, &own);
	if (own.n > 0)
		store_flush_end(reln->smgr_rlocator.locator, false);
	if (own.blocks)
		pfree(own.blocks);
	INJECTION_POINT("bmap-roots-captured", NULL);
	return id;
}

/*
 * Root 'id' of the store's slot, read under its page's lock, which stays
 * held in *buf in 'mode' (BUFFER_LOCK_SHARE or _EXCLUSIVE; with -1, a share
 * lock let go of): false, *buf invalid, if the slot holds another root, or
 * none.
 */
static bool
root_read(RelFileLocator store, BmapRootId id, int mode,
		  Buffer *buf, xl_bmap_root *r)
{
	return root_read_under(store, id, mode, buf, r, InvalidBuffer);
}

/* root_read(), the metapage held already in 'heldmeta' if valid */
static bool
root_read_under(RelFileLocator store, BmapRootId id, int mode,
				Buffer *buf, xl_bmap_root *r, Buffer heldmeta)
{
	uint32		slot = ROOT_SLOT(id);
	BmapRootsMeta meta;
	Buffer		metabuf;
	bool		isnew;

	*buf = InvalidBuffer;
	if (id == 0 || !roots_exist(store))
		return false;
	metabuf = BufferIsValid(heldmeta) ? heldmeta : roots_read(store, 0);
	if (!BufferIsValid(heldmeta))
		LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	isnew = PageIsNew(BufferGetPage(metabuf));
	if (!isnew)
	{
		check_page(BufferGetPage(metabuf), BMAP_PAGE_ROOTS_META,
				   with_relnumber(store, BMAP_ROOTS_RELNUMBER), MAIN_FORKNUM, 0);
		meta = *PageGetBmapRootsMeta(BufferGetPage(metabuf));
	}
	if (!BufferIsValid(heldmeta))
		UnlockReleaseBuffer(metabuf);
	if (isnew || ROOT_PAGE(slot) > meta.npages)
		return false;
	*buf = roots_read(store, ROOT_PAGE(slot));
	LockBuffer(*buf, mode < 0 ? BUFFER_LOCK_SHARE : mode);
	check_page(BufferGetPage(*buf), BMAP_PAGE_ROOTS,
			   with_relnumber(store, BMAP_ROOTS_RELNUMBER), MAIN_FORKNUM,
			   ROOT_PAGE(slot));
	*r = PageGetBmapRoots(BufferGetPage(*buf))[ROOT_INDEX(slot)];
	if (r->snap.id != id)
	{
		UnlockReleaseBuffer(*buf);
		*buf = InvalidBuffer;
		return false;
	}
	if (mode < 0)
	{
		UnlockReleaseBuffer(*buf);
		*buf = InvalidBuffer;
	}
	return true;
}

/*
 * Is the root in slot *r held as this (sub)transaction sees it: captured by
 * a committed transaction or this one, and let go of by nobody that
 * committed, nor by this transaction?
 */
static bool
root_held_as_seen(const xl_bmap_root *r)
{
	OwnerState	cap = owner_state(FullTransactionIdFromU64(r->snap.owner));

	if (cap == OWNER_ENDED ||
		(cap == OWNER_RUNNING &&
		 !TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(FullTransactionIdFromU64(r->snap.owner)))))
		return false;
	if (r->released_by != 0)
	{
		OwnerState	rel = owner_state(FullTransactionIdFromU64(r->released_by));

		if (rel != OWNER_ENDED &&
			(rel != OWNER_RUNNING ||
			 TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(FullTransactionIdFromU64(r->released_by)))))
			return false;
	}
	return true;
}

/*
 * May the root in slot *r, read under its page's lock, gain a holder (be
 * shared, or copied): held as this transaction sees it, and named by no
 * releaser that may yet commit.  A releaser names a root under the page's
 * exclusive lock and then queues its subtrees, so one not named while the
 * lock is held can't let go of them before the holders are added; one
 * named may commit meanwhile, and the drain then free what the copy is
 * to hold.
 */
static bool
root_may_hold(const xl_bmap_root *r)
{
	return root_held_as_seen(r) &&
		(r->released_by == 0 ||
		 owner_state(FullTransactionIdFromU64(r->released_by)) == OWNER_ENDED);
}

/*
 * Root 'id' of the store held again, by a root of its own: a free slot set
 * to the same subtrees, each gaining a holder, naming the current
 * (sub)transaction as its capturer, so that it is held from that one's
 * commit and let go of on its own, and naming the map of holder (the
 * relation it is held for) as the one it was captured of: that map's own,
 * and its lineage, as a root captured of it would be (own_root_tops()).  O(1), nothing
 * written out: the blocks are the root's, durable since it was captured.
 * Its id, or 0 if the store holds no such root.
 */
BmapRootId
bmap_root_copy(RelFileLocator store, BmapRootId id, RelFileLocator holder)
{
	uint64		holder_map;
	uint64		holder_lineage;
	LOCKTAG		sweeptag;
	Buffer		buf;
	Buffer		rootsmeta;
	Buffer		slotbuf;
	BmapRootsMeta meta;
	bool		isnew;
	bool		slotnew;
	uint32		slot;
	xl_bmap_root r;
	BlockNumber tops[BMAP_TREE_TOPS];
	uint32		ntops = 0;
	RefsPages	rp;
	Buffer		metabuf;
	xl_bmap_roots_delta rdelta;
	xl_bmap_roots_meta_delta mdelta;
	xl_bmap_tree xlrec;
	XLogRecPtr	lsn;
	BmapRootId	copy;

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("cannot capture a root of a relation during recovery")));
	if (!bmap_root_held(store, id, NULL) ||
		!root_read(store, id, -1, &buf, &r))
		return 0;
	buf = map_read(holder, 1);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(buf), BMAP_PAGE_ROOT, holder, BMAP_FORKNUM, 1);
	holder_map = PageGetBmapRoot(BufferGetPage(buf))->map_id;
	holder_lineage = root_lineage(PageGetBmapRoot(BufferGetPage(buf)));
	UnlockReleaseBuffer(buf);

	/* not while a sweep counts what the maps hold */
	SET_LOCKTAG_OBJECT(sweeptag, store.dbOid, BMAP_SWEEP_CLASSID, store.spcOid, 0);
	(void) LockAcquire(&sweeptag, ShareLock, false, false);
	metabuf = alloc_read(store, 0);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(metabuf), BMAP_PAGE_META, store, MAIN_FORKNUM, 0);
	refs_cover(store, PageGetBmapMeta(BufferGetPage(metabuf))->hw);
	UnlockReleaseBuffer(metabuf);

	/*
	 * (the root read again under its slot page's lock, which keeps a
	 * releaser from naming it until the holders are added: one named since
	 * the first look may commit, and its subtrees be let go of, meanwhile.
	 * The slot taken may be on the same page, locked already.)
	 */
	rootsmeta = roots_take_slot(store, &meta, &isnew, &slotbuf, &slotnew, &slot);
	buf = InvalidBuffer;
	if (ROOT_PAGE(ROOT_SLOT(id)) == ROOT_PAGE(slot))
		r = PageGetBmapRoots(BufferGetPage(slotbuf))[ROOT_INDEX(ROOT_SLOT(id))];
	else
	{
		/* (not by root_read(), which locks the metapage, held here) */
		buf = roots_read(store, ROOT_PAGE(ROOT_SLOT(id)));
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		check_page(BufferGetPage(buf), BMAP_PAGE_ROOTS,
				   with_relnumber(store, BMAP_ROOTS_RELNUMBER), MAIN_FORKNUM,
				   ROOT_PAGE(ROOT_SLOT(id)));
		r = PageGetBmapRoots(BufferGetPage(buf))[ROOT_INDEX(ROOT_SLOT(id))];
	}
	if (r.snap.id != id || !root_may_hold(&r))
	{
		if (BufferIsValid(buf))
			UnlockReleaseBuffer(buf);
		UnlockReleaseBuffer(slotbuf);
		UnlockReleaseBuffer(rootsmeta);
		LockRelease(&sweeptag, ShareLock, false);
		return 0;
	}
	for (int k = 0; k < BMAP_TREE_TOPS; k++)
		if (BlockNumberIsValid(r.snap.tops[k].block))
			tops[ntops++] = r.snap.tops[k].block;
	qsort(tops, ntops, sizeof(BlockNumber), cmp_block);
	rp.n = 0;
	if (ntops > 0 && refs_lock(store, tops, ntops, &rp) != ntops)
		elog(ERROR, "the subtrees of a captured root span too many count pages");
	refs_add_holders(&rp, store, tops, ntops);

	memset(&rdelta, 0, sizeof(rdelta));
	rdelta.kind = BMAP_DELTA_ROOTS;
	rdelta.flags = slotnew ? BMAP_DELTA_INIT : 0;
	rdelta.index = ROOT_INDEX(slot);
	rdelta.root.snap = r.snap;
	rdelta.root.snap.owner = U64FromFullTransactionId(GetCurrentFullTransactionId());
	rdelta.root.relnumber = holder.relNumber;
	rdelta.root.map_id = holder_map;
	rdelta.root.lineage = holder_lineage;
	/*
	 * (a frozen root's copy is frozen too, but the holder's changes since
	 * are told by the root's own epoch, the holder being made of it)
	 */
	rdelta.root.live_epoch = r.live_epoch != 0 ? BMAP_LIVE_EPOCH_COPY : 0;
	/* (the same pages, so the same horizon) */
	rdelta.root.frozenxid = r.frozenxid;
	rdelta.root.minmxid = r.minmxid;
	memset(&mdelta, 0, sizeof(mdelta));
	mdelta.kind = BMAP_DELTA_ROOTS_META;
	mdelta.flags = isnew ? BMAP_DELTA_INIT : 0;
	mdelta.npages = meta.npages;
	mdelta.hint = meta.hint;
	mdelta.seq = meta.seq + 1;
	mdelta.naming = ROOT_PAGE(slot);
	copy = rdelta.root.snap.id = ROOT_ID(slot, mdelta.seq);
	xlrec.locator = with_relnumber(store, BMAP_ROOTS_RELNUMBER);
	xlrec.op = BMAP_TREE_ROOT_SLOT;
	xlrec.flags = 0;
	xlrec.node = xlrec.from = InvalidBlockNumber;
	xlrec.old_hw = xlrec.new_hw = InvalidBlockNumber;

	XLogEnsureRecordSpace(2 + BMAP_TREE_TOPS, 3 + BMAP_TREE_TOPS);
	START_CRIT_SECTION();
	apply_roots_delta(BufferGetPage(slotbuf), &rdelta);
	MarkBufferDirty(slotbuf);
	apply_roots_meta_delta(BufferGetPage(rootsmeta), &mdelta);
	MarkBufferDirty(rootsmeta);
	XLogBeginInsert();
	XLogRegisterData(&xlrec, sizeof(xlrec));
	XLogRegisterBuffer(0, slotbuf, REGBUF_STANDARD |
					   (slotnew ? REGBUF_WILL_INIT : 0));
	XLogRegisterBufData(0, &rdelta, sizeof(rdelta));
	XLogRegisterBuffer(1, rootsmeta, REGBUF_STANDARD |
					   (isnew ? REGBUF_WILL_INIT : 0));
	XLogRegisterBufData(1, &mdelta, sizeof(mdelta));
	refs_apply_register(&rp, 2);
	lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_TREE);
	PageSetLSN(BufferGetPage(slotbuf), lsn);
	PageSetLSN(BufferGetPage(rootsmeta), lsn);
	refs_set_lsn(&rp, lsn);
	END_CRIT_SECTION();

	refs_release(&rp);
	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
	UnlockReleaseBuffer(slotbuf);
	UnlockReleaseBuffer(rootsmeta);
	LockRelease(&sweeptag, ShareLock, false);
	return copy;
}


/*
 * Is root 'id' held, as this (sub)transaction sees it: captured by a
 * committed transaction or this one, and let go of by nobody that
 * committed, nor by this transaction?  Its length into *nblocks, if so.
 */
bool
bmap_root_held(RelFileLocator store, BmapRootId id, BlockNumber *nblocks)
{
	Buffer		buf;
	xl_bmap_root r;

	if (!root_read(store, id, -1, &buf, &r) || !root_held_as_seen(&r))
		return false;
	if (nblocks)
		*nblocks = r.snap.nblocks;
	return true;
}

/*
 * Let go of root 'id' as the current (sub)transaction commits: its slot
 * names it, and its subtrees are pushed onto the release queue as entries
 * naming it (see "Captured roots").  False if the store has no such root,
 * or it is let go of already.  One a releaser that didn't commit named is
 * named again.
 */
bool
bmap_root_release(RelFileLocator store, BmapRootId id)
{
	Buffer		buf;
	Buffer		metabuf;
	xl_bmap_root r;
	FullTransactionId me;
	BmapQueued	items[BMAP_TREE_TOPS];
	uint32		n;
	LOCKTAG		sweeptag;

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("cannot let go of a captured root during recovery")));
	/* (a store dropped with its tablespace holds nothing) */
	if (id == 0 || !roots_exist(store))
		return false;
	/* (a queue to make takes a lock, not to be waited for under a page) */
	ensure_queue(store);
	/* not while a sweep counts: the slot and the queue change apart */
	SET_LOCKTAG_OBJECT(sweeptag, store.dbOid, BMAP_SWEEP_CLASSID, store.spcOid, 0);
	(void) LockAcquire(&sweeptag, ShareLock, false, false);
	/* (the metapage first: the slot's page marked as naming its releaser) */
	metabuf = roots_read(store, 0);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	if (!root_read_under(store, id, BUFFER_LOCK_EXCLUSIVE, &buf, &r, metabuf))
	{
		UnlockReleaseBuffer(metabuf);
		LockRelease(&sweeptag, ShareLock, false);
		return false;
	}
	if ((r.released_by != 0 &&
		 owner_state(FullTransactionIdFromU64(r.released_by)) != OWNER_ENDED) ||
		root_state(&r) == ROOT_ABANDONED)
	{
		UnlockReleaseBuffer(buf);
		UnlockReleaseBuffer(metabuf);
		LockRelease(&sweeptag, ShareLock, false);
		return false;
	}
	me = GetCurrentFullTransactionId();
	r.released_by = U64FromFullTransactionId(me);
	roots_set_slot(store, buf, ROOT_SLOT(id), &r, metabuf);
	UnlockReleaseBuffer(buf);
	UnlockReleaseBuffer(metabuf);

	/*
	 * A crash before the push leaves the slot naming a releaser that never
	 * committed: it holds as before.
	 */
	n = queued_tops(snapshot_tops(&r.snap), items);
	for (uint32 i = 0; i < n; i++)
	{
		queued_set_owner(&items[i], me);
		/* (a root the roots' counts count: its pointers carry them now) */
		if (r.counted)
			items[i].flags |= BMAP_QUEUED_COUNTED;
	}
	(void) queue_push(store, items, n);
	LockRelease(&sweeptag, ShareLock, false);
	roots_lower_hint(store, ROOT_PAGE(ROOT_SLOT(id)) - 1);
	return true;
}

/*
 * Are reln's pages still those of root 'id': its root page pointing to the
 * subtrees the root holds, and its forks' lengths the root's?  O(1): a page
 * changed since under a subtree the root holds was copied first, so the
 * root page points to the copy.  False if the store holds no such root.
 */
bool
bmap_root_unchanged(SMgrRelation reln, BmapRootId id)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		headerbuf;
	Buffer		rootbuf;
	Buffer		buf;
	BmapRoot	root;
	xl_bmap_root r;
	BlockNumber nblocks;
	BlockNumber vm_nblocks;
	bool		same;

	if (!root_read(rel, id, -1, &buf, &r))
		return false;
	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	nblocks = PageGetBmapHeader(BufferGetPage(headerbuf))->nblocks;
	vm_nblocks = PageGetBmapHeader(BufferGetPage(headerbuf))->vm_nblocks;
	rootbuf = map_read(rel, 1);
	LockBuffer(rootbuf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(rootbuf), BMAP_PAGE_ROOT, rel, BMAP_FORKNUM, 1);
	root = *PageGetBmapRoot(BufferGetPage(rootbuf));
	UnlockReleaseBuffer(rootbuf);
	UnlockReleaseBuffer(headerbuf);
	same = r.snap.nblocks == nblocks && r.snap.vm_nblocks == vm_nblocks;
	for (int k = 0; same && k < BMAP_TREE_TOPS; k++)
	{
		const BmapEntry *top = &snapshot_tops(&r.snap)[k];

		same = root.top[k].block == top->block &&
			(!BlockNumberIsValid(top->block) || root.top[k].since == top->since);
	}
	return same;
}

/*
 * Make 'dst', a new relation file number, a copy of root 'id' of its page
 * store: its root page gets the root's subtrees, each gaining a holder, and
 * its header the root's length, in one record.  O(1) in the relation's size; the root's blocks
 * were made durable when it was captured.
 * With 'replacing' (the storage dst takes the place of, dropped as the
 * transaction commits: a restore), dst's entries may be shared with
 * another map only where replacing's might.  Otherwise every entry of dst
 * may be; and with 'sharer', a relation still holding the root's pages
 * (the storage of another branch's table the root was kept of), so may
 * those of sharer, which its header's shared_below then says (in the same
 * record), so that its VACUUM leaves them.  With 'lineage_of', dst carries
 * on that storage's lineage (the one it takes the place of, whose roots are
 * then of dst too); otherwise it begins its own.  Only the main fork.  The
 * root's slot is read under its page's lock, which keeps a releaser from
 * naming it meanwhile; a header held is locked first, as a capture locks
 * it.
 */
static uint64 bmap_map_id(SMgrRelation reln);

void
bmap_share_root(BmapRootId id, SMgrRelation dst, SMgrRelation replacing,
				SMgrRelation sharer, SMgrRelation lineage_of)
{
	RelFileLocator drel = dst->smgr_rlocator.locator;
	LOCKTAG		sweeptag;
	Buffer		buf;
	Buffer		sheader = InvalidBuffer;
	xl_bmap_root r;
	xl_bmap_header_delta hdelta;
	xl_bmap_header_delta shdelta;
	uint64		lineage = lineage_of ? bmap_lineage(lineage_of) : 0;

	smgrusewhich(dst, SMGR_BMAP);
	SET_LOCKTAG_OBJECT(sweeptag, drel.dbOid, BMAP_SWEEP_CLASSID, drel.spcOid, 0);
	(void) LockAcquire(&sweeptag, ShareLock, false, false);
	if (!bmap_root_held(drel, id, NULL) ||
		!root_read(drel, id, -1, &buf, &r))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("page store of database %u holds no captured root %llu",
						drel.dbOid, (unsigned long long) id)));

	INJECTION_POINT("bmap-root-share-checked", NULL);

	/*
	 * The relation the root was captured of, if the caller didn't name it
	 * and it is still that map (its file number may be another's since):
	 * it shares the root's nodes with the new storage from now on, so its
	 * shared_below is raised past them, as a share raises its source's.  A
	 * node it has alone again once the new storage copies it is then moved
	 * at its next change rather than changed in place (tree_leaf_for()),
	 * where a lookup of the new storage holding a pointer to it would read
	 * the relation's page.  (A frozen root's nodes are of the copy it was
	 * captured of, whose file number it doesn't keep: that copy is the
	 * caller's to name, if it outlives the capture.)
	 */
	if (sharer == NULL && r.live_map == 0)
	{
		RelFileLocator of = drel;
		SMgrRelation ofreln;

		of.relNumber = r.relnumber;
		ofreln = smgropen(of, INVALID_PROC_NUMBER);
		if (!RelFileLocatorEquals(of, drel) &&
			(replacing == NULL ||
			 !RelFileLocatorEquals(of, replacing->smgr_rlocator.locator)) &&
			mdexists(ofreln, BMAP_FORKNUM) &&
			bmap_map_id(ofreln) == r.map_id)
			sharer = ofreln;
	}

	/* the header of the one captured of */
	if (sharer != NULL)
	{
		RelFileLocator srel = sharer->smgr_rlocator.locator;

		sheader = map_read(srel, 0);
		LockBuffer(sheader, BUFFER_LOCK_EXCLUSIVE);
		if (PageIsNew(BufferGetPage(sheader)))
		{
			UnlockReleaseBuffer(sheader);
			sheader = InvalidBuffer;
		}
		else
		{
			BmapHeader *sh = PageGetBmapHeader(BufferGetPage(sheader));

			check_page(BufferGetPage(sheader), BMAP_PAGE_HEADER, srel,
					   BMAP_FORKNUM, 0);
			shdelta.kind = BMAP_DELTA_HEADER;
			shdelta.flags = BMAP_DELTA_SHARED_BELOW;
			shdelta.nblocks = sh->nblocks;
			shdelta.vm_nblocks = sh->vm_nblocks;
			shdelta.free_space_unknown = 0;
			shdelta.epoch = sh->epoch;
			shdelta.shared_below = Max(sh->shared_below, r.snap.epoch);
			if (shdelta.shared_below == sh->shared_below)
			{
				UnlockReleaseBuffer(sheader);
				sheader = InvalidBuffer;
			}
		}
	}

	/*
	 * (held still, under the slot page's lock, which keeps a releaser from
	 * naming it until the holders are added: one named since the first
	 * look may commit, and its subtrees be let go of, meanwhile)
	 */
	if (!root_read(drel, id, BUFFER_LOCK_SHARE, &buf, &r) || !root_may_hold(&r))
	{
		if (BufferIsValid(buf))
			UnlockReleaseBuffer(buf);
		if (BufferIsValid(sheader))
			UnlockReleaseBuffer(sheader);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("page store of database %u holds no captured root %llu",
						drel.dbOid, (unsigned long long) id)));
	}
	create_map(dst);
	hdelta.kind = BMAP_DELTA_HEADER;
	/* (made without a free space map, or with another's: none known) */
	hdelta.flags = BMAP_DELTA_SHARED_BELOW | BMAP_DELTA_FREE_SPACE;
	hdelta.nblocks = r.snap.nblocks;
	hdelta.vm_nblocks = r.snap.vm_nblocks;
	hdelta.free_space_unknown = r.snap.nblocks;
	hdelta.epoch = r.snap.epoch;
	/*
	 * Every node the new storage holds is the root's, which other maps may
	 * hold too: so all are born before its shared_below, and a node it has
	 * alone again is moved, not changed in place (tree_leaf_for()).  (Not
	 * the storage replaced's lower one: its nodes aren't these.)
	 */
	hdelta.shared_below = hdelta.epoch;
	tree_share_root(dst, dst, snapshot_tops(&r.snap), &hdelta, sheader,
					&shdelta, id, lineage);
	UnlockReleaseBuffer(buf);
	if (BufferIsValid(sheader))
		UnlockReleaseBuffer(sheader);
	LockRelease(&sweeptag, ShareLock, false);
	dst->smgr_cached_nblocks[MAIN_FORKNUM] = InvalidBlockNumber;
	dst->smgr_cached_nblocks[VISIBILITYMAP_FORKNUM] = InvalidBlockNumber;
	INJECTION_POINT("bmap-root-shared", NULL);
}

/*
 * Freezing roots (bmap_root_capture() of a copy whose pages are frozen): a
 * relation's pages changed since a root, found by their entries' epochs
 * (bmap_changed_pages()); the map's epoch raised, so each page's next change
 * marks it changed again (bmap_raise_epoch()); and a root captured of a copy
 * holding the frozen pages, its own pages logged as images of their
 * physical blocks (bmap_root_capture_of()), recorded as of the relation it
 * froze and the epoch its next changes are told by.  The walks are of the
 * main fork alone: a frozen root keeps no visibility map (see
 * root_capture_slot()), so the map's pages changed since matter to none.
 */

typedef struct ChangedWalk
{
	uint32		since;
	BlockNumber nblocks;
	bool		any;			/* only whether one changed: the first ends it */
	BlockNumber *blknos;
	uint32		n;
	uint32		cap;
} ChangedWalk;

static void
changed_add(ChangedWalk *cw, BlockNumber blkno)
{
	if (cw->any)
	{
		cw->n = 1;
		return;
	}
	if (cw->n == cw->cap)
	{
		cw->cap = Max(cw->cap * 2, 64);
		cw->blknos = cw->blknos ? repalloc_array(cw->blknos, BlockNumber, cw->cap) :
			palloc_array(BlockNumber, cw->cap);
	}
	cw->blknos[cw->n++] = blkno;
}

/*
 * The changed pages under node 'ptr', of 'level', mapping from 'first':
 * only its children set since the walk's epoch are read further (a
 * pointer set before it has nothing changed since under it: a change
 * stamps every pointer above its leaf, tree_stamp_node()).
 */
static void
changed_walk_node(SMgrRelation reln, const BmapEntry *ptr, uint16 level,
				  BlockNumber first, ChangedWalk *cw)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	BmapEntry  *entries;
	Buffer		buf;

	if (first >= cw->nblocks || ptr->epoch < cw->since)
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
		uint64		child = (uint64) first + (level == 0 ? i :
											   i * tree_span(level - 1));

		if (!BlockNumberIsValid(entries[i].block) || child >= cw->nblocks)
			continue;
		if (level == 0)
		{
			if (entries[i].epoch >= cw->since)
				changed_add(cw, (BlockNumber) child);
		}
		else
			changed_walk_node(reln, &entries[i], level - 1,
							  (BlockNumber) child, cw);
		if (cw->any && cw->n > 0)
			break;
	}
	pfree(entries);
}

/* changed_walk_node() from each of reln's subtrees: its length into *nblocks */
static void
changed_walk(SMgrRelation reln, ChangedWalk *cw)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		headerbuf;
	Buffer		buf;
	BmapRoot	root;
	uint64		first = 0;

	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	cw->nblocks = PageGetBmapHeader(BufferGetPage(headerbuf))->nblocks;
	buf = map_read(rel, 1);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(buf), BMAP_PAGE_ROOT, rel, BMAP_FORKNUM, 1);
	root = *PageGetBmapRoot(BufferGetPage(buf));
	UnlockReleaseBuffer(buf);
	UnlockReleaseBuffer(headerbuf);
	for (int k = 0; k < BMAP_TREE_TOPS && first < cw->nblocks; k++)
	{
		if (BlockNumberIsValid(root.top[k].block))
			changed_walk_node(reln, &root.top[k], k, (BlockNumber) first, cw);
		if (cw->any && cw->n > 0)
			break;
		first += tree_span(k);
	}
}

/*
 * The logical blocks of reln under its length whose entries were set in
 * epoch 'since' or after (each changed since: a write after a raise of the
 * epoch claims or copies its page, setting its entry's epoch), into
 * *blknos (palloc'd, ascending): how many.  Its length into *nblocks.  The
 * caller keeps reln from changing.  It reads the map's nodes set since,
 * which are those above the pages changed: O(changed pages) map pages,
 * none but its header and root page if nothing changed.
 */
uint32
bmap_changed_pages(SMgrRelation reln, uint32 since, BlockNumber **blknos,
				   BlockNumber *nblocks)
{
	ChangedWalk cw = {since, 0, false, NULL, 0, 0};

	changed_walk(reln, &cw);
	if (cw.n > 1)
		qsort(cw.blknos, cw.n, sizeof(BlockNumber), cmp_block);
	*blknos = cw.blknos;
	*nblocks = cw.nblocks;
	return cw.n;
}

/*
 * Did a page of reln under its length change since epoch 'since', as
 * bmap_changed_pages() would find one?  Its length into *nblocks.  The walk
 * ends at the first changed page found, so it reads the map's nodes on one
 * path down, not every node set since: O(the tree's height), not O(changed
 * pages), for a caller that asks only whether the pages are a root's still.
 */
bool
bmap_changed_since(SMgrRelation reln, uint32 since, BlockNumber *nblocks)
{
	ChangedWalk cw = {since, 0, true, NULL, 0, 0};

	changed_walk(reln, &cw);
	*nblocks = cw.nblocks;
	return cw.n > 0;
}

/*
 * Raise reln's epoch, as a capture does, so that each of its pages' next
 * change claims or copies it, setting its entry's epoch: the new epoch.
 */
uint32
bmap_raise_epoch(SMgrRelation reln)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		headerbuf;
	BmapHeader *header;
	xl_bmap_header_delta hdelta;
	xl_bmap_tree xlrec;
	XLogRecPtr	lsn;

	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	header = PageGetBmapHeader(BufferGetPage(headerbuf));
	hdelta.kind = BMAP_DELTA_HEADER;
	hdelta.flags = 0;
	hdelta.nblocks = header->nblocks;
	hdelta.vm_nblocks = header->vm_nblocks;
	hdelta.free_space_unknown = 0;
	hdelta.epoch = header->epoch + 1;
	hdelta.shared_below = 0;
	xlrec.locator = rel;
	xlrec.op = BMAP_TREE_EPOCH;
	xlrec.flags = 0;
	xlrec.node = xlrec.from = InvalidBlockNumber;
	xlrec.old_hw = xlrec.new_hw = InvalidBlockNumber;

	START_CRIT_SECTION();
	apply_header_delta(BufferGetPage(headerbuf), &hdelta);
	MarkBufferDirty(headerbuf);
	XLogBeginInsert();
	XLogRegisterData(&xlrec, sizeof(xlrec));
	XLogRegisterBuffer(0, headerbuf, REGBUF_STANDARD);
	XLogRegisterBufData(0, &hdelta, sizeof(hdelta));
	lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_TREE);
	PageSetLSN(BufferGetPage(headerbuf), lsn);
	END_CRIT_SECTION();
	UnlockReleaseBuffer(headerbuf);
	MarkRelationPagesShared(rel);
	return hdelta.epoch;
}

/*
 * How many of the relation's first pages the free space map may know
 * nothing of (see "The visibility map is mapped too"): VACUUM reads those
 * of them it would skip, to record their free space.  0 for a fork other
 * than the main fork.
 */
BlockNumber
bmapfreespaceunknown(SMgrRelation reln)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		buf;
	BlockNumber n;

	if (!mdexists(reln, BMAP_FORKNUM))
		return 0;
	buf = map_read(rel, 0);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(buf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	n = PageGetBmapHeader(BufferGetPage(buf))->free_space_unknown;
	UnlockReleaseBuffer(buf);
	return n;
}

/*
 * VACUUM recorded the free space of each of the relation's pages below
 * 'upto': if that is every page the header says the free space map may not
 * know, the header says none now.  Logged, so that a standby promoted, or a
 * recovery, doesn't read them all again.  The free space map's pages aren't
 * logged, and a crash may lose what was recorded in them, as it may lose
 * any of VACUUM's records of free space: a hint.
 */
void
bmapfreespaceknown(SMgrRelation reln, BlockNumber upto)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		headerbuf;
	xl_bmap_header_delta hdelta;
	xl_bmap_tree xlrec;
	XLogRecPtr	lsn;
	BlockNumber unknown;

	if (!mdexists(reln, BMAP_FORKNUM))
		return;
	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	unknown = PageGetBmapHeader(BufferGetPage(headerbuf))->free_space_unknown;
	if (unknown == 0 || upto < unknown)
	{
		UnlockReleaseBuffer(headerbuf);
		return;
	}
	header_delta_of(BufferGetPage(headerbuf), &hdelta);
	hdelta.flags = BMAP_DELTA_FREE_SPACE;
	hdelta.free_space_unknown = 0;
	xlrec.locator = rel;
	xlrec.op = BMAP_TREE_FREE_SPACE;
	xlrec.flags = 0;
	xlrec.node = xlrec.from = InvalidBlockNumber;
	xlrec.old_hw = xlrec.new_hw = InvalidBlockNumber;

	START_CRIT_SECTION();
	apply_header_delta(BufferGetPage(headerbuf), &hdelta);
	MarkBufferDirty(headerbuf);
	XLogBeginInsert();
	XLogRegisterData(&xlrec, sizeof(xlrec));
	XLogRegisterBuffer(0, headerbuf, REGBUF_STANDARD);
	XLogRegisterBufData(0, &hdelta, sizeof(hdelta));
	lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_TREE);
	PageSetLSN(BufferGetPage(headerbuf), lsn);
	END_CRIT_SECTION();
	UnlockReleaseBuffer(headerbuf);
	elog(DEBUG1, "free space of the first %u pages of %s is known",
		 unknown, relpathperm(rel, MAIN_FORKNUM).str);
}

/*
 * May root 'id' be a frozen base of relation 'live' (a root its frozen
 * roots are made of, the pages changed since it found by their epochs)?
 * Yes if it was captured of live, frozen (its pages changed since told by
 * the epoch it recorded of live), or if live was made a copy of it, or of
 * a root it is a copy of (told by the root's own epoch, live's when made).
 * The epoch into *since.
 */

bool
bmap_root_base(SMgrRelation live, BmapRootId id, uint32 *since)
{
	RelFileLocator rel = live->smgr_rlocator.locator;
	Buffer		buf;
	xl_bmap_root r;
	BmapRoot	root;

	if (!root_read(rel, id, -1, &buf, &r) || r.live_epoch == 0)
		return false;
	/*
	 * (captured of live's very map, not only of its file number, which a
	 * map dropped may have had)
	 */
	if (r.live_epoch != BMAP_LIVE_EPOCH_COPY && r.relnumber == rel.relNumber &&
		r.live_map == bmap_map_id(live))
	{
		*since = r.live_epoch;
		return true;
	}
	buf = map_read(rel, 1);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(buf), BMAP_PAGE_ROOT, rel, BMAP_FORKNUM, 1);
	root = *PageGetBmapRoot(BufferGetPage(buf));
	UnlockReleaseBuffer(buf);
	if (root.made_of != id)
	{
		Buffer		mbuf;
		xl_bmap_root m;

		/*
		 * (or a copy of the root live was made of, bmap_root_copy()'s: the
		 * same subtrees, as of the same epoch)
		 */
		if (root.made_of == 0 ||
			!root_read(rel, root.made_of, -1, &mbuf, &m) ||
			m.snap.epoch != r.snap.epoch || m.snap.nblocks != r.snap.nblocks ||
			m.snap.vm_nblocks != r.snap.vm_nblocks ||
			memcmp(m.snap.tops, r.snap.tops, sizeof(m.snap.tops)) != 0)
			return false;
	}
	*since = r.snap.epoch;
	return true;
}

/*
 * The epoch root 'id' of the page store of 'store' was captured at (its
 * relation's, raised by the capture: the pages changed since have entries
 * of it or later), into *epoch; false if the store holds no such root.
 */
bool
bmap_root_epoch(RelFileLocator store, BmapRootId id, uint32 *epoch)
{
	Buffer		buf;
	xl_bmap_root r;

	if (!root_read(store, id, -1, &buf, &r))
		return false;
	*epoch = r.snap.epoch;
	return true;
}

/*
 * The database's datfrozenxid and datminmxid as root 'id' of the store was
 * captured (a copy of a root keeps its original's), no newer than any
 * transaction or multixact its pages name unfrozen: false if the store
 * holds no such root.
 */
bool
bmap_root_horizon(RelFileLocator store, BmapRootId id,
				  TransactionId *frozenxid, MultiXactId *minmxid)
{
	Buffer		buf;
	xl_bmap_root r;

	if (!root_read(store, id, -1, &buf, &r))
		return false;
	*frozenxid = r.frozenxid;
	*minmxid = r.minmxid;
	return true;
}

/* reln's map's identity (its map_id). */
static uint64
bmap_map_id(SMgrRelation reln)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		buf = map_read(rel, 1);
	uint64		id;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(buf), BMAP_PAGE_ROOT, rel, BMAP_FORKNUM, 1);
	id = PageGetBmapRoot(BufferGetPage(buf))->map_id;
	UnlockReleaseBuffer(buf);
	return id;
}

/* reln's map's lineage (see root_lineage()). */
uint64
bmap_lineage(SMgrRelation reln)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		buf = map_read(rel, 1);
	uint64		lineage;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(buf), BMAP_PAGE_ROOT, rel, BMAP_FORKNUM, 1);
	lineage = root_lineage(PageGetBmapRoot(BufferGetPage(buf)));
	UnlockReleaseBuffer(buf);
	return lineage;
}

/*
 * Was root 'id' captured of reln's lineage: of reln, of storage reln took
 * the place of by carrying its lineage on (a restore), or of a copy of
 * either's pages frozen as of them?  False if the store holds no such root.
 */
bool
bmap_root_of(SMgrRelation reln, BmapRootId id)
{
	Buffer		buf;
	xl_bmap_root r;

	if (!root_read(reln->smgr_rlocator.locator, id, -1, &buf, &r))
		return false;
	return r.lineage != 0 && r.lineage == bmap_lineage(reln);
}

/* The captured root reln's tree was made a copy of (0: none). */
BmapRootId
bmap_made_of(SMgrRelation reln)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		buf = map_read(rel, 1);
	BmapRootId	id;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(buf), BMAP_PAGE_ROOT, rel, BMAP_FORKNUM, 1);
	id = PageGetBmapRoot(BufferGetPage(buf))->made_of;
	UnlockReleaseBuffer(buf);
	return id;
}

/*
 * Diffs (bmap_diff()): the logical blocks two maps of a page store map to
 * different physical blocks, a block mapped under one's length and not
 * under the other's counting as different too.  Two maps hold a page in
 * common only by holding its physical block, and a node in common only by
 * pointing to it: a change under a node another map holds copies the path
 * down to the leaf first (tree_copy_node()), so the copy, a node of another
 * incarnation, is what the changed map points to from then on.  A pointer
 * naming the same incarnation in both maps (the same block, born at the
 * same record) is therefore the same subtree in both, which maps every
 * logical block it covers to the same physical block, and the walk skips
 * it without reading it.  It reads only the nodes whose pointers differ,
 * which are those above the pages that differ, and, where the lengths
 * differ, those mapping the blocks between them.
 *
 * Either side may be a relation's map (its header's length and root page's
 * pointers, read under share locks, as a lookup reads them) or a root
 * captured into the store (its slot's length and pointers).  Each node is
 * read under a share lock, one at a time, with no lock held between them,
 * as a lookup reads them (tree_find()).  A relation may change meanwhile:
 * a node it holds alone changes in place under its buffer lock, and one it
 * shares is copied, not changed, so a change gives a mixed result, each
 * block as it was before the change or as it is after.  A node freed and
 * made something else under the walk (a truncation or a drop of the
 * relation, a root let go of) isn't the incarnation its pointer names, and
 * the diff begins again from both sides as they are then, as a lookup
 * does; only a node that still isn't after TREE_FIND_TRIES walks is an
 * error.  The caller holds the roots, which a diff can't stand in for.
 */

/* one side of a diff: a tree's pointers and the length its blocks count to */
typedef struct DiffSide
{
	BmapEntry	top[BMAP_TREE_TOPS];
	BlockNumber nblocks;
} DiffSide;

typedef struct DiffWalk
{
	RelFileLocator store;
	BlockNumber nblocks[2];
	BlockNumber *blknos;
	uint32		n;
	uint32		cap;
	bool		mismatch;		/* a node wasn't the one its pointer named */
	BlockNumber mismatched;		/* (its block) */
} DiffWalk;

static void
diff_add(DiffWalk *dw, BlockNumber blkno)
{
	if (dw->n == dw->cap)
	{
		dw->cap = Max(dw->cap * 2, 64);
		dw->blknos = dw->blknos ? repalloc_array(dw->blknos, BlockNumber, dw->cap) :
			palloc_array(BlockNumber, dw->cap);
	}
	dw->blknos[dw->n++] = blkno;
}

/*
 * The entries of node 'ptr', of 'level', mapping from 'first', into
 * entries[] (BMAP_TREE_FANOUT of them), read under a share lock.  False,
 * the walk marked to begin again, if the block isn't that node any more.
 */
static bool
diff_read_node(DiffWalk *dw, const BmapEntry *ptr, uint16 level,
			   BlockNumber first, BmapEntry *entries)
{
	Buffer		buf;
	bool		is;

	INJECTION_POINT("bmap-diff-node", NULL);
	buf = tree_read(dw->store, ptr->block, RBM_NORMAL);

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	is = tree_node_is(BufferGetPage(buf), level, first, ptr->since);
	if (is)
		memcpy(entries, PageGetBmapEntries(BufferGetPage(buf)),
			   BMAP_TREE_FANOUT * sizeof(BmapEntry));
	else
	{
		dw->mismatch = true;
		dw->mismatched = ptr->block;
	}
	UnlockReleaseBuffer(buf);
	CHECK_FOR_INTERRUPTS();
	return is;
}

/*
 * The logical blocks under nodes ptr[0] and ptr[1] (NULL: that side has
 * none there), both of 'level' and mapping from 'first', whose physical
 * blocks differ, added in ascending order.  Nodes the pointers name alike
 * are the same subtree, skipped unless part of it lies between the two
 * lengths, where a block one side maps and the other doesn't count; past
 * both lengths nothing counts.
 */
static void
diff_walk_node(DiffWalk *dw, const BmapEntry *const *ptr, uint16 level,
			   BlockNumber first)
{
	uint64		end = (uint64) first + tree_span(level);
	BlockNumber lo = Min(dw->nblocks[0], dw->nblocks[1]);
	BlockNumber hi = Max(dw->nblocks[0], dw->nblocks[1]);
	bool		same;
	BmapEntry  *entries[2] = {NULL, NULL};

	if (first >= hi)
		return;
	same = ptr[0] != NULL && ptr[1] != NULL &&
		ptr[0]->block == ptr[1]->block && ptr[0]->since == ptr[1]->since;
	/* (a subtree both share counts only where it maps between the lengths) */
	if (same && (end <= lo || lo == hi))
		return;

	for (int s = 0; s < 2; s++)
	{
		if (ptr[s] == NULL)
			continue;
		entries[s] = palloc(BMAP_TREE_FANOUT * sizeof(BmapEntry));
		/* (the same node is read once, for both) */
		if (s == 1 && same)
			memcpy(entries[1], entries[0], BMAP_TREE_FANOUT * sizeof(BmapEntry));
		else if (!diff_read_node(dw, ptr[s], level, first, entries[s]))
			goto done;
	}

	for (uint32 i = 0; i < BMAP_TREE_FANOUT; i++)
	{
		uint64		child = (uint64) first + (level == 0 ? i :
											   i * tree_span(level - 1));

		if (child >= hi)
			break;
		if (level == 0)
		{
			BlockNumber phys[2];

			for (int s = 0; s < 2; s++)
				phys[s] = entries[s] != NULL && child < dw->nblocks[s] ?
					entries[s][i].block : InvalidBlockNumber;
			if (phys[0] != phys[1])
				diff_add(dw, (BlockNumber) child);
		}
		else
		{
			const BmapEntry *next[2];

			for (int s = 0; s < 2; s++)
				next[s] = entries[s] != NULL &&
					BlockNumberIsValid(entries[s][i].block) ?
					&entries[s][i] : NULL;
			if (next[0] != NULL || next[1] != NULL)
				diff_walk_node(dw, next, level - 1, (BlockNumber) child);
			if (dw->mismatch)
				break;
		}
	}
done:
	for (int s = 0; s < 2; s++)
		if (entries[s] != NULL)
			pfree(entries[s]);
}

/*
 * diff_walk_node() from each pair of the sides' subtrees.  False if a node
 * wasn't the one its pointer named, nothing kept: the diff begins again.
 */
static bool
diff_walk(RelFileLocator store, const DiffSide *a, const DiffSide *b,
		  BlockNumber **blknos, uint32 *n, BlockNumber *mismatched)
{
	DiffWalk	dw = {0};
	uint64		first = 0;

	dw.store = store;
	dw.nblocks[0] = a->nblocks;
	dw.nblocks[1] = b->nblocks;
	for (int k = 0; k < BMAP_TREE_TOPS && !dw.mismatch; k++)
	{
		const BmapEntry *ptr[2];

		ptr[0] = BlockNumberIsValid(a->top[k].block) ? &a->top[k] : NULL;
		ptr[1] = BlockNumberIsValid(b->top[k].block) ? &b->top[k] : NULL;
		if (first >= Max(a->nblocks, b->nblocks))
			break;
		if (ptr[0] != NULL || ptr[1] != NULL)
			diff_walk_node(&dw, ptr, k, (BlockNumber) first);
		first += tree_span(k);
	}
	if (dw.mismatch)
	{
		if (dw.blknos)
			pfree(dw.blknos);
		*mismatched = dw.mismatched;
		return false;
	}
	*blknos = dw.blknos;
	*n = dw.n;
	return true;
}

/* a diff's walks have found a node not the one its pointer named too often */
pg_noreturn static void
diff_mismatched(RelFileLocator store, BlockNumber block)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("block %u of page store \"%s\" is not the node of a block map it was",
					block,
					relpathperm(with_relnumber(store, BMAP_STORE_RELNUMBER),
								MAIN_FORKNUM).str)));
}

/* reln's side of a diff: its header's length and its root page's pointers */
static void
diff_side_of(SMgrRelation reln, DiffSide *side)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		headerbuf;
	Buffer		buf;

	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	side->nblocks = PageGetBmapHeader(BufferGetPage(headerbuf))->nblocks;
	buf = map_read(rel, 1);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(buf), BMAP_PAGE_ROOT, rel, BMAP_FORKNUM, 1);
	memcpy(side->top, PageGetBmapRoot(BufferGetPage(buf))->top,
		   sizeof(side->top));
	UnlockReleaseBuffer(buf);
	UnlockReleaseBuffer(headerbuf);
}

/*
 * Root 'id''s side of a diff: the length and pointers its slot holds.  It
 * must be held as this transaction sees it, as a root let go of may have
 * had its nodes freed.
 */
static void
diff_side_of_root(RelFileLocator store, BmapRootId id, DiffSide *side)
{
	Buffer		buf;
	xl_bmap_root r;

	if (!root_read(store, id, -1, &buf, &r) || !root_held_as_seen(&r))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("page store of database %u holds no captured root %llu",
						store.dbOid, (unsigned long long) id)));
	side->nblocks = r.snap.nblocks;
	memcpy(side->top, snapshot_tops(&r.snap), sizeof(side->top));
}

/* Are two relations' (or a relation's and a root's) maps of one page store? */
static void
diff_check_store(RelFileLocator a, RelFileLocator b)
{
	if (a.spcOid != b.spcOid || a.dbOid != b.dbOid)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("block maps of different page stores can't be compared")));
}

/*
 * The logical blocks a's and b's maps, of one page store, map to different
 * physical blocks, or that one maps under its length and the other doesn't
 * under its own, into *blknos (palloc'd, ascending): how many.  Blocks
 * past both lengths don't count.  A change of either relation meanwhile
 * gives a mixed result, each block as before the change or as after, not
 * an error (see the walk's comment above).  It reads each map's header and
 * root page, and the nodes whose pointers differ between the maps: those on
 * the paths down to the pages that differ, which copy-on-write made each map's own, and those
 * mapping blocks between the two lengths.  So it costs O(the pages that
 * differ) map pages, at most the tree's height for each, none but the
 * headers and root pages for maps that share every subtree (a share and
 * its source before either changes), whatever the maps' sizes.
 */
uint32
bmap_diff(SMgrRelation a, SMgrRelation b, BlockNumber **blknos)
{
	RelFileLocator store = a->smgr_rlocator.locator;
	BlockNumber mismatched = InvalidBlockNumber;
	uint32		n;

	diff_check_store(store, b->smgr_rlocator.locator);
	for (int tries = 0; tries < TREE_FIND_TRIES; tries++)
	{
		DiffSide	sa;
		DiffSide	sb;

		diff_side_of(a, &sa);
		diff_side_of(b, &sb);
		if (diff_walk(store, &sa, &sb, blknos, &n, &mismatched))
			return n;
	}
	diff_mismatched(store, mismatched);
}

/*
 * bmap_diff() of a's map and root 'id' of its page store, held as this
 * transaction sees it and kept held by the caller: the blocks a changed
 * since the root was captured of it, for one.  Every change of a page the
 * root holds copies it first, VACUUM's included: a page shared only with
 * roots of the relation is vacuumed as the relation's own, which copies it
 * and leaves the root its version, so it is found here like any other.
 */
uint32
bmap_diff_root(SMgrRelation a, BmapRootId id, BlockNumber **blknos)
{
	RelFileLocator store = a->smgr_rlocator.locator;
	BlockNumber mismatched = InvalidBlockNumber;
	uint32		n;

	for (int tries = 0; tries < TREE_FIND_TRIES; tries++)
	{
		DiffSide	sa;
		DiffSide	sb;

		diff_side_of_root(store, id, &sb);
		diff_side_of(a, &sa);
		if (diff_walk(store, &sa, &sb, blknos, &n, &mismatched))
			return n;
	}
	diff_mismatched(store, mismatched);
}

/* bmap_diff() of roots 'a' and 'b' of the page store of 'store' */
uint32
bmap_diff_roots(RelFileLocator store, BmapRootId a, BmapRootId b,
				BlockNumber **blknos)
{
	BlockNumber mismatched = InvalidBlockNumber;
	uint32		n;

	for (int tries = 0; tries < TREE_FIND_TRIES; tries++)
	{
		DiffSide	sa;
		DiffSide	sb;

		diff_side_of_root(store, a, &sa);
		diff_side_of_root(store, b, &sb);
		if (diff_walk(store, &sa, &sb, blknos, &n, &mismatched))
			return n;
	}
	diff_mismatched(store, mismatched);
}

/*
 * The roots' counts.  A sweep counts what every holder holds, and captured
 * roots may be many, one for each commit of a version history, holding
 * much the same blocks: walked anew by each sweep, they would make each
 * sweep cost as much as all of them.  So what they hold stays counted, in
 * each reference-count page's roots' counts (PageGetBmapRootRefs()): for
 * each block, how many of the roots' pages point to it, and whether it is
 * a node the roots reach (ROOTREF_IN), whose own pointers are then counted
 * too, once however many reach it.  The roots' pages are the slots counted
 * (a slot's 'counted'), the nodes they reach, and the release queue's
 * entries carrying a pointer counted (BMAP_QUEUED_COUNTED): a counted
 * root's release pushes its pointers so, and a drain freeing a node the
 * roots reach pushes its children so.  A sweep adds the roots captured
 * since the last (the pages of their slots marked in the roots' metapage),
 * walking only the nodes no counted root reaches yet (root_count_node());
 * a drain takes off what the pointers it lets go of counted, as it lets go
 * of them (release_step()).  A sweep then begins its holders from the
 * roots' counts and walks the relations, the release queue and the roots
 * not yet counted: its cost has no part that grows with the roots held.
 *
 * A node the roots no longer reach (its roots' count gone to zero) while
 * another map still holds it leaves what they hold, and its pointers'
 * counts are to be taken off.  It is a page of the other map, which may
 * change it in place once it holds it alone, so the hold the roots had is
 * kept, by an entry pushed below entries pointing to its children
 * (BMAP_QUEUED_UNCOUNT, holding nothing), each taking one off its child's
 * count and a node child left unreached handled the same way above it,
 * all before the hold under them is let go of.  Until they are, a sweep
 * takes them off the counts it begins from.
 *
 * Every change of the counts is in the record of the change that makes
 * it, but a sweep's, which takes several: it first says, in the roots'
 * metapage, that the counts are not whole ('counted' cleared), then writes
 * them and the slots it counted, then says they are whole again.  A sweep
 * that finds them not whole (another failed or crashed between those
 * records, or none has counted them yet) counts every root and every
 * counted entry of the queue anew (as a sweep counted the roots before the
 * counts were kept) and writes the counts it finds.  A root is counted only
 * once its capture can no longer be abandoned (its capturer committed) and
 * nothing is releasing it (whose release would push pointers not
 * counted): until then it counts in each sweep's holders alone.  A count
 * has 31 bits, past what the roots of any version history give one block;
 * one that would pass ROOTREF_MAX stays there, leaving the counts not
 * whole, each sweep then counting anew and taking the block for held
 * however many pointers it takes off.
 */
typedef struct RootCounts
{
	BlockNumber n;				/* blocks covered: the reference-count pages' */
	uint32	   *v;				/* each block's roots' count, as counted */
	bool		overflow;		/* a count passed ROOTREF_MAX */
	BmapEntry  *extra;			/* tops of the roots not counted yet */
	int			nextra;
} RootCounts;

/* One more of the roots' pages pointing to block b. */
static void
root_count_add(RootCounts *rc, BlockNumber b)
{
	if (b >= rc->n || ROOTREF_COUNT(rc->v[b]) == ROOTREF_MAX)
	{
		rc->overflow = true;
		return;
	}
	rc->v[b]++;
}

/*
 * A pointer of the roots' pages to node 'ptr', counted, and if no counted
 * root reached the node before, its own pointers too: a leaf's to each
 * block it maps once, however many of its entries map it, as the release
 * queue lets go of them (queued_children()).
 */
static void
root_count_node(RelFileLocator rel, const BmapEntry *ptr, uint16 level,
				BlockNumber first, RootCounts *rc)
{
	BlockNumber b = ptr->block;
	BmapEntry  *entries;
	Buffer		buf;

	root_count_add(rc, b);
	if (b >= rc->n || (rc->v[b] & ROOTREF_IN))
		return;
	rc->v[b] |= ROOTREF_IN;
	entries = palloc(BMAP_TREE_FANOUT * sizeof(BmapEntry));
	buf = tree_read(rel, b, RBM_NORMAL);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	if (!tree_node_is(BufferGetPage(buf), level, first, ptr->since))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("block %u of the page store of database %u is not the node a captured root holds",
						b, rel.dbOid)));
	memcpy(entries, PageGetBmapEntries(BufferGetPage(buf)),
		   BMAP_TREE_FANOUT * sizeof(BmapEntry));
	UnlockReleaseBuffer(buf);
	CHECK_FOR_INTERRUPTS();
	if (level == 0)
	{
		BlockNumber *blocks = palloc(BMAP_TREE_FANOUT * sizeof(BlockNumber));
		uint32		n = 0;

		for (uint32 i = 0; i < BMAP_TREE_FANOUT; i++)
			if (BlockNumberIsValid(entries[i].block))
				blocks[n++] = entries[i].block;
		qsort(blocks, n, sizeof(BlockNumber), cmp_block);
		for (uint32 i = 0; i < n; i++)
			if (i == 0 || blocks[i] != blocks[i - 1])
				root_count_add(rc, blocks[i]);
		pfree(blocks);
	}
	else
	{
		for (uint32 i = 0; i < BMAP_TREE_FANOUT; i++)
			if (BlockNumberIsValid(entries[i].block))
				root_count_node(rel, &entries[i], level - 1,
								(BlockNumber) Min((uint64) first +
												  i * tree_span(level - 1),
												  (uint64) MaxBlockNumber),
								rc);
	}
	pfree(entries);
}

/* a root's subtrees counted, as its slot's pointers */
static void
root_count_tops(RelFileLocator rel, const BmapEntry *tops, RootCounts *rc)
{
	uint64		first = 0;

	for (int k = 0; k < BMAP_TREE_TOPS; k++)
	{
		if (BlockNumberIsValid(tops[k].block))
			root_count_node(rel, &tops[k], k, (BlockNumber) first, rc);
		first += tree_span(k);
	}
}

/* A change of the roots' metapage alone: 'flags', of page 'naming'. */
static void
roots_meta_change(RelFileLocator rel, uint16 flags, uint32 naming,
				  bool counted)
{
	Buffer		metabuf = roots_read(rel, 0);
	BmapRootsMeta *meta;
	xl_bmap_roots_meta_delta mdelta;
	xl_bmap_tree xlrec;
	XLogRecPtr	lsn;

	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(metabuf), BMAP_PAGE_ROOTS_META,
			   with_relnumber(rel, BMAP_ROOTS_RELNUMBER), MAIN_FORKNUM, 0);
	meta = PageGetBmapRootsMeta(BufferGetPage(metabuf));
	memset(&mdelta, 0, sizeof(mdelta));
	mdelta.kind = BMAP_DELTA_ROOTS_META;
	mdelta.flags = flags;
	mdelta.npages = meta->npages;
	mdelta.hint = meta->hint;
	mdelta.seq = meta->seq;
	mdelta.naming = naming;
	mdelta.counted = counted ? 1 : 0;
	xlrec.locator = with_relnumber(rel, BMAP_ROOTS_RELNUMBER);
	xlrec.op = BMAP_TREE_ROOT_SLOT;
	xlrec.flags = 0;
	xlrec.node = xlrec.from = InvalidBlockNumber;
	xlrec.old_hw = xlrec.new_hw = InvalidBlockNumber;

	START_CRIT_SECTION();
	apply_roots_meta_delta(BufferGetPage(metabuf), &mdelta);
	MarkBufferDirty(metabuf);
	XLogBeginInsert();
	XLogRegisterData(&xlrec, sizeof(xlrec));
	XLogRegisterBuffer(0, metabuf, REGBUF_STANDARD);
	XLogRegisterBufData(0, &mdelta, sizeof(mdelta));
	lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_TREE);
	PageSetLSN(BufferGetPage(metabuf), lsn);
	END_CRIT_SECTION();
	UnlockReleaseBuffer(metabuf);
}

/*
 * Write the roots' counts of reference-count page 'p' that differ from
 * what it has, 'was': one record of the page.
 */
static void
root_counts_write(RelFileLocator rel, BlockNumber p, const uint32 *was,
				  const uint32 *v)
{
	xl_bmap_refs_delta *delta;
	xl_bmap_ref_delta *ref;
	xl_bmap_unref xlrec;
	Buffer		buf;
	XLogRecPtr	lsn;

	if (memcmp(was, v, BMAP_REFS_PER_PAGE * sizeof(uint32)) == 0)
		return;
	delta = palloc0(sizeof(xl_bmap_refs_delta) +
					BMAP_REFS_PER_PAGE * sizeof(xl_bmap_ref_delta));
	ref = (xl_bmap_ref_delta *) (delta + 1);
	delta->kind = BMAP_DELTA_REFS;
	for (uint32 i = 0; i < BMAP_REFS_PER_PAGE; i++)
	{
		if (was[i] == v[i])
			continue;
		ref[delta->nroots].slot = i;
		ref[delta->nroots].count = v[i];
		delta->nroots++;
	}
	if (delta->nroots == 0)
	{
		pfree(delta);
		return;
	}
	buf = refs_read(rel, p);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	check_refs_page(BufferGetPage(buf), rel, p);
	if (PageIsNew(BufferGetPage(buf)))
		delta->flags |= BMAP_DELTA_INIT;
	xlrec.locator = rel;
	xlrec.flags = BMAP_UNREF_ROOTS;

	START_CRIT_SECTION();
	apply_refs_delta(BufferGetPage(buf), delta);
	MarkBufferDirty(buf);
	XLogBeginInsert();
	XLogRegisterData(&xlrec, sizeof(xlrec));
	XLogRegisterBuffer(0, buf, REGBUF_STANDARD |
					   ((delta->flags & BMAP_DELTA_INIT) ? REGBUF_WILL_INIT : 0));
	XLogRegisterBufData(0, delta, refs_delta_size(delta));
	lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_UNREF);
	PageSetLSN(BufferGetPage(buf), lsn);
	END_CRIT_SECTION();
	UnlockReleaseBuffer(buf);
	pfree(delta);
}

/*
 * Bring rel's store's roots' counts up to date, under the sweep's lock
 * (held exclusively: nothing is captured, let go of or reclaimed
 * meanwhile, and no drain runs), for a sweep whose store's high-water
 * mark is 'hw': the roots captured since the last sweep added, or, if the
 * counts aren't whole, every root counted anew.  Into *rc the counts, and
 * the roots not counted yet (held by a transaction still running), whose
 * holders the sweep counts itself.
 *
 * With 'check', for a test, nothing is written: the roots counted are
 * counted anew, and how many blocks' counts differ from what they have is
 * returned.
 */
static uint64
root_counts(RelFileLocator rel, BlockNumber hw, RootCounts *rc, bool check)
{
	Buffer		metabuf;
	BmapRootsMeta meta;
	uint8	   *marks;
	bool		whole;
	BlockNumber nrefpages;
	uint32	   *was;
	List	   *tomark = NIL;	/* slots to say are counted */
	List	   *unmark = NIL;	/* pages of slots all counted */
	int			extra_cap = 0;
	bool		changed;
	uint64		differ = 0;

	memset(rc, 0, sizeof(*rc));
	if (!check)
		refs_cover(rel, hw);
	nrefpages = refs_nblocks(rel);
	rc->n = nrefpages * BMAP_REFS_PER_PAGE;
	rc->v = MemoryContextAllocHuge(CurrentMemoryContext,
								   Max(rc->n, 1) * sizeof(uint32));
	memset(rc->v, 0, Max(rc->n, 1) * sizeof(uint32));
	if (!roots_exist(rel))
		return 0;
	metabuf = roots_read(rel, 0);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	if (PageIsNew(BufferGetPage(metabuf)))
	{
		UnlockReleaseBuffer(metabuf);
		return 0;
	}
	check_page(BufferGetPage(metabuf), BMAP_PAGE_ROOTS_META,
			   with_relnumber(rel, BMAP_ROOTS_RELNUMBER), MAIN_FORKNUM, 0);
	meta = *PageGetBmapRootsMeta(BufferGetPage(metabuf));
	marks = palloc((meta.npages + 7) / 8);
	memcpy(marks, PageGetBmapRootsUncounted(BufferGetPage(metabuf)),
		   (meta.npages + 7) / 8);
	UnlockReleaseBuffer(metabuf);
	whole = meta.counted != 0 && !check;

	/* the counts as they are, kept if whole */
	was = MemoryContextAllocHuge(CurrentMemoryContext,
								 Max(rc->n, 1) * sizeof(uint32));
	for (BlockNumber p = 0; p < nrefpages; p++)
	{
		Buffer		buf = refs_read(rel, p);

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		check_refs_page(BufferGetPage(buf), rel, p);
		if (PageIsNew(BufferGetPage(buf)))
			memset(was + p * BMAP_REFS_PER_PAGE, 0,
				   BMAP_REFS_PER_PAGE * sizeof(uint32));
		else
			memcpy(was + p * BMAP_REFS_PER_PAGE,
				   PageGetBmapRootRefs(BufferGetPage(buf)),
				   BMAP_REFS_PER_PAGE * sizeof(uint32));
		UnlockReleaseBuffer(buf);
	}
	if (whole)
		memcpy(rc->v, was, rc->n * sizeof(uint32));

	/*
	 * The slots: those of the pages marked, or, the counts not whole, all
	 * of them, counted anew.
	 */
	for (uint32 p = 0; p < meta.npages; p++)
	{
		xl_bmap_root page[BMAP_ROOTS_PER_PAGE];
		Buffer		buf;
		bool		marked = (marks[p / 8] & (1 << (p % 8))) != 0;
		bool		left = false;

		if (whole && !marked)
			continue;
		buf = roots_read(rel, 1 + p);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		check_page(BufferGetPage(buf), BMAP_PAGE_ROOTS,
				   with_relnumber(rel, BMAP_ROOTS_RELNUMBER), MAIN_FORKNUM, 1 + p);
		memcpy(page, PageGetBmapRoots(BufferGetPage(buf)), sizeof(page));
		UnlockReleaseBuffer(buf);
		for (uint32 i = 0; i < BMAP_ROOTS_PER_PAGE; i++)
		{
			RootState	state = root_state(&page[i]);

			if (state == ROOT_FREE)
				continue;
			if (page[i].counted)
			{
				if (!whole)
					root_count_tops(rel, snapshot_tops(&page[i].snap), rc);
				continue;
			}
			if (check)
				continue;
			if (state == ROOT_HELD)
			{
				root_count_tops(rel, snapshot_tops(&page[i].snap), rc);
				tomark = lappend_int(tomark, (int) (p * BMAP_ROOTS_PER_PAGE + i));
				continue;
			}
			/* (pending or abandoned: the sweep's holders alone) */
			if (rc->nextra == extra_cap)
			{
				extra_cap = Max(extra_cap * 2, 8);
				rc->extra = rc->extra ?
					repalloc_array(rc->extra, BmapEntry, extra_cap * BMAP_TREE_TOPS) :
					palloc_array(BmapEntry, extra_cap * BMAP_TREE_TOPS);
			}
			memcpy(rc->extra + rc->nextra * BMAP_TREE_TOPS,
				   snapshot_tops(&page[i].snap), sizeof(BmapEntry) * BMAP_TREE_TOPS);
			rc->nextra++;
			left = true;
		}
		if (marked && !left)
			unmark = lappend_int(unmark, (int) p);
	}

	/*
	 * Counted anew, the queue's counted entries too, as they hold (those of
	 * a releaser still running count as their slot), and the pointers still
	 * to be taken off, one each.
	 */
	if (!whole)
	{
		uint32		n;
		BmapQueued *entries = queue_entries(rel, &n);

		for (uint32 i = 0; i < n; i++)
		{
			FullTransactionId owner = queued_owner(&entries[i]);
			BmapEntry	ptr;

			if (entries[i].flags & BMAP_QUEUED_UNCOUNT)
			{
				root_count_add(rc, entries[i].block);
				continue;
			}
			if ((entries[i].flags & BMAP_QUEUED_COUNTED) == 0 ||
				(FullTransactionIdIsValid(owner) &&
				 owner_state(owner) != OWNER_COMMITTED))
				continue;
			if (entries[i].level == BMAP_QUEUED_BLOCK)
			{
				root_count_add(rc, entries[i].block);
				continue;
			}
			ptr.block = entries[i].block;
			ptr.epoch = 0;
			ptr.since = entries[i].since;
			root_count_node(rel, &ptr, entries[i].level, entries[i].first, rc);
		}
		pfree(entries);
	}

	if (check)
	{
		for (BlockNumber b = 0; b < rc->n; b++)
			differ += was[b] != rc->v[b];
		pfree(marks);
		pfree(was);
		return differ;
	}

	/*
	 * Written, if they changed: said not to be whole first, so that a sweep
	 * failing before the end leaves them to be counted anew.
	 */
	changed = !whole || tomark != NIL || unmark != NIL ||
		memcmp(was, rc->v, rc->n * sizeof(uint32)) != 0;
	if (changed)
	{
		if (whole)
			roots_meta_change(rel, BMAP_DELTA_COUNTED, 0, false);
		for (BlockNumber p = 0; p < nrefpages; p++)
			root_counts_write(rel, p, was + p * BMAP_REFS_PER_PAGE,
							  rc->v + p * BMAP_REFS_PER_PAGE);
		INJECTION_POINT("bmap-sweep-root-counts", NULL);
		foreach_int(slot, tomark)
		{
			Buffer		buf = roots_read(rel, ROOT_PAGE(slot));
			xl_bmap_root r;

			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
			r = PageGetBmapRoots(BufferGetPage(buf))[ROOT_INDEX(slot)];
			r.counted = 1;
			roots_set_slot(rel, buf, (uint32) slot, &r, InvalidBuffer);
			UnlockReleaseBuffer(buf);
		}
		foreach_int(p, unmark)
			roots_meta_change(rel, BMAP_DELTA_UNCOUNTED_CLEAR, (uint32) p + 1,
							  false);
		if (!rc->overflow)
			roots_meta_change(rel, BMAP_DELTA_COUNTED, 0, true);
	}
	list_free(tomark);
	list_free(unmark);
	pfree(marks);
	pfree(was);
	return 0;
}

/*
 * Whether the roots' counts of a store are whole, how many roots they
 * count, and how many blocks' counts differ from the roots counted anew:
 * none, if they are kept as they should be.  Under the sweep's lock, so
 * that nothing changes them meanwhile.  For tests.
 */
bool
bmap_inspect_root_counts(RelFileLocator store, uint64 *counted,
						 uint64 *differ)
{
	LOCKTAG		tag;
	bool		whole = false;
	Buffer		metabuf;
	RootCounts	rc;

	*counted = *differ = 0;
	if (!alloc_exists(store) || !roots_exist(store))
		return false;
	SET_LOCKTAG_OBJECT(tag, store.dbOid, BMAP_SWEEP_CLASSID, store.spcOid, 0);
	(void) LockAcquire(&tag, ExclusiveLock, false, false);
	metabuf = roots_read(store, 0);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	if (!PageIsNew(BufferGetPage(metabuf)))
	{
		BmapRootsMeta meta = *PageGetBmapRootsMeta(BufferGetPage(metabuf));

		whole = meta.counted != 0;
		UnlockReleaseBuffer(metabuf);
		for (uint32 p = 0; p < meta.npages; p++)
		{
			Buffer		buf = roots_read(store, 1 + p);

			LockBuffer(buf, BUFFER_LOCK_SHARE);
			for (uint32 i = 0; i < BMAP_ROOTS_PER_PAGE; i++)
			{
				const xl_bmap_root *r = &PageGetBmapRoots(BufferGetPage(buf))[i];

				if (root_state(r) != ROOT_FREE && r->counted)
					(*counted)++;
			}
			UnlockReleaseBuffer(buf);
		}
		*differ = root_counts(store, 0, &rc, true);
		pfree(rc.v);
		if (rc.extra)
			pfree(rc.extra);
	}
	else
		UnlockReleaseBuffer(metabuf);
	LockRelease(&tag, ExclusiveLock, false);
	return whole;
}

/*
 * The roots a store still counts (not free), read once for a sweep's
 * lowerings of every relation's shared_below (own_root_tops()), rather than
 * every page of slots once for each relation: by map, then by id.  The
 * sweep's lock keeps any root from being captured or let go of meanwhile.
 */
typedef struct SweepRoot
{
	uint64		map_id;			/* of the map it was captured of */
	BmapRootId	id;
	BmapEntry	tops[BMAP_TREE_TOPS];
} SweepRoot;

typedef struct SweepRoots
{
	RelFileLocator rel;			/* the store */
	bool		loaded;
	SweepRoot  *by_map;			/* sorted by map_id */
	SweepRoot **by_id;			/* the same, sorted by id */
	int			n;
} SweepRoots;

static int
cmp_root_map(const void *a, const void *b)
{
	const SweepRoot *ra = a;
	const SweepRoot *rb = b;

	return pg_cmp_u64(ra->map_id, rb->map_id);
}

static int
cmp_root_id(const void *a, const void *b)
{
	const SweepRoot *ra = *(SweepRoot *const *) a;
	const SweepRoot *rb = *(SweepRoot *const *) b;

	return pg_cmp_u64(ra->id, rb->id);
}

static void
sweep_roots_load(SweepRoots *sr)
{
	RelFileLocator rel = sr->rel;
	BmapRootsMeta meta;
	Buffer		metabuf;
	int			cap = 0;

	if (sr->loaded)
		return;
	sr->loaded = true;
	if (!roots_exist(rel))
		return;
	metabuf = roots_read(rel, 0);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	if (PageIsNew(BufferGetPage(metabuf)))
	{
		UnlockReleaseBuffer(metabuf);
		return;
	}
	meta = *PageGetBmapRootsMeta(BufferGetPage(metabuf));
	UnlockReleaseBuffer(metabuf);
	for (uint32 p = 0; p < meta.npages; p++)
	{
		Buffer		buf = roots_read(rel, 1 + p);

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		for (uint32 i = 0; i < BMAP_ROOTS_PER_PAGE; i++)
		{
			const xl_bmap_root *r = &PageGetBmapRoots(BufferGetPage(buf))[i];

			if (root_state(r) == ROOT_FREE)
				continue;
			if (sr->n == cap)
			{
				cap = Max(cap * 2, 64);
				sr->by_map = sr->by_map ? repalloc_array(sr->by_map, SweepRoot, cap) :
					palloc_array(SweepRoot, cap);
			}
			sr->by_map[sr->n].map_id = r->map_id;
			sr->by_map[sr->n].id = r->snap.id;
			memcpy(sr->by_map[sr->n].tops, snapshot_tops(&r->snap),
				   sizeof(BmapEntry) * BMAP_TREE_TOPS);
			sr->n++;
		}
		UnlockReleaseBuffer(buf);
	}
	if (sr->n == 0)
		return;
	qsort(sr->by_map, sr->n, sizeof(SweepRoot), cmp_root_map);
	sr->by_id = palloc_array(SweepRoot *, sr->n);
	for (int i = 0; i < sr->n; i++)
		sr->by_id[i] = &sr->by_map[i];
	qsort(sr->by_id, sr->n, sizeof(SweepRoot *), cmp_root_id);
}

static void
sweep_roots_free(SweepRoots *sr)
{
	if (sr->by_map)
		pfree(sr->by_map);
	if (sr->by_id)
		pfree(sr->by_id);
}

/*
 * The subtrees of rel's own roots that its store still counts (not free),
 * of those the sweep read (sr), BMAP_TREE_TOPS for each, into *tops: how
 * many roots.  Its own are those captured of its map (by the map's
 * identity, whatever file number a map had before it), and the root it was
 * made a copy of: a relation restored of a root holds it as its branch's
 * commit, as one captured of it.  They share its pages with no other
 * map.  Under rel's header.  O(log roots)
 * and its own roots.
 */
static int
own_root_tops(RelFileLocator rel, SweepRoots *sr, BmapEntry **tops)
{
	Buffer		rootbuf;
	uint64		map_id;
	uint64		made_of;
	int			lo = 0;
	int			hi;
	int			n = 0;
	int			cap;
	SweepRoot	key;
	SweepRoot  *keyp = &key;
	SweepRoot **made;

	*tops = NULL;
	sweep_roots_load(sr);
	if (sr->n == 0)
		return 0;
	rootbuf = map_read(rel, 1);
	LockBuffer(rootbuf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(rootbuf), BMAP_PAGE_ROOT, rel, BMAP_FORKNUM, 1);
	map_id = PageGetBmapRoot(BufferGetPage(rootbuf))->map_id;
	made_of = PageGetBmapRoot(BufferGetPage(rootbuf))->made_of;
	UnlockReleaseBuffer(rootbuf);

	/* (the first captured of the map) */
	hi = sr->n;
	while (lo < hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (sr->by_map[mid].map_id < map_id)
			lo = mid + 1;
		else
			hi = mid;
	}
	for (hi = lo; hi < sr->n && sr->by_map[hi].map_id == map_id; hi++)
		;
	key.id = made_of;
	made = made_of == 0 ? NULL :
		bsearch(&keyp, sr->by_id, sr->n, sizeof(SweepRoot *), cmp_root_id);
	if (made && (*made)->map_id == map_id)
		made = NULL;			/* (one of those already) */
	cap = (hi - lo) + (made ? 1 : 0);
	if (cap == 0)
		return 0;
	*tops = palloc_array(BmapEntry, cap * BMAP_TREE_TOPS);
	for (int i = lo; i < hi; i++)
		memcpy(*tops + (n++) * BMAP_TREE_TOPS, sr->by_map[i].tops,
			   sizeof(BmapEntry) * BMAP_TREE_TOPS);
	if (made)
		memcpy(*tops + (n++) * BMAP_TREE_TOPS, (*made)->tops,
			   sizeof(BmapEntry) * BMAP_TREE_TOPS);
	return n;
}

/*
 * How many roots the store holds (root_state()), of relation file number
 * store.relNumber if valid (captured of it), and the store's slots; and
 * into *naming if not NULL, how many of the slots in use name a
 * transaction (a capturer or releaser, not settled).
 */
void
bmap_inspect_roots(RelFileLocator store, uint64 *held, uint64 *slots,
				   uint64 *naming)
{
	BmapRootsMeta meta;
	Buffer		metabuf;

	*held = *slots = 0;
	if (naming)
		*naming = 0;
	if (!roots_exist(store))
		return;
	metabuf = roots_read(store, 0);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	if (PageIsNew(BufferGetPage(metabuf)))
	{
		UnlockReleaseBuffer(metabuf);
		return;
	}
	meta = *PageGetBmapRootsMeta(BufferGetPage(metabuf));
	UnlockReleaseBuffer(metabuf);
	*slots = (uint64) meta.npages * BMAP_ROOTS_PER_PAGE;
	for (uint32 p = 0; p < meta.npages; p++)
	{
		Buffer		buf = roots_read(store, 1 + p);

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		for (uint32 i = 0; i < BMAP_ROOTS_PER_PAGE; i++)
		{
			const xl_bmap_root *r = &PageGetBmapRoots(BufferGetPage(buf))[i];
			RootState	state = root_state(r);

			if ((state == ROOT_HELD || state == ROOT_PENDING) &&
				(!RelFileNumberIsValid(store.relNumber) ||
				 r->relnumber == store.relNumber))
				(*held)++;
			if (naming && r->snap.id != 0 &&
				(r->snap.owner != 0 || r->released_by != 0))
				(*naming)++;
		}
		UnlockReleaseBuffer(buf);
	}
}

/* ----------------------------------------------------------------
 * The sweep
 * ----------------------------------------------------------------
 */

/*
 * One more holder of block b.  A block's holders are at most a count's most
 * and one, which a uint32 holds; one counted more than that (which no
 * count could say) stays at the most.
 */
static inline void
hold(uint32 *holders, BlockNumber b)
{
	if (holders[b] < PG_UINT32_MAX)
		holders[b]++;
}

typedef struct SweepHolders
{
	uint32	   *holders;
	uint64	   *reached;		/* the nodes reached, a bit each */
	BlockNumber hw;
} SweepHolders;

/*
 * A tree's node, one holder more: one for each page that points to it.  Its
 * children are counted only the first time it is reached, as it holds each
 * once, whichever relations reach it.  That it was reached is kept apart
 * from its holders, which count the block however it is held, so the
 * node's children are counted once whatever else counted the block first.
 * (A node past the blocks
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

/* A pointer the roots' counts still count, though gone: one holder fewer. */
static void
sweep_uncount(const BmapEntry *entry, BlockNumber blkno, void *arg)
{
	SweepHolders *sh = arg;

	if (entry->block < sh->hw && sh->holders[entry->block] > 0 &&
		sh->holders[entry->block] < PG_UINT32_MAX)
		sh->holders[entry->block]--;
}

/*
 * Count, in holders[], each physical block a relation holds: its map's
 * entries (those past its length too; a tree's nodes too).  Its header is
 * held exclusively meanwhile, so no extension, remap or truncation of it
 * runs: a block they allocated is mapped by the time it is read.
 */
static void
sweep_relation_read(SMgrRelation reln, void *arg)
{
	SweepHolders *counts = arg;
	RelFileLocator rel = reln->smgr_rlocator.locator;
	BlockNumber mapnblocks;
	Buffer		headerbuf;

	/*
	 * A relation dropped since the directory was read is passed over; once
	 * its map is open it stays readable.
	 */
	if (!mdexists(reln, BMAP_FORKNUM))
		return;
	mapnblocks = mdnblocks(reln, BMAP_FORKNUM);
	if (mapnblocks == 0)
		return;
#ifdef USE_INJECTION_POINTS
	{
		/* (the relation's file number, for a test to stop at one only) */
		char		relnumber[16];

		snprintf(relnumber, sizeof(relnumber), "%u", rel.relNumber);
		INJECTION_POINT("bmap-sweep-relation", relnumber);
	}
#endif
	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_EXCLUSIVE);
	if (PageIsNew(BufferGetPage(headerbuf)))
	{
		UnlockReleaseBuffer(headerbuf);
		return;
	}
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	tree_walk(reln, sweep_node, sweep_entry, counts);
	UnlockReleaseBuffer(headerbuf);
}

/*
 * sweep_relation_read(), the relation perhaps dropped while it is read (its
 * buffers dropped before these were read): none of its pages stays in
 * shared buffers (read_maybe_dropped()).  One whose files go as it is read
 * is passed over, what it held counted from the release queue.  What was
 * counted of it before its read failed stays counted, which only holds a
 * block more: of its map's own fork only the header and the root page are
 * read before the tree's nodes, which are the page store's, so no node is
 * marked reached with its children left uncounted.
 */
static void
sweep_relation(SMgrRelation reln, uint32 *holders, uint64 *reached,
			   BlockNumber hw)
{
	SweepHolders sh = {holders, reached, hw};

	read_maybe_dropped(reln, sweep_relation_read, &sh);
}

/*
 * A relation's own holders of each block, as the sweep counts every
 * relation's (sweep_node(), sweep_entry()): its tree's and its captured
 * roots' nodes once for each of its pages pointing to them, a node's
 * children once.  Then which of its tree's entries another map may hold.
 * A node or a block is held elsewhere if more hold it than the relation
 * does, or if any of the relation's nodes pointing to it is (a root's too:
 * another map sharing a root's subtree shares everything under it, the
 * subtrees and blocks the tree has in common with the root among them,
 * through a copy of a node as much as through the node itself); an
 * entry is, if its leaf or its block is.  Only what more than one page
 * holds is tracked (a node or block held once is held by its one parent
 * alone, as that parent is held), so the table is of the shared part of
 * the map, not the whole.  The
 * roots' subtrees are walked first, a node reached again walked again
 * when it is newly held elsewhere, so that the tree's walk, last, finds
 * each node's mark as every pointer to it leaves it.
 */
typedef struct OwnHolder
{
	BlockNumber block;
	uint32		count;
	bool		reached;		/* by the count */
	bool		walked;			/* by the walk marking what is elsewhere */
	bool		elsewhere;
} OwnHolder;

typedef struct OwnHolders
{
	HTAB	   *own;
	const uint32 *holders;		/* every relation's */
	BlockNumber hw;
	bool		tree;			/* the tree walked now, not a root */
	bool		elsewhere[BMAP_TREE_TOPS + 1];	/* each level's node on the
												 * path now */
	uint32		below;			/* how far shared_below goes down */
	uint32		shared_below;	/* as it was */
	uint64		entries;		/* the tree's entries walked */
	BmapEntry  *root_tops;		/* the roots captured of it, as its own */
	int			nroots;
} OwnHolders;

static OwnHolder *
own_holder(OwnHolders *oh, BlockNumber b)
{
	bool		found;
	OwnHolder  *h = hash_search(oh->own, &b, HASH_ENTER, &found);

	if (!found)
	{
		h->count = 0;
		h->reached = false;
		h->walked = false;
		h->elsewhere = false;
	}
	return h;
}

static bool
own_node(BlockNumber node, uint16 level, BlockNumber first, void *arg)
{
	OwnHolders *oh = arg;
	OwnHolder  *h;

	if (node >= oh->hw || oh->holders[node] <= 1)
		return true;
	h = own_holder(oh, node);
	h->count++;
	if (h->reached)
		return false;
	h->reached = true;
	return true;
}

static void
own_entry(const BmapEntry *entry, BlockNumber blkno, void *arg)
{
	OwnHolders *oh = arg;

	if (entry->block < oh->hw && oh->holders[entry->block] > 1)
		own_holder(oh, entry->block)->count++;
}

/* Does another map hold block b, as counted (or may it: one newer)? */
static bool
held_elsewhere(OwnHolders *oh, BlockNumber b)
{
	OwnHolder  *h;

	if (b >= oh->hw)
		return true;
	if (oh->holders[b] <= 1)
		return false;
	h = hash_search(oh->own, &b, HASH_FIND, NULL);
	return h == NULL || oh->holders[b] > h->count;
}

/*
 * A node reached from the one above it on the path now (elsewhere[level +
 * 1]; a top has none, the flags cleared before each).  In a root's
 * walk, a node walked already is walked again only if it is newly held
 * elsewhere; the tree's walk, each node of the tree reached once, walks
 * them all.
 */
static bool
shared_node(BlockNumber node, uint16 level, BlockNumber first, void *arg)
{
	OwnHolders *oh = arg;
	OwnHolder  *h;
	bool		newly;

	if (node >= oh->hw)
	{
		oh->elsewhere[level] = true;
		return true;
	}

	/* (one held once is reached once, from its one parent) */
	if (oh->holders[node] <= 1)
	{
		oh->elsewhere[level] = oh->elsewhere[level + 1];
		return true;
	}
	h = own_holder(oh, node);
	newly = !h->elsewhere &&
		(oh->elsewhere[level + 1] || held_elsewhere(oh, node));
	h->elsewhere |= newly;
	oh->elsewhere[level] = h->elsewhere;
	if (oh->tree || !h->walked)
	{
		h->walked = true;
		return true;
	}
	return newly;
}

/*
 * An entry: in a root's leaf held elsewhere, its block is marked so
 * (the tree's leaf holding the same block, a copy, shares it no less); in
 * the tree's, it counts if its leaf or its block is held elsewhere.
 */
static void
shared_entry(const BmapEntry *entry, BlockNumber blkno, void *arg)
{
	OwnHolders *oh = arg;
	OwnHolder  *h;

	if (!oh->tree)
	{
		if (oh->elsewhere[0] && entry->block < oh->hw &&
			oh->holders[entry->block] > 1)
			own_holder(oh, entry->block)->elsewhere = true;
		return;
	}
	oh->entries++;
	if (entry->epoch >= oh->shared_below)
		return;
	h = entry->block < oh->hw && oh->holders[entry->block] > 1 ?
		hash_search(oh->own, &entry->block, HASH_FIND, NULL) : NULL;
	if (oh->elsewhere[0] || held_elsewhere(oh, entry->block) ||
		(h != NULL && h->elsewhere))
		oh->below = Max(oh->below, entry->epoch + 1);
}

/* The roots' subtrees, then the tree, marking what is held elsewhere. */
static void
shared_walk(SMgrRelation reln, OwnHolders *oh)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	BmapRoot	root;
	Buffer		buf = map_read(rel, 1);
	uint64		first;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(buf), BMAP_PAGE_ROOT, rel, BMAP_FORKNUM, 1);
	root = *PageGetBmapRoot(BufferGetPage(buf));
	UnlockReleaseBuffer(buf);
	oh->tree = false;
	for (int i = 0; i < oh->nroots; i++)
	{
		const BmapEntry *tops = oh->root_tops + i * BMAP_TREE_TOPS;

		first = 0;
		for (int k = 0; k < BMAP_TREE_TOPS; k++)
		{
			memset(oh->elsewhere, 0, sizeof(oh->elsewhere));
			if (BlockNumberIsValid(tops[k].block))
				tree_walk_node(reln, &tops[k], k, (BlockNumber) first, true,
							   shared_node, shared_entry, oh);
			first += tree_span(k);
		}
	}
	oh->tree = true;
	first = 0;
	for (int k = 0; k < BMAP_TREE_TOPS; k++)
	{
		memset(oh->elsewhere, 0, sizeof(oh->elsewhere));
		if (BlockNumberIsValid(root.top[k].block))
			tree_walk_node(reln, &root.top[k], k, (BlockNumber) first, false,
						   shared_node, shared_entry, oh);
		first += tree_span(k);
	}
}

/*
 * Hold the relation whose storage rel is against its storage's drop, as
 * long as the sweep reads and changes its map: its row in pg_class, seen
 * committed by a snapshot now, locked as its readers lock it (a drop of the
 * table, a rewrite, a share take AccessExclusiveLock), tried, not waited
 * for, then the row found again.  A relation made by a transaction not yet committed
 * (and dropped if it aborts) has no row a snapshot sees.  False, nothing
 * held, if any of this fails (or the store is another database's).
 */
typedef struct HeldRelation
{
	LOCKTAG		rel;
	struct BmapCompacting *slot;	/* a compaction's, asked to let go */
} HeldRelation;

static void let_go_relation(HeldRelation *held);

/* the longest wait between looks at a compaction asked to let go, in ms */
#define COMPACTION_LET_GO_MAX_DELAY_MS	10

static bool compacting_cleared_at_exit = false;

/*
 * What this backend's sweep was asked to let go of: passed over for the
 * rest of the sweep, so that one asking waits for one hold, not for a hold
 * of the same relation again.  Past the most remembered, a hold asked for
 * is only let go of.
 */
#define SWEEP_YIELDED_MAX	64

static LOCKTAG sweep_yielded[SWEEP_YIELDED_MAX];
static int	sweep_nyielded = 0;

/*
 * This backend's sweep holds nothing now (see BmapCompacting): whether the
 * hold was asked to let go, read with its end, so that an ask either comes
 * before (and is answered) or finds nothing held.
 */
static bool
compacting_stop(void)
{
	BmapCompacting *c;
	bool		asked;

	if (MyProcNumber == INVALID_PROC_NUMBER || MyProcNumber >= MaxBackends)
		return false;
	c = &bmap_compacting[MyProcNumber];
	SpinLockAcquire(&c->mutex);
	asked = c->holding && pg_atomic_read_u32(&c->asked) != 0;
	if (c->holding)
		c->generation++;
	c->holding = false;
	pg_atomic_write_u32(&c->asked, 0);
	SpinLockRelease(&c->mutex);
	return asked;
}

static void
compacting_end(int code, Datum arg)
{
	(void) compacting_stop();
}

/*
 * This backend's slot cleared as it exits.  (Registered before a sweep's
 * error cleanup is, which must be the latest when it ends.)
 */
static void
compacting_clear_at_exit(void)
{
	if (!compacting_cleared_at_exit)
	{
		before_shmem_exit(compacting_end, 0);
		compacting_cleared_at_exit = true;
	}
}

/*
 * This backend's compaction holds 'held' from now on, until
 * compacting_end(): its slot, whose 'asked' it reads before each move, or
 * NULL if it has none (an auxiliary process, which compacts nothing).
 */
static BmapCompacting *
compacting_begin(const HeldRelation *held)
{
	BmapCompacting *c;

	if (MyProcNumber == INVALID_PROC_NUMBER || MyProcNumber >= MaxBackends)
		return NULL;
	compacting_clear_at_exit();
	c = &bmap_compacting[MyProcNumber];
	pg_atomic_write_u32(&c->asked, 0);
	SpinLockAcquire(&c->mutex);
	c->rel = held->rel;
	c->holding = true;
	c->generation++;
	SpinLockRelease(&c->mutex);
	return c;
}

static inline bool
locktag_equal(const LOCKTAG *a, const LOCKTAG *b)
{
	return a->locktag_field1 == b->locktag_field1 &&
		a->locktag_field2 == b->locktag_field2 &&
		a->locktag_field3 == b->locktag_field3 &&
		a->locktag_field4 == b->locktag_field4 &&
		a->locktag_type == b->locktag_type &&
		a->locktag_lockmethodid == b->locktag_lockmethodid;
}

bool
bmap_compaction_let_go(const LOCKTAG *tag, TimestampTz *start)
{
	long		delay = 1;
	bool		let_go = false;
	int			nasked = 0;
	int		   *asked = palloc_array(int, MaxBackends);
	uint64	   *generations = palloc_array(uint64, MaxBackends);

	/* the holds of 'tag' now, each asked to let go */
	for (int i = 0; i < MaxBackends; i++)
	{
		BmapCompacting *c = &bmap_compacting[i];

		/* (a slot holding nothing is passed over without its lock) */
		if (!c->holding)
			continue;
		SpinLockAcquire(&c->mutex);
		if (c->holding && locktag_equal(&c->rel, tag))
		{
			pg_atomic_write_u32(&c->asked, 1);
			asked[nasked] = i;
			generations[nasked++] = c->generation;
		}
		SpinLockRelease(&c->mutex);
	}
	if (nasked == 0)
	{
		pfree(asked);
		pfree(generations);
		return false;
	}

	/*
	 * ... until each has ended.  Another hold of the tag begun since isn't
	 * waited for: asked, a sweep passes over what it was asked for from
	 * then on (hold_relation()).
	 */
	if (*start == 0)
		*start = GetCurrentTimestamp();
	for (;;)
	{
		bool		ended = true;

		for (int k = 0; k < nasked && ended; k++)
		{
			BmapCompacting *c = &bmap_compacting[asked[k]];

			SpinLockAcquire(&c->mutex);
			ended = c->generation != generations[k];
			SpinLockRelease(&c->mutex);
		}
		if (ended)
		{
			let_go = true;
			break;
		}
		if (LockTimeout > 0 &&
			TimestampDifferenceExceeds(*start, GetCurrentTimestamp(),
									   LockTimeout))
			break;
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 delay, WAIT_EVENT_BMAP_COMPACTION_LET_GO);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
		delay = Min(delay * 2, COMPACTION_LET_GO_MAX_DELAY_MS);
	}
	pfree(asked);
	pfree(generations);
	return let_go;
}

static bool
class_row_of(RelFileLocator rel, Oid *relid)
{
	Relation	pg_class;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	bool		found = false;

	InvalidateCatalogSnapshot();
	pg_class = table_open(RelationRelationId, AccessShareLock);
	ScanKeyInit(&key[0], Anum_pg_class_reltablespace, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(rel.spcOid == MyDatabaseTableSpace ?
										  InvalidOid : rel.spcOid));
	ScanKeyInit(&key[1], Anum_pg_class_relfilenode, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(rel.relNumber));
	scan = systable_beginscan(pg_class, ClassTblspcRelfilenodeIndexId, true,
							  NULL, 2, key);
	if (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		*relid = ((Form_pg_class) GETSTRUCT(tup))->oid;
		found = true;
	}
	systable_endscan(scan);
	table_close(pg_class, AccessShareLock);
	/* (no xmin kept for the rest of a sweep or compaction) */
	InvalidateCatalogSnapshot();
	return found;
}

static bool
hold_relation(RelFileLocator rel, HeldRelation *held, bool compacting)
{
	Oid			relid;
	Oid			again_relid;

	held->slot = NULL;
	if (rel.dbOid != MyDatabaseId || !IsTransactionState() ||
		!class_row_of(rel, &relid))
		return false;
	SET_LOCKTAG_RELATION(held->rel, MyDatabaseId, relid);
	if (compacting)
	{
		for (int i = 0; i < sweep_nyielded; i++)
			if (locktag_equal(&sweep_yielded[i], &held->rel))
				return false;
	}

	/*
	 * (a compaction's hold shown before it is taken: one who tries a lock
	 * the hold has just taken finds whom to ask)
	 */
	if (compacting)
		held->slot = compacting_begin(held);
	if (LockAcquire(&held->rel, AccessShareLock, false, true) == LOCKACQUIRE_NOT_AVAIL)
	{
		if (held->slot)
			compacting_end(0, 0);
		return false;
	}
	AcceptInvalidationMessages();
	if (!class_row_of(rel, &again_relid) || again_relid != relid)
	{
		let_go_relation(held);
		return false;
	}
	return true;
}

static void
let_go_relation(HeldRelation *held)
{
	LockRelease(&held->rel, AccessShareLock, false);
	if (held->slot && compacting_stop() &&
		sweep_nyielded < SWEEP_YIELDED_MAX)
		sweep_yielded[sweep_nyielded++] = held->rel;
}

/*
 * Lower a relation's shared_below to just past the newest of its entries
 * another map still holds (0 if none), now that every holder is counted,
 * holders[] (each block's, of every map and the release queue).  Never raised,
 * and never below an entry another map holds: no share adds a holder while the
 * sweep's lock is held, and a copy of a node or a page meanwhile only adds
 * holders, of blocks the counts already hold or of blocks newer than any
 * counted, which count as held elsewhere.  Under the header's lock, WAL-logged
 * as a header delta, the relation's length as it is (a truncation's record,
 * which carries one), the relation held against its drop meanwhile
 * (hold_relation()).
 */
static void
sweep_shared_below(SMgrRelation reln, const uint32 *holders, BlockNumber hw,
				   SweepRoots *sr)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		headerbuf;
	BmapHeader *header;
	HASHCTL		ctl;
	OwnHolders	oh;
	HeldRelation held;

	if (!mdexists(reln, BMAP_FORKNUM) || mdnblocks(reln, BMAP_FORKNUM) == 0 ||
		!bmap_relation_exists(rel))
		return;

	/*
	 * (held against its drop before anything of it is read: else a page read
	 * after a drop dropped its buffers would stay in shared buffers once the
	 * fork is unlinked, for a relation given its file number later to find,
	 * a drop committed before the lowering's record would be replayed
	 * before it, and a header dirtied then would be written to a fork
	 * unlinked)
	 */
	if (!hold_relation(rel, &held, true))
		return;
#ifdef USE_INJECTION_POINTS
	{
		/* (the relation's file number, for a test to stop at one only) */
		char		relnumber[16];

		snprintf(relnumber, sizeof(relnumber), "%u", rel.relNumber);
		INJECTION_POINT("bmap-shared-below-held", relnumber);
	}
#endif
	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_EXCLUSIVE);
	if (PageIsNew(BufferGetPage(headerbuf)))
	{
		UnlockReleaseBuffer(headerbuf);
		let_go_relation(&held);
		return;
	}
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	header = PageGetBmapHeader(BufferGetPage(headerbuf));
	if (header->shared_below == 0)
	{
		UnlockReleaseBuffer(headerbuf);
		let_go_relation(&held);
		return;
	}

	ctl.keysize = sizeof(BlockNumber);
	ctl.entrysize = sizeof(OwnHolder);
	ctl.hcxt = CurrentMemoryContext;
	memset(&oh, 0, sizeof(oh));
	oh.own = hash_create("bmap own holders", 1024, &ctl,
						 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	oh.holders = holders;
	oh.hw = hw;
	oh.shared_below = header->shared_below;
	tree_walk(reln, own_node, own_entry, &oh);
	{
		/* (and the roots captured of it) */
		BmapEntry  *tops;
		int			nroots = own_root_tops(rel, sr, &tops);

		for (int i = 0; i < nroots; i++)
		{
			uint64		first = 0;

			for (int k = 0; k < BMAP_TREE_TOPS; k++)
			{
				if (BlockNumberIsValid(tops[i * BMAP_TREE_TOPS + k].block))
					tree_walk_node(reln, &tops[i * BMAP_TREE_TOPS + k], k,
								   (BlockNumber) first, true, own_node,
								   own_entry, &oh);
				first += tree_span(k);
			}
		}
		oh.root_tops = tops;
		oh.nroots = nroots;
	}
	shared_walk(reln, &oh);
	if (oh.root_tops)
		pfree(oh.root_tops);
	hash_destroy(oh.own);

	/* (a tree of no entries, as a dropped map's is: nothing to change) */
	if (oh.entries > 0 && oh.below < header->shared_below)
	{
		xl_bmap_header_delta delta;
		XLogRecPtr	lsn;

		delta.kind = BMAP_DELTA_HEADER;
		delta.flags = BMAP_DELTA_SHARED_BELOW;
		delta.nblocks = header->nblocks;
		delta.vm_nblocks = header->vm_nblocks;
		delta.free_space_unknown = 0;
		delta.epoch = header->epoch;
		delta.shared_below = oh.below;
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
	}
	UnlockReleaseBuffer(headerbuf);
	let_go_relation(&held);
}

/*
 * Zero the counts of the blocks blocks[0 .. n), sorted, which nothing holds,
 * before they are freed: a count a crash left too high (a holder's going
 * whose record was lost) would otherwise come with the block when it is
 * allocated again.  Nothing gives such a block a holder meanwhile, as a
 * path copy adds holders only to blocks held already.  Returns how many
 * counts changed.
 */
static uint32
sweep_recount(RelFileLocator rel, const BlockNumber *blocks, uint32 n)
{
	uint32		i = 0;
	uint32		recounted = 0;

	while (i < n)
	{
		RefsPages	rp;
		uint32		covered = refs_lock(rel, blocks + i, n - i, &rp);

		for (uint32 k = i; k < i + covered; k++)
			refs_set(&rp, blocks[k], 0);
		recounted += covered;
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
 *
 * Not while another sweep of the store runs (this one no longer counts
 * itself).  That sweep may have counted blocks this one has since freed,
 * as nothing held them, and still be about to free them.  Shortened past
 * them, the store would grow again into the same block numbers, the
 * allocator taking no free block while a sweep runs, and its frees would
 * then let go of blocks a relation holds.  A sweep that begins after the
 * check reads the metapage after the store is shortened.  The next sweep
 * shortens it.
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
	if (sweep_running(rel))
	{
		UnlockReleaseBuffer(metabuf);
		return 0;
	}

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
 * block-mapped relation there holds (found by their map files, not by
 * catalog rows), then free the blocks allocated when the sweep began that
 * nothing holds (left by a crash after a drop's commit), what the release
 * queue holds let go of first and the rest counted, their counts zeroed
 * first, and shorten the store to its
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


/* ----------------------------------------------------------------
 * Compaction: the blocks past where the page store would end moved down
 * ----------------------------------------------------------------
 */

/*
 * Freeing leaves holes where the freed blocks were (a dropped or truncated
 * relation's, among the others').  The store keeps them until later allocations
 * fill them: a sweep can shorten it only to its last allocated block.  So
 * after counting, a sweep moves the blocks past the allocated count down,
 * each to the lowest free block, and then shortens the store past them
 * (sweep_shrink()).  It moves only data pages one relation alone holds, its
 * leaf and every node above it the relation's own, each as a copy before a
 * change would move it, which redo replays the same way: copied to the new
 * block by a REMAP (the page's image, the leaf's entry, the allocation)
 * under its buffer's cleanup lock, then the old block freed.  Its entry
 * keeps its epoch.  The image restores
 * the page whatever records redo skips for it from before the move.
 *
 * A tree's nodes stay where they are: a node moved would get a new pointer,
 * and recovery skips the records of every page under a pointer newer than
 * them (tree_find()), which only a share's images of those pages (and
 * their write) make safe; so does anything shared, a root's, or busy (a
 * pinned buffer).  A node in the tail ends what the store
 * can be shortened by.  A crash between a move and the freeing of
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

typedef struct CompactWalk
{
	BlockNumber target;			/* move the blocks from here */
	BlockNumber nblocks;		/* the relation's length */
	BlockNumber vm_nblocks;		/* and its visibility map's */
	CompactMove *moves;
	uint32		n;
	uint32		cap;
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
	return true;
}

static void
compact_visit_entry(const BmapEntry *entry, BlockNumber blkno, void *arg)
{
	CompactWalk *cw = arg;

	/* (not a root's, nor one past the relation's length) */
	if (entry->block >= cw->target && BlockNumberIsValid(blkno) &&
		key_in_length(cw->nblocks, cw->vm_nblocks, blkno))
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
 * still names the block, which nothing else holds (nor a copy made without
 * an image may still read), on a path the relation owns.  The header is
 * held, at least shared.
 */
static bool
compact_page_movable(SMgrRelation reln, Buffer headerbuf, BlockNumber blkno,
					 BlockNumber phys, BmapEntry *entry, MapPage *mp)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	BmapHeader *header = PageGetBmapHeader(BufferGetPage(headerbuf));

	return key_in_length(header->nblocks, header->vm_nblocks, blkno) &&
		lookup_entry_at(reln, blkno, entry, mp, NULL) &&
		entry->block == phys &&
		lookup_ref(rel, phys) == 0 &&
		!lookup_copy_holds(rel, phys) &&
		tree_path_own(reln, blkno, header->epoch, false);
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
#ifdef USE_INJECTION_POINTS
	{
		/* (the relation's file number, for a test to stop at one only) */
		char		relnumber[16];

		snprintf(relnumber, sizeof(relnumber), "%u", rel.relNumber);
		INJECTION_POINT("bmap-compact-room", relnumber);
	}
#endif

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
	buf = ReadBufferWithoutRelcache(rel, key_fork(blkno), key_block(blkno),
									RBM_NORMAL,
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
				  zeroed && new_block < alloc.old_hw ? 1 : 0, false, 0);

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
	UnlockReleaseBuffer(buf);

	/*
	 * The old block freed before the header is let go of.  Held by nothing
	 * from the remap on, it would otherwise be found so by a sweep counting
	 * the relation (under its header) once this compaction's own sweep no
	 * longer counts as running: freed by that sweep, then given out again
	 * by the allocator, it would be freed here from under its new holder.
	 * (A crash between the two records leaves it to a sweep, as before.)
	 */
	free_blocks(rel, &phys, 1);
	UnlockReleaseBuffer(headerbuf);
	pfree(ndelta);
	return true;
}

static int
cmp_move_desc(const void *a, const void *b)
{
	const CompactMove *x = (const CompactMove *) a;
	const CompactMove *y = (const CompactMove *) b;

	return pg_cmp_u32(y->phys, x->phys);
}

/*
 * Move one relation's blocks from 'target' on, the highest first; each old
 * block goes back to the allocator with its move (compact_page()).  With
 * 'held' (the relation held against its drop), the moves stop once another
 * waits for that hold, so a DDL waits for no more than a move.  Those that
 * only try their locks aren't waiting, and ask through the compaction's slot
 * instead (bmap_compaction_let_go()): the moves stop as well once it is asked.
 */
static uint64
compact_relation(SMgrRelation reln, BlockNumber target,
				 const LOCKTAG *held, BmapCompacting *slot)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	CompactWalk cw = {target, 0, 0, NULL, 0, 0};
	Buffer		headerbuf;
	uint64		moved = 0;

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
	cw.vm_nblocks = PageGetBmapHeader(BufferGetPage(headerbuf))->vm_nblocks;
	tree_walk(reln, compact_visit_node, compact_visit_entry, &cw);
	UnlockReleaseBuffer(headerbuf);
	if (cw.n == 0)
		return 0;

	qsort(cw.moves, cw.n, sizeof(CompactMove), cmp_move_desc);
	for (uint32 i = 0; i < cw.n; i++)
	{
		const CompactMove *m = &cw.moves[i];

		if (held != NULL && i > 0 &&
			((slot != NULL && pg_atomic_read_u32(&slot->asked) != 0) ||
			 LockHasWaiters(held, AccessShareLock, false)))
			break;
		if (compact_page(reln, m->blkno, m->phys))
			moved++;
		CHECK_FOR_INTERRUPTS();
	}
	INJECTION_POINT("bmap-compact-moved", NULL);
	pfree(cw.moves);
	return moved;
}

typedef struct CompactRead
{
	BlockNumber target;
	const LOCKTAG *held;
	BmapCompacting *slot;
	uint64		moved;
} CompactRead;

static void
compact_read(SMgrRelation reln, void *arg)
{
	CompactRead *cr = arg;

	cr->moved = compact_relation(reln, cr->target, cr->held, cr->slot);
}

/*
 * Move the blocks of a database's page store in a tablespace (directory
 * 'path') that lie past its allocated count: how many moved.  Each relation
 * is moved in a subtransaction of its own, so one dropped or truncated
 * meanwhile is passed over, and held against its drop (hold_relation()),
 * which is what keeps the WAL in order: a relation's drop removes its map
 * as its commit record is replayed, so a move logged after that record (a
 * drop committed, its map not yet let go of) would be replayed against no
 * map, and a standby's recovery would fail there, again at every restart.
 * One whose drop is in flight isn't moved at all, nor is one locked by
 * another; and a relation's moves stop once another waits for its hold.
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
			CompactRead cr = {target, NULL, NULL, 0};
			HeldRelation held;

			if (hold_relation(reln->smgr_rlocator.locator, &held, true))
			{
				cr.held = &held.rel;
				cr.slot = held.slot;
				read_maybe_dropped(reln, compact_read, &cr);
				let_go_relation(&held);
			}
			moved += cr.moved;
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
			/* (shown held until the subtransaction let go of the hold) */
			compacting_end(0, 0);
			MemoryContextSwitchTo(oldcxt);
			CurrentResourceOwner = oldowner;
			/*
			 * (a relation whose blocks can't be moved is passed over; a
			 * cancel or timeout, a shutdown, or running out of memory or
			 * disk ends the sweep)
			 */
			if (ERRCODE_TO_CATEGORY(edata->sqlerrcode) ==
				ERRCODE_OPERATOR_INTERVENTION ||
				ERRCODE_TO_CATEGORY(edata->sqlerrcode) ==
				ERRCODE_INSUFFICIENT_RESOURCES)
			{
				FreeDir(dir);
				ReThrowError(edata);
			}
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
 * A sweep's counting failed: as sweep_done(), and a hold of a lowering of
 * shared_below (sweep_shared_below()) shown let go of with it.
 */
static void
sweep_failed(int code, Datum arg)
{
	compacting_end(code, 0);
	sweep_done(code, arg);
}

void
bmap_sweep(Oid spcOid, Oid dbOid, BmapSweep *result)
{
	RelFileLocator rel = {spcOid, dbOid, InvalidRelFileNumber};
	char	   *path = GetDatabasePath(dbOid, spcOid);

	memset(result, 0, sizeof(*result));
	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("cannot sweep a page store during recovery")));
	if (!alloc_exists(rel))
		return;

	/* what dropped maps held, let go of first: the sweep counts the rest */
	{
		BmapRelease done = {0};

		bmap_release_drain(spcOid, dbOid, 0, &done);
	}
	INJECTION_POINT("bmap-sweep-drained", NULL);
	swept_note(rel, true);
	sweep_note(rel, true);
	sweep_nyielded = 0;
	compacting_clear_at_exit();
	PG_ENSURE_ERROR_CLEANUP(sweep_failed, PointerGetDatum(&rel));
	{
		sweep_store(rel, path, result);
	}
	PG_END_ENSURE_ERROR_CLEANUP(sweep_failed, PointerGetDatum(&rel));
	sweep_done(0, PointerGetDatum(&rel));
	result->moved = compact_store(rel, path);
	result->truncated = sweep_shrink(rel);
	swept_note(rel, false);
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
	BlockNumber vm_nblocks;
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
	if (key_in_length(sb->nblocks, sb->vm_nblocks, blkno))
		sb->mapped[b / 64] |= UINT64CONST(1) << (b % 64);
	else
		sb->listed[b / 64] |= UINT64CONST(1) << (b % 64);
}

/*
 * Mark, in mapped[] and listed[] (bits of the store's blocks below hw), the
 * blocks a relation holds: its map's entries under its forks' lengths (its
 * visibility map's pages are the store's too), and the rest (entries past
 * them, a tree's nodes).  Its header is held shared
 * meanwhile, so no extension or truncation of it runs.
 * Its length, or 0 if it was dropped since the directory was read.
 */
typedef struct SizeRead
{
	uint64	   *mapped;
	uint64	   *listed;
	BlockNumber hw;
	BlockNumber nblocks;
} SizeRead;

static void
size_relation_read(SMgrRelation reln, void *arg)
{
	SizeRead   *sr = arg;
	uint64	   *mapped = sr->mapped;
	uint64	   *listed = sr->listed;
	BlockNumber hw = sr->hw;
	RelFileLocator rel = reln->smgr_rlocator.locator;
	BlockNumber nblocks;
	Buffer		headerbuf;

	sr->nblocks = 0;
	if (!mdexists(reln, BMAP_FORKNUM) || mdnblocks(reln, BMAP_FORKNUM) == 0)
		return;
	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_SHARE);
	if (PageIsNew(BufferGetPage(headerbuf)))
	{
		UnlockReleaseBuffer(headerbuf);
		return;
	}
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	nblocks = PageGetBmapHeader(BufferGetPage(headerbuf))->nblocks;
	{
		SizeBits	sb = {mapped, listed, hw, nblocks,
		PageGetBmapHeader(BufferGetPage(headerbuf))->vm_nblocks};

		tree_walk(reln, size_node, size_entry, &sb);
	}
	UnlockReleaseBuffer(headerbuf);
	sr->nblocks = nblocks;
}

/* size_relation_read(), the relation perhaps dropped as it is read */
static BlockNumber
size_relation(SMgrRelation reln, uint64 *mapped, uint64 *listed,
			  BlockNumber hw)
{
	SizeRead	sr = {mapped, listed, hw, 0};

	read_maybe_dropped(reln, size_relation_read, &sr);
	return sr.nblocks;
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
				 relnumber == BMAP_REFS_RELNUMBER ||
				 relnumber == BMAP_QUEUE_RELNUMBER ||
				 relnumber == BMAP_ROOTS_RELNUMBER)
			sizes->alloc_bytes += st.st_size;
		else if (bsearch(&relnumber, maps, nmaps, sizeof(RelFileNumber),
						 cmp_relnumber) == NULL)
			continue;			/* (not block-mapped) */
		else if (fork == BMAP_FORKNUM)
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
 * the server starts (a crash before may have leaked a drop's blocks): counts
 * are exact, so a block's last holder going frees it with no sweep.  Past
 * the most stores remembered, a store not among them isn't due.
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
		due = !found->swept;
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
	uint32	   *holders;
	uint64	   *reached;
	DIR		   *dir;
	SweepRoots	sr;
	struct dirent *de;
	BlockList	tofree = {0};
	BlockList	recount = {0};
	BlockNumber nrefpages;
	uint32	   *refs;
	RootCounts	rc;

	/* the roots whose capturers never committed let go of first, the rest settled */
	(void) bmap_roots_reclaim(rel, true);
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

	/*
	 * What the captured roots hold, as their counts say once brought up to
	 * date (the nodes they reach counted as reached: their children are
	 * counted with them)
	 */
	(void) root_counts(rel, hw, &rc, false);
	holders = MemoryContextAllocHuge(CurrentMemoryContext,
									 (hw + 1) * sizeof(uint32));
	memset(holders, 0, (hw + 1) * sizeof(uint32));
	reached = MemoryContextAllocHuge(CurrentMemoryContext,
									 (hw / 64 + 1) * sizeof(uint64));
	memset(reached, 0, (hw / 64 + 1) * sizeof(uint64));
	for (BlockNumber b = 0; b < hw && b < rc.n; b++)
	{
		/*
		 * (a count at its limit says at least so many: taken as the most
		 * holders can say, which no pointer taken off lowers)
		 */
		holders[b] = ROOTREF_COUNT(rc.v[b]) == ROOTREF_MAX ? PG_UINT32_MAX :
			ROOTREF_COUNT(rc.v[b]);
		if (rc.v[b] & ROOTREF_IN)
			reached[b / 64] |= UINT64CONST(1) << (b % 64);
	}

	/* what every relation holds */
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

	/* and what the roots not counted yet and the release queue hold */
	{
		SweepHolders sh = {holders, reached, hw};
		SMgrRelation store = store_smgr(rel);

		for (int i = 0; i < rc.nextra; i++)
		{
			const BmapEntry *tops = rc.extra + i * BMAP_TREE_TOPS;
			uint64		first = 0;

			for (int k = 0; k < BMAP_TREE_TOPS; k++)
			{
				if (BlockNumberIsValid(tops[k].block))
					tree_walk_node(store, &tops[k], k, (BlockNumber) first,
								   true, sweep_node, sweep_entry, &sh);
				first += tree_span(k);
			}
		}
		sweep_queue(rel, sweep_node, sweep_entry, sweep_uncount, &sh);
	}
	pfree(rc.v);
	if (rc.extra)
		pfree(rc.extra);

	/* the counts, a page at a time */
	nrefpages = refs_nblocks(rel);
	refs = MemoryContextAllocHuge(CurrentMemoryContext,
								  (Size) Max(nrefpages, 1) * BMAP_REFS_PER_PAGE *
								  sizeof(uint32));
	for (BlockNumber p = 0; p < nrefpages; p++)
	{
		Buffer		buf = refs_read(rel, p);

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		check_refs_page(BufferGetPage(buf), rel, p);
		if (PageIsNew(BufferGetPage(buf)))
			memset(refs + (Size) p * BMAP_REFS_PER_PAGE, 0,
				   BMAP_REFS_PER_PAGE * sizeof(uint32));
		else
			memcpy(refs + (Size) p * BMAP_REFS_PER_PAGE,
				   PageGetBmapRefs(BufferGetPage(buf)),
				   BMAP_REFS_PER_PAGE * sizeof(uint32));
		UnlockReleaseBuffer(buf);
	}

	/*
	 * Nothing holds them: freed, their counts zeroed first if not zero.  The
	 * count of a block something holds is left as it is: a tree's node
	 * copied meanwhile (which this lock doesn't hold off) gave its children a
	 * holder each after their relation was counted, so a count rewritten
	 * from what was counted could be a holder short.  Counts are exact, so
	 * none needs rewriting but one a crash left too high, whose block a sweep
	 * frees once its holders are gone.
	 */
	for (BlockNumber b = 0; b < hw; b++)
	{
		if ((allocated[b / 64] & (UINT64CONST(1) << (b % 64))) == 0)
			continue;
		if (holders[b] == 0)
		{
			block_list_add(&tofree, b);
			if (REF_PAGE(b) < nrefpages && refs[b] != 0)
				block_list_add(&recount, b);
		}
	}
	INJECTION_POINT("bmap-sweep-counted", NULL);
	result->recounted = sweep_recount(rel, recount.blocks, recount.n);

	/*
	 * Each relation's shared_below lowered to what other maps still hold,
	 * while no share can add a holder.  (The roots are read only for a
	 * relation whose shared_below is to be lowered.)
	 */
	pfree(refs);
	memset(&sr, 0, sizeof(sr));
	sr.rel = rel;
	dir = AllocateDir(path);
	while ((de = ReadDir(dir, path)) != NULL)
	{
		RelFileNumber relnumber;
		char	   *end;

		relnumber = (RelFileNumber) strtoul(de->d_name, &end, 10);
		if (end == de->d_name || strcmp(end, "_bmap") != 0)
			continue;
		sweep_shared_below(smgropen(with_relnumber(rel, relnumber),
									INVALID_PROC_NUMBER), holders, hw, &sr);
	}
	FreeDir(dir);
	sweep_roots_free(&sr);
	LockRelease(&tag, ExclusiveLock, false);
	INJECTION_POINT("bmap-sweep-freeing", NULL);

	/* (a block nothing holds gets no new holder: freed after the lock) */
	result->freed = tofree.n - free_blocks(rel, tofree.blocks, tofree.n);

	pfree(allocated);
	pfree(holders);
	pfree(reached);
	if (tofree.blocks)
		pfree(tofree.blocks);
	if (recount.blocks)
		pfree(recount.blocks);
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
	SMgrRelation srels[5];

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
	srels[4] = roots_smgr(rel);
	if (!smgrexists(srels[0], MAIN_FORKNUM) &&
		!smgrexists(srels[1], MAIN_FORKNUM) &&
		!refs_file_exists(srels[2]) &&
		!smgrexists(srels[3], MAIN_FORKNUM) &&
		!smgrexists(srels[4], MAIN_FORKNUM))
		return;
	/* at once, as in redo: nothing may use the tablespace any more */
	smgrdounlinkall(srels, 5, true);
	/* (nor the lengths this backend knew: see refs_known()) */
	for (int i = 0; i < 5; i++)
		srels[i]->smgr_cached_nblocks[MAIN_FORKNUM] = InvalidBlockNumber;
	store_unwatch(rel);
}

/* ----------------------------------------------------------------
 * Inspection, for tests
 * ----------------------------------------------------------------
 */

/*
 * Block 'blkno' of fork 'forknum' (its main fork or its visibility map) of a
 * block-mapped relation: its physical block
 * (InvalidBlockNumber if unmapped), the entry's birth epoch, and how many
 * other maps reach the block: its own count and the counts of the nodes on
 * its path, added up (a node shared by k maps more shares every block under
 * it with them; a sum past a uint32 says the most one holds).
 */
void
bmap_inspect_block(SMgrRelation reln, ForkNumber forknum, BlockNumber blkno,
				   BlockNumber *phys, uint32 *epoch, uint32 *refs)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	BmapEntry	entry;
	TreePath	path;
	BmapEntry	ptr;
	Buffer		buf;
	uint64		n;

	*phys = InvalidBlockNumber;
	*epoch = 0;
	*refs = 0;
	if (!fork_mapped(forknum))
		return;
	blkno = fork_key(forknum, blkno);
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
	*refs = (uint32) Min(n, PG_UINT32_MAX);
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

typedef struct InspectEntries
{
	BmapEntryInspect visit;
	void	   *arg;
} InspectEntries;

static bool
inspect_entries_node(BlockNumber node, uint16 level, BlockNumber first,
					 void *arg)
{
	return true;
}

static void
inspect_entries_entry(const BmapEntry *entry, BlockNumber blkno, void *arg)
{
	InspectEntries *ie = arg;

	if (blkno < BMAP_VM_BASE)
		ie->visit(blkno, entry->block, ie->arg);
}

/*
 * Every entry of a block-mapped relation's map that maps a block of its main
 * fork, its logical block and physical block told to 'visit', in logical
 * order; the holes are not visited, so the cost is of the map's nodes.  The
 * header is held meanwhile.
 */
void
bmap_inspect_entries(SMgrRelation reln, BmapEntryInspect visit, void *arg)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	InspectEntries ie = {visit, arg};
	Buffer		headerbuf;

	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_SHARE);
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	tree_walk(reln, inspect_entries_node, inspect_entries_entry, &ie);
	UnlockReleaseBuffer(headerbuf);
}

/*
 * The logical blocks of its main fork a block-mapped relation's map pages
 * have entries for
 */
uint64
bmap_inspect_map_blocks(SMgrRelation reln)
{
	return Min(map_blocks_below(reln, BMAP_VM_BASE), (uint64) BMAP_VM_BASE);
}

/* a block-mapped relation's epoch */
uint32
bmap_inspect_epoch(SMgrRelation reln)
{
	return lookup_epoch(reln);
}

/* the allocator's high-water mark and free count, of rel's page store */
/*
 * The slot following rel's page store's flushes (BmapStoreSync): its flushes
 * written, and those in flight; false if it has none.
 */
bool
bmap_inspect_store_sync(RelFileLocator rel, uint64 *written, uint32 *flushing)
{
	BmapStoreSync *slot;

	SpinLockAcquire(&bmap_shared->mutex);
	slot = store_sync_slot(rel, false, InvalidXLogRecPtr);
	*written = slot ? slot->written : 0;
	*flushing = slot ? slot->flushing : 0;
	SpinLockRelease(&bmap_shared->mutex);
	return slot != NULL;
}

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
	reln->bmap_pretrunc_forks = 0;
	reln->bmap_read_blkno = InvalidBlockNumber;
}

void
bmapclose(SMgrRelation reln, ForkNumber forknum)
{
	if (fork_mapped(forknum))
		reln->bmap_read_blkno = InvalidBlockNumber;
	mdclose(reln, forknum);
}

void
bmapcreate(SMgrRelation reln, ForkNumber forknum, bool isRedo)
{
	/* (the visibility map is the map's, made with it: see bmapexists()) */
	if (forknum == VISIBILITYMAP_FORKNUM)
		return;
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

/*
 * Storage made in a transaction that drops it if it aborts (its creation
 * registered a pending delete) names that transaction in its map's header,
 * set here by the caller just before.  A crash, or a recovery stopped at a
 * point before the transaction ended, leaves such a map with no commit and
 * no abort record to drop it, holding whatever it was made sharing:
 * bmap_drop_orphans() finds and drops it.  Storage made outside any such
 * transaction (a test's copy) names none and is never taken for an orphan.
 */
static FullTransactionId creating_xact = {0};

void
bmap_creating(FullTransactionId xid)
{
	creating_xact = xid;
}

void
bmap_recovery_done(FullTransactionId next)
{
	pg_atomic_write_u64(&bmap_shared->run_since_xid, U64FromFullTransactionId(next));
}

/* The lock that keeps two scans of one page store for orphans apart. */
#define ORPHANS_LOCK(tag, spc, db) \
	SET_LOCKTAG_OBJECT(tag, db, TableSpaceRelationId, spc, 0x626d)

/*
 * The maps of the database's page store in tablespace spcOid made by a
 * transaction that is neither in progress nor committed: it aborted
 * without its abort's record, or the server crashed or a recovery stopped
 * before it ended.  Each is dropped at the commit of the caller's
 * transaction, as any relation dropped then is: its holds queued for
 * release and its map unlinked, logged in the commit record, so that
 * redo does it too.  A transaction older than the commit log keeps is
 * taken to have committed.  Returns how many.
 */
int
bmap_drop_orphans(Oid spcOid, Oid dbOid)
{
	RelFileLocator rel = {spcOid, dbOid, InvalidRelFileNumber};
	char	   *path;
	DIR		   *dir;
	struct dirent *de;
	int			n = 0;
	FullTransactionId since;
	LOCKTAG		tag;

	if (RecoveryInProgress())
		return 0;
	since = FullTransactionIdFromU64(pg_atomic_read_u64(&bmap_shared->run_since_xid));
	if (!FullTransactionIdIsValid(since))
		return 0;

	/*
	 * One scan at a time, the lock held to this transaction's end; a later
	 * one finds what this one took as taken (below).
	 */
	ORPHANS_LOCK(tag, spcOid, dbOid);
	(void) LockAcquire(&tag, ExclusiveLock, false, false);

	path = GetDatabasePath(dbOid, spcOid);
	dir = AllocateDir(path);
	while ((de = ReadDir(dir, path)) != NULL)
	{
		RelFileNumber relnumber;
		char	   *end;
		SMgrRelation reln;
		Buffer		buf;
		Page		page;
		FullTransactionId creator;

		relnumber = (RelFileNumber) strtoul(de->d_name, &end, 10);
		if (end == de->d_name || strcmp(end, "_bmap") != 0)
			continue;
		rel.relNumber = relnumber;
		reln = smgropen(rel, INVALID_PROC_NUMBER);
		if (!mdexists(reln, BMAP_FORKNUM) || mdnblocks(reln, BMAP_FORKNUM) == 0)
			continue;
		buf = map_read(rel, 0);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
		creator = PageIsNew(page) ? InvalidFullTransactionId :
			PageGetBmapHeader(page)->creator;

		/*
		 * An orphan: made by a transaction that began before recovery
		 * ended, so by no backend running now, that is not prepared (nor
		 * being finished by COMMIT or ROLLBACK PREPARED, which drops its
		 * storage itself), and that ended without committing (owner_state()
		 * compares full transaction ids, and knows one too old for the
		 * commit log for unknown, which is kept).
		 */
		if (!FullTransactionIdIsValid(creator) ||
			!FullTransactionIdPrecedes(creator, since) ||
			TwoPhaseTransactionIsKnown(creator) ||
			owner_state(creator) != OWNER_ENDED)
		{
			UnlockReleaseBuffer(buf);
			continue;
		}

		/*
		 * Taken: the map names this transaction instead, which began after
		 * recovery ended, so no other scan takes it; if this one aborts, the
		 * next start finds it again.  Logged, as the header always is.
		 */
		{
			xl_bmap_header_delta delta;
			xl_bmap_tree xlrec;
			XLogRecPtr	lsn;

			header_delta_of(page, &delta);
			delta.flags = BMAP_DELTA_CREATOR;
			delta.creator = GetTopFullTransactionId();
			xlrec.locator = rel;
			xlrec.op = BMAP_TREE_CREATOR;
			xlrec.flags = 0;
			xlrec.node = xlrec.from = InvalidBlockNumber;
			xlrec.old_hw = xlrec.new_hw = InvalidBlockNumber;
			START_CRIT_SECTION();
			apply_header_delta(page, &delta);
			MarkBufferDirty(buf);
			XLogBeginInsert();
			XLogRegisterData(&xlrec, sizeof(xlrec));
			XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
			XLogRegisterBufData(0, &delta, sizeof(delta));
			lsn = XLogInsert(RM_BMAP_ID, XLOG_BMAP_TREE);
			PageSetLSN(page, lsn);
			END_CRIT_SECTION();
		}
		UnlockReleaseBuffer(buf);
		RelationDropStorageAtCommit(rel);
		ereport(LOG,
				(errmsg("dropping storage \"%s\" made by transaction %" PRIu64 ", which did not commit",
						relpath(reln->smgr_rlocator, MAIN_FORKNUM).str,
						U64FromFullTransactionId(creator))));
		n++;
	}
	FreeDir(dir);
	pfree(path);
	return n;
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

	memset(&delta, 0, sizeof(delta));
	delta.kind = BMAP_DELTA_HEADER;
	delta.flags = BMAP_DELTA_INIT;
	delta.nblocks = 0;
	delta.vm_nblocks = 0;
	delta.free_space_unknown = 0;
	delta.epoch = 0;
	delta.shared_below = 0;
	/* the transaction making it, which drops it if it aborts (not one an error left) */
	if (FullTransactionIdIsValid(creating_xact) &&
		FullTransactionIdEquals(creating_xact, GetTopFullTransactionIdIfAny()))
	{
		delta.flags |= BMAP_DELTA_CREATOR;
		delta.creator = creating_xact;
		creating_xact = InvalidFullTransactionId;
	}
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
	/* (the map's identity: where its record began, which no other did) */
	PageGetBmapRoot(BufferGetPage(rootbuf))->map_id = ProcLastRecPtr;
	PageSetLSN(page, lsn);
	PageSetLSN(BufferGetPage(rootbuf), lsn);
	END_CRIT_SECTION();

	UnlockReleaseBuffer(buf);
	UnlockReleaseBuffer(rootbuf);
}

/*
 * The map fork is the storage manager's own, so it is hidden from callers
 * that go through every fork (copying a relation, measuring it).  The
 * visibility map is in the map's tree, so it exists as the relation does,
 * at first with no blocks: it is made with the map and goes with it.
 */
bool
bmapexists(SMgrRelation reln, ForkNumber forknum)
{
	if (fork_mapped(forknum))
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
			if (f != BMAP_FORKNUM && f != VISIBILITYMAP_FORKNUM)
				mdunlink(rlocator, f, isRedo);
		bmapunlink(rlocator, MAIN_FORKNUM, isRedo);
		return;
	}
	/* (the visibility map goes with the map) */
	if (forknum == VISIBILITYMAP_FORKNUM)
		return;
	if (forknum != MAIN_FORKNUM)
	{
		mdunlink(rlocator, forknum, isRedo);
		return;
	}

	/* (its statistics go with it; replay keeps none) */
	if (!isRedo)
		pgstat_drop_block_map(rlocator.locator.spcOid, rlocator.locator.dbOid,
							  rlocator.locator.relNumber);

	if (isRedo)
	{
		/*
		 * The primary logged the frees; in recovery nothing reuses the name.
		 * The map's buffers go first: dropping the relation's buffers left
		 * them, taking the fork smgrexists() hides for absent, and one
		 * written out later would find no file.
		 */
		ForkNumber	fork = BMAP_FORKNUM;
		BlockNumber first = 0;

		DropRelationBuffers(smgropen(rlocator.locator, INVALID_PROC_NUMBER),
							&fork, 1, &first);
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
	ForkNumber	forks[2] = {MAIN_FORKNUM, VISIBILITYMAP_FORKNUM};
	BlockNumber firsts[2] = {0, 0};

	if (isRedo || !mdexists(reln, BMAP_FORKNUM))
		return;

	/*
	 * The relation's pages, and its visibility map's, leave shared buffers
	 * first: once a block is free, another relation may be given it, and a
	 * dirty page still cached here would be written onto it through this
	 * relation's map.  The map's own pages stay until the blocks are freed.
	 */
	DropRelationBuffers(reln, forks, 2, firsts);
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
 * its page store has fewer free blocks than that and its release queue isn't
 * empty, take release steps first (not waiting for another drain or a sweep),
 * until enough are free, the queue is empty or its top waits for a drop, or
 * BMAP_GROWTH_RELEASE_STEPS were taken, so that dropped storage is reused
 * before the store grows.  Only in a transaction.  Autovacuum drains the
 * queue in the background too (bmapmaintain()), but only when it visits the
 * database, and a store grown meanwhile would keep the blocks it grew by.
 * The caller holds no extension lock (a step takes locks of its own), but
 * may hold a relation's page locked, as a B-tree split does: a step locks
 * the allocator's pages and counts, as the extension's allocation does, and
 * the queue's pages, none of which a backend waits for while holding a
 * relation's page.
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

	if (forknum != MAIN_FORKNUM || nblocks == 0 ||
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
}

/*
 * Background maintenance of the current database's page stores, one in
 * each tablespace that has one.  First their release queues are drained, a
 * step at a time, not waiting for another drain or a sweep (a queue being
 * drained by another backend is passed over: it is being let go of
 * already), until one is empty or its top waits for a drop yet to finish, or
 * release_max entries in all were let go of (-1: no limit), pace called
 * after each step.  A step pops up to a record's worth
 * of entries, so the last may go past the limit by that much.  Then each
 * store whose sweep is due (bmap_sweep_due()) is swept.  A sweep drains its
 * queue in full first, whatever the limit, and isn't paced, as it holds off
 * shares and the reuse of freed blocks while it runs; but one is due rarely:
 * once after the server starts, and after a count it could fix has lost a
 * holder.
 */
void
bmapmaintain(int64 release_max, void (*pace) (void), SMgrMaintenance *done)
{
	List	   *spcs;
	BmapRelease released = {0};

	if (RecoveryInProgress())
		return;
	spcs = store_tablespaces();
	foreach_oid(spc, spcs)
	{
		while (release_max < 0 || released.entries < (uint64) release_max)
		{
			if (!bmap_release_step(spc, MyDatabaseId, false, &released))
				break;
			if (pace)
				pace();
			else
				CHECK_FOR_INTERRUPTS();
		}
	}
	done->released += released.entries;
	done->released_freed += released.freed;

	foreach_oid(spc, spcs)
	{
		BmapSweep	swept;

		if (!bmap_sweep_due(spc, MyDatabaseId))
			continue;
		/* (a sweep is due after a start: what a crash orphaned goes too) */
		done->orphans += bmap_drop_orphans(spc, MyDatabaseId);
		bmap_sweep(spc, MyDatabaseId, &swept);
		done->swept++;
		done->swept_freed += swept.freed;
	}
	list_free(spcs);
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

/*
 * The most blocks fork 'forknum' can have: the main fork stops short of
 * the visibility map's blocks of the tree.
 */
static BlockNumber
fork_max_blocks(ForkNumber forknum)
{
	return forknum == VISIBILITYMAP_FORKNUM ? BMAP_VM_SPAN : BMAP_VM_BASE;
}

/* refuse to extend fork 'forknum' past block 'end' - 1 if it can't have it */
static void
check_extend_limit(SMgrRelation reln, ForkNumber forknum, uint64 end)
{
	if (end > fork_max_blocks(forknum))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("cannot extend file \"%s\" beyond %u blocks",
						relpath(reln->smgr_rlocator, forknum).str,
						fork_max_blocks(forknum))));
}

void
bmapextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		   const void *buffer, bool skipFsync)
{
	if (!fork_mapped(forknum))
	{
		mdextend(reln, forknum, blocknum, buffer, skipFsync);
		return;
	}
	/* (as md.c's, which can't name the block after the last) */
	check_extend_limit(reln, forknum, (uint64) blocknum + 1);

	/*
	 * (bulk writes and copies: no shared buffer, nothing else counts it)
	 * Only the block written is mapped: a write past the end leaves a hole
	 * between, as it does in md.c's file.
	 */
	extend_to(reln, fork_key(forknum, blocknum),
			  (uint64) fork_key(forknum, blocknum) + 1, buffer, skipFsync);
}

void
bmapzeroextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			   int nblocks, bool skipFsync)
{
	if (!fork_mapped(forknum))
	{
		mdzeroextend(reln, forknum, blocknum, nblocks, skipFsync);
		return;
	}
	/*
	 * (shared buffers extending: pg_stat_io counts these as extends)  Every
	 * block is mapped, as md.c allocates them in its file (fallocate()):
	 * the buffer manager zero-extends by the pages it is about to hand out
	 * to be initialized, and settles them as the relation's own, needing no
	 * preparing before they change.  Left as holes, each would be given its
	 * block by a record of its own at its first change, where mapping them
	 * here takes one record for a leaf's worth.
	 */
	/*
	 * (as md.c's, which stops short of a length of InvalidBlockNumber: a
	 * cached length would read as unknown)
	 */
	check_extend_limit(reln, forknum, (uint64) blocknum + nblocks);
	if (nblocks > 0)
		extend_to(reln, fork_key(forknum, blocknum),
				  (uint64) fork_key(forknum, blocknum) + nblocks, NULL,
				  skipFsync);
}

/*
 * Allocate blocks before their first WAL record: callers that log a page's
 * image before writing it call this first, so that redo finds the block
 * already mapped.  Only the blocks named are mapped; any between the
 * relation's end and them are left holes.
 */
void
bmappreextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			  int nblocks)
{
	if (!fork_mapped(forknum) || nblocks <= 0)
		return;
	/* (no block past the fork's last, as smgrextend() would refuse it) */
	check_extend_limit(reln, forknum, (uint64) blocknum + nblocks);
	extend_to(reln, fork_key(forknum, blocknum),
			  (uint64) fork_key(forknum, blocknum) + nblocks, NULL, true);
}

bool
bmapprefetch(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			 int nblocks)
{
	SMgrRelation ssrel;

	if (!fork_mapped(forknum))
		return mdprefetch(reln, forknum, blocknum, nblocks);

	blocknum = fork_key(forknum, blocknum);
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

	if (!fork_mapped(forknum))
		return mdmaxcombine(reln, forknum, blocknum);
	blocknum = fork_key(forknum, blocknum);

	/* (a hole reads as zeroes, without I/O: see bmapprepareread()) */
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

	if (!fork_mapped(forknum))
	{
		mdreadv(reln, forknum, blocknum, buffers, nblocks);
		return;
	}
	blocknum = fork_key(forknum, blocknum);
	ssrel = store_smgr(reln->smgr_rlocator.locator);
	while (nblocks > 0)
	{
		BlockNumber run;
		BlockNumber phys = hole_or_lookup(reln, blocknum, nblocks, &run);

		if (BlockNumberIsValid(phys))
			mdreadv(ssrel, MAIN_FORKNUM, phys, buffers, run);
		else
		{
			for (BlockNumber i = 0; i < run; i++)
				memset(buffers[i], 0, BLCKSZ);
		}
		blocknum += run;
		buffers += run;
		nblocks -= run;
	}
}

/*
 * Look up an asynchronous read's blocks before its I/O handle is acquired:
 * reading the map may need a handle of its own.  A hole has no block to
 * read: how many blocks from 'blocknum' are holes is returned, and the
 * caller fills their pages with zeroes itself, starting no read for them.
 */
BlockNumber
bmapprepareread(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
				BlockNumber nblocks)
{
	if (!fork_mapped(forknum))
		return 0;
	blocknum = fork_key(forknum, blocknum);
	reln->bmap_read_phys = hole_or_lookup(reln, blocknum, nblocks,
										  &reln->bmap_read_run);
	if (!BlockNumberIsValid(reln->bmap_read_phys))
	{
		reln->bmap_read_blkno = InvalidBlockNumber;
		return reln->bmap_read_run;
	}
	reln->bmap_read_blkno = blocknum;
	return 0;
}

void
bmapstartreadv(PgAioHandle *ioh, SMgrRelation reln, ForkNumber forknum,
			   BlockNumber blocknum, void **buffers, BlockNumber nblocks)
{
	BlockNumber phys;

	if (!fork_mapped(forknum))
	{
		mdstartreadv(ioh, reln, forknum, blocknum, buffers, nblocks);
		return;
	}
	blocknum = fork_key(forknum, blocknum);

	/*
	 * smgrprepareread() looked the blocks up, and smgrmaxcombine() limited
	 * the read to a physically consecutive run.  The read is started on the
	 * page store itself, so the I/O's target needs no map to be reopened
	 * elsewhere.
	 */
	if (reln->bmap_read_blkno != blocknum)
		elog(ERROR, "read of block %u of %s was not prepared",
			 key_block(blocknum), relpath(reln->smgr_rlocator, forknum).str);
	phys = reln->bmap_read_phys;
	reln->bmap_read_blkno = InvalidBlockNumber;
	if (reln->bmap_read_run < nblocks)
		elog(ERROR, "read of %u blocks at block %u of %s crosses a gap in its block map",
			 nblocks, key_block(blocknum), relpath(reln->smgr_rlocator, forknum).str);
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

	if (!fork_mapped(forknum))
	{
		mdwritev(reln, forknum, blocknum, buffers, nblocks, skipFsync);
		return;
	}
	blocknum = fork_key(forknum, blocknum);
	INJECTION_POINT("bmap-write", NULL);
	ssrel = store_smgr(reln->smgr_rlocator.locator);
	while (nblocks > 0)
	{
		BlockNumber run;
		BlockNumber phys = lookup_ext(reln, blocknum, recovery ? 1 : nblocks,
									  &run, true, true);

		if (!BlockNumberIsValid(phys) &&
			pg_memory_is_all_zeros(buffers[0], BLCKSZ))
		{
			/*
			 * A page of zeroes with no entry: a hole reads as zeroes already,
			 * and the page is given a block only when it changes.
			 */
			run = 1;
		}
		else if (!BlockNumberIsValid(phys) && !recovery)
			(void) lookup_ext(reln, blocknum, 1, NULL, false, true);
		else if (!BlockNumberIsValid(phys) && forknum == VISIBILITYMAP_FORKNUM)
		{
			/*
			 * In recovery, a visibility map's page with no block: redo
			 * extended the map past the header's length without a record
			 * of the primary's (extend_in_recovery()), or the block was let
			 * go of since.  Redo gives none, so the page goes nowhere: what
			 * it loses is bits set at most, and a bit not set claims
			 * nothing of the heap's page.
			 */
			elog(DEBUG1, "block %u of the visibility map of %s, without an entry, not written in recovery",
				 key_block(blocknum), relpath(reln->smgr_rlocator, MAIN_FORKNUM).str);
			run = 1;
		}
		else if (!BlockNumberIsValid(phys))
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
				(blocknum < header_length(reln, MAIN_FORKNUM, true, &lsn) ||
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

	if (!fork_mapped(forknum))
	{
		mdwriteback(reln, forknum, blocknum, nblocks);
		return;
	}
	blocknum = fork_key(forknum, blocknum);
	INJECTION_POINT("bmap-writeback", NULL);
	ssrel = store_smgr(reln->smgr_rlocator.locator);
	while (nblocks > 0)
	{
		BlockNumber run;
		BlockNumber phys = lookup_cached(reln, blocknum, nblocks, &run);

		/*
		 * Writeback is a hint; blocks without an entry, or whose map page
		 * isn't cached, are skipped.  So are a relation's dropped since its
		 * pages were written: its map's buffers went with it, and its
		 * map's file isn't opened (read_entries()).
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

	if (!fork_mapped(forknum))
		return mdnblocks(reln, forknum);
	nblocks = header_length(reln, forknum, false, NULL);
	/* (a length past the header's, see extend_in_recovery()) */
	if (InRecovery)
		nblocks = Max(nblocks, recovery_length(reln->smgr_rlocator.locator,
											   forknum));
	return nblocks;
}

/*
 * A truncation writes two records: XLOG_SMGR_TRUNCATE, flushed before any
 * buffer is dropped, then the header's own (set_length()), which nothing
 * flushes.  A crash in between leaves only the first durable, whose replay
 * drops the buffers past the new length: were the header left long, those
 * pages would be read again from disk, older than the records replayed
 * onto them (rows deleted before the truncation would come back).  So the
 * first record's replay cuts the header itself, as the second record does;
 * that one's replay, if it comes, applies the same again.  A header newer
 * than the first record's end means the second is durable (the header is
 * held from one record to the other): then the header is left to it.  A
 * header at the record's end was changed by an earlier replay of it, which
 * a crash interrupted.
 *
 * In redo, before the truncation's critical section: the header, held
 * exclusively.
 */
static void
pretruncate_in_recovery(SMgrRelation reln)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		headerbuf;

	reln->bmap_pretrunc = InvalidBuffer;
	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	reln->bmap_pretrunc = headerbuf;
	/* (run in the truncation's critical section, which can't load it) */
	INJECTION_POINT_LOAD("bmap-truncated-in-recovery");
}

/*
 * In redo, in XLOG_SMGR_TRUNCATE's critical section: cut the header's
 * length of fork 'forknum' to 'nblocks', unless the header's own record is
 * durable (see pretruncate_in_recovery()).  The header takes this record's
 * end as its LSN, so that the header's record still replays onto it.  A
 * record truncating both the main fork and the visibility map cuts both:
 * the second finds the header at the record's end already, and cuts it as
 * the first did, if its own length is still longer.  The header is let go
 * of once the last fork is cut.
 */
static void
truncate_in_recovery(SMgrRelation reln, ForkNumber forknum,
					 BlockNumber nblocks)
{
	Buffer		headerbuf = reln->bmap_pretrunc;
	XLogRecPtr	end = GetCurrentReplayRecPtr(NULL);
	Page		hpage;
	BmapHeader *header;
	BlockNumber cur;

	if (!BufferIsValid(headerbuf))
		elog(PANIC, "block-mapped relation %s truncated in recovery without being prepared",
			 relpath(reln->smgr_rlocator, MAIN_FORKNUM).str);
	hpage = BufferGetPage(headerbuf);
	header = PageGetBmapHeader(hpage);
	cur = forknum == VISIBILITYMAP_FORKNUM ? header->vm_nblocks : header->nblocks;

	/* (an earlier replay changed the header only if it cut it) */
	if (PageGetLSN(hpage) <= end &&
		(PageGetLSN(hpage) == end || nblocks < cur))
	{
		if (nblocks < cur)
		{
			xl_bmap_header_delta delta;

			header_delta_of(hpage, &delta);
			if (forknum == VISIBILITYMAP_FORKNUM)
				delta.vm_nblocks = nblocks;
			else
				delta.nblocks = nblocks;
			apply_header_delta(hpage, &delta);
			PageSetLSN(hpage, end);
			MarkBufferDirty(headerbuf);
		}
		/* (the header changed, still locked) */
		if (forknum == MAIN_FORKNUM)
			INJECTION_POINT_CACHED("bmap-truncated-in-recovery", &headerbuf);
	}
	reln->smgr_cached_nblocks[forknum] = InvalidBlockNumber;
	if (--reln->bmap_pretrunc_forks <= 0)
	{
		reln->bmap_pretrunc = InvalidBuffer;
		UnlockReleaseBuffer(headerbuf);
	}
}

/*
 * Truncation happens inside a critical section, where reading a buffer
 * isn't safe, so the header is read beforehand, and held exclusively until
 * the truncation of the last of the forks it maps (the main fork, the
 * visibility map: a relation's truncation cuts both at once).
 */
void
bmappretruncate(SMgrRelation reln, ForkNumber *forknum, int nforks)
{
	RelFileLocator rel = reln->smgr_rlocator.locator;
	Buffer		headerbuf;
	int			mapped = 0;

	for (int i = 0; i < nforks; i++)
		if (fork_mapped(forknum[i]))
			mapped++;
	if (mapped == 0)
		return;
	reln->bmap_pretrunc_forks = mapped;
	if (InRecovery)
	{
		pretruncate_in_recovery(reln);
		return;
	}

	/* (pins a failed truncation left are gone with its resource owner) */
	reln->bmap_pretrunc = InvalidBuffer;

	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	reln->bmap_pretrunc = headerbuf;
	INJECTION_POINT("bmap-pretruncated", NULL);
	/* (run in the truncation's critical section, which can't load it) */
	INJECTION_POINT_LOAD("bmap-truncating");
}

void
bmaptruncate(SMgrRelation reln, ForkNumber forknum, BlockNumber curnblk,
			 BlockNumber nblocks)
{
	Buffer		headerbuf;

	if (!fork_mapped(forknum))
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
		truncate_in_recovery(reln, forknum, nblocks);
		cut_recovery_length(reln->smgr_rlocator.locator, forknum, nblocks);
		return;
	}

	headerbuf = reln->bmap_pretrunc;
	if (!BufferIsValid(headerbuf))
		elog(ERROR, "block-mapped relation %s truncated without being prepared",
			 relpath(reln->smgr_rlocator, forknum).str);
	if (forknum == MAIN_FORKNUM)
		INJECTION_POINT_CACHED("bmap-truncating", NULL);

	/*
	 * Blocks past the new length stay mapped until the truncation is done
	 * (bmapposttruncate()).
	 */
	if (nblocks < curnblk)
		set_length(reln->smgr_rlocator.locator, headerbuf, forknum, nblocks);
	if (--reln->bmap_pretrunc_forks <= 0)
	{
		reln->bmap_pretrunc = InvalidBuffer;
		UnlockReleaseBuffer(headerbuf);
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
 * Let go of the entries of blocks [from, end) of reln's map, freeing the
 * blocks no other map holds; the header 'headerbuf' is locked exclusively,
 * and the blocks are past the relation's length.  Only the leaves the map
 * has are read (tree_next_leaf()), so a map with holes costs its leaves,
 * not the blocks between them.
 */
static void
let_go_range(SMgrRelation reln, Buffer headerbuf, uint64 from, uint64 end)
{
	uint32		epoch = PageGetBmapHeader(BufferGetPage(headerbuf))->epoch;

	for (uint64 page = from; page < end;)
	{
		uint64		pageend;
		uint64		first;
		uint64		last;
		MapPage		leaf;
		TreePath	path;

		page = Max(page, tree_next_leaf(reln, page));
		if (page >= end)
			break;
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
			mapped_span(reln, &leaf, page, pageend, &first, &last))
		{
			(void) tree_leaf_for(reln, headerbuf, (BlockNumber) page, false,
								 true, &leaf);
			while (first < last)
			{
				uint64		to = Min(last, first + BMAP_PIECE);

				unshare_stale(reln, &leaf, epoch,
							  (BlockNumber) first, (BlockNumber) to, true);
				first = to;
			}
		}
		page = pageend;
	}
}

/*
 * Once a truncation is done, outside its critical section: let go of the
 * entries past the relation's length, this truncation's and any an earlier
 * one left (a crash before this), freeing the blocks no other map holds.
 * Kept, they would be held until the relation grew again
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

	if (!fork_mapped(forknum) || InRecovery)
		return;
	if (forknum == MAIN_FORKNUM)
		INJECTION_POINT("bmap-truncated", NULL);
	headerbuf = map_read(rel, 0);
	LockBuffer(headerbuf, BUFFER_LOCK_EXCLUSIVE);
	check_page(BufferGetPage(headerbuf), BMAP_PAGE_HEADER, rel, BMAP_FORKNUM, 0);
	header = PageGetBmapHeader(BufferGetPage(headerbuf));

	/* (the main fork's past its length up to the visibility map's blocks) */
	if (forknum == VISIBILITYMAP_FORKNUM)
		let_go_range(reln, headerbuf,
					 (uint64) BMAP_VM_BASE + header->vm_nblocks,
					 Max(map_blocks(reln), (uint64) BMAP_VM_BASE));
	else
		let_go_range(reln, headerbuf, header->nblocks,
					 map_blocks_below(reln, BMAP_VM_BASE));
	UnlockReleaseBuffer(headerbuf);
}

void
bmapimmedsync(SMgrRelation reln, ForkNumber forknum)
{
	if (!fork_mapped(forknum))
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
	if (!fork_mapped(forknum))
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

	if (!fork_mapped(forknum))
		return mdfd(reln, forknum, blocknum, off);
	phys = lookup(reln, fork_key(forknum, blocknum), 1, NULL);
	return mdfd(store_smgr(reln->smgr_rlocator.locator), MAIN_FORKNUM, phys,
				off);
}

/* ----------------------------------------------------------------
 * Redo
 * ----------------------------------------------------------------
 */

/*
 * Redo's view of a map entry never goes backwards.  A map page read
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
 * newer page's too.
 */
static void
keep_newer_entries(Buffer buf, char *newer)
{
	Page		page = BufferGetPage(buf);
	BmapEntry  *entries = PageGetBmapEntries(page);
	const BmapEntry *kept = PageGetBmapEntries(newer);
	bool		any = false;
	uint32		nslots = BMAP_TREE_FANOUT;

	/* (a root page has its pointers, a node its entries) */
	if (map_page_is(newer, BMAP_PAGE_ROOT))
		nslots = BMAP_TREE_TOPS;
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
		PageSetLSN(page, Max(PageGetLSN(page), PageGetLSN(newer)));
		MarkBufferDirty(buf);
	}
}

/*
 * In redo: does page-store block 'blk' hold a page that is no tree's node
 * (nor all zeroes)?  As replay has it in a buffer, if it does; else on
 * disk, read as it is (through a buffer, a relation's page would fail its
 * checksum, which is of its logical block).
 */
static bool
redo_block_not_node(RelFileLocator rel, BlockNumber blk)
{
	SMgrRelation ssrel = store_smgr(rel);
	RelFileLocator sloc = with_relnumber(rel, BMAP_STORE_RELNUMBER);
	PrefetchBufferResult r;
	PGIOAlignedBlock page;
	void	   *pages[1] = {page.data};

	if (blk >= mdnblocks(ssrel, MAIN_FORKNUM))
		return false;
	r = PrefetchSharedBuffer(ssrel, MAIN_FORKNUM, blk);
	if (BufferIsValid(r.recent_buffer) &&
		ReadRecentBuffer(sloc, MAIN_FORKNUM, blk, r.recent_buffer))
	{
		bool		other;

		LockBuffer(r.recent_buffer, BUFFER_LOCK_SHARE);
		other = !PageIsNew(BufferGetPage(r.recent_buffer)) &&
			!tree_node_any(BufferGetPage(r.recent_buffer));
		UnlockReleaseBuffer(r.recent_buffer);
		return other;
	}
	mdreadv(ssrel, MAIN_FORKNUM, blk, pages, 1);
	return !PageIsNew(page.data) && !tree_node_any(page.data);
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

	/*
	 * A node's entries changed, on a block holding something else on disk
	 * now (freed since and made a relation's page, written out before a
	 * crash): the node was let go of after the record, and nothing reads it
	 * any more.  The block is left as it is; read through a buffer, a
	 * relation's page would fail its checksum, which is of its logical
	 * block.
	 */
	if (node && !(flags & BMAP_DELTA_INIT) && redo_block_not_node(rlocator, blkno))
	{
		pfree(copy);
		return;
	}

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
		map_page_is(BufferGetPage(buf), BMAP_PAGE_ROOT))
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
				PageGetBmapRoot(page)->map_id = record->ReadRecPtr;
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
			case BMAP_DELTA_QUEUE_META:
				apply_queue_meta_delta(page, (xl_bmap_queue_meta_delta *) copy);
				break;
			case BMAP_DELTA_QUEUE:
				apply_queue_delta(page, (xl_bmap_queue_delta *) copy);
				break;
			case BMAP_DELTA_ROOTS:
				apply_roots_delta(page, (xl_bmap_roots_delta *) copy);
				break;
			case BMAP_DELTA_ROOTS_META:
				apply_roots_meta_delta(page, (xl_bmap_roots_meta_delta *) copy);
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
	reln->smgr_cached_nblocks[VISIBILITYMAP_FORKNUM] = InvalidBlockNumber;
}

/*
 * In redo: make sure no later recovery of this server begins before 'need'
 * (its restart point, a standby's last restartpoint or a backup's start,
 * at or past it), asking for a restartpoint and waiting for it if not.
 * 'what' names the record's kind for the log.  False if it is still
 * before: a restartpoint wasn't made (none can be before the replay is
 * consistent, nor in a server running alone).
 */
static bool
redo_restart_point_at(XLogRecPtr need, const char *what)
{
	if (last_checkpoint_redo() >= need)
		return true;
	if (!IsUnderPostmaster)
		return false;
	elog(DEBUG1, "redo of %s asks for a restartpoint: its restart point %X/%08X is before %X/%08X",
		 what, LSN_FORMAT_ARGS(last_checkpoint_redo()), LSN_FORMAT_ARGS(need));
	/* (a test takes the restartpoint for one that couldn't be made) */
	if (!IS_INJECTION_POINT_ATTACHED("bmap-redo-no-restartpoint"))
		RequestCheckpoint(CHECKPOINT_FAST | CHECKPOINT_WAIT);
	return last_checkpoint_redo() >= need;
}

/*
 * In redo of a share that synced its source's own blocks instead of logging
 * them (BMAP_SHARE_SYNCED; see bmapshare()), once the source's pages are
 * written out: the page store is synced, so that this server holds the
 * blocks durably, as the primary does, before replay goes on to anything
 * that may change a map (a later recovery that skips the source's records
 * for a block because its map changed finds the block synced).  And no
 * later recovery of this server may begin at or before the newest of the
 * blocks was given to the source ('synced_since'): replay from there could
 * write something else over the block (its allocation zeroes it, an
 * earlier holder's page may be written out there) and then skip the
 * source's records that made it the page again.  The primary's crash
 * recovery begins past it; this server's restart point, its last
 * restartpoint (a standby's lag behind the primary's checkpoints), or a
 * base backup's start, may not, so a restartpoint is asked for and waited
 * for.  It begins at the last checkpoint record replayed, which is no
 * older than the checkpoint the share began after, past every block the
 * share synced.  A restartpoint can't always be made: none is while
 * recovery has references to pages it couldn't find, and a checkpoint
 * record replayed meanwhile (a backup's replay passing one before it
 * reaches consistency) is never one's.  So the position is kept in
 * pg_control too (XLogNoteSyncedPages()), where it keeps an online backup
 * of this server, and a recovery, from beginning at or before it until a
 * restartpoint past it is made.
 */
static void
redo_share_synced(const xl_bmap_share *xlrec, XLogRecPtr lsn)
{
	mdimmedsync(store_smgr(xlrec->src), MAIN_FORKNUM);
	XLogNoteSyncedReplay();
	if (!redo_restart_point_at(xlrec->synced_since + 1,
							   psprintf("a share of %s",
										relpathperm(xlrec->src, MAIN_FORKNUM).str)) &&
		IsUnderPostmaster)
		ereport(WARNING,
				(errmsg("restart point of recovery %X/%08X is not past the blocks a share of \"%s\" synced",
						LSN_FORMAT_ARGS(last_checkpoint_redo()),
						relpathperm(xlrec->src, MAIN_FORKNUM).str),
				 errdetail("Until a restartpoint is made past %X/%08X, an online backup of this server is refused and a recovery from its restart point fails.",
						   LSN_FORMAT_ARGS(xlrec->synced_since))));

	/*
	 * Said in pg_control once the restartpoint is made, if it could be: a
	 * crash before then recovers from the restart point before it, which
	 * finds every map as at this record, so replays the source's records
	 * for its blocks.  If it couldn't be made, recovery from the restart
	 * point would fail, and so would a backup beginning there, until a
	 * restartpoint past the blocks is made.
	 */
	XLogNoteSyncedPages(xlrec->synced_since);
	elog(DEBUG1, "redo of a share of %s synced its own pages, its restart point %X/%08X, the newest of them given at %X/%08X",
		 relpathperm(xlrec->src, MAIN_FORKNUM).str,
		 LSN_FORMAT_ARGS(last_checkpoint_redo()),
		 LSN_FORMAT_ARGS(xlrec->synced_since));
}

/*
 * The newest copy made without an image (BMAP_REMAP_COPIED) this replay
 * replayed (from its buffer or its old block: a recovery replaying it
 * again may take the other), before which the restart point may still be: what a PASSED record's redo moves the restart
 * point to (redo_copies_passed()).  The startup process's alone, which
 * replays every such copy again after a restart, from the restart point.
 */
static XLogRecPtr redo_copied_newest = InvalidXLogRecPtr;

/*
 * In redo of a copy of a shared page logged without the page's image
 * (XLOG_BMAP_REMAP's block 'block_id', the page the copy is of): the page
 * locked exclusively in its buffer as replay has it, taken before the
 * leaf names the new block, or InvalidBuffer if the page was given other
 * storage after the record (smgrreplacedafter()), which needs nothing
 * from it.  The caller sets its LSN and dirties it, so that it is written
 * to the new block.
 *
 * The primary logs no image when the page has changed since the redo
 * pointer (XLogInsert()'s rule for a full-page write), so that a page's
 * image or init since then is in the WAL, from which replay rebuilds the
 * page in its buffer.  A buffer that holds the page is that page, if the
 * map as replay reads it gave the block its storage no later than the
 * page's last change: replay then skipped none of the page's records since
 * (it skips those from before the map's position, but restores images).
 * Otherwise (no buffer: evicted, or never read; or one replay may have
 * skipped records of, its map on disk newer than they are), the page is
 * read from the old block itself, never through the map, which may name
 * the new block already (written out after this record before a crash).
 * Replay writes no page there meanwhile: an evicted page goes through the
 * map, which names the old block only while replay applies the page's
 * records, so that it writes there the page as replay rebuilt it.  The
 * old block holds the page as the copy saw it: shared, it hasn't changed
 * in place since it was made durable as it came to be shared (synced, or
 * written by the redo of its image), and no hint is set on it; and it
 * isn't changed in place nor freed until the restart point of every
 * server that may replay this record is past it (see "Copies without an
 * image").
 */
static Buffer
redo_remap_page(XLogReaderState *record, uint8 block_id,
				const xl_bmap_remap *xlrec)
{
	RelFileLocator rlocator;
	ForkNumber	forknum;
	BlockNumber blkno;
	SMgrRelation reln;
	PrefetchBufferResult r;
	Buffer		buf;
	PGIOAlignedBlock copy;
	void	   *pages[1] = {copy.data};

	XLogRecGetBlockTag(record, block_id, &rlocator, &forknum, &blkno);
	Assert(forknum == MAIN_FORKNUM);
	reln = smgropen(rlocator, INVALID_PROC_NUMBER);
	redo_copied_newest = record->EndRecPtr;
	if (smgrreplacedafter(reln, forknum, blkno, record->ReadRecPtr))
		return InvalidBuffer;

	r = PrefetchSharedBuffer(reln, forknum, blkno);
	if (BufferIsValid(r.recent_buffer) &&
		ReadRecentBuffer(rlocator, forknum, blkno, r.recent_buffer))
	{
		BmapEntry	entry;
		MapPage		mp;
		XLogRecPtr	since = InvalidXLogRecPtr;

		LockBuffer(r.recent_buffer, BUFFER_LOCK_EXCLUSIVE);
		if (lookup_entry_at(reln, blkno, &entry, &mp, NULL))
			since = Max(entry.since,
						remap_ahead(with_relnumber(rlocator, BMAP_STORE_RELNUMBER),
									mp.blk, MP_SLOT(&mp, blkno)));
		if (!PageIsNew(BufferGetPage(r.recent_buffer)) &&
			since <= PageGetLSN(BufferGetPage(r.recent_buffer)))
			return r.recent_buffer;

		/*
		 * The map as replay reads it gave the block its storage after the
		 * page's last change replay made, so that it skipped the records
		 * since (those of its old block, the map on disk naming the new
		 * one).  But not an image, which it restores all the same: the
		 * buffer may hold one of those, older than the page the copy saw.
		 */
		UnlockReleaseBuffer(r.recent_buffer);
	}

	elog(DEBUG1, "redo of a copy of block %u of %s without an image reads the page from block %u",
		 blkno, relpathperm(rlocator, MAIN_FORKNUM).str, xlrec->old_block);
	mdreadv(store_smgr(rlocator), MAIN_FORKNUM, xlrec->old_block, pages, 1);
	if (!PageIsVerified(copy.data, blkno, PIV_LOG_WARNING, NULL) ||
		PageIsNew(copy.data))
		ereport(PANIC,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("block %u of the page store of \"%s\", copied to block %u for its block %u, is not a page of it",
						xlrec->old_block, relpathperm(rlocator, MAIN_FORKNUM).str,
						xlrec->new_block, blkno)));
	buf = XLogReadBufferExtended(rlocator, forknum, blkno, RBM_ZERO_AND_LOCK,
								 InvalidBuffer);
	memcpy(BufferGetPage(buf), copy.data, BLCKSZ);
	return buf;
}

/*
 * In redo of a copy without an image: the old block's reference-count page,
 * registered as 'block_id', gets the copy's position, the record's end, as
 * the primary set it once the record was inserted (an image of the page
 * leaves it out, and the delta can't carry it).  A page newer than the
 * record (written out before a crash) has a position no older already.
 */
static void
redo_copy_lsn(XLogReaderState *record, uint8 block_id)
{
	RelFileLocator rlocator;
	ForkNumber	forknum;
	BlockNumber blkno;
	Buffer		buf;

	XLogRecGetBlockTag(record, block_id, &rlocator, &forknum, &blkno);
	buf = XLogReadBufferExtended(rlocator, forknum, blkno, RBM_NORMAL,
								 InvalidBuffer);
	if (!BufferIsValid(buf))
		return;
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	if (PageGetLSN(BufferGetPage(buf)) == record->EndRecPtr)
	{
		page_set_copy_lsn(BufferGetPage(buf), record->EndRecPtr);
		MarkBufferDirty(buf);
	}
	UnlockReleaseBuffer(buf);
}

/*
 * In redo of a PASSED record: blocks copied from without an image up to
 * 'horizon' may now change in place or be freed, which a recovery from
 * before such a copy would then read in place of what the copy saw.  So
 * this server's restart point must be past every such copy this replay
 * made (a standby's restartpoints lag the primary's checkpoints; a
 * recovery from archives begins at its backup's start): a restartpoint is
 * asked for and waited for if it isn't.  It begins at the last checkpoint
 * record replayed, whose redo pointer is 'horizon' or later, the primary
 * having logged the record once that checkpoint completed.  Crash
 * recovery begins there or later, and asks for nothing.  An online backup
 * taken on this standby meanwhile, which begins at an earlier restartpoint
 * and copies the blocks as they may change from here on, fails as it stops
 * (XLogNoteSyncedReplay(), when a restartpoint is asked for; and the note
 * below, past the backup's start, when the restart point passed the copies
 * already but the backup began before them).
 *
 * And the newest copy passed ('copied') is kept in pg_control, as the
 * primary keeps it, once the restartpoint is made, if it could be (see
 * redo_share_synced()): it keeps any recovery of this server from
 * beginning before the copy, and pg_rewind from rewinding a server from
 * this one, whether this replay replayed the copy or an earlier one did.
 */
static void
redo_copies_passed(const xl_bmap_passed *xlrec)
{
	XLogRecPtr	need;

	if (XLogRecPtrIsInvalid(redo_copied_newest))
	{
		XLogNoteSyncedPages(xlrec->copied - 1);
		return;
	}
	need = Min(redo_copied_newest, xlrec->horizon);
	if (last_checkpoint_redo() < need)
	{
		XLogNoteSyncedReplay();
		if (!redo_restart_point_at(need, "copies without an image") &&
			IsUnderPostmaster)
			ereport(WARNING,
					(errmsg("restart point of recovery %X/%08X is not past copies made without an image at %X/%08X",
							LSN_FORMAT_ARGS(last_checkpoint_redo()),
							LSN_FORMAT_ARGS(need)),
					 errdetail("A crash before the next restartpoint could lose rows of the pages copied.")));
	}
	XLogNoteSyncedPages(xlrec->copied - 1);
	if (redo_copied_newest <= xlrec->horizon)
		redo_copied_newest = InvalidXLogRecPtr;
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
							  reused, xlrec->nreused, false, 0);
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

				BlockList	own = {0};
				bool		kept = xlrec->op == BMAP_TREE_CAPTURE;
				bool		walked = false;
				const char *why = NULL;

				/* the store reaches a node made, which its image makes */
				if (xlrec->op == BMAP_TREE_NODE || xlrec->op == BMAP_TREE_COPY ||
					xlrec->op == BMAP_TREE_MOVE)
					prepare_node_store(xlrec->locator, xlrec->node,
									   xlrec->old_hw, xlrec->new_hw);

				/* a share's tops, for its source's walk (XLOG_BMAP_SHARED) */
				if (xlrec->op == BMAP_TREE_SHARE &&
					!(xlrec->flags & BMAP_TREE_OF_ROOT))
				{
					xl_bmap_root_delta rdelta;
					Size		len;
					char	   *data = XLogRecGetBlockData(record, 0, &len);

					if (data != NULL && len == sizeof(rdelta))
					{
						memcpy(&rdelta, data, sizeof(rdelta));
						redo_share_begun(xlrec->locator, rdelta.tops);
					}
				}
				if (kept)
				{
					xl_bmap_roots_delta rdelta;
					Size		len;
					char	   *data = XLogRecGetBlockData(record, 0, &len);

					/* (the root's delta comes with its image too) */
					if (data == NULL || len != sizeof(rdelta))
						elog(PANIC, "block map capture record without its slot's delta");
					memcpy(&rdelta, data, sizeof(rdelta));
					/* (every capture logs its own blocks as images) */
					if (!(xlrec->flags & BMAP_TREE_PRESERVE_LOGGED))
						elog(PANIC, "block map capture record of %s without its own blocks' images",
							 relpathperm(xlrec->locator, MAIN_FORKNUM).str);
					if (!DataChecksumsNeedVerify())
						why = "data checksums are off, so a page it would read could be torn";
					else if (!redo_own_blocks(xlrec->locator, &rdelta.root.snap,
											  record->ReadRecPtr, false, &own))
						why = "a page of its map or counts read is newer than the replay, unverified, or not the node named";
					else
						walked = true;
				}
				for (uint8 id = 0; id <= XLogRecMaxBlockId(record); id++)
					redo_block(record, id);

				/*
				 * A root captured: as the primary did, the relation's pages
				 * as of this record written to the blocks the root holds.
				 * The pages written are those of the root's own blocks
				 * (redo_own_blocks()), each looked up in the buffers, as
				 * bmap_root_capture() writes them: as many as changed since
				 * the relation's last capture.  Every buffer is visited
				 * instead if the walk can't trust what it reads: with data
				 * checksums off, or once it reads a page newer than the
				 * replay, one not verified, or one not the node the pointer
				 * to it names (redo_own_blocks()).
				 *
				 * The own blocks were logged as images just before the
				 * record, yet they are still written out: a page may have
				 * changed in place after its image and before the record
				 * (a row lock, a prune), whose redo went to the relation's
				 * buffer only.  Nothing is synced: redo of each image wrote
				 * it to its block, as a page written out, which the
				 * checkpointer (a restartpoint) syncs, as it syncs these
				 * writes, and a crash before that replays the images and
				 * the changes again.
				 */
				if (kept)
				{
					SMgrRelation reln = smgropen(xlrec->locator,
												 INVALID_PROC_NUMBER);

					if (walked)
					{
						elog(DEBUG1, "redo of a capture of %s writes out its own pages: %u, found reading %u nodes of its map and %u count pages",
							 relpathperm(xlrec->locator, MAIN_FORKNUM).str,
							 own.n, redo_walk_nodes, redo_walk_ref_pages);
						flush_own_blocks(reln, &own);
					}
					else
					{
						elog(DEBUG1, "redo of a capture of %s writes out every buffer of it: %s",
							 relpathperm(xlrec->locator, MAIN_FORKNUM).str, why);
						FlushRelationsAllBuffers(&reln, 1);
					}
					if (own.blocks)
						pfree(own.blocks);
					elog(DEBUG1, "redo of a capture of %s syncs nothing: its own pages were logged as images",
						 relpathperm(xlrec->locator, MAIN_FORKNUM).str);
				}
				break;
			}
		case XLOG_BMAP_FREE:
		case XLOG_BMAP_UNREF:
		case XLOG_BMAP_RELEASE:
			for (uint8 id = 0; id <= XLogRecMaxBlockId(record); id++)
				redo_block(record, id);
			break;
		case XLOG_BMAP_REMAP:
			{
				xl_bmap_remap *xlrec = (xl_bmap_remap *) XLogRecGetData(record);
				uint8		last = XLogRecMaxBlockId(record);

				bool		zeroed = (xlrec->flags & BMAP_REMAP_ZERO) != 0;
				bool		copied = (xlrec->flags & BMAP_REMAP_COPIED) != 0 &&
					!XLogRecBlockImageApply(record, last);
				Buffer		page = InvalidBuffer;

				/*
				 * The page store reaches the new block; the page itself is
				 * the last block, an image, or zeroes, which redo writes
				 * there, or the page as replay has it or the old block
				 * holds it, which redo takes before the leaf names the new
				 * block (redo_remap_page()); so redo never reads the new
				 * block.  The old block keeps what it had: shared blocks
				 * were made durable when shared (synced by the share, or
				 * logged as images that redo writes and a checkpoint after
				 * syncs).
				 */
				if (copied)
					page = redo_remap_page(record, last, xlrec);
				if (!zeroed && xlrec->new_block < xlrec->old_hw)
					drop_node_buffer(xlrec->locator, xlrec->new_block);
				prepare_store(xlrec->locator, xlrec->old_hw, xlrec->new_hw,
							  &xlrec->new_block,
							  zeroed && xlrec->new_block < xlrec->old_hw ? 1 : 0,
							  !zeroed, 0);
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
				else if (copied)
				{
					/* written to the new block from here on */
					if (BufferIsValid(page))
					{
						if (PageGetLSN(BufferGetPage(page)) < record->EndRecPtr)
						{
							PageSetLSN(BufferGetPage(page), record->EndRecPtr);
							MarkBufferDirty(page);
						}
						UnlockReleaseBuffer(page);
					}
				}
				else
					redo_block(record, last);
				if (xlrec->flags & BMAP_REMAP_COPIED)
					redo_copy_lsn(record, last - 1);
				XLogReplaceBlock(xlrec->locator, key_fork(xlrec->blkno),
								 key_block(xlrec->blkno));
				break;
			}
		case XLOG_BMAP_PASSED:
			redo_copies_passed((xl_bmap_passed *) XLogRecGetData(record));
			break;
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
				Assert(fork_mapped(forknum));

				/*
				 * Restore the buffer the image names too: before SHARED it
				 * is the source's, which SHARED flushes, and which must not
				 * overwrite the preserved block with incomplete redo (a
				 * newer map sends it nowhere until its remap); after it, the
				 * copy's, whose map names the block (preserve_synced()).
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
				PageSetChecksum((Page) copy.data, blkno);
				smgrwrite(store_smgr(rel), MAIN_FORKNUM, phys, copy.data, false);
				store_written(rel, phys, 1);
				break;
			}
		case XLOG_BMAP_SHARED:
			{
				xl_bmap_share *xlrec = (xl_bmap_share *) XLogRecGetData(record);
				SMgrRelation src = smgropen(xlrec->src, INVALID_PROC_NUMBER);
				xl_bmap_snapshot snap;
				xl_bmap_header_delta sdelta;
				Size		len;
				char	   *data = XLogRecGetBlockData(record, 0, &len);
				BlockList	own = {0};
				const char *why = NULL;

				/* (every share logs the own blocks it doesn't sync as images) */
				if (!(xlrec->flags & BMAP_PRESERVE_LOGGED))
					elog(PANIC, "block map share record of %s without its own blocks' images",
						 relpathperm(xlrec->src, MAIN_FORKNUM).str);

				/*
				 * The source's own blocks as of the share, as the capture's
				 * redo finds a root's: from the tops its tree record gave
				 * the copy, and the source's length and epoch, which this
				 * record raises.
				 */
				memset(&snap, 0, sizeof(snap));
				if (!redo_share_tops(xlrec->dst, snap.tops))
					why = "its tree's record was not replayed";
				else if (data == NULL || len != sizeof(sdelta))
					why = "its source's header has no delta";
				else if (!DataChecksumsNeedVerify())
					why = "data checksums are off, so a page it would read could be torn";
				else
				{
					memcpy(&sdelta, data, sizeof(sdelta));
					snap.nblocks = sdelta.nblocks;
					snap.vm_nblocks = sdelta.vm_nblocks;
					snap.epoch = sdelta.epoch;
					if (!redo_own_blocks(xlrec->src, &snap, record->ReadRecPtr,
										 true, &own))
						why = "a page of its map or counts read is newer than the replay, unverified, or not the node named";
				}
				redo_block(record, 0);
				redo_block(record, 1);
				forget_cached_size(xlrec->dst);

				/*
				 * As the primary did: the shared blocks get the source's
				 * pages as of this record, durably.  The source's buffers
				 * are written out though some of its own blocks were logged
				 * as images, whose redo wrote each to its block: a page found
				 * the source's own before the share may have changed in place
				 * after its image and before this record (a row lock, a
				 * prune, which don't ask the map), and the change's redo
				 * went to the source's buffer only.  The blocks the share
				 * synced instead are synced here too (redo_share_synced());
				 * for the others the sync is left to the checkpointer (a
				 * restartpoint), as for any page written out; the blocks
				 * shared already were written when they were first shared.  The own pages are those of
				 * the walk above, each looked up in the buffers, so that a
				 * share costs redo as many lookups as it logged images, not
				 * a visit of every buffer; every buffer is visited only if
				 * the walk can't trust what it reads, as for a capture.
				 */
				if (why == NULL)
				{
					elog(DEBUG1, "redo of a share of %s writes out its own pages: %u, found reading %u nodes of its map and %u count pages",
						 relpathperm(xlrec->src, MAIN_FORKNUM).str,
						 own.n, redo_walk_nodes, redo_walk_ref_pages);
					flush_own_blocks(src, &own);
				}
				else
				{
					elog(DEBUG1, "redo of a share of %s writes out every buffer of it: %s",
						 relpathperm(xlrec->src, MAIN_FORKNUM).str, why);
					FlushRelationsAllBuffers(&src, 1);
				}
				if (own.blocks)
					pfree(own.blocks);
				if (!(xlrec->flags & BMAP_SHARE_SYNCED))
				{
					elog(DEBUG1, "redo of a share of %s syncs nothing: its own pages were logged as images",
						 relpathperm(xlrec->src, MAIN_FORKNUM).str);
					break;
				}
				redo_share_synced(xlrec, record->ReadRecPtr);
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

	/*
	 * A map's identity is where the record that made it began, set once it
	 * is inserted (a record's image of the page is taken before)
	 */
	if (map_page_is(pagedata, BMAP_PAGE_ROOT))
		PageGetBmapRoot((Page) pagedata)->map_id = 0;

	/* so is a reference-count page's copy position */
	if (map_page_is(pagedata, BMAP_PAGE_REFS))
		page_set_copy_lsn((Page) pagedata, InvalidXLogRecPtr);
}
