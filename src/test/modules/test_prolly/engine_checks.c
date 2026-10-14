/*--------------------------------------------------------------------------
 *
 * engine_checks.c
 *		Conformance checks for the engine, commit pipeline, table keying and
 *		row identity interfaces.
 *
 * These need relations, so each check creates its tables with SPI.  Every
 * check does its work in a subtransaction it rolls back (the suite's own
 * subtransaction is kept when a check passes), so the tables and any
 * private writes disappear afterwards.  Commits can't happen inside
 * the test function's transaction; committed behaviour is tested from SQL
 * and by the TAP tests.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_prolly/engine_checks.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "common/hashfn.h"
#include "common/pg_prng.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "test_prolly.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/resowner.h"

/* ------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------
 */

static void
run_sql(const char *sql)
{
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	if (SPI_execute(sql, false, 0) < 0)
		elog(ERROR, "SPI_execute failed: %s", sql);
	SPI_finish();
	CommandCounterIncrement();
}

/* Create a table with the AM of the profile using engine, and open it. */
static Relation
make_table_for_engine(const char *engine, const char *name, const char *cols)
{
	run_sql(psprintf("CREATE TABLE %s (%s) USING prolly_%s", name, cols, engine));
	return table_open(RelnameGetRelid(name), AccessShareLock);
}

/* The engine named in a profile whose other components are impl's. */
static const char *
engine_for_impl(const ProllyImplInfo *impl)
{
	if (impl->iface == PROLLY_IFACE_ENGINE)
		return impl->name;
	return "d1";
}

static ProllyKey
int_key(int k, uint8 *buf)
{
	ProllyKey	key;

	buf[0] = (uint8) ((k >> 24) ^ 0x80);
	buf[1] = (uint8) (k >> 16);
	buf[2] = (uint8) (k >> 8);
	buf[3] = (uint8) k;
	key.data = buf;
	key.len = 4;
	return key;
}

static int
key_int(const ProllyKey *key)
{
	if (key->len != 4)
		elog(ERROR, "unexpected key length %u", key->len);
	return (int) ((((uint32) key->data[0] ^ 0x80) << 24) |
				  ((uint32) key->data[1] << 16) |
				  ((uint32) key->data[2] << 8) | key->data[3]);
}

static ProllyValue
int_value(int v, uint8 *buf)
{
	ProllyValue value;

	memcpy(buf, &v, sizeof(int));
	value.data = buf;
	value.len = sizeof(int);
	return value;
}

static int
value_int(const ProllyValue *value)
{
	int			v;

	if (value->len != sizeof(int))
		elog(ERROR, "unexpected value length %u", value->len);
	memcpy(&v, value->data, sizeof(int));
	return v;
}

/*
 * The items a cursor at cid sees, forward from the start, as (key, value)
 * pairs in keys[]/values[]; returns how many.
 */
static int
scan_ints(const ProllyEngineRoutine *e, ProllyRootSet *roots, Relation rel,
		  ProllyTreeKind kind, CommandId cid, ScanDirection dir,
		  const ProllyKey *from, int *keys, int *values, int max)
{
	ProllyCursor *c = PROLLY_CALL(e, cursor_open, roots, rel, kind, cid);
	ProllyKey	key;
	ProllyValue value;
	int			n = 0;

	PROLLY_CALL(e, cursor_seek, c, from, dir);
	while (PROLLY_CALL(e, cursor_next, c, &key, &value))
	{
		if (n >= max)
			elog(ERROR, "cursor returned more than %d items", max);
		keys[n] = key_int(&key);
		values[n] = value_int(&value);
		n++;
	}
	PROLLY_CALL(e, cursor_close, c);
	return n;
}

static void
expect_count(int got, int want, const char *what)
{
	if (got != want)
		elog(ERROR, "%s: %d items, expected %d", what, got, want);
}

/*
 * Run body in a subtransaction that is always rolled back.  On error the
 * suite's runner rolls back its own subtransaction, and this one with it.
 */
static void
rolled_back(void (*body) (const ProllyImplInfo *), const ProllyImplInfo *impl)
{
	MemoryContext cxt = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(cxt);
	body(impl);
	RollbackAndReleaseCurrentSubTransaction();
	MemoryContextSwitchTo(cxt);
	CurrentResourceOwner = owner;
}

/* ------------------------------------------------------------------------
 * Engine
 * ------------------------------------------------------------------------
 */

static void
engine_read_empty_tree_body(const ProllyImplInfo *impl)
{
	const ProllyEngineRoutine *e = (const ProllyEngineRoutine *) impl;
	Relation	rel = make_table_for_engine(impl->name, "conf_empty", "a int");
	ProllyRootSet *roots = PROLLY_CALL(e, roots_acquire, GetActiveSnapshot());
	int			keys[1];
	int			values[1];
	uint8		kb[4];
	ProllyKey	k = int_key(1, kb);
	StringInfoData v;

	expect_count(scan_ints(e, roots, rel, PROLLY_TREE_DATA, InvalidCommandId,
						   ForwardScanDirection, NULL, keys, values, 1),
				 0, "new table, forward");
	expect_count(scan_ints(e, roots, rel, PROLLY_TREE_DATA, InvalidCommandId,
						   BackwardScanDirection, NULL, keys, values, 1),
				 0, "new table, backward");
	initStringInfo(&v);
	if (PROLLY_CALL(e, get, roots, rel, PROLLY_TREE_DATA, &k, InvalidCommandId, &v))
		elog(ERROR, "get found a key in a new table");
	PROLLY_CALL(e, roots_release, roots);
	table_close(rel, AccessShareLock);
}

/*
 * A command sees the writes of earlier commands, never its own; the newest
 * earlier version of a key wins; removes hide keys.
 */
static void
engine_read_own_writes_body(const ProllyImplInfo *impl)
{
	const ProllyEngineRoutine *e = (const ProllyEngineRoutine *) impl;
	Relation	rel = make_table_for_engine(impl->name, "conf_own", "a int");
	ProllyRootSet *roots = PROLLY_CALL(e, roots_acquire, GetActiveSnapshot());
	CommandId	c = 100;
	uint8		kb[4];
	uint8		vb[4];
	ProllyKey	k;
	ProllyValue v;
	int			keys[600];
	int			values[600];
	int			n;
	pg_prng_state rng;
	bool		present[500] = {0};
	StringInfoData got;

	k = int_key(7, kb);
	v = int_value(1, vb);
	PROLLY_CALL(e, put, rel, PROLLY_TREE_DATA, &k, &v, c);
	expect_count(scan_ints(e, roots, rel, PROLLY_TREE_DATA, c,
						   ForwardScanDirection, NULL, keys, values, 600),
				 0, "a command's own put");
	n = scan_ints(e, roots, rel, PROLLY_TREE_DATA, c + 1, ForwardScanDirection,
				  NULL, keys, values, 600);
	if (n != 1 || keys[0] != 7 || values[0] != 1)
		elog(ERROR, "the next command does not see the put");

	v = int_value(2, vb);
	PROLLY_CALL(e, put, rel, PROLLY_TREE_DATA, &k, &v, c + 1);
	n = scan_ints(e, roots, rel, PROLLY_TREE_DATA, c + 1, ForwardScanDirection,
				  NULL, keys, values, 600);
	if (n != 1 || values[0] != 1)
		elog(ERROR, "a command sees its own replacement");
	n = scan_ints(e, roots, rel, PROLLY_TREE_DATA, InvalidCommandId,
				  ForwardScanDirection, NULL, keys, values, 600);
	if (n != 1 || values[0] != 2)
		elog(ERROR, "InvalidCommandId does not see the newest version");

	PROLLY_CALL(e, remove, rel, PROLLY_TREE_DATA, &k, c + 2);
	initStringInfo(&got);
	if (!PROLLY_CALL(e, get, roots, rel, PROLLY_TREE_DATA, &k, c + 2, &got) ||
		got.len != 4)
		elog(ERROR, "get at the removing command does not see the old value");
	resetStringInfo(&got);
	if (PROLLY_CALL(e, get, roots, rel, PROLLY_TREE_DATA, &k, c + 3, &got))
		elog(ERROR, "get after a remove finds the key");

	/* Many keys in random order: scans in both directions, and seeks. */
	pg_prng_seed(&rng, 4242);
	for (int i = 0; i < 2000; i++)
	{
		int			key = (int) pg_prng_uint64_range(&rng, 0, 499);

		k = int_key(key, kb);
		if (pg_prng_uint64_range(&rng, 0, 3) == 0)
		{
			PROLLY_CALL(e, remove, rel, PROLLY_TREE_DATA, &k, c + 10);
			present[key] = false;
		}
		else
		{
			v = int_value(key * 10, vb);
			PROLLY_CALL(e, put, rel, PROLLY_TREE_DATA, &k, &v, c + 10);
			present[key] = true;
		}
	}
	n = scan_ints(e, roots, rel, PROLLY_TREE_DATA, c + 11, ForwardScanDirection,
				  NULL, keys, values, 600);
	for (int key = 0, j = 0; key < 500; key++)
	{
		if (!present[key])
			continue;
		if (j >= n || keys[j] != key || values[j] != key * 10)
			elog(ERROR, "forward scan is wrong at key %d", key);
		j++;
		if (key == 499 || j == n)
			expect_count(n, j, "forward scan");
	}
	n = scan_ints(e, roots, rel, PROLLY_TREE_DATA, c + 11, BackwardScanDirection,
				  NULL, keys, values, 600);
	for (int key = 499, j = 0; key >= 0; key--)
	{
		if (!present[key])
			continue;
		if (j >= n || keys[j] != key)
			elog(ERROR, "backward scan is wrong at key %d", key);
		j++;
	}
	k = int_key(250, kb);
	n = scan_ints(e, roots, rel, PROLLY_TREE_DATA, c + 11, ForwardScanDirection,
				  &k, keys, values, 600);
	if (n > 0 && keys[0] < 250)
		elog(ERROR, "forward seek returned a key below the bound");
	for (int key = 250, j = 0; key < 500; key++)
		if (present[key] && (j >= n || keys[j++] != key))
			elog(ERROR, "forward seek is wrong at key %d", key);

	/* Local metadata trees are separate from the data tree. */
	k = int_key(100000, kb);
	v = int_value(5, vb);
	PROLLY_CALL(e, put, rel, PROLLY_TREE_ROWID, &k, &v, c + 20);
	n = scan_ints(e, roots, rel, PROLLY_TREE_ROWID, c + 21, ForwardScanDirection,
				  NULL, keys, values, 600);
	if (n != 1 || keys[0] != 100000)
		elog(ERROR, "row identity tree does not hold just its own key");
	n = scan_ints(e, roots, rel, PROLLY_TREE_ROWID, c + 21, BackwardScanDirection,
				  NULL, keys, values, 600);
	if (n != 1 || keys[0] != 100000)
		elog(ERROR, "backward scan of the row identity tree is wrong");
	resetStringInfo(&got);
	if (PROLLY_CALL(e, get, roots, rel, PROLLY_TREE_DATA, &k, InvalidCommandId, &got))
		elog(ERROR, "data tree sees a row identity key");

	PROLLY_CALL(e, roots_release, roots);
	table_close(rel, AccessShareLock);
}

/*
 * Aborting a subtransaction drops exactly its writes (and its children's),
 * restoring what earlier versions said.
 */
static void
engine_subxact_abort_discards_writes_body(const ProllyImplInfo *impl)
{
	const ProllyEngineRoutine *e = (const ProllyEngineRoutine *) impl;
	Relation	rel = make_table_for_engine(impl->name, "conf_subxact", "a int");
	ProllyRootSet *roots = PROLLY_CALL(e, roots_acquire, GetActiveSnapshot());
	MemoryContext cxt = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	uint8		kb[4];
	uint8		vb[4];
	ProllyKey	k;
	ProllyValue v;
	int			keys[400];
	int			values[400];
	int			n;

	/* Outer writes: keys 0..199 = key. */
	for (int i = 0; i < 200; i++)
	{
		k = int_key(i, kb);
		v = int_value(i, vb);
		PROLLY_CALL(e, put, rel, PROLLY_TREE_DATA, &k, &v, 1);
	}

	/*
	 * A subtransaction overwrites the even keys, removes the odd ones and
	 * adds 200..399; its child adds more.  Rolling it back must remove many
	 * keys at once.
	 */
	BeginInternalSubTransaction(NULL);
	for (int i = 0; i < 400; i++)
	{
		k = int_key(i, kb);
		if (i < 200 && i % 2 == 1)
			PROLLY_CALL(e, remove, rel, PROLLY_TREE_DATA, &k, 2);
		else
		{
			v = int_value(-i, vb);
			PROLLY_CALL(e, put, rel, PROLLY_TREE_DATA, &k, &v, 2);
		}
	}
	BeginInternalSubTransaction(NULL);
	for (int i = 400; i < 450; i++)
	{
		k = int_key(i, kb);
		v = int_value(i, vb);
		PROLLY_CALL(e, put, rel, PROLLY_TREE_DATA, &k, &v, 3);
	}
	ReleaseCurrentSubTransaction();
	RollbackAndReleaseCurrentSubTransaction();
	MemoryContextSwitchTo(cxt);
	CurrentResourceOwner = owner;

	n = scan_ints(e, roots, rel, PROLLY_TREE_DATA, InvalidCommandId,
				  ForwardScanDirection, NULL, keys, values, 400);
	expect_count(n, 200, "after rolling back the subtransaction");
	for (int i = 0; i < 200; i++)
		if (keys[i] != i || values[i] != i)
			elog(ERROR, "key %d is %d=%d after the rollback", i, keys[i], values[i]);

	PROLLY_CALL(e, roots_release, roots);
	table_close(rel, AccessShareLock);
}

static void
engine_create_and_truncate_body(const ProllyImplInfo *impl)
{
	const ProllyEngineRoutine *e = (const ProllyEngineRoutine *) impl;
	Relation	rel = make_table_for_engine(impl->name, "conf_trunc", "a int");
	ProllyRootSet *roots = PROLLY_CALL(e, roots_acquire, GetActiveSnapshot());
	uint8		kb[4];
	uint8		vb[4];
	int			keys[10];
	int			values[10];

	for (int i = 0; i < 10; i++)
	{
		ProllyKey	k = int_key(i, kb);
		ProllyValue v = int_value(i, vb);

		PROLLY_CALL(e, put, rel, PROLLY_TREE_DATA, &k, &v, 1);
		PROLLY_CALL(e, put, rel, PROLLY_TREE_ROWID, &k, &v, 1);
	}
	PROLLY_CALL(e, tree_truncate, rel);
	expect_count(scan_ints(e, roots, rel, PROLLY_TREE_DATA, InvalidCommandId,
						   ForwardScanDirection, NULL, keys, values, 10),
				 0, "data tree after truncate");
	expect_count(scan_ints(e, roots, rel, PROLLY_TREE_ROWID, InvalidCommandId,
						   ForwardScanDirection, NULL, keys, values, 10),
				 0, "row identity tree after truncate");
	PROLLY_CALL(e, roots_release, roots);
	table_close(rel, AccessShareLock);
}

static void
engine_count_range_matches_scan_body(const ProllyImplInfo *impl)
{
	const ProllyEngineRoutine *e = (const ProllyEngineRoutine *) impl;
	Relation	rel = make_table_for_engine(impl->name, "conf_count", "a int");
	ProllyRootSet *roots = PROLLY_CALL(e, roots_acquire, GetActiveSnapshot());
	uint8		kb[4];
	uint8		vb[4];
	uint64		n;

	for (int i = 0; i < 300; i++)
	{
		ProllyKey	k = int_key(i, kb);
		ProllyValue v = int_value(i, vb);

		PROLLY_CALL(e, put, rel, PROLLY_TREE_DATA, &k, &v, 1);
	}
	n = PROLLY_CALL(e, count_range, roots, rel, PROLLY_TREE_DATA, NULL, NULL);
	if (n != 300)
		elog(ERROR, "count_range says %llu, a scan finds 300",
			 (unsigned long long) n);
	PROLLY_CALL(e, roots_release, roots);
	table_close(rel, AccessShareLock);
}

/* ------------------------------------------------------------------------
 * Commit pipeline
 * ------------------------------------------------------------------------
 */

static ProllyNodeRef
fake_root(uint8 fill)
{
	ProllyNodeRef r;

	memset(&r, 0, sizeof(r));
	r.hash.len = 32;
	memset(r.hash.bytes, fill, 32);
	r.location.blkno = InvalidBlockNumber;
	return r;
}

static bool
same_root(const ProllyNodeRef *a, const ProllyNodeRef *b)
{
	return a->hash.len == b->hash.len &&
		memcmp(a->hash.bytes, b->hash.bytes, a->hash.len) == 0;
}

/*
 * A branch number no database uses: heads are never removed, so each run
 * picks a fresh one.
 */
static ProllyBranchId
scratch_branch(void)
{
	return (ProllyBranchId) (0x70000000 +
							 pg_prng_uint64_range(&pg_global_prng_state, 0, 0x0FFFFFFF));
}

static void
commit_publishes_in_enqueue_order_body(const ProllyImplInfo *impl)
{
	const ProllyCommitRoutine *cp = (const ProllyCommitRoutine *) impl;
	ProllyBranchId b = scratch_branch();
	ProllyNodeRef r1 = fake_root(0x11);
	ProllyNodeRef r2 = fake_root(0x22);
	ProllyNodeRef r3 = fake_root(0x33);
	ProllyNodeRef got;
	ProllyCommitTicket t1;
	ProllyCommitTicket t2;
	ProllyCommitTicket t3;

	PROLLY_CALL(cp, branch_lock, b);
	if (PROLLY_CALL(cp, tip, b, &got) || PROLLY_CALL(cp, head, b, &got))
		elog(ERROR, "a new branch has a root");

	t1 = PROLLY_CALL(cp, enqueue, b, &r1);
	if (!PROLLY_CALL(cp, tip, b, &got) || !same_root(&got, &r1))
		elog(ERROR, "tip is not the enqueued root");
	if (PROLLY_CALL(cp, head, b, &got))
		elog(ERROR, "an enqueued root is the head before publish");
	PROLLY_CALL(cp, branch_unlock, b);
	PROLLY_CALL(cp, publish, t1);
	if (!PROLLY_CALL(cp, head, b, &got) || !same_root(&got, &r1))
		elog(ERROR, "publish did not make the root the head");

	/* An abandoned root never becomes the head. */
	PROLLY_CALL(cp, branch_lock, b);
	t2 = PROLLY_CALL(cp, enqueue, b, &r2);
	PROLLY_CALL(cp, branch_unlock, b);
	PROLLY_CALL(cp, abandon, t2);
	if (!PROLLY_CALL(cp, head, b, &got) || !same_root(&got, &r1))
		elog(ERROR, "abandon changed the head");
	PROLLY_CALL(cp, branch_lock, b);
	if (!PROLLY_CALL(cp, tip, b, &got) || !same_root(&got, &r1))
		elog(ERROR, "tip still shows an abandoned root");

	t3 = PROLLY_CALL(cp, enqueue, b, &r3);
	PROLLY_CALL(cp, branch_unlock, b);
	PROLLY_CALL(cp, publish, t3);
	if (!PROLLY_CALL(cp, head, b, &got) || !same_root(&got, &r3))
		elog(ERROR, "the second publish did not move the head");
}

/* ------------------------------------------------------------------------
 * Table keying
 * ------------------------------------------------------------------------
 */

static TupleTableSlot *
row_slot(Relation rel, int id, const char *t, int n)
{
	TupleTableSlot *slot = MakeSingleTupleTableSlot(RelationGetDescr(rel),
													&TTSOpsVirtual);

	ExecClearTuple(slot);
	slot->tts_values[0] = Int32GetDatum(id);
	slot->tts_isnull[0] = false;
	slot->tts_values[1] = CStringGetTextDatum(t);
	slot->tts_isnull[1] = false;
	slot->tts_values[2] = Int32GetDatum(n);
	slot->tts_isnull[2] = (n < 0);
	ExecStoreVirtualTuple(slot);
	return slot;
}

typedef struct EncodedRow
{
	StringInfoData key;
	StringInfoData row;
	ProllyHash	content;
} EncodedRow;

static void
encode(const ProllyKeyingRoutine *kr, ProllyKeying *k, Relation rel, int id,
	   const char *t, int n, EncodedRow *out)
{
	TupleTableSlot *slot = row_slot(rel, id, t, n);

	initStringInfo(&out->key);
	initStringInfo(&out->row);
	PROLLY_CALL(kr, encode_row, k, slot, &out->key, &out->row, &out->content);
	ExecDropSingleTupleTableSlot(slot);
}

static bool
same_bytes(const StringInfoData *a, const StringInfoData *b)
{
	return a->len == b->len && memcmp(a->data, b->data, a->len) == 0;
}

static bool
same_hash(const ProllyHash *a, const ProllyHash *b)
{
	return a->len == b->len && memcmp(a->bytes, b->bytes, a->len) == 0;
}

static void
keying_round_trip_body(const ProllyImplInfo *impl)
{
	const ProllyKeyingRoutine *kr = (const ProllyKeyingRoutine *) impl;
	const char *engine = engine_for_impl(impl);
	Relation	tables[2];

	tables[0] = make_table_for_engine(engine, "conf_key_pk",
									  "id int PRIMARY KEY, t text, n int");
	tables[1] = make_table_for_engine(engine, "conf_key_none",
									  "id int, t text, n int");
	for (int r = 0; r < 2; r++)
	{
		Relation	rel = tables[r];
		ProllyKeying *k = PROLLY_CALL(kr, open, rel, NULL, CurrentMemoryContext);
		EncodedRow	er;
		StringInfoData value;
		ProllyValue v;
		ProllyValue row;
		uint64		count = 0;
		TupleTableSlot *out = MakeSingleTupleTableSlot(RelationGetDescr(rel),
													   &TTSOpsVirtual);

		if (PROLLY_CALL(kr, keyless, k) != (r == 1))
			elog(ERROR, "keyless() is wrong for %s", RelationGetRelationName(rel));

		encode(kr, k, rel, 42, "forty-two", -1, &er);
		row.data = (const uint8 *) er.row.data;
		row.len = er.row.len;
		initStringInfo(&value);
		PROLLY_CALL(kr, make_value, k, &row, r == 1 ? 3 : 1, &value);
		v.data = (const uint8 *) value.data;
		v.len = value.len;
		PROLLY_CALL(kr, parse_value, k, &v, &row, &count);
		if (count != (r == 1 ? 3 : 1) || row.len != er.row.len ||
			memcmp(row.data, er.row.data, row.len) != 0)
			elog(ERROR, "parse_value does not give back make_value's row and count");

		PROLLY_CALL(kr, decode_row, k, &row, out);
		slot_getallattrs(out);
		if (out->tts_isnull[0] || DatumGetInt32(out->tts_values[0]) != 42 ||
			out->tts_isnull[1] ||
			strcmp(TextDatumGetCString(out->tts_values[1]), "forty-two") != 0 ||
			!out->tts_isnull[2])
			elog(ERROR, "decode_row does not give back the row");
		ExecDropSingleTupleTableSlot(out);
		PROLLY_CALL(kr, close, k);
		table_close(rel, AccessShareLock);
	}
}

/*
 * Keys follow the key: a primary-keyed table's key depends on the key
 * columns only, a keyless table's on the whole row.  The content hash
 * always names the whole row.
 */
static void
keying_keys_and_content_body(const ProllyImplInfo *impl)
{
	const ProllyKeyingRoutine *kr = (const ProllyKeyingRoutine *) impl;
	const char *engine = engine_for_impl(impl);
	Relation	pk = make_table_for_engine(engine, "conf_kc_pk",
										   "id int PRIMARY KEY, t text, n int");
	Relation	none = make_table_for_engine(engine, "conf_kc_none",
											 "id int, t text, n int");
	ProllyKeying *kp = PROLLY_CALL(kr, open, pk, NULL, CurrentMemoryContext);
	ProllyKeying *kn = PROLLY_CALL(kr, open, none, NULL, CurrentMemoryContext);
	EncodedRow	a,
				b,
				c;
	ProllyValue row;
	ProllyHash	h;
	int			ids[] = {-1000, -1, 0, 1, 2, 255, 256, 70000};

	/* Primary key: same key, different content. */
	encode(kr, kp, pk, 5, "one", 1, &a);
	encode(kr, kp, pk, 5, "two", 2, &b);
	if (!same_bytes(&a.key, &b.key))
		elog(ERROR, "rows with the same primary key got different keys");
	if (same_hash(&a.content, &b.content))
		elog(ERROR, "different rows got the same content hash");

	/* Keyless: the key is the content; identical rows share it. */
	encode(kr, kn, none, 5, "one", 1, &a);
	encode(kr, kn, none, 5, "one", 1, &b);
	encode(kr, kn, none, 5, "two", 1, &c);
	if (!same_bytes(&a.key, &b.key) || !same_hash(&a.content, &b.content))
		elog(ERROR, "identical keyless rows got different keys");
	if (same_bytes(&a.key, &c.key))
		elog(ERROR, "different keyless rows got the same key");
	row.data = (const uint8 *) a.row.data;
	row.len = a.row.len;
	PROLLY_CALL(kr, content_hash, kn, &row, &h);
	if (!same_hash(&h, &a.content))
		elog(ERROR, "content_hash disagrees with encode_row");

	/* Primary keys sort like the key column. */
	for (int i = 1; i < lengthof(ids); i++)
	{
		ProllyKey	ka;
		ProllyKey	kb;

		encode(kr, kp, pk, ids[i - 1], "x", 0, &a);
		encode(kr, kp, pk, ids[i], "x", 0, &b);
		ka.data = (const uint8 *) a.key.data;
		ka.len = a.key.len;
		kb.data = (const uint8 *) b.key.data;
		kb.len = b.key.len;
		if (PROLLY_CALL(kr, compare, kp, &ka, &kb) >= 0)
			elog(ERROR, "primary key %d does not sort before %d",
				 ids[i - 1], ids[i]);
	}

	PROLLY_CALL(kr, close, kp);
	PROLLY_CALL(kr, close, kn);
	table_close(pk, AccessShareLock);
	table_close(none, AccessShareLock);
}

/*
 * A keying reopened from its description keys rows the same way, even on
 * a table whose own keying is now different (its primary key dropped).
 */
static void
keying_description_reopens_body(const ProllyImplInfo *impl)
{
	const ProllyKeyingRoutine *kr = (const ProllyKeyingRoutine *) impl;
	const char *engine = engine_for_impl(impl);
	Relation	pk = make_table_for_engine(engine, "conf_kd_pk",
										   "id int PRIMARY KEY, t text, n int");
	Relation	none = make_table_for_engine(engine, "conf_kd_none",
											 "id int, t text, n int");
	Relation	tables[2] = {pk, none};

	for (int from = 0; from < 2; from++)
	{
		ProllyKeying *k = PROLLY_CALL(kr, open, tables[from], NULL,
									  CurrentMemoryContext);
		StringInfoData desc;
		ProllyValue d;

		initStringInfo(&desc);
		PROLLY_CALL(kr, describe, k, &desc);
		d.data = (const uint8 *) desc.data;
		d.len = desc.len;

		/* On either table: same keyless flag, same keys and rows. */
		for (int on = 0; on < 2; on++)
		{
			ProllyKeying *k2 = PROLLY_CALL(kr, open, tables[on], &d,
										   CurrentMemoryContext);
			EncodedRow	a;
			EncodedRow	b;
			StringInfoData desc2;

			if (PROLLY_CALL(kr, keyless, k2) != PROLLY_CALL(kr, keyless, k))
				elog(ERROR, "a reopened keying changed keyless()");
			encode(kr, k, tables[from], 3, "three", 3, &a);
			encode(kr, k2, tables[on], 3, "three", 3, &b);
			if (!same_bytes(&a.key, &b.key) || !same_bytes(&a.row, &b.row))
				elog(ERROR, "a reopened keying keys rows differently");
			initStringInfo(&desc2);
			PROLLY_CALL(kr, describe, k2, &desc2);
			if (!same_bytes(&desc, &desc2))
				elog(ERROR, "a reopened keying describes itself differently");
			PROLLY_CALL(kr, close, k2);
		}
		PROLLY_CALL(kr, close, k);
	}

	{
		ProllyKeying *a = PROLLY_CALL(kr, open, pk, NULL, CurrentMemoryContext);
		ProllyKeying *b = PROLLY_CALL(kr, open, none, NULL, CurrentMemoryContext);
		StringInfoData da;
		StringInfoData db;

		initStringInfo(&da);
		initStringInfo(&db);
		PROLLY_CALL(kr, describe, a, &da);
		PROLLY_CALL(kr, describe, b, &db);
		if (same_bytes(&da, &db))
			elog(ERROR, "different keyings have the same description");
	}
	table_close(pk, AccessShareLock);
	table_close(none, AccessShareLock);
}

/*
 * A keying reopened from its description after its key column was
 * dropped still orders the old keys and decodes the old rows.
 */
static void
keying_reads_dropped_key_body(const ProllyImplInfo *impl)
{
	const ProllyKeyingRoutine *kr = (const ProllyKeyingRoutine *) impl;
	const char *engine = engine_for_impl(impl);
	Relation	rel = make_table_for_engine(engine, "conf_kdrop",
											"id int PRIMARY KEY, t text, n int");
	Oid			relid = RelationGetRelid(rel);
	ProllyKeying *k = PROLLY_CALL(kr, open, rel, NULL, CurrentMemoryContext);
	StringInfoData desc;
	ProllyValue d;
	EncodedRow	a;
	EncodedRow	b;
	ProllyKey	ka;
	ProllyKey	kb;
	ProllyValue row;
	TupleTableSlot *slot;

	initStringInfo(&desc);
	PROLLY_CALL(kr, describe, k, &desc);
	d.data = (const uint8 *) desc.data;
	d.len = desc.len;
	encode(kr, k, rel, 1, "one", 1, &a);
	encode(kr, k, rel, 2, "two", 2, &b);
	PROLLY_CALL(kr, close, k);
	table_close(rel, AccessShareLock);

	run_sql("ALTER TABLE conf_kdrop DROP COLUMN id CASCADE");
	rel = table_open(relid, AccessShareLock);
	k = PROLLY_CALL(kr, open, rel, &d, CurrentMemoryContext);
	ka.data = (const uint8 *) a.key.data;
	ka.len = a.key.len;
	kb.data = (const uint8 *) b.key.data;
	kb.len = b.key.len;
	if (PROLLY_CALL(kr, compare, k, &ka, &kb) >= 0 ||
		PROLLY_CALL(kr, compare, k, &kb, &ka) <= 0 ||
		PROLLY_CALL(kr, compare, k, &ka, &ka) != 0)
		elog(ERROR, "a keying reopened after its key column was dropped misorders keys");

	slot = MakeSingleTupleTableSlot(RelationGetDescr(rel), &TTSOpsVirtual);
	row.data = (const uint8 *) b.row.data;
	row.len = b.row.len;
	PROLLY_CALL(kr, decode_row, k, &row, slot);
	slot_getallattrs(slot);
	if (slot->tts_isnull[1] || slot->tts_isnull[2] ||
		strcmp(TextDatumGetCString(slot->tts_values[1]), "two") != 0 ||
		DatumGetInt32(slot->tts_values[2]) != 2)
		elog(ERROR, "a keying reopened after its key column was dropped misreads rows");
	ExecDropSingleTupleTableSlot(slot);
	PROLLY_CALL(kr, close, k);
	table_close(rel, AccessShareLock);
}

/* ------------------------------------------------------------------------
 * Row identity
 * ------------------------------------------------------------------------
 */

static ProllyHash
content_of(int i)
{
	ProllyHash	h;

	memset(&h, 0, sizeof(h));
	h.len = 32;
	for (int b = 0; b < 32; b++)
		h.bytes[b] = (uint8) (i * 31 + b);
	return h;
}

static void
check_valid_tid(ItemPointer tid)
{
	if (!BlockNumberIsValid(ItemPointerGetBlockNumber(tid)) ||
		ItemPointerGetOffsetNumber(tid) < 1 ||
		ItemPointerGetOffsetNumber(tid) > MaxHeapTuplesPerPage)
		elog(ERROR, "TID (%u,%u) is not a valid heap-style item pointer",
			 ItemPointerGetBlockNumber(tid), ItemPointerGetOffsetNumber(tid));
}

static void
rowid_assign_then_resolve_body(const ProllyImplInfo *impl)
{
	const ProllyRowIdRoutine *ri = (const ProllyRowIdRoutine *) impl;
	Relation	rel = make_table_for_engine(engine_for_impl(impl), "conf_rowid", "a int");
	ProllyComponents c;
	ProllyRootSet *roots;
	ItemPointerData tids[200];
	ProllyRowIdScan *scan;

	prolly_components_for_relation(rel, &c);
	roots = PROLLY_CALL(c.engine, roots_acquire, GetActiveSnapshot());
	for (int i = 0; i < 200; i++)
	{
		ProllyHash	h = content_of(i / 2);
		uint8		kb[4];
		ProllyKey	key = int_key(i, kb);

		/* Pairs of identical rows: ordinals 0 and 1. */
		PROLLY_CALL(ri, tid_assign, roots, rel, &h, i % 2, &key, 1, &tids[i]);
		check_valid_tid(&tids[i]);
		for (int j = 0; j < i; j++)
			if (ItemPointerEquals(&tids[i], &tids[j]))
				elog(ERROR, "versions %d and %d got the same TID", i, j);
	}

	scan = PROLLY_CALL(ri, scan_begin, roots, rel, 2);
	for (int i = 0; i < 200; i++)
	{
		ProllyHash	h = content_of(i / 2);
		ProllyHash	got_h;
		uint64		got_ord;
		ItemPointerData tid;
		StringInfoData key;

		if (!PROLLY_CALL(ri, tid_find, scan, &h, i % 2, &tid) ||
			!ItemPointerEquals(&tid, &tids[i]))
			elog(ERROR, "tid_find disagrees with tid_assign for version %d", i);

		initStringInfo(&key);
		if (!PROLLY_CALL(ri, tid_resolve, roots, rel, &tids[i], 2, &key,
						 &got_h, &got_ord))
			elog(ERROR, "tid_resolve does not know an assigned TID");
		if (!same_hash(&got_h, &h) || got_ord != (uint64) (i % 2) ||
			key.len != 4 || key_int(&(ProllyKey) {(const uint8 *) key.data, 4}) != i)
			elog(ERROR, "tid_resolve returned the wrong version for %d", i);
	}
	PROLLY_CALL(ri, scan_end, scan);
	PROLLY_CALL(c.engine, roots_release, roots);
	table_close(rel, AccessShareLock);
}

static void
rowid_same_version_same_tid_body(const ProllyImplInfo *impl)
{
	const ProllyRowIdRoutine *ri = (const ProllyRowIdRoutine *) impl;
	Relation	rel = make_table_for_engine(engine_for_impl(impl), "conf_rowid2", "a int");
	ProllyComponents c;
	ProllyRootSet *roots;
	ProllyHash	h = content_of(9);
	uint8		kb[4];
	ProllyKey	k1 = int_key(1, kb);
	ItemPointerData t1;
	ItemPointerData t2;
	StringInfoData key;
	ProllyHash	got_h;
	uint64		got_ord;

	prolly_components_for_relation(rel, &c);
	roots = PROLLY_CALL(c.engine, roots_acquire, GetActiveSnapshot());
	PROLLY_CALL(ri, tid_assign, roots, rel, &h, 0, &k1, 1, &t1);

	/* Same version, now under another key (as after a re-keying). */
	{
		uint8		kb2[4];
		ProllyKey	k2 = int_key(2, kb2);

		PROLLY_CALL(ri, tid_assign, roots, rel, &h, 0, &k2, 2, &t2);
	}
	if (!ItemPointerEquals(&t1, &t2))
		elog(ERROR, "the same version got a second TID");
	initStringInfo(&key);
	if (!PROLLY_CALL(ri, tid_resolve, roots, rel, &t1, 3, &key, &got_h, &got_ord) ||
		key_int(&(ProllyKey) {(const uint8 *) key.data, key.len}) != 2)
		elog(ERROR, "the TID does not resolve to the version's current key");
	PROLLY_CALL(c.engine, roots_release, roots);
	table_close(rel, AccessShareLock);
}

/* ------------------------------------------------------------------------
 * The "hashed" row identity's collision path (white box)
 * ------------------------------------------------------------------------
 */

/* The hashed implementation's first-choice TID; must match rowid_hashed.c. */
static void
hashed_candidate(const ProllyHash *content, uint64 ordinal, int probe,
				 ItemPointer tid)
{
	uint64		h = hash_bytes_extended(content->bytes, content->len,
										hash_combine64(ordinal, (uint64) probe));

	ItemPointerSet(tid, (BlockNumber) ((h >> 32) % (uint64) InvalidBlockNumber),
				   (OffsetNumber) (1 + (uint32) h % MaxHeapTuplesPerPage));
}

/*
 * A version whose first-choice TID is taken by another version gets a
 * different TID, keeps it, and scans find it.  The collision is made by
 * writing a 'T' entry for another version at the first-choice TID.
 */
static void
rowid_hashed_displaced_version_body(const ProllyImplInfo *impl)
{
	const ProllyRowIdRoutine *ri = (const ProllyRowIdRoutine *) impl;
	Relation	rel;
	ProllyComponents c;
	ProllyRootSet *roots;
	ProllyHash	mine = content_of(1);
	ProllyHash	other = content_of(2);
	ItemPointerData first;
	ItemPointerData got;
	ItemPointerData again;
	ProllyRowIdScan *scan;
	uint8		tk[7];
	StringInfoData entry;
	uint8		kb[4];
	ProllyKey	key = int_key(1, kb);
	ProllyKey	tkey;
	ProllyValue tval;

	if (strcmp(impl->name, "hashed") != 0)
		return;
	rel = make_table_for_engine("d1", "conf_rowid_hashed", "a int");
	prolly_components_for_relation(rel, &c);
	roots = PROLLY_CALL(c.engine, roots_acquire, GetActiveSnapshot());

	/* Another version holds my first choice. */
	hashed_candidate(&mine, 0, 0, &first);
	tk[0] = 'T';
	tk[1] = (uint8) (ItemPointerGetBlockNumber(&first) >> 24);
	tk[2] = (uint8) (ItemPointerGetBlockNumber(&first) >> 16);
	tk[3] = (uint8) (ItemPointerGetBlockNumber(&first) >> 8);
	tk[4] = (uint8) ItemPointerGetBlockNumber(&first);
	tk[5] = (uint8) (ItemPointerGetOffsetNumber(&first) >> 8);
	tk[6] = (uint8) ItemPointerGetOffsetNumber(&first);
	tkey.data = tk;
	tkey.len = 7;
	initStringInfo(&entry);
	appendStringInfoChar(&entry, (char) other.len);
	appendBinaryStringInfo(&entry, (const char *) other.bytes, other.len);
	appendStringInfoChar(&entry, 0);	/* ordinal 0 */
	appendBinaryStringInfo(&entry, (const char *) key.data, key.len);
	tval.data = (const uint8 *) entry.data;
	tval.len = entry.len;
	PROLLY_CALL(c.engine, put, rel, PROLLY_TREE_ROWID, &tkey, &tval, 1);

	PROLLY_CALL(ri, tid_assign, roots, rel, &mine, 0, &key, 2, &got);
	check_valid_tid(&got);
	if (ItemPointerEquals(&got, &first))
		elog(ERROR, "a displaced version took a TID held by another version");

	/* It keeps its TID, and scans find it. */
	PROLLY_CALL(ri, tid_assign, roots, rel, &mine, 0, &key, 3, &again);
	if (!ItemPointerEquals(&got, &again))
		elog(ERROR, "a displaced version got a different TID the second time");
	scan = PROLLY_CALL(ri, scan_begin, roots, rel, 4);
	if (!PROLLY_CALL(ri, tid_find, scan, &mine, 0, &again) ||
		!ItemPointerEquals(&got, &again))
		elog(ERROR, "a scan does not find the displaced version's TID");
	PROLLY_CALL(ri, scan_end, scan);
	PROLLY_CALL(c.engine, roots_release, roots);
	table_close(rel, AccessShareLock);
}

/* ------------------------------------------------------------------------
 * Value encoder: rows from earlier layouts
 * ------------------------------------------------------------------------
 */

/*
 * Rows encoded before DROP COLUMN decode, with begin_layout(), into the
 * table's new shape; rows from before ADD COLUMN decode with the current
 * encoder whenever reads_layout() says so.
 */
static void
valenc_decodes_earlier_layouts_body(const ProllyImplInfo *impl)
{
	const ProllyValueEncoderRoutine *ve = (const ProllyValueEncoderRoutine *) impl;
	Oid			relid;
	Relation	rel;
	ProllyValueEncoder *enc;
	TupleTableSlot *slot;
	StringInfoData row;
	StringInfoData layout;
	ProllyValue v;
	ProllyValue l;

	run_sql("CREATE TABLE conf_layout (a int, b text, c int)");
	relid = RelnameGetRelid("conf_layout");

	/* Encode (1, 'gone', 3) under the first layout. */
	rel = table_open(relid, AccessShareLock);
	enc = PROLLY_CALL(ve, begin, RelationGetDescr(rel), CurrentMemoryContext);
	slot = MakeSingleTupleTableSlot(RelationGetDescr(rel), &TTSOpsVirtual);
	ExecClearTuple(slot);
	slot->tts_values[0] = Int32GetDatum(1);
	slot->tts_isnull[0] = false;
	slot->tts_values[1] = CStringGetTextDatum("gone");
	slot->tts_isnull[1] = false;
	slot->tts_values[2] = Int32GetDatum(3);
	slot->tts_isnull[2] = false;
	ExecStoreVirtualTuple(slot);
	initStringInfo(&row);
	initStringInfo(&layout);
	PROLLY_CALL(ve, encode, enc, slot, &row);
	PROLLY_CALL(ve, layout, enc, &layout);
	PROLLY_CALL(ve, end, enc);
	ExecDropSingleTupleTableSlot(slot);
	table_close(rel, AccessShareLock);
	v.data = (const uint8 *) row.data;
	v.len = row.len;
	l.data = (const uint8 *) layout.data;
	l.len = layout.len;

	/* Drop b; the old row decodes as (1, 3), whatever the dropped b holds. */
	run_sql("ALTER TABLE conf_layout DROP COLUMN b");
	rel = table_open(relid, AccessShareLock);
	enc = PROLLY_CALL(ve, begin_layout, RelationGetDescr(rel), &l,
					  CurrentMemoryContext);
	slot = MakeSingleTupleTableSlot(RelationGetDescr(rel), &TTSOpsVirtual);
	PROLLY_CALL(ve, decode, enc, &v, slot);
	slot_getallattrs(slot);
	if (slot->tts_isnull[0] || DatumGetInt32(slot->tts_values[0]) != 1 ||
		slot->tts_isnull[2] || DatumGetInt32(slot->tts_values[2]) != 3)
		elog(ERROR, "a row encoded before DROP COLUMN decodes wrongly");
	PROLLY_CALL(ve, end, enc);

	/* If the current encoder claims to read the old layout, it must. */
	enc = PROLLY_CALL(ve, begin, RelationGetDescr(rel), CurrentMemoryContext);
	if (PROLLY_CALL(ve, reads_layout, enc, &l))
	{
		PROLLY_CALL(ve, decode, enc, &v, slot);
		slot_getallattrs(slot);
		if (slot->tts_isnull[2] || DatumGetInt32(slot->tts_values[2]) != 3)
			elog(ERROR, "reads_layout() is true, but the row decodes wrongly");
	}

	/* Re-encode under the new layout; then add a column with a default. */
	resetStringInfo(&row);
	resetStringInfo(&layout);
	PROLLY_CALL(ve, encode, enc, slot, &row);
	PROLLY_CALL(ve, layout, enc, &layout);
	PROLLY_CALL(ve, end, enc);
	ExecDropSingleTupleTableSlot(slot);
	table_close(rel, AccessShareLock);
	v.data = (const uint8 *) row.data;
	v.len = row.len;
	l.data = (const uint8 *) layout.data;
	l.len = layout.len;

	run_sql("ALTER TABLE conf_layout ADD COLUMN d int DEFAULT 5");
	rel = table_open(relid, AccessShareLock);
	enc = PROLLY_CALL(ve, begin, RelationGetDescr(rel), CurrentMemoryContext);
	if (!PROLLY_CALL(ve, reads_layout, enc, &l))
		elog(ERROR, "adding a column made earlier rows unreadable");
	slot = MakeSingleTupleTableSlot(RelationGetDescr(rel), &TTSOpsVirtual);
	PROLLY_CALL(ve, decode, enc, &v, slot);
	slot_getallattrs(slot);
	if (DatumGetInt32(slot->tts_values[0]) != 1 ||
		DatumGetInt32(slot->tts_values[2]) != 3 ||
		slot->tts_isnull[3] || DatumGetInt32(slot->tts_values[3]) != 5)
		elog(ERROR, "a row encoded before ADD COLUMN decodes wrongly");
	PROLLY_CALL(ve, end, enc);
	ExecDropSingleTupleTableSlot(slot);
	table_close(rel, AccessShareLock);
}

/* ------------------------------------------------------------------------
 * Entry points
 * ------------------------------------------------------------------------
 */

void
engine_read_empty_tree(const ProllyImplInfo *impl)
{
	rolled_back(engine_read_empty_tree_body, impl);
}

void
engine_read_own_writes(const ProllyImplInfo *impl)
{
	rolled_back(engine_read_own_writes_body, impl);
}

void
engine_subxact_abort_discards_writes(const ProllyImplInfo *impl)
{
	rolled_back(engine_subxact_abort_discards_writes_body, impl);
}

void
engine_create_and_truncate(const ProllyImplInfo *impl)
{
	rolled_back(engine_create_and_truncate_body, impl);
}

void
engine_count_range_matches_scan(const ProllyImplInfo *impl)
{
	rolled_back(engine_count_range_matches_scan_body, impl);
}

void
commit_publishes_in_enqueue_order(const ProllyImplInfo *impl)
{
	rolled_back(commit_publishes_in_enqueue_order_body, impl);
}

void
keying_round_trip(const ProllyImplInfo *impl)
{
	rolled_back(keying_round_trip_body, impl);
}

void
keying_keys_and_content(const ProllyImplInfo *impl)
{
	rolled_back(keying_keys_and_content_body, impl);
}

void
rowid_assign_then_resolve(const ProllyImplInfo *impl)
{
	rolled_back(rowid_assign_then_resolve_body, impl);
}

void
rowid_same_version_same_tid(const ProllyImplInfo *impl)
{
	rolled_back(rowid_same_version_same_tid_body, impl);
}

void
rowid_hashed_displaced_version(const ProllyImplInfo *impl)
{
	rolled_back(rowid_hashed_displaced_version_body, impl);
}

void
keying_description_reopens(const ProllyImplInfo *impl)
{
	rolled_back(keying_description_reopens_body, impl);
}

void
keying_reads_dropped_key(const ProllyImplInfo *impl)
{
	rolled_back(keying_reads_dropped_key_body, impl);
}

void
valenc_decodes_earlier_layouts(const ProllyImplInfo *impl)
{
	rolled_back(valenc_decodes_earlier_layouts_body, impl);
}
