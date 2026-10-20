/*-------------------------------------------------------------------------
 *
 * prolly_database.h
 *	  Keeping prolly state in step with CREATE and DROP DATABASE.
 *
 * A database's chunk store is files no catalog lists, so copying the
 * template's relations alone would leave its history behind.  createdb()
 * calls ProllyCopyDatabase() once the template's files are copied, which
 * passes the call to every registered chunk store.  The chunk store's
 * files go with the database's directory; dropdb() (and its redo, and a
 * CREATE DATABASE that fails) calls ProllyDropDatabase() for what a store
 * keeps of a database elsewhere (shared memory, sync requests), so an OID
 * used again starts anew.
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
