/*-------------------------------------------------------------------------
 *
 * move_alter.h
 *	  A table a move keeps, altered in place from one definition to another.
 *
 * Whether a table can be is said by its two records alone, and the types it would add columns of,
 * before anything is done: by renaming, dropping and adding at the end its columns, setting and
 * dropping their defaults and not-null constraints, renaming those, dropping and adding its
 * constraints and indexes, renaming its key, and setting its storage parameters.  Anything else
 * has it made again: a column retyped or moved; a generated or identity column changed or added;
 * a column added of a domain, which adding with every row null would check, or of a type not made
 * yet, which may be one (domains names the target's); a not-null constraint renamed to a name
 * another constraint of the table has; its key, partitioning, inheritance, comments, owner, grants or any other
 * setting changed, an index's own among them; an index or constraint made again that has settings,
 * a comment, or is the replica identity's or clustered on; a table with no key whose columns
 * changed; or one that is partitioned, a partition, or inherits or is inherited from, which
 * related says.  A column retyped rewrites every row either way, and Postgres refuses to retype one that
 * a view or rule uses.
 *
 * What's done is in three parts: before the target's new objects are made, what it drops and
 * renames, which may free the names they take; then what the rows need the table to be (its
 * columns added, and defaults), which may use those objects; and after the rows, what needs them (not-null constraints, constraints and indexes the target
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

extern bool move_alterable(const WsTableSchema *from, const WsTableSchema *to, bool related,
						   const char *const *domains, int ndomains);
extern void move_alter_drop(const WsTableSchema *from, const WsTableSchema *to, const char *schema);
extern void move_alter_add(const WsTableSchema *from, const WsTableSchema *to, const char *schema);
extern void move_alter_after(const WsTableSchema *from, const WsTableSchema *to, const char *schema);

/* An error if the table isn't now as the record has it. */
extern void move_alter_check(const WsTableSchema *to, const char *schema);

/*
 * Whether altering drops a unique or exclusion constraint, or a unique index, which a foreign key
 * of another table may need: those are to be dropped first, and made again after.
 */
extern bool move_alter_drops_unique(const WsTableSchema *from, const WsTableSchema *to);

#endif							/* PROLLY_MOVE_ALTER_H */
