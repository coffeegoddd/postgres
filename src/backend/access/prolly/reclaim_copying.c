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
 * may read it has ended.  Reading is paced by the cost-based vacuum delay
 * where one is active (autovacuum's), a chunk read costing a page missed;
 * never in the window, where interrupts are held.
 *
 * What a chunk names is what its format declares (prolly_chunk_references(),
 * the tree's references()): a node's children, a leaf's values stored out
 * of line, and the references its items declare (a row's value trees, a
 * commit's root, parents and dependencies, the refs' heads, baselines and
 * merges, a canonical root's data trees, a baseline's).  Nothing else in
 * a chunk's bytes is read as a reference, so bytes that happen to spell a
 * hash keep nothing.  (Assert-enabled builds check it: a collected chunk
 * whose hash a chunk's bytes hold, as a reference would, and that the
 * chunk doesn't declare, is warned of.)
 *
 * Tables are roots too: a versioned table's values out of line are value
 * trees its rows name by hash pointers (prolly_toast.h), which no ref
 * reaches.  Every tree a pointer still on a page of the database's page
 * stores names is marked, dead tuples' too (prolly_toast_roots()), after
 * the transactions that named a tree before the collection began have
 * ended; and every tree a transaction names while it runs (the store's
 * keep set, gc_kept()), before the window and in it.  A keep set too large
 * to remember keeps every chunk collected.  So is every tree a hash pointer
 * in the WAL may name from the oldest position a logical slot may decode
 * from, this server's or a standby's (prolly_toast_wal_roots()): the slot
 * fetches the values of the rows it decodes, which may be gone from the
 * pages by then.  WAL from there no longer here keeps every chunk.
 *
 * The chunks marked and not yet read wait in a queue, breadth first, of
 * at most maintenance_work_mem in memory (a ring, so taking one and adding
 * one costs the same however full it is); past that it goes on in a
 * temporary file, read back as the memory empties.  (The marks themselves
 * are a bit a chunk collected.)
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
#include "access/prolly_toast.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "replication/slot.h"
#include "replication/walsender.h"
#include "storage/ipc.h"
#include "storage/buffile.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"

/* a settled file with no more garbage than this is kept whole */
#define REWRITE_GARBAGE		0.5

typedef struct Mark
{
	const ProllyChunkStoreRoutine *cs;
	ProllyChunkGc *gc;
	uint64		nslots;
	uint8	   *marks;			/* a bit a slot */
	uint64	   *queue;			/* marked, not yet read: a ring, count of
								 * them from head */
	uint64		head;
	uint64		count;
	uint64		cap;			/* of queue: maintenance_work_mem's worth */
	BufFile    *spill;			/* and the rest, past cap, in order */
	uint64		spill_read;		/* slots read back from it */
	uint64		spill_written;	/* and written */
	uint8		hash_len;
	StringInfoData buf;
} Mark;

static void
mark_slot(Mark *mk, uint64 slot)
{
	if (mk->marks[slot / 8] & (1 << (slot % 8)))
		return;
	mk->marks[slot / 8] |= 1 << (slot % 8);
	/* (once some are in the file, the rest follow them there, in order) */
	if (mk->count == mk->cap || mk->spill_written > mk->spill_read)
	{
		if (mk->spill == NULL)
			mk->spill = BufFileCreateTemp(false);
		if (BufFileSeek(mk->spill, 0, mk->spill_written * sizeof(uint64),
						SEEK_SET) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not seek in the collection's temporary file: %m")));
		BufFileWrite(mk->spill, &slot, sizeof(slot));
		mk->spill_written++;
		return;
	}
	mk->queue[(mk->head + mk->count++) % mk->cap] = slot;
}

/* The next slot marked and not yet read; false if none is left. */
static bool
next_marked(Mark *mk, uint64 *slot)
{
	if (mk->count == 0)
	{
		uint64		n = Min(mk->cap, mk->spill_written - mk->spill_read);

		mk->head = 0;
		if (n == 0)
		{
			mk->spill_read = mk->spill_written = 0;
			return false;
		}
		if (BufFileSeek(mk->spill, 0, mk->spill_read * sizeof(uint64),
						SEEK_SET) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not seek in the collection's temporary file: %m")));
		BufFileReadExact(mk->spill, mk->queue, n * sizeof(uint64));
		mk->spill_read += n;
		mk->count = n;
		if (mk->spill_read == mk->spill_written)
			mk->spill_read = mk->spill_written = 0;
	}
	*slot = mk->queue[mk->head];
	mk->head = (mk->head + 1) % mk->cap;
	mk->count--;
	return true;
}

static void
mark_hash(Mark *mk, const ProllyHash *hash)
{
	int64		slot = PROLLY_CALL(mk->cs, gc_find, mk->gc, hash);

	if (slot >= 0)
		mark_slot(mk, (uint64) slot);
}

static void
mark_ref(const ProllyHash *hash, void *arg)
{
	Mark	   *mk = arg;

	if (mk->hash_len == 0)
		mk->hash_len = hash->len;
	mark_hash(mk, hash);
}

#ifdef USE_ASSERT_CHECKING
static int
hash_cmp(const void *a, const void *b)
{
	const ProllyHash *x = a;
	const ProllyHash *y = b;

	if (x->len != y->len)
		return x->len < y->len ? -1 : 1;
	return memcmp(x->bytes, y->bytes, x->len);
}

typedef struct Declared
{
	ProllyHash *hashes;
	int			n;
	int			max;
} Declared;

static void
note_declared(const ProllyHash *hash, void *arg)
{
	Declared   *d = arg;

	if (d->n == d->max)
	{
		d->max = Max(16, d->max * 2);
		d->hashes = d->hashes ? repalloc_array(d->hashes, ProllyHash, d->max) :
			palloc_array(ProllyHash, d->max);
	}
	d->hashes[d->n++] = *hash;
}

/*
 * Every collected chunk the values of data's items name as a reference
 * would (the hash's length, then the hash) is one data declares.  (Keys
 * are passed over: a keyless row's key holds its content's hash, which
 * may be a chunk's, naming nothing; and so are a value tree's leaves, with
 * no keys: what a value stored out of line names, the item that stores it
 * declares.)
 */
static void
check_declared(Mark *mk, const char *data, Size len)
{
	const ProllyChunkFormatRoutine *format = prolly_chunkformat_lookup("basic");
	ProllyChunkNode node;
	uint8		hl = mk->hash_len;
	Declared	d = {0};
	ProllyHash	h;

	if (hl == 0 || !PROLLY_CALL(format, open, (const uint8 *) data, len, &node))
		return;
	prolly_chunk_references(data, len, note_declared, &d);
	/* (sorted, so each run of bytes is looked up, not compared with all) */
	if (d.n > 1)
		qsort(d.hashes, d.n, sizeof(ProllyHash), hash_cmp);
	memset(&h, 0, sizeof(h));
	h.len = hl;
	for (int item = 0; item < node.nitems; item++)
	{
		ProllyKey	k;
		ProllyValue v;
		uint8		flags;

		PROLLY_CALL(format, item_get, &node, item, &k, &v, &flags);
		if (node.level == 0 && k.len == 0)
			continue;
		for (Size i = 0; i + 1 + hl <= v.len; i++)
		{
			if (v.data[i] != hl)
				continue;
			memcpy(h.bytes, v.data + i + 1, hl);
			if (PROLLY_CALL(mk->cs, gc_find, mk->gc, &h) < 0)
				continue;
			if (d.n == 0 ||
				bsearch(&h, d.hashes, d.n, sizeof(ProllyHash), hash_cmp) == NULL)
				elog(WARNING, "a chunk names chunk %s, which it doesn't declare",
					 prolly_hash_hex(&h));
		}
	}
	if (d.hashes)
		pfree(d.hashes);
}
#endif

/* Mark every collected chunk data declares. */
static void
mark_named(Mark *mk, const char *data, Size len)
{
	prolly_chunk_references(data, len, mark_ref, mk);
#ifdef USE_ASSERT_CHECKING
	check_declared(mk, data, len);
#endif
}

/*
 * A root outside the refs.  (The first may come before any chunk told how
 * long the store's hashes are: a store with no root yet.)
 */
static void
mark_root(const ProllyHash *hash, void *arg)
{
	Mark	   *mk = arg;

	if (mk->hash_len == 0)
		mk->hash_len = hash->len;
	mark_hash(mk, hash);
}

/* Every chunk collected, as a keep set too large to remember asks. */
static void
mark_all(Mark *mk)
{
	for (uint64 slot = 0; slot < mk->nslots; slot++)
		mark_slot(mk, slot);
}

/* Read what was marked, marking what it names, until nothing is left. */
static void
drain(Mark *mk)
{
	uint64		slot;

	while (next_marked(mk, &slot))
	{
		resetStringInfo(&mk->buf);
		PROLLY_CALL(mk->cs, gc_read, mk->gc, slot, &mk->buf);
		mark_named(mk, mk->buf.data, mk->buf.len);

		/* (a chunk read costs as a page missed, where a delay is active) */
		if (VacuumCostActive)
			VacuumCostBalance += VacuumCostPageMiss;
		vacuum_delay_point(false);
	}
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

/* (a collection's reading of the standbys' positions, ended by an error) */
static void
horizon_read_end(int code, Datum arg)
{
	WalSndHorizonReadEnd();
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
	XLogRecPtr	horizon;

	memset(&mk, 0, sizeof(mk));
	mk.cs = cs;
	mk.gc = PROLLY_CALL(cs, gc_begin, store,
						full ? PROLLY_GC_FULL : PROLLY_GC_DEFAULT,
						&root, &mk.nslots);
	mk.marks = MemoryContextAllocHuge(CurrentMemoryContext, mk.nslots / 8 + 1);
	memset(mk.marks, 0, mk.nslots / 8 + 1);
	mk.cap = Max((uint64) maintenance_work_mem * 1024 / sizeof(uint64), 1024);
	mk.queue = MemoryContextAllocHuge(CurrentMemoryContext, sizeof(uint64) * mk.cap);
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

	/* what the store keeps for its own reasons */
	PROLLY_CALL(cs, gc_retained, mk.gc, mark_new, &mk);
	drain(&mk);

	/* what tables name, once those who named a tree before have ended */
	prolly_toast_roots(mark_root, &mk);

	/*
	 * What a logical slot, here or a standby's, may still decode: read as
	 * one of the readers of the standbys' positions until the swap, so a
	 * standby's slot created meanwhile is told it is kept only once this
	 * collection, which may not keep it, is done.
	 */
	WalSndHorizonReadBegin();
	PG_ENSURE_ERROR_CLEANUP(horizon_read_end, (Datum) 0);
	{
		horizon = ReplicationSlotsLogicalDecodingHorizon(MyDatabaseId);
		if (!prolly_toast_wal_roots(horizon, mark_root, &mk))
		{
			ereport(WARNING,
					(errmsg("the WAL from %X/%08X, which a logical replication slot may still decode, is gone",
							LSN_FORMAT_ARGS(horizon)),
					 errdetail("The collection keeps every chunk it would have freed.")));
			mark_all(&mk);
		}
		INJECTION_POINT("prolly-gc-roots-scanned", NULL);
		if (!PROLLY_CALL(cs, gc_kept, mk.gc, mark_root, &mk))
			mark_all(&mk);
		drain(&mk);

		/* the settled files kept whole, and what they name */
		if (full)
		{
			Keep		k = {&mk, keep, true};

			/*
			 * Until no file is newly kept: marking from a kept file's
			 * garbage may bring another file to little enough garbage to
			 * keep.
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
		if (!PROLLY_CALL(cs, gc_kept, mk.gc, mark_root, &mk))
			mark_all(&mk);
		INJECTION_POINT("prolly-gc-kept", NULL);
		drain(&mk);
		PROLLY_CALL(cs, gc_swap, mk.gc, mk.marks, keep, result);
	}
	PG_END_ENSURE_ERROR_CLEANUP(horizon_read_end, (Datum) 0);
	WalSndHorizonReadEnd();
	PROLLY_CALL(cs, gc_retire, mk.gc);
	if (mk.spill != NULL)
		BufFileClose(mk.spill);
}

/* (nothing to mark: every chunk is) */
static void
mark_none(const ProllyHash *hash, const char *data, Size len, void *arg)
{
}

/*
 * A collection of the journal with every chunk marked: its sealed segments
 * rewritten as one table file, not settled (it keeps garbage, which the
 * next default collection collects), nothing dropped; the table files are
 * left as they are.  Copying is paced as marking is.
 */
static void
copying_move(ProllyGcResult *result)
{
	const ProllyChunkStoreRoutine *cs = prolly_chunk_store();
	ProllyChunkStore *store = PROLLY_CALL(cs, open, MyDatabaseId,
										  MyDatabaseTableSpace);
	ProllyChunkGc *gc;
	ProllyHash	root;
	uint64		nslots;
	uint8	   *marks;
	int			nfiles = 0;
	bool	   *keep;

	gc = PROLLY_CALL(cs, gc_begin, store, PROLLY_GC_JOURNAL, &root, &nslots);
	marks = MemoryContextAllocHuge(CurrentMemoryContext, nslots / 8 + 1);
	memset(marks, 0xFF, nslots / 8 + 1);
	PROLLY_CALL(cs, gc_files, gc, count_file, &nfiles);
	keep = palloc0(sizeof(bool) * Max(nfiles, 1));
	PROLLY_CALL(cs, gc_copy, gc, marks, keep);
	PROLLY_CALL(cs, gc_new, gc, true, mark_none, NULL);
	PROLLY_CALL(cs, gc_swap, gc, marks, keep, result);
	PROLLY_CALL(cs, gc_retire, gc);
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
	.move = copying_move,
};
