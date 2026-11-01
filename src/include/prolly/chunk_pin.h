/*-------------------------------------------------------------------------
 *
 * chunk_pin.h
 *	  Transient holds on the history store's chunks, for this transaction's
 *	  own freeing.
 *
 * A transaction holds each chunk it puts until it ends (or at least until
 * the subtransaction that put it is rolled back; its pin stays): one it
 * stores, by its entry (not committed: no other's freer frees it, and its
 * own freeing passes by); one it finds stored already, by a change of the
 * chunk's counts adding nothing (running: no other's freer frees the chunk
 * while it does, and one freeing it as it is found is waited for), and by
 * a pin; one of a pack found stored that another of the pack found
 * stored, and held, references, by that one's references.
 * A pin is the transaction's own note that it holds the chunk, so that its
 * own freeing, which counts its own changes as made, passes the chunk by.
 * Pins are this backend's alone, in its memory, let go of as its
 * transaction ends; none is shared, and there is no limit to them.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/chunk_pin.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_CHUNK_PIN_H
#define PROLLY_CHUNK_PIN_H

/* Notes that this transaction holds the chunk named hash, until it ends. */
extern void ChunkPin(const uint8 *hash);

/* Whether this transaction holds the chunk (for its own freeing). */
extern bool ChunkPinned(const uint8 *hash);

#endif							/* PROLLY_CHUNK_PIN_H */
