/*-------------------------------------------------------------------------
 *
 * prolly_types.h
 *	  Data types shared by the prolly storage interfaces.
 *
 * Everything here is plain data passed between interfaces.  Nothing here
 * belongs to any one implementation.
 *
 * src/include/access/prolly_types.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_TYPES_H
#define PROLLY_TYPES_H

#include "storage/block.h"

/*
 * A content hash.  The length is decided by the content-hash implementation
 * (prolly_hash.h) and is part of the canonical format; PROLLY_HASH_MAX_LEN
 * only bounds the in-memory representation.
 */
#define PROLLY_HASH_MAX_LEN 64

typedef struct ProllyHash
{
	uint8		len;			/* 0 means "no hash" */
	uint8		bytes[PROLLY_HASH_MAX_LEN];
} ProllyHash;

/*
 * An encoded key: bytes produced by a key encoder (prolly_keyenc.h).  The
 * memory is owned by whoever produced it; consumers copy if they keep it.
 */
typedef struct ProllyKey
{
	const uint8 *data;
	uint32		len;
} ProllyKey;

/*
 * An encoded value: bytes produced by a value encoder (prolly_valenc.h), or a
 * child reference in an internal node.  Same ownership rule as ProllyKey.
 */
typedef struct ProllyValue
{
	const uint8 *data;
	uint32		len;
} ProllyValue;

/*
 * A physical location inside a relation's storage: a whole block, or a run
 * of bytes within one (prolly_alloc.h).
 */
typedef struct ProllyExtent
{
	BlockNumber blkno;			/* InvalidBlockNumber if none */
	uint16		offset;			/* byte offset within the block */
	uint16		length;			/* 0 means the whole block */
} ProllyExtent;

/*
 * A reference to a node.  Hash-addressed stores identify nodes by hash;
 * location-addressed stores by extent, with the hash computed on demand
 * (lazy hashing).  Either may be absent; at least one must be present.  The
 * location may also be a read hint for a hash-addressed store.
 */
typedef struct ProllyNodeRef
{
	ProllyHash	hash;			/* hash.len == 0 if not (yet) known */
	ProllyExtent location;		/* location.blkno invalid if not known */
} ProllyNodeRef;

/* A branch, as numbered by the refs implementation (prolly_refs.h). */
typedef uint32 ProllyBranchId;

#define InvalidProllyBranchId	((ProllyBranchId) 0)

/*
 * Every database's default branch.  Until refs exist, it is the only one.
 */
#define PROLLY_MAIN_BRANCH		((ProllyBranchId) 1)

/*
 * Which of a relation's trees is meant.  DATA is the relation's own rows
 * (or index entries); the others are local metadata, never part of a
 * version-control commit.  These numbers are persisted; never renumber.
 */
typedef enum ProllyTreeKind
{
	PROLLY_TREE_DATA = 0,
	PROLLY_TREE_ROWID = 1,		/* row identity: TIDs <-> rows */
	PROLLY_TREE_META = 2,		/* facts about the data tree, such as its keying */
} ProllyTreeKind;

#define PROLLY_TREE_KIND_LAST	PROLLY_TREE_META

/*
 * Opaque handles.  Each is defined privately by the implementation that
 * creates it; callers only pass them back to the same implementation.
 */
typedef struct ProllyRootSet ProllyRootSet;		/* engine */
typedef struct ProllyCursor ProllyCursor;		/* engine */
typedef struct ProllyChunker ProllyChunker;		/* chunker */
typedef struct ProllyHashCtx ProllyHashCtx;		/* content hash */
typedef struct ProllyKeyEncoder ProllyKeyEncoder;	/* key encoder */
typedef struct ProllyValueEncoder ProllyValueEncoder;	/* value encoder */
typedef struct ProllyNodeStore ProllyNodeStore; /* node store */
typedef struct ProllyAllocator ProllyAllocator; /* allocator */

/*
 * Receives canonical chunks (engine export, transfer).  Callers embed this
 * as the first member of their own struct to carry state.
 */
typedef struct ProllyChunkSink ProllyChunkSink;
struct ProllyChunkSink
{
	void		(*put) (ProllyChunkSink *self, const ProllyHash *hash,
						const uint8 *data, Size len);
};

/* Receives row differences (diff/merge).  Same embedding convention. */
typedef enum ProllyDiffKind
{
	PROLLY_DIFF_ADDED,
	PROLLY_DIFF_REMOVED,
	PROLLY_DIFF_MODIFIED,
} ProllyDiffKind;

typedef struct ProllyDiffSink ProllyDiffSink;
struct ProllyDiffSink
{
	void		(*change) (ProllyDiffSink *self, Oid relid, ProllyDiffKind kind,
						   const ProllyKey *key,
						   const ProllyValue *before,	/* NULL if added */
						   const ProllyValue *after);	/* NULL if removed */
};

/* Nodes read from and written to a node store, for counting work. */
typedef struct ProllyNodeCounters
{
	uint64		nodes_read;
	uint64		nodes_written;		/* new nodes, not ones already stored */
} ProllyNodeCounters;

#endif							/* PROLLY_TYPES_H */
