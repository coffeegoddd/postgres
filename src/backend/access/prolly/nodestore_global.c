/*-------------------------------------------------------------------------
 *
 * nodestore_global.c
 *	  Node store implementation "global": one cluster-wide, hash-addressed
 *	  store.
 *
 * Every relation's nodes, in every database and on every branch, live in
 * one store, so a tree is found from its branch's root, never from a
 * catalog entry (catalogs are themselves versioned).  The store
 * is three relation files in the global tablespace, read through smgr and
 * the buffer manager like any shared catalog, so checksums, checkpoints,
 * full-page writes and pg_stat_io apply unchanged:
 *
 *	STORE_NODES		the nodes, one per block, appended
 *	STORE_MAP		hash -> block map: metapage, then bucket b at block b + 1
 *	STORE_OVERFLOW	the map's overflow pages
 *
 * Their relation file numbers are below FirstGenbkiObjectId and are not
 * catalog OIDs, so neither initdb nor a running server ever assigns them to
 * anything else.  The store is created the first time it is used, under a
 * heavyweight lock on the map's file number.
 *
 * The map is a linear hash table keyed by the first four bytes of the
 * content hash (which are uniformly distributed).  Each entry is the full
 * hash and a block number.  Pages hold entries unsorted.  When an insert
 * has to add an overflow page, the next bucket in order is split, so the
 * table grows one bucket at a time.
 *
 * Locking: every map operation holds the metapage's content lock for its
 * whole duration, shared for lookups, inserts and deletes and exclusive for
 * a split, so a split never moves entries under a lookup.  Changes to a
 * bucket's chain are serialized by an exclusive lock on its primary page;
 * other pages in the chain are locked when they are read or changed.
 *
 * WAL: inserts and deletes log small ProllyStore records that redo
 * replays; a split logs full images of every page it touches.  Writing node
 * contents is the log interface's job (prolly_log.h), not the store's.
 *
 * Limitations for now: release() removes a node from the map, but its
 * block is not reused (reclamation and space reuse come later).  Overflow
 * pages freed by splits are reused only by later splits.  Only permanent
 * relations are supported; temporary and unlogged ones are not decided yet.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/nodestore_global.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"
#include "access/prolly_xlog.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "catalog/pg_class.h"
#include "catalog/pg_tablespace_d.h"
#include "catalog/storage_xlog.h"
#include "miscadmin.h"
#include "port/pg_bswap.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/smgr.h"

#define STORE_NODES		((RelFileNumber) 9393)
#define STORE_MAP		((RelFileNumber) 9394)
#define STORE_OVERFLOW	((RelFileNumber) 9395)

#define STORE_IMPL_ID	1

/* The map's key: SHA-256 digests (content hash "sha256"). */
#define MAP_HASH_LEN	32
#define MAP_ENTRY_SIZE	(MAP_HASH_LEN + sizeof(BlockNumber))

#define MAP_MAGIC		0x50524D50	/* "PRMP" */
#define MAP_VERSION		1
#define MAP_INITIAL_BUCKETS 2

/* A split logs every page it touches; leave room for the metapage. */
#define MAX_SPLIT_PAGES	(XLR_MAX_BLOCK_ID - 1)

/* ProllyStore record types (high bits of info) */
#define XLOG_STORE_MAP_INSERT		0x10	/* entry added to an existing page */
#define XLOG_STORE_MAP_INSERT_NEW	0x20	/* entry in a new overflow page */
#define XLOG_STORE_MAP_DELETE		0x30	/* entry removed */
#define XLOG_STORE_MAP_SPLIT		0x40	/* bucket split: full page images */

typedef struct MapMeta
{
	uint32		magic;
	uint32		version;
	uint32		hash_len;
	uint32		maxbucket;		/* highest bucket number in use */
	uint32		lowmask;
	uint32		highmask;
	BlockNumber free_overflow;	/* free overflow pages, linked by next */
} MapMeta;

/* Special space of bucket and overflow pages. */
typedef struct MapPageOpaque
{
	BlockNumber next;			/* next overflow page, or InvalidBlockNumber */
	uint16		nentries;
	uint16		flags;
} MapPageOpaque;

struct ProllyNodeStore
{
	int			unused;			/* the store is global; nothing per handle */
};

/* ------------------------------------------------------------------------
 * Files, buffers and creation
 * ------------------------------------------------------------------------
 */

static RelFileLocator
store_locator(RelFileNumber relnumber)
{
	RelFileLocator loc;

	loc.spcOid = GLOBALTABLESPACE_OID;
	loc.dbOid = InvalidOid;
	loc.relNumber = relnumber;
	return loc;
}

static SMgrRelation
store_smgr(RelFileNumber relnumber)
{
	return smgropen(store_locator(relnumber), INVALID_PROC_NUMBER);
}

static Buffer
store_read(RelFileNumber relnumber, BlockNumber blkno, int lockmode)
{
	Buffer		buf = ReadBufferWithoutRelcache(store_locator(relnumber),
												MAIN_FORKNUM, blkno, RBM_NORMAL,
												NULL, true);

	LockBuffer(buf, lockmode);
	return buf;
}

/*
 * Add a block to one of the store's files and return it zeroed and
 * exclusive-locked.  The files have no relcache entry, so this takes the
 * relation extension lock itself.
 */
static Buffer
store_extend(RelFileNumber relnumber)
{
	LOCKTAG		tag;
	Buffer		buf;

	SET_LOCKTAG_RELATION_EXTEND(tag, InvalidOid, relnumber);
	LockAcquire(&tag, ExclusiveLock, false, false);
	buf = ExtendBufferedRel(BMR_SMGR(store_smgr(relnumber), RELPERSISTENCE_PERMANENT),
							MAIN_FORKNUM, NULL,
							EB_LOCK_FIRST | EB_SKIP_EXTENSION_LOCK);
	LockRelease(&tag, ExclusiveLock, false);
	return buf;
}

static inline MapMeta *
meta_of(Page page)
{
	return (MapMeta *) PageGetContents(page);
}

static inline MapPageOpaque *
opaque_of(Page page)
{
	return (MapPageOpaque *) PageGetSpecialPointer(page);
}

static inline char *
entry_at(Page page, int i)
{
	return PageGetContents(page) + i * MAP_ENTRY_SIZE;
}

static int
page_capacity(void)
{
	return (BLCKSZ - MAXALIGN(SizeOfPageHeaderData) -
			MAXALIGN(sizeof(MapPageOpaque))) / MAP_ENTRY_SIZE;
}

static void
map_page_init(Page page)
{
	PageInit(page, BLCKSZ, sizeof(MapPageOpaque));
	opaque_of(page)->next = InvalidBlockNumber;
	opaque_of(page)->nentries = 0;
	opaque_of(page)->flags = 0;
	((PageHeader) page)->pd_lower = MAXALIGN(SizeOfPageHeaderData);
}

static void
map_page_set_count(Page page, int n)
{
	opaque_of(page)->nentries = n;
	((PageHeader) page)->pd_lower = MAXALIGN(SizeOfPageHeaderData) + n * MAP_ENTRY_SIZE;
}

static void
meta_page_init(Page page)
{
	MapMeta    *meta;

	PageInit(page, BLCKSZ, 0);
	meta = meta_of(page);
	meta->magic = MAP_MAGIC;
	meta->version = MAP_VERSION;
	meta->hash_len = MAP_HASH_LEN;
	meta->maxbucket = MAP_INITIAL_BUCKETS - 1;
	meta->lowmask = MAP_INITIAL_BUCKETS - 1;
	meta->highmask = (MAP_INITIAL_BUCKETS << 1) - 1;
	meta->free_overflow = InvalidBlockNumber;
	((PageHeader) page)->pd_lower = (char *) (meta + 1) - (char *) page;
}

static bool
store_file_exists(RelFileNumber relnumber)
{
	return smgrexists(store_smgr(relnumber), MAIN_FORKNUM);
}

/* Create the store's files and an empty map, if not done yet. */
static void
ensure_store(void)
{
	static bool exists = false;
	RelFileNumber files[] = {STORE_NODES, STORE_MAP, STORE_OVERFLOW};
	LOCKTAG		tag;

	if (exists)
		return;
	if (store_file_exists(STORE_MAP) &&
		smgrnblocks(store_smgr(STORE_MAP), MAIN_FORKNUM) > MAP_INITIAL_BUCKETS)
	{
		exists = true;
		return;
	}
	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("prolly node store does not exist yet")));

	SET_LOCKTAG_RELATION(tag, InvalidOid, STORE_MAP);
	LockAcquire(&tag, AccessExclusiveLock, false, false);
	for (int i = 0; i < lengthof(files); i++)
	{
		if (!store_file_exists(files[i]))
		{
			RelFileLocator loc = store_locator(files[i]);

			smgrcreate(store_smgr(files[i]), MAIN_FORKNUM, false);
			log_smgrcreate(&loc, MAIN_FORKNUM);
		}
	}

	/* The metapage and the initial buckets. */
	for (BlockNumber blk = smgrnblocks(store_smgr(STORE_MAP), MAIN_FORKNUM);
		 blk <= MAP_INITIAL_BUCKETS; blk++)
	{
		Buffer		buf = store_extend(STORE_MAP);

		if (BufferGetBlockNumber(buf) != blk)
			elog(ERROR, "prolly map extended to block %u, expected %u",
				 BufferGetBlockNumber(buf), blk);
		START_CRIT_SECTION();
		if (blk == 0)
			meta_page_init(BufferGetPage(buf));
		else
			map_page_init(BufferGetPage(buf));
		MarkBufferDirty(buf);
		log_newpage_buffer(buf, true);
		END_CRIT_SECTION();
		UnlockReleaseBuffer(buf);
	}
	LockRelease(&tag, AccessExclusiveLock, false);
	exists = true;
}

/* ------------------------------------------------------------------------
 * The map
 * ------------------------------------------------------------------------
 */

static uint32
hash_key(const uint8 *hash)
{
	uint32		k;

	memcpy(&k, hash, sizeof(k));
	return pg_ntoh32(k);
}

static uint32
bucket_for(uint32 key, uint32 maxbucket, uint32 lowmask, uint32 highmask)
{
	uint32		b = key & highmask;

	return b > maxbucket ? b & lowmask : b;
}

static void
check_hash(const ProllyHash *hash)
{
	if (hash->len != MAP_HASH_LEN)
		elog(ERROR, "prolly node store expects %d-byte hashes, got %d",
			 MAP_HASH_LEN, hash->len);
}

static void
meta_check(MapMeta *meta)
{
	if (meta->magic != MAP_MAGIC || meta->version != MAP_VERSION ||
		meta->hash_len != MAP_HASH_LEN)
		elog(ERROR, "prolly node store map has bad metapage "
			 "(magic %08X, version %u, hash length %u)",
			 meta->magic, meta->version, meta->hash_len);
}

/*
 * Find hash in the bucket chain starting at primary, whose content lock the
 * caller holds.  Returns the block, or InvalidBlockNumber.  If page and slot
 * are given, they get the buffer (still locked in mode, caller releases) and
 * index of the entry; a buffer other than primary is otherwise released.
 */
static BlockNumber
chain_find(Buffer primary, const uint8 *hash, BlockNumber want_block,
		   int mode, Buffer *page_out, int *slot_out)
{
	Buffer		buf = primary;

	for (;;)
	{
		Page		page = BufferGetPage(buf);
		int			n = opaque_of(page)->nentries;
		BlockNumber next = opaque_of(page)->next;

		for (int i = 0; i < n; i++)
		{
			char	   *e = entry_at(page, i);
			BlockNumber blk;

			if (memcmp(e, hash, MAP_HASH_LEN) != 0)
				continue;
			memcpy(&blk, e + MAP_HASH_LEN, sizeof(blk));
			if (BlockNumberIsValid(want_block) && blk != want_block)
				continue;
			if (page_out)
			{
				*page_out = buf;
				*slot_out = i;
			}
			else if (buf != primary)
				UnlockReleaseBuffer(buf);
			return blk;
		}
		if (buf != primary)
			UnlockReleaseBuffer(buf);
		if (!BlockNumberIsValid(next))
			return InvalidBlockNumber;
		buf = store_read(STORE_OVERFLOW, next, mode);
	}
}

static BlockNumber
map_lookup(const ProllyHash *hash)
{
	Buffer		metabuf = store_read(STORE_MAP, 0, BUFFER_LOCK_SHARE);
	MapMeta    *meta = meta_of(BufferGetPage(metabuf));
	Buffer		primary;
	BlockNumber blk;
	uint32		bucket;

	meta_check(meta);
	bucket = bucket_for(hash_key(hash->bytes), meta->maxbucket,
						meta->lowmask, meta->highmask);
	primary = store_read(STORE_MAP, bucket + 1, BUFFER_LOCK_SHARE);
	blk = chain_find(primary, hash->bytes, InvalidBlockNumber,
					 BUFFER_LOCK_SHARE, NULL, NULL);
	UnlockReleaseBuffer(primary);
	UnlockReleaseBuffer(metabuf);
	return blk;
}

static void
log_header(xl_prolly_header *hdr)
{
	hdr->impl_id = STORE_IMPL_ID;
	XLogRegisterData(hdr, SizeOfProllyHeader);
}

static void
page_append_entry(Page page, const uint8 *hash, BlockNumber blk)
{
	int			n = opaque_of(page)->nentries;
	char	   *e = entry_at(page, n);

	memcpy(e, hash, MAP_HASH_LEN);
	memcpy(e + MAP_HASH_LEN, &blk, sizeof(blk));
	map_page_set_count(page, n + 1);
}

static void
page_remove_entry(Page page, int slot)
{
	int			n = opaque_of(page)->nentries;

	if (slot != n - 1)
		memcpy(entry_at(page, slot), entry_at(page, n - 1), MAP_ENTRY_SIZE);
	map_page_set_count(page, n - 1);
}

static void map_split(void);

/*
 * Add hash -> blk unless hash is already mapped.  Returns true if it was
 * added.
 */
static bool
map_insert(const ProllyHash *hash, BlockNumber blk)
{
	Buffer		metabuf = store_read(STORE_MAP, 0, BUFFER_LOCK_SHARE);
	MapMeta    *meta = meta_of(BufferGetPage(metabuf));
	uint32		bucket;
	Buffer		primary;
	Buffer		buf;
	int			capacity = page_capacity();
	bool		grew = false;
	uint8		entry[MAP_ENTRY_SIZE];
	xl_prolly_header hdr;

	meta_check(meta);
	bucket = bucket_for(hash_key(hash->bytes), meta->maxbucket,
						meta->lowmask, meta->highmask);
	primary = store_read(STORE_MAP, bucket + 1, BUFFER_LOCK_EXCLUSIVE);

	if (BlockNumberIsValid(chain_find(primary, hash->bytes, InvalidBlockNumber,
									  BUFFER_LOCK_SHARE, NULL, NULL)))
	{
		UnlockReleaseBuffer(primary);
		UnlockReleaseBuffer(metabuf);
		return false;
	}

	memcpy(entry, hash->bytes, MAP_HASH_LEN);
	memcpy(entry + MAP_HASH_LEN, &blk, sizeof(blk));

	/* Find the first page in the chain with room. */
	buf = primary;
	while (opaque_of(BufferGetPage(buf))->nentries >= capacity &&
		   BlockNumberIsValid(opaque_of(BufferGetPage(buf))->next))
	{
		BlockNumber next = opaque_of(BufferGetPage(buf))->next;

		if (buf != primary)
			UnlockReleaseBuffer(buf);
		buf = store_read(STORE_OVERFLOW, next, BUFFER_LOCK_EXCLUSIVE);
	}

	if (opaque_of(BufferGetPage(buf))->nentries < capacity)
	{
		Page		page = BufferGetPage(buf);
		XLogRecPtr	recptr;

		START_CRIT_SECTION();
		page_append_entry(page, hash->bytes, blk);
		MarkBufferDirty(buf);
		XLogBeginInsert();
		log_header(&hdr);
		XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
		XLogRegisterBufData(0, entry, MAP_ENTRY_SIZE);
		recptr = XLogInsert(RM_PROLLY_STORE_ID, XLOG_STORE_MAP_INSERT);
		PageSetLSN(page, recptr);
		END_CRIT_SECTION();
	}
	else
	{
		/* buf is the last page in the chain, and full: add a page. */
		Buffer		newbuf = store_extend(STORE_OVERFLOW);
		BlockNumber newblk = BufferGetBlockNumber(newbuf);
		XLogRecPtr	recptr;

		START_CRIT_SECTION();
		map_page_init(BufferGetPage(newbuf));
		page_append_entry(BufferGetPage(newbuf), hash->bytes, blk);
		opaque_of(BufferGetPage(buf))->next = newblk;
		MarkBufferDirty(newbuf);
		MarkBufferDirty(buf);
		XLogBeginInsert();
		log_header(&hdr);
		XLogRegisterBuffer(0, newbuf, REGBUF_WILL_INIT | REGBUF_STANDARD);
		XLogRegisterBufData(0, entry, MAP_ENTRY_SIZE);
		XLogRegisterBuffer(1, buf, REGBUF_STANDARD);
		XLogRegisterBufData(1, &newblk, sizeof(newblk));
		recptr = XLogInsert(RM_PROLLY_STORE_ID, XLOG_STORE_MAP_INSERT_NEW);
		PageSetLSN(BufferGetPage(newbuf), recptr);
		PageSetLSN(BufferGetPage(buf), recptr);
		END_CRIT_SECTION();
		UnlockReleaseBuffer(newbuf);
		grew = true;
	}

	if (buf != primary)
		UnlockReleaseBuffer(buf);
	UnlockReleaseBuffer(primary);
	UnlockReleaseBuffer(metabuf);

	if (grew)
		map_split();
	return true;
}

/* Remove hash's entries (only the one for want_block, if valid). */
static void
map_delete(const ProllyHash *hash, BlockNumber want_block)
{
	Buffer		metabuf = store_read(STORE_MAP, 0, BUFFER_LOCK_SHARE);
	MapMeta    *meta = meta_of(BufferGetPage(metabuf));
	uint32		bucket;
	Buffer		primary;

	meta_check(meta);
	bucket = bucket_for(hash_key(hash->bytes), meta->maxbucket,
						meta->lowmask, meta->highmask);
	primary = store_read(STORE_MAP, bucket + 1, BUFFER_LOCK_EXCLUSIVE);

	for (;;)
	{
		Buffer		buf;
		int			slot;
		uint16		slot16;
		xl_prolly_header hdr;
		XLogRecPtr	recptr;

		if (!BlockNumberIsValid(chain_find(primary, hash->bytes, want_block,
										   BUFFER_LOCK_EXCLUSIVE, &buf, &slot)))
			break;

		slot16 = (uint16) slot;
		START_CRIT_SECTION();
		page_remove_entry(BufferGetPage(buf), slot);
		MarkBufferDirty(buf);
		XLogBeginInsert();
		log_header(&hdr);
		XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
		XLogRegisterBufData(0, &slot16, sizeof(slot16));
		recptr = XLogInsert(RM_PROLLY_STORE_ID, XLOG_STORE_MAP_DELETE);
		PageSetLSN(BufferGetPage(buf), recptr);
		END_CRIT_SECTION();
		if (buf != primary)
			UnlockReleaseBuffer(buf);
	}
	UnlockReleaseBuffer(primary);
	UnlockReleaseBuffer(metabuf);
}

/*
 * Split the next bucket in linear-hashing order.  Called after an insert
 * added an overflow page.  Skipped (the chain just stays longer) if the
 * split would touch more pages than one WAL record can carry.
 */
static void
map_split(void)
{
	Buffer		metabuf = store_read(STORE_MAP, 0, BUFFER_LOCK_EXCLUSIVE);
	Page		metapage = BufferGetPage(metabuf);
	MapMeta    *meta = meta_of(metapage);
	uint32		new_bucket = meta->maxbucket + 1;
	uint32		lowmask = meta->lowmask;
	uint32		highmask = meta->highmask;
	uint32		old_bucket;
	Buffer		pages[MAX_SPLIT_PAGES];
	int			nold = 0;
	int			nnew_pages;
	int			nold_pages;
	int			npages;
	int			capacity = page_capacity();
	char	   *keep;
	char	   *move;
	int			nkeep = 0;
	int			nmove = 0;
	BlockNumber freed = meta->free_overflow;
	xl_prolly_header hdr;
	XLogRecPtr	recptr;

	meta_check(meta);
	if (new_bucket > highmask)
	{
		lowmask = highmask;
		highmask = (highmask << 1) | 1;
	}
	old_bucket = new_bucket & lowmask;

	/* The old bucket's chain, exclusive-locked. */
	pages[nold++] = store_read(STORE_MAP, old_bucket + 1, BUFFER_LOCK_EXCLUSIVE);
	while (BlockNumberIsValid(opaque_of(BufferGetPage(pages[nold - 1]))->next))
	{
		if (nold == MAX_SPLIT_PAGES / 2)
		{
			for (int i = 0; i < nold; i++)
				UnlockReleaseBuffer(pages[i]);
			UnlockReleaseBuffer(metabuf);
			elog(DEBUG1, "prolly map: bucket %u chain too long to split", old_bucket);
			return;
		}
		pages[nold] = store_read(STORE_OVERFLOW,
								 opaque_of(BufferGetPage(pages[nold - 1]))->next,
								 BUFFER_LOCK_EXCLUSIVE);
		nold++;
	}

	/* Partition the entries. */
	keep = palloc(nold * capacity * MAP_ENTRY_SIZE);
	move = palloc(nold * capacity * MAP_ENTRY_SIZE);
	for (int p = 0; p < nold; p++)
	{
		Page		page = BufferGetPage(pages[p]);

		for (int i = 0; i < opaque_of(page)->nentries; i++)
		{
			char	   *e = entry_at(page, i);

			if (bucket_for(hash_key((uint8 *) e), new_bucket, lowmask, highmask) ==
				new_bucket)
				memcpy(move + MAP_ENTRY_SIZE * nmove++, e, MAP_ENTRY_SIZE);
			else
				memcpy(keep + MAP_ENTRY_SIZE * nkeep++, e, MAP_ENTRY_SIZE);
		}
	}
	nold_pages = Max(1, (nkeep + capacity - 1) / capacity);
	nnew_pages = Max(1, (nmove + capacity - 1) / capacity);

	/*
	 * Pages for the new chain: the new primary page (next block of the map
	 * file), then old overflow pages no longer needed, then new ones.
	 */
	npages = nold;
	{
		Buffer		newprimary = store_extend(STORE_MAP);

		if (BufferGetBlockNumber(newprimary) != new_bucket + 1)
			elog(ERROR, "prolly map extended to block %u, expected %u",
				 BufferGetBlockNumber(newprimary), new_bucket + 1);
		pages[npages++] = newprimary;
	}
	for (int i = 1; i < nnew_pages; i++)
	{
		if (npages >= MAX_SPLIT_PAGES)
			elog(ERROR, "prolly map split needs too many pages");
		/* overflow pages beyond what the old chain keeps are reused below */
		pages[npages++] = InvalidBuffer;
	}

	/* Assign overflow pages for the new chain: freed old pages first. */
	{
		int			spare = nold_pages;	/* first old page not kept */

		for (int i = nold + 1; i < npages; i++)
		{
			if (spare < nold)
			{
				pages[i] = pages[spare];
				pages[spare] = InvalidBuffer;
				spare++;
			}
			else
				pages[i] = store_extend(STORE_OVERFLOW);
		}
	}

	XLogEnsureRecordSpace(npages + 1, 0);
	START_CRIT_SECTION();

	/* Rewrite the old chain. */
	for (int p = 0; p < nold_pages; p++)
	{
		Page		page = BufferGetPage(pages[p]);
		BlockNumber next = opaque_of(page)->next;
		int			n = Min(capacity, nkeep - p * capacity);

		map_page_init(page);
		if (n > 0)
			memcpy(entry_at(page, 0), keep + p * capacity * MAP_ENTRY_SIZE,
				   n * MAP_ENTRY_SIZE);
		map_page_set_count(page, Max(n, 0));
		opaque_of(page)->next = (p == nold_pages - 1) ? InvalidBlockNumber : next;
	}

	/* Old pages left over go on the free list. */
	for (int p = nold_pages; p < nold; p++)
	{
		Page		page;

		if (!BufferIsValid(pages[p]))
			continue;
		page = BufferGetPage(pages[p]);
		map_page_init(page);
		opaque_of(page)->next = freed;
		freed = BufferGetBlockNumber(pages[p]);
	}

	/* The new chain. */
	for (int i = 0; i < nnew_pages; i++)
	{
		Page		page = BufferGetPage(pages[nold + i]);
		int			n = Min(capacity, nmove - i * capacity);

		map_page_init(page);
		if (n > 0)
			memcpy(entry_at(page, 0), move + i * capacity * MAP_ENTRY_SIZE,
				   n * MAP_ENTRY_SIZE);
		map_page_set_count(page, Max(n, 0));
		opaque_of(page)->next = (i == nnew_pages - 1) ? InvalidBlockNumber :
			BufferGetBlockNumber(pages[nold + i + 1]);
	}

	meta->maxbucket = new_bucket;
	meta->lowmask = lowmask;
	meta->highmask = highmask;
	meta->free_overflow = freed;

	MarkBufferDirty(metabuf);
	for (int i = 0; i < npages; i++)
	{
		if (BufferIsValid(pages[i]))
			MarkBufferDirty(pages[i]);
	}

	XLogBeginInsert();
	log_header(&hdr);
	XLogRegisterBuffer(0, metabuf, REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
	{
		int			id = 1;

		for (int i = 0; i < npages; i++)
		{
			if (BufferIsValid(pages[i]))
				XLogRegisterBuffer(id++, pages[i], REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
		}
	}
	recptr = XLogInsert(RM_PROLLY_STORE_ID, XLOG_STORE_MAP_SPLIT);
	PageSetLSN(metapage, recptr);
	for (int i = 0; i < npages; i++)
	{
		if (BufferIsValid(pages[i]))
			PageSetLSN(BufferGetPage(pages[i]), recptr);
	}
	END_CRIT_SECTION();

	for (int i = 0; i < npages; i++)
	{
		if (BufferIsValid(pages[i]))
			UnlockReleaseBuffer(pages[i]);
	}
	UnlockReleaseBuffer(metabuf);
	pfree(keep);
	pfree(move);
}

/* ------------------------------------------------------------------------
 * Callbacks
 * ------------------------------------------------------------------------
 */

static void
global_create(Relation rel, const RelFileLocator *rlocator, char persistence)
{
	if (persistence != RELPERSISTENCE_PERMANENT)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("prolly storage supports only permanent tables")));
	ensure_store();
}

static ProllyNodeStore *
global_open(Relation rel)
{
	ensure_store();
	return palloc0(sizeof(ProllyNodeStore));
}

static void
global_close(ProllyNodeStore *store)
{
	pfree(store);
}

static Buffer
global_read(ProllyNodeStore *store, const ProllyNodeRef *ref)
{
	BlockNumber blk = ref->location.blkno;

	if (!BlockNumberIsValid(blk))
	{
		check_hash(&ref->hash);
		blk = map_lookup(&ref->hash);
		if (!BlockNumberIsValid(blk))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("prolly node not found in the node store")));
	}
	return ReadBufferWithoutRelcache(store_locator(STORE_NODES), MAIN_FORKNUM,
									 blk, RBM_NORMAL, NULL, true);
}

static Buffer
global_new_node(ProllyNodeStore *store, ProllyNodeRef *ref)
{
	Buffer		buf = store_extend(STORE_NODES);

	memset(ref, 0, sizeof(*ref));
	ref->location.blkno = BufferGetBlockNumber(buf);
	return buf;
}

static void
global_publish(ProllyNodeStore *store, Buffer buf, const ProllyHash *hash,
			   ProllyNodeRef *ref)
{
	check_hash(hash);
	(void) map_insert(hash, BufferGetBlockNumber(buf));
	ref->hash = *hash;
	ref->location.blkno = BufferGetBlockNumber(buf);
}

static bool
global_contains(ProllyNodeStore *store, const ProllyHash *hash,
				ProllyExtent *location)
{
	BlockNumber blk;

	check_hash(hash);
	blk = map_lookup(hash);
	if (!BlockNumberIsValid(blk))
		return false;
	if (location)
	{
		memset(location, 0, sizeof(*location));
		location->blkno = blk;
	}
	return true;
}

static void
global_release(ProllyNodeStore *store, const ProllyNodeRef *ref)
{
	check_hash(&ref->hash);
	map_delete(&ref->hash, ref->location.blkno);
}

static void
global_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & PROLLY_XLOG_TYPE_MASK;
	Buffer		buf;
	char	   *data;
	Size		len;

	switch (info)
	{
		case XLOG_STORE_MAP_INSERT:
			if (XLogReadBufferForRedo(record, 0, &buf) == BLK_NEEDS_REDO)
			{
				BlockNumber entry_blk;

				data = XLogRecGetBlockData(record, 0, &len);
				Assert(len == MAP_ENTRY_SIZE);
				memcpy(&entry_blk, data + MAP_HASH_LEN, sizeof(entry_blk));
				page_append_entry(BufferGetPage(buf), (uint8 *) data, entry_blk);
				PageSetLSN(BufferGetPage(buf), record->EndRecPtr);
				MarkBufferDirty(buf);
			}
			if (BufferIsValid(buf))
				UnlockReleaseBuffer(buf);
			break;

		case XLOG_STORE_MAP_INSERT_NEW:
			{
				BlockNumber entry_blk;

				buf = XLogInitBufferForRedo(record, 0);
				data = XLogRecGetBlockData(record, 0, &len);
				memcpy(&entry_blk, data + MAP_HASH_LEN, sizeof(entry_blk));
				map_page_init(BufferGetPage(buf));
				page_append_entry(BufferGetPage(buf), (uint8 *) data, entry_blk);
				PageSetLSN(BufferGetPage(buf), record->EndRecPtr);
				MarkBufferDirty(buf);
				UnlockReleaseBuffer(buf);

				if (XLogReadBufferForRedo(record, 1, &buf) == BLK_NEEDS_REDO)
				{
					BlockNumber next;

					data = XLogRecGetBlockData(record, 1, &len);
					memcpy(&next, data, sizeof(next));
					opaque_of(BufferGetPage(buf))->next = next;
					PageSetLSN(BufferGetPage(buf), record->EndRecPtr);
					MarkBufferDirty(buf);
				}
				if (BufferIsValid(buf))
					UnlockReleaseBuffer(buf);
				break;
			}

		case XLOG_STORE_MAP_DELETE:
			if (XLogReadBufferForRedo(record, 0, &buf) == BLK_NEEDS_REDO)
			{
				uint16		slot;

				data = XLogRecGetBlockData(record, 0, &len);
				memcpy(&slot, data, sizeof(slot));
				page_remove_entry(BufferGetPage(buf), slot);
				PageSetLSN(BufferGetPage(buf), record->EndRecPtr);
				MarkBufferDirty(buf);
			}
			if (BufferIsValid(buf))
				UnlockReleaseBuffer(buf);
			break;

		case XLOG_STORE_MAP_SPLIT:
			for (int id = 0; id <= XLogRecMaxBlockId(record); id++)
			{
				if (XLogReadBufferForRedo(record, id, &buf) != BLK_RESTORED)
					elog(PANIC, "prolly map split record block %d has no image", id);
				UnlockReleaseBuffer(buf);
			}
			break;

		default:
			elog(PANIC, "prolly node store: unknown record type 0x%02X", info);
	}
}

const ProllyNodeStoreRoutine prolly_nodestore_global = {
	.info = {
		.iface = PROLLY_IFACE_NODE_STORE,
		.impl_id = STORE_IMPL_ID,
		.format_version = 1,
		.name = "global",
		.description = "one cluster-wide store; nodes found by a linear-hashing "
		"hash -> block map",
	},
	.create = global_create,
	.open = global_open,
	.close = global_close,
	.read = global_read,
	.new_node = global_new_node,
	.publish = global_publish,
	.contains = global_contains,
	.release = global_release,
	.redo = global_redo,
};
