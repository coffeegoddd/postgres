/*-------------------------------------------------------------------------
 *
 * prolly_extconfig.c
 *	  Extensions' configuration rows, as commits record them
 *	  (prolly_extconfig.h).
 *
 * An element is one of:
 *
 *	config<tab><table><tab><column>=<value>,...
 *		a row of a configuration table that its extcondition selects: each
 *		of its stored columns (not generated ones), in column order, by
 *		name, its value in canonical form (prolly_datum.h), "N" for NULL
 *	configseq<tab><sequence><tab><last value><tab><t|f>
 *		the state of a configuration sequence, or of a sequence a
 *		configuration table owns, when it has moved from its start
 *
 * Names and values are hex, so no name or value can be taken for a
 * separator, and nothing an element holds is ever put in a query as text:
 * a table's name is looked up, and must be one of the extension's
 * configuration tables (or a sequence one of them owns), and values are
 * bound as parameters.  Tables are read and written as their owners with
 * row security off and a search path of pg_catalog only, as pg_dump and a
 * restore read and write them: the caller's schemas can't hold an
 * operator or function that an extcondition, a trigger or a default would
 * run as the owner.
 *
 * A table with a primary key (or a replica identity index) has its rows
 * matched by key: a row the commits record is the database's when the
 * database has a row with its key, whatever its other columns hold now.
 * A keyless table's rows are matched whole, each as many times as the
 * commits record it.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_extconfig.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/prolly_datum.h"
#include "access/prolly_extconfig.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/dependency.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_index.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_type.h"
#include "commands/extension.h"
#include "common/cryptohash.h"
#include "common/sha2.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/varlena.h"

/* A configuration relation of an extension, with its condition. */
typedef struct ConfigRelation
{
	Oid			relid;
	char	   *condition;		/* extcondition's ("" for none) */
} ConfigRelation;

/* The identity a block of work runs as: a relation's owner. */
typedef struct AsOwner
{
	Oid			save_user;
	int			save_sec;
	int			nest;
} AsOwner;

/* A row (its fields, after the table's name), and its key. */
typedef struct KeyedRow
{
	char	   *key;			/* the key's fields; the row for a keyless table */
	char	   *row;
} KeyedRow;

/* An INSERT into a configuration table, for rows of one set of columns. */
typedef struct InsertPlan
{
	char	   *columns;		/* the fields' hex names, joined */
	SPIPlanPtr	plan;
	int			n;
	ProllyDatumCodec **codecs;
} InsertPlan;

bool
prolly_extconfig_element(const char *element)
{
	return strncmp(element, PROLLY_EXTCONFIG_ROW, strlen(PROLLY_EXTCONFIG_ROW)) == 0 ||
		strncmp(element, PROLLY_EXTCONFIG_SEQUENCE, strlen(PROLLY_EXTCONFIG_SEQUENCE)) == 0;
}

/*
 * An extension's configuration relations (ConfigRelation *), and the
 * sequences its configuration tables' columns own (an identity's, a
 * serial's), whose states pg_dump dumps with the table.
 */
static List *
config_relations(Oid extoid)
{
	HeapTuple	tup = SearchSysCache1(EXTENSIONOID, ObjectIdGetDatum(extoid));
	Datum		config;
	Datum		condition;
	bool		isnull;
	bool		cond_null;
	Datum	   *rels;
	Datum	   *conds = NULL;
	int			nrels;
	int			nconds = 0;
	List	   *out = NIL;
	List	   *tables = NIL;

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for extension %u", extoid);
	config = SysCacheGetAttr(EXTENSIONOID, tup, Anum_pg_extension_extconfig, &isnull);
	condition = SysCacheGetAttr(EXTENSIONOID, tup, Anum_pg_extension_extcondition, &cond_null);
	if (!isnull)
	{
		deconstruct_array_builtin(DatumGetArrayTypeP(config), OIDOID, &rels, NULL, &nrels);
		if (!cond_null)
			deconstruct_array_builtin(DatumGetArrayTypeP(condition), TEXTOID, &conds, NULL,
									  &nconds);
		for (int i = 0; i < nrels; i++)
		{
			ConfigRelation *r = palloc(sizeof(ConfigRelation));

			r->relid = DatumGetObjectId(rels[i]);
			r->condition = i < nconds ? TextDatumGetCString(conds[i]) : "";
			out = lappend(out, r);
			if (get_rel_relkind(r->relid) == RELKIND_RELATION)
				tables = lappend_oid(tables, r->relid);
		}
	}
	ReleaseSysCache(tup);
	foreach_oid(table, tables)
	{
		foreach_oid(seq, getOwnedSequences(table))
		{
			bool		listed = false;

			foreach_ptr(ConfigRelation, o, out)
				listed = listed || o->relid == seq;
			if (!listed)
			{
				ConfigRelation *s = palloc(sizeof(ConfigRelation));

				s->relid = seq;
				s->condition = "";
				out = lappend(out, s);
			}
		}
	}
	return out;
}

static char *
qualified_name(Oid relid)
{
	return quote_qualified_identifier(get_namespace_name(get_rel_namespace(relid)),
									  get_rel_name(relid));
}

static char *
to_hex(const char *data, int len)
{
	char	   *out = palloc(len * 2 + 1);

	hex_encode(data, len, out);
	out[len * 2] = '\0';
	return out;
}

static char *
from_hex(const char *hex, int hexlen, int *len)
{
	char	   *out = palloc(hexlen / 2 + 1);

	if (hexlen % 2 != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid configuration row in an extension dependency")));
	*len = hex_decode(hex, hexlen, out);
	out[*len] = '\0';
	return out;
}

static void
bad_row(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("invalid configuration row in an extension dependency")));
}

static void
pin_setting(const char *name, const char *value)
{
	(void) set_config_option(name, value, PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);
}

/*
 * Run as relid's owner, as pg_dump and pg_restore do: row security off, and
 * nothing resolved through the caller's search path or read with the
 * caller's text settings.
 */
static void
as_owner_begin(Oid relid, AsOwner *o)
{
	HeapTuple	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	Oid			owner;

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	owner = ((Form_pg_class) GETSTRUCT(tup))->relowner;
	ReleaseSysCache(tup);
	GetUserIdAndSecContext(&o->save_user, &o->save_sec);
	SetUserIdAndSecContext(owner, o->save_sec | SECURITY_LOCAL_USERID_CHANGE |
						   SECURITY_RESTRICTED_OPERATION);
	o->nest = NewGUCNestLevel();
	pin_setting("search_path", "pg_catalog, pg_temp");
	pin_setting("row_security", "off");
	pin_setting("datestyle", "ISO, MDY");
	pin_setting("intervalstyle", "postgres");
	pin_setting("extra_float_digits", "1");
	pin_setting("timezone", "UTC");
}

static void
as_owner_end(AsOwner *o)
{
	AtEOXact_GUC(true, o->nest);
	SetUserIdAndSecContext(o->save_user, o->save_sec);
}

/* A relation's stored columns: their numbers, in order. */
static List *
stored_columns(Relation rel)
{
	TupleDesc	desc = RelationGetDescr(rel);
	List	   *out = NIL;

	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (!att->attisdropped && att->attgenerated == '\0')
			out = lappend_int(out, att->attnum);
	}
	return out;
}

/*
 * The rows of a configuration table a condition selects, as elements
 * (allocated in mcxt).  Inside SPI.
 */
static List *
table_rows(Oid relid, const char *condition, MemoryContext mcxt)
{
	Relation	rel = table_open(relid, AccessShareLock);
	TupleDesc	desc = RelationGetDescr(rel);
	List	   *cols = stored_columns(rel);
	ProllyDatumCodec **codecs = palloc(sizeof(ProllyDatumCodec *) * Max(list_length(cols), 1));
	char	  **hexnames = palloc(sizeof(char *) * Max(list_length(cols), 1));
	StringInfoData q;
	char	   *table = qualified_name(relid);
	char	   *hextable = to_hex(table, strlen(table));
	List	   *out = NIL;
	ListCell   *lc;
	AsOwner		as;

	initStringInfo(&q);
	appendStringInfoString(&q, "SELECT ");
	foreach(lc, cols)
	{
		Form_pg_attribute att = TupleDescAttr(desc, lfirst_int(lc) - 1);
		char	   *name = NameStr(att->attname);

		codecs[foreach_current_index(lc)] = prolly_datum_codec(att->atttypid, att->atttypmod,
															   CurrentMemoryContext);
		hexnames[foreach_current_index(lc)] = to_hex(name, strlen(name));
		appendStringInfo(&q, "%s%s", foreach_current_index(lc) ? ", " : "",
						 quote_identifier(name));
	}
	if (cols == NIL)
		appendStringInfoString(&q, "1");
	appendStringInfo(&q, " FROM ONLY %s %s", table, condition);

	as_owner_begin(relid, &as);
	if (SPI_execute(q.data, true, 0) != SPI_OK_SELECT)
		elog(ERROR, "could not read configuration table %s", table);
	as_owner_end(&as);

	for (uint64 r = 0; r < SPI_processed; r++)
	{
		StringInfoData e;
		MemoryContext old;

		initStringInfo(&e);
		appendStringInfo(&e, "%s%s\t", PROLLY_EXTCONFIG_ROW, hextable);
		for (int i = 0; i < list_length(cols); i++)
		{
			bool		isnull;
			Datum		v = SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, i + 1,
										  &isnull);

			appendStringInfo(&e, "%s%s=", i ? "," : "", hexnames[i]);
			if (isnull)
				appendStringInfoChar(&e, 'N');
			else
			{
				StringInfoData bytes;

				initStringInfo(&bytes);
				prolly_datum_encode(codecs[i], v, &bytes);
				appendStringInfoString(&e, to_hex(bytes.data, bytes.len));
				pfree(bytes.data);
			}
		}
		old = MemoryContextSwitchTo(mcxt);
		out = lappend(out, pstrdup(e.data));
		MemoryContextSwitchTo(old);
		pfree(e.data);
	}
	SPI_freetuptable(SPI_tuptable);
	table_close(rel, AccessShareLock);
	return out;
}

/*
 * A sequence's state, as an element (in mcxt), or NULL when it hasn't
 * moved from its start, so an extension not configured records none.
 * Inside SPI.
 */
static char *
sequence_state(Oid relid, MemoryContext mcxt)
{
	char	   *seq = qualified_name(relid);
	HeapTuple	tup = SearchSysCache1(SEQRELID, ObjectIdGetDatum(relid));
	int64		start;
	int64		last;
	bool		called;
	bool		isnull;
	AsOwner		as;
	char	   *out;
	MemoryContext old;

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for sequence %u", relid);
	start = ((Form_pg_sequence) GETSTRUCT(tup))->seqstart;
	ReleaseSysCache(tup);
	as_owner_begin(relid, &as);
	if (SPI_execute(psprintf("SELECT last_value, is_called FROM %s", seq), true, 0) != SPI_OK_SELECT ||
		SPI_processed != 1)
		elog(ERROR, "could not read configuration sequence %s", seq);
	as_owner_end(&as);
	last = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
	called = DatumGetBool(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &isnull));
	if (!called && last == start)
		return NULL;
	old = MemoryContextSwitchTo(mcxt);
	out = psprintf("%s%s\t" INT64_FORMAT "\t%c", PROLLY_EXTCONFIG_SEQUENCE,
				   to_hex(seq, strlen(seq)), last, called ? 't' : 'f');
	MemoryContextSwitchTo(old);
	return out;
}

static int
cstring_cmp(const ListCell *a, const ListCell *b)
{
	return strcmp(lfirst(a), lfirst(b));
}

List *
prolly_extconfig_elements(Oid extoid, bool *has_config)
{
	List	   *relations = config_relations(extoid);
	MemoryContext caller = CurrentMemoryContext;
	List	   *out = NIL;
	ListCell   *lc;

	if (relations == NIL)
		return NIL;
	*has_config = true;
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	foreach(lc, relations)
	{
		ConfigRelation *r = lfirst(lc);
		char		kind = get_rel_relkind(r->relid);

		List	   *more = NIL;
		MemoryContext spi;

		if (kind == RELKIND_SEQUENCE)
		{
			char	   *s = sequence_state(r->relid, caller);
			MemoryContext spi1;

			if (s)
			{
				spi1 = MemoryContextSwitchTo(caller);
				more = list_make1(s);
				MemoryContextSwitchTo(spi1);
			}
		}
		else if (kind == RELKIND_RELATION)
			more = table_rows(r->relid, r->condition, caller);
		/* (the list in the caller's memory: SPI_finish() frees SPI's) */
		spi = MemoryContextSwitchTo(caller);
		out = list_concat(out, more);
		MemoryContextSwitchTo(spi);
	}
	SPI_finish();
	list_sort(out, cstring_cmp);
	return out;
}

/*
 * A digest of a heap table's rows: each visible row's place and inserting
 * transaction, which every insert and update gives a new pair, while a
 * delete removes one.  Two states with the same digest have the same rows;
 * a digest that changes without the rows changing (a VACUUM FULL, a
 * freeze) only costs reading them again.
 */
static void
heap_digest(Relation rel, StringInfo out)
{
	Snapshot	snapshot = ActiveSnapshotSet() ? GetActiveSnapshot() : GetTransactionSnapshot();
	TableScanDesc scan = table_beginscan(rel, snapshot, 0, NULL, SO_NONE);
	TupleTableSlot *slot = table_slot_create(rel, NULL);
	pg_cryptohash_ctx *ctx = pg_cryptohash_create(PG_SHA256);
	uint8		digest[PG_SHA256_DIGEST_LENGTH];
	uint64		n = 0;

	if (pg_cryptohash_init(ctx) < 0)
		elog(ERROR, "could not compute a configuration table's digest");
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		bool		isnull;
		TransactionId xmin = DatumGetTransactionId(slot_getsysattr(slot,
																   MinTransactionIdAttributeNumber,
																   &isnull));
		BlockNumber block = ItemPointerGetBlockNumber(&slot->tts_tid);
		OffsetNumber offset = ItemPointerGetOffsetNumber(&slot->tts_tid);

		if (pg_cryptohash_update(ctx, (uint8 *) &block, sizeof(block)) < 0 ||
			pg_cryptohash_update(ctx, (uint8 *) &offset, sizeof(offset)) < 0 ||
			pg_cryptohash_update(ctx, (uint8 *) &xmin, sizeof(xmin)) < 0)
			elog(ERROR, "could not compute a configuration table's digest");
		n++;
	}
	if (pg_cryptohash_final(ctx, digest, sizeof(digest)) < 0)
		elog(ERROR, "could not compute a configuration table's digest");
	pg_cryptohash_free(ctx);
	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
	appendStringInfo(out, "%u " UINT64_FORMAT " %s\n", RelationGetRelid(rel), n,
					 to_hex((char *) digest, sizeof(digest)));
}

char *
prolly_extconfig_fingerprint(Oid extoid)
{
	List	   *relations = config_relations(extoid);
	MemoryContext caller = CurrentMemoryContext;
	StringInfoData out;

	initStringInfo(&out);
	appendStringInfo(&out, "%u\n", extoid);
	if (relations == NIL)
		return out.data;
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	foreach_ptr(ConfigRelation, r, relations)
	{
		char		kind = get_rel_relkind(r->relid);
		MemoryContext spi = MemoryContextSwitchTo(caller);

		appendStringInfo(&out, "%u %c %s\n", r->relid, kind, r->condition);
		MemoryContextSwitchTo(spi);
		if (kind == RELKIND_SEQUENCE)
		{
			char	   *s = sequence_state(r->relid, caller);

			if (s)
			{
				spi = MemoryContextSwitchTo(caller);
				appendStringInfo(&out, "%s\n", s);
				MemoryContextSwitchTo(spi);
			}
		}
		else if (kind == RELKIND_RELATION)
		{
			Relation	rel = table_open(r->relid, AccessShareLock);

			spi = MemoryContextSwitchTo(caller);
			if (rel->rd_tableam == GetHeapamTableAmRoutine())
				heap_digest(rel, &out);
			else
				foreach_ptr(char, e, table_rows(r->relid, r->condition, caller))
					appendStringInfo(&out, "%s\n", e);
			MemoryContextSwitchTo(spi);
			table_close(rel, AccessShareLock);
		}
	}
	SPI_finish();
	return out.data;
}

/* The configuration relation an element's hex name names, or an error. */
static Oid
element_relation(const char *hexname, int len, List *relations, const char *extname)
{
	int			namelen;
	char	   *name = from_hex(hexname, len, &namelen);
	Oid			relid;
	ListCell   *lc;

	relid = RangeVarGetRelid(makeRangeVarFromNameList(stringToQualifiedNameList(name, NULL)),
							 NoLock, true);
	foreach(lc, relations)
		if (OidIsValid(relid) && ((ConfigRelation *) lfirst(lc))->relid == relid)
			return relid;
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("an extension dependency records configuration rows of %s, which isn't a configuration table of extension %s",
					name, extname)));
	return InvalidOid;
}

/*
 * The hex names of a table's key columns, in the key's order: its primary
 * key's, or else its replica identity index's; NIL for a keyless table.
 */
static List *
key_columns(Relation rel)
{
	Oid			index = RelationGetPrimaryKeyIndex(rel, true);
	HeapTuple	tup;
	Form_pg_index form;
	List	   *out = NIL;

	if (!OidIsValid(index))
		index = RelationGetReplicaIndex(rel);
	if (!OidIsValid(index))
		return NIL;
	tup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(index));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for index %u", index);
	form = (Form_pg_index) GETSTRUCT(tup);
	for (int i = 0; i < form->indnkeyatts; i++)
	{
		char	   *name = get_attname(RelationGetRelid(rel), form->indkey.values[i], false);

		out = lappend(out, to_hex(name, strlen(name)));
	}
	ReleaseSysCache(tup);
	return out;
}

/*
 * A row's key: the fields of the key's columns, in the key's order (the
 * whole row for a keyless table).  A recorded row lacking a key column
 * can't be matched by key: an error.
 */
static char *
row_key(const char *row, List *keys, const char *table)
{
	StringInfoData out;

	if (keys == NIL)
		return (char *) row;
	initStringInfo(&out);
	foreach_ptr(char, k, keys)
	{
		int			klen = strlen(k);
		const char *f = row;
		bool		found = false;

		while (f && *f)
		{
			const char *end = strchr(f, ',');

			if (strncmp(f, k, klen) == 0 && f[klen] == '=')
			{
				int			flen = end ? end - f : strlen(f);

				appendBinaryStringInfo(&out, f, flen);
				appendStringInfoChar(&out, ',');
				found = true;
				break;
			}
			f = end ? end + 1 : NULL;
		}
		if (!found)
		{
			int			len;

			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("the commits' rows of configuration table %s lack its key column \"%s\"",
							table, from_hex(k, klen, &len))));
		}
	}
	return out.data;
}

static int
keyed_cmp(const void *a, const void *b)
{
	const KeyedRow *x = a;
	const KeyedRow *y = b;
	int			c = strcmp(x->key, y->key);

	return c != 0 ? c : strcmp(x->row, y->row);
}

/* The first of n sorted rows with key, or -1. */
static int
first_with_key(KeyedRow *rows, int n, const char *key)
{
	int			lo = 0;
	int			hi = n;

	while (lo < hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (strcmp(rows[mid].key, key) < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < n && strcmp(rows[lo].key, key) == 0 ? lo : -1;
}

/* The INSERT for rows of a recorded row's columns, prepared once.  Inside SPI. */
static InsertPlan *
insert_plan(Relation rel, const char *table, const char *row, List **plans)
{
	TupleDesc	desc = RelationGetDescr(rel);
	StringInfoData columns;
	StringInfoData q;
	StringInfoData vals;
	char	   *copy = pstrdup(row);
	char	   *field;
	char	   *save;
	List	   *names = NIL;
	InsertPlan *p;
	Oid		   *types;
	int			n = 0;

	initStringInfo(&columns);
	for (field = strtok_r(copy, ",", &save); field; field = strtok_r(NULL, ",", &save))
	{
		char	   *eq = strchr(field, '=');

		if (eq == NULL)
			bad_row();
		appendBinaryStringInfo(&columns, field, eq - field);
		appendStringInfoChar(&columns, ',');
		names = lappend(names, pnstrdup(field, eq - field));
	}
	foreach_ptr(InsertPlan, have, *plans)
		if (strcmp(have->columns, columns.data) == 0)
			return have;

	p = palloc0(sizeof(InsertPlan));
	p->columns = columns.data;
	p->n = list_length(names);
	p->codecs = palloc(sizeof(ProllyDatumCodec *) * Max(p->n, 1));
	types = palloc(sizeof(Oid) * Max(p->n, 1));
	initStringInfo(&q);
	initStringInfo(&vals);
	appendStringInfo(&q, "INSERT INTO %s (", table);
	foreach_ptr(char, hexname, names)
	{
		int			len;
		char	   *name = from_hex(hexname, strlen(hexname), &len);
		AttrNumber	attnum = get_attnum(RelationGetRelid(rel), name);
		Form_pg_attribute att;

		if (attnum == InvalidAttrNumber)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("configuration table %s has no column \"%s\", which the commits' rows of it have",
							table, name)));
		att = TupleDescAttr(desc, attnum - 1);
		types[n] = att->atttypid;
		p->codecs[n] = prolly_datum_codec(att->atttypid, att->atttypmod, CurrentMemoryContext);
		appendStringInfo(&q, "%s%s", n ? ", " : "", quote_identifier(name));
		appendStringInfo(&vals, "%s$%d", n ? ", " : "", n + 1);
		n++;
	}
	appendStringInfo(&q, ") OVERRIDING SYSTEM VALUE VALUES (%s)", vals.data);
	p->plan = SPI_prepare(q.data, p->n, types);
	if (p->plan == NULL)
		elog(ERROR, "could not prepare a write of configuration table %s", table);
	*plans = lappend(*plans, p);
	return p;
}

/*
 * Write a recorded row into its configuration table.  Inside SPI, as the
 * table's owner.  Decoding checks each value (text is valid in the
 * database's encoding, and so on), as reading it from any commit does.
 */
static void
restore_row(Relation rel, const char *table, const char *row, List **plans)
{
	InsertPlan *p = insert_plan(rel, table, row, plans);
	Datum	   *values = palloc(sizeof(Datum) * Max(p->n, 1));
	char	   *nulls = palloc(Max(p->n, 1));
	char	   *copy = pstrdup(row);
	char	   *field;
	char	   *save;
	int			n = 0;

	for (field = strtok_r(copy, ",", &save); field; field = strtok_r(NULL, ",", &save))
	{
		char	   *eq = strchr(field, '=');

		if (strcmp(eq + 1, "N") == 0)
		{
			values[n] = (Datum) 0;
			nulls[n] = 'n';
		}
		else
		{
			int			len;
			char	   *bytes = from_hex(eq + 1, strlen(eq + 1), &len);

			values[n] = prolly_datum_decode(p->codecs[n], bytes, len);
			nulls[n] = ' ';
		}
		n++;
	}
	if (SPI_execute_plan(p->plan, values, nulls, false, 0) != SPI_OK_INSERT)
		elog(ERROR, "could not write configuration table %s", table);
}

/* One recorded configuration row, and the table it is of. */
typedef struct RecordedRow
{
	Oid			relid;
	const char *row;
} RecordedRow;

/*
 * Check (or, with restore, write) one table's recorded rows against its
 * rows.
 */
static void
apply_table(const char *extname, Oid relid, List *rows, bool restore,
			StringInfo missing)
{
	Relation	rel = table_open(relid, restore ? RowExclusiveLock : AccessShareLock);
	char	   *table = qualified_name(relid);
	List	   *keys = key_columns(rel);
	List	   *have_rows = table_rows(relid, "", CurrentMemoryContext);
	int			nhave = list_length(have_rows);
	int			nrec = list_length(rows);
	KeyedRow   *have = palloc(sizeof(KeyedRow) * Max(nhave, 1));
	KeyedRow   *rec = palloc(sizeof(KeyedRow) * Max(nrec, 1));
	List	   *to_write = NIL;
	List	   *plans = NIL;
	int			wanted = 0;
	int			lacking = 0;
	int			differing = 0;
	int			i;

	/* (the table's own rows are encoded under its name as it is now) */
	i = 0;
	foreach_ptr(char, h, have_rows)
	{
		char	   *fields = strchr(h + strlen(PROLLY_EXTCONFIG_ROW), '\t') + 1;

		have[i].row = fields;
		have[i].key = row_key(fields, keys, table);
		i++;
	}
	i = 0;
	foreach_ptr(char, r, rows)
	{
		rec[i].row = r;
		rec[i].key = row_key(r, keys, table);
		i++;
	}
	qsort(have, nhave, sizeof(KeyedRow), keyed_cmp);
	qsort(rec, nrec, sizeof(KeyedRow), keyed_cmp);

	for (i = 0; i < nrec;)
	{
		int			j = i + 1;
		int			at;

		while (j < nrec && strcmp(rec[j].key, rec[i].key) == 0)
			j++;
		at = first_with_key(have, nhave, rec[i].key);
		if (keys != NIL)
		{
			/*
			 * One row per key, whatever the commits recorded its other
			 * columns as (they may differ between commits): the database's
			 * when it has the key.  A clone's database whose row with the
			 * key differs from every one the commits record (one its
			 * extension's script made) can't take the commits' rows.
			 */
			wanted++;
			if (at < 0)
			{
				if (restore)
					to_write = lappend(to_write, rec[i].row);
				else
					lacking++;
			}
			else if (restore)
			{
				bool		same = false;

				for (int k = i; k < j && !same; k++)
					same = bsearch(&rec[k], have, nhave, sizeof(KeyedRow), keyed_cmp) != NULL;
				if (!same)
					differing++;
			}
		}
		else
		{
			/* a keyless table's rows, as many times as recorded */
			int			count = 0;

			for (int k = at; k >= 0 && k < nhave && strcmp(have[k].key, rec[i].key) == 0; k++)
				count++;
			wanted += j - i;
			for (int k = count; k < j - i; k++)
			{
				if (restore)
					to_write = lappend(to_write, rec[i].row);
				else
					lacking++;
			}
		}
		i = j;
	}
	if (differing > 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNIQUE_VIOLATION),
				 errmsg("configuration table %s of extension %s has other rows with the keys of %d of the rows the commits record",
						table, extname, differing),
				 errdetail("A row is matched by the table's key, and the extension made rows with those keys itself.")));
	if (to_write != NIL)
	{
		AsOwner		as;

		as_owner_begin(relid, &as);
		foreach_ptr(char, r, to_write)
			restore_row(rel, table, r, &plans);
		as_owner_end(&as);
	}
	if (lacking > 0)
		appendStringInfo(missing, "\nextension %s: configuration table %s lacks %d of the %d rows the commits record",
						 extname, table, lacking, wanted);
	table_close(rel, NoLock);
}

void
prolly_extconfig_apply(const char *extname, List *elements, bool restore,
					   StringInfo missing)
{
	Oid			extoid;
	List	   *relations;
	List	   *tables = NIL;	/* Oid, those elements name */
	List	   *recorded = NIL; /* RecordedRow * */

	if (elements == NIL)
		return;
	extoid = get_extension_oid(extname, false);
	relations = config_relations(extoid);
	foreach_ptr(const char, e, elements)
	{
		if (strncmp(e, PROLLY_EXTCONFIG_SEQUENCE, strlen(PROLLY_EXTCONFIG_SEQUENCE)) == 0)
		{
			const char *name = e + strlen(PROLLY_EXTCONFIG_SEQUENCE);
			const char *tab = strchr(name, '\t');
			Oid			relid;
			int64		last;
			bool		called;
			char	   *rest;

			if (tab == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("invalid configuration sequence in an extension dependency")));
			relid = element_relation(name, tab - name, relations, extname);
			last = strtoi64(tab + 1, &rest, 10);
			if (*rest != '\t' || (rest[1] != 't' && rest[1] != 'f') ||
				get_rel_relkind(relid) != RELKIND_SEQUENCE)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("invalid configuration sequence in an extension dependency")));
			called = rest[1] == 't';
			/* (a move leaves sequences alone: they hold no row data refers to) */
			if (restore)
			{
				Oid			types[3] = {REGCLASSOID, INT8OID, BOOLOID};
				Datum		values[3] = {ObjectIdGetDatum(relid), Int64GetDatum(last),
				BoolGetDatum(called)};
				AsOwner		as;

				as_owner_begin(relid, &as);
				if (SPI_execute_with_args("SELECT pg_catalog.setval($1, $2, $3)", 3, types, values,
										  NULL, false, 0) != SPI_OK_SELECT)
					elog(ERROR, "could not set configuration sequence %u", relid);
				as_owner_end(&as);
			}
		}
		else
		{
			const char *name = e + strlen(PROLLY_EXTCONFIG_ROW);
			const char *tab = strchr(name, '\t');
			RecordedRow *r = palloc(sizeof(RecordedRow));

			if (tab == NULL)
				bad_row();
			r->relid = element_relation(name, tab - name, relations, extname);
			if (get_rel_relkind(r->relid) != RELKIND_RELATION)
				bad_row();
			r->row = tab + 1;
			recorded = lappend(recorded, r);
			tables = list_append_unique_oid(tables, r->relid);
		}
	}

	foreach_oid(relid, tables)
	{
		List	   *rows = NIL;

		foreach_ptr(RecordedRow, r, recorded)
			if (r->relid == relid)
				rows = lappend(rows, (char *) r->row);
		apply_table(extname, relid, rows, restore, missing);
	}
}
