/*-------------------------------------------------------------------------
 *
 * chunker_bytes.c
 *	  Chunker implementation "bytes": boundaries by the bytes a node holds.
 *
 * A node ends after an item with a probability that grows with the node's
 * size, so nodes are about NODE_TARGET bytes whatever their items' sizes:
 * the node's size follows a Weibull distribution (shape 4, mean
 * NODE_TARGET), and no node ends before NODE_MIN bytes.  Whether an item
 * ends its node is decided by a hash of its key alone, seeded by the
 * level, against the chance of a boundary between the node's size before
 * and after the item; so updating a value moves a boundary only by
 * changing the sizes, and inserting or deleting a key moves at most the
 * boundaries next to it.
 *
 * The distribution is a table of its CDF, scaled to 2^32, in steps of 64
 * bytes; compared in integers, so every platform decides alike.  Past the
 * table the CDF is 1: a node that large ends at its next item.  A node
 * that the next item would push past capacity ends before it (a forced
 * split); if that item would also end a new node, it is alone in it.
 *
 * The table, the target and the hash are part of the canonical format.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/chunker_bytes.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"

#define NODE_MIN		512
#define NODE_TARGET		4096	/* the distribution's mean */
#define CDF_STEP		64

#define FNV64_OFFSET	UINT64CONST(0xcbf29ce484222325)
#define FNV64_PRIME		UINT64CONST(0x100000001b3)

/*
 * The Weibull CDF (shape 4, scale 4518.96, mean 4096), times 2^32, at
 * 0, 64, 128, ... bytes; 1 (0xFFFFFFFF) from 9792 bytes on.
 */
static const uint32 weibull_cdf[] = {
	0x00000000, 0x000000AD, 0x00000ACD, 0x000036AC, 0x0000ACCB, 0x0001A5DA,
	0x00036ABD, 0x00065486, 0x000ACC73, 0x00114BE4, 0x001A5C57, 0x00269753,
	0x0036A656, 0x004B42BE, 0x0065359D, 0x00855792, 0x00AC908E, 0x00DBD78B,
	0x01143237, 0x0156B48C, 0x01A48054, 0x01FEC499, 0x0266BCFC, 0x02DDB0EF,
	0x0364F2DC, 0x03FDDF21, 0x04A9DAF3, 0x056A5320, 0x0640BAA9, 0x072E8932,
	0x08353956, 0x095646C5, 0x0A932C44, 0x0BED617B, 0x0D66589C, 0x0EFF7BE3,
	0x10BA2AEA, 0x1297B7D8, 0x14996471, 0x16C05EFD, 0x190DBF1A, 0x1B828271,
	0x1E1F8963, 0x20E5939E, 0x23D53CBB, 0x26EEF8DC, 0x2A33115A, 0x2DA1A190,
	0x313A93C4, 0x34FD9E4D, 0x38EA40E7, 0x3CFFC263, 0x413D2E9C, 0x45A154E3,
	0x4A2AC6D2, 0x4ED7D7A0, 0x53A69C04, 0x5894EAAA, 0x5DA05D4A, 0x62C65269,
	0x6803EFC5, 0x6D56257A, 0x72B9B1D6, 0x782B25E6, 0x7DA6EAAF, 0x83294713,
	0x88AE6654, 0x8E325F25, 0x93B13B42, 0x9926FF66, 0x9E8FB3A8, 0xA3E76BFA,
	0xA92A50DC, 0xAE54A7FB, 0xB362DCB7, 0xB851885E, 0xBD1D7A0B, 0xC1C3BDF5,
	0xC641A419, 0xCA94C628, 0xCEBB0C87, 0xD2B2B26A, 0xD67A48D2, 0xDA10B881,
	0xDD7542C8, 0xE0A78126, 0xE3A763D5, 0xE6752F2B, 0xE91177FB, 0xEB7D1EEF,
	0xEDB94B0E, 0xEFC76375, 0xF1A90870, 0xF3600C14, 0xF4EE6A7D, 0xF65641DE,
	0xF799CA7E, 0xF8BB4ED6, 0xF9BD23E4, 0xFAA1A1E8, 0xFB6B1D8D, 0xFC1BE1B3,
	0xFCB629D9, 0xFD3C1D3E, 0xFDAFCAC6, 0xFE13259F, 0xFE6802AC, 0xFEB016AB,
	0xFEECF514, 0xFF200F9E, 0xFF4AB65A, 0xFF6E1858, 0xFF8B44BC, 0xFFA32C2F,
	0xFFB6A2A4, 0xFFC66149, 0xFFD308A7, 0xFFDD22CD, 0xFFE52586, 0xFFEB7488,
	0xFFF0638C, 0xFFF4384C, 0xFFF72C65, 0xFFF96F0C, 0xFFFB269D, 0xFFFC7204,
	0xFFFD69F5, 0xFFFE2205, 0xFFFEA996, 0xFFFF0CA3, 0xFFFF546B, 0xFFFF8803,
	0xFFFFACC8, 0xFFFFC6C5, 0xFFFFD8F9, 0xFFFFE59E, 0xFFFFEE52, 0xFFFFF442,
	0xFFFFF846, 0xFFFFFAF6, 0xFFFFFCBF, 0xFFFFFDEB, 0xFFFFFEAE, 0xFFFFFF2C,
	0xFFFFFF7C, 0xFFFFFFAF, 0xFFFFFFCF, 0xFFFFFFE2, 0xFFFFFFEE, 0xFFFFFFF6,
	0xFFFFFFFA, 0xFFFFFFFC, 0xFFFFFFFE, 0xFFFFFFFF,
};

struct ProllyChunker
{
	uint16		level;
	Size		capacity;
	Size		used;			/* bytes in the current node */
	bool		pending;		/* the last item forced a split and also
								 * ends its new node */
};

static uint32
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
	return (uint32) (h >> 32);
}

static uint64
cdf(Size size)
{
	Size		i = size / CDF_STEP;

	if (i >= lengthof(weibull_cdf))
		return PG_UINT32_MAX;
	return weibull_cdf[i];
}

/*
 * Does an item growing a node from before to after bytes end it?  With
 * probability (F(after) - F(before)) / (1 - F(before)): the chance the
 * node ends here, given that it didn't before.
 */
static bool
ends_node(uint16 level, const ProllyKey *key, Size before, Size after)
{
	uint64		f0;
	uint64		f1;
	uint64		h;

	if (after < NODE_MIN)
		return false;
	f0 = cdf(before);
	f1 = cdf(after);
	if (f0 >= PG_UINT32_MAX)
		return true;
	h = boundary_hash(level, key);
	/* h / 2^32 < (f1 - f0) / (2^32 - f0) */
	return h * ((UINT64CONST(1) << 32) - f0) < (f1 - f0) << 32;
}

static ProllyChunker *
bytes_begin(uint16 level, Size capacity, MemoryContext mcxt)
{
	ProllyChunker *c = MemoryContextAllocZero(mcxt, sizeof(ProllyChunker));

	c->level = level;
	c->capacity = capacity;
	return c;
}

static ProllyChunkDecision
bytes_add(ProllyChunker *c, const ProllyKey *key, const ProllyValue *value,
		  Size item_space)
{
	if (c->pending || (c->used > 0 && c->used + item_space > c->capacity))
	{
		c->used = item_space;
		c->pending = ends_node(c->level, key, 0, item_space);
		return PROLLY_CHUNK_SPLIT_BEFORE;
	}

	if (ends_node(c->level, key, c->used, c->used + item_space))
	{
		c->used = 0;
		return PROLLY_CHUNK_SPLIT_AFTER;
	}
	c->used += item_space;
	return PROLLY_CHUNK_CONTINUE;
}

static void
bytes_reset(ProllyChunker *c)
{
	c->used = 0;
	c->pending = false;
}

static void
bytes_end(ProllyChunker *c)
{
	pfree(c);
}

const ProllyChunkerRoutine prolly_chunker_bytes = {
	.info = {
		.iface = PROLLY_IFACE_CHUNKER,
		.impl_id = 2,
		.format_version = 1,
		.name = "bytes",
		.description = "boundary after items as nodes reach about 4 kB "
		"(Weibull by bytes, key-hashed); forced split at capacity",
	},
	.begin = bytes_begin,
	.add = bytes_add,
	.reset = bytes_reset,
	.end = bytes_end,
};
