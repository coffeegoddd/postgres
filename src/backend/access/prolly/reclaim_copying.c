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
 * pages by then.  WAL from there no longer here (a standby's slot behind
 * what max_slot_wal_keep_size kept) is scanned from what is left, with a
 * warning: what only the rest named isn't kept.
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
 * A retire collects only some generations' settled files.  choose() picks
 * them by the store's estimates of their garbage: the oldest generation
 * thought at least REWRITE_GARBAGE garbage, then those after it as much,
 * within prolly.gc_retire_budget of their files' bytes.  A file kept whole
 * has its garbage as found recorded, so it isn't chosen again until a
 * sever or another retire notes more: an old file is read again only once
 * it is half garbage, which bounds what reading it costs by twice what it
 * frees.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/reclaim_copying.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_autovacuum.h"
#include "access/prolly_builtins.h"
#include "access/prolly_chunkformat.h"
#include "access/prolly_config.h"
#include "access/prolly_toast.h"
#include "access/prolly_window.h"
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
	bool		read_any;		/* the horizon's edges read through any
								 * settled file (a retire's) */
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
	HTAB	   *cut;			/* CutChunk, by hash: a boundary commit's
								 * chunks, with the parents not followed */
	HTAB	   *cut_slots;		/* CutSlot, by slot: the same, collected */
} Mark;

/*
 * A chunk of a boundary commit of the history horizon (prolly_window.h):
 * what it names but the parents the horizon severed is marked.
 */
typedef struct CutChunk
{
	ProllyHash	hash;			/* key */
	List	   *parents;		/* ProllyHash *, not followed */
} CutChunk;

typedef struct CutSlot
{
	uint64		slot;			/* key */
	CutChunk   *chunk;
} CutSlot;

typedef struct CutMark
{
	struct Mark *mk;
	List	   *parents;
} CutMark;

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

static bool
same_hash(const ProllyHash *a, const ProllyHash *b)
{
	return a->len == b->len && memcmp(a->bytes, b->bytes, a->len) == 0;
}

static void
mark_ref_uncut(const ProllyHash *hash, void *arg)
{
	CutMark    *cm = arg;
	ListCell   *lc;

	foreach(lc, cm->parents)
		if (same_hash(lfirst(lc), hash))
			return;
	mark_ref(hash, cm->mk);
}

/*
 * Mark what data declares, a boundary commit's chunk (cut) leaving out
 * the parents the horizon severed.
 */
static void
mark_named_cut(Mark *mk, const char *data, Size len, CutChunk *cut)
{
	CutMark		cm;

	if (cut == NULL)
	{
		mark_named(mk, data, len);
		return;
	}
	cm.mk = mk;
	cm.parents = cut->parents;
	prolly_chunk_references(data, len, mark_ref_uncut, &cm);
}

/* Is this chunk (by its hash, or slot) one of a boundary commit's? */
static CutChunk *
cut_of_hash(Mark *mk, const ProllyHash *hash)
{
	ProllyHash	key;

	if (mk->cut == NULL)
		return NULL;
	memset(&key, 0, sizeof(key));
	key.len = hash->len;
	memcpy(key.bytes, hash->bytes, hash->len);
	return hash_search(mk->cut, &key, HASH_FIND, NULL);
}

static CutChunk *
cut_of_slot(Mark *mk, uint64 slot)
{
	CutSlot    *cs;

	if (mk->cut_slots == NULL)
		return NULL;
	cs = hash_search(mk->cut_slots, &slot, HASH_FIND, NULL);
	return cs ? cs->chunk : NULL;
}

typedef struct CutBuild
{
	Mark	   *mk;
	List	   *children;
} CutBuild;

/*
 * A chunk collected, read as the collection reads (by its slot): what the
 * horizon's edges are read through, a collection reading nothing its
 * transaction would note as used.  False for one not collected.
 */
static bool
collected_read(void *arg, const ProllyHash *hash, StringInfo out)
{
	Mark	   *mk = arg;
	int64		slot = PROLLY_CALL(mk->cs, gc_find, mk->gc, hash);

	if (slot < 0)
		return false;
	PROLLY_CALL(mk->cs, gc_read, mk->gc, (uint64) slot, out);
	return true;
}

static void
note_child(const ProllyHash *ref, void *arg)
{
	CutBuild   *cb = arg;

	cb->children = lappend(cb->children,
						   memcpy(palloc(sizeof(ProllyHash)), ref,
								  sizeof(ProllyHash)));
}

/*
 * Note a horizon edge: each chunk of the boundary commit's tree (its
 * root's, and down an inner node to its children) is a cut chunk, not to
 * follow to the parent.
 */
static void
note_cut(const ProllyHash *boundary, const ProllyHash *parent, void *arg)
{
	CutBuild   *cb = arg;
	Mark	   *mk = cb->mk;
	const ProllyChunkFormatRoutine *format = prolly_chunkformat_lookup("basic");
	List	   *todo = list_make1(memcpy(palloc(sizeof(ProllyHash)), boundary,
										 sizeof(ProllyHash)));
	StringInfoData data;

	if (mk->cut == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(ProllyHash);
		ctl.entrysize = sizeof(CutChunk);
		ctl.hcxt = CurrentMemoryContext;
		mk->cut = hash_create("prolly collection cut chunks", 16, &ctl,
							  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		ctl.keysize = sizeof(uint64);
		ctl.entrysize = sizeof(CutSlot);
		mk->cut_slots = hash_create("prolly collection cut slots", 16, &ctl,
									HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	initStringInfo(&data);
	while (todo != NIL)
	{
		ProllyHash *h = llast(todo);
		ProllyChunkNode node;
		ProllyHash	key;
		CutChunk   *cc;
		bool		found;

		todo = list_delete_last(todo);
		resetStringInfo(&data);
		/* (a boundary commit gone already names nothing) */
		if (!(mk->read_any ? PROLLY_CALL(mk->cs, gc_read_any, mk->gc, h, &data) :
			  collected_read(mk, h, &data)) ||
			!PROLLY_CALL(format, open, (const uint8 *) data.data, data.len,
						 &node))
			continue;
		if (node.level > 0)
		{
			cb->children = NIL;
			prolly_chunk_references(data.data, data.len, note_child, cb);
			todo = list_concat(todo, cb->children);
			continue;
		}
		memset(&key, 0, sizeof(key));
		key.len = h->len;
		memcpy(key.bytes, h->bytes, h->len);
		cc = hash_search(mk->cut, &key, HASH_ENTER, &found);
		if (!found)
		{
			int64		slot = PROLLY_CALL(mk->cs, gc_find, mk->gc, h);

			cc->parents = NIL;
			if (slot >= 0)
			{
				uint64		s = (uint64) slot;
				CutSlot    *cs = hash_search(mk->cut_slots, &s, HASH_ENTER,
											 NULL);

				cs->chunk = cc;
			}
		}
		cc->parents = lappend(cc->parents,
							  memcpy(palloc(sizeof(ProllyHash)), parent,
									 sizeof(ProllyHash)));
	}
	pfree(data.data);
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
		mark_named_cut(mk, mk->buf.data, mk->buf.len, cut_of_slot(mk, slot));

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
		mark_named_cut(mk, data, len, cut_of_hash(mk, hash));
}

static void
count_file(int file, uint64 first, uint64 nslots, bool settled, uint64 size,
		   void *arg)
{
	(*(int *) arg)++;
}

/*
 * The size below which a settled file with garbage is rewritten, not kept
 * whole (prolly.gc_rewrite_below; -1: a quarter of prolly.gc_file_size).
 */
static uint64
rewrite_below(void)
{
	if (prolly_gc_rewrite_below < 0)
		return (uint64) prolly_gc_file_size * 1024 / 4;
	return (uint64) prolly_gc_rewrite_below * 1024;
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

	/*
	 * A small one with garbage is rewritten whatever its share: it costs
	 * little, and kept whole it would keep its garbage, and what that
	 * names, as long as it is kept (a generation's file of a few chunks,
	 * of history severed and of what stays).
	 */
	if (live < nslots && size < rewrite_below())
		return;
	if ((double) (nslots - live) <= REWRITE_GARBAGE * nslots)
	{
		k->keep[file] = true;
		k->changed = true;
		/*
		 * (its garbage as found, its estimate from here on: below the
		 * share a retire is chosen for, until more is noted)
		 */
		if (PROLLY_HAS(k->mk->cs, gc_measured))
			PROLLY_CALL(k->mk->cs, gc_measured, k->mk->gc, file, nslots - live);
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

/*
 * What a settled file kept whole lists in older generations: the copy of
 * the generation listed is kept, whichever copy marking took (one file's
 * list must stay true of what is there), but a boundary commit's severed
 * parent (a cut chunk's, as the leaf naming it says).
 */
static void
mark_listed(const ProllyHash *target, const ProllyHash *source, uint32 epoch,
			void *arg)
{
	Mark	   *mk = arg;
	int64		slot;

	if (source != NULL)
	{
		CutChunk   *cut = cut_of_hash(mk, source);

		if (cut != NULL)
		{
			ListCell   *lc;

			foreach(lc, cut->parents)
				if (same_hash(lfirst(lc), target))
					return;
		}
	}
	/* (epoch 0: any copy, as a reference marks) */
	if (epoch == 0)
	{
		mark_ref(target, mk);
		return;
	}
	slot = PROLLY_CALL(mk->cs, gc_find_in, mk->gc, target, epoch);
	if (slot >= 0)
		mark_slot(mk, (uint64) slot);
}

static void
mark_from_listed(int file, uint64 first, uint64 nslots, bool settled,
				 uint64 size, void *arg)
{
	Keep	   *k = arg;

	if (!k->keep[file] || !settled)
		return;
	PROLLY_CALL(k->mk->cs, gc_listed, k->mk->gc, file, mark_listed, k->mk);
	drain(k->mk);
}

/* (a collection's reading of the standbys' positions, ended by an error) */
static void
horizon_read_end(int code, Datum arg)
{
	WalSndHorizonReadEnd();
}

/*
 * A chunk as a retire reads the horizon's edges through: collected, or in
 * a settled file it doesn't collect (the refs, the boundary commits, may
 * be in one).
 */
static bool
any_read(void *arg, const ProllyHash *hash, StringInfo out)
{
	Mark	   *mk = arg;

	return PROLLY_CALL(mk->cs, gc_read_any, mk->gc, hash, out);
}

/* The refs tree's walk: the nodes left to read. */
typedef struct RefsWalk
{
	List	   *todo;			/* ProllyHash * */
} RefsWalk;

static void
push_refs_node(const ProllyHash *hash, void *arg)
{
	RefsWalk   *rw = arg;

	rw->todo = lappend(rw->todo,
					   memcpy(palloc(sizeof(ProllyHash)), hash, sizeof(ProllyHash)));
}

/*
 * The refs tree as of the seal (root): each of its nodes collected told
 * to the store, which lists what they name in older generations apart
 * (they change with every commit, and one no longer live, kept in a
 * settled file a retire doesn't collect, would otherwise keep what it
 * named), and marked.  A collection that isn't a retire reads only those
 * collected: what changed since the last collection is all of them, as
 * a node is written anew whenever one below it is.  A retire, which
 * reads no node's list, reads every node of the live tree through any
 * file, and marks what each of its leaves names.  What a node names past
 * the tree (commits, working sets) is marked as any chunk's references
 * are.
 */
static void
mark_refs_tree(Mark *mk, const ProllyHash *root, bool retire)
{
	RefsWalk	rw = {NIL};
	HASHCTL		ctl;
	HTAB	   *seen;
	StringInfoData data;

	if (root->len == 0 || !PROLLY_HAS(mk->cs, gc_refs_node))
		return;
	ctl.keysize = sizeof(ProllyHash);
	ctl.entrysize = sizeof(ProllyHash);
	ctl.hcxt = CurrentMemoryContext;
	seen = hash_create("prolly refs nodes walked", 256, &ctl,
					   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	initStringInfo(&data);
	push_refs_node(root, &rw);
	while (rw.todo != NIL)
	{
		ProllyHash *h = llast(rw.todo);
		ProllyHash	key;
		int64		slot;
		bool		found;

		rw.todo = list_delete_last(rw.todo);
		CHECK_FOR_INTERRUPTS();
		memset(&key, 0, sizeof(key));
		key.len = h->len;
		memcpy(key.bytes, h->bytes, h->len);
		pfree(h);
		(void) hash_search(seen, &key, HASH_ENTER, &found);
		if (found)
			continue;
		slot = PROLLY_CALL(mk->cs, gc_find, mk->gc, &key);
		resetStringInfo(&data);
		if (slot >= 0)
		{
			PROLLY_CALL(mk->cs, gc_refs_node, mk->gc, (uint64) slot);
			mark_slot(mk, (uint64) slot);
			PROLLY_CALL(mk->cs, gc_read, mk->gc, (uint64) slot, &data);
		}
		else if (!retire ||
				 !PROLLY_CALL(mk->cs, gc_read_any, mk->gc, &key, &data))
			continue;

		/*
		 * (an internal node's children are the tree's nodes; a leaf's
		 * references, past it, marked: drained, for one collected)
		 */
		if (!prolly_chunk_is_leaf(data.data, data.len))
			prolly_chunk_references(data.data, data.len, push_refs_node, &rw);
		else if (slot < 0)
			mark_named(mk, data.data, data.len);
		drain(mk);
	}
	pfree(data.data);
	hash_destroy(seen);
}

static bool collect_scoped(ProllyGcScope scope, uint32 lo, uint32 hi,
						   bool if_free, ProllyGcResult *result);

static bool
copying_collect(bool full, bool if_free, ProllyGcResult *result)
{
	return collect_scoped(full ? PROLLY_GC_FULL : PROLLY_GC_DEFAULT, 0, 0,
						  if_free, result);
}

/*
 * Retire generations lo to hi: what of their settled files nothing names
 * is freed, marking only what is collected (the journal, the files not
 * settled, and theirs), from the roots and what the other settled files'
 * sections list in them.  A file of theirs with little garbage is kept
 * whole; one with more rewritten, its chunks keeping their generations;
 * one all garbage dropped.
 */
static bool
copying_retire(uint32 lo, uint32 hi, bool if_free, ProllyGcResult *result)
{
	return collect_scoped(PROLLY_GC_RETIRE, lo, hi, if_free, result);
}

/* generations a choice may take: the settled files of an interval */
typedef struct Unit
{
	uint32		lo;
	uint32		hi;
	uint64		nchunks;
	uint64		garbage;
	uint64		size;
} Unit;

typedef struct Units
{
	Unit	   *units;
	int			n;
	int			max;
} Units;

static void
note_unit(const ProllyChunkGeneration *g, void *arg)
{
	Units	   *us = arg;

	if (!g->settled || g->epoch_lo == 0)
		return;
	if (us->n == us->max)
	{
		us->max = Max(16, us->max * 2);
		us->units = us->units ? repalloc(us->units, sizeof(Unit) * us->max) :
			palloc(sizeof(Unit) * us->max);
	}
	us->units[us->n].lo = g->epoch_lo;
	us->units[us->n].hi = g->epoch_hi;
	us->units[us->n].nchunks = g->nchunks;
	us->units[us->n].garbage = g->garbage;
	us->units[us->n].size = g->size;
	us->n++;
}

static int
unit_cmp(const void *a, const void *b)
{
	const Unit *x = a;
	const Unit *y = b;

	if (x->lo != y->lo)
		return x->lo < y->lo ? -1 : 1;
	return x->hi < y->hi ? -1 : (x->hi > y->hi ? 1 : 0);
}

/* Is a unit thought garbage enough to be retired? */
static bool
unit_due(const Unit *u)
{
	return u->nchunks > 0 && (double) u->garbage >= REWRITE_GARBAGE * u->nchunks;
}

/*
 * The settled files by generation, those of overlapping intervals one
 * unit (a retire takes a file whole), oldest first; then the oldest unit
 * thought at least REWRITE_GARBAGE garbage, and those after it as much,
 * within budget.  Cost: the manifest, and each file's section's head.
 */
static bool
copying_choose(uint64 budget, uint32 max_hi, uint32 *lo, uint32 *hi)
{
	const ProllyChunkStoreRoutine *cs = prolly_chunk_store();
	ProllyChunkStore *store;
	Units		us = {0};
	int			n = 0;
	int			first = -1;
	uint64		bytes = 0;

	if (!PROLLY_HAS(cs, generations) || !PROLLY_HAS(cs, gc_begin_retire))
		return false;
	store = PROLLY_CALL(cs, open, MyDatabaseId, MyDatabaseTableSpace);
	PROLLY_CALL(cs, generations, store, note_unit, &us);
	if (us.n == 0)
		return false;
	qsort(us.units, us.n, sizeof(Unit), unit_cmp);
	/* (overlapping intervals merged) */
	for (int i = 0; i < us.n; i++)
	{
		if (n > 0 && us.units[i].lo <= us.units[n - 1].hi)
		{
			Unit	   *u = &us.units[n - 1];

			u->hi = Max(u->hi, us.units[i].hi);
			u->nchunks += us.units[i].nchunks;
			u->garbage += us.units[i].garbage;
			u->size += us.units[i].size;
			continue;
		}
		us.units[n++] = us.units[i];
	}
	for (int i = 0; i < n; i++)
	{
		Unit	   *u = &us.units[i];

		if (u->hi > max_hi)
			break;
		if (first < 0)
		{
			if (!unit_due(u))
				continue;
			first = i;
			*lo = u->lo;
		}
		else if (!unit_due(u) || bytes + u->size > budget)
			break;
		*hi = u->hi;
		bytes += u->size;
	}
	pfree(us.units);
	return first >= 0;
}

static bool
collect_scoped(ProllyGcScope scope, uint32 lo, uint32 hi, bool if_free,
			   ProllyGcResult *result)
{
	bool		full = (scope == PROLLY_GC_FULL);
	bool		retire = (scope == PROLLY_GC_RETIRE);
	const ProllyChunkStoreRoutine *cs = prolly_chunk_store();
	ProllyChunkStore *store = PROLLY_CALL(cs, open, MyDatabaseId,
										  MyDatabaseTableSpace);
	Mark		mk;
	ProllyHash	root;
	int			nfiles = 0;
	bool	   *keep;
	XLogRecPtr	horizon;
	XLogRecPtr	wanted;

	/*
	 * (a full collection reads the history horizon at its seal: its lock
	 * is taken first, as a sever or repair takes it before what it locks,
	 * and held until the transaction ends, so the horizon read is the one
	 * committed and stays so)
	 */
	if (full || retire)
	{
		ProllyDatabaseComponents dc;

		prolly_database_components(&dc);
		if (dc.history_window != NULL && PROLLY_HAS(dc.history_window, hold))
			PROLLY_CALL(dc.history_window, hold);
	}

	memset(&mk, 0, sizeof(mk));
	mk.cs = cs;
	if (retire)
		mk.gc = PROLLY_CALL(cs, gc_begin_retire, store, lo, hi, if_free,
							&root, &mk.nslots);
	else
		mk.gc = PROLLY_CALL(cs, gc_begin, store, scope, if_free, &root,
							&mk.nslots);
	if (mk.gc == NULL)
		return false;			/* (another collection running) */

	/*
	 * A retire relies on the settled files' generations to mark only what
	 * it collects: checked first where checking is on, refused if they're
	 * wrong, so it never frees by a list that is.
	 */
	if (retire && debug_prolly_verify_generations &&
		PROLLY_HAS(cs, verify_generations))
		(void) PROLLY_CALL(cs, verify_generations, store,
						   prolly_window_cut_edge, NULL);

	/*
	 * A fold's tree no ref names may be garbage now: the folds made so far
	 * are forgotten before the store's first gc_new() waits for the
	 * transactions running, so that none begun after it builds on one.
	 * (Those made from here on are appended since the seal, and kept.)
	 */
	prolly_forget_folds();
	mk.marks = MemoryContextAllocHuge(CurrentMemoryContext, mk.nslots / 8 + 1);
	memset(mk.marks, 0, mk.nslots / 8 + 1);
	mk.cap = Max((uint64) maintenance_work_mem * 1024 / sizeof(uint64), 1024);
	mk.queue = MemoryContextAllocHuge(CurrentMemoryContext, sizeof(uint64) * mk.cap);
	mk.hash_len = root.len;
	initStringInfo(&mk.buf);
	PROLLY_CALL(cs, gc_files, mk.gc, count_file, &nfiles);
	keep = palloc0(sizeof(bool) * Max(nfiles, 1));

	/*
	 * The history horizon's edges, as the root at the seal has them: its
	 * boundary commits' parents aren't followed from them (they are
	 * archived, or were let go of).  Only a full collection, which
	 * collects every file, reads them: a default one keeps what the
	 * boundary reaches, as its settled files keep what they reach.
	 */
	if ((full || retire) && root.len > 0)
	{
		ProllyDatabaseComponents dc;

		prolly_database_components(&dc);
		if (dc.history_window != NULL && PROLLY_HAS(dc.history_window, edges))
		{
			CutBuild	cb = {&mk, NIL};

			mk.read_any = retire;
			PROLLY_CALL(dc.history_window, edges, &root,
						retire ? any_read : collected_read, &mk,
						note_cut, &cb);
		}
	}

	/* the refs tree as of the seal, its nodes told to the store */
	mark_refs_tree(&mk, &root, retire);

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

	/* a retire's: what the settled files it doesn't collect name */
	if (retire)
	{
		PROLLY_CALL(cs, gc_inbound, mk.gc, mark_listed, &mk);
		drain(&mk);
	}

	/* what tables name, once those who named a tree before have ended */
	if (retire)
		prolly_toast_roots_in(lo, hi, mark_root, &mk);
	else
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
		horizon = ReplicationSlotsLogicalDecodingHorizonKept(MyDatabaseId,
															 &wanted);
		if (horizon != wanted)
			ereport(WARNING,
					(errmsg("the WAL from %X/%08X, which a logical replication slot may still decode, is gone",
							LSN_FORMAT_ARGS(wanted)),
					 errdetail("The collection keeps the values the WAL from %X/%08X names, not those named only before it.",
							   LSN_FORMAT_ARGS(horizon)),
					 errhint("A standby's logical slot this far behind may fail to decode a value. A physical slot's \"standby_restart_lsn\" in \"pg_replication_slots\" is its standby's position, which \"max_slot_wal_keep_size\" limits the WAL kept for.")));
		prolly_toast_wal_roots(horizon, mark_root, &mk);
		INJECTION_POINT("prolly-gc-roots-scanned", NULL);
		if (!PROLLY_CALL(cs, gc_kept, mk.gc, mark_root, &mk))
			mark_all(&mk);
		drain(&mk);

		/* the settled files kept whole, and what they name */
		if (full || retire)
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
			if (PROLLY_HAS(cs, gc_listed))
				PROLLY_CALL(cs, gc_files, mk.gc, mark_from_listed, &k);
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
	if (debug_prolly_verify_generations && PROLLY_HAS(cs, verify_generations))
		(void) PROLLY_CALL(cs, verify_generations, store,
						   prolly_window_cut_edge, NULL);
	if (mk.spill != NULL)
		BufFileClose(mk.spill);
	return true;
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

	gc = PROLLY_CALL(cs, gc_begin, store, PROLLY_GC_JOURNAL, false, &root,
					 &nslots);
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
	.retire = copying_retire,
	.choose = copying_choose,
};
