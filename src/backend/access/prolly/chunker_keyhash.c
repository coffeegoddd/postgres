/*-------------------------------------------------------------------------
 *
 * chunker_keyhash.c
 *	  Chunker implementation "keyhash": boundaries chosen by key alone.
 *
 * A node ends after an item whose key hashes below a threshold.  The
 * decision depends on that key's bytes and the tree level only, so updating
 * a value never moves a boundary, and inserting or deleting a key moves at
 * most the boundaries next to it.  This is the starting choice for the
 * first engine.
 *
 * Because a node must fit in a page, an item that would overflow the
 * current node starts a new one (PROLLY_CHUNK_SPLIT_BEFORE).  That depends
 * on the bytes of the items since the last boundary, which is still
 * deterministic from content.  If an item that forced a split is itself a
 * boundary, it is alone in its node, and the next item starts a new node.
 *
 * The boundary hash is 64-bit FNV-1a over the key, seeded by level, with the
 * splitmix64 finalizer.  It is written out here rather than taken from
 * hashfn.c, so it is the same on every platform by construction.
 *
 * Target node sizes are starting values, to be set by measurement and then
 * frozen as part of the canonical format.  A
 * key-only rule targets a number of items, not bytes; the targets assume an
 * average item of LEAF_ITEM_BYTES in leaves and INTERNAL_ITEM_BYTES in
 * internal nodes.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/chunker_keyhash.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"

#define LEAF_ITEM_BYTES		256
#define INTERNAL_ITEM_BYTES	128

#define FNV64_OFFSET	UINT64CONST(0xcbf29ce484222325)
#define FNV64_PRIME		UINT64CONST(0x100000001b3)

struct ProllyChunker
{
	uint16		level;
	Size		capacity;
	uint64		threshold;		/* boundary if hash < threshold */
	Size		used;			/* item space in the current node */
	bool		pending;		/* the last item was a boundary that
								 * also forced a split */
};

static uint64
boundary_hash(uint16 level, const ProllyKey *key)
{
	uint64		h = FNV64_OFFSET;

	h = (h ^ (level & 0xff)) * FNV64_PRIME;
	h = (h ^ (level >> 8)) * FNV64_PRIME;
	for (uint32 i = 0; i < key->len; i++)
		h = (h ^ key->data[i]) * FNV64_PRIME;

	/* splitmix64 finalizer, to spread FNV's weak low bits */
	h ^= h >> 30;
	h *= UINT64CONST(0xbf58476d1ce4e5b9);
	h ^= h >> 27;
	h *= UINT64CONST(0x94d049bb133111eb);
	h ^= h >> 31;
	return h;
}

static ProllyChunker *
keyhash_begin(uint16 level, Size capacity, MemoryContext mcxt)
{
	ProllyChunker *c = MemoryContextAllocZero(mcxt, sizeof(ProllyChunker));
	uint64		target;

	target = capacity / (level == 0 ? LEAF_ITEM_BYTES : INTERNAL_ITEM_BYTES);
	target = Max(target, 2);

	c->level = level;
	c->capacity = capacity;
	c->threshold = PG_UINT64_MAX / target;
	return c;
}

static ProllyChunkDecision
keyhash_add(ProllyChunker *c, const ProllyKey *key, const ProllyValue *value,
			Size item_space)
{
	bool		boundary = boundary_hash(c->level, key) < c->threshold;

	if (c->pending || (c->used > 0 && c->used + item_space > c->capacity))
	{
		c->used = item_space;
		c->pending = boundary;
		return PROLLY_CHUNK_SPLIT_BEFORE;
	}

	c->used += item_space;
	if (boundary)
	{
		c->used = 0;
		return PROLLY_CHUNK_SPLIT_AFTER;
	}
	return PROLLY_CHUNK_CONTINUE;
}

static void
keyhash_reset(ProllyChunker *c)
{
	c->used = 0;
	c->pending = false;
}

static void
keyhash_end(ProllyChunker *c)
{
	pfree(c);
}

const ProllyChunkerRoutine prolly_chunker_keyhash = {
	.info = {
		.iface = PROLLY_IFACE_CHUNKER,
		.impl_id = 1,
		.format_version = 1,
		.name = "keyhash",
		.description = "boundary after keys whose hash is below a threshold; "
		"forced split when a page is full",
	},
	.begin = keyhash_begin,
	.add = keyhash_add,
	.reset = keyhash_reset,
	.end = keyhash_end,
};
