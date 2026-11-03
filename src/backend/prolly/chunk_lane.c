/*-------------------------------------------------------------------------
 *
 * chunk_lane.c
 *	  The lanes of chunk logs, and which live backend holds each
 *	  (prolly/chunk_lane.h).
 *
 * Each process slot has a few entries in shared memory, its own alone
 * writes, under one spinlock that a backend takes only to take or let go
 * of a lane (once a log, as it first appends to it): the database, the
 * log and the lane it holds.  A lane is taken from those that exist (as
 * the catalogs say) that no entry names, or made, its number the log's
 * next (its meta page's, never given again: an aborted making's lane is
 * gone, and entries its transaction left name it, which no lane must be).
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/chunk_lane.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "access/twophase.h"
#include "catalog/pg_class.h"
#include "commands/defrem.h"
#include "commands/tablespace.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "prolly/chunk_lane.h"
#include "prolly/chunk_log.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#define LANES_PER_BACKEND	4

typedef struct LaneEntry
{
	Oid			db;				/* InvalidOid: unused */
	Oid			log;
	uint32		lane;
} LaneEntry;

typedef struct LaneShared
{
	slock_t		lock;
	LaneEntry	entries[FLEXIBLE_ARRAY_MEMBER];
} LaneShared;

/* what this backend alone knows of its entries */
typedef struct LocalLane
{
	bool		making;			/* made in a transaction not yet ended */
	SubTransactionId subid;		/* ... in this subtransaction */
	uint64		used;
} LocalLane;

static LaneShared *lanes;
static int	nslots;
static LocalLane local[LANES_PER_BACKEND];
static uint64 use_clock = 0;
static bool callbacks_registered = false;

static void ChunkLaneShmemRequest(void *arg);
static void ChunkLaneShmemInit(void *arg);

const ShmemCallbacks ChunkLaneShmemCallbacks = {
	.request_fn = ChunkLaneShmemRequest,
	.init_fn = ChunkLaneShmemInit,
};

static int
slot_count(void)
{
	/* (every process that may run a transaction has a number below it) */
	return MaxBackends + NUM_AUXILIARY_PROCS + max_prepared_xacts;
}

static void
ChunkLaneShmemRequest(void *arg)
{
	nslots = slot_count();
	ShmemRequestStruct(.name = "Chunk Log Lanes",
					   .size = add_size(offsetof(LaneShared, entries),
										mul_size(mul_size(nslots, LANES_PER_BACKEND),
												 sizeof(LaneEntry))),
					   .ptr = (void **) &lanes,
		);
}

static void
ChunkLaneShmemInit(void *arg)
{
	SpinLockInit(&lanes->lock);
	for (int i = 0; i < nslots * LANES_PER_BACKEND; i++)
		lanes->entries[i].db = InvalidOid;
}

static LaneEntry *
my_entries(void)
{
	if (nslots == 0)
		nslots = slot_count();
	Assert(MyProcNumber >= 0 && MyProcNumber < nslots);
	return &lanes->entries[MyProcNumber * LANES_PER_BACKEND];
}

/* (a backend exiting holds no lane) */
static void
lanes_exit(int code, Datum arg)
{
	LaneEntry  *mine = my_entries();

	SpinLockAcquire(&lanes->lock);
	for (int j = 0; j < LANES_PER_BACKEND; j++)
		mine[j].db = InvalidOid;
	SpinLockRelease(&lanes->lock);
}

static void
clear_entry(int j)
{
	SpinLockAcquire(&lanes->lock);
	my_entries()[j].db = InvalidOid;
	SpinLockRelease(&lanes->lock);
	local[j].making = false;
}

/* (a lane made in a transaction, or subtransaction, that aborts is gone) */
static void
lanes_xact(XactEvent event, void *arg)
{
	for (int j = 0; j < LANES_PER_BACKEND; j++)
	{
		if (!local[j].making)
			continue;
		if (event == XACT_EVENT_ABORT || event == XACT_EVENT_PARALLEL_ABORT)
			clear_entry(j);
		else if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_PARALLEL_COMMIT ||
				 event == XACT_EVENT_PREPARE)
			local[j].making = false;
	}
}

static void
lanes_subxact(SubXactEvent event, SubTransactionId mySubid,
			  SubTransactionId parentSubid, void *arg)
{
	for (int j = 0; j < LANES_PER_BACKEND; j++)
	{
		if (!local[j].making || local[j].subid != mySubid)
			continue;
		if (event == SUBXACT_EVENT_ABORT_SUB)
			clear_entry(j);
		else if (event == SUBXACT_EVENT_COMMIT_SUB)
			local[j].subid = parentSubid;
	}
}

static void
register_callbacks(void)
{
	if (callbacks_registered)
		return;
	before_shmem_exit(lanes_exit, 0);
	RegisterXactCallback(lanes_xact, NULL);
	RegisterSubXactCallback(lanes_subxact, NULL);
	callbacks_registered = true;
}

static char *
lane_name(Relation log, uint32 lane)
{
	return psprintf("%s_lane_%u", RelationGetRelationName(log), lane);
}

static Oid
lane_relid(Relation log, uint32 lane)
{
	char	   *name;
	Oid			relid;

	if (lane == 0)
		return RelationGetRelid(log);
	name = lane_name(log, lane);
	relid = get_relname_relid(name, RelationGetNamespace(log));
	pfree(name);
	return relid;
}

uint32
ChunkLaneCount(Relation log)
{
	return ChunkLogLanes(log);
}

Relation
ChunkLaneOpen(Relation log, uint32 lane, LOCKMODE lockmode)
{
	Oid			relid;

	if (lane == 0)
		return log;
	relid = lane_relid(log, lane);
	if (!OidIsValid(relid))
		return NULL;
	return try_relation_open(relid, lockmode);
}

void
ChunkLaneClose(Relation log, Relation lane)
{
	if (lane != NULL && lane != log)
		relation_close(lane, NoLock);
}

/*
 * The next lane made: a relation of log's access method beside it, in its
 * tablespace, owned by its owner, internally dependent on it, its meta page
 * naming it a lane of log's (and bound to log's index, if log is bound)
 */
static void
make_lane(Relation log, uint32 lane)
{
	char	   *name = lane_name(log, lane);
	char	   *nsp = get_namespace_name(RelationGetNamespace(log));
	char	   *am = get_am_name(log->rd_rel->relam);
	char	   *ts = OidIsValid(log->rd_rel->reltablespace) ?
		get_tablespace_name(log->rd_rel->reltablespace) : NULL;
	StringInfoData sql;
	Oid			save_userid;
	int			save_sec_context;
	Relation	made;
	ObjectAddress laneaddr,
				logaddr;

	initStringInfo(&sql);
	appendStringInfo(&sql, "CREATE TABLE %s.%s () USING %s",
					 quote_identifier(nsp), quote_identifier(name), quote_identifier(am));
	if (ts)
		appendStringInfo(&sql, " TABLESPACE %s", quote_identifier(ts));

	/* (as the log's owner, whoever appends) */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(log->rd_rel->relowner,
						   save_sec_context | SECURITY_LOCAL_USERID_CHANGE |
						   SECURITY_RESTRICTED_OPERATION);
	PG_TRY();
	{
		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "SPI_connect failed");
		if (SPI_execute(sql.data, false, 0) != SPI_OK_UTILITY)
			elog(ERROR, "could not make lane \"%s\" of chunk log \"%s\"",
				 name, RelationGetRelationName(log));
		SPI_finish();
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(save_userid, save_sec_context);
	}
	PG_END_TRY();
	CommandCounterIncrement();

	made = relation_open(lane_relid(log, lane), RowExclusiveLock);
	ObjectAddressSet(laneaddr, RelationRelationId, RelationGetRelid(made));
	ObjectAddressSet(logaddr, RelationRelationId, RelationGetRelid(log));
	recordDependencyOn(&laneaddr, &logaddr, DEPENDENCY_INTERNAL);
	CommandCounterIncrement();
	ChunkLogMakeLane(made, lane, log);
	relation_close(made, NoLock);
	pfree(name);
	pfree(sql.data);
}

Relation
ChunkLaneForWrite(Relation log, uint32 *lane_out)
{
	LaneEntry  *mine;
	uint32		nlanes;
	uint32		lane = 0;
	int			slot = -1;
	bool		make = false;
	bool	   *held;
	bool	   *exists;
	Relation	rel;

	register_callbacks();
	mine = my_entries();

	/* (the lane held: unless its making aborted, or it is gone) */
	for (int j = 0; j < LANES_PER_BACKEND; j++)
		if (mine[j].db == MyDatabaseId && mine[j].log == RelationGetRelid(log))
		{
			local[j].used = ++use_clock;
			*lane_out = mine[j].lane;
			rel = ChunkLaneOpen(log, mine[j].lane, RowExclusiveLock);
			if (rel != NULL)
				return rel;
			clear_entry(j);
			break;
		}

	/* (a slot for it: one free, or the least used, let go of) */
	for (int j = 0; j < LANES_PER_BACKEND; j++)
		if (mine[j].db == InvalidOid && !local[j].making)
		{
			slot = j;
			break;
		}
	if (slot < 0)
	{
		for (int j = 0; j < LANES_PER_BACKEND; j++)
			if (!local[j].making && (slot < 0 || local[j].used < local[slot].used))
				slot = j;
		if (slot < 0)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("a transaction makes lanes of at most %d chunk logs",
							LANES_PER_BACKEND)));
		clear_entry(slot);
	}

	/*
	 * the lanes there are (those whose making aborted are gone, their
	 * numbers never given again), and those no live backend holds
	 */
	nlanes = ChunkLaneCount(log);
	exists = palloc0(sizeof(bool) * nlanes);
	for (uint32 l = 0; l < nlanes; l++)
		exists[l] = OidIsValid(lane_relid(log, l));
	SpinLockAcquire(&lanes->lock);
	held = palloc0(sizeof(bool) * nlanes);
	for (int i = 0; i < nslots * LANES_PER_BACKEND; i++)
	{
		LaneEntry  *e = &lanes->entries[i];

		if (e->db == MyDatabaseId && e->log == RelationGetRelid(log) && e->lane < nlanes)
			held[e->lane] = true;
	}
	for (lane = 0; lane < nlanes && (held[lane] || !exists[lane]); lane++)
		;
	make = lane == nlanes;
	if (!make)
	{
		mine[slot].db = MyDatabaseId;
		mine[slot].log = RelationGetRelid(log);
		mine[slot].lane = lane;
	}
	SpinLockRelease(&lanes->lock);
	pfree(held);
	pfree(exists);
	if (make)
	{
		/* (all held: the next number, never given before, its own) */
		lane = ChunkLogTakeLane(log);
		SpinLockAcquire(&lanes->lock);
		mine[slot].db = MyDatabaseId;
		mine[slot].log = RelationGetRelid(log);
		mine[slot].lane = lane;
		SpinLockRelease(&lanes->lock);
		local[slot].making = true;
		local[slot].subid = GetCurrentSubTransactionId();
	}
	local[slot].used = ++use_clock;
	if (make)
		make_lane(log, lane);
	*lane_out = lane;
	rel = ChunkLaneOpen(log, lane, RowExclusiveLock);
	if (rel == NULL)
		elog(ERROR, "lane %u of chunk log \"%s\" is gone", lane, RelationGetRelationName(log));
	return rel;
}
