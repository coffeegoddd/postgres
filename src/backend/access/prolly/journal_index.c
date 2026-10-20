/*-------------------------------------------------------------------------
 *
 * journal_index.c
 *	  The chunk journal's index, shared by every backend of a database.
 *
 * Each database's journal has one index in dynamic shared memory: a
 * dshash of where each chunk's record is (its segment, offset and length,
 * and the transaction that appended it while that may still be in
 * progress; another copy of the chunk, while this one isn't everyone's),
 * and the state the backends reading the segments share: how far each
 * segment has been read, the ranges skipped there because their appends
 * were in flight, the PADs seen that may become ROOTs, and the root.  A
 * backend reads what was appended since, under the index's lock, for all
 * of them; so the journal is read once, not once a backend.
 *
 * All databases' indexes are in one DSA, found by a registry keyed by
 * database OID.  A dropped database's index is forgotten with it
 * (jindex_forget()), so an OID used again starts anew.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/journal_index.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "journal_index.h"
#include "lib/dshash.h"
#include "miscadmin.h"
#include "storage/dsm_registry.h"
#include "storage/lwlock.h"
#include "storage/procarray.h"
#include "utils/memutils.h"

/* the tranche of every index's locks, made once */
typedef struct IndexTranche
{
	int			tranche;
} IndexTranche;

/* a database's index, in the registry */
typedef struct Registered
{
	Oid			dboid;			/* key */
	dsa_pointer db;				/* its IndexShared */
} Registered;

/* a database's index */
typedef struct IndexShared
{
	LWLock		lock;			/* the scan state's */
	dshash_table_handle chunks;
	int			capacity;		/* PADs and holes a segment may have */
	JournalScan scan;
	int			segs_max;		/* segments segs has room for */
	dsa_pointer segs;			/* scan.nsegments segment blocks */
} IndexShared;

/* a chunk, and its first copy */
typedef struct Chunk
{
	uint8		key[JINDEX_MAX_HASH];	/* the hash, zero-padded */
	uint8		hash_len;
	uint64		segment;
	uint64		offset;
	uint32		len;
	TransactionId xid;			/* while not known to have ended; invalid:
								 * everyone's */
	dsa_pointer other;			/* another Copy, while this one isn't */
} Chunk;

/* another copy of a chunk */
typedef struct Copy
{
	uint64		segment;
	uint64		offset;
	uint32		len;
	TransactionId xid;
	dsa_pointer other;
} Copy;

struct JournalIndex
{
	Oid			dboid;
	IndexShared *db;
	dshash_table *chunks;
};

static dsa_area *area = NULL;
static dshash_table *registry = NULL;
static int	tranche = 0;

static void
init_tranche(void *ptr, void *arg)
{
	((IndexTranche *) ptr)->tranche = LWLockNewTrancheId("prolly journal index");
}

static const dshash_parameters registry_params = {
	sizeof(Oid),
	sizeof(Registered),
	dshash_memcmp,
	dshash_memhash,
	dshash_memcpy,
	0							/* (the registry's own) */
};

static dshash_parameters
chunk_params(void)
{
	dshash_parameters p = {
		JINDEX_MAX_HASH,
		sizeof(Chunk),
		dshash_memcmp,
		dshash_memhash,
		dshash_memcpy,
		tranche
	};

	return p;
}

/* The DSA, the registry and the tranche, once a process. */
static void
attach_shared(void)
{
	MemoryContext old;
	bool		found;
	IndexTranche *t;

	if (registry != NULL)
		return;
	old = MemoryContextSwitchTo(TopMemoryContext);
	t = GetNamedDSMSegment("prolly journal index tranche", sizeof(IndexTranche),
						   init_tranche, &found, NULL);
	tranche = t->tranche;
	area = GetNamedDSA("prolly journal index", &found);
	registry = GetNamedDSHash("prolly journal indexes", &registry_params,
							  &found);
	MemoryContextSwitchTo(old);
}

static Size
segment_size(int capacity)
{
	return MAXALIGN(sizeof(JournalSegScan)) +
		MAXALIGN(sizeof(uint64) * capacity) +
		MAXALIGN(sizeof(JournalHole) * capacity);
}

JournalIndex *
jindex_attach(Oid dboid, int capacity)
{
	Registered *r;
	bool		found;
	JournalIndex *ix;
	dshash_parameters params;
	MemoryContext old;

	attach_shared();
	params = chunk_params();
	old = MemoryContextSwitchTo(TopMemoryContext);
	r = dshash_find_or_insert(registry, &dboid, &found);
	if (!found)
	{
		IndexShared *db;
		dshash_table *t;

		r->db = dsa_allocate0(area, sizeof(IndexShared));
		db = dsa_get_address(area, r->db);
		LWLockInitialize(&db->lock, tranche);
		db->capacity = capacity;
		t = dshash_create(area, &params, NULL);
		db->chunks = dshash_get_hash_table_handle(t);
		dshash_detach(t);
	}
	ix = palloc(sizeof(JournalIndex));
	ix->dboid = dboid;
	ix->db = dsa_get_address(area, r->db);
	dshash_release_lock(registry, r);
	ix->chunks = dshash_attach(area, &params, ix->db->chunks, NULL);
	MemoryContextSwitchTo(old);
	return ix;
}

void
jindex_forget(Oid dboid)
{
	Registered *r;
	IndexShared *db;
	dshash_parameters params;
	dshash_table *t;

	attach_shared();
	params = chunk_params();
	r = dshash_find(registry, &dboid, true);
	if (r == NULL)
		return;
	db = dsa_get_address(area, r->db);
	t = dshash_attach(area, &params, db->chunks, NULL);
	{
		dshash_seq_status status;
		Chunk	   *c;

		/* (the copies first: the table's own memory goes with it) */
		dshash_seq_init(&status, t, true);
		while ((c = dshash_seq_next(&status)) != NULL)
		{
			for (dsa_pointer p = c->other; DsaPointerIsValid(p);)
			{
				dsa_pointer next = ((Copy *) dsa_get_address(area, p))->other;

				dsa_free(area, p);
				p = next;
			}
		}
		dshash_seq_term(&status);
	}
	dshash_destroy(t);
	if (DsaPointerIsValid(db->segs))
		dsa_free(area, db->segs);
	dsa_free(area, r->db);
	dshash_delete_entry(registry, r);
}

void
jindex_lock(JournalIndex *ix, bool exclusive)
{
	LWLockAcquire(&ix->db->lock, exclusive ? LW_EXCLUSIVE : LW_SHARED);
}

void
jindex_unlock(JournalIndex *ix)
{
	LWLockRelease(&ix->db->lock);
}

JournalScan *
jindex_scan(JournalIndex *ix)
{
	return &ix->db->scan;
}

int
jindex_capacity(JournalIndex *ix)
{
	return ix->db->capacity;
}

JournalSegScan *
jindex_segment(JournalIndex *ix, int i)
{
	char	   *segs;

	Assert(i >= 0 && i < ix->db->scan.nsegments);
	segs = dsa_get_address(area, ix->db->segs);
	return (JournalSegScan *) (segs + (Size) i * segment_size(ix->db->capacity));
}

JournalSegScan *
jindex_segment_of(JournalIndex *ix, uint64 id)
{
	for (int i = 0; i < ix->db->scan.nsegments; i++)
	{
		JournalSegScan *g = jindex_segment(ix, i);

		if (g->id == id)
			return g;
	}
	return NULL;
}

uint64 *
jindex_pads(JournalIndex *ix, JournalSegScan *g)
{
	return (uint64 *) ((char *) g + MAXALIGN(sizeof(JournalSegScan)));
}

JournalHole *
jindex_holes(JournalIndex *ix, JournalSegScan *g)
{
	return (JournalHole *) ((char *) jindex_pads(ix, g) +
							MAXALIGN(sizeof(uint64) * ix->db->capacity));
}

static void drop_segment(JournalIndex *ix, uint64 segment);

void
jindex_set_segments(JournalIndex *ix, const uint64 *ids, int n, uint64 start)
{
	IndexShared *db = ix->db;
	Size		size = segment_size(db->capacity);
	dsa_pointer segs = dsa_allocate(area, size * Max(n, 1));
	char	   *to = dsa_get_address(area, segs);
	int			old = db->scan.nsegments;
	uint64	   *dropped = palloc(sizeof(uint64) * Max(old, 1));
	int			ndropped = 0;

	for (int i = 0; i < n; i++)
	{
		JournalSegScan *g = jindex_segment_of(ix, ids[i]);
		JournalSegScan *ng = (JournalSegScan *) (to + (Size) i * size);

		if (g != NULL)
			memcpy(ng, g, size);
		else
		{
			memset(ng, 0, size);
			ng->id = ids[i];
			ng->scanned = start;
		}
	}
	for (int i = 0; i < old; i++)
	{
		uint64		id = jindex_segment(ix, i)->id;
		bool		kept = false;

		for (int j = 0; j < n && !kept; j++)
			kept = ids[j] == id;
		if (!kept)
			dropped[ndropped++] = id;
	}
	if (DsaPointerIsValid(db->segs))
		dsa_free(area, db->segs);
	db->segs = segs;
	db->scan.nsegments = n;
	for (int i = 0; i < ndropped; i++)
		drop_segment(ix, dropped[i]);
	pfree(dropped);
}

static void
hash_key(const ProllyHash *hash, uint8 *key)
{
	if (hash->len > JINDEX_MAX_HASH)
		elog(ERROR, "a chunk hash of %u bytes is too long for the journal's index",
			 hash->len);
	memset(key, 0, JINDEX_MAX_HASH);
	memcpy(key, hash->bytes, hash->len);
}

/*
 * May this backend read a copy (its transaction's xid)?  Not one of a
 * transaction in progress, unless it is its own; one known to have ended
 * is everyone's from then on.
 */
static bool
copy_visible(TransactionId *xid)
{
	if (!TransactionIdIsValid(*xid))
		return true;
	if (TransactionIdIsCurrentTransactionId(*xid))
		return true;
	if (!TransactionIdIsInProgress(*xid) || TransactionIdDidCommit(*xid))
	{
		*xid = InvalidTransactionId;
		return true;
	}
	return false;
}

void
jindex_add(JournalIndex *ix, const ProllyHash *hash, uint64 segment,
		   uint64 offset, uint32 len, TransactionId xid)
{
	uint8		key[JINDEX_MAX_HASH];
	Chunk	   *c;
	bool		found;
	dsa_pointer *link;
	Copy	   *last;

	hash_key(hash, key);
	c = dshash_find_or_insert(ix->chunks, key, &found);
	if (!found)
	{
		c->hash_len = hash->len;
		c->segment = segment;
		c->offset = offset;
		c->len = len;
		c->xid = xid;
		c->other = InvalidDsaPointer;
		dshash_release_lock(ix->chunks, c);
		return;
	}
	/* (one is everyone's already, or this copy is indexed) */
	if (!TransactionIdIsValid(c->xid) ||
		(c->segment == segment && c->offset == offset))
	{
		dshash_release_lock(ix->chunks, c);
		return;
	}
	link = &c->other;
	while (DsaPointerIsValid(*link))
	{
		last = dsa_get_address(area, *link);
		if (!TransactionIdIsValid(last->xid) ||
			(last->segment == segment && last->offset == offset))
		{
			dshash_release_lock(ix->chunks, c);
			return;
		}
		link = &last->other;
	}
	{
		dsa_pointer p = dsa_allocate(area, sizeof(Copy));
		Copy	   *n = dsa_get_address(area, p);

		n->segment = segment;
		n->offset = offset;
		n->len = len;
		n->xid = xid;
		n->other = InvalidDsaPointer;
		*link = p;
	}
	dshash_release_lock(ix->chunks, c);
}

/*
 * Where a copy of a chunk this backend may read is; with wait, a copy of a
 * transaction in progress is waited for, if none is readable now (a reader
 * following a root may meet one being committed).  False if none.
 */
bool
jindex_find(JournalIndex *ix, const ProllyHash *hash, bool wait,
			uint64 *segment, uint64 *offset, uint32 *len)
{
	uint8		key[JINDEX_MAX_HASH];

	hash_key(hash, key);
	for (;;)
	{
		Chunk	   *c = dshash_find(ix->chunks, key, true);

		if (c == NULL)
			return false;
		if (copy_visible(&c->xid))
		{
			*segment = c->segment;
			*offset = c->offset;
			*len = c->len;
			dshash_release_lock(ix->chunks, c);
			return true;
		}
		for (dsa_pointer p = c->other; DsaPointerIsValid(p);)
		{
			Copy	   *o = dsa_get_address(area, p);

			if (copy_visible(&o->xid))
			{
				*segment = o->segment;
				*offset = o->offset;
				*len = o->len;
				dshash_release_lock(ix->chunks, c);
				return true;
			}
			p = o->other;
		}
		dshash_release_lock(ix->chunks, c);
		if (!wait)
			return false;
		CHECK_FOR_INTERRUPTS();
		pg_usleep(1000L);
	}
}

bool
jindex_has(JournalIndex *ix, const ProllyHash *hash)
{
	uint64		segment;
	uint64		offset;
	uint32		len;

	return jindex_find(ix, hash, false, &segment, &offset, &len);
}

void
jindex_visit(JournalIndex *ix,
			 void (*visit) (const ProllyHash *hash, uint64 segment,
							int ordinal, void *arg),
			 void *arg)
{
	dshash_seq_status status;
	Chunk	   *c;
	int			ordinal = 0;

	dshash_seq_init(&status, ix->chunks, false);
	while ((c = dshash_seq_next(&status)) != NULL)
	{
		ProllyHash	h;

		memset(&h, 0, sizeof(h));
		h.len = c->hash_len;
		memcpy(h.bytes, c->key, c->hash_len);
		visit(&h, c->segment, ordinal, arg);
		for (dsa_pointer p = c->other; DsaPointerIsValid(p);)
		{
			Copy	   *o = dsa_get_address(area, p);

			visit(&h, o->segment, ordinal, arg);
			p = o->other;
		}
		ordinal++;
	}
	dshash_seq_term(&status);
}

/* Forget the copies in a segment no longer named. */
static void
drop_segment(JournalIndex *ix, uint64 segment)
{
	dshash_seq_status status;
	Chunk	   *c;

	dshash_seq_init(&status, ix->chunks, true);
	while ((c = dshash_seq_next(&status)) != NULL)
	{
		dsa_pointer *link = &c->other;

		while (DsaPointerIsValid(*link))
		{
			Copy	   *o = dsa_get_address(area, *link);

			if (o->segment == segment)
			{
				dsa_pointer gone = *link;

				*link = o->other;
				dsa_free(area, gone);
			}
			else
				link = &o->other;
		}
		if (c->segment != segment)
			continue;
		if (!DsaPointerIsValid(c->other))
			dshash_delete_current(&status);
		else
		{
			dsa_pointer next = c->other;
			Copy	   *o = dsa_get_address(area, next);

			c->segment = o->segment;
			c->offset = o->offset;
			c->len = o->len;
			c->xid = o->xid;
			c->other = o->other;
			dsa_free(area, next);
		}
	}
	dshash_seq_term(&status);
}
