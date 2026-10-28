/*
 * chunkstore.h
 *	  Content-addressed chunk storage interface.
 *
 * Every tree node, schema and commit is an immutable chunk named by the
 * hash of its bytes.  Implementations plug in through ChunkStoreOps; the
 * cs_* wrappers count every call so tests can measure the asymptotic cost
 * of an operation in chunk reads and writes.
 *
 * Contract: bytes returned by get stay valid for the life of the store, and
 * a store only holds a chunk once it holds every chunk the chunk refers to
 * (writers put children before parents).
 */
#ifndef CHUNKSTORE_H
#define CHUNKSTORE_H

#include "phash.h"

typedef struct ChunkStoreStats
{
	uint64_t	gets;
	uint64_t	puts;
	uint64_t	new_chunks;		/* puts that stored a chunk not already present */
	uint64_t	new_bytes;
	uint64_t	has_calls;
} ChunkStoreStats;

typedef struct ChunkStore ChunkStore;

typedef struct ChunkStoreOps
{
	/* Returns the chunk's bytes, or NULL if absent. */
	const uint8_t *(*get) (ChunkStore *cs, const PHash *h, size_t *len);
	/* Stores the chunk; returns true if it was not already present. */
	bool		(*put) (ChunkStore *cs, const PHash *h, const uint8_t *data, size_t len);
	bool		(*has) (ChunkStore *cs, const PHash *h);
	size_t		(*count) (ChunkStore *cs);
	uint64_t	(*bytes) (ChunkStore *cs);
	void		(*destroy) (ChunkStore *cs);
} ChunkStoreOps;

struct ChunkStore
{
	const ChunkStoreOps *ops;
	ChunkStoreStats stats;
};

static inline const uint8_t *
cs_get(ChunkStore *cs, const PHash *h, size_t *len)
{
	cs->stats.gets++;
	return cs->ops->get(cs, h, len);
}

static inline bool
cs_put(ChunkStore *cs, const PHash *h, const uint8_t *data, size_t len)
{
	bool		isnew = cs->ops->put(cs, h, data, len);

	cs->stats.puts++;
	if (isnew)
	{
		cs->stats.new_chunks++;
		cs->stats.new_bytes += len;
	}
	return isnew;
}

static inline bool
cs_has(ChunkStore *cs, const PHash *h)
{
	cs->stats.has_calls++;
	return cs->ops->has(cs, h);
}

static inline PHash
cs_put_chunk(ChunkStore *cs, const uint8_t *data, size_t len)
{
	PHash		h;

	phash_compute(data, len, &h);
	cs_put(cs, &h, data, len);
	return h;
}

static inline void
cs_destroy(ChunkStore *cs)
{
	cs->ops->destroy(cs);
}

/* In-memory implementation. */
extern ChunkStore *memstore_create(void);

#endif							/* CHUNKSTORE_H */
