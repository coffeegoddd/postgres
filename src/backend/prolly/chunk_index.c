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

#define CHUNK_INDEX_MAGIC	0x50434958	/* "PCIX" */
#define CHUNK_INDEX_VERSION	6

#define INDEX_PAGE_META		1
#define INDEX_PAGE_DIR		2
#define INDEX_PAGE_BUCKET	3			/* a bucket's first page */
#define INDEX_PAGE_OVERFLOW	4			/* a later page of its chain */

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
	uint32		ndir;			/* the directory's pages */
	BlockNumber dir[FLEXIBLE_ARRAY_MEMBER];
} IndexMetaData;

#define MAX_DIR			((CONTENTS_SIZE - offsetof(IndexMetaData, dir)) / \
						 sizeof(BlockNumber))
#define DIR_PER_PAGE	(CONTENTS_SIZE / sizeof(BlockNumber))
#define PER_PAGE		(CONTENTS_SIZE / sizeof(ChunkIndexEntry))

/* A backend's copy of the meta page, in the relation's rd_amcache. */
typedef struct IndexCache
{
	RelFileNumber relnumber;
	Oid			log;
	uint64		log_stamp;
	uint32		maxbucket;
	uint32		lowmask;
	uint32		highmask;
	uint32		ndir;
	BlockNumber dir[MAX_DIR];
} IndexCache;

StaticAssertDecl(sizeof(ChunkIndexEntry) == 56,
				 "a chunk index entry is 56 bytes");
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
	if (meta->ndir == 0 || meta->ndir > MAX_DIR)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("chunk index \"%s\" has a meta page of %u directory pages",
						RelationGetRelationName(rel), meta->ndir)));
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
			  int *own_at, bool *any_own)
{
	Buffer		cur = primary;
	TransactionId mine = own_blk ? GetCurrentTransactionIdIfAny() : InvalidTransactionId;

	*drefs = *droots = 0;
	if (running)
		*running = InvalidTransactionId;
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
				  false, &drefs, &droots, NULL, NULL, NULL, NULL);
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
	BlockNumber own_blk;
	int			own_at;
	bool		any_own;

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
		count_changes(idx, primary, BUFFER_LOCK_EXCLUSIVE, out, bucket, &masks,
					  false, &crefs, &croots, &running, &own_blk, &own_at, &any_own);

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
	UnlockReleaseBuffer(primary);
	return true;
}

/*
 * Sets xmax of the chunk's entry, this transaction's, if nothing holds the
 * chunk: its counts (committed, and this transaction's) zero, no other
 * transaction changing them, and not being freed already.
 */
static bool
page_free(Relation idx, const uint8 *hash, bool (*held) (const uint8 *hash),
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
				  true, &drefs, &droots, &running, NULL, NULL, NULL);
	if (found != primary)
		LockBuffer(found, BUFFER_LOCK_EXCLUSIVE);
	/*
	 * (a chunk this transaction is putting is held by it until it ends:
	 * another's freer sees no such chunk, its entry not committed)
	 */
	if ((int64) out->refs + drefs != 0 || (int64) out->roots + droots != 0 ||
		TransactionIdIsValid(running) || ChunkEntryIsMine(out) ||
		(held && held(hash)))
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

/*
 * Each chunk's entry this transaction sees, a bucket at a time: its counts
 * added up on the bucket's pages, and fn called with the bucket unlocked.
 */
static void
page_visit(Relation idx, void (*fn) (void *arg, const ChunkIndexEntry *e),
			 void *arg)
{
	for (uint32 b = 0; b <= load_cache(idx)->maxbucket; b++)
	{
		IndexCache	masks = *(IndexCache *) idx->rd_amcache;
		BlockNumber blk = bucket_block(idx, &masks, b);
		Buffer		primary = ReadBuffer(idx, blk);
		Buffer		cur;
		ChunkIndexEntry *chunks = NULL;
		ChunkIndexEntry *changes = NULL;
		int			nchunks = 0,
					nchanges = 0,
					cap1 = 0,
					cap2 = 0;

		LockBuffer(primary, BUFFER_LOCK_SHARE);
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

				if (!belongs(e, b, &masks) || entry_state(e, cur) != ENTRY_VISIBLE)
					continue;
				if (e->kind == CHUNK_KIND_CHUNK)
				{
					if (nchunks == cap1)
					{
						cap1 = cap1 ? cap1 * 2 : 64;
						chunks = chunks ? repalloc(chunks, cap1 * sizeof(*chunks)) :
							palloc(cap1 * sizeof(*chunks));
					}
					chunks[nchunks++] = *e;
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
			LockBuffer(cur, BUFFER_LOCK_SHARE);
			check_ipage(idx, next, BufferGetPage(cur), INDEX_PAGE_OVERFLOW);
		}
		UnlockReleaseBuffer(primary);

		for (int i = 0; i < nchunks; i++)
		{
			int64		drefs = 0,
						droots = 0;

			/* (summed first: in no order, they may dip below zero midway) */
			for (int j = 0; j < nchanges; j++)
				if (change_of(&changes[j], &chunks[i]))
				{
					drefs += (int32) changes[j].refs;
					droots += (int32) changes[j].roots;
				}
			if (!counts_add(&chunks[i], drefs, droots))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("a chunk's reference counts are out of range")));
			fn(arg, &chunks[i]);
		}
		if (chunks)
			pfree(chunks);
		if (changes)
			pfree(changes);
		CHECK_FOR_INTERRUPTS();
	}
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
				  true, &drefs, &droots, &running, NULL, NULL, NULL);
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

const ChunkIndexRoutine PageChunkIndex = {
	.lookup = page_lookup,
	.insert = page_insert,
	.adjust = page_adjust,
	.free = page_free,
	.log_of = page_log_of,
	.bind_log = page_bind_log,
	.remove = page_remove,
	.visit = page_visit,
	.lookup_as_of = page_lookup_as_of,
	.visit_as_of = page_visit_as_of,
	.move = page_move,
};

const ChunkIndexRoutine *ChunkIndex = &PageChunkIndex;

/* ------------------------------------------------------- storage */

void
ChunkIndexInitStorage(SMgrRelation srel, bool use_wal)
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

/* a committed change: of increases (arg true), or of any */
static bool
committed_change(const ChunkIndexEntry *e, const void *arg)
{
	return e->kind == CHUNK_KIND_CHANGE &&
		(!*(const bool *) arg || ((int32) e->refs >= 0 && (int32) e->roots >= 0)) &&
		entry_state_unhinted(e) == ENTRY_VISIBLE;
}

/* the entry (not freed) the change arg is of */
static bool
chunk_of(const ChunkIndexEntry *e, const void *arg)
{
	return e->kind == CHUNK_KIND_CHUNK &&
		change_of(arg, e) && entry_state_unhinted(e) != ENTRY_DEAD;
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
			TransactionId frozen)
{
	bool		freeze;
	bool		clear = false;

	if (!belongs(e, bucket, masks))
		return FATE_REMOVE_COPY;
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

void
ChunkIndexVacuum(Relation idx, bool truncate)
{
	TransactionId frozen = GetOldestNonRemovableTransactionId(idx);
	TimestampTz starttime = GetCurrentTimestamp();
	double		kept_entries = 0;
	Relation	log = NULL;
	bool		log_tried = false;
	bool		any_pass = false;
	void	   *sweep = NULL;
	uint64		unheld = 0;
	ChunkLogSegments segs = {0};
	/*
	 * (the log's segments with bytes of chunks freed kept for snapshots, a
	 * bit each, to pass by: made as the log is opened, outside the windows
	 * between taking entries out and counting their bytes dead, where
	 * nothing is allocated; segments the log grows by meanwhile are not)
	 */
	uint8	   *pending = NULL;
	uint64		npending = 0;
	uint64		segment_bytes = 0;
	ChunkIndexEntry *dead = NULL;
	int			ndead = 0,
				dead_cap = 0;

	/*
	 * Every transaction older than frozen has ended: past its bucket's
	 * pages, each of its entries is out or frozen (WAL-logged), and none
	 * needs the commit log again.  Buckets split meanwhile are walked too
	 * (the highest bucket is read again after each): a split moves entries,
	 * flags and all, from a bucket to a higher one, and an entry inserted
	 * meanwhile is of a transaction past frozen.
	 */
	for (uint32 b = 0; b <= load_cache(idx)->maxbucket; b++)
	{
		IndexCache	masks = *(IndexCache *) idx->rd_amcache;
		BlockNumber blk = bucket_block(idx, &masks, b);
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
		if (!log_tried)
		{
			uint64		stamp;

			if (OidIsValid(page_log_of(idx, &stamp)))
			{
				log = open_bound_log(idx);
				log_tried = true;
				if (log != NULL)
				{
					segment_bytes = ChunkLogOf(log)->segment_bytes(log);
					npending = ChunkLogOf(log)->end(log) / segment_bytes + 1;
					pending = palloc0((npending + 7) / 8);
				}

				/*
				 * (the pages of regions held by none as the pass begins, to
				 * let go of after it if unchanged: the log's vacuum_begin)
				 */
				if (log != NULL && !any_pass)
					sweep = ChunkLogOf(log)->vacuum_begin(log);
			}
		}
		any_pass = true;
		HOLD_INTERRUPTS();
		primary = ReadBuffer(idx, blk);
		LockBuffer(primary, BUFFER_LOCK_EXCLUSIVE);
		check_ipage(idx, blk, BufferGetPage(primary), INDEX_PAGE_BUCKET);
		if (((IndexPageOpaque) PageGetSpecialPointer(BufferGetPage(primary)))->split_at > masks.maxbucket)
			masks = *load_cache(idx);
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
				fates[i] = vacuum_fate(page_entry(page, i), b, &masks, frozen);
				if (log == NULL &&
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
					pending != NULL &&
					page_entry(page, i)->kind == CHUNK_KIND_CHUNK &&
					page_entry(page, i)->len > 0 &&
					TransactionIdIsValid(page_entry(page, i)->xmax) &&
					!(page_entry(page, i)->flags & CHUNK_ENTRY_XMAX_ABORTED))
				{
					ChunkIndexEntry *pe = page_entry(page, i);

					for (uint64 g = pe->loc / segment_bytes;
						 g < npending && g <= (pe->loc + pe->len - 1) / segment_bytes; g++)
						pending[g / 8] |= (uint8) (1 << (g % 8));
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
						if (ndead == dead_cap)
						{
							dead_cap = dead_cap ? dead_cap * 2 : 64;
							dead = dead ? repalloc(dead, dead_cap * sizeof(*dead)) :
								palloc(dead_cap * sizeof(*dead));
						}
						dead[ndead++] = *e;
					}
				}
				START_CRIT_SECTION();
				kept_entries += ipage_vacuum(page, fates, n);
				log_index_change(idx, XLOG_CHUNK_INDEX_VACUUM, &xlrec, sizeof(xlrec), &lp, 1,
								 primary);
				END_CRIT_SECTION();
			}
			else
				kept_entries += n;
			if (cur != primary)
				UnlockReleaseBuffer(cur);
			if (!BlockNumberIsValid(next))
				break;
			CHECK_FOR_INTERRUPTS();
			cur = ReadBuffer(idx, next);
			LockBuffer(cur, BUFFER_LOCK_EXCLUSIVE);
			check_ipage(idx, next, BufferGetPage(cur), INDEX_PAGE_OVERFLOW);
		}
		fold_changes(idx, primary, b, &masks);
		unheld += count_unheld(idx, primary);
		UnlockReleaseBuffer(primary);

		/* the bytes of the chunks taken out, dead in their log */
		if (ndead > 0 && log != NULL)
			for (int i = 0; i < ndead; i++)
				ChunkLogOf(log)->mark_dead(log, dead[i].loc, dead[i].len, dead[i].log_gen);
		ndead = 0;
		RESUME_INTERRUPTS();
	}
	if (!log_tried)
		log = open_bound_log(idx);
	if (log != NULL)
	{
		/*
		 * The log's own end of VACUUM: the regions of backends gone, or that
		 * forgot them, let go of; the free end cut (unless VACUUM is not to
		 * truncate, or the log's own vacuum_truncate is off: the cut's
		 * AccessExclusiveLock, WAL-logged, conflicts with a hot standby's
		 * readers of the log); then what it finds of its segments, those with
		 * chunks freed kept for snapshots passed by
		 */
		ChunkLogOf(log)->vacuum_end(log, sweep,
									truncate &&
									!(log->rd_options != NULL &&
									  ((StdRdOptions *) log->rd_options)->vacuum_truncate == PG_TERNARY_FALSE),
									chunk_compact_dead_fraction, pending, npending,
									&segs);
		if (pending)
			pfree(pending);

		/*
		 * chunks nothing holds, segments to compact, or the end to evacuate:
		 * a freer frees them, and what they held, compacts, and evacuates
		 */
		if (unheld > 0 || (segs.compactable > 0 && segs.worth) || segs.evacuable > 0)
		{
			ereport(DEBUG1,
					(errmsg_internal("chunk index \"%s\": a freer to start: %llu chunks unheld, %u segments to compact%s, %u to evacuate, %u passed by for snapshots",
									 RelationGetRelationName(idx),
									 (unsigned long long) unheld, segs.compactable,
									 segs.worth ? "" : " (not worth it)",
									 segs.evacuable, segs.pending)));
			(void) ChunkFreerLaunch(RelationGetRelid(log), RelationGetRelid(idx),
									segs.compactable > 0 && segs.worth,
									segs.evacuable > 0,
									chunk_compact_dead_fraction, segs.grow);
		}
		relation_close(log, NoLock);
	}

	vac_update_relstats(idx, RelationGetNumberOfBlocks(idx), kept_entries, 0, 0,
						false, frozen, InvalidMultiXactId, NULL, NULL, false);

	/* (as a heap's VACUUM does: autovacuum counts inserts from here) */
	pgstat_report_vacuum(idx, (PgStat_Counter) kept_entries, 0, starttime);
}

uint64
ChunkIndexUnfrozen(Relation idx)
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

void
ChunkIndexShape(Relation idx, uint32 *buckets, uint32 *pages, uint64 *entries)
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

