/*-------------------------------------------------------------------------
 *
 * ws_root.c
 *	  What a root value holds of a versioned schema, over the version
 *	  control layer's table entries.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/prolly/ws/ws_root.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "mb/pg_wchar.h"
#include "prolly/engine/vc.h"
#include "prolly/ws/ws_root.h"
#include "utils/builtins.h"

#define TABLE_PREFIX "t/"
#define TABLE_PREFIX_LEN 2

static const char *const map_keys[] = {
	[WS_ROOT_OBJECTS] = "~obj",
	[WS_ROOT_SEQUENCES] = "~seq",
	[WS_ROOT_VIOLATIONS] = "~viol",
	[WS_ROOT_FOREIGN_KEYS] = "~fk",
};

pg_noreturn static void
corrupt(const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("a root value %s", what)));
}

static char *
table_key(const char *name)
{
	return psprintf(TABLE_PREFIX "%s", name);
}

static void
root_must(ProllyCtx *ctx, int rc)
{
	if (rc == VC_ERR)
		elog(ERROR, "a root value's entry is not as expected");
	if (rc < 0)
		prolly_report(ctx);
}

static void
root_put_table(ProllyCtx *ctx, const PHash *rv, const WsTableSchema *s, const PHash *rows,
			   PHash *out)
{
	Buf			bytes = BUF_INIT;
	char	   *key;

	/* (raises an error if the record has a problem) */
	WsSchemaCodec->encode(s, &bytes);
	key = table_key(s->name);
	vc_table_put(ctx, rv, key, bytes.data, bytes.len, rows, out);
	pfree(key);
	buf_free(&bytes);
}

static bool
root_get_table(ProllyCtx *ctx, const PHash *rv, const char *name, WsTableSchema *s, PHash *rows)
{
	char	   *key = table_key(name);
	VcTable		t;
	uint8	   *bytes;
	size_t		len;

	if (!vc_table_get(ctx, rv, key, &t))
	{
		pfree(key);
		return false;
	}
	pfree(key);
	bytes = vc_schema_get(ctx, &t.schema, &len);
	if (bytes == NULL)
		corrupt("names a table's schema that is none");
	WsSchemaCodec->decode(bytes, len, s);
	px_free(bytes);
	/* (the record's own name: the entry could otherwise restore as another) */
	if (strcmp(s->name, name) != 0)
		corrupt("has a table's entry holding another table's record");
	*rows = t.data;
	return true;
}

static bool
root_drop_table(ProllyCtx *ctx, const PHash *rv, const char *name, PHash *out)
{
	char	   *key = table_key(name);
	VcTable		t;
	bool		found = vc_table_get(ctx, rv, key, &t);

	if (found)
		root_must(ctx, vc_table_drop(ctx, rv, key, out));
	pfree(key);
	return found;
}

static bool
is_map_key(const uint8 *key, size_t len)
{
	for (int i = 0; i < lengthof(map_keys); i++)
		if (strlen(map_keys[i]) == len && memcmp(map_keys[i], key, len) == 0)
			return true;
	return false;
}

static void
root_tables(ProllyCtx *ctx, const PHash *rv, WsRootTableFn fn, void *arg)
{
	ProllyIter *it;
	const uint8 *key;
	const uint8 *val;
	size_t		klen;
	size_t		vlen;
	int			rc;
	bool		more = true;

	root_must(ctx, prolly_iter_open(ctx, rv, NULL, 0, &it));
	while (more && (rc = prolly_iter_next(it, &key, &klen, &val, &vlen)) > 0)
	{
		char	   *name;
		size_t		nlen;
		VcTable		t;

		if (is_map_key(key, klen))
			continue;
		if (klen <= TABLE_PREFIX_LEN || memcmp(key, TABLE_PREFIX, TABLE_PREFIX_LEN) != 0)
			corrupt("has an entry that is nothing known");
		nlen = klen - TABLE_PREFIX_LEN;
		/* (as a record's name is: the name a caller is given is a name) */
		if (nlen >= NAMEDATALEN ||
			!pg_verify_mbstr(PG_UTF8, (const char *) key + TABLE_PREFIX_LEN, (int) nlen, true))
			corrupt("has a table's entry that names no table");
		vc_table_decode(val, vlen, &t);
		name = pnstrdup((const char *) key + TABLE_PREFIX_LEN, nlen);
		more = fn(arg, name, &t);
		pfree(name);
	}
	if (more)
		root_must(ctx, rc);
	prolly_iter_close(it);
}

static bool
root_get_map(ProllyCtx *ctx, const PHash *rv, WsRootMap which, PHash *map)
{
	VcTable		t;
	uint8	   *bytes;
	size_t		len;
	uint64		count;

	if (!vc_table_get(ctx, rv, map_keys[which], &t))
		return false;
	bytes = vc_schema_get(ctx, &t.schema, &len);
	if (bytes == NULL || len != 0)
		corrupt("has a map's entry with a schema");
	px_free(bytes);
	root_must(ctx, prolly_count(ctx, &t.data, &count));
	if (count == 0)
		corrupt("has a map's entry with nothing in it");
	*map = t.data;
	return true;
}

static void
root_put_map(ProllyCtx *ctx, const PHash *rv, WsRootMap which, const PHash *map, PHash *out)
{
	const char *key = map_keys[which];
	VcTable		t;
	bool		has = vc_table_get(ctx, rv, key, &t);
	uint64		count;

	root_must(ctx, prolly_count(ctx, map, &count));
	if (count == 0)
	{
		if (has)
			root_must(ctx, vc_table_drop(ctx, rv, key, out));
		else
			*out = *rv;
		return;
	}
	vc_table_put(ctx, rv, key, "", 0, map, out);
}

const WsRootRoutine MapWsRoot = {
	.name = "map",
	.put_table = root_put_table,
	.get_table = root_get_table,
	.drop_table = root_drop_table,
	.tables = root_tables,
	.get_map = root_get_map,
	.put_map = root_put_map,
};

const WsRootRoutine *WsRoot = &MapWsRoot;

void
ws_seq_encode(int64 last_value, bool is_called, Buf *out)
{
	char		num[MAXINT8LEN + 1];
	int			len = pg_lltoa(last_value, num);

	buf_append(out, num, len);
	buf_append_byte(out, is_called ? 't' : 'f');
}

void
ws_seq_decode(const uint8 *p, size_t len, int64 *last_value, bool *is_called)
{
	char		text[MAXINT8LEN + 2];
	char		back[MAXINT8LEN + 1];
	char	   *end;
	int64		value;
	int			backlen;

	if (len < 2 || len > MAXINT8LEN + 1 || (p[len - 1] != 't' && p[len - 1] != 'f'))
		corrupt("has a sequence's position that is none");
	memcpy(text, p, len - 1);
	text[len - 1] = '\0';
	errno = 0;
	value = strtoi64(text, &end, 10);
	/* (printed again, its one form: no sign, zeros or spaces it does not need) */
	backlen = (errno == 0 && *end == '\0') ? pg_lltoa(value, back) : 0;
	if (backlen != (int) len - 1 || memcmp(back, text, backlen) != 0)
		corrupt("has a sequence's position that is none");
	*last_value = value;
	*is_called = p[len - 1] == 't';
}
