/*-------------------------------------------------------------------------
 *
 * branch_ops.c
 *	  A branch made, its working set sharing its parent's pages.
 *
 * A branch is made at the point its parent's barrier marks, with the parent's
 * relations held: each table and sequence of the parent's schemas is locked so
 * as to wait out the writers in progress and hold off new ones (a lock that
 * keeps writers out, which VACUUM's and REFRESH's wait for too), once this
 * transaction is left out of capture, as taking one gives it an id.  Every
 * tuple on the parent's pages is then of a transaction that has committed or
 * aborted, so the new branch, sharing them, shows exactly what the parent had
 * committed then, and the barrier's root is that too: the parent's working
 * set.  Catalog changes in the database wait until the transaction ends, as
 * they do for any commit from capture that's left out.
 *
 * The catalog rows are copied (prolly/branch_catalog.h), each relation's
 * storage sharing its source's copy-on-write, but a sequence's, whose page is
 * copied.  The new branch's tables then hold the parent's working set, and
 * what differs from what it's to hold is written on it as a move writes it
 * (prolly/move.h): a commit's rows, or the head's, leaving the parent's
 * changes on the parent; or with its changes taken, nothing, and the parent
 * is moved back to its head.  A schema the target lacks is dropped, and one it
 * has that isn't there is restored.  Capture is told each table's rows, as the
 * target has them, once all that's written is.  The branch of version control
 * is made at its commit, and linked to the catalog branch.  All in the
 * caller's transaction: an abort leaves none of it, the shared storage dropped
 * as any made in it is.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/prolly/branch_ops.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_db_role_setting.h"
#include "catalog/pg_index.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "prolly/branch_catalog.h"
#include "prolly/branch_checkout.h"
#include "prolly/branch_link.h"
#include "prolly/branch_ops.h"
#include "prolly/capture.h"
#include "prolly/chunkstore.h"
#include "prolly/engine/vc.h"
#include "prolly/engine/vc_refs.h"
#include "prolly/move.h"
#include "prolly/ws/ws_dbroot.h"
#include "prolly/ws/ws_restore.h"
#include "prolly/ws/ws_root.h"
#include "prolly/ws/ws_schema.h"
#include "prolly/ws/ws_sql.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

/* What the parent's writers are held off for: the branch, and how long a lock is waited for. */
typedef struct Holding
{
	int32		branch;
	int			timeout_ms;
	TimestampTz start;			/* when the holding began */
	TimestampTz round;			/* when this round of it began */
	int			user_timeout;	/* the session's own lock_timeout, or 0 */
} Holding;

static void lock_making(const char *name);

/* (each index of a relation, as the catalogs have them now) */
static List *
indexes_of(Relation pg_index, Oid relid, List *rels)
{
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;

	ScanKeyInit(&key, Anum_pg_index_indrelid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(relid));
	scan = systable_beginscan(pg_index, IndexIndrelidIndexId, true, NULL, 1, &key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
		rels = lappend_oid(rels, ((Form_pg_index) GETSTRUCT(tup))->indexrelid);
	systable_endscan(scan);
	return rels;
}

/*
 * Each table, materialized view and sequence of a branch's schemas, its owner's to share, with its
 * TOAST table and every index of either, in order of their ids, as the catalogs have them now.
 */
static List *
relations_of(int32 branch)
{
	List	   *nsps = NIL;
	List	   *rels = NIL;
	Relation	rel;
	Relation	pg_index;
	SysScanDesc scan;
	HeapTuple	tup;

	rel = table_open(NamespaceRelationId, AccessShareLock);
	scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_namespace ns = (Form_pg_namespace) GETSTRUCT(tup);

		if (ns->nspbranch == branch)
			nsps = lappend_oid(nsps, ns->oid);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	rel = table_open(RelationRelationId, AccessShareLock);
	pg_index = table_open(IndexRelationId, AccessShareLock);
	foreach_oid(nsp, nsps)
	{
		ScanKeyData key;

		ScanKeyInit(&key, Anum_pg_class_relnamespace, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(nsp));
		scan = systable_beginscan(rel, InvalidOid, false, NULL, 1, &key);
		while (HeapTupleIsValid(tup = systable_getnext(scan)))
		{
			Form_pg_class c = (Form_pg_class) GETSTRUCT(tup);

			if (c->relkind != RELKIND_RELATION && c->relkind != RELKIND_MATVIEW &&
				c->relkind != RELKIND_SEQUENCE)
				continue;
			if (!object_ownercheck(RelationRelationId, c->oid, GetUserId()))
				aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(c->relkind), NameStr(c->relname));
			rels = indexes_of(pg_index, c->oid, lappend_oid(rels, c->oid));
			if (OidIsValid(c->reltoastrelid))
				rels = indexes_of(pg_index, c->reltoastrelid, lappend_oid(rels, c->reltoastrelid));
		}
		systable_endscan(scan);
	}
	table_close(pg_index, AccessShareLock);
	table_close(rel, AccessShareLock);
	list_free(nsps);
	list_sort(rels, list_oid_cmp);
	return rels;
}

/*
 * The parent held: its DDL and its commits waited out and held off (its catalogs' lock, and its
 * head's), and each of its relations locked so that its writers are waited out and held off, with
 * VACUUM and whatever else changes its pages.  A transaction of the parent's can take these in any
 * order, a write and then DDL, so no order of taking them rules out a deadlock: so each is waited
 * for at most half of what would find one, and past that all taken so far are let go of, giving
 * way, and taken again, until the time given is up.  A relation dropped or made meanwhile is found
 * as they're taken again.
 */
/*
 * (what's left of this round's wait, which all of it together keeps under half of what would find a
 * deadlock, as one waiting on what this holds began after this took it; and of the whole time, and
 * the session's own bound: none left, and this round gives way)
 */
static void
round_budget(Holding *h)
{
	TimestampTz now = GetCurrentTimestamp();
	long		left = Max(DeadlockTimeout / 2, 1) - TimestampDifferenceMilliseconds(h->round, now);
	long		whole = h->timeout_ms - TimestampDifferenceMilliseconds(h->start, now);
	char		ms[32];

	left = Min(left, whole);
	if (h->user_timeout > 0)
		left = Min(left, h->user_timeout);
	if (left <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
				 errmsg("a round of holding off a branch's writers ran out of time")));
	snprintf(ms, sizeof(ms), "%ld", left);
	(void) set_config_option("lock_timeout", ms, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
}

static bool
hold_try(Holding *h)
{
	List	   *rels;
	List	   *again;
	bool		same;

	round_budget(h);
	LockBranchCatalogs(h->branch, AccessExclusiveLock);
	round_budget(h);
	LockBranchHead(h->branch);
	rels = relations_of(h->branch);
	foreach_oid(relid, rels)
	{
		round_budget(h);
		LockRelationOid(relid, ExclusiveLock);
	}
	again = relations_of(h->branch);
	same = equal(rels, again);
	list_free(rels);
	list_free(again);
	return same;
}

static void
lock_writers_out(void *arg)
{
	Holding    *h = arg;
	MemoryContext cxt = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	int			nest = NewGUCNestLevel();

	h->start = GetCurrentTimestamp();
	h->user_timeout = LockTimeout;
	for (;;)
	{
		bool		held = false;
		bool		gave_way = false;

		h->round = GetCurrentTimestamp();
		BeginInternalSubTransaction(NULL);
		MemoryContextSwitchTo(cxt);
		PG_TRY();
		{
			held = hold_try(h);
		}
		PG_CATCH();
		{
			ErrorData  *e;

			MemoryContextSwitchTo(cxt);
			e = CopyErrorData();
			FlushErrorState();
			RollbackAndReleaseCurrentSubTransaction();
			MemoryContextSwitchTo(cxt);
			CurrentResourceOwner = owner;
			if (e->sqlerrcode != ERRCODE_LOCK_NOT_AVAILABLE)
				ReThrowError(e);
			FreeErrorData(e);
			gave_way = true;
		}
		PG_END_TRY();
		if (!gave_way)
		{
			if (held)
			{
				ReleaseCurrentSubTransaction();
				MemoryContextSwitchTo(cxt);
				CurrentResourceOwner = owner;
				break;
			}
			RollbackAndReleaseCurrentSubTransaction();
			MemoryContextSwitchTo(cxt);
			CurrentResourceOwner = owner;
		}
		if (TimestampDifferenceExceeds(h->start, GetCurrentTimestamp(), h->timeout_ms))
			ereport(ERROR,
					(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					 errmsg("could not hold off the writers of the branch being made from in time")));
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, 10, WAIT_EVENT_BRANCH_HOLD_GIVE_WAY);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}
	AtEOXact_GUC(true, nest);
}

typedef struct Told
{
	ProllyCtx  *ctx;
	Oid			nsp;			/* the schema being told of */
	const char *schema;
} Told;

/* (a table's map told as its rows; a partitioned table holds none of its own) */
static bool
tell_table(void *arg, const char *name, const VcTable *entry)
{
	Told	   *t = arg;
	char	   *table = ws_name_from_utf8(name);
	Oid			relid = get_relname_relid(table, t->nsp);
	Relation	rel;

	if (!OidIsValid(relid))
		elog(ERROR, "table \"%s\" of schema \"%s\" isn't there", table, t->schema);
	rel = relation_open(relid, AccessShareLock);
	if (rel->rd_rel->relkind == RELKIND_RELATION)
		CaptureRowsAre(rel, &entry->data);
	relation_close(rel, AccessShareLock);
	pfree(table);
	return true;
}

static bool
tell_schema(void *arg, const char *name, const PHash *root)
{
	Told	   *t = arg;
	char	   *schema = ws_name_from_utf8(name);

	t->schema = schema;
	t->nsp = get_namespace_oid(schema, false);
	WsRoot->tables(t->ctx, root, tell_table, t);
	pfree(schema);
	return true;
}

typedef enum MoveStep
{
	MOVE_MAKE,					/* a schema the target has that isn't there, restored */
	MOVE_KEPT,					/* one both have, moved to the target's */
	MOVE_DROP,					/* one the target lacks, dropped with all in it */
} MoveStep;

typedef struct Moving
{
	ProllyCtx  *ctx;
	const PHash *other;			/* the branch root the other side is */
	MoveStep	step;
} Moving;

static void
run_statement(const char *sql)
{
	WsSqlRows	rows;
	int			level = WsSql->begin();

	WsSql->run(sql, 0, NULL, &rows);
	ws_sql_rows_free(&rows);
	WsSql->end(level);
}

static bool
move_schema(void *arg, const char *name, const PHash *root)
{
	Moving	   *m = arg;
	char	   *schema = ws_name_from_utf8(name);
	PHash		other;
	bool		has = WsDbRoot->get_schema(m->ctx, m->other, name, &other);

	if (m->step == MOVE_DROP && !has)
		run_statement(psprintf("DROP SCHEMA %s CASCADE", quote_identifier(schema)));
	else if (m->step == MOVE_KEPT && has)
	{
		WsMoveStats stats;

		WsMove->move_from(m->ctx, schema, &other, root, &stats);
	}
	else if (m->step == MOVE_MAKE && !has)
	{
		run_statement(psprintf("CREATE SCHEMA %s VERSIONED", quote_identifier(schema)));
		WsRootIO->restore(m->ctx, root, NULL, schema);
	}
	pfree(schema);
	return true;
}

/*
 * A branch whose working set holds from made to hold to, in a transaction left out of capture
 * that holds off its writers, and capture told each of its tables' rows, as to has them: the
 * schemas to has that aren't there made first, as a schema it keeps may use what's in them; then
 * those kept moved, and last those it lacks dropped, once nothing kept uses them.
 */
static void
move_branch(ProllyCtx *ctx, int32 branch, const PHash *from, const PHash *to)
{
	int			level = CatalogBranchEnter(branch);
	Moving		m = {ctx, from, MOVE_MAKE};
	Told		told = {ctx};

	if (!phash_eq(from, to))
	{
		WsDbRoot->schemas(ctx, to, move_schema, &m);
		CommandCounterIncrement();
		m.step = MOVE_KEPT;
		WsDbRoot->schemas(ctx, to, move_schema, &m);
		CommandCounterIncrement();
		m.other = to;
		m.step = MOVE_DROP;
		WsDbRoot->schemas(ctx, from, move_schema, &m);
		CommandCounterIncrement();
	}
	WsDbRoot->schemas(ctx, to, tell_schema, &told);
	CatalogBranchLeave(level);
}

/* A commit the repository has, in its window, loaded into at: an error otherwise. */
static void
commit_check(ProllyCtx *ctx, VcRepo *repo, const PHash *commit, VcCommit *at)
{
	char		hex[PHASH_HEX_LEN + 1];

	if (vc_commit_load(ctx, commit, at))
	{
		if (vc_in_window(repo, commit))
			return;
		vc_commit_release(at);
		phash_hex(commit, hex);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("commit %s is outside the window", hex),
				 errhint("Attach the history it's in first.")));
	}
	phash_hex(commit, hex);
	ereport(ERROR,
			(errcode(ERRCODE_UNDEFINED_OBJECT),
			 errmsg("commit %s not found", hex)));
}

/*
 * A working set for branch name, shared from parent's, which from names: at commit (or from's head),
 * or with take_changes, at from's head with from's working set, from reset to its head.  With
 * new_head, the branch of version control is made too; else it's there, at commit.
 */
static int32
share_from(ProllyCtx *ctx, VcRepo *repo, const char *name, int32 parent, const char *from,
		   const PHash *commit, bool take_changes, bool new_head, int timeout_ms)
{
	int32		branch;
	VcBranch	head;
	VcBranch	taken;
	VcCommit	at;
	PHash		at_commit;
	PHash		root;
	PHash		target;
	Holding		holding;

	/*
	 * (the parent at one point, its writers held off until this transaction ends; with its DDL and
	 * its commits, as a table made meanwhile would have writers the barrier doesn't hold off, and a
	 * commit writing its head from a root taken before this one's would leave the head behind the
	 * working set: so the head is read once they're held off)
	 */
	holding.branch = parent;
	holding.timeout_ms = timeout_ms;
	CaptureBranchBarrierLeavingOut(ctx, parent, timeout_ms, lock_writers_out, &holding, &root);

	/* (the parent still the one held, its working set not given back meanwhile) */
	if (BranchLink->branch_of(from) != parent)
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("branch \"%s\" gave back its working set as a branch was being made from it", from),
				 errhint("Try again.")));
	if (BranchLink->branch_of(name) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("a branch named \"%s\" already exists", name)));
	if (vc_branch_get(repo, name, &taken))
	{
		/* (one there already is the one being given a working set, at the head it was asked at) */
		bool		moved = !new_head && commit != NULL && !phash_eq(&taken.head, commit);

		vc_branch_free(&taken);
		if (new_head)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("a branch named \"%s\" already exists", name)));
		if (moved)
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("branch \"%s\" moved as it was being given a working set", name),
					 errhint("Try again.")));
	}
	if (!vc_branch_get(repo, from, &head))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("branch \"%s\" has no commit", from)));

	/* (where it's made: the commit, or from's head as it is now) */
	at_commit = commit != NULL ? *commit : head.head;
	commit_check(ctx, repo, &at_commit, &at);
	target = take_changes ? root : at.root;

	branch = BranchIds->allocate();
	BranchCatalogCopy->copy(parent, branch, true);
	CommandCounterIncrement();

	/*
	 * (its tables hold the parent's working set, the root: what differs from what it's to hold
	 * written; and with its changes taken, the parent's written back to its head)
	 */
	move_branch(ctx, branch, &root, &target);
	if (take_changes)
		move_branch(ctx, parent, &root, &head.head_root);

	if (new_head && vc_branch_create(repo, name, &at_commit) != 0)
		elog(ERROR, "could not make branch \"%s\" of version control", name);

	/* (its working root, with the changes it took, as its tables hold it) */
	if (take_changes)
	{
		if (vc_checkout(repo, name) != 0)
			elog(ERROR, "could not check out branch \"%s\" of version control", name);
		vc_set_working(repo, &root);
	}
	BranchLink->link(name, branch);
	BranchCheckout->used(branch);
	vc_commit_release(&at);
	vc_branch_free(&head);
	CaptureLeftOutSeal();
	return branch;
}

static void
name_check(const char *name)
{
	if (name[0] == '\0' || strlen(name) >= NAMEDATALEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("a branch's name is 1 to %d bytes", NAMEDATALEN - 1)));
}

/* The materialized branches, by name, and each one's head. */
typedef struct Sources
{
	PHash		want;			/* the head a working set is made at */
	char	   *same;			/* a materialized branch at that head */
	char	   *main;			/* "main", if it's materialized */
	char	   *any;			/* any other */
} Sources;

static void
source_note(void *arg, const VcBranch *b)
{
	Sources    *src = arg;

	if (BranchLink->branch_of(b->name) == 0)
		return;
	if (src->same == NULL && phash_eq(&b->head, &src->want))
		src->same = pstrdup(b->name);
	else if (src->main == NULL && strcmp(b->name, "main") == 0)
		src->main = pstrdup(b->name);
	else if (src->any == NULL)
		src->any = pstrdup(b->name);
}

/*
 * The materialized branch to make a working set at head from: one at the same head if there is, as
 * then no row differs, or "main", or any; NULL if none has one.
 */
static char *
source_for(VcRepo *repo, const PHash *head)
{
	Sources		src = {{{0}}, NULL, NULL, NULL};

	src.want = *head;
	vc_branch_list(repo, source_note, &src);
	return src.same != NULL ? src.same : src.main != NULL ? src.main : src.any;
}

static int32
shared_create(ProllyCtx *ctx, const char *name, const char *from, const PHash *commit,
			  bool take_changes, bool materialize, int timeout_ms)
{
	ChunkStore *cs = ChunkStoreOpen("pages");
	VcRepo		repo;
	VcBranch	head;
	VcBranch	taken;
	VcCommit	at;
	int32		branch = 0;
	int32		parent = BranchLink->branch_of(from);

	if (commit != NULL && take_changes)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a branch made at a commit can't take another's changes")));
	name_check(name);
	/* (none other of the name made, renamed to it, or given a working set meanwhile) */
	lock_making(name);
	vc_repo_open(&repo, cs, VcRefsOpen("pointers", cs), &prolly_default_policy);
	if (!vc_branch_get(&repo, from, &head))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("branch \"%s\" not found", from)));
	if (BranchLink->branch_of(name) != 0 || vc_branch_get(&repo, name, &taken))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("a branch named \"%s\" already exists", name)));
	commit_check(ctx, &repo, commit != NULL ? commit : &head.head, &at);
	vc_commit_release(&at);

	if (take_changes)
	{
		/* (what it takes is in the parent's working set, so it must have one) */
		if (parent == 0)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("branch \"%s\" has no working set to take changes from", from)));
		branch = share_from(ctx, &repo, name, parent, from, NULL, true, true, timeout_ms);
	}
	else if (materialize && parent != 0)
		branch = share_from(ctx, &repo, name, parent, from, commit, false, true, timeout_ms);
	else if (materialize)
	{
		/* (from only a head: made from another that has a working set, at what from's head is) */
		const PHash *at_commit = commit != NULL ? commit : &head.head;
		char	   *source = source_for(&repo, at_commit);

		if (source == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("branch \"%s\" can't be given a working set: no branch has one to make it from", name)));
		branch = share_from(ctx, &repo, name, BranchLink->branch_of(source), source, at_commit, false, true,
							timeout_ms);
	}
	else if (vc_branch_create(&repo, name, commit != NULL ? commit : &head.head) != 0)
	{
		/* (another of that name made meanwhile) */
		if (vc_branch_get(&repo, name, &taken))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("a branch named \"%s\" already exists", name)));
		elog(ERROR, "could not make branch \"%s\" of version control", name);
	}
	vc_branch_free(&head);
	vc_repo_free(&repo);
	return branch;
}

static Oid
making_key(const char *name)
{
	return (Oid) hash_bytes((const unsigned char *) name, strlen(name));
}

/* (one name's making taken a turn at, as two making one branch at once are to make one) */
static void
lock_making(const char *name)
{
	LockDatabaseObject(DatabaseRelationId, making_key(name), 4, ExclusiveLock);
}

/* Two names' makings, in the order of their locks, so two takers of both wait rather than deadlock. */
static void
lock_making_two(const char *a, const char *b)
{
	bool		a_first = making_key(a) <= making_key(b);

	lock_making(a_first ? a : b);
	lock_making(a_first ? b : a);
}

static int32
shared_materialize(ProllyCtx *ctx, const char *name, int timeout_ms)
{
	ChunkStore *cs = ChunkStoreOpen("pages");
	int32		branch = BranchLink->branch_of(name);
	VcRepo		repo;
	VcBranch	b;
	const char *from;

	if (branch != 0)
		return branch;
	lock_making(name);
	branch = BranchLink->branch_of(name);
	if (branch != 0)
		return branch;
	vc_repo_open(&repo, cs, VcRefsOpen("pointers", cs), &prolly_default_policy);
	if (!vc_branch_get(&repo, name, &b))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("branch \"%s\" not found", name)));

	from = source_for(&repo, &b.head);
	if (from == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("branch \"%s\" can't be given a working set: no branch has one to make it from", name)));
	branch = share_from(ctx, &repo, name, BranchLink->branch_of(from), from, &b.head, false, false, timeout_ms);
	vc_branch_free(&b);
	vc_repo_free(&repo);
	return branch;
}

/* Whether a database's or role's settings name a branch as default_branch. */
static bool
named_default(const char *name)
{
	/* (none set meanwhile: what sets one waits until this transaction ends) */
	Relation	rel = table_open(DbRoleSettingRelationId, ShareLock);
	SysScanDesc scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
	HeapTuple	tup;
	char	   *setting = psprintf("default_branch=%s", name);
	bool		named = false;

	while (!named && HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		bool		isnull;
		Datum		config = heap_getattr(tup, Anum_pg_db_role_setting_setconfig, RelationGetDescr(rel), &isnull);
		Oid			db = ((Form_pg_db_role_setting) GETSTRUCT(tup))->setdatabase;

		if (isnull || (db != MyDatabaseId && db != InvalidOid))
			continue;
		{
			ArrayType  *a = DatumGetArrayTypeP(config);
			Datum	   *elems;
			int			n;

			deconstruct_array_builtin(a, TEXTOID, &elems, NULL, &n);
			for (int i = 0; i < n && !named; i++)
				named = strcmp(TextDatumGetCString(elems[i]), setting) == 0;
		}
	}
	systable_endscan(scan);
	table_close(rel, NoLock);
	return named;
}

/* Refused while a database's or role's default_branch names it. */
static void
default_check(const char *name)
{
	/* (last of the refusals, as what's set for every database is held off until this transaction ends) */
	if (named_default(name))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("branch \"%s\" is a default_branch", name)));
}

/*
 * A branch's working set given back, as its writers and commits are held off: its catalog rows and
 * the relations they hold dropped, the link let go of; true.  With keep_changes, refused while it
 * has changes not committed, or with quiet, false and nothing dropped.  Refused while it's a
 * default_branch.  (Its making's lock held.)
 */
static bool
give_back(ProllyCtx *ctx, VcRepo *repo, const char *name, int32 branch, bool keep_changes, bool quiet,
		  int timeout_ms)
{
	PHash		root;
	Holding		holding;

	holding.branch = branch;
	holding.timeout_ms = timeout_ms;
	CaptureBranchBarrierLeavingOut(ctx, branch, timeout_ms, lock_writers_out, &holding, &root);
	if (keep_changes)
	{
		VcBranch	b;
		bool		changed;

		if (!vc_branch_get(repo, name, &b))
			elog(ERROR, "branch \"%s\" has a working set and no head", name);
		changed = !phash_eq(&root, &b.head_root);
		vc_branch_free(&b);
		if (changed && quiet)
			return false;
		if (changed)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("branch \"%s\" has changes not committed", name),
					 errhint("Commit them, or reset the branch, first.")));
	}
	default_check(name);
	BranchCatalogs->drop(branch);
	(void) BranchLink->unlink(name);
	return true;
}

/* Refused for "main", which every repository has. */
static void
not_main(const char *name, const char *what)
{
	if (strcmp(name, "main") == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("branch \"main\" can't be %s", what)));
}

static void
shared_dematerialize(ProllyCtx *ctx, const char *name, int timeout_ms)
{
	ChunkStore *cs = ChunkStoreOpen("pages");
	int32		branch;
	VcRepo		repo;

	/* (it keeps its working set, so that there's always one to make another from) */
	not_main(name, "dematerialized");

	/* (no one giving it a working set meanwhile, as it's given back) */
	lock_making(name);
	branch = BranchLink->branch_of(name);
	if (branch == 0)
		return;
	if (BranchCheckout->mine(branch) || BranchCheckout->in_use(branch))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("branch \"%s\" is checked out", name)));
	vc_repo_open(&repo, cs, VcRefsOpen("pointers", cs), &prolly_default_policy);
	(void) give_back(ctx, &repo, name, branch, true, false, timeout_ms);
	CaptureLeftOutSeal();
	vc_repo_free(&repo);
}

static bool
shared_dematerialize_idle(ProllyCtx *ctx, const char *name, int idle_ms, int timeout_ms)
{
	ChunkStore *cs = ChunkStoreOpen("pages");
	int32		branch;
	VcRepo		repo;
	bool		given;

	if (strcmp(name, "main") == 0)
		return false;
	lock_making(name);
	branch = BranchLink->branch_of(name);

	/* (in_use holds off what would come on, so what has left is in unused_for) */
	if (branch == 0 || BranchCheckout->mine(branch) || BranchCheckout->in_use(branch) ||
		BranchCheckout->unused_for(branch) < idle_ms || named_default(name))
		return false;
	vc_repo_open(&repo, cs, VcRefsOpen("pointers", cs), &prolly_default_policy);
	given = give_back(ctx, &repo, name, branch, true, true, timeout_ms);

	/* (one with changes not looked at again until it has been unused as long once more) */
	if (!given)
		BranchCheckout->used(branch);
	else
		CaptureLeftOutSeal();
	vc_repo_free(&repo);
	return given;
}

/* Refused while another session is on it, unless forced; branch 0 has no working set, so none. */
static void
others_check(const char *name, int32 branch, bool force)
{
	if (!force && branch != 0 && BranchCheckout->in_use(branch))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("branch \"%s\" is checked out in another session", name),
				 errhint("Use force to change it anyway.")));
}

/*
 * Whether the branch's head is in the history of the head of the branch this session is on, or of
 * "main" if it's on none: whether deleting it loses no commit.
 */
static bool
merged(VcRepo *repo, const VcBranch *b)
{
	char	   *on = catalog_branch == 0 ? NULL : BranchLink->name_of(catalog_branch);
	VcBranch	into;
	PHash		base;
	bool		is;
	bool		found = vc_branch_get(repo, on != NULL ? on : "main", &into);

	if (on != NULL)
		pfree(on);
	if (!found)
		return false;
	is = phash_eq(&into.head, &b->head) ||
		(vc_merge_base_in(repo, &b->head, &into.head, 0, &base) == 1 && phash_eq(&base, &b->head));
	vc_branch_free(&into);
	return is;
}

/* A branch deleted, its making's lock held: refused as remove says. */
static void
remove_locked(ProllyCtx *ctx, VcRepo *repo, const char *name, bool force, int timeout_ms)
{
	VcBranch	b;
	int32		branch = BranchLink->branch_of(name);

	not_main(name, "deleted");
	if (branch != 0 && BranchCheckout->mine(branch))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("branch \"%s\" is checked out in this session", name),
				 errhint("Check out another first.")));
	others_check(name, branch, force);

	/*
	 * (its head as it is, nothing committing on it meanwhile: one with a working set has capture's
	 * commits held off, and is locked once its writers are, as a transaction that has written
	 * nothing is what holds them off; one without, at once)
	 */
	if (branch != 0)
		LockBranchHead(branch);
	if (!(branch != 0 ? vc_branch_get(repo, name, &b) : vc_branch_lock(repo, name, &b)))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("branch \"%s\" not found", name)));
	if (!force && !merged(repo, &b))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("branch \"%s\" is not fully merged", name),
				 errhint("Use force to delete it anyway.")));

	/* (its working set given back, changes and all, as its head goes) */
	if (branch != 0)
	{
		VcBranch	now;
		bool		moved;

		(void) give_back(ctx, repo, name, branch, false, false, timeout_ms);
		moved = !vc_branch_lock(repo, name, &now) || !phash_eq(&now.head, &b.head);
		if (!moved)
			vc_branch_free(&now);
		if (moved)
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("branch \"%s\" moved as it was being deleted", name),
					 errhint("Try again.")));
	}
	else
		default_check(name);
	vc_branch_free(&b);
	if (vc_branch_delete(repo, name) != VC_OK)
		elog(ERROR, "could not delete branch \"%s\" of version control", name);
}

static void
shared_remove(ProllyCtx *ctx, const char *name, bool force, int timeout_ms)
{
	ChunkStore *cs = ChunkStoreOpen("pages");
	VcRepo		repo;

	lock_making(name);
	vc_repo_open(&repo, cs, VcRefsOpen("pointers", cs), &prolly_default_policy);
	remove_locked(ctx, &repo, name, force, timeout_ms);
	CaptureLeftOutSeal();
	vc_repo_free(&repo);
}

static void
shared_rename(ProllyCtx *ctx, const char *name, const char *to, bool force, int timeout_ms)
{
	ChunkStore *cs = ChunkStoreOpen("pages");
	VcRepo		repo;
	VcBranch	b;
	int32		branch;

	name_check(to);
	not_main(name, "renamed");
	if (strcmp(name, to) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("branch \"%s\" is already named so", name)));
	not_main(to, "replaced");

	/* (both names' makings held, so neither is made, given a working set or given it back meanwhile) */
	lock_making_two(name, to);
	vc_repo_open(&repo, cs, VcRefsOpen("pointers", cs), &prolly_default_policy);
	branch = BranchLink->branch_of(name);
	others_check(name, branch, force);

	/*
	 * (its commits waited out, as one in flight would commit on the name it read, and then held;
	 * its reference locked as it's renamed, after the forced delete's barrier, which is to be in a
	 * transaction that has written nothing)
	 */
	if (branch != 0)
		LockBranchHead(branch);
	if (!vc_branch_get(&repo, name, &b))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("branch \"%s\" not found", name)));
	vc_branch_free(&b);

	/* (one with the new name already replaced, forced: deleted as a forced delete is) */
	if (vc_branch_get(&repo, to, &b))
	{
		vc_branch_free(&b);
		if (!force)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("a branch named \"%s\" already exists", to),
					 errhint("Use force to replace it.")));
		remove_locked(ctx, &repo, to, true, timeout_ms);
	}
	default_check(name);
	if (vc_branch_rename(&repo, name, to) != VC_OK)
	{
		/* (deleted meanwhile by what didn't take its making's lock) */
		if (!vc_branch_get(&repo, name, &b))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("branch \"%s\" not found", name)));
		elog(ERROR, "could not rename branch \"%s\" of version control", name);
	}
	if (branch != 0)
	{
		(void) BranchLink->unlink(name);
		BranchLink->link(to, branch);
	}
	CaptureLeftOutSeal();
	vc_repo_free(&repo);
}

const BranchOpsRoutine SharedBranchOps = {
	.name = "shared",
	.create = shared_create,
	.materialize = shared_materialize,
	.dematerialize = shared_dematerialize,
	.dematerialize_idle = shared_dematerialize_idle,
	.rename = shared_rename,
	.remove = shared_remove,
};

const BranchOpsRoutine *BranchOps = &SharedBranchOps;
