/*-------------------------------------------------------------------------
 *
 * prolly_shmem.c
 *	  The prolly shared-memory subsystem and checkpoint hook.
 *
 * At postmaster startup this reserves a shared area for every registered
 * chunk store, value memo and refs implementation whose shmem_size() is
 * non-zero, named after the implementation, and calls its shmem_init() with the area once.
 * Every process finds the area again with prolly_shmem_area().
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_shmem.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_chunkstore.h"
#include "access/prolly_memo.h"
#include "access/prolly_refs.h"
#include "access/prolly_registry.h"
#include "access/prolly_shmem.h"
#include "storage/shmem.h"
#include "storage/subsystems.h"

/* Interfaces whose implementations may have shared state. */
static const ProllyInterface shmem_interfaces[] = {
	PROLLY_IFACE_CHUNK_STORE,
	PROLLY_IFACE_VALUE_MEMO,
	PROLLY_IFACE_REFS,
};

#define MAX_AREAS 64
#define AREA_NAME_LEN 64

typedef struct ProllyShmemArea
{
	const ProllyImplInfo *impl;
	char		name[AREA_NAME_LEN];
	void	   *area;
} ProllyShmemArea;

static ProllyShmemArea areas[MAX_AREAS];
static int	nareas = 0;

static Size
impl_shmem_size(const ProllyImplInfo *impl)
{
	switch (impl->iface)
	{
		case PROLLY_IFACE_CHUNK_STORE:
			{
				const ProllyChunkStoreRoutine *r = (const ProllyChunkStoreRoutine *) impl;

				return r->shmem_size ? r->shmem_size() : 0;
			}
		case PROLLY_IFACE_VALUE_MEMO:
			{
				const ProllyValueMemoRoutine *r = (const ProllyValueMemoRoutine *) impl;

				return r->shmem_size ? r->shmem_size() : 0;
			}
		case PROLLY_IFACE_REFS:
			{
				const ProllyRefsRoutine *r = (const ProllyRefsRoutine *) impl;

				return r->shmem_size ? r->shmem_size() : 0;
			}
		default:
			return 0;
	}
}

static void
impl_shmem_init(const ProllyImplInfo *impl, void *area)
{
	switch (impl->iface)
	{
		case PROLLY_IFACE_CHUNK_STORE:
			PROLLY_CALL(((const ProllyChunkStoreRoutine *) impl), shmem_init, area);
			break;
		case PROLLY_IFACE_VALUE_MEMO:
			PROLLY_CALL(((const ProllyValueMemoRoutine *) impl), shmem_init, area);
			break;
		case PROLLY_IFACE_REFS:
			PROLLY_CALL(((const ProllyRefsRoutine *) impl), shmem_init, area);
			break;
		default:
			elog(ERROR, "prolly %s implementations have no shared memory",
				 prolly_interface_name(impl->iface));
	}
}

static void
ProllyShmemRequest(void *arg)
{
	nareas = 0;
	for (int i = 0; i < lengthof(shmem_interfaces); i++)
	{
		List	   *impls = prolly_list_impls(shmem_interfaces[i]);
		ListCell   *lc;

		foreach(lc, impls)
		{
			const ProllyImplInfo *impl = lfirst(lc);
			Size		size = impl_shmem_size(impl);
			ProllyShmemArea *a;

			if (size == 0)
				continue;
			if (nareas >= MAX_AREAS)
				elog(FATAL, "too many prolly implementations with shared memory");
			a = &areas[nareas++];
			a->impl = impl;
			a->area = NULL;
			snprintf(a->name, sizeof(a->name), "prolly %s %s",
					 prolly_interface_name(impl->iface), impl->name);
			ShmemRequestStruct(.name = a->name,
							   .size = size,
							   .ptr = &a->area,
				);
		}
		list_free(impls);
	}
}

static void
ProllyShmemInit(void *arg)
{
	for (int i = 0; i < nareas; i++)
		impl_shmem_init(areas[i].impl, areas[i].area);
}

const ShmemCallbacks ProllyShmemCallbacks = {
	.request_fn = ProllyShmemRequest,
	.init_fn = ProllyShmemInit,
};

void *
prolly_shmem_area(const ProllyImplInfo *impl)
{
	for (int i = 0; i < nareas; i++)
	{
		if (areas[i].impl == impl)
			return areas[i].area;
	}
	return NULL;
}

/*
 * Called by CheckPointGuts(), after the redo point is fixed: the chunk
 * stores make their appends before it durable.
 */
void
CheckPointProlly(int flags)
{
	prolly_chunkstore_checkpoint();
}

/*
 * Called as archive recovery reaches a consistent state (beside
 * XLogCheckInvalidPages()), and as crash recovery ends: the chunk stores
 * check that redo left nothing unresolved.
 */
void
ProllyRecoveryConsistent(void)
{
	prolly_chunkstore_consistent();
}

/*
 * Called by StartupXLOG() once crash recovery is over, before connections
 * are let in: the chunk stores remove what a crash cut short left.
 */
void
ProllyCrashRecovered(void)
{
	prolly_chunkstore_consistent();
	prolly_chunkstore_recovered();
}
