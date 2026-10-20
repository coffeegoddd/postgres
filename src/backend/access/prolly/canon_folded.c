/*-------------------------------------------------------------------------
 *
 * canon_folded.c
 *	  Canonicalizer implementation "folded": the canonical form of the
 *	  session branch's versioned schemas, derived from the catalogs, by
 *	  name, their tables' rows folded into trees.
 *
 * The canonical root is a tree keyed (by memcmp) by kind and qualified
 * name, every string in UTF-8:
 *
 *	"!:format"			-> the format its items and data trees are in,
 *						   FOLDED_FORMAT
 *	"c:" collation		-> its provider, locale, ICU rules, whether it is
 *						   deterministic and its library's version, a line
 *						   each ("default": the database's)
 *	"f:" function(args)	-> its definition
 *	"m:" materialized view -> its data tree (hash length, hash), then
 *						   whether it is unpopulated, its indexes, its
 *						   explicit options and its columns' nondefault
 *						   settings, a line each, then its query
 *	"n:" schema			-> nothing: a versioned schema, there whether or not
 *						   anything is in it
 *	"s:" sequence		-> its definition, then a line of its own, its
 *						   value: the last value, and whether it was given
 *						   out ("t" or "f")
 *	"t:" table			-> its data tree (hash length, hash), then its schema
 *	"v:" view			-> its options (WITH (...) AS), if any, then its
 *						   query
 *	"x:" source AS target -> a cast: its method (and function), its
 *						   context
 *	"y:" type			-> an enum's labels, a domain's, a composite or
 *						   a range type's definition
 *
 * A table's schema lists its live columns in order (name, type, nullness,
 * collation if not the type's, default, identity, generation, nondefault
 * storage, compression and statistics target), then its
 * constraints, indexes, triggers and rules by name, as Postgres's deparser
 * prints them, a trigger or rule that doesn't fire as made with when it
 * does (then its policies and extended statistics, its partitioning, the
 * tables it inherits from and a typed table's type, explicit table and
 * TOAST options, and nondefault replica identity, if any).  Names are
 * qualified whatever the session's search path (it
 * is set to pg_catalog while deparsing), so nothing local, no OID or
 * relfilenumber, and nothing about how the objects came to be (their
 * order, dropped columns) reaches the hash.
 *
 * A collation's, a table's schema and a type's definition are split into
 * lines (by newline) and fields (by tab), and every name, type, expression
 * or definition in them is escaped (a backslash, tab or newline as \\, \t
 * or \n: append_field()), as a quoted name or a string literal may hold
 * one; so a raw tab or newline is only a separator, and a field is read
 * back (unescape_field()) where it is used.  A function's definition, and
 * a view's or materialized view's query, is never split, and is kept as it
 * is (what comes before a query, in lines, never begins as one does).
 *
 * A table's rows are folded
 * into its data tree (keyed by the table keying, sorted, a tree in the
 * chunk store), and the canonical tree is kept there too.
 *
 * Covered, in versioned schemas: prolly tables, their columns,
 * constraints, indexes, triggers, rules and row security policies;
 * partitioned tables and their partitions; views and materialized views;
 * functions, procedures and aggregates; enum, domain and composite types;
 * sequences.
 * Ordinary schemas and extensions are every branch's, not the commit's.
 * Anything else in versioned schemas (window functions, foreign tables,
 * partitions apart from their partitioned tables, other types) makes a
 * commit fail: never a hash that might depend on history.  So does a
 * column whose values are local OIDs (type_holds_oids()): never a hash of
 * local identifiers.
 *
 * What the versioned objects depend on outside the versioned schemas
 * isn't in the root, which is what they are: fold_dependencies() builds a
 * tree of it for a commit to record beside its root (prolly_refs.h), keyed
 * by kind and identity ("extension hstore", "type public.money2"):
 * an extension valued by its version and the schema it is in (a text[]
 * literal: the objects that use its members name them there), another
 * object by its address (a
 * text[] literal of its type and of text[] literals of its names and
 * arguments, as pg_identify_object_as_address() gives them), which
 * pg_get_object_address() finds again in another database.  Found
 * through pg_depend: each object of an ordinary schema, as the extension
 * with its version if an extension's member, and each extension a
 * versioned object depends on explicitly (DEPENDS ON EXTENSION); not what
 * every database has (objects made by initdb).  Not roles: owners and
 * grants aren't part of the canonical form, as a database's dump doesn't
 * make roles.  Each is described afresh at each commit, so an extension's
 * update or an object's rename shows in the next.  A function's body is
 * text, and what it names isn't in pg_depend: the list can't be complete.
 *
 * Each collation that orders a table's keys is recorded, and a table whose
 * keys are ordered by a collation that differs now from what a root
 * recorded can't have that root's rows read: its tree's keys may be in
 * another order.  The root is still listed, and its other tables read.
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
 *	  src/backend/access/prolly/canon_folded.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "nodes/makefuncs.h"
#include "nodes/miscnodes.h"
#include "nodes/nodeFuncs.h"
#include "catalog/pg_attribute.h"
#include "parser/parse_type.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parser.h"
#include "access/htup_details.h"
#include "access/prolly_applier.h"
#include "access/prolly_builtins.h"
#include "access/prolly_config.h"
#include "access/prolly_datum.h"
#include "access/prolly_keying.h"
#include "access/relation.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/toast_compression.h"
#include "access/xact.h"
#include "catalog/branch.h"
#include "catalog/branch_ops.h"
#include "catalog/dependency.h"
#include "catalog/objectaddress.h"
#include "catalog/objectaccess.h"
#include "commands/comment.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_attrdef.h"
#include "catalog/heap.h"
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
#include "catalog/pg_conversion.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_ts_parser.h"
#include "catalog/pg_ts_template.h"
#include "catalog/pg_policy.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_range.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_trigger.h"
#include "catalog/partition.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_default_acl.h"
#include "catalog/pg_init_privs.h"
#include "catalog/pg_seclabel.h"
#include "commands/seclabel.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "commands/extension.h"
#include "commands/tablespace.h"
#include "catalog/versioned.h"
#include "commands/defrem.h"
#include "commands/sequence.h"
#include "commands/trigger.h"
#include "common/int.h"
#include "common/string.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteSupport.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "common/hashfn.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/fmgrprotos.h"
#include "utils/guc.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/pg_locale.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

typedef struct CanonItem
{
	StringInfoData key;
	StringInfoData value;
} CanonItem;

typedef struct BaseTable BaseTable;

/*
 * The format a folded root is in: "12", every chunk and hash pointer named
 * by BLAKE3's first 20 bytes; "11" named them by SHA-256, and was the
 * first with primary keys whose opfamily
 * compares them as a type they're binary-coercible to (varchar as text,
 * cidr as inet) ordered by that comparison, rather than rows keyed by
 * content; "10" described table and TOAST options, replica identity and
 * column storage, compression and statistics targets (a materialized
 * view's options and column settings too); "9" left those out, and was
 * the first with what changes how a definition behaves described when it
 * isn't the default (inheritance, and which inherited columns and
 * constraints are a child's own too; typed tables, trigger and rule
 * firing, domain collations and defaults, composite attributes'
 * collations, view options, matviews' indexes and being unpopulated);
 * "8" left those out, and was the first with every float NaN in rows and
 * keys the one canonical NaN (prolly_datum.c); "7" had a NaN's
 * own bits, and was the first with indexes described without their
 * tablespaces; "6" had an index's TABLESPACE clause when it wasn't in the
 * default one, and was the first with definitions' expressions (checks,
 * defaults, generated expressions, policies) as they print read back
 * (reprinted()); "5" had them as printed, and was the first with a row's
 * long values each out of line in a value tree of its own; "4" had only
 * long rows out of line; "3" keyed value trees' chunks by offset, and was
 * the first with its definitions' fields escaped (append_field()); "2" was
 * keyless rows keyed by their first bytes and their hash, and a root
 * without the item keyed them by the hash alone.  A root in another has
 * its tables' rows unreadable here, and is neither moved to nor merged:
 * its definitions or row keying aren't this format's.
 */
#define FOLDED_FORMAT	"13"

typedef struct Canon
{
	ProllyTreeEnv env;
	ProllyTreeEnv root_env;		/* a canonical root's: its items' references */
	List	   *items;
	const ProllyTreeRoutine *tree;
	List	   *collations;		/* OIDs of the collations ordering keys */

	/* the branch's baseline, and the next one if it is made */
	const ProllyChangeFinderRoutine *finder;
	BaseTable  *base;			/* in key order */
	int			nbase;
	int			maxbase;
	bool		advance;
	List	   *next;			/* CanonItem *, the next baseline's */
	bool		reading;		/* objects not described are left out */
	List	   *left_out;		/* (reading) their names */
	List	   *left_addrs;		/* (reading) ObjectAddress *: the same */
	List	   *refused;		/* (committing) Refused *: the same */
	Oid			only;			/* valid: only this table's rows folded */
	bool		no_rows;		/* no table's rows folded (objects only) */
} Canon;

static void build_canonical(Canon *cn, ProllyNodeRef *root);
static void append_statistics(StringInfo out, Relation rel);

void
prolly_folded_tree_env(ProllyTreeEnv *env)
{
	memset(env, 0, sizeof(*env));
	env->chunk_format = prolly_chunkformat_lookup("basic");
	env->chunker = prolly_chunker_lookup("bytes");
	env->hash = prolly_content_hash();
	env->chunk_store = prolly_chunk_store();
	env->chunks = PROLLY_CALL(env->chunk_store, open, MyDatabaseId,
							  MyDatabaseTableSpace);
	env->compare = prolly_bytes_compare;
	env->counters = pgstat_prolly_tree_counters();
}

/* Declare the hash at value's start (a length byte, then the hash). */
static void
declare_leading_hash(const ProllyValue *value, StringInfo refs)
{
	ProllyHash	h;

	if (value->len < 1 || value->data[0] == 0 ||
		value->data[0] > PROLLY_HASH_MAX_LEN || value->len < 1 + value->data[0])
		return;
	memset(&h, 0, sizeof(h));
	h.len = value->data[0];
	memcpy(h.bytes, value->data + 1, h.len);
	PROLLY_CALL(prolly_tree_lookup("cow"), value_reference, &h, 0, refs);
}

/* A canonical root's items: a table's or materialized view's data tree. */
static void
canon_chunk_refs(const ProllyKey *key, const ProllyValue *value, void *arg,
				 StringInfo refs)
{
	if (key->len >= 2 && key->data[1] == ':' &&
		(key->data[0] == 't' || key->data[0] == 'm'))
		declare_leading_hash(value, refs);
}

void
prolly_folded_canon_env(ProllyTreeEnv *env)
{
	prolly_folded_tree_env(env);
	env->chunk_refs = canon_chunk_refs;
}

/*
 * A baseline's items: the table's data tree (its definition's hash, next,
 * names no chunk), then what its finder's state names, as every change
 * finder declares it (the one that wrote it may not be the one configured
 * now).
 */
static void
base_chunk_refs(const ProllyKey *key, const ProllyValue *value, void *arg,
				StringInfo refs)
{
	ProllyValue state = *value;
	static List *finders = NIL;

	declare_leading_hash(value, refs);
	for (int i = 0; i < 2 && state.len >= 1 && state.data[0] <= PROLLY_HASH_MAX_LEN &&
		 state.len >= 1 + state.data[0]; i++)
	{
		state.len -= 1 + state.data[0];
		state.data += 1 + state.data[0];
	}
	/* (the finders registered, as of the first: registered at start) */
	if (finders == NIL)
	{
		MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

		finders = prolly_list_impls(PROLLY_IFACE_CHANGE_FINDER);
		MemoryContextSwitchTo(old);
	}
	foreach_ptr(const ProllyChangeFinderRoutine, finder, finders)
	{
		if (finder->state_refs != NULL)
			finder->state_refs(&state, refs);
	}
}

static void
canon_env(Canon *cn)
{
	cn->tree = prolly_tree_lookup("cow");
	prolly_folded_tree_env(&cn->env);
	prolly_folded_canon_env(&cn->root_env);
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

/*
 * Append s in UTF-8 as a field of a value split into lines and fields: a
 * backslash, tab or newline in it escaped (\\, \t, \n), so that a raw tab
 * or newline is only ever a separator.  unescape_field() reads it back.
 */
static void
append_field(StringInfo out, const char *s)
{
	char	   *u = pg_server_to_any(s, strlen(s), PG_UTF8);

	for (const char *c = u; *c; c++)
	{
		if (*c == '\\')
			appendStringInfoString(out, "\\\\");
		else if (*c == '\t')
			appendStringInfoString(out, "\\t");
		else if (*c == '\n')
			appendStringInfoString(out, "\\n");
		else
			appendStringInfoChar(out, *c);
	}
	if (u != s)
		pfree(u);
}

/* A field append_field() wrote, as it was (any part of one: no separators). */
static char *
unescape_field(const char *s)
{
	StringInfoData out;

	initStringInfo(&out);
	for (const char *c = s; *c; c++)
	{
		if (*c == '\\' && (c[1] == '\\' || c[1] == 't' || c[1] == 'n'))
		{
			c++;
			appendStringInfoChar(&out, *c == 't' ? '\t' : *c == 'n' ? '\n' : '\\');
		}
		else
			appendStringInfoChar(&out, *c);
	}
	return out.data;
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

/* An object a commit can't hold: what kind, and its name. */
typedef struct Refused
{
	const char *what;
	char	   *name;
} Refused;

/*
 * An object the canonical form doesn't describe: left out (true).  When
 * reading, its name is noted, and its address if it is left out whole (a
 * move may discard it); when committing, it is refused once every object
 * has been read (check_refused()).
 */
static bool
uncovered(Canon *cn, const char *what, const char *name,
		  const ObjectAddress *addr)
{
	if (cn->reading)
	{
		cn->left_out = lappend(cn->left_out, pstrdup(name));
		if (addr)
		{
			ObjectAddress *a = palloc(sizeof(ObjectAddress));

			*a = *addr;
			cn->left_addrs = lappend(cn->left_addrs, a);
		}
	}
	else
	{
		Refused    *r = palloc(sizeof(Refused));

		r->what = what;
		r->name = pstrdup(name);
		cn->refused = lappend(cn->refused, r);
	}
	return true;
}

static int
refused_cmp(const ListCell *a, const ListCell *b)
{
	const Refused *x = lfirst(a);
	const Refused *y = lfirst(b);
	int			c = strcmp(x->name, y->name);

	return c != 0 ? c : strcmp(x->what, y->what);
}

/*
 * Refuse a commit of objects the canonical form doesn't describe, naming
 * the first by name: the same one whatever order the catalogs were read in.
 */
static void
check_refused(Canon *cn)
{
	Refused    *r;

	if (cn->refused == NIL)
		return;
	list_sort(cn->refused, refused_cmp);
	r = linitial(cn->refused);
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("prolly commits don't cover %s yet: %s", r->what, r->name),
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
	ProllyHash	description;	/* of its definition (baseline_hash()) */
	StringInfoData state;		/* the change finder's */
};

static void
content_hash(Canon *cn, const void *data, Size len, ProllyHash *h)
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

/* The empty data tree. */
static void
empty_root(Canon *cn, ProllyNodeRef *root)
{
	ProllyTreeBuilder *b = PROLLY_CALL(cn->tree, build_begin, &cn->env,
									   CurrentMemoryContext);

	PROLLY_CALL(cn->tree, build_finish, b, root);
}

/*
 * What a baseline's tree is kept for: what its rows' keys and canonical
 * bytes depend on, the keying's row shape where it gives one (else the
 * table's whole description), so DDL that changes no row's bytes (an
 * index, a constraint, a trigger) keeps the baseline; and what they depend
 * on that changes no page (so no change the finder would see).  The columns rows are read over, by number: a
 * column dropped and one of the same name and type added describe the
 * table alike, but its rows read as NULL.  And the definitions of the
 * columns' types (prolly_datum_definition()): an enum's label renamed, a
 * composite's attribute dropped, encode the same rows otherwise.  And
 * the key encoder, value encoder and keying, by implementation and format
 * version: one whose format changed encodes the same rows otherwise too.
 *
 * (An enum's label added changes its definition but no existing row's
 * bytes; telling it from a rename would take the old labels, so the next
 * fold reads each table using it in full, once.)
 */
static void
baseline_hash(Canon *cn, Relation rel, const StringInfoData *description,
			  ProllyHash *h)
{
	TupleDesc	desc = RelationGetDescr(rel);
	StringInfoData match;
	ProllyComponents c;
	const ProllyImplInfo *encoders[3];

	initStringInfo(&match);
	prolly_components_for_relation(rel, &c);

	/*
	 * (the keying's row shape where it says it: DDL that changes no row's
	 * bytes, as an index or a constraint, keeps the baseline; else the
	 * whole description)
	 */
	if (PROLLY_HAS(c.keying, row_shape))
	{
		ProllyKeying *k = PROLLY_CALL(c.keying, open, rel, CurrentMemoryContext);

		appendStringInfoChar(&match, 'S');
		PROLLY_CALL(c.keying, row_shape, k, rel, &match);
		PROLLY_CALL(c.keying, close, k);
	}
	else
		appendBinaryStringInfo(&match, description->data, description->len);
	appendStringInfoChar(&match, '\0');
	encoders[0] = &c.key_encoder->info;
	encoders[1] = &c.value_encoder->info;
	encoders[2] = &c.keying->info;
	for (int i = 0; i < lengthof(encoders); i++)
	{
		appendBinaryStringInfo(&match, (const char *) &encoders[i]->impl_id,
							   sizeof(encoders[i]->impl_id));
		appendBinaryStringInfo(&match,
							   (const char *) &encoders[i]->format_version,
							   sizeof(encoders[i]->format_version));
	}
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (att->attisdropped)
			continue;
		appendBinaryStringInfo(&match, (const char *) &att->attnum,
							   sizeof(att->attnum));
		prolly_datum_definition(att->atttypid, &match);
	}
	content_hash(cn, match.data, match.len, h);
	pfree(match.data);
}

/*
 * A table's data tree, folded from its rows as the transaction sees them by
 * the change finder: from its baseline if it has one for this definition
 * (described in 'description', its rows as baseline_hash() has them); with
 * a next baseline being made, its entry there too.  The same rows give the
 * same tree whatever pages, order or history they come from.
 */
static void
table_root(Canon *cn, CanonItem *it, Relation rel,
				  const StringInfoData *description, ProllyNodeRef *root)
{
	ProllyHash	dh;
	BaseTable  *bt;
	ProllyTableFold tf;
	StringInfoData state;
	Oid			pkoid = RelationGetPrimaryKeyIndex(rel, false);
	ProllyFullFoldReason why;

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

	baseline_hash(cn, rel, description, &dh);
	bt = find_base_table(cn, &it->key);
	why = bt ? PROLLY_FULL_SHAPE_CHANGED : PROLLY_FULL_NO_BASELINE;
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
	if (cn->no_rows ||
		(OidIsValid(cn->only) && RelationGetRelid(rel) != cn->only))
	{
		if (bt)
			*root = bt->data;
		else
			empty_root(cn, root);
		return;
	}
	initStringInfo(&state);
	PROLLY_CALL(cn->finder, fold, rel, &cn->env, bt ? &tf : NULL, why, root,
				cn->advance ? &state : NULL);
	if (cn->advance)
	{
		CanonItem  *next = palloc0(sizeof(CanonItem));
		ProllyComponents c;

		/* (what the fold saw is in the next baseline: its entries are spent) */
		prolly_components_for_relation(rel, &c);
		if (c.change_log)
			PROLLY_CALL(c.change_log, consume, rel, GetActiveSnapshot());

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

/* Is an object in this namespace the canonical form's? */
static bool
canon_namespace(Oid nsp)
{
	if (is_system_namespace(nsp))
		return false;
	return namespace_is_versioned(nsp);
}

static int
oid_name_cmp(const ListCell *a, const ListCell *b)
{
	return strcmp(get_rel_name(lfirst_oid(a)), get_rel_name(lfirst_oid(b)));
}

/* A trigger's or rule's definition, and when it fires (ev_enabled, tgenabled). */
typedef struct FiringDef
{
	char	   *def;
	char		enabled;		/* 0: as made (fires on the origin) */
} FiringDef;

static int
firing_def_cmp(const ListCell *a, const ListCell *b)
{
	return strcmp(((FiringDef *) lfirst(a))->def, ((FiringDef *) lfirst(b))->def);
}

/*
 * The definitions of rel's triggers (internal ones aside) or rules (a
 * view's own aside), sorted, one per line; one that doesn't fire as made
 * (disabled, or set to fire on a replica or always: ALTER TABLE ... ENABLE
 * REPLICA TRIGGER) followed by a field of when it does, its pg_trigger
 * tgenabled or pg_rewrite ev_enabled ('D', 'R' or 'A').  (The definition
 * ends in a parenthesis or a semicolon: never so.)
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
		char		enabled;
		FiringDef  *fd;

		if (catalog == TriggerRelationId)
		{
			Form_pg_trigger trg = (Form_pg_trigger) GETSTRUCT(tup);

			/* (a partition's clone of its parent's: the parent's) */
			if (trg->tgisinternal || OidIsValid(trg->tgparentid))
				continue;
			def = DirectFunctionCall1(pg_get_triggerdef, ObjectIdGetDatum(trg->oid));
			enabled = trg->tgenabled == TRIGGER_FIRES_ON_ORIGIN ? 0 : trg->tgenabled;
		}
		else
		{
			Form_pg_rewrite rw = (Form_pg_rewrite) GETSTRUCT(tup);

			if (strcmp(NameStr(rw->rulename), ViewSelectRuleName) == 0)
				continue;
			def = DirectFunctionCall1(pg_get_ruledef, ObjectIdGetDatum(rw->oid));
			enabled = rw->ev_enabled == RULE_FIRES_ON_ORIGIN ? 0 : rw->ev_enabled;
		}
		fd = palloc(sizeof(FiringDef));
		fd->def = TextDatumGetCString(def);
		fd->enabled = enabled;
		defs = lappend(defs, fd);
	}
	systable_endscan(scan);
	table_close(crel, AccessShareLock);
	list_sort(defs, firing_def_cmp);
	foreach(lc, defs)
	{
		FiringDef  *fd = lfirst(lc);

		append_field(out, fd->def);
		if (fd->enabled)
			appendStringInfo(out, "\t%c", fd->enabled);
		appendStringInfoChar(out, '\n');
	}
}

/* VALUE in a domain's check, as Postgres reads it (typecmds.c's). */
static Node *
domain_value_ref(ParseState *pstate, ColumnRef *cref)
{
	CoerceToDomainValue *v;

	if (list_length(cref->fields) != 1 ||
		strcmp(strVal(linitial(cref->fields)), "value") != 0)
		return NULL;
	v = copyObject((CoerceToDomainValue *) pstate->p_ref_hook_state);
	v->location = cref->location;
	return (Node *) v;
}

/*
 * An expression's text as it prints once read back.  Postgres doesn't always
 * read its printed text back as the expression it printed (an IN list over a
 * varchar prints its array with a cast, which reads back as a cast of each
 * element), and a move makes an object again from its text, so a definition
 * described by the printed text alone isn't the same after a move.  The text
 * read back prints the same again.
 *
 * src is an expression printed, read as Postgres reads its kind: a check
 * or policy of rel (kind its expression kind), a default or generated
 * expression of att, or a check of domain (with rel NULL).
 */
static char *
reprinted(const char *src, Relation rel, Form_pg_attribute att,
		  Form_pg_type domain, ParseExprKind kind)
{
	List	   *raw = raw_parser(psprintf("SELECT %s", src), RAW_PARSE_DEFAULT);
	SelectStmt *sel = (SelectStmt *) linitial_node(RawStmt, raw)->stmt;
	ResTarget  *rt = linitial_node(ResTarget, sel->targetList);
	ParseState *pstate = make_parsestate(NULL);
	Node	   *expr;

	if (rel)
	{
		ParseNamespaceItem *nsitem;

		nsitem = addRangeTableEntryForRelation(pstate, rel, AccessShareLock,
											   NULL, false, true);
		addNSItemToQuery(pstate, nsitem, true, true, true);
	}
	if (att)
		expr = cookDefault(pstate, rt->val, att->atttypid, att->atttypmod,
						   NameStr(att->attname), att->attgenerated);
	else
	{
		if (domain)
		{
			CoerceToDomainValue *v = makeNode(CoerceToDomainValue);

			v->typeId = domain->typbasetype;
			v->typeMod = domain->typtypmod;
			v->collation = get_typcollation(domain->typbasetype);
			v->location = -1;
			pstate->p_pre_columnref_hook = domain_value_ref;
			pstate->p_ref_hook_state = v;
		}
		expr = transformExpr(pstate, rt->val, kind);
		expr = coerce_to_boolean(pstate, expr,
								 kind == EXPR_KIND_POLICY ? "POLICY" : "CHECK");
		assign_expr_collations(pstate, expr);
	}
	free_parsestate(pstate);
	return TextDatumGetCString(DirectFunctionCall2(pg_get_expr,
												   CStringGetTextDatum(nodeToString(expr)),
												   ObjectIdGetDatum(rel ? RelationGetRelid(rel) : InvalidOid)));
}

/* bin (an expression's node tree as text) printed, as it prints read back. */
static char *
printed_expr(Datum bin, Relation rel, Form_pg_attribute att,
			 Form_pg_type domain, ParseExprKind kind)
{
	char	   *src = TextDatumGetCString(DirectFunctionCall2(pg_get_expr, bin,
															   ObjectIdGetDatum(rel ? RelationGetRelid(rel) : InvalidOid)));

	return reprinted(src, rel, att, domain, kind);
}

/*
 * A check constraint's definition as pg_get_constraintdef prints it, with
 * its expression as it prints read back (see reprinted); other constraints'
 * as printed.  It prints as CHECK (expression) and then words (NO INHERIT,
 * NOT VALID and the like) without parentheses, so the expression ends at
 * its last; a definition printed otherwise is taken as printed.
 */
static char *
constraint_text(HeapTuple tup, Relation rel, Form_pg_type domain)
{
	Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tup);
	char	   *def = TextDatumGetCString(DirectFunctionCall1(pg_get_constraintdef,
															  ObjectIdGetDatum(con->oid)));
	const char *head = "CHECK (";
	char	   *end;

	if (con->contype != CONSTRAINT_CHECK || strncmp(def, head, strlen(head)) != 0 ||
		(end = strrchr(def, ')')) == NULL || end < def + strlen(head))
		return def;
	return psprintf("CHECK (%s)%s",
					reprinted(pnstrdup(def + strlen(head), end - def - strlen(head)),
							  rel, NULL, domain,
							  domain ? EXPR_KIND_DOMAIN_CHECK : EXPR_KIND_CHECK_CONSTRAINT),
					end + 1);
}

/* A table's policies section's lines for its row security being on. */
#define ROW_SECURITY		"row level security"
#define FORCED_ROW_SECURITY "forced row level security"

/*
 * rel's row security policies, sorted, one per line, each as CREATE POLICY
 * makes it (Postgres has no deparser for policies; pg_dump writes them
 * so): permissive or restrictive, its command, its roles by name (sorted),
 * its expressions.
 */
static void
append_policies(StringInfo out, Relation rel)
{
	Relation	prel = table_open(PolicyRelationId, AccessShareLock);
	ScanKeyData skey;
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *defs = NIL;
	ListCell   *lc;
	char	   *table = quote_qualified_identifier(get_namespace_name(RelationGetNamespace(rel)),
												   RelationGetRelationName(rel));

	ScanKeyInit(&skey, Anum_pg_policy_polrelid, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	scan = systable_beginscan(prel, PolicyPolrelidPolnameIndexId, true, NULL, 1,
							  &skey);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_policy pol = (Form_pg_policy) GETSTRUCT(tup);
		StringInfoData def;
		Datum		d;
		bool		isnull;
		Datum	   *roles;
		int			nroles;
		List	   *names = NIL;

		initStringInfo(&def);
		appendStringInfo(&def, "CREATE POLICY %s ON %s AS %s FOR %s TO ",
						 quote_identifier(NameStr(pol->polname)), table,
						 pol->polpermissive ? "PERMISSIVE" : "RESTRICTIVE",
						 pol->polcmd == ACL_SELECT_CHR ? "SELECT" :
						 pol->polcmd == ACL_INSERT_CHR ? "INSERT" :
						 pol->polcmd == ACL_UPDATE_CHR ? "UPDATE" :
						 pol->polcmd == ACL_DELETE_CHR ? "DELETE" : "ALL");
		d = heap_getattr(tup, Anum_pg_policy_polroles, RelationGetDescr(prel), &isnull);
		deconstruct_array_builtin(DatumGetArrayTypeP(d), OIDOID, &roles, NULL, &nroles);
		for (int i = 0; i < nroles; i++)
			names = lappend(names, DatumGetObjectId(roles[i]) == ACL_ID_PUBLIC ?
							pstrdup("PUBLIC") :
							pstrdup(quote_identifier(GetUserNameFromId(DatumGetObjectId(roles[i]),
																	   false))));
		list_sort(names, cstring_cmp);
		foreach(lc, names)
			appendStringInfo(&def, "%s%s", foreach_current_index(lc) > 0 ? ", " : "",
							 (char *) lfirst(lc));
		d = heap_getattr(tup, Anum_pg_policy_polqual, RelationGetDescr(prel), &isnull);
		if (!isnull)
			appendStringInfo(&def, " USING (%s)",
							 printed_expr(d, rel, NULL, NULL, EXPR_KIND_POLICY));
		d = heap_getattr(tup, Anum_pg_policy_polwithcheck, RelationGetDescr(prel), &isnull);
		if (!isnull)
			appendStringInfo(&def, " WITH CHECK (%s)",
							 printed_expr(d, rel, NULL, NULL, EXPR_KIND_POLICY));
		defs = lappend(defs, def.data);
	}
	systable_endscan(scan);
	table_close(prel, AccessShareLock);
	list_sort(defs, cstring_cmp);
	foreach(lc, defs)
	{
		append_field(out, lfirst(lc));
		appendStringInfoChar(out, '\n');
	}
}

/*
 * A line of a table's "local" section: a column or constraint it inherits
 * that is its own too (attislocal, conislocal), by name.
 */
static char *
local_line(bool column, const char *name)
{
	StringInfoData line;

	initStringInfo(&line);
	appendStringInfoString(&line, column ? "column " : "constraint ");
	append_field(&line, quote_identifier(name));
	return line.data;
}

/*
 * Is con a foreign key Postgres made for one of the table's own, for a
 * partition of the partitioned table it references?  It comes and goes
 * with that one, which describes it.
 */
static bool
foreign_key_clone(Form_pg_constraint con)
{
	HeapTuple	tup;
	bool		clone;

	if (!OidIsValid(con->conparentid))
		return false;
	tup = SearchSysCache1(CONSTROID, ObjectIdGetDatum(con->conparentid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for constraint %u", con->conparentid);
	clone = ((Form_pg_constraint) GETSTRUCT(tup))->conrelid == con->conrelid;
	ReleaseSysCache(tup);
	return clone;
}

/* Explicit options, by name, independent of the order they were set in. */
static List *
table_options(List *options, Oid relid, const char *prefix)
{
	HeapTuple	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	bool		isnull;
	Datum		value;
	ListCell   *lc;

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	value = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions, &isnull);
	if (!isnull)
	{
		foreach(lc, untransformRelOptions(value))
		{
			DefElem    *def = lfirst_node(DefElem, lc);
			StringInfoData line;

			initStringInfo(&line);
			append_field(&line, psprintf("%s%s", prefix, quote_identifier(def->defname)));
			appendStringInfoChar(&line, '\t');
			append_field(&line, quote_literal_cstr(defGetString(def)));
			options = lappend(options, line.data);
		}
	}
	ReleaseSysCache(tuple);
	return options;
}

static const char *
column_storage(char storage)
{
	switch (storage)
	{
		case TYPSTORAGE_PLAIN:
			return "PLAIN";
		case TYPSTORAGE_EXTERNAL:
			return "EXTERNAL";
		case TYPSTORAGE_EXTENDED:
			return "EXTENDED";
		case TYPSTORAGE_MAIN:
			return "MAIN";
		default:
			elog(ERROR, "unexpected column storage %c", storage);
	}
	return NULL;
}

/*
 * A column's settings that aren't its type's or the server's defaults, as
 * fields: storage, compression method, statistics target.
 */
static void
append_column_settings(StringInfo out, Relation rel, Form_pg_attribute att)
{
	HeapTuple	atup;
	bool		isnull;
	Datum		target;

	if (att->attstorage != get_typstorage(att->atttypid))
		appendStringInfo(out, "\tstorage %s", column_storage(att->attstorage));
	if (CompressionMethodIsValid(att->attcompression))
		appendStringInfo(out, "\tcompression %s", GetCompressionMethodName(att->attcompression));
	/* (the statistics target is nullable, outside Form_pg_attribute) */
	atup = SearchSysCache2(ATTNUM, ObjectIdGetDatum(RelationGetRelid(rel)),
						   Int16GetDatum(att->attnum));
	if (!HeapTupleIsValid(atup))
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 att->attnum, RelationGetRelid(rel));
	target = SysCacheGetAttr(ATTNUM, atup, Anum_pg_attribute_attstattarget, &isnull);
	if (!isnull)
		appendStringInfo(out, "\tstatistics %d", DatumGetInt16(target));
	ReleaseSysCache(atup);
}

/* A replica identity by an index: this, then the index's name. */
#define IDENTITY_BY_INDEX "USING INDEX "

/*
 * A table's (or materialized view's) explicit options, sorted, its TOAST
 * table's prefixed "toast.".  Its TOAST table's only if it needs one: one
 * left over from columns since dropped isn't made again with the table, so
 * its options wouldn't be either.
 */
static List *
described_options(Relation rel)
{
	List	   *options = table_options(NIL, RelationGetRelid(rel), "");

	if (OidIsValid(rel->rd_rel->reltoastrelid) && table_relation_needs_toast_table(rel))
		options = table_options(options, rel->rd_rel->reltoastrelid, "toast.");
	list_sort(options, cstring_cmp);
	return options;
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
		append_field(out, quote_identifier(NameStr(att->attname)));
		appendStringInfoChar(out, '\t');
		append_field(out, format_type_extended(att->atttypid, att->atttypmod,
											   FORMAT_TYPE_TYPEMOD_GIVEN |
											   FORMAT_TYPE_FORCE_QUALIFY));
		if (att->attnotnull)
			appendStringInfoString(out, "\tnot null");
		if (OidIsValid(att->attcollation) &&
			att->attcollation != get_typcollation(att->atttypid))
		{
			appendStringInfoString(out, "\tcollate ");
			append_field(out, generate_collation_name(att->attcollation));
		}
		if (att->atthasdef && desc->constr)
		{
			for (int d = 0; d < desc->constr->num_defval; d++)
			{
				AttrDefault *def = &desc->constr->defval[d];

				if (def->adnum == att->attnum)
				{
					appendStringInfoString(out, att->attgenerated ?
										   "\tgenerated " : "\tdefault ");
					append_field(out, printed_expr(CStringGetTextDatum(def->adbin),
												   rel, att, NULL, EXPR_KIND_NONE));
				}
			}
		}
		if (att->attidentity)
			appendStringInfo(out, "\tidentity %c", att->attidentity);
		if (att->attgenerated)
			appendStringInfo(out, "\tstored %c", att->attgenerated);
		append_column_settings(out, rel, att);
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
		StringInfo	line;

		/* (a partition's from its partitioned table: the table's) */
		if (rel->rd_rel->relispartition && !con->conislocal)
			continue;
		/* (one Postgres made for a referenced table's partition: its foreign key's) */
		if (con->contype == CONSTRAINT_FOREIGN && foreign_key_clone(con))
			continue;
		line = makeStringInfo();
		append_field(line, quote_identifier(NameStr(con->conname)));
		appendStringInfoChar(line, '\t');
		append_field(line, constraint_text(tup, rel, NULL));
		appendStringInfoChar(line, '\n');
		cons = lappend(cons, line->data);
	}
	systable_endscan(scan);
	table_close(conrel, AccessShareLock);
	list_sort(cons, cstring_cmp);	/* constraint names are unique */
	foreach(lc, cons)
		appendStringInfoString(out, lfirst(lc));

	/*
	 * Indexes, by name, without their tablespaces: tablespaces are the
	 * cluster's, so a table's isn't described either (and a move keeps an
	 * index it makes again where it was: index_places()).
	 */
	appendStringInfoString(out, "indexes\n");
	indexes = list_copy(RelationGetIndexList(rel));
	list_sort(indexes, oid_name_cmp);
	foreach(lc, indexes)
	{
		Oid			idx = lfirst_oid(lc);

		/* (a partition's made for its partitioned table's: the table's) */
		if (get_rel_relispartition(idx))
			continue;
		append_field(out, pg_get_indexdef_string_notblspc(idx));
		appendStringInfoChar(out, '\n');
	}

	/* Triggers and rules, by name. */
	appendStringInfoString(out, "triggers\n");
	append_sorted_defs(out, TriggerRelationId, Anum_pg_trigger_tgrelid,
					   TriggerRelidNameIndexId, RelationGetRelid(rel));
	appendStringInfoString(out, "rules\n");
	append_sorted_defs(out, RewriteRelationId, Anum_pg_rewrite_ev_class,
					   RewriteRelRulenameIndexId, RelationGetRelid(rel));

	/*
	 * Row security: whether it is on (and forced), then policies by name.
	 * (A table with none has no section, as before policies were held: its
	 * description is the same.)
	 */
	{
		StringInfoData policies;

		initStringInfo(&policies);
		if (rel->rd_rel->relrowsecurity)
			appendStringInfoString(&policies, ROW_SECURITY "\n");
		if (rel->rd_rel->relforcerowsecurity)
			appendStringInfoString(&policies, FORCED_ROW_SECURITY "\n");
		append_policies(&policies, rel);
		if (policies.len > 0)
			appendStringInfo(out, "policies\n%s", policies.data);
	}

	/* Extended statistics on it, by name (a section only if it has any). */
	append_statistics(out, rel);

	/* A partitioned table's key; a partition's parent and bound. */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		appendStringInfoString(out, "partition by\n");
		append_field(out, TextDatumGetCString(DirectFunctionCall1(pg_get_partkeydef,
																  ObjectIdGetDatum(RelationGetRelid(rel)))));
		appendStringInfoChar(out, '\n');
	}
	if (rel->rd_rel->relispartition)
	{
		Oid			parent = get_partition_parent(RelationGetRelid(rel), false);
		HeapTuple	ctup = SearchSysCache1(RELOID, ObjectIdGetDatum(RelationGetRelid(rel)));
		bool		isnull;
		Datum		bound;

		if (!HeapTupleIsValid(ctup))
			elog(ERROR, "cache lookup failed for relation %u", RelationGetRelid(rel));
		bound = SysCacheGetAttr(RELOID, ctup, Anum_pg_class_relpartbound, &isnull);
		if (isnull)
			elog(ERROR, "partition %u has no bound", RelationGetRelid(rel));
		appendStringInfoString(out, "partition of\n");
		append_field(out, qualified(get_rel_namespace(parent), get_rel_name(parent)));
		appendStringInfoChar(out, '\t');
		append_field(out, TextDatumGetCString(DirectFunctionCall2(pg_get_expr, bound,
																  ObjectIdGetDatum(RelationGetRelid(rel)))));
		appendStringInfoChar(out, '\n');
		ReleaseSysCache(ctup);
	}

	/*
	 * The tables it inherits from (not a partition's partitioned table), in
	 * order; a typed table's type.  (A section only if it has them.)
	 */
	if (!rel->rd_rel->relispartition)
	{
		Relation	inhrel = table_open(InheritsRelationId, AccessShareLock);
		bool		any = false;

		ScanKeyInit(&skey, Anum_pg_inherits_inhrelid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(rel)));
		scan = systable_beginscan(inhrel, InheritsRelidSeqnoIndexId, true, NULL, 1,
								  &skey);
		while ((tup = systable_getnext(scan)) != NULL)
		{
			Oid			parent = ((Form_pg_inherits) GETSTRUCT(tup))->inhparent;

			if (!any)
				appendStringInfoString(out, "inherits\n");
			any = true;
			append_field(out, qualified(get_rel_namespace(parent), get_rel_name(parent)));
			appendStringInfoChar(out, '\n');
		}
		systable_endscan(scan);
		table_close(inhrel, AccessShareLock);
	}
	if (OidIsValid(rel->rd_rel->reloftype))
	{
		appendStringInfoString(out, "of\n");
		append_field(out, format_type_extended(rel->rd_rel->reloftype, -1,
											   FORMAT_TYPE_FORCE_QUALIFY));
		appendStringInfoChar(out, '\n');
	}

	/*
	 * What it inherits that is its own too (declared by it, or its before
	 * it inherited it): a parent's DROP COLUMN or DROP CONSTRAINT leaves
	 * those.  Only an inheritance child has any (a section only if so;
	 * a partition's are its partitioned table's).
	 */
	if (!rel->rd_rel->relispartition)
	{
		List	   *local = NIL;

		for (int i = 0; i < desc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(desc, i);

			if (!att->attisdropped && att->attinhcount > 0 && att->attislocal)
				local = lappend(local, local_line(true, NameStr(att->attname)));
		}
		conrel = table_open(ConstraintRelationId, AccessShareLock);
		ScanKeyInit(&skey, Anum_pg_constraint_conrelid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(rel)));
		scan = systable_beginscan(conrel, ConstraintRelidTypidNameIndexId, true,
								  NULL, 1, &skey);
		while ((tup = systable_getnext(scan)) != NULL)
		{
			Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tup);

			if (con->coninhcount > 0 && con->conislocal)
				local = lappend(local, local_line(false, NameStr(con->conname)));
		}
		systable_endscan(scan);
		table_close(conrel, AccessShareLock);
		list_sort(local, cstring_cmp);
		if (local != NIL)
			appendStringInfoString(out, "local\n");
		foreach(lc, local)
			appendStringInfo(out, "%s\n", (char *) lfirst(lc));
	}

	/* Local placement is not versioned; explicit tuning options are. */
	{
		List	   *options = described_options(rel);

		if (options != NIL)
			appendStringInfoString(out, "options\n");
		foreach(lc, options)
			appendStringInfo(out, "%s\n", (char *) lfirst(lc));
	}
	if (rel->rd_rel->relreplident != REPLICA_IDENTITY_DEFAULT)
	{
		Oid			index = RelationGetReplicaIndex(rel);

		appendStringInfoString(out, "replica identity\n");
		if (rel->rd_rel->relreplident == REPLICA_IDENTITY_FULL)
			appendStringInfoString(out, "FULL");
		else if (OidIsValid(index))
		{
			appendStringInfoString(out, IDENTITY_BY_INDEX);
			append_field(out, quote_identifier(get_rel_name(index)));
		}
		else
		{
			/* A dropped identity index leaves mode i, behaving as NOTHING. */
			appendStringInfoString(out, "NOTHING");
		}
		appendStringInfoChar(out, '\n');
	}
}

/*
 * A view's options, if any, before its query: "WITH (...) AS", as CREATE
 * VIEW takes them, each one set (check_option, security_barrier,
 * security_invoker), in that order.  (A query as Postgres's deparser
 * prints it begins with a space or a parenthesis: never so.)
 */
static void
append_view_options(StringInfo out, Oid view)
{
	Relation	rel = relation_open(view, AccessShareLock);
	List	   *opts = NIL;
	ListCell   *lc;

	if (RelationHasCheckOption(rel))
		opts = lappend(opts, RelationHasLocalCheckOption(rel) ?
					   "check_option=local" : "check_option=cascaded");
	if (RelationIsSecurityView(rel))
		opts = lappend(opts, "security_barrier=true");
	if (RelationHasSecurityInvoker(rel))
		opts = lappend(opts, "security_invoker=true");
	relation_close(rel, AccessShareLock);
	if (opts == NIL)
		return;
	appendStringInfoString(out, "WITH (");
	foreach(lc, opts)
		appendStringInfo(out, "%s%s", foreach_current_index(lc) ? ", " : "",
						 (char *) lfirst(lc));
	appendStringInfoString(out, ") AS");
}

/* A materialized view's line before its query for its being unpopulated. */
#define MATVIEW_UNPOPULATED "with no data"

/* A materialized view's lines for its options and its columns' settings. */
#define MATVIEW_OPTION "option\t"
#define MATVIEW_COLUMN "column\t"

/*
 * What a materialized view has besides its query, a line each before it:
 * whether it is unpopulated (made WITH NO DATA, not refreshed since), then
 * its indexes by name, as a table's are, then its explicit options (as a
 * table's) and the columns with settings that aren't their defaults (each
 * as a table's column line, its name and those).  (None begins with a
 * space or a parenthesis, as the query does.)
 */
static void
append_matview_state(StringInfo out, Form_pg_class form)
{
	Relation	rel = relation_open(form->oid, AccessShareLock);
	List	   *indexes = list_copy(RelationGetIndexList(rel));
	TupleDesc	tupdesc = RelationGetDescr(rel);
	StringInfoData settings;
	ListCell   *lc;

	if (!form->relispopulated)
		appendStringInfoString(out, MATVIEW_UNPOPULATED "\n");
	list_sort(indexes, oid_name_cmp);
	foreach(lc, indexes)
	{
		append_field(out, pg_get_indexdef_string_notblspc(lfirst_oid(lc)));
		appendStringInfoChar(out, '\n');
	}
	foreach(lc, described_options(rel))
		appendStringInfo(out, MATVIEW_OPTION "%s\n", (char *) lfirst(lc));
	initStringInfo(&settings);
	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			continue;
		resetStringInfo(&settings);
		append_column_settings(&settings, rel, att);
		if (settings.len == 0)
			continue;
		appendStringInfoString(out, MATVIEW_COLUMN);
		append_field(out, quote_identifier(NameStr(att->attname)));
		appendStringInfo(out, "%s\n", settings.data);
	}
	pfree(settings.data);
	relation_close(rel, AccessShareLock);
}

/* ------------------------------------------------------------------------
 * Types, sequences, extensions
 * ------------------------------------------------------------------------
 */

static char *qualified_function(Oid fn);
static void refuse_undescribed(Canon *cn);
static void describe_casts(Canon *cn);
static void describe_collations(Canon *cn);
static bool held_kind(Oid classid);

/*
 * Is an object part of a type (a range type's constructors, its cast to its
 * multirange type)?
 */
static bool
part_of_type(Oid classid, Oid objid)
{
	Relation	depRel = table_open(DependRelationId, AccessShareLock);
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	bool		found = false;

	ScanKeyInit(&key[0], Anum_pg_depend_classid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(classid));
	ScanKeyInit(&key[1], Anum_pg_depend_objid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(objid));
	scan = systable_beginscan(depRel, DependDependerIndexId, true, NULL, 2, key);
	while (!found && (tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_depend d = (Form_pg_depend) GETSTRUCT(tup);

		found = d->deptype == DEPENDENCY_INTERNAL && d->refclassid == TypeRelationId;
	}
	systable_endscan(scan);
	table_close(depRel, AccessShareLock);
	return found;
}

/*
 * rel's extended statistics objects, sorted, one per line: its qualified
 * name, the CREATE STATISTICS that makes it (as Postgres's deparser prints
 * it), and its statistics target if one was set.  None: no section.
 */
static void
append_statistics(StringInfo out, Relation rel)
{
	Relation	srel = table_open(StatisticExtRelationId, AccessShareLock);
	ScanKeyData skey;
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *lines = NIL;
	ListCell   *lc;

	ScanKeyInit(&skey, Anum_pg_statistic_ext_stxrelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(rel)));
	scan = systable_beginscan(srel, StatisticExtRelidIndexId, true, NULL, 1, &skey);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_statistic_ext stx = (Form_pg_statistic_ext) GETSTRUCT(tup);
		StringInfoData line;
		bool		isnull;
		Datum		target = heap_getattr(tup, Anum_pg_statistic_ext_stxstattarget,
										  RelationGetDescr(srel), &isnull);

		initStringInfo(&line);
		append_field(&line, qualified(stx->stxnamespace, NameStr(stx->stxname)));
		appendStringInfoChar(&line, '\t');
		append_field(&line, pg_get_statisticsobjdef_string(stx->oid));
		if (!isnull)
			appendStringInfo(&line, "\t%d", DatumGetInt16(target));
		lines = lappend(lines, line.data);
	}
	systable_endscan(scan);
	table_close(srel, AccessShareLock);
	if (lines == NIL)
		return;
	list_sort(lines, cstring_cmp);
	appendStringInfoString(out, "statistics\n");
	foreach(lc, lines)
	{
		appendStringInfoString(out, lfirst(lc));
		appendStringInfoChar(out, '\n');
	}
}

/*
 * A range type: "range", then its subtype, the subtype's operator class,
 * its collation (empty if none), its subtype difference function (empty if
 * none) and its multirange type, a line each, as CREATE TYPE ... AS RANGE
 * names them.  Its multirange type, constructors and cast come with it.  A
 * canonical function is written in C, and is made with a shell type the
 * range fills: not held.  Nor is a range whose subtype difference
 * function, operator class or collation is of a versioned schema, which a
 * move would make after the type.
 */
static void
describe_range(Canon *cn, Form_pg_type typ)
{
	HeapTuple	tup = SearchSysCache1(RANGETYPE, ObjectIdGetDatum(typ->oid));
	Form_pg_range rng;
	char	   *name = qualified(typ->typnamespace, NameStr(typ->typname));
	CanonItem  *it;
	ObjectAddress self;

	ObjectAddressSet(self, TypeRelationId, typ->oid);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for range type %u", typ->oid);
	rng = (Form_pg_range) GETSTRUCT(tup);
	if (OidIsValid(rng->rngcanonical))
	{
		ReleaseSysCache(tup);
		(void) uncovered(cn, "range types with a canonical function", name, &self);
		return;
	}
	if (OidIsValid(rng->rngsubdiff) &&
		canon_namespace(get_func_namespace(rng->rngsubdiff)))
	{
		ReleaseSysCache(tup);
		(void) uncovered(cn, "range types whose subtype difference function is versioned", name, &self);
		return;
	}
	{
		ObjectAddress opc;
		ObjectAddress coll;

		ObjectAddressSet(opc, OperatorClassRelationId, rng->rngsubopc);
		ObjectAddressSet(coll, CollationRelationId, rng->rngcollation);
		if (canon_namespace(get_object_namespace(&opc)) ||
			(OidIsValid(rng->rngcollation) &&
			 canon_namespace(get_object_namespace(&coll))))
		{
			ReleaseSysCache(tup);
			(void) uncovered(cn, "range types whose operator class or collation is versioned", name, &self);
			return;
		}
	}
	it = add_item(cn, "y:", name);
	appendStringInfoString(&it->value, "range\n");
	append_field(&it->value, format_type_extended(rng->rngsubtype, -1,
												  FORMAT_TYPE_FORCE_QUALIFY));
	appendStringInfoChar(&it->value, '\n');
	{
		HeapTuple	ctup = SearchSysCache1(CLAOID, ObjectIdGetDatum(rng->rngsubopc));
		Form_pg_opclass opc;

		if (!HeapTupleIsValid(ctup))
			elog(ERROR, "cache lookup failed for operator class %u", rng->rngsubopc);
		opc = (Form_pg_opclass) GETSTRUCT(ctup);
		append_field(&it->value, qualified(opc->opcnamespace, NameStr(opc->opcname)));
		ReleaseSysCache(ctup);
	}
	appendStringInfoChar(&it->value, '\n');
	if (OidIsValid(rng->rngcollation))
		append_field(&it->value, generate_collation_name(rng->rngcollation));
	appendStringInfoChar(&it->value, '\n');
	if (OidIsValid(rng->rngsubdiff))
		append_field(&it->value, qualified_function(rng->rngsubdiff));
	appendStringInfoChar(&it->value, '\n');
	append_field(&it->value, format_type_extended(rng->rngmultitypid, -1,
												  FORMAT_TYPE_FORCE_QUALIFY));
	appendStringInfoChar(&it->value, '\n');
	ReleaseSysCache(tup);
}

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
			append_field(&it->value, quote_literal_cstr(arr[i].name));
			appendStringInfoChar(&it->value, '\n');
		}
	}
	else if (typ->typtype == TYPTYPE_DOMAIN)
	{
		it = add_item(cn, "y:", qualified(typ->typnamespace, NameStr(typ->typname)));
		appendStringInfoString(&it->value, "domain\n");
		append_field(&it->value, format_type_extended(typ->typbasetype,
													  typ->typtypmod,
													  FORMAT_TYPE_TYPEMOD_GIVEN |
													  FORMAT_TYPE_FORCE_QUALIFY));
		if (typ->typnotnull)
			appendStringInfoString(&it->value, "\tnot null");
		/* (its collation if not its base type's, its default: if any) */
		if (OidIsValid(typ->typcollation) &&
			typ->typcollation != get_typcollation(typ->typbasetype))
		{
			appendStringInfoString(&it->value, "\tcollate ");
			append_field(&it->value, generate_collation_name(typ->typcollation));
		}
		{
			HeapTuple	ttup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typ->oid));
			bool		isnull;
			Datum		def;

			if (!HeapTupleIsValid(ttup))
				elog(ERROR, "cache lookup failed for type %u", typ->oid);
			def = SysCacheGetAttr(TYPEOID, ttup, Anum_pg_type_typdefaultbin, &isnull);
			if (!isnull)
			{
				appendStringInfoString(&it->value, "\tdefault ");
				append_field(&it->value,
							 TextDatumGetCString(DirectFunctionCall2(pg_get_expr, def,
																	 ObjectIdGetDatum(InvalidOid))));
			}
			ReleaseSysCache(ttup);
		}
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
				StringInfoData line;

				initStringInfo(&line);
				append_field(&line, quote_identifier(NameStr(con->conname)));
				appendStringInfoChar(&line, '\t');
				append_field(&line, constraint_text(tup, NULL, typ));
				appendStringInfoChar(&line, '\n');
				cons = lappend(cons, line.data);
			}
			systable_endscan(scan);
			table_close(conrel, AccessShareLock);
			list_sort(cons, cstring_cmp);
			foreach(lc, cons)
				appendStringInfoString(&it->value, lfirst(lc));
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
			append_field(&it->value, quote_identifier(NameStr(att->attname)));
			appendStringInfoChar(&it->value, '\t');
			append_field(&it->value, format_type_extended(att->atttypid,
														  att->atttypmod,
														  FORMAT_TYPE_TYPEMOD_GIVEN |
														  FORMAT_TYPE_FORCE_QUALIFY));
			/* (as a table's column: its collation if not its type's) */
			if (OidIsValid(att->attcollation) &&
				att->attcollation != get_typcollation(att->atttypid))
			{
				appendStringInfoString(&it->value, "\tcollate ");
				append_field(&it->value, generate_collation_name(att->attcollation));
			}
			appendStringInfoChar(&it->value, '\n');
		}
		relation_close(crel, AccessShareLock);
	}
	else if (typ->typtype == TYPTYPE_RANGE)
		describe_range(cn, typ);
	else if (typ->typtype == TYPTYPE_MULTIRANGE)
		return;					/* (with its range type) */
	else if (typ->typtype == TYPTYPE_BASE)
	{
		ObjectAddress self;

		/* Array types of covered types come with them. */
		if (OidIsValid(typ->typelem) && typ->typcategory == TYPCATEGORY_ARRAY)
			return;
		ObjectAddressSet(self, TypeRelationId, typ->oid);
		(void) uncovered(cn, "this type", qualified(typ->typnamespace, NameStr(typ->typname)),
						 &self);
	}
}

static void
describe_sequence(Canon *cn, Oid seqid, Oid nsp, const char *name)
{
	HeapTuple	tup = SearchSysCache1(SEQRELID, ObjectIdGetDatum(seqid));
	Form_pg_sequence seq;
	CanonItem  *it;
	int64		last;
	bool		called;

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
	if (ReadSequenceState(seqid, &last, &called))
		appendStringInfo(&it->value, INT64_FORMAT "\t%s\n", last,
						 called ? "t" : "f");
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

/* An extension's version, from its catalog row. */
static char *
extension_version(Oid extoid)
{
	Relation	rel = table_open(ExtensionRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;
	char	   *version = NULL;

	ScanKeyInit(&key, Anum_pg_extension_oid, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(extoid));
	scan = systable_beginscan(rel, ExtensionOidIndexId, true, NULL, 1, &key);
	if ((tup = systable_getnext(scan)) != NULL)
	{
		bool		isnull;
		Datum		d = heap_getattr(tup, Anum_pg_extension_extversion,
									 RelationGetDescr(rel), &isnull);

		if (!isnull)
			version = TextDatumGetCString(d);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	if (version == NULL)
		elog(ERROR, "cache lookup failed for extension %u", extoid);
	return version;
}

/* A list of strings as a text[] literal. */
static char *
text_array_literal(List *strings)
{
	Datum	   *elems = palloc(sizeof(Datum) * Max(list_length(strings), 1));
	int			n = 0;
	ListCell   *lc;

	foreach(lc, strings)
		elems[n++] = CStringGetTextDatum(lfirst(lc));
	/* (through fmgr: array_out keeps state in its flinfo) */
	return OidOutputFunctionCall(F_ARRAY_OUT,
								 PointerGetDatum(construct_array_builtin(elems, n, TEXTOID)));
}

static char *from_utf8(const uint8 *data, int len);

/* A dependency, as a commit records it (UTF-8). */
typedef struct Dependency
{
	StringInfoData key;
	StringInfoData value;
} Dependency;

static int
dependency_cmp(const ListCell *a, const ListCell *b)
{
	const Dependency *x = lfirst(a);
	const Dependency *y = lfirst(b);
	ProllyKey	kx = {(const uint8 *) x->key.data, x->key.len};
	ProllyKey	ky = {(const uint8 *) y->key.data, y->key.len};

	return prolly_bytes_compare(&kx, &ky, NULL);
}

/*
 * Describe a dependency outside the versioned schemas: its key, kind and
 * identity; its value, an extension's version and schema or another
 * object's address.
 */
static Dependency *
describe_dependency(const ObjectAddress *addr)
{
	Dependency *dep = palloc(sizeof(Dependency));
	char	   *type = getObjectTypeDescription(addr, false);

	initStringInfo(&dep->key);
	initStringInfo(&dep->value);
	append_utf8(&dep->key, psprintf("%s %s", type, getObjectIdentity(addr, false)));
	if (addr->classId == ExtensionRelationId)
	{
		Oid			nsp = get_extension_schema(addr->objectId);

		append_utf8(&dep->value,
					text_array_literal(list_make2(extension_version(addr->objectId),
												  get_namespace_name(nsp))));
	}
	else
	{
		List	   *names = NIL;
		List	   *objargs = NIL;

		(void) getObjectIdentityParts(addr, &names, &objargs, false);
		append_utf8(&dep->value,
					text_array_literal(list_make3(type,
												  text_array_literal(names),
												  text_array_literal(objargs))));
	}
	return dep;
}

/*
 * Whether an object is one the fold covers, by collect()'s criteria: in a
 * versioned schema (a table's default, trigger or rule, its table's), and
 * not an extension's member.
 */
static bool
depender_is_canonical(const ObjectAddress *object)
{
	Oid			nsp;

	/*
	 * (what a commit doesn't hold needs nothing for it: an operator class's
	 * entries, say)
	 */
	if (object->classId == AccessMethodOperatorRelationId ||
		object->classId == AccessMethodProcedureRelationId ||
		(is_objectclass_supported(object->classId) &&
		 get_object_attnum_namespace(object->classId) != InvalidAttrNumber &&
		 !held_kind(object->classId)))
		return false;
	if (!object_is_versioned(object) ||
		in_extension(object->classId, object->objectId))
		return false;
	if (object->classId == RelationRelationId)
		nsp = get_rel_namespace(object->objectId);
	else if (is_objectclass_supported(object->classId))
		nsp = get_object_namespace(object);
	else
		nsp = InvalidOid;		/* (a default, trigger or rule: its table's) */
	return !OidIsValid(nsp) || canon_namespace(nsp);
}

/*
 * The tree of what the versioned objects depend on outside the versioned
 * schemas, for a commit.  What initdb made (OIDs below FirstNormalObjectId)
 * every database has, and isn't recorded; an object an extension made, or
 * one in a versioned schema, doesn't depend on anything for the commit.
 * Each dependency is described once, however many objects need it.
 */
static void
folded_fold_dependencies(ProllyNodeRef *out)
{
	Canon		cn;
	Relation	rel;
	SysScanDesc scan;
	HeapTuple	tup;
	HASHCTL		ctl;
	HTAB	   *seen;
	List	   *deps = NIL;
	ListCell   *lc;
	ProllyTreeBuilder *b;

	memset(&cn, 0, sizeof(cn));
	canon_env(&cn);
	ctl.keysize = sizeof(ObjectAddress);
	ctl.entrysize = sizeof(ObjectAddress);
	ctl.hcxt = CurrentMemoryContext;
	seen = hash_create("prolly dependencies", 64, &ctl,
					   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	rel = table_open(DependRelationId, AccessShareLock);
	scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_depend d = (Form_pg_depend) GETSTRUCT(tup);
		ObjectAddress depender;
		ObjectAddress referenced;
		Oid			ext;
		bool		found;

		/* (an extension's members are its own, not the commit's) */
		if (d->deptype == DEPENDENCY_EXTENSION ||
			d->objid < FirstNormalObjectId || d->refobjid < FirstNormalObjectId)
			continue;
		ObjectAddressSet(depender, d->classid, d->objid);
		if (!depender_is_canonical(&depender))
			continue;
		ObjectAddressSet(referenced, d->refclassid, d->refobjid);
		if (object_is_versioned(&referenced))
			continue;
		ext = getExtensionOfObject(referenced.classId, referenced.objectId);
		if (OidIsValid(ext))
			ObjectAddressSet(referenced, ExtensionRelationId, ext);
		(void) hash_search(seen, &referenced, HASH_ENTER, &found);
		if (!found)
			deps = lappend(deps, describe_dependency(&referenced));
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	list_sort(deps, dependency_cmp);
	b = PROLLY_CALL(cn.tree, build_begin, &cn.env, CurrentMemoryContext);
	foreach(lc, deps)
	{
		Dependency *dep = lfirst(lc);
		ProllyKey	k = {(const uint8 *) dep->key.data, dep->key.len};
		ProllyValue v = {(const uint8 *) dep->value.data, dep->value.len};

		/* (two objects with one identity: the first) */
		if (foreach_current_index(lc) > 0 &&
			dependency_cmp(lc, list_nth_cell(deps, foreach_current_index(lc) - 1)) == 0)
			continue;
		PROLLY_CALL(cn.tree, build_add, b, &k, &v);
	}
	PROLLY_CALL(cn.tree, build_finish, b, out);
	hash_destroy(seen);
}

/* The dependencies a commit records: each, by its key and value. */
static void
folded_dependencies(const ProllyNodeRef *tree, ProllyCanonDependencyVisit visit,
					void *arg)
{
	Canon		cn;
	ProllyTreeCursor *c;
	ProllyKey	k;
	ProllyValue v;

	memset(&cn, 0, sizeof(cn));
	canon_env(&cn);
	c = PROLLY_CALL(cn.tree, cursor_open, &cn.env, tree, CurrentMemoryContext);
	PROLLY_CALL(cn.tree, cursor_seek, c, NULL, ForwardScanDirection);
	while (PROLLY_CALL(cn.tree, cursor_next, c, &k, &v))
	{
		char	   *name = from_utf8(k.data, k.len);
		char	   *value = from_utf8(v.data, v.len);

		visit(name, value, arg);
		pfree(name);
		pfree(value);
	}
	PROLLY_CALL(cn.tree, cursor_close, c);
}

/* A function, by its qualified name (an aggregate's support function). */
static char *
qualified_function(Oid fn)
{
	return qualified(get_func_namespace(fn), get_func_name(fn));
}

/* An aggregate's FINALFUNC_MODIFY or MFINALFUNC_MODIFY, as written. */
static const char *
aggmodify_name(char modify)
{
	return modify == AGGMODIFY_READ_ONLY ? "READ_ONLY" :
		modify == AGGMODIFY_SHAREABLE ? "SHAREABLE" : "READ_WRITE";
}

/*
 * An aggregate's definition, as CREATE OR REPLACE AGGREGATE writes it: every
 * option, defaults too, so a change in any of them changes the commit.
 * (Postgres has no deparser for aggregates; pg_dump writes them so.)
 */
static char *
aggregate_def(Form_pg_proc proc)
{
	HeapTuple	tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(proc->oid));
	Form_pg_aggregate agg;
	StringInfoData def;
	Datum		d;
	bool		isnull;

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for aggregate %u", proc->oid);
	agg = (Form_pg_aggregate) GETSTRUCT(tup);
	initStringInfo(&def);
	appendStringInfo(&def, "CREATE OR REPLACE AGGREGATE %s(%s) (\n SFUNC = %s,\n STYPE = %s",
					 qualified(proc->pronamespace, NameStr(proc->proname)),
					 TextDatumGetCString(DirectFunctionCall1(pg_get_function_arguments,
															 ObjectIdGetDatum(proc->oid))),
					 qualified_function(agg->aggtransfn),
					 format_type_be_qualified(agg->aggtranstype));
	if (agg->aggtransspace != 0)
		appendStringInfo(&def, ",\n SSPACE = %d", agg->aggtransspace);
	if (OidIsValid(agg->aggfinalfn))
	{
		appendStringInfo(&def, ",\n FINALFUNC = %s", qualified_function(agg->aggfinalfn));
		if (agg->aggfinalextra)
			appendStringInfoString(&def, ",\n FINALFUNC_EXTRA");
	}
	appendStringInfo(&def, ",\n FINALFUNC_MODIFY = %s", aggmodify_name(agg->aggfinalmodify));
	if (OidIsValid(agg->aggcombinefn))
		appendStringInfo(&def, ",\n COMBINEFUNC = %s", qualified_function(agg->aggcombinefn));
	if (OidIsValid(agg->aggserialfn))
		appendStringInfo(&def, ",\n SERIALFUNC = %s", qualified_function(agg->aggserialfn));
	if (OidIsValid(agg->aggdeserialfn))
		appendStringInfo(&def, ",\n DESERIALFUNC = %s", qualified_function(agg->aggdeserialfn));
	d = SysCacheGetAttr(AGGFNOID, tup, Anum_pg_aggregate_agginitval, &isnull);
	if (!isnull)
		appendStringInfo(&def, ",\n INITCOND = %s",
						 quote_literal_cstr(TextDatumGetCString(d)));
	if (OidIsValid(agg->aggmtransfn))
	{
		appendStringInfo(&def, ",\n MSFUNC = %s,\n MINVFUNC = %s,\n MSTYPE = %s",
						 qualified_function(agg->aggmtransfn),
						 qualified_function(agg->aggminvtransfn),
						 format_type_be_qualified(agg->aggmtranstype));
		if (agg->aggmtransspace != 0)
			appendStringInfo(&def, ",\n MSSPACE = %d", agg->aggmtransspace);
		if (OidIsValid(agg->aggmfinalfn))
		{
			appendStringInfo(&def, ",\n MFINALFUNC = %s", qualified_function(agg->aggmfinalfn));
			if (agg->aggmfinalextra)
				appendStringInfoString(&def, ",\n MFINALFUNC_EXTRA");
		}
		appendStringInfo(&def, ",\n MFINALFUNC_MODIFY = %s",
						 aggmodify_name(agg->aggmfinalmodify));
		d = SysCacheGetAttr(AGGFNOID, tup, Anum_pg_aggregate_aggminitval, &isnull);
		if (!isnull)
			appendStringInfo(&def, ",\n MINITCOND = %s",
							 quote_literal_cstr(TextDatumGetCString(d)));
	}
	if (OidIsValid(agg->aggsortop))
	{
		HeapTuple	op = SearchSysCache1(OPEROID, ObjectIdGetDatum(agg->aggsortop));
		Form_pg_operator opform;

		if (!HeapTupleIsValid(op))
			elog(ERROR, "cache lookup failed for operator %u", agg->aggsortop);
		opform = (Form_pg_operator) GETSTRUCT(op);
		appendStringInfo(&def, ",\n SORTOP = OPERATOR(%s.%s)",
						 quote_identifier(get_namespace_name(opform->oprnamespace)),
						 NameStr(opform->oprname));
		ReleaseSysCache(op);
	}
	if (agg->aggkind == AGGKIND_HYPOTHETICAL)
		appendStringInfoString(&def, ",\n HYPOTHETICAL");
	appendStringInfo(&def, ",\n PARALLEL = %s\n)",
					 proc->proparallel == PROPARALLEL_SAFE ? "SAFE" :
					 proc->proparallel == PROPARALLEL_RESTRICTED ? "RESTRICTED" : "UNSAFE");
	ReleaseSysCache(tup);
	return def.data;
}


/* rel's columns' names, in order. */
static List *
column_names(Oid relid)
{
	Relation	rel = table_open(relid, AccessShareLock);
	TupleDesc	desc = RelationGetDescr(rel);
	List	   *names = NIL;

	for (int i = 0; i < desc->natts; i++)
		if (!TupleDescAttr(desc, i)->attisdropped)
			names = lappend(names, pstrdup(NameStr(TupleDescAttr(desc, i)->attname)));
	table_close(rel, AccessShareLock);
	return names;
}

/*
 * Is a partition, or a partitioned table, one a commit can't hold whole
 * (and so refused)?  Partitioned tables and their partitions are held
 * together, all in versioned schemas; and a partition is made again as
 * its partitioned table's (PARTITION OF), so with its columns in that
 * table's order.
 */
static bool
partition_uncovered(Canon *cn, Form_pg_class form, const char *name)
{
	ObjectAddress self;

	ObjectAddressSet(self, RelationRelationId, form->oid);
	if (form->relispartition)
	{
		Oid			parent = get_partition_parent(form->oid, false);
		List	   *own = column_names(form->oid);
		List	   *its = column_names(parent);
		ListCell   *a;
		ListCell   *b;

		if (!canon_namespace(get_rel_namespace(parent)))
			return uncovered(cn, "partitions of tables outside versioned schemas",
							 qualified(form->relnamespace, name), &self);
		if (list_length(own) != list_length(its))
			return uncovered(cn, "partitions with columns their partitioned table lacks",
							 qualified(form->relnamespace, name), &self);
		forboth(a, own, b, its)
			if (strcmp(lfirst(a), lfirst(b)) != 0)
				return uncovered(cn, "partitions with their columns in another order than their partitioned table's",
								 qualified(form->relnamespace, name), &self);
	}
	if (form->relkind == RELKIND_PARTITIONED_TABLE)
	{
		ListCell   *lc;

		foreach(lc, find_inheritance_children(form->oid, AccessShareLock))
			if (!canon_namespace(get_rel_namespace(lfirst_oid(lc))))
				/* (not discarded: its partitions outside are every branch's) */
				return uncovered(cn, "partitioned tables with partitions outside versioned schemas",
								 qualified(form->relnamespace, name), NULL);
	}
	return false;
}

/*
 * Whether a type's values hold local OIDs, which no root may: oid and oid8,
 * the reg* types (their send functions send the OID), pg_node_tree (whose
 * text names objects by OID), and the domains, arrays, composites and
 * ranges made of them.  A large object's reference
 * is an oid, and the object itself is every branch's: versioned as the
 * number alone, a reset or merge wouldn't bring its contents back.
 */
static bool
type_holds_oids(Oid typid)
{
	Oid			base = getBaseType(typid);
	Oid			elem;

	switch (base)
	{
		case OIDOID:
		case OID8OID:
		case REGPROCOID:
		case REGPROCEDUREOID:
		case REGOPEROID:
		case REGOPERATOROID:
		case REGCLASSOID:
		case REGCOLLATIONOID:
		case REGTYPEOID:
		case REGROLEOID:
		case REGNAMESPACEOID:
		case REGDATABASEOID:
		case REGCONFIGOID:
		case REGDICTIONARYOID:
		case PG_NODE_TREEOID:
			return true;
	}
	elem = get_element_type(base);
	if (OidIsValid(elem))
		return type_holds_oids(elem);
	switch (get_typtype(base))
	{
		case TYPTYPE_COMPOSITE:
			{
				TupleDesc	desc = lookup_rowtype_tupdesc(base, -1);
				bool		holds = false;

				for (int i = 0; i < desc->natts && !holds; i++)
				{
					Form_pg_attribute att = TupleDescAttr(desc, i);

					holds = !att->attisdropped && type_holds_oids(att->atttypid);
				}
				ReleaseTupleDesc(desc);
				return holds;
			}
		case TYPTYPE_RANGE:
			return type_holds_oids(get_range_subtype(base));
		case TYPTYPE_MULTIRANGE:
			return type_holds_oids(get_multirange_range(base));
	}
	return false;
}

/*
 * Note each column of rel whose type holds OIDs as not covered; whether
 * there was one.  Reading, the table is still folded, with the numbers.
 */
static bool
oid_columns(Canon *cn, Relation rel)
{
	TupleDesc	desc = RelationGetDescr(rel);
	bool		any = false;

	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (att->attisdropped || !type_holds_oids(att->atttypid))
			continue;
		(void) uncovered(cn, "columns of types that hold OIDs",
						 psprintf("%s.%s",
								  qualified(RelationGetNamespace(rel),
											RelationGetRelationName(rel)),
								  quote_identifier(NameStr(att->attname))),
						 NULL);	/* (when reading, its table is read whole) */
		any = true;
	}
	return any;
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

		if (!canon_namespace(form->relnamespace) ||
			in_extension(RelationRelationId, form->oid))
			continue;
		switch (form->relkind)
		{
			case RELKIND_RELATION:
				if (form->relam == PROLLY_TABLE_AM_OID &&
					!partition_uncovered(cn, form, name))
				{
					Relation	rel = table_open(form->oid, AccessShareLock);
					CanonItem  *it;
					ProllyNodeRef root;
					StringInfoData desc;

					if (oid_columns(cn, rel) && !cn->reading)
					{
						table_close(rel, AccessShareLock);
						break;
					}
					it = add_item(cn, "t:", qualified(form->relnamespace, name));
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

					append_view_options(&it->value, form->oid);
					append_utf8(&it->value,
								TextDatumGetCString(DirectFunctionCall2(pg_get_viewdef_ext,
																		ObjectIdGetDatum(form->oid),
																		BoolGetDatum(false))));
				}
				break;
			case RELKIND_MATVIEW:
				{
					CanonItem  *it;
					Relation	rel = NULL;
					StringInfoData query;

					if (form->relam == PROLLY_TABLE_AM_OID)
					{
						rel = table_open(form->oid, AccessShareLock);
						if (oid_columns(cn, rel) && !cn->reading)
						{
							table_close(rel, AccessShareLock);
							break;
						}
					}
					it = add_item(cn, "m:", qualified(form->relnamespace, name));
					initStringInfo(&query);
					append_matview_state(&query, form);
					append_utf8(&query,
								TextDatumGetCString(DirectFunctionCall2(pg_get_viewdef_ext,
																		ObjectIdGetDatum(form->oid),
																		BoolGetDatum(false))));
					if (rel != NULL)
					{
						ProllyNodeRef root;

						table_root(cn, it, rel, &query, &root);
						put_hash(&it->value, &root.hash);
						table_close(rel, AccessShareLock);
					}
					else
						appendStringInfoChar(&it->value, 0);	/* (no tree) */
					appendBinaryStringInfo(&it->value, query.data, query.len);
					pfree(query.data);
				}
				break;
			case RELKIND_FOREIGN_TABLE:
				{
					ObjectAddress self;

					ObjectAddressSet(self, RelationRelationId, form->oid);
					(void) uncovered(cn, "foreign tables", qualified(form->relnamespace, name),
									 &self);
				}
				break;
			case RELKIND_PARTITIONED_TABLE:
				{
					/* (its rows are its partitions': an empty data tree) */
					Relation	rel;
					CanonItem  *it;
					ProllyNodeRef root;
					StringInfoData desc;

					if (partition_uncovered(cn, form, name))
						break;
					rel = table_open(form->oid, AccessShareLock);
					it = add_item(cn, "t:", qualified(form->relnamespace, name));
					empty_root(cn, &root);
					initStringInfo(&desc);
					describe_table(cn, rel, &desc);
					put_hash(&it->value, &root.hash);
					appendBinaryStringInfo(&it->value, desc.data, desc.len);
					pfree(desc.data);
					table_close(rel, AccessShareLock);
				}
				break;
			case RELKIND_PARTITIONED_INDEX:
				break;			/* with its table */
		}
	}
	systable_endscan(scan);
	table_close(pg_class, AccessShareLock);

	/* Versioned schemas themselves, empty ones too. */
	{
		Relation	pg_ns = table_open(NamespaceRelationId, AccessShareLock);

		scan = systable_beginscan(pg_ns, InvalidOid, false, NULL, 0, NULL);
		while ((tup = systable_getnext(scan)) != NULL)
		{
			Form_pg_namespace ns = (Form_pg_namespace) GETSTRUCT(tup);

			if (canon_namespace(ns->oid))
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

			if (!canon_namespace(typ->typnamespace) ||
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

			if (!canon_namespace(proc->pronamespace) ||
				in_extension(ProcedureRelationId, proc->oid) ||
				((get_typtype(proc->prorettype) == TYPTYPE_RANGE ||
				  get_typtype(proc->prorettype) == TYPTYPE_MULTIRANGE) &&
				 part_of_type(ProcedureRelationId, proc->oid)))
				continue;
			name = psprintf("%s(%s)",
							qualified(proc->pronamespace, NameStr(proc->proname)),
							TextDatumGetCString(DirectFunctionCall1(pg_get_function_identity_arguments,
																	ObjectIdGetDatum(proc->oid))));
			/* (a window function is written in C: none to hold) */
			if (proc->prokind == PROKIND_WINDOW)
			{
				ObjectAddress self;

				ObjectAddressSet(self, ProcedureRelationId, proc->oid);
				(void) uncovered(cn, "window functions", name, &self);
				continue;
			}
			it = add_item(cn, "f:", name);
			if (proc->prokind == PROKIND_AGGREGATE)
				append_utf8(&it->value, aggregate_def(proc));
			else
				append_utf8(&it->value,
							TextDatumGetCString(DirectFunctionCall1(pg_get_functiondef,
																	ObjectIdGetDatum(proc->oid))));
		}
		systable_endscan(scan);
		table_close(pg_proc, AccessShareLock);
	}
	refuse_undescribed(cn);
}

/*
 * Is a catalog's kind of object one a commit holds (or one never held:
 * privileges)?  Every other kind a branch-aware catalog holds in a
 * versioned schema is refused, not left out of a commit's hash (a clone or
 * checkout wouldn't have it): a kind added later included.
 */
static bool
held_kind(Oid classid)
{
	switch (classid)
	{
		case NamespaceRelationId:
		case RelationRelationId:
		case TypeRelationId:
		case ProcedureRelationId:
		case ConstraintRelationId:	/* (a table's or a domain's) */
		case CollationRelationId:
		case StatisticExtRelationId:	/* (a table's) */
		case DefaultAclRelationId:	/* (privileges aren't held) */
			return true;
		default:
			return false;
	}
}

/*
 * Objects of versioned schemas the canonical form doesn't describe, each
 * named as Postgres describes it (which tells an operator's argument
 * types, an operator class's access method): refused.  Branch-aware
 * catalogs with a schema column are the ones that can hold them.  (Casts,
 * which have no schema, are described.)
 */
static void
refuse_undescribed(Canon *cn)
{
	int			ncatalogs;
	const CatalogBranchInfo *catalogs = catalog_branch_catalogs(&ncatalogs);
	List	   *versioned = NIL;	/* schemas seen, and whether */
	List	   *plain = NIL;

	for (int k = 0; k < ncatalogs; k++)
	{
		Oid			classid = catalogs[k].relid;
		AttrNumber	nspatt;
		AttrNumber	oidatt;
		Relation	rel;
		SysScanDesc scan;
		HeapTuple	tup;

		if (held_kind(classid) || !is_objectclass_supported(classid) ||
			(nspatt = get_object_attnum_namespace(classid)) == InvalidAttrNumber)
			continue;
		oidatt = get_object_attnum_oid(classid);
		rel = table_open(classid, AccessShareLock);
		scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
		while ((tup = systable_getnext(scan)) != NULL)
		{
			bool		isnull;
			Oid			oid = DatumGetObjectId(heap_getattr(tup, oidatt,
															RelationGetDescr(rel), &isnull));
			Oid			nsp;
			ObjectAddress self;

			/* (what initdb made is in no versioned schema) */
			if (oid < FirstNormalObjectId)
				continue;
			nsp = DatumGetObjectId(heap_getattr(tup, nspatt, RelationGetDescr(rel), &isnull));
			if (isnull || list_member_oid(plain, nsp))
				continue;
			if (!list_member_oid(versioned, nsp))
			{
				if (!canon_namespace(nsp))
				{
					plain = lappend_oid(plain, nsp);
					continue;
				}
				versioned = lappend_oid(versioned, nsp);
			}
			if (in_extension(classid, oid))
				continue;
			ObjectAddressSet(self, classid, oid);
			(void) uncovered(cn, psprintf("%s objects", get_object_class_descr(classid)),
							 getObjectDescription(&self, false), &self);
		}
		systable_endscan(scan);
		table_close(rel, AccessShareLock);
	}

	describe_casts(cn);
	describe_collations(cn);
}

/*
 * Collations of versioned schemas: "l:" items, each its provider, its
 * locale (a libc one's LC_COLLATE and LC_CTYPE), its ICU rules (empty if
 * none) and whether it is deterministic, a line each: what CREATE
 * COLLATION takes.  (A "c:" item records the version of each collation
 * that orders keys, these among them.)
 */
static void
describe_collations(Canon *cn)
{
	Relation	rel = table_open(CollationRelationId, AccessShareLock);
	SysScanDesc scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
	HeapTuple	tup;

	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_collation coll = (Form_pg_collation) GETSTRUCT(tup);
		CanonItem  *it;
		bool		isnull;
		Datum		d;

		if (coll->oid < FirstNormalObjectId || !canon_namespace(coll->collnamespace) ||
			in_extension(CollationRelationId, coll->oid))
			continue;
		it = add_item(cn, "l:", qualified(coll->collnamespace, NameStr(coll->collname)));
		appendStringInfo(&it->value, "%c\n", coll->collprovider);
		if (coll->collprovider == COLLPROVIDER_LIBC)
		{
			d = SysCacheGetAttrNotNull(COLLOID, tup, Anum_pg_collation_collcollate);
			append_field(&it->value, TextDatumGetCString(d));
			appendStringInfoChar(&it->value, '\t');
			d = SysCacheGetAttrNotNull(COLLOID, tup, Anum_pg_collation_collctype);
			append_field(&it->value, TextDatumGetCString(d));
		}
		else
		{
			d = SysCacheGetAttrNotNull(COLLOID, tup, Anum_pg_collation_colllocale);
			append_field(&it->value, TextDatumGetCString(d));
		}
		appendStringInfoChar(&it->value, '\n');
		d = SysCacheGetAttr(COLLOID, tup, Anum_pg_collation_collicurules, &isnull);
		if (!isnull)
			append_field(&it->value, TextDatumGetCString(d));
		appendStringInfoChar(&it->value, '\n');
		appendStringInfo(&it->value, "%s\n", coll->collisdeterministic ? "t" : "f");
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
}

/*
 * Casts of a type of a versioned schema, but those part of one (a range
 * type's to its multirange type): "x:" items named "source AS target",
 * each its method ("function" and the function's qualified signature,
 * "inout" or "binary") and its context ("explicit", "assignment" or
 * "implicit"), a line each.  (What initdb made casts no such type.)
 */
static void
describe_casts(Canon *cn)
{
	Relation	rel = table_open(CastRelationId, AccessShareLock);
	SysScanDesc scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
	HeapTuple	tup;

	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_cast c = (Form_pg_cast) GETSTRUCT(tup);
		ObjectAddress src;
		ObjectAddress tgt;
		CanonItem  *it;

		if (c->oid < FirstNormalObjectId)
			continue;
		ObjectAddressSet(src, TypeRelationId, c->castsource);
		ObjectAddressSet(tgt, TypeRelationId, c->casttarget);
		if (!canon_namespace(get_object_namespace(&src)) &&
			!canon_namespace(get_object_namespace(&tgt)))
			continue;
		if (part_of_type(CastRelationId, c->oid) ||
			in_extension(CastRelationId, c->oid))
			continue;
		it = add_item(cn, "x:",
					  psprintf("%s AS %s",
							   format_type_extended(c->castsource, -1, FORMAT_TYPE_FORCE_QUALIFY),
							   format_type_extended(c->casttarget, -1, FORMAT_TYPE_FORCE_QUALIFY)));
		if (c->castmethod == COERCION_METHOD_FUNCTION)
		{
			append_field(&it->value, "function");
			appendStringInfoChar(&it->value, '\t');
			append_field(&it->value, format_procedure_qualified(c->castfunc));
		}
		else
			append_field(&it->value, c->castmethod == COERCION_METHOD_INOUT ?
						 "inout" : "binary");
		appendStringInfoChar(&it->value, '\n');
		append_field(&it->value, c->castcontext == COERCION_CODE_IMPLICIT ? "implicit" :
					 c->castcontext == COERCION_CODE_ASSIGNMENT ? "assignment" :
					 "explicit");
		appendStringInfoChar(&it->value, '\n');
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
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
	append_field(value, locale);
	appendStringInfoChar(value, '\n');
	if (rules)
		append_field(value, rules);
	appendStringInfo(value, "\n%c\n", deterministic ? 't' : 'f');
	append_field(value, version);
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
					f[0], quote_literal_cstr(unescape_field(f[1])),
					f[2][0] ? ", rules " : "",
					f[2][0] ? quote_literal_cstr(unescape_field(f[2])) : "",
					f[3][0] == 't' ? "" : "not ", quote_literal_cstr(unescape_field(f[4])));
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
	add_collations(cn);
	check_refused(cn);
	appendStringInfoString(&add_item(cn, "!:", "format")->value, FOLDED_FORMAT);

	list_sort(cn->items, item_cmp);
	b = PROLLY_CALL(cn->tree, build_begin, &cn->root_env, CurrentMemoryContext);
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
	ProllyTreeEnv benv = cn->env;
	ListCell   *lc;

	list_sort(cn->next, item_cmp);
	benv.chunk_refs = base_chunk_refs;
	b = PROLLY_CALL(cn->tree, build_begin, &benv, CurrentMemoryContext);
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
	cn.reading = reading;
	cn.only = only;
	canon_env(&cn);
	cn.finder = prolly_change_finder();
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
			ProllyNodeRef *next_baseline, bool commit)
{
	(void) fold_canon(baseline, root, next_baseline, !commit, InvalidOid);
}

/*
 * Drop the objects of the session branch's versioned schemas that the
 * canonical form doesn't describe, and what depends on them: work no
 * commit holds, which a move to a commit discards.  (Objects only: no
 * table's rows are folded.)
 */
static void
folded_discard_undescribed(void)
{
	Canon		cn;
	ObjectAddresses *addrs;
	ListCell   *lc;

	memset(&cn, 0, sizeof(cn));
	cn.reading = true;
	cn.no_rows = true;
	canon_env(&cn);
	CommandCounterIncrement();
	PushActiveSnapshot(GetTransactionSnapshot());
	PG_TRY();
	{
		collect(&cn);
	}
	PG_FINALLY();
	{
		PopActiveSnapshot();
	}
	PG_END_TRY();
	if (cn.left_addrs == NIL)
		return;
	addrs = new_object_addresses();
	foreach(lc, cn.left_addrs)
		add_exact_object_address(lfirst(lc), addrs);
	performMultipleDeletions(addrs, DROP_CASCADE, 0);
	free_object_addresses(addrs);
	CommandCounterIncrement();
}

static void
folded_read_fold(const ProllyNodeRef *baseline, Oid only, ProllyNodeRef *root,
				 List **left_out)
{
	List	   *l = fold_canon(baseline, root, NULL, true, only);

	if (left_out)
		*left_out = list_concat(*left_out, l);
}

/*
 * A baseline's tree, its tables' data trees and the finders' states.  Each
 * state is walked by every change finder, not the one configured now: the
 * one that wrote it may be another (a fold under another setting, or before
 * the default changed), and each passes over states it didn't write.
 */
static void
folded_walk_baseline(const ProllyNodeRef *baseline, ProllyTreeVisitNode visit,
					 void *arg)
{
	Canon		cn;
	List	   *finders;

	memset(&cn, 0, sizeof(cn));
	canon_env(&cn);
	/* (a baseline the visitor declines: it has seen the trees it names) */
	if (!PROLLY_CALL(cn.tree, walk, &cn.env, baseline, visit, arg))
		return;
	finders = prolly_list_impls(PROLLY_IFACE_CHANGE_FINDER);
	load_baseline(&cn, baseline);
	for (int i = 0; i < cn.nbase; i++)
	{
		ProllyValue state = {(const uint8 *) cn.base[i].state.data,
		cn.base[i].state.len};

		PROLLY_CALL(cn.tree, walk, &cn.env, &cn.base[i].data, visit, arg);
		foreach_ptr(const ProllyChangeFinderRoutine, finder, finders)
		{
			if (finder->walk != NULL)
				finder->walk(&cn.env, &state, visit, arg);
		}
	}
	list_free(finders);
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
	canon_env(&cn);
	/* (a root the visitor declines: it has seen the trees the root names) */
	if (!PROLLY_CALL(cn.tree, walk, &cn.env, root, visit, arg))
		return;
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

/* A data tree a root names, by its item's key. */
typedef struct NamedTree
{
	char	   *key;
	ProllyNodeRef ref;
} NamedTree;

/* The data trees a root's items name. */
static List *
named_trees(Canon *cn, const ProllyNodeRef *root)
{
	ProllyTreeCursor *c;
	ProllyKey	k;
	ProllyValue v;
	List	   *trees = NIL;

	c = PROLLY_CALL(cn->tree, cursor_open, &cn->env, root, CurrentMemoryContext);
	PROLLY_CALL(cn->tree, cursor_seek, c, NULL, ForwardScanDirection);
	while (PROLLY_CALL(cn->tree, cursor_next, c, &k, &v))
	{
		NamedTree  *t = palloc0(sizeof(NamedTree));
		ProllyValue def;

		if (!tree_item(&k, &v, &t->ref, &def))
		{
			pfree(t);
			continue;
		}
		t->key = pnstrdup((const char *) k.data, k.len);
		trees = lappend(trees, t);
	}
	PROLLY_CALL(cn->tree, cursor_close, c);
	return trees;
}

/*
 * Visit the nodes of root to (its trees and the data trees it names) that
 * root from (NULL: none) may lack: each tree diffed against from's of the
 * same name, so what changed between them is visited, not all of to.
 */
/* A data tree an item changed between two roots names: before and after. */
typedef struct ChangedTree
{
	bool		was;
	ProllyNodeRef before;
	ProllyNodeRef after;
} ChangedTree;

static void
note_changed_tree(ProllyDiffKind kind, const ProllyKey *key,
				  const ProllyValue *before, const ProllyValue *after, void *arg)
{
	List	  **changed = arg;
	ChangedTree *c;
	ProllyValue def;

	if (after == NULL)
		return;
	c = palloc0(sizeof(ChangedTree));
	if (!tree_item(key, after, &c->after, &def))
	{
		pfree(c);
		return;
	}
	c->was = before != NULL && tree_item(key, before, &c->before, &def);
	*changed = lappend(*changed, c);
}

/*
 * Visit the nodes of root to (its trees and the data trees it names) that
 * root from (NULL: none) may lack: the roots diffed, and each data tree an
 * item added or changed names diffed against the one it named before, so
 * what changed is visited, not all of to.
 */
static void
folded_walk_changes(const ProllyNodeRef *from, const ProllyNodeRef *to,
					ProllyTreeVisitNode visit, void *arg)
{
	Canon		cn;
	List	   *changed = NIL;
	ListCell   *lc;

	memset(&cn, 0, sizeof(cn));
	canon_env(&cn);
	PROLLY_CALL(cn.tree, diff_nodes, &cn.env, from, to, visit, arg);
	if (from && from->hash.len > 0)
		PROLLY_CALL(cn.tree, diff, &cn.env, from, to, note_changed_tree, &changed);
	else
	{
		foreach(lc, named_trees(&cn, to))
		{
			ChangedTree *c = palloc0(sizeof(ChangedTree));

			c->after = ((NamedTree *) lfirst(lc))->ref;
			changed = lappend(changed, c);
		}
	}
	foreach(lc, changed)
	{
		ChangedTree *c = lfirst(lc);

		PROLLY_CALL(cn.tree, diff_nodes, &cn.env, c->was ? &c->before : NULL,
					&c->after, visit, arg);
	}
}

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

/*
 * Refuse a root of another format: its definitions' text isn't this
 * format's, so it is neither moved to nor merged (how: "moved to").
 */
static void
check_format(Canon *cn, const ProllyNodeRef *root, const char *how)
{
	if (!root_in_format(cn, root))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a commit made in an older format of prolly's history can't be %s", how),
				 errhint("Commit the branch to write it in the current one.")));
}

static bool
folded_in_format(const ProllyNodeRef *root)
{
	Canon		cn;

	memset(&cn, 0, sizeof(cn));
	canon_env(&cn);
	return root_in_format(&cn, root);
}

static void
folded_format(StringInfo out)
{
	appendStringInfoString(out, FOLDED_FORMAT);
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
			  !current ? PROLLY_UNREADABLE_OLDER_FORMAT :
			  table_unreadable(pg_any_to_server(name, strlen(name), PG_UTF8), stale),
			  arg);
		pfree(name);
	}
	PROLLY_CALL(cn.tree, cursor_close, c);
}

static char *from_utf8(const uint8 *data, int len);

/*
 * A literal's value, as quote_literal_cstr() wrote it: quotes doubled, and
 * backslashes too if it begins E.
 */
static char *
literal_value(const char *literal)
{
	bool		e = literal[0] == 'E';
	const char *p = literal + (e ? 1 : 0);
	StringInfoData out;

	if (*p++ != '\'')
		elog(ERROR, "invalid canonical literal %s", literal);
	initStringInfo(&out);
	for (; *p; p++)
	{
		if (*p == '\'' && p[1] == '\'')
			p++;
		else if (*p == '\'')
			break;
		else if (e && *p == '\\' && p[1] == '\\')
			p++;
		appendStringInfoChar(&out, *p);
	}
	if (*p != '\'' || p[1] != '\0')
		elog(ERROR, "invalid canonical literal %s", literal);
	return out.data;
}

/* Saved type definitions accompany historical rows; labels are not OIDs. */
static List *
folded_type_definitions(const ProllyNodeRef *root)
{
	Canon		cn;
	ProllyTreeCursor *cur;
	ProllyKey	from = {(const uint8 *) "y:", 2};
	ProllyKey	k;
	ProllyValue v;
	List	   *types = NIL;

	memset(&cn, 0, sizeof(cn));
	canon_env(&cn);
	if (!root_in_format(&cn, root))
		return NIL;
	cur = PROLLY_CALL(cn.tree, cursor_open, &cn.env, root, CurrentMemoryContext);
	PROLLY_CALL(cn.tree, cursor_seek, cur, &from, ForwardScanDirection);
	while (PROLLY_CALL(cn.tree, cursor_next, cur, &k, &v))
	{
		ProllyTypeDefinition *e;
		char	   *line;

		if (k.len < 2 || memcmp(k.data, "y:", 2) != 0)
			break;
		e = palloc0(sizeof(ProllyTypeDefinition));
		e->name = from_utf8(k.data + 2, k.len - 2);
		e->definition = from_utf8(v.data, v.len);
		types = lappend(types, e);
		if (v.len < 5 || memcmp(v.data, "enum\n", 5) != 0)
			continue;
		line = e->definition + 5;
		while (*line)
		{
			char	   *next = strchr(line, '\n');
			char	   *field;

			if (next == NULL)
				elog(ERROR, "invalid canonical enum definition");
			field = unescape_field(pnstrdup(line, next - line));
			e->labels = lappend(e->labels, makeString(literal_value(field)));
			line = next + 1;
		}
	}
	PROLLY_CALL(cn.tree, cursor_close, cur);
	return types;
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

/*
 * Visits a pair of items in a merge of two folded roots' items, in key
 * order: one side's item is NULL where only the other root holds it.
 * Returns false to end the walk.
 */
typedef bool (*ItemPairVisit) (const ProllyKey *ka, const ProllyValue *va,
							   const ProllyKey *kb, const ProllyValue *vb,
							   void *arg);

/*
 * The next item that defines an object: with others_only, not a table
 * (seeking past the tables' items, which tables() is for) nor the root's
 * format.
 */
static bool
next_item(Canon *cn, ProllyTreeCursor *c, bool others_only, ProllyKey *k,
		  ProllyValue *v)
{
	static const ProllyKey after_tables = {(const uint8 *) "u", 1};

	while (next_object(cn, c, k, v))
	{
		if (!others_only || k->len < 2 || k->data[1] != ':')
			return true;
		if (k->data[0] == 't')
			PROLLY_CALL(cn->tree, cursor_seek, c, &after_tables,
						ForwardScanDirection);
		else if (k->data[0] != '!')
			return true;
	}
	return false;
}

/* Merge two folded roots' items in key order, visiting each pair. */
static void
merge_items(Canon *cn, const ProllyNodeRef *a, const ProllyNodeRef *b,
			bool others_only, ItemPairVisit visit, void *arg)
{
	ProllyTreeCursor *ca;
	ProllyTreeCursor *cb;
	ProllyKey	ka,
				kb;
	ProllyValue va,
				vb;
	bool		more_a,
				more_b;

	ca = PROLLY_CALL(cn->tree, cursor_open, &cn->env, a, CurrentMemoryContext);
	cb = PROLLY_CALL(cn->tree, cursor_open, &cn->env, b, CurrentMemoryContext);
	PROLLY_CALL(cn->tree, cursor_seek, ca, NULL, ForwardScanDirection);
	PROLLY_CALL(cn->tree, cursor_seek, cb, NULL, ForwardScanDirection);
	more_a = next_item(cn, ca, others_only, &ka, &va);
	more_b = next_item(cn, cb, others_only, &kb, &vb);
	while (more_a || more_b)
	{
		int			c = !more_a ? 1 : !more_b ? -1 :
			prolly_bytes_compare(&ka, &kb, NULL);

		if (!visit(c <= 0 ? &ka : NULL, c <= 0 ? &va : NULL,
				   c >= 0 ? &kb : NULL, c >= 0 ? &vb : NULL, arg))
			break;
		if (c <= 0)
			more_a = next_item(cn, ca, others_only, &ka, &va);
		if (c >= 0)
			more_b = next_item(cn, cb, others_only, &kb, &vb);
	}
	PROLLY_CALL(cn->tree, cursor_close, ca);
	PROLLY_CALL(cn->tree, cursor_close, cb);
}

static bool
same_values(const ProllyValue *a, const ProllyValue *b)
{
	return a->len == b->len && memcmp(a->data, b->data, a->len) == 0;
}

static bool
same_definition_pair(const ProllyKey *ka, const ProllyValue *va,
					 const ProllyKey *kb, const ProllyValue *vb, void *arg)
{
	bool	   *same = arg;
	ProllyValue da,
				db;

	if (ka == NULL || kb == NULL)
		*same = false;
	else
	{
		definition_of(ka, va, &da);
		definition_of(kb, vb, &db);
		*same = same_values(&da, &db);
	}
	return *same;
}

/* The same objects, the same values but for tables' and views' rows. */
static bool
folded_same_schema(const ProllyNodeRef *a, const ProllyNodeRef *b)
{
	Canon		cn;
	bool		same = true;

	memset(&cn, 0, sizeof(cn));
	canon_env(&cn);
	merge_items(&cn, a, b, false, same_definition_pair, &same);
	return same;
}

typedef struct ObjectsWalk
{
	ProllyCanonObjectVisit visit;
	void	   *arg;
	bool		same_format;	/* both roots' data trees in one format */
} ObjectsWalk;

/*
 * A sequence's value: the line after its definition, if the fold could
 * read it (and the root's format records it).
 */
static bool
sequence_value(const ProllyKey *k, const ProllyValue *v, ProllyValue *value)
{
	ProllyValue def;

	definition_of(k, v, &def);
	value->data = v->data + def.len;
	value->len = v->len - def.len;
	return value->len > 0;
}

/*
 * Whether an object differs: by its definition; a sequence by its value
 * too, where both roots have it (an older root's, or one the fold couldn't
 * read, isn't a change); a materialized view by its rows too, where both
 * roots' data trees are in one format.
 */
static bool
object_differs(const ObjectsWalk *w, const ProllyKey *ka, const ProllyValue *va,
			   const ProllyKey *kb, const ProllyValue *vb)
{
	ProllyValue da,
				db;

	definition_of(ka, va, &da);
	definition_of(kb, vb, &db);
	if (!same_values(&da, &db))
		return true;
	if (ka->data[0] == 's')
	{
		ProllyValue sa,
					sb;

		return sequence_value(ka, va, &sa) && sequence_value(kb, vb, &sb) &&
			!same_values(&sa, &sb);
	}
	if (ka->data[0] == 'm' && w->same_format)
		return !same_values(va, vb);
	return false;
}

static bool
object_pair(const ProllyKey *ka, const ProllyValue *va,
			const ProllyKey *kb, const ProllyValue *vb, void *arg)
{
	ObjectsWalk *w = arg;
	const ProllyKey *k = ka ? ka : kb;
	ProllyDiffKind kind;
	char	   *name;

	if (ka == NULL)
		kind = PROLLY_DIFF_ADDED;
	else if (kb == NULL)
		kind = PROLLY_DIFF_REMOVED;
	else if (object_differs(w, ka, va, kb, vb))
		kind = PROLLY_DIFF_MODIFIED;
	else
		return true;
	name = from_utf8(k->data + 2, k->len - 2);
	w->visit(name, kind, (char) k->data[0], w->arg);
	pfree(name);
	return true;
}

/* The objects other than tables that differ: both roots' items merged. */
static void
folded_objects(const ProllyNodeRef *from, const ProllyNodeRef *to,
			   ProllyCanonObjectVisit visit, void *arg)
{
	Canon		cn;
	ObjectsWalk w = {visit, arg};

	memset(&cn, 0, sizeof(cn));
	canon_env(&cn);
	w.same_format = root_in_format(&cn, from) == root_in_format(&cn, to);
	merge_items(&cn, from, to, true, object_pair, &w);
}

/* An object's change, as the roots' tree diff gives it, copied. */
typedef struct Changed
{
	char		object;
	char	   *from_name;		/* NULL: not in from */
	char	   *to_name;		/* NULL: not in to */
	ProllyValue from;			/* the items (copied) */
	ProllyValue to;
	bool		data_change;
	bool		schema_change;
	uint64		content;		/* an added or dropped one's: its item's hash,
								 * its own name in it masked */
	ProllyValue masked;			/* and that item */
} Changed;

typedef struct ChangesWalk
{
	List	   *changed;		/* Changed * */
	bool		same_format;
} ChangesWalk;

static ProllyValue
copy_value(const ProllyValue *v)
{
	ProllyValue c;
	uint8	   *data = palloc(Max(v->len, 1));

	memcpy(data, v->data, v->len);
	c.data = data;
	c.len = v->len;
	return c;
}

/*
 * An item with its object's own name masked: a table's description names
 * the table (its indexes are made ON it), so a renamed one's items differ
 * by that alone.  (A longer name holding it is masked in part, which can
 * only keep two items apart, never make them alike.)
 */
static ProllyValue
masked_item(const ProllyValue *v, const char *name)
{
	StringInfoData out;
	char	   *utf8 = pg_server_to_any(name, strlen(name), PG_UTF8);
	size_t		n = strlen(utf8);
	const char *p = (const char *) v->data;
	const char *end = p + v->len;
	ProllyValue m;

	initStringInfo(&out);
	while (p < end)
	{
		if (n > 0 && (size_t) (end - p) >= n && memcmp(p, utf8, n) == 0)
		{
			appendStringInfoChar(&out, '\x01');
			p += n;
		}
		else
			appendStringInfoChar(&out, *p++);
	}
	m.data = (const uint8 *) out.data;
	m.len = out.len;
	return m;
}

/*
 * Whether an object holds data: a table or materialized view any rows (its
 * data tree isn't empty), a sequence its value.  An empty table added or
 * dropped changes no data, as in Dolt.
 */
static bool
holds_data(const ProllyKey *key, const ProllyValue *item)
{
	Canon		cn;
	ProllyNodeRef root;
	ProllyValue def;
	ProllyTreeCursor *c;
	ProllyKey	k;
	ProllyValue v;
	bool		any;

	if (key->data[0] == 's')
		return true;
	if (!tree_item(key, item, &root, &def))
		return false;
	memset(&cn, 0, sizeof(cn));
	canon_env(&cn);
	c = PROLLY_CALL(cn.tree, cursor_open, &cn.env, &root, CurrentMemoryContext);
	PROLLY_CALL(cn.tree, cursor_seek, c, NULL, ForwardScanDirection);
	any = PROLLY_CALL(cn.tree, cursor_next, c, &k, &v);
	PROLLY_CALL(cn.tree, cursor_close, c);
	return any;
}

static void
note_change(ProllyDiffKind kind, const ProllyKey *key, const ProllyValue *before,
			const ProllyValue *after, void *arg)
{
	ChangesWalk *w = arg;
	Changed    *c;
	char	   *name;

	/* (the format, and collations, aren't objects) */
	if (key->len < 2 || key->data[1] != ':' || key->data[0] == '!' ||
		key->data[0] == 'c')
		return;
	c = palloc0(sizeof(Changed));
	c->object = (char) key->data[0];
	name = from_utf8(key->data + 2, key->len - 2);
	if (before)
	{
		c->from_name = name;
		c->from = copy_value(before);
	}
	if (after)
	{
		c->to_name = name;
		c->to = copy_value(after);
	}
	if (before && after)
	{
		ProllyValue da,
					db;
		ProllyNodeRef ra,
					rb;

		definition_of(key, before, &da);
		definition_of(key, after, &db);
		c->schema_change = !same_values(&da, &db);
		if (c->object == 't' ||
			(c->object == 'm' && w->same_format))
		{
			(void) tree_item(key, before, &ra, &da);
			(void) tree_item(key, after, &rb, &db);
			c->data_change = !(ra.hash.len == rb.hash.len &&
							   memcmp(ra.hash.bytes, rb.hash.bytes, ra.hash.len) == 0);
		}
		else if (c->object == 's')
		{
			ProllyValue sa,
						sb;

			c->data_change = sequence_value(key, before, &sa) &&
				sequence_value(key, after, &sb) && !same_values(&sa, &sb);
		}
		if (!c->data_change && !c->schema_change)
		{
			pfree(c);
			return;
		}
	}
	else
	{
		const ProllyValue *v = before ? before : after;

		c->data_change = holds_data(key, v);
		c->schema_change = true;
		c->masked = masked_item(v, name);
		c->content = hash_bytes_extended(c->masked.data, c->masked.len,
										 (uint64) c->object);
	}
	w->changed = lappend(w->changed, c);
}

static int
changed_cmp(const ListCell *a, const ListCell *b)
{
	const Changed *x = lfirst(a);
	const Changed *y = lfirst(b);

	return strcmp(x->from_name ? x->from_name : x->to_name,
				  y->from_name ? y->from_name : y->to_name);
}

/* the dropped and added objects of an item's content, and how many */
typedef struct ContentMatch
{
	uint64		content;		/* key */
	int			ndropped;
	int			nadded;
	Changed    *dropped;
	Changed    *added;
} ContentMatch;

/*
 * Each object from and to hold differently, by name: what the roots' tree
 * diff gives (equal subtrees skipped), objects of a kind dropped and added
 * with the same item (but for their own names), and the only such pair,
 * made one renamed.
 */
static void folded_render(char object, const char *name, const ProllyValue *item,
						  StringInfo out);

static void
folded_changes(const ProllyNodeRef *from, const ProllyNodeRef *to,
			   ProllyCanonChangeVisit visit, void *arg)
{
	Canon		cn;
	ChangesWalk w = {NIL, false};
	HTAB	   *matches;
	HASHCTL		ctl;
	HASH_SEQ_STATUS status;
	ContentMatch *m;
	ListCell   *lc;

	memset(&cn, 0, sizeof(cn));
	canon_env(&cn);
	w.same_format = root_in_format(&cn, from) == root_in_format(&cn, to);
	PROLLY_CALL(cn.tree, diff, &cn.env, from, to, note_change, &w);

	/* renames: a dropped and an added object of one content, alone */
	ctl.keysize = sizeof(uint64);
	ctl.entrysize = sizeof(ContentMatch);
	ctl.hcxt = CurrentMemoryContext;
	matches = hash_create("prolly renames", 64, &ctl,
						  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	foreach(lc, w.changed)
	{
		Changed    *c = lfirst(lc);
		bool		found;

		if (c->from_name && c->to_name)
			continue;
		m = hash_search(matches, &c->content, HASH_ENTER, &found);
		if (!found)
		{
			m->ndropped = m->nadded = 0;
			m->dropped = m->added = NULL;
		}
		if (c->from_name)
		{
			m->ndropped++;
			m->dropped = c;
		}
		else
		{
			m->nadded++;
			m->added = c;
		}
	}
	hash_seq_init(&status, matches);
	while ((m = hash_seq_search(&status)) != NULL)
	{
		if (m->ndropped != 1 || m->nadded != 1 ||
			m->dropped->object != m->added->object ||
			!same_values(&m->dropped->masked, &m->added->masked))
			continue;
		/*
		 * (one row: the dropped one, its new name, its rows as they were; a
		 * new name is a change of definition, as in Dolt)
		 */
		m->dropped->to_name = m->added->to_name;
		m->dropped->to = m->added->to;
		m->dropped->data_change = false;
		m->dropped->schema_change = true;
		w.changed = list_delete_ptr(w.changed, m->added);
	}
	hash_destroy(matches);

	list_sort(w.changed, changed_cmp);
	foreach(lc, w.changed)
	{
		Changed    *c = lfirst(lc);
		ProllyObjectChange oc = {0};

		oc.from_name = c->from_name;
		oc.to_name = c->to_name;
		oc.object = c->object;
		oc.data_change = c->data_change;
		oc.schema_change = c->schema_change;
		oc.from_item = c->from_name ? &c->from : NULL;
		oc.to_item = c->to_name ? &c->to : NULL;
		visit(&oc, arg);
	}
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
	List	   *triggers;		/* statements (firing_statement()) */
	List	   *rules;			/* statements (likewise) */
	List	   *policies;		/* statements */
	List	   *statistics;		/* lines: name, a tab, the statement[, a tab,
								 * the target] */
	bool		row_security;	/* (and forced) */
	bool		forced_row_security;
	char	   *partition_by;	/* a partitioned table's key, or NULL */
	char	   *partition_of;	/* a partition's parent and bound (escaped), or NULL */
	List	   *inherits;		/* the tables it inherits from, in order */
	char	   *of;				/* a typed table's type, or NULL */
	List	   *options;		/* lines: option name, a tab, SQL literal */
	List	   *replica_identity;	/* one statement suffix, or NIL for
									 * DEFAULT */
	List	   *local;			/* local_line()s: what it inherits that is
								 * its own too */
} TableDef;

typedef struct SchemaMove
{
	List	   *before;			/* statements before the rows are written */
	List	   *changes;		/* before too, once the objects they may use
								 * are made: tables made, columns added and
								 * changed, keys added */
	List	   *after;			/* and after */
	List	   *added;			/* the checks, NOT NULLs and foreign keys
								 * after and foreign_keys add unvalidated
								 * (ProllyAddedConstraint *) */
	List	   *validations;	/* validating the checks and NOT NULLs */
	List	   *foreign_keys;	/* after, last */
	List	   *fk_validations; /* validating the foreign keys */
	List	   *rekeyed;		/* tables whose keys change in place */
	List	   *emptied;		/* tables whose rows go first */
	List	   *fk_drops;		/* foreign keys going: first of all, before
								 * the columns of types the commit defines
								 * otherwise convert (which would check them
								 * against a table emptied) */
	bool		rewritten;		/* the rows written are all to's: those
								 * the tables have needn't convert */
	bool		render;			/* statements to show, not to run: each
								 * constraint whole, nothing read from the
								 * catalogs */
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
		else if (strcmp(line, "policies") == 0)
			section = &td->policies;
		else if (strcmp(line, "statistics") == 0)
			section = &td->statistics;
		else if (strcmp(line, "inherits") == 0)
			section = &td->inherits;
		else if (strcmp(line, "local") == 0)
			section = &td->local;
		else if (strcmp(line, "options") == 0)
			section = &td->options;
		else if (strcmp(line, "replica identity") == 0)
			section = &td->replica_identity;
		else if (strcmp(line, "of") == 0)
		{
			section = NULL;
			if (next == NULL || *next == '\0')
				elog(ERROR, "unexpected table definition: \"%s\" ends it", line);
			line = next;
			next = strchr(line, '\n');
			if (next)
				*next++ = '\0';
			td->of = unescape_field(line);
		}
		else if (section == &td->policies && strcmp(line, ROW_SECURITY) == 0)
			td->row_security = true;
		else if (section == &td->policies && strcmp(line, FORCED_ROW_SECURITY) == 0)
			td->forced_row_security = true;
		else if (strcmp(line, "partition by") == 0 ||
				 strcmp(line, "partition of") == 0)
		{
			char	  **field = line[10] == 'b' ? &td->partition_by : &td->partition_of;

			section = NULL;
			if (next == NULL || *next == '\0')
				elog(ERROR, "unexpected table definition: \"%s\" ends it", line);
			line = next;
			next = strchr(line, '\n');
			if (next)
				*next++ = '\0';
			*field = field == &td->partition_by ? unescape_field(line) : pstrdup(line);
		}
		else if (section == &td->columns || section == &td->constraints ||
				 section == &td->statistics || section == &td->local ||
				 section == &td->options)
			*section = lappend(*section, pstrdup(line));	/* (fields escaped) */
		else if (section)
			*section = lappend(*section, unescape_field(line));
	}
}

static char *first_field(const char *line);
static char *unquoted(const char *ident);

/*
 * A table's description read back: its columns (each line's first field,
 * its name, and second, its type, and a generated column's expression),
 * and whether a primary key, not deferrable, is among its constraints.
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
		const char *generated = strstr(line, "\tgenerated ");
		ProllyTableColumn *c = palloc0(sizeof(ProllyTableColumn));

		c->name = unquoted(first_field(line));
		c->type = type ? first_field(type + 1) : pstrdup("");
		if (generated)
			c->generated = first_field(generated + 11);
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
	char	   *field = pnstrdup(line, strcspn(line, "\t"));
	char	   *name = unescape_field(field);

	pfree(field);
	return name;
}

/* A line's fields after its first, as one (a constraint's definition, an attribute's type). */
static char *
constraint_def(const char *line)
{
	const char *tab = strchr(line, '\t');

	if (tab == NULL)
		elog(ERROR, "unexpected definition line \"%s\"", line);
	return unescape_field(tab + 1);
}

/*
 * Is column col (quoted as written) one of table t's primary key's (its
 * PRIMARY KEY line's column list)?
 */
static bool
in_primary_key(const TableDef *t, const char *col)
{
	ListCell   *lc;

	foreach(lc, t->constraints)
	{
		const char *line = lfirst(lc);
		char	   *def;
		char	   *open;
		char	   *close;

		if (strstr(line, "\tPRIMARY KEY ") == NULL)
			continue;
		def = constraint_def(line);
		open = strchr(def, '(');
		close = open ? strchr(open, ')') : NULL;
		if (open == NULL || close == NULL)
			return true;		/* (unexpected: kept as a key's) */
		*close = '\0';
		for (char *c = strtok(open + 1, ","); c != NULL; c = strtok(NULL, ","))
		{
			while (*c == ' ')
				c++;
			if (strcmp(c, col) == 0)
				return true;
		}
	}
	return false;
}

/*
 * Where the identifier p begins with ends, as written: quoted (spaces,
 * doubled quotes and all) or up to a space.  value, if given, gets it
 * unquoted.
 */
static const char *
scan_identifier(const char *p, StringInfo value)
{
	if (*p != '"')
	{
		size_t		n = strcspn(p, " ");

		if (value)
			appendBinaryStringInfo(value, p, n);
		return p + n;
	}
	for (p++; *p; p++)
	{
		if (*p == '"' && p[1] != '"')
			return p + 1;
		if (*p == '"')
			p++;				/* (a doubled quote) */
		if (value)
			appendStringInfoChar(value, *p);
	}
	return p;
}

/* The identifier p begins with, as written: quoted (spaces and all) or not. */
static char *
leading_identifier(const char *p)
{
	return pnstrdup(p, scan_identifier(p, NULL) - p);
}

/* An index statement's name: CREATE [UNIQUE] INDEX name ON ... */
static char *
index_name(const char *stmt)
{
	const char *p = strstr(stmt, " INDEX ");

	if (p == NULL)
		elog(ERROR, "unexpected index definition \"%s\"", stmt);
	return leading_identifier(p + strlen(" INDEX "));
}

/*
 * A trigger's, rule's or policy's name: CREATE [CONSTRAINT] TRIGGER name
 * ..., CREATE RULE name ..., CREATE POLICY name ...
 */
static char *
object_name(const char *stmt, const char *word)
{
	const char *p = strstr(stmt, word);

	if (p == NULL)
		elog(ERROR, "unexpected definition \"%s\"", stmt);
	return leading_identifier(p + strlen(word));
}

/*
 * A trigger's or rule's line, as parse_table_def() has it: its statement,
 * and into *enabled when it fires if not as made (append_sorted_defs()'s
 * last field; 0 if none).
 */
static char *
firing_statement(const char *line, char *enabled)
{
	size_t		len = strlen(line);

	if (len >= 2 && line[len - 2] == '\t' && strchr("DRA", line[len - 1]) != NULL)
	{
		*enabled = line[len - 1];
		return pnstrdup(line, len - 2);
	}
	*enabled = 0;
	return pstrdup(line);
}

/*
 * A trigger's or rule's line, made: its statement, then, if it doesn't
 * fire as made, ALTER TABLE ... DISABLE or ENABLE REPLICA or ALWAYS.
 */
static void
make_firing(List **stmts, const char *table, const char *line, bool trigger)
{
	char		enabled;
	char	   *stmt = firing_statement(line, &enabled);

	*stmts = lappend(*stmts, stmt);
	if (enabled)
		*stmts = lappend(*stmts,
						 psprintf("ALTER TABLE %s %s %s %s", table,
								  enabled == 'D' ? "DISABLE" :
								  enabled == 'R' ? "ENABLE REPLICA" : "ENABLE ALWAYS",
								  trigger ? "TRIGGER" : "RULE",
								  object_name(stmt, trigger ? " TRIGGER " : "CREATE RULE ")));
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

/*
 * The schema of a qualified name as an item's key writes it (quoted as
 * needed, a routine's followed by its arguments): what is before its
 * first unquoted '.'; NULL for a name without one (the root's format).
 */
static char *
schema_of(const char *qualified_name)
{
	bool		quoted = false;

	for (const char *c = qualified_name; *c; c++)
	{
		if (*c == '"')
			quoted = !quoted;
		else if (*c == '.' && !quoted)
			return pnstrdup(qualified_name, c - qualified_name);
	}
	return NULL;
}

/*
 * A column of table, as CREATE TABLE and ADD COLUMN write it; with what
 * must run right after that statement appended to *after.
 */
static char *
column_sql(const char *table, const char *line, List *constraints,
		   List **after)
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

	if (name == NULL || type == NULL)
		elog(ERROR, "unexpected column line \"%s\"", line);
	name = unescape_field(name);
	type = unescape_field(type);
	while ((f = strtok(NULL, "\t")) != NULL)
	{
		if (strncmp(f, "collate ", 8) == 0)
			collate = unescape_field(f + 8);
		else if (strncmp(f, "default ", 8) == 0)
			def = unescape_field(f + 8);
		else if (strncmp(f, "generated ", 10) == 0)
			generated = unescape_field(f + 10);
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
	{
		char	   *prefix = psprintf("NOT NULL %s", name);
		size_t		len = strlen(prefix);
		ListCell   *lc;

		/*
		 * Give the identity's implicit NOT NULL its recorded name now.
		 * Automatic naming may choose another name if, for example, a
		 * child still has the constraint inherited from a remade parent.
		 * Postgres refuses NO INHERIT beside IDENTITY in a column
		 * definition, though ALTER can make a table with both: the
		 * constraint is made inheritable, then made NO INHERIT right after.
		 */
		foreach(lc, constraints)
		{
			const char *constraint = lfirst(lc);
			char	   *definition = constraint_def(constraint);

			if (strncmp(definition, prefix, len) == 0 &&
				(definition[len] == '\0' || definition[len] == ' '))
			{
				const char *rest = definition + len;

				appendStringInfo(&sql, " CONSTRAINT %s NOT NULL",
								 first_field(constraint));
				if (strcmp(rest, " NO INHERIT") == 0)
					*after = lappend(*after,
									 psprintf("ALTER TABLE %s ALTER CONSTRAINT %s NO INHERIT",
											  table, first_field(constraint)));
				else
					appendStringInfoString(&sql, rest);
				break;
			}
		}
		appendStringInfo(&sql, " GENERATED %s AS IDENTITY",
						 identity == 'a' ? "ALWAYS" : "BY DEFAULT");
	}
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
			return unescape_field(f + strlen(prefix));
	return NULL;
}

/* A column line's type, as written there (its second field). */
static char *
column_type(const char *line)
{
	const char *tab = strchr(line, '\t');

	if (tab == NULL || tab[1] == '\0' || tab[1] == '\t')
		elog(ERROR, "unexpected column line \"%s\"", line);
	return unescape_field(pnstrdup(tab + 1, strcspn(tab + 1, "\t")));
}

/* The column line (type and collation fields) as ALTER COLUMN TYPE writes it. */
static char *
column_type_sql(const char *line)
{
	char	   *type = column_type(line);
	char	   *collate = column_attr(line, "collate ");

	return collate ? psprintf("%s COLLATE %s", type, collate) : type;
}

static bool
same_strings(const char *a, const char *b)
{
	return (a == NULL && b == NULL) || (a && b && strcmp(a, b) == 0);
}

/* A column line's field that begins with prefix (unescaped), or NULL. */
static char *
column_field(const char *line, const char *prefix)
{
	char	   *copy = pstrdup(line);
	char	   *f;

	(void) strtok(copy, "\t");
	while ((f = strtok(NULL, "\t")) != NULL)
		if (strncmp(f, prefix, strlen(prefix)) == 0)
			return unescape_field(f + strlen(prefix));
	return NULL;
}

/*
 * A partition's own defaults: its columns are its partitioned table's,
 * with the table's defaults when made (from: NULL), but a default may be
 * its own.  (Generated and identity columns are the table's.)
 */
static void
partition_defaults(SchemaMove *sm, const char *name, const TableDef *from,
				   const TableDef *to)
{
	ListCell   *lc;

	foreach(lc, to->columns)
	{
		const char *line = lfirst(lc);
		char	   *col = first_field(line);
		char	   *def = column_field(line, "default ");
		const char *was = NULL;
		ListCell   *fc;

		if (column_field(line, "generated ") || column_field(line, "identity "))
			continue;
		if (from != NULL)
		{
			foreach(fc, from->columns)
				if (strcmp(first_field(lfirst(fc)), col) == 0)
					was = column_field(lfirst(fc), "default ");
			if (same_strings(was, def))
				continue;
		}
		sm->after = lappend(sm->after,
							def ? psprintf("ALTER TABLE %s ALTER COLUMN %s SET DEFAULT %s",
										   name, col, def) :
							psprintf("ALTER TABLE %s ALTER COLUMN %s DROP DEFAULT", name, col));
	}
}

/*
 * Whether move_table() makes a table again, its definition from as to has
 * it (both): a column added before a kept one, kept ones in another order,
 * or a kept one's generation or identity changed, which ALTER can't do in
 * place.  (A partition's columns are its partitioned table's: move_table()
 * keeps it.)
 */
static bool
table_remade(const TableDef *from, const TableDef *to)
{
	bool		remake = false;
	List	   *from_names = NIL;
	List	   *to_names = NIL;
	ListCell   *lc;
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
	return remake;
}

/*
 * The statements turning table name's definition from (NULL: none) into to
 * (NULL: dropped): what must go before the rows are written, and what the
 * rows must satisfy, after.  Columns are added and dropped, and changed in
 * type or default, when that gives to's columns in to's order; else the
 * table is made again (its rows written afresh).  A column's type changes
 * with the rows converted, unless sm->rewritten: then the rows go first
 * (they may not cast, or cast to values its keys or NOT NULLs refuse),
 * all to's written afresh.
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
		/*
		 * A partition goes before its partitioned table, which takes its
		 * partitions with it (and so, IF EXISTS); a partition's sequences
		 * are its parent's.
		 */
		if (from->partition_of)
		{
			/* (its own foreign keys first, as a table's) */
			foreach(lc, from->constraints)
				if (strstr(lfirst(lc), "\tFOREIGN KEY ") != NULL)
					sm->fk_drops = lappend(sm->fk_drops,
										   psprintf("ALTER TABLE %s DROP CONSTRAINT IF EXISTS %s",
													name, first_field(lfirst(lc))));
			sm->before = lcons(psprintf("DROP TABLE IF EXISTS %s", name), sm->before);
			return true;
		}
		/* (its foreign keys first, as other tables' may point at it) */
		foreach(lc, from->constraints)
			if (strstr(lfirst(lc), "\tFOREIGN KEY ") != NULL)
				sm->fk_drops = lappend(sm->fk_drops,
									   psprintf("ALTER TABLE %s DROP CONSTRAINT %s",
												name, first_field(lfirst(lc))));
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
		foreach(lc, from->columns)
			from_names = lappend(from_names, first_field(lfirst(lc)));
		foreach(lc, to->columns)
			to_names = lappend(to_names, first_field(lfirst(lc)));
		remake = table_remade(from, to);
	}
	if (from != NULL && (from->partition_of == NULL) != (to->partition_of == NULL))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("table %s is %s partition at the commit", name,
						to->partition_of ? "a" : "no longer a"),
				 errdetail("A move doesn't make a table a partition, or a partition a table.")));
	if (from != NULL && !same_strings(from->partition_by, to->partition_by))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("table %s is partitioned differently at the commit", name),
				 errdetail("Postgres can't change how a table is partitioned; a move can't either.")));
	/* (a partition's columns are its partitioned table's) */
	if (from != NULL && from->partition_of && to->partition_of)
		remake = false;
	if (remake && to->partition_by)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("partitioned table %s has its columns in another order at the commit", name),
				 errdetail("Making it again would take its partitions with it.")));
	if (from == NULL || remake)
	{
		StringInfoData sql;
		List	   *after_columns = NIL;

		if (from != NULL)
			(void) move_table(sm, name, from, NULL);
		initStringInfo(&sql);
		if (to->partition_of)
		{
			appendStringInfo(&sql, "CREATE TABLE %s PARTITION OF %s %s", name,
							 first_field(to->partition_of), constraint_def(to->partition_of));
			partition_defaults(sm, name, NULL, to);
		}
		else
		{
			appendStringInfo(&sql, "CREATE TABLE %s (", name);
			foreach(lc, to->columns)
				appendStringInfo(&sql, "%s%s", foreach_current_index(lc) ? ", " : "",
								 column_sql(name, lfirst(lc), to->constraints,
											&after_columns));
			appendStringInfoChar(&sql, ')');
		}
		if (to->partition_by)
			appendStringInfo(&sql, " PARTITION BY %s", to->partition_by);
		sm->changes = lappend(sm->changes, sql.data);
		sm->changes = list_concat(sm->changes, after_columns);

		/* sequences its defaults draw from, that no column owns: its own */
		if (!sm->render)
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
	else if (from->partition_of && to->partition_of)
	{
		/*
		 * A partition's columns come with its partitioned table's; moved to
		 * another parent or bound, it is detached before the rows are
		 * written and attached after (which checks them against the bound).
		 */
		if (strcmp(from->partition_of, to->partition_of) != 0)
		{
			/* (first: before its old partitioned table may go, with it) */
			sm->before = lcons(psprintf("ALTER TABLE %s DETACH PARTITION %s",
										first_field(from->partition_of), name),
							   sm->before);
			sm->after = lappend(sm->after,
								psprintf("ALTER TABLE %s ATTACH PARTITION %s %s",
										 first_field(to->partition_of), name,
										 constraint_def(to->partition_of)));
		}
		partition_defaults(sm, name, from, to);
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
				List	   *after_column = NIL;

				sm->changes = lappend(sm->changes,
									  psprintf("ALTER TABLE %s ADD COLUMN %s",
											   name, column_sql(name, line, to->constraints,
																&after_column)));
				sm->changes = list_concat(sm->changes, after_column);
				continue;
			}
			old = list_nth(from->columns, i);
			if (strcmp(column_type_sql(old), column_type_sql(line)) != 0)
			{
				if (sm->rewritten && !list_has_string(sm->emptied, name))
					sm->emptied = lappend(sm->emptied, pstrdup(name));
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

	/*
	 * Type changes reset storage and compression; a new partition inherits
	 * them.  Reapply explicit values even when the column line is unchanged:
	 * type_moves() may have redefined its named type.  Do not recurse into a
	 * child whose own description may specify something else.
	 */
	foreach(lc, to->columns)
	{
		const char *line = lfirst(lc);
		char	   *col = first_field(line);
		const char *old = NULL;
		ListCell   *c;
		static const char *const fields[] = {"storage ", "compression ", "statistics "};
		static const char *const defaults[] = {"DEFAULT", "default", "-1"};

		foreach(c, from->columns)
			if (strcmp(first_field(lfirst(c)), col) == 0)
				old = lfirst(c);
		for (int i = 0; i < lengthof(fields); i++)
		{
			char	   *value = column_attr(line, fields[i]);
			char	   *before = old ? column_attr(old, fields[i]) : NULL;

			if (value || before || (to->partition_of && !old))
				sm->changes = lappend(sm->changes,
									  psprintf("ALTER TABLE ONLY %s ALTER COLUMN %s SET %s%s",
											   name, col, fields[i], value ? value : defaults[i]));
		}
	}

	/* Reset removed options, then set changed ones, including TOAST's. */
	foreach(lc, from->options)
		if (!list_has_string(to->options, lfirst(lc)))
			sm->changes = lappend(sm->changes,
								  psprintf("ALTER TABLE ONLY %s RESET (%s)", name, first_field(lfirst(lc))));
	foreach(lc, to->options)
		if (!list_has_string(from->options, lfirst(lc)))
			sm->changes = lappend(sm->changes,
								  psprintf("ALTER TABLE ONLY %s SET (%s = %s)", name,
										   first_field(lfirst(lc)), constraint_def(lfirst(lc))));

	/*
	 * A typed table's type: it stops being one before its columns change
	 * (first: before its type may go), and is one once they have.  (Its
	 * inheritance: folded_move_schema()'s.)
	 */
	if (!same_strings(from->of, to->of))
	{
		if (from->of)
			sm->before = lcons(psprintf("ALTER TABLE %s NOT OF", name), sm->before);
		if (to->of)
			sm->changes = lappend(sm->changes,
								  psprintf("ALTER TABLE %s OF %s", name, to->of));
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
			sm->fk_drops = lappend(sm->fk_drops, sql);
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
			bool		after_rows;
			char	   *sql;

			if (list_has_string(from->constraints, line) ||
				(pass == 0) != not_null)
				continue;
			sql = psprintf("ALTER TABLE %s ADD CONSTRAINT %s %s", name,
						   first_field(line), constraint_def(line));

			/*
			 * A NOT NULL goes after the rows: a column added to a table with
			 * rows has none in it until they are written; so is one the table
			 * had, whose NULLs the rows written may replace, unless the
			 * primary key, which the rows are keyed by, needs it.
			 */
			if (not_null && from != &none)
			{
				char	   *col = unescape_field(strstr(line, "\tNOT NULL ") + 10);

				after_rows = !list_has_string(from_names, col) || !in_primary_key(to, col);
			}
			else
				after_rows = false;

			/*
			 * A check, or a NOT NULL added after the rows, that the rows
			 * written may break: added unvalidated, validated once the
			 * caller has had them (not one unvalidated in to; nor a
			 * partitioned table's or a partition's, which its partitions'
			 * or parent's validation would reach).
			 */
			if (!sm->render &&
				(strstr(line, "\tCHECK ") != NULL || after_rows) &&
				strstr(constraint_def(line), "NOT VALID") == NULL &&
				to->partition_by == NULL && to->partition_of == NULL)
			{
				ProllyAddedConstraint *ac = palloc(sizeof(ProllyAddedConstraint));

				ac->table = pstrdup(name);
				ac->name = pstrdup(first_field(line));
				ac->foreign_key = false;
				sm->added = lappend(sm->added, ac);
				sm->validations = lappend(sm->validations,
										  psprintf("ALTER TABLE %s VALIDATE CONSTRAINT %s",
												   name, first_field(line)));
				sql = psprintf("%s NOT VALID", sql);
			}

			/*
			 * (an identity column, whose column definition gives its NOT
			 * NULL this recorded name, or a key, may have made it already; a
			 * partition's partitioned table, its own)
			 */
			if (!sm->render && (not_null || to->partition_of))
				sql = psprintf("DO $move$ BEGIN IF NOT EXISTS (SELECT FROM pg_catalog.pg_constraint "
							   "WHERE conrelid = %s::pg_catalog.regclass AND "
							   "pg_catalog.quote_ident(conname) = %s) THEN EXECUTE %s; END IF; END $move$",
							   quote_literal_cstr(name), quote_literal_cstr(first_field(line)),
							   quote_literal_cstr(sql));
			if (after_rows)
				sm->after = lappend(sm->after, sql);
			else if (not_null || key)
				sm->changes = lappend(sm->changes, sql);
			else if (strstr(line, "\tFOREIGN KEY ") != NULL)
			{
				/* (likewise a foreign key, which the rows may break too) */
				if (!sm->render &&
					strstr(constraint_def(line), "NOT VALID") == NULL &&
					to->partition_by == NULL && to->partition_of == NULL)
				{
					ProllyAddedConstraint *ac = palloc(sizeof(ProllyAddedConstraint));

					ac->table = pstrdup(name);
					ac->name = pstrdup(first_field(line));
					ac->foreign_key = true;
					sm->added = lappend(sm->added, ac);
					sm->fk_validations = lappend(sm->fk_validations,
												 psprintf("ALTER TABLE %s VALIDATE CONSTRAINT %s",
														  name, first_field(line)));
					sql = psprintf("%s NOT VALID", sql);
				}
				sm->foreign_keys = lappend(sm->foreign_keys, sql);
			}
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

	/*
	 * The identity index must exist first.  Reapply even an unchanged
	 * identity: its index may have been dropped and made again above.
	 */
	if (to->replica_identity != NIL || from->replica_identity != NIL)
		sm->after = lappend(sm->after,
							psprintf("ALTER TABLE ONLY %s REPLICA IDENTITY %s", name,
									 to->replica_identity ? (char *) linitial(to->replica_identity) : "DEFAULT"));

	foreach(lc, to->triggers)
		if (!list_has_string(from->triggers, lfirst(lc)))
			make_firing(&sm->after, name, lfirst(lc), true);
	foreach(lc, to->rules)
		if (!list_has_string(from->rules, lfirst(lc)))
			make_firing(&sm->after, name, lfirst(lc), false);
	/* Statistics: one changed goes and comes again. */
	foreach(lc, from->statistics)
		if (!list_has_string(to->statistics, lfirst(lc)))
			sm->before = lappend(sm->before,
								 psprintf("DROP STATISTICS IF EXISTS %s",
										  first_field(lfirst(lc))));
	foreach(lc, to->statistics)
		if (!list_has_string(from->statistics, lfirst(lc)))
		{
			char	   *copy = pstrdup(lfirst(lc));
			char	   *tab = strchr(copy, '\t');
			char	   *target = tab ? strchr(tab + 1, '\t') : NULL;

			if (target)
				*target++ = '\0';
			sm->after = lappend(sm->after, unescape_field(tab + 1));
			if (target)
				sm->after = lappend(sm->after,
									psprintf("ALTER STATISTICS %s SET STATISTICS %s",
											 first_field(lfirst(lc)), target));
		}

	/* (policies: every move's, in move_schema) */
	if (from->row_security != to->row_security)
		sm->after = lappend(sm->after,
							psprintf("ALTER TABLE %s %s ROW LEVEL SECURITY", name,
									 to->row_security ? "ENABLE" : "DISABLE"));
	if (from->forced_row_security != to->forced_row_security)
		sm->after = lappend(sm->after,
							psprintf("ALTER TABLE %s %s ROW LEVEL SECURITY", name,
									 to->forced_row_security ? "FORCE" : "NO FORCE"));
	return remake;
}

/* A collation, as described (provider, locale, rules, deterministic). */
static char *
create_collation_sql(const char *name, const char *def)
{
	char	   *f[4];
	char	   *p = pstrdup(def);
	StringInfoData sql;

	for (int i = 0; i < 4; i++)
	{
		char	   *nl = strchr(p, '\n');

		if (nl == NULL)
			elog(ERROR, "unexpected collation definition for %s", name);
		*nl = '\0';
		f[i] = p;
		p = nl + 1;
	}
	initStringInfo(&sql);
	appendStringInfo(&sql, "CREATE COLLATION %s (provider = %s, ", name,
					 f[0][0] == COLLPROVIDER_LIBC ? "libc" :
					 f[0][0] == COLLPROVIDER_ICU ? "icu" : "builtin");
	if (f[0][0] == COLLPROVIDER_LIBC)
	{
		char	   *tab = strchr(f[1], '\t');

		*tab = '\0';
		appendStringInfo(&sql, "lc_collate = %s, lc_ctype = %s",
						 quote_literal_cstr(unescape_field(f[1])),
						 quote_literal_cstr(unescape_field(tab + 1)));
	}
	else
		appendStringInfo(&sql, "locale = %s", quote_literal_cstr(unescape_field(f[1])));
	if (*f[2])
		appendStringInfo(&sql, ", rules = %s", quote_literal_cstr(unescape_field(f[2])));
	appendStringInfo(&sql, ", deterministic = %s)", strcmp(f[3], "t") == 0 ? "true" : "false");
	return sql.data;
}

/* Does anything use a collation? */
static bool
collation_in_use(Oid coll)
{
	Relation	depRel = table_open(DependRelationId, AccessShareLock);
	ScanKeyData key[2];
	SysScanDesc scan;
	bool		used;

	ScanKeyInit(&key[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(CollationRelationId));
	ScanKeyInit(&key[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(coll));
	scan = systable_beginscan(depRel, DependReferenceIndexId, true, NULL, 2, key);
	used = systable_getnext(scan) != NULL;
	systable_endscan(scan);
	table_close(depRel, AccessShareLock);
	return used;
}

/*
 * An ordinary schema a versioned one of its name replaces: what the commit
 * doesn't hold, carried onto the new one (carry_over()).
 */
typedef struct Replaced
{
	char	   *name;			/* unquoted */
	Oid			owner;
	Acl		   *acl;			/* NULL: the default */
	char	   *comment;		/* NULL: none */
	List	   *labels;			/* its security labels: provider, label, ... */
	char		privtype;		/* its pg_init_privs row's, or 0: none */
	Acl		   *initprivs;		/* likewise */
} Replaced;

/* A branch other than the session's whose folded root lacks schema name. */
typedef struct Lacking
{
	Canon	   *cn;
	const char *name;			/* quoted, as a commit has it */
	const char *session;		/* the session's branch */
	char	   *branch;			/* the first found */
} Lacking;

static void
note_lacking(const char *branch, const ProllyHash *head, void *arg)
{
	Lacking    *l = arg;
	ProllyDatabaseComponents dc;
	ProllyNodeRef root;

	if (l->branch != NULL || strcmp(branch, l->session) == 0)
		return;
	prolly_database_components(&dc);
	if (head->len == 0 ||
		!PROLLY_CALL(dc.refs, working_set, branch, &root) ||
		root.hash.len == 0 ||
		find_item(move_items(l->cn, &root), 'n', l->name) == NULL)
		l->branch = pstrdup(branch);
}

/*
 * A schema the move makes, name (quoted) as a commit has it: the statements
 * making it.  An ordinary schema of its name is in the way.  It is every
 * branch's, so it is replaced only where nothing else has a use for it:
 * nothing depends on it (as a new database's public), the database has no
 * versioned schemas yet (its first history, cloned or pushed into it), and
 * every other branch's root has a versioned one of its name, which that
 * branch makes in its place in turn.  Its owner, privileges and comment,
 * which a commit doesn't hold, are noted in *replaced for the new one.
 * Otherwise the move is refused, naming it and what uses it.
 */
static List *
make_schema(Canon *cn, const char *name, List **replaced)
{
	char	   *plain = strVal(linitial(stringToQualifiedNameList(name, NULL)));
	Oid			nsp = get_namespace_oid(plain, true);
	char	   *create = psprintf("CREATE SCHEMA %s VERSIONED", name);
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	dep;
	char	   *user = NULL;	/* what depends on it, the first */
	Lacking		lacking = {cn, name, branch_ops()->current(), NULL};
	HeapTuple	tup;
	Datum		acl;
	bool		isnull;
	Replaced   *r;

	if (!OidIsValid(nsp))
		return list_make1(create);
	depRel = table_open(DependRelationId, AccessShareLock);
	ScanKeyInit(&key[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(NamespaceRelationId));
	ScanKeyInit(&key[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(nsp));
	scan = systable_beginscan(depRel, DependReferenceIndexId, true, NULL, 2, key);
	if ((dep = systable_getnext(scan)) != NULL)
	{
		Form_pg_depend d = (Form_pg_depend) GETSTRUCT(dep);
		ObjectAddress obj;

		ObjectAddressSubSet(obj, d->classid, d->objid, d->objsubid);
		user = getObjectDescription(&obj, true);
		if (user == NULL)
			user = pstrdup("an object");
	}
	systable_endscan(scan);
	table_close(depRel, AccessShareLock);
	if (user != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_SCHEMA),
				 errmsg("schema \"%s\" already exists and isn't versioned", plain),
				 errdetail("The commit's schema \"%s\" is versioned, and %s depends on this one.",
						   plain, user),
				 errhint("Drop it, or rename it, and try again.")));
	if (database_has_versioned_schemas())
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_SCHEMA),
				 errmsg("schema \"%s\" already exists and isn't versioned", plain),
				 errdetail("The commit's schema \"%s\" is versioned, and the database has versioned schemas already; this one is every branch's.",
						   plain),
				 errhint("Drop it, or rename it, and try again.")));
	{
		ProllyDatabaseComponents dc;
		ProllyHash	head = {0};

		/*
		 * (a branch with commits already: those from before may use this
		 * one, and moving back to them would find none)
		 */
		prolly_database_components(&dc);
		if (PROLLY_CALL(dc.refs, resolve, lacking.session, &head) &&
			head.len > 0)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_SCHEMA),
					 errmsg("schema \"%s\" already exists and isn't versioned", plain),
					 errdetail("The commit's schema \"%s\" is versioned, and branch \"%s\" has commits already: only a branch's first history (a clone, or a push into a new database) replaces an ordinary schema.",
							   plain, lacking.session),
					 errhint("Drop it, or rename it, and try again.")));
		PROLLY_CALL(dc.refs, branches, note_lacking, &lacking);
	}
	if (lacking.branch != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_SCHEMA),
				 errmsg("schema \"%s\" already exists and isn't versioned", plain),
				 errdetail("The commit's schema \"%s\" is versioned, and branch \"%s\" has no versioned one: this one is that branch's too.",
						   plain, lacking.branch),
				 errhint("Drop it, or rename it, and try again.")));

	/* what the new one keeps of it */
	tup = SearchSysCache1(NAMESPACEOID, ObjectIdGetDatum(nsp));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for schema %u", nsp);
	r = palloc(sizeof(Replaced));
	r->name = pstrdup(plain);
	r->owner = ((Form_pg_namespace) GETSTRUCT(tup))->nspowner;
	acl = SysCacheGetAttr(NAMESPACEOID, tup, Anum_pg_namespace_nspacl, &isnull);
	r->acl = isnull ? NULL : DatumGetAclPCopy(acl);
	ReleaseSysCache(tup);
	r->comment = GetComment(nsp, NamespaceRelationId, 0);
	r->labels = NIL;
	r->privtype = 0;
	r->initprivs = NULL;
	{
		Relation	rel;
		ScanKeyData skey[3];
		HeapTuple	row;

		rel = table_open(SecLabelRelationId, AccessShareLock);
		ScanKeyInit(&skey[0], Anum_pg_seclabel_objoid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(nsp));
		ScanKeyInit(&skey[1], Anum_pg_seclabel_classoid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(NamespaceRelationId));
		ScanKeyInit(&skey[2], Anum_pg_seclabel_objsubid, BTEqualStrategyNumber,
					F_INT4EQ, Int32GetDatum(0));
		scan = systable_beginscan(rel, SecLabelObjectIndexId, true, NULL, 3, skey);
		while ((row = systable_getnext(scan)) != NULL)
		{
			Datum		provider;
			Datum		label;

			provider = heap_getattr(row, Anum_pg_seclabel_provider,
									RelationGetDescr(rel), &isnull);
			label = heap_getattr(row, Anum_pg_seclabel_label,
								 RelationGetDescr(rel), &isnull);
			r->labels = lappend(lappend(r->labels, TextDatumGetCString(provider)),
								TextDatumGetCString(label));
		}
		systable_endscan(scan);
		table_close(rel, AccessShareLock);

		rel = table_open(InitPrivsRelationId, AccessShareLock);
		ScanKeyInit(&skey[0], Anum_pg_init_privs_objoid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(nsp));
		ScanKeyInit(&skey[1], Anum_pg_init_privs_classoid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(NamespaceRelationId));
		ScanKeyInit(&skey[2], Anum_pg_init_privs_objsubid, BTEqualStrategyNumber,
					F_INT4EQ, Int32GetDatum(0));
		scan = systable_beginscan(rel, InitPrivsObjIndexId, true, NULL, 3, skey);
		if ((row = systable_getnext(scan)) != NULL)
		{
			r->privtype = ((Form_pg_init_privs) GETSTRUCT(row))->privtype;
			acl = heap_getattr(row, Anum_pg_init_privs_initprivs,
							   RelationGetDescr(rel), &isnull);
			r->initprivs = DatumGetAclPCopy(acl);
		}
		systable_endscan(scan);
		table_close(rel, AccessShareLock);
	}
	*replaced = lappend(*replaced, r);
	/* (no CASCADE: nothing is in it) */
	return list_make2(psprintf("DROP SCHEMA %s", name), create);
}

/*
 * The schemas make_schema() replaced, made now: each given the owner,
 * privileges and comment of the ordinary one it replaced, which a commit
 * doesn't hold (a new database's public stays pg_database_owner's, usable
 * by PUBLIC).
 */
static void
carry_over(List *replaced)
{
	ListCell   *lc;

	if (replaced == NIL)
		return;
	CommandCounterIncrement();
	foreach(lc, replaced)
	{
		Replaced   *r = lfirst(lc);
		Oid			nsp = get_namespace_oid(r->name, false);
		Relation	rel = table_open(NamespaceRelationId, RowExclusiveLock);
		HeapTuple	tup = SearchSysCache1(NAMESPACEOID, ObjectIdGetDatum(nsp));
		Datum		values[Natts_pg_namespace] = {0};
		bool		nulls[Natts_pg_namespace] = {0};
		bool		replaces[Natts_pg_namespace] = {0};
		HeapTuple	newtup;
		Oid		   *members = NULL;
		int			nmembers;

		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for schema %u", nsp);
		values[Anum_pg_namespace_nspowner - 1] = ObjectIdGetDatum(r->owner);
		replaces[Anum_pg_namespace_nspowner - 1] = true;
		if (r->acl != NULL)
			values[Anum_pg_namespace_nspacl - 1] = PointerGetDatum(r->acl);
		else
			nulls[Anum_pg_namespace_nspacl - 1] = true;
		replaces[Anum_pg_namespace_nspacl - 1] = true;
		newtup = heap_modify_tuple(tup, RelationGetDescr(rel), values, nulls,
								   replaces);
		CatalogTupleUpdate(rel, &newtup->t_self, newtup);
		ReleaseSysCache(tup);
		heap_freetuple(newtup);
		table_close(rel, RowExclusiveLock);

		/* (made with no privileges given: none to take back) */
		changeDependencyOnOwner(NamespaceRelationId, nsp, r->owner);
		nmembers = r->acl ? aclmembers(r->acl, &members) : 0;
		updateAclDependencies(NamespaceRelationId, nsp, 0, r->owner,
							  0, NULL, nmembers, members);
		if (r->comment != NULL)
			CreateComments(nsp, NamespaceRelationId, 0, r->comment);
		for (int i = 0; i < list_length(r->labels); i += 2)
		{
			ObjectAddress obj;

			ObjectAddressSet(obj, NamespaceRelationId, nsp);
			SetSecurityLabel(&obj, list_nth(r->labels, i),
							 list_nth(r->labels, i + 1));
		}
		if (r->privtype != 0)
		{
			Relation	ip = table_open(InitPrivsRelationId, RowExclusiveLock);
			Datum		ivalues[Natts_pg_init_privs] = {0};
			bool		inulls[Natts_pg_init_privs] = {0};
			HeapTuple	row;

			ivalues[Anum_pg_init_privs_objoid - 1] = ObjectIdGetDatum(nsp);
			ivalues[Anum_pg_init_privs_classoid - 1] =
				ObjectIdGetDatum(NamespaceRelationId);
			ivalues[Anum_pg_init_privs_objsubid - 1] = Int32GetDatum(0);
			ivalues[Anum_pg_init_privs_privtype - 1] = CharGetDatum(r->privtype);
			ivalues[Anum_pg_init_privs_initprivs - 1] =
				PointerGetDatum(r->initprivs);
			row = heap_form_tuple(RelationGetDescr(ip), ivalues, inulls);
			CatalogTupleInsert(ip, row);
			heap_freetuple(row);
			table_close(ip, RowExclusiveLock);
			nmembers = aclmembers(r->initprivs, &members);
			updateInitAclDependencies(NamespaceRelationId, nsp, 0,
									  0, NULL, nmembers, members);
		}
		InvokeObjectPostAlterHook(NamespaceRelationId, nsp, 0);
	}
	CommandCounterIncrement();
}

/* A cast, as described (its method, then its context). */
static char *
create_cast_sql(const char *name, const char *def)
{
	char	   *copy = pstrdup(def);
	char	   *method = strtok(copy, "\n");
	char	   *context = unescape_field(strtok(NULL, "\n"));
	char	   *tab = strchr(method, '\t');
	StringInfoData sql;

	initStringInfo(&sql);
	appendStringInfo(&sql, "CREATE CAST (%s) ", name);
	if (tab)
		appendStringInfo(&sql, "WITH FUNCTION %s", unescape_field(tab + 1));
	else if (strcmp(method, "inout") == 0)
		appendStringInfoString(&sql, "WITH INOUT");
	else
		appendStringInfoString(&sql, "WITHOUT FUNCTION");
	if (strcmp(context, "assignment") == 0)
		appendStringInfoString(&sql, " AS ASSIGNMENT");
	else if (strcmp(context, "implicit") == 0)
		appendStringInfoString(&sql, " AS IMPLICIT");
	return sql.data;
}

/*
 * A type, as described ("enum", "domain", "composite" or "range", then
 * lines).  A domain's unvalidated checks, which CREATE DOMAIN can't make,
 * are appended to *unvalidated as ALTER DOMAIN, to run once the columns
 * using it have moved to it: as on a live database, the values those
 * columns have are then kept.
 */
static char *
create_type_sql(const char *name, const char *def, List **unvalidated)
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
			appendStringInfo(&sql, "%s%s", first ? "" : ", ", unescape_field(line));
			first = false;
		}
		appendStringInfoChar(&sql, ')');
	}
	else if (strcmp(kind, "range") == 0)
	{
		/* (strtok would skip an empty line: split by hand) */
		char	   *f[5];
		char	   *p = copy + strlen(kind) + 1;

		for (int i = 0; i < 5; i++)
		{
			char	   *nl = strchr(p, '\n');

			if (nl == NULL)
				elog(ERROR, "unexpected range type definition for %s", name);
			*nl = '\0';
			f[i] = unescape_field(p);
			p = nl + 1;
		}
		appendStringInfo(&sql, "CREATE TYPE %s AS RANGE (SUBTYPE = %s, SUBTYPE_OPCLASS = %s",
						 name, f[0], f[1]);
		if (*f[2])
			appendStringInfo(&sql, ", COLLATION = %s", f[2]);
		if (*f[3])
			appendStringInfo(&sql, ", SUBTYPE_DIFF = %s", f[3]);
		appendStringInfo(&sql, ", MULTIRANGE_TYPE_NAME = %s)", f[4]);
	}
	else if (strcmp(kind, "composite") == 0)
	{
		List	   *lines = NIL;
		ListCell   *lc;

		/* (an attribute's line is a column's: column_type_sql(), which strtoks) */
		while ((line = strtok(NULL, "\n")) != NULL)
			lines = lappend(lines, line);
		appendStringInfo(&sql, "CREATE TYPE %s AS (", name);
		foreach(lc, lines)
			appendStringInfo(&sql, "%s%s %s", foreach_current_index(lc) ? ", " : "",
							 first_field(lfirst(lc)), column_type_sql(lfirst(lc)));
		appendStringInfoChar(&sql, ')');
	}
	else
	{
		char	   *base = strtok(NULL, "\n");
		char	   *f = strchr(base, '\t');
		char	   *fields;
		bool		not_null = false;
		char	   *collate = NULL;
		char	   *dflt = NULL;

		/* its base type's line: then not null, collate, default fields */
		if (f)
			*f++ = '\0';
		for (f = f ? strtok_r(f, "\t", &fields) : NULL; f; f = strtok_r(NULL, "\t", &fields))
		{
			if (strcmp(f, "not null") == 0)
				not_null = true;
			else if (strncmp(f, "collate ", 8) == 0)
				collate = unescape_field(f + 8);
			else if (strncmp(f, "default ", 8) == 0)
				dflt = unescape_field(f + 8);
		}
		appendStringInfo(&sql, "CREATE DOMAIN %s AS %s", name, unescape_field(base));
		if (collate)
			appendStringInfo(&sql, " COLLATE %s", collate);
		if (dflt)
			appendStringInfo(&sql, " DEFAULT %s", dflt);
		/* (a NOT NULL constraint line names it: not said twice) */
		if (not_null && strstr(def, "\tNOT NULL\n") == NULL &&
			(strlen(def) < 9 || strcmp(def + strlen(def) - 9, "\tNOT NULL") != 0))
			appendStringInfoString(&sql, " NOT NULL");
		while ((line = strtok(NULL, "\n")) != NULL)
		{
			char	   *condef = constraint_def(line);

			/*
			 * CREATE DOMAIN cannot accept NOT VALID: those checks come after
			 * (above).  Validated checks stay inline: ALTER would scan
			 * existing uses.  Test the deparser's suffix, not words inside
			 * an expression.
			 */
			if (pg_str_endswith(condef, " NOT VALID"))
				*unvalidated = lappend(*unvalidated,
									   psprintf("ALTER DOMAIN %s ADD CONSTRAINT %s %s",
												name, first_field(line), condef));
			else
				appendStringInfo(&sql, " CONSTRAINT %s %s", first_field(line),
								 condef);
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

/* Is a routine's definition an aggregate's? */
static bool
is_aggregate(const MoveItem *it)
{
	return strncmp(it->def, "CREATE OR REPLACE AGGREGATE", 27) == 0;
}

/* DROP FUNCTION, PROCEDURE or AGGREGATE, as the definition says. */
static char *
drop_routine_sql(const MoveItem *it)
{
	return psprintf("DROP %s IF EXISTS %s",
					strstr(it->def, "CREATE OR REPLACE PROCEDURE") == it->def ?
					"PROCEDURE" : is_aggregate(it) ? "AGGREGATE" : "FUNCTION",
					it->name);
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

/*
 * A materialized view, as described (append_matview_state()'s lines, then
 * its query): made, unpopulated if so, then its indexes.
 */
static List *
make_matview_sql(const char *name, const char *def)
{
	const char *query = def;
	bool		populated = true;
	List	   *indexes = NIL;
	List	   *settings = NIL;
	StringInfoData with;
	size_t		len;

	initStringInfo(&with);

	while (*query && *query != ' ' && *query != '(')
	{
		const char *nl = strchr(query, '\n');
		char	   *line;

		if (nl == NULL)
			elog(ERROR, "unexpected materialized view definition for %s", name);
		line = pnstrdup(query, nl - query);
		if (strcmp(line, MATVIEW_UNPOPULATED) == 0)
			populated = false;
		else if (strncmp(line, MATVIEW_OPTION, strlen(MATVIEW_OPTION)) == 0)
		{
			const char *option = line + strlen(MATVIEW_OPTION);

			appendStringInfo(&with, "%s%s = %s", with.len > 0 ? ", " : "",
							 first_field(option), constraint_def(option));
		}
		else if (strncmp(line, MATVIEW_COLUMN, strlen(MATVIEW_COLUMN)) == 0)
		{
			const char *column = line + strlen(MATVIEW_COLUMN);
			static const char *const fields[] = {"storage ", "compression ", "statistics "};

			for (int i = 0; i < lengthof(fields); i++)
			{
				char	   *value = column_attr(column, fields[i]);

				if (value)
					settings = lappend(settings,
									   psprintf("ALTER MATERIALIZED VIEW %s ALTER COLUMN %s SET %s%s",
												name, first_field(column), fields[i], value));
			}
		}
		else
			indexes = lappend(indexes, unescape_field(line));
		query = nl + 1;
	}
	/* (the deparsed query ends its statement: WITH NO DATA after it) */
	len = strlen(query);
	if (len > 0 && query[len - 1] == ';')
		len--;
	return lcons(psprintf("CREATE MATERIALIZED VIEW %s%s%s%s AS %.*s WITH %sDATA", name,
						  with.len > 0 ? " WITH (" : "", with.data, with.len > 0 ? ")" : "",
						  (int) len, query, populated ? "" : "NO "),
				 list_concat(settings, indexes));
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
	(void) scan_identifier(ident, &out);
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
		names = lappend(names, first_field(line));
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

		types = lappend(types, tab ? unescape_field(end ? pnstrdup(tab + 1, end - tab - 1) :
													tab + 1) :
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
static bool is_aggregate(const MoveItem *it);
static char *drop_routine_sql(const MoveItem *it);

typedef struct TypeMoves
{
	List	   *before;			/* what uses them dropped; then each renamed
								 * aside */
	List	   *creates;		/* the types made (these and new ones), in any
								 * order */
	List	   *converts;		/* the columns using them moved to the new */
	List	   *unvalidated;	/* then the new domains' unvalidated checks */
	List	   *tables;			/* the tables of those columns */
	List	   *detached;		/* typed tables detached before their types
								 * are replaced */
	List	   *drops;			/* the old ones, in any order */
	List	   *after;			/* what used them, made again */
	List	   *aggregate_drops;	/* (before the functions they use; made
									 * again with the others, after them) */
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

/*
 * Does anything use a range type, its multirange type or an array of
 * either (anything but what comes with them)?
 */
static bool
range_in_use(Oid typid)
{
	Oid			multi = get_range_multirange(typid);
	Oid			types[4] = {typid, get_array_type(typid), multi,
	get_array_type(multi)};
	Relation	depRel = table_open(DependRelationId, AccessShareLock);
	bool		used = false;

	for (int i = 0; i < 4 && !used; i++)
	{
		ScanKeyData key[2];
		SysScanDesc scan;
		HeapTuple	tup;

		if (!OidIsValid(types[i]))
			continue;
		ScanKeyInit(&key[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(TypeRelationId));
		ScanKeyInit(&key[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(types[i]));
		scan = systable_beginscan(depRel, DependReferenceIndexId, true, NULL, 2, key);
		while (!used && (tup = systable_getnext(scan)) != NULL)
		{
			Form_pg_depend d = (Form_pg_depend) GETSTRUCT(tup);

			/* (its constructors take it, and are part of it) */
			used = d->deptype != DEPENDENCY_INTERNAL &&
				!part_of_type(d->classid, d->objid);
		}
		systable_endscan(scan);
	}
	table_close(depRel, AccessShareLock);
	return used;
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
	/* (renamed aside, its constructors and multirange would collide) */
	if (typ->typtype == TYPTYPE_RANGE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("the branch can't be moved to a commit that defines type %s otherwise",
						user),
				 errdetail("Range type %s uses it, and a range type in use can't be made again in place.",
						   name)));
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

/* Whether a partitioned table's key uses its column attnum. */
static bool
in_partition_key(Oid relid, AttrNumber attnum)
{
	Relation	rel = table_open(relid, AccessShareLock);
	bool		expr;
	bool		used = has_partition_attrs(rel,
										   bms_make_singleton(attnum - FirstLowInvalidHeapAttributeNumber),
										   &expr);

	table_close(rel, AccessShareLock);
	return used;
}

/*
 * The statements that make the types to defines otherwise than from, under
 * their names, and those that use them, in turn: each renamed aside, made
 * anew, the columns of versioned tables using it (or arrays of it)
 * converted, and the old dropped.  What uses one is found through what
 * depends on it: a composite type or a domain the branch has is moved
 * with it (as to defines it); typed tables are detached for move_table()
 * to attach again once their columns match; a check constraint, an index
 * or a default of a versioned table, or a routine, is dropped first and
 * made again after (as to defines it, if alike; the table's or routine's
 * own move does the rest).  Anything else using it (a column of another
 * schema's table, an operator) can't be moved with it: refused.  Views and
 * materialized views using them go and come back with the others
 * (folded_move_schema()).
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

		/*
		 * A range type can't be made otherwise beside itself (renamed aside,
		 * its constructors and multirange would collide), nor altered: one
		 * nothing uses goes and comes again; one in use is refused.
		 */
		if (f->kind == 'y' && t != NULL && strcmp(t->def, f->def) != 0 &&
			(strncmp(f->def, "range\n", 6) == 0 || strncmp(t->def, "range\n", 6) == 0))
		{
			Oid			typid = typenameTypeId(NULL, makeTypeNameFromNameList(stringToQualifiedNameList(f->name, NULL)));

			if (range_in_use(typid))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("the branch can't be moved to a commit that defines type %s otherwise",
								f->name),
						 errdetail("A range type in use can't be changed in place; a move can't change one either.")));
			tm.before = lappend(tm.before, psprintf("DROP TYPE %s", f->name));
			tm.creates = lappend(tm.creates, create_type_sql(t->name, t->def,
															 &tm.unvalidated));
			tm.any = true;
			continue;
		}
		if (f->kind == 'y' && t != NULL && strcmp(t->def, f->def) != 0)
			(void) move_type(&moved, from, to,
							 typenameTypeId(NULL, makeTypeNameFromNameList(stringToQualifiedNameList(f->name, NULL))),
							 f->name);
	}
	tm.any = tm.any || moved != NIL;

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
					else if ((kind == RELKIND_RELATION || kind == RELKIND_PARTITIONED_TABLE) &&
							 versioned && d->objsubid == 0 && !array)
					{
						char	   *rel = qualified(get_rel_namespace(d->objid), get_rel_name(d->objid));
						MoveItem   *f = find_item(from, 't', rel);
						TableDef	fd;

						if (f)
						{
							parse_table_def(f->def, &fd);
							if (same_strings(fd.of, mt->from->name))
							{
								tm.before = lappend(tm.before, psprintf("ALTER TABLE %s NOT OF", rel));
								tm.detached = lappend(tm.detached, rel);
								handled = true;
							}
						}
					}
					else if (kind == RELKIND_PARTITIONED_TABLE && versioned &&
							 d->objsubid > 0 && in_partition_key(d->objid, d->objsubid))
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("the branch can't be moved to a commit that defines type %s otherwise",
										mt->from->name),
								 errdetail("%s uses it, in the table's partition key, which can't change type.",
										   getObjectDescription(&obj, false))));
					else if ((kind == RELKIND_RELATION || kind == RELKIND_PARTITIONED_TABLE) &&
							 versioned && d->objsubid > 0)
					{
						char	   *rel = qualified(get_rel_namespace(d->objid), get_rel_name(d->objid));
						const char *col = quote_identifier(get_attname(d->objid, d->objsubid, false));
						char	   *conv;
						bool		inherited = false;

						/*
						 * (a partition's inherited column is its parent's:
						 * converting the parent's converts it)
						 */
						if (get_rel_relispartition(d->objid))
						{
							HeapTuple	at = SearchSysCache2(ATTNUM, ObjectIdGetDatum(d->objid),
															 Int16GetDatum(d->objsubid));

							if (!HeapTupleIsValid(at))
								elog(ERROR, "cache lookup failed for attribute %d of relation %u",
									 d->objsubid, d->objid);
							inherited = ((Form_pg_attribute) GETSTRUCT(at))->attinhcount > 0;
							ReleaseSysCache(at);
						}
						if (array && !inherited)
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
						else if (!inherited)
							tm.converts = lappend(tm.converts,
												  psprintf("ALTER TABLE %s ALTER COLUMN %s TYPE %s USING %s",
														   rel, col, mt->to->name,
														   converted_value(col, mt->from->def, mt->to->def,
																		   mt->to->name)));
						if (!list_has_string(tm.tables, rel))
							tm.tables = lappend(tm.tables, rel);
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
														constraint_def(table_def_line(tt, 'k', cname))));
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
				else if (d->classid == ProcedureRelationId &&
						 part_of_type(ProcedureRelationId, d->objid))
					/* (a range type's constructor: the range is) */
					(void) move_type(&moved, from, to, get_func_rettype(d->objid),
									 mt->from->name);
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
					if (fp && is_aggregate(fp))
					{
						tm.aggregate_drops = lappend(tm.aggregate_drops, drop_routine_sql(fp));
						handled = true;
					}
					else if (fp)
					{
						tm.before = lcons(drop_routine_sql(fp), tm.before);
						if (tp && strcmp(tp->def, fp->def) == 0)
							tm.after = lappend(tm.after, tp->def);
						handled = true;
					}
				}
				else if (d->classid == RewriteRelationId)
					handled = true; /* (a view's: it goes, and comes again) */
				else if (d->classid == PolicyRelationId)
					handled = true; /* (a policy's: every move's go, and come) */
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
		tm.creates = lappend(tm.creates, create_type_sql(mt->to->name, mt->to->def,
														 &tm.unvalidated));
		tm.drops = lappend(tm.drops, psprintf("DROP TYPE %s", mt->aside));
	}
	tm.before = list_concat(list_copy(tm.aggregate_drops), tm.before);
	return tm;
}

/*
 * Where the branch's indexes are stored, when not in the default
 * tablespace.  Placement is the cluster's, not the commit's: the canonical
 * form describes an index without its tablespace (describe_table()), so an
 * index a move drops and makes again (its definition changed, its table
 * made again, a column under it changing type) goes back where it was,
 * rather than to default_tablespace.  An index the branch didn't have is
 * made where CREATE INDEX makes it.  The rule is by name, so a partition's
 * index made again for its redefined partitioned index (named afresh) is
 * left in the default tablespace.  Placement is kept at best: a user
 * without CREATE on the tablespace leaves an index made again where it was
 * made, with a warning, rather than failing the move.
 */
typedef struct IndexPlace
{
	char	   *schema;
	char	   *index;
	Oid			tablespace;
} IndexPlace;

static List *
index_places(List *items)
{
	List	   *places = NIL;
	ListCell   *lc;

	foreach(lc, items)
	{
		MoveItem   *f = lfirst(lc);
		char	   *schema;
		char	   *table;
		Oid			nsp;
		Oid			relid;
		Relation	rel;
		List	   *indexes;
		ListCell   *ic;

		if (f->kind != 't')
			continue;
		/* (by the catalogs: what the move reads needs no privilege on it) */
		DeconstructQualifiedName(stringToQualifiedNameList(f->name, NULL),
								 &schema, &table);
		nsp = schema ? get_namespace_oid(schema, true) : InvalidOid;
		relid = OidIsValid(nsp) ? get_relname_relid(table, nsp) : InvalidOid;
		if (!OidIsValid(relid))
			continue;
		rel = relation_open(relid, AccessShareLock);
		indexes = RelationGetIndexList(rel);
		relation_close(rel, NoLock);
		foreach(ic, indexes)
		{
			Oid			idx = lfirst_oid(ic);
			Oid			spc = get_rel_tablespace(idx);
			IndexPlace *p;

			if (!OidIsValid(spc))
				continue;
			p = palloc(sizeof(IndexPlace));
			p->schema = get_namespace_name(get_rel_namespace(idx));
			p->index = get_rel_name(idx);
			p->tablespace = spc;
			places = lappend(places, p);
		}
	}
	return places;
}

/* The indexes index_places() found, made again elsewhere, moved back. */
static void
restore_index_places(List *places)
{
	ListCell   *lc;

	CommandCounterIncrement();
	foreach(lc, places)
	{
		IndexPlace *p = lfirst(lc);
		Oid			nsp = get_namespace_oid(p->schema, true);
		Oid			idx = OidIsValid(nsp) ? get_relname_relid(p->index, nsp) : InvalidOid;
		char	   *spc;
		char		kind;

		if (!OidIsValid(idx))
			continue;			/* the move dropped it */
		kind = get_rel_relkind(idx);
		if ((kind != RELKIND_INDEX && kind != RELKIND_PARTITIONED_INDEX) ||
			get_rel_tablespace(idx) == p->tablespace)
			continue;			/* not made again, or another object */
		spc = get_tablespace_name(p->tablespace);
		if (spc == NULL)
			continue;
		if (p->tablespace != MyDatabaseTableSpace &&
			object_aclcheck(TableSpaceRelationId, p->tablespace, GetUserId(),
							ACL_CREATE) != ACLCHECK_OK)
		{
			ereport(WARNING,
					(errmsg("index \"%s.%s\" made again in the default tablespace, not in \"%s\"",
							p->schema, p->index, spc),
					 errdetail("Permission to create in tablespace \"%s\" is needed to move it back.",
							   spc)));
			continue;
		}
		run_statements(list_make1(psprintf("ALTER INDEX %s SET TABLESPACE %s",
										   quote_qualified_identifier(p->schema, p->index),
										   quote_identifier(spc))));
	}
}

/*
 * The table name (qualified) emptied, each partition of it with storage, by
 * the applier the move writes rows with: no trigger fires (one enabled
 * ALWAYS neither), no rule applies and no foreign key is checked, and a
 * table row-level security applies to is refused, as its rows written
 * would be.  A row read is removed as it is: the scan's snapshot keeps
 * seeing those removed.
 */
static void
empty_table(const char *name)
{
	const ProllyApplierRoutine *ar = prolly_applier_lookup("replication");
	Oid			relid = RangeVarGetRelid(makeRangeVarFromNameList(stringToQualifiedNameList(name, NULL)),
										 RowExclusiveLock, false);
	List	   *rels;
	ListCell   *lc;

	CommandCounterIncrement();

	/*
	 * (A partitioned table's partitions; a table inherited from keeps its
	 * children's rows: each child is a table of its own, emptied only if it
	 * is itself.)
	 */
	if (get_rel_relkind(relid) == RELKIND_PARTITIONED_TABLE)
		rels = find_all_inheritors(relid, RowExclusiveLock, NULL);
	else
		rels = list_make1_oid(relid);
	foreach(lc, rels)
	{
		Relation	rel = table_open(lfirst_oid(lc), NoLock);
		ProllyApplier *a;
		TableScanDesc scan;
		TupleTableSlot *slot;
		Snapshot	snap;

		if (rel->rd_rel->relkind != RELKIND_RELATION)
		{
			table_close(rel, NoLock);
			continue;
		}
		a = PROLLY_CALL(ar, begin, rel);
		slot = table_slot_create(rel, NULL);
		snap = RegisterSnapshot(GetLatestSnapshot());
		scan = table_beginscan(rel, snap, 0, NULL, SO_NONE);
		while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
		{
			slot_getallattrs(slot);
			if (!PROLLY_CALL(ar, remove, a, slot))
				elog(ERROR, "could not find a row of \"%s\" to remove",
					 RelationGetRelationName(rel));
		}
		table_endscan(scan);
		UnregisterSnapshot(snap);
		ExecDropSingleTupleTableSlot(slot);
		PROLLY_CALL(ar, end, a);
		table_close(rel, NoLock);
	}
	CommandCounterIncrement();
}

static bool
same_lines(List *a, List *b)
{
	ListCell   *x;
	ListCell   *y;

	if (list_length(a) != list_length(b))
		return false;
	forboth(x, a, y, b)
		if (strcmp(lfirst(x), lfirst(y)) != 0)
			return false;
	return true;
}

static bool
same_str(const char *a, const char *b)
{
	return (a == NULL) == (b == NULL) && (a == NULL || strcmp(a, b) == 0);
}

/* Of lines, those whose first field is one of names (all, if names is NIL). */
static List *
lines_named(List *lines, List *names, bool all)
{
	List	   *out = NIL;
	ListCell   *lc;

	foreach(lc, lines)
		if (all || list_has_string(names, first_field(lfirst(lc))))
			out = lappend(out, lfirst(lc));
	return out;
}

/*
 * Whether a move leaves what a table shares with the tables it inherits
 * from, or they with it, as it is: its parents, type and partitioning, the
 * same in from and to (table name, in both), its columns and constraints
 * too (only those named shared_cols and shared_cons, unless all: what it
 * shares as a child), and no column of it changing type in turn
 * (types_moved: type_moves()'s tables).
 */
static bool
inheritance_unmoved(List *from, List *to, const char *name, List *types_moved,
					List *shared_cols, List *shared_cons, bool all)
{
	MoveItem   *f = find_item(from, 't', name);
	MoveItem   *t = find_item(to, 't', name);
	TableDef	fd;
	TableDef	td;

	if (f == NULL || t == NULL || list_has_string(types_moved, name))
		return false;
	if (strcmp(f->def, t->def) == 0)
		return true;
	parse_table_def(f->def, &fd);
	parse_table_def(t->def, &td);
	return same_lines(lines_named(fd.columns, shared_cols, all),
					  lines_named(td.columns, shared_cols, all)) &&
		same_lines(lines_named(fd.constraints, shared_cons, all),
				   lines_named(td.constraints, shared_cons, all)) &&
		same_lines(fd.inherits, td.inherits) &&
		same_lines(fd.local, td.local) &&
		same_str(fd.of, td.of) &&
		same_str(fd.partition_by, td.partition_by) &&
		same_str(fd.partition_of, td.partition_of);
}

/*
 * Whether table name's inheritance (td's, as from or to defines it) stays
 * as it is through a move: neither the tables it inherits from change what
 * they share (their columns and constraints, all of them), nor it what it
 * shares with them (its columns and constraints of their names, and which
 * of those are its own too; its own may change with the links in place)
 * (inheritance_unmoved()), nor is it made again.  What changes it is done
 * to each table alone, unlinked: an ALTER of a parent's column would
 * recurse to its children, and an inherited column can't be altered
 * alone.  (A link made again makes all the child has its own:
 * restore_locality() makes it as to describes it.)
 */
static bool
inheritance_kept(List *from, List *to, const char *name, const TableDef *td,
				 List *types_moved)
{
	List	   *cols = NIL;
	List	   *cons = NIL;
	ListCell   *lc;

	foreach(lc, td->inherits)
	{
		MoveItem   *pf = find_item(from, 't', lfirst(lc));
		MoveItem   *pt = find_item(to, 't', lfirst(lc));

		/* (one of another schema's isn't the move's) */
		if (pf == NULL && pt == NULL)
			continue;
		if (!inheritance_unmoved(from, to, lfirst(lc), types_moved, NIL, NIL, true))
			return false;
		/* (unmoved: both alike) */
		{
			TableDef	pd;
			ListCell   *c;

			parse_table_def(pf->def, &pd);
			foreach(c, pd.columns)
				cols = lappend(cols, first_field(lfirst(c)));
			foreach(c, pd.constraints)
				cons = lappend(cons, first_field(lfirst(c)));
		}
	}
	if (!inheritance_unmoved(from, to, name, types_moved, cols, cons, false))
		return false;

	/* (nor is it made again, which leaves it inheriting from none) */
	{
		MoveItem   *f = find_item(from, 't', name);
		MoveItem   *t = find_item(to, 't', name);
		TableDef	fd;
		TableDef	td2;

		parse_table_def(f->def, &fd);
		parse_table_def(t->def, &td2);
		return !table_remade(&fd, &td2);
	}
}

/*
 * Each table linked again (tables, by name), what it inherits its own or
 * not (attislocal, conislocal) as to describes it (its "local" section):
 * ALTER TABLE INHERIT makes all it has its own, the table's columns and
 * constraints made before the link, so a later change of a parent would
 * otherwise not reach them.
 */
static void
restore_locality(List *tables, List *to)
{
	Relation	pg_attribute = NULL;
	Relation	pg_constraint = NULL;
	ListCell   *lc;

	foreach(lc, tables)
	{
		const char *name = lfirst(lc);
		MoveItem   *t = find_item(to, 't', name);
		Oid			relid = RangeVarGetRelid(makeRangeVarFromNameList(stringToQualifiedNameList(name, NULL)),
											 NoLock, true);
		TableDef	td;
		ScanKeyData key;
		SysScanDesc scan;
		HeapTuple	tup;

		if (t == NULL || !OidIsValid(relid))
			continue;
		parse_table_def(t->def, &td);
		if (pg_attribute == NULL)
		{
			pg_attribute = table_open(AttributeRelationId, RowExclusiveLock);
			pg_constraint = table_open(ConstraintRelationId, RowExclusiveLock);
		}
		ScanKeyInit(&key, Anum_pg_attribute_attrelid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(relid));
		scan = systable_beginscan(pg_attribute, AttributeRelidNumIndexId, true,
								  NULL, 1, &key);
		while ((tup = systable_getnext(scan)) != NULL)
		{
			Form_pg_attribute att = (Form_pg_attribute) GETSTRUCT(tup);
			bool		local;
			HeapTuple	copy;

			if (att->attnum <= 0 || att->attisdropped || att->attinhcount == 0)
				continue;
			local = list_has_string(td.local, local_line(true, NameStr(att->attname)));
			if (att->attislocal == local)
				continue;
			copy = heap_copytuple(tup);
			((Form_pg_attribute) GETSTRUCT(copy))->attislocal = local;
			CatalogTupleUpdate(pg_attribute, &copy->t_self, copy);
			heap_freetuple(copy);
		}
		systable_endscan(scan);
		ScanKeyInit(&key, Anum_pg_constraint_conrelid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(relid));
		scan = systable_beginscan(pg_constraint, ConstraintRelidTypidNameIndexId,
								  true, NULL, 1, &key);
		while ((tup = systable_getnext(scan)) != NULL)
		{
			Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tup);
			bool		local;
			HeapTuple	copy;

			if (con->coninhcount == 0)
				continue;
			local = list_has_string(td.local, local_line(false, NameStr(con->conname)));
			if (con->conislocal == local)
				continue;
			copy = heap_copytuple(tup);
			((Form_pg_constraint) GETSTRUCT(copy))->conislocal = local;
			CatalogTupleUpdate(pg_constraint, &copy->t_self, copy);
			heap_freetuple(copy);
		}
		systable_endscan(scan);
		CacheInvalidateRelcacheByRelid(relid);
	}
	if (pg_attribute != NULL)
	{
		table_close(pg_constraint, RowExclusiveLock);
		table_close(pg_attribute, RowExclusiveLock);
	}
	CommandCounterIncrement();
}

/*
 * Make the session branch's objects defined as to has them, from as from
 * has them (the branch folded now): the statements before the rows, then
 * rows(arg), which writes them, then what they must satisfy (the checks,
 * NOT NULLs and foreign keys they may break unvalidated, breaking(them,
 * arg), their validation); and, if materialized views were made again,
 * rows(arg) once more, for theirs.  rewritten: rows(arg) writes every
 * table's rows as to has them (see prolly_canon.h).
 */
static void
move_schema(const ProllyNodeRef *from_root, const ProllyNodeRef *to_root,
			void (*rows) (void *arg),
			void (*breaking) (List *added, void *arg), void *arg,
			bool rewritten)
{
	bool		matviews_made = false;
	List	   *replaced = NIL; /* ordinary schemas they replace */
	Canon		cn;
	List	   *from;
	List	   *to;
	SchemaMove	sm = {NIL, NIL, NIL, NIL, NIL, NIL, NIL, NIL, NIL, NIL, rewritten};
	List	   *views = NIL;
	List	   *view_drops = NIL;
	List	   *matview_drops = NIL;
	List	   *keyed;
	List	   *identities;
	List	   *late = NIL;		/* what the changes stop using, dropped */
	List	   *routines = NIL;
	List	   *aggregates_made = NIL;
	List	   *policy_drops = NIL;
	List	   *policy_makes = NIL;
	List	   *remade = NIL;
	List	   *schemas = NIL;
	List	   *type_drops = NIL;
	List	   *collation_drops = NIL;
	List	   *collation_makes = NIL;
	List	   *schema_drops = NIL;
	List	   *unlinks = NIL;		/* inheritance, broken first */
	List	   *links = NIL;		/* and made last */
	List	   *relinked = NIL; /* (the tables linked, by name) */
	List	   *places;			/* the branch's indexes' tablespaces */
	TypeMoves	tm;
	bool		matviews_change;
	ListCell   *lc;
	int			save_nestlevel;
	MemoryContext caller = CurrentMemoryContext;

	memset(&cn, 0, sizeof(cn));
	canon_env(&cn);
	check_format(&cn, to_root, "moved to");
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
				/*
				 * (an aggregate going, first: before the functions and the
				 * tables' row types it uses; what stays doesn't use it)
				 */
				if (t == NULL && is_aggregate(f))
					sm.before = lcons(drop_routine_sql(f), sm.before);
				else if (t == NULL)
					routines = lappend(routines, drop_routine_sql(f));
				break;
			case 'l':
				if (t == NULL)
					collation_drops = lappend(collation_drops,
											  psprintf("DROP COLLATION IF EXISTS %s", f->name));
				else
				{
					/* (one can't be altered: one nothing uses goes, and comes again) */
					Oid			coll = get_collation_oid(stringToQualifiedNameList(f->name, NULL),
														 false);

					if (collation_in_use(coll))
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("the branch can't be moved to a commit that defines collation %s otherwise",
										f->name),
								 errdetail("A collation in use can't be changed in place; a move can't change one either.")));
					tm.before = lappend(tm.before, psprintf("DROP COLLATION %s", f->name));
				}
				break;
			case 'x':
				/* (before the routines and types it uses go) */
				sm.before = lcons(psprintf("DROP CAST IF EXISTS (%s)", f->name),
								  sm.before);
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

		if (f->kind != 't' ||
			(t && strcmp(t->def, f->def) == 0 &&
			 !list_has_string(tm.tables, f->name) &&
			 !list_has_string(tm.detached, f->name)))
			continue;
		parse_table_def(f->def, &fd);
		/* type_moves() detaches it first; move_table() restores to's link. */
		if (list_has_string(tm.detached, f->name))
			fd.of = NULL;
		if (t)
			parse_table_def(t->def, &td);
		if (move_table(&sm, f->name, &fd, t ? &td : NULL))
			remade = lappend(remade, f->name);
	}

	/*
	 * Tables whose columns change type in turn (type_moves()) are emptied
	 * too, as move_table() empties those it changes: converted, their rows
	 * may break what the new type's column must satisfy (a NOT NULL, a
	 * unique index).
	 */
	if (rewritten)
		foreach(lc, tm.tables)
			if (!list_has_string(sm.emptied, lfirst(lc)))
				sm.emptied = lappend(sm.emptied, lfirst(lc));

	/*
	 * Foreign keys both sides have, into a table made again or dropped,
	 * emptied, or whose keys change: they go first (sm.fk_drops), and the
	 * ones kept come back last, checked against the rows written.
	 */
	keyed = list_concat(list_concat_copy(remade, sm.rekeyed), sm.emptied);
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
				sm.fk_drops = lappend(sm.fk_drops,
									  psprintf("ALTER TABLE %s DROP CONSTRAINT %s",
											   f->name, first_field(line)));
				if (t)
					sm.foreign_keys = lappend(sm.foreign_keys,
											  psprintf("ALTER TABLE %s ADD CONSTRAINT %s %s",
													   f->name, first_field(line),
													   constraint_def(line)));
				break;
			}
		}
	}
	/*
	 * Inheritance: a table's links to the tables it inherits from go first
	 * and come back last (once the rows are in, and the constraints they
	 * merge with valid), unless neither it nor they change what they share
	 * (inheritance_kept()).
	 */
	foreach(lc, from)
	{
		MoveItem   *f = lfirst(lc);
		TableDef	fd;
		ListCell   *c;

		if (f->kind != 't')
			continue;
		parse_table_def(f->def, &fd);
		if (fd.inherits == NIL || inheritance_kept(from, to, f->name, &fd, tm.tables))
			continue;
		foreach(c, fd.inherits)
			unlinks = lappend(unlinks, psprintf("ALTER TABLE %s NO INHERIT %s",
												f->name, (char *) lfirst(c)));
	}
	foreach(lc, to)
	{
		MoveItem   *t = lfirst(lc);
		TableDef	td;
		ListCell   *c;

		if (t->kind != 't')
			continue;
		parse_table_def(t->def, &td);
		if (td.inherits == NIL || inheritance_kept(from, to, t->name, &td, tm.tables))
			continue;

		relinked = lappend(relinked, t->name);
		foreach(c, td.inherits)
			links = lappend(links, psprintf("ALTER TABLE %s INHERIT %s",
											t->name, (char *) lfirst(c)));
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
			schemas = list_concat(schemas,
								  make_schema(&cn, t->name, &replaced));
	}
	foreach(lc, to)
	{
		MoveItem   *t = lfirst(lc);
		MoveItem   *f = find_item(from, t->kind, t->name);

		if (f && strcmp(t->def, f->def) == 0)
			continue;
		if (t->kind == 'n')
			continue;			/* (above) */
		if (t->kind == 'l')
			collation_makes = lappend(collation_makes,
									  create_collation_sql(t->name, t->def));
		else if (t->kind == 'y' && f == NULL)
			tm.creates = lappend(tm.creates, create_type_sql(t->name, t->def,
															 &tm.unvalidated));
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
		else if (t->kind != 'f' && t->kind != 't' && t->kind != 'v' && t->kind != 'x')
			goto unsupported;
	}
	{
		/* (a type move's drop of an aggregate counts too) */
		bool		functions_dropped = routines != NIL || tm.aggregate_drops != NIL;

		/* (a function a routine's signature change drops) */
		foreach(lc, to)
		{
			MoveItem   *t = lfirst(lc);
			MoveItem   *f = find_item(from, t->kind, t->name);

			if (t->kind == 'f' && f && !is_aggregate(t) && strcmp(t->def, f->def) != 0 &&
				strcmp(routine_signature(f->def), routine_signature(t->def)) != 0)
				functions_dropped = true;
		}

		/*
		 * Aggregates: where a function goes, any may use it, so all go
		 * before the functions and all are made after them (and after the
		 * tables, whose row types they may use); otherwise one changed is
		 * replaced in place, what uses it kept.
		 */
		foreach(lc, from)
		{
			MoveItem   *f = lfirst(lc);

			if (f->kind == 'f' && is_aggregate(f) && functions_dropped &&
				find_item(to, 'f', f->name) != NULL)
				sm.before = lcons(drop_routine_sql(f), sm.before);
		}
		foreach(lc, to)
		{
			MoveItem   *t = lfirst(lc);
			MoveItem   *f = find_item(from, t->kind, t->name);

			if (t->kind != 'f')
				continue;
			if (is_aggregate(t))
			{
				if (functions_dropped || !f || strcmp(t->def, f->def) != 0)
					aggregates_made = lappend(aggregates_made, t->def);
				continue;
			}
			if (f && strcmp(t->def, f->def) == 0)
				continue;
			/* (CREATE OR REPLACE can't change what a routine returns) */
			if (f && strcmp(routine_signature(f->def), routine_signature(t->def)) != 0)
				sm.before = lappend(sm.before, drop_routine_sql(f));
			sm.before = lappend(sm.before, t->def);
		}
	}
	/* Casts coming, once the types and routines they name are made. */
	foreach(lc, to)
	{
		MoveItem   *t = lfirst(lc);
		MoveItem   *f = find_item(from, t->kind, t->name);

		if (t->kind == 'x' && !(f && strcmp(t->def, f->def) == 0))
			sm.before = lappend(sm.before, create_cast_sql(t->name, t->def));
	}
	/* Tables made: a partition after its partitioned table. */
	{
		List	   *pending = NIL;
		List	   *made = NIL;

		foreach(lc, to)
		{
			MoveItem   *t = lfirst(lc);

			if (t->kind == 't' && find_item(from, 't', t->name) == NULL)
			{
				TableDef   *td = palloc(sizeof(TableDef));

				parse_table_def(t->def, td);
				pending = lappend(pending, list_make2(t, td));
			}
		}
		while (pending != NIL)
		{
			List	   *later = NIL;

			foreach(lc, pending)
			{
				MoveItem   *t = linitial(lfirst(lc));
				TableDef   *td = lsecond(lfirst(lc));
				char	   *parent = td->partition_of ? first_field(td->partition_of) : NULL;

				if (parent && !list_has_string(made, parent) &&
					find_item(from, 't', parent) == NULL &&
					find_item(to, 't', parent) != NULL)
				{
					later = lappend(later, lfirst(lc));
					continue;
				}
				move_table(&sm, t->name, NULL, td);
				made = lappend(made, t->name);
			}
			if (list_length(later) == list_length(pending))
			{
				TableDef   *td = lsecond(linitial(later));

				elog(ERROR, "partitioned table %s can't be made before its partitions",
					 first_field(td->partition_of));
			}
			pending = later;
		}
	}
	/* (aggregates after the tables, whose row types they may use) */
	sm.changes = list_concat(sm.changes, aggregates_made);
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
			views = lappend(views,
							psprintf(strncmp(t->def, "WITH (", 6) == 0 ?
									 "CREATE VIEW %s %s" : "CREATE VIEW %s AS %s",
									 t->name, t->def));
		/*
		 * materialized views made again once the rows are in; their rows,
		 * as to holds them, written after (those of the others are written
		 * with the tables')
		 */
		else if (t->kind == 'm' && matviews_change)
		{
			views = list_concat(views, make_matview_sql(t->name, t->def));
			matviews_made = true;
		}
	}

	/* Run them as deparsed: names qualified, routines' bodies unchecked. */
	save_nestlevel = NewGUCNestLevel();
	/*
	 * Policies: every one goes first and comes last, as views do, as they
	 * may use any table, view, column, type or function the move changes.
	 */
	foreach(lc, from)
	{
		MoveItem   *f = lfirst(lc);
		TableDef	td;
		ListCell   *pc;

		if (f->kind != 't')
			continue;
		parse_table_def(f->def, &td);
		foreach(pc, td.policies)
			policy_drops = lappend(policy_drops,
								   psprintf("DROP POLICY IF EXISTS %s ON %s",
											object_name(lfirst(pc), "CREATE POLICY "),
											f->name));
	}
	foreach(lc, to)
	{
		MoveItem   *t = lfirst(lc);
		TableDef	td;

		if (t->kind != 't')
			continue;
		parse_table_def(t->def, &td);
		policy_makes = list_concat(policy_makes, td.policies);
	}

	(void) set_config_option("search_path", "pg_catalog", PGC_USERSET,
							 PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	(void) set_config_option("check_function_bodies", "off", PGC_USERSET,
							 PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	/* (what's already gone, an IF EXISTS skips: no notice of it) */
	(void) set_config_option("client_min_messages", "warning", PGC_USERSET,
							 PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	SPI_connect();
	places = index_places(from);
	run_statements(policy_drops);

	/*
	 * Rows to go before their columns change type (with every row written
	 * afresh), as rows the move writes go (empty_table()).
	 */
	run_statements(sm.fk_drops);
	foreach(lc, sm.emptied)
		empty_table(lfirst(lc));
	make_views(list_concat(matviews_change ? matview_drops : NIL, view_drops));
	run_statements(unlinks);

	/*
	 * Types first, in turn (type_moves()): new schemas made once the types
	 * changed are aside, all the types made (each after those it uses), the
	 * columns using the changed ones moved, the new domains' unvalidated
	 * checks added, the old ones dropped.
	 */
	run_statements(tm.before);
	run_statements(schemas);
	carry_over(replaced);
	run_statements(collation_makes);	/* (types and columns may use them) */
	make_views(tm.creates);
	run_statements(tm.converts);
	run_statements(tm.unvalidated);
	make_views(tm.drops);
	run_statements(tm.after);
	run_statements(list_concat(list_concat(sm.before, sm.changes), late));
	make_views(type_drops);
	run_statements(collation_drops);
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
	if (breaking && sm.added != NIL)
	{
		MemoryContext spi;

		CommandCounterIncrement();
		spi = MemoryContextSwitchTo(caller);
		breaking(sm.added, arg);
		MemoryContextSwitchTo(spi);
	}
	run_statements(sm.validations);
	if (breaking == NULL)
		run_statements(sm.fk_validations);
	run_statements(links);
	restore_locality(relinked, to);
	make_views(views);
	run_statements(policy_makes);
	restore_index_places(places);
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
 * ... every row written, and every primary key's index built over rows,
 * from trees a commit held: none is refused for its key's length (see
 * prolly_table.c).
 */
static void
folded_move_schema(const ProllyNodeRef *from_root, const ProllyNodeRef *to_root,
				   void (*rows) (void *arg),
				   void (*breaking) (List *added, void *arg), void *arg,
				   bool rewritten)
{
	bool		held = prolly_writing_held_rows(true);

	PG_TRY();
	{
		move_schema(from_root, to_root, rows, breaking, arg, rewritten);
	}
	PG_FINALLY();
	{
		prolly_writing_held_rows(held);
	}
	PG_END_TRY();
}

/*
 * A table's description in its sections (describe_table()'s), each a list
 * of its lines as written (fields escaped).  The first five are always
 * written; the others only when they have lines.
 */
static const char *const table_sections[] = {
	"columns", "constraints", "indexes", "triggers", "rules", "policies",
	"statistics", "partition by", "partition of", "inherits", "of", "local",
	"options", "replica identity"
};
#define NUM_TABLE_SECTIONS lengthof(table_sections)
#define ALWAYS_WRITTEN_SECTIONS 5

static bool
split_table_def(const char *def, List **sections)
{
	char	   *copy = pstrdup(def);
	int			s = -1;
	char	   *line;
	char	   *next;

	for (int i = 0; i < NUM_TABLE_SECTIONS; i++)
		sections[i] = NIL;
	for (line = copy; line && *line; line = next)
	{
		bool		header = false;

		next = strchr(line, '\n');
		if (next)
			*next++ = '\0';
		/* (a section's lines can't be its header: a statement or a field) */
		for (int i = s + 1; i < NUM_TABLE_SECTIONS && !header; i++)
			if (strcmp(line, table_sections[i]) == 0)
			{
				s = i;
				header = true;
			}
		if (header)
			continue;
		if (s < 0)
			return false;
		sections[s] = lappend(sections[s], line);
	}
	return true;
}

/* What a line of section s is the definition of: its name. */
static char *
table_line_key(int s, const char *line)
{
	switch (s)
	{
		case 2:
			return unquoted(index_name(unescape_field(line)));
		case 3:
			return object_name(unescape_field(line), " TRIGGER ");
		case 4:
			return object_name(unescape_field(line), " RULE ");
		case 5:
			if (strcmp(line, ROW_SECURITY) == 0 ||
				strcmp(line, FORCED_ROW_SECURITY) == 0)
				return pstrdup(line);
			return object_name(unescape_field(line), " POLICY ");
		case 7:
		case 8:
		case 10:
		case 13:
			return pstrdup("");	/* (one line, the table's) */
		default:
			return pnstrdup(line, strcspn(line, "\t"));	/* its first field */
	}
}

/*
 * A line of a section of a table's description, with what it's the
 * definition of (its key), where it comes in its side's section, and what
 * the merged section is sorted by.
 */
typedef struct TableLine
{
	const char *line;
	char	   *key;
	int			pos;
	int			rank;			/* (policies: row security lines first) */
	char	   *sort;
} TableLine;

static int
table_line_key_cmp(const void *a, const void *b)
{
	return strcmp(((const TableLine *) a)->key, ((const TableLine *) b)->key);
}

static int
table_line_pos_cmp(const void *a, const void *b)
{
	return pg_cmp_s32(((const TableLine *) a)->pos, ((const TableLine *) b)->pos);
}

static int
table_line_sort_cmp(const void *a, const void *b)
{
	const TableLine *x = a;
	const TableLine *y = b;

	if (x->rank != y->rank)
		return pg_cmp_s32(x->rank, y->rank);
	return strcmp(x->sort, y->sort);
}

/* Section s's lines, each with its key computed once, sorted by key. */
static TableLine *
table_lines(int s, List *lines)
{
	TableLine  *tl = palloc_array(TableLine, list_length(lines) + 1);
	ListCell   *lc;

	foreach(lc, lines)
	{
		TableLine  *l = &tl[foreach_current_index(lc)];

		l->line = lfirst(lc);
		l->key = table_line_key(s, l->line);
		l->pos = foreach_current_index(lc);
		l->rank = 0;
		l->sort = NULL;
	}
	qsort(tl, list_length(lines), sizeof(TableLine), table_line_key_cmp);
	return tl;
}

/*
 * How a merged section is sorted, as its description has it: constraints
 * and statistics objects by their lines, indexes by name, triggers and
 * rules by their statements, policies by theirs after the row security
 * lines; columns, the partitioning and the tables inherited from (and a
 * typed table's type), as they come.
 */
static void
set_table_line_sort(int s, TableLine *l)
{
	switch (s)
	{
		case 1:
		case 6:
		case 11:
		case 12:
			l->sort = unconstify(char *, l->line);
			break;
		case 2:
			l->sort = l->key;
			break;
		case 5:
			if (strcmp(l->key, l->line) == 0)
			{
				l->rank = strcmp(l->line, ROW_SECURITY) == 0 ? 0 : 1;
				l->sort = "";
				break;
			}
			l->rank = 2;
			pg_fallthrough;
		case 3:
		case 4:
			l->sort = unescape_field(l->line);
			break;
		default:
			l->sort = "";
			break;
	}
}

/*
 * Whether merged constraints could stand together on one table: no more
 * than one primary key, nor one column with two NOT NULLs.  Merged by
 * name, two sides' keys or NOT NULLs named differently would both be
 * kept.
 */
static bool
constraints_compatible(TableLine *merged, int n)
{
	List	   *not_null = NIL;
	int			keys = 0;

	for (int i = 0; i < n; i++)
	{
		const char *nn = strstr(merged[i].line, "\tNOT NULL ");

		if (strstr(merged[i].line, "\tPRIMARY KEY ") != NULL && ++keys > 1)
			return false;
		if (nn != NULL)
		{
			char	   *col = unescape_field(nn + 10);

			if (list_has_string(not_null, col))
				return false;
			not_null = lappend(not_null, col);
		}
	}
	return true;
}

/* Whether two sides have a part alike: both the same line, or neither one. */
static bool
same_line(const TableLine *a, const TableLine *b)
{
	if (a == NULL || b == NULL)
		return a == b;
	return strcmp(a->line, b->line) == 0;
}

/*
 * A table's description as both sides changed it from base's, merged part
 * by part, as Dolt merges a table's schema: each column, constraint, index
 * (and trigger, rule, policy, statistics object; and its partitioning) as
 * the side that changed it has it, both alike as they have it.  Columns
 * come in ours' order, then those only theirs added, in theirs'.  NULL if
 * both changed a part differently, a merged local marker names a column
 * or constraint the merge drops, or a drop would discard a part the other
 * side kept while removing an inheritance parent: a schema conflict.
 */
static char *
merge_table_defs(const char *base, const char *ours, const char *theirs)
{
	List	   *sec[3][NUM_TABLE_SECTIONS];
	List	   *indexes = NIL;
	List	   *local_parts = NIL; /* local markers for the parts kept */
	bool		detached[3] = {false, false, false};
	StringInfoData out;

	if (!split_table_def(base, sec[0]) || !split_table_def(ours, sec[1]) ||
		!split_table_def(theirs, sec[2]))
		return NULL;
	for (int side = 1; side < 3; side++)
	{
		foreach_ptr(char, parent, sec[0][9])
		{
			if (!list_has_string(sec[side][9], parent))
			{
				detached[side] = true;
				break;
			}
		}
	}
	initStringInfo(&out);
	for (int s = 0; s < NUM_TABLE_SECTIONS; s++)
	{
		TableLine  *tl[3];
		int			len[3];
		int			at[3] = {0, 0, 0};
		TableLine  *merged;
		int			n = 0;

		for (int side = 0; side < 3; side++)
		{
			tl[side] = table_lines(s, sec[side][s]);
			len[side] = list_length(sec[side][s]);
		}
		merged = palloc_array(TableLine, len[0] + len[1] + len[2] + 1);

		/* every part any side has, the three sides' lines walked by key */
		for (;;)
		{
			const char *key = NULL;
			const TableLine *x[3] = {NULL, NULL, NULL};
			const TableLine *m;

			for (int side = 0; side < 3; side++)
				if (at[side] < len[side] &&
					(key == NULL || strcmp(tl[side][at[side]].key, key) < 0))
					key = tl[side][at[side]].key;
			if (key == NULL)
				break;
			for (int side = 0; side < 3; side++)
				if (at[side] < len[side] && strcmp(tl[side][at[side]].key, key) == 0)
					x[side] = &tl[side][at[side]++];
			if (same_line(x[1], x[0]))
				m = x[2];
			else if (same_line(x[2], x[0]) || same_line(x[1], x[2]))
				m = x[1];
			else
				return NULL;
			if (m == NULL)
			{
				/*
				 * NO INHERIT can make a retained part local without a
				 * local marker: it no longer has an inheritance count.
				 * Don't let the other side's drop silently discard it.
				 * Both sides dropping the part is still agreement.
				 */
				if ((s == 0 || s == 1) &&
					((x[1] && detached[1]) || (x[2] && detached[2])))
					return NULL;
				continue;
			}

			/* (where it comes: as ours has it, then theirs, then base) */
			merged[n] = *m;
			merged[n].pos = x[1] ? x[1]->pos :
				x[2] ? len[1] + x[2]->pos : len[1] + len[2] + x[0]->pos;
			set_table_line_sort(s, &merged[n]);
			n++;
		}
		if (s == 0 || (s >= 7 && s <= 10))
			qsort(merged, n, sizeof(TableLine), table_line_pos_cmp);
		else
			qsort(merged, n, sizeof(TableLine), table_line_sort_cmp);
		if (s == 1 && !constraints_compatible(merged, n))
			return NULL;
		if (s == 2)
			for (int i = 0; i < n; i++)
				indexes = lappend(indexes, merged[i].key);

		/*
		 * (a replica identity by an index one side set, the other dropped:
		 * neither side's)
		 */
		if (s == 13 && n == 1 &&
			strncmp(merged[0].line, IDENTITY_BY_INDEX, strlen(IDENTITY_BY_INDEX)) == 0 &&
			!list_has_string(indexes,
							 unquoted(unescape_field(merged[0].line + strlen(IDENTITY_BY_INDEX)))))
			return NULL;
		if (n == 0 && s >= ALWAYS_WRITTEN_SECTIONS)
			continue;
		appendStringInfo(&out, "%s\n", table_sections[s]);
		for (int i = 0; i < n; i++)
		{
			if (s == 0 || s == 1)
				local_parts = lappend(local_parts,
									 psprintf("%s %s", s == 0 ? "column" : "constraint",
											  merged[i].key));
			/*
			 * Locality and the part's definition merge separately.  One
			 * side making an inherited part local mustn't lose it to the
			 * other side's drop from the parent.
			 */
			else if (s == 11 && !list_has_string(local_parts, merged[i].line))
				return NULL;
			appendStringInfo(&out, "%s\n", merged[i].line);
		}
	}
	return out.data;
}

/*
 * The longest value of a string type that a change to another merges
 * across (string_change_merges()), -1 for a type that isn't one: varchar,
 * char and text, of any length.
 */
static int32
string_type_length(const char *type)
{
	ErrorSaveContext escontext = {T_ErrorSaveContext};
	Oid			typid;
	int32		typmod;

	if (!parseTypeString(type, &typid, &typmod, (Node *) &escontext))
		return -1;
	if (typid != VARCHAROID && typid != BPCHAROID && typid != TEXTOID)
		return -1;
	return typmod < 0 ? PG_INT32_MAX : typmod - VARHDRSZ;
}

/*
 * Whether a column changed from type from to type to on one side merges
 * with the other side's changes, as Dolt's does: only between string types
 * (varchar, char, text), and only if the longest value doesn't shrink.
 * Other changes Dolt can't make to the other side's rows, and calls a
 * schema conflict.
 */
static bool
string_change_merges(const char *from, const char *to)
{
	int32		from_len = string_type_length(from);
	int32		to_len = string_type_length(to);

	return from_len >= 0 && to_len >= 0 && to_len >= from_len;
}

/*
 * A table both sides changed (its rows or its definition), whose
 * definition one side changed, fails to merge as a schema conflict where
 * that side changed a column's type in a way Dolt doesn't merge
 * (string_change_merges()).  A column both changed differently is a
 * conflict already (merge_table_defs()).
 */
static void
check_type_changes(const char *name, const char *base, const char *ours,
				   const char *theirs)
{
	List	   *sec[3][NUM_TABLE_SECTIONS];
	TableLine  *tl[3];
	int			len[3];

	if (!split_table_def(base, sec[0]) || !split_table_def(ours, sec[1]) ||
		!split_table_def(theirs, sec[2]))
		return;
	for (int side = 0; side < 3; side++)
	{
		tl[side] = table_lines(0, sec[side][0]);
		len[side] = list_length(sec[side][0]);
	}
	for (int i = 0; i < len[0]; i++)
	{
		const char *col = tl[0][i].key;
		char	   *type[3];
		bool		found = true;

		type[0] = column_type(tl[0][i].line);
		for (int side = 1; side < 3 && found; side++)
		{
			TableLine	want = {.key = unconstify(char *, col)};
			TableLine  *l = bsearch(&want, tl[side], len[side], sizeof(TableLine),
									table_line_key_cmp);

			found = l != NULL;
			if (found)
				type[side] = column_type(l->line);
		}
		if (!found)
			continue;
		for (int side = 1; side < 3; side++)
		{
			char	   *other = type[3 - side];

			if (strcmp(type[side], type[0]) != 0 && strcmp(other, type[0]) == 0 &&
				!string_change_merges(type[0], type[side]))
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("schema conflict: column %s of %s changed type from %s to %s on one side, and the other side changed the table",
								col, name, type[0], type[side]),
						 errdetail("A change of type merges with the other side's changes only as a string type made no shorter.")));
		}
	}
}


/*
 * Whether a merged table's columns (td's) have each of columns' (a
 * parent's, as a table's lines, or a composite type's) by name and type;
 * exactly them, in order, if exact (a typed table's).
 */
static bool
columns_cover(const TableDef *td, List *names, List *types, bool exact)
{
	ListCell   *n;
	ListCell   *t;
	int			i = 0;

	if (exact && list_length(td->columns) != list_length(names))
		return false;
	forboth(n, names, t, types)
	{
		bool		found = false;
		ListCell   *lc;

		foreach(lc, td->columns)
		{
			const char *line = lfirst(lc);

			if (exact && foreach_current_index(lc) != i)
				continue;
			if (strcmp(first_field(line), lfirst(n)) == 0 &&
				strcmp(column_type(line), lfirst(t)) == 0)
			{
				found = true;
				break;
			}
		}
		if (!found)
			return false;
		i++;
	}
	return true;
}

/*
 * What Postgres requires of a merged table's inheritance and type, which
 * merging its definition and its parents' (or its type's) apart can break:
 * a table has every column of each table it inherits from, of the same
 * type, and a typed table exactly its type's attributes; and each is
 * there, if any side had it.  Otherwise a schema conflict: the move would
 * fail making it.  tables and types: the merge's, as names, then
 * definitions; seen: every table's and type's key any side had.
 */
static void
check_merged_inheritance(List *tables, List *table_defs, List *types,
						 List *type_defs, List *seen)
{
	ListCell   *a;
	ListCell   *b;

	forboth(a, tables, b, table_defs)
	{
		const char *name = lfirst(a);
		TableDef	td;
		ListCell   *lc;

		parse_table_def(lfirst(b), &td);
		foreach(lc, td.inherits)
		{
			char	   *parent = unescape_field(lfirst(lc));
			int			at = list_member_index_string(tables, parent);
			TableDef	pd;
			List	   *names = NIL;
			List	   *coltypes = NIL;
			ListCell   *c;

			if (at < 0)
			{
				if (list_has_string(seen, psprintf("t:%s", parent)))
					ereport(ERROR,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("schema conflict: %s inherits from %s on one side, and the other side dropped %s",
									name, parent, parent),
							 errdetail("A table can't inherit from a table that isn't there.")));
				continue;		/* (another schema's) */
			}
			parse_table_def(list_nth(table_defs, at), &pd);
			foreach(c, pd.columns)
			{
				names = lappend(names, first_field(lfirst(c)));
				coltypes = lappend(coltypes, column_type(lfirst(c)));
			}
			if (!columns_cover(&td, names, coltypes, false))
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("schema conflict: %s inherits from %s, whose columns the merge doesn't give it",
								name, parent),
						 errdetail("One side changed the inheritance, and the other the columns of either table; a table inheriting from another must have each of its columns, of the same type.")));
		}
		if (td.of)
		{
			int			at = list_member_index_string(types, td.of);

			if (at < 0)
			{
				if (list_has_string(seen, psprintf("y:%s", td.of)))
					ereport(ERROR,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("schema conflict: %s is of type %s on one side, and the other side dropped %s",
									name, td.of, td.of),
							 errdetail("A typed table's type must be there.")));
				continue;
			}
			if (strncmp(list_nth(type_defs, at), "composite\n", 10) != 0 ||
				!columns_cover(&td, composite_attributes(list_nth(type_defs, at)),
							   composite_attribute_types(list_nth(type_defs, at)), true))
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("schema conflict: %s is of type %s, whose attributes the merge doesn't give it as its columns",
								name, td.of),
						 errdetail("One side made the table typed, and the other changed its columns or the type's attributes; a typed table's columns are exactly its type's.")));
		}
	}
}

/*
 * The definitions of base, ours and theirs merged: each object as the side
 * that changed it since base has it (ours if neither, or both alike); a
 * table both changed, part by part (merge_table_defs()).  An object both
 * changed differently is a schema conflict, and fails; so is a schema one
 * side dropped that the other put objects in.  Only the
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
	List	   *tables = NIL;	/* the tables it keeps, by name */
	List	   *table_defs = NIL;	/* (their definitions) */
	List	   *types = NIL;	/* likewise its types */
	List	   *type_defs = NIL;
	List	   *seen = NIL;		/* every table's and type's key, any side's */
	ListCell   *lc;

	memset(&cn, 0, sizeof(cn));
	canon_env(&cn);
	/* (a merge base too: its definitions are what both sides changed from) */
	check_format(&cn, base_root, "merged");
	check_format(&cn, ours_root, "merged");
	check_format(&cn, theirs_root, "merged");
	cur[0] = PROLLY_CALL(cn.tree, cursor_open, &cn.env, base_root, CurrentMemoryContext);
	cur[1] = PROLLY_CALL(cn.tree, cursor_open, &cn.env, ours_root, CurrentMemoryContext);
	cur[2] = PROLLY_CALL(cn.tree, cursor_open, &cn.env, theirs_root, CurrentMemoryContext);
	for (int i = 0; i < 3; i++)
	{
		PROLLY_CALL(cn.tree, cursor_seek, cur[i], NULL, ForwardScanDirection);
		more[i] = PROLLY_CALL(cn.tree, cursor_next, cur[i], &k[i], &v[i]);
	}
	b = PROLLY_CALL(cn.tree, build_begin, &cn.root_env, CurrentMemoryContext);
	while (more[0] || more[1] || more[2])
	{
		const ProllyKey *least = NULL;
		bool		has[3];
		ProllyValue def[3];
		const ProllyValue *take = NULL;
		ProllyValue table;
		char	   *merged_def = NULL;

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
#define CHANGED(a) (v[a].len != v[0].len || memcmp(v[a].data, v[0].data, v[0].len) != 0)
		if (least->data[0] == 't' && has[0] && has[1] && has[2] &&
			CHANGED(1) && CHANGED(2) && !(SAME(0, 1) && SAME(0, 2)) && !SAME(1, 2))
			check_type_changes(from_utf8(least->data + 2, least->len - 2),
							   pnstrdup((const char *) def[0].data, def[0].len),
							   pnstrdup((const char *) def[1].data, def[1].len),
							   pnstrdup((const char *) def[2].data, def[2].len));
		if (SAME(0, 2))
			take = has[1] ? &v[1] : NULL;	/* theirs left it: ours */
		else if (SAME(0, 1) || SAME(1, 2))
			take = has[2] ? &v[2] : NULL;	/* ours left it, or both alike: theirs */
		else if (least->data[0] == 'c')
			take = has[1] ? &v[1] : &v[2];	/* (collations: as recorded) */
		else if (least->data[0] == 't' && has[0] && has[1] && has[2] &&
				 (merged_def = merge_table_defs(pnstrdup((const char *) def[0].data, def[0].len),
												pnstrdup((const char *) def[1].data, def[1].len),
												pnstrdup((const char *) def[2].data, def[2].len))) != NULL)
		{
			/* ours' item, its definition the merged one */
			StringInfoData item;

			initStringInfo(&item);
			appendBinaryStringInfo(&item, (const char *) v[1].data,
								   def[1].data - v[1].data);
			appendStringInfoString(&item, merged_def);
			table.data = (const uint8 *) item.data;
			table.len = item.len;
			take = &table;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("schema conflict: %s was changed differently on both sides",
							from_utf8(least->data + 2, least->len - 2)),
					 errdetail("Both branches made, dropped or redefined it since their common ancestor.")));
#undef SAME
#undef CHANGED
		if (take)
			PROLLY_CALL(cn.tree, build_add, b, least, take);
		if (least->data[0] == 't' || least->data[0] == 'y')
		{
			seen = lappend(seen, from_utf8(least->data, least->len));
			if (take)
			{
				ProllyValue d;
				char	   *name = from_utf8(least->data + 2, least->len - 2);
				char	   *text = merged_def;

				if (text == NULL)
				{
					definition_of(least, take, &d);
					text = pnstrdup((const char *) d.data, d.len);
				}
				if (least->data[0] == 't')
				{
					tables = lappend(tables, name);
					table_defs = lappend(table_defs, text);
				}
				else
				{
					types = lappend(types, name);
					type_defs = lappend(type_defs, text);
				}
			}
		}
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
	check_merged_inheritance(tables, table_defs, types, type_defs, seen);

	/* a schema dropped on one side, given objects on the other */
	if (dropped != NIL)
	{
		foreach(lc, kept)
		{
			char	   *schema = schema_of(lfirst(lc));

			if (schema && list_has_string(dropped, schema))
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("schema conflict: schema %s was dropped on one side and given %s on the other",
								schema, (char *) lfirst(lc)),
						 errdetail("Dropping it would drop what the other side made in it.")));
			if (schema)
				pfree(schema);
		}
	}
}

/* (edits in key order, as the root's items are) */
static int
edit_key_cmp(const void *a, const void *b)
{
	return prolly_bytes_compare(&((const ProllyEdit *) a)->key,
								&((const ProllyEdit *) b)->key, NULL);
}

/* An edit putting item it, as it is. */
static void
put_edit(ProllyEdit *edits, int *nedits, const CanonItem *it)
{
	ProllyEdit *e = &edits[(*nedits)++];

	e->key.data = (const uint8 *) it->key.data;
	e->key.len = it->key.len;
	e->is_delete = false;
	e->value.data = (const uint8 *) it->value.data;
	e->value.len = it->value.len;
}

/*
 * head's item named key into *v; false if head has none.
 */
static bool
head_item(Canon *cn, const ProllyNodeRef *head, const StringInfoData *key,
		  StringInfo v)
{
	ProllyKey	k = {(const uint8 *) key->data, key->len};

	return PROLLY_CALL(cn->tree, get, &cn->env, head, &k, v);
}

static bool
folded_fold_rows(const ProllyNodeRef *head, const ProllyRowChanges *changes,
				 int nchanges, const Oid *sequences, int nsequences,
				 ProllyNodeRef *root)
{
	Canon		cn;
	ProllyEdit *edits = palloc_array(ProllyEdit, Max(nchanges + nsequences, 1));
	int			nedits = 0;
	int			save_nestlevel;
	bool		ok = true;
	StringInfoData v;

	memset(&cn, 0, sizeof(cn));
	canon_env(&cn);
	cn.finder = prolly_change_finder();
	if (!root_in_format(&cn, head))
		return false;
	initStringInfo(&v);

	/* (described as a commit describes them: every name qualified) */
	save_nestlevel = NewGUCNestLevel();
	(void) set_config_option("search_path", "pg_catalog", PGC_USERSET,
							 PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	CommandCounterIncrement();
	PushActiveSnapshot(GetTransactionSnapshot());
	PG_TRY();
	{
		for (int i = 0; ok && i < nchanges; i++)
		{
			Relation	rel = try_table_open(changes[i].relid, AccessShareLock);
			CanonItem  *it;
			StringInfoData desc;
			ProllyKey	k;
			ProllyValue hv;
			ProllyNodeRef data;
			ProllyValue described;
			ProllyNodeRef rows;

			if (rel == NULL)
			{
				ok = false;
				break;
			}
			it = add_item(&cn, "t:", qualified(RelationGetNamespace(rel),
											   RelationGetRelationName(rel)));
			resetStringInfo(&v);
			initStringInfo(&desc);
			if (rel->rd_rel->relam != PROLLY_TABLE_AM_OID ||
				!head_item(&cn, head, &it->key, &v))
				ok = false;
			else
			{
				/* (rows encode by the table as it is: as head has it, only) */
				describe_table(&cn, rel, &desc);
				k.data = (const uint8 *) it->key.data;
				k.len = it->key.len;
				hv.data = (const uint8 *) v.data;
				hv.len = v.len;
				(void) tree_item(&k, &hv, &data, &described);
				ok = described.len == (uint32) desc.len &&
					memcmp(described.data, desc.data, desc.len) == 0 &&
					PROLLY_CALL(cn.finder, fold_rows, rel, &cn.env, &data,
								changes[i].tids, changes[i].ntids, &rows);
			}
			if (ok)
			{
				put_hash(&it->value, &rows.hash);
				appendBinaryStringInfo(&it->value, desc.data, desc.len);
				pfree(desc.data);
				put_edit(edits, &nedits, it);
			}
			table_close(rel, AccessShareLock);
		}
		for (int i = 0; ok && i < nsequences; i++)
		{
			HeapTuple	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(sequences[i]));
			CanonItem  *it;
			Form_pg_class form;

			if (!HeapTupleIsValid(tup))
			{
				ok = false;
				break;
			}
			form = (Form_pg_class) GETSTRUCT(tup);
			describe_sequence(&cn, sequences[i], form->relnamespace,
							  NameStr(form->relname));
			ReleaseSysCache(tup);
			it = llast(cn.items);
			resetStringInfo(&v);
			if (!head_item(&cn, head, &it->key, &v))
				ok = false;
			else
			{
				put_edit(edits, &nedits, it);
			}
		}
	}
	PG_FINALLY();
	{
		PopActiveSnapshot();
		AtEOXact_GUC(false, save_nestlevel);
	}
	PG_END_TRY();
	if (!ok)
		return false;
	if (nedits > 1)
		qsort(edits, nedits, sizeof(ProllyEdit), edit_key_cmp);
	if (nedits > 0)
		PROLLY_CALL(cn.tree, apply, &cn.root_env, head, edits, nedits, root);
	else
		*root = *head;
	return true;
}

/* A statement, its trailing space and ";" trimmed, ended by one. */
static void
append_statement(StringInfo out, const char *stmt)
{
	int			len = strlen(stmt);

	while (len > 0 && (isspace((unsigned char) stmt[len - 1]) || stmt[len - 1] == ';'))
		len--;
	if (len == 0)
		return;
	if (out->len > 0)
		appendStringInfoChar(out, '\n');
	appendBinaryStringInfo(out, stmt, len);
	appendStringInfoChar(out, ';');
}

static void
append_statements(StringInfo out, List *stmts)
{
	ListCell   *lc;

	foreach(lc, stmts)
		append_statement(out, lfirst(lc));
}

/*
 * A table as made from nothing: its CREATE TABLE, then its columns' and
 * constraints' statements, its indexes, triggers, rules, statistics and
 * row security, its foreign keys, its policies and the tables it inherits
 * from: what a move to it runs (move_table(), rendering), each constraint
 * whole.
 */
static void
render_table(const char *name, const char *def, StringInfo out)
{
	TableDef	td;
	SchemaMove	sm;
	ListCell   *lc;

	parse_table_def(def, &td);
	memset(&sm, 0, sizeof(sm));
	sm.render = true;
	(void) move_table(&sm, name, NULL, &td);
	append_statements(out, sm.changes);
	append_statements(out, sm.after);
	append_statements(out, sm.foreign_keys);
	append_statements(out, td.policies);
	foreach(lc, td.inherits)
		append_statement(out, psprintf("ALTER TABLE %s INHERIT %s", name,
									   (char *) lfirst(lc)));
}

static void
folded_render(char object, const char *name, const ProllyValue *item,
			  StringInfo out)
{
	char	   *utf8 = pg_server_to_any(name, strlen(name), PG_UTF8);
	StringInfoData key;
	ProllyKey	k;
	ProllyValue d;
	char	   *def;

	initStringInfo(&key);
	appendStringInfoChar(&key, object);
	appendStringInfoChar(&key, ':');
	appendStringInfoString(&key, utf8);
	k.data = (const uint8 *) key.data;
	k.len = key.len;
	definition_of(&k, item, &d);
	def = from_utf8(d.data, d.len);
	switch (object)
	{
		case 't':
			render_table(name, def, out);
			break;
		case 'v':
			append_statement(out, psprintf(strncmp(def, "WITH (", 6) == 0 ?
										   "CREATE VIEW %s %s" : "CREATE VIEW %s AS %s",
										   name, def));
			break;
		case 'm':
			append_statements(out, make_matview_sql(name, def));
			break;
		case 'f':
			append_statement(out, def);
			break;
		case 'y':
			{
				List	   *unvalidated = NIL;

				append_statement(out, create_type_sql(name, def, &unvalidated));
				append_statements(out, unvalidated);
			}
			break;
		case 's':
			append_statement(out, psprintf("CREATE SEQUENCE %s %s", name,
										   sequence_options_sql(def)));
			break;
		case 'n':
			append_statement(out, psprintf("CREATE SCHEMA %s VERSIONED", name));
			break;
		case 'x':
			append_statement(out, create_cast_sql(name, def));
			break;
		case 'l':
			append_statement(out, create_collation_sql(name, def));
			break;
		default:
			elog(ERROR, "can't render an object of kind '%c'", object);
	}
	pfree(key.data);
}

/* Does key name one of tables (qualified, server encoding)? */
static bool
names_table(const ProllyKey *k, List *tables)
{
	char	   *name;
	ListCell   *lc;

	if (k->len < 2 || k->data[0] != 't' || k->data[1] != ':')
		return false;
	name = from_utf8(k->data + 2, k->len - 2);
	foreach(lc, tables)
		if (strcmp(lfirst(lc), name) == 0)
			return true;
	return false;
}

/* Does one of defs (tables' definitions) name the object a key names? */
static bool
defs_name(List *defs, const ProllyKey *k)
{
	char	   *name = from_utf8(k->data + 2, k->len - 2);
	ListCell   *lc;

	foreach(lc, defs)
		if (strstr(lfirst(lc), name) != NULL)
			return true;
	return false;
}

/* sequences_drawn()'s walk: string constants cast to regclass */
static bool
regclass_names(Node *node, List **names)
{
	if (node == NULL)
		return false;
	if (IsA(node, TypeCast))
	{
		TypeCast   *tc = (TypeCast *) node;

		if (tc->arg && IsA(tc->arg, A_Const) &&
			!((A_Const *) tc->arg)->isnull &&
			IsA(&((A_Const *) tc->arg)->val, String) &&
			strcmp(strVal(llast(tc->typeName->names)), "regclass") == 0)
			*names = lappend(*names, strVal(&((A_Const *) tc->arg)->val));
	}
	return raw_expression_tree_walker(node, regclass_names, names);
}

/*
 * The sequences table (qualified) draws from as def (its definition)
 * describes it: the relations its columns' defaults name as regclass
 * constants (nextval('s'::regclass)), parsed, each qualified as an item's
 * key names it (a bare name in the table's schema).  Not a name's text
 * found anywhere: a literal, or a longer name, isn't one.
 */
static List *
sequences_drawn(const char *table, const char *def)
{
	ErrorSaveContext escontext = {T_ErrorSaveContext};
	List	   *tparts = stringToQualifiedNameList(table, (Node *) &escontext);
	TableDef	td;
	List	   *seqs = NIL;
	ListCell   *lc;

	if (list_length(tparts) != 2)
		return NIL;
	parse_table_def(def, &td);
	foreach(lc, td.columns)
	{
		const char *d = strstr(lfirst(lc), "\tdefault ");
		List	   *raw;
		List	   *names = NIL;
		ListCell   *nc;

		if (d == NULL)
			continue;
		raw = raw_parser(psprintf("SELECT %s", first_field(d + 9)),
						 RAW_PARSE_DEFAULT);
		(void) regclass_names(linitial_node(ResTarget,
											castNode(SelectStmt,
													 linitial_node(RawStmt, raw)->stmt)->targetList)->val,
							  &names);
		foreach(nc, names)
		{
			List	   *parts = stringToQualifiedNameList(lfirst(nc),
														 (Node *) &escontext);

			if (list_length(parts) == 1)
				seqs = lappend(seqs, quote_qualified_identifier(strVal(linitial(tparts)),
																strVal(linitial(parts))));
			else if (list_length(parts) == 2)
				seqs = lappend(seqs, quote_qualified_identifier(strVal(linitial(parts)),
																strVal(lsecond(parts))));
		}
	}
	return seqs;
}

/* Is the object key names (its kind's tag, a colon, its name) one of names? */
static bool
key_named(const ProllyKey *k, List *names)
{
	char	   *name = from_utf8(k->data + 2, k->len - 2);
	ListCell   *lc;

	foreach(lc, names)
		if (strcmp(lfirst(lc), name) == 0)
			return true;
	return false;
}

/*
 * into, but for the tables named, as from has them, into *out: a merge of
 * the two roots' items in key order, each table named taken from from (or
 * left out where from hasn't it), with the sequences it names (as from
 * has them: its values' series goes with it), every other from into.  A
 * type the two hold differently that a table named (as either has it)
 * names is refused: other tables may use it as into has it.
 */
static void
folded_splice(const ProllyNodeRef *into, const ProllyNodeRef *from,
			  List *tables, ProllyNodeRef *out)
{
	Canon		cn;
	ProllyTreeCursor *cur[2];
	bool		more[2];
	ProllyKey	k[2];
	ProllyValue v[2];
	ProllyTreeBuilder *b;
	const ProllyNodeRef *roots[2] = {into, from};
	List	   *defs = NIL;		/* the tables' definitions, both sides' */
	List	   *seqs = NIL;		/* the sequences they draw from */
	List	   *refused = NIL;
	ListCell   *lc;

	memset(&cn, 0, sizeof(cn));
	canon_env(&cn);
	check_format(&cn, into, "spliced");
	check_format(&cn, from, "spliced");

	/* the tables' definitions first: what they name, both sides' */
	foreach(lc, tables)
	{
		StringInfoData key;

		initStringInfo(&key);
		appendStringInfoString(&key, "t:");
		append_utf8(&key, lfirst(lc));
		for (int i = 0; i < 2; i++)
		{
			StringInfoData item;
			ProllyKey	tk = {(const uint8 *) key.data, key.len};

			initStringInfo(&item);
			if (PROLLY_CALL(cn.tree, get, &cn.env, roots[i], &tk, &item))
			{
				ProllyValue iv = {(const uint8 *) item.data, item.len};
				ProllyValue def;

				definition_of(&tk, &iv, &def);
				defs = lappend(defs, pnstrdup((const char *) def.data, def.len));
				seqs = list_concat(seqs,
								   sequences_drawn(lfirst(lc),
												   from_utf8(def.data, def.len)));
			}
		}
	}

	cur[0] = PROLLY_CALL(cn.tree, cursor_open, &cn.env, into, CurrentMemoryContext);
	cur[1] = PROLLY_CALL(cn.tree, cursor_open, &cn.env, from, CurrentMemoryContext);
	for (int i = 0; i < 2; i++)
	{
		PROLLY_CALL(cn.tree, cursor_seek, cur[i], NULL, ForwardScanDirection);
		more[i] = PROLLY_CALL(cn.tree, cursor_next, cur[i], &k[i], &v[i]);
	}
	b = PROLLY_CALL(cn.tree, build_begin, &cn.env, CurrentMemoryContext);
	while (more[0] || more[1])
	{
		int			c = !more[0] ? 1 : !more[1] ? -1 :
			prolly_bytes_compare(&k[0], &k[1], NULL);
		const ProllyKey *key = c <= 0 ? &k[0] : &k[1];
		bool		has[2] = {c <= 0, c >= 0};
		bool		differ = !has[0] || !has[1] || v[0].len != v[1].len ||
			memcmp(v[0].data, v[1].data, v[0].len) != 0;
		int			take = has[0] ? 0 : -1;

		if (names_table(key, tables) ||
			(differ && key->len >= 2 && key->data[0] == 's' &&
			 key->data[1] == ':' && key_named(key, seqs)))
			take = has[1] ? 1 : -1;
		else if (differ && key->len >= 2 && key->data[0] == 'y' &&
				 key->data[1] == ':' && defs_name(defs, key))
			refused = lappend(refused, from_utf8(key->data + 2, key->len - 2));
		if (take >= 0)
			PROLLY_CALL(cn.tree, build_add, b, &k[take], &v[take]);
		for (int i = 0; i < 2; i++)
			if (has[i])
				more[i] = PROLLY_CALL(cn.tree, cursor_next, cur[i], &k[i], &v[i]);
		CHECK_FOR_INTERRUPTS();
	}
	for (int i = 0; i < 2; i++)
		PROLLY_CALL(cn.tree, cursor_close, cur[i]);
	if (refused != NIL)
	{
		StringInfoData names;

		initStringInfo(&names);
		foreach(lc, refused)
			appendStringInfo(&names, "%s%s", foreach_current_index(lc) ? ", " : "",
							 (char *) lfirst(lc));
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("the tables named use types the two versions define differently"),
				 errdetail("They use %s.", names.data),
				 errhint("Commit or reset the changes to those types first.")));
	}
	PROLLY_CALL(cn.tree, build_finish, b, out);
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
	.walk_changes = folded_walk_changes,
	.fold = folded_fold,
	.read_fold = folded_read_fold,
	.discard_undescribed = folded_discard_undescribed,
	.fold_rows = folded_fold_rows,
	.splice = folded_splice,
	.tables = folded_tables,
	.materialized_views = folded_materialized_views,
	.same_schema = folded_same_schema,
	.objects = folded_objects,
	.changes = folded_changes,
	.render = folded_render,
	.move_schema = folded_move_schema,
	.sequences = folded_sequences,
	.fold_dependencies = folded_fold_dependencies,
	.dependencies = folded_dependencies,
	.merge_schema = folded_merge_schema,
	.walk_baseline = folded_walk_baseline,
	.table_columns = folded_table_columns,
	.type_definitions = folded_type_definitions,
	.in_format = folded_in_format,
	.format = folded_format,
};
