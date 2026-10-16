/*-------------------------------------------------------------------------
 *
 * diffmerge_folded.c
 *	  Diff and merge implementation "folded": versions of a database whose
 *	  branches are copies of its versioned schemas, compared as their
 *	  folded canonical trees.
 *
 * A commit's root is the canonical tree its branch folded to; WORKING (and
 * STAGED: nothing is staged) is the session branch folded now.  Each table
 * in the canonical tree names its data tree, and two versions of a table
 * are compared by diffing those trees, in the order of the table's keys;
 * subtrees the same on both sides aren't read.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/diffmerge_folded.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"
#include "access/prolly_canon.h"
#include "access/prolly_chunkstore.h"
#include "access/prolly_config.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/varlena.h"

/* A table as one version has it. */
typedef struct Table
{
	char	   *name;			/* qualified, in the server's encoding */
	char	   *utf8;			/* the same in UTF-8, as the canonical tree
								 * orders them */
	ProllyNodeRef data;
	ProllyHash	description;	/* of its definition */
} Table;

typedef struct Tables
{
	List	   *tables;			/* Table *, in name order */
} Tables;

static ProllyDatabaseComponents
components(void)
{
	ProllyDatabaseComponents dc;

	prolly_database_components(&dc);
	return dc;
}

static void
chunk_env(ProllyTreeEnv *env)
{
	memset(env, 0, sizeof(*env));
	env->chunk_format = prolly_chunkformat_lookup("basic");
	env->chunker = prolly_chunker_lookup("bytes");
	env->hash = prolly_hash_lookup("sha256");
	env->chunk_store = prolly_chunk_store();
	env->chunks = PROLLY_CALL(env->chunk_store, open, MyDatabaseId,
							  MyDatabaseTableSpace);
}

/* The canonical root of a ref: a commit's, or WORKING folded now. */
static void
root_of(const char *ref, ProllyNodeRef *root)
{
	ProllyDatabaseComponents dc = components();
	ProllyHash	id;
	ProllyCommitInfo info;

	if (strcmp(ref, "WORKING") == 0 || strcmp(ref, "STAGED") == 0)
	{
		PROLLY_CALL(dc.canonicalizer, fold, root);
		return;
	}
	if (!PROLLY_CALL(dc.refs, resolve, ref, &id) ||
		!PROLLY_CALL(dc.refs, commit_get, &id, &info))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("prolly ref \"%s\" does not exist", ref)));
	*root = info.root;
}

static void
add_table(const char *name, const ProllyNodeRef *data,
		  const ProllyValue *description, void *arg)
{
	Tables	   *ts = arg;
	Table	   *t = palloc(sizeof(Table));
	const ProllyHashRoutine *h = prolly_hash_lookup("sha256");
	ProllyHashCtx *ctx = PROLLY_CALL(h, begin, CurrentMemoryContext);

	/* (the canonical tree's names are UTF-8; the catalogs' the server's) */
	t->name = pstrdup(pg_any_to_server(name, strlen(name), PG_UTF8));
	t->utf8 = pstrdup(name);
	t->data = *data;
	PROLLY_CALL(h, update, ctx, description->data, description->len);
	PROLLY_CALL(h, finish, ctx, &t->description);
	ts->tables = lappend(ts->tables, t);
}

static List *
tables_of(const char *ref)
{
	ProllyNodeRef root;
	Tables		ts = {NIL};

	root_of(ref, &root);
	PROLLY_CALL(components().canonicalizer, tables, &root, add_table, &ts);
	return ts.tables;
}

static bool
same_hash(const ProllyHash *a, const ProllyHash *b)
{
	return a->len == b->len && memcmp(a->bytes, b->bytes, a->len) == 0;
}

static Table *
find_table(List *tables, const char *name)
{
	ListCell   *lc;

	foreach(lc, tables)
	{
		Table	   *t = lfirst(lc);

		if (strcmp(t->name, name) == 0)
			return t;
	}
	return NULL;
}

/* ------------------------------------------------------------------------
 * Rows
 * ------------------------------------------------------------------------
 */

typedef struct RowDiff
{
	const ProllyKeyingRoutine *kr;
	ProllyKeying *k;
	Oid			relid;
	ProllyDiffSink *sink;
} RowDiff;

static int
keying_compare(const ProllyKey *a, const ProllyKey *b, void *arg)
{
	RowDiff    *rd = arg;

	return PROLLY_CALL(rd->kr, compare, rd->k, a, b);
}

static void
row_change(ProllyDiffKind kind, const ProllyKey *key,
		   const ProllyValue *before, const ProllyValue *after, void *arg)
{
	RowDiff    *rd = arg;

	rd->sink->change(rd->sink, rd->relid, kind, key, before, after);
}

/*
 * The rows of table relid that differ between two versions of it (NULL:
 * the version has no such table), in key order.
 */
static void
diff_rows(Oid relid, const Table *from, const Table *to, ProllyDiffSink *sink)
{
	const ProllyTreeRoutine *tree = prolly_tree_lookup("cow");
	ProllyTreeEnv env;
	RowDiff		rd;
	Relation	rel;
	ProllyNodeRef empty;
	ProllyTreeBuilder *b;

	if (from && to && same_hash(&from->data.hash, &to->data.hash))
		return;
	rel = table_open(relid, AccessShareLock);
	rd.kr = (const ProllyKeyingRoutine *)
		prolly_get_impl(PROLLY_IFACE_TABLE_KEYING, "pk_or_content");
	rd.k = PROLLY_CALL(rd.kr, open_rewritten, rel, CurrentMemoryContext);
	rd.relid = relid;
	rd.sink = sink;
	chunk_env(&env);
	env.compare = keying_compare;
	env.compare_arg = &rd;
	b = PROLLY_CALL(tree, build_begin, &env, CurrentMemoryContext);
	PROLLY_CALL(tree, build_finish, b, &empty);
	PROLLY_CALL(tree, diff, &env, from ? &from->data : &empty,
				to ? &to->data : &empty, row_change, &rd);
	PROLLY_CALL(rd.kr, close, rd.k);
	table_close(rel, AccessShareLock);
}

/* A versioned table of the session branch's, by qualified name. */
static Oid
table_named(const char *name)
{
	List	   *names = stringToQualifiedNameList(name, NULL);

	return RangeVarGetRelid(makeRangeVarFromNameList(names), NoLock, true);
}

static char *
table_name(Oid relid)
{
	return quote_qualified_identifier(get_namespace_name(get_rel_namespace(relid)),
									  get_rel_name(relid));
}

/* ------------------------------------------------------------------------
 * Callbacks
 * ------------------------------------------------------------------------
 */

static int
name_cmp(const ListCell *a, const ListCell *b)
{
	return strcmp(lfirst(a), lfirst(b));
}

/*
 * Every table the session branch has: tables only in other versions can't
 * be read as rows here (tables() names them).
 */
static void
folded_diff(const char *from_ref, const char *to_ref, Oid relid,
			ProllyDiffSink *sink)
{
	List	   *from = tables_of(from_ref);
	List	   *to = tables_of(to_ref);
	List	   *names = NIL;
	ListCell   *lc;

	if (OidIsValid(relid))
	{
		char	   *name = table_name(relid);

		diff_rows(relid, find_table(from, name), find_table(to, name), sink);
		return;
	}
	foreach(lc, from)
		names = lappend(names, ((Table *) lfirst(lc))->name);
	foreach(lc, to)
	{
		if (!find_table(from, ((Table *) lfirst(lc))->name))
			names = lappend(names, ((Table *) lfirst(lc))->name);
	}
	list_sort(names, name_cmp);
	foreach(lc, names)
	{
		char	   *name = lfirst(lc);
		Oid			tid = table_named(name);

		if (OidIsValid(tid))
			diff_rows(tid, find_table(from, name), find_table(to, name), sink);
	}
}

static void
folded_tables(const char *from_ref, const char *to_ref,
			  ProllyTableChangeVisit visit, void *arg)
{
	List	   *from = tables_of(from_ref);
	List	   *to = tables_of(to_ref);
	ListCell   *a = list_head(from);
	ListCell   *b = list_head(to);

	/* both in the canonical tree's order (UTF-8 bytes): a merge */
	while (a || b)
	{
		Table	   *ta = a ? lfirst(a) : NULL;
		Table	   *tb = b ? lfirst(b) : NULL;
		int			c = !ta ? 1 : !tb ? -1 : strcmp(ta->utf8, tb->utf8);

		if (c < 0)
		{
			visit(ta->name, PROLLY_DIFF_REMOVED, arg);
			a = lnext(from, a);
		}
		else if (c > 0)
		{
			visit(tb->name, PROLLY_DIFF_ADDED, arg);
			b = lnext(to, b);
		}
		else
		{
			if (!same_hash(&ta->data.hash, &tb->data.hash) ||
				!same_hash(&ta->description, &tb->description))
				visit(ta->name, PROLLY_DIFF_MODIFIED, arg);
			a = lnext(from, a);
			b = lnext(to, b);
		}
	}
}

const ProllyMergeRoutine prolly_merge_folded = {
	.info = {
		.iface = PROLLY_IFACE_DIFF_MERGE,
		.impl_id = 3,
		.format_version = 1,
		.name = "folded",
		.description = "versions compared as their folded canonical trees",
	},
	.diff = folded_diff,
	.tables = folded_tables,
};
