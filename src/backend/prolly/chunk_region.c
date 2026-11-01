/*-------------------------------------------------------------------------
 *
 * chunk_region.c
 *	  The regions of chunk logs live backends hold (prolly/chunk_region.h).
 *
 * Each backend has REGIONS_PER_BACKEND entries, a log's and its region's
 * pages each, which only it writes, under a sequence count each (odd while
 * it writes), so that a reader takes an entry's whole value, retrying while
 * it changes.  An entry for a log is replaced by the backend's next region
 * in it; a backend's entries are cleared as it exits.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/chunk_region.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/twophase.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "prolly/chunk_region.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/shmem.h"

#define REGIONS_PER_BACKEND	8

typedef struct RegionEntry
{
	pg_atomic_uint32 seq;		/* odd while being written */
	RelFileLocator log;			/* relNumber InvalidRelFileNumber: unused */
	BlockNumber first;
	BlockNumber end;
} RegionEntry;

typedef struct RegionTable
{
	pg_atomic_uint32 full;		/* counts as holding every page */
	RegionEntry entries[REGIONS_PER_BACKEND];
} RegionTable;

static RegionTable *region_tables;
static int	nregion_tables;
static bool exit_registered = false;

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
	{
		pg_atomic_init_u32(&region_tables[i].full, 0);
		for (int j = 0; j < REGIONS_PER_BACKEND; j++)
		{
			RegionEntry *e = &region_tables[i].entries[j];

			pg_atomic_init_u32(&e->seq, 0);
			e->log.relNumber = InvalidRelFileNumber;
		}
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
write_entry(RegionEntry *e, RelFileLocator log, BlockNumber first, BlockNumber end)
{
	pg_atomic_fetch_add_u32(&e->seq, 1);
	pg_write_barrier();
	e->log = log;
	e->first = first;
	e->end = end;
	pg_write_barrier();
	pg_atomic_fetch_add_u32(&e->seq, 1);
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
	RelFileLocator none = {0};

	for (int j = 0; j < REGIONS_PER_BACKEND; j++)
		if (RelFileNumberIsValid(t->entries[j].log.relNumber))
			write_entry(&t->entries[j], none, 0, 0);
	pg_write_barrier();
	pg_atomic_write_u32(&t->full, 0);
}

void
ChunkRegionPublish(RelFileLocator log, BlockNumber first, BlockNumber end)
{
	RegionTable *t = my_table();
	RegionEntry *free_entry = NULL;

	if (!exit_registered)
	{
		on_shmem_exit(clear_regions, (Datum) 0);
		exit_registered = true;
	}
	for (int j = 0; j < REGIONS_PER_BACKEND; j++)
	{
		RegionEntry *e = &t->entries[j];

		if (RelFileLocatorEquals(e->log, log))
		{
			write_entry(e, log, first, end);
			pg_memory_barrier();
			return;
		}
		if (free_entry == NULL && !RelFileNumberIsValid(e->log.relNumber))
			free_entry = e;
	}
	if (free_entry != NULL)
		write_entry(free_entry, log, first, end);
	else
		pg_atomic_write_u32(&t->full, 1);

	/* (before any page of it is made: see chunk_log.c's callers) */
	pg_memory_barrier();
}

/* an entry's whole value, as written last */
static void
read_entry(RegionEntry *e, RelFileLocator *log, BlockNumber *first, BlockNumber *end)
{
	for (;;)
	{
		uint32		s1 = pg_atomic_read_u32(&e->seq);

		if (s1 & 1)
			continue;
		pg_read_barrier();
		*log = e->log;
		*first = e->first;
		*end = e->end;
		pg_read_barrier();
		if (pg_atomic_read_u32(&e->seq) == s1)
			return;
	}
}

ChunkRegionSet *
ChunkRegionLive(RelFileLocator log)
{
	ChunkRegionSet *set = palloc0(sizeof(ChunkRegionSet));
	int			cap = 16;

	if (nregion_tables == 0)
		nregion_tables = region_table_count();
	set->first = palloc(cap * sizeof(BlockNumber));
	set->end = palloc(cap * sizeof(BlockNumber));
	pg_memory_barrier();
	for (int i = 0; i < nregion_tables; i++)
	{
		RegionTable *t = &region_tables[i];

		if (pg_atomic_read_u32(&t->full))
			set->all = true;
		for (int j = 0; j < REGIONS_PER_BACKEND; j++)
		{
			RelFileLocator l;
			BlockNumber first,
						end;

			read_entry(&t->entries[j], &l, &first, &end);
			if (!RelFileLocatorEquals(l, log))
				continue;
			if (set->n == cap)
			{
				cap *= 2;
				set->first = repalloc(set->first, cap * sizeof(BlockNumber));
				set->end = repalloc(set->end, cap * sizeof(BlockNumber));
			}
			set->first[set->n] = first;
			set->end[set->n] = end;
			set->n++;
		}
	}
	return set;
}

static bool
set_has(const ChunkRegionSet *set, BlockNumber blk)
{
	if (set->all)
		return true;
	for (int k = 0; k < set->n; k++)
		if (blk >= set->first[k] && blk < set->end[k])
			return true;
	return false;
}

bool
ChunkRegionHeld(RelFileLocator log, BlockNumber blk, const ChunkRegionSet *also)
{
	ChunkRegionSet *now;
	bool		held;

	if (also != NULL && set_has(also, blk))
		return true;
	now = ChunkRegionLive(log);
	held = set_has(now, blk);
	pfree(now->first);
	pfree(now->end);
	pfree(now);
	return held;
}
