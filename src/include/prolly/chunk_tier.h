/*-------------------------------------------------------------------------
 *
 * chunk_tier.h
 *	  The history store's tier: a chunk log whose bytes are in pack objects
 *	  off the server (prolly/object_store.h), its index on the server.
 *
 * A tier log is a relation of the prolly_tier access method: its pages are
 * the catalog of its packs, a pack an immutable object of chunks' bytes one
 * after another.  The index's entries (prolly/chunk_index.h) are as the
 * pages store's, a chunk's place being its pack's number (the high 32
 * bits) and where in the pack its bytes begin (the low): so counts, holds
 * and frees are transactional, WAL-logged and on hot standbys as there.
 *
 * A transaction's puts gather their chunks into a pack of the backend's, a
 * number the catalog gives it (its entry the transaction's, WAL-logged),
 * written whole to the object store as the transaction commits, before the
 * commit record, and its size then recorded; a pack grown to
 * TIER_PACK_BYTES is written then, and another begun.  A pack whose
 * transaction aborted, or a crash cut short, is no committed entry's: VACUUM
 * removes its object, if one was written.  A pack all of whose bytes are
 * dead (its chunks' entries out) is removed too.
 *
 * The objects are where history_tier_store (an implementation, "directory")
 * at history_tier_location keeps them, the names "packs/" and the log's
 * stamp and the pack's number.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/chunk_tier.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_CHUNK_TIER_H
#define PROLLY_CHUNK_TIER_H

#include "prolly/chunk_log.h"
#include "storage/smgr.h"

/* The bytes a pack has at most, but for one chunk longer, alone in its own. */
#define TIER_PACK_BYTES		((uint64) 64 * 1024 * 1024)

extern PGDLLIMPORT char *history_tier_store;
extern PGDLLIMPORT char *history_tier_location;

extern PGDLLIMPORT const ChunkLogRoutine TierChunkLog;

/* For the access method: a new tier log's first page, written to its storage. */
extern void ChunkTierInitStorage(SMgrRelation srel, bool use_wal);

#endif							/* PROLLY_CHUNK_TIER_H */
