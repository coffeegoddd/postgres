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
 * time (two rows swapping a value).  Definitions must be the same on both
 * sides for now.  Merges only fast-forward so far.
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
#include "access/table.h"
#include "catalog/namespace.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/varlena.h"

/* A table as one version has it. */
typedef struct Table
{
	char	   *name;			/* qualified, in the server's encoding */
	char	   *utf8;			/* the same in UTF-8, as the canonical tree
								 * orders them */
	ProllyNodeRef data;
	ProllyHash	description;	/* of its definition */
	char	   *unreadable;		/* why its rows can't be read here, or NULL */
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
		  const ProllyValue *description, const char *unreadable, void *arg)
{
	Tables	   *ts = arg;
	Table	   *t = palloc(sizeof(Table));
	const ProllyHashRoutine *h = prolly_hash_lookup("sha256");
	ProllyHashCtx *ctx = PROLLY_CALL(h, begin, CurrentMemoryContext);

	/* (the canonical tree's names are UTF-8; the catalogs' the server's) */
	t->name = pstrdup(pg_any_to_server(name, strlen(name), PG_UTF8));
	t->utf8 = pstrdup(name);
	t->unreadable = unreadable ?
		pstrdup(pg_any_to_server(unreadable, strlen(unreadable), PG_UTF8)) : NULL;
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
	RowDiff		rd;
	Relation	rel;
	ProllyNodeRef empty;
	ProllyTreeBuilder *b;

	if (from && to && same_hash(&from->data.hash, &to->data.hash))
		return;
	require_readable(from);
	require_readable(to);
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
note_change(const char *name, ProllyDiffKind kind, void *arg)
{
	*(bool *) arg = true;
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

/* ------------------------------------------------------------------------
 * Moving the branch
 * ------------------------------------------------------------------------
 */

/* One row changed, copied out of the trees. */
typedef struct Changes
{
	RowDiff		rd;				/* (for the keying's order) */
} Changes;

static int
changes_compare(const ProllyKey *a, const ProllyKey *b, void *arg)
{
	Changes    *c = arg;

	return PROLLY_CALL(c->rd.kr, compare, c->rd.k, a, b);
}

/* One pass of apply_rows over the diff: its removals, or its inserts. */
typedef struct Applying
{
	Changes		c;				/* (for the keying) */
	const ProllyApplierRoutine *ar;
	ProllyApplier *a;
	Relation	rel;
	TupleTableSlot *slot;
	bool		keyless;
	bool		inserting;
} Applying;

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
			PROLLY_CALL(ap->ar, insert, ap->a, ap->slot);
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
	Applying	ap;

	require_readable(from);
	require_readable(to);
	memset(&ap, 0, sizeof(ap));
	ap.rel = table_open(relid, RowExclusiveLock);
	ap.ar = prolly_applier_lookup("replication");
	ap.c.rd.kr = (const ProllyKeyingRoutine *)
		prolly_get_impl(PROLLY_IFACE_TABLE_KEYING, "pk_or_content");
	ap.c.rd.k = PROLLY_CALL(ap.c.rd.kr, open_rewritten, ap.rel,
							CurrentMemoryContext);
	ap.keyless = PROLLY_CALL(ap.c.rd.kr, keyless, ap.c.rd.k);
	chunk_env(&env);
	env.compare = changes_compare;
	env.compare_arg = &ap.c;

	ap.slot = table_slot_create(ap.rel, NULL);
	ap.a = PROLLY_CALL(ap.ar, begin, ap.rel);
	for (int pass = 0; pass < 2; pass++)
	{
		ap.inserting = (pass == 1);
		PROLLY_CALL(tree, diff, &env, &from->data, &to->data, apply_change, &ap);
	}
	PROLLY_CALL(ap.ar, end, ap.a);
	ExecDropSingleTupleTableSlot(ap.slot);
	PROLLY_CALL(ap.c.rd.kr, close, ap.c.rd.k);
	table_close(ap.rel, NoLock);
}

static void
folded_move_to(const ProllyNodeRef *target)
{
	ProllyDatabaseComponents dc = components();
	ProllyNodeRef working;
	Tables		from = {NIL};
	Tables		to = {NIL};
	ListCell   *lc;

	PROLLY_CALL(dc.canonicalizer, fold, &working);
	if (!PROLLY_CALL(dc.canonicalizer, same_schema, &working, target))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("the branch can't be moved to a commit that defines its objects differently yet"),
				 errdetail("Its versioned schemas' tables or other objects are made, dropped or defined differently there.")));
	PROLLY_CALL(dc.canonicalizer, tables, &working, add_table, &from);
	PROLLY_CALL(dc.canonicalizer, tables, target, add_table, &to);
	/* materialized views' rows too: a commit holds them as it was made */
	PROLLY_CALL(dc.canonicalizer, materialized_views, &working, add_table, &from);
	PROLLY_CALL(dc.canonicalizer, materialized_views, target, add_table, &to);
	foreach(lc, to.tables)
	{
		Table	   *t = lfirst(lc);
		Table	   *f = find_table(from.tables, t->name);

		if (f == NULL)
			elog(ERROR, "relation \"%s\" is missing from the working set", t->name);
		if (!same_hash(&f->data.hash, &t->data.hash))
			apply_rows(table_named(t->name), f, t);
	}
}

static int
canon_compare(const ProllyKey *a, const ProllyKey *b, void *arg)
{
	int			r = memcmp(a->data, b->data, Min(a->len, b->len));

	return r != 0 ? r : (a->len > b->len) - (a->len < b->len);
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
	env.compare = canon_compare;
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

static void
refuse_local_changes(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("cannot merge with uncommitted changes"),
			 errhint("Commit them, or reset the branch, first.")));
}

/*
 * Merge branch into the session's: nothing to do if it is merged already,
 * a fast-forward if the session's head is its ancestor.  The heads are
 * read with the refs held, so they stay as read until the transaction
 * ends, and the branch is moved to the commit that was checked.
 */
static int
merge_held(const char *branch, const char *message, int flags,
		   ProllyHash *commit_id)
{
	ProllyDatabaseComponents dc = components();
	ProllyHash	ours;
	ProllyHash	theirs;
	ProllyHash	base;
	bool		changed = false;

	if (!PROLLY_CALL(dc.refs, resolve, branch, &theirs))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("prolly ref \"%s\" does not exist", branch)));
	if (!PROLLY_CALL(dc.refs, resolve, "HEAD", &ours))
	{
		if (flags & PROLLY_MERGE_NO_FF)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("the current branch has no commit to merge into")));
		/* all it has is uncommitted */
		if (working_has_objects())
			refuse_local_changes();
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
	if (!same_hash(&base, &ours))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("branches made by copying can only fast-forward so far"),
				 errdetail("The branches have each changed since their common ancestor.")));
	if (flags & PROLLY_MERGE_NO_FF)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("branches made by copying can only fast-forward so far"),
				 errdetail("A merge commit where a fast-forward would do (--no-ff) isn't made yet.")));

	/* uncommitted changes would be lost */
	folded_tables("HEAD", "WORKING", note_change, &changed);
	if (changed)
		refuse_local_changes();
	reset_to(&theirs);
	*commit_id = theirs;
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
	.tables = folded_tables,
	.move_to = folded_move_to,
};
