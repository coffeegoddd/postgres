/*-------------------------------------------------------------------------
 *
 * chunk_region.h
 *	  The regions of the history store's chunk logs that live backends
 *	  hold.
 *
 * A backend appends into a region of a log's pages of its own (prolly/
 * chunk_log.h), its rest let go of (sealed: counted dead) when it takes
 * another.  A backend that exits, or crashes, or forgets its region (to an
 * invalidation of its relcache entry), never lets go of it: VACUUM does,
 * sealing the pages no live backend's region has.  Each backend publishes,
 * in shared memory, the region it holds in each log, with the log's
 * generation then (one a tail cut has passed holds nothing: its backend
 * forgot it); its own alone writes them.  An entry goes as the backend
 * exits (a crash takes them all), as its transaction ends once it has
 * forgotten the region, or to make room for another log's; a backend
 * appends only in the region its entry names, which it checks before each
 * append.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/chunk_region.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_CHUNK_REGION_H
#define PROLLY_CHUNK_REGION_H

#include "storage/block.h"
#include "storage/relfilelocator.h"
#include "storage/subsystems.h"

extern PGDLLIMPORT const ShmemCallbacks ChunkRegionShmemCallbacks;

/* The regions live in one log at one moment, by their first pages. */
typedef struct ChunkRegionSet
{
	int			n;
	BlockNumber *first;
	BlockNumber *end;			/* ... and the furthest end of those before */
} ChunkRegionSet;

/*
 * This backend's region in the log (relation relid): pages first..end-1,
 * of the log's generation; its last there replaced.
 */
extern void ChunkRegionPublish(RelFileLocator log, Oid relid, uint32 generation,
							   BlockNumber first, BlockNumber end);

/* Whether this backend's entry for the log is its region ending at end. */
extern bool ChunkRegionMine(RelFileLocator log, BlockNumber end);

/* Whether this backend is between a region's prepare and its release. */
extern void ChunkRegionAppending(bool on);

/* This backend keeps its region in the log (in its relcache entry) again. */
extern void ChunkRegionKept(RelFileLocator log);

/* The regions live in the log now, of its generation. */
extern ChunkRegionSet *ChunkRegionLive(RelFileLocator log, uint32 generation);

/* Whether a region of the set has page blk. */
extern bool ChunkRegionSetHas(const ChunkRegionSet *set, BlockNumber blk);


#endif							/* PROLLY_CHUNK_REGION_H */
