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

	/* Resolve a branch name or commit id to a commit id. */
	bool		(*resolve) (const char *refname, ProllyHash *commit_id);

	bool		(*commit_get) (const ProllyHash *commit_id,
							   ProllyCommitInfo *info);
	bool		(*merge_base) (const ProllyHash *a, const ProllyHash *b,
							   ProllyHash *base);

	/*
	 * Visit every node the refs keep alive, from the root of their own
	 * store (the head of PROLLY_REFS_BRANCH): for reclamation.
	 */
	void		(*walk) (const ProllyNodeRef *refs, ProllyTreeVisitNode visit,
						 void *arg);
} ProllyRefsRoutine;

#define PROLLY_REFS_CALLBACK_NAMES \
	"branch_create", "branch_delete", "branch_lookup", "checkout", \
	"commit", "resolve", "commit_get", "merge_base", "walk"
#define PROLLY_REFS_FIRST_CALLBACK branch_create

static inline const ProllyRefsRoutine *
prolly_refs_lookup(const char *name)
{
	return (const ProllyRefsRoutine *)
		prolly_get_impl(PROLLY_IFACE_REFS, name);
}

#endif							/* PROLLY_REFS_H */
