/*-------------------------------------------------------------------------
 *
 * prolly_merge.h
 *	  Diff and merge interface: comparing and combining versions.
 *
 * Diff and three-way merge work on canonical trees, so they don't depend on
 * how either side's working set is stored.  Structural sharing lets a diff
 * skip every subtree whose hash is equal on both sides.
 *
 * Contract:
 * - diff() reports every row that differs between the two refs, once, in
 *   key order per relation.  A ref is anything the refs resolve, or
 *   WORKING: the session branch's working set as last committed.
 * - merge() merges branch into the current branch and returns the commit
 *   the branch is at afterwards: the current head if branch was merged
 *   already; branch's head if the current one is its ancestor (a
 *   fast-forward, unless PROLLY_MERGE_NO_FF); else a merge commit, with
 *   message (NULL: a default one).  Rows changed differently on both
 *   sides are conflicts: the merge applies everything else, keeps the
 *   current branch's version of those rows, records a merge in progress
 *   (the refs' merge state), makes no commit (commit_id's len is 0) and
 *   returns how many conflicts there are; with PROLLY_MERGE_FAIL_ON_CONFLICT
 *   it raises an error instead, changing nothing.  With
 *   PROLLY_MERGE_NO_COMMIT a merge that would make a merge commit makes
 *   none (commit_id's len is 0) and leaves the merge in progress, as its
 *   conflicts would; with PROLLY_MERGE_SQUASH it brings in the changes
 *   without a merge in progress, and makes a commit of them with one
 *   parent unless PROLLY_MERGE_NO_COMMIT (a fast-forward's moves only the
 *   rows: the head stays, and commit_id is branch's head).
 *   PROLLY_MERGE_FF_ONLY refuses a merge that isn't a fast-forward.
 * - conflicts() reports a table's unresolved conflicts of the merge in
 *   progress, in key order, each row as the common ancestor, the current
 *   branch and the merged branch have it (NULL where it doesn't exist).
 *   A table it can't read them for as it is now (renamed or moved since
 *   the merge began) is an error, or, if the sink has unreadable, is
 *   reported there instead.
 * - resolve() resolves them, keeping the current rows or taking the merged
 *   branch's.
 * - tables() reports each table that differs between two refs (its rows or
 *   its definition), by qualified name, in name order: added, removed or
 *   modified; then each other object that differs (the canonicalizer's
 *   objects()), as Doltgres's dolt_status lists them.  diff() may leave
 *   out the rows of tables the session branch doesn't have; tables() names
 *   them.
 * - move_to() makes the session branch hold what a canonical root holds,
 *   writing the rows that differ as changes (prolly_applier.h), with the
 *   calling transaction.
 * - Rows a merge (or pick) would write that break a unique, check or
 *   not-null constraint aren't written, and rows of tables it wrote that
 *   break a foreign key are left: both are recorded as the branch's
 *   constraint violations (prolly_refs.h), and stop it as conflicts do:
 *   merge() and cherry_pick() count them with its conflicts.
 * - verify() checks the session branch's tables of relids (NIL: every
 *   one) against their foreign keys, and against their check and not-null
 *   constraints not validated yet (Postgres keeps the others), and returns
 *   the violations found.
 * - cherry_pick() commits refs' changes (each from its parent) again on
 *   the current branch, merging each three-way with the branch as the
 *   ones before left it, every ref resolved first, and returns the one
 *   commit made of them; a commit of nothing fails unless allow_empty.
 *   Rows changed differently on both sides are conflicts, as a merge's:
 *   it makes no commit (commit_id's len is 0), records the pick in
 *   progress, whose commit, once they are resolved, has the one parent,
 *   and returns how many there are.
 * - revert() undoes refs' changes, one commit each, every ref resolved
 *   first; commit_id gets the last.  One that conflicts stops the series
 *   as a pick's conflicts do, recording in the merge state the head before
 *   the series (series_head; NULL: the head now), for an abort to go back
 *   to, and the commits still to revert.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_merge.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_MERGE_H
#define PROLLY_MERGE_H

#include "access/prolly_registry.h"
#include "access/prolly_types.h"
#include "nodes/pg_list.h"

/* merge() flags */
#define PROLLY_MERGE_NO_FF	0x01	/* a merge commit even if it could
									 * fast-forward */
#define PROLLY_MERGE_FAIL_ON_CONFLICT 0x02	/* conflicts are an error */
#define PROLLY_MERGE_FF_ONLY 0x04	/* only a fast-forward (or nothing to
									 * merge): else an error */
#define PROLLY_MERGE_NO_COMMIT 0x08 /* no merge commit: the merge left in
									 * progress, its commit dolt_commit's */
#define PROLLY_MERGE_SQUASH 0x10	/* the changes alone, uncommitted unless
									 * a commit is made of them, with one
									 * parent; no merge in progress */

typedef struct ProllyConflictSink ProllyConflictSink;
struct ProllyConflictSink
{
	void		(*conflict) (ProllyConflictSink *self, const ProllyKey *key,
							 const ProllyValue *base,
							 const ProllyValue *ours,
							 const ProllyValue *theirs);
	/* (if set: why the table's conflicts can't be read, not an error) */
	void		(*unreadable) (ProllyConflictSink *self, const char *why);
};

/*
 * Visits one table, or other object, that differs between two refs; object
 * is its kind (PROLLY_OBJECT_*).
 */
typedef void (*ProllyTableChangeVisit) (const char *name, ProllyDiffKind kind,
										char object, void *arg);

typedef struct ProllyMergeRoutine
{
	ProllyImplInfo info;

	/* relid InvalidOid: every table. */
	void		(*diff) (const char *from_ref, const char *to_ref, Oid relid,
						 ProllyDiffSink *sink);
	int			(*merge) (const char *branch, const char *message, int flags,
						  ProllyHash *commit_id);
	void		(*conflicts) (Oid relid, ProllyConflictSink *sink);
	void		(*resolve) (Oid relid, bool theirs);
	void		(*tables) (const char *from_ref, const char *to_ref,
						   ProllyTableChangeVisit visit, void *arg);
	/*
	 * Move the session's branch to a canonical root: its rows (and
	 * definitions) written.  working is its working set folded already, or
	 * NULL to fold it here.
	 */
	void		(*move_to) (const ProllyNodeRef *root, const ProllyNodeRef *working);
	int			(*cherry_pick) (const char *const *refs, int nrefs,
								bool allow_empty, ProllyHash *commit_id);
	List	   *(*verify) (List *relids);
	int			(*revert) (const char *const *refs, int nrefs,
						   const ProllyHash *series_head,
						   ProllyHash *commit_id);
} ProllyMergeRoutine;

#define PROLLY_MERGE_CALLBACK_NAMES \
	"diff", "merge", "conflicts", "resolve", "tables", "move_to", \
	"cherry_pick", "verify", "revert"
#define PROLLY_MERGE_FIRST_CALLBACK diff
#define PROLLY_MERGE_LAST_CALLBACK revert

#endif							/* PROLLY_MERGE_H */
