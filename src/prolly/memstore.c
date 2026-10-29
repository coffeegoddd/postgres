/*
 * memstore.c
 *	  In-memory ChunkStore backend: an open-addressing table of owned chunk
 *	  copies with their metadata.
 */
#include "chunkstore.h"

#define SLOT_EMPTY 0
#define SLOT_USED 1
#define SLOT_DELETED 2

typedef struct MemEntry
{
	PHash		h;
	uint8_t    *data;
	size_t		len;
	ChunkMeta	meta;
	uint8_t		state;
} MemEntry;

typedef struct MemStore
{
	ChunkStore	base;
	MemEntry   *tab;
	size_t		cap;
	size_t		used;
	size_t		deleted;
	uint64_t	bytes;
} MemStore;

static size_t
mem_slot(const PHash *h, size_t cap)
{
	uint64_t	x;

	memcpy(&x, h->b, sizeof(x));
	return (size_t) (x & (cap - 1));
}

/* The entry holding h, or NULL. */
static MemEntry *
mem_find(MemStore *ms, const PHash *h)
{
	for (size_t i = mem_slot(h, ms->cap);; i = (i + 1) & (ms->cap - 1))
	{
		MemEntry   *e = &ms->tab[i];

		if (e->state == SLOT_EMPTY)
			return NULL;
		if (e->state == SLOT_USED && phash_eq(&e->h, h))
			return e;
	}
}

static void
mem_rehash(MemStore *ms, size_t cap)
{
	MemEntry   *old = ms->tab;
	size_t		oldcap = ms->cap;

	ms->tab = calloc(cap, sizeof(MemEntry));
	if (ms->tab == NULL)
		abort();
	ms->cap = cap;
	ms->deleted = 0;
	for (size_t i = 0; i < oldcap; i++)
	{
		if (old[i].state != SLOT_USED)
			continue;
		for (size_t j = mem_slot(&old[i].h, cap);; j = (j + 1) & (cap - 1))
			if (ms->tab[j].state == SLOT_EMPTY)
			{
				ms->tab[j] = old[i];
				break;
			}
	}
	free(old);
}

static const uint8_t *
mem_get(ChunkStore *cs, const PHash *h, size_t *len)
{
	MemEntry   *e = mem_find((MemStore *) cs, h);

	if (e == NULL)
		return NULL;
	*len = e->len;
	return e->data;
}

static bool
mem_put(ChunkStore *cs, const PHash *h, const uint8_t *data, size_t len)
{
	MemStore   *ms = (MemStore *) cs;
	MemEntry   *slot = NULL;

	if (mem_find(ms, h))
		return false;
	if ((ms->used + ms->deleted + 1) * 10 > ms->cap * 7)
		mem_rehash(ms, (ms->used + 1) * 10 > ms->cap * 5 ? ms->cap * 2 : ms->cap);
	for (size_t i = mem_slot(h, ms->cap);; i = (i + 1) & (ms->cap - 1))
	{
		if (ms->tab[i].state != SLOT_USED)
		{
			slot = &ms->tab[i];
			break;
		}
	}
	if (slot->state == SLOT_DELETED)
		ms->deleted--;
	slot->state = SLOT_USED;
	slot->h = *h;
	slot->data = px_memdup(data, len);
	slot->len = len;
	memset(&slot->meta, 0, sizeof(slot->meta));
	ms->used++;
	ms->bytes += len;
	return true;
}

static bool
mem_has(ChunkStore *cs, const PHash *h)
{
	return mem_find((MemStore *) cs, h) != NULL;
}

static ChunkMeta *
mem_meta(ChunkStore *cs, const PHash *h)
{
	MemEntry   *e = mem_find((MemStore *) cs, h);

	return e ? &e->meta : NULL;
}

static void
mem_del(ChunkStore *cs, const PHash *h)
{
	MemStore   *ms = (MemStore *) cs;
	MemEntry   *e = mem_find(ms, h);

	if (e == NULL)
		return;
	free(e->data);
	e->data = NULL;
	ms->bytes -= e->len;
	e->state = SLOT_DELETED;
	ms->used--;
	ms->deleted++;
}

static void
mem_foreach(ChunkStore *cs, ChunkVisitFn fn, void *arg)
{
	MemStore   *ms = (MemStore *) cs;

	for (size_t i = 0; i < ms->cap; i++)
		if (ms->tab[i].state == SLOT_USED)
			fn(arg, &ms->tab[i].h);
}

static size_t
mem_count(ChunkStore *cs)
{
	return ((MemStore *) cs)->used;
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
		if (ms->tab[i].state == SLOT_USED)
			free(ms->tab[i].data);
	free(ms->tab);
	free(ms);
}

static const ChunkStoreOps mem_ops = {
	mem_get, mem_put, mem_has, mem_meta, mem_del, mem_foreach,
	mem_count, mem_bytes, mem_destroy, NULL
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
