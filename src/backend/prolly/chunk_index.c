/*-------------------------------------------------------------------------
 *
 * chunk_index.c
 *	  The history store's index from a chunk's hash to its place in the
 *	  chunk log and its reference counts, by linear hashing in pages of
 *	  its own layout (prolly/chunk_index.h).
 *
 * Every change to a bucket holds the bucket's first page in exclusive lock,
 * and a lookup holds it in share lock, while they go through the bucket's
 * chain.  The meta page is changed only to give a bucket's chain a page, or
 * to split a bucket; a backend keeps a copy of it in the relation's
 * rd_amcache, and a bucket's first page notes the buckets there were when
 * it was last split, so that a copy older than that is read again.
 *
 * Splitting bucket b makes the next bucket, n: the entries of b that hash
 * to n under the new masks are copied to pages of n's that nothing reaches
 * yet, then one record makes n a bucket (the meta page, the directory and
 * b's first page), and then the entries copied are taken out of b.  A
 * crash between the two leaves copies in b of entries that are n's; they
 * are not b's entries, so lookups pass them by, and an insert into b takes
 * them out.
 *
 * Changes are WAL-logged with generic records, and each gives its
 * transaction an id, so that the transaction's commit flushes them.  Locks
 * are taken in one order: a bucket's first page, its chain's pages, the
 * meta page, the directory's pages (a split reads the old bucket's chain
 * with the meta page locked, which it may: only a holder of the bucket's
 * first page, which it holds, goes through that chain); a lookup reads the
 * meta page and the directory with nothing else locked.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/chunk_index.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/bufmask.h"
#include "access/multixact.h"
#include "access/relation.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "prolly/chunk_freer.h"
#include "prolly/chunk_store.h"
#include "prolly/chunk_index.h"
#include "prolly/chunk_log.h"
#include "prolly/chunk_lane.h"
#include "prolly/chunk_xlog.h"
#include "storage/bufmgr.h"
#include "storage/bulk_write.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "storage/standby.h"
#include "utils/timestamp.h"
#include "utils/injection_point.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#define CHUNK_INDEX_MAGIC	0x50434958	/* "PCIX" */
#define CHUNK_INDEX_VERSION	8

#define INDEX_PAGE_META		1
#define INDEX_PAGE_DIR		2
#define INDEX_PAGE_BUCKET	3			/* a bucket's first page */
#define INDEX_PAGE_OVERFLOW	4			/* a later page of its chain */
#define INDEX_PAGE_MARKS	5			/* buckets marked, a bit each */

#define META_BLOCK			0
#define INIT_BUCKETS		8			/* a power of two */

/* The special space of each of the index's pages. */
typedef struct IndexPageOpaqueData
{
	uint32		magic;			/* CHUNK_INDEX_MAGIC */
	uint16		kind;			/* INDEX_PAGE_* */
	uint16		unused;
	uint32		bucket;			/* a bucket's page: which bucket */
	uint32		split_at;		/* a first page: the highest bucket there
								 * was when it was made or last split */
	BlockNumber next;			/* a bucket's page: the next in its chain */
} IndexPageOpaqueData;

typedef IndexPageOpaqueData *IndexPageOpaque;

#define OPAQUE_SIZE		MAXALIGN(sizeof(IndexPageOpaqueData))
#define CONTENTS_SIZE	(BLCKSZ - SizeOfPageHeaderData - OPAQUE_SIZE)

/* The meta page's contents. */
typedef struct IndexMetaData
{
	uint32		version;		/* CHUNK_INDEX_VERSION */
	uint32		maxbucket;		/* the highest bucket there is */
	uint32		lowmask;
	uint32		highmask;
	BlockNumber next_block;		/* the first page not in use */
	Oid			log;			/* the chunk log the entries are of, or 0 */
	uint64		log_stamp;		/* and its storage's stamp */
	uint32		lane;			/* a lane's: its number (prolly/chunk_lane.h) */
	Oid			parent;			/* a lane's: the index it is a lane of; the
								 * index's own, 0 */
	uint32		nmarks;			/* the marks' pages */
	BlockNumber marks[64];		/* MAX_MARKS */
	uint32		ndir;			/* the directory's pages */
	BlockNumber dir[FLEXIBLE_ARRAY_MEMBER];
} IndexMetaData;

#define MAX_DIR			((CONTENTS_SIZE - offsetof(IndexMetaData, dir)) / \
						 sizeof(BlockNumber))
#define DIR_PER_PAGE	(CONTENTS_SIZE / sizeof(BlockNumber))
#define PER_PAGE		(CONTENTS_SIZE / sizeof(ChunkIndexEntry))
#define MAX_MARKS		64
#define MARK_BITS		((uint32) (CONTENTS_SIZE * 8))

/* (a bit for every bucket there can be) */
StaticAssertDecl((uint64) MAX_MARKS * MARK_BITS >= (uint64) MAX_DIR * DIR_PER_PAGE,
				 "the marks' pages have a bit for each bucket");

/* A backend's copy of the meta page, in the relation's rd_amcache. */
typedef struct IndexCache
{
	RelFileNumber relnumber;
	Oid			log;
	uint64		log_stamp;
	uint32		maxbucket;
	uint32		lowmask;
	uint32		highmask;
	uint32		nmarks;
	BlockNumber marks[MAX_MARKS];
	uint32		ndir;
	BlockNumber dir[MAX_DIR];
} IndexCache;

StaticAssertDecl(sizeof(ChunkIndexEntry) == 64,
				 "a chunk index entry is 64 bytes");
StaticAssertDecl(sizeof(xl_chunk_index_vacuum) == 8 &&
				 sizeof(xl_chunk_index_slot) == 16 &&
				 sizeof(xl_chunk_index_fold) == 16 &&
				 sizeof(xl_chunk_index_init_page) == 20 &&
				 sizeof(xl_chunk_index_split) == 28 &&
				 sizeof(xl_chunk_index_bind_log) == 16,
				 "the index's records have no padding");

typedef enum EntryState
{
	ENTRY_VISIBLE,				/* committed, or this transaction's */
	ENTRY_IN_PROGRESS,			/* another transaction's, running */
	ENTRY_DEAD,					/* its transaction aborted */
} EntryState;

/*
 * What the entry's inserting transaction is to this one, learned from the
 * commit log and kept in the entry's flags as heap tuples keep hint bits
 * (not WAL-logged, and set under a share lock): an abort always, a commit
 * only once its record is flushed, or the page's own changes are after it,
 * lest the page reach disk noting a commit a crash then loses.
 */
/* whether xid committed, a hint of it set in the entry (or of its abort) */
static bool
hinted_commit(ChunkIndexEntry *e, Buffer buf, TransactionId xid,
			  uint16 committed_flag, uint16 aborted_flag)
{
	bool		committed = TransactionIdDidCommit(xid);

	if (committed)
	{
		XLogRecPtr	commit_lsn = TransactionIdGetCommitLSN(xid);

		if (!XLogNeedsFlush(commit_lsn) || BufferGetLSNAtomic(buf) >= commit_lsn)
			(void) BufferSetHintBits16(&e->flags, e->flags | committed_flag, buf);
	}
	else
		(void) BufferSetHintBits16(&e->flags, e->flags | aborted_flag, buf);
	return committed;
}

/*
 * What the entry is to this transaction: its xmin's, and, of a chunk's
 * entry visible, its xmax's (freed, being freed, or not).  The transaction
 * to wait for, if the entry is another's in progress, in *wait.
 */
static EntryState
entry_state_wait(ChunkIndexEntry *e, Buffer buf, TransactionId *wait)
{
	if (!(e->flags & (CHUNK_ENTRY_FROZEN | CHUNK_ENTRY_COMMITTED)))
	{
		if (e->flags & CHUNK_ENTRY_ABORTED)
			return ENTRY_DEAD;
		if (!TransactionIdIsCurrentTransactionId(e->xmin))
		{
			if (TransactionIdIsInProgress(e->xmin))
			{
				if (wait)
					*wait = e->xmin;
				return ENTRY_IN_PROGRESS;
			}
			if (!hinted_commit(e, buf, e->xmin, CHUNK_ENTRY_COMMITTED,
							   CHUNK_ENTRY_ABORTED))
				return ENTRY_DEAD;
		}
	}
	if (e->kind != CHUNK_KIND_CHUNK || !TransactionIdIsValid(e->xmax) ||
		(e->flags & CHUNK_ENTRY_XMAX_ABORTED))
		return ENTRY_VISIBLE;
	if (e->flags & CHUNK_ENTRY_XMAX_COMMITTED)
		return ENTRY_DEAD;
	if (TransactionIdIsCurrentTransactionId(e->xmax))
		return ENTRY_DEAD;
	if (TransactionIdIsInProgress(e->xmax))
	{
		/*
		 * (a hot standby's, which can't wait for the primary's transactions:
		 * the chunk is there, its freeing not committed; should it commit,
		 * taking the entry out conflicts with this snapshot first.  So a
		 * visit there sees the chunk too, where one on the primary passes it
		 * by.)
		 */
		if (RecoveryInProgress())
			return ENTRY_VISIBLE;
		if (wait)
			*wait = e->xmax;
		return ENTRY_IN_PROGRESS;
	}
	return hinted_commit(e, buf, e->xmax, CHUNK_ENTRY_XMAX_COMMITTED,
						 CHUNK_ENTRY_XMAX_ABORTED) ? ENTRY_DEAD : ENTRY_VISIBLE;
}

static inline EntryState
entry_state(ChunkIndexEntry *e, Buffer buf)
{
	return entry_state_wait(e, buf, NULL);
}

/*
 * Whether a chunk's entry was the store's as of snapshot: its insert
 * committed before it, and its freeing not (a chunk freed since is as it
 * was then: its entry, and bytes, are taken out only once no snapshot might
 * have found it).  Changes are not asked of.
 */
static bool
chunk_as_of(ChunkIndexEntry *e, Buffer buf, Snapshot snap)
{
	if (e->kind != CHUNK_KIND_CHUNK)
		return false;
	if (!(e->flags & CHUNK_ENTRY_FROZEN))
	{
		if (e->flags & CHUNK_ENTRY_ABORTED)
			return false;
		if (TransactionIdIsCurrentTransactionId(e->xmin))
			return true;
		if (XidInMVCCSnapshot(e->xmin, snap))
			return false;
		if (!(e->flags & CHUNK_ENTRY_COMMITTED) &&
			!hinted_commit(e, buf, e->xmin, CHUNK_ENTRY_COMMITTED, CHUNK_ENTRY_ABORTED))
			return false;
	}
	if (!TransactionIdIsValid(e->xmax) || (e->flags & CHUNK_ENTRY_XMAX_ABORTED))
		return true;
	if (TransactionIdIsCurrentTransactionId(e->xmax))
		return false;
	if (XidInMVCCSnapshot(e->xmax, snap))
		return true;
	if (e->flags & CHUNK_ENTRY_XMAX_COMMITTED)
		return false;
	return !TransactionIdDidCommit(e->xmax);
}

/* the entry a change is of: its chunk's, by xmin and loc */
static inline bool
change_of(const ChunkIndexEntry *change, const ChunkIndexEntry *chunk)
{
	return change->kind == CHUNK_KIND_CHANGE &&
		memcmp(change->hash, chunk->hash, CHUNK_HASH_LEN) == 0 &&
		change->xmax == chunk->xmin && change->loc == chunk->loc;
}

/* ------------------------------------------------------------ the pages */

static void
init_ipage(Page page, uint16 kind, uint32 bucket, uint32 split_at)
{
	IndexPageOpaque opaque;

	PageInit(page, BLCKSZ, sizeof(IndexPageOpaqueData));
	opaque = (IndexPageOpaque) PageGetSpecialPointer(page);
	opaque->magic = CHUNK_INDEX_MAGIC;
	opaque->kind = kind;
	opaque->unused = 0;
	opaque->bucket = bucket;
	opaque->split_at = split_at;
	opaque->next = InvalidBlockNumber;
}

static IndexPageOpaque
check_ipage(Relation rel, BlockNumber blk, Page page, uint16 kind)
{
	IndexPageOpaque opaque;

	if (PageIsNew(page) || PageGetSpecialSize(page) != OPAQUE_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("block %u of \"%s\" is not a page of a chunk index",
						blk, RelationGetRelationName(rel))));
	opaque = (IndexPageOpaque) PageGetSpecialPointer(page);
	if (opaque->magic != CHUNK_INDEX_MAGIC || opaque->kind != kind)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("block %u of chunk index \"%s\" is not the kind of page expected",
						blk, RelationGetRelationName(rel))));
	return opaque;
}

static IndexMetaData *
page_meta(Relation rel, Page page)
{
	IndexMetaData *meta;

	check_ipage(rel, META_BLOCK, page, INDEX_PAGE_META);
	meta = (IndexMetaData *) PageGetContents(page);
	if (meta->ndir == 0 || meta->ndir > MAX_DIR || meta->nmarks > MAX_MARKS)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("chunk index \"%s\" has a meta page of %u directory pages and %u marks' pages",
						RelationGetRelationName(rel), meta->ndir, meta->nmarks)));
	if (meta->version != CHUNK_INDEX_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("chunk index \"%s\" has format version %u, not %u",
						RelationGetRelationName(rel), meta->version,
						CHUNK_INDEX_VERSION)));
	return meta;
}

static void
set_meta_lower(Page page, IndexMetaData *meta)
{
	((PageHeader) page)->pd_lower =
		(char *) &meta->dir[meta->ndir] - (char *) page;
}

static inline int
page_entries(Page page)
{
	return (((PageHeader) page)->pd_lower - SizeOfPageHeaderData) /
		sizeof(ChunkIndexEntry);
}

static inline ChunkIndexEntry *
page_entry(Page page, int i)
{
	return (ChunkIndexEntry *) PageGetContents(page) + i;
}

static inline void
set_page_entries(Page page, int n)
{
	((PageHeader) page)->pd_lower =
		SizeOfPageHeaderData + n * sizeof(ChunkIndexEntry);
}

/*
 * The changes the index makes to its pages, each WAL-logged as a record of
 * its own (prolly/chunk_xlog.h) and made again by its redo.  An entry is
 * moved whole: on a standby, with the hint flags it has there.
 */
static void
ipage_add(Page page, const ChunkIndexEntry *e)
{
	int			n = page_entries(page);

	*page_entry(page, n) = *e;
	set_page_entries(page, n + 1);
}

static void
ipage_set_counts(Page page, int at, uint32 refs, uint32 roots)
{
	page_entry(page, at)->refs = refs;
	page_entry(page, at)->roots = roots;
}

static void
ipage_set_merge(Page page, int at, TransactionId merge_xid)
{
	page_entry(page, at)->merge_xid = merge_xid;
}

static void
ipage_set_xmax(Page page, int at, TransactionId xmax)
{
	ChunkIndexEntry *e = page_entry(page, at);

	e->xmax = xmax;
	e->flags &= ~(CHUNK_ENTRY_XMAX_COMMITTED | CHUNK_ENTRY_XMAX_ABORTED);
}

/* the entries in slots kept (in order) kept, the rest taken out */
static void
ipage_keep(Page page, const uint16 *kept, int nkept)
{
	for (int k = 0; k < nkept; k++)
		*page_entry(page, k) = *page_entry(page, kept[k]);
	set_page_entries(page, nkept);
}

/* the entry in slot at taken out, the last moved into it */
static void
ipage_remove_slot(Page page, int at)
{
	int			n = page_entries(page);

	*page_entry(page, at) = *page_entry(page, n - 1);
	set_page_entries(page, n - 1);
}

static void
ipage_make(Page page, uint16 kind, uint32 bucket, uint32 split_at,
		   BlockNumber next, const void *contents, uint32 len)
{
	init_ipage(page, kind, bucket, split_at);
	((IndexPageOpaque) PageGetSpecialPointer(page))->next = next;
	if (len > 0)
		memcpy(PageGetContents(page), contents, len);
	((PageHeader) page)->pd_lower = SizeOfPageHeaderData + len;
}

/* a directory page's slot */
static void
idir_set(Page page, uint32 slot, BlockNumber blk)
{
	((BlockNumber *) PageGetContents(page))[slot] = blk;
	((PageHeader) page)->pd_lower = Max(((PageHeader) page)->pd_lower,
										SizeOfPageHeaderData +
										(slot + 1) * sizeof(BlockNumber));
}

/* a marks' page, the nth, no bucket marked */
static void
imarks_init(Page page, uint32 nth)
{
	init_ipage(page, INDEX_PAGE_MARKS, nth, 0);
	memset(PageGetContents(page), 0, CONTENTS_SIZE);
	/* (no hole: its image is the bits, every one) */
	((PageHeader) page)->pd_lower = ((PageHeader) page)->pd_upper;
}

static inline bool
imarks_get(Page page, uint32 bit)
{
	return (((uint8 *) PageGetContents(page))[bit / 8] >> (bit % 8)) & 1;
}

static void
imarks_set(Page page, uint32 bit, bool on)
{
	uint8	   *bits = (uint8 *) PageGetContents(page);

	if (on)
		bits[bit / 8] |= (uint8) (1 << (bit % 8));
	else
		bits[bit / 8] &= (uint8) ~(1 << (bit % 8));
}

/* the meta page's nth marks' page, blk */
static void
imeta_add_marks(Page page, uint32 nth, BlockNumber blk)
{
	IndexMetaData *meta = (IndexMetaData *) PageGetContents(page);

	meta->marks[nth] = blk;
	meta->nmarks = nth + 1;
	meta->next_block = blk + 1;
}

/* a split made public, in the meta page */
static void
imeta_split(Page page, const xl_chunk_index_split *x)
{
	IndexMetaData *meta = (IndexMetaData *) PageGetContents(page);

	if (x->new_dir)
		meta->dir[meta->ndir++] = x->dir_block;
	meta->maxbucket = x->new_bucket;
	meta->lowmask = x->lowmask;
	meta->highmask = x->highmask;
	meta->next_block = x->next_block;
	set_meta_lower(page, meta);
}

/* A WAL record's page, its change made, and what to register it with. */
typedef struct LoggedPage
{
	Buffer		buf;
	bool		will_init;		/* made anew: no image needed */
	const void *data;			/* the block's data, or NULL */
	size_t		len;
} LoggedPage;

static void fold_changes(Relation idx, Buffer primary, uint32 bucket,
						 const IndexCache *masks);

/*
 * The block id of a record's bucket's first page, when the pages it changes
 * are others of the bucket's chain: registered unchanged, so that redo
 * locks it first, as the change was made with it locked (what keeps a
 * reader from walking the chain meanwhile).
 */
#define BUCKET_BLOCK	3

/*
 * WAL-logs the change just made to the pages, in the critical section that
 * made it (dirtying them): rec, the record's data; bucket, the first page of
 * their bucket, if it is locked for the change (InvalidBuffer if not)
 */
static void
log_index_change(Relation idx, uint8 info, const void *rec, size_t reclen,
				 const LoggedPage *pages, int npages, Buffer bucket)
{
	bool		bucket_changed = !BufferIsValid(bucket);

	XLogRecPtr	lsn;

	for (int i = 0; i < npages; i++)
		MarkBufferDirty(pages[i].buf);
	if (!RelationNeedsWAL(idx))
		return;
	XLogBeginInsert();
	if (reclen > 0)
		XLogRegisterData(rec, reclen);
	for (int i = 0; i < npages; i++)
	{
		XLogRegisterBuffer(i, pages[i].buf,
						   REGBUF_STANDARD | (pages[i].will_init ? REGBUF_WILL_INIT : 0));
		if (pages[i].len > 0)
			XLogRegisterBufData(i, pages[i].data, pages[i].len);
		if (pages[i].buf == bucket)
			bucket_changed = true;
	}
	if (!bucket_changed)
		XLogRegisterBuffer(BUCKET_BLOCK, bucket, REGBUF_NO_CHANGE | REGBUF_NO_IMAGE);
	lsn = XLogInsert(RM_CHUNK_INDEX_ID, info);
	for (int i = 0; i < npages; i++)
		PageSetLSN(BufferGetPage(pages[i].buf), lsn);
}

/* a page's entries kept where keep[slot], WAL-logged; false if all are */
static bool
keep_entries(Relation idx, Buffer bucket, Buffer buf, const bool *keep)
{
	Page		page = BufferGetPage(buf);
	int			n = page_entries(page);
	uint16		kept[PER_PAGE];
	xl_chunk_index_keep xlrec;
	LoggedPage lp;

	xlrec.nkept = 0;
	for (int i = 0; i < n; i++)
		if (keep[i])
			kept[xlrec.nkept++] = i;
	if (xlrec.nkept == n)
		return false;
	lp.buf = buf;
	lp.will_init = false;
	lp.data = kept;
	lp.len = xlrec.nkept * sizeof(uint16);
	START_CRIT_SECTION();
	ipage_keep(page, kept, xlrec.nkept);
	log_index_change(idx, XLOG_CHUNK_INDEX_KEEP, &xlrec, sizeof(xlrec), &lp, 1, bucket);
	END_CRIT_SECTION();
	return true;
}

/* one page's change of one slot, WAL-logged */
static void
log_slot_change(Relation idx, Buffer bucket, Buffer buf, uint8 info,
				const xl_chunk_index_slot *xlrec)
{
	LoggedPage	lp = {.buf = buf};

	log_index_change(idx, info, xlrec, sizeof(*xlrec), &lp, 1, bucket);
}

/* the entry in slot at taken out, WAL-logged */
static void
remove_slot(Relation idx, Buffer bucket, Buffer buf, int at)
{
	xl_chunk_index_slot xlrec = {.slot = at};

	START_CRIT_SECTION();
	ipage_remove_slot(BufferGetPage(buf), at);
	log_slot_change(idx, bucket, buf, XLOG_CHUNK_INDEX_REMOVE_SLOT, &xlrec);
	END_CRIT_SECTION();
}

/*
 * A page the index has just given a use: read, or the relation extended to
 * it, and locked exclusively (its contents are made anew; nobody else has
 * it, as pages are given out under the meta page's lock).
 */
static Buffer
new_page_buffer(Relation rel, BlockNumber blk)
{
	Buffer		buf;

	if (blk < RelationGetNumberOfBlocks(rel))
	{
		buf = ReadBuffer(rel, blk);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		return buf;
	}
	return ExtendBufferedRelTo(BMR_REL(rel), MAIN_FORKNUM, NULL, 0, blk + 1,
							   RBM_ZERO_AND_LOCK);
}

/* ------------------------------------------------------- hashing */

static inline uint32
hash_key(const uint8 *hash)
{
	return ((uint32) hash[0] << 24) | ((uint32) hash[1] << 16) |
		((uint32) hash[2] << 8) | (uint32) hash[3];
}

static inline uint32
bucket_of(uint32 key, uint32 maxbucket, uint32 lowmask, uint32 highmask)
{
	uint32		b = key & highmask;

	return b > maxbucket ? (key & lowmask) : b;
}

static IndexCache *
load_cache(Relation rel)
{
	Buffer		mbuf = ReadBuffer(rel, META_BLOCK);
	IndexMetaData *meta;
	IndexCache	cache;

	LockBuffer(mbuf, BUFFER_LOCK_SHARE);
	meta = page_meta(rel, BufferGetPage(mbuf));
	cache.relnumber = rel->rd_locator.relNumber;
	cache.log = meta->log;
	cache.log_stamp = meta->log_stamp;
	cache.maxbucket = meta->maxbucket;
	cache.lowmask = meta->lowmask;
	cache.highmask = meta->highmask;
	cache.nmarks = meta->nmarks;
	memcpy(cache.marks, meta->marks, meta->nmarks * sizeof(BlockNumber));
	cache.ndir = meta->ndir;
	memcpy(cache.dir, meta->dir, meta->ndir * sizeof(BlockNumber));
	UnlockReleaseBuffer(mbuf);

	/* (reading may have reset the relcache entry) */
	if (rel->rd_amcache == NULL)
		rel->rd_amcache = MemoryContextAlloc(CacheMemoryContext,
											 sizeof(IndexCache));
	memcpy(rel->rd_amcache, &cache, sizeof(IndexCache));
	return rel->rd_amcache;
}

static IndexCache *
get_cache(Relation rel)
{
	IndexCache *cache = rel->rd_amcache;

	if (cache == NULL || cache->relnumber != rel->rd_locator.relNumber)
		cache = load_cache(rel);
	return cache;
}

/* The first page of bucket b, which the directory says. */
static BlockNumber
bucket_block(Relation rel, const IndexCache *cache, uint32 b)
{
	uint32		d = b / DIR_PER_PAGE;
	Buffer		buf;
	Page		page;
	BlockNumber blk;

	if (d >= cache->ndir)
		elog(ERROR, "bucket %u of chunk index \"%s\" has no directory page",
			 b, RelationGetRelationName(rel));
	buf = ReadBuffer(rel, cache->dir[d]);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	check_ipage(rel, cache->dir[d], page, INDEX_PAGE_DIR);
	blk = ((BlockNumber *) PageGetContents(page))[b % DIR_PER_PAGE];
	UnlockReleaseBuffer(buf);
	return blk;
}

/*
 * The first page of the bucket hash is in, locked in mode, its bucket in
 * *bucket and the masks it was found by in *masks.
 */
static Buffer
lock_bucket(Relation rel, const uint8 *hash, int mode, uint32 *bucket,
			IndexCache *masks)
{
	uint32		key = hash_key(hash);
	IndexCache *cache = get_cache(rel);

	for (;;)
	{
		uint32		b = bucket_of(key, cache->maxbucket, cache->lowmask,
								  cache->highmask);
		BlockNumber blk = bucket_block(rel, cache, b);
		Buffer		buf = ReadBuffer(rel, blk);
		IndexPageOpaque opaque;

		LockBuffer(buf, mode);
		opaque = check_ipage(rel, blk, BufferGetPage(buf), INDEX_PAGE_BUCKET);
		if (opaque->bucket != b)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("block %u of chunk index \"%s\" is bucket %u's, not %u's",
							blk, RelationGetRelationName(rel),
							opaque->bucket, b)));

		/* split since the copy of the meta page was read: read it again */
		if (opaque->split_at > cache->maxbucket)
		{
			UnlockReleaseBuffer(buf);
			cache = load_cache(rel);
			continue;
		}
		*bucket = b;
		masks->maxbucket = cache->maxbucket;
		masks->lowmask = cache->lowmask;
		masks->highmask = cache->highmask;
		return buf;
	}
}

static inline bool
belongs(const ChunkIndexEntry *e, uint32 bucket, const IndexCache *masks)
{
	return bucket_of(hash_key(e->hash), masks->maxbucket, masks->lowmask,
					 masks->highmask) == bucket;
}

/*
 * Goes through the chain of the bucket whose first page is primary (locked
 * in mode): finds hash's entry, with its page left locked in *found_buf
 * (primary itself, or another the caller releases) and its place in
 * *found_at.  With purge, takes out of each page (exclusive) the entries
 * not the bucket's.  Notes in *space the first page with room, and in
 * *last the chain's last page.
 */
static EntryState find_entry_freeing(Relation rel, Buffer primary, int mode,
									 const uint8 *hash, uint32 bucket,
									 const IndexCache *masks, bool purge,
									 Buffer *found_buf, int *found_at,
									 BlockNumber *space, BlockNumber *last,
									 TransactionId *running, bool *freeing);

/*
 * (a purge takes out entries not the bucket's, and aborted changes; an
 * aborted chunk's entry with bytes is left to VACUUM, which counts its
 * bytes dead in its log as it takes it out)
 */
static inline bool
kept_by_purge(const ChunkIndexEntry *e, uint32 bucket, const IndexCache *masks)
{
	return belongs(e, bucket, masks) &&
		!((e->flags & CHUNK_ENTRY_ABORTED) &&
		  !(e->kind == CHUNK_KIND_CHUNK && e->len > 0));
}

static EntryState
find_entry(Relation rel, Buffer primary, int mode, const uint8 *hash,
		   uint32 bucket, const IndexCache *masks, bool purge,
		   Buffer *found_buf, int *found_at,
		   BlockNumber *space, BlockNumber *last, TransactionId *running)
{
	return find_entry_freeing(rel, primary, mode, hash, bucket, masks, purge,
							  found_buf, found_at, space, last, running, NULL);
}

/*
 * find_entry, and whether a chunk's entry another transaction (*running)
 * holds is being freed by it, in *freeing, rather than being put
 */
static EntryState
find_entry_freeing(Relation rel, Buffer primary, int mode, const uint8 *hash,
				   uint32 bucket, const IndexCache *masks, bool purge,
				   Buffer *found_buf, int *found_at,
				   BlockNumber *space, BlockNumber *last,
				   TransactionId *running, bool *freeing)
{
	EntryState	result = ENTRY_DEAD;	/* (none found) */

	Buffer		cur = primary;

	if (space)
		*space = InvalidBlockNumber;
	for (;;)
	{
		Page		page = BufferGetPage(cur);
		IndexPageOpaque opaque = (IndexPageOpaque) PageGetSpecialPointer(page);
		BlockNumber next;
		int			n = page_entries(page);

		if (purge)
		{
			int			kept = 0;

			bool		keep[PER_PAGE];

			for (int i = 0; i < n; i++)
				if ((keep[i] = kept_by_purge(page_entry(page, i), bucket, masks)))
					kept++;
			if (kept < n)
			{
				(void) keep_entries(rel, primary, cur, keep);
				n = kept;
			}
		}
		for (int i = 0; i < n; i++)
		{
			ChunkIndexEntry *e = page_entry(page, i);

			TransactionId wait = InvalidTransactionId;

			if (e->kind != CHUNK_KIND_CHUNK ||
				memcmp(e->hash, hash, CHUNK_HASH_LEN) != 0 ||
				!belongs(e, bucket, masks))
				continue;
			switch (entry_state_wait(e, cur, &wait))
			{
				case ENTRY_VISIBLE:
					*found_buf = cur;
					*found_at = i;
					return ENTRY_VISIBLE;
				case ENTRY_IN_PROGRESS:
					result = ENTRY_IN_PROGRESS;

					/*
					 * (a freeing found stands, whatever entry of the chunk
					 * follows: a move's new one, put by the transaction
					 * freeing the old, is waited for with it)
					 */
					if (freeing && *freeing)
						break;
					if (running)
						*running = wait;
					/*
					 * (a chunk put and freed by one transaction running is
					 * not the store's to wait for: whatever it does, the
					 * chunk is gone)
					 */
					if (freeing && running)
						*freeing = (wait == e->xmax && e->xmax != e->xmin);
					break;
				case ENTRY_DEAD:
					break;
			}
		}
		if (space && *space == InvalidBlockNumber && n < PER_PAGE)
			*space = BufferGetBlockNumber(cur);
		next = opaque->next;
		if (!BlockNumberIsValid(next))
		{
			if (last)
				*last = BufferGetBlockNumber(cur);
			if (cur != primary)
				UnlockReleaseBuffer(cur);
			return result;
		}
		if (cur != primary)
			UnlockReleaseBuffer(cur);
		CHECK_FOR_INTERRUPTS();
		cur = ReadBuffer(rel, next);
		LockBuffer(cur, mode);
		check_ipage(rel, next, BufferGetPage(cur), INDEX_PAGE_OVERFLOW);
	}
}

static void
release_found(Buffer primary, Buffer found)
{
	if (found != primary)
		UnlockReleaseBuffer(found);
	UnlockReleaseBuffer(primary);
}

/* ------------------------------------------------------- splitting */

/*
 * Splits the next bucket in order in two (if no other backend has since
 * the caller looked).
 */
static void set_mark(Relation idx, uint32 b, bool on);
static bool is_marked(Relation idx, uint32 b);
static int	page_marked(Relation idx, uint32 from, int max, uint32 *buckets);

static void
split_bucket(Relation rel)
{
	IndexCache *cache = load_cache(rel);
	uint32		new_b = cache->maxbucket + 1;
	uint32		lowmask = cache->lowmask;
	uint32		highmask = cache->highmask;
	uint32		old_b;
	BlockNumber old_blk;
	Buffer		primary;
	Buffer		mbuf;
	IndexMetaData *meta;
	ChunkIndexEntry *moving = NULL;
	int			nmoving = 0;
	int			cap = 0;
	bool		new_dir;
	uint32		npages;
	BlockNumber first;
	BlockNumber dir_blk;
	IndexCache	masks;

	if (new_b == 0 || new_b / DIR_PER_PAGE >= MAX_DIR)
		return;					/* (as many buckets as there can be) */
	if (new_b > highmask)
	{
		lowmask = highmask;
		highmask = (highmask << 1) | 1;
	}
	old_b = new_b & lowmask;
	old_blk = bucket_block(rel, cache, old_b);

	primary = ReadBuffer(rel, old_blk);
	LockBuffer(primary, BUFFER_LOCK_EXCLUSIVE);
	if (check_ipage(rel, old_blk, BufferGetPage(primary), INDEX_PAGE_BUCKET)->bucket != old_b)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("block %u of chunk index \"%s\" is not bucket %u's",
						old_blk, RelationGetRelationName(rel), old_b)));
	mbuf = ReadBuffer(rel, META_BLOCK);
	LockBuffer(mbuf, BUFFER_LOCK_EXCLUSIVE);
	meta = page_meta(rel, BufferGetPage(mbuf));
	if (meta->maxbucket + 1 != new_b)
	{
		/* (another backend split it meanwhile) */
		UnlockReleaseBuffer(mbuf);
		UnlockReleaseBuffer(primary);
		return;
	}

	/* the entries of the old bucket that are the new one's */
	masks.maxbucket = new_b;
	masks.lowmask = lowmask;
	masks.highmask = highmask;
	{
		Buffer		cur = primary;

		for (;;)
		{
			Page		page = BufferGetPage(cur);
			BlockNumber next = ((IndexPageOpaque) PageGetSpecialPointer(page))->next;
			int			n = page_entries(page);

			for (int i = 0; i < n; i++)
			{
				ChunkIndexEntry *e = page_entry(page, i);

				if (belongs(e, new_b, &masks))
				{
					if (nmoving == cap)
					{
						cap = cap ? cap * 2 : PER_PAGE;
						moving = moving ? repalloc(moving, cap * sizeof(ChunkIndexEntry)) :
							palloc(cap * sizeof(ChunkIndexEntry));
					}
					moving[nmoving++] = *e;
				}
			}
			if (cur != primary)
				UnlockReleaseBuffer(cur);
			if (!BlockNumberIsValid(next))
				break;
			cur = ReadBuffer(rel, next);
			LockBuffer(cur, BUFFER_LOCK_SHARE);
			check_ipage(rel, next, BufferGetPage(cur), INDEX_PAGE_OVERFLOW);
		}
	}

	/*
	 * The new bucket's pages (and a directory page, if its slot needs one),
	 * made where nothing reaches them yet
	 */
	new_dir = new_b / DIR_PER_PAGE >= meta->ndir;
	npages = Max(1, (nmoving + PER_PAGE - 1) / PER_PAGE);
	first = meta->next_block + (new_dir ? 1 : 0);
	if (meta->next_block > MaxBlockNumber - npages - 1)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("chunk index \"%s\" is full", RelationGetRelationName(rel))));
	for (uint32 p = 0; p < npages; p++)
	{
		BlockNumber blk = first + p;
		Buffer		buf = new_page_buffer(rel, blk);
		int			from = p * PER_PAGE;
		int			n = Max(Min(nmoving - from, (int) PER_PAGE), 0);
		xl_chunk_index_init_page xlrec = {
			.kind = p == 0 ? INDEX_PAGE_BUCKET : INDEX_PAGE_OVERFLOW,
			.bucket = new_b,
			.split_at = new_b,
			.next = p + 1 < npages ? blk + 1 : InvalidBlockNumber,
			.len = n * sizeof(ChunkIndexEntry),
		};
		LoggedPage	lp = {.buf = buf, .will_init = true,
			.data = moving + from, .len = xlrec.len};

		START_CRIT_SECTION();
		ipage_make(BufferGetPage(buf), xlrec.kind, xlrec.bucket, xlrec.split_at,
				   xlrec.next, moving + from, xlrec.len);
		log_index_change(rel, XLOG_CHUNK_INDEX_INIT_PAGE, &xlrec, sizeof(xlrec), &lp, 1,
						 InvalidBuffer);
		END_CRIT_SECTION();
		UnlockReleaseBuffer(buf);
	}
	if (new_dir)
	{
		Buffer		buf;
		uint32		slot = new_b % DIR_PER_PAGE;
		BlockNumber *slots = palloc0((slot + 1) * sizeof(BlockNumber));
		xl_chunk_index_init_page xlrec = {
			.kind = INDEX_PAGE_DIR,
			.next = InvalidBlockNumber,
			.len = (slot + 1) * sizeof(BlockNumber),
		};
		LoggedPage	lp = {.will_init = true, .data = slots, .len = xlrec.len};

		dir_blk = meta->next_block;
		slots[slot] = first;
		buf = new_page_buffer(rel, dir_blk);
		lp.buf = buf;
		START_CRIT_SECTION();
		ipage_make(BufferGetPage(buf), INDEX_PAGE_DIR, 0, 0, InvalidBlockNumber,
				   slots, xlrec.len);
		log_index_change(rel, XLOG_CHUNK_INDEX_INIT_PAGE, &xlrec, sizeof(xlrec), &lp, 1,
						 InvalidBuffer);
		END_CRIT_SECTION();
		UnlockReleaseBuffer(buf);
		pfree(slots);
	}
	else
		dir_blk = meta->dir[new_b / DIR_PER_PAGE];
	INJECTION_POINT("chunk-index-split-copied", NULL);

	/* one record makes it a bucket */
	{
		xl_chunk_index_split xlrec = {
			.new_bucket = new_b,
			.lowmask = lowmask,
			.highmask = highmask,
			.first = first,
			.next_block = first + npages,
			.dir_block = dir_blk,
			.new_dir = new_dir,
		};
		LoggedPage	lps[3] = {{.buf = mbuf}, {.buf = primary}};
		Buffer		dbuf = InvalidBuffer;

		if (!new_dir)
		{
			dbuf = ReadBuffer(rel, dir_blk);
			LockBuffer(dbuf, BUFFER_LOCK_EXCLUSIVE);
			check_ipage(rel, dir_blk, BufferGetPage(dbuf), INDEX_PAGE_DIR);
			lps[2].buf = dbuf;
		}
		START_CRIT_SECTION();
		imeta_split(BufferGetPage(mbuf), &xlrec);
		((IndexPageOpaque) PageGetSpecialPointer(BufferGetPage(primary)))->split_at = new_b;
		if (!new_dir)
			idir_set(BufferGetPage(dbuf), new_b % DIR_PER_PAGE, first);
		log_index_change(rel, XLOG_CHUNK_INDEX_SPLIT, &xlrec, sizeof(xlrec),
						 lps, new_dir ? 2 : 3, primary);
		END_CRIT_SECTION();
		if (BufferIsValid(dbuf))
			UnlockReleaseBuffer(dbuf);
	}
	UnlockReleaseBuffer(mbuf);

	/* (a bucket marked: the new one, which some of its chunks are now, too) */
	if (is_marked(rel, old_b))
		set_mark(rel, new_b, true);

	INJECTION_POINT("chunk-index-split-published", NULL);

	/* and then out of the old bucket */
	masks.maxbucket = new_b;
	{
		Buffer		cur = primary;

		for (;;)
		{
			Page		page = BufferGetPage(cur);
			BlockNumber next = ((IndexPageOpaque) PageGetSpecialPointer(page))->next;
			int			n = page_entries(page);
			bool		keep[PER_PAGE];

			for (int i = 0; i < n; i++)
				keep[i] = belongs(page_entry(page, i), old_b, &masks);
			(void) keep_entries(rel, primary, cur, keep);
			if (cur != primary)
				UnlockReleaseBuffer(cur);
			if (!BlockNumberIsValid(next))
				break;
			cur = ReadBuffer(rel, next);
			LockBuffer(cur, BUFFER_LOCK_EXCLUSIVE);
			check_ipage(rel, next, BufferGetPage(cur), INDEX_PAGE_OVERFLOW);
		}
	}
	UnlockReleaseBuffer(primary);
	if (moving)
		pfree(moving);
}

/* ------------------------------------------------------- marks */

/*
 * A bucket marked may hold a chunk nothing holds: one whose last hold a
 * transaction let go of, or whose counts another transaction was changing
 * as one let go of one (neither then sees the last go).  The freer frees
 * what nothing holds in the buckets marked, and unmarks each that then holds
 * none, nor a change or entry of a transaction running: freeing costs the
 * buckets marked, not the index.  A bucket is marked and unmarked with its
 * first page locked exclusively, so an unmark sees every change made before
 * it; a split marks the new bucket of a bucket marked, the old one's first
 * page locked (not the new one's: a mark more is only a look more).  The
 * marks are a bitmap, a bit a bucket, on pages the meta page lists, made
 * as buckets reach them (locks: a bucket's first page, the meta page, a
 * marks' page).
 */

/* the marks' pages up to the kth made (a bucket's first page locked) */
static void
add_marks(Relation idx, uint32 k)
{
	Buffer		mbuf = ReadBuffer(idx, META_BLOCK);
	IndexMetaData *meta;

	LockBuffer(mbuf, BUFFER_LOCK_EXCLUSIVE);
	meta = page_meta(idx, BufferGetPage(mbuf));
	while (meta->nmarks <= k)
	{
		BlockNumber blk = meta->next_block;
		xl_chunk_index_add_marks xlrec = {.nth = meta->nmarks};
		Buffer		nbuf;
		LoggedPage	lps[2];

		if (meta->nmarks >= MAX_MARKS || blk >= MaxBlockNumber)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("chunk index \"%s\" is full", RelationGetRelationName(idx))));
		nbuf = new_page_buffer(idx, blk);
		lps[0] = (LoggedPage) {.buf = mbuf};
		lps[1] = (LoggedPage) {.buf = nbuf, .will_init = true};
		START_CRIT_SECTION();
		imeta_add_marks(BufferGetPage(mbuf), xlrec.nth, blk);
		imarks_init(BufferGetPage(nbuf), xlrec.nth);
		log_index_change(idx, XLOG_CHUNK_INDEX_ADD_MARKS, &xlrec, sizeof(xlrec), lps, 2,
						 InvalidBuffer);
		END_CRIT_SECTION();
		UnlockReleaseBuffer(nbuf);
	}
	UnlockReleaseBuffer(mbuf);
}

/* the marks' page of bucket b, pinned; none if it is not made and not to be */
static Buffer
marks_page(Relation idx, uint32 b, bool make)
{
	uint32		k = b / MARK_BITS;
	IndexCache *cache = get_cache(idx);

	if (k >= cache->nmarks)
		cache = load_cache(idx);
	if (k >= cache->nmarks)
	{
		if (!make)
			return InvalidBuffer;
		add_marks(idx, k);
		cache = load_cache(idx);
	}
	return ReadBuffer(idx, cache->marks[k]);
}

/* bucket b marked, or unmarked, WAL-logged (its first page locked) */
static void
set_mark(Relation idx, uint32 b, bool on)
{
	Buffer		buf = marks_page(idx, b, on);
	uint32		bit = b % MARK_BITS;
	xl_chunk_index_mark xlrec = {.bit = bit, .on = on};
	LoggedPage	lp;

	if (!BufferIsValid(buf))
		return;
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	check_ipage(idx, BufferGetBlockNumber(buf), BufferGetPage(buf), INDEX_PAGE_MARKS);
	if (imarks_get(BufferGetPage(buf), bit) == on)
	{
		UnlockReleaseBuffer(buf);
		return;
	}
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	if (imarks_get(BufferGetPage(buf), bit) == on)
	{
		UnlockReleaseBuffer(buf);
		return;
	}
	lp = (LoggedPage) {.buf = buf};
	START_CRIT_SECTION();
	imarks_set(BufferGetPage(buf), bit, on);
	log_index_change(idx, XLOG_CHUNK_INDEX_MARK, &xlrec, sizeof(xlrec), &lp, 1, InvalidBuffer);
	END_CRIT_SECTION();
	UnlockReleaseBuffer(buf);
}

/* whether bucket b is marked (as it was read: nothing locked) */
static bool
is_marked(Relation idx, uint32 b)
{
	Buffer		buf = marks_page(idx, b, false);
	bool		on;

	if (!BufferIsValid(buf))
		return false;
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	check_ipage(idx, BufferGetBlockNumber(buf), BufferGetPage(buf), INDEX_PAGE_MARKS);
	on = imarks_get(BufferGetPage(buf), b % MARK_BITS);
	UnlockReleaseBuffer(buf);
	return on;
}

/*
 * The buckets marked from bucket from, up to max of them, in buckets[];
 * how many.  The marks' pages read, each in share lock alone.
 */
static int
page_marked(Relation idx, uint32 from, int max, uint32 *buckets)
{
	IndexCache	cache = *load_cache(idx);
	uint32		nbuckets = cache.maxbucket + 1;
	int			n = 0;

	for (uint32 k = from / MARK_BITS; k < cache.nmarks && n < max; k++)
	{
		Buffer		buf = ReadBuffer(idx, cache.marks[k]);
		const uint8 *bits;

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		check_ipage(idx, cache.marks[k], BufferGetPage(buf), INDEX_PAGE_MARKS);
		bits = (const uint8 *) PageGetContents(BufferGetPage(buf));
		for (uint32 bit = k == from / MARK_BITS ? from % MARK_BITS : 0;
			 bit < MARK_BITS && n < max; bit++)
		{
			uint32		b = k * MARK_BITS + bit;

			if (b >= nbuckets)
				break;
			if (bit % 8 == 0 && bits[bit / 8] == 0)
			{
				bit += 7;
				continue;
			}
			if ((bits[bit / 8] >> (bit % 8)) & 1)
				buckets[n++] = b;
		}
		UnlockReleaseBuffer(buf);
	}
	return n;
}

/* ------------------------------------------------------- the routine */

/*
 * The changes of hash's counts on the chain of the bucket whose first page
 * is primary (locked): those this transaction sees added to *drefs and
 * *droots; the transaction of a decrease of roots it doesn't see
 * (another's, running) in *running, if it is given (an increase it doesn't
 * see can only add to the counts, and references are let go of by one
 * transaction each: see page_adjust), or of any change, if any_running;
 * and where this very transaction's own change is, in
 * *own_blk and *own_at, if they are given.
 */
static void
count_changes(Relation rel, Buffer primary, int mode,
			  const ChunkIndexEntry *chunk, uint32 bucket,
			  const IndexCache *masks, bool any_running, int64 *drefs,
			  int64 *droots, TransactionId *running, BlockNumber *own_blk,
			  int *own_at, bool *any_own, TransactionId *other)
{
	Buffer		cur = primary;
	TransactionId mine = own_blk ? GetCurrentTransactionIdIfAny() : InvalidTransactionId;

	*drefs = *droots = 0;
	if (running)
		*running = InvalidTransactionId;
	if (other)
		*other = InvalidTransactionId;
	if (any_own)
		*any_own = false;
	if (own_blk)
		*own_blk = InvalidBlockNumber;
	for (;;)
	{
		Page		page = BufferGetPage(cur);
		BlockNumber next = ((IndexPageOpaque) PageGetSpecialPointer(page))->next;
		int			n = page_entries(page);

		for (int i = 0; i < n; i++)
		{
			ChunkIndexEntry *e = page_entry(page, i);

			if (!change_of(e, chunk) || !belongs(e, bucket, masks))
				continue;
			switch (entry_state(e, cur))
			{
				case ENTRY_VISIBLE:
					*drefs += (int32) e->refs;
					*droots += (int32) e->roots;
					if (any_own && TransactionIdIsCurrentTransactionId(e->xmin))
						*any_own = true;
					if (own_blk && TransactionIdIsValid(mine) && e->xmin == mine)
					{
						*own_blk = BufferGetBlockNumber(cur);
						*own_at = i;
					}
					break;
				case ENTRY_IN_PROGRESS:
					if (running && (any_running || (int32) e->roots < 0))
						*running = e->xmin;
					if (other)
						*other = e->xmin;
					break;
				case ENTRY_DEAD:
					break;
			}
		}
		if (cur != primary)
			UnlockReleaseBuffer(cur);
		if (!BlockNumberIsValid(next))
			return;
		cur = ReadBuffer(rel, next);
		LockBuffer(cur, mode);
		check_ipage(rel, next, BufferGetPage(cur), INDEX_PAGE_OVERFLOW);
	}
}

/*
 * The first page with room on the chain of the bucket whose first page is
 * primary (locked exclusively), or InvalidBlockNumber, and its last page.
 */
static void
chain_room(Relation rel, Buffer primary, BlockNumber *space, BlockNumber *last)
{
	Buffer		cur = primary;

	*space = InvalidBlockNumber;
	for (;;)
	{
		Page		page = BufferGetPage(cur);
		BlockNumber next = ((IndexPageOpaque) PageGetSpecialPointer(page))->next;

		if (*space == InvalidBlockNumber && page_entries(page) < PER_PAGE)
			*space = BufferGetBlockNumber(cur);
		if (!BlockNumberIsValid(next))
		{
			*last = BufferGetBlockNumber(cur);
			if (cur != primary)
				UnlockReleaseBuffer(cur);
			return;
		}
		if (cur != primary)
			UnlockReleaseBuffer(cur);
		cur = ReadBuffer(rel, next);
		LockBuffer(cur, BUFFER_LOCK_EXCLUSIVE);
		check_ipage(rel, next, BufferGetPage(cur), INDEX_PAGE_OVERFLOW);
	}
}

/*
 * Adds entry to the bucket whose first page is primary (locked
 * exclusively): onto the page with room space, or a new page at the
 * chain's end, last.  Answers whether the entry went past the first page.
 */
static bool
add_entry(Relation idx, Buffer primary, uint32 bucket, BlockNumber space,
		  BlockNumber last, ChunkIndexEntry *entry, ChunkReserveFn reserve,
		  void *arg)
{
	if (BlockNumberIsValid(space))
	{
		Buffer		buf = primary;
		LoggedPage	lp;

		if (space != BufferGetBlockNumber(primary))
		{
			buf = ReadBuffer(idx, space);
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		}
		lp.buf = buf;
		lp.will_init = false;
		lp.data = entry;
		lp.len = sizeof(*entry);
		if (reserve)
			reserve(arg, entry);
		START_CRIT_SECTION();
		ipage_add(BufferGetPage(buf), entry);
		log_index_change(idx, XLOG_CHUNK_INDEX_ADD_ENTRY, NULL, 0, &lp, 1, primary);
		END_CRIT_SECTION();
		if (buf == primary)
			return false;
		UnlockReleaseBuffer(buf);
		return true;
	}
	else
	{
		/* a new page at the chain's end, from the meta page */
		Buffer		mbuf = ReadBuffer(idx, META_BLOCK);
		Buffer		lbuf = primary;
		Buffer		nbuf;
		IndexMetaData *meta;
		BlockNumber blk;
		xl_chunk_index_add_page xlrec = {.bucket = bucket};
		LoggedPage	lps[3];

		if (last != BufferGetBlockNumber(primary))
		{
			lbuf = ReadBuffer(idx, last);
			LockBuffer(lbuf, BUFFER_LOCK_EXCLUSIVE);
		}
		LockBuffer(mbuf, BUFFER_LOCK_EXCLUSIVE);
		meta = page_meta(idx, BufferGetPage(mbuf));
		blk = meta->next_block;
		if (blk >= MaxBlockNumber)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("chunk index \"%s\" is full", RelationGetRelationName(idx))));
		nbuf = new_page_buffer(idx, blk);
		lps[0] = (LoggedPage) {.buf = mbuf};
		lps[1] = (LoggedPage) {.buf = lbuf};
		lps[2] = (LoggedPage) {.buf = nbuf, .will_init = true,
			.data = entry, .len = sizeof(*entry)};
		if (reserve)
			reserve(arg, entry);
		START_CRIT_SECTION();
		((IndexMetaData *) PageGetContents(BufferGetPage(mbuf)))->next_block = blk + 1;
		((IndexPageOpaque) PageGetSpecialPointer(BufferGetPage(lbuf)))->next = blk;
		ipage_make(BufferGetPage(nbuf), INDEX_PAGE_OVERFLOW, bucket, 0,
				   InvalidBlockNumber, entry, sizeof(*entry));
		log_index_change(idx, XLOG_CHUNK_INDEX_ADD_PAGE, &xlrec, sizeof(xlrec), lps, 3,
						 primary);
		END_CRIT_SECTION();
		UnlockReleaseBuffer(nbuf);
		UnlockReleaseBuffer(mbuf);
		if (lbuf != primary)
			UnlockReleaseBuffer(lbuf);
		return true;
	}
}

static bool
counts_add(ChunkIndexEntry *out, int64 drefs, int64 droots)
{
	int64		refs = (int64) out->refs + drefs;
	int64		roots = (int64) out->roots + droots;

	if (refs < 0 || roots < 0 || refs > PG_UINT32_MAX || roots > PG_UINT32_MAX)
		return false;
	out->refs = (uint32) refs;
	out->roots = (uint32) roots;
	return true;
}

/*
 * A chunk being moved by xid (compaction: its entry freed, and a new one
 * put, by that one transaction running): the entry freed, still the
 * chunk's until xid commits, its page left locked in *found_buf (primary,
 * or another the caller releases) and its place in *found_at; false if the
 * chunk is not being moved so.
 */
static bool
find_moved(Relation rel, Buffer primary, int mode, const uint8 *hash,
		   uint32 bucket, const IndexCache *masks, TransactionId xid,
		   Buffer *found_buf, int *found_at)
{
	Buffer		cur = primary;
	Buffer		old_buf = InvalidBuffer;
	int			old_at = -1;
	bool		put = false;

	for (;;)
	{
		Page		page = BufferGetPage(cur);
		IndexPageOpaque opaque = (IndexPageOpaque) PageGetSpecialPointer(page);
		BlockNumber next;
		int			n = page_entries(page);
		bool		keep = false;

		for (int i = 0; i < n; i++)
		{
			ChunkIndexEntry *e = page_entry(page, i);

			if (e->kind != CHUNK_KIND_CHUNK ||
				memcmp(e->hash, hash, CHUNK_HASH_LEN) != 0 ||
				!belongs(e, bucket, masks))
				continue;
			/* (one this transaction put and freed is neither) */
			if (TransactionIdEquals(e->xmin, xid) && TransactionIdEquals(e->xmax, xid))
				continue;
			if (TransactionIdEquals(e->xmin, xid) &&
				!(e->flags & (CHUNK_ENTRY_FROZEN | CHUNK_ENTRY_COMMITTED)))
				put = true;
			else if (TransactionIdEquals(e->xmax, xid) && !BufferIsValid(old_buf))
			{
				old_buf = cur;
				old_at = i;
				keep = true;
			}
		}
		next = opaque->next;
		if (cur != primary && !keep)
			UnlockReleaseBuffer(cur);
		if (!BlockNumberIsValid(next))
			break;
		CHECK_FOR_INTERRUPTS();
		cur = ReadBuffer(rel, next);
		LockBuffer(cur, mode);
		check_ipage(rel, next, BufferGetPage(cur), INDEX_PAGE_OVERFLOW);
	}
	if (!put || !BufferIsValid(old_buf))
	{
		if (BufferIsValid(old_buf) && old_buf != primary)
			UnlockReleaseBuffer(old_buf);
		return false;
	}
	*found_buf = old_buf;
	*found_at = old_at;
	return true;
}

static bool
page_lookup(Relation idx, const uint8 *hash, ChunkIndexEntry *out)
{
	uint32		bucket;
	IndexCache	masks;
	Buffer		primary;
	Buffer		found;
	int			at;
	int64		drefs,
				droots;
	TransactionId running;
	bool		freeing;

	/*
	 * (a chunk being freed is waited for: it is there again, or gone; one
	 * being put by another is not the store's yet)
	 */
	for (;;)
	{
		EntryState	st;

		freeing = false;
		primary = lock_bucket(idx, hash, BUFFER_LOCK_SHARE, &bucket, &masks);
		st = find_entry_freeing(idx, primary, BUFFER_LOCK_SHARE, hash, bucket,
								&masks, false, &found, &at, NULL, NULL,
								&running, &freeing);
		if (st == ENTRY_VISIBLE)
			break;

		/*
		 * (a chunk being moved is read where it was, not waited for: the
		 * same chunk either way, and its old bytes stay until no snapshot
		 * might read them, a reader's included)
		 */
		if (st == ENTRY_IN_PROGRESS && freeing &&
			find_moved(idx, primary, BUFFER_LOCK_SHARE, hash, bucket, &masks,
					   running, &found, &at))
			break;
		UnlockReleaseBuffer(primary);
		if (st != ENTRY_IN_PROGRESS || !freeing)
			return false;
		XactLockTableWait(running, NULL, NULL, XLTW_None);
	}
	*out = *page_entry(BufferGetPage(found), at);
	if (found != primary)
		UnlockReleaseBuffer(found);
	count_changes(idx, primary, BUFFER_LOCK_SHARE, out, bucket, &masks,
				  false, &drefs, &droots, NULL, NULL, NULL, NULL, NULL);
	UnlockReleaseBuffer(primary);
	if (!counts_add(out, drefs, droots))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("a chunk's reference counts are out of range")));
	return true;
}

static bool
page_insert(Relation idx, ChunkIndexEntry *entry, ChunkReserveFn reserve,
			void *arg)
{
	uint32		bucket;
	IndexCache	masks;
	Buffer		primary;
	Buffer		found;
	int			at;
	BlockNumber space;
	BlockNumber last;
	bool		grew;
	TransactionId running;

	PreventCommandIfReadOnly("insert into a chunk index");
	entry->xmin = GetCurrentTransactionId();
	entry->xmax = InvalidTransactionId;
	entry->flags = 0;
	entry->kind = CHUNK_KIND_CHUNK;
	entry->merge_xid = InvalidTransactionId;
	entry->unused = 0;
	for (;;)
	{
		primary = lock_bucket(idx, entry->hash, BUFFER_LOCK_EXCLUSIVE, &bucket,
							  &masks);
		switch (find_entry(idx, primary, BUFFER_LOCK_EXCLUSIVE, entry->hash,
						   bucket, &masks, true, &found, &at, &space, &last,
						   &running))
		{
			case ENTRY_VISIBLE:
				*entry = *page_entry(BufferGetPage(found), at);
				release_found(primary, found);
				return false;
			case ENTRY_IN_PROGRESS:
				/* (as for a unique index: whether it is there is up to it) */
				UnlockReleaseBuffer(primary);
				XactLockTableWait(running, NULL, NULL, XLTW_None);
				continue;
			case ENTRY_DEAD:
				break;
		}
		break;
	}
	grew = add_entry(idx, primary, bucket, space, last, entry, reserve, arg);
	UnlockReleaseBuffer(primary);

	/* (counted as a table's inserts are, so that autovacuum VACUUMs it) */
	pgstat_count_heap_insert(idx, 1);

	/*
	 * An entry past a bucket's first page: the index grows a bucket, so that
	 * buckets stay a page long, mostly
	 */
	if (grew)
		split_bucket(idx);
	return true;
}

/* whether this transaction's own change of a chunk has drefs and droots in it */
static bool
own_change_covers(Relation idx, Buffer primary, BlockNumber own_blk, int own_at,
				  int32 drefs, int32 droots)
{
	ChunkIndexEntry own;

	if (!BlockNumberIsValid(own_blk))
		return false;
	if (own_blk == BufferGetBlockNumber(primary))
		own = *page_entry(BufferGetPage(primary), own_at);
	else
	{
		Buffer		buf = ReadBuffer(idx, own_blk);

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		own = *page_entry(BufferGetPage(buf), own_at);
		UnlockReleaseBuffer(buf);
	}
	return (int64) (int32) own.refs + drefs >= 0 &&
		(int64) (int32) own.roots + droots >= 0;
}

static bool
page_adjust(Relation idx, const uint8 *hash, int32 drefs, int32 droots,
			ChunkIndexEntry *out)
{
	uint32		bucket;
	IndexCache	masks;
	Buffer		primary;
	Buffer		found;
	int			at;
	BlockNumber space;
	BlockNumber last;
	ChunkIndexEntry change;
	int64		crefs,
				croots;
	TransactionId running;
	TransactionId other;
	BlockNumber own_blk;
	int			own_at;
	bool		any_own;
	bool		marked = false;

	PreventCommandIfReadOnly("change a chunk index");
	memset(&change, 0, sizeof(change));
	memcpy(change.hash, hash, CHUNK_HASH_LEN);
	change.refs = (uint32) drefs;
	change.roots = (uint32) droots;
	change.xmin = GetCurrentTransactionId();
	change.kind = CHUNK_KIND_CHANGE;
	for (;;)
	{
		EntryState	st;
		bool		freeing = false;

		primary = lock_bucket(idx, hash, BUFFER_LOCK_EXCLUSIVE, &bucket, &masks);
		st = find_entry_freeing(idx, primary, BUFFER_LOCK_EXCLUSIVE, hash, bucket,
								&masks, true, &found, &at, &space, &last,
								&running, &freeing);
		if (st != ENTRY_VISIBLE)
		{
			UnlockReleaseBuffer(primary);
			if (st != ENTRY_IN_PROGRESS || !freeing)
				return false;
			/* (being freed: wait to see) */
			XactLockTableWait(running, NULL, NULL, XLTW_None);
			continue;
		}
		*out = *page_entry(BufferGetPage(found), at);
		if (found != primary)
			UnlockReleaseBuffer(found);
		/*
		 * (and whether another transaction changes the chunk's counts: one
		 * letting go of a hold of it, its last but this one's, perhaps)
		 */
		count_changes(idx, primary, BUFFER_LOCK_EXCLUSIVE, out, bucket, &masks,
					  false, &crefs, &croots, &running, &own_blk, &own_at, &any_own,
					  &other);

		/*
		 * (a root let go of waits for another's, as a row's update waits for
		 * another's: two taking the last root away would both see it.  One
		 * taking back no more than this transaction's own increase does not:
		 * no other sees what it takes back.  Nor does a reference let go
		 * of: one is let go of only by the freeing of the chunk it is from,
		 * which one transaction alone may free, so no two let go of one
		 * reference, and freeings, each letting go of the references of
		 * the chunks it frees in an order of its own, wait for no other's)
		 */
		if (droots < 0 && TransactionIdIsValid(running) &&
			!own_change_covers(idx, primary, own_blk, own_at, drefs, droots))
		{
			UnlockReleaseBuffer(primary);
			XactLockTableWait(running, NULL, NULL, XLTW_None);
			continue;
		}
		break;
	}
	{
		int64		refs = (int64) out->refs + crefs + drefs;
		int64		roots = (int64) out->roots + croots + droots;

		if (refs < 0 || roots < 0 || refs > PG_UINT32_MAX || roots > PG_UINT32_MAX)
		{
			UnlockReleaseBuffer(primary);
			ereport(ERROR,
					(errcode(refs < 0 || roots < 0 ? ERRCODE_INVALID_PARAMETER_VALUE :
							 ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("a chunk's reference counts would be %lld and %lld",
							(long long) refs, (long long) roots)));
		}
		out->refs = (uint32) refs;
		out->roots = (uint32) roots;
	}

	if (any_own && drefs == 0 && droots == 0)
	{
		/*
		 * (a change of this transaction's, or of one it is part of, holds the
		 * chunk already, for as long as this subtransaction runs)
		 */
	}
	else if (BlockNumberIsValid(own_blk))
	{
		/* this transaction's change of the chunk's counts, changed again */
		Buffer		buf = own_blk == BufferGetBlockNumber(primary) ? primary :
			ReadBuffer(idx, own_blk);
		ChunkIndexEntry *e;
		xl_chunk_index_slot xlrec = {.slot = own_at};

		if (buf != primary)
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		e = page_entry(BufferGetPage(buf), own_at);
		xlrec.refs = (uint32) ((int32) e->refs + drefs);
		xlrec.roots = (uint32) ((int32) e->roots + droots);
		START_CRIT_SECTION();
		ipage_set_counts(BufferGetPage(buf), own_at, xlrec.refs, xlrec.roots);
		log_slot_change(idx, primary, buf, XLOG_CHUNK_INDEX_SET_COUNTS, &xlrec);
		END_CRIT_SECTION();
		if (buf != primary)
			UnlockReleaseBuffer(buf);
	}
	else
	{
		change.loc = out->loc;
		change.xmax = out->xmin;

		/*
		 * (a change past the bucket's first page splits no bucket: the
		 * changes of one chunk are all in its bucket, and VACUUM takes them)
		 */
		chain_room(idx, primary, &space, &last);
		(void) add_entry(idx, primary, bucket, space, last, &change, NULL, NULL);
		pgstat_count_heap_insert(idx, 1);
	}

	/*
	 * (a hold let go of, and none left, or another transaction changing the
	 * counts: the bucket marked, for the freer, which this transaction's
	 * commit starts)
	 */
	if ((drefs < 0 || droots < 0) &&
		((out->refs == 0 && out->roots == 0) || TransactionIdIsValid(other)))
	{
		set_mark(idx, bucket, true);
		marked = true;
	}
	UnlockReleaseBuffer(primary);
	ChunkFreerChanged(idx, marked);
	return true;
}

/*
 * Sets xmax of the chunk's entry, this transaction's, if nothing holds the
 * chunk: its counts (committed, and this transaction's) zero, no other
 * transaction changing them, and not being freed already.
 */
static bool
page_free(Relation idx, const uint8 *hash, bool (*held) (Relation idx, const uint8 *hash),
		  ChunkIndexEntry *out)
{
	uint32		bucket;
	IndexCache	masks;
	Buffer		primary;
	Buffer		found;
	int			at;
	int64		drefs,
				droots;
	TransactionId running;
	TransactionId xid;
	xl_chunk_index_slot xlrec = {0};

	PreventCommandIfReadOnly("free a chunk");
	xid = GetCurrentTransactionId();
	primary = lock_bucket(idx, hash, BUFFER_LOCK_EXCLUSIVE, &bucket, &masks);
	if (find_entry(idx, primary, BUFFER_LOCK_EXCLUSIVE, hash, bucket, &masks,
				   false, &found, &at, NULL, NULL, NULL) != ENTRY_VISIBLE)
	{
		UnlockReleaseBuffer(primary);
		return false;
	}
	*out = *page_entry(BufferGetPage(found), at);

	/*
	 * (the chain's pages are each locked to count the changes: the entry's,
	 * if not the first, let go of meanwhile, nothing changing a chain but
	 * with its first page locked, as it is)
	 */
	if (found != primary)
		LockBuffer(found, BUFFER_LOCK_UNLOCK);
	count_changes(idx, primary, BUFFER_LOCK_EXCLUSIVE, out, bucket, &masks,
				  true, &drefs, &droots, &running, NULL, NULL, NULL, NULL);
	if (found != primary)
		LockBuffer(found, BUFFER_LOCK_EXCLUSIVE);
	/*
	 * (a chunk this transaction is putting is held by it until it ends:
	 * another's freer sees no such chunk, its entry not committed)
	 */
	if ((int64) out->refs + drefs != 0 || (int64) out->roots + droots != 0 ||
		TransactionIdIsValid(running) || ChunkEntryIsMine(out) ||
		(held && held(idx, hash)))
	{
		release_found(primary, found);
		return false;
	}
	xlrec.slot = at;
	xlrec.xmax = xid;
	START_CRIT_SECTION();
	ipage_set_xmax(BufferGetPage(found), at, xid);
	log_slot_change(idx, primary, found, XLOG_CHUNK_INDEX_SET_XMAX, &xlrec);
	END_CRIT_SECTION();
	pgstat_count_heap_delete(idx);
	out->refs = 0;
	out->roots = 0;
	release_found(primary, found);
	return true;
}

static bool
page_remove(Relation idx, const uint8 *hash)
{
	uint32		bucket;
	IndexCache	masks;
	Buffer		primary;
	Buffer		found;
	int			at;
	bool		removed = false;

	PreventCommandIfReadOnly("change a chunk index");
	(void) GetCurrentTransactionId();
	primary = lock_bucket(idx, hash, BUFFER_LOCK_EXCLUSIVE, &bucket, &masks);

	/*
	 * the chunk's entry, and every entry of its hash: other transactions'
	 * changes too (for tests, which take a chunk out from under the store)
	 */
	while (find_entry(idx, primary, BUFFER_LOCK_EXCLUSIVE, hash, bucket, &masks,
					  false, &found, &at, NULL, NULL, NULL) == ENTRY_VISIBLE)
	{
		remove_slot(idx, primary, found, at);
		if (found != primary)
			UnlockReleaseBuffer(found);
		removed = true;
	}
	if (removed)
	{
		Buffer		cur = primary;

		for (;;)
		{
			Page		page = BufferGetPage(cur);
			BlockNumber next = ((IndexPageOpaque) PageGetSpecialPointer(page))->next;
			int			n = page_entries(page);
			bool		keep[PER_PAGE];

			for (int i = 0; i < n; i++)
				keep[i] = memcmp(page_entry(page, i)->hash, hash, CHUNK_HASH_LEN) != 0;
			(void) keep_entries(idx, primary, cur, keep);
			if (cur != primary)
				UnlockReleaseBuffer(cur);
			if (!BlockNumberIsValid(next))
				break;
			cur = ReadBuffer(idx, next);
			LockBuffer(cur, BUFFER_LOCK_EXCLUSIVE);
			check_ipage(idx, next, BufferGetPage(cur), INDEX_PAGE_OVERFLOW);
		}
	}
	UnlockReleaseBuffer(primary);
	return removed;
}

/* a bucket's chunks this transaction sees, their counts added up */
typedef struct BucketChunks
{
	ChunkIndexEntry *chunks;
	int			n;
	int			cap;
} BucketChunks;

/*
 * Bucket b's first page, read and locked in mode, its chunks this
 * transaction sees in *bc, each's counts its own and its changes' it sees
 * added; in *wait (if given) a transaction running that changes a chunk's
 * counts there, or frees one (none: InvalidTransactionId).
 */
static Buffer
read_bucket(Relation idx, uint32 b, int mode, BucketChunks *bc, TransactionId *wait)
{
	IndexCache	masks = *get_cache(idx);
	BlockNumber blk = bucket_block(idx, &masks, b);
	Buffer		primary = ReadBuffer(idx, blk);
	Buffer		cur;
	ChunkIndexEntry *changes = NULL;
	int			nchanges = 0,
				cap2 = 0;

	bc->n = 0;
	if (wait)
		*wait = InvalidTransactionId;
	LockBuffer(primary, mode);
	check_ipage(idx, blk, BufferGetPage(primary), INDEX_PAGE_BUCKET);
	if (((IndexPageOpaque) PageGetSpecialPointer(BufferGetPage(primary)))->split_at > masks.maxbucket)
		masks = *load_cache(idx);
	cur = primary;
	for (;;)
	{
		Page		page = BufferGetPage(cur);
		BlockNumber next = ((IndexPageOpaque) PageGetSpecialPointer(page))->next;
		int			n = page_entries(page);

		for (int i = 0; i < n; i++)
		{
			ChunkIndexEntry *e = page_entry(page, i);
			TransactionId running = InvalidTransactionId;
			EntryState	st;

			if (!belongs(e, b, &masks))
				continue;
			st = entry_state_wait(e, cur, &running);
			/* (a chunk's put running: not a change of counts, nor a freeing) */
			if (st == ENTRY_IN_PROGRESS && wait &&
				(e->kind == CHUNK_KIND_CHANGE || running == e->xmax))
				*wait = running;
			if (st != ENTRY_VISIBLE)
				continue;
			if (e->kind == CHUNK_KIND_CHUNK)
			{
				if (bc->n == bc->cap)
				{
					bc->cap = bc->cap ? bc->cap * 2 : 64;
					bc->chunks = bc->chunks ? repalloc(bc->chunks, bc->cap * sizeof(*bc->chunks)) :
						palloc(bc->cap * sizeof(*bc->chunks));
				}
				bc->chunks[bc->n++] = *e;
			}
			else
			{
				if (nchanges == cap2)
				{
					cap2 = cap2 ? cap2 * 2 : 64;
					changes = changes ? repalloc(changes, cap2 * sizeof(*changes)) :
						palloc(cap2 * sizeof(*changes));
				}
				changes[nchanges++] = *e;
			}
		}
		if (cur != primary)
			UnlockReleaseBuffer(cur);
		if (!BlockNumberIsValid(next))
			break;
		cur = ReadBuffer(idx, next);
		LockBuffer(cur, mode);
		check_ipage(idx, next, BufferGetPage(cur), INDEX_PAGE_OVERFLOW);
	}

	for (int i = 0; i < bc->n; i++)
	{
		int64		drefs = 0,
					droots = 0;

		/* (summed first: in no order, they may dip below zero midway) */
		for (int j = 0; j < nchanges; j++)
			if (change_of(&changes[j], &bc->chunks[i]))
			{
				drefs += (int32) changes[j].refs;
				droots += (int32) changes[j].roots;
			}
		if (!counts_add(&bc->chunks[i], drefs, droots))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("a chunk's reference counts are out of range")));
	}
	if (changes)
		pfree(changes);
	return primary;
}

/*
 * Each chunk's entry this transaction sees, a bucket at a time: its counts
 * added up on the bucket's pages, and fn called with the bucket unlocked.
 */
static void
page_visit(Relation idx, void (*fn) (void *arg, const ChunkIndexEntry *e),
		   void *arg)
{
	BucketChunks bc = {0};

	for (uint32 b = 0; b <= load_cache(idx)->maxbucket; b++)
	{
		UnlockReleaseBuffer(read_bucket(idx, b, BUFFER_LOCK_SHARE, &bc, NULL));
		for (int i = 0; i < bc.n; i++)
			fn(arg, &bc.chunks[i]);
		CHECK_FOR_INTERRUPTS();
	}
	if (bc.chunks)
		pfree(bc.chunks);
}

/* each chunk's entry of bucket b, as page_visit's */
static void
page_visit_bucket(Relation idx, uint32 b,
				  void (*fn) (void *arg, const ChunkIndexEntry *e), void *arg)
{
	BucketChunks bc = {0};

	UnlockReleaseBuffer(read_bucket(idx, b, BUFFER_LOCK_SHARE, &bc, NULL));
	for (int i = 0; i < bc.n; i++)
		fn(arg, &bc.chunks[i]);
	if (bc.chunks)
		pfree(bc.chunks);
}

/*
 * Bucket b unmarked, if it holds no chunk nothing holds, nor a change of a
 * chunk's counts or a freeing of a transaction running (then that one's
 * in *wait); whether it was
 */
static bool
page_unmark(Relation idx, uint32 b, TransactionId *wait)
{
	BucketChunks bc = {0};
	Buffer		primary = read_bucket(idx, b, BUFFER_LOCK_EXCLUSIVE, &bc, wait);
	bool		unheld = false;

	for (int i = 0; i < bc.n && !unheld; i++)
		unheld = bc.chunks[i].refs == 0 && bc.chunks[i].roots == 0;
	if (!unheld && !TransactionIdIsValid(*wait))
		set_mark(idx, b, false);
	UnlockReleaseBuffer(primary);
	if (bc.chunks)
		pfree(bc.chunks);
	return !unheld && !TransactionIdIsValid(*wait);
}

static Oid
page_log_of(Relation idx, uint64 *stamp)
{
	Buffer		mbuf = ReadBuffer(idx, META_BLOCK);
	IndexMetaData *meta;
	Oid			log;

	LockBuffer(mbuf, BUFFER_LOCK_SHARE);
	meta = page_meta(idx, BufferGetPage(mbuf));
	log = meta->log;
	*stamp = meta->log_stamp;
	UnlockReleaseBuffer(mbuf);
	return log;
}

static void page_bind_log(Relation idx, Oid log, uint64 stamp);

bool
ChunkIndexLaneOf(Relation rel, uint32 *lane, Oid *parent)
{
	Buffer		mbuf;
	IndexMetaData *meta;

	if (RelationGetNumberOfBlocks(rel) == 0)
		return false;
	mbuf = ReadBuffer(rel, META_BLOCK);
	LockBuffer(mbuf, BUFFER_LOCK_SHARE);
	meta = page_meta(rel, BufferGetPage(mbuf));
	*lane = meta->lane;
	*parent = meta->parent;
	UnlockReleaseBuffer(mbuf);
	return true;
}

void
ChunkIndexMakeLane(Relation lane, uint32 n, Relation idx)
{
	uint64		stamp;
	Oid			log = page_log_of(idx, &stamp);
	Buffer		mbuf = ReadBuffer(lane, META_BLOCK);
	IndexMetaData *meta;

	LockBuffer(mbuf, BUFFER_LOCK_EXCLUSIVE);
	meta = page_meta(lane, BufferGetPage(mbuf));
	START_CRIT_SECTION();
	meta->lane = n;
	meta->parent = RelationGetRelid(idx);
	MarkBufferDirty(mbuf);
	if (RelationNeedsWAL(lane))
		log_newpage_buffer(mbuf, true);
	END_CRIT_SECTION();
	UnlockReleaseBuffer(mbuf);

	/* (its entries of the index's log, as the index's are) */
	if (OidIsValid(log))
		page_bind_log(lane, log, stamp);
}

/*
 * The log the index's entries are of, opened, if it is the one they are
 * of (the same storage), else NULL, with a WARNING: its dead bytes are
 * then not counted (left, not wrongly counted).
 */
static Relation
open_bound_log(Relation idx)
{
	uint64		stamp;
	Oid			logid = page_log_of(idx, &stamp);
	Relation	log;

	if (!OidIsValid(logid))
		return NULL;
	log = try_relation_open(logid, RowExclusiveLock);
	if (log == NULL)
		return NULL;
	if (!RelationIsAnyChunkLog(log) || ChunkLogOf(log)->stamp(log) != stamp)
	{
		ereport(WARNING,
				(errmsg("chunk index \"%s\" is of chunk log \"%s\" as it was: the bytes of chunks it takes out are not counted dead",
						RelationGetRelationName(idx), RelationGetRelationName(log))));
		relation_close(log, RowExclusiveLock);
		return NULL;
	}
	return log;
}

static void
page_bind_log(Relation idx, Oid log, uint64 stamp)
{
	Buffer		mbuf;
	IndexMetaData *meta;
	IndexCache *cache;

	/* (once bound, as a backend's copy of the meta page says: every put) */
	cache = get_cache(idx);
	if (cache->log == log && cache->log_stamp == stamp)
		return;
	cache = load_cache(idx);
	if (cache->log == log && cache->log_stamp == stamp)
		return;
	mbuf = ReadBuffer(idx, META_BLOCK);
	LockBuffer(mbuf, BUFFER_LOCK_EXCLUSIVE);
	meta = page_meta(idx, BufferGetPage(mbuf));
	if (meta->log == log && meta->log_stamp == stamp)
	{
		UnlockReleaseBuffer(mbuf);
		return;
	}
	if (OidIsValid(meta->log))
	{
		UnlockReleaseBuffer(mbuf);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("chunk index \"%s\" is another chunk log's",
						RelationGetRelationName(idx)),
				 errdetail("Its chunks are of another log, or of this log's storage before it was truncated.")));
	}
	{
		xl_chunk_index_bind_log xlrec = {.log = log, .stamp = stamp};
		LoggedPage	lp = {.buf = mbuf};

		START_CRIT_SECTION();
		meta->log = log;
		meta->log_stamp = stamp;
		log_index_change(idx, XLOG_CHUNK_INDEX_BIND_LOG, &xlrec, sizeof(xlrec), &lp, 1,
						 InvalidBuffer);
		END_CRIT_SECTION();
	}
	UnlockReleaseBuffer(mbuf);
}

/*
 * The chunks of bucket b (its first page primary, locked) as of snapshot,
 * those named hash alone if it is given, into *out (and *n, *cap); the chain
 * walked a page at a time, primary left locked
 */
static void
bucket_as_of(Relation idx, Buffer primary, uint32 b, const IndexCache *masks,
			 Snapshot snap, const uint8 *hash, ChunkIndexEntry **out, int *n,
			 int *cap)
{
	Buffer		cur = primary;

	for (;;)
	{
		Page		page = BufferGetPage(cur);
		BlockNumber next = ((IndexPageOpaque) PageGetSpecialPointer(page))->next;
		int			nentries = page_entries(page);

		for (int i = 0; i < nentries; i++)
		{
			ChunkIndexEntry *e = page_entry(page, i);

			if (!belongs(e, b, masks) ||
				(hash && memcmp(e->hash, hash, CHUNK_HASH_LEN) != 0) ||
				!chunk_as_of(e, cur, snap))
				continue;
			if (*n == *cap)
			{
				*cap = *cap ? *cap * 2 : 64;
				*out = *out ? repalloc(*out, *cap * sizeof(**out)) :
					palloc(*cap * sizeof(**out));
			}
			(*out)[(*n)++] = *e;
		}
		if (cur != primary)
			UnlockReleaseBuffer(cur);
		if (!BlockNumberIsValid(next))
			return;
		cur = ReadBuffer(idx, next);
		LockBuffer(cur, BUFFER_LOCK_SHARE);
		check_ipage(idx, next, BufferGetPage(cur), INDEX_PAGE_OVERFLOW);
	}
}

static bool
page_lookup_as_of(Relation idx, const uint8 *hash, Snapshot snap,
				  ChunkIndexEntry *out)
{
	uint32		bucket;
	IndexCache	masks;
	Buffer		primary = lock_bucket(idx, hash, BUFFER_LOCK_SHARE, &bucket, &masks);
	ChunkIndexEntry *found = NULL;
	int			n = 0,
				cap = 0;

	bucket_as_of(idx, primary, bucket, &masks, snap, hash, &found, &n, &cap);
	UnlockReleaseBuffer(primary);
	if (n > 0)
		*out = found[0];
	if (found)
		pfree(found);
	return n > 0;
}

static void
page_visit_as_of(Relation idx, Snapshot snap,
				 bool (*fn) (void *arg, const ChunkIndexEntry *e), void *arg)
{
	for (uint32 b = 0; b <= load_cache(idx)->maxbucket; b++)
	{
		IndexCache	masks = *(IndexCache *) idx->rd_amcache;
		BlockNumber blk = bucket_block(idx, &masks, b);
		Buffer		primary = ReadBuffer(idx, blk);
		ChunkIndexEntry *chunks = NULL;
		int			n = 0,
					cap = 0;
		bool		go_on = true;

		CHECK_FOR_INTERRUPTS();
		LockBuffer(primary, BUFFER_LOCK_SHARE);
		check_ipage(idx, blk, BufferGetPage(primary), INDEX_PAGE_BUCKET);
		if (((IndexPageOpaque) PageGetSpecialPointer(BufferGetPage(primary)))->split_at > masks.maxbucket)
			masks = *load_cache(idx);
		bucket_as_of(idx, primary, b, &masks, snap, NULL, &chunks, &n, &cap);
		UnlockReleaseBuffer(primary);

		/* (called with nothing locked) */
		for (int i = 0; i < n && go_on; i++)
			go_on = fn(arg, &chunks[i]);
		if (chunks)
			pfree(chunks);
		if (!go_on)
			return;
	}
}

/*
 * A chunk's entry moved (compaction): the entry seen, if it is the chunk's
 * still (its xmin and place), its committed changes folded in, held by no
 * transaction (no change running, not being freed) and not this one's, is
 * marked freed by this transaction, and a new entry of the chunk put, its
 * counts the old's, its place reserved by reserve (the bytes written after,
 * as a put's are).  To others the chunk is being freed and put until this
 * transaction ends, and they wait to see; once it commits the new entry is
 * the chunk's, and the old entry, freed, is taken out by VACUUM (its bytes
 * then dead) once no snapshot might be reading them.  False, and nothing
 * done, if the chunk is not to be moved.
 */
static bool
page_move(Relation idx, const ChunkIndexEntry *seen, ChunkIndexEntry *moved,
		  ChunkReserveFn reserve, void *arg)
{
	uint32		bucket;
	IndexCache	masks;
	Buffer		primary;
	Buffer		found;
	int			at;
	int64		drefs,
				droots;
	TransactionId running;
	TransactionId xid;
	ChunkIndexEntry e;
	BlockNumber space;
	BlockNumber last;
	bool		grew;
	xl_chunk_index_slot xlrec = {0};

	PreventCommandIfReadOnly("move a chunk");
	xid = GetCurrentTransactionId();
	primary = lock_bucket(idx, seen->hash, BUFFER_LOCK_EXCLUSIVE, &bucket, &masks);
	fold_changes(idx, primary, bucket, &masks);
	if (find_entry(idx, primary, BUFFER_LOCK_EXCLUSIVE, seen->hash, bucket, &masks,
				   false, &found, &at, NULL, NULL, NULL) != ENTRY_VISIBLE)
	{
		UnlockReleaseBuffer(primary);
		return false;
	}
	e = *page_entry(BufferGetPage(found), at);
	if (found != primary)
		LockBuffer(found, BUFFER_LOCK_UNLOCK);
	count_changes(idx, primary, BUFFER_LOCK_EXCLUSIVE, &e, bucket, &masks,
				  true, &drefs, &droots, &running, NULL, NULL, NULL, NULL);
	if (found != primary)
		LockBuffer(found, BUFFER_LOCK_EXCLUSIVE);
	if (e.xmin != seen->xmin || e.loc != seen->loc || e.len != seen->len ||
		TransactionIdIsValid(running) || drefs != 0 || droots != 0 ||
		ChunkEntryIsMine(&e))
	{
		release_found(primary, found);
		return false;
	}

	/* the old entry freed by this transaction */
	xlrec.slot = at;
	xlrec.xmax = xid;
	START_CRIT_SECTION();
	ipage_set_xmax(BufferGetPage(found), at, xid);
	log_slot_change(idx, primary, found, XLOG_CHUNK_INDEX_SET_XMAX, &xlrec);
	END_CRIT_SECTION();
	if (found != primary)
		UnlockReleaseBuffer(found);

	/* and the chunk's new one, its counts the old's */
	memset(moved, 0, sizeof(*moved));
	memcpy(moved->hash, e.hash, CHUNK_HASH_LEN);
	moved->len = e.len;
	moved->refs = e.refs;
	moved->roots = e.roots;
	moved->xmin = xid;
	moved->kind = CHUNK_KIND_CHUNK;
	chain_room(idx, primary, &space, &last);
	grew = add_entry(idx, primary, bucket, space, last, moved, reserve, arg);
	UnlockReleaseBuffer(primary);
	pgstat_count_heap_insert(idx, 1);
	pgstat_count_heap_delete(idx);
	if (grew)
		split_bucket(idx);
	return true;
}

/* ------------------------------------------------------------- the lanes */

/*
 * LaneChunkIndex: an index in lanes (prolly/chunk_lane.h).  A backend
 * writes its entries, chunks and changes of counts, into the index's lane
 * numbered as the lane of the index's log it holds, a relation it alone
 * extends; the index itself is written by its merger alone, which moves
 * the lanes' entries into it.  A chunk may have entries in several lanes
 * at once (two putting it at once, each into its own): its counts are its
 * entries' and their changes', wherever they are, the merger folding them
 * into one.
 *
 * A move is the merger's transaction's: the lane's entry marked moved by
 * it (merge_xid), its copy in the index tagged by it.  A lookup decides
 * that transaction by a snapshot of its own, taken as it starts: a lane's
 * entry counts unless the snapshot sees the move, the index's copy only if
 * it does, so that no entry is counted twice, nor missed, as the merger
 * moves it.  A chunk the merger freed is gone to a snapshot that sees the
 * freeing.  Inserts never wait: a chunk another transaction is putting is
 * this one's to put too.
 *
 * An index bound to no log, or to a log of no lanes (a tier), is written
 * itself, as PageChunkIndex is.
 */

/* whether the merger's transaction xid is done, to snap: committed in it, or this one */
static bool
merge_done(TransactionId xid, Snapshot snap)
{
	if (!TransactionIdIsValid(xid))
		return false;
	if (TransactionIdIsCurrentTransactionId(xid))
		return true;
	if (XidInMVCCSnapshot(xid, snap))
		return false;
	return TransactionIdDidCommit(xid);
}

/* what an entry's inserting transaction is to this one, alone */
static EntryState
xmin_state(ChunkIndexEntry *e, Buffer buf)
{
	if (!(e->flags & (CHUNK_ENTRY_FROZEN | CHUNK_ENTRY_COMMITTED)))
	{
		if (e->flags & CHUNK_ENTRY_ABORTED)
			return ENTRY_DEAD;
		if (!TransactionIdIsCurrentTransactionId(e->xmin))
		{
			if (TransactionIdIsInProgress(e->xmin))
				return ENTRY_IN_PROGRESS;
			if (!hinted_commit(e, buf, e->xmin, CHUNK_ENTRY_COMMITTED,
							   CHUNK_ENTRY_ABORTED))
				return ENTRY_DEAD;
		}
	}
	return ENTRY_VISIBLE;
}

/*
 * Whether an entry is counted, to snap: its insert visible, and moved out
 * of a lane by no merger snap sees, or moved into the index by one it does;
 * a chunk's, not freed by a transaction snap sees (*freeing set if one is
 * freeing it, running)
 */
static bool
lane_counts(ChunkIndexEntry *e, Buffer buf, bool lane, Snapshot snap,
			TransactionId *freeing)
{
	if (xmin_state(e, buf) != ENTRY_VISIBLE)
		return false;
	if (lane ? merge_done(e->merge_xid, snap) :
		(TransactionIdIsValid(e->merge_xid) && !merge_done(e->merge_xid, snap)))
		return false;
	if (e->kind == CHUNK_KIND_CHUNK && TransactionIdIsValid(e->xmax) &&
		!(e->flags & CHUNK_ENTRY_XMAX_ABORTED))
	{
		if (merge_done(e->xmax, snap))
			return false;
		if (freeing && TransactionIdIsInProgress(e->xmax))
			*freeing = e->xmax;
	}
	return true;
}

/* A chunk's entries and changes, wherever they are, as a lookup sees them. */
#define SCAN_INLINE 8

typedef struct LaneScan
{
	Snapshot	snap;
	ChunkIndexEntry inline_chunks[SCAN_INLINE];	/* (no allocation, mostly) */
	ChunkIndexEntry inline_changes[SCAN_INLINE];
	ChunkIndexEntry *chunks;
	int			nchunks,
				chunkcap;
	ChunkIndexEntry *changes;
	int			nchanges,
				changecap;
	TransactionId freeing;		/* a transaction freeing one, running, or 0 */
	TransactionId running;		/* another's entry or change of it, running */
} LaneScan;

static void
scan_push_grow(ChunkIndexEntry **v, int *n, int *cap, const ChunkIndexEntry *e,
			   ChunkIndexEntry *inl)
{
	if (*n == *cap)
	{
		ChunkIndexEntry *nv;

		*cap = *cap ? *cap * 2 : 8;
		if (*v == inl || *v == NULL)
		{
			nv = palloc_array(ChunkIndexEntry, *cap);
			if (*n > 0)
				memcpy(nv, *v, sizeof(ChunkIndexEntry) * *n);
		}
		else
			nv = repalloc_array(*v, ChunkIndexEntry, *cap);
		*v = nv;
	}
	(*v)[(*n)++] = *e;
}

static void
scan_push(ChunkIndexEntry **v, int *n, int *cap, const ChunkIndexEntry *e)
{
	scan_push_grow(v, n, cap, e, NULL);
}

/* hash's entries in rel (a lane, or the index), counted, into s */
static void
scan_rel(LaneScan *s, Relation rel, bool lane, const uint8 *hash)
{
	uint32		bucket;
	IndexCache	masks;
	Buffer		primary = lock_bucket(rel, hash, BUFFER_LOCK_SHARE, &bucket, &masks);
	Buffer		cur = primary;

	for (;;)
	{
		Page		page = BufferGetPage(cur);
		BlockNumber next = ((IndexPageOpaque) PageGetSpecialPointer(page))->next;
		int			n = page_entries(page);

		for (int i = 0; i < n; i++)
		{
			ChunkIndexEntry *e = page_entry(page, i);

			if (memcmp(e->hash, hash, CHUNK_HASH_LEN) != 0 || !belongs(e, bucket, &masks))
				continue;
			if (xmin_state(e, cur) == ENTRY_IN_PROGRESS)
			{
				s->running = e->xmin;
				continue;
			}
			if (!lane_counts(e, cur, lane, s->snap, &s->freeing))
				continue;
			if (e->kind == CHUNK_KIND_CHUNK)
				scan_push_grow(&s->chunks, &s->nchunks, &s->chunkcap, e, s->inline_chunks);
			else
				scan_push_grow(&s->changes, &s->nchanges, &s->changecap, e, s->inline_changes);
		}
		if (cur != primary)
			UnlockReleaseBuffer(cur);
		if (!BlockNumberIsValid(next))
			break;
		cur = ReadBuffer(rel, next);
		LockBuffer(cur, BUFFER_LOCK_SHARE);
		check_ipage(rel, next, BufferGetPage(cur), INDEX_PAGE_OVERFLOW);
	}
	UnlockReleaseBuffer(primary);
}

/*
 * hash's entries in every lane of idx and in idx, under a snapshot taken
 * now; the chunk in *out with its counts (its entries' and their
 * changes'), false if it has none
 */
static bool
lanes_scan(Relation idx, const uint8 *hash, LaneScan *s, ChunkIndexEntry *out)
{
	uint32		n = ChunkIndexLaneCount(idx);
	int64		refs = 0,
				roots = 0;

	memset(s, 0, sizeof(*s));
	s->chunks = s->inline_chunks;
	s->chunkcap = SCAN_INLINE;
	s->changes = s->inline_changes;
	s->changecap = SCAN_INLINE;
	s->snap = RegisterSnapshot(GetLatestSnapshot());
	for (uint32 l = 0; l < n; l++)
	{
		Relation	rel = ChunkIndexLaneOpen(idx, l, AccessShareLock);

		if (rel == NULL)
			continue;
		scan_rel(s, rel, true, hash);
		ChunkIndexLaneClose(rel);
	}
	scan_rel(s, idx, false, hash);
	UnregisterSnapshot(s->snap);
	s->snap = NULL;
	if (s->nchunks == 0)
		return false;
	for (int i = 0; i < s->nchunks; i++)
	{
		refs += s->chunks[i].refs;
		roots += s->chunks[i].roots;
		for (int j = 0; j < s->nchanges; j++)
			if (change_of(&s->changes[j], &s->chunks[i]))
			{
				refs += (int32) s->changes[j].refs;
				roots += (int32) s->changes[j].roots;
			}
	}
	if (refs < 0 || roots < 0 || refs > PG_UINT32_MAX || roots > PG_UINT32_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("a chunk's reference counts are out of range")));
	*out = s->chunks[0];
	out->refs = (uint32) refs;
	out->roots = (uint32) roots;
	return true;
}

static void
lanes_scan_free(LaneScan *s)
{
	if (s->chunks && s->chunks != s->inline_chunks)
		pfree(s->chunks);
	if (s->changes && s->changes != s->inline_changes)
		pfree(s->changes);
}

/* the log idx is bound to, opened, if it is one of lanes (a page log); else NULL */
static Relation
laned_log(Relation idx)
{
	uint64		stamp;
	Oid			logid = page_log_of(idx, &stamp);
	Relation	log;

	if (!OidIsValid(logid) || (log = try_relation_open(logid, AccessShareLock)) == NULL)
		return NULL;
	if (!RelationIsChunkLog(log))
	{
		relation_close(log, AccessShareLock);
		return NULL;
	}
	return log;
}

static bool
lane_lookup(Relation idx, const uint8 *hash, ChunkIndexEntry *out)
{
	Relation	log = laned_log(idx);
	LaneScan	s;
	bool		found;

	if (log == NULL)
		return page_lookup(idx, hash, out);
	relation_close(log, AccessShareLock);
	found = lanes_scan(idx, hash, &s, out);
	lanes_scan_free(&s);
	return found;
}

static bool
lane_insert(Relation idx, ChunkIndexEntry *entry, ChunkReserveFn reserve, void *arg)
{
	Relation	log = laned_log(idx);
	Relation	lane;
	LaneScan	s;
	ChunkIndexEntry have;
	uint32		n,
				bucket;
	IndexCache	masks;
	Buffer		primary;
	BlockNumber space,
				last;
	bool		grew;

	if (log == NULL)
		return page_insert(idx, entry, reserve, arg);
	PreventCommandIfReadOnly("insert into a chunk index");

	/* (the store's already: committed, or this transaction's) */
	if (lanes_scan(idx, entry->hash, &s, &have))
	{
		lanes_scan_free(&s);
		relation_close(log, AccessShareLock);
		*entry = have;
		return false;
	}
	lanes_scan_free(&s);

	lane = ChunkIndexLaneForWrite(idx, log, &n);
	entry->xmin = GetCurrentTransactionId();
	entry->xmax = InvalidTransactionId;
	entry->flags = 0;
	entry->kind = CHUNK_KIND_CHUNK;
	entry->merge_xid = InvalidTransactionId;
	entry->unused = 0;
	primary = lock_bucket(lane, entry->hash, BUFFER_LOCK_EXCLUSIVE, &bucket, &masks);
	chain_room(lane, primary, &space, &last);
	grew = add_entry(lane, primary, bucket, space, last, entry, reserve, arg);
	UnlockReleaseBuffer(primary);
	if (grew)
		split_bucket(lane);
	ChunkIndexLaneClose(lane);
	relation_close(log, AccessShareLock);

	/* (counted as the index's: autovacuum VACUUMs it, its merger) */
	pgstat_count_heap_insert(idx, 1);
	ChunkFreerChanged(idx, true);
	return true;
}

/*
 * This transaction's change, in rel's bucket for hash, of the chunk entry
 * chunk: its place, if its subtransaction's own (*own_blk, *own_at); and
 * whether any of the transaction's is there (*any_own)
 */
static void
own_change(Relation rel, Buffer primary, uint32 bucket, const IndexCache *masks,
		   const ChunkIndexEntry *chunk, BlockNumber *own_blk, int *own_at,
		   bool *any_own)
{
	TransactionId mine = GetCurrentTransactionIdIfAny();
	Buffer		cur = primary;

	*own_blk = InvalidBlockNumber;
	*any_own = false;
	for (;;)
	{
		Page		page = BufferGetPage(cur);
		BlockNumber next = ((IndexPageOpaque) PageGetSpecialPointer(page))->next;
		int			n = page_entries(page);

		for (int i = 0; i < n; i++)
		{
			ChunkIndexEntry *e = page_entry(page, i);

			if (!change_of(e, chunk) || !belongs(e, bucket, masks) ||
				!TransactionIdIsCurrentTransactionId(e->xmin))
				continue;
			*any_own = true;
			if (TransactionIdIsValid(mine) && e->xmin == mine)
			{
				*own_blk = BufferGetBlockNumber(cur);
				*own_at = i;
			}
		}
		if (cur != primary)
			UnlockReleaseBuffer(cur);
		if (!BlockNumberIsValid(next))
			return;
		cur = ReadBuffer(rel, next);
		LockBuffer(cur, BUFFER_LOCK_EXCLUSIVE);
		check_ipage(rel, next, BufferGetPage(cur), INDEX_PAGE_OVERFLOW);
	}
}

/*
 * This transaction's change of the chunk entry chunk's counts, written in
 * this backend's lane (its own change of it there changed, if it has one;
 * a hold not written again, if any of its changes holds it)
 */
static void
write_change(Relation idx, Relation log, const uint8 *hash, int32 drefs,
			 int32 droots, const ChunkIndexEntry *chunk)
{
	Relation	lane;
	uint32		n,
				bucket;
	IndexCache	masks;
	Buffer		primary;
	BlockNumber own_blk,
				space,
				last;
	int			own_at = 0;
	bool		any_own;

	lane = ChunkIndexLaneForWrite(idx, log, &n);
	primary = lock_bucket(lane, hash, BUFFER_LOCK_EXCLUSIVE, &bucket, &masks);
	own_change(lane, primary, bucket, &masks, chunk, &own_blk, &own_at, &any_own);
	if (any_own && drefs == 0 && droots == 0)
	{
		/* (a change of this transaction's holds the chunk already) */
	}
	else if (BlockNumberIsValid(own_blk))
	{
		Buffer		buf = own_blk == BufferGetBlockNumber(primary) ? primary :
			ReadBuffer(lane, own_blk);
		ChunkIndexEntry *e;
		xl_chunk_index_slot xlrec = {.slot = own_at};

		if (buf != primary)
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		e = page_entry(BufferGetPage(buf), own_at);
		xlrec.refs = (uint32) ((int32) e->refs + drefs);
		xlrec.roots = (uint32) ((int32) e->roots + droots);
		START_CRIT_SECTION();
		ipage_set_counts(BufferGetPage(buf), own_at, xlrec.refs, xlrec.roots);
		log_slot_change(lane, primary, buf, XLOG_CHUNK_INDEX_SET_COUNTS, &xlrec);
		END_CRIT_SECTION();
		if (buf != primary)
			UnlockReleaseBuffer(buf);
	}
	else
	{
		ChunkIndexEntry change;
		bool		grew;

		memset(&change, 0, sizeof(change));
		memcpy(change.hash, hash, CHUNK_HASH_LEN);
		change.refs = (uint32) drefs;
		change.roots = (uint32) droots;
		change.xmin = GetCurrentTransactionId();
		change.xmax = chunk->xmin;
		change.loc = chunk->loc;
		change.kind = CHUNK_KIND_CHANGE;
		chain_room(lane, primary, &space, &last);
		grew = add_entry(lane, primary, bucket, space, last, &change, NULL, NULL);
		if (grew)
		{
			UnlockReleaseBuffer(primary);
			split_bucket(lane);
			primary = InvalidBuffer;
		}
	}
	if (BufferIsValid(primary))
		UnlockReleaseBuffer(primary);
	ChunkIndexLaneClose(lane);
}

static bool
lane_adjust(Relation idx, const uint8 *hash, int32 drefs, int32 droots,
			ChunkIndexEntry *out)
{
	Relation	log = laned_log(idx);
	LaneScan	s;
	int64		refs,
				roots;

	if (log == NULL)
		return page_adjust(idx, hash, drefs, droots, out);
	PreventCommandIfReadOnly("change a chunk index");
	if (!lanes_scan(idx, hash, &s, out))
	{
		lanes_scan_free(&s);
		relation_close(log, AccessShareLock);
		return false;
	}
	lanes_scan_free(&s);
	refs = (int64) out->refs + drefs;
	roots = (int64) out->roots + droots;
	if (refs < 0 || roots < 0 || refs > PG_UINT32_MAX || roots > PG_UINT32_MAX)
	{
		relation_close(log, AccessShareLock);
		ereport(ERROR,
				(errcode(refs < 0 || roots < 0 ? ERRCODE_INVALID_PARAMETER_VALUE :
						 ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("a chunk's reference counts would be %lld and %lld",
						(long long) refs, (long long) roots)));
	}

	/*
	 * (the change written, in this backend's lane, naming the entry found;
	 * then the chunk looked for again: should a transaction be freeing or
	 * moving it, the change is one its look before committing finds (and
	 * takes it back for), or else that is seen here and waited for; moved,
	 * the change is written again, of the entry it has now, the one before
	 * of an entry freed, counting nothing)
	 */
	for (;;)
	{
		ChunkIndexEntry again;
		bool		there;
		TransactionId freeing;

		write_change(idx, log, hash, drefs, droots, out);
		for (;;)
		{
			there = lanes_scan(idx, hash, &s, &again);
			freeing = s.freeing;
			lanes_scan_free(&s);
			if (!there || !TransactionIdIsValid(freeing))
				break;
			XactLockTableWait(freeing, NULL, NULL, XLTW_None);
		}
		if (!there)
		{
			relation_close(log, AccessShareLock);
			return false;		/* (freed: gone) */
		}
		if (again.xmin == out->xmin && again.loc == out->loc)
			break;
		*out = again;			/* (moved meanwhile: of its new entry) */
	}
	relation_close(log, AccessShareLock);
	pgstat_count_heap_insert(idx, 1);
	out->refs = (uint32) refs;
	out->roots = (uint32) roots;
	if (drefs < 0 || droots < 0)
		ChunkFreerChanged(idx, true);
	return true;
}

/*
 * Each entry of rel counted (lane_counts, or as of snap if as_of), page by
 * page, every bucket's chain: fn called with each, nothing locked between
 * pages (an entry a split copied may come twice), until it answers false
 */
static bool
walk_rel(Relation rel, bool lane, Snapshot snap, bool as_of,
		 bool (*fn) (void *arg, const ChunkIndexEntry *e), void *arg)
{
	IndexCache *masks = load_cache(rel);
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);
	ChunkIndexEntry *batch = palloc_array(ChunkIndexEntry, PER_PAGE);

	for (BlockNumber blk = 1; blk < nblocks; blk++)
	{
		Buffer		buf = ReadBuffer(rel, blk);
		Page		page;
		IndexPageOpaque opaque;
		int			n,
					nb = 0;

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		opaque = (IndexPageOpaque) PageGetSpecialPointer(page);
		if (PageIsNew(page) ||
			(opaque->kind != INDEX_PAGE_BUCKET && opaque->kind != INDEX_PAGE_OVERFLOW))
		{
			UnlockReleaseBuffer(buf);
			continue;
		}
		n = page_entries(page);
		for (int i = 0; i < n; i++)
		{
			ChunkIndexEntry *e = page_entry(page, i);

			if (!belongs(e, opaque->bucket, masks))
				continue;
			if (as_of)
			{
				if (!chunk_as_of(e, buf, snap) ||
					(lane ? merge_done(e->merge_xid, snap) :
					 (TransactionIdIsValid(e->merge_xid) && !merge_done(e->merge_xid, snap))))
					continue;
			}
			else if (!lane_counts(e, buf, lane, snap, NULL))
				continue;
			batch[nb++] = *e;
		}
		UnlockReleaseBuffer(buf);
		for (int i = 0; i < nb; i++)
			if (!fn(arg, &batch[i]))
			{
				pfree(batch);
				return false;
			}
	}
	pfree(batch);
	return true;
}

/* every relation of idx walked: its lanes, then itself */
static bool
walk_lanes(Relation idx, Snapshot snap, bool as_of,
		   bool (*fn) (void *arg, const ChunkIndexEntry *e), void *arg)
{
	uint32		n = ChunkIndexLaneCount(idx);

	for (uint32 l = 0; l < n; l++)
	{
		Relation	rel = ChunkIndexLaneOpen(idx, l, AccessShareLock);
		bool		go;

		if (rel == NULL)
			continue;
		go = walk_rel(rel, true, snap, as_of, fn, arg);
		ChunkIndexLaneClose(rel);
		if (!go)
			return false;
	}
	return walk_rel(idx, false, snap, as_of, fn, arg);
}

/* the chunks a visit gathers, by hash: their entries' counts and changes' */
typedef struct VisitChunk
{
	uint8		hash[CHUNK_HASH_LEN];	/* the key */
	bool		present;
	ChunkIndexEntry chunk;		/* the first entry */
	int64		refs,
				roots;
	ChunkIndexEntry *ids;		/* each entry's (its changes named by it) */
	int			nids,
				idcap;
	ChunkIndexEntry *changes;
	int			nchanges,
				changecap;
} VisitChunk;

static bool
gather(void *arg, const ChunkIndexEntry *e)
{
	HTAB	   *chunks = arg;
	bool		found;
	VisitChunk *v = hash_search(chunks, e->hash, HASH_ENTER, &found);

	if (!found)
	{
		memset((char *) v + CHUNK_HASH_LEN, 0, sizeof(*v) - CHUNK_HASH_LEN);
	}
	if (e->kind == CHUNK_KIND_CHUNK)
	{
		if (!v->present)
			v->chunk = *e;
		v->present = true;
		v->refs += e->refs;
		v->roots += e->roots;
		scan_push(&v->ids, &v->nids, &v->idcap, e);
	}
	else
		scan_push(&v->changes, &v->nchanges, &v->changecap, e);
	return true;
}

static void
lane_visit(Relation idx, void (*fn) (void *arg, const ChunkIndexEntry *e), void *arg)
{
	Relation	log = laned_log(idx);
	Snapshot	snap;
	HASHCTL		ctl;
	HTAB	   *chunks;
	HASH_SEQ_STATUS seq;
	VisitChunk *v;

	if (log == NULL)
	{
		page_visit(idx, fn, arg);
		return;
	}
	relation_close(log, AccessShareLock);
	ctl.keysize = CHUNK_HASH_LEN;
	ctl.entrysize = sizeof(VisitChunk);
	ctl.hcxt = CurrentMemoryContext;
	chunks = hash_create("chunk index visit", 1024, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	snap = RegisterSnapshot(GetLatestSnapshot());
	(void) walk_lanes(idx, snap, false, gather, chunks);
	UnregisterSnapshot(snap);
	hash_seq_init(&seq, chunks);
	while ((v = hash_seq_search(&seq)) != NULL)
	{
		ChunkIndexEntry e;

		if (!v->present)
			continue;
		for (int i = 0; i < v->nids; i++)
			for (int j = 0; j < v->nchanges; j++)
				if (change_of(&v->changes[j], &v->ids[i]))
				{
					v->refs += (int32) v->changes[j].refs;
					v->roots += (int32) v->changes[j].roots;
				}
		e = v->chunk;
		if (v->refs < 0 || v->roots < 0 || v->refs > PG_UINT32_MAX || v->roots > PG_UINT32_MAX)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("a chunk's reference counts are out of range")));
		e.refs = (uint32) v->refs;
		e.roots = (uint32) v->roots;
		fn(arg, &e);
	}
	hash_destroy(chunks);
}

/* hash's chunk as of snap, in rel: the first entry there that was the store's */
static bool
as_of_rel(Relation rel, bool lane, const uint8 *hash, Snapshot snap, ChunkIndexEntry *out)
{
	uint32		bucket;
	IndexCache	masks;
	Buffer		primary = lock_bucket(rel, hash, BUFFER_LOCK_SHARE, &bucket, &masks);
	Buffer		cur = primary;
	bool		found = false;

	for (;;)
	{
		Page		page = BufferGetPage(cur);
		BlockNumber next = ((IndexPageOpaque) PageGetSpecialPointer(page))->next;
		int			n = page_entries(page);

		for (int i = 0; i < n && !found; i++)
		{
			ChunkIndexEntry *e = page_entry(page, i);

			if (memcmp(e->hash, hash, CHUNK_HASH_LEN) != 0 || !belongs(e, bucket, &masks) ||
				!chunk_as_of(e, cur, snap) ||
				(lane ? merge_done(e->merge_xid, snap) :
				 (TransactionIdIsValid(e->merge_xid) && !merge_done(e->merge_xid, snap))))
				continue;
			*out = *e;
			found = true;
		}
		if (cur != primary)
			UnlockReleaseBuffer(cur);
		if (found || !BlockNumberIsValid(next))
			break;
		cur = ReadBuffer(rel, next);
		LockBuffer(cur, BUFFER_LOCK_SHARE);
		check_ipage(rel, next, BufferGetPage(cur), INDEX_PAGE_OVERFLOW);
	}
	UnlockReleaseBuffer(primary);
	return found;
}

static bool
lane_lookup_as_of(Relation idx, const uint8 *hash, Snapshot snap, ChunkIndexEntry *out)
{
	Relation	log = laned_log(idx);
	uint32		n;

	if (log == NULL)
		return page_lookup_as_of(idx, hash, snap, out);
	relation_close(log, AccessShareLock);
	n = ChunkIndexLaneCount(idx);
	for (uint32 l = 0; l < n; l++)
	{
		Relation	rel = ChunkIndexLaneOpen(idx, l, AccessShareLock);
		bool		found;

		if (rel == NULL)
			continue;
		found = as_of_rel(rel, true, hash, snap, out);
		ChunkIndexLaneClose(rel);
		if (found)
			return true;
	}
	return as_of_rel(idx, false, hash, snap, out);
}

/* (a visit as of a snapshot calls fn with chunks only) */
typedef struct AsOfVisit
{
	bool		(*fn) (void *arg, const ChunkIndexEntry *e);
	void	   *arg;
} AsOfVisit;

static bool
as_of_chunk(void *arg, const ChunkIndexEntry *e)
{
	AsOfVisit  *v = arg;

	return e->kind != CHUNK_KIND_CHUNK || v->fn(v->arg, e);
}

static void
lane_visit_as_of(Relation idx, Snapshot snap,
				 bool (*fn) (void *arg, const ChunkIndexEntry *e), void *arg)
{
	Relation	log = laned_log(idx);
	AsOfVisit	v = {.fn = fn, .arg = arg};

	if (log == NULL)
	{
		page_visit_as_of(idx, snap, fn, arg);
		return;
	}
	relation_close(log, AccessShareLock);
	(void) walk_lanes(idx, snap, true, as_of_chunk, &v);
}

static void
lane_bind_log(Relation idx, Oid log, uint64 stamp)
{
	uint32		n;

	page_bind_log(idx, log, stamp);
	n = ChunkIndexLaneCount(idx);
	for (uint32 l = 0; l < n; l++)
	{
		Relation	rel = ChunkIndexLaneOpen(idx, l, RowExclusiveLock);

		if (rel == NULL)
			continue;
		page_bind_log(rel, log, stamp);
		ChunkIndexLaneClose(rel);
	}
}

static bool
lane_remove(Relation idx, const uint8 *hash)
{
	uint32		n = ChunkIndexLaneCount(idx);
	bool		any = page_remove(idx, hash);

	for (uint32 l = 0; l < n; l++)
	{
		Relation	rel = ChunkIndexLaneOpen(idx, l, RowExclusiveLock);

		if (rel == NULL)
			continue;
		any = page_remove(rel, hash) || any;
		ChunkIndexLaneClose(rel);
	}
	return any;
}


/* ------------------------------------------------------------ the merger */

/*
 * The entries of hash in idx itself as this transaction (the merger) sees
 * them: the chunk's entries present (its first in *chunk; false if none),
 * and its counts, entries' and changes', summed.
 */
static bool
main_counts(Relation idx, const uint8 *hash, ChunkIndexEntry *chunk, int64 *refs,
			int64 *roots)
{
	LaneScan	s;

	memset(&s, 0, sizeof(s));
	s.snap = RegisterSnapshot(GetLatestSnapshot());
	scan_rel(&s, idx, false, hash);
	UnregisterSnapshot(s.snap);
	*refs = *roots = 0;
	for (int i = 0; i < s.nchunks; i++)
	{
		*refs += s.chunks[i].refs;
		*roots += s.chunks[i].roots;
		for (int j = 0; j < s.nchanges; j++)
			if (change_of(&s.changes[j], &s.chunks[i]))
			{
				*refs += (int32) s.changes[j].refs;
				*roots += (int32) s.changes[j].roots;
			}
	}
	if (s.nchunks > 0)
	{
		/* (the first not an alias: the chunk's own) */
		*chunk = s.chunks[0];
		for (int i = 0; i < s.nchunks; i++)
			if (!(s.chunks[i].flags & CHUNK_ENTRY_ALIAS))
			{
				*chunk = s.chunks[i];
				break;
			}
	}
	lanes_scan_free(&s);
	return s.nchunks > 0;
}

/* an entry added to idx itself, as an insert adds one (the bucket split if it grew) */
static void
main_add(Relation idx, ChunkIndexEntry *e)
{
	uint32		bucket;
	IndexCache	masks;
	Buffer		primary = lock_bucket(idx, e->hash, BUFFER_LOCK_EXCLUSIVE, &bucket, &masks);
	BlockNumber space,
				last;
	bool		grew;

	chain_room(idx, primary, &space, &last);
	grew = add_entry(idx, primary, bucket, space, last, e, NULL, NULL);
	UnlockReleaseBuffer(primary);
	if (grew)
		split_bucket(idx);
}

/*
 * A lane's entry moved into idx by this transaction: a change, copied; a
 * chunk's, copied as the chunk's entry if idx has none of it present, else
 * as an alias of the one there (the same chunk put twice at once: its own
 * counts a change of that one, its changes counted through it); tagged by
 * this transaction.  Whether its counts left the chunk none (*zero).
 */
static void
merge_into(Relation idx, const ChunkIndexEntry *e, TransactionId xid, bool *zero)
{
	ChunkIndexEntry copy = *e;
	ChunkIndexEntry chunk;
	int64		refs,
				roots;

	copy.merge_xid = xid;
	copy.flags &= ~(CHUNK_ENTRY_XMAX_COMMITTED | CHUNK_ENTRY_XMAX_ABORTED);
	if (e->kind == CHUNK_KIND_CHUNK && main_counts(idx, e->hash, &chunk, &refs, &roots))
	{
		ChunkIndexEntry change;

		copy.flags |= CHUNK_ENTRY_ALIAS;
		copy.refs = copy.roots = 0;
		main_add(idx, &copy);
		if (e->refs != 0 || e->roots != 0)
		{
			memset(&change, 0, sizeof(change));
			memcpy(change.hash, e->hash, CHUNK_HASH_LEN);
			change.kind = CHUNK_KIND_CHANGE;
			change.refs = e->refs;
			change.roots = e->roots;
			change.xmin = xid;
			change.xmax = chunk.xmin;
			change.loc = chunk.loc;
			change.flags = 0;
			change.merge_xid = xid;
			main_add(idx, &change);
		}
	}
	else
		main_add(idx, &copy);
	(void) main_counts(idx, e->hash, &chunk, &refs, &roots);
	*zero = refs == 0 && roots == 0;
}

/*
 * The entries of lane committed, and not moved yet (or moved by a merger
 * that aborted), moved into idx by this transaction: each marked moved in
 * the lane, bucket by bucket, its chain locked from its first page, as
 * VACUUM walks it (a bucket split meanwhile read again, so that no copy a
 * split made is moved twice), then copied into idx.  The buckets of idx of
 * chunks left with no counts marked, for the freer.  How many moved.
 */
static uint64
merge_lane(Relation idx, Relation lane)
{
	TransactionId xid = GetCurrentTransactionId();
	IndexCache *cache = load_cache(lane);
	ChunkIndexEntry *moved = NULL;
	int			cap = 0;
	uint64		total = 0;

	for (uint32 b = 0; b <= cache->maxbucket; b++)
	{
		BlockNumber blk = bucket_block(lane, cache, b);
		Buffer		primary = ReadBuffer(lane, blk);
		Buffer		cur;
		IndexPageOpaque opaque;
		IndexCache	masks;
		int			nm = 0;

		LockBuffer(primary, BUFFER_LOCK_EXCLUSIVE);
		opaque = check_ipage(lane, blk, BufferGetPage(primary), INDEX_PAGE_BUCKET);
		if (opaque->split_at > cache->maxbucket)
		{
			UnlockReleaseBuffer(primary);
			cache = load_cache(lane);
			b--;
			continue;
		}
		masks = *cache;
		cur = primary;
		for (;;)
		{
			Page		page = BufferGetPage(cur);
			BlockNumber next = ((IndexPageOpaque) PageGetSpecialPointer(page))->next;
			int			n = page_entries(page);

			for (int i = 0; i < n; i++)
			{
				ChunkIndexEntry *e = page_entry(page, i);
				xl_chunk_index_slot xlrec = {.slot = i, .xmax = xid};

				if (!belongs(e, b, &masks) || xmin_state(e, cur) != ENTRY_VISIBLE ||
					TransactionIdIsCurrentTransactionId(e->xmin))
					continue;
				if (TransactionIdIsValid(e->merge_xid) &&
					(TransactionIdIsCurrentTransactionId(e->merge_xid) ||
					 TransactionIdIsInProgress(e->merge_xid) ||
					 TransactionIdDidCommit(e->merge_xid)))
					continue;
				/* (freed, or being freed: VACUUM takes it out of the lane) */
				if (e->kind == CHUNK_KIND_CHUNK && TransactionIdIsValid(e->xmax) &&
					!(e->flags & CHUNK_ENTRY_XMAX_ABORTED) &&
					(TransactionIdIsInProgress(e->xmax) || TransactionIdDidCommit(e->xmax)))
					continue;
				START_CRIT_SECTION();
				ipage_set_merge(page, i, xid);
				log_slot_change(lane, primary, cur, XLOG_CHUNK_INDEX_SET_MERGE, &xlrec);
				END_CRIT_SECTION();
				if (nm == cap)
				{
					cap = cap ? cap * 2 : 64;
					moved = moved ? repalloc_array(moved, ChunkIndexEntry, cap) :
						palloc_array(ChunkIndexEntry, cap);
				}
				moved[nm++] = *e;
			}
			if (cur != primary)
				UnlockReleaseBuffer(cur);
			if (!BlockNumberIsValid(next))
				break;
			cur = ReadBuffer(lane, next);
			LockBuffer(cur, BUFFER_LOCK_EXCLUSIVE);
			check_ipage(lane, next, BufferGetPage(cur), INDEX_PAGE_OVERFLOW);
		}
		UnlockReleaseBuffer(primary);
		for (int k = 0; k < nm; k++)
		{
			bool		zero;

			CHECK_FOR_INTERRUPTS();
			merge_into(idx, &moved[k], xid, &zero);
			if (zero)
			{
				uint32		ib;
				IndexCache	im;
				Buffer		ip = lock_bucket(idx, moved[k].hash, BUFFER_LOCK_SHARE, &ib, &im);

				UnlockReleaseBuffer(ip);
				set_mark(idx, ib, true);
			}
		}
		total += nm;
	}
	if (moved)
		pfree(moved);
	return total;
}

static uint64
lane_merge(Relation idx)
{
	Relation	log = laned_log(idx);
	uint32		n;
	uint64		total = 0;

	if (log == NULL)
		return 0;
	relation_close(log, AccessShareLock);
	PreventCommandIfReadOnly("merge a chunk index's lanes");
	n = ChunkIndexLaneCount(idx);
	for (uint32 l = 0; l < n; l++)
	{
		Relation	lane = ChunkIndexLaneOpen(idx, l, RowExclusiveLock);

		if (lane == NULL)
			continue;
		total += merge_lane(idx, lane);
		ChunkIndexLaneClose(lane);
	}
	if (total > 0)
		pgstat_count_heap_insert(idx, (PgStat_Counter) total);
	return total;
}

/*
 * xmax set on every entry of hash's chunk in rel (a lane, or the index)
 * present to this transaction, or (xid 0) cleared on those it set it on
 */
static void
rel_set_xmax(Relation rel, bool lane, const uint8 *hash, TransactionId xid, Snapshot snap)
{
	uint32		bucket;
	IndexCache	masks;
	Buffer		primary = lock_bucket(rel, hash, BUFFER_LOCK_EXCLUSIVE, &bucket, &masks);
	Buffer		cur = primary;

	for (;;)
	{
		Page		page = BufferGetPage(cur);
		BlockNumber next = ((IndexPageOpaque) PageGetSpecialPointer(page))->next;
		int			n = page_entries(page);

		for (int i = 0; i < n; i++)
		{
			ChunkIndexEntry *e = page_entry(page, i);
			xl_chunk_index_slot xlrec = {.slot = i, .xmax = xid};

			if (e->kind != CHUNK_KIND_CHUNK || memcmp(e->hash, hash, CHUNK_HASH_LEN) != 0 ||
				!belongs(e, bucket, &masks))
				continue;
			if (TransactionIdIsValid(xid) ? !lane_counts(e, cur, lane, snap, NULL) :
				!(TransactionIdIsValid(e->xmax) && TransactionIdIsCurrentTransactionId(e->xmax)))
				continue;
			START_CRIT_SECTION();
			ipage_set_xmax(page, i, xid);
			log_slot_change(rel, primary, cur, XLOG_CHUNK_INDEX_SET_XMAX, &xlrec);
			END_CRIT_SECTION();
		}
		if (cur != primary)
			UnlockReleaseBuffer(cur);
		if (!BlockNumberIsValid(next))
			break;
		cur = ReadBuffer(rel, next);
		LockBuffer(cur, BUFFER_LOCK_EXCLUSIVE);
		check_ipage(rel, next, BufferGetPage(cur), INDEX_PAGE_OVERFLOW);
	}
	UnlockReleaseBuffer(primary);
}

/* rel_set_xmax on every lane of idx and on idx */
static void
lanes_set_xmax(Relation idx, const uint8 *hash, TransactionId xid)
{
	uint32		n = ChunkIndexLaneCount(idx);
	Snapshot	snap = RegisterSnapshot(GetLatestSnapshot());

	for (uint32 l = 0; l < n; l++)
	{
		Relation	rel = ChunkIndexLaneOpen(idx, l, RowExclusiveLock);

		if (rel == NULL)
			continue;
		rel_set_xmax(rel, true, hash, xid, snap);
		ChunkIndexLaneClose(rel);
	}
	rel_set_xmax(idx, false, hash, xid, snap);
	UnregisterSnapshot(snap);
}

/*
 * A chunk freed, if nothing holds it: its counts zero, wherever its entries
 * and changes are; no other transaction's entry or change of it running;
 * none of its entries this transaction's own put; and held false.  Each
 * entry of it present marked freed (changing pages, never adding one: a
 * lane is extended by its writer alone); then looked for again: an entry
 * or change of it another made meanwhile (a transaction holding it as it
 * is freed, whose own look then finds the freeing running and waits for
 * it) takes the freeing back.
 */
static bool
lane_free(Relation idx, const uint8 *hash, bool (*held) (Relation idx, const uint8 *hash),
		  ChunkIndexEntry *out)
{
	Relation	log = laned_log(idx);
	LaneScan	s;
	bool		there,
				mine = false;

	if (log == NULL)
		return page_free(idx, hash, held, out);
	relation_close(log, AccessShareLock);
	PreventCommandIfReadOnly("free a chunk");
	there = lanes_scan(idx, hash, &s, out);
	for (int i = 0; there && i < s.nchunks; i++)
		mine = mine || ChunkEntryIsMine(&s.chunks[i]);
	if (!there || out->refs != 0 || out->roots != 0 || TransactionIdIsValid(s.running) ||
		TransactionIdIsValid(s.freeing) || mine || (held && held(idx, hash)))
	{
		lanes_scan_free(&s);
		return false;
	}
	lanes_scan_free(&s);
	lanes_set_xmax(idx, hash, GetCurrentTransactionId());
	{
		ChunkIndexEntry again;

		there = lanes_scan(idx, hash, &s, &again);
		if (there || TransactionIdIsValid(s.running))
		{
			lanes_scan_free(&s);
			lanes_set_xmax(idx, hash, InvalidTransactionId);
			return false;
		}
		lanes_scan_free(&s);
	}
	pgstat_count_heap_delete(idx);
	out->refs = 0;
	out->roots = 0;
	return true;
}

/*
 * Whether a change of the chunk entry chunk is in any lane of idx or in
 * idx (any transaction's, running or committed, not this one's)
 */
static bool
changes_of(Relation idx, const ChunkIndexEntry *chunk)
{
	uint32		n = ChunkIndexLaneCount(idx);

	for (uint32 l = 0; l <= n; l++)
	{
		Relation	rel = l < n ? ChunkIndexLaneOpen(idx, l, AccessShareLock) : idx;
		uint32		bucket;
		IndexCache	masks;
		Buffer		primary,
					cur;
		bool		have = false;

		if (rel == NULL)
			continue;
		primary = lock_bucket(rel, chunk->hash, BUFFER_LOCK_SHARE, &bucket, &masks);
		cur = primary;
		for (;;)
		{
			Page		page = BufferGetPage(cur);
			BlockNumber next = ((IndexPageOpaque) PageGetSpecialPointer(page))->next;
			int			np = page_entries(page);

			for (int i = 0; i < np && !have; i++)
			{
				ChunkIndexEntry *e = page_entry(page, i);

				if (change_of(e, chunk) && belongs(e, bucket, &masks) &&
					!TransactionIdIsCurrentTransactionId(e->xmin) &&
					xmin_state(e, cur) != ENTRY_DEAD)
					have = true;
			}
			if (cur != primary)
				UnlockReleaseBuffer(cur);
			if (have || !BlockNumberIsValid(next))
				break;
			cur = ReadBuffer(rel, next);
			LockBuffer(cur, BUFFER_LOCK_SHARE);
			check_ipage(rel, next, BufferGetPage(cur), INDEX_PAGE_OVERFLOW);
		}
		UnlockReleaseBuffer(primary);
		if (rel != idx)
			ChunkIndexLaneClose(rel);
		if (have)
			return true;
	}
	return false;
}

/*
 * A chunk's entry moved, for compaction: only a chunk all of whose counts
 * are in its one entry in idx itself (no alias, no change of it anywhere,
 * none running), the one seen.  Its entry marked freed; then every lane
 * looked in again for a change of it another made meanwhile (which the
 * new entry would leave counting nothing): one there takes the freeing
 * back.  Else the new entry, its counts the old's, its place reserve's.
 */
static bool
lane_move(Relation idx, const ChunkIndexEntry *seen, ChunkIndexEntry *moved,
		  ChunkReserveFn reserve, void *arg)
{
	Relation	log = laned_log(idx);
	LaneScan	s;
	ChunkIndexEntry e;
	bool		there;
	xl_chunk_index_slot xlrec = {0};
	uint32		bucket;
	IndexCache	masks;
	Buffer		primary,
				cur;
	BlockNumber space,
				last;
	bool		set = false;
	bool		grew;
	TransactionId xid;

	if (log == NULL)
		return page_move(idx, seen, moved, reserve, arg);
	relation_close(log, AccessShareLock);
	PreventCommandIfReadOnly("move a chunk");
	there = lanes_scan(idx, seen->hash, &s, &e);
	if (!there || s.nchunks != 1 || s.nchanges != 0 || TransactionIdIsValid(s.running) ||
		TransactionIdIsValid(s.freeing) || TransactionIdIsValid(s.chunks[0].merge_xid) ||
		s.chunks[0].xmin != seen->xmin || s.chunks[0].loc != seen->loc ||
		s.chunks[0].len != seen->len || ChunkEntryIsMine(&s.chunks[0]))
	{
		lanes_scan_free(&s);
		return false;
	}
	lanes_scan_free(&s);

	/* (in idx itself: a lane's entry has its own place, and is the merger's) */
	xid = GetCurrentTransactionId();
	primary = lock_bucket(idx, seen->hash, BUFFER_LOCK_EXCLUSIVE, &bucket, &masks);
	cur = primary;
	for (;;)
	{
		Page		page = BufferGetPage(cur);
		BlockNumber next = ((IndexPageOpaque) PageGetSpecialPointer(page))->next;
		int			n = page_entries(page);

		for (int i = 0; i < n && !set; i++)
		{
			ChunkIndexEntry *x = page_entry(page, i);

			if (x->kind != CHUNK_KIND_CHUNK || memcmp(x->hash, seen->hash, CHUNK_HASH_LEN) != 0 ||
				!belongs(x, bucket, &masks) || x->xmin != seen->xmin || x->loc != seen->loc ||
				TransactionIdIsValid(x->xmax))
				continue;
			xlrec.slot = i;
			xlrec.xmax = xid;
			START_CRIT_SECTION();
			ipage_set_xmax(page, i, xid);
			log_slot_change(idx, primary, cur, XLOG_CHUNK_INDEX_SET_XMAX, &xlrec);
			END_CRIT_SECTION();
			set = true;
		}
		if (cur != primary)
			UnlockReleaseBuffer(cur);
		if (set || !BlockNumberIsValid(next))
			break;
		cur = ReadBuffer(idx, next);
		LockBuffer(cur, BUFFER_LOCK_EXCLUSIVE);
		check_ipage(idx, next, BufferGetPage(cur), INDEX_PAGE_OVERFLOW);
	}
	UnlockReleaseBuffer(primary);
	if (!set)
		return false;
	if (changes_of(idx, &e))
	{
		Snapshot	snap = RegisterSnapshot(GetLatestSnapshot());

		rel_set_xmax(idx, false, seen->hash, InvalidTransactionId, snap);
		UnregisterSnapshot(snap);
		return false;
	}

	memset(moved, 0, sizeof(*moved));
	memcpy(moved->hash, e.hash, CHUNK_HASH_LEN);
	moved->len = e.len;
	moved->refs = e.refs;
	moved->roots = e.roots;
	moved->xmin = xid;
	moved->kind = CHUNK_KIND_CHUNK;
	primary = lock_bucket(idx, seen->hash, BUFFER_LOCK_EXCLUSIVE, &bucket, &masks);
	chain_room(idx, primary, &space, &last);
	grew = add_entry(idx, primary, bucket, space, last, moved, reserve, arg);
	UnlockReleaseBuffer(primary);
	pgstat_count_heap_insert(idx, 1);
	pgstat_count_heap_delete(idx);
	if (grew)
		split_bucket(idx);
	return true;
}

const ChunkIndexRoutine PageChunkIndex = {
	.lookup = page_lookup,
	.insert = page_insert,
	.adjust = page_adjust,
	.free = page_free,
	.log_of = page_log_of,
	.bind_log = page_bind_log,
	.remove = page_remove,
	.visit = page_visit,
	.visit_bucket = page_visit_bucket,
	.marked = page_marked,
	.unmark = page_unmark,
	.lookup_as_of = page_lookup_as_of,
	.visit_as_of = page_visit_as_of,
	.move = page_move,
	.merge = NULL,
};

/*
 * (the freer's marks, and compaction's moves, are of the index itself:
 * chunks are freed and moved once the merger has moved them in)
 */
const ChunkIndexRoutine LaneChunkIndex = {
	.lookup = lane_lookup,
	.insert = lane_insert,
	.adjust = lane_adjust,
	.free = lane_free,
	.log_of = page_log_of,
	.bind_log = lane_bind_log,
	.remove = lane_remove,
	.visit = lane_visit,
	.visit_bucket = page_visit_bucket,
	.marked = page_marked,
	.unmark = page_unmark,
	.lookup_as_of = lane_lookup_as_of,
	.visit_as_of = lane_visit_as_of,
	.move = lane_move,
	.merge = lane_merge,
};

const ChunkIndexRoutine *ChunkIndex = &LaneChunkIndex;

/* ------------------------------------------------------- storage */

void
ChunkIndexInitStorage(SMgrRelation srel, bool use_wal)
{
	ChunkIndexInitStorageAs(srel, use_wal, 0, InvalidOid);
}

void
ChunkIndexInitStorageAs(SMgrRelation srel, bool use_wal, uint32 lane, Oid parent)
{
	BulkWriteState *bulk = smgr_bulk_start_smgr(srel, MAIN_FORKNUM, use_wal);
	BulkWriteBuffer buf;
	Page		page;
	IndexMetaData *meta;
	BlockNumber *dir;

	/* block 0: the meta page; 1: the directory; 2..: the first buckets */
	buf = smgr_bulk_get_buf(bulk);
	page = (Page) buf;
	init_ipage(page, INDEX_PAGE_META, 0, 0);
	meta = (IndexMetaData *) PageGetContents(page);
	meta->version = CHUNK_INDEX_VERSION;
	meta->maxbucket = INIT_BUCKETS - 1;
	meta->lowmask = INIT_BUCKETS - 1;
	meta->highmask = 2 * INIT_BUCKETS - 1;
	meta->next_block = 2 + INIT_BUCKETS;
	meta->log = InvalidOid;
	meta->log_stamp = 0;
	meta->lane = lane;
	meta->parent = parent;
	meta->nmarks = 0;
	meta->ndir = 1;
	meta->dir[0] = 1;
	set_meta_lower(page, meta);
	smgr_bulk_write(bulk, META_BLOCK, buf, true);

	buf = smgr_bulk_get_buf(bulk);
	page = (Page) buf;
	init_ipage(page, INDEX_PAGE_DIR, 0, 0);
	dir = (BlockNumber *) PageGetContents(page);
	for (uint32 b = 0; b < INIT_BUCKETS; b++)
		dir[b] = 2 + b;
	((PageHeader) page)->pd_lower = SizeOfPageHeaderData +
		INIT_BUCKETS * sizeof(BlockNumber);
	smgr_bulk_write(bulk, 1, buf, true);

	for (uint32 b = 0; b < INIT_BUCKETS; b++)
	{
		buf = smgr_bulk_get_buf(bulk);
		init_ipage((Page) buf, INDEX_PAGE_BUCKET, b, INIT_BUCKETS - 1);
		smgr_bulk_write(bulk, 2 + b, buf, true);
	}
	smgr_bulk_finish(bulk);
}

/*
 * entry_state without hints: what VACUUM rewrites is decided from the flags
 * as they were (a hint set meanwhile would hide an entry not yet frozen)
 */
static EntryState
entry_state_unhinted(const ChunkIndexEntry *e)
{
	if (!(e->flags & (CHUNK_ENTRY_FROZEN | CHUNK_ENTRY_COMMITTED)))
	{
		if (e->flags & CHUNK_ENTRY_ABORTED)
			return ENTRY_DEAD;
		if (!TransactionIdIsCurrentTransactionId(e->xmin))
		{
			if (TransactionIdIsInProgress(e->xmin))
				return ENTRY_IN_PROGRESS;
			if (!TransactionIdDidCommit(e->xmin))
				return ENTRY_DEAD;
		}
	}
	if (e->kind != CHUNK_KIND_CHUNK || !TransactionIdIsValid(e->xmax) ||
		(e->flags & CHUNK_ENTRY_XMAX_ABORTED))
		return ENTRY_VISIBLE;
	if ((e->flags & CHUNK_ENTRY_XMAX_COMMITTED) ||
		TransactionIdIsCurrentTransactionId(e->xmax))
		return ENTRY_DEAD;
	if (TransactionIdIsInProgress(e->xmax))
		return ENTRY_IN_PROGRESS;
	return TransactionIdDidCommit(e->xmax) ? ENTRY_DEAD : ENTRY_VISIBLE;
}

/* The page and slot of the first entry on the chain that match wants. */
typedef bool (*EntryTest) (const ChunkIndexEntry *e, const void *arg);

static bool
chain_find(Relation idx, Buffer primary, EntryTest wants, const void *arg,
		   BlockNumber *blk, int *at, ChunkIndexEntry *copy)
{
	Buffer		cur = primary;

	for (;;)
	{
		Page		page = BufferGetPage(cur);
		BlockNumber next = ((IndexPageOpaque) PageGetSpecialPointer(page))->next;
		int			n = page_entries(page);

		for (int i = 0; i < n; i++)
			if (wants(page_entry(page, i), arg))
			{
				*blk = BufferGetBlockNumber(cur);
				*at = i;
				*copy = *page_entry(page, i);
				if (cur != primary)
					UnlockReleaseBuffer(cur);
				return true;
			}
		if (cur != primary)
			UnlockReleaseBuffer(cur);
		if (!BlockNumberIsValid(next))
			return false;
		cur = ReadBuffer(idx, next);
		LockBuffer(cur, BUFFER_LOCK_EXCLUSIVE);
		check_ipage(idx, next, BufferGetPage(cur), INDEX_PAGE_OVERFLOW);
	}
}

/*
 * (an entry a merger moved into the index: folded, or folded into, only
 * once every snapshot sees the move, lest a snapshot not seeing it lose
 * what the fold adds in: VACUUM's horizon, as it folds)
 */
static TransactionId fold_horizon = InvalidTransactionId;

static bool
merge_seen_by_all(const ChunkIndexEntry *e)
{
	return !TransactionIdIsValid(e->merge_xid) ||
		(TransactionIdIsValid(fold_horizon) &&
		 TransactionIdPrecedes(e->merge_xid, fold_horizon) &&
		 TransactionIdDidCommit(e->merge_xid));
}

/* a committed change: of increases (arg true), or of any */
static bool
committed_change(const ChunkIndexEntry *e, const void *arg)
{
	return e->kind == CHUNK_KIND_CHANGE &&
		(!*(const bool *) arg || ((int32) e->refs >= 0 && (int32) e->roots >= 0)) &&
		entry_state_unhinted(e) == ENTRY_VISIBLE && merge_seen_by_all(e);
}

/* the entry (not freed) the change arg is of */
static bool
chunk_of(const ChunkIndexEntry *e, const void *arg)
{
	return e->kind == CHUNK_KIND_CHUNK &&
		change_of(arg, e) && entry_state_unhinted(e) != ENTRY_DEAD &&
		merge_seen_by_all(e);
}

/*
 * Adds each committed change of a chunk's counts on the bucket's chain into
 * the chunk's entry, and takes it out, both in one record: a crash leaves
 * a change in or out, never in twice.  (The bucket's first page, primary,
 * locked exclusively, keeps everyone else off its chain.)
 */
static void
fold_changes(Relation idx, Buffer primary, uint32 bucket, const IndexCache *masks)
{
	BlockNumber cblk,
				eblk;
	int			cat,
				eat;
	ChunkIndexEntry change,
				chunk;
	bool		increases = true;

	/*
	 * Increases first: each decrease was made against counts with the
	 * increases committed before it, so none takes a count below zero
	 */
	for (;;)
	{
		Buffer		cbuf,
					ebuf;
		ChunkIndexEntry e;
		xl_chunk_index_fold xlrec = {0};
		LoggedPage	lps[2];

		if (!chain_find(idx, primary, committed_change, &increases, &cblk, &cat,
						&change))
		{
			if (!increases)
				break;
			increases = false;
			continue;
		}

		if (!chain_find(idx, primary, chunk_of, &change, &eblk, &eat, &chunk))
		{
			/* (a change of a chunk freed: out with it) */
			Buffer		buf = cblk == BufferGetBlockNumber(primary) ? primary :
				ReadBuffer(idx, cblk);

			if (buf != primary)
				LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
			remove_slot(idx, primary, buf, cat);
			if (buf != primary)
				UnlockReleaseBuffer(buf);
			continue;
		}
		cbuf = cblk == BufferGetBlockNumber(primary) ? primary : ReadBuffer(idx, cblk);
		ebuf = eblk == BufferGetBlockNumber(primary) ? primary :
			eblk == cblk ? cbuf : ReadBuffer(idx, eblk);
		if (cbuf != primary)
			LockBuffer(cbuf, BUFFER_LOCK_EXCLUSIVE);
		if (ebuf != primary && ebuf != cbuf)
			LockBuffer(ebuf, BUFFER_LOCK_EXCLUSIVE);
		e = *page_entry(BufferGetPage(ebuf), eat);
		if (!counts_add(&e, (int32) change.refs, (int32) change.roots))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("a chunk's reference counts are out of range")));
		xlrec.chunk_slot = eat;
		xlrec.change_slot = cat;
		xlrec.refs = e.refs;
		xlrec.roots = e.roots;
		xlrec.same_page = ebuf == cbuf;
		xlrec.chunk_on_bucket_page = ebuf == primary;
		lps[0] = (LoggedPage) {.buf = ebuf};
		lps[1] = (LoggedPage) {.buf = cbuf};
		START_CRIT_SECTION();
		ipage_set_counts(BufferGetPage(ebuf), eat, e.refs, e.roots);

		/* (out by the last entry moved into its slot: the chunk's may be it) */
		ipage_remove_slot(BufferGetPage(cbuf), cat);
		log_index_change(idx, XLOG_CHUNK_INDEX_FOLD, &xlrec, sizeof(xlrec),
						 lps, xlrec.same_page ? 1 : 2, primary);
		END_CRIT_SECTION();
		if (ebuf != primary && ebuf != cbuf)
			UnlockReleaseBuffer(ebuf);
		if (cbuf != primary)
			UnlockReleaseBuffer(cbuf);
		CHECK_FOR_INTERRUPTS();
	}
}

/* What VACUUM does with an entry, by what it knows of xmin and xmax. */
typedef enum VacuumFate
{
	FATE_KEEP = 0,
	FATE_REMOVE = 1,			/* aborted: its bytes dead */
	FATE_REMOVE_FREED = 2,		/* freed: its bytes dead */
	FATE_REMOVE_COPY = 3,		/* not the bucket's: another's bytes */
	FATE_FREEZE = 4,			/* xmin committed, not noted so in WAL */
	FATE_CLEAR = 5,				/* xmax aborted: no freeing now */
	FATE_FREEZE_CLEAR = 6,
} VacuumFate;

/* (VACUUM's records have a byte of each: their values are WAL's) */
StaticAssertDecl(FATE_FREEZE_CLEAR <= PG_UINT8_MAX, "a fate fits a byte");

static VacuumFate vacuum_fate_entry(const ChunkIndexEntry *e, TransactionId frozen);

/*
 * VACUUM's fates of a page's entries, made: those taken out out, the rest
 * frozen or their freeing cleared as their fates say; the entries kept
 */
static int
ipage_vacuum(Page page, const uint8 *fates, int n)
{
	int			k = 0;

	for (int i = 0; i < n; i++)
	{
		ChunkIndexEntry e = *page_entry(page, i);

		if (fates[i] == FATE_REMOVE || fates[i] == FATE_REMOVE_FREED ||
			fates[i] == FATE_REMOVE_COPY)
			continue;
		if (fates[i] == FATE_FREEZE || fates[i] == FATE_FREEZE_CLEAR)
			e.flags |= CHUNK_ENTRY_FROZEN | CHUNK_ENTRY_COMMITTED;
		if (fates[i] == FATE_CLEAR || fates[i] == FATE_FREEZE_CLEAR)
		{
			/* (a freeing that aborted, gone with it) */
			e.flags &= ~(CHUNK_ENTRY_XMAX_COMMITTED | CHUNK_ENTRY_XMAX_ABORTED);
			e.xmax = InvalidTransactionId;
		}
		*page_entry(page, k++) = e;
	}
	set_page_entries(page, k);
	return k;
}

/*
 * Each transaction id an entry names on its own: xmin, committed and before
 * the horizon (frozen: the oldest any snapshot running might not see), is
 * frozen whatever xmax is (a freeing running leaves it, being past the
 * horizon); xmax, committed, makes the entry dead, aborted, gone
 */
static VacuumFate
vacuum_fate(const ChunkIndexEntry *e, uint32 bucket, const IndexCache *masks,
			TransactionId frozen, bool lane)
{
	if (!belongs(e, bucket, masks))
		return FATE_REMOVE_COPY;

	/*
	 * (a lane's entry moved into the index: out once every snapshot sees
	 * the move, its bytes the copy's; the index's copy of a move aborted:
	 * out, its bytes the lane's entry's still; of one running (this
	 * VACUUM's own, perhaps), not taken out, but frozen as any entry is,
	 * its insert being committed: the relation's relfrozenxid passes it)
	 */
	if (TransactionIdIsValid(e->merge_xid) && !lane)
	{
		bool		running = TransactionIdIsCurrentTransactionId(e->merge_xid) ||
			TransactionIdIsInProgress(e->merge_xid);

		if (running)
		{
			VacuumFate	f = vacuum_fate_entry(e, frozen);

			return f == FATE_REMOVE || f == FATE_REMOVE_FREED ? FATE_KEEP : f;
		}
		if (!TransactionIdDidCommit(e->merge_xid))
			return FATE_REMOVE_COPY;
	}
	else if (TransactionIdIsValid(e->merge_xid) &&
			 !TransactionIdIsCurrentTransactionId(e->merge_xid) &&
			 !TransactionIdIsInProgress(e->merge_xid) &&
			 TransactionIdDidCommit(e->merge_xid) &&
			 TransactionIdPrecedes(e->merge_xid, frozen))
		return FATE_REMOVE_COPY;
	return vacuum_fate_entry(e, frozen);
}

/* an entry's fate by its xmin and xmax alone */
static VacuumFate
vacuum_fate_entry(const ChunkIndexEntry *e, TransactionId frozen)
{
	bool		freeze;
	bool		clear = false;

	if (!(e->flags & (CHUNK_ENTRY_FROZEN | CHUNK_ENTRY_COMMITTED)))
	{
		if (e->flags & CHUNK_ENTRY_ABORTED)
			return FATE_REMOVE;
		if (TransactionIdIsInProgress(e->xmin))
			return FATE_KEEP;
		if (!TransactionIdDidCommit(e->xmin))
			return FATE_REMOVE;
	}
	if (e->kind != CHUNK_KIND_CHUNK)
		return FATE_KEEP;		/* (a change is added in, not frozen) */

	/*
	 * (frozen once no snapshot running might not see it: frozen is seen by
	 * every snapshot, as of which a dump reads the store, and the relation's
	 * relfrozenxid is the same horizon)
	 */
	freeze = !(e->flags & CHUNK_ENTRY_FROZEN) && TransactionIdPrecedes(e->xmin, frozen);
	if (TransactionIdIsValid(e->xmax))
	{
		if (e->flags & CHUNK_ENTRY_XMAX_COMMITTED)
			return TransactionIdPrecedes(e->xmax, frozen) ? FATE_REMOVE_FREED :
				(freeze ? FATE_FREEZE : FATE_KEEP);
		if (e->flags & CHUNK_ENTRY_XMAX_ABORTED)
			clear = true;
		else if (!TransactionIdIsInProgress(e->xmax))
		{
			/*
			 * (freed: out once no transaction running could have found it
			 * before the freeing, and be reading its bytes, which then go
			 * to others)
			 */
			if (TransactionIdDidCommit(e->xmax))
				return TransactionIdPrecedes(e->xmax, frozen) ? FATE_REMOVE_FREED :
					(freeze ? FATE_FREEZE : FATE_KEEP);
			clear = true;
		}
	}
	return freeze ? (clear ? FATE_FREEZE_CLEAR : FATE_FREEZE) :
		(clear ? FATE_CLEAR : FATE_KEEP);
}

/*
 * The chunks on the bucket's chain nothing holds: counts zero, no change
 * of them left on the chain (VACUUM has added the committed ones in), not
 * being freed.  (A count, for whether to start a freer, which looks again.)
 */
static uint64
count_unheld(Relation idx, Buffer primary)
{
	HTAB	   *changed;
	HASHCTL		ctl;
	ChunkIndexEntry *zero = NULL;
	int			nzero = 0,
				cap = 0;
	uint64		n = 0;
	Buffer		cur = primary;

	ctl.keysize = CHUNK_HASH_LEN;
	ctl.entrysize = CHUNK_HASH_LEN;
	ctl.hcxt = CurrentMemoryContext;
	changed = hash_create("chunks changed", 64, &ctl,
						  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	for (;;)
	{
		Page		page = BufferGetPage(cur);
		BlockNumber next = ((IndexPageOpaque) PageGetSpecialPointer(page))->next;
		int			ne = page_entries(page);

		for (int i = 0; i < ne; i++)
		{
			ChunkIndexEntry *e = page_entry(page, i);

			if (e->kind == CHUNK_KIND_CHANGE)
				(void) hash_search(changed, e->hash, HASH_ENTER, NULL);
			else if (e->refs == 0 && e->roots == 0 &&
					 !TransactionIdIsValid(e->xmax) &&
					 entry_state_unhinted(e) == ENTRY_VISIBLE)
			{
				if (nzero == cap)
				{
					cap = cap ? cap * 2 : 64;
					zero = zero ? repalloc(zero, cap * sizeof(*zero)) :
						palloc(cap * sizeof(*zero));
				}
				zero[nzero++] = *e;
			}
		}
		if (cur != primary)
			UnlockReleaseBuffer(cur);
		if (!BlockNumberIsValid(next))
			break;
		cur = ReadBuffer(idx, next);
		LockBuffer(cur, BUFFER_LOCK_SHARE);
		check_ipage(idx, next, BufferGetPage(cur), INDEX_PAGE_OVERFLOW);
	}
	for (int i = 0; i < nzero; i++)
		if (hash_search(changed, zero[i].hash, HASH_FIND, NULL) == NULL)
			n++;
	hash_destroy(changed);
	if (zero)
		pfree(zero);
	return n;
}

/* What a VACUUM of an index and its lanes carries from one relation to the next. */
typedef struct VacState
{
	TransactionId frozen;
	double		kept_entries;
	Relation	log;
	bool		log_tried;
	bool		any_pass;
	void	   *sweep;
	uint64		unheld;
	uint64	   *pending_locs;
	uint32	   *pending_lens;
	int			npending,
				pendcap;
	bool		pending_wanted;
	ChunkIndexEntry *dead;
	int			ndead,
				dead_cap;
	TransactionId oldest;		/* the oldest transaction a kept entry of the
								 * relation passed still names, or 0 */
} VacState;

/* (xid noted, if older than the oldest yet) */
static inline void
note_oldest(VacState *vs, TransactionId xid)
{
	if (TransactionIdIsNormal(xid) &&
		(!TransactionIdIsValid(vs->oldest) || TransactionIdPrecedes(xid, vs->oldest)))
		vs->oldest = xid;
}

/* a relation's relfrozenxid: the horizon, unless a kept entry names an older transaction */
static inline TransactionId
pass_frozen(const VacState *vs)
{
	return TransactionIdIsValid(vs->oldest) && TransactionIdPrecedes(vs->oldest, vs->frozen) ?
		vs->oldest : vs->frozen;
}

/*
 * VACUUM's pass of rel, the index or (lane) one of its lanes: each bucket's
 * entries taken out, frozen or cleared as their fates say, their bytes
 * counted dead; the index's changes folded in and its buckets of chunks
 * nothing holds marked (a lane's are not: its entries are the merger's to
 * move in, its changes counted where they are)
 */
static void
vacuum_pass(Relation rel, bool lane, VacState *vs)
{
	for (uint32 b = 0; b <= load_cache(rel)->maxbucket; b++)
	{
		IndexCache	masks = *(IndexCache *) rel->rd_amcache;
		BlockNumber blk = bucket_block(rel, &masks, b);
		Buffer		primary;
		Buffer		cur;

		vacuum_delay_point(false);

		/*
		 * The log the entries' bytes are in, opened (its lock waited for)
		 * before the bucket's entries are taken out and their bytes counted
		 * dead, not interrupted between: a cancel there would leave them
		 * counted by nobody.  An index bound to no log yet has no bytes; one
		 * a first put binds meanwhile keeps the entries of chunks with bytes
		 * for the next VACUUM.
		 */
		if (!vs->log_tried)
		{
			uint64		stamp;

			if (OidIsValid(page_log_of(rel, &stamp)))
			{
				vs->log = open_bound_log(rel);
				vs->log_tried = true;
				if (vs->log != NULL)
				{
					vs->pending_wanted = true;
				}

				/*
				 * (the pages of regions held by none as the pass begins, to
				 * let go of after it if unchanged: the log's vacuum_begin)
				 */
				if (vs->log != NULL && !vs->any_pass)
					vs->sweep = ChunkLogOf(vs->log)->vacuum_begin(vs->log);
			}
		}
		vs->any_pass = true;
		HOLD_INTERRUPTS();
		primary = ReadBuffer(rel, blk);
		LockBuffer(primary, BUFFER_LOCK_EXCLUSIVE);
		check_ipage(rel, blk, BufferGetPage(primary), INDEX_PAGE_BUCKET);
		if (((IndexPageOpaque) PageGetSpecialPointer(BufferGetPage(primary)))->split_at > masks.maxbucket)
			masks = *load_cache(rel);
		cur = primary;
		for (;;)
		{
			Page		page = BufferGetPage(cur);
			BlockNumber next = ((IndexPageOpaque) PageGetSpecialPointer(page))->next;
			int			n = page_entries(page);
			uint8		fates[PER_PAGE];
			bool		change = false;
			TransactionId horizon = InvalidTransactionId;

			for (int i = 0; i < n; i++)
			{
				fates[i] = vacuum_fate(page_entry(page, i), b, &masks, vs->frozen, lane);
				if (vs->log == NULL &&
					(fates[i] == FATE_REMOVE || fates[i] == FATE_REMOVE_FREED) &&
					page_entry(page, i)->kind == CHUNK_KIND_CHUNK &&
					page_entry(page, i)->len > 0)
					fates[i] = FATE_KEEP;
				if (fates[i] != FATE_KEEP)
					change = true;

				/*
				 * (a chunk's entry freed, or being freed, kept: its bytes
				 * dead only later, once no snapshot might read them)
				 */
				if ((fates[i] == FATE_KEEP || fates[i] == FATE_FREEZE) &&
					vs->pending_wanted &&
					page_entry(page, i)->kind == CHUNK_KIND_CHUNK &&
					page_entry(page, i)->len > 0 &&
					TransactionIdIsValid(page_entry(page, i)->xmax) &&
					!(page_entry(page, i)->flags & CHUNK_ENTRY_XMAX_ABORTED))
				{
					ChunkIndexEntry *pe = page_entry(page, i);

					if (vs->npending == vs->pendcap)
					{
						vs->pendcap = Max(vs->pendcap * 2, 64);
						vs->pending_locs = vs->pending_locs ?
							repalloc_array(vs->pending_locs, uint64, vs->pendcap) :
							palloc_array(uint64, vs->pendcap);
						vs->pending_lens = vs->pending_lens ?
							repalloc_array(vs->pending_lens, uint32, vs->pendcap) :
							palloc_array(uint32, vs->pendcap);
					}
					vs->pending_locs[vs->npending] = pe->loc;
					vs->pending_lens[vs->npending++] = pe->len;
				}
				if (fates[i] == FATE_REMOVE_FREED &&
					(!TransactionIdIsValid(horizon) ||
					 TransactionIdFollows(page_entry(page, i)->xmax, horizon)))
					horizon = page_entry(page, i)->xmax;

				/*
				 * (an entry frozen is seen by every snapshot: a hot
				 * standby's query whose snapshot did not see its insert
				 * conflicts with its freezing, as with heap's)
				 */
				if ((fates[i] == FATE_FREEZE || fates[i] == FATE_FREEZE_CLEAR) &&
					(!TransactionIdIsValid(horizon) ||
					 TransactionIdFollows(page_entry(page, i)->xmin, horizon)))
					horizon = page_entry(page, i)->xmin;
			}
			/*
			 * (what a kept entry still names: its insert, unless frozen; its
			 * freeing; and its move, cleared once every snapshot sees it, or
			 * it aborted, so that nothing asks the commit log of it after)
			 */
			for (int i = 0; i < n; i++)
			{
				ChunkIndexEntry *e = page_entry(page, i);
				bool		kept = fates[i] == FATE_KEEP || fates[i] == FATE_CLEAR ||
					fates[i] == FATE_FREEZE || fates[i] == FATE_FREEZE_CLEAR;

				if (!kept)
					continue;
				if (!(e->flags & CHUNK_ENTRY_FROZEN) &&
					fates[i] != FATE_FREEZE && fates[i] != FATE_FREEZE_CLEAR)
					note_oldest(vs, e->xmin);
				if (TransactionIdIsValid(e->xmax) &&
					fates[i] != FATE_CLEAR && fates[i] != FATE_FREEZE_CLEAR)
					note_oldest(vs, e->xmax);
				if (TransactionIdIsValid(e->merge_xid) &&
					!TransactionIdIsCurrentTransactionId(e->merge_xid) &&
					!TransactionIdIsInProgress(e->merge_xid) &&
					(!TransactionIdDidCommit(e->merge_xid) ||
					 TransactionIdPrecedes(e->merge_xid, vs->frozen)))
				{
					xl_chunk_index_slot xlrec = {.slot = i, .xmax = InvalidTransactionId};

					START_CRIT_SECTION();
					ipage_set_merge(page, i, InvalidTransactionId);
					log_slot_change(rel, primary, cur, XLOG_CHUNK_INDEX_SET_MERGE, &xlrec);
					END_CRIT_SECTION();
				}
				else if (TransactionIdIsValid(e->merge_xid))
					note_oldest(vs, e->merge_xid);
			}
			if (change)
			{
				/*
				 * (the latest freeing of a chunk taken out: a hot standby's
				 * queries whose snapshots might have found one conflict with
				 * the record, there being no knowing on the primary which
				 * chunks they are reading)
				 */
				xl_chunk_index_vacuum xlrec = {
					.snapshotConflictHorizon = horizon,
					.nentries = n,
				};
				LoggedPage	lp = {.buf = cur, .data = fates, .len = n};

				/* (a chunk's bytes, dead once its entry is out) */
				for (int i = 0; i < n; i++)
				{
					ChunkIndexEntry *e = page_entry(page, i);

					if ((fates[i] == FATE_REMOVE || fates[i] == FATE_REMOVE_FREED) &&
						e->kind == CHUNK_KIND_CHUNK && e->len > 0)
					{
						if (vs->ndead == vs->dead_cap)
						{
							vs->dead_cap = vs->dead_cap ? vs->dead_cap * 2 : 64;
							vs->dead = vs->dead ? repalloc(vs->dead, vs->dead_cap * sizeof(*vs->dead)) :
								palloc(vs->dead_cap * sizeof(*vs->dead));
						}
						vs->dead[vs->ndead++] = *e;
					}
				}
				START_CRIT_SECTION();
				vs->kept_entries += ipage_vacuum(page, fates, n);
				log_index_change(rel, XLOG_CHUNK_INDEX_VACUUM, &xlrec, sizeof(xlrec), &lp, 1,
								 primary);
				END_CRIT_SECTION();
			}
			else
				vs->kept_entries += n;
			if (cur != primary)
				UnlockReleaseBuffer(cur);
			if (!BlockNumberIsValid(next))
				break;
			CHECK_FOR_INTERRUPTS();
			cur = ReadBuffer(rel, next);
			LockBuffer(cur, BUFFER_LOCK_EXCLUSIVE);
			check_ipage(rel, next, BufferGetPage(cur), INDEX_PAGE_OVERFLOW);
		}
		if (!lane)
			fold_changes(rel, primary, b, &masks);
		/* (chunks nothing holds: the bucket marked, for the freer) */
		if (!lane && count_unheld(rel, primary) > 0)
		{
			vs->unheld++;
			set_mark(rel, b, true);
		}
		UnlockReleaseBuffer(primary);

		/* the bytes of the chunks taken out, dead in their log */
		if (vs->ndead > 0 && vs->log != NULL)
			for (int i = 0; i < vs->ndead; i++)
				ChunkLogOf(vs->log)->mark_dead(vs->log, vs->dead[i].loc, vs->dead[i].len, vs->dead[i].log_gen);
		vs->ndead = 0;
		RESUME_INTERRUPTS();
	}
}

void
ChunkIndexVacuum(Relation idx, bool truncate)
{
	uint32		first_marked;
	TransactionId frozen = GetOldestNonRemovableTransactionId(idx);
	TimestampTz starttime = GetCurrentTimestamp();
	VacState	vs = {.frozen = frozen};
	TransactionId main_frozen;
	double		main_kept;
	uint32		lane_no;
	Oid			lane_parent;
	bool		lane_of = ChunkIndexLaneOf(idx, &lane_no, &lane_parent) &&
		OidIsValid(lane_parent);
	ChunkLogSegments segs = {0};

	/*
	 * Every transaction older than frozen has ended: past its bucket's
	 * pages, each of its entries is out or frozen (WAL-logged), and none
	 * needs the commit log again.  Buckets split meanwhile are walked too
	 * (the highest bucket is read again after each): a split moves entries,
	 * flags and all, from a bucket to a higher one, and an entry inserted
	 * meanwhile is of a transaction past frozen.
	 */
	/* (the lanes' entries moved in first: what the pass sees, and frees) */
	if (!lane_of && ChunkIndex->merge)
		(void) ChunkIndex->merge(idx);
	fold_horizon = frozen;
	vs.oldest = InvalidTransactionId;
	vacuum_pass(idx, lane_of, &vs);
	main_frozen = pass_frozen(&vs);
	main_kept = vs.kept_entries;
	if (!lane_of)
	{
		uint32		nl = ChunkIndexLaneCount(idx);

		for (uint32 l = 0; l < nl; l++)
		{
			Relation	lane = ChunkIndexLaneOpen(idx, l, ShareUpdateExclusiveLock);
			double		before = vs.kept_entries;

			if (lane == NULL)
				continue;
			vs.oldest = InvalidTransactionId;
			vacuum_pass(lane, true, &vs);
			vac_update_relstats(lane, RelationGetNumberOfBlocks(lane), vs.kept_entries - before,
								0, 0, false, pass_frozen(&vs), InvalidMultiXactId, NULL, NULL, false);
			ChunkIndexLaneClose(lane);
		}
	}
	fold_horizon = InvalidTransactionId;
	if (!vs.log_tried)
		vs.log = open_bound_log(idx);
	if (vs.log != NULL)
	{
		/*
		 * The log's own end of VACUUM: the regions of backends gone, or that
		 * forgot them, let go of; the free end cut (unless VACUUM is not to
		 * truncate, or the log's own vacuum_truncate is off: the cut's
		 * AccessExclusiveLock, WAL-logged, conflicts with a hot standby's
		 * readers of the log); then what it finds of its segments, those with
		 * chunks freed kept for snapshots passed by
		 */
		ChunkLogOf(vs.log)->vacuum_end(vs.log, vs.sweep,
									truncate &&
									!(vs.log->rd_options != NULL &&
									  ((StdRdOptions *) vs.log->rd_options)->vacuum_truncate == PG_TERNARY_FALSE),
									chunk_compact_dead_fraction, vs.npending, vs.pending_locs,
									vs.pending_lens, &segs);
		if (vs.pending_locs)
		{
			pfree(vs.pending_locs);
			pfree(vs.pending_lens);
		}

		/*
		 * chunks nothing holds, segments to compact, or the end to evacuate:
		 * a freer frees them, and what they held, compacts, and evacuates
		 */
		if (vs.unheld > 0 || page_marked(idx, 0, 1, &first_marked) > 0 ||
			(segs.compactable > 0 && segs.worth) || segs.evacuable > 0)
		{
			ereport(DEBUG1,
					(errmsg_internal("chunk index \"%s\": a freer to start: %llu buckets of chunks unheld, %u segments to compact%s, %u to evacuate, %u passed by for snapshots",
									 RelationGetRelationName(idx),
									 (unsigned long long) vs.unheld, segs.compactable,
									 segs.worth ? "" : " (not worth it)",
									 segs.evacuable, segs.pending)));
			(void) ChunkFreerLaunch(RelationGetRelid(vs.log), RelationGetRelid(idx), false,
									segs.compactable > 0 && segs.worth,
									segs.evacuable > 0,
									chunk_compact_dead_fraction, segs.grow, 0);
		}
		relation_close(vs.log, NoLock);
	}

	vac_update_relstats(idx, RelationGetNumberOfBlocks(idx), main_kept, 0, 0,
						false, main_frozen, InvalidMultiXactId, NULL, NULL, false);

	/* (as a heap's VACUUM does: autovacuum counts inserts from here) */
	pgstat_report_vacuum(idx, (PgStat_Counter) main_kept, 0, starttime);
}

static uint64
rel_unfrozen(Relation idx)
{
	uint32		nbuckets = load_cache(idx)->maxbucket + 1;
	uint64		unfrozen = 0;

	for (uint32 b = 0; b < nbuckets; b++)
	{
		BlockNumber blk = bucket_block(idx, load_cache(idx), b);

		while (BlockNumberIsValid(blk))
		{
			Buffer		buf = ReadBuffer(idx, blk);
			Page		page;
			int			n;

			LockBuffer(buf, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buf);
			n = page_entries(page);
			for (int i = 0; i < n; i++)
				if (page_entry(page, i)->kind != CHUNK_KIND_CHUNK ||
					!(page_entry(page, i)->flags & CHUNK_ENTRY_FROZEN))
					unfrozen++;
			blk = ((IndexPageOpaque) PageGetSpecialPointer(page))->next;
			UnlockReleaseBuffer(buf);
		}
	}
	return unfrozen;
}

static void
rel_shape(Relation idx, uint32 *buckets, uint32 *pages, uint64 *entries)
{
	IndexCache *cache = load_cache(idx);
	uint32		nbuckets = cache->maxbucket + 1;

	*buckets = nbuckets;
	*pages = RelationGetNumberOfBlocks(idx);
	*entries = 0;

	/* every entry on a bucket's pages, its own or not */
	for (uint32 b = 0; b < nbuckets; b++)
	{
		BlockNumber blk = bucket_block(idx, load_cache(idx), b);

		while (BlockNumberIsValid(blk))
		{
			Buffer		buf = ReadBuffer(idx, blk);
			Page		page;

			LockBuffer(buf, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buf);
			*entries += page_entries(page);
			blk = ((IndexPageOpaque) PageGetSpecialPointer(page))->next;
			UnlockReleaseBuffer(buf);
		}
	}
}

/* (for tests: an index's lanes counted with it, the index as a whole) */
uint64
ChunkIndexUnfrozen(Relation idx)
{
	uint64		n = rel_unfrozen(idx);
	uint32		nl = ChunkIndexLaneCount(idx);

	for (uint32 l = 0; l < nl; l++)
	{
		Relation	lane = ChunkIndexLaneOpen(idx, l, AccessShareLock);

		if (lane == NULL)
			continue;
		n += rel_unfrozen(lane);
		ChunkIndexLaneClose(lane);
	}
	return n;
}

void
ChunkIndexShape(Relation idx, uint32 *buckets, uint32 *pages, uint64 *entries)
{
	uint32		nl = ChunkIndexLaneCount(idx);

	rel_shape(idx, buckets, pages, entries);
	for (uint32 l = 0; l < nl; l++)
	{
		Relation	lane = ChunkIndexLaneOpen(idx, l, AccessShareLock);
		uint32		b,
					p;
		uint64		e;

		if (lane == NULL)
			continue;
		rel_shape(lane, &b, &p, &e);
		*buckets += b;
		*pages += p;
		*entries += e;
		ChunkIndexLaneClose(lane);
	}
}

/* ------------------------------------------------------------- redo */

/*
 * A record's pages, locked as the change was made: the bucket's first page
 * (if the record has it unchanged) first, then the others in the order the
 * primary locked them, each held until all are changed, so that a hot
 * standby's reader sees the change whole, as one on the primary does.
 */
typedef struct RedoPages
{
	XLogReaderState *record;
	Buffer		bufs[BUCKET_BLOCK + 1];
	bool		redo[BUCKET_BLOCK + 1];
} RedoPages;

static void
redo_begin(RedoPages *rp, XLogReaderState *record)
{
	rp->record = record;
	for (int i = 0; i <= BUCKET_BLOCK; i++)
	{
		rp->bufs[i] = InvalidBuffer;
		rp->redo[i] = false;
	}
	if (XLogRecHasBlockRef(record, BUCKET_BLOCK))
		(void) XLogReadBufferForRedo(record, BUCKET_BLOCK, &rp->bufs[BUCKET_BLOCK]);
}

/* block id's page, locked, if its change is to be made again (else NULL) */
static Page
redo_page(RedoPages *rp, uint8 id)
{
	if (XLogReadBufferForRedo(rp->record, id, &rp->bufs[id]) != BLK_NEEDS_REDO)
		return NULL;
	rp->redo[id] = true;
	return BufferGetPage(rp->bufs[id]);
}

/* block id's page, made anew */
static Page
redo_init_page(RedoPages *rp, uint8 id)
{
	rp->bufs[id] = XLogInitBufferForRedo(rp->record, id);
	rp->redo[id] = true;
	return BufferGetPage(rp->bufs[id]);
}

static void
redo_end(RedoPages *rp)
{
	for (int i = 0; i <= BUCKET_BLOCK; i++)
		if (rp->redo[i])
		{
			PageSetLSN(BufferGetPage(rp->bufs[i]), rp->record->EndRecPtr);
			MarkBufferDirty(rp->bufs[i]);
		}
	for (int i = 0; i <= BUCKET_BLOCK; i++)
		if (BufferIsValid(rp->bufs[i]))
			UnlockReleaseBuffer(rp->bufs[i]);
}

/* block id's data, of exactly len bytes */
static char *
redo_data(XLogReaderState *record, uint8 id, Size len)
{
	Size		got;
	char	   *data = XLogRecGetBlockData(record, id, &got);

	if (got != len)
		elog(PANIC, "chunk_index_redo: block %u has %zu bytes of data, not %zu",
			 id, got, len);
	return data;
}

static void
redo_slot(RedoPages *rp, uint8 info)
{
	xl_chunk_index_slot *xlrec = (xl_chunk_index_slot *) XLogRecGetData(rp->record);
	Page		page = redo_page(rp, 0);

	if (!page)
		return;
	if (xlrec->slot >= page_entries(page))
		elog(PANIC, "chunk_index_redo: slot %u of a page of %d entries",
			 xlrec->slot, page_entries(page));
	if (info == XLOG_CHUNK_INDEX_SET_COUNTS)
		ipage_set_counts(page, xlrec->slot, xlrec->refs, xlrec->roots);
	else if (info == XLOG_CHUNK_INDEX_SET_XMAX)
		ipage_set_xmax(page, xlrec->slot, xlrec->xmax);
	else if (info == XLOG_CHUNK_INDEX_SET_MERGE)
		ipage_set_merge(page, xlrec->slot, xlrec->xmax);
	else
		ipage_remove_slot(page, xlrec->slot);
}

void
chunk_index_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	char	   *rec = XLogRecGetData(record);
	RedoPages	rp;
	Page		page;

	/*
	 * (VACUUM's conflict with a hot standby's queries resolved before its
	 * pages are locked, as heap's pruning's)
	 */
	if (info == XLOG_CHUNK_INDEX_VACUUM && InHotStandby &&
		TransactionIdIsValid(((xl_chunk_index_vacuum *) rec)->snapshotConflictHorizon))
	{
		RelFileLocator locator;

		XLogRecGetBlockTag(record, 0, &locator, NULL, NULL);
		ResolveRecoveryConflictWithSnapshot(((xl_chunk_index_vacuum *) rec)->snapshotConflictHorizon,
											false, locator);
	}

	redo_begin(&rp, record);
	switch (info)
	{
		case XLOG_CHUNK_INDEX_VACUUM:
			{
				xl_chunk_index_vacuum *xlrec = (xl_chunk_index_vacuum *) rec;

				page = redo_page(&rp, 0);
				if (page)
				{
					uint8	   *fates = (uint8 *) redo_data(record, 0, xlrec->nentries);

					if (xlrec->nentries != page_entries(page))
						elog(PANIC, "chunk_index_redo: fates of %u entries for a page of %d",
							 xlrec->nentries, page_entries(page));
					(void) ipage_vacuum(page, fates, xlrec->nentries);
				}
				break;
			}
		case XLOG_CHUNK_INDEX_ADD_ENTRY:
			page = redo_page(&rp, 0);
			if (page)
				ipage_add(page, (ChunkIndexEntry *) redo_data(record, 0, sizeof(ChunkIndexEntry)));
			break;
		case XLOG_CHUNK_INDEX_ADD_PAGE:
			{
				xl_chunk_index_add_page *xlrec = (xl_chunk_index_add_page *) rec;
				BlockNumber blk;

				/* (the chain's last page, the meta page, the new page) */
				XLogRecGetBlockTag(record, 2, NULL, NULL, &blk);
				page = redo_page(&rp, 1);
				if (page)
					((IndexPageOpaque) PageGetSpecialPointer(page))->next = blk;
				page = redo_page(&rp, 0);
				if (page)
					((IndexMetaData *) PageGetContents(page))->next_block = blk + 1;
				INJECTION_POINT("chunk-index-redo-add-page", NULL);
				ipage_make(redo_init_page(&rp, 2), INDEX_PAGE_OVERFLOW, xlrec->bucket, 0,
						   InvalidBlockNumber,
						   redo_data(record, 2, sizeof(ChunkIndexEntry)),
						   sizeof(ChunkIndexEntry));
				break;
			}
		case XLOG_CHUNK_INDEX_SET_COUNTS:
		case XLOG_CHUNK_INDEX_SET_XMAX:
		case XLOG_CHUNK_INDEX_SET_MERGE:
		case XLOG_CHUNK_INDEX_REMOVE_SLOT:
			redo_slot(&rp, info);
			break;
		case XLOG_CHUNK_INDEX_KEEP:
			{
				xl_chunk_index_keep *xlrec = (xl_chunk_index_keep *) rec;

				page = redo_page(&rp, 0);
				if (page)
					ipage_keep(page, (uint16 *) redo_data(record, 0, xlrec->nkept * sizeof(uint16)),
							   xlrec->nkept);
				break;
			}
		case XLOG_CHUNK_INDEX_FOLD:
			{
				xl_chunk_index_fold *xlrec = (xl_chunk_index_fold *) rec;
				Page		cpage = NULL;
				Page		chunk_page = NULL;

				/*
				 * (the bucket's first page first, whichever it is, as readers
				 * lock it before its chain's others: then the change's page,
				 * and the chunk's, as the primary)
				 */
				if (xlrec->chunk_on_bucket_page)
					chunk_page = redo_page(&rp, 0);
				if (!xlrec->same_page)
					cpage = redo_page(&rp, 1);
				if (!xlrec->chunk_on_bucket_page && !xlrec->same_page)
					INJECTION_POINT("chunk-index-redo-fold", NULL);
				if (!xlrec->chunk_on_bucket_page)
					chunk_page = redo_page(&rp, 0);
				if ((chunk_page && xlrec->chunk_slot >= page_entries(chunk_page)) ||
					(chunk_page && xlrec->same_page &&
					 xlrec->change_slot >= page_entries(chunk_page)) ||
					(cpage && xlrec->change_slot >= page_entries(cpage)))
					elog(PANIC, "chunk_index_redo: a fold of slots %u and %u past their pages' entries",
						 xlrec->chunk_slot, xlrec->change_slot);
				if (chunk_page)
				{
					ipage_set_counts(chunk_page, xlrec->chunk_slot, xlrec->refs, xlrec->roots);
					if (xlrec->same_page)
						ipage_remove_slot(chunk_page, xlrec->change_slot);
				}
				if (cpage)
					ipage_remove_slot(cpage, xlrec->change_slot);
				break;
			}
		case XLOG_CHUNK_INDEX_INIT_PAGE:
			{
				xl_chunk_index_init_page *xlrec = (xl_chunk_index_init_page *) rec;

				ipage_make(redo_init_page(&rp, 0), xlrec->kind, xlrec->bucket,
						   xlrec->split_at, xlrec->next,
						   redo_data(record, 0, xlrec->len), xlrec->len);
				break;
			}
		case XLOG_CHUNK_INDEX_SPLIT:
			{
				xl_chunk_index_split *xlrec = (xl_chunk_index_split *) rec;

				/* (the old bucket's first page, the meta page, the directory's) */
				page = redo_page(&rp, 1);
				if (page)
					((IndexPageOpaque) PageGetSpecialPointer(page))->split_at = xlrec->new_bucket;
				page = redo_page(&rp, 0);
				if (page)
					imeta_split(page, xlrec);
				INJECTION_POINT("chunk-index-redo-split", NULL);
				if (!xlrec->new_dir)
				{
					page = redo_page(&rp, 2);
					if (page)
						idir_set(page, xlrec->new_bucket % DIR_PER_PAGE, xlrec->first);
				}
				break;
			}
		case XLOG_CHUNK_INDEX_BIND_LOG:
			{
				xl_chunk_index_bind_log *xlrec = (xl_chunk_index_bind_log *) rec;

				page = redo_page(&rp, 0);
				if (page)
				{
					((IndexMetaData *) PageGetContents(page))->log = xlrec->log;
					((IndexMetaData *) PageGetContents(page))->log_stamp = xlrec->stamp;
				}
				break;
			}
		case XLOG_CHUNK_INDEX_MARK:
			page = redo_page(&rp, 0);
			if (page)
				imarks_set(page, ((xl_chunk_index_mark *) rec)->bit,
						   ((xl_chunk_index_mark *) rec)->on != 0);
			break;
		case XLOG_CHUNK_INDEX_ADD_MARKS:
			{
				xl_chunk_index_add_marks *xlrec = (xl_chunk_index_add_marks *) rec;
				BlockNumber blk;

				XLogRecGetBlockTag(record, 1, NULL, NULL, &blk);
				page = redo_page(&rp, 0);
				if (page)
					imeta_add_marks(page, xlrec->nth, blk);
				imarks_init(redo_init_page(&rp, 1), xlrec->nth);
				break;
			}
		default:
			elog(PANIC, "chunk_index_redo: unknown op code %u", info);
	}
	redo_end(&rp);
}

/*
 * For wal_consistency_checking: a page as replayed is the primary's but for
 * its LSN, checksum, hint bits, the bytes past its contents, and its
 * entries' hint flags, which each server learns of its own (FROZEN, made by
 * VACUUM's records, is no hint)
 */
void
chunk_index_mask(char *pagedata, BlockNumber blkno)
{
	Page		page = (Page) pagedata;

	mask_page_lsn_and_checksum(page);
	mask_page_hint_bits(page);
	mask_unused_space(page);
	if (!PageIsNew(page) && PageGetSpecialSize(page) == OPAQUE_SIZE)
	{
		uint16		kind = ((IndexPageOpaque) PageGetSpecialPointer(page))->kind;

		if (kind == INDEX_PAGE_BUCKET || kind == INDEX_PAGE_OVERFLOW)
			for (int i = 0; i < page_entries(page); i++)
				page_entry(page, i)->flags &=
					~(CHUNK_ENTRY_COMMITTED | CHUNK_ENTRY_ABORTED |
					  CHUNK_ENTRY_XMAX_COMMITTED | CHUNK_ENTRY_XMAX_ABORTED);
	}
}


uint32
ChunkIndexFormatVersion(void)
{
	return CHUNK_INDEX_VERSION;
}

uint32
ChunkIndexBuckets(Relation idx)
{
	return load_cache(idx)->maxbucket + 1;
}
