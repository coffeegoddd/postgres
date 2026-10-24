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
 *		a configuration sequence's state, when it has moved from its start
 *
 * Names and values are hex, so no name or value can be taken for a
 * separator, and nothing an element holds is ever put in a query as text:
 * a table's name is looked up, and must be one of the extension's
 * configuration tables, and values are bound as parameters.  Tables are
 * read and written as their owners with row security off, as pg_dump and a
 * restore read and write them.
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
#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_type.h"
#include "commands/extension.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/regproc.h"
#include "utils/rel.h"
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

bool
prolly_extconfig_element(const char *element)
{
	return strncmp(element, PROLLY_EXTCONFIG_ROW, strlen(PROLLY_EXTCONFIG_ROW)) == 0 ||
		strncmp(element, PROLLY_EXTCONFIG_SEQUENCE, strlen(PROLLY_EXTCONFIG_SEQUENCE)) == 0;
}

/* An extension's configuration relations (ConfigRelation *). */
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
		}
	}
	ReleaseSysCache(tup);
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
	(void) set_config_option("row_security", "off", PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);
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

		codecs[foreach_current_index(lc)] = prolly_datum_codec(att->atttypid, att->atttypmod,
															   CurrentMemoryContext);
		appendStringInfo(&q, "%s%s", foreach_current_index(lc) ? ", " : "",
						 quote_identifier(NameStr(att->attname)));
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
		foreach(lc, cols)
		{
			Form_pg_attribute att = TupleDescAttr(desc, lfirst_int(lc) - 1);
			int			i = foreach_current_index(lc);
			bool		isnull;
			Datum		v = SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, i + 1,
										  &isnull);
			char	   *name = NameStr(att->attname);

			appendStringInfo(&e, "%s%s=", i ? "," : "", to_hex(name, strlen(name)));
			if (isnull)
				appendStringInfoChar(&e, 'N');
			else
			{
				StringInfoData bytes;

				initStringInfo(&bytes);
				prolly_datum_encode(codecs[i], v, &bytes);
				appendStringInfoString(&e, to_hex(bytes.data, bytes.len));
			}
		}
		old = MemoryContextSwitchTo(mcxt);
		out = lappend(out, pstrdup(e.data));
		MemoryContextSwitchTo(old);
	}
	table_close(rel, AccessShareLock);
	return out;
}

/*
 * A configuration sequence's state, as an element (in mcxt), or NULL when
 * it hasn't moved from its start, so an extension not configured records
 * none.  Inside SPI.
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

static int
strptr_cmp(const void *a, const void *b)
{
	return strcmp(*(char *const *) a, *(char *const *) b);
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

/* Write a recorded row into its configuration table.  Inside SPI. */
static void
restore_row(Oid relid, const char *row)
{
	Relation	rel = table_open(relid, RowExclusiveLock);
	TupleDesc	desc = RelationGetDescr(rel);
	char	   *table = qualified_name(relid);
	List	   *fields = NIL;
	StringInfoData q;
	StringInfoData vals;
	int			n = 0;
	Oid		   *types;
	Datum	   *values;
	char	   *nulls;
	char	   *copy = pstrdup(row);
	char	   *field;
	char	   *save;
	AsOwner		as;

	for (field = strtok_r(copy, ",", &save); field; field = strtok_r(NULL, ",", &save))
		fields = lappend(fields, field);
	types = palloc(sizeof(Oid) * Max(list_length(fields), 1));
	values = palloc(sizeof(Datum) * Max(list_length(fields), 1));
	nulls = palloc(Max(list_length(fields), 1));
	initStringInfo(&q);
	initStringInfo(&vals);
	appendStringInfo(&q, "INSERT INTO %s (", table);
	foreach_ptr(char, f, fields)
	{
		char	   *eq = strchr(f, '=');
		int			namelen;
		char	   *name;
		AttrNumber	attnum;
		Form_pg_attribute att;

		if (eq == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid configuration row in an extension dependency")));
		name = from_hex(f, eq - f, &namelen);
		attnum = get_attnum(relid, name);
		if (attnum == InvalidAttrNumber)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("configuration table %s has no column \"%s\", which the commits' rows of it have",
							table, name)));
		att = TupleDescAttr(desc, attnum - 1);
		types[n] = att->atttypid;
		if (strcmp(eq + 1, "N") == 0)
		{
			values[n] = (Datum) 0;
			nulls[n] = 'n';
		}
		else
		{
			int			len;
			char	   *bytes = from_hex(eq + 1, strlen(eq + 1), &len);
			ProllyDatumCodec *codec = prolly_datum_codec(att->atttypid, att->atttypmod,
														 CurrentMemoryContext);

			values[n] = prolly_datum_decode(codec, bytes, len);
			nulls[n] = ' ';
		}
		appendStringInfo(&q, "%s%s", n ? ", " : "", quote_identifier(name));
		appendStringInfo(&vals, "%s$%d", n ? ", " : "", n + 1);
		n++;
	}
	appendStringInfo(&q, ") OVERRIDING SYSTEM VALUE VALUES (%s)", vals.data);
	table_close(rel, NoLock);
	as_owner_begin(relid, &as);
	if (SPI_execute_with_args(q.data, n, types, values, nulls, false, 0) != SPI_OK_INSERT)
		elog(ERROR, "could not write configuration table %s", table);
	as_owner_end(&as);
}

void
prolly_extconfig_apply(const char *extname, List *elements, bool restore,
					   StringInfo missing)
{
	Oid			extoid = get_extension_oid(extname, false);
	List	   *relations = config_relations(extoid);
	List	   *tables = NIL;	/* Oid, those elements name */
	ListCell   *lc;

	if (elements == NIL)
		return;
	foreach(lc, elements)
	{
		const char *e = lfirst(lc);

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
			continue;
		}
		else
		{
			const char *name = e + strlen(PROLLY_EXTCONFIG_ROW);
			const char *tab = strchr(name, '\t');
			Oid			relid;

			if (tab == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("invalid configuration row in an extension dependency")));
			relid = element_relation(name, tab - name, relations, extname);
			if (get_rel_relkind(relid) != RELKIND_RELATION)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("invalid configuration row in an extension dependency")));
			tables = list_append_unique_oid(tables, relid);
		}
	}

	/* each table's rows: those recorded it lacks, found by sorted search */
	foreach(lc, tables)
	{
		Oid			relid = lfirst_oid(lc);
		List	   *have = table_rows(relid, "", CurrentMemoryContext);
		char	  **sorted = palloc(sizeof(char *) * Max(list_length(have), 1));
		int			nhave = 0;
		int			wanted = 0;
		int			lacking = 0;
		char	   *table = qualified_name(relid);
		char	   *prefix = psprintf("%s%s\t", PROLLY_EXTCONFIG_ROW, to_hex(table, strlen(table)));
		ListCell   *ec;

		foreach_ptr(char, h, have)
			sorted[nhave++] = h;
		qsort(sorted, nhave, sizeof(char *), strptr_cmp);
		foreach(ec, elements)
		{
			char	   *e = lfirst(ec);

			if (strncmp(e, PROLLY_EXTCONFIG_ROW, strlen(PROLLY_EXTCONFIG_ROW)) != 0 ||
				element_relation(e + strlen(PROLLY_EXTCONFIG_ROW),
								 strchr(e + strlen(PROLLY_EXTCONFIG_ROW), '\t') - (e + strlen(PROLLY_EXTCONFIG_ROW)),
								 relations, extname) != relid)
				continue;
			wanted++;
			/* (the table's own rows are encoded under its name as it is now) */
			{
				char	   *mine = psprintf("%s%s", prefix, strchr(e + strlen(PROLLY_EXTCONFIG_ROW), '\t') + 1);

				if (bsearch(&mine, sorted, nhave, sizeof(char *), strptr_cmp))
					continue;
			}
			if (restore)
				restore_row(relid, strchr(e + strlen(PROLLY_EXTCONFIG_ROW), '\t') + 1);
			else
				lacking++;
		}
		if (lacking > 0)
			appendStringInfo(missing, "\nextension %s: configuration table %s lacks %d of the %d rows the commits record",
							 extname, table, lacking, wanted);
	}
}
