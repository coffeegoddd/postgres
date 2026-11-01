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
#include "access/relation.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "prolly/chunk_freer.h"
#include "prolly/chunk_index.h"
#include "prolly/chunk_log.h"
#include "prolly/chunk_xlog.h"
#include "storage/bufmgr.h"
#include "storage/bulk_write.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "utils/timestamp.h"
#include "utils/injection_point.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#define CHUNK_INDEX_MAGIC	0x50434958	/* "PCIX" */
#define CHUNK_INDEX_VERSION	5

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

/*
 * A record that moves entries between a page's slots logs the whole page: a
 * delta would leave out bytes the same on the primary, and a standby may
 * have hint flags of its own in them, which would then be another entry's.
 */
#define MOVES_ENTRIES	GENERIC_XLOG_FULL_IMAGE

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

			for (int i = 0; i < n; i++)
				if (kept_by_purge(page_entry(page, i), bucket, masks))
					kept++;
			if (kept < n)
			{
				GenericXLogState *st = GenericXLogStart(rel);
				Page		p = GenericXLogRegisterBuffer(st, cur, MOVES_ENTRIES);
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

			if (e->kind != CHUNK_KIND_CHUNK ||
				memcmp(e->hash, hash, CHUNK_HASH_LEN) != 0 ||
				!belongs(e, bucket, masks))
				continue;
			switch (entry_state_wait(e, cur, running))
			{
				case ENTRY_VISIBLE:
					*found_buf = cur;
					*found_at = i;
					return ENTRY_VISIBLE;
				case ENTRY_IN_PROGRESS:
					result = ENTRY_IN_PROGRESS;
					/*
					 * (a chunk put and freed by one transaction running is
					 * not the store's to wait for: whatever it does, the
					 * chunk is gone)
					 */
					if (freeing && running)
						*freeing = (*running == e->xmax && e->xmax != e->xmin);
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
				Page		p = GenericXLogRegisterBuffer(st, cur, MOVES_ENTRIES);
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

/*
 * The changes of hash's counts on the chain of the bucket whose first page
 * is primary (locked): those this transaction sees added to *drefs and
 * *droots; the transaction of a decrease it doesn't see (another's,
 * running) in *running, if it is given (an increase it doesn't see can only
 * add to the counts); and where this very transaction's own change is, in
 * *own_blk and *own_at, if they are given.
 */
static void
count_changes(Relation rel, Buffer primary, int mode,
			  const ChunkIndexEntry *chunk, uint32 bucket,
			  const IndexCache *masks, bool any_running, int64 *drefs,
			  int64 *droots, TransactionId *running, BlockNumber *own_blk,
			  int *own_at)
{
	Buffer		cur = primary;
	TransactionId mine = own_blk ? GetCurrentTransactionIdIfAny() : InvalidTransactionId;

	*drefs = *droots = 0;
	if (running)
		*running = InvalidTransactionId;
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
					if (own_blk && TransactionIdIsValid(mine) && e->xmin == mine)
					{
						*own_blk = BufferGetBlockNumber(cur);
						*own_at = i;
					}
					break;
				case ENTRY_IN_PROGRESS:
					if (running &&
						(any_running || (int32) e->refs < 0 || (int32) e->roots < 0))
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
		  BlockNumber last, const ChunkIndexEntry *entry)
{
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
		UnlockReleaseBuffer(primary);
		if (st != ENTRY_IN_PROGRESS || !freeing)
			return false;
		XactLockTableWait(running, NULL, NULL, XLTW_None);
	}
	*out = *page_entry(BufferGetPage(found), at);
	if (found != primary)
		UnlockReleaseBuffer(found);
	count_changes(idx, primary, BUFFER_LOCK_SHARE, out, bucket, &masks,
				  false, &drefs, &droots, NULL, NULL, NULL);
	UnlockReleaseBuffer(primary);
	if (!counts_add(out, drefs, droots))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("a chunk's reference counts are out of range")));
	return true;
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
	bool		grew;
	TransactionId running;

	PreventCommandIfReadOnly("insert into a chunk index");
	entry->xmin = GetCurrentTransactionId();
	entry->xmax = InvalidTransactionId;
	entry->flags = 0;
	entry->kind = CHUNK_KIND_CHUNK;
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
	grew = add_entry(idx, primary, bucket, space, last, entry);
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
					  false, &crefs, &croots, &running, &own_blk, &own_at);

		/*
		 * (a decrease waits for another's decreases, as a row's update waits
		 * for another's: two taking the last root away would both see it)
		 */
		if ((drefs < 0 || droots < 0) && TransactionIdIsValid(running))
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

	if (BlockNumberIsValid(own_blk))
	{
		/* this transaction's change of the chunk's counts, changed again */
		Buffer		buf = own_blk == BufferGetBlockNumber(primary) ? primary :
			ReadBuffer(idx, own_blk);
		GenericXLogState *st;
		ChunkIndexEntry *e;

		if (buf != primary)
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		st = GenericXLogStart(idx);
		e = page_entry(GenericXLogRegisterBuffer(st, buf, 0), own_at);
		e->refs = (uint32) ((int32) e->refs + drefs);
		e->roots = (uint32) ((int32) e->roots + droots);
		GenericXLogFinish(st);
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
		(void) add_entry(idx, primary, bucket, space, last, &change);
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
	GenericXLogState *st;
	ChunkIndexEntry *e;

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
	count_changes(idx, primary, BUFFER_LOCK_EXCLUSIVE, out, bucket, &masks,
				  true, &drefs, &droots, &running, NULL, NULL);
	if ((int64) out->refs + drefs != 0 || (int64) out->roots + droots != 0 ||
		TransactionIdIsValid(running) || (held && held(hash)))
	{
		release_found(primary, found);
		return false;
	}
	st = GenericXLogStart(idx);
	e = page_entry(GenericXLogRegisterBuffer(st, found, 0), at);
	e->xmax = xid;
	e->flags &= ~(CHUNK_ENTRY_XMAX_COMMITTED | CHUNK_ENTRY_XMAX_ABORTED);
	GenericXLogFinish(st);
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
		GenericXLogState *st = GenericXLogStart(idx);
		Page		page = GenericXLogRegisterBuffer(st, found, MOVES_ENTRIES);
		int			n = page_entries(page);

		*page_entry(page, at) = *page_entry(page, n - 1);
		set_page_entries(page, n - 1);
		GenericXLogFinish(st);
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
			int			kept = 0;

			for (int i = 0; i < n; i++)
				if (memcmp(page_entry(page, i)->hash, hash, CHUNK_HASH_LEN) != 0)
					kept++;
			if (kept < n)
			{
				GenericXLogState *st = GenericXLogStart(idx);
				Page		p = GenericXLogRegisterBuffer(st, cur, MOVES_ENTRIES);
				int			k = 0;

				for (int i = 0; i < n; i++)
					if (memcmp(page_entry(p, i)->hash, hash, CHUNK_HASH_LEN) != 0)
						*page_entry(p, k++) = *page_entry(p, i);
				set_page_entries(p, k);
				GenericXLogFinish(st);
			}
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
	if (!RelationIsChunkLog(log) || ChunkLog->stamp(log) != stamp)
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
	GenericXLogState *st;
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
	st = GenericXLogStart(idx);
	meta = (IndexMetaData *) PageGetContents(GenericXLogRegisterBuffer(st, mbuf, 0));
	meta->log = log;
	meta->log_stamp = stamp;
	GenericXLogFinish(st);
	UnlockReleaseBuffer(mbuf);
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
		GenericXLogState *st;
		Page		cpage,
					epage;
		ChunkIndexEntry *e;
		int			n;

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
			Page		p;

			if (buf != primary)
				LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
			st = GenericXLogStart(idx);
			p = GenericXLogRegisterBuffer(st, buf, MOVES_ENTRIES);
			n = page_entries(p);
			*page_entry(p, cat) = *page_entry(p, n - 1);
			set_page_entries(p, n - 1);
			GenericXLogFinish(st);
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
		st = GenericXLogStart(idx);
		cpage = GenericXLogRegisterBuffer(st, cbuf, MOVES_ENTRIES);
		epage = ebuf == cbuf ? cpage : GenericXLogRegisterBuffer(st, ebuf, 0);
		e = page_entry(epage, eat);
		if (!counts_add(e, (int32) change.refs, (int32) change.roots))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("a chunk's reference counts are out of range")));
		n = page_entries(cpage);

		/* (out by the last entry moved into its slot: the chunk's may be it) */
		*page_entry(cpage, cat) = *page_entry(cpage, n - 1);
		set_page_entries(cpage, n - 1);
		GenericXLogFinish(st);
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
	FATE_KEEP,
	FATE_REMOVE,				/* aborted: its bytes dead */
	FATE_REMOVE_FREED,			/* freed: its bytes dead */
	FATE_REMOVE_COPY,			/* not the bucket's: another's bytes */
	FATE_FREEZE,				/* xmin committed, not noted so in WAL */
	FATE_CLEAR,					/* xmax aborted: no freeing now */
	FATE_FREEZE_CLEAR,
} VacuumFate;

/*
 * Before freed chunks' entries are taken out, and their bytes dead, for
 * hot standbys: their queries whose snapshots might have found one before
 * its freeing committed conflict with it (as with heap pruning's), there
 * being no knowing on the primary which chunks those are reading.
 */
static void
log_cleanup_info(Relation idx, TransactionId horizon)
{
	xl_chunk_index_cleanup_info xlrec;

	xlrec.locator = idx->rd_locator;
	xlrec.snapshotConflictHorizon = horizon;
	XLogBeginInsert();
	XLogRegisterData(&xlrec, SizeOfChunkIndexCleanupInfo);
	(void) XLogInsert(RM_CHUNK_STORE_ID, XLOG_CHUNK_INDEX_CLEANUP_INFO);
}

/*
 * Each transaction id an entry names on its own: xmin, committed, is
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
	freeze = !(e->flags & CHUNK_ENTRY_FROZEN);
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
ChunkIndexVacuum(Relation idx)
{
	TransactionId frozen = GetOldestNonRemovableTransactionId(idx);
	TimestampTz starttime = GetCurrentTimestamp();
	double		kept_entries = 0;
	Relation	log = NULL;
	bool		log_tried = false;
	uint64		unheld = 0;
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
			VacuumFate *fates = palloc(Max(n, 1) * sizeof(VacuumFate));
			bool		change = false;
			TransactionId horizon = InvalidTransactionId;

			for (int i = 0; i < n; i++)
			{
				fates[i] = vacuum_fate(page_entry(page, i), b, &masks, frozen);
				if (fates[i] != FATE_KEEP)
					change = true;
				if (fates[i] == FATE_REMOVE_FREED &&
					(!TransactionIdIsValid(horizon) ||
					 TransactionIdFollows(page_entry(page, i)->xmax, horizon)))
					horizon = page_entry(page, i)->xmax;
			}
			if (TransactionIdIsValid(horizon) && RelationNeedsWAL(idx) &&
				XLogStandbyInfoActive())
				log_cleanup_info(idx, horizon);
			if (change)
			{
				GenericXLogState *st = GenericXLogStart(idx);
				Page		p = GenericXLogRegisterBuffer(st, cur, MOVES_ENTRIES);
				int			k = 0;

				for (int i = 0; i < n; i++)
				{
					ChunkIndexEntry e = *page_entry(p, i);

					if (fates[i] == FATE_REMOVE_COPY)
						continue;
					if (fates[i] == FATE_REMOVE || fates[i] == FATE_REMOVE_FREED)
					{
						/* (a chunk's bytes, dead once its entry is out) */
						if (e.kind == CHUNK_KIND_CHUNK && e.len > 0)
						{
							if (ndead == dead_cap)
							{
								dead_cap = dead_cap ? dead_cap * 2 : 64;
								dead = dead ? repalloc(dead, dead_cap * sizeof(*dead)) :
									palloc(dead_cap * sizeof(*dead));
							}
							dead[ndead++] = e;
						}
						continue;
					}
					if (fates[i] == FATE_FREEZE || fates[i] == FATE_FREEZE_CLEAR)
						e.flags |= CHUNK_ENTRY_FROZEN | CHUNK_ENTRY_COMMITTED;
					if (fates[i] == FATE_CLEAR || fates[i] == FATE_FREEZE_CLEAR)
					{
						/* (a freeing that aborted, gone with it) */
						e.flags &= ~(CHUNK_ENTRY_XMAX_COMMITTED | CHUNK_ENTRY_XMAX_ABORTED);
						e.xmax = InvalidTransactionId;
					}
					*page_entry(p, k++) = e;
				}
				set_page_entries(p, k);
				GenericXLogFinish(st);
				kept_entries += k;
			}
			else
				kept_entries += n;
			pfree(fates);
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

		/*
		 * the bytes of the chunks taken out, dead in their log (found when
		 * there are some: a first put may have bound the index meanwhile)
		 */
		if (ndead > 0 && !log_tried)
		{
			log = open_bound_log(idx);
			log_tried = true;
		}
		if (ndead > 0 && log != NULL)
			for (int i = 0; i < ndead; i++)
				ChunkLog->mark_dead(log, dead[i].loc, dead[i].len);
		ndead = 0;
	}
	if (!log_tried)
		log = open_bound_log(idx);
	if (log != NULL)
	{
		ChunkLogRecordFree(log);
		(void) ChunkLogTruncateTail(log);

		/* chunks nothing holds: a freer frees them, and what they held */
		if (unheld > 0)
			(void) ChunkFreerLaunch(RelationGetRelid(log), RelationGetRelid(idx));
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
