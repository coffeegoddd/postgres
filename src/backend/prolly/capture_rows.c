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
 * in the transaction, so that they void all of it before them, in the same
 * transaction or not, and its fingerprint is recorded.
 *
 * A lock on the database held from there to the commit has two such
 * transactions take turns, each comparing only once it has it, or each would
 * write rows without the other's change.  ShareLock on each table holds off its
 * writers, or one committing after the read and before this commits would come
 * first and be lost.  So those can deadlock, as any taking of locks late can,
 * and it's an error above READ COMMITTED, as it reads what's committed now.
 *
 * This reads with the system's rights, as one may rename the value of a type of
 * one's own that a table one can't reach has a column of.
 *
 * Text naming an object, as a regclass column prints a table's name, changes
 * when it's renamed, which no fingerprint here says.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/capture_rows.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
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
#include "catalog/pg_range.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "postmaster/autovacuum.h"
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

/* The sub-id of the lock on a database that has transactions rewriting rows take turns. */
#define CAPTURE_ROWS_LOCK 2

/* How many rows go in a message. */
#define CAPTURE_ROWS_BATCH_SIZE 1000

typedef struct RowsOut
{
	Oid			relid;
	int32		branch;
	int			nstored;
} RowsOut;

static void
put_int32(StringInfo out, int32 v)
{
	appendBinaryStringInfo(out, &v, sizeof(v));
}

static void
rows_batch(void *arg, const WsSqlRows *rows)
{
	RowsOut    *o = arg;
	StringInfoData out;

	if (rows->nrows == 0)
		return;
	initStringInfo(&out);
	appendStringInfoChar(&out, CAPTURE_ROWS_BATCH);
	put_int32(&out, o->branch);
	put_int32(&out, (int32) o->relid);
	put_int32(&out, rows->nrows);
	for (int r = 0; r < rows->nrows; r++)
	{
		for (int c = 0; c < o->nstored; c++)
		{
			const char *text = ws_sql_value(rows, r, c);

			if (text == NULL)
				put_int32(&out, -1);
			else
			{
				const char *utf8 = ws_text_to_utf8(text);
				int32		len = (int32) strlen(utf8);

				put_int32(&out, len);
				appendBinaryStringInfo(&out, utf8, len);
			}
		}
	}
	(void) LogLogicalMessage(CAPTURE_ROWS_PREFIX, out.data, out.len, true, false);
	pfree(out.data);
}

/* A table's rows, the last thing it's told of them. */
static void
rows_write(Relation rel, int32 branch)
{
	Oid			relid = RelationGetRelid(rel);
	WsRowShape	shape;
	StringInfoData out;
	Buf			sql = BUF_INIT;
	RowsOut		o;
	TupleDesc	desc = RelationGetDescr(rel);
	int			scope;
	int			on_branch;

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
	if (shape.nkey == 0)
	{
		ws_row_shape_free(&shape);
		return;
	}

	ws_put_text(&sql, "SELECT ");
	for (int i = 0; i < shape.nstored; i++)
	{
		for (int a = 0; a < desc->natts; a++)
		{
			Form_pg_attribute att = TupleDescAttr(desc, a);

			if (!att->attisdropped && (uint64) att->attcoltag == shape.tags[i])
			{
				if (i > 0)
					ws_put_text(&sql, ", ");
				ws_put_identifier(&sql, NameStr(att->attname));
				break;
			}
		}
	}
	ws_put_text(&sql, " FROM ONLY ");
	ws_put_identifier(&sql, get_namespace_name(RelationGetNamespace(rel)));
	ws_put_text(&sql, ".");
	ws_put_identifier(&sql, RelationGetRelationName(rel));
	buf_append_byte(&sql, 0);

	o.relid = relid;
	o.branch = branch;
	o.nstored = shape.nstored;
	on_branch = CatalogBranchEnter(branch);
	scope = WsSql->begin();
	WsSql->scan((const char *) sql.data, 0, NULL, CAPTURE_ROWS_BATCH_SIZE, rows_batch, &o);
	WsSql->end(scope);
	CatalogBranchLeave(on_branch);
	buf_free(&sql);
	ws_row_shape_free(&shape);
}

static int
label_cmp(const ListCell *a, const ListCell *b)
{
	return strcmp((const char *) lfirst(a), (const char *) lfirst(b));
}

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

			names = lappend(names, psprintf("%u=%s", e->oid, NameStr(e->enumlabel)));
		}
		ReleaseSysCacheList(labels);
		list_sort(names, label_cmp);
		foreach(lc, names)
			appendStringInfo(out, " %s", (char *) lfirst(lc));
	}
	else if (t->typtype == TYPTYPE_COMPOSITE)
	{
		Relation	rel = relation_open(t->typrelid, AccessShareLock);
		TupleDesc	desc = RelationGetDescr(rel);

		for (int i = 0; i < desc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(desc, i);

			if (!att->attisdropped)
				print_type(out, att->atttypid, att->atttypmod, seen);
		}
		relation_close(rel, AccessShareLock);
	}
	ReleaseSysCache(tup);
	appendStringInfoChar(out, ')');
}

static char *
rows_fingerprint(Relation rel)
{
	TupleDesc	desc = RelationGetDescr(rel);
	WsRowShape	shape;
	StringInfoData out;

	initStringInfo(&out);
	appendStringInfo(&out, "file %u key", rel->rd_rel->relfilenode);
	CaptureRelationShape(rel, &shape);
	for (int k = 0; k < shape.nkey; k++)
		appendStringInfo(&out, " " UINT64_FORMAT, shape.tags[k]);
	ws_row_shape_free(&shape);
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);
		List	   *seen = NIL;

		if (att->attisdropped || att->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL)
			continue;
		appendStringInfo(&out, " %d:" INT64_FORMAT, att->attnum, att->attcoltag);
		print_type(&out, att->atttypid, att->atttypmod, &seen);
		list_free(seen);
	}
	return out.data;
}

/* Each versioned table there is, in the snapshot running, whichever branch this is on. */
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
			tables = lappend_oid(tables, c->oid);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	return tables;
}

/*
 * Each table whose fingerprint isn't the one recorded, with those of tables gone forgotten; true if
 * any.  If write, their rows are written too, and what's recorded made so.
 */
static bool
rows_compare(bool write)
{
	List	   *tables = versioned_tables();
	bool		any = false;
	ListCell   *lc;
	Oid			types[2] = {OIDOID, TEXTOID};

	foreach(lc, tables)
	{
		Oid			relid = lfirst_oid(lc);
		Relation	rel;
		char	   *fingerprint;
		bool		same;
		Datum		values[2];

		rel = try_table_open(relid, write ? ShareLock : AccessShareLock);
		if (rel == NULL)
			continue;
		fingerprint = rows_fingerprint(rel);
		values[0] = ObjectIdGetDatum(relid);
		values[1] = CStringGetTextDatum(fingerprint);
		if (SPI_execute_with_args("SELECT 1 FROM pg_history.capture_fingerprints WHERE relid = $1 AND fingerprint = $2",
								  2, types, values, NULL, true, 1) != SPI_OK_SELECT)
			elog(ERROR, "could not read a table's fingerprint");
		same = SPI_processed == 1;
		if (!same)
		{
			any = true;
			if (write)
			{
				CommandCounterIncrement();
				rows_write(rel, NamespaceBranch(RelationGetNamespace(rel)));
				if (SPI_execute_with_args("INSERT INTO pg_history.capture_fingerprints VALUES ($1, $2) "
										  "ON CONFLICT (relid) DO UPDATE SET fingerprint = excluded.fingerprint",
										  2, types, values, NULL, false, 0) != SPI_OK_INSERT)
					elog(ERROR, "could not record a table's fingerprint");
			}
		}
		table_close(rel, write ? NoLock : AccessShareLock);
		pfree(fingerprint);
		if (any && !write)
			break;
	}
	list_free(tables);
	return any;
}

/* As a transaction that changed catalogs commits. */
static void
rows_at_commit(XactEvent event, void *arg)
{
	static bool busy = false;
	SharedInvalidationMessage *msgs;
	bool		relcache;
	Oid			saved_user;
	int			saved_context;
	int			gucs;

	if ((event != XACT_EVENT_PRE_COMMIT && event != XACT_EVENT_PRE_PREPARE) || busy)
		return;
	if (!capture_to_trees || !XLogLogicalInfoActive() || IsParallelWorker() || !AmRegularBackendProcess() ||
		!TransactionIdIsValid(GetTopTransactionIdIfAny()) || IsBinaryUpgrade)
		return;
	if (xactGetCommittedInvalidationMessages(&msgs, &relcache) == 0)
		return;

	busy = true;
	PG_TRY();
	{
		GetUserIdAndSecContext(&saved_user, &saved_context);
		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID, saved_context | SECURITY_LOCAL_USERID_CHANGE | SECURITY_RESTRICTED_OPERATION);
		gucs = NewGUCNestLevel();
		(void) set_config_option("catalog_branch_filter", "off", PGC_SUSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
		PushActiveSnapshot(GetLatestSnapshot());
		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "could not connect to SPI");
		if (rows_compare(false))
		{
			if (IsolationUsesXactSnapshot())
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("capture to trees can't commit this above READ COMMITTED"),
						 errdetail("It changes a table's rows, which capture then reads as they're committed now.")));
			LockSharedObject(DatabaseRelationId, MyDatabaseId, CAPTURE_ROWS_LOCK, ExclusiveLock);
			AcceptInvalidationMessages();
			PopActiveSnapshot();
			PushActiveSnapshot(GetLatestSnapshot());
			(void) rows_compare(true);
			INJECTION_POINT("capture-rows-written", NULL);
			if (SPI_execute("DELETE FROM pg_history.capture_fingerprints f WHERE NOT EXISTS "
							"(SELECT 1 FROM pg_catalog.pg_class c WHERE c.oid = f.relid)", false, 0) != SPI_OK_DELETE)
				elog(ERROR, "could not forget a table's fingerprint");
		}
		if (SPI_finish() != SPI_OK_FINISH)
			elog(ERROR, "could not finish with SPI");
		PopActiveSnapshot();
		AtEOXact_GUC(true, gucs);
		SetUserIdAndSecContext(saved_user, saved_context);
	}
	PG_FINALLY();
	{
		busy = false;
	}
	PG_END_TRY();
}

void
CaptureBackendInit(void)
{
	if (capture_to_trees && AmRegularBackendProcess())
		RegisterXactCallback(rows_at_commit, NULL);
}
