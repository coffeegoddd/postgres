/*-------------------------------------------------------------------------
 *
 * prolly_shmem.h
 *	  Shared memory and checkpoints for prolly implementations.
 *
 * Implementations of the interfaces that have shared state (the chunk
 * store, the value memo, the refs, the materialization and node caches)
 * declare its size with shmem_size() and set it up in shmem_init(area).
 * The prolly shared-memory subsystem reserves one area per implementation
 * at postmaster startup, so implementations added by
 * shared_preload_libraries get theirs too.
 *
 * CheckPointProlly() runs inside every checkpoint and restartpoint, and
 * makes the chunk stores' appends durable; ProllyRecoveryConsistent() runs
 * as recovery reaches a consistent state; ProllyCrashRecovered() runs once
 * crash recovery is over, before connections are let in.
 *
 * src/include/access/prolly_shmem.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_SHMEM_H
#define PROLLY_SHMEM_H

#include "access/prolly_impl.h"

/* The shared area reserved for impl, or NULL if it asked for none. */
extern void *prolly_shmem_area(const ProllyImplInfo *impl);

extern void CheckPointProlly(int flags);
extern void ProllyRecoveryConsistent(void);
extern void ProllyCrashRecovered(void);

#endif							/* PROLLY_SHMEM_H */
