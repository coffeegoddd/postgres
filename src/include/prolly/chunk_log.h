/*-------------------------------------------------------------------------
 *
 * chunk_log.h
 *	  The history store's bytes: an append-only log of chunk bytes in a
 *	  relation of its own.
 *
 * Every tree node, schema and commit of the version-control history is an
 * immutable chunk, named by the hash of its bytes.  A ChunkLogRoutine keeps
 * those bytes: append writes bytes into the log and answers where they
 * begin, read reads bytes back from where they begin, and end answers
 * where the log's space ends.  What names a chunk, and where it is in the log, is kept
 * elsewhere (an index from hash to place); the log knows only bytes.
 *
 * PageChunkLog keeps the log in the pages of a relation of the
 * prolly_chunks table access method, through the buffer manager and the
 * WAL, so that checkpoints, crash recovery, standbys and base backups take
 * the history with the rest of the database.  Its layout:
 *
 *	block 0		the meta page: the log's format version, and the first
 *				page no region has
 *	block 1..	regions of pages, each a backend's to append into: bytes
 *				after each page's header, an append's bytes running on to
 *				the region's next page
 *
 * Many backends append at once, each into a region of its own, which it
 * claims from the meta page (the only place appends meet, once a region)
 * and fills in order; what a backend leaves of a region is unused.  A
 * place in the log is (page - 1) * the bytes a page holds + where on the
 * page, so an append's bytes are at consecutive places.
 *
 * Every page keeps the standard page layout (a page header, pd_lower at the
 * end of the bytes, the unused rest a hole) with a ChunkPageOpaque in its
 * special space marking it as a page of this log, which a read checks, and
 * counting the page's bytes no chunk's any more.  The log's space is
 * handed out, taken back and compacted a segment at a time (32 data pages,
 * aligned): a region is whole segments, a claim at the end is too, and a
 * segment every page of which is written full and all dead goes in the
 * relation's free space map (its first page), which an append that needs
 * room takes from before claiming new pages.  (A page a region did not
 * fill stays: its backend may go on appending to it.)
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/chunk_log.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_CHUNK_LOG_H
#define PROLLY_CHUNK_LOG_H

#include "storage/relfilelocator.h"
#include "utils/relcache.h"

/*
 * Room in a backend's region of a log, prepared for a write: bytes are
 * reserved from next, before limit.  The caller's, for the time of one
 * write (a chunk's, or a pack's).
 */
typedef struct ChunkLogRun
{
	RelFileNumber relnumber;
	uint64		next;
	uint64		limit;
	uint32		generation;		/* the log's, the bytes' (mark_dead's) */
} ChunkLogRun;

typedef struct ChunkLogSegments ChunkLogSegments;

typedef struct ChunkLogRoutine
{
	/*
	 * Writes len bytes into this backend's region of rel's log and returns
	 * where they begin.  The caller holds rel in RowExclusiveLock: other
	 * backends append at once, into regions of their own, and readers wait
	 * for at most a page's change.  The bytes are durable
	 * once the appending transaction commits, but not transactional:
	 * readers see them from when each page of them is written, and they
	 * stay if the transaction aborts.  A crash during an append can leave a
	 * part of its bytes written.
	 * What names chunks (the index) is what is transactional.
	 */
	uint64		(*append) (Relation rel, const void *data, size_t len);

	/*
	 * append in its steps, for a caller naming its bytes before they are
	 * written (an index's entries): prepare makes room for len bytes in this
	 * backend's region (it may fail); reserve takes len of that room and
	 * answers where they begin, changing nothing but memory (it does not
	 * fail); write writes bytes reserved; release keeps the region for the
	 * next.  Bytes reserved and never written (the write failing, or never
	 * made) are the reserving entry's, which counts them dead (mark_dead
	 * takes them, written or not); the region goes on after them.
	 */
	void		(*prepare) (Relation rel, size_t len, ChunkLogRun *run);
	uint64		(*reserve) (Relation rel, ChunkLogRun *run, size_t len);
	void		(*write) (Relation rel, uint64 loc, const void *data, size_t len);
	void		(*release) (Relation rel, const ChunkLogRun *run);

	/*
	 * The run prepared, grown in place over the unused segments after it
	 * until it has room for len bytes; false if it has not (grown as far
	 * as they went).
	 * Nothing of the run is let go of either way: bytes reserved in it and
	 * not yet written may stay so (a pack's, written after).
	 */
	bool		(*extend) (Relation rel, ChunkLogRun *run, size_t len);

	/*
	 * prepare, but only in the room this backend's region has, or in
	 * segments free, and (below not 0) wholly before the place below:
	 * false, if it would have to grow the log, the run (for release) the
	 * region as it is, with too little room, or none.
	 */
	bool		(*prepare_free) (Relation rel, size_t len, uint64 below,
								 ChunkLogRun *run);

	/*
	 * For compaction (prolly/chunk_store.h's ChunkStoreCompactBegin), the
	 * log's space handed out and taken back a segment at a time: the
	 * segments to compact, up to max, ascending (done with, fraction of
	 * their bytes dead, not all), and the segments at the end to evacuate
	 * (their live chunks fit in the free segments below the lowest, and
	 * moved there, with prepare_free's below the lowest's start, leave the
	 * end free to cut): the places each begins and ends at, in starts and
	 * ends; how many.  O(log).  And a segment's bytes: a chunk longer goes
	 * on pages new alone.
	 */
	int			(*compactable) (Relation rel, double fraction, int max,
								uint64 *starts, uint64 *ends);
	int			(*evacuable) (Relation rel, int max, uint64 *starts,
							  uint64 *ends);
	uint64		(*segment_bytes) (Relation rel);

	/*
	 * What VACUUM would find of the log's segments (ChunkLogRecordFree),
	 * none passed by, the free space map left as it is.  For tests.
	 */
	void		(*find_segments) (Relation rel, double fraction,
								  ChunkLogSegments *out);

	/*
	 * For VACUUM of the log's index: its pass begun (what the log keeps for
	 * its end, or NULL), and ended: the log's own upkeep (letting go of
	 * what no live backend holds; with truncate, cutting off its free end),
	 * and what it finds of its segments in *out, those with bytes of chunks
	 * freed kept for snapshots (pending, a bit a segment, of the first
	 * npending) passed by.  Between them the pass counts bytes dead.
	 */
	void	   *(*vacuum_begin) (Relation rel);
	void		(*vacuum_end) (Relation rel, void *state, bool truncate,
							   double fraction, const uint8 *pending,
							   uint64 npending, ChunkLogSegments *out);

	/*
	 * For audits: whether no part of the log counts bytes of the chunks
	 * given (at locs, lens long) dead, and it has room for all it counts;
	 * what is wrong in *why if not.  O(log).
	 */
	bool		(*check_live) (Relation rel, int n, const uint64 *locs,
							   const uint32 *lens, char **why);

	/*
	 * Binds the log to the one chunk index naming its bytes (the first to
	 * ask), or errors if it is another's: a tail cut, by that index's
	 * VACUUM, needs every entry naming the log's bytes to have been seen.
	 */
	void		(*bind_index) (Relation rel, Oid index);

	/*
	 * Reads len bytes of rel's log beginning at loc into dst.  An error if
	 * they were not all written, or a page they are on is not one of the
	 * log's.
	 */
	void		(*read) (Relation rel, uint64 loc, void *dst, size_t len);

	/* Where the log's regions end: no bytes are at or after it. */
	uint64		(*end) (Relation rel);

	/*
	 * Counts len bytes from loc dead: no chunk's any more (written, or
	 * reserved and never written), of the log's generation given, the run's
	 * they were reserved in (a tail cut makes the next: bytes of the one
	 * before on pages it cut are gone).  Space all of whose bytes are dead
	 * is used again for appends.
	 */
	void		(*mark_dead) (Relation rel, uint64 loc, size_t len,
								  uint32 generation);

	/*
	 * The log's storage's stamp: made with it (and again by a truncation),
	 * kept by a move to another tablespace, so that an index of chunks in
	 * one log's storage knows another's.
	 */
	uint64		(*stamp) (Relation rel);
} ChunkLogRoutine;

/*
 * The routine of the chunk log rel: its access method's.  A store reaches
 * its log through it, so that its index may be bound to a log of any kind.
 */
extern const ChunkLogRoutine *ChunkLogOf(Relation rel);
extern PGDLLIMPORT const ChunkLogRoutine PageChunkLog;

/*
 * For tests: the log's pages written, those written full, its dead bytes,
 * and the pages free to use again.
 */
extern void ChunkLogSpace(Relation rel, uint32 *pages, uint32 *full,
						  uint64 *dead, uint32 *free_pages);

/*
 * For VACUUM of the log's index: lets go of the rest of every region no
 * live backend holds (one whose backend exited, crashed, or forgot it),
 * its pages found so as VACUUM's pass over the index begins, and not
 * changed by its end.
 */
typedef struct ChunkLogSweep ChunkLogSweep;

/* The log's generation: one more at each tail cut. */
extern uint32 ChunkLogGeneration(Relation rel);
extern ChunkLogSweep *ChunkLogSweepBegin(Relation rel);
extern void ChunkLogSweepEnd(Relation rel, ChunkLogSweep *sweep);

/*
 * The log's space is handed out and taken back a segment at a time (pages
 * one after another, aligned), and compacted so: what VACUUM finds of its
 * segments.
 */
struct ChunkLogSegments
{
	uint32		segments;		/* the log's */
	uint32		free;			/* every byte of every page dead */
	uint32		compactable;	/* written full, fraction of their bytes dead */
	uint64		compactable_dead;	/* their dead bytes */
	bool		worth;			/* those come to a segment's */
	uint32		evacuable;		/* at the end, their live chunks fitting in
								 * the free segments below */
	uint32		pending;		/* passed by: chunks freed on them kept for
								 * snapshots still */
	BlockNumber grow;			/* the pages compaction may grow the log by:
								 * a segment's if none is free */
};

/*
 * Puts every segment free in the free space map (O(log); VACUUM's), and
 * what it found of the segments in *out, those with bytes of chunks freed
 * kept for snapshots (pending, a bit a segment, of the first npending)
 * passed by: not counted to compact, evacuation's count stopping at them.
 * (A freer started for others finds its own, not knowing these: it moves
 * nothing off them, their chunks moved already or kept.)
 */
extern void ChunkLogRecordFree(Relation rel, double fraction,
							   const uint8 *pending, uint64 npending,
							   ChunkLogSegments *out);


/*
 * For audits: whether no page counts bytes of the chunks given (at locs,
 * lens long) dead, its live bytes and dead bytes no more than it has
 * written; what is wrong in *why if not.  O(log).
 */
extern bool ChunkLogCheckLive(Relation rel, int n, const uint64 *locs,
							  const uint32 *lens, char **why);

/*
 * Cuts the log's free tail off, if it gets the relation's exclusive lock
 * without waiting (VACUUM's); answers the pages cut.
 */
extern BlockNumber ChunkLogTruncateTail(Relation rel);

/* The prolly_chunks table access method's relations are a PageChunkLog's. */
extern bool RelationIsChunkLog(Relation rel);

/* A chunk log of either kind: the pages', or a tier's (prolly/chunk_tier.h). */
extern bool RelationIsTierLog(Relation rel);
static inline bool
RelationIsAnyChunkLog(Relation rel)
{
	return RelationIsChunkLog(rel) || RelationIsTierLog(rel);
}

#endif							/* PROLLY_CHUNK_LOG_H */
