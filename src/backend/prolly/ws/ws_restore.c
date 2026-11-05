/*-------------------------------------------------------------------------
 *
 * ws_restore.c
 *	  A versioned schema as a root value, and a root value as a schema.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/ws/ws_restore.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "prolly/ws/ws_fk.h"
#include "prolly/ws/ws_obj.h"
#include "prolly/ws/ws_restore.h"
#include "prolly/ws/ws_role.h"
#include "prolly/ws/ws_root.h"
#include "prolly/ws/ws_rows.h"
#include "prolly/ws/ws_schema.h"
#include "prolly/ws/ws_sql.h"
#include "utils/builtins.h"

/* ---------------------------------------------------------------- capture */

static int
name_cmp(const void *a, const void *b)
{
	return strcmp(*(char *const *) a, *(char *const *) b);
}

/* What a list of names holds once, in order. */
static int
names_distinct(char **names, int n)
{
	int			kept = 0;

	if (n > 1)
		qsort(names, n, sizeof(char *), name_cmp);
	for (int i = 0; i < n; i++)
		if (kept == 0 || strcmp(names[kept - 1], names[i]) != 0)
			names[kept++] = names[i];
	return kept;
}

static void
name_add(char ***names, int *n, const char *name)
{
	*names = *names == NULL ? palloc_array(char *, 1) : repalloc_array(*names, char *, *n + 1);
	(*names)[(*n)++] = pstrdup(name);
}

static void
schema_capture(ProllyCtx *ctx, const char *schema, PHash *root)
{
	const char *params[1] = {schema};
	WsSqlRows	tables;
	PHash		rv = vc_root_empty(ctx);
	WsObject   *objs;
	int			nobjs;
	WsSeqPosition *seqs;
	int			nseqs;
	WsForeignKey *all = NULL;
	int			nall = 0;
	char	  **roles = NULL;
	int			nroles = 0;
	PHash		map;

	WsSql->read("SELECT c.relname::text FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
				"WHERE n.nspname = $1::name AND c.relkind IN ('r', 'p') ORDER BY c.relname COLLATE \"C\"",
				1, params, &tables);
	for (int i = 0; i < tables.nrows; i++)
	{
		const char *table = ws_sql_value(&tables, i, 0);
		WsTableSchema rec;
		PHash		rows;
		WsForeignKey *fks;
		int			nfks;

		CHECK_FOR_INTERRUPTS();
		if (!ws_schema_read(schema, table, &rec))
			elog(ERROR, "table \"%s\" of schema \"%s\" is gone", table, schema);
		WsRows->capture(ctx, schema, &rec, &rows);
		WsRoot->put_table(ctx, &rv, &rec, &rows, &rv);
		if (rec.owner != NULL)
			name_add(&roles, &nroles, rec.owner);
		for (int g = 0; g < rec.ngrants; g++)
			if (rec.grants[g].grantee != NULL)
				name_add(&roles, &nroles, rec.grants[g].grantee);
		ws_schema_free(&rec);
		ws_fk_read(schema, table, &fks, &nfks);
		if (nfks > 0)
		{
			all = all == NULL ? palloc_array(WsForeignKey, nfks) : repalloc_array(all, WsForeignKey, nall + nfks);
			memcpy(all + nall, fks, nfks * sizeof(WsForeignKey));
			nall += nfks;
		}
	}
	ws_sql_rows_free(&tables);
	if (nall > 0)
	{
		ws_fk_map(ctx, all, nall, &map);
		WsRoot->put_map(ctx, &rv, WS_ROOT_FOREIGN_KEYS, &map, &rv);
	}
	ws_obj_read(schema, &objs, &nobjs);
	if (nobjs > 0)
	{
		ws_obj_map(ctx, objs, nobjs, &map);
		WsRoot->put_map(ctx, &rv, WS_ROOT_OBJECTS, &map, &rv);
		for (int i = 0; i < nobjs; i++)
			ws_obj_roles(&objs[i], &roles, &nroles);
	}
	ws_seq_read(schema, &seqs, &nseqs);
	if (nseqs > 0)
	{
		ws_seq_map(ctx, seqs, nseqs, &map);
		WsRoot->put_map(ctx, &rv, WS_ROOT_SEQUENCES, &map, &rv);
	}
	nroles = names_distinct(roles, nroles);
	if (nroles > 0)
	{
		WsRole	   *held;
		int			nheld;

		ws_role_read((const char *const *) roles, nroles, &held, &nheld);
		ws_role_map(ctx, held, nheld, &map);
		WsRoot->put_map(ctx, &rv, WS_ROOT_ROLES, &map, &rv);
	}
	*root = rv;
}

/* ---------------------------------------------------------------- restore */

static void
run_statement(Buf *sql)
{
	WsSqlRows	rows;

	if (sql->len == 0)
		return;
	buf_append_byte(sql, 0);
	WsSql->run((const char *) sql->data, 0, NULL, &rows);
	ws_sql_rows_free(&rows);
	sql->len = 0;
}

/* A root's tables, in records, each after the tables it is a partition or child of. */
typedef struct TableOrder
{
	const PHash *root;
	ProllyCtx  *ctx;
	char	  **names;
	int			nnames;
	WsTableSchema *recs;
	PHash	   *rows;
	bool	   *placed;
	int		   *order;
	int			n;
} TableOrder;

static bool
order_name(void *arg, const char *name, const VcTable *entry)
{
	TableOrder *o = arg;

	o->names = o->names == NULL ? palloc_array(char *, 1) : repalloc_array(o->names, char *, o->nnames + 1);
	o->names[o->nnames++] = pstrdup(name);
	return true;
}

static int
table_index(const TableOrder *o, const char *name)
{
	for (int i = 0; i < o->nnames; i++)
		if (strcmp(o->names[i], name) == 0)
			return i;
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("a table of this root's names table \"%s\", which it does not hold", name)));
}

static void
place_table(TableOrder *o, int i, int depth)
{
	const WsTableSchema *rec = &o->recs[i];

	if (o->placed[i])
		return;
	if (depth > o->nnames)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("table \"%s\" of this root is a partition or child of itself", rec->name)));
	if (rec->partition_of != NULL)
		place_table(o, table_index(o, rec->partition_of), depth + 1);
	for (int p = 0; p < rec->ninherits; p++)
		place_table(o, table_index(o, rec->inherits[p]), depth + 1);
	o->placed[i] = true;
	o->order[o->n++] = i;
}

static void
tables_in_order(ProllyCtx *ctx, const PHash *root, TableOrder *o)
{
	memset(o, 0, sizeof(*o));
	o->ctx = ctx;
	o->root = root;
	WsRoot->tables(ctx, root, order_name, o);
	o->recs = palloc_array(WsTableSchema, Max(o->nnames, 1));
	o->rows = palloc_array(PHash, Max(o->nnames, 1));
	o->placed = palloc0_array(bool, Max(o->nnames, 1));
	o->order = palloc_array(int, Max(o->nnames, 1));
	for (int i = 0; i < o->nnames; i++)
		if (!WsRoot->get_table(ctx, root, o->names[i], &o->recs[i], &o->rows[i]))
			elog(ERROR, "a table listed is not there");
	for (int i = 0; i < o->nnames; i++)
		place_table(o, i, 0);
}

static void
make_table(ProllyCtx *ctx, const WsTableSchema *s, const PHash *rows, const char *schema)
{
	Buf			sql = BUF_INIT;

	ws_schema_create_table(s, schema, &sql);
	run_statement(&sql);
	ws_schema_column_settings(s, schema, &sql);
	run_statement(&sql);
	ws_schema_set_tags(schema, s);
	CommandCounterIncrement();

	/* (its rows before its indexes, which then build once) */
	WsRows->restore(ctx, schema, s, rows);
	for (int i = 0; i < s->nindexes; i++)
	{
		ws_schema_create_index(s, i, schema, &sql);
		run_statement(&sql);
	}
	for (int i = 0; i < s->nindex_settings; i++)
	{
		ws_schema_index_stats(s, i, schema, &sql);
		run_statement(&sql);
	}
	ws_schema_table_settings(s, schema, &sql);
	run_statement(&sql);
	buf_free(&sql);
}

static void
add_foreign_keys(ProllyCtx *ctx, const PHash *root, const TableOrder *tables, const char *schema)
{
	PHash		map;
	WsForeignKey *fks;
	int			n;
	Buf			sql = BUF_INIT;

	if (!WsRoot->get_map(ctx, root, WS_ROOT_FOREIGN_KEYS, &map))
		return;
	ws_fk_list(ctx, &map, &fks, &n);
	for (int t = 0; t < tables->n; t++)
	{
		const WsTableSchema *rec = &tables->recs[tables->order[t]];

		/* (a table's last name first: the list is by table, then name) */
		for (int i = n - 1; i >= 0; i--)
			if (strcmp(fks[i].table, rec->name) == 0)
			{
				const WsTableSchema *to = &tables->recs[table_index(tables, fks[i].ref_table)];

				ws_fk_add_statement(&fks[i], rec, to, schema, &sql);
				run_statement(&sql);
			}
	}
	buf_free(&sql);
}

static void
schema_restore(ProllyCtx *ctx, const PHash *root, const PHash *previous, const char *schema)
{
	const char *params[1] = {schema};
	WsSqlRows	rows;
	int			level = WsSql->begin();
	int			nest = ws_schema_path_begin(schema);
	PHash		map;
	TableOrder	tables;
	WsObject   *objs = NULL;
	int			nobjs = 0;
	Buf			sql = BUF_INIT;

	WsSql->read("SELECT (SELECT count(*) FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
				"WHERE n.nspname = $1::name) + (SELECT count(*) FROM pg_type t JOIN pg_namespace n "
				"ON n.oid = t.typnamespace WHERE n.nspname = $1::name) + (SELECT count(*) FROM pg_proc p "
				"JOIN pg_namespace n ON n.oid = p.pronamespace WHERE n.nspname = $1::name)", 1, params, &rows);
	if (strcmp(ws_sql_value(&rows, 0, 0), "0") != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_SCHEMA),
				 errmsg("schema \"%s\" holds something, and a root is restored into one that does not", schema)));
	ws_sql_rows_free(&rows);

	if (WsRoot->get_map(ctx, root, WS_ROOT_ROLES, &map))
	{
		WsRole	   *held;
		WsRole	   *was = NULL;
		int			nheld;
		int			nwas = 0;
		PHash		before;

		ws_role_list(ctx, &map, &held, &nheld);
		if (previous != NULL && WsRoot->get_map(ctx, previous, WS_ROOT_ROLES, &before))
			ws_role_list(ctx, &before, &was, &nwas);
		ws_role_check_arrival(held, nheld, was, nwas);
		ws_role_restore(held, nheld, was, nwas);
	}
	if (WsRoot->get_map(ctx, root, WS_ROOT_OBJECTS, &map))
		ws_obj_list(ctx, &map, &objs, &nobjs);
	ws_obj_restore(objs, nobjs, schema, true);

	tables_in_order(ctx, root, &tables);
	for (int t = 0; t < tables.n; t++)
	{
		int			i = tables.order[t];

		CHECK_FOR_INTERRUPTS();
		make_table(ctx, &tables.recs[i], &tables.rows[i], schema);
	}
	for (int t = 0; t < tables.n; t++)
	{
		const WsTableSchema *s = &tables.recs[tables.order[t]];

		if (s->partition_of != NULL)
		{
			ws_schema_attach(s, schema, &sql);
			run_statement(&sql);
		}
	}
	for (int t = tables.n - 1; t >= 0; t--)
	{
		const WsTableSchema *s = &tables.recs[tables.order[t]];

		for (int i = 0; i < s->nconstraints; i++)
			if (s->constraints[i].not_valid)
			{
				ws_schema_add_check(s, i, schema, &sql);
				run_statement(&sql);
			}
	}
	for (int t = 0; t < tables.n; t++)
	{
		const WsTableSchema *s = &tables.recs[tables.order[t]];

		for (int p = 0; p < s->ninherits; p++)
		{
			ws_schema_inherit(s, p, schema, &sql);
			run_statement(&sql);
		}
	}
	for (int t = 0; t < tables.n; t++)
		ws_schema_set_inherited(schema, &tables.recs[tables.order[t]]);
	add_foreign_keys(ctx, root, &tables, schema);
	for (int t = 0; t < tables.n; t++)
	{
		const WsTableSchema *s = &tables.recs[tables.order[t]];

		for (int i = 0; i < s->ncomments; i++)
		{
			ws_schema_comment(s, i, schema, &sql);
			run_statement(&sql);
		}
	}
	for (int t = 0; t < tables.n; t++)
	{
		const WsTableSchema *s = &tables.recs[tables.order[t]];

		/* (its owner before what follows: a sequence it owns must have its owner) */
		ws_schema_owner(s, schema, &sql);
		run_statement(&sql);
		for (int i = 0; i < s->ngrants; i++)
		{
			ws_schema_grant(s, i, schema, &sql);
			run_statement(&sql);
		}
	}
	ws_obj_restore(objs, nobjs, schema, false);
	for (int t = 0; t < tables.n; t++)
	{
		ws_schema_row_security(&tables.recs[tables.order[t]], schema, &sql);
		run_statement(&sql);
	}
	if (WsRoot->get_map(ctx, root, WS_ROOT_SEQUENCES, &map))
	{
		WsSeqPosition *seqs;
		int			nseqs;

		ws_seq_list(ctx, &map, &seqs, &nseqs);
		ws_seq_restore(seqs, nseqs, schema);
	}
	CommandCounterIncrement();
	for (int t = 0; t < tables.n; t++)
		ws_schema_remember(&tables.recs[tables.order[t]], schema);
	ws_obj_remember(objs, nobjs, schema);

	for (int i = 0; i < nobjs; i++)
		ws_obj_free(&objs[i]);
	for (int i = 0; i < tables.nnames; i++)
		ws_schema_free(&tables.recs[i]);
	buf_free(&sql);
	ws_schema_path_end(nest);
	WsSql->end(level);
}

const WsRootIORoutine SchemaWsRootIO = {
	.name = "schema",
	.capture = schema_capture,
	.restore = schema_restore,
};

const WsRootIORoutine *WsRootIO = &SchemaWsRootIO;

/* --------------------------------------------------------------- a dry run */

typedef struct DryRun
{
	ProllyCtx  *ctx;
	const PHash *root;
	bool		went_through;
} DryRun;

static void
dry_run(void *arg)
{
	DryRun	   *d = arg;
	char	   *schema = psprintf("ws_dry_run_%d", MyProcPid);
	char	   *sql = psprintf("CREATE SCHEMA %s VERSIONED", quote_identifier(schema));
	WsSqlRows	rows;

	WsSql->run(sql, 0, NULL, &rows);
	ws_sql_rows_free(&rows);
	WsRootIO->restore(d->ctx, d->root, NULL, schema);
	d->went_through = true;

	/* (what makes the subtransaction undo it: the only way this one ends) */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("a dry run is undone")));
}

bool
ws_root_refused(ProllyCtx *ctx, const PHash *root, WsSqlFailure *failure)
{
	DryRun		d = {ctx, root, false};

	(void) WsSql->attempt_fn(dry_run, &d, failure);
	if (d.went_through)
	{
		ws_sql_failure_free(failure);
		memset(failure, 0, sizeof(*failure));
		return false;
	}
	return true;
}
