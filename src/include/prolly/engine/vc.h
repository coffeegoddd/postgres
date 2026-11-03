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
 * A commit writes its own reference and moves its branch's, and nothing
 * else, so commits on different branches change different rows.  A VcRepo
 * is a backend's view of them: the branch it has checked out is its own,
 * and each call reads the references it needs, so opening one reads main's
 * reference, its chunk and head commit, however long the history.
 *
 * Calls that change a branch lock its reference first (for update), and
 * read it after, so two that change one branch take turns: two commits on
 * one branch both land, one after the other.  vc_set_working replaces the
 * working root it is given, whatever it was meanwhile: two sessions
 * writing one branch's working set without committing between them are
 * the working set's own concern, not this layer's.  Errors of the
 * caller's (an unknown or misnamed branch, uncommitted changes) are
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

typedef struct VcRepo
{
	ProllyCtx	ctx;
	VcRefs	   *refs;
	char	   *cur;			/* the branch checked out: this backend's */
	uint64		(*now) (void *arg); /* microseconds; NULL: the server's clock */
	void	   *now_arg;
} VcRepo;

typedef void (*VcLogFn) (void *arg, const VcCommit *c);
typedef void (*VcBranchFn) (void *arg, const VcBranch *b);

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
extern void vc_repo_free(VcRepo *r);
/* The branch, false if there is none; vc_branch_free frees out's name. */
extern bool vc_branch_get(VcRepo *r, const char *name, VcBranch *out);
extern void vc_branch_free(VcBranch *b);
/* Every branch, in name order. */
extern void vc_branch_list(VcRepo *r, VcBranchFn fn, void *arg);
/* The branch checked out. */
extern void vc_head(VcRepo *r, VcBranch *out);
extern PHash vc_working(VcRepo *r);
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

/* The clock commits read, in microseconds since 1970 (for tests). */
extern void vc_set_clock(VcRepo *r, uint64 (*now) (void *arg), void *arg);

#endif							/* PROLLY_ENGINE_VC_H */
