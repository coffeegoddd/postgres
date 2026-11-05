/*-------------------------------------------------------------------------
 *
 * ws_print.c
 *	  What a restore printed of what a commit holds, in pg_ws_print
 *	  (prolly/ws/ws_print.h).
 *
 * src/backend/prolly/ws/ws_print.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "common/cryptohash.h"
#include "common/sha2.h"
#include "catalog/namespace.h"
#include "catalog/pg_ws_print.h"
#include "prolly/ws/ws_print.h"
#include "prolly/ws/ws_schema.h"
#include "prolly/ws/ws_sql.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"

static bytea *
key_hash(const char *key)
{
	pg_cryptohash_ctx *ctx = pg_cryptohash_create(PG_SHA256);
	bytea	   *hash = palloc(VARHDRSZ + PG_SHA256_DIGEST_LENGTH);

	SET_VARSIZE(hash, VARHDRSZ + PG_SHA256_DIGEST_LENGTH);
	if (pg_cryptohash_init(ctx) < 0 ||
		pg_cryptohash_update(ctx, (const uint8 *) key, strlen(key)) < 0 ||
		pg_cryptohash_final(ctx, (uint8 *) VARDATA(hash), PG_SHA256_DIGEST_LENGTH) < 0)
		elog(ERROR, "could not compute a key's hash: %s", pg_cryptohash_error(ctx));
	pg_cryptohash_free(ctx);
	return hash;
}

static WsPrintEntry *
catalog_load(const char *schema, const char *prefix, int *n)
{
	const char *params[2] = {schema, ws_text_from_utf8(prefix)};
	WsSqlRows	rows;
	WsPrintEntry *entries;

	WsSql->read("SELECT p.prtkey, p.prtprinted, p.prtheld FROM pg_ws_print p JOIN pg_namespace n ON n.oid = p.prtnamespace "
				"WHERE n.nspname = $1::name AND starts_with(p.prtkey, $2)", 2, params, &rows);
	entries = palloc0_array(WsPrintEntry, Max(rows.nrows, 1));
	for (int i = 0; i < rows.nrows; i++)
	{
		entries[i].key = ws_text_to_utf8(ws_sql_value(&rows, i, 0));
		entries[i].printed = ws_text_to_utf8(ws_sql_value(&rows, i, 1));
		entries[i].held = ws_text_to_utf8(ws_sql_value(&rows, i, 2));
	}
	*n = rows.nrows;
	ws_sql_rows_free(&rows);
	return entries;
}

static void
catalog_remember(const char *schema, const char *key, const char *printed, const char *held)
{
	Relation	rel = table_open(WsPrintRelationId, RowExclusiveLock);
	Datum		values[Natts_pg_ws_print];
	bool		nulls[Natts_pg_ws_print] = {false};
	HeapTuple	tup;

	values[Anum_pg_ws_print_prtnamespace - 1] = ObjectIdGetDatum(get_namespace_oid(schema, false));
	values[Anum_pg_ws_print_prtkeyhash - 1] = PointerGetDatum(key_hash(key));
	values[Anum_pg_ws_print_prtkey - 1] = CStringGetTextDatum(ws_text_from_utf8(key));
	values[Anum_pg_ws_print_prtprinted - 1] = CStringGetTextDatum(ws_text_from_utf8(printed));
	values[Anum_pg_ws_print_prtheld - 1] = CStringGetTextDatum(ws_text_from_utf8(held));
	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);
	table_close(rel, RowExclusiveLock);
}

static void
catalog_forget(const char *schema, const char *key)
{
	Relation	rel = table_open(WsPrintRelationId, RowExclusiveLock);
	ScanKeyData keys[2];
	SysScanDesc scan;
	HeapTuple	tup;

	ScanKeyInit(&keys[0], Anum_pg_ws_print_prtnamespace, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(get_namespace_oid(schema, false)));
	ScanKeyInit(&keys[1], Anum_pg_ws_print_prtkeyhash, BTEqualStrategyNumber, F_BYTEAEQ,
				PointerGetDatum(key_hash(key)));
	scan = systable_beginscan(rel, WsPrintNamespaceKeyHashIndexId, true, NULL, 2, keys);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
		CatalogTupleDelete(rel, &tup->t_self);
	systable_endscan(scan);
	table_close(rel, RowExclusiveLock);
}

static void
catalog_forget_schema(Oid nspoid)
{
	Relation	rel = table_open(WsPrintRelationId, RowExclusiveLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;

	ScanKeyInit(&key, Anum_pg_ws_print_prtnamespace, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(nspoid));
	scan = systable_beginscan(rel, WsPrintNamespaceKeyHashIndexId, true, NULL, 1, &key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
		CatalogTupleDelete(rel, &tup->t_self);
	systable_endscan(scan);
	table_close(rel, RowExclusiveLock);
}

static const WsPrintStore catalog_store = {
	.load = catalog_load,
	.remember = catalog_remember,
	.forget = catalog_forget,
	.forget_schema = catalog_forget_schema,
};

const WsPrintStore *const WsPrints = &catalog_store;
