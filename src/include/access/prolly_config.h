/*-------------------------------------------------------------------------
 *
 * prolly_config.h
 *	  Choosing which implementation of each interface to use.
 *
 * Components are chosen by configuration, never by code: a prolly table's
 * (the components its rows' history is folded with) by its access
 * method's profile, a database's version control by its profile.
 *
 * src/include/access/prolly_config.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_CONFIG_H
#define PROLLY_CONFIG_H

#include "access/prolly_applier.h"
#include "access/prolly_canon.h"
#include "access/prolly_changes.h"
#include "access/prolly_chunker.h"
#include "access/prolly_chunkformat.h"
#include "access/prolly_chunkstore.h"
#include "access/prolly_hash.h"
#include "access/prolly_keyenc.h"
#include "access/prolly_keying.h"
#include "access/prolly_memo.h"
#include "access/prolly_merge.h"
#include "access/prolly_reclaim.h"
#include "access/prolly_refs.h"
#include "access/prolly_remote.h"
#include "access/prolly_tablefile.h"
#include "access/prolly_transfer.h"
#include "access/prolly_tree.h"
#include "access/prolly_valenc.h"
#include "access/prolly_writers.h"

/* The components a prolly table's rows' history is folded with. */
typedef struct ProllyComponents
{
	const ProllyChunkerRoutine *chunker;
	const ProllyHashRoutine *hash;
	const ProllyKeyEncoderRoutine *key_encoder;
	const ProllyValueEncoderRoutine *value_encoder;
	const ProllyTreeRoutine *tree;
	const ProllyKeyingRoutine *keying;
} ProllyComponents;

/* Database-wide components: version control. */
typedef struct ProllyDatabaseComponents
{
	const ProllyRefsRoutine *refs;
	const ProllyTransferRoutine *transfer;
	const ProllyMergeRoutine *diff_merge;
	const ProllyCanonRoutine *canonicalizer;
	const ProllyReclaimRoutine *reclamation;
	const ProllyWritersRoutine *writers;
} ProllyDatabaseComponents;

/* Components for rel: a prolly table, or an index on one (an error otherwise). */
extern void prolly_components_for_relation(Relation rel,
										   ProllyComponents *components);

extern void prolly_database_components(ProllyDatabaseComponents *components);

/*
 * The table access method a new relation of relpersistence gets, given
 * amoid: a temporary or unlogged relation is never stored by prolly (it is
 * never part of a commit), so it gets heap; asking for prolly explicitly
 * (explicit) for one is an error.
 */
extern Oid	prolly_table_am_for_persistence(Oid amoid, char relpersistence,
											bool explicit);

/* Error if a prolly table is to become unlogged. */
extern void prolly_check_unlogged(Relation rel);

#endif							/* PROLLY_CONFIG_H */
