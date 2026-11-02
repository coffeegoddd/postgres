/*-------------------------------------------------------------------------
 *
 * chunk_pointers.c
 *	  The history store of a database (pg_history), and the roots its
 *	  pointers hold.
 *
 * Each database has its history store from initdb: pg_history.chunks (the
 * chunk log), pg_history.chunk_index (its index) and pg_history.pointers,
 * the named pointers (branch heads, working roots, the repository's
 * state), each a chunk's hash.  A pointer is a row, made, moved and dropped
 * in the user's transaction; its trigger takes a root on the chunk it
 * names, moves it to the chunk it is moved to, and lets it go when it is
 * dropped, all in that transaction, so that the chunks and their pointer
 * commit or abort together.  A pointer to a chunk the store hasn't is
 * refused, and so is TRUNCATE of the pointers, which fires no row's
 * trigger.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/chunk_pointers.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "catalog/namespace.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "prolly/chunk_store.h"
#include "storage/lmgr.h"
#include "utils/array.h"
#include "utils/fmgrprotos.h"
#include "utils/injection_point.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "varatt.h"

static Oid
store_relid(const char *name)
{
	Oid			relid = get_relname_relid(name, PG_HISTORY_NAMESPACE);

	if (!OidIsValid(relid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("relation \"pg_history.%s\" does not exist", name),
				 errhint("The history store is made by initdb.")));
	return relid;
}

Oid
ChunkStoreLogRelid(void)
{
	return store_relid("chunks");
}

Oid
ChunkStoreIndexRelid(void)
{
	return store_relid("chunk_index");
}

Oid
ChunkStoreTierLogRelid(void)
{
	return store_relid("tier_chunks");
}

Oid
ChunkStoreTierIndexRelid(void)
{
	return store_relid("tier_index");
}

Oid
ChunkStorePointersRelid(void)
{
	return store_relid("pointers");
}

Oid
ChunkStoreBundlesRelid(void)
{
	return store_relid("bundles");
}

void
ChunkStoreLockPointersTo(const uint8 *hash, LOCKMODE mode)
{
	uint32		objid;
	uint16		subid;

	memcpy(&objid, hash, sizeof(objid));
	memcpy(&subid, hash + sizeof(objid), sizeof(subid));
	LockDatabaseObject(ChunkStorePointersRelid(), objid, subid, mode);
}

/*
 * A root about to be taken of hash in the store of idx: an error if the
 * store has not the chunk because it was pushed to the tier, saying so
 */
static void
refuse_pushed(Relation idx, const uint8 *hash)
{
	Relation	tier;
	bool		pushed;

	if (ChunkStoreHas(NULL, idx, hash))
		return;
	tier = relation_open(ChunkStoreTierIndexRelid(), AccessShareLock);
	pushed = ChunkStoreHas(NULL, tier, hash);
	relation_close(tier, NoLock);
	if (pushed)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("chunk %s was pushed to the history store's tier", ChunkHashText(hash)),
				 errdetail("History pushed is not fetched back yet.")));
}

/* the hash a pointer's row names */
static const uint8 *
row_hash(TriggerData *td, HeapTuple tuple)
{
	TupleDesc	desc = RelationGetDescr(td->tg_relation);
	int			attnum = SPI_fnumber(desc, "hash");
	bool		isnull;
	Datum		d;
	bytea	   *b;

	if (attnum <= 0)
		elog(ERROR, "\"%s\" has no column \"hash\"", RelationGetRelationName(td->tg_relation));
	d = heap_getattr(tuple, attnum, desc, &isnull);

	if (isnull)
		elog(ERROR, "a history store pointer names no chunk");
	b = DatumGetByteaPP(d);
	if (VARSIZE_ANY_EXHDR(b) != CHUNK_HASH_LEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a chunk's hash is %d bytes", CHUNK_HASH_LEN)));
	return (const uint8 *) VARDATA_ANY(b);
}

Datum
pg_history_pointer_changed(PG_FUNCTION_ARGS)
{
	TriggerData *td = (TriggerData *) fcinfo->context;
	Relation	idx;
	ChunkIndexEntry e;

	if (!CALLED_AS_TRIGGER(fcinfo))
		elog(ERROR, "pg_history_pointer_changed is a trigger function");
	if (TRIGGER_FIRED_BY_TRUNCATE(td->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot truncate the history store's pointers"),
				 errhint("Delete them: each holds a root on the chunk it names.")));
	if (!TRIGGER_FIRED_FOR_ROW(td->tg_event) || !TRIGGER_FIRED_AFTER(td->tg_event))
		elog(ERROR, "pg_history_pointer_changed is an AFTER ROW trigger");

	/* (another table's rows are no pointers: their roots aren't theirs) */
	if (RelationGetRelid(td->tg_relation) != ChunkStorePointersRelid())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("pg_history_pointer_changed is pg_history.pointers' trigger alone")));

	idx = relation_open(ChunkStoreIndexRelid(), RowExclusiveLock);
	/* (each chunk's pointers locked: a push of it waits, or is waited for) */
	if (TRIGGER_FIRED_BY_INSERT(td->tg_event))
	{
		ChunkStoreLockPointersTo(row_hash(td, td->tg_trigtuple), RowExclusiveLock);
		refuse_pushed(idx, row_hash(td, td->tg_trigtuple));
		ChunkStoreRefRoot(idx, row_hash(td, td->tg_trigtuple), &e);
	}
	else if (TRIGGER_FIRED_BY_DELETE(td->tg_event))
	{
		ChunkStoreLockPointersTo(row_hash(td, td->tg_trigtuple), RowExclusiveLock);
		ChunkStoreUnrefRoot(idx, row_hash(td, td->tg_trigtuple), &e);
	}
	else
	{
		const uint8 *old = row_hash(td, td->tg_trigtuple);
		const uint8 *new = row_hash(td, td->tg_newtuple);

		/* (the new chunk's first: a pointer moved to itself keeps it) */
		if (memcmp(old, new, CHUNK_HASH_LEN) != 0)
		{
			ChunkStoreLockPointersTo(new, RowExclusiveLock);
			ChunkStoreLockPointersTo(old, RowExclusiveLock);
			refuse_pushed(idx, new);
			ChunkStoreRefRoot(idx, new, &e);
			INJECTION_POINT("history-pointer-moving", NULL);
			ChunkStoreUnrefRoot(idx, old, &e);
		}
	}
	relation_close(idx, NoLock);
	return PointerGetDatum(NULL);
}

/*
 * pg_history.bundles' trigger: each row holds its roots in the tier (a
 * bundle's, pushed there: prolly/chunk_push.c), taken as it is inserted
 * and let go of as it is deleted; rows are not changed, and the table is
 * not truncated
 */
Datum
pg_history_bundle_changed(PG_FUNCTION_ARGS)
{
	TriggerData *td = (TriggerData *) fcinfo->context;
	Relation	idx;
	ChunkIndexEntry e;

	if (!CALLED_AS_TRIGGER(fcinfo))
		elog(ERROR, "pg_history_bundle_changed is a trigger function");
	if (TRIGGER_FIRED_BY_TRUNCATE(td->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot truncate the history store's bundles"),
				 errhint("Release them: each holds its roots in the tier.")));
	if (!TRIGGER_FIRED_FOR_ROW(td->tg_event) || !TRIGGER_FIRED_AFTER(td->tg_event))
		elog(ERROR, "pg_history_bundle_changed is an AFTER ROW trigger");
	if (RelationGetRelid(td->tg_relation) != ChunkStoreBundlesRelid())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("pg_history_bundle_changed is pg_history.bundles' trigger alone")));
	if (TRIGGER_FIRED_BY_UPDATE(td->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot change a bundle's roots"),
				 errhint("Push another bundle, and release this one.")));

	idx = relation_open(ChunkStoreTierIndexRelid(), RowExclusiveLock);
	{
		TupleDesc	desc = RelationGetDescr(td->tg_relation);
		int			attnum = SPI_fnumber(desc, "roots");
		bool		isnull;
		Datum		d;
		Datum	   *elems;
		bool	   *nulls;
		int			n;

		if (attnum <= 0)
			elog(ERROR, "\"%s\" has no column \"roots\"", RelationGetRelationName(td->tg_relation));
		d = heap_getattr(td->tg_trigtuple, attnum, desc, &isnull);
		if (isnull)
			elog(ERROR, "a bundle has no roots");
		deconstruct_array(DatumGetArrayTypeP(d), BYTEAOID, -1, false, TYPALIGN_INT,
						  &elems, &nulls, &n);
		for (int i = 0; i < n; i++)
		{
			bytea	   *b;

			if (nulls[i])
				elog(ERROR, "a bundle's root is null");
			b = DatumGetByteaPP(elems[i]);
			if (VARSIZE_ANY_EXHDR(b) != CHUNK_HASH_LEN)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("a chunk's hash is %d bytes", CHUNK_HASH_LEN)));
			/* (a root once: a bundle holds each of its roots once) */
			for (int j = 0; j < i && TRIGGER_FIRED_BY_INSERT(td->tg_event); j++)
				if (memcmp(VARDATA_ANY(DatumGetByteaPP(elems[j])), VARDATA_ANY(b), CHUNK_HASH_LEN) == 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("a bundle's root is given twice")));
			if (TRIGGER_FIRED_BY_INSERT(td->tg_event))
				ChunkStoreRefRoot(idx, (const uint8 *) VARDATA_ANY(b), &e);
			else
				ChunkStoreUnrefRoot(idx, (const uint8 *) VARDATA_ANY(b), &e);
		}
	}
	relation_close(idx, NoLock);
	return PointerGetDatum(NULL);
}
