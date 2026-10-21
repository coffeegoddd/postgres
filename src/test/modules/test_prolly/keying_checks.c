/*--------------------------------------------------------------------------
 *
 * keying_checks.c
 *		Conformance checks for the table keying interface, and the value
 *		encoder's checks that need relations.
 *
 * These need relations, so each check creates its tables with SPI: a
 * keying's in a versioned schema, as prolly tables.  Every check does its
 * work in a subtransaction it rolls back (the suite's own subtransaction is
 * kept when a check passes), so the tables disappear afterwards.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_prolly/keying_checks.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "nodes/makefuncs.h"
#include "test_prolly.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/resowner.h"

/* The versioned schema the keying checks' tables are made in. */
#define KEYING_SCHEMA	"conf_keying"

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

/* The keying checks' versioned schema, made if it isn't there. */
static void
keying_schema(void)
{
	if (!OidIsValid(get_namespace_oid(KEYING_SCHEMA, true)))
		run_sql("CREATE SCHEMA " KEYING_SCHEMA " VERSIONED");
}

/* Create a prolly table in the keying checks' versioned schema, and open it. */
static Relation
make_table(const char *name, const char *cols)
{
	keying_schema();
	run_sql(psprintf("CREATE TABLE " KEYING_SCHEMA ".%s (%s) USING prolly",
					 name, cols));
	return table_open(RangeVarGetRelid(makeRangeVar(KEYING_SCHEMA,
													(char *) name, -1),
									   NoLock, false),
					  AccessShareLock);
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

/* Whether encode_key() gives rel's row its key as encode_row() did. */
static bool
key_alone(const ProllyKeyingRoutine *kr, ProllyKeying *k, Relation rel,
		  int id, const char *t, int n, const EncodedRow *er)
{
	TupleTableSlot *slot = row_slot(rel, id, t, n);
	StringInfoData key;
	bool		same;

	initStringInfo(&key);
	same = PROLLY_CALL(kr, encode_key, k, slot, &key) &&
		same_bytes(&key, &er->key);
	ExecDropSingleTupleTableSlot(slot);
	pfree(key.data);
	return same;
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
	Relation	tables[2];

	tables[0] = make_table("conf_key_pk",
									  "id int PRIMARY KEY, t text, n int");
	tables[1] = make_table("conf_key_none",
									  "id int, t text, n int");
	for (int r = 0; r < 2; r++)
	{
		Relation	rel = tables[r];
		ProllyKeying *k = PROLLY_CALL(kr, open, rel, CurrentMemoryContext);
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
	Relation	pk = make_table("conf_kc_pk",
										   "id int PRIMARY KEY, t text, n int");
	Relation	none = make_table("conf_kc_none",
											 "id int, t text, n int");
	ProllyKeying *kp = PROLLY_CALL(kr, open, pk, CurrentMemoryContext);
	ProllyKeying *kn = PROLLY_CALL(kr, open, none, CurrentMemoryContext);
	EncodedRow	a,
				b,
				c;
	int			ids[] = {-1000, -1, 0, 1, 2, 255, 256, 70000};

	/* Primary key: same key, different content. */
	encode(kr, kp, pk, 5, "one", 1, &a);
	encode(kr, kp, pk, 5, "two", 2, &b);
	if (!same_bytes(&a.key, &b.key))
		elog(ERROR, "rows with the same primary key got different keys");
	if (same_hash(&a.content, &b.content))
		elog(ERROR, "different rows got the same content hash");
	if (!key_alone(kr, kp, pk, 5, "two", 2, &b))
		elog(ERROR, "a row's key alone is not the key its row was encoded with");

	/* Keyless: the key is the content; identical rows share it. */
	encode(kr, kn, none, 5, "one", 1, &a);
	encode(kr, kn, none, 5, "one", 1, &b);
	encode(kr, kn, none, 5, "two", 1, &c);
	if (!same_bytes(&a.key, &b.key) || !same_hash(&a.content, &b.content))
		elog(ERROR, "identical keyless rows got different keys");
	if (same_bytes(&a.key, &c.key))
		elog(ERROR, "different keyless rows got the same key");
	{
		TupleTableSlot *slot = row_slot(none, 5, "one", 1);
		StringInfoData key;

		initStringInfo(&key);
		if (PROLLY_CALL(kr, encode_key, kn, slot, &key) || key.len != 0)
			elog(ERROR, "a row keyed by content was given a key alone");
		ExecDropSingleTupleTableSlot(slot);
	}

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
 * A keying lives in the context it was opened with, not the caller's:
 * after the caller's context is gone, it still encodes and decodes.
 */
static void
keying_owns_its_memory_body(const ProllyImplInfo *impl)
{
	const ProllyKeyingRoutine *kr = (const ProllyKeyingRoutine *) impl;
	Relation	rel = make_table("conf_kmem",
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
	k = PROLLY_CALL(kr, open, rel, owner);
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
 * relid's row shape now (the table opened only meanwhile: DDL between
 * mustn't find it in use).
 */
static StringInfo
shape_of(const ProllyKeyingRoutine *kr, Oid relid)
{
	Relation	rel = table_open(relid, AccessShareLock);
	ProllyKeying *k = PROLLY_CALL(kr, open, rel, CurrentMemoryContext);
	StringInfo	s = makeStringInfo();

	PROLLY_CALL(kr, row_shape, k, rel, s);
	PROLLY_CALL(kr, close, k);
	table_close(rel, AccessShareLock);
	return s;
}

static bool
extends(const ProllyKeyingRoutine *kr, Oid relid, StringInfo old)
{
	Relation	rel = table_open(relid, AccessShareLock);
	ProllyKeying *k = PROLLY_CALL(kr, open, rel, CurrentMemoryContext);
	ProllyValue v = {(const uint8 *) old->data, old->len};
	bool		r = PROLLY_CALL(kr, shape_extends, k, rel, &v);

	PROLLY_CALL(kr, close, k);
	table_close(rel, AccessShareLock);
	return r;
}

/*
 * A row shape is stable under DDL that changes no row (an index), and
 * changes under DDL that changes how rows encode; shape_extends() accepts
 * a change that leaves every row as it was (a nullable column added, an
 * enum's label added) and refuses the rest (a label renamed, a column
 * added with a default, one dropped).
 */
static void
keying_row_shapes_body(const ProllyImplInfo *impl)
{
	const ProllyKeyingRoutine *kr = (const ProllyKeyingRoutine *) impl;
	Relation	rel;
	Oid			relid;
	StringInfo	s0;
	StringInfo	s;

	keying_schema();
	run_sql("CREATE TYPE " KEYING_SCHEMA ".conf_mood AS ENUM ('low', 'high')");
	rel = make_table("conf_shape", "id int PRIMARY KEY, t text, n int");
	relid = RelationGetRelid(rel);
	table_close(rel, AccessShareLock);
	s0 = shape_of(kr, relid);
	run_sql("CREATE INDEX ON " KEYING_SCHEMA ".conf_shape (t)");
	s = shape_of(kr, relid);
	if (!same_bytes(s, s0) || !extends(kr, relid, s0))
		elog(ERROR, "an index changed the row shape");
	run_sql("ALTER TABLE " KEYING_SCHEMA ".conf_shape ADD COLUMN m "
			KEYING_SCHEMA ".conf_mood");
	s = shape_of(kr, relid);
	if (same_bytes(s, s0) || !extends(kr, relid, s0))
		elog(ERROR, "a nullable column added doesn't extend the row shape");
	s0 = s;
	run_sql("ALTER TYPE " KEYING_SCHEMA ".conf_mood ADD VALUE 'mid' BEFORE 'high'");
	s = shape_of(kr, relid);
	if (same_bytes(s, s0) || !extends(kr, relid, s0))
		elog(ERROR, "an enum's label added doesn't extend the row shape");
	s0 = s;
	run_sql("ALTER TYPE " KEYING_SCHEMA ".conf_mood RENAME VALUE 'low' TO 'least'");
	if (extends(kr, relid, s0))
		elog(ERROR, "an enum's label renamed extends the row shape");
	s0 = shape_of(kr, relid);
	run_sql("ALTER TABLE " KEYING_SCHEMA ".conf_shape ADD COLUMN d int DEFAULT 5");
	if (extends(kr, relid, s0))
		elog(ERROR, "a column added with a default extends the row shape");
	s0 = shape_of(kr, relid);
	run_sql("ALTER TABLE " KEYING_SCHEMA ".conf_shape DROP COLUMN n");
	if (extends(kr, relid, s0))
		elog(ERROR, "a column dropped extends the row shape");
}

/* ------------------------------------------------------------------------
 * Value encoder: rows from earlier layouts
 * ------------------------------------------------------------------------
 */

/*
 * Rows encoded before DROP COLUMN decode, with begin_layout(), into the
 * table's new shape; rows from before ADD COLUMN decode with the current
 * encoder.
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

	/* Re-encode under the new layout; then add a column with a default. */
	enc = PROLLY_CALL(ve, begin, RelationGetDescr(rel), CurrentMemoryContext);
	resetStringInfo(&row);
	PROLLY_CALL(ve, encode, enc, slot, &row);
	PROLLY_CALL(ve, end, enc);
	ExecDropSingleTupleTableSlot(slot);
	table_close(rel, AccessShareLock);
	v.data = (const uint8 *) row.data;
	v.len = row.len;

	run_sql("ALTER TABLE conf_layout ADD COLUMN d int DEFAULT 5");
	rel = table_open(relid, AccessShareLock);
	enc = PROLLY_CALL(ve, begin, RelationGetDescr(rel), CurrentMemoryContext);
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
keying_owns_its_memory(const ProllyImplInfo *impl)
{
	rolled_back(keying_owns_its_memory_body, impl);
}

void
keying_row_shapes(const ProllyImplInfo *impl)
{
	rolled_back(keying_row_shapes_body, impl);
}

void
valenc_decodes_earlier_layouts(const ProllyImplInfo *impl)
{
	rolled_back(valenc_decodes_earlier_layouts_body, impl);
}

/*
 * Run sql with its rows written as the row applier writes them, from a
 * tree a commit held: not refused for their keys' length.  (A commit with
 * a key longer than the bound, as one made before it was can hold.)
 */
PG_FUNCTION_INFO_V1(test_write_held_rows);
Datum
test_write_held_rows(PG_FUNCTION_ARGS)
{
	char	   *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
	bool		held = prolly_writing_held_rows(true);

	PG_TRY();
	{
		SPI_connect();
		if (SPI_execute(sql, false, 0) < 0)
			elog(ERROR, "could not run: %s", sql);
		SPI_finish();
	}
	PG_FINALLY();
	{
		prolly_writing_held_rows(held);
	}
	PG_END_TRY();
	PG_RETURN_VOID();
}
