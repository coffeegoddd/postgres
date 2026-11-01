/*-------------------------------------------------------------------------
 *
 * chunk_log.h
 *	  The history store's bytes: an append-only log of chunk bytes in a
 *	  relation of its own.
 *
 * Every tree node, schema and commit of the version-control history is an
 * immutable chunk, named by the hash of its bytes.  A ChunkLogRoutine keeps
 * those bytes: append writes bytes at the log's end and answers where they
 * begin, read reads bytes back from where they begin, and end answers the
 * log's length.  What names a chunk, and where it is in the log, is kept
 * elsewhere (an index from hash to place); the log knows only bytes.
 *
 * PageChunkLog keeps the log in the pages of a relation of the
 * prolly_chunks table access method, through the buffer manager and the
 * WAL, so that checkpoints, crash recovery, standbys and base backups take
 * the history with the rest of the database.  Its layout:
 *
 *	block 0		the meta page: the log's format version and its end
 *	block 1..	the log's bytes, in order, after each page's header; a
 *				chunk's bytes may continue on the next page
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
	 * Writes len bytes at the end of rel's log and returns where they
	 * begin.  The caller holds rel in ExclusiveLock (one writer at a time;
	 * readers are not blocked).  The bytes are durable once the appending
	 * transaction commits.
	 */
	uint64		(*append) (Relation rel, const void *data, size_t len);

	/*
	 * Reads len bytes of rel's log beginning at loc into dst.  An error if
	 * they are not all before the log's end, or a page they are on is not
	 * one of the log's.
	 */
	void		(*read) (Relation rel, uint64 loc, void *dst, size_t len);

	/* The log's length: where the next append begins. */
	uint64		(*end) (Relation rel);
} ChunkLogRoutine;

extern PGDLLIMPORT const ChunkLogRoutine *ChunkLog;
extern PGDLLIMPORT const ChunkLogRoutine PageChunkLog;

/* The prolly_chunks table access method's relations are a PageChunkLog's. */
extern bool RelationIsChunkLog(Relation rel);

#endif							/* PROLLY_CHUNK_LOG_H */
