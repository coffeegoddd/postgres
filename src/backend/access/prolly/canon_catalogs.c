/*-------------------------------------------------------------------------
 *
 * canon_catalogs.c
 *	  Canonicalizer implementation "catalogs": a working set's canonical
 *	  form, derived from the database's catalogs, by name.
 *
 * The canonical root is a tree keyed (by memcmp) by kind and qualified
 * name, every string in UTF-8:
 *
 *	"e:" extension		-> its version
 *	"f:" function(args)	-> its definition
 *	"i:" index			-> a prolly index's tree (hash length, hash)
 *	"m:" materialized view -> its data tree (hash length, hash), then its
 *						   query
 *	"n:" schema			-> nothing: a versioned schema (folded only), there
 *						   whether or not anything is in it
 *	"s:" sequence		-> its definition
 *	"t:" table			-> its data tree (hash length, hash), then its schema
 *	"v:" view			-> its query
 *	"y:" type			-> an enum's labels, a domain's or a composite
 *						   type's definition
 *
 * A table's schema lists its live columns in order (name, type, nullness,
 * collation if not the type's, default, identity, generation), then its
 * constraints, indexes, triggers and rules by name, as Postgres's deparser
 * prints them.
 * Names are qualified whatever the session's search path (it is set to
 * pg_catalog while deparsing), so nothing local, no OID or relfilenumber,
 * and nothing about how the objects came to be (their order, dropped
 * columns) reaches the hash.  Data trees and prolly index trees are
 * canonical as written; prepare() rewrites the rows of tables whose rows
 * aren't (prolly_canonicalize_table).
 *
 * Covered: prolly tables, their columns, constraints, indexes (prolly
 * index trees and other index AMs' definitions), triggers and rules;
 * views and materialized views; functions and procedures; enum, domain and
 * composite types; sequences; and extensions (by name and version; their
 * members aren't listed).  Anything else in the database's own schemas
 * (aggregates, policies, partitioned or foreign tables, other types) makes
 * canonical_root() fail: never a hash that might depend on history.  Heap
 * tables are not versioned and are left out.
 *
 * Implementation "folded" builds the same form for the session branch's
 * versioned schemas, the tables being block-mapped prolly tables: their
 * rows are folded into data trees (keyed by the table keying, sorted, each
 * a tree in the chunk store), and the canonical tree is kept there too.
 * Only objects in versioned schemas are described: ordinary schemas and
 * extensions are every branch's, not the commit's.  Each collation that
 * orders a table's keys is recorded too:
 *
 *	"c:" collation		-> its provider, locale, ICU rules, whether it is
 *						   deterministic and its library's version, a line
 *						   each ("default": the database's)
 *
 * and a table whose keys are ordered by a collation that differs now from
 * what a root recorded can't have that root's rows read: its tree's keys
 * may be in another order.  The root is still listed, and its other
 * tables read.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/canon_catalogs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/prolly_builtins.h"
#include "access/prolly_config.h"
#include "access/prolly_indexam.h"
#include "access/prolly_tableam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_database.h"
#include "catalog/pg_enum.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_policy.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "catalog/versioned.h"
#include "commands/defrem.h"
#include "executor/tuptable.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "rewrite/rewriteSupport.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/pg_locale.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

typedef struct CanonItem
{
	StringInfoData key;
	StringInfoData value;
} CanonItem;

typedef struct Canon
{
	ProllyComponents c;
	ProllyTreeEnv env;
	ProllyRootSet *roots;
	List	   *items;
	bool		folded;			/* the session branch's versioned schemas */
	const ProllyTreeRoutine *tree;
	List	   *collations;		/* OIDs of the collations ordering keys */
} Canon;

static void build_canonical(Canon *cn, ProllyNodeRef *root);

static int
memcmp_keys(const ProllyKey *a, const ProllyKey *b, void *arg)
{
	int			r = memcmp(a->data, b->data, Min(a->len, b->len));

	if (r != 0)
		return r;
	return (a->len > b->len) - (a->len < b->len);
}

static void
canon_env(Canon *cn)
{
	cn->tree = prolly_tree_lookup("cow");
	memset(&cn->env, 0, sizeof(cn->env));
	if (cn->folded)
	{
		cn->env.chunk_format = prolly_chunkformat_lookup("basic");
		cn->env.chunker = prolly_chunker_lookup("bytes");
		cn->env.hash = prolly_hash_lookup("sha256");
		cn->env.chunk_store = prolly_chunk_store();
		cn->env.chunks = PROLLY_CALL(cn->env.chunk_store, open, MyDatabaseId,
									 MyDatabaseTableSpace);
		cn->env.compare = memcmp_keys;
		return;
	}
	prolly_components_for_engine(&prolly_engine_d1.info, &cn->c);
	cn->env.node_format = cn->c.node_format;
	cn->env.chunker = cn->c.chunker;
	cn->env.hash = cn->c.hash;
	cn->env.node_store = cn->c.node_store;
	cn->env.store = PROLLY_CALL(cn->c.node_store, open, NULL);
	cn->env.log = cn->c.log;
	cn->env.compare = memcmp_keys;
}

/* Append s in UTF-8. */
static void
append_utf8(StringInfo out, const char *s)
{
	char	   *u = pg_server_to_any(s, strlen(s), PG_UTF8);

	appendStringInfoString(out, u);
	if (u != s)
		pfree(u);
}

static CanonItem *
add_item(Canon *cn, const char *kind, const char *name)
{
	CanonItem  *it = palloc(sizeof(CanonItem));

	initStringInfo(&it->key);
	appendStringInfoString(&it->key, kind);
	append_utf8(&it->key, name);
	initStringInfo(&it->value);
	cn->items = lappend(cn->items, it);
	return it;
}

static void
put_hash(StringInfo out, const ProllyHash *h)
{
	appendStringInfoChar(out, (char) h->len);
	appendBinaryStringInfo(out, h->bytes, h->len);
}

static int
cstring_cmp(const ListCell *a, const ListCell *b)
{
	return strcmp(lfirst(a), lfirst(b));
}

static bool
is_system_namespace(Oid nsp)
{
	return IsCatalogNamespace(nsp) || IsToastNamespace(nsp) ||
		isAnyTempNamespace(nsp) ||
		nsp == get_namespace_oid("information_schema", true);
}

static char *
qualified(Oid nsp, const char *name)
{
	return quote_qualified_identifier(get_namespace_name(nsp), name);
}

static bool
is_prolly_am(Oid amoid)
{
	char	   *amname = OidIsValid(amoid) ? get_am_name(amoid) : NULL;
	bool		prolly = prolly_is_table_am(amname);

	if (amname)
		pfree(amname);
	return prolly;
}

static void
uncovered(const char *what, const char *name)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("prolly commits don't cover %s yet: %s", what, name),
			 errdetail("A commit's root hash must depend only on what it holds, and the canonical form doesn't describe this object.")));
}

/* ------------------------------------------------------------------------
 * Tables
 * ------------------------------------------------------------------------
 */

/* A row folded: its key and its canonical encoding. */
typedef struct FoldRow
{
	StringInfoData key;
	StringInfoData row;
} FoldRow;

typedef struct FoldSort
{
	const ProllyKeyingRoutine *kr;
	ProllyKeying *k;
} FoldSort;

static int
fold_row_cmp(const void *a, const void *b, void *arg)
{
	const FoldRow *ra = *(FoldRow *const *) a;
	const FoldRow *rb = *(FoldRow *const *) b;
	FoldSort   *fs = arg;
	ProllyKey	ka = {(const uint8 *) ra->key.data, ra->key.len};
	ProllyKey	kb = {(const uint8 *) rb->key.data, rb->key.len};
	int			r = PROLLY_CALL(fs->kr, compare, fs->k, &ka, &kb);

	/* equal keys of a keyless table: the same row; order by bytes anyway */
	if (r == 0)
		r = memcmp(ra->key.data, rb->key.data, Min(ra->key.len, rb->key.len));
	return r != 0 ? r : (ra->key.len > rb->key.len) - (ra->key.len < rb->key.len);
}

static int
keying_compare(const ProllyKey *a, const ProllyKey *b, void *arg)
{
	FoldSort   *fs = arg;

	return PROLLY_CALL(fs->kr, compare, fs->k, a, b);
}

/*
 * A table's rows, as the transaction sees them, folded into a data tree:
 * each row's key and canonical encoding (the table keying's), in key
 * order, identical rows of a keyless table counted once.  The same rows
 * give the same tree whatever pages, order or history they come from.
 */
static void
fold_table(Canon *cn, Relation rel, ProllyNodeRef *root)
{
	FoldSort	fs;
	TableScanDesc scan;
	TupleTableSlot *slot;
	FoldRow   **rows = NULL;
	int			nrows = 0;
	int			maxrows = 0;
	ProllyTreeEnv env = cn->env;
	ProllyTreeBuilder *b;
	bool		keyless;

	fs.kr = (const ProllyKeyingRoutine *)
		prolly_get_impl(PROLLY_IFACE_TABLE_KEYING, "pk_or_content");

	/*
	 * Rows as a rewrite leaves them: a column added with a default is in
	 * every row (the scan gives its missing value), so whether the table was
	 * rewritten since doesn't change the encoding.
	 */
	fs.k = PROLLY_CALL(fs.kr, open_rewritten, rel, CurrentMemoryContext);
	keyless = PROLLY_CALL(fs.kr, keyless, fs.k);
	if (!keyless)
	{
		/* (the keying's key: the primary key, as it chooses it) */
		Relation	pk = index_open(RelationGetPrimaryKeyIndex(rel, false),
									AccessShareLock);

		for (int i = 0; i < IndexRelationGetNumberOfKeyAttributes(pk); i++)
		{
			if (OidIsValid(pk->rd_indcollation[i]))
				cn->collations = list_append_unique_oid(cn->collations,
														pk->rd_indcollation[i]);
		}
		index_close(pk, AccessShareLock);
	}

	scan = table_beginscan(rel, GetActiveSnapshot(), 0, NULL, 0);
	slot = table_slot_create(rel, NULL);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		FoldRow    *r = palloc(sizeof(FoldRow));
		ProllyHash	content;

		initStringInfo(&r->key);
		initStringInfo(&r->row);
		PROLLY_CALL(fs.kr, encode_row, fs.k, slot, &r->key, &r->row, &content);
		if (nrows == maxrows)
		{
			maxrows = Max(64, maxrows * 2);
			rows = rows ? repalloc_array(rows, FoldRow *, maxrows) :
				palloc_array(FoldRow *, maxrows);
		}
		rows[nrows++] = r;
		CHECK_FOR_INTERRUPTS();
	}
	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);

	if (nrows > 1)
		qsort_arg(rows, nrows, sizeof(FoldRow *), fold_row_cmp, &fs);

	env.compare = keying_compare;
	env.compare_arg = &fs;
	b = PROLLY_CALL(cn->tree, build_begin, &env, CurrentMemoryContext);
	for (int i = 0; i < nrows;)
	{
		int			j = i + 1;
		ProllyKey	k = {(const uint8 *) rows[i]->key.data, rows[i]->key.len};
		ProllyValue row = {(const uint8 *) rows[i]->row.data, rows[i]->row.len};
		StringInfoData value;
		ProllyValue v;

		while (j < nrows && rows[j]->key.len == rows[i]->key.len &&
			   memcmp(rows[j]->key.data, rows[i]->key.data, k.len) == 0)
			j++;
		if (!keyless && j > i + 1)
			ereport(ERROR,
					(errcode(ERRCODE_UNIQUE_VIOLATION),
					 errmsg("table \"%s\" holds rows with the same primary key",
							RelationGetRelationName(rel)),
					 errdetail("A commit can't hold them until the key is unique again.")));
		initStringInfo(&value);
		PROLLY_CALL(fs.kr, make_value, fs.k, &row, (uint64) (j - i), &value);
		v.data = (const uint8 *) value.data;
		v.len = value.len;
		PROLLY_CALL(cn->tree, build_add, b, &k, &v);
		pfree(value.data);
		i = j;
	}
	PROLLY_CALL(cn->tree, build_finish, b, root);
	PROLLY_CALL(fs.kr, close, fs.k);
}

/* A table's data tree: folded from its rows, or its working set's. */
static void
table_root(Canon *cn, Relation rel, ProllyNodeRef *root)
{
	if (cn->folded)
		fold_table(cn, rel, root);
	else
		PROLLY_CALL(cn->c.engine, tree_root, cn->roots, rel, PROLLY_TREE_DATA,
					root);
}

/* Is a relation of this access method's rows in the canonical form? */
static bool
versioned_am(Canon *cn, Oid amoid)
{
	return cn->folded ? amoid == PROLLY_TABLE_AM_OID : is_prolly_am(amoid);
}

/* Is an object in this namespace the canonical form's? */
static bool
canon_namespace(Canon *cn, Oid nsp)
{
	if (is_system_namespace(nsp))
		return false;
	return !cn->folded || namespace_is_versioned(nsp);
}

static int
oid_name_cmp(const ListCell *a, const ListCell *b)
{
	return strcmp(get_rel_name(lfirst_oid(a)), get_rel_name(lfirst_oid(b)));
}

/*
 * The definitions of rel's triggers (internal ones aside) or rules (a
 * view's own aside), sorted, one per line.
 */
static void
append_sorted_defs(StringInfo out, Oid catalog, AttrNumber relattr,
				   Oid index, Oid relid)
{
	Relation	crel = table_open(catalog, AccessShareLock);
	ScanKeyData skey;
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *defs = NIL;
	ListCell   *lc;

	ScanKeyInit(&skey, relattr, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	scan = systable_beginscan(crel, index, true, NULL, 1, &skey);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		Datum		def;

		if (catalog == TriggerRelationId)
		{
			Form_pg_trigger trg = (Form_pg_trigger) GETSTRUCT(tup);

			if (trg->tgisinternal)
				continue;
			def = DirectFunctionCall1(pg_get_triggerdef, ObjectIdGetDatum(trg->oid));
		}
		else
		{
			Form_pg_rewrite rw = (Form_pg_rewrite) GETSTRUCT(tup);

			if (strcmp(NameStr(rw->rulename), ViewSelectRuleName) == 0)
				continue;
			def = DirectFunctionCall1(pg_get_ruledef, ObjectIdGetDatum(rw->oid));
		}
		defs = lappend(defs, psprintf("%s\n", TextDatumGetCString(def)));
	}
	systable_endscan(scan);
	table_close(crel, AccessShareLock);
	list_sort(defs, cstring_cmp);
	foreach(lc, defs)
		append_utf8(out, lfirst(lc));
}

static void
describe_table(Canon *cn, Relation rel, StringInfo out)
{
	TupleDesc	desc = RelationGetDescr(rel);
	List	   *indexes;
	ListCell   *lc;
	Relation	conrel;
	SysScanDesc scan;
	ScanKeyData skey;
	HeapTuple	tup;
	List	   *cons = NIL;

	appendStringInfoString(out, "columns\n");
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (att->attisdropped)
			continue;
		append_utf8(out, quote_identifier(NameStr(att->attname)));
		appendStringInfoChar(out, '\t');
		append_utf8(out, format_type_extended(att->atttypid, att->atttypmod,
											  FORMAT_TYPE_TYPEMOD_GIVEN |
											  FORMAT_TYPE_FORCE_QUALIFY));
		if (att->attnotnull)
			appendStringInfoString(out, "\tnot null");
		if (OidIsValid(att->attcollation) &&
			att->attcollation != get_typcollation(att->atttypid))
		{
			appendStringInfoString(out, "\tcollate ");
			append_utf8(out, generate_collation_name(att->attcollation));
		}
		if (att->atthasdef && desc->constr)
		{
			for (int d = 0; d < desc->constr->num_defval; d++)
			{
				AttrDefault *def = &desc->constr->defval[d];

				if (def->adnum == att->attnum)
				{
					Datum		txt = DirectFunctionCall2(pg_get_expr,
														  CStringGetTextDatum(def->adbin),
														  ObjectIdGetDatum(RelationGetRelid(rel)));

					appendStringInfoString(out, att->attgenerated ?
										   "\tgenerated " : "\tdefault ");
					append_utf8(out, TextDatumGetCString(txt));
				}
			}
		}
		if (att->attidentity)
			appendStringInfo(out, "\tidentity %c", att->attidentity);
		if (att->attgenerated)
			appendStringInfo(out, "\tstored %c", att->attgenerated);
		appendStringInfoChar(out, '\n');
	}

	/* Constraints, by name. */
	appendStringInfoString(out, "constraints\n");
	conrel = table_open(ConstraintRelationId, AccessShareLock);
	ScanKeyInit(&skey, Anum_pg_constraint_conrelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(rel)));
	scan = systable_beginscan(conrel, ConstraintRelidTypidNameIndexId, true,
							  NULL, 1, &skey);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tup);
		StringInfo	line = makeStringInfo();

		append_utf8(line, quote_identifier(NameStr(con->conname)));
		appendStringInfoChar(line, '\t');
		append_utf8(line, TextDatumGetCString(DirectFunctionCall1(pg_get_constraintdef,
																  ObjectIdGetDatum(con->oid))));
		appendStringInfoChar(line, '\n');
		cons = lappend(cons, line->data);
	}
	systable_endscan(scan);
	table_close(conrel, AccessShareLock);
	list_sort(cons, cstring_cmp);	/* constraint names are unique */
	foreach(lc, cons)
		appendStringInfoString(out, lfirst(lc));

	/* Indexes, by name. */
	appendStringInfoString(out, "indexes\n");
	indexes = list_copy(RelationGetIndexList(rel));
	list_sort(indexes, oid_name_cmp);
	foreach(lc, indexes)
	{
		Oid			idx = lfirst_oid(lc);

		append_utf8(out, pg_get_indexdef_string(idx));
		appendStringInfoChar(out, '\n');
		if (!cn->folded && is_prolly_am(get_rel_relam(idx)))
		{
			Relation	irel = index_open(idx, AccessShareLock);
			CanonItem  *it = add_item(cn, "i:",
									  qualified(RelationGetNamespace(irel),
												RelationGetRelationName(irel)));
			ProllyNodeRef root;

			PROLLY_CALL(cn->c.engine, tree_root, cn->roots, irel,
						PROLLY_TREE_DATA, &root);
			put_hash(&it->value, &root.hash);
			index_close(irel, AccessShareLock);
		}
	}

	/* Triggers and rules, by name. */
	appendStringInfoString(out, "triggers\n");
	append_sorted_defs(out, TriggerRelationId, Anum_pg_trigger_tgrelid,
					   TriggerRelidNameIndexId, RelationGetRelid(rel));
	appendStringInfoString(out, "rules\n");
	append_sorted_defs(out, RewriteRelationId, Anum_pg_rewrite_ev_class,
					   RewriteRelRulenameIndexId, RelationGetRelid(rel));
}

/* ------------------------------------------------------------------------
 * Types, sequences, extensions
 * ------------------------------------------------------------------------
 */

static void
describe_type(Canon *cn, Form_pg_type typ)
{
	CanonItem  *it;

	if (typ->typtype == TYPTYPE_ENUM)
	{
		List	   *labels = NIL;
		Relation	erel = table_open(EnumRelationId, AccessShareLock);
		ScanKeyData skey;
		SysScanDesc scan;
		HeapTuple	tup;
		typedef struct Label
		{
			float4		order;
			char	   *name;
		} Label;
		Label	   *arr;
		int			n = 0;
		ListCell   *lc;

		ScanKeyInit(&skey, Anum_pg_enum_enumtypid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(typ->oid));
		scan = systable_beginscan(erel, EnumTypIdLabelIndexId, true, NULL, 1,
								  &skey);
		while ((tup = systable_getnext(scan)) != NULL)
		{
			Form_pg_enum e = (Form_pg_enum) GETSTRUCT(tup);
			Label	   *l = palloc(sizeof(Label));

			l->order = e->enumsortorder;
			l->name = pstrdup(NameStr(e->enumlabel));
			labels = lappend(labels, l);
		}
		systable_endscan(scan);
		table_close(erel, AccessShareLock);
		arr = palloc(sizeof(Label) * Max(list_length(labels), 1));
		foreach(lc, labels)
			arr[n++] = *(Label *) lfirst(lc);
		/* By sort order: insertion sort, enums are small. */
		for (int i = 1; i < n; i++)
			for (int j = i; j > 0 && arr[j - 1].order > arr[j].order; j--)
			{
				Label		t = arr[j];

				arr[j] = arr[j - 1];
				arr[j - 1] = t;
			}
		it = add_item(cn, "y:", qualified(typ->typnamespace, NameStr(typ->typname)));
		appendStringInfoString(&it->value, "enum\n");
		for (int i = 0; i < n; i++)
		{
			append_utf8(&it->value, quote_literal_cstr(arr[i].name));
			appendStringInfoChar(&it->value, '\n');
		}
	}
	else if (typ->typtype == TYPTYPE_DOMAIN)
	{
		it = add_item(cn, "y:", qualified(typ->typnamespace, NameStr(typ->typname)));
		appendStringInfoString(&it->value, "domain\n");
		append_utf8(&it->value, format_type_extended(typ->typbasetype,
													 typ->typtypmod,
													 FORMAT_TYPE_TYPEMOD_GIVEN |
													 FORMAT_TYPE_FORCE_QUALIFY));
		if (typ->typnotnull)
			appendStringInfoString(&it->value, "\tnot null");
		appendStringInfoChar(&it->value, '\n');
		{
			Relation	conrel = table_open(ConstraintRelationId, AccessShareLock);
			ScanKeyData skey[2];
			SysScanDesc scan;
			HeapTuple	tup;
			List	   *cons = NIL;
			ListCell   *lc;

			ScanKeyInit(&skey[0], Anum_pg_constraint_conrelid,
						BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(InvalidOid));
			ScanKeyInit(&skey[1], Anum_pg_constraint_contypid,
						BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(typ->oid));
			scan = systable_beginscan(conrel, ConstraintRelidTypidNameIndexId,
									  true, NULL, 2, skey);
			while ((tup = systable_getnext(scan)) != NULL)
			{
				Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tup);

				cons = lappend(cons, psprintf("%s\t%s\n",
											  quote_identifier(NameStr(con->conname)),
											  TextDatumGetCString(DirectFunctionCall1(pg_get_constraintdef,
																					  ObjectIdGetDatum(con->oid)))));
			}
			systable_endscan(scan);
			table_close(conrel, AccessShareLock);
			list_sort(cons, cstring_cmp);
			foreach(lc, cons)
				append_utf8(&it->value, lfirst(lc));
		}
	}
	else if (typ->typtype == TYPTYPE_COMPOSITE &&
			 get_rel_relkind(typ->typrelid) == RELKIND_COMPOSITE_TYPE)
	{
		Relation	crel = relation_open(typ->typrelid, AccessShareLock);
		TupleDesc	desc = RelationGetDescr(crel);

		it = add_item(cn, "y:", qualified(typ->typnamespace, NameStr(typ->typname)));
		appendStringInfoString(&it->value, "composite\n");
		for (int i = 0; i < desc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(desc, i);

			if (att->attisdropped)
				continue;
			append_utf8(&it->value, quote_identifier(NameStr(att->attname)));
			appendStringInfoChar(&it->value, '\t');
			append_utf8(&it->value, format_type_extended(att->atttypid,
														 att->atttypmod,
														 FORMAT_TYPE_TYPEMOD_GIVEN |
														 FORMAT_TYPE_FORCE_QUALIFY));
			appendStringInfoChar(&it->value, '\n');
		}
		relation_close(crel, AccessShareLock);
	}
	else if (typ->typtype == TYPTYPE_RANGE || typ->typtype == TYPTYPE_MULTIRANGE ||
			 typ->typtype == TYPTYPE_BASE)
	{
		/* Array types of covered types come with them. */
		if (OidIsValid(typ->typelem) && typ->typcategory == TYPCATEGORY_ARRAY)
			return;
		uncovered("this type", qualified(typ->typnamespace, NameStr(typ->typname)));
	}
}

static void
describe_sequence(Canon *cn, Oid seqid, Oid nsp, const char *name)
{
	HeapTuple	tup = SearchSysCache1(SEQRELID, ObjectIdGetDatum(seqid));
	Form_pg_sequence seq;
	CanonItem  *it;

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for sequence %u", seqid);
	seq = (Form_pg_sequence) GETSTRUCT(tup);
	it = add_item(cn, "s:", qualified(nsp, name));
	appendStringInfo(&it->value, "%s\t" INT64_FORMAT "\t" INT64_FORMAT "\t"
					 INT64_FORMAT "\t" INT64_FORMAT "\t" INT64_FORMAT "\t%s\n",
					 format_type_extended(seq->seqtypid, -1, FORMAT_TYPE_FORCE_QUALIFY),
					 seq->seqstart, seq->seqincrement, seq->seqmax, seq->seqmin,
					 seq->seqcache, seq->seqcycle ? "cycle" : "no cycle");
	ReleaseSysCache(tup);
}

/* Members of an extension come with it. */
static bool
in_extension(Oid classid, Oid objid)
{
	return OidIsValid(getExtensionOfObject(classid, objid));
}

/* ------------------------------------------------------------------------
 * The canonical root
 * ------------------------------------------------------------------------
 */

static int
item_cmp(const ListCell *a, const ListCell *b)
{
	const CanonItem *ia = lfirst(a);
	const CanonItem *ib = lfirst(b);
	ProllyKey	ka = {(const uint8 *) ia->key.data, ia->key.len};
	ProllyKey	kb = {(const uint8 *) ib->key.data, ib->key.len};

	return memcmp_keys(&ka, &kb, NULL);
}

static void
collect(Canon *cn)
{
	Relation	pg_class;
	SysScanDesc scan;
	HeapTuple	tup;

	/* Relations. */
	pg_class = table_open(RelationRelationId, AccessShareLock);
	scan = systable_beginscan(pg_class, InvalidOid, false, NULL, 0, NULL);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_class form = (Form_pg_class) GETSTRUCT(tup);
		char	   *name = NameStr(form->relname);

		if (!canon_namespace(cn, form->relnamespace) ||
			in_extension(RelationRelationId, form->oid))
			continue;
		switch (form->relkind)
		{
			case RELKIND_RELATION:
				if (versioned_am(cn, form->relam))
				{
					Relation	rel = table_open(form->oid, AccessShareLock);
					CanonItem  *it = add_item(cn, "t:", qualified(form->relnamespace, name));
					ProllyNodeRef root;

					table_root(cn, rel, &root);
					put_hash(&it->value, &root.hash);
					describe_table(cn, rel, &it->value);
					table_close(rel, AccessShareLock);
				}
				break;
			case RELKIND_SEQUENCE:
				describe_sequence(cn, form->oid, form->relnamespace, name);
				break;
			case RELKIND_INDEX:
			case RELKIND_TOASTVALUE:
			case RELKIND_COMPOSITE_TYPE:
				break;			/* with their tables, or as types */
			case RELKIND_VIEW:
				{
					CanonItem  *it = add_item(cn, "v:", qualified(form->relnamespace, name));

					append_utf8(&it->value,
								TextDatumGetCString(DirectFunctionCall2(pg_get_viewdef_ext,
																		ObjectIdGetDatum(form->oid),
																		BoolGetDatum(false))));
				}
				break;
			case RELKIND_MATVIEW:
				{
					CanonItem  *it = add_item(cn, "m:", qualified(form->relnamespace, name));

					if (versioned_am(cn, form->relam))
					{
						Relation	rel = table_open(form->oid, AccessShareLock);
						ProllyNodeRef root;

						table_root(cn, rel, &root);
						put_hash(&it->value, &root.hash);
						table_close(rel, AccessShareLock);
					}
					append_utf8(&it->value,
								TextDatumGetCString(DirectFunctionCall2(pg_get_viewdef_ext,
																		ObjectIdGetDatum(form->oid),
																		BoolGetDatum(false))));
				}
				break;
			case RELKIND_FOREIGN_TABLE:
				uncovered("foreign tables", qualified(form->relnamespace, name));
				break;
			case RELKIND_PARTITIONED_TABLE:
			case RELKIND_PARTITIONED_INDEX:
				uncovered("partitioned tables", qualified(form->relnamespace, name));
				break;
		}
	}
	systable_endscan(scan);
	table_close(pg_class, AccessShareLock);

	/* Versioned schemas themselves, empty ones too. */
	if (cn->folded)
	{
		Relation	pg_ns = table_open(NamespaceRelationId, AccessShareLock);

		scan = systable_beginscan(pg_ns, InvalidOid, false, NULL, 0, NULL);
		while ((tup = systable_getnext(scan)) != NULL)
		{
			Form_pg_namespace ns = (Form_pg_namespace) GETSTRUCT(tup);

			if (canon_namespace(cn, ns->oid))
				(void) add_item(cn, "n:", quote_identifier(NameStr(ns->nspname)));
		}
		systable_endscan(scan);
		table_close(pg_ns, AccessShareLock);
	}

	/* Types. */
	{
		Relation	pg_type = table_open(TypeRelationId, AccessShareLock);

		scan = systable_beginscan(pg_type, InvalidOid, false, NULL, 0, NULL);
		while ((tup = systable_getnext(scan)) != NULL)
		{
			Form_pg_type typ = (Form_pg_type) GETSTRUCT(tup);

			if (!canon_namespace(cn, typ->typnamespace) ||
				in_extension(TypeRelationId, typ->oid))
				continue;
			/* Tables' row types come with their tables. */
			if (typ->typtype == TYPTYPE_COMPOSITE &&
				get_rel_relkind(typ->typrelid) != RELKIND_COMPOSITE_TYPE)
				continue;
			if (OidIsValid(typ->typelem) && typ->typcategory == TYPCATEGORY_ARRAY)
				continue;
			describe_type(cn, typ);
		}
		systable_endscan(scan);
		table_close(pg_type, AccessShareLock);
	}

	/* Functions and procedures, by name and argument types. */
	{
		Relation	pg_proc = table_open(ProcedureRelationId, AccessShareLock);

		scan = systable_beginscan(pg_proc, InvalidOid, false, NULL, 0, NULL);
		while ((tup = systable_getnext(scan)) != NULL)
		{
			Form_pg_proc proc = (Form_pg_proc) GETSTRUCT(tup);
			char	   *name;
			CanonItem  *it;

			if (!canon_namespace(cn, proc->pronamespace) ||
				in_extension(ProcedureRelationId, proc->oid))
				continue;
			name = psprintf("%s(%s)",
							qualified(proc->pronamespace, NameStr(proc->proname)),
							TextDatumGetCString(DirectFunctionCall1(pg_get_function_identity_arguments,
																	ObjectIdGetDatum(proc->oid))));
			if (proc->prokind != PROKIND_FUNCTION &&
				proc->prokind != PROKIND_PROCEDURE)
				uncovered("aggregates and window functions", name);
			it = add_item(cn, "f:", name);
			append_utf8(&it->value,
						TextDatumGetCString(DirectFunctionCall1(pg_get_functiondef,
																ObjectIdGetDatum(proc->oid))));
		}
		systable_endscan(scan);
		table_close(pg_proc, AccessShareLock);
	}
	{
		Relation	pg_policy = table_open(PolicyRelationId, AccessShareLock);

		scan = systable_beginscan(pg_policy, InvalidOid, false, NULL, 0, NULL);
		while ((tup = systable_getnext(scan)) != NULL)
		{
			Form_pg_policy pol = (Form_pg_policy) GETSTRUCT(tup);

			if (canon_namespace(cn, get_rel_namespace(pol->polrelid)))
				uncovered("row security policies", NameStr(pol->polname));
		}
		systable_endscan(scan);
		table_close(pg_policy, AccessShareLock);
	}
	/* Extensions, by name and version (every branch's, if folded). */
	if (!cn->folded)
	{
		Relation	pg_ext = table_open(ExtensionRelationId, AccessShareLock);

		scan = systable_beginscan(pg_ext, InvalidOid, false, NULL, 0, NULL);
		while ((tup = systable_getnext(scan)) != NULL)
		{
			Form_pg_extension ext = (Form_pg_extension) GETSTRUCT(tup);
			bool		isnull;
			Datum		ver = heap_getattr(tup, Anum_pg_extension_extversion,
										   RelationGetDescr(pg_ext), &isnull);
			CanonItem  *it;

			if (strcmp(NameStr(ext->extname), "plpgsql") == 0)
				continue;		/* every database has it */
			it = add_item(cn, "e:", NameStr(ext->extname));
			if (!isnull)
				append_utf8(&it->value, TextDatumGetCString(ver));
		}
		systable_endscan(scan);
		table_close(pg_ext, AccessShareLock);
	}
}

static void
catalogs_prepare(void)
{
	Relation	pg_class = table_open(RelationRelationId, AccessShareLock);
	SysScanDesc scan = systable_beginscan(pg_class, InvalidOid, false, NULL,
										  0, NULL);
	HeapTuple	tup;
	List	   *tables = NIL;
	ListCell   *lc;

	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_class form = (Form_pg_class) GETSTRUCT(tup);

		if (form->relkind == RELKIND_RELATION && is_prolly_am(form->relam) &&
			!is_system_namespace(form->relnamespace))
			tables = lappend_oid(tables, form->oid);
	}
	systable_endscan(scan);
	table_close(pg_class, AccessShareLock);
	foreach(lc, tables)
	{
		Relation	rel = table_open(lfirst_oid(lc), RowExclusiveLock);

		prolly_canonicalize_table(rel);
		table_close(rel, NoLock);
	}
}

static void
catalogs_canonical_root(const ProllyNodeRef *working_set, ProllyNodeRef *root)
{
	Canon		cn;

	memset(&cn, 0, sizeof(cn));
	canon_env(&cn);
	cn.roots = PROLLY_CALL(cn.c.engine, roots_at, working_set);
	build_canonical(&cn, root);
	PROLLY_CALL(cn.c.engine, roots_release, cn.roots);
}

/* The canonical tree, and the data and index trees its items name. */
static void
catalogs_walk(const ProllyNodeRef *root, ProllyTreeVisitNode visit, void *arg)
{
	Canon		cn;
	ProllyTreeCursor *c;
	ProllyKey	k;
	ProllyValue v;
	List	   *trees = NIL;
	ListCell   *lc;

	memset(&cn, 0, sizeof(cn));
	canon_env(&cn);
	PROLLY_CALL(cn.tree, walk, &cn.env, root, visit, arg);
	c = PROLLY_CALL(cn.tree, cursor_open, &cn.env, root, CurrentMemoryContext);
	PROLLY_CALL(cn.tree, cursor_seek, c, NULL, ForwardScanDirection);
	while (PROLLY_CALL(cn.tree, cursor_next, c, &k, &v))
	{
		ProllyNodeRef *ref;

		if (k.len < 2 || (k.data[0] != 't' && k.data[0] != 'i') || v.len < 1 ||
			v.len < 1 + v.data[0])
			continue;
		ref = palloc0(sizeof(ProllyNodeRef));
		ref->hash.len = v.data[0];
		memcpy(ref->hash.bytes, v.data + 1, ref->hash.len);
		ref->location.blkno = InvalidBlockNumber;
		trees = lappend(trees, ref);
	}
	PROLLY_CALL(cn.tree, cursor_close, c);
	foreach(lc, trees)
		PROLLY_CALL(cn.tree, walk, &cn.env, lfirst(lc), visit, arg);
}

/*
 * A collation's name (if name isn't NULL), and what decides the order it gives: its provider,
 * locale, ICU rules (empty if none) and whether it is deterministic, and
 * the version its library reports now (not the one the catalog recorded,
 * which a library upgrade leaves behind), a line each; false if the
 * library reports no version (as for C and POSIX).
 */
static bool
collation_version(Oid coll, StringInfo name, StringInfo value)
{
	HeapTuple	tup;
	char		provider;
	Datum		d;
	bool		isnull;
	char	   *locale;
	char	   *rules = NULL;
	bool		deterministic = true;
	char	   *version;

	if (coll == DEFAULT_COLLATION_OID)
	{
		tup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for database %u", MyDatabaseId);
		provider = ((Form_pg_database) GETSTRUCT(tup))->datlocprovider;
		if (name)
			appendStringInfoString(name, "default");
		locale = TextDatumGetCString(provider == COLLPROVIDER_LIBC ?
									 SysCacheGetAttrNotNull(DATABASEOID, tup, Anum_pg_database_datcollate) :
									 SysCacheGetAttrNotNull(DATABASEOID, tup, Anum_pg_database_datlocale));
		d = SysCacheGetAttr(DATABASEOID, tup, Anum_pg_database_daticurules, &isnull);
		if (!isnull)
			rules = TextDatumGetCString(d);
	}
	else
	{
		Form_pg_collation form;

		tup = SearchSysCache1(COLLOID, ObjectIdGetDatum(coll));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for collation %u", coll);
		form = (Form_pg_collation) GETSTRUCT(tup);
		provider = form->collprovider;
		deterministic = form->collisdeterministic;
		if (name)
			append_utf8(name, qualified(form->collnamespace, NameStr(form->collname)));
		locale = TextDatumGetCString(provider == COLLPROVIDER_LIBC ?
									 SysCacheGetAttrNotNull(COLLOID, tup, Anum_pg_collation_collcollate) :
									 SysCacheGetAttrNotNull(COLLOID, tup, Anum_pg_collation_colllocale));
		d = SysCacheGetAttr(COLLOID, tup, Anum_pg_collation_collicurules, &isnull);
		if (!isnull)
			rules = TextDatumGetCString(d);
	}
	ReleaseSysCache(tup);
	version = get_collation_actual_version(provider, locale);
	if (version == NULL)
		return false;
	appendStringInfo(value, "%c\n", provider);
	append_utf8(value, locale);
	appendStringInfoChar(value, '\n');
	if (rules)
		append_utf8(value, rules);
	appendStringInfo(value, "\n%c\n", deterministic ? 't' : 'f');
	append_utf8(value, version);
	return true;
}

/* Record the collations ordering the folded tables' keys. */
static void
add_collations(Canon *cn)
{
	ListCell   *lc;

	foreach(lc, cn->collations)
	{
		StringInfoData name;
		StringInfoData value;

		initStringInfo(&name);
		initStringInfo(&value);
		if (collation_version(lfirst_oid(lc), &name, &value))
		{
			CanonItem  *it = palloc(sizeof(CanonItem));

			initStringInfo(&it->key);
			appendStringInfoString(&it->key, "c:");
			appendBinaryStringInfo(&it->key, name.data, name.len);
			it->value = value;
			cn->items = lappend(cn->items, it);
		}
	}}

/* A collation's provider, locale, rules and version, for messages. */
static char *
describe_collation(const char *value, int len)
{
	char	   *f[5] = {0};
	char	   *p = pnstrdup(value, len);

	for (int i = 0; i < 5 && p; i++)
	{
		f[i] = p;
		p = strchr(p, '\n');
		if (p)
			*p++ = '\0';
	}
	if (f[4] == NULL)
		return quote_literal_cstr(value);
	return psprintf("provider %s, locale %s%s%s, %sdeterministic, version %s",
					f[0], quote_literal_cstr(f[1]),
					f[2][0] ? ", rules " : "", f[2][0] ? quote_literal_cstr(f[2]) : "",
					f[3][0] == 't' ? "" : "not ", quote_literal_cstr(f[4]));
}

/* The collation a root's "c:" item names: "default", or a qualified name. */
static Oid
collation_named(const char *name)
{
	if (strcmp(name, "default") == 0)
		return DEFAULT_COLLATION_OID;
	return get_collation_oid(stringToQualifiedNameList(name, NULL), true);
}

/*
 * The collations a root recorded that differ now (or are gone): a List of
 * String pairs, the collation's name (as its item names it, in the
 * server's encoding) then how it differs.
 */
static List *
stale_collations(Canon *cn, const ProllyNodeRef *root)
{
	ProllyTreeCursor *c;
	ProllyKey	k;
	ProllyValue v;
	ProllyKey	from = {(const uint8 *) "c:", 2};
	List	   *out = NIL;

	c = PROLLY_CALL(cn->tree, cursor_open, &cn->env, root, CurrentMemoryContext);
	PROLLY_CALL(cn->tree, cursor_seek, c, &from, ForwardScanDirection);
	while (PROLLY_CALL(cn->tree, cursor_next, c, &k, &v))
	{
		char	   *name;
		Oid			coll;
		StringInfoData cur;

		if (k.len < 2 || memcmp(k.data, "c:", 2) != 0)
			break;
		name = pg_any_to_server(pnstrdup((const char *) k.data + 2, k.len - 2),
								k.len - 2, PG_UTF8);
		coll = collation_named(name);
		initStringInfo(&cur);
		if (!OidIsValid(coll) || !collation_version(coll, NULL, &cur) ||
			cur.len != v.len || memcmp(cur.data, v.data, v.len) != 0)
		{
			out = lappend(out, makeString(name));
			out = lappend(out, makeString(psprintf("collation \"%s\" ordered its keys as %s; the database has %s",
												   name, describe_collation((const char *) v.data, v.len),
												   OidIsValid(coll) && cur.len > 0 ?
												   describe_collation(cur.data, cur.len) : "none")));
		}
	}
	PROLLY_CALL(cn->tree, cursor_close, c);
	return out;
}

/*
 * Why the rows of the table the session branch has by this name can't be
 * read from a root whose collations stale_collations() found differing
 * now, or NULL: a collation ordering its keys (as the table has them now)
 * is one of them.  A table the branch doesn't have isn't read.
 */
static const char *
table_unreadable(const char *name, List *stale)
{
	Oid			relid;
	Oid			pkoid;
	Relation	pk;
	const char *why = NULL;

	if (stale == NIL)
		return NULL;
	relid = RangeVarGetRelid(makeRangeVarFromNameList(stringToQualifiedNameList(name, NULL)),
							 NoLock, true);
	if (!OidIsValid(relid) || get_rel_relkind(relid) != RELKIND_RELATION)
		return NULL;
	{
		Relation	rel = table_open(relid, AccessShareLock);

		pkoid = RelationGetPrimaryKeyIndex(rel, false);
		table_close(rel, AccessShareLock);
	}
	if (!OidIsValid(pkoid))
		return NULL;
	pk = index_open(pkoid, AccessShareLock);
	for (int i = 0; i < IndexRelationGetNumberOfKeyAttributes(pk) && !why; i++)
	{
		Oid			coll = pk->rd_indcollation[i];
		StringInfoData cname;

		if (!OidIsValid(coll))
			continue;
		initStringInfo(&cname);
		if (coll == DEFAULT_COLLATION_OID)
			appendStringInfoString(&cname, "default");
		else
		{
			HeapTuple	tup = SearchSysCache1(COLLOID, ObjectIdGetDatum(coll));
			Form_pg_collation form;

			if (!HeapTupleIsValid(tup))
				elog(ERROR, "cache lookup failed for collation %u", coll);
			form = (Form_pg_collation) GETSTRUCT(tup);
			appendStringInfoString(&cname, qualified(form->collnamespace,
													 NameStr(form->collname)));
			ReleaseSysCache(tup);
		}
		for (int j = 0; j + 1 < list_length(stale); j += 2)
			if (strcmp(strVal(list_nth(stale, j)), cname.data) == 0)
				why = strVal(list_nth(stale, j + 1));
	}
	index_close(pk, AccessShareLock);
	return why;
}

/* The canonical tree, built in the environment from the items collected. */
static void
build_canonical(Canon *cn, ProllyNodeRef *root)
{
	int			save_nestlevel;
	ProllyTreeBuilder *b;
	ListCell   *lc;

	/* Deparse with every name qualified, whatever the session's settings. */
	save_nestlevel = NewGUCNestLevel();
	(void) set_config_option("search_path", "pg_catalog", PGC_USERSET,
							 PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	collect(cn);
	AtEOXact_GUC(false, save_nestlevel);
	add_collations(cn);		/* (none unless folded) */

	list_sort(cn->items, item_cmp);
	b = PROLLY_CALL(cn->tree, build_begin, &cn->env, CurrentMemoryContext);
	foreach(lc, cn->items)
	{
		CanonItem  *it = lfirst(lc);
		ProllyKey	k = {(const uint8 *) it->key.data, it->key.len};
		ProllyValue v = {(const uint8 *) it->value.data, it->value.len};

		PROLLY_CALL(cn->tree, build_add, b, &k, &v);
	}
	PROLLY_CALL(cn->tree, build_finish, b, root);
}

static void
folded_fold(ProllyNodeRef *root)
{
	Canon		cn;

	memset(&cn, 0, sizeof(cn));
	cn.folded = true;
	canon_env(&cn);

	/* Everything the transaction wrote, this statement's writes too. */
	CommandCounterIncrement();
	PushCopiedSnapshot(GetActiveSnapshot());
	UpdateActiveSnapshotCommandId();
	PG_TRY();
	{
		build_canonical(&cn, root);
	}
	PG_FINALLY();
	{
		PopActiveSnapshot();
	}
	PG_END_TRY();
}

/*
 * A folded item's data tree and the rest of its value (its definition):
 * a table's or materialized view's value starts with the tree (hash
 * length, hash).  False for other items, whose value is all definition.
 */
static bool
tree_item(const ProllyKey *k, const ProllyValue *v, ProllyNodeRef *data,
		  ProllyValue *def)
{
	*def = *v;
	if (k->len < 2 || k->data[1] != ':' ||
		(k->data[0] != 't' && k->data[0] != 'm'))
		return false;
	if (v->len < 1 || v->data[0] > PROLLY_HASH_MAX_LEN ||
		v->len < 1 + v->data[0])
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("canonical tree holds an invalid entry for \"%s\"",
						pnstrdup((const char *) k->data + 2, k->len - 2))));
	if (data)
	{
		memset(data, 0, sizeof(*data));
		data->hash.len = v->data[0];
		memcpy(data->hash.bytes, v->data + 1, data->hash.len);
		data->location.blkno = InvalidBlockNumber;
	}
	def->data = v->data + 1 + v->data[0];
	def->len = v->len - 1 - v->data[0];
	return true;
}

/* The canonical tree, and the data trees its tables and materialized views name. */
static void
folded_walk(const ProllyNodeRef *root, ProllyTreeVisitNode visit, void *arg)
{
	Canon		cn;
	ProllyTreeCursor *c;
	ProllyKey	k;
	ProllyValue v;
	List	   *trees = NIL;
	ListCell   *lc;

	memset(&cn, 0, sizeof(cn));
	cn.folded = true;
	canon_env(&cn);
	PROLLY_CALL(cn.tree, walk, &cn.env, root, visit, arg);
	c = PROLLY_CALL(cn.tree, cursor_open, &cn.env, root, CurrentMemoryContext);
	PROLLY_CALL(cn.tree, cursor_seek, c, NULL, ForwardScanDirection);
	while (PROLLY_CALL(cn.tree, cursor_next, c, &k, &v))
	{
		ProllyNodeRef *ref = palloc0(sizeof(ProllyNodeRef));
		ProllyValue def;

		if (tree_item(&k, &v, ref, &def))
			trees = lappend(trees, ref);
		else
			pfree(ref);
	}
	PROLLY_CALL(cn.tree, cursor_close, c);
	foreach(lc, trees)
		PROLLY_CALL(cn.tree, walk, &cn.env, lfirst(lc), visit, arg);
}

/*
 * The items of a folded root of one kind ("t:" or "m:"), each with its
 * data tree and definition; tables' rows may be unreadable here (their
 * keys ordered by a collation that differs now).
 */
static void
folded_trees(const ProllyNodeRef *root, char kind, ProllyCanonTableVisit visit,
			 void *arg)
{
	Canon		cn;
	ProllyTreeCursor *c;
	ProllyKey	k;
	ProllyValue v;
	char		prefix[2] = {kind, ':'};
	ProllyKey	from = {(const uint8 *) prefix, 2};
	List	   *stale = NIL;

	memset(&cn, 0, sizeof(cn));
	cn.folded = true;
	canon_env(&cn);
	if (kind == 't')
		stale = stale_collations(&cn, root);
	c = PROLLY_CALL(cn.tree, cursor_open, &cn.env, root, CurrentMemoryContext);
	PROLLY_CALL(cn.tree, cursor_seek, c, &from, ForwardScanDirection);
	while (PROLLY_CALL(cn.tree, cursor_next, c, &k, &v))
	{
		ProllyNodeRef data;
		ProllyValue description;
		char	   *name;

		if (k.len < 2 || memcmp(k.data, prefix, 2) != 0)
			break;
		(void) tree_item(&k, &v, &data, &description);
		name = pnstrdup((const char *) k.data + 2, k.len - 2);
		visit(name, &data, &description,
			  kind == 't' ?
			  table_unreadable(pg_any_to_server(name, strlen(name), PG_UTF8), stale) :
			  NULL,
			  arg);
		pfree(name);
	}
	PROLLY_CALL(cn.tree, cursor_close, c);
}

/* The tables of a folded root: its "t:" items. */
static void
folded_tables(const ProllyNodeRef *root, ProllyCanonTableVisit visit,
			  void *arg)
{
	folded_trees(root, 't', visit, arg);
}

/* Its materialized views: its "m:" items. */
static void
folded_materialized_views(const ProllyNodeRef *root,
						  ProllyCanonTableVisit visit, void *arg)
{
	folded_trees(root, 'm', visit, arg);
}

/* An item's value without the data tree a table's or view's starts with. */
static void
definition_of(const ProllyKey *k, const ProllyValue *v, ProllyValue *def)
{
	(void) tree_item(k, v, NULL, def);
}

/*
 * The next item that defines an object: collations' items are left out,
 * as what the fold notes of the tables (checked when rows are read).
 */
static bool
next_object(Canon *cn, ProllyTreeCursor *c, ProllyKey *k, ProllyValue *v)
{
	while (PROLLY_CALL(cn->tree, cursor_next, c, k, v))
		if (!(k->len >= 2 && k->data[0] == 'c' && k->data[1] == ':'))
			return true;
	return false;
}

/* The same objects, the same values but for tables' and views' rows. */
static bool
folded_same_schema(const ProllyNodeRef *a, const ProllyNodeRef *b)
{
	Canon		cn;
	ProllyTreeCursor *ca;
	ProllyTreeCursor *cb;
	bool		same = true;

	memset(&cn, 0, sizeof(cn));
	cn.folded = true;
	canon_env(&cn);
	ca = PROLLY_CALL(cn.tree, cursor_open, &cn.env, a, CurrentMemoryContext);
	cb = PROLLY_CALL(cn.tree, cursor_open, &cn.env, b, CurrentMemoryContext);
	PROLLY_CALL(cn.tree, cursor_seek, ca, NULL, ForwardScanDirection);
	PROLLY_CALL(cn.tree, cursor_seek, cb, NULL, ForwardScanDirection);
	for (;;)
	{
		ProllyKey	ka,
					kb;
		ProllyValue va,
					vb,
					da,
					db;
		bool		more_a = next_object(&cn, ca, &ka, &va);
		bool		more_b = next_object(&cn, cb, &kb, &vb);

		if (more_a != more_b)
		{
			same = false;
			break;
		}
		if (!more_a)
			break;
		definition_of(&ka, &va, &da);
		definition_of(&kb, &vb, &db);
		if (ka.len != kb.len || memcmp(ka.data, kb.data, ka.len) != 0 ||
			da.len != db.len || memcmp(da.data, db.data, da.len) != 0)
		{
			same = false;
			break;
		}
	}
	PROLLY_CALL(cn.tree, cursor_close, ca);
	PROLLY_CALL(cn.tree, cursor_close, cb);
	return same;
}

const ProllyCanonRoutine prolly_canon_folded = {
	.info = {
		.iface = PROLLY_IFACE_CANONICALIZER,
		.impl_id = 2,
		.format_version = 2,		/* 2: collations' rules and flag, by library */
		.name = "folded",
		.description = "canonical form by name of the session branch's "
		"versioned schemas, their tables' rows folded into trees",
	},
	.walk = folded_walk,
	.fold = folded_fold,
	.tables = folded_tables,
	.materialized_views = folded_materialized_views,
	.same_schema = folded_same_schema,
};

const ProllyCanonRoutine prolly_canon_catalogs = {
	.info = {
		.iface = PROLLY_IFACE_CANONICALIZER,
		.impl_id = 1,
		.format_version = 1,
		.name = "catalogs",
		.description = "canonical form by name, from the catalogs and the "
		"working set's trees",
	},
	.prepare = catalogs_prepare,
	.canonical_root = catalogs_canonical_root,
	.walk = catalogs_walk,
};
