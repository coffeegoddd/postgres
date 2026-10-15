/*-------------------------------------------------------------------------
 *
 * bmap.h
 *	  Block-mapped storage manager: relations whose main fork's logical
 *	  blocks are mapped to physical blocks of a shared page store.
 *
 * See src/backend/storage/smgr/bmap.c.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/bmap.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BMAP_H
#define BMAP_H

#include "storage/bmap_files.h"
#include "storage/smgr.h"
#include "storage/sync.h"

/* the smgr implementation's number in smgrsw[] */
#define SMGR_BMAP		1

/* developer setting: heap tables made in block-mapped storage */
extern PGDLLIMPORT bool debug_block_mapped_heap;

/* storage manager callbacks */
extern void bmapinit(void);
extern bool bmapclaims(RelFileLocatorBackend rlocator);
extern void bmapopen(SMgrRelation reln);
extern void bmapclose(SMgrRelation reln, ForkNumber forknum);
extern void bmapcreate(SMgrRelation reln, ForkNumber forknum, bool isRedo);
extern bool bmapexists(SMgrRelation reln, ForkNumber forknum);
extern void bmappredrop(SMgrRelation reln, bool isRedo);
extern void bmapunlink(RelFileLocatorBackend rlocator, ForkNumber forknum,
					   bool isRedo);
extern void bmapextend(SMgrRelation reln, ForkNumber forknum,
					   BlockNumber blocknum, const void *buffer, bool skipFsync);
extern void bmapzeroextend(SMgrRelation reln, ForkNumber forknum,
						   BlockNumber blocknum, int nblocks, bool skipFsync);
extern void bmappreextend(SMgrRelation reln, ForkNumber forknum,
						  BlockNumber blocknum, int nblocks);
extern bool bmapprefetch(SMgrRelation reln, ForkNumber forknum,
						 BlockNumber blocknum, int nblocks);
extern uint32 bmapmaxcombine(SMgrRelation reln, ForkNumber forknum,
							 BlockNumber blocknum);
extern void bmapreadv(SMgrRelation reln, ForkNumber forknum,
					  BlockNumber blocknum, void **buffers,
					  BlockNumber nblocks);
extern void bmapprepareread(SMgrRelation reln, ForkNumber forknum,
							BlockNumber blocknum, BlockNumber nblocks);
extern void bmapstartreadv(PgAioHandle *ioh, SMgrRelation reln,
						   ForkNumber forknum, BlockNumber blocknum,
						   void **buffers, BlockNumber nblocks);
extern void bmapwritev(SMgrRelation reln, ForkNumber forknum,
					   BlockNumber blocknum, const void **buffers,
					   BlockNumber nblocks, bool skipFsync);
extern void bmapwriteback(SMgrRelation reln, ForkNumber forknum,
						  BlockNumber blocknum, BlockNumber nblocks);
extern BlockNumber bmapnblocks(SMgrRelation reln, ForkNumber forknum);
extern void bmappretruncate(SMgrRelation reln, ForkNumber forknum);
extern void bmaptruncate(SMgrRelation reln, ForkNumber forknum,
						 BlockNumber curnblk, BlockNumber nblocks);
extern void bmapimmedsync(SMgrRelation reln, ForkNumber forknum);
extern void bmapregistersync(SMgrRelation reln, ForkNumber forknum);
extern int	bmapfd(SMgrRelation reln, ForkNumber forknum,
				   BlockNumber blocknum, uint32 *off);

/* sync handler callbacks (maps unlinked after a checkpoint) */
extern int	bmapsyncfiletag(const FileTag *ftag, char *path);
extern int	bmapunlinkfiletag(const FileTag *ftag, char *path);
extern bool bmapfiletagmatches(const FileTag *ftag, const FileTag *candidate);

/* remove a database's unused block map from a tablespace's directory */
extern void bmap_drop_unused(Oid spcOid, Oid dbOid, const char *path);

/* does a relation file number name a block-mapped relation? */
extern bool bmap_relation_exists(RelFileLocator rlocator);

#endif							/* BMAP_H */
