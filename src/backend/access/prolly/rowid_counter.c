/*-------------------------------------------------------------------------
 *
 * rowid_counter.c
 *	  Row identity implementation "counter": TIDs from a per-table counter.
 *
 * TIDs are numbered 0, 1, 2, ... per table, number n being the item
 * pointer (n / MaxHeapTuplesPerPage, n % MaxHeapTuplesPerPage + 1), so
 * they are valid heap-style item pointers in insertion order.  A number is
 * never handed out twice, whatever becomes of the transaction that got it:
 * the counter is not transactional, as heap's line pointers are not.  One
 * counter serves every branch of the table, so a TID never names
 * different versions on different branches, and an index serving several
 * branches stays correct.
 *
 * The counter is block 0 of the table's main fork: a page whose special
 * space holds how far numbers have been reserved.  A backend reserves
 * RESERVE_TIDS numbers at a time, WAL-logging the page (generic WAL), and
 * hands them out from memory, as sequences cache values; numbers reserved
 * but not handed out (the backend exits, the server crashes) are skipped.
 * The page is created on first use.  New storage (TRUNCATE, rewrites)
 * starts a new counter, as its indexes are rebuilt; SET TABLESPACE and
 * CREATE DATABASE copy the page with the table.
 *
 * The relation's row-identity tree (PROLLY_TREE_ROWID) holds, keyed by a
 * tag byte:
 *
 *	'T' block(4) offset(2)
 *		-> content-length content varint-ordinal xmin(4) row-key
 *		for every TID whose version may exist, or once its version is gone
 *		-> 0 cid(4) xmin(4) flag [successor TID (6 bytes) if flag]
 *	'G' block(4) offset(2)
 *		-> the gone version's row, kept like a dead tuple, as its own item:
 *		the same bytes the data tree held, so a large row's value tree is
 *		shared with it, not written again
 *
 * and its tree of local metadata by row (PROLLY_TREE_ROWKEY), in the data
 * tree's key order, holds a version entry for every version in the table:
 *
 *	row-key ordinal(8) -> TID (6 bytes)
 *
 * An entry may outlive its version (see tid_rename), but only while no
 * version is at its key.  Integers are big-endian; xmin is the transaction
 * that wrote the version: local metadata, like the TIDs themselves, never
 * part of canonical content.  A scan finds a row's TID by its version
 * entry, seeking one cursor over them as it goes: they are in the order
 * the scan reads rows, whatever the table's key order, so the cursor only
 * moves forward.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/rowid_counter.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/generic_xlog.h"
#include "access/htup_details.h"
#include "access/prolly_builtins.h"
#include "access/prolly_datum.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#define TAG_TID			'T'
#define TAG_GONE_ROW	'G'

#define TID_KEY_LEN		7
#define TID_BYTES		6

/* Numbers a backend reserves at a time. */
#define RESERVE_TIDS	1024

/* Numbers that fit in a valid item pointer. */
#define MAX_TID_NUMBER	((uint64) (InvalidBlockNumber - 1) * MaxHeapTuplesPerPage)

#define COUNTER_MAGIC	0x50544944	/* "PTID" */
#define COUNTER_VERSION 1

typedef struct CounterPageData
{
	uint32		magic;
	uint32		version;
	uint64		reserved_to;	/* numbers below this may have been used */
} CounterPageData;

/* A backend's reserved numbers, per table storage. */
typedef struct Reservation
{
	RelFileLocator locator;		/* hash key */
	uint64		next;
	uint64		end;
} Reservation;

static HTAB *reservations = NULL;

/* A version entry, read whole while the entries' key order is lost. */
typedef struct LoadedVersion
{
	uint8	   *key;
	uint32		keylen;
	ItemPointerData tid;
} LoadedVersion;

struct ProllyRowIdScan
{
	ProllyRootSet *roots;
	Relation	rel;
	CommandId	cid;
	StringInfoData buf;
	ProllyCursor *cursor;		/* over the version entries, made on first use */

	/* The cursor can't seek: every entry, sorted by bytes */
	bool		loaded;
	LoadedVersion *versions;
	int			nversions;
};

static const ProllyEngineRoutine *
engine_for(Relation rel)
{
	ProllyComponents c;

	prolly_components_for_relation(rel, &c);
	return c.engine;
}

/* ------------------------------------------------------------------------
 * The counter
 * ------------------------------------------------------------------------
 */

static CounterPageData *
counter_data(Page page, Relation rel)
{
	CounterPageData *cp;

	if (PageIsNew(page) || PageGetSpecialSize(page) != MAXALIGN(sizeof(CounterPageData)))
		goto bad;
	cp = (CounterPageData *) PageGetSpecialPointer(page);
	if (cp->magic != COUNTER_MAGIC || cp->version != COUNTER_VERSION)
		goto bad;
	return cp;

bad:
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("prolly TID counter of \"%s\" is invalid",
					RelationGetRelationName(rel))));
	return NULL;				/* keep compiler quiet */
}

/* Block 0 of rel's main fork, made if the fork is still empty. */
static Buffer
counter_buffer(Relation rel)
{
	if (RelationGetNumberOfBlocks(rel) == 0)
	{
		LockRelationForExtension(rel, ExclusiveLock);
		if (RelationGetNumberOfBlocks(rel) == 0)
		{
			Buffer		buf = ExtendBufferedRel(BMR_REL(rel), MAIN_FORKNUM,
												NULL, EB_LOCK_FIRST);
			GenericXLogState *state = GenericXLogStart(rel);
			Page		page = GenericXLogRegisterBuffer(state, buf,
														 GENERIC_XLOG_FULL_IMAGE);
			CounterPageData *cp;

			PageInit(page, BLCKSZ, sizeof(CounterPageData));
			cp = (CounterPageData *) PageGetSpecialPointer(page);
			cp->magic = COUNTER_MAGIC;
			cp->version = COUNTER_VERSION;
			cp->reserved_to = 0;
			GenericXLogFinish(state);
			UnlockReleaseBuffer(buf);
		}
		UnlockRelationForExtension(rel, ExclusiveLock);
	}
	return ReadBuffer(rel, 0);
}

/* The next TID number for rel: never one handed out before. */
static uint64
next_number(Relation rel)
{
	Reservation *res;
	bool		found;

	if (reservations == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(RelFileLocator);
		ctl.entrysize = sizeof(Reservation);
		ctl.hcxt = TopMemoryContext;
		reservations = hash_create("prolly TID reservations", 64, &ctl,
								   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	res = hash_search(reservations, &rel->rd_locator, HASH_ENTER, &found);
	if (!found)
		res->next = res->end = 0;

	if (res->next >= res->end)
	{
		Buffer		buf = counter_buffer(rel);
		GenericXLogState *state;
		CounterPageData *cp;
		uint64		start;

		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		start = counter_data(BufferGetPage(buf), rel)->reserved_to;
		if (start >= MAX_TID_NUMBER)
		{
			UnlockReleaseBuffer(buf);
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("table \"%s\" has used every TID",
							RelationGetRelationName(rel)),
					 errhint("Rewrite the table, for example with VACUUM FULL.")));
		}
		state = GenericXLogStart(rel);
		cp = counter_data(GenericXLogRegisterBuffer(state, buf, 0), rel);
		cp->reserved_to = Min(start + RESERVE_TIDS, MAX_TID_NUMBER);
		res->next = start;
		res->end = cp->reserved_to;
		GenericXLogFinish(state);
		UnlockReleaseBuffer(buf);
	}
	return res->next++;
}

static void
number_tid(uint64 n, ItemPointer tid)
{
	ItemPointerSet(tid, (BlockNumber) (n / MaxHeapTuplesPerPage),
				   (OffsetNumber) (n % MaxHeapTuplesPerPage + 1));
}

/* ------------------------------------------------------------------------
 * Map entries
 * ------------------------------------------------------------------------
 */

static void
put_uint32(StringInfo out, uint32 v)
{
	for (int i = 3; i >= 0; i--)
		appendStringInfoChar(out, (char) (v >> (i * 8)));
}

static uint32
get_uint32(const uint8 *d)
{
	return ((uint32) d[0] << 24) | ((uint32) d[1] << 16) |
		((uint32) d[2] << 8) | (uint32) d[3];
}

static void
encode_tid(ItemPointer tid, StringInfo out)
{
	BlockNumber blk = ItemPointerGetBlockNumber(tid);
	OffsetNumber off = ItemPointerGetOffsetNumber(tid);

	put_uint32(out, blk);
	appendStringInfoChar(out, (char) (off >> 8));
	appendStringInfoChar(out, (char) off);
}

static void
decode_tid(const uint8 *data, Size len, ItemPointer tid)
{
	if (len != TID_BYTES)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid prolly row identity entry")));
	ItemPointerSet(tid, get_uint32(data), (OffsetNumber) ((data[4] << 8) | data[5]));
}

static ProllyKey
tagged_tid_key(ItemPointer tid, uint8 *buf, uint8 tag)
{
	BlockNumber blk = ItemPointerGetBlockNumber(tid);
	OffsetNumber off = ItemPointerGetOffsetNumber(tid);
	ProllyKey	key;

	buf[0] = tag;
	buf[1] = (uint8) (blk >> 24);
	buf[2] = (uint8) (blk >> 16);
	buf[3] = (uint8) (blk >> 8);
	buf[4] = (uint8) blk;
	buf[5] = (uint8) (off >> 8);
	buf[6] = (uint8) off;
	key.data = buf;
	key.len = TID_KEY_LEN;
	return key;
}

static ProllyKey
tid_key(ItemPointer tid, uint8 *buf)
{
	return tagged_tid_key(tid, buf, TAG_TID);
}

/* The version entry's key of the version at key with ordinal, in buf. */
static ProllyKey
version_key(const ProllyKey *key, uint64 ordinal, StringInfo buf)
{
	ProllyKey	k;

	StaticAssertDecl(PROLLY_ROWKEY_SUFFIX_LEN == 8,
					 "a version key's suffix is its ordinal");
	resetStringInfo(buf);
	appendBinaryStringInfo(buf, (const char *) key->data, key->len);
	for (int i = 7; i >= 0; i--)
		appendStringInfoChar(buf, (char) (ordinal >> (i * 8)));
	k.data = (const uint8 *) buf->data;
	k.len = buf->len;
	return k;
}

static void
encode_entry(const ProllyHash *content, uint64 ordinal, TransactionId xmin,
			 const ProllyKey *key, StringInfo out)
{
	appendStringInfoChar(out, (char) content->len);
	appendBinaryStringInfo(out, (const char *) content->bytes, content->len);
	prolly_put_varint(out, ordinal);
	put_uint32(out, xmin);
	appendBinaryStringInfo(out, (const char *) key->data, key->len);
}

/* A 'T' entry of a version that may exist; key points into v. */
typedef struct Entry
{
	ProllyHash	content;
	uint64		ordinal;
	TransactionId xmin;
	ProllyKey	key;
} Entry;

static bool
entry_gone(const StringInfoData *v)
{
	return v->len > 0 && v->data[0] == 0;
}

static void
decode_entry(const StringInfoData *v, Entry *e)
{
	const uint8 *data = (const uint8 *) v->data;
	const char *p = v->data;
	const char *end = p + v->len;

	if (v->len < 1 || data[0] == 0 || data[0] > PROLLY_HASH_MAX_LEN ||
		v->len < 1 + data[0])
		goto bad;
	memset(&e->content, 0, sizeof(e->content));
	e->content.len = data[0];
	memcpy(e->content.bytes, data + 1, e->content.len);
	p += 1 + e->content.len;
	e->ordinal = prolly_get_varint(&p, end);
	if (end - p < 4)
		goto bad;
	e->xmin = get_uint32((const uint8 *) p);
	p += 4;
	e->key.data = (const uint8 *) p;
	e->key.len = end - p;
	return;

bad:
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("invalid prolly row identity entry")));
}

static void
encode_gone(ItemPointer successor, CommandId cid, TransactionId xmin,
			StringInfo out)
{
	appendStringInfoChar(out, 0);
	put_uint32(out, cid);
	put_uint32(out, xmin);
	appendStringInfoChar(out, successor ? 1 : 0);
	if (successor)
		encode_tid(successor, out);
}

/* A gone entry: sets *info. */
static void
decode_gone(const StringInfoData *v, ProllyTidInfo *info)
{
	const uint8 *d = (const uint8 *) v->data;

	if (v->len != (d[9] ? 10 + TID_BYTES : 10) || (d[9] != 0 && d[9] != 1))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid prolly row identity entry")));
	info->cid = get_uint32(d + 1);
	info->xmin = get_uint32(d + 5);
	if (d[9])
		decode_tid(d + 10, TID_BYTES, &info->successor);
	else
		ItemPointerSetInvalid(&info->successor);
}

static bool
same_content(const ProllyHash *a, const ProllyHash *b)
{
	return a->len == b->len && memcmp(a->bytes, b->bytes, a->len) == 0;
}

/* The TID the version entry of (key, ordinal) holds, seeing writes before cid. */
static bool
version_tid(const ProllyEngineRoutine *engine, ProllyRootSet *roots,
			Relation rel, const ProllyKey *key, uint64 ordinal, CommandId cid,
			StringInfo buf, ItemPointer tid)
{
	ProllyKey	vkey = version_key(key, ordinal, buf);
	StringInfoData v;
	bool		found;

	initStringInfo(&v);
	found = PROLLY_CALL(engine, get, roots, rel, PROLLY_TREE_ROWKEY, &vkey, cid, &v);
	if (found)
		decode_tid((const uint8 *) v.data, v.len, tid);
	pfree(v.data);
	return found;
}

/* Remove the version entry of (key, ordinal) if it holds tid. */
static void
drop_version(const ProllyEngineRoutine *engine, ProllyRootSet *roots,
			 Relation rel, const ProllyKey *key, uint64 ordinal,
			 ItemPointer tid, CommandId cid)
{
	StringInfoData buf;
	ItemPointerData held;

	initStringInfo(&buf);
	if (version_tid(engine, roots, rel, key, ordinal, InvalidCommandId, &buf,
					&held) && ItemPointerEquals(&held, tid))
	{
		ProllyKey	vkey = version_key(key, ordinal, &buf);

		PROLLY_CALL(engine, remove, rel, PROLLY_TREE_ROWKEY, &vkey, cid);
	}
	pfree(buf.data);
}

static void
put_version(const ProllyEngineRoutine *engine, Relation rel,
			const ProllyKey *key, uint64 ordinal, ItemPointer tid,
			CommandId cid)
{
	StringInfoData buf;
	StringInfoData t;
	ProllyKey	vkey;
	ProllyValue v;

	initStringInfo(&buf);
	initStringInfo(&t);
	vkey = version_key(key, ordinal, &buf);
	encode_tid(tid, &t);
	v.data = (const uint8 *) t.data;
	v.len = t.len;
	PROLLY_CALL(engine, put, rel, PROLLY_TREE_ROWKEY, &vkey, &v, cid);
	pfree(buf.data);
	pfree(t.data);
}

static void
put_entry(const ProllyEngineRoutine *engine, Relation rel, ItemPointer tid,
		  const StringInfoData *entry, CommandId cid)
{
	uint8		tbuf[TID_KEY_LEN];
	ProllyKey	tkey = tid_key(tid, tbuf);
	ProllyValue v = {(const uint8 *) entry->data, entry->len};

	PROLLY_CALL(engine, put, rel, PROLLY_TREE_ROWID, &tkey, &v, cid);
}

/* tid's gone version's row: its own entry, keyed 'G'. */
static void
put_gone_row(const ProllyEngineRoutine *engine, Relation rel, ItemPointer tid,
			 const ProllyValue *row, CommandId cid)
{
	uint8		gbuf[TID_KEY_LEN];
	ProllyKey	gkey = tagged_tid_key(tid, gbuf, TAG_GONE_ROW);

	PROLLY_CALL(engine, put, rel, PROLLY_TREE_ROWID, &gkey, row, cid);
}

static void
get_gone_row(const ProllyEngineRoutine *engine, ProllyRootSet *roots,
			 Relation rel, ItemPointer tid, CommandId cid, StringInfo out)
{
	uint8		gbuf[TID_KEY_LEN];
	ProllyKey	gkey = tagged_tid_key(tid, gbuf, TAG_GONE_ROW);

	if (!PROLLY_CALL(engine, get, roots, rel, PROLLY_TREE_ROWID, &gkey, cid, out))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("gone TID (%u,%u) of \"%s\" has no row",
						ItemPointerGetBlockNumber(tid),
						ItemPointerGetOffsetNumber(tid),
						RelationGetRelationName(rel))));
}

/* The 'T' entry of tid with every write; false if there is none. */
static bool
get_entry(const ProllyEngineRoutine *engine, ProllyRootSet *roots,
		  Relation rel, ItemPointer tid, CommandId cid, StringInfo v)
{
	uint8		tbuf[TID_KEY_LEN];
	ProllyKey	tkey = tid_key(tid, tbuf);

	return PROLLY_CALL(engine, get, roots, rel, PROLLY_TREE_ROWID, &tkey, cid, v);
}

/* ------------------------------------------------------------------------
 * Callbacks
 * ------------------------------------------------------------------------
 */

static void
counter_tid_assign(ProllyRootSet *roots, Relation rel, const ProllyHash *content,
				   uint64 ordinal, const ProllyKey *key, CommandId cid,
				   ItemPointer tid)
{
	const ProllyEngineRoutine *engine = engine_for(rel);
	StringInfoData buf;
	StringInfoData entry;

	/* A version that has a TID keeps it. */
	initStringInfo(&buf);
	if (version_tid(engine, roots, rel, key, ordinal, InvalidCommandId, &buf, tid))
	{
		StringInfoData v;
		bool		same = false;

		initStringInfo(&v);
		if (get_entry(engine, roots, rel, tid, InvalidCommandId, &v) &&
			!entry_gone(&v))
		{
			Entry		e;

			decode_entry(&v, &e);
			same = same_content(&e.content, content) && e.ordinal == ordinal &&
				e.key.len == key->len &&
				memcmp(e.key.data, key->data, key->len) == 0;
		}
		pfree(v.data);
		if (same)
		{
			pfree(buf.data);
			return;
		}
	}
	pfree(buf.data);

	number_tid(next_number(rel), tid);
	initStringInfo(&entry);
	encode_entry(content, ordinal, GetCurrentTransactionId(), key, &entry);
	put_entry(engine, rel, tid, &entry, cid);
	put_version(engine, rel, key, ordinal, tid, cid);
	pfree(entry.data);
}

static ProllyRowIdScan *
counter_scan_begin(ProllyRootSet *roots, Relation rel, CommandId cid)
{
	ProllyRowIdScan *scan = palloc0(sizeof(ProllyRowIdScan));

	scan->roots = roots;
	scan->rel = rel;
	scan->cid = cid;
	initStringInfo(&scan->buf);
	return scan;
}

static int
loaded_cmp(const void *a, const void *b)
{
	const LoadedVersion *va = a;
	const LoadedVersion *vb = b;
	int			r = memcmp(va->key, vb->key, Min(va->keylen, vb->keylen));

	if (r != 0)
		return r;
	return (va->keylen > vb->keylen) - (va->keylen < vb->keylen);
}

/*
 * Read every version entry, in the scan's memory: while the table's key
 * order is lost, the entries can't be sought, only scanned (the table's
 * next write rebuilds them).
 */
static void
load_versions(const ProllyEngineRoutine *engine, ProllyRowIdScan *scan)
{
	ProllyKey	k;
	ProllyValue v;
	int			cap = 1024;

	scan->versions = palloc(sizeof(LoadedVersion) * cap);
	PROLLY_CALL(engine, cursor_seek, scan->cursor, NULL, ForwardScanDirection);
	while (PROLLY_CALL(engine, cursor_next, scan->cursor, &k, &v))
	{
		LoadedVersion *lv;

		if (scan->nversions == cap)
		{
			cap *= 2;
			scan->versions = repalloc_huge(scan->versions,
										   sizeof(LoadedVersion) * cap);
		}
		lv = &scan->versions[scan->nversions++];
		lv->key = palloc(k.len);
		memcpy(lv->key, k.data, k.len);
		lv->keylen = k.len;
		decode_tid(v.data, v.len, &lv->tid);
		CHECK_FOR_INTERRUPTS();
	}
	qsort(scan->versions, scan->nversions, sizeof(LoadedVersion), loaded_cmp);
	scan->loaded = true;
}

/*
 * A scan finds rows in its key order, and the version entries are in that
 * order, so it seeks one cursor over them rather than looking up each row:
 * seeks near the last one read no new nodes, where a lookup reads a whole
 * path.
 */
static bool
counter_tid_find(ProllyRowIdScan *scan, const ProllyKey *key, uint64 ordinal,
				 ItemPointer tid)
{
	const ProllyEngineRoutine *engine = engine_for(scan->rel);
	ProllyKey	vkey = version_key(key, ordinal, &scan->buf);
	ProllyKey	k;
	ProllyValue v;

	if (scan->cursor == NULL)
	{
		/* It lives as long as the scan, not the caller's current context. */
		MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(scan));

		scan->cursor = PROLLY_CALL(engine, cursor_open, scan->roots, scan->rel,
								   PROLLY_TREE_ROWKEY, scan->cid);
		if (!PROLLY_CALL(engine, cursor_can_seek, scan->cursor))
			load_versions(engine, scan);
		MemoryContextSwitchTo(old);
	}
	if (scan->loaded)
	{
		LoadedVersion probe;
		LoadedVersion *found;

		probe.key = (uint8 *) vkey.data;
		probe.keylen = vkey.len;
		found = bsearch(&probe, scan->versions, scan->nversions,
						sizeof(LoadedVersion), loaded_cmp);
		if (found == NULL)
			return false;
		*tid = found->tid;
		return true;
	}
	PROLLY_CALL(engine, cursor_seek, scan->cursor, &vkey, ForwardScanDirection);
	if (!PROLLY_CALL(engine, cursor_next, scan->cursor, &k, &v) ||
		k.len != vkey.len || memcmp(k.data, vkey.data, k.len) != 0)
		return false;
	decode_tid(v.data, v.len, tid);
	return true;
}

static void
counter_scan_end(ProllyRowIdScan *scan)
{
	if (scan->cursor)
		PROLLY_CALL(engine_for(scan->rel), cursor_close, scan->cursor);
	pfree(scan->buf.data);
	pfree(scan);
}

static ProllyTidState
counter_tid_resolve(ProllyRootSet *roots, Relation rel, ItemPointer tid,
					CommandId cid, StringInfo out, ProllyHash *content,
					uint64 *ordinal, ProllyTidInfo *info)
{
	StringInfoData v;
	ProllyTidState state = PROLLY_TID_NAMED;

	if (!ItemPointerIsValid(tid) ||
		ItemPointerGetOffsetNumber(tid) > MaxHeapTuplesPerPage)
		return PROLLY_TID_UNKNOWN;
	initStringInfo(&v);
	if (!get_entry(engine_for(rel), roots, rel, tid, cid, &v))
		state = PROLLY_TID_UNKNOWN;
	else if (entry_gone(&v))
	{
		decode_gone(&v, info);
		get_gone_row(engine_for(rel), roots, rel, tid, cid, out);
		state = PROLLY_TID_GONE;
	}
	else
	{
		Entry		e;

		decode_entry(&v, &e);
		*content = e.content;
		*ordinal = e.ordinal;
		info->xmin = e.xmin;
		ItemPointerSetInvalid(&info->successor);
		info->cid = InvalidCommandId;
		appendBinaryStringInfo(out, (const char *) e.key.data, e.key.len);
	}
	pfree(v.data);
	return state;
}

/* tid's 'T' entry, which must name a version. */
static void
named_entry(const ProllyEngineRoutine *engine, ProllyRootSet *roots,
			Relation rel, ItemPointer tid, StringInfo v, Entry *e)
{
	if (!get_entry(engine, roots, rel, tid, InvalidCommandId, v) || entry_gone(v))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("TID (%u,%u) of \"%s\" names no row",
						ItemPointerGetBlockNumber(tid),
						ItemPointerGetOffsetNumber(tid),
						RelationGetRelationName(rel))));
	decode_entry(v, e);
}

static void
counter_tid_retire(ProllyRootSet *roots, Relation rel, ItemPointer tid,
				   ItemPointer successor, const ProllyValue *row, CommandId cid)
{
	const ProllyEngineRoutine *engine = engine_for(rel);
	StringInfoData v;
	StringInfoData entry;
	Entry		e;

	initStringInfo(&v);
	named_entry(engine, roots, rel, tid, &v, &e);
	drop_version(engine, roots, rel, &e.key, e.ordinal, tid, cid);
	initStringInfo(&entry);
	encode_gone(successor, cid, e.xmin, &entry);
	put_entry(engine, rel, tid, &entry, cid);
	put_gone_row(engine, rel, tid, row, cid);
	pfree(entry.data);
	pfree(v.data);
}

static void
counter_tid_renew(ProllyRootSet *roots, Relation rel, ItemPointer otid,
				  const ProllyValue *row, CommandId cid, ItemPointer ntid)
{
	const ProllyEngineRoutine *engine = engine_for(rel);
	StringInfoData v;
	StringInfoData entry;
	Entry		e;

	initStringInfo(&v);
	named_entry(engine, roots, rel, otid, &v, &e);
	number_tid(next_number(rel), ntid);
	initStringInfo(&entry);
	encode_entry(&e.content, e.ordinal, GetCurrentTransactionId(), &e.key, &entry);
	put_entry(engine, rel, ntid, &entry, cid);
	put_version(engine, rel, &e.key, e.ordinal, ntid, cid);
	resetStringInfo(&entry);
	encode_gone(ntid, cid, e.xmin, &entry);
	put_entry(engine, rel, otid, &entry, cid);
	put_gone_row(engine, rel, otid, row, cid);
	pfree(entry.data);
	pfree(v.data);
}

static void
counter_tid_rename(ProllyRootSet *roots, Relation rel, ItemPointer tid,
				   const ProllyHash *content, uint64 ordinal,
				   const ProllyKey *key, CommandId cid)
{
	const ProllyEngineRoutine *engine = engine_for(rel);
	StringInfoData v;
	StringInfoData entry;
	Entry		e;

	initStringInfo(&v);
	named_entry(engine, roots, rel, tid, &v, &e);

	/*
	 * The entry at the new key and ordinal is always written: a re-keyed
	 * table's version entries were emptied to be rebuilt in its new key
	 * order.  The entry at the old key and ordinal stays: a reader of the
	 * tree the row was renamed from (a statement whose own write rebuilt the
	 * table while it was scanning it) still finds the TID there, reading the
	 * entries as they were.  Only a version at that key and ordinal is ever
	 * looked up, and writing one replaces the entry.
	 */
	put_version(engine, rel, key, ordinal, tid, cid);
	initStringInfo(&entry);
	encode_entry(content, ordinal, e.xmin, key, &entry);
	put_entry(engine, rel, tid, &entry, cid);
	pfree(entry.data);
	pfree(v.data);
}

const ProllyRowIdRoutine prolly_rowid_counter = {
	.info = {
		.iface = PROLLY_IFACE_ROW_IDENTITY,
		.impl_id = 2,
		.format_version = 3,
		.name = "counter",
		.description = "TIDs from a per-table counter that aborts don't roll "
		"back, with a map both ways",
	},
	.tid_assign = counter_tid_assign,
	.scan_begin = counter_scan_begin,
	.tid_find = counter_tid_find,
	.scan_end = counter_scan_end,
	.tid_resolve = counter_tid_resolve,
	.tid_retire = counter_tid_retire,
	.tid_renew = counter_tid_renew,
	.tid_rename = counter_tid_rename,
	/* tid_forget comes with VACUUM, which must also drop version entries */
};
