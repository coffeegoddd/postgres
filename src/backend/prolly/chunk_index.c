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

#include "access/generic_xlog.h"
#include "access/multixact.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "prolly/chunk_index.h"
#include "storage/bufmgr.h"
#include "storage/bulk_write.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#define CHUNK_INDEX_MAGIC	0x50434958	/* "PCIX" */
#define CHUNK_INDEX_VERSION	2

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
	uint32		maxbucket;
	uint32		lowmask;
	uint32		highmask;
	uint32		ndir;
	BlockNumber dir[MAX_DIR];
} IndexCache;

StaticAssertDecl(sizeof(ChunkIndexEntry) == 48,
				 "a chunk index entry is 48 bytes");

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
static EntryState
entry_state(ChunkIndexEntry *e, Buffer buf)
{
	bool		committed;

	if (e->flags & CHUNK_ENTRY_COMMITTED)
		return ENTRY_VISIBLE;
	if (e->flags & CHUNK_ENTRY_ABORTED)
		return ENTRY_DEAD;
	if (TransactionIdIsCurrentTransactionId(e->xmin))
		return ENTRY_VISIBLE;
	if (TransactionIdIsInProgress(e->xmin))
		return ENTRY_IN_PROGRESS;
	committed = TransactionIdDidCommit(e->xmin);
	if (committed)
	{
		XLogRecPtr	commit_lsn = TransactionIdGetCommitLSN(e->xmin);

		if (!XLogNeedsFlush(commit_lsn) || BufferGetLSNAtomic(buf) >= commit_lsn)
			(void) BufferSetHintBits16(&e->flags,
									   e->flags | CHUNK_ENTRY_COMMITTED, buf);
	}
	else
		(void) BufferSetHintBits16(&e->flags, e->flags | CHUNK_ENTRY_ABORTED, buf);
	return committed ? ENTRY_VISIBLE : ENTRY_DEAD;
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
/*
 * (a purge takes out entries not the bucket's, and those of transactions
 * known to have aborted)
 */
static inline bool
kept_by_purge(const ChunkIndexEntry *e, uint32 bucket, const IndexCache *masks)
{
	return belongs(e, bucket, masks) && !(e->flags & CHUNK_ENTRY_ABORTED);
}

static EntryState
find_entry(Relation rel, Buffer primary, int mode, const uint8 *hash,
		   uint32 bucket, const IndexCache *masks, bool purge,
		   Buffer *found_buf, int *found_at,
		   BlockNumber *space, BlockNumber *last, TransactionId *running)
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

			for (int i = 0; i < n; i++)
				if (kept_by_purge(page_entry(page, i), bucket, masks))
					kept++;
			if (kept < n)
			{
				GenericXLogState *st = GenericXLogStart(rel);
				Page		p = GenericXLogRegisterBuffer(st, cur, 0);
				int			k = 0;

				for (int i = 0; i < n; i++)
					if (kept_by_purge(page_entry(p, i), bucket, masks))
						*page_entry(p, k++) = *page_entry(p, i);
				set_page_entries(p, k);
				GenericXLogFinish(st);
				n = k;
			}
		}
		for (int i = 0; i < n; i++)
		{
			ChunkIndexEntry *e = page_entry(page, i);

			if (memcmp(e->hash, hash, CHUNK_HASH_LEN) != 0 ||
				!belongs(e, bucket, masks))
				continue;
			switch (entry_state(e, cur))
			{
				case ENTRY_VISIBLE:
					*found_buf = cur;
					*found_at = i;
					return ENTRY_VISIBLE;
				case ENTRY_IN_PROGRESS:
					if (running)
						*running = e->xmin;
					result = ENTRY_IN_PROGRESS;
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
		GenericXLogState *st = GenericXLogStart(rel);
		Page		page = GenericXLogRegisterBuffer(st, buf, GENERIC_XLOG_FULL_IMAGE);
		int			from = p * PER_PAGE;
		int			n = Min(nmoving - from, (int) PER_PAGE);

		init_ipage(page, p == 0 ? INDEX_PAGE_BUCKET : INDEX_PAGE_OVERFLOW,
				   new_b, new_b);
		if (p + 1 < npages)
			((IndexPageOpaque) PageGetSpecialPointer(page))->next = blk + 1;
		if (n > 0)
			memcpy(page_entry(page, 0), moving + from, n * sizeof(ChunkIndexEntry));
		set_page_entries(page, Max(n, 0));
		GenericXLogFinish(st);
		UnlockReleaseBuffer(buf);
	}
	if (new_dir)
	{
		Buffer		buf;
		GenericXLogState *st;
		Page		page;

		dir_blk = meta->next_block;
		buf = new_page_buffer(rel, dir_blk);
		st = GenericXLogStart(rel);
		page = GenericXLogRegisterBuffer(st, buf, GENERIC_XLOG_FULL_IMAGE);
		init_ipage(page, INDEX_PAGE_DIR, 0, 0);
		((BlockNumber *) PageGetContents(page))[new_b % DIR_PER_PAGE] = first;
		((PageHeader) page)->pd_lower = SizeOfPageHeaderData +
			(new_b % DIR_PER_PAGE + 1) * sizeof(BlockNumber);
		GenericXLogFinish(st);
		UnlockReleaseBuffer(buf);
	}
	else
		dir_blk = meta->dir[new_b / DIR_PER_PAGE];

	/* one record makes it a bucket */
	{
		GenericXLogState *st = GenericXLogStart(rel);
		Page		mpage = GenericXLogRegisterBuffer(st, mbuf, 0);
		Page		opage = GenericXLogRegisterBuffer(st, primary, 0);
		Buffer		dbuf = InvalidBuffer;

		if (!new_dir)
		{
			Page		dpage;

			dbuf = ReadBuffer(rel, dir_blk);
			LockBuffer(dbuf, BUFFER_LOCK_EXCLUSIVE);
			check_ipage(rel, dir_blk, BufferGetPage(dbuf), INDEX_PAGE_DIR);
			dpage = GenericXLogRegisterBuffer(st, dbuf, 0);
			((BlockNumber *) PageGetContents(dpage))[new_b % DIR_PER_PAGE] = first;
			((PageHeader) dpage)->pd_lower = SizeOfPageHeaderData +
				(new_b % DIR_PER_PAGE + 1) * sizeof(BlockNumber);
		}
		meta = (IndexMetaData *) PageGetContents(mpage);
		if (new_dir)
			meta->dir[meta->ndir++] = dir_blk;
		meta->maxbucket = new_b;
		meta->lowmask = lowmask;
		meta->highmask = highmask;
		meta->next_block = first + npages;
		set_meta_lower(mpage, meta);
		((IndexPageOpaque) PageGetSpecialPointer(opage))->split_at = new_b;
		GenericXLogFinish(st);
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
			int			kept = 0;

			for (int i = 0; i < n; i++)
				if (belongs(page_entry(page, i), old_b, &masks))
					kept++;
			if (kept < n)
			{
				GenericXLogState *st = GenericXLogStart(rel);
				Page		p = GenericXLogRegisterBuffer(st, cur, 0);
				int			k = 0;

				for (int i = 0; i < n; i++)
					if (belongs(page_entry(p, i), old_b, &masks))
						*page_entry(p, k++) = *page_entry(p, i);
				set_page_entries(p, k);
				GenericXLogFinish(st);
			}
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

static bool
page_lookup(Relation idx, const uint8 *hash, ChunkIndexEntry *out)
{
	uint32		bucket;
	IndexCache	masks;
	Buffer		primary = lock_bucket(idx, hash, BUFFER_LOCK_SHARE, &bucket, &masks);
	Buffer		found;
	int			at;
	bool		hit;

	hit = find_entry(idx, primary, BUFFER_LOCK_SHARE, hash, bucket, &masks,
					 false, &found, &at, NULL, NULL, NULL) == ENTRY_VISIBLE;
	if (hit)
	{
		*out = *page_entry(BufferGetPage(found), at);
		release_found(primary, found);
	}
	else
		UnlockReleaseBuffer(primary);
	return hit;
}

static bool
page_insert(Relation idx, ChunkIndexEntry *entry)
{
	uint32		bucket;
	IndexCache	masks;
	Buffer		primary;
	Buffer		found;
	int			at;
	BlockNumber space;
	BlockNumber last;
	bool		grew = false;
	TransactionId running;

	PreventCommandIfReadOnly("insert into a chunk index");
	entry->xmin = GetCurrentTransactionId();
	entry->flags = 0;
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

	if (BlockNumberIsValid(space))
	{
		Buffer		buf = primary;
		GenericXLogState *st;
		Page		page;
		int			n;

		if (space != BufferGetBlockNumber(primary))
		{
			buf = ReadBuffer(idx, space);
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		}
		st = GenericXLogStart(idx);
		page = GenericXLogRegisterBuffer(st, buf, 0);
		n = page_entries(page);
		*page_entry(page, n) = *entry;
		set_page_entries(page, n + 1);
		GenericXLogFinish(st);
		if (buf != primary)
		{
			UnlockReleaseBuffer(buf);
			grew = true;
		}
	}
	else
	{
		/* a new page at the chain's end, from the meta page */
		Buffer		mbuf = ReadBuffer(idx, META_BLOCK);
		Buffer		lbuf = primary;
		Buffer		nbuf;
		IndexMetaData *meta;
		BlockNumber blk;
		GenericXLogState *st;
		Page		mpage;
		Page		lpage;
		Page		npage;

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
		st = GenericXLogStart(idx);
		mpage = GenericXLogRegisterBuffer(st, mbuf, 0);
		lpage = GenericXLogRegisterBuffer(st, lbuf, 0);
		npage = GenericXLogRegisterBuffer(st, nbuf, GENERIC_XLOG_FULL_IMAGE);
		((IndexMetaData *) PageGetContents(mpage))->next_block = blk + 1;
		((IndexPageOpaque) PageGetSpecialPointer(lpage))->next = blk;
		init_ipage(npage, INDEX_PAGE_OVERFLOW, bucket, 0);
		*page_entry(npage, 0) = *entry;
		set_page_entries(npage, 1);
		GenericXLogFinish(st);
		UnlockReleaseBuffer(nbuf);
		UnlockReleaseBuffer(mbuf);
		if (lbuf != primary)
			UnlockReleaseBuffer(lbuf);
		grew = true;
	}
	UnlockReleaseBuffer(primary);

	/*
	 * An entry past a bucket's first page: the index grows a bucket, so that
	 * buckets stay a page long, mostly
	 */
	if (grew)
		split_bucket(idx);
	return true;
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
	GenericXLogState *st;
	ChunkIndexEntry *e;
	int64		refs,
				roots;

	PreventCommandIfReadOnly("change a chunk index");
	(void) GetCurrentTransactionId();
	primary = lock_bucket(idx, hash, BUFFER_LOCK_EXCLUSIVE, &bucket, &masks);
	if (find_entry(idx, primary, BUFFER_LOCK_EXCLUSIVE, hash, bucket, &masks,
				   false, &found, &at, NULL, NULL, NULL) != ENTRY_VISIBLE)
	{
		UnlockReleaseBuffer(primary);
		return false;
	}
	st = GenericXLogStart(idx);
	e = page_entry(GenericXLogRegisterBuffer(st, found, 0), at);
	refs = (int64) e->refs + drefs;
	roots = (int64) e->roots + droots;
	if (refs < 0 || roots < 0 || refs > PG_UINT32_MAX || roots > PG_UINT32_MAX)
	{
		GenericXLogAbort(st);
		release_found(primary, found);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("a chunk's reference counts would be %lld and %lld",
						(long long) refs, (long long) roots)));
	}
	e->refs = (uint32) refs;
	e->roots = (uint32) roots;
	*out = *e;
	GenericXLogFinish(st);
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
	GenericXLogState *st;
	Page		page;
	int			n;

	PreventCommandIfReadOnly("change a chunk index");
	(void) GetCurrentTransactionId();
	primary = lock_bucket(idx, hash, BUFFER_LOCK_EXCLUSIVE, &bucket, &masks);
	if (find_entry(idx, primary, BUFFER_LOCK_EXCLUSIVE, hash, bucket, &masks,
				   false, &found, &at, NULL, NULL, NULL) != ENTRY_VISIBLE)
	{
		UnlockReleaseBuffer(primary);
		return false;
	}
	st = GenericXLogStart(idx);
	page = GenericXLogRegisterBuffer(st, found, 0);
	n = page_entries(page);
	*page_entry(page, at) = *page_entry(page, n - 1);
	set_page_entries(page, n - 1);
	GenericXLogFinish(st);
	release_found(primary, found);
	return true;
}

const ChunkIndexRoutine PageChunkIndex = {
	.lookup = page_lookup,
	.insert = page_insert,
	.adjust = page_adjust,
	.remove = page_remove,
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

void
ChunkIndexVacuum(Relation idx)
{
	TransactionId frozen = GetOldestNonRemovableTransactionId(idx);
	IndexCache *cache = load_cache(idx);
	uint32		nbuckets = cache->maxbucket + 1;
	IndexCache	masks = *cache;

	/*
	 * Every transaction older than frozen has ended: past its bucket's
	 * pages, each of its entries is out or noted committed, and none needs
	 * the commit log again.  (A split meanwhile only moves entries, flags
	 * and all; buckets made after the copy of the meta page was read are
	 * made of entries of buckets before them.)
	 */
	for (uint32 b = 0; b < nbuckets; b++)
	{
		BlockNumber blk = bucket_block(idx, cache, b);
		Buffer		primary = ReadBuffer(idx, blk);
		Buffer		cur;

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
			bool		change = false;

			for (int i = 0; i < n && !change; i++)
			{
				ChunkIndexEntry *e = page_entry(page, i);
				EntryState	state = entry_state(e, cur);

				change = !belongs(e, b, &masks) || state == ENTRY_DEAD ||
					(state == ENTRY_VISIBLE && !(e->flags & CHUNK_ENTRY_COMMITTED));
			}
			if (change)
			{
				GenericXLogState *st = GenericXLogStart(idx);
				Page		p = GenericXLogRegisterBuffer(st, cur, 0);
				int			k = 0;

				/* (each entry's state from the buffer's page, not the copy) */
				for (int i = 0; i < n; i++)
				{
					EntryState	state = entry_state(page_entry(page, i), cur);
					ChunkIndexEntry e = *page_entry(p, i);

					if (!belongs(&e, b, &masks) || state == ENTRY_DEAD)
						continue;
					if (state == ENTRY_VISIBLE)
						e.flags |= CHUNK_ENTRY_COMMITTED;
					*page_entry(p, k++) = e;
				}
				set_page_entries(p, k);
				GenericXLogFinish(st);
			}
			if (cur != primary)
				UnlockReleaseBuffer(cur);
			if (!BlockNumberIsValid(next))
				break;
			CHECK_FOR_INTERRUPTS();
			cur = ReadBuffer(idx, next);
			LockBuffer(cur, BUFFER_LOCK_EXCLUSIVE);
			check_ipage(idx, next, BufferGetPage(cur), INDEX_PAGE_OVERFLOW);
		}
		UnlockReleaseBuffer(primary);
	}

	vac_update_relstats(idx, RelationGetNumberOfBlocks(idx), 0, 0, 0, false,
						frozen, InvalidMultiXactId, NULL, NULL, false);
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
