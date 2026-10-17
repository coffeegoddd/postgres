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
	Relation	pk = make_table("conf_kc_pk",
										   "id int PRIMARY KEY, t text, n int");
	Relation	none = make_table("conf_kc_none",
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
	Relation	pk = make_table("conf_kd_pk",
										   "id int PRIMARY KEY, t text, n int");
	Relation	none = make_table("conf_kd_none",
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
	Relation	rel = make_table("conf_kdrop",
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

	run_sql("ALTER TABLE " KEYING_SCHEMA ".conf_kdrop DROP COLUMN id CASCADE");
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
	Relation	rel;
	Relation	none;
	Oid			relid;
	ProllyKeying *k;
	StringInfoData desc;
	ProllyValue d;

	keying_schema();
	run_sql("CREATE TYPE " KEYING_SCHEMA ".conf_kt_mood AS ENUM ('sad', 'happy')");
	rel = make_table("conf_kt",
								"m " KEYING_SCHEMA ".conf_kt_mood PRIMARY KEY, t text");
	none = make_table("conf_kt_none", "id int, t text");
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

	run_sql("DROP TYPE " KEYING_SCHEMA ".conf_kt_mood CASCADE");
	rel = table_open(relid, AccessShareLock);
	k = PROLLY_CALL(kr, open, rel, &d, CurrentMemoryContext);
	if (PROLLY_CALL(kr, can_compare, k))
		elog(ERROR, "a keying claims to compare keys of a dropped type");
	PROLLY_CALL(kr, close, k);
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
valenc_decodes_earlier_layouts(const ProllyImplInfo *impl)
{
	rolled_back(valenc_decodes_earlier_layouts_body, impl);
}
