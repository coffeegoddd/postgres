/*-------------------------------------------------------------------------
 *
 * chunkstore.h
 *	  The engine's chunk store: content-addressed chunks, and their
 *	  lifetime, behind one interface.
 *
 * Every tree node, schema, commit and delta is an immutable chunk named by
 * the hash of its bytes (prolly/chunk_store.h's ChunkHash).  A store keeps
 * a chunk while anything holds it:
 *	refs	strong references from chunks in the store (as ChunkFormat finds
 *			them), counted per edge as the referring chunk is put
 *	roots	holds from outside the store (branch heads, working sets)
 *	pins	transient holds of the work in flight: a chunk put, or found
 *			already there, is pinned until the transaction ends
 * and only if it keeps every chunk the chunk strongly references: a put of
 * a chunk referencing one the store hasn't is refused.  Freeing cascades
 * through strong references, so it costs the chunks freed.
 *
 * A store owns its lifetime rules: a transactional one keeps its counts as
 * its transactions change them (a put, a root's change or a free is the
 * transaction's, gone if it aborts, and others' once it commits).  One that
 * is not makes them at once, and an abort leaves them.
 *
 * Unlike the prototype engine's store (src/prolly), whose layer above kept
 * the counts: pins last until the transaction ends, with no letting go of
 * them before; letting go of a chunk's last root frees nothing (cs_free,
 * or a store's own freeing in the background, does); a root of a chunk the
 * store hasn't is an error, letting go of one too; and errors are raised,
 * with their codes the contract's: ERRCODE_DATA_CORRUPTED (bytes not their
 * chunk's), ERRCODE_FOREIGN_KEY_VIOLATION (a reference to a chunk not
 * there), ERRCODE_UNDEFINED_OBJECT (a root of one not there), and
 * ERRCODE_INVALID_PARAMETER_VALUE (a root let go of that it hasn't).  A
 * store may need an active snapshot to read ("pages" does).
 *
 * Implementations register with the registry (prolly/registry.h) under
 * CHUNK_STORE_IFACE: "pages", the history store of each database, in its
 * pages (prolly/chunk_store.h); "memory", a backend's, in memory, for unit
 * tests.  Conformance tests every one.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/chunkstore.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_CHUNKSTORE_H
#define PROLLY_CHUNKSTORE_H

#include "prolly/chunk_index.h"

#define CHUNK_STORE_IFACE	"chunk_store"

typedef struct ChunkStore ChunkStore;

typedef struct ChunkCounts
{
	uint32		refs;
	uint32		roots;
} ChunkCounts;

typedef struct ChunkStoreRoutine
{
	/* puts, root changes and frees are their transactions' */
	bool		transactional;

	/* The store, for this backend. */
	ChunkStore *(*open) (void);

	bool		(*has) (ChunkStore *cs, const uint8 *hash);

	/* The chunk's bytes, palloc'd, and their length; NULL if absent. */
	void	   *(*get) (ChunkStore *cs, const uint8 *hash, uint32 *len);

	/*
	 * get of each of hashes[0..n): out[i] and lens[i], NULL for one absent,
	 * read in the store's own order (a tree's level at once; the caller
	 * bounds how many, all of their bytes held at once)
	 */
	void		(*get_many) (ChunkStore *cs, const uint8 *hashes, int n,
							 void **out, uint32 *lens);

	/*
	 * Stores the chunk the bytes are unless the store has it (true if it
	 * stored it), its name in *hash, and pins it, and every chunk it
	 * references.  A strong reference to a chunk the store hasn't is an
	 * error; each one is counted.
	 */
	bool		(*put) (ChunkStore *cs, const void *data, uint32 len,
						uint8 *hash);

	/* A root taken or let go of; of a chunk the store hasn't, an error. */
	void		(*ref_root) (ChunkStore *cs, const uint8 *hash);
	void		(*unref_root) (ChunkStore *cs, const uint8 *hash);

	/* The chunk's counts, as this transaction sees them; false if absent. */
	bool		(*counts) (ChunkStore *cs, const uint8 *hash, ChunkCounts *out);

	/*
	 * Frees those of hashes[0..n) nothing holds, and those their references
	 * alone held, and so on; how many it freed.
	 */
	uint64		(*free) (ChunkStore *cs, const uint8 *hashes, int n);

	/* free of every chunk nothing holds (O(store): for tests and repair) */
	uint64		(*free_unheld) (ChunkStore *cs);

	/*
	 * The audit: every chunk's bytes its own, every chunk it references
	 * there, every count of references the edges to it; the chunks, and
	 * those with no references and no roots (pins, transient, aside), in
	 * *chunks and *unheld, and what is wrong in *why.  O(store).
	 */
	bool		(*check) (ChunkStore *cs, uint64 *chunks, uint64 *unheld,
						  char **why);
} ChunkStoreRoutine;

struct ChunkStore
{
	const ChunkStoreRoutine *routine;
};

extern PGDLLIMPORT const ChunkStoreRoutine PagesChunkStore;
extern PGDLLIMPORT const ChunkStoreRoutine MemoryChunkStore;

/* The store registered as name; an error if there is none. */
extern ChunkStore *ChunkStoreOpen(const char *name);

/* The stores registered (ProllyImplementation), in registration order. */
extern List *ChunkStoreList(void);

/*
 * For tests: empties this backend's memory store; counts one reference of
 * a chunk too many there (for the audit to find).
 */
extern void MemoryChunkStoreReset(void);
extern void MemoryChunkStoreMiscount(const uint8 *hash);

static inline bool
cs_has(ChunkStore *cs, const uint8 *hash)
{
	return cs->routine->has(cs, hash);
}

static inline void *
cs_get(ChunkStore *cs, const uint8 *hash, uint32 *len)
{
	return cs->routine->get(cs, hash, len);
}

static inline void
cs_get_many(ChunkStore *cs, const uint8 *hashes, int n, void **out, uint32 *lens)
{
	cs->routine->get_many(cs, hashes, n, out, lens);
}

static inline bool
cs_put(ChunkStore *cs, const void *data, uint32 len, uint8 *hash)
{
	return cs->routine->put(cs, data, len, hash);
}

static inline void
cs_ref_root(ChunkStore *cs, const uint8 *hash)
{
	cs->routine->ref_root(cs, hash);
}

static inline void
cs_unref_root(ChunkStore *cs, const uint8 *hash)
{
	cs->routine->unref_root(cs, hash);
}

static inline bool
cs_counts(ChunkStore *cs, const uint8 *hash, ChunkCounts *out)
{
	return cs->routine->counts(cs, hash, out);
}

static inline uint64
cs_free(ChunkStore *cs, const uint8 *hashes, int n)
{
	return cs->routine->free(cs, hashes, n);
}

static inline uint64
cs_free_unheld(ChunkStore *cs)
{
	return cs->routine->free_unheld(cs);
}

static inline bool
cs_check(ChunkStore *cs, uint64 *chunks, uint64 *unheld, char **why)
{
	return cs->routine->check(cs, chunks, unheld, why);
}

#endif							/* PROLLY_CHUNKSTORE_H */
