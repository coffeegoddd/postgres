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
 *	holds	transient holds of the work in flight: a chunk put, or found
 *			already there, is held until the transaction ends (those it
 *			references are held by the references counted), or at least
 *			until the subtransaction that put it is rolled back
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
 * the counts: holds last until the transaction ends (or at least until a
 * rollback to a savepoint before them), with no letting go of them
 * before; letting go of a chunk's last root frees nothing (cs_free, or a
 * store's own freeing in the background, does); a root of a chunk the
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

/* A chunk as a visit of the store sees it. */
typedef void (*ChunkVisitFn) (void *arg, const uint8 *hash, uint32 len,
							  const ChunkCounts *counts);

/*
 * What a store was asked, counted by the cs_* calls since the backend first
 * opened it (or its counts were reset): for tests and benchmarks to hold
 * costs to.  A call that fails counts nothing.
 */
typedef struct ChunkStoreStats
{
	uint64		gets;			/* chunks asked for (get, get_many) */
	uint64		found;			/* ... and found */
	uint64		puts;			/* chunks put (put, put_many) */
	uint64		stored;			/* ... and stored (the store had them not) */
	uint64		holds;			/* chunks asked to be held (hold) */
	uint64		held;			/* ... and held (the store had them) */
	uint64		freed;			/* chunks freed (free, free_unheld) */
} ChunkStoreStats;

typedef struct ChunkStoreRoutine
{
	/* puts, root changes and frees are their transactions' */
	bool		transactional;

	/* The store, for this backend. */
	ChunkStore *(*open) (void);

	bool		(*has) (ChunkStore *cs, const uint8 *hash);

	/*
	 * The chunk's bytes and their length; NULL if absent.  The bytes are a
	 * copy, palloc'd in the caller's memory context, the caller's to free:
	 * a long walk resets a context of its own a batch at a time.
	 */
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
	 * stored it), its name in *hash, and holds it until the transaction
	 * ends (by its entry, or by a change of its counts adding nothing).
	 * A strong reference to a chunk the store hasn't is an error; each one
	 * is counted, which holds the chunk it names (a put refused holds
	 * none).
	 */
	bool		(*put) (ChunkStore *cs, const void *data, uint32 len,
						uint8 *hash);

	/*
	 * A pack's chunks put at once, as a put of each, hashes[i] naming
	 * datas[i] (bytes not their name's an error), but a chunk may reference
	 * another of the pack, before or after it; those the store has, and
	 * those named twice, put once.  How many it stored.  A store may put
	 * them more cheaply than one at a time ("pages" appends their bytes
	 * together, in one pass).
	 */
	uint64		(*put_many) (ChunkStore *cs, int n, const uint8 *hashes,
							 const void *const *datas, const uint32 *lens);

	/*
	 * Whether the store has the chunk, held until the transaction ends if
	 * so, as a put finding it holds it: for a chunk about to be referenced
	 * (a root taken, a parent put) without being put, whose freeing
	 * meanwhile would make that an error.
	 */
	bool		(*hold) (ChunkStore *cs, const uint8 *hash);

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
	 * Calls fn with each chunk this transaction sees, its counts as it
	 * sees them, in no order (O(store): archives, repair, audits).  fn may
	 * not change the store.
	 */
	void		(*visit) (ChunkStore *cs, ChunkVisitFn fn, void *arg);

	/*
	 * The audit: every chunk's bytes its own, every chunk it references
	 * there, every count of references the edges to it; the chunks, and
	 * those with no references and no roots (holds, transient, aside), in
	 * *chunks and *unheld, and what is wrong in *why.  O(store).
	 */
	bool		(*check) (ChunkStore *cs, uint64 *chunks, uint64 *unheld,
						  char **why);
} ChunkStoreRoutine;

struct ChunkStore
{
	const ChunkStoreRoutine *routine;
	ChunkStoreStats stats;		/* counted by the cs_* calls */
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
	void	   *data = cs->routine->get(cs, hash, len);

	cs->stats.gets++;
	if (data != NULL)
		cs->stats.found++;
	return data;
}

static inline void
cs_get_many(ChunkStore *cs, const uint8 *hashes, int n, void **out, uint32 *lens)
{
	cs->routine->get_many(cs, hashes, n, out, lens);
	cs->stats.gets += n;
	for (int i = 0; i < n; i++)
		if (out[i] != NULL)
			cs->stats.found++;
}

static inline bool
cs_put(ChunkStore *cs, const void *data, uint32 len, uint8 *hash)
{
	bool		stored = cs->routine->put(cs, data, len, hash);

	cs->stats.puts++;
	if (stored)
		cs->stats.stored++;
	return stored;
}

static inline uint64
cs_put_many(ChunkStore *cs, int n, const uint8 *hashes, const void *const *datas,
			const uint32 *lens)
{
	uint64		stored = cs->routine->put_many(cs, n, hashes, datas, lens);

	cs->stats.puts += n;
	cs->stats.stored += stored;
	return stored;
}

static inline bool
cs_hold(ChunkStore *cs, const uint8 *hash)
{
	bool		held = cs->routine->hold(cs, hash);

	cs->stats.holds++;
	if (held)
		cs->stats.held++;
	return held;
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
	uint64		freed = cs->routine->free(cs, hashes, n);

	cs->stats.freed += freed;
	return freed;
}

static inline uint64
cs_free_unheld(ChunkStore *cs)
{
	uint64		freed = cs->routine->free_unheld(cs);

	cs->stats.freed += freed;
	return freed;
}

static inline void
cs_visit(ChunkStore *cs, ChunkVisitFn fn, void *arg)
{
	cs->routine->visit(cs, fn, arg);
}

/* What the store was asked since it was opened, or its counts reset. */
static inline const ChunkStoreStats *
cs_stats(ChunkStore *cs)
{
	return &cs->stats;
}

static inline void
cs_stats_reset(ChunkStore *cs)
{
	memset(&cs->stats, 0, sizeof(cs->stats));
}

static inline bool
cs_check(ChunkStore *cs, uint64 *chunks, uint64 *unheld, char **why)
{
	return cs->routine->check(cs, chunks, unheld, why);
}

#endif							/* PROLLY_CHUNKSTORE_H */
