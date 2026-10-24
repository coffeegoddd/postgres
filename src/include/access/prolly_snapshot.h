/*-------------------------------------------------------------------------
 *
 * prolly_snapshot.h
 *	  Read-only snapshot mounts: a commit's tables as foreign tables.
 *
 * prolly_mount() resolves a ref to one commit and makes a foreign table of
 * the server prolly_local (foreign data wrapper prolly_snapshot) for each
 * of its tables asked for, in an ordinary schema, each defined as the
 * commit describes the table.  The refs pin the commit under the mount's
 * name (their pin_set()), so a collection keeps everything it reaches; a
 * foreign table's options name the mount, the commit and the table, and a
 * scan reads the commit's tree only while the mount pins it.  Scans seek
 * and stream: a primary key's equalities and a range after them bound the
 * keys read, and rows come in key order.
 *
 * src/include/access/prolly_snapshot.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_SNAPSHOT_H
#define PROLLY_SNAPSHOT_H

/*
 * Refuse the user reading the rows of the table named name (qualified,
 * quoted) as commit (hex) has them, as dolt_diff() by name refuses it: the
 * database's owner and members of pg_read_all_data read every version;
 * anyone else needs SELECT on the table of that name here, row security
 * not applying to them, and that table must be the commit's (the name
 * never without a table since).  With lineage false, the last isn't
 * checked (the mount checked it).  fn names the caller in errors.
 */
extern void prolly_check_history_by_name(const char *name, const char *commit,
										 bool lineage, const char *fn);

#endif							/* PROLLY_SNAPSHOT_H */
