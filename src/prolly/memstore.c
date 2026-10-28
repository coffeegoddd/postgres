/*
 * memstore.c
 *	  In-memory ChunkStore: an open-addressing table of owned chunk copies.
 */
#include "chunkstore.h"

typedef struct MemEntry
{
	PHash		h;
	uint8_t    *data;
	size_t		len;
	bool		used;
} MemEntry;

typedef struct MemStore
{
	ChunkStore	base;
	MemEntry   *tab;
	size_t		cap;
	size_t		n;
	uint64_t	bytes;
} MemStore;

static size_t
mem_slot(const PHash *h, size_t cap)
{
	uint64_t	x;

	memcpy(&x, h->b, sizeof(x));
	return (size_t) (x & (cap - 1));
}

static MemEntry *
mem_lookup(MemStore *ms, const PHash *h)
{
	for (size_t i = mem_slot(h, ms->cap);; i = (i + 1) & (ms->cap - 1))
	{
		MemEntry   *e = &ms->tab[i];

		if (!e->used || phash_eq(&e->h, h))
			return e;
	}
}

static void
mem_grow(MemStore *ms)
{
	MemEntry   *old = ms->tab;
	size_t		oldcap = ms->cap;

	ms->cap *= 2;
	ms->tab = calloc(ms->cap, sizeof(MemEntry));
	if (ms->tab == NULL)
		abort();
	for (size_t i = 0; i < oldcap; i++)
		if (old[i].used)
			*mem_lookup(ms, &old[i].h) = old[i];
	free(old);
}

static const uint8_t *
mem_get(ChunkStore *cs, const PHash *h, size_t *len)
{
	MemEntry   *e = mem_lookup((MemStore *) cs, h);

	if (!e->used)
		return NULL;
	*len = e->len;
	return e->data;
}

static bool
mem_put(ChunkStore *cs, const PHash *h, const uint8_t *data, size_t len)
{
	MemStore   *ms = (MemStore *) cs;
	MemEntry   *e;

	if ((ms->n + 1) * 10 > ms->cap * 7)
		mem_grow(ms);
	e = mem_lookup(ms, h);
	if (e->used)
		return false;
	e->used = true;
	e->h = *h;
	e->data = px_memdup(data, len);
	e->len = len;
	ms->n++;
	ms->bytes += len;
	return true;
}

static bool
mem_has(ChunkStore *cs, const PHash *h)
{
	return mem_lookup((MemStore *) cs, h)->used;
}

static size_t
mem_count(ChunkStore *cs)
{
	return ((MemStore *) cs)->n;
}

static uint64_t
mem_bytes(ChunkStore *cs)
{
	return ((MemStore *) cs)->bytes;
}

static void
mem_destroy(ChunkStore *cs)
{
	MemStore   *ms = (MemStore *) cs;

	for (size_t i = 0; i < ms->cap; i++)
		if (ms->tab[i].used)
			free(ms->tab[i].data);
	free(ms->tab);
	free(ms);
}

static const ChunkStoreOps mem_ops = {
	mem_get, mem_put, mem_has, mem_count, mem_bytes, mem_destroy
};

ChunkStore *
memstore_create(void)
{
	MemStore   *ms = px_malloc(sizeof(MemStore));

	memset(ms, 0, sizeof(*ms));
	ms->base.ops = &mem_ops;
	ms->cap = 1024;
	ms->tab = calloc(ms->cap, sizeof(MemEntry));
	if (ms->tab == NULL)
		abort();
	return &ms->base;
}
