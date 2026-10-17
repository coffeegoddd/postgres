/*-------------------------------------------------------------------------
 *
 * prolly_chunker.h
 *	  Chunker interface: where node boundaries fall.
 *
 * The chunker sees the items of one tree level in key order and decides
 * where each node ends.  Its algorithm and parameters are part of the
 * canonical format: the same items must give the same boundaries in every
 * database, on every machine.  Which rule is best is still to be measured.
 *
 * Contract:
 * - Decisions depend only on the items fed since begin (and level), never
 *   on timing, memory or history.
 * - Forced split: because a node has a size limit, the chunker returns
 *   PROLLY_CHUNK_SPLIT_BEFORE when the item would not fit in capacity.  That
 *   too is deterministic from content.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_chunker.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_CHUNKER_H
#define PROLLY_CHUNKER_H

#include "access/prolly_registry.h"
#include "access/prolly_types.h"

typedef enum ProllyChunkDecision
{
	PROLLY_CHUNK_CONTINUE,		/* item joins the current node */
	PROLLY_CHUNK_SPLIT_AFTER,	/* item ends the current node */
	PROLLY_CHUNK_SPLIT_BEFORE,	/* item starts a new node (forced split);
								 * the chunker has already counted it there */
} ProllyChunkDecision;

typedef struct ProllyChunkerRoutine
{
	ProllyImplInfo info;

	/*
	 * Start chunking a level.  capacity is the space available for items in
	 * a node, as measured by the chunk format's item_space().
	 */
	ProllyChunker *(*begin) (uint16 level, Size capacity, MemoryContext mcxt);

	/* Feed the next item; item_space is the chunk format's item_space(). */
	ProllyChunkDecision (*add) (ProllyChunker *chunker, const ProllyKey *key,
								const ProllyValue *value, Size item_space);

	void		(*end) (ProllyChunker *chunker);
} ProllyChunkerRoutine;

#define PROLLY_CHUNKER_CALLBACK_NAMES "begin", "add", "end"
#define PROLLY_CHUNKER_FIRST_CALLBACK begin
#define PROLLY_CHUNKER_LAST_CALLBACK end

static inline const ProllyChunkerRoutine *
prolly_chunker_lookup(const char *name)
{
	return (const ProllyChunkerRoutine *)
		prolly_get_impl(PROLLY_IFACE_CHUNKER, name);
}

#endif							/* PROLLY_CHUNKER_H */
