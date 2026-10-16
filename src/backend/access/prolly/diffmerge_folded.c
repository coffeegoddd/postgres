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
 * time (two rows swapping a value).  Definitions that differ are changed
 * first, by the canonicalizer.  Sequences are set to the root's values.
 * A merge gives each sequence the other side changed the larger of the
 * two values.
 *
 * Merge is three-way, from the newest common ancestor of the session's
 * head and the other one, as in implementation "rows": for each table,
 * the other side's changes since the ancestor are written to the branch
 * (with the row applier) unless the branch changed the same key to
 * another row since, a conflict, which keeps the branch's row and leaves
 * the merge in progress in the refs.  Its conflicts are found again from
 * the three commits when they're listed or resolved.  A keyless table's
 * rows have no identity, so the two sides' changes in a row's copies add
 * up.  The branch must have no changes since its head, and the three
 * commits must define their objects alike, for now.
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
#include "access/prolly_xact.h"
#include "access/table.h"
#include "catalog/branch_ops.h"
#include "catalog/namespace.h"
#include "commands/sequence.h"
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

static Sequence *
find_sequence(List *list, const char *name)
{
	ListCell   *lc;

	foreach(lc, list)
		if (strcmp(((Sequence *) lfirst(lc))->name, name) == 0)
			return lfirst(lc);
	return NULL;
}

static bool
same_sequence(const Sequence *a, const Sequence *b)
{
	return a->last == b->last && a->called == b->called;
}

/* How far a sequence has gone: the values it gave out, or none. */
static bool
further(const Sequence *a, const Sequence *b)
{
	if (a->called != b->called)
		return a->called;
	return a->last > b->last;
}

static void
set_sequence(const Sequence *sq)
{
	List	   *names = stringToQualifiedNameList(sq->name, NULL);
	Oid			relid = RangeVarGetRelid(makeRangeVarFromNameList(names),
										 RowExclusiveLock, false);

	SetSequence(relid, sq->last, sq->called);
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
	/* materialized views' rows too: a commit holds them as it was made */
	PROLLY_CALL(dc.canonicalizer, materialized_views, working, add_table, &from);
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

	/* and every sequence's value */
	{
		List	   *now = sequences_of(working);

		foreach(lc, sequences_of(target))
		{
			Sequence   *t = lfirst(lc);
			Sequence   *f = find_sequence(now, t->name);

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

	PROLLY_CALL(components().canonicalizer, fold, &working);
	rows_from(&working, arg);
}

/* The definitions first, if they differ, then the rows. */
static void
folded_move_to(const ProllyNodeRef *target)
{
	ProllyDatabaseComponents dc = components();
	ProllyNodeRef working;

	PROLLY_CALL(dc.canonicalizer, fold, &working);
	if (PROLLY_CALL(dc.canonicalizer, same_schema, &working, target))
		rows_from(&working, target);	/* (folded already) */
	else
		PROLLY_CALL(dc.canonicalizer, move_schema, &working, target, move_rows,
					(void *) target);
}

/*
 * Each sequence theirs changed since base gets the larger of its value and
 * ours.
 */
static void
merge_sequences(const ProllyNodeRef *base, const ProllyNodeRef *ours,
				const ProllyNodeRef *theirs)
{
	List	   *b = sequences_of(base);
	List	   *o = sequences_of(ours);
	ListCell   *lc;

	foreach(lc, sequences_of(theirs))
	{
		Sequence   *t = lfirst(lc);
		Sequence   *bs = find_sequence(b, t->name);
		Sequence   *os = find_sequence(o, t->name);

		if (bs && same_sequence(bs, t))
			continue;
		if (os == NULL || further(t, os))
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
} KeyWrite;

/*
 * Walk theirs (table relid's changes from base to theirs) against ours
 * (base to ours, the session's head, which the branch still is), both in
 * key order, as mode says.  A key both sides changed to different rows is
 * a conflict; one they changed alike isn't.  A keyless table's changes in
 * a row's copies add up (unless both sides made it the same number), and
 * conflict only when theirs removes more copies than ours has.  Returns
 * how many keys conflict.
 */
static int
merge_table(Oid relid, const Table *base, const Table *ours,
			const Table *theirs, WalkMode mode, ProllyConflictSink *sink)
{
	const ProllyTreeRoutine *tree = prolly_tree_lookup("cow");
	const ProllyApplierRoutine *ar = prolly_applier_lookup("replication");
	Relation	rel = table_open(relid, mode == WALK_LIST ? AccessShareLock :
								 RowExclusiveLock);
	ProllyTreeEnv env;
	Changes		oc;
	Changes		tc;
	ProllyApplier *a = NULL;
	TupleTableSlot *s1;
	TupleTableSlot *s2;
	ListCell   *lo;
	ListCell   *lt;
	int			conflicts = 0;
	bool		keyless;
	List	   *writes = NIL;

	require_readable(base);
	require_readable(ours);
	require_readable(theirs);
	memset(&tc, 0, sizeof(tc));
	tc.rd.kr = (const ProllyKeyingRoutine *)
		prolly_get_impl(PROLLY_IFACE_TABLE_KEYING, "pk_or_content");
	tc.rd.k = PROLLY_CALL(tc.rd.kr, open_rewritten, rel, CurrentMemoryContext);
	keyless = PROLLY_CALL(tc.rd.kr, keyless, tc.rd.k);
	oc = tc;
	chunk_env(&env);
	env.compare = changes_compare;
	env.compare_arg = &tc;
	PROLLY_CALL(tree, diff, &env, &base->data, &theirs->data, collect_change,
				&tc);
	PROLLY_CALL(tree, diff, &env, &base->data, &ours->data, collect_change,
				&oc);

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

		/* the row as the branch has it, and as theirs has it */
		now = o ? &o->after : &t->before;
		have = (int64) decode(&tc, now, s1);
		want = (int64) decode(&tc, &t->after, s2);
		row = &t->after;
		if (keyless)
		{
			int64		delta = want - (int64) decode(&tc, &t->before, s2);

			want = have + delta;
			conflict = want < 0 && !(o != NULL && same_bytes(&o->after, &t->after));
			row = delta > 0 ? &t->after : &t->before;
		}
		else
			conflict = o != NULL && !same_bytes(&o->after, &t->after);

		if (conflict)
		{
			ProllyValue bv,
						ov,
						tv;

			conflicts++;
			if (mode == WALK_LIST)
				sink->conflict(sink, &tk, value_or_null(&t->before, &bv),
							   value_or_null(now, &ov),
							   value_or_null(&t->after, &tv));
			if (mode != WALK_TAKE_THEIRS)
				continue;
			/* as theirs has it: one row or none, or its copies */
			want = keyless ? (int64) decode(&tc, &t->after, s2) :
				(t->after.len > 0);
			if (keyless)
				row = want == 0 ? now : &t->after;
		}
		else if (mode != WALK_MERGE || (o != NULL && (!keyless ||
														same_bytes(&o->after, &t->after))))
			continue;			/* listing; or both changed it alike */

		/* the branch's row (now, have copies) becomes theirs (row, want) */
		w = palloc0(sizeof(KeyWrite));
		if (!keyless && have == 1 && want == 1)
		{
			w->old = now;
			w->nold = 1;
			w->row = row;
			w->nrow = 1;
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

		if (w->nold == 0)
			continue;
		(void) decode(&tc, w->old, s1);
		for (int64 i = 0; i < w->nold; i++)
			if (!PROLLY_CALL(ar, remove, a, s1))
				goto missing;
	}
	foreach(lt, writes)
	{
		KeyWrite   *w = lfirst(lt);

		if (w->nrow == 0)
			continue;
		(void) decode(&tc, w->row, s2);
		for (int64 i = 0; i < w->nrow; i++)
			PROLLY_CALL(ar, insert, a, s2);
	}
	if (a)
		PROLLY_CALL(ar, end, a);
	ExecDropSingleTupleTableSlot(s1);
	ExecDropSingleTupleTableSlot(s2);
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
	ListCell   *lc;
	int			conflicts = 0;

	if (!PROLLY_CALL(dc.canonicalizer, same_schema, &roots[0], &roots[1]) ||
		!PROLLY_CALL(dc.canonicalizer, same_schema, &roots[0], &roots[2]))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("commits whose objects are defined differently can't be merged yet"),
				 errdetail("A table or other object of the versioned schemas was made, dropped or redefined between them.")));
	base_tables = tables_in_root(&roots[0]);
	ours_tables = tables_in_root(&roots[1]);
	theirs_tables = tables_in_root(&roots[2]);
	merge_sequences(&roots[0], &roots[1], &roots[2]);
	foreach(lc, theirs_tables)
	{
		Table	   *t = lfirst(lc);
		Table	   *b = find_table(base_tables, t->name);
		Table	   *o = find_table(ours_tables, t->name);
		Oid			relid;
		int			n;

		if (same_hash(&b->data.hash, &t->data.hash))
			continue;
		relid = table_named(t->name);
		if (same_hash(&b->data.hash, &o->data.hash))
		{
			apply_rows(relid, o, t);
			continue;
		}
		n = merge_table(relid, b, o, t, WALK_MERGE, NULL);
		if (n > 0 && fail_on_conflict && name)
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("%s of branch \"%s\" conflicts in table \"%s\"",
							what, name, get_rel_name(relid)),
					 errdetail_plural("%d row was changed differently on both sides.",
									  "%d rows were changed differently on both sides.",
									  n, n)));
		if (n > 0 && fail_on_conflict)
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("%s conflicts in table \"%s\"", what, get_rel_name(relid)),
					 errdetail_plural("%d row was changed differently on both sides.",
									  "%d rows were changed differently on both sides.",
									  n, n)));
		if (n > 0)
		{
			conflicts += n;
			*conflicted = lappend_oid(*conflicted, relid);
		}
	}
	return conflicts;
}

/* The same, of three commits. */
static int
merge_commits(const ProllyHash *base, const ProllyHash *ours,
			  const ProllyHash *theirs, bool fail_on_conflict,
			  const char *what, const char *name, List **conflicted)
{
	ProllyNodeRef roots[3];

	commit_root(base, &roots[0]);
	commit_root(ours, &roots[1]);
	commit_root(theirs, &roots[2]);
	return merge_roots(roots, fail_on_conflict, what, name, conflicted);
}

/* Refuse a merge or pick over uncommitted changes, which it would lose. */
static void
check_clean(const char *what)
{
	bool		changed = false;

	folded_tables("HEAD", "WORKING", note_change, &changed);
	if (changed)
		refuse_local_changes(what);
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
	ListCell   *lc;
	ProllyHash	parents[2];

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

	check_clean("merge");
	if (same_hash(&base, &ours) && !(flags & PROLLY_MERGE_NO_FF))
	{
		reset_to(&theirs);
		*commit_id = theirs;
		return 0;
	}

	/* Three-way. */
	conflicts = merge_commits(&base, &ours, &theirs,
							  (flags & PROLLY_MERGE_FAIL_ON_CONFLICT) != 0,
							  "merge", branch, &conflicted);

	/* Conflicts: a merge in progress, to resolve before its commit. */
	if (conflicts > 0)
	{
		int			i = 0;

		ms.theirs = theirs;
		ms.base = base;
		ms.ntables = list_length(conflicted);
		ms.tables = palloc(sizeof(Oid) * ms.ntables);
		foreach(lc, conflicted)
			ms.tables[i++] = lfirst_oid(lc);
		PROLLY_CALL(dc.refs, set_merge_state, &ms);
		memset(commit_id, 0, sizeof(*commit_id));
		return conflicts;
	}
	parents[0] = ours;
	parents[1] = theirs;
	if (message == NULL)
		message = psprintf("Merge branch '%s' into %s", branch,
						   branch_ops()->current());
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
 * Commit refs' changes (each from its parent) again on the session's
 * branch, or, reverting, undo them (each from it to its parent): every ref
 * resolved before anything moves, each merged three-way into the branch as
 * the ones before left it, and one commit made of them all, with one
 * parent.  Conflicts are an error, for now; so is a commit of nothing,
 * unless allow_empty.  A cherry-pick keeps its commit's author.
 */
static void
pick_held(const char *const *refs, int nrefs, bool revert, bool allow_empty,
		  ProllyHash *commit_id)
{
	ProllyDatabaseComponents dc = components();
	const char *what = revert ? "revert" : "cherry-pick";
	const char *fn = revert ? "dolt_revert" : "dolt_cherry_pick";
	ProllyHash *picked = palloc(sizeof(ProllyHash) * nrefs);
	ProllyCommitInfo *info = palloc(sizeof(ProllyCommitInfo) * nrefs);
	ProllyHash	ours;
	ProllyNodeRef roots[3];
	ProllyNodeRef head;
	ProllyMergeState ms;
	List	   *conflicted = NIL;
	StringInfoData message;

	if (PROLLY_CALL(dc.refs, merge_state, &ms))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("a merge is in progress on the current branch"),
				 errhint("Resolve its conflicts and commit, or abort it.")));
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
	}
	if (!PROLLY_CALL(dc.refs, resolve, "HEAD", &ours))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("the current branch has no commit to apply changes to")));
	check_clean(what);

	commit_root(&ours, &head);
	roots[1] = head;
	initStringInfo(&message);
	for (int i = 0; i < nrefs; i++)
	{
		commit_root(revert ? &picked[i] : &info[i].parents[0], &roots[0]);
		commit_root(revert ? &info[i].parents[0] : &picked[i], &roots[2]);
		(void) merge_roots(roots, true, fn, NULL, &conflicted);
		/* the next merges into the rows as this one left them */
		root_of("WORKING", &roots[1]);
		if (!revert)
			appendStringInfoString(&message, info[i].message);
		else
			appendStringInfo(&message, i == 0 ? "Revert \"%s\"" : " and \"%s\"",
							 info[i].message);
	}
	if (same_hash(&roots[1].hash, &head.hash) && !allow_empty)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("%s changes nothing on the branch", fn),
				 revert ? 0 : errhint("Pass --allow-empty to commit it anyway.")));

	if (!revert)
		prolly_set_commit_author(info[0].author);
	PG_TRY();
	{
		PROLLY_CALL(dc.refs, commit_with, message.data, &ours, 1, NULL, commit_id);
	}
	PG_FINALLY();
	{
		prolly_set_commit_author(NULL);
	}
	PG_END_TRY();
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

/* Walk relid's changes in the merge in progress, as mode says. */
static void
walk_conflicts(Oid relid, const ProllyMergeState *ms, WalkMode mode,
			   ProllyConflictSink *sink)
{
	char	   *name = table_name(relid);
	ProllyHash	ours;

	/* The merge left the head where it was: ours is the head commit. */
	if (!PROLLY_CALL(components().refs, resolve, "HEAD", &ours))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("a merge is in progress on a branch with no commit")));
	(void) merge_table(relid, find_table(tables_at(&ms->base), name),
					   find_table(tables_at(&ours), name),
					   find_table(tables_at(&ms->theirs), name), mode, sink);
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

static void
folded_cherry_pick(const char *const *refs, int nrefs, bool revert,
				   bool allow_empty, ProllyHash *commit_id)
{
	/* the head read under the refs held, as it is */
	bool		pushed = PROLLY_CALL(components().refs, hold);

	pick_held(refs, nrefs, revert, allow_empty, commit_id);
	if (pushed)
		PopActiveSnapshot();
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
};
