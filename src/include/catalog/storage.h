/*-------------------------------------------------------------------------
 *
 * storage.h
 *	  prototypes for functions in backend/catalog/storage.c
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/storage.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef STORAGE_H
#define STORAGE_H

#include "storage/block.h"
#include "storage/relfilelocator.h"
#include "storage/smgr.h"
#include "utils/relcache.h"

/* GUC variables */
extern PGDLLIMPORT int wal_skip_threshold;

/* while not -1, new permanent heap storage goes to this storage manager */
extern PGDLLIMPORT int new_relation_smgr;

extern SMgrRelation RelationCreateStorage(RelFileLocator rlocator,
										  char relpersistence,
										  bool register_delete);
extern int	NewRelationStorageManager(char relpersistence, bool shared,
									  bool catalog);
extern SMgrRelation RelationCreateStorageWith(RelFileLocator rlocator,
											  char relpersistence,
											  bool register_delete,
											  int smgr_which);
extern SMgrRelation RelationCreateStorageShared(RelFileLocator rlocator,
												SMgrRelation src,
												bool register_delete);
extern SMgrRelation RelationCreateStorageFromRoot(RelFileLocator rlocator,
												  uint64 root,
												  SMgrRelation maps_from,
												  SMgrRelation replacing,
												  SMgrRelation sharer,
												  SMgrRelation lineage_of,
												  bool register_delete);
extern void RelationDropStorage(Relation rel);
extern void RelationPreserveStorage(RelFileLocator rlocator, bool atCommit);
extern void RelationPreTruncate(Relation rel);
extern void RelationTruncate(Relation rel, BlockNumber nblocks);
extern void RelationCopyStorage(SMgrRelation src, SMgrRelation dst,
								ForkNumber forkNum, char relpersistence);
extern void RelationCopyMapFork(SMgrRelation src, SMgrRelation dst,
								ForkNumber forkNum);
extern bool RelFileLocatorSkippingWAL(RelFileLocator rlocator);
extern void RelFileLocatorForceWALAtCommit(RelFileLocator rlocator);
extern Size EstimatePendingSyncsSpace(void);
extern void SerializePendingSyncs(Size maxSize, char *startAddress);
extern void RestorePendingSyncs(char *startAddress);

/*
 * These functions used to be in storage/smgr/smgr.c, which explains the
 * naming
 */
extern void smgrDoPendingDeletes(bool isCommit);
extern void smgrDoPendingSyncs(bool isCommit, bool isParallelWorker);
extern void smgrPreCommitPendingDeletes(void);
extern int	smgrGetPendingDeletes(bool forCommit, RelFileLocator **ptr);
extern void AtSubCommit_smgr(void);
extern void AtSubAbort_smgr(void);
extern void PostPrepare_smgr(void);

#endif							/* STORAGE_H */
