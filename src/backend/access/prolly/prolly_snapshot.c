/*-------------------------------------------------------------------------
 *
 * prolly_snapshot.c
 *	  Read-only snapshot mounts: a commit's tables as foreign tables of the
 *	  foreign data wrapper prolly_snapshot, read from the commit's trees.
 *
 * prolly_mount(source_ref, source_schema, target_schema, tables) resolves
 * the ref to one commit and makes a foreign table in target_schema (an
 * ordinary schema: one in a versioned schema would be versioned, and a
 * commit would hold it) for each table of source_schema the commit has (or
 * those named), defined as the commit describes it: its columns, by name,
 * type and collation, and nothing else (a historical constraint isn't
 * enforced, so the optimizer is never told of one).  Each foreign table's
 * options name the mount (target_schema), the commit and the table.  The
 * refs pin the commit under the mount's name, with the mount's source
 * beside it, so a collection keeps every chunk the commit reaches while
 * the mount is there.  prolly_refresh_mount() moves a mount to what a ref
 * names now, and prolly_unmount() drops its tables and its pin; both drop
 * the tables first, so they wait for every scan of them to end.  Mounting
 * reads the commit's root items alone (the canonicalizer's
 * table_metadata()): no row is read, sorted or written, no storage is
 * made and no index is built.
 *
 * A scan reads only what its mount pins (the pin must name the commit its
 * table's options do), checks the reader may read the table's history (as
 * dolt_diff() by name does; the mount checked the table's lineage), and
 * streams the commit's data tree in key order.  On a table keyed by its
 * primary key, equalities on the key's leading columns and a range on the
 * one after them bound the keys read: a seek to the first, and the scan
 * stops past the last.  The quals are all kept as the plan's filter as
 * well, so a bound is only ever an optimization.  The planner is offered
 * the key's order, forward and backward, where each key column's operator
 * family and collation are the column's own; and a scan parameterized by
 * an equality joined to another relation, re-seeking on each rescan.  Only
 * the columns the query uses are decoded (the keying's
 * decode_row_columns()): a long value of a column not used is never read.
 * A table keyed by content gives each copy of a row.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_snapshot.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "access/prolly_builtins.h"
#include "access/prolly_canon.h"
#include "access/prolly_config.h"
#include "access/prolly_keying.h"
#include "access/prolly_refs.h"
#include "access/prolly_snapshot.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "catalog/versioned.h"
#include "commands/defrem.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "executor/spi.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "pgstat.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"

#define SNAPSHOT_FDW	"prolly_snapshot"
#define SNAPSHOT_SERVER "prolly_local"

/* A pin's record: its format, then lines of text (mount_info_write()). */
#define PIN_FORMAT		"mount 1"

/* ------------------------------------------------------------------------
 * Components
 * ------------------------------------------------------------------------
 */

static const ProllyRefsRoutine *
refs(void)
{
	ProllyDatabaseComponents dc;

	prolly_database_components(&dc);
	return dc.refs;
}

static const ProllyCanonRoutine *
canonicalizer(void)
{
	ProllyDatabaseComponents dc;

	prolly_database_components(&dc);
	return dc.canonicalizer;
}

static void
require_snapshots(void)
{
	const ProllyRefsRoutine *r = refs();
	const ProllyCanonRoutine *cn = canonicalizer();

	if (!PROLLY_HAS(r, pin_set) || !PROLLY_HAS(cn, table_metadata) ||
		!PROLLY_HAS(cn, table_names))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("the database's version control can't mount its history")));
}

/* ------------------------------------------------------------------------
 * A mount's record, kept with its pin
 * ------------------------------------------------------------------------
 */

typedef struct MountInfo
{
	Oid			owner;			/* the role that mounted it */
	char	   *source_ref;
	char	   *source_schema;
	bool		all;			/* every table of the schema, at each refresh */
	List	   *tables;			/* unqualified names */
} MountInfo;

static void
mount_info_write(const MountInfo *mi, StringInfo out)
{
	ListCell   *lc;

	appendStringInfo(out, "%s\n%u\n%s\n%s\n%c\n", PIN_FORMAT, mi->owner,
					 mi->source_ref, mi->source_schema, mi->all ? 'a' : 'n');
	foreach(lc, mi->tables)
		appendStringInfo(out, "%s\n", (char *) lfirst(lc));
}

/* (names never hold a newline: mount refuses one; the owner is local) */
static void
mount_info_read(const char *data, int len, MountInfo *mi)
{
	char	   *copy = pnstrdup(data, len);
	char	   *line;
	char	   *next;
	int			n = 0;

	memset(mi, 0, sizeof(*mi));
	for (line = copy; line && *line; line = next)
	{
		next = strchr(line, '\n');
		if (next)
			*next++ = '\0';
		switch (n++)
		{
			case 0:
				if (strcmp(line, PIN_FORMAT) != 0)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("a snapshot mount's record is of another format: \"%s\"",
									line),
							 errhint("Unmount it and mount it again.")));
				break;
			case 1:
				mi->owner = (Oid) strtoul(line, NULL, 10);
				break;
			case 2:
				mi->source_ref = pstrdup(line);
				break;
			case 3:
				mi->source_schema = pstrdup(line);
				break;
			case 4:
				mi->all = (line[0] == 'a');
				break;
			default:
				mi->tables = lappend(mi->tables, pstrdup(line));
				break;
		}
	}
	if (n < 5)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid snapshot mount record")));
}

/*
 * The pin of the mount in schema nsp: named by the schema's OID, which a
 * rename keeps, so a mount follows its schema and no other schema takes
 * its name's pin.
 */
static char *
pin_name(Oid nsp)
{
	return psprintf("%u", nsp);
}

/* The mount in schema nsp: its commit and record; false if none. */
static bool
mount_lookup(Oid nsp, ProllyHash *commit, MountInfo *mi)
{
	StringInfoData info;
	ProllyHash	ignored;
	bool		found;

	if (!OidIsValid(nsp))
		return false;
	initStringInfo(&info);
	found = PROLLY_CALL(refs(), pin_get, pin_name(nsp), commit ? commit : &ignored,
						&info);
	if (found && mi)
		mount_info_read(info.data, info.len, mi);
	pfree(info.data);
	return found;
}

/* ------------------------------------------------------------------------
 * Making a mount's tables
 * ------------------------------------------------------------------------
 */

/*
 * The foreign data wrapper and its server, made the first time a database
 * mounts: by the bootstrap superuser, the server usable by everyone (a
 * table of it a user makes without a mount reads nothing).
 */
static void
ensure_server(void)
{
	Oid			save_userid;
	int			save_sec_context;

	if (OidIsValid(get_foreign_server_oid(SNAPSHOT_SERVER, true)))
		return;
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID,
						   save_sec_context | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "SPI_connect failed");
		if (!OidIsValid(get_foreign_data_wrapper_oid(SNAPSHOT_FDW, true)) &&
			SPI_execute("CREATE FOREIGN DATA WRAPPER " SNAPSHOT_FDW
						" HANDLER pg_catalog.prolly_snapshot_fdw_handler",
						false, 0) != SPI_OK_UTILITY)
			elog(ERROR, "could not make the snapshot foreign data wrapper");
		if (SPI_execute("CREATE SERVER " SNAPSHOT_SERVER " FOREIGN DATA WRAPPER "
						SNAPSHOT_FDW, false, 0) != SPI_OK_UTILITY ||
			SPI_execute("GRANT USAGE ON FOREIGN SERVER " SNAPSHOT_SERVER
						" TO PUBLIC", false, 0) != SPI_OK_UTILITY)
			elog(ERROR, "could not make the snapshot server");
		SPI_finish();
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(save_userid, save_sec_context);
	}
	PG_END_TRY();
}

/* The commit ref names, refused if it's the working set or names none. */
static void
resolve_commit(const char *ref, ProllyHash *commit, ProllyCommitInfo *info)
{
	if (pg_strcasecmp(ref, "WORKING") == 0 || pg_strcasecmp(ref, "STAGED") == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a snapshot mount reads a commit, not the working set"),
				 errhint("Commit the working set, then mount the commit.")));
	if (!PROLLY_CALL(refs(), resolve, ref, commit))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("prolly ref \"%s\" does not exist", ref)));
	if (!PROLLY_CALL(refs(), commit_get, commit, info))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly commit %s is missing", prolly_hash_hex(commit))));
}

static void
check_name(const char *name, const char *what)
{
	if (strchr(name, '\n') != NULL || name[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("invalid %s name \"%s\"", what, name)));
}

static void
run_ddl(const char *sql)
{
	if (SPI_execute(sql, false, 0) != SPI_OK_UTILITY)
		elog(ERROR, "could not run \"%s\"", sql);
}

/*
 * Make mi's tables of commit (root) in target, as the user: each foreign
 * table defined as the commit describes its table.  Refused, before any is
 * made, if one can't be read here as stored, or the user may not read it.
 */
static void
make_tables(const char *target, const ProllyHash *commit,
			const ProllyNodeRef *root, MountInfo *mi, MemoryContext cxt,
			const char *fn)
{
	char	   *hex = prolly_hash_hex(commit);
	List	   *defs = NIL;
	ListCell   *lc;

	if (mi->all)
	{
		List	   *names = PROLLY_CALL(canonicalizer(), table_names, root,
										mi->source_schema);

		MemoryContext old = MemoryContextSwitchTo(cxt);

		/* (the record outlives SPI's memory) */
		mi->tables = NIL;
		foreach(lc, names)
		{
			List	   *parts = stringToQualifiedNameList(lfirst(lc), NULL);

			mi->tables = lappend(mi->tables, pstrdup(strVal(lsecond(parts))));
		}
		MemoryContextSwitchTo(old);
		if (mi->tables == NIL)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("%s has no tables in schema \"%s\"", mi->source_ref,
							mi->source_schema)));
	}
	foreach(lc, mi->tables)
	{
		const char *table = lfirst(lc);
		char	   *name = quote_qualified_identifier(mi->source_schema, table);
		ProllyHistoricalTable t;
		bool		found;
		const char *why;
		StringInfoData sql;

		check_name(table, "table");
		why = PROLLY_CALL(canonicalizer(), table_metadata, root, name, &t, &found);
		if (!found)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("%s has no table \"%s\"", mi->source_ref, name)));
		if (why)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("%s can't mount \"%s\": %s", fn, name, why)));
		prolly_check_history_by_name(name, hex, true, fn);

		initStringInfo(&sql);
		appendStringInfo(&sql, "CREATE FOREIGN TABLE %s (",
						 quote_qualified_identifier(target, table));
		for (int i = 0; i < t.desc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(t.desc, i);

			appendStringInfo(&sql, "%s%s %s", i > 0 ? ", " : "",
							 quote_identifier(NameStr(att->attname)),
							 format_type_with_typemod(att->atttypid, att->atttypmod));
			if (OidIsValid(att->attcollation) &&
				att->attcollation != get_typcollation(att->atttypid))
				appendStringInfo(&sql, " COLLATE %s",
								 generate_collation_name(att->attcollation));
		}
		appendStringInfo(&sql, ") SERVER " SNAPSHOT_SERVER
						 " OPTIONS (commit %s, source %s)",
						 quote_literal_cstr(hex), quote_literal_cstr(name));
		defs = lappend(defs, sql.data);
	}
	foreach(lc, defs)
		run_ddl(lfirst(lc));
}

/* Drop mi's tables in target, those still there. */
static void
drop_tables(const char *target, const MountInfo *mi)
{
	ListCell   *lc;

	foreach(lc, mi->tables)
		run_ddl(psprintf("DROP FOREIGN TABLE IF EXISTS %s",
						 quote_qualified_identifier(target, lfirst(lc))));
}

/* Refuse a change to a mount by anyone but the role that made it. */
static void
check_mount_owner(const char *target, const MountInfo *mi)
{
	if (mi->owner != GetUserId() && !superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be the owner of snapshot mount \"%s\"", target)));
}

/*
 * Refuse a mount's target the user may not make it in: a system schema
 * (one named pg_*, or information_schema), or one the user doesn't own.
 */
static void
check_target(const char *target)
{
	Oid			nsp = get_namespace_oid(target, true);

	if (strncmp(target, "pg_", 3) == 0 || strcmp(target, "information_schema") == 0)
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("a snapshot mount can't be in system schema \"%s\"", target)));
	if (OidIsValid(nsp) &&
		!object_ownercheck(NamespaceRelationId, nsp, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_SCHEMA, target);
	if (!OidIsValid(nsp) &&
		object_aclcheck(DatabaseRelationId, MyDatabaseId, GetUserId(),
						ACL_CREATE) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_DATABASE,
					   get_database_name(MyDatabaseId));
}

/*
 * prolly_mount(source_ref text, source_schema text, target_schema text,
 *				tables text[] DEFAULT NULL, OUT mount text, OUT commit text)
 *	  returns record
 */
Datum
prolly_mount(PG_FUNCTION_ARGS)
{
	char	   *ref;
	char	   *target;
	MountInfo	mi = {0};
	ProllyHash	commit;
	ProllyCommitInfo info;
	Oid			nsp;
	StringInfoData rec;
	ProllyValue recv;
	TupleDesc	tupdesc;
	Datum		values[2];
	bool		nulls[2] = {false, false};
	MemoryContext cxt;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("prolly_mount's ref and schemas can't be null")));
	require_snapshots();
	ref = text_to_cstring(PG_GETARG_TEXT_PP(0));
	mi.source_schema = text_to_cstring(PG_GETARG_TEXT_PP(1));
	target = text_to_cstring(PG_GETARG_TEXT_PP(2));
	check_name(ref, "ref");
	check_name(mi.source_schema, "schema");
	check_name(target, "schema");
	mi.source_ref = ref;
	mi.owner = GetUserId();
	mi.all = PG_ARGISNULL(3);
	if (!mi.all)
	{
		Datum	   *elems;
		bool	   *enulls;
		int			n;

		deconstruct_array_builtin(PG_GETARG_ARRAYTYPE_P(3), TEXTOID, &elems,
								  &enulls, &n);
		for (int i = 0; i < n; i++)
		{
			if (enulls[i])
				ereport(ERROR,
						(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
						 errmsg("prolly_mount's tables can't be null")));
			mi.tables = lappend(mi.tables, TextDatumGetCString(elems[i]));
		}
	}
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	if (!mi.all && mi.tables == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("prolly_mount's tables can't be empty")));
	check_target(target);
	resolve_commit(ref, &commit, &info);
	if (mount_lookup(get_namespace_oid(target, true), NULL, NULL))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("schema \"%s\" is a snapshot mount already", target),
				 errhint("Refresh it with prolly_refresh_mount(), or unmount it first.")));
	ensure_server();
	cxt = CurrentMemoryContext;
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	nsp = get_namespace_oid(target, true);
	if (!OidIsValid(nsp))
	{
		run_ddl(psprintf("CREATE SCHEMA %s", quote_identifier(target)));
		CommandCounterIncrement();
		nsp = get_namespace_oid(target, false);
	}
	if (namespace_is_versioned(nsp))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("a snapshot mount can't be in versioned schema \"%s\"", target),
				 errdetail("A commit would hold its tables' definitions.")));
	make_tables(target, &commit, &info.root, &mi, cxt, "prolly_mount");
	SPI_finish();

	initStringInfo(&rec);
	mount_info_write(&mi, &rec);
	recv.data = (const uint8 *) rec.data;
	recv.len = rec.len;
	PROLLY_CALL(refs(), pin_set, pin_name(nsp), &commit, &recv);

	values[0] = CStringGetTextDatum(target);
	values[1] = CStringGetTextDatum(prolly_hash_hex(&commit));
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(tupdesc),
													  values, nulls)));
}

/*
 * prolly_refresh_mount(mount text, source_ref text DEFAULT NULL)
 *	  returns text
 *
 * The mount moved to the commit source_ref names now (NULL: the ref it was
 * mounted from): its tables dropped and made again, as of that commit.
 */
Datum
prolly_refresh_mount(PG_FUNCTION_ARGS)
{
	char	   *target;
	Oid			nsp;
	MountInfo	mi;
	ProllyHash	old;
	ProllyHash	commit;
	ProllyCommitInfo info;
	StringInfoData rec;
	ProllyValue recv;
	MemoryContext cxt;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("prolly_refresh_mount's mount can't be null")));
	require_snapshots();
	target = text_to_cstring(PG_GETARG_TEXT_PP(0));
	nsp = get_namespace_oid(target, false);
	if (!mount_lookup(nsp, &old, &mi))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("schema \"%s\" is not a snapshot mount", target)));
	check_mount_owner(target, &mi);
	if (!PG_ARGISNULL(1))
	{
		mi.source_ref = text_to_cstring(PG_GETARG_TEXT_PP(1));
		check_name(mi.source_ref, "ref");
	}
	resolve_commit(mi.source_ref, &commit, &info);
	cxt = CurrentMemoryContext;
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	drop_tables(target, &mi);
	CommandCounterIncrement();
	make_tables(target, &commit, &info.root, &mi, cxt, "prolly_refresh_mount");
	SPI_finish();
	initStringInfo(&rec);
	mount_info_write(&mi, &rec);
	recv.data = (const uint8 *) rec.data;
	recv.len = rec.len;
	PROLLY_CALL(refs(), pin_set, pin_name(nsp), &commit, &recv);
	PG_RETURN_TEXT_P(cstring_to_text(prolly_hash_hex(&commit)));
}

/*
 * prolly_unmount(mount text) returns void
 *
 * The mount's tables dropped, then its pin: the commit is collected once
 * nothing else keeps it.
 */
Datum
prolly_unmount(PG_FUNCTION_ARGS)
{
	char	   *target;
	Oid			nsp;
	MountInfo	mi;
	ProllyHash	commit;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("prolly_unmount's mount can't be null")));
	require_snapshots();
	target = text_to_cstring(PG_GETARG_TEXT_PP(0));
	nsp = get_namespace_oid(target, false);
	if (!mount_lookup(nsp, &commit, &mi))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("schema \"%s\" is not a snapshot mount", target)));
	check_mount_owner(target, &mi);
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	drop_tables(target, &mi);
	SPI_finish();
	PROLLY_CALL(refs(), pin_set, pin_name(nsp), NULL, NULL);
	PG_RETURN_VOID();
}

/*
 * A schema nsp dropped (doDeletion()): if it is a snapshot mount's, the
 * mount goes with it, its pin released, as unmounting it would.
 */
void
prolly_snapshot_schema_dropped(Oid nsp)
{
	ProllyHash	commit;
	const ProllyRefsRoutine *r;

	if (!IsNormalProcessingMode() || namespace_is_versioned(nsp))
		return;
	r = refs();
	if (!PROLLY_HAS(r, pin_get))
		return;
	if (PROLLY_CALL(r, pin_get, pin_name(nsp), &commit, NULL))
		PROLLY_CALL(r, pin_set, pin_name(nsp), NULL, NULL);
}

typedef struct MountsVisit
{
	ReturnSetInfo *rsinfo;
} MountsVisit;

static void
visit_mount(const char *name, const ProllyHash *commit, const ProllyValue *info,
			void *arg)
{
	MountsVisit *mv = arg;
	MountInfo	mi;
	Datum		values[5];
	bool		nulls[5] = {0};
	Datum	   *elems;
	int			n = 0;
	ListCell   *lc;

	mount_info_read((const char *) info->data, info->len, &mi);
	elems = palloc(sizeof(Datum) * Max(list_length(mi.tables), 1));
	foreach(lc, mi.tables)
		elems[n++] = CStringGetTextDatum(lfirst(lc));
	{
		char	   *schema = get_namespace_name((Oid) strtoul(name, NULL, 10));

		values[0] = CStringGetTextDatum(schema ? schema : name);
	}
	values[1] = CStringGetTextDatum(prolly_hash_hex(commit));
	values[2] = CStringGetTextDatum(mi.source_ref);
	values[3] = CStringGetTextDatum(mi.source_schema);
	values[4] = PointerGetDatum(construct_array_builtin(elems, n, TEXTOID));
	tuplestore_putvalues(mv->rsinfo->setResult, mv->rsinfo->setDesc, values,
						 nulls);
}

/*
 * prolly_mounts(OUT mount text, OUT commit text, OUT source_ref text,
 *				 OUT source_schema text, OUT tables text[]) returns setof record
 */
Datum
prolly_mounts(PG_FUNCTION_ARGS)
{
	MountsVisit mv;

	require_snapshots();
	InitMaterializedSRF(fcinfo, 0);
	mv.rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	PROLLY_CALL(refs(), pins, visit_mount, &mv);
	return (Datum) 0;
}

/* ------------------------------------------------------------------------
 * A mounted table, as a scan opens it
 * ------------------------------------------------------------------------
 */

typedef struct Mounted
{
	char	   *mount;
	char	   *source;			/* the table's name in the commit */
	ProllyHash	commit;
	ProllyHistoricalTable t;
	double		count;			/* the tree's items */
} Mounted;

/*
 * The mounted table relid's snapshot: its options, checked against the
 * pin of the mount in its schema, and the table as the commit describes it, checked against
 * the foreign table's columns.
 */
static void
snapshot_open(Oid relid, Mounted *s)
{
	ForeignTable *ft = GetForeignTable(relid);
	char	   *hex = NULL;
	ProllyHash	pinned;
	ProllyCommitInfo info;
	MountInfo	mi;
	ListCell   *lc;
	bool		found;
	const char *why;
	Relation	rel;
	TupleDesc	desc;

	require_snapshots();
	memset(s, 0, sizeof(*s));
	foreach(lc, ft->options)
	{
		DefElem    *d = lfirst(lc);

		if (strcmp(d->defname, "commit") == 0)
			hex = defGetString(d);
		else if (strcmp(d->defname, "source") == 0)
			s->source = defGetString(d);
	}
	s->mount = get_namespace_name(get_rel_namespace(relid));
	if (hex == NULL || s->source == NULL ||
		!prolly_hash_from_hex(hex, strlen(hex), &s->commit))
		ereport(ERROR,
				(errcode(ERRCODE_FDW_OPTION_NAME_NOT_FOUND),
				 errmsg("foreign table \"%s\" isn't a snapshot mount's table",
						get_rel_name(relid)),
				 errhint("Make snapshot tables with prolly_mount().")));
	if (!mount_lookup(get_rel_namespace(relid), &pinned, &mi) ||
		pinned.len != s->commit.len ||
		memcmp(pinned.bytes, s->commit.bytes, pinned.len) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("snapshot mount \"%s\" doesn't pin the commit of table \"%s\"",
						s->mount, get_rel_name(relid)),
				 errhint("Mount the commit with prolly_mount(), or refresh the mount.")));
	/* (the mount's own tables alone: those it checked could be read) */
	found = false;
	foreach(lc, mi.tables)
		if (strcmp(quote_qualified_identifier(mi.source_schema, lfirst(lc)),
				   s->source) == 0)
			found = true;
	if (!found)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("snapshot mount \"%s\" doesn't mount \"%s\", which table \"%s\" names",
						s->mount, s->source, get_rel_name(relid))));
	if (!PROLLY_CALL(refs(), commit_get, &s->commit, &info))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly commit %s is missing", hex)));
	why = PROLLY_CALL(canonicalizer(), table_metadata, &info.root, s->source,
					  &s->t, &found);
	if (!found)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("commit %s has no table \"%s\"", hex, s->source)));
	if (why)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("can't read \"%s\" as commit %s stores it: %s",
						s->source, hex, why)));

	/* the foreign table's columns must still be the version's */
	rel = table_open(relid, NoLock);
	desc = RelationGetDescr(rel);
	found = desc->natts == s->t.desc->natts;
	for (int i = 0; found && i < desc->natts; i++)
		found = !TupleDescAttr(desc, i)->attisdropped &&
			TupleDescAttr(desc, i)->atttypid == TupleDescAttr(s->t.desc, i)->atttypid &&
			TupleDescAttr(desc, i)->atttypmod == TupleDescAttr(s->t.desc, i)->atttypmod;
	table_close(rel, NoLock);
	if (!found)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("foreign table \"%s\" no longer has the columns of \"%s\" as its commit has them",
						get_rel_name(relid), s->source),
				 errhint("Refresh its mount with prolly_refresh_mount().")));
}

/* The keying of a version's rows: by its primary key, or by content. */
static ProllyKeying *
open_rows(const ProllyComponents *c, const ProllyHistoricalTable *t,
		  const char *source, MemoryContext mcxt)
{
	ProllyKeying *k;

	if (!t->keyed)
		return PROLLY_CALL(c->keying, open_described, t->desc, mcxt);
	if (!PROLLY_HAS(c->keying, open_keyed) ||
		(k = PROLLY_CALL(c->keying, open_keyed, t->desc, t->nkeys,
						 t->key_attnos, t->opfamilies, t->collations,
						 t->options, mcxt)) == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("can't order the keys of \"%s\" as its version stores them",
						source)));
	return k;
}

/* ------------------------------------------------------------------------
 * Key bounds
 * ------------------------------------------------------------------------
 */

/*
 * What a path's quals bound of the key: the expressions the leading neq
 * key columns equal, then a lower and an upper bound (in SQL's order, not
 * the tree's) on the column after them.
 */
typedef struct KeyBounds
{
	int			neq;
	Expr	   *eq[INDEX_MAX_KEYS];
	Expr	   *lo;
	bool		lo_strict;
	Expr	   *hi;
	bool		hi_strict;
} KeyBounds;

/*
 * The integer types, whose cross-type btree operators (int84eq and the
 * like) bound a key: a value of one converts exactly to another, or is out
 * of its range (bound_value()).
 */
static bool
integer_type(Oid type)
{
	return type == INT2OID || type == INT4OID || type == INT8OID;
}

/*
 * A bound's value d of type from as the key column's type to: false if it
 * is out of that type's range (no key equals it).
 */
static bool
bound_value(Datum d, Oid from, Oid to, Datum *out)
{
	int64		v;

	if (from == to)
	{
		*out = d;
		return true;
	}
	v = from == INT2OID ? DatumGetInt16(d) : from == INT4OID ?
		DatumGetInt32(d) : DatumGetInt64(d);
	if (to == INT2OID && v >= PG_INT16_MIN && v <= PG_INT16_MAX)
		*out = Int16GetDatum((int16) v);
	else if (to == INT4OID && v >= PG_INT32_MIN && v <= PG_INT32_MAX)
		*out = Int32GetDatum((int32) v);
	else if (to == INT8OID)
		*out = Int64GetDatum(v);
	else
		return false;
	return true;
}

/*
 * Whether clause bounds key column k of rel: Var op expr (or expr op Var),
 * op a btree operator of the column's family for its type on the left and
 * on the right (or, for integers, another integer type), under the
 * column's collation, expr not of rel and not volatile.  Sets
 * the strategy (as Var op expr) and expr.
 */
static bool
key_clause(RelOptInfo *rel, const Mounted *s, int k, RestrictInfo *ri,
		   int *strategy, Expr **expr)
{
	OpExpr	   *op;
	Node	   *left;
	Node	   *right;
	Oid			opno;
	AttrNumber	attno = s->t.key_attnos[k];
	Oid			type = TupleDescAttr(s->t.desc, attno - 1)->atttypid;
	int			strat;
	Oid			lefttype;
	Oid			righttype;

	if (!IsA(ri->clause, OpExpr))
		return false;
	op = (OpExpr *) ri->clause;
	if (list_length(op->args) != 2)
		return false;
	left = linitial(op->args);
	right = lsecond(op->args);
	opno = op->opno;
	if (!(IsA(left, Var) && ((Var *) left)->varno == rel->relid &&
		  ((Var *) left)->varattno == attno && ((Var *) left)->varlevelsup == 0))
	{
		Node	   *tmp = left;

		left = right;
		right = tmp;
		opno = get_commutator(opno);
		if (!OidIsValid(opno) ||
			!(IsA(left, Var) && ((Var *) left)->varno == rel->relid &&
			  ((Var *) left)->varattno == attno &&
			  ((Var *) left)->varlevelsup == 0))
			return false;
	}
	if (bms_is_member(rel->relid, pull_varnos(NULL, right)) ||
		contain_volatile_functions(right))
		return false;
	if (!op_in_opfamily(opno, s->t.opfamilies[k]))
		return false;
	get_op_opfamily_properties(opno, s->t.opfamilies[k], false, &strat,
							   &lefttype, &righttype);
	if (lefttype != type || exprType(right) != righttype ||
		(righttype != type && !(integer_type(type) && integer_type(righttype))))
		return false;
	if (OidIsValid(s->t.collations[k]) && op->inputcollid != s->t.collations[k])
		return false;
	*strategy = strat;
	*expr = (Expr *) right;
	return true;
}

/* The bounds clauses (RestrictInfos) put on rel's key. */
static void
key_bounds(RelOptInfo *rel, const Mounted *s, List *clauses, KeyBounds *b)
{
	memset(b, 0, sizeof(*b));
	if (!s->t.keyed)
		return;
	for (int k = 0; k < s->t.nkeys; k++)
	{
		ListCell   *lc;
		bool		eq = false;

		foreach(lc, clauses)
		{
			int			strat;
			Expr	   *e;

			if (!key_clause(rel, s, k, lfirst(lc), &strat, &e))
				continue;
			if (strat == BTEqualStrategyNumber)
			{
				b->eq[k] = e;
				eq = true;
				break;
			}
		}
		if (eq)
		{
			b->neq++;
			continue;
		}
		foreach(lc, clauses)
		{
			int			strat;
			Expr	   *e;

			if (!key_clause(rel, s, k, lfirst(lc), &strat, &e))
				continue;
			if ((strat == BTGreaterStrategyNumber ||
				 strat == BTGreaterEqualStrategyNumber) && b->lo == NULL)
			{
				b->lo = e;
				b->lo_strict = (strat == BTGreaterStrategyNumber);
			}
			else if ((strat == BTLessStrategyNumber ||
					  strat == BTLessEqualStrategyNumber) && b->hi == NULL)
			{
				b->hi = e;
				b->hi_strict = (strat == BTLessStrategyNumber);
			}
		}
		break;
	}
}

/* ------------------------------------------------------------------------
 * Planning
 * ------------------------------------------------------------------------
 */

/* Per-tuple costs: reading a leaf item, and decoding a row. */
#define ITEM_COST		(cpu_tuple_cost)
#define DECODE_COST		(4 * cpu_operator_cost)

/*
 * A seek's bounds, as keys of the tree's order: what a scan computes from
 * its bound expressions' values, and the planner from constants, to count
 * the keys between them.
 */
typedef struct Seek
{
	const Mounted *s;
	ProllyComponents c;
	ProllyKeying *k;
	int			neq;
	bool		has_lo;
	bool		has_hi;
	bool		lo_strict;		/* (in SQL's order) */
	bool		hi_strict;

	StringInfo	tree_lo;		/* NULL: none */
	bool		tree_lo_strict;
	StringInfo	tree_hi;
	bool		tree_hi_strict;
	bool		empty;			/* no key can be in them */
} Seek;

/*
 * One of the tree's bounds: the equality prefix, then v (if has_v), as the
 * least key with them (high: the greatest).
 */
static StringInfo
tree_bound(Seek *sk, Datum *values, bool *isnull, bool has_v, Datum v,
		   bool high)
{
	StringInfo	key = makeStringInfo();
	int			n = sk->neq;

	if (has_v)
	{
		values[n] = v;
		isnull[n] = false;
		n++;
	}
	PROLLY_CALL(sk->c.keying, encode_key_bound, sk->k, values, isnull, n,
				high, key);
	return key;
}

/*
 * The sign of an integer bound's value: which way it is out of a smaller
 * type's range.
 */
static int
integer_sign(Datum d, Oid type)
{
	int64		v = type == INT2OID ? DatumGetInt16(d) : type == INT4OID ?
		DatumGetInt32(d) : DatumGetInt64(d);

	return v > 0 ? 1 : -1;
}

/*
 * The bounds as keys of the tree's order, from the bounds' values (each
 * NULL where nulls says, of the types types say: the equalities', then the
 * lower bound's and the upper's, those the seek has), in the current
 * memory context.  A strict lower bound is the greatest key with its
 * values (keys after it are past them), an inclusive one the least; an
 * upper bound the other way round.  On a DESC column SQL's lower bound is
 * the tree's upper.  No key equals NULL or is past it, nor equals an
 * integer out of its column's range; a range bound out of it is either
 * past every key or none.
 */
static void
compute_bounds(Seek *sk, const Datum *in, const bool *nulls, const Oid *types)
{
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	const Mounted *s = sk->s;
	int			i = 0;
	Datum		lo = 0;
	Datum		hi = 0;
	bool		has_lo = sk->has_lo;
	bool		has_hi = sk->has_hi;
	bool		desc;
	bool		has_tlo;
	bool		has_thi;
	Datum		tlo;
	Datum		thi;
	bool		tlo_strict;
	bool		thi_strict;
	int			nexprs = sk->neq + (sk->has_lo ? 1 : 0) + (sk->has_hi ? 1 : 0);

	sk->empty = false;
	sk->tree_lo = sk->tree_hi = NULL;
	for (i = 0; i < nexprs; i++)
	{
		int			c = Min(i, sk->neq);
		Oid			to = TupleDescAttr(s->t.desc, s->t.key_attnos[c] - 1)->atttypid;
		bool		is_lo = (i == sk->neq && sk->has_lo);
		bool		is_eq = (i < sk->neq);
		Datum		d;

		if (nulls[i])
		{
			sk->empty = true;
			return;
		}
		if (!bound_value(in[i], types[i], to, &d))
		{
			int			sign = integer_sign(in[i], types[i]);

			/* (equal to no key; past every key, or before every one) */
			if (is_eq || (is_lo && sign > 0) || (!is_lo && sign < 0))
			{
				sk->empty = true;
				return;
			}
			if (is_lo)
				has_lo = false;
			else
				has_hi = false;
			continue;
		}
		if (is_eq)
		{
			values[i] = d;
			isnull[i] = false;
		}
		else if (is_lo)
			lo = d;
		else
			hi = d;
	}
	if (sk->neq == 0 && !has_lo && !has_hi)
		return;
	desc = sk->neq < s->t.nkeys && (s->t.options[sk->neq] & INDOPTION_DESC) != 0;
	has_tlo = desc ? has_hi : has_lo;
	has_thi = desc ? has_lo : has_hi;
	tlo = desc ? hi : lo;
	thi = desc ? lo : hi;
	tlo_strict = desc ? sk->hi_strict : sk->lo_strict;
	thi_strict = desc ? sk->lo_strict : sk->hi_strict;
	if (has_tlo || sk->neq > 0)
	{
		sk->tree_lo_strict = has_tlo && tlo_strict;
		sk->tree_lo = tree_bound(sk, values, isnull, has_tlo, tlo,
								 sk->tree_lo_strict);
	}
	if (has_thi || sk->neq > 0)
	{
		sk->tree_hi_strict = has_thi && thi_strict;
		sk->tree_hi = tree_bound(sk, values, isnull, has_thi, thi,
								 !sk->tree_hi_strict);
	}
}

typedef struct MountPlan
{
	Mounted		s;
	ProllyComponents c;
	ProllyKeying *k;			/* (keyed) for counting keys in bounds */
	ProllyTreeEnv env;
	ProllyKeyingRefs refs;
} MountPlan;

/* The key's order as pathkeys (NIL where it can't be told the planner). */
static List *
key_pathkeys(PlannerInfo *root, RelOptInfo *rel, const Mounted *s,
			 bool backward)
{
	List	   *keys = NIL;

	/*
	 * (a base relation's alone: an appendrel member's equivalence members
	 * are its parent's, translated, and making new ones for it is wrong)
	 */
	if (!s->t.keyed || rel->reloptkind != RELOPT_BASEREL)
		return NIL;
	for (int k = 0; k < s->t.nkeys; k++)
	{
		Form_pg_attribute att = TupleDescAttr(s->t.desc, s->t.key_attnos[k] - 1);
		Var		   *var;
		bool		desc = (s->t.options[k] & INDOPTION_DESC) != 0;
		bool		nulls_first = (s->t.options[k] & INDOPTION_NULLS_FIRST) != 0;
		Oid			opno;
		List	   *pk;

		/* (a key ordered by another collation than its column's: not told) */
		if (att->attcollation != s->t.collations[k] || nulls_first != desc)
			break;
		if (backward)
			desc = !desc;
		opno = get_opfamily_member(s->t.opfamilies[k], att->atttypid,
								   att->atttypid,
								   desc ? BTGreaterStrategyNumber : BTLessStrategyNumber);
		if (!OidIsValid(opno))
			break;
		var = makeVar(rel->relid, s->t.key_attnos[k], att->atttypid,
					  att->atttypmod, att->attcollation, 0);
		pk = build_expression_pathkey(root, (Expr *) var, opno, rel->relids,
									  true);

		/*
		 * (as an index's pathkeys: one a constant fixes, or given already,
		 * says nothing of the order)
		 */
		if (pk != NIL &&
			!((PathKey *) linitial(pk))->pk_eclass->ec_has_const &&
			!list_member_ptr(keys, linitial(pk)))
			keys = list_concat(keys, pk);
	}
	return keys;
}

static int
plan_compare(const ProllyKey *a, const ProllyKey *b, void *arg)
{
	MountPlan  *ps = arg;

	return PROLLY_CALL(ps->c.keying, compare, ps->k, a, b);
}

/* Whether the version's tree has key. */
static bool
tree_has(MountPlan *ps, const ProllyKey *key)
{
	StringInfoData v;
	bool		found;

	initStringInfo(&v);
	found = PROLLY_CALL(ps->c.tree, get, &ps->env, &ps->s.t.data, key, &v);
	pfree(v.data);
	return found;
}

/*
 * How many keys bounds b holds, exactly, where every bound is a constant
 * and the tree counts by rank (the keys below each end); -1 if not.
 */
static double
exact_rows(MountPlan *ps, const KeyBounds *b)
{
	Seek		sk = {0};
	Datum		values[INDEX_MAX_KEYS + 2];
	bool		nulls[INDEX_MAX_KEYS + 2];
	Oid			types[INDEX_MAX_KEYS + 2];
	int			n = 0;
	uint64		lo;
	uint64		hi;
	Expr	   *exprs[INDEX_MAX_KEYS + 2];

	if (!ps->s.t.keyed || ps->k == NULL || !PROLLY_HAS(ps->c.tree, rank))
		return -1;
	for (int c = 0; c < b->neq; c++)
		exprs[n++] = b->eq[c];
	if (b->lo)
		exprs[n++] = b->lo;
	if (b->hi)
		exprs[n++] = b->hi;
	for (int i = 0; i < n; i++)
	{
		if (!IsA(exprs[i], Const))
			return -1;
		values[i] = ((Const *) exprs[i])->constvalue;
		nulls[i] = ((Const *) exprs[i])->constisnull;
		types[i] = ((Const *) exprs[i])->consttype;
	}
	sk.s = &ps->s;
	sk.c = ps->c;
	sk.k = ps->k;
	sk.neq = b->neq;
	sk.has_lo = b->lo != NULL;
	sk.has_hi = b->hi != NULL;
	sk.lo_strict = b->lo_strict;
	sk.hi_strict = b->hi_strict;
	compute_bounds(&sk, values, nulls, types);
	if (sk.empty)
		return 0;
	/*
	 * (a bound of every key column is a key itself: rank() doesn't count
	 * it, so a strict lower bound skips it and an inclusive upper takes it)
	 */
	if (sk.tree_lo)
	{
		ProllyKey	key = {(const uint8 *) sk.tree_lo->data, sk.tree_lo->len};

		lo = PROLLY_CALL(ps->c.tree, rank, &ps->env, &ps->s.t.data, &key);
		if (sk.tree_lo_strict && tree_has(ps, &key))
			lo++;
	}
	else
		lo = 0;
	if (sk.tree_hi)
	{
		ProllyKey	key = {(const uint8 *) sk.tree_hi->data, sk.tree_hi->len};

		hi = PROLLY_CALL(ps->c.tree, rank, &ps->env, &ps->s.t.data, &key);
		if (!sk.tree_hi_strict && tree_has(ps, &key))
			hi++;
	}
	else
		hi = (uint64) ps->s.count;
	return hi > lo ? (double) (hi - lo) : 0;
}

static List *bound_clauses(RelOptInfo *rel, const Mounted *s,
						   const KeyBounds *b, List *clauses);

static void
snapshot_get_rel_size(PlannerInfo *root, RelOptInfo *baserel,
					  Oid foreigntableid)
{
	MountPlan  *ps = palloc0(sizeof(MountPlan));
	KeyBounds	b;
	double		exact;

	snapshot_open(foreigntableid, &ps->s);
	prolly_check_history_by_name(ps->s.source, prolly_hash_hex(&ps->s.commit),
								 true, "a snapshot mount");
	prolly_history_components(&ps->c);
	prolly_folded_tree_env(&ps->env);
	ps->s.count = (double) PROLLY_CALL(ps->c.tree, count, &ps->env, &ps->s.t.data);
	if (ps->s.t.keyed)
	{
		ps->k = open_rows(&ps->c, &ps->s.t, ps->s.source, CurrentMemoryContext);
		ps->env.compare = plan_compare;
		ps->env.compare_arg = ps;
		prolly_keying_use_env(&ps->refs, ps->c.keying, ps->k, ps->c.tree, &ps->env);
	}
	baserel->fdw_private = ps;
	baserel->tuples = ps->s.count;
	baserel->rows = clamp_row_est(ps->s.count *
								  clauselist_selectivity(root,
														 baserel->baserestrictinfo,
														 0, JOIN_INNER, NULL));
	key_bounds(baserel, &ps->s, baserel->baserestrictinfo, &b);
	exact = exact_rows(ps, &b);
	if (exact >= 0)
	{
		List	   *rest = list_difference_ptr(baserel->baserestrictinfo,
											   bound_clauses(baserel, &ps->s, &b,
															 baserel->baserestrictinfo));

		baserel->rows = clamp_row_est(exact *
									  clauselist_selectivity(root, rest, 0,
															 JOIN_INNER, NULL));
	}
	if (b.neq == ps->s.t.nkeys && ps->s.t.keyed)
		baserel->rows = 1;
}

/* The clauses that bound the key's columns bounds b bounds. */
static List *
bound_clauses(RelOptInfo *rel, const Mounted *s, const KeyBounds *b,
			  List *clauses)
{
	List	   *used = NIL;
	ListCell   *lc;

	foreach(lc, clauses)
	{
		RestrictInfo *ri = lfirst(lc);
		int			strat;
		Expr	   *e;

		for (int k = 0; k <= b->neq && k < s->t.nkeys; k++)
			if (key_clause(rel, s, k, ri, &strat, &e))
			{
				used = lappend(used, ri);
				break;
			}
	}
	return used;
}

/* How many rows bounds b reads, of a table of count. */
static double
bounded_rows(PlannerInfo *root, RelOptInfo *rel, MountPlan *ps,
			 const KeyBounds *b, List *clauses)
{
	const Mounted *s = &ps->s;
	List	   *used;
	double		exact;

	if (s->t.keyed && b->neq == s->t.nkeys)
		return 1;
	if (b->neq == 0 && b->lo == NULL && b->hi == NULL)
		return Max(s->count, 1);
	exact = exact_rows(ps, b);
	if (exact >= 0)
		return clamp_row_est(exact);
	used = bound_clauses(rel, s, b, clauses);
	return clamp_row_est(s->count * clauselist_selectivity(root, used, 0,
														   JOIN_INNER, NULL));
}

/* A path's private list: direction, then the bounds (fdw_private). */
static List *
path_private(const KeyBounds *b, bool backward)
{
	List	   *eqs = NIL;

	for (int k = 0; k < b->neq; k++)
		eqs = lappend(eqs, b->eq[k]);
	return list_make5(makeInteger(backward ? 1 : 0), eqs,
					  b->lo ? (Node *) b->lo : (Node *) makeInteger(0),
					  b->hi ? (Node *) b->hi : (Node *) makeInteger(0),
					  list_make2_int(b->lo_strict, b->hi_strict));
}

static void
add_paths(PlannerInfo *root, RelOptInfo *rel, MountPlan *ps, List *clauses,
		  ParamPathInfo *ppi)
{
	KeyBounds	b;
	double		read;
	double		rows;
	Cost		startup;
	Cost		run;
	double		depth;

	key_bounds(rel, &ps->s, clauses, &b);
	if (ppi && b.neq == 0 && b.lo == NULL && b.hi == NULL)
		return;
	read = bounded_rows(root, rel, ps, &b, clauses);
	rows = ppi ? clamp_row_est(Min(read, ppi->ppi_rows)) : Min(read, rel->rows);
	if (ppi)
		ppi->ppi_rows = rows;
	depth = ps->s.count > 1 ? ceil(log(ps->s.count) / log(64.0)) + 1 : 1;
	startup = depth * random_page_cost;
	run = read * (ITEM_COST + DECODE_COST) +
		read * cpu_operator_cost * list_length(rel->baserestrictinfo);
	for (int dir = 0; dir < 2; dir++)
	{
		List	   *pathkeys = key_pathkeys(root, rel, &ps->s, dir == 1);

		/* (backward only where it gives an order) */
		if (dir == 1 && pathkeys == NIL)
			continue;
		add_path(rel, (Path *)
				 create_foreignscan_path(root, rel, NULL, rows, 0, startup,
										 startup + run, pathkeys,
										 ppi ? ppi->ppi_req_outer : NULL,
										 NULL, NIL,
										 path_private(&b, dir == 1)));
	}
}

typedef struct KeyMemberArg
{
	const Mounted *s;
	Index		relid;
	int			k;				/* the key column sought */
} KeyMemberArg;

static bool
key_member(PlannerInfo *root, RelOptInfo *rel, EquivalenceClass *ec,
		   EquivalenceMember *em, void *arg)
{
	KeyMemberArg *a = arg;
	Var		   *v = (Var *) em->em_expr;

	return IsA(v, Var) && v->varno == a->relid && v->varlevelsup == 0 &&
		v->varattno == a->s->t.key_attnos[a->k];
}

static void
snapshot_get_paths(PlannerInfo *root, RelOptInfo *baserel,
				   Oid foreigntableid)
{
	MountPlan  *ps = baserel->fdw_private;
	List	   *ppis = NIL;
	ListCell   *lc;

	add_paths(root, baserel, ps, baserel->baserestrictinfo, NULL);
	if (!ps->s.t.keyed)
		return;

	/*
	 * Parameterized by an equality of a key column to another relation's
	 * expression, from the join clauses and the equivalence classes: a
	 * nested loop re-seeks for each outer row.
	 */
	foreach(lc, baserel->joininfo)
	{
		RestrictInfo *ri = lfirst(lc);
		int			strat;
		Expr	   *e;
		Relids		outer;

		if (!join_clause_is_movable_to(ri, baserel))
			continue;
		for (int k = 0; k < ps->s.t.nkeys; k++)
			if (key_clause(baserel, &ps->s, k, ri, &strat, &e) &&
				strat == BTEqualStrategyNumber)
			{
				outer = bms_del_member(bms_union(ri->clause_relids,
												 baserel->lateral_relids),
									   baserel->relid);
				if (!bms_is_empty(outer))
					ppis = list_append_unique_ptr(ppis,
												  get_baserel_parampathinfo(root, baserel, outer));
				break;
			}
	}
	if (baserel->has_eclass_joins)
	{
		for (int k = 0; k < ps->s.t.nkeys; k++)
		{
			KeyMemberArg a = {&ps->s, baserel->relid, k};
			List	   *clauses = generate_implied_equalities_for_column(root, baserel,
																		 key_member, &a,
																		 baserel->lateral_referencers);

			foreach(lc, clauses)
			{
				RestrictInfo *ri = lfirst(lc);
				Relids		outer;

				if (!join_clause_is_movable_to(ri, baserel))
					continue;
				outer = bms_del_member(bms_union(ri->clause_relids,
												 baserel->lateral_relids),
									   baserel->relid);
				if (!bms_is_empty(outer))
					ppis = list_append_unique_ptr(ppis,
												  get_baserel_parampathinfo(root, baserel, outer));
			}
		}
	}
	foreach(lc, ppis)
	{
		ParamPathInfo *ppi = lfirst(lc);

		add_paths(root, baserel, ps,
				  list_concat_copy(baserel->baserestrictinfo, ppi->ppi_clauses),
				  ppi);
	}
}

/* The attribute numbers the query uses of rel (0: the whole row). */
static List *
used_columns(RelOptInfo *rel, List *quals)
{
	Bitmapset  *attrs = NULL;
	List	   *cols = NIL;
	int			x = -1;

	pull_varattnos((Node *) rel->reltarget->exprs, rel->relid, &attrs);
	pull_varattnos((Node *) quals, rel->relid, &attrs);
	while ((x = bms_next_member(attrs, x)) >= 0)
	{
		AttrNumber	attno = x + FirstLowInvalidHeapAttributeNumber;

		if (attno == 0)
			return list_make1_int(0);
		if (attno > 0)
			cols = lappend_int(cols, attno);
	}
	return cols;
}

static ForeignScan *
snapshot_get_plan(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid,
				  ForeignPath *best_path, List *tlist, List *scan_clauses,
				  Plan *outer_plan)
{
	List	   *priv = best_path->fdw_private;
	List	   *eqs = lsecond(priv);
	Node	   *lo = lthird(priv);
	Node	   *hi = lfourth(priv);
	List	   *strict = (List *) list_nth(priv, 4);
	List	   *exprs = list_copy(eqs);
	List	   *quals;

	/* (every qual a filter too: the bounds only spare reading rows) */
	quals = extract_actual_clauses(scan_clauses, false);
	if (!IsA(lo, Integer))
		exprs = lappend(exprs, lo);
	if (!IsA(hi, Integer))
		exprs = lappend(exprs, hi);
	return make_foreignscan(tlist, quals, baserel->relid, exprs,
							list_make5(linitial(priv),
									   makeInteger(list_length(eqs)),
									   list_make2_int(!IsA(lo, Integer), !IsA(hi, Integer)),
									   strict,
									   used_columns(baserel, quals)),
							NIL, NIL, outer_plan);
}

/* ------------------------------------------------------------------------
 * Scanning
 * ------------------------------------------------------------------------
 */

typedef struct MountScan
{
	Mounted		s;
	Seek		seek;			/* its components, keying and bounds */
	ProllyTreeEnv env;
	ProllyKeyingRefs refs;
	ProllyTreeCursor *cursor;
	bool		backward;
	List	   *exprs;			/* ExprStates of the bounds */
	Oid		   *types;			/* and their types */
	bool	   *wanted;			/* attributes decoded; NULL: all */
	bool		done;
	uint64		copies;			/* copies of the row in the slot still to give */
	MemoryContext cxt;
	MemoryContext boundcxt;		/* the bounds', reset at each (re)scan */

	/* work, for EXPLAIN ANALYZE */
	uint64		rows_read;
	uint64		nodes_before;
	uint64		nodes_read;
} MountScan;

static int
keying_compare(const ProllyKey *a, const ProllyKey *b, void *arg)
{
	MountScan  *ss = arg;

	return PROLLY_CALL(ss->seek.c.keying, compare, ss->seek.k, a, b);
}

static void
snapshot_begin(ForeignScanState *node, int eflags)
{
	ForeignScan *fs = (ForeignScan *) node->ss.ps.plan;
	MountScan  *ss = palloc0(sizeof(MountScan));
	Relation	rel = node->ss.ss_currentRelation;
	List	   *priv = fs->fdw_private;
	List	   *has = lthird(priv);
	List	   *strict = lfourth(priv);
	List	   *cols = (List *) list_nth(priv, 4);
	MemoryContext old;
	ListCell   *lc;
	int			i = 0;

	node->fdw_state = ss;
	ss->backward = intVal(linitial(priv)) != 0;
	ss->seek.neq = intVal(lsecond(priv));
	ss->seek.has_lo = linitial_int(has);
	ss->seek.has_hi = lsecond_int(has);
	ss->seek.lo_strict = linitial_int(strict);
	ss->seek.hi_strict = lsecond_int(strict);
	ss->exprs = ExecInitExprList(fs->fdw_exprs, (PlanState *) node);
	ss->types = palloc(sizeof(Oid) * Max(list_length(fs->fdw_exprs), 1));
	foreach(lc, fs->fdw_exprs)
		ss->types[i++] = exprType(lfirst(lc));
	if (!(list_length(cols) == 1 && linitial_int(cols) == 0))
	{
		ss->wanted = palloc0(sizeof(bool) * Max(RelationGetDescr(rel)->natts, 1));
		foreach(lc, cols)
			ss->wanted[lfirst_int(lc) - 1] = true;
	}
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	snapshot_open(RelationGetRelid(rel), &ss->s);
	prolly_check_history_by_name(ss->s.source, prolly_hash_hex(&ss->s.commit),
								 true, "a snapshot mount");
	ss->seek.s = &ss->s;
	ss->cxt = AllocSetContextCreate(CurrentMemoryContext, "prolly snapshot scan",
									ALLOCSET_DEFAULT_SIZES);
	ss->boundcxt = AllocSetContextCreate(ss->cxt, "prolly snapshot scan bounds",
										 ALLOCSET_SMALL_SIZES);
	old = MemoryContextSwitchTo(ss->cxt);
	prolly_history_components(&ss->seek.c);
	ss->seek.k = open_rows(&ss->seek.c, &ss->s.t, ss->s.source, ss->cxt);
	prolly_folded_tree_env(&ss->env);
	ss->env.compare = keying_compare;
	ss->env.compare_arg = ss;
	prolly_keying_use_env(&ss->refs, ss->seek.c.keying, ss->seek.k,
						  ss->seek.c.tree, &ss->env);
	MemoryContextSwitchTo(old);
	ss->nodes_before = pgstat_prolly_tree_counters()->nodes_read;
}

/* The bounds, from the bound expressions' values now. */
static void
set_bounds(ForeignScanState *node, MountScan *ss)
{
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	int			n = list_length(ss->exprs);
	Datum	   *values;
	bool	   *nulls;
	ListCell   *lc;
	int			i = 0;
	MemoryContext old;

	/* (the values, and what evaluating them makes, freed at the next) */
	MemoryContextReset(ss->boundcxt);
	old = MemoryContextSwitchTo(ss->boundcxt);
	values = palloc(sizeof(Datum) * Max(n, 1));
	nulls = palloc(sizeof(bool) * Max(n, 1));
	foreach(lc, ss->exprs)
	{
		values[i] = ExecEvalExpr(lfirst(lc), econtext, &nulls[i]);
		i++;
	}
	compute_bounds(&ss->seek, values, nulls, ss->types);
	MemoryContextSwitchTo(old);
}

static void
start_scan(ForeignScanState *node, MountScan *ss)
{
	ProllyKey	at;
	StringInfo	from;

	set_bounds(node, ss);
	ss->done = ss->seek.empty;
	ss->copies = 0;
	if (ss->done)
		return;
	if (ss->cursor == NULL)
		ss->cursor = PROLLY_CALL(ss->seek.c.tree, cursor_open, &ss->env,
								 &ss->s.t.data, ss->cxt);
	from = ss->backward ? ss->seek.tree_hi : ss->seek.tree_lo;
	if (from)
	{
		at.data = (const uint8 *) from->data;
		at.len = from->len;
	}
	PROLLY_CALL(ss->seek.c.tree, cursor_seek, ss->cursor, from ? &at : NULL,
				ss->backward ? BackwardScanDirection : ForwardScanDirection);
}

static int
compare_bound(MountScan *ss, const ProllyKey *key, StringInfo bound)
{
	ProllyKey	b = {(const uint8 *) bound->data, bound->len};

	return PROLLY_CALL(ss->seek.c.keying, compare, ss->seek.k, key, &b);
}

/* Is key past the end the scan goes toward? */
static bool
past_end(MountScan *ss, const ProllyKey *key)
{
	StringInfo	end = ss->backward ? ss->seek.tree_lo : ss->seek.tree_hi;
	bool		strict = ss->backward ? ss->seek.tree_lo_strict : ss->seek.tree_hi_strict;
	int			c;

	if (end == NULL)
		return false;
	c = compare_bound(ss, key, end);
	if (ss->backward)
		c = -c;
	return strict ? c >= 0 : c > 0;
}

/* Is key short of a strict start, a seek having put the scan at it? */
static bool
before_start(MountScan *ss, const ProllyKey *key)
{
	StringInfo	start = ss->backward ? ss->seek.tree_hi : ss->seek.tree_lo;
	bool		strict = ss->backward ? ss->seek.tree_hi_strict : ss->seek.tree_lo_strict;
	int			c;

	if (start == NULL || !strict)
		return false;
	c = compare_bound(ss, key, start);
	if (ss->backward)
		c = -c;
	return c <= 0;
}

static TupleTableSlot *
snapshot_iterate(ForeignScanState *node)
{
	MountScan  *ss = node->fdw_state;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	ProllyKey	key;
	ProllyValue value;

	if (ss->cursor == NULL && !ss->done)
		start_scan(node, ss);
	/* (the slot still holds the row, decoded once for all its copies) */
	if (ss->copies > 0 && !TTS_EMPTY(slot))
	{
		ss->copies--;
		return slot;
	}
	ExecClearTuple(slot);
	while (!ss->done)
	{
		ProllyValue row;
		uint64		count;

		CHECK_FOR_INTERRUPTS();
		if (!PROLLY_CALL(ss->seek.c.tree, cursor_next, ss->cursor, &key, &value))
		{
			ss->done = true;
			break;
		}
		if (past_end(ss, &key))
		{
			ss->done = true;
			break;
		}
		if (before_start(ss, &key))
			continue;
		PROLLY_CALL(ss->seek.c.keying, parse_value_as, ss->seek.k,
					!ss->s.t.keyed, &value, &row, &count);
		PROLLY_CALL(ss->seek.c.keying, decode_row_columns, ss->seek.k, &row,
					ss->wanted, slot);
		ss->rows_read++;
		ss->copies = count - 1;
		return slot;
	}
	return slot;
}

static void
snapshot_rescan(ForeignScanState *node)
{
	MountScan  *ss = node->fdw_state;

	ss->done = false;
	start_scan(node, ss);
}

static void
snapshot_end(ForeignScanState *node)
{
	MountScan  *ss = node->fdw_state;

	if (ss == NULL || ss->cxt == NULL)
		return;
	ss->nodes_read = pgstat_prolly_tree_counters()->nodes_read - ss->nodes_before;
	if (ss->cursor)
		PROLLY_CALL(ss->seek.c.tree, cursor_close, ss->cursor);
	ss->cursor = NULL;
	PROLLY_CALL(ss->seek.c.keying, close, ss->seek.k);
	MemoryContextDelete(ss->cxt);
	ss->cxt = NULL;
}

static void
snapshot_explain(ForeignScanState *node, ExplainState *es)
{
	MountScan  *ss = node->fdw_state;
	ForeignScan *fs = (ForeignScan *) node->ss.ps.plan;
	List	   *priv = fs->fdw_private;
	List	   *has = lthird(priv);
	int			neq = intVal(lsecond(priv));
	StringInfoData access;

	initStringInfo(&access);
	if (neq == 0 && !linitial_int(has) && !lsecond_int(has))
		appendStringInfoString(&access, "full");
	else
	{
		appendStringInfo(&access, "primary key, %d equal", neq);
		if (linitial_int(has) || lsecond_int(has))
			appendStringInfoString(&access, ", range");
	}
	if (intVal(linitial(priv)))
		appendStringInfoString(&access, ", backward");
	ExplainPropertyText("Snapshot Access", access.data, es);
	if (es->verbose && ss && ss->s.source)
	{
		ExplainPropertyText("Snapshot Commit", prolly_hash_hex(&ss->s.commit), es);
		ExplainPropertyText("Snapshot Table", ss->s.source, es);
	}
	if (es->analyze && ss)
	{
		uint64		nodes = ss->cxt ?
			pgstat_prolly_tree_counters()->nodes_read - ss->nodes_before :
			ss->nodes_read;

		ExplainPropertyUInteger("Rows Read", NULL, ss->rows_read, es);
		if (es->verbose)
			ExplainPropertyUInteger("Nodes Read", NULL, nodes, es);
	}
}

/*
 * prolly_snapshot_fdw_handler() returns fdw_handler
 */
Datum
prolly_snapshot_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *r = makeNode(FdwRoutine);

	r->GetForeignRelSize = snapshot_get_rel_size;
	r->GetForeignPaths = snapshot_get_paths;
	r->GetForeignPlan = snapshot_get_plan;
	r->BeginForeignScan = snapshot_begin;
	r->IterateForeignScan = snapshot_iterate;
	r->ReScanForeignScan = snapshot_rescan;
	r->EndForeignScan = snapshot_end;
	r->ExplainForeignScan = snapshot_explain;
	PG_RETURN_POINTER(r);
}
