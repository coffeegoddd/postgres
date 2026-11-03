/*-------------------------------------------------------------------------
 *
 * chunk_lane.c
 *	  The lanes of chunk logs, and which live backend holds each
 *	  (prolly/chunk_lane.h).
 *
 * Each process slot has a few entries in shared memory, its own alone
 * writes, under ChunkLaneLock, which a backend takes only to take or let
 * go of a lane (once a log, as it first appends to it): the database, the
 * log and the lane it holds.  A lane is taken from those that exist that
 * no entry names, or made, its number the log's to give
 * (ChunkLogTakeLane): a hole (a lane whose making aborted) safe to give
 * again, or a new one.  The lock is held from finding the holes to the
 * maker's entry naming its number, so that no hole is a number another is
 * about to make.  A log's lanes are found from what depends on it, each
 * lane's meta page naming its number and log: the names a lane is made
 * with are no more than names.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/chunk_lane.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_depend.h"
#include "commands/defrem.h"
#include "commands/tablespace.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "prolly/chunk_index.h"
#include "prolly/chunk_lane.h"
#include "prolly/chunk_log.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#define LANES_PER_BACKEND	4

typedef struct LaneEntry
{
	Oid			db;				/* InvalidOid: unused */
	Oid			log;
	uint32		lane;
	Oid			index_lane;		/* the lane of the log's index this backend
								 * writes, its relation, once known: set
								 * before it writes there, so that a reader
								 * finds it though its catalogs do not have
								 * the lane yet */
} LaneEntry;

/* what this backend alone knows of its entries */
typedef struct LocalLane
{
	bool		making;			/* made in a transaction not yet ended */
	SubTransactionId subid;		/* ... in this subtransaction */
	uint64		used;
} LocalLane;

/* every process slot's entries, under ChunkLaneLock */
static LaneEntry *lanes;
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
					   .size = mul_size(mul_size(nslots, LANES_PER_BACKEND), sizeof(LaneEntry)),
					   .ptr = (void **) &lanes,
		);
}

static void
ChunkLaneShmemInit(void *arg)
{
	for (int i = 0; i < nslots * LANES_PER_BACKEND; i++)
		lanes[i].db = InvalidOid;
}

static LaneEntry *
my_entries(void)
{
	if (nslots == 0)
		nslots = slot_count();
	Assert(MyProcNumber >= 0 && MyProcNumber < nslots);
	return &lanes[MyProcNumber * LANES_PER_BACKEND];
}

/* (a backend exiting holds no lane) */
static void
lanes_exit(int code, Datum arg)
{
	LaneEntry  *mine = my_entries();

	LWLockAcquire(ChunkLaneLock, LW_EXCLUSIVE);
	for (int j = 0; j < LANES_PER_BACKEND; j++)
		mine[j].db = InvalidOid;
	LWLockRelease(ChunkLaneLock);
}

static void forget_lanes(Oid log);

/*
 * The lock a lane's maker holds on its number until its transaction ends
 * (prepared, until it is committed or rolled back, across a restart too):
 * a number with no lane, no holder and this lock free is a hole, its
 * making ended without the lane.
 */
static void
making_tag(LOCKTAG *tag, Oid log, uint32 lane)
{
	SET_LOCKTAG_OBJECT(*tag, MyDatabaseId, RelationRelationId, log, (uint16) lane);
}

static bool
is_being_made(Oid log, uint32 lane)
{
	LOCKTAG		tag;

	making_tag(&tag, log, lane);
	if (LockAcquire(&tag, ExclusiveLock, false, true) == LOCKACQUIRE_NOT_AVAIL)
		return true;
	LockRelease(&tag, ExclusiveLock, false);
	return false;
}

/*
 * (an entry let go of: a lane being made, its making's transaction ended
 * without it here, aborted or prepared, the lanes this backend knew of the
 * log forgotten, the lane not its to see)
 */
static void
clear_entry(int j)
{
	Oid			log = my_entries()[j].log;

	LWLockAcquire(ChunkLaneLock, LW_EXCLUSIVE);
	my_entries()[j].db = InvalidOid;
	LWLockRelease(ChunkLaneLock);
	if (local[j].making)
		forget_lanes(log);
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
		/*
		 * (prepared, the lane is the prepared transaction's, not this
		 * backend's: let go of, as though aborted, its number held by the
		 * making's lock until that transaction ends)
		 */
		if (event == XACT_EVENT_ABORT || event == XACT_EVENT_PARALLEL_ABORT ||
			event == XACT_EVENT_PREPARE)
			clear_entry(j);
		else if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_PARALLEL_COMMIT)
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
	/*
	 * (on_shmem_exit, not before_shmem_exit: registered as a backend first
	 * writes a log, maybe inside another's PG_ENSURE_ERROR_CLEANUP, whose
	 * callback must stay the latest; and run after the backend's
	 * transaction is aborted, the entry naming the lane until then)
	 */
	on_shmem_exit(lanes_exit, 0);
	RegisterXactCallback(lanes_xact, NULL);
	RegisterSubXactCallback(lanes_subxact, NULL);
	callbacks_registered = true;
}

static char *
lane_name(Relation log, uint32 lane)
{
	return psprintf("%s_lane_%u", RelationGetRelationName(log), lane);
}

/*
 * A backend's map of each log's lanes (lane -> relation), from what depends
 * on the log internally, each lane's meta page naming it: made again as a
 * lane is asked for that it lacks, or that is gone or no longer names
 * itself that lane of that log as it is opened (a relation's OID may be
 * given again once it is dropped)
 */
typedef struct LaneMap
{
	Oid			log;			/* the key: the log's, or the index's */
	bool		index;			/* a chunk index's lanes (its lane 0 a
								 * relation of its own), not a log's */
	uint32		n;
	Oid		   *relids;			/* InvalidOid: a hole */
	bool	   *checked;		/* ... its meta page read naming it so, since
								 * the relation's last invalidation */
} LaneMap;

static HTAB *lane_maps = NULL;

/*
 * (built apart and put in the map after: opening lanes may take in
 * invalidations, the log's forgetting its map meanwhile)
 */
/*
 * The lanes a chunk index may have: as many as its log has (index lane n
 * is written by the backend holding log lane n), and one if it is bound to
 * none, or to a log of no lanes (a tier).
 */
static uint32
index_lane_count(Relation idx)
{
	uint64		stamp;
	Oid			logid = PageChunkIndex.log_of(idx, &stamp);
	Relation	log;
	uint32		n = 1;

	if (!OidIsValid(logid) || (log = try_relation_open(logid, AccessShareLock)) == NULL)
		return n;
	if (RelationIsChunkLog(log))
		n = ChunkLogLanes(log);
	relation_close(log, AccessShareLock);
	return n;
}

/* (rel names itself lane number of parent) */
static bool
lane_names(Relation rel, bool index, Oid parent, uint32 number)
{
	uint32		lane;
	Oid			of;

	if (index)
		return RelationIsChunkIndex(rel) && ChunkIndexLaneOf(rel, &lane, &of) &&
			of == parent && lane == number;
	return RelationIsChunkLog(rel) && ChunkLogLaneOf(rel, &lane, &of) &&
		of == parent && lane == number;
}

static LaneMap *
map_lanes(Relation log, bool index)
{
	LaneMap    *map;
	Oid		   *relids;
	bool	   *checked;
	Relation	dep = table_open(DependRelationId, AccessShareLock);
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	uint32		n = index ? index_lane_count(log) : ChunkLogLanes(log);

	/* (the catalogs as they are now: a lane made since the last look found) */
	InvalidateCatalogSnapshot();
	relids = MemoryContextAllocZero(TopMemoryContext, sizeof(Oid) * Max(n, 1));
	checked = MemoryContextAllocZero(TopMemoryContext, sizeof(bool) * Max(n, 1));
	if (!index)
		relids[0] = RelationGetRelid(log);
	ScanKeyInit(&key[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(log)));
	scan = systable_beginscan(dep, DependReferenceIndexId, true, NULL, 2, key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend d = (Form_pg_depend) GETSTRUCT(tup);
		Relation	lane;
		uint32		number;
		Oid			parent;

		if (d->classid != RelationRelationId || d->deptype != DEPENDENCY_INTERNAL)
			continue;
		lane = try_relation_open(d->objid, AccessShareLock);
		if (lane == NULL)
			continue;
		if (index ? RelationIsChunkIndex(lane) && ChunkIndexLaneOf(lane, &number, &parent) &&
			parent == RelationGetRelid(log) && number < n :
			RelationIsChunkLog(lane) && ChunkLogLaneOf(lane, &number, &parent) &&
			parent == RelationGetRelid(log) && number > 0 && number < n)
			relids[number] = d->objid;
		relation_close(lane, AccessShareLock);
	}
	systable_endscan(scan);
	table_close(dep, AccessShareLock);
	forget_lanes(RelationGetRelid(log));
	map = hash_search(lane_maps, &RelationGetRelid(log), HASH_ENTER, NULL);
	map->index = index;
	map->n = n;
	map->relids = relids;
	map->checked = checked;
	return map;
}

static void
forget_lanes(Oid log)
{
	LaneMap    *map;

	if (lane_maps == NULL)
		return;
	map = hash_search(lane_maps, &log, HASH_FIND, NULL);
	if (map == NULL)
		return;
	if (map->relids)
		pfree(map->relids);
	if (map->checked)
		pfree(map->checked);
	(void) hash_search(lane_maps, &log, HASH_REMOVE, NULL);
}

/*
 * (a relation invalidated: a log's map forgotten; a lane's checks
 * forgotten, read again as it is next opened, in case it is gone and its
 * OID given to another)
 */
static void
lanes_relcache(Datum arg, Oid relid)
{
	HASH_SEQ_STATUS seq;
	LaneMap    *map;

	if (lane_maps == NULL)
		return;
	if (!OidIsValid(relid))
	{
		hash_seq_init(&seq, lane_maps);
		while ((map = hash_seq_search(&seq)) != NULL)
			memset(map->checked, 0, sizeof(bool) * Max(map->n, 1));
		return;
	}
	forget_lanes(relid);
	hash_seq_init(&seq, lane_maps);
	while ((map = hash_seq_search(&seq)) != NULL)
		for (uint32 l = 0; l < map->n; l++)
			if (map->relids[l] == relid)
				map->checked[l] = false;
}

static LaneMap *
lane_map(Relation log, bool index)
{
	LaneMap    *map;

	if (lane_maps == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(LaneMap);
		lane_maps = hash_create("chunk log lanes", 16, &ctl, HASH_ELEM | HASH_BLOBS);
		CacheRegisterRelcacheCallback(lanes_relcache, (Datum) 0);
	}
	map = hash_search(lane_maps, &RelationGetRelid(log), HASH_FIND, NULL);
	return map != NULL ? map : map_lanes(log, index);
}

/* lane's relation, InvalidOid if it has none; fresh if asked */
static Oid
lane_relid_kind(Relation log, uint32 lane, bool fresh, bool index)
{
	LaneMap    *map;

	if (lane == 0 && !index)
		return RelationGetRelid(log);
	map = lane_map(log, index);
	if (fresh || lane >= map->n || !OidIsValid(map->relids[lane]))
		map = map_lanes(log, index);
	return lane < map->n ? map->relids[lane] : InvalidOid;
}

static Oid
lane_relid_of(Relation log, uint32 lane, bool fresh)
{
	return lane_relid_kind(log, lane, fresh, false);
}

static Oid
lane_relid(Relation log, uint32 lane)
{
	return lane_relid_of(log, lane, false);
}

uint32
ChunkLaneCount(Relation log)
{
	return ChunkLogLanes(log);
}

static Relation
open_lane(Relation log, uint32 lane, LOCKMODE lockmode, bool index)
{
	Oid			relid;
	Relation	rel;
	LaneMap    *map;

	if (lane == 0 && !index)
		return log;
	for (int tries = 0; tries < 2; tries++)
	{
		/* (gone, or another relation, since mapped: mapped again) */
		relid = lane_relid_kind(log, lane, tries > 0, index);
		if (!OidIsValid(relid))
			return NULL;
		rel = try_relation_open(relid, lockmode);
		if (rel == NULL)
			continue;
		/* (its meta page read once, until the relation is invalidated) */
		map = lane_map(log, index);
		if (lane < map->n && map->relids[lane] == relid && map->checked[lane])
			return rel;
		if (lane_names(rel, index, RelationGetRelid(log), lane))
		{
			map = lane_map(log, index);
			if (lane < map->n && map->relids[lane] == relid)
				map->checked[lane] = true;
			return rel;
		}
		relation_close(rel, lockmode);
	}
	return NULL;
}

Relation
ChunkLaneOpen(Relation log, uint32 lane, LOCKMODE lockmode)
{
	return open_lane(log, lane, lockmode, false);
}

Relation
ChunkIndexLaneOpen(Relation idx, uint32 lane, LOCKMODE lockmode)
{
	return open_lane(idx, lane, lockmode, true);
}

uint32
ChunkIndexLaneCount(Relation idx)
{
	return lane_map(idx, true)->n;
}

void
ChunkIndexLaneClose(Relation lane)
{
	if (lane != NULL)
		relation_close(lane, NoLock);
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
make_lane(Relation log, uint32 lane, bool index)
{
	char	   *name = lane_name(log, lane);
	char	   *nsp = get_namespace_name(RelationGetNamespace(log));
	char	   *am = get_am_name(log->rd_rel->relam);
	/* (the log's, as it is: the database's named, whatever the session's default) */
	char	   *ts = get_tablespace_name(OidIsValid(log->rd_rel->reltablespace) ?
										 log->rd_rel->reltablespace : MyDatabaseTableSpace);
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
			elog(ERROR, "could not make lane \"%s\" of \"%s\"",
				 name, RelationGetRelationName(log));
		SPI_finish();
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(save_userid, save_sec_context);
	}
	PG_END_TRY();
	CommandCounterIncrement();

	made = relation_open(get_relname_relid(name, RelationGetNamespace(log)), RowExclusiveLock);
	ObjectAddressSet(laneaddr, RelationRelationId, RelationGetRelid(made));
	ObjectAddressSet(logaddr, RelationRelationId, RelationGetRelid(log));
	recordDependencyOn(&laneaddr, &logaddr, DEPENDENCY_INTERNAL);
	CommandCounterIncrement();
	if (index)
	{
		ChunkIndexMakeLane(made, lane, log);

		/*
		 * (others' maps of the index's lanes made again as they next lock
		 * it, this one committed: none misses the lane's entries)
		 */
		CacheInvalidateRelcache(log);
	}
	else
		ChunkLogMakeLane(made, lane, log);
	relation_close(made, NoLock);
	(void) lane_relid_kind(log, lane, true, index);
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
	uint32	   *holes;
	Oid			index;
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
	 * the lanes there are (as the catalogs say, read before the lock is
	 * taken), those no live backend holds, and the holes: numbers given with
	 * no lane and no holder, their making aborted
	 */
	nlanes = ChunkLaneCount(log);
	index = ChunkLogIndexOf(log);
	exists = palloc0(sizeof(bool) * nlanes);
	held = palloc0(sizeof(bool) * nlanes);
	holes = palloc(sizeof(uint32) * nlanes);
	for (uint32 l = 0; l < nlanes; l++)
		exists[l] = OidIsValid(lane_relid(log, l));
	LWLockAcquire(ChunkLaneLock, LW_EXCLUSIVE);
	for (int i = 0; i < nslots * LANES_PER_BACKEND; i++)
	{
		LaneEntry  *e = &lanes[i];

		if (e->db == MyDatabaseId && e->log == RelationGetRelid(log) && e->lane < nlanes)
			held[e->lane] = true;
	}
	/* (nor one whose index lane is being made, a prepared transaction's) */
	for (lane = 0; lane < nlanes &&
		 (held[lane] || !exists[lane] ||
		  (OidIsValid(index) && is_being_made(index, lane))); lane++)
		;
	make = lane == nlanes;
	if (make)
	{
		int			nholes = 0;

		/* (none whose making's transaction is still running, prepared too) */
		for (uint32 l = 1; l < nlanes; l++)
			if (!held[l] && !exists[l] && !is_being_made(RelationGetRelid(log), l))
				holes[nholes++] = l;
		/*
		 * (a hole made since the catalogs were read is one its maker's entry
		 * named under this lock, so held: a hole read stale is one made and
		 * its maker gone, which ChunkLogTakeLane gives again only once seen
		 * again later, after a pass of VACUUM)
		 */
		lane = ChunkLogTakeLane(log, holes, nholes);
	}
	mine[slot].db = MyDatabaseId;
	mine[slot].log = RelationGetRelid(log);
	mine[slot].lane = lane;
	mine[slot].index_lane = InvalidOid;
	LWLockRelease(ChunkLaneLock);
	pfree(held);
	pfree(exists);
	pfree(holes);
	local[slot].used = ++use_clock;
	if (make)
	{
		LOCKTAG		tag;

		local[slot].making = true;
		local[slot].subid = GetCurrentSubTransactionId();

		/*
		 * (free: a new number's no one has had, a hole's no one holds; while
		 * the entry names the lane, so that no one finds it a hole meanwhile)
		 */
		making_tag(&tag, RelationGetRelid(log), lane);
		if (LockAcquire(&tag, ExclusiveLock, false, true) == LOCKACQUIRE_NOT_AVAIL)
			elog(ERROR, "lane %u of chunk log \"%s\" is being made by another",
				 lane, RelationGetRelationName(log));
		make_lane(log, lane, false);
	}
	*lane_out = lane;
	rel = ChunkLaneOpen(log, lane, RowExclusiveLock);
	if (rel == NULL)
		elog(ERROR, "lane %u of chunk log \"%s\" is gone", lane, RelationGetRelationName(log));
	return rel;
}

/*
 * The lane of idx this backend writes: the one numbered as the lane of log
 * it holds (made if it has none yet, in this transaction, under the
 * making lock of its number, as a log's lane is), opened in
 * RowExclusiveLock; its number in *lane.
 */
Relation
ChunkIndexLaneForWrite(Relation idx, Relation log, uint32 *lane_out)
{
	uint32		lane;
	Relation	rel = ChunkLaneForWrite(log, &lane);
	Relation	ir;

	ChunkLaneClose(log, rel);
	*lane_out = lane;
	ir = ChunkIndexLaneOpen(idx, lane, RowExclusiveLock);
	if (ir == NULL)
	{
		LOCKTAG		tag;

		/*
		 * (none else makes it: the lane of log is this backend's, and none
		 * takes a lane of log whose index lane is being made, a prepared
		 * transaction's making included)
		 */
		making_tag(&tag, RelationGetRelid(idx), lane);
		if (LockAcquire(&tag, ExclusiveLock, false, true) == LOCKACQUIRE_NOT_AVAIL)
			elog(ERROR, "lane %u of chunk index \"%s\" is being made by another",
				 lane, RelationGetRelationName(idx));
		make_lane(idx, lane, true);
		ir = ChunkIndexLaneOpen(idx, lane, RowExclusiveLock);
		if (ir == NULL)
			elog(ERROR, "lane %u of chunk index \"%s\" made, and not found",
				 lane, RelationGetRelationName(idx));
	}

	/* (named in this backend's entry before anything is written there) */
	{
		LaneEntry  *mine = my_entries();

		for (int j = 0; j < LANES_PER_BACKEND; j++)
			if (mine[j].db == MyDatabaseId && mine[j].log == RelationGetRelid(log) &&
				mine[j].lane == lane && mine[j].index_lane != RelationGetRelid(ir))
			{
				LWLockAcquire(ChunkLaneLock, LW_EXCLUSIVE);
				mine[j].index_lane = RelationGetRelid(ir);
				LWLockRelease(ChunkLaneLock);
			}
	}
	return ir;
}

/*
 * relid opened in lockmode if it exists as the catalogs are now, else NULL:
 * looked for afresh, not through the system caches, which may remember it
 * missing from a look before its transaction committed.  Looked for before
 * it is locked: a lane its maker has not committed is no lane yet, and its
 * maker's lock on it is not to be waited for.
 */
static Relation
open_fresh(Oid relid, LOCKMODE lockmode)
{
	Relation	pg_class;
	ScanKeyData key;
	SysScanDesc scan;
	bool		exists;

	InvalidateCatalogSnapshot();
	pg_class = table_open(RelationRelationId, AccessShareLock);
	ScanKeyInit(&key, Anum_pg_class_oid, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	scan = systable_beginscan(pg_class, ClassOidIndexId, true, NULL, 1, &key);
	exists = HeapTupleIsValid(systable_getnext(scan));
	systable_endscan(scan);
	table_close(pg_class, AccessShareLock);
	if (!exists)
		return NULL;
	LockRelationOid(relid, lockmode);
	InvalidateCatalogSnapshot();
	pg_class = table_open(RelationRelationId, AccessShareLock);
	ScanKeyInit(&key, Anum_pg_class_oid, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	scan = systable_beginscan(pg_class, ClassOidIndexId, true, NULL, 1, &key);
	exists = HeapTupleIsValid(systable_getnext(scan));
	systable_endscan(scan);
	table_close(pg_class, AccessShareLock);
	if (!exists)
	{
		UnlockRelationOid(relid, lockmode);
		return NULL;
	}
	return relation_open(relid, NoLock);
}

/*
 * Every lane of idx a reader is to look in, opened in lockmode, into
 * *lanes (palloc'd): how many.  Those this backend's map of them has (made
 * again if the catalogs changed since: the invalidations taken in first),
 * and those the live backends' entries name, which a lane is named in
 * before anything is written there: a lane made by a transaction just
 * committed, whose catalogs others may not see yet, is found so.  (The
 * entries read first: a backend gone since had sent its invalidations.)
 */
int
ChunkIndexLanesOpen(Relation idx, LOCKMODE lockmode, Relation **lanes_out)
{
	uint64		stamp;
	Oid			log = PageChunkIndex.log_of(idx, &stamp);
	Oid		   *named;
	int			nnamed = 0;
	LaneMap    *map;
	Relation   *out;
	int			nout = 0,
				cap;

	if (nslots == 0)
		nslots = slot_count();
	/* (in the transaction's memory: not the caller's, which a scan leaves as it was) */
	named = MemoryContextAlloc(TopTransactionContext, sizeof(Oid) * nslots * LANES_PER_BACKEND);
	if (OidIsValid(log))
	{
		LWLockAcquire(ChunkLaneLock, LW_SHARED);
		for (int i = 0; i < nslots * LANES_PER_BACKEND; i++)
		{
			LaneEntry  *e = &lanes[i];

			if (e->db == MyDatabaseId && e->log == log && OidIsValid(e->index_lane))
				named[nnamed++] = e->index_lane;
		}
		LWLockRelease(ChunkLaneLock);
	}
	AcceptInvalidationMessages();
	map = lane_map(idx, true);
	cap = map->n + nnamed + 1;
	out = MemoryContextAlloc(TopTransactionContext, sizeof(Relation) * cap);
	for (uint32 l = 0; l < map->n; l++)
	{
		Relation	rel = ChunkIndexLaneOpen(idx, l, lockmode);

		if (rel != NULL)
			out[nout++] = rel;
	}
	for (int i = 0; i < nnamed; i++)
	{
		bool		have = false;
		Relation	rel;
		uint32		number;
		Oid			parent;

		for (int j = 0; j < nout && !have; j++)
			have = RelationGetRelid(out[j]) == named[i];
		if (have)
			continue;
		rel = open_fresh(named[i], lockmode);
		if (rel == NULL)
			continue;
		if (RelationIsChunkIndex(rel) && ChunkIndexLaneOf(rel, &number, &parent) &&
			parent == RelationGetRelid(idx))
			out[nout++] = rel;
		else
			relation_close(rel, lockmode);
	}
	pfree(named);
	*lanes_out = out;
	return nout;
}

void
ChunkIndexLanesClose(Relation *lanes, int n)
{
	for (int i = 0; i < n; i++)
		relation_close(lanes[i], NoLock);
	if (lanes)
		pfree(lanes);
}
