/*-------------------------------------------------------------------------
 *
 * chunk_lane.h
 *	  The lanes of a chunk log: relations of their own, each appended to by
 *	  one live backend at a time.
 *
 * A chunk log (prolly/chunk_log.h) is a set of lanes: the log's own
 * relation, lane 0, and relations of the same access method beside it,
 * lane n named "<log>_lane_<n>" in its namespace, each internally
 * dependent on it (dropped with it, not alone).  Each lane is a whole log
 * of pages: its meta page, its regions, segments and free space.  A
 * backend appends only into the lane it holds, so that only it ever
 * extends that lane's relation: Postgres takes a relation's extension lock
 * per relation, and so no backend waits for another to grow the log.
 *
 * Which backend holds which lane is in shared memory, as each backend's
 * own entries: a backend appending to a log for the first time takes a
 * lane no live backend holds, or, if every lane is held, makes the next,
 * in its own transaction (gone with it if that aborts, and the entry with
 * it).  A backend holds its lane until it exits (a crash lets go of them
 * all), so a log has as many lanes as it has had writers at once.  A
 * place in the log carries its lane in its top LANE_BITS bits.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/chunk_lane.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_CHUNK_LANE_H
#define PROLLY_CHUNK_LANE_H

#include "storage/lockdefs.h"
#include "storage/subsystems.h"
#include "utils/relcache.h"

/* A place in the log: its lane, and where in the lane's own pages. */
#define LANE_BITS		16
#define LANE_SHIFT		(64 - LANE_BITS)
#define LANE_MAX		((1 << LANE_BITS) - 1)
#define LANE_OF(loc)	((uint32) ((loc) >> LANE_SHIFT))
#define LANE_PLACE(loc)	((loc) & ((UINT64CONST(1) << LANE_SHIFT) - 1))
#define LANE_LOC(lane, place)	(((uint64) (lane) << LANE_SHIFT) | (place))

extern PGDLLIMPORT const ShmemCallbacks ChunkLaneShmemCallbacks;

/*
 * The lane of log this backend appends to, its number in *lane, opened in
 * RowExclusiveLock: the one it holds, or one taken (no live backend
 * holding it), or one made.  log itself for lane 0 (not to be closed).
 */
extern Relation ChunkLaneForWrite(Relation log, uint32 *lane);

/*
 * Lane lane of log opened in lockmode (log itself, lane 0); NULL if it has
 * none such (yet, as this transaction sees).
 */
extern Relation ChunkLaneOpen(Relation log, uint32 lane, LOCKMODE lockmode);

/* A lane ChunkLaneOpen or ChunkLaneForWrite gave, closed (its lock kept). */
extern void ChunkLaneClose(Relation log, Relation lane);

/* How many lanes log has, lane 0 among them: lanes 0 .. n - 1. */
extern uint32 ChunkLaneCount(Relation log);

#endif							/* PROLLY_CHUNK_LANE_H */
