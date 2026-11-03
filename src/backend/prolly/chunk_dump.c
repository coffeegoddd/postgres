/*-------------------------------------------------------------------------
 *
 * chunk_dump.c
 *	  The history store taken out, and put back, by pg_dump and pg_restore.
 *
 * pg_history_export() is the store as of the transaction's snapshot: every
 * chunk it had then, each after the chunks it references (as its index's
 * format finds them); then every chunk of its tier's bundles, read from the tier,
 * each after those it references (one the store has too given again: it was
 * in both); then every pointer; then every bundle.  As rows (hash, data,
 * name, roots, tier): a chunk's with no name, tier true if the tier's; a
 * pointer's with no data; a bundle's with no hash (its name and roots).  A
 * chunk freed since the snapshot is the store's still to it (its entry and
 * bytes are taken out once no snapshot might find it), so the chunks, the
 * pointers and the bundles are one moment's.  pg_history.chunk_restore is
 * a view of no rows whose INSERT puts a chunk (its bytes checked against
 * its name) in the store, or in the tier the target has (a tier's), inserts
 * a pointer (which takes its root, by the pointers' trigger), or pushes a
 * bundle (pg_history_push: its chunks the tier's already, none copied, its
 * manifest and row made, its roots held there): pg_restore COPYs the rows
 * into it in one statement, one transaction, so no chunk restored is freed
 * before the pointers or the bundles holding it are.  So a restore puts in
 * the store what the source's store had, and no more.
 * pg_history_has_chunks() says whether the store has a chunk, or a bundle
 * is in its tier, for pg_dump.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/chunk_dump.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "prolly/chunk_format.h"
#include "prolly/chunk_index.h"
#include "prolly/chunk_log.h"
#include "prolly/chunk_store.h"
#include "prolly/chunk_tier.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplestore.h"
#include "varatt.h"

typedef struct HashList
{
	uint8	   *hashes;
	int			n;
	int			cap;
} HashList;

static void
hash_list_add(HashList *l, const uint8 *hash)
{
	if (l->n == l->cap)
	{
		l->cap = Max(16, l->cap * 2);
		l->hashes = l->hashes ?
			repalloc_huge(l->hashes, (Size) l->cap * CHUNK_HASH_LEN) :
			MemoryContextAllocHuge(CurrentMemoryContext, (Size) l->cap * CHUNK_HASH_LEN);
	}
	memcpy(l->hashes + (Size) l->n * CHUNK_HASH_LEN, hash, CHUNK_HASH_LEN);
	l->n++;
}

static bool
collect_chunk(void *arg, const ChunkIndexEntry *e)
{
	hash_list_add((HashList *) arg, e->hash);
	return true;
}

static void
collect_ref(void *arg, const uint8 *hash)
{
	hash_list_add((HashList *) arg, hash);
}

/* a chunk on the way down: its bytes, and the references not yet out */
typedef struct DumpFrame
{
	uint8		hash[CHUNK_HASH_LEN];
	void	   *data;
	uint32		len;
	HashList	refs;
	int			next;
} DumpFrame;

static bytea *
to_bytea(const void *data, uint32 len)
{
	bytea	   *b = palloc_extended(VARHDRSZ + len, MCXT_ALLOC_HUGE);

	SET_VARSIZE(b, VARHDRSZ + len);
	memcpy(VARDATA(b), data, len);
	return b;
}

static void
emit_chunk(ReturnSetInfo *rsinfo, const DumpFrame *f, bool tier)
{
	Datum		values[5];
	bool		nulls[5] = {false, false, true, true, false};
	bytea	   *h = to_bytea(f->hash, CHUNK_HASH_LEN);
	bytea	   *d = to_bytea(f->data, f->len);

	values[0] = PointerGetDatum(h);
	values[1] = PointerGetDatum(d);
	values[2] = (Datum) 0;
	values[3] = (Datum) 0;
	values[4] = BoolGetDatum(tier);
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	pfree(h);
	pfree(d);
}

/* a chunk's bytes as of the snapshot; an error if the store had none */
static void *
read_as_of(Relation log, Relation idx, Snapshot snap, const uint8 *hash,
		   uint32 *len)
{
	ChunkIndexEntry e;
	void	   *data;

	if (!ChunkIndex->lookup_as_of(idx, hash, snap, &e))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("the history store has no chunk %s, which another references",
						ChunkHashText(hash))));
	data = palloc_extended(Max(e.len, 1), MCXT_ALLOC_HUGE);
	ChunkLogOf(log)->read(log, e.loc, data, e.len);
	*len = e.len;
	return data;
}

static bool note_chunk(void *arg, const ChunkIndexEntry *e);

/*
 * The chunks root reaches the store of log and idx had as of snap, not out
 * yet (seen), each after those it references (tier: rows of the tier's)
 */
static void
export_from(ReturnSetInfo *rsinfo, Relation log, Relation idx, Snapshot snap,
			HTAB *seen, const uint8 *root, bool tier)
{
	DumpFrame  *stack;
	int			cap = 16;
	int			depth;
	bool		found;

	(void) hash_search(seen, root, HASH_ENTER, &found);
	if (found)
		return;
	stack = palloc(cap * sizeof(DumpFrame));

	/* (a chunk pushed is seen: one named again is out, or on the way) */
	memcpy(stack[0].hash, root, CHUNK_HASH_LEN);
	stack[0].data = NULL;
	depth = 1;
	while (depth > 0)
	{
		DumpFrame  *f = &stack[depth - 1];

		CHECK_FOR_INTERRUPTS();
		if (f->data == NULL)
		{
			f->data = read_as_of(log, idx, snap, f->hash, &f->len);
			memset(&f->refs, 0, sizeof(f->refs));
			ChunkIndexFormat(idx)->refs(f->data, f->len, collect_ref, &f->refs);
			f->next = 0;
		}
		if (f->next < f->refs.n)
		{
			const uint8 *kid = f->refs.hashes + (Size) f->next++ * CHUNK_HASH_LEN;

			(void) hash_search(seen, kid, HASH_ENTER, &found);
			if (found)
				continue;
			if (depth == cap)
			{
				cap *= 2;
				stack = repalloc(stack, cap * sizeof(DumpFrame));
				f = &stack[depth - 1];
			}
			memcpy(stack[depth].hash, kid, CHUNK_HASH_LEN);
			stack[depth].data = NULL;
			depth++;
			continue;
		}
		emit_chunk(rsinfo, f, tier);
		pfree(f->data);
		if (f->refs.hashes)
			pfree(f->refs.hashes);
		depth--;
	}
	pfree(stack);
}

/*
 * pg_history_export(): the store as of the snapshot, its chunks each after
 * those it references; then its tier's bundles' chunks so; then its
 * pointers; then its bundles.  Each chunk is visited once a walk (a set of
 * those out, or on the way: the store's, then the tier's), by a walk down
 * from each in turn, a chunk out once the chunks it names are.
 * O(store and tier), in memory a set of the chunks' names (the rows
 * themselves go to the function's tuplestore, which spills to disk).
 */
Datum
pg_history_export(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Snapshot	snap = GetActiveSnapshot();
	MemoryContext outer = CurrentMemoryContext;
	Relation	log;
	Relation	idx;
	HashList	all = {0};
	HashList	bundle_roots = {0};
	Datum	   *bundle_names = NULL;
	Datum	   *bundle_arrays = NULL;
	int			nbundles = 0;
	HASHCTL		ctl;
	HTAB	   *seen;

	InitMaterializedSRF(fcinfo, 0);

	/* the bundles, as of the snapshot, kept here (SPI_finish lets go of SPI's) */
	SPI_connect();
	if (SPI_execute("SELECT name, roots FROM pg_history.bundles ORDER BY name", true, 0) != SPI_OK_SELECT)
		elog(ERROR, "could not read the history store's bundles");
	{
		MemoryContext spi_cxt = MemoryContextSwitchTo(outer);

		nbundles = (int) SPI_processed;
		bundle_names = palloc_array(Datum, Max(nbundles, 1));
		bundle_arrays = palloc_array(Datum, Max(nbundles, 1));
		for (int r = 0; r < nbundles; r++)
		{
			bool		isnull;
			Datum	   *elems;
			bool	   *nulls;
			int			n;

			bundle_names[r] = datumCopy(SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 1, &isnull),
										false, -1);
			bundle_arrays[r] = datumCopy(SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 2, &isnull),
										 false, -1);
			deconstruct_array(DatumGetArrayTypeP(bundle_arrays[r]), BYTEAOID, -1, false, TYPALIGN_INT,
							  &elems, &nulls, &n);
			for (int k = 0; k < n; k++)
				hash_list_add(&bundle_roots, (const uint8 *) VARDATA_ANY(DatumGetByteaPP(elems[k])));
		}
		MemoryContextSwitchTo(spi_cxt);
	}
	SPI_finish();

	log = relation_open(ChunkStoreLogRelid(), AccessShareLock);
	idx = relation_open(ChunkStoreIndexRelid(), AccessShareLock);
	ChunkIndex->visit_as_of(idx, snap, collect_chunk, &all);

	ctl.keysize = CHUNK_HASH_LEN;
	ctl.entrysize = CHUNK_HASH_LEN;
	ctl.hcxt = CurrentMemoryContext;
	seen = hash_create("history export", Max(all.n, 16), &ctl,
					   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	for (int i = 0; i < all.n; i++)
		export_from(rsinfo, log, idx, snap, seen, all.hashes + (Size) i * CHUNK_HASH_LEN, false);
	relation_close(idx, AccessShareLock);
	relation_close(log, AccessShareLock);

	/*
	 * the bundles' chunks, read from the tier under the same snapshot, a walk
	 * of their own: each the tier's, put there on restore, so that a bundle's
	 * push copies none (one the store has too is given again)
	 */
	if (bundle_roots.n > 0)
	{
		hash_destroy(seen);
		seen = hash_create("history export", 1024, &ctl,
						   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		log = relation_open(ChunkStoreTierLogRelid(), AccessShareLock);
		idx = relation_open(ChunkStoreTierIndexRelid(), AccessShareLock);
		for (int i = 0; i < bundle_roots.n; i++)
			export_from(rsinfo, log, idx, snap, seen,
						bundle_roots.hashes + (Size) i * CHUNK_HASH_LEN, true);
		relation_close(idx, AccessShareLock);
		relation_close(log, AccessShareLock);
	}

	/* the pointers, read under the same snapshot */
	SPI_connect();
	if (SPI_execute("SELECT name, hash FROM pg_history.pointers ORDER BY name", true, 0) != SPI_OK_SELECT)
		elog(ERROR, "could not read the history store's pointers");
	for (uint64 r = 0; r < SPI_processed; r++)
	{
		Datum		values[5];
		bool		nulls[5] = {false, true, false, true, false};
		bool		isnull;

		values[2] = SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 1, &isnull);
		values[0] = SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 2, &isnull);
		values[1] = (Datum) 0;
		values[3] = (Datum) 0;
		values[4] = BoolGetDatum(false);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	SPI_finish();

	/* and the bundles */
	for (int r = 0; r < nbundles; r++)
	{
		Datum		values[5] = {0, 0, bundle_names[r], bundle_arrays[r], BoolGetDatum(false)};
		bool		nulls[5] = {true, true, false, false, false};

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

static bool
note_chunk(void *arg, const ChunkIndexEntry *e)
{
	*(bool *) arg = true;
	return false;
}

/*
 * pg_history_has_chunks(): whether the store has a chunk as of the snapshot
 * (a pointer names one), the index read only as far as the first, or a
 * bundle is in its tier
 */
Datum
pg_history_has_chunks(PG_FUNCTION_ARGS)
{
	Relation	idx = relation_open(ChunkStoreIndexRelid(), AccessShareLock);
	bool		any = false;

	ChunkIndex->visit_as_of(idx, GetActiveSnapshot(), note_chunk, &any);
	relation_close(idx, AccessShareLock);
	if (!any)
	{
		SPI_connect();
		if (SPI_execute("SELECT 1 FROM pg_history.bundles LIMIT 1", true, 0) != SPI_OK_SELECT)
			elog(ERROR, "could not read the history store's bundles");
		any = SPI_processed > 0;
		SPI_finish();
	}
	PG_RETURN_BOOL(any);
}

/* a bundle's restore, named in its errors */
static void
bundle_context(void *arg)
{
	errcontext("restoring bundle \"%s\" of the history store", (const char *) arg);
}

/*
 * The INSTEAD OF INSERT trigger of pg_history.chunk_restore: a row of
 * (hash, data, name, roots, tier) puts its chunk, its bytes checked against
 * its name, in the store, or in its tier (tier); with a name and no bytes,
 * inserts its pointer; with a name and roots, pushes its bundle
 */
Datum
pg_history_chunk_restore(PG_FUNCTION_ARGS)
{
	TriggerData *td = (TriggerData *) fcinfo->context;
	HeapTuple	tuple;
	TupleDesc	desc;
	Datum		hash_d,
				data_d,
				name_d,
				roots_d;
	bool		hash_null,
				data_null,
				name_null,
				roots_null,
				tier_null;
	bool		tier;
	bytea	   *hash;

	if (!CALLED_AS_TRIGGER(fcinfo) || !TRIGGER_FIRED_INSTEAD(td->tg_event) ||
		!TRIGGER_FIRED_BY_INSERT(td->tg_event) || !TRIGGER_FIRED_FOR_ROW(td->tg_event) ||
		RelationGetNamespace(td->tg_relation) != PG_HISTORY_NAMESPACE)
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("pg_history_chunk_restore() is pg_history.chunk_restore's trigger alone")));
	tuple = td->tg_trigtuple;
	desc = RelationGetDescr(td->tg_relation);
	hash_d = heap_getattr(tuple, 1, desc, &hash_null);
	data_d = heap_getattr(tuple, 2, desc, &data_null);
	name_d = heap_getattr(tuple, 3, desc, &name_null);
	roots_d = heap_getattr(tuple, 4, desc, &roots_null);
	tier = DatumGetBool(heap_getattr(tuple, 5, desc, &tier_null));
	tier = !tier_null && tier;

	/* (a bundle's: pushed, its chunks put before it, to the tier the target has) */
	if (!roots_null)
	{
		Oid			types[2] = {TEXTOID, BYTEAARRAYOID};
		Datum		args[2] = {name_d, roots_d};
		ErrorContextCallback cb;

		if (!hash_null || !data_null || name_null || tier)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("a bundle's row restored has its name and roots alone")));
		cb.callback = bundle_context;
		cb.arg = TextDatumGetCString(name_d);
		cb.previous = error_context_stack;
		error_context_stack = &cb;
		SPI_connect();
		if (SPI_execute_with_args("SELECT pg_catalog.pg_history_push($1, $2)",
								  2, types, args, NULL, false, 0) != SPI_OK_SELECT)
			elog(ERROR, "could not restore a history store's bundle");
		SPI_finish();
		error_context_stack = cb.previous;
		return PointerGetDatum(tuple);
	}
	if (hash_null || VARSIZE_ANY_EXHDR(DatumGetByteaPP(hash_d)) != CHUNK_HASH_LEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a chunk's hash is %d bytes", CHUNK_HASH_LEN)));
	hash = DatumGetByteaPP(hash_d);
	if (data_null == name_null)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a row restored is a chunk's (its bytes) or a pointer's (its name), one of them")));
	if (tier && data_null)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a row restored of the tier's is a chunk's")));
	if (tier && (history_tier_location == NULL || history_tier_location[0] == '\0'))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("the history restored has chunks pushed to a tier, and this server's history store has no tier"),
				 errhint("Set history_tier_location, or restore without the history (pg_dump --no-history, or pg_restore --use-list without its HISTORY entry).")));
	if (!data_null)
	{
		bytea	   *data = DatumGetByteaPP(data_d);
		Relation	log = relation_open(tier ? ChunkStoreTierLogRelid() : ChunkStoreLogRelid(),
										RowExclusiveLock);
		Relation	idx = relation_open(tier ? ChunkStoreTierIndexRelid() : ChunkStoreIndexRelid(),
										RowExclusiveLock);

		(void) ChunkStorePut(log, idx, (const uint8 *) VARDATA_ANY(hash),
							 VARDATA_ANY(data), VARSIZE_ANY_EXHDR(data));
		relation_close(idx, NoLock);
		relation_close(log, NoLock);
	}
	else
	{
		Oid			types[2] = {TEXTOID, BYTEAOID};
		Datum		args[2] = {name_d, PointerGetDatum(hash)};

		SPI_connect();
		if (SPI_execute_with_args("INSERT INTO pg_history.pointers (name, hash) VALUES ($1, $2)",
								  2, types, args, NULL, false, 0) != SPI_OK_INSERT)
			elog(ERROR, "could not restore a history store's pointer");
		SPI_finish();
	}
	return PointerGetDatum(tuple);
}

/*
 * pg_chunk_index_format(idx regclass) returns text: the format of the
 * chunks a chunk index names, as it records it (for pg_dump, which makes
 * the index again with it)
 */
Datum
pg_chunk_index_format(PG_FUNCTION_ARGS)
{
	Relation	idx = relation_open(PG_GETARG_OID(0), AccessShareLock);
	char	   *name;

	if (!RelationIsChunkIndex(idx))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a chunk index", RelationGetRelationName(idx))));
	/* (the name recorded: no module's format need be loaded to dump it) */
	name = pstrdup(ChunkIndexFormatName(idx));
	relation_close(idx, AccessShareLock);
	PG_RETURN_TEXT_P(cstring_to_text(name));
}
