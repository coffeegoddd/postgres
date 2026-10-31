/*
 * pgstore.h
 *	  The history store inside Postgres: chunks in pages of their own layout
 *	  (contrib/prolly_chunkpages) behind the ChunkStore interface, reached
 *	  over libpq.
 *
 * A store is a schema: its log of pages, the index from hash to place, the
 * durable roots and the repo state.  Like dirstore, chunk metadata lives in
 * memory and reference counts are rebuilt from the roots on open; a flush
 * sends what changed (new chunks, chunks let go, roots, state) in one
 * transaction, and invalidates bytes previously returned by get.  Chunks
 * stay cached across flushes up to a budget, least recently used let go.
 */
#ifndef PGSTORE_H
#define PGSTORE_H

#include "chunkstore.h"

/*
 * Opens the store in schema of the database conninfo names, made empty
 * first with fresh.  An ephemeral store's schema is dropped when the store
 * is destroyed.  NULL, with the reason in err, on failure.
 */
extern ChunkStore *pgstore_open(const char *conninfo, const char *schema, bool fresh,
								bool ephemeral, char *err, size_t errlen);
extern int	pgstore_flush(ChunkStore *cs);
extern void pgstore_set_state(ChunkStore *cs, const PHash *state);
extern bool pgstore_get_state(ChunkStore *cs, PHash *state);
/* Bytes the store's relations take on disk. */
extern uint64_t pgstore_disk_bytes(ChunkStore *cs);
/*
 * Bytes of flushed chunks kept cached across a flush (default 256 MB); 0
 * reads everything flushed from the server.
 */
extern void pgstore_set_cache(ChunkStore *cs, uint64_t bytes);
/* Reads served by the server since open (not from the store's cache). */
extern uint64_t pgstore_server_reads(ChunkStore *cs);
extern double pgstore_server_read_secs(ChunkStore *cs);
extern uint64_t pgstore_server_read_calls(ChunkStore *cs);

#endif							/* PGSTORE_H */
