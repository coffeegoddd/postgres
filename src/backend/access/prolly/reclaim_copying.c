/*-------------------------------------------------------------------------
 *
 * reclaim_copying.c
 *	  Reclamation implementation "copying": a collector that marks what is
 *	  live and has the chunk store copy it forward.
 *
 * It drives the chunk store's side of a collection (prolly_chunkstore.h).
 * The store seals its journal and numbers the chunks collected; this marks,
 * a bit a chunk, every collected chunk the root as of the seal reaches,
 * breadth first.  Then what is new since the seal is a root too (the root,
 * and the chunks appended meanwhile, which the store keeps whole), marked
 * from twice: before the window, after which the store copies what is
 * marked to new files, and in it, appends held off, for what came since.
 * The store swaps the manifest, and retires what wasn't kept once whoever
 * may read it has ended.
 *
 * What a chunk names is found without knowing what kind of chunk it is:
 * the store's chunks write every reference as the hash's length, then the
 * hash (a tree's children and values stored out of line, a commit's root,
 * parents and dependencies, the refs' heads and baselines, a canonical
 * root's data trees), so wherever a chunk's bytes hold the length followed
 * by a collected chunk's hash, that chunk is marked.  Bytes that happen to
 * spell one can keep a chunk nothing names; nothing named is ever lost.
 *
 * A default collection stops at settled files (the store doesn't collect
 * them, so marking ends there): it costs what changed since the last.  A
 * full one collects every file, marks through all of them, then keeps
 * whole each settled file whose garbage is at most REWRITE_GARBAGE of its
 * chunks, rewriting only the rest: a full collection of a mostly live
 * store writes little.  (Chosen again until none is newly kept: what a kept
 * file's garbage names counts as live in the files it is in.)  A file kept whole keeps its garbage too, and with
 * it the store's closure: whatever its chunks name, live or not, is marked
 * (and copied from the files rewritten).
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/reclaim_copying.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"
#include "miscadmin.h"
#include "utils/memutils.h"

/* a settled file with no more garbage than this is kept whole */
#define REWRITE_GARBAGE		0.5

typedef struct Mark
{
	const ProllyChunkStoreRoutine *cs;
	ProllyChunkGc *gc;
	uint64		nslots;
	uint8	   *marks;			/* a bit a slot */
	uint64	   *queue;			/* marked, not yet read */
	uint64		head;
	uint64		tail;
	uint64		cap;
	uint8		hash_len;
	StringInfoData buf;
} Mark;

static void
mark_slot(Mark *mk, uint64 slot)
{
	if (mk->marks[slot / 8] & (1 << (slot % 8)))
		return;
	mk->marks[slot / 8] |= 1 << (slot % 8);
	if (mk->tail == mk->cap)
	{
		mk->cap *= 2;
		mk->queue = repalloc_huge(mk->queue, sizeof(uint64) * mk->cap);
	}
	mk->queue[mk->tail++] = slot;
}

static void
mark_hash(Mark *mk, const ProllyHash *hash)
{
	int64		slot = PROLLY_CALL(mk->cs, gc_find, mk->gc, hash);

	if (slot >= 0)
		mark_slot(mk, (uint64) slot);
}

/* Mark every collected chunk data names. */
static void
mark_named(Mark *mk, const char *data, Size len)
{
	uint8		hl = mk->hash_len;
	ProllyHash	h;

	if (hl == 0)
		return;
	memset(&h, 0, sizeof(h));
	h.len = hl;
	for (Size i = 0; i + 1 + hl <= len; i++)
	{
		if ((uint8) data[i] != hl)
			continue;
		memcpy(h.bytes, data + i + 1, hl);
		mark_hash(mk, &h);
	}
}

/* Read what was marked, marking what it names, until nothing is left. */
static void
drain(Mark *mk)
{
	while (mk->head < mk->tail)
	{
		uint64		slot = mk->queue[mk->head++];

		resetStringInfo(&mk->buf);
		PROLLY_CALL(mk->cs, gc_read, mk->gc, slot, &mk->buf);
		mark_named(mk, mk->buf.data, mk->buf.len);
		CHECK_FOR_INTERRUPTS();
	}
	mk->head = mk->tail = 0;
}

/* What is new since the seal: a root, or a chunk kept whole. */
static void
mark_new(const ProllyHash *hash, const char *data, Size len, void *arg)
{
	Mark	   *mk = arg;

	if (mk->hash_len == 0)
		mk->hash_len = hash->len;
	if (data == NULL)
		mark_hash(mk, hash);
	else
		mark_named(mk, data, len);
}

static void
count_file(int file, uint64 first, uint64 nslots, bool settled, uint64 size,
		   void *arg)
{
	(*(int *) arg)++;
}

/* a full collection's choice of the files it keeps whole */
typedef struct Keep
{
	Mark	   *mk;
	bool	   *keep;
	bool		changed;		/* a file was newly kept */
} Keep;

/* A settled file with little garbage, as marked so far, is kept whole. */
static void
choose_file(int file, uint64 first, uint64 nslots, bool settled, uint64 size,
			void *arg)
{
	Keep	   *k = arg;
	uint64		live = 0;

	if (k->keep[file] || !settled || nslots == 0)
		return;
	for (uint64 slot = first; slot < first + nslots; slot++)
		live += (k->mk->marks[slot / 8] >> (slot % 8)) & 1;
	if ((double) (nslots - live) <= REWRITE_GARBAGE * nslots)
	{
		k->keep[file] = true;
		k->changed = true;
	}
}

/*
 * What a file kept whole names, live or not, is kept with it: its chunks
 * marked (those marked before were read already), then read.
 */
static void
mark_from_kept(int file, uint64 first, uint64 nslots, bool settled,
			   uint64 size, void *arg)
{
	Keep	   *k = arg;

	if (!k->keep[file])
		return;
	for (uint64 slot = first; slot < first + nslots; slot++)
		mark_slot(k->mk, slot);
	drain(k->mk);
}

static void
copying_collect(bool full, ProllyGcResult *result)
{
	const ProllyChunkStoreRoutine *cs = prolly_chunk_store();
	ProllyChunkStore *store = PROLLY_CALL(cs, open, MyDatabaseId,
										  MyDatabaseTableSpace);
	Mark		mk;
	ProllyHash	root;
	int			nfiles = 0;
	bool	   *keep;

	memset(&mk, 0, sizeof(mk));
	mk.cs = cs;
	mk.gc = PROLLY_CALL(cs, gc_begin, store, full, &root, &mk.nslots);
	mk.marks = MemoryContextAllocHuge(CurrentMemoryContext, mk.nslots / 8 + 1);
	memset(mk.marks, 0, mk.nslots / 8 + 1);
	mk.cap = 1024;
	mk.queue = palloc(sizeof(uint64) * mk.cap);
	mk.hash_len = root.len;
	initStringInfo(&mk.buf);
	PROLLY_CALL(cs, gc_files, mk.gc, count_file, &nfiles);
	keep = palloc0(sizeof(bool) * Max(nfiles, 1));

	/* what the root as of the seal reaches */
	if (root.len > 0)
		mark_hash(&mk, &root);
	drain(&mk);

	/* what is new since, before the window */
	PROLLY_CALL(cs, gc_new, mk.gc, false, mark_new, &mk);
	drain(&mk);

	/* the settled files kept whole, and what they name */
	if (full)
	{
		Keep		k = {&mk, keep, true};

		/*
		 * Until no file is newly kept: marking from a kept file's garbage
		 * may bring another file to little enough garbage to keep.
		 */
		while (k.changed)
		{
			k.changed = false;
			PROLLY_CALL(cs, gc_files, mk.gc, choose_file, &k);
			if (k.changed)
				PROLLY_CALL(cs, gc_files, mk.gc, mark_from_kept, &k);
		}
	}

	/* copied, before the window */
	PROLLY_CALL(cs, gc_copy, mk.gc, mk.marks, keep);

	/* in the window: what came since */
	PROLLY_CALL(cs, gc_new, mk.gc, true, mark_new, &mk);
	drain(&mk);
	PROLLY_CALL(cs, gc_swap, mk.gc, mk.marks, keep, result);
	PROLLY_CALL(cs, gc_retire, mk.gc);
}

const ProllyReclaimRoutine prolly_reclaim_copying = {
	.info = {
		.iface = PROLLY_IFACE_RECLAMATION,
		.impl_id = 1,
		.format_version = 1,
		.name = "copying",
		.description = "marks what is live and has the chunk store copy it "
		"to new files, online",
	},
	.collect = copying_collect,
};
