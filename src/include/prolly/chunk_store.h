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

/* A chunk's name, in hex, palloc'd: for messages. */
extern char *ChunkHashText(const uint8 *hash);

/* Whether the store has the chunk named hash. */
extern bool ChunkStoreHas(Relation log, Relation idx, const uint8 *hash);

/*
 * The chunk's bytes, palloc'd, and their length in *len; NULL if absent.
 * They are not checked against its name (a page's checksum covers the
 * read; the audit, and puts, check names).
 */
extern void *ChunkStoreGet(Relation log, Relation idx, const uint8 *hash,
						   uint32 *len);

/*
 * Many chunks' bytes at once: out[i] (palloc'd) and lens[i] for hashes[i],
 * NULL for one absent.  Read in the log's order, whatever the order asked
 * (a tree's level read at once reads its pages through once); a name asked
 * twice is read twice.
 */
extern void ChunkStoreGetMany(Relation log, Relation idx, const uint8 *hashes,
							  int n, void **out, uint32 *lens);

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
 * Whether the store has the chunk, held by this transaction until it ends
 * if so, as a put that finds it holds it (prolly/chunk_pin.h).
 */
extern bool ChunkStoreHold(Relation idx, const uint8 *hash);

/*
 * ChunkStorePut of the chunk the bytes are, named in *hash (hashed here,
 * once).
 */
extern bool ChunkStorePutData(Relation log, Relation idx, const void *data,
							  uint32 len, uint8 *hash);

/*
 * A pack's chunks put at once: hashes[i]'s bytes datas[i], lens[i] long.
 * Each is put as ChunkStorePut puts it, but their bytes are appended to
 * the log together, in one pass, and a chunk may reference another of the
 * pack, before or after it (a pack's order is its own).  Chunks the store
 * has, and those named twice, are put once.  How many it stored.
 *
 * Its entries go in first, in their names' order, each reserving its
 * bytes' place in the log as it is put, the bytes written after: a pack
 * failing (an error, a FATAL, a crash) leaves no byte that no entry names.
 */
extern uint64 ChunkStorePutMany(Relation log, Relation idx, int n,
								const uint8 *hashes, const void *const *datas,
								const uint32 *lens);

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
 * nothing is if it aborts.  Time is the chunks freed's.  A chunk whose last
 * references two transactions let go of at once is freed by neither (each
 * sees the other's reference left): VACUUM's freer finds it.  So is one
 * being moved by compaction as it is to be freed (its entry being freed by
 * the move's transaction), and the chunks it alone holds.
 */
extern uint64 ChunkStoreFree(Relation log, Relation idx, const uint8 *hashes,
							 int n);

/* ChunkStoreFree of every chunk nothing holds (O(store): for tests). */
extern uint64 ChunkStoreFreeUnheld(Relation log, Relation idx);

/*
 * Compaction: the chunks with bytes on segments of the log given (from
 * prolly/chunk_log.h's compactable or evacuable), moved:
 * their bytes read, checked, and put elsewhere in the log, their entries
 * moved (prolly/chunk_index.h's move).  The old bytes are dead once VACUUM
 * takes the old entries out, after the move commits and no snapshot might
 * read them: the segments, then free, are used again, or (at the end) cut
 * off.
 *
 * Begin finds the chunks, in one visit of the index (O(store)), kept in
 * cxt; each step then moves the next max of them, in log order, in the
 * transaction it is in (a chunk a transaction holds or changes is passed
 * by), into this backend's region, a segment free, or (may_grow) pages
 * new; with below (not 0) into room wholly before it alone.  A step
 * answers the chunks it looked at, none if all are, or -1 if there is no
 * room for the next (a chunk longer than a segment is passed by then);
 * the chunks it moved and their bytes in *chunks and *bytes.
 */
typedef struct ChunkCompaction ChunkCompaction;

extern ChunkCompaction *ChunkStoreCompactBegin(Relation log, Relation idx,
											   int n, const uint64 *starts,
											   const uint64 *ends,
											   MemoryContext cxt);
extern int	ChunkStoreCompactStep(Relation log, Relation idx,
								  ChunkCompaction *c, int max, uint64 below,
								  bool may_grow, uint64 *chunks,
								  uint64 *bytes);
extern void ChunkStoreCompactEnd(ChunkCompaction *c);

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
