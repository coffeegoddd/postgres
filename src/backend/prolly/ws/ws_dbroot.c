/*-------------------------------------------------------------------------
 *
 * ws_dbroot.c
 *	  What a branch's root value holds, over the version control layer's
 *	  table entries.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/prolly/ws/ws_dbroot.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "mb/pg_wchar.h"
#include "prolly/branch_catalog.h"
#include "prolly/engine/vc.h"
#include "prolly/ws/ws_dbroot.h"
#include "prolly/ws/ws_restore.h"
#include "prolly/ws/ws_schema.h"
#include "prolly/ws/ws_sql.h"
#include "utils/builtins.h"

#define SCHEMA_PREFIX "s/"
#define SCHEMA_PREFIX_LEN 2

pg_noreturn static void
corrupt(const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("a branch's root value %s", what)));
}

static char *
schema_key(const char *name)
{
	return psprintf(SCHEMA_PREFIX "%s", name);
}

static void
dbroot_must(ProllyCtx *ctx, int rc)
{
	if (rc == VC_ERR)
		elog(ERROR, "a branch's root value's entry is not as expected");
	if (rc < 0)
		prolly_report(ctx);
}

/* (a schema's entry: an empty schema chunk, its data the schema's root) */
static void
entry_check(ProllyCtx *ctx, const VcTable *t)
{
	uint8	   *bytes;
	size_t		len;

	bytes = vc_schema_get(ctx, &t->schema, &len);
	if (bytes == NULL || len != 0)
		corrupt("has a schema's entry with a schema chunk");
	px_free(bytes);
}

static void
dbroot_put_schema(ProllyCtx *ctx, const PHash *rv, const char *name, const PHash *root, PHash *out)
{
	char	   *key = schema_key(name);

	vc_table_put(ctx, rv, key, "", 0, root, out);
	pfree(key);
}

static bool
dbroot_get_schema(ProllyCtx *ctx, const PHash *rv, const char *name, PHash *root)
{
	char	   *key = schema_key(name);
	VcTable		t;
	bool		found = vc_table_get(ctx, rv, key, &t);

	pfree(key);
	if (!found)
		return false;
	entry_check(ctx, &t);
	*root = t.data;
	return true;
}

static bool
dbroot_drop_schema(ProllyCtx *ctx, const PHash *rv, const char *name, PHash *out)
{
	char	   *key = schema_key(name);
	VcTable		t;
	bool		found = vc_table_get(ctx, rv, key, &t);

	if (found)
		dbroot_must(ctx, vc_table_drop(ctx, rv, key, out));
	pfree(key);
	return found;
}

static void
dbroot_schemas(ProllyCtx *ctx, const PHash *rv, WsDbRootSchemaFn fn, void *arg)
{
	ProllyIter *it;
	const uint8 *key;
	const uint8 *val;
	size_t		klen;
	size_t		vlen;
	int			rc;
	bool		more = true;

	dbroot_must(ctx, prolly_iter_open(ctx, rv, NULL, 0, &it));
	while (more && (rc = prolly_iter_next(it, &key, &klen, &val, &vlen)) > 0)
	{
		char	   *name;
		size_t		nlen;
		VcTable		t;

		if (klen <= SCHEMA_PREFIX_LEN || memcmp(key, SCHEMA_PREFIX, SCHEMA_PREFIX_LEN) != 0)
			corrupt("has an entry that is nothing known");
		nlen = klen - SCHEMA_PREFIX_LEN;
		/* (as a schema's name is: the name a caller is given is a name) */
		if (nlen >= NAMEDATALEN || memchr(key + SCHEMA_PREFIX_LEN, '\0', nlen) != NULL ||
			!pg_verify_mbstr(PG_UTF8, (const char *) key + SCHEMA_PREFIX_LEN, (int) nlen, true))
			corrupt("has a schema's entry that names no schema");
		vc_table_decode(val, vlen, &t);
		entry_check(ctx, &t);
		name = pnstrdup((const char *) key + SCHEMA_PREFIX_LEN, nlen);
		more = fn(arg, name, &t.data);
		pfree(name);
	}
	if (more)
		dbroot_must(ctx, rc);
	prolly_iter_close(it);
}

const WsDbRootRoutine MapWsDbRoot = {
	.name = "map",
	.put_schema = dbroot_put_schema,
	.get_schema = dbroot_get_schema,
	.drop_schema = dbroot_drop_schema,
	.schemas = dbroot_schemas,
};

const WsDbRootRoutine *WsDbRoot = &MapWsDbRoot;

typedef struct DbRestore
{
	ProllyCtx  *ctx;
} DbRestore;

static bool
restore_one(void *arg, const char *name, const PHash *root)
{
	DbRestore  *r = arg;
	char	   *schema = ws_name_from_utf8(name);
	WsSqlRows	rows;
	int			level = WsSql->begin();

	/* (made on the branch, which the caller is on, versioned, owned by the caller) */
	WsSql->run(psprintf("CREATE SCHEMA %s VERSIONED", quote_identifier(schema)), 0, NULL, &rows);
	ws_sql_rows_free(&rows);
	WsSql->end(level);
	WsRootIO->restore(r->ctx, root, NULL, schema);
	pfree(schema);
	return true;
}

void
ws_dbroot_restore(ProllyCtx *ctx, const PHash *rv, int32 branch)
{
	DbRestore	r = {ctx};
	int			level = CatalogBranchEnter(branch);

	WsDbRoot->schemas(ctx, rv, restore_one, &r);
	CatalogBranchLeave(level);
}
