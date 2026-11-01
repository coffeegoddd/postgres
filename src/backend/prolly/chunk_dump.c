/*-------------------------------------------------------------------------
 *
 * chunk_dump.c
 *	  The history store's chunks taken out, and put back, by pg_dump and
 *	  pg_restore.
 *
 * pg_history_export() is every chunk of the store this transaction sees,
 * each after the chunks it references (as ChunkFormat finds them), so that
 * they are put back in order, each put finding what it references there
 * already.  pg_history.chunk_restore is a view of no rows whose INSERT
 * puts a chunk (its bytes checked against its name), for pg_restore to
 * COPY them into; the pointers, COPYd after, take their roots by their own
 * trigger.  pg_history_has_chunks() says whether there are any to dump.
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
#include "commands/trigger.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "prolly/chunk_format.h"
#include "prolly/chunk_index.h"
#include "prolly/chunk_log.h"
#include "prolly/chunk_store.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tuplestore.h"
#include "varatt.h"

static char *
hash_text(const uint8 *hash)
{
	char	   *s = palloc(CHUNK_HASH_LEN * 2 + 1);

	for (int i = 0; i < CHUNK_HASH_LEN; i++)
		sprintf(s + i * 2, "%02x", hash[i]);
	return s;
}

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

static void
collect_chunk(void *arg, const ChunkIndexEntry *e)
{
	hash_list_add((HashList *) arg, e->hash);
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
emit(ReturnSetInfo *rsinfo, const DumpFrame *f)
{
	Datum		values[2];
	bool		nulls[2] = {false, false};
	bytea	   *h = to_bytea(f->hash, CHUNK_HASH_LEN);
	bytea	   *d = to_bytea(f->data, f->len);

	values[0] = PointerGetDatum(h);
	values[1] = PointerGetDatum(d);
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	pfree(h);
	pfree(d);
}

/*
 * pg_history_export(): every chunk, each after those it references, its
 * name and bytes.  Each is visited once (a set of those out, or on the way),
 * by a walk down from each in turn, a chunk out once the chunks it names
 * are.  O(store).
 */
Datum
pg_history_export(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	log;
	Relation	idx;
	HashList	all = {0};
	HASHCTL		ctl;
	HTAB	   *seen;
	DumpFrame  *stack;
	int			depth = 0;
	int			cap = 16;

	InitMaterializedSRF(fcinfo, 0);
	log = relation_open(ChunkStoreLogRelid(), AccessShareLock);
	idx = relation_open(ChunkStoreIndexRelid(), AccessShareLock);
	ChunkIndex->visit(idx, collect_chunk, &all);

	ctl.keysize = CHUNK_HASH_LEN;
	ctl.entrysize = CHUNK_HASH_LEN;
	ctl.hcxt = CurrentMemoryContext;
	seen = hash_create("history export", Max(all.n, 16), &ctl,
					   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	stack = palloc(cap * sizeof(DumpFrame));
	for (int i = 0; i < all.n; i++)
	{
		const uint8 *root = all.hashes + (Size) i * CHUNK_HASH_LEN;
		bool		found;

		(void) hash_search(seen, root, HASH_ENTER, &found);
		if (found)
			continue;

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
				f->data = ChunkStoreGet(log, idx, f->hash, &f->len);
				if (f->data == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("the history store has no chunk %s, which another references",
									hash_text(f->hash))));
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
			emit(rsinfo, f);
			pfree(f->data);
			if (f->refs.hashes)
				pfree(f->refs.hashes);
			depth--;
		}
	}
	relation_close(idx, AccessShareLock);
	relation_close(log, AccessShareLock);
	return (Datum) 0;
}

static void
note_chunk(void *arg, const ChunkIndexEntry *e)
{
	*(bool *) arg = true;
}

/* pg_history_has_chunks(): whether the store has a chunk this transaction sees */
Datum
pg_history_has_chunks(PG_FUNCTION_ARGS)
{
	Relation	idx = relation_open(ChunkStoreIndexRelid(), AccessShareLock);
	bool		any = false;

	ChunkIndex->visit(idx, note_chunk, &any);
	relation_close(idx, AccessShareLock);
	PG_RETURN_BOOL(any);
}

/*
 * The INSTEAD OF INSERT trigger of pg_history.chunk_restore: puts the row's
 * chunk, its bytes checked against its name (hash, data)
 */
Datum
pg_history_chunk_restore(PG_FUNCTION_ARGS)
{
	TriggerData *td = (TriggerData *) fcinfo->context;
	HeapTuple	tuple;
	TupleDesc	desc;
	bool		isnull;
	bytea	   *hash;
	bytea	   *data;
	Relation	log;
	Relation	idx;

	if (!CALLED_AS_TRIGGER(fcinfo) || !TRIGGER_FIRED_INSTEAD(td->tg_event) ||
		!TRIGGER_FIRED_BY_INSERT(td->tg_event) || !TRIGGER_FIRED_FOR_ROW(td->tg_event) ||
		RelationGetNamespace(td->tg_relation) != PG_HISTORY_NAMESPACE)
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("pg_history_chunk_restore() is pg_history.chunk_restore's trigger alone")));
	tuple = td->tg_trigtuple;
	desc = RelationGetDescr(td->tg_relation);
	hash = DatumGetByteaPP(heap_getattr(tuple, 1, desc, &isnull));
	if (isnull || VARSIZE_ANY_EXHDR(hash) != CHUNK_HASH_LEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a chunk's hash is %d bytes", CHUNK_HASH_LEN)));
	data = DatumGetByteaPP(heap_getattr(tuple, 2, desc, &isnull));
	if (isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("a chunk restored has no bytes")));
	log = relation_open(ChunkStoreLogRelid(), RowExclusiveLock);
	idx = relation_open(ChunkStoreIndexRelid(), RowExclusiveLock);
	(void) ChunkStorePut(log, idx, (const uint8 *) VARDATA_ANY(hash),
						 VARDATA_ANY(data), VARSIZE_ANY_EXHDR(data));
	relation_close(idx, NoLock);
	relation_close(log, NoLock);
	return PointerGetDatum(tuple);
}
