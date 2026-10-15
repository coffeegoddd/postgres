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

	/* Record a commit of the current branch's working root. */
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
	 * Record a commit of the current branch with these parents and this
	 * working set (a merge, whose working set the transaction is about to
	 * commit), and make it the branch's head commit.
	 */
	void		(*commit_with) (const char *message, const ProllyHash *parents,
								int nparents, const ProllyNodeRef *working_set,
								ProllyHash *commit_id);

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
	 * Visit every node the refs keep alive, from the root of their own
	 * store (the head of PROLLY_REFS_BRANCH): for reclamation.
	 */
	void		(*walk) (const ProllyNodeRef *refs, ProllyTreeVisitNode visit,
						 void *arg);
} ProllyRefsRoutine;

#define PROLLY_REFS_CALLBACK_NAMES \
	"branch_create", "branch_delete", "branch_lookup", "checkout", \
	"commit", "resolve", "commit_get", "merge_base", "commit_working_set", \
	"commit_with", "branches", "working_set", "reset", "walk"
#define PROLLY_REFS_FIRST_CALLBACK branch_create

static inline const ProllyRefsRoutine *
prolly_refs_lookup(const char *name)
{
	return (const ProllyRefsRoutine *)
		prolly_get_impl(PROLLY_IFACE_REFS, name);
}

#endif							/* PROLLY_REFS_H */
