/*
 * prolly.h
 *	  Prolly tree: a content-addressed, history-independent ordered map.
 *
 * A map is named by the hash of its root chunk.  Leaves hold sorted
 * key/value pairs; internal nodes hold (last key, child hash, subtree item
 * count) per child.  Node boundaries are chosen from the content (a hash of
 * each key and the bytes in the chunk so far), so the same contents always
 * give the same tree and the same root hash, however they were built.
 *
 * Every node records the chunk policy that shaped it and how many leading
 * hashes each leaf value holds (vrefs), so a tree is self-describing: edits
 * continue with the tree's own policy, and storage can find every reference
 * in a node without knowing what the map is for.
 *
 * Costs, for a map of n entries with fanout f (height h = log_f n):
 *	get				O(h) chunk reads
 *	count			O(1)
 *	apply k edits	O(k h) chunk reads and writes
 *	diff			O(d h) chunk reads for d differences
 *	merge			O((d1 + d2) h)
 *	chunk diff		O(d h)
 *
 * Holds: the caller must hold (root ref or pin) every tree it passes in for
 * the length of the call.  Every chunk an operation writes or reuses is
 * pinned in the context until prolly_release_pins, so the caller can take
 * a root ref on the result first.  Operations return PROLLY_OK or a
 * negative error, with the chunk at fault in ctx->err_hash; they never
 * leave a partial tree visible.
 */
#ifndef PROLLY_H
#define PROLLY_H

#include "chunkstore.h"

#define PROLLY_MAX_HEIGHT 32

#define PROLLY_OK CS_OK
#define PROLLY_NOT_FOUND 1
#define PROLLY_ERR_MISSING CS_ERR_MISSING
#define PROLLY_ERR_CORRUPT CS_ERR_CORRUPT

extern const ChunkPolicy prolly_default_policy;

typedef struct ProllyCtx
{
	ChunkStore *cs;
	ChunkPolicy policy;			/* for trees built from scratch */
	PinSet		pins;
	int			err;
	PHash		err_hash;
} ProllyCtx;

typedef struct ProllyKV
{
	const uint8_t *key;
	size_t		klen;
	const uint8_t *val;
	size_t		vlen;
} ProllyKV;

typedef struct ProllyEdit
{
	const uint8_t *key;
	size_t		klen;
	const uint8_t *val;
	size_t		vlen;
	bool		del;
} ProllyEdit;

/* One changed key; from is NULL for an insert, to is NULL for a delete. */
typedef struct ProllyDiff
{
	const uint8_t *key;
	size_t		klen;
	const uint8_t *from;
	size_t		flen;
	const uint8_t *to;
	size_t		tlen;
} ProllyDiff;

typedef void (*ProllyDiffFn) (void *arg, const ProllyDiff *d);
typedef void (*ProllyChunkFn) (void *arg, const PHash *h);

typedef struct ProllyDiffList
{
	ProllyDiff *v;
	size_t		n;
	size_t		cap;
} ProllyDiffList;

/* A key both sides changed differently; absent sides are NULL. */
typedef struct ProllyConflict
{
	const uint8_t *key;
	size_t		klen;
	const uint8_t *base;
	size_t		blen;
	const uint8_t *ours;
	size_t		olen;
	const uint8_t *theirs;
	size_t		tlen;
} ProllyConflict;

typedef struct ProllyMergeResult
{
	PHash		root;			/* ours with every non-conflicting change of theirs */
	ProllyConflict *conflicts;
	size_t		nconflicts;
} ProllyMergeResult;

typedef struct ProllyIter ProllyIter;

extern void prolly_ctx_init(ProllyCtx *ctx, ChunkStore *cs, const ChunkPolicy *policy);
extern void prolly_ctx_free(ProllyCtx *ctx);
extern void prolly_release_pins(ProllyCtx *ctx);

/* Stores a chunk of any kind, pinned like the trees an operation writes. */
extern int	prolly_put_chunk(ProllyCtx *ctx, const uint8_t *data, size_t len,
							 PHash *out);

/* vrefs: leading hashes in each value that the map holds strongly. */
extern int	prolly_empty(ProllyCtx *ctx, uint8_t vrefs, PHash *out);
/* kvs must be sorted by key with no duplicates. */
extern int	prolly_build(ProllyCtx *ctx, uint8_t vrefs, const ProllyKV *kvs,
						 size_t n, PHash *out);
/* Edits may come in any order; the last edit of a key wins. */
extern int	prolly_apply(ProllyCtx *ctx, const PHash *root,
						 const ProllyEdit *edits, size_t n, PHash *out);
/* PROLLY_OK and the value, or PROLLY_NOT_FOUND. */
extern int	prolly_get(ProllyCtx *ctx, const PHash *root,
					   const uint8_t *key, size_t klen,
					   const uint8_t **val, size_t *vlen);
extern int	prolly_count(ProllyCtx *ctx, const PHash *root, uint64_t *out);
extern int	prolly_height(ProllyCtx *ctx, const PHash *root, int *out);

/* Iterates keys >= start (all keys if start is NULL) in order. */
extern int	prolly_iter_open(ProllyCtx *ctx, const PHash *root,
							 const uint8_t *start, size_t slen, ProllyIter **out);
/* 1 with the next entry, 0 at the end, or a negative error. */
extern int	prolly_iter_next(ProllyIter *it, const uint8_t **key, size_t *klen,
							 const uint8_t **val, size_t *vlen);
extern void prolly_iter_close(ProllyIter *it);

/* Reports changes from -> to in key order, skipping shared subtrees. */
extern int	prolly_diff(ProllyCtx *ctx, const PHash *from, const PHash *to,
						ProllyDiffFn fn, void *arg);
extern int	prolly_diff_collect(ProllyCtx *ctx, const PHash *from,
								const PHash *to, ProllyDiffList *out);
extern void prolly_diff_list_free(ProllyDiffList *l);

/* Three-way merge of theirs into ours. */
extern int	prolly_merge(ProllyCtx *ctx, const PHash *base, const PHash *ours,
						 const PHash *theirs, ProllyMergeResult *out);
extern void prolly_merge_result_free(ProllyMergeResult *r);

/*
 * Reports exactly the nodes of tree a that are not nodes of tree b: what
 * dropping a frees while b is kept, what a holder of b lacks to have a.
 */
extern int	prolly_chunk_diff(ProllyCtx *ctx, const PHash *a, const PHash *b,
							  ProllyChunkFn fn, void *arg);

/*
 * A delta is a chunk listing the row edits that turn tree base into tree
 * target.  Because a tree is a function of its contents, applying it to
 * base reproduces target exactly; delta_apply checks the hash.
 */
extern int	prolly_delta_write(ProllyCtx *ctx, const PHash *base,
							   const PHash *target, PHash *out);
extern int	prolly_delta_apply(ProllyCtx *ctx, const PHash *delta, PHash *out);

/* Restores the strong closure of root from source, verifying every hash. */
extern int	prolly_repair(ProllyCtx *ctx, ChunkStore *source, const PHash *root);

#endif							/* PROLLY_H */
