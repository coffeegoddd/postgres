/*-------------------------------------------------------------------------
 *
 * prolly_config.h
 *	  Choosing which implementation of each interface to use.
 *
 * A tree's components (engine, coordination and foundation) are chosen by
 * configuration, never by code.
 * Today the only source is a default profile per prolly table AM.  Later
 * sources (tablespace options, settings) plug in here without changing
 * callers.  Once a tree exists, its components come from the format stamps
 * it recorded at creation, not from current configuration, so changing
 * configuration never changes how existing data is read.  (No tree can be
 * created yet, so that part isn't built.)
 *
 * src/include/access/prolly_config.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_CONFIG_H
#define PROLLY_CONFIG_H

#include "access/prolly_alloc.h"
#include "access/prolly_chunker.h"
#include "access/prolly_commit.h"
#include "access/prolly_coord.h"
#include "access/prolly_engine.h"
#include "access/prolly_hash.h"
#include "access/prolly_keyenc.h"
#include "access/prolly_keying.h"
#include "access/prolly_log.h"
#include "access/prolly_merge.h"
#include "access/prolly_nodeformat.h"
#include "access/prolly_nodestore.h"
#include "access/prolly_reclaim.h"
#include "access/prolly_refs.h"
#include "access/prolly_rootlayout.h"
#include "access/prolly_rowid.h"
#include "access/prolly_transfer.h"
#include "access/prolly_tree.h"
#include "access/prolly_valenc.h"

/* The components of one tree. */
typedef struct ProllyComponents
{
	const ProllyEngineRoutine *engine;
	const ProllyCoordRoutine *coord;
	const ProllyNodeFormatRoutine *node_format;
	const ProllyChunkerRoutine *chunker;
	const ProllyHashRoutine *hash;
	const ProllyKeyEncoderRoutine *key_encoder;
	const ProllyValueEncoderRoutine *value_encoder;
	const ProllyNodeStoreRoutine *node_store;
	const ProllyAllocatorRoutine *allocator;
	const ProllyReclaimRoutine *reclamation;
	const ProllyLogRoutine *log;
	const ProllyCommitRoutine *commit;
	const ProllyRowIdRoutine *row_identity;
	const ProllyTreeRoutine *tree;
	const ProllyRootLayoutRoutine *root_layout;
	const ProllyKeyingRoutine *keying;
} ProllyComponents;

/* Database-wide components: version control. */
typedef struct ProllyDatabaseComponents
{
	const ProllyRefsRoutine *refs;
	const ProllyTransferRoutine *transfer;
	const ProllyMergeRoutine *diff_merge;
} ProllyDatabaseComponents;

/*
 * Components for a table using prolly table AM amname.  Raises an error if
 * amname is not a prolly table AM.
 */
extern void prolly_components_for_am(const char *amname,
									 ProllyComponents *components);

/*
 * Components for rel: a table using a prolly table AM, or an index on one.
 */
extern void prolly_components_for_relation(Relation rel,
										   ProllyComponents *components);

/*
 * Components of the first profile that uses engine (for an engine's own
 * work that isn't about one relation, such as finding branch heads).
 */
extern void prolly_components_for_engine(const ProllyImplInfo *engine,
										 ProllyComponents *components);

extern void prolly_database_components(ProllyDatabaseComponents *components);

#endif							/* PROLLY_CONFIG_H */
