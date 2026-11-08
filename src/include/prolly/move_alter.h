/*-------------------------------------------------------------------------
 *
 * move_alter.h
 *	  A table a move keeps, altered in place from one definition to another.
 *
 * Whether a table can be is said by its two records alone, before anything is done: by
 * renaming, dropping and adding at the end its columns, setting and dropping their defaults
 * and not-null constraints, dropping and adding its constraints and indexes, renaming its key,
 * and setting its storage parameters.  Anything else (a column retyped or moved, a generated
 * or identity column changed or added, its key, partitioning, inheritance, comments, owner,
 * grants or any other setting changed, a table with no key whose columns changed, or one that is
 * partitioned, a partition, or inherits or is inherited from, which related says) has it made
 * again.  A column retyped rewrites every row either way, and Postgres refuses to retype one that
 * a view or rule uses.
 *
 * What's done is in two halves, around the rows being written: before, what the rows need
 * the table to be (its columns, with nothing they'd be refused by that the target doesn't have
 * too); after, what needs the rows (not-null constraints, constraints and indexes the target
 * adds, and storage parameters).  Then the table is read again, and anything but the target's
 * record is an error.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/move_alter.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_MOVE_ALTER_H
#define PROLLY_MOVE_ALTER_H

#include "prolly/ws/ws_schema.h"

extern bool move_alterable(const WsTableSchema *from, const WsTableSchema *to, bool related);
extern void move_alter_before(const WsTableSchema *from, const WsTableSchema *to, const char *schema);
extern void move_alter_after(const WsTableSchema *from, const WsTableSchema *to, const char *schema);

/* An error if the table isn't now as the record has it. */
extern void move_alter_check(const WsTableSchema *to, const char *schema);

#endif							/* PROLLY_MOVE_ALTER_H */
