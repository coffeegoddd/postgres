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
 *							verify checksums from files skip it.  It holds the
 *							nodes of tree maps too, pages of the store itself
 *	BMAP_REFS_RELNUMBER		reference counts: for each physical block, how many
 *							maps (or a tree's pages) hold it besides the first
 *	BMAP_QUEUE_RELNUMBER	the release queue: subtrees and blocks of dropped
 *							maps, each holding what it names until a
 *							background pass lets go of it
 *	BMAP_ROOTS_RELNUMBER	captured roots: trees of relations as they were
 *							at a moment, each holding its subtrees until it
 *							is let go of, whatever becomes of the relation
 *
 * The five relation file numbers are reserved: below FirstGenbkiObjectId and
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
#define BMAP_QUEUE_RELNUMBER	9441
#define BMAP_ROOTS_RELNUMBER	9442

#endif							/* BMAP_FILES_H */
