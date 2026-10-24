/*-------------------------------------------------------------------------
 *
 * chunkstore_local.c
 *	  Chunks kept in the backend's memory: the trees a step builds only to
 *	  compare with.
 *
 * A move from a dirty working set folds it to compare it with the target,
 * and to have a baseline to advance to the target after; the fold's trees
 * are then no part of anything kept, but put in the server's store they
 * are appended to its journal, WAL-logged and synced all the same.  So,
 * between prolly_local_chunks_begin() and _end(), the trees built in an
 * environment whose store is the keeping one go into a table in the
 * backend's memory instead, and every environment of the server's trees
 * (prolly_folded_tree_env()'s, whose store is the reading one) reads them
 * from there.  What outlives the step is persisted from its root: those
 * of its chunks kept here, and what they name that is kept here too.
 *
 * Both routines are the server's store's, but for the callbacks below: a
 * chunk kept here is found by get() through either, and by has() only
 * through the keeping one, so a tree built in another environment (a
 * durable one) puts every node it names itself, never taking one for
 * stored that only this backend has.  The server's store checks that what
 * it is given names only what it has, too.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/chunkstore_local.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_chunkstore.h"
#include "access/prolly_hash.h"
#include "access/prolly_impl.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

/* A chunk kept, by its hash (zero-padded, as a key). */
typedef struct LocalChunk
{
	ProllyHash	key;
	char	   *data;
	Size		len;
	bool		persisted;
} LocalChunk;

static HTAB *local_chunks = NULL;	/* NULL once spilled */
static MemoryContext local_cxt = NULL;
static bool local_open = false;
static Size local_bytes = 0;
static SubTransactionId local_subid = InvalidSubTransactionId;
static bool callbacks_registered = false;

static ProllyChunkStoreRoutine keeping;
static ProllyChunkStoreRoutine reading;
static bool routines_ready = false;

static uint32
local_key_hash(const void *key, Size keysize)
{
	return prolly_hash_bucket(((const ProllyHash *) key)->bytes);
}

static LocalChunk *
local_find(const ProllyHash *hash)
{
	ProllyHash	key;

	if (local_chunks == NULL)
		return NULL;
	memset(&key, 0, sizeof(key));
	key.len = hash->len;
	memcpy(key.bytes, hash->bytes, hash->len);
	return hash_search(local_chunks, &key, HASH_FIND, NULL);
}

static void local_spill(ProllyChunkStore *store);

static void
keeping_put(ProllyChunkStore *store, const ProllyHash *hash, const void *data,
			Size len)
{
	ProllyHash	key;
	LocalChunk *c;
	bool		found;

	if (local_chunks == NULL)
	{
		PROLLY_CALL(prolly_chunk_store(), put, store, hash, data, len);
		return;
	}
	if (local_bytes + len > (Size) maintenance_work_mem * 1024)
	{
		local_spill(store);
		PROLLY_CALL(prolly_chunk_store(), put, store, hash, data, len);
		return;
	}
	memset(&key, 0, sizeof(key));
	key.len = hash->len;
	memcpy(key.bytes, hash->bytes, hash->len);
	c = hash_search(local_chunks, &key, HASH_ENTER, &found);
	if (found)
		return;
	c->data = MemoryContextAlloc(local_cxt, Max(len, 1));
	memcpy(c->data, data, len);
	c->len = len;
	c->persisted = false;
	local_bytes += len;
}

static bool
local_get(ProllyChunkStore *store, const ProllyHash *hash, StringInfo out)
{
	LocalChunk *c = local_find(hash);

	if (c != NULL)
	{
		appendBinaryStringInfo(out, c->data, c->len);
		return true;
	}
	return PROLLY_CALL(prolly_chunk_store(), get, store, hash, out);
}

static bool
keeping_has(ProllyChunkStore *store, const ProllyHash *hash)
{
	return local_find(hash) != NULL ||
		PROLLY_CALL(prolly_chunk_store(), has, store, hash);
}

static void
local_get_many(ProllyChunkStore *store, const ProllyHash *hashes, int n,
			   ProllyChunkManyVisit visit, void *arg)
{
	ProllyHash *rest;
	int		   *at;
	int			nrest = 0;
	const ProllyChunkStoreRoutine *cs = prolly_chunk_store();

	if (local_chunks == NULL)
	{
		PROLLY_CALL(cs, get_many, store, hashes, n, visit, arg);
		return;
	}
	rest = palloc(sizeof(ProllyHash) * Max(n, 1));
	at = palloc(sizeof(int) * Max(n, 1));
	for (int i = 0; i < n; i++)
	{
		LocalChunk *c = local_find(&hashes[i]);

		if (c != NULL)
			visit(i, c->data, c->len, arg);
		else
		{
			rest[nrest] = hashes[i];
			at[nrest++] = i;
		}
	}
	for (int i = 0; i < nrest; i++)
	{
		StringInfoData data;

		/* (few: the chunks of a step's trees are kept here mostly) */
		initStringInfo(&data);
		if (PROLLY_CALL(cs, get, store, &rest[i], &data))
			visit(at[i], data.data, data.len, arg);
		else
			visit(at[i], NULL, 0, arg);
		pfree(data.data);
	}
	pfree(rest);
	pfree(at);
}

/* A chunk kept here is the step's: nothing outside it names it. */
static void
keeping_keep(ProllyChunkStore *store, const ProllyHash *hash)
{
	if (local_find(hash) == NULL)
		PROLLY_CALL(prolly_chunk_store(), keep, store, hash);
}

static void
routines_init(void)
{
	if (routines_ready)
		return;
	keeping = *prolly_chunk_store();
	keeping.put = keeping_put;
	keeping.get = local_get;
	keeping.has = keeping_has;
	keeping.get_many = local_get_many;
	keeping.keep = keeping_keep;
	reading = *prolly_chunk_store();
	reading.get = local_get;
	reading.get_many = local_get_many;
	routines_ready = true;
}

const ProllyChunkStoreRoutine *
prolly_local_chunks_keeping(void)
{
	routines_init();
	return &keeping;
}

const ProllyChunkStoreRoutine *
prolly_local_chunks_reading(void)
{
	routines_init();
	return &reading;
}

bool
prolly_local_chunks_kept(const ProllyChunkStoreRoutine *store)
{
	return store == &keeping && local_chunks != NULL;
}

const ProllyChunkStoreRoutine *
prolly_local_chunks_store(const ProllyChunkStoreRoutine *store)
{
	if (routines_ready && (store == &keeping || store == &reading))
		return prolly_chunk_store();
	return store;
}

bool
prolly_local_chunks_active(void)
{
	return local_open;
}

static void
local_forget(void)
{
	if (local_chunks != NULL)
		hash_destroy(local_chunks);
	local_chunks = NULL;
	if (local_cxt != NULL)
		MemoryContextDelete(local_cxt);
	local_cxt = NULL;
	local_bytes = 0;
}

void
prolly_local_chunks_end(void)
{
	if (!local_open)
		return;
	local_forget();
	local_open = false;
	local_subid = InvalidSubTransactionId;
}

static void
local_xact(XactEvent event, void *arg)
{
	prolly_local_chunks_end();
}

static void
local_subxact(SubXactEvent event, SubTransactionId mySubid,
			  SubTransactionId parentSubid, void *arg)
{
	if (event == SUBXACT_EVENT_ABORT_SUB && mySubid == local_subid)
		prolly_local_chunks_end();
}

void
prolly_local_chunks_begin(void)
{
	HASHCTL		ctl;

	if (local_open)
		elog(ERROR, "local chunks are begun already");
	if (!callbacks_registered)
	{
		RegisterXactCallback(local_xact, NULL);
		RegisterSubXactCallback(local_subxact, NULL);
		callbacks_registered = true;
	}
	local_cxt = AllocSetContextCreate(TopTransactionContext,
									  "prolly local chunks",
									  ALLOCSET_DEFAULT_SIZES);
	ctl.keysize = sizeof(ProllyHash);
	ctl.entrysize = sizeof(LocalChunk);
	ctl.hash = local_key_hash;
	ctl.hcxt = local_cxt;
	local_chunks = hash_create("prolly local chunks", 256, &ctl,
							   HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);
	local_subid = GetCurrentSubTransactionId();
	local_bytes = 0;
	local_open = true;
}

/* The chunks a chunk names, collected. */
typedef struct Named
{
	ProllyHash *refs;
	int			n;
	int			cap;
} Named;

static void
note_named(const ProllyHash *ref, void *arg)
{
	Named	   *nm = arg;

	if (nm->n == nm->cap)
	{
		nm->cap *= 2;
		nm->refs = repalloc(nm->refs, sizeof(ProllyHash) * nm->cap);
	}
	nm->refs[nm->n++] = *ref;
}

/*
 * (Depth-first: a tree's depth is a few levels; each chunk persisted once,
 * after what it names.)
 */
static void
persist_chunk(ProllyChunkStore *store, LocalChunk *c)
{
	Named		nm;

	c->persisted = true;
	nm.cap = 16;
	nm.n = 0;
	nm.refs = palloc(sizeof(ProllyHash) * nm.cap);
	prolly_chunk_references(c->data, c->len, note_named, &nm);
	for (int i = 0; i < nm.n; i++)
	{
		LocalChunk *child = local_find(&nm.refs[i]);

		if (child != NULL && !child->persisted &&
			!PROLLY_CALL(prolly_chunk_store(), has, store, &nm.refs[i]))
			persist_chunk(store, child);
	}
	pfree(nm.refs);
	PROLLY_CALL(prolly_chunk_store(), put, store, &c->key, c->data, c->len);
}

void
prolly_local_chunks_persist(const ProllyHash *hash)
{
	const ProllyChunkStoreRoutine *cs = prolly_chunk_store();
	ProllyChunkStore *store;
	LocalChunk *c = local_find(hash);

	if (c == NULL || c->persisted)
		return;
	store = PROLLY_CALL(cs, open, MyDatabaseId, MyDatabaseTableSpace);
	if (!PROLLY_CALL(cs, has, store, hash))
		persist_chunk(store, c);
}

/*
 * Past maintenance_work_mem of chunks kept, the step stops keeping them:
 * those kept are persisted (each after those it names) and forgotten, and
 * every put from then on goes to the store, as if nothing had been kept.
 * So a step that builds much (a fold of most of a table) costs the memory
 * an ordinary fold's pending appends do, no more, and saves nothing.
 */
static void
local_spill(ProllyChunkStore *store)
{
	HASH_SEQ_STATUS seq;
	LocalChunk *c;

	elog(DEBUG1, "chunks kept in memory past maintenance_work_mem: %zu bytes put in the store",
		 local_bytes);
	hash_seq_init(&seq, local_chunks);
	while ((c = hash_seq_search(&seq)) != NULL)
		if (!c->persisted &&
			!PROLLY_CALL(prolly_chunk_store(), has, store, &c->key))
			persist_chunk(store, c);
	local_forget();
}
