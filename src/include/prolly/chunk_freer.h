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

extern PGDLLIMPORT bool chunk_free_in_background;

/*
 * Starts a chunk freer for the chunk index idx of the chunk log log, in
 * this database; false if it didn't (turned off, or no worker free).
 */
extern bool ChunkFreerLaunch(Oid log, Oid idx);

/* The chunk freer's main function. */
pg_noreturn extern void ChunkFreerMain(Datum main_arg);

#endif							/* PROLLY_CHUNK_FREER_H */
