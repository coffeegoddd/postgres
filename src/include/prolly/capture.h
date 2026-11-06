/*-------------------------------------------------------------------------
 *
 * capture.h
 *	  Keeping each branch's working tree equal to its tables, from the WAL.
 *
 * One worker per database with a versioned schema reads the WAL on one logical slot.
 * pg_database.datversioned says which databases those are: the first versioned schema sets it
 * in its own transaction, and a database copied from one has it too.  A launcher, started
 * with the server, sees to a worker for each so marked, and drops a capture slot of any other.  A
 * worker whose database has no versioned schema left clears the mark and drops the slot, so
 * that it holds no WAL.  A slot lost to max_slot_wal_keep_size is made again.
 *
 * wal_level minimal, or no replication slots, is no capture.  As with any logical slot, a
 * server with one will not start with those set, and capture's slots count against
 * max_replication_slots.  Each such database takes a worker of max_worker_processes, and the
 * launcher another.
 *
 * capture_in_background turns all of this off, and the slots go with it.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/capture.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_CAPTURE_H
#define PROLLY_CAPTURE_H

#include "replication/output_plugin.h"

/* The output plugin's name, which the server has built in. */
#define PROLLY_CAPTURE_PLUGIN "prolly_capture"

extern PGDLLIMPORT bool capture_in_background;

extern void CaptureLauncherRegister(void);
pg_noreturn extern void CaptureLauncherMain(Datum main_arg);
pg_noreturn extern void CaptureWorkerMain(Datum main_arg);

/* A versioned schema was made here: this database's worker is seen to as this commits. */
extern void CaptureSchemaMade(void);

/* Stops a database's worker, as the database goes, with its lock held; its slot goes with it. */
extern void CaptureDatabaseDropping(Oid dboid);

/* What decoding calls. */
extern void CaptureOutputPluginInit(OutputPluginCallbacks *callbacks);

#endif							/* PROLLY_CAPTURE_H */
