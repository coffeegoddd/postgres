/*-------------------------------------------------------------------------
 *
 * chunk_tier.c
 *	  The history store's tier (prolly/chunk_tier.h): a chunk log whose
 *	  pages are the catalog of its packs, and whose bytes are in them.
 *
 *	block 0		the meta page: the next pack's number, the log's stamp (part
 *				of its packs' names, so that a log made afresh names none
 *				of another's), and the index bound to it
 *	blocks 1-	the packs, by number, an entry each: the transaction that
 *				made it, its size once written, and the bytes of it dead
 *
 * Changes of the catalog are WAL-logged as generic records.  A pack's entry
 * is its transaction's: once that commits, the pack is the store's (its
 * object written before the commit record); if it aborts, or a crash cuts
 * it short, VACUUM removes what object of it there may be.  The entry's
 * record is flushed before the object is written, as the WAL is before
 * the pages it describes, so the catalog has every object there may be.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/chunk_tier.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/generic_xlog.h"
#include "access/relation.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "miscadmin.h"
#include "prolly/chunk_tier.h"
#include "prolly/object_store.h"
#include "storage/bufmgr.h"
#include "storage/bulk_write.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"
#include "utils/rel.h"

char	   *history_tier_store = NULL;
char	   *history_tier_location = NULL;

#define TIER_MAGIC			0x54494552	/* "TIER" */
#define TIER_VERSION		1
#define TIER_META_BLOCK		0

typedef struct TierMeta
{
	uint32		magic;
	uint32		version;
	uint32		next_pack;		/* the next pack's number, from 1 */
	uint32		unused;
	uint64		stamp;			/* made with the storage */
	Oid			index;			/* the chunk index bound to it */
} TierMeta;

/* a pack's states */
#define PACK_MADE			1	/* its transaction's: if committed, the store's */
#define PACK_REMOVED		2	/* its object gone, or never written */

typedef struct TierPack
{
	uint32		state;
	TransactionId xid;			/* the transaction that made it */
	uint64		size;			/* its bytes, once written */
	uint64		dead;			/* of them, no chunk's any more */
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
typedef struct TierPending
{
	uint32		id;				/* 0: none */
	Oid			relid;
	RelFileNumber relnumber;
	uint64		stamp;
	char	   *buf;
	uint64		len;
	uint64		cap;
} TierPending;

static TierPending pending;
static bool callback_registered = false;

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

/* pack id's entry's page, locked exclusively; *slot its place */
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

/* a pack's object's name: the log's stamp and the pack's number */
static char *
pack_name(uint64 stamp, uint32 id)
{
	return psprintf("packs/%016llx/%u", (unsigned long long) stamp, id);
}

/* the store the tier keeps its packs in */
static ObjectStore *
tier_objects(void)
{
	if (history_tier_location == NULL || history_tier_location[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("the history store's tier has no place set"),
				 errhint("Set history_tier_location.")));
	return ObjectStoreOpen(history_tier_store, history_tier_location);
}

/* whether a pack's transaction committed (this one's: as if it had) */
static bool
pack_committed(const TierPack *p)
{
	if (TransactionIdIsCurrentTransactionId(p->xid))
		return true;
	if (TransactionIdIsInProgress(p->xid))
		return false;
	return TransactionIdDidCommit(p->xid);
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
	}

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
	((TierMeta *) PageGetContents(mpage))->next_pack = id + 1;
	p = pack_entry(ppage, id);
	p->state = PACK_MADE;
	p->xid = GetCurrentTransactionId();
	p->size = 0;
	p->dead = 0;
	GenericXLogFinish(state);
	UnlockReleaseBuffer(pbuf);
	UnlockReleaseBuffer(mbuf);

	if (!callback_registered)
	{
		RegisterXactCallback(tier_xact_callback, NULL);
		callback_registered = true;
	}
	pending.id = id;
	pending.relid = RelationGetRelid(rel);
	pending.relnumber = rel->rd_locator.relNumber;
	pending.stamp = meta->stamp;
	pending.len = 0;
}

static void
forget_pending(void)
{
	if (pending.buf)
		pfree(pending.buf);
	memset(&pending, 0, sizeof(pending));
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
		char	   *name = pack_name(pending.stamp, pending.id);

		/*
		 * (the entry's record flushed before the object is written, as the
		 * WAL is before the pages it describes: after a crash the catalog
		 * knows every object there may be, an orphan's for VACUUM)
		 */
		XLogFlush(PageGetLSN(BufferGetPage(buf)));

		/* (no lock held as it is written: its entry is this transaction's) */
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		os->routine->put(os, name, pending.buf, pending.len);
		os->routine->close(os);
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
				Relation	rel = relation_open(pending.relid, NoLock);

				write_pack(rel);
				relation_close(rel, NoLock);
			}
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
	if (pending.id != 0 && pending.relnumber != rel->rd_locator.relNumber)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a transaction puts chunks in one tier of the history store")));

	/* (a pack full written now, and another begun) */
	if (pending.id != 0 && pending.len > 0 && pending.len + len > TIER_PACK_BYTES)
		write_pack(rel);
	if (pending.id == 0)
		begin_pack(rel);

	/* (room made now: reserve, which must not fail, takes it) */
	if (pending.len + len > pending.cap)
	{
		uint64		cap = Max(pending.len + len, Max(pending.cap * 2, 1024 * 1024));

		cap = Min(cap, Max(TIER_PACK_BYTES, pending.len + len));
		pending.buf = pending.buf ?
			repalloc_huge(pending.buf, cap) :
			MemoryContextAllocHuge(TopMemoryContext, cap);
		pending.cap = cap;
	}
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
	pending.len = Max(pending.len, (uint64) LOC_OFF(loc) + len);
	return loc;
}

static void
tier_write(Relation rel, uint64 loc, const void *data, size_t len)
{
	if (LOC_PACK(loc) != pending.id || (uint64) LOC_OFF(loc) + len > pending.len)
		elog(ERROR, "tier \"%s\": bytes written at %llu where none were reserved",
			 RelationGetRelationName(rel), (unsigned long long) loc);
	memcpy(pending.buf + LOC_OFF(loc), data, len);
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
		TierMeta	meta = read_meta(rel);
		ObjectStore *os = tier_objects();
		char	   *name = pack_name(meta.stamp, id);

		os->routine->read(os, name, LOC_OFF(loc), dst, len);
		os->routine->close(os);
		pfree(name);
	}
}

/* a pack all dead, its transaction committed: its object removed */
static void
remove_pack(Relation rel, Buffer buf, uint32 id, uint64 stamp)
{
	GenericXLogState *state;
	ObjectStore *os = tier_objects();
	char	   *name = pack_name(stamp, id);

	os->routine->remove(os, name);
	os->routine->close(os);
	pfree(name);
	state = GenericXLogStart(rel);
	pack_entry(GenericXLogRegisterBuffer(state, buf, 0), id)->state = PACK_REMOVED;
	GenericXLogFinish(state);
}

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
	if (p->state != PACK_MADE)
	{
		UnlockReleaseBuffer(buf);
		return;
	}
	state = GenericXLogStart(rel);
	p = pack_entry(GenericXLogRegisterBuffer(state, buf, 0), id);
	p->dead += len;
	GenericXLogFinish(state);
	p = pack_entry(BufferGetPage(buf), id);
	if (p->size > 0 && p->dead >= p->size && pack_committed(p) &&
		!TransactionIdIsCurrentTransactionId(p->xid))
		remove_pack(rel, buf, id, meta.stamp);
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

static void *
tier_vacuum_begin(Relation rel)
{
	return NULL;
}

/*
 * VACUUM's end: every pack whose transaction aborted, or a crash cut
 * short, removed (what object of it may have been written: the store's
 * removes are of what is there); every pack all dead removed
 */
static void
tier_vacuum_end(Relation rel, void *vstate, bool truncate, double fraction,
				const uint8 *pending_bits, uint64 npending, ChunkLogSegments *out)
{
	TierMeta	meta = read_meta(rel);

	memset(out, 0, sizeof(*out));
	for (uint32 id = 1; id < meta.next_pack; id++)
	{
		Buffer		buf;
		TierPack   *p;

		if (PACK_SLOT(id) == 0 || id == 1)
			CHECK_FOR_INTERRUPTS();
		buf = pack_buffer(rel, id, BUFFER_LOCK_EXCLUSIVE);
		p = pack_entry(BufferGetPage(buf), id);
		if (p->state == PACK_MADE &&
			!TransactionIdIsCurrentTransactionId(p->xid) &&
			!TransactionIdIsInProgress(p->xid) &&
			(!TransactionIdDidCommit(p->xid) || (p->size > 0 && p->dead >= p->size)))
			remove_pack(rel, buf, id, meta.stamp);
		UnlockReleaseBuffer(buf);
	}
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
		if (p.state != PACK_MADE || !pack_committed(&p))
			*why = psprintf("pack %u of tier \"%s\" has live chunks but is %s",
							id, RelationGetRelationName(rel),
							p.state != PACK_MADE ? "removed" : "no committed transaction's");
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
