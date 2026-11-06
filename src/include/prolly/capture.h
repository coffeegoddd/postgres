/*-------------------------------------------------------------------------
 *
 * capture.h
 *	  Keeping each branch's working tree equal to its tables, from the WAL.
 *
 * One worker per database with a versioned schema reads the WAL on one logical slot.  The
 * slot says which databases those are, as it lasts and needs no catalog of its own: a launcher,
 * started with the server, sees to a worker for each slot with none.  As that leaves a
 * database whose first versioned schema committed before its worker made a slot, the
 * launcher also starts a worker in each database once as it starts, which makes the slot if the
 * database has a versioned schema, and otherwise goes.  A first versioned schema starts its
 * worker as it commits, and a worker whose database has no versioned schema left drops the
 * slot, so that it holds no WAL.  wal_level minimal, or no replication slots, is no capture.
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

/* Stops a database's worker and drops its slot, as the database goes, with its lock held. */
extern void CaptureDatabaseDropping(Oid dboid);

/* What decoding calls. */
extern void CaptureOutputPluginInit(OutputPluginCallbacks *callbacks);

#endif							/* PROLLY_CAPTURE_H */
