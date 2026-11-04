/*-------------------------------------------------------------------------
 *
 * unit_ws_root.c
 *	  Unit tests of what a root value holds of a versioned schema: tables by
 *	  their records, maps of everything else, and sequences' positions.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/test/modules/test_prolly/unit_ws_root.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "engine_test_util.h"
#include "prolly/ws/ws_root.h"
#include "test_prolly.h"

static WsColumn cols[] = {
	{.tag = 7,.name = "id",.type = "bigint",.kind = WS_COL_STORED,.not_null = true},
	{.tag = 8,.name = "v",.type = "text",.kind = WS_COL_STORED},
};
static uint64 key_tags[] = {7};

static WsTableSchema
record_named(const char *name)
{
	WsTableSchema s = {.name = (char *) name,.ncols = 2,.cols = cols,
	.key_name = "k",.nkey = 1,.key = key_tags};

	return s;
}

static PHash
one_row_map(ProllyCtx *ctx, const char *key)
{
	ProllyKV	kv = {(const uint8 *) key, strlen(key), (const uint8 *) "v", 1};
	PHash		h;

	CHECK_EQ(prolly_build(ctx, 0, &kv, 1, &h), PROLLY_OK);
	return h;
}

/* A table's record and rows come back, and its entry's name is its own. */
ENGINE_TEST(test_tables)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &prolly_default_policy);
	PHash		rv = vc_root_empty(&ctx);
	PHash		empty_rv = rv;
	PHash		rows = one_row_map(&ctx, "a");
	WsTableSchema s = record_named("t");
	WsTableSchema changed = record_named("t");
	WsTableSchema got;
	PHash		got_rows;
	PHash		after;

	CHECK(!WsRoot->get_table(&ctx, &rv, "t", &got, &got_rows));
	WsRoot->put_table(&ctx, &rv, &s, &rows, &rv);
	CHECK(WsRoot->get_table(&ctx, &rv, "t", &got, &got_rows));
	CHECK(memcmp(got_rows.b, rows.b, PHASH_LEN) == 0);
	CHECK(strcmp(got.name, "t") == 0 && got.ncols == 2 && got.cols[0].tag == 7);
	ws_schema_free(&got);

	/* in place of what it was */
	{
		PHash		rows2 = one_row_map(&ctx, "b");

		WsRoot->put_table(&ctx, &rv, &s, &rows2, &after);
		CHECK(memcmp(after.b, rv.b, PHASH_LEN) != 0);
		WsRoot->put_table(&ctx, &after, &s, &rows, &after);
		CHECK(memcmp(after.b, rv.b, PHASH_LEN) == 0);

		/* the record too */
		changed.ncols = 1;
		WsRoot->put_table(&ctx, &rv, &changed, &rows, &after);
		CHECK(memcmp(after.b, rv.b, PHASH_LEN) != 0);
		CHECK(WsRoot->get_table(&ctx, &after, "t", &got, &got_rows));
		CHECK_EQ(got.ncols, 1);
		ws_schema_free(&got);
	}

	CHECK(!WsRoot->drop_table(&ctx, &rv, "nosuch", &after));
	CHECK(WsRoot->drop_table(&ctx, &rv, "t", &after));
	CHECK(memcmp(after.b, empty_rv.b, PHASH_LEN) == 0);
}

/* No table's name is taken for a map, however it is spelled. */
ENGINE_TEST(test_names_apart)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &prolly_default_policy);
	PHash		rv = vc_root_empty(&ctx);
	PHash		rows = one_row_map(&ctx, "row");
	PHash		objs = one_row_map(&ctx, "obj");
	WsTableSchema s = record_named("~obj");
	WsTableSchema got;
	PHash		got_rows;
	PHash		map;
	static const char *const odd[] = {
		"a/b", "/", "t/x", "caf\xc3\xa9",
		"123456789012345678901234567890123456789012345678901234567890123",
	};

	for (int i = 0; i < lengthof(odd); i++)
	{
		WsTableSchema named = record_named(odd[i]);
		PHash		with;

		WsRoot->put_table(&ctx, &rv, &named, &rows, &with);
		CHECK(WsRoot->get_table(&ctx, &with, odd[i], &got, &got_rows));
		CHECK(strcmp(got.name, odd[i]) == 0);
		ws_schema_free(&got);
	}
	WsRoot->put_table(&ctx, &rv, &s, &rows, &rv);
	WsRoot->put_map(&ctx, &rv, WS_ROOT_OBJECTS, &objs, &rv);
	CHECK(WsRoot->get_table(&ctx, &rv, "~obj", &got, &got_rows));
	CHECK(memcmp(got_rows.b, rows.b, PHASH_LEN) == 0);
	ws_schema_free(&got);
	CHECK(WsRoot->get_map(&ctx, &rv, WS_ROOT_OBJECTS, &map));
	CHECK(memcmp(map.b, objs.b, PHASH_LEN) == 0);
}

typedef struct Seen
{
	int			n;
	char		names[4][NAMEDATALEN];
	int			stop_after;
} Seen;

static bool
see_table(void *arg, const char *name, const VcTable *entry)
{
	Seen	   *seen = arg;

	strlcpy(seen->names[seen->n++], name, NAMEDATALEN);
	return seen->n != seen->stop_after;
}

/* Tables in name order, and only tables. */
ENGINE_TEST(test_listing)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &prolly_default_policy);
	PHash		rv = vc_root_empty(&ctx);
	PHash		rows = one_row_map(&ctx, "row");
	const char *names[] = {"b", "~odd", "a"};
	Seen		seen = {0};

	for (int i = 0; i < lengthof(names); i++)
	{
		WsTableSchema s = record_named(names[i]);

		WsRoot->put_table(&ctx, &rv, &s, &rows, &rv);
	}
	WsRoot->put_map(&ctx, &rv, WS_ROOT_SEQUENCES, &rows, &rv);
	WsRoot->tables(&ctx, &rv, see_table, &seen);
	CHECK_EQ(seen.n, 3);
	CHECK(strcmp(seen.names[0], "a") == 0);
	CHECK(strcmp(seen.names[1], "b") == 0);
	CHECK(strcmp(seen.names[2], "~odd") == 0);

	memset(&seen, 0, sizeof(seen));
	seen.stop_after = 1;
	WsRoot->tables(&ctx, &rv, see_table, &seen);
	CHECK_EQ(seen.n, 1);
}

/* One state, one root: an empty map has no entry, however it came to be empty. */
ENGINE_TEST(test_maps)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &prolly_default_policy);
	PHash		rv = vc_root_empty(&ctx);
	PHash		first = rv;
	PHash		map = one_row_map(&ctx, "x");
	PHash		empty;
	PHash		got;
	PHash		out;

	CHECK_EQ(prolly_empty(&ctx, 0, &empty), PROLLY_OK);
	CHECK(!WsRoot->get_map(&ctx, &rv, WS_ROOT_VIOLATIONS, &got));

	WsRoot->put_map(&ctx, &rv, WS_ROOT_VIOLATIONS, &empty, &out);
	CHECK(memcmp(out.b, first.b, PHASH_LEN) == 0);

	WsRoot->put_map(&ctx, &rv, WS_ROOT_VIOLATIONS, &map, &rv);
	CHECK(WsRoot->get_map(&ctx, &rv, WS_ROOT_VIOLATIONS, &got));
	CHECK(memcmp(got.b, map.b, PHASH_LEN) == 0);
	CHECK(!WsRoot->get_map(&ctx, &rv, WS_ROOT_OBJECTS, &got));

	WsRoot->put_map(&ctx, &rv, WS_ROOT_VIOLATIONS, &map, &out);
	CHECK(memcmp(out.b, rv.b, PHASH_LEN) == 0);
	WsRoot->put_map(&ctx, &rv, WS_ROOT_VIOLATIONS, &empty, &out);
	CHECK(memcmp(out.b, first.b, PHASH_LEN) == 0);
}

typedef struct Damage
{
	ProllyCtx  *ctx;
	PHash		rv;
	const char *name;
} Damage;

static bool
list_all(void *arg, const char *name, const VcTable *entry)
{
	return true;
}

static void
read_tables(void *arg)
{
	Damage	   *d = arg;

	WsRoot->tables(d->ctx, &d->rv, list_all, NULL);
}

static void
read_table(void *arg)
{
	Damage	   *d = arg;
	WsTableSchema s;
	PHash		rows;

	(void) WsRoot->get_table(d->ctx, &d->rv, d->name, &s, &rows);
}

static void
read_objects(void *arg)
{
	Damage	   *d = arg;
	PHash		map;

	(void) WsRoot->get_map(d->ctx, &d->rv, WS_ROOT_OBJECTS, &map);
}

/* An error's code and message: a code alone could be any failure's. */
static void
check_refused(void (*fn) (void *), void *arg, int code, const char *why)
{
	MemoryContext cxt = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	ErrorData  *edata = NULL;

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(cxt);
	PG_TRY();
	{
		fn(arg);
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(cxt);
		edata = CopyErrorData();
		FlushErrorState();
	}
	PG_END_TRY();
	RollbackAndReleaseCurrentSubTransaction();
	MemoryContextSwitchTo(cxt);
	CurrentResourceOwner = owner;
	if (edata == NULL)
		elog(ERROR, "no error, for: %s", why);
	if (edata->sqlerrcode != code || strstr(edata->message, why) == NULL)
		elog(ERROR, "not refused for %s: %s", why, edata->message);
	FreeErrorData(edata);
}

static void
put_bad_record(void *arg)
{
	Damage	   *d = arg;
	WsTableSchema s = record_named("t");
	PHash		rows;
	PHash		out;

	s.cols = NULL;
	s.ncols = 0;
	CHECK_EQ(prolly_empty(d->ctx, 0, &rows), PROLLY_OK);
	WsRoot->put_table(d->ctx, &d->rv, &s, &rows, &out);
}

/* What no writer here makes is refused: a pulled commit may hold anything. */
ENGINE_TEST(test_refused)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &prolly_default_policy);
	PHash		empty_rv = vc_root_empty(&ctx);
	PHash		rows = one_row_map(&ctx, "r");
	PHash		empty;
	Damage		d = {&ctx};
	Buf			bytes = BUF_INIT;
	WsTableSchema other = record_named("other");

	CHECK_EQ(prolly_empty(&ctx, 0, &empty), PROLLY_OK);

	/* an entry that is nothing known */
	CHECK_EQ(vc_table_create(&ctx, &empty_rv, "plain", "", 0, &d.rv), VC_OK);
	check_refused(read_tables, &d, ERRCODE_DATA_CORRUPTED, "nothing known");

	/* a table's entry naming no table: not UTF8, and too long for a name */
	CHECK_EQ(vc_table_create(&ctx, &empty_rv, "t/\xff", "", 0, &d.rv), VC_OK);
	check_refused(read_tables, &d, ERRCODE_DATA_CORRUPTED, "names no table");
	{
		char		key[NAMEDATALEN + 8];

		memset(key, 'x', sizeof(key));
		memcpy(key, "t/", 2);
		key[NAMEDATALEN + 2] = '\0';
		CHECK_EQ(vc_table_create(&ctx, &empty_rv, key, "", 0, &d.rv), VC_OK);
		check_refused(read_tables, &d, ERRCODE_DATA_CORRUPTED, "names no table");
	}

	/* a table's entry holding another table's record */
	WsSchemaCodec->encode(&other, &bytes);
	CHECK_EQ(vc_table_create(&ctx, &empty_rv, "t/t", bytes.data, bytes.len, &d.rv), VC_OK);
	d.name = "t";
	check_refused(read_table, &d, ERRCODE_DATA_CORRUPTED, "another table's record");

	/* a table's entry holding no record */
	CHECK_EQ(vc_table_create(&ctx, &empty_rv, "t/t", "junk", 4, &d.rv), VC_OK);
	expect_error(read_table, &d, ERRCODE_DATA_CORRUPTED, NULL);

	/* a map's entry with a schema */
	CHECK_EQ(vc_table_create(&ctx, &empty_rv, "~obj", "x", 1, &d.rv), VC_OK);
	CHECK_EQ(vc_table_set_data(&ctx, &d.rv, "~obj", &rows, &d.rv), VC_OK);
	check_refused(read_objects, &d, ERRCODE_DATA_CORRUPTED, "with a schema");

	/* a map's entry with nothing in it: another root for one state */
	CHECK_EQ(vc_table_create(&ctx, &empty_rv, "~obj", "", 0, &d.rv), VC_OK);
	check_refused(read_objects, &d, ERRCODE_DATA_CORRUPTED, "with nothing in it");

	/* a record with a problem is not written */
	d.rv = empty_rv;
	check_refused(put_bad_record, &d, ERRCODE_INTERNAL_ERROR, "more columns than the table has");
}

/* A read writes nothing: a root's maps, in a transaction that may not write. */
ENGINE_TEST(test_read_only)
{
	ChunkStore *cs = memstore_create();
	ProllyCtx	ctx = t_ctx(cs, &prolly_default_policy);
	PHash		rv = vc_root_empty(&ctx);
	PHash		map;
	bool		was = XactReadOnly;

	XactReadOnly = true;
	PG_TRY();
	{
		CHECK(!WsRoot->get_map(&ctx, &rv, WS_ROOT_VIOLATIONS, &map));
	}
	PG_FINALLY();
	{
		XactReadOnly = was;
	}
	PG_END_TRY();
}

static void
decode_text(void *arg)
{
	const char *text = arg;
	int64		v;
	bool		c;

	ws_seq_decode((const uint8 *) text, strlen(text), &v, &c);
}

/* A sequence's position has one set of bytes. */
static void
test_seq(void)
{
	static const struct
	{
		int64		value;
		bool		called;
		const char *bytes;
	}			good[] = {
		{1, false, "1f"},
		{42, true, "42t"},
		{-7, true, "-7t"},
		{PG_INT64_MAX, true, "9223372036854775807t"},
		{PG_INT64_MIN, false, "-9223372036854775808f"},
	};
	static const char *const bad[] = {
		"", "t", "1", "1x", "01t", "+1t", " 1t", "1 t", "-0t", "1.5t",
		"9223372036854775808t", "-9223372036854775809f", "1tt",
		"99999999999999999999999999999999999999t",
	};

	for (int i = 0; i < lengthof(good); i++)
	{
		Buf			b = BUF_INIT;
		int64		v;
		bool		c;

		ws_seq_encode(good[i].value, good[i].called, &b);
		CHECK_EQ(b.len, strlen(good[i].bytes));
		CHECK(memcmp(b.data, good[i].bytes, b.len) == 0);
		ws_seq_decode(b.data, b.len, &v, &c);
		CHECK(v == good[i].value && c == good[i].called);
		buf_free(&b);
	}
	for (int i = 0; i < lengthof(bad); i++)
		expect_error(decode_text, (void *) bad[i], ERRCODE_DATA_CORRUPTED, NULL);
}

static const ProllyUnitTest tests[] = {
	{"tables", test_tables},
	{"names_apart", test_names_apart},
	{"listing", test_listing},
	{"maps", test_maps},
	{"refused", test_refused},
	{"read_only", test_read_only},
	{"seq", test_seq},
};

const ProllyUnitSuite unit_ws_root_suite = {"ws_root", tests, lengthof(tests)};
