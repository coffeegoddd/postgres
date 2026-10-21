/*-------------------------------------------------------------------------
 *
 * changelog_relation.c
 *	  Change log implementation "relation": a prolly table's entries in a
 *	  relation of their own.
 *
 * A prolly table's log is pg_toast.pg_prolly_log_<table's OID>, made with
 * the table's access method as a TOAST-kind relation: block-mapped and
 * versioned with its table (a branch's copy shares its pages as the
 * table's are shared), passed over by the canonical form, and dropped with
 * the table, on which it depends internally.  It has no TOAST table of its
 * own.  Format 2: its rows are a transaction's entries, packed:
 *
 *	xid		int8	the writing transaction's top-level FullTransactionId
 *	entries	bytea	entries, each a count delta (int4: 0 for a key's, the
 *					sum of +1s and -1s for a keyless row's), an image's
 *					length (uint4; all ones: read the table in full) and
 *					the image (the entry's tuple from t_infomask2 on)
 *
 * with a btree on xid.  A log of format 1 (an image and a delta a row, no
 * index) is taken as none: the table is folded in full until one is made
 * anew.  An entry is made as each tuple version is noted, buffered by the
 * transaction, and written in a row by the (sub)transaction that holds it
 * (see the buffer, below): seen exactly when the write is.  A table with a
 * primary key (checked immediately) gets the version's key columns, their
 * values out of line brought inline (compressed ones stay compressed),
 * once for an update that kept the key; one without gets the version's
 * tuple as stored, its values out of line as their hash pointers (by
 * content in the chunk store, where nothing deletes them with the row, and
 * a collection keeps what a pointer on the log's pages names), unless one
 * is out of line otherwise (the value may be gone by the time the entry is
 * read).  Either, too long for a page of the
 * log with the entry around it (a key's compressed values needn't be as
 * its index holds them), is an entry to read the table in full instead.  A
 * truncation or a rewrite of the table is that too.
 *
 * Reads go by the index, from a fold's position: the rows of transactions
 * it didn't see finished.  They judge each row by the snapshot as a heap
 * scan would, but take no predicate locks: a fold reading the log mustn't
 * make every writer of the table conflict with it.  Rows are deleted only
 * by their own transaction's fold (consumed) and once every snapshot sees
 * their transaction finished (trimmed).
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/changelog_relation.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/stratnum.h"
#include "access/transam.h"
#include "access/htup_details.h"
#include "access/prolly_builtins.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/toast_compression.h"
#include "access/xact.h"
#include "catalog/branch.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_opclass_d.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "nodes/makefuncs.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"

#define FORMAT_VERSION		2

#define Anum_log_xid		1
#define Anum_log_entries	2
#define Natts_log			2

/* An entry's length that says to read the table in full. */
#define FULL_LEN			0xFFFFFFFF

/*
 * The most a row's packed entries hold: a log tuple (its header, the
 * transaction id and the bytea's length word, aligned) must fit a page.
 */
#define MAX_ROW_ENTRIES \
	(MaxHeapTupleSize - MAXALIGN(SizeofHeapTupleHeader + sizeof(int64)) - \
	 VARHDRSZ - MAXIMUM_ALIGNOF)

/* The longest image an entry holds: one entry, alone in a row. */
#define MAX_IMAGE_LEN		(MAX_ROW_ENTRIES - 2 * sizeof(uint32))

#define LOG_PREFIX			"pg_prolly_log_"

static void
log_name(Oid relid, char *name)
{
	snprintf(name, NAMEDATALEN, LOG_PREFIX "%u", relid);
}

/*
 * logid, if it is a log of this format (its first column the transaction
 * id); InvalidOid if not (none, or one of format 1).
 */
static Oid
log_of_format(Oid logid)
{
	char	   *first;
	bool		ours;

	if (!OidIsValid(logid))
		return InvalidOid;
	first = get_attname(logid, Anum_log_xid, true);
	ours = first != NULL && strcmp(first, "xid") == 0;
	if (first)
		pfree(first);
	return ours ? logid : InvalidOid;
}

/*
 * The log's OID, cached in the table's relcache entry (rd_amcache), which
 * an invalidation of the table drops: the log's creation sends one, and a
 * branch checked out drops every entry.  Tagged, so that another of the
 * AM's components caching there isn't read as this.
 */
#define LOG_CACHE_MAGIC		0x50524c47	/* "PRLG" */

typedef struct LogCache
{
	uint32		magic;
	Oid			logid;			/* InvalidOid: none */
} LogCache;

/*
 * The log of rel, a prolly table (or materialized view); InvalidOid if none.
 * A transient heap a rewrite fills (relrewrite set) has none: its table is
 * told of the rewrite as a whole.
 */
static Oid
log_relid(Relation rel)
{
	char		name[NAMEDATALEN];
	LogCache   *lc = rel->rd_amcache;
	Oid			logid;

	if (rel->rd_rel->relam != PROLLY_TABLE_AM_OID ||
		(rel->rd_rel->relkind != RELKIND_RELATION &&
		 rel->rd_rel->relkind != RELKIND_MATVIEW) ||
		OidIsValid(rel->rd_rel->relrewrite))
		return InvalidOid;
	if (lc != NULL && lc->magic == LOG_CACHE_MAGIC)
		return lc->logid;
	log_name(RelationGetRelid(rel), name);
	logid = log_of_format(get_relname_relid(name, PG_TOAST_NAMESPACE));
	if (rel->rd_amcache == NULL)
	{
		lc = MemoryContextAlloc(CacheMemoryContext, sizeof(LogCache));
		lc->magic = LOG_CACHE_MAGIC;
		lc->logid = logid;
		rel->rd_amcache = lc;
	}
	return logid;
}

static void
relation_create(Relation rel)
{
	char		name[NAMEDATALEN];
	TupleDesc	desc;
	Oid			logid;
	ObjectAddress table;
	ObjectAddress log;

	/*
	 * (not as pg_upgrade restores the table: it preassigns the relation
	 * numbers of what it will move, and a log's own, unassigned, could be
	 * one of them; the table has no log, and is folded in full, until an
	 * ALTER makes one)
	 */
	if (IsBinaryUpgrade)
		return;
	log_name(RelationGetRelid(rel), name);

	/* (one of format 1 made anew: it goes first, its entries unread) */
	{
		Oid			old = get_relname_relid(name, PG_TOAST_NAMESPACE);

		if (OidIsValid(old))
		{
			ObjectAddress was;

			deleteDependencyRecordsFor(RelationRelationId, old, false);
			CommandCounterIncrement();
			ObjectAddressSet(was, RelationRelationId, old);
			performDeletion(&was, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);
			CommandCounterIncrement();
		}
	}
	desc = CreateTemplateTupleDesc(Natts_log);
	TupleDescInitEntry(desc, Anum_log_xid, "xid", INT8OID, -1, 0);
	TupleDescInitEntry(desc, Anum_log_entries, "entries", BYTEAOID, -1, 0);

	/* (an entry is never itself moved out of line, nor compressed) */
	for (int i = 0; i < Natts_log; i++)
	{
		TupleDescAttr(desc, i)->attstorage = TYPSTORAGE_PLAIN;
		TupleDescAttr(desc, i)->attcompression = InvalidCompressionMethod;
		populate_compact_attribute(desc, i);
	}
	TupleDescFinalize(desc);

	logid = heap_create_with_catalog(name,
									 PG_TOAST_NAMESPACE,
									 rel->rd_rel->reltablespace,
									 InvalidOid,
									 InvalidOid,
									 InvalidOid,
									 rel->rd_rel->relowner,
									 rel->rd_rel->relam,
									 desc,
									 NIL,
									 RELKIND_TOASTVALUE,
									 rel->rd_rel->relpersistence,
									 false,
									 false,
									 ONCOMMIT_NOOP,
									 (Datum) 0,
									 false,
									 true,
									 true,
									 InvalidOid,
									 NULL);

	/* (its index on the transaction ids, a fold's reads by position) */
	CommandCounterIncrement();
	{
		Relation	logrel = table_open(logid, ShareLock);
		IndexInfo  *ii = makeIndexInfo(1, 1, BTREE_AM_OID, NIL, NIL,
									   false, false, true, false, false,
									   false);
		Oid			collations[1] = {InvalidOid};
		Oid			opclasses[1] = {INT8_BTREE_OPS_OID};
		int16		coloptions[1] = {0};
		char		idxname[NAMEDATALEN];

		ii->ii_IndexAttrNumbers[0] = Anum_log_xid;
		snprintf(idxname, NAMEDATALEN, "%s_index", name);
		index_create(logrel, idxname, InvalidOid, InvalidOid, InvalidOid,
					 InvalidOid, ii, list_make1("xid"), BTREE_AM_OID,
					 rel->rd_rel->reltablespace, collations, opclasses, NULL,
					 coloptions, NULL, (Datum) 0,
					 INDEX_CREATE_SUPPRESS_PROGRESS, 0, true, true, NULL);
		table_close(logrel, NoLock);
	}

	/* (part of its table: dropped with it, never alone) */
	ObjectAddressSet(table, RelationRelationId, RelationGetRelid(rel));
	ObjectAddressSet(log, RelationRelationId, logid);
	recordDependencyOn(&log, &table, DEPENDENCY_INTERNAL);

	/* (the table's cached log OID, here and in other sessions, is stale) */
	CacheInvalidateRelcache(rel);
}

/* (its OID: a branch's copy of it keeps the OID, with the entries) */
static uint64
relation_identity(Relation rel)
{
	return (uint64) log_relid(rel);
}

/* A copy of the tuple version at tid, read under a share lock of its page. */
static HeapTuple
read_version(Relation rel, const ItemPointerData *tid)
{
	Buffer		buf = ReadBuffer(rel, ItemPointerGetBlockNumber(tid));
	Page		page;
	OffsetNumber off = ItemPointerGetOffsetNumber(tid);
	ItemId		lp;
	HeapTupleData tup;
	HeapTuple	copy = NULL;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	lp = off <= PageGetMaxOffsetNumber(page) ? PageGetItemId(page, off) : NULL;
	if (lp != NULL && ItemIdIsNormal(lp))
	{
		tup.t_data = (HeapTupleHeader) PageGetItem(page, lp);
		tup.t_len = ItemIdGetLength(lp);
		tup.t_tableOid = RelationGetRelid(rel);
		tup.t_self = *tid;
		copy = heap_copytuple(&tup);
	}
	UnlockReleaseBuffer(buf);
	if (copy == NULL)
		elog(ERROR, "tuple (%u,%u) of \"%s\" noted as written is not there",
			 ItemPointerGetBlockNumber(tid), off, RelationGetRelationName(rel));
	return copy;
}

/*
 * The version's key columns, every other column null, values out of line
 * brought inline.
 */
static HeapTuple
key_image(Relation rel, HeapTuple version, const Bitmapset *keyattrs)
{
	TupleDesc	desc = RelationGetDescr(rel);
	Datum	   *values = palloc_array(Datum, desc->natts);
	bool	   *isnull = palloc_array(bool, desc->natts);
	HeapTuple	image;

	heap_deform_tuple(version, desc, values, isnull);
	for (int i = 0; i < desc->natts; i++)
	{
		if (!bms_is_member(i + 1 - FirstLowInvalidHeapAttributeNumber, keyattrs))
			isnull[i] = true;
		else if (!isnull[i] && TupleDescAttr(desc, i)->attlen == -1 &&
				 VARATT_IS_EXTERNAL(DatumGetPointer(values[i])))
			values[i] = PointerGetDatum(detoast_external_attr((varlena *)
															  DatumGetPointer(values[i])));
	}
	image = heap_form_tuple(desc, values, isnull);
	pfree(values);
	pfree(isnull);
	return image;
}

/* A write's entries: the images and deltas a note makes, for the buffer. */
typedef struct Entries
{
	Relation	log;			/* (its OID: the buffer's key) */
	int			n;
	int			max;
	HeapTuple  *images;			/* NULL: read the table in full */
	int32	   *deltas;
} Entries;

/* An entry: image (NULL: read the table in full) and delta. */
static void
insert_entry(Entries *e, HeapTuple image, int32 delta)
{
	if (e->n == e->max)
	{
		e->max = Max(8, e->max * 2);
		e->images = e->images ? repalloc_array(e->images, HeapTuple, e->max) :
			palloc_array(HeapTuple, e->max);
		e->deltas = e->deltas ? repalloc_array(e->deltas, int32, e->max) :
			palloc_array(int32, e->max);
	}
	e->images[e->n] = image;
	e->deltas[e->n++] = delta;
}

/* ----------------------------------------------------------------
 * The transaction's entries, buffered (format 2): per log and
 * subtransaction, a key's image once, a keyless row's image once with its
 * deltas summed.  A subtransaction committing hands what it holds to its
 * parent; one aborting drops it.  They are written at PRE_COMMIT and
 * PRE_PREPARE (all its parent's by then), before the log is read in the
 * transaction, before the session's branch changes or a branch's copies are
 * made, and past BUFFER_FLUSH_BYTES: as log rows of at most a page each, by
 * the (sub)transaction current then.  An ancestor's entries written so, by a
 * subtransaction that then aborts, would go with it: a copy of them is kept
 * till it ends, and put back in the ancestor's buffer if it aborts.
 *
 * The transaction's rows in a log are found by the index (their xid its
 * own); a log it wrote a row of saying to read the table in full is noted,
 * as consume_own() can't consume such changes.
 * ----------------------------------------------------------------
 */
#define BUFFER_FLUSH_BYTES	(64 * 1024)

typedef struct BufKey
{
	uint32		len;
	const char *data;			/* the image's bytes, from t_infomask2 on */
} BufKey;

typedef struct BufEntry
{
	BufKey		key;			/* hash key */
	int32		delta;
	bool		row;			/* a keyless row's (counted), not a key's */
} BufEntry;

typedef struct BufTag
{
	Oid			logid;
	SubTransactionId subid;		/* the (sub)transaction whose entries */
} BufTag;

typedef struct LogBuffer
{
	BufTag		tag;			/* hash key */
	HTAB	   *entries;		/* BufEntry */
	MemoryContext cxt;			/* the entries and their images */
	Size		bytes;			/* their packed size */
	bool		full;			/* one says to read the table in full */
} LogBuffer;

/* An ancestor's entries a subtransaction wrote, kept till it ends. */
typedef struct WrittenSet
{
	SubTransactionId writer;
	LogBuffer	buf;			/* buf.tag.subid: the ancestor */
} WrittenSet;

typedef struct OwnLog
{
	Oid			logid;			/* hash key */
	bool		full;			/* a row of the transaction's says to read
								 * the table in full */
} OwnLog;

static HTAB *buffers = NULL;	/* LogBuffer; the transaction's */
static List *written = NIL;		/* WrittenSet *; the transaction's */
static HTAB *own_logs = NULL;	/* OwnLog; the transaction's */
static bool callbacks_registered = false;

static void flush_all(void);
static Oid	log_index_relid(Relation log);

static uint32
bufkey_hash(const void *key, Size keysize)
{
	const BufKey *k = key;

	return hash_bytes((const unsigned char *) k->data, (int) k->len);
}

static int
bufkey_match(const void *a, const void *b, Size keysize)
{
	const BufKey *x = a;
	const BufKey *y = b;

	if (x->len != y->len)
		return 1;
	return memcmp(x->data, y->data, x->len);
}

/* A buffer's entries, empty, in a memory context of their own. */
static void
buffer_init(LogBuffer *b)
{
	HASHCTL		ctl;

	b->cxt = AllocSetContextCreate(TopTransactionContext,
								   "prolly change log buffer",
								   ALLOCSET_DEFAULT_SIZES);
	ctl.keysize = sizeof(BufKey);
	ctl.entrysize = sizeof(BufEntry);
	ctl.hash = bufkey_hash;
	ctl.match = bufkey_match;
	ctl.hcxt = b->cxt;
	b->entries = hash_create("prolly change log entries", 64, &ctl,
							 HASH_ELEM | HASH_FUNCTION | HASH_COMPARE |
							 HASH_CONTEXT);
	b->bytes = 0;
	b->full = false;
}

/* Add an entry to a buffer. */
static void
buffer_add(LogBuffer *b, const char *data, uint32 len, int32 delta, bool row)
{
	BufKey		key = {len, data};
	BufEntry   *be;
	bool		found;

	be = hash_search(b->entries, &key, HASH_ENTER, &found);
	if (!found)
	{
		char	   *copy = MemoryContextAlloc(b->cxt, Max(len, 1));

		memcpy(copy, data, len);
		be->key.data = copy;
		be->key.len = len;
		be->delta = 0;
		be->row = false;
		b->bytes += 2 * sizeof(uint32) + len;
	}
	be->delta += delta;
	be->row = be->row || row;
}

/* Add what src holds to dst. */
static void
buffer_merge(LogBuffer *dst, const LogBuffer *src)
{
	HASH_SEQ_STATUS seq;
	BufEntry   *be;

	hash_seq_init(&seq, src->entries);
	while ((be = hash_seq_search(&seq)) != NULL)
		buffer_add(dst, be->key.data, be->key.len, be->delta, be->row);
	dst->full = dst->full || src->full;
}

/* The buffer of a log for a (sub)transaction, made if need be. */
static LogBuffer *
buffer_of(Oid logid, SubTransactionId subid)
{
	BufTag		tag = {logid, subid};
	LogBuffer  *b;
	bool		found;

	if (buffers == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(BufTag);
		ctl.entrysize = sizeof(LogBuffer);
		ctl.hcxt = TopTransactionContext;
		buffers = hash_create("prolly change log buffers", 16, &ctl,
							  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	b = hash_search(buffers, &tag, HASH_ENTER, &found);
	if (!found)
		buffer_init(b);
	return b;
}

/* Forget what the transaction holds and wrote (its memory goes with it). */
static void
forget_all(void)
{
	buffers = NULL;
	written = NIL;
	own_logs = NULL;
}

static void
log_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PRE_PREPARE:
			flush_all();
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
		case XACT_EVENT_PREPARE:
			forget_all();
			break;
		default:
			break;
	}
}

static void
log_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
					 SubTransactionId parentSubid, void *arg)
{
	HASH_SEQ_STATUS seq;
	LogBuffer  *b;
	List	   *kept = NIL;

	if (event != SUBXACT_EVENT_COMMIT_SUB && event != SUBXACT_EVENT_ABORT_SUB)
		return;
	if (buffers)
	{
		List	   *mine = NIL;

		hash_seq_init(&seq, buffers);
		while ((b = hash_seq_search(&seq)) != NULL)
			if (b->tag.subid == mySubid)
				mine = lappend(mine, b);
		foreach_ptr(LogBuffer, m, mine)
		{
			BufTag		tag = m->tag;

			if (event == SUBXACT_EVENT_COMMIT_SUB)
				buffer_merge(buffer_of(tag.logid, parentSubid), m);
			MemoryContextDelete(m->cxt);
			(void) hash_search(buffers, &tag, HASH_REMOVE, NULL);
		}
		list_free(mine);
	}
	foreach_ptr(WrittenSet, ws, written)
	{
		if (ws->writer != mySubid)
		{
			kept = lappend(kept, ws);
			continue;
		}
		if (event == SUBXACT_EVENT_ABORT_SUB)
		{
			/* (its rows go with it: what was its ancestor's is held again) */
			buffer_merge(buffer_of(ws->buf.tag.logid, ws->buf.tag.subid), &ws->buf);
			MemoryContextDelete(ws->buf.cxt);
			pfree(ws);
		}
		else if (ws->buf.tag.subid == parentSubid)
		{
			/* (its rows are the parent's now, as the entries were) */
			MemoryContextDelete(ws->buf.cxt);
			pfree(ws);
		}
		else
		{
			ws->writer = parentSubid;
			kept = lappend(kept, ws);
		}
	}
	list_free(written);
	written = kept;
}

static void
register_callbacks(void)
{
	if (callbacks_registered)
		return;
	RegisterXactCallback(log_xact_callback, NULL);
	RegisterSubXactCallback(log_subxact_callback, NULL);
	callbacks_registered = true;
}

/* An image's bytes as an entry holds them: from t_infomask2 on. */
#define IMAGE_SKIP	offsetof(HeapTupleHeaderData, t_infomask2)

static void flush_buffer(LogBuffer *b);

/* Hold a write's entries; written past BUFFER_FLUSH_BYTES. */
static void
buffer_entries(Entries *e)
{
	LogBuffer  *b;

	register_callbacks();
	b = buffer_of(RelationGetRelid(e->log), GetCurrentSubTransactionId());
	for (int i = 0; i < e->n; i++)
	{
		HeapTuple	image = e->images[i];

		if (image == NULL)
		{
			b->full = true;
			continue;
		}
		{
			/*
			 * (the header's transaction bits cleared: a keyless row's
			 * version made and the same taken out are one row, its deltas
			 * summed; a reader deforms the image, never asks them)
			 */
			uint32		len = image->t_len - IMAGE_SKIP;
			char	   *bytes = palloc(len);
			HeapTupleHeader h;

			memcpy(bytes, (const char *) image->t_data + IMAGE_SKIP, len);
			h = (HeapTupleHeader) (bytes - IMAGE_SKIP);
			h->t_infomask2 &= ~HEAP2_XACT_MASK;
			h->t_infomask &= ~HEAP_XACT_MASK;
			buffer_add(b, bytes, len, e->deltas[i], e->deltas[i] != 0);
			pfree(bytes);
		}
	}
	if (b->bytes > BUFFER_FLUSH_BYTES)
		flush_buffer(b);
}

/* Note that the transaction wrote a log a row saying to read in full. */
static void
own_full(Oid logid)
{
	OwnLog	   *ol;
	bool		found;

	register_callbacks();
	if (own_logs == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(OwnLog);
		ctl.hcxt = TopTransactionContext;
		own_logs = hash_create("prolly own change logs", 16, &ctl,
							   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	ol = hash_search(own_logs, &logid, HASH_ENTER, &found);
	ol->full = true;
}

/* One log row: the transaction's id, and the packed entries. */
static void
insert_row_packed(Relation log, Relation index, IndexInfo *ii, StringInfo packed)
{
	Datum		values[Natts_log];
	bool		isnull[Natts_log] = {0};
	bytea	   *b = palloc(VARHDRSZ + packed->len);
	HeapTuple	tup;

	SET_VARSIZE(b, VARHDRSZ + packed->len);
	memcpy(VARDATA(b), packed->data, packed->len);
	values[Anum_log_xid - 1] =
		Int64GetDatum((int64) U64FromFullTransactionId(GetTopFullTransactionId()));
	values[Anum_log_entries - 1] = PointerGetDatum(b);
	tup = heap_form_tuple(RelationGetDescr(log), values, isnull);
	simple_heap_insert(log, tup);
	index_insert(index, values, isnull, &tup->t_self, log, UNIQUE_CHECK_NO,
				 false, ii);
	heap_freetuple(tup);
	pfree(b);
	resetStringInfo(packed);
}

/* Append an entry to the row being packed, starting another when full. */
static void
pack_entry(Relation log, Relation index, IndexInfo *ii, StringInfo packed,
		   int32 delta, uint32 len, const char *data)
{
	uint32		need = 2 * sizeof(uint32) + (len == FULL_LEN ? 0 : len);

	if (packed->len > 0 && packed->len + need > MAX_ROW_ENTRIES)
		insert_row_packed(log, index, ii, packed);
	appendBinaryStringInfo(packed, (const char *) &delta, sizeof(delta));
	appendBinaryStringInfo(packed, (const char *) &len, sizeof(len));
	if (len != FULL_LEN)
		appendBinaryStringInfo(packed, data, len);
}

/*
 * Write what a log's buffer holds, as the current (sub)transaction's.  An
 * ancestor's entries are kept, till the current one ends.
 */
static void
flush_buffer(LogBuffer *b)
{
	Relation	log;
	Relation	index;
	IndexInfo  *ii;
	StringInfoData packed;
	HASH_SEQ_STATUS seq;
	BufEntry   *be;
	SubTransactionId current = GetCurrentSubTransactionId();
	BufTag		tag = b->tag;

	if (hash_get_num_entries(b->entries) == 0 && !b->full)
		return;
	/* (a log gone since, with its table: its entries are of nothing) */
	log = try_table_open(tag.logid, RowExclusiveLock);
	if (log == NULL)
	{
		MemoryContextDelete(b->cxt);
		(void) hash_search(buffers, &tag, HASH_REMOVE, NULL);
		return;
	}
	index = index_open(log_index_relid(log), RowExclusiveLock);
	ii = BuildIndexInfo(index);
	initStringInfo(&packed);
	BRANCH_WRITES_HUSHED(
	{
		if (b->full)
			pack_entry(log, index, ii, &packed, 0, FULL_LEN, NULL);
		hash_seq_init(&seq, b->entries);
		while ((be = hash_seq_search(&seq)) != NULL)
		{
			/* (a keyless row made and taken out again: no change) */
			if (be->row && be->delta == 0)
				continue;
			pack_entry(log, index, ii, &packed, be->delta, be->key.len,
					   be->key.data);
		}
		if (packed.len > 0)
			insert_row_packed(log, index, ii, &packed);
	});
	pfree(packed.data);
	index_close(index, NoLock);
	table_close(log, NoLock);
	if (b->full)
		own_full(tag.logid);

	if (tag.subid != current && GetCurrentTransactionNestLevel() > 1)
	{
		/* (an ancestor's, written by this one: kept till this one ends) */
		WrittenSet *ws = MemoryContextAlloc(TopTransactionContext,
											sizeof(WrittenSet));
		MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);

		ws->writer = current;
		ws->buf = *b;
		written = lappend(written, ws);
		MemoryContextSwitchTo(old);
		(void) hash_search(buffers, &tag, HASH_REMOVE, NULL);
		return;
	}
	MemoryContextDelete(b->cxt);
	(void) hash_search(buffers, &tag, HASH_REMOVE, NULL);
}

/* Write the buffers of one log (InvalidOid: of every one). */
static bool
flush_logs(Oid logid)
{
	HASH_SEQ_STATUS seq;
	LogBuffer  *b;
	List	   *some = NIL;
	bool		any = false;

	if (buffers == NULL)
		return false;
	hash_seq_init(&seq, buffers);
	while ((b = hash_seq_search(&seq)) != NULL)
		if (!OidIsValid(logid) || b->tag.logid == logid)
			some = lappend(some, b);
	foreach_ptr(LogBuffer, one, some)
	{
		if (hash_get_num_entries(one->entries) > 0 || one->full)
			any = true;
		flush_buffer(one);
	}
	list_free(some);
	return any;
}

static void
flush_all(void)
{
	(void) flush_logs(InvalidOid);
}

/*
 * Write a log's buffered entries, before the transaction reads it; true if
 * there were any.  They are the transaction's writes the reader's snapshot
 * sees, but written by a command after it was taken.
 */
static bool
flush_log(Oid logid)
{
	if (!flush_logs(logid))
		return false;
	CommandCounterIncrement();
	return true;
}

/*
 * The snapshot, seeing the transaction's own rows written just now too: a
 * copy, at the current command (the others' as the snapshot has them),
 * pushed: the caller pops it.
 */
static Snapshot
push_with_own(Snapshot snapshot)
{
	PushCopiedSnapshot(snapshot);
	UpdateActiveSnapshotCommandId();
	return GetActiveSnapshot();
}

/*
 * Does a version of rel's hold a value out of line other than by a hash
 * pointer?  A hash pointer names its value by content in the chunk store,
 * where nothing deletes it with the row, and a collection keeps every tree
 * a pointer on a page of the store names, the log's pages too: it can be
 * read when the entry is.  Any other may be gone by then.
 */
static bool
holds_unkept_value(Relation rel, HeapTuple version)
{
	TupleDesc	desc = RelationGetDescr(rel);
	Datum	   *values;
	bool	   *nulls;
	bool		unkept = false;

	if (!HeapTupleHasExternal(version))
		return false;
	values = palloc_array(Datum, desc->natts);
	nulls = palloc_array(bool, desc->natts);
	heap_deform_tuple(version, desc, values, nulls);
	for (int i = 0; i < desc->natts && !unkept; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (nulls[i] || att->attlen != -1)
			continue;
		if (VARATT_IS_EXTERNAL(DatumGetPointer(values[i])) &&
			!VARATT_IS_EXTERNAL_ONDISK_HASH(DatumGetPointer(values[i])))
			unkept = true;
	}
	pfree(values);
	pfree(nulls);
	return unkept;
}

/*
 * A keyless row's entry: its tuple as stored, its values out of line by
 * their hash pointers, unless it can't be kept.
 */
static void
insert_row(Relation rel, Entries *e, HeapTuple version, int32 delta)
{
	if (holds_unkept_value(rel, version) || version->t_len > MAX_IMAGE_LEN)
		insert_entry(e, NULL, 0);
	else
		insert_entry(e, version, delta);
}

static bool
same_image(HeapTuple a, HeapTuple b)
{
	return a->t_len == b->t_len &&
		memcmp((char *) a->t_data + offsetof(HeapTupleHeaderData, t_infomask2),
			   (char *) b->t_data + offsetof(HeapTupleHeaderData, t_infomask2),
			   a->t_len - offsetof(HeapTupleHeaderData, t_infomask2)) == 0;
}

/* A key's entry: its image, unless it can't be kept (it may: see above). */
static void
insert_key(Entries *e, HeapTuple image)
{
	if (image->t_len > MAX_IMAGE_LEN)
		insert_entry(e, NULL, 0);
	else
		insert_entry(e, image, 0);
}

/*
 * The entries for a write: the version it replaced (old, if any) and those
 * it made.
 */
static void
note_versions(Relation rel, Entries *e, HeapTuple old, HeapTuple *made,
			  int nmade)
{
	if (old == NULL && nmade == 0)
		insert_entry(e, NULL, 0);	/* (every row may have changed) */
	else if (OidIsValid(RelationGetPrimaryKeyIndex(rel, false)))
	{
		Bitmapset  *keyattrs = RelationGetIndexAttrBitmap(rel,
														  INDEX_ATTR_BITMAP_PRIMARY_KEY);
		HeapTuple	oldkey = old ? key_image(rel, old, keyattrs) : NULL;

		for (int i = 0; i < nmade; i++)
		{
			HeapTuple	newkey = key_image(rel, made[i], keyattrs);

			/* (an update that kept its key: the key once) */
			if (oldkey && !same_image(oldkey, newkey))
				insert_key(e, oldkey);
			oldkey = NULL;
			insert_key(e, newkey);
		}
		if (oldkey)
			insert_key(e, oldkey);
	}
	else
	{
		if (old)
			insert_row(rel, e, old, -1);
		for (int i = 0; i < nmade; i++)
			insert_row(rel, e, made[i], 1);
	}
}

/*
 * What a note allocates, freed as it ends: a note is made for each tuple
 * version written, in the statement's memory (heap writes don't switch to a
 * per-tuple context), so a statement writing many rows would otherwise
 * hold copies of each until it ends.
 */
static MemoryContext note_cxt = NULL;

/*
 * Note a write: the version at oldtid it replaced, and those it made, in
 * hand (made) or at newtid.
 */
static void
note_write(Relation rel, const ItemPointerData *oldtid, HeapTuple oldtup,
		   const ItemPointerData *newtid, HeapTuple *made, int nmade)
{
	Oid			logid = log_relid(rel);
	HeapTuple	old = NULL;
	HeapTuple	new;
	Entries		e = {0};
	MemoryContext caller;
	RelationData logstub;

	if (!OidIsValid(logid))
		return;
	if (note_cxt == NULL)
		note_cxt = AllocSetContextCreate(TopMemoryContext, "change log note",
										 ALLOCSET_SMALL_SIZES);
	/* (left over by a note an error ended) */
	MemoryContextReset(note_cxt);
	/* (the log isn't opened: the entries are held, its OID their key) */
	memset(&logstub, 0, sizeof(logstub));
	logstub.rd_id = logid;
	e.log = &logstub;
	caller = MemoryContextSwitchTo(note_cxt);
	if (oldtup)
		old = oldtup;
	else if (oldtid)
		old = read_version(rel, oldtid);
	if (newtid)
	{
		new = read_version(rel, newtid);
		made = &new;
		nmade = 1;
	}
	note_versions(rel, &e, old, made, nmade);
	MemoryContextSwitchTo(caller);
	buffer_entries(&e);
	MemoryContextReset(note_cxt);
}

static void
relation_note(Relation rel, const ItemPointerData *oldtid, HeapTuple oldtup,
			  const ItemPointerData *newtid)
{
	note_write(rel, oldtid, oldtup, newtid, NULL, 0);
}

/* (the versions made are heap's: they aren't read again from their pages) */
static void
relation_note_made(Relation rel, const ItemPointerData *oldtid,
				   HeapTuple oldtup, HeapTuple *made, int nmade)
{
	note_write(rel, oldtid, oldtup, NULL, made, nmade);
}

/* The log's index on its transaction ids (made with it). */
static Oid
log_index_relid(Relation log)
{
	List	   *indexes = RelationGetIndexList(log);
	Oid			index = indexes ? linitial_oid(indexes) : InvalidOid;

	list_free(indexes);
	if (!OidIsValid(index))
		elog(ERROR, "change log \"%s\" has no index", RelationGetRelationName(log));
	return index;
}

/*
 * Visit a log row's entries: false if one says to read the table in full,
 * or a visit stopped.
 */
static bool
visit_row(Relation rel, HeapTuple tup, TupleDesc desc,
		  ProllyChangeLogVisit visit, void *arg)
{
	bool		isnull;
	Datum		d = heap_getattr(tup, Anum_log_entries, desc, &isnull);
	bytea	   *b = DatumGetByteaPP(d);
	const char *p = VARDATA_ANY(b);
	const char *end = p + VARSIZE_ANY_EXHDR(b);

	while (p + 2 * sizeof(uint32) <= end)
	{
		int32		delta;
		uint32		len;
		HeapTuple	image;
		bool		go;

		memcpy(&delta, p, sizeof(delta));
		memcpy(&len, p + sizeof(delta), sizeof(len));
		p += 2 * sizeof(uint32);
		if (len == FULL_LEN)
			return false;		/* read the table in full */
		if (len > (uint32) (end - p))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("corrupt change log row in \"%s\"",
							RelationGetRelationName(rel))));
		/* (the image's bytes, behind a header of its own, aligned) */
		image = palloc0(HEAPTUPLESIZE + IMAGE_SKIP + len);
		image->t_len = IMAGE_SKIP + len;
		image->t_data = (HeapTupleHeader) ((char *) image + HEAPTUPLESIZE);
		memcpy((char *) image->t_data + IMAGE_SKIP, p, len);
		image->t_tableOid = RelationGetRelid(rel);
		ItemPointerSetInvalid(&image->t_self);
		p += len;
		go = visit(image, delta, arg);
		pfree(image);
		if (!go)
			return false;
	}
	return true;
}

/*
 * Each row of the log by its index, transactions from 'from' on (to 'to'
 * if valid, else to the end; one alone if from == to), as the snapshot
 * sees them (SnapshotAny: every one); the visit returns false to stop.
 */
typedef bool (*RowVisit) (Relation log, HeapTuple tup, void *arg);

static bool
scan_rows(Relation log, Relation index, Snapshot snapshot,
		  FullTransactionId from, FullTransactionId to, bool bounded,
		  RowVisit visit, void *arg)
{
	ScanKeyData keys[2];
	int			nkeys = 0;
	IndexScanDesc scan;
	TupleTableSlot *slot = table_slot_create(log, NULL);
	bool		go = true;

	ScanKeyInit(&keys[nkeys++], 1, BTGreaterEqualStrategyNumber, F_INT8GE,
				Int64GetDatum((int64) U64FromFullTransactionId(from)));
	if (bounded)
		ScanKeyInit(&keys[nkeys++], 1, BTLessEqualStrategyNumber, F_INT8LE,
					Int64GetDatum((int64) U64FromFullTransactionId(to)));
	scan = index_beginscan(log, index, false, snapshot, NULL, nkeys, 0, 0);
	index_rescan(scan, keys, nkeys, NULL, 0);
	while (go && table_index_getnext_slot(scan, ForwardScanDirection, slot))
	{
		bool		shouldfree;
		HeapTuple	tup = ExecFetchSlotHeapTuple(slot, false, &shouldfree);

		go = visit(log, tup, arg);
		if (shouldfree)
			heap_freetuple(tup);
		CHECK_FOR_INTERRUPTS();
	}
	index_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	return go;
}

typedef struct ReadState
{
	Relation	rel;
	ProllyChangeLogVisit visit;
	void	   *arg;
	uint64		rows;
} ReadState;

static bool
read_row(Relation log, HeapTuple tup, void *arg)
{
	ReadState  *rs = arg;

	rs->rows++;
	return visit_row(rs->rel, tup, RelationGetDescr(log), rs->visit, rs->arg);
}

/*
 * The entries of rel's log the snapshot sees and the position since
 * doesn't hold (NULL: every one): of transactions from its xmax on, of
 * each it saw running, and of its own.  The transaction's own buffered
 * entries are written first, to be read with the rest.
 */
static bool
relation_read(Relation rel, Snapshot snapshot, const ProllyLogPosition *since,
			  ProllyChangeLogVisit visit, void *arg)
{
	Oid			logid = log_relid(rel);
	Relation	log;
	Relation	index;
	ReadState	rs = {rel, visit, arg, 0};
	bool		go;
	bool		pushed = false;

	if (!OidIsValid(logid))
		return false;
	if (flush_log(logid) && IsMVCCSnapshot(snapshot))
	{
		snapshot = push_with_own(snapshot);
		pushed = true;
	}
	log = table_open(logid, AccessShareLock);
	index = index_open(log_index_relid(log), AccessShareLock);
	if (since == NULL)
		go = scan_rows(log, index, snapshot, FullTransactionIdFromU64(0),
					   InvalidFullTransactionId, false, read_row, &rs);
	else
	{
		go = scan_rows(log, index, snapshot, since->xmax,
					   InvalidFullTransactionId, false, read_row, &rs);
		for (uint32 i = 0; go && i < since->nxip; i++)
			if (FullTransactionIdPrecedes(since->xip[i], since->xmax))
				go = scan_rows(log, index, snapshot, since->xip[i],
							   since->xip[i], true, read_row, &rs);
		if (go && FullTransactionIdIsValid(since->own) &&
			FullTransactionIdPrecedes(since->own, since->xmax))
			go = scan_rows(log, index, snapshot, since->own, since->own, true,
						   read_row, &rs);
	}
	pgstat_count_prolly(PGSTAT_PROLLY_LOG_PAGES_READ, rs.rows);
	index_close(index, AccessShareLock);
	table_close(log, AccessShareLock);
	if (pushed)
		PopActiveSnapshot();
	return go;
}

/* The log's upkeep: rows deleted (hushed: no write of the table's branch). */
static void
delete_rows(Relation log, ItemPointerData *tids, int n)
{
	CommandId	cid;

	if (n == 0)
		return;
	/* (the deletes as a command after the inserts) */
	CommandCounterIncrement();
	cid = GetCurrentCommandId(true);
	BRANCH_WRITES_HUSHED(
		for (int i = 0; i < n; i++)
		{
			TM_FailureData tmfd;
			TM_Result	result;

			/* (one another fold trimmed meanwhile is let be) */
			result = heap_delete(log, &tids[i], cid, 0, InvalidSnapshot,
								 true, &tmfd);
			if (result != TM_Ok && result != TM_Updated &&
				result != TM_Deleted && result != TM_SelfModified)
				elog(ERROR, "unexpected result %d consuming a row of \"%s\"",
					 (int) result, RelationGetRelationName(log));
			CHECK_FOR_INTERRUPTS();
		}
	);
}

typedef struct Trimmed
{
	Snapshot	snapshot;
	ItemPointerData *tids;
	int			n;
	int			max;
} Trimmed;

static bool
trim_row(Relation log, HeapTuple tup, void *arg)
{
	Trimmed    *t = arg;

	if (t->n == t->max)
	{
		t->max = Max(64, t->max * 2);
		t->tids = t->tids ? repalloc_array(t->tids, ItemPointerData, t->max) :
			palloc_array(ItemPointerData, t->max);
	}
	t->tids[t->n++] = tup->t_self;
	return true;
}

/*
 * Delete the transaction's own rows of the log, found by the index: those
 * it wrote and hasn't aborted (written just now too).
 */
static void
delete_own_rows(Relation log)
{
	FullTransactionId own = GetTopFullTransactionIdIfAny();
	Relation	index;
	Snapshot	snap;
	Trimmed    *t;

	if (!FullTransactionIdIsValid(own))
		return;
	t = palloc0(sizeof(*t));
	CommandCounterIncrement();
	snap = push_with_own(GetActiveSnapshot());
	index = index_open(log_index_relid(log), RowExclusiveLock);
	(void) scan_rows(log, index, snap, own, own, true, trim_row, t);
	index_close(index, NoLock);
	PopActiveSnapshot();
	delete_rows(log, t->tids, t->n);
}

/*
 * A fold that begins a baseline read rel's log by this snapshot: its own
 * transaction's rows are deleted (the next fold reads that transaction's
 * rows again, and a keyless row's would count twice), and the rows of
 * transactions every snapshot sees as finished and this one saw, folded
 * into the baseline, are trimmed.  Both in the transaction: if it aborts,
 * they come back with the old baseline.  The others' it read are behind the
 * new baseline's position, read no more.
 */
static void
relation_consume(Relation rel, Snapshot snapshot)
{
	Oid			logid = log_relid(rel);
	Relation	log;
	Relation	index;
	Trimmed		t = {snapshot, NULL, 0, 0};
	TransactionId horizon;
	FullTransactionId below;

	if (!OidIsValid(logid))
		return;
	/* (buffered entries the fold read: written, so as to be consumed) */
	(void) flush_log(logid);
	log = table_open(logid, RowExclusiveLock);
	delete_own_rows(log);
	if (own_logs)
		(void) hash_search(own_logs, &logid, HASH_REMOVE, NULL);

	/* (trimmed: transactions below the horizon, the snapshot saw finished) */
	horizon = GetOldestNonRemovableTransactionId(log);
	if (TransactionIdIsNormal(horizon))
	{
		below = FullTransactionIdFromAllowableAt(ReadNextFullTransactionId(),
												 horizon);
		if (U64FromFullTransactionId(below) > 0)
		{
			index = index_open(log_index_relid(log), RowExclusiveLock);
			(void) scan_rows(log, index, snapshot, FullTransactionIdFromU64(0),
							 FullTransactionIdFromU64(U64FromFullTransactionId(below) - 1),
							 true, trim_row, &t);
			index_close(index, NoLock);
			delete_rows(log, t.tids, t.n);
		}
	}
	table_close(log, NoLock);
}

/* Did a transaction the snapshot doesn't see, committed, write xid's part? */
static bool
xid_unseen(TransactionId xid, Snapshot snap)
{
	return TransactionIdIsNormal(xid) &&
		!TransactionIdIsCurrentTransactionId(xid) &&
		XidInMVCCSnapshot(xid, snap) && TransactionIdDidCommit(xid);
}

typedef struct Unseen
{
	Snapshot	snapshot;
	bool		found;
} Unseen;

static bool
unseen_row(Relation log, HeapTuple tup, void *arg)
{
	Unseen	   *u = arg;
	HeapTupleHeader t = tup->t_data;

	if (!HeapTupleHeaderXminFrozen(t) && !(t->t_infomask & HEAP_XMIN_INVALID) &&
		xid_unseen(HeapTupleHeaderGetRawXmin(t), u->snapshot))
		u->found = true;
	return !u->found;
}

/*
 * Did a transaction the snapshot doesn't see write an entry of rel's log?
 * Its rows are of transactions from the snapshot's xmin on, and kept while
 * the snapshot is (trimming keeps the rows of transactions any snapshot
 * may not see finished): read by the index, every one whatever the
 * snapshot sees of it.
 */
static bool
relation_written_unseen(Relation rel, Snapshot snapshot)
{
	Oid			logid = log_relid(rel);
	Relation	log;
	Relation	index;
	Unseen		u = {snapshot, false};
	FullTransactionId from;

	if (!OidIsValid(logid))
		return true;			/* (no log to tell: as if it did) */
	log = table_open(logid, AccessShareLock);
	index = index_open(log_index_relid(log), AccessShareLock);
	from = FullTransactionIdFromAllowableAt(ReadNextFullTransactionId(),
											snapshot->xmin);
	(void) scan_rows(log, index, SnapshotAny, from, InvalidFullTransactionId,
					 false, unseen_row, &u);
	index_close(index, AccessShareLock);
	table_close(log, AccessShareLock);
	return u.found;
}

/*
 * Drop the entries of rel's log the current transaction holds unwritten,
 * and delete the rows it wrote (and hasn't aborted): those of its own
 * changes, once a baseline holding them is to be the table's.  False,
 * consuming none, if not all can be told (one says to read the table in
 * full, or there were too many).
 */
typedef struct Others
{
	FullTransactionId own;
	bool		found;
} Others;

static bool
other_row(Relation log, HeapTuple tup, void *arg)
{
	Others	   *o = arg;
	bool		isnull;
	FullTransactionId xid = FullTransactionIdFromU64((uint64)
													 DatumGetInt64(heap_getattr(tup, Anum_log_xid,
																				RelationGetDescr(log),
																				&isnull)));

	if (!FullTransactionIdEquals(xid, o->own))
		o->found = true;
	return !o->found;
}

/*
 * Does the log hold an entry since the position, the active snapshot sees,
 * of a transaction but this one?
 */
static bool
others_since(Relation log, const ProllyLogPosition *since)
{
	Relation	index = index_open(log_index_relid(log), AccessShareLock);
	Snapshot	snap = GetActiveSnapshot();
	Others		o = {GetTopFullTransactionIdIfAny(), false};

	(void) scan_rows(log, index, snap, since->xmax, InvalidFullTransactionId,
					 false, other_row, &o);
	for (uint32 i = 0; !o.found && i < since->nxip; i++)
		if (FullTransactionIdPrecedes(since->xip[i], since->xmax))
			(void) scan_rows(log, index, snap, since->xip[i], since->xip[i],
							 true, other_row, &o);
	if (!o.found && FullTransactionIdIsValid(since->own) &&
		FullTransactionIdPrecedes(since->own, since->xmax))
		(void) scan_rows(log, index, snap, since->own, since->own, true,
						 other_row, &o);
	index_close(index, AccessShareLock);
	return o.found;
}

static bool
relation_consume_own(Relation rel, const ProllyLogPosition *since)
{
	Oid			logid = log_relid(rel);
	OwnLog	   *ol;
	LogBuffer  *b;
	List	   *mine = NIL;
	Relation	log;

	if (!OidIsValid(logid))
		return false;
	ol = own_logs ? hash_search(own_logs, &logid, HASH_FIND, NULL) : NULL;
	if (ol != NULL && ol->full)
		return false;
	if (buffers)
	{
		HASH_SEQ_STATUS seq;

		hash_seq_init(&seq, buffers);
		while ((b = hash_seq_search(&seq)) != NULL)
			if (b->tag.logid == logid)
				mine = lappend(mine, b);
	}
	foreach_ptr(LogBuffer, one, mine)
		if (one->full)
			return false;
	log = table_open(logid, RowExclusiveLock);
	if (others_since(log, since))
	{
		table_close(log, NoLock);
		return false;
	}
	/* (held, not written: the new baseline holds them) */
	foreach_ptr(LogBuffer, one, mine)
	{
		BufTag		tag = one->tag;

		MemoryContextDelete(one->cxt);
		(void) hash_search(buffers, &tag, HASH_REMOVE, NULL);
	}
	delete_own_rows(log);
	table_close(log, NoLock);
	return true;
}

static Oid
relation_relid(Oid relid)
{
	char		name[NAMEDATALEN];

	log_name(relid, name);
	return log_of_format(get_relname_relid(name, PG_TOAST_NAMESPACE));
}

static bool
relation_is_log(Relation rel)
{
	return rel->rd_rel->relkind == RELKIND_TOASTVALUE &&
		rel->rd_rel->relnamespace == PG_TOAST_NAMESPACE &&
		strncmp(NameStr(rel->rd_rel->relname), LOG_PREFIX,
				sizeof(LOG_PREFIX) - 1) == 0;
}

const ProllyChangeLogRoutine prolly_changelog_relation = {
	.info = {
		.iface = PROLLY_IFACE_CHANGE_LOG,
		.impl_id = 1,
		.format_version = FORMAT_VERSION,
		.name = "relation",
		.description = "entries in a relation of the table's own, written "
		"with each change",
	},
	.create = relation_create,
	.note = relation_note,
	.identity = relation_identity,
	.read = relation_read,
	.consume = relation_consume,
	.is_log = relation_is_log,
	.relid = relation_relid,
	.note_made = relation_note_made,
	.consume_own = relation_consume_own,
	.written_unseen = relation_written_unseen,
	.flush = flush_all,
};
