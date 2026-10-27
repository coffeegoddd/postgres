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
 * - bounded_rows() reads one commit's rows of a table within bounds on
 *   its primary key, seeking the commit's data tree, where the version
 *   sorts and decodes as the table does now.
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
 *   the violations found.  With all, every row is checked; without, only
 *   the rows changed since the head commit (matched by key), and, for a
 *   foreign key, those referencing a row the referenced table removed or
 *   changed since; a table with violations recorded, or one whose rows
 *   can't be matched by key, whole.
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
 * - A merge's, pick's or revert's definitions that don't merge (a schema
 *   conflict, the merge's limits on reading history across a type's
 *   changes too) fail with errschemaconflict() among the error's
 *   arguments, which callers that stop on schema conflicts (a rebase)
 *   tell the error apart by: prolly_schema_conflict_raised is set as it
 *   is raised, for the caller to clear before and read once caught.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_merge.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_MERGE_H
#define PROLLY_MERGE_H

#include "access/prolly_canon.h"
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

/*
 * Bounds on a table's rows by its primary key as the table is defined now:
 * its first neq key columns equal to eq[], then, if has_lo (has_hi), the
 * next one at least lo (at most hi), strictly if lo_strict (hi_strict).
 * The values are of the key columns' types.
 */
typedef struct ProllyKeyBounds
{
	int			neq;
	Datum	   *eq;
	bool		has_lo;
	bool		lo_strict;
	Datum		lo;
	bool		has_hi;
	bool		hi_strict;
	Datum		hi;
} ProllyKeyBounds;

/* What a bounded_rows() read did */
typedef enum ProllyBoundedRead
{
	PROLLY_BOUNDED_READ,		/* the rows in bounds given */
	PROLLY_BOUNDED_SAME,		/* the same tree as *data: none given */
	PROLLY_BOUNDED_NO,			/* can't read it so: none given */
} ProllyBoundedRead;

typedef struct ProllyMergeRoutine
{
	ProllyImplInfo info;

	/* relid InvalidOid: every table; from_ref NULL: from the empty root. */
	void		(*diff) (const char *from_ref, const char *to_ref, Oid relid,
						 ProllyDiffSink *sink);
	int			(*merge) (const char *branch, const char *message, int flags,
						  ProllyHash *commit_id);
	void		(*conflicts) (Oid relid, ProllyConflictSink *sink);
	void		(*resolve) (Oid relid, bool theirs);
	void		(*tables) (const char *from_ref, const char *to_ref,
						   ProllyTableChangeVisit visit, void *arg);

	/*
	 * Optional: what differs between two refs, object by object, as the
	 * canonicalizer's changes() tells it, with the objects a read of a
	 * working set left out (not_covered) after them.  With definitions,
	 * for their definitions' text: a ref of an older format is refused.
	 * from_ref NULL: from the empty root (what a first commit adds).
	 */
	void		(*summary) (const char *from_ref, const char *to_ref,
							bool definitions, ProllyCanonChangeVisit visit,
							void *arg);
	/*
	 * Move the session's branch to a canonical root: its rows (and
	 * definitions) written.  working is its working set folded already, or
	 * NULL to fold it here.
	 */
	void		(*move_to) (const ProllyNodeRef *root, const ProllyNodeRef *working);
	int			(*cherry_pick) (const char *const *refs, int nrefs,
								bool allow_empty, ProllyHash *commit_id);
	List	   *(*verify) (List *relids, bool all);
	int			(*revert) (const char *const *refs, int nrefs,
						   const ProllyHash *series_head,
						   ProllyHash *commit_id);

	/*
	 * Optional: cherry_pick() of one ref without a commit: its changes
	 * merged into the working set, three-way with the branch, but changes
	 * overlapping uncommitted ones, conflicts and violations each an error
	 * (fn's, the caller's name in it), nothing left in progress.
	 */
	void		(*apply) (const char *ref, const char *fn);

	/*
	 * Optional: about how many rows relid's table has at ref (a commit, by
	 * any ref but WORKING), read from its tree's counts, not its rows: for
	 * the planner, so it reads no more than the commit and its root's
	 * items, and refuses nothing.  False if it can't tell (no such ref, a
	 * ref without the table, WORKING).  A keyless table's rows count once
	 * however many copies each has.
	 */
	bool		(*row_count) (const char *ref, Oid relid, double *rows);

	/*
	 * Optional: relid's table's rows at ref (a commit) whose keys are within
	 * any of bounds[] (nbounds of them), to sink as diff() from nothing
	 * gives them (each added), in key order within each bound, read by
	 * seeking the commit's data tree: O(its depth) a bound, and the rows
	 * in it.  *data is the table's data tree there (len 0: none); if it is
	 * the tree *data named already, nothing is read (PROLLY_BOUNDED_SAME:
	 * the caller has those rows).  PROLLY_BOUNDED_NO if the version can't
	 * be read so (keyed otherwise, or defined otherwise than the table is
	 * now): the caller reads it with diff().
	 */
	ProllyBoundedRead (*bounded_rows) (const char *ref, Oid relid,
									   const ProllyKeyBounds *bounds,
									   int nbounds, ProllyHash *data,
									   ProllyDiffSink *sink);
} ProllyMergeRoutine;

#define PROLLY_MERGE_CALLBACK_NAMES \
	"diff", "merge", "conflicts", "resolve", "tables", "summary", \
	"move_to", "cherry_pick", "verify", "revert", "apply", "row_count", \
	"bounded_rows"
#define PROLLY_MERGE_FIRST_CALLBACK diff
#define PROLLY_MERGE_LAST_CALLBACK bounded_rows

/* a schema conflict's error: see above */
extern PGDLLIMPORT bool prolly_schema_conflict_raised;
extern int	errschemaconflict(void);

#endif							/* PROLLY_MERGE_H */
