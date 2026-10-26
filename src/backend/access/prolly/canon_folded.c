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
 *	"!:ordering " table -> row-layout fingerprint, newline, then the extensions
 *						   ordering its keys and their versions (text[])
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
 *	"r:" foreign table	-> its definition, a line each: its server, its
 *						   options, its columns (type, collation, NOT NULL,
 *						   default or generation), their options, its CHECK
 *						   constraints (describe_foreign_table()).  No rows:
 *						   they are the server's
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
 * Foreign tables' definitions are covered, not their rows (the server's).
 * Anything else in versioned schemas (window functions, foreign tables
 * that are partitions, inherit or have triggers or row security,
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
 * An extension whose opclass or support function orders a primary key,
 * including through container types, records an "ordering" dependency flag.
 * Clone and move checks require that exact version.  Historical reads check
 * each table's ordering metadata and re-sort under the installed comparator
 * when its definition and row types still match.  Ordinary uses
 * keep minimum-version semantics.  The baseline shape records the ordering
 * version too: updating the extension requires sorting the table afresh.
 * Keyless tables (including deferrable or unsupported primary keys) order
 * by content bytes, so their extension dependencies are not pinned for this.
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
#include "funcapi.h"
#include "nodes/makefuncs.h"
#include "nodes/miscnodes.h"
#include "nodes/nodeFuncs.h"
#include "catalog/pg_attribute.h"
#include "parser/parse_type.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parse_func.h"
#include "parser/parser.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/prolly_applier.h"
#include "access/prolly_builtins.h"
#include "access/prolly_config.h"
#include "access/prolly_datum.h"
#include "access/prolly_fold.h"
#include "access/prolly_keying.h"
#include "access/relation.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "access/transam.h"
#include "access/tableam.h"
#include "access/toast_compression.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/branch.h"
#include "catalog/branch_ops.h"
#include "catalog/pg_authid_d.h"
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
#include "access/prolly_extconfig.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_index.h"
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
#include "commands/tablecmds.h"
#include "commands/trigger.h"
#include "common/int.h"
#include "common/string.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "foreign/foreign.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "optimizer/optimizer.h"
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
#include "utils/injection_point.h"
#include "utils/acl.h"
#include "utils/catcache.h"
#include "utils/lsyscache.h"
#include "utils/pg_locale.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/plancache.h"
#include "utils/typcache.h"

typedef struct CanonItem
{
	StringInfoData key;
	StringInfoData value;
	Oid			relid;			/* a table's, materialized view's or
								 * sequence's, whose rows or value it
								 * holds; else InvalidOid */
} CanonItem;

typedef struct BaseTable BaseTable;

/*
 * The format a folded root is in: "22", whose rows may hold a long jsonb
 * document as a JSON tree (valenc_canonical.c); "21" held every long value
 * as a value tree of its bytes, and was the first whose definitions
 * (defaults, checks, generated columns, index expressions, views,
 * policies) are described with the settings values' text forms are
 * written under fixed (describing_settings()), so a constant in one prints
 * alike in any session; "20" printed them under the committer's
 * DateStyle, IntervalStyle, TimeZone and the rest, and was the first whose
 * text forms of values (a type without binary I/O's, or one its receive
 * refuses) are written with times in UTC, hex bytea and the C locale's
 * money, numbers and dates (prolly_datum.c); "19" had them in the committer's TimeZone, locale and
 * bytea_output, and was the first whose
 * dependencies record each extension's configuration rows (those pg_dump
 * dumps with it: prolly_extconfig.h); "18" didn't, and was the first whose commits record
 * their objects' identities beside it (folded_identify()), which a server
 * of an earlier format would read past, matching renamed objects by name; "17"
 * had none, and was the first with extension send values normalized by
 * binary receive/send; "16", per-table ordering metadata and
 * ordering dependency flags (not readable by older servers); "15", a foreign
 * table's definition a line each (describe_foreign_table()); "14" had it as CREATE FOREIGN
 * TABLE's tail, and was the first with a value of a type whose binary I/O
 * may change (an extension's) tagged with the form it is in, its send
 * format or its text form (prolly_datum.c); "13" had such values untagged;
 * "12", every chunk and hash pointer named by BLAKE3's first 20 bytes; "11" named them by SHA-256, and was the
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
 * its definitions, row keying or identities aren't this format's.
 */
#define FOLDED_FORMAT	"22"
#define ORDERING_PREFIX "!:ordering "

/*
 * The format versions of the encoders FOLDED_FORMAT's trees are written
 * with: the key encoder's, the value encoder's, the keying's and the JSON
 * trees' (which long jsonb documents are).  A tree's
 * bytes are theirs, so one changed alone would read trees of the format
 * as its own, with phantom changes in every status, diff and commit, and
 * spurious conflicts in merges.  Changing one means changing
 * FOLDED_FORMAT too, and these with it: a fold refuses to run till they
 * agree (folded_encoders_checked()).
 */
#define FOLDED_KEY_ENCODER		7
#define FOLDED_VALUE_ENCODER	8
#define FOLDED_KEYING			1
#define FOLDED_JSON_TREE		2

/*
 * What a canonical tree or baseline built here was folded at, for a later
 * fold that works out again only what changed since (fold_changed_only()):
 * the refs' mark, and this backend's catalog epoch then.  Valid only for a
 * fold of everything that left nothing out, made since the deparsing
 * settings were last changed.
 */
typedef struct FoldedAt
{
	bool		valid;
	ProllyFoldMark mark;
	uint64		catalog_epoch;
} FoldedAt;

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
	bool		cached;			/* descriptions from this backend's cache */
	const ProllyNodeRef *baseline;	/* the branch's baseline (NULL: none) */
	bool		partial;		/* only the objects that may have changed:
								 * the next baseline's entries too */
	bool		verifying;		/* the full fold a partial one is checked
								 * against: remembered as nothing */
	List	   *given;			/* (verifying) CanonItem *: the rows of the
								 * tables the partial one folded, taken
								 * from its items, not folded again */
	bool		marked;			/* a fold of everything, marked: */
	FoldedAt	at;				/* what it was folded at */
} Canon;

static void build_canonical(Canon *cn, ProllyNodeRef *root);
static void append_statistics(StringInfo out, Relation rel);
static char *printed_expr(Datum bin, Relation rel, Form_pg_attribute att,
						  Form_pg_type domain, ParseExprKind kind);
static char *constraint_text(HeapTuple tup, Relation rel, Form_pg_type domain);
static bool list_has_string(List *list, const char *s);

/*
 * A change finder's fold of a table, and its fold of a transaction's own
 * rows, each as one batch of encoding (prolly_datum_batch()): run again if
 * an extension's receive refused a value.
 */
typedef struct FinderFold
{
	const ProllyChangeFinderRoutine *finder;
	Relation	rel;
	const ProllyTreeEnv *env;
	const ProllyTableFold *baseline;
	ProllyFullFoldReason why;
	ProllyNodeRef *data;
	StringInfo	next;
	const ProllyNodeRef *data_in;
	ItemPointerData *tids;
	int			ntids;
	bool		folded;
} FinderFold;

static void
finder_fold_run(void *arg)
{
	FinderFold *f = arg;

	if (f->next)
		resetStringInfo(f->next);
	PROLLY_CALL(f->finder, fold, f->rel, f->env, f->baseline, f->why, f->data, f->next);
}

static void
finder_fold(const ProllyChangeFinderRoutine *finder, Relation rel,
			const ProllyTreeEnv *env, const ProllyTableFold *baseline,
			ProllyFullFoldReason why, ProllyNodeRef *data, StringInfo next)
{
	FinderFold	f = {.finder = finder, .rel = rel, .env = env, .baseline = baseline,
	.why = why, .data = data, .next = next};

	prolly_datum_batch(finder_fold_run, &f);
}

static void
finder_fold_rows_run(void *arg)
{
	FinderFold *f = arg;

	f->folded = PROLLY_CALL(f->finder, fold_rows, f->rel, f->env, f->data_in,
							f->tids, f->ntids, f->data);
}

static bool
finder_fold_rows(const ProllyChangeFinderRoutine *finder, Relation rel,
				 const ProllyTreeEnv *env, const ProllyNodeRef *data_in,
				 ItemPointerData *tids, int ntids, ProllyNodeRef *data)
{
	FinderFold	f = {.finder = finder, .rel = rel, .env = env, .data_in = data_in,
	.tids = tids, .ntids = ntids, .data = data};

	prolly_datum_batch(finder_fold_rows_run, &f);
	return f.folded;
}

void
prolly_folded_tree_env(ProllyTreeEnv *env)
{
	memset(env, 0, sizeof(*env));
	env->chunk_format = prolly_chunkformat_lookup("basic");
	env->chunker = prolly_chunker_lookup("bytes");
	env->hash = prolly_content_hash();
	/* (reading the chunks a step keeps in the backend, if it keeps any) */
	env->chunk_store = prolly_local_chunks_reading();
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

static bool split_base_value(const ProllyValue *v, ProllyHash *data,
							 ProllyHash *shape, ProllyValue *shape_ref,
							 ProllyValue *state);

/*
 * A baseline's items: the table's data tree, the value tree of its shape,
 * then what its finder's state names, as every change finder declares it
 * (the one that wrote it may not be the one configured now).
 */
static void
base_chunk_refs(const ProllyKey *key, const ProllyValue *value, void *arg,
				StringInfo refs)
{
	ProllyHash	data;
	ProllyHash	shape;
	ProllyValue shape_ref;
	ProllyValue state;
	static List *finders = NIL;

	declare_leading_hash(value, refs);
	if (!split_base_value(value, &data, &shape, &shape_ref, &state))
		return;
	if (shape_ref.len > 0)
		appendBinaryStringInfo(refs, (const char *) shape_ref.data, shape_ref.len);
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

/*
 * The encoders this build writes trees with are those FOLDED_FORMAT was
 * pinned to; checked once.
 */
static void
folded_encoders_checked(void)
{
	static bool checked = false;
	const ProllyImplInfo *impls[4];
	const int	pinned[4] = {FOLDED_KEY_ENCODER, FOLDED_VALUE_ENCODER, FOLDED_KEYING,
	FOLDED_JSON_TREE};

	if (checked)
		return;
	impls[0] = prolly_get_impl(PROLLY_IFACE_KEY_ENCODER, "ordered");
	impls[1] = prolly_get_impl(PROLLY_IFACE_VALUE_ENCODER, "canonical");
	impls[2] = prolly_get_impl(PROLLY_IFACE_TABLE_KEYING, "pk_or_prefix");
	impls[3] = prolly_get_impl(PROLLY_IFACE_JSON_TREE, "paths");
	for (int i = 0; i < lengthof(impls); i++)
		if (impls[i] && impls[i]->format_version != pinned[i])
			elog(ERROR, "prolly %s \"%s\" is at format version %u, but folded format %s pins %d: change FOLDED_FORMAT with it",
				 prolly_interface_name(impls[i]->iface), impls[i]->name,
				 impls[i]->format_version, FOLDED_FORMAT, pinned[i]);
	checked = true;
}

static void
canon_env(Canon *cn)
{
	folded_encoders_checked();
	cn->tree = prolly_tree_lookup("cow");
	prolly_folded_tree_env(&cn->env);
	prolly_folded_canon_env(&cn->root_env);

	/* (a step that only compares keeps what it builds in the backend) */
	if (prolly_local_chunks_active())
	{
		cn->env.chunk_store = prolly_local_chunks_keeping();
		cn->root_env.chunk_store = prolly_local_chunks_keeping();
	}
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
	it->relid = InvalidOid;
	cn->items = lappend(cn->items, it);
	pgstat_count_prolly(PGSTAT_PROLLY_FOLD_OBJECTS, 1);
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

/* A routine's item name: qualified, with its identity arguments. */
static char *
routine_item_name(Form_pg_proc proc)
{
	return psprintf("%s(%s)",
					qualified(proc->pronamespace, NameStr(proc->proname)),
					TextDatumGetCString(DirectFunctionCall1(pg_get_function_identity_arguments,
															ObjectIdGetDatum(proc->oid))));
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

/* ------------------------------------------------------------------------
 * Foreign tables
 *
 * A foreign table's definition, a line each (fields tab-separated, escaped):
 * "server", its server; "option", each of its options, by name; "column",
 * each live column in order: its name and type (quoted, qualified), its
 * collation if not its type's, "t" if NOT NULL, and "d", "s" or "v" with
 * its default or generation expression (stored, virtual) as it prints read
 * back; "colopt", each column's options, by name; "check", each CHECK
 * constraint, by name, as it prints read back.  Option names are unique per
 * object, so ordered by them the lines don't depend on the order options
 * were given in.
 * ------------------------------------------------------------------------
 */

typedef struct ForeignColumn
{
	char	   *name;			/* quoted */
	char	   *type;
	char	   *collation;		/* "" if the type's */
	bool		notnull;
	char		defkind;		/* '\0', 'd', 's' or 'v' */
	char	   *expr;
	List	   *options;		/* "name\tvalue", by name */
} ForeignColumn;

typedef struct ForeignDef
{
	char	   *server;			/* quoted */
	List	   *options;		/* "name\tvalue", by name */
	List	   *columns;		/* ForeignColumn * */
	List	   *checks;			/* "name\tdefinition", by name (quoted) */
} ForeignDef;

/* an object's options (DefElem *), as "option" or "colopt" lines, by name */
static void
append_fdw_option_lines(StringInfo out, const char *kind, const char *column,
						List *options)
{
	List	   *lines = NIL;
	ListCell   *lc;

	foreach(lc, options)
	{
		DefElem    *d = lfirst(lc);
		StringInfoData line;

		initStringInfo(&line);
		appendStringInfoString(&line, kind);
		if (column)
		{
			appendStringInfoChar(&line, '\t');
			append_field(&line, column);
		}
		appendStringInfoChar(&line, '\t');
		append_field(&line, d->defname);
		appendStringInfoChar(&line, '\t');
		append_field(&line, defGetString(d));
		appendStringInfoChar(&line, '\n');
		lines = lappend(lines, line.data);
	}
	list_sort(lines, cstring_cmp);
	foreach(lc, lines)
		appendStringInfoString(out, lfirst(lc));
}

/* the definition of foreign table relid, as above, appended to out */
static void
describe_foreign_table(Oid relid, StringInfo out)
{
	Relation	rel = relation_open(relid, AccessShareLock);
	TupleDesc	desc = RelationGetDescr(rel);
	ForeignTable *ft = GetForeignTable(relid);
	ForeignServer *server = GetForeignServer(ft->serverid);
	StringInfoData colopts;
	Relation	conrel;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *checks = NIL;
	ListCell   *lc;

	appendStringInfoString(out, "server\t");
	append_field(out, quote_identifier(server->servername));
	appendStringInfoChar(out, '\n');
	append_fdw_option_lines(out, "option", NULL, ft->options);
	initStringInfo(&colopts);
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);
		const char *colname;

		if (att->attisdropped)
			continue;
		colname = quote_identifier(NameStr(att->attname));
		appendStringInfoString(out, "column\t");
		append_field(out, colname);
		appendStringInfoChar(out, '\t');
		append_field(out, format_type_extended(att->atttypid, att->atttypmod,
											   FORMAT_TYPE_TYPEMOD_GIVEN |
											   FORMAT_TYPE_FORCE_QUALIFY));
		appendStringInfoChar(out, '\t');
		if (OidIsValid(att->attcollation) &&
			att->attcollation != get_typcollation(att->atttypid))
		{
			HeapTuple	ctup = SearchSysCache1(COLLOID, ObjectIdGetDatum(att->attcollation));
			Form_pg_collation c;

			if (!HeapTupleIsValid(ctup))
				elog(ERROR, "cache lookup failed for collation %u", att->attcollation);
			c = (Form_pg_collation) GETSTRUCT(ctup);
			append_field(out, qualified(c->collnamespace, NameStr(c->collname)));
			ReleaseSysCache(ctup);
		}
		appendStringInfo(out, "\t%s\t", att->attnotnull ? "t" : "f");
		if (att->atthasdef && desc->constr)
		{
			char	   *expr = NULL;

			for (int d = 0; d < desc->constr->num_defval; d++)
				if (desc->constr->defval[d].adnum == att->attnum)
					expr = printed_expr(CStringGetTextDatum(desc->constr->defval[d].adbin),
										rel, att, NULL,
										att->attgenerated ? EXPR_KIND_GENERATED_COLUMN :
										EXPR_KIND_COLUMN_DEFAULT);
			if (expr == NULL)
				elog(ERROR, "default of column \"%s\" not found", NameStr(att->attname));
			appendStringInfoChar(out, att->attgenerated == ATTRIBUTE_GENERATED_STORED ? 's' :
								 att->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL ? 'v' : 'd');
			appendStringInfoChar(out, '\t');
			append_field(out, expr);
		}
		else
			appendStringInfoChar(out, '\t');
		appendStringInfoChar(out, '\n');
		append_fdw_option_lines(&colopts, "colopt", colname,
								GetForeignColumnOptions(relid, att->attnum));
	}
	appendStringInfoString(out, colopts.data);

	/* its CHECK constraints, by name */
	conrel = table_open(ConstraintRelationId, AccessShareLock);
	ScanKeyInit(&key, Anum_pg_constraint_conrelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(relid));
	scan = systable_beginscan(conrel, ConstraintRelidTypidNameIndexId, true,
							  NULL, 1, &key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tup);
		StringInfoData line;

		if (con->contype != CONSTRAINT_CHECK)
			continue;
		initStringInfo(&line);
		appendStringInfoString(&line, "check\t");
		append_field(&line, quote_identifier(NameStr(con->conname)));
		appendStringInfoChar(&line, '\t');
		append_field(&line, constraint_text(tup, rel, NULL));
		appendStringInfoChar(&line, '\n');
		checks = lappend(checks, line.data);
	}
	systable_endscan(scan);
	table_close(conrel, AccessShareLock);
	list_sort(checks, cstring_cmp);
	foreach(lc, checks)
		appendStringInfoString(out, lfirst(lc));
	relation_close(rel, AccessShareLock);
}

/*
 * What of a foreign table a commit doesn't describe yet, or NULL: extended
 * statistics on it, and columns' statistics targets, storage and options.
 */
static const char *
foreign_table_undescribed(Oid relid)
{
	Relation	rel;
	ScanKeyData key;
	SysScanDesc scan;
	bool		any;
	TupleDesc	desc;

	rel = table_open(StatisticExtRelationId, AccessShareLock);
	ScanKeyInit(&key, Anum_pg_statistic_ext_stxrelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(relid));
	scan = systable_beginscan(rel, StatisticExtRelidIndexId, true, NULL, 1, &key);
	any = HeapTupleIsValid(systable_getnext(scan));
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	if (any)
		return "extended statistics on foreign tables";

	rel = relation_open(relid, AccessShareLock);
	desc = RelationGetDescr(rel);
	for (int i = 0; i < desc->natts && !any; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);
		HeapTuple	atup;
		bool		isnull;

		if (att->attisdropped)
			continue;
		if (att->attstorage != get_typstorage(att->atttypid))
			any = true;
		atup = SearchSysCache2(ATTNUM, ObjectIdGetDatum(relid), Int16GetDatum(att->attnum));
		if (HeapTupleIsValid(atup))
		{
			(void) SysCacheGetAttr(ATTNUM, atup, Anum_pg_attribute_attstattarget, &isnull);
			if (!isnull)
				any = true;
			(void) SysCacheGetAttr(ATTNUM, atup, Anum_pg_attribute_attoptions, &isnull);
			if (!isnull)
				any = true;
			ReleaseSysCache(atup);
		}
	}
	relation_close(rel, AccessShareLock);
	return any ? "column statistics targets, storage or options on foreign tables" : NULL;
}

/*
 * Has relation relid a trigger?  (relhastriggers stays set once one is
 * dropped, until a VACUUM.)
 */
static bool
has_triggers(Oid relid)
{
	Relation	rel = table_open(TriggerRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	bool		any;

	ScanKeyInit(&key, Anum_pg_trigger_tgrelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(relid));
	scan = systable_beginscan(rel, TriggerRelidNameIndexId, true, NULL, 1, &key);
	any = HeapTupleIsValid(systable_getnext(scan));
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	return any;
}

/* the fields of one line of a foreign table's definition */
static List *
foreign_fields(char *line)
{
	List	   *fields = NIL;
	char	   *f = line;

	for (;;)
	{
		char	   *tab = strchr(f, '\t');

		if (tab)
			*tab = '\0';
		fields = lappend(fields, unescape_field(f));
		if (!tab)
			break;
		f = tab + 1;
	}
	return fields;
}

static void
parse_foreign_def(const char *def, ForeignDef *fd)
{
	char	   *copy = pstrdup(def);
	char	   *line;
	char	   *next;

	memset(fd, 0, sizeof(*fd));
	for (line = copy; line && *line; line = next)
	{
		List	   *f;
		const char *kind;

		next = strchr(line, '\n');
		if (next)
			*next++ = '\0';
		f = foreign_fields(line);
		kind = linitial(f);
		if (strcmp(kind, "server") == 0 && list_length(f) == 2)
			fd->server = lsecond(f);
		else if (strcmp(kind, "option") == 0 && list_length(f) == 3)
			fd->options = lappend(fd->options, psprintf("%s\t%s", (char *) lsecond(f),
														(char *) lthird(f)));
		else if (strcmp(kind, "column") == 0 && list_length(f) == 7)
		{
			ForeignColumn *c = palloc0(sizeof(ForeignColumn));

			c->name = lsecond(f);
			c->type = lthird(f);
			c->collation = lfourth(f);
			c->notnull = strcmp(list_nth(f, 4), "t") == 0;
			c->defkind = ((char *) list_nth(f, 5))[0];
			c->expr = list_nth(f, 6);
			fd->columns = lappend(fd->columns, c);
		}
		else if (strcmp(kind, "colopt") == 0 && list_length(f) == 4)
		{
			ListCell   *lc;

			foreach(lc, fd->columns)
			{
				ForeignColumn *c = lfirst(lc);

				if (strcmp(c->name, lsecond(f)) == 0)
					c->options = lappend(c->options, psprintf("%s\t%s", (char *) lthird(f),
															  (char *) lfourth(f)));
			}
		}
		else if (strcmp(kind, "check") == 0 && list_length(f) == 3)
			fd->checks = lappend(fd->checks, psprintf("%s\t%s", (char *) lsecond(f),
													  (char *) lthird(f)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid foreign table definition line \"%s\"", line)));
	}
	if (fd->server == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("foreign table definition without its server")));
}

/* "name\tvalue" lines as OPTIONS (name 'value', ...), if any */
static void
append_options_sql(StringInfo out, List *options)
{
	ListCell   *lc;

	if (options == NIL)
		return;
	appendStringInfoString(out, " OPTIONS (");
	foreach(lc, options)
	{
		char	   *o = pstrdup(lfirst(lc));
		char	   *tab = strchr(o, '\t');

		*tab = '\0';
		appendStringInfo(out, "%s%s %s", foreach_current_index(lc) > 0 ? ", " : "",
						 quote_identifier(o), quote_literal_cstr(tab + 1));
	}
	appendStringInfoChar(out, ')');
}

/* the statement making foreign table name (quoted, qualified) as def has it */
static char *
create_foreign_table_sql(const char *name, const char *def)
{
	ForeignDef	fd;
	StringInfoData out;
	ListCell   *lc;
	bool		first = true;

	parse_foreign_def(def, &fd);
	initStringInfo(&out);
	appendStringInfo(&out, "CREATE FOREIGN TABLE %s (", name);
	foreach(lc, fd.columns)
	{
		ForeignColumn *c = lfirst(lc);

		appendStringInfo(&out, "%s%s %s", first ? "" : ", ", c->name, c->type);
		first = false;
		append_options_sql(&out, c->options);
		if (*c->collation)
			appendStringInfo(&out, " COLLATE %s", c->collation);
		if (c->notnull)
			appendStringInfoString(&out, " NOT NULL");
		if (c->defkind == 'd')
			appendStringInfo(&out, " DEFAULT %s", c->expr);
		else if (c->defkind == 's' || c->defkind == 'v')
			appendStringInfo(&out, " GENERATED ALWAYS AS (%s) %s", c->expr,
							 c->defkind == 's' ? "STORED" : "VIRTUAL");
	}
	foreach(lc, fd.checks)
	{
		char	   *k = pstrdup(lfirst(lc));
		char	   *tab = strchr(k, '\t');

		*tab = '\0';
		appendStringInfo(&out, "%sCONSTRAINT %s %s", first ? "" : ", ", k, tab + 1);
		first = false;
	}
	appendStringInfo(&out, ") SERVER %s", fd.server);
	append_options_sql(&out, fd.options);
	return out.data;
}

/* an OPTIONS (ADD/SET/DROP ...) clause from options from to to, or NULL */
static char *
alter_options_sql(List *from, List *to)
{
	StringInfoData out;
	ListCell   *lc;
	bool		any = false;

	initStringInfo(&out);
	appendStringInfoString(&out, " OPTIONS (");
#define OPTION_NAME(o) pnstrdup((o), strchr((o), '\t') - (o))
	foreach(lc, from)
	{
		char	   *n = OPTION_NAME((char *) lfirst(lc));
		bool		kept = false;
		ListCell   *lt;

		foreach(lt, to)
			if (strcmp(OPTION_NAME((char *) lfirst(lt)), n) == 0)
				kept = true;
		if (!kept)
		{
			appendStringInfo(&out, "%sDROP %s", any ? ", " : "", quote_identifier(n));
			any = true;
		}
	}
	foreach(lc, to)
	{
		char	   *o = lfirst(lc);
		char	   *n = OPTION_NAME(o);
		const char *had = NULL;
		ListCell   *lf;

		foreach(lf, from)
			if (strcmp(OPTION_NAME((char *) lfirst(lf)), n) == 0)
				had = lfirst(lf);
		if (had && strcmp(had, o) == 0)
			continue;
		appendStringInfo(&out, "%s%s %s %s", any ? ", " : "", had ? "SET" : "ADD",
						 quote_identifier(n), quote_literal_cstr(strchr(o, '\t') + 1));
		any = true;
	}
#undef OPTION_NAME
	appendStringInfoChar(&out, ')');
	return any ? out.data : NULL;
}

/*
 * The statements changing foreign table name from def from to def to in
 * place: its options, its columns' options, NOT NULL and defaults, its
 * checks.  NIL with *remake set if it must be made again instead (another
 * server; columns added, dropped, reordered or of another type, collation
 * or generation).
 */
static List *
alter_foreign_table_sql(const char *name, const char *from, const char *to,
						bool *remake)
{
	ForeignDef	f;
	ForeignDef	t;
	List	   *stmts = NIL;
	char	   *opts;
	ListCell   *lf;
	ListCell   *lt;

	parse_foreign_def(from, &f);
	parse_foreign_def(to, &t);
	*remake = strcmp(f.server, t.server) != 0 ||
		list_length(f.columns) != list_length(t.columns);
	forboth(lf, f.columns, lt, t.columns)
	{
		ForeignColumn *fc = lfirst(lf);
		ForeignColumn *tc = lfirst(lt);

		if (*remake)
			break;
		if (strcmp(fc->name, tc->name) != 0 || strcmp(fc->type, tc->type) != 0 ||
			strcmp(fc->collation, tc->collation) != 0 ||
			((fc->defkind == 's' || fc->defkind == 'v' ||
			  tc->defkind == 's' || tc->defkind == 'v') &&
			 (fc->defkind != tc->defkind || strcmp(fc->expr, tc->expr) != 0)))
			*remake = true;
	}
	if (*remake)
		return NIL;

	if ((opts = alter_options_sql(f.options, t.options)) != NULL)
		stmts = lappend(stmts, psprintf("ALTER FOREIGN TABLE %s%s", name, opts));
	forboth(lf, f.columns, lt, t.columns)
	{
		ForeignColumn *fc = lfirst(lf);
		ForeignColumn *tc = lfirst(lt);

		if ((opts = alter_options_sql(fc->options, tc->options)) != NULL)
			stmts = lappend(stmts, psprintf("ALTER FOREIGN TABLE %s ALTER COLUMN %s%s",
											name, tc->name, opts));
		if (fc->notnull != tc->notnull)
			stmts = lappend(stmts, psprintf("ALTER FOREIGN TABLE %s ALTER COLUMN %s %s NOT NULL",
											name, tc->name, tc->notnull ? "SET" : "DROP"));
		if (fc->defkind != tc->defkind || strcmp(fc->expr, tc->expr) != 0)
			stmts = lappend(stmts, tc->defkind == 'd' ?
							psprintf("ALTER FOREIGN TABLE %s ALTER COLUMN %s SET DEFAULT %s",
									 name, tc->name, tc->expr) :
							psprintf("ALTER FOREIGN TABLE %s ALTER COLUMN %s DROP DEFAULT",
									 name, tc->name));
	}
	foreach(lf, f.checks)
		if (!list_has_string(t.checks, lfirst(lf)))
		{
			char	   *k = pstrdup(lfirst(lf));

			*strchr(k, '\t') = '\0';
			stmts = lcons(psprintf("ALTER FOREIGN TABLE %s DROP CONSTRAINT IF EXISTS %s",
								   name, k), stmts);
		}
	foreach(lt, t.checks)
		if (!list_has_string(f.checks, lfirst(lt)))
		{
			char	   *k = pstrdup(lfirst(lt));
			char	   *tab = strchr(k, '\t');

			*tab = '\0';
			stmts = lappend(stmts, psprintf("ALTER FOREIGN TABLE %s ADD CONSTRAINT %s %s",
											name, k, tab + 1));
		}
	return stmts;
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
	ProllyHash	shape_hash;		/* of what its rows were folded under
								 * (baseline_shape()); empty if not known */
	StringInfoData shape_ref;	/* the value tree holding that */
	StringInfoData shape;		/* and its bytes, once read (base_shape()) */
	bool		shape_read;
	StringInfoData state;		/* the change finder's */
};

/*
 * A baseline's value: the table's data tree (a length byte, then its
 * hash); what its rows were folded under, BASE_SHAPED, BASE_VERSION and a
 * length byte, then the content hash of baseline_shape()'s bytes and the
 * reference of a value tree holding them (each a length byte, then it);
 * then the finder's state.  The bytes are out of line: every item is written again at each
 * fold that begins a baseline (the finder's state changes), the shape
 * seldom, so it is written once, as it changes, and its hash compared.  (A
 * build before kept only a hash of the table's shape, as a length byte
 * then the hash: it names no shape, and its table is read in full once.)
 */
#define BASE_SHAPED		0xFF	/* (no hash is as long) */
#define BASE_VERSION	1

static bool
split_base_value(const ProllyValue *v, ProllyHash *data, ProllyHash *shape,
				 ProllyValue *shape_ref, ProllyValue *state)
{
	const uint8 *p = v->data;
	const uint8 *end = v->data + v->len;

	memset(data, 0, sizeof(*data));
	memset(shape, 0, sizeof(*shape));
	shape_ref->data = NULL;
	shape_ref->len = 0;
	if (p >= end || p[0] > PROLLY_HASH_MAX_LEN || end - p < 1 + p[0] + 1)
		return false;
	data->len = p[0];
	memcpy(data->bytes, p + 1, p[0]);
	p += 1 + p[0];
	if (p[0] == BASE_SHAPED)
	{
		const uint8 *f;
		const uint8 *fend;

		if (end - p < 3 || end - p < 3 + p[2])
			return false;
		f = p + 3;
		fend = f + p[2];
		/* (another version's: no shape known) */
		if (p[1] == BASE_VERSION)
		{
			if (f >= fend || f[0] > PROLLY_HASH_MAX_LEN || fend - f < 1 + f[0] + 1)
				return false;
			shape->len = f[0];
			memcpy(shape->bytes, f + 1, f[0]);
			f += 1 + f[0];
			if (fend - f != 1 + f[0])
				return false;
			shape_ref->data = f + 1;
			shape_ref->len = f[0];
		}
		p = fend;
	}
	else
	{
		if (p[0] > PROLLY_HASH_MAX_LEN || end - p < 1 + p[0])
			return false;
		p += 1 + p[0];
	}
	state->data = p;
	state->len = end - p;
	return true;
}

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

static bool
parse_base_table(const ProllyValue *v, BaseTable *bt)
{
	ProllyValue ref;
	ProllyValue state;

	memset(&bt->data, 0, sizeof(bt->data));
	if (!split_base_value(v, &bt->data.hash, &bt->shape_hash, &ref, &state))
		return false;
	initStringInfo(&bt->shape_ref);
	if (ref.len > 0)
		appendBinaryStringInfo(&bt->shape_ref, (const char *) ref.data, ref.len);
	initStringInfo(&bt->shape);
	bt->shape_read = false;
	initStringInfo(&bt->state);
	appendBinaryStringInfo(&bt->state, (const char *) state.data, state.len);
	return true;
}

/* bt's shape's bytes (empty if not known, or its tree is gone). */
static const StringInfoData *
base_shape(Canon *cn, BaseTable *bt)
{
	ProllyValue ref = {(const uint8 *) bt->shape_ref.data, bt->shape_ref.len};

	if (!bt->shape_read && ref.len > 0 &&
		PROLLY_CALL(cn->tree, value_present, &cn->env, &ref))
		PROLLY_CALL(cn->tree, value_read, &cn->env, &ref, 0, PG_UINT64_MAX,
					&bt->shape);
	bt->shape_read = true;
	return &bt->shape;
}

/*
 * The shape (hash h, bytes shape) of a next baseline's item: found's tree
 * of it if it has one, else one written.
 */
static void
put_base_shape(Canon *cn, StringInfo out, const ProllyHash *h,
			   const StringInfoData *shape, const BaseTable *found)
{
	StringInfoData ref;

	initStringInfo(&ref);
	if (found && found->shape_ref.len > 0 &&
		found->shape_hash.len == h->len &&
		memcmp(found->shape_hash.bytes, h->bytes, h->len) == 0)
		appendBinaryStringInfo(&ref, found->shape_ref.data, found->shape_ref.len);
	else
		PROLLY_CALL(cn->tree, value_write, &cn->env, (const uint8 *) shape->data,
					shape->len, &ref);
	Assert(1 + h->len + 1 + ref.len <= PG_UINT8_MAX);
	appendStringInfoChar(out, (char) BASE_SHAPED);
	appendStringInfoChar(out, BASE_VERSION);
	appendStringInfoChar(out, (char) (1 + h->len + 1 + ref.len));
	put_hash(out, h);
	appendStringInfoChar(out, (char) ref.len);
	appendBinaryStringInfo(out, ref.data, ref.len);
	pfree(ref.data);
}

/*
 * bt's shape as a baseline's value holds it, as parse_base_table() read it:
 * its hash and tree (put_base_shape()), or, not known, an empty hash (its
 * table is read in full once).
 */
static void
put_base_kept_shape(StringInfo out, const BaseTable *bt)
{
	ProllyHash	none = {0};

	if (bt->shape_hash.len == 0 || bt->shape_ref.len == 0)
	{
		put_hash(out, &none);
		return;
	}
	Assert(1 + bt->shape_hash.len + 1 + bt->shape_ref.len <= PG_UINT8_MAX);
	appendStringInfoChar(out, (char) BASE_SHAPED);
	appendStringInfoChar(out, BASE_VERSION);
	appendStringInfoChar(out, (char) (1 + bt->shape_hash.len + 1 + bt->shape_ref.len));
	put_hash(out, &bt->shape_hash);
	appendStringInfoChar(out, (char) bt->shape_ref.len);
	appendBinaryStringInfo(out, bt->shape_ref.data, bt->shape_ref.len);
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

static Oid extension_of(Oid classid, Oid objid);
static char *extension_version(Oid extoid);
static char *text_array_literal(List *strings);

/* Both an opclass and its comparator can belong to extensions. */
static List *
ordering_opclass(List *out, Oid opclass)
{
	Oid			ext;
	Oid			type;
	Oid			proc;

	if (!OidIsValid(opclass))
		return out;
	ext = extension_of(OperatorClassRelationId, opclass);
	if (OidIsValid(ext))
		out = list_append_unique_oid(out, ext);
	type = get_opclass_input_type(opclass);
	proc = get_opfamily_proc(get_opclass_family(opclass), type, type,
							 BTORDER_PROC);
	ext = extension_of(ProcedureRelationId, proc);
	if (OidIsValid(ext))
		out = list_append_unique_oid(out, ext);
	return out;
}

/* Core container comparators delegate to the contained types' comparators. */
static List *
ordering_type(List *out, Oid type, List *seen)
{
	Oid			nested;
	TypeCacheEntry *tc;

	check_stack_depth();
	if (list_member_oid(seen, type))
		return out;
	seen = lappend_oid(list_copy(seen), type);
	out = ordering_opclass(out, GetDefaultOpClass(type, BTREE_AM_OID));
	nested = getBaseType(type);
	if (nested != type)
		return ordering_type(out, nested, seen);
	nested = get_element_type(type);
	if (OidIsValid(nested))
		return ordering_type(out, nested, seen);
	if (type_is_multirange(type))
		return ordering_type(out, get_multirange_range(type), seen);
	if (type_is_range(type))
	{
		Oid			ext;

		tc = lookup_type_cache(type, TYPECACHE_RANGE_INFO);
		ext = extension_of(ProcedureRelationId, tc->rng_cmp_proc_finfo.fn_oid);
		if (OidIsValid(ext))
			out = list_append_unique_oid(out, ext);
		return ordering_type(out, tc->rngelemtype->type_id, seen);
	}
	if (type_is_rowtype(type))
	{
		tc = lookup_type_cache(type, TYPECACHE_TUPDESC);
		if (tc->tupDesc)
			for (int i = 0; i < tc->tupDesc->natts; i++)
			{
				Form_pg_attribute att = TupleDescAttr(tc->tupDesc, i);

				if (!att->attisdropped)
					out = ordering_type(out, att->atttypid, seen);
			}
	}
	return out;
}

/* Extensions whose comparators actually order this table's prolly keys. */
static List *
ordering_extensions(Relation rel)
{
	Oid			pkoid = RelationGetPrimaryKeyIndex(rel, false);
	ProllyComponents c;
	ProllyKeying *k;
	Relation	pk;
	List	   *out = NIL;
	bool		keyless;

	if (!OidIsValid(pkoid))
		return NIL;
	prolly_components_for_relation(rel, &c);
	k = PROLLY_CALL(c.keying, open, rel, CurrentMemoryContext);
	keyless = PROLLY_CALL(c.keying, keyless, k);
	PROLLY_CALL(c.keying, close, k);
	if (keyless)
		return NIL;
	pk = index_open(pkoid, AccessShareLock);
	for (int i = 0; i < IndexRelationGetNumberOfKeyAttributes(pk); i++)
	{
		out = ordering_opclass(out, get_index_column_opclass(pkoid, i + 1));
		out = ordering_type(out,
							TupleDescAttr(RelationGetDescr(rel),
										  pk->rd_index->indkey.values[i] - 1)->atttypid, NIL);
	}
	index_close(pk, AccessShareLock);
	return out;
}

/* Stable across databases: no catalog OIDs in the baseline shape. */
static char *
ordering_versions(Relation rel)
{
	List	   *extensions = ordering_extensions(rel);
	List	   *versions = NIL;
	ListCell   *lc;

	foreach(lc, extensions)
	{
		Oid			ext = lfirst_oid(lc);

		versions = lappend(versions, get_extension_name(ext));
		versions = lappend(versions, extension_version(ext));
	}
	return versions ? text_array_literal(versions) : NULL;
}

static void describe_type(Canon *cn, Form_pg_type typ);

/*
 * The settings definitions are described under, and their descriptions
 * run under when a move makes them again, set at the caller's GUC nest
 * level: search_path pg_catalog, so each name prints qualified;
 * quote_all_identifiers off, so names are quoted alike (string constants
 * always print standard conforming); and the settings values' text
 * forms are written under (TimeZone UTC, ISO dates, the postgres
 * IntervalStyle, and the rest: prolly_datum.h), so a constant in a
 * default, a check, an index's expression or a view prints, and reads
 * back, alike in any session.  No other setting changes how a description
 * prints, so the descriptions cached (described) hold in any session.
 */
static void
describing_settings(void)
{
	(void) set_config_option("search_path", "pg_catalog", PGC_USERSET,
							 PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	if (quote_all_identifiers)
		(void) set_config_option("quote_all_identifiers", "off", PGC_USERSET,
								 PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	prolly_datum_fix_text_settings();
}

/* Type definitions in canonical names and labels, never database-local OIDs. */
static void
ordering_type_layout(StringInfo out, Oid type, List *seen)
{
	HeapTuple	tup;
	Form_pg_type typ;
	Canon		cn = {0};
	ListCell   *lc;
	Oid			nested;

	check_stack_depth();
	append_field(out, format_type_be_qualified(type));
	appendStringInfoChar(out, '\n');
	if (list_member_oid(seen, type))
		return;
	seen = lappend_oid(list_copy(seen), type);
	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", type);
	typ = (Form_pg_type) GETSTRUCT(tup);
	describe_type(&cn, typ);
	foreach(lc, cn.items)
	{
		CanonItem  *it = lfirst(lc);

		appendBinaryStringInfo(out, it->value.data, it->value.len);
	}
	nested = getBaseType(type);
	if (nested != type)
		ordering_type_layout(out, nested, seen);
	else if (OidIsValid(nested = get_element_type(type)))
		ordering_type_layout(out, nested, seen);
	else if (typ->typtype == TYPTYPE_COMPOSITE)
	{
		TupleDesc	desc = lookup_rowtype_tupdesc(type, -1);

		for (int i = 0; i < desc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(desc, i);

			appendStringInfoChar(out, att->attisdropped ? 'd' : 'a');
			if (!att->attisdropped)
				ordering_type_layout(out, att->atttypid, seen);
		}
		ReleaseTupleDesc(desc);
	}
	else if (typ->typtype == TYPTYPE_RANGE)
		ordering_type_layout(out, get_range_subtype(type), seen);
	else if (typ->typtype == TYPTYPE_MULTIRANGE)
		ordering_type_layout(out, get_multirange_range(type), seen);
	ReleaseSysCache(tup);
}

/* A portable description of the rows whose encoded keys we can re-sort. */
static char *
ordering_layout(Canon *cn, Relation rel, const StringInfoData *description)
{
	StringInfoData shape;
	ProllyHash	hash;
	TupleDesc	desc = RelationGetDescr(rel);

	initStringInfo(&shape);
	appendBinaryStringInfo(&shape, description->data, description->len);
	for (int i = 0; i < desc->natts; i++)
		if (!TupleDescAttr(desc, i)->attisdropped)
			ordering_type_layout(&shape, TupleDescAttr(desc, i)->atttypid, NIL);
	content_hash(cn, shape.data, shape.len, &hash);
	pfree(shape.data);
	return prolly_hash_hex(&hash);
}

static void
record_ordering(Canon *cn, Relation rel, const StringInfoData *description)
{
	char	   *versions = ordering_versions(rel);

	if (versions)
	{
		CanonItem  *it = add_item(cn, ORDERING_PREFIX,
								  qualified(RelationGetNamespace(rel), RelationGetRelationName(rel)));

		appendStringInfo(&it->value, "%s\n", ordering_layout(cn, rel, description));
		append_utf8(&it->value, versions);
	}
}

/*
 * What a baseline's tree is kept for, appended to shape: what its rows'
 * keys and canonical bytes depend on, so DDL that changes no row's bytes
 * (an index, a constraint, a trigger) keeps the baseline; and what they
 * depend on that changes no page (so no change the finder would see).
 * The keying's row shape where it gives one ('S', its length, then it):
 * the columns rows are read over, by number (a column dropped and one of
 * the same name and type added describe the table alike, but its rows read
 * as NULL), their types' definitions (an enum's label renamed, a
 * composite's attribute dropped, encode the same rows otherwise), and so
 * on.  Else ('D') the hash of the table's whole description and of those
 * columns and definitions.  Then the key encoder, value encoder and
 * keying, by implementation and format version: one whose format changed
 * encodes the same rows otherwise too.
 */
static void
baseline_shape(Canon *cn, Relation rel, const StringInfoData *description,
			   StringInfo shape)
{
	TupleDesc	desc = RelationGetDescr(rel);
	ProllyComponents c;
	const ProllyImplInfo *encoders[3];
	char	   *ordering = ordering_versions(rel);

	prolly_components_for_relation(rel, &c);
	if (PROLLY_HAS(c.keying, row_shape))
	{
		ProllyKeying *k = PROLLY_CALL(c.keying, open, rel, CurrentMemoryContext);
		uint32		len;
		int			at;

		appendStringInfoChar(shape, 'S');
		at = shape->len;
		appendBinaryStringInfo(shape, (const char *) &len, sizeof(len));
		PROLLY_CALL(c.keying, row_shape, k, rel, shape);
		PROLLY_CALL(c.keying, close, k);
		len = shape->len - at - sizeof(len);
		memcpy(shape->data + at, &len, sizeof(len));
	}
	else
	{
		StringInfoData match;
		ProllyHash	h;

		initStringInfo(&match);
		appendBinaryStringInfo(&match, description->data, description->len);
		appendStringInfoChar(&match, '\0');
		for (int i = 0; i < desc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(desc, i);

			if (att->attisdropped)
				continue;
			appendBinaryStringInfo(&match, (const char *) &att->attnum,
								   sizeof(att->attnum));
			prolly_datum_definition(att->atttypid, &match);
		}
		content_hash(cn, match.data, match.len, &h);
		pfree(match.data);
		appendStringInfoChar(shape, 'D');
		put_hash(shape, &h);
	}
	encoders[0] = &c.key_encoder->info;
	encoders[1] = &c.value_encoder->info;
	encoders[2] = &c.keying->info;
	for (int i = 0; i < lengthof(encoders); i++)
	{
		appendBinaryStringInfo(shape, (const char *) &encoders[i]->impl_id,
							   sizeof(encoders[i]->impl_id));
		appendBinaryStringInfo(shape,
							   (const char *) &encoders[i]->format_version,
							   sizeof(encoders[i]->format_version));
	}
	/* A changed comparator requires sorting afresh, even with no row writes. */
	if (ordering)
		appendStringInfoString(shape, ordering);
}

/*
 * A baseline_shape()'s keying's row shape, into row_shape, and the rest
 * (the encoders), into rest: false if it has none.
 */
static bool
split_shape(const StringInfoData *shape, ProllyValue *row_shape,
			ProllyValue *rest)
{
	uint32		len;

	if (shape->len < 1 + (int) sizeof(len) || shape->data[0] != 'S')
		return false;
	memcpy(&len, shape->data + 1, sizeof(len));
	if (len > shape->len - 1 - sizeof(len))
		return false;
	row_shape->data = (const uint8 *) shape->data + 1 + sizeof(len);
	row_shape->len = len;
	rest->data = row_shape->data + len;
	rest->len = shape->len - 1 - sizeof(len) - len;
	return true;
}

/*
 * Does a baseline folded under shape was hold for rel, whose shape is now?
 * The same, or (the same encoders) one the keying says every row is the
 * same under (shape_extends(): a nullable column added, an enum's label).
 */
static bool
shape_holds(Relation rel, const StringInfoData *was, const StringInfoData *now)
{
	ProllyComponents c;
	ProllyValue was_rows;
	ProllyValue was_rest;
	ProllyValue now_rows;
	ProllyValue now_rest;
	ProllyKeying *k;
	bool		holds;

	if (was->len == now->len && memcmp(was->data, now->data, now->len) == 0)
		return true;
	prolly_components_for_relation(rel, &c);
	if (!PROLLY_HAS(c.keying, shape_extends) ||
		!split_shape(was, &was_rows, &was_rest) ||
		!split_shape(now, &now_rows, &now_rest) ||
		was_rest.len != now_rest.len ||
		memcmp(was_rest.data, now_rest.data, now_rest.len) != 0)
		return false;
	k = PROLLY_CALL(c.keying, open, rel, CurrentMemoryContext);
	holds = PROLLY_CALL(c.keying, shape_extends, k, rel, &was_rows);
	PROLLY_CALL(c.keying, close, k);
	return holds;
}

/*
 * Can a baseline folded under another shape than now's be folded from, its
 * rows mapped to rel's shape now (prolly_fold_map())?  Not if the encoders
 * changed since, the keying gives no row shape or can't read the old
 * one's rows, or a column's type changed: then the finder has no baseline,
 * so reads none of what changed since by it.  *was gets the shape its rows
 * were folded under.  (The map itself waits for the finder, which asks
 * for it only once it will fold from the baseline: a fold that reads rel
 * in full instead, forced, its log saying to after a rewrite, or what
 * changed since costing more than that, maps nothing.)
 */
static bool
shape_mappable(Canon *cn, Relation rel, BaseTable *bt,
			   const StringInfoData *now, ProllyValue *was)
{
	ProllyValue was_rest;
	ProllyValue now_rows;
	ProllyValue now_rest;

	return split_shape(base_shape(cn, bt), was, &was_rest) &&
		split_shape(now, &now_rows, &now_rest) &&
		was_rest.len == now_rest.len &&
		memcmp(was_rest.data, now_rest.data, now_rest.len) == 0 &&
		prolly_fold_mappable(rel, was);
}

/*
 * Will a fold from bt map its rows to rel's shape now, *was getting the
 * shape they were folded under: they can be (shape_mappable()), and doing
 * so costs less than reading rel in full (prolly_fold_map_cheaper()).
 * Asked by table_root() and dolt_pending_full_folds() alike.
 */
static bool
shape_will_map(Canon *cn, Relation rel, BaseTable *bt,
			   const StringInfoData *now, ProllyValue *was)
{
	return shape_mappable(cn, rel, bt, now, was) &&
		prolly_fold_map_cheaper(rel);
}

/*
 * A prolly table's shape: the content hash of its baseline_shape()
 * (described in 'description'), and the collations ordering its keys,
 * added to the fold's (and to *collations if given).
 */
static void
table_shape(Canon *cn, Relation rel, const StringInfoData *description,
			ProllyHash *dh, List **collations)
{
	StringInfoData shape;
	Oid			pkoid = RelationGetPrimaryKeyIndex(rel, false);

	if (OidIsValid(pkoid))
	{
		/* (the keying's key: the primary key, as it chooses it) */
		Relation	pk = index_open(pkoid, AccessShareLock);

		for (int i = 0; i < IndexRelationGetNumberOfKeyAttributes(pk); i++)
		{
			if (!OidIsValid(pk->rd_indcollation[i]))
				continue;
			cn->collations = list_append_unique_oid(cn->collations,
													pk->rd_indcollation[i]);
			if (collations)
				*collations = list_append_unique_oid(*collations,
													 pk->rd_indcollation[i]);
		}
		index_close(pk, AccessShareLock);
	}
	initStringInfo(&shape);
	baseline_shape(cn, rel, description, &shape);
	content_hash(cn, shape.data, shape.len, dh);
	pfree(shape.data);
}

/*
 * rel's baseline_shape() bytes (described in 'description'), made into
 * *shape the first time they're asked for (shape->data NULL until then).
 */
static const StringInfoData *
table_shape_bytes(Canon *cn, Relation rel, const StringInfoData *description,
				  StringInfo shape)
{
	if (shape->data == NULL)
	{
		initStringInfo(shape);
		baseline_shape(cn, rel, description, shape);
	}
	return shape;
}

/*
 * A table's data tree, folded from its rows as the transaction sees them by
 * the change finder: from its baseline if it has one that holds for this
 * definition (described in 'description', dh its table_shape(): the same,
 * or one shape_holds() says its rows are as they were under), or one whose
 * rows will be mapped to it (shape_will_map()); with a next baseline
 * being made, its entry there too.  The same rows give the same
 * tree whatever pages, order or history they come from.
 */
static void
table_root(Canon *cn, CanonItem *it, Relation rel,
		   const StringInfoData *description, const ProllyHash *dh,
		   ProllyNodeRef *root)
{
	StringInfoData shape = {0};
	BaseTable  *found;
	BaseTable  *bt;
	ProllyTableFold tf;
	StringInfoData state;
	ProllyFullFoldReason why;

	bt = found = find_base_table(cn, &it->key);
	why = bt ? PROLLY_FULL_SHAPE_CHANGED : PROLLY_FULL_NO_BASELINE;
	memset(&tf, 0, sizeof(tf));
	if (bt)
		tf.data = bt->data;
	if (bt && !(bt->shape_hash.len == dh->len &&
				memcmp(bt->shape_hash.bytes, dh->bytes, dh->len) == 0) &&
		!shape_holds(rel, base_shape(cn, bt),
					 table_shape_bytes(cn, rel, description, &shape)))
	{
		/* (when reading another table: not mapped, as its rows aren't read) */
		if (!cn->no_rows &&
			(!OidIsValid(cn->only) || RelationGetRelid(rel) == cn->only) &&
			shape_will_map(cn, rel, bt, &shape, &tf.shape))
			tf.reshaped = true;
		else
			bt = NULL;
	}
	if (bt)
	{
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
	/* (checking a partial fold: the rows it folded as it has them) */
	foreach_ptr(CanonItem, given, cn->given)
	{
		if (given->relid == RelationGetRelid(rel))
		{
			uint8		len = (uint8) given->value.data[0];

			Assert(!cn->advance);
			root->hash.len = len;
			memcpy(root->hash.bytes, given->value.data + 1, len);
			return;
		}
	}
	initStringInfo(&state);
	pgstat_count_prolly(PGSTAT_PROLLY_FOLD_TABLES, 1);
	finder_fold(cn->finder, rel, &cn->env, bt ? &tf : NULL, why, root,
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
		put_base_shape(cn, &next->value, dh,
					   table_shape_bytes(cn, rel, description, &shape), found);
		appendBinaryStringInfo(&next->value, state.data, state.len);
		cn->next = lappend(cn->next, next);
	}
	pfree(state.data);
	if (shape.data)
		pfree(shape.data);
}

static bool namespace_known(Oid nsp, bool *canon);
static void namespace_learned(Oid nsp, bool canon, uint64 epoch);
static uint64 catalog_epoch;

/* Is an object in this namespace the canonical form's? */
static bool
canon_namespace(Oid nsp)
{
	bool		canon;
	uint64		epoch = catalog_epoch;

	if (namespace_known(nsp, &canon))
		return canon;
	canon = !is_system_namespace(nsp) && namespace_is_versioned(nsp);
	namespace_learned(nsp, canon, epoch);
	return canon;
}

/*
 * Is this object one a commit describes: in a versioned schema, and the
 * branch's own (an extension's members there are shared, and aren't)?
 */
static bool
canon_object(Oid classid, Oid objid)
{
	ObjectAddress obj;

	ObjectAddressSet(obj, classid, objid);
	return canon_namespace(get_object_namespace(&obj)) &&
		object_is_versioned(&obj);
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
		canon_object(ProcedureRelationId, rng->rngsubdiff))
	{
		ReleaseSysCache(tup);
		(void) uncovered(cn, "range types whose subtype difference function is versioned", name, &self);
		return;
	}
	{
		if (canon_object(OperatorClassRelationId, rng->rngsubopc) ||
			(OidIsValid(rng->rngcollation) &&
			 canon_object(CollationRelationId, rng->rngcollation)))
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
	return OidIsValid(extension_of(classid, objid));
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

/* ------------------------------------------------------------------------
 * Caches
 * ------------------------------------------------------------------------
 */

/*
 * This backend's caches of what each fold would otherwise work out again
 * for every table, changed or not (#483); no other implementation sees
 * them.
 *
 * - A table's description (describe_table()), with its table_shape(), by
 *   table, for the branch it was described on.  A relcache invalidation of
 *   the table drops it, and an invalidation of any syscache drops them
 *   all: a description, or a dependency's key, names objects of every
 *   kind (a type, function, schema, role, text search configuration,
 *   foreign server ...), and renaming one sends its table nothing.  Callbacks run
 *   for the transaction's own changes at each command's end too, and at a
 *   cache reset (a branch switch).  One described while an invalidation
 *   came isn't kept; and they are dropped when the settings that change how
 *   a constant prints (DateStyle, say) differ from those they were
 *   described under.
 * - The tree of what the versioned objects depend on (fold_dependencies()),
 *   against an epoch every one of those callbacks moves, and a relcache
 *   invalidation of pg_depend or pg_extension, which neither has a
 *   syscache to send (inval.c sends one for each change to them).
 * - The last canonical tree built, with its items in order: the next one
 *   is it with only the items that differ applied.  Its hash is its
 *   content, so it is never stale.
 * - What that tree (and the last next baseline built) was folded at: the
 *   refs' mark and the catalog epoch, so the next fold can take its items
 *   and work out again only the tables written since and the sequences
 *   (fold_changed_only()).
 * - Which objects are extensions' members, and which namespaces' objects
 *   the canonical form's, against the same epoch: each is asked of every
 *   relation, type and function.
 *
 * A tree is taken only while its chunks are known to be there: made by
 * this transaction (not in a subtransaction rolled back) or one that
 * committed, and still in the store, which keeps it from a collection
 * from then on (keep()).  With debug_prolly_verify_fold_caches, whatever
 * is taken is worked out again too, and they differing is an error.
 */
typedef struct DescribedTable
{
	Oid			relid;			/* the hash key */
	Oid			branch;			/* the branch it was described on */
	char	   *description;
	int			len;
	bool		shaped;			/* a prolly table's: shape and collations */
	ProllyHash	shape;
	int			ncollations;
	Oid		   *collations;
} DescribedTable;

/* A tree made here, and whether its chunks are known to be there. */
typedef struct MadeTree
{
	bool		valid;
	Oid			branch;			/* the session's branch when it was made */
	ProllyNodeRef root;
	bool		pending;		/* made by the transaction in progress */
	SubTransactionId subid;		/* (pending) in this subtransaction */
} MadeTree;

static bool fold_caches_registered = false;
static uint64 catalog_epoch = 0;	/* moved by each callback */
static MemoryContext described_cxt = NULL;
static HTAB *described = NULL;
static MadeTree dependencies_made;
static uint64 dependencies_epoch;

/*
 * The extensions with configuration tables the tree made last recorded,
 * and their elements then: their rows aren't catalogs, so the tree is
 * taken again only while they are what they were.
 */
static List *dependencies_configured;
static char *dependencies_config;

/*
 * A digest of extensions' configuration elements, as one string: read
 * without encoding the rows (prolly_extconfig_fingerprint()).
 */
static char *
config_fingerprint(List *extoids)
{
	StringInfoData out;
	ListCell   *lc;

	initStringInfo(&out);
	foreach(lc, extoids)
		appendStringInfoString(&out, prolly_extconfig_fingerprint(lfirst_oid(lc)));
	return out.data;
}
static MadeTree items_made;

static FoldedAt items_at;
static MadeTree baseline_made;	/* the last next baseline built here */
static FoldedAt baseline_at;
static Oid *made_sequences = NULL;	/* the sequences among made_items */
static int	nmade_sequences = 0;
static MemoryContext items_cxt = NULL;
static CanonItem *made_items = NULL;	/* items_made's, in key order */
static int	nmade_items = 0;

/*
 * The last definitions read here (a fold of no table's rows, from a
 * baseline): their tree, its items, sequences and what it left out, as of
 * the catalog epoch it began at and the baseline it took the rows of.
 * Kept apart from the folds' own items, which the next fold works out the
 * changes from.
 */
static MadeTree defs_made;
static bool defs_valid = false;
static uint64 defs_epoch;
static ProllyHash defs_baseline;
static MemoryContext defs_cxt = NULL;
static CanonItem *defs_items = NULL;
static int	ndefs_items = 0;
static Oid *defs_sequences = NULL;
static int	ndefs_sequences = 0;
static List *defs_left_out = NIL;	/* char *, in defs_cxt */
static MemoryContext members_cxt = NULL;
static HTAB *members = NULL;	/* Member, by object */
static uint64 members_epoch;
static uint64 namespaces_epoch;
static int	nnamespaces = 0;
static struct
{
	Oid			nsp;
	bool		canon;
}			namespaces[16];

/* An extension's member: (class, object) -> extension. */
typedef struct Member
{
	Oid			classid;
	Oid			objid;
	Oid			extension;
} Member;

static void
forget_descriptions(void)
{
	if (described_cxt != NULL)
		MemoryContextReset(described_cxt);
	described = NULL;
}

static void
fold_caches_relcache(Datum arg, Oid relid)
{
	DescribedTable *d;

	catalog_epoch++;
	if (!OidIsValid(relid))
		forget_descriptions();
	else if (described != NULL &&
			 (d = hash_search(described, &relid, HASH_FIND, NULL)) != NULL)
	{
		pfree(d->description);
		if (d->collations)
			pfree(d->collations);
		(void) hash_search(described, &relid, HASH_REMOVE, NULL);
	}
}

static void
fold_caches_syscache(Datum arg, int cacheid, uint32 hashvalue)
{
	catalog_epoch++;
	forget_descriptions();
}

/* A transaction's end: its trees kept if it committed, else dropped. */
static void
made_tree_end(MadeTree *t, bool commit)
{
	if (t->valid && t->pending)
	{
		t->pending = false;
		t->valid = commit;
	}
}

/* (prepared: its chunks are others' only if it commits, and not here) */
static void
fold_caches_xact(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
			made_tree_end(&dependencies_made, true);
			made_tree_end(&items_made, true);
			made_tree_end(&baseline_made, true);
			made_tree_end(&defs_made, true);
			break;
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
		case XACT_EVENT_PREPARE:
			made_tree_end(&dependencies_made, false);
			made_tree_end(&items_made, false);
			made_tree_end(&baseline_made, false);
			made_tree_end(&defs_made, false);
			break;
		default:
			break;
	}
}

static void
made_tree_sub(MadeTree *t, SubXactEvent event, SubTransactionId mySubid,
			  SubTransactionId parentSubid)
{
	if (!t->valid || !t->pending || t->subid != mySubid)
		return;
	if (event == SUBXACT_EVENT_COMMIT_SUB)
		t->subid = parentSubid;
	else if (event == SUBXACT_EVENT_ABORT_SUB)
		t->valid = false;
}

static void
fold_caches_subxact(SubXactEvent event, SubTransactionId mySubid,
					SubTransactionId parentSubid, void *arg)
{
	made_tree_sub(&dependencies_made, event, mySubid, parentSubid);
	made_tree_sub(&items_made, event, mySubid, parentSubid);
	made_tree_sub(&baseline_made, event, mySubid, parentSubid);
	made_tree_sub(&defs_made, event, mySubid, parentSubid);
}

/*
 * Ready the caches for a fold: callbacks registered once, and every
 * invalidation sent so far taken in (a catalog read now may see changes
 * whose invalidations haven't been, which a cache would miss).
 */
static void
fold_caches_begin(void)
{
	if (!fold_caches_registered)
	{
		CacheRegisterRelcacheCallback(fold_caches_relcache, (Datum) 0);

		/*
		 * (a description, and a dependency's key, print by name whatever
		 * they refer to, of any kind: a type, a function, a text search
		 * configuration, a foreign server.  Renaming one tells no table it
		 * is of, nor pg_depend, only its own syscache: every one's)
		 */
		CacheRegisterAnySyscacheCallback(fold_caches_syscache, (Datum) 0);
		RegisterXactCallback(fold_caches_xact, NULL);
		RegisterSubXactCallback(fold_caches_subxact, NULL);
		described_cxt = AllocSetContextCreate(CacheMemoryContext,
											  "prolly folded descriptions",
											  ALLOCSET_DEFAULT_SIZES);
		items_cxt = AllocSetContextCreate(CacheMemoryContext,
										  "prolly folded items",
										  ALLOCSET_DEFAULT_SIZES);
		members_cxt = AllocSetContextCreate(CacheMemoryContext,
											"prolly extension members",
											ALLOCSET_DEFAULT_SIZES);
		defs_cxt = AllocSetContextCreate(CacheMemoryContext,
										 "prolly folded definitions",
										 ALLOCSET_DEFAULT_SIZES);
		fold_caches_registered = true;
	}
	AcceptInvalidationMessages();
}

/*
 * The extension an object is a member of (InvalidOid: none), as
 * getExtensionOfObject() finds it, from every member read at once.  (Before
 * a fold readies the caches, nothing would say they changed: asked of the
 * catalog each time.)
 */
static Oid
extension_of(Oid classid, Oid objid)
{
	Member		key;
	Member	   *m;
	Oid			ext;

	if (!fold_caches_registered)
		return getExtensionOfObject(classid, objid);
	if (members == NULL || members_epoch != catalog_epoch)
	{
		uint64		epoch = catalog_epoch;
		Relation	rel;
		ScanKeyData skey;
		SysScanDesc scan;
		HeapTuple	tup;
		HASHCTL		ctl;

		MemoryContextReset(members_cxt);
		members = NULL;
		ctl.keysize = offsetof(Member, extension);
		ctl.entrysize = sizeof(Member);
		ctl.hcxt = members_cxt;
		members = hash_create("prolly extension members", 256, &ctl,
							  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		rel = table_open(DependRelationId, AccessShareLock);
		ScanKeyInit(&skey, Anum_pg_depend_refclassid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(ExtensionRelationId));
		scan = systable_beginscan(rel, DependReferenceIndexId, true, NULL, 1,
								  &skey);
		while ((tup = systable_getnext(scan)) != NULL)
		{
			Form_pg_depend d = (Form_pg_depend) GETSTRUCT(tup);
			bool		found;

			if (d->deptype != DEPENDENCY_EXTENSION)
				continue;
			key.classid = d->classid;
			key.objid = d->objid;
			m = hash_search(members, &key, HASH_ENTER, &found);
			if (!found)
				m->extension = d->refobjid;
		}
		systable_endscan(scan);
		table_close(rel, AccessShareLock);
		/* (read while an invalidation came: read again next time) */
		members_epoch = epoch;
	}
	key.classid = classid;
	key.objid = objid;
	m = hash_search(members, &key, HASH_FIND, NULL);
	ext = m ? m->extension : InvalidOid;
	if (debug_prolly_verify_fold_caches &&
		ext != getExtensionOfObject(classid, objid))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly extension membership of object %u of catalog %u taken from this backend's cache differs from pg_depend's",
						objid, classid)));
	return ext;
}

/* canon_namespace()'s answer for nsp, if known since nothing changed. */
static bool
namespace_known(Oid nsp, bool *canon)
{
	if (!fold_caches_registered || namespaces_epoch != catalog_epoch)
		return false;
	for (int i = 0; i < nnamespaces; i++)
	{
		if (namespaces[i].nsp == nsp)
		{
			*canon = namespaces[i].canon;
			return true;
		}
	}
	return false;
}

/* (learned at epoch: not kept if an invalidation came meanwhile) */
static void
namespace_learned(Oid nsp, bool canon, uint64 epoch)
{
	if (!fold_caches_registered || epoch != catalog_epoch)
		return;
	if (namespaces_epoch != catalog_epoch)
	{
		namespaces_epoch = catalog_epoch;
		nnamespaces = 0;
	}
	if (nnamespaces < lengthof(namespaces))
	{
		namespaces[nnamespaces].nsp = nsp;
		namespaces[nnamespaces++].canon = canon;
	}
}

static void
made_tree(MadeTree *t, const ProllyNodeRef *root)
{
	t->valid = true;
	t->branch = MyBranch;
	t->root = *root;
	t->pending = true;
	t->subid = GetCurrentSubTransactionId();
}

/*
 * Whether a tree made here can be taken: made on the session's branch, and
 * its chunks there, kept from here on.
 */
static bool
made_tree_usable(Canon *cn, const MadeTree *t)
{
	if (!t->valid || t->branch != MyBranch || RecoveryInProgress() ||
		!PROLLY_CALL(cn->env.chunk_store, has, cn->env.chunks, &t->root.hash))
		return false;
	PROLLY_CALL(cn->env.chunk_store, keep, cn->env.chunks, &t->root.hash);
	return true;
}

static void
put_description(DescribedTable *d, const StringInfoData *desc,
				const ProllyHash *shape, List *collations)
{
	d->branch = MyBranch;
	d->description = MemoryContextAlloc(described_cxt, desc->len + 1);
	memcpy(d->description, desc->data, desc->len + 1);
	d->len = desc->len;
	d->shaped = shape != NULL;
	if (shape)
		d->shape = *shape;
	d->ncollations = list_length(collations);
	d->collations = NULL;
	if (collations != NIL)
	{
		d->collations = MemoryContextAlloc(described_cxt,
										   sizeof(Oid) * d->ncollations);
		for (int i = 0; i < d->ncollations; i++)
			d->collations[i] = list_nth_oid(collations, i);
	}
}

/*
 * rel's description (describe_table()) into desc, and for a prolly table
 * (shape not NULL) its table_shape(), from the cache if there.
 */
static void
table_description(Canon *cn, Relation rel, StringInfo desc, ProllyHash *shape)
{
	Oid			relid = RelationGetRelid(rel);
	DescribedTable *d = NULL;
	uint64		epoch = catalog_epoch;
	List	   *collations = NIL;

	if (cn->cached && described != NULL)
		d = hash_search(described, &relid, HASH_FIND, NULL);
	if (d != NULL && d->branch == MyBranch && d->shaped == (shape != NULL))
	{
		appendBinaryStringInfo(desc, d->description, d->len);
		if (shape)
			*shape = d->shape;
		for (int i = 0; i < d->ncollations; i++)
			cn->collations = list_append_unique_oid(cn->collations,
													d->collations[i]);
		pgstat_count_prolly(PGSTAT_PROLLY_DESCRIBE_CACHE_HITS, 1);
		if (debug_prolly_verify_fold_caches)
		{
			StringInfoData again;
			ProllyHash	h;

			initStringInfo(&again);
			describe_table(cn, rel, &again);
			if (shape)
				table_shape(cn, rel, &again, &h, NULL);
			if (again.len != desc->len ||
				memcmp(again.data, desc->data, desc->len) != 0 ||
				(shape && (h.len != shape->len ||
						   memcmp(h.bytes, shape->bytes, h.len) != 0)))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("prolly description of table \"%s\" taken from this backend's cache differs from its catalogs",
								RelationGetRelationName(rel))));
			pfree(again.data);
		}
		return;
	}
	describe_table(cn, rel, desc);
	if (shape)
		table_shape(cn, rel, desc, shape, &collations);
	if (!cn->cached || catalog_epoch != epoch)
		return;
	if (described == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(DescribedTable);
		ctl.hcxt = described_cxt;
		described = hash_create("prolly folded descriptions", 256, &ctl,
								HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	d = hash_search(described, &relid, HASH_ENTER, NULL);
	put_description(d, desc, shape, collations);
}

/*
 * With debug_prolly_verify_fold_caches, a tree applied from an earlier one
 * (#483) against the same items built whole.
 */
static void
check_applied(const char *what, const ProllyNodeRef *applied,
			  const ProllyNodeRef *built)
{
	if (applied->hash.len != built->hash.len ||
		memcmp(applied->hash.bytes, built->hash.bytes, built->hash.len) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly %s tree applied from an earlier one differs from it built whole",
						what),
				 errdetail("Applied: %s; built: %s.",
						   prolly_hash_hex(&applied->hash),
						   prolly_hash_hex(&built->hash))));
}

/*
 * The canonical tree of cn's items (sorted), as the last one built here with
 * the items that differ applied (true), if it can be taken.
 */
static bool
apply_items(Canon *cn, ProllyNodeRef *root)
{
	ProllyEdit *edits;
	int			nedits = 0;
	int			i = 0;
	ListCell   *lc;

	if (made_items == NULL || !made_tree_usable(cn, &items_made))
		return false;
	edits = palloc_array(ProllyEdit, nmade_items + list_length(cn->items));
	lc = list_head(cn->items);
	while (i < nmade_items || lc != NULL)
	{
		CanonItem  *was = i < nmade_items ? &made_items[i] : NULL;
		CanonItem  *is = lc ? lfirst(lc) : NULL;
		ProllyKey	kw = {0};
		ProllyKey	ki = {0};
		int			c;

		if (was)
			kw = (ProllyKey) {(const uint8 *) was->key.data, was->key.len};
		if (is)
			ki = (ProllyKey) {(const uint8 *) is->key.data, is->key.len};
		c = !was ? 1 : !is ? -1 : prolly_bytes_compare(&kw, &ki, NULL);
		if (c < 0)
		{
			edits[nedits].key = kw;
			edits[nedits++].is_delete = true;
		}
		else if (c > 0 || was->value.len != is->value.len ||
				 memcmp(was->value.data, is->value.data, is->value.len) != 0)
		{
			edits[nedits].key = ki;
			edits[nedits].value = (ProllyValue) {(const uint8 *) is->value.data,
			is->value.len};
			edits[nedits++].is_delete = false;
		}
		if (c <= 0)
			i++;
		if (c >= 0)
			lc = lnext(cn->items, lc);
	}
	if (nedits == 0)
		*root = items_made.root;
	else
		PROLLY_CALL(cn->tree, apply, &cn->root_env, &items_made.root, edits,
					nedits, root);
	pfree(edits);
	return true;
}

/* Keep cn's items (sorted) as those of the canonical tree root. */
static void
remember_items(Canon *cn, const ProllyNodeRef *root)
{
	ListCell   *lc;

	if (made_items != NULL && items_made.valid &&
		items_made.branch == MyBranch &&
		items_made.root.hash.len == root->hash.len &&
		memcmp(items_made.root.hash.bytes, root->hash.bytes, root->hash.len) == 0)
		return;
	MemoryContextReset(items_cxt);
	made_items = MemoryContextAlloc(items_cxt,
									sizeof(CanonItem) * Max(list_length(cn->items), 1));
	nmade_items = 0;
	foreach(lc, cn->items)
	{
		CanonItem  *it = lfirst(lc);
		CanonItem  *m = &made_items[nmade_items++];

		m->key.data = MemoryContextAlloc(items_cxt, it->key.len + 1);
		memcpy(m->key.data, it->key.data, it->key.len + 1);
		m->key.len = m->key.maxlen = it->key.len;
		m->key.cursor = 0;
		m->value.data = MemoryContextAlloc(items_cxt, it->value.len + 1);
		memcpy(m->value.data, it->value.data, it->value.len + 1);
		m->value.len = m->value.maxlen = it->value.len;
		m->value.cursor = 0;
		m->relid = it->relid;
	}
	made_sequences = MemoryContextAlloc(items_cxt,
										sizeof(Oid) * Max(nmade_items, 1));
	nmade_sequences = 0;
	for (int i = 0; i < nmade_items; i++)
	{
		if (OidIsValid(made_items[i].relid) &&
			strncmp(made_items[i].key.data, "s:", 2) == 0)
			made_sequences[nmade_sequences++] = made_items[i].relid;
	}
	made_tree(&items_made, root);
}

/*
 * Keep cn's items (sorted), a fold of no table's rows from cn's baseline,
 * as the last definitions read: not if the transaction changed a catalog
 * (what it read is so only if it commits).
 */
static void
remember_definitions(Canon *cn, const ProllyNodeRef *root)
{
	MemoryContext old;

	defs_valid = false;
	if (cn->baseline == NULL || XactHasInvalidations() || defs_cxt == NULL)
		return;
	MemoryContextReset(defs_cxt);
	old = MemoryContextSwitchTo(defs_cxt);
	defs_items = palloc_array(CanonItem, Max(list_length(cn->items), 1));
	defs_sequences = palloc_array(Oid, Max(list_length(cn->items), 1));
	ndefs_items = ndefs_sequences = 0;
	foreach_ptr(CanonItem, it, cn->items)
	{
		CanonItem  *m = &defs_items[ndefs_items++];

		initStringInfo(&m->key);
		appendBinaryStringInfo(&m->key, it->key.data, it->key.len);
		initStringInfo(&m->value);
		appendBinaryStringInfo(&m->value, it->value.data, it->value.len);
		m->relid = it->relid;
		if (OidIsValid(it->relid) && strncmp(it->key.data, "s:", 2) == 0)
			defs_sequences[ndefs_sequences++] = it->relid;
	}
	defs_left_out = NIL;
	foreach_ptr(char, name, cn->left_out)
		defs_left_out = lappend(defs_left_out, pstrdup(name));
	MemoryContextSwitchTo(old);
	defs_baseline = cn->baseline->hash;
	defs_epoch = cn->at.catalog_epoch;
	made_tree(&defs_made, root);
	defs_valid = true;
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
describe_dependency(const ObjectAddress *addr, List *ordering, List **configured)
{
	Dependency *dep = palloc(sizeof(Dependency));
	char	   *type = getObjectTypeDescription(addr, false);

	initStringInfo(&dep->key);
	initStringInfo(&dep->value);
	append_utf8(&dep->key, psprintf("%s %s", type, getObjectIdentity(addr, false)));
	if (addr->classId == ExtensionRelationId)
	{
		Oid			nsp = get_extension_schema(addr->objectId);

		List	   *value = list_make2(extension_version(addr->objectId),
										   get_namespace_name(nsp));

		/* (a clone makes a versioned schema before the extension in it) */
		if (namespace_is_versioned(nsp))
			value = lappend(value, "versioned");
		if (list_member_oid(ordering, addr->objectId))
			value = lappend(value, "ordering");
		{
			bool		has_config = false;

			value = list_concat(value, prolly_extconfig_elements(addr->objectId, &has_config));
			if (has_config)
				*configured = lappend_oid(*configured, addr->objectId);
		}
		append_utf8(&dep->value, text_array_literal(value));
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
	List	   *ordering = NIL;
	ListCell   *lc;
	ProllyTreeBuilder *b;
	uint64		epoch;
	bool		taken = false;
	List	   *configured = NIL;

	memset(&cn, 0, sizeof(cn));
	canon_env(&cn);

	/* (the tree made last, if nothing it was read from changed since: #483) */
	fold_caches_begin();
	epoch = catalog_epoch;
	if (dependencies_epoch == epoch && made_tree_usable(&cn, &dependencies_made) &&
		(dependencies_configured == NIL ||
		 strcmp(config_fingerprint(dependencies_configured), dependencies_config) == 0))
	{
		*out = dependencies_made.root;
		if (!debug_prolly_verify_fold_caches)
			return;
		taken = true;
	}
	ctl.keysize = sizeof(ObjectAddress);
	ctl.entrysize = sizeof(ObjectAddress);
	ctl.hcxt = CurrentMemoryContext;
	seen = hash_create("prolly dependencies", 64, &ctl,
					   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/*
	 * Core polymorphic opclasses have no pg_depend entry on the extension
	 * comparators they call.  Inspect every canonical table's key types.
	 */
	rel = table_open(RelationRelationId, AccessShareLock);
	scan = catalog_branch_beginscan(rel, MyBranch);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_class form = (Form_pg_class) GETSTRUCT(tup);

		if (form->relkind == RELKIND_RELATION &&
			form->relam == PROLLY_TABLE_AM_OID && canon_namespace(form->relnamespace) &&
			!in_extension(RelationRelationId, form->oid))
		{
			Relation table = table_open(form->oid, AccessShareLock);

			ordering = list_concat_unique_oid(ordering, ordering_extensions(table));
			table_close(table, AccessShareLock);
		}
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	rel = table_open(DependRelationId, AccessShareLock);
	scan = catalog_branch_beginscan(rel, MyBranch);
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
		ext = extension_of(referenced.classId, referenced.objectId);
		if (OidIsValid(ext))
			ObjectAddressSet(referenced, ExtensionRelationId, ext);
		(void) hash_search(seen, &referenced, HASH_ENTER, &found);
		if (!found)
			deps = lappend(deps, hash_search(seen, &referenced, HASH_FIND, NULL));
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	/* Include key opclasses even when their extension's schema is versioned. */
	foreach(lc, ordering)
	{
		ObjectAddress addr;
		void	   *entry;
		bool		found;

		ObjectAddressSet(addr, ExtensionRelationId, lfirst_oid(lc));
		entry = hash_search(seen, &addr, HASH_ENTER, &found);
		if (!found)
			deps = lappend(deps, entry);
	}
	foreach(lc, deps)
		lfirst(lc) = describe_dependency(lfirst(lc), ordering, &configured);

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
	if (taken)
	{
		ProllyNodeRef built;

		PROLLY_CALL(cn.tree, build_finish, b, &built);
		if (built.hash.len != out->hash.len ||
			memcmp(built.hash.bytes, out->hash.bytes, built.hash.len) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("prolly dependencies taken from this backend's cache differ from the catalogs'"),
					 errdetail("Taken: %s; folded: %s.",
							   prolly_hash_hex(&out->hash),
							   prolly_hash_hex(&built.hash))));
	}
	else
	{
		PROLLY_CALL(cn.tree, build_finish, b, out);
		if (catalog_epoch == epoch)
		{
			MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

			made_tree(&dependencies_made, out);
			dependencies_epoch = epoch;
			list_free(dependencies_configured);
			if (dependencies_config)
				pfree(dependencies_config);
			dependencies_configured = list_copy(configured);
			MemoryContextSwitchTo(old);
			dependencies_config = MemoryContextStrdup(TopMemoryContext,
													  config_fingerprint(configured));
		}
	}
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

/*
 * A relation's items, if the canonical form holds it (in a versioned
 * schema, not an extension's member).  The item of a table's, materialized
 * view's or sequence's rows or value names it (relid): the first it adds,
 * any ordering item after.
 */
static void
collect_relation(Canon *cn, Form_pg_class form)
{
	char	   *name = NameStr(form->relname);
	int			before = list_length(cn->items);

	if (!canon_namespace(form->relnamespace) ||
		in_extension(RelationRelationId, form->oid))
		return;
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
				ProllyHash	dh;

				if (oid_columns(cn, rel) && !cn->reading)
				{
					table_close(rel, AccessShareLock);
					break;
				}
				it = add_item(cn, "t:", qualified(form->relnamespace, name));
				initStringInfo(&desc);
				table_description(cn, rel, &desc, &dh);
				record_ordering(cn, rel, &desc);
				table_root(cn, it, rel, &desc, &dh, &root);
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
					ProllyHash	dh;

					table_shape(cn, rel, &query, &dh, NULL);
					table_root(cn, it, rel, &query, &dh, &root);
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
				const char *what = NULL;

				/* (what CREATE FOREIGN TABLE alone doesn't make: not yet) */
				ObjectAddressSet(self, RelationRelationId, form->oid);
				if (form->relispartition)
					what = "foreign tables that are partitions";
				else if (has_superclass(form->oid))
					what = "foreign tables that inherit";
				else if (form->relhastriggers && has_triggers(form->oid))
					what = "triggers on foreign tables";
				else if (form->relrowsecurity)
					what = "row security on foreign tables";
				else
					what = foreign_table_undescribed(form->oid);
				if (what)
					(void) uncovered(cn, what, qualified(form->relnamespace, name),
									 &self);
				else
					describe_foreign_table(form->oid,
										   &add_item(cn, "r:", qualified(form->relnamespace, name))->value);
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
				table_description(cn, rel, &desc, NULL);
				put_hash(&it->value, &root.hash);
				appendBinaryStringInfo(&it->value, desc.data, desc.len);
				pfree(desc.data);
				table_close(rel, AccessShareLock);
			}
			break;
		case RELKIND_PARTITIONED_INDEX:
			break;			/* with its table */
	}
	if (list_length(cn->items) > before &&
		(form->relkind == RELKIND_SEQUENCE ||
		 ((form->relkind == RELKIND_RELATION ||
		   form->relkind == RELKIND_MATVIEW) &&
		  form->relam == PROLLY_TABLE_AM_OID)))
		((CanonItem *) list_nth(cn->items, before))->relid = form->oid;
}

static void
collect(Canon *cn)
{
	Relation	pg_class;
	SysScanDesc scan;
	HeapTuple	tup;

	pgstat_count_prolly(PGSTAT_PROLLY_FOLD_CATALOG_SCANS, 1);

	/* Relations. */
	pg_class = table_open(RelationRelationId, AccessShareLock);
	scan = catalog_branch_beginscan(pg_class, MyBranch);
	while ((tup = systable_getnext(scan)) != NULL)
		collect_relation(cn, (Form_pg_class) GETSTRUCT(tup));
	systable_endscan(scan);
	table_close(pg_class, AccessShareLock);

	/* Versioned schemas themselves, empty ones too. */
	{
		Relation	pg_ns = table_open(NamespaceRelationId, AccessShareLock);

		scan = catalog_branch_beginscan(pg_ns, MyBranch);
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

		scan = catalog_branch_beginscan(pg_type, MyBranch);
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

		scan = catalog_branch_beginscan(pg_proc, MyBranch);
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
			name = routine_item_name(proc);
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
		scan = catalog_branch_beginscan(rel, MyBranch);
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
	SysScanDesc scan = catalog_branch_beginscan(rel, MyBranch);
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
 * "implicit"), a line each.  (What initdb made casts no such type.)  Every
 * cast is read, not only the branch's: one between a type an extension has
 * in a versioned schema and a type outside, made outside the extension, is
 * shared, as both its types are.
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

/*
 * The canonical tree of everything, built from the items collected: the
 * catalogs read through, every table's rows folded.
 */
static void
build_all(Canon *cn, ProllyNodeRef *root)
{
	int			save_nestlevel;
	ProllyTreeBuilder *b;
	ListCell   *lc;
	bool		applied;

	if (cn->baseline)
		load_baseline(cn, cn->baseline);
	save_nestlevel = NewGUCNestLevel();
	describing_settings();
	collect(cn);
	AtEOXact_GUC(false, save_nestlevel);
	add_collations(cn);
	check_refused(cn);
	appendStringInfoString(&add_item(cn, "!:", "format")->value, FOLDED_FORMAT);

	list_sort(cn->items, item_cmp);
	applied = apply_items(cn, root);
	if (!applied || debug_prolly_verify_fold_caches)
	{
		ProllyNodeRef built;

		b = PROLLY_CALL(cn->tree, build_begin, &cn->root_env, CurrentMemoryContext);
		foreach(lc, cn->items)
		{
			CanonItem  *it = lfirst(lc);
			ProllyKey	k = {(const uint8 *) it->key.data, it->key.len};
			ProllyValue v = {(const uint8 *) it->value.data, it->value.len};

			PROLLY_CALL(cn->tree, build_add, b, &k, &v);
		}
		PROLLY_CALL(cn->tree, build_finish, b, &built);
		if (applied)
			check_applied("canonical", root, &built);
		*root = built;
	}
	if (cn->verifying)
		return;
	if (cn->no_rows)
		remember_definitions(cn, root);
	else
		remember_items(cn, root);
}

static int
made_item_cmp(const void *a, const void *b)
{
	const CanonItem *x = a;
	const CanonItem *y = b;
	ProllyKey	kx = {(const uint8 *) x->key.data, x->key.len};
	ProllyKey	ky = {(const uint8 *) y->key.data, y->key.len};

	return prolly_bytes_compare(&kx, &ky, NULL);
}

/* The last canonical tree's item of key, or NULL. */
static CanonItem *
made_item(const StringInfoData *key)
{
	CanonItem	probe;

	probe.key = *key;
	return bsearch(&probe, made_items, nmade_items, sizeof(CanonItem),
				   made_item_cmp);
}

/* The baseline's entries of keys (StringInfo *), in key order. */
static void
load_baseline_keys(Canon *cn, const ProllyNodeRef *baseline, List *keys)
{
	StringInfoData v;

	initStringInfo(&v);
	foreach_ptr(StringInfoData, key, keys)
	{
		ProllyKey	k = {(const uint8 *) key->data, key->len};
		ProllyValue pv;
		BaseTable  *bt;

		resetStringInfo(&v);
		if (!PROLLY_CALL(cn->tree, get, &cn->env, baseline, &k, &v))
			continue;
		if (cn->nbase == cn->maxbase)
		{
			cn->maxbase = Max(16, cn->maxbase * 2);
			cn->base = cn->base ? repalloc_array(cn->base, BaseTable, cn->maxbase) :
				palloc_array(BaseTable, cn->maxbase);
		}
		bt = &cn->base[cn->nbase];
		initStringInfo(&bt->key);
		appendBinaryStringInfo(&bt->key, key->data, key->len);
		pv.data = (const uint8 *) v.data;
		pv.len = v.len;
		if (!parse_base_table(&pv, bt))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("a branch's baseline holds an invalid table entry")));
		cn->nbase++;
	}
	pfree(v.data);
	if (cn->nbase > 1)
		qsort(cn->base, cn->nbase, sizeof(BaseTable), base_table_cmp);
}

/*
 * The item key collect_relation() gives a relation's rows or value (a
 * prolly table's or materialized view's, a sequence's), or NULL if it
 * gives none.
 */
static StringInfo
relation_item_key(Form_pg_class form)
{
	const char *kind;
	StringInfo	key;

	if (form->relkind == RELKIND_SEQUENCE)
		kind = "s:";
	else if (form->relam != PROLLY_TABLE_AM_OID)
		return NULL;
	else if (form->relkind == RELKIND_RELATION)
		kind = "t:";
	else if (form->relkind == RELKIND_MATVIEW)
		kind = "m:";
	else
		return NULL;
	key = makeStringInfo();
	appendStringInfoString(key, kind);
	append_utf8(key, qualified(form->relnamespace, NameStr(form->relname)));
	return key;
}

/*
 * The canonical tree as the last one built here, only what may have
 * changed since worked out again (true): the items of the tables and
 * materialized views whose rows a transaction that ended since, or this
 * one, wrote (the refs' written_since()), and every sequence's, whose
 * values change outside transactions.  The rest is as it was, as nothing
 * else a fold reads changed: no catalog change or collection since (the
 * refs' mark, which a catalog change sent moves, and this backend's
 * catalog epoch, which its own move), the same deparsing settings, and a
 * snapshot that sees every writer that ended (the mark is taken only
 * then).  A fold that begins the next baseline does so from the one built
 * here last, whose entries stay but those of the tables written since it.
 * False where it can't be: then everything is folded.
 * debug_prolly_verify_fold_caches then folds everything but the tables
 * this fold folded, and the trees differing is an error.
 */
static bool
fold_changed_only(Canon *cn, ProllyNodeRef *root)
{
	ProllyDatabaseComponents dc;
	ProllyFullFoldReason reason;
	const FoldedAt *since = &items_at;
	Oid		   *relids;
	int			nrelids;
	List	   *seen = NIL;
	List	   *rels = NIL;
	List	   *keys = NIL;
	ProllyNodeRef expect = {0};
	ProllyEdit *edits;
	CanonItem **changed;
	int			nedits = 0;
	int			save_nestlevel;

	/* (a fold from no baseline is asked to fold everything from nothing) */
	if (!cn->marked || !items_at.valid || made_items == NULL ||
		cn->baseline == NULL || prolly_full_fold_wanted(&reason) ||
		items_at.catalog_epoch != catalog_epoch ||
		!made_tree_usable(cn, &items_made))
		return false;
	if (cn->advance)
	{
		if (cn->baseline == NULL || !baseline_at.valid ||
			baseline_at.catalog_epoch != catalog_epoch ||
			baseline_made.root.hash.len != cn->baseline->hash.len ||
			memcmp(baseline_made.root.hash.bytes, cn->baseline->hash.bytes,
				   cn->baseline->hash.len) != 0 ||
			!made_tree_usable(cn, &baseline_made))
			return false;
		if (baseline_at.mark.generation < items_at.mark.generation)
			since = &baseline_at;
	}
	prolly_database_components(&dc);
	if (!PROLLY_HAS(dc.refs, written_since) ||
		!PROLLY_CALL(dc.refs, written_since, &since->mark, GetActiveSnapshot(),
					 &relids, &nrelids))
		return false;

	/*
	 * What to work out again: each relation's item, as the last tree has it
	 * (one it doesn't have is a catalog change, which the mark would have
	 * seen; but never folded so)
	 */
	for (int i = 0; i < nrelids + nmade_sequences; i++)
	{
		Oid			relid = i < nrelids ? relids[i] : made_sequences[i - nrelids];
		HeapTuple	tup;
		Form_pg_class form;
		StringInfo	key;

		if (list_member_oid(seen, relid))
			continue;
		seen = lappend_oid(seen, relid);
		tup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));
		if (!HeapTupleIsValid(tup))
			return false;
		form = (Form_pg_class) GETSTRUCT(tup);
		if (!canon_namespace(form->relnamespace) ||
			in_extension(RelationRelationId, form->oid) ||
			(key = relation_item_key(form)) == NULL)
			continue;			/* (no item: none of the canonical form's) */
		if (made_item(key) == NULL)
			return false;
		if (form->relkind != RELKIND_SEQUENCE)
			keys = lappend(keys, key);
		rels = lappend(rels, tup);
	}

	cn->partial = true;
	if (cn->baseline && keys != NIL)
		load_baseline_keys(cn, cn->baseline, keys);
	save_nestlevel = NewGUCNestLevel();
	describing_settings();
	foreach_ptr(HeapTupleData, tup, rels)
		collect_relation(cn, (Form_pg_class) GETSTRUCT(tup));
	AtEOXact_GUC(false, save_nestlevel);
	if (cn->left_out != NIL || cn->refused != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("prolly fold of the objects changed since the last left out one it had held")));

	/*
	 * Each item that differs, applied to the last tree; the items kept
	 * changed only once the tree is made, so they are always a tree's.
	 */
	list_sort(cn->items, item_cmp);
	edits = palloc_array(ProllyEdit, Max(list_length(cn->items), 1));
	changed = palloc_array(CanonItem *, Max(list_length(cn->items), 1));
	foreach_ptr(CanonItem, it, cn->items)
	{
		CanonItem  *m = made_item(&it->key);

		if (m == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("prolly fold of the objects changed since the last found one it didn't hold")));
		if (m->value.len == it->value.len &&
			memcmp(m->value.data, it->value.data, it->value.len) == 0)
			continue;
		changed[nedits] = it;
		edits[nedits].key = (ProllyKey) {(const uint8 *) it->key.data, it->key.len};
		edits[nedits].value = (ProllyValue) {(const uint8 *) it->value.data, it->value.len};
		edits[nedits++].is_delete = false;
	}
	if (nedits == 0)
		*root = items_made.root;
	else
		PROLLY_CALL(cn->tree, apply, &cn->root_env, &items_made.root, edits,
					nedits, root);
	for (int i = 0; i < nedits; i++)
	{
		CanonItem  *it = changed[i];
		CanonItem  *m = made_item(&it->key);

		pfree(m->value.data);
		m->value.data = MemoryContextAlloc(items_cxt, it->value.len + 1);
		memcpy(m->value.data, it->value.data, it->value.len + 1);
		m->value.len = m->value.maxlen = it->value.len;
	}
	pfree(edits);
	pfree(changed);
	made_tree(&items_made, root);
	pgstat_count_prolly(PGSTAT_PROLLY_FOLD_INCREMENTAL, 1);

	/*
	 * (checked against a fold of everything but the tables this one
	 * folded, their rows taken from its items: every other table's rows,
	 * folded from the baseline again, must be as the last tree had them.
	 * Its work isn't counted: the statistics put back as they were, the
	 * change finders told.  In the same command, so what this fold
	 * consumed of the change log is still seen.)
	 */
	if (debug_prolly_verify_fold_caches)
	{
		Canon		full;
		PgStat_Counter *stats = palloc_array(PgStat_Counter, PGSTAT_PROLLY_NUM_COUNTERS);
		ProllyNodeCounters tree = *pgstat_prolly_tree_counters();

		memcpy(stats, PendingProllyStats, sizeof(PendingProllyStats));
		memset(&full, 0, sizeof(full));
		full.reading = cn->reading;
		canon_env(&full);
		full.finder = cn->finder;
		full.baseline = cn->baseline;
		full.cached = true;
		full.verifying = true;
		foreach_ptr(CanonItem, it, cn->items)
		{
			if (OidIsValid(it->relid) && strncmp(it->key.data, "s:", 2) != 0)
				full.given = lappend(full.given, it);
		}
		prolly_fold_counted = false;
		PG_TRY();
		{
			build_all(&full, &expect);
		}
		PG_FINALLY();
		{
			prolly_fold_counted = true;
		}
		PG_END_TRY();
		memcpy(PendingProllyStats, stats, sizeof(PendingProllyStats));
		*pgstat_prolly_tree_counters() = tree;
		pfree(stats);
		check_applied("canonical folded from the objects changed", root, &expect);
	}
	return true;
}

static int
defs_item_cmp(const void *a, const void *b)
{
	const CanonItem *x = a;
	const CanonItem *y = b;
	ProllyKey	kx = {(const uint8 *) x->key.data, x->key.len};
	ProllyKey	ky = {(const uint8 *) y->key.data, y->key.len};

	return prolly_bytes_compare(&kx, &ky, NULL);
}

/*
 * The definitions as the last read here, if nothing they're of changed
 * since (true): no catalog change since (this backend's catalog epoch,
 * which every invalidation moves, and every one sent is taken in as the
 * read begins), the same deparsing settings, the same baseline (each
 * table's rows are its), and the tree still in the store.  Rows written
 * since don't change them; sequences' values do, outside transactions, so
 * every sequence's item is worked out again and applied.  With
 * debug_prolly_verify_fold_caches, everything is read again and compared.
 */
static bool
definitions_unchanged(Canon *cn, ProllyNodeRef *root)
{
	ProllyFullFoldReason reason;
	ProllyEdit *edits;
	CanonItem **changed;
	int			nedits = 0;
	int			save_nestlevel;
	List	   *rels = NIL;

	if (!defs_valid || cn->baseline == NULL || OidIsValid(cn->only) ||
		defs_epoch != catalog_epoch || XactHasInvalidations() ||
		prolly_full_fold_wanted(&reason) ||
		defs_baseline.len != cn->baseline->hash.len ||
		memcmp(defs_baseline.bytes, cn->baseline->hash.bytes,
			   defs_baseline.len) != 0 ||
		!made_tree_usable(cn, &defs_made))
		return false;

	for (int i = 0; i < ndefs_sequences; i++)
	{
		HeapTuple	tup = SearchSysCacheCopy1(RELOID,
											  ObjectIdGetDatum(defs_sequences[i]));

		/* (gone: a catalog change, which the epoch would have seen) */
		if (!HeapTupleIsValid(tup))
			return false;
		rels = lappend(rels, tup);
	}
	cn->partial = true;
	save_nestlevel = NewGUCNestLevel();
	describing_settings();
	foreach_ptr(HeapTupleData, tup, rels)
		collect_relation(cn, (Form_pg_class) GETSTRUCT(tup));
	AtEOXact_GUC(false, save_nestlevel);

	list_sort(cn->items, item_cmp);
	edits = palloc_array(ProllyEdit, Max(list_length(cn->items), 1));
	changed = palloc_array(CanonItem *, Max(list_length(cn->items), 1));
	foreach_ptr(CanonItem, it, cn->items)
	{
		CanonItem	probe;
		CanonItem  *m;

		probe.key = it->key;
		m = bsearch(&probe, defs_items, ndefs_items, sizeof(CanonItem),
					defs_item_cmp);
		if (m == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("prolly definitions read again found an object the last read didn't hold")));
		if (m->value.len == it->value.len &&
			memcmp(m->value.data, it->value.data, it->value.len) == 0)
			continue;
		changed[nedits] = it;
		edits[nedits].key = (ProllyKey) {(const uint8 *) it->key.data, it->key.len};
		edits[nedits].value = (ProllyValue) {(const uint8 *) it->value.data, it->value.len};
		edits[nedits++].is_delete = false;
	}
	if (nedits == 0)
		*root = defs_made.root;
	else
		PROLLY_CALL(cn->tree, apply, &cn->root_env, &defs_made.root, edits,
					nedits, root);
	for (int i = 0; i < nedits; i++)
	{
		CanonItem	probe;
		CanonItem  *m;
		MemoryContext old;

		probe.key = changed[i]->key;
		m = bsearch(&probe, defs_items, ndefs_items, sizeof(CanonItem),
					defs_item_cmp);
		old = MemoryContextSwitchTo(defs_cxt);
		resetStringInfo(&m->value);
		appendBinaryStringInfo(&m->value, changed[i]->value.data,
							   changed[i]->value.len);
		MemoryContextSwitchTo(old);
	}
	pfree(edits);
	pfree(changed);
	if (nedits > 0)
		made_tree(&defs_made, root);
	cn->left_out = NIL;
	foreach_ptr(char, name, defs_left_out)
		cn->left_out = lappend(cn->left_out, pstrdup(name));
	pgstat_count_prolly(PGSTAT_PROLLY_DEFINITIONS_REUSED, 1);

	/* (checked against reading them all again, its work not counted) */
	if (debug_prolly_verify_fold_caches)
	{
		Canon		full;
		ProllyNodeRef expect;
		ListCell   *a;
		ListCell   *b;
		PgStat_Counter *stats = palloc_array(PgStat_Counter, PGSTAT_PROLLY_NUM_COUNTERS);
		ProllyNodeCounters tree = *pgstat_prolly_tree_counters();

		memcpy(stats, PendingProllyStats, sizeof(PendingProllyStats));
		memset(&full, 0, sizeof(full));
		full.reading = cn->reading;
		full.no_rows = true;
		canon_env(&full);
		full.finder = cn->finder;
		full.baseline = cn->baseline;
		full.cached = true;
		full.verifying = true;
		prolly_fold_counted = false;
		PG_TRY();
		{
			build_all(&full, &expect);
		}
		PG_FINALLY();
		{
			prolly_fold_counted = true;
		}
		PG_END_TRY();
		memcpy(PendingProllyStats, stats, sizeof(PendingProllyStats));
		*pgstat_prolly_tree_counters() = tree;
		pfree(stats);
		check_applied("definitions read again", root, &expect);
		/* (and what they left out, the same objects) */
		if (list_length(full.left_out) != list_length(cn->left_out))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("prolly definitions taken from this backend's last read leave out %d objects, read again %d",
							list_length(cn->left_out), list_length(full.left_out))));
		forboth(a, full.left_out, b, cn->left_out)
		{
			if (strcmp(lfirst(a), lfirst(b)) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("prolly definitions taken from this backend's last read leave out \"%s\", read again \"%s\"",
								(char *) lfirst(b), (char *) lfirst(a))));
		}
	}
	return true;
}

/*
 * The canonical tree: the last one built here with only what changed
 * since worked out again, if it can be (fold_changed_only()), else
 * everything.  A fold of everything that left nothing out is marked for
 * the next.
 */
static void
build_canonical(Canon *cn, ProllyNodeRef *root)
{
	ProllyDatabaseComponents dc;

	/*
	 * Deparse with every name qualified, whatever the session's settings
	 * (describing_settings()); tables described as before taken from the
	 * cache (#483).
	 */
	fold_caches_begin();
	cn->cached = true;

	/* (marked before anything is read) */
	prolly_database_components(&dc);
	cn->at.catalog_epoch = catalog_epoch;
	cn->marked = !OidIsValid(cn->only) && !cn->no_rows &&
		PROLLY_HAS(dc.refs, fold_mark) &&
		PROLLY_CALL(dc.refs, fold_mark, MyBranch, GetActiveSnapshot(),
					&cn->at.mark);
	cn->at.valid = cn->marked;
	if (cn->no_rows)
	{
		/* (definitions: the folds' items, and what they're as of, kept) */
		if (!definitions_unchanged(cn, root))
			build_all(cn, root);
		return;
	}
	if (!fold_changed_only(cn, root))
		build_all(cn, root);
	if (cn->left_out != NIL)
		cn->at.valid = false;
	items_at = cn->at;
}

/*
 * The next baseline: each table's entry, as a tree by key; the baseline
 * folded from (NULL: none) with only the entries that differ applied, if
 * there was one (#483).
 */
static void
build_baseline(Canon *cn, const ProllyNodeRef *baseline, ProllyNodeRef *next)
{
	ProllyTreeBuilder *b;
	ProllyTreeEnv benv = cn->env;
	ListCell   *lc;

	list_sort(cn->next, item_cmp);
	benv.chunk_refs = base_chunk_refs;
	if (baseline)
	{
		ProllyEdit *edits = palloc_array(ProllyEdit, cn->nbase + list_length(cn->next));
		int			nedits = 0;
		int			i = 0;

		lc = list_head(cn->next);
		while (i < cn->nbase || lc != NULL)
		{
			BaseTable  *was = i < cn->nbase ? &cn->base[i] : NULL;
			CanonItem  *is = lc ? lfirst(lc) : NULL;
			ProllyKey	kw = {0};
			ProllyKey	ki = {0};
			StringInfoData value;
			int			c;

			if (was)
				kw = (ProllyKey) {(const uint8 *) was->key.data, was->key.len};
			if (is)
				ki = (ProllyKey) {(const uint8 *) is->key.data, is->key.len};
			c = !was ? 1 : !is ? -1 : prolly_bytes_compare(&kw, &ki, NULL);
			if (c == 0)
			{
				/* (its value as parse_base_table() read it) */
				initStringInfo(&value);
				put_hash(&value, &was->data.hash);
				put_base_kept_shape(&value, was);
				appendBinaryStringInfo(&value, was->state.data, was->state.len);
			}
			if (c < 0)
			{
				edits[nedits].key = kw;
				edits[nedits++].is_delete = true;
			}
			else if (c > 0 || value.len != is->value.len ||
					 memcmp(value.data, is->value.data, value.len) != 0)
			{
				edits[nedits].key = ki;
				edits[nedits].value = (ProllyValue) {(const uint8 *) is->value.data,
				is->value.len};
				edits[nedits++].is_delete = false;
			}
			if (c <= 0)
				i++;
			if (c >= 0)
				lc = lnext(cn->next, lc);
		}
		if (nedits == 0)
			*next = *baseline;
		else
			PROLLY_CALL(cn->tree, apply, &benv, baseline, edits, nedits, next);
		pfree(edits);
		/* (partial: its entries are only those of the tables written) */
		if (!debug_prolly_verify_fold_caches || cn->partial)
			return;
	}
	Assert(!cn->partial);
	b = PROLLY_CALL(cn->tree, build_begin, &benv, CurrentMemoryContext);
	foreach(lc, cn->next)
	{
		CanonItem  *it = lfirst(lc);
		ProllyKey	k = {(const uint8 *) it->key.data, it->key.len};
		ProllyValue v = {(const uint8 *) it->value.data, it->value.len};

		PROLLY_CALL(cn->tree, build_add, b, &k, &v);
	}
	if (baseline)
	{
		ProllyNodeRef built;

		PROLLY_CALL(cn->tree, build_finish, b, &built);
		check_applied("baseline", next, &built);
	}
	else
		PROLLY_CALL(cn->tree, build_finish, b, next);
}

static List *
fold_canon(const ProllyNodeRef *baseline, ProllyNodeRef *root,
		   ProllyNodeRef *next_baseline, bool reading, Oid only, bool no_rows)
{
	Canon		cn;

	/* (a baseline holds every table's state: never made from one alone) */
	Assert((!OidIsValid(only) && !no_rows) || next_baseline == NULL);
	memset(&cn, 0, sizeof(cn));
	cn.reading = reading;
	cn.only = only;
	cn.no_rows = no_rows;
	canon_env(&cn);
	cn.finder = prolly_change_finder();
	cn.advance = next_baseline != NULL;
	cn.baseline = baseline;

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
	{
		build_baseline(&cn, baseline, next_baseline);
		made_tree(&baseline_made, next_baseline);
		baseline_at = cn.at;
	}
	return cn.left_out;
}

static void
folded_fold(const ProllyNodeRef *baseline, ProllyNodeRef *root,
			ProllyNodeRef *next_baseline, bool commit)
{
	(void) fold_canon(baseline, root, next_baseline, !commit, InvalidOid,
					  false);
}

/*
 * The session branch's tables whose next fold from baseline would read
 * them in full, as table_root() would find (no entry, a definition its
 * rows can't be mapped from) and then the change finder (full_reason()).
 * The catalogs are read as a fold reads them, by a snapshot of now.
 */
static void
folded_pending_full(const ProllyNodeRef *baseline,
					ProllyPendingFullVisit visit, void *arg)
{
	Canon		cn;
	Relation	classes;
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *relids = NIL;

	memset(&cn, 0, sizeof(cn));
	cn.reading = true;
	canon_env(&cn);
	cn.finder = prolly_change_finder();
	if (baseline)
		load_baseline(&cn, baseline);
	if (ActiveSnapshotSet())
		PushCopiedSnapshot(GetActiveSnapshot());
	else
		PushActiveSnapshot(GetTransactionSnapshot());
	PG_TRY();
	{
		classes = table_open(RelationRelationId, AccessShareLock);
		scan = catalog_branch_beginscan(classes, MyBranch);
		while ((tup = systable_getnext(scan)) != NULL)
		{
			Form_pg_class form = (Form_pg_class) GETSTRUCT(tup);

			if (form->relkind == RELKIND_RELATION &&
				form->relam == PROLLY_TABLE_AM_OID &&
				canon_namespace(form->relnamespace) &&
				!in_extension(RelationRelationId, form->oid))
				relids = lappend_oid(relids, form->oid);
		}
		systable_endscan(scan);
		table_close(classes, AccessShareLock);

		foreach_oid(relid, relids)
		{
			Relation	rel = try_table_open(relid, AccessShareLock);
			char	   *name;
			StringInfoData key;
			BaseTable  *bt;
			StringInfoData desc;
			StringInfoData shape = {0};
			ProllyHash	dh;
			ProllyTableFold tf;
			ProllyFullFoldReason why = PROLLY_FULL_NONE;

			if (rel == NULL)
				continue;
			name = qualified(RelationGetNamespace(rel),
							 RelationGetRelationName(rel));
			initStringInfo(&key);
			appendStringInfoString(&key, "t:");
			append_utf8(&key, name);
			initStringInfo(&desc);
			table_description(&cn, rel, &desc, &dh);
			bt = find_base_table(&cn, &key);
			memset(&tf, 0, sizeof(tf));
			if (prolly_full_fold_wanted(&why))
				;
			else if (bt == NULL)
				why = PROLLY_FULL_NO_BASELINE;
			else if (!(bt->shape_hash.len == dh.len &&
					   memcmp(bt->shape_hash.bytes, dh.bytes, dh.len) == 0) &&
					 !shape_holds(rel, base_shape(&cn, bt),
								  table_shape_bytes(&cn, rel, &desc, &shape)) &&
					 !shape_will_map(&cn, rel, bt, &shape, &tf.shape))
				why = PROLLY_FULL_SHAPE_CHANGED;
			else if (PROLLY_HAS(cn.finder, full_reason))
			{
				tf.data = bt->data;
				tf.state.data = (const uint8 *) bt->state.data;
				tf.state.len = bt->state.len;
				tf.reshaped = tf.shape.data != NULL;
				why = cn.finder->full_reason(rel, &tf);
			}
			if (why != PROLLY_FULL_NONE)
				visit(name, relid, why, arg);
			pfree(key.data);
			pfree(desc.data);
			table_close(rel, AccessShareLock);
		}
	}
	PG_FINALLY();
	{
		PopActiveSnapshot();
	}
	PG_END_TRY();
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
	List	   *l = fold_canon(baseline, root, NULL, true, only, false);

	if (left_out)
		*left_out = list_concat(*left_out, l);
}

static void
folded_read_definitions(const ProllyNodeRef *baseline, ProllyNodeRef *root,
						List **left_out)
{
	List	   *l = fold_canon(baseline, root, NULL, true, InvalidOid, true);

	if (left_out)
		*left_out = list_concat(*left_out, l);
}

/*
 * What table_changes() and table_fewer_changes() ask of the change finder:
 * the changes visited, or (limit > 0) whether fewer than limit changed.
 */
typedef struct TableChangesAsk
{
	ProllyRowChangeVisit visit;
	void	   *arg;
	int			limit;
} TableChangesAsk;

/* A change finder's walk of one table's changes. */
typedef struct ChangesRun
{
	Canon	   *cn;
	Relation	rel;
	ProllyTableFold tf;
	const TableChangesAsk *ask;
	bool		ok;
} ChangesRun;

static void
changes_run(void *arg)
{
	ChangesRun *run = arg;

	run->ok = run->ask->limit > 0 ?
		run->cn->finder->fewer_changes(run->rel, &run->tf, run->ask->limit) :
		run->cn->finder->changes(run->rel, &run->cn->env, &run->tf,
								 run->ask->visit, run->ask->arg);
}

/*
 * Table relid's rows that differ from the baseline's, read by the change
 * finder from what it recorded, as table_root() would fold them: only for
 * a table collect() describes as a table and reads (not a partition, nor
 * one with columns of types that hold OIDs), or a prolly materialized
 * view, whose baseline is of its definition now.
 */
static bool
table_changes_asked(const ProllyNodeRef *baseline, Oid relid,
					ProllyNodeRef *data, const TableChangesAsk *ask)
{
	Canon		cn;
	Relation	rel;
	Form_pg_class form;
	bool		ok = false;

	memset(&cn, 0, sizeof(cn));
	cn.reading = true;
	canon_env(&cn);
	cn.finder = prolly_change_finder();
	if (ask->limit > 0 ? !PROLLY_HAS(cn.finder, fewer_changes) :
		!PROLLY_HAS(cn.finder, changes))
		return false;

	/* (the fold's snapshot: the transaction's writes, this statement's too) */
	if (!IsInParallelMode())
		CommandCounterIncrement();
	if (ActiveSnapshotSet())
		PushCopiedSnapshot(GetActiveSnapshot());
	else
		PushActiveSnapshot(GetTransactionSnapshot());
	if (!IsInParallelMode())
		UpdateActiveSnapshotCommandId();
	rel = table_open(relid, AccessShareLock);
	PG_TRY();
	{
		form = rel->rd_rel;
		if ((form->relkind == RELKIND_RELATION ||
			 form->relkind == RELKIND_MATVIEW) &&
			form->relam == PROLLY_TABLE_AM_OID && !form->relispartition &&
			canon_namespace(form->relnamespace) &&
			!in_extension(RelationRelationId, relid) &&
			!oid_columns(&cn, rel))
		{
			StringInfoData key;
			StringInfoData desc;
			ProllyHash	dh;
			BaseTable  *bt;
			int			save_nestlevel;
			bool		matview = form->relkind == RELKIND_MATVIEW;

			initStringInfo(&key);
			appendStringInfoString(&key, matview ? "m:" : "t:");
			append_utf8(&key, qualified(form->relnamespace,
										NameStr(form->relname)));
			/* (the table's entry alone: a status asks each table so) */
			load_baseline_keys(&cn, baseline, list_make1(&key));
			/* (described as collect() describes it: see build_canonical()) */
			fold_caches_begin();
			cn.cached = true;
			save_nestlevel = NewGUCNestLevel();
			describing_settings();
			initStringInfo(&desc);
			if (matview)
			{
				/* (as collect_relation() describes one) */
				append_matview_state(&desc, form);
				append_utf8(&desc,
							TextDatumGetCString(DirectFunctionCall2(pg_get_viewdef_ext,
																	ObjectIdGetDatum(relid),
																	BoolGetDatum(false))));
				table_shape(&cn, rel, &desc, &dh, NULL);
			}
			else
				table_description(&cn, rel, &desc, &dh);

			/*
			 * (its rows folded under the shape they have now, as table_root()
			 * takes a baseline, not one it would map)
			 */
			bt = find_base_table(&cn, &key);
			if (bt && !(bt->shape_hash.len == dh.len &&
						memcmp(bt->shape_hash.bytes, dh.bytes, dh.len) == 0))
			{
				StringInfoData shape = {0};

				if (!shape_holds(rel, base_shape(&cn, bt),
								 table_shape_bytes(&cn, rel, &desc, &shape)))
					bt = NULL;
			}
			AtEOXact_GUC(false, save_nestlevel);
			if (bt)
			{
				ChangesRun	run;

				memset(&run, 0, sizeof(run));
				run.tf.data = bt->data;
				run.tf.state.data = (const uint8 *) bt->state.data;
				run.tf.state.len = bt->state.len;
				run.cn = &cn;
				run.rel = rel;
				run.ask = ask;
				*data = bt->data;
				/* (the rows' values encoded with the text settings fixed once) */
				prolly_datum_fixed_text(changes_run, &run);
				ok = run.ok;
			}
		}
	}
	PG_FINALLY();
	{
		PopActiveSnapshot();
	}
	PG_END_TRY();
	table_close(rel, AccessShareLock);
	return ok;
}

static bool
folded_table_changes(const ProllyNodeRef *baseline, Oid relid,
					 ProllyNodeRef *data, ProllyRowChangeVisit visit,
					 void *arg)
{
	TableChangesAsk ask = {visit, arg, 0};

	return table_changes_asked(baseline, relid, data, &ask);
}

static bool
folded_table_fewer_changes(const ProllyNodeRef *baseline, Oid relid,
						   int limit, ProllyNodeRef *data)
{
	TableChangesAsk ask = {NULL, NULL, Max(limit, 1)};

	return table_changes_asked(baseline, relid, data, &ask);
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
		ProllyValue shape_ref = {(const uint8 *) cn.base[i].shape_ref.data,
		cn.base[i].shape_ref.len};

		PROLLY_CALL(cn.tree, walk, &cn.env, &cn.base[i].data, visit, arg);
		if (shape_ref.len > 0)
		{
			ProllyNodeRef shape;
			uint64		len;

			memset(&shape, 0, sizeof(shape));
			PROLLY_CALL(cn.tree, value_root, &shape_ref, &shape.hash, &len);
			PROLLY_CALL(cn.tree, walk, &cn.env, &shape, visit, arg);
		}
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

static bool
note_changed_tree(ProllyDiffKind kind, const ProllyKey *key,
				  const ProllyValue *before, const ProllyValue *after, void *arg)
{
	List	  **changed = arg;
	ChangedTree *c;
	ProllyValue def;

	if (after == NULL)
		return true;
	c = palloc0(sizeof(ChangedTree));
	if (!tree_item(key, after, &c->after, &def))
	{
		pfree(c);
		return true;
	}
	c->was = before != NULL && tree_item(key, before, &c->before, &def);
	*changed = lappend(*changed, c);
	return true;
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

/* Whether a table's recorded ordering extensions are still installed. */
static bool
ordering_current(const char *literal)
{
	Datum	   *values;
	bool	   *nulls;
	int			n;

	deconstruct_array_builtin(DatumGetArrayTypeP(OidInputFunctionCall(F_ARRAY_IN,
																	  (char *) literal, TEXTOID, -1)),
							  TEXTOID, &values, &nulls, &n);
	if (n == 0 || n % 2)
		elog(ERROR, "invalid historical ordering metadata");
	for (int i = 0; i < n; i += 2)
	{
		Oid			ext;

		if (nulls[i] || nulls[i + 1])
			elog(ERROR, "invalid historical ordering metadata");
		ext = get_extension_oid(TextDatumGetCString(values[i]), true);
		if (!OidIsValid(ext) ||
			strcmp(extension_version(ext), TextDatumGetCString(values[i + 1])) != 0)
			return false;
	}
	return true;
}

static int
ordering_compare(const ProllyKey *a, const ProllyKey *b, void *arg)
{
	ProllyKeyingRefs *refs = arg;

	return PROLLY_CALL(refs->routine, compare, refs->keying, a, b);
}

/*
 * A changed extension needn't make unrelated tables unreadable.  If this
 * table's row layout is unchanged, sort its encoded keys under the installed
 * comparator before anyone seeks or diffs them.  No stored commit is changed.
 * A simultaneous schema/type change is refused: its old codec is not proven
 * compatible with the current comparator.
 */
static const char *
read_ordering(Canon *cn, const ProllyNodeRef *root, const char *name,
			  const ProllyValue *description, ProllyNodeRef *data)
{
	StringInfoData value;
	char	   *keyname = psprintf("%s%s", ORDERING_PREFIX, name);
	ProllyKey	key = {(const uint8 *) keyname, strlen(keyname)};
	char	   *recorded;
	char	   *versions;
	char	   *server_name;
	Oid			relid;
	Relation	rel;
	StringInfoData desc;
	bool		compatible = false;
	bool		unique = true;
	int			nestlevel;

	initStringInfo(&value);
	if (!PROLLY_CALL(cn->tree, get, &cn->env, root, &key, &value))
		return NULL;
	recorded = pg_any_to_server(value.data, value.len, PG_UTF8);
	versions = strchr(recorded, '\n');
	if (versions == NULL)
		elog(ERROR, "invalid historical ordering metadata");
	*versions++ = '\0';
	if (ordering_current(versions))
		return NULL;
	server_name = pg_any_to_server(name, strlen(name), PG_UTF8);
	relid = RangeVarGetRelid(makeRangeVarFromNameList(stringToQualifiedNameList(server_name, NULL)),
							 AccessShareLock, true);
	if (!OidIsValid(relid) || get_rel_relkind(relid) != RELKIND_RELATION)
		return "ordering extension changed and the historical table has no compatible current definition";
	rel = table_open(relid, NoLock);
	initStringInfo(&desc);
	nestlevel = NewGUCNestLevel();
	describing_settings();
	PG_TRY();
	{
		describe_table(cn, rel, &desc);
		compatible = desc.len == description->len &&
			memcmp(desc.data, description->data, desc.len) == 0 &&
			strcmp(recorded, ordering_layout(cn, rel, &desc)) == 0;
	}
	PG_FINALLY();
	{
		AtEOXact_GUC(false, nestlevel);
	}
	PG_END_TRY();
	if (compatible)
	{
		ProllyComponents components;
		ProllyKeying *k;
		ProllyKeyingRefs refs;
		ProllyTreeEnv env = cn->env;

		prolly_components_for_relation(rel, &components);
		k = PROLLY_CALL(components.keying, open, rel, CurrentMemoryContext);
		env.compare = ordering_compare;
		env.compare_arg = &refs;
		prolly_keying_use_env(&refs, components.keying, k, cn->tree, &env);
		unique = prolly_fold_reorder(components.keying, k, cn->tree, &env, data, data);
		PROLLY_CALL(components.keying, close, k);
	}
	table_close(rel, AccessShareLock);
	if (!unique)
		return "ordering extension now considers distinct historical primary keys equal";
	return compatible ? NULL :
		"ordering extension changed together with the historical table's definition or row types";
}

static void
folded_trees(const ProllyNodeRef *root, char kind, const char *only,
			 ProllyCanonTableVisit visit, void *arg)
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
	if (only)
	{
		char *seek = psprintf("%c:%s", kind,
			pg_server_to_any(only, strlen(only), PG_UTF8));

		from.data = (const uint8 *) seek;
		from.len = strlen(seek);
	}
	c = PROLLY_CALL(cn.tree, cursor_open, &cn.env, root, CurrentMemoryContext);
	PROLLY_CALL(cn.tree, cursor_seek, c, &from, ForwardScanDirection);
	while (PROLLY_CALL(cn.tree, cursor_next, c, &k, &v))
	{
		ProllyNodeRef data;
		ProllyValue description;
		char	   *name;
		const char *unreadable = NULL;

		if (k.len < 2 || memcmp(k.data, prefix, 2) != 0)
			break;
		if (only && (k.len != from.len || memcmp(k.data, from.data, from.len) != 0))
			break;
		(void) tree_item(&k, &v, &data, &description);
		name = pnstrdup((const char *) k.data + 2, k.len - 2);
		if (kind == 't')
		{
			unreadable = !current ? PROLLY_UNREADABLE_OLDER_FORMAT :
				table_unreadable(pg_any_to_server(name, strlen(name), PG_UTF8), stale);
			if (unreadable == NULL)
				unreadable = read_ordering(&cn, root, name, &description, &data);
		}
		visit(name, &data, &description, unreadable, arg);
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
folded_tables(const ProllyNodeRef *root, const char *name,
			  ProllyCanonTableVisit visit, void *arg)
{
	folded_trees(root, 't', name, visit, arg);
}

/* Its materialized views: its "m:" items. */
static void
folded_materialized_views(const ProllyNodeRef *root,
						  ProllyCanonTableVisit visit, void *arg)
{
	folded_trees(root, 'm', NULL, visit, arg);
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

/* Ordering metadata is not schema and must not create schema conflicts. */
static bool
next_schema_item(Canon *cn, ProllyTreeCursor *c, ProllyKey *k, ProllyValue *v)
{
	while (PROLLY_CALL(cn->tree, cursor_next, c, k, v))
		if (!(k->len >= strlen(ORDERING_PREFIX) &&
			  memcmp(k->data, ORDERING_PREFIX, strlen(ORDERING_PREFIX)) == 0))
			return true;
	return false;
}

/* Objects exclude ordering metadata and the recorded collation versions. */
static bool
next_object(Canon *cn, ProllyTreeCursor *c, ProllyKey *k, ProllyValue *v)
{
	while (PROLLY_CALL(cn->tree, cursor_next, c, k, v))
		if (!(k->len >= 2 && k->data[0] == 'c' && k->data[1] == ':') &&
			!(k->len >= strlen(ORDERING_PREFIX) &&
			  memcmp(k->data, ORDERING_PREFIX, strlen(ORDERING_PREFIX)) == 0))
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

/* What objects_diffed() visits, as the diff of two roots gives it. */
typedef struct DiffedWalk
{
	ObjectsWalk w;
	ProllyCanonObjectVisit table;
} DiffedWalk;

static bool
diffed_change(ProllyDiffKind kind, const ProllyKey *key,
			  const ProllyValue *before, const ProllyValue *after, void *arg)
{
	DiffedWalk *dw = arg;
	char	   *name;

	/* (what next_item() skips: the format, ordering, collations) */
	if (key->len < 2 || key->data[1] != ':' || key->data[0] == '!' ||
		key->data[0] == 'c')
		return true;
	if (key->data[0] == PROLLY_OBJECT_TABLE)
	{
		if (dw->table == NULL)
			return true;
		name = from_utf8(key->data + 2, key->len - 2);
		dw->table(name, before == NULL ? PROLLY_DIFF_ADDED :
				  after == NULL ? PROLLY_DIFF_REMOVED : PROLLY_DIFF_MODIFIED,
				  PROLLY_OBJECT_TABLE, dw->w.arg);
		pfree(name);
		return true;
	}
	(void) object_pair(before ? key : NULL, before, after ? key : NULL, after,
					   &dw->w);
	return true;
}

/*
 * objects() read from the roots' tree diff: an object whose items are the
 * same in both is one objects() doesn't visit either, and the diff gives
 * the rest in key order, as objects()'s merge of every item does.
 */
static void
folded_objects_diffed(const ProllyNodeRef *from, const ProllyNodeRef *to,
					  ProllyCanonObjectVisit visit,
					  ProllyCanonObjectVisit table, void *arg)
{
	Canon		cn;
	DiffedWalk	dw = {{visit, arg}, table};

	memset(&cn, 0, sizeof(cn));
	canon_env(&cn);
	dw.w.same_format = root_in_format(&cn, from) == root_in_format(&cn, to);
	PROLLY_CALL(cn.tree, diff, &cn.env, from, to, diffed_change, &dw);
}

/*
 * root with its sequences' values read again, as describe_sequence()
 * appends them to each one's definition: a sequence's item is its
 * definition's line, then its value's (none where it can't be read).
 */
static bool
folded_sequences_now(const ProllyNodeRef *root, ProllyNodeRef *out)
{
	Canon		cn;
	ProllyTreeCursor *c;
	ProllyKey	k;
	ProllyValue v;
	ProllyKey	from = {(const uint8 *) "s:", 2};
	List	   *edits = NIL;
	bool		ok = true;

	memset(&cn, 0, sizeof(cn));
	canon_env(&cn);
	c = PROLLY_CALL(cn.tree, cursor_open, &cn.env, root, CurrentMemoryContext);
	PROLLY_CALL(cn.tree, cursor_seek, c, &from, ForwardScanDirection);
	while (ok && PROLLY_CALL(cn.tree, cursor_next, c, &k, &v))
	{
		char	   *name;
		List	   *parts;
		Oid			nsp;
		Oid			seqid = InvalidOid;
		const char *nl;
		StringInfoData value;
		int64		last;
		bool		called;

		if (k.len < 2 || memcmp(k.data, "s:", 2) != 0)
			break;
		name = from_utf8(k.data + 2, k.len - 2);
		parts = stringToQualifiedNameList(name, NULL);
		if (list_length(parts) == 2 &&
			OidIsValid(nsp = get_namespace_oid(strVal(linitial(parts)), true)))
			seqid = get_relname_relid(strVal(lsecond(parts)), nsp);
		nl = memchr(v.data, '\n', v.len);
		if (!OidIsValid(seqid) || get_rel_relkind(seqid) != RELKIND_SEQUENCE ||
			nl == NULL)
		{
			ok = false;
			break;
		}
		initStringInfo(&value);
		appendBinaryStringInfo(&value, (const char *) v.data,
							   (int) (nl - (const char *) v.data) + 1);
		if (ReadSequenceState(seqid, &last, &called))
			appendStringInfo(&value, INT64_FORMAT "\t%s\n", last,
							 called ? "t" : "f");
		if (value.len != v.len || memcmp(value.data, v.data, v.len) != 0)
		{
			ProllyEdit *e = palloc(sizeof(ProllyEdit));
			uint8	   *kd = palloc(k.len);

			memcpy(kd, k.data, k.len);
			e->key = (ProllyKey) {kd, k.len};
			e->value = (ProllyValue) {(const uint8 *) value.data, value.len};
			e->is_delete = false;
			edits = lappend(edits, e);
		}
	}
	PROLLY_CALL(cn.tree, cursor_close, c);
	if (!ok)
		return false;
	if (edits == NIL)
		*out = *root;
	else
	{
		ProllyEdit *all = palloc_array(ProllyEdit, list_length(edits));
		int			n = 0;

		foreach_ptr(ProllyEdit, e, edits)
			all[n++] = *e;
		PROLLY_CALL(cn.tree, apply, &cn.root_env, root, all, n, out);
	}
	return true;
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

static bool
note_change(ProllyDiffKind kind, const ProllyKey *key, const ProllyValue *before,
			const ProllyValue *after, void *arg)
{
	ChangesWalk *w = arg;
	Changed    *c;
	char	   *name;

	/* (the format, and collations, aren't objects) */
	if (key->len < 2 || key->data[1] != ':' || key->data[0] == '!' ||
		key->data[0] == 'c')
		return true;
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
			return true;
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
	return true;
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
 * diff gives (equal subtrees skipped); a table renames says was renamed
 * (by identity, which a rename keeps), dropped by one name and added by
 * another, made one renamed, its rows and definition perhaps changed too.
 * Other objects (views, routines), which have no identity, of a kind
 * dropped and added with the same item (but for their own names), and the
 * only such pair, are made one renamed; tables never are, so that
 * unrelated tables that happen to be alike aren't.
 */
static void folded_render(char object, const char *name, const ProllyValue *item,
						  StringInfo out);

static Changed *
dropped_or_added(List *changed, const char *name, bool dropped)
{
	ListCell   *lc;

	foreach(lc, changed)
	{
		Changed    *c = lfirst(lc);

		if (c->object == PROLLY_OBJECT_TABLE &&
			(dropped ? c->from_name && !c->to_name &&
			 strcmp(c->from_name, name) == 0 :
			 c->to_name && !c->from_name && strcmp(c->to_name, name) == 0))
			return c;
	}
	return NULL;
}

/*
 * One side's part of a changed object, the other's NULL: dropped or added,
 * as note_change() notes one.
 */
static void
one_side(Changed *c, const ProllyValue *v, const char *name)
{
	StringInfoData key;
	ProllyKey	k;

	initStringInfo(&key);
	appendStringInfoChar(&key, c->object);
	appendStringInfoChar(&key, ':');
	append_utf8(&key, name);
	k.data = (const uint8 *) key.data;
	k.len = key.len;
	c->data_change = holds_data(&k, v);
	c->schema_change = true;
	c->masked = masked_item(v, name);
	c->content = hash_bytes_extended(c->masked.data, c->masked.len,
									 (uint64) c->object);
}

/*
 * A table under name in both roots, changed or not, made a drop of from's
 * and an add of to's: they are other tables, one renamed away from the
 * name or to it.  False if both haven't one.
 */
static bool
split_changed(Canon *cn, ChangesWalk *w, const ProllyNodeRef *from,
			  const ProllyNodeRef *to, const char *name)
{
	ListCell   *lc;
	StringInfoData key;
	StringInfoData fv;
	StringInfoData tv;
	ProllyKey	k;
	Changed    *d;
	Changed    *a;
	bool		split;

	foreach(lc, w->changed)
	{
		Changed    *c = lfirst(lc);

		if (c->object != PROLLY_OBJECT_TABLE || !c->from_name || !c->to_name ||
			strcmp(c->from_name, name) != 0 || strcmp(c->to_name, name) != 0)
			continue;
		a = palloc0(sizeof(Changed));
		a->object = c->object;
		a->to_name = c->to_name;
		a->to = c->to;
		one_side(a, &a->to, name);
		c->to_name = NULL;
		one_side(c, &c->from, name);
		w->changed = lappend(w->changed, a);
		return true;
	}

	/*
	 * (the same in both, so not among the changes the diff gave: the two
	 * are still others, one renamed away and another made with its name)
	 */
	initStringInfo(&key);
	appendStringInfoString(&key, "t:");
	append_utf8(&key, name);
	k.data = (const uint8 *) key.data;
	k.len = key.len;
	initStringInfo(&fv);
	initStringInfo(&tv);
	split = PROLLY_CALL(cn->tree, get, &cn->env, from, &k, &fv) &&
		PROLLY_CALL(cn->tree, get, &cn->env, to, &k, &tv);
	if (split)
	{
		d = palloc0(sizeof(Changed));
		d->object = PROLLY_OBJECT_TABLE;
		d->from_name = pstrdup(name);
		d->from.data = (const uint8 *) fv.data;
		d->from.len = fv.len;
		one_side(d, &d->from, name);
		a = palloc0(sizeof(Changed));
		a->object = PROLLY_OBJECT_TABLE;
		a->to_name = pstrdup(name);
		a->to.data = (const uint8 *) tv.data;
		a->to.len = tv.len;
		one_side(a, &a->to, name);
		w->changed = lappend(lappend(w->changed, d), a);
	}
	return split;
}

static void
folded_changes(const ProllyNodeRef *from, const ProllyNodeRef *to,
			   List *renames, ProllyCanonChangeVisit visit, void *arg)
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

	/* tables renamed, by identity */
	foreach(lc, renames)
	{
		ProllyRename *r = lfirst(lc);
		Changed    *d;
		Changed    *a;
		ProllyNodeRef rd;
		ProllyNodeRef ra;
		ProllyValue def;
		StringInfoData key;
		ProllyKey	k;

		if (r->column != NULL)
			continue;
		/* (a name another table has since, or had before: that one apart) */
		if (dropped_or_added(w.changed, r->table, true) == NULL)
			(void) split_changed(&cn, &w, from, to, r->table);
		if (dropped_or_added(w.changed, r->to, false) == NULL)
			(void) split_changed(&cn, &w, from, to, r->to);
		if ((d = dropped_or_added(w.changed, r->table, true)) == NULL ||
			(a = dropped_or_added(w.changed, r->to, false)) == NULL)
			continue;
		initStringInfo(&key);
		appendStringInfoString(&key, "t:");
		k.data = (const uint8 *) key.data;
		k.len = key.len;
		(void) tree_item(&k, &d->from, &rd, &def);
		(void) tree_item(&k, &a->to, &ra, &def);
		d->to_name = a->to_name;
		d->to = a->to;
		d->data_change = !(rd.hash.len == ra.hash.len &&
						   memcmp(rd.hash.bytes, ra.hash.bytes, rd.hash.len) == 0);
		d->schema_change = true;
		w.changed = list_delete_ptr(w.changed, a);
	}
	/*
	 * renames: a dropped and an added object of one content, alone (not a
	 * table, where identities tell renames)
	 */
	ctl.keysize = sizeof(uint64);
	ctl.entrysize = sizeof(ContentMatch);
	ctl.hcxt = CurrentMemoryContext;
	matches = hash_create("prolly renames", 64, &ctl,
						  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	foreach(lc, w.changed)
	{
		Changed    *c = lfirst(lc);
		bool		found;

		if ((c->from_name && c->to_name) || c->object == PROLLY_OBJECT_TABLE)
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
	List	   *at_once;		/* of emptied, those the ALTER changing a
								 * column's type rewrites anyway: emptied
								 * as TRUNCATE empties them */
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

		if (k.len < 2 || k.data[1] != ':' || k.data[0] == 'c' ||
			(k.len >= strlen(ORDERING_PREFIX) &&
			 memcmp(k.data, ORDERING_PREFIX, strlen(ORDERING_PREFIX)) == 0))
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
 * The tables a table as description describes it inherits from, or is a
 * partition of (qualified and quoted).
 */
static List *
folded_table_parents(const ProllyValue *description)
{
	TableDef	td;
	List	   *out = NIL;
	ListCell   *lc;

	parse_table_def(from_utf8(description->data, description->len), &td);
	if (td.partition_of)
		out = lappend(out, first_field(td.partition_of));
	foreach(lc, td.inherits)
		out = lappend(out, first_field(lfirst(lc)));
	return out;
}

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

List	   *prolly_move_provenance = NIL;
List	   *prolly_move_kept = NIL;

/*
 * A table a move makes, or makes again: its name, and the relation (OID)
 * it is to be (prolly_move_kept_relid()).
 */
typedef struct MoveRelid
{
	char	   *name;
	Oid			relid;
	Oid			owner;			/* the role it is made as (InvalidOid: the
								 * move's) */
} MoveRelid;

/*
 * A relation the running move makes, re-makes or changes, as the role that
 * owns it (prolly_move_maker()): one it makes from a commit, as the owner
 * the commit's record names (made), and one the branch has, as its owner
 * here (not made): a table ('t'), a materialized view ('m'), a view ('v')
 * or a sequence ('S' one a table made so owns, 's' one the branch has), by
 * name, and its OID once looked up.  One the branch has keeps its grants
 * where the move makes it again (saved: its ACLs, each a SavedAcl *).
 */
typedef struct MoveMaker
{
	char	   *name;
	char		kind;
	Oid			owner;
	bool		made;
	Oid			relid;			/* (InvalidOid: not yet looked up) */
	List	   *saved;
} MoveMaker;

/* An ACL a relation (name) or one of its columns (attname) had */
typedef struct SavedAcl
{
	char		kind;			/* (the object's item kind) */
	char	   *name;
	char	   *attname;		/* (NULL: the object's own) */
	Acl		   *acl;			/* (NULL: none, its owner's implicit ones) */
} SavedAcl;

/* The relations the running move makes as their makers (MoveMaker *) */
static List *move_makers = NIL;

/*
 * ... by kind and name: "k name" -> MoveMaker *.  The key is the whole
 * string, however long (a routine's item name has every argument's name
 * and type), hashed and compared in full: an entry is found by its whole
 * name or not at all.
 */
typedef struct MakerEntry
{
	char	   *key;			/* (the hash key: the string's pointer) */
	void	   *mm;
} MakerEntry;

static HTAB *makers_by_name = NULL;

/* ... by OID, tables and materialized views (MakerRelid) */
static HTAB *makers_by_relid = NULL;

/*
 * What each statement the running move makes is of, said as it is made:
 * the object (an item's kind and name) it makes or changes, whose maker
 * runs it (owner InvalidOid; kind '\0' with no owner: the role moving,
 * itself), or a role named outright (owner).  A statement nothing said
 * this of runs guarded (execute_statement()).
 */
typedef struct MakerStatement
{
	const char *sql;			/* (the key: the statement's pointer) */
	Oid			owner;
	char		kind;
	char	   *name;
} MakerStatement;

static HTAB *maker_statements = NULL;

/* While a move's statements run, the relations its tables are to be. */
static List *move_kept_relids = NIL;

Oid
prolly_recorded_owner(const char *name, Oid owner, const char *owner_name)
{
	char	   *now = OidIsValid(owner) ? GetUserNameFromId(owner, true) : NULL;

	if (now == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("cannot make %s as the commit has it: the role that owned it there no longer exists",
						name),
				 errdetail("A relation a move makes from a commit is made by its recorded owner, with no grants."),
				 errhint("Move to a commit whose tables' owners exist, or drop the table at the commit first.")));
	if (owner_name == NULL || strcmp(now, owner_name) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("cannot make %s as the commit has it: the role that owned it there, \"%s\", is now named \"%s\"",
						name, owner_name ? owner_name : "", now),
				 errdetail("A relation a move makes from a commit is made by its recorded owner, matched by its OID and its name; the OID now names another role, or the role was renamed."),
				 errhint("Rename the role back, or move to a commit whose tables' owners exist.")));
	return owner;
}

static uint32
maker_key_hash(const void *key, Size keysize)
{
	const char *s = *(const char *const *) key;

	return hash_bytes((const unsigned char *) s, strlen(s));
}

static int
maker_key_match(const void *a, const void *b, Size keysize)
{
	return strcmp(*(const char *const *) a, *(const char *const *) b);
}

/* A table of MakerEntry, in cxt, keyed by whole "k name" strings. */
static HTAB *
maker_table(const char *what, MemoryContext cxt)
{
	HASHCTL		ctl;

	ctl.keysize = sizeof(char *);
	ctl.entrysize = sizeof(MakerEntry);
	ctl.hash = maker_key_hash;
	ctl.match = maker_key_match;
	ctl.hcxt = cxt;
	return hash_create(what, 64, &ctl,
					   HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_CONTEXT);
}

/*
 * The entry of (kind, name) in a maker table: found, or (enter) entered
 * with its key copied into cxt and mm NULL; NULL if neither.
 */
static MakerEntry *
maker_entry(HTAB *tab, MemoryContext cxt, const char *name, char kind,
			bool enter, bool *found)
{
	char	   *key = psprintf("%c %s", kind, name);
	MakerEntry *e;
	bool		was;

	e = hash_search(tab, &key, enter ? HASH_ENTER : HASH_FIND, &was);
	if (e != NULL && !was)
	{
		e->key = MemoryContextStrdup(cxt, key);
		e->mm = NULL;
	}
	pfree(key);
	if (found)
		*found = was;
	return e;
}

/* The maker the running move makes relation name (of kind) as, if any. */
static MoveMaker *
maker_named(const char *name, char kind)
{
	MakerEntry *e;

	if (makers_by_name == NULL)
		return NULL;
	e = maker_entry(makers_by_name, NULL, name, kind, false, NULL);
	return e ? e->mm : NULL;
}

/*
 * Record that the running move makes or changes object name (of kind) as
 * owner.  An owner must be known: a move never falls back to its own role
 * for an object it found a maker for.
 */
static MoveMaker *
add_maker(const char *name, char kind, Oid owner, bool made)
{
	MakerEntry *e;
	bool		found;
	MoveMaker  *mm;

	if (!OidIsValid(owner))
		elog(ERROR, "no owner to make %s as", name);
	if (makers_by_name == NULL)
		makers_by_name = maker_table("prolly move makers by name",
									 CurrentMemoryContext);
	e = maker_entry(makers_by_name, GetMemoryChunkContext(makers_by_name),
					name, kind, true, &found);
	if (found)
		return e->mm;
	mm = palloc0(sizeof(MoveMaker));
	mm->name = pstrdup(name);
	mm->kind = kind;
	mm->owner = owner;
	mm->made = made;
	e->mm = mm;
	move_makers = lappend(move_makers, mm);
	return mm;
}

/* Forget the running move's makers and their statements. */
static void
reset_makers(void)
{
	move_makers = NIL;
	makers_by_name = NULL;
	maker_statements = NULL;
	makers_by_relid = NULL;
}

static Oid	object_owner(char kind, const char *name);

/* sql's entry in maker_statements, entered if new */
static MakerStatement *
statement_entry(const char *sql)
{
	MakerStatement *ms;
	bool		found;

	if (maker_statements == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(const char *);
		ctl.entrysize = sizeof(MakerStatement);
		ctl.hcxt = CurrentMemoryContext;
		maker_statements = hash_create("prolly move makers", 64, &ctl,
									   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	ms = hash_search(maker_statements, &sql, HASH_ENTER, &found);
	if (!found)
	{
		ms->owner = InvalidOid;
		ms->kind = '\0';
		ms->name = NULL;
	}
	return ms;
}

/*
 * Run statement sql, when the move runs it, as owner; InvalidOid: as the
 * role moving, which it is said to be (the object it changes is that
 * role's, or no other role's).
 */
static void
made_as(const char *sql, Oid owner)
{
	MakerStatement *ms = statement_entry(sql);

	ms->owner = owner;
	ms->kind = '\0';
	ms->name = NULL;
}

/*
 * The object (kind, name) a statement is made for: one the branch has now
 * that another role owns, which the role moving has the privileges of (a
 * superuser, a member of the owning role), is changed as that owner, if it
 * has no maker yet (one it makes again or changes, beyond the definitions
 * that differ: move_owners_before()).  So an object's statements are run
 * by its owner however the move comes to make them.
 */
static void
object_maker_now(const char *name, char kind)
{
	Oid			owner;
	Oid			mover = GetUserId();

	if (kind == 'x' || maker_named(name, kind) != NULL ||
		(kind == 's' && maker_named(name, 'S') != NULL))
		return;
	owner = object_owner(kind, name);
	if (OidIsValid(owner) && owner != mover && has_privs_of_role(mover, owner))
		(void) add_maker(name, kind, owner, false);
}

/* Run statement sql as the maker of the object an item (kind, name) names. */
static void
made_as_item(const char *sql, const char *name, char kind)
{
	MakerStatement *ms = statement_entry(sql);

	object_maker_now(name, kind);
	ms->owner = InvalidOid;
	ms->kind = kind;
	ms->name = pstrdup(name);
}

/* Run each statement of stmts from the nth on as object name's maker. */
static void
made_as_from(List *stmts, int n, const char *name, char kind)
{
	for (int i = n; i < list_length(stmts); i++)
		made_as_item(list_nth(stmts, i), name, kind);
}

/* Run each statement of stmts as object name's maker. */
static List *
made_as_all(List *stmts, const char *name, char kind)
{
	made_as_from(stmts, 0, name, kind);
	return stmts;
}

/* A list's length and first cell, before statements are added to it. */
typedef struct ListMark
{
	int			n;
	void	   *first;
} ListMark;

static ListMark
list_mark(List *l)
{
	ListMark	m = {list_length(l), l != NIL ? linitial(l) : NULL};

	return m;
}

/*
 * Run each statement added to stmts since m (prepended, lcons(), or
 * appended) as object name's maker.
 */
static void
made_as_since(List *stmts, ListMark m, const char *name, char kind)
{
	int			front = 0;

	if (m.n > 0)
		while (front < list_length(stmts) && list_nth(stmts, front) != m.first)
			front++;
	for (int i = 0; i < list_length(stmts); i++)
		if (i < front || i >= front + m.n)
			made_as_item(list_nth(stmts, i), name, kind);
}

static Oid	cast_maker(const char *name);

/*
 * The role statement sql is run as: its maker (made_as_item()), or the
 * role made_as() names; InvalidOid, the move's own, where it is said to be
 * (*attributed) or nothing was said of it (not *attributed).
 */
static Oid
statement_maker(const char *sql, bool *attributed)
{
	MakerStatement *ms;
	MoveMaker  *mm;

	*attributed = false;
	if (maker_statements == NULL)
		return InvalidOid;
	ms = hash_search(maker_statements, &sql, HASH_FIND, NULL);
	if (ms == NULL)
		return InvalidOid;
	*attributed = true;
	if (ms->kind == '\0')
		return ms->owner;
	if (ms->kind == 'x')
		return cast_maker(ms->name);
	mm = maker_named(ms->name, ms->kind);
	if (mm == NULL && ms->kind == 's')
		mm = maker_named(ms->name, 'S');
	return mm ? mm->owner : InvalidOid;
}

/*
 * Run as role, restricted, as REFRESH runs a materialized view's owner's
 * query: search_path too (RestrictSearchPath(): pg_catalog, then pg_temp,
 * so that the mover's temporary objects are never found first).
 */
/* The role running the move, while it runs as a maker (InvalidOid: not) */
static Oid	move_mover = InvalidOid;

Oid
prolly_move_mover(void)
{
	return OidIsValid(move_mover) ? move_mover : GetUserId();
}

static void
as_role(Oid role, ProllyMakerSwitch *sw)
{
	sw->switched = false;
	if (!OidIsValid(role))
		return;
	GetUserIdAndSecContext(&sw->user, &sw->sec);
	sw->mover = move_mover;
	if (!OidIsValid(move_mover))
		move_mover = sw->user;
	SetUserIdAndSecContext(role, sw->sec | SECURITY_LOCAL_USERID_CHANGE |
						   SECURITY_RESTRICTED_OPERATION);
	sw->nest = NewGUCNestLevel();
	RestrictSearchPath();
	sw->switched = true;
}

/* The running move's makers of tables and materialized views, by OID */
typedef struct MakerRelid
{
	Oid			relid;
	Oid			owner;
} MakerRelid;


static Oid	relation_named(const char *name);

Oid
prolly_move_maker(Oid relid)
{
	MakerRelid *e;
	bool		missed;

	if (move_makers == NIL || !OidIsValid(relid))
		return InvalidOid;
	if (makers_by_relid == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(MakerRelid);
		ctl.hcxt = GetMemoryChunkContext(move_makers);
		makers_by_relid = hash_create("prolly move makers by relid", 64, &ctl,
									  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		foreach_ptr(MoveMaker, mm, move_makers)
			mm->relid = InvalidOid;
	}
	e = hash_search(makers_by_relid, &relid, HASH_FIND, NULL);
	if (e)
		return e->owner;

	/*
	 * (one not yet looked up, or made since it was: a materialized view
	 * made after the tables' rows)
	 */
	missed = false;
	foreach_ptr(MoveMaker, mm, move_makers)
	{
		MakerRelid *n;
		bool		found;

		if (OidIsValid(mm->relid) || (mm->kind != 't' && mm->kind != 'm'))
			continue;
		mm->relid = relation_named(mm->name);
		if (!OidIsValid(mm->relid))
			continue;
		n = hash_search(makers_by_relid, &mm->relid, HASH_ENTER, &found);
		n->owner = mm->owner;
		if (mm->relid == relid)
			missed = true;
	}
	if (!missed)
		return InvalidOid;
	e = hash_search(makers_by_relid, &relid, HASH_FIND, NULL);
	return e ? e->owner : InvalidOid;
}

void
prolly_as_maker(Oid relid, ProllyMakerSwitch *sw)
{
	as_role(move_makers != NIL ? prolly_move_maker(relid) : InvalidOid, sw);
}

void
prolly_as_maker_end(ProllyMakerSwitch *sw)
{
	if (sw->switched)
	{
		AtEOXact_GUC(true, sw->nest);
		SetUserIdAndSecContext(sw->user, sw->sec);
		move_mover = sw->mover;
	}
	sw->switched = false;
}

/*
 * The role a cast of a move's (its item name: "source AS target") is made
 * or dropped as.  CREATE CAST requires owning one of its types: so the
 * role moving, if it owns one; else the owner of one whose privileges the
 * role has (a superuser, a member of the owning role), its versioned type's
 * first; else the move is refused, since nothing the role may do makes it.
 * A cast between the system's types alone is the role's own (PostgreSQL
 * then requires a superuser).  Read as the statement runs, its types made
 * by then.  The types are read by the parser, not from the name's text.
 */
static Oid
cast_maker(const char *name)
{
	Oid			mover = GetUserId();
	List	   *raw = raw_parser(psprintf("DROP CAST (%s)", name), RAW_PARSE_DEFAULT);
	DropStmt   *drop;
	List	   *types;
	Oid			typids[2];
	Oid			owners[2];

	if (list_length(raw) != 1 ||
		!IsA(((RawStmt *) linitial(raw))->stmt, DropStmt) ||
		(drop = (DropStmt *) ((RawStmt *) linitial(raw))->stmt)->removeType != OBJECT_CAST ||
		list_length(drop->objects) != 1 ||
		list_length(types = linitial(drop->objects)) != 2)
		elog(ERROR, "unexpected cast name \"%s\"", name);
	for (int i = 0; i < 2; i++)
	{
		HeapTuple	tup;

		typids[i] = LookupTypeNameOid(NULL, list_nth_node(TypeName, types, i), true);
		owners[i] = InvalidOid;
		if (!OidIsValid(typids[i]))
			continue;
		tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typids[i]));
		if (HeapTupleIsValid(tup))
		{
			owners[i] = ((Form_pg_type) GETSTRUCT(tup))->typowner;
			ReleaseSysCache(tup);
		}
	}
	if (owners[0] == mover || owners[1] == mover)
		return InvalidOid;
	/* (a type gone already: a drop IF EXISTS of nothing) */
	if (!OidIsValid(owners[0]) && !OidIsValid(owners[1]))
		return InvalidOid;
	if ((!OidIsValid(typids[0]) || typids[0] < FirstNormalObjectId) &&
		(!OidIsValid(typids[1]) || typids[1] < FirstNormalObjectId))
		return InvalidOid;
	for (int i = 0; i < 2; i++)
		if (OidIsValid(typids[i]) && typids[i] >= FirstNormalObjectId &&
			has_privs_of_role(mover, owners[i]))
			return owners[i];
	ereport(ERROR,
			(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
			 errmsg("cannot move cast (%s): role \"%s\" owns neither of its types",
					name, GetUserNameFromId(mover, false)),
			 errdetail("A move makes and drops a cast as the owner of one of its types, as CREATE CAST requires, and the role moving has neither owner's privileges.")));
	return InvalidOid;			/* keep compiler quiet */
}

/*
 * A statement of a move's that nothing said the maker of runs as the role
 * moving, guarded: while it runs, each object it makes, changes, drops or
 * empties, and each routine it runs, must be that role's own, or the
 * system's, or another role's whose privileges it hasn't (which
 * PostgreSQL's own checks then refuse it); and nothing it makes may be one
 * the move has a maker of other than it.  Otherwise it would act with its
 * role's authority on another's object, or run another's code with it:
 * refused, as an internal error, since every statement a move makes says
 * whose it is.
 */
static object_access_hook_type move_guard_next = NULL;
static int	move_guard_depth = 0;
static Oid	move_guard_role = InvalidOid;
static const char *move_guard_sql = NULL;

/* objectId's row (classId's catalog) as the running command sees it, copied */
static HeapTuple
catalog_row_now(Relation catalog, Oid classId, Oid objectId)
{
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;

	ScanKeyInit(&key, get_object_attnum_oid(classId), BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(objectId));
	scan = systable_beginscan(catalog, get_object_oid_index(classId), true,
							  SnapshotSelf, 1, &key);
	tup = systable_getnext(scan);
	if (HeapTupleIsValid(tup))
		tup = heap_copytuple(tup);
	systable_endscan(scan);
	return tup;
}

/*
 * The owner of the object (or part of one: a constraint, trigger, policy,
 * rule or default is its relation's or type's) the guard sees touched;
 * InvalidOid if it has none.  A cast is that of whichever of its types the
 * role owns, else its source type's owner.
 */
static Oid
touched_owner(Oid classId, Oid objectId, Oid role)
{
	Relation	catalog;
	HeapTuple	tup;
	Oid			owner = InvalidOid;
	AttrNumber	ownerattr;
	AttrNumber	relattr = InvalidAttrNumber;
	bool		isnull;

	if (objectId < FirstNormalObjectId || !is_objectclass_supported(classId))
		return InvalidOid;
	switch (classId)
	{
		case TriggerRelationId:
			relattr = Anum_pg_trigger_tgrelid;
			break;
		case PolicyRelationId:
			relattr = Anum_pg_policy_polrelid;
			break;
		case RewriteRelationId:
			relattr = Anum_pg_rewrite_ev_class;
			break;
		case AttrDefaultRelationId:
			relattr = Anum_pg_attrdef_adrelid;
			break;
		case ConstraintRelationId:
		case CastRelationId:
			break;
		default:
			if (get_object_attnum_owner(classId) == InvalidAttrNumber)
				return InvalidOid;
	}
	catalog = table_open(classId, AccessShareLock);
	tup = catalog_row_now(catalog, classId, objectId);
	if (HeapTupleIsValid(tup))
	{
		if (classId == ConstraintRelationId)
		{
			Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tup);

			if (OidIsValid(con->conrelid))
				owner = touched_owner(RelationRelationId, con->conrelid, role);
			else if (OidIsValid(con->contypid))
				owner = touched_owner(TypeRelationId, con->contypid, role);
		}
		else if (classId == CastRelationId)
		{
			Form_pg_cast c = (Form_pg_cast) GETSTRUCT(tup);
			Oid			src = touched_owner(TypeRelationId, c->castsource, role);
			Oid			tgt = touched_owner(TypeRelationId, c->casttarget, role);

			owner = (src == role || tgt == role) ? role :
				OidIsValid(src) ? src : tgt;
		}
		else if (relattr != InvalidAttrNumber)
		{
			Datum		rel = heap_getattr(tup, relattr, RelationGetDescr(catalog),
										   &isnull);

			if (!isnull)
				owner = touched_owner(RelationRelationId, DatumGetObjectId(rel), role);
		}
		else
		{
			Datum		o;

			ownerattr = get_object_attnum_owner(classId);
			o = heap_getattr(tup, ownerattr, RelationGetDescr(catalog), &isnull);
			if (!isnull)
				owner = DatumGetObjectId(o);
		}
		heap_freetuple(tup);
	}
	table_close(catalog, AccessShareLock);
	return owner;
}

/*
 * The maker the move has of an object made now (InvalidOid: none): one of
 * the kind and name the object's item would have.  A routine is matched by
 * its qualified name, whatever its arguments (its identity can't be read
 * yet), so an overload of one with a maker is held to that maker too.
 */
static Oid
made_object_maker(Oid classId, Oid objectId)
{
	Relation	catalog;
	HeapTuple	tup;
	Oid			owner = InvalidOid;

	if (move_makers == NIL || objectId < FirstNormalObjectId)
		return InvalidOid;
	if (classId != RelationRelationId && classId != ProcedureRelationId &&
		classId != TypeRelationId && classId != NamespaceRelationId &&
		classId != CollationRelationId)
		return InvalidOid;
	catalog = table_open(classId, AccessShareLock);
	tup = catalog_row_now(catalog, classId, objectId);
	if (HeapTupleIsValid(tup))
	{
		MoveMaker  *mm = NULL;

		switch (classId)
		{
			case RelationRelationId:
				{
					Form_pg_class c = (Form_pg_class) GETSTRUCT(tup);
					char	   *nsp = get_namespace_name(c->relnamespace);
					char	   *name = nsp ? quote_qualified_identifier(nsp, NameStr(c->relname)) : NULL;

					if (name == NULL)
						break;
					if (c->relkind == RELKIND_RELATION ||
						c->relkind == RELKIND_PARTITIONED_TABLE)
						mm = maker_named(name, 't');
					else if (c->relkind == RELKIND_MATVIEW)
						mm = maker_named(name, 'm');
					else if (c->relkind == RELKIND_VIEW)
						mm = maker_named(name, 'v');
					else if (c->relkind == RELKIND_FOREIGN_TABLE)
						mm = maker_named(name, 'r');
					else if (c->relkind == RELKIND_SEQUENCE &&
							 (mm = maker_named(name, 's')) == NULL)
						mm = maker_named(name, 'S');
				}
				break;
			case ProcedureRelationId:
				{
					Form_pg_proc p = (Form_pg_proc) GETSTRUCT(tup);
					char	   *nsp = get_namespace_name(p->pronamespace);
					char	   *prefix;

					if (nsp == NULL)
						break;
					prefix = psprintf("%s(", quote_qualified_identifier(nsp, NameStr(p->proname)));
					foreach_ptr(MoveMaker, m, move_makers)
						if (m->kind == 'f' && strncmp(m->name, prefix, strlen(prefix)) == 0 &&
							m->owner != move_guard_role)
							mm = m;
				}
				break;
			case TypeRelationId:
				{
					Form_pg_type t = (Form_pg_type) GETSTRUCT(tup);
					char	   *nsp = get_namespace_name(t->typnamespace);

					if (nsp != NULL)
						mm = maker_named(quote_qualified_identifier(nsp, NameStr(t->typname)), 'y');
				}
				break;
			case NamespaceRelationId:
				mm = maker_named(quote_identifier(NameStr(((Form_pg_namespace) GETSTRUCT(tup))->nspname)),
								 'n');
				break;
			case CollationRelationId:
				{
					Form_pg_collation c = (Form_pg_collation) GETSTRUCT(tup);
					char	   *nsp = get_namespace_name(c->collnamespace);

					if (nsp != NULL)
						mm = maker_named(quote_qualified_identifier(nsp, NameStr(c->collname)), 'l');
				}
				break;
		}
		if (mm != NULL)
			owner = mm->owner;
		heap_freetuple(tup);
	}
	table_close(catalog, AccessShareLock);
	return owner;
}

static void
move_guard_refuse(Oid classId, Oid objectId, int subId, Oid owner,
				  const char *what)
{
	ObjectAddress addr;
	char	   *desc;
	char	   *owner_name = GetUserNameFromId(owner, true);

	ObjectAddressSubSet(addr, classId, objectId, subId);
	desc = getObjectDescription(&addr, true);
	/* (one being made, not yet seen by the caches: its catalog row's name) */
	if (desc == NULL && is_objectclass_supported(classId) &&
		get_object_attnum_name(classId) != InvalidAttrNumber)
	{
		Relation	catalog = table_open(classId, AccessShareLock);
		HeapTuple	tup = catalog_row_now(catalog, classId, objectId);
		bool		isnull;

		if (HeapTupleIsValid(tup))
		{
			Datum		name = heap_getattr(tup, get_object_attnum_name(classId),
											RelationGetDescr(catalog), &isnull);

			if (!isnull)
				desc = psprintf("%s \"%s\"", get_object_class_descr(classId),
								NameStr(*DatumGetName(name)));
		}
		table_close(catalog, AccessShareLock);
	}
	elog(ERROR, "a move's statement with no maker would %s %s, which role \"%s\" %s, as role \"%s\": \"%s\"",
		 what, desc ? desc : "an object", owner_name ? owner_name : "?",
		 strcmp(what, "make") == 0 ? "is to make" : "owns",
		 GetUserNameFromId(move_guard_role, false), move_guard_sql);
}

/*
 * Whether routine proc, run by role, is an extension's as installed: owned
 * by the extension's owner, which is the bootstrap superuser, or has the
 * privileges of role (so running it as role gives its code nothing).  An
 * extension another role made (a trusted one) holds that role's code, and
 * a routine it added (ALTER EXTENSION ... ADD FUNCTION) is as much its own,
 * so membership alone exempts nothing.
 */
static bool
installed_routine(Oid proc, Oid role)
{
	Oid			ext = getExtensionOfObject(ProcedureRelationId, proc);
	HeapTuple	tup;
	Oid			ext_owner = InvalidOid;
	Oid			owner;

	if (!OidIsValid(ext))
		return false;
	tup = SearchSysCache1(EXTENSIONOID, ObjectIdGetDatum(ext));
	if (HeapTupleIsValid(tup))
	{
		ext_owner = ((Form_pg_extension) GETSTRUCT(tup))->extowner;
		ReleaseSysCache(tup);
	}
	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(proc));
	if (!HeapTupleIsValid(tup))
		return false;
	owner = ((Form_pg_proc) GETSTRUCT(tup))->proowner;
	ReleaseSysCache(tup);
	return OidIsValid(ext_owner) && owner == ext_owner &&
		(ext_owner == BOOTSTRAP_SUPERUSERID || has_privs_of_role(ext_owner, role));
}

static void
move_guard_hook(ObjectAccessType access, Oid classId, Oid objectId, int subId,
				void *arg)
{
	const char *what;
	Oid			owner;

	if (move_guard_next)
		move_guard_next(access, classId, objectId, subId, arg);
	if (move_guard_depth == 0)
		return;
	switch (access)
	{
		case OAT_POST_CREATE:
			what = "make";
			break;
		case OAT_POST_ALTER:
			what = "change";
			break;
		case OAT_DROP:
			what = "drop";
			break;
		case OAT_TRUNCATE:
			what = "empty";
			break;
		case OAT_FUNCTION_EXECUTE:
			what = "run";
			if (installed_routine(objectId, move_guard_role))
				return;
			break;
		default:
			return;
	}
	owner = touched_owner(classId, objectId, move_guard_role);
	if (OidIsValid(owner) && owner != move_guard_role &&
		has_privs_of_role(move_guard_role, owner))
		move_guard_refuse(classId, objectId, subId, owner,
						  access == OAT_POST_CREATE ? "change" : what);
	if (access == OAT_POST_CREATE && subId == 0)
	{
		owner = made_object_maker(classId, objectId);
		if (OidIsValid(owner) && owner != move_guard_role)
			move_guard_refuse(classId, objectId, subId, owner, "make");
	}
}

/*
 * Refuse a statement nothing said the maker of, in a move that has
 * makers: what it would touch can't be told from its text, so the move
 * fails closed rather than run it as the role moving beside objects other
 * roles' makers make.  (Every statement a move makes says whose it is;
 * this is an internal error.)
 */
static void
unattributed_statement(const char *sql)
{
	elog(ERROR, "a move's statement has no maker, while the move makes objects as their owners: \"%s\"",
		 sql);
}

/*
 * Run sql (SPI) as the move runs it: as its maker, if one was given it
 * (statement_maker()), under SECURITY_RESTRICTED_OPERATION; else as the
 * role running the move, guarded if nothing was said of it.
 */
static int
execute_statement(const char *sql)
{
	ProllyMakerSwitch sw;
	bool		attributed;
	bool		installed = false;
	object_access_hook_type next = move_guard_next;
	int			rc;
	int			depth = move_guard_depth;
	Oid			role = move_guard_role;
	const char *was = move_guard_sql;

	as_role(statement_maker(sql, &attributed), &sw);
	if (!attributed && move_makers != NIL)
		unattributed_statement(sql);
	if (attributed && !sw.switched && move_guard_depth == 0)
		return SPI_execute(sql, false, 0);
	PG_TRY();
	{
		if (!attributed)
		{
			/* (for this statement only: others' hooks come and go) */
			if (object_access_hook != move_guard_hook)
			{
				move_guard_next = object_access_hook;
				object_access_hook = move_guard_hook;
				installed = true;
			}
			move_guard_depth++;
			move_guard_role = GetUserId();
			move_guard_sql = sql;
		}
		else
			move_guard_depth = 0;	/* (one said to be another's: unguarded) */
		rc = SPI_execute(sql, false, 0);
	}
	PG_FINALLY();
	{
		if (installed)
		{
			object_access_hook = move_guard_next;
			move_guard_next = next;
		}
		move_guard_depth = depth;
		move_guard_role = role;
		move_guard_sql = was;
		prolly_as_maker_end(&sw);
	}
	PG_END_TRY();
	return rc;
}

/*
 * While a move runs (prolly_move_calls_begin() to its end), each routine
 * it calls that isn't the system's (an OID below FirstNormalObjectId) runs
 * as its owner, under SECURITY_RESTRICTED_OPERATION, as a SECURITY DEFINER
 * routine runs, wherever the call comes from: a statement the move runs as
 * a maker or as itself, a check, default or index expression of the rows
 * it writes, a domain's check of a conversion, a materialized view's
 * query, another routine.  So no role's code ever runs with the authority
 * of the role moving, or of another maker, whatever path reaches it.  The
 * function manager's hooks do it (needs_fmgr_hook, fmgr_hook, as sepgsql
 * uses them): a routine the hook is needed for is never inlined by the
 * planner either, so an SQL routine's body can't run unseen inside the
 * expression that calls it.
 *
 * A routine whose owner has the privileges of the role calling it (its
 * own, a superuser's, the bootstrap superuser's, an extension's installed
 * by one) runs as that caller, as in PostgreSQL: its code gains nothing by
 * it, while running it as its owner would give the caller the owner's
 * authority.  A SECURITY DEFINER routine runs as its owner already, and
 * here restricted too (but a superuser's).  A
 * routine's own search_path setting is kept; any other runs with
 * pg_catalog, then pg_temp (RestrictSearchPath()).  The routine still
 * needs EXECUTE for its caller, checked before the call.
 *
 * Domain checks the type cache planned before the move, and every plan the
 * session cached (either may have inlined an SQL routine, or set up its
 * calls unhooked: a PL/pgSQL routine's), are planned again as it begins,
 * and the plans made in it once more as it ends; and no query or index
 * build of the move runs in parallel workers, which the hooks don't reach.
 * Function lookups other long-lived caches made before the move (index and
 * partition support functions, the type cache's comparison and range
 * functions) aren't hooked: those routines are an operator class's, which
 * a superuser makes, C functions, which only a superuser can make, or a
 * range type's subtype difference, which the type's maker chose.
 */
static int	move_calls_depth = 0;
static bool move_calls_installed = false;
static needs_fmgr_hook_type move_calls_next_needs = NULL;
static fmgr_hook_type move_calls_next = NULL;

/* What a hooked call's routine is, worked out on its first call */
typedef struct MoveCallee
{
	Oid			owner;
	bool		definer;		/* (SECURITY DEFINER: its owner already) */
	bool		configured;		/* (settings of its own: a GUC level) */
	bool		own_path;		/* (search_path among them) */
	Datum		next_private;	/* (the next hook's) */
} MoveCallee;

/* A call's role switch, undone as it ends (innermost last) */
typedef struct MoveCallSwitch
{
	bool		switched;
	Oid			user;
	int			sec;
	int			nest;			/* (0: none of its own) */
} MoveCallSwitch;

static MoveCallSwitch *move_call_switches = NULL;
static int	move_call_depth = 0;
static int	move_call_max = 0;

static bool
move_calls_needed(Oid fn_oid)
{
	if (move_calls_next_needs && move_calls_next_needs(fn_oid))
		return true;
	return move_calls_depth > 0 && fn_oid >= FirstNormalObjectId;
}

static MoveCallee *
move_callee(FmgrInfo *flinfo, Datum *private)
{
	MoveCallee *c = (MoveCallee *) DatumGetPointer(*private);
	HeapTuple	tup;
	Form_pg_proc p;
	Datum		config;
	bool		isnull;

	if (c != NULL)
		return c;
	c = MemoryContextAllocZero(flinfo->fn_mcxt, sizeof(MoveCallee));
	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(flinfo->fn_oid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for function %u", flinfo->fn_oid);
	p = (Form_pg_proc) GETSTRUCT(tup);
	c->owner = p->proowner;
	c->definer = p->prosecdef;
	config = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_proconfig, &isnull);
	if (!isnull)
	{
		List	   *names;
		List	   *values;

		c->configured = true;
		TransformGUCArray(DatumGetArrayTypeP(config), &names, &values);
		foreach_ptr(char, name, names)
			if (pg_strcasecmp(name, "search_path") == 0)
				c->own_path = true;
	}
	ReleaseSysCache(tup);
	*private = PointerGetDatum(c);
	return c;
}

static void
move_calls_hook(FmgrHookEventType event, FmgrInfo *flinfo, Datum *private)
{
	MoveCallee *c = move_callee(flinfo, private);

	if (event == FHET_START)
	{
		MoveCallSwitch sw = {0};
		Oid			caller = GetUserId();

		/*
		 * (the next hook's first, and nothing after the switch is kept that
		 * can fail: a start that fails hears no end)
		 */
		if (move_calls_next)
			move_calls_next(event, flinfo, &c->next_private);

		if (move_call_depth == move_call_max)
		{
			int			max = Max(16, move_call_max * 2);

			move_call_switches = move_call_switches == NULL ?
				MemoryContextAlloc(TopMemoryContext, sizeof(MoveCallSwitch) * max) :
				repalloc(move_call_switches, sizeof(MoveCallSwitch) * max);
			move_call_max = max;
		}
		/*
		 * (a SECURITY DEFINER routine's owner the function manager has made
		 * the user already, unrestricted, and gives the caller back itself
		 * before this hook hears the end: restricted here, unless a
		 * superuser's, whose privileges every caller's are within)
		 */
		if (move_calls_depth > 0 && flinfo->fn_oid >= FirstNormalObjectId &&
			c->definer && !superuser_arg(c->owner))
		{
			Oid			user;
			int			sec;

			GetUserIdAndSecContext(&user, &sec);
			SetUserIdAndSecContext(user, sec | SECURITY_RESTRICTED_OPERATION);
		}
		if (move_calls_depth > 0 && flinfo->fn_oid >= FirstNormalObjectId &&
			!c->definer && c->owner != caller &&
			!has_privs_of_role(c->owner, caller))
		{
			GetUserIdAndSecContext(&sw.user, &sw.sec);
			SetUserIdAndSecContext(c->owner, sw.sec | SECURITY_LOCAL_USERID_CHANGE |
								   SECURITY_RESTRICTED_OPERATION);
			sw.switched = true;
			/*
			 * (a routine with settings of its own has its GUC level, which
			 * the function manager ends before this hook hears the end)
			 */
			if (!c->configured)
				sw.nest = NewGUCNestLevel();
			if (!c->own_path)
				RestrictSearchPath();
		}
		move_call_switches[move_call_depth++] = sw;
		return;
	}
	if (move_call_depth > 0)
	{
		MoveCallSwitch sw = move_call_switches[--move_call_depth];

		if (sw.switched)
		{
			if (sw.nest > 0)
				AtEOXact_GUC(event == FHET_END, sw.nest);
			SetUserIdAndSecContext(sw.user, sw.sec);
		}
	}
	if (move_calls_next)
		move_calls_next(event, flinfo, &c->next_private);
}

int
prolly_move_calls_begin(void)
{
	int			nest = 0;

	if (!move_calls_installed)
	{
		move_calls_next_needs = needs_fmgr_hook;
		needs_fmgr_hook = move_calls_needed;
		move_calls_next = fmgr_hook;
		fmgr_hook = move_calls_hook;
		move_calls_installed = true;
	}
	if (move_calls_depth++ == 0)
	{
		nest = NewGUCNestLevel();
		(void) set_config_option("max_parallel_workers_per_gather", "0",
								 PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE,
								 true, 0, false);
		(void) set_config_option("max_parallel_maintenance_workers", "0",
								 PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE,
								 true, 0, false);
		/*
		 * (the type cache's domain checks, and every plan the session
		 * cached, planned again: one made before may have another role's
		 * SQL routine inlined, or set up its calls unhooked)
		 */
		TypeCacheForgetDomainConstraints();
		ResetPlanCache();
	}
	return nest;
}

void
prolly_move_calls_end(int nest, bool done)
{
	Assert(move_calls_depth > 0);
	if (--move_calls_depth == 0 && nest > 0)
	{
		AtEOXact_GUC(done, nest);
		/* (and those made in the move, hooked, not kept past it) */
		ResetPlanCache();
	}
}

/* Set a catalog row's ACL column to NULL (no grants), as owner's. */
static void
clear_acl(Relation catalog, HeapTuple tup, AttrNumber aclattr, Oid relid,
		  AttrNumber attnum, Oid owner)
{
	bool		isnull;
	Datum		acl = heap_getattr(tup, aclattr, RelationGetDescr(catalog), &isnull);
	Datum	   *values;
	bool	   *nulls;
	bool	   *replaces;
	Oid		   *members;
	int			nmembers;
	HeapTuple	newtup;

	if (isnull)
		return;
	nmembers = aclmembers(DatumGetAclP(acl), &members);
	values = palloc0_array(Datum, RelationGetDescr(catalog)->natts);
	nulls = palloc0_array(bool, RelationGetDescr(catalog)->natts);
	replaces = palloc0_array(bool, RelationGetDescr(catalog)->natts);
	nulls[aclattr - 1] = true;
	replaces[aclattr - 1] = true;
	newtup = heap_modify_tuple(tup, RelationGetDescr(catalog), values, nulls,
							   replaces);
	CatalogTupleUpdate(catalog, &newtup->t_self, newtup);
	updateAclDependencies(RelationRelationId, relid, attnum, owner,
						  nmembers, members, 0, NULL);
}

/*
 * Leave relid with no grants: its own ACL, its columns' and those of the
 * sequences it owns (a serial's, an identity's), which its making gave
 * them from its maker's default privileges.
 */
static void
clear_made_privileges(Oid relid)
{
	Relation	pg_class = table_open(RelationRelationId, RowExclusiveLock);
	Relation	pg_attribute = table_open(AttributeRelationId, RowExclusiveLock);
	List	   *relids = list_make1_oid(relid);
	Relation	depend;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;

	depend = table_open(DependRelationId, AccessShareLock);
	ScanKeyInit(&key[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(relid));
	scan = systable_beginscan(depend, DependReferenceIndexId, true, NULL, 2, key);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_depend d = (Form_pg_depend) GETSTRUCT(tup);

		if (d->classid == RelationRelationId &&
			(d->deptype == DEPENDENCY_AUTO || d->deptype == DEPENDENCY_INTERNAL) &&
			get_rel_relkind(d->objid) == RELKIND_SEQUENCE)
			relids = list_append_unique_oid(relids, d->objid);
	}
	systable_endscan(scan);
	table_close(depend, AccessShareLock);
	foreach_oid(r, relids)
	{
		HeapTuple	ctup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(r));
		Oid			owner;
		ScanKeyData akey;
		SysScanDesc ascan;
		HeapTuple	atup;

		if (!HeapTupleIsValid(ctup))
			continue;
		owner = ((Form_pg_class) GETSTRUCT(ctup))->relowner;
		clear_acl(pg_class, ctup, Anum_pg_class_relacl, r, 0, owner);
		ScanKeyInit(&akey, Anum_pg_attribute_attrelid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(r));
		ascan = systable_beginscan(pg_attribute, AttributeRelidNumIndexId, true,
								   NULL, 1, &akey);
		while ((atup = systable_getnext(ascan)) != NULL)
		{
			Form_pg_attribute a = (Form_pg_attribute) GETSTRUCT(atup);

			if (a->attnum > 0 && !a->attisdropped)
				clear_acl(pg_attribute, atup, Anum_pg_attribute_attacl, r,
						  a->attnum, owner);
		}
		systable_endscan(ascan);
	}
	table_close(pg_attribute, RowExclusiveLock);
	table_close(pg_class, RowExclusiveLock);
	CommandCounterIncrement();
}

/*
 * The sequences a table (name, as td defines it) owns: its identity
 * columns' (named as Postgres names them) and those its columns' defaults
 * draw from (nextval()), which a move making the table makes its own.
 */
static List *
table_sequences(const char *name, const TableDef *td)
{
	List	   *out = NIL;
	const char *dot = strrchr(name, '.');
	ListCell   *c;

	foreach(c, td->columns)
	{
		char	   *col = first_field(lfirst(c));
		char	   *def = column_attr(lfirst(c), "default ");
		const char *p;

		if (column_attr(lfirst(c), "identity ") != NULL && dot != NULL)
			out = lappend(out,
						  psprintf("%s.%s", schema_of(name),
								   quote_identifier(psprintf("%s_%s_seq",
															 unquoted(dot + 1),
															 unquoted(col)))));
		if (def == NULL || (p = strstr(def, "nextval('")) == NULL)
			continue;
		{
			StringInfoData seq;

			initStringInfo(&seq);
			for (p += strlen("nextval('"); *p; p++)
			{
				if (*p == '\'' && p[1] == '\'')
					appendStringInfoChar(&seq, *p++);
				else if (*p == '\'')
					break;
				else
					appendStringInfoChar(&seq, *p);
			}
			if (*p == '\'')
				out = lappend(out, seq.data);
		}
	}
	return out;
}

static Oid	item_owner(const MoveItem *it, bool *known);

/*
 * The relation of a qualified name (a move item's), looked up without the
 * schema USAGE check RangeVarGetRelid() makes: a move reads what the branch
 * has whatever its role may use (InvalidOid: none).
 */
static Oid
relation_named(const char *name)
{
	List	   *names = stringToQualifiedNameList(name, NULL);
	Oid			nsp;

	if (list_length(names) != 2)
		return InvalidOid;
	nsp = get_namespace_oid(strVal(linitial(names)), true);
	return OidIsValid(nsp) ? get_relname_relid(strVal(lsecond(names)), nsp) :
		InvalidOid;
}

/* relid's owner (InvalidOid: no such relation) */
static Oid
relation_owner_of(Oid relid)
{
	HeapTuple	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	Oid			owner;

	if (!HeapTupleIsValid(tup))
		return InvalidOid;
	owner = ((Form_pg_class) GETSTRUCT(tup))->relowner;
	ReleaseSysCache(tup);
	return owner;
}

/* Whether an item kind is a relation's */
static bool
relation_kind(char kind)
{
	return kind == 't' || kind == 'm' || kind == 'v' || kind == 's' ||
		kind == 'S' || kind == 'r';
}

/*
 * The routine an item names (qualified, its identity arguments, as
 * routine_item_name() writes them), looked up without USAGE checks.
 */
static Oid
routine_named(const char *name)
{
	const char *paren = strchr(name, '(');
	List	   *names;
	Oid			nsp;
	Oid			found = InvalidOid;
	CatCList   *list;
	int			save_nestlevel;

	if (paren == NULL)
		return InvalidOid;
	names = stringToQualifiedNameList(pnstrdup(name, paren - name), NULL);
	if (list_length(names) != 2 ||
		!OidIsValid(nsp = get_namespace_oid(strVal(linitial(names)), true)))
		return InvalidOid;
	save_nestlevel = NewGUCNestLevel();
	describing_settings();
	list = SearchSysCacheList1(PROCNAMEARGSNSP,
							   CStringGetDatum(strVal(lsecond(names))));
	for (int i = 0; i < list->n_members && !OidIsValid(found); i++)
	{
		Form_pg_proc proc = (Form_pg_proc) GETSTRUCT(&list->members[i]->tuple);

		if (proc->pronamespace == nsp &&
			strcmp(routine_item_name(proc), name) == 0)
			found = proc->oid;
	}
	ReleaseSysCacheList(list);
	AtEOXact_GUC(true, save_nestlevel);
	return found;
}

/* The type (or domain) an item names, looked up without USAGE checks. */
static Oid
type_named(const char *name)
{
	List	   *names = stringToQualifiedNameList(name, NULL);
	Oid			nsp;

	if (list_length(names) != 2 ||
		!OidIsValid(nsp = get_namespace_oid(strVal(linitial(names)), true)))
		return InvalidOid;
	return GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
						   CStringGetDatum(strVal(lsecond(names))),
						   ObjectIdGetDatum(nsp));
}

/*
 * The object an item (kind, name) names on the session's branch, and its
 * catalog (*classid); InvalidOid if none.
 */
static Oid
object_named(char kind, const char *name, Oid *classid)
{
	if (relation_kind(kind))
	{
		*classid = RelationRelationId;
		return relation_named(name);
	}
	switch (kind)
	{
		case 'f':
			*classid = ProcedureRelationId;
			return routine_named(name);
		case 'y':
			*classid = TypeRelationId;
			return type_named(name);
		case 'n':
			*classid = NamespaceRelationId;
			return get_namespace_oid(unquoted(name), true);
		case 'l':
			*classid = CollationRelationId;
			{
				List	   *names = stringToQualifiedNameList(name, NULL);
				Oid			nsp;
				Oid			coll;

				if (list_length(names) != 2 ||
					!OidIsValid(nsp = get_namespace_oid(strVal(linitial(names)), true)))
					return InvalidOid;
				coll = GetSysCacheOid3(COLLNAMEENCNSP, Anum_pg_collation_oid,
									   CStringGetDatum(strVal(lsecond(names))),
									   Int32GetDatum(-1), ObjectIdGetDatum(nsp));
				if (!OidIsValid(coll))
					coll = GetSysCacheOid3(COLLNAMEENCNSP, Anum_pg_collation_oid,
										   CStringGetDatum(strVal(lsecond(names))),
										   Int32GetDatum(GetDatabaseEncoding()),
										   ObjectIdGetDatum(nsp));
				return coll;
			}
	}
	*classid = InvalidOid;
	return InvalidOid;
}

/*
 * An object's catalog row's syscache, ACL and owner columns (false: its
 * catalog has no ACL).
 */
static bool
acl_columns(Oid classid, int *cacheid, AttrNumber *aclattr, AttrNumber *ownerattr)
{
	switch (classid)
	{
		case RelationRelationId:
			*cacheid = RELOID;
			*aclattr = Anum_pg_class_relacl;
			*ownerattr = Anum_pg_class_relowner;
			return true;
		case ProcedureRelationId:
			*cacheid = PROCOID;
			*aclattr = Anum_pg_proc_proacl;
			*ownerattr = Anum_pg_proc_proowner;
			return true;
		case TypeRelationId:
			*cacheid = TYPEOID;
			*aclattr = Anum_pg_type_typacl;
			*ownerattr = Anum_pg_type_typowner;
			return true;
		case NamespaceRelationId:
			*cacheid = NAMESPACEOID;
			*aclattr = Anum_pg_namespace_nspacl;
			*ownerattr = Anum_pg_namespace_nspowner;
			return true;
	}
	return false;
}

/*
 * Set object oid's (of classid; a relation's column attname, if given) ACL
 * to acl, NULL for none (its owner's implicit privileges), its shared
 * dependencies with it.
 */
static void
set_object_acl(Oid classid, Oid oid, const char *attname, Acl *acl)
{
	int			cacheid;
	AttrNumber	aclattr;
	AttrNumber	ownerattr;
	Relation	catalog;
	HeapTuple	tup;
	HeapTuple	ctup;
	HeapTuple	newtup;
	Datum		old;
	bool		isnull;
	Oid			owner;
	Oid		   *oldmembers = NULL;
	int			noldmembers = 0;
	Oid		   *newmembers = NULL;
	int			nnewmembers = 0;
	Datum	   *values;
	bool	   *nulls;
	bool	   *replaces;
	AttrNumber	attnum = 0;

	if (!acl_columns(classid, &cacheid, &aclattr, &ownerattr))
		return;
	ctup = SearchSysCache1(cacheid, ObjectIdGetDatum(oid));
	if (!HeapTupleIsValid(ctup))
		return;
	owner = DatumGetObjectId(SysCacheGetAttrNotNull(cacheid, ctup, ownerattr));
	if (attname)
	{
		ReleaseSysCache(ctup);
		catalog = table_open(AttributeRelationId, RowExclusiveLock);
		aclattr = Anum_pg_attribute_attacl;
		tup = SearchSysCacheCopyAttName(oid, attname);
		if (!HeapTupleIsValid(tup))
		{
			table_close(catalog, RowExclusiveLock);
			return;
		}
		attnum = ((Form_pg_attribute) GETSTRUCT(tup))->attnum;
	}
	else
	{
		tup = heap_copytuple(ctup);
		ReleaseSysCache(ctup);
		catalog = table_open(classid, RowExclusiveLock);
	}
	old = heap_getattr(tup, aclattr, RelationGetDescr(catalog), &isnull);
	if (!isnull)
		noldmembers = aclmembers(DatumGetAclP(old), &oldmembers);
	if (isnull && acl == NULL)
	{
		table_close(catalog, RowExclusiveLock);
		return;
	}
	if (acl)
		nnewmembers = aclmembers(acl, &newmembers);
	values = palloc0_array(Datum, RelationGetDescr(catalog)->natts);
	nulls = palloc0_array(bool, RelationGetDescr(catalog)->natts);
	replaces = palloc0_array(bool, RelationGetDescr(catalog)->natts);
	if (acl)
		values[aclattr - 1] = PointerGetDatum(acl);
	else
		nulls[aclattr - 1] = true;
	replaces[aclattr - 1] = true;
	newtup = heap_modify_tuple(tup, RelationGetDescr(catalog), values, nulls,
							   replaces);
	CatalogTupleUpdate(catalog, &newtup->t_self, newtup);
	updateAclDependencies(classid, oid, attnum, owner,
						  noldmembers, oldmembers, nnewmembers, newmembers);
	table_close(catalog, RowExclusiveLock);
	CommandCounterIncrement();
}

/* An object's (or its column's) ACL now, copied: NULL for none. */
static Acl *
object_acl(Oid classid, Oid oid, HeapTuple atup, Relation pg_attribute)
{
	int			cacheid;
	AttrNumber	aclattr;
	AttrNumber	ownerattr;
	bool		isnull;
	Datum		acl;

	if (atup)
	{
		acl = heap_getattr(atup, Anum_pg_attribute_attacl,
						   RelationGetDescr(pg_attribute), &isnull);
		return isnull ? NULL : DatumGetAclPCopy(acl);
	}
	if (!acl_columns(classid, &cacheid, &aclattr, &ownerattr))
		return NULL;
	{
		HeapTuple	tup = SearchSysCache1(cacheid, ObjectIdGetDatum(oid));
		Acl		   *out = NULL;

		if (!HeapTupleIsValid(tup))
			return NULL;
		acl = SysCacheGetAttr(cacheid, tup, aclattr, &isnull);
		if (!isnull)
			out = DatumGetAclPCopy(acl);
		ReleaseSysCache(tup);
		return out;
	}
}

/*
 * The ACLs an object (an item, kind and name) has, to put back once a move
 * made it again (restore_saved_acls()): its own and, a relation's, its
 * columns' and those of the sequences it owns, each by name, a NULL one
 * (none: its owner's implicit privileges) as well, so that exactly those
 * come back.
 */
static List *
saved_acls(char kind, const char *name)
{
	List	   *out = NIL;
	Oid			classid;
	Oid			oid = object_named(kind, name, &classid);
	List	   *relids;
	Relation	depend;
	Relation	pg_attribute;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;

	if (!OidIsValid(oid))
		return NIL;
	if (classid != RelationRelationId)
	{
		SavedAcl   *sa = palloc0(sizeof(SavedAcl));

		sa->kind = kind;
		sa->name = pstrdup(name);
		sa->acl = object_acl(classid, oid, NULL, NULL);
		return list_make1(sa);
	}
	relids = list_make1_oid(oid);
	depend = table_open(DependRelationId, AccessShareLock);
	ScanKeyInit(&key[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(oid));
	scan = systable_beginscan(depend, DependReferenceIndexId, true, NULL, 2, key);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_depend d = (Form_pg_depend) GETSTRUCT(tup);

		if (d->classid == RelationRelationId &&
			(d->deptype == DEPENDENCY_AUTO || d->deptype == DEPENDENCY_INTERNAL) &&
			get_rel_relkind(d->objid) == RELKIND_SEQUENCE)
			relids = list_append_unique_oid(relids, d->objid);
	}
	systable_endscan(scan);
	table_close(depend, AccessShareLock);
	pg_attribute = table_open(AttributeRelationId, AccessShareLock);
	foreach_oid(r, relids)
	{
		char	   *rname;
		SavedAcl   *sa;
		ScanKeyData akey;
		SysScanDesc ascan;
		HeapTuple	atup;

		if (!OidIsValid(get_rel_namespace(r)))
			continue;
		rname = quote_qualified_identifier(get_namespace_name(get_rel_namespace(r)),
										   get_rel_name(r));
		sa = palloc0(sizeof(SavedAcl));
		sa->kind = 't';
		sa->name = rname;
		sa->acl = object_acl(RelationRelationId, r, NULL, NULL);
		out = lappend(out, sa);
		ScanKeyInit(&akey, Anum_pg_attribute_attrelid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(r));
		ascan = systable_beginscan(pg_attribute, AttributeRelidNumIndexId, true,
								   NULL, 1, &akey);
		while ((atup = systable_getnext(ascan)) != NULL)
		{
			Form_pg_attribute a = (Form_pg_attribute) GETSTRUCT(atup);

			if (a->attnum <= 0 || a->attisdropped)
				continue;
			sa = palloc0(sizeof(SavedAcl));
			sa->kind = 't';
			sa->name = rname;
			sa->attname = pstrdup(NameStr(a->attname));
			sa->acl = object_acl(RelationRelationId, r, atup, pg_attribute);
			out = lappend(out, sa);
		}
		systable_endscan(ascan);
	}
	table_close(pg_attribute, AccessShareLock);
	return out;
}

/*
 * Put exactly saved_acls()'s ACLs back on the objects (and columns) of
 * their names, made again by the move as their owner: what the making gave
 * a relation (its maker's default privileges, a new sequence's) cleared
 * first, then the same grants, from the same owner, or none.
 */
static void
restore_saved_acls(char kind, const char *name, List *saved)
{
	Oid			classid;
	Oid			oid;

	CommandCounterIncrement();
	oid = object_named(kind, name, &classid);
	if (!OidIsValid(oid))
		return;
	if (classid == RelationRelationId)
		clear_made_privileges(oid);
	foreach_ptr(SavedAcl, sa, saved)
	{
		Oid			cid;
		Oid			o = object_named(sa->kind, sa->name, &cid);

		if (OidIsValid(o))
			set_object_acl(cid, o, sa->attname, sa->acl);
	}
}

/* The owner of the object an item names here (InvalidOid: none) */
static Oid
object_owner(char kind, const char *name)
{
	Oid			classid;
	Oid			oid = object_named(kind, name, &classid);
	int			cacheid;
	AttrNumber	aclattr;
	AttrNumber	ownerattr;
	HeapTuple	tup;
	Oid			owner;

	if (!OidIsValid(oid))
		return InvalidOid;
	if (classid == CollationRelationId)
	{
		tup = SearchSysCache1(COLLOID, ObjectIdGetDatum(oid));
		if (!HeapTupleIsValid(tup))
			return InvalidOid;
		owner = ((Form_pg_collation) GETSTRUCT(tup))->collowner;
		ReleaseSysCache(tup);
		return owner;
	}
	if (!acl_columns(classid, &cacheid, &aclattr, &ownerattr))
		return InvalidOid;
	tup = SearchSysCache1(cacheid, ObjectIdGetDatum(oid));
	if (!HeapTupleIsValid(tup))
		return InvalidOid;
	owner = DatumGetObjectId(SysCacheGetAttrNotNull(cacheid, tup, ownerattr));
	ReleaseSysCache(tup);
	return owner;
}

/*
 * The makers of the objects the branch has (from's items: relations,
 * routines, types, schemas, collations) that a move changes or makes
 * again: each its owner here, where the role moving has that owner's
 * privileges (a superuser, a member of the owning role), so that the
 * owner's code (a check's function, a materialized view's query, a
 * domain's check) never runs with more than the owner's own, and what is
 * made again stays the owner's.  A role without them moves as itself, as
 * its DDL would run: it may change only what it owns, and a writer
 * branch's role only what its base defines (move_authority()).  Before
 * the move's statements are made (move_makers_before() adds those it
 * makes from a commit).  Each changed one's grants are saved, to be put
 * back exactly as they were (save_remade()).
 */
static void
move_owners_before(List *from, List *to)
{
	Oid			mover = GetUserId();

	reset_makers();
	foreach_ptr(MoveItem, f, from)
	{
		MoveItem   *t;
		Oid			owner;
		MoveMaker  *mm;

		if (!relation_kind(f->kind) && f->kind != 'f' && f->kind != 'y' &&
			f->kind != 'n' && f->kind != 'l')
			continue;
		t = find_item(to, f->kind, f->name);
		/* (one unchanged needs none, unless a view, always made again) */
		if (t && strcmp(t->def, f->def) == 0 && f->kind != 'v' && f->kind != 'm' &&
			f->kind != 't')
			continue;
		owner = object_owner(f->kind, f->name);
		if (!OidIsValid(owner) || owner == mover || !has_privs_of_role(mover, owner))
			continue;
		mm = add_maker(f->name, f->kind, owner, false);
		/* (a view is always made again) */
		if (mm && t && (strcmp(t->def, f->def) != 0 || f->kind == 'v') &&
			f->kind != 'n')
			mm->saved = saved_acls(f->kind, f->name);
	}
}

/*
 * Save the grants of an object the branch has (name, of kind) that the
 * move makes again, as its owner, to put back after (restore_saved_acls()).
 */
static void
save_remade(const char *name, char kind)
{
	MoveMaker  *mm = maker_named(name, kind);

	if (mm == NULL || mm->made || mm->saved != NIL)
		return;
	mm->saved = saved_acls(kind, name);
}

/*
 * The relations the tables a move makes are to be: a table it makes again
 * (move_table()'s remade) is the relation it was; one it makes is the
 * relation the caller names (prolly_move_kept), or else a commit the move
 * takes its versions from (prolly_move_provenance) records under its
 * name, as the version is that relation's.  So a table keeps its relation, by OID, across the moves
 * that bring its versions here: a branch's copies have the same OIDs, and
 * a merge of a table the other side made, or a reset to a commit holding
 * one dropped here, makes it again as that relation.  Read before the
 * move's statements.
 */
static List *
move_relids_before(List *from, List *to, List *remade)
{
	ProllyDatabaseComponents dc;
	List	   *out = NIL;
	ListCell   *lc;

	prolly_database_components(&dc);
	foreach(lc, remade)
	{
		MoveRelid  *mr;
		Oid			relid;

		if (find_item(to, 't', lfirst(lc)) == NULL)
			continue;
		relid = RangeVarGetRelid(makeRangeVarFromNameList(stringToQualifiedNameList(lfirst(lc), NULL)),
								 NoLock, true);
		if (!OidIsValid(relid))
			continue;
		mr = palloc0(sizeof(MoveRelid));
		mr->name = lfirst(lc);
		mr->relid = relid;
		out = lappend(out, mr);
	}
	foreach(lc, to)
	{
		MoveItem   *t = lfirst(lc);
		ListCell   *pc;
		bool		given = false;

		if (t->kind != 't' || find_item(from, 't', t->name) != NULL)
			continue;
		foreach_ptr(ProllyKeptRelation, k, prolly_move_kept)
			if (!given && strcmp(k->name, t->name) == 0)
			{
				MoveRelid  *mr = palloc0(sizeof(MoveRelid));

				mr->name = t->name;
				mr->relid = k->relid;
				mr->owner = k->owner;
				out = lappend(out, mr);
				given = true;
			}
		if (given || !PROLLY_HAS(dc.refs, commit_relations))
			continue;
		foreach(pc, prolly_move_provenance)
		{
			List	   *relations;
			ProllyCommitRelation *of = NULL;
			ListCell   *rc;
			MoveRelid  *mr;

			if (!PROLLY_CALL(dc.refs, commit_relations, lfirst(pc), &relations))
				continue;
			foreach(rc, relations)
				if (strcmp(((ProllyCommitRelation *) lfirst(rc))->name, t->name) == 0 &&
					(((ProllyCommitRelation *) lfirst(rc))->relkind == RELKIND_RELATION ||
					 ((ProllyCommitRelation *) lfirst(rc))->relkind == RELKIND_PARTITIONED_TABLE))
					of = lfirst(rc);
			if (of == NULL)
				continue;
			mr = palloc0(sizeof(MoveRelid));
			mr->name = t->name;
			mr->relid = of->relid;
			mr->owner = prolly_recorded_owner(t->name, of->owner, of->owner_name);
			out = lappend(out, mr);
			break;
		}
	}
	return out;
}

/*
 * The relations the move makes as their makers (move_makers): each table
 * it makes (from has none of its name) that relids names a maker for (its
 * recorded owner, or the one prolly_move_kept names), the sequences it
 * owns that the move makes, and each materialized view it makes that a
 * commit it takes versions from (prolly_move_provenance) records, first,
 * under its name: as that record's owner.
 */
static void
move_makers_before(List *from, List *to, List *relids)
{
	ProllyDatabaseComponents dc;

	foreach_ptr(MoveRelid, mr, relids)
	{
		MoveItem   *t;
		TableDef	td;

		if (!OidIsValid(mr->owner) || find_item(from, 't', mr->name) != NULL ||
			(t = find_item(to, 't', mr->name)) == NULL)
			continue;
		add_maker(mr->name, 't', mr->owner, true);
		parse_table_def(t->def, &td);
		foreach_ptr(char, seq, table_sequences(mr->name, &td))
			if (find_item(to, 's', seq) != NULL && find_item(from, 's', seq) == NULL)
				add_maker(seq, 'S', mr->owner, true);
	}
	/*
	 * (the sequences a table the branch has, defined otherwise, comes to
	 * own: an identity column added, or the table made again, makes its
	 * sequence as the table's owner)
	 */
	foreach_ptr(MoveItem, t, to)
	{
		MoveItem   *f;
		MoveMaker  *mm;
		TableDef	td;

		if (t->kind != 't' || (f = find_item(from, 't', t->name)) == NULL ||
			strcmp(f->def, t->def) == 0 || (mm = maker_named(t->name, 't')) == NULL)
			continue;
		parse_table_def(t->def, &td);
		foreach_ptr(char, seq, table_sequences(t->name, &td))
			if (find_item(to, 's', seq) != NULL)
				add_maker(seq, 'S', mm->owner, false);
	}
	prolly_database_components(&dc);
	if (prolly_move_provenance == NIL || !PROLLY_HAS(dc.refs, commit_relations))
		return;

	/*
	 * Every other object it makes from a commit (one from hasn't): a
	 * materialized view, view, sequence no table owns, foreign table,
	 * routine, type, schema or collation, as the owner the first of the
	 * commits recording it names.
	 */
	{
		HTAB	   *recorded = maker_table("prolly recorded owners",
											   CurrentMemoryContext);
		foreach_ptr(ProllyHash, commit, prolly_move_provenance)
		{
			List	   *relations;

			if (!PROLLY_CALL(dc.refs, commit_relations, commit, &relations))
				continue;
			foreach_ptr(ProllyCommitRelation, r, relations)
			{
				MakerEntry *e;
				bool		found;

				e = maker_entry(recorded, CurrentMemoryContext, r->name,
								r->relkind, true, &found);
				if (!found)
					e->mm = r;	/* (the first commit's) */
			}
		}
		foreach_ptr(MoveItem, t, to)
		{
			char		rkind;
			MakerEntry *e;
			ProllyCommitRelation *of;

			switch (t->kind)
			{
				case 'm':
					rkind = RELKIND_MATVIEW;
					break;
				case 'v':
					rkind = 'v';
					break;
				case 's':
					rkind = 'S';
					break;
				case 'r':
					rkind = 'f';
					break;
				case 'f':
					rkind = 'P';
					break;
				case 'y':
					rkind = 'T';
					break;
				case 'n':
					rkind = 'N';
					break;
				case 'l':
					rkind = 'C';
					break;
				default:
					continue;
			}
			if (find_item(from, t->kind, t->name) != NULL ||
				(t->kind == 's' && maker_named(t->name, 'S') != NULL))
				continue;
			e = maker_entry(recorded, NULL, t->name, rkind, false, NULL);
			if (e == NULL)
				continue;
			of = (ProllyCommitRelation *) e->mm;
			add_maker(t->name, t->kind,
					  prolly_recorded_owner(t->name, of->owner, of->owner_name), true);
		}
	}
}

/*
 * The OID a table named relname in schema relnamespace, made now, is to
 * have: the relation a move making it says it is (move_relids_before()),
 * if the branch has no relation of that OID (and the relation cache's
 * entry of it, another branch's, isn't open); else InvalidOid (a new one).
 * Asked as the relation is made (heap_create_with_catalog()), once, only
 * while the move's statements run: the name's entry is used up.
 */
Oid
prolly_move_kept_relid(const char *relname, Oid relnamespace)
{
	char	   *nsp;
	char	   *name;
	ListCell   *lc;
	Oid			relid = InvalidOid;

	if (move_kept_relids == NIL)
		return InvalidOid;
	nsp = get_namespace_name(relnamespace);
	if (nsp == NULL)
		return InvalidOid;
	name = quote_qualified_identifier(nsp, relname);
	foreach(lc, move_kept_relids)
	{
		MoveRelid  *mr = lfirst(lc);

		if (strcmp(mr->name, name) != 0)
			continue;
		relid = mr->relid;
		move_kept_relids = foreach_delete_current(move_kept_relids, lc);
		break;
	}
	pfree(name);
	if (OidIsValid(relid) &&
		(SearchSysCacheExists1(RELOID, ObjectIdGetDatum(relid)) ||
		 !RelationForgetForNewRelation(relid)))
		relid = InvalidOid;
	return relid;
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
 * Whether ALTER TABLE name ALTER COLUMN col TYPE type USING col::type, as
 * move_table() runs it, rewrites the table: decided as the ALTER decides
 * it, from the conversion of the column's value, planned.  One that is
 * binary coercible (varchar(10) to varchar(20), varchar to text) doesn't:
 * the rows stay where they are, and a snapshot from before the move still
 * sees them.  False when that can't be told yet: the table, its column or
 * the type isn't there (the move makes it later).
 */
static bool
retype_rewrites(const char *name, const char *col, const char *type)
{
	Oid			relid = RangeVarGetRelid(makeRangeVarFromNameList(stringToQualifiedNameList(name, NULL)),
										 AccessShareLock, true);
	List	   *raw;
	ResTarget  *rt;
	Node	   *cast;
	ColumnRef  *ref;
	AttrNumber	attnum;
	Type		typ;
	Relation	rel;
	ParseState *pstate;
	ParseNamespaceItem *nsitem;
	Node	   *expr;

	if (!OidIsValid(relid))
		return false;
	raw = raw_parser(psprintf("SELECT %s::%s", col, type), RAW_PARSE_DEFAULT);
	rt = linitial_node(ResTarget,
					   ((SelectStmt *) linitial_node(RawStmt, raw)->stmt)->targetList);
	/* (col::type, or (col::type) COLLATE collation) */
	cast = rt->val;
	if (IsA(cast, CollateClause))
		cast = ((CollateClause *) cast)->arg;
	ref = castNode(ColumnRef, castNode(TypeCast, cast)->arg);
	attnum = get_attnum(relid, strVal(llast(ref->fields)));
	if (attnum == InvalidAttrNumber)
		return false;
	typ = LookupTypeName(NULL, castNode(TypeCast, cast)->typeName, NULL, true);
	if (typ == NULL)
		return false;
	ReleaseSysCache(typ);
	/* (a virtual generated column has no rows to rewrite) */
	if (get_attgenerated(relid, attnum) == ATTRIBUTE_GENERATED_VIRTUAL)
		return false;
	rel = relation_open(relid, NoLock);
	pstate = make_parsestate(NULL);
	nsitem = addRangeTableEntryForRelation(pstate, rel, AccessShareLock,
										   NULL, false, true);
	addNSItemToQuery(pstate, nsitem, false, true, true);
	expr = transformExpr(pstate, rt->val, EXPR_KIND_ALTER_COL_TRANSFORM);
	assign_expr_collations(pstate, expr);
	expr = (Node *) expression_planner((Expr *) expr);
	free_parsestate(pstate);
	relation_close(rel, NoLock);
	return ATColumnChangeRequiresRewrite(expr, attnum);
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
	List	   *not_null_drops = NIL;
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
				if (sm->rewritten && !sm->render &&
					!list_has_string(sm->at_once, name) &&
					retype_rewrites(name, col, column_type_sql(line)))
					sm->at_once = lappend(sm->at_once, pstrdup(name));
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
		/*
		 * (a column's NOT NULL after the rest: one a primary key going holds
		 * can't go before the key, whatever their names' order, as when two
		 * tables' names were swapped)
		 */
		else if (strstr(lfirst(lc), "\tNOT NULL ") != NULL)
			not_null_drops = lappend(not_null_drops, sql);
		else
			sm->before = lappend(sm->before, sql);
	}
	sm->before = list_concat(sm->before, not_null_drops);
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
/*
 * Does an extension live in versioned schema name?  Then every branch keeps
 * the schema, and a move to a state without it leaves it, empty but for the
 * extension's objects, which are every branch's.
 */
static bool
holds_extension(const char *name)
{
	char	   *plain = strVal(linitial(stringToQualifiedNameList(name, NULL)));
	Oid			nsp = get_namespace_oid(plain, true);

	return OidIsValid(nsp) && OidIsValid(extension_in_namespace(nsp));
}

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
 * Is ed an error a move's definition may fail with only for what another
 * of the move's statements makes, drops or changes first, so that it may
 * succeed once those have run?  A routine with a SQL-standard body, a view
 * or a materialized view is parsed against the objects it uses, which may
 * not be made yet, or not be as they will be (a column it groups by not
 * yet functionally dependent on a primary key the move adds, a type not
 * yet changed): any error of the statement's own is such a failure, and
 * one that never succeeds is raised once no statement makes progress
 * (make_views()).  Not so a cancel, a timeout or a shutdown (class 57),
 * running out of memory or disk (53), a system error (58), an internal
 * error or corruption (XX), a deadlock or serialization failure (40), or
 * a lock not granted in time: those are the move's, not the statement's,
 * and are raised as they are.
 */
static bool
waits_on_another(const ErrorData *ed)
{
	switch (ERRCODE_TO_CATEGORY(ed->sqlerrcode))
	{
		case ERRCODE_OPERATOR_INTERVENTION:
		case ERRCODE_INSUFFICIENT_RESOURCES:
		case ERRCODE_SYSTEM_ERROR:
		case ERRCODE_INTERNAL_ERROR:
		case ERRCODE_TRANSACTION_ROLLBACK:
			return false;
		default:
			return ed->sqlerrcode != ERRCODE_LOCK_NOT_AVAILABLE;
	}
}

/*
 * Run sql in a subtransaction of its own: true if it ran.  If it failed
 * for another of the move's statements (waits_on_another()), its error is
 * returned in *failure, in mcxt, the subtransaction rolled back; any other
 * error is raised again, the subtransaction rolled back first so that
 * whatever catches it finds its own (sub)transaction current.
 */
static bool
try_statement(const char *sql, MemoryContext mcxt, ResourceOwner owner,
			  ErrorData **failure)
{
	bool		ran = false;

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(mcxt);
	PG_TRY();
	{
		int			rc;

		INJECTION_POINT("prolly-move-try-statement", NULL);
		rc = execute_statement(sql);
		if (rc < 0)
			elog(ERROR, "could not run \"%s\": %s", sql,
				 SPI_result_code_string(rc));
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(mcxt);
		CurrentResourceOwner = owner;
		ran = true;
	}
	PG_CATCH();
	{
		ErrorData  *ed;

		MemoryContextSwitchTo(mcxt);
		ed = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(mcxt);
		CurrentResourceOwner = owner;
		if (!waits_on_another(ed))
			ReThrowError(ed);
		/* (a schema conflict swallowed here isn't the caller's: prolly_merge.h) */
		prolly_schema_conflict_raised = false;
		if (failure)
			*failure = ed;
		else
			FreeErrorData(ed);
	}
	PG_END_TRY();
	return ran;
}

/*
 * Run each statement in a subtransaction of its own: those that fail for
 * another statement of the move (waits_on_another()), left for later (each
 * as is), in order.
 */
static List *
try_statements(List *stmts)
{
	MemoryContext mcxt = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	List	   *failed = NIL;
	ListCell   *lc;

	foreach(lc, stmts)
	{
		if (!try_statement(lfirst(lc), mcxt, owner, NULL))
			failed = lappend(failed, lfirst(lc));
	}
	return failed;
}

/*
 * Make views, each after the views it uses, or drop them, each after those
 * using it: those that fail for another (waits_on_another()) are tried
 * again once others are done, until none can be.
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
			ErrorData  *ed = NULL;

			if (!try_statement(lfirst(lc), mcxt, owner, &ed))
			{
				if (last)
					FreeErrorData(last);
				last = ed;
				failed = lappend(failed, lfirst(lc));
			}
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
		int			rc = execute_statement(lfirst(lc));

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

/*
 * The routines (with SQL-standard bodies) using foreign table name, which
 * goes to be made again: each dropped (appended to drops) before it, and
 * made again after as to defines it, if it does (appended to makes).
 */
static void
foreign_table_routines(const char *name, List *from, List *to,
					   List **drops, List **makes)
{
	Oid			relid = RangeVarGetRelid(makeRangeVarFromNameList(stringToQualifiedNameList(name, NULL)),
										 NoLock, true);
	Relation	pg_depend;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;

	if (!OidIsValid(relid))
		return;
	pg_depend = table_open(DependRelationId, AccessShareLock);
	ScanKeyInit(&key[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(relid));
	scan = systable_beginscan(pg_depend, DependReferenceIndexId, true, NULL, 2, key);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_depend d = (Form_pg_depend) GETSTRUCT(tup);
		HeapTuple	pt;
		Form_pg_proc proc;
		char	   *fname;
		MoveItem   *fp;
		MoveItem   *tp;

		if (d->classid != ProcedureRelationId)
			continue;
		pt = SearchSysCache1(PROCOID, ObjectIdGetDatum(d->objid));
		if (!HeapTupleIsValid(pt))
			elog(ERROR, "cache lookup failed for function %u", d->objid);
		proc = (Form_pg_proc) GETSTRUCT(pt);
		fname = psprintf("%s(%s)",
						 qualified(proc->pronamespace, NameStr(proc->proname)),
						 TextDatumGetCString(DirectFunctionCall1(pg_get_function_identity_arguments,
																 ObjectIdGetDatum(d->objid))));
		ReleaseSysCache(pt);
		fp = find_item(from, 'f', fname);
		tp = find_item(to, 'f', fname);
		if (fp == NULL || list_has_string(*drops, drop_routine_sql(fp)))
			continue;
		*drops = lappend(*drops, drop_routine_sql(fp));
		made_as_item(llast(*drops), fp->name, 'f');
		save_remade(fp->name, 'f');
		if (tp)
		{
			*makes = lappend(*makes, tp->def);
			made_as_item(tp->def, tp->name, 'f');
		}
	}
	systable_endscan(scan);
	table_close(pg_depend, AccessShareLock);
}

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
	List	   *foreign_remakes;	/* foreign tables using them, dropped
									 * before and made again (qualified) */
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
 *
 * A column's conversion runs as its table's maker, whoever owns the type:
 * each routine the type's checks or casts call runs as its own owner
 * (prolly_move_calls_begin()), so no type owner's routine gets the table
 * owner's authority.  A domain's check expression otherwise runs as it
 * does for every row the table's owner writes, which PostgreSQL runs as
 * the writer too.
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
			ListMark	m = list_mark(tm.unvalidated);

			if (range_in_use(typid))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("the branch can't be moved to a commit that defines type %s otherwise",
								f->name),
						 errdetail("A range type in use can't be changed in place; a move can't change one either.")));

			tm.before = lappend(tm.before, psprintf("DROP TYPE %s", f->name));
			made_as_item(llast(tm.before), f->name, 'y');
			tm.creates = lappend(tm.creates, create_type_sql(t->name, t->def,
															 &tm.unvalidated));
			made_as_item(llast(tm.creates), t->name, 'y');
			made_as_since(tm.unvalidated, m, t->name, 'y');
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
					else if (kind == RELKIND_FOREIGN_TABLE && versioned)
					{
						/* (no rows: it goes, and is made again) */
						char	   *rel = qualified(get_rel_namespace(d->objid), get_rel_name(d->objid));

						if (!list_has_string(tm.foreign_remakes, rel))
							tm.foreign_remakes = lappend(tm.foreign_remakes, rel);
						handled = true;
					}
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
								made_as_item(llast(tm.before), rel, 't');
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
							/* (the helper too: made and dropped by the table's owner) */
							made_as_from(tm.converts, list_length(tm.converts) - 3, rel, 't');
						}
						else if (!inherited)
						{
							tm.converts = lappend(tm.converts,
												  psprintf("ALTER TABLE %s ALTER COLUMN %s TYPE %s USING %s",
														   rel, col, mt->to->name,
														   converted_value(col, mt->from->def, mt->to->def,
																		   mt->to->name)));
							made_as_item(llast(tm.converts), rel, 't');
						}
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

						/* (an index is its table's: dropped and made by its owner) */
						tm.before = lcons(psprintf("DROP INDEX IF EXISTS %s.%s",
												   schema_of(tname), idx), tm.before);
						made_as_item(linitial(tm.before), tname, 't');
						if (same_table_line(ft, tt, 'i', idx))
						{
							tm.after = lappend(tm.after, pstrdup(table_def_line(tt, 'i', idx)));
							made_as_item(llast(tm.after), tname, 't');
						}
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
							 get_rel_relkind(con->conrelid) == RELKIND_FOREIGN_TABLE &&
							 namespace_is_versioned(get_rel_namespace(con->conrelid)))
					{
						/* (a foreign table's: made again with it) */
						char	   *rel = qualified(get_rel_namespace(con->conrelid),
													get_rel_name(con->conrelid));

						if (!list_has_string(tm.foreign_remakes, rel))
							tm.foreign_remakes = lappend(tm.foreign_remakes, rel);
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
						made_as_item(linitial(tm.before), tname, 't');
						if (same_table_line(ft, tt, 'k', cname))
						{
							tm.after = lappend(tm.after,
											   psprintf("ALTER TABLE %s ADD CONSTRAINT %s %s", tname, cname,
														constraint_def(table_def_line(tt, 'k', cname))));
							made_as_item(llast(tm.after), tname, 't');
						}
						handled = true;
					}
					ReleaseSysCache(ct);
				}
				else if (d->classid == AttrDefaultRelationId)
				{
					ObjectAddress col = GetAttrDefaultColumnAddress(d->objid);

					if (OidIsValid(col.objectId) &&
						get_rel_relkind(col.objectId) == RELKIND_FOREIGN_TABLE &&
						namespace_is_versioned(get_rel_namespace(col.objectId)))
					{
						/* (a foreign table's: made again with it) */
						char	   *rel = qualified(get_rel_namespace(col.objectId),
													get_rel_name(col.objectId));

						if (!list_has_string(tm.foreign_remakes, rel))
							tm.foreign_remakes = lappend(tm.foreign_remakes, rel);
						handled = true;
					}
					else if (OidIsValid(col.objectId) &&
							 namespace_is_versioned(get_rel_namespace(col.objectId)))
					{
						char	   *tname = qualified(get_rel_namespace(col.objectId),
													  get_rel_name(col.objectId));
						const char *cname = quote_identifier(get_attname(col.objectId, col.objectSubId, false));
						MoveItem   *ft = find_item(from, 't', tname);
						MoveItem   *tt = find_item(to, 't', tname);

						const char *fl = ft ? table_def_line(ft, 'c', cname) : NULL;
						const char *tl = tt ? table_def_line(tt, 'c', cname) : NULL;
						const char *fdef = fl ? column_attr(fl, "default ") : NULL;
						const char *tdef = tl ? column_attr(tl, "default ") : NULL;

						tm.before = lappend(tm.before,
											psprintf("ALTER TABLE %s ALTER COLUMN %s DROP DEFAULT",
													 tname, cname));
						made_as_item(llast(tm.before), tname, 't');

						/*
						 * (one the move keeps, put back: move_table() sets only a
						 * default whose text changed, whatever else of the
						 * column's line did, its storage or statistics)
						 */
						if (fdef && tdef && strcmp(fdef, tdef) == 0)
						{
							tm.after = lappend(tm.after,
											   psprintf("ALTER TABLE %s ALTER COLUMN %s SET DEFAULT %s",
														tname, cname, tdef));
							made_as_item(llast(tm.after), tname, 't');
						}
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
						made_as_item(llast(tm.aggregate_drops), fp->name, 'f');
						handled = true;
					}
					else if (fp)
					{
						tm.before = lcons(drop_routine_sql(fp), tm.before);
						made_as_item(linitial(tm.before), fp->name, 'f');
						if (tp && strcmp(tp->def, fp->def) == 0)
						{
							tm.after = lappend(tm.after, tp->def);
							made_as_item(tp->def, tp->name, 'f');
							save_remade(fp->name, 'f');
						}
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

		ListMark	m = list_mark(tm.unvalidated);

		/* (the type renamed aside is still the branch's, and its owner's) */
		tm.before = lappend(tm.before,
							psprintf("ALTER TYPE %s RENAME TO %s", mt->from->name,
									 strrchr(mt->aside, '.') + 1));
		made_as_item(llast(tm.before), mt->from->name, 'y');
		tm.creates = lappend(tm.creates, create_type_sql(mt->to->name, mt->to->def,
														 &tm.unvalidated));
		made_as_item(llast(tm.creates), mt->to->name, 'y');
		made_as_since(tm.unvalidated, m, mt->to->name, 'y');
		tm.drops = lappend(tm.drops, psprintf("DROP TYPE %s", mt->aside));
		made_as_item(llast(tm.drops), mt->from->name, 'y');
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
		Oid			owner;
		char	   *sql;

		if (!OidIsValid(idx))
			continue;			/* the move dropped it */
		kind = get_rel_relkind(idx);
		if ((kind != RELKIND_INDEX && kind != RELKIND_PARTITIONED_INDEX) ||
			get_rel_tablespace(idx) == p->tablespace)
			continue;			/* not made again, or another object */
		spc = get_tablespace_name(p->tablespace);
		if (spc == NULL)
			continue;
		/* (as its table's owner, restricted, where the move runs as one) */
		owner = move_makers != NIL ? relation_owner_of(idx) : InvalidOid;
		if (p->tablespace != MyDatabaseTableSpace &&
			object_aclcheck(TableSpaceRelationId, p->tablespace,
							OidIsValid(owner) ? owner : GetUserId(),
							ACL_CREATE) != ACLCHECK_OK)
		{
			ereport(WARNING,
					(errmsg("index \"%s.%s\" made again in the default tablespace, not in \"%s\"",
							p->schema, p->index, spc),
					 errdetail("Permission to create in tablespace \"%s\" is needed to move it back.",
							   spc)));
			continue;
		}
		sql = psprintf("ALTER INDEX %s SET TABLESPACE %s",
					   quote_qualified_identifier(p->schema, p->index),
					   quote_identifier(spc));
		made_as(sql, owner);
		run_statements(list_make1(sql));
	}
}

/*
 * The table name (qualified) emptied, each partition of it with storage, by
 * the applier the move writes rows with: no trigger fires (one enabled
 * ALWAYS neither), no rule applies and no foreign key is checked, and a
 * table row-level security applies to is refused, as its rows written
 * would be.  at_once (the ALTER that follows rewrites the table anyway):
 * every row at once if the applier can (#492), the table given new
 * storage, as TRUNCATE gives it.  Else a row read is removed as it is:
 * the scan's snapshot keeps seeing those removed, as a snapshot from
 * before the move does, which new storage would leave none to see.
 */
static void
empty_table(const char *name, bool at_once)
{
	const ProllyApplierRoutine *ar = prolly_applier_lookup("replication");
	bool		remove_all = at_once && PROLLY_HAS(ar, remove_all);

	/*
	 * (Emptied at once, the table is locked ACCESS EXCLUSIVE before its
	 * partitions are, as TRUNCATE and the ALTER after lock them: a query
	 * on it locks it before them, and would deadlock with a move holding a
	 * partition and waiting for the table.)
	 */
	Oid			relid = RangeVarGetRelid(makeRangeVarFromNameList(stringToQualifiedNameList(name, NULL)),
										 remove_all ? AccessExclusiveLock : RowExclusiveLock,
										 false);
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
		if (remove_all && PROLLY_CALL(ar, remove_all, a))
		{
			PROLLY_CALL(ar, end, a);
			table_close(rel, NoLock);
			continue;
		}
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

/* the database's owner */
static Oid
get_database_owner_role(void)
{
	HeapTuple	tup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
	Oid			owner;

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for database %u", MyDatabaseId);
	owner = ((Form_pg_database) GETSTRUCT(tup))->datdba;
	ReleaseSysCache(tup);
	return owner;
}

/* A table's policies, as items has its definition (NIL: none, or no table). */
static List *
table_policies(List *items, const char *name)
{
	MoveItem   *it = items ? find_item(items, 't', name) : NULL;
	TableDef	td;

	if (it == NULL)
		return NIL;
	parse_table_def(it->def, &td);
	return td.policies;
}

static bool
same_statements(List *a, List *b)
{
	ListCell   *la;
	ListCell   *lb;

	if (list_length(a) != list_length(b))
		return false;
	forboth(la, a, lb, b)
		if (strcmp(lfirst(la), lfirst(lb)) != 0)
			return false;
	return true;
}

/*
 * A policy statement of a move, on table, and whether it may run as the
 * table's owner when the session's role isn't it: the move leaves the
 * table's policies as they are, or makes them its writer branch's base's.
 */
typedef struct PolicyStatement
{
	char	   *sql;
	char	   *table;
	bool		owners;
} PolicyStatement;

static PolicyStatement *
policy_statement(char *sql, const char *table, List *from, List *to, List *base)
{
	PolicyStatement *ps = palloc(sizeof(PolicyStatement));
	List	   *now = table_policies(to, table);

	ps->sql = sql;
	ps->table = pstrdup(table);
	ps->owners = same_statements(table_policies(from, table), now) ||
		(base != NIL && same_statements(table_policies(base, table), now));
	return ps;
}

/*
 * Run the policy statements, each as the session's role, or, where it may
 * (PolicyStatement.owners) and the role hasn't its table owner's privileges,
 * as that owner.  A move drops and makes again every table's policies, as
 * they may use what it changes; one whose table's policies it leaves as
 * they are, or makes as the base's, writes no policy of the role's own
 * making, so the table's owner's authority is the one it needs.
 */
static void
run_policy_statements(List *stmts)
{
	ListCell   *lc;

	foreach(lc, stmts)
	{
		PolicyStatement *ps = lfirst(lc);
		Oid			relid = InvalidOid;
		Oid			owner = InvalidOid;
		Oid			save_user;
		int			save_sec;
		int			rc;

		/* (a policy of a table the move makes or changes as its maker) */
		bool		attributed;

		if (OidIsValid(statement_maker(ps->sql, &attributed)) ||
			(!attributed && move_makers != NIL))
		{
			/* (execute_statement() refuses one nothing was said of) */
			rc = execute_statement(ps->sql);
			if (rc < 0)
				elog(ERROR, "could not run \"%s\": %s", ps->sql,
					 SPI_result_code_string(rc));
			continue;
		}
		if (ps->owners)
		{
			relid = RangeVarGetRelid(makeRangeVarFromNameList(stringToQualifiedNameList(ps->table, NULL)),
									 NoLock, true);
			if (OidIsValid(relid))
			{
				HeapTuple	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));

				if (HeapTupleIsValid(tup))
				{
					owner = ((Form_pg_class) GETSTRUCT(tup))->relowner;
					ReleaseSysCache(tup);
				}
			}
		}
		GetUserIdAndSecContext(&save_user, &save_sec);
		if (OidIsValid(owner) && !has_privs_of_role(save_user, owner))
		{
			SetUserIdAndSecContext(owner, save_sec | SECURITY_LOCAL_USERID_CHANGE |
								   SECURITY_RESTRICTED_OPERATION);
			PG_TRY();
			{
				rc = SPI_execute(ps->sql, false, 0);
			}
			PG_FINALLY();
			{
				SetUserIdAndSecContext(save_user, save_sec);
			}
			PG_END_TRY();
		}
		else
			rc = SPI_execute(ps->sql, false, 0);
		if (rc < 0)
			elog(ERROR, "could not run \"%s\": %s", ps->sql,
				 SPI_result_code_string(rc));
	}
}

/*
 * Does every definition a move changes (to's objects defined otherwise than
 * from's, or that from hasn't; from's that to hasn't) become its writer
 * branch's base's (base, the base's head's items)?  Then the move makes only
 * what the base's writers defined, which a writer branch's role may take
 * from its base whatever it owns: a reset to the base, a merge of it, the
 * fast-forward of an integration.
 */
static bool
definitions_of_base(List *from, List *to, List *base)
{
	ListCell   *lc;

	foreach(lc, to)
	{
		MoveItem   *t = lfirst(lc);
		MoveItem   *f = find_item(from, t->kind, t->name);
		MoveItem   *b;

		if (f != NULL && strcmp(f->def, t->def) == 0)
			continue;
		b = find_item(base, t->kind, t->name);
		if (b == NULL || strcmp(b->def, t->def) != 0)
			return false;
	}
	foreach(lc, from)
	{
		MoveItem   *f = lfirst(lc);

		if (find_item(to, f->kind, f->name) == NULL &&
			find_item(base, f->kind, f->name) != NULL)
			return false;
	}
	return true;
}

/*
 * The owner of the object a move's item names, as the session's branch has
 * it now; InvalidOid if it has none of the name, or the item is of a kind
 * whose owner isn't looked up so (routines, types, collations).
 */
static Oid
item_owner(const MoveItem *it, bool *known)
{
	Oid			owner = InvalidOid;

	*known = true;
	if (it->kind == 'n')
	{
		HeapTuple	tup = SearchSysCache1(NAMESPACENAME,
										  CStringGetDatum(unquoted(it->name)));

		if (HeapTupleIsValid(tup))
		{
			owner = ((Form_pg_namespace) GETSTRUCT(tup))->nspowner;
			ReleaseSysCache(tup);
		}
	}
	else if (it->kind == 't' || it->kind == 'v' || it->kind == 'm' ||
			 it->kind == 's' || it->kind == 'f')
	{
		Oid			relid = relation_named(it->name);
		HeapTuple	tup;

		if (OidIsValid(relid) &&
			HeapTupleIsValid(tup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid))))
		{
			owner = ((Form_pg_class) GETSTRUCT(tup))->relowner;
			ReleaseSysCache(tup);
		}
	}
	else
		*known = false;
	return owner;
}

/*
 * The role a writer branch's move to its base's definitions
 * (definitions_of_base()) runs its statements as: the least that may make
 * them.  The owner of every object it changes or drops, if they are one
 * role, and the database's owner for those it makes (made by it, they are
 * its, as the base's writers' would be theirs had they made them here);
 * else, objects of several owners, or of a kind whose owner isn't looked
 * up, the bootstrap superuser, as no one role may make all of it.  The
 * move runs no user code with it: its statements are deparsed definitions,
 * run with check_function_bodies off, and neither triggers nor event
 * triggers fire for a move's own DDL (internal history work's), so the
 * authority makes only the base's definitions.
 */
static Oid
move_authority(List *from, List *to)
{
	Oid			authority = InvalidOid;
	ListCell   *lc;

#define AUTHORITY_IS(role) \
	do { \
		if (!OidIsValid(authority)) \
			authority = (role); \
		else if (authority != (role)) \
			return BOOTSTRAP_SUPERUSERID; \
	} while (0)

	foreach(lc, to)
	{
		MoveItem   *t = lfirst(lc);
		MoveItem   *f = find_item(from, t->kind, t->name);
		bool		known;
		Oid			owner;

		if (f != NULL && strcmp(f->def, t->def) == 0)
			continue;
		owner = item_owner(t, &known);
		if (!known)
			return BOOTSTRAP_SUPERUSERID;
		AUTHORITY_IS(OidIsValid(owner) ? owner : get_database_owner_role());
	}
	foreach(lc, from)
	{
		MoveItem   *f = lfirst(lc);
		bool		known;
		Oid			owner;

		if (find_item(to, f->kind, f->name) != NULL)
			continue;
		owner = item_owner(f, &known);
		if (!known || !OidIsValid(owner))
			return BOOTSTRAP_SUPERUSERID;
		AUTHORITY_IS(owner);
	}
#undef AUTHORITY_IS
	return OidIsValid(authority) ? authority : get_database_owner_role();
}

/*
 * The definitions of the session's branch's writer base, if the session is
 * on a writer branch: its head commit's items (NIL: not a writer branch, or
 * a base without commits), and *root, the head's root.
 */
static List *
writer_base_items(Canon *cn, ProllyNodeRef *root)
{
	ProllyDatabaseComponents dc;
	char	   *writer;
	char	   *base;
	Oid			role;
	ProllyHash	head;
	ProllyCommitInfo info;

	memset(root, 0, sizeof(*root));
	prolly_database_components(&dc);
	if (!PROLLY_HAS(dc.writers, writer_branch) ||
		!PROLLY_CALL(dc.writers, writer_branch, branch_ops()->current(),
					 &writer, &base, &role) ||
		!PROLLY_CALL(dc.refs, resolve, base, &head) ||
		!PROLLY_CALL(dc.refs, commit_get, &head, &info))
		return NIL;
	*root = info.root;
	return move_items(cn, &info.root);
}

/*
 * The routines depending on the object (refclassid, refobjid, and its
 * column refsubid, or any of its columns if -1), added to *procs;
 * *matviews set if a materialized view does.
 */
static void
add_dependents(Oid refclassid, Oid refobjid, int32 refsubid, List **procs,
			   bool *matviews)
{
	Relation	depRel = table_open(DependRelationId, AccessShareLock);
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple	tup;

	ScanKeyInit(&key[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(refclassid));
	ScanKeyInit(&key[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(refobjid));
	ScanKeyInit(&key[2], Anum_pg_depend_refobjsubid, BTEqualStrategyNumber,
				F_INT4EQ, Int32GetDatum(refsubid));
	scan = systable_beginscan(depRel, DependReferenceIndexId, true, NULL,
							  refsubid >= 0 ? 3 : 2, key);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_depend d = (Form_pg_depend) GETSTRUCT(tup);

		if (d->classid == ProcedureRelationId)
			*procs = list_append_unique_oid(*procs, d->objid);
		else if (d->classid == RewriteRelationId)
		{
			Relation	rwRel = table_open(RewriteRelationId, AccessShareLock);
			ScanKeyData rkey;
			SysScanDesc rscan;
			HeapTuple	rtup;

			ScanKeyInit(&rkey, Anum_pg_rewrite_oid, BTEqualStrategyNumber,
						F_OIDEQ, ObjectIdGetDatum(d->objid));
			rscan = systable_beginscan(rwRel, RewriteOidIndexId, true, NULL, 1, &rkey);
			if ((rtup = systable_getnext(rscan)) != NULL &&
				get_rel_relkind(((Form_pg_rewrite) GETSTRUCT(rtup))->ev_class) ==
				RELKIND_MATVIEW)
				*matviews = true;
			systable_endscan(rscan);
			table_close(rwRel, AccessShareLock);
		}
	}
	systable_endscan(scan);
	table_close(depRel, AccessShareLock);
}

/*
 * What the move drops of from's tables, and the routines (their OIDs) that
 * use it: a table that goes, or is made again (table_remade()), a column
 * dropped, and a constraint whose line to doesn't have.  A routine with a
 * SQL-standard body depends on the tables and columns it reads, and on a
 * primary key it relies on (to group by it, selecting the columns the key
 * determines), so it would keep them from going: those are returned.  A
 * routine of any language whose arguments or result are a table's row type
 * depends on the type, which goes with the table: those are *retyped.
 * *matviews is set if a materialized view depends on what goes: those go
 * and come again.  Tables the move leaves as they are aren't looked up.
 */
static List *
routines_on_going(List *from, List *to, List **retyped, bool *matviews)
{
	List	   *procs = NIL;
	ListCell   *lc;

	*retyped = NIL;
	foreach(lc, from)
	{
		MoveItem   *f = lfirst(lc);
		MoveItem   *t;
		TableDef	ftd;
		TableDef	ttd;
		bool		gone;
		List	   *going = NIL;
		List	   *columns = NIL;	/* names of the columns dropped */
		Oid			relid;
		Relation	conrel;
		ScanKeyData key;
		SysScanDesc scan;
		HeapTuple	tup;
		ListCell   *c;

		if (f->kind != 't')
			continue;
		t = find_item(to, 't', f->name);
		if (t && strcmp(t->def, f->def) == 0)
			continue;
		parse_table_def(f->def, &ftd);
		if (t)
			parse_table_def(t->def, &ttd);
		gone = t == NULL || table_remade(&ftd, &ttd);
		if (!gone)
		{
			List	   *to_names = NIL;

			foreach(c, ftd.constraints)
				if (!list_has_string(ttd.constraints, lfirst(c)))
					going = lappend(going, first_field(lfirst(c)));
			foreach(c, ttd.columns)
				to_names = lappend(to_names, first_field(lfirst(c)));
			foreach(c, ftd.columns)
				if (!list_has_string(to_names, first_field(lfirst(c))))
					columns = lappend(columns, first_field(lfirst(c)));
			if (going == NIL && columns == NIL)
				continue;
		}
		relid = RangeVarGetRelid(makeRangeVarFromNameList(stringToQualifiedNameList(f->name, NULL)),
								 NoLock, true);
		if (!OidIsValid(relid))
			continue;
		if (gone)
		{
			add_dependents(RelationRelationId, relid, -1, &procs, matviews);
			add_dependents(TypeRelationId, get_rel_type_id(relid), -1, retyped,
						   matviews);
		}
		for (AttrNumber att = 1; columns != NIL; att++)
		{
			HeapTuple	atup = SearchSysCache2(ATTNUM, ObjectIdGetDatum(relid),
											   Int16GetDatum(att));
			Form_pg_attribute attr;

			if (!HeapTupleIsValid(atup))
				break;
			attr = (Form_pg_attribute) GETSTRUCT(atup);
			if (!attr->attisdropped &&
				list_has_string(columns, quote_identifier(NameStr(attr->attname))))
				add_dependents(RelationRelationId, relid, att, &procs, matviews);
			ReleaseSysCache(atup);
		}
		conrel = table_open(ConstraintRelationId, AccessShareLock);
		ScanKeyInit(&key, Anum_pg_constraint_conrelid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(relid));
		scan = systable_beginscan(conrel, ConstraintRelidTypidNameIndexId, true,
								  NULL, 1, &key);
		while ((tup = systable_getnext(scan)) != NULL)
		{
			Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tup);

			if (gone ||
				list_has_string(going, quote_identifier(NameStr(con->conname))))
				add_dependents(ConstraintRelationId, con->oid, -1, &procs,
							   matviews);
		}
		systable_endscan(scan);
		table_close(conrel, AccessShareLock);
	}
	return procs;
}

/* Routine f's OID (from's: there now), or InvalidOid. */
static Oid
routine_oid(const MoveItem *f)
{
	/* (its name is as DROP FUNCTION takes it, argument names and all) */
	List	   *raw = raw_parser(drop_routine_sql(f), RAW_PARSE_DEFAULT);
	DropStmt   *drop = castNode(DropStmt, linitial_node(RawStmt, raw)->stmt);

	return LookupFuncWithArgs(OBJECT_ROUTINE, linitial(drop->objects), true);
}

/* Where a routine's SQL-standard body begins in its definition, or NULL. */
static const char *
sql_body(const char *def)
{
	const char *body = strstr(def, "\nBEGIN ATOMIC");

	return body ? body : strstr(def, "\nRETURN ");
}

/*
 * Routine f (with a SQL-standard body) with a body that uses nothing: its
 * definition up to the body, with an empty SQL body, which
 * (check_function_bodies off) isn't parsed, so records no dependencies.
 * Replaced so, it keeps its OID, owner, grants and comment, its
 * volatility, strictness, parallel safety and cost (a generated column or
 * an index made meanwhile may call it), and what uses it keeps using it,
 * while what it read can go; it goes, or is made again as to defines it,
 * after.  (Only a superuser may make a routine LEAKPROOF: for another
 * role it is replaced without, and to's definition, made again, refused.)
 */
static char *
unbodied_routine_sql(const MoveItem *f)
{
	const char *body = sql_body(f->def);
	char	   *head = pnstrdup(f->def, body ? body - f->def : strlen(f->def));

	if (!superuser())
	{
		char	   *lp;

		while ((lp = strstr(head, " LEAKPROOF")) != NULL)
			memmove(lp, lp + 10, strlen(lp + 10) + 1);
	}
	return psprintf("%s\nAS $prolly_move$$prolly_move$", head);
}

/*
 * A routine dropped and made again (its row type's table made again): its
 * owner, privileges and comment, which a commit doesn't hold, given back
 * once it is made (restore_routines()).  Its OID is new, so what depends
 * on it (another routine calling it, a default, a view) keeps it from
 * being dropped, and the move fails as dropping it would.
 */
typedef struct DroppedRoutine
{
	char	   *name;			/* as DROP FUNCTION takes it */
	char	   *def;			/* from's definition */
	Oid			owner;
	Acl		   *acl;			/* NULL: the default */
	char	   *comment;		/* NULL: none */
} DroppedRoutine;

static DroppedRoutine *
note_dropped_routine(const MoveItem *f, Oid proc)
{
	HeapTuple	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(proc));
	DroppedRoutine *dr = palloc(sizeof(DroppedRoutine));
	Datum		acl;
	bool		isnull;

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for function %u", proc);
	dr->name = pstrdup(f->name);
	dr->def = pstrdup(f->def);
	dr->owner = ((Form_pg_proc) GETSTRUCT(tup))->proowner;
	acl = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_proacl, &isnull);
	dr->acl = isnull ? NULL : DatumGetAclPCopy(acl);
	ReleaseSysCache(tup);
	dr->comment = GetComment(proc, ProcedureRelationId, 0);
	return dr;
}

/* The routines note_dropped_routine() noted, made again: theirs given back. */
static void
restore_routines(List *dropped)
{
	ListCell   *lc;

	if (dropped == NIL)
		return;
	CommandCounterIncrement();
	foreach(lc, dropped)
	{
		DroppedRoutine *dr = lfirst(lc);
		MoveItem	it = {'f', dr->name, dr->def};
		Oid			proc = routine_oid(&it);
		Relation	rel;
		HeapTuple	tup;
		Datum		values[Natts_pg_proc] = {0};
		bool		nulls[Natts_pg_proc] = {0};
		bool		replaces[Natts_pg_proc] = {0};
		HeapTuple	newtup;
		Oid		   *members = NULL;
		int			nmembers;
		Oid		   *old_members = NULL;
		int			old_nmembers;
		Datum		old_acl;
		bool		isnull;
		Oid			old_owner;

		if (!OidIsValid(proc))
			continue;			/* (not made again: to doesn't have it) */
		rel = table_open(ProcedureRelationId, RowExclusiveLock);
		tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(proc));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for function %u", proc);
		old_owner = ((Form_pg_proc) GETSTRUCT(tup))->proowner;
		old_acl = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_proacl, &isnull);
		old_nmembers = isnull ? 0 : aclmembers(DatumGetAclP(old_acl), &old_members);
		values[Anum_pg_proc_proowner - 1] = ObjectIdGetDatum(dr->owner);
		replaces[Anum_pg_proc_proowner - 1] = true;
		if (dr->acl != NULL)
			values[Anum_pg_proc_proacl - 1] = PointerGetDatum(dr->acl);
		else
			nulls[Anum_pg_proc_proacl - 1] = true;
		replaces[Anum_pg_proc_proacl - 1] = true;
		newtup = heap_modify_tuple(tup, RelationGetDescr(rel), values, nulls,
								   replaces);
		CatalogTupleUpdate(rel, &newtup->t_self, newtup);
		ReleaseSysCache(tup);
		heap_freetuple(newtup);
		table_close(rel, RowExclusiveLock);
		if (old_owner != dr->owner)
			changeDependencyOnOwner(ProcedureRelationId, proc, dr->owner);
		nmembers = dr->acl ? aclmembers(dr->acl, &members) : 0;
		updateAclDependencies(ProcedureRelationId, proc, 0, dr->owner,
							  old_nmembers, old_members, nmembers, members);
		if (dr->comment != NULL)
			CreateComments(proc, ProcedureRelationId, 0, dr->comment);
	}
	CommandCounterIncrement();
}

/*
 * Statements tried in turn, again while any is made: those still failing
 * for another statement (try_statement()) are left, in order.
 */
static List *
try_until_stuck(List *stmts)
{
	int			n;

	do
	{
		n = list_length(stmts);
		stmts = try_statements(stmts);
	} while (stmts != NIL && list_length(stmts) < n);
	return stmts;
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
	List	   *sql_bodies = NIL;	/* routines with SQL-standard bodies, then
									 * the casts made with functions, which
									 * may be those */
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
	List	   *unkeyed = NIL;	/* routines kept, emptied of their bodies */
	List	   *unbodied = NIL;	/* those emptied, first */
	List	   *going_users;	/* routines using what goes */
	List	   *retyped;		/* routines on a row type that goes */
	List	   *redone = NIL;	/* routines dropped and made again */
	List	   *dropped_routines = NIL; /* DroppedRoutine *: theirs */
	bool		going_matviews = false;	/* a matview uses what goes */
	List	   *foreign_makes = NIL;	/* foreign tables made, after those going */
	List	   *foreign_alters = NIL;	/* foreign tables changed in place */
	List	   *foreign_routines = NIL; /* routines using those made again,
										 * made again after them */
	bool		foreign_remade = false; /* one went to be made again */
	List	   *routines = NIL;
	List	   *aggregates_made = NIL;
	List	   *policy_drops = NIL;
	List	   *policy_makes = NIL;
	List	   *base;			/* the writer base's items, if any */
	ProllyNodeRef base_root;
	bool		as_base;		/* the move makes only the base's definitions:
								 * its statements run as authority */
	Oid			authority = InvalidOid;
	Oid			session_user = InvalidOid;
	int			session_sec = 0;
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
	base = writer_base_items(&cn, &base_root);
	to = move_items(&cn, to_root);
	/* (each relation the branch has, changed or made again, as its owner) */
	move_owners_before(from, to);
	as_base = base != NIL && definitions_of_base(from, to, base);
	if (as_base)
	{
		authority = move_authority(from, to);
		/* (a role that may make them all already: as itself) */
		if (has_privs_of_role(GetUserId(), authority))
			as_base = false;
	}
	tm = type_moves(from, to);

	/*
	 * Foreign tables using a type the move changes go before it does (they
	 * hold no rows), and are made again after, as to defines them.
	 */
	foreach(lc, tm.foreign_remakes)
	{
		MoveItem   *t = find_item(to, 'r', lfirst(lc));

		List	   *drops = NIL;

		foreign_table_routines(lfirst(lc), from, to, &drops, &foreign_routines);
		tm.before = list_concat(drops,
								lcons(psprintf("DROP FOREIGN TABLE IF EXISTS %s", (char *) lfirst(lc)),
									  tm.before));
		made_as_item(list_nth(tm.before, list_length(drops)), lfirst(lc), 'r');
		if (t)
		{
			foreign_makes = lappend(foreign_makes, create_foreign_table_sql(t->name, t->def));
			made_as_item(llast(foreign_makes), t->name, 'r');
		}
	}

	going_users = routines_on_going(from, to, &retyped, &going_matviews);

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
			made_as_from(view_drops, list_length(view_drops) - 1, f->name, 'v');
			if (t)
				save_remade(f->name, 'v');
			continue;
		}

		/*
		 * Materialized views go before them, as they may use them, and
		 * come again with the rows the commit holds.
		 */
		if (f->kind == 'm')
		{
			matview_drops = lappend(matview_drops,
									psprintf("DROP MATERIALIZED VIEW IF EXISTS %s",
											 f->name));
			made_as_from(matview_drops, list_length(matview_drops) - 1, f->name, 'm');
		}
		/*
		 * (one using what goes, emptied of its body first: see
		 * unbodied_routine_sql(); to's made again with the rest)
		 */
		if (f->kind == 'f' && !is_aggregate(f) &&
			(going_users != NIL || retyped != NIL))
		{
			Oid			proc = routine_oid(f);

			/*
			 * (one whose arguments or result are a row type that goes:
			 * dropped, as its signature can't stay, and made again as to
			 * defines it, its owner, privileges and comment given back)
			 */
			if (list_member_oid(retyped, proc))
			{
				unbodied = lappend(unbodied, drop_routine_sql(f));
				made_as_item(llast(unbodied), f->name, 'f');
				if (t)
				{
					dropped_routines = lappend(dropped_routines,
											   note_dropped_routine(f, proc));
					unkeyed = lappend(unkeyed, f->name);
					redone = lappend(redone, f->name);
				}
				continue;
			}
			if (list_member_oid(going_users, proc) && sql_body(f->def) != NULL)
			{
				unbodied = lappend(unbodied, unbodied_routine_sql(f));
				made_as_item(llast(unbodied), f->name, 'f');
				if (t)
					unkeyed = lappend(unkeyed, f->name);
			}
		}
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
				{
					sm.before = lcons(drop_routine_sql(f), sm.before);
					made_as_item(linitial(sm.before), f->name, 'f');
				}
				else if (t == NULL)
				{
					routines = lappend(routines, drop_routine_sql(f));
					made_as_item(llast(routines), f->name, 'f');
				}
				break;
			case 'l':
				if (t == NULL)
				{
					collation_drops = lappend(collation_drops,
											  psprintf("DROP COLLATION IF EXISTS %s", f->name));
					made_as_item(llast(collation_drops), f->name, 'l');
				}
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
					made_as_item(llast(tm.before), f->name, 'l');
				}
				break;
			case 'x':
				/* (before the routines and types it uses go) */
				sm.before = lcons(psprintf("DROP CAST IF EXISTS (%s)", f->name),
								  sm.before);
				made_as_item(linitial(sm.before), f->name, 'x');
				break;
			case 'r':
				{
					/*
					 * A foreign table changed is altered in place where it
					 * can be; going, or changed in what can't be altered,
					 * it goes after the views (and materialized views) using
					 * it, and is made again as to defines it (it holds no
					 * rows: they are its server's).  One a type move remakes
					 * goes with that.
					 */
					bool		remake = (t == NULL);

					if (list_has_string(tm.foreign_remakes, f->name))
						break;
					if (t)
						foreign_alters = list_concat(foreign_alters,
													 made_as_all(alter_foreign_table_sql(f->name, f->def,
																						 t->def, &remake),
																 f->name, 'r'));
					if (remake)
					{
						foreign_table_routines(f->name, from, to, &late, &foreign_routines);
						late = lappend(late, psprintf("DROP FOREIGN TABLE IF EXISTS %s", f->name));
						made_as_item(llast(late), f->name, 'r');
						foreign_remade = true;
					}
				}
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
		{
			/* (what changes it, makes it again or drops it: as its owner) */
			ListMark	m_before = list_mark(sm.before);
			ListMark	m_changes = list_mark(sm.changes);
			ListMark	m_after = list_mark(sm.after);
			ListMark	m_validations = list_mark(sm.validations);
			ListMark	m_foreign_keys = list_mark(sm.foreign_keys);
			ListMark	m_fk_validations = list_mark(sm.fk_validations);
			ListMark	m_fk_drops = list_mark(sm.fk_drops);
			bool		again = move_table(&sm, f->name, &fd, t ? &td : NULL);

			made_as_since(sm.before, m_before, f->name, 't');
			made_as_since(sm.changes, m_changes, f->name, 't');
			made_as_since(sm.after, m_after, f->name, 't');
			made_as_since(sm.validations, m_validations, f->name, 't');
			made_as_since(sm.foreign_keys, m_foreign_keys, f->name, 't');
			made_as_since(sm.fk_validations, m_fk_validations, f->name, 't');
			made_as_since(sm.fk_drops, m_fk_drops, f->name, 't');
			if (again)
			{
				remade = lappend(remade, f->name);
				if (t)
					save_remade(f->name, 't');
			}
		}
	}
	move_kept_relids = move_relids_before(from, to, remade);
	move_makers_before(from, to, move_kept_relids);

	/*
	 * Tables whose columns change type in turn (type_moves()) are emptied
	 * too, as move_table() empties those it changes: converted, their rows
	 * may break what the new type's column must satisfy (a NOT NULL, a
	 * unique index).
	 */
	if (rewritten)
		foreach(lc, tm.tables)
		{
			if (!list_has_string(sm.emptied, lfirst(lc)))
				sm.emptied = lappend(sm.emptied, lfirst(lc));
			/* (each converts through an expression: a rewrite) */
			if (!list_has_string(sm.at_once, lfirst(lc)))
				sm.at_once = lappend(sm.at_once, lfirst(lc));
		}

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
				made_as_from(sm.fk_drops, list_length(sm.fk_drops) - 1, f->name, 't');
				if (t)
				{
					sm.foreign_keys = lappend(sm.foreign_keys,
											  psprintf("ALTER TABLE %s ADD CONSTRAINT %s %s",
													   f->name, first_field(line),
													   constraint_def(line)));
					made_as_from(sm.foreign_keys, list_length(sm.foreign_keys) - 1,
								 f->name, 't');
				}
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
		{
			unlinks = lappend(unlinks, psprintf("ALTER TABLE %s NO INHERIT %s",
												f->name, (char *) lfirst(c)));
			made_as_from(unlinks, list_length(unlinks) - 1, f->name, 't');
		}
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
		{
			links = lappend(links, psprintf("ALTER TABLE %s INHERIT %s",
											t->name, (char *) lfirst(c)));
			made_as_from(links, list_length(links) - 1, t->name, 't');
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
			{
				type_drops = lappend(type_drops, psprintf("DROP TYPE %s", f->name));
				made_as_item(llast(type_drops), f->name, 'y');
			}
			else if (f->kind == 's')
			{
				late = lappend(late, psprintf("DROP SEQUENCE IF EXISTS %s", f->name));
				made_as_from(late, list_length(late) - 1, f->name, 's');
			}
		}
	}
	/* Schemas going: last, what was in them gone already. */
	foreach(lc, from)
	{
		MoveItem   *f = lfirst(lc);

		if (f->kind == 'n' && find_item(to, 'n', f->name) == NULL &&
			!holds_extension(f->name))
		{
			schema_drops = lappend(schema_drops,
								   psprintf("DROP SCHEMA IF EXISTS %s CASCADE", f->name));
			made_as_item(llast(schema_drops), f->name, 'n');
		}
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
		{
			int			n = list_length(schemas);
			MoveMaker  *mm = maker_named(t->name, 'n');

			schemas = list_concat(schemas,
								  make_schema(&cn, t->name, &replaced));

			/*
			 * (as its recorded maker, if it has one; else as the role moving,
			 * said so: an ordinary schema of the name it replaces, whoever's,
			 * is dropped empty and made versioned, its owner, privileges and
			 * comment carried over)
			 */
			for (int i = n; i < list_length(schemas); i++)
				made_as(list_nth(schemas, i), mm ? mm->owner : InvalidOid);
		}
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
		{
			collation_makes = lappend(collation_makes,
									  create_collation_sql(t->name, t->def));
			made_as_item(llast(collation_makes), t->name, 'l');
		}
		else if (t->kind == 'y' && f == NULL)
		{
			ListMark	m = list_mark(tm.unvalidated);

			tm.creates = lappend(tm.creates, create_type_sql(t->name, t->def,
															 &tm.unvalidated));
			made_as_item(llast(tm.creates), t->name, 'y');
			/* (its NOT VALID checks too) */
			made_as_since(tm.unvalidated, m, t->name, 'y');
		}
		else if (t->kind == 's' && !f && list_has_string(identities, t->name))
		{
			sm.after = lappend(sm.after,
							   psprintf("ALTER SEQUENCE %s %s", t->name,
										sequence_options_sql(t->def)));
			/* (its maker: one the branch has, else the table's that owns it) */
			made_as_item(llast(sm.after), t->name, 's');
		}
		else if (t->kind == 'y' || t->kind == 'm')
			;					/* changed types first, materialized views last */
		else if (t->kind == 's')
		{
			sm.before = lappend(sm.before,
								psprintf("%s SEQUENCE %s %s", f ? "ALTER" : "CREATE",
										 t->name, sequence_options_sql(t->def)));
			made_as_item(llast(sm.before), t->name, 's');
		}
		else if (t->kind == 'r')
		{
			bool		remake = (f == NULL);

			if (list_has_string(tm.foreign_remakes, t->name))
				continue;		/* (above) */
			if (f)
				(void) alter_foreign_table_sql(t->name, f->def, t->def, &remake);
			if (remake)
			{
				foreign_makes = lappend(foreign_makes, create_foreign_table_sql(t->name, t->def));
				made_as_item(llast(foreign_makes), t->name, 'r');
			}
		}
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
			{
				sm.before = lcons(drop_routine_sql(f), sm.before);
				made_as_item(linitial(sm.before), f->name, 'f');
				save_remade(f->name, 'f');
			}
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
				{
					aggregates_made = lappend(aggregates_made, t->def);
					made_as_item(t->def, t->name, 'f');
				}
				continue;
			}
			if (f && strcmp(t->def, f->def) == 0 &&
				!list_has_string(unkeyed, t->name))
				continue;
			/* (CREATE OR REPLACE can't change what a routine returns) */
			if (f && strcmp(routine_signature(f->def), routine_signature(t->def)) != 0 &&
				!list_has_string(redone, t->name))
			{
				sm.before = lappend(sm.before, drop_routine_sql(f));
				made_as_item(llast(sm.before), f->name, 'f');
				save_remade(f->name, 'f');
			}
			made_as_item(t->def, t->name, 'f');
			/*
			 * (one with an SQL-standard body is parsed as it's made, so needs
			 * what it reads, which may be a table or materialized view the
			 * move makes: tried with the rest, again after the tables, and
			 * last with the views, each after those it uses)
			 */
			if (sql_body(t->def) != NULL || list_has_string(redone, t->name))
				sql_bodies = lappend(sql_bodies, t->def);
			else
				sm.before = lappend(sm.before, t->def);
		}
	}
	/*
	 * Casts coming, once the types and routines they name are made: one
	 * with a function, after the routines with SQL-standard bodies, which
	 * may be made later, and tried with them (still before the tables).
	 */
	foreach(lc, to)
	{
		MoveItem   *t = lfirst(lc);
		MoveItem   *f = find_item(from, t->kind, t->name);

		if (t->kind != 'x' || (f && strcmp(t->def, f->def) == 0))
			continue;
		if (strncmp(t->def, "function\t", 9) == 0)
		{
			sql_bodies = lappend(sql_bodies, create_cast_sql(t->name, t->def));
			made_as_item(llast(sql_bodies), t->name, 'x');
		}
		else
		{
			sm.before = lappend(sm.before, create_cast_sql(t->name, t->def));
			made_as_item(llast(sm.before), t->name, 'x');
		}
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
				{
					/* (what makes it, as its maker, if it has one) */
					int			n_before = list_length(sm.before);
					int			n_changes = list_length(sm.changes);
					int			n_after = list_length(sm.after);
					int			n_validations = list_length(sm.validations);
					int			n_foreign_keys = list_length(sm.foreign_keys);
					int			n_fk_validations = list_length(sm.fk_validations);

					move_table(&sm, t->name, NULL, td);
					made_as_from(sm.before, n_before, t->name, 't');
					made_as_from(sm.changes, n_changes, t->name, 't');
					made_as_from(sm.after, n_after, t->name, 't');
					made_as_from(sm.validations, n_validations, t->name, 't');
					made_as_from(sm.foreign_keys, n_foreign_keys, t->name, 't');
					made_as_from(sm.fk_validations, n_fk_validations, t->name, 't');
				}
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
	matviews_change = tm.any || view_drops != NIL || foreign_remade ||
		going_matviews;
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
		{
			views = lappend(views,
							psprintf(strncmp(t->def, "WITH (", 6) == 0 ?
									 "CREATE VIEW %s %s" : "CREATE VIEW %s AS %s",
									 t->name, t->def));
			made_as_from(views, list_length(views) - 1, t->name, 'v');
		}
		/*
		 * materialized views made again once the rows are in; their rows,
		 * as to holds them, written after (those of the others are written
		 * with the tables')
		 */
		else if (t->kind == 'm' && matviews_change)
		{
			int			n = list_length(views);

			views = list_concat(views, make_matview_sql(t->name, t->def));
			made_as_from(views, n, t->name, 'm');
			save_remade(t->name, 'm');
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
		{
			PolicyStatement *ps = policy_statement(psprintf("DROP POLICY IF EXISTS %s ON %s",
															object_name(lfirst(pc), "CREATE POLICY "),
															f->name),
												   f->name, from, to, base);
			made_as_item(ps->sql, f->name, 't');
			policy_drops = lappend(policy_drops, ps);
		}
	}
	foreach(lc, to)
	{
		MoveItem   *t = lfirst(lc);
		TableDef	td;
		ListCell   *pc;

		if (t->kind != 't')
			continue;
		parse_table_def(t->def, &td);
		foreach(pc, td.policies)
		{
			PolicyStatement *ps = policy_statement(lfirst(pc), t->name, from, to, base);
			made_as_item(ps->sql, t->name, 't');
			policy_makes = lappend(policy_makes, ps);
		}
	}

	describing_settings();
	(void) set_config_option("check_function_bodies", "off", PGC_USERSET,
							 PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	/* (what's already gone, an IF EXISTS skips: no notice of it) */
	(void) set_config_option("client_min_messages", "warning", PGC_USERSET,
							 PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	SPI_connect();
	/*
	 * (the authority, restricted, for the statements; the session's role for
	 * the rows.  An error between gives the session its role back as the
	 * (sub)transaction aborts, which restores the user and security context
	 * it began with.)
	 */
	if (as_base)
	{
		GetUserIdAndSecContext(&session_user, &session_sec);
		SetUserIdAndSecContext(authority, session_sec | SECURITY_LOCAL_USERID_CHANGE |
							   SECURITY_RESTRICTED_OPERATION);
	}
	places = index_places(from);
	run_policy_statements(policy_drops);
	run_statements(unbodied);

	/*
	 * Rows to go before their columns change type (with every row written
	 * afresh), as rows the move writes go (empty_table()).
	 */
	run_statements(sm.fk_drops);
	foreach(lc, sm.emptied)
		empty_table(lfirst(lc), list_has_string(sm.at_once, lfirst(lc)));
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
	run_statements(sm.before);
	sql_bodies = try_statements(sql_bodies);
	run_statements(list_concat(sm.changes, late));
	run_statements(foreign_makes);	/* (after those going; before the views) */
	run_statements(foreign_alters);
	run_statements(foreign_routines);
	sql_bodies = try_statements(sql_bodies);
	make_views(type_drops);
	run_statements(collation_drops);
	run_statements(schema_drops);

	/*
	 * The routines and views not made yet, tried again now, before what
	 * may call them (the rows written, with their generated columns and
	 * checks, then indexes, triggers, the checks validated): a routine
	 * reading a view the move makes again, emptied of its body before,
	 * is made again here.  What still waits on another is made last.
	 */
	sql_bodies = try_until_stuck(list_concat(sql_bodies, views));
	views = NIL;
	restore_routines(dropped_routines);
	CommandCounterIncrement();
	/* the rows in the caller's memory: what they keep outlives SPI's */
	{
		MemoryContext spi = MemoryContextSwitchTo(caller);

		if (as_base)
			SetUserIdAndSecContext(session_user, session_sec);
		rows(arg);
		if (as_base)
			SetUserIdAndSecContext(authority, session_sec | SECURITY_LOCAL_USERID_CHANGE |
								   SECURITY_RESTRICTED_OPERATION);
		MemoryContextSwitchTo(spi);
	}
	run_statements(sm.after);
	run_statements(sm.foreign_keys);
	if (breaking && sm.added != NIL)
	{
		MemoryContext spi;

		CommandCounterIncrement();
		spi = MemoryContextSwitchTo(caller);
		if (as_base)
			SetUserIdAndSecContext(session_user, session_sec);
		breaking(sm.added, arg);
		if (as_base)
			SetUserIdAndSecContext(authority, session_sec | SECURITY_LOCAL_USERID_CHANGE |
								   SECURITY_RESTRICTED_OPERATION);
		MemoryContextSwitchTo(spi);
	}
	run_statements(sm.validations);
	if (breaking == NULL)
		run_statements(sm.fk_validations);
	run_statements(links);
	restore_locality(relinked, to);
	make_views(sql_bodies);
	restore_routines(dropped_routines);
	run_policy_statements(policy_makes);
	restore_index_places(places);
	if (as_base)
		SetUserIdAndSecContext(session_user, session_sec);
	if (matviews_made)
	{
		MemoryContext spi;

		CommandCounterIncrement();
		spi = MemoryContextSwitchTo(caller);
		rows(arg);
		MemoryContextSwitchTo(spi);
	}
	move_kept_relids = NIL;
	/*
	 * (what its making gave those made from a commit, from its maker's
	 * default privileges: none; those the branch had, made again, their
	 * grants back)
	 */
	foreach_ptr(MoveMaker, mm, move_makers)
	{
		Oid			classid;
		Oid			oid;

		if (mm->kind == 'S' || (!mm->made && mm->saved == NIL))
			continue;			/* (a table's, or unchanged) */
		if (!mm->made)
		{
			restore_saved_acls(mm->kind, mm->name, mm->saved);
			continue;
		}
		oid = object_named(mm->kind, mm->name, &classid);
		if (!OidIsValid(oid))
			continue;
		if (classid == RelationRelationId)
			clear_made_privileges(oid);
		else
			set_object_acl(classid, oid, NULL, NULL);
	}
	reset_makers();
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
	int			calls = prolly_move_calls_begin();
	bool		done = false;

	PG_TRY();
	{
		move_schema(from_root, to_root, rows, breaking, arg, rewritten);
		done = true;
	}
	PG_FINALLY();
	{
		/* (a move that failed leaves no makers behind: their memory goes) */
		reset_makers();
		move_kept_relids = NIL;
		prolly_writing_held_rows(held);
		prolly_move_calls_end(calls, done);
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
 * conflict already (merge_table_defs()).  With keep, true for such a
 * conflict instead of the error.
 */
static bool
check_type_changes(const char *name, const char *base, const char *ours,
				   const char *theirs, bool keep)
{
	List	   *sec[3][NUM_TABLE_SECTIONS];
	TableLine  *tl[3];
	int			len[3];

	if (!split_table_def(base, sec[0]) || !split_table_def(ours, sec[1]) ||
		!split_table_def(theirs, sec[2]))
		return false;
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
				!string_change_merges(type[0], type[side]) && keep)
				return true;
			if (strcmp(type[side], type[0]) != 0 && strcmp(other, type[0]) == 0 &&
				!string_change_merges(type[0], type[side]))
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errschemaconflict(),
						 errmsg("schema conflict: column %s of %s changed type from %s to %s on one side, and the other side changed the table",
								col, name, type[0], type[side]),
						 errdetail("A change of type merges with the other side's changes only as a string type made no shorter.")));
		}
	}
	return false;
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
							 errschemaconflict(),
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
						 errschemaconflict(),
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
							 errschemaconflict(),
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
						 errschemaconflict(),
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
					const ProllyNodeRef *theirs_root, ProllyNodeRef *merged,
					List **kept_conflicts)
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
		more[i] = next_schema_item(&cn, cur[i], &k[i], &v[i]);
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
		bool		retyped = false;

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
			retyped = check_type_changes(from_utf8(least->data + 2, least->len - 2),
										 pnstrdup((const char *) def[0].data, def[0].len),
										 pnstrdup((const char *) def[1].data, def[1].len),
										 pnstrdup((const char *) def[2].data, def[2].len),
										 kept_conflicts != NULL);
		/*
		 * (a table one side dropped and the other changed, its rows or its
		 * definition: kept, a schema conflict, as Dolt keeps it)
		 */
		if (!retyped && kept_conflicts != NULL && least->data[0] == 't' &&
			has[0] && has[1] != has[2] && CHANGED(has[1] ? 1 : 2))
			retyped = true;
		if (retyped)
		{
			/* (kept as ours has it, or gone if ours dropped it) */
			take = has[1] ? &v[1] : NULL;
			*kept_conflicts = lappend(*kept_conflicts,
									  from_utf8(least->data + 2, least->len - 2));
		}
		else if (SAME(0, 2))
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
		else if (kept_conflicts != NULL && least->data[0] == 't')
		{
			/* (kept as ours has it, or gone if ours dropped it) */
			take = has[1] ? &v[1] : NULL;
			*kept_conflicts = lappend(*kept_conflicts,
									  from_utf8(least->data + 2, least->len - 2));
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errschemaconflict(),
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
				more[i] = next_schema_item(&cn, cur[i], &k[i], &v[i]);
		for (int i = 0; i < 3; i++)
			if (has[i] && &k[i] == least)
				more[i] = next_schema_item(&cn, cur[i], &k[i], &v[i]);
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
						 errschemaconflict(),
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
	describing_settings();
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
					finder_fold_rows(cn.finder, rel, &cn.env, &data,
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

static bool
same_hash(const ProllyHash *a, const ProllyHash *b)
{
	return a->len == b->len && memcmp(a->bytes, b->bytes, a->len) == 0;
}

/*
 * The baseline after a commit of the transaction's own row changes (head
 * to root, by fold_rows()): a table changed whose baseline held exactly
 * head's rows (and the same row shape) holds root's, its finder's state
 * advanced past them; every other table's entry as it was.  Its rows are
 * then the baseline's rows of before with the transaction's changes, which
 * the finder marks folded, the changes of others since staying for the
 * next fold.
 */
static bool
folded_advance_baseline(const ProllyNodeRef *baseline, const ProllyNodeRef *head,
						const ProllyNodeRef *root, const ProllyRowChanges *changes,
						int nchanges, ProllyNodeRef *next)
{
	Canon		cn;
	ProllyEdit *edits = palloc_array(ProllyEdit, Max(nchanges, 1));
	int			nedits = 0;
	int			save_nestlevel;
	StringInfoData v;

	memset(&cn, 0, sizeof(cn));
	canon_env(&cn);
	cn.finder = prolly_change_finder();
	if (!PROLLY_HAS(cn.finder, advance) || !root_in_format(&cn, head))
		return false;
	load_baseline(&cn, baseline);
	initStringInfo(&v);

	/* (described as a commit describes them: every name qualified) */
	save_nestlevel = NewGUCNestLevel();
	describing_settings();
	CommandCounterIncrement();
	PushActiveSnapshot(GetTransactionSnapshot());
	PG_TRY();
	{
		for (int i = 0; i < nchanges; i++)
		{
			Relation	rel = try_table_open(changes[i].relid, AccessShareLock);
			CanonItem  *it;
			BaseTable  *bt;
			ProllyKey	k;
			ProllyValue iv;
			ProllyValue def;
			ProllyNodeRef was;
			ProllyNodeRef now;
			StringInfoData desc;
			StringInfoData shape;
			ProllyHash	dh;
			ProllyValue state;
			StringInfoData ns;

			if (rel == NULL)
				continue;
			it = add_item(&cn, "t:", qualified(RelationGetNamespace(rel),
											   RelationGetRelationName(rel)));
			bt = find_base_table(&cn, &it->key);
			k.data = (const uint8 *) it->key.data;
			k.len = it->key.len;
			resetStringInfo(&v);
			if (bt == NULL || !head_item(&cn, head, &it->key, &v))
			{
				table_close(rel, AccessShareLock);
				continue;
			}
			iv.data = (const uint8 *) v.data;
			iv.len = v.len;
			(void) tree_item(&k, &iv, &was, &def);
			resetStringInfo(&v);
			if (!same_hash(&was.hash, &bt->data.hash) ||
				!head_item(&cn, root, &it->key, &v))
			{
				table_close(rel, AccessShareLock);
				continue;
			}
			iv.data = (const uint8 *) v.data;
			iv.len = v.len;
			(void) tree_item(&k, &iv, &now, &def);
			initStringInfo(&desc);
			describe_table(&cn, rel, &desc);
			initStringInfo(&shape);
			baseline_shape(&cn, rel, &desc, &shape);
			content_hash(&cn, shape.data, shape.len, &dh);
			state.data = (const uint8 *) bt->state.data;
			state.len = bt->state.len;
			initStringInfo(&ns);
			if (same_hash(&dh, &bt->shape_hash) &&
				PROLLY_CALL(cn.finder, advance, rel, &state, &ns))
			{
				resetStringInfo(&it->value);
				put_hash(&it->value, &now.hash);
				put_base_shape(&cn, &it->value, &dh, &shape, bt);
				appendBinaryStringInfo(&it->value, ns.data, ns.len);
				put_edit(edits, &nedits, it);
			}
			pfree(shape.data);
			pfree(desc.data);
			table_close(rel, AccessShareLock);
		}
	}
	PG_FINALLY();
	{
		PopActiveSnapshot();
		AtEOXact_GUC(false, save_nestlevel);
	}
	PG_END_TRY();
	if (nedits == 0)
		return false;
	{
		ProllyTreeEnv benv = cn.env;

		benv.chunk_refs = base_chunk_refs;
		if (nedits > 1)
			qsort(edits, nedits, sizeof(ProllyEdit), edit_key_cmp);
		PROLLY_CALL(cn.tree, apply, &benv, baseline, edits, nedits, next);
	}
	return true;
}

/*
 * baseline with the current transaction's own row changes to these tables
 * applied to its rows (the change finder's fold_rows() from each table's
 * baseline tree, not a commit's) into *next, each table's finder state
 * advanced past them (advance(): its record of them consumed), so the
 * baseline holds what the transaction committed: a fold at its commit.
 * A table whose baseline isn't of its shape now, or whose changes or state
 * can't be applied or advanced so, keeps its entry, and its record of the
 * changes, for the next fold.  Every table's tree is made first (of those
 * whose state the finder can advance, can_advance() says, so none is made
 * for nothing); only then are states advanced, each table's entry put only
 * if its state was.
 * False, next undefined, if no table's could be.
 */
static bool
folded_advance_own(const ProllyNodeRef *baseline,
				   const ProllyRowChanges *changes, int nchanges,
				   ProllyNodeRef *next)
{
	Canon		cn;
	ProllyEdit *edits = palloc_array(ProllyEdit, Max(nchanges, 1));
	int			nedits = 0;
	int			save_nestlevel;
	typedef struct Applied
	{
		Relation	rel;
		CanonItem  *it;
		BaseTable  *bt;
		ProllyNodeRef now;
		ProllyHash	dh;
		StringInfoData shape;
	} Applied;
	Applied    *ap = palloc0_array(Applied, Max(nchanges, 1));
	int			nap = 0;

	memset(&cn, 0, sizeof(cn));
	canon_env(&cn);
	cn.finder = prolly_change_finder();
	if (!PROLLY_HAS(cn.finder, advance) || !PROLLY_HAS(cn.finder, fold_rows))
		return false;
	load_baseline(&cn, baseline);

	/* (described as a commit describes them: every name qualified) */
	save_nestlevel = NewGUCNestLevel();
	describing_settings();
	CommandCounterIncrement();
	PushActiveSnapshot(GetTransactionSnapshot());
	PG_TRY();
	{
		/* each table's rows applied to its baseline's tree */
		for (int i = 0; i < nchanges; i++)
		{
			Relation	rel = try_table_open(changes[i].relid, AccessShareLock);
			Applied    *a = &ap[nap];
			StringInfoData desc;

			if (rel == NULL)
				continue;
			a->rel = rel;
			a->it = add_item(&cn, "t:", qualified(RelationGetNamespace(rel),
												  RelationGetRelationName(rel)));
			a->bt = find_base_table(&cn, &a->it->key);
			initStringInfo(&desc);
			describe_table(&cn, rel, &desc);
			initStringInfo(&a->shape);
			baseline_shape(&cn, rel, &desc, &a->shape);
			content_hash(&cn, a->shape.data, a->shape.len, &a->dh);
			pfree(desc.data);
			if (a->bt == NULL || !same_hash(&a->dh, &a->bt->shape_hash) ||
				(PROLLY_HAS(cn.finder, can_advance) &&
				 !cn.finder->can_advance(rel, &(ProllyValue)
										 {(const uint8 *) a->bt->state.data,
										  a->bt->state.len})) ||
				!finder_fold_rows(cn.finder, rel, &cn.env, &a->bt->data,
								  changes[i].tids, changes[i].ntids, &a->now))
			{
				table_close(rel, AccessShareLock);
				continue;
			}
			nap++;
		}

		/* then each one's state advanced: its entry put only if it was */
		for (int i = 0; i < nap; i++)
		{
			Applied    *a = &ap[i];
			ProllyValue state = {(const uint8 *) a->bt->state.data,
			a->bt->state.len};
			StringInfoData ns;

			initStringInfo(&ns);
			if (PROLLY_CALL(cn.finder, advance, a->rel, &state, &ns))
			{
				resetStringInfo(&a->it->value);
				put_hash(&a->it->value, &a->now.hash);
				put_base_shape(&cn, &a->it->value, &a->dh, &a->shape, a->bt);
				appendBinaryStringInfo(&a->it->value, ns.data, ns.len);
				put_edit(edits, &nedits, a->it);
			}
			table_close(a->rel, AccessShareLock);
			a->rel = NULL;
		}
	}
	PG_FINALLY();
	{
		PopActiveSnapshot();
		AtEOXact_GUC(false, save_nestlevel);
	}
	PG_END_TRY();
	if (nedits == 0)
		return false;
	{
		ProllyTreeEnv benv = cn.env;

		benv.chunk_refs = base_chunk_refs;
		if (nedits > 1)
			qsort(edits, nedits, sizeof(ProllyEdit), edit_key_cmp);
		PROLLY_CALL(cn.tree, apply, &benv, baseline, edits, nedits, next);
	}
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
		case 'r':
			append_statement(out, create_foreign_table_sql(name, def));
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

	if (k->len >= strlen(ORDERING_PREFIX) &&
		memcmp(k->data, ORDERING_PREFIX, strlen(ORDERING_PREFIX)) == 0)
		name = from_utf8(k->data + strlen(ORDERING_PREFIX),
						 k->len - strlen(ORDERING_PREFIX));
	else if (k->len >= 2 && k->data[0] == 't' && k->data[1] == ':')
		name = from_utf8(k->data + 2, k->len - 2);
	else
		return false;
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
	b = PROLLY_CALL(cn.tree, build_begin, &cn.root_env, CurrentMemoryContext);
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

/*
 * into, but for the tables named, whose entries are from's (none where from
 * hasn't one), into *out: into with those entries applied.
 */
static void
folded_splice_baseline(const ProllyNodeRef *into, const ProllyNodeRef *from,
					   List *tables, ProllyNodeRef *out)
{
	Canon		a;
	Canon		b;
	ProllyTreeEnv benv;
	ProllyEdit *edits = palloc_array(ProllyEdit, Max(list_length(tables), 1));
	int			nedits = 0;
	List	   *keys = NIL;

	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));
	canon_env(&a);
	canon_env(&b);
	load_baseline(&a, into);
	load_baseline(&b, from);
	foreach_ptr(char, name, tables)
	{
		CanonItem  *it = palloc0(sizeof(CanonItem));

		initStringInfo(&it->key);
		appendStringInfoString(&it->key, "t:");
		append_utf8(&it->key, name);
		keys = lappend(keys, it);
	}
	list_sort(keys, item_cmp);
	foreach_ptr(CanonItem, it, keys)
	{
		BaseTable  *bt = find_base_table(&b, &it->key);

		/* (named twice: once) */
		if (nedits > 0 && edits[nedits - 1].key.len == it->key.len &&
			memcmp(edits[nedits - 1].key.data, it->key.data, it->key.len) == 0)
			continue;
		edits[nedits].key = (ProllyKey) {(const uint8 *) it->key.data, it->key.len};
		edits[nedits].is_delete = bt == NULL;
		if (bt)
		{
			/* (its value as parse_base_table() read it) */
			initStringInfo(&it->value);
			put_hash(&it->value, &bt->data.hash);
			put_base_kept_shape(&it->value, bt);
			appendBinaryStringInfo(&it->value, bt->state.data, bt->state.len);
			edits[nedits].value = (ProllyValue) {(const uint8 *) it->value.data,
			it->value.len};
		}
		nedits++;
	}
	benv = a.env;
	benv.chunk_refs = base_chunk_refs;
	if (nedits == 0)
		*out = *into;
	else
		PROLLY_CALL(a.tree, apply, &benv, into, edits, nedits, out);
	pfree(edits);
}

/*
 * Make versioned schemas names (quoted) as a move making them would, before
 * the move: for a clone, which makes the extensions in versioned schemas
 * first.  Each is judged before any is made, as a move judges the schemas it
 * makes (make_schema() replaces an ordinary one only while the database has
 * no versioned schema).
 */
static void
folded_make_versioned_schemas(List *names)
{
	Canon		cn;
	List	   *replaced = NIL;
	List	   *stmts = NIL;
	ListCell   *lc;
	HTAB	   *statements;

	if (names == NIL)
		return;
	memset(&cn, 0, sizeof(cn));
	canon_env(&cn);
	foreach(lc, names)
		stmts = list_concat(stmts, make_schema(&cn, lfirst(lc), &replaced));
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	/*
	 * (as the role making them, said so: an ordinary schema one replaces,
	 * whoever's, is dropped to be made again with its owner, privileges and
	 * comment carried over)
	 */
	statements = maker_statements;
	maker_statements = NULL;
	foreach_ptr(char, sql, stmts)
		made_as(sql, InvalidOid);
	PG_TRY();
	{
		run_statements(stmts);
	}
	PG_FINALLY();
	{
		maker_statements = statements;
	}
	PG_END_TRY();
	SPI_finish();
	CommandCounterIncrement();
	carry_over(replaced);
	CommandCounterIncrement();
}

/* ------------------------------------------------------------------------
 * Identity
 * ------------------------------------------------------------------------
 */

/*
 * A table's or column's identity, which its renames keep: the name it was
 * made with (a table's qualified), and which of the objects made with that
 * name it is (0, unless another holding that name's identity was there,
 * as a column renamed is from one added with its old name).  A root holds
 * none of it: the same tables, columns and rows are the same root however
 * they were named before.  A commit records its root's beside it, as an
 * identity record: in key order, a line for each table and column whose
 * identity isn't its own name's with 0, fields escaped (append_field()),
 * names in UTF-8 (a table's qualified and quoted, a column's unquoted):
 *
 *	c <table> <column> <origin> <n>
 *	t <table> <origin> <n>
 *
 * So a commit whose objects were never renamed records nothing.  An
 * anchor, the working set's, has a line for each table and column, by the
 * branch's own relation and attribute number (0 for the table), which a
 * rename keeps: relid, attnum, origin, n.
 */
typedef struct IdObject
{
	char	   *name;			/* a table's qualified and quoted, a
								 * column's unquoted; server encoding */
	char	   *origin;			/* likewise */
	int			n;
	bool		known;			/* identity settled */
	Oid			relid;			/* (a working set's) */
	AttrNumber	attnum;			/* (a working set's column; 0: a table) */
	List	   *columns;		/* (a table's) IdObject *, in order */
} IdObject;

/* A line of an identity record or anchor, read back. */
typedef struct IdEntry
{
	char	   *table;			/* (a record's) */
	char	   *column;			/* (a record's) NULL: the table */
	Oid			relid;			/* (an anchor's) */
	AttrNumber	attnum;			/* (an anchor's) */
	char	   *origin;
	int			n;
} IdEntry;

/* A field of an identity record or anchor, unescaped, in the server's encoding. */
static char *
identity_field(const char *f)
{
	char	   *u = unescape_field(f);

	return pg_any_to_server(u, strlen(u), PG_UTF8);
}

static int
identity_number(const char *f)
{
	char	   *end;
	long		n = strtol(f, &end, 10);

	if (*f == '\0' || *end != '\0' || n < 0 || n > PG_INT32_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("a prolly identity record is invalid")));
	return (int) n;
}

/*
 * The lines of an identity record (anchor false) or anchor (true), each an
 * IdEntry *.
 */
static List *
identity_entries(const ProllyValue *v, bool anchor)
{
	List	   *out = NIL;
	char	   *text;
	char	   *line;
	char	   *next;

	if (v == NULL || v->len == 0)
		return NIL;
	text = pnstrdup((const char *) v->data, v->len);
	for (line = text; line && *line; line = next)
	{
		char	   *f[5];
		int			nf = 0;
		char	   *p = line;
		IdEntry    *e = palloc0(sizeof(IdEntry));

		next = strchr(line, '\n');
		if (next)
			*next++ = '\0';
		for (;;)
		{
			f[nf++] = p;
			p = strchr(p, '\t');
			if (p == NULL || nf == lengthof(f))
				break;
			*p++ = '\0';
		}
		if (p != NULL)
			nf = 0;				/* (too many fields) */
		if (anchor && nf == 4)
		{
			e->relid = (Oid) strtoul(f[0], NULL, 10);
			e->attnum = (AttrNumber) identity_number(f[1]);
			e->origin = identity_field(f[2]);
			e->n = identity_number(f[3]);
		}
		else if (!anchor && nf == 4 && strcmp(f[0], "t") == 0)
		{
			e->table = identity_field(f[1]);
			e->origin = identity_field(f[2]);
			e->n = identity_number(f[3]);
		}
		else if (!anchor && nf == 5 && strcmp(f[0], "c") == 0)
		{
			e->table = identity_field(f[1]);
			e->column = identity_field(f[2]);
			e->origin = identity_field(f[3]);
			e->n = identity_number(f[4]);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("a prolly identity record is invalid")));
		out = lappend(out, e);
	}
	return out;
}

/* A record's entry for table (and column, if not NULL), or NULL. */
static IdEntry *
record_entry(List *entries, const char *table, const char *column)
{
	ListCell   *lc;

	foreach(lc, entries)
	{
		IdEntry    *e = lfirst(lc);

		if (strcmp(e->table, table) == 0 &&
			(column ? e->column && strcmp(e->column, column) == 0 : e->column == NULL))
			return e;
	}
	return NULL;
}

/* An anchor's entry for relid's attribute attnum (0: the table), or NULL. */
static IdEntry *
anchor_entry(List *entries, Oid relid, AttrNumber attnum)
{
	ListCell   *lc;

	foreach(lc, entries)
	{
		IdEntry    *e = lfirst(lc);

		if (e->relid == relid && e->attnum == attnum)
			return e;
	}
	return NULL;
}

/* o's identity as a record (entries) gives it by name: its entry's, or its own name's. */
static void
identity_by_name(List *entries, const char *table, IdObject *o)
{
	IdEntry    *e = record_entry(entries, table ? table : o->name,
								 table ? o->name : NULL);

	o->origin = pstrdup(e ? e->origin : o->name);
	o->n = e ? e->n : 0;
}

static int
id_object_cmp(const ListCell *a, const ListCell *b)
{
	return strcmp(((IdObject *) lfirst(a))->name, ((IdObject *) lfirst(b))->name);
}

static bool
same_identity(const IdObject *a, const IdObject *b)
{
	return a->n == b->n && strcmp(a->origin, b->origin) == 0;
}

/* The object among objs (IdObject *) with o's identity, but o, or NULL. */
static IdObject *
with_identity(List *objs, const IdObject *o)
{
	ListCell   *lc;

	foreach(lc, objs)
	{
		IdObject   *x = lfirst(lc);

		if (x != o && x->known && same_identity(x, o))
			return x;
	}
	return NULL;
}

static IdObject *
named_object(List *objs, const char *name)
{
	ListCell   *lc;

	foreach(lc, objs)
	{
		IdObject   *x = lfirst(lc);

		if (strcmp(x->name, name) == 0)
			return x;
	}
	return NULL;
}

/*
 * Settle o's identity, origin and n, among objs: if another holds it
 * already, the first of its own name's that none holds.
 */
static void
settle_identity(List *objs, IdObject *o, const char *origin, int n)
{
	o->origin = pstrdup(origin);
	o->n = n;
	if (with_identity(objs, o))
	{
		o->origin = pstrdup(o->name);
		o->n = 0;
		while (with_identity(objs, o))
			o->n++;
	}
	o->known = true;
}

/* Settle objs' identities: those an anchor has first, then the rest by name. */
static void
settle_identities(List *objs, List *anchors, List *entries, const char *table)
{
	ListCell   *lc;

	foreach(lc, objs)
	{
		IdObject   *o = lfirst(lc);
		IdEntry    *e = anchor_entry(anchors, o->relid, o->attnum);

		if (e)
			settle_identity(objs, o, e->origin, e->n);
	}
	foreach(lc, objs)
	{
		IdObject   *o = lfirst(lc);
		IdObject	by_name = *o;

		if (o->known)
			continue;
		identity_by_name(entries, table, &by_name);
		settle_identity(objs, o, by_name.origin, by_name.n);
	}
}

/* The tables (IdObject *) of a folded root, with identities by its record. */
static List *
root_identities(const ProllyNodeRef *root, const ProllyValue *record)
{
	Canon		cn;
	List	   *entries = identity_entries(record, false);
	List	   *tables = NIL;
	ProllyTreeCursor *c;
	ProllyKey	start = {(const uint8 *) "t:", 2};
	ProllyKey	k;
	ProllyValue v;

	memset(&cn, 0, sizeof(cn));
	canon_env(&cn);
	c = PROLLY_CALL(cn.tree, cursor_open, &cn.env, root, CurrentMemoryContext);
	PROLLY_CALL(cn.tree, cursor_seek, c, &start, ForwardScanDirection);
	while (PROLLY_CALL(cn.tree, cursor_next, c, &k, &v) &&
		   k.len >= 2 && memcmp(k.data, "t:", 2) == 0)
	{
		IdObject   *t = palloc0(sizeof(IdObject));
		ProllyValue def;
		TableDef	td;
		ListCell   *lc;

		t->name = from_utf8(k.data + 2, k.len - 2);
		identity_by_name(entries, NULL, t);
		t->known = true;
		definition_of(&k, &v, &def);
		parse_table_def(from_utf8(def.data, def.len), &td);
		foreach(lc, td.columns)
		{
			IdObject   *col = palloc0(sizeof(IdObject));

			col->name = unquoted(first_field(lfirst(lc)));
			identity_by_name(entries, t->name, col);
			col->known = true;
			t->columns = lappend(t->columns, col);
		}
		tables = lappend(tables, t);
	}
	PROLLY_CALL(cn.tree, cursor_close, c);
	return tables;
}

/* Append an identity record's or anchor's line: its fields, tab-separated. */
static void
identity_line(List **lines, const char *f1, const char *f2, const char *f3,
			  const char *f4, const char *f5)
{
	const char *fields[5] = {f1, f2, f3, f4, f5};
	StringInfoData line;

	initStringInfo(&line);
	for (int i = 0; i < lengthof(fields) && fields[i]; i++)
	{
		if (i > 0)
			appendStringInfoChar(&line, '\t');
		append_field(&line, fields[i]);
	}
	appendStringInfoChar(&line, '\n');
	*lines = lappend(*lines, line.data);
}

static void
append_lines(StringInfo out, List *lines)
{
	ListCell   *lc;

	list_sort(lines, cstring_cmp);
	foreach(lc, lines)
		appendStringInfoString(out, lfirst(lc));
}

/* The identity record of tables (IdObject *): what isn't their own names'. */
static void
identity_record(List *tables, StringInfo out)
{
	List	   *lines = NIL;
	ListCell   *lc;

	foreach(lc, tables)
	{
		IdObject   *t = lfirst(lc);
		ListCell   *cc;

		if (t->n != 0 || strcmp(t->origin, t->name) != 0)
			identity_line(&lines, "t", t->name, t->origin, psprintf("%d", t->n),
						  NULL);
		foreach(cc, t->columns)
		{
			IdObject   *col = lfirst(cc);

			if (col->n != 0 || strcmp(col->origin, col->name) != 0)
				identity_line(&lines, "c", t->name, col->name, col->origin,
							  psprintf("%d", col->n));
		}
	}
	append_lines(out, lines);
}

/*
 * A relation of the session branch's that identities are kept for (a
 * prolly table or a partitioned table, in a versioned schema, not an
 * extension's), with its live columns, as an IdObject; NULL if it isn't
 * one.  Read from the catalogs alone, without opening (or locking) the
 * relation: identities are worked out for reads of any table, which
 * mustn't wait for another's locks.
 */
static IdObject *
identified_relation(Form_pg_class form)
{
	IdObject   *t;

	if (!((form->relkind == RELKIND_RELATION &&
		   form->relam == PROLLY_TABLE_AM_OID) ||
		  form->relkind == RELKIND_PARTITIONED_TABLE) ||
		!canon_namespace(form->relnamespace) ||
		in_extension(RelationRelationId, form->oid))
		return NULL;
	t = palloc0(sizeof(IdObject));
	t->name = qualified(form->relnamespace, NameStr(form->relname));
	t->relid = form->oid;
	for (AttrNumber a = 1; a <= form->relnatts; a++)
	{
		HeapTuple	at = SearchSysCache2(ATTNUM, ObjectIdGetDatum(form->oid),
										 Int16GetDatum(a));
		Form_pg_attribute att;
		IdObject   *col;

		if (!HeapTupleIsValid(at))
			continue;
		att = (Form_pg_attribute) GETSTRUCT(at);
		if (!att->attisdropped)
		{
			col = palloc0(sizeof(IdObject));
			col->name = pstrdup(NameStr(att->attname));
			col->relid = form->oid;
			col->attnum = att->attnum;
			t->columns = lappend(t->columns, col);
		}
		ReleaseSysCache(at);
	}
	return t;
}

/*
 * The session branch's tables and columns (only's alone, if valid), each
 * found as its anchor entry says (its relation and attribute numbers kept
 * through renames), its identity that of the object it was before, so a
 * rename since keeps it.  With an anchor, one it hasn't is new since:
 * given its own name's identity (or, held, the first of that name's free),
 * never another's that its name had (an object dropped since and made
 * again under the name a rename gave the old one is another).  With none
 * (the working set just made what known describes, or moved since), each
 * is found by name in known.
 */
static void
folded_identify(const ProllyValue *anchor, const ProllyValue *known, Oid only,
				StringInfo identity, StringInfo next_anchor)
{
	List	   *anchors = identity_entries(anchor, true);
	List	   *entries = anchor ? NIL : identity_entries(known, false);
	List	   *tables = NIL;
	ListCell   *lc;

	if (OidIsValid(only))
	{
		HeapTuple	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(only));

		if (HeapTupleIsValid(tup))
		{
			IdObject   *t = identified_relation((Form_pg_class) GETSTRUCT(tup));

			if (t)
				tables = lappend(tables, t);
			ReleaseSysCache(tup);
		}
	}
	else
	{
		Relation	pg_class = table_open(RelationRelationId, AccessShareLock);
		SysScanDesc scan = catalog_branch_beginscan(pg_class, MyBranch);
		HeapTuple	tup;

		while ((tup = systable_getnext(scan)) != NULL)
		{
			IdObject   *t = identified_relation((Form_pg_class) GETSTRUCT(tup));

			if (t)
				tables = lappend(tables, t);
		}
		systable_endscan(scan);
		table_close(pg_class, AccessShareLock);
	}

	/* (in name order, so that a name's identities are given alike everywhere) */
	list_sort(tables, id_object_cmp);
	settle_identities(tables, anchors, entries, NULL);
	foreach(lc, tables)
	{
		IdObject   *t = lfirst(lc);

		/* (a table renamed since known: its columns by its name there) */
		IdObject	was = *t;
		IdEntry    *e = NULL;
		ListCell   *ec;

		foreach(ec, entries)
		{
			IdEntry    *x = lfirst(ec);

			if (x->column == NULL && x->n == t->n && strcmp(x->origin, t->origin) == 0)
				e = x;
		}
		was.name = e ? e->table : t->n == 0 ? t->origin : t->name;
		settle_identities(t->columns, anchors, entries, was.name);
	}
	identity_record(tables, identity);
	if (next_anchor == NULL || OidIsValid(only))
		return;
	{
		List	   *lines = NIL;

		foreach(lc, tables)
		{
			IdObject   *t = lfirst(lc);
			ListCell   *cc;

			identity_line(&lines, psprintf("%u", t->relid), "0", t->origin,
						  psprintf("%d", t->n), NULL);
			foreach(cc, t->columns)
			{
				IdObject   *col = lfirst(cc);

				identity_line(&lines, psprintf("%u", t->relid),
							  psprintf("%d", col->attnum), col->origin,
							  psprintf("%d", col->n), NULL);
			}
		}
		append_lines(next_anchor, lines);
	}
}

static ProllyRename *
make_rename(const char *table, const char *column, const char *to)
{
	ProllyRename *r = palloc(sizeof(ProllyRename));

	r->table = pstrdup(table);
	r->column = column ? pstrdup(column) : NULL;
	r->to = pstrdup(to);
	return r;
}

/* What renames make of table (column NULL) or its column, or NULL: none. */
static const char *
renamed_to(List *renames, const char *table, const char *column)
{
	ListCell   *lc;

	foreach(lc, renames)
	{
		ProllyRename *r = lfirst(lc);

		if (strcmp(r->table, table) == 0 &&
			(column ? r->column && strcmp(r->column, column) == 0 : r->column == NULL))
			return r->to;
	}
	return NULL;
}

/* Is name among names (IdObject *), or a name renames give (for table, if given)? */
static bool
name_used(List *a, List *b, List *renames, const char *table, const char *name)
{
	ListCell   *lc;

	if (named_object(a, name) || named_object(b, name))
		return true;
	foreach(lc, renames)
	{
		ProllyRename *r = lfirst(lc);

		if ((table ? r->column && strcmp(r->table, table) == 0 : r->column == NULL) &&
			strcmp(r->to, name) == 0)
			return true;
	}
	return false;
}

/*
 * A name for an object out of the way of another of its name: one neither
 * a nor b (IdObject *) nor renames use, in table's schema for a table.
 */
static char *
out_of_the_way(List *a, List *b, List *renames, const char *table,
			   const char *schema_of_table)
{
	for (int k = 1;; k++)
	{
		char	   *name = psprintf("prolly_renamed_%d", k);

		if (schema_of_table)
		{
			List	   *names = stringToQualifiedNameList(schema_of_table, NULL);

			name = list_length(names) > 1 ?
				quote_qualified_identifier(strVal(linitial(names)), name) :
				pstrdup(quote_identifier(name));
		}
		if (!name_used(a, b, renames, table, name))
			return name;
	}
}

/*
 * The renames that make the names of from's tables and columns (IdObject
 * *) those of to's of the same identities, and move each out of the way of
 * a name to gives an object of another identity.
 */
static List *
identity_renames(List *from, List *to)
{
	List	   *renames = NIL;
	ListCell   *lc;

	foreach(lc, from)
	{
		IdObject   *f = lfirst(lc);
		IdObject   *t = with_identity(to, f);
		ListCell   *cc;

		if (t == NULL)
		{
			if (named_object(to, f->name))
				renames = lappend(renames,
								  make_rename(f->name, NULL,
											  out_of_the_way(from, to, renames, NULL, f->name)));
			continue;
		}
		if (strcmp(f->name, t->name) != 0)
			renames = lappend(renames, make_rename(f->name, NULL, t->name));
		foreach(cc, f->columns)
		{
			IdObject   *fc = lfirst(cc);
			IdObject   *tc = with_identity(t->columns, fc);

			if (tc && strcmp(fc->name, tc->name) != 0)
				renames = lappend(renames, make_rename(f->name, fc->name, tc->name));
			else if (tc == NULL && named_object(t->columns, fc->name))
				renames = lappend(renames,
								  make_rename(f->name, fc->name,
											  out_of_the_way(f->columns, t->columns,
															 renames, f->name, NULL)));
		}
	}
	return renames;
}

static List *
folded_renames(const ProllyNodeRef *from, const ProllyValue *from_identity,
			   const ProllyNodeRef *to, const ProllyValue *to_identity)
{
	return identity_renames(root_identities(from, from_identity),
							root_identities(to, to_identity));
}

/* An object of a merge, as each of its three roots names it (NULL: not there). */
typedef struct MergedObject
{
	IdObject	id;				/* its identity, and its merged name */
	IdObject   *in[3];
	List	   *columns;		/* (a table's) MergedObject * */
} MergedObject;

static MergedObject *
merged_object(List **objs, const IdObject *o, int side)
{
	ListCell   *lc;
	MergedObject *m;

	foreach(lc, *objs)
	{
		m = lfirst(lc);
		if (same_identity(&m->id, o))
		{
			m->in[side] = (IdObject *) o;
			return m;
		}
	}
	m = palloc0(sizeof(MergedObject));
	m->id.origin = o->origin;
	m->id.n = o->n;
	m->id.known = true;
	m->in[side] = (IdObject *) o;
	*objs = lappend(*objs, m);
	return m;
}

/*
 * The name a merge gives an object (a column of table, if table isn't
 * NULL): as the side that renamed it names it.
 */
static char *
merged_name(MergedObject *m, const char *table)
{
	const char *b = m->in[0] ? m->in[0]->name : NULL;
	const char *o = m->in[1] ? m->in[1]->name : NULL;
	const char *t = m->in[2] ? m->in[2]->name : NULL;
	const char *was = b ? b : m->id.origin;

	if (o && t)
	{
		if (b && strcmp(o, b) == 0)
			return pstrdup(t);
		if (strcmp(o, t) == 0 || (b && strcmp(t, b) == 0))
			return pstrdup(o);
		if (table)
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errschemaconflict(),
					 errmsg("schema conflict: column %s of %s is renamed %s on one side and %s on the other",
							quote_identifier(was), table, quote_identifier(o),
							quote_identifier(t))));
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errschemaconflict(),
				 errmsg("schema conflict: table %s is renamed %s on one side and %s on the other",
						was, o, t)));
	}
	return pstrdup(o ? o : t ? t : b);
}

/*
 * Does a merge keep the object: both sides have it, or one made it?  (One
 * a side dropped goes, unless the other changed it: then the merge of its
 * definitions fails.)
 */
static bool
merge_keeps(const MergedObject *m)
{
	return (m->in[1] && m->in[2]) || (!m->in[0] && (m->in[1] || m->in[2]));
}

static bool
merged_name_used(List *objs, const MergedObject *m, const char *name)
{
	ListCell   *lc;

	foreach(lc, objs)
	{
		MergedObject *x = lfirst(lc);

		if (x != m && x->id.name && strcmp(x->id.name, name) == 0)
			return true;
	}
	return false;
}

/*
 * Refuse two objects a merge keeps (MergedObject *, columns of table if it
 * isn't NULL) given one name; one it doesn't keep, named as another, is
 * moved out of the way.
 */
static void
settle_merged_names(List *objs, const char *table)
{
	ListCell   *lc;

	foreach(lc, objs)
	{
		MergedObject *m = lfirst(lc);

		if (merge_keeps(m) && merged_name_used(objs, m, m->id.name))
		{
			ListCell   *oc;

			foreach(oc, objs)
			{
				MergedObject *x = lfirst(oc);

				if (x != m && merge_keeps(x) && strcmp(x->id.name, m->id.name) == 0)
					ereport(ERROR,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errschemaconflict(),
							 table ?
							 errmsg("schema conflict: the merge would give two columns of %s the name %s",
									table, quote_identifier(m->id.name)) :
							 errmsg("schema conflict: the merge would give two tables the name %s",
									m->id.name),
							 errdetail("One side renamed one to the name, and the other has another of the name.")));
			}
		}
	}
	foreach(lc, objs)
	{
		MergedObject *m = lfirst(lc);

		if (merge_keeps(m) || !merged_name_used(objs, m, m->id.name))
			continue;
		for (int k = 1;; k++)
		{
			char	   *name = psprintf("prolly_renamed_%d", k);

			if (table == NULL)
			{
				List	   *names = stringToQualifiedNameList(m->id.name, NULL);

				name = list_length(names) > 1 ?
					quote_qualified_identifier(strVal(linitial(names)), name) :
					pstrdup(quote_identifier(name));
			}
			if (!merged_name_used(objs, m, name))
			{
				m->id.name = name;
				break;
			}
		}
	}
}

static void
folded_merge_renames(const ProllyNodeRef *roots, const ProllyValue *identities,
					 List *keep_ours, List **renames, StringInfo identity)
{
	List	   *merged = NIL;
	List	   *tables = NIL;
	List	   *kept_as_ours = NIL;
	List	   *kept_merged_names = NIL;
	ListCell   *lc;
	ListCell   *kc;
	ListCell   *nc;

	for (int i = 0; i < 3; i++)
	{
		foreach(lc, root_identities(&roots[i], &identities[i]))
		{
			IdObject   *t = lfirst(lc);
			MergedObject *m = merged_object(&merged, t, i);
			ListCell   *cc;

			foreach(cc, t->columns)
				(void) merged_object(&m->columns, lfirst(cc), i);
		}
		renames[i] = NIL;
	}
	foreach(lc, merged)
	{
		MergedObject *m = lfirst(lc);
		ListCell   *cc;

		bool		ours = false;

		m->id.name = merged_name(m, NULL);

		/* (kept as ours has it: ours' names, the table's and its columns') */
		foreach_ptr(char, keep, keep_ours)
			ours |= strcmp(keep, m->id.name) == 0;
		if (ours && m->in[1])
		{
			kept_as_ours = lappend(kept_as_ours, m);
			kept_merged_names = lappend(kept_merged_names, m->id.name);
			m->id.name = pstrdup(m->in[1]->name);
		}
		foreach(cc, m->columns)
		{
			MergedObject *c = lfirst(cc);

			c->id.name = ours && c->in[1] ? pstrdup(c->in[1]->name) :
				merged_name(c, m->id.name);
		}
		settle_merged_names(m->columns, m->id.name);
	}

	/*
	 * (another table the merge keeps under the name a table kept as ours
	 * has it takes, theirs having swapped their names or made another of
	 * it: refused, not kept)
	 */
	forboth(kc, kept_as_ours, nc, kept_merged_names)
	{
		MergedObject *m = lfirst(kc);

		foreach_ptr(MergedObject, x, merged)
			if (x != m && merge_keeps(x) && strcmp(x->id.name, m->id.name) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errschemaconflict(),
						 errmsg("schema conflict: %s was changed differently on both sides, and renamed on one",
								(char *) lfirst(nc)),
						 errdetail("Kept as the branch names it, %s, it would share that name with another table the merge keeps.",
								   m->id.name)));
	}
	settle_merged_names(merged, NULL);

	/* each side's renames, and the merge's identities */
	foreach(lc, merged)
	{
		MergedObject *m = lfirst(lc);
		IdObject   *t = palloc0(sizeof(IdObject));
		ListCell   *cc;

		*t = m->id;
		t->columns = NIL;
		for (int i = 0; i < 3; i++)
			if (m->in[i] && strcmp(m->in[i]->name, m->id.name) != 0)
				renames[i] = lappend(renames[i],
									 make_rename(m->in[i]->name, NULL, m->id.name));
		foreach(cc, m->columns)
		{
			MergedObject *c = lfirst(cc);
			IdObject   *col = palloc(sizeof(IdObject));

			*col = c->id;
			if (merge_keeps(c))
				t->columns = lappend(t->columns, col);
			for (int i = 0; i < 3; i++)
				if (m->in[i] && c->in[i] && strcmp(c->in[i]->name, c->id.name) != 0)
					renames[i] = lappend(renames[i],
										 make_rename(m->in[i]->name, c->in[i]->name,
													 c->id.name));
		}
		if (merge_keeps(m))
			tables = lappend(tables, t);
	}
	identity_record(tables, identity);
}

/*
 * A token of SQL as the deparser prints it, for renaming the tables and
 * columns it names: an identifier (quoted or not, its name in value), a
 * literal or anything else, copied as it is.
 */
typedef struct SqlToken
{
	int			start;
	int			end;
	bool		ident;
	bool		quoted;
	char	   *value;			/* (an identifier's name) */
} SqlToken;

/* The tokens of sql, in order. */
static List *
sql_tokens(const char *sql)
{
	List	   *tokens = NIL;
	int			n = strlen(sql);
	int			i = 0;

	while (i < n)
	{
		SqlToken   *t = palloc0(sizeof(SqlToken));
		unsigned char c = (unsigned char) sql[i];

		t->start = i;
		if (c == '\'' || ((c == 'E' || c == 'e') && sql[i + 1] == '\'' &&
						  (i == 0 || !(isalnum((unsigned char) sql[i - 1]) ||
									   sql[i - 1] == '_'))))
		{
			bool		escapes = c != '\'';

			i += escapes ? 2 : 1;
			while (i < n)
			{
				if (escapes && sql[i] == '\\' && i + 1 < n)
					i += 2;
				else if (sql[i] == '\'' && sql[i + 1] == '\'')
					i += 2;
				else if (sql[i] == '\'')
				{
					i++;
					break;
				}
				else
					i++;
			}
		}
		else if (c == '$' && (sql[i + 1] == '$' || isalpha((unsigned char) sql[i + 1]) ||
							  sql[i + 1] == '_'))
		{
			int			j = i + 1;
			char	   *tag;
			const char *close;

			while (j < n && (isalnum((unsigned char) sql[j]) || sql[j] == '_'))
				j++;
			if (sql[j] != '$')
				i = j;			/* (not a dollar quote) */
			else
			{
				tag = pnstrdup(sql + i, j + 1 - i);
				close = strstr(sql + j + 1, tag);
				i = close ? (close - sql) + strlen(tag) : n;
			}
		}
		else if (c == '"')
		{
			StringInfoData v;

			initStringInfo(&v);
			i++;
			while (i < n)
			{
				if (sql[i] == '"' && sql[i + 1] == '"')
				{
					appendStringInfoChar(&v, '"');
					i += 2;
				}
				else if (sql[i] == '"')
				{
					i++;
					break;
				}
				else
					appendStringInfoChar(&v, sql[i++]);
			}
			t->ident = true;
			t->quoted = true;
			t->value = v.data;
		}
		else if (isalpha(c) || c == '_' || c >= 0x80)
		{
			while (i < n && (isalnum((unsigned char) sql[i]) || sql[i] == '_' ||
							 sql[i] == '$' || (unsigned char) sql[i] >= 0x80))
				i++;
			t->ident = true;
			t->value = pnstrdup(sql + t->start, i - t->start);
		}
		else if (isdigit(c))
		{
			while (i < n && (isalnum((unsigned char) sql[i]) || sql[i] == '.' ||
							 sql[i] == '_'))
				i++;
		}
		else
			i++;
		t->end = i;
		tokens = lappend(tokens, t);
	}
	return tokens;
}

/* Is token i of tokens (not an identifier) text? */
static bool
token_is(List *tokens, int i, const char *sql, const char *text)
{
	SqlToken   *t;

	if (i < 0 || i >= list_length(tokens))
		return false;
	t = list_nth(tokens, i);
	return !t->ident && t->end - t->start == (int) strlen(text) &&
		strncmp(sql + t->start, text, t->end - t->start) == 0;
}

/* The next token after i that isn't blank, or -1. */
static int
next_token(List *tokens, int i, const char *sql)
{
	for (i++; i < list_length(tokens); i++)
	{
		SqlToken   *t = list_nth(tokens, i);

		if (t->ident || !isspace((unsigned char) sql[t->start]))
			return i;
	}
	return -1;
}

static int
prev_token(List *tokens, int i, const char *sql)
{
	for (i--; i >= 0; i--)
	{
		SqlToken   *t = list_nth(tokens, i);

		if (t->ident || !isspace((unsigned char) sql[t->start]))
			return i;
	}
	return -1;
}

/* Is identifier token i an unquoted keyword among words? */
static bool
keyword_among(List *tokens, int i, const char *const *words)
{
	SqlToken   *t;

	if (i < 0)
		return false;
	t = list_nth(tokens, i);
	if (!t->ident || t->quoted)
		return false;
	for (; *words; words++)
		if (pg_strcasecmp(t->value, *words) == 0)
			return true;
	return false;
}

/*
 * sql, a definition of table's (as a version names it: qualified,
 * quoted) as the deparser printed it, with the tables and columns it names
 * renamed as renames say, into out; false (out as sql) if none are.  A
 * column is an unqualified name (or one qualified by NEW or OLD) that
 * isn't a function's, a type's or an object's being named, table's, or,
 * in a REFERENCES list, the referenced table's; a table is a name
 * qualified by its schema.
 */
static bool
rename_sql(const char *sql, const char *table, List *renames, StringInfo out)
{
	static const char *const named[] = {"INDEX", "CONSTRAINT", "TRIGGER", "RULE",
	"POLICY", "STATISTICS", "COLLATE", "FUNCTION", "PROCEDURE", "TYPE", NULL};
	static const char *const rows[] = {"NEW", "OLD", NULL};
	static const char *const referencing[] = {"REFERENCES", NULL};
	List	   *tokens = sql_tokens(sql);
	const char *columns_of = table;
	int			depth = 0;
	int			referenced_depth = -1;
	bool		changed = false;
	int			at = 0;

	for (int i = 0; i < list_length(tokens); i++)
	{
		SqlToken   *t = list_nth(tokens, i);
		int			p;
		int			nx;
		const char *to = NULL;
		int			last = i;

		if (token_is(tokens, i, sql, "("))
			depth++;
		else if (token_is(tokens, i, sql, ")"))
		{
			if (--depth == referenced_depth)
			{
				columns_of = table;
				referenced_depth = -1;
			}
		}
		if (!t->ident)
			continue;
		p = prev_token(tokens, i, sql);
		/* (a type after ::, an object's own name) */
		if (token_is(tokens, p, sql, ":") || keyword_among(tokens, p, named))
		{
			while (token_is(tokens, last + 1, sql, ".") && last + 2 < list_length(tokens))
				last += 2;
			i = last;
			continue;
		}
		if (token_is(tokens, i + 1, sql, ".") && i + 2 < list_length(tokens) &&
			((SqlToken *) list_nth(tokens, i + 2))->ident)
		{
			SqlToken   *second = list_nth(tokens, i + 2);
			char	   *qualified_name = quote_qualified_identifier(t->value,
																	second->value);
			bool		referenced = keyword_among(tokens, p, referencing);

			last = i + 2;
			if (keyword_among(tokens, i, rows))
			{
				to = renamed_to(renames, table, second->value);
				if (to)
				{
					appendBinaryStringInfo(out, sql + at, second->start - at);
					appendStringInfoString(out, quote_identifier(to));
					at = second->end;
					changed = true;
				}
				i = last;
				continue;
			}
			to = renamed_to(renames, qualified_name, NULL);
			if (referenced)
			{
				columns_of = pstrdup(qualified_name);
				referenced_depth = depth;
			}
			if (to)
			{
				appendBinaryStringInfo(out, sql + at, t->start - at);
				appendStringInfoString(out, to);
				at = second->end;
				changed = true;
			}
			/* (schema.table.column) */
			if (token_is(tokens, i + 3, sql, ".") && i + 4 < list_length(tokens) &&
				((SqlToken *) list_nth(tokens, i + 4))->ident)
			{
				SqlToken   *third = list_nth(tokens, i + 4);
				const char *col = renamed_to(renames, qualified_name, third->value);

				last = i + 4;
				if (col)
				{
					appendBinaryStringInfo(out, sql + at, third->start - at);
					appendStringInfoString(out, quote_identifier(col));
					at = third->end;
					changed = true;
				}
			}
			i = last;
			continue;
		}
		nx = next_token(tokens, i, sql);
		/* (a function's name, or a name qualifying another) */
		if (token_is(tokens, nx, sql, "(") || token_is(tokens, i + 1, sql, "."))
			continue;
		to = renamed_to(renames, columns_of, t->value);
		if (to)
		{
			appendBinaryStringInfo(out, sql + at, t->start - at);
			appendStringInfoString(out, quote_identifier(to));
			at = t->end;
			changed = true;
		}
	}
	appendStringInfoString(out, sql + at);
	return changed;
}

/*
 * A field of a description (escaped, UTF-8) with what it names renamed
 * (rename_sql()), appended to out.
 */
static void
append_renamed_field(StringInfo out, const char *field, const char *table,
					 List *renames)
{
	StringInfoData sql;

	initStringInfo(&sql);
	if (rename_sql(identity_field(field), table, renames, &sql))
		append_field(out, sql.data);
	else
		appendStringInfoString(out, field);
	pfree(sql.data);
}

/*
 * A table's description (UTF-8, as its item holds it) with the tables and
 * columns it names renamed as renames say (table its own name, as its
 * root has it), into out: its columns' names, and what its definitions
 * name (rename_sql()), as Postgres would print them had the renames been
 * made, so that one side's renamed definitions merge with the other's.
 * An object's own name (a constraint's, a statistic's) is left as it is,
 * as are options.
 */
static void
rename_columns(const char *desc, int len, const char *table, List *renames,
			   StringInfo out)
{
	char	   *copy = pnstrdup(desc, len);
	char	   *line;
	char	   *next;
	const char *section = NULL;

	for (line = copy; line && *line; line = next)
	{
		bool		header = false;
		char	   *field;
		char	   *tab;
		int			nfield = 0;

		next = strchr(line, '\n');
		if (next)
			*next++ = '\0';
		for (int s = 0; s < NUM_TABLE_SECTIONS; s++)
			if (strcmp(line, table_sections[s]) == 0)
			{
				section = table_sections[s];
				header = true;
			}
		if (header || section == NULL || strcmp(section, "options") == 0)
			appendStringInfoString(out, line);
		else if (strcmp(section, "local") == 0 && strncmp(line, "column ", 7) == 0)
		{
			const char *to = renamed_to(renames, table,
										unquoted(identity_field(line + 7)));

			appendStringInfoString(out, "column ");
			if (to)
				append_field(out, quote_identifier(to));
			else
				appendStringInfoString(out, line + 7);
		}
		else
			for (field = line; field; field = tab, nfield++)
			{
				tab = strchr(field, '\t');
				if (tab)
					*tab++ = '\0';
				if (nfield > 0)
					appendStringInfoChar(out, '\t');
				if (nfield == 0 && strcmp(section, "columns") == 0)
				{
					const char *to = renamed_to(renames, table,
												unquoted(identity_field(field)));

					if (to)
						append_field(out, quote_identifier(to));
					else
						appendStringInfoString(out, field);
				}
				else if (nfield == 0 && (strcmp(section, "constraints") == 0 ||
										 strcmp(section, "statistics") == 0))
					appendStringInfoString(out, field);
				else
					append_renamed_field(out, field, table, renames);
			}
		if (next)
			appendStringInfoChar(out, '\n');
	}
}

static void
folded_rename_description(const ProllyValue *description, const char *table,
						  List *renames, StringInfo out)
{
	rename_columns((const char *) description->data, description->len, table,
				   renames, out);
}

static void
folded_rename_root(const ProllyNodeRef *root, List *renames, ProllyNodeRef *out)
{
	Canon		cn;
	ProllyTreeCursor *c;
	ProllyKey	k;
	ProllyValue v;
	List	   *items = NIL;
	ProllyTreeBuilder *b;
	ListCell   *lc;
	int			olen = strlen(ORDERING_PREFIX);

	memset(&cn, 0, sizeof(cn));
	canon_env(&cn);
	c = PROLLY_CALL(cn.tree, cursor_open, &cn.env, root, CurrentMemoryContext);
	PROLLY_CALL(cn.tree, cursor_seek, c, NULL, ForwardScanDirection);
	while (PROLLY_CALL(cn.tree, cursor_next, c, &k, &v))
	{
		CanonItem  *it = palloc(sizeof(CanonItem));

		initStringInfo(&it->key);
		initStringInfo(&it->value);
		if (k.len >= 2 && memcmp(k.data, "t:", 2) == 0)
		{
			char	   *name = from_utf8(k.data + 2, k.len - 2);
			const char *to = renamed_to(renames, name, NULL);
			ProllyNodeRef data;
			ProllyValue def;

			appendStringInfoString(&it->key, "t:");
			append_utf8(&it->key, to ? to : name);
			(void) tree_item(&k, &v, &data, &def);
			put_hash(&it->value, &data.hash);
			rename_columns((const char *) def.data, def.len, name, renames,
						   &it->value);
		}
		else if (k.len > olen && memcmp(k.data, ORDERING_PREFIX, olen) == 0)
		{
			char	   *name = from_utf8(k.data + olen, k.len - olen);
			const char *to = renamed_to(renames, name, NULL);

			appendStringInfoString(&it->key, ORDERING_PREFIX);
			append_utf8(&it->key, to ? to : name);
			appendBinaryStringInfo(&it->value, (const char *) v.data, v.len);
		}
		else
		{
			appendBinaryStringInfo(&it->key, (const char *) k.data, k.len);
			appendBinaryStringInfo(&it->value, (const char *) v.data, v.len);
		}
		items = lappend(items, it);
		CHECK_FOR_INTERRUPTS();
	}
	PROLLY_CALL(cn.tree, cursor_close, c);
	list_sort(items, item_cmp);
	b = PROLLY_CALL(cn.tree, build_begin, &cn.root_env, CurrentMemoryContext);
	foreach(lc, items)
	{
		CanonItem  *it = lfirst(lc);
		ProllyKey	ik = {(const uint8 *) it->key.data, it->key.len};
		ProllyValue iv = {(const uint8 *) it->value.data, it->value.len};

		PROLLY_CALL(cn.tree, build_add, b, &ik, &iv);
	}
	PROLLY_CALL(cn.tree, build_finish, b, out);
}

/* A table of the session branch's, by its qualified name. */
/*
 * Run a rename of relid as its owner, where the role renaming has the
 * owner's privileges and isn't it (as a move changes what the branch
 * has); else as that role, which PostgreSQL then holds to owning it.
 */
static void
renamed_as_owner(const char *sql, Oid relid)
{
	Oid			owner = relation_owner_of(relid);

	made_as(sql, OidIsValid(owner) && owner != GetUserId() &&
			has_privs_of_role(GetUserId(), owner) ? owner : InvalidOid);
}

static Oid
renamed_relid(const char *name)
{
	return RangeVarGetRelid(makeRangeVarFromNameList(stringToQualifiedNameList(name, NULL)),
							AccessExclusiveLock, false);
}

/*
 * Rename the session branch's tables and columns as renames say, each
 * first to a name of its own, so names swapped or passed along don't
 * collide on the way: columns, by their tables' names before, both of
 * their steps, then tables (so a column of a table renamed too is renamed
 * while its table still has the name it is named by).
 */
static void
folded_rename_objects(List *renames)
{
	List	   *first = NIL;
	List	   *then = NIL;
	List	   *columns_then = NIL;
	ListCell   *lc;
	int			save_nestlevel;
	int			k = 0;
	HTAB	   *statements = maker_statements;

	if (renames == NIL)
		return;
	/* (each renamed as its table's owner, where the role moving may) */
	maker_statements = NULL;
	foreach(lc, renames)
	{
		ProllyRename *r = lfirst(lc);
		Oid			relid = renamed_relid(r->table);
		char	   *table = qualified(get_rel_namespace(relid), get_rel_name(relid));
		char	   *step;

		if (r->column == NULL)
			continue;
		do
			step = psprintf("prolly_renaming_%d", ++k);
		while (get_attnum(relid, step) != InvalidAttrNumber);
		first = lappend(first, psprintf("ALTER TABLE %s RENAME COLUMN %s TO %s",
										table, quote_identifier(r->column),
										quote_identifier(step)));
		renamed_as_owner(llast(first), relid);
		columns_then = lappend(columns_then,
							   psprintf("ALTER TABLE %s RENAME COLUMN %s TO %s",
										table, quote_identifier(step),
										quote_identifier(r->to)));
		renamed_as_owner(llast(columns_then), relid);
	}
	first = list_concat(first, columns_then);
	foreach(lc, renames)
	{
		ProllyRename *r = lfirst(lc);
		Oid			relid;
		Oid			nsp;
		List	   *to;
		char	   *schema;
		char	   *step;

		if (r->column != NULL)
			continue;
		relid = renamed_relid(r->table);
		nsp = get_rel_namespace(relid);
		to = stringToQualifiedNameList(r->to, NULL);
		schema = list_length(to) > 1 ? strVal(linitial(to)) : get_namespace_name(nsp);
		do
			step = psprintf("prolly_renaming_%d", ++k);
		while (OidIsValid(get_relname_relid(step, nsp)) ||
			   OidIsValid(get_relname_relid(step, get_namespace_oid(schema, false))));
		first = lappend(first, psprintf("ALTER TABLE %s RENAME TO %s",
										qualified(nsp, get_rel_name(relid)),
										quote_identifier(step)));
		renamed_as_owner(llast(first), relid);
		if (strcmp(schema, get_namespace_name(nsp)) != 0)
		{
			then = lappend(then, psprintf("ALTER TABLE %s SET SCHEMA %s",
										  qualified(nsp, step),
										  quote_identifier(schema)));
			renamed_as_owner(llast(then), relid);
		}
		then = lappend(then, psprintf("ALTER TABLE %s RENAME TO %s",
									  quote_qualified_identifier(schema, step),
									  quote_identifier(strVal(llast(to)))));
		renamed_as_owner(llast(then), relid);
	}
	save_nestlevel = NewGUCNestLevel();
	describing_settings();
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	PG_TRY();
	{
		run_statements(first);
		run_statements(then);
	}
	PG_FINALLY();
	{
		maker_statements = statements;
	}
	PG_END_TRY();
	SPI_finish();
	AtEOXact_GUC(true, save_nestlevel);
	CommandCounterIncrement();
}

/*
 * The name a root's "c:" items give a collation: "default", or the
 * collation qualified.
 */
static char *
collation_item_name(Oid coll)
{
	HeapTuple	tup;
	Form_pg_collation form;
	char	   *name;

	if (coll == DEFAULT_COLLATION_OID)
		return pstrdup("default");
	tup = SearchSysCache1(COLLOID, ObjectIdGetDatum(coll));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for collation %u", coll);
	form = (Form_pg_collation) GETSTRUCT(tup);
	name = qualified(form->collnamespace, NameStr(form->collname));
	ReleaseSysCache(tup);
	return name;
}

/*
 * The key columns of an index statement of a table's description (and
 * its INCLUDE columns, if include isn't NULL), resolved in this database:
 * NULL, or why they can't be ordered here as the version's index orders
 * them.  Built-in types, operator classes and collations only: an
 * extension's comparator may order keys otherwise than the version did,
 * and nothing proves it doesn't.
 */
static const char *
index_columns(const char *what, const ProllyComponents *c, List *stale,
			  IndexStmt *stmt,
			  List *colnames, TupleDesc desc, int *nkeys, AttrNumber *attnos,
			  Oid *opfamilies, Oid *collations, int16 *options,
			  int *ninclude, AttrNumber *include)
{
	ListCell   *lc;

	*nkeys = 0;
	if (ninclude)
		*ninclude = 0;
	if (strcmp(stmt->accessMethod, "btree") != 0)
		return psprintf("%s isn't a btree index", what);
	if (list_length(stmt->indexParams) + list_length(stmt->indexIncludingParams) >
		INDEX_MAX_KEYS)
		return psprintf("%s has too many columns", what);
	if (stmt->whereClause != NULL)
		return psprintf("%s is a partial index", what);
	foreach(lc, stmt->indexParams)
	{
		IndexElem  *e = lfirst_node(IndexElem, lc);
		int			attno = 0;
		Oid			type;
		Oid			opclass;
		Oid			coll;
		int16		option = 0;
		ListCell   *cl;
		int			i = 0;

		if (e->name == NULL)
			return psprintf("%s has an expression", what);
		foreach(cl, colnames)
		{
			i++;
			if (strcmp(lfirst(cl), e->name) == 0)
				attno = i;
		}
		if (attno == 0)
			return psprintf("%s names a column the table hasn't", what);
		type = TupleDescAttr(desc, attno - 1)->atttypid;
		opclass = e->opclass == NIL ? GetDefaultOpClass(type, BTREE_AM_OID) :
			get_opclass_oid(BTREE_AM_OID, e->opclass, true);
		coll = e->collation == NIL ? TupleDescAttr(desc, attno - 1)->attcollation :
			get_collation_oid(e->collation, true);
		if (!OidIsValid(opclass) || (e->collation != NIL && !OidIsValid(coll)))
			return psprintf("%s has an operator class or collation this database hasn't", what);
		if (type >= FirstNormalObjectId || opclass >= FirstNormalObjectId ||
			coll >= FirstNormalObjectId)
			return psprintf("%s is ordered by a type, operator class or collation that isn't built in", what);
		if (OidIsValid(coll))
		{
			char	   *cname = collation_item_name(coll);

			for (int j = 0; j + 1 < list_length(stale); j += 2)
				if (strcmp(strVal(list_nth(stale, j)), cname) == 0)
					return strVal(list_nth(stale, j + 1));
		}
		if (e->ordering == SORTBY_DESC)
			option |= INDOPTION_DESC | INDOPTION_NULLS_FIRST;
		if (e->nulls_ordering == SORTBY_NULLS_FIRST)
			option |= INDOPTION_NULLS_FIRST;
		else if (e->nulls_ordering == SORTBY_NULLS_LAST)
			option &= ~INDOPTION_NULLS_FIRST;
		attnos[*nkeys] = attno;
		opfamilies[*nkeys] = get_opclass_family(opclass);
		collations[*nkeys] = coll;
		options[*nkeys] = option;
		if (!PROLLY_CALL(c->key_encoder, supports, opfamilies[*nkeys], type, coll))
			return psprintf("the key encoder can't order %s", what);
		(*nkeys)++;
	}
	foreach(lc, stmt->indexIncludingParams)
	{
		IndexElem  *e = lfirst_node(IndexElem, lc);
		ListCell   *cl;
		int			i = 0;
		int			attno = 0;

		if (include == NULL)
			continue;
		if (e->name == NULL)
			return psprintf("%s includes an expression", what);
		foreach(cl, colnames)
		{
			i++;
			if (strcmp(lfirst(cl), e->name) == 0)
				attno = i;
		}
		if (attno == 0)
			return psprintf("%s includes a column the table hasn't", what);
		include[(*ninclude)++] = attno;
	}
	return NULL;
}

/*
 * The routines and types of the session's branch a commit's record lists
 * (prolly_owned_routines()), kept from one commit to the next: a commit
 * lists them all, so that a move finds any one's owner, but works out
 * again only those changed since.  Each is kept with its catalog row's
 * place and inserting transaction, which any change to the row (an ALTER,
 * an owner changed) changes, and with what its name is made of besides:
 * the schemas and types a routine's name and arguments name, a type's
 * schema.  The caches' invalidations say what to look at again: a
 * routine's row (PROCOID) has the routines read again; a type's or a
 * schema's (TYPEOID, NAMESPACEOID) has both read again, and the name of
 * each one whose row or whose names' objects that invalidation is of
 * (matched by the caches' hash of their OIDs) worked out again.  Every
 * other row unchanged keeps its name.  So a table made, a temporary one
 * too (a type invalidation for its row type), reads the rows but works
 * out no name.  A reset of the caches, or many invalidations at once, has
 * everything worked out again, as does a move to another branch.  Every
 * member, an extension's too, is listed: membership changes (ALTER
 * EXTENSION ... ADD) invalidate nothing, and a line no item has is never
 * read.
 */
typedef struct OwnedObject
{
	Oid			oid;
	Oid			owner;
	char		kind;
	char	   *name;
	ItemPointerData tid;
	TransactionId xmin;
	int			ntypes;			/* (the types its name names) */
	Oid		   *types;
	int			nschemas;		/* (and the schemas) */
	Oid		   *schemas;
} OwnedObject;

/* An OwnedObject known before, by OID */
typedef struct OwnedKnown
{
	Oid			oid;
	OwnedObject *o;
} OwnedKnown;

static MemoryContext owned_cxt = NULL;
static Oid	owned_branch = InvalidOid;
static List *owned_routines = NIL;
static List *owned_types = NIL;
static bool owned_routines_known = false;
static bool owned_types_known = false;
static bool owned_names_known = false;
static bool owned_callbacks = false;
static uint64 owned_generation = 0;

/*
 * The caches' hash values of the types and schemas invalidated since the
 * listing was last read (a reset, or more than fit: owned_names_known
 * false instead).
 */
#define OWNED_CHANGED_MAX 64
static uint32 owned_changed_types[OWNED_CHANGED_MAX];
static int	owned_nchanged_types = 0;
static uint32 owned_changed_schemas[OWNED_CHANGED_MAX];
static int	owned_nchanged_schemas = 0;

/*
 * (only flags and hash values: an invalidation may come while the lists
 * are read, which goes on with what it has; the next listing reads them
 * again)
 */
static void
owned_forget(Datum arg, int cacheid, uint32 hashvalue)
{
	owned_routines_known = false;
	if (cacheid == PROCOID)
		return;
	owned_types_known = false;
	if (hashvalue == 0)
		owned_names_known = false;
	else if (cacheid == TYPEOID)
	{
		if (owned_nchanged_types < OWNED_CHANGED_MAX)
			owned_changed_types[owned_nchanged_types++] = hashvalue;
		else
			owned_names_known = false;
	}
	else
	{
		if (owned_nchanged_schemas < OWNED_CHANGED_MAX)
			owned_changed_schemas[owned_nchanged_schemas++] = hashvalue;
		else
			owned_names_known = false;
	}
}

static bool
hash_among(uint32 h, const uint32 *hashes, int n)
{
	for (int i = 0; i < n; i++)
		if (hashes[i] == h)
			return true;
	return false;
}

/* Whether an invalidation since it was known may have changed o's name. */
static bool
owned_name_changed(const OwnedObject *o)
{
	for (int i = 0; i < o->ntypes && owned_nchanged_types > 0; i++)
		if (hash_among(GetSysCacheHashValue1(TYPEOID, ObjectIdGetDatum(o->types[i])),
					   owned_changed_types, owned_nchanged_types))
			return true;
	for (int i = 0; i < o->nschemas && owned_nchanged_schemas > 0; i++)
		if (hash_among(GetSysCacheHashValue1(NAMESPACEOID, ObjectIdGetDatum(o->schemas[i])),
					   owned_changed_schemas, owned_nchanged_schemas))
			return true;
	return false;
}

/*
 * What a routine's item name is made of besides its row (o's types and
 * schemas, in owned_cxt): its schema, and each argument's type, its
 * element's if an array, and their schemas.
 */
static void
routine_name_parts(OwnedObject *o, HeapTuple tup)
{
	Form_pg_proc p = (Form_pg_proc) GETSTRUCT(tup);
	Oid		   *argtypes;
	char	  **argnames;
	char	   *argmodes;
	int			nargs = get_func_arg_info(tup, &argtypes, &argnames, &argmodes);
	List	   *types = NIL;
	List	   *schemas = list_make1_oid(p->pronamespace);
	int			i;

	for (i = 0; i < nargs; i++)
	{
		Oid			t = argtypes[i];

		while (OidIsValid(t) && !list_member_oid(types, t))
		{
			HeapTuple	tt = SearchSysCache1(TYPEOID, ObjectIdGetDatum(t));
			Form_pg_type typ;

			types = lappend_oid(types, t);
			if (!HeapTupleIsValid(tt))
				break;
			typ = (Form_pg_type) GETSTRUCT(tt);
			schemas = list_append_unique_oid(schemas, typ->typnamespace);
			t = IsTrueArrayType(typ) ? typ->typelem : InvalidOid;
			ReleaseSysCache(tt);
		}
	}
	o->ntypes = list_length(types);
	o->types = MemoryContextAlloc(owned_cxt, sizeof(Oid) * Max(1, o->ntypes));
	i = 0;
	foreach_oid(t, types)
		o->types[i++] = t;
	o->nschemas = list_length(schemas);
	o->schemas = MemoryContextAlloc(owned_cxt, sizeof(Oid) * o->nschemas);
	i = 0;
	foreach_oid(s, schemas)
		o->schemas[i++] = s;
}

/* The same of o (known before) into c, copied into owned_cxt */
static void
copy_name_parts(OwnedObject *c, const OwnedObject *o)
{
	c->ntypes = o->ntypes;
	c->types = MemoryContextAlloc(owned_cxt, sizeof(Oid) * Max(1, o->ntypes));
	memcpy(c->types, o->types, sizeof(Oid) * o->ntypes);
	c->nschemas = o->nschemas;
	c->schemas = MemoryContextAlloc(owned_cxt, sizeof(Oid) * Max(1, o->nschemas));
	memcpy(c->schemas, o->schemas, sizeof(Oid) * o->nschemas);
}

/*
 * The branch's routines (proc) or types (else), as they are now, from
 * those known before (was): each row unchanged since, with no part of its
 * name invalidated since, taken as it was.
 */
static List *
owned_rows(bool proc, List *was)
{
	Oid			catalog = proc ? ProcedureRelationId : TypeRelationId;
	HTAB	   *known = NULL;
	List	   *out = NIL;
	Relation	rel;
	SysScanDesc scan;
	HeapTuple	tup;

	if (was != NIL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(OwnedKnown);
		ctl.hcxt = CurrentMemoryContext;
		known = hash_create("prolly owned objects", list_length(was), &ctl,
							HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		foreach_ptr(OwnedObject, o, was)
		{
			OwnedKnown *e = hash_search(known, &o->oid, HASH_ENTER, NULL);

			e->o = o;
		}
	}
	rel = table_open(catalog, AccessShareLock);
	scan = catalog_branch_beginscan(rel, MyBranch);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		Oid			oid;
		Oid			nsp;
		Oid			owner;
		OwnedObject *o;
		OwnedKnown *e;

		if (proc)
		{
			Form_pg_proc p = (Form_pg_proc) GETSTRUCT(tup);

			oid = p->oid;
			nsp = p->pronamespace;
			owner = p->proowner;
		}
		else
		{
			Form_pg_type t = (Form_pg_type) GETSTRUCT(tup);

			/* (a relation's row type, or an array, is its relation's or element's) */
			if (OidIsValid(t->typrelid) || IsTrueArrayType(t))
				continue;
			oid = t->oid;
			nsp = t->typnamespace;
			owner = t->typowner;
		}
		if (!canon_namespace(nsp))
			continue;
		e = known ? hash_search(known, &oid, HASH_FIND, NULL) : NULL;
		o = MemoryContextAllocZero(owned_cxt, sizeof(OwnedObject));
		o->oid = oid;
		o->owner = owner;
		o->kind = proc ? 'P' : 'T';
		o->tid = tup->t_self;
		o->xmin = HeapTupleHeaderGetXmin(tup->t_data);
		if (e != NULL && ItemPointerEquals(&e->o->tid, &o->tid) &&
			TransactionIdEquals(e->o->xmin, o->xmin) &&
			!owned_name_changed(e->o))
		{
			o->name = MemoryContextStrdup(owned_cxt, e->o->name);
			copy_name_parts(o, e->o);
		}
		else if (proc)
		{
			o->name = MemoryContextStrdup(owned_cxt,
										  routine_item_name((Form_pg_proc) GETSTRUCT(tup)));
			routine_name_parts(o, tup);
		}
		else
		{
			o->name = MemoryContextStrdup(owned_cxt,
										  qualified(nsp, NameStr(((Form_pg_type) GETSTRUCT(tup))->typname)));
			o->nschemas = 1;
			o->schemas = MemoryContextAlloc(owned_cxt, sizeof(Oid));
			o->schemas[0] = nsp;
		}
		{
			MemoryContext old = MemoryContextSwitchTo(owned_cxt);

			out = lappend(out, o);
			MemoryContextSwitchTo(old);
		}
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	if (known)
		hash_destroy(known);
	return out;
}

/* Whether two listings list the same objects, owners and names, in order. */
static bool
same_owned(List *a, List *b)
{
	ListCell   *la;
	ListCell   *lb;

	if (list_length(a) != list_length(b))
		return false;
	forboth(la, a, lb, b)
	{
		OwnedObject *x = lfirst(la);
		OwnedObject *y = lfirst(lb);

		if (x->oid != y->oid || x->owner != y->owner || x->kind != y->kind ||
			strcmp(x->name, y->name) != 0)
			return false;
	}
	return true;
}

/*
 * Each object of the session's branch a commit may hold, other than its
 * prolly tables, partitioned tables and materialized views (which the refs
 * list themselves) and its routines and types (prolly_owned_routines()):
 * its views ('v'), sequences ('S'), foreign tables ('f'), versioned
 * schemas ('N') and collations ('C'), each emit()ted with its OID, owner,
 * that kind and its name as the commit's items name it (so a move making
 * it finds its recorded owner).
 */
void
prolly_owned_objects(void (*emit) (Oid oid, Oid owner, char kind,
								   const char *name, void *arg),
					 void *arg)
{
	int			save_nestlevel = NewGUCNestLevel();
	Relation	rel;
	SysScanDesc scan;
	HeapTuple	tup;

	describing_settings();
	rel = table_open(RelationRelationId, AccessShareLock);
	scan = catalog_branch_beginscan(rel, MyBranch);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_class c = (Form_pg_class) GETSTRUCT(tup);
		char		kind;

		if (c->relkind == RELKIND_VIEW)
			kind = 'v';
		else if (c->relkind == RELKIND_SEQUENCE)
			kind = 'S';
		else if (c->relkind == RELKIND_FOREIGN_TABLE)
			kind = 'f';
		else
			continue;
		if (!canon_namespace(c->relnamespace) ||
			in_extension(RelationRelationId, c->oid))
			continue;
		emit(c->oid, c->relowner, kind,
			 qualified(c->relnamespace, NameStr(c->relname)), arg);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	rel = table_open(NamespaceRelationId, AccessShareLock);
	scan = catalog_branch_beginscan(rel, MyBranch);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_namespace ns = (Form_pg_namespace) GETSTRUCT(tup);

		if (canon_namespace(ns->oid))
			emit(ns->oid, ns->nspowner, 'N',
				 quote_identifier(NameStr(ns->nspname)), arg);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	rel = table_open(CollationRelationId, AccessShareLock);
	scan = catalog_branch_beginscan(rel, MyBranch);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_collation coll = (Form_pg_collation) GETSTRUCT(tup);

		if (coll->oid < FirstNormalObjectId || !canon_namespace(coll->collnamespace) ||
			in_extension(CollationRelationId, coll->oid))
			continue;
		emit(coll->oid, coll->collowner, 'C',
			 qualified(coll->collnamespace, NameStr(coll->collname)), arg);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	AtEOXact_GUC(true, save_nestlevel);
}

bool
prolly_owned_routines(uint64 *generation,
					  void (*emit) (Oid oid, Oid owner, char kind,
									const char *name, void *arg),
					  void *arg)
{
	int			save_nestlevel = NewGUCNestLevel();

	describing_settings();
	if (!owned_callbacks)
	{
		owned_cxt = AllocSetContextCreate(TopMemoryContext, "prolly owned objects",
										  ALLOCSET_DEFAULT_SIZES);
		CacheRegisterSyscacheCallback(PROCOID, owned_forget, (Datum) 0);
		CacheRegisterSyscacheCallback(TYPEOID, owned_forget, (Datum) 0);
		CacheRegisterSyscacheCallback(NAMESPACEOID, owned_forget, (Datum) 0);
		owned_callbacks = true;
	}
	AcceptInvalidationMessages();
	if (owned_branch != MyBranch || !owned_names_known)
	{
		MemoryContextReset(owned_cxt);
		owned_routines = owned_types = NIL;
		owned_routines_known = owned_types_known = false;
		owned_nchanged_types = owned_nchanged_schemas = 0;
		owned_branch = MyBranch;
		owned_names_known = true;
		owned_generation++;
	}

	/*
	 * (each list made whole before it is kept: an error leaves it unknown;
	 * an invalidation while it is read, read again next time, with the
	 * hash values it brought kept for then)
	 */
	if (!owned_routines_known || !owned_types_known)
	{
		MemoryContext was_cxt = owned_cxt;
		List	   *was_routines = owned_routines;
		List	   *was_types = owned_types;
		List	   *routines = NIL;
		List	   *types = NIL;
		bool		read_types = !owned_types_known;
		int			ntypes = owned_nchanged_types;
		int			nschemas = owned_nchanged_schemas;

		owned_routines_known = owned_types_known = true;
		owned_cxt = AllocSetContextCreate(TopMemoryContext, "prolly owned objects",
										  ALLOCSET_DEFAULT_SIZES);
		PG_TRY();
		{
			if (read_types)
				types = owned_rows(false, was_types);
			else
			{
				/* (the types kept, in the new context) */
				foreach_ptr(OwnedObject, o, was_types)
				{
					OwnedObject *c = MemoryContextAlloc(owned_cxt, sizeof(OwnedObject));
					MemoryContext old;

					*c = *o;
					c->name = MemoryContextStrdup(owned_cxt, o->name);
					copy_name_parts(c, o);
					old = MemoryContextSwitchTo(owned_cxt);
					types = lappend(types, c);
					MemoryContextSwitchTo(old);
				}
			}
			routines = owned_rows(true, was_routines);
		}
		PG_CATCH();
		{
			MemoryContextDelete(owned_cxt);
			owned_cxt = was_cxt;
			owned_routines = owned_types = NIL;
			owned_routines_known = owned_types_known = owned_names_known = false;
			PG_RE_THROW();
		}
		PG_END_TRY();
		if (!same_owned(routines, was_routines) || !same_owned(types, was_types))
			owned_generation++;
		owned_routines = routines;
		owned_types = types;
		MemoryContextDelete(was_cxt);
		/* (the hash values read with, gone; any come since, kept) */
		if (owned_names_known)
		{
			memmove(owned_changed_types, owned_changed_types + ntypes,
					sizeof(uint32) * (owned_nchanged_types - ntypes));
			owned_nchanged_types -= ntypes;
			memmove(owned_changed_schemas, owned_changed_schemas + nschemas,
					sizeof(uint32) * (owned_nchanged_schemas - nschemas));
			owned_nchanged_schemas -= nschemas;
		}
	}
	if (*generation != 0 && *generation == owned_generation)
	{
		AtEOXact_GUC(true, save_nestlevel);
		return false;
	}
	foreach_ptr(OwnedObject, o, owned_routines)
		emit(o->oid, o->owner, o->kind, o->name, arg);
	foreach_ptr(OwnedObject, o, owned_types)
		emit(o->oid, o->owner, o->kind, o->name, arg);
	*generation = owned_generation;
	AtEOXact_GUC(true, save_nestlevel);
	return true;
}

/*
 * The primary key's columns of a table's description (its PRIMARY KEY
 * constraint's index statement) into t, and its other indexes: NULL, or
 * why the rows can't be read here in the order the version's tree has
 * them.  An index that can't be followed is listed, with why.
 */
static const char *
historical_key(Canon *cn, const ProllyNodeRef *root, const char *name,
			   const TableDef *td, List *colnames, ProllyHistoricalTable *t)
{
	char	   *pkname = NULL;
	IndexStmt  *pk = NULL;
	List	   *others = NIL;
	ListCell   *lc;
	ProllyComponents c;
	List	   *stale;
	const char *why;

	foreach(lc, td->constraints)
	{
		const char *line = lfirst(lc);
		const char *def = strchr(line, '\t');

		/* (as folded_table_columns() has it: one that keys the rows) */
		if (def && strncmp(def + 1, "PRIMARY KEY", 11) == 0 &&
			strstr(def, "DEFERRABLE") == NULL &&
			strstr(def, "WITHOUT OVERLAPS") == NULL)
			pkname = unquoted(first_field(line));
	}
	t->keyed = (pkname != NULL);
	foreach(lc, td->indexes)
	{
		List	   *parsed = raw_parser(lfirst(lc), RAW_PARSE_DEFAULT);
		Node	   *n;

		if (list_length(parsed) != 1)
			continue;
		n = linitial_node(RawStmt, parsed)->stmt;
		if (!IsA(n, IndexStmt) || ((IndexStmt *) n)->idxname == NULL)
			continue;
		if (pkname && strcmp(((IndexStmt *) n)->idxname, pkname) == 0)
			pk = (IndexStmt *) n;
		else
			others = lappend(others, list_make2(n, lfirst(lc)));
	}
	prolly_history_components(&c);
	stale = stale_collations(cn, root);
	if (t->keyed)
	{
		if (pk == NULL)
			return "its primary key's index isn't one a seek can follow";
		why = index_columns("its primary key", &c, stale, pk, colnames, t->desc, &t->nkeys,
							t->key_attnos, t->opfamilies, t->collations,
							t->options, NULL, NULL);
		if (why)
			return why;
	}

	/* the other indexes, each usable or not on its own */
	foreach(lc, others)
	{
		IndexStmt  *stmt = linitial(lfirst(lc));
		ProllyHistoricalIndex *ix = palloc0(sizeof(ProllyHistoricalIndex));

		ix->name = pstrdup(stmt->idxname);
		ix->definition = pstrdup(lsecond(lfirst(lc)));
		ix->unique = stmt->unique;
		ix->unusable = index_columns("the index", &c, stale, stmt, colnames,
									 t->desc,
									 &ix->nkeys, ix->attnos, ix->opfamilies,
									 ix->collations, ix->options,
									 &ix->ninclude, ix->include);
		t->indexes = lappend(t->indexes, ix);
	}

	/* an ordering an extension recorded, changed since: refused, not re-sorted */
	if (t->keyed)
	{
		StringInfoData value;
		char	   *keyname = psprintf("%s%s", ORDERING_PREFIX, name);
		ProllyKey	key = {(const uint8 *) keyname, strlen(keyname)};

		initStringInfo(&value);
		if (PROLLY_CALL(cn->tree, get, &cn->env, root, &key, &value))
		{
			char	   *recorded = pg_any_to_server(value.data, value.len, PG_UTF8);
			char	   *versions = strchr(recorded, '\n');

			if (versions == NULL)
				elog(ERROR, "invalid historical ordering metadata");
			if (!ordering_current(versions + 1))
				return "an extension ordering its primary key changed since";
		}
	}
	return NULL;
}

static const char *
folded_table_metadata(const ProllyNodeRef *root, const char *name,
					  ProllyHistoricalTable *t, bool *found)
{
	Canon		cn;
	char	   *utf8 = pg_server_to_any(name, strlen(name), PG_UTF8);
	char	   *keyname = psprintf("t:%s", utf8);
	ProllyKey	key = {(const uint8 *) keyname, strlen(keyname)};
	StringInfoData item;
	ProllyValue v;
	ProllyValue description;
	TableDef	td;
	List	   *colnames = NIL;
	ListCell   *lc;
	int			i = 0;
	int			nestlevel;
	const char *why = NULL;

	memset(&cn, 0, sizeof(cn));
	canon_env(&cn);
	memset(t, 0, sizeof(*t));
	initStringInfo(&item);
	*found = PROLLY_CALL(cn.tree, get, &cn.env, root, &key, &item);
	if (!*found)
		return NULL;
	if (!root_in_format(&cn, root))
		return PROLLY_UNREADABLE_OLDER_FORMAT;
	v.data = (const uint8 *) item.data;
	v.len = item.len;
	(void) tree_item(&key, &v, &t->data, &description);
	parse_table_def(from_utf8(description.data, description.len), &td);

	/* (names as the description writes them: qualified, under pg_catalog) */
	nestlevel = NewGUCNestLevel();
	describing_settings();
	PG_TRY();
	{
		t->desc = CreateTemplateTupleDesc(list_length(td.columns));
		foreach(lc, td.columns)
		{
			const char *line = lfirst(lc);
			const char *type = strchr(line, '\t');
			const char *collate = strstr(line, "\tcollate ");
			ErrorSaveContext escontext = {T_ErrorSaveContext};
			char	   *colname = unquoted(first_field(line));
			Oid			typid;
			int32		typmod;
			Oid			coll;

			if (type == NULL ||
				!parseTypeString(first_field(type + 1), &typid, &typmod,
								 (Node *) &escontext) || !OidIsValid(typid))
			{
				why = "one of its columns' types isn't one this database has";
				break;
			}
			TupleDescInitEntry(t->desc, ++i, colname, typid, typmod, 0);
			coll = get_typcollation(typid);
			if (collate)
			{
				coll = get_collation_oid(stringToQualifiedNameList(first_field(collate + 9),
																   NULL), true);
				if (!OidIsValid(coll))
				{
					why = "one of its columns' collations isn't one this database has";
					break;
				}
			}
			TupleDescInitEntryCollation(t->desc, i, coll);
			colnames = lappend(colnames, colname);
		}
		if (why == NULL)
		{
			TupleDescFinalize(t->desc);
			why = historical_key(&cn, root, utf8, &td, colnames, t);
		}
	}
	PG_FINALLY();
	{
		AtEOXact_GUC(false, nestlevel);
	}
	PG_END_TRY();
	return why;
}

static List *
folded_table_names(const ProllyNodeRef *root, const char *schema)
{
	Canon		cn;
	ProllyTreeCursor *c;
	ProllyKey	k;
	ProllyValue v;
	ProllyKey	from = {(const uint8 *) "t:", 2};
	List	   *names = NIL;

	memset(&cn, 0, sizeof(cn));
	canon_env(&cn);
	c = PROLLY_CALL(cn.tree, cursor_open, &cn.env, root, CurrentMemoryContext);
	PROLLY_CALL(cn.tree, cursor_seek, c, &from, ForwardScanDirection);
	while (PROLLY_CALL(cn.tree, cursor_next, c, &k, &v))
	{
		char	   *name;
		List	   *parts;

		if (k.len < 2 || memcmp(k.data, "t:", 2) != 0)
			break;
		name = pg_any_to_server(pnstrdup((const char *) k.data + 2, k.len - 2),
								k.len - 2, PG_UTF8);
		parts = stringToQualifiedNameList(name, NULL);
		if (list_length(parts) == 2 && strcmp(strVal(linitial(parts)), schema) == 0)
			names = lappend(names, name);
	}
	PROLLY_CALL(cn.tree, cursor_close, c);
	return names;
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
	.read_definitions = folded_read_definitions,
	.table_changes = folded_table_changes,
	.table_fewer_changes = folded_table_fewer_changes,
	.discard_undescribed = folded_discard_undescribed,
	.fold_rows = folded_fold_rows,
	.advance_baseline = folded_advance_baseline,
	.advance_own = folded_advance_own,
	.splice = folded_splice,
	.splice_baseline = folded_splice_baseline,
	.tables = folded_tables,
	.materialized_views = folded_materialized_views,
	.same_schema = folded_same_schema,
	.objects = folded_objects,
	.changes = folded_changes,
	.render = folded_render,
	.move_schema = folded_move_schema,
	.make_versioned_schemas = folded_make_versioned_schemas,
	.pending_full = folded_pending_full,
	.sequences = folded_sequences,
	.fold_dependencies = folded_fold_dependencies,
	.dependencies = folded_dependencies,
	.merge_schema = folded_merge_schema,
	.walk_baseline = folded_walk_baseline,
	.table_columns = folded_table_columns,
	.table_parents = folded_table_parents,
	.type_definitions = folded_type_definitions,
	.in_format = folded_in_format,
	.format = folded_format,
	.identify = folded_identify,
	.renames = folded_renames,
	.merge_renames = folded_merge_renames,
	.rename_description = folded_rename_description,
	.rename_root = folded_rename_root,
	.rename_objects = folded_rename_objects,
	.table_metadata = folded_table_metadata,
	.table_names = folded_table_names,
	.objects_diffed = folded_objects_diffed,
	.sequences_now = folded_sequences_now,
};
