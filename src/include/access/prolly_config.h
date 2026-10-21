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
#include "access/prolly_changelog.h"
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
#include "storage/lockdefs.h"

/* The components a prolly table's rows' history is folded with. */
typedef struct ProllyComponents
{
	const ProllyChunkerRoutine *chunker;
	const ProllyHashRoutine *hash;
	const ProllyKeyEncoderRoutine *key_encoder;
	const ProllyValueEncoderRoutine *value_encoder;
	const ProllyTreeRoutine *tree;
	const ProllyKeyingRoutine *keying;
	const ProllyChangeFinderRoutine *change_finder;
	const ProllyChangeLogRoutine *change_log;	/* NULL: none is kept */
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

/* The content hash every chunk is named by. */
extern const ProllyHashRoutine *prolly_content_hash(void);

/*
 * The components a table's committed versions are written with, without
 * the table: for reading a version only its commits describe.
 */
extern void prolly_history_components(ProllyComponents *components);

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

/*
 * The change finder folds use: the profile's, unless the developer setting
 * names another (any may fold any table: each folds in full from a
 * baseline another made).
 */
extern PGDLLIMPORT char *debug_prolly_change_finder;
extern const ProllyChangeFinderRoutine *prolly_change_finder(void);

/*
 * The change log, if the profile keeps one: a prolly table's made as the
 * table is made (or, for one that has none, as it is altered), and told
 * each tuple version a write to one made or replaced.
 */
extern void prolly_make_change_log(Oid relid, LOCKMODE lockmode);
extern void prolly_change_logs_flush(void);
extern void prolly_note_change(Relation rel, const ItemPointerData *oldtid,
							   HeapTuple oldtup, const ItemPointerData *newtid);
/* ... or that a table was emptied (truncated) */
extern void prolly_note_emptied(Relation rel);
/* ... or of the versions a write made, in hand (prolly_changelog.h) */
extern void prolly_note_made(Relation rel, const ItemPointerData *oldtid,
							 HeapTuple oldtup, HeapTuple *made, int nmade);

/*
 * The relation holding the change log of table relid (of access method
 * relam), if it is one (else InvalidOid).
 */
extern Oid	prolly_change_log_relid(Oid relid, Oid relam);

/*
 * Told to the refs, for the folds they skip (#482): a transaction about to
 * write a branch (catalog/branch.c), and something every fold reads
 * changed under them all (a catalog change sent to other sessions, chunks
 * a collection dropped), and a prepared transaction that ended
 * (twophase.c), which may have written any branch.  prolly_forget_folds()
 * and prolly_note_prepared_ended() neither error nor allocate: they are
 * called in critical sections, or with interrupts held, too.
 */
extern void prolly_note_branch_write(Oid branch);
extern void prolly_forget_folds(void);
extern void prolly_note_prepared_ended(FullTransactionId xid);

#endif							/* PROLLY_CONFIG_H */
