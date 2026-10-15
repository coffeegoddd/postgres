/*-------------------------------------------------------------------------
 *
 * prolly_indexam.h
 *	  What the prolly table AM asks of the prolly index AM.
 *
 * Postgres asks an index AM only to insert entries; a prolly index holds
 * exactly its table's rows, so the table AM also has it remove the entries
 * of a row version that goes, and rebuild when the table's row keys change
 * (the table was re-keyed).
 *
 * src/include/access/prolly_indexam.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_INDEXAM_H
#define PROLLY_INDEXAM_H

#include "access/prolly_types.h"
#include "executor/tuptable.h"
#include "utils/relcache.h"

/* Is index a prolly index? */
extern bool prolly_index_is_prolly(Relation index);

/*
 * Remove, from every prolly index of heap, the entries of the row in slot
 * whose row key is rowkey with ordinal ordinal, tagged with cid.
 */
extern void prolly_index_remove_row(Relation heap, TupleTableSlot *slot,
									const ProllyKey *rowkey, uint64 ordinal,
									CommandId cid);

/* Rebuild every prolly index of heap from its rows. */
extern void prolly_index_rebuild_all(Relation heap);

/*
 * Lock the values the row in slot has in heap's unique indexes of other
 * access methods (btree's), as a prolly unique index locks its own: so
 * their writers take turns, and a uniqueness check that meets a TID it
 * can't resolve knows the row's writer is gone.
 */
extern void prolly_lock_unique_values(Relation heap, TupleTableSlot *slot);

/*
 * Rebuild index from heap's rows as every write so far sees them; the
 * caller holds the right to write heap, so nothing changes meanwhile.
 */
extern void prolly_index_rebuild(Relation heap, Relation index);

#endif							/* PROLLY_INDEXAM_H */
