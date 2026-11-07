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
#include "prolly/engine/prolly.h"
#include "prolly/ws/ws_row.h"
#include "utils/snapshot.h"
#include "replication/output_plugin.h"
#include "utils/relcache.h"

/*
 * A logical message of a table's rows, each of whose int32s and the like are as this server keeps
 * them: its first byte says which.
 *	CAPTURE_ROWS_BEGIN	int32 branch, relid, nkey, nstored; uint64 tags[nstored]: all that's been told
 *						of the table is void, and these are its rows.  With nkey 0 there are none to
 *						follow, as nothing can hold them
 *	CAPTURE_ROWS_BATCH	int32 branch, relid, nrows; each row nstored fields, each int32 length (-1:
 *						NULL) and that much of its output text
 */
#define PROLLY_MESSAGE_PREFIX "prolly."

/* A barrier's marker, and where a branch's row maps are saved as of it: <lsn>/<relid>. */
#define CAPTURE_BARRIER_PREFIX PROLLY_MESSAGE_PREFIX "capture.barrier"
#define CAPTURE_BARRIER_REF_PREFIX "capture/barrier/"

#define CAPTURE_SLOT_PREFIX "prolly_capture_"
#define CAPTURE_ROWS_PREFIX PROLLY_MESSAGE_PREFIX "capture.rows"
#define CAPTURE_ROWS_BEGIN 'R'
#define CAPTURE_ROWS_BATCH 'D'

/* The sub-id of the lock on a database that has what changes its tables' rows, untold, take turns. */
#define CAPTURE_ROWS_LOCK 2

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
	bool		catalogs_changed;	/* its transaction changed catalogs */
} CaptureChange;

/* What decoding finds. */
typedef struct CaptureSink
{
	const char *name;

	/* Whether it reads tables under a new slot's snapshot, which holds up vacuum cluster-wide. */
	bool		reads_at_start;

	/*
	 * Each time a worker starts: all that commits after this position is told, and some before it
	 * may be again.  What committed before a slot's first position never is.  With no transaction
	 * running.
	 */
	void		(*start) (XLogRecPtr from);
	void		(*begin) (TransactionId xid, XLogRecPtr final_lsn);
	void		(*change) (const CaptureChange *change);

	/* A relation at a time, as they were named: a partitioned table, and then its partitions. */
	void		(*truncate) (int32 branch, Relation relation, bool restart_seqs, XLogRecPtr lsn);

	/*
	 * A logical message: transactional ones between begin and commit, others at once, once flushed,
	 * outside any transaction told, though as decoding's other callbacks are, so nothing can be
	 * written from it.
	 */
	void		(*message) (const char *prefix, const char *data, Size size, bool transactional, XLogRecPtr lsn);
	void		(*commit) (TransactionId xid, XLogRecPtr commit_lsn, XLogRecPtr end_lsn);

	/*
	 * What it was told is to be kept: with no transaction running, as nothing can be written from
	 * the others.  The slot says what was told only once this returns, so what's lost before then
	 * is told again.
	 */
	void		(*apply) (void);

	/*
	 * Once a record's been decoded, before the next is read, with no transaction running.  True if
	 * all that's been told has been applied, so that the slot can be told of it.
	 */
	bool		(*after_record) (void);

	/*
	 * A new slot is being made, so what the sink has kept may not join what it is told next.  With
	 * no transaction running and the slot's name held, and before the slot is kept, as a worker can
	 * die just after.
	 */
	void		(*slot_to_be_made) (void);

	/*
	 * The new slot's first position is from: in the transaction running, whose snapshot has all that
	 * committed before it, and which commits before the slot is kept.
	 */
	void		(*slot_made) (XLogRecPtr from);

	/* Whether a slot already there is one this sink began from, with no transaction running. */
	bool		(*slot_will_do) (void);
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

/*
 * Each of a table's rows in a snapshot, as output text in the shape's order (UTF8, or NULL), under the
 * settings of a statement's scope.  Read directly: nothing is planned, so what's in a table's index
 * expressions isn't run, however privileged the caller.
 */
typedef void (*CaptureRowFn) (void *arg, const char **fields);
extern void CaptureTableRowsRead(Relation rel, const WsRowShape *shape, Snapshot snapshot,
								 CaptureRowFn fn, void *arg);

/*
 * A versioned schema's root value as of an exact point, every transaction committed before it and
 * none after, its chunks put with ctx, to be committed in this transaction; capture_barrier.c says
 * what it holds off and refuses.  An error if capture hasn't got there within timeout_ms, which is
 * only of waiting for capture, not of waiting for locks or of building the root.
 */
extern void CaptureBarrier(ProllyCtx *ctx, const char *schema, int timeout_ms, PHash *root);

/*
 * The replica identity a change of this table logs by, in place of its own.  With capture on, a
 * versioned table logs its whole old row unless it has a primary key that identifies a row by
 * itself, so that a table with no key, one whose key is deferrable and one set to log nothing or
 * another index all say what a change was of.  What the table says is left as it is, being the
 * user's, and what a commit holds.  It reads catalogs, so it's not for when a page is locked.
 */
extern char CaptureLoggedIdentity(Relation relation);

/* As a transaction commits or prepares: tells capture of what it did to rows and told no change of. */
extern void CaptureRowsAtCommit(bool prepare);

/* What makes up what a table's rows are, of the catalogs as they are for this reader. */
extern char *CaptureRowsFingerprint(Oid relid, Oid relfilenode);

/* This is capture's own worker. */
extern PGDLLIMPORT bool am_capture_worker;

/* What decoding calls. */
extern void CaptureOutputPluginInit(OutputPluginCallbacks *callbacks);

#endif							/* PROLLY_CAPTURE_H */
