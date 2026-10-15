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

#endif							/* PROLLY_TABLEAM_H */
