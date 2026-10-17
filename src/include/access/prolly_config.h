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
#include "access/prolly_canon.h"
#include "access/prolly_changes.h"
#include "access/prolly_chunker.h"
#include "access/prolly_applier.h"
#include "access/prolly_chunkformat.h"
#include "access/prolly_chunkstore.h"
#include "access/prolly_commit.h"
#include "access/prolly_coord.h"
#include "access/prolly_engine.h"
#include "access/prolly_hash.h"
#include "access/prolly_ixkeying.h"
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
#include "access/prolly_tablefile.h"
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
	const ProllyIndexKeyingRoutine *index_keying;
} ProllyComponents;

/* Database-wide components: version control. */
typedef struct ProllyDatabaseComponents
{
	const ProllyRefsRoutine *refs;
	const ProllyTransferRoutine *transfer;
	const ProllyMergeRoutine *diff_merge;
	const ProllyCanonRoutine *canonicalizer;
} ProllyDatabaseComponents;

/*
 * Components for a table using prolly table AM amname.  Raises an error if
 * amname is not a prolly table AM.
 */
extern void prolly_components_for_am(const char *amname,
									 ProllyComponents *components);

/* How the history of page-based prolly tables keys their rows. */
extern const ProllyKeyingRoutine *prolly_history_keying(void);

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
extern bool prolly_database_uses_copies(void);

/* Is amname a prolly table access method? */
extern bool prolly_is_table_am(const char *amname);

/*
 * The table access method a new relation of relpersistence gets, given
 * amoid: a temporary or unlogged relation is never stored by prolly (it is
 * never part of a commit), so it gets heap; asking for prolly explicitly
 * (explicit) for one is an error.  In a versioned database, a permanent
 * relation must be a prolly one (prolly_check_versioned_am).
 */
extern Oid	prolly_table_am_for_persistence(Oid amoid, char relpersistence,
											bool explicit);

/* Error if a prolly table is to become unlogged. */
extern void prolly_check_unlogged(Relation rel);

/*
 * Error if a permanent relation stored by amoid can't be in the current
 * database: a versioned database versions every permanent table, so they
 * are all prolly tables.
 */
extern void prolly_check_versioned_am(Oid amoid, char relpersistence);

/*
 * NOTICE that a relation stored by amoid (a table or index access method)
 * ignores the tablespace it is given, if it is a prolly one: its rows are
 * in the node store.
 */
extern void prolly_note_tablespace(Oid amoid, const char *tablespacename);

#endif							/* PROLLY_CONFIG_H */
