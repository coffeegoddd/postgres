/*-------------------------------------------------------------------------
 *
 * prolly_database.h
 *	  Keeping prolly state in step with CREATE DATABASE.
 *
 * A database's chunk store is files no catalog lists, so copying the
 * template's relations alone would leave its history behind.  createdb()
 * calls ProllyCopyDatabase() once the template's files are copied, which
 * passes the call to every registered chunk store.  (DROP DATABASE needs
 * nothing: the chunk store is in the database's directory.)
 *
 * src/include/access/prolly_database.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_DATABASE_H
#define PROLLY_DATABASE_H

extern void ProllyCopyDatabase(Oid src_dboid, Oid src_spcoid,
							   Oid dst_dboid, Oid dst_spcoid);

#endif							/* PROLLY_DATABASE_H */
