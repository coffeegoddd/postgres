/*-------------------------------------------------------------------------
 *
 * prolly_keying.h
 *	  Table keying interface: how a table's rows become tree items.
 *
 * A table's data tree is a sorted map, but a Postgres table needn't have a
 * key, and the table AM never sees one at insert.  A keying decides what a
 * row's key is and how its value is laid out.  Implementation
 * "pk_or_content": a table with a primary key is keyed by its encoded key
 * columns; a table without one (or whose primary key is deferrable, so may
 * briefly hold duplicates) by the row's content hash, with a count of
 * identical rows in the value.
 *
 * Every row also has a content hash: the hash of its canonical encoding,
 * which names one version of the row (row identity uses it for TIDs).
 *
 * Contract:
 * - Keys and values depend only on the row's logical content (they are
 *   part of the canonical format), never on how or when it was written.
 * - A table keyed by its primary key has exactly one row per key, so
 *   make_value() is only given count 1 for it; a keyless table's value
 *   carries count >= 1.
 * - decode_row(encode_row(slot)) gives back the same attribute values.
 * - decode_row() fills the slot in the slot's own descriptor, which may
 *   give columns other types than the relation has now: a table rewrite
 *   reads the old rows with the types they were written with.
 * - compare() orders keys as the data tree must be ordered.
 * - can_compare() is false when compare() can no longer order the keys
 *   of a keying opened from a description, because something it orders
 *   by (a key column's type, operator family or collation) has since
 *   been dropped.  Callers can still match keys by their bytes, which
 *   agrees with compare() except under a nondeterministic collation.
 * - A keying is opened for one relation and is valid until the relation's
 *   relcache entry is rebuilt; callers reopen after invalidation.  It
 *   holds nothing but memory in the context it was opened in.
 * - describe() gives bytes that identify how rows are keyed and encoded
 *   (local, not canonical); open() with them reads rows written that way,
 *   whatever the relation looks like now.  That changes when a primary key
 *   is added or dropped, when Postgres rewrites the table through one
 *   without a key, and when columns are dropped without a rewrite.  A
 *   tree records the description it was written with; can_read() says
 *   whether such rows still read correctly as the relation is now, and if
 *   not the tree is rebuilt before it is next written.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_keying.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_KEYING_H
#define PROLLY_KEYING_H

#include "access/prolly_registry.h"
#include "access/prolly_types.h"
#include "executor/tuptable.h"
#include "lib/stringinfo.h"
#include "utils/relcache.h"

typedef struct ProllyKeying ProllyKeying;

/*
 * The key, in a table's PROLLY_TREE_META tree, of the description of the
 * keying its data tree was written with.
 */
#define PROLLY_META_KEYING		"keying"
#define PROLLY_META_KEYING_LEN	6

typedef struct ProllyKeyingRoutine
{
	ProllyImplInfo info;

	/*
	 * Rows of rel as keyed now (description NULL), or as described by an
	 * earlier describe().
	 */
	ProllyKeying *(*open) (Relation rel, const ProllyValue *description,
						   MemoryContext mcxt);

	/* Append the description of this keying. */
	void		(*describe) (ProllyKeying *k, StringInfo out);

	/* True if rows written as description reads read correctly with k. */
	bool		(*can_read) (ProllyKeying *k, const ProllyValue *description);

	/* True if rows are keyed by content (and values carry a count). */
	bool		(*keyless) (ProllyKeying *k);

	/*
	 * Encode slot's row: append its canonical encoding to row and its key to
	 * key, and set its content hash.
	 */
	void		(*encode_row) (ProllyKeying *k, TupleTableSlot *slot,
							   StringInfo key, StringInfo row,
							   ProllyHash *content);

	/* The tree value for count copies of row; and back. */
	void		(*make_value) (ProllyKeying *k, const ProllyValue *row,
							   uint64 count, StringInfo value);
	void		(*parse_value) (ProllyKeying *k, const ProllyValue *value,
								ProllyValue *row, uint64 *count);

	/* The content hash of an encoded row. */
	void		(*content_hash) (ProllyKeying *k, const ProllyValue *row,
								 ProllyHash *content);

	/* Store the encoded row into slot (as a virtual tuple). */
	void		(*decode_row) (ProllyKeying *k, const ProllyValue *row,
							   TupleTableSlot *slot);

	int			(*compare) (ProllyKeying *k, const ProllyKey *a,
							const ProllyKey *b);
	bool		(*can_compare) (ProllyKeying *k);

	void		(*close) (ProllyKeying *k);
} ProllyKeyingRoutine;

#define PROLLY_KEYING_CALLBACK_NAMES \
	"open", "describe", "can_read", "keyless", "encode_row", "make_value", \
	"parse_value", "content_hash", "decode_row", "compare", "can_compare", \
	"close"
#define PROLLY_KEYING_FIRST_CALLBACK open

static inline const ProllyKeyingRoutine *
prolly_keying_lookup(const char *name)
{
	return (const ProllyKeyingRoutine *)
		prolly_get_impl(PROLLY_IFACE_TABLE_KEYING, name);
}

#endif							/* PROLLY_KEYING_H */
