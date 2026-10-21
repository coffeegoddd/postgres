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

#include "access/prolly_canon.h"
#include "access/prolly_registry.h"
#include "access/prolly_tree.h"
#include "access/prolly_types.h"
#include "access/transam.h"
#include "datatype/timestamp.h"

#define PROLLY_MAX_PARENTS 2

/*
 * The prefix of the hidden branches sessions checked out at commits are on,
 * one a commit (dolt_checkout() of a commit hash); no other branch's name
 * may begin with it.
 */
#define PROLLY_DETACHED_PREFIX "detached/"

typedef struct ProllyCommitInfo
{
	ProllyHash	id;
	ProllyNodeRef root;
	int			nparents;
	ProllyHash	parents[PROLLY_MAX_PARENTS];
	TimestampTz committed_at;
	char	   *author;			/* palloc'd */
	char	   *message;		/* palloc'd */

	/*
	 * What the commit's versioned objects needed from outside the versioned
	 * schemas when it was made (the canonicalizer's fold_dependencies());
	 * has_dependencies false for a commit that didn't record them.
	 */
	bool		has_dependencies;
	ProllyNodeRef dependencies;

	/*
	 * Its height, as prolly_history_height() finds it (1 + the most of its
	 * parents'; a root's 1); has_height false for a commit that doesn't
	 * record it (one made before commits recorded it, or elsewhere).
	 */
	bool		has_height;
	int64		height;
} ProllyCommitInfo;

/*
 * A merge (or cherry-pick) in progress on a branch: it stopped with
 * conflicts in tables, which must be resolved before its commit is made.
 */
typedef struct ProllyMergeState
{
	ProllyHash	theirs;			/* the merged branch's head commit */
	ProllyHash	base;			/* the common ancestor */
	int			ntables;
	Oid		   *tables;			/* with unresolved conflicts (palloc'd) */
	char	  **table_names;	/* each one's name (qualified) when the merge
								 * recorded it, as tables[] (NULL, or an
								 * entry NULL: not known) */
	bool		pick;			/* a cherry-pick: its commit has one parent */
	char	   *message;		/* a pick's: the picked commit's message */

	/*
	 * A revert of a series of commits, one commit each, stopped at one (a
	 * pick, of its undoing): the branch's head before the series, and the
	 * commits still to revert after it, in order.
	 */
	bool		revert;
	ProllyHash	series_head;
	int			npending;
	ProllyHash *pending;		/* (palloc'd) */
} ProllyMergeState;

/*
 * A constraint violation a merge (or cherry-pick) left on a branch: a row
 * of a table, as JSON, that breaks one of its constraints ("foreign key":
 * the row is in the table; "unique index", "check constraint", "not null":
 * the merge didn't write it), and what it breaks, as JSON.
 */
typedef struct ProllyViolation
{
	Oid			relid;
	char	   *type;
	char	   *row;
	char	   *info;
} ProllyViolation;

/* Visits one branch: its name and head commit (len 0: none yet). */
typedef void (*ProllyBranchVisit) (const char *name, const ProllyHash *head,
								   void *arg);

/* Visits a stash: its list's name, its place there (0: newest), its commit. */
typedef void (*ProllyStashVisit) (const char *name, int n, const char *branch,
								  const ProllyHash *stash, void *arg);

/* Visits a remote: its name and URL. */
typedef void (*ProllyRemoteVisit) (const char *name, const char *url,
								   void *arg);

/* Visits a ref a root named: its full name and its commit. */
typedef void (*ProllyRootRefVisit) (const char *ref, const ProllyHash *commit,
									void *arg);

/* Visits a remote-tracking branch: the remote, its branch, the commit. */
typedef void (*ProllyTrackingVisit) (const char *remote, const char *branch,
									 const ProllyHash *commit, void *arg);

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
	 * been resolved.  Unless allow_empty, a commit of the head alone that
	 * would hold just what the head does makes no commit and returns false
	 * (the branch's head and a merge in progress stay as they were).
	 */
	bool		(*commit) (const char *message, bool allow_empty,
						   ProllyHash *commit_id);

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
	 * (NULL: none), with the current transaction.  A transaction that set
	 * it, or wrote to the branch, can't commit while the branch's merge has
	 * unresolved conflicts, unless prolly_allow_commit_conflicts().  A
	 * reset ends a merge.
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
	 * store: for reclamation and
	 * transfer.  A commit whose node the visitor declines isn't looked
	 * into (its root and parents are taken as seen), as a tree's node
	 * isn't.
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

	/*
	 * The session branch's constraint violations (ProllyViolation *), and
	 * setting them (NIL: none), with the transaction.  A transaction that
	 * set them, or wrote to the branch, can't commit while it has some, unless
	 * prolly_allow_commit_conflicts(), nor can a commit be made.  A reset
	 * clears them.
	 */
	List	   *(*violations) (void);
	void		(*set_violations) (List *violations);

	/*
	 * working_set() for reading what changed, never to commit or move from:
	 * objects a commit would refuse are left out, their names appended to
	 * *left_out (if not NULL), and with only valid, only that table's rows
	 * are read (the canonicalizer's read_fold()).
	 */
	bool		(*read_working_set) (const char *branch, Oid only,
									 ProllyNodeRef *root, List **left_out);

	/*
	 * Visit every node a commit keeps alive: its own, its root's and its
	 * ancestors' (walk()'s, from one commit).
	 */
	void		(*walk_commit) (const ProllyHash *commit_id,
								ProllyTreeVisitNode visit, void *arg);

	/*
	 * Remotes: set one's URL (NULL: remove it, with its remote-tracking
	 * branches and the upstreams naming it), and visit them by name, with
	 * the transaction as any change to the refs.
	 */
	void		(*remote_set) (const char *name, const char *url);
	void		(*remotes) (ProllyRemoteVisit visit, void *arg);

	/*
	 * Remote-tracking branches (remote/branch: where a fetch last found a
	 * remote's branch): set one (commit NULL: delete it), and visit those of
	 * a remote (NULL: every remote's), by remote and branch.  resolve()
	 * finds them as "remote/branch" (after any branch of that name) and
	 * "remotes/remote/branch".
	 */
	void		(*tracking_set) (const char *remote, const char *branch,
								 const ProllyHash *commit_id);
	void		(*trackings) (const char *remote, ProllyTrackingVisit visit,
							  void *arg);

	/*
	 * A branch's upstream (the remote and its branch a pull or push without
	 * them uses): set it (remote NULL: none), and read it (false if none).
	 */
	void		(*upstream_set) (const char *branch, const char *remote,
								 const char *remote_branch);
	bool		(*upstream) (const char *branch, char **remote,
							 char **remote_branch);

	/*
	 * Move branch name from expected (an empty hash: no head, or no
	 * branch) to commit head (NULL: delete the branch), as a push to this
	 * database moves it: a compare-and-swap, the refs held, false and
	 * nothing changed if the branch isn't at expected.  A branch that
	 * doesn't exist is made, with only its head; one with copies is moved
	 * there, its rows written, refused if it has uncommitted changes
	 * unless force (which drops them, and the claim of the writer whose
	 * they were).  With the transaction.
	 */
	bool		(*branch_move) (const char *name, const ProllyHash *expected,
								const ProllyHash *head, bool force);

	/*
	 * The database's setting called name, as config_set() last set it (false
	 * if it never was), and setting it (value NULL: unset), with the
	 * transaction.  Settings are the database's own: clones and pushes
	 * don't carry them.
	 */
	bool		(*config_get) (const char *name, StringInfo value);
	void		(*config_set) (const char *name, const char *value);

	/*
	 * Open the refs now, if not yet: a transaction that will need them at
	 * its end (prolly_refs_end_hook) opens them before it.
	 */
	void		(*open) (void);

	/*
	 * Commit the session's branch's head with the current transaction's own
	 * row changes (and these sequences as they are now) applied: a commit
	 * whose parent is the head as it is now, whatever the transaction's
	 * snapshot saw; the refs held to transaction end; the branch's baseline
	 * left as it is.  1 if committed, 0 if it would change nothing (no
	 * commit), -1 if it can't be made so (the canonicalizer's fold_rows()
	 * says why: then commit() it as the working set).
	 */
	int			(*commit_rows) (const char *message,
								const ProllyRowChanges *changes, int nchanges,
								const Oid *sequences, int nsequences,
								ProllyHash *commit_id);

	/*
	 * Call branch name newname, with its head, working set, merge in
	 * progress, violations and upstream; refused if another session is on
	 * it, or a branch is called newname, unless force (which deletes that
	 * one).  With the transaction.
	 */
	void		(*branch_rename) (const char *name, const char *newname,
								  bool force);

	/*
	 * The refs a root of the chunk store named (one it was at, which may be
	 * long past): each branch's head and remote-tracking branch's commit, by
	 * full name ("refs/heads/main", "refs/remotes/origin/main"), in no
	 * particular order.  False, visiting nothing, if the store no longer
	 * has that root's chunks.
	 */
	bool		(*root_refs) (const ProllyHash *root, ProllyRootRefVisit visit,
							  void *arg);

	/*
	 * Optional: stashes, working sets set aside, the database's (not a
	 * branch's) in named lists, each newest first.  stash_push() records a
	 * commit of root (the session branch's working set, a canonical root)
	 * whose parent is its head, as the newest of name's list, and sets
	 * *stash to it; stashes() visits every list's entries, by name, then
	 * newest first (n: from 0), with the branch each was pushed from;
	 * stash_select() sets *stash to name's nth (false: it has none), the
	 * refs locked first, so that no other session's push or drop moves it
	 * until the transaction ends; stash_drop() forgets name's nth (false:
	 * it has none); stash_clear() every one of name's.  The walk keeps each one's commit.  With the
	 * transaction; never pushed to a remote.
	 */
	void		(*stash_push) (const char *name, const ProllyNodeRef *root,
							   ProllyHash *stash);
	void		(*stashes) (ProllyStashVisit visit, void *arg);
	bool		(*stash_select) (const char *name, int n, ProllyHash *stash);
	bool		(*stash_drop) (const char *name, int n);
	void		(*stash_clear) (const char *name);

	/*
	 * Optional: what lets a working set's fold be skipped when nothing it
	 * reads has changed (#482).  shmem_size() and shmem_init() as a chunk
	 * store's (prolly_shmem.h).  note_write() is told of a transaction
	 * about to write a branch (its rows, its versioned objects' catalog
	 * rows, or a versioned sequence's value), by every writer, hushed or
	 * not, before the write is seen by anyone else; forget_folds() that
	 * something every fold reads changed under them all (a catalog change
	 * sent to other sessions, chunks a collection dropped), from critical
	 * sections too, so it neither errors nor allocates.  prepared_ended()
	 * is told of a prepared transaction committed or rolled back (once
	 * others see it), which may have written any branch: one recovered at
	 * startup was never told of by note_write(), so a fold by a snapshot
	 * from before its end must not be taken for the working set.  It is
	 * called with interrupts held, so it neither errors nor allocates.
	 */
	Size		(*shmem_size) (void);
	void		(*shmem_init) (void *area);
	void		(*note_write) (Oid branch);
	void		(*forget_folds) (void);
	void		(*prepared_ended) (FullTransactionId xid);

	/*
	 * Optional: the session branch's working set for its definitions
	 * alone, as read_working_set() would read them, its rows not read
	 * (the canonicalizer's read_definitions()): a root to read objects'
	 * definitions from, not rows, unless *rows is set (the working set
	 * was known, rows and all; rows may be NULL).  Where it isn't, *read
	 * (if not NULL) is what the read began at, for publish_definitions().
	 */
	void		(*read_definitions) (ProllyNodeRef *root, List **left_out,
									 bool *rows, void **read);

	/*
	 * Optional: the rows of the session branch's table relid that differ
	 * between its baseline (its data tree for the table put in *data) and
	 * its working set, visited without folding them (the canonicalizer's
	 * table_changes()).  False, having visited nothing, where they can't be
	 * read so, or where the working set is known (read_working_set() takes
	 * it, folding nothing): the caller reads the working set.
	 */
	bool		(*working_changes) (Oid relid, ProllyNodeRef *data,
									ProllyRowChangeVisit visit, void *arg);

	/*
	 * Optional: read_definitions()'s root, begun at read, found to be the
	 * working set (no table's rows changed since the baseline, as
	 * working_changes() read them): published as a fold of it would be,
	 * for later reads to take; complete if it left nothing out.
	 */
	void		(*publish_definitions) (void *read, const ProllyNodeRef *root,
										bool complete);
} ProllyRefsRoutine;

/*
 * Called by the refs as the transaction they were opened in is about to
 * commit (prepare: or be prepared), before the chunk store's own end of
 * transaction, once the refs' own checks have passed: what it records then
 * is the transaction's.
 */
typedef void (*ProllyRefsEndHook) (bool prepare);
extern PGDLLIMPORT ProllyRefsEndHook prolly_refs_end_hook;

#define PROLLY_REFS_CALLBACK_NAMES \
	"branch_create", "branch_delete", "branch_lookup", "checkout", \
	"commit", "resolve", "commit_get", "merge_base", "commit_working_set", \
	"commit_with", "branches", "working_set", "reset", "merge_state", \
	"set_merge_state", "hold", "walk", "checkpoint", "set_head", "violations", \
	"set_violations", "read_working_set", "walk_commit", "remote_set", \
	"remotes", "tracking_set", "trackings", "upstream_set", "upstream", \
	"branch_move", "config_get", "config_set", "open", "commit_rows", \
	"branch_rename", "root_refs", "stash_push", "stashes", "stash_drop", \
	"stash_clear", "stash_select", "shmem_size", "shmem_init", "note_write", \
	"forget_folds", "prepared_ended", "read_definitions", "working_changes", \
	"publish_definitions"
#define PROLLY_REFS_FIRST_CALLBACK branch_create
#define PROLLY_REFS_LAST_CALLBACK publish_definitions

/*
 * For refs implementations: a commit's info by its id (false if there is
 * no such commit), and the best common ancestor of two commits (no other
 * common one descending from it; of two such, the one furthest from the
 * history's roots, then the least id), found with it.  The walks visit a
 * commit at most once each: a's ancestors, b's down to the common ones,
 * and, with several of those, theirs and the history beneath them.
 */
typedef bool (*ProllyCommitLookup) (const ProllyHash *id,
									ProllyCommitInfo *info, void *arg);

extern bool prolly_history_merge_base(const ProllyHash *a, const ProllyHash *b,
									  ProllyCommitLookup lookup, void *lookup_arg,
									  ProllyHash *base);

/*
 * A commit's height: the most commits on a path from it to a root of its
 * history (a root's is 1, and one that isn't found is taken for a root), so
 * each commit's is greater than its parents'.  A commit that records its
 * height (has_height) is taken at its word, and its parents aren't looked
 * up; for one that doesn't, its parents' heights are found first.
 * heights, a table prolly_history_heights() makes, keeps those found, so
 * each commit's height is found once however many are asked for, and
 * lookup is asked once for each commit it finds one of.
 */
extern struct HTAB *prolly_history_heights(void);
extern int64 prolly_history_height(const ProllyHash *id, struct HTAB *heights,
								   ProllyCommitLookup lookup, void *lookup_arg);

#endif							/* PROLLY_REFS_H */
