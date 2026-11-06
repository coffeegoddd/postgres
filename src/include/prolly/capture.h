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
 * The worker decodes in-process, on a plugin the server has built in, so a change is a tuple
 * and not text.  What decoding finds goes to a CaptureSink, in commit order, a transaction at a
 * time and only once it has committed: what is rolled back is never seen, and a transaction of
 * nothing but DDL is a begin and a commit.
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
	HeapTuple	old_tuple;		/* what replica identity gives of the row, or NULL */
	HeapTuple	new_tuple;		/* an unchanged TOAST value is not in it; NULL for a delete */
	XLogRecPtr	lsn;
} CaptureChange;

/* What decoding finds. */
typedef struct CaptureSink
{
	const char *name;
	void		(*begin) (TransactionId xid, XLogRecPtr final_lsn);
	void		(*change) (const CaptureChange *change);
	void		(*truncate) (Relation *relations, int n, bool restart_seqs, XLogRecPtr lsn);

	/* A logical message: transactional ones between begin and commit, others at once. */
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

/* Stops a database's worker and drops its slot, as the database goes, with its lock held. */
extern void CaptureDatabaseDropping(Oid dboid);

/* What decoding calls. */
extern void CaptureOutputPluginInit(OutputPluginCallbacks *callbacks);

#endif							/* PROLLY_CAPTURE_H */
