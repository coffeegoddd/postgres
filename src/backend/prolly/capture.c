/*-------------------------------------------------------------------------
 *
 * capture.c
 *	  Keeping each branch's working tree equal to its tables, from the WAL.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/capture.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "access/xlog_internal.h"
#include "catalog/indexing.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
#include "lib/stringinfo.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "prolly/branch_catalog.h"
#include "prolly/branch_idle.h"
#include "prolly/capture.h"
#include "prolly/chunkstore.h"
#include "prolly/engine/vc_refs.h"
#include "replication/decode.h"
#include "replication/logical.h"
#include "replication/logicalctl.h"
#include "replication/message.h"
#include "replication/reorderbuffer.h"
#include "replication/slot.h"
#include "replication/snapbuild.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/injection_point.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
#include "utils/wait_event.h"

/* How often the launcher looks, and how often a worker looks for what is new. */
#define CAPTURE_LAUNCHER_NAPTIME_MS 5000
#define CAPTURE_WORKER_NAPTIME_MS 1000

/* The sub-id of the lock on a database that says who may set or clear its mark. */
#define CAPTURE_MARK_LOCK 1

bool		capture_in_background = true;
bool		capture_to_trees = false;

static void
slot_name_of(Oid dboid, char *name)
{
	snprintf(name, NAMEDATALEN, CAPTURE_SLOT_PREFIX "%u", dboid);
}

static bool
capture_possible(void)
{
	return wal_level >= WAL_LEVEL_REPLICA && max_replication_slots > 0;
}

/* -------------------------------------------------------------- the plugin */

/* Only the worker tells the sink, so a session can't be a second writer behind it. */
bool		am_capture_worker = false;

static void
capture_startup(LogicalDecodingContext *ctx, OutputPluginOptions *opt, bool is_init)
{
	if (!am_capture_worker)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("output plugin \"%s\" is for capture's background worker alone", PROLLY_CAPTURE_PLUGIN)));
	opt->output_type = OUTPUT_PLUGIN_BINARY_OUTPUT;
}

/* Whether the transaction being told was left out of capture, from its first message on. */
static bool capture_left_out = false;

static void
capture_begin(LogicalDecodingContext *ctx, ReorderBufferTXN *txn)
{
	capture_left_out = false;
	CaptureSinkInUse->begin(txn->xid, txn->final_lsn);
}

/*
 * The branch a relation is on, 0 for a shared schema's.  This is asked in the middle of decoding, so
 * it is the catalog as it was at the change, whatever has happened to the schema since.
 */
static int32
relation_branch(Relation relation)
{
	HeapTuple	tup = SearchSysCache1(NAMESPACEOID, ObjectIdGetDatum(RelationGetNamespace(relation)));
	int32		branch;

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for namespace %u", RelationGetNamespace(relation));
	branch = ((Form_pg_namespace) GETSTRUCT(tup))->nspbranch;
	ReleaseSysCache(tup);
	return branch;
}

void
CaptureRelationShape(Relation relation, WsRowShape *shape)
{
	TupleDesc	desc = RelationGetDescr(relation);
	uint64	   *tags = palloc_array(uint64, Max(desc->natts, 1));
	Oid			pk = RelationGetPrimaryKeyIndex(relation, true);
	Relation	index = OidIsValid(pk) ? RelationIdGetRelation(pk) : NULL;
	int			nkey = index != NULL ? index->rd_index->indnkeyatts : 0;
	int			n = 0;

	if (OidIsValid(pk) && !RelationIsValid(index))
		elog(ERROR, "could not open relation with OID %u", pk);
	for (int k = 0; k < nkey; k++)
		tags[n++] = (uint64) TupleDescAttr(desc, index->rd_index->indkey.values[k] - 1)->attcoltag;
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);
		bool		in_key = false;

		for (int k = 0; k < nkey; k++)
			in_key |= index->rd_index->indkey.values[k] == att->attnum;
		if (!att->attisdropped && att->attgenerated != ATTRIBUTE_GENERATED_VIRTUAL && !in_key)
			tags[n++] = (uint64) att->attcoltag;
	}
	for (int i = 0; i < n; i++)
	{
		if (tags[i] == 0)
			elog(ERROR, "a column of \"%s\" has no tag, in a versioned schema", RelationGetRelationName(relation));
	}
	if (index != NULL)
		RelationClose(index);
	ws_row_shape_init(shape, tags, n, nkey);
	pfree(tags);
}

static void
capture_change(LogicalDecodingContext *ctx, ReorderBufferTXN *txn, Relation relation,
			   ReorderBufferChange *change)
{
	CaptureChange out = {.branch = relation_branch(relation), .relation = relation, .lsn = change->lsn};

	/* (a materialized view's rows reach here only from a concurrent refresh) */
	if (capture_left_out || out.branch == 0 || relation->rd_rel->relkind != RELKIND_RELATION)
		return;
	switch (change->action)
	{
		case REORDER_BUFFER_CHANGE_INSERT:
			out.kind = CAPTURE_INSERT;
			break;
		case REORDER_BUFFER_CHANGE_UPDATE:
			out.kind = CAPTURE_UPDATE;
			break;
		case REORDER_BUFFER_CHANGE_DELETE:
			out.kind = CAPTURE_DELETE;
			break;
		default:
			elog(ERROR, "unexpected change of kind %d decoded", (int) change->action);
	}
	out.catalogs_changed = rbtxn_has_catalog_changes(txn);
	out.old_tuple = change->data.tp.oldtuple;
	out.new_tuple = change->data.tp.newtuple;
	CaptureSinkInUse->change(&out);
}

static void
capture_truncate(LogicalDecodingContext *ctx, ReorderBufferTXN *txn, int nrelations, Relation relations[],
				 ReorderBufferChange *change)
{
	if (capture_left_out)
		return;
	for (int i = 0; i < nrelations; i++)
	{
		int32		branch = relation_branch(relations[i]);

		if (branch != 0)
			CaptureSinkInUse->truncate(branch, relations[i], change->data.truncate.restart_seqs, change->lsn);
	}
}

static void
capture_message(LogicalDecodingContext *ctx, ReorderBufferTXN *txn, XLogRecPtr lsn, bool transactional,
				const char *prefix, Size size, const char *message)
{
	if (transactional && strcmp(prefix, CAPTURE_LEAVE_OUT_PREFIX) == 0)
	{
		capture_left_out = true;
		return;
	}
	if (transactional && capture_left_out && strcmp(prefix, CAPTURE_ROWS_ARE_PREFIX) != 0)
		return;
	CaptureSinkInUse->message(prefix, message, size, transactional, lsn);
}

static void
capture_commit(LogicalDecodingContext *ctx, ReorderBufferTXN *txn, XLogRecPtr commit_lsn)
{
	CaptureSinkInUse->commit(txn->xid, commit_lsn, txn->end_lsn);
}

/*
 * The writes of this transaction are left out of capture: a message saying so goes first, before
 * the transaction has written anything, and decoding tells the sink nothing more of it than that it
 * began and committed.  So it's the whole of it, as a prepared transaction's when it's committed;
 * and in a subtransaction, it's left out with what that rolls back.  For version control's own
 * writes, which make the tables what it has made the branch's trees, such as a hard reset's: what
 * the transaction does to any versioned table, its rows and its shape, is the caller's to have made
 * the trees of, which CaptureRowsAre tells capture.  As it has written, a commit from capture
 * refuses it, but for the one CaptureBarrierLeavingOut takes as it begins.  (Written is having an
 * xid, which a subtransaction's write rolled back still leaves the transaction: that refuses too.)
 */
static TransactionId left_out = InvalidTransactionId;

bool
CaptureLeftOut(void)
{
	return TransactionIdIsValid(left_out) && TransactionIdIsCurrentTransactionId(left_out);
}

/*
 * Once version control's own work is done, the transaction left out is sealed: what it would write
 * after to a versioned table, or change of one's objects, would be in no branch's history, as capture
 * leaves out all it writes, so it's refused.  (A transaction does one such work: a commit from
 * capture refuses one that has written, and the second would take one.)  And as it commits, a table
 * whose rows' layout changed is to have been told, and as it is.
 */
static TransactionId sealed = InvalidTransactionId;

/* Each table told as a map in this transaction, by its object id, and its fingerprint as it was told. */
typedef struct Told
{
	Oid			relid;
	char	   *fingerprint;
} Told;

static HTAB *told = NULL;
static bool told_callback = false;

static bool
sealed_now(void)
{
	return TransactionIdIsValid(sealed) && TransactionIdIsCurrentTransactionId(sealed);
}

void
CaptureLeftOutSeal(void)
{
	if (CaptureLeftOut())
		sealed = left_out;
}

void
CaptureCheckWrite(Relation relation, const char *what)
{
	char		kind = relation->rd_rel->relkind;

	if (sealed_now() && (kind == RELKIND_RELATION || kind == RELKIND_PARTITIONED_TABLE) &&
		NamespaceBranch(RelationGetNamespace(relation)) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("cannot %s versioned table \"%s\" in a transaction left out of capture",
						what, RelationGetRelationName(relation)),
				 errdetail("Its version control work is done, and what it wrote after would be in no branch's history."),
				 errhint("Do it in a transaction of its own.")));
}

void
CaptureCheckCatalogChange(void)
{
	if (sealed_now())
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("cannot change a versioned schema's objects in a transaction left out of capture"),
				 errdetail("Its version control work is done, and what it changed after would be in no branch's history."),
				 errhint("Do it in a transaction of its own.")));
}

static void
told_callback_fn(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_ABORT || event == XACT_EVENT_PREPARE ||
		event == XACT_EVENT_PARALLEL_COMMIT || event == XACT_EVENT_PARALLEL_ABORT)
		told = NULL;			/* (it's in the transaction's memory) */
}

static void
told_note(Relation relation)
{
	MemoryContext was;
	Told	   *t;
	char	   *fingerprint = CaptureRowsFingerprint(RelationGetRelid(relation),
												 relation->rd_rel->relfilenode);

	if (!told_callback)
	{
		RegisterXactCallback(told_callback_fn, NULL);
		told_callback = true;
	}
	if (told == NULL)
	{
		HASHCTL		ctl = {.keysize = sizeof(Oid), .entrysize = sizeof(Told), .hcxt = TopTransactionContext};

		told = hash_create("tables told", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	t = hash_search(told, &(Oid) {RelationGetRelid(relation)}, HASH_ENTER, NULL);
	was = MemoryContextSwitchTo(TopTransactionContext);
	t->fingerprint = pstrdup(fingerprint);
	MemoryContextSwitchTo(was);
	pfree(fingerprint);
}

void
CaptureCheckChanged(Oid relid, const char *fingerprint)
{
	Told	   *t;

	if (!CaptureLeftOut())
		return;
	t = told == NULL ? NULL : hash_search(told, &relid, HASH_FIND, NULL);
	if (t == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("table \"%s\" changed in a transaction left out of capture, and its rows weren't told",
						get_rel_name(relid)),
				 errdetail("A transaction left out of capture is to tell it the rows of each table whose layout it changes, and none was told for this one.")));
	if (strcmp(t->fingerprint, fingerprint) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("table \"%s\" changed in a transaction left out of capture after its rows were told",
						get_rel_name(relid)),
				 errdetail("A transaction left out of capture tells it each table's rows as they commit."),
				 errhint("Change it in a transaction of its own.")));
}

void
CaptureLeaveOut(void)
{
	if (!IsTransactionState() || IsInParallelMode())
		elog(ERROR, "capture can only leave out a transaction, and not in parallel");
	if (TransactionIdIsValid(left_out) && TransactionIdIsCurrentTransactionId(left_out))
		return;
	if (TransactionIdIsValid(GetTopTransactionIdIfAny()))
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("cannot leave this transaction out of capture"),
				 errdetail("It has written already.")));
	(void) LogLogicalMessage(CAPTURE_LEAVE_OUT_PREFIX, "", 0, true, false);
	left_out = GetCurrentTransactionId();
}

/*
 * Told whole as a row map, rather than as rows: in a transaction left out, as only what's made the
 * table's rows so knows they're the map's, and what's told of them otherwise, its changes and its
 * rows told at commit, isn't told.
 */
void
CaptureRowsAre(Relation relation, const PHash *map)
{
	WsRowShape	shape;
	StringInfoData out;
	int32		head[4];

	if (!CaptureLeftOut())
		elog(ERROR, "a table's rows can only be told as a map in a transaction left out of capture");
	head[0] = NamespaceBranch(RelationGetNamespace(relation));
	if (head[0] == 0)
		elog(ERROR, "relation \"%s\" is not versioned", RelationGetRelationName(relation));
	CaptureRelationShape(relation, &shape);
	head[1] = (int32) RelationGetRelid(relation);
	head[2] = shape.nkey;
	head[3] = shape.nstored;
	initStringInfo(&out);
	appendBinaryStringInfo(&out, (const char *) head, sizeof(head));
	appendBinaryStringInfo(&out, (const char *) shape.tags, sizeof(uint64) * shape.nstored);
	appendBinaryStringInfo(&out, (const char *) map->b, PHASH_LEN);
	(void) LogLogicalMessage(CAPTURE_ROWS_ARE_PREFIX, out.data, out.len, true, false);
	pfree(out.data);
	ws_row_shape_free(&shape);
	told_note(relation);

	/* (the map is held for capture, which may get to it after whatever else held it lets it go) */
	{
		VcRefs	   *refs = VcRefsOpen("pointers", ChunkStoreOpen("pages"));
		char		name[CAPTURE_TOLD_REF_LEN];

		CaptureToldRefName(RelationGetRelid(relation), GetTopTransactionId(), map, name);
		refs->routine->set(refs, name, map);
	}
}

void
CaptureToldRefName(Oid relid, TransactionId xid, const PHash *map, char *name)
{
	int			at = snprintf(name, CAPTURE_TOLD_REF_LEN, CAPTURE_TOLD_REF_PREFIX "%u/%u/", relid, xid);

	for (int i = 0; i < PHASH_LEN; i++)
		at += snprintf(name + at, CAPTURE_TOLD_REF_LEN - at, "%02x", map->b[i]);
}

void
CaptureOutputPluginInit(OutputPluginCallbacks *callbacks)
{
	callbacks->startup_cb = capture_startup;
	callbacks->begin_cb = capture_begin;
	callbacks->change_cb = capture_change;
	callbacks->truncate_cb = capture_truncate;
	callbacks->message_cb = capture_message;
	callbacks->commit_cb = capture_commit;
}

/* ------------------------------------------------------------------ the sink */

static bool
logging_slot_will_do(void)
{
	return true;
}

static void
logging_slot_made(XLogRecPtr from)
{
}

static void
logging_start(XLogRecPtr from)
{
	elog(DEBUG1, "capture starts at %X/%X", LSN_FORMAT_ARGS(from));
}

static void
logging_begin(TransactionId xid, XLogRecPtr final_lsn)
{
	elog(DEBUG1, "capture: begin");
}

static void
logging_change(const CaptureChange *change)
{
	if (message_level_is_interesting(DEBUG1))
	{
		WsRowShape	shape;
		StringInfoData tags;

		CaptureRelationShape(change->relation, &shape);
		initStringInfo(&tags);
		for (int i = 0; i < shape.nstored; i++)
			appendStringInfo(&tags, "%s" UINT64_FORMAT, i > 0 ? "," : "", shape.tags[i]);
		elog(DEBUG1, "capture layout: %s nkey=%d tags=%s", RelationGetRelationName(change->relation),
			 shape.nkey, tags.data);
		pfree(tags.data);
		ws_row_shape_free(&shape);
	}
	elog(DEBUG1, "capture: branch %d: %s %s old=%s new=%s", change->branch,
		 change->kind == CAPTURE_INSERT ? "insert" : change->kind == CAPTURE_UPDATE ? "update" : "delete",
		 RelationGetRelationName(change->relation),
		 change->old_tuple != NULL ? "yes" : "no", change->new_tuple != NULL ? "yes" : "no");
}

static void
logging_truncate(int32 branch, Relation relation, bool restart_seqs, XLogRecPtr lsn)
{
	elog(DEBUG1, "capture: branch %d: truncate %s restart=%s", branch, RelationGetRelationName(relation),
		 restart_seqs ? "yes" : "no");
}

static void
logging_message(const char *prefix, const char *data, Size size, bool transactional, XLogRecPtr lsn)
{
	elog(DEBUG1, "capture: message %s size=%zu transactional=%s", prefix, size, transactional ? "yes" : "no");
}

static void
logging_commit(TransactionId xid, XLogRecPtr commit_lsn, XLogRecPtr end_lsn)
{
	elog(DEBUG1, "capture: commit");
}

static void
logging_apply(void)
{
}

static bool
logging_after_record(void)
{
	return false;
}

static void
logging_slot_to_be_made(void)
{
}

const CaptureSink LoggingCaptureSink = {
	.name = "logging",
	.start = logging_start,
	.begin = logging_begin,
	.change = logging_change,
	.truncate = logging_truncate,
	.message = logging_message,
	.commit = logging_commit,
	.apply = logging_apply,
	.after_record = logging_after_record,
	.slot_to_be_made = logging_slot_to_be_made,
	.slot_made = logging_slot_made,
	.slot_will_do = logging_slot_will_do,
};

const CaptureSink *CaptureSinkInUse = &LoggingCaptureSink;

/* ---------------------------------------------------------------- starting */

static bool
capture_launch(Oid dboid, BackgroundWorkerHandle **handle)
{
	BackgroundWorker bgw;

	memset(&bgw, 0, sizeof(bgw));
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION | BGWORKER_INTERRUPTIBLE;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	bgw.bgw_restart_time = BGW_NEVER_RESTART;
	snprintf(bgw.bgw_library_name, MAXPGPATH, "postgres");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "CaptureWorkerMain");
	snprintf(bgw.bgw_name, BGW_MAXLEN, "capture for database %u", dboid);
	snprintf(bgw.bgw_type, BGW_MAXLEN, "capture worker");
	bgw.bgw_main_arg = ObjectIdGetDatum(dboid);
	return RegisterDynamicBackgroundWorker(&bgw, handle);
}

/* Whether this database's slot has its worker. */
static bool
slot_is_held(Oid dboid)
{
	char		name[NAMEDATALEN];
	ReplicationSlot *slot;
	bool		held;

	slot_name_of(dboid, name);
	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	slot = SearchNamedReplicationSlot(name, false);
	held = slot != NULL && slot->active_proc != INVALID_PROC_NUMBER;
	LWLockRelease(ReplicationSlotControlLock);
	return held;
}

static bool schema_made = false;

static void
capture_at_xact_end(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_COMMIT && schema_made && capture_in_background && capture_possible() &&
		!IsBinaryUpgrade && !slot_is_held(MyDatabaseId))
		(void) capture_launch(MyDatabaseId, NULL);
	if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_ABORT ||
		event == XACT_EVENT_PARALLEL_COMMIT || event == XACT_EVENT_PARALLEL_ABORT ||
		event == XACT_EVENT_PREPARE)
		schema_made = false;
}

/* Sets or clears this database's mark, with the lock that says who may. */
static void
database_mark(bool versioned)
{
	Relation	rel = table_open(DatabaseRelationId, RowExclusiveLock);
	HeapTuple	tup = SearchSysCacheLockedCopy1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
	ItemPointerData otid;

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for database %u", MyDatabaseId);
	otid = tup->t_self;
	if (((Form_pg_database) GETSTRUCT(tup))->datversioned != versioned)
	{
		((Form_pg_database) GETSTRUCT(tup))->datversioned = versioned;
		CatalogTupleUpdate(rel, &otid, tup);
		CommandCounterIncrement();
	}
	UnlockTuple(rel, &otid, InplaceUpdateTupleLock);
	heap_freetuple(tup);
	table_close(rel, RowExclusiveLock);
}

static bool
database_is_marked(void)
{
	HeapTuple	tup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
	bool		marked;

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for database %u", MyDatabaseId);
	marked = ((Form_pg_database) GETSTRUCT(tup))->datversioned;
	ReleaseSysCache(tup);
	return marked;
}

/*
 * To be called before the schema is: what it waits for mustn't be waiting for that schema's name.
 * Held until this commits, the lock keeps a worker that finds no schema from clearing the mark.  A
 * database already marked needs no more than a shared hold of it, so makers don't wait on each
 * other; only one that has to set the mark waits, and is waited for.
 */
void
CaptureSchemaMade(void)
{
	static bool registered = false;

	if (!registered)
	{
		RegisterXactCallback(capture_at_xact_end, NULL);
		registered = true;
	}

	for (;;)
	{
		bool		marked;
		LOCKMODE	mode;

		AcceptInvalidationMessages();
		marked = database_is_marked();
		mode = marked ? ShareLock : ExclusiveLock;
		LockSharedObject(DatabaseRelationId, MyDatabaseId, CAPTURE_MARK_LOCK, mode);
		if (database_is_marked() == marked)
		{
			if (!marked)
				database_mark(true);
			break;
		}

		/*
		 * Set or cleared while it waited, so this mode is the wrong one.  It wasn't held earlier in
		 * this transaction: a clear can't commit under a hold, nor a set under ExclusiveLock.
		 */
		UnlockSharedObject(DatabaseRelationId, MyDatabaseId, CAPTURE_MARK_LOCK, mode);
	}
	schema_made = true;
}

/* ---------------------------------------------------------------- the worker */

/* Whether this database has a versioned schema, on any branch, in the transaction running. */
static bool
versioned_schema_exists(void)
{
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	bool		found = false;

	PushActiveSnapshot(GetTransactionSnapshot());
	rel = table_open(NamespaceRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);
	while (!found && (tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
		found = ((Form_pg_namespace) GETSTRUCT(tup))->nspbranch != 0;
	table_endscan(scan);
	table_close(rel, AccessShareLock);
	PopActiveSnapshot();
	return found;
}

static bool
database_has_versioned_schema(void)
{
	bool		found;

	StartTransactionCommand();
	found = versioned_schema_exists();
	CommitTransactionCommand();
	return found;
}

static void
capture_slot_create(const char *name)
{
	LogicalDecodingContext *ctx;

	/*
	 * (with the name held, so another worker starting too doesn't take this for its own new slot,
	 * and before it's persisted, as a worker can die just after; for failover, so that a standby
	 * syncing slots has it, and capture goes on from it once that's promoted: the maps come over
	 * by the WAL, and what it tells again that the maps already have is passed over)
	 */
	ReplicationSlotCreate(name, true, RS_EPHEMERAL, false, false, true, false);
	CaptureSinkInUse->slot_to_be_made();

	/* (repeatable read, so that the snapshot this gets is what all it reads is under) */
	StartTransactionCommand();
	if (CaptureSinkInUse->reads_at_start)
		XactIsoLevel = XACT_REPEATABLE_READ;
	EnsureLogicalDecodingEnabled();
	ctx = CreateInitDecodingContext(PROLLY_CAPTURE_PLUGIN, NIL, CaptureSinkInUse->reads_at_start, false, InvalidXLogRecPtr,
									XL_ROUTINE(.page_read = read_local_xlog_page,
											   .segment_open = wal_segment_open,
											   .segment_close = wal_segment_close),
									NULL, NULL, NULL);
	DecodingContextFindStartpoint(ctx);

	/*
	 * All that committed before the slot's first position is in this snapshot, and all after it will
	 * be told: the two are the whole of a table.  The slot's kept only once what the sink makes of
	 * it has committed, so a worker dying before then leaves no slot, and the next begins again.
	 */
	if (CaptureSinkInUse->reads_at_start)
		RestoreTransactionSnapshot(SnapBuildInitialSnapshot(ctx->snapshot_builder), MyProc);
	FreeDecodingContext(ctx);
	INJECTION_POINT("capture-slot-snapshot", NULL);
	CaptureSinkInUse->slot_made(MyReplicationSlot->data.confirmed_flush);
	CommitTransactionCommand();
	ReplicationSlotPersist();
	ReplicationSlotRelease();
}

static bool capture_at_end_of_wal = false;

/*
 * Reads a page if it is there.  The reader's own private data is the decoding context, which this
 * isn't to touch, and returning to the worker's loop is how it notices the postmaster going.
 */
static int
capture_page_read(XLogReaderState *state, XLogRecPtr target_page, int req_len, XLogRecPtr target_rec, char *buf)
{
	void	   *ctx = state->private_data;
	ReadLocalXLogPageNoWaitPrivate nowait = {0};
	int			n;

	state->private_data = &nowait;
	n = read_local_xlog_page_no_wait(state, target_page, req_len, target_rec, buf);
	state->private_data = ctx;
	if (nowait.end_of_wal)
		capture_at_end_of_wal = true;
	return n;
}

/* Whether capture has warned its slot retains past half of what it may, since it was last below. */
static bool capture_behind_warned = false;

/* How much WAL is read between checks of what the slot retains. */
#define CAPTURE_BEHIND_EVERY (1024 * 1024)

/*
 * Warns, once each time it passes it, that the WAL capture's slot retains is past half of
 * max_slot_wal_keep_size: past all of it, the slot is invalidated at a checkpoint and every table
 * read again, so that's while something can still be done.  A slot retains WAL from where it may
 * have to begin decoding again, which a transaction still running holds back however far capture
 * has read, as well as what capture has yet to read: which it is shows by where each is, which the
 * warning gives.  How far behind each branch is shows as ever, by pg_replication_slots and
 * pg_history.capture_positions.
 */
static void
capture_check_behind(XLogRecPtr read)
{
	XLogRecPtr	restart;
	XLogRecPtr	now = GetXLogInsertRecPtr();
	uint64		limit;

	if (max_slot_wal_keep_size_mb < 0)
		return;
	SpinLockAcquire(&MyReplicationSlot->mutex);
	restart = MyReplicationSlot->data.restart_lsn;
	SpinLockRelease(&MyReplicationSlot->mutex);
	if (XLogRecPtrIsInvalid(restart))
		return;
	limit = (uint64) max_slot_wal_keep_size_mb * 1024 * 1024;
	if (now - restart <= limit / 2)
	{
		capture_behind_warned = false;
		return;
	}
	if (capture_behind_warned)
		return;
	capture_behind_warned = true;
	ereport(WARNING,
			(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
			 errmsg("capture's replication slot \"%s\" retains %llu MB of WAL, more than half of \"max_slot_wal_keep_size\" (%d MB)",
					NameStr(MyReplicationSlot->data.name), (unsigned long long) ((now - restart) / (1024 * 1024)),
					max_slot_wal_keep_size_mb),
			 errdetail("Past all of it, the slot is invalidated at a checkpoint, and every versioned table is read again.  "
					   "The WAL is written up to %X/%X, capture has read up to %X/%X, and the slot keeps it from %X/%X, "
					   "where decoding would begin again.",
					   LSN_FORMAT_ARGS(now), LSN_FORMAT_ARGS(read), LSN_FORMAT_ARGS(restart)),
			 errhint("Let capture catch up, end any transaction that has long been running, or raise \"max_slot_wal_keep_size\".")));
}

/*
 * Decodes what is flushed now, and says it was.  With a record that goes past what's flushed, it
 * stops at the one before, to read it whole later.
 */
static void
capture_decode(LogicalDecodingContext *ctx)
{
	XLogRecPtr	flushed = GetFlushRecPtr(NULL);
	XLogRecPtr	from = ctx->reader->EndRecPtr;
	XLogRecPtr	check_at = from + CAPTURE_BEHIND_EVERY;

	capture_check_behind(from);
	capture_at_end_of_wal = false;
	while (ctx->reader->EndRecPtr < flushed && !capture_at_end_of_wal)
	{
		char	   *errm = NULL;
		XLogRecord *record = XLogReadRecord(ctx->reader, &errm);

		/* (a pass reads what was flushed as it began, which may be a long way) */
		if (ctx->reader->EndRecPtr >= check_at)
		{
			capture_check_behind(ctx->reader->EndRecPtr);
			check_at = ctx->reader->EndRecPtr + CAPTURE_BEHIND_EVERY;
		}

		if (errm != NULL)
			elog(ERROR, "could not read a record for capture: %s", errm);
		if (record != NULL)
		{
			LogicalDecodingProcessRecord(ctx, ctx->reader);
			if (CaptureSinkInUse->after_record())
				LogicalConfirmReceivedLocation(ctx->reader->EndRecPtr);
		}
		CHECK_FOR_INTERRUPTS();
	}
	if (ctx->reader->EndRecPtr > from)
	{
		CaptureSinkInUse->apply();
		LogicalConfirmReceivedLocation(ctx->reader->EndRecPtr);
	}
	else
	{
		/* (with nothing read, what a sink has to do later, such as heal a table, is done now) */
		CaptureSinkInUse->apply();
	}
}

/*
 * A decoding context has this backend announced as only decoding, whose xmin the slot sees to, so
 * that snapshots and horizons pass it by.  Passing by its xid too would have other backends take a
 * transaction it's writing in, which has none but this worker's, for one that's over: for one that
 * aborted, as it hasn't committed, which a hint would then make so for good.  This one writes.
 */
static void
capture_not_decoding_only(void)
{
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
	MyProc->statusFlags &= ~PROC_IN_LOGICAL_DECODING;
	ProcGlobal->statusFlags[MyProc->pgxactoff] = MyProc->statusFlags;
	LWLockRelease(ProcArrayLock);
}

/*
 * Clears the mark of a database with no versioned schema; false if one is being made, or there
 * now is one.  It looks again with the lock held, which whoever makes one holds until they commit.
 */
static bool
capture_clear_mark(void)
{
	bool		cleared = false;

	INJECTION_POINT("capture-clear-mark", NULL);
	StartTransactionCommand();
	if (ConditionalLockSharedObject(DatabaseRelationId, MyDatabaseId, CAPTURE_MARK_LOCK, ExclusiveLock) &&
		!versioned_schema_exists())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		database_mark(false);
		PopActiveSnapshot();
		cleared = true;
	}
	CommitTransactionCommand();
	return cleared;
}

/*
 * Makes this database's slot if it has none.  False, with why, if this worker has nothing to do:
 * it has a worker, or its name is some other slot's.  An invalidated slot has lost WAL it needs,
 * so it is dropped and made again.
 */
static bool
capture_slot_ready(const char *name)
{
	ReplicationSlot *slot;
	bool		exists;
	bool		held = false;
	bool		ours = true;
	bool		lost = false;

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	slot = SearchNamedReplicationSlot(name, false);
	exists = slot != NULL;
	if (exists)
	{
		held = slot->active_proc != INVALID_PROC_NUMBER;
		ours = slot->data.database == MyDatabaseId &&
			strcmp(NameStr(slot->data.plugin), PROLLY_CAPTURE_PLUGIN) == 0;
		lost = slot->data.invalidated != RS_INVAL_NONE;
	}
	LWLockRelease(ReplicationSlotControlLock);
	if (held)
		return false;
	if (!ours)
	{
		ereport(WARNING,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("replication slot \"%s\" is not capture's, so this database is not being captured", name)));
		return false;
	}
	if (exists && !lost && !CaptureSinkInUse->slot_will_do())
	{
		/* (it's been read by another sink, of which this one kept nothing) */
		ReplicationSlotDrop(name, true);
		exists = false;
	}
	if (lost)
	{
		ereport(WARNING,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("capture's replication slot \"%s\" was invalidated, and is being made again", name)));
		INJECTION_POINT("capture-slot-lost", NULL);
		ReplicationSlotDrop(name, true);
		exists = false;
	}
	if (!exists)
	{
		capture_slot_create(name);
		INJECTION_POINT("capture-slot-made", NULL);
	}
	return true;
}

void
CaptureWorkerMain(Datum main_arg)
{
	Oid			dboid = DatumGetObjectId(main_arg);
	char		name[NAMEDATALEN];
	LogicalDecodingContext *ctx;

	am_capture_worker = true;
	if (capture_to_trees)
		CaptureSinkInUse = &TreeCaptureSink;
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();
	BackgroundWorkerInitializeConnectionByOid(dboid, InvalidOid, BGWORKER_BYPASS_ALLOWCONN | BGWORKER_SKIP_SETTINGS);

	/*
	 * (A database's and its owner's settings aren't this superuser's, whose work is the whole
	 * cluster's WAL: they're left out above.)  On no branch, whatever the server's settings say: a slot keeps the branch it was made on,
	 * and one made on a branch decodes only that branch's changes.  With the filter off, it sees
	 * every branch's schemas.
	 */
	SetConfigOption("catalog_branch", "0", PGC_SUSET, PGC_S_OVERRIDE);

	/* (this is a superuser, whose SQL no setting of the server's is to have be someone else's) */
	SetConfigOption("search_path", "pg_catalog, pg_temp", PGC_SUSET, PGC_S_OVERRIDE);
	SetConfigOption("catalog_branch_filter", "off", PGC_SUSET, PGC_S_OVERRIDE);
	slot_name_of(dboid, name);

	if (!capture_possible())
		proc_exit(0);

	/* (a mark with no schema is cleared whatever is wrong with the slot; the launcher takes the slot) */
	if (!database_has_versioned_schema() && capture_clear_mark())
		proc_exit(0);
	if (!capture_slot_ready(name))
		proc_exit(0);
	ReplicationSlotAcquire(name, true, true);
	CurrentResourceOwner = ResourceOwnerCreate(NULL, "capture");
	ctx = CreateDecodingContext(InvalidXLogRecPtr, NIL, false,
								XL_ROUTINE(.page_read = capture_page_read,
										   .segment_open = wal_segment_open,
										   .segment_close = wal_segment_close),
								NULL, NULL, NULL);
	capture_not_decoding_only();
	XLogBeginRead(ctx->reader, MyReplicationSlot->data.restart_lsn);
	CaptureSinkInUse->start(MyReplicationSlot->data.confirmed_flush);

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/* the launcher drops a slot with no worker, if capture is off */
		if (!capture_in_background)
			break;
		if (!database_has_versioned_schema() && capture_clear_mark())
		{
			FreeDecodingContext(ctx);
			ReplicationSlotRelease();
			ReplicationSlotDrop(name, true);
			break;
		}
		capture_decode(ctx);
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 CAPTURE_WORKER_NAPTIME_MS, WAIT_EVENT_CAPTURE_WORKER_MAIN);
		ResetLatch(MyLatch);
	}
	proc_exit(0);
}

void
CaptureDatabaseDropping(Oid dboid)
{
	char		name[NAMEDATALEN];
	TimestampTz start = GetCurrentTimestamp();

	slot_name_of(dboid, name);

	/* (another can't start: it waits on the lock held on the database) */
	TerminateBackgroundWorkersForDatabase(dboid);
	while (slot_is_held(dboid))
	{
		CHECK_FOR_INTERRUPTS();

		/* (the count of a database's slots says, if this doesn't get there) */
		if (TimestampDifferenceExceeds(start, GetCurrentTimestamp(), 5000))
			break;
		pg_usleep(10000);
	}
}

/* -------------------------------------------------------------- the launcher */

typedef struct CaptureRunning
{
	Oid			dboid;
	BackgroundWorkerHandle *handle;
} CaptureRunning;

/* The databases marked as having versioned schemas. */
static List *
databases_marked(void)
{
	List	   *dbs = NIL;
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	MemoryContext cxt = CurrentMemoryContext;

	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	rel = table_open(DatabaseRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);
	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_database db = (Form_pg_database) GETSTRUCT(tup);

		if (db->datversioned)
		{
			MemoryContext old = MemoryContextSwitchTo(cxt);

			dbs = lappend_oid(dbs, db->oid);
			MemoryContextSwitchTo(old);
		}
	}
	table_endscan(scan);
	table_close(rel, AccessShareLock);
	PopActiveSnapshot();
	CommitTransactionCommand();
	return dbs;
}

/* Drops capture's slots with no worker, of no database in the list; with none, all of them. */
static void
drop_slots_not_of(List *dbs)
{
	List	   *names = NIL;
	ListCell   *lc;

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (int i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];

		if (s->in_use && s->active_proc == INVALID_PROC_NUMBER &&
			strcmp(NameStr(s->data.plugin), PROLLY_CAPTURE_PLUGIN) == 0 &&
			strncmp(NameStr(s->data.name), CAPTURE_SLOT_PREFIX, strlen(CAPTURE_SLOT_PREFIX)) == 0 &&
			!list_member_oid(dbs, s->data.database))
			names = lappend(names, pstrdup(NameStr(s->data.name)));
	}
	LWLockRelease(ReplicationSlotControlLock);
	foreach(lc, names)
		ReplicationSlotDrop(lfirst(lc), true);
}

void
CaptureLauncherMain(Datum main_arg)
{
	/* (each a palloc in this, to last from one look to the next) */
	MemoryContext top = AllocSetContextCreate(TopMemoryContext, "capture launcher", ALLOCSET_DEFAULT_SIZES);
	MemoryContext look = AllocSetContextCreate(top, "capture launcher look", ALLOCSET_DEFAULT_SIZES);
	List	   *running = NIL;
	bool		none_free;
	bool		warned = false;
	uint32		look_count = 0;

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	BackgroundWorkerUnblockSignals();
	BackgroundWorkerInitializeConnection(NULL, NULL, 0);

	if (!capture_possible())
		proc_exit(0);

	for (;;)
	{
		List	   *marked;
		ListCell   *lc;
		MemoryContext old;

		CHECK_FOR_INTERRUPTS();
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}
		MemoryContextReset(look);
		old = MemoryContextSwitchTo(look);
		marked = capture_in_background ? databases_marked() : NIL;
		drop_slots_not_of(marked);
		BranchIdleLook(marked);
		foreach(lc, running)
		{
			CaptureRunning *one = lfirst(lc);
			pid_t		pid;

			if (GetBackgroundWorkerPid(one->handle, &pid) == BGWH_STOPPED)
			{
				running = foreach_delete_current(running, lc);
				pfree(one->handle);
				pfree(one);
			}
		}
		none_free = false;

		/* (from a different one each look: one whose worker can't stay mustn't take the last free) */
		look_count++;
		for (int i = 0; i < list_length(marked); i++)
		{
			Oid			dboid = list_nth_oid(marked, (look_count + i) % list_length(marked));
			ListCell   *r;
			bool		have = slot_is_held(dboid);
			BackgroundWorkerHandle *handle;

			foreach(r, running)
				have = have || ((CaptureRunning *) lfirst(r))->dboid == dboid;
			if (have)
				continue;
			MemoryContextSwitchTo(top);
			if (capture_launch(dboid, &handle))
			{
				CaptureRunning *one = palloc_object(CaptureRunning);

				one->dboid = dboid;
				one->handle = handle;
				running = lappend(running, one);
			}
			else
				none_free = true;
			MemoryContextSwitchTo(look);
		}

		/* (once, for as long as it lasts; the next look tries again) */
		if (none_free && !warned)
			ereport(WARNING,
					(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
					 errmsg("no background worker is free for capture, so a database with versioned schemas is not being captured"),
					 errhint("Each such database takes one.  You might need to increase \"max_worker_processes\".")));
		warned = none_free;
		MemoryContextSwitchTo(old);
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 CAPTURE_LAUNCHER_NAPTIME_MS, WAIT_EVENT_CAPTURE_LAUNCHER_MAIN);
		ResetLatch(MyLatch);
	}
}

void
CaptureLauncherRegister(void)
{
	BackgroundWorker bgw;

	if (IsBinaryUpgrade)
		return;
	memset(&bgw, 0, sizeof(bgw));
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	bgw.bgw_restart_time = 5;
	snprintf(bgw.bgw_library_name, MAXPGPATH, "postgres");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "CaptureLauncherMain");
	snprintf(bgw.bgw_name, BGW_MAXLEN, "capture launcher");
	snprintf(bgw.bgw_type, BGW_MAXLEN, "capture launcher");
	RegisterBackgroundWorker(&bgw);
}
