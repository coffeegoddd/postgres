/*-------------------------------------------------------------------------
 *
 * chunk_format.h
 *	  What the history store needs to know of a chunk's bytes: the chunks it
 *	  references.
 *
 * A ChunkFormatRoutine lists a chunk's strong references, the chunks it
 * names that must be in the store while it is.  The store counts them as
 * a chunk is put and as it is freed, and its audit checks them.
 *
 * Each chunk index records the format of the chunks it names as it is
 * made (chunk_format, by name: "engine", the version-control engine's,
 * unless set; "leaf", which finds no references in any chunk; or one a
 * module registered), keeps it through TRUNCATE, and the store reads
 * every chunk it names by it, whoever asks: no session or worker can read
 * an index's chunks by another.  A memory store, with no index, reads by
 * the session's (ChunkFormat, the leaf format unless a test sets another).
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

/* (a format's name: what an index records) */
#define CHUNK_FORMAT_NAMELEN	16

typedef struct ChunkFormatRoutine
{
	/* Its name, shorter than CHUNK_FORMAT_NAMELEN. */
	const char *name;

	/*
	 * Calls fn for each chunk the len bytes at data strongly reference, in
	 * order, once an edge (a chunk named twice is two edges).  Bytes that
	 * are no chunk of the format are an error.
	 */
	void		(*refs) (const void *data, size_t len, ChunkRefFn fn, void *arg);
} ChunkFormatRoutine;

/* The session's: a memory store's (an index's is its own). */
extern PGDLLIMPORT const ChunkFormatRoutine *ChunkFormat;

/* The format a chunk index made now records (the setting chunk_format). */
extern PGDLLIMPORT char *chunk_format_name;

extern PGDLLIMPORT const ChunkFormatRoutine LeafChunkFormat;

/*
 * A format's registration, by its name (a module's, as it loads: the
 * built-in "leaf" and "engine" are there), and the format a name names:
 * NULL if none (Find), an error (Named).
 */
extern void ChunkFormatRegister(const ChunkFormatRoutine *format);
extern const ChunkFormatRoutine *ChunkFormatFind(const char *name);
extern const ChunkFormatRoutine *ChunkFormatNamed(const char *name);

#endif							/* PROLLY_CHUNK_FORMAT_H */
