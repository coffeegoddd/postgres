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

#include "access/genam.h"
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
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"

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
	PG_TRY();
	{
		body(impl);
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		/* Pass the error on, without this subtransaction. */
		MemoryContextSwitchTo(cxt);
		edata = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(cxt);
		CurrentResourceOwner = owner;
		ReThrowError(edata);
	}
	PG_END_TRY();
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

/* The key a seek to key then a step finds, or -1 if none. */
static int
seek_one(const ProllyEngineRoutine *e, ProllyCursor *cur, int key)
{
	uint8		kb[4];
	ProllyKey	k = int_key(key, kb);
	ProllyKey	got;
	ProllyValue v;

	PROLLY_CALL(e, cursor_seek, cur, &k, ForwardScanDirection);
	if (!PROLLY_CALL(e, cursor_next, cur, &got, &v))
		return -1;
	return key_int(&got);
}

/*
 * A seek sees the writes made before it, as get does, even when they come
 * after the cursor was opened: the tree's first write, more writes, and a
 * data tree being emptied to be rebuilt.  rel's trees start empty, or rel
 * holds committed rows (keys of other forms than the check's), which a
 * cursor reads before the data tree is emptied.
 */
static void
seek_sees_later_writes(const ProllyEngineRoutine *e, Relation rel)
{
	ProllyRootSet *roots = PROLLY_CALL(e, roots_acquire, GetActiveSnapshot());
	ProllyTreeKind kinds[] = {PROLLY_TREE_DATA, PROLLY_TREE_ROWID};
	uint8		kb[4];
	uint8		vb[4];

	for (int t = 0; t < lengthof(kinds); t++)
	{
		ProllyCursor *cur = PROLLY_CALL(e, cursor_open, roots, rel, kinds[t],
										InvalidCommandId);
		ProllyKey	k;
		ProllyValue v = int_value(1, vb);
		ProllyKey	got;
		ProllyValue gotv;

		PROLLY_CALL(e, cursor_seek, cur, NULL, ForwardScanDirection);
		if (kinds[t] == PROLLY_TREE_DATA &&
			PROLLY_CALL(e, cursor_next, cur, &got, &gotv))
		{
			/* Committed rows, one read: emptying the tree hides them. */
			PROLLY_CALL(e, tree_reset, rel, kinds[t]);
			PROLLY_CALL(e, cursor_seek, cur, NULL, ForwardScanDirection);
			if (PROLLY_CALL(e, cursor_next, cur, &got, &gotv))
				elog(ERROR, "a seek sees committed rows of a tree emptied after it read them");
		}
		if (seek_one(e, cur, 0) != -1)
			elog(ERROR, "the tree is not empty");
		k = int_key(5, kb);
		PROLLY_CALL(e, put, rel, kinds[t], &k, &v, 1);
		if (seek_one(e, cur, 0) != 5)
			elog(ERROR, "a seek does not see the tree's first write");
		k = int_key(3, kb);
		PROLLY_CALL(e, put, rel, kinds[t], &k, &v, 1);
		if (seek_one(e, cur, 0) != 3)
			elog(ERROR, "a seek does not see a later write");
		if (kinds[t] == PROLLY_TREE_DATA)
		{
			PROLLY_CALL(e, tree_reset, rel, kinds[t]);
			if (seek_one(e, cur, 0) != -1)
				elog(ERROR, "a seek sees rows of a tree emptied after it opened");
			k = int_key(9, kb);
			PROLLY_CALL(e, put, rel, kinds[t], &k, &v, 1);
			if (seek_one(e, cur, 0) != 9)
				elog(ERROR, "a seek does not see a write to an emptied tree");
		}
		PROLLY_CALL(e, cursor_close, cur);
	}
	PROLLY_CALL(e, roots_release, roots);
}

static void
engine_seek_sees_later_writes_body(const ProllyImplInfo *impl)
{
	Relation	rel = make_table_for_engine(impl->name, "conf_seek", "a int");

	seek_sees_later_writes((const ProllyEngineRoutine *) impl, rel);
	table_close(rel, AccessShareLock);
}

/*
 * prolly_seek_sees_later_writes(rel): the same, on a table the transaction
 * hasn't written yet (made by an earlier transaction), whose cursors open
 * before it has any writes.  The writes are rolled back.
 */
static Relation seek_rel;

static void
seek_sees_later_writes_on_rel(const ProllyImplInfo *impl)
{
	seek_sees_later_writes((const ProllyEngineRoutine *) impl, seek_rel);
}

PG_FUNCTION_INFO_V1(prolly_seek_sees_later_writes);

Datum
prolly_seek_sees_later_writes(PG_FUNCTION_ARGS)
{
	ProllyComponents c;

	seek_rel = table_open(PG_GETARG_OID(0), RowExclusiveLock);
	prolly_components_for_relation(seek_rel, &c);
	rolled_back(seek_sees_later_writes_on_rel, &c.engine->info);
	table_close(seek_rel, RowExclusiveLock);
	PG_RETURN_VOID();
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

	/*
	 * This transaction hasn't committed, so a snapshot sees none of its
	 * roots, published or not: the branch had no root before them.
	 */
	if (PROLLY_CALL(cp, snapshot_head, b, GetActiveSnapshot(), &got))
		elog(ERROR, "a snapshot sees a root of a transaction still running");
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

/*
 * A keying lives in the context it was opened with, not the caller's:
 * after the caller's context is gone, it still encodes and decodes.
 */
static void
keying_owns_its_memory_body(const ProllyImplInfo *impl)
{
	const ProllyKeyingRoutine *kr = (const ProllyKeyingRoutine *) impl;
	Relation	rel = make_table_for_engine(engine_for_impl(impl), "conf_kmem",
											"id int PRIMARY KEY, t text, n int");
	MemoryContext owner = AllocSetContextCreate(CurrentMemoryContext,
												"keying owner",
												ALLOCSET_DEFAULT_SIZES);
	MemoryContext caller = AllocSetContextCreate(CurrentMemoryContext,
												 "keying caller",
												 ALLOCSET_DEFAULT_SIZES);
	MemoryContext old;
	ProllyKeying *k;
	EncodedRow	er;
	ProllyValue row;
	TupleTableSlot *slot;
	MemoryContextCounters before;
	MemoryContextCounters after;

	/* Nothing the keying keeps is left in the caller's memory. */
	memset(&before, 0, sizeof(before));
	memset(&after, 0, sizeof(after));
	MemoryContextMemConsumed(caller, &before);
	old = MemoryContextSwitchTo(caller);
	k = PROLLY_CALL(kr, open, rel, NULL, owner);
	MemoryContextSwitchTo(old);
	MemoryContextMemConsumed(caller, &after);
	if (after.totalspace - after.freespace != before.totalspace - before.freespace)
		elog(ERROR, "opening a keying left %zu bytes in the caller's memory",
			 (after.totalspace - after.freespace) -
			 (before.totalspace - before.freespace));
	MemoryContextDelete(caller);

	encode(kr, k, rel, 7, "seven", 7, &er);
	row.data = (const uint8 *) er.row.data;
	row.len = er.row.len;
	slot = MakeSingleTupleTableSlot(RelationGetDescr(rel), &TTSOpsVirtual);
	PROLLY_CALL(kr, decode_row, k, &row, slot);
	slot_getallattrs(slot);
	if (slot->tts_isnull[0] || DatumGetInt32(slot->tts_values[0]) != 7 ||
		strcmp(TextDatumGetCString(slot->tts_values[1]), "seven") != 0)
		elog(ERROR, "a keying misreads rows once its caller's memory is gone");
	ExecDropSingleTupleTableSlot(slot);
	PROLLY_CALL(kr, close, k);
	MemoryContextDelete(owner);
	table_close(rel, AccessShareLock);
}

/*
 * can_compare() holds while the key's type exists, and fails for a keying
 * reopened from its description after the type was dropped.
 */
static void
keying_compare_after_type_dropped_body(const ProllyImplInfo *impl)
{
	const ProllyKeyingRoutine *kr = (const ProllyKeyingRoutine *) impl;
	const char *engine = engine_for_impl(impl);
	Relation	rel;
	Relation	none;
	Oid			relid;
	ProllyKeying *k;
	StringInfoData desc;
	ProllyValue d;

	run_sql("CREATE TYPE conf_kt_mood AS ENUM ('sad', 'happy')");
	rel = make_table_for_engine(engine, "conf_kt",
								"m conf_kt_mood PRIMARY KEY, t text");
	none = make_table_for_engine(engine, "conf_kt_none", "id int, t text");
	relid = RelationGetRelid(rel);

	k = PROLLY_CALL(kr, open, none, NULL, CurrentMemoryContext);
	if (!PROLLY_CALL(kr, can_compare, k))
		elog(ERROR, "a keying without a primary key can't compare");
	PROLLY_CALL(kr, close, k);
	table_close(none, AccessShareLock);

	k = PROLLY_CALL(kr, open, rel, NULL, CurrentMemoryContext);
	if (!PROLLY_CALL(kr, can_compare, k))
		elog(ERROR, "a keying can't compare while its key's type exists");
	initStringInfo(&desc);
	PROLLY_CALL(kr, describe, k, &desc);
	d.data = (const uint8 *) desc.data;
	d.len = desc.len;
	PROLLY_CALL(kr, close, k);
	table_close(rel, AccessShareLock);

	run_sql("DROP TYPE conf_kt_mood CASCADE");
	rel = table_open(relid, AccessShareLock);
	k = PROLLY_CALL(kr, open, rel, &d, CurrentMemoryContext);
	if (PROLLY_CALL(kr, can_compare, k))
		elog(ERROR, "a keying claims to compare keys of a dropped type");
	PROLLY_CALL(kr, close, k);
	table_close(rel, AccessShareLock);
}

/* ------------------------------------------------------------------------
 * Index keying
 * ------------------------------------------------------------------------
 */

typedef struct IxCase
{
	int			a;				/* -1: NULL */
	const char *b;				/* NULL: NULL */
	const char *rowkey;
	uint64		ordinal;
	StringInfoData entry;
} IxCase;

/* The order of an index on (a, b DESC): NULLs last; then row key, ordinal. */
static int
ix_reference(const IxCase *x, const IxCase *y)
{
	int			r;

	if ((x->a < 0) != (y->a < 0))
		return x->a < 0 ? 1 : -1;
	if (x->a != y->a)
		return x->a < y->a ? -1 : 1;
	if ((x->b == NULL) != (y->b == NULL))
		return x->b == NULL ? -1 : 1;	/* DESC: NULLS FIRST */
	if (x->b && (r = strcmp(x->b, y->b)) != 0)
		return r > 0 ? -1 : 1;
	if ((r = strcmp(x->rowkey, y->rowkey)) != 0)
		return r < 0 ? -1 : 1;
	return (x->ordinal > y->ordinal) - (x->ordinal < y->ordinal);
}

static void
ix_values(const IxCase *x, Datum *values, bool *isnull)
{
	isnull[0] = x->a < 0;
	values[0] = Int32GetDatum(x->a);
	isnull[1] = x->b == NULL;
	values[1] = x->b ? CStringGetTextDatum(x->b) : (Datum) 0;
}

/*
 * Entries round-trip and sort as their index orders them, then by row key
 * and ordinal; bounds sort before (lower) or after (upper) exactly the
 * entries whose first columns equal theirs; a reopened keying is the same.
 */
static void
ixkeying_entries_body(const ProllyImplInfo *impl)
{
	const ProllyIndexKeyingRoutine *kr = (const ProllyIndexKeyingRoutine *) impl;
	const char *engine = engine_for_impl(impl);
	Relation	heap;
	Relation	index;
	ProllyIndexKeying *k;
	ProllyIndexKeying *k2;
	StringInfoData desc;
	ProllyValue d;
	IxCase		cases[64];
	int			n = 0;
	int			as[] = {-1, 1, 2};
	const char *bs[] = {NULL, "x", "y"};
	const char *rks[] = {"r1", "r2"};

	run_sql(psprintf("CREATE TABLE conf_ix (a int, b text COLLATE \"C\") USING prolly_%s",
					 engine));
	run_sql("CREATE INDEX conf_ix_ab ON conf_ix USING prolly (a, b DESC)");
	heap = table_open(RelnameGetRelid("conf_ix"), AccessShareLock);
	index = index_open(RelnameGetRelid("conf_ix_ab"), AccessShareLock);
	k = PROLLY_CALL(kr, open, index, NULL, CurrentMemoryContext);
	if (PROLLY_CALL(kr, nkeys, k) != 2)
		elog(ERROR, "an index keying on two columns has %d", PROLLY_CALL(kr, nkeys, k));
	initStringInfo(&desc);
	PROLLY_CALL(kr, describe, k, &desc);
	d.data = (const uint8 *) desc.data;
	d.len = desc.len;
	k2 = PROLLY_CALL(kr, open, index, &d, CurrentMemoryContext);

	for (int i = 0; i < lengthof(as); i++)
		for (int j = 0; j < lengthof(bs); j++)
			for (int r = 0; r < lengthof(rks); r++)
				for (uint64 o = 0; o < 2; o++)
				{
					IxCase	   *x = &cases[n++];
					Datum		values[2];
					bool		isnull[2];
					ProllyKey	rk = {(const uint8 *) rks[r], 2};

					x->a = as[i];
					x->b = bs[j];
					x->rowkey = rks[r];
					x->ordinal = o;
					initStringInfo(&x->entry);
					ix_values(x, values, isnull);
					PROLLY_CALL(kr, encode_entry, k, values, isnull, &rk, o, &x->entry);
				}

	for (int i = 0; i < n; i++)
	{
		ProllyKey	ei = {(const uint8 *) cases[i].entry.data, cases[i].entry.len};
		Datum		values[2];
		bool		isnull[2];
		ProllyKey	rk;
		uint64		ord;
		Datum		bv[2];
		bool		bn[2];

		/* Round trip. */
		PROLLY_CALL(kr, decode_entry, k, &ei, values, isnull, &rk, &ord);
		if (isnull[0] != (cases[i].a < 0) ||
			(!isnull[0] && DatumGetInt32(values[0]) != cases[i].a) ||
			isnull[1] != (cases[i].b == NULL) ||
			(!isnull[1] && strcmp(TextDatumGetCString(values[1]), cases[i].b) != 0) ||
			rk.len != 2 || memcmp(rk.data, cases[i].rowkey, 2) != 0 ||
			ord != cases[i].ordinal)
			elog(ERROR, "an index entry did not decode to what it was made from");

		for (int j = 0; j < n; j++)
		{
			ProllyKey	ej = {(const uint8 *) cases[j].entry.data, cases[j].entry.len};
			int			got = PROLLY_CALL(kr, compare, k, &ei, &ej);
			int			got2 = PROLLY_CALL(kr, compare, k2, &ei, &ej);
			int			want = ix_reference(&cases[i], &cases[j]);
			bool		eq1 = (cases[i].a == cases[j].a);
			bool		eq2 = eq1 && ((cases[i].b == NULL && cases[j].b == NULL) ||
									  (cases[i].b && cases[j].b &&
									   strcmp(cases[i].b, cases[j].b) == 0));

			got = got < 0 ? -1 : (got > 0 ? 1 : 0);
			got2 = got2 < 0 ? -1 : (got2 > 0 ? 1 : 0);
			if (got != want || got2 != want)
				elog(ERROR, "index entries %d and %d compare %d (reopened %d), should be %d",
					 i, j, got, got2, want);
			if (PROLLY_CALL(kr, columns_equal, k, &ei, &ej, 1) != eq1 ||
				PROLLY_CALL(kr, columns_equal, k, &ei, &ej, 2) != eq2)
				elog(ERROR, "columns_equal is wrong for index entries %d and %d", i, j);
		}

		/* Bounds on the first column, and on both. */
		ix_values(&cases[i], bv, bn);
		for (int nk = 1; nk <= 2; nk++)
		{
			StringInfoData lo;
			StringInfoData hi;
			ProllyKey	l;
			ProllyKey	h;

			initStringInfo(&lo);
			initStringInfo(&hi);
			PROLLY_CALL(kr, encode_bound, k, nk, bv, bn, false, &lo);
			PROLLY_CALL(kr, encode_bound, k, nk, bv, bn, true, &hi);
			l.data = (const uint8 *) lo.data;
			l.len = lo.len;
			h.data = (const uint8 *) hi.data;
			h.len = hi.len;
			for (int j = 0; j < n; j++)
			{
				ProllyKey	ej = {(const uint8 *) cases[j].entry.data, cases[j].entry.len};
				bool		same = PROLLY_CALL(kr, columns_equal, k, &ei, &ej, nk);
				int			cl = PROLLY_CALL(kr, compare, k, &l, &ej);
				int			ch = PROLLY_CALL(kr, compare, k, &h, &ej);
				int			want = ix_reference(&cases[i], &cases[j]);

				if (same ? !(cl < 0 && ch > 0) :
					(want < 0 ? !(cl < 0 && ch < 0) : !(cl > 0 && ch > 0)))
					elog(ERROR, "bounds on %d columns of entry %d misplace entry %d",
						 nk, i, j);
			}
		}
	}
	PROLLY_CALL(kr, close, k);
	PROLLY_CALL(kr, close, k2);
	index_close(index, AccessShareLock);
	table_close(heap, AccessShareLock);
}

void
ixkeying_entries(const ProllyImplInfo *impl)
{
	rolled_back(ixkeying_entries_body, impl);
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
		ProllyHash	h = content_of(i);
		uint8		kb[4];
		ProllyKey	key = int_key(i / 2, kb);

		/* Pairs of identical rows at one key: ordinals 0 and 1. */
		PROLLY_CALL(ri, tid_assign, roots, rel, &h, i % 2, &key, 1, &tids[i]);
		check_valid_tid(&tids[i]);
		for (int j = 0; j < i; j++)
			if (ItemPointerEquals(&tids[i], &tids[j]))
				elog(ERROR, "versions %d and %d got the same TID", i, j);
	}

	scan = PROLLY_CALL(ri, scan_begin, roots, rel, 2);
	for (int i = 0; i < 200; i++)
	{
		ProllyHash	h = content_of(i);
		uint8		kb[4];
		ProllyKey	k = int_key(i / 2, kb);
		ProllyHash	got_h;
		uint64		got_ord;
		ItemPointerData tid;
		StringInfoData key;
		ProllyTidInfo info;

		if (!PROLLY_CALL(ri, tid_find, scan, &k, i % 2, &tid) ||
			!ItemPointerEquals(&tid, &tids[i]))
			elog(ERROR, "tid_find disagrees with tid_assign for version %d", i);

		initStringInfo(&key);
		if (PROLLY_CALL(ri, tid_resolve, roots, rel, &tids[i], 2, &key,
						&got_h, &got_ord, &info) != PROLLY_TID_NAMED)
			elog(ERROR, "tid_resolve does not know an assigned TID");
		if (!same_hash(&got_h, &h) || got_ord != (uint64) (i % 2) ||
			key.len != 4 || key_int(&(ProllyKey) {(const uint8 *) key.data, 4}) != i / 2)
			elog(ERROR, "tid_resolve returned the wrong version for %d", i);
		if (!TransactionIdIsCurrentTransactionId(info.xmin))
			elog(ERROR, "a version's xmin is not the transaction that wrote it");
	}
	PROLLY_CALL(ri, scan_end, scan);
	PROLLY_CALL(c.engine, roots_release, roots);
	table_close(rel, AccessShareLock);
}

/*
 * A version renamed (re-encoded, re-keyed) keeps its TID, which now
 * resolves to its new key and content; assigning the version again gives
 * the same TID.
 */
static void
rowid_rename_keeps_tid_body(const ProllyImplInfo *impl)
{
	const ProllyRowIdRoutine *ri = (const ProllyRowIdRoutine *) impl;
	Relation	rel = make_table_for_engine(engine_for_impl(impl), "conf_rowid2", "a int");
	ProllyComponents c;
	ProllyRootSet *roots;
	ProllyHash	h1 = content_of(9);
	ProllyHash	h2 = content_of(10);
	uint8		kb1[4];
	uint8		kb2[4];
	ProllyKey	k1 = int_key(1, kb1);
	ProllyKey	k2 = int_key(2, kb2);
	ItemPointerData t1;
	ItemPointerData t2;
	StringInfoData key;
	ProllyHash	got_h;
	uint64		got_ord;
	ProllyTidInfo info;
	ProllyRowIdScan *scan;

	prolly_components_for_relation(rel, &c);
	roots = PROLLY_CALL(c.engine, roots_acquire, GetActiveSnapshot());
	PROLLY_CALL(ri, tid_assign, roots, rel, &h1, 0, &k1, 1, &t1);
	PROLLY_CALL(ri, tid_rename, roots, rel, &t1, &h2, 0, &k2, 2);

	initStringInfo(&key);
	if (PROLLY_CALL(ri, tid_resolve, roots, rel, &t1, 3, &key, &got_h, &got_ord,
					&info) != PROLLY_TID_NAMED ||
		key_int(&(ProllyKey) {(const uint8 *) key.data, key.len}) != 2 ||
		!same_hash(&got_h, &h2))
		elog(ERROR, "a renamed TID does not resolve to the new version");
	scan = PROLLY_CALL(ri, scan_begin, roots, rel, 3);
	if (!PROLLY_CALL(ri, tid_find, scan, &k2, 0, &t2) || !ItemPointerEquals(&t1, &t2))
		elog(ERROR, "tid_find does not find a renamed version's TID");
	PROLLY_CALL(ri, scan_end, scan);
	PROLLY_CALL(ri, tid_assign, roots, rel, &h2, 0, &k2, 3, &t2);
	if (!ItemPointerEquals(&t1, &t2))
		elog(ERROR, "the same version got a second TID");
	PROLLY_CALL(c.engine, roots_release, roots);
	table_close(rel, AccessShareLock);
}

/*
 * A TID is never given to a second version: not after the subtransaction
 * that got it aborts (index entries made for it would survive the abort),
 * and not after its version is retired.  A retired TID resolves as gone,
 * with its successor.
 */
static void
rowid_never_reused_body(const ProllyImplInfo *impl)
{
	const ProllyRowIdRoutine *ri = (const ProllyRowIdRoutine *) impl;
	Relation	rel = make_table_for_engine(engine_for_impl(impl), "conf_rowid3", "a int");
	ProllyComponents c;
	ProllyRootSet *roots;
	ProllyHash	h = content_of(5);
	ProllyHash	h2 = content_of(6);
	uint8		kb[4];
	uint8		kb2[4];
	ProllyKey	k = int_key(5, kb);
	ProllyKey	k2 = int_key(6, kb2);
	ItemPointerData aborted;
	ItemPointerData t1;
	ItemPointerData t2;
	ItemPointerData t3;
	StringInfoData key;
	ProllyHash	got_h;
	uint64		got_ord;
	ProllyTidInfo info;
	MemoryContext cxt = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	ProllyRowIdScan *scan;

	prolly_components_for_relation(rel, &c);

	/* Assigned in a subtransaction that aborts... */
	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(cxt);
	roots = PROLLY_CALL(c.engine, roots_acquire, GetActiveSnapshot());
	PROLLY_CALL(ri, tid_assign, roots, rel, &h, 0, &k, 1, &aborted);
	PROLLY_CALL(c.engine, roots_release, roots);
	RollbackAndReleaseCurrentSubTransaction();
	MemoryContextSwitchTo(cxt);
	CurrentResourceOwner = owner;

	/* ...the same version again gets another TID. */
	roots = PROLLY_CALL(c.engine, roots_acquire, GetActiveSnapshot());
	PROLLY_CALL(ri, tid_assign, roots, rel, &h, 0, &k, 2, &t1);
	if (ItemPointerEquals(&t1, &aborted))
		elog(ERROR, "a TID was given out again after its transaction aborted");

	/* Retired with a successor: gone, and followable. */
	PROLLY_CALL(ri, tid_assign, roots, rel, &h2, 0, &k2, 3, &t2);
	{
		ProllyValue row = {(const uint8 *) "old row", 7};

		PROLLY_CALL(ri, tid_retire, roots, rel, &t1, &t2, &row, 3);
	}
	initStringInfo(&key);
	if (PROLLY_CALL(ri, tid_resolve, roots, rel, &t1, 4, &key, &got_h, &got_ord,
					&info) != PROLLY_TID_GONE ||
		!ItemPointerEquals(&info.successor, &t2) || info.cid != 3)
		elog(ERROR, "a retired TID does not resolve to its successor");
	if (key.len != 7 || memcmp(key.data, "old row", 7) != 0)
		elog(ERROR, "a retired TID does not keep its version's row");
	scan = PROLLY_CALL(ri, scan_begin, roots, rel, 4);
	if (PROLLY_CALL(ri, tid_find, scan, &k, 0, &t3))
		elog(ERROR, "tid_find still finds a retired version");
	PROLLY_CALL(ri, scan_end, scan);

	/* The same version written again gets a new TID. */
	PROLLY_CALL(ri, tid_assign, roots, rel, &h, 0, &k, 4, &t3);
	if (ItemPointerEquals(&t3, &t1) || ItemPointerEquals(&t3, &aborted))
		elog(ERROR, "a retired TID was given out again");
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
engine_seek_sees_later_writes(const ProllyImplInfo *impl)
{
	rolled_back(engine_seek_sees_later_writes_body, impl);
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
rowid_rename_keeps_tid(const ProllyImplInfo *impl)
{
	rolled_back(rowid_rename_keeps_tid_body, impl);
}

/*
 * A scan's tid_find sees versions assigned after the scan began, when its
 * command can see them: one scan, finding as it goes.
 */
static void
rowid_find_sees_later_assign_body(const ProllyImplInfo *impl)
{
	const ProllyRowIdRoutine *ri = (const ProllyRowIdRoutine *) impl;
	Relation	rel = make_table_for_engine(engine_for_impl(impl), "conf_rowid5", "a int");
	ProllyComponents c;
	ProllyRootSet *roots;
	ProllyRowIdScan *scan;

	prolly_components_for_relation(rel, &c);
	roots = PROLLY_CALL(c.engine, roots_acquire, GetActiveSnapshot());
	scan = PROLLY_CALL(ri, scan_begin, roots, rel, InvalidCommandId);
	for (int i = 0; i < 20; i++)
	{
		ProllyHash	h = content_of(i);
		uint8		kb[4];
		ProllyKey	k = int_key(i, kb);
		ItemPointerData assigned;
		ItemPointerData found;

		if (PROLLY_CALL(ri, tid_find, scan, &k, 0, &found))
			elog(ERROR, "tid_find finds version %d before it was assigned", i);
		PROLLY_CALL(ri, tid_assign, roots, rel, &h, 0, &k, 1, &assigned);
		if (!PROLLY_CALL(ri, tid_find, scan, &k, 0, &found) ||
			!ItemPointerEquals(&found, &assigned))
			elog(ERROR, "tid_find does not see version %d, assigned during the scan", i);
	}
	PROLLY_CALL(ri, scan_end, scan);
	PROLLY_CALL(c.engine, roots_release, roots);
	table_close(rel, AccessShareLock);
}

void
rowid_find_sees_later_assign(const ProllyImplInfo *impl)
{
	rolled_back(rowid_find_sees_later_assign_body, impl);
}

/*
 * Renewing gives the version a new TID that the version's key finds, and
 * retires the old one with the new one as its successor, keeping the row.
 */
static void
rowid_renew_body(const ProllyImplInfo *impl)
{
	const ProllyRowIdRoutine *ri = (const ProllyRowIdRoutine *) impl;
	Relation	rel = make_table_for_engine(engine_for_impl(impl), "conf_rowid4", "a int");
	ProllyComponents c;
	ProllyRootSet *roots;
	ProllyHash	h = content_of(3);
	uint8		kb[4];
	ProllyKey	k = int_key(3, kb);
	ProllyValue row = {(const uint8 *) "row", 3};
	ItemPointerData t1;
	ItemPointerData t2;
	ItemPointerData t3;
	StringInfoData out;
	ProllyHash	got_h;
	uint64		got_ord;
	ProllyTidInfo info;
	ProllyRowIdScan *scan;

	prolly_components_for_relation(rel, &c);
	roots = PROLLY_CALL(c.engine, roots_acquire, GetActiveSnapshot());
	PROLLY_CALL(ri, tid_assign, roots, rel, &h, 0, &k, 1, &t1);
	PROLLY_CALL(ri, tid_renew, roots, rel, &t1, &row, 2, &t2);
	if (ItemPointerEquals(&t1, &t2))
		elog(ERROR, "a renewed version kept its TID");
	initStringInfo(&out);
	if (PROLLY_CALL(ri, tid_resolve, roots, rel, &t1, 3, &out, &got_h, &got_ord,
					&info) != PROLLY_TID_GONE ||
		!ItemPointerEquals(&info.successor, &t2) || info.cid != 2 ||
		out.len != 3 || memcmp(out.data, "row", 3) != 0)
		elog(ERROR, "a renewed TID does not resolve to its successor and row");
	resetStringInfo(&out);
	if (PROLLY_CALL(ri, tid_resolve, roots, rel, &t2, 3, &out, &got_h, &got_ord,
					&info) != PROLLY_TID_NAMED || !same_hash(&got_h, &h))
		elog(ERROR, "a renewed version's new TID does not name it");
	scan = PROLLY_CALL(ri, scan_begin, roots, rel, 3);
	if (!PROLLY_CALL(ri, tid_find, scan, &k, 0, &t3) || !ItemPointerEquals(&t2, &t3))
		elog(ERROR, "tid_find does not find a renewed version's new TID");
	PROLLY_CALL(ri, scan_end, scan);
	PROLLY_CALL(c.engine, roots_release, roots);
	table_close(rel, AccessShareLock);
}

void
rowid_renew(const ProllyImplInfo *impl)
{
	rolled_back(rowid_renew_body, impl);
}

void
rowid_never_reused(const ProllyImplInfo *impl)
{
	rolled_back(rowid_never_reused_body, impl);
}

void
keying_description_reopens(const ProllyImplInfo *impl)
{
	rolled_back(keying_description_reopens_body, impl);
}

void
keying_owns_its_memory(const ProllyImplInfo *impl)
{
	rolled_back(keying_owns_its_memory_body, impl);
}

void
keying_compare_after_type_dropped(const ProllyImplInfo *impl)
{
	rolled_back(keying_compare_after_type_dropped_body, impl);
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
