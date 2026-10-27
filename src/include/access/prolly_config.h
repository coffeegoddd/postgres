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
#include "access/prolly_commitcache.h"
#include "access/prolly_window.h"
#include "access/prolly_hash.h"
#include "access/prolly_jsontree.h"
#include "access/prolly_keyenc.h"
#include "access/prolly_keying.h"
#include "access/prolly_matcache.h"
#include "access/prolly_memo.h"
#include "access/prolly_moveplan.h"
#include "access/prolly_nodecache.h"
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
	const ProllyMatCacheRoutine *matcache;
	const ProllyCommitCacheRoutine *commit_cache;
	const ProllyWindowRoutine *history_window;
} ProllyDatabaseComponents;

/* Components for rel: a prolly table, or an index on one (an error otherwise). */
extern void prolly_components_for_relation(Relation rel,
										   ProllyComponents *components);

extern void prolly_database_components(ProllyDatabaseComponents *components);

/* The content hash every chunk is named by. */
extern const ProllyHashRoutine *prolly_content_hash(void);

/* The cache of decoded chunks the chunk stores read through (NULL: none). */
extern const ProllyNodeCacheRoutine *prolly_node_cache(void);

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
/*
 * Privileges over the version control's shared state.  prolly_writes_refs():
 * the role owns the database, has its CREATE privilege, or has
 * pg_prolly_write's privileges.  prolly_branch_owner(): the role that made a
 * branch, else (main, or a maker unrecorded) the database's owner.
 * A move of a branch's head (a commit, reset, merge, cherry-pick, revert or
 * rebase, its deletion or rename) takes either (prolly_may_move_head(),
 * prolly_check_head_move()); a rename's new name takes the first, as
 * making a branch does (prolly_check_branch_rename()).
 */
extern bool prolly_writes_refs(void);
extern Oid	prolly_branch_owner(const char *name);
extern bool prolly_may_move_head(const char *name);
extern void prolly_check_head_move(const char *name);

/*
 * A merge, cherry-pick, revert or stash apply runs between
 * prolly_move_begin() and prolly_move_end() (the end in a PG_FINALLY);
 * inside it, prolly_check_not_moving() refuses a head move, a checkout or
 * another move (prolly_check_head_move() refuses too).
 */
extern void prolly_move_begin(void);
extern void prolly_move_end(void);
extern void prolly_check_not_moving(void);
extern void prolly_check_branch_rename(const char *newname);

/*
 * A table is its relation (by OID) and its name together.  Whether a move
 * bringing versions from the commits (n of them) may put into a table here
 * a version of another relation of its name: false, and
 * prolly_refuse_mismatched_versions() would refuse nothing.
 */
extern bool prolly_versions_may_mismatch(const ProllyHash *commits, int n);

/* The relations of root's tables on the session's branch (OIDs). */
extern List *prolly_relations_of(const ProllyNodeRef *root);

/*
 * Refuse a move from before (the branch folded before it, its tables then
 * the relations relids) of versions from the commits provenance
 * (nprovenance of them) that put into a table here a version of another
 * relation: one that a commit of them made here records, under the
 * table's name, as made from another relation.  Called after the move,
 * before its transaction can commit: the error undoes all it wrote.  A
 * table the move made is its version's relation (or a new one).  kind: the
 * move, for the error's hint.
 */
typedef enum ProllyMismatchedMove
{
	PROLLY_MISMATCH_MOVE,		/* a reset, a table's checkout, ... */
	PROLLY_MISMATCH_FIRST_USE,	/* a branch's first use */
	PROLLY_MISMATCH_PUSH,		/* a push's or forced move of a branch */
} ProllyMismatchedMove;

extern void prolly_refuse_mismatched_versions(const ProllyNodeRef *before,
											  List *relids,
											  const ProllyHash *provenance,
											  int nprovenance,
											  ProllyMismatchedMove kind);

/*
 * Before a move back to head's commit (an abort's, a stash's), or a
 * branch's first use, which moves copies of another branch's relations to
 * it: drop the tables here whose relation isn't the one the commit
 * records under their name, so that the move makes each its version's
 * relation again.  why: what moves, for an error; owned: the session's
 * role must own them (not for a first use's copies, the branch's
 * nobody's yet).
 */
extern void prolly_drop_mismatched_tables(const ProllyHash *head,
										  const char *why, bool owned);

/*
 * Give back privileges a merge's resolution saved (the merge in progress's
 * identity_saved) to the relations of those names now: only views', or
 * all (an abort's).
 */
extern void prolly_restore_privileges(const char *saved, bool views);

/*
 * While set, a branch's move to a commit (refs' branch_move()) replaces
 * the branch: its tables that are other relations than the commit's of
 * their names are dropped, and the move makes the commit's (a rebase's
 * end, the rebased commits being the branch's now).  Else such a move is
 * refused, as a hard reset is.
 */
extern PGDLLIMPORT bool prolly_move_replaces;

/*
 * The branches, tags and stashes roles made, as the role commands see them:
 * each maker recorded in the refs is a shared dependency of its role
 * (classId RefsBranchClassId, RefsTagClassId or RefsStashClassId, objid the
 * record's id), so DROP ROLE refuses a role that still makes any.
 * prolly_describe_made_object() appends the object's description (DROP
 * ROLE's detail), and prolly_made_object_identity() its name (a stash's
 * list's) and a stash's place in its list, false if the refs no longer
 * have it; prolly_reassign_made_object() makes newrole its maker
 * (REASSIGN OWNED); prolly_drop_made_object() deletes a tag or stash and
 * gives a branch to the database's owner (DROP OWNED).  One the refs no
 * longer have is described by its id, and its entry is changed or dropped.
 */
extern void prolly_describe_made_object(StringInfo buf, Oid classId, Oid id);
extern bool prolly_made_object_identity(Oid classId, Oid id, char **name,
										int *n);
extern void prolly_reassign_made_object(Oid classId, Oid id, Oid newrole);
extern void prolly_drop_made_object(Oid classId, Oid id);

extern void prolly_make_change_log(Oid relid, LOCKMODE lockmode);
extern void prolly_change_logs_flush(void);
extern void prolly_note_change(Relation rel, const ItemPointerData *oldtid,
							   HeapTuple oldtup, const ItemPointerData *newtid);
/*
 * How many entries a table's change log holds that no fold has consumed,
 * estimated (-1: one says to read it in full; 0 without a log or an
 * estimate).
 */
extern double prolly_change_log_pending(Relation rel);
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
/*
 * ... and of a relation whose rows the write changes (InvalidOid: none),
 * nrows of them.
 */
extern void prolly_note_relation_write(Oid branch, Oid relid, int nrows);
extern void prolly_forget_folds(void);
extern void prolly_note_prepared_ended(FullTransactionId xid);

/*
 * Whether the relation of pg_class row form is a change log, judged by the
 * row alone: a row read on any branch.
 */
extern bool prolly_is_change_log_class(Form_pg_class form);

/*
 * What the change log of rel, a prolly table, takes (all zero if it has
 * none); false if the change log can't say.
 */
extern bool prolly_change_log_sizes(Relation rel, ProllyChangeLogSizes *sizes);

#endif							/* PROLLY_CONFIG_H */
