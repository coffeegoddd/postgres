/*-------------------------------------------------------------------------
 *
 * chunk_freer.h
 *	  Freeing the history store's chunks in the background.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/chunk_freer.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_CHUNK_FREER_H
#define PROLLY_CHUNK_FREER_H

#include "postmaster/bgworker.h"
#include "storage/block.h"

extern PGDLLIMPORT bool chunk_free_in_background;
extern PGDLLIMPORT double chunk_compact_dead_fraction;

/*
 * Starts a chunk freer for the chunk index idx of the chunk log log, in
 * this database, which frees the chunks nothing holds, then (if compact)
 * compacts the log's segments fraction of whose bytes are dead, the log
 * grown by grow pages at most, and (if evacuate) moves the live chunks of
 * its last segments into the free ones below them; false if it didn't
 * (turned off, or no worker free).
 */
extern bool ChunkFreerLaunch(Oid log, Oid idx, bool compact, bool evacuate,
							 double fraction, BlockNumber grow);

/* The chunk freer's main function. */
pg_noreturn extern void ChunkFreerMain(Datum main_arg);

#endif							/* PROLLY_CHUNK_FREER_H */
