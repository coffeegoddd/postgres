/*-------------------------------------------------------------------------
 *
 * chunkstore.c
 *	  The engine's chunk stores, by name (prolly/chunkstore.h), and "pages",
 *	  each database's history store.
 *
 * The server's stores register themselves the first time one is asked
 * for.  "pages" is the history store in pg_history (prolly/chunk_store.h),
 * whose relations it opens, as each call needs them, under the locks a
 * reader or a writer takes; its counts are its transactions'.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/chunkstore.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relation.h"
#include "prolly/chunk_store.h"
#include "prolly/chunkstore.h"
#include "prolly/registry.h"
#include "storage/lmgr.h"

static void
register_stores(void)
{
	static bool registered = false;

	if (registered)
		return;
	ProllyRegisterImplementation(CHUNK_STORE_IFACE, "pages", &PagesChunkStore);
	ProllyRegisterImplementation(CHUNK_STORE_IFACE, "memory", &MemoryChunkStore);
	registered = true;
}

ChunkStore *
ChunkStoreOpen(const char *name)
{
	const ProllyImplementation *impl;

	register_stores();
	impl = ProllyLookupImplementation(CHUNK_STORE_IFACE, name);
	if (impl == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("there is no chunk store \"%s\"", name)));
	return ((const ChunkStoreRoutine *) impl->routine)->open();
}

List *
ChunkStoreList(void)
{
	register_stores();
	return ProllyListImplementations(CHUNK_STORE_IFACE);
}

/* ------------------------------------------------------------- pages */

static ChunkStore pages_store = {.routine = &PagesChunkStore};

static ChunkStore *
pages_open(void)
{
	return &pages_store;
}

/* the store's relations, locked as a reader or a writer */
static void
open_store(LOCKMODE mode, Relation *log, Relation *idx)
{
	if (log)
		*log = relation_open(ChunkStoreLogRelid(), mode);
	*idx = relation_open(ChunkStoreIndexRelid(), mode);
}

static void
close_store(Relation log, Relation idx)
{
	if (log)
		relation_close(log, NoLock);
	relation_close(idx, NoLock);
}

static bool
pages_has(ChunkStore *cs, const uint8 *hash)
{
	Relation	idx;
	bool		has;

	open_store(AccessShareLock, NULL, &idx);
	has = ChunkStoreHas(NULL, idx, hash);
	close_store(NULL, idx);
	return has;
}

static void *
pages_get(ChunkStore *cs, const uint8 *hash, uint32 *len)
{
	Relation	log,
				idx;
	void	   *data;

	open_store(AccessShareLock, &log, &idx);
	data = ChunkStoreGet(log, idx, hash, len);
	close_store(log, idx);
	return data;
}

static bool
pages_put(ChunkStore *cs, const void *data, uint32 len, uint8 *hash)
{
	Relation	log,
				idx;
	bool		stored;

	open_store(RowExclusiveLock, &log, &idx);
	stored = ChunkStorePutData(log, idx, data, len, hash);
	close_store(log, idx);
	return stored;
}

static void
pages_root(const uint8 *hash, bool ref)
{
	Relation	idx;
	ChunkIndexEntry e;

	open_store(RowExclusiveLock, NULL, &idx);
	if (ref)
		ChunkStoreRefRoot(idx, hash, &e);
	else
		ChunkStoreUnrefRoot(idx, hash, &e);
	close_store(NULL, idx);
}

static void
pages_ref_root(ChunkStore *cs, const uint8 *hash)
{
	pages_root(hash, true);
}

static void
pages_unref_root(ChunkStore *cs, const uint8 *hash)
{
	pages_root(hash, false);
}

static bool
pages_counts(ChunkStore *cs, const uint8 *hash, ChunkCounts *out)
{
	Relation	idx;
	ChunkIndexEntry e;
	bool		found;

	open_store(AccessShareLock, NULL, &idx);
	found = ChunkIndex->lookup(idx, hash, &e);
	close_store(NULL, idx);
	if (found)
	{
		out->refs = e.refs;
		out->roots = e.roots;
	}
	return found;
}

static uint64
pages_free(ChunkStore *cs, const uint8 *hashes, int n)
{
	Relation	log,
				idx;
	uint64		freed;

	open_store(RowExclusiveLock, &log, &idx);
	freed = ChunkStoreFree(log, idx, hashes, n);
	close_store(log, idx);
	return freed;
}

static uint64
pages_free_unheld(ChunkStore *cs)
{
	Relation	log,
				idx;
	uint64		freed;

	open_store(RowExclusiveLock, &log, &idx);
	freed = ChunkStoreFreeUnheld(log, idx);
	close_store(log, idx);
	return freed;
}

static bool
pages_check(ChunkStore *cs, uint64 *chunks, uint64 *unheld, char **why)
{
	Relation	log,
				idx;
	bool		ok;

	/*
	 * (under the ShareLock the audit takes, at once, not upgraded to it from
	 * a reader's: so on a primary only)
	 */
	open_store(ShareLock, &log, &idx);
	ok = ChunkStoreCheck(log, idx, chunks, unheld, why);
	close_store(log, idx);
	return ok;
}

const ChunkStoreRoutine PagesChunkStore = {
	.transactional = true,
	.open = pages_open,
	.has = pages_has,
	.get = pages_get,
	.put = pages_put,
	.ref_root = pages_ref_root,
	.unref_root = pages_unref_root,
	.counts = pages_counts,
	.free = pages_free,
	.free_unheld = pages_free_unheld,
	.check = pages_check,
};
