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
 * (jindex_forget()), so an OID used again starts anew.  The DSA isn't
 * bounded: an index holds an entry for each chunk in its journal's
 * segments (about 64 bytes, and 48 more for each other copy), which a
 * move of the journal into a table file, or a collection, empties, so
 * prolly.journal_move_threshold bounds it as it bounds the journal.
 *
 * A checkpoint writes each index changed since the last as a file in its
 * database's directory, JINDEX_FILE (jindex_checkpoint()), which the
 * first backend to use the index after the server starts reads instead of
 * the whole journal (jindex_load()): the chunks, and how far each segment
 * had been read, but for the holes and PADs it hadn't settled, which are
 * read again (a segment's position saved is the first of those).  It is a
 * cache, not WAL-logged: one missing, of another format or naming a
 * segment the manifest doesn't is passed over, and the journal read.  The
 * records it names are in the journal after a crash too: a chunk is
 * indexed only once its bytes are written, so after its WAL record, which
 * redo writes again at the same place.
 *
 *		header	"PRLYJIX1", uint32 version (1), the store's format
 *				(uint8 length, its bytes), uint8 hash length
 *		root	uint8 length, the hash, uint64 segment, uint64 offset
 *		segments  uint32 count; each uint64 id, uint64 read up to
 *		chunks	uint64 count; each the hash, uint64 segment, uint64 offset,
 *				uint32 length, uint32 xid (0: everyone's)
 *		uint32 CRC-32C of all before it
 *
 * integers in the server's byte order (the file is the server's own).
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/journal_index.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "access/transam.h"
#include "access/xact.h"
#include "common/relpath.h"
#include "journal_index.h"
#include "lib/dshash.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/pg_crc32c.h"
#include "storage/dsm_registry.h"
#include "storage/fd.h"
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
	Oid			dboid;
	Oid			spcoid;
	pg_atomic_uint64 changes;	/* chunks and segments changed */
	uint64		saved;			/* changes as of the file last written */
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
jindex_attach(Oid dboid, Oid spcoid, int capacity, bool *made)
{
	Registered *r;
	bool		found;
	JournalIndex *ix;
	dshash_parameters params;
	MemoryContext old;

	attach_shared();
	params = chunk_params();
	old = MemoryContextSwitchTo(TopMemoryContext);
	/* (exclusive: the database's tablespace is set again, below) */
	r = dshash_find(registry, &dboid, true);
	found = r != NULL;
	if (r == NULL)
	{
		/*
		 * Made whole before it is registered, so an allocation that fails
		 * leaves no entry another backend would take for one made (and
		 * frees what it made); one another backend registered meanwhile is
		 * taken instead.
		 */
		dsa_pointer created = dsa_allocate0(area, sizeof(IndexShared));
		IndexShared *db = dsa_get_address(area, created);
		dshash_table *t = NULL;

		PG_TRY();
		{
			db->dboid = dboid;
			db->spcoid = spcoid;
			pg_atomic_init_u64(&db->changes, 0);
			LWLockInitialize(&db->lock, tranche);
			db->capacity = capacity;
			t = dshash_create(area, &params, NULL);
			db->chunks = dshash_get_hash_table_handle(t);
			r = dshash_find_or_insert(registry, &dboid, &found);
		}
		PG_CATCH();
		{
			if (t != NULL)
				dshash_destroy(t);
			dsa_free(area, created);
			PG_RE_THROW();
		}
		PG_END_TRY();
		if (found)
		{
			dshash_destroy(t);
			dsa_free(area, created);
		}
		else
		{
			r->db = created;
			dshash_detach(t);
		}
	}
	ix = palloc(sizeof(JournalIndex));
	ix->dboid = dboid;
	ix->db = dsa_get_address(area, r->db);
	/* (moved since, by ALTER DATABASE SET TABLESPACE: its file goes there) */
	ix->db->spcoid = spcoid;
	dshash_release_lock(registry, r);
	ix->chunks = dshash_attach(area, &params, ix->db->chunks, NULL);
	MemoryContextSwitchTo(old);
	*made = !found;
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

static void drop_segments(JournalIndex *ix, const uint64 *dropped,
						  int ndropped);

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
	/* (a file saved names the segments: changed if they did) */
	if (n != old || ndropped > 0)
		pg_atomic_fetch_add_u64(&db->changes, 1);
	if (ndropped > 0)
		drop_segments(ix, dropped, ndropped);
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
		pg_atomic_fetch_add_u64(&ix->db->changes, 1);
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
	pg_atomic_fetch_add_u64(&ix->db->changes, 1);
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

/* is segment one of the n of dropped? */
static bool
dropped_segment(const uint64 *dropped, int n, uint64 segment)
{
	for (int i = 0; i < n; i++)
	{
		if (dropped[i] == segment)
			return true;
	}
	return false;
}

/* Forget the copies in the segments no longer named, in one pass. */
static void
drop_segments(JournalIndex *ix, const uint64 *dropped, int ndropped)
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

			if (dropped_segment(dropped, ndropped, o->segment))
			{
				dsa_pointer gone = *link;

				*link = o->other;
				dsa_free(area, gone);
			}
			else
				link = &o->other;
		}
		if (!dropped_segment(dropped, ndropped, c->segment))
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

/* ----------------------------------------------------------------
 * The index as a file
 * ----------------------------------------------------------------
 */

#define FILE_MAGIC		"PRLYJIX1"
#define FILE_VERSION	1

/*
 * The file written as it is made, through a buffer, its CRC kept as it
 * goes (so its size isn't bounded by what one allocation holds); ok goes
 * false at the first write that fails.
 */
#define WRITER_BUFFER	(64 * 1024)
typedef struct IndexWriter
{
	int			fd;
	bool		ok;
	pg_crc32c	crc;
	Size		len;
	char		buf[WRITER_BUFFER];
} IndexWriter;

static void
writer_flush(IndexWriter *w)
{
	if (w->len == 0)
		return;
	COMP_CRC32C(w->crc, w->buf, w->len);
	if (w->ok && write(w->fd, w->buf, w->len) != w->len)
		w->ok = false;
	w->len = 0;
}

static void
put_bytes(IndexWriter *w, const void *p, Size n)
{
	while (n > 0)
	{
		Size		take = Min(n, WRITER_BUFFER - w->len);

		memcpy(w->buf + w->len, p, take);
		w->len += take;
		p = (const char *) p + take;
		n -= take;
		if (w->len == WRITER_BUFFER)
			writer_flush(w);
	}
}

/*
 * A segment's position to save: the first of what it read up to, its
 * holes and its PADs, so all of those are read again.
 */
static uint64
saved_position(JournalIndex *ix, JournalSegScan *g)
{
	uint64		pos = g->scanned;
	uint64	   *pads = jindex_pads(ix, g);
	JournalHole *holes = jindex_holes(ix, g);

	for (int i = 0; i < g->npads; i++)
		pos = Min(pos, pads[i]);
	for (int i = 0; i < g->nholes; i++)
		pos = Min(pos, holes[i].offset);
	return pos;
}

/*
 * Write ix's file, aside and then renamed over the last: written (to the
 * OS) under the index's lock, shared, as it is read; made durable and
 * renamed after.  Laid out: magic, version, the store's format, the hash's
 * length, the root (its length, bytes, segment and offset), the segments
 * (each's id and position), each copy of each chunk (its hash, segment,
 * offset, length and xid), then how many copies, and the CRC of all of it.
 */
static void
save_index(JournalIndex *ix, const char *format, int format_len)
{
	IndexShared *db = ix->db;
	char	   *dir = GetDatabasePath(db->dboid, db->spcoid);
	char	   *path = psprintf("%s/%s", dir, JINDEX_FILE);
	char	   *temp = psprintf("%s.tmp", path);
	IndexWriter *w;
	uint32		version = FILE_VERSION;
	uint8		flen = (uint8) format_len;
	uint8		hlen = 0;
	uint64		changes;
	uint64		count = 0;
	dshash_seq_status status;
	Chunk	   *c;
	bool		ok;

	w = palloc(sizeof(IndexWriter));
	w->fd = OpenTransientFile(temp, O_RDWR | O_CREAT | O_TRUNC | PG_BINARY);
	if (w->fd < 0)
	{
		/* (a database dropped meanwhile) */
		if (errno != ENOENT)
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", temp)));
		pfree(w);
		pfree(temp);
		pfree(path);
		pfree(dir);
		return;
	}
	w->ok = true;
	w->len = 0;
	INIT_CRC32C(w->crc);
	put_bytes(w, FILE_MAGIC, 8);
	put_bytes(w, &version, sizeof(version));
	put_bytes(w, &flen, 1);
	put_bytes(w, format, format_len);
	jindex_lock(ix, false);
	changes = pg_atomic_read_u64(&db->changes);
	/* (the hash's length: the root's, or any chunk's) */
	hlen = db->scan.root.len;
	if (hlen == 0)
	{
		dshash_seq_init(&status, ix->chunks, false);
		if ((c = dshash_seq_next(&status)) != NULL)
			hlen = c->hash_len;
		dshash_seq_term(&status);
	}
	{
		uint32		nsegs = db->scan.nsegments;

		put_bytes(w, &hlen, 1);
		put_bytes(w, &db->scan.root.len, 1);
		put_bytes(w, db->scan.root.bytes, db->scan.root.len);
		put_bytes(w, &db->scan.root_segment, 8);
		put_bytes(w, &db->scan.root_offset, 8);
		put_bytes(w, &nsegs, 4);
		for (int i = 0; i < db->scan.nsegments; i++)
		{
			JournalSegScan *g = jindex_segment(ix, i);
			uint64		pos = saved_position(ix, g);

			put_bytes(w, &g->id, 8);
			put_bytes(w, &pos, 8);
		}
	}
	dshash_seq_init(&status, ix->chunks, false);
	while ((c = dshash_seq_next(&status)) != NULL)
	{
		for (dsa_pointer p = InvalidDsaPointer;;)
		{
			uint64		segment = c->segment;
			uint64		offset = c->offset;
			uint32		len = c->len;
			TransactionId xid = c->xid;

			if (DsaPointerIsValid(p))
			{
				Copy	   *o = dsa_get_address(area, p);

				segment = o->segment;
				offset = o->offset;
				len = o->len;
				xid = o->xid;
			}
			if (TransactionIdIsValid(xid) && !TransactionIdIsInProgress(xid))
				xid = InvalidTransactionId;
			if (c->hash_len == hlen)
			{
				put_bytes(w, c->key, c->hash_len);
				put_bytes(w, &segment, 8);
				put_bytes(w, &offset, 8);
				put_bytes(w, &len, 4);
				put_bytes(w, &xid, 4);
				count++;
			}
			p = DsaPointerIsValid(p) ? ((Copy *) dsa_get_address(area, p))->other :
				c->other;
			if (!DsaPointerIsValid(p))
				break;
		}
	}
	dshash_seq_term(&status);
	jindex_unlock(ix);
	put_bytes(w, &count, 8);
	writer_flush(w);
	FIN_CRC32C(w->crc);
	if (w->ok && write(w->fd, &w->crc, sizeof(w->crc)) != sizeof(w->crc))
		w->ok = false;

	ok = w->ok && pg_fsync(w->fd) == 0;
	if (CloseTransientFile(w->fd) != 0)
		ok = false;
	if (ok && durable_rename(temp, path, LOG) == 0)
		db->saved = changes;
	else
		(void) unlink(temp);
	pfree(w);
	pfree(temp);
	pfree(path);
	pfree(dir);
}

void
jindex_checkpoint(const char *format, int format_len)
{
	dshash_seq_status status;
	Registered *r;
	List	   *dbs = NIL;
	ListCell   *lc;
	dshash_parameters params;

	attach_shared();
	params = chunk_params();
	dshash_seq_init(&status, registry, false);
	while ((r = dshash_seq_next(&status)) != NULL)
	{
		IndexShared *db = dsa_get_address(area, r->db);

		if (pg_atomic_read_u64(&db->changes) != db->saved)
			dbs = lappend_oid(dbs, r->dboid);
	}
	dshash_seq_term(&status);
	foreach(lc, dbs)
	{
		Oid			dboid = lfirst_oid(lc);
		JournalIndex ix;

		/* (the registry's entry held while written: a drop waits) */
		r = dshash_find(registry, &dboid, false);
		if (r == NULL)
			continue;
		ix.dboid = dboid;
		ix.db = dsa_get_address(area, r->db);
		ix.chunks = dshash_attach(area, &params, ix.db->chunks, NULL);
		save_index(&ix, format, format_len);
		dshash_detach(ix.chunks);
		dshash_release_lock(registry, r);
	}
	list_free(dbs);
}

/* (a reader of the file, which fails at its end) */
typedef struct FileReader
{
	const char *p;
	const char *end;
} FileReader;

static bool
take(FileReader *fr, void *out, Size n)
{
	if ((Size) (fr->end - fr->p) < n)
		return false;
	memcpy(out, fr->p, n);
	fr->p += n;
	return true;
}

bool
jindex_load(JournalIndex *ix, const char *format, int format_len,
			const uint64 *ids, const uint64 *sizes, int n, uint64 start)
{
	IndexShared *db = ix->db;
	char	   *dir = GetDatabasePath(db->dboid, db->spcoid);
	char	   *path = psprintf("%s/%s", dir, JINDEX_FILE);
	int			fd;
	struct stat st;
	char	   *buf = NULL;
	FileReader	fr;
	char		magic[8];
	uint32		version;
	uint8		flen;
	uint8		hlen;
	ProllyHash	root = {0};
	uint64		root_segment;
	uint64		root_offset;
	uint32		nsegs;
	uint64	   *seg_ids = NULL;
	uint64	   *seg_pos = NULL;
	uint64		count;
	pg_crc32c	crc;
	bool		loaded = false;

	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	if (fd < 0)
		goto done;
	if (fstat(fd, &st) != 0 || st.st_size < (off_t) sizeof(crc))
	{
		CloseTransientFile(fd);
		goto done;
	}
	buf = palloc_extended(st.st_size, MCXT_ALLOC_HUGE);
	if (read(fd, buf, st.st_size) != st.st_size)
	{
		CloseTransientFile(fd);
		goto done;
	}
	CloseTransientFile(fd);
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, buf, st.st_size - sizeof(crc));
	FIN_CRC32C(crc);
	if (memcmp(&crc, buf + st.st_size - sizeof(crc), sizeof(crc)) != 0)
		goto done;
	fr.p = buf;
	fr.end = buf + st.st_size - sizeof(crc);
	/* (how many copies: last, before the CRC) */
	if ((Size) (fr.end - fr.p) < sizeof(count))
		goto done;
	fr.end -= sizeof(count);
	memcpy(&count, fr.end, sizeof(count));
	if (!take(&fr, magic, 8) || memcmp(magic, FILE_MAGIC, 8) != 0 ||
		!take(&fr, &version, 4) || version != FILE_VERSION ||
		!take(&fr, &flen, 1) || flen != format_len ||
		(Size) (fr.end - fr.p) < flen || memcmp(fr.p, format, flen) != 0)
		goto done;
	fr.p += flen;
	if (!take(&fr, &hlen, 1) || hlen > JINDEX_MAX_HASH ||
		!take(&fr, &root.len, 1) || root.len > JINDEX_MAX_HASH ||
		!take(&fr, root.bytes, root.len) ||
		!take(&fr, &root_segment, 8) || !take(&fr, &root_offset, 8) ||
		!take(&fr, &nsegs, 4) || nsegs > (uint32) (fr.end - fr.p) / 16)
		goto done;
	seg_ids = palloc(sizeof(uint64) * Max(nsegs, 1));
	seg_pos = palloc(sizeof(uint64) * Max(nsegs, 1));
	for (uint32 i = 0; i < nsegs; i++)
	{
		if (!take(&fr, &seg_ids[i], 8) || !take(&fr, &seg_pos[i], 8))
			goto done;
	}
	if ((hlen > 0 && count > (uint64) (fr.end - fr.p) / (hlen + 24)) ||
		(Size) (fr.end - fr.p) != count * (hlen + 24))
		goto done;

	jindex_lock(ix, true);
	/* (only into an index nothing has read into yet) */
	if (db->scan.nsegments == 0 && db->scan.manifests_seen == 0)
	{
		const char *p;
		JournalScan *js = &db->scan;
		bool		root_named = root_segment == 0 && root_offset == 0;

		jindex_set_segments(ix, ids, n, start);
		for (int i = 0; i < n; i++)
		{
			JournalSegScan *g = jindex_segment(ix, i);

			/*
			 * (not past the segment's end now: a file saved before the
			 * segment's bytes were synced, which a crash then lost, says
			 * nothing of it, read again whole, its copies left out)
			 */
			for (uint32 j = 0; j < nsegs; j++)
			{
				if (seg_ids[j] == ids[i] && seg_pos[j] >= start &&
					seg_pos[j] <= sizes[i])
					g->scanned = seg_pos[j];
			}
			root_named |= ids[i] == root_segment;
		}
		for (p = fr.p; p < fr.end; p += hlen + 24)
		{
			ProllyHash	h;
			uint64		segment;
			uint64		offset;
			uint32		len;
			TransactionId xid;
			bool		named = false;

			memset(&h, 0, sizeof(h));
			h.len = hlen;
			memcpy(h.bytes, p, hlen);
			memcpy(&segment, p + hlen, 8);
			memcpy(&offset, p + hlen + 8, 8);
			memcpy(&len, p + hlen + 16, 4);
			memcpy(&xid, p + hlen + 20, 4);
			for (int i = 0; i < n && !named; i++)
				named = ids[i] == segment &&
					offset + len <= jindex_segment(ix, i)->scanned;
			if (named)
				jindex_add(ix, &h, segment, offset, len, xid);
		}
		if (root_named && root.len > 0)
		{
			js->root = root;
			js->root_segment = root_segment;
			js->root_offset = root_offset;
		}
		loaded = true;
	}
	jindex_unlock(ix);

done:
	if (buf)
		pfree(buf);
	if (seg_ids)
		pfree(seg_ids);
	if (seg_pos)
		pfree(seg_pos);
	pfree(path);
	pfree(dir);
	return loaded;
}
