/*-------------------------------------------------------------------------
 *
 * canon_catalogs.c
 *	  Canonicalizer implementation "catalogs": a working set's canonical
 *	  form, derived from the database's catalogs, by name.
 *
 * The canonical root is a tree keyed (by memcmp) by kind and qualified
 * name, every string in UTF-8:
 *
 *	"!:format"			-> (folded only) the format its data trees are in,
 *						   FOLDED_FORMAT
 *	"e:" extension		-> its version
 *	"f:" function(args)	-> its definition
 *	"i:" index			-> a prolly index's tree (hash length, hash)
 *	"m:" materialized view -> its data tree (hash length, hash), then its
 *						   query
 *	"n:" schema			-> nothing: a versioned schema (folded only), there
 *						   whether or not anything is in it
 *	"s:" sequence		-> its definition; in folded roots then, a line of
 *						   its own, its value: the last value, and whether it
 *						   was given out ("t" or "f")
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
 * In the other direction, move_schema() turns the branch's definitions into
 * a root's, with the statements Postgres's deparser printed for them:
 * tables made, dropped or altered (columns added, dropped, retyped, given
 * other defaults; constraints, indexes, triggers and rules by name),
 * routines and views made, replaced or dropped, types and sequences made or
 * dropped.  Primary keys, which name the rows, and NOT NULL come before
 * the caller writes the rows; what else they must satisfy (constraints,
 * indexes, triggers) after.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/canon_catalogs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "nodes/makefuncs.h"
#include "catalog/pg_attribute.h"
#include "parser/parse_type.h"
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
#include "catalog/objectaddress.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_attrdef.h"
#include "catalog/index.h"
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
#include "commands/sequence.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "rewrite/rewriteSupport.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
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

typedef struct BaseTable BaseTable;

/*
 * The format a folded root's data trees are in: "2", keyless rows keyed by
 * their first bytes and their hash.  A root in another (one without the
 * item was keyed by the hash alone) has its tables' rows unreadable here.
 */
#define FOLDED_FORMAT	"2"

typedef struct Canon
{
	ProllyComponents c;
	ProllyTreeEnv env;
	ProllyRootSet *roots;
	List	   *items;
	bool		folded;			/* the session branch's versioned schemas */
	const ProllyTreeRoutine *tree;
	List	   *collations;		/* OIDs of the collations ordering keys */

	/* folded: the branch's baseline, and the next one if it is made */
	const ProllyChangeFinderRoutine *finder;
	BaseTable  *base;			/* in key order */
	int			nbase;
	int			maxbase;
	bool		advance;
	List	   *next;			/* CanonItem *, the next baseline's */
	bool		reading;		/* objects not described are left out */
	List	   *left_out;		/* (reading) their names */
	Oid			only;			/* valid: only this table's rows folded */
} Canon;

static void build_canonical(Canon *cn, ProllyNodeRef *root);

void
prolly_folded_tree_env(ProllyTreeEnv *env)
{
	memset(env, 0, sizeof(*env));
	env->chunk_format = prolly_chunkformat_lookup("basic");
	env->chunker = prolly_chunker_lookup("bytes");
	env->hash = prolly_hash_lookup("sha256");
	env->chunk_store = prolly_chunk_store();
	env->chunks = PROLLY_CALL(env->chunk_store, open, MyDatabaseId,
							  MyDatabaseTableSpace);
	env->compare = prolly_bytes_compare;
}

static void
canon_env(Canon *cn)
{
	cn->tree = prolly_tree_lookup("cow");
	if (cn->folded)
	{
		prolly_folded_tree_env(&cn->env);
		return;
	}
	memset(&cn->env, 0, sizeof(cn->env));
	prolly_components_for_engine(&prolly_engine_d1.info, &cn->c);
	cn->env.node_format = cn->c.node_format;
	cn->env.chunker = cn->c.chunker;
	cn->env.hash = cn->c.hash;
	cn->env.node_store = cn->c.node_store;
	cn->env.store = PROLLY_CALL(cn->c.node_store, open, NULL);
	cn->env.log = cn->c.log;
	cn->env.compare = prolly_bytes_compare;
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

/*
 * An object the canonical form doesn't describe: refused, or, when only
 * reading, left out (true).
 */
static bool
uncovered(Canon *cn, const char *what, const char *name)
{
	if (cn->reading)
	{
		cn->left_out = lappend(cn->left_out, pstrdup(name));
		return true;
	}
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("prolly commits don't cover %s yet: %s", what, name),
			 errdetail("A commit's root hash must depend only on what it holds, and the canonical form doesn't describe this object.")));
}

/* ------------------------------------------------------------------------
 * Tables
 * ------------------------------------------------------------------------
 */

/* A table's baseline: what its last fold for a commit left. */
struct BaseTable
{
	StringInfoData key;			/* its item's key */
	ProllyNodeRef data;
	ProllyHash	description;	/* of its definition */
	StringInfoData state;		/* the change finder's */
};

static void
sha256(Canon *cn, const void *data, Size len, ProllyHash *h)
{
	const ProllyHashRoutine *r = cn->env.hash;
	ProllyHashCtx *ctx = PROLLY_CALL(r, begin, CurrentMemoryContext);

	PROLLY_CALL(r, update, ctx, data, len);
	PROLLY_CALL(r, finish, ctx, h);
}

static int
base_table_cmp(const void *a, const void *b)
{
	const BaseTable *x = a;
	const BaseTable *y = b;
	ProllyKey	kx = {(const uint8 *) x->key.data, x->key.len};
	ProllyKey	ky = {(const uint8 *) y->key.data, y->key.len};

	return prolly_bytes_compare(&kx, &ky, NULL);
}

/*
 * A baseline's value: the table's data tree and the hash of its
 * definition (each a length byte, then the hash), then the finder's state.
 */
static bool
parse_base_table(const ProllyValue *v, BaseTable *bt)
{
	const uint8 *p = v->data;
	const uint8 *end = v->data + v->len;

	memset(&bt->data, 0, sizeof(bt->data));
	memset(&bt->description, 0, sizeof(bt->description));
	if (p >= end || p[0] > PROLLY_HASH_MAX_LEN || end - p < 1 + p[0] + 1)
		return false;
	bt->data.hash.len = p[0];
	memcpy(bt->data.hash.bytes, p + 1, p[0]);
	bt->data.location.blkno = InvalidBlockNumber;
	p += 1 + p[0];
	if (p[0] > PROLLY_HASH_MAX_LEN || end - p < 1 + p[0])
		return false;
	bt->description.len = p[0];
	memcpy(bt->description.bytes, p + 1, p[0]);
	p += 1 + p[0];
	initStringInfo(&bt->state);
	appendBinaryStringInfo(&bt->state, p, end - p);
	return true;
}

/* Read a baseline's tables, in key order. */
static void
load_baseline(Canon *cn, const ProllyNodeRef *baseline)
{
	ProllyTreeCursor *c;
	ProllyKey	k;
	ProllyValue v;

	c = PROLLY_CALL(cn->tree, cursor_open, &cn->env, baseline, CurrentMemoryContext);
	PROLLY_CALL(cn->tree, cursor_seek, c, NULL, ForwardScanDirection);
	while (PROLLY_CALL(cn->tree, cursor_next, c, &k, &v))
	{
		BaseTable  *bt;

		if (cn->nbase == cn->maxbase)
		{
			cn->maxbase = Max(16, cn->maxbase * 2);
			cn->base = cn->base ? repalloc_array(cn->base, BaseTable, cn->maxbase) :
				palloc_array(BaseTable, cn->maxbase);
		}
		bt = &cn->base[cn->nbase];
		initStringInfo(&bt->key);
		appendBinaryStringInfo(&bt->key, k.data, k.len);
		if (!parse_base_table(&v, bt))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("a branch's baseline holds an invalid table entry")));
		cn->nbase++;
	}
	PROLLY_CALL(cn->tree, cursor_close, c);
}

static BaseTable *
find_base_table(Canon *cn, const StringInfoData *key)
{
	BaseTable	probe;

	if (cn->nbase == 0)
		return NULL;
	probe.key = *key;
	return bsearch(&probe, cn->base, cn->nbase, sizeof(BaseTable), base_table_cmp);
}

/*
 * A table's data tree, folded from its rows as the transaction sees them by
 * the change finder: from its baseline if it has one for this definition
 * (described in 'description'); with a next baseline being made, its entry
 * there too.  The same rows give the same tree whatever pages, order or
 * history they come from.
 */
static void
folded_table_root(Canon *cn, CanonItem *it, Relation rel,
				  const StringInfoData *description, ProllyNodeRef *root)
{
	ProllyHash	dh;
	BaseTable  *bt;
	ProllyTableFold tf;
	StringInfoData state;
	Oid			pkoid = RelationGetPrimaryKeyIndex(rel, false);

	if (OidIsValid(pkoid))
	{
		/* (the keying's key: the primary key, as it chooses it) */
		Relation	pk = index_open(pkoid, AccessShareLock);

		for (int i = 0; i < IndexRelationGetNumberOfKeyAttributes(pk); i++)
		{
			if (OidIsValid(pk->rd_indcollation[i]))
				cn->collations = list_append_unique_oid(cn->collations,
														pk->rd_indcollation[i]);
		}
		index_close(pk, AccessShareLock);
	}

	sha256(cn, description->data, description->len, &dh);
	bt = find_base_table(cn, &it->key);
	if (bt && (bt->description.len != dh.len ||
			   memcmp(bt->description.bytes, dh.bytes, dh.len) != 0))
		bt = NULL;
	if (bt)
	{
		tf.data = bt->data;
		tf.state.data = (const uint8 *) bt->state.data;
		tf.state.len = bt->state.len;
	}
	/* (when reading another table: its rows as the baseline has them) */
	if (OidIsValid(cn->only) && RelationGetRelid(rel) != cn->only)
	{
		if (bt)
			*root = bt->data;
		else
		{
			ProllyTreeBuilder *b = PROLLY_CALL(cn->tree, build_begin, &cn->env,
											   CurrentMemoryContext);

			PROLLY_CALL(cn->tree, build_finish, b, root);
		}
		return;
	}
	initStringInfo(&state);
	PROLLY_CALL(cn->finder, fold, rel, &cn->env, bt ? &tf : NULL, root,
				cn->advance ? &state : NULL);
	if (cn->advance)
	{
		CanonItem  *next = palloc0(sizeof(CanonItem));

		initStringInfo(&next->key);
		appendBinaryStringInfo(&next->key, it->key.data, it->key.len);
		initStringInfo(&next->value);
		put_hash(&next->value, &root->hash);
		put_hash(&next->value, &dh);
		appendBinaryStringInfo(&next->value, state.data, state.len);
		cn->next = lappend(cn->next, next);
	}
	pfree(state.data);
}

/* A table's data tree: folded from its rows, or its working set's. */
static void
table_root(Canon *cn, CanonItem *it, Relation rel,
		   const StringInfoData *description, ProllyNodeRef *root)
{
	if (cn->folded)
		folded_table_root(cn, it, rel, description, root);
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
		(void) uncovered(cn, "this type", qualified(typ->typnamespace, NameStr(typ->typname)));
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

	/*
	 * A branch's sequences are its data too: read as the fold reads rows,
	 * whatever the session may read.  One that can't be read here has no
	 * value recorded (and a move leaves it as it is).
	 */
	if (cn->folded)
	{
		int64		last;
		bool		called;

		if (ReadSequenceState(seqid, &last, &called))
			appendStringInfo(&it->value, INT64_FORMAT "\t%s\n", last,
							 called ? "t" : "f");
	}
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

	return prolly_bytes_compare(&ka, &kb, NULL);
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
				/* (a partition goes with its partitioned table) */
				if (form->relispartition)
				{
					(void) uncovered(cn, "partitions of partitioned tables",
									 qualified(form->relnamespace, name));
					break;
				}
				if (versioned_am(cn, form->relam))
				{
					Relation	rel = table_open(form->oid, AccessShareLock);
					CanonItem  *it = add_item(cn, "t:", qualified(form->relnamespace, name));
					ProllyNodeRef root;
					StringInfoData desc;

					initStringInfo(&desc);
					describe_table(cn, rel, &desc);
					table_root(cn, it, rel, &desc, &root);
					put_hash(&it->value, &root.hash);
					appendBinaryStringInfo(&it->value, desc.data, desc.len);
					pfree(desc.data);
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
					StringInfoData query;

					initStringInfo(&query);
					append_utf8(&query,
								TextDatumGetCString(DirectFunctionCall2(pg_get_viewdef_ext,
																		ObjectIdGetDatum(form->oid),
																		BoolGetDatum(false))));
					if (versioned_am(cn, form->relam))
					{
						Relation	rel = table_open(form->oid, AccessShareLock);
						ProllyNodeRef root;

						table_root(cn, it, rel, &query, &root);
						put_hash(&it->value, &root.hash);
						table_close(rel, AccessShareLock);
					}
					appendBinaryStringInfo(&it->value, query.data, query.len);
					pfree(query.data);
				}
				break;
			case RELKIND_FOREIGN_TABLE:
				(void) uncovered(cn, "foreign tables", qualified(form->relnamespace, name));
				break;
			case RELKIND_PARTITIONED_TABLE:
			case RELKIND_PARTITIONED_INDEX:
				(void) uncovered(cn, "partitioned tables", qualified(form->relnamespace, name));
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
				proc->prokind != PROKIND_PROCEDURE &&
				uncovered(cn, "aggregates and window functions", name))
				continue;
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
				(void) uncovered(cn, "row security policies",
								 psprintf("%s on %s",
										  quote_identifier(NameStr(pol->polname)),
										  qualified(get_rel_namespace(pol->polrelid),
													get_rel_name(pol->polrelid))));
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
	if (cn->folded)
		appendStringInfoString(&add_item(cn, "!:", "format")->value, FOLDED_FORMAT);

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

/* The next baseline: each table's entry, as a tree by key. */
static void
build_baseline(Canon *cn, ProllyNodeRef *next)
{
	ProllyTreeBuilder *b;
	ListCell   *lc;

	list_sort(cn->next, item_cmp);
	b = PROLLY_CALL(cn->tree, build_begin, &cn->env, CurrentMemoryContext);
	foreach(lc, cn->next)
	{
		CanonItem  *it = lfirst(lc);
		ProllyKey	k = {(const uint8 *) it->key.data, it->key.len};
		ProllyValue v = {(const uint8 *) it->value.data, it->value.len};

		PROLLY_CALL(cn->tree, build_add, b, &k, &v);
	}
	PROLLY_CALL(cn->tree, build_finish, b, next);
}

static List *
fold_canon(const ProllyNodeRef *baseline, ProllyNodeRef *root,
		   ProllyNodeRef *next_baseline, bool reading, Oid only)
{
	Canon		cn;

	/* (a baseline holds every table's state: never made from one alone) */
	Assert(!OidIsValid(only) || next_baseline == NULL);
	memset(&cn, 0, sizeof(cn));
	cn.folded = true;
	cn.reading = reading;
	cn.only = only;
	canon_env(&cn);
	cn.finder = prolly_changes_lookup("pages");
	cn.advance = next_baseline != NULL;
	if (baseline)
		load_baseline(&cn, baseline);

	/*
	 * Everything the transaction wrote, this statement's writes too: a
	 * snapshot that sees them.  (In parallel mode nothing was written, and
	 * the active snapshot is it; without one, the transaction's.)
	 */
	if (!IsInParallelMode())
		CommandCounterIncrement();
	if (ActiveSnapshotSet())
		PushCopiedSnapshot(GetActiveSnapshot());
	else
		PushActiveSnapshot(GetTransactionSnapshot());
	if (!IsInParallelMode())
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
	if (next_baseline)
		build_baseline(&cn, next_baseline);
	return cn.left_out;
}

static void
folded_fold(const ProllyNodeRef *baseline, ProllyNodeRef *root,
			ProllyNodeRef *next_baseline)
{
	(void) fold_canon(baseline, root, next_baseline, false, InvalidOid);
}

static void
folded_read_fold(const ProllyNodeRef *baseline, Oid only, ProllyNodeRef *root,
				 List **left_out)
{
	List	   *l = fold_canon(baseline, root, NULL, true, only);

	if (left_out)
		*left_out = list_concat(*left_out, l);
}

/* A baseline's tree, its tables' data trees and the finder's states. */
static void
folded_walk_baseline(const ProllyNodeRef *baseline, ProllyTreeVisitNode visit,
					 void *arg)
{
	Canon		cn;

	memset(&cn, 0, sizeof(cn));
	cn.folded = true;
	canon_env(&cn);
	cn.finder = prolly_changes_lookup("pages");
	PROLLY_CALL(cn.tree, walk, &cn.env, baseline, visit, arg);
	load_baseline(&cn, baseline);
	for (int i = 0; i < cn.nbase; i++)
	{
		ProllyValue state = {(const uint8 *) cn.base[i].state.data,
		cn.base[i].state.len};

		PROLLY_CALL(cn.tree, walk, &cn.env, &cn.base[i].data, visit, arg);
		PROLLY_CALL(cn.finder, walk, &cn.env, &state, visit, arg);
	}
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
/* Is a folded root in the current format (its "!:format" item)? */
static bool
root_in_format(Canon *cn, const ProllyNodeRef *root)
{
	ProllyTreeCursor *c;
	ProllyKey	want = {(const uint8 *) "!:format", 8};
	ProllyKey	k;
	ProllyValue v;
	bool		current;

	c = PROLLY_CALL(cn->tree, cursor_open, &cn->env, root, CurrentMemoryContext);
	PROLLY_CALL(cn->tree, cursor_seek, c, &want, ForwardScanDirection);
	current = PROLLY_CALL(cn->tree, cursor_next, c, &k, &v) &&
		k.len == want.len && memcmp(k.data, want.data, want.len) == 0 &&
		v.len == strlen(FOLDED_FORMAT) &&
		memcmp(v.data, FOLDED_FORMAT, v.len) == 0;
	PROLLY_CALL(cn->tree, cursor_close, c);
	return current;
}

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

	bool		current;

	memset(&cn, 0, sizeof(cn));
	cn.folded = true;
	canon_env(&cn);
	if (kind == 't')
		stale = stale_collations(&cn, root);
	current = root_in_format(&cn, root);
	if (!current && kind != 't')
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialized views' rows of a commit made by an older format can't be read"),
				 errhint("Commit the branch to write it in the current format.")));
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
			  kind != 't' ? NULL :
			  !current ? "it was committed in an older format of prolly's history (commit the branch to write it in the current one)" :
			  table_unreadable(pg_any_to_server(name, strlen(name), PG_UTF8), stale),
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
static char *from_utf8(const uint8 *data, int len);

/* (and without a sequence's value) */
static void
definition_of(const ProllyKey *k, const ProllyValue *v, ProllyValue *def)
{
	if (tree_item(k, v, NULL, def))
		return;
	if (k->len >= 2 && k->data[0] == 's' && k->data[1] == ':')
	{
		const uint8 *nl = memchr(v->data, '\n', v->len);

		if (nl)
			def->len = nl + 1 - v->data;
	}
}

/* The sequences of a folded root, with their values. */
static void
folded_sequences(const ProllyNodeRef *root, ProllyCanonSequenceVisit visit,
				 void *arg)
{
	Canon		cn;
	ProllyTreeCursor *c;
	ProllyKey	k;
	ProllyValue v;
	ProllyKey	from = {(const uint8 *) "s:", 2};

	memset(&cn, 0, sizeof(cn));
	cn.folded = true;
	canon_env(&cn);
	c = PROLLY_CALL(cn.tree, cursor_open, &cn.env, root, CurrentMemoryContext);
	PROLLY_CALL(cn.tree, cursor_seek, c, &from, ForwardScanDirection);
	while (PROLLY_CALL(cn.tree, cursor_next, c, &k, &v))
	{
		char	   *value;
		char	   *nl;
		char	   *tab;
		char	   *name;

		if (k.len < 2 || memcmp(k.data, "s:", 2) != 0)
			break;
		value = pnstrdup((const char *) v.data, v.len);
		nl = strchr(value, '\n');
		if (nl == NULL || nl[1] == '\0')
			continue;			/* no value: unreadable, or an older commit */
		tab = strchr(nl + 1, '\t');
		if (tab == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("canonical tree holds a sequence with an invalid value")));
		name = from_utf8(k.data + 2, k.len - 2);
		visit(name, strtoi64(nl + 1, NULL, 10), tab[1] == 't', arg);
	}
	PROLLY_CALL(cn.tree, cursor_close, c);
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

/* ------------------------------------------------------------------------
 * Moving definitions (folded)
 * ------------------------------------------------------------------------
 */

/* An item of a folded root: its kind, name and definition (no rows). */
typedef struct MoveItem
{
	char		kind;
	char	   *name;			/* qualified, in the server's encoding */
	char	   *def;			/* likewise */
	ProllyHash	data;			/* a table's or materialized view's rows */
} MoveItem;

/* A table's definition, as describe_table() lists it. */
typedef struct TableDef
{
	List	   *columns;		/* lines: name, then tab-separated attributes */
	List	   *constraints;	/* lines: name, a tab, the definition */
	List	   *indexes;		/* statements */
	List	   *triggers;		/* statements */
	List	   *rules;			/* statements */
} TableDef;

typedef struct SchemaMove
{
	List	   *before;			/* statements before the rows are written */
	List	   *changes;		/* before too, once the objects they may use
								 * are made: tables made, columns added and
								 * changed, keys added */
	List	   *after;			/* and after */
	List	   *foreign_keys;	/* after, last */
	List	   *rekeyed;		/* tables whose keys change in place */
} SchemaMove;

static char *
from_utf8(const uint8 *data, int len)
{
	return pg_any_to_server(pnstrdup((const char *) data, len), len, PG_UTF8);
}

/* The items of a folded root, in key order; collations aren't objects. */
static List *
move_items(Canon *cn, const ProllyNodeRef *root)
{
	ProllyTreeCursor *c;
	ProllyKey	k;
	ProllyValue v;
	List	   *items = NIL;

	c = PROLLY_CALL(cn->tree, cursor_open, &cn->env, root, CurrentMemoryContext);
	PROLLY_CALL(cn->tree, cursor_seek, c, NULL, ForwardScanDirection);
	while (PROLLY_CALL(cn->tree, cursor_next, c, &k, &v))
	{
		MoveItem   *it;
		ProllyValue def;

		if (k.len < 2 || k.data[1] != ':' || k.data[0] == 'c')
			continue;
		definition_of(&k, &v, &def);
		it = palloc0(sizeof(MoveItem));
		if ((k.data[0] == 't' || k.data[0] == 'm') && def.data != v.data)
		{
			it->data.len = v.data[0];
			memcpy(it->data.bytes, v.data + 1, v.data[0]);
		}
		it->kind = (char) k.data[0];
		it->name = from_utf8(k.data + 2, k.len - 2);
		it->def = from_utf8(def.data, def.len);
		items = lappend(items, it);
	}
	PROLLY_CALL(cn->tree, cursor_close, c);
	return items;
}

static MoveItem *
find_item(List *items, char kind, const char *name)
{
	ListCell   *lc;

	foreach(lc, items)
	{
		MoveItem   *it = lfirst(lc);

		if (it->kind == kind && strcmp(it->name, name) == 0)
			return it;
	}
	return NULL;
}

static void
parse_table_def(const char *def, TableDef *td)
{
	char	   *copy = pstrdup(def);
	List	  **section = NULL;
	char	   *line;
	char	   *next;

	memset(td, 0, sizeof(*td));
	for (line = copy; line && *line; line = next)
	{
		next = strchr(line, '\n');
		if (next)
			*next++ = '\0';
		if (strcmp(line, "columns") == 0)
			section = &td->columns;
		else if (strcmp(line, "constraints") == 0)
			section = &td->constraints;
		else if (strcmp(line, "indexes") == 0)
			section = &td->indexes;
		else if (strcmp(line, "triggers") == 0)
			section = &td->triggers;
		else if (strcmp(line, "rules") == 0)
			section = &td->rules;
		else if (section && *section != NIL && strchr(line, '\t') == NULL &&
				 (section == &td->columns || section == &td->constraints))
		{
			/*
			 * A column's or constraint's line is its name, a tab and the
			 * rest: one without a tab goes on the one before (an
			 * expression's literal with a newline in it).
			 */
			ListCell   *last = list_last_cell(*section);

			lfirst(last) = psprintf("%s\n%s", (char *) lfirst(last), line);
		}
		else if (section)
			*section = lappend(*section, pstrdup(line));
	}
}

static char *first_field(const char *line);
static char *unquoted(const char *ident);

/*
 * A table's description read back: its columns (each line's first field,
 * its name, and second, its type), and whether a primary key, not
 * deferrable, is among its constraints.
 */
static List *
folded_table_columns(const ProllyValue *description, bool *keyed)
{
	TableDef	td;
	List	   *cols = NIL;
	ListCell   *lc;

	parse_table_def(from_utf8(description->data, description->len), &td);
	foreach(lc, td.columns)
	{
		const char *line = lfirst(lc);
		const char *type = strchr(line, '\t');
		ProllyTableColumn *c = palloc0(sizeof(ProllyTableColumn));

		c->name = unquoted(first_field(line));
		c->type = type ? first_field(type + 1) : pstrdup("");
		cols = lappend(cols, c);
	}
	*keyed = false;
	foreach(lc, td.constraints)
	{
		const char *def = strchr(lfirst(lc), '\t');

		/*
		 * (as the keying has it: a deferrable key, or one over a range
		 * WITHOUT OVERLAPS, which the key encoder can't order, doesn't key
		 * the rows)
		 */
		if (def && strncmp(def + 1, "PRIMARY KEY", 11) == 0 &&
			strstr(def, "DEFERRABLE") == NULL &&
			strstr(def, "WITHOUT OVERLAPS") == NULL)
			*keyed = true;
	}
	return cols;
}

/* The first tab-separated field of a line: a column's or constraint's name. */
static char *
first_field(const char *line)
{
	return pnstrdup(line, strcspn(line, "\t"));
}

/* An index statement's name: CREATE [UNIQUE] INDEX name ON ... */
static char *
index_name(const char *stmt)
{
	const char *p = strstr(stmt, " INDEX ");

	if (p == NULL)
		elog(ERROR, "unexpected index definition \"%s\"", stmt);
	p += strlen(" INDEX ");
	return pnstrdup(p, strcspn(p, " "));
}

/* A trigger's or rule's name: CREATE [CONSTRAINT] TRIGGER name ..., CREATE RULE name ... */
static char *
object_name(const char *stmt, const char *word)
{
	const char *p = strstr(stmt, word);

	if (p == NULL)
		elog(ERROR, "unexpected definition \"%s\"", stmt);
	p += strlen(word);
	return pnstrdup(p, strcspn(p, " "));
}

/* Where s is in list, or -1. */
static int
list_member_index_string(List *list, const char *s)
{
	ListCell   *lc;

	foreach(lc, list)
		if (strcmp(lfirst(lc), s) == 0)
			return foreach_current_index(lc);
	return -1;
}

static bool
list_has_string(List *list, const char *s)
{
	ListCell   *lc;

	foreach(lc, list)
		if (strcmp(lfirst(lc), s) == 0)
			return true;
	return false;
}

/* The schema of a qualified name, as written. */
static char *
schema_of(const char *qualified_name)
{
	List	   *names = stringToQualifiedNameList(qualified_name, NULL);

	return list_length(names) > 1 ? pstrdup(quote_identifier(strVal(linitial(names)))) :
		pstrdup("public");
}

/* A column, as CREATE TABLE and ADD COLUMN write it. */
static char *
column_sql(const char *line)
{
	char	   *copy = pstrdup(line);
	char	   *name = strtok(copy, "\t");
	char	   *type = strtok(NULL, "\t");
	char	   *f;
	char	   *collate = NULL;
	char	   *def = NULL;
	char	   *generated = NULL;
	char		identity = 0;
	char		stored = 's';
	StringInfoData sql;

	while ((f = strtok(NULL, "\t")) != NULL)
	{
		if (strncmp(f, "collate ", 8) == 0)
			collate = f + 8;
		else if (strncmp(f, "default ", 8) == 0)
			def = f + 8;
		else if (strncmp(f, "generated ", 10) == 0)
			generated = f + 10;
		else if (strncmp(f, "identity ", 9) == 0)
			identity = f[9];
		else if (strncmp(f, "stored ", 7) == 0)
			stored = f[7];
		/* "not null" comes with the constraints */
	}
	initStringInfo(&sql);
	appendStringInfo(&sql, "%s %s", name, type);
	if (collate)
		appendStringInfo(&sql, " COLLATE %s", collate);
	if (generated)
		appendStringInfo(&sql, " GENERATED ALWAYS AS (%s) %s", generated,
						 stored == 'v' ? "VIRTUAL" : "STORED");
	else if (def)
		appendStringInfo(&sql, " DEFAULT %s", def);
	if (identity)
		appendStringInfo(&sql, " GENERATED %s AS IDENTITY",
						 identity == 'a' ? "ALWAYS" : "BY DEFAULT");
	return sql.data;
}

/* A column line's attribute, by its prefix ("default "); NULL if none. */
static char *
column_attr(const char *line, const char *prefix)
{
	char	   *copy = pstrdup(line);
	char	   *f = strtok(copy, "\t");

	while ((f = strtok(NULL, "\t")) != NULL)
		if (strncmp(f, prefix, strlen(prefix)) == 0)
			return f + strlen(prefix);
	return NULL;
}

/* The column line (type and collation fields) as ALTER COLUMN TYPE writes it. */
static char *
column_type_sql(const char *line)
{
	char	   *copy = pstrdup(line);
	char	   *type;
	char	   *collate = column_attr(line, "collate ");

	(void) strtok(copy, "\t");
	type = strtok(NULL, "\t");
	return collate ? psprintf("%s COLLATE %s", type, collate) : pstrdup(type);
}

static bool
same_strings(const char *a, const char *b)
{
	return (a == NULL && b == NULL) || (a && b && strcmp(a, b) == 0);
}

/*
 * The statements turning table name's definition from (NULL: none) into to
 * (NULL: dropped): what must go before the rows are written, and what the
 * rows must satisfy, after.  Columns are added and dropped, and changed in
 * type or default, when that gives to's columns in to's order; else the
 * table is made again (its rows written afresh).
 */
static bool
move_table(SchemaMove *sm, const char *name, const TableDef *from,
		   const TableDef *to)
{
	TableDef	none;
	bool		remake = false;
	List	   *from_names = NIL;
	List	   *to_names = NIL;
	ListCell   *lc;

	memset(&none, 0, sizeof(none));
	if (to == NULL)
	{
		/* (its foreign keys first, as other tables' may point at it) */
		foreach(lc, from->constraints)
			if (strstr(lfirst(lc), "\tFOREIGN KEY ") != NULL)
				sm->before = lcons(psprintf("ALTER TABLE %s DROP CONSTRAINT %s",
											name, first_field(lfirst(lc))),
								   sm->before);
		/* (sequences its columns own stay: to has them, or drops them) */
		sm->before = lappend(sm->before,
							 psprintf("DO $move$ DECLARE s regclass; BEGIN "
									  "FOR s IN SELECT d.objid::pg_catalog.regclass FROM pg_catalog.pg_depend d "
									  "JOIN pg_catalog.pg_class c ON c.oid = d.objid AND c.relkind = 'S' "
									  "WHERE d.refobjid = %s::pg_catalog.regclass AND d.classid = 'pg_catalog.pg_class'::pg_catalog.regclass "
									  "AND d.deptype = 'a' LOOP "
									  "EXECUTE pg_catalog.format('ALTER SEQUENCE %%s OWNED BY NONE', s); END LOOP; END $move$",
									  quote_literal_cstr(name)));
		sm->before = lappend(sm->before, psprintf("DROP TABLE %s", name));
		return true;
	}
	if (from != NULL)
	{
		ListCell   *a;
		ListCell   *b;
		List	   *kept_from = NIL;
		List	   *kept_to = NIL;
		bool		adding = false;

		foreach(lc, from->columns)
			from_names = lappend(from_names, first_field(lfirst(lc)));
		foreach(lc, to->columns)
			to_names = lappend(to_names, first_field(lfirst(lc)));
		foreach(lc, from_names)
			if (list_has_string(to_names, lfirst(lc)))
				kept_from = lappend(kept_from, lfirst(lc));
		foreach(lc, to_names)
		{
			if (list_has_string(from_names, lfirst(lc)))
			{
				kept_to = lappend(kept_to, lfirst(lc));
				if (adding)
					remake = true;	/* an added column before a kept one */
			}
			else
				adding = true;
		}
		forboth(a, kept_from, b, kept_to)
			if (strcmp(lfirst(a), lfirst(b)) != 0)
				remake = true;	/* kept columns in another order */
		foreach(lc, to->columns)
		{
			const char *line = lfirst(lc);
			char	   *col = first_field(line);
			int			i = list_member_index_string(from_names, col);

			if (i < 0)
				continue;
			if (!same_strings(column_attr(line, "generated "),
							  column_attr(list_nth(from->columns, i), "generated ")) ||
				!same_strings(column_attr(line, "identity "),
							  column_attr(list_nth(from->columns, i), "identity ")))
				remake = true;
		}
	}
	if (from == NULL || remake)
	{
		StringInfoData sql;

		if (from != NULL)
			(void) move_table(sm, name, from, NULL);
		initStringInfo(&sql);
		appendStringInfo(&sql, "CREATE TABLE %s (", name);
		foreach(lc, to->columns)
			appendStringInfo(&sql, "%s%s", foreach_current_index(lc) ? ", " : "",
							 column_sql(lfirst(lc)));
		appendStringInfoChar(&sql, ')');
		sm->changes = lappend(sm->changes, sql.data);

		/* sequences its defaults draw from, that no column owns: its own */
		sm->changes = lappend(sm->changes,
							  psprintf("DO $move$ DECLARE r record; BEGIN "
									   "FOR r IN SELECT d.refobjid::pg_catalog.regclass AS seq, a.attname "
									   "FROM pg_catalog.pg_attrdef ad "
									   "JOIN pg_catalog.pg_attribute a ON a.attrelid = ad.adrelid AND a.attnum = ad.adnum "
									   "JOIN pg_catalog.pg_depend d ON d.classid = 'pg_catalog.pg_attrdef'::pg_catalog.regclass "
									   "AND d.objid = ad.oid AND d.refclassid = 'pg_catalog.pg_class'::pg_catalog.regclass "
									   "JOIN pg_catalog.pg_class c ON c.oid = d.refobjid AND c.relkind = 'S' "
									   "WHERE ad.adrelid = %s::pg_catalog.regclass AND NOT EXISTS "
									   "(SELECT FROM pg_catalog.pg_depend o WHERE o.classid = 'pg_catalog.pg_class'::pg_catalog.regclass "
									   "AND o.objid = d.refobjid AND o.deptype IN ('a', 'i')) LOOP "
									   "EXECUTE pg_catalog.format('ALTER SEQUENCE %%s OWNED BY %%s.%%I', r.seq, %s, r.attname); "
									   "END LOOP; END $move$",
									   quote_literal_cstr(name), quote_literal_cstr(name)));
		from = &none;
	}
	else
	{
		/* Columns dropped, added, changed. */
		foreach(lc, from_names)
			if (!list_has_string(to_names, lfirst(lc)))
				sm->before = lappend(sm->before,
									 psprintf("ALTER TABLE %s DROP COLUMN %s",
											  name, (char *) lfirst(lc)));
		foreach(lc, to->columns)
		{
			const char *line = lfirst(lc);
			char	   *col = first_field(line);
			int			i = list_member_index_string(from_names, col);
			const char *old;

			if (i < 0)
			{
				sm->changes = lappend(sm->changes,
									  psprintf("ALTER TABLE %s ADD COLUMN %s",
											   name, column_sql(line)));
				continue;
			}
			old = list_nth(from->columns, i);
			if (strcmp(column_type_sql(old), column_type_sql(line)) != 0)
			{
				/* the old default goes first: it may not cast */
				if (column_attr(old, "default "))
					sm->changes = lappend(sm->changes,
										  psprintf("ALTER TABLE %s ALTER COLUMN %s DROP DEFAULT",
												   name, col));
				sm->changes = lappend(sm->changes,
									  psprintf("ALTER TABLE %s ALTER COLUMN %s TYPE %s USING %s::%s",
											   name, col, column_type_sql(line), col,
											   column_type_sql(line)));
				if (column_attr(line, "default "))
					sm->changes = lappend(sm->changes,
										  psprintf("ALTER TABLE %s ALTER COLUMN %s SET DEFAULT %s",
												   name, col, column_attr(line, "default ")));
			}
			else if (!same_strings(column_attr(old, "default "), column_attr(line, "default ")))
				sm->changes = lappend(sm->changes,
									  column_attr(line, "default ") ?
									  psprintf("ALTER TABLE %s ALTER COLUMN %s SET DEFAULT %s",
											   name, col, column_attr(line, "default ")) :
									  psprintf("ALTER TABLE %s ALTER COLUMN %s DROP DEFAULT",
											   name, col));
		}
	}

	/* Triggers, rules, constraints and indexes going: before the rows. */
	foreach(lc, from->triggers)
		if (!list_has_string(to->triggers, lfirst(lc)))
			sm->before = lappend(sm->before,
								 psprintf("DROP TRIGGER %s ON %s",
										  object_name(lfirst(lc), " TRIGGER "), name));
	foreach(lc, from->rules)
		if (!list_has_string(to->rules, lfirst(lc)))
			sm->before = lappend(sm->before,
								 psprintf("DROP RULE %s ON %s",
										  object_name(lfirst(lc), "CREATE RULE "), name));
	foreach(lc, from->constraints)
	{
		char	   *sql;

		if (list_has_string(to->constraints, lfirst(lc)))
			continue;
		/* (IF EXISTS: a column dropped took its own) */
		sql = psprintf("ALTER TABLE %s DROP CONSTRAINT IF EXISTS %s", name,
					   first_field(lfirst(lc)));
		/* a key going: the foreign keys into it go first, and come back */
		if (strstr(lfirst(lc), "\tPRIMARY KEY ") != NULL ||
			strstr(lfirst(lc), "\tUNIQUE ") != NULL)
			sm->rekeyed = lappend(sm->rekeyed, pstrdup(name));
		/* (foreign keys first: they may need others' keys) */
		if (strstr(lfirst(lc), "\tFOREIGN KEY ") != NULL)
			sm->before = lcons(sql, sm->before);
		else
			sm->before = lappend(sm->before, sql);
	}
	foreach(lc, from->indexes)
	{
		char	   *idx = index_name(lfirst(lc));
		bool		owned = false;
		ListCell   *c;

		foreach(c, from->constraints)
			if (strcmp(first_field(lfirst(c)), idx) == 0)
				owned = true;	/* goes with its constraint */
		if (!owned && !list_has_string(to->indexes, lfirst(lc)))
			sm->before = lappend(sm->before, psprintf("DROP INDEX IF EXISTS %s.%s",
													  schema_of(name), idx));
	}

	/*
	 * And coming: NOT NULL first, then the primary key, before the rows,
	 * since the key names them (and both sides hold to them); the rest
	 * after, foreign keys last.
	 */
	for (int pass = 0; pass < 2; pass++)
	{
		foreach(lc, to->constraints)
		{
			const char *line = lfirst(lc);
			bool		not_null = strstr(line, "\tNOT NULL ") != NULL;
			bool		key = strstr(line, "\tPRIMARY KEY ") != NULL;
			char	   *sql;

			if (list_has_string(from->constraints, line) ||
				(pass == 0) != not_null)
				continue;
			sql = psprintf("ALTER TABLE %s ADD CONSTRAINT %s %s", name,
						   first_field(line), strchr(line, '\t') + 1);

			/* (an identity column, or a key, may have made it already) */
			if (not_null)
				sql = psprintf("DO $move$ BEGIN IF NOT EXISTS (SELECT FROM pg_catalog.pg_constraint "
							   "WHERE conrelid = %s::pg_catalog.regclass AND "
							   "pg_catalog.quote_ident(conname) = %s) THEN EXECUTE %s; END IF; END $move$",
							   quote_literal_cstr(name), quote_literal_cstr(first_field(line)),
							   quote_literal_cstr(sql));
			/*
			 * (a column added to a table with rows has none in it until
			 * they are written: its NOT NULL after them)
			 */
			if (not_null && from != &none &&
				!list_has_string(from_names, strstr(line, "\tNOT NULL ") + 10))
				sm->after = lappend(sm->after, sql);
			else if (not_null || key)
				sm->changes = lappend(sm->changes, sql);
			else if (strstr(line, "\tFOREIGN KEY ") != NULL)
				sm->foreign_keys = lappend(sm->foreign_keys, sql);
			else
				sm->after = lappend(sm->after, sql);
		}
	}
	foreach(lc, to->indexes)
	{
		char	   *idx = index_name(lfirst(lc));
		bool		owned = false;
		ListCell   *c;

		foreach(c, to->constraints)
			if (strcmp(first_field(lfirst(c)), idx) == 0)
				owned = true;
		if (!owned && !list_has_string(from->indexes, lfirst(lc)))
			sm->after = lappend(sm->after, pstrdup(lfirst(lc)));
	}
	foreach(lc, to->triggers)
		if (!list_has_string(from->triggers, lfirst(lc)))
			sm->after = lappend(sm->after, pstrdup(lfirst(lc)));
	foreach(lc, to->rules)
		if (!list_has_string(from->rules, lfirst(lc)))
			sm->after = lappend(sm->after, pstrdup(lfirst(lc)));
	return remake;
}

/* A type, as described ("enum", "domain" or "composite", then lines). */
static char *
create_type_sql(const char *name, const char *def)
{
	char	   *copy = pstrdup(def);
	char	   *kind = strtok(copy, "\n");
	char	   *line;
	StringInfoData sql;
	bool		first = true;

	initStringInfo(&sql);
	if (strcmp(kind, "enum") == 0)
	{
		appendStringInfo(&sql, "CREATE TYPE %s AS ENUM (", name);
		while ((line = strtok(NULL, "\n")) != NULL)
		{
			appendStringInfo(&sql, "%s%s", first ? "" : ", ", line);
			first = false;
		}
		appendStringInfoChar(&sql, ')');
	}
	else if (strcmp(kind, "composite") == 0)
	{
		appendStringInfo(&sql, "CREATE TYPE %s AS (", name);
		while ((line = strtok(NULL, "\n")) != NULL)
		{
			char	   *tab = strchr(line, '\t');

			*tab = ' ';
			appendStringInfo(&sql, "%s%s", first ? "" : ", ", line);
			first = false;
		}
		appendStringInfoChar(&sql, ')');
	}
	else
	{
		char	   *base = strtok(NULL, "\n");
		char	   *tab = strchr(base, '\t');

		if (tab)
			*tab = '\0';
		/* (a NOT NULL constraint line names it: not said twice) */
		if (tab && strstr(def, "\tNOT NULL\n") == NULL &&
			(strlen(def) < 9 || strcmp(def + strlen(def) - 9, "\tNOT NULL") != 0))
			appendStringInfo(&sql, "CREATE DOMAIN %s AS %s NOT NULL", name, base);
		else
			appendStringInfo(&sql, "CREATE DOMAIN %s AS %s", name, base);
		while ((line = strtok(NULL, "\n")) != NULL)
		{
			char	   *t = strchr(line, '\t');

			*t = '\0';
			appendStringInfo(&sql, " CONSTRAINT %s %s", line, t + 1);
		}
	}
	return sql.data;
}

/* A sequence's options, as described (type, start, increment, max, min, cache, cycle). */
static char *
sequence_options_sql(const char *def)
{
	char	   *copy = pstrdup(def);
	char	   *type = strtok(copy, "\t");
	char	   *start = strtok(NULL, "\t");
	char	   *incr = strtok(NULL, "\t");
	char	   *max = strtok(NULL, "\t");
	char	   *min = strtok(NULL, "\t");
	char	   *cache = strtok(NULL, "\t");
	char	   *cycle = strtok(NULL, "\t\n");

	return psprintf("AS %s START %s INCREMENT %s MAXVALUE %s MINVALUE %s CACHE %s %s",
					type, start, incr, max, min, cache,
					strcmp(cycle, "cycle") == 0 ? "CYCLE" : "NO CYCLE");
}

/* DROP FUNCTION or DROP PROCEDURE, as the definition says. */
static char *
drop_routine_sql(const MoveItem *it)
{
	return psprintf("DROP %s IF EXISTS %s",
					strstr(it->def, "CREATE OR REPLACE PROCEDURE") == it->def ?
					"PROCEDURE" : "FUNCTION", it->name);
}

/*
 * Make views, each after the views it uses, or drop them, each after those
 * using it: those that fail are tried again once others are done, until
 * none can be.
 */
static void
make_views(List *views)
{
	MemoryContext mcxt = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;

	while (views != NIL)
	{
		List	   *failed = NIL;
		ErrorData  *last = NULL;
		ListCell   *lc;

		foreach(lc, views)
		{
			BeginInternalSubTransaction(NULL);
			MemoryContextSwitchTo(mcxt);
			PG_TRY();
			{
				int			rc = SPI_execute(lfirst(lc), false, 0);

				if (rc < 0)
					elog(ERROR, "could not run \"%s\": %s", (char *) lfirst(lc),
						 SPI_result_code_string(rc));
				ReleaseCurrentSubTransaction();
				MemoryContextSwitchTo(mcxt);
				CurrentResourceOwner = owner;
			}
			PG_CATCH();
			{
				MemoryContextSwitchTo(mcxt);
				last = CopyErrorData();
				FlushErrorState();
				RollbackAndReleaseCurrentSubTransaction();
				MemoryContextSwitchTo(mcxt);
				CurrentResourceOwner = owner;
				failed = lappend(failed, lfirst(lc));
			}
			PG_END_TRY();
		}
		if (list_length(failed) == list_length(views))
			ReThrowError(last);
		views = failed;
	}
}

static void
run_statements(List *stmts)
{
	ListCell   *lc;

	foreach(lc, stmts)
	{
		int			rc = SPI_execute(lfirst(lc), false, 0);

		if (rc < 0)
			elog(ERROR, "could not run \"%s\": %s", (char *) lfirst(lc),
				 SPI_result_code_string(rc));
	}
}

/* A routine's definition up to its body: its signature and what it returns. */
static char *
routine_signature(const char *def)
{
	const char *body = strstr(def, "\n LANGUAGE ");

	return body ? pnstrdup(def, body - def) : pstrdup(def);
}

/* An identifier as written (quoted or not), as it is. */
static char *
unquoted(const char *ident)
{
	StringInfoData out;

	if (ident[0] != '"')
		return pstrdup(ident);
	initStringInfo(&out);
	for (const char *p = ident + 1; *p; p++)
	{
		if (*p == '"' && p[1] == '"')
			p++;
		else if (*p == '"')
			break;
		appendStringInfoChar(&out, *p);
	}
	return out.data;
}

/*
 * The sequences identity columns made by this move make (their tables made,
 * made again, or the columns added): named as Postgres names them.
 */
static List *
identity_sequences(List *from, List *to, List *remade)
{
	List	   *names = NIL;
	ListCell   *lc;

	foreach(lc, to)
	{
		MoveItem   *t = lfirst(lc);
		MoveItem   *f = find_item(from, 't', t->name);
		TableDef	td;
		TableDef	fd;
		ListCell   *c;
		const char *dot;

		if (t->kind != 't')
			continue;
		parse_table_def(t->def, &td);
		if (f && !list_has_string(remade, t->name))
			parse_table_def(f->def, &fd);
		dot = strrchr(t->name, '.');
		foreach(c, td.columns)
		{
			char	   *col = first_field(lfirst(c));
			bool		made = f == NULL || list_has_string(remade, t->name);
			ListCell   *o;

			if (column_attr(lfirst(c), "identity ") == NULL)
				continue;
			if (!made)
			{
				made = true;
				foreach(o, fd.columns)
					if (strcmp(first_field(lfirst(o)), col) == 0)
						made = false;
			}
			if (made)
				names = lappend(names,
								psprintf("%s.%s", schema_of(t->name),
										 quote_identifier(psprintf("%s_%s_seq",
																   unquoted(dot + 1),
																   unquoted(col)))));
		}
	}
	return names;
}

/* The attribute names of a composite type, as described. */
static List *
composite_attributes(const char *def)
{
	char	   *copy = pstrdup(def);
	char	   *line = strtok(copy, "\n");	/* "composite" */
	List	   *names = NIL;

	while ((line = strtok(NULL, "\n")) != NULL)
	{
		char	   *tab = strchr(line, '\t');

		if (tab)
			*tab = '\0';
		names = lappend(names, pstrdup(line));
	}
	return names;
}

/* A composite type's definition: its attributes' types, in order. */
static List *
composite_attribute_types(const char *def)
{
	char	   *copy = pstrdup(def);
	char	   *line = strtok(copy, "\n");	/* "composite" */
	List	   *types = NIL;

	while ((line = strtok(NULL, "\n")) != NULL)
	{
		char	   *tab = strchr(line, '\t');
		char	   *end = tab ? strchr(tab + 1, '\t') : NULL;

		types = lappend(types, tab ? (end ? pnstrdup(tab + 1, end - tab - 1) : pstrdup(tab + 1)) :
						pstrdup(""));
	}
	return types;
}

/*
 * A value expr of a type (its definition old_def, renamed aside) as the
 * type made again as new_def under name has it, for rows the move is about
 * to write as the commit holds them anyway: what can't be had becomes what
 * can, so no conversion fails on rows about to be replaced.  An enum's
 * label it no longer has becomes its first; a composite's attributes come
 * by name (null where the old had none); anything else comes through its
 * text form, or null where that doesn't read as the new type (a domain's
 * check that fails).
 */
static char *
converted_value(const char *expr, const char *old_def, const char *new_def,
				const char *name)
{
	StringInfoData e;

	initStringInfo(&e);
	if (strncmp(new_def, "enum\n", 5) == 0)
		appendStringInfo(&e,
						 "CASE WHEN %s IS NULL THEN NULL "
						 "WHEN %s::pg_catalog.text = ANY (pg_catalog.enum_range(NULL::%s)::pg_catalog.text[]) "
						 "THEN %s::pg_catalog.text::%s ELSE pg_catalog.enum_first(NULL::%s) END",
						 expr, expr, name, expr, name, name);
	else if (strncmp(new_def, "composite\n", 10) == 0 &&
			 strncmp(old_def, "composite\n", 10) == 0)
	{
		List	   *from = composite_attributes(old_def);
		List	   *to = composite_attributes(new_def);
		List	   *types = composite_attribute_types(new_def);
		ListCell   *lc;
		ListCell   *lt;
		bool		first = true;

		appendStringInfo(&e, "CASE WHEN %s IS NULL THEN NULL ELSE ROW(", expr);
		forboth(lc, to, lt, types)
		{
			char	   *attr = psprintf("(%s).%s", expr, (char *) lfirst(lc));

			appendStringInfoString(&e, first ? "" : ", ");
			if (list_has_string(from, lfirst(lc)))
				appendStringInfo(&e,
								 "CASE WHEN pg_catalog.pg_input_is_valid(%s::pg_catalog.text, %s) "
								 "THEN %s::pg_catalog.text::%s END",
								 attr, quote_literal_cstr(lfirst(lt)), attr,
								 (char *) lfirst(lt));
			else
				appendStringInfo(&e, "NULL::%s", (char *) lfirst(lt));
			first = false;
		}
		appendStringInfo(&e, ")::%s END", name);
	}
	else
		appendStringInfo(&e,
						 "CASE WHEN pg_catalog.pg_input_is_valid(%s::pg_catalog.text, %s) "
						 "THEN %s::pg_catalog.text::%s END",
						 expr, quote_literal_cstr(name), expr, name);
	return e.data;
}

/* What a move does about the types the commit defines otherwise, in turn. */
typedef struct TypeMoves
{
	List	   *before;			/* what uses them dropped; then each renamed
								 * aside */
	List	   *creates;		/* the types made (these and new ones), in any
								 * order */
	List	   *converts;		/* the columns using them moved to the new */
	List	   *drops;			/* the old ones, in any order */
	List	   *after;			/* what used them, made again */
	bool		any;
} TypeMoves;

/* A type moved: its definitions, and its oid as the branch has it now. */
typedef struct MovedType
{
	MoveItem   *from;
	MoveItem   *to;
	Oid			typid;
	char	   *aside;			/* qualified, once renamed */
} MovedType;

static MovedType *
moved_type(List *moved, Oid typid)
{
	ListCell   *lc;

	foreach(lc, moved)
		if (((MovedType *) lfirst(lc))->typid == typid)
			return lfirst(lc);
	return NULL;
}

/* A type of the branch's, as from and to name it: added to moved. */
static MovedType *
move_type(List **moved, List *from, List *to, Oid typid, const char *user)
{
	HeapTuple	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	Form_pg_type typ;
	char	   *name;
	MovedType  *mt;

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", typid);
	typ = (Form_pg_type) GETSTRUCT(tup);
	name = qualified(typ->typnamespace, NameStr(typ->typname));
	ReleaseSysCache(tup);
	mt = palloc0(sizeof(MovedType));
	mt->from = find_item(from, 'y', name);
	mt->to = find_item(to, 'y', name);
	mt->typid = typid;
	if (mt->from == NULL || mt->to == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("the branch can't be moved to a commit that defines type %s otherwise",
						user),
				 errdetail("Type %s, which isn't the branch's to move, uses it.", name)));
	*moved = lappend(*moved, mt);
	return mt;
}

/* The line of item def's table definition, of a section, named name. */
static const char *
table_def_line(const MoveItem *t, int section, const char *name)
{
	TableDef	td;
	List	   *lines;
	ListCell   *lc;

	if (t == NULL)
		return NULL;
	parse_table_def(t->def, &td);
	lines = section == 'c' ? td.columns : section == 'k' ? td.constraints : td.indexes;
	foreach(lc, lines)
	{
		const char *line = lfirst(lc);

		if (strcmp(section == 'i' ? index_name(line) : first_field(line), name) == 0)
			return line;
	}
	return NULL;
}

/* Whether a line both definitions of a table have alike, named name. */
static bool
same_table_line(const MoveItem *f, const MoveItem *t, int section, const char *name)
{
	const char *a = table_def_line(f, section, name);
	const char *b = table_def_line(t, section, name);

	return a && b && strcmp(a, b) == 0;
}

/*
 * The statements that make the types to defines otherwise than from, under
 * their names, and those that use them, in turn: each renamed aside, made
 * anew, the columns of versioned tables using it (or arrays of it)
 * converted, and the old dropped.  What uses one is found through what
 * depends on it: a composite type or a domain the branch has is moved
 * with it (as to defines it); a check constraint, an index or a default of
 * a versioned table, or a routine, is dropped first and made again after
 * (as to defines it, if alike; the table's or routine's own move does the
 * rest).  Anything else using it (a column of another schema's table, an
 * operator) can't be moved with it: refused.  Views and materialized views
 * using them go and come back with the others (folded_move_schema()).
 */
static TypeMoves
type_moves(List *from, List *to)
{
	TypeMoves	tm = {0};
	List	   *moved = NIL;
	ListCell   *lc;
	int			i;

	foreach(lc, from)
	{
		MoveItem   *f = lfirst(lc);
		MoveItem   *t = find_item(to, f->kind, f->name);

		if (f->kind == 'y' && t != NULL && strcmp(t->def, f->def) != 0)
			(void) move_type(&moved, from, to,
							 typenameTypeId(NULL, makeTypeNameFromNameList(stringToQualifiedNameList(f->name, NULL))),
							 f->name);
	}
	tm.any = moved != NIL;

	/* each one, and what uses it or arrays of it (more types, found as it goes) */
	for (i = 0; i < list_length(moved); i++)
	{
		MovedType  *mt = list_nth(moved, i);
		Oid			ids[2] = {mt->typid, get_array_type(mt->typid)};

		mt->aside = psprintf("%s.%s", schema_of(mt->from->name),
							 quote_identifier(psprintf("prolly_moving_%u", mt->typid)));
		for (int a = 0; a < 2; a++)
		{
			Relation	pg_depend = table_open(DependRelationId, AccessShareLock);
			ScanKeyData key[2];
			SysScanDesc scan;
			HeapTuple	tup;
			bool		array = a == 1;

			if (!OidIsValid(ids[a]))
			{
				table_close(pg_depend, AccessShareLock);
				continue;
			}
			ScanKeyInit(&key[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber,
						F_OIDEQ, ObjectIdGetDatum(TypeRelationId));
			ScanKeyInit(&key[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber,
						F_OIDEQ, ObjectIdGetDatum(ids[a]));
			scan = systable_beginscan(pg_depend, DependReferenceIndexId, true,
									  NULL, 2, key);
			while ((tup = systable_getnext(scan)) != NULL)
			{
				Form_pg_depend d = (Form_pg_depend) GETSTRUCT(tup);
				ObjectAddress obj = {d->classid, d->objid, d->objsubid};
				bool		handled = false;

				if (d->deptype == DEPENDENCY_INTERNAL)
					continue;	/* (its array type) */
				if (d->classid == RelationRelationId)
				{
					char		kind = get_rel_relkind(d->objid);
					bool		versioned = namespace_is_versioned(get_rel_namespace(d->objid));

					if (kind == RELKIND_VIEW || kind == RELKIND_MATVIEW)
						handled = true; /* (they go, and come again) */
					else if (kind == RELKIND_COMPOSITE_TYPE)
					{
						Oid			ct = get_rel_type_id(d->objid);

						if (moved_type(moved, ct) == NULL)
							(void) move_type(&moved, from, to, ct, mt->from->name);
						handled = true;
					}
					else if (kind == RELKIND_RELATION && versioned && d->objsubid > 0)
					{
						char	   *rel = qualified(get_rel_namespace(d->objid), get_rel_name(d->objid));
						const char *col = quote_identifier(get_attname(d->objid, d->objsubid, false));
						char	   *conv;

						if (array)
						{
							/*
							 * (an array's elements one by one, as a function:
							 * not a subquery; beside the type, and dropped
							 * after)
							 */
							char	   *fn = psprintf("%s.prolly_moving_%u_array",
													  schema_of(mt->to->name), mt->typid);

							tm.converts = lappend(tm.converts,
												  psprintf("CREATE FUNCTION %s(a %s[]) RETURNS %s[] "
														   "LANGUAGE sql AS $f$ SELECT COALESCE(pg_catalog.array_agg(%s ORDER BY i), '{}') "
														   "FROM pg_catalog.generate_subscripts(a, 1) i $f$",
														   fn, mt->aside, mt->to->name,
														   converted_value("a[i]", mt->from->def, mt->to->def, mt->to->name)));
							conv = psprintf("CASE WHEN %s IS NOT NULL THEN %s(%s) END", col, fn, col);
							tm.converts = lappend(tm.converts,
												  psprintf("ALTER TABLE %s ALTER COLUMN %s TYPE %s[] USING %s",
														   rel, col, mt->to->name, conv));
							tm.converts = lappend(tm.converts,
												  psprintf("DROP FUNCTION %s(%s[])", fn, mt->aside));
						}
						else
							tm.converts = lappend(tm.converts,
												  psprintf("ALTER TABLE %s ALTER COLUMN %s TYPE %s USING %s",
														   rel, col, mt->to->name,
														   converted_value(col, mt->from->def, mt->to->def,
																		   mt->to->name)));
						handled = true;
					}
					else if (kind == RELKIND_INDEX && versioned)
					{
						Oid			table = IndexGetRelation(d->objid, false);
						char	   *tname = qualified(get_rel_namespace(table), get_rel_name(table));
						const char *idx = quote_identifier(get_rel_name(d->objid));
						MoveItem   *ft = find_item(from, 't', tname);
						MoveItem   *tt = find_item(to, 't', tname);

						tm.before = lcons(psprintf("DROP INDEX IF EXISTS %s.%s",
												   schema_of(tname), idx), tm.before);
						if (same_table_line(ft, tt, 'i', idx))
							tm.after = lappend(tm.after, pstrdup(table_def_line(tt, 'i', idx)));
						handled = true;
					}
				}
				else if (d->classid == TypeRelationId)
				{
					if (moved_type(moved, d->objid) == NULL)
						(void) move_type(&moved, from, to, d->objid, mt->from->name);
					handled = true;
				}
				else if (d->classid == ConstraintRelationId)
				{
					HeapTuple	ct = SearchSysCache1(CONSTROID, ObjectIdGetDatum(d->objid));
					Form_pg_constraint con;

					if (!HeapTupleIsValid(ct))
						elog(ERROR, "cache lookup failed for constraint %u", d->objid);
					con = (Form_pg_constraint) GETSTRUCT(ct);
					if (OidIsValid(con->contypid))
					{
						/* (a domain's: it moves with it) */
						if (moved_type(moved, con->contypid) == NULL)
							(void) move_type(&moved, from, to, con->contypid, mt->from->name);
						handled = true;
					}
					else if (OidIsValid(con->conrelid) &&
							 namespace_is_versioned(get_rel_namespace(con->conrelid)))
					{
						char	   *tname = qualified(get_rel_namespace(con->conrelid),
													  get_rel_name(con->conrelid));
						const char *cname = quote_identifier(NameStr(con->conname));
						MoveItem   *ft = find_item(from, 't', tname);
						MoveItem   *tt = find_item(to, 't', tname);

						tm.before = lcons(psprintf("ALTER TABLE %s DROP CONSTRAINT IF EXISTS %s",
												   tname, cname), tm.before);
						if (same_table_line(ft, tt, 'k', cname))
							tm.after = lappend(tm.after,
											   psprintf("ALTER TABLE %s ADD CONSTRAINT %s %s", tname, cname,
														strchr(table_def_line(tt, 'k', cname), '\t') + 1));
						handled = true;
					}
					ReleaseSysCache(ct);
				}
				else if (d->classid == AttrDefaultRelationId)
				{
					ObjectAddress col = GetAttrDefaultColumnAddress(d->objid);

					if (OidIsValid(col.objectId) &&
						namespace_is_versioned(get_rel_namespace(col.objectId)))
					{
						char	   *tname = qualified(get_rel_namespace(col.objectId),
													  get_rel_name(col.objectId));
						const char *cname = quote_identifier(get_attname(col.objectId, col.objectSubId, false));
						MoveItem   *ft = find_item(from, 't', tname);
						MoveItem   *tt = find_item(to, 't', tname);

						tm.before = lappend(tm.before,
											psprintf("ALTER TABLE %s ALTER COLUMN %s DROP DEFAULT",
													 tname, cname));
						if (same_table_line(ft, tt, 'c', cname) &&
							column_attr(table_def_line(tt, 'c', cname), "default "))
							tm.after = lappend(tm.after,
											   psprintf("ALTER TABLE %s ALTER COLUMN %s SET DEFAULT %s",
														tname, cname,
														column_attr(table_def_line(tt, 'c', cname), "default ")));
						handled = true;
					}
				}
				else if (d->classid == ProcedureRelationId)
				{
					HeapTuple	pt = SearchSysCache1(PROCOID, ObjectIdGetDatum(d->objid));
					Form_pg_proc proc;
					char	   *name;
					MoveItem   *fp;
					MoveItem   *tp;

					if (!HeapTupleIsValid(pt))
						elog(ERROR, "cache lookup failed for function %u", d->objid);
					proc = (Form_pg_proc) GETSTRUCT(pt);
					name = psprintf("%s(%s)",
									qualified(proc->pronamespace, NameStr(proc->proname)),
									TextDatumGetCString(DirectFunctionCall1(pg_get_function_identity_arguments,
																			ObjectIdGetDatum(d->objid))));
					ReleaseSysCache(pt);
					fp = find_item(from, 'f', name);
					tp = find_item(to, 'f', name);
					if (fp)
					{
						tm.before = lcons(drop_routine_sql(fp), tm.before);
						if (tp && strcmp(tp->def, fp->def) == 0)
							tm.after = lappend(tm.after, tp->def);
						handled = true;
					}
				}
				else if (d->classid == RewriteRelationId)
					handled = true; /* (a view's: it goes, and comes again) */
				if (!handled)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("the branch can't be moved to a commit that defines type %s otherwise",
									mt->from->name),
							 errdetail("%s uses it, and isn't the branch's to move.",
									   getObjectDescription(&obj, false))));
			}
			systable_endscan(scan);
			table_close(pg_depend, AccessShareLock);
		}
	}

	foreach(lc, moved)
	{
		MovedType  *mt = lfirst(lc);

		tm.before = lappend(tm.before,
							psprintf("ALTER TYPE %s RENAME TO %s", mt->from->name,
									 strrchr(mt->aside, '.') + 1));
		tm.creates = lappend(tm.creates, create_type_sql(mt->to->name, mt->to->def));
		tm.drops = lappend(tm.drops, psprintf("DROP TYPE %s", mt->aside));
	}
	return tm;
}

/*
 * Make the session branch's objects defined as to has them, from as from
 * has them (the branch folded now): the statements before the rows, then
 * rows(arg), which writes them, then what they must satisfy; and, if
 * materialized views were made again, rows(arg) once more, for theirs.
 */
static void
folded_move_schema(const ProllyNodeRef *from_root, const ProllyNodeRef *to_root,
				   void (*rows) (void *arg), void *arg)
{
	bool		matviews_made = false;
	Canon		cn;
	List	   *from;
	List	   *to;
	SchemaMove	sm = {NIL, NIL, NIL, NIL, NIL};
	List	   *views = NIL;
	List	   *view_drops = NIL;
	List	   *matview_drops = NIL;
	List	   *keyed;
	List	   *identities;
	List	   *late = NIL;		/* what the changes stop using, dropped */
	List	   *routines = NIL;
	List	   *remade = NIL;
	List	   *schemas = NIL;
	List	   *type_drops = NIL;
	List	   *schema_drops = NIL;
	TypeMoves	tm;
	bool		matviews_change;
	ListCell   *lc;
	int			save_nestlevel;
	MemoryContext caller = CurrentMemoryContext;

	memset(&cn, 0, sizeof(cn));
	cn.folded = true;
	canon_env(&cn);
	from = move_items(&cn, from_root);
	to = move_items(&cn, to_root);
	tm = type_moves(from, to);

	/* Going: views first, as they may use anything; then tables, routines, the rest. */
	foreach(lc, from)
	{
		MoveItem   *f = lfirst(lc);
		MoveItem   *t = find_item(to, f->kind, f->name);

		/*
		 * Views go, and come again: they may use what changes.  (Not with
		 * CASCADE: what else uses one, unchanged, would go and not come
		 * back; the views go each after the views using it.)
		 */
		if (f->kind == 'v')
		{
			view_drops = lappend(view_drops,
								 psprintf("DROP VIEW IF EXISTS %s", f->name));
			continue;
		}

		/*
		 * Materialized views go before them, as they may use them, and
		 * come again with the rows the commit holds.
		 */
		if (f->kind == 'm')
			matview_drops = lappend(matview_drops,
									psprintf("DROP MATERIALIZED VIEW IF EXISTS %s",
											 f->name));
		if (t && strcmp(t->def, f->def) == 0)
			continue;
		switch (f->kind)
		{
			case 'f':
				if (t == NULL)
					routines = lappend(routines, drop_routine_sql(f));
				break;
			case 'y':
			case 'm':
			case 'n':
			case 's':
			case 't':
				break;
			default:
				goto unsupported;
		}
	}
	foreach(lc, from)
	{
		MoveItem   *f = lfirst(lc);
		MoveItem   *t = find_item(to, f->kind, f->name);
		TableDef	fd;
		TableDef	td;

		if (f->kind != 't' || (t && strcmp(t->def, f->def) == 0))
			continue;
		parse_table_def(f->def, &fd);
		if (t)
			parse_table_def(t->def, &td);
		if (move_table(&sm, f->name, &fd, t ? &td : NULL))
			remade = lappend(remade, f->name);
	}

	/*
	 * Foreign keys both sides have, into a table made again or dropped, or
	 * whose keys change: they go first, and the ones kept come back last.
	 */
	keyed = list_concat_copy(remade, sm.rekeyed);
	foreach(lc, from)
	{
		MoveItem   *f = lfirst(lc);
		MoveItem   *t = find_item(to, f->kind, f->name);
		TableDef	fd;
		TableDef	td;
		ListCell   *c;

		if (f->kind != 't' || keyed == NIL || list_has_string(remade, f->name))
			continue;
		parse_table_def(f->def, &fd);
		if (t)
			parse_table_def(t->def, &td);
		foreach(c, fd.constraints)
		{
			const char *line = lfirst(c);
			ListCell   *r;

			if (strstr(line, "\tFOREIGN KEY ") == NULL ||
				(t && !list_has_string(td.constraints, line)))
				continue;		/* (changed ones are move_table()'s) */
			foreach(r, keyed)
			{
				if (strstr(line, psprintf(" REFERENCES %s(", (char *) lfirst(r))) == NULL)
					continue;
				sm.before = lcons(psprintf("ALTER TABLE %s DROP CONSTRAINT %s",
										   f->name, first_field(line)), sm.before);
				if (t)
					sm.foreign_keys = lappend(sm.foreign_keys,
											  psprintf("ALTER TABLE %s ADD CONSTRAINT %s %s",
													   f->name, first_field(line),
													   strchr(line, '\t') + 1));
				break;
			}
		}
	}
	/*
	 * Routines and sequences going: once the tables don't use them; then
	 * types, each after those using it.
	 */
	late = list_concat(late, routines);
	foreach(lc, from)
	{
		MoveItem   *f = lfirst(lc);

		if (find_item(to, f->kind, f->name) == NULL)
		{
			if (f->kind == 'y')
				type_drops = lappend(type_drops, psprintf("DROP TYPE %s", f->name));
			else if (f->kind == 's')
				late = lappend(late, psprintf("DROP SEQUENCE IF EXISTS %s", f->name));
		}
	}
	/* Schemas going: last, what was in them gone already. */
	foreach(lc, from)
	{
		MoveItem   *f = lfirst(lc);

		if (f->kind == 'n' && find_item(to, 'n', f->name) == NULL)
			schema_drops = lappend(schema_drops,
								   psprintf("DROP SCHEMA IF EXISTS %s CASCADE", f->name));
	}

	/*
	 * Coming: schemas, types, sequences, routines, tables; views after the
	 * rows.  An identity column made here makes its own sequence, whose
	 * options are set after.
	 */
	identities = identity_sequences(from, to, remade);
	foreach(lc, to)
	{
		MoveItem   *t = lfirst(lc);

		if (t->kind == 'n' && find_item(from, 'n', t->name) == NULL)
			schemas = lappend(schemas,
							  psprintf("CREATE SCHEMA %s VERSIONED", t->name));
	}
	foreach(lc, to)
	{
		MoveItem   *t = lfirst(lc);
		MoveItem   *f = find_item(from, t->kind, t->name);

		if (f && strcmp(t->def, f->def) == 0)
			continue;
		if (t->kind == 'n')
			continue;			/* (above) */
		if (t->kind == 'y' && f == NULL)
			tm.creates = lappend(tm.creates, create_type_sql(t->name, t->def));
		else if (t->kind == 's' && !f && list_has_string(identities, t->name))
			sm.after = lappend(sm.after,
							   psprintf("ALTER SEQUENCE %s %s", t->name,
										sequence_options_sql(t->def)));
		else if (t->kind == 'y' || t->kind == 'm')
			;					/* changed types first, materialized views last */
		else if (t->kind == 's')
			sm.before = lappend(sm.before,
								psprintf("%s SEQUENCE %s %s", f ? "ALTER" : "CREATE",
										 t->name, sequence_options_sql(t->def)));
		else if (t->kind != 'f' && t->kind != 't' && t->kind != 'v')
			goto unsupported;
	}
	foreach(lc, to)
	{
		MoveItem   *t = lfirst(lc);
		MoveItem   *f = find_item(from, t->kind, t->name);

		if (t->kind != 'f' || (f && strcmp(t->def, f->def) == 0))
			continue;
		/* (CREATE OR REPLACE can't change what a routine returns) */
		if (f && strcmp(routine_signature(f->def), routine_signature(t->def)) != 0)
			sm.before = lappend(sm.before, drop_routine_sql(f));
		sm.before = lappend(sm.before, t->def);
	}
	foreach(lc, to)
	{
		MoveItem   *t = lfirst(lc);
		TableDef	td;

		if (t->kind != 't' || find_item(from, 't', t->name) != NULL)
			continue;
		parse_table_def(t->def, &td);
		move_table(&sm, t->name, NULL, &td);
	}
	/*
	 * Materialized views go and come again, with the rows the commit holds,
	 * if any changes, or a view or a type they may use does: all of them,
	 * each after those it uses (one going takes no other with it).
	 */
	matviews_change = tm.any || view_drops != NIL;
	foreach(lc, from)
	{
		MoveItem   *f = lfirst(lc);
		MoveItem   *t = find_item(to, f->kind, f->name);

		if (f->kind == 'm' && (t == NULL || strcmp(t->def, f->def) != 0))
			matviews_change = true;
	}
	foreach(lc, to)
	{
		MoveItem   *t = lfirst(lc);

		if (t->kind == 'm' && find_item(from, 'm', t->name) == NULL)
			matviews_change = true;
	}
	foreach(lc, to)
	{
		MoveItem   *t = lfirst(lc);

		if (t->kind == 'v')
			views = lappend(views, psprintf("CREATE VIEW %s AS %s", t->name, t->def));
		/*
		 * materialized views made again once the rows are in; their rows,
		 * as to holds them, written after (those of the others are written
		 * with the tables')
		 */
		else if (t->kind == 'm' && matviews_change)
		{
			views = lappend(views, psprintf("CREATE MATERIALIZED VIEW %s AS %s",
											t->name, t->def));
			matviews_made = true;
		}
	}

	/* Run them as deparsed: names qualified, routines' bodies unchecked. */
	save_nestlevel = NewGUCNestLevel();
	(void) set_config_option("search_path", "pg_catalog", PGC_USERSET,
							 PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	(void) set_config_option("check_function_bodies", "off", PGC_USERSET,
							 PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	/* (what's already gone, an IF EXISTS skips: no notice of it) */
	(void) set_config_option("client_min_messages", "warning", PGC_USERSET,
							 PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	SPI_connect();
	make_views(list_concat(matviews_change ? matview_drops : NIL, view_drops));

	/*
	 * Types first, in turn (type_moves()): new schemas made once the types
	 * changed are aside, all the types made (each after those it uses), the
	 * columns using the changed ones moved, the old ones dropped.
	 */
	run_statements(tm.before);
	run_statements(schemas);
	make_views(tm.creates);
	run_statements(tm.converts);
	make_views(tm.drops);
	run_statements(tm.after);
	run_statements(list_concat(list_concat(sm.before, sm.changes), late));
	make_views(type_drops);
	run_statements(schema_drops);
	CommandCounterIncrement();
	/* the rows in the caller's memory: what they keep outlives SPI's */
	{
		MemoryContext spi = MemoryContextSwitchTo(caller);

		rows(arg);
		MemoryContextSwitchTo(spi);
	}
	run_statements(sm.after);
	run_statements(sm.foreign_keys);
	make_views(views);
	if (matviews_made)
	{
		MemoryContext spi;

		CommandCounterIncrement();
		spi = MemoryContextSwitchTo(caller);
		rows(arg);
		MemoryContextSwitchTo(spi);
	}
	SPI_finish();
	AtEOXact_GUC(true, save_nestlevel);
	return;

unsupported:
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("the branch can't be moved to a commit that defines objects of this kind differently")));
}

/*
 * The definitions of base, ours and theirs merged: each object as the side
 * that changed it since base has it (ours if neither, or both alike).  An
 * object both changed differently is a schema conflict, and fails; so is a
 * schema one side dropped that the other put objects in.  Only the
 * definitions count: a table's item is the side's it was taken from, its
 * rows that side's, which the merge doesn't use (it merges the rows
 * itself).  Collations' items aren't definitions, but what the fold
 * recorded of them: either side's will do, the merge commit's fold
 * recording them as they are then.
 */
static void
folded_merge_schema(const ProllyNodeRef *base_root, const ProllyNodeRef *ours_root,
					const ProllyNodeRef *theirs_root, ProllyNodeRef *merged)
{
	Canon		cn;
	ProllyTreeCursor *cur[3];
	bool		more[3];
	ProllyKey	k[3];
	ProllyValue v[3];
	ProllyTreeBuilder *b;
	List	   *dropped = NIL;	/* schemas the merge drops */
	List	   *kept = NIL;		/* the names of the objects it keeps */
	ListCell   *lc;

	memset(&cn, 0, sizeof(cn));
	cn.folded = true;
	canon_env(&cn);
	cur[0] = PROLLY_CALL(cn.tree, cursor_open, &cn.env, base_root, CurrentMemoryContext);
	cur[1] = PROLLY_CALL(cn.tree, cursor_open, &cn.env, ours_root, CurrentMemoryContext);
	cur[2] = PROLLY_CALL(cn.tree, cursor_open, &cn.env, theirs_root, CurrentMemoryContext);
	for (int i = 0; i < 3; i++)
	{
		PROLLY_CALL(cn.tree, cursor_seek, cur[i], NULL, ForwardScanDirection);
		more[i] = PROLLY_CALL(cn.tree, cursor_next, cur[i], &k[i], &v[i]);
	}
	b = PROLLY_CALL(cn.tree, build_begin, &cn.env, CurrentMemoryContext);
	while (more[0] || more[1] || more[2])
	{
		const ProllyKey *least = NULL;
		bool		has[3];
		ProllyValue def[3];
		const ProllyValue *take = NULL;

		/* the least key of the three, and which have it */
		for (int i = 0; i < 3; i++)
			if (more[i] && (least == NULL || prolly_bytes_compare(&k[i], least, NULL) < 0))
				least = &k[i];
		for (int i = 0; i < 3; i++)
		{
			has[i] = more[i] && prolly_bytes_compare(&k[i], least, NULL) == 0;
			if (has[i])
				definition_of(&k[i], &v[i], &def[i]);
		}
#define SAME(a, b) (has[a] == has[b] && (!has[a] || \
					(def[a].len == def[b].len && memcmp(def[a].data, def[b].data, def[a].len) == 0)))
		if (SAME(0, 2))
			take = has[1] ? &v[1] : NULL;	/* theirs left it: ours */
		else if (SAME(0, 1) || SAME(1, 2))
			take = has[2] ? &v[2] : NULL;	/* ours left it, or both alike: theirs */
		else if (least->data[0] == 'c')
			take = has[1] ? &v[1] : &v[2];	/* (collations: as recorded) */
		else
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("schema conflict: %s was changed differently on both sides",
							from_utf8(least->data + 2, least->len - 2)),
					 errdetail("Both branches made, dropped or redefined it since their common ancestor.")));
#undef SAME
		if (take)
			PROLLY_CALL(cn.tree, build_add, b, least, take);
		if (least->data[0] == 'n' && has[0] && !take)
			dropped = lappend(dropped, from_utf8(least->data + 2, least->len - 2));
		else if (take && least->data[0] != 'n' && least->data[0] != 'c')
			kept = lappend(kept, from_utf8(least->data + 2, least->len - 2));

		/* (the least key last: the others point into its cursor's node) */
		for (int i = 0; i < 3; i++)
			if (has[i] && &k[i] != least)
				more[i] = PROLLY_CALL(cn.tree, cursor_next, cur[i], &k[i], &v[i]);
		for (int i = 0; i < 3; i++)
			if (has[i] && &k[i] == least)
				more[i] = PROLLY_CALL(cn.tree, cursor_next, cur[i], &k[i], &v[i]);
	}
	PROLLY_CALL(cn.tree, build_finish, b, merged);
	for (int i = 0; i < 3; i++)
		PROLLY_CALL(cn.tree, cursor_close, cur[i]);

	/* a schema dropped on one side, given objects on the other */
	foreach(lc, kept)
		if (list_has_string(dropped, schema_of(lfirst(lc))))
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("schema conflict: schema %s was dropped on one side and given %s on the other",
							schema_of(lfirst(lc)), (char *) lfirst(lc)),
					 errdetail("Dropping it would drop what the other side made in it.")));
}

const ProllyCanonRoutine prolly_canon_folded = {
	.info = {
		.iface = PROLLY_IFACE_CANONICALIZER,
		.impl_id = 2,
		.format_version = 3,		/* 3: sequences' values */
		.name = "folded",
		.description = "canonical form by name of the session branch's "
		"versioned schemas, their tables' rows folded into trees",
	},
	.walk = folded_walk,
	.fold = folded_fold,
	.read_fold = folded_read_fold,
	.tables = folded_tables,
	.materialized_views = folded_materialized_views,
	.same_schema = folded_same_schema,
	.move_schema = folded_move_schema,
	.sequences = folded_sequences,
	.merge_schema = folded_merge_schema,
	.walk_baseline = folded_walk_baseline,
	.table_columns = folded_table_columns,
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
