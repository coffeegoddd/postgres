/*-------------------------------------------------------------------------
 *
 * prolly_tableam.h
 *	  What the prolly table access method offers the rest of prolly.
 *
 * src/include/access/prolly_tableam.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_TABLEAM_H
#define PROLLY_TABLEAM_H

#include "utils/relcache.h"

/*
 * Make rel's rows canonical, as writes of the current transaction: rows
 * stored before columns were dropped, or added with defaults, are written
 * again as the table is now.  Does nothing if they already are.
 */
extern void prolly_canonicalize_table(Relation rel);

/*
 * A rewrite swaps the storage of relations r1 and r2 (swap_relation_files,
 * before the catalogs change): their trees on the current branch are
 * swapped too, as trees are found by relation, not by storage.
 */
extern void prolly_swap_relations(Oid r1, Oid relam1, Oid r2, Oid relam2);

/* rel, a prolly table or index, is being dropped: its trees go too. */
extern void prolly_drop_relation(Relation rel);

#endif							/* PROLLY_TABLEAM_H */
