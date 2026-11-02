/*-------------------------------------------------------------------------
 *
 * chunk_tier.c
 *	  The history store's tier (prolly/chunk_tier.h): a chunk log whose
 *	  pages are the catalog of its packs, and whose bytes are in them.
 *
 *	block 0		the meta page: the next pack's number, the log's stamp (part
 *				of its packs' names, so that a log made afresh names none
 *				of another's), its owner, and the index bound to it
 *	blocks 1-	the packs, by number, an entry each: the transaction that
 *				made it, its owner, its size once written, and the bytes of
 *				it dead
 *
 * Changes of the catalog are WAL-logged as generic records.  A pack's entry
 * is its transaction's (its top transaction's): once that commits, the
 * pack is the store's (its object written before the commit record); if
 * it aborts, or a crash cuts it short, the pack is an orphan.  The entry's
 * record is flushed before the object is written, as the WAL is before
 * the pages it describes, so the catalog has every object there may be.
 *
 * VACUUM (of the tier's index, or of the tier itself) settles each pack
 * whose transaction ended, once: committed, or orphaned; so the catalog
 * keeps no transaction's id past the tier's relfrozenxid, which it then
 * advances.  At the end of VACUUM of the index, an orphan's object is
 * removed, and so is a committed pack all of whose bytes are dead once
 * every standby connected has replayed past where they died (its readers
 * may read the pack until then).  A standby the primary does not see
 * streaming from it (cascaded, fed from an archive, or between a base
 * backup and streaming) is not waited for: a read there of a pack removed
 * fails, "not there".
 *
 * A pack is its owner's, a random number the server takes the first time
 * it begins a pack on a timeline, and its name has its owner's: so servers
 * promoted apart, on the same timeline or not, name none of each other's.
 * A server removes only its own owner's packs, and those of packs adopted
 * (pg_history_tier_adopt): packs from before a promotion, or a recovery to
 * a point in time, may be another server's still, until it is said they
 * are not.  A server that goes on as primary after another is promoted
 * (the two sharing the tier's place) is not supported: it removes its own
 * packs, which the other's catalog has too.  Nor are two servers started
 * from a copy of one's files, on its timeline: they share its owner.
 *
 * What is not removed: the objects of a tier dropped, or of its storage
 * before a TRUNCATE (their names of its stamp then), are left in the
 * store, no catalog naming them.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/chunk_tier.c
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
#include "port/pg_bswap.h"
#include "prolly/chunk_store.h"
#include "prolly/chunk_tier.h"
#include "prolly/object_store.h"
#include "storage/bufmgr.h"
#include "storage/bulk_write.h"
#include "storage/lmgr.h"
#include "replication/walsender.h"
#include "replication/walsender_private.h"
#include "storage/procarray.h"
#include "utils/injection_point.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/fmgrprotos.h"
#include "utils/rel.h"

char	   *history_tier_store = NULL;
char	   *history_tier_location = NULL;

#define TIER_MAGIC			0x54494552	/* "TIER" */
#define TIER_VERSION		3
#define TIER_META_BLOCK		0

typedef struct TierMeta
{
	uint32		magic;
	uint32		version;
	uint32		next_pack;		/* the next pack's number, from 1 */
	TimeLineID	owner_tli;		/* the timeline owner was taken on (0: none) */
	uint64		stamp;			/* made with the storage */
	uint64		owner;			/* the packs this server begins are its */
	Oid			index;			/* the chunk index bound to it */
	uint32		adopted;		/* packs numbered below are this server's */
} TierMeta;

/* a pack's states */
#define PACK_MADE			1	/* its transaction's, running or not settled */
#define PACK_REMOVED		2	/* its object gone, or never written */
#define PACK_COMMITTED		3	/* the store's: its transaction committed */
#define PACK_ORPHANED		4	/* its transaction aborted, or a crash cut it */

typedef struct TierPack
{
	uint32		state;
	TransactionId xid;			/* the transaction that made it */
	uint64		owner;			/* the server's that made it */
	uint64		size;			/* its bytes, once written */
	uint64		dead;			/* of them, no chunk's any more */
	XLogRecPtr	dead_lsn;		/* where the last of them died */
} TierPack;

#define PACKS_PER_PAGE \
	((BLCKSZ - MAXALIGN(SizeOfPageHeaderData)) / sizeof(TierPack))
#define PACK_BLOCK(id)		((BlockNumber) (1 + ((id) - 1) / PACKS_PER_PAGE))
#define PACK_SLOT(id)		((int) (((id) - 1) % PACKS_PER_PAGE))

#define LOC_PACK(loc)		((uint32) ((loc) >> 32))
#define LOC_OFF(loc)		((uint32) ((loc) & 0xFFFFFFFF))
#define PACK_LOC(id, off)	(((uint64) (id) << 32) | (uint64) (off))

/*
 * This backend's pack, its transaction's chunks gathered, written as the
 * transaction commits (or once full)
 */
/* a chunk reserved in this backend's pack: where, and if written */
typedef struct TierSlot
{
	uint32		off;
	uint32		len;
	bool		written;
} TierSlot;

/* the room a pack's table and footer take, of n entries */
#define TABLE_ROOM(n) \
	((uint64) (n) * sizeof(ChunkTierPackEntry) + sizeof(ChunkTierPackFooter))

typedef struct TierPending
{
	uint32		id;				/* 0: none */
	Oid			relid;
	RelFileNumber relnumber;
	uint64		stamp;
	uint64		owner;
	char	   *buf;
	uint64		len;
	uint64		cap;
	TierSlot   *slots;			/* the chunks reserved, for the pack's table */
	int			nslots;
	int			slots_cap;
	uint64		table_len;		/* the table's and footer's, once appended */
} TierPending;

static TierPending pending;
static bool callback_registered = false;
static bool past_pre_commit = false;	/* this transaction's packs written */

/* ------------------------------------------------------------ pages */

static TierMeta *
tier_meta(Relation rel, Page page)
{
	TierMeta   *meta = (TierMeta *) PageGetContents(page);

	if (PageIsNew(page) || meta->magic != TIER_MAGIC)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("\"%s\" is not a tier of the history store",
						RelationGetRelationName(rel))));
	if (meta->version != TIER_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("tier \"%s\" has format version %u, not %u",
						RelationGetRelationName(rel), meta->version, TIER_VERSION)));
	return meta;
}

void
ChunkTierInitStorage(SMgrRelation srel, bool use_wal)
{
	BulkWriteState *bulk = smgr_bulk_start_smgr(srel, MAIN_FORKNUM, use_wal);
	BulkWriteBuffer buf = smgr_bulk_get_buf(bulk);
	Page		page = (Page) buf;
	TierMeta   *meta;

	PageInit(page, BLCKSZ, 0);
	meta = (TierMeta *) PageGetContents(page);
	memset(meta, 0, sizeof(*meta));
	meta->magic = TIER_MAGIC;
	meta->version = TIER_VERSION;
	meta->next_pack = 1;
	meta->index = InvalidOid;
	if (!pg_strong_random(&meta->stamp, sizeof(meta->stamp)))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not make a tier's stamp")));
	((PageHeader) page)->pd_lower = (char *) meta + sizeof(TierMeta) - (char *) page;
	smgr_bulk_write(bulk, TIER_META_BLOCK, buf, true);
	smgr_bulk_finish(bulk);
}

/* the meta page's copy, read under a share lock */
static TierMeta
read_meta(Relation rel)
{
	Buffer		buf = ReadBuffer(rel, TIER_META_BLOCK);
	TierMeta	meta;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	meta = *tier_meta(rel, BufferGetPage(buf));
	UnlockReleaseBuffer(buf);
	return meta;
}

/* pack id's entry's page, locked in mode */
static Buffer
pack_buffer(Relation rel, uint32 id, int mode)
{
	BlockNumber blk = PACK_BLOCK(id);
	Buffer		buf;

	if (blk >= RelationGetNumberOfBlocks(rel))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("tier \"%s\" has no pack %u", RelationGetRelationName(rel), id)));
	buf = ReadBuffer(rel, blk);
	LockBuffer(buf, mode);
	if (PageIsNew(BufferGetPage(buf)))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("tier \"%s\" has no pack %u", RelationGetRelationName(rel), id)));
	return buf;
}

static inline TierPack *
pack_entry(Page page, uint32 id)
{
	return &((TierPack *) PageGetContents(page))[PACK_SLOT(id)];
}

/* a pack's object's name: the log's stamp, its owner, its number */
static char *
pack_name(uint64 stamp, uint64 owner, uint32 id)
{
	return psprintf("packs/%016llx/%016llx/%u", (unsigned long long) stamp,
					(unsigned long long) owner, id);
}

/*
 * The store the tier keeps its packs in, opened once a backend (again if
 * the settings change)
 */
static ObjectStore *
tier_objects(void)
{
	static ObjectStore *os = NULL;
	static char *os_store = NULL;
	static char *os_location = NULL;
	MemoryContext old;

	if (history_tier_location == NULL || history_tier_location[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("the history store's tier has no place set"),
				 errhint("Set history_tier_location.")));
	if (os != NULL && strcmp(os_store, history_tier_store) == 0 &&
		strcmp(os_location, history_tier_location) == 0)
		return os;
	old = MemoryContextSwitchTo(TopMemoryContext);
	if (os != NULL)
	{
		os->routine->close(os);
		pfree(os_store);
		pfree(os_location);
		os = NULL;
	}
	os = ObjectStoreOpen(history_tier_store, history_tier_location);
	os_store = pstrdup(history_tier_store);
	os_location = pstrdup(history_tier_location);
	MemoryContextSwitchTo(old);
	return os;
}

/* the log's stamp, kept in its relcache entry */
static uint64
cached_stamp(Relation rel)
{
	if (rel->rd_amcache == NULL)
	{
		uint64	   *stamp = MemoryContextAlloc(CacheMemoryContext, sizeof(uint64));

		*stamp = read_meta(rel).stamp;
		rel->rd_amcache = stamp;
	}
	return *(uint64 *) rel->rd_amcache;
}

/* whether a pack is the store's: committed, or this transaction's */
static bool
pack_live(const TierPack *p)
{
	return p->state == PACK_COMMITTED ||
		(p->state == PACK_MADE && TransactionIdIsCurrentTransactionId(p->xid));
}

/* ------------------------------------------------------------ packs */

static void tier_xact_callback(XactEvent event, void *arg);

/* the pack begun: a number from the meta page, its entry this transaction's */
static void
begin_pack(Relation rel)
{
	Buffer		mbuf = ReadBuffer(rel, TIER_META_BLOCK);
	Buffer		pbuf;
	GenericXLogState *state;
	Page		mpage;
	Page		ppage;
	TierMeta   *meta;
	uint32		id;
	BlockNumber blk;
	TierPack   *p;
	bool		fresh;
	uint64		stamp;
	uint64		owner;
	TimeLineID	tli = GetWALInsertionTimeLine();
	/* (the top transaction's: a subtransaction's rollback leaves its pack) */
	TransactionId xid = GetTopTransactionId();

	/* (taken before the locks: a new owner, if this timeline has none) */
	if (!pg_strong_random(&owner, sizeof(owner)))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not make a tier's owner")));
	LockBuffer(mbuf, BUFFER_LOCK_EXCLUSIVE);
	meta = tier_meta(rel, BufferGetPage(mbuf));
	id = meta->next_pack;
	if (id == PG_UINT32_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("tier \"%s\" has as many packs as it may", RelationGetRelationName(rel))));
	blk = PACK_BLOCK(id);

	/* (a page of entries new: numbers are taken in order, under the meta's lock) */
	fresh = blk >= RelationGetNumberOfBlocks(rel);
	if (fresh)
	{
		pbuf = ExtendBufferedRel(BMR_REL(rel), MAIN_FORKNUM, NULL, EB_LOCK_FIRST);
		if (BufferGetBlockNumber(pbuf) != blk)
			elog(ERROR, "tier \"%s\" extended to block %u, not %u",
				 RelationGetRelationName(rel), BufferGetBlockNumber(pbuf), blk);
	}
	else
	{
		pbuf = ReadBuffer(rel, blk);
		LockBuffer(pbuf, BUFFER_LOCK_EXCLUSIVE);

		/*
		 * (one the file was extended by, and a crash before its record:
		 * made now)
		 */
		fresh = PageIsNew(BufferGetPage(pbuf));
	}
	INJECTION_POINT("chunk-tier-page-extended", NULL);

	state = GenericXLogStart(rel);
	mpage = GenericXLogRegisterBuffer(state, mbuf, 0);
	ppage = GenericXLogRegisterBuffer(state, pbuf, fresh ? GENERIC_XLOG_FULL_IMAGE : 0);
	if (fresh)
	{
		PageInit(ppage, BLCKSZ, 0);
		((PageHeader) ppage)->pd_lower =
			MAXALIGN(SizeOfPageHeaderData) + PACKS_PER_PAGE * sizeof(TierPack);
		memset(PageGetContents(ppage), 0, PACKS_PER_PAGE * sizeof(TierPack));
	}
	meta = (TierMeta *) PageGetContents(mpage);
	meta->next_pack = id + 1;
	/* (the first pack begun on this timeline: this server's own owner) */
	if (meta->owner_tli != tli)
	{
		meta->owner = owner;
		meta->owner_tli = tli;
	}
	p = pack_entry(ppage, id);
	p->state = PACK_MADE;
	p->xid = xid;
	p->owner = meta->owner;
	p->size = 0;
	p->dead = 0;
	p->dead_lsn = InvalidXLogRecPtr;
	stamp = meta->stamp;
	owner = meta->owner;
	GenericXLogFinish(state);
	UnlockReleaseBuffer(pbuf);
	UnlockReleaseBuffer(mbuf);

	pending.id = id;
	pending.relid = RelationGetRelid(rel);
	pending.relnumber = rel->rd_locator.relNumber;
	pending.stamp = stamp;
	pending.owner = owner;
	pending.len = 0;
}

static void
forget_pending(void)
{
	if (pending.buf)
		pfree(pending.buf);
	if (pending.slots)
		pfree(pending.slots);
	memset(&pending, 0, sizeof(pending));
}

static int
entry_cmp(const void *a, const void *b)
{
	const ChunkTierPackEntry *x = a;
	const ChunkTierPackEntry *y = b;
	int			c = memcmp(x->hash, y->hash, CHUNK_HASH_LEN);

	if (c != 0)
		return c;
	return (x->off > y->off) - (x->off < y->off);
}

/*
 * The pack's table appended to its bytes: each chunk reserved and written,
 * by hash (each re-hashed here, from the bytes going out), then the
 * footer; numbers in network byte order.  (Its room is prepared.)
 */
static void
append_table(void)
{
	ChunkTierPackEntry *entries = palloc_array(ChunkTierPackEntry, Max(pending.nslots, 1));
	ChunkTierPackFooter footer;
	int			n = 0;
	char	   *at;

	for (int i = 0; i < pending.nslots; i++)
	{
		if (!pending.slots[i].written)
			continue;
		ChunkHash(pending.buf + pending.slots[i].off, pending.slots[i].len, entries[n].hash);
		entries[n].off = pending.slots[i].off;
		entries[n].len = pending.slots[i].len;
		n++;
	}
	qsort(entries, n, sizeof(ChunkTierPackEntry), entry_cmp);
	Assert(pending.len + TABLE_ROOM(n) <= pending.cap);
	at = pending.buf + pending.len;
	for (int i = 0; i < n; i++)
	{
		ChunkTierPackEntry e = entries[i];

		e.off = pg_hton32(e.off);
		e.len = pg_hton32(e.len);
		memcpy(at, &e, sizeof(e));
		at += sizeof(e);
	}
	memset(&footer, 0, sizeof(footer));
	footer.nentries = pg_hton32(n);
	footer.version = pg_hton32(CHUNK_TIER_PACK_VERSION);
	footer.data_len = pg_hton64(pending.len);
	footer.magic = pg_hton64(CHUNK_TIER_PACK_MAGIC);
	memcpy(at, &footer, sizeof(footer));
	pending.table_len = TABLE_ROOM(n);
	pfree(entries);
}

int
ChunkTierReadPackTable(ObjectStore *os, const char *name,
					   ChunkTierPackEntry **entries, uint64 *data_len)
{
	uint64		size;
	ChunkTierPackFooter footer;
	uint32		n;
	uint64		len;
	ChunkTierPackEntry *e;

	if (!os->routine->size(os, name, &size))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("pack \"%s\" is not there", name)));
	if (size < sizeof(footer))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("pack \"%s\" is %llu bytes, too few for its footer",
						name, (unsigned long long) size)));
	os->routine->read(os, name, size - sizeof(footer), &footer, sizeof(footer));
	if (pg_ntoh64(footer.magic) != CHUNK_TIER_PACK_MAGIC)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("\"%s\" is not a pack", name)));
	if (pg_ntoh32(footer.version) != CHUNK_TIER_PACK_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pack \"%s\" has format version %u, not %u",
						name, pg_ntoh32(footer.version), CHUNK_TIER_PACK_VERSION)));
	n = pg_ntoh32(footer.nentries);
	len = pg_ntoh64(footer.data_len);
	/* (each part checked against what is left: no sum to wrap) */
	if (len > size - sizeof(footer) ||
		(uint64) n > (size - sizeof(footer) - len) / sizeof(ChunkTierPackEntry) ||
		(uint64) n * sizeof(ChunkTierPackEntry) != size - sizeof(footer) - len)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("pack \"%s\" is %llu bytes, which its footer's %u entries and %llu bytes of chunks do not fill",
						name, (unsigned long long) size, n, (unsigned long long) len)));
	e = palloc_array(ChunkTierPackEntry, Max(n, 1));
	if (n > 0)
		os->routine->read(os, name, len, e, (size_t) n * sizeof(ChunkTierPackEntry));
	for (uint32 i = 0; i < n; i++)
	{
		e[i].off = pg_ntoh32(e[i].off);
		e[i].len = pg_ntoh32(e[i].len);
		if ((uint64) e[i].off + e[i].len > len || e[i].len == 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("pack \"%s\"'s entry %u, of bytes %u to %llu, is not in its %llu bytes of chunks",
							name, i, e[i].off, (unsigned long long) e[i].off + e[i].len,
							(unsigned long long) len)));
		if (i > 0 && entry_cmp(&e[i - 1], &e[i]) > 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("pack \"%s\"'s entries are not in order at %u", name, i)));
	}
	*entries = e;
	*data_len = len;
	return (int) n;
}

void
ChunkTierNewStorage(Relation rel)
{
	if (pending.id != 0 && pending.relid == RelationGetRelid(rel))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot truncate tier \"%s\" in a transaction that put chunks in it",
						RelationGetRelationName(rel))));
}

void
ChunkTierTruncated(Relation rel)
{
	if (pending.id != 0 && pending.relid == RelationGetRelid(rel))
		forget_pending();
}

/*
 * The pack written whole to the object store, and its size recorded; or,
 * nothing gathered in it, it is let go of, removed
 */
static void
write_pack(Relation rel)
{
	Buffer		buf = pack_buffer(rel, pending.id, BUFFER_LOCK_EXCLUSIVE);
	GenericXLogState *state;
	TierPack   *p;

	if (pending.len > 0)
	{
		ObjectStore *os = tier_objects();
		char	   *name = pack_name(pending.stamp, pending.owner, pending.id);

		/*
		 * (the entry's record flushed before the object is written, as the
		 * WAL is before the pages it describes: after a crash the catalog
		 * knows every object there may be, an orphan's for VACUUM)
		 */
		XLogFlush(PageGetLSN(BufferGetPage(buf)));

		/* (no lock held as it is written: its entry is this transaction's) */
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		append_table();
		os->routine->put(os, name, pending.buf, pending.len + pending.table_len);
		pfree(name);
		INJECTION_POINT("chunk-tier-pack-written", NULL);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	}
	state = GenericXLogStart(rel);
	p = pack_entry(GenericXLogRegisterBuffer(state, buf, 0), pending.id);
	p->size = pending.len;
	if (pending.len == 0)
		p->state = PACK_REMOVED;
	GenericXLogFinish(state);
	UnlockReleaseBuffer(buf);
	forget_pending();
}

/*
 * (as the transaction commits, its pack written before the commit record;
 * as it ends either way, the backend's copy let go of: aborted, the pack
 * is an orphan, its object, if any, VACUUM's to remove)
 */
static void
tier_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
			if (pending.id != 0)
			{
				Relation	rel = try_relation_open(pending.relid, NoLock);

				if (rel == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							 errmsg("the tier this transaction put chunks in was dropped")));
				write_pack(rel);
				relation_close(rel, NoLock);
			}
			/* (a put after, by a callback run later, would be lost: refused) */
			past_pre_commit = true;
			break;
		case XACT_EVENT_PRE_PREPARE:
			if (pending.id != 0)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot prepare a transaction that put chunks in the history store's tier")));
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PREPARE:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_PARALLEL_ABORT:
			forget_pending();
			past_pre_commit = false;
			break;
		default:
			break;
	}
}

/* ------------------------------------------------------- the routine */

static void
check_writer(Relation rel)
{
	if (!CheckRelationLockedByMe(rel, RowExclusiveLock, true))
		elog(ERROR, "tier \"%s\" appended to without RowExclusiveLock",
			 RelationGetRelationName(rel));
}

static void
tier_prepare(Relation rel, size_t len, ChunkLogRun *run)
{
	check_writer(rel);
	if (IsInParallelMode())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot put chunks in the history store's tier in parallel mode")));
	if (past_pre_commit)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot put chunks in the history store's tier as the transaction commits")));
	if (!callback_registered)
	{
		RegisterXactCallback(tier_xact_callback, NULL);
		callback_registered = true;
	}
	if (pending.id != 0 && pending.relnumber != rel->rd_locator.relNumber)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a transaction puts chunks in one tier of the history store")));

	/* (a pack full written now, and another begun) */
	if (pending.id != 0 && pending.len > 0 && pending.len + len > TIER_PACK_BYTES)
		write_pack(rel);
	if (pending.id == 0)
		begin_pack(rel);

	/*
	 * (room made now: reserve, which must not fail, takes it, and a slot
	 * for it; and room for the table, so that writing the pack copies
	 * nothing)
	 */
	if (pending.len + len + TABLE_ROOM(pending.nslots + 1) > pending.cap)
	{
		uint64		need = pending.len + len + TABLE_ROOM(pending.nslots + 1);
		uint64		cap = Max(pending.cap * 2, 1024 * 1024);

		/*
		 * (doubling, to a pack's bytes and an eighth more for its table;
		 * past that, an eighth more than is needed at once, so a table of
		 * many small chunks grows in few steps)
		 */
		cap = Min(cap, TIER_PACK_BYTES + TIER_PACK_BYTES / 8);
		cap = Max(cap, need + need / 8);
		pending.buf = pending.buf ?
			repalloc_huge(pending.buf, cap) :
			MemoryContextAllocHuge(TopMemoryContext, cap);
		pending.cap = cap;
	}
	if (pending.nslots == pending.slots_cap)
	{
		pending.slots_cap = Max(pending.slots_cap * 2, 64);
		pending.slots = pending.slots ?
			repalloc_huge(pending.slots, pending.slots_cap * sizeof(TierSlot)) :
			MemoryContextAllocHuge(TopMemoryContext, pending.slots_cap * sizeof(TierSlot));
	}
	/* (zeroed: bytes reserved and never written are no memory's) */
	memset(pending.buf + pending.len, 0, len);
	run->relnumber = rel->rd_locator.relNumber;
	run->next = PACK_LOC(pending.id, pending.len);
	run->limit = run->next + len;
	run->generation = 0;
}

static bool
tier_prepare_free(Relation rel, size_t len, uint64 below, ChunkLogRun *run)
{
	/* (no room made below a place: a tier is not evacuated) */
	if (below > 0)
	{
		memset(run, 0, sizeof(*run));
		run->relnumber = rel->rd_locator.relNumber;
		return false;
	}
	tier_prepare(rel, len, run);
	return true;
}

static uint64
tier_reserve(Relation rel, ChunkLogRun *run, size_t len)
{
	uint64		loc = run->next;

	if (len > run->limit - run->next || LOC_PACK(loc) != pending.id)
		elog(PANIC, "tier \"%s\": %zu bytes reserved past the room prepared",
			 RelationGetRelationName(rel), len);
	run->next += len;
	/* (a run is a chunk's: the next one prepares its own room, and slot) */
	run->limit = run->next;
	pending.len = Max(pending.len, (uint64) LOC_OFF(loc) + len);
	if (len > 0)
	{
		pending.slots[pending.nslots].off = LOC_OFF(loc);
		pending.slots[pending.nslots].len = len;
		pending.slots[pending.nslots].written = false;
		pending.nslots++;
	}
	return loc;
}

static void
tier_write(Relation rel, uint64 loc, const void *data, size_t len)
{
	if (LOC_PACK(loc) != pending.id || (uint64) LOC_OFF(loc) + len > pending.len)
		elog(ERROR, "tier \"%s\": bytes written at %llu where none were reserved",
			 RelationGetRelationName(rel), (unsigned long long) loc);
	memcpy(pending.buf + LOC_OFF(loc), data, len);

	/*
	 * (the chunks reserved whose bytes these are written: one write may be
	 * of several, one after another; slots are in the order of their
	 * offsets)
	 */
	{
		uint64		start = LOC_OFF(loc);
		uint64		end = start + len;
		int			lo = 0,
					hi = pending.nslots;

		while (lo < hi)
		{
			int			mid = lo + (hi - lo) / 2;

			if (pending.slots[mid].off < start)
				lo = mid + 1;
			else
				hi = mid;
		}
		for (int i = lo; i < pending.nslots && pending.slots[i].off < end; i++)
			if ((uint64) pending.slots[i].off + pending.slots[i].len <= end)
				pending.slots[i].written = true;
	}
}

static void
tier_release(Relation rel, const ChunkLogRun *run)
{
}

static bool
tier_extend(Relation rel, ChunkLogRun *run, size_t len)
{
	return run->limit - run->next >= len;
}

static uint64
tier_append(Relation rel, const void *data, size_t len)
{
	ChunkLogRun run;
	uint64		loc;

	tier_prepare(rel, len, &run);
	loc = tier_reserve(rel, &run, len);
	tier_write(rel, loc, data, len);
	return loc;
}

static void
tier_read(Relation rel, uint64 loc, void *dst, size_t len)
{
	uint32		id = LOC_PACK(loc);

	/* (an empty chunk's bytes are in no pack) */
	if (len == 0)
		return;
	/* (this transaction's pack, not yet written: its copy) */
	if (id != 0 && id == pending.id && pending.relnumber == rel->rd_locator.relNumber)
	{
		if ((uint64) LOC_OFF(loc) + len > pending.len)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("bytes %llu to %llu are past the end of tier \"%s\"'s pack %u",
							(unsigned long long) LOC_OFF(loc),
							(unsigned long long) (LOC_OFF(loc) + len),
							RelationGetRelationName(rel), id)));
		memcpy(dst, pending.buf + LOC_OFF(loc), len);
	}
	else
	{
		Buffer		buf = pack_buffer(rel, id, BUFFER_LOCK_SHARE);
		uint64		owner = pack_entry(BufferGetPage(buf), id)->owner;
		ObjectStore *os;
		char	   *name;

		UnlockReleaseBuffer(buf);
		os = tier_objects();
		name = pack_name(cached_stamp(rel), owner, id);
		os->routine->read(os, name, LOC_OFF(loc), dst, len);
		pfree(name);
	}
}

/*
 * (only counted, WAL-logged, nothing else: VACUUM's end removes what is
 * dead.  Called between taking entries out and the rest, it must not fail
 * for want of anything outside.)
 */
static void
tier_mark_dead(Relation rel, uint64 loc, size_t len, uint32 generation)
{
	uint32		id = LOC_PACK(loc);
	TierMeta	meta = read_meta(rel);
	Buffer		buf;
	TierPack   *p;
	GenericXLogState *state;

	if (id == 0 || id >= meta.next_pack || len == 0)
		return;
	buf = pack_buffer(rel, id, BUFFER_LOCK_EXCLUSIVE);
	p = pack_entry(BufferGetPage(buf), id);
	if (p->state == PACK_REMOVED)
	{
		UnlockReleaseBuffer(buf);
		return;
	}
	state = GenericXLogStart(rel);
	p = pack_entry(GenericXLogRegisterBuffer(state, buf, 0), id);
	p->dead += len;
	/* (where the last died: after the index's records taking them out) */
	if (p->size > 0 && p->dead >= p->size)
		p->dead_lsn = GetXLogInsertRecPtr();
	GenericXLogFinish(state);
	UnlockReleaseBuffer(buf);
}

static uint64
tier_stamp(Relation rel)
{
	return read_meta(rel).stamp;
}

static uint64
tier_end(Relation rel)
{
	return PACK_LOC(read_meta(rel).next_pack, 0);
}

static void
tier_bind_index(Relation rel, Oid index)
{
	Buffer		buf = ReadBuffer(rel, TIER_META_BLOCK);
	TierMeta   *meta;

	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	meta = tier_meta(rel, BufferGetPage(buf));
	if (meta->index != index)
	{
		GenericXLogState *state = GenericXLogStart(rel);

		((TierMeta *) PageGetContents(GenericXLogRegisterBuffer(state, buf, 0)))->index = index;
		GenericXLogFinish(state);
	}
	UnlockReleaseBuffer(buf);
}

/* (packs are not compacted, nor the tier evacuated, yet: none to) */
static int
tier_no_segments(Relation rel, double fraction, int max, uint64 *starts,
				 uint64 *ends)
{
	return 0;
}

static int
tier_no_evacuable(Relation rel, int max, uint64 *starts, uint64 *ends)
{
	return 0;
}

/* (a pack's places: the low 32 bits of a place) */
static uint64
tier_segment_bytes(Relation rel)
{
	return (uint64) 1 << 32;
}

static void
tier_find_segments(Relation rel, double fraction, ChunkLogSegments *out)
{
	memset(out, 0, sizeof(*out));
}

/*
 * Whether every standby connected (a physical walsender) has replayed past
 * lsn: one starting, taking a base backup (a standby being made), or that
 * has not yet replied (a standby just connected), has not.  One that has
 * replied with no place replayed is no hot standby's (pg_receivewal's, a
 * base backup's stream of WAL).  If not, the walsender waited for in
 * *blocker, and what it is doing in *why.
 */
static bool
standbys_replayed(XLogRecPtr lsn, pid_t *blocker, const char **why)
{
	if (WalSndCtl == NULL)
		return true;
	for (int i = 0; i < max_wal_senders; i++)
	{
		WalSnd	   *w = &WalSndCtl->walsnds[i];
		XLogRecPtr	apply;
		pid_t		pid;
		ReplicationKind kind;
		WalSndState st;
		TimestampTz replied;

		SpinLockAcquire(&w->mutex);
		pid = w->pid;
		apply = w->apply;
		kind = w->kind;
		st = w->state;
		replied = w->replyTime;
		SpinLockRelease(&w->mutex);
		if (pid == 0 || kind != REPLICATION_KIND_PHYSICAL)
			continue;
		if (st == WALSNDSTATE_STARTUP || st == WALSNDSTATE_BACKUP)
			*why = st == WALSNDSTATE_STARTUP ? "is starting" : "is taking a base backup";
		else if (replied == 0)
			*why = "has not yet replied";
		else if (apply != InvalidXLogRecPtr && apply < lsn)
			*why = "has not replayed past where they died";
		else
			continue;
		*blocker = pid;
		return false;
	}
	return true;
}

/* whether a pack's transaction ended, and the pack is to be settled */
static bool
to_settle(const TierPack *p)
{
	return p->state == PACK_MADE && !TransactionIdIsCurrentTransactionId(p->xid) &&
		!TransactionIdIsInProgress(p->xid);
}

/*
 * Whether a pack is to be removed: this server's (its owner's, or adopted),
 * an orphan, or committed and all dead, every standby past where it died
 */
static bool
to_remove(const TierMeta *meta, TimeLineID tli, uint32 id, const TierPack *p,
		  int *held, pid_t *blocker, const char **why)
{
	if (!((meta->owner_tli == tli && p->owner == meta->owner) || id < meta->adopted))
		return false;
	if (p->state == PACK_ORPHANED)
		return true;
	if (!(p->state == PACK_COMMITTED && p->size > 0 && p->dead >= p->size))
		return false;
	if (standbys_replayed(p->dead_lsn, blocker, why))
		return true;
	(*held)++;
	return false;
}

/*
 * Every pack whose transaction ended settled, once: committed, or orphaned
 * (WAL-logged, a record a page); the oldest transaction a pack still
 * names, or RecentXmin, in *frozen.  And if remove, the packs to be removed
 * removed: their objects (no lock held), then their entries.
 */
static void
walk_packs(Relation rel, bool remove, TransactionId *frozen)
{
	TierMeta	meta = read_meta(rel);
	TimeLineID	tli = GetWALInsertionTimeLine();
	uint32	   *gone = palloc_array(uint32, PACKS_PER_PAGE);
	int			held = 0;
	pid_t		blocker = 0;
	const char *why = NULL;

	*frozen = RecentXmin;
	for (uint32 first = 1; first < meta.next_pack; first += PACKS_PER_PAGE)
	{
		uint32		last = Min(first + PACKS_PER_PAGE, meta.next_pack);
		Buffer		buf = pack_buffer(rel, first, BUFFER_LOCK_SHARE);
		bool		settle = false;
		int			ngone = 0;

		CHECK_FOR_INTERRUPTS();
		for (uint32 id = first; id < last && !settle; id++)
			settle = to_settle(pack_entry(BufferGetPage(buf), id));
		if (settle)
		{
			GenericXLogState *state;
			Page		page;

			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
			state = GenericXLogStart(rel);
			page = GenericXLogRegisterBuffer(state, buf, 0);
			for (uint32 id = first; id < last; id++)
			{
				TierPack   *p = pack_entry(page, id);

				if (to_settle(p))
					p->state = TransactionIdDidCommit(p->xid) ? PACK_COMMITTED : PACK_ORPHANED;
			}
			GenericXLogFinish(state);
		}
		for (uint32 id = first; id < last; id++)
		{
			TierPack   *p = pack_entry(BufferGetPage(buf), id);

			if (p->state == PACK_MADE && TransactionIdPrecedes(p->xid, *frozen))
				*frozen = p->xid;
			if (remove && to_remove(&meta, tli, id, p, &held, &blocker, &why))
				gone[ngone++] = id;
		}
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);

		/*
		 * (orphans and packs all dead change no more, and only VACUUM of the
		 * index removes, under its lock: what is seen stays so)
		 */
		if (ngone > 0)
		{
			ObjectStore *os = tier_objects();
			GenericXLogState *state;
			Page		page;

			for (int i = 0; i < ngone; i++)
			{
				TierPack	p;
				char	   *name;

				LockBuffer(buf, BUFFER_LOCK_SHARE);
				p = *pack_entry(BufferGetPage(buf), gone[i]);
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);
				name = pack_name(meta.stamp, p.owner, gone[i]);
				os->routine->remove(os, name);
				pfree(name);
			}
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
			state = GenericXLogStart(rel);
			page = GenericXLogRegisterBuffer(state, buf, 0);
			for (int i = 0; i < ngone; i++)
				pack_entry(page, gone[i])->state = PACK_REMOVED;
			GenericXLogFinish(state);
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		}
		ReleaseBuffer(buf);
	}
	pfree(gone);
	if (held > 0)
		ereport(LOG,
				(errmsg("%d packs of tier \"%s\" all dead are kept for standbys",
						held, RelationGetRelationName(rel)),
				 errdetail("Walsender %d %s.", (int) blocker, why)));
}

/*
 * the tier's relfrozenxid advanced to frozen, under the lock VACUUM of it
 * takes (got without waiting from VACUUM of its index: else that VACUUM
 * advances it)
 */
static void
advance_frozen(Relation rel, TransactionId frozen, bool locked)
{
	if (!locked && !ConditionalLockRelation(rel, ShareUpdateExclusiveLock))
		return;
	vac_update_relstats(rel, RelationGetNumberOfBlocks(rel), 0, 0, 0, false,
						frozen, InvalidMultiXactId, NULL, NULL, false);
	if (!locked)
		UnlockRelation(rel, ShareUpdateExclusiveLock);
}

void
ChunkTierVacuum(Relation rel)
{
	TransactionId frozen;

	walk_packs(rel, false, &frozen);
	advance_frozen(rel, frozen, true);
}

static void *
tier_vacuum_begin(Relation rel)
{
	return NULL;
}

/*
 * VACUUM of the index ended: every pack settled, the relfrozenxid advanced,
 * and the packs to be removed removed (what object of an orphan's may have
 * been written: the store's removes are of what is there)
 */
static void
tier_vacuum_end(Relation rel, void *vstate, bool truncate, double fraction,
				const uint8 *pending_bits, uint64 npending, ChunkLogSegments *out)
{
	TransactionId frozen;

	memset(out, 0, sizeof(*out));
	walk_packs(rel, true, &frozen);
	advance_frozen(rel, frozen, false);
}

/*
 * pg_history_tier_adopt(): ChunkTierAdopt of the store's tier, for a
 * superuser, who says no other server uses its packs
 */
Datum
pg_history_tier_adopt(PG_FUNCTION_ARGS)
{
	Relation	rel;
	int			n;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("only a superuser may adopt the history store's tier's packs")));
	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("cannot adopt the history store's tier's packs during recovery")));
	/*
	 * (not waiting for VACUUM of the index, which takes the tier's
	 * RowExclusiveLock: a walk with the meta page from before removes less)
	 */
	rel = relation_open(ChunkStoreTierLogRelid(), ShareUpdateExclusiveLock);
	n = ChunkTierAdopt(rel);
	relation_close(rel, ShareUpdateExclusiveLock);
	PG_RETURN_INT32(n);
}

int
ChunkTierAdopt(Relation rel)
{
	Buffer		buf = ReadBuffer(rel, TIER_META_BLOCK);
	GenericXLogState *state;
	TierMeta   *meta;
	int			n;

	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	state = GenericXLogStart(rel);
	meta = tier_meta(rel, GenericXLogRegisterBuffer(state, buf, 0));
	n = meta->next_pack - Max(meta->adopted, 1);
	meta->adopted = meta->next_pack;
	GenericXLogFinish(state);
	UnlockReleaseBuffer(buf);
	return Max(n, 0);
}

/*
 * For audits: every chunk given in a pack committed and not removed, its
 * bytes within the pack's, and no pack's live and dead bytes more than it
 * has
 */
static bool
tier_check_live(Relation rel, int n, const uint64 *locs, const uint32 *lens,
				char **why)
{
	TierMeta	meta = read_meta(rel);
	uint64	   *live = palloc0_array(uint64, Max(meta.next_pack, 1));
	bool		ok = true;

	*why = NULL;
	for (int i = 0; i < n && ok; i++)
	{
		uint32		id = LOC_PACK(locs[i]);

		if (lens[i] == 0)
			continue;
		if (id == 0 || id >= meta.next_pack)
		{
			*why = psprintf("a chunk of %u bytes is in pack %u of tier \"%s\", which has %u packs",
							lens[i], id, RelationGetRelationName(rel), meta.next_pack - 1);
			ok = false;
			break;
		}
		live[id] += lens[i];
	}
	for (uint32 id = 1; id < meta.next_pack && ok; id++)
	{
		Buffer		buf;
		TierPack	p;

		if (live[id] == 0)
			continue;
		buf = pack_buffer(rel, id, BUFFER_LOCK_SHARE);
		p = *pack_entry(BufferGetPage(buf), id);
		UnlockReleaseBuffer(buf);
		if (id == pending.id && pending.relnumber == rel->rd_locator.relNumber)
			p.size = pending.len;
		if (p.state == PACK_MADE && !TransactionIdIsCurrentTransactionId(p.xid) &&
			!TransactionIdIsInProgress(p.xid) && TransactionIdDidCommit(p.xid))
			p.state = PACK_COMMITTED;
		if (!pack_live(&p))
			*why = psprintf("pack %u of tier \"%s\" has live chunks but is %s",
							id, RelationGetRelationName(rel),
							p.state == PACK_REMOVED ? "removed" :
							p.state == PACK_ORPHANED ? "an orphan" : "no committed transaction's");
		else if (live[id] + p.dead > p.size)
			*why = psprintf("pack %u of tier \"%s\" has %llu bytes of live chunks, %llu dead, and %llu in all",
							id, RelationGetRelationName(rel),
							(unsigned long long) live[id], (unsigned long long) p.dead,
							(unsigned long long) p.size);
		ok = *why == NULL;
	}
	pfree(live);
	return ok;
}

int
ChunkTierPackTable(Relation rel, uint32 id, ChunkTierPackEntry **entries, uint64 *data_len)
{
	TierMeta	meta = read_meta(rel);
	Buffer		buf;
	TierPack	p;
	char	   *name;
	int			n;

	if (id == 0 || id >= meta.next_pack)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("tier \"%s\" has no pack %u", RelationGetRelationName(rel), id)));
	buf = pack_buffer(rel, id, BUFFER_LOCK_SHARE);
	p = *pack_entry(BufferGetPage(buf), id);
	UnlockReleaseBuffer(buf);
	if (p.state == PACK_REMOVED || p.size == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("tier \"%s\"'s pack %u is not written, or removed",
						RelationGetRelationName(rel), id)));
	name = pack_name(meta.stamp, p.owner, id);
	n = ChunkTierReadPackTable(tier_objects(), name, entries, data_len);
	if (*data_len != p.size)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("pack \"%s\" has %llu bytes of chunks, its entry %llu",
						name, (unsigned long long) *data_len, (unsigned long long) p.size)));
	pfree(name);
	return n;
}

int
ChunkTierPacks(Relation rel, ChunkTierPackInfo **out)
{
	TierMeta	meta = read_meta(rel);
	ChunkTierPackInfo *packs = palloc0_array(ChunkTierPackInfo, Max(meta.next_pack, 1));
	int			n = 0;

	for (uint32 id = 1; id < meta.next_pack; id++)
	{
		Buffer		buf = pack_buffer(rel, id, BUFFER_LOCK_SHARE);
		TierPack	p = *pack_entry(BufferGetPage(buf), id);

		UnlockReleaseBuffer(buf);
		packs[n].id = id;
		packs[n].state = p.state == PACK_MADE ? "made" :
			p.state == PACK_COMMITTED ? "committed" :
			p.state == PACK_ORPHANED ? "orphaned" : "removed";
		packs[n].size = p.size;
		packs[n].dead = p.dead;
		packs[n].owner = p.owner;
		n++;
	}
	*out = packs;
	return n;
}

const ChunkLogRoutine TierChunkLog = {
	.append = tier_append,
	.prepare = tier_prepare,
	.reserve = tier_reserve,
	.write = tier_write,
	.release = tier_release,
	.extend = tier_extend,
	.prepare_free = tier_prepare_free,
	.compactable = tier_no_segments,
	.evacuable = tier_no_evacuable,
	.segment_bytes = tier_segment_bytes,
	.find_segments = tier_find_segments,
	.vacuum_begin = tier_vacuum_begin,
	.vacuum_end = tier_vacuum_end,
	.check_live = tier_check_live,
	.bind_index = tier_bind_index,
	.read = tier_read,
	.mark_dead = tier_mark_dead,
	.stamp = tier_stamp,
	.end = tier_end,
};
