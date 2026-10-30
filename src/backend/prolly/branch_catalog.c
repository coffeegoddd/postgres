/*-------------------------------------------------------------------------
 *
 * branch_catalog.c
 *	  Branched catalogs: the session's branch, schema names on branches,
 *	  and branch identifiers.
 *
 * A versioned schema is a set of pg_namespace rows of one name, one per
 * branch, each with its branch in nspbranch; a shared schema is one row
 * with nspbranch 0.  The name index is (nspname, nspbranch), so the rows
 * of one name are next to each other in it, and a lookup of a name on one
 * branch is a single syscache probe.
 *
 * Changing the session's branch changes what schema names mean, and
 * nothing else: every other catalog entry is keyed by object id, or by a
 * name together with a namespace's object id, and each branch's objects
 * have object ids of their own, so cached catalog entries stay right on
 * any branch.  What a branch change must discard is whatever was resolved
 * from a schema name: the search path, cached plans, the text search
 * configuration named by default_text_search_config, and the replication
 * output plugin's view of which publications cover which tables.  Each of
 * those is kept current by a syscache callback on pg_namespace, so a
 * branch change runs those callbacks as if every schema had changed (the
 * catalog filter's map of objects to branches, which does not depend on
 * the session's branch, ignores that; see catalog_branch_changing).  (The
 * session's sequence state is kept: it is by object id, each branch's
 * sequences have their own, and currval() of a name asks the branch's.  A
 * function running on its own branch switches it on each call, and must
 * not lose its caller's lastval().)  None of this reads the catalogs, so it
 * is safe wherever a setting can change, including while a transaction
 * aborts.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/prolly/branch_catalog.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/transam.h"
#include "catalog/namespace.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "commands/sequence.h"
#include "common/hashfn.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "prolly/branch_catalog.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

int			catalog_branch = 0;

/* set while a branch change runs the pg_namespace callbacks */
bool		catalog_branch_changing = false;

/*
 * A branch change: discard what was resolved from schema names (see the
 * file header).
 */
void
assign_catalog_branch(int newval, void *extra)
{
	if (newval == catalog_branch)
		return;

	catalog_branch_changing = true;
	PG_TRY();
	{
		CallSyscacheCallbacks(NAMESPACEOID, 0);
	}
	PG_FINALLY();
	{
		catalog_branch_changing = false;
	}
	PG_END_TRY();
	CallSyscacheCallbacks(TSCONFIGOID, 0);
}

/*
 * The pg_namespace row named nspname on branch, from the syscache, or
 * NULL.
 */
static HeapTuple
search_namespace(const char *nspname, int32 branch)
{
	HeapTuple	tup;

	tup = SearchSysCache2(NAMESPACENAME, CStringGetDatum(nspname),
						  Int32GetDatum(branch));
	return HeapTupleIsValid(tup) ? tup : NULL;
}

HeapTuple
SearchNamespaceByName(const char *nspname)
{
	HeapTuple	tup = NULL;

	if (catalog_branch != 0)
		tup = search_namespace(nspname, catalog_branch);
	if (tup == NULL)
		tup = search_namespace(nspname, 0);
	return tup;
}

Oid
NamespaceOidByName(const char *nspname)
{
	HeapTuple	tup = SearchNamespaceByName(nspname);
	Oid			oid;

	if (tup == NULL)
		return InvalidOid;
	oid = ((Form_pg_namespace) GETSTRUCT(tup))->oid;
	ReleaseSysCache(tup);
	return oid;
}

int32
NamespaceBranch(Oid nspid)
{
	HeapTuple	tup;
	int32		branch = 0;

	tup = SearchSysCache1(NAMESPACEOID, ObjectIdGetDatum(nspid));
	if (HeapTupleIsValid(tup))
	{
		branch = ((Form_pg_namespace) GETSTRUCT(tup))->nspbranch;
		ReleaseSysCache(tup);
	}
	return branch;
}

int32
FunctionBranch(HeapTuple proctup)
{
	Form_pg_proc proc = (Form_pg_proc) GETSTRUCT(proctup);
	int32		branch;
	Datum		config;
	bool		isnull;

	/* (initdb's schemas are shared) */
	if (proc->pronamespace < FirstNormalObjectId && !proc->prosecdef)
		return -1;
	branch = NamespaceBranch(proc->pronamespace);
	if (branch != 0)
		return branch;
	if (!proc->prosecdef)
		return -1;

	config = SysCacheGetAttr(PROCOID, proctup, Anum_pg_proc_proconfig,
							 &isnull);
	if (!isnull)
	{
		List	   *names;
		List	   *values;

		TransformGUCArray(DatumGetArrayTypeP(config), &names, &values);
		foreach_ptr(char, name, names)
		{
			if (pg_strcasecmp(name, "catalog_branch") == 0)
				return -1;
		}
	}
	return 0;
}

/*
 * Whether any branch has a versioned schema named nspname.  The rows of
 * one name are adjacent in the name index, so this reads only them.
 */
static bool
versioned_namespace_exists(const char *nspname)
{
	Relation	rel;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;
	bool		found = false;

	rel = table_open(NamespaceRelationId, AccessShareLock);
	ScanKeyInit(&key, Anum_pg_namespace_nspname, BTEqualStrategyNumber,
				F_NAMEEQ, CStringGetDatum(nspname));
	scan = systable_beginscan(rel, NamespaceNameIndexId, true, NULL, 1, &key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		if (((Form_pg_namespace) GETSTRUCT(tup))->nspbranch != 0)
		{
			found = true;
			break;
		}
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	return found;
}

/*
 * The unique index keeps one row per name and branch, but not one kind of
 * schema per name: a transaction making a shared schema and another making
 * a versioned one of the same name would both pass the checks below, and
 * both commit, unless one waits for the other.  So each takes a lock on
 * the name first.  The lock is on an object no catalog row has (sub-id 1
 * of an object id made from the name), so it conflicts only with another
 * schema of a name with the same hash being made or renamed; an unrelated
 * name with the same hash merely waits.
 */
static void
lock_namespace_name(const char *nspname)
{
	uint32		h = hash_bytes((const unsigned char *) nspname, strlen(nspname));

	LockDatabaseObject(NamespaceRelationId, (Oid) h, 1, ExclusiveLock);
}

void
NamespaceCheckNameFree(const char *nspname, int32 branch)
{
	HeapTuple	tup;

	lock_namespace_name(nspname);

	tup = search_namespace(nspname, branch);
	if (tup == NULL && branch != 0)
		tup = search_namespace(nspname, 0);
	if (tup != NULL)
	{
		ReleaseSysCache(tup);
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_SCHEMA),
				 errmsg("schema \"%s\" already exists", nspname)));
	}

	if (branch == 0 && versioned_namespace_exists(nspname))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_SCHEMA),
				 errmsg("schema \"%s\" already exists", nspname),
				 errdetail("A versioned schema of that name exists on a branch.")));
}

int32
VersionedNamespaceBranch(const char *nspname)
{
	if (catalog_branch == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot create versioned schema \"%s\" on no branch",
						nspname),
				 errhint("Set \"catalog_branch\" to the branch to create it on.")));
	if (!BranchIds->allocated(catalog_branch))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("branch %d has not been allocated", catalog_branch),
				 errhint("Allocate a branch with pg_branch_id_allocate().")));
	return catalog_branch;
}

/*
 * Extensions.  pg_extension's name index is (extname, extnamespace), so an
 * extension name can have one row per branch; the rows of one name are
 * adjacent in it, and there are only as many as there are branches with
 * their own copy.
 */
typedef enum ExtensionMatch
{
	EXT_VISIBLE,				/* shared, or on the session's branch */
	EXT_CONFLICTS,				/* would conflict with one on branch */
} ExtensionMatch;

static Oid
find_extension(const char *extname, ExtensionMatch match, int32 branch)
{
	Relation	rel;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;
	Oid			result = InvalidOid;

	rel = table_open(ExtensionRelationId, AccessShareLock);
	ScanKeyInit(&key, Anum_pg_extension_extname, BTEqualStrategyNumber,
				F_NAMEEQ, CStringGetDatum(extname));
	scan = systable_beginscan(rel, ExtensionNameIndexId, true, NULL, 1, &key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_extension e = (Form_pg_extension) GETSTRUCT(tup);
		int32		b = NamespaceBranch(e->extnamespace);
		bool		hit;

		if (match == EXT_VISIBLE)
			hit = b == 0 || b == catalog_branch;
		else
			hit = branch == 0 || b == 0 || b == branch;
		if (hit)
		{
			result = e->oid;
			break;
		}
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	return result;
}

Oid
ExtensionOidByName(const char *extname)
{
	return find_extension(extname, EXT_VISIBLE, 0);
}

void
ExtensionCheckNameFree(const char *extname, Oid nspid)
{
	int32		branch = NamespaceBranch(nspid);
	uint32		h = hash_bytes((const unsigned char *) extname, strlen(extname));

	/* (see lock_namespace_name) */
	LockDatabaseObject(ExtensionRelationId, (Oid) h, 1, ExclusiveLock);

	if (OidIsValid(find_extension(extname, EXT_CONFLICTS, branch)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("extension \"%s\" already exists", extname),
				 branch == 0 ?
				 errdetail("A branch has its own copy of the extension; a shared one cannot be made beside it.") :
				 errdetail("The extension is shared, or this branch already has it.")));
}

void
ExtensionCheckMove(const char *extname, Oid oldnspid, Oid newnspid)
{
	if (NamespaceBranch(oldnspid) != NamespaceBranch(newnspid))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot move extension \"%s\" between a versioned schema and another kind or branch of schema",
						extname)));
}

/*
 * Branch identifiers from a sequence, pg_catalog.pg_branch_id_seq, which
 * every database gets from its template.  A sequence is the Postgres way to
 * hand out numbers once: its state is logged, so no number is handed out
 * twice across a crash, a standby promotion or a restore, and
 * CREATE DATABASE copies it with the rest of the template, together with
 * the template's pg_namespace rows that name branches already handed out.
 * It is not transactional, which is what an identifier needs: one that a
 * transaction allocated and then rolled back stays used.
 */
static Oid
branch_id_sequence(void)
{
	Oid			seqoid = get_relname_relid("pg_branch_id_seq",
										   PG_CATALOG_NAMESPACE);

	if (!OidIsValid(seqoid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("sequence \"pg_catalog.pg_branch_id_seq\" does not exist")));
	return seqoid;
}

static int32
sequence_allocate(void)
{
	return (int32) nextval_internal(branch_id_sequence(), false);
}

static bool
sequence_allocated(int32 branch)
{
	LOCAL_FCINFO(fcinfo, 1);
	Datum		last;

	if (branch <= 0)
		return false;

	/*
	 * pg_sequence_last_value answers NULL before the first nextval, and for
	 * a user who may not read the sequence; PUBLIC may.
	 */
	InitFunctionCallInfoData(*fcinfo, NULL, 1, InvalidOid, NULL, NULL);
	fcinfo->args[0].value = ObjectIdGetDatum(branch_id_sequence());
	fcinfo->args[0].isnull = false;
	last = pg_sequence_last_value(fcinfo);
	if (fcinfo->isnull)
		return false;
	return branch <= DatumGetInt64(last);
}

const BranchIdRoutine SequenceBranchIds = {
	.allocate = sequence_allocate,
	.allocated = sequence_allocated,
};

const BranchIdRoutine *BranchIds = &SequenceBranchIds;

/* SQL: pg_branch_id_allocate() returns int4 */
Datum
pg_branch_id_allocate(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(BranchIds->allocate());
}
