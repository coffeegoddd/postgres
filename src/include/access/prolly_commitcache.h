/*-------------------------------------------------------------------------
 *
 * prolly_commitcache.h
 *	  Commit cache interface: a commit's links (its root, parents and
 *	  height), found by its id without reading the commit.
 *
 * Walking history (HEAD~n, a merge base, a log) reads one commit per step,
 * and a commit is a chunk somewhere in the store's files.  The chunks a
 * backend keeps (the chunk store's cache) are let go of as soon as a large
 * diff or reset reads past them, and the page cache may have let go of the
 * files too, so each step can be a read from disk.  A commit is named by
 * its content, so what it links to never changes: a cache of the links
 * never needs invalidating, only forgetting.
 *
 * A commit can stop being in the store: a collection drops what nothing
 * names any more.  So every entry is of a chunk store's GC generation (the
 * chunk store's generation()), and is found only while the store is still
 * in that generation.  A dropped database's store goes whole, and a new
 * database given its OID starts its generations again, so dropping a
 * database forgets its entries.
 *
 * Contract:
 * - lookup() gives the links remember() was given for commit id of the
 *   store of database dboid, if remembered in GC generation generation,
 *   or nothing.  A hit says the commit is in the store: it was read there
 *   and no collection has run since.
 * - remember() notes the links of a commit the current transaction read
 *   from the store.  The transaction sees them at once; others only once
 *   it commits (a commit it wrote is no one else's until then).  An abort
 *   (of the transaction, or of the subtransaction that remembered it), or
 *   PREPARE TRANSACTION, forgets it: links to a commit that may not be
 *   stored are never given out.
 * - forget_database() forgets every entry of database dboid, everyone's
 *   (its store is being dropped).
 * - A cache may forget anything at any time: a miss is only slower.
 *   pg_stat_prolly counts lookups that hit and missed (commit_cache_hits,
 *   commit_cache_misses); a cache that holds nothing counts none.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_commitcache.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_COMMITCACHE_H
#define PROLLY_COMMITCACHE_H

#include "access/prolly_refs.h"
#include "access/prolly_registry.h"
#include "access/prolly_types.h"

/* What a commit links to: enough to walk history and find its tables. */
typedef struct ProllyCommitLinks
{
	ProllyHash	root;
	int			nparents;
	ProllyHash	parents[PROLLY_MAX_PARENTS];
	int64		height;			/* 0 if the commit records none */
} ProllyCommitLinks;

typedef struct ProllyCommitCacheRoutine
{
	ProllyImplInfo info;

	/* Shared memory: its size (0 for none), and its setup, once. */
	Size		(*shmem_size) (void);
	void		(*shmem_init) (void *area);

	bool		(*lookup) (Oid dboid, uint32 generation, const ProllyHash *id,
						   ProllyCommitLinks *links);
	void		(*remember) (Oid dboid, uint32 generation, const ProllyHash *id,
							 const ProllyCommitLinks *links);
	void		(*forget_database) (Oid dboid);
} ProllyCommitCacheRoutine;

#define PROLLY_COMMITCACHE_CALLBACK_NAMES \
	"shmem_size", "shmem_init", "lookup", "remember", "forget_database"
#define PROLLY_COMMITCACHE_FIRST_CALLBACK shmem_size
#define PROLLY_COMMITCACHE_LAST_CALLBACK forget_database

static inline const ProllyCommitCacheRoutine *
prolly_commitcache_lookup(const char *name)
{
	return (const ProllyCommitCacheRoutine *)
		prolly_get_impl(PROLLY_IFACE_COMMIT_CACHE, name);
}

/* prolly_commit_cache_entries: how many commits a shared cache holds */
extern PGDLLIMPORT int prolly_commit_cache_entries;

#endif							/* PROLLY_COMMITCACHE_H */
