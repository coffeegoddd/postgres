/*-------------------------------------------------------------------------
 *
 * prolly_merge.h
 *	  Diff and merge interface: comparing and combining versions.
 *
 * Diff and three-way merge work on canonical trees, so they don't depend on
 * the engine that stores either side.  Structural sharing lets a diff skip
 * every subtree whose hash is equal on both sides.
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
 *   it raises an error instead, changing nothing.
 * - conflicts() reports a table's unresolved conflicts of the merge in
 *   progress, in key order, each row as the common ancestor, the current
 *   branch and the merged branch have it (NULL where it doesn't exist).
 * - resolve() resolves them, keeping the current rows or taking the merged
 *   branch's.
 * - tables() reports each table that differs between two refs (its rows or
 *   its definition), by qualified name, in name order: added, removed or
 *   modified.  diff() may leave out the rows of tables the session branch
 *   doesn't have; tables() names them.
 * - move_to() makes the session branch hold what a canonical root holds,
 *   writing the rows that differ as changes (prolly_applier.h), with the
 *   calling transaction.
 * - cherry_pick() commits refs' changes (each from its parent) again on
 *   the current branch, or with revert undoes them, merging each three-way
 *   with the branch as the ones before left it, every ref resolved first,
 *   and returns the one commit made of them; a commit of nothing fails
 *   unless allow_empty.  A pick's rows changed differently on both sides
 *   are conflicts, as a merge's: it makes no commit (commit_id's len is
 *   0), records the pick in progress, whose commit, once they are
 *   resolved, has the one parent, and returns how many there are.  A
 *   revert's conflicts are an error.
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

/* merge() flags */
#define PROLLY_MERGE_NO_FF	0x01	/* a merge commit even if it could
									 * fast-forward */
#define PROLLY_MERGE_FAIL_ON_CONFLICT 0x02	/* conflicts are an error */

typedef struct ProllyConflictSink ProllyConflictSink;
struct ProllyConflictSink
{
	void		(*conflict) (ProllyConflictSink *self, const ProllyKey *key,
							 const ProllyValue *base,
							 const ProllyValue *ours,
							 const ProllyValue *theirs);
};

/* Visits one table that differs between two refs. */
typedef void (*ProllyTableChangeVisit) (const char *name, ProllyDiffKind kind,
										void *arg);

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
	void		(*move_to) (const ProllyNodeRef *root);
	int			(*cherry_pick) (const char *const *refs, int nrefs, bool revert,
								bool allow_empty, ProllyHash *commit_id);
} ProllyMergeRoutine;

#define PROLLY_MERGE_CALLBACK_NAMES \
	"diff", "merge", "conflicts", "resolve", "tables", "move_to", \
	"cherry_pick"
#define PROLLY_MERGE_FIRST_CALLBACK diff

static inline const ProllyMergeRoutine *
prolly_merge_lookup(const char *name)
{
	return (const ProllyMergeRoutine *)
		prolly_get_impl(PROLLY_IFACE_DIFF_MERGE, name);
}

#endif							/* PROLLY_MERGE_H */
