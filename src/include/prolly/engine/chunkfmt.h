/*-------------------------------------------------------------------------
 *
 * chunkfmt.h
 *	  Chunk formats and the one place that knows where each chunk's
 *	  references are.
 *
 * Every chunk starts with a tag byte and a format version byte.  A
 * reference is either strong (the target must be kept, and must exist
 * before the chunk naming it is stored) or weak (a link only, such as a
 * commit's parents).  Storage lifetime, sync and repair walk chunks only
 * through engine_chunk_refs, so they need no knowledge of what a chunk means.
 *
 * Node:	tag, version, uvarint min/target/max chunk bytes, level,
 *			vrefs, uvarint count, items (uvarint klen, key, uvarint vlen, value)
 *			Internal values: child hash, uvarint leaf count (strong).
 *			Leaf values start with vrefs hashes (strong); the rest is opaque.
 *			Every node of a tree carries the same policy and vrefs.
 * Schema:	tag, version, DDL bytes
 * Commit:	tag, version, root value hash (strong), uvarint generation,
 *			uvarint change bytes, uvarint time, uvarint nparents, (parent
 *			hash (weak), uvarint generation, uvarint change bytes, uvarint
 *			time)..., uvarint msglen, msg
 * Delta:	tag, version, base hash (weak), target hash (weak), uvarint n,
 *			(op byte, uvarint klen, key, [uvarint vlen, value])...
 * Manifest: tag, version, uvarint namelen, source repo name, uvarint n,
 *			(member commit hash (strong), uvarint generation)..., uvarint nb,
 *			(boundary commit hash (weak))..., uvarint nkf, (commit hash (weak),
 *			commit image hash (strong), uvarint change bytes)...
 *			Describes a segment exported to an archive, with the keyframes of
 *			its members, and holds both there.
 * Page:	tag, version, the page's bytes
 * Image:	tag, version, uvarint pg major, uvarint page size, uvarint
 *			catalog version, 8-byte system id, commit hash (weak), has-base
 *			byte, [base image hash (weak)], uvarint lsn, uvarint snapshot xmin,
 *			uvarint xmax, uvarint nxip, uvarint xip..., page map root (strong),
 *			relation map root (strong)
 * Branch:	tag, version, head commit hash (strong), working root value hash
 *			(strong), merging byte (0 or 1), [merge head commit hash (strong)]
 *			A branch's state, which its reference names.
 *
 * EngineChunkFormat is these formats as the history store's ChunkFormat
 * (prolly/chunk_format.h): the strong references, those a store keeps a
 * chunk for; bytes that are no chunk of these an error.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/engine/chunkfmt.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_ENGINE_CHUNKFMT_H
#define PROLLY_ENGINE_CHUNKFMT_H

#include "prolly/chunk_format.h"
#include "prolly/engine/phash.h"

#define CHUNK_TAG_NODE 0x50
#define CHUNK_TAG_SCHEMA 0x53
#define CHUNK_TAG_COMMIT 0x43
#define CHUNK_TAG_DELTA 0x44
#define CHUNK_TAG_MANIFEST 0x4d
#define CHUNK_TAG_PAGE 0x47
#define CHUNK_TAG_IMAGE 0x49
#define CHUNK_TAG_BRANCH 0x42

/*
 * Covers the encodings and the chunk boundary function; anything that
 * changes the bytes a given set of rows produces must bump it.
 */
#define CHUNK_FORMAT_VERSION 3

/* Chunk size policy; chunks average about target_bytes. */
typedef struct ChunkPolicy
{
	uint32	min_bytes;
	uint32	target_bytes;
	uint32	max_bytes;
} ChunkPolicy;

typedef struct NodeHeader
{
	ChunkPolicy policy;
	int			level;
	uint8		vrefs;
	uint32	count;
	const uint8 *items;		/* first item */
	const uint8 *end;
} NodeHeader;

extern bool engine_policy_valid(const ChunkPolicy *p);
extern bool engine_node_header_parse(const uint8 *data, size_t len, NodeHeader *h);
extern void engine_node_header_write(Buf *b, const ChunkPolicy *p, int level,
							  uint8 vrefs, uint32 count);

typedef void (*EngineRefFn) (void *arg, const PHash *h, bool strong);

/* Reports every reference in the chunk; false if the bytes do not parse. */
extern bool engine_chunk_refs(const uint8 *data, size_t len, EngineRefFn fn, void *arg);

/* These formats as a store's ChunkFormat: the strong references. */
extern PGDLLIMPORT const ChunkFormatRoutine EngineChunkFormat;

#endif							/* PROLLY_ENGINE_CHUNKFMT_H */
