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
} VcSegment;

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
extern int	vc_checkout(VcRepo *r, const char *name);
extern PHash vc_commit(VcRepo *r, const char *msg);
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
extern int	vc_segment_export(VcRepo *r, uint32_t seg, ChunkStore *archive);
extern int	vc_segment_discard(VcRepo *r, uint32_t seg);
extern int	vc_segment_attach(VcRepo *r, uint32_t seg);
extern VcLocation vc_locate(VcRepo *r, const PHash *commit, uint32_t *seg);

/* Remotes: another repo with its own chunk store. */
extern int	vc_sync_commit(VcRepo *src, VcRepo *dst, const PHash *commit);
extern int	vc_push(VcRepo *local, VcRepo *remote, const char *branch);
extern int	vc_fetch(VcRepo *local, VcRepo *remote, const char *branch,
					 PHash *head);
extern int	vc_repo_clone(VcRepo *dst, ChunkStore *cs, VcRepo *src);

#endif							/* VC_H */
