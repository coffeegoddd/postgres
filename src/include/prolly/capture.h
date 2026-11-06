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
 * The worker decodes in-process, on a plugin the server has built in, so a change is a tuple
 * and not text.  What decoding finds goes to a CaptureSink, in commit order, a transaction at a
 * time and only once it has committed: what is rolled back is never seen, and a transaction of
 * nothing but DDL is a begin and a commit.  A worker stopped in the middle of telling a transaction
 * tells all of it again: a begin means whatever came since the last commit is void.  After a crash
 * a transaction already told whole can be told again, so a sink keeping anything goes by the
 * positions it is given.  Someone with REPLICATION rights could advance or drop the slot in the
 * moment it has no worker, and what that skips isn't told.  A materialized
 * view's rows are never told, nor those of any table's rewrite.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/capture.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_CAPTURE_H
#define PROLLY_CAPTURE_H

#include "access/htup.h"
#include "access/xlogdefs.h"
#include "replication/output_plugin.h"
#include "utils/relcache.h"

/* The output plugin's name, which the server has built in. */
#define PROLLY_CAPTURE_PLUGIN "prolly_capture"

extern PGDLLIMPORT bool capture_in_background;

typedef enum CaptureChangeKind
{
	CAPTURE_INSERT,
	CAPTURE_UPDATE,
	CAPTURE_DELETE,
} CaptureChangeKind;

/* One row's change.  What it points to is gone when the sink returns. */
typedef struct CaptureChange
{
	CaptureChangeKind kind;
	Relation	relation;
	HeapTuple	old_tuple;		/* what replica identity gives of the row, or NULL:
								 * NULL for a delete if it gives nothing */
	HeapTuple	new_tuple;		/* an unchanged TOAST value is not in it; NULL for a delete */
	XLogRecPtr	lsn;
} CaptureChange;

/* What decoding finds. */
typedef struct CaptureSink
{
	const char *name;

	/*
	 * Each time a worker starts: all that commits after this position is told, and some before it
	 * may be again.  What committed before a slot's first position never is.  With no transaction
	 * running.
	 */
	void		(*start) (XLogRecPtr from);
	void		(*begin) (TransactionId xid, XLogRecPtr final_lsn);
	void		(*change) (const CaptureChange *change);

	/* Named as they were: a partitioned table, and then its partitions. */
	void		(*truncate) (Relation *relations, int n, bool restart_seqs, XLogRecPtr lsn);

	/*
	 * A logical message: transactional ones between begin and commit, others at once, once flushed,
	 * and with no transaction running.
	 */
	void		(*message) (const char *prefix, const char *data, Size size, bool transactional, XLogRecPtr lsn);
	void		(*commit) (TransactionId xid, XLogRecPtr commit_lsn, XLogRecPtr end_lsn);
} CaptureSink;

/* Says what it is told, at DEBUG1, and does nothing with it. */
extern PGDLLIMPORT const CaptureSink LoggingCaptureSink;

/* The implementation in use. */
extern PGDLLIMPORT const CaptureSink *CaptureSinkInUse;

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
