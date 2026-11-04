/*-------------------------------------------------------------------------
 *
 * vc.h
 *	  Version control over prolly trees for one schema.
 *
 * A schema's state is a root value: a prolly map from table name to the
 * table's schema chunk (its DDL) and the root of its row map.  Commits name
 * a root value and their parents.  Branches each keep their own head and
 * working root value.
 *
 * A repository's state is in references (prolly/engine/vc_refs.h), not in
 * one chunk:
 *	vc/branch/<name>	the branch's chunk: its head, working root and
 *						merge state, which it holds
 *	vc/commit/<hash>	a commit the window's history keeps, which it holds
 *						(a commit's parents are weak references: its row is
 *						what keeps it)
 *	vc/repo				the repository's chunk: where each commit outside
 *						the window is (a map), the next segment's id, and
 *						the repository's name
 *	vc/segment/<id>		commits severed from the window together, which a
 *						local segment holds (an exported one's archive does)
 * A commit writes its own reference and moves its branch's, and nothing
 * else, so commits on different branches change different rows.  A VcRepo
 * is a backend's view of them: the branch it has checked out is its own,
 * and each call reads the references it needs, so opening one reads main's
 * reference, its chunk and head commit, however long the history.
 *
 * The window's own calls (sever, and a segment's export, discard and
 * attach) lock vc/repo for update, and take turns; calls that point a
 * branch at a commit they did not just write (a branch made, a reset, a
 * merge) lock it for share, so that a sever cannot cut a commit they are
 * moving a branch to.  A commit, and a working set's change, take only
 * their branch's reference.  vc/repo is locked before a branch's: a
 * transaction that commits and then severs or resets takes them the other
 * way, which Postgres finds as a deadlock.  The window's calls need READ
 * COMMITTED: each reads the branches after its lock, with a snapshot of
 * its own, and under a transaction's snapshot would not see a reset that
 * committed while it waited (they refuse to run so).
 *
 * Calls that change a branch lock its reference first (for update), and
 * read it after, so two that change one branch take turns: two commits on
 * one branch both land, one after the other.  A change of the working set
 * made from what it was (an edit: read, change, write back) reads it with
 * vc_working_lock, which locks the branch as a commit does, so that two
 * such edits take turns too and neither is lost; vc_set_working writes the
 * root it is given, whatever the working set was meanwhile.  The one chunk
 * commits on different branches can both write is a commit whose hash
 * another branch's commit has: the same parents, root, time and message
 * (two merges of one branch into two at one head, at one time: in one
 * microsecond, or with the clock behind the parents', which puts a commit
 * just after them); the second waits for the first's transaction there.
 * Errors of the caller's (an unknown or misnamed branch, uncommitted
 * changes) are
 * VC_ERR, and a name is any of 1 to NAMEDATALEN - 1 bytes; history a
 * call cannot reach is VC_ERR_SEVERED or VC_ERR_OUTSIDE_WINDOW, the commit
 * in ctx.err_hash; damage, of a tree or a commit, is raised.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/engine/vc.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_ENGINE_VC_H
#define PROLLY_ENGINE_VC_H

#include "prolly/engine/prolly.h"
#include "prolly/engine/vc_refs.h"

#define VC_MAX_PARENTS 8

/* (below the tree's codes, prolly/engine/prolly.h's) */
#define VC_OK 0
#define VC_ERR (-10)			/* bad argument or state */
#define VC_ERR_SEVERED (-11)	/* needs a commit cut from local history */
#define VC_ERR_OUTSIDE_WINDOW (-12)	/* stopped at the window's edge */
#define VC_ERR_UNRELATED (-13)	/* the histories share no commit */
#define VC_ERR_DETACHED (-14)	/* the sender's history is cut before it attaches */
#define VC_ERR_REWRITTEN (-15)	/* remotes, with a repository that rewrites its history */

/* Stop at the window's edge: refuse any commit the window does not hold. */
#define VC_WINDOW_ONLY 0x1

typedef struct VcTable
{
	PHash		schema;
	PHash		data;
} VcTable;

typedef struct VcTableChange
{
	const uint8 *name;
	size_t		nlen;
	bool		has_from;
	bool		has_to;
	VcTable		from;
	VcTable		to;
} VcTableChange;

typedef void (*VcTableChangeFn) (void *arg, const VcTableChange *c);

/*
 * A commit as loaded: its fields, its message in its chunk's bytes, which
 * it owns (vc_commit_release frees them).
 */
typedef struct VcCommit
{
	PHash		hash;
	PHash		root;
	uint64		generation;		/* 1 + the largest parent generation */
	uint64		bytes;			/* change bytes: its own + the largest parent's */
	uint64		time;			/* microseconds; never before a parent's */
	uint32		nparents;
	PHash		parents[VC_MAX_PARENTS];
	uint64		parent_gen[VC_MAX_PARENTS];
	uint64		parent_bytes[VC_MAX_PARENTS];
	uint64		parent_time[VC_MAX_PARENTS];
	const char *msg;			/* not NUL-terminated */
	size_t		msglen;
	uint8	   *chunk;			/* the commit's bytes, msg in them */
} VcCommit;

typedef enum VcConflictKind
{
	VC_CONFLICT_ROW,
	VC_CONFLICT_SCHEMA,
	VC_CONFLICT_TABLE			/* dropped on one side, changed on the other */
} VcConflictKind;

typedef struct VcConflict
{
	VcConflictKind kind;
	char	   *table;
	uint8	   *key;			/* row conflicts only */
	size_t		klen;
} VcConflict;

typedef enum VcMergeStatus
{
	VC_MERGE_UP_TO_DATE,
	VC_MERGE_FAST_FORWARD,
	VC_MERGE_COMMITTED,
	VC_MERGE_CONFLICTS
} VcMergeStatus;

typedef struct VcMergeResult
{
	VcMergeStatus status;
	PHash		commit;
	VcConflict *conflicts;
	size_t		nconflicts;
} VcMergeResult;

/* A branch as its reference names it (and its head's root value). */
typedef struct VcBranch
{
	char	   *name;
	PHash		head;
	PHash		head_root;
	PHash		working;
	bool		merging;
	PHash		merge_head;
} VcBranch;

/*
 * What a sever does with the history it cuts, a mode of the repository's
 * (its state's, so every backend's).
 *	KEEP	commits keep their hashes: what is cut becomes a segment, to
 *			export, discard or attach again, and the kept commits still
 *			name the parents they lost.  Remotes work across cuts.
 *	REWRITE	what is cut is let go at once, and every kept commit with cut
 *			history beneath it is written again without it: the oldest
 *			kept commits become roots, and their descendants get new
 *			hashes.  Nothing of the cut history is kept.  Hashes change at
 *			every sever, so a repository in this mode has no remotes.
 */
typedef enum VcHistoryMode
{
	VC_HISTORY_KEEP = 0,
	VC_HISTORY_REWRITE = 1
} VcHistoryMode;

/* Told, for each commit a rewriting sever replaced, the old and new hash. */
typedef void (*VcRewriteFn) (void *arg, const PHash *old, const PHash *now);

typedef struct VcRepo
{
	ProllyCtx	ctx;
	VcRefs	   *refs;
	char	   *cur;			/* the branch checked out: this backend's */
	uint64		(*now) (void *arg); /* microseconds; NULL: the server's clock */
	void	   *now_arg;
	VcRewriteFn rewrite_fn;		/* this backend's: told of each rewrite */
	void	   *rewrite_arg;
} VcRepo;

typedef void (*VcLogFn) (void *arg, const VcCommit *c);
typedef void (*VcBranchFn) (void *arg, const VcBranch *b);

/* Root values. */
extern PHash vc_root_empty(ProllyCtx *ctx);
extern bool vc_table_get(ProllyCtx *ctx, const PHash *rv, const char *name,
						 VcTable *out);
/* A root value's entry from its value; raises an error if it is none. */
extern void vc_table_decode(const uint8 *val, size_t len, VcTable *out);
/* Sets an entry whole, new or in place of what it was. */
extern void vc_table_put(ProllyCtx *ctx, const PHash *rv, const char *name,
						 const void *schema, size_t slen, const PHash *data, PHash *out);
extern int	vc_table_create(ProllyCtx *ctx, const PHash *rv, const char *name,
							const void *schema, size_t slen, PHash *out);
extern int	vc_table_alter(ProllyCtx *ctx, const PHash *rv, const char *name,
						   const void *schema, size_t slen, PHash *out);
extern int	vc_table_drop(ProllyCtx *ctx, const PHash *rv, const char *name,
						  PHash *out);
extern int	vc_table_edit(ProllyCtx *ctx, const PHash *rv, const char *name,
						  const ProllyEdit *edits, size_t n, PHash *out);
extern int	vc_table_set_data(ProllyCtx *ctx, const PHash *rv, const char *name,
							  const PHash *data, PHash *out);
/* A schema's DDL, palloc'd; NULL if the chunk is no schema. */
extern uint8 *vc_schema_get(ProllyCtx *ctx, const PHash *schema, size_t *len);
extern void vc_diff_tables(ProllyCtx *ctx, const PHash *from, const PHash *to,
						   VcTableChangeFn fn, void *arg);
extern void vc_merge_roots(ProllyCtx *ctx, const PHash *base, const PHash *ours,
						   const PHash *theirs, PHash *out,
						   VcConflict **conflicts, size_t *nconflicts);
extern void vc_conflicts_free(VcConflict *c, size_t n);

/* Commits. */
extern bool vc_commit_load(ProllyCtx *ctx, const PHash *h, VcCommit *out);
extern void vc_commit_release(VcCommit *c);
extern PHash vc_commit_write(ProllyCtx *ctx, const PHash *root,
							 const PHash *parents, uint32 nparents,
							 uint64 time, const char *msg);
/* 1 with the base, 0 if none, VC_ERR_SEVERED if the walk crosses a cut. */
extern int	vc_merge_base(ProllyCtx *ctx, const PHash *a, const PHash *b,
						  PHash *out);

/*
 * Repositories and branches.  vc_repo_open views the repository the
 * references hold, with "main" checked out; one without "main" (none yet:
 * "main" is never deleted) is made, an "init" commit on "main".
 */
extern void vc_repo_open(VcRepo *r, ChunkStore *cs, VcRefs *refs,
						 const ChunkPolicy *policy);
/* vc_repo_open with the clock commits read, the init's too (for tests). */
extern void vc_repo_open_clock(VcRepo *r, ChunkStore *cs, VcRefs *refs,
							   const ChunkPolicy *policy,
							   uint64 (*now) (void *arg), void *now_arg);
extern void vc_repo_free(VcRepo *r);
/* The branch, false if there is none; vc_branch_free frees out's name. */
extern bool vc_branch_get(VcRepo *r, const char *name, VcBranch *out);
extern void vc_branch_free(VcBranch *b);
/* Every branch, in name order. */
extern void vc_branch_list(VcRepo *r, VcBranchFn fn, void *arg);
/* The branch checked out. */
extern void vc_head(VcRepo *r, VcBranch *out);
extern PHash vc_working(VcRepo *r);
/* The working root, its branch locked until the transaction ends: for an edit. */
extern PHash vc_working_lock(VcRepo *r);
extern void vc_set_working(VcRepo *r, const PHash *rv);
/* A new branch at commit (NULL: the head checked out). */
extern int	vc_branch_create(VcRepo *r, const char *name, const PHash *commit);
/*
 * Deletes a branch other than the one checked out, and other than "main";
 * its working set goes.
 */
extern int	vc_branch_delete(VcRepo *r, const char *name);
extern int	vc_checkout(VcRepo *r, const char *name);
extern PHash vc_commit(VcRepo *r, const char *msg);

/*
 * Commits root (held by the caller until this returns) on the branch
 * checked out.  The working root is left alone, so changes it already has
 * beyond root stay uncommitted: a commit can capture the state as of a
 * point while the working set keeps moving.
 */
extern PHash vc_commit_root(VcRepo *r, const PHash *root, const char *msg);
extern int	vc_reset_hard(VcRepo *r, const PHash *commit);

/*
 * Moves the branch's head to commit and leaves its working root alone (a
 * pending merge is let go of): what the working root has beyond commit is
 * then uncommitted.  As every call that changes a branch, it locks the
 * branch before it reads it: a commit made meanwhile is not lost to a
 * head read before it.
 */
extern int	vc_reset_soft(VcRepo *r, const PHash *commit);
extern int	vc_merge(VcRepo *r, const char *branch, VcMergeResult *res);
extern void vc_merge_result_free(VcMergeResult *res);

/*
 * History, merge base and diff by commit.  They read as far as local data
 * goes (VC_ERR_SEVERED at a cut), or with VC_WINDOW_ONLY stop at the
 * window's edge (VC_ERR_OUTSIDE_WINDOW).  The failing commit is in
 * r->ctx.err_hash.  vc_log follows first parents, at most max commits.
 * In window-only mode a merge base past the edge is VC_ERR_OUTSIDE_WINDOW
 * too, with the base in err_hash.
 */
extern int	vc_log(VcRepo *r, const PHash *start, size_t max, unsigned flags,
				   VcLogFn fn, void *arg);
extern int	vc_merge_base_in(VcRepo *r, const PHash *a, const PHash *b,
							 unsigned flags, PHash *out);
extern int	vc_diff_commits(VcRepo *r, const PHash *from, const PHash *to,
							unsigned flags, VcTableChangeFn fn, void *arg);
/* Whether the window's history keeps the commit (it has a reference). */
extern bool vc_in_window(VcRepo *r, const PHash *commit);

/*
 * The window: every branch head (and pending merge head), each pinned
 * commit, and each commit an ancestor of a head that the head's floors keep
 * and its ceilings allow.  Measured from a head, a commit's
 *	commits is how many generations it lies back (the head is 1),
 *	bytes is the change bytes of it and every commit after it up to the head
 *		(a commit's change bytes: the chunks its root has that its first
 *		parent's root lacks),
 *	age is how long ago it was committed.
 * A floor keeps a commit while commits <= depth, or bytes from the commits
 * after it < keep_bytes, or age < keep_seconds: any floor keeps it, and with
 * no floor set everything is kept.  A ceiling cuts it once commits >
 * max_commits, or bytes > max_bytes, or age >= max_seconds: any ceiling
 * cuts it, and a ceiling wins over a floor.  Zero leaves a limit unset; a
 * window with none set is refused.  Heads are always kept.  A pinned commit
 * must be held: one cut already (in a segment, or discarded, its chunks
 * freed or not) is refused, VC_ERR_SEVERED.
 *
 * Low marks.  Without them the window slides: each sever cuts a head's
 * history back to exactly the window above (its high mark).  With any low
 * mark set, a head's window grows until it reaches its high mark, which is
 * when the window would let go of a commit still held; the sever then cuts
 * that head's history back to its low mark, the window with the low marks
 * as further ceilings, and it grows again.  low_commits = 1 keeps only the
 * head (a tumbling window, history in batches); low marks at or past the
 * ceilings slide as before.  A head short of its high mark keeps its whole
 * window, so each branch fills and empties on its own.  A low mark needs a
 * high mark: low marks alone are refused.
 */
typedef struct VcWindow
{
	uint32		depth;			/* floor: generations */
	const PHash *pinned;
	size_t		npinned;
	uint64		keep_bytes;		/* floor */
	uint64		keep_seconds;	/* floor */
	uint64		max_commits;	/* ceiling */
	uint64		max_bytes;		/* ceiling */
	uint64		max_seconds;	/* ceiling */
	uint64		low_commits;	/* low mark */
	uint64		low_bytes;		/* low mark */
	uint64		low_seconds;	/* low mark */
	uint64		max_cut;		/* the most one sever cuts (0: VC_SEVER_MAX) */
} VcWindow;

#define VC_NO_SEGMENT PG_UINT32_MAX

/* A segment as its row names it: local, or exported to an archive. */
typedef struct VcSegment
{
	uint32		id;
	bool		exported;
	PHash	   *commits;
	size_t		n;
	char	   *archive_name;	/* exported: where */
	PHash		manifest;		/* exported: holds it there */
	/* exported: the members' parents that are not members */
	PHash	   *bounds;
	size_t		nbounds;
} VcSegment;

/*
 * The most commits one sever cuts, its segment's chunk bounded with them:
 * the rest wait for the next.
 */
#define VC_SEVER_MAX 1000000

/*
 * Anything chunks can be copied to or from by hash: an archive, a remote's
 * store, another repository's.  Everything copied is checked against its
 * name, so any will do.  The name is what a segment records of it.
 */
typedef struct VcSource
{
	const char *name;
	ChunkStore *cs;
} VcSource;

/* What an archive says about a segment exported to it. */
typedef struct VcManifest
{
	char	   *repo;			/* the repository that exported it */
	PHash	   *members;
	uint64	   *gens;
	size_t		n;
	PHash	   *bounds;			/* parents of members that are not members */
	size_t		nb;
} VcManifest;

typedef enum VcLocation
{
	VC_LOC_UNKNOWN,
	VC_LOC_WINDOW,				/* held by the window's history */
	VC_LOC_SEGMENT,				/* in a local, detached segment */
	VC_LOC_EXPORTED,			/* in a segment moved to an archive */
	VC_LOC_DISCARDED,			/* a parent of a commit held, discarded here */
	VC_LOC_BOUNDARY				/* named as a parent; never had locally */
} VcLocation;

/*
 * Severing.  vc_sever moves every commit the window no longer covers into
 * a new local segment, its id in *seg (or VC_NO_SEGMENT): only holds move,
 * from the commits' references to the segment's, and no chunk is copied
 * or freed.  A segment can then be exported (copied whole to an archive,
 * then let go of here), discarded, or attached again.
 *
 * A discarded segment's commits are forgotten, so the repository's state
 * stays the size of what it holds however much is cut.  What it still
 * knows of them is what the commits it holds name: their parents, which
 * vc_locate reports as discarded.  Deeper discarded commits are unknown
 * again.  (Those parents are found from what is held when asked, not
 * kept: nothing to keep right as segments go in any order.)
 *
 * An archive's roots change with the segment's row: it must be as
 * transactional as the repository's store (its root given up by a discard
 * that then aborts would free what the segment's row still names), or the
 * call is VC_ERR; its name must be the one the segment was exported under.
 * A segment's window walks keep what each head reached apart (a history
 * many heads share is walked once a head).
 */
extern int	vc_sever(VcRepo *r, const VcWindow *w, uint32 *seg);
extern int	vc_segment_export(VcRepo *r, uint32 seg, const VcSource *archive);
/* archive: the one an exported segment is in (NULL for a local one). */
extern int	vc_segment_discard(VcRepo *r, uint32 seg, const VcSource *archive);
extern int	vc_segment_attach(VcRepo *r, uint32 seg, const VcSource *archive);
/* The segment, false if it has no row (discarded, attached, or none). */
extern bool vc_segment_get(VcRepo *r, uint32 seg, VcSegment *out);
extern void vc_segment_free(VcSegment *seg);
/*
 * Where a commit is: O(log n) for one the repository holds (its location
 * map); one it does not is looked for among the parents of what it holds,
 * which reads every commit of the window and of its local segments.
 */
extern VcLocation vc_locate(VcRepo *r, const PHash *commit, uint32 *seg);

/* Every commit the repository knows of and where it is (O(history); for tests). */
typedef void (*VcLocFn) (void *arg, const PHash *commit, VcLocation where, uint32 seg);
extern void vc_locations(VcRepo *r, VcLocFn fn, void *arg);

/* The name the repository's manifests give it (and remotes, its hints). */
extern void vc_repo_set_name(VcRepo *r, const char *name);

/*
 * The history mode (KEEP until set).  In REWRITE mode vc_sever rewrites
 * and reports no segment: it locks every branch, then the repository's
 * state, so commits wait for it, and moves heads and merge heads to the
 * new hashes.  The hook (this backend's) hears each commit replaced, so
 * whatever names commits outside the repository can follow; it hears them
 * as the sever writes them, before its transaction commits, so a sever
 * that then aborts has moved none of them.  A commit pinned by hash
 * (VcWindow.pinned) with cut history beneath it is replaced as any: the
 * hook names its new hash, which a later sever's pin must name (the old
 * one, without a reference, is VC_ERR_SEVERED, as any commit cut already
 * that a pin names is, whether or not its chunks are freed yet).  Changing
 * the branches or the mode while a rewriting sever waits for the
 * repository's state refuses it (a serialization failure), to be run
 * again.
 */
extern void vc_set_history_mode(VcRepo *r, VcHistoryMode mode);
extern VcHistoryMode vc_history_mode(VcRepo *r);
extern void vc_set_rewrite_hook(VcRepo *r, VcRewriteFn fn, void *arg);

/*
 * Remotes: another repository with its own store and references.
 *
 * A transfer sends the commits the receiver lacks, each with its whole
 * tree, stopping at any commit the receiver knows, wherever it is there:
 * in its window, a segment, exported, a boundary, or a discarded parent of
 * a commit it holds.  So exported history, and discarded history it still
 * names, never comes back implicitly.  Discarded commits nothing held
 * names are forgotten: a commit sent that builds on one brings it back,
 * with what it needs beneath it.
 *
 * A transfer locks the receiver's state for update (it may record
 * boundaries), so it takes turns with window operations there, never with
 * commits; like them it needs a READ COMMITTED transaction, which reads
 * what committed while it waited.  Every call locks branches before the
 * repository's state, so calls do not deadlock on the two.
 *
 * Fetch, push and pull require the histories to attach: walking back from
 * the heads must reach a commit the receiver knows (a discarded one too:
 * the same hash is the same history).  Otherwise they are refused with
 * nothing kept: VC_ERR_UNRELATED if the walk reached roots, or
 * VC_ERR_DETACHED if it reached the sender's own cut first (err_hash names
 * it, vc_hint says where it is), which deepening or attaching repairs.
 * Only a clone, into a store with no repository, starts unattached, at the
 * given depth (0: everything the source has).
 *
 * A repository that rewrites its history has no remotes: every call here
 * with one, on either side, is VC_ERR_REWRITTEN, with nothing done.
 *
 * Parents not sent become boundaries on the receiver, with a hint naming
 * where their content can be found.  Push is checked for fast-forward by
 * the receiver, its branch locked, and refused (VC_ERR) if that branch has
 * work not committed or a merge under way.  Pull needs a named remote, its
 * name the tracking branch's prefix.  Each repository applies its own
 * window: nothing is severed on receipt.
 */
/* Where this repository says a commit it lacks is (palloc'd), or NULL. */
extern char *vc_hint(VcRepo *r, const PHash *commit);
extern int	vc_push(VcRepo *local, VcRepo *remote, const char *branch);
extern int	vc_fetch(VcRepo *local, VcRepo *remote, const char *branch,
					 PHash *head);
extern int	vc_repo_clone(VcRepo *dst, ChunkStore *cs, VcRefs *refs,
						  const ChunkPolicy *policy, VcRepo *src, uint32 depth);
/* History past a cut, from any source, into a new local segment. */
extern int	vc_deepen(VcRepo *r, const VcSource *src, const PHash *from,
					  uint32 generations, uint32 *seg);
/* Fetch into "<remote>/<branch>" and merge, deepening up to max_deepen. */
extern int	vc_pull(VcRepo *local, VcRepo *remote, const char *branch,
					uint32 max_deepen, VcMergeResult *res);

/* Archives: list and read the manifests of segments exported there. */
extern void vc_archive_list(ChunkStore *archive, PHash **manifests, size_t *n);
extern bool vc_manifest_load(ChunkStore *archive, const PHash *h, VcManifest *m);
extern void vc_manifest_free(VcManifest *m);

/* The clock commits read, in microseconds since 1970 (for tests). */
extern void vc_set_clock(VcRepo *r, uint64 (*now) (void *arg), void *arg);

#endif							/* PROLLY_ENGINE_VC_H */
