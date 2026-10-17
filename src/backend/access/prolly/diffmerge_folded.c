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
 * up.  The branch may have uncommitted changes, as in Dolt, if the merge
 * changes none of the tables and sequences they changed, and no
 * definitions change: they stay uncommitted, left out of the merge's
 * commit.  Definitions changed since the ancestor are merged first (the canonicalizer's
 * merge_schema(): an object both sides changed differently fails the
 * merge); then a table all three define alike merges its rows three-way,
 * and one defined otherwise takes the rows of the side that redefined,
 * made or kept it, if the other side didn't change them.
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
#include "parser/parse_type.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "catalog/versioned.h"
#include "access/htup_details.h"
#include "access/genam.h"
#include "utils/fmgrprotos.h"
#include "utils/json.h"
#include "utils/array.h"
#include "executor/spi.h"
#include "catalog/pg_constraint.h"
#include "access/xact.h"
#include "catalog/branch_ops.h"
#include "catalog/namespace.h"
#include "catalog/pg_sequence.h"
#include "commands/sequence.h"
#include "common/int.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "parser/parse_coerce.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/injection_point.h"
#include "utils/lsyscache.h"
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
static int	keep_violations(List *found);
static List *foreign_key_violations(List *relids);
static void diff_rows_now(Oid relid, const Table *from, const Table *to,
						  const Table *now, ProllyDiffSink *sink);

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
	prolly_folded_tree_env(env);
	env->compare = NULL;
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
 * left out), only relid's rows if it is valid.
 */
static List *
tables_of(const char *ref, Oid relid)
{
	ProllyNodeRef root;
	Tables		ts = {NIL};

	if (is_working(ref))
		(void) PROLLY_CALL(components().refs, read_working_set, NULL, relid, &root);
	else
		root_of(ref, &root);
	PROLLY_CALL(components().canonicalizer, tables, &root, add_table, &ts);
	return ts.tables;
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
rel_named(const char *name, LOCKMODE lockmode, bool missing_ok)
{
	List	   *names = stringToQualifiedNameList(name, NULL);

	return RangeVarGetRelid(makeRangeVarFromNameList(names), lockmode,
							missing_ok);
}

static Oid
table_named(const char *name)
{
	return rel_named(name, NoLock, true);
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

/*
 * Every table the session branch has: tables only in other versions can't
 * be read as rows here (tables() names them).
 */
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

static void
folded_tables(const char *from_ref, const char *to_ref,
			  ProllyTableChangeVisit visit, void *arg)
{
	List	   *from = tables_of(from_ref, InvalidOid);
	List	   *to = strcmp(to_ref, from_ref) == 0 ? from :
		tables_of(to_ref, InvalidOid);
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
				!same_description(ta, tb))
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
		found = list_concat(recorded, foreign_key_violations(written));
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
	ListCell   *lc;

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
	foreach(lc, RelationGetIndexList(rel))
	{
		HeapTuple	tup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(lfirst_oid(lc)));
		Form_pg_index index;
		bool		unique;

		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for index %u", lfirst_oid(lc));
		index = (Form_pg_index) GETSTRUCT(tup);
		unique = !index->indisprimary && (index->indisunique || index->indisexclusion);
		ReleaseSysCache(tup);
		if (unique)
			return true;
	}
	breakable = false;
	return false;
}

/*
 * Write a row as a merge does: insert it, or update old to it.  While a
 * merge records violations, a row that breaks a unique, check or not-null
 * (or exclusion) constraint isn't written, and is recorded instead.
 * Returns false if old isn't there.
 */
static bool
merge_write(const ProllyApplierRoutine *ar, ProllyApplier *a, Relation rel,
			TupleTableSlot *old, TupleTableSlot *row)
{
	MemoryContext cxt = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	volatile bool ok = true;

	if (!recording || !may_break_constraints(rel))
	{
		if (old)
			return PROLLY_CALL(ar, update, a, old, row);
		PROLLY_CALL(ar, insert, a, row);
		return true;
	}
	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(cxt);
	PG_TRY();
	{
		if (old)
			ok = PROLLY_CALL(ar, update, a, old, row);
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
		rec = MemoryContextSwitchTo(TopTransactionContext);
		recorded = lappend(recorded,
						   violation(RelationGetRelid(rel), type, row_json(row),
									 psprintf("{\"Name\": %s}",
											  quote_json(ed->constraint_name ?
														 ed->constraint_name : ""))));
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
			(void) merge_write(ap->ar, ap->a, ap->rel, NULL, ap->slot);
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
	if (recording)
		note_written(relid);
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
	RestoreSequenceState(rel_named(sq->name, NoLock, false), sq->last,
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
folded_move_to(const ProllyNodeRef *target)
{
	ProllyDatabaseComponents dc = components();
	ProllyNodeRef working;

	(void) PROLLY_CALL(dc.refs, working_set, NULL, &working);
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
		relid = rel_named(t->name, NoLock, true);
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
	if (recording && mode == WALK_MERGE)
		note_written(relid);
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
		int			nrecorded = list_length(recorded);

		if (w->nrow == 0)
			continue;
		(void) decode(&tc, w->row, s2);
		for (int64 i = 0; i < w->nrow; i++)
			(void) merge_write(ar, a, rel, NULL, s2);

		/*
		 * A keyed row theirs changed, whose new version is recorded as a
		 * violation instead of written: the row stays as it was.
		 */
		if (!keyless && w->nold == 1 && list_length(recorded) > nrecorded)
		{
			(void) decode(&tc, w->old, s1);
			(void) merge_write(ar, a, rel, NULL, s1);
		}
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
		(void) parseTypeString(tc->type, &c->type, &c->typmod, NULL);
		cols = lappend(cols, c);
	}
	return cols;
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
 * A version x of table rel's rows as rel defines them now (now: the
 * version that defines it so), into *out: each row decoded with x's
 * definition and made a row of rel's by column name, a column x hasn't
 * taking its missing value (or null), one of another type converted
 * through its text form.  x itself if it defines the table so already, or
 * if now is NULL (the working set has no such table); NULL if x is.
 *
 * For a merge ('strict'), refused where rel's rows can't faithfully come
 * from x's: when x lacks a column whose values each row was given as it
 * was added (an identity, a generated column, a default such as a
 * serial's), or when x has a column rel hasn't while rel has one x hasn't
 * (a column renamed reads, by name, as one dropped and one added).  A diff
 * shows those as null.
 */
static Table *
as_defined_now(Relation rel, const Table *x, const Table *now, bool strict,
			   Table *out)
{
	RowDiff		rd;
	bool		keyed;
	List	   *cols;
	TupleDesc	xd;
	TupleDesc	md = RelationGetDescr(rel);
	const ProllyValueEncoderRoutine *ve = prolly_valenc_lookup("canonical");
	ProllyValueEncoder *enc;
	TupleTableSlot *xs;
	TupleTableSlot *ms;
	int		   *from;
	FmgrInfo   *out_fn;
	Oid		   *in_fn;
	Oid		   *in_param;
	const ProllyTreeRoutine *tree = prolly_tree_lookup("cow");
	ProllyTreeEnv env;
	ProllyTreeCursor *cur;
	ProllyKey	k;
	ProllyValue v;
	ConvRow   **rows = NULL;
	int			n = 0;
	int			max = 0;
	int			i;
	ProllyTreeBuilder *b;
	ListCell   *lc;
	bool		x_has_more = false;
	bool		keys_kept;
	Bitmapset  *pk;
	MemoryContext rowcxt;
	MemoryContext outer;

	if (x == NULL)
		return NULL;
	*out = *x;
	if (now == NULL || same_description(x, now))
		return out;
	require_readable(x);

	rd.kr = (const ProllyKeyingRoutine *)
		prolly_get_impl(PROLLY_IFACE_TABLE_KEYING, "pk_or_content");
	rd.k = PROLLY_CALL(rd.kr, open_rewritten, rel, CurrentMemoryContext);
	cols = version_columns(x, &keyed);
	if (keyed == PROLLY_CALL(rd.kr, keyless, rd.k))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("rows of table \"%s\" can't be merged across a change of its primary key",
						RelationGetRelationName(rel))));

	xd = CreateTemplateTupleDesc(list_length(cols));
	i = 1;
	foreach(lc, cols)
	{
		VersionColumn *c = lfirst(lc);

		TupleDescInitEntry(xd, i++, c->name, c->type, c->typmod, 0);
	}
	TupleDescFinalize(xd);
	enc = PROLLY_CALL(ve, begin, xd, CurrentMemoryContext);
	xs = MakeSingleTupleTableSlot(xd, &TTSOpsVirtual);
	ms = MakeSingleTupleTableSlot(md, &TTSOpsVirtual);

	/* each column of rel's: which of x's it comes from, and how */
	from = palloc_array(int, md->natts);
	out_fn = palloc0_array(FmgrInfo, md->natts);
	in_fn = palloc0_array(Oid, md->natts);
	in_param = palloc0_array(Oid, md->natts);
	for (int j = 0; j < xd->natts; j++)
	{
		bool		has = false;

		for (int a = 0; a < md->natts; a++)
			has |= !TupleDescAttr(md, a)->attisdropped &&
				strcmp(NameStr(TupleDescAttr(xd, j)->attname),
					   NameStr(TupleDescAttr(md, a)->attname)) == 0;
		x_has_more |= !has;
	}
	for (int a = 0; a < md->natts; a++)
	{
		Form_pg_attribute att = TupleDescAttr(md, a);

		from[a] = -1;
		if (att->attisdropped)
			continue;
		for (int j = 0; j < xd->natts; j++)
			if (strcmp(NameStr(TupleDescAttr(xd, j)->attname), NameStr(att->attname)) == 0)
				from[a] = j;
		if (from[a] < 0 && strict &&
			(att->attidentity || att->attgenerated ||
			 (att->atthasdef && !att->atthasmissing)))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("rows of table \"%s\" can't be merged across column \"%s\" being added",
							RelationGetRelationName(rel), NameStr(att->attname)),
					 errdetail("Each row was given its own value as the column was added (an identity, a generated column, or a default such as a serial's), which rows from before it don't have.")));
		if (from[a] < 0 && strict && x_has_more && !att->atthasmissing)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("rows of table \"%s\" can't be merged across column \"%s\" being renamed or replaced",
							RelationGetRelationName(rel), NameStr(att->attname)),
					 errdetail("One version has a column the table hasn't now, and the table has \"%s\", which that version hasn't.",
							   NameStr(att->attname))));
		if (from[a] >= 0 && TupleDescAttr(xd, from[a])->atttypid != att->atttypid)
		{
			Oid			outf;
			bool		varlena;

			getTypeOutputInfo(TupleDescAttr(xd, from[a])->atttypid, &outf, &varlena);
			fmgr_info(outf, &out_fn[a]);
			getTypeInputInfo(att->atttypid, &in_fn[a], &in_param[a]);
		}
	}

	/*
	 * Keys made of columns x has, as x has them, come out as x's did, in its
	 * order: the rows go straight into the tree.  Otherwise they're sorted
	 * first.
	 */
	pk = RelationGetIndexAttrBitmap(rel, INDEX_ATTR_BITMAP_PRIMARY_KEY);
	keys_kept = keyed;
	for (int a = 0; a < md->natts && keys_kept; a++)
		if (bms_is_member(a + 1 - FirstLowInvalidHeapAttributeNumber, pk))
			keys_kept = from[a] >= 0 && !OidIsValid(in_fn[a]);

	chunk_env(&env);
	env.compare = keying_compare;
	env.compare_arg = &rd;
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

		outer = MemoryContextSwitchTo(rowcxt);
		PROLLY_CALL(rd.kr, parse_value, rd.k, &v, &row, &count);
		ExecClearTuple(xs);
		PROLLY_CALL(ve, decode, enc, &row, xs);
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
			if (OidIsValid(in_fn[a]))
				ms->tts_values[a] =
					OidInputFunctionCall(in_fn[a],
										 OutputFunctionCall(&out_fn[a], xs->tts_values[from[a]]),
										 in_param[a], att->atttypmod);
			else
				ms->tts_values[a] = xs->tts_values[from[a]];
		}
		ExecStoreVirtualTuple(ms);
		initStringInfo(&key);
		initStringInfo(&crow);
		PROLLY_CALL(rd.kr, encode_row, rd.k, ms, &key, &crow, &content);
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
			if (keyed)
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
	MemoryContextDelete(rowcxt);
	out->text = now->text;
	out->textlen = now->textlen;
	PROLLY_CALL(ve, end, enc);
	ExecDropSingleTupleTableSlot(xs);
	ExecDropSingleTupleTableSlot(ms);
	PROLLY_CALL(rd.kr, close, rd.k);
	return out;
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
	fp = as_defined_now(rel, from, now, false, &f);
	tp = as_defined_now(rel, to, now, false, &t);
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
							  merge_table(relid, b, o, t, WALK_MERGE, NULL));
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
			(void) as_defined_now(rel, b, w, true, &bn);
			(void) as_defined_now(rel, o, w, true, &on);
			(void) as_defined_now(rel, t, w, true, &tn);
			table_close(rel, AccessShareLock);
			if (!same_hash(&w->data.hash, &on.data.hash))
				apply_rows(relid, w, &on);
			if (same_hash(&tn.data.hash, &bn.data.hash))
				continue;
			if (same_hash(&on.data.hash, &bn.data.hash))
				apply_rows(relid, &on, &tn);
			else
				row_conflicts(mr, relid,
							  merge_table(relid, &bn, &on, &tn, WALK_MERGE, NULL));
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
					merge_rows, mr);
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
 * MATCH FULL, some of it set, has no referenced row.
 */
static List *
foreign_key_violations(List *relids)
{
	Relation	pg_constraint = table_open(ConstraintRelationId, AccessShareLock);
	SysScanDesc scan = systable_beginscan(pg_constraint, InvalidOid, false,
										  NULL, 0, NULL);
	HeapTuple	tup;
	List	   *found = NIL;
	List	   *queries = NIL;
	ListCell   *lc;
	MemoryContext cxt = CurrentMemoryContext;

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

		/* (a partition's clone of its parent's is checked as the parent's) */
		if (con->contype != CONSTRAINT_FOREIGN || OidIsValid(con->conparentid) ||
			!namespace_is_versioned(get_rel_namespace(con->conrelid)) ||
			(relids != NIL && !list_member_oid(relids, con->conrelid) &&
			 !list_member_oid(relids, con->confrelid)))
			continue;
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
						 "SELECT row_to_json(c)::text FROM %s%s c WHERE "
						 "NOT EXISTS (SELECT 1 FROM %s%s p WHERE %s) AND ",
						 only_unless_partitioned(con->conrelid), child,
						 only_unless_partitioned(con->confrelid), parent,
						 eq.data);
		if (con->confmatchtype == FKCONSTR_MATCH_FULL)
			appendStringInfo(&q, "(%s)", any.data);
		else
			appendStringInfo(&q, "(%s)", set.data);
		queries = lappend(queries, list_make3(q.data, info.data,
											  (void *) (uintptr_t) con->conrelid));
	}
	systable_endscan(scan);
	table_close(pg_constraint, AccessShareLock);

	if (queries == NIL)
		return NIL;
	/*
	 * With a snapshot that sees the merge's writes; as each referencing
	 * table's owner, its rows and those it references seen whatever their
	 * row security says, as the foreign key's own checks see them.
	 */
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
		if (ret != SPI_OK_SELECT)
			elog(ERROR, "could not check a foreign key: %s", (char *) linitial(qi));
		for (uint64 r = 0; r < SPI_processed; r++)
		{
			char	   *row = SPI_getvalue(SPI_tuptable->vals[r],
										   SPI_tuptable->tupdesc, 1);
			MemoryContext spi = MemoryContextSwitchTo(cxt);

			found = lappend(found, violation(relid, "foreign key", pstrdup(row),
											 pstrdup(lsecond(qi))));
			MemoryContextSwitchTo(spi);
		}
	}
	SPI_finish();
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
 * for now, if definitions change on either side.
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
		!PROLLY_CALL(dc.canonicalizer, same_schema, &l->head, &l->working) ||
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
 * are, and theirs made the head.
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
	ListCell   *lc;

	commit_root(theirs, &target);
	PROLLY_CALL(dc.canonicalizer, tables, &l->head, add_table, &ht);
	PROLLY_CALL(dc.canonicalizer, tables, &target, add_table, &tt);
	foreach(lc, tt.tables)
	{
		Table	   *t = lfirst(lc);
		Table	   *h = find_table(ht.tables, t->name);

		if (h && !same_hash(&h->data.hash, &t->data.hash))
			apply_rows(table_named(t->name), h, t);
	}
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
}

/*
 * Record a merge in progress on the session's branch, of theirs from base,
 * with conflicts in the tables of conflicted (OIDs); with pick_message, a
 * cherry-pick's, whose commit takes that message and one parent.
 */
static void
record_merge_state(const ProllyHash *theirs, const ProllyHash *base,
				   List *conflicted, const char *pick_message)
{
	ProllyMergeState ms;
	ListCell   *lc;
	int			i = 0;

	memset(&ms, 0, sizeof(ms));
	ms.theirs = *theirs;
	ms.base = *base;
	ms.ntables = list_length(conflicted);
	ms.tables = palloc(sizeof(Oid) * Max(ms.ntables, 1));
	foreach(lc, conflicted)
		ms.tables[i++] = lfirst_oid(lc);
	ms.pick = pick_message != NULL;
	ms.message = unconstify(char *, pick_message);
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
	commit_root(&base, &roots[0]);
	commit_root(&ours, &roots[1]);
	commit_root(&theirs, &roots[2]);
	local_changes(&ours, &local);
	check_local(&local, &roots[0], &roots[1], &roots[2], "merge");
	if (same_hash(&base, &ours) && !(flags & PROLLY_MERGE_NO_FF))
	{
		if (has_local_changes(&local))
			fast_forward_local(&local, &theirs);
		else
			reset_to(&theirs);
		*commit_id = theirs;
		return 0;
	}

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
	if (conflicts + nviolations > 0)
	{
		record_merge_state(&theirs, &base, conflicted, NULL);
		memset(commit_id, 0, sizeof(*commit_id));
		return conflicts + nviolations;
	}
	parents[0] = ours;
	parents[1] = theirs;
	if (message == NULL)
		message = psprintf("Merge branch '%s' into %s", branch,
						   branch_ops()->current());
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
 * Commit refs' changes (each from its parent) again on the session's
 * branch, or, reverting, undo them (each from it to its parent): every ref
 * resolved before anything moves, each merged three-way into the branch as
 * the ones before left it, and one commit made of them all, with one
 * parent.  A pick's conflicts leave it in progress, to resolve before
 * its commit; a revert's are an error.  So is a commit of nothing, unless
 * allow_empty.  A cherry-pick keeps its commit's author.
 */
static int
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
	ProllyNodeRef before;
	Local		local;

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
	/* every pick checked against the uncommitted changes first */
	commit_root(&ours, &head);
	local_changes(&ours, &local);
	for (int i = 0; i < nrefs; i++)
	{
		commit_root(revert ? &picked[i] : &info[i].parents[0], &roots[0]);
		commit_root(revert ? &info[i].parents[0] : &picked[i], &roots[2]);
		check_local(&local, &roots[0], &head, &roots[2], what);
	}
	root_of("WORKING", &before);

	roots[1] = head;
	initStringInfo(&message);
	for (int i = 0; i < nrefs; i++)
	{
		int			conflicts;
		int			nviolations = 0;

		commit_root(revert ? &picked[i] : &info[i].parents[0], &roots[0]);
		commit_root(revert ? &info[i].parents[0] : &picked[i], &roots[2]);
		/* (a revert's conflicts are an error) */
		conflicts = merge_recording(roots, revert, fn, NULL, &conflicted,
									&nviolations);

		/*
		 * A pick's (a revert's failed): the pick in progress.  Not over
		 * uncommitted changes, which its commit would take in, and its
		 * abort (a reset) would lose.
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
			if (revert)
				record_merge_state(&info[i].parents[0], &picked[i], conflicted,
								   psprintf("Revert \"%s\"", info[i].message));
			else
				record_merge_state(&picked[i], &info[i].parents[0], conflicted,
								   info[i].message);
			memset(commit_id, 0, sizeof(*commit_id));
			return conflicts + nviolations;
		}
		/* the next merges into the rows as this one left them */
		root_of("WORKING", &roots[1]);
		if (!revert)
			appendStringInfoString(&message, info[i].message);
		else
			appendStringInfo(&message, i == 0 ? "Revert \"%s\"" : " and \"%s\"",
							 info[i].message);
	}
	if (same_hash(&roots[1].hash, &before.hash) && !allow_empty)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("%s changes nothing on the branch", fn),
				 revert ? 0 : errhint("Pass --allow-empty to commit it anyway.")));

	if (!revert)
		prolly_set_commit_author(info[0].author);
	PG_TRY();
	{
		/* the commit leaves the uncommitted changes uncommitted */
		if (has_local_changes(&local))
		{
			ProllyNodeRef picked_root;

			without_local(&local, &picked_root);
			PROLLY_CALL(dc.refs, commit_with, message.data, &ours, 1,
						&picked_root, commit_id);
		}
		else
			PROLLY_CALL(dc.refs, commit_with, message.data, &ours, 1, NULL,
						commit_id);
	}
	PG_FINALLY();
	{
		prolly_set_commit_author(NULL);
	}
	PG_END_TRY();
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

/* Walk relid's changes in the merge in progress, as mode says. */
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

	/* The merge left the head where it was: ours is the head commit. */
	if (!PROLLY_CALL(components().refs, resolve, "HEAD", &ours))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("a merge is in progress on a branch with no commit")));
	/* (each as the table is defined now, if one defines it otherwise) */
	rel = table_open(relid, AccessShareLock);
	bp = as_defined_now(rel, find_table(tables_at(&ms->base), name), now, false, &b);
	op = as_defined_now(rel, find_table(tables_at(&ours), name), now, false, &o);
	tp = as_defined_now(rel, find_table(tables_at(&ms->theirs), name), now, false, &t);
	table_close(rel, AccessShareLock);
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
	(void) merge_table(relid, bp, op, tp, mode, sink);
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
folded_cherry_pick(const char *const *refs, int nrefs, bool revert,
				   bool allow_empty, ProllyHash *commit_id)
{
	/* the head read under the refs held, as it is */
	bool		pushed = PROLLY_CALL(components().refs, hold);
	int			result = pick_held(refs, nrefs, revert, allow_empty, commit_id);

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
	.verify = foreign_key_violations,
};
