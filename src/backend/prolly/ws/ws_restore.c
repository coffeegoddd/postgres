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
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "commands/user.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type_d.h"
#include "prolly/branch_catalog.h"
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
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

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
	WsSqlRows	viewed;
	int			level = WsSql->begin();
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

	/* (run, not read: a statement may have made these, and it sees what that did) */
	WsSql->run("SELECT c.relname::text, a.attname::text, r.relname::text FROM pg_class c "
			   "JOIN pg_namespace n ON n.oid = c.relnamespace JOIN pg_attribute a ON a.attrelid = c.oid "
			   "JOIN pg_type t ON t.oid = a.atttypid LEFT JOIN pg_type e ON e.oid = t.typelem AND t.typcategory = 'A' "
			   "JOIN pg_class r ON r.oid = coalesce(nullif(t.typrelid, 0), e.typrelid) "
			   "WHERE n.nspname = $1::name AND c.relkind IN ('r', 'p') AND NOT a.attisdropped AND r.relkind IN ('v', 'm') "
			   "AND r.relnamespace = n.oid LIMIT 1", 1, params, &viewed);
	if (viewed.nrows > 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("column \"%s\" of table \"%s\" in schema \"%s\" cannot be held in a commit: its type is the row of view \"%s\", which is made after tables",
						ws_sql_value(&viewed, 0, 1), ws_sql_value(&viewed, 0, 0), schema, ws_sql_value(&viewed, 0, 2))));
	ws_sql_rows_free(&viewed);
	WsSql->run("SELECT c.relname::text FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
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

		/* (as a commit holds them, and it reads them by this database's names) */
		for (int i = 0; i < nroles; i++)
			roles[i] = ws_name_from_utf8(roles[i]);
		ws_role_read((const char *const *) roles, nroles, &held, &nheld);
		ws_role_map(ctx, held, nheld, &map);
		WsRoot->put_map(ctx, &rv, WS_ROOT_ROLES, &map, &rv);
	}
	WsSql->end(level);
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
	bool	   *visiting;
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

/* The table a column's type is the row of, or an array of, if any. */
static int
table_named_by_type(const TableOrder *o, const char *type)
{
	char	   *name = pstrdup(type);
	char	   *cut = strchr(name, '[');
	int			found = -1;

	if (cut != NULL)
		*cut = '\0';
	if (name[0] == '"' && name[strlen(name) - 1] == '"')
	{
		char	   *to = name;

		name[strlen(name) - 1] = '\0';
		for (const char *p = name + 1; *p != '\0'; p++)
		{
			*to++ = *p;
			if (p[0] == '"' && p[1] == '"')
				p++;
		}
		*to = '\0';
	}

	/* (a name a built-in type has too is taken as that: a table's would fail loudly) */
	if (!OidIsValid(GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum(name),
									ObjectIdGetDatum(PG_CATALOG_NAMESPACE))))
		for (int i = 0; found < 0 && i < o->nnames; i++)
			if (strcmp(o->names[i], name) == 0)
				found = i;
	pfree(name);
	return found;
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

	/* (a column of another table's row type: it has to be there) */
	o->visiting[i] = true;
	for (int c = 0; c < rec->ncols; c++)
	{
		int			of = table_named_by_type(o, rec->cols[c].type);

		if (of >= 0 && !o->visiting[of])
			place_table(o, of, depth + 1);
	}
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
	o->visiting = palloc0_array(bool, Max(o->nnames, 1));
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

	/*
	 * Its owner before them: an index runs what it is made of as its table's owner,
	 * which must not be whoever is restoring this.  That takes having that owner's
	 * rights, for all that is done to the table after.
	 */
	if (s->owner != NULL)
	{
		char	   *owner = ws_name_from_utf8(s->owner);

		if (!has_privs_of_role(GetUserId(), get_role_oid(owner, false)))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("restoring table \"%s\" takes having the privileges of its owner \"%s\"", s->name, owner),
					 errhint("Being able to SET ROLE to it is not enough.")));
		pfree(owner);
	}
	ws_schema_owner(s, schema, &sql);
	run_statement(&sql);
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

/*
 * A record of what its rows need, sharing what it keeps: its key's and identities' not
 * null, which a record must have, and indexes that refuse nothing.
 */
static void
bare_record(const WsTableSchema *s, WsTableSchema *out)
{
	*out = *s;
	out->cols = palloc_array(WsColumn, Max(s->ncols, 1));
	for (int i = 0; i < s->ncols; i++)
	{
		bool		keep = s->cols[i].identity != WS_IDENTITY_NONE;

		for (int k = 0; !keep && k < s->nkey; k++)
			keep = s->key[k] == s->cols[i].tag;
		out->cols[i] = s->cols[i];
		if (!keep)
			out->cols[i].not_null = NULL;
	}
	out->nconstraints = 0;
	out->indexes = palloc_array(WsIndex, Max(s->nindexes, 1));
	out->nindexes = 0;
	for (int i = 0; i < s->nindexes; i++)
		if (!s->indexes[i].unique)
			out->indexes[out->nindexes++] = s->indexes[i];
	out->index_settings = palloc_array(WsIndexSettings, Max(s->nindex_settings, 1));
	out->nindex_settings = 0;
	for (int i = 0; i < s->nindex_settings; i++)
	{
		const char *index = s->index_settings[i].index;
		bool		keep = s->key_name != NULL && strcmp(index, s->key_name) == 0;

		for (int k = 0; !keep && k < out->nindexes; k++)
			keep = strcmp(index, out->indexes[k].name) == 0;
		if (keep)
			out->index_settings[out->nindex_settings++] = s->index_settings[i];
	}
	out->ninherits = 0;
	out->ninherited_cols = 0;
	out->ninherited_cons = 0;
	out->replica_identity = WS_REPLICA_DEFAULT;
	out->replica_index = NULL;
	out->clustered = NULL;
	out->row_security = false;
	out->force_row_security = false;
	out->ncomments = 0;
	out->ngrants = 0;
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

/*
 * A function that runs as its owner runs as no one it should with that owner not here:
 * refused, as what it works out could not be what it means.
 */
static void
refuse_definers_with_no_owner(const char *schema)
{
	const char *params[1] = {schema};
	WsSqlRows	rows;

	WsSql->run("SELECT p.proname::text FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace "
			   "WHERE n.nspname = $1::name AND p.prosecdef AND p.proowner = current_user::regrole LIMIT 1",
			   1, params, &rows);
	if (rows.nrows > 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("function \"%s\" runs as its owner, a role not here", ws_sql_value(&rows, 0, 0))));
	ws_sql_rows_free(&rows);
}

/*
 * Restoring: all of it; with fn, only what restore_rows says, then fn; with no_rows, all
 * of it, each table holding that, or if definitions_only, what restore_definitions says.
 */
static void
restore_as(ProllyCtx *ctx, const PHash *root, const PHash *previous, const char *schema,
		   void (*fn) (void *arg, const char *schema), void *arg, const PHash *no_rows,
		   bool definitions_only)
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
	bool		owner_absent = false;

	WsSql->run("SELECT (SELECT count(*) FROM pg_namespace n, LATERAL (" \
			   "SELECT 1 FROM pg_class WHERE relnamespace = n.oid UNION ALL " \
			   "SELECT 1 FROM pg_type WHERE typnamespace = n.oid UNION ALL " \
			   "SELECT 1 FROM pg_proc WHERE pronamespace = n.oid UNION ALL " \
			   "SELECT 1 FROM pg_collation WHERE collnamespace = n.oid UNION ALL " \
			   "SELECT 1 FROM pg_conversion WHERE connamespace = n.oid UNION ALL " \
			   "SELECT 1 FROM pg_operator WHERE oprnamespace = n.oid UNION ALL " \
			   "SELECT 1 FROM pg_opclass WHERE opcnamespace = n.oid UNION ALL " \
			   "SELECT 1 FROM pg_opfamily WHERE opfnamespace = n.oid UNION ALL " \
			   "SELECT 1 FROM pg_statistic_ext WHERE stxnamespace = n.oid UNION ALL " \
			   "SELECT 1 FROM pg_ts_config WHERE cfgnamespace = n.oid UNION ALL " \
			   "SELECT 1 FROM pg_ts_dict WHERE dictnamespace = n.oid UNION ALL " \
			   "SELECT 1 FROM pg_ts_parser WHERE prsnamespace = n.oid UNION ALL " \
			   "SELECT 1 FROM pg_ts_template WHERE tmplnamespace = n.oid) x WHERE n.nspname = $1::name)",
			   1, params, &rows);
	if (strcmp(ws_sql_value(&rows, 0, 0), "0") != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_SCHEMA),
				 errmsg("schema \"%s\" holds something, and a root is restored into one that does not", schema)));
	ws_sql_rows_free(&rows);

	if (!definitions_only && WsRoot->get_map(ctx, root, WS_ROOT_ROLES, &map))
	{
		WsRole	   *held;
		WsRole	   *was = NULL;
		int			nheld;
		int			nwas = 0;
		PHash		before;

		ws_role_list(ctx, &map, &held, &nheld);
		if (previous != NULL && WsRoot->get_map(ctx, previous, WS_ROOT_ROLES, &before))
		{
			/*
			 * A role this root does not name is one it no longer uses, not one a
			 * commit took a membership from: only roles it holds can have lost one.
			 */
			WsRole	   *all;
			int			nall;

			ws_role_list(ctx, &before, &all, &nall);
			was = palloc_array(WsRole, Max(nall, 1));
			for (int i = 0; i < nall; i++)
				for (int k = 0; k < nheld; k++)
					if (strcmp(all[i].name, held[k].name) == 0)
					{
						was[nwas++] = all[i];
						break;
					}
		}
		ws_role_check_arrival(held, nheld, was, nwas);
		ws_role_restore(held, nheld, was, nwas);
	}
	if (WsRoot->get_map(ctx, root, WS_ROOT_OBJECTS, &map))
		ws_obj_list(ctx, &map, &objs, &nobjs);
	if (definitions_only)
		for (int i = 0; i < nobjs; i++)
		{
			objs[i].ncomments = 0;
			objs[i].ngrants = 0;
			objs[i].public_revoked = false;

			/* (a sequence owned by a table has its owner, here whoever merges) */
			if (objs[i].kind == WS_OBJ_SEQUENCE)
				objs[i].owner = NULL;
			if (objs[i].owner != NULL && !OidIsValid(get_role_oid(ws_name_from_utf8(objs[i].owner), true)))
			{
				objs[i].owner = NULL;
				owner_absent = true;
			}
		}
	ws_obj_restore(objs, nobjs, schema, true);

	tables_in_order(ctx, root, &tables);
	for (int t = 0; t < tables.n; t++)
	{
		int			i = tables.order[t];

		CHECK_FOR_INTERRUPTS();
		if (fn != NULL || definitions_only)
		{
			WsTableSchema bare;

			bare_record(&tables.recs[i], &bare);
			if (definitions_only)
				bare.owner = NULL;
			make_table(ctx, &bare, no_rows != NULL ? no_rows : &tables.rows[i], schema);
		}
		else
			make_table(ctx, &tables.recs[i], no_rows != NULL ? no_rows : &tables.rows[i], schema);
	}
	if (definitions_only)
	{
		/* (what a function's text may use, for all that is said of it: not what acts on tables) */
		WsObject   *usable = palloc_array(WsObject, Max(nobjs, 1));
		int			nusable = 0;

		for (int i = 0; i < nobjs; i++)
			if (objs[i].kind != WS_OBJ_TRIGGER && objs[i].kind != WS_OBJ_RULE && objs[i].kind != WS_OBJ_POLICY)
				usable[nusable++] = objs[i];
		CommandCounterIncrement();
		ws_obj_restore(usable, nusable, schema, false);
		pfree(usable);
		if (owner_absent)
			refuse_definers_with_no_owner(schema);
		goto done;
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
	if (fn != NULL)
	{
		CommandCounterIncrement();
		fn(arg, schema);
		goto done;
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

done:
	for (int i = 0; i < nobjs; i++)
		ws_obj_free(&objs[i]);
	for (int i = 0; i < tables.nnames; i++)
		ws_schema_free(&tables.recs[i]);
	buf_free(&sql);
	ws_schema_path_end(nest);
	WsSql->end(level);
}

static void
schema_restore(ProllyCtx *ctx, const PHash *root, const PHash *previous, const char *schema)
{
	restore_as(ctx, root, previous, schema, NULL, NULL, NULL, false);
}

static void
schema_restore_rows(ProllyCtx *ctx, const PHash *root, const char *schema,
					void (*fn) (void *arg, const char *schema), void *arg)
{
	restore_as(ctx, root, NULL, schema, fn, arg, NULL, false);
}

static void
schema_restore_statements(ProllyCtx *ctx, const PHash *root, const char *schema, const PHash *no_rows)
{
	restore_as(ctx, root, NULL, schema, NULL, NULL, no_rows, false);
}

static void
schema_restore_definitions(ProllyCtx *ctx, const PHash *root, const char *schema, const PHash *no_rows)
{
	restore_as(ctx, root, NULL, schema, NULL, NULL, no_rows, true);
}

const WsRootIORoutine SchemaWsRootIO = {
	.name = "schema",
	.capture = schema_capture,
	.restore = schema_restore,
	.restore_rows = schema_restore_rows,
	.restore_statements = schema_restore_statements,
	.restore_definitions = schema_restore_definitions,
};

const WsRootIORoutine *WsRootIO = &SchemaWsRootIO;

/* --------------------------------------------------------------- a dry run */

typedef struct DryRun
{
	ProllyCtx  *ctx;
	const PHash *root;
	void		(*fn) (void *arg, const char *schema);	/* restore_rows, then this */
	void	   *arg;
	const PHash *no_rows;		/* restore_statements, with this */
	const char *schema;			/* what it is named */
	bool		kept;			/* restore_definitions in a schema that stays */
	bool		went_through;
} DryRun;

/* A branch of this backend's for what is undone or deleted, on which nothing else is. */
int32
ws_root_scratch_branch(void)
{
	static int32 branch = 0;

	if (branch == 0)
		branch = BranchIds->allocate();
	return branch;
}

/*
 * A schema for a dry run, on the branch that is for them, under the name it is to have: what
 * a function's text says of its own schema then means this one, and not what is on the
 * branch whoever merges is on.  Whoever merges needs no right to make it, for it to own: it is
 * made by no statement, so that nothing a statement would fire runs for it.  All may use
 * it, as a root's owners must.  It goes when the run does.
 */
static void
make_dry_run_schema(const char *schema)
{
	WsSqlRows	rows;
	char	   *sql;

	(void) NamespaceCreateOnBranch(schema, GetUserId(), false, VersionedNamespaceBranch(schema));
	CommandCounterIncrement();
	sql = psprintf("GRANT ALL ON SCHEMA %s TO PUBLIC", quote_identifier(schema));
	WsSql->run(sql, 0, NULL, &rows);
	ws_sql_rows_free(&rows);
	pfree(sql);
}

static void
dry_run(void *arg)
{
	DryRun	   *d = arg;
	const char *schema = d->schema;
	int			nest = CatalogBranchEnter(ws_root_scratch_branch());

	make_dry_run_schema(schema);
	if (d->kept)
	{
		WsRootIO->restore_definitions(d->ctx, d->root, schema, d->no_rows);
		d->went_through = true;
		CatalogBranchLeave(nest);
		return;
	}
	if (d->fn != NULL)
		WsRootIO->restore_rows(d->ctx, d->root, schema, d->fn, d->arg);
	else if (d->no_rows != NULL)
		WsRootIO->restore_statements(d->ctx, d->root, schema, d->no_rows);
	else
		WsRootIO->restore(d->ctx, d->root, NULL, schema);
	d->went_through = true;

	/* (what makes the subtransaction undo it: the only way this one ends) */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("a dry run is undone")));
}

static bool
dry_run_refused(DryRun *dp, WsSqlFailure *failure)
{
	DryRun		d = *dp;
	MemoryContext cxt = CurrentMemoryContext;
	volatile bool kept = false;

	memset(failure, 0, sizeof(*failure));
	PG_TRY();
	{
		kept = WsSql->attempt_fn(dry_run, &d, failure);
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(cxt);
		edata = CopyErrorData();

		/*
		 * What a merge made naming what it no longer has: a view of a column
		 * dropped, a function of a type dropped.  It is the merge's to say, as is
		 * this code's every other meaning here, its message saying which: a
		 * chunk missing is no less a root that cannot be restored.  (Its
		 * subtransaction is gone already, as attempt_fn raises only after that.)
		 */
		if (edata->sqlerrcode != ERRCODE_DATA_CORRUPTED)
			PG_RE_THROW();
		FlushErrorState();
		memset(failure, 0, sizeof(*failure));
		memcpy(failure->sqlstate, unpack_sql_state(edata->sqlerrcode), 6);
		failure->message = edata->message != NULL ? pstrdup(edata->message) : NULL;
		FreeErrorData(edata);
	}
	PG_END_TRY();
	if (kept)
		return false;
	if (d.went_through)
	{
		ws_sql_failure_free(failure);
		memset(failure, 0, sizeof(*failure));
		return false;
	}
	return true;
}

bool
ws_root_refused(ProllyCtx *ctx, const PHash *root, const char *schema, WsSqlFailure *failure)
{
	DryRun		d = {.ctx = ctx, .root = root, .schema = schema};

	return dry_run_refused(&d, failure);
}

bool
ws_root_rows_refused(ProllyCtx *ctx, const PHash *root, const char *schema, void (*fn) (void *arg, const char *schema),
					 void *arg, WsSqlFailure *failure)
{
	DryRun		d = {.ctx = ctx, .root = root, .schema = schema, .fn = fn, .arg = arg};

	return dry_run_refused(&d, failure);
}

bool
ws_root_statements_refused(ProllyCtx *ctx, const PHash *root, const char *schema, WsSqlFailure *failure)
{
	PHash		no_rows;
	DryRun		d = {.ctx = ctx, .root = root, .schema = schema, .no_rows = &no_rows};

	/* (made here, as what a dry run writes it may not keep) */
	if (prolly_empty(ctx, WS_ROW_VREFS, &no_rows) < 0)
		prolly_report(ctx);
	return dry_run_refused(&d, failure);
}

bool
ws_root_definitions_begin(ProllyCtx *ctx, const PHash *root, const char *schema, WsSqlFailure *failure)
{
	PHash		no_rows;
	DryRun		d = {.ctx = ctx, .root = root, .schema = schema, .no_rows = &no_rows, .kept = true};

	if (prolly_empty(ctx, WS_ROW_VREFS, &no_rows) < 0)
		prolly_report(ctx);
	return dry_run_refused(&d, failure);
}

void
ws_root_definitions_end(const char *schema)
{
	int			nest = CatalogBranchEnter(ws_root_scratch_branch());
	ObjectAddress address;

	ObjectAddressSet(address, NamespaceRelationId, get_namespace_oid(schema, false));
	performDeletion(&address, DROP_CASCADE, PERFORM_DELETION_INTERNAL | PERFORM_DELETION_QUIETLY);
	CommandCounterIncrement();
	CatalogBranchLeave(nest);
}
