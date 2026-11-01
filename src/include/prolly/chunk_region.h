/*-------------------------------------------------------------------------
 *
 * chunk_region.h
 *	  The regions of the history store's chunk logs that live backends
 *	  hold.
 *
 * A backend appends into a region of a log's pages of its own (prolly/
 * chunk_log.h), its rest let go of (sealed: counted dead) when it takes
 * another.  A backend that exits, or crashes, never lets go of its last:
 * VACUUM does, sealing the pages no live backend's region has.  Each
 * backend publishes, in shared memory, the region it holds in each log;
 * its own alone writes them, and they are gone when it exits (a crash
 * takes them all).  A backend holding regions in more logs than it has
 * room for counts as holding every page.
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

/* The regions live in one log at one moment. */
typedef struct ChunkRegionSet
{
	bool		all;			/* a backend holding every page */
	int			n;
	BlockNumber *first;
	BlockNumber *end;
} ChunkRegionSet;

/* This backend's region in the log: pages first..end-1, its last's replaced. */
extern void ChunkRegionPublish(RelFileLocator log, BlockNumber first, BlockNumber end);

/* The regions live in the log now. */
extern ChunkRegionSet *ChunkRegionLive(RelFileLocator log);

/* Whether a live backend's region in the log has page blk, or one in also. */
extern bool ChunkRegionHeld(RelFileLocator log, BlockNumber blk,
							const ChunkRegionSet *also);

#endif							/* PROLLY_CHUNK_REGION_H */
