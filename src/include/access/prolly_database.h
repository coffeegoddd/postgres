/*-------------------------------------------------------------------------
 *
 * prolly_database.h
 *	  Keeping prolly state in step with CREATE DATABASE and DROP DATABASE.
 *
 * A database's prolly tables are found from its branch heads, which live
 * outside its directory, so copying or removing the directory alone would
 * lose them or leave them behind.  createdb() calls ProllyCopyDatabase()
 * once the template's files are copied; dropdb() calls
 * ProllyDropDatabase() as it removes the database's files.  Each passes the
 * call to every registered commit pipeline.
 *
 * src/include/access/prolly_database.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_DATABASE_H
#define PROLLY_DATABASE_H

extern void ProllyCopyDatabase(Oid src_dboid, Oid src_spcoid,
							   Oid dst_dboid, Oid dst_spcoid);
extern void ProllyDropDatabase(Oid dboid);

#endif							/* PROLLY_DATABASE_H */
