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
 * Moving the session branch to a root (a reset, a fast-forward) writes
 * the rows that differ between it folded now and the root, through the
 * row applier: every row leaving (removed, or changed) is removed first,
 * then every row arriving inserted, so the rows are always a subset of
 * the root's or of the branch's, and a unique constraint both satisfy
 * isn't violated on the way, as it could be by changing rows one at a
 * time (two rows swapping a value).  Where no unique or exclusion index
 * but the primary key's could be, a keyed row that changed is updated in
 * place instead, its values out of line that didn't change (the same
 * reference on both sides) kept as they are stored: TOAST keeps their
 * pointers, and they are neither read nor stored again.  Definitions that differ are changed
 * first, by the canonicalizer.  Sequences are set to the root's values.
 * A merge gives each sequence the other side changed the larger of the
 * two values.
 *
 * Merge is three-way, from the best common ancestor of the session's
 * head and the other one, as in implementation "rows": for each table,
 * the other side's changes since the ancestor are written to the branch
 * (with the row applier) unless the branch changed the same key to
 * another row since, a conflict, which keeps the branch's row and leaves
 * the merge in progress in the refs.  Its conflicts are found again from
 * the three commits when they're listed or resolved.  A keyless table's
 * rows have no identity: a row's copies merge as merge_table() says,
 * without conflicts.  The branch may have uncommitted changes, as in Dolt,
 * if the merge changes none of the tables and sequences they changed, and
 * no definitions change: they stay uncommitted, left out of the merge's
 * commit.  Definitions changed since the ancestor are merged first (the
 * canonicalizer's merge_schema(): an object both sides changed differently
 * fails the merge); then a table all three define alike merges its rows
 * three-way, and one defined otherwise takes the rows of the side that
 * redefined, made or kept it, if the other side didn't change them.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/diffmerge_folded.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"
#include "access/prolly_applier.h"
#include "access/prolly_canon.h"
#include "access/prolly_chunkstore.h"
#include "access/prolly_config.h"
#include "access/prolly_deps.h"
#include "access/prolly_xact.h"
#include "access/table.h"
#include "nodes/makefuncs.h"
#include "parser/parse_collate.h"
#include "parser/parse_type.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "catalog/versioned.h"
#include "access/htup_details.h"
#include "access/genam.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/guc.h"
#include "utils/json.h"
#include "utils/array.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "catalog/indexing.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_index.h"
#include "access/xact.h"
#include "catalog/branch_ops.h"
#include "catalog/namespace.h"
#include "catalog/partition.h"
#include "catalog/pg_sequence.h"
#include "commands/sequence.h"
#include "common/hashfn.h"
#include "common/int.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "parser/parse_coerce.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/hsearch.h"
#include "utils/injection_point.h"
#include "utils/lsyscache.h"
#include "rewrite/rewriteHandler.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/varlena.h"

/* A table as one version has it. */
typedef struct Table
{
	char	   *name;			/* qualified, in the server's encoding */
	char	   *utf8;			/* the same in UTF-8, as the canonical tree
								 * orders them */
	ProllyNodeRef data;
	char	   *text;			/* the definition, as described */
	int			textlen;
	char	   *unreadable;		/* why its rows can't be read here, or NULL */
} Table;

typedef struct Tables
{
	List	   *tables;			/* Table *, in name order */
} Tables;

static int	merge_roots(const ProllyNodeRef *roots, bool fail_on_conflict,
						const char *what, const char *name, List **conflicted);
static bool same_description(const Table *a, const Table *b);
static Table *find_table(List *tables, const char *name);
static char *table_name(Oid relid);
static int	keep_violations(List *found);
static List *foreign_key_violations(List *relids, List *changed);
static void violating_rows(List *queries, const char *type, bool removing,
						   List **found);
static bool changed_rows_of(List *changed, Oid relid, List **now, List **was);
static bool primary_key_columns(Oid relid, const char *alias, StringInfo cols,
								StringInfo defs);
static void key_columns(Oid relid, const int16 *attnums, int n, const char *alias,
						StringInfo cols, StringInfo defs);
static void key_queries(List *qi, List *keys, const char *cols, const char *defs,
						List **queries);
static List *distinct_violations(List *found);
static void take_breaking_rows(List *added, void *arg);
static void diff_rows_now(Oid relid, const Table *from, const Table *to,
						  const Table *now, ProllyDiffSink *sink);

static ProllyDatabaseComponents
components(void)
{
	ProllyDatabaseComponents dc;

	prolly_database_components(&dc);
	return dc;
}

/*
 * The folded trees' environment, without an order: a diff of rows sets the
 * keying's, and callers that walk canonical roots set bytes' themselves.
 */
static void
chunk_env(ProllyTreeEnv *env)
{
	prolly_folded_tree_env(env);
	env->compare = NULL;
}

/* How rel's rows are keyed in history: its access method's keying. */
static const ProllyKeyingRoutine *
keying_of(Relation rel)
{
	ProllyComponents c;

	prolly_components_for_relation(rel, &c);
	return c.keying;
}

static bool
is_working(const char *ref)
{
	return strcmp(ref, "WORKING") == 0 || strcmp(ref, "STAGED") == 0;
}

/* The canonical root of a ref: a commit's, or WORKING folded now. */
static void
root_of(const char *ref, ProllyNodeRef *root)
{
	ProllyDatabaseComponents dc = components();
	ProllyHash	id;
	ProllyCommitInfo info;

	if (is_working(ref))
	{
		(void) PROLLY_CALL(dc.refs, working_set, NULL, root);
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
		  const ProllyValue *description, const char *unreadable, void *arg)
{
	Tables	   *ts = arg;
	Table	   *t = palloc(sizeof(Table));

	/* (the canonical tree's names are UTF-8; the catalogs' the server's) */
	t->name = pstrdup(pg_any_to_server(name, strlen(name), PG_UTF8));
	t->utf8 = pstrdup(name);
	t->unreadable = unreadable ?
		pstrdup(pg_any_to_server(unreadable, strlen(unreadable), PG_UTF8)) : NULL;
	t->data = *data;
	t->text = palloc(description->len + 1);
	memcpy(t->text, description->data, description->len);
	t->text[description->len] = '\0';
	t->textlen = description->len;
	ts->tables = lappend(ts->tables, t);
}

/*
 * A ref's tables, to read: WORKING's read (objects a commit would refuse
 * left out, and their names appended to *left_out if it isn't NULL); with
 * relid valid, relid's table alone (the others' rows aren't read).
 */
static int
string_cmp(const ListCell *a, const ListCell *b)
{
	return strcmp(lfirst(a), lfirst(b));
}

static List *
tables_of_reading(const char *ref, Oid relid, List **left_out,
				  ProllyNodeRef *root_out)
{
	ProllyNodeRef root;
	Tables		ts = {NIL};

	if (is_working(ref))
		(void) PROLLY_CALL(components().refs, read_working_set, NULL, relid,
						   &root, left_out);
	else
		root_of(ref, &root);
	if (root_out)
		*root_out = root;
	PROLLY_CALL(components().canonicalizer, tables, &root, add_table, &ts);
	if (OidIsValid(relid))
	{
		Table	   *t = find_table(ts.tables, table_name(relid));

		return t ? list_make1(t) : NIL;
	}
	return ts.tables;
}

static List *
tables_of(const char *ref, Oid relid)
{
	return tables_of_reading(ref, relid, NULL, NULL);
}

static bool
same_hash(const ProllyHash *a, const ProllyHash *b)
{
	return a->len == b->len && memcmp(a->bytes, b->bytes, a->len) == 0;
}

/* The same definition, as described. */
static bool
same_description(const Table *a, const Table *b)
{
	return a->textlen == b->textlen && memcmp(a->text, b->text, a->textlen) == 0;
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

/* Refuse to read the rows of a version of a table that can't be read here. */
static void
require_readable(const Table *t)
{
	if (t && t->unreadable)
		ereport(ERROR,
				(errcode(ERRCODE_COLLATION_MISMATCH),
				 errmsg("the rows of table %s in this version can't be read here", t->name),
				 errdetail("The %s.", t->unreadable),
				 errhint("A version's rows can only be read where the collations ordering their keys are as it recorded.")));
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
	ProllyKeyingRefs krefs;
	RowDiff		rd;
	Relation	rel;
	ProllyNodeRef empty;
	ProllyTreeBuilder *b;

	if (from && to && same_hash(&from->data.hash, &to->data.hash))
		return;
	require_readable(from);
	require_readable(to);
	rel = table_open(relid, AccessShareLock);
	rd.kr = keying_of(rel);
	rd.k = PROLLY_CALL(rd.kr, open, rel, CurrentMemoryContext);
	rd.relid = relid;
	rd.sink = sink;
	chunk_env(&env);
	env.compare = keying_compare;
	env.compare_arg = &rd;
	prolly_keying_use_env(&krefs, rd.kr, rd.k, tree, &env);
	b = PROLLY_CALL(tree, build_begin, &env, CurrentMemoryContext);
	PROLLY_CALL(tree, build_finish, b, &empty);
	PROLLY_CALL(tree, diff, &env, from ? &from->data : &empty,
				to ? &to->data : &empty, row_change, &rd);
	PROLLY_CALL(rd.kr, close, rd.k);
	table_close(rel, AccessShareLock);
}

/* A versioned table of the session branch's, by qualified name. */
static Oid
rel_named(const char *name, bool missing_ok)
{
	RangeVar   *rv = makeRangeVarFromNameList(stringToQualifiedNameList(name, NULL));
	Oid			nsp;
	Oid			relid;

	if (rv->catalogname != NULL &&
		strcmp(rv->catalogname, get_database_name(MyDatabaseId)) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cross-database references are not implemented: \"%s\"",
						name)));
	if (rv->schemaname == NULL)
		return RangeVarGetRelid(rv, NoLock, missing_ok);

	/*
	 * (a version's table, named with its schema: found whatever the
	 * session may use, as the fold that recorded it read every versioned
	 * schema)
	 */
	nsp = get_namespace_oid(rv->schemaname, missing_ok);
	relid = OidIsValid(nsp) ? get_relname_relid(rv->relname, nsp) : InvalidOid;
	if (!OidIsValid(relid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("relation \"%s\" does not exist", name)));
	return relid;
}

static Oid
table_named(const char *name)
{
	return rel_named(name, true);
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

/*
 * Does any table (relid's, if it's valid) differ between versions from and
 * to, in its rows or its definition, or is in one alone?
 */
static bool
tables_differ(List *from, List *to, Oid relid)
{
	char	   *only = OidIsValid(relid) ? table_name(relid) : NULL;
	ListCell   *lc;

	foreach(lc, from)
	{
		Table	   *f = lfirst(lc);
		Table	   *t = find_table(to, f->name);

		if (only && strcmp(only, f->name) != 0)
			continue;
		if (t == NULL || !same_hash(&f->data.hash, &t->data.hash) ||
			!same_description(f, t))
			return true;
	}
	foreach(lc, to)
	{
		Table	   *t = lfirst(lc);

		if ((only == NULL || strcmp(only, t->name) == 0) &&
			find_table(from, t->name) == NULL)
			return true;
	}
	return false;
}

/* A table of either side of a diff, with its versions. */
typedef struct DiffTable
{
	const char *name;
	Table	   *from;
	Table	   *to;
	Table	   *now;
} DiffTable;

static int
diff_table_cmp(const ListCell *a, const ListCell *b)
{
	return strcmp(((DiffTable *) lfirst(a))->name, ((DiffTable *) lfirst(b))->name);
}

/*
 * The tables of from and to, each with its versions in from, to and now:
 * the lists merged (each in the canonical tree's order), then sorted by
 * name.
 */
static List *
diff_tables(List *from, List *to, List *now)
{
	List	   *out = NIL;
	ListCell   *a = list_head(from);
	ListCell   *b = list_head(to);
	ListCell   *n = list_head(now);

	while (a || b)
	{
		Table	   *ta = a ? lfirst(a) : NULL;
		Table	   *tb = b ? lfirst(b) : NULL;
		int			c = !ta ? 1 : !tb ? -1 : strcmp(ta->utf8, tb->utf8);
		DiffTable  *d = palloc0(sizeof(DiffTable));

		d->from = c <= 0 ? ta : NULL;
		d->to = c >= 0 ? tb : NULL;
		d->name = (d->from ? d->from : d->to)->name;
		while (n && strcmp(((Table *) lfirst(n))->utf8,
						   (d->from ? d->from : d->to)->utf8) < 0)
			n = lnext(now, n);
		if (n && strcmp(((Table *) lfirst(n))->utf8,
						(d->from ? d->from : d->to)->utf8) == 0)
			d->now = lfirst(n);
		out = lappend(out, d);
		if (c <= 0)
			a = lnext(from, a);
		if (c >= 0)
			b = lnext(to, b);
	}
	list_sort(out, diff_table_cmp);
	return out;
}

/*
 * Every table the session branch has: tables only in other versions can't
 * be read as rows here (tables() names them).  With relid valid, that
 * table's alone: only its rows are folded.
 */
static void
folded_diff(const char *from_ref, const char *to_ref, Oid relid,
			ProllyDiffSink *sink)
{
	List	   *from = tables_of(from_ref, relid);
	List	   *to;
	List	   *now;
	ListCell   *lc;

	/* (WORKING folded once) */
	to = is_working(to_ref) && is_working(from_ref) ? from :
		tables_of(to_ref, relid);

	/*
	 * Rows are read as the tables are defined now: the working set's
	 * definitions, folded only if a version may define one otherwise.
	 */
	if (is_working(to_ref))
		now = to;
	else if (is_working(from_ref))
		now = from;
	else if (!tables_differ(from, to, relid))
		return;
	else
		now = tables_of("WORKING", relid);

	if (OidIsValid(relid))
	{
		char	   *name = table_name(relid);

		diff_rows_now(relid, find_table(from, name), find_table(to, name),
					  find_table(now, name), sink);
		return;
	}
	foreach(lc, diff_tables(from, to, now))
	{
		DiffTable  *d = lfirst(lc);
		Oid			tid = table_named(d->name);

		if (OidIsValid(tid))
			diff_rows_now(tid, d->from, d->to, d->now, sink);
	}
}

typedef struct ObjectChanges
{
	ProllyTableChangeVisit visit;
	void	   *arg;
	List	   *left_out;		/* reported as not covered instead */
} ObjectChanges;

static void
object_change(const char *name, ProllyDiffKind kind, char object, void *arg)
{
	ObjectChanges *oc = arg;
	ListCell   *lc;

	foreach(lc, oc->left_out)
		if (strcmp(lfirst(lc), name) == 0)
			return;
	oc->visit(name, kind, object, oc->arg);
}

static void
folded_tables(const char *from_ref, const char *to_ref,
			  ProllyTableChangeVisit visit, void *arg)
{
	List	   *left_out = NIL;
	ListCell   *lc;
	ProllyNodeRef from_root;
	ProllyNodeRef to_root;
	List	   *from = tables_of_reading(from_ref, InvalidOid, &left_out,
										 &from_root);
	List	   *to;
	ListCell   *a;
	ListCell   *b;

	if (is_working(to_ref) && is_working(from_ref))
	{
		to = from;
		to_root = from_root;
	}
	else
		to = tables_of_reading(to_ref, InvalidOid, &left_out, &to_root);
	a = list_head(from);
	b = list_head(to);

	/* both in the canonical tree's order (UTF-8 bytes): a merge */
	while (a || b)
	{
		Table	   *ta = a ? lfirst(a) : NULL;
		Table	   *tb = b ? lfirst(b) : NULL;
		int			c = !ta ? 1 : !tb ? -1 : strcmp(ta->utf8, tb->utf8);

		if (c < 0)
		{
			visit(ta->name, PROLLY_DIFF_REMOVED, PROLLY_OBJECT_TABLE, arg);
			a = lnext(from, a);
		}
		else if (c > 0)
		{
			visit(tb->name, PROLLY_DIFF_ADDED, PROLLY_OBJECT_TABLE, arg);
			b = lnext(to, b);
		}
		else
		{
			if (!same_hash(&ta->data.hash, &tb->data.hash) ||
				!same_description(ta, tb))
				visit(ta->name, PROLLY_DIFF_MODIFIED, PROLLY_OBJECT_TABLE, arg);
			a = lnext(from, a);
			b = lnext(to, b);
		}
	}

	/* the other objects, but those the reads left out (named below) */
	if (PROLLY_HAS(components().canonicalizer, objects) &&
		!same_hash(&from_root.hash, &to_root.hash))
	{
		ObjectChanges oc = {visit, arg, left_out};

		PROLLY_CALL(components().canonicalizer, objects, &from_root, &to_root,
					object_change, &oc);
	}

	/* (what the reads left out, once each) */
	list_sort(left_out, string_cmp);
	foreach(lc, left_out)
	{
		if (foreach_current_index(lc) == 0 ||
			strcmp(lfirst(lc), list_nth(left_out, foreach_current_index(lc) - 1)) != 0)
			visit(lfirst(lc), PROLLY_DIFF_NOT_COVERED, PROLLY_OBJECT_UNKNOWN, arg);
	}
}

/* ------------------------------------------------------------------------
 * Moving the branch
 * ------------------------------------------------------------------------
 */

/* One row changed, copied out of the trees. */
typedef struct Change
{
	ProllyDiffKind kind;
	StringInfoData key;
	StringInfoData before;		/* empty: none */
	StringInfoData after;		/* empty: none */
} Change;

typedef struct Changes
{
	RowDiff		rd;				/* (for the keying's order) */
	List	   *list;
} Changes;

static int
changes_compare(const ProllyKey *a, const ProllyKey *b, void *arg)
{
	Changes    *c = arg;

	return PROLLY_CALL(c->rd.kr, compare, c->rd.k, a, b);
}

/*
 * A merge base's rows with a value that's unsure (see ReadFor), and in
 * which columns, in key order: a side that has the row has changed those.
 */
typedef struct UnsureRow
{
	ProllyKey	key;
	Bitmapset  *cols;			/* attribute numbers, from 0 */
} UnsureRow;

typedef struct UnsureRows
{
	int			n;
	UnsureRow **rows;
} UnsureRows;

static int
unsure_row_cmp(const void *a, const void *b, void *arg)
{
	const UnsureRow *x = *(UnsureRow *const *) a;
	const UnsureRow *y = *(UnsureRow *const *) b;

	return keying_compare(&x->key, &y->key, arg);
}

/* A copy of len bytes, sized to them (none: empty). */
static void
copy_bytes(StringInfo s, const uint8 *data, int len)
{
	s->data = palloc(len + 1);
	if (len > 0)
		memcpy(s->data, data, len);
	s->data[len] = '\0';
	s->len = len;
	s->maxlen = len + 1;
	s->cursor = 0;
}

static void
collect_change(ProllyDiffKind kind, const ProllyKey *key,
			   const ProllyValue *before, const ProllyValue *after, void *arg)
{
	Changes    *c = arg;
	Change	   *ch = palloc0(sizeof(Change));

	ch->kind = kind;
	copy_bytes(&ch->key, key->data, key->len);
	copy_bytes(&ch->before, before ? before->data : NULL, before ? before->len : 0);
	copy_bytes(&ch->after, after ? after->data : NULL, after ? after->len : 0);
	c->list = lappend(c->list, ch);
}

/*
 * changes (a side's, from base) with a change made up for each of base's
 * unsure rows it hasn't one for (the side has the row as base has it,
 * which only the cast says), so that a row both sides have is walked as
 * changed on both, its unsure columns merged as changed on both.
 */
static List *
with_unsure_rows(Changes *changes, const UnsureRows *unsure,
				 const ProllyTreeRoutine *tree, ProllyTreeEnv *env,
				 const Table *base)
{
	List	   *out = NIL;
	ListCell   *lc = list_head(changes->list);
	int			i = 0;

	while (lc || i < unsure->n)
	{
		Change	   *ch = lc ? lfirst(lc) : NULL;
		int			r;

		if (ch == NULL)
			r = -1;
		else if (i == unsure->n)
			r = 1;
		else
		{
			ProllyKey	ck = {(const uint8 *) ch->key.data, ch->key.len};

			r = changes_compare(&unsure->rows[i]->key, &ck, changes);
		}
		if (r < 0)
		{
			Change	   *made = palloc0(sizeof(Change));
			StringInfoData row;

			initStringInfo(&row);
			if (!PROLLY_CALL(tree, get, env, &base->data, &unsure->rows[i]->key, &row))
				elog(ERROR, "an unsure row of a merge's base is missing from it");
			made->kind = PROLLY_DIFF_MODIFIED;
			copy_bytes(&made->key, unsure->rows[i]->key.data, unsure->rows[i]->key.len);
			copy_bytes(&made->before, (const uint8 *) row.data, row.len);
			copy_bytes(&made->after, (const uint8 *) row.data, row.len);
			pfree(row.data);
			out = lappend(out, made);
			i++;
			continue;
		}
		if (r == 0)
			i++;
		out = lappend(out, ch);
		lc = lnext(changes->list, lc);
	}
	return out;
}

/* A tree value's row into slot; returns how many copies it stands for. */
static uint64
decode(Changes *c, const StringInfoData *value, TupleTableSlot *slot)
{
	ProllyValue v = {(const uint8 *) value->data, value->len};
	ProllyValue row;
	uint64		count;

	if (value->len == 0)
		return 0;
	PROLLY_CALL(c->rd.kr, parse_value, c->rd.k, &v, &row, &count);
	ExecClearTuple(slot);
	PROLLY_CALL(c->rd.kr, decode_row, c->rd.k, &row, slot);
	return count;
}

/* How many copies a tree value stands for, its row not decoded. */
static uint64
count_of(Changes *c, const StringInfoData *value)
{
	ProllyValue v = {(const uint8 *) value->data, value->len};
	ProllyValue row;
	uint64		count;

	if (value->len == 0)
		return 0;
	PROLLY_CALL(c->rd.kr, parse_value, c->rd.k, &v, &row, &count);
	return count;
}

/*
 * While a merge (or pick) runs: the constraint violations it records, and
 * the tables it wrote to, whose foreign keys are checked after; and the
 * last table whose rows a merge wrote, whether a row written may break a
 * constraint there.
 */
static bool recording = false;
static List *recorded = NIL;	/* ProllyViolation * */
static List *written = NIL;		/* OIDs */
static Oid	breakable_relid = InvalidOid;
static bool breakable;

static void
note_written(Oid relid)
{
	MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);

	written = list_append_unique_oid(written, relid);
	MemoryContextSwitchTo(old);
}

/*
 * Merge base to theirs into ours, recording the violations it leaves (with
 * those the branch had): its rows it didn't write, and those of the tables
 * it wrote that break a foreign key.  *nviolations gets how many it left.
 */
static int
merge_recording(const ProllyNodeRef *roots, bool fail_on_conflict,
				const char *what, const char *name, List **conflicted,
				int *nviolations)
{
	int			conflicts;
	List	   *found = NIL;

	recording = true;
	recorded = NIL;
	written = NIL;
	breakable_relid = InvalidOid;
	PG_TRY();
	{
		conflicts = merge_roots(roots, fail_on_conflict, what, name,
								conflicted);
		recording = false;
		found = list_concat(recorded, foreign_key_violations(written, NIL));
	}
	PG_FINALLY();
	{
		recording = false;
		recorded = NIL;
		written = NIL;
	}
	PG_END_TRY();
	*nviolations = keep_violations(found);
	return conflicts;
}

static char *
quote_json(const char *s)
{
	StringInfoData buf;

	initStringInfo(&buf);
	escape_json(&buf, s);
	return buf.data;
}

static char *
row_json(TupleTableSlot *slot)
{
	Datum		row = ExecFetchSlotHeapTupleDatum(slot);

	return TextDatumGetCString(DirectFunctionCall1(row_to_json, row));
}

static ProllyViolation *
violation(Oid relid, const char *type, char *row, char *info)
{
	ProllyViolation *pv = palloc0(sizeof(ProllyViolation));

	pv->relid = relid;
	pv->type = pstrdup(type);
	pv->row = row;
	pv->info = info;
	return pv;
}

/* Whether rel has a unique or exclusion index other than its primary key's. */
static bool
other_unique_index(Relation rel)
{
	ListCell   *lc;

	foreach(lc, RelationGetIndexList(rel))
	{
		HeapTuple	tup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(lfirst_oid(lc)));
		Form_pg_index index;
		bool		other;

		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for index %u", lfirst_oid(lc));
		index = (Form_pg_index) GETSTRUCT(tup);
		other = !index->indisprimary && (index->indisunique || index->indisexclusion);
		ReleaseSysCache(tup);
		if (other)
			return true;
	}
	return false;
}

/*
 * May a row a merge writes to rel break one of its constraints there: a
 * check, a not-null column outside the primary key, or a unique or
 * exclusion index other than the primary key's?  (The merge decides each
 * row by its primary key, so that one holds.)  Asked once a table.
 */
static bool
may_break_constraints(Relation rel)
{
	TupleDesc	desc = RelationGetDescr(rel);
	Bitmapset  *pk;

	if (RelationGetRelid(rel) == breakable_relid)
		return breakable;
	breakable_relid = RelationGetRelid(rel);
	breakable = true;
	if (desc->constr && desc->constr->num_check > 0)
		return true;
	pk = RelationGetIndexAttrBitmap(rel, INDEX_ATTR_BITMAP_PRIMARY_KEY);
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (att->attnotnull && !att->attisdropped &&
			!bms_is_member(att->attnum - FirstLowInvalidHeapAttributeNumber, pk))
			return true;
	}
	if (other_unique_index(rel))
		return true;
	breakable = false;
	return false;
}

/*
 * What a row a merge couldn't write breaks (ed, the error writing it
 * raised), as Dolt describes it: a unique index by its name and columns, a
 * check constraint by its name and expression, a not-null constraint by
 * its column.
 */
static char *
constraint_info(Relation rel, const char *type, const ErrorData *ed)
{
	const char *name = ed->constraint_name ? ed->constraint_name : "";
	StringInfoData s;

	initStringInfo(&s);
	if (strcmp(type, "not null") == 0)
		appendStringInfo(&s, "{\"Columns\": [%s]}",
						 quote_json(ed->column_name ? ed->column_name : ""));
	else if (strcmp(type, "check constraint") == 0)
	{
		Oid			con = get_relation_constraint_oid(RelationGetRelid(rel), name, true);
		char	   *expr = "";
		HeapTuple	tup;

		if (OidIsValid(con) &&
			(tup = SearchSysCache1(CONSTROID, ObjectIdGetDatum(con))) != NULL)
		{
			bool		isnull;
			Datum		bin = SysCacheGetAttr(CONSTROID, tup, Anum_pg_constraint_conbin,
											  &isnull);

			if (!isnull)
				expr = TextDatumGetCString(DirectFunctionCall2(pg_get_expr, bin,
															   ObjectIdGetDatum(RelationGetRelid(rel))));
			ReleaseSysCache(tup);
		}
		appendStringInfo(&s, "{\"Name\": %s, \"Expression\": %s}",
						 quote_json(name), quote_json(expr));
	}
	else
	{
		Oid			index = get_relname_relid(name, RelationGetNamespace(rel));
		HeapTuple	tup;
		bool		first = true;

		appendStringInfo(&s, "{\"Name\": %s, \"Columns\": [", quote_json(name));
		if (OidIsValid(index) &&
			(tup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(index))) != NULL)
		{
			Form_pg_index ix = (Form_pg_index) GETSTRUCT(tup);

			/* (an expression's column has no name) */
			for (int i = 0; i < ix->indnkeyatts; i++)
			{
				if (ix->indkey.values[i] == 0)
					continue;
				appendStringInfo(&s, "%s%s", first ? "" : ", ",
								 quote_json(get_attname(RelationGetRelid(rel),
														ix->indkey.values[i], false)));
				first = false;
			}
			ReleaseSysCache(tup);
		}
		appendStringInfoString(&s, "]}");
	}
	return s.data;
}

/*
 * Write a row as a merge does: insert it, or update old to it.  While a
 * merge records violations, a row that breaks a unique, check or not-null
 * (or exclusion) constraint isn't written, and is recorded instead: a
 * row the merge made of both sides' columns too, the branch's kept (a
 * table holds no row breaking a constraint, unlike Dolt's).
 * Returns false if old isn't there.
 */
static bool
merge_write(const ProllyApplierRoutine *ar, ProllyApplier *a, Relation rel,
			TupleTableSlot *old, TupleTableSlot *row, const bool *keep)
{
	MemoryContext cxt = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	volatile bool ok = true;

	if (!recording || !may_break_constraints(rel))
	{
		if (old)
			return PROLLY_CALL(ar, update, a, old, row, keep);
		PROLLY_CALL(ar, insert, a, row);
		return true;
	}
	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(cxt);
	PG_TRY();
	{
		if (old)
			ok = PROLLY_CALL(ar, update, a, old, row, keep);
		else
			PROLLY_CALL(ar, insert, a, row);
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(cxt);
		CurrentResourceOwner = owner;
	}
	PG_CATCH();
	{
		ErrorData  *ed;
		const char *type;
		char	   *info;
		MemoryContext rec;

		MemoryContextSwitchTo(cxt);
		ed = CopyErrorData();
		switch (ed->sqlerrcode)
		{
			case ERRCODE_UNIQUE_VIOLATION:
			case ERRCODE_EXCLUSION_VIOLATION:
				type = "unique index";
				break;
			case ERRCODE_CHECK_VIOLATION:
				type = "check constraint";
				break;
			case ERRCODE_NOT_NULL_VIOLATION:
				type = "not null";
				break;
			default:
				PG_RE_THROW();
		}
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(cxt);
		CurrentResourceOwner = owner;
		info = constraint_info(rel, type, ed);
		rec = MemoryContextSwitchTo(TopTransactionContext);
		recorded = lappend(recorded,
						   violation(RelationGetRelid(rel), type, row_json(row),
									 pstrdup(info)));
		MemoryContextSwitchTo(rec);
		FreeErrorData(ed);
	}
	PG_END_TRY();
	return ok;
}

/* One pass of apply_rows over the diff: its removals, or its inserts. */
typedef struct Applying
{
	Changes		c;				/* (for the keying) */
	const ProllyApplierRoutine *ar;
	ProllyApplier *a;
	Relation	rel;
	TupleTableSlot *slot;
	TupleTableSlot *old;		/* a row updated, as it was */
	bool	   *kept;			/* its columns kept as stored */
	Bitmapset  *pk;				/* the primary key's columns */
	bool		keyless;
	bool		in_place;		/* changed keyed rows updated in place */
	bool		inserting;
} Applying;

/*
 * May a keyed row of rel that changed be updated in place, rather than
 * removed and inserted again?  Only where no unique or exclusion index but
 * the primary key's could be broken on the way, as by two rows swapping a
 * value.
 */
static bool
updates_in_place(Relation rel)
{
	return !other_unique_index(rel);
}

/*
 * A keyed row's update in place: old as it was, with its columns out of
 * line left unread, but its primary key (pk, its columns as
 * RelationGetIndexAttrBitmap() gives them) whole: the row is found by it;
 * and row, with the columns out of line that old has too marked in kept.
 */
static void
decode_update(Changes *c, Relation rel, const Bitmapset *pk,
			  const ProllyValue *before, const ProllyValue *after,
			  TupleTableSlot *old, TupleTableSlot *row, bool *kept)
{
	TupleDesc	desc = RelationGetDescr(rel);

	/* (decoded as kept against itself: every column out of line unread) */
	ExecClearTuple(old);
	PROLLY_CALL(c->rd.kr, decode_row_kept, c->rd.k, before, before, old, kept);
	for (int i = 0; i < desc->natts; i++)
	{
		if (kept[i] &&
			bms_is_member(i + 1 - FirstLowInvalidHeapAttributeNumber, pk))
		{
			ExecClearTuple(old);
			PROLLY_CALL(c->rd.kr, decode_row, c->rd.k, before, old);
			break;
		}
	}
	ExecClearTuple(row);
	PROLLY_CALL(c->rd.kr, decode_row_kept, c->rd.k, after, before, row, kept);
}

/* How many copies a tree value stands for (0: none), and its row. */
static uint64
copies_of(Changes *c, const ProllyValue *value, ProllyValue *row)
{
	uint64		count;

	if (value == NULL || value->len == 0)
		return 0;
	PROLLY_CALL(c->rd.kr, parse_value, c->rd.k, value, row, &count);
	return count;
}

static void
apply_change(ProllyDiffKind kind, const ProllyKey *key,
			 const ProllyValue *before, const ProllyValue *after, void *arg)
{
	Applying   *ap = arg;
	ProllyValue brow;
	ProllyValue arow;
	uint64		nb = copies_of(&ap->c, before, &brow);
	uint64		na = copies_of(&ap->c, after, &arow);

	/* a keyed row that changed, updated in place, while inserting */
	if (ap->in_place && nb == 1 && na == 1)
	{
		if (!ap->inserting)
			return;
		decode_update(&ap->c, ap->rel, ap->pk, &brow, &arow, ap->old,
					  ap->slot, ap->kept);
		if (!merge_write(ap->ar, ap->a, ap->rel, ap->old, ap->slot, ap->kept))
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("a row of \"%s\" changed while the branch was moved",
							RelationGetRelationName(ap->rel)),
					 errhint("Try again.")));
		return;
	}

	/* a keyless row's copies change in number, a keyed row whole */
	if (ap->keyless && na >= nb)
	{
		na -= nb;
		nb = 0;
	}
	else if (ap->keyless)
	{
		nb -= na;
		na = 0;
	}
	if (!ap->inserting && nb > 0)
	{
		ExecClearTuple(ap->slot);
		PROLLY_CALL(ap->c.rd.kr, decode_row, ap->c.rd.k, &brow, ap->slot);
		for (uint64 i = 0; i < nb; i++)
			if (!PROLLY_CALL(ap->ar, remove, ap->a, ap->slot))
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("a row of \"%s\" changed while the branch was moved",
								RelationGetRelationName(ap->rel)),
						 errhint("Try again.")));
	}
	else if (ap->inserting && na > 0)
	{
		ExecClearTuple(ap->slot);
		PROLLY_CALL(ap->c.rd.kr, decode_row, ap->c.rd.k, &arow, ap->slot);
		for (uint64 i = 0; i < na; i++)
			(void) merge_write(ap->ar, ap->a, ap->rel, NULL, ap->slot, NULL);
	}
}

/*
 * Write the rows of relation relid that differ between from and to: the
 * removals, then the inserts (a row taking a unique value another gives
 * up), each written as the trees' diff finds it, nothing held.
 */
static void
apply_rows(Oid relid, const Table *from, const Table *to)
{
	const ProllyTreeRoutine *tree = prolly_tree_lookup("cow");
	ProllyTreeEnv env;
	ProllyKeyingRefs krefs;
	Applying	ap;

	require_readable(from);
	require_readable(to);
	if (recording)
		note_written(relid);
	memset(&ap, 0, sizeof(ap));
	ap.rel = table_open(relid, RowExclusiveLock);
	ap.ar = prolly_applier_lookup("replication");
	ap.c.rd.kr = keying_of(ap.rel);
	ap.c.rd.k = PROLLY_CALL(ap.c.rd.kr, open, ap.rel,
							CurrentMemoryContext);
	ap.keyless = PROLLY_CALL(ap.c.rd.kr, keyless, ap.c.rd.k);
	ap.in_place = !ap.keyless && updates_in_place(ap.rel);
	chunk_env(&env);
	env.compare = changes_compare;
	env.compare_arg = &ap.c;
	prolly_keying_use_env(&krefs, ap.c.rd.kr, ap.c.rd.k, tree, &env);
	/* (a keyless row is matched by its values: read once, not by pointer) */
	krefs.values.pointers = !ap.keyless;

	ap.slot = table_slot_create(ap.rel, NULL);
	ap.old = table_slot_create(ap.rel, NULL);
	ap.kept = palloc0_array(bool, RelationGetDescr(ap.rel)->natts);
	ap.pk = RelationGetIndexAttrBitmap(ap.rel, INDEX_ATTR_BITMAP_PRIMARY_KEY);
	ap.a = PROLLY_CALL(ap.ar, begin, ap.rel);
	for (int pass = 0; pass < 2; pass++)
	{
		ap.inserting = (pass == 1);
		PROLLY_CALL(tree, diff, &env, &from->data, &to->data, apply_change, &ap);
	}
	PROLLY_CALL(ap.ar, end, ap.a);
	ExecDropSingleTupleTableSlot(ap.slot);
	ExecDropSingleTupleTableSlot(ap.old);
	pfree(ap.kept);
	bms_free(ap.pk);
	PROLLY_CALL(ap.c.rd.kr, close, ap.c.rd.k);
	table_close(ap.rel, NoLock);
}

/* A sequence's value, as a canonical root has it. */
typedef struct Sequence
{
	char	   *name;
	int64		last;
	bool		called;
} Sequence;

static void
add_sequence(const char *name, int64 last, bool called, void *arg)
{
	List	  **list = arg;
	Sequence   *sq = palloc(sizeof(Sequence));

	sq->name = pstrdup(name);
	sq->last = last;
	sq->called = called;
	*list = lappend(*list, sq);
}

static List *
sequences_of(const ProllyNodeRef *root)
{
	List	   *list = NIL;

	PROLLY_CALL(components().canonicalizer, sequences, root, add_sequence, &list);
	return list;
}

static bool
same_sequence(const Sequence *a, const Sequence *b)
{
	return a->last == b->last && a->called == b->called;
}

/*
 * The value a sequence gives out next (saturating at the type's end, as
 * no further one is).
 */
static int64
next_value(const Sequence *sq, int64 increment)
{
	int64		next;

	if (!sq->called)
		return sq->last;
	if (pg_add_s64_overflow(sq->last, increment, &next))
		return increment > 0 ? PG_INT64_MAX : PG_INT64_MIN;
	return next;
}

/*
 * a has gone further than b: it gives out next a value b would only after
 * (higher, or lower for a descending sequence).
 */
static bool
further(const Sequence *a, const Sequence *b, int64 increment)
{
	int64		na = next_value(a, increment);
	int64		nb = next_value(b, increment);

	return increment > 0 ? na > nb : na < nb;
}

static int64
sequence_increment(Oid relid)
{
	HeapTuple	tup = SearchSysCache1(SEQRELID, ObjectIdGetDatum(relid));
	int64		increment;

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for sequence %u", relid);
	increment = ((Form_pg_sequence) GETSTRUCT(tup))->seqincrement;
	ReleaseSysCache(tup);
	return increment;
}

/*
 * A sequence set as a commit has it: with the transaction (a rollback
 * undoes it, as the rows), as the branch's data rather than as setval.
 */
static void
set_sequence(const Sequence *sq)
{
	RestoreSequenceState(rel_named(sq->name, false), sq->last,
						 sq->called);
}

/* Sequences by name, to find each once. */
typedef struct SequenceEntry
{
	char		name[2 * (2 * NAMEDATALEN + 2) + 2];	/* (the key) */
	Sequence   *sq;
} SequenceEntry;

static HTAB *
sequences_by_name(List *list)
{
	HASHCTL		ctl;
	HTAB	   *h;
	ListCell   *lc;

	ctl.keysize = sizeof(((SequenceEntry *) NULL)->name);
	ctl.entrysize = sizeof(SequenceEntry);
	ctl.hcxt = CurrentMemoryContext;
	h = hash_create("prolly sequences", Max(list_length(list), 16), &ctl,
					HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);
	foreach(lc, list)
	{
		Sequence   *sq = lfirst(lc);
		SequenceEntry *e = hash_search(h, sq->name, HASH_ENTER, NULL);

		e->sq = sq;
	}
	return h;
}

static Sequence *
lookup_sequence(HTAB *h, const char *name)
{
	SequenceEntry *e = hash_search(h, name, HASH_FIND, NULL);

	return e ? e->sq : NULL;
}

/*
 * Write every table's rows that differ from target's, the branch's as
 * working has them.
 */
static void
rows_from(const ProllyNodeRef *working, const ProllyNodeRef *target)
{
	ProllyDatabaseComponents dc = components();
	Tables		from = {NIL};
	Tables		to = {NIL};
	ListCell   *lc;

	PROLLY_CALL(dc.canonicalizer, tables, working, add_table, &from);
	PROLLY_CALL(dc.canonicalizer, tables, target, add_table, &to);
	foreach(lc, to.tables)
	{
		Table	   *t = lfirst(lc);
		Table	   *f = find_table(from.tables, t->name);

		if (f == NULL)
			elog(ERROR, "table \"%s\" is missing from the working set", t->name);
		if (!same_hash(&f->data.hash, &t->data.hash))
			apply_rows(table_named(t->name), f, t);
	}

	/*
	 * Materialized views' rows too, as the commit holds them: those there
	 * are (one being made again is written once it is).
	 */
	from.tables = to.tables = NIL;
	PROLLY_CALL(dc.canonicalizer, materialized_views, working, add_table, &from);
	PROLLY_CALL(dc.canonicalizer, materialized_views, target, add_table, &to);
	foreach(lc, to.tables)
	{
		Table	   *t = lfirst(lc);
		Table	   *f = find_table(from.tables, t->name);

		if (f != NULL && !same_hash(&f->data.hash, &t->data.hash))
			apply_rows(table_named(t->name), f, t);
	}

	/* and every sequence's value */
	{
		HTAB	   *now = sequences_by_name(sequences_of(working));

		foreach(lc, sequences_of(target))
		{
			Sequence   *t = lfirst(lc);
			Sequence   *f = lookup_sequence(now, t->name);

			if (f == NULL || !same_sequence(f, t))
				set_sequence(t);
		}
	}
}

/* The same after definitions changed: the branch folded as it is now. */
static void
move_rows(void *arg)
{
	ProllyNodeRef working;

	(void) PROLLY_CALL(components().refs, working_set, NULL, &working);
	rows_from(&working, arg);
}

/* The definitions first, if they differ, then the rows. */
static void
folded_move_to(const ProllyNodeRef *target, const ProllyNodeRef *folded)
{
	ProllyDatabaseComponents dc = components();
	ProllyNodeRef working;

	if (folded)
		working = *folded;
	else
		(void) PROLLY_CALL(dc.refs, working_set, NULL, &working);
	if (PROLLY_CALL(dc.canonicalizer, same_schema, &working, target))
		rows_from(&working, target);	/* (folded already) */
	else
		PROLLY_CALL(dc.canonicalizer, move_schema, &working, target, move_rows,
					NULL, (void *) target, true);
}

/*
 * Each sequence theirs changed since base gets the larger of its value and
 * ours.
 */
static void
merge_sequences(const ProllyNodeRef *base, const ProllyNodeRef *ours,
				const ProllyNodeRef *theirs)
{
	HTAB	   *b = sequences_by_name(sequences_of(base));
	HTAB	   *o = sequences_by_name(sequences_of(ours));
	ListCell   *lc;
	Oid			relid;

	foreach(lc, sequences_of(theirs))
	{
		Sequence   *t = lfirst(lc);
		Sequence   *bs = lookup_sequence(b, t->name);
		Sequence   *os = lookup_sequence(o, t->name);

		if (bs && same_sequence(bs, t))
			continue;
		relid = rel_named(t->name, true);
		if (!OidIsValid(relid))
			continue;			/* the merged definitions dropped it */
		if (os == NULL || further(t, os, sequence_increment(relid)))
			set_sequence(t);
	}
}

/* ------------------------------------------------------------------------
 * Merging
 * ------------------------------------------------------------------------
 */

/* What merge_table() does with each change of theirs. */
typedef enum WalkMode
{
	WALK_MERGE,					/* write it, unless it conflicts */
	WALK_LIST,					/* report it if it conflicts */
	WALK_TAKE_THEIRS			/* if it conflicts, make the row theirs */
} WalkMode;

static const ProllyValue *
value_or_null(const StringInfoData *s, ProllyValue *v)
{
	if (s->len == 0)
		return NULL;
	v->data = (const uint8 *) s->data;
	v->len = s->len;
	return v;
}

static bool
same_bytes(const StringInfoData *a, const StringInfoData *b)
{
	return a->len == b->len && memcmp(a->data, b->data, a->len) == 0;
}

/* What a merge writes for one key: copies of a row removed, then added. */
typedef struct KeyWrite
{
	const StringInfoData *old;	/* the row removed, nold times */
	int64		nold;
	const StringInfoData *row;	/* the row inserted, nrow times */
	int64		nrow;
	bool		in_place;		/* or old updated to row */
} KeyWrite;

/*
 * Each column's length coercion (as INSERT applies its typmod), for
 * checking the rows a resolution writes.
 */
typedef struct FitCheck
{
	EState	   *estate;
	ExprState **coerce;			/* per column, or NULL: none */
} FitCheck;

/*
 * Refuse row (of rel, not yet written) where a column's value isn't one
 * the column can hold as its typmod is now: theirs' row, taken in a
 * conflict's resolution, read from a version whose column was wider and
 * kept as stored (see ReadFor), not written as it is.  The coercion's own
 * error (value too long) where it raises one.  kept: columns not decoded.
 */
static void
check_fits(FitCheck *fc, Relation rel, TupleTableSlot *row, const bool *kept)
{
	TupleDesc	desc = RelationGetDescr(rel);
	ExprContext *econtext;

	if (fc->estate == NULL)
	{
		fc->estate = CreateExecutorState();
		fc->coerce = palloc0_array(ExprState *, desc->natts);
		for (int i = 0; i < desc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(desc, i);
			Node	   *var;
			Node	   *e;

			if (att->attisdropped || att->attgenerated || att->atttypmod < 0)
				continue;
			var = (Node *) makeVar(1, i + 1, att->atttypid, -1,
								   att->attcollation, 0);
			e = coerce_to_target_type(NULL, var, att->atttypid,
									  att->atttypid, att->atttypmod,
									  COERCION_ASSIGNMENT, COERCE_IMPLICIT_CAST, -1);
			if (e == NULL || e == var)
				continue;
			assign_expr_collations(NULL, e);
			fc->coerce[i] = ExecPrepareExpr((Expr *) e, fc->estate);
		}
	}
	econtext = GetPerTupleExprContext(fc->estate);
	slot_getallattrs(row);
	econtext->ecxt_scantuple = row;
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);
		bool		isnull;
		Datum		d;

		if (fc->coerce[i] == NULL || row->tts_isnull[i] || (kept && kept[i]))
			continue;
		d = ExecEvalExprSwitchContext(fc->coerce[i], econtext, &isnull);
		if (isnull || !datum_image_eq(d, row->tts_values[i], att->attbyval,
									  att->attlen))
			ereport(ERROR,
					(errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
					 errmsg("the value of column \"%s\" of a row of \"%s\" taken doesn't fit type %s",
							NameStr(att->attname), RelationGetRelationName(rel),
							format_type_with_typemod(att->atttypid, att->atttypmod))));
	}
	ResetExprContext(econtext);
}

/* Three versions of a row decoded, for merging it column by column. */
typedef struct CellMerge
{
	TupleTableSlot *base;
	TupleTableSlot *ours;
	TupleTableSlot *theirs;
	TupleTableSlot *merged;
	StringInfoData key;
	StringInfoData row;
} CellMerge;

/*
 * Merge a keyed row both sides changed, column by column, as Dolt does: a
 * column one side changed takes that side's value, one both changed alike
 * keeps it, and one they changed differently is a conflict (the row's).
 * A stored generated column isn't compared: written, the row's is
 * computed again from the rest.  A column in unsure (base's value there
 * unsure: see ReadFor) is taken as changed on both sides.  False on a
 * conflict; else *merged is the merged row's tree value (palloc'd).
 * (Only rows both sides changed are decoded.)
 */
static bool
merge_cells(Changes *c, CellMerge *cm, const StringInfoData *base,
			const StringInfoData *ours, const StringInfoData *theirs,
			const Bitmapset *unsure, const StringInfoData **merged)
{
	TupleDesc	desc = cm->merged->tts_tupleDescriptor;
	StringInfo	value;
	ProllyValue row;
	ProllyHash	content;

	(void) decode(c, base, cm->base);
	(void) decode(c, ours, cm->ours);
	(void) decode(c, theirs, cm->theirs);
	slot_getallattrs(cm->base);
	slot_getallattrs(cm->ours);
	slot_getallattrs(cm->theirs);
	ExecClearTuple(cm->merged);
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);
		bool		bn = cm->base->tts_isnull[i];
		bool		on = cm->ours->tts_isnull[i];
		bool		tn = cm->theirs->tts_isnull[i];
		Datum		bv = cm->base->tts_values[i];
		Datum		ov = cm->ours->tts_values[i];
		Datum		tv = cm->theirs->tts_values[i];
		bool		ours_changed;
		bool		theirs_changed;

		if (att->attisdropped)
		{
			cm->merged->tts_isnull[i] = true;
			continue;
		}
		if (att->attgenerated)
		{
			cm->merged->tts_isnull[i] = on;
			cm->merged->tts_values[i] = ov;
			continue;
		}
		ours_changed = bn != on || bms_is_member(i, unsure) ||
			(!bn && !datum_image_eq(bv, ov, att->attbyval, att->attlen));
		theirs_changed = bn != tn || bms_is_member(i, unsure) ||
			(!bn && !datum_image_eq(bv, tv, att->attbyval, att->attlen));
		if (ours_changed && theirs_changed &&
			(on != tn ||
			 (!on && !datum_image_eq(ov, tv, att->attbyval, att->attlen))))
			return false;
		cm->merged->tts_isnull[i] = theirs_changed ? tn : on;
		cm->merged->tts_values[i] = theirs_changed ? tv : ov;
	}
	ExecStoreVirtualTuple(cm->merged);
	resetStringInfo(&cm->key);
	resetStringInfo(&cm->row);
	PROLLY_CALL(c->rd.kr, encode_row, c->rd.k, cm->merged, &cm->key, &cm->row,
				&content);
	row.data = (const uint8 *) cm->row.data;
	row.len = cm->row.len;
	value = palloc(sizeof(StringInfoData));
	initStringInfo(value);
	PROLLY_CALL(c->rd.kr, make_value, c->rd.k, &row, 1, value);
	*merged = value;
	return true;
}

/*
 * Walk theirs (table relid's changes from base to theirs) against ours
 * (base to ours, the session's head, which the branch still is), both in
 * key order, as mode says.  A key both sides changed to different rows is
 * a conflict, unless the columns each changed merge (merge_cells()); one
 * they changed alike isn't.  A keyless table's row
 * changed alike on both sides (by the same number of copies) is changed
 * once; otherwise it has the copies its base had, those either side
 * added, less the most either removed; it never conflicts.  current, if
 * not NULL, is the table as the branch has it now (listing or resolving a
 * merge in progress): a conflict's row on the branch is its row there,
 * none if it was deleted since, not ours'.  unsure, if not NULL, is base's
 * rows with an unsure value (as_defined_now()): each side that has such a
 * row changed it, in those columns.  Returns how many keys conflict.
 */
static int
merge_table(Oid relid, const Table *base, const Table *ours,
			const Table *theirs, WalkMode mode, ProllyConflictSink *sink,
			const Table *current, const UnsureRows *unsure)
{
	const ProllyTreeRoutine *tree = prolly_tree_lookup("cow");
	const ProllyApplierRoutine *ar = prolly_applier_lookup("replication");
	Relation	rel = table_open(relid, mode == WALK_LIST ? AccessShareLock :
								 RowExclusiveLock);
	ProllyTreeEnv env;
	ProllyKeyingRefs krefs;
	Changes		oc;
	Changes		tc;
	ProllyApplier *a = NULL;
	TupleTableSlot *s1;
	TupleTableSlot *s2;
	ListCell   *lo;
	ListCell   *lt;
	int			conflicts = 0;
	bool		keyless;
	bool		in_place;
	bool	   *kept;
	Bitmapset  *pk;
	List	   *writes = NIL;
	CellMerge	cm = {0};
	StringInfoData listed = {0};	/* (the branch's row now, listing) */
	int			ui = 0;			/* (unsure's next row) */
	FitCheck	fits = {0};

	require_readable(base);
	require_readable(ours);
	require_readable(theirs);
	require_readable(current);
	if (recording && mode == WALK_MERGE)
		note_written(relid);
	memset(&tc, 0, sizeof(tc));
	tc.rd.kr = keying_of(rel);
	tc.rd.k = PROLLY_CALL(tc.rd.kr, open, rel, CurrentMemoryContext);
	keyless = PROLLY_CALL(tc.rd.kr, keyless, tc.rd.k);
	in_place = !keyless && updates_in_place(rel);
	kept = palloc0_array(bool, RelationGetDescr(rel)->natts);
	pk = RelationGetIndexAttrBitmap(rel, INDEX_ATTR_BITMAP_PRIMARY_KEY);
	oc = tc;
	chunk_env(&env);
	env.compare = changes_compare;
	env.compare_arg = &tc;
	prolly_keying_use_env(&krefs, tc.rd.kr, tc.rd.k, tree, &env);
	krefs.values.pointers = !keyless;
	PROLLY_CALL(tree, diff, &env, &base->data, &theirs->data, collect_change,
				&tc);
	PROLLY_CALL(tree, diff, &env, &base->data, &ours->data, collect_change,
				&oc);
	if (unsure && unsure->n > 0 && !keyless)
	{
		tc.list = with_unsure_rows(&tc, unsure, tree, &env, base);
		oc.list = with_unsure_rows(&oc, unsure, tree, &env, base);
	}
	else
		unsure = NULL;

	s1 = table_slot_create(rel, NULL);
	s2 = table_slot_create(rel, NULL);
	if (mode != WALK_LIST)
		a = PROLLY_CALL(ar, begin, rel);
	lo = list_head(oc.list);
	foreach(lt, tc.list)
	{
		Change	   *t = lfirst(lt);
		Change	   *o = NULL;
		ProllyKey	tk = {(const uint8 *) t->key.data, t->key.len};
		const StringInfoData *now;
		const StringInfoData *row;	/* what s2 holds */
		int64		have;
		int64		want;
		bool		conflict;
		KeyWrite   *w;
		const Bitmapset *unsure_cols = NULL;

		while (lo)
		{
			Change	   *cand = lfirst(lo);
			ProllyKey	ok = {(const uint8 *) cand->key.data, cand->key.len};
			int			r = PROLLY_CALL(tc.rd.kr, compare, tc.rd.k, &ok, &tk);

			if (r < 0)
				lo = lnext(oc.list, lo);
			else
			{
				if (r == 0)
					o = cand;
				break;
			}
		}

		/* the row as the branch has it, and as theirs has it: how many */
		now = o ? &o->after : &t->before;
		have = (int64) count_of(&tc, now);
		want = (int64) count_of(&tc, &t->after);
		row = &t->after;
		if (keyless)
		{
			int64		had = (int64) count_of(&tc, &t->before);
			int64		by_ours = have - had;
			int64		by_theirs = want - had;

			/*
			 * The same change on both sides is made once (the same update,
			 * or a pick the branch has already); otherwise copies added on
			 * either side are each a row, and copies removed on both are
			 * removed once (two deleting it deleted it).  Each side removes
			 * no more than the base had, so the count isn't below none, and
			 * a keyless row never conflicts.
			 */
			if (by_ours == by_theirs)
				want = have;
			else
				want = had + Max(by_ours, 0) + Max(by_theirs, 0) -
					Max(Max(-by_ours, 0), Max(-by_theirs, 0));
			Assert(want >= 0);
			conflict = false;
			/* (theirs changed the key: a row before or after) */
			Assert(t->after.len > 0 || t->before.len > 0);
			row = t->after.len > 0 ? &t->after : &t->before;
		}
		else
		{
			conflict = o != NULL && !same_bytes(&o->after, &t->after);
			while (unsure && ui < unsure->n)
			{
				int			r = changes_compare(&unsure->rows[ui]->key, &tk, &tc);

				if (r > 0)
					break;
				if (r == 0)
					unsure_cols = unsure->rows[ui]->cols;
				ui++;
			}

			/* both changed the row, neither removed it: column by column */
			if (conflict && t->before.len > 0 && o->after.len > 0 &&
				t->after.len > 0)
			{
				const StringInfoData *merged;

				if (cm.merged == NULL)
				{
					cm.base = table_slot_create(rel, NULL);
					cm.ours = table_slot_create(rel, NULL);
					cm.theirs = table_slot_create(rel, NULL);
					cm.merged = MakeSingleTupleTableSlot(RelationGetDescr(rel),
														 &TTSOpsVirtual);
					initStringInfo(&cm.key);
					initStringInfo(&cm.row);
				}
				if (merge_cells(&tc, &cm, &t->before, &o->after, &t->after,
								unsure_cols, &merged))
				{
					conflict = false;
					row = merged;
				}
			}
		}

		if (conflict)
		{
			ProllyValue bv,
						ov,
						tv;

			conflicts++;
			if (current)
			{
				StringInfo	cur;

				/* (a listed row is copied out at once; a write keeps its own) */
				if (mode == WALK_LIST)
				{
					if (listed.data == NULL)
						initStringInfo(&listed);
					resetStringInfo(&listed);
					cur = &listed;
				}
				else
					cur = makeStringInfo();
				if (!PROLLY_CALL(tree, get, &env, &current->data, &tk, cur))
					resetStringInfo(cur);
				now = cur;
				have = (int64) count_of(&tc, now);
			}
			if (mode == WALK_LIST)
				sink->conflict(sink, &tk, value_or_null(&t->before, &bv),
							   value_or_null(now, &ov),
							   value_or_null(&t->after, &tv));
			if (mode != WALK_TAKE_THEIRS)
				continue;
			/* as theirs has it: one row or none (keyed: keyless never conflict) */
			Assert(!keyless);
			want = (t->after.len > 0);
		}
		else if (mode != WALK_MERGE ||
				 (!keyless && o != NULL && same_bytes(&o->after, row)) ||
				 (keyless && want == have))
			continue;			/* listing; or the branch has it so already */

		/* the branch's row (now, have copies) becomes theirs (row, want) */
		w = palloc0(sizeof(KeyWrite));
		if (!keyless && have == 1 && want == 1)
		{
			w->old = now;
			w->nold = 1;
			w->row = row;
			w->nrow = 1;
			w->in_place = in_place;
		}
		else if (want < have)
		{
			w->old = keyless ? row : now;
			w->nold = have - Max(want, 0);
		}
		else
		{
			w->row = row;
			w->nrow = want - have;
		}
		writes = lappend(writes, w);
	}

	/*
	 * The rows written: those going first, then those coming, so a row may
	 * take a unique value another gives up (theirs swapping two, say).
	 */
	foreach(lt, writes)
	{
		KeyWrite   *w = lfirst(lt);

		if (w->nold == 0 || w->in_place)
			continue;
		(void) decode(&tc, w->old, s1);
		for (int64 i = 0; i < w->nold; i++)
			if (!PROLLY_CALL(ar, remove, a, s1))
				goto missing;
	}
	foreach(lt, writes)
	{
		KeyWrite   *w = lfirst(lt);
		int			nrecorded = list_length(recorded);

		if (w->nrow == 0)
			continue;
		if (w->in_place)
		{
			ProllyValue before;
			ProllyValue after;
			ProllyValue v;
			uint64		count;

			/* (a violation recorded instead leaves the row as it was) */
			v.data = (const uint8 *) w->old->data;
			v.len = w->old->len;
			PROLLY_CALL(tc.rd.kr, parse_value, tc.rd.k, &v, &before, &count);
			v.data = (const uint8 *) w->row->data;
			v.len = w->row->len;
			PROLLY_CALL(tc.rd.kr, parse_value, tc.rd.k, &v, &after, &count);
			decode_update(&tc, rel, pk, &before, &after, s1, s2, kept);
			if (mode == WALK_TAKE_THEIRS)
				check_fits(&fits, rel, s2, kept);
			if (!merge_write(ar, a, rel, s1, s2, kept))
				goto missing;
			continue;
		}
		(void) decode(&tc, w->row, s2);
		if (mode == WALK_TAKE_THEIRS)
			check_fits(&fits, rel, s2, NULL);
		for (int64 i = 0; i < w->nrow; i++)
			(void) merge_write(ar, a, rel, NULL, s2, NULL);

		/*
		 * A keyed row theirs changed, whose new version is recorded as a
		 * violation instead of written: the row stays as it was.
		 */
		if (!keyless && w->nold == 1 && list_length(recorded) > nrecorded)
		{
			(void) decode(&tc, w->old, s1);
			(void) merge_write(ar, a, rel, NULL, s1, NULL);
		}
	}
	if (a)
		PROLLY_CALL(ar, end, a);
	ExecDropSingleTupleTableSlot(s1);
	ExecDropSingleTupleTableSlot(s2);
	if (cm.merged)
	{
		ExecDropSingleTupleTableSlot(cm.base);
		ExecDropSingleTupleTableSlot(cm.ours);
		ExecDropSingleTupleTableSlot(cm.theirs);
		ExecDropSingleTupleTableSlot(cm.merged);
	}
	if (fits.estate)
		FreeExecutorState(fits.estate);
	pfree(kept);
	bms_free(pk);
	PROLLY_CALL(tc.rd.kr, close, tc.rd.k);
	table_close(rel, NoLock);
	return conflicts;

missing:
	ereport(ERROR,
			(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
			 errmsg("a row of \"%s\" changed while it was merged",
					RelationGetRelationName(rel)),
			 errhint("Try again.")));
	return 0;					/* keep compiler quiet */
}

/* ------------------------------------------------------------------------
 * Rows of another definition
 * ------------------------------------------------------------------------
 */

/* A column of a table as a version defines it. */
typedef struct VersionColumn
{
	char	   *name;
	Oid			type;
	int32		typmod;
	char	   *generated;		/* a generated column's expression, or NULL */
} VersionColumn;

/*
 * A version's live columns, in order, from its definition as described
 * (the canonicalizer reads its own descriptions back); and whether it has
 * a primary key.
 */
static List *
version_columns(const Table *x, bool *keyed)
{
	ProllyValue d = {(const uint8 *) x->text, x->textlen};
	List	   *cols = NIL;
	ListCell   *lc;

	foreach(lc, PROLLY_CALL(components().canonicalizer, table_columns, &d, keyed))
	{
		ProllyTableColumn *tc = lfirst(lc);
		VersionColumn *c = palloc0(sizeof(VersionColumn));

		c->name = tc->name;
		c->generated = tc->generated;
		(void) parseTypeString(tc->type, &c->type, &c->typmod, NULL);
		cols = lappend(cols, c);
	}
	return cols;
}

/*
 * A version's rows decoded: its columns, as its description has them, and
 * whether it has a primary key.
 */
typedef struct VersionReader
{
	ProllyValueEncoder *enc;
	TupleTableSlot *slot;
	List	   *cols;			/* VersionColumn * */
	bool		keyed;
} VersionReader;

static void
version_reader(const Table *x, ProllyKeyingRefs *krefs, VersionReader *vr)
{
	const ProllyValueEncoderRoutine *ve = prolly_valenc_lookup("canonical");
	TupleDesc	xd;
	ListCell   *lc;
	int			i = 1;

	vr->cols = version_columns(x, &vr->keyed);
	xd = CreateTemplateTupleDesc(list_length(vr->cols));
	foreach(lc, vr->cols)
	{
		VersionColumn *c = lfirst(lc);

		TupleDescInitEntry(xd, i++, c->name, c->type, c->typmod, 0);
	}
	TupleDescFinalize(xd);
	vr->enc = PROLLY_CALL(ve, begin, xd, CurrentMemoryContext);
	PROLLY_CALL(ve, set_values, vr->enc, &krefs->values);
	vr->slot = MakeSingleTupleTableSlot(xd, &TTSOpsVirtual);
}

/* Column name's generation expression among cols, or "" (none, or no such column). */
static const char *
generation_of(List *cols, const char *name)
{
	ListCell   *lc;

	foreach(lc, cols)
	{
		VersionColumn *c = lfirst(lc);

		if (strcmp(c->name, name) == 0)
			return c->generated ? c->generated : "";
	}
	return "";
}

typedef struct ConvRow
{
	ProllyKey	key;
	ProllyValue row;
	uint64		count;
} ConvRow;

static int
conv_row_cmp(const void *a, const void *b, void *arg)
{
	const ConvRow *x = *(ConvRow *const *) a;
	const ConvRow *y = *(ConvRow *const *) b;

	return keying_compare(&x->key, &y->key, arg);
}

/* A copy of len bytes, exactly. */
static const uint8 *
dup_bytes(const char *data, int len)
{
	uint8	   *p = palloc(Max(len, 1));

	memcpy(p, data, len);
	return p;
}

/*
 * How as_defined_now() reads a version's rows, by what's done with their
 * values: shown (a diff; a merge's versions listed or resolved), written
 * (a merge's sides, whose rows become the table's), or compared only (a
 * merge's base: a side changed a row where its value isn't base's).
 *
 * Only a written value has to be one the column could hold now, and it's
 * converted as ALTER TABLE ... TYPE without USING converted the side's
 * own rows.  A value shown or compared is made the column's only where
 * nothing is lost: it's cast, and the cast kept only if casting it back
 * gives the value again.  A lossy cast (rounding, cutting to fit) could
 * make two of base's keys one, or make a side's change look like none
 * (cut to 'abc', base's 'abcdefghij' would equal a side's 'abc'); and a
 * column both sides retyped alike may have been converted with a USING
 * other than the cast ((n * 100)::int), which the cast only matches
 * where it's what the side's ALTER did.  Where the cast doesn't come
 * back, a value of the same type, or of one stored alike (text and
 * varchar), is kept as it was: one a column of the new typmod can't
 * hold, so no side's value equals it, and every side has changed (or
 * deleted) that row.  (Into char(n), a value of another string type is
 * compared without its trailing blanks, which char(n) pads and casting
 * back trims, except in a key: 'ab' and 'ab ' would be one key.)
 *
 * A compared value of another type is trusted only where, besides coming
 * back, the cast agrees with the value's text form read as the new type
 * (as master converted it; between string types, where the cast only
 * pads or trims, that isn't asked).  A USING other than the cast ((n *
 * 100)::int, (NOT flag)::int) isn't recorded, so a cast that merely
 * round-trips could still match a side's edit by chance: true to 1
 * where ALTER made it 0.  Requiring the text form too keeps such a merge
 * no less careful than master's.  A value not trusted, in a column not
 * of the key, is unsure: the merge takes either side as having changed
 * it (as_defined_now()'s *unsure).  In a key, or a table without one,
 * nothing can say which side changed it: the merge is refused.
 *
 * A column both sides retyped is compared as master compared it: both
 * ALTERs may have had a USING (upper(code), rpad(name, 5)) that the cast
 * misses, and a side's edit back to the cast's value would look like
 * none.  Of the same type, base's cast is trusted only where it gives
 * the stored value, which is otherwise kept (char(3) to char(10): the
 * padding is a change, so a plain one made on both sides conflicts on a
 * row a side also edited, as on master).  Of another type, base's value
 * is its text form read as the new type, whatever the cast gives; only
 * where that fails (where master failed) is it unsure, or refused where
 * it can't be.
 */
typedef enum ReadFor
{
	READ_SHOWN,
	READ_WRITTEN,
	READ_COMPARED,
} ReadFor;

/*
 * What becomes of a value whose cast doesn't come back (ColumnRead's
 * back): it's cast anyway (none is checked), kept as it was, or cast
 * where it can be and marked unsure.
 */
typedef enum CastLoss
{
	LOSS_UNCHECKED,
	LOSS_KEEP_STORED,
	LOSS_UNSURE,
	LOSS_REFUSE,
} CastLoss;

/* How a column of a version's is read into the column it is now. */
typedef struct ColumnRead
{
	ExprState  *conv;			/* the cast, or NULL: none (unsure) */
	ExprState  *back;			/* conv's value cast back, or NULL: none
								 * (unchecked, or not kept: see loss) */
	bool		guarded;		/* conv or back may fail on a value */
	bool		need_back;		/* not kept without a cast back */
	bool		match_stored;	/* kept only as the stored value itself */
	bool		blank_padded;	/* compared back without trailing blanks */
	bool		by_text;		/* checked against the text form too, */
	bool		text_only;		/* or read as the text form alone: */
	FmgrInfo	out;			/* the old type's output */
	Oid			in;				/* the new type's input */
	Oid			in_param;
	CastLoss	loss;
} ColumnRead;

/* Numeric typmod tm's precision and scale. */
static void
numeric_typmod(int32 tm, int *precision, int *scale)
{
	*precision = ((tm - VARHDRSZ) >> 16) & 0xffff;
	*scale = (((tm - VARHDRSZ) & 0x7ff) ^ 1024) - 1024;
}

/*
 * Is a column of type type, typmod from, made typmod to, one whose cast
 * loses nothing and can't fail: a longer string or bit string, more
 * digits on both sides of a numeric's point, a finer time?
 */
static bool
typmod_widens(Oid type, int32 from, int32 to)
{
	switch (type)
	{
		case VARCHAROID:
		case BPCHAROID:
		case BITOID:
		case VARBITOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case TIMEOID:
		case TIMETZOID:
			return to < 0 || (from >= 0 && to >= from);
		case NUMERICOID:
			{
				int			fp,
							fs,
							tp,
							ts;

				if (to < 0)
					return true;
				if (from < 0)
					return false;
				numeric_typmod(from, &fp, &fs);
				numeric_typmod(to, &tp, &ts);
				return ts >= fs && tp - ts >= fp - fs;
			}
		default:
			return false;
	}
}

static bool
is_string_type(Oid type)
{
	return type == TEXTOID || type == VARCHAROID || type == BPCHAROID;
}

static bool
is_bit_type(Oid type)
{
	return type == BITOID || type == VARBITOID;
}

/*
 * How column j of xd (a version's) is read as rel's column att, read for
 * what (see ReadFor), into *cr; key: att is one of rel's key; keyless:
 * rel has none (in either, a compared value can't be unsure).  A value
 * written goes through the
 * column's assignment cast, with its typmod, as ALTER TABLE ... TYPE
 * without USING converts rows; where there's none (a change made with
 * USING), through the explicit cast a move's ALTER converts them with
 * (canon_folded.c's move_table()).  One shown or compared goes through
 * the explicit cast (which cuts a string to fit rather than failing),
 * checked by casting it back unless the cast is a widening, or a relabel
 * of a type stored alike, which loses nothing.
 */
static void
column_read(Relation rel, TupleDesc xd, int j, Form_pg_attribute att,
			ReadFor read, bool key, bool keyless, bool both, EState *estate,
			ColumnRead *cr)
{
	Form_pg_attribute xa = TupleDescAttr(xd, j);
	Node	   *var = (Node *) makeVar(1, j + 1, xa->atttypid, xa->atttypmod,
									   xa->attcollation, 0);
	Node	   *e = NULL;
	Node	   *back;
	bool		same = xa->atttypid == att->atttypid;
	bool		alike = same ||
		(get_typtype(att->atttypid) != TYPTYPE_DOMAIN &&
		 IsBinaryCoercible(xa->atttypid, att->atttypid));

	memset(cr, 0, sizeof(ColumnRead));
	if (read == READ_WRITTEN)
		e = coerce_to_target_type(NULL, var, xa->atttypid,
								  att->atttypid, att->atttypmod,
								  COERCION_ASSIGNMENT, COERCE_IMPLICIT_CAST, -1);
	if (e == NULL)
		e = coerce_to_target_type(NULL, var, xa->atttypid,
								  att->atttypid, att->atttypmod,
								  COERCION_EXPLICIT, COERCE_EXPLICIT_CAST, -1);

	/*
	 * (compared, retyped by both sides: as master compared it, base's
	 * stored value for the same type, the cast trusted only where it gives
	 * that; for another type its text form read as the new type)
	 */
	if (read == READ_COMPARED && both)
	{
		if (same)
		{
			assign_expr_collations(NULL, e);
			cr->conv = ExecPrepareExpr((Expr *) e, estate);
			cr->loss = LOSS_KEEP_STORED;
			cr->match_stored = true;
			cr->guarded = !(is_string_type(att->atttypid) ||
							is_bit_type(att->atttypid));
		}
		else
		{
			Oid			outf;
			bool		varlena;

			/*
			 * (the text form is what master compared, whether or not the
			 * cast agrees; only where it doesn't read as the new type,
			 * where master failed, is the value unsure, or refused)
			 */
			cr->loss = key || keyless ? LOSS_REFUSE : LOSS_UNSURE;
			cr->text_only = true;
			getTypeOutputInfo(xa->atttypid, &outf, &varlena);
			fmgr_info(outf, &cr->out);
			getTypeInputInfo(att->atttypid, &cr->in, &cr->in_param);
			cr->guarded = !(is_string_type(xa->atttypid) &&
							is_string_type(att->atttypid) &&
							att->atttypmod < 0);
		}
		return;
	}

	if (e == NULL && read == READ_COMPARED && !key && !keyless)
	{
		/* (no cast at all: every value of base's is unsure) */
		cr->loss = LOSS_UNSURE;
		return;
	}
	if (e == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("rows of table \"%s\" can't be read across column \"%s\" changing type from %s to %s",
						RelationGetRelationName(rel), NameStr(att->attname),
						format_type_with_typemod(xa->atttypid, xa->atttypmod),
						format_type_with_typemod(att->atttypid, att->atttypmod))));
	assign_expr_collations(NULL, e);
	cr->conv = ExecPrepareExpr((Expr *) e, estate);

	/* (written; or nothing to lose) */
	if (read == READ_WRITTEN ||
		(same && typmod_widens(att->atttypid, xa->atttypmod, att->atttypmod)) ||
		(alike && !same && att->atttypmod < 0))
		return;
	if (alike)
	{
		cr->loss = LOSS_KEEP_STORED;
		cr->blank_padded = att->atttypid == BPCHAROID &&
			xa->atttypid != BPCHAROID && !key;
	}
	else if (read == READ_COMPARED)
	{
		cr->loss = key || keyless ? LOSS_REFUSE : LOSS_UNSURE;
		if (!(is_string_type(xa->atttypid) && is_string_type(att->atttypid)))
		{
			Oid			outf;
			bool		varlena;

			cr->by_text = true;
			getTypeOutputInfo(xa->atttypid, &outf, &varlena);
			fmgr_info(outf, &cr->out);
			getTypeInputInfo(att->atttypid, &cr->in, &cr->in_param);
		}
	}
	else
		return;					/* (shown: as the cast has it) */

	back = coerce_to_target_type(NULL, copyObject(e), att->atttypid,
								 xa->atttypid, xa->atttypmod,
								 COERCION_EXPLICIT, COERCE_EXPLICIT_CAST, -1);
	if (back != NULL)
	{
		assign_expr_collations(NULL, back);
		cr->back = ExecPrepareExpr((Expr *) back, estate);
	}
	cr->need_back = true;

	/*
	 * (an explicit cast between string types, or between bit strings, only
	 * cuts, pads or trims, both ways; anything else may fail)
	 */
	cr->guarded = !((is_string_type(xa->atttypid) && is_string_type(att->atttypid)) ||
					(is_bit_type(xa->atttypid) && is_bit_type(att->atttypid)));
}

/* Are string values a and b the same, but for trailing blanks? */
static bool
same_but_trailing_blanks(Datum a, Datum b)
{
	text	   *ta = DatumGetTextPP(a);
	text	   *tb = DatumGetTextPP(b);
	const char *pa = VARDATA_ANY(ta);
	const char *pb = VARDATA_ANY(tb);
	int			la = VARSIZE_ANY_EXHDR(ta);
	int			lb = VARSIZE_ANY_EXHDR(tb);

	while (la > 0 && pa[la - 1] == ' ')
		la--;
	while (lb > 0 && pb[lb - 1] == ' ')
		lb--;
	return la == lb && memcmp(pa, pb, la) == 0;
}

/*
 * Column cr's value of xs's attribute j (att's type before: xatt), read
 * into *d and *isnull, in the current memory context, through cr->conv
 * (evaluated in econtext), or as its text form read as the new type
 * (cr->text_only).  False where the value doesn't come back from
 * it (cr->back; or there's no cast back where one's needed), or differs
 * from the value's text form read as the new type (cr->by_text), or the
 * cast or that read fails with a data exception (value too long,
 * numeric field overflow, invalid input) where it's guarded; *d and
 * *isnull are then the cast's value if it gave one, else null.  Other
 * errors are raised.
 */
static bool
read_value(const ColumnRead *cr, ExprContext *econtext, TupleTableSlot *xs,
		   int j, Form_pg_attribute att, Form_pg_attribute xatt,
		   Datum *d, bool *isnull)
{
	MemoryContext cxt = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	volatile bool kept = true;

	*isnull = true;
	*d = (Datum) 0;
	if (cr->conv == NULL && !cr->text_only)
		return false;
	econtext->ecxt_scantuple = xs;
	if (cr->guarded)
	{
		BeginInternalSubTransaction(NULL);
		MemoryContextSwitchTo(cxt);
	}
	PG_TRY();
	{
		bool		n;
		Datum		v;

		if (cr->text_only)
		{
			char	   *text = OutputFunctionCall((FmgrInfo *) &cr->out,
												  xs->tts_values[j]);

			*d = OidInputFunctionCall(cr->in, text, cr->in_param,
									  att->atttypmod);
			*isnull = false;
			goto done;
		}
		v = ExecEvalExprSwitchContext(cr->conv, econtext, &n);
		*isnull = n;
		*d = n ? (Datum) 0 : datumCopy(v, att->attbyval, att->attlen);
		if (cr->match_stored)
			kept = !n && datum_image_eq(*d, xs->tts_values[j], att->attbyval,
										att->attlen);
		else if (cr->back)
		{
			Datum		b = ExecEvalExprSwitchContext(cr->back, econtext, &n);

			kept = !n &&
				(cr->blank_padded ?
				 same_but_trailing_blanks(b, xs->tts_values[j]) :
				 datum_image_eq(b, xs->tts_values[j], xatt->attbyval,
								xatt->attlen));
		}
		else if (cr->need_back)
			kept = false;		/* (no cast back: nothing says it's kept) */
		if (kept && cr->by_text && !*isnull)
		{
			char	   *text = OutputFunctionCall((FmgrInfo *) &cr->out,
												  xs->tts_values[j]);
			Datum		t = OidInputFunctionCall(cr->in, text, cr->in_param,
												 att->atttypmod);

			kept = datum_image_eq(t, *d, att->attbyval, att->attlen);
		}
done:
		if (cr->guarded)
		{
			ReleaseCurrentSubTransaction();
			MemoryContextSwitchTo(cxt);
			CurrentResourceOwner = owner;
		}
	}
	PG_CATCH();
	{
		ErrorData  *ed;

		if (!cr->guarded)
			PG_RE_THROW();
		MemoryContextSwitchTo(cxt);
		ed = CopyErrorData();
		if (ERRCODE_TO_CATEGORY(ed->sqlerrcode) != ERRCODE_DATA_EXCEPTION)
			PG_RE_THROW();
		FlushErrorState();
		FreeErrorData(ed);
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(cxt);
		CurrentResourceOwner = owner;
		kept = false;
	}
	PG_END_TRY();
	ResetExprContext(econtext);
	return kept;
}

static bool
name_listed(List *names, const char *name)
{
	ListCell   *lc;

	foreach(lc, names)
		if (strcmp(lfirst(lc), name) == 0)
			return true;
	return false;
}

/*
 * The columns of base's a merge's side retyped (another type or typmod,
 * by name), of the table as base, ours and theirs define it (any NULL:
 * none), into *either; those both sides retyped also into *both.  Those
 * whose values a merge reads base's across, which a later retype
 * (mid-resolution) isn't.
 */
static void
retyped_columns(const Table *base, const Table *ours, const Table *theirs,
				List **either, List **both)
{
	List	   *sides[2];
	bool		keyed;
	ListCell   *lc;

	*either = NIL;
	*both = NIL;
	if (base == NULL || ours == NULL || theirs == NULL)
		return;
	sides[0] = version_columns(ours, &keyed);
	sides[1] = version_columns(theirs, &keyed);
	foreach(lc, version_columns(base, &keyed))
	{
		VersionColumn *c = lfirst(lc);
		int			n = 0;

		for (int i = 0; i < 2; i++)
		{
			ListCell   *sc;

			foreach(sc, sides[i])
			{
				VersionColumn *s = lfirst(sc);

				if (strcmp(s->name, c->name) == 0 &&
					(s->type != c->type || s->typmod != c->typmod))
					n++;
			}
		}
		if (n > 0)
			*either = lappend(*either, c->name);
		if (n == 2)
			*both = lappend(*both, c->name);
	}
}

/*
 * A version x of table rel's rows as rel defines them now (now: the
 * version that defines it so), into *out: each row decoded with x's
 * definition and made a row of rel's by column name, a column x hasn't
 * taking its missing value (or null), one of another type or typmod
 * read through its cast as read says (ReadFor; column_read()).  x itself
 * if it defines the table so already, or if now is NULL (the working set
 * has no such table); NULL if x is.
 *
 * Read for comparing (a merge's base), only the columns named in retyped
 * (those a side retyped: retyped_columns()) are; others are read as
 * shown, as the sides' are.  Those in both_retyped, which both sides
 * retyped, are trusted through the cast only where it gives what master
 * compared (see ReadFor).  *unsure (if not NULL) gets the rows (keyed)
 * with a value that's unsure (see ReadFor): a side has changed them,
 * where it has them (merge_table()).  It's left empty otherwise.
 *
 * Where refuse (a merge), refused where rel's rows can't faithfully come
 * from x's: when x lacks a column whose values each row was given as it
 * was added (an identity, a default such as a serial's).  Otherwise
 * (a diff, a merge's conflicts listed or resolved) those show as null.
 * (A column x has that rel hasn't, dropped, or renamed, which reads by
 * name as a drop and an add, is left out: a merge refuses first where
 * that loses a change, refuse_lost_changes().)
 */
static Table *
as_defined_now(Relation rel, const Table *x, const Table *now, ReadFor read,
			   List *retyped, List *both_retyped, bool refuse, Table *out,
			   UnsureRows *unsure)
{
	RowDiff		rd;
	VersionReader vr;
	List	   *now_cols;
	TupleDesc	xd;
	TupleDesc	md = RelationGetDescr(rel);
	const ProllyValueEncoderRoutine *ve = prolly_valenc_lookup("canonical");
	TupleTableSlot *xs;
	TupleTableSlot *ms;
	int		   *from;
	ColumnRead *reads;
	bool	   *converted;
	const ProllyTreeRoutine *tree = prolly_tree_lookup("cow");
	ProllyTreeEnv env;
	ProllyKeyingRefs krefs;
	ProllyTreeCursor *cur;
	ProllyKey	k;
	ProllyValue v;
	ConvRow   **rows = NULL;
	int			n = 0;
	int			max = 0;
	int			i;
	ProllyTreeBuilder *b;
	EState	   *estate = NULL;	/* (casts, and generated columns computed) */
	List	   *generated = NIL;	/* those computed, and their expressions */
	List	   *gen_exprs = NIL;
	bool		keyed_now;
	bool		keys_kept;
	Bitmapset  *pk;
	MemoryContext rowcxt;
	MemoryContext outer;
	int			nunsure = 0;
	int			maxunsure = 0;

	if (unsure)
		memset(unsure, 0, sizeof(UnsureRows));
	if (x == NULL)
		return NULL;
	*out = *x;
	if (now == NULL || same_description(x, now))
		return out;
	require_readable(x);

	rd.kr = keying_of(rel);
	rd.k = PROLLY_CALL(rd.kr, open, rel, CurrentMemoryContext);
	chunk_env(&env);
	env.compare = keying_compare;
	env.compare_arg = &rd;
	prolly_keying_use_env(&krefs, rd.kr, rd.k, tree, &env);
	version_reader(x, &krefs, &vr);
	now_cols = version_columns(now, &keyed_now);

	/*
	 * (a version with a primary key where the table has none now, or the
	 * other way: its rows are keyed otherwise; one whose key the encoder
	 * can't order is keyed by content as the table is)
	 */
	if (vr.keyed != keyed_now)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("rows of table \"%s\" can't be merged across a change of its primary key",
						RelationGetRelationName(rel))));

	xs = vr.slot;
	xd = xs->tts_tupleDescriptor;
	ms = MakeSingleTupleTableSlot(md, &TTSOpsVirtual);

	/* each column of rel's: which of x's it comes from, and how */
	from = palloc_array(int, md->natts);
	reads = palloc0_array(ColumnRead, md->natts);
	converted = palloc0_array(bool, md->natts);
	pk = RelationGetIndexAttrBitmap(rel, INDEX_ATTR_BITMAP_PRIMARY_KEY);
	for (int a = 0; a < md->natts; a++)
	{
		Form_pg_attribute att = TupleDescAttr(md, a);

		from[a] = -1;
		if (att->attisdropped)
			continue;
		for (int j = 0; j < xd->natts; j++)
			if (strcmp(NameStr(TupleDescAttr(xd, j)->attname), NameStr(att->attname)) == 0)
				from[a] = j;
		/*
		 * (a stored generated column added, or one x computed otherwise, is
		 * computed from the rest of each row, as Postgres computed it; a
		 * virtual one isn't stored)
		 */
		if (att->attgenerated == ATTRIBUTE_GENERATED_STORED &&
			(from[a] < 0 ||
			 strcmp(generation_of(vr.cols, NameStr(att->attname)),
					generation_of(now_cols, NameStr(att->attname))) != 0))
		{
			if (estate == NULL)
				estate = CreateExecutorState();
			generated = lappend_int(generated, a);
			gen_exprs = lappend(gen_exprs,
								ExecPrepareExpr((Expr *) build_generation_expression(rel, a + 1),
												estate));
			continue;
		}
		if (from[a] < 0 && att->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL)
			continue;
		if (from[a] < 0 && refuse &&
			(att->attidentity || att->attgenerated ||
			 (att->atthasdef && !att->atthasmissing)))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("rows of table \"%s\" can't be merged across column \"%s\" being added",
							RelationGetRelationName(rel), NameStr(att->attname)),
					 errdetail("Each row was given its own value as the column was added (an identity, a generated column, or a default such as a serial's), which rows from before it don't have.")));
		if (from[a] >= 0 &&
			(TupleDescAttr(xd, from[a])->atttypid != att->atttypid ||
			 TupleDescAttr(xd, from[a])->atttypmod != att->atttypmod))
		{
			if (estate == NULL)
				estate = CreateExecutorState();
			column_read(rel, xd, from[a], att,
						read == READ_COMPARED &&
						!name_listed(retyped, NameStr(att->attname)) ?
						READ_SHOWN : read,
						bms_is_member(a + 1 - FirstLowInvalidHeapAttributeNumber, pk),
						!vr.keyed, name_listed(both_retyped, NameStr(att->attname)),
						estate, &reads[a]);
			converted[a] = true;
		}
	}

	/*
	 * Keys made of columns x has, as x has them, come out as x's did, in its
	 * order: the rows go straight into the tree.  Otherwise they're sorted
	 * first.
	 */
	keys_kept = vr.keyed;
	for (int a = 0; a < md->natts && keys_kept; a++)
		if (bms_is_member(a + 1 - FirstLowInvalidHeapAttributeNumber, pk))
			keys_kept = from[a] >= 0 && !converted[a];

	b = PROLLY_CALL(tree, build_begin, &env, CurrentMemoryContext);
	rowcxt = AllocSetContextCreate(CurrentMemoryContext, "converted row",
								   ALLOCSET_DEFAULT_SIZES);
	cur = PROLLY_CALL(tree, cursor_open, &env, &x->data, CurrentMemoryContext);
	PROLLY_CALL(tree, cursor_seek, cur, NULL, ForwardScanDirection);
	while (PROLLY_CALL(tree, cursor_next, cur, &k, &v))
	{
		ProllyValue row;
		uint64		count;
		ProllyHash	content;
		StringInfoData key;
		StringInfoData crow;
		Bitmapset  *unsure_cols = NULL;

		outer = MemoryContextSwitchTo(rowcxt);
		PROLLY_CALL(rd.kr, parse_value, rd.k, &v, &row, &count);
		ExecClearTuple(xs);
		PROLLY_CALL(ve, decode, vr.enc, &row, xs);
		slot_getallattrs(xs);
		ExecClearTuple(ms);
		for (int a = 0; a < md->natts; a++)
		{
			Form_pg_attribute att = TupleDescAttr(md, a);

			ms->tts_isnull[a] = true;
			if (att->attisdropped)
				continue;
			if (from[a] < 0)
			{
				if (att->atthasmissing)
					ms->tts_values[a] = getmissingattr(md, a + 1, &ms->tts_isnull[a]);
				continue;
			}
			if (xs->tts_isnull[from[a]])
				continue;
			ms->tts_isnull[a] = false;
			if (converted[a] &&
				!read_value(&reads[a], GetPerTupleExprContext(estate), xs,
							from[a], att, TupleDescAttr(xd, from[a]),
							&ms->tts_values[a], &ms->tts_isnull[a]))
			{
				/* (the cast lost the value, or failed) */
				if (reads[a].loss == LOSS_KEEP_STORED)
				{
					ms->tts_isnull[a] = false;
					ms->tts_values[a] = xs->tts_values[from[a]];
				}
				else if (reads[a].loss == LOSS_REFUSE && refuse)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("rows of table \"%s\" can't be merged across column \"%s\" changing type from %s to %s",
									RelationGetRelationName(rel), NameStr(att->attname),
									format_type_with_typemod(TupleDescAttr(xd, from[a])->atttypid,
															 TupleDescAttr(xd, from[a])->atttypmod),
									format_type_with_typemod(att->atttypid, att->atttypmod)),
							 errdetail("A value of the merge base's doesn't convert to the new type as both its cast and its text form say, so which side changed it can't be told, and the table's %s.",
									   vr.keyed ? "key holds it" : "rows have no key")));
				else if (reads[a].loss == LOSS_UNSURE && unsure)
					unsure_cols = bms_add_member(unsure_cols, a);
			}
			else if (!converted[a])
				ms->tts_values[a] = xs->tts_values[from[a]];
		}
		ExecStoreVirtualTuple(ms);
		if (generated != NIL)
		{
			ExprContext *econtext = GetPerTupleExprContext(estate);
			ListCell   *g;
			ListCell   *e;

			econtext->ecxt_scantuple = ms;
			forboth(g, generated, e, gen_exprs)
			{
				int			a = lfirst_int(g);
				Datum		d = ExecEvalExprSwitchContext(lfirst(e), econtext,
														  &ms->tts_isnull[a]);

				ms->tts_values[a] = ms->tts_isnull[a] ? (Datum) 0 :
					datumCopy(d, TupleDescAttr(md, a)->attbyval,
							  TupleDescAttr(md, a)->attlen);
			}
			ResetExprContext(econtext);
		}
		initStringInfo(&key);
		initStringInfo(&crow);
		PROLLY_CALL(rd.kr, encode_row, rd.k, ms, &key, &crow, &content);
		if (unsure_cols != NULL)
		{
			UnsureRow  *u;

			MemoryContextSwitchTo(outer);
			u = palloc(sizeof(UnsureRow));
			u->key.data = dup_bytes(key.data, key.len);
			u->key.len = key.len;
			u->cols = bms_copy(unsure_cols);
			if (nunsure == maxunsure)
			{
				maxunsure = Max(16, maxunsure * 2);
				unsure->rows = unsure->rows ?
					repalloc_array(unsure->rows, UnsureRow *, maxunsure) :
					palloc_array(UnsureRow *, maxunsure);
			}
			unsure->rows[nunsure++] = u;
			MemoryContextSwitchTo(rowcxt);
		}
		if (keys_kept)
		{
			StringInfoData value;
			ProllyKey	ck = {(const uint8 *) key.data, key.len};
			ProllyValue cr = {(const uint8 *) crow.data, crow.len};
			ProllyValue val;

			initStringInfo(&value);
			PROLLY_CALL(rd.kr, make_value, rd.k, &cr, count, &value);
			val.data = (const uint8 *) value.data;
			val.len = value.len;
			PROLLY_CALL(tree, build_add, b, &ck, &val);
			MemoryContextSwitchTo(outer);
		}
		else
		{
			ConvRow    *r;

			MemoryContextSwitchTo(outer);
			r = palloc(sizeof(ConvRow));
			r->key.data = dup_bytes(key.data, key.len);
			r->key.len = key.len;
			r->row.data = dup_bytes(crow.data, crow.len);
			r->row.len = crow.len;
			r->count = count;
			if (n == max)
			{
				max = Max(64, max * 2);
				rows = rows ? repalloc_array(rows, ConvRow *, max) :
					palloc_array(ConvRow *, max);
			}
			rows[n++] = r;
		}
		MemoryContextReset(rowcxt);
		CHECK_FOR_INTERRUPTS();
	}
	PROLLY_CALL(tree, cursor_close, cur);
	if (estate)
		FreeExecutorState(estate);
	if (n > 1)
		qsort_arg(rows, n, sizeof(ConvRow *), conv_row_cmp, &rd);

	for (i = 0; i < n;)
	{
		int			j = i + 1;
		uint64		count = rows[i]->count;
		StringInfoData value;
		ProllyValue val;

		/* (rows that become the same: copies of one, or a key twice) */
		while (j < n && conv_row_cmp(&rows[i], &rows[j], &rd) == 0)
		{
			if (vr.keyed)
				ereport(ERROR,
						(errcode(ERRCODE_UNIQUE_VIOLATION),
						 errmsg("rows of table \"%s\" share a primary key as it is defined now",
								RelationGetRelationName(rel))));
			count += rows[j++]->count;
		}
		initStringInfo(&value);
		PROLLY_CALL(rd.kr, make_value, rd.k, &rows[i]->row, count, &value);
		val.data = (const uint8 *) value.data;
		val.len = value.len;
		PROLLY_CALL(tree, build_add, b, &rows[i]->key, &val);
		pfree(value.data);
		i = j;
	}
	PROLLY_CALL(tree, build_finish, b, &out->data);
	if (nunsure > 1)
		qsort_arg(unsure->rows, nunsure, sizeof(UnsureRow *), unsure_row_cmp, &rd);
	if (unsure)
		unsure->n = nunsure;
	MemoryContextDelete(rowcxt);
	out->text = now->text;
	out->textlen = now->textlen;
	PROLLY_CALL(ve, end, vr.enc);
	ExecDropSingleTupleTableSlot(xs);
	ExecDropSingleTupleTableSlot(ms);
	PROLLY_CALL(rd.kr, close, rd.k);
	return out;
}


/* A column of each of two versions, by attribute index in its reader's slot. */
typedef struct GoneColumn
{
	char	   *name;
	int			base;
	int			side;
	FmgrInfo	base_out;		/* (text forms compared: types differ) */
	FmgrInfo	side_out;
	bool		by_text;
} GoneColumn;

/* refuse_lost_changes()'s diff: base's rows and side's, checked as they come. */
typedef struct LostChanges
{
	Changes		c;				/* (first: changes_compare() reads it) */
	Relation	rel;
	VersionReader br;
	VersionReader sr;
	List	   *gone;			/* GoneColumn * */
	MemoryContext rowcxt;
} LostChanges;

static int
version_attno(VersionReader *vr, const char *name)
{
	TupleDesc	d = vr->slot->tts_tupleDescriptor;

	for (int a = 0; a < d->natts; a++)
		if (strcmp(NameStr(TupleDescAttr(d, a)->attname), name) == 0)
			return a;
	elog(ERROR, "column \"%s\" missing from its version", name);
	return -1;					/* keep compiler quiet */
}

/* A tree value's row into a version reader's slot. */
static void
decode_version_row(LostChanges *lc, VersionReader *vr, const ProllyValue *v)
{
	const ProllyValueEncoderRoutine *ve = prolly_valenc_lookup("canonical");
	ProllyValue row;
	uint64		count;

	PROLLY_CALL(lc->c.rd.kr, parse_value, lc->c.rd.k, v, &row, &count);
	ExecClearTuple(vr->slot);
	PROLLY_CALL(ve, decode, vr->enc, &row, vr->slot);
	slot_getallattrs(vr->slot);
}

static void
check_lost_change(ProllyDiffKind kind, const ProllyKey *key,
				  const ProllyValue *before, const ProllyValue *after, void *arg)
{
	LostChanges *lc = arg;
	MemoryContext old;
	ListCell   *g;

	/* (a row side added or deleted loses the column as every row does) */
	if (before == NULL || after == NULL)
		return;
	old = MemoryContextSwitchTo(lc->rowcxt);
	decode_version_row(lc, &lc->br, before);
	decode_version_row(lc, &lc->sr, after);
	foreach(g, lc->gone)
	{
		GoneColumn *gc = lfirst(g);
		TupleTableSlot *bs = lc->br.slot;
		TupleTableSlot *ss = lc->sr.slot;
		Form_pg_attribute att = TupleDescAttr(bs->tts_tupleDescriptor, gc->base);
		bool		same;

		if (bs->tts_isnull[gc->base] || ss->tts_isnull[gc->side])
			same = bs->tts_isnull[gc->base] && ss->tts_isnull[gc->side];
		else if (gc->by_text)
			same = strcmp(OutputFunctionCall(&gc->base_out, bs->tts_values[gc->base]),
						  OutputFunctionCall(&gc->side_out, ss->tts_values[gc->side])) == 0;
		else
			same = datumIsEqual(bs->tts_values[gc->base], ss->tts_values[gc->side],
								att->attbyval, att->attlen);
		if (!same)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("rows of table \"%s\" can't be merged across column \"%s\" being dropped or renamed",
							RelationGetRelationName(lc->rel), gc->name),
					 errdetail("One side changed values of \"%s\", which the other dropped or renamed: the changes would be lost.",
							   gc->name)));
	}
	MemoryContextSwitchTo(old);
	MemoryContextReset(lc->rowcxt);
	CHECK_FOR_INTERRUPTS();
}

/*
 * For a merge: refused where side changed, in a row base had, the value of
 * a column the table, as merged, hasn't: one the other side dropped, or
 * renamed (a rename reads, by name, as a drop and an add).  The change
 * would be lost: refused, not lost.  (Rows side added lose the column as
 * every row does.)  The rows are checked as the diff finds them, the
 * first change lost refused.  Versions keyed otherwise than the table now
 * are left to as_defined_now(), which refuses them.
 */
static void
refuse_lost_changes(Relation rel, const Table *base, const Table *side,
					const Table *now)
{
	const ProllyTreeRoutine *tree = prolly_tree_lookup("cow");
	bool		keyed_base;
	bool		keyed_side;
	bool		keyed_now;
	List	   *base_cols;
	List	   *now_cols;
	List	   *gone = NIL;
	ListCell   *lc;
	LostChanges lost;
	ProllyTreeEnv env;
	ProllyKeyingRefs krefs;

	if (base == NULL || side == NULL || now == NULL ||
		same_hash(&base->data.hash, &side->data.hash) ||
		same_description(side, now))
		return;
	base_cols = version_columns(base, &keyed_base);
	now_cols = version_columns(now, &keyed_now);
	foreach(lc, version_columns(side, &keyed_side))
	{
		VersionColumn *sc = lfirst(lc);
		bool		in_base = false;
		bool		in_now = false;
		ListCell   *l2;

		foreach(l2, base_cols)
			in_base |= strcmp(((VersionColumn *) lfirst(l2))->name, sc->name) == 0;
		foreach(l2, now_cols)
			in_now |= strcmp(((VersionColumn *) lfirst(l2))->name, sc->name) == 0;
		if (in_base && !in_now)
			gone = lappend(gone, sc->name);
	}
	if (gone == NIL || keyed_base != keyed_now || keyed_side != keyed_now)
		return;
	require_readable(base);
	require_readable(side);

	memset(&lost, 0, sizeof(lost));
	lost.rel = rel;
	lost.c.rd.kr = keying_of(rel);
	lost.c.rd.k = PROLLY_CALL(lost.c.rd.kr, open, rel, CurrentMemoryContext);
	chunk_env(&env);
	env.compare = changes_compare;
	env.compare_arg = &lost.c;
	prolly_keying_use_env(&krefs, lost.c.rd.kr, lost.c.rd.k, tree, &env);
	version_reader(base, &krefs, &lost.br);
	version_reader(side, &krefs, &lost.sr);
	foreach(lc, gone)
	{
		GoneColumn *gc = palloc0(sizeof(GoneColumn));
		Oid			bt;
		Oid			st;

		gc->name = lfirst(lc);
		gc->base = version_attno(&lost.br, gc->name);
		gc->side = version_attno(&lost.sr, gc->name);
		bt = TupleDescAttr(lost.br.slot->tts_tupleDescriptor, gc->base)->atttypid;
		st = TupleDescAttr(lost.sr.slot->tts_tupleDescriptor, gc->side)->atttypid;
		gc->by_text = bt != st;
		if (gc->by_text)
		{
			Oid			outf;
			bool		varlena;

			getTypeOutputInfo(bt, &outf, &varlena);
			fmgr_info(outf, &gc->base_out);
			getTypeOutputInfo(st, &outf, &varlena);
			fmgr_info(outf, &gc->side_out);
		}
		lost.gone = lappend(lost.gone, gc);
	}
	lost.rowcxt = AllocSetContextCreate(CurrentMemoryContext, "lost change check",
										ALLOCSET_DEFAULT_SIZES);
	PROLLY_CALL(tree, diff, &env, &base->data, &side->data, check_lost_change, &lost);
	MemoryContextDelete(lost.rowcxt);
	PROLLY_CALL(lost.c.rd.kr, close, lost.c.rd.k);
}


/*
 * The rows of table relid that differ between two versions of it, each as
 * the table is defined now (now: the working set's version), so that rows
 * a version wrote with other columns read as the table's rows do.
 */
static void
diff_rows_now(Oid relid, const Table *from, const Table *to, const Table *now,
			  ProllyDiffSink *sink)
{
	Relation	rel;
	Table		f;
	Table		t;
	const Table *fp;
	const Table *tp;

	/* (nothing changed: nothing to convert) */
	if (from && to && same_hash(&from->data.hash, &to->data.hash) &&
		same_description(from, to))
		return;
	if (now == NULL || ((from == NULL || same_description(from, now)) &&
						(to == NULL || same_description(to, now))))
	{
		diff_rows(relid, from, to, sink);
		return;
	}
	rel = table_open(relid, AccessShareLock);
	fp = as_defined_now(rel, from, now, READ_SHOWN, NIL, NIL, false, &f, NULL);
	tp = as_defined_now(rel, to, now, READ_SHOWN, NIL, NIL, false, &t, NULL);
	table_close(rel, AccessShareLock);
	diff_rows(relid, fp, tp, sink);
}

/* What a merge across definitions writes, once they're merged. */
typedef struct MergeRows
{
	List	   *base;
	List	   *ours;
	List	   *theirs;
	bool		fail_on_conflict;
	const char *what;
	const char *name;			/* the branch merged, or NULL */
	List	  **conflicted;
	int			conflicts;
	List	   *now;			/* the tables as the branch has them, if
								 * known (NIL: folded when needed) */
} MergeRows;


/* What failed, for an error: a merge of branch name, or what (a pick). */
static char *
op_label(const char *what, const char *name)
{
	return name ? psprintf("%s of branch \"%s\"", what, name) : pstrdup(what);
}

static void
cannot_merge_rows(const char *what, const char *name)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("%s can't combine the rows of table %s yet", what, name),
			 errdetail("One side changed its definition, or dropped it, and the other its rows.")));
}

/* A table's data tree with no rows, defined as like. */
static void
no_rows(const Table *like, Table *out)
{
	ProllyTreeEnv env;
	const ProllyTreeRoutine *tree = prolly_tree_lookup("cow");
	ProllyTreeBuilder *b;

	*out = *like;
	chunk_env(&env);
	b = PROLLY_CALL(tree, build_begin, &env, CurrentMemoryContext);
	PROLLY_CALL(tree, build_finish, b, &out->data);
}

static void
row_conflicts(MergeRows *mr, Oid relid, int n)
{
	if (n > 0 && mr->fail_on_conflict)
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("%s conflicts in table \"%s\"",
						op_label(mr->what, mr->name), get_rel_name(relid)),
				 errdetail_plural("%d row was changed differently on both sides.",
								  "%d rows were changed differently on both sides.",
								  n, n)));
	if (n > 0)
	{
		mr->conflicts += n;
		*mr->conflicted = lappend_oid(*mr->conflicted, relid);
	}
}

/*
 * Each table's rows, after the definitions are merged (or alike): three-way
 * where both sides define it alike (against no rows, for a table both
 * made); else the rows of the side whose definition is the merged one, if
 * the other didn't change them.  The rows each table has now (mr->now, or
 * the branch folded) are made the rows it should have, whatever the
 * definitions' merge did to it (a table made again is empty).
 */
static void
merge_rows(void *arg)
{
	MergeRows  *mr = arg;
	List	   *now = mr->now;
	List	   *names = NIL;
	ListCell   *lc;

	if (now == NIL)
	{
		ProllyNodeRef working;
		Tables		ts = {NIL};

		(void) PROLLY_CALL(components().refs, working_set, NULL, &working);
		PROLLY_CALL(components().canonicalizer, tables, &working, add_table, &ts);
		now = ts.tables;
	}
	foreach(lc, mr->ours)
		names = lappend(names, ((Table *) lfirst(lc))->name);
	foreach(lc, mr->theirs)
		if (!find_table(mr->ours, ((Table *) lfirst(lc))->name))
			names = lappend(names, ((Table *) lfirst(lc))->name);
	foreach(lc, names)
	{
		char	   *name = lfirst(lc);
		Table	   *b = find_table(mr->base, name);
		Table	   *o = find_table(mr->ours, name);
		Table	   *t = find_table(mr->theirs, name);
		Table	   *w = find_table(now, name);
		bool		o_rows = b ? !o || !same_hash(&o->data.hash, &b->data.hash) : o != NULL;
		bool		t_rows = b ? !t || !same_hash(&t->data.hash, &b->data.hash) : t != NULL;
		const Table *want;
		Oid			relid;

		if (w == NULL)
		{
			/* the merged definitions drop it: unless its rows changed */
			if ((o && o_rows && b) || (t && t_rows && b))
				cannot_merge_rows(op_label(mr->what, mr->name), name);
			continue;
		}
		relid = table_named(name);
		if (t == NULL || o == NULL)
		{
			/* one side dropped it (its rows changed: the other's stay) */
			if (b && ((t == NULL && o_rows) || (o == NULL && t_rows)))
				cannot_merge_rows(op_label(mr->what, mr->name), name);
			want = t ? t : o;
		}
		else if (same_description(o, t) && (b == NULL || same_description(b, o)))
		{
			Table		none;

			if (b == NULL)
			{
				no_rows(o, &none);
				b = &none;
			}
			if (!t_rows)
				want = o;
			else if (!o_rows)
				want = t;
			else
			{
				/* (the branch has ours' rows: same definitions, unmoved) */
				if (!same_hash(&w->data.hash, &o->data.hash))
					apply_rows(relid, w, o);
				row_conflicts(mr, relid,
							  merge_table(relid, b, o, t, WALK_MERGE, NULL, NULL,
										  NULL));
				continue;
			}
		}
		else if (b != NULL)
		{
			/*
			 * Defined otherwise somewhere: the rows of all three as the
			 * merged definition has them, merged three-way (the branch's
			 * made ours' first: a table made again has none).
			 */
			Relation	rel;
			Table		bn;
			Table		on;
			Table		tn;
			UnsureRows	unsure;
			List	   *retyped;
			List	   *both_retyped;

			/*
			 * (one side's rows alone, defined as merged already: they're
			 * the rows, as they'd be had it changed nothing else)
			 */
			if (!o_rows && same_description(t, w))
			{
				if (!same_hash(&w->data.hash, &t->data.hash))
					apply_rows(relid, w, t);
				continue;
			}
			if (!t_rows && same_description(o, w))
			{
				if (!same_hash(&w->data.hash, &o->data.hash))
					apply_rows(relid, w, o);
				continue;
			}
			rel = table_open(relid, AccessShareLock);
			refuse_lost_changes(rel, b, o, w);
			refuse_lost_changes(rel, b, t, w);
			retyped_columns(b, o, t, &retyped, &both_retyped);
			(void) as_defined_now(rel, b, w, READ_COMPARED, retyped, both_retyped,
								  true, &bn, &unsure);
			(void) as_defined_now(rel, o, w, READ_WRITTEN, NIL, NIL, true, &on, NULL);
			(void) as_defined_now(rel, t, w, READ_WRITTEN, NIL, NIL, true, &tn, NULL);
			table_close(rel, AccessShareLock);
			if (!same_hash(&w->data.hash, &on.data.hash))
				apply_rows(relid, w, &on);
			/* (a side as base, unless base's rows have unsure values) */
			if (unsure.n == 0 && same_hash(&tn.data.hash, &bn.data.hash))
				continue;
			if (unsure.n == 0 && same_hash(&on.data.hash, &bn.data.hash))
				apply_rows(relid, &on, &tn);
			else
				row_conflicts(mr, relid,
							  merge_table(relid, &bn, &on, &tn, WALK_MERGE, NULL, NULL,
										  &unsure));
			continue;
		}
		else
		{
			/* both made it, defined otherwise: one side's rows, if only one has any */
			if (o_rows && t_rows)
				cannot_merge_rows(op_label(mr->what, mr->name), name);
			want = t_rows ? t : o;
		}
		if (!same_hash(&w->data.hash, &want->data.hash))
			apply_rows(relid, w, want);
	}
}

/*
 * What a commit needs from outside the versioned schemas that since (NULL:
 * nothing) didn't, checked before a move (what) brings it in.
 */
static void
check_needs(const ProllyHash *commit, const ProllyHash *since, const char *what)
{
	ProllyCommitInfo info;
	ProllyCommitInfo before;

	if (!PROLLY_CALL(components().refs, commit_get, commit, &info) ||
		(since && !PROLLY_CALL(components().refs, commit_get, since, &before)))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly commit is missing")));
	prolly_check_commit_dependencies(&info, since ? &before : NULL, what);
}

/* A commit's canonical root. */
static void
commit_root(const ProllyHash *id, ProllyNodeRef *root)
{
	ProllyCommitInfo info;

	if (!PROLLY_CALL(components().refs, commit_get, id, &info))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly commit is missing")));
	*root = info.root;
}

static List *
tables_in_root(const ProllyNodeRef *root)
{
	Tables		ts = {NIL};

	PROLLY_CALL(components().canonicalizer, tables, root, add_table, &ts);
	return ts.tables;
}

static List *
tables_at(const ProllyHash *id)
{
	ProllyNodeRef root;

	commit_root(id, &root);
	return tables_in_root(&root);
}

/*
 * The working set holds something of its own: an object other than a
 * versioned schema (a branch with no commit has nothing else to lose).
 */
static bool
working_has_objects(void)
{
	const ProllyTreeRoutine *tree = prolly_tree_lookup("cow");
	ProllyTreeEnv env;
	ProllyNodeRef working;
	ProllyTreeCursor *c;
	ProllyKey	k;
	ProllyValue v;
	bool		found = false;

	root_of("WORKING", &working);
	chunk_env(&env);
	env.compare = prolly_bytes_compare;
	c = PROLLY_CALL(tree, cursor_open, &env, &working, CurrentMemoryContext);
	PROLLY_CALL(tree, cursor_seek, c, NULL, ForwardScanDirection);
	while (!found && PROLLY_CALL(tree, cursor_next, c, &k, &v))
		found = !(k.len >= 2 && memcmp(k.data, "n:", 2) == 0);
	PROLLY_CALL(tree, cursor_close, c);
	return found;
}

/* Move the session's branch to commit id (not to a name, which may move). */
static void
reset_to(const ProllyHash *id)
{
	ProllyDatabaseComponents dc = components();
	char	   *hex = palloc(id->len * 2 + 1);

	hex_encode((const char *) id->bytes, id->len, hex);
	hex[id->len * 2] = '\0';
	PROLLY_CALL(dc.refs, reset, hex);
	pfree(hex);
}

/* what: the operation refused ("merge", "cherry-pick", "revert") */
static void
refuse_local_changes(const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("cannot %s with uncommitted changes", what),
			 errhint("Commit them, or reset the branch, first.")));
}

/*
 * The branch's definitions made the merged ones, its rows merged as mr
 * says.  What one side dropped that the other's new objects use (a
 * function a view calls, a table a foreign key points at) fails there: a
 * schema conflict, as is an object both changed.
 */
static void
move_schema_merging(const ProllyNodeRef *ours, const ProllyNodeRef *merged,
					MergeRows *mr)
{
	MemoryContext cxt = CurrentMemoryContext;

	PG_TRY();
	{
		PROLLY_CALL(components().canonicalizer, move_schema, ours, merged,
					merge_rows, take_breaking_rows, mr, false);
	}
	PG_CATCH();
	{
		ErrorData  *ed;

		MemoryContextSwitchTo(cxt);
		ed = CopyErrorData();
		switch (ed->sqlerrcode)
		{
			case ERRCODE_UNDEFINED_FUNCTION:
			case ERRCODE_UNDEFINED_TABLE:
			case ERRCODE_UNDEFINED_OBJECT:
			case ERRCODE_UNDEFINED_COLUMN:
			case ERRCODE_UNDEFINED_SCHEMA:
			case ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST:
				break;
			default:
				PG_RE_THROW();
		}
		FlushErrorState();
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("schema conflict: the merged definitions don't fit together"),
				 errdetail("Merged, one side's objects use what the other dropped: %s",
						   ed->message)));
	}
	PG_END_TRY();
}

/*
 * Write to the session's branch (its rows as roots[1] has them) what
 * changed from roots[0] to roots[2], three-way: what (a merge of branch
 * name, a cherry-pick) fails if a row conflicts and fail_on_conflict, else
 * conflicted gets the tables with conflicts.  Returns how many keys
 * conflict.
 */
static int
merge_roots(const ProllyNodeRef *roots, bool fail_on_conflict,
			const char *what, const char *name, List **conflicted)
{
	ProllyDatabaseComponents dc = components();
	List	   *base_tables;
	List	   *ours_tables;
	List	   *theirs_tables;

	base_tables = tables_in_root(&roots[0]);
	ours_tables = tables_in_root(&roots[1]);
	theirs_tables = tables_in_root(&roots[2]);

	/* Definitions changed: merged first, then the rows as they allow. */
	if (!PROLLY_CALL(dc.canonicalizer, same_schema, &roots[0], &roots[1]) ||
		!PROLLY_CALL(dc.canonicalizer, same_schema, &roots[0], &roots[2]))
	{
		MergeRows	mr = {base_tables, ours_tables, theirs_tables,
		fail_on_conflict, what, name, conflicted, 0, NIL};
		ProllyNodeRef merged;

		PROLLY_CALL(dc.canonicalizer, merge_schema, &roots[0], &roots[1],
					&roots[2], &merged);
		move_schema_merging(&roots[1], &merged, &mr);
		merge_sequences(&roots[0], &roots[1], &roots[2]);
		return mr.conflicts;
	}
	else
	{
		/* (the branch has ours' rows, every table defined alike) */
		MergeRows	mr = {base_tables, ours_tables, theirs_tables,
		fail_on_conflict, what, name, conflicted, 0, ours_tables};

		merge_sequences(&roots[0], &roots[1], &roots[2]);
		merge_rows(&mr);
		return mr.conflicts;
	}
}

/*
 * "ONLY " for a table a foreign key covers alone; nothing for a
 * partitioned one, whose partitions' rows are its own (as the foreign
 * key's checks read them).
 */
static const char *
only_unless_partitioned(Oid relid)
{
	return get_rel_relkind(relid) == RELKIND_PARTITIONED_TABLE ? "" : "ONLY ";
}

/* The owner of relation relid. */
static Oid
rel_owner(Oid relid)
{
	HeapTuple	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	Oid			owner;

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	owner = ((Form_pg_class) GETSTRUCT(tup))->relowner;
	ReleaseSysCache(tup);
	return owner;
}

/*
 * The rows of the session branch's tables of relids (NIL: every one) that
 * break a foreign key, theirs or one referencing them, as violations: a
 * row whose key, all of it set (MATCH SIMPLE: any of it null passes), or,
 * MATCH FULL, some of it set, has no referenced row.  With changed (the
 * rows changed since the head, changed_since_head()'s; NIL: every row),
 * only the rows changed, and those referencing a row the referenced table
 * removed or changed, unless either table is checked whole.
 */
static List *
foreign_key_violations(List *relids, List *changed)
{
	Relation	pg_constraint = table_open(ConstraintRelationId, AccessShareLock);
	SysScanDesc scan = systable_beginscan(pg_constraint, InvalidOid, false,
										  NULL, 0, NULL);
	HeapTuple	tup;
	List	   *found = NIL;
	List	   *queries = NIL;

	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tup);
		bool		isnull;
		Datum		d;
		ArrayType  *ck;
		ArrayType  *pk;
		int16	   *cks;
		int16	   *pks;
		int			n;
		StringInfoData q;
		StringInfoData set;
		StringInfoData any;
		StringInfoData eq;
		StringInfoData info;
		char	   *child;
		char	   *parent;
		List	   *child_now = NIL;
		List	   *parent_was = NIL;
		List	   *qi;

		/* (a partition's clone of its parent's is checked as the parent's) */
		if (con->contype != CONSTRAINT_FOREIGN || OidIsValid(con->conparentid) ||
			!namespace_is_versioned(get_rel_namespace(con->conrelid)) ||
			(relids != NIL && !list_member_oid(relids, con->conrelid) &&
			 !list_member_oid(relids, con->confrelid)))
			continue;
		if (changed != NIL)
		{
			bool		whole;

			whole = changed_rows_of(changed, con->conrelid, &child_now, NULL);
			whole |= changed_rows_of(changed, con->confrelid, NULL, &parent_was);
			if (whole)
				child_now = parent_was = NIL;
			else if (child_now == NIL && parent_was == NIL)
				continue;
		}
		d = heap_getattr(tup, Anum_pg_constraint_conkey,
						 RelationGetDescr(pg_constraint), &isnull);
		ck = DatumGetArrayTypeP(d);
		d = heap_getattr(tup, Anum_pg_constraint_confkey,
						 RelationGetDescr(pg_constraint), &isnull);
		pk = DatumGetArrayTypeP(d);
		n = ARR_DIMS(ck)[0];
		cks = (int16 *) ARR_DATA_PTR(ck);
		pks = (int16 *) ARR_DATA_PTR(pk);
		child = table_name(con->conrelid);
		parent = table_name(con->confrelid);
		initStringInfo(&set);
		initStringInfo(&any);
		initStringInfo(&eq);
		initStringInfo(&info);
		appendStringInfo(&info, "{\"ForeignKey\": %s, \"Table\": %s, \"ReferencedTable\": %s, \"Columns\": [",
						 quote_json(NameStr(con->conname)), quote_json(child),
						 quote_json(parent));
		for (int i = 0; i < n; i++)
		{
			const char *c = quote_identifier(get_attname(con->conrelid, cks[i], false));
			const char *p = quote_identifier(get_attname(con->confrelid, pks[i], false));

			appendStringInfo(&set, "%sc.%s IS NOT NULL", i ? " AND " : "", c);
			appendStringInfo(&any, "%sc.%s IS NOT NULL", i ? " OR " : "", c);
			appendStringInfo(&eq, "%sp.%s = c.%s", i ? " AND " : "", p, c);
			appendStringInfo(&info, "%s%s", i ? ", " : "",
							 quote_json(get_attname(con->conrelid, cks[i], false)));
		}
		appendStringInfoString(&info, "], \"ReferencedColumns\": [");
		for (int i = 0; i < n; i++)
			appendStringInfo(&info, "%s%s", i ? ", " : "",
							 quote_json(get_attname(con->confrelid, pks[i], false)));
		appendStringInfoString(&info, "]}");
		initStringInfo(&q);
		appendStringInfo(&q,
						 "SELECT row_to_json(c.*)::text FROM %s%s c WHERE "
						 "NOT EXISTS (SELECT 1 FROM %s%s p WHERE %s) AND ",
						 only_unless_partitioned(con->conrelid), child,
						 only_unless_partitioned(con->confrelid), parent,
						 eq.data);
		if (con->confmatchtype == FKCONSTR_MATCH_FULL)
			appendStringInfo(&q, "(%s)", any.data);
		else
			appendStringInfo(&q, "(%s)", set.data);
		qi = list_make3(q.data, info.data, (void *) (uintptr_t) con->conrelid);
		if (child_now == NIL && parent_was == NIL)
		{
			queries = lappend(queries, qi);
			continue;
		}
		/* (the rows changed, by key, and those referencing a parent's row was) */
		if (child_now != NIL)
		{
			StringInfoData cols;
			StringInfoData defs;

			initStringInfo(&cols);
			initStringInfo(&defs);
			if (!primary_key_columns(con->conrelid, "c", &cols, &defs))
			{
				queries = lappend(queries, qi); /* (no key: whole) */
				continue;
			}
			key_queries(qi, child_now, cols.data, defs.data, &queries);
		}
		if (parent_was != NIL)
		{
			StringInfoData cols;
			StringInfoData defs;
			StringInfoData unused;

			initStringInfo(&cols);
			initStringInfo(&defs);
			initStringInfo(&unused);
			key_columns(con->conrelid, cks, n, "c", &cols, &unused);
			resetStringInfo(&unused);
			key_columns(con->confrelid, pks, n, "w", &unused, &defs);
			key_queries(qi, parent_was, cols.data, defs.data, &queries);
		}
	}
	systable_endscan(scan);
	table_close(pg_constraint, AccessShareLock);

	violating_rows(queries, "foreign key", false, &found);
	return distinct_violations(found);
}

/*
 * Run each query (sql, info, relid), appending a violation of type for each
 * row it returns (as JSON) to *found: with a snapshot that sees what was
 * written before (a merge's writes), as the table's owner, its rows and
 * those it references seen whatever their row security says, as a foreign
 * key's own checks see them.  Removing: the queries delete the rows they
 * return.
 */
static void
violating_rows(List *queries, const char *type, bool removing, List **found)
{
	MemoryContext cxt = CurrentMemoryContext;
	ListCell   *lc;

	if (queries == NIL)
		return;
	CommandCounterIncrement();
	SPI_connect();
	foreach(lc, queries)
	{
		List	   *qi = lfirst(lc);
		Oid			relid = (Oid) (uintptr_t) lthird(qi);
		Oid			save_userid;
		int			save_sec;
		int			ret;

		INJECTION_POINT("prolly-merge-foreign-keys", NULL);
		GetUserIdAndSecContext(&save_userid, &save_sec);
		SetUserIdAndSecContext(rel_owner(relid),
							   save_sec | SECURITY_LOCAL_USERID_CHANGE |
							   SECURITY_NOFORCE_RLS);
		ret = SPI_execute(linitial(qi), false, 0);
		SetUserIdAndSecContext(save_userid, save_sec);
		if (ret != (removing ? SPI_OK_DELETE_RETURNING : SPI_OK_SELECT))
			elog(ERROR, "could not check a constraint: %s", (char *) linitial(qi));
		for (uint64 r = 0; r < SPI_processed; r++)
		{
			char	   *row = SPI_getvalue(SPI_tuptable->vals[r],
										   SPI_tuptable->tupdesc, 1);
			MemoryContext spi = MemoryContextSwitchTo(cxt);

			*found = lappend(*found, violation(relid, type, pstrdup(row),
											   pstrdup(lsecond(qi))));
			MemoryContextSwitchTo(spi);
		}
	}
	SPI_finish();
}

/*
 * The query (sql, info, relid) finding the rows of con's table that break
 * it, a check or not-null constraint, as JSON (removing: deleting them);
 * *check: whether it's a check.
 */
static List *
breaking_query(Relation pg_constraint, HeapTuple tup, bool removing,
			   bool *check)
{
	Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tup);
	char	   *table = table_name(con->conrelid);
	StringInfoData q;
	StringInfoData info;
	char	   *cond;

	initStringInfo(&q);
	initStringInfo(&info);
	*check = con->contype == CONSTRAINT_CHECK;
	if (*check)
	{
		bool		isnull;
		Datum		bin = heap_getattr(tup, Anum_pg_constraint_conbin,
									   RelationGetDescr(pg_constraint), &isnull);
		char	   *expr = TextDatumGetCString(DirectFunctionCall2(pg_get_expr, bin,
																   ObjectIdGetDatum(con->conrelid)));

		cond = psprintf("NOT (%s)", expr);
		appendStringInfo(&info, "{\"Name\": %s, \"Expression\": %s}",
						 quote_json(NameStr(con->conname)), quote_json(expr));
	}
	else
	{
		AttrNumber	col = extractNotNullColumn(tup);
		char	   *name = get_attname(con->conrelid, col, false);

		cond = psprintf("c.%s IS NULL", quote_identifier(name));
		appendStringInfo(&info, "{\"Columns\": [%s]}", quote_json(name));
	}
	if (removing)
		appendStringInfo(&q, "DELETE FROM %s%s c WHERE %s RETURNING row_to_json(c.*)::text",
						 only_unless_partitioned(con->conrelid), table, cond);
	else
		appendStringInfo(&q, "SELECT row_to_json(c.*)::text FROM %s%s c WHERE %s",
						 only_unless_partitioned(con->conrelid), table, cond);
	return list_make3(q.data, info.data, (void *) (uintptr_t) con->conrelid);
}

/*
 * The rows of the tables of relids (NIL: every versioned one) that break
 * a check or not-null constraint not validated yet: a table holds none
 * that break one validated, Postgres checks every row written against it.
 * With changed (as foreign_key_violations() takes it), only the rows
 * added or changed since the head, unless the table is checked whole.
 */
static List *
unvalidated_violations(List *relids, List *changed)
{
	Relation	pg_constraint = table_open(ConstraintRelationId, AccessShareLock);
	SysScanDesc scan = systable_beginscan(pg_constraint, InvalidOid, false,
										  NULL, 0, NULL);
	HeapTuple	tup;
	List	   *checks = NIL;
	List	   *notnulls = NIL;
	List	   *found = NIL;

	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tup);
		List	   *qi;
		List	   *now = NIL;
		bool		check;

		/*
		 * (a partition's inherited one is checked as its partitioned
		 * table's, whose query reads every partition)
		 */
		if ((con->contype != CONSTRAINT_CHECK && con->contype != CONSTRAINT_NOTNULL) ||
			con->convalidated || !OidIsValid(con->conrelid) ||
			OidIsValid(con->conparentid) ||
			(!con->conislocal && get_rel_relispartition(con->conrelid)) ||
			!namespace_is_versioned(get_rel_namespace(con->conrelid)) ||
			(relids != NIL && !list_member_oid(relids, con->conrelid)))
			continue;
		if (changed != NIL)
		{
			if (changed_rows_of(changed, con->conrelid, &now, NULL))
				now = NIL;		/* (whole) */
			else if (now == NIL)
				continue;
		}
		qi = breaking_query(pg_constraint, tup, false, &check);
		if (now != NIL)
		{
			StringInfoData cols;
			StringInfoData defs;
			List	   *batches = NIL;

			initStringInfo(&cols);
			initStringInfo(&defs);
			/* (the rows changed, by key; no key: whole) */
			if (primary_key_columns(con->conrelid, "c", &cols, &defs))
				key_queries(qi, now, cols.data, defs.data, &batches);
			else
				batches = list_make1(qi);
			if (check)
				checks = list_concat(checks, batches);
			else
				notnulls = list_concat(notnulls, batches);
			continue;
		}
		if (check)
			checks = lappend(checks, qi);
		else
			notnulls = lappend(notnulls, qi);
	}
	systable_endscan(scan);
	table_close(pg_constraint, AccessShareLock);
	violating_rows(checks, "check constraint", false, &found);
	violating_rows(notnulls, "not null", false, &found);
	return found;
}

/*
 * Mark foreign key conoid validated, and the constraints Postgres made for
 * it (those for a partitioned table's partitions, referencing or
 * referenced), as VALIDATE CONSTRAINT would.
 */
static void
mark_foreign_key_validated(Relation pg_constraint, Oid conoid)
{
	ScanKeyData skey;
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *clones = NIL;
	ListCell   *lc;

	tup = SearchSysCacheCopy1(CONSTROID, ObjectIdGetDatum(conoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for constraint %u", conoid);
	((Form_pg_constraint) GETSTRUCT(tup))->convalidated = true;
	CatalogTupleUpdate(pg_constraint, &tup->t_self, tup);
	heap_freetuple(tup);

	ScanKeyInit(&skey, Anum_pg_constraint_conparentid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(conoid));
	scan = systable_beginscan(pg_constraint, ConstraintParentIndexId, true,
							  NULL, 1, &skey);
	while ((tup = systable_getnext(scan)) != NULL)
		clones = lappend_oid(clones, ((Form_pg_constraint) GETSTRUCT(tup))->oid);
	systable_endscan(scan);
	foreach(lc, clones)
		mark_foreign_key_validated(pg_constraint, lfirst_oid(lc));
}

/*
 * A merge's move's breaking(): the rows of its tables that break the
 * checks and NOT NULLs it added (unvalidated, ProllyAddedConstraint *)
 * taken away and recorded as violations, as the rows it couldn't write
 * are, so that the constraints validate.  The rows breaking one may be the
 * branch's own as well as those merged: Dolt records them either way.  (No
 * trigger fires for the rows taken away, as none does for those written.)
 * A foreign key it added is marked validated, its table's rows checked
 * against it as the merge checks every foreign key of the tables it wrote:
 * those breaking it stay, recorded.
 */
static void
take_breaking_rows(List *added, void *arg)
{
	Relation	pg_constraint;
	List	   *checks = NIL;
	List	   *notnulls = NIL;
	List	   *found = NIL;
	ListCell   *lc;
	int			save_nestlevel;
	MemoryContext old;

	Assert(recording);
	pg_constraint = table_open(ConstraintRelationId, RowExclusiveLock);
	foreach(lc, added)
	{
		ProllyAddedConstraint *ac = lfirst(lc);
		Oid			relid = rel_named(ac->table, false);
		char	   *name = strVal(linitial(stringToQualifiedNameList(ac->name, NULL)));
		Oid			conoid = get_relation_constraint_oid(relid, name, false);
		HeapTuple	tup;
		List	   *qi;
		bool		check;

		note_written(relid);
		if (ac->foreign_key)
		{
			mark_foreign_key_validated(pg_constraint, conoid);
			continue;
		}
		tup = SearchSysCacheCopy1(CONSTROID, ObjectIdGetDatum(conoid));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for constraint %u", conoid);
		qi = breaking_query(pg_constraint, tup, true, &check);
		heap_freetuple(tup);
		if (check)
			checks = lappend(checks, qi);
		else
			notnulls = lappend(notnulls, qi);
	}
	table_close(pg_constraint, RowExclusiveLock);
	CommandCounterIncrement();
	save_nestlevel = NewGUCNestLevel();
	(void) set_config_option("session_replication_role", "replica", PGC_SUSET,
							 PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	old = MemoryContextSwitchTo(TopTransactionContext);
	violating_rows(checks, "check constraint", true, &found);
	violating_rows(notnulls, "not null", true, &found);
	recorded = list_concat(recorded, found);
	MemoryContextSwitchTo(old);
	AtEOXact_GUC(true, save_nestlevel);
}

/*
 * The rows of the session branch's tables changed since its head, by
 * table, each as its key: a JSON object of the columns rows are matched
 * by (its primary key's, and those a foreign key references), each value
 * as its type's output gives it, which json_to_recordset() reads back to match the table's rows by
 * key.  Those added or changed as they are now, and those removed or
 * changed as they were.  (A keyless row's copies added are a row now, its
 * copies removed one that was.)  Only key columns are read, so a row
 * matches whatever its other columns hold or how they print.  A table is
 * checked whole where rows can't be matched so: one made or redefined
 * since (its constraints may be new), one with violations recorded (each
 * is found again or not, whatever its row is now), and one with neither
 * a primary key nor columns a foreign key references.
 */
typedef struct ChangedTable
{
	Oid			relid;
	List	   *ancestors;		/* its partitioned tables', if a partition */
	bool		whole;			/* every row is to be checked */
	List	   *now;			/* char *, keys as JSON objects */
	List	   *was;			/* char * */
} ChangedTable;

typedef struct ChangedRows
{
	ProllyDiffSink sink;		/* must be first */
	List	   *tables;			/* ChangedTable * */
	List	   *relevant;		/* the tables to check, and those their
								 * foreign keys join (NIL: every one) */
	ChangedTable *cur;			/* the table being read, as: */
	Relation	rel;
	const ProllyKeyingRoutine *kr;
	ProllyKeying *k;
	bool		keyless;
	ProllyTreeEnv env;			/* (its values out of line read through) */
	ProllyKeyingRefs krefs;
	Bitmapset  *keycols;		/* its columns matched by (match_columns()) */
	bool	   *kept;			/* (decoding: the columns left unread) */
	TupleTableSlot *slot;
} ChangedRows;

/* The most keys one query matches: more are matched a batch at a time. */
#define CHANGED_BATCH_ROWS 1000
#define CHANGED_BATCH_BYTES (1024 * 1024)

static ChangedTable *changed_table(List **tables, Oid relid);

static void
changed_close(ChangedRows *cr)
{
	if (cr->rel == NULL)
		return;
	/* (each only if opened: a table with no key is closed before them) */
	if (cr->slot != NULL)
		ExecDropSingleTupleTableSlot(cr->slot);
	if (cr->k != NULL)
		PROLLY_CALL(cr->kr, close, cr->k);
	table_close(cr->rel, AccessShareLock);
	bms_free(cr->keycols);
	cr->keycols = NULL;
	cr->slot = NULL;
	cr->k = NULL;
	cr->rel = NULL;
}

/*
 * A tree value's row's key, as a JSON object of its key columns' values
 * as text; *count gets how many copies it stands for.
 */
static char *
changed_key(ChangedRows *cr, const ProllyValue *value, uint64 *count)
{
	ProllyValue row;
	TupleDesc	desc = RelationGetDescr(cr->rel);
	StringInfoData buf;
	int			col = -1;
	bool		first = true;

	PROLLY_CALL(cr->kr, parse_value, cr->k, value, &row, count);
	/*
	 * (its columns out of line left unread, as kept against itself, unless
	 * a key column is one of them: then read whole)
	 */
	ExecClearTuple(cr->slot);
	PROLLY_CALL(cr->kr, decode_row_kept, cr->k, &row, &row, cr->slot, cr->kept);
	for (int i = 0; i < desc->natts; i++)
	{
		if (cr->kept[i] &&
			bms_is_member(i + 1 - FirstLowInvalidHeapAttributeNumber, cr->keycols))
		{
			ExecClearTuple(cr->slot);
			PROLLY_CALL(cr->kr, decode_row, cr->k, &row, cr->slot);
			break;
		}
	}
	slot_getallattrs(cr->slot);
	initStringInfo(&buf);
	appendStringInfoChar(&buf, '{');
	while ((col = bms_next_member(cr->keycols, col)) >= 0)
	{
		AttrNumber	attno = col + FirstLowInvalidHeapAttributeNumber;
		Form_pg_attribute att = TupleDescAttr(desc, attno - 1);
		Oid			out;
		bool		varlena;

		if (!first)
			appendStringInfoString(&buf, ", ");
		first = false;
		escape_json(&buf, NameStr(att->attname));
		appendStringInfoString(&buf, ": ");
		if (cr->slot->tts_isnull[attno - 1])
		{
			appendStringInfoString(&buf, "null");
			continue;
		}
		getTypeOutputInfo(att->atttypid, &out, &varlena);
		escape_json(&buf, OidOutputFunctionCall(out, cr->slot->tts_values[attno - 1]));
	}
	appendStringInfoChar(&buf, '}');
	return buf.data;
}

/*
 * The columns of rel (a partition, of ancestors) its changed rows are
 * matched by: its primary key's, which its own rows' checks match, and
 * those a foreign key references, which its referencing rows' match (as
 * RelationGetIndexAttrBitmap() numbers them).  Only these are read, so a
 * unique column out of line no check matches by stays unread.
 */
static Bitmapset *
match_columns(Relation rel, List *ancestors)
{
	Oid			relid = RelationGetRelid(rel);
	Oid			conoid;
	Bitmapset  *out = bms_copy(get_primary_key_attnos(relid, false, &conoid));
	Relation	pg_constraint = table_open(ConstraintRelationId, AccessShareLock);
	SysScanDesc scan = systable_beginscan(pg_constraint, InvalidOid, false,
										  NULL, 0, NULL);
	HeapTuple	tup;

	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tup);
		bool		isnull;
		Datum		d;
		ArrayType  *fk;
		int16	   *keys;

		if (con->contype != CONSTRAINT_FOREIGN ||
			(con->confrelid != relid && !list_member_oid(ancestors, con->confrelid)))
			continue;
		d = heap_getattr(tup, Anum_pg_constraint_confkey,
						 RelationGetDescr(pg_constraint), &isnull);
		fk = DatumGetArrayTypeP(d);
		keys = (int16 *) ARR_DATA_PTR(fk);
		/* (by name: a partition's columns may be numbered otherwise) */
		for (int i = 0; i < ARR_DIMS(fk)[0]; i++)
		{
			AttrNumber	attno = get_attnum(relid, get_attname(con->confrelid, keys[i], false));

			if (attno != InvalidAttrNumber)
				out = bms_add_member(out, attno - FirstLowInvalidHeapAttributeNumber);
		}
	}
	systable_endscan(scan);
	table_close(pg_constraint, AccessShareLock);
	return out;
}

/* Whether table ct is one to check, or one a foreign key joins it to. */
static bool
changed_relevant(const ChangedRows *cr, const ChangedTable *ct)
{
	ListCell   *lc;

	if (cr->relevant == NIL || list_member_oid(cr->relevant, ct->relid))
		return true;
	foreach(lc, ct->ancestors)
		if (list_member_oid(cr->relevant, lfirst_oid(lc)))
			return true;
	return false;
}

/* Table ct checked whole: its rows aren't kept. */
static void
changed_whole(ChangedTable *ct)
{
	ct->whole = true;
	list_free_deep(ct->now);
	list_free_deep(ct->was);
	ct->now = ct->was = NIL;
}

static void
changed_row(ProllyDiffSink *self, Oid relid, ProllyDiffKind kind,
			const ProllyKey *key, const ProllyValue *before,
			const ProllyValue *after)
{
	ChangedRows *cr = (ChangedRows *) self;
	uint64		nbefore = 0;
	uint64		nafter = 0;
	char	   *was = NULL;
	char	   *now = NULL;

	/* (a table's rows come together) */
	if (cr->cur == NULL || cr->cur->relid != relid)
	{
		changed_close(cr);
		cr->cur = changed_table(&cr->tables, relid);
		/* (a table checked whole, or not at all: its rows aren't read) */
		if (cr->cur->whole || !changed_relevant(cr, cr->cur))
			return;
		cr->rel = table_open(relid, AccessShareLock);
		cr->keycols = match_columns(cr->rel, cr->cur->ancestors);
		if (bms_is_empty(cr->keycols))
		{
			/* (no key to match its rows by) */
			changed_close(cr);
			changed_whole(cr->cur);
			return;
		}
		cr->kr = keying_of(cr->rel);
		cr->k = PROLLY_CALL(cr->kr, open, cr->rel, CurrentMemoryContext);
		cr->keyless = PROLLY_CALL(cr->kr, keyless, cr->k);
		chunk_env(&cr->env);
		prolly_keying_use_env(&cr->krefs, cr->kr, cr->k,
							  prolly_tree_lookup("cow"), &cr->env);
		cr->kept = palloc0_array(bool, RelationGetDescr(cr->rel)->natts);
		cr->slot = MakeSingleTupleTableSlot(RelationGetDescr(cr->rel),
											&TTSOpsVirtual);
	}
	if (cr->rel == NULL || cr->cur->whole)
		return;
	if (before)
		was = changed_key(cr, before, &nbefore);
	if (after)
		now = changed_key(cr, after, &nafter);
	if (cr->keyless)
	{
		if (nafter > nbefore)
			cr->cur->now = lappend(cr->cur->now, now);
		else
			cr->cur->was = lappend(cr->cur->was, was);
		return;
	}
	if (now)
		cr->cur->now = lappend(cr->cur->now, now);
	if (was)
		cr->cur->was = lappend(cr->cur->was, was);
}

/*
 * The tables of relids, and those their foreign keys join them to: theirs
 * and those referencing them (NIL, relids NIL: every one).
 */
static List *
joined_tables(List *relids)
{
	Relation	pg_constraint;
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *out = list_copy(relids);

	if (relids == NIL)
		return NIL;
	pg_constraint = table_open(ConstraintRelationId, AccessShareLock);
	scan = systable_beginscan(pg_constraint, InvalidOid, false, NULL, 0, NULL);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tup);

		if (con->contype != CONSTRAINT_FOREIGN)
			continue;
		if (list_member_oid(relids, con->conrelid))
			out = list_append_unique_oid(out, con->confrelid);
		if (list_member_oid(relids, con->confrelid))
			out = list_append_unique_oid(out, con->conrelid);
	}
	systable_endscan(scan);
	table_close(pg_constraint, AccessShareLock);
	return out;
}

/*
 * What changed since the head (for the tables of relids, NIL: every
 * one), as ChangedTables.
 */
static List *
changed_since_head(List *relids)
{
	ChangedRows cr;
	List	   *head = tables_of("HEAD", InvalidOid);
	ListCell   *lc;

	memset(&cr, 0, sizeof(cr));
	cr.sink.change = changed_row;
	cr.relevant = joined_tables(relids);
	/* (those made or redefined, or with violations recorded, first) */
	foreach(lc, tables_of("WORKING", InvalidOid))
	{
		Table	   *t = lfirst(lc);
		Table	   *h = find_table(head, t->name);
		Oid			relid = table_named(t->name);

		if (OidIsValid(relid) && (h == NULL || !same_description(h, t)))
			changed_table(&cr.tables, relid)->whole = true;
	}
	foreach(lc, PROLLY_CALL(components().refs, violations))
	{
		ProllyViolation *pv = lfirst(lc);

		if (OidIsValid(pv->relid) &&
			SearchSysCacheExists1(RELOID, ObjectIdGetDatum(pv->relid)))
			changed_table(&cr.tables, pv->relid)->whole = true;
	}
	folded_diff("HEAD", "WORKING", InvalidOid, &cr.sink);
	changed_close(&cr);
	return cr.tables;
}

/* Table relid's entry in *tables, made if it has none. */
static ChangedTable *
changed_table(List **tables, Oid relid)
{
	ChangedTable *ct;
	ListCell   *lc;

	foreach(lc, *tables)
		if (((ChangedTable *) lfirst(lc))->relid == relid)
			return lfirst(lc);
	ct = palloc0(sizeof(ChangedTable));
	ct->relid = relid;
	if (get_rel_relispartition(relid))
		ct->ancestors = get_partition_ancestors(relid);
	*tables = lappend(*tables, ct);
	return ct;
}

/*
 * Table relid's rows in changed (its partitions' too, if it's
 * partitioned), appended to *now and *was (where not NULL).  Returns
 * whether it (or a partition of it) is checked whole: every row.
 */
static bool
changed_rows_of(List *changed, Oid relid, List **now, List **was)
{
	bool		whole = false;
	ListCell   *lc;

	foreach(lc, changed)
	{
		ChangedTable *ct = lfirst(lc);

		if (ct->relid != relid && !list_member_oid(ct->ancestors, relid))
			continue;
		whole |= ct->whole;
		if (now)
			*now = list_concat(*now, ct->now);
		if (was)
			*was = list_concat(*was, ct->was);
	}
	return whole;
}

/*
 * Columns attnums (n of them) of relid as a column list (each prefixed
 * by alias, "c.a, c.b") into *cols and as json_to_recordset()'s
 * definitions ("a int4, b text") into *defs.
 */
static void
key_columns(Oid relid, const int16 *attnums, int n, const char *alias,
			StringInfo cols, StringInfo defs)
{
	for (int i = 0; i < n; i++)
	{
		HeapTuple	tup = SearchSysCache2(ATTNUM, ObjectIdGetDatum(relid),
										  Int16GetDatum(attnums[i]));
		Form_pg_attribute att;

		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for attribute %d of relation %u",
				 attnums[i], relid);
		att = (Form_pg_attribute) GETSTRUCT(tup);
		appendStringInfo(cols, "%s%s.%s", i ? ", " : "", alias,
						 quote_identifier(NameStr(att->attname)));
		appendStringInfo(defs, "%s%s %s", i ? ", " : "",
						 quote_identifier(NameStr(att->attname)),
						 format_type_with_typemod(att->atttypid, att->atttypmod));
		ReleaseSysCache(tup);
	}
}

/*
 * Relid's primary key columns, as key_columns() gives them; false if it
 * has none.
 */
static bool
primary_key_columns(Oid relid, const char *alias, StringInfo cols,
					StringInfo defs)
{
	Oid			conoid;
	Bitmapset  *pk = get_primary_key_attnos(relid, false, &conoid);
	int16	   *attnums = palloc_array(int16, Max(bms_num_members(pk), 1));
	int			n = 0;
	int			col = -1;

	if (pk == NULL)
		return false;
	while ((col = bms_next_member(pk, col)) >= 0)
		attnums[n++] = col + FirstLowInvalidHeapAttributeNumber;
	key_columns(relid, attnums, n, alias, cols, defs);
	return true;
}

/*
 * Keys (char *, JSON objects) as quoted JSON array literals, a batch of
 * them each, so that no one query's grows with how much changed.
 */
static List *
key_batches(List *keys)
{
	List	   *out = NIL;
	StringInfoData buf;
	int			n = 0;
	ListCell   *lc;

	initStringInfo(&buf);
	foreach(lc, keys)
	{
		const char *key = lfirst(lc);

		if (n > 0 && (n >= CHANGED_BATCH_ROWS ||
					  buf.len + strlen(key) > CHANGED_BATCH_BYTES))
		{
			appendStringInfoChar(&buf, ']');
			out = lappend(out, quote_literal_cstr(buf.data));
			resetStringInfo(&buf);
			n = 0;
		}
		appendStringInfoString(&buf, n ? ", " : "[");
		appendStringInfoString(&buf, key);
		n++;
	}
	if (n > 0)
	{
		appendStringInfoChar(&buf, ']');
		out = lappend(out, quote_literal_cstr(buf.data));
	}
	return out;
}

/*
 * Query (sql, info, relid) qi once for each batch of keys: its rows whose
 * columns cols (definitions defs, as key_columns() gives them) are one of
 * the batch's, each query appended to *queries.  (sql ends in a WHERE
 * clause's condition.)
 */
static void
key_queries(List *qi, List *keys, const char *cols, const char *defs,
			List **queries)
{
	ListCell   *lc;

	foreach(lc, key_batches(keys))
	{
		char	   *sql = psprintf("%s AND (%s) IN (SELECT * FROM json_to_recordset(%s::json) AS w(%s))",
								   (char *) linitial(qi), cols,
								   (char *) lfirst(lc), defs);

		*queries = lappend(*queries, list_make3(sql, lsecond(qi), lthird(qi)));
	}
}

/*
 * Violations found, less any found twice (a row matched by two batches,
 * by its own key and by its parent's, say).
 */
typedef struct SeenViolations
{
	uint64		hash;			/* the hash key: must be first */
	List	   *rows;			/* char *, those of this hash */
} SeenViolations;

static List *
distinct_violations(List *found)
{
	HTAB	   *seen;
	HASHCTL		ctl;
	List	   *out = NIL;
	ListCell   *lc;

	if (list_length(found) < 2)
		return found;
	ctl.keysize = sizeof(uint64);
	ctl.entrysize = sizeof(SeenViolations);
	ctl.hcxt = CurrentMemoryContext;
	seen = hash_create("verified violations", list_length(found), &ctl,
					   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	foreach(lc, found)
	{
		ProllyViolation *pv = lfirst(lc);
		char	   *k = psprintf("%u\t%s\t%s\t%s", pv->relid, pv->type, pv->info,
								 pv->row);
		uint64		h = hash_bytes_extended((const unsigned char *) k, strlen(k), 0);
		SeenViolations *sv;
		bool		had;
		ListCell   *ls;

		sv = hash_search(seen, &h, HASH_ENTER, &had);
		if (!had)
			sv->rows = NIL;
		foreach(ls, sv->rows)
			if (strcmp(lfirst(ls), k) == 0)
				break;
		if (ls != NULL)
			continue;
		sv->rows = lappend(sv->rows, k);
		out = lappend(out, pv);
	}
	hash_destroy(seen);
	return out;
}

/*
 * dolt_verify_constraints' check of the session branch's tables of relids
 * (NIL: every one): the rows breaking a foreign key, as a merge finds them,
 * and those breaking a check or not-null constraint not validated yet.
 * Unless all, only rows changed since the head commit are checked (and,
 * for a foreign key, those referencing a row removed or changed), as Dolt
 * checks what the working set changed, matched by key a batch at a time;
 * a table made or redefined since, one with violations recorded (which
 * the caller replaces with what's found) and one without a key, whole.  (The rows merges couldn't write aren't in the
 * tables: their violations aren't found again, what to do with them is
 * the user's.)
 */
static List *
verify_constraints(List *relids, bool all)
{
	List	   *changed = NIL;
	List	   *found = NIL;
	ProllyHash	head;
	bool		partial;
	int			save_nestlevel = NewGUCNestLevel();

	/*
	 * (keys print as their values, read back exactly, whatever the session
	 * says: a float's every digit)
	 */
	(void) set_config_option("extra_float_digits", "3", PGC_USERSET,
							 PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	/* (a branch with no commit yet: every row is a change) */
	partial = !all && PROLLY_CALL(components().refs, resolve, "HEAD", &head);
	if (partial)
		changed = changed_since_head(relids);
	if (!partial || changed != NIL)
		found = list_concat(foreign_key_violations(relids, changed),
							unvalidated_violations(relids, changed));
	AtEOXact_GUC(true, save_nestlevel);
	return found;
}

/*
 * Keep the violations a merge (or pick) found with those the session's
 * branch has already (a violation found again, once).  Returns how many
 * are new.
 */
static int
keep_violations(List *found)
{
	List	   *all = PROLLY_CALL(components().refs, violations);
	int			n = 0;
	ListCell   *lc;

	foreach(lc, found)
	{
		ProllyViolation *pv = lfirst(lc);
		bool		known = false;
		ListCell   *la;

		foreach(la, all)
		{
			ProllyViolation *a = lfirst(la);

			if (a->relid == pv->relid && strcmp(a->type, pv->type) == 0 &&
				strcmp(a->row, pv->row) == 0 && strcmp(a->info, pv->info) == 0)
			{
				known = true;
				break;
			}
		}
		if (!known)
		{
			all = lappend(all, pv);
			n++;
		}
	}
	if (n > 0)
		PROLLY_CALL(components().refs, set_violations, all);
	return n;
}

/*
 * What the working set changed since the head: the canonical items that
 * differ, each with the head's value (to put back), and by kind the
 * tables, sequences and materialized views (refreshed) among them, by
 * name.  A collation's item isn't among them: it says how the collation
 * orders now, which the working set's commits record whatever else they
 * leave out (the rows of the tables it keys are checked as they're read).
 */
typedef struct Local
{
	ProllyNodeRef head;
	ProllyNodeRef working;
	List	   *edits;			/* ProllyEdit *: the head's items, in order */
	List	   *tables;			/* char * */
	List	   *sequences;		/* char * */
	List	   *matviews;		/* char * */
	bool		definitions;	/* something other than tables and sequences */
} Local;

/* Does item key, of kind ("t:", say), name an object of that kind? */
static bool
item_kind(const ProllyKey *key, const char *kind)
{
	return key->len >= 2 && memcmp(key->data, kind, 2) == 0;
}

static char *
item_name(const ProllyKey *key)
{
	return pnstrdup((const char *) key->data + 2, key->len - 2);
}

/*
 * A materialized view's item: its data tree's hash (length, then hash),
 * then its query; the query alone, past the hash.
 */
static bool
same_matview_query(const ProllyValue *a, const ProllyValue *b)
{
	uint32		ha = a->len > 0 ? 1 + a->data[0] : 0;
	uint32		hb = b->len > 0 ? 1 + b->data[0] : 0;

	return ha <= a->len && hb <= b->len && a->len - ha == b->len - hb &&
		memcmp(a->data + ha, b->data + hb, a->len - ha) == 0;
}

static void
local_item(ProllyDiffKind kind, const ProllyKey *key, const ProllyValue *before,
		   const ProllyValue *after, void *arg)
{
	Local	   *l = arg;
	ProllyEdit *e;
	uint8	   *k;

	if (item_kind(key, "c:"))
		return;
	e = palloc0(sizeof(ProllyEdit));
	k = palloc(Max(key->len, 1));
	memcpy(k, key->data, key->len);
	e->key.data = k;
	e->key.len = key->len;
	if (before)
	{
		uint8	   *v = palloc(Max(before->len, 1));

		memcpy(v, before->data, before->len);
		e->value.data = v;
		e->value.len = before->len;
	}
	else
		e->is_delete = true;
	l->edits = lappend(l->edits, e);

	if (item_kind(key, "t:"))
		l->tables = lappend(l->tables, item_name(key));
	else if (item_kind(key, "s:"))
		l->sequences = lappend(l->sequences, item_name(key));
	else if (item_kind(key, "m:") && before && after &&
			 same_matview_query(before, after))
		l->matviews = lappend(l->matviews, item_name(key));
	else
		l->definitions = true;
}

/* Root's item key, into out; false if it has none. */
static bool
root_item(const ProllyNodeRef *root, const ProllyKey *key, StringInfo out)
{
	const ProllyTreeRoutine *tree = prolly_tree_lookup("cow");
	ProllyTreeEnv env;
	ProllyTreeCursor *c;
	ProllyKey	k;
	ProllyValue v;
	bool		found;

	chunk_env(&env);
	env.compare = prolly_bytes_compare;
	c = PROLLY_CALL(tree, cursor_open, &env, root, CurrentMemoryContext);
	PROLLY_CALL(tree, cursor_seek, c, key, ForwardScanDirection);
	found = PROLLY_CALL(tree, cursor_next, c, &k, &v) &&
		prolly_bytes_compare(&k, key, NULL) == 0;
	resetStringInfo(out);
	if (found)
		appendBinaryStringInfo(out, (const char *) v.data, v.len);
	PROLLY_CALL(tree, cursor_close, c);
	return found;
}

static void
local_changes(const ProllyHash *head, Local *l)
{
	ProllyTreeEnv env;

	memset(l, 0, sizeof(*l));
	commit_root(head, &l->head);
	(void) PROLLY_CALL(components().refs, working_set, NULL, &l->working);
	chunk_env(&env);
	env.compare = prolly_bytes_compare;
	PROLLY_CALL(prolly_tree_lookup("cow"), diff, &env, &l->head, &l->working,
				local_item, l);
}

static bool
has_local_changes(const Local *l)
{
	return l->edits != NIL;
}

static bool
list_has_name(List *list, const char *name)
{
	ListCell   *lc;

	foreach(lc, list)
		if (strcmp(lfirst(lc), name) == 0)
			return true;
	return false;
}

/*
 * A merge (or pick, or fast-forward) of what changed from base to theirs
 * into a working set with uncommitted changes, as in Dolt: refused if it
 * would change what they changed (a table's rows, a sequence's value), and,
 * for now, if the merge changes definitions (its move from ours' would put
 * the uncommitted rows back as ours has them), or the uncommitted changes
 * are of something other than tables and sequences.  Tables the working
 * set made or redefined, a merge changing no definitions leaves alone.
 */
static void
check_local(const Local *l, const ProllyNodeRef *base, const ProllyNodeRef *ours,
			const ProllyNodeRef *theirs, const char *what)
{
	ProllyDatabaseComponents dc = components();
	Tables		bt = {NIL};
	Tables		tt = {NIL};
	HTAB	   *bs;
	List	   *ts;
	ListCell   *lc;

	if (!has_local_changes(l))
		return;
	if (l->definitions ||
		!PROLLY_CALL(dc.canonicalizer, same_schema, base, ours) ||
		!PROLLY_CALL(dc.canonicalizer, same_schema, base, theirs))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot %s with uncommitted changes where definitions change", what),
				 errhint("Commit the changes, or reset the branch, first.")));
	PROLLY_CALL(dc.canonicalizer, tables, base, add_table, &bt);
	PROLLY_CALL(dc.canonicalizer, tables, theirs, add_table, &tt);
	/* (base and theirs define the same objects) */
	foreach(lc, tt.tables)
	{
		Table	   *t = lfirst(lc);
		Table	   *b = find_table(bt.tables, t->name);

		if (!same_hash(&b->data.hash, &t->data.hash) &&
			list_has_name(l->tables, t->name))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("local changes would be stomped by %s", what),
					 errdetail("Table \"%s\" has uncommitted changes, and the %s changes it too.",
							   t->name, what),
					 errhint("Commit the changes, or reset the branch, first.")));
	}
	bs = sequences_by_name(sequences_of(base));
	ts = sequences_of(theirs);
	foreach(lc, ts)
	{
		Sequence   *t = lfirst(lc);

		if (!same_sequence(lookup_sequence(bs, t->name), t) &&
			list_has_name(l->sequences, t->name))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("local changes would be stomped by %s", what),
					 errdetail("Sequence \"%s\" has uncommitted changes, and the %s changes it too.",
							   t->name, what),
					 errhint("Commit the changes, or reset the branch, first.")));
	}
	/* a materialized view refreshed: refused if theirs refreshed it too */
	foreach(lc, l->matviews)
	{
		const char *name = lfirst(lc);
		StringInfoData key;
		StringInfoData bv;
		StringInfoData tv;
		ProllyKey	k;

		initStringInfo(&key);
		initStringInfo(&bv);
		initStringInfo(&tv);
		appendStringInfoString(&key, "m:");
		appendStringInfoString(&key, name);
		k.data = (const uint8 *) key.data;
		k.len = key.len;
		if (root_item(base, &k, &bv) != root_item(theirs, &k, &tv) ||
			bv.len != tv.len || memcmp(bv.data, tv.data, bv.len) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("local changes would be stomped by %s", what),
					 errdetail("Materialized view \"%s\" has uncommitted changes, and the %s changes it too.",
							   name, what),
					 errhint("Commit the changes, or reset the branch, first.")));
	}
}

/*
 * The working set as it is, without the uncommitted changes it had before
 * a merge: the commit the merge makes, which leaves them uncommitted.
 */
static void
without_local(const Local *l, ProllyNodeRef *root)
{
	ProllyTreeEnv env;
	ProllyNodeRef now;
	ProllyEdit *edits;
	int			n = 0;
	ListCell   *lc;

	(void) PROLLY_CALL(components().refs, working_set, NULL, &now);
	if (!has_local_changes(l))
	{
		*root = now;
		return;
	}
	edits = palloc_array(ProllyEdit, list_length(l->edits));
	foreach(lc, l->edits)
		edits[n++] = *(ProllyEdit *) lfirst(lc);
	chunk_env(&env);
	env.compare = prolly_bytes_compare;
	PROLLY_CALL(prolly_tree_lookup("cow"), apply, &env, &now, edits, n, root);
}

/*
 * A fast-forward to theirs with uncommitted changes: the rows and sequence
 * values theirs changed since the head written, the changes left as they
 * are, and theirs made the head.  Refused if the rows written break a
 * foreign key, as a merge over uncommitted changes that leaves violations
 * is: the working set may have one the head hasn't.
 */
static void
fast_forward_local(const Local *l, const ProllyHash *theirs)
{
	ProllyDatabaseComponents dc = components();
	ProllyNodeRef target;
	Tables		ht = {NIL};
	Tables		tt = {NIL};
	HTAB	   *hs;
	List	   *ts;
	List	   *relids = NIL;
	List	   *broken;
	ListCell   *lc;

	commit_root(theirs, &target);
	PROLLY_CALL(dc.canonicalizer, tables, &l->head, add_table, &ht);
	PROLLY_CALL(dc.canonicalizer, tables, &target, add_table, &tt);
	foreach(lc, tt.tables)
	{
		Table	   *t = lfirst(lc);
		Table	   *h = find_table(ht.tables, t->name);

		if (h && !same_hash(&h->data.hash, &t->data.hash))
		{
			Oid			relid = table_named(t->name);

			apply_rows(relid, h, t);
			relids = lappend_oid(relids, relid);
		}
	}
	if (relids != NIL && (broken = foreign_key_violations(relids, NIL)) != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot merge with uncommitted changes when the merge conflicts"),
				 errdetail_plural("The fast-forward leaves %d constraint violation.",
								  "The fast-forward leaves %d constraint violations.",
								  list_length(broken), list_length(broken)),
				 errhint("Commit the changes, or reset the branch, first.")));
	hs = sequences_by_name(sequences_of(&l->head));
	ts = sequences_of(&target);
	foreach(lc, ts)
	{
		Sequence   *t = lfirst(lc);
		Sequence   *h = lookup_sequence(hs, t->name);

		if (h && !same_sequence(h, t))
			set_sequence(t);
	}
	PROLLY_CALL(dc.refs, set_head, theirs);

	/*
	 * Then a checkpoint, as after any move: else the pages its rows were
	 * written to stay changed since the baseline, and each fold reads them.
	 */
	{
		ProllyNodeRef root;

		PROLLY_CALL(dc.refs, checkpoint, &root);
	}
}

/*
 * A merge in progress on the session's branch, of theirs from base, with
 * conflicts in the tables of conflicted (OIDs); with pick_message, a
 * cherry-pick's, whose commit takes that message and one parent.
 */
static void
fill_merge_state(ProllyMergeState *ms, const ProllyHash *theirs,
				 const ProllyHash *base, List *conflicted,
				 const char *pick_message)
{
	ListCell   *lc;
	int			i = 0;

	memset(ms, 0, sizeof(*ms));
	ms->theirs = *theirs;
	ms->base = *base;
	ms->ntables = list_length(conflicted);
	ms->tables = palloc(sizeof(Oid) * Max(ms->ntables, 1));
	foreach(lc, conflicted)
		ms->tables[i++] = lfirst_oid(lc);
	ms->pick = pick_message != NULL;
	ms->message = unconstify(char *, pick_message);
}

/* Record such a merge in progress (fill_merge_state()). */
static void
record_merge_state(const ProllyHash *theirs, const ProllyHash *base,
				   List *conflicted, const char *pick_message)
{
	ProllyMergeState ms;

	fill_merge_state(&ms, theirs, base, conflicted, pick_message);
	PROLLY_CALL(components().refs, set_merge_state, &ms);
}

/*
 * Merge branch into the session's: nothing to do if it is merged already,
 * a fast-forward if the session's head is its ancestor, else three-way.
 * The heads are read with the refs held, so they stay as read until the
 * transaction ends, and a fast-forward moves to the commit checked.
 */
static int
merge_held(const char *branch, const char *message, int flags,
		   ProllyHash *commit_id)
{
	ProllyDatabaseComponents dc = components();
	ProllyHash	ours;
	ProllyHash	theirs;
	ProllyHash	base;
	bool		have_ours;

	ProllyMergeState ms;
	List	   *conflicted = NIL;
	int			conflicts = 0;
	ProllyHash	parents[2];
	ProllyNodeRef roots[3];
	Local		local;
	int			nviolations = 0;

	if (PROLLY_CALL(dc.refs, merge_state, &ms))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("a merge is in progress on the current branch"),
				 errhint("Resolve its conflicts and commit, or abort it.")));
	if (!PROLLY_CALL(dc.refs, resolve, branch, &theirs))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("prolly ref \"%s\" does not exist", branch)));
	have_ours = PROLLY_CALL(dc.refs, resolve, "HEAD", &ours);
	if (!have_ours)
	{
		if (flags & PROLLY_MERGE_NO_FF)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("the current branch has no commit to merge into")));
		/* all it has is uncommitted */
		if (working_has_objects())
			refuse_local_changes("merge");
		check_needs(&theirs, NULL, "merge");
		reset_to(&theirs);
		*commit_id = theirs;
		return 0;
	}
	if (!PROLLY_CALL(dc.refs, merge_base, &ours, &theirs, &base))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("branch \"%s\" has no history in common with the current one",
						branch)));
	if (same_hash(&base, &theirs))
	{
		*commit_id = ours;
		return 0;
	}
	/* (what their changes since the base bring in) */
	check_needs(&theirs, &base, "merge");
	commit_root(&base, &roots[0]);
	commit_root(&ours, &roots[1]);
	commit_root(&theirs, &roots[2]);
	local_changes(&ours, &local);
	check_local(&local, &roots[0], &roots[1], &roots[2], "merge");
	if (same_hash(&base, &ours) && !(flags & PROLLY_MERGE_NO_FF))
	{
		if (flags & PROLLY_MERGE_SQUASH)
		{
			/* their rows, the head left where it is */
			if (has_local_changes(&local))
				refuse_local_changes("squash a merge");
			folded_move_to(&roots[2], NULL);
		}
		else if (has_local_changes(&local))
			fast_forward_local(&local, &theirs);
		else
			reset_to(&theirs);
		*commit_id = theirs;
		return 0;
	}
	if (flags & PROLLY_MERGE_FF_ONLY)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("not possible to fast-forward, aborting"),
				 errdetail("The current branch has commits that branch \"%s\" doesn't.",
						   branch)));

	/* Three-way. */
	conflicts = merge_recording(roots,
								(flags & PROLLY_MERGE_FAIL_ON_CONFLICT) != 0,
								"merge", branch, &conflicted, &nviolations);

	/*
	 * Conflicts or constraint violations: a merge in progress, to resolve
	 * before its commit.  Not over uncommitted changes, which its commit
	 * would take in, and its abort (a reset) would lose.
	 */
	if (conflicts + nviolations > 0 && has_local_changes(&local))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot merge with uncommitted changes when the merge conflicts"),
				 errdetail("The merge has %d conflicting rows and %d constraint violations.",
						   conflicts, nviolations),
				 errhint("Commit the changes, or reset the branch, first.")));
	if (message == NULL)
		message = psprintf("Merge branch '%s' into %s", branch,
						   branch_ops()->current());

	/*
	 * A squash's conflicts are resolved as a pick's are: its commit has the
	 * one parent.
	 */
	if (conflicts + nviolations > 0)
	{
		record_merge_state(&theirs, &base, conflicted,
						   (flags & PROLLY_MERGE_SQUASH) ? message : NULL);
		memset(commit_id, 0, sizeof(*commit_id));
		return conflicts + nviolations;
	}

	/*
	 * No commit: the merge in progress (none, for a squash), as its
	 * conflicts would leave it.  Not over uncommitted changes, which its
	 * commit would take in.
	 */
	if (flags & PROLLY_MERGE_NO_COMMIT)
	{
		if (has_local_changes(&local))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot merge without committing over uncommitted changes"),
					 errhint("Commit the changes, or reset the branch, first.")));
		if (!(flags & PROLLY_MERGE_SQUASH))
			record_merge_state(&theirs, &base, NIL, NULL);
		memset(commit_id, 0, sizeof(*commit_id));
		return 0;
	}
	parents[0] = ours;
	parents[1] = theirs;
	if (flags & PROLLY_MERGE_SQUASH)
	{
		ProllyNodeRef merged;

		if (has_local_changes(&local))
			without_local(&local, &merged);
		PROLLY_CALL(dc.refs, commit_with, message, parents, 1,
					has_local_changes(&local) ? &merged : NULL, commit_id);
		return 0;
	}
	/* the merge's commit leaves the uncommitted changes uncommitted */
	if (has_local_changes(&local))
	{
		ProllyNodeRef merged;

		without_local(&local, &merged);
		PROLLY_CALL(dc.refs, commit_with, message, parents, 2, &merged, commit_id);
	}
	else
		PROLLY_CALL(dc.refs, commit_with, message, parents, 2, NULL, commit_id);
	return 0;
}

static int
folded_merge(const char *branch, const char *message, int flags,
			 ProllyHash *commit_id)
{
	/* the heads read under the refs held, as they are */
	bool		pushed = PROLLY_CALL(components().refs, hold);
	int			result = merge_held(branch, message, flags, commit_id);

	if (pushed)
		PopActiveSnapshot();
	return result;
}

/*
 * Resolve refs to commits with one parent each, whose changes a pick or
 * revert takes (what: "cherry-pick" or "revert", fn its function), checking
 * what their changes bring in: a pick's, what it needs that its parent
 * didn't; a revert's, what the parent needed that it doesn't.
 */
static void
resolve_picks(const char *const *refs, int nrefs, bool revert, const char *fn,
			  ProllyHash *picked, ProllyCommitInfo *info)
{
	ProllyDatabaseComponents dc = components();
	const char *what = revert ? "revert" : "cherry-pick";

	for (int i = 0; i < nrefs; i++)
	{
		if (!PROLLY_CALL(dc.refs, resolve, refs[i], &picked[i]) ||
			!PROLLY_CALL(dc.refs, commit_get, &picked[i], &info[i]))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("prolly ref \"%s\" does not exist", refs[i])));
		if (info[i].nparents == 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("%s of a branch's first commit isn't supported", fn),
					 errdetail("Commit \"%s\" has no parent to take its changes from.",
							   refs[i])));
		if (info[i].nparents != 1)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("%s of a commit with %d parents isn't supported", fn,
							info[i].nparents),
					 errdetail("Its changes are changes from one parent.")));
		if (revert)
			check_needs(&info[i].parents[0], &picked[i], what);
		else
			check_needs(&picked[i], &info[i].parents[0], what);
	}
}

/*
 * Commit the working set with one parent, the session's head: leaving the
 * uncommitted changes l had uncommitted.
 */
static void
commit_one_parent(const char *message, const ProllyHash *head, const Local *l,
				  ProllyHash *commit_id)
{
	ProllyDatabaseComponents dc = components();

	if (has_local_changes(l))
	{
		ProllyNodeRef root;

		without_local(l, &root);
		PROLLY_CALL(dc.refs, commit_with, message, head, 1, &root, commit_id);
	}
	else
		PROLLY_CALL(dc.refs, commit_with, message, head, 1, NULL, commit_id);
}

/*
 * Commit refs' changes (each from its parent) again on the session's
 * branch: every ref resolved before anything moves, each merged three-way
 * into the branch as the ones before left it, and one commit made of them
 * all, with one parent.  Conflicts leave the pick in progress, to resolve
 * before its commit.  So is a commit of nothing, unless allow_empty.  A
 * cherry-pick keeps its commit's author.
 */
static int
pick_held(const char *const *refs, int nrefs, bool allow_empty,
		  ProllyHash *commit_id)
{
	ProllyDatabaseComponents dc = components();
	const char *what = "cherry-pick";
	const char *fn = "dolt_cherry_pick";
	ProllyHash *picked = palloc(sizeof(ProllyHash) * nrefs);
	ProllyCommitInfo *info = palloc(sizeof(ProllyCommitInfo) * nrefs);
	ProllyHash	ours;
	ProllyNodeRef roots[3];
	ProllyNodeRef head;
	ProllyMergeState ms;
	List	   *conflicted = NIL;
	StringInfoData message;
	ProllyNodeRef before;
	Local		local;

	if (PROLLY_CALL(dc.refs, merge_state, &ms))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("a merge is in progress on the current branch"),
				 errhint("Resolve its conflicts and commit, or abort it.")));
	resolve_picks(refs, nrefs, false, fn, picked, info);
	if (!PROLLY_CALL(dc.refs, resolve, "HEAD", &ours))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("the current branch has no commit to apply changes to")));
	/* every pick checked against the uncommitted changes first */
	commit_root(&ours, &head);
	local_changes(&ours, &local);
	for (int i = 0; i < nrefs; i++)
	{
		commit_root(&info[i].parents[0], &roots[0]);
		commit_root(&picked[i], &roots[2]);
		check_local(&local, &roots[0], &head, &roots[2], what);
	}
	root_of("WORKING", &before);

	roots[1] = head;
	initStringInfo(&message);
	for (int i = 0; i < nrefs; i++)
	{
		int			conflicts;
		int			nviolations = 0;

		commit_root(&info[i].parents[0], &roots[0]);
		commit_root(&picked[i], &roots[2]);
		conflicts = merge_recording(roots, false, fn, NULL, &conflicted,
									&nviolations);

		/*
		 * The pick in progress.  Not over uncommitted changes, which its
		 * commit would take in, and its abort (a reset) would lose.
		 */
		if (conflicts + nviolations > 0 && has_local_changes(&local))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot %s with uncommitted changes when the %s conflicts",
							what, what),
					 errdetail("The %s has %d conflicting rows and %d constraint violations.",
							   what, conflicts, nviolations),
					 errhint("Commit the changes, or reset the branch, first.")));
		if (conflicts + nviolations > 0)
		{
			record_merge_state(&picked[i], &info[i].parents[0], conflicted,
							   info[i].message);
			memset(commit_id, 0, sizeof(*commit_id));
			return conflicts + nviolations;
		}
		/* the next merges into the rows as this one left them */
		root_of("WORKING", &roots[1]);
		appendStringInfoString(&message, info[i].message);
	}
	if (same_hash(&roots[1].hash, &before.hash) && !allow_empty)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("%s changes nothing on the branch", fn),
				 errhint("Pass --allow-empty to commit it anyway.")));

	prolly_set_commit_author(info[0].author);
	PG_TRY();
	{
		commit_one_parent(message.data, &ours, &local, commit_id);
	}
	PG_FINALLY();
	{
		prolly_set_commit_author(NULL);
	}
	PG_END_TRY();
	return 0;
}

/*
 * Undo refs' changes (each from it to its parent), one commit each, as in
 * Dolt: every ref resolved before anything moves, each merged three-way
 * into the branch as the ones before left it.  A revert that conflicts (or
 * leaves rows breaking constraints) stops the series there, in progress
 * as a pick is: its commit, once they are resolved, is the next of the
 * series (dolt_revert --continue goes on with the rest), and an abort
 * goes back to series_head, the head before the series (NULL: the head
 * now).  commit_id gets the last commit made.
 */
static int
revert_held(const char *const *refs, int nrefs, const ProllyHash *series_head,
			ProllyHash *commit_id)
{
	ProllyDatabaseComponents dc = components();
	const char *what = "revert";
	const char *fn = "dolt_revert";
	ProllyHash *picked = palloc(sizeof(ProllyHash) * nrefs);
	ProllyCommitInfo *info = palloc(sizeof(ProllyCommitInfo) * nrefs);
	ProllyHash	ours;
	ProllyHash	series;
	ProllyNodeRef roots[3];
	ProllyNodeRef head;
	ProllyMergeState ms;
	Local		local;

	if (PROLLY_CALL(dc.refs, merge_state, &ms))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("a merge is in progress on the current branch"),
				 errhint("Resolve its conflicts and commit, or abort it.")));
	resolve_picks(refs, nrefs, true, fn, picked, info);
	if (!PROLLY_CALL(dc.refs, resolve, "HEAD", &ours))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("the current branch has no commit to apply changes to")));
	series = series_head ? *series_head : ours;
	commit_root(&ours, &head);
	local_changes(&ours, &local);
	for (int i = 0; i < nrefs; i++)
	{
		commit_root(&picked[i], &roots[0]);
		commit_root(&info[i].parents[0], &roots[2]);
		check_local(&local, &roots[0], &head, &roots[2], what);
	}

	roots[1] = head;
	memset(commit_id, 0, sizeof(*commit_id));
	for (int i = 0; i < nrefs; i++)
	{
		List	   *conflicted = NIL;
		int			conflicts;
		int			nviolations = 0;
		char	   *message = psprintf("Revert \"%s\"", info[i].message);
		ProllyNodeRef before;

		root_of("WORKING", &before);
		commit_root(&picked[i], &roots[0]);
		commit_root(&info[i].parents[0], &roots[2]);
		conflicts = merge_recording(roots, false, fn, NULL, &conflicted,
									&nviolations);
		if (conflicts + nviolations > 0 && has_local_changes(&local))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot %s with uncommitted changes when the %s conflicts",
							what, what),
					 errdetail("The %s has %d conflicting rows and %d constraint violations.",
							   what, conflicts, nviolations),
					 errhint("Commit the changes, or reset the branch, first.")));
		if (conflicts + nviolations > 0)
		{
			ProllyMergeState rs;

			fill_merge_state(&rs, &info[i].parents[0], &picked[i], conflicted,
							 message);
			rs.revert = true;
			rs.series_head = series;
			rs.npending = nrefs - i - 1;
			rs.pending = picked + i + 1;
			PROLLY_CALL(dc.refs, set_merge_state, &rs);
			memset(commit_id, 0, sizeof(*commit_id));
			return conflicts + nviolations;
		}
		root_of("WORKING", &roots[1]);
		if (same_hash(&roots[1].hash, &before.hash))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("%s changes nothing on the branch", fn),
					 errdetail("Reverting \"%s\" changes nothing.", refs[i])));
		commit_one_parent(message, &ours, &local, commit_id);
		ours = *commit_id;
	}
	return 0;
}

/*
 * The merge in progress, if relid has unresolved conflicts in it (its
 * position in state->tables), else -1.
 */
static int
conflicted_table(Oid relid, ProllyMergeState *state)
{
	if (!PROLLY_CALL(components().refs, merge_state, state))
		return -1;
	for (int i = 0; i < state->ntables; i++)
		if (state->tables[i] == relid)
			return i;
	return -1;
}

/*
 * Walk relid's changes in the merge in progress, as mode says, against
 * the branch's rows as they are now.  (A table the working set leaves out,
 * one the canonical form can't describe as defined now, is walked against
 * ours' rows, the branch's as last committed.)
 */
static void
walk_conflicts(Oid relid, const ProllyMergeState *ms, WalkMode mode,
			   ProllyConflictSink *sink)
{
	char	   *name = table_name(relid);
	ProllyHash	ours;

	Table	   *now = find_table(tables_of("WORKING", relid), name);
	Table		b;
	Table		o;
	Table		t;
	const Table *bp;
	const Table *op;
	const Table *tp;
	Table		nb;
	Table		no;
	Table		nt;
	Relation	rel;
	UnsureRows	unsure;
	Table	   *bt;
	Table	   *ot;
	Table	   *tt;
	List	   *retyped;
	List	   *both_retyped;

	/* The merge left the head where it was: ours is the head commit. */
	if (!PROLLY_CALL(components().refs, resolve, "HEAD", &ours))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("a merge is in progress on a branch with no commit")));
	/*
	 * (each as the table is defined now, if one defines it otherwise: base
	 * compared as the merge compared it in the columns the merge retyped,
	 * so its conflicts are the ones the merge found, and in a column
	 * retyped since as the sides' are, so it finds none there; ours and
	 * theirs as a diff shows them, the table perhaps changed since, with
	 * theirs' rows checked as they're taken.  Nothing refused, as a merge
	 * would: rows are listed whatever happened to the table since.)
	 */
	bt = find_table(tables_at(&ms->base), name);
	ot = find_table(tables_at(&ours), name);
	tt = find_table(tables_at(&ms->theirs), name);
	rel = table_open(relid, AccessShareLock);
	retyped_columns(bt, ot, tt, &retyped, &both_retyped);
	bp = as_defined_now(rel, bt, now, READ_COMPARED, retyped, both_retyped,
						false, &b, &unsure);
	op = as_defined_now(rel, ot, now, READ_SHOWN, NIL, NIL, false, &o, NULL);
	tp = as_defined_now(rel, tt, now, READ_SHOWN, NIL, NIL, false, &t, NULL);
	table_close(rel, AccessShareLock);

	/*
	 * (none of the merge's versions has the table by its name now: renamed
	 * or moved since the merge began, its rows there can't be found)
	 */
	if (bp == NULL && op == NULL && tp == NULL)
	{
		char	   *why = psprintf("table %s was renamed or moved since the merge began",
								   name);

		if (sink != NULL && sink->unreadable != NULL)
		{
			sink->unreadable(sink, why);
			return;
		}
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("%s", why),
				 errhint("Give it its name back to list or resolve its conflicts.")));
	}
	/* (a version without the table has none of its rows) */
	if (bp == NULL)
	{
		no_rows(op ? op : tp, &nb);
		bp = &nb;
	}
	if (op == NULL)
	{
		no_rows(bp, &no);
		op = &no;
	}
	if (tp == NULL)
	{
		no_rows(bp, &nt);
		tp = &nt;
	}
	(void) merge_table(relid, bp, op, tp, mode, sink, now, &unsure);
}

static void
folded_conflicts(Oid relid, ProllyConflictSink *sink)
{
	ProllyMergeState ms;

	if (conflicted_table(relid, &ms) < 0)
		return;
	walk_conflicts(relid, &ms, WALK_LIST, sink);
}

static void
folded_resolve(Oid relid, bool theirs)
{
	ProllyMergeState ms;
	int			i = conflicted_table(relid, &ms);

	if (i < 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("table \"%s\" has no merge conflicts", get_rel_name(relid))));
	if (theirs)
		walk_conflicts(relid, &ms, WALK_TAKE_THEIRS, NULL);
	ms.tables[i] = ms.tables[--ms.ntables];
	PROLLY_CALL(components().refs, set_merge_state, &ms);
}

static int
folded_cherry_pick(const char *const *refs, int nrefs, bool allow_empty,
				   ProllyHash *commit_id)
{
	/* the head read under the refs held, as it is */
	bool		pushed = PROLLY_CALL(components().refs, hold);
	int			result = pick_held(refs, nrefs, allow_empty, commit_id);

	if (pushed)
		PopActiveSnapshot();
	return result;
}

static int
folded_revert(const char *const *refs, int nrefs, const ProllyHash *series_head,
			  ProllyHash *commit_id)
{
	bool		pushed = PROLLY_CALL(components().refs, hold);
	int			result = revert_held(refs, nrefs, series_head, commit_id);

	if (pushed)
		PopActiveSnapshot();
	return result;
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
	.merge = folded_merge,
	.conflicts = folded_conflicts,
	.resolve = folded_resolve,
	.tables = folded_tables,
	.move_to = folded_move_to,
	.cherry_pick = folded_cherry_pick,
	.revert = folded_revert,
	.verify = verify_constraints,
};
