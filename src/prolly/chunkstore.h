/*
 * chunkstore.h
 *	  Content-addressed chunk storage, with chunk lifetime.
 *
 * Every tree node, schema, commit and delta is an immutable chunk named by
 * the hash of its bytes.  Backends plug in through ChunkStoreOps; the
 * lifetime rules are implemented once, above them, in chunkstore.c.
 *
 * Invariant: a store only holds a chunk if it holds every chunk the chunk
 * strongly references.  cs_put refuses a chunk whose strong references are
 * absent, and a chunk is freed only when nothing keeps it:
 *	refs	strong references from chunks in the store, counted per edge and
 *			added only when the referring chunk is first stored
 *	roots	durable references from outside the store (branch heads,
 *			commits kept in history, working sets)
 *	pins	transient holds by work in flight
 * Freeing cascades through strong references, so its cost is the number of
 * chunks freed.
 *
 * Writers pin every chunk they store or find already stored until the
 * result is held by a root.  That closes the race in which a chunk with no
 * holders is about to be freed just as a writer reuses it.  Readers hold
 * the root they read; the invariant then keeps everything under it.
 *
 * Bytes returned by cs_get stay valid while the chunk is held.
 */
#ifndef CHUNKSTORE_H
#define CHUNKSTORE_H

#include "chunkfmt.h"

#define CS_OK 0
#define CS_ERR_MISSING (-1)		/* a needed chunk is not in the store */
#define CS_ERR_CORRUPT (-2)		/* bytes do not parse or do not match their hash */

typedef struct ChunkMeta
{
	uint64_t	refs;
	uint64_t	roots;
	uint64_t	pins;
} ChunkMeta;

typedef struct ChunkStoreStats
{
	uint64_t	gets;
	uint64_t	puts;
	uint64_t	new_chunks;		/* puts that stored a chunk not already present */
	uint64_t	new_bytes;
	uint64_t	has_calls;
	uint64_t	frees;
	uint64_t	freed_bytes;
	uint64_t	ref_updates;	/* strong reference counts changed */
} ChunkStoreStats;

typedef struct ChunkStore ChunkStore;

typedef void (*ChunkVisitFn) (void *arg, const PHash *h);

typedef struct ChunkStoreOps
{
	/* Returns the chunk's bytes, or NULL if absent. */
	const uint8_t *(*get) (ChunkStore *cs, const PHash *h, size_t *len);
	/* Stores a new chunk with zeroed metadata; false if already present. */
	bool		(*put) (ChunkStore *cs, const PHash *h, const uint8_t *data, size_t len);
	bool		(*has) (ChunkStore *cs, const PHash *h);
	/* The chunk's metadata, or NULL if absent; valid until the next put or del. */
	ChunkMeta  *(*meta) (ChunkStore *cs, const PHash *h);
	void		(*del) (ChunkStore *cs, const PHash *h);
	void		(*foreach) (ChunkStore *cs, ChunkVisitFn fn, void *arg);
	size_t		(*count) (ChunkStore *cs);
	uint64_t	(*bytes) (ChunkStore *cs);
	void		(*destroy) (ChunkStore *cs);
	/* Optional: told each time a chunk's durable root count changes. */
	void		(*note_root) (ChunkStore *cs, const PHash *h, uint64_t roots);
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
cs_has(ChunkStore *cs, const PHash *h)
{
	cs->stats.has_calls++;
	return cs->ops->has(cs, h);
}

static inline void
cs_destroy(ChunkStore *cs)
{
	cs->ops->destroy(cs);
}

/*
 * Stores a chunk (a no-op if already present) and returns its hash.  Fails
 * with CS_ERR_CORRUPT if the bytes do not parse, or CS_ERR_MISSING naming a
 * strong reference the store lacks.  The caller must hold the result.
 */
extern int	cs_put(ChunkStore *cs, const uint8_t *data, size_t len, PHash *out,
				   PHash *err_hash);

/* Durable holds; the chunk must be present. */
extern void cs_ref_root(ChunkStore *cs, const PHash *h);
extern void cs_unref_root(ChunkStore *cs, const PHash *h);

/* Transient holds. */
extern void cs_pin(ChunkStore *cs, const PHash *h);
extern void cs_unpin(ChunkStore *cs, const PHash *h);

/* A set of pins taken by one piece of work, released together. */
typedef struct PinSet
{
	ChunkStore *cs;
	PHash	   *v;
	size_t		n;
	size_t		cap;
} PinSet;

extern void pinset_init(PinSet *ps, ChunkStore *cs);
extern void pinset_add(PinSet *ps, const PHash *h);
extern void pinset_release(PinSet *ps);
extern void pinset_free(PinSet *ps);

/* Frees every chunk nothing holds (left behind by failed work); returns how many. */
extern size_t cs_collect_orphans(ChunkStore *cs);

/*
 * Copies the strong closure of h from src into the store behind dst,
 * skipping any chunk dst already has (the invariant makes it complete).
 * Children are stored before parents, and every copied chunk is pinned in
 * dst.  With verify, each chunk's bytes must hash to its name.
 */
extern int	cs_copy_closure(ChunkStore *src, PinSet *dst, const PHash *h,
							bool verify, PHash *err_hash);

/*
 * Audits the whole store: every chunk parses, every strong reference is
 * present, and every refs count equals the strong edges that point at it.
 * Reports chunks nothing holds in *orphans.  For tests; O(store).
 */
extern bool cs_check_invariants(ChunkStore *cs, size_t *orphans, char *why,
								size_t whylen);

/* In-memory backend. */
extern ChunkStore *memstore_create(void);

/*
 * File-backed backend: one append-only data file, memory-mapped for reads,
 * with an in-memory index.  Freed bytes are not reclaimed in the file.
 */
extern ChunkStore *filestore_create(const char *path);
extern uint64_t filestore_file_bytes(ChunkStore *cs);

/*
 * Durable backend: a directory of immutable chunk files plus a MANIFEST
 * (see dirstore.c for the format).  Chunks are written by dirstore_flush
 * as new files; dirstore_compact (garbage collection) copies the live
 * chunks of files with at least min_dead_fraction dead space, and of small
 * files, into new files, installs a manifest without the old ones and
 * deletes them.  Reference counts are rebuilt on open from the manifest's
 * roots.  cs_destroy flushes; dirstore_crash lets go without writing.
 * Flush and compaction invalidate bytes previously returned by get.
 */
#define DS_CRASHED (-30)			/* stopped at a failpoint */
#define DS_FP_FLUSH_BEFORE_RENAME 1
#define DS_FP_FLUSH_BEFORE_MANIFEST 2
#define DS_FP_COMPACT_BEFORE_RENAME 3
#define DS_FP_COMPACT_BEFORE_MANIFEST 4
#define DS_FP_COMPACT_BEFORE_UNLINK 5

typedef struct DirCompactStats
{
	uint64_t	files_before,
				files_after;
	uint64_t	disk_before,
				disk_after;		/* bytes allocated on disk */
	uint64_t	chunks_copied,
				bytes_copied;
} DirCompactStats;

extern ChunkStore *dirstore_open(const char *dir, char *err, size_t errlen);
extern int	dirstore_flush(ChunkStore *cs);
extern int	dirstore_compact(ChunkStore *cs, double min_dead_fraction,
							 DirCompactStats *stats);
extern uint64_t dirstore_disk_bytes(ChunkStore *cs);
extern size_t dirstore_file_count(ChunkStore *cs);
extern void dirstore_set_state(ChunkStore *cs, const PHash *state);
extern bool dirstore_get_state(ChunkStore *cs, PHash *state);
extern void dirstore_set_failpoint(ChunkStore *cs, int failpoint);
extern void dirstore_set_file_target(ChunkStore *cs, size_t bytes);
extern void dirstore_crash(ChunkStore *cs);
extern bool dirstore_verify(ChunkStore *cs, PHash *bad);

#endif							/* CHUNKSTORE_H */
