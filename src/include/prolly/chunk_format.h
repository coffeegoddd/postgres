/*-------------------------------------------------------------------------
 *
 * chunk_format.h
 *	  What the history store needs to know of a chunk's bytes: the chunks it
 *	  references.
 *
 * A ChunkFormatRoutine lists a chunk's strong references, the chunks it
 * names that must be in the store while it is.  The store counts them as
 * a chunk is put and as it is freed, and its audit checks them.  The
 * version-control engine's format is one; LeafChunkFormat, which finds no
 * references in any chunk, is the default.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/chunk_format.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_CHUNK_FORMAT_H
#define PROLLY_CHUNK_FORMAT_H

typedef void (*ChunkRefFn) (void *arg, const uint8 *hash);

typedef struct ChunkFormatRoutine
{
	/*
	 * Calls fn for each chunk the len bytes at data strongly reference, in
	 * order, once an edge (a chunk named twice is two edges).  Bytes that
	 * are no chunk of the format are an error.
	 */
	void		(*refs) (const void *data, size_t len, ChunkRefFn fn, void *arg);
} ChunkFormatRoutine;

extern PGDLLIMPORT const ChunkFormatRoutine *ChunkFormat;
extern PGDLLIMPORT const ChunkFormatRoutine LeafChunkFormat;

#endif							/* PROLLY_CHUNK_FORMAT_H */
