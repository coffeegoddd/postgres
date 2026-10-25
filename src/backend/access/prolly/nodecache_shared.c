/*-------------------------------------------------------------------------
 *
 * nodecache_shared.c
 *	  Node cache implementation "shared": decoded chunks in a fixed area of
 *	  shared memory, for every backend, evicted by a clock.
 *
 * The area is prolly_node_cache_size kB (a server setting; 0, none), cut
 * into blocks of NODE_BLOCK bytes, which hold the chunks, and entries,
 * which name them: an entry is a scope and a hash (prolly_nodecache.h),
 * the chunk's length, and its first block, the rest chained from it.  So
 * chunks of any size up to a sixteenth of the cache (and NODE_MAX_ENTRY)
 * share the area without fragmenting it.  There are half as many entries
 * as blocks: a tree node is about four blocks, so entries run out only for
 * many chunks of under two.
 *
 * Entries are found from a table of buckets, each a chain of entries, the
 * buckets split among NODE_PARTITIONS locks: a lookup holds its bucket's
 * lock shared while it copies the chunk out, so readers of different
 * chunks, and of one, don't wait for each other; a change to a chain holds
 * it exclusive.  The free entries and blocks are lists under a spinlock.
 *
 * A chunk offered from a read of the files is kept only if it was offered
 * lately before: a table as long as the buckets' holds, for each slot,
 * the fingerprint of the last chunk offered there and not kept, so a
 * chunk read once (a scan's leaves, a fold's) costs the cache a word, not
 * an entry and a copy, and evicts nothing; the second read of a chunk
 * within about as many reads as the cache holds entries keeps it.  A
 * transaction's own chunks, offered as it commits, are kept at once.
 *
 * An insert takes an entry and the blocks it needs from the free lists,
 * fills them without any lock (they are no one else's yet), and links the
 * entry into its bucket.  When the free lists are short, the clock frees
 * some first: its hand moves over the entries, and an entry in use that
 * has been read since the hand last passed it (its usage count above
 * zero) is passed over with its count lowered, one that hasn't is
 * unlinked and its blocks freed.  A lookup that finds an entry raises its
 * count, up to NODE_MAX_USAGE, so chunks read again and again (a tree's
 * upper levels, the nodes repeated diffs and moves share) stay while
 * chunks read once are what the hand frees.  An insert that can't free
 * enough in a bounded sweep keeps nothing: inserts never wait for one
 * another and never fail.  The hand takes a lock only conditionally, so
 * it never waits on readers either.
 *
 * Nothing outlives a restart.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/nodecache_shared.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"
#include "access/prolly_hash.h"
#include "access/prolly_shmem.h"
#include "common/hashfn.h"
#include "port/atomics.h"
#include "port/pg_bitutils.h"
#include "storage/lwlock.h"
#include "storage/spin.h"

int			prolly_node_cache_size = 131072;

#define NODE_BLOCK			1024
#define NODE_PARTITIONS		128
#define NODE_MAX_USAGE		5
#define NODE_MAX_ENTRY		(256 * 1024)
/* (entries the hand looks at, at most, to free room for one insert) */
#define NODE_SWEEP(c)		(2 * (NODE_MAX_USAGE + 1) * (c)->nentries)

#define NONE				(-1)

typedef struct NodeEntry
{
	ProllyNodeCacheScope scope;
	ProllyHash	hash;
	uint32		len;
	int32		first;			/* its first block */
	int32		next;			/* the next in its bucket, or free */
	uint32		bucket;
	bool		used;			/* linked into its bucket */
	pg_atomic_uint32 usage;
} NodeEntry;

typedef struct NodeCache
{
	LWLockPadded parts[NODE_PARTITIONS];
	slock_t		mutex;			/* the free lists' */
	int32		free_entry;
	int32		free_block;
	int32		nfree_blocks;
	pg_atomic_uint32 hand;
	int32		nentries;
	int32		nblocks;
	uint32		nbuckets;		/* a power of two */
	/* then buckets, the offers' fingerprints, entries, the blocks' chain
	 * and the blocks */
} NodeCache;

static NodeCache *cache = NULL;
static int32 *buckets;
static pg_atomic_uint32 *offered;	/* nbuckets of them */
static NodeEntry *entries;
static int32 *block_next;
static char *blocks;

/* What prolly_node_cache_size gives: entries, blocks and buckets. */
static void
dimensions(int32 *nentries, int32 *nblocks, uint32 *nbuckets)
{
	Size		bytes = (Size) prolly_node_cache_size * 1024;
	Size		per_block = NODE_BLOCK + sizeof(int32) +
		(sizeof(NodeEntry) + 2 * sizeof(int32)) / 2;
	Size		n = bytes / per_block;

	n = Min(n, (Size) PG_INT32_MAX / 2);
	if (n < 16)
		n = 0;
	*nblocks = (int32) n;
	*nentries = (int32) (n / 2);
	*nbuckets = *nentries > 0 ? pg_nextpower2_32((uint32) *nentries) : 0;
}

static Size
shared_shmem_size(void)
{
	int32		nentries;
	int32		nblocks;
	uint32		nbuckets;
	Size		size;

	dimensions(&nentries, &nblocks, &nbuckets);
	if (nblocks == 0)
		return 0;
	size = MAXALIGN(sizeof(NodeCache));
	size = add_size(size, MAXALIGN(mul_size(nbuckets, sizeof(int32))));
	size = add_size(size, MAXALIGN(mul_size(nbuckets, sizeof(pg_atomic_uint32))));
	size = add_size(size, MAXALIGN(mul_size(nentries, sizeof(NodeEntry))));
	size = add_size(size, MAXALIGN(mul_size(nblocks, sizeof(int32))));
	size = add_size(size, mul_size(nblocks, NODE_BLOCK));
	return size;
}

static void
attach(NodeCache *c)
{
	char	   *p = (char *) c + MAXALIGN(sizeof(NodeCache));

	buckets = (int32 *) p;
	p += MAXALIGN(c->nbuckets * sizeof(int32));
	offered = (pg_atomic_uint32 *) p;
	p += MAXALIGN(c->nbuckets * sizeof(pg_atomic_uint32));
	entries = (NodeEntry *) p;
	p += MAXALIGN(c->nentries * sizeof(NodeEntry));
	block_next = (int32 *) p;
	p += MAXALIGN(c->nblocks * sizeof(int32));
	blocks = p;
	cache = c;
}

static void
shared_shmem_init(void *area)
{
	NodeCache  *c = area;
	int			tranche = LWLockNewTrancheId("prolly node cache");

	memset(c, 0, sizeof(NodeCache));
	for (int i = 0; i < NODE_PARTITIONS; i++)
		LWLockInitialize(&c->parts[i].lock, tranche);
	SpinLockInit(&c->mutex);
	dimensions(&c->nentries, &c->nblocks, &c->nbuckets);
	pg_atomic_init_u32(&c->hand, 0);
	attach(c);
	for (uint32 b = 0; b < c->nbuckets; b++)
	{
		buckets[b] = NONE;
		pg_atomic_init_u32(&offered[b], 0);
	}
	for (int32 i = 0; i < c->nentries; i++)
	{
		memset(&entries[i], 0, sizeof(NodeEntry));
		entries[i].next = i + 1 < c->nentries ? i + 1 : NONE;
		pg_atomic_init_u32(&entries[i].usage, 0);
	}
	for (int32 i = 0; i < c->nblocks; i++)
		block_next[i] = i + 1 < c->nblocks ? i + 1 : NONE;
	c->free_entry = c->nentries > 0 ? 0 : NONE;
	c->free_block = c->nblocks > 0 ? 0 : NONE;
	c->nfree_blocks = c->nblocks;
}

/* The cache, or NULL if it is off. */
static NodeCache *
shared(void)
{
	if (cache == NULL)
	{
		NodeCache  *c = prolly_shmem_area(&prolly_nodecache_shared.info);

		if (c == NULL || c->nentries == 0)
			return NULL;
		attach(c);
	}
	return cache;
}

static uint32
key_hash(const ProllyNodeCacheScope *scope, const ProllyHash *hash)
{
	uint32		h = hash->len >= 4 ? prolly_hash_bucket(hash->bytes) :
		hash_bytes(hash->bytes, hash->len);

	h = hash_combine(h, scope->dboid);
	h = hash_combine(h, scope->spcoid);
	return hash_combine(h, scope->generation);
}

static uint32
bucket_of(const NodeCache *c, const ProllyNodeCacheScope *scope,
		  const ProllyHash *hash)
{
	return key_hash(scope, hash) & (c->nbuckets - 1);
}

/*
 * Was the chunk offered lately, and not kept?  If not, it is noted as
 * offered, in place of whichever chunk its slot noted.
 */
static bool
offered_before(NodeCache *c, const ProllyNodeCacheScope *scope,
			   const ProllyHash *hash)
{
	uint32		h = key_hash(scope, hash);
	pg_atomic_uint32 *slot = &offered[pg_rotate_right32(h, 16) &
									  (c->nbuckets - 1)];
	uint32		print = h | 1;	/* (never 0, a slot's first value) */

	if (pg_atomic_read_u32(slot) == print)
		return true;
	pg_atomic_write_u32(slot, print);
	return false;
}

static LWLock *
part_lock(NodeCache *c, uint32 bucket)
{
	return &c->parts[bucket % NODE_PARTITIONS].lock;
}

static bool
matches(const NodeEntry *e, const ProllyNodeCacheScope *scope,
		const ProllyHash *hash)
{
	return e->scope.dboid == scope->dboid &&
		e->scope.spcoid == scope->spcoid &&
		e->scope.generation == scope->generation &&
		e->hash.len == hash->len &&
		memcmp(e->hash.bytes, hash->bytes, hash->len) == 0;
}

/* The entry for scope and hash in bucket, its lock held; or NONE. */
static int32
find(const ProllyNodeCacheScope *scope, const ProllyHash *hash,
	 uint32 bucket)
{
	for (int32 i = buckets[bucket]; i != NONE; i = entries[i].next)
		if (matches(&entries[i], scope, hash))
			return i;
	return NONE;
}

static void
raise_usage(NodeEntry *e)
{
	uint32		u = pg_atomic_read_u32(&e->usage);

	if (u < NODE_MAX_USAGE)
		pg_atomic_write_u32(&e->usage, u + 1);
}

static bool
shared_lookup(const ProllyNodeCacheScope *scope, const ProllyHash *hash,
			  StringInfo out)
{
	NodeCache  *c = shared();
	uint32		bucket;
	LWLock	   *lock;
	int32		i;

	if (c == NULL)
		return false;
	bucket = bucket_of(c, scope, hash);
	lock = part_lock(c, bucket);
	LWLockAcquire(lock, LW_SHARED);
	i = find(scope, hash, bucket);
	if (i != NONE)
	{
		NodeEntry  *e = &entries[i];
		uint32		left = e->len;

		/* (the blocks are the entry's while its bucket's lock is held) */
		enlargeStringInfo(out, (int) left);
		for (int32 b = e->first; left > 0; b = block_next[b])
		{
			uint32		n = Min(left, NODE_BLOCK);

			appendBinaryStringInfo(out, blocks + (Size) b * NODE_BLOCK, n);
			left -= n;
		}
		raise_usage(e);
	}
	LWLockRelease(lock);
	return i != NONE;
}

/* Give back an entry and its chain of blocks, nblocks long. */
static void
give_back(NodeCache *c, int32 i, int32 first, int32 nblocks)
{
	int32		last = first;

	for (int32 k = 1; k < nblocks; k++)
		last = block_next[last];
	SpinLockAcquire(&c->mutex);
	if (nblocks > 0)
	{
		block_next[last] = c->free_block;
		c->free_block = first;
		c->nfree_blocks += nblocks;
	}
	entries[i].next = c->free_entry;
	c->free_entry = i;
	SpinLockRelease(&c->mutex);
}

static int32
blocks_for(uint32 len)
{
	return (int32) Max((len + NODE_BLOCK - 1) / NODE_BLOCK, 1);
}

/* Unlink entry i, in use in bucket, its lock held exclusive. */
static void
unlink_entry(NodeEntry *e, int32 i)
{
	int32	   *link = &buckets[e->bucket];

	while (*link != i)
		link = &entries[*link].next;
	*link = e->next;
	e->used = false;
}

/*
 * One step of the clock: the entry under the hand lets go of its blocks if
 * it hasn't been read since the hand last passed it, or has its count
 * lowered.
 */
static void
clock_step(NodeCache *c)
{
	int32		i = (int32) (pg_atomic_fetch_add_u32(&c->hand, 1) %
							 (uint32) c->nentries);
	NodeEntry  *e = &entries[i];
	uint32		bucket = e->bucket;
	LWLock	   *lock;
	uint32		u;
	int32		first;
	int32		n;

	if (!e->used)
		return;
	lock = part_lock(c, bucket);
	if (!LWLockConditionalAcquire(lock, LW_EXCLUSIVE))
		return;
	/* (looked at again under the lock: it may have changed hands) */
	if (!e->used || e->bucket % NODE_PARTITIONS != bucket % NODE_PARTITIONS)
	{
		LWLockRelease(lock);
		return;
	}
	u = pg_atomic_read_u32(&e->usage);
	if (u > 0)
	{
		pg_atomic_write_u32(&e->usage, u - 1);
		LWLockRelease(lock);
		return;
	}
	unlink_entry(e, i);
	first = e->first;
	n = blocks_for(e->len);
	LWLockRelease(lock);
	give_back(c, i, first, n);
}

/*
 * An entry and nblocks blocks off the free lists, the clock freeing them
 * where there are too few: false, taking none, if a bounded sweep didn't.
 */
static bool
take_free(NodeCache *c, int32 nblocks, int32 *entry, int32 *first)
{
	for (int64 steps = 0; steps <= NODE_SWEEP(c); steps++)
	{
		SpinLockAcquire(&c->mutex);
		if (c->free_entry != NONE && c->nfree_blocks >= nblocks)
		{
			int32		b = c->free_block;

			*entry = c->free_entry;
			c->free_entry = entries[*entry].next;
			*first = b;
			for (int32 k = 1; k < nblocks; k++)
				b = block_next[b];
			c->free_block = block_next[b];
			block_next[b] = NONE;
			c->nfree_blocks -= nblocks;
			SpinLockRelease(&c->mutex);
			return true;
		}
		SpinLockRelease(&c->mutex);
		clock_step(c);
	}
	return false;
}

static void
shared_insert(const ProllyNodeCacheScope *scope, const ProllyHash *hash,
			  const char *data, Size len, bool at_once)
{
	NodeCache  *c = shared();
	uint32		bucket;
	LWLock	   *lock;
	int32		i;
	int32		first;
	int32		nblocks;
	NodeEntry  *e;
	Size		left;
	const char *p;

	if (c == NULL || len > NODE_MAX_ENTRY ||
		len > (Size) c->nblocks * NODE_BLOCK / 16)
		return;
	bucket = bucket_of(c, scope, hash);
	lock = part_lock(c, bucket);

	/* (kept already: it is read again) */
	LWLockAcquire(lock, LW_SHARED);
	i = find(scope, hash, bucket);
	if (i != NONE)
		raise_usage(&entries[i]);
	LWLockRelease(lock);
	if (i != NONE || (!at_once && !offered_before(c, scope, hash)))
		return;

	nblocks = blocks_for((uint32) len);
	if (!take_free(c, nblocks, &i, &first))
		return;
	e = &entries[i];
	e->scope = *scope;
	memset(&e->hash, 0, sizeof(e->hash));
	e->hash.len = hash->len;
	memcpy(e->hash.bytes, hash->bytes, hash->len);
	e->len = (uint32) len;
	e->first = first;
	e->bucket = bucket;
	pg_atomic_write_u32(&e->usage, 1);
	p = data;
	left = len;
	for (int32 b = first; left > 0; b = block_next[b])
	{
		Size		n = Min(left, NODE_BLOCK);

		memcpy(blocks + (Size) b * NODE_BLOCK, p, n);
		p += n;
		left -= n;
	}

	LWLockAcquire(lock, LW_EXCLUSIVE);
	/* (another backend may have kept it meanwhile) */
	if (find(scope, hash, bucket) != NONE)
	{
		LWLockRelease(lock);
		give_back(c, i, first, nblocks);
		return;
	}
	e->next = buckets[bucket];
	buckets[bucket] = i;
	e->used = true;
	LWLockRelease(lock);
}

static void
shared_forget_database(Oid dboid)
{
	NodeCache  *c = shared();

	if (c == NULL)
		return;
	for (int p = 0; p < NODE_PARTITIONS; p++)
	{
		LWLockAcquire(&c->parts[p].lock, LW_EXCLUSIVE);
		for (uint32 b = p; b < c->nbuckets; b += NODE_PARTITIONS)
		{
			int32		i = buckets[b];

			while (i != NONE)
			{
				NodeEntry  *e = &entries[i];
				int32		next = e->next;

				if (e->scope.dboid == dboid)
				{
					unlink_entry(e, i);
					give_back(c, i, e->first, blocks_for(e->len));
				}
				i = next;
			}
		}
		LWLockRelease(&c->parts[p].lock);
	}
}

static Size
shared_capacity(void)
{
	NodeCache  *c = shared();

	return c == NULL ? 0 : (Size) c->nblocks * NODE_BLOCK;
}

const ProllyNodeCacheRoutine prolly_nodecache_shared = {
	.info = {
		.iface = PROLLY_IFACE_NODE_CACHE,
		.impl_id = 1,
		.format_version = 1,
		.name = "shared",
		.description = "decoded chunks by store, collection generation and "
		"hash, in a fixed area of shared memory evicted by a clock",
	},
	.shmem_size = shared_shmem_size,
	.shmem_init = shared_shmem_init,
	.lookup = shared_lookup,
	.insert = shared_insert,
	.forget_database = shared_forget_database,
	.capacity = shared_capacity,
};
