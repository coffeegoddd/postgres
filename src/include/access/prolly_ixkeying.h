/*-------------------------------------------------------------------------
 *
 * prolly_ixkeying.h
 *	  Index keying interface: how a prolly index's entries are made.
 *
 * A prolly index is a tree with one entry per row of its table: the
 * index's key columns, then the row's key in its table and its ordinal
 * among identical rows (prolly_keying.h), so the tree depends only on the
 * table's rows and never on TIDs.  Entries have no value.  The tree
 * records the description of its keying once, as a table's data tree
 * records its table keying; entries carry no schema.
 *
 * Contract:
 * - open() returns the keying of index as description describes it (NULL:
 *   as the index is now).  Everything it allocates is in mcxt.
 * - encode_entry() makes the entry for a row from its index column values;
 *   decode_entry() gives them back, with the row key and ordinal.
 * - encode_bound() makes a key that sorts just before (or, with upper,
 *   just after) every entry whose first nkeys columns equal values, for
 *   seeking; a bound is never stored.
 * - compare() orders entries and bounds: by the key columns as their btree
 *   operator classes order them (honouring DESC and NULLS FIRST/LAST),
 *   then by row key and ordinal.
 * - columns_equal() says whether two entries' first ncols columns are
 *   equal (NULLs are equal to NULLs here; callers handle NULLS DISTINCT).
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_ixkeying.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_IXKEYING_H
#define PROLLY_IXKEYING_H

#include "access/prolly_registry.h"
#include "access/prolly_types.h"
#include "lib/stringinfo.h"
#include "utils/relcache.h"

typedef struct ProllyIndexKeying ProllyIndexKeying;

typedef struct ProllyIndexKeyingRoutine
{
	ProllyImplInfo info;

	ProllyIndexKeying *(*open) (Relation index, const ProllyValue *description,
								MemoryContext mcxt);
	void		(*describe) (ProllyIndexKeying *k, StringInfo out);

	/* The number of key columns. */
	int			(*nkeys) (ProllyIndexKeying *k);

	void		(*encode_entry) (ProllyIndexKeying *k, const Datum *values,
								 const bool *isnull, const ProllyKey *rowkey,
								 uint64 ordinal, StringInfo out);
	/* rowkey points into entry. */
	void		(*decode_entry) (ProllyIndexKeying *k, const ProllyKey *entry,
								 Datum *values, bool *isnull, ProllyKey *rowkey,
								 uint64 *ordinal);
	void		(*encode_bound) (ProllyIndexKeying *k, int nkeys,
								 const Datum *values, const bool *isnull,
								 bool upper, StringInfo out);

	int			(*compare) (ProllyIndexKeying *k, const ProllyKey *a,
							const ProllyKey *b);
	bool		(*columns_equal) (ProllyIndexKeying *k, const ProllyKey *a,
								  const ProllyKey *b, int ncols);

	void		(*close) (ProllyIndexKeying *k);
} ProllyIndexKeyingRoutine;

#define PROLLY_IXKEYING_CALLBACK_NAMES \
	"open", "describe", "nkeys", "encode_entry", "decode_entry", \
	"encode_bound", "compare", "columns_equal", "close"
#define PROLLY_IXKEYING_FIRST_CALLBACK open

static inline const ProllyIndexKeyingRoutine *
prolly_ixkeying_lookup(const char *name)
{
	return (const ProllyIndexKeyingRoutine *)
		prolly_get_impl(PROLLY_IFACE_INDEX_KEYING, name);
}

#endif							/* PROLLY_IXKEYING_H */
