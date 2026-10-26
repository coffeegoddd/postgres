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
#include "catalog/branch_ops.h"
#include "datatype/timestamp.h"
#include "utils/snapshot.h"

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

	/*
	 * The identity of its root's tables and columns, which their renames
	 * keep (the canonicalizer's identify()): an identity record, palloc'd,
	 * or NULL where every object has its own name's (nothing in its
	 * history was renamed).
	 */
	char	   *identity;
	int			identity_len;
} ProllyCommitInfo;

/*
 * A table in a merge's table identity conflict: the merge kept it as the
 * branch has it, as theirs, under the name, is another relation (another
 * table, made apart from it: dropped and made again, or made on both
 * sides), or it is in an inheritance or partition tree with one that is.
 */
typedef struct ProllyIdentityConflict
{
	char	   *name;			/* here (qualified) */
	char	   *their_name;		/* in theirs */
	Oid			relid;			/* the relation here (InvalidOid: none) */
	Oid			their_relid;	/* theirs (InvalidOid: none, or unrecorded) */
	int			tree;			/* the tree it is kept with: the members of
								 * one are resolved together */
} ProllyIdentityConflict;

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
	int			nschema;
	char	  **schema_tables;	/* in schema conflicts, kept as the branch
								 * has them (names, qualified; palloc'd) */
	char	  **schema_base_tables; /* each one's name in the base */
	char	  **schema_their_tables;	/* and in theirs */
	int			nidentity;
	ProllyIdentityConflict *identity;	/* in table identity conflicts
										 * (palloc'd) */
	char	   *identity_source;	/* the local branch theirs was the head
									 * of, or NULL */
	char	   *identity_saved; /* the privileges of the branch's relations
								 * --theirs dropped, for an abort to give
								 * back (prolly_save_privileges()'s), or
								 * NULL */
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
 * the row is in the table; "exclusion constraint", "check constraint",
 * "not null": the merge didn't write it; "unique index": the row the merge
 * didn't write, or a row of the table it collides with), and what it
 * breaks, as JSON.
 */
typedef struct ProllyViolation
{
	Oid			relid;
	char	   *type;
	char	   *row;
	char	   *info;
} ProllyViolation;

/* Visits one violation; it is good only during the visit. */
typedef void (*ProllyViolationVisit) (const ProllyViolation *violation,
									  void *arg);

/*
 * A tag: a name for a commit, with who made it, when and why, as Dolt's
 * (and git's annotated tags).  A tag is an object of its own, as a commit
 * is, identified by its content's hash, so the same tag has the same id in
 * every database (clone/push/fetch).
 */
typedef struct ProllyTagInfo
{
	ProllyHash	id;				/* the tag object's */
	ProllyHash	commit;
	TimestampTz tagged_at;
	char	   *tagger;			/* palloc'd: "Name <email>" */
	char	   *message;		/* palloc'd */
} ProllyTagInfo;

/*
 * A rebase in progress, as Dolt's: the branch being rebased (its head when
 * the rebase began), the commit its commits are replayed onto, and the plan,
 * steps in rebase order, each an action on one of the branch's commits.
 * The session works on a branch of its own while it rebases; the state is
 * that branch's.
 */
typedef struct ProllyRebaseStep
{
	char	   *order;			/* a numeric(6,2), as text (palloc'd) */
	char	   *action;			/* pick, reword, squash, fixup, drop, edit */
	ProllyHash	commit;
	char	   *message;		/* (palloc'd) */
} ProllyRebaseStep;

typedef struct ProllyRebaseState
{
	char	   *branch;			/* the branch rebased (palloc'd) */
	ProllyHash	head;			/* its head as the rebase began */
	ProllyHash	onto;
	bool		keep_empty;		/* keep a commit that becomes empty */
	char	   *last_step;		/* the order of the step last begun, or NULL */
	int			nsteps;
	ProllyRebaseStep *steps;	/* (palloc'd) */
} ProllyRebaseState;

/* Visits a pin: its name, its commit and the record kept beside it. */
typedef void (*ProllyPinVisit) (const char *name, const ProllyHash *commit,
								const ProllyValue *info, void *arg);

/* Visits a tag: its name, its tag object and that tag's commit. */
typedef void (*ProllyTagVisit) (const char *name, const ProllyHash *tag,
								const ProllyHash *commit, void *arg);

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

/*
 * What a fold of a branch's working set was made at (written_since()):
 * opaque to all but the refs.
 */
typedef struct ProllyFoldMark
{
	Oid			branch;
	uint64		generation;
	uint64		overflow;
	uint64		epoch;
} ProllyFoldMark;

/*
 * A branch written since the background folder last saw it
 * (moved_branches()): the generation it's at, and what its writers since
 * did, for the folder to judge whether a fold is due yet.  rows counts the
 * rows the writers that ended since wrote on it (a multi-row insertion's
 * every row; a writer of more branches than the refs note for one has
 * those past them uncounted); since is when the first of them ended (0:
 * none has), last_ended when the last did; writers, how many are writing
 * it now.
 */
typedef struct ProllyMovedBranch
{
	Oid			branch;
	uint64		generation;
	uint64		rows;
	TimestampTz since;
	TimestampTz last_ended;
	int			writers;
} ProllyMovedBranch;

/*
 * A relation a commit was made from (refs' commit_relations()): its OID,
 * its kind (RELKIND_RELATION, RELKIND_PARTITIONED_TABLE or
 * RELKIND_MATVIEW; or, an object of another kind the commit holds, one of
 * prolly_owned_objects()'s kinds), its owner's OID and name then, and its
 * qualified name then (quoted as needed, as the commit's items name it).
 */
typedef struct ProllyCommitRelation
{
	Oid			relid;
	char		relkind;
	Oid			owner;			/* its owner then */
	char	   *owner_name;		/* that role's name then */
	char	   *name;
} ProllyCommitRelation;

/*
 * A commit a branch's pages are kept as of: each table's relations as
 * branch operations' keep() kept them (the table's own first), the
 * branch's baseline then (len 0: none), and the transaction that kept
 * them, which saw every writer of the pages end.
 */
typedef struct ProllyKeptTable
{
	const char *name;			/* qualified, as tables() names it */
	int			nrels;
	const BranchKeptRelation *rels;
} ProllyKeptTable;

typedef struct ProllyKeptCommit
{
	ProllyHash	commit;
	ProllyHash	baseline;
	FullTransactionId kept_by;
	int			ntables;
	const ProllyKeptTable *tables;
} ProllyKeptCommit;

typedef void (*ProllyKeptCommitVisit) (const ProllyKeptCommit *kept, void *arg);
/* A branch keeping pages as of commit (kept_holders()). */
typedef void (*ProllyKeptHolderVisit) (const char *name, const ProllyHash *commit,
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
	 * been resolved.  Unless allow_empty, a commit of the head alone that
	 * would hold just what the head does makes no commit and returns false
	 * (the branch's head and a merge in progress stay as they were).
	 */
	bool		(*commit) (const char *message, bool allow_empty,
						   ProllyHash *commit_id);

	/*
	 * Resolve a branch name, tag name, commit id or HEAD (the session
	 * branch's head commit), each optionally followed by ancestry steps as
	 * in Dolt and Git (~n: the nth first-parent ancestor; ^n: the nth
	 * parent), to a commit id.  A name is a branch's before a tag's, and a
	 * tag's before a remote-tracking branch's, as in Dolt; "refs/heads/x"
	 * and "refs/tags/x" name only a branch or only a tag.
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
	 * Hold the refs as hold() does, without the snapshot: for a caller that
	 * reads only the refs themselves, whatever its isolation level.  Taken
	 * in a subtransaction, they are let go again if it rolls back.
	 */
	void		(*lock) (void);

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
	 * The session branch's violations visited in the order they were
	 * added, those of tables since dropped too, without holding them all;
	 * and violations (n of them) added after them, with the transaction,
	 * the others neither read nor written again.
	 */
	void		(*visit_violations) (ProllyViolationVisit visit, void *arg);
	void		(*add_violations) (ProllyViolation *const *violations, int n);

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
	 * long past): each branch's head, remote-tracking branch's commit and
	 * tag's commit, by full name ("refs/heads/main",
	 * "refs/remotes/origin/main", "refs/tags/v1"), in no particular order.  False, visiting nothing, if the store no longer
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

	/*
	 * Optional: the current transaction's own row changes to these tables
	 * folded into the session branch's baseline as it commits (the
	 * canonicalizer's advance_own()), under the refs: false, nothing moved,
	 * if none could be, the changes left for the branch's next fold.
	 */
	bool		(*fold_own) (const ProllyRowChanges *changes, int nchanges);

	/*
	 * Optional: checkpoint(), but folded before the refs are taken, so they
	 * are held only to set the baseline, not through the fold, and taken
	 * only if free (never waited for): false, nothing set, if another holds
	 * them or the branch's head or baseline moved meanwhile, and then the
	 * caller must roll its transaction back (the fold consumed the change
	 * finder's record of what it folded).  The fold waits for no relation's
	 * lock either: one not free at once fails it with
	 * ERRCODE_LOCK_NOT_AVAILABLE, RelationLockRefused set (lmgr.h), which
	 * the caller rolls back the same way.
	 */
	bool		(*checkpoint_unless_moved) (ProllyNodeRef *root);

	/*
	 * Optional, together, for a background folder (prolly_folder.c), from
	 * shared memory, needing no database: the databases with a branch
	 * written since the folder last saw it (at most max, their count); the
	 * current database's such branches, each with the generation it's at
	 * and what was written since (ProllyMovedBranch);
	 * that the folder saw branch at generation (written again since: still
	 * moved); and that it can't fold any branch of database (gone, or
	 * accepting no connections), so that none of its branches counts as
	 * moved until written again.
	 */
	int			(*moved_databases) (Oid *databases, int max);
	int			(*moved_branches) (ProllyMovedBranch *branches, int max);
	void		(*saw_branch) (Oid branch, uint64 generation);
	void		(*saw_database) (Oid database);

	/*
	 * Optional, together: which tables' rows may have changed since a fold
	 * of a branch, for a canonicalizer that works out again only what
	 * changed.  note_relation() is told, as note_write() is (and after
	 * it), of each relation whose rows a write to branch changes, and of
	 * how many rows the write changed (moved_branches() counts them).
	 * fold_mark() marks a fold by snap of branch, before it reads anything:
	 * false where nothing could be known of it later (in recovery, while a
	 * transaction is prepared, after the transaction's catalog changes, or
	 * by a snapshot that doesn't see every writer of the branch that ended).
	 * written_since() puts in *relids (*nrelids of them, palloc'd) every
	 * relation whose rows a transaction that ended since mark, or this
	 * one, wrote on mark's branch, as snap reads them: false where that
	 * isn't known (a catalog change or collection since, a writer that
	 * wrote too many relations or found no room to be told of, a prepared
	 * transaction ended, the branch forgotten, a snapshot that doesn't see
	 * every writer that ended, or another writer of the branch not ended
	 * yet: others see its commit before its relations are kept), and then
	 * everything may have changed.
	 * Catalog changes and sequences' values aren't relations' rows: a
	 * catalog change moves what fold_mark() takes, and a sequence is read
	 * each time.
	 */
	void		(*note_relation) (Oid branch, Oid relid, int nrows);
	bool		(*fold_mark) (Oid branch, Snapshot snap, ProllyFoldMark *mark);
	bool		(*written_since) (const ProllyFoldMark *mark, Snapshot snap,
								  Oid **relids, int *nrelids);

	/*
	 * Optional: visit each table of the session branch whose next fold
	 * would read it in full, with why (the canonicalizer's pending_full()
	 * from the branch's baseline).
	 */
	void		(*pending_full_folds) (ProllyPendingFullVisit visit, void *arg);

	/*
	 * Optional, together: the identity record of the session branch's
	 * working set (with only valid, of that table alone), as the commit it
	 * would make records it; and the session branch's working set made
	 * what identity (an identity record) describes, by name (a move to a
	 * root whose identity it is), for later renames to be told by.
	 */
	void		(*working_identity) (Oid only, StringInfo identity);
	void		(*anchor_identity) (const ProllyValue *identity);

	/*
	 * Optional, together: tags.  tag_write() writes a tag object of commit,
	 * by tagger, with message, dated now, and sets *tag to its id; it names
	 * nothing.  tag_get() reads a tag object (false: tag isn't one).
	 * tag_lookup() finds the tag called name (false: none).  tag_set()
	 * calls name tag, a tag object the store has (NULL: deletes the tag), as
	 * a compare-and-swap: expected NULL takes whatever tag of that name
	 * there is, an empty hash only none; false, nothing changed, if it isn't
	 * so.  A new tag's name follows a branch's rules, and isn't a commit's
	 * id.  tags() visits every tag, by name.  Changes are with the
	 * transaction, as any change to the refs.  The walk keeps each tag
	 * object and its commit, and walk_commit() given a tag object walks it
	 * and then its commit.
	 */
	void		(*tag_write) (const ProllyHash *commit, const char *tagger,
							  const char *message, ProllyHash *tag);
	bool		(*tag_get) (const ProllyHash *tag, ProllyTagInfo *info);
	bool		(*tag_lookup) (const char *name, ProllyHash *tag);
	bool		(*tag_set) (const char *name, const ProllyHash *expected,
							const ProllyHash *tag);
	void		(*tags) (ProllyTagVisit visit, void *arg);

	/*
	 * Optional, together: the session branch's rebase in progress (false:
	 * none), and setting it (NULL: none), with the transaction.  It goes
	 * with its branch when the branch is deleted, and is renamed with it.
	 * The walk keeps the commits it names.
	 */
	bool		(*rebase_state) (ProllyRebaseState *state);
	void		(*set_rebase_state) (const ProllyRebaseState *state);

	/*
	 * Write a commit of a canonical state the caller already holds: root,
	 * with dependencies (what its objects need from outside, as
	 * fold_dependencies() records it), these parents, author and message,
	 * and identity (its "identity" item; NULL or empty: none), dated as
	 * commit_with() dates one.  Nothing is folded, no catalog read for it,
	 * and no branch moved: the caller names the commit (branch_move(),
	 * set_head()) in the same transaction.  Until a ref names it nothing
	 * holds it, and a chunk-store collection after the transaction ends may
	 * drop it as unreachable.  commit_with() folds, then writes its commit
	 * with this.
	 */
	void		(*commit_root) (const ProllyHash *root,
								const ProllyHash *dependencies,
								const ProllyHash *parents, int nparents,
								const char *author, const char *message,
								const StringInfoData *identity,
								ProllyHash *commit_id);

	/*
	 * Optional: visit the commits branch name's pages are kept as of (see
	 * prolly.kept_commits), newest first; not those retired.  What is
	 * visited is the refs' as this transaction reads them.
	 */
	void		(*kept_commits) (const char *name, ProllyKeptCommitVisit visit,
								 void *arg);

	/*
	 * Optional: visit the branches keeping pages as of commit (data NULL),
	 * or as of a commit one of whose kept tables has the data tree data,
	 * each with that commit (retired ones too: kept_commits()
	 * says which still are).  A seek of an index: the work is in those,
	 * however many branches there are.
	 */
	void		(*kept_holders) (const ProllyHash *commit, const ProllyHash *data,
								 ProllyKeptHolderVisit visit, void *arg);

	/*
	 * Optional, together: pins.  A pin keeps a commit, and everything it
	 * reaches, from collection under a name, for a reader outside the refs
	 * that names the commit (a snapshot mount), with the nkeep trees keep
	 * names (the reader's own, as a mount's index trees), which must be in
	 * the store; info is the reader's, kept beside it.  pin_set() sets
	 * name's pin (commit NULL: removes it), holding the refs; pin_get()
	 * reads one (false: none), appending its info; pins() visits each, by
	 * name.  Changes are with the transaction, as any change to the refs.
	 * Pins are the database's own: transfers never send them.
	 */
	void		(*pin_set) (const char *name, const ProllyHash *commit,
							const ProllyHash *keep, int nkeep,
							const ProllyValue *info);
	bool		(*pin_get) (const char *name, ProllyHash *commit,
							StringInfo info);
	void		(*pins) (ProllyPinVisit visit, void *arg);

	/*
	 * Optional: the relation (*relid) that branch's records of its head's
	 * relations (its tables, and its other relations with privileges of
	 * their own), made by its last commit, list under the qualified name
	 * name (prolly_record_relation_name()'s), and that commit (*at); false
	 * if they list none so, or there is none.  A relation of the branch
	 * now of that name and another OID was made since: it isn't the one
	 * the commit's object was made from.
	 */
	bool		(*recorded_relid) (const char *branch, const char *name,
								   Oid *relid, ProllyHash *at);

	/*
	 * Optional: the role that made branch name, which owns it; false if
	 * none is recorded (main, the database owner's, never has one).
	 */
	bool		(*branch_creator) (const char *name, Oid *role);

	/*
	 * Optional: record role (InvalidOid: none) as the one that made branch
	 * name, as a branch made again in its own place keeps its owner.
	 */
	void		(*branch_set_creator) (const char *name, Oid role);

	/*
	 * Optional: the role that made tag name here (false: none recorded),
	 * and the role that pushed name's nth stash (false: it has none).
	 */
	bool		(*tag_creator) (const char *name, Oid *role);
	bool		(*stash_maker) (const char *name, int n, Oid *role);

	/*
	 * Optional: record role as the one that made tag name here (tag_set()
	 * records none: a tag fetched or pushed is no one's).
	 */
	void		(*tag_set_creator) (const char *name, Oid role);

	/*
	 * Optional: the relations commit was made from, as this database made
	 * it: *relations, a ProllyCommitRelation for each prolly table,
	 * partitioned table and materialized view, and each other object it
	 * holds (prolly_owned_objects()) (palloc'd); false for a commit made elsewhere
	 * (fetched, cloned, pushed here) or before they were kept.  A history read takes a
	 * version for the session's table only along commits made from the
	 * table's relation.
	 */
	bool		(*commit_relations) (const ProllyHash *commit,
									 List **relations);

	/*
	 * Optional, together: each maker the refs record (a branch's, a tag's,
	 * a stash's) is a shared dependency of its role, as an object's owner
	 * is (pg_shdepend: classId RefsBranchClassId, RefsTagClassId or
	 * RefsStashClassId, and an id the record keeps, newMadeObjectId()'s),
	 * recorded, moved and dropped with the record, by the transaction.
	 * made_object() finds the object of kind classId whose record has id,
	 * appending its name (a stash's, its list's) and setting *n to a
	 * stash's place in its list; false if none has.  stash_set_maker()
	 * records role as the one that pushed name's nth stash.
	 */
	bool		(*made_object) (Oid classId, Oid id, StringInfo name, int *n);
	void		(*stash_set_maker) (const char *name, int n, Oid role);

	/*
	 * Optional: the session branch's working set as the last fold of it
	 * known made it, though writers may have ended since, into *root, and
	 * a mark (fold_mark()'s) as of when it was known into *mark: the
	 * working set is that root but for the relations written_since(mark)
	 * says were written since.  Only a fold that left nothing out, from the
	 * branch's baseline now, with no catalog change or collection since,
	 * as snap sees the branch's writers; false where there's none such.
	 * Its tree is in the store, whichever backend made it.
	 */
	bool		(*last_fold) (Snapshot snap, ProllyNodeRef *root,
							  ProllyFoldMark *mark);

	/*
	 * Optional: visit the branches whose names begin with prefix, as
	 * branches() visits them: the work is in those, however many other
	 * branches there are.
	 */
	void		(*branches_named) (const char *prefix, ProllyBranchVisit visit,
								   void *arg);

	/*
	 * Optional: forget the relation records of commits the chunk store no
	 * longer has, as a step after a collection (true if any was), and the
	 * entries records share (their routines and types) that no record left
	 * names.  A record isn't a reference the collection follows, so it
	 * would outlive its commit.
	 */
	bool		(*forget_records) (void);
} ProllyRefsRoutine;

/*
 * Called by the refs as the transaction they were opened in is about to
 * commit (prepare: or be prepared), before the chunk store's own end of
 * transaction, once the refs' own checks have passed: what it records then
 * is the transaction's.
 */
typedef void (*ProllyRefsEndHook) (bool prepare);
extern PGDLLIMPORT ProllyRefsEndHook prolly_refs_end_hook;

/*
 * prolly.kept_commits: how many commits each branch's pages are kept as of,
 * the last it was at (0: none), for a move back to one to give the pages
 * back rather than write rows.
 */
extern PGDLLIMPORT int prolly_kept_commits;

/*
 * prolly.restore_min_changes: how many of a table's rows must differ from
 * a kept commit's, since a baseline holding them, before a move back to it
 * gives the table its pages back rather than write those rows (0: always).
 */
extern PGDLLIMPORT int prolly_restore_min_changes;

/*
 * While set, the tables (qualified names) a commit being made changed in the
 * working set before the move it commits: recorded made only from a
 * relation of the class its first parent's record lists.
 */
extern PGDLLIMPORT List *prolly_commit_local_tables;

/*
 * A relation's qualified name as the refs' records of relations write it:
 * quoted as needed with quote_all_identifiers off, in any session.
 */
extern char *prolly_record_relation_name(const char *nspname,
										 const char *relname);

#define PROLLY_REFS_CALLBACK_NAMES \
	"branch_create", "branch_delete", "branch_lookup", "checkout", \
	"commit", "resolve", "commit_get", "merge_base", "commit_working_set", \
	"commit_with", "branches", "working_set", "reset", "merge_state", \
	"set_merge_state", "hold", "lock", "walk", "checkpoint", "set_head", "violations", \
	"set_violations", "visit_violations", "add_violations", \
	"read_working_set", "walk_commit", "remote_set", \
	"remotes", "tracking_set", "trackings", "upstream_set", "upstream", \
	"branch_move", "config_get", "config_set", "open", "commit_rows", \
	"branch_rename", "root_refs", "stash_push", "stashes", "stash_drop", \
	"stash_clear", "stash_select", "shmem_size", "shmem_init", "note_write", \
	"forget_folds", "prepared_ended", "read_definitions", "working_changes", \
	"publish_definitions", "fold_own", "checkpoint_unless_moved", \
	"moved_databases", "moved_branches", "saw_branch", "saw_database", \
	"note_relation", "fold_mark", "written_since", "pending_full_folds", \
	"working_identity", "anchor_identity", "tag_write", "tag_get", \
	"tag_lookup", "tag_set", "tags", \
	"rebase_state", "set_rebase_state", "commit_root", "kept_commits", "kept_holders", \
	"pin_set", "pin_get", "pins", "recorded_relid", "branch_creator", \
	"branch_set_creator", "tag_creator", "stash_maker", "tag_set_creator", \
	"commit_relations", \
	"made_object", "stash_set_maker", "last_fold", "branches_named", \
	"forget_records"
#define PROLLY_REFS_FIRST_CALLBACK branch_create
#define PROLLY_REFS_LAST_CALLBACK forget_records

/*
 * For refs implementations: a commit's info by its id (false if there is
 * no such commit), and the best common ancestor of two commits (no other
 * common one descending from it; of two such, the one furthest from the
 * history's roots, then the least id), found with it.  Where the commits
 * record their heights, it reads the commits above the best common ones
 * and their parents, and holds marks for those alone, however long the
 * history beneath them (criss-cross merges, and a side reaching beneath
 * the base through an older merge, included).  Otherwise
 * the walks visit a commit at most once each: a's ancestors, b's down to
 * the common ones, and, with several of those, theirs and the history
 * beneath them.
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
