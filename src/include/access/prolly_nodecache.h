/*-------------------------------------------------------------------------
 *
 * prolly_nodecache.h
 *	  Node cache interface: chunks a chunk store read from its files,
 *	  decoded and checked, kept by hash for every backend's later reads.
 *
 * A chunk store's read of a chunk from its files costs a read of the file,
 * a CRC check and a decompression; a tree's walks read the same nodes again
 * and again (its upper levels on every walk, and the nodes two diffs or
 * moves of the same trees share), from every backend.  The cache keeps the
 * decoded bytes so a read that finds them skips that work.
 *
 * A chunk is named by its content, so the bytes kept under a hash are the
 * chunk's whatever happens after: nothing is ever stale.  What the cache
 * can't say on its own is whether the store still has the chunk, which a
 * collection may have removed.  So each entry is of a scope: a store
 * (database and tablespace) as of one of its collections' generations,
 * and a lookup finds only what was kept in the scope it asks of.  A
 * collection that removes chunks moves the store to a new generation, so
 * nothing kept before it is found by a reader that has seen it; a reader
 * that hasn't is one that still reads the files the collection retired.
 * A chunk given the same content again after its collection has the same
 * hash and bytes, kept anew in the new generation's scope.
 *
 * Contract:
 * - lookup() appends the bytes kept for hash in scope to out: false if
 *   none are (a miss).
 * - insert() keeps a chunk's bytes for hash in scope: bytes the store has
 *   in its files as of that scope's generation (read from them, or
 *   appended by a transaction that has committed), checked as the store
 *   checks what it reads.  It never raises an error and may keep nothing
 *   (the cache full of what is in use, a chunk too large, the cache off).
 * - forget_database() lets go of everything kept for a database, which is
 *   being dropped: its OID may be given to a new database, whose store has
 *   none of these chunks.
 * - capacity() is the most the cache holds, in bytes (0: off).
 * - A cache may let go of anything at any time: a miss is only slower.
 *
 * src/include/access/prolly_nodecache.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_NODECACHE_H
#define PROLLY_NODECACHE_H

#include "access/prolly_registry.h"
#include "access/prolly_types.h"
#include "lib/stringinfo.h"

/* A store as of one of its collections' generations. */
typedef struct ProllyNodeCacheScope
{
	Oid			dboid;
	Oid			spcoid;
	uint32		generation;
} ProllyNodeCacheScope;

typedef struct ProllyNodeCacheRoutine
{
	ProllyImplInfo info;

	/* Shared memory: its size (0 for none), and its setup, once. */
	Size		(*shmem_size) (void);
	void		(*shmem_init) (void *area);

	bool		(*lookup) (const ProllyNodeCacheScope *scope,
						   const ProllyHash *hash, StringInfo out);
	void		(*insert) (const ProllyNodeCacheScope *scope,
						   const ProllyHash *hash, const char *data,
						   Size len);
	void		(*forget_database) (Oid dboid);
	Size		(*capacity) (void);
} ProllyNodeCacheRoutine;

#define PROLLY_NODECACHE_CALLBACK_NAMES \
	"shmem_size", "shmem_init", "lookup", "insert", "forget_database", \
	"capacity"
#define PROLLY_NODECACHE_FIRST_CALLBACK shmem_size
#define PROLLY_NODECACHE_LAST_CALLBACK capacity

static inline const ProllyNodeCacheRoutine *
prolly_nodecache_lookup(const char *name)
{
	return (const ProllyNodeCacheRoutine *)
		prolly_get_impl(PROLLY_IFACE_NODE_CACHE, name);
}

/* implementation "shared": its size, in kB (a server setting) */
extern PGDLLIMPORT int prolly_node_cache_size;

#endif							/* PROLLY_NODECACHE_H */
