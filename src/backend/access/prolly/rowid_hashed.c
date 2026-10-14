/*-------------------------------------------------------------------------
 *
 * rowid_hashed.c
 *	  Row identity implementation "hashed": TIDs derived from row content.
 *
 * A row version (content hash, ordinal) gets the TID
 *
 *	candidate(content, ordinal, probe)
 *
 * a 64-bit hash folded into a valid heap-style item pointer (block number
 * below InvalidBlockNumber, offset 1..MaxHeapTuplesPerPage, about 2^40
 * values), for the first probe = 0, 1, ... whose TID isn't held by another
 * version.  So a scan can compute nearly every row's TID without a lookup;
 * only versions that lost a collision (probe > 0) are recorded as
 * displaced.
 *
 * The relation's row-identity tree holds, keyed by a tag byte:
 *
 *	'D' content-length content ordinal(8, big-endian) -> TID (6 bytes)
 *		for displaced versions only
 *	'T' block(4, big-endian) offset(2, big-endian)
 *		-> content-length content varint-ordinal row-key
 *		for every version that has a TID
 *
 * The hash is local (hash_bytes_extended): TIDs never leave the cluster
 * and are never part of canonical content.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/rowid_hashed.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/prolly_builtins.h"
#include "access/prolly_datum.h"
#include "common/hashfn.h"

#define TAG_DISPLACED	'D'
#define TAG_TID			'T'

/* Collisions this deep mean the map is broken, not unlucky. */
#define MAX_PROBES		64

#define TID_KEY_LEN		7
#define MAX_DKEY_LEN	(1 + 1 + PROLLY_HASH_MAX_LEN + 8)

struct ProllyRowIdScan
{
	ProllyRootSet *roots;
	Relation	rel;
	CommandId	cid;
	bool		has_displaced;
};

static const ProllyEngineRoutine *
engine_for(Relation rel)
{
	ProllyComponents c;

	prolly_components_for_relation(rel, &c);
	return c.engine;
}

static void
candidate(const ProllyHash *content, uint64 ordinal, int probe,
		  ItemPointer tid)
{
	uint64		h = hash_bytes_extended(content->bytes, content->len,
										hash_combine64(ordinal, (uint64) probe));
	BlockNumber blk = (BlockNumber) ((h >> 32) % (uint64) InvalidBlockNumber);
	OffsetNumber off = (OffsetNumber) (1 + (uint32) h % MaxHeapTuplesPerPage);

	ItemPointerSet(tid, blk, off);
}

static ProllyKey
tid_key(ItemPointer tid, uint8 *buf)
{
	BlockNumber blk = ItemPointerGetBlockNumber(tid);
	OffsetNumber off = ItemPointerGetOffsetNumber(tid);
	ProllyKey	key;

	buf[0] = TAG_TID;
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
displaced_key(const ProllyHash *content, uint64 ordinal, uint8 *buf)
{
	ProllyKey	key;
	int			n = 0;

	buf[n++] = TAG_DISPLACED;
	buf[n++] = content->len;
	memcpy(buf + n, content->bytes, content->len);
	n += content->len;
	for (int i = 7; i >= 0; i--)
		buf[n++] = (uint8) (ordinal >> (i * 8));
	key.data = buf;
	key.len = n;
	return key;
}

static void
encode_tid(ItemPointer tid, StringInfo out)
{
	uint8		buf[TID_KEY_LEN];

	(void) tid_key(tid, buf);
	appendBinaryStringInfo(out, (const char *) buf + 1, TID_KEY_LEN - 1);
}

static void
decode_tid(const uint8 *data, Size len, ItemPointer tid)
{
	if (len != TID_KEY_LEN - 1)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid prolly row identity entry")));
	ItemPointerSet(tid,
				   ((BlockNumber) data[0] << 24) | ((BlockNumber) data[1] << 16) |
				   ((BlockNumber) data[2] << 8) | (BlockNumber) data[3],
				   (OffsetNumber) ((data[4] << 8) | data[5]));
}

static void
encode_entry(const ProllyHash *content, uint64 ordinal, const ProllyKey *key,
			 StringInfo out)
{
	appendStringInfoChar(out, (char) content->len);
	appendBinaryStringInfo(out, (const char *) content->bytes, content->len);
	prolly_put_varint(out, ordinal);
	appendBinaryStringInfo(out, (const char *) key->data, key->len);
}

/* Parse a 'T' entry; key points into data. */
static void
decode_entry(const uint8 *data, Size len, ProllyHash *content,
			 uint64 *ordinal, ProllyKey *key)
{
	const char *p = (const char *) data;
	const char *end = p + len;

	if (len < 1 || data[0] == 0 || data[0] > PROLLY_HASH_MAX_LEN ||
		len < 1 + (Size) data[0])
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid prolly row identity entry")));
	memset(content, 0, sizeof(*content));
	content->len = data[0];
	memcpy(content->bytes, data + 1, content->len);
	p += 1 + content->len;
	*ordinal = prolly_get_varint(&p, end);
	key->data = (const uint8 *) p;
	key->len = end - p;
}

static bool
same_version(const ProllyHash *a, uint64 aord, const ProllyHash *b, uint64 bord)
{
	return aord == bord && a->len == b->len &&
		memcmp(a->bytes, b->bytes, a->len) == 0;
}

static bool
same_key(const ProllyKey *a, const ProllyKey *b)
{
	return a->len == b->len && memcmp(a->data, b->data, a->len) == 0;
}

/* ------------------------------------------------------------------------
 * Callbacks
 * ------------------------------------------------------------------------
 */

static void
hashed_tid_assign(ProllyRootSet *roots, Relation rel, const ProllyHash *content,
				  uint64 ordinal, const ProllyKey *key, CommandId cid,
				  ItemPointer tid)
{
	const ProllyEngineRoutine *engine = engine_for(rel);
	uint8		dbuf[MAX_DKEY_LEN];
	uint8		tbuf[TID_KEY_LEN];
	ProllyKey	dkey = displaced_key(content, ordinal, dbuf);
	StringInfoData v;
	StringInfoData entry;
	bool		known = false;

	initStringInfo(&v);
	initStringInfo(&entry);
	encode_entry(content, ordinal, key, &entry);

	/*
	 * Reads see every write of the transaction, this command's included,
	 * so two rows assigned in one command never take the same TID.
	 */

	/* A version that was displaced keeps its TID. */
	if (PROLLY_CALL(engine, get, roots, rel, PROLLY_TREE_ROWID, &dkey,
					InvalidCommandId, &v))
	{
		decode_tid((const uint8 *) v.data, v.len, tid);
		known = true;
	}

	for (int probe = 0; !known; probe++)
	{
		ProllyKey	tkey;
		ProllyHash	held;
		uint64		held_ord;
		ProllyKey	held_key;

		if (probe >= MAX_PROBES)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("could not assign a TID in \"%s\" after %d probes",
							RelationGetRelationName(rel), MAX_PROBES)));
		candidate(content, ordinal, probe, tid);
		tkey = tid_key(tid, tbuf);
		resetStringInfo(&v);
		if (!PROLLY_CALL(engine, get, roots, rel, PROLLY_TREE_ROWID, &tkey,
						 InvalidCommandId, &v))
		{
			ProllyValue value = {(const uint8 *) entry.data, entry.len};

			PROLLY_CALL(engine, put, rel, PROLLY_TREE_ROWID, &tkey, &value, cid);
			if (probe > 0)
			{
				StringInfoData t;
				ProllyValue tv;

				initStringInfo(&t);
				encode_tid(tid, &t);
				tv.data = (const uint8 *) t.data;
				tv.len = t.len;
				PROLLY_CALL(engine, put, rel, PROLLY_TREE_ROWID, &dkey, &tv, cid);
				pfree(t.data);
			}
			pfree(v.data);
			pfree(entry.data);
			return;
		}
		decode_entry((const uint8 *) v.data, v.len, &held, &held_ord, &held_key);
		if (same_version(&held, held_ord, content, ordinal))
			known = true;
	}

	/* Known version: keep its entry's key current. */
	{
		uint8		buf[TID_KEY_LEN];
		ProllyKey	tkey = tid_key(tid, buf);
		ProllyHash	held;
		uint64		held_ord;
		ProllyKey	held_key;

		resetStringInfo(&v);
		if (!PROLLY_CALL(engine, get, roots, rel, PROLLY_TREE_ROWID, &tkey,
						 InvalidCommandId, &v))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("displaced TID (%u,%u) of \"%s\" has no entry",
							ItemPointerGetBlockNumber(tid),
							ItemPointerGetOffsetNumber(tid),
							RelationGetRelationName(rel))));
		decode_entry((const uint8 *) v.data, v.len, &held, &held_ord, &held_key);
		if (!same_key(&held_key, key))
		{
			ProllyValue value = {(const uint8 *) entry.data, entry.len};

			PROLLY_CALL(engine, put, rel, PROLLY_TREE_ROWID, &tkey, &value, cid);
		}
	}
	pfree(v.data);
	pfree(entry.data);
}

/*
 * The TID a version has now: its displaced record, or its first free
 * candidate's.  Reads see every write of the transaction.
 */
static bool
current_tid(const ProllyEngineRoutine *engine, ProllyRootSet *roots,
			Relation rel, const ProllyHash *content, uint64 ordinal,
			ItemPointer tid, bool *displaced)
{
	uint8		dbuf[MAX_DKEY_LEN];
	ProllyKey	dkey = displaced_key(content, ordinal, dbuf);
	StringInfoData v;

	initStringInfo(&v);
	*displaced = false;
	if (PROLLY_CALL(engine, get, roots, rel, PROLLY_TREE_ROWID, &dkey,
					InvalidCommandId, &v))
	{
		decode_tid((const uint8 *) v.data, v.len, tid);
		*displaced = true;
		pfree(v.data);
		return true;
	}
	for (int probe = 0; probe < MAX_PROBES; probe++)
	{
		uint8		tbuf[TID_KEY_LEN];
		ProllyKey	tkey;
		ProllyHash	held;
		uint64		held_ord;
		ProllyKey	held_key;

		candidate(content, ordinal, probe, tid);
		tkey = tid_key(tid, tbuf);
		resetStringInfo(&v);
		if (!PROLLY_CALL(engine, get, roots, rel, PROLLY_TREE_ROWID, &tkey,
						 InvalidCommandId, &v))
			break;
		decode_entry((const uint8 *) v.data, v.len, &held, &held_ord, &held_key);
		if (same_version(&held, held_ord, content, ordinal))
		{
			pfree(v.data);
			return true;
		}
	}
	pfree(v.data);
	return false;
}

static void
hashed_tid_rename(ProllyRootSet *roots, Relation rel,
				  const ProllyHash *old_content, uint64 old_ordinal,
				  const ProllyHash *new_content, uint64 new_ordinal,
				  const ProllyKey *key, CommandId cid)
{
	const ProllyEngineRoutine *engine = engine_for(rel);
	ItemPointerData tid;
	ItemPointerData first;
	bool		displaced;
	uint8		tbuf[TID_KEY_LEN];
	uint8		dbuf[MAX_DKEY_LEN];
	ProllyKey	tkey;
	StringInfoData entry;
	ProllyValue value;

	if (!current_tid(engine, roots, rel, old_content, old_ordinal, &tid, &displaced))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("a row of \"%s\" has no TID", RelationGetRelationName(rel))));

	/* The TID now names the new content... */
	tkey = tid_key(&tid, tbuf);
	initStringInfo(&entry);
	encode_entry(new_content, new_ordinal, key, &entry);
	value.data = (const uint8 *) entry.data;
	value.len = entry.len;
	PROLLY_CALL(engine, put, rel, PROLLY_TREE_ROWID, &tkey, &value, cid);

	/* ...and the new content finds it, unless it is its first choice. */
	if (displaced)
	{
		ProllyKey	odkey = displaced_key(old_content, old_ordinal, dbuf);

		PROLLY_CALL(engine, remove, rel, PROLLY_TREE_ROWID, &odkey, cid);
	}
	candidate(new_content, new_ordinal, 0, &first);
	if (!ItemPointerEquals(&first, &tid))
	{
		ProllyKey	ndkey = displaced_key(new_content, new_ordinal, dbuf);
		StringInfoData t;
		ProllyValue tv;

		initStringInfo(&t);
		encode_tid(&tid, &t);
		tv.data = (const uint8 *) t.data;
		tv.len = t.len;
		PROLLY_CALL(engine, put, rel, PROLLY_TREE_ROWID, &ndkey, &tv, cid);
		pfree(t.data);
	}
	pfree(entry.data);
}

static ProllyRowIdScan *
hashed_scan_begin(ProllyRootSet *roots, Relation rel, CommandId cid)
{
	const ProllyEngineRoutine *engine = engine_for(rel);
	ProllyRowIdScan *scan = palloc0(sizeof(ProllyRowIdScan));
	ProllyCursor *c;
	uint8		tag = TAG_DISPLACED;
	ProllyKey	start = {&tag, 1};
	ProllyKey	key;
	ProllyValue value;

	scan->roots = roots;
	scan->rel = rel;
	scan->cid = cid;

	c = PROLLY_CALL(engine, cursor_open, roots, rel, PROLLY_TREE_ROWID, cid);
	PROLLY_CALL(engine, cursor_seek, c, &start, ForwardScanDirection);
	scan->has_displaced =
		PROLLY_CALL(engine, cursor_next, c, &key, &value) &&
		key.len > 0 && key.data[0] == TAG_DISPLACED;
	PROLLY_CALL(engine, cursor_close, c);
	return scan;
}

static bool
hashed_tid_find(ProllyRowIdScan *scan, const ProllyHash *content,
				uint64 ordinal, ItemPointer tid)
{
	if (scan->has_displaced)
	{
		const ProllyEngineRoutine *engine = engine_for(scan->rel);
		uint8		dbuf[MAX_DKEY_LEN];
		ProllyKey	dkey = displaced_key(content, ordinal, dbuf);
		StringInfoData v;
		bool		found;

		initStringInfo(&v);
		found = PROLLY_CALL(engine, get, scan->roots, scan->rel,
							PROLLY_TREE_ROWID, &dkey, scan->cid, &v);
		if (found)
			decode_tid((const uint8 *) v.data, v.len, tid);
		pfree(v.data);
		if (found)
			return true;
	}
	candidate(content, ordinal, 0, tid);
	return true;
}

static void
hashed_scan_end(ProllyRowIdScan *scan)
{
	pfree(scan);
}

static bool
hashed_tid_resolve(ProllyRootSet *roots, Relation rel, ItemPointer tid,
				   CommandId cid, StringInfo key_out, ProllyHash *content,
				   uint64 *ordinal)
{
	const ProllyEngineRoutine *engine = engine_for(rel);
	uint8		buf[TID_KEY_LEN];
	ProllyKey	tkey;
	ProllyKey	key;
	StringInfoData v;

	if (!ItemPointerIsValid(tid))
		return false;
	tkey = tid_key(tid, buf);
	initStringInfo(&v);
	if (!PROLLY_CALL(engine, get, roots, rel, PROLLY_TREE_ROWID, &tkey, cid, &v))
	{
		pfree(v.data);
		return false;
	}
	decode_entry((const uint8 *) v.data, v.len, content, ordinal, &key);
	appendBinaryStringInfo(key_out, (const char *) key.data, key.len);
	pfree(v.data);
	return true;
}

const ProllyRowIdRoutine prolly_rowid_hashed = {
	.info = {
		.iface = PROLLY_IFACE_ROW_IDENTITY,
		.impl_id = 1,
		.format_version = 1,
		.name = "hashed",
		.description = "TIDs hashed from row content, with a map back to rows",
	},
	.tid_assign = hashed_tid_assign,
	.scan_begin = hashed_scan_begin,
	.tid_find = hashed_tid_find,
	.scan_end = hashed_scan_end,
	.tid_resolve = hashed_tid_resolve,
	.tid_rename = hashed_tid_rename,
	/* tid_forget comes with VACUUM, which must also drop 'D' entries */
};
