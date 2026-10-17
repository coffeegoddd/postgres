/*-------------------------------------------------------------------------
 *
 * prolly_alloc.h
 *	  Allocator interface: space management inside a relation's storage.
 *
 * Used by in-place engines (D3, D4): fixed-size runs (e.g. 512 bytes) or
 * variable-size allocation.
 *
 * Contract:
 * - allocate() returns an extent of at least size bytes that no other live
 *   allocation overlaps, or raises an error.
 * - free() makes the extent available again; the caller guarantees nothing
 *   can still read it (that is the reclamation interface's decision).
 * - Allocation state is crash-safe through WAL or rebuilt at recovery.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_alloc.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_ALLOC_H
#define PROLLY_ALLOC_H

#include "access/prolly_registry.h"
#include "access/prolly_types.h"
#include "utils/relcache.h"

typedef struct ProllyAllocatorRoutine
{
	ProllyImplInfo info;

	ProllyAllocator *(*open) (Relation rel);
	void		(*close) (ProllyAllocator *alloc);

	void		(*allocate) (ProllyAllocator *alloc, Size size,
							 ProllyExtent *extent);
	void		(*free) (ProllyAllocator *alloc, const ProllyExtent *extent);
} ProllyAllocatorRoutine;

#define PROLLY_ALLOC_CALLBACK_NAMES "open", "close", "allocate", "free"
#define PROLLY_ALLOC_FIRST_CALLBACK open
#define PROLLY_ALLOC_LAST_CALLBACK free

static inline const ProllyAllocatorRoutine *
prolly_alloc_lookup(const char *name)
{
	return (const ProllyAllocatorRoutine *)
		prolly_get_impl(PROLLY_IFACE_ALLOCATOR, name);
}

#endif							/* PROLLY_ALLOC_H */
