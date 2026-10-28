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
 * Costs, for a map of n entries with fanout f (height h = log_f n):
 *	get				O(h) chunk reads
 *	count			O(1)
 *	apply k edits	O(k h) chunk reads and writes
 *	diff			O(d h) chunk reads for d differences
 *	merge			O((d1 + d2) h)
 */
#ifndef PROLLY_H
#define PROLLY_H

#include "chunkstore.h"

#define PROLLY_MAX_HEIGHT 32

/* Chunk size policy; chunks average about target_bytes. */
typedef struct ChunkPolicy
{
	uint32_t	min_bytes;
	uint32_t	target_bytes;
	uint32_t	max_bytes;
} ChunkPolicy;

extern const ChunkPolicy prolly_default_policy;

typedef struct ProllyCtx
{
	ChunkStore *cs;
	ChunkPolicy policy;
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

typedef void (*ProllyLeafFn) (void *arg, const uint8_t *key, size_t klen,
							  const uint8_t *val, size_t vlen);

extern PHash prolly_empty(ProllyCtx *ctx);
/* kvs must be sorted by key with no duplicates. */
extern PHash prolly_build(ProllyCtx *ctx, const ProllyKV *kvs, size_t n);
/* Edits may come in any order; the last edit of a key wins. */
extern PHash prolly_apply(ProllyCtx *ctx, const PHash *root,
						  const ProllyEdit *edits, size_t n);
extern bool prolly_get(ProllyCtx *ctx, const PHash *root,
					   const uint8_t *key, size_t klen,
					   const uint8_t **val, size_t *vlen);
extern uint64_t prolly_count(ProllyCtx *ctx, const PHash *root);
extern int	prolly_height(ProllyCtx *ctx, const PHash *root);

/* Iterates keys >= start (all keys if start is NULL) in order. */
extern ProllyIter *prolly_iter_open(ProllyCtx *ctx, const PHash *root,
									const uint8_t *start, size_t slen);
extern bool prolly_iter_next(ProllyIter *it, const uint8_t **key, size_t *klen,
							 const uint8_t **val, size_t *vlen);
extern void prolly_iter_close(ProllyIter *it);

/* Reports changes from -> to in key order, skipping shared subtrees. */
extern void prolly_diff(ProllyCtx *ctx, const PHash *from, const PHash *to,
						ProllyDiffFn fn, void *arg);
extern void prolly_diff_collect(ProllyCtx *ctx, const PHash *from,
								const PHash *to, ProllyDiffList *out);
extern void prolly_diff_list_free(ProllyDiffList *l);

/* Three-way merge of theirs into ours. */
extern void prolly_merge(ProllyCtx *ctx, const PHash *base, const PHash *ours,
						 const PHash *theirs, ProllyMergeResult *out);
extern void prolly_merge_result_free(ProllyMergeResult *r);

/*
 * Copies the chunks of the tree at root that dst lacks, pruning at subtrees
 * dst already holds.  leaf_fn sees each entry of every copied leaf before
 * the leaf is stored, so values that name other chunks can copy them first.
 */
extern void prolly_sync(ProllyCtx *src, ChunkStore *dst, const PHash *root,
						ProllyLeafFn leaf_fn, void *arg);

#endif							/* PROLLY_H */
