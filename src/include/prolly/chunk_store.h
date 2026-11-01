/*-------------------------------------------------------------------------
 *
 * chunk_store.h
 *	  The history store's chunks, from C: has, get and put over a chunk log
 *	  (prolly/chunk_log.h) and its index (prolly/chunk_index.h), with no SQL
 *	  and no executor.
 *
 * A chunk is put by appending its bytes to the log and inserting its
 * entry, with no references, into the index; one already there is not put
 * again.  Two backends putting one chunk at once both append it: the
 * entry of one is the chunk's, and the other's bytes are unused.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/chunk_store.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_CHUNK_STORE_H
#define PROLLY_CHUNK_STORE_H

#include "prolly/chunk_index.h"

/*
 * A chunk's name: the first CHUNK_HASH_LEN bytes of the SHA-256 of its
 * bytes.  (It needs a resource owner, as pg_cryptohash does: a caller
 * outside a transaction makes one.)
 */
extern void ChunkHash(const void *data, size_t len, uint8 *out);

/*
 * The relations of this database's history store, from initdb:
 * pg_history.chunks, pg_history.chunk_index and pg_history.pointers.
 */
extern Oid	ChunkStoreLogRelid(void);
extern Oid	ChunkStoreIndexRelid(void);
extern Oid	ChunkStorePointersRelid(void);

/* Whether the store has the chunk named hash. */
extern bool ChunkStoreHas(Relation log, Relation idx, const uint8 *hash);

/*
 * The chunk's bytes, palloc'd, and their length in *len; NULL if absent.
 * Bytes that don't hash to its name are an error.
 */
extern void *ChunkStoreGet(Relation log, Relation idx, const uint8 *hash,
						   uint32 *len);

/*
 * Stores the chunk unless the store has it; true if it stored it.  A hash
 * that isn't the bytes' is an error, as is a strong reference (as
 * ChunkFormat finds them) to a chunk the store doesn't have.  Storing it
 * counts a reference of each chunk it references, in this transaction.
 * (A chunk another transaction is putting is not the store's until it
 * commits: a chunk referencing it is refused meanwhile.)
 */
extern bool ChunkStorePut(Relation log, Relation idx, const uint8 *hash,
						  const void *data, uint32 len);

/*
 * A root of the chunk (a hold from outside the store: a branch's head, a
 * working set), taken or let go of in this transaction, its counts after
 * in *out.  Letting go of one it hasn't is an error.
 */
extern void ChunkStoreRefRoot(Relation idx, const uint8 *hash,
							  ChunkIndexEntry *out);
extern void ChunkStoreUnrefRoot(Relation idx, const uint8 *hash,
								ChunkIndexEntry *out);

/*
 * Frees the chunks of hashes[0..n) that nothing holds (no references, no
 * roots, no pins, no other transaction changing their counts), and those
 * their references alone held, and so on; answers how many it freed.  The
 * freeing is this transaction's: the chunks are gone as it commits, and
 * nothing is if it aborts.  Time is the chunks freed's.
 */
extern uint64 ChunkStoreFree(Relation log, Relation idx, const uint8 *hashes,
							 int n);

/* ChunkStoreFree of every chunk nothing holds (O(store): for tests). */
extern uint64 ChunkStoreFreeUnheld(Relation log, Relation idx);

/*
 * The audit: every chunk's bytes are its own, every chunk it references is
 * in the store, and every chunk's count of references is the edges to it.
 * The chunks, and those nothing holds, in *chunks and *unheld; the first
 * fault in *why.  It locks the index in ShareLock, so that nothing
 * writes it meanwhile.  For tests; O(store).
 */
extern bool ChunkStoreCheck(Relation log, Relation idx, uint64 *chunks,
							uint64 *unheld, char **why);

#endif							/* PROLLY_CHUNK_STORE_H */
