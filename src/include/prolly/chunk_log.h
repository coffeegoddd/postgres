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
 * special space marking it as a page of this log, which a read checks.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/chunk_log.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_CHUNK_LOG_H
#define PROLLY_CHUNK_LOG_H

#include "utils/relcache.h"

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
	 * Reads len bytes of rel's log beginning at loc into dst.  An error if
	 * they were not all written, or a page they are on is not one of the
	 * log's.
	 */
	void		(*read) (Relation rel, uint64 loc, void *dst, size_t len);

	/* Where the log's regions end: no bytes are at or after it. */
	uint64		(*end) (Relation rel);
} ChunkLogRoutine;

extern PGDLLIMPORT const ChunkLogRoutine *ChunkLog;
extern PGDLLIMPORT const ChunkLogRoutine PageChunkLog;

/* The prolly_chunks table access method's relations are a PageChunkLog's. */
extern bool RelationIsChunkLog(Relation rel);

#endif							/* PROLLY_CHUNK_LOG_H */
