/*-------------------------------------------------------------------------
 *
 * prolly_chunkformat.h
 *	  Chunk node format interface: how a tree node is laid out in a chunk.
 *
 * A node kept in a chunk store (prolly_chunkstore.h) is a byte string of
 * its own length, not a page: all of it is canonical and hashed.  Every
 * chunk starts with the format's version byte.
 *
 * Contract:
 * - Items are (key, value, flags) in key order; in internal nodes (level
 *   > 0) the value is an encoded child reference.  An item's flags say
 *   whether its value refers to a value tree (PROLLY_ITEM_VALUE_REF), and
 *   whether it lists references to value trees that it holds
 *   (PROLLY_ITEM_VALUE_REFS); what they mean is the tree's.
 * - encode() depends only on the level and the items: the same node is
 *   the same bytes on every machine.
 * - open() checks a chunk is a node of this format and reads its header;
 *   item_get() points key and value into the chunk's bytes.
 * - item_space() is what an item adds to a node's size, so the chunker can
 *   cut nodes by size; header_size() what a node takes besides its items.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_chunkformat.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_CHUNKFORMAT_H
#define PROLLY_CHUNKFORMAT_H

#include "access/prolly_registry.h"
#include "access/prolly_types.h"
#include "lib/stringinfo.h"

/* Item flags. */
#define PROLLY_ITEM_VALUE_REF	0x01	/* the value refers to a value tree */
#define PROLLY_ITEM_VALUE_REFS	0x02	/* the value begins with the references
										 * to value trees it holds */

/* A node read from a chunk: points into the chunk's bytes. */
typedef struct ProllyChunkNode
{
	const uint8 *data;
	Size		len;
	uint16		level;
	int			nitems;
} ProllyChunkNode;

typedef struct ProllyChunkFormatRoutine
{
	ProllyImplInfo info;

	void		(*encode) (uint16 level, int nitems, const ProllyKey *keys,
						   const ProllyValue *values, const uint8 *flags,
						   StringInfo out);
	bool		(*open) (const uint8 *data, Size len, ProllyChunkNode *node);
	void		(*item_get) (const ProllyChunkNode *node, int index,
							 ProllyKey *key, ProllyValue *value, uint8 *flags);
	Size		(*item_space) (uint16 level, const ProllyKey *key,
							   const ProllyValue *value);
	Size		(*header_size) (void);
} ProllyChunkFormatRoutine;

#define PROLLY_CHUNKFORMAT_CALLBACK_NAMES \
	"encode", "open", "item_get", "item_space", "header_size"
#define PROLLY_CHUNKFORMAT_FIRST_CALLBACK encode
#define PROLLY_CHUNKFORMAT_LAST_CALLBACK header_size

static inline const ProllyChunkFormatRoutine *
prolly_chunkformat_lookup(const char *name)
{
	return (const ProllyChunkFormatRoutine *)
		prolly_get_impl(PROLLY_IFACE_CHUNK_FORMAT, name);
}

#endif							/* PROLLY_CHUNKFORMAT_H */
