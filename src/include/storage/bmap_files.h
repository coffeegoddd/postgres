/*-------------------------------------------------------------------------
 *
 * bmap_files.h
 *	  The files of the block-mapped storage manager, for the backend and for
 *	  tools that read a data directory.
 *
 * A block-mapped relation's main fork has no file of its own: its pages are
 * in the page store of its tablespace and database, and its map, from
 * logical to physical block, is its map fork (BMAP_FORKNUM, "<relfilenumber>
 * _bmap"), an ordinary md.c file whose pages are WAL-logged.  The map
 * fork's existence is the relation's.  Per tablespace and database:
 *
 *	BMAP_ALLOC_RELNUMBER	the allocator: a metapage and bitmap pages saying
 *							which physical blocks of the page store are used
 *	BMAP_STORE_RELNUMBER	the page store.  Its pages carry checksums computed
 *							with their logical block numbers, so tools that
 *							verify checksums from files skip it
 *	BMAP_REFS_RELNUMBER		reference counts: for each physical block, how many
 *							maps hold it besides the first
 *
 * The three relation file numbers are reserved: below FirstGenbkiObjectId and
 * used by no catalog object.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/bmap_files.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BMAP_FILES_H
#define BMAP_FILES_H

#define BMAP_ALLOC_RELNUMBER	9420
#define BMAP_STORE_RELNUMBER	9421
#define BMAP_REFS_RELNUMBER		9419

#endif							/* BMAP_FILES_H */
