/*-------------------------------------------------------------------------
 *
 * capture_rows.c
 *	  Writing a table's rows into the WAL, for DDL that changes them and
 *	  tells no change of them.
 *
 * Rewriting a table, dropping a column or renaming an enum's value changes
 * what a table's rows are, and decoding sees none of it.  So the statement
 * writes the table's rows, as logical messages in its own transaction, in
 * the batches it reads them in: a CAPTURE_ROWS_BEGIN with the table's layout,
 * then CAPTURE_ROWS_BATCH of output text under the settings of a statement's
 * scope (prolly/ws/ws_sql.h).  What's told of the table after them follows
 * from them, and what came before them is void.
 *
 * It holds off writers until the transaction ends, or a change committed
 * after the read and before this commits would come first and be lost.  And
 * it reads what's committed now, which a transaction's own older snapshot
 * doesn't show, so it's an error in REPEATABLE READ or higher.
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
#include "access/table.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_class.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "prolly/branch_catalog.h"
#include "prolly/capture.h"
#include "prolly/ws/ws_schema.h"
#include "prolly/ws/ws_sql.h"
#include "replication/message.h"
#include "storage/lmgr.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

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

void
CaptureTableRowsChanged(Oid relid)
{
	Relation	rel;
	int32		branch;
	WsRowShape	shape;
	StringInfoData out;
	Buf			sql = BUF_INIT;
	RowsOut		o;
	TupleDesc	desc;
	int			scope;
	int			on_branch;

	if (!capture_to_trees || !XLogLogicalInfoActive() || get_rel_relkind(relid) != RELKIND_RELATION)
		return;
	branch = NamespaceBranch(get_rel_namespace(relid));
	if (branch == 0)
		return;
	if (IsolationUsesXactSnapshot())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("capture to trees can't take this statement in a transaction above READ COMMITTED"),
				 errdetail("It changes the rows of \"%s\", which capture then reads as they're committed now.",
						   get_rel_name(relid))));

	LockRelationOid(relid, ShareLock);
	CommandCounterIncrement();
	rel = table_open(relid, NoLock);
	desc = RelationGetDescr(rel);
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
	table_close(rel, NoLock);
}

/* Each table with a column reaching a type, through whatever is made of it. */
static void
tables_of_type(Oid type, List **types, List **tables)
{
	Relation	dep;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;

	check_stack_depth();
	if (list_member_oid(*types, type))
		return;
	*types = lappend_oid(*types, type);

	dep = table_open(DependRelationId, AccessShareLock);
	ScanKeyInit(&key[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(TypeRelationId));
	ScanKeyInit(&key[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(type));
	scan = systable_beginscan(dep, DependReferenceIndexId, true, NULL, 2, key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend d = (Form_pg_depend) GETSTRUCT(tup);

		if (d->classid == TypeRelationId)
			tables_of_type(d->objid, types, tables);
		else if (d->classid == RelationRelationId && d->objsubid > 0)
		{
			char		kind = get_rel_relkind(d->objid);

			if (kind == RELKIND_RELATION)
				*tables = list_append_unique_oid(*tables, d->objid);

			/* (whatever has this table's or composite type's rows for a column) */
			if (kind == RELKIND_RELATION || kind == RELKIND_COMPOSITE_TYPE)
				tables_of_type(get_rel_type_id(d->objid), types, tables);
		}
	}
	systable_endscan(scan);
	table_close(dep, AccessShareLock);
}

void
CaptureTypeRowsChanged(Oid type)
{
	List	   *types = NIL;
	List	   *tables = NIL;
	ListCell   *lc;

	if (!capture_to_trees || !XLogLogicalInfoActive())
		return;
	CommandCounterIncrement();
	tables_of_type(type, &types, &tables);
	foreach(lc, tables)
		CaptureTableRowsChanged(lfirst_oid(lc));
	list_free(types);
	list_free(tables);
}
