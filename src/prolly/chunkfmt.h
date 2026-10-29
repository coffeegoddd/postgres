/*
 * chunkfmt.h
 *	  Chunk formats and the one place that knows where each chunk's
 *	  references are.
 *
 * Every chunk starts with a tag byte and a format version byte.  A
 * reference is either strong (the target must be kept, and must exist
 * before the chunk naming it is stored) or weak (a link only, such as a
 * commit's parents).  Storage lifetime, sync and repair walk chunks only
 * through chunk_refs, so they need no knowledge of what a chunk means.
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
 *			(boundary commit hash (weak))...
 *			Describes a segment exported to an archive and holds it there.
 * Page:	tag, version, the page's bytes
 * Image:	tag, version, uvarint pg major, uvarint page size, uvarint
 *			catalog version, 8-byte system id, commit hash (weak), has-base
 *			byte, [base image hash (weak)], uvarint lsn, uvarint snapshot xmin,
 *			uvarint xmax, uvarint nxip, uvarint xip..., page map root (strong),
 *			relation map root (strong)
 * Repo state: tag, version, then a repo's branches, commit locations,
 *			segments, hints and image holds (see vc_repo_save).  It names no
 *			chunk strongly: what it lists is held by the store's roots.
 */
#ifndef CHUNKFMT_H
#define CHUNKFMT_H

#include "phash.h"

#define CHUNK_TAG_NODE 0x50
#define CHUNK_TAG_SCHEMA 0x53
#define CHUNK_TAG_COMMIT 0x43
#define CHUNK_TAG_DELTA 0x44
#define CHUNK_TAG_MANIFEST 0x4d
#define CHUNK_TAG_PAGE 0x47
#define CHUNK_TAG_IMAGE 0x49
#define CHUNK_TAG_REPO 0x52

/*
 * Covers the encodings and the chunk boundary function; anything that
 * changes the bytes a given set of rows produces must bump it.
 */
#define CHUNK_FORMAT_VERSION 2

/* Chunk size policy; chunks average about target_bytes. */
typedef struct ChunkPolicy
{
	uint32_t	min_bytes;
	uint32_t	target_bytes;
	uint32_t	max_bytes;
} ChunkPolicy;

typedef struct NodeHeader
{
	ChunkPolicy policy;
	int			level;
	uint8_t		vrefs;
	uint32_t	count;
	const uint8_t *items;		/* first item */
	const uint8_t *end;
} NodeHeader;

extern bool policy_valid(const ChunkPolicy *p);
extern bool node_header_parse(const uint8_t *data, size_t len, NodeHeader *h);
extern void node_header_write(Buf *b, const ChunkPolicy *p, int level,
							  uint8_t vrefs, uint32_t count);

typedef void (*ChunkRefFn) (void *arg, const PHash *h, bool strong);

/* Reports every reference in the chunk; false if the bytes do not parse. */
extern bool chunk_refs(const uint8_t *data, size_t len, ChunkRefFn fn, void *arg);

#endif							/* CHUNKFMT_H */
