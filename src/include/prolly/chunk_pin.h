/*-------------------------------------------------------------------------
 *
 * chunk_pin.h
 *	  Transient holds on the history store's chunks: pins.
 *
 * A transaction pins each chunk it stores or finds already stored, until it
 * ends: the chunk is then held by what the transaction made of it (a
 * reference from a chunk it stored, a root it took) or freeing may have it.
 * A pin closes the race in which a chunk nothing holds is freed just as a
 * writer is about to use it: the writer pins, then looks the chunk up; the
 * freer marks the chunk being freed, then looks for pins, and lets it be if
 * it finds one.
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
