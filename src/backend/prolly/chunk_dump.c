/*-------------------------------------------------------------------------
 *
 * chunk_dump.c
 *	  The history store taken out, and put back, by pg_dump and pg_restore.
 *
 * pg_history_export() is the store as of the transaction's snapshot: every
 * chunk it had then, each after the chunks it references (as ChunkFormat
 * finds them); then every chunk of its tier's bundles (the tier's, each
 * after those it references, one the store has too out once); then every
 * pointer; then every bundle: as rows (hash, data, name, roots), a chunk's
 * with no name, a pointer's with no data, a bundle's with no hash (its
 * name and roots).  A chunk freed since the snapshot is the store's still
 * to it (its entry and bytes are taken out once no snapshot might find
 * it), so the chunks, the pointers and the bundles are one moment's.
 * pg_history.chunk_restore is a view of no rows whose INSERT puts a chunk
 * (its bytes checked against its name) in the store, inserts a pointer
 * (which takes its root, by the pointers' trigger), or pushes a bundle
 * (pg_history_push: its chunks, put in the store before it, copied to the
 * tier the target has): pg_restore COPYs the rows into it in one
 * statement, one transaction, so no chunk restored is freed before the
 * pointers holding it are, or the push taking it.  pg_history_has_chunks()
 * says whether the store, or its tier, has a chunk, for pg_dump.
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
#include "utils/array.h"
#include "utils/builtins.h"
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
emit_chunk(ReturnSetInfo *rsinfo, const DumpFrame *f)
{
	Datum		values[4];
	bool		nulls[4] = {false, false, true, true};
	bytea	   *h = to_bytea(f->hash, CHUNK_HASH_LEN);
	bytea	   *d = to_bytea(f->data, f->len);

	values[0] = PointerGetDatum(h);
	values[1] = PointerGetDatum(d);
	values[2] = (Datum) 0;
	values[3] = (Datum) 0;
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

/* whether the tier's index has a chunk, as of the active snapshot */
static bool
tier_has_chunks(void)
{
	Relation	idx = relation_open(ChunkStoreTierIndexRelid(), AccessShareLock);
	bool		any = false;

	ChunkIndex->visit_as_of(idx, GetActiveSnapshot(), note_chunk, &any);
	relation_close(idx, AccessShareLock);
	return any;
}

/*
 * pg_history_export(): the store as of the snapshot, its chunks each after
 * those it references, then its pointers.  Each chunk is visited once (a set
 * of those out, or on the way), by a walk down from each in turn, a chunk
 * out once the chunks it names are.  O(store), in memory a set of the
 * chunks' names (the rows themselves go to the function's tuplestore,
 * which spills to disk).
 */
/*
 * The chunks root reaches the store of log and idx had as of snap, not out
 * yet (seen), each after those it references
 */
static void
export_from(ReturnSetInfo *rsinfo, Relation log, Relation idx, Snapshot snap,
			HTAB *seen, const uint8 *root)
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
			ChunkFormat->refs(f->data, f->len, collect_ref, &f->refs);
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
		emit_chunk(rsinfo, f);
		pfree(f->data);
		if (f->refs.hashes)
			pfree(f->refs.hashes);
		depth--;
	}
	pfree(stack);
}

Datum
pg_history_export(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Snapshot	snap = GetActiveSnapshot();
	Relation	log;
	Relation	idx;
	HashList	all = {0};
	HashList	bundle_roots = {0};
	HASHCTL		ctl;
	HTAB	   *seen;
	MemoryContext outer;

	InitMaterializedSRF(fcinfo, 0);
	log = relation_open(ChunkStoreLogRelid(), AccessShareLock);
	idx = relation_open(ChunkStoreIndexRelid(), AccessShareLock);
	ChunkIndex->visit_as_of(idx, snap, collect_chunk, &all);

	ctl.keysize = CHUNK_HASH_LEN;
	ctl.entrysize = CHUNK_HASH_LEN;
	ctl.hcxt = CurrentMemoryContext;
	seen = hash_create("history export", Max(all.n, 16), &ctl,
					   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	for (int i = 0; i < all.n; i++)
		export_from(rsinfo, log, idx, snap, seen, all.hashes + (Size) i * CHUNK_HASH_LEN);
	relation_close(idx, AccessShareLock);
	relation_close(log, AccessShareLock);

	/*
	 * the tier's bundles' chunks, read from the tier under the same snapshot
	 * (each once: one the store has out already)
	 */
	outer = CurrentMemoryContext;
	SPI_connect();
	if (SPI_execute("SELECT name, roots FROM pg_history.bundles ORDER BY name", true, 0) != SPI_OK_SELECT)
		elog(ERROR, "could not read the history store's bundles");
	for (uint64 r = 0; r < SPI_processed; r++)
	{
		MemoryContext spi_cxt;
		bool		isnull;
		Datum		d = SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 2, &isnull);
		Datum	   *elems;
		bool	   *nulls;
		int			n;

		deconstruct_array(DatumGetArrayTypeP(d), BYTEAOID, -1, false, TYPALIGN_INT,
						  &elems, &nulls, &n);
		/* (in the function's memory: SPI_finish lets go of SPI's) */
		spi_cxt = MemoryContextSwitchTo(outer);
		for (int i = 0; i < n; i++)
			hash_list_add(&bundle_roots, (const uint8 *) VARDATA_ANY(DatumGetByteaPP(elems[i])));
		MemoryContextSwitchTo(spi_cxt);
	}
	SPI_finish();
	if (bundle_roots.n > 0)
	{
		log = relation_open(ChunkStoreTierLogRelid(), AccessShareLock);
		idx = relation_open(ChunkStoreTierIndexRelid(), AccessShareLock);
		for (int i = 0; i < bundle_roots.n; i++)
			export_from(rsinfo, log, idx, snap, seen,
						bundle_roots.hashes + (Size) i * CHUNK_HASH_LEN);
		relation_close(idx, AccessShareLock);
		relation_close(log, AccessShareLock);
	}

	/* the pointers, and the bundles, read under the same snapshot */
	SPI_connect();
	if (SPI_execute("SELECT name, hash FROM pg_history.pointers ORDER BY name", true, 0) != SPI_OK_SELECT)
		elog(ERROR, "could not read the history store's pointers");
	for (uint64 r = 0; r < SPI_processed; r++)
	{
		Datum		values[4];
		bool		nulls[4] = {false, true, false, true};
		bool		isnull;

		values[2] = SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 1, &isnull);
		values[0] = SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 2, &isnull);
		values[1] = (Datum) 0;
		values[3] = (Datum) 0;
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	if (SPI_execute("SELECT name, roots FROM pg_history.bundles ORDER BY name", true, 0) != SPI_OK_SELECT)
		elog(ERROR, "could not read the history store's bundles");
	for (uint64 r = 0; r < SPI_processed; r++)
	{
		Datum		values[4];
		bool		nulls[4] = {true, true, false, false};
		bool		isnull;

		values[0] = (Datum) 0;
		values[1] = (Datum) 0;
		values[2] = SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 1, &isnull);
		values[3] = SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 2, &isnull);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	SPI_finish();
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
 * (a pointer names one), the index read only as far as the first
 */
Datum
pg_history_has_chunks(PG_FUNCTION_ARGS)
{
	Relation	idx = relation_open(ChunkStoreIndexRelid(), AccessShareLock);
	bool		any = false;

	ChunkIndex->visit_as_of(idx, GetActiveSnapshot(), note_chunk, &any);
	relation_close(idx, AccessShareLock);
	/* (its tier's too: its export refuses those, not yet dumped) */
	PG_RETURN_BOOL(any || tier_has_chunks());
}

/*
 * The INSTEAD OF INSERT trigger of pg_history.chunk_restore: a row of
 * (hash, data, name) puts its chunk, its bytes checked against its name, or,
 * with a name and no bytes, inserts its pointer
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
				roots_null;
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

	/* (a bundle's: pushed, its chunks put before it, to the tier the target has) */
	if (!roots_null)
	{
		Oid			types[2] = {TEXTOID, BYTEAARRAYOID};
		Datum		args[2] = {name_d, roots_d};

		if (!hash_null || !data_null || name_null)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("a bundle's row restored has its name and roots alone")));
		SPI_connect();
		if (SPI_execute_with_args("SELECT pg_catalog.pg_history_push($1, $2)",
								  2, types, args, NULL, false, 0) != SPI_OK_SELECT)
			elog(ERROR, "could not restore a history store's bundle");
		SPI_finish();
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
	if (!data_null)
	{
		bytea	   *data = DatumGetByteaPP(data_d);
		Relation	log = relation_open(ChunkStoreLogRelid(), RowExclusiveLock);
		Relation	idx = relation_open(ChunkStoreIndexRelid(), RowExclusiveLock);

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
