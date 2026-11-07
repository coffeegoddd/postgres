/*-------------------------------------------------------------------------
 *
 * capture_rows.c
 *	  Writing a table's rows into the WAL, for what changes them and tells no
 *	  change of them.
 *
 * Rewriting a table, a column's type changing with no rewrite, dropping a
 * type and with it a column, an enum's value renamed or a composite's attribute
 * added all change what a table's rows are, and decoding sees none of it.  They
 * are too many ways to catch each, so nothing tries: a transaction that changed
 * any catalog compares, as it commits, each versioned table's fingerprint with
 * the one last recorded (what makes up its file, key, columns and the types of
 * those, whatever they're made of).  A table's differs or has none: its rows go
 * in the WAL, as logical messages (capture.h's CAPTURE_ROWS_*), the last thing
 * of it in the transaction, so that they void all of it before them, in the same
 * transaction or not, and its fingerprint is recorded.
 *
 * Every process with a transaction commits through here, once what's done at
 * commit has been: dropping a table ON COMMIT can drop a column of another.
 *
 * In a database with versioned schemas such transactions take turns from there
 * to the commit, by a lock on the database, each comparing only once it has it:
 * two at once would each record what it sees without the other's change.  Fingerprints
 * are of the catalogs alone, so comparing waits for no table.  Writing a table's rows
 * takes ShareLock on it, which holds off its writers, or one committing after the read
 * and before this commits would come first and be lost.  So that can deadlock with
 * them, as any taking of locks late can, and it's an error above READ COMMITTED, as it
 * reads what's committed now.
 *
 * This runs with the system's rights, as one may rename the value of a type of
 * one's own that a table one can't reach has a column of.  So what it runs is only its
 * own SQL on its own table, under a search path of its own, and a table's rows are read
 * directly: planning a query would run what a table's index expressions call.
 *
 * It's an error to prepare such a transaction, as this one's lock would then be held
 * until it's resolved.
 *
 * Text naming an object, as a regclass column prints a table's name or an aclitem a
 * role's, changes when it's renamed, and depends on who's asking: a temporary table's prints
 * with its schema to all but its own session.  No fingerprint here says so.  Nor does one say
 * a type's output function was replaced, which takes a superuser.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/capture_rows.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/indexing.h"
#include "catalog/pg_authid_d.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_enum.h"
#include "catalog/pg_index.h"
#include "catalog/pg_range.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "prolly/branch_catalog.h"
#include "prolly/capture.h"
#include "prolly/ws/ws_schema.h"
#include "prolly/ws/ws_sql.h"
#include "replication/message.h"
#include "storage/lmgr.h"
#include "storage/sinval.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/injection_point.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/* How many rows go in a message. */
#define CAPTURE_ROWS_BATCH_SIZE 1000

static void
put_int32(StringInfo out, int32 v)
{
	appendBinaryStringInfo(out, &v, sizeof(v));
}

/* What a batch of a table's rows is written in, until it's written. */
typedef struct RowsOut
{
	StringInfoData rows;
	int			nrows;
	int			nstored;
	int32		branch;
	Oid			relid;
} RowsOut;

static void
rows_flush(RowsOut *o)
{
	StringInfoData out;

	if (o->nrows == 0)
		return;
	initStringInfo(&out);
	appendStringInfoChar(&out, CAPTURE_ROWS_BATCH);
	put_int32(&out, o->branch);
	put_int32(&out, (int32) o->relid);
	put_int32(&out, o->nrows);
	appendBinaryStringInfo(&out, o->rows.data, o->rows.len);
	(void) LogLogicalMessage(CAPTURE_ROWS_PREFIX, out.data, out.len, true, false);
	pfree(out.data);
	resetStringInfo(&o->rows);
	o->nrows = 0;
}

void
CaptureTableRowsRead(Relation rel, const WsRowShape *shape, Snapshot snapshot, CaptureRowFn fn, void *arg)
{
	TupleDesc	desc = RelationGetDescr(rel);
	int		   *at = palloc_array(int, Max(shape->nstored, 1));
	FmgrInfo   *output = palloc_array(FmgrInfo, Max(shape->nstored, 1));
	const char **fields = palloc_array(const char *, Max(shape->nstored, 1));
	MemoryContext row_cxt = AllocSetContextCreate(CurrentMemoryContext, "capture rows", ALLOCSET_DEFAULT_SIZES);
	TupleTableSlot *slot;
	TableScanDesc scan;
	int			scope;

	for (int i = 0; i < shape->nstored; i++)
	{
		bool		found = false;

		for (int a = 0; !found && a < desc->natts; a++)
		{
			Form_pg_attribute att = TupleDescAttr(desc, a);

			if (!att->attisdropped && (uint64) att->attcoltag == shape->tags[i])
			{
				Oid			out_fn;
				bool		varlena;

				at[i] = a;
				getTypeOutputInfo(att->atttypid, &out_fn, &varlena);
				fmgr_info(out_fn, &output[i]);
				found = true;
			}
		}
		if (!found)
			elog(ERROR, "\"%s\" has no column of tag " UINT64_FORMAT, RelationGetRelationName(rel), shape->tags[i]);
	}
	scope = WsSql->begin();
	slot = table_slot_create(rel, NULL);
	scan = table_beginscan(rel, snapshot, 0, NULL, SO_NONE);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		MemoryContext old = MemoryContextSwitchTo(row_cxt);

		CHECK_FOR_INTERRUPTS();
		slot_getallattrs(slot);
		for (int i = 0; i < shape->nstored; i++)
			fields[i] = slot->tts_isnull[at[i]] ? NULL :
				ws_text_to_utf8(OutputFunctionCall(&output[i], slot->tts_values[at[i]]));
		MemoryContextSwitchTo(old);

		/* (in the caller's context, what it keeps of a row staying when the row's text goes) */
		fn(arg, fields);
		MemoryContextReset(row_cxt);
	}
	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	WsSql->end(scope);
	MemoryContextDelete(row_cxt);
	pfree(at);
	pfree(output);
	pfree(fields);
}

static void
rows_write_row(void *arg, const char **fields)
{
	RowsOut    *o = arg;

	for (int i = 0; i < o->nstored; i++)
	{
		if (fields[i] == NULL)
			put_int32(&o->rows, -1);
		else
		{
			int32		len = (int32) strlen(fields[i]);

			put_int32(&o->rows, len);
			appendBinaryStringInfo(&o->rows, fields[i], len);
		}
	}
	if (++o->nrows == CAPTURE_ROWS_BATCH_SIZE)
		rows_flush(o);
}

char
CaptureLoggedIdentity(Relation relation)
{
	char		identity = relation->rd_rel->relreplident;

	if (!capture_to_trees || identity == REPLICA_IDENTITY_FULL || !RelationIsLogicallyLogged(relation))
		return identity;
	if (identity == REPLICA_IDENTITY_DEFAULT && OidIsValid(RelationGetPrimaryKeyIndex(relation, false)))
		return identity;
	return NamespaceBranch(RelationGetNamespace(relation)) != 0 ? REPLICA_IDENTITY_FULL : identity;
}

/* A table's rows, the last thing it's told of them. */
static void
rows_write(Relation rel, int32 branch)
{
	Oid			relid = RelationGetRelid(rel);
	WsRowShape	shape;
	StringInfoData out;
	RowsOut		o;
	Snapshot	snapshot;

	CaptureRelationShape(rel, &shape);
	initStringInfo(&out);
	appendStringInfoChar(&out, CAPTURE_ROWS_BEGIN);
	put_int32(&out, branch);
	put_int32(&out, (int32) relid);
	put_int32(&out, shape.nkey);
	put_int32(&out, shape.nstored);
	appendBinaryStringInfo(&out, shape.tags, sizeof(uint64) * shape.nstored);
	(void) LogLogicalMessage(CAPTURE_ROWS_PREFIX, out.data, out.len, true, false);
	pfree(out.data);
	initStringInfo(&o.rows);
	o.nrows = 0;
	o.nstored = shape.nstored;
	o.branch = branch;
	o.relid = relid;
	snapshot = RegisterSnapshot(GetLatestSnapshot());
	CaptureTableRowsRead(rel, &shape, snapshot, rows_write_row, &o);
	UnregisterSnapshot(snapshot);
	rows_flush(&o);
	pfree(o.rows.data);
	ws_row_shape_free(&shape);
}

bool
CaptureRowsResend(Oid relid)
{
	Relation	rel;

	if (!ConditionalLockRelationOid(relid, ShareLock))
		return false;
	rel = try_relation_open(relid, NoLock);
	if (rel == NULL)
		return true;
	if (rel->rd_rel->relkind == RELKIND_RELATION && NamespaceBranch(RelationGetNamespace(rel)) != 0)
		rows_write(rel, NamespaceBranch(RelationGetNamespace(rel)));
	relation_close(rel, NoLock);
	return true;
}

static int
label_cmp(const ListCell *a, const ListCell *b)
{
	return strcmp((const char *) lfirst(a), (const char *) lfirst(b));
}

static void print_columns(StringInfo out, Oid relid, bool virtuals, List **seen);

/* What makes up what a type's values print as, appended; a type met again is only named. */
static void
print_type(StringInfo out, Oid type, int32 typmod, List **seen)
{
	HeapTuple	tup;
	Form_pg_type t;

	check_stack_depth();
	appendStringInfo(out, "(%u %d", type, typmod);
	if (list_member_oid(*seen, type))
	{
		appendStringInfoChar(out, ')');
		return;
	}
	*seen = lappend_oid(*seen, type);
	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", type);
	t = (Form_pg_type) GETSTRUCT(tup);
	if (OidIsValid(t->typelem) && IsTrueArrayType(t))
		print_type(out, t->typelem, -1, seen);
	else if (t->typtype == TYPTYPE_DOMAIN)
		print_type(out, t->typbasetype, t->typtypmod, seen);
	else if (t->typtype == TYPTYPE_RANGE)
		print_type(out, get_range_subtype(type), -1, seen);
	else if (t->typtype == TYPTYPE_MULTIRANGE)
		print_type(out, get_multirange_range(type), -1, seen);
	else if (t->typtype == TYPTYPE_ENUM)
	{
		CatCList   *labels = SearchSysCacheList1(ENUMTYPOIDNAME, ObjectIdGetDatum(type));
		List	   *names = NIL;
		ListCell   *lc;

		for (int i = 0; i < labels->n_members; i++)
		{
			Form_pg_enum e = (Form_pg_enum) GETSTRUCT(&labels->members[i]->tuple);

			names = lappend(names, psprintf("%u=%zu:%s", e->oid, strlen(NameStr(e->enumlabel)), NameStr(e->enumlabel)));
		}
		ReleaseSysCacheList(labels);
		list_sort(names, label_cmp);
		foreach(lc, names)
			appendStringInfo(out, " %s", (char *) lfirst(lc));
	}
	else if (t->typtype == TYPTYPE_COMPOSITE)
		print_columns(out, t->typrelid, true, seen);
	ReleaseSysCache(tup);
	appendStringInfoChar(out, ')');
}

/*
 * A relation's live columns' places, tags and types, of the catalogs alone.  A virtual generated
 * column isn't in a table's rows, but its row type's values have it.
 */
static void
print_columns(StringInfo out, Oid relid, bool virtuals, List **seen)
{
	HeapTuple	class = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	int			natts;

	if (!HeapTupleIsValid(class))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	natts = ((Form_pg_class) GETSTRUCT(class))->relnatts;
	ReleaseSysCache(class);
	for (AttrNumber n = 1; n <= natts; n++)
	{
		HeapTuple	tup = SearchSysCacheAttNum(relid, n);
		Form_pg_attribute att;

		/* (a column dropped has none) */
		if (!HeapTupleIsValid(tup))
			continue;
		att = (Form_pg_attribute) GETSTRUCT(tup);
		if (virtuals || att->attgenerated != ATTRIBUTE_GENERATED_VIRTUAL)
		{
			appendStringInfo(out, " %d:" INT64_FORMAT, att->attnum, att->attcoltag);
			print_type(out, att->atttypid, att->atttypmod, seen);
		}
		ReleaseSysCache(tup);
	}
}

/* A table's fingerprint, of the catalogs alone: it takes no lock on the table. */
char *
CaptureRowsFingerprint(Oid relid, Oid relfilenode)
{
	StringInfoData out;
	Relation	indexes;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *seen = NIL;

	initStringInfo(&out);
	appendStringInfo(&out, "file %u key", relfilenode);
	indexes = table_open(IndexRelationId, AccessShareLock);
	ScanKeyInit(&key, Anum_pg_index_indrelid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(relid));
	scan = systable_beginscan(indexes, IndexIndrelidIndexId, true, NULL, 1, &key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_index index = (Form_pg_index) GETSTRUCT(tup);

		if (!index->indisprimary)
			continue;
		for (int k = 0; k < index->indnkeyatts; k++)
			appendStringInfo(&out, " %d", index->indkey.values[k]);
	}
	systable_endscan(scan);
	table_close(indexes, AccessShareLock);
	appendStringInfoString(&out, " columns");
	print_columns(&out, relid, false, &seen);
	list_free(seen);
	return out.data;
}

/* Each versioned table there is, with its file, whichever branch this is on. */
typedef struct VersionedTable
{
	Oid			relid;
	Oid			relfilenode;
} VersionedTable;

static List *
versioned_tables(void)
{
	List	   *tables = NIL;
	Relation	rel = table_open(RelationRelationId, AccessShareLock);
	SysScanDesc scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
	HeapTuple	tup;

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_class c = (Form_pg_class) GETSTRUCT(tup);

		if (c->relkind == RELKIND_RELATION && c->relpersistence == RELPERSISTENCE_PERMANENT &&
			NamespaceBranch(c->relnamespace) != 0)
		{
			VersionedTable *t = palloc_object(VersionedTable);

			t->relid = c->oid;
			t->relfilenode = c->relfilenode;
			tables = lappend(tables, t);
		}
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	return tables;
}

/* Each table whose fingerprint isn't the one recorded has its rows written, and what's recorded made so. */
static void
rows_compare(void)
{
	List	   *tables = versioned_tables();
	ListCell   *lc;
	Oid			types[2] = {OIDOID, TEXTOID};

	foreach(lc, tables)
	{
		VersionedTable *t = lfirst(lc);
		char	   *fingerprint = CaptureRowsFingerprint(t->relid, t->relfilenode);
		Datum		values[2] = {ObjectIdGetDatum(t->relid), CStringGetTextDatum(fingerprint)};

		if (SPI_execute_with_args("SELECT 1 FROM pg_history.capture_fingerprints WHERE relid = $1 AND fingerprint = $2",
								  2, types, values, NULL, true, 1) != SPI_OK_SELECT)
			elog(ERROR, "could not read a table's fingerprint");
		if (SPI_processed == 0)
		{
			Relation	rel;

			if (IsolationUsesXactSnapshot())
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("capture to trees can't commit this above READ COMMITTED"),
						 errdetail("It changes a table's rows, which capture then reads as they're committed now.")));
			rel = table_open(t->relid, ShareLock);
			rows_write(rel, NamespaceBranch(RelationGetNamespace(rel)));
			table_close(rel, NoLock);
			if (SPI_execute_with_args("INSERT INTO pg_history.capture_fingerprints VALUES ($1, $2) "
									  "ON CONFLICT (relid) DO UPDATE SET fingerprint = excluded.fingerprint",
									  2, types, values, NULL, false, 0) != SPI_OK_INSERT)
				elog(ERROR, "could not record a table's fingerprint");
		}
		pfree(fingerprint);
	}
	list_free_deep(tables);

	/* (above READ COMMITTED it'd read what could fail another; one left behind does no harm) */
	if (!IsolationUsesXactSnapshot() &&
		SPI_execute("DELETE FROM pg_history.capture_fingerprints f WHERE NOT EXISTS "
					"(SELECT 1 FROM pg_catalog.pg_class c WHERE c.oid = f.relid)", false, 0) != SPI_OK_DELETE)
		elog(ERROR, "could not forget a table's fingerprint");
}

static bool
database_is_versioned(void)
{
	HeapTuple	tup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
	bool		versioned;

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for database %u", MyDatabaseId);
	versioned = ((Form_pg_database) GETSTRUCT(tup))->datversioned;
	ReleaseSysCache(tup);
	return versioned;
}

void
CaptureRowsAtCommit(bool prepare)
{
	SharedInvalidationMessage *msgs;
	bool		relcache;
	Oid			saved_user;
	int			saved_context;
	int			gucs;
	bool		saved_read_only = XactReadOnly;

	/* (an error here aborts the transaction, which puts all this back) */
	if (!capture_to_trees || !XLogLogicalInfoActive() || IsParallelWorker() || am_capture_worker ||
		!OidIsValid(MyDatabaseId) || IsBinaryUpgrade || IsBootstrapProcessingMode() ||
		!TransactionIdIsValid(GetTopTransactionIdIfAny()))
		return;
	CommandCounterIncrement();
	if (xactGetCommittedInvalidationMessages(&msgs, &relcache) == 0 || !database_is_versioned())
		return;

	/* (its lock would then be held until it's resolved, by every other such commit's waiting) */
	if (prepare)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("capture to trees can't prepare a transaction that changed catalogs")));

	/* (what's recorded here is the system's own, whatever this transaction was to write) */
	XactReadOnly = false;
	GetUserIdAndSecContext(&saved_user, &saved_context);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, saved_context | SECURITY_LOCAL_USERID_CHANGE | SECURITY_RESTRICTED_OPERATION);
	gucs = NewGUCNestLevel();
	(void) set_config_option("search_path", "pg_catalog, pg_temp", PGC_SUSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	(void) set_config_option("catalog_branch_filter", "off", PGC_SUSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	LockSharedObject(DatabaseRelationId, MyDatabaseId, CAPTURE_ROWS_LOCK, ExclusiveLock);
	AcceptInvalidationMessages();
	PushActiveSnapshot(GetLatestSnapshot());
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect to SPI");
	rows_compare();
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "could not finish with SPI");
	PopActiveSnapshot();
	INJECTION_POINT("capture-rows-written", NULL);
	AtEOXact_GUC(true, gucs);
	SetUserIdAndSecContext(saved_user, saved_context);
	XactReadOnly = saved_read_only;
}
