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
 * backend appends only into the lane it holds, so that no other writer
 * ever extends that lane's relation: Postgres takes a relation's extension
 * lock per relation, and so no writer waits for another to grow the log.
 * (VACUUM, counting bytes dead that were reserved and never written past a
 * lane's end, may make that lane's pages too.)
 *
 * Which backend holds which lane is in shared memory, as each backend's
 * own entries: a backend appending to a log for the first time takes a
 * lane no live backend holds, or, if every lane is held, makes the next,
 * in its own transaction (gone with it if that aborts, and the entry with
 * it).  A backend holds its lane until it exits (a crash lets go of them
 * all), or until it needs room for another log's (it holds lanes of a few
 * logs at once), so a log has about as many lanes as it has had writers
 * at once.  A
 * place in the log carries its lane in its top LANE_BITS bits.  A lane
 * whose making ended without it (aborted; a prepared making's number is
 * held, by a lock its transaction holds, until it ends) leaves a hole, its
 * number given again only once a
 * VACUUM pass of the log's index begun after the hole was seen has ended
 * (the entries its transaction left, naming the lane, taken out by it); a
 * log has at most LANE_MAX lanes.  Lanes are found by what depends on the
 * log, each lane's meta page naming its number and log, not by name.
 *
 * Each lane's free space is its own: a backend's appends reuse the
 * segments freed in its own lane, never another's (that lane's holder
 * alone may grow it).  Compaction moves chunks off any lane's tail into
 * the compacting backend's lane, so that the tails of lanes no one writes
 * to are cut off in turn.  Evacuation (moving a tail into room below it,
 * never growing anything) moves one lane's tail a pass, into free segments
 * of the evacuating backend's lane: a lane another holds, its tail behind
 * live chunks, waits for compaction, or for its holder to free room.  A lane is made in its log's tablespace as it is
 * then; moving the log's relation moves its own storage, not its lanes'.
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

/*
 * A chunk index's lanes: relations of its access method beside it, lane n
 * named "<index>_lane_<n>" and written by the backend holding lane n of
 * the index's log (lane 0 too, a relation of its own: the index itself is
 * written by its merger alone).  Lane lane of idx opened in lockmode, NULL
 * if it has none; the lane this backend writes, made if missing; how many
 * idx may have (its log's lanes); a lane closed (its lock kept).
 */
extern Relation ChunkIndexLaneOpen(Relation idx, uint32 lane, LOCKMODE lockmode);
extern Relation ChunkIndexLaneForWrite(Relation idx, Relation log, uint32 *lane);
extern uint32 ChunkIndexLaneCount(Relation idx);
extern void ChunkIndexLaneClose(Relation lane);

/*
 * Every lane of idx a reader is to look in, opened (a lane just made by a
 * live backend included, whose catalogs this one may not see yet): how
 * many, into *lanes; and those closed (their locks kept).
 */
extern int	ChunkIndexLanesOpen(Relation idx, LOCKMODE lockmode, Relation **lanes);
extern void ChunkIndexLanesClose(Relation *lanes, int n);

#endif							/* PROLLY_CHUNK_LANE_H */
