/*-------------------------------------------------------------------------
 *
 * prolly_keying.h
 *	  Table keying interface: how a table's rows become tree items.
 *
 * A table's data tree is a sorted map, but a Postgres table needn't have a
 * key, and the table AM never sees one at insert.  A keying decides what a
 * row's key is and how its value is laid out.  Implementation
 * "pk_or_prefix": a table with a primary key is keyed by its encoded key
 * columns; a table without one (or whose primary key is deferrable, so may
 * briefly hold duplicates) by the row's first bytes and its content hash,
 * with a count of identical rows in the value.
 *
 * Every row also has a content hash: the hash of its canonical encoding,
 * which names one version of the row.
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
 * - A keying is opened for one relation and is valid until the relation's
 *   relcache entry is rebuilt; callers reopen after invalidation.  It
 *   holds nothing but memory in the context it was opened in.
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
#include "access/prolly_tree.h"
#include "access/prolly_types.h"
#include "executor/tuptable.h"
#include "lib/stringinfo.h"
#include "utils/relcache.h"

typedef struct ProllyKeying ProllyKeying;

typedef struct ProllyKeyingRoutine
{
	ProllyImplInfo info;

	/*
	 * Rows of rel as keyed now, as they are once Postgres rewrites the
	 * table (CLUSTER, VACUUM FULL): the catalog's missing values of added
	 * columns are cleared then, so every row carries its own, and a folded
	 * row is the same however the table came to have it.
	 */
	ProllyKeying *(*open) (Relation rel, MemoryContext mcxt);

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

	/* Store the encoded row into slot (as a virtual tuple). */
	void		(*decode_row) (ProllyKeying *k, const ProllyValue *row,
							   TupleTableSlot *slot);

	int			(*compare) (ProllyKeying *k, const ProllyKey *a,
							const ProllyKey *b);

	void		(*close) (ProllyKeying *k);

	/*
	 * Where rows' values out of line are stored and read (the value
	 * encoder's set_values()), kept, not copied; and the references an
	 * encoded row holds.
	 */
	void		(*set_values) (ProllyKeying *k, const ProllyValueStore *values);
	void		(*row_refs) (ProllyKeying *k, const ProllyValue *row,
							 StringInfo refs);
} ProllyKeyingRoutine;

#define PROLLY_KEYING_CALLBACK_NAMES \
	"open", "keyless", "encode_row", "make_value", "parse_value", \
	"decode_row", "compare", "close", "set_values", "row_refs"
#define PROLLY_KEYING_FIRST_CALLBACK open
#define PROLLY_KEYING_LAST_CALLBACK row_refs

/*
 * Set up a data tree's environment and k's value store for each other:
 * k stores values out of line in env's chunk store with tree, and env
 * finds the references a tree value of k's holds (its value_refs()).
 * env and refs must outlive their use.
 */
typedef struct ProllyKeyingRefs
{
	const struct ProllyKeyingRoutine *routine;
	ProllyKeying *keying;
	ProllyValueStore values;
} ProllyKeyingRefs;

extern void prolly_keying_use_env(ProllyKeyingRefs *refs,
								  const struct ProllyKeyingRoutine *routine,
								  ProllyKeying *k,
								  const struct ProllyTreeRoutine *tree,
								  ProllyTreeEnv *env);

/* A value_refs() for a tree whose values are encoded rows themselves. */
extern void prolly_keying_row_refs(const ProllyValue *value, void *arg,
								   StringInfo refs);

#endif							/* PROLLY_KEYING_H */
