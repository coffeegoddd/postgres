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
 * nothing but DDL is a begin and a commit.  A change is told with the branch its table's schema
 * was on at the time, and one on a shared schema isn't told, which leaves its transaction a begin
 * and a commit as well.  A transaction can change more than one branch.  A worker stopped in the
 * middle of telling a transaction tells all of it again: a begin means whatever came since the
 * last commit is void.  After a crash a transaction already told whole can be told again, so a
 * sink keeping anything goes by the positions it is given.  Someone with REPLICATION rights could
 * advance or drop the slot in the moment it has no worker, and what that skips isn't told.  A
 * materialized view's rows are never told, nor those of any table's rewrite.
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
#include "prolly/ws/ws_row.h"
#include "replication/output_plugin.h"
#include "utils/relcache.h"

/* The output plugin's name, which the server has built in. */
#define PROLLY_CAPTURE_PLUGIN "prolly_capture"

extern PGDLLIMPORT bool capture_in_background;
extern PGDLLIMPORT bool capture_to_trees;

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
	int32		branch;			/* never 0 */
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
	 * may be again.  What committed before a slot's first position never is, and new_slot says this
	 * is that first position.  With no transaction running.
	 */
	void		(*start) (XLogRecPtr from, bool new_slot);
	void		(*begin) (TransactionId xid, XLogRecPtr final_lsn);
	void		(*change) (const CaptureChange *change);

	/* A relation at a time, as they were named: a partitioned table, and then its partitions. */
	void		(*truncate) (int32 branch, Relation relation, bool restart_seqs, XLogRecPtr lsn);

	/*
	 * A logical message: transactional ones between begin and commit, others at once, once flushed,
	 * and with no transaction running.
	 */
	void		(*message) (const char *prefix, const char *data, Size size, bool transactional, XLogRecPtr lsn);
	void		(*commit) (TransactionId xid, XLogRecPtr commit_lsn, XLogRecPtr end_lsn);

	/*
	 * What it was told is to be kept: with no transaction running, as nothing can be written from
	 * the others.  The slot says what was told only once this returns, so what's lost before then
	 * is told again.
	 */
	void		(*apply) (void);
} CaptureSink;

/* Says what it is told, at DEBUG1, and does nothing with it. */
extern PGDLLIMPORT const CaptureSink LoggingCaptureSink;

/* Keeps each table's rows as a row map, as prolly/ws/ws_rows.h's capture makes them. */
extern PGDLLIMPORT const CaptureSink TreeCaptureSink;

/* The implementation in use. */
extern PGDLLIMPORT const CaptureSink *CaptureSinkInUse;

extern void CaptureLauncherRegister(void);
pg_noreturn extern void CaptureLauncherMain(Datum main_arg);
pg_noreturn extern void CaptureWorkerMain(Datum main_arg);

/* A versioned schema was made here: this database's worker is seen to as this commits. */
extern void CaptureSchemaMade(void);

/* Stops a database's worker, as the database goes, with its lock held; its slot goes with it. */
extern void CaptureDatabaseDropping(Oid dboid);

/*
 * What a relation's rows are made of, as prolly/ws/ws_schema.h's ws_schema_row_shape gives: its
 * stored columns' tags, the primary key's first and in key order, the caller's to
 * ws_row_shape_free.  A relation a sink is told of is as the catalog was at that change, so a row
 * goes by the layout it was written under, whatever DDL came after it, in that transaction or not.
 * Two layouts alike don't say the rows are: DDL can change a table's rows and tell no change of them.
 */
extern void CaptureRelationShape(Relation relation, WsRowShape *shape);

/* What decoding calls. */
extern void CaptureOutputPluginInit(OutputPluginCallbacks *callbacks);

#endif							/* PROLLY_CAPTURE_H */
