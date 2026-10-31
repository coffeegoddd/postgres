/*
 * packstore.h
 *	  A ChunkStore on a plain-storage remote (remote.h): chunks in immutable
 *	  packs, a manifest naming the packs and the repo's state, replaced by
 *	  compare-and-swap.  All the logic is the client's, as with Dolt's file
 *	  and S3 remotes.
 *
 * A pack is one push's new chunks, in the order they were put (a chunk's
 * children before it), each zstd-compressed if PACK_ZSTD names a level and
 * that makes it smaller, then an index of (hash, offset, stored length,
 * length) entries.  The manifest is text:
 *
 *	prolly-remote 2
 *	state <hex>
 *	pack <name> <chunks> <data bytes> <index offset> <index bytes>
 *	...
 *
 * Opening reads the manifest and every pack's index (one ranged GET each, in
 * parallel): what has() answers from.  get() reads one chunk with a ranged
 * GET; get_many() coalesces ranges of the same pack and reads them in
 * parallel.  put() keeps a chunk pending; packstore_flush() uploads the
 * pending chunks as one pack and swaps the manifest, failing if another
 * writer swapped it first.  Chunks in packs are held by the remote for good
 * (a remote here is never collected), so nothing the remote has is freed.
 */
#ifndef PACKSTORE_H
#define PACKSTORE_H

#include "../chunkstore.h"
#include "remote.h"

typedef struct PackStoreStats
{
	uint64_t	packs;			/* in the manifest when opened */
	uint64_t	index_bytes;	/* read to open */
	double		open_secs;
	uint64_t	chunk_gets;		/* chunks read from packs */
	uint64_t	range_gets;		/* ranges requested for them */
	uint64_t	packs_written;
	uint64_t	pack_bytes_written;
	uint64_t	raw_bytes_written;	/* the chunks' own bytes, before compression */
	double		pack_build_secs;	/* building packs, upload excluded */
} PackStoreStats;

/* Opens the remote's store; *state the repo state its manifest names (zero if none). */
extern ChunkStore *packstore_open(Remote *r, PHash *state, char *err, size_t errlen);
extern PackStoreStats *packstore_stats(ChunkStore *cs);
extern Remote *packstore_remote(ChunkStore *cs);

/*
 * Uploads the pending chunks as one pack and swaps the manifest to name it
 * and 'state': 0, 1 if another writer swapped the manifest first (nothing
 * named; the pack stays as garbage), -1 on error.
 */
extern int	packstore_flush(ChunkStore *cs, const PHash *state);

/*
 * Reads the manifest again, and the indexes of the packs added since this
 * store last read it (packs never change, so the indexes read stay good):
 * *state the repo state it names.  Fails if the packs it read are no
 * longer a prefix of the manifest's (the remote was rewritten).
 */
extern bool packstore_refresh(ChunkStore *cs, PHash *state);

/* Calls fn for every chunk the remote has, pack by pack, in put order. */
typedef void (*PackChunkFn) (void *arg, const PHash *h, const uint8_t *data, size_t len);
extern bool packstore_stream(ChunkStore *cs, PackChunkFn fn, void *arg);

#endif							/* PACKSTORE_H */
