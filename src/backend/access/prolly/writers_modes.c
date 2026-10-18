/*-------------------------------------------------------------------------
 *
 * writers_modes.c
 *	  Writers implementation "modes": the database's writer mode, kept as
 *	  a database setting in the refs, and branch owners on pg_branch.
 *
 * In the shared mode nothing is checked.  In the owned mode a branch's
 * pg_branch row records its owner (the writer, and the role that claimed
 * it for the writer) while the branch is dirty.  The first write to a
 * branch with no owner claims it; writes by the owner go on; anyone
 * else's write is refused at once, since waiting couldn't change the
 * outcome.  A claim is an ordinary catalog update, so it commits or
 * aborts with its transaction, and it never holds the refs (which would
 * serialize every claiming transaction in the database).
 *
 * The ownership lock (a lock on the branch's pg_branch row, held to
 * transaction end) orders claims: the owner's writers take it shared, a
 * claimer takes it exclusively and looks again once it has it, since an
 * in-flight transaction may have just claimed the branch or made it clean.
 * A branch is clean again once its working set is its head: after a
 * commit or a reset, the owner is forgotten if no other backend of the
 * writer is writing it (the lock is free) and nothing has been written
 * since (the working set, read with a new snapshot, is still the head's).
 * Otherwise the owner stays: the branch is taken to be dirty, which only
 * makes it wait for the owner's next commit.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/writers_modes.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/htup_details.h"
#include "access/prolly_builtins.h"
#include "access/prolly_impl.h"
#include "access/table.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/branch.h"
#include "catalog/indexing.h"
#include "catalog/pg_branch.h"
#include "catalog/pg_database.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/* the longest writer: room for it in a branch name with a base name */
#define WRITER_MAX		32

/* the setting in the refs that keeps the mode */
#define MODE_SETTING	"writer_branches"

/* the ownership lock: on the branch's pg_branch row, subobject 1 */
#define OWNERSHIP_SUBID 1

static char *dolt_writer = NULL;
static bool dolt_writer_nowait = false;

static const char *const mode_names[] = {PROLLY_WRITER_MODE_NAMES};

/* the mode, as the transaction last read or set it */
static LocalTransactionId mode_lxid = InvalidLocalTransactionId;
static ProllyWriterMode mode_read;

static bool
check_writer(char **newval, void **extra, GucSource source)
{
	for (const char *p = *newval; *p; p++)
	{
		if (!isalnum((unsigned char) *p) && strchr("._-@+", *p) == NULL)
		{
			GUC_check_errdetail("A writer is made of letters, digits, and \".\", \"_\", \"-\", \"@\" or \"+\".");
			return false;
		}
	}
	if (strlen(*newval) > WRITER_MAX)
	{
		GUC_check_errdetail("A writer is at most %d bytes.", WRITER_MAX);
		return false;
	}
	/* (what a transaction has written is one writer's) */
	if (IsTransactionState() && branches_written() != NIL)
	{
		GUC_check_errcode(ERRCODE_ACTIVE_SQL_TRANSACTION);
		GUC_check_errdetail("The transaction has written as writer \"%s\".",
							prolly_current_writer());
		return false;
	}
	return true;
}

void
prolly_writers_init(void)
{
	DefineCustomStringVariable("dolt.writer",
							   "Who a session's writes to branches are attributed to.",
							   "Empty means the session itself.",
							   &dolt_writer,
							   "",
							   PGC_USERSET,
							   0,
							   check_writer, NULL, NULL);
	DefineCustomBoolVariable("dolt.writer_nowait",
							 "Fails a claim of a branch at once rather than waiting for its writers.",
							 NULL,
							 &dolt_writer_nowait,
							 false,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);
}

char *
prolly_current_writer(void)
{
	if (dolt_writer != NULL && dolt_writer[0] != '\0')
		return pstrdup(dolt_writer);
	return psprintf("s%d.%ld", MyProcPid, (long) MyStartTime);
}

static const ProllyRefsRoutine *
refs(void)
{
	ProllyDatabaseComponents dc;

	prolly_database_components(&dc);
	return dc.refs;
}

/* (read once a transaction: a write's first note needn't read the refs) */
static ProllyWriterMode
modes_mode(void)
{
	StringInfoData v;
	ProllyWriterMode mode = PROLLY_WRITERS_SHARED;

	if (MyProc->vxid.lxid == mode_lxid)
		return mode_read;
	initStringInfo(&v);
	if (PROLLY_CALL(refs(), config_get, MODE_SETTING, &v))
	{
		for (int m = 0; m < lengthof(mode_names); m++)
		{
			if (strcmp(v.data, mode_names[m]) == 0)
				mode = (ProllyWriterMode) m;
		}
	}
	pfree(v.data);
	mode_lxid = MyProc->vxid.lxid;
	mode_read = mode;
	return mode;
}

static bool
database_owner(void)
{
	return object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId());
}

static void
modes_set_mode(ProllyWriterMode mode)
{
	if (!database_owner())
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_DATABASE,
					   get_database_name(MyDatabaseId));
	PROLLY_CALL(refs(), config_set, MODE_SETTING,
				mode == PROLLY_WRITERS_SHARED ? NULL : mode_names[mode]);
	mode_lxid = MyProc->vxid.lxid;
	mode_read = mode;
}

/* branch's pg_branch row, with its owner (NULL if none) and owner's role */
static HeapTuple
branch_row(Oid branch, char **owner, Oid *role)
{
	HeapTuple	tup = SearchSysCacheCopy1(BRANCHOID, ObjectIdGetDatum(branch));
	Datum		d;
	bool		isnull;

	*owner = NULL;
	*role = InvalidOid;
	if (!HeapTupleIsValid(tup))
		return NULL;
	d = SysCacheGetAttr(BRANCHOID, tup, Anum_pg_branch_browner, &isnull);
	if (!isnull)
		*owner = TextDatumGetCString(d);
	*role = ((Form_pg_branch) GETSTRUCT(tup))->brownerrole;
	return tup;
}

static void
set_owner(Relation rel, HeapTuple tup, const char *owner, Oid role)
{
	Datum		values[Natts_pg_branch] = {0};
	bool		nulls[Natts_pg_branch] = {0};
	bool		replace[Natts_pg_branch] = {0};
	HeapTuple	newtup;

	replace[Anum_pg_branch_browner - 1] = true;
	replace[Anum_pg_branch_brownerrole - 1] = true;
	if (owner)
		values[Anum_pg_branch_browner - 1] = CStringGetTextDatum(owner);
	else
		nulls[Anum_pg_branch_browner - 1] = true;
	values[Anum_pg_branch_brownerrole - 1] = ObjectIdGetDatum(role);
	newtup = heap_modify_tuple(tup, RelationGetDescr(rel), values, nulls,
							   replace);
	CatalogTupleUpdate(rel, &newtup->t_self, newtup);
	heap_freetuple(newtup);
}

/* (a role dropped since it claimed a branch has no name) */
static const char *
role_name(Oid role)
{
	const char *name = OidIsValid(role) ? GetUserNameFromId(role, true) : NULL;

	return name ? name : "?";
}

static void
refuse(HeapTuple tup, const char *owner)
{
	const char *name = NameStr(((Form_pg_branch) GETSTRUCT(tup))->brname);

	ereport(ERROR,
			(errcode(ERRCODE_OBJECT_IN_USE),
			 errmsg("branch \"%s\" has uncommitted changes by writer \"%s\"",
					name, owner),
			 errhint("Write on a new branch with dolt_checkout('-b', ...), ask writer \"%s\" to commit, or take the changes over with dolt_branch('--take', '%s').",
					 owner, name)));
}

/* the ownership lock, waited for (or, with dolt.writer_nowait, not) */
static void
lock_ownership(Oid branch, LOCKMODE mode, const char *name)
{
	if (mode == ExclusiveLock && dolt_writer_nowait)
	{
		if (!ConditionalLockDatabaseObject(BranchRelationId, branch,
										   OWNERSHIP_SUBID, mode))
			ereport(ERROR,
					(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					 errmsg("branch \"%s\" is being written", name)));
	}
	else
		LockDatabaseObject(BranchRelationId, branch, OWNERSHIP_SUBID, mode);
}

/*
 * Claim branch for the session's writer: as the owner's write, or as a
 * clean branch's first; or (take) a dirty branch whatever its owner, for
 * the database's owner or the role holding it.
 *
 * The owner's writers share the ownership lock; a claimer has it alone,
 * once their transactions end.  Either way the row is read again once the
 * lock is had, since meanwhile the branch may have been claimed, made
 * clean or taken over.  No lock on pg_branch is held while waiting (the
 * owner's transaction may make a branch meanwhile).
 */
static void
claim(Oid branch, bool take)
{
	char	   *writer = prolly_current_writer();
	HeapTuple	tup;
	char	   *owner;
	Oid			role;
	char	   *name;
	bool		shared;
	Relation	rel;

	tup = branch_row(branch, &owner, &role);
	if (tup == NULL)
		return;
	name = pstrdup(NameStr(((Form_pg_branch) GETSTRUCT(tup))->brname));
	if (take)
	{
		/* (a clean branch has nothing to take: the next write claims it) */
		if (owner == NULL)
			return;
		if (!database_owner() &&
			!(OidIsValid(role) && has_privs_of_role(GetUserId(), role)))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be the database's owner or role \"%s\" to take branch \"%s\" over",
							role_name(role), name)));
	}
	else if (owner != NULL && strcmp(owner, writer) != 0)
		refuse(tup, owner);

	shared = !take && owner != NULL;
	lock_ownership(branch, shared ? ShareLock : ExclusiveLock, name);
	heap_freetuple(tup);
	tup = branch_row(branch, &owner, &role);
	if (tup == NULL)
		return;
	if (!take && owner == NULL && shared)
	{
		/* (made clean while the owner's write waited: claimed alone) */
		lock_ownership(branch, ExclusiveLock, name);
		heap_freetuple(tup);
		tup = branch_row(branch, &owner, &role);
		if (tup == NULL)
			return;
	}
	if (!take && owner != NULL)
	{
		if (strcmp(owner, writer) != 0)
			refuse(tup, owner);

		/* (a writer is a role's: another role can't write as it) */
		if (OidIsValid(role) && !has_privs_of_role(GetUserId(), role) &&
			!database_owner())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("writer \"%s\" of branch \"%s\" is role \"%s\"'s",
							writer, name, role_name(role))));
		heap_freetuple(tup);
		return;
	}
	if (take && owner == NULL)
	{
		heap_freetuple(tup);
		return;
	}
	rel = table_open(BranchRelationId, RowExclusiveLock);
	set_owner(rel, tup, writer, GetUserId());
	table_close(rel, RowExclusiveLock);
	heap_freetuple(tup);
}

static void
modes_note_write(Oid branch)
{
	if (modes_mode() == PROLLY_WRITERS_SHARED)
		return;
	claim(branch, false);
}

static void
modes_take(Oid branch)
{
	if (modes_mode() == PROLLY_WRITERS_SHARED)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("branches have no owners in writer mode \"shared\"")));
	claim(branch, true);
	/* (the statement's own writes, claimed next, see the new owner) */
	CommandCounterIncrement();
}

/* Is the session's branch's working set its head's, as a new snapshot sees it? */
static bool
working_is_head(const char *name)
{
	ProllyHash	head;
	ProllyNodeRef ws;
	ProllyNodeRef committed;
	List	   *left_out = NIL;
	bool		same;

	PushActiveSnapshot(GetLatestSnapshot());
	same = PROLLY_CALL(refs(), resolve, name, &head) &&
		PROLLY_CALL(refs(), read_working_set, name, InvalidOid, &ws, &left_out) &&
		left_out == NIL &&
		PROLLY_CALL(refs(), commit_working_set, &head, &committed) &&
		ws.hash.len == committed.hash.len &&
		memcmp(ws.hash.bytes, committed.hash.bytes, ws.hash.len) == 0;
	PopActiveSnapshot();
	return same;
}

/*
 * (from a version-control call, between its own writes and the next
 * statement's: the claim it may have just made is seen)
 */
static void
modes_settled(Oid branch)
{
	Relation	rel;
	HeapTuple	tup;
	char	   *owner;
	Oid			role;

	CommandCounterIncrement();
	tup = branch_row(branch, &owner, &role);
	if (tup == NULL)
		return;
	if (owner != NULL &&
		ConditionalLockDatabaseObject(BranchRelationId, branch,
									  OWNERSHIP_SUBID, ExclusiveLock) &&
		working_is_head(NameStr(((Form_pg_branch) GETSTRUCT(tup))->brname)))
	{
		rel = table_open(BranchRelationId, RowExclusiveLock);
		set_owner(rel, tup, NULL, InvalidOid);
		table_close(rel, RowExclusiveLock);
		CommandCounterIncrement();

		/* (the transaction's next write to it claims it again) */
		branch_forget_written(branch);
	}
	heap_freetuple(tup);
}

const ProllyWritersRoutine prolly_writers_modes = {
	.info = {
		.iface = PROLLY_IFACE_WRITERS,
		.impl_id = 1,
		.format_version = 1,
		.name = "modes",
		.description = "writer modes: shared, or one owner per dirty branch",
	},
	.note_write = modes_note_write,
	.settled = modes_settled,
	.take = modes_take,
	.mode = modes_mode,
	.set_mode = modes_set_mode,
};
