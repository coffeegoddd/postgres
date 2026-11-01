/*-------------------------------------------------------------------------
 *
 * chunk_region.c
 *	  The regions of chunk logs live backends hold (prolly/chunk_region.h).
 *
 * Each backend has REGIONS_PER_BACKEND entries, a log's, its generation and
 * its region's pages each, which only it writes, under a sequence count
 * each (odd while it writes), so that a reader takes an entry's whole
 * value, retrying while it changes.  The backend keeps, in its memory, the
 * relation each entry is of, whether it has forgotten the region (its
 * relcache entry invalidated, and not kept again since), and when it last
 * used it, to make room by the one used longest ago.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/chunk_region.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/twophase.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "prolly/chunk_region.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/s_lock.h"
#include "storage/shmem.h"
#include "utils/inval.h"

#define REGIONS_PER_BACKEND	8

typedef struct RegionEntry
{
	pg_atomic_uint32 seq;		/* odd while being written */
	RelFileLocator log;			/* relNumber InvalidRelFileNumber: unused */
	uint32		generation;
	BlockNumber first;
	BlockNumber end;
} RegionEntry;

typedef struct RegionTable
{
	RegionEntry entries[REGIONS_PER_BACKEND];
} RegionTable;

/* what this backend alone knows of its entries */
typedef struct LocalEntry
{
	Oid			relid;
	bool		forgotten;
	uint64		used;
} LocalEntry;

static RegionTable *region_tables;
static int	nregion_tables;
static LocalEntry local[REGIONS_PER_BACKEND];
static uint64 use_clock = 0;
static bool callbacks_registered = false;
static bool appending = false;	/* between a region's prepare and release */

static void ChunkRegionShmemRequest(void *arg);
static void ChunkRegionShmemInit(void *arg);

const ShmemCallbacks ChunkRegionShmemCallbacks = {
	.request_fn = ChunkRegionShmemRequest,
	.init_fn = ChunkRegionShmemInit,
};

static int
region_table_count(void)
{
	/* (every process that may run a transaction has a number below it) */
	return MaxBackends + NUM_AUXILIARY_PROCS + max_prepared_xacts;
}

static void
ChunkRegionShmemRequest(void *arg)
{
	nregion_tables = region_table_count();
	ShmemRequestStruct(.name = "Chunk Log Regions",
					   .size = mul_size(nregion_tables, sizeof(RegionTable)),
					   .ptr = (void **) &region_tables,
		);
}

static void
ChunkRegionShmemInit(void *arg)
{
	for (int i = 0; i < nregion_tables; i++)
		for (int j = 0; j < REGIONS_PER_BACKEND; j++)
		{
			RegionEntry *e = &region_tables[i].entries[j];

			pg_atomic_init_u32(&e->seq, 0);
			e->log.relNumber = InvalidRelFileNumber;
		}
}

static RegionTable *
my_table(void)
{
	if (nregion_tables == 0)
		nregion_tables = region_table_count();
	Assert(MyProcNumber >= 0 && MyProcNumber < nregion_tables);
	return &region_tables[MyProcNumber];
}

static void
write_entry(RegionEntry *e, RelFileLocator log, uint32 generation,
			BlockNumber first, BlockNumber end)
{
	pg_atomic_fetch_add_u32(&e->seq, 1);
	pg_write_barrier();
	e->log = log;
	e->generation = generation;
	e->first = first;
	e->end = end;
	pg_write_barrier();
	pg_atomic_fetch_add_u32(&e->seq, 1);
}

static void
clear_entry(int j)
{
	RelFileLocator none = {0};

	write_entry(&my_table()->entries[j], none, 0, 0, 0);
	local[j].relid = InvalidOid;
	local[j].forgotten = false;
}

/*
 * (a backend exiting holds no region: once its transaction is ended, an
 * entry it was putting found aborted by any VACUUM that finds its region
 * not held)
 */
static void
clear_regions(int code, Datum arg)
{
	RegionTable *t = my_table();

	for (int j = 0; j < REGIONS_PER_BACKEND; j++)
		if (RelFileNumberIsValid(t->entries[j].log.relNumber))
			clear_entry(j);
	pg_memory_barrier();
}

/*
 * (a relcache entry invalidated forgets the region kept in it: let go of
 * at once if no append runs, or else as the transaction ends unless the
 * append keeps it again)
 */
static void
forget_regions(Datum arg, Oid relid)
{
	for (int j = 0; j < REGIONS_PER_BACKEND; j++)
		if (OidIsValid(local[j].relid) &&
			(!OidIsValid(relid) || local[j].relid == relid))
		{
			if (appending)
				local[j].forgotten = true;
			else
				clear_entry(j);
		}
	pg_memory_barrier();
}

/*
 * (as a transaction ends, no append running, a region forgotten is let go
 * of: none of this backend's appends goes in it again, its entry checked
 * first)
 */
static void
drop_forgotten(XactEvent event, void *arg)
{
	RegionTable *t;

	if (event != XACT_EVENT_COMMIT && event != XACT_EVENT_ABORT &&
		event != XACT_EVENT_PREPARE)
		return;
	appending = false;
	t = my_table();
	for (int j = 0; j < REGIONS_PER_BACKEND; j++)
		if (local[j].forgotten && RelFileNumberIsValid(t->entries[j].log.relNumber))
			clear_entry(j);
	pg_memory_barrier();
}

static int
entry_of(RelFileLocator log)
{
	RegionTable *t = my_table();

	for (int j = 0; j < REGIONS_PER_BACKEND; j++)
		if (RelFileLocatorEquals(t->entries[j].log, log))
			return j;
	return -1;
}

void
ChunkRegionPublish(RelFileLocator log, Oid relid, uint32 generation,
				   BlockNumber first, BlockNumber end)
{
	RegionTable *t = my_table();
	int			j;

	if (!callbacks_registered)
	{
		on_shmem_exit(clear_regions, (Datum) 0);
		CacheRegisterRelcacheCallback(forget_regions, (Datum) 0);
		RegisterXactCallback(drop_forgotten, NULL);
		callbacks_registered = true;
	}
	j = entry_of(log);
	if (j < 0)
	{
		/*
		 * an entry unused, or else the one used longest ago, its log's
		 * region let go of by this backend (it appends there only in a
		 * region it publishes again)
		 */
		for (int k = 0; k < REGIONS_PER_BACKEND; k++)
			if (!RelFileNumberIsValid(t->entries[k].log.relNumber))
			{
				j = k;
				break;
			}
		if (j < 0)
		{
			j = 0;
			for (int k = 1; k < REGIONS_PER_BACKEND; k++)
				if (local[k].used < local[j].used)
					j = k;
		}
	}
	write_entry(&t->entries[j], log, generation, first, end);
	local[j].relid = relid;
	local[j].forgotten = false;
	local[j].used = ++use_clock;

	/* (before any page of it is made: see chunk_log.c's callers) */
	pg_memory_barrier();
}

bool
ChunkRegionMine(RelFileLocator log, BlockNumber end)
{
	int			j = entry_of(log);

	if (j < 0 || my_table()->entries[j].end != end)
		return false;
	local[j].used = ++use_clock;
	return true;
}

void
ChunkRegionAppending(bool on)
{
	appending = on;
}

void
ChunkRegionKept(RelFileLocator log)
{
	int			j = entry_of(log);

	if (j >= 0)
		local[j].forgotten = false;
}

/* an entry's whole value, as written last */
static void
read_entry(RegionEntry *e, RelFileLocator *log, uint32 *generation,
		   BlockNumber *first, BlockNumber *end)
{
	for (;;)
	{
		uint32		s1 = pg_atomic_read_u32(&e->seq);

		if (s1 & 1)
		{
			SPIN_DELAY();
			continue;
		}
		pg_read_barrier();
		*log = e->log;
		*generation = e->generation;
		*first = e->first;
		*end = e->end;
		pg_read_barrier();
		if (pg_atomic_read_u32(&e->seq) == s1)
			return;
	}
}

static int
cmp_block(const void *a, const void *b)
{
	BlockNumber x = *(const BlockNumber *) a;
	BlockNumber y = *(const BlockNumber *) b;

	return x < y ? -1 : x > y ? 1 : 0;
}

ChunkRegionSet *
ChunkRegionLive(RelFileLocator log, uint32 generation)
{
	ChunkRegionSet *set = palloc0(sizeof(ChunkRegionSet));
	BlockNumber *pairs;
	int			cap = 16;

	if (nregion_tables == 0)
		nregion_tables = region_table_count();
	pairs = palloc(cap * 2 * sizeof(BlockNumber));
	pg_memory_barrier();
	for (int i = 0; i < nregion_tables; i++)
		for (int j = 0; j < REGIONS_PER_BACKEND; j++)
		{
			RelFileLocator l;
			uint32		gen;
			BlockNumber first,
						end;

			read_entry(&region_tables[i].entries[j], &l, &gen, &first, &end);
			if (!RelFileLocatorEquals(l, log) || gen != generation)
				continue;
			if (set->n == cap)
			{
				cap *= 2;
				pairs = repalloc(pairs, cap * 2 * sizeof(BlockNumber));
			}
			pairs[set->n * 2] = first;
			pairs[set->n * 2 + 1] = end;
			set->n++;
		}

	/* by first page, each end the furthest of those up to it */
	qsort(pairs, set->n, 2 * sizeof(BlockNumber), cmp_block);
	set->first = palloc(Max(set->n, 1) * sizeof(BlockNumber));
	set->end = palloc(Max(set->n, 1) * sizeof(BlockNumber));
	for (int k = 0; k < set->n; k++)
	{
		set->first[k] = pairs[k * 2];
		set->end[k] = Max(pairs[k * 2 + 1], k > 0 ? set->end[k - 1] : 0);
	}
	pfree(pairs);
	return set;
}

bool
ChunkRegionSetHas(const ChunkRegionSet *set, BlockNumber blk)
{
	int			lo = 0,
				hi = set->n;

	/* the last region starting at blk or before */
	while (lo < hi)
	{
		int			mid = (lo + hi) / 2;

		if (set->first[mid] <= blk)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo > 0 && set->end[lo - 1] > blk;
}
