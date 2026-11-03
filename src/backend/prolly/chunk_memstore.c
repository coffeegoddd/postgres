/*-------------------------------------------------------------------------
 *
 * chunk_memstore.c
 *	  "memory", a backend's chunk store in memory, for unit tests
 *	  (prolly/chunkstore.h).
 *
 * Each store's chunks, their counts and its pins are its own, kept in hash
 * tables of its own: the backend's store's last as long as the backend (or
 * MemoryChunkStoreReset), a test's own store's until it is destroyed
 * (MemoryChunkStoreCreate: a second store to copy from).  Tests may end a
 * store's holds before the transaction does, and damage it (a chunk
 * dropped, or a byte of it changed).
 * Its lifetime rules are the interface's, made at once, not by
 * transactions: a put, a root's change or a free stands though its
 * transaction aborts.  Its pins are let go of as each transaction ends.
 * Its errors are the history store's (prolly/chunk_store.c), so that one
 * test's output is both stores'.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/chunk_memstore.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "lib/ilist.h"
#include "prolly/chunk_format.h"
#include "prolly/chunk_store.h"
#include "prolly/chunkstore.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

typedef struct MemChunk
{
	uint8		hash[CHUNK_HASH_LEN];	/* the key */
	char	   *data;
	uint32		len;
	uint32		refs;
	uint32		roots;
} MemChunk;

typedef struct MemPin
{
	uint8		hash[CHUNK_HASH_LEN];
} MemPin;

typedef struct MemStore
{
	ChunkStore	cs;				/* first: a ChunkStore is a MemStore */
	MemoryContext cxt;
	HTAB	   *chunks;
	HTAB	   *pins;
	dlist_node	node;			/* in stores */
} MemStore;

static MemStore *backend_store = NULL;
static dlist_head stores = DLIST_STATIC_INIT(stores);

static HTAB *
make_table(MemStore *ms, const char *name, Size entrysize)
{
	HASHCTL		ctl;

	ctl.keysize = CHUNK_HASH_LEN;
	ctl.entrysize = entrysize;
	ctl.hcxt = ms->cxt;
	return hash_create(name, 256, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

static void
end_holds(MemStore *ms)
{
	if (hash_get_num_entries(ms->pins) > 0)
	{
		hash_destroy(ms->pins);
		ms->pins = make_table(ms, "memory chunk store pins", sizeof(MemPin));
	}
}

/* (pins last until the transaction ends) */
static void
release_pins(XactEvent event, void *arg)
{
	dlist_iter	it;

	if (event != XACT_EVENT_COMMIT && event != XACT_EVENT_ABORT &&
		event != XACT_EVENT_PARALLEL_COMMIT &&
		event != XACT_EVENT_PARALLEL_ABORT && event != XACT_EVENT_PREPARE)
		return;
	dlist_foreach(it, &stores)
		end_holds(dlist_container(MemStore, node, it.cur));
}

static MemStore *
store_create(void)
{
	static bool registered = false;
	MemStore   *ms = MemoryContextAllocZero(TopMemoryContext, sizeof(MemStore));

	if (!registered)
	{
		RegisterXactCallback(release_pins, NULL);
		registered = true;
	}
	ms->cs.routine = &MemoryChunkStore;
	ms->cxt = AllocSetContextCreate(TopMemoryContext, "memory chunk store",
									ALLOCSET_DEFAULT_SIZES);
	ms->chunks = make_table(ms, "memory chunk store", sizeof(MemChunk));
	ms->pins = make_table(ms, "memory chunk store pins", sizeof(MemPin));
	dlist_push_tail(&stores, &ms->node);
	return ms;
}

static ChunkStore *
mem_open(void)
{
	if (backend_store == NULL)
		backend_store = store_create();
	return &backend_store->cs;
}

void
MemoryChunkStoreReset(void)
{
	if (backend_store == NULL)
		return;
	MemoryContextReset(backend_store->cxt);
	backend_store->chunks = make_table(backend_store, "memory chunk store",
									   sizeof(MemChunk));
	backend_store->pins = make_table(backend_store, "memory chunk store pins",
									 sizeof(MemPin));
}

ChunkStore *
MemoryChunkStoreCreate(void)
{
	return &store_create()->cs;
}

void
MemoryChunkStoreDestroy(ChunkStore *cs)
{
	MemStore   *ms = (MemStore *) cs;

	Assert(cs->routine == &MemoryChunkStore && ms != backend_store);
	dlist_delete(&ms->node);
	MemoryContextDelete(ms->cxt);
	pfree(ms);
}

void
MemoryChunkStoreEndHolds(ChunkStore *cs)
{
	Assert(cs->routine == &MemoryChunkStore);
	end_holds((MemStore *) cs);
}

static char *
hash_text(const uint8 *hash)
{
	char	   *out = palloc(2 * CHUNK_HASH_LEN + 1);

	hex_encode((const char *) hash, CHUNK_HASH_LEN, out);
	out[2 * CHUNK_HASH_LEN] = '\0';
	return out;
}

static MemChunk *
find(MemStore *ms, const uint8 *hash)
{
	return hash_search(ms->chunks, hash, HASH_FIND, NULL);
}

static void
pin(MemStore *ms, const uint8 *hash)
{
	(void) hash_search(ms->pins, hash, HASH_ENTER, NULL);
}

static bool
mem_hold(ChunkStore *cs, const uint8 *hash)
{
	MemStore   *ms = (MemStore *) cs;
	if (find(ms, hash) == NULL)
		return false;
	pin(ms, hash);
	return true;
}

static void
mem_visit(ChunkStore *cs, ChunkVisitFn fn, void *arg)
{
	MemStore   *ms = (MemStore *) cs;
	HASH_SEQ_STATUS seq;
	MemChunk   *c;

	hash_seq_init(&seq, ms->chunks);
	while ((c = hash_seq_search(&seq)) != NULL)
	{
		ChunkCounts counts = {.refs = c->refs, .roots = c->roots};

		fn(arg, c->hash, c->len, &counts);
	}
}

typedef struct RefList
{
	uint8	   *hashes;
	int			n;
	int			cap;
} RefList;

static void
add_ref(void *arg, const uint8 *hash)
{
	RefList    *list = arg;

	if (list->n == list->cap)
	{
		list->cap = list->cap ? list->cap * 2 : 16;
		list->hashes = list->hashes ?
			repalloc(list->hashes, list->cap * CHUNK_HASH_LEN) :
			palloc(list->cap * CHUNK_HASH_LEN);
	}
	memcpy(list->hashes + list->n * CHUNK_HASH_LEN, hash, CHUNK_HASH_LEN);
	list->n++;
}

static bool
mem_has(ChunkStore *cs, const uint8 *hash)
{
	MemStore   *ms = (MemStore *) cs;
	return find(ms, hash) != NULL;
}

static void *
mem_get(ChunkStore *cs, const uint8 *hash, uint32 *len)
{
	MemStore   *ms = (MemStore *) cs;
	MemChunk   *c = find(ms, hash);
	void	   *data;

	if (c == NULL)
		return NULL;
	data = palloc(Max(c->len, 1));
	memcpy(data, c->data, c->len);
	*len = c->len;
	return data;
}

static void
mem_get_many(ChunkStore *cs, const uint8 *hashes, int n, void **out,
			 uint32 *lens)
{
	for (int i = 0; i < n; i++)
	{
		out[i] = mem_get(cs, hashes + i * CHUNK_HASH_LEN, &lens[i]);
		if (out[i] == NULL)
			lens[i] = 0;
	}
}

static bool
mem_put(ChunkStore *cs, const void *data, uint32 len, uint8 *hash)
{
	MemStore   *ms = (MemStore *) cs;
	RefList		refs = {0};
	MemChunk   *c;
	bool		found;

	ChunkHash(data, len, hash);
	pin(ms, hash);
	if (find(ms, hash))
		return false;
	ChunkFormat->refs(data, len, add_ref, &refs);
	for (int i = 0; i < refs.n; i++)
		if (!find(ms, refs.hashes + i * CHUNK_HASH_LEN))
			ereport(ERROR,
					(errcode(ERRCODE_FOREIGN_KEY_VIOLATION),
					 errmsg("chunk %s references chunk %s, which the store does not have",
							hash_text(hash), hash_text(refs.hashes + i * CHUNK_HASH_LEN))));
	c = hash_search(ms->chunks, hash, HASH_ENTER, &found);
	c->data = MemoryContextAlloc(ms->cxt, Max(len, 1));
	memcpy(c->data, data, len);
	c->len = len;
	c->refs = 0;
	c->roots = 0;
	for (int i = 0; i < refs.n; i++)
		find(ms, refs.hashes + i * CHUNK_HASH_LEN)->refs++;
	if (refs.hashes)
		pfree(refs.hashes);
	return true;
}

/*
 * (each checked first, every name, then what each new chunk references,
 * the store's or the pack's, as "pages" checks them: a pack refused
 * leaves none of it)
 */
static uint64
mem_put_many(ChunkStore *cs, int n, const uint8 *hashes,
			 const void *const *datas, const uint32 *lens)
{
	MemStore   *ms = (MemStore *) cs;
	HASHCTL		ctl;
	HTAB	   *pack;
	bool	   *fresh = palloc0(Max(n, 1) * sizeof(bool));
	uint64		stored = 0;
	uint8		hash[CHUNK_HASH_LEN];

	for (int i = 0; i < n; i++)
	{
		ChunkHash(datas[i], lens[i], hash);
		if (memcmp(hash, hashes + i * CHUNK_HASH_LEN, CHUNK_HASH_LEN) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("bytes put as chunk %s do not hash to it",
							hash_text(hashes + i * CHUNK_HASH_LEN))));
	}
	ctl.keysize = CHUNK_HASH_LEN;
	ctl.entrysize = CHUNK_HASH_LEN;
	ctl.hcxt = CurrentMemoryContext;
	pack = hash_create("memory chunk pack", Max(n, 16), &ctl,
					   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	for (int i = 0; i < n; i++)
	{
		const uint8 *h = hashes + i * CHUNK_HASH_LEN;
		bool		found;

		pin(ms, h);
		(void) hash_search(pack, h, HASH_ENTER, &found);
		fresh[i] = !found && find(ms, h) == NULL;
	}
	for (int i = 0; i < n; i++)
	{
		RefList		refs = {0};

		if (!fresh[i])
			continue;
		ChunkFormat->refs(datas[i], lens[i], add_ref, &refs);
		for (int r = 0; r < refs.n; r++)
		{
			const uint8 *rh = refs.hashes + r * CHUNK_HASH_LEN;

			if (hash_search(pack, rh, HASH_FIND, NULL) == NULL && !find(ms, rh))
				ereport(ERROR,
						(errcode(ERRCODE_FOREIGN_KEY_VIOLATION),
						 errmsg("chunk %s references chunk %s, which the store does not have",
								hash_text(hashes + i * CHUNK_HASH_LEN), hash_text(rh))));
		}
		if (refs.hashes)
			pfree(refs.hashes);
	}
	hash_destroy(pack);

	/* then stored, every one before any reference is counted */
	for (int i = 0; i < n; i++)
	{
		MemChunk   *c;

		if (!fresh[i])
			continue;
		c = hash_search(ms->chunks, hashes + i * CHUNK_HASH_LEN, HASH_ENTER, NULL);
		c->data = MemoryContextAlloc(ms->cxt, Max(lens[i], 1));
		memcpy(c->data, datas[i], lens[i]);
		c->len = lens[i];
		c->refs = 0;
		c->roots = 0;
		stored++;
	}
	for (int i = 0; i < n; i++)
	{
		RefList		refs = {0};

		if (!fresh[i])
			continue;
		ChunkFormat->refs(datas[i], lens[i], add_ref, &refs);
		for (int r = 0; r < refs.n; r++)
			find(ms, refs.hashes + r * CHUNK_HASH_LEN)->refs++;
		if (refs.hashes)
			pfree(refs.hashes);
	}
	pfree(fresh);
	return stored;
}

static void
mem_root(MemStore *ms, const uint8 *hash, int32 droots)
{
	MemChunk   *c = find(ms, hash);
	int64		roots;

	if (c == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("the store has no chunk %s", hash_text(hash))));
	roots = (int64) c->roots + droots;
	if (roots < 0 || roots > PG_UINT32_MAX)
		ereport(ERROR,
				(errcode(roots < 0 ? ERRCODE_INVALID_PARAMETER_VALUE :
						 ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("a chunk's reference counts would be %lld and %lld",
						(long long) c->refs, (long long) roots)));
	c->roots = (uint32) roots;
}

static void
mem_ref_root(ChunkStore *cs, const uint8 *hash)
{
	mem_root((MemStore *) cs, hash, 1);
}

static void
mem_unref_root(ChunkStore *cs, const uint8 *hash)
{
	mem_root((MemStore *) cs, hash, -1);
}

static bool
mem_counts(ChunkStore *cs, const uint8 *hash, ChunkCounts *out)
{
	MemStore   *ms = (MemStore *) cs;
	MemChunk   *c = find(ms, hash);

	if (c == NULL)
		return false;
	out->refs = c->refs;
	out->roots = c->roots;
	return true;
}

static bool
unheld(MemStore *ms, const MemChunk *c)
{
	return c->refs == 0 && c->roots == 0 &&
		hash_search(ms->pins, c->hash, HASH_FIND, NULL) == NULL;
}

static uint64
mem_free(ChunkStore *cs, const uint8 *hashes, int n)
{
	MemStore   *ms = (MemStore *) cs;
	RefList		work = {0};
	uint64		freed = 0;

	for (int i = 0; i < n; i++)
		add_ref(&work, hashes + i * CHUNK_HASH_LEN);

	/* (each freed, the chunks it references one reference less, and maybe free) */
	while (work.n > 0)
	{
		uint8		hash[CHUNK_HASH_LEN];
		MemChunk   *c;
		RefList		refs = {0};

		work.n--;
		memcpy(hash, work.hashes + work.n * CHUNK_HASH_LEN, CHUNK_HASH_LEN);
		c = find(ms, hash);
		if (c == NULL || !unheld(ms, c))
			continue;
		ChunkFormat->refs(c->data, c->len, add_ref, &refs);

		/* (every reference checked before any is let go of) */
		for (int i = 0; i < refs.n; i++)
		{
			MemChunk   *r = find(ms, refs.hashes + i * CHUNK_HASH_LEN);

			if (r == NULL || r->refs == 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("chunk %s references chunk %s, which the store does not have",
								hash_text(hash), hash_text(refs.hashes + i * CHUNK_HASH_LEN))));
		}
		for (int i = 0; i < refs.n; i++)
		{
			MemChunk   *r = find(ms, refs.hashes + i * CHUNK_HASH_LEN);

			r->refs--;
			add_ref(&work, r->hash);
		}
		if (refs.hashes)
			pfree(refs.hashes);
		pfree(c->data);
		(void) hash_search(ms->chunks, hash, HASH_REMOVE, NULL);
		freed++;
	}
	if (work.hashes)
		pfree(work.hashes);
	return freed;
}

static uint64
mem_free_unheld(ChunkStore *cs)
{
	MemStore   *ms = (MemStore *) cs;
	RefList		list = {0};
	HASH_SEQ_STATUS seq;
	MemChunk   *c;
	uint64		freed;

	hash_seq_init(&seq, ms->chunks);
	while ((c = hash_seq_search(&seq)) != NULL)
		if (unheld(ms, c))
			add_ref(&list, c->hash);
	freed = mem_free(cs, list.hashes, list.n);
	if (list.hashes)
		pfree(list.hashes);
	return freed;
}

typedef struct EdgeCount
{
	uint8		hash[CHUNK_HASH_LEN];
	uint64		edges;
} EdgeCount;

typedef struct CheckState
{
	MemStore   *ms;
	HTAB	   *edges;
	char	   *why;
} CheckState;

static void
count_edge(void *arg, const uint8 *hash)
{
	CheckState *st = arg;
	MemStore   *ms = st->ms;
	EdgeCount  *e;
	bool		found;

	if (st->why == NULL && find(ms, hash) == NULL)
		st->why = psprintf("chunk %s, referenced, is not in the store", hash_text(hash));
	e = hash_search(st->edges, hash, HASH_ENTER, &found);
	if (!found)
		e->edges = 0;
	e->edges++;
}

static bool
mem_check(ChunkStore *cs, uint64 *nchunks, uint64 *nunheld, char **why)
{
	MemStore   *ms = (MemStore *) cs;
	HASHCTL		ctl;
	CheckState	st = {0};
	HASH_SEQ_STATUS seq;
	MemChunk   *c;

	*nchunks = *nunheld = 0;
	st.ms = ms;
	ctl.keysize = CHUNK_HASH_LEN;
	ctl.entrysize = sizeof(EdgeCount);
	ctl.hcxt = CurrentMemoryContext;
	st.edges = hash_create("memory chunk store edges", 256, &ctl,
						   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	hash_seq_init(&seq, ms->chunks);
	while ((c = hash_seq_search(&seq)) != NULL)
	{
		uint8		actual[CHUNK_HASH_LEN];

		(*nchunks)++;
		if (c->refs == 0 && c->roots == 0)
			(*nunheld)++;
		ChunkHash(c->data, c->len, actual);
		if (st.why == NULL && memcmp(actual, c->hash, CHUNK_HASH_LEN) != 0)
			st.why = psprintf("stored chunk %s does not match its hash", hash_text(c->hash));
		ChunkFormat->refs(c->data, c->len, count_edge, &st);
	}
	hash_seq_init(&seq, ms->chunks);
	while ((c = hash_seq_search(&seq)) != NULL)
	{
		EdgeCount  *e = hash_search(st.edges, c->hash, HASH_FIND, NULL);
		uint64		edges = e ? e->edges : 0;

		if (st.why == NULL && edges != c->refs)
			st.why = psprintf("chunk %s has %u references counted, and %llu edges to it",
							  hash_text(c->hash), c->refs, (unsigned long long) edges);
	}
	hash_destroy(st.edges);
	*why = st.why;
	return st.why == NULL;
}

const ChunkStoreRoutine MemoryChunkStore = {
	.transactional = false,
	.open = mem_open,
	.has = mem_has,
	.get = mem_get,
	.get_many = mem_get_many,
	.put = mem_put,
	.put_many = mem_put_many,
	.ref_root = mem_ref_root,
	.unref_root = mem_unref_root,
	.counts = mem_counts,
	.free = mem_free,
	.free_unheld = mem_free_unheld,
	.hold = mem_hold,
	.visit = mem_visit,
	.check = mem_check,
};

void
MemoryChunkStoreMiscount(const uint8 *hash)
{
	MemStore   *ms = backend_store;
	MemChunk   *c = ms ? find(ms, hash) : NULL;

	if (c == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("the store has no chunk %s", hash_text(hash))));
	c->refs++;
}

static MemChunk *
must_find(ChunkStore *cs, const uint8 *hash)
{
	MemStore   *ms = (MemStore *) cs;
	MemChunk   *c;

	Assert(cs->routine == &MemoryChunkStore);
	c = find(ms, hash);
	if (c == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("the store has no chunk %s", hash_text(hash))));
	return c;
}

void
MemoryChunkStoreDrop(ChunkStore *cs, const uint8 *hash)
{
	MemChunk   *c = must_find(cs, hash);

	pfree(c->data);
	(void) hash_search(((MemStore *) cs)->chunks, hash, HASH_REMOVE, NULL);
}

void
MemoryChunkStoreDamage(ChunkStore *cs, const uint8 *hash, uint32 at)
{
	MemChunk   *c = must_find(cs, hash);

	if (at >= c->len)
		elog(ERROR, "chunk %s has no byte %u", hash_text(hash), at);
	c->data[at] ^= 1;
}
