/*-------------------------------------------------------------------------
 *
 * prolly_nodeformat.h
 *	  Node page format interface: how a tree node is laid out in a page.
 *
 * A node is a Postgres page with a standard PageHeaderData.  It has a
 * canonical region, whose bytes are hashed, and an unhashed special space
 * for physical metadata (read hints, cached hash, flags, and for D4 version
 * and lock data).
 *
 * Contract:
 * - Items in a node are (key, value) pairs in key order.  In internal nodes
 *   the value is an encoded child reference.
 * - Each item in an internal node (level > 0) also has a block hint: where
 *   the child was stored when the node was written.  Hints are outside the
 *   canonical region, so they never affect the hash.  Leaves have none.
 * - The canonical region depends only on the node's items and level, never
 *   on how or in what order they were written.
 * - Callbacks don't lock or pin; the caller holds the buffer appropriately.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_nodeformat.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_NODEFORMAT_H
#define PROLLY_NODEFORMAT_H

#include "access/prolly_registry.h"
#include "access/prolly_types.h"
#include "storage/bufpage.h"

typedef struct ProllyNodeFormatRoutine
{
	ProllyImplInfo info;

	/* Initialize an empty node at level (0 = leaf) in page. */
	void		(*page_init) (Page page, Size page_size, uint16 level);

	uint16		(*page_level) (Page page);
	int			(*item_count) (Page page);

	/* Item index (0-based); key and value point into the page. */
	void		(*item_get) (Page page, int index, ProllyKey *key,
							 ProllyValue *value);

	/*
	 * Append an item after the last one; false if it doesn't fit.  hint is
	 * the child's block in an internal node (InvalidBlockNumber if unknown),
	 * and is ignored in a leaf.
	 */
	bool		(*item_append) (Page page, const ProllyKey *key,
								const ProllyValue *value, BlockNumber hint);

	/* The item's block hint; InvalidBlockNumber in a leaf or if unknown. */
	BlockNumber (*item_hint) (Page page, int index);

	/*
	 * Space an item would take in a node at level, and the free space left
	 * in page.  The chunker uses these for its deterministic forced split.
	 */
	Size		(*item_space) (uint16 level, const ProllyKey *key,
							   const ProllyValue *value);
	Size		(*free_space) (Page page);

	/* The bytes to hash: the canonical region. */
	void		(*canonical_bytes) (Page page, const uint8 **data, Size *len);

	/* The unhashed special space. */
	char	   *(*special_space) (Page page, Size *len);

	/* Structural check for amcheck-style verification. */
	bool		(*page_check) (Page page, char **detail);
} ProllyNodeFormatRoutine;

#define PROLLY_NODEFORMAT_CALLBACK_NAMES \
	"page_init", "page_level", "item_count", "item_get", "item_append", \
	"item_hint", "item_space", "free_space", "canonical_bytes", \
	"special_space", "page_check"
#define PROLLY_NODEFORMAT_FIRST_CALLBACK page_init

static inline const ProllyNodeFormatRoutine *
prolly_nodeformat_lookup(const char *name)
{
	return (const ProllyNodeFormatRoutine *)
		prolly_get_impl(PROLLY_IFACE_NODE_FORMAT, name);
}

#endif							/* PROLLY_NODEFORMAT_H */
