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
 * the table, on which it depends internally.  It has no index and no
 * TOAST table of its own.  Its rows are entries:
 *
 *	image	bytea	the entry's tuple, as heap stores one (NULL: read the
 *					table in full)
 *	delta	int4	0 for a key's entry, +1 or -1 for a row's
 *
 * An entry is inserted as each tuple version is noted, by the write's own
 * (sub)transaction: it is seen exactly when the write is.  A table with a
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
 * Reads and consumption walk the log's pages, judging each entry by the
 * snapshot as a heap scan would, but take no predicate locks: a fold
 * reading the log mustn't make every writer of the table conflict with it.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/changelog_relation.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/heapam.h"
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
#include "catalog/pg_am_d.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relcache.h"

#define FORMAT_VERSION		1

#define Anum_log_image		1
#define Anum_log_delta		2
#define Natts_log			2

/*
 * The longest image an entry holds: a log tuple (its header, the image's
 * length word and the delta, aligned) must fit a page.
 */
#define MAX_IMAGE_LEN \
	(MaxHeapTupleSize - MAXALIGN(SizeofHeapTupleHeader + VARHDRSZ + sizeof(int32)) - \
	 MAXIMUM_ALIGNOF)

#define LOG_PREFIX			"pg_prolly_log_"

static void
log_name(Oid relid, char *name)
{
	snprintf(name, NAMEDATALEN, LOG_PREFIX "%u", relid);
}

/*
 * The log of rel, a prolly table (or materialized view); InvalidOid if none.
 * A transient heap a rewrite fills (relrewrite set) has none: its table is
 * told of the rewrite as a whole.
 */
static Oid
log_relid(Relation rel)
{
	char		name[NAMEDATALEN];

	if (rel->rd_rel->relam != PROLLY_TABLE_AM_OID ||
		(rel->rd_rel->relkind != RELKIND_RELATION &&
		 rel->rd_rel->relkind != RELKIND_MATVIEW) ||
		OidIsValid(rel->rd_rel->relrewrite))
		return InvalidOid;
	log_name(RelationGetRelid(rel), name);
	return get_relname_relid(name, PG_TOAST_NAMESPACE);
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
	desc = CreateTemplateTupleDesc(Natts_log);
	TupleDescInitEntry(desc, Anum_log_image, "image", BYTEAOID, -1, 0);
	TupleDescInitEntry(desc, Anum_log_delta, "delta", INT4OID, -1, 0);

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

	/* (part of its table: dropped with it, never alone) */
	ObjectAddressSet(table, RelationRelationId, RelationGetRelid(rel));
	ObjectAddressSet(log, RelationRelationId, logid);
	recordDependencyOn(&log, &table, DEPENDENCY_INTERNAL);
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

/* A write's entries, inserted together (one WAL record for a page's). */
typedef struct Entries
{
	Relation	log;
	HeapTuple  *tups;
	int			n;
	int			max;
} Entries;

/* An entry: image (NULL: read the table in full) and delta. */
static void
insert_entry(Entries *e, HeapTuple image, int32 delta)
{
	Datum		values[Natts_log];
	bool		isnull[Natts_log] = {0};

	if (image != NULL)
	{
		bytea	   *b = palloc(VARHDRSZ + image->t_len);

		SET_VARSIZE(b, VARHDRSZ + image->t_len);
		memcpy(VARDATA(b), image->t_data, image->t_len);
		values[Anum_log_image - 1] = PointerGetDatum(b);
	}
	else
		isnull[Anum_log_image - 1] = true;
	values[Anum_log_delta - 1] = Int32GetDatum(delta);
	if (e->n == e->max)
	{
		e->max = Max(8, e->max * 2);
		e->tups = e->tups ? repalloc_array(e->tups, HeapTuple, e->max) :
			palloc_array(HeapTuple, e->max);
	}
	e->tups[e->n++] = heap_form_tuple(RelationGetDescr(e->log), values, isnull);
}

static void
write_entries(Entries *e)
{
	TupleTableSlot **slots;

	if (e->n == 0)
		return;
	if (e->n == 1)
	{
		simple_heap_insert(e->log, e->tups[0]);
		return;
	}
	slots = palloc_array(TupleTableSlot *, e->n);
	for (int i = 0; i < e->n; i++)
	{
		slots[i] = MakeSingleTupleTableSlot(RelationGetDescr(e->log),
											&TTSOpsHeapTuple);
		ExecStoreHeapTuple(e->tups[i], slots[i], false);
	}
	heap_multi_insert(e->log, slots, e->n, GetCurrentCommandId(true), 0, NULL);
	for (int i = 0; i < e->n; i++)
		ExecDropSingleTupleTableSlot(slots[i]);
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
note_write(Relation rel, const ItemPointerData *oldtid,
		   const ItemPointerData *newtid, HeapTuple *made, int nmade)
{
	Oid			logid = log_relid(rel);
	HeapTuple	old = NULL;
	HeapTuple	new;
	Entries		e = {0};
	MemoryContext caller;

	if (!OidIsValid(logid))
		return;
	if (note_cxt == NULL)
		note_cxt = AllocSetContextCreate(TopMemoryContext, "change log note",
										 ALLOCSET_SMALL_SIZES);
	/* (left over by a note an error ended) */
	MemoryContextReset(note_cxt);
	e.log = table_open(logid, RowExclusiveLock);
	caller = MemoryContextSwitchTo(note_cxt);
	if (oldtid)
		old = read_version(rel, oldtid);
	if (newtid)
	{
		new = read_version(rel, newtid);
		made = &new;
		nmade = 1;
	}
	note_versions(rel, &e, old, made, nmade);
	write_entries(&e);
	MemoryContextSwitchTo(caller);
	MemoryContextReset(note_cxt);
	table_close(e.log, NoLock);
}

static void
relation_note(Relation rel, const ItemPointerData *oldtid,
			  const ItemPointerData *newtid)
{
	note_write(rel, oldtid, newtid, NULL, 0);
}

/* (the versions made are heap's: they aren't read again from their pages) */
static void
relation_note_made(Relation rel, const ItemPointerData *oldtid,
				   HeapTuple *made, int nmade)
{
	note_write(rel, oldtid, NULL, made, nmade);
}

/*
 * Visit each entry of the log the snapshot sees, by its TID, under a share
 * lock of its page (the visit mustn't take buffer locks); false if a visit
 * stopped.
 */
typedef bool (*EntryVisit) (Relation log, HeapTuple tup, void *arg);

static bool
walk_log(Relation log, Snapshot snapshot, EntryVisit visit, void *arg)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(log);

	for (BlockNumber blk = 0; blk < nblocks; blk++)
	{
		Buffer		buf = ReadBuffer(log, blk);
		Page		page;
		OffsetNumber maxoff;
		bool		go = true;

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		maxoff = PageIsNew(page) ? InvalidOffsetNumber : PageGetMaxOffsetNumber(page);
		for (OffsetNumber off = FirstOffsetNumber; go && off <= maxoff; off++)
		{
			ItemId		lp = PageGetItemId(page, off);
			HeapTupleData tup;

			if (!ItemIdIsNormal(lp))
				continue;
			tup.t_data = (HeapTupleHeader) PageGetItem(page, lp);
			tup.t_len = ItemIdGetLength(lp);
			tup.t_tableOid = RelationGetRelid(log);
			ItemPointerSet(&tup.t_self, blk, off);
			if (HeapTupleSatisfiesVisibility(&tup, snapshot, buf))
				go = visit(log, &tup, arg);
		}
		UnlockReleaseBuffer(buf);
		if (!go)
			return false;
		CHECK_FOR_INTERRUPTS();
	}
	return true;
}

typedef struct Collected
{
	HeapTuple  *entries;
	int			n;
	int			max;
	ItemPointerData *tids;		/* consume: just their TIDs */
} Collected;

/* Copy each entry, to visit once the page is let go of. */
static bool
collect_entry(Relation log, HeapTuple tup, void *arg)
{
	Collected  *c = arg;

	if (c->n == c->max)
	{
		c->max = Max(64, c->max * 2);
		if (c->tids)
			c->tids = repalloc_array(c->tids, ItemPointerData, c->max);
		else if (c->entries)
			c->entries = repalloc_array(c->entries, HeapTuple, c->max);
		else
			c->entries = palloc_array(HeapTuple, c->max);
	}
	if (c->tids)
		c->tids[c->n++] = tup->t_self;
	else
		c->entries[c->n++] = heap_copytuple(tup);
	return true;
}

static bool
relation_read(Relation rel, Snapshot snapshot, ProllyChangeLogVisit visit,
			  void *arg)
{
	Oid			logid = log_relid(rel);
	Relation	log;
	Collected	c = {0};
	bool		go;
	MemoryContext cxt;
	MemoryContext old;

	if (!OidIsValid(logid))
		return false;
	cxt = AllocSetContextCreate(CurrentMemoryContext, "change log page",
								ALLOCSET_DEFAULT_SIZES);
	log = table_open(logid, AccessShareLock);

	/* a page at a time: its entries copied, then visited */
	go = true;
	for (BlockNumber blk = 0, nblocks = RelationGetNumberOfBlocks(log);
		 go && blk < nblocks; blk++)
	{
		Buffer		buf = ReadBuffer(log, blk);
		Page		page;
		OffsetNumber maxoff;

		MemoryContextReset(cxt);
		old = MemoryContextSwitchTo(cxt);
		c.entries = NULL;
		c.n = c.max = 0;
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		maxoff = PageIsNew(page) ? InvalidOffsetNumber : PageGetMaxOffsetNumber(page);
		for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
		{
			ItemId		lp = PageGetItemId(page, off);
			HeapTupleData tup;

			if (!ItemIdIsNormal(lp))
				continue;
			tup.t_data = (HeapTupleHeader) PageGetItem(page, lp);
			tup.t_len = ItemIdGetLength(lp);
			tup.t_tableOid = logid;
			ItemPointerSet(&tup.t_self, blk, off);
			if (HeapTupleSatisfiesVisibility(&tup, snapshot, buf))
				collect_entry(log, &tup, &c);
		}
		UnlockReleaseBuffer(buf);
		MemoryContextSwitchTo(old);

		for (int i = 0; go && i < c.n; i++)
		{
			bool		isnull;
			Datum		image;
			int32		delta;
			HeapTupleData entry;

			delta = DatumGetInt32(heap_getattr(c.entries[i], Anum_log_delta,
											   RelationGetDescr(log), &isnull));
			image = heap_getattr(c.entries[i], Anum_log_image,
								 RelationGetDescr(log), &isnull);
			if (isnull)
			{
				go = false;		/* read the table in full */
				break;
			}
			entry.t_len = VARSIZE_ANY_EXHDR(DatumGetPointer(image));
			entry.t_data = (HeapTupleHeader) VARDATA_ANY(DatumGetPointer(image));
			entry.t_tableOid = RelationGetRelid(rel);
			ItemPointerSetInvalid(&entry.t_self);

			/*
			 * (the image's bytes, as heap stored them: aligned as a tuple
			 * read from a page is, in a copy of their own)
			 */
			{
				HeapTuple	aligned = heap_copytuple(&entry);

				go = visit(aligned, delta, arg);
				heap_freetuple(aligned);
			}
		}
		CHECK_FOR_INTERRUPTS();
	}
	table_close(log, AccessShareLock);
	MemoryContextDelete(cxt);
	return go;
}

static void
relation_consume(Relation rel, Snapshot snapshot)
{
	Oid			logid = log_relid(rel);
	Relation	log;
	Collected	c = {0};
	CommandId	cid = GetCurrentCommandId(true);

	if (!OidIsValid(logid))
		return;
	log = table_open(logid, RowExclusiveLock);
	c.max = 64;
	c.tids = palloc_array(ItemPointerData, c.max);
	(void) walk_log(log, snapshot, collect_entry, &c);

	/* (the log's upkeep, not a write of the table's branch) */
	BRANCH_WRITES_HUSHED(
		for (int i = 0; i < c.n; i++)
		{
			TM_FailureData tmfd;
			TM_Result	result;

			/* (one consumed meanwhile by another fold is let be) */
			result = heap_delete(log, &c.tids[i], cid, 0, InvalidSnapshot,
								 true, &tmfd);
			if (result != TM_Ok && result != TM_Updated &&
				result != TM_Deleted && result != TM_SelfModified)
				elog(ERROR, "unexpected result %d consuming an entry of \"%s\"",
					 (int) result, RelationGetRelationName(log));
			CHECK_FOR_INTERRUPTS();
		}
	);
	pfree(c.tids);
	table_close(log, NoLock);
}

/*
 * New storage for the log, as TRUNCATE gives it (its entries are of rows
 * the table no longer has: no other transaction sees its branch), and an
 * entry to read the table in full.
 */
static void
relation_reset(Relation rel)
{
	Oid			logid = log_relid(rel);
	Entries		e = {0};

	if (!OidIsValid(logid))
		return;
	/*
	 * (a writer's lock, as the table's copy is given new storage under: the
	 * log's OID is every branch's, and only the branch being made, which no
	 * other transaction sees, sees this storage)
	 */
	e.log = table_open(logid, RowExclusiveLock);
	RelationSetNewRelfilenumber(e.log, e.log->rd_rel->relpersistence);
	CommandCounterIncrement();
	insert_entry(&e, NULL, 0);
	write_entries(&e);
	table_close(e.log, NoLock);
}

static Oid
relation_relid(Oid relid)
{
	char		name[NAMEDATALEN];

	log_name(relid, name);
	return get_relname_relid(name, PG_TOAST_NAMESPACE);
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
	.reset = relation_reset,
	.relid = relation_relid,
	.note_made = relation_note_made,
};
