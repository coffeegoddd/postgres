/*-------------------------------------------------------------------------
 *
 * branch_toast.c
 *	  Which TOAST relation a value stored out of line is in, on a branch,
 *	  from the copies recorded in pg_history.toast_copies.
 *
 * A read names only the value, not the heap it's of, so its TOAST relation is
 * found from the branch names resolve on: what it names, if that's this
 * branch's (or no branch's); else the copy of it here, found by following the
 * copies from it, a branch of a branch as much as a branch.  What's found is
 * kept for this backend until a relation or the branch changes.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/prolly/branch_toast.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "prolly/branch_catalog.h"
#include "prolly/branch_filter.h"
#include "prolly/branch_toast.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/* What a TOAST relation named on a branch was found as. */
typedef struct FoundKey
{
	Oid			named;
	int32		branch;
} FoundKey;

typedef struct FoundEntry
{
	FoundKey	key;
	Oid			found;
} FoundEntry;

static HTAB *found_cache = NULL;

static void
found_forget(Datum arg, Oid relid)
{
	if (found_cache != NULL)
	{
		hash_destroy(found_cache);
		found_cache = NULL;
	}
}

static void
found_forget_all(Datum arg, int cacheid, uint32 hashvalue)
{
	found_forget(arg, InvalidOid);
}

static HTAB *
found_cache_get(void)
{
	static bool registered = false;

	if (!registered)
	{
		/* (a relation made or dropped, a branch's copies among them; or the branch changed) */
		CacheRegisterRelcacheCallback(found_forget, (Datum) 0);
		CacheRegisterSyscacheCallback(NAMESPACEOID, found_forget_all, (Datum) 0);
		registered = true;
	}
	if (found_cache == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(FoundKey);
		ctl.entrysize = sizeof(FoundEntry);
		ctl.hcxt = CacheMemoryContext;
		found_cache = hash_create("TOAST relations found on branches", 16, &ctl,
								  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	return found_cache;
}

/* The copies made of source, as recorded now. */
static List *
copies_of(Oid source)
{
	Oid			history = get_namespace_oid("pg_history", false);
	Relation	rel = table_open(get_relname_relid("toast_copies", history), AccessShareLock);
	Oid			key = get_relname_relid("toast_copies_source", history);
	ScanKeyData skey;
	Snapshot	snapshot = RegisterSnapshot(GetLatestSnapshot());
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *copies = NIL;

	ScanKeyInit(&skey, 2, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(source));
	scan = systable_beginscan(rel, key, true, snapshot, 1, &skey);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		bool		isnull;

		copies = lappend_oid(copies, DatumGetObjectId(heap_getattr(tup, 1, RelationGetDescr(rel), &isnull)));
	}
	systable_endscan(scan);
	UnregisterSnapshot(snapshot);
	table_close(rel, AccessShareLock);
	return copies;
}

/* (a relation's branch, if it's still there: 0 for a shared one, -1 for one gone) */
static int32
toast_branch(Oid relid)
{
	if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(relid)))
		return -1;
	return BranchMap->object_branch(RelationRelationId, relid);
}

/* The copy of named, or of a copy of it, on branch, or InvalidOid. */
static Oid
copy_on(Oid named, int32 branch)
{
	List	   *todo = list_make1_oid(named);
	Oid			found = InvalidOid;

	while (todo != NIL && !OidIsValid(found))
	{
		Oid			source = linitial_oid(todo);

		todo = list_delete_first(todo);
		foreach_oid(copy, copies_of(source))
		{
			if (toast_branch(copy) == branch)
			{
				found = copy;
				break;
			}
			todo = lappend_oid(todo, copy);
		}
	}
	list_free(todo);
	return found;
}

static Oid
copies_to_read(Oid toastrelid)
{
	int32		branch = CatalogBranchOfNames();
	int32		of;
	FoundKey	key;
	FoundEntry *e;
	bool		found;

	if (branch == 0)
		return toastrelid;
	of = toast_branch(toastrelid);
	if (of == branch || of == 0)
		return toastrelid;

	memset(&key, 0, sizeof(key));
	key.named = toastrelid;
	key.branch = branch;
	e = hash_search(found_cache_get(), &key, HASH_FIND, &found);
	if (found)
		return e->found;
	{
		Oid			copy = copy_on(toastrelid, branch);

		if (!OidIsValid(copy))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("a value stored out of line names TOAST relation %u, of which branch %d has no copy",
							toastrelid, branch)));
		e = hash_search(found_cache_get(), &key, HASH_ENTER, &found);
		e->found = copy;
		return copy;
	}
}

static Oid
copies_to_delete(Relation heap, Oid toastrelid)
{
	Oid			own = heap->rd_rel->reltoastrelid;

	if (OidIsValid(own) && own != toastrelid && NamespaceBranch(RelationGetNamespace(heap)) != 0)
		return own;
	return toastrelid;
}

/* (written directly, as any role's drop of a TOAST relation writes it) */
static Relation
copies_open(LOCKMODE mode)
{
	Oid			history = get_namespace_oid("pg_history", true);
	Oid			relid = OidIsValid(history) ? get_relname_relid("toast_copies", history) : InvalidOid;

	return OidIsValid(relid) ? table_open(relid, mode) : NULL;
}

/* (each row whose column col is oid, given to fn) */
static void
copies_scan(Relation rel, AttrNumber col, Oid oid, void (*fn) (Relation rel, HeapTuple tup, void *arg),
			void *arg)
{
	ScanKeyData skey;
	Snapshot	snapshot = RegisterSnapshot(GetLatestSnapshot());
	SysScanDesc scan;
	HeapTuple	tup;

	ScanKeyInit(&skey, col, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(oid));
	scan = systable_beginscan(rel, InvalidOid, false, snapshot, 1, &skey);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
		fn(rel, tup, arg);
	systable_endscan(scan);
	UnregisterSnapshot(snapshot);
}

static void
copy_insert(Relation rel, Oid copy, Oid source)
{
	Datum		values[2] = {ObjectIdGetDatum(copy), ObjectIdGetDatum(source)};
	bool		nulls[2] = {false, false};
	HeapTuple	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);

	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);
}

static void
copies_copied(Oid copy, Oid source)
{
	Relation	rel = copies_open(RowExclusiveLock);

	if (rel == NULL)
		elog(ERROR, "pg_history.toast_copies is gone");
	copy_insert(rel, copy, source);
	table_close(rel, RowExclusiveLock);
}

typedef struct Dropping
{
	Oid			dropped;
	List	   *sources;		/* what the one dropped was a copy of */
	List	   *copies;			/* what was copied of it */
} Dropping;

static void
note_source(Relation rel, HeapTuple tup, void *arg)
{
	Dropping   *d = arg;
	bool		isnull;

	d->sources = lappend_oid(d->sources, DatumGetObjectId(heap_getattr(tup, 2, RelationGetDescr(rel), &isnull)));
	CatalogTupleDelete(rel, &tup->t_self);
}

static void
note_copy(Relation rel, HeapTuple tup, void *arg)
{
	Dropping   *d = arg;
	bool		isnull;

	d->copies = lappend_oid(d->copies, DatumGetObjectId(heap_getattr(tup, 1, RelationGetDescr(rel), &isnull)));
}

static void
copies_dropped(Oid toastrelid)
{
	Relation	rel = copies_open(RowExclusiveLock);
	Dropping	d = {toastrelid, NIL, NIL};

	if (rel == NULL)
		return;
	copies_scan(rel, 1, toastrelid, note_source, &d);
	if (d.sources != NIL)
	{
		copies_scan(rel, 2, toastrelid, note_copy, &d);
		CommandCounterIncrement();
		foreach_oid(copy, d.copies)
		{
			foreach_oid(source, d.sources)
			{
				List	   *had = copies_of(source);

				if (!list_member_oid(had, copy))
					copy_insert(rel, copy, source);
				list_free(had);
			}
		}
	}
	list_free(d.sources);
	list_free(d.copies);
	table_close(rel, RowExclusiveLock);
}

const BranchToastRoutine CopiesBranchToast = {
	.name = "copies",
	.copied = copies_copied,
	.dropped = copies_dropped,
	.to_read = copies_to_read,
	.to_delete = copies_to_delete,
};

const BranchToastRoutine *BranchToast = &CopiesBranchToast;
