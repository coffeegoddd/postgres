/*-------------------------------------------------------------------------
 *
 * prolly_refs.h
 *	  Refs and commit graph interface: branches, commits and history.
 *
 * A commit records a root, its parents and metadata.  Branches name commits
 * (or working roots).  Where this lives (catalog tables or elsewhere) and how
 * it relates to versioned catalogs is the implementation's business.
 *
 * Contract:
 * - Branch and commit changes are transactional with the calling
 *   transaction.
 * - Commit ids are content hashes of the commit record, so the same history
 *   has the same ids in every database (clone/push/pull).
 * - Lookup functions return false when not found; other functions raise an
 *   error.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_refs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_REFS_H
#define PROLLY_REFS_H

#include "access/prolly_registry.h"
#include "access/prolly_tree.h"
#include "access/prolly_types.h"
#include "datatype/timestamp.h"

#define PROLLY_MAX_PARENTS 2

typedef struct ProllyCommitInfo
{
	ProllyHash	id;
	ProllyNodeRef root;
	int			nparents;
	ProllyHash	parents[PROLLY_MAX_PARENTS];
	TimestampTz committed_at;
	char	   *author;			/* palloc'd */
	char	   *message;		/* palloc'd */
} ProllyCommitInfo;

/*
 * A merge in progress on a branch: it stopped with conflicts in tables,
 * which must be resolved before the merge commit is made.
 */
typedef struct ProllyMergeState
{
	ProllyHash	theirs;			/* the merged branch's head commit */
	ProllyHash	base;			/* the common ancestor */
	int			ntables;
	Oid		   *tables;			/* with unresolved conflicts (palloc'd) */
} ProllyMergeState;

/* Visits one branch: its name and head commit (len 0: none yet). */
typedef void (*ProllyBranchVisit) (const char *name, const ProllyHash *head,
								   void *arg);

typedef struct ProllyRefsRoutine
{
	ProllyImplInfo info;

	/* start_point NULL means the current branch's head. */
	void		(*branch_create) (const char *name, const char *start_point);
	void		(*branch_delete) (const char *name, bool force);
	bool		(*branch_lookup) (const char *name, ProllyBranchId *branch);

	/* Make name the current session's branch. */
	void		(*checkout) (const char *name);

	/*
	 * Record a commit of the current branch's working root, as the current
	 * transaction will commit it.  With a merge in progress, it is the
	 * merge commit (both heads its parents), and every conflict must have
	 * been resolved.
	 */
	void		(*commit) (const char *message, ProllyHash *commit_id);

	/*
	 * Resolve a branch name, commit id or HEAD (the session branch's head
	 * commit), each optionally followed by ancestry steps as in Dolt and
	 * Git (~n: the nth first-parent ancestor; ^n: the nth parent), to a
	 * commit id.
	 */
	bool		(*resolve) (const char *refname, ProllyHash *commit_id);

	bool		(*commit_get) (const ProllyHash *commit_id,
							   ProllyCommitInfo *info);
	bool		(*merge_base) (const ProllyHash *a, const ProllyHash *b,
							   ProllyHash *base);

	/* The working set a commit recorded; false if the refs don't know it. */
	bool		(*commit_working_set) (const ProllyHash *commit_id,
									   ProllyNodeRef *root);

	/*
	 * Record a commit of the current branch with these parents, and make
	 * it the branch's head commit: of its working set as the transaction
	 * will commit it, or of working_set if it isn't NULL (a canonical root:
	 * a merge's, leaving uncommitted changes out).
	 */
	void		(*commit_with) (const char *message, const ProllyHash *parents,
								int nparents, const ProllyNodeRef *working_set,
								ProllyHash *commit_id);

	/*
	 * A number that grows (as the current transaction sees the refs)
	 * whenever a commit or a branch is made: a root that may share the rows
	 * of a branch's working set.  0: none ever was.
	 */
	uint64		(*epoch) (void);

	/* Visit every branch, by name. */
	void		(*branches) (ProllyBranchVisit visit, void *arg);

	/*
	 * A branch's working set as last committed (NULL: the session's
	 * branch); false if there's no such branch.
	 */
	bool		(*working_set) (const char *branch, ProllyNodeRef *root);

	/*
	 * Make the commit ref names the session branch's head commit, and its
	 * working set the one that commit recorded, when the transaction
	 * commits (a hard reset, or a fast-forward).  The transaction must not
	 * have written to prolly tables.
	 */
	void		(*reset) (const char *ref);

	/*
	 * The session branch's merge in progress (false: none), and setting it
	 * (NULL: none), with the current transaction.  The transaction can't
	 * commit while its branch's merge has unresolved conflicts, unless
	 * prolly_allow_commit_conflicts().  A reset ends a merge.
	 */
	bool		(*merge_state) (ProllyMergeState *state);
	void		(*set_merge_state) (const ProllyMergeState *state);

	/*
	 * Hold the refs until the transaction ends, before reading heads to
	 * move one by (a merge's): other movers wait, and it waits for one
	 * moving them now.  If the session's branch was moved by a transaction
	 * the snapshot doesn't see (what it would read of the branch's rows
	 * isn't the head's), it pushes a newer snapshot, which sees it, and
	 * returns true, the caller popping it when done (read committed); or
	 * fails, to be retried, where the snapshot can't move.
	 */
	bool		(*hold) (void);

	/*
	 * Visit every node the refs keep alive, from the root of their own
	 * store (the head of PROLLY_REFS_BRANCH): for reclamation.
	 */
	void		(*walk) (const ProllyNodeRef *refs, ProllyTreeVisitNode visit,
						 void *arg);

	/*
	 * A working-set checkpoint: fold the session branch's working set as a
	 * commit would, without a commit, so later folds start from it; with
	 * the transaction.  *root gets the working set's canonical root.
	 */
	void		(*checkpoint) (ProllyNodeRef *root);

	/*
	 * Make a commit the session branch's head commit, with the
	 * transaction, changing nothing of its working set (a fast-forward
	 * whose rows the caller wrote).
	 */
	void		(*set_head) (const ProllyHash *commit_id);
} ProllyRefsRoutine;

#define PROLLY_REFS_CALLBACK_NAMES \
	"branch_create", "branch_delete", "branch_lookup", "checkout", \
	"commit", "resolve", "commit_get", "merge_base", "commit_working_set", \
	"commit_with", "epoch", "branches", "working_set", "reset", "merge_state", \
	"set_merge_state", "hold", "walk", "checkpoint", "set_head"
#define PROLLY_REFS_FIRST_CALLBACK branch_create

static inline const ProllyRefsRoutine *
prolly_refs_lookup(const char *name)
{
	return (const ProllyRefsRoutine *)
		prolly_get_impl(PROLLY_IFACE_REFS, name);
}

#endif							/* PROLLY_REFS_H */
