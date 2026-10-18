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

/* A reference to a node: the hash it is found by. */
typedef struct ProllyNodeRef
{
	ProllyHash	hash;			/* hash.len == 0 if not (yet) known */
} ProllyNodeRef;

/* A branch, as numbered by the refs implementation (prolly_refs.h). */
typedef uint32 ProllyBranchId;

/*
 * Opaque handles.  Each is defined privately by the implementation that
 * creates it; callers only pass them back to the same implementation.
 */
typedef struct ProllyChunker ProllyChunker;		/* chunker */
typedef struct ProllyHashCtx ProllyHashCtx;		/* content hash */
typedef struct ProllyKeyEncoder ProllyKeyEncoder;	/* key encoder */
typedef struct ProllyValueEncoder ProllyValueEncoder;	/* value encoder */

/*
 * Receives row differences (diff/merge).  Callers embed this as the first
 * member of their own struct to carry state.
 */
typedef enum ProllyDiffKind
{
	PROLLY_DIFF_ADDED,
	PROLLY_DIFF_REMOVED,
	PROLLY_DIFF_MODIFIED,
	PROLLY_DIFF_NOT_COVERED,	/* (tables only) an object a read left out,
								 * which a commit refuses */
} ProllyDiffKind;

typedef struct ProllyDiffSink ProllyDiffSink;
struct ProllyDiffSink
{
	void		(*change) (ProllyDiffSink *self, Oid relid, ProllyDiffKind kind,
						   const ProllyKey *key,
						   const ProllyValue *before,	/* NULL if added */
						   const ProllyValue *after);	/* NULL if removed */
};

/* Tree nodes read from and written to a chunk store, for counting work. */
typedef struct ProllyNodeCounters
{
	uint64		nodes_read;
	uint64		nodes_written;		/* new nodes, not ones already stored */
	uint64		value_nodes_written;	/* of those, value trees' */
} ProllyNodeCounters;

/*
 * A hash as text (lowercase hex), and read back from len characters of hex
 * (false if they aren't a hash's): prolly_hashtext.c.
 */
extern char *prolly_hash_hex(const ProllyHash *hash);
extern bool prolly_hash_from_hex(const char *hex, size_t len, ProllyHash *hash);

#endif							/* PROLLY_TYPES_H */
