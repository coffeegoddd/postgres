/*-------------------------------------------------------------------------
 *
 * commitcache_shared.c
 *	  Commit cache implementation "shared": a bounded table in shared
 *	  memory of commits' links, keyed by database and commit id, each of
 *	  the chunk store GC generation it was read in.
 *
 * Every backend walking the same history reads the same commits, so one
 * table serves them all, and it outlives any backend's own chunk cache:
 * a long diff or reset that reads a backend's chunks out of that cache
 * leaves this one as it was.
 *
 * A transaction's entries are kept in its backend until it commits (a
 * commit it wrote is in no one else's store until then), and forgotten if
 * it, or the subtransaction that made them, aborts, or if it is prepared
 * (they wait for no COMMIT PREPARED).  A parallel worker's are forgotten
 * when it ends: its leader may yet abort.  At most as many as the shared
 * table holds, and PENDING_MAX, are kept.
 *
 * The table has prolly_commit_cache_entries slots (a server setting; 0,
 * none, and nothing is remembered) under a spinlock.  A commit is looked
 * for in CACHE_PROBE slots from its id's hash, and a new entry takes the
 * first free or matching one there, else the one used longest ago.  An
 * entry of another GC generation than the store's now is never matched,
 * and is taken first.  Dropping a database walks the whole table (under
 * the spinlock, a batch of slots at a time).  Nothing outlives a restart.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/commitcache_shared.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"
#include "access/prolly_shmem.h"
#include "access/xact.h"
#include "common/hashfn.h"
#include "pgstat.h"
#include "storage/spin.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

#define CACHE_PROBE		8
#define PENDING_MAX		8192	/* a transaction's entries, at most */
#define FORGET_BATCH	256		/* slots a forget looks at under the lock */

/* (zeroed before it's filled: hashed and compared as bytes) */
typedef struct CommitKey
{
	Oid			dboid;
	ProllyHash	id;
} CommitKey;

typedef struct CacheEntry
{
	CommitKey	key;
	uint32		generation;
	uint64		used;			/* the table's clock at the last use; 0 if free */
	ProllyCommitLinks links;
} CacheEntry;

typedef struct CacheShared
{
	slock_t		mutex;
	int			nentries;
	uint64		clock;
	CacheEntry	entries[FLEXIBLE_ARRAY_MEMBER];
} CacheShared;

int			prolly_commit_cache_entries = 4096;

/* An entry of this backend's transaction, not yet everyone's. */
typedef struct Pending
{
	CommitKey	key;			/* the hash key */
	SubTransactionId subxid;	/* the subtransaction that made it */
	uint32		generation;
	ProllyCommitLinks links;
} Pending;

static CacheShared *cache = NULL;
static HTAB *pending = NULL;	/* by key; NULL if none */
static SubTransactionId pending_newest; /* no subxid of theirs is later */
static bool callbacks_registered = false;

static CacheShared *
shared(void)
{
	if (cache == NULL)
		cache = prolly_shmem_area(&prolly_commitcache_shared.info);
	if (cache == NULL)
		elog(ERROR, "prolly commit cache has no shared memory");
	return cache;
}

static Size
shared_shmem_size(void)
{
	if (prolly_commit_cache_entries == 0)
		return 0;
	return add_size(offsetof(CacheShared, entries),
					mul_size(prolly_commit_cache_entries, sizeof(CacheEntry)));
}

static void
shared_shmem_init(void *area)
{
	CacheShared *c = area;

	SpinLockInit(&c->mutex);
	c->nentries = prolly_commit_cache_entries;
	c->clock = 0;
	memset(c->entries, 0, sizeof(CacheEntry) * c->nentries);
}

static void
key_of(Oid dboid, const ProllyHash *id, CommitKey *key)
{
	memset(key, 0, sizeof(*key));
	key->dboid = dboid;
	key->id.len = id->len;
	memcpy(key->id.bytes, id->bytes, id->len);
}

static int
first_slot(const CommitKey *key)
{
	return (int) (hash_bytes((const unsigned char *) key, sizeof(*key)) %
				  prolly_commit_cache_entries);
}

static bool
same_key(const CommitKey *a, const CommitKey *b)
{
	return memcmp(a, b, sizeof(*a)) == 0;
}

static void
count(bool hit)
{
	pgstat_count_prolly(hit ? PGSTAT_PROLLY_COMMIT_CACHE_HITS :
						PGSTAT_PROLLY_COMMIT_CACHE_MISSES, 1);
}

static bool
shared_lookup(Oid dboid, uint32 generation, const ProllyHash *id,
			  ProllyCommitLinks *links)
{
	CommitKey	key;
	bool		found = false;

	/* (no cache: no lookup, and nothing counted) */
	if (prolly_commit_cache_entries == 0)
		return false;
	if (id->len == 0)
	{
		count(false);
		return false;
	}
	key_of(dboid, id, &key);

	/* This transaction's own, then everyone's. */
	if (pending != NULL)
	{
		Pending    *p = hash_search(pending, &key, HASH_FIND, NULL);

		if (p != NULL && p->generation == generation)
		{
			*links = p->links;
			found = true;
		}
	}
	if (!found)
	{
		CacheShared *c = shared();
		int			slot = first_slot(&key);

		SpinLockAcquire(&c->mutex);
		for (int i = 0; i < CACHE_PROBE; i++)
		{
			CacheEntry *e = &c->entries[(slot + i) % c->nentries];

			if (e->used == 0 || e->generation != generation ||
				!same_key(&e->key, &key))
				continue;
			e->used = ++c->clock;
			*links = e->links;
			found = true;
			break;
		}
		SpinLockRelease(&c->mutex);
	}
	count(found);
	return found;
}

/* Is e of another GC generation than pending entry p? */
static inline bool
stale_entry(const CacheEntry *e, const Pending *p)
{
	return e->generation != p->generation;
}

static void
publish(void)
{
	CacheShared *c = shared();
	HASH_SEQ_STATUS seq;
	Pending    *p;

	hash_seq_init(&seq, pending);
	while ((p = hash_seq_search(&seq)) != NULL)
	{
		int			slot = first_slot(&p->key);
		CacheEntry *take = NULL;

		SpinLockAcquire(&c->mutex);
		for (int i = 0; i < CACHE_PROBE; i++)
		{
			CacheEntry *e = &c->entries[(slot + i) % c->nentries];

			if (e->used == 0 || same_key(&e->key, &p->key))
			{
				take = e;
				break;
			}
			/* (one of another generation, never matched again, goes first) */
			if (take == NULL ||
				(stale_entry(e, p) && !stale_entry(take, p)) ||
				(stale_entry(e, p) == stale_entry(take, p) &&
				 e->used < take->used))
				take = e;
		}
		take->key = p->key;
		take->generation = p->generation;
		take->links = p->links;
		take->used = ++c->clock;
		SpinLockRelease(&c->mutex);
	}
}

static void
forget_all(void)
{
	if (pending != NULL)
		hash_destroy(pending);
	pending = NULL;
}

static void
cache_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_COMMIT:
			if (pending != NULL)
				publish();
			forget_all();
			break;
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_PARALLEL_ABORT:
		case XACT_EVENT_PREPARE:
			/*
			 * (a worker's leader may still abort; a prepared transaction's
			 * chunks wait for COMMIT PREPARED)
			 */
			forget_all();
			break;
		default:
			break;
	}
}

/*
 * A subtransaction's entries are its parent's when it commits, and they
 * keep its ID: every subtransaction begun since an open one began is that
 * one's descendant, so when a subtransaction aborts, the entries to forget
 * are those of its ID or a later one.
 */
static void
cache_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
					   SubTransactionId parentSubid, void *arg)
{
	HASH_SEQ_STATUS seq;
	Pending    *p;

	if (event != SUBXACT_EVENT_ABORT_SUB ||
		pending == NULL || pending_newest < mySubid)
		return;
	hash_seq_init(&seq, pending);
	while ((p = hash_seq_search(&seq)) != NULL)
		if (p->subxid >= mySubid)
			(void) hash_search(pending, &p->key, HASH_REMOVE, NULL);
	pending_newest = mySubid - 1;	/* (at most) */
}

static void
shared_remember(Oid dboid, uint32 generation, const ProllyHash *id,
				const ProllyCommitLinks *links)
{
	CommitKey	key;
	Pending    *p;

	if (prolly_commit_cache_entries == 0 || id->len == 0 ||
		!IsTransactionState())
		return;
	if (!callbacks_registered)
	{
		RegisterXactCallback(cache_xact_callback, NULL);
		RegisterSubXactCallback(cache_subxact_callback, NULL);
		callbacks_registered = true;
	}
	if (pending == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(CommitKey);
		ctl.entrysize = sizeof(Pending);
		ctl.hcxt = TopMemoryContext;
		pending = hash_create("prolly commit cache", 64, &ctl,
							  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		pending_newest = InvalidSubTransactionId;
	}
	key_of(dboid, id, &key);

	/*
	 * No more than the shared table holds (the rest would be dropped), nor
	 * than PENDING_MAX: a transaction's memory, and its work to publish
	 * them as it commits, don't grow with the setting.
	 */
	p = hash_search(pending, &key,
					hash_get_num_entries(pending) <
					Min(prolly_commit_cache_entries, PENDING_MAX) ?
					HASH_ENTER : HASH_FIND, NULL);
	if (p == NULL)
		return;
	p->subxid = GetCurrentSubTransactionId();
	pending_newest = Max(pending_newest, p->subxid);
	p->generation = generation;
	p->links = *links;
}

static void
shared_forget_database(Oid dboid)
{
	CacheShared *c;

	if (prolly_commit_cache_entries == 0)
		return;
	c = shared();

	/*
	 * (FORGET_BATCH slots at a time, so lookups wait at most that long: no
	 * entry of the database is made meanwhile, since nothing is connected
	 * to a database being dropped)
	 */
	for (int start = 0; start < c->nentries; start += FORGET_BATCH)
	{
		int			end = Min(start + FORGET_BATCH, c->nentries);

		SpinLockAcquire(&c->mutex);
		for (int i = start; i < end; i++)
			if (c->entries[i].used != 0 && c->entries[i].key.dboid == dboid)
				c->entries[i].used = 0;
		SpinLockRelease(&c->mutex);
	}
}

const ProllyCommitCacheRoutine prolly_commitcache_shared = {
	.info = {
		.iface = PROLLY_IFACE_COMMIT_CACHE,
		.impl_id = 1,
		.format_version = 1,
		.name = "shared",
		.description = "commits' links, by database and commit id, in a "
		"bounded table in shared memory",
	},
	.shmem_size = shared_shmem_size,
	.shmem_init = shared_shmem_init,
	.lookup = shared_lookup,
	.remember = shared_remember,
	.forget_database = shared_forget_database,
};
