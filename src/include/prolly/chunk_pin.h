/*-------------------------------------------------------------------------
 *
 * chunk_pin.h
 *	  Transient holds on the history store's chunks: pins.
 *
 * A transaction pins each chunk it finds stored already, until it ends (a
 * chunk it stores itself is held by it without a pin: its entry, until it
 * commits, is the transaction's, which no other's freer frees, and its own
 * freeing passes by): the chunk is then held by what
 * the transaction made of it (a reference from a chunk it stored, a root it
 * took) or freeing may have it.  A pin closes the race in which a chunk
 * nothing holds is freed just as a writer is about to use it: the writer
 * pins, then looks the chunk up (again, if it found it before pinning);
 * the freer, with the chunk's bucket locked,
 * looks for pins, and marks the chunk being freed only if it finds none, so
 * that a writer looking it up after waits for the freeing to end.  (A
 * chunk only looked up, not put, is not pinned.)
 *
 * Pins are in shared memory, a table per backend of the chunks' hashes (a
 * prefix of each), written only by its backend and read by freers; they
 * are never WAL-logged, and a backend's are let go of at its transaction's
 * end.  A backend whose table fills counts as pinning every chunk until
 * then.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/chunk_pin.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_CHUNK_PIN_H
#define PROLLY_CHUNK_PIN_H

/* Pins the chunk named hash until this transaction ends. */
extern void ChunkPin(const uint8 *hash);

/* Whether any backend has the chunk pinned (or might have). */
extern bool ChunkPinned(const uint8 *hash);

#endif							/* PROLLY_CHUNK_PIN_H */
