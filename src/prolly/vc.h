/*
 * vc.h
 *	  Version control over prolly trees for one schema.
 *
 * A schema's state is a root value: a prolly map from table name to the
 * table's schema chunk (its DDL) and the root of its row map.  Commits name
 * a root value and their parents.  Branches are mutable refs that each keep
 * their own head and working root value.
 */
#ifndef VC_H
#define VC_H

#include "prolly.h"

#define VC_MAX_PARENTS 8

#define VC_OK 0
#define VC_ERR (-10)			/* bad argument or state */
#define VC_ERR_SEVERED (-3)		/* needs a commit cut from local history */
#define VC_ERR_OUTSIDE_WINDOW (-4)	/* stopped at the window's edge */
#define VC_ERR_UNRELATED (-5)	/* the histories share no commit */
#define VC_ERR_DETACHED (-6)	/* the sender's history is cut before it attaches */

/*
 * Stop at the window's edge: refuse any commit the window does not hold,
 * even one still present in a local segment.  The edge is the one set by
 * the last vc_sever (before any sever, all history is in the window).
 */
#define VC_WINDOW_ONLY 0x1

typedef struct VcTable
{
	PHash		schema;
	PHash		data;
} VcTable;

typedef struct VcTableChange
{
	const uint8_t *name;
	size_t		nlen;
	bool		has_from;
	bool		has_to;
	VcTable		from;
	VcTable		to;
} VcTableChange;

typedef void (*VcTableChangeFn) (void *arg, const VcTableChange *c);

typedef struct VcCommit
{
	PHash		hash;
	PHash		root;
	uint64_t	generation;		/* 1 + the largest parent generation */
	uint32_t	nparents;
	PHash		parents[VC_MAX_PARENTS];
	uint64_t	parent_gen[VC_MAX_PARENTS];
	uint64_t	time;
	const char *msg;			/* not NUL-terminated */
	size_t		msglen;
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
	uint8_t    *key;			/* row conflicts only */
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
 * The window: every branch head (and pending merge head), each commit
 * within depth generations of one, and each pinned commit.
 */
typedef struct VcWindow
{
	uint32_t	depth;			/* >= 1 */
	const PHash *pinned;
	size_t		npinned;
} VcWindow;

typedef enum VcSegState
{
	VC_SEG_LOCAL,				/* detached, still held here */
	VC_SEG_EXPORTED,			/* copied whole to an archive, freed here */
	VC_SEG_DISCARDED,			/* gone */
	VC_SEG_ATTACHED				/* moved back into the window's history */
} VcSegState;

/* Commits severed from the window together. */
typedef struct VcSegment
{
	VcSegState	state;
	PHash	   *commits;
	size_t		n;
	ChunkStore *archive;		/* where an exported segment lives */
	char	   *archive_name;
	PHash		manifest;		/* holds it there */
} VcSegment;

/*
 * Anything chunks can be fetched from by hash: a remote's store, an
 * archive, another repo.  Everything fetched is verified, so any source
 * will do.  The name is what boundaries record as a hint.
 */
typedef struct VcSource
{
	const char *name;
	ChunkStore *cs;
} VcSource;

/* What an archive says about a segment exported to it. */
typedef struct VcManifest
{
	char	   *repo;			/* the repo that exported it */
	PHash	   *members;
	uint64_t   *gens;
	size_t		n;
	PHash	   *bounds;			/* parents of members that are not members */
	size_t		nb;
} VcManifest;

#define VC_NO_SEGMENT UINT32_MAX

typedef enum VcLocation
{
	VC_LOC_UNKNOWN,
	VC_LOC_WINDOW,				/* held by the window's history */
	VC_LOC_SEGMENT,				/* in a local, detached segment */
	VC_LOC_EXPORTED,			/* in a segment moved to an archive */
	VC_LOC_DISCARDED,			/* in a discarded segment */
	VC_LOC_BOUNDARY				/* named as a parent; never had locally */
} VcLocation;

typedef struct VcRepo
{
	ProllyCtx	ctx;
	VcBranch   *br;
	size_t		nbr;
	size_t		cap;
	size_t		cur;
	uint64_t	clock;
	PHashMap	loc;			/* commit -> where it is */
	VcSegment  *segs;
	size_t		nsegs;
	size_t		segcap;
	char	   *name;			/* how others refer to this repo */
	uint32_t	window_depth;	/* the window depth this repo keeps */
	PHashMap	hint;			/* boundary commit -> hints index + 1 */
	char	  **hints;
	size_t		nhints;
	size_t		hintcap;
	PHash	   *img_commit;		/* commit images held, by commit */
	PHash	   *img_hash;
	size_t		nimg;
	size_t		imgcap;
	PHashMap	brmap;			/* hash of branch name -> index + 1 */
	PHashMap	imgmap;			/* commit -> image index + 1 */
	uint32_t	track_depth;	/* incremental window depth; 0: not tracked */
	PHashMap	cover;			/* commit -> heads whose window covers it */
	PHash	   *pending;		/* commits whose cover fell to 0, or entered */
	size_t		npending;
	size_t		pendcap;
} VcRepo;

typedef void (*VcLogFn) (void *arg, const VcCommit *c);

/* Root values. */
extern PHash vc_root_empty(ProllyCtx *ctx);
extern bool vc_table_get(ProllyCtx *ctx, const PHash *rv, const char *name,
						 VcTable *out);
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
extern const uint8_t *vc_schema_get(ProllyCtx *ctx, const PHash *schema,
									size_t *len);
extern void vc_diff_tables(ProllyCtx *ctx, const PHash *from, const PHash *to,
						   VcTableChangeFn fn, void *arg);
extern void vc_merge_roots(ProllyCtx *ctx, const PHash *base, const PHash *ours,
						   const PHash *theirs, PHash *out,
						   VcConflict **conflicts, size_t *nconflicts);
extern void vc_conflicts_free(VcConflict *c, size_t n);

/* Commits. */
extern bool vc_commit_load(ProllyCtx *ctx, const PHash *h, VcCommit *out);
extern PHash vc_commit_write(ProllyCtx *ctx, const PHash *root,
							 const PHash *parents, uint32_t nparents,
							 uint64_t time, const char *msg);
/* 1 with the base, 0 if none, VC_ERR_SEVERED if the walk crosses a cut. */
extern int	vc_merge_base(ProllyCtx *ctx, const PHash *a, const PHash *b,
						  PHash *out);

/* Repositories and branches. */
extern void vc_repo_init(VcRepo *r, ChunkStore *cs, const ChunkPolicy *policy);
extern void vc_repo_free(VcRepo *r);
extern VcBranch *vc_branch_find(VcRepo *r, const char *name);
extern VcBranch *vc_head(VcRepo *r);
extern PHash vc_working(VcRepo *r);
extern void vc_set_working(VcRepo *r, const PHash *rv);
extern int	vc_branch_create(VcRepo *r, const char *name, const PHash *commit);
/* Deletes a branch other than the current one; its working set is let go. */
extern int	vc_branch_delete(VcRepo *r, const char *name);
extern int	vc_checkout(VcRepo *r, const char *name);
extern PHash vc_commit(VcRepo *r, const char *msg);

/*
 * Commits root (held by the caller until this returns) on the current
 * branch.  The working root is left alone, so changes it already has
 * beyond root stay uncommitted: a commit can capture the state as of a
 * point while the working set keeps moving.
 */
extern PHash vc_commit_root(VcRepo *r, const PHash *root, const char *msg);
extern int	vc_reset_hard(VcRepo *r, const PHash *commit);
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

/*
 * Severing.  vc_sever moves every commit the window no longer covers into
 * a new local segment (or reports VC_NO_SEGMENT): only holds move, no chunk
 * is copied or freed.  A segment can then be exported (copied whole to an
 * archive store, then let go here), discarded, or attached again.
 */
extern int	vc_sever(VcRepo *r, const VcWindow *w, uint32_t *seg);

/*
 * Incremental window.  vc_window_track(r, depth) starts counting, for every
 * commit, how many branch heads (and pending merge heads) have it within
 * depth generations.  Moving, creating or deleting a head then updates only
 * the commits near it, O(depth), and a vc_sever with the same depth and no
 * pins takes only the commits whose count fell to zero, so neither cost
 * grows with the number of branches.  vc_window_verify recomputes the
 * counts from scratch and checks them (O(branches x depth); for tests).
 */
extern int	vc_window_track(VcRepo *r, uint32_t depth);
extern bool vc_window_verify(VcRepo *r);
extern int	vc_segment_export(VcRepo *r, uint32_t seg, const VcSource *archive);
extern int	vc_segment_discard(VcRepo *r, uint32_t seg);
extern int	vc_segment_attach(VcRepo *r, uint32_t seg);
extern VcLocation vc_locate(VcRepo *r, const PHash *commit, uint32_t *seg);

/*
 * Commit images (see image.h) are local optimization artifacts: a commit
 * in the window may hold one, replacing any it held before, and severing
 * the commit releases it.  They are never exported or sent to remotes.
 */
extern int	vc_image_set(VcRepo *r, const PHash *commit, const PHash *image);
extern bool vc_image_get(VcRepo *r, const PHash *commit, PHash *image);

/* Archives: list and read the manifests of segments exported there. */
extern void vc_archive_list(ChunkStore *archive, PHash **manifests, size_t *n);
extern bool vc_manifest_load(ChunkStore *archive, const PHash *h, VcManifest *m);
extern void vc_manifest_free(VcManifest *m);

/*
 * Remotes: another repo with its own store and window.
 *
 * A transfer sends the commits the receiver lacks, each with its whole
 * tree, stopping at any commit the receiver already knows, wherever it is
 * there, so discarded or exported history never comes back implicitly.
 *
 * Fetch, push and pull require the histories to attach: walking back from
 * the heads must reach a commit the receiver knows (as a cut commit too:
 * the same hash is the same history).  Otherwise they are rejected with
 * nothing left behind: VC_ERR_UNRELATED if the walk reached roots, or
 * VC_ERR_DETACHED if it reached the sender's own cut first (err_hash names
 * it, vc_hint says where it lives), which deepening or attaching repairs.
 * Only a clone into an empty repo starts unattached, at the given depth.
 *
 * Parents not sent become boundaries on the receiver, with a hint naming
 * where their content can be found.  Push is checked for fast-forward by
 * the receiver.  Each repo applies its own window: nothing is severed on
 * receive.
 */
extern void vc_repo_set_name(VcRepo *r, const char *name);
extern void vc_set_window_depth(VcRepo *r, uint32_t depth);
/* Where this repo says a commit it lacks can be found: its archive or a hint. */
extern const char *vc_hint(VcRepo *r, const PHash *commit);
extern int	vc_push(VcRepo *local, VcRepo *remote, const char *branch);
extern int	vc_fetch(VcRepo *local, VcRepo *remote, const char *branch,
					 PHash *head);
/* depth 0 clones everything the source has. */
extern int	vc_repo_clone(VcRepo *dst, ChunkStore *cs, VcRepo *src, uint32_t depth);
/* History past a cut, from any source, into a new local segment. */
extern int	vc_deepen(VcRepo *r, const VcSource *src, const PHash *from,
					  uint32_t generations, uint32_t *seg);
/* Fetch into "<remote>/<branch>" and merge, deepening up to max_deepen. */
extern int	vc_pull(VcRepo *local, VcRepo *remote, const char *branch,
					uint32_t max_deepen, VcMergeResult *res);

#endif							/* VC_H */
