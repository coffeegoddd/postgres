/*-------------------------------------------------------------------------
 *
 * chunk_pin.c
 *	  Transient holds on the history store's chunks (prolly/chunk_pin.h).
 *
 * Each backend has a table of PINS_PER_BACKEND slots, open addressing, of
 * the first 8 bytes of the hashes it pins (with the low bit set, so that no
 * key is zero).  Its backend alone writes it; a freer reads every table.
 * A write is a slot's key, then a barrier, before the backend looks the
 * chunk up, as a freer's read of the tables follows its marking the chunk
 * being freed; both go through the chunk's bucket's lock, which orders the
 * two.  A table three quarters full is full: its backend counts as pinning
 * everything.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/chunk_pin.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/twophase.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "prolly/chunk_index.h"
#include "prolly/chunk_pin.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "storage/subsystems.h"

#define PINS_PER_BACKEND	2048	/* a power of two */
#define PINS_FULL			(PINS_PER_BACKEND / 4 * 3)

typedef struct PinTable
{
	pg_atomic_uint32 full;		/* counts as pinning everything */
	uint32		used;			/* (its backend's alone) */
	pg_atomic_uint64 keys[PINS_PER_BACKEND];
} PinTable;

static PinTable *pin_tables;
static int	npin_tables;

/* the slots this backend has set, to clear at its transaction's end */
static uint16 my_slots[PINS_FULL];
static bool callback_registered = false;

static void ChunkPinShmemRequest(void *arg);
static void ChunkPinShmemInit(void *arg);

const ShmemCallbacks ChunkPinShmemCallbacks = {
	.request_fn = ChunkPinShmemRequest,
	.init_fn = ChunkPinShmemInit,
};

static int
pin_table_count(void)
{
	/* (every process that may run a transaction has a number below it) */
	return MaxBackends + NUM_AUXILIARY_PROCS + max_prepared_xacts;
}

static void
ChunkPinShmemRequest(void *arg)
{
	npin_tables = pin_table_count();
	ShmemRequestStruct(.name = "Chunk Pins",
					   .size = mul_size(npin_tables, sizeof(PinTable)),
					   .ptr = (void **) &pin_tables,
		);
}

static void
ChunkPinShmemInit(void *arg)
{
	for (int i = 0; i < npin_tables; i++)
	{
		pg_atomic_init_u32(&pin_tables[i].full, 0);
		pin_tables[i].used = 0;
		for (int j = 0; j < PINS_PER_BACKEND; j++)
			pg_atomic_init_u64(&pin_tables[i].keys[j], 0);
	}
}

static inline uint64
pin_key(const uint8 *hash)
{
	uint64		key;

	memcpy(&key, hash, sizeof(key));
	return key | 1;
}

static PinTable *
my_table(void)
{
	if (npin_tables == 0)
		npin_tables = pin_table_count();
	Assert(MyProcNumber >= 0 && MyProcNumber < npin_tables);
	return &pin_tables[MyProcNumber];
}

static void
release_pins(XactEvent event, void *arg)
{
	PinTable   *t;

	if (event != XACT_EVENT_COMMIT && event != XACT_EVENT_ABORT &&
		event != XACT_EVENT_PREPARE && event != XACT_EVENT_PARALLEL_COMMIT &&
		event != XACT_EVENT_PARALLEL_ABORT)
		return;
	t = my_table();
	for (uint32 i = 0; i < t->used; i++)
		pg_atomic_write_u64(&t->keys[my_slots[i]], 0);
	t->used = 0;
	pg_write_barrier();
	pg_atomic_write_u32(&t->full, 0);
}

void
ChunkPin(const uint8 *hash)
{
	PinTable   *t = my_table();
	uint64		key = pin_key(hash);
	uint32		slot = (uint32) (key >> 32) & (PINS_PER_BACKEND - 1);

	if (!callback_registered)
	{
		RegisterXactCallback(release_pins, NULL);
		callback_registered = true;
	}
	if (pg_atomic_read_u32(&t->full))
		return;
	if (t->used >= PINS_FULL)
	{
		pg_atomic_write_u32(&t->full, 1);
		pg_memory_barrier();
		return;
	}
	for (;;)
	{
		uint64		k = pg_atomic_read_u64(&t->keys[slot]);

		if (k == key)
			break;
		if (k == 0)
		{
			pg_atomic_write_u64(&t->keys[slot], key);
			my_slots[t->used++] = (uint16) slot;
			break;
		}
		slot = (slot + 1) & (PINS_PER_BACKEND - 1);
	}

	/* (before the chunk is looked up: see the file's comment) */
	pg_memory_barrier();
}

bool
ChunkPinned(const uint8 *hash)
{
	uint64		key = pin_key(hash);

	if (npin_tables == 0)
		npin_tables = pin_table_count();
	pg_memory_barrier();
	for (int i = 0; i < npin_tables; i++)
	{
		PinTable   *t = &pin_tables[i];
		uint32		slot = (uint32) (key >> 32) & (PINS_PER_BACKEND - 1);

		if (pg_atomic_read_u32(&t->full))
			return true;
		for (;;)
		{
			uint64		k = pg_atomic_read_u64(&t->keys[slot]);

			if (k == key)
				return true;
			if (k == 0)
				break;
			slot = (slot + 1) & (PINS_PER_BACKEND - 1);
		}
	}
	return false;
}
