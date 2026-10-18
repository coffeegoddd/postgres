/*-------------------------------------------------------------------------
 *
 * chunkstore_journal.c
 *	  Chunk store: an append-only journal per database.
 *
 * A database's chunks are records in its journal, in its directory in its
 * default tablespace: in segments, files of a header, then records
 *
 *		uint32 len (of the whole record), uint8 kind, payload,
 *		uint32 CRC-32C of kind and payload
 *
 *		CHUNK	uint8 hash length, the hash, the chunk's bytes
 *		XCHUNK	uint32 xid, then as CHUNK: a chunk of the transaction
 *				xid, which others don't see while it is in progress
 *		ROOT	uint8 hash length, the hash: the store's root from here on
 *		PAD		room for a ROOT record, which its transaction's commit
 *				writes over it (a PAD left means the transaction aborted);
 *				its payload starts with its transaction's xid
 *
 * Segments.  The manifest (below) names the journal's segments, oldest
 * first: prolly_journal (segment 0, all of a journal before segments
 * were), then prolly_journal_<id> (id in hex, from the ids table files are
 * given); a store without a manifest has segment 0 alone.  Appends go to
 * the last.  seal() makes a new one, its header written, and a manifest
 * naming it, both under ProllyJournal, so every append after goes to it: a
 * backend reads the segments again, if a manifest was written since, before
 * it appends.  Every segment named is read; a backend notices a manifest
 * that drops one (a collection's) at its next lookup, and forgets the
 * segment's records.
 *
 * Appending.  A backend gathers its transaction's chunks and appends them
 * in batches, and the rest before it commits, under ProllyJournal: a
 * batch's bytes are WAL-logged, the log is flushed, and only then are they
 * written at the segment's end.  So a segment never holds a byte the WAL
 * doesn't, and redo rewrites whatever a crash lost.  A checkpoint waits for
 * an append in progress (DELAY_CHKPT_START) and fsyncs the segments.  A
 * batch is checked first: every chunk its chunks name is in the batch or
 * the store (as of a manifest no other replaced before the append), or the
 * transaction fails with a serialization failure, so the store never has a
 * chunk without the chunks it names.
 *
 * Reading.  Each backend indexes the records by hash in memory, scanning
 * the file from where it left off when a lookup misses or the root is
 * asked for.  A record not completely written yet ends the scan, until the
 * next one.  A chunk appended before its transaction commits (a batch, or
 * the rest at pre-commit) is an XCHUNK, and isn't found by others while the
 * transaction is in progress: a chunk is others' once its transaction has
 * committed (or aborted: its bytes are whole, and anyone may use them).
 * Another transaction putting the same chunk meanwhile appends its own
 * copy; the index keeps every copy of a chunk until one is known to be
 * everyone's.
 *
 * The root.  A move locks the store until its transaction ends, so moves
 * are serialized, and they reach the file in commit order: at pre-commit a
 * PAD is appended for the new ROOT record; the commit record carries its
 * segment and offset and the root; once the commit record is flushed,
 * still within the commit's critical section, the ROOT record is written
 * over the PAD (redo of the commit record writes it too), in a segment
 * sealed meanwhile as in any.  The root is the last ROOT record of the
 * segments named, in their order; if they have none, the manifest's.
 *
 * Table files.  Chunks added together (received from another store, or
 * rewritten by conjoin()) go to an immutable table file of their own,
 * prolly_table_<id> (id in hex), in the table file format.  The manifest,
 * prolly_manifest, names the table files (each settled or not: closed
 * under what its chunks name), the segments, the files and segments
 * retiring (no longer named, kept until whoever may read them has ended),
 * the last id given, the root as of its writing and the GC generation; it
 * is replaced whole (written aside, then renamed over the old one).  Every
 * byte of a table file is WAL-logged before it is written, as journal
 * appends are; the file is synced before the manifest naming it is logged;
 * a manifest's record carries its contents, and an unlink's names the
 * file.  One writer of table files (and sealer of segments) at a time (a
 * lock on the manifest, until its transaction ends); a file or segment
 * whose writer failed before its manifest stays unnamed, and the next
 * writer removes it.  Backends notice a new manifest by a counter in shared
 * memory, when a lookup misses.
 *
 * Collection (gc_*, for a collector).  One at a time per database (a lock
 * of its own, to the transaction's end), holding the manifest's lock from
 * its seal to its swap.  It seals the journal (a SEAL manifest record),
 * and numbers the chunks of the sealed segments and of the table files it
 * collects (all, or those not settled) as slots: a sealed segment's by a
 * hash table of them (what changed since the last collection), a table
 * file's by its index.  Transactions that appended to a sealed segment and
 * were in progress at the seal are waited for before anything new is read:
 * then their PADs are ROOTs or not, for good, and their chunks committed or
 * not.  The marked chunks are copied, each re-hashed, to a new settled
 * table file before the window; the window takes ProllyJournal, so no
 * append runs, while the last new chunks are marked and copied and the
 * manifest swapped (a SWAP record): the new files and those kept, the
 * segment appends go to, what was collected retiring, the next generation.
 * The manifest's lock is let go there: a transfer older than the swap is
 * refused, not left to wait.  A list of the chunks the swap drops (those
 * unmarked before the window) is written beside, not logged: a transaction
 * that began before the swap checks the hashes its chunks hold against it
 * when it appends.  Once every transaction running at the swap has ended,
 * what it stopped naming is removed (a RETIRE record, whose replay cancels
 * the standby's queries running when the swap was replayed).  A crash
 * before the swap leaves the new file unnamed, removed by the next writer;
 * one after leaves files retiring, removed by the next collection once
 * nobody who may read them is left.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/chunkstore_journal.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "access/prolly_chunkstore.h"
#include "access/prolly_hash.h"
#include "access/prolly_shmem.h"
#include "access/prolly_tablefile.h"
#include "access/prolly_xlog.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "common/relpath.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "port/pg_crc32c.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/standby.h"
#include "utils/hsearch.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

#define JOURNAL_FILE		"prolly_journal"	/* segment 0's */
#define SEGMENT_PREFIX		"prolly_journal_"	/* the others', id in hex */
#define MANIFEST_FILE		"prolly_manifest"
#define MANIFEST_TEMP		"prolly_manifest.tmp"
#define MANIFEST_MAGIC		"PRLYMAN1"
#define MANIFEST_VERSION	2
#define MANIFEST_V1			1	/* no segments (segment 0 alone), no flags */
#define TABLE_FILE_FORMAT	"basic"
#define JOURNAL_MAGIC		"PRLYJRN1"
#define JOURNAL_VERSION		1
#define JOURNAL_HEADER_SIZE	16	/* magic, version, reserved */

#define KIND_CHUNK			1
#define KIND_ROOT			2
#define KIND_PAD			3
#define KIND_XCHUNK			4

/* len, kind ... crc */
#define RECORD_OVERHEAD		(sizeof(uint32) + 1 + sizeof(uint32))

/* a backend appends its chunks in batches of about this much */
#define APPEND_BATCH		(1024 * 1024)

/* the root lock: an object of the database no catalog has */
#define ROOT_LOCK_CLASSID	((Oid) 0x504A5254)
/* the lock of table file writers, likewise */
#define MANIFEST_LOCK_CLASSID	((Oid) 0x504A4D46)
/* a collection's, likewise: one at a time, from its seal to its retire */
#define GC_LOCK_CLASSID		((Oid) 0x504A4743)

/* WAL */
#define XLOG_CHUNKS_APPEND	0x10	/* bytes written at an offset */

typedef struct xl_chunks_append
{
	xl_prolly_header hdr;
	Oid			dboid;
	Oid			spcoid;
	uint64		segment;		/* the journal segment's id */
	uint64		offset;
	/* the bytes follow */
} xl_chunks_append;

#define SizeOfChunksAppend	(offsetof(xl_chunks_append, offset) + sizeof(uint64))

#define XLOG_CHUNKS_FILE		0x20	/* table file bytes written at an offset */
#define XLOG_CHUNKS_MANIFEST	0x30	/* the manifest replaced */
#define XLOG_CHUNKS_UNLINK		0x40	/* a table file or segment removed */

/*
 * A collection's manifests: a segment sealed, the files swapped, and those
 * retired (removed, as the manifest before named them retiring)
 */
#define XLOG_CHUNKS_SEAL		0x50
#define XLOG_CHUNKS_SWAP		0x60
#define XLOG_CHUNKS_RETIRE		0x70

/* XLOG_CHUNKS_UNLINK's offset: what it removes */
#define UNLINK_TABLE_FILE		0
#define UNLINK_SEGMENT			1

typedef struct xl_chunks_file
{
	xl_prolly_header hdr;
	Oid			dboid;
	Oid			spcoid;
	uint64		id;				/* the table file's */
	uint64		offset;			/* XLOG_CHUNKS_FILE: the bytes follow */
} xl_chunks_file;

#define SizeOfChunksFile	(offsetof(xl_chunks_file, offset) + sizeof(uint64))

/* XLOG_CHUNKS_MANIFEST: the manifest's contents follow */
#define SizeOfChunksManifest	(offsetof(xl_chunks_file, id))

/* a table file's flags in the manifest */
#define MANIFEST_FILE_SETTLED	0x01	/* closed under what its chunks name */

/* the manifest: the table files, the journal's segments, and the ids given */
typedef struct ManifestFile
{
	uint64		id;
	uint32		nchunks;
	uint64		size;
	uint8		flags;
} ManifestFile;

typedef struct Manifest
{
	uint64		last_id;		/* table file and segment ids given so far */
	uint32		gc_generation;
	ProllyHash	root;			/* as of the manifest's writing: the root
								 * while no segment it names has a ROOT */
	int			nfiles;
	ManifestFile *files;
	int			nsegments;		/* at least one */
	uint64	   *segments;		/* oldest first; appends go to the last */
	int			nretiring;
	uint64	   *retiring;		/* table files and segments no longer named,
								 * kept until those who may read them end */
} Manifest;

/* a table file a backend reads */
typedef struct TableFile
{
	uint64		id;
	char	   *path;
	File		file;
	uint64		size;
	bool		settled;
	ProllyTableFile *tf;
} TableFile;

/* shared by every backend */
typedef struct JournalShared
{
	pg_atomic_uint64 manifests;	/* manifests written or replayed */
} JournalShared;

static JournalShared *journal_shared = NULL;

/* what a root move's commit record carries */
typedef struct CommitPayload
{
	Oid			dboid;
	Oid			spcoid;
	uint64		segment;		/* the PAD's */
	uint64		offset;			/* of the PAD the ROOT record goes over */
	ProllyHash	root;
} CommitPayload;

/* an index entry: where a chunk's record is */
typedef struct IndexEntry
{
	ProllyHash	hash;			/* key; unused bytes zero */
	uint64		segment;		/* the record's journal segment */
	uint64		offset;			/* of the record, or in pending */
	uint32		len;			/* of the record */
	TransactionId xid;			/* its transaction, while not known to have
								 * ended; invalid: everyone's */
	struct IndexEntry *other;	/* another copy, while this one isn't */
} IndexEntry;

/* a journal segment a backend reads, and (the last) appends to */
typedef struct Segment
{
	uint64		id;
	char	   *path;
	File		file;
	uint64		scanned;		/* indexed up to here */
	List	   *pads;			/* offsets of PADs seen, which may become ROOTs */
} Segment;

struct ProllyChunkStore
{
	Oid			dboid;
	Oid			spcoid;

	List	   *segments;		/* Segment *, as the manifest last read names */
	HTAB	   *index;			/* chunks in the segments, by hash */
	ProllyHash	root;
	uint64		root_segment;	/* where the last ROOT record seen is; 0, 0: */
	uint64		root_offset;	/* the manifest's root, or none */

	List	   *tables;			/* TableFile *, as the manifest last read named */
	uint64		manifests_seen; /* the shared count when it was read */
	uint64		segments_seen;	/* likewise, for the segments */
	uint32		gc_generation;	/* the manifest's, as last read */

	/* the GC generation the transaction (lxid) began in */
	LocalTransactionId xact_lxid;
	uint32		xact_generation;
	ProllyChunkGc *gc;			/* a collection the transaction runs */

	/* chunks the store hasn't, read from elsewhere (read_through()) */
	ProllyChunkGet through;
	ProllyChunkHint through_hint;
	void	   *through_arg;

	/* this transaction's */
	StringInfoData pending;		/* records not appended yet */
	HTAB	   *pending_index;
	bool		moving;
	ProllyHash	new_root;
	List	   *saved;			/* MoveSave *, innermost last */
	List	   *pending_saved;	/* PendingSave *, innermost last */
	uint64		flushes;		/* times pending was appended */
	Segment    *pad_segment;
	uint64		pad_offset;		/* 0: no PAD yet */
	/* the ROOT record, made before the commit's critical section */
	char		root_rec[RECORD_OVERHEAD + 1 + PROLLY_HASH_MAX_LEN];
	uint32		root_rec_len;
};

/*
 * The move as it was before a subtransaction (at nesting level 'level')
 * first moved the root, to go back to if it aborts.
 */
typedef struct MoveSave
{
	int			level;
	bool		moving;
	ProllyHash	new_root;
} MoveSave;

/*
 * The pending chunks as they were before a subtransaction (at nesting
 * level 'level') first put one, to go back to if it aborts: pending's
 * length then, unless it was appended since (flushes moved), when all of
 * pending is the subtransaction's.
 */
typedef struct PendingSave
{
	int			level;
	uint64		flushes;
	int			len;
} PendingSave;

extern const ProllyChunkStoreRoutine prolly_chunkstore_journal;

static ProllyChunkStore *session_store = NULL;

static bool ensure_journal(ProllyChunkStore *s, bool create);
static void journal_root(ProllyChunkStore *s, ProllyHash *root);
static void retire_files(Oid dboid, Oid spcoid, const Manifest *before,
						 const Manifest *after);
static void standby_readers_note(Oid dboid);
static void standby_readers_resolve(Oid dboid);
static void check_dropped(ProllyChunkStore *s);
static void gc_abort(ProllyChunkGc *gc);

/* ----------------------------------------------------------------
 * Records
 * ----------------------------------------------------------------
 */

static void
hash_key(ProllyHash *key, const ProllyHash *hash)
{
	memset(key, 0, sizeof(ProllyHash));
	key->len = hash->len;
	memcpy(key->bytes, hash->bytes, hash->len);
}

/* Append a record of kind with payload (head, then tail) to buf. */
static void
record_append(StringInfo buf, uint8 kind, const void *head, Size headlen,
			  const void *tail, Size taillen)
{
	uint32		len = RECORD_OVERHEAD + headlen + taillen;
	pg_crc32c	crc;

	INIT_CRC32C(crc);
	COMP_CRC32C(crc, &kind, 1);
	if (headlen > 0)
		COMP_CRC32C(crc, head, headlen);
	if (taillen > 0)
		COMP_CRC32C(crc, tail, taillen);
	FIN_CRC32C(crc);

	appendBinaryStringInfo(buf, &len, sizeof(len));
	appendBinaryStringInfo(buf, &kind, 1);
	if (headlen > 0)
		appendBinaryStringInfo(buf, head, headlen);
	if (taillen > 0)
		appendBinaryStringInfo(buf, tail, taillen);
	appendBinaryStringInfo(buf, &crc, sizeof(crc));
}

/* A hash as a record's payload starts: its length, then its bytes. */
static void
hash_payload(const ProllyHash *hash, uint8 *buf, Size *len)
{
	buf[0] = hash->len;
	memcpy(buf + 1, hash->bytes, hash->len);
	*len = 1 + hash->len;
}

/*
 * A ROOT record, in buf; or a PAD as long as one, which names its
 * transaction in its payload's first bytes (the rest zero).
 */
static void
root_record(StringInfo buf, const ProllyHash *root, bool pad)
{
	uint8		payload[1 + PROLLY_HASH_MAX_LEN];
	Size		len;

	hash_payload(root, payload, &len);
	if (pad)
	{
		TransactionId xid = GetTopTransactionIdIfAny();

		memset(payload, 0, len);
		memcpy(payload, &xid, Min(len, sizeof(xid)));
	}
	record_append(buf, pad ? KIND_PAD : KIND_ROOT, payload, len, NULL, 0);
}

/* Is the record at rec (len bytes) whole and uncorrupted? */
static bool
record_valid(const char *rec, uint32 len)
{
	pg_crc32c	crc;
	pg_crc32c	stored;

	if (len < RECORD_OVERHEAD)
		return false;
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, rec + sizeof(uint32), len - sizeof(uint32) - sizeof(uint32));
	FIN_CRC32C(crc);
	memcpy(&stored, rec + len - sizeof(uint32), sizeof(uint32));
	return EQ_CRC32C(crc, stored);
}

/*
 * The hash a CHUNK, XCHUNK or ROOT record's payload has (after an XCHUNK's
 * xid, which *xid gets; InvalidTransactionId for the others).
 */
static bool
record_hash_xid(const char *rec, uint32 len, ProllyHash *hash, Size *after,
				TransactionId *xid)
{
	Size		skip = 0;
	const char *payload;
	uint8		hlen;

	*xid = InvalidTransactionId;
	if (len > sizeof(uint32) && (uint8) rec[sizeof(uint32)] == KIND_XCHUNK)
	{
		if (len < RECORD_OVERHEAD + sizeof(TransactionId))
			return false;
		memcpy(xid, rec + sizeof(uint32) + 1, sizeof(TransactionId));
		skip = sizeof(TransactionId);
	}
	payload = rec + sizeof(uint32) + 1 + skip;
	hlen = (uint8) payload[0];
	if (hlen == 0 || hlen > PROLLY_HASH_MAX_LEN ||
		sizeof(uint32) + 1 + skip + 1 + hlen + sizeof(uint32) > len)
		return false;
	memset(hash, 0, sizeof(ProllyHash));
	hash->len = hlen;
	memcpy(hash->bytes, payload + 1, hlen);
	*after = sizeof(uint32) + 1 + skip + 1 + hlen;
	return true;
}

static bool
record_hash(const char *rec, uint32 len, ProllyHash *hash, Size *after)
{
	TransactionId xid;

	return record_hash_xid(rec, len, hash, after, &xid);
}

/*
 * Index a copy of a chunk: the first, or another while those before it
 * aren't known to be everyone's.
 */
static void
index_add(HTAB *index, const ProllyHash *key, uint64 segment, uint64 offset,
		  uint32 len, TransactionId xid)
{
	bool		found;
	IndexEntry *e = hash_search(index, key, HASH_ENTER, &found);
	IndexEntry *last;

	if (!found)
	{
		e->segment = segment;
		e->offset = offset;
		e->len = len;
		e->xid = xid;
		e->other = NULL;
		return;
	}
	for (last = e; last != NULL; last = last->other)
	{
		if (!TransactionIdIsValid(last->xid))
			return;				/* one is everyone's already */
		if (last->other == NULL)
			break;
	}
	last->other = MemoryContextAlloc(TopMemoryContext, sizeof(IndexEntry));
	*last->other = *e;
	last->other->segment = segment;
	last->other->offset = offset;
	last->other->len = len;
	last->other->xid = xid;
	last->other->other = NULL;
}

/*
 * May this backend read a copy?  Not one of a transaction in progress,
 * unless it is its own; with wait, one whose transaction is in progress is
 * waited for (a reader following a root may meet one being committed).
 */
static bool
copy_visible(IndexEntry *e, bool wait)
{
	if (!TransactionIdIsValid(e->xid))
		return true;
	if (TransactionIdIsCurrentTransactionId(e->xid))
		return true;
	for (;;)
	{
		if (!TransactionIdIsInProgress(e->xid) || TransactionIdDidCommit(e->xid))
		{
			e->xid = InvalidTransactionId;	/* everyone's from now on */
			return true;
		}
		if (!wait)
			return false;
		CHECK_FOR_INTERRUPTS();
		pg_usleep(1000L);
	}
}

/* A copy of a chunk this backend may read, waiting only if it must. */
static IndexEntry *
index_find(HTAB *index, const ProllyHash *key)
{
	IndexEntry *first = hash_search(index, key, HASH_FIND, NULL);
	IndexEntry *e;

	for (e = first; e != NULL; e = e->other)
		if (copy_visible(e, false))
			return e;
	for (e = first; e != NULL; e = e->other)
		if (copy_visible(e, true))
			return e;
	return NULL;
}

/* Is there a copy this backend may read now?  Never waits. */
static bool
index_has(HTAB *index, const ProllyHash *key)
{
	IndexEntry *e;

	for (e = hash_search(index, key, HASH_FIND, NULL); e != NULL; e = e->other)
		if (copy_visible(e, false))
			return true;
	return false;
}

/* ----------------------------------------------------------------
 * The segments
 * ----------------------------------------------------------------
 */

static char *
segment_path(Oid dboid, Oid spcoid, uint64 id)
{
	char	   *dir = GetDatabasePath(dboid, spcoid);
	char	   *path;

	if (id == 0)
		path = psprintf("%s/%s", dir, JOURNAL_FILE);
	else
		path = psprintf("%s/%s%016llx", dir, SEGMENT_PREFIX,
						(unsigned long long) id);
	pfree(dir);
	return path;
}

static Segment *
segment_of(ProllyChunkStore *s, uint64 id)
{
	ListCell   *lc;

	foreach(lc, s->segments)
	{
		if (((Segment *) lfirst(lc))->id == id)
			return lfirst(lc);
	}
	return NULL;
}

/* the segment appends go to */
static Segment *
current_segment(ProllyChunkStore *s)
{
	return llast(s->segments);
}

static void
read_exact(Segment *g, char *buf, Size len, uint64 offset)
{
	ssize_t		n = FileRead(g->file, buf, len, offset, WAIT_EVENT_DATA_FILE_READ);

	if (n != len)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read %zu bytes at %llu in file \"%s\": %m",
						len, (unsigned long long) offset, g->path)));
}

/* Open a segment's file; false if it has none yet. */
static bool
open_segment(Segment *g)
{
	if (g->file >= 0)
		return true;
	g->file = PathNameOpenFile(g->path, O_RDWR | PG_BINARY);
	if (g->file < 0)
	{
		if (errno == ENOENT)
			return false;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", g->path)));
	}
	return true;
}

/*
 * WAL-log bytes written to a segment at offset, flush the log, and only
 * then write them (to file, or to fd if file is -1): so the segment never
 * holds what the WAL doesn't.  A checkpoint waits meanwhile
 * (DELAY_CHKPT_START), so one whose redo point follows the record finds
 * the bytes written.
 */
static void
logged_write(Oid dboid, Oid spcoid, uint64 segment, uint64 offset,
			 const char *data, Size len, File file, int fd, const char *path)
{
	xl_chunks_append xlrec;
	XLogRecPtr	lsn;
	ssize_t		n;

	xlrec.hdr.impl_id = prolly_chunkstore_journal.info.impl_id;
	xlrec.dboid = dboid;
	xlrec.spcoid = spcoid;
	xlrec.segment = segment;
	xlrec.offset = offset;

	Assert((MyProc->delayChkptFlags & DELAY_CHKPT_START) == 0);
	START_CRIT_SECTION();
	MyProc->delayChkptFlags |= DELAY_CHKPT_START;
	XLogBeginInsert();
	XLogRegisterData(&xlrec, SizeOfChunksAppend);
	XLogRegisterData(data, len);
	lsn = XLogInsert(RM_PROLLY_CHUNKS_ID, XLOG_CHUNKS_APPEND);
	XLogFlush(lsn);
	if (file >= 0)
		n = FileWrite(file, data, len, offset, WAIT_EVENT_DATA_FILE_WRITE);
	else
	{
		pgstat_report_wait_start(WAIT_EVENT_DATA_FILE_WRITE);
		n = pg_pwrite(fd, data, len, offset);
		pgstat_report_wait_end();
	}
	if (n != len)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not write %zu bytes at %llu in file \"%s\": %m",
						len, (unsigned long long) offset, path)));
	MyProc->delayChkptFlags &= ~DELAY_CHKPT_START;
	END_CRIT_SECTION();
}

/*
 * Give segment id its header, if it has none, logged as any append.  The
 * caller holds ProllyJournalLock.
 */
static void
segment_header(Oid dboid, Oid spcoid, uint64 id)
{
	char	   *path = segment_path(dboid, spcoid, id);
	int			fd = OpenTransientFile(path, O_RDWR | O_CREAT | PG_BINARY);
	struct stat st;
	char		header[JOURNAL_HEADER_SIZE];
	uint32		version = JOURNAL_VERSION;

	if (fd < 0 || fstat(fd, &st) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
	if (st.st_size == 0)
	{
		memset(header, 0, sizeof(header));
		memcpy(header, JOURNAL_MAGIC, 8);
		memcpy(header + 8, &version, sizeof(version));
		logged_write(dboid, spcoid, id, 0, header, sizeof(header), -1, fd, path);
	}
	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));
	pfree(path);
}

static void refresh_segments(ProllyChunkStore *s);
static uint64 manifests_count(void);

/*
 * Append bytes at the end of the segment appends go to; returns the offset
 * they went to, and the segment (*seg).  The segments are read again first
 * if a manifest was written since (a seal takes the same lock), so nothing
 * is appended to a segment that was sealed: before the lock, and under it
 * only if another manifest came meanwhile, so the lock is seldom held
 * while a manifest is read.  With checked, the bytes'
 * chunks were checked against the store as of that manifest count: if a
 * manifest was written since, nothing is appended and false returned (the
 * caller checks them again).
 */
static bool
append_bytes(ProllyChunkStore *s, const char *data, Size len, uint64 checked,
			 Segment **seg, uint64 *at)
{
	Segment    *g;
	pgoff_t		size;

	refresh_segments(s);
	LWLockAcquire(ProllyJournalLock, LW_EXCLUSIVE);
	if (checked != 0 && manifests_count() != checked)
	{
		LWLockRelease(ProllyJournalLock);
		return false;
	}
	refresh_segments(s);
	g = current_segment(s);
	if (!open_segment(g) || (size = FileSize(g->file)) < JOURNAL_HEADER_SIZE)
		elog(ERROR, "chunk journal \"%s\" has no header", g->path);
	logged_write(s->dboid, s->spcoid, g->id, size, data, len, g->file, -1,
				 g->path);
	LWLockRelease(ProllyJournalLock);
	*seg = g;
	*at = size;
	return true;
}

/* Write bytes at offset, in redo or over a PAD; the WAL has them. */
static void
write_at(const char *path, const char *data, Size len, uint64 offset)
{
	int			fd = OpenTransientFile(path, O_RDWR | O_CREAT | PG_BINARY);

	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
	pgstat_report_wait_start(WAIT_EVENT_DATA_FILE_WRITE);
	if (pg_pwrite(fd, data, len, offset) != len)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write %zu bytes at %llu in file \"%s\": %m",
						len, (unsigned long long) offset, path)));
	pgstat_report_wait_end();
	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));
}

/* Is a ROOT record at (segment, offset) later than the root's? */
static bool
root_later(ProllyChunkStore *s, uint64 segment, uint64 offset)
{
	return segment > s->root_segment ||
		(segment == s->root_segment && offset >= s->root_offset);
}

/*
 * Index a segment's records written since its last scan: up to the first
 * that isn't whole yet.  A PAD seen may since have become a ROOT.
 */
static void
scan_segment(ProllyChunkStore *s, Segment *g)
{
	uint64		size;
	uint64		offset = g->scanned;
	ListCell   *lc;
	List	   *still_pads = NIL;
	char	   *rec = NULL;
	Size		reclen = 0;

	if (!open_segment(g))
		return;
	size = FileSize(g->file);

	foreach(lc, g->pads)
	{
		uint64		pad = *(uint64 *) lfirst(lc);
		uint8		kind;

		read_exact(g, (char *) &kind, 1, pad + sizeof(uint32));
		if (kind == KIND_ROOT)
		{
			uint32		len;
			char		buf[RECORD_OVERHEAD + 1 + PROLLY_HASH_MAX_LEN];
			ProllyHash	h;
			Size		after;

			read_exact(g, (char *) &len, sizeof(len), pad);
			if (len <= sizeof(buf))
			{
				read_exact(g, buf, len, pad);
				if (record_valid(buf, len) && record_hash(buf, len, &h, &after) &&
					root_later(s, g->id, pad))
				{
					s->root = h;
					s->root_segment = g->id;
					s->root_offset = pad;
				}
			}
			pfree(lfirst(lc));
		}
		else
			still_pads = lappend(still_pads, lfirst(lc));
	}
	list_free(g->pads);
	g->pads = still_pads;

	while (offset + RECORD_OVERHEAD <= size)
	{
		uint32		len;
		uint8		kind;
		ProllyHash	h;
		Size		after;
		TransactionId xid;

		read_exact(g, (char *) &len, sizeof(len), offset);
		if (len < RECORD_OVERHEAD || offset + len > size)
			break;
		if (len > reclen)
		{
			if (rec)
				pfree(rec);
			reclen = Max(len, 8192);
			rec = MemoryContextAlloc(TopMemoryContext, reclen);
		}
		read_exact(g, rec, len, offset);
		if (!record_valid(rec, len))
			break;
		kind = (uint8) rec[sizeof(uint32)];
		if ((kind == KIND_CHUNK || kind == KIND_XCHUNK) &&
			record_hash_xid(rec, len, &h, &after, &xid))
		{
			ProllyHash	key;

			hash_key(&key, &h);
			index_add(s->index, &key, g->id, offset, len, xid);
		}
		else if (kind == KIND_ROOT && record_hash(rec, len, &h, &after))
		{
			if (root_later(s, g->id, offset))
			{
				s->root = h;
				s->root_segment = g->id;
				s->root_offset = offset;
			}
		}
		else if (kind == KIND_PAD)
		{
			uint64	   *pad = MemoryContextAlloc(TopMemoryContext, sizeof(uint64));
			MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

			*pad = offset;
			g->pads = lappend(g->pads, pad);
			MemoryContextSwitchTo(old);
		}
		offset += len;
	}
	if (rec)
		pfree(rec);
	g->scanned = offset;
}

/* Index what was written to the segments since they were last scanned. */
static void
scan_journal(ProllyChunkStore *s)
{
	ListCell   *lc;

	refresh_segments(s);
	foreach(lc, s->segments)
		scan_segment(s, lfirst(lc));
}

/* Forget the index's copies in a segment no longer named. */
static void
index_drop_segment(HTAB *index, uint64 segment)
{
	HASH_SEQ_STATUS status;
	IndexEntry *e;

	hash_seq_init(&status, index);
	while ((e = hash_seq_search(&status)) != NULL)
	{
		IndexEntry **link = &e->other;

		while (*link != NULL)
		{
			if ((*link)->segment == segment)
			{
				IndexEntry *gone = *link;

				*link = gone->other;
				pfree(gone);
			}
			else
				link = &(*link)->other;
		}
		if (e->segment != segment)
			continue;
		if (e->other == NULL)
			hash_search(index, &e->hash, HASH_REMOVE, NULL);
		else
		{
			IndexEntry *next = e->other;

			e->segment = next->segment;
			e->offset = next->offset;
			e->len = next->len;
			e->xid = next->xid;
			e->other = next->other;
			pfree(next);
		}
	}
}

/*
 * Take the segments a manifest names, in its order: those read before as
 * they are, new ones to be read from their start.  The index forgets the
 * records of those no longer named (a collection retired them), and a root
 * read from one gives way to the manifest's.
 */
static void
load_segments(ProllyChunkStore *s, const Manifest *m)
{
	List	   *segs = NIL;
	List	   *dropped = NIL;
	ListCell   *lc;
	MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

	for (int i = 0; i < m->nsegments; i++)
	{
		Segment    *g = segment_of(s, m->segments[i]);

		if (g == NULL)
		{
			g = palloc0(sizeof(Segment));
			g->id = m->segments[i];
			g->path = segment_path(s->dboid, s->spcoid, g->id);
			g->file = -1;
			g->scanned = JOURNAL_HEADER_SIZE;
		}
		segs = lappend(segs, g);
	}
	foreach(lc, s->segments)
	{
		if (!list_member_ptr(segs, lfirst(lc)))
			dropped = lappend(dropped, lfirst(lc));
	}
	MemoryContextSwitchTo(old);
	list_free(s->segments);
	s->segments = segs;

	s->gc_generation = m->gc_generation;
	if (s->root_offset == 0 || segment_of(s, s->root_segment) == NULL)
	{
		s->root = m->root;
		s->root_segment = 0;
		s->root_offset = 0;
	}
	foreach(lc, dropped)
	{
		Segment    *g = lfirst(lc);

		index_drop_segment(s->index, g->id);
		if (g->file >= 0)
			FileClose(g->file);
		list_free_deep(g->pads);
		pfree(g->path);
		pfree(g);
	}
	list_free(dropped);
}

/* ----------------------------------------------------------------
 * Transactions
 * ----------------------------------------------------------------
 */

static bool tables_get(ProllyChunkStore *s, const ProllyHash *hash,
					   StringInfo out);
static void load_tables(ProllyChunkStore *s, bool force);

/*
 * Is the chunk (its key) in the store's files?  Without reading through:
 * what the store itself holds.
 */
static bool
stored_has(ProllyChunkStore *s, const ProllyHash *key)
{
	if (index_has(s->index, key) || tables_get(s, key, NULL))
		return true;
	scan_journal(s);
	if (index_has(s->index, key))
		return true;
	load_tables(s, false);
	return tables_get(s, key, NULL);
}

/*
 * Note the GC generation the transaction began in, at its first use of the
 * store: it holds chunks no swap since may have dropped.
 */
static void
xact_note(ProllyChunkStore *s)
{
	if (MyProc->vxid.lxid == s->xact_lxid)
		return;
	refresh_segments(s);
	s->xact_lxid = MyProc->vxid.lxid;
	s->xact_generation = s->gc_generation;
}

/* Has a swap happened since the transaction began? */
static bool
xact_straddles(ProllyChunkStore *s)
{
	return MyProc->vxid.lxid == s->xact_lxid &&
		s->gc_generation != s->xact_generation;
}

/* a pending chunk whose references are being checked */
typedef struct RefCheck
{
	ProllyChunkStore *s;
	const ProllyHash *chunk;
} RefCheck;

/*
 * A chunk a pending one names must be pending too, or the store's: has()
 * without reading through, and without waiting (a chunk another
 * transaction hasn't committed is one this one can't have read).
 */
static void
check_reference(const ProllyHash *ref, void *arg)
{
	RefCheck   *rc = arg;
	ProllyChunkStore *s = rc->s;
	ProllyHash	key;

	hash_key(&key, ref);
	if (hash_search(s->pending_index, &key, HASH_FIND, NULL) != NULL ||
		stored_has(s, &key))
		return;
	ereport(ERROR,
			(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
			 errmsg("could not store chunk %s: chunk %s, which it names, is not in the chunk store",
					prolly_hash_hex(rc->chunk), prolly_hash_hex(ref)),
			 errdetail("A garbage collection may have removed it after the transaction read it."),
			 errhint("Try the transaction again.")));
}

/*
 * Every chunk the pending ones name is pending or in the store: so the
 * store never holds a chunk without what it names.
 */
static void
check_pending(ProllyChunkStore *s)
{
	HASH_SEQ_STATUS status;
	IndexEntry *e;

	hash_seq_init(&status, s->pending_index);
	while ((e = hash_seq_search(&status)) != NULL)
	{
		const char *rec = s->pending.data + e->offset;
		ProllyHash	h;
		Size		after;
		RefCheck	rc;

		if (!record_hash(rec, e->len, &h, &after))
			elog(ERROR, "corrupt pending chunk record");
		rc.s = s;
		rc.chunk = &h;
		prolly_chunk_references(rec + after, e->len - after - sizeof(uint32),
								check_reference, &rc);
	}
}

/* Append the transaction's pending chunks. */
static void
flush_pending(ProllyChunkStore *s)
{
	HASH_SEQ_STATUS status;
	IndexEntry *e;
	Segment    *g;
	uint64		base;

	if (s->pending.len == 0)
		return;
	(void) ensure_journal(s, true);

	/*
	 * Checked against the store as of a manifest, and appended only if no
	 * manifest replaced it meanwhile (a collection's would have removed
	 * chunks).
	 */
	for (;;)
	{
		uint64		count = manifests_count();

		load_tables(s, false);
		check_pending(s);
		if (xact_straddles(s))
			check_dropped(s);
		if (append_bytes(s, s->pending.data, s->pending.len, count, &g, &base))
			break;
	}

	/* the chunks are in the file now */
	hash_seq_init(&status, s->pending_index);
	while ((e = hash_seq_search(&status)) != NULL)
	{
		index_add(s->index, &e->hash, g->id, base + e->offset, e->len, e->xid);
		hash_search(s->pending_index, &e->hash, HASH_REMOVE, NULL);
	}
	resetStringInfo(&s->pending);
	s->flushes++;
}

/* Forget the pending chunks from offset len on. */
static void
pending_truncate(ProllyChunkStore *s, int len)
{
	HASH_SEQ_STATUS status;
	IndexEntry *e;

	hash_seq_init(&status, s->pending_index);
	while ((e = hash_seq_search(&status)) != NULL)
	{
		if (e->offset >= len)
			hash_search(s->pending_index, &e->hash, HASH_REMOVE, NULL);
	}
	s->pending.len = len;
	s->pending.data[len] = '\0';
}

static void
discard_pending(ProllyChunkStore *s)
{
	HASH_SEQ_STATUS status;
	IndexEntry *e;

	hash_seq_init(&status, s->pending_index);
	while ((e = hash_seq_search(&status)) != NULL)
		hash_search(s->pending_index, &e->hash, HASH_REMOVE, NULL);
	resetStringInfo(&s->pending);
	s->moving = false;
	s->pad_offset = 0;
}

static void
journal_xact_callback(XactEvent event, void *arg)
{
	ProllyChunkStore *s = session_store;

	if (s == NULL)
		return;
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PARALLEL_PRE_COMMIT:
			flush_pending(s);
			if (s->moving)
			{
				StringInfoData pad;

				(void) ensure_journal(s, true);
				initStringInfo(&pad);
				root_record(&pad, &s->new_root, true);
				(void) append_bytes(s, pad.data, pad.len, 0, &s->pad_segment,
									&s->pad_offset);
				INJECTION_POINT("prolly-root-padded", NULL);
				resetStringInfo(&pad);
				root_record(&pad, &s->new_root, false);
				Assert(pad.len <= sizeof(s->root_rec));
				memcpy(s->root_rec, pad.data, pad.len);
				s->root_rec_len = pad.len;
				pfree(pad.data);
			}
			break;
		case XACT_EVENT_PRE_PREPARE:
			if (s->moving)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot prepare a transaction that moved the chunk store's root")));
			flush_pending(s);
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_PREPARE:
			/* a root moved was written by committed() */
			s->moving = false;
			s->pad_offset = 0;
			s->saved = NIL;
			s->pending_saved = NIL;
			s->through = NULL;
			s->through_hint = NULL;
			break;
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
			if (s->gc != NULL)
				gc_abort(s->gc);
			discard_pending(s);
			s->saved = NIL;
			s->pending_saved = NIL;
			s->through = NULL;
			s->through_hint = NULL;
			break;
	}
}

/*
 * A subtransaction's pending chunks go with it: an aborted one's are
 * forgotten (one naming a chunk the store lacks would fail its parent's
 * every append), a committed one's are its parent's.  What it appended
 * already passed the check, and may stay, unreachable.
 */
static void
pending_subxact(ProllyChunkStore *s, SubXactEvent event, int level)
{
	PendingSave *last;

	if (s->pending_saved == NIL)
		return;
	last = llast(s->pending_saved);
	if (last->level < level)
		return;
	Assert(last->level == level);
	if (event == SUBXACT_EVENT_ABORT_SUB)
	{
		pending_truncate(s, last->flushes == s->flushes ? last->len : 0);
		s->pending_saved = list_delete_last(s->pending_saved);
	}
	else if (event == SUBXACT_EVENT_COMMIT_SUB)
	{
		/* the parent's saved state, if it has one, is the older */
		if (level - 1 <= 1 ||
			(list_length(s->pending_saved) > 1 &&
			 ((PendingSave *) list_nth(s->pending_saved,
									   list_length(s->pending_saved) - 2))->level == level - 1))
			s->pending_saved = list_delete_last(s->pending_saved);
		else
			last->level = level - 1;
	}
}

/*
 * A subtransaction's root moves go with it: an aborted one's are undone
 * (its lock on the root, if it took it, is released with it); a committed
 * one's are its parent's.  Its chunks too (pending_subxact()).
 */
static void
journal_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
						 SubTransactionId parentSubid, void *arg)
{
	ProllyChunkStore *s = session_store;
	int			level = GetCurrentTransactionNestLevel();
	MoveSave   *last;

	if (s == NULL)
		return;
	pending_subxact(s, event, level);
	if (s->saved == NIL)
		return;
	last = llast(s->saved);
	if (last->level < level)
		return;
	Assert(last->level == level);
	if (event == SUBXACT_EVENT_ABORT_SUB)
	{
		s->moving = last->moving;
		s->new_root = last->new_root;
		s->saved = list_delete_last(s->saved);
	}
	else if (event == SUBXACT_EVENT_COMMIT_SUB)
	{
		/* the parent's saved state, if it has one, is the older */
		if (level - 1 <= 1 ||
			(list_length(s->saved) > 1 &&
			 ((MoveSave *) list_nth(s->saved, list_length(s->saved) - 2))->level == level - 1))
			s->saved = list_delete_last(s->saved);
		else
			last->level = level - 1;
	}
}

/*
 * Open the segment appends go to, if it exists; with create, make it (with
 * its header, logged as any append) if not.  A store with none is empty.
 * In recovery nothing is made: redo makes the segments, and the store reads
 * as it is.
 */
static bool
ensure_journal(ProllyChunkStore *s, bool create)
{
	char		header[JOURNAL_HEADER_SIZE];
	Segment    *g;

	refresh_segments(s);
	g = current_segment(s);
	if (g->file >= 0 && FileSize(g->file) >= JOURNAL_HEADER_SIZE)
		return true;
	if (create && RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("cannot change a chunk store during recovery")));
	if (create)
	{
		LWLockAcquire(ProllyJournalLock, LW_EXCLUSIVE);
		segment_header(s->dboid, s->spcoid, g->id);
		LWLockRelease(ProllyJournalLock);
	}
	if (!open_segment(g) || FileSize(g->file) < JOURNAL_HEADER_SIZE)
		return false;
	read_exact(g, header, sizeof(header), 0);
	if (memcmp(header, JOURNAL_MAGIC, 8) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("file \"%s\" is not a chunk journal", g->path)));
	return true;
}

/* ----------------------------------------------------------------
 * The manifest and table files
 * ----------------------------------------------------------------
 */

static const ProllyTableFileRoutine *
table_format(void)
{
	static const ProllyTableFileRoutine *fmt = NULL;

	if (fmt == NULL)
		fmt = prolly_tablefile_lookup(TABLE_FILE_FORMAT);
	return fmt;
}

static JournalShared *
shared(void)
{
	if (journal_shared == NULL)
		journal_shared = prolly_shmem_area(&prolly_chunkstore_journal.info);
	if (journal_shared == NULL)
		elog(ERROR, "the chunk journal has no shared memory");
	return journal_shared;
}

static char *
dir_file(const char *dir, const char *name)
{
	return psprintf("%s/%s", dir, name);
}

static char *
table_file_name(uint64 id)
{
	return psprintf("prolly_table_%016llx", (unsigned long long) id);
}

static char *
table_file_path(const char *dir, uint64 id)
{
	char	   *name = table_file_name(id);
	char	   *path = dir_file(dir, name);

	pfree(name);
	return path;
}

/* n bytes of v, little-endian, as table files' numbers are */
static void
append_le(StringInfo out, uint64 v, int n)
{
	for (int i = 0; i < n; i++)
		appendStringInfoChar(out, (char) ((v >> (8 * i)) & 0xff));
}

static uint64
read_le(const char *p, int n)
{
	uint64		v = 0;

	for (int i = n - 1; i >= 0; i--)
		v = (v << 8) | (uint8) p[i];
	return v;
}

static void
manifest_bytes(const Manifest *m, StringInfo out)
{
	pg_crc32c	crc;

	appendBinaryStringInfo(out, MANIFEST_MAGIC, 8);
	append_le(out, MANIFEST_VERSION, 4);
	append_le(out, m->gc_generation, 4);
	append_le(out, m->last_id, 8);
	append_le(out, m->root.len, 1);
	appendBinaryStringInfo(out, m->root.bytes, m->root.len);
	append_le(out, m->nfiles, 4);
	for (int i = 0; i < m->nfiles; i++)
	{
		append_le(out, m->files[i].id, 8);
		append_le(out, m->files[i].nchunks, 4);
		append_le(out, m->files[i].size, 8);
		append_le(out, m->files[i].flags, 1);
	}
	append_le(out, m->nsegments, 4);
	for (int i = 0; i < m->nsegments; i++)
		append_le(out, m->segments[i], 8);
	append_le(out, m->nretiring, 4);
	for (int i = 0; i < m->nretiring; i++)
		append_le(out, m->retiring[i], 8);
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, out->data, out->len);
	FIN_CRC32C(crc);
	append_le(out, crc, 4);
}

static bool read_manifest_at(const char *path, Manifest *m);

/*
 * Read the manifest in database directory dir into m; false (m empty, but
 * for segment 0) if there is none.
 */
static bool
read_manifest(const char *dir, Manifest *m)
{
	char	   *path = dir_file(dir, MANIFEST_FILE);
	bool		found = read_manifest_at(path, m);

	pfree(path);
	return found;
}

/* A manifest naming segment 0 alone, as a store without one has. */
static void
manifest_segment_zero(Manifest *m)
{
	m->nsegments = 1;
	m->segments = palloc0(sizeof(uint64));
}

/*
 * Read the manifest at path into m; false (m empty, but for segment 0) if
 * there is none.  One of version 1 names segment 0 alone, and no file
 * settled.
 */
static bool
read_manifest_at(const char *path, Manifest *m)
{
	int			fd;
	struct stat st;
	char	   *buf;
	const char *p;
	const char *end;
	pg_crc32c	crc;
	uint32		version;
	uint8		rootlen;
	uint32		nfiles;
	Size		entry;

	memset(m, 0, sizeof(*m));
	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	if (fd < 0)
	{
		if (errno == ENOENT)
		{
			manifest_segment_zero(m);
			return false;
		}
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
	}
	if (fstat(fd, &st) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", path)));
	buf = palloc(Max(st.st_size, 1));
	if (read(fd, buf, st.st_size) != st.st_size)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m", path)));
	CloseTransientFile(fd);

	p = buf;
	end = buf + st.st_size;
	if (st.st_size < 8 + 4 + 4 + 8 + 1 + 4 + 4 ||
		memcmp(p, MANIFEST_MAGIC, 8) != 0)
		goto corrupt;
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, buf, st.st_size - sizeof(crc));
	FIN_CRC32C(crc);
	if (crc != (pg_crc32c) read_le(end - sizeof(crc), 4))
		goto corrupt;
	end -= sizeof(crc);
	p += 8;
	version = (uint32) read_le(p, 4);
	p += 4;
	if (version != MANIFEST_VERSION && version != MANIFEST_V1)
		goto corrupt;
	m->gc_generation = (uint32) read_le(p, 4);
	p += 4;
	m->last_id = read_le(p, 8);
	p += 8;
	rootlen = (uint8) *p++;
	if (rootlen > PROLLY_HASH_MAX_LEN || end - p < rootlen + 4)
		goto corrupt;
	m->root.len = rootlen;
	memcpy(m->root.bytes, p, rootlen);
	p += rootlen;
	nfiles = (uint32) read_le(p, 4);
	p += 4;
	entry = version == MANIFEST_V1 ? 20 : 21;
	if ((Size) (end - p) < (Size) nfiles * entry)
		goto corrupt;
	m->nfiles = nfiles;
	m->files = palloc0(sizeof(ManifestFile) * Max(nfiles, 1));
	for (uint32 i = 0; i < nfiles; i++)
	{
		m->files[i].id = read_le(p, 8);
		m->files[i].nchunks = (uint32) read_le(p + 8, 4);
		m->files[i].size = read_le(p + 12, 8);
		if (version != MANIFEST_V1)
			m->files[i].flags = (uint8) p[20];
		p += entry;
	}
	if (version == MANIFEST_V1)
	{
		if (p != end)
			goto corrupt;
		manifest_segment_zero(m);
	}
	else
	{
		uint32		nsegs;
		uint32		nretiring;

		if (end - p < 4)
			goto corrupt;
		nsegs = (uint32) read_le(p, 4);
		p += 4;
		if (nsegs < 1 || (Size) (end - p) < (Size) nsegs * 8 + 4)
			goto corrupt;
		m->nsegments = nsegs;
		m->segments = palloc(sizeof(uint64) * nsegs);
		for (uint32 i = 0; i < nsegs; i++)
		{
			m->segments[i] = read_le(p, 8);
			p += 8;
		}
		nretiring = (uint32) read_le(p, 4);
		p += 4;
		if ((Size) (end - p) != (Size) nretiring * 8)
			goto corrupt;
		m->nretiring = nretiring;
		m->retiring = palloc(sizeof(uint64) * Max(nretiring, 1));
		for (uint32 i = 0; i < nretiring; i++)
		{
			m->retiring[i] = read_le(p, 8);
			p += 8;
		}
	}
	pfree(buf);
	return true;

corrupt:
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("file \"%s\" is not a chunk store manifest", path)));
	return false;				/* keep the compiler quiet */
}

/*
 * fsync a file of dir, if it exists: opened first, so one removed after
 * (a table file a conjoin replaced) is still synced, or, already gone,
 * skipped, never failed on.
 */
static void
sync_if_exists(const char *path)
{
	int			fd = OpenTransientFile(path, O_RDWR | PG_BINARY);

	if (fd < 0)
	{
		if (errno == ENOENT)
			return;
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
		return;
	}
	if (pg_fsync(fd) != 0)
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", path)));
	CloseTransientFile(fd);
}

/* Write a new manifest aside, durably, to be renamed over the old one. */
static void
write_manifest_temp(const char *dir, const char *data, Size len)
{
	char	   *path = dir_file(dir, MANIFEST_TEMP);
	int			fd = OpenTransientFile(path, O_RDWR | O_CREAT | O_TRUNC | PG_BINARY);

	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
	pgstat_report_wait_start(WAIT_EVENT_DATA_FILE_WRITE);
	if (write(fd, data, len) != len)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", path)));
	pgstat_report_wait_end();
	if (pg_fsync(fd) != 0)
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", path)));
	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));
	pfree(path);
}

static void
manifests_changed(void)
{
	pg_atomic_fetch_add_u64(&shared()->manifests, 1);
}

/* Manifests written or replayed, in any database: a change if it moved. */
static uint64
manifests_count(void)
{
	return pg_atomic_read_u64(&shared()->manifests);
}

/* Read the segments again, if a manifest was written since they were. */
static void
refresh_segments(ProllyChunkStore *s)
{
	uint64		count = manifests_count();
	char	   *dir;
	Manifest	m;

	if (count == s->segments_seen)
		return;
	dir = GetDatabasePath(s->dboid, s->spcoid);
	(void) read_manifest(dir, &m);
	load_segments(s, &m);
	s->segments_seen = count;
	pfree(dir);
}

/*
 * Replace the manifest of s's database with m, WAL-logged first (a record
 * of type info: XLOG_CHUNKS_MANIFEST, or a collection's).  A checkpoint
 * waits (DELAY_CHKPT_START), so one whose redo point follows the record
 * finds the new manifest in place.
 */
static void
replace_manifest(ProllyChunkStore *s, const Manifest *m, uint8 info)
{
	char	   *dir = GetDatabasePath(s->dboid, s->spcoid);
	char	   *temp = dir_file(dir, MANIFEST_TEMP);
	char	   *path = dir_file(dir, MANIFEST_FILE);
	StringInfoData bytes;
	xl_chunks_file xlrec;
	XLogRecPtr	lsn;

	initStringInfo(&bytes);
	manifest_bytes(m, &bytes);
	write_manifest_temp(dir, bytes.data, bytes.len);

	xlrec.hdr.impl_id = prolly_chunkstore_journal.info.impl_id;
	xlrec.dboid = s->dboid;
	xlrec.spcoid = s->spcoid;

	Assert((MyProc->delayChkptFlags & DELAY_CHKPT_START) == 0);
	START_CRIT_SECTION();
	MyProc->delayChkptFlags |= DELAY_CHKPT_START;
	XLogBeginInsert();
	XLogRegisterData(&xlrec, SizeOfChunksManifest);
	XLogRegisterData(bytes.data, bytes.len);
	lsn = XLogInsert(RM_PROLLY_CHUNKS_ID, info);
	XLogFlush(lsn);
	(void) durable_rename(temp, path, PANIC);
	MyProc->delayChkptFlags &= ~DELAY_CHKPT_START;
	END_CRIT_SECTION();

	manifests_changed();
	pfree(bytes.data);
	pfree(temp);
	pfree(path);
	pfree(dir);
}

/* A table file's bytes at offset, WAL-logged first, as appends are. */
static void
table_file_write(ProllyChunkStore *s, int fd, const char *path, uint64 id,
				 uint64 offset, const char *data, Size len)
{
	xl_chunks_file xlrec;
	XLogRecPtr	lsn;

	xlrec.hdr.impl_id = prolly_chunkstore_journal.info.impl_id;
	xlrec.dboid = s->dboid;
	xlrec.spcoid = s->spcoid;
	xlrec.id = id;
	xlrec.offset = offset;

	Assert((MyProc->delayChkptFlags & DELAY_CHKPT_START) == 0);
	START_CRIT_SECTION();
	MyProc->delayChkptFlags |= DELAY_CHKPT_START;
	XLogBeginInsert();
	XLogRegisterData(&xlrec, SizeOfChunksFile);
	XLogRegisterData(data, len);
	lsn = XLogInsert(RM_PROLLY_CHUNKS_ID, XLOG_CHUNKS_FILE);
	XLogFlush(lsn);
	pgstat_report_wait_start(WAIT_EVENT_DATA_FILE_WRITE);
	if (pg_pwrite(fd, data, len, offset) != len)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not write %zu bytes at %llu in file \"%s\": %m",
						len, (unsigned long long) offset, path)));
	pgstat_report_wait_end();
	MyProc->delayChkptFlags &= ~DELAY_CHKPT_START;
	END_CRIT_SECTION();
}

/* Remove a table file, or (segment) a journal segment. */
static void
table_file_unlink(ProllyChunkStore *s, const char *dir, uint64 id,
				  bool segment)
{
	xl_chunks_file xlrec;
	char	   *path = segment ? segment_path(s->dboid, s->spcoid, id) :
		table_file_path(dir, id);

	xlrec.hdr.impl_id = prolly_chunkstore_journal.info.impl_id;
	xlrec.dboid = s->dboid;
	xlrec.spcoid = s->spcoid;
	xlrec.id = id;
	xlrec.offset = segment ? UNLINK_SEGMENT : UNLINK_TABLE_FILE;
	XLogBeginInsert();
	XLogRegisterData(&xlrec, SizeOfChunksFile);
	XLogFlush(XLogInsert(RM_PROLLY_CHUNKS_ID, XLOG_CHUNKS_UNLINK));
	if (unlink(path) != 0 && errno != ENOENT)
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not remove file \"%s\": %m", path)));
	pfree(path);
}

static bool
table_file_read(void *arg, uint64 offset, char *buf, Size len)
{
	TableFile  *t = arg;

	return FileRead(t->file, buf, len, offset, WAIT_EVENT_DATA_FILE_READ) == len;
}

static void
close_table(TableFile *t)
{
	if (t->tf)
		PROLLY_CALL(table_format(), close, t->tf);
	if (t->file >= 0)
		FileClose(t->file);
	pfree(t->path);
	pfree(t);
}

/*
 * Open the table file f of database directory dir, in cxt: NULL if it is
 * gone (a conjoin replaced it since its manifest was read).
 */
static TableFile *
open_table(const char *dir, const ManifestFile *f, MemoryContext cxt)
{
	MemoryContext old = MemoryContextSwitchTo(cxt);
	TableFile  *t = palloc0(sizeof(TableFile));

	t->id = f->id;
	t->path = table_file_path(dir, t->id);
	t->size = f->size;
	t->settled = (f->flags & MANIFEST_FILE_SETTLED) != 0;
	t->file = PathNameOpenFile(t->path, O_RDONLY | PG_BINARY);
	MemoryContextSwitchTo(old);
	if (t->file < 0)
	{
		if (errno == ENOENT)
		{
			pfree(t->path);
			pfree(t);
			return NULL;
		}
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", t->path)));
	}
	t->tf = PROLLY_CALL(table_format(), open, table_file_read, t, t->size, cxt);
	if (t->tf == NULL)
	{
		FileClose(t->file);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("file \"%s\" is not a table file", t->path)));
	}
	return t;
}

static void
close_tables(List *tables)
{
	ListCell   *lc;

	foreach(lc, tables)
		close_table(lfirst(lc));
	list_free(tables);
}

/*
 * Read the manifest again, if one was written since it was last read: open
 * the table files it names and close those it no longer does.  The files
 * are opened before any is closed, and the store's list changed only once
 * all are; one gone meanwhile (a conjoin replaced it) sends us to the
 * newer manifest.
 */
static void
load_tables(ProllyChunkStore *s, bool force)
{
	char	   *dir;

	if (!force && pg_atomic_read_u64(&shared()->manifests) == s->manifests_seen)
		return;
	dir = GetDatabasePath(s->dboid, s->spcoid);
	for (int attempt = 0;; attempt++)
	{
		uint64		count = pg_atomic_read_u64(&shared()->manifests);
		Manifest	m;
		List	   *opened = NIL;
		List	   *kept = NIL;
		List	   *dropped = NIL;
		bool		gone = false;
		ListCell   *lc;

		(void) read_manifest(dir, &m);
		if (attempt == 0)
			INJECTION_POINT("prolly-manifest-read", NULL);
		PG_TRY();
		{
			for (int i = 0; i < m.nfiles && !gone; i++)
			{
				TableFile  *t = NULL;

				foreach(lc, s->tables)
					if (((TableFile *) lfirst(lc))->id == m.files[i].id)
						t = lfirst(lc);
				if (t == NULL)
				{
					t = open_table(dir, &m.files[i], TopMemoryContext);
					if (t == NULL)
						gone = true;
					else
						opened = lappend(opened, t);
				}
			}
		}
		PG_CATCH();
		{
			close_tables(opened);
			PG_RE_THROW();
		}
		PG_END_TRY();
		if (gone)
		{
			close_tables(opened);
			if (attempt >= 10)
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("the chunk store's table files keep changing while they are read"),
						 errhint("Try again.")));
			continue;
		}

		/* the files the manifest names, opened before or now */
		{
			MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

			foreach(lc, s->tables)
			{
				TableFile  *t = lfirst(lc);
				bool		named = false;

				for (int i = 0; i < m.nfiles; i++)
					named |= m.files[i].id == t->id;
				if (named)
					kept = lappend(kept, t);
				else
					dropped = lappend(dropped, t);
			}
			kept = list_concat(kept, opened);
			MemoryContextSwitchTo(old);
		}
		list_free(s->tables);
		s->tables = kept;
		close_tables(dropped);
		s->manifests_seen = count;
		if (s->segments_seen < count)
		{
			load_segments(s, &m);
			s->segments_seen = count;
		}
		break;
	}
	pfree(dir);
}

/*
 * Find a chunk in the table files, its bytes to out if out isn't NULL.  A
 * file removed since the manifest was read (by conjoin()) sends us to the
 * manifest again.
 */
static bool
tables_get(ProllyChunkStore *s, const ProllyHash *hash, StringInfo out)
{
	for (int attempt = 0; attempt < 2; attempt++)
	{
		ListCell   *lc;
		bool		stale = false;

		foreach(lc, s->tables)
		{
			TableFile  *t = lfirst(lc);
			uint64		offset;
			uint32		len;
			char	   *bytes;
			ssize_t		n;

			if (!PROLLY_CALL(table_format(), find, t->tf, hash, &offset, &len))
				continue;
			if (out == NULL)
				return true;
			bytes = palloc(len);
			n = FileRead(t->file, bytes, len, offset, WAIT_EVENT_DATA_FILE_READ);
			if (n != len)
			{
				pfree(bytes);
				if (n < 0 && errno == ENOENT)
				{
					stale = true;
					break;
				}
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not read %u bytes at %llu in file \"%s\": %m",
								len, (unsigned long long) offset, t->path)));
			}
			PROLLY_CALL(table_format(), decode, t->tf, bytes, len, out);
			pfree(bytes);
			return true;
		}
		if (!stale)
			return false;
		load_tables(s, true);
	}
	return false;
}

/*
 * Write table file id of the chunks next() gives (a chunk given twice
 * once), and sync it; false, writing nothing, if it gives none.
 */
static bool
write_file(ProllyChunkStore *s, uint64 id, ProllyChunkNext next, void *arg,
		   uint32 *nchunks_out, uint64 *size_out)
{
	const ProllyTableFileRoutine *fmt = table_format();
	char	   *dir = GetDatabasePath(s->dboid, s->spcoid);
	char	   *path = table_file_path(dir, id);
	ProllyTableFileWriter *w;
	StringInfoData buf;
	StringInfoData data;
	ProllyHash	hash;
	uint64		offset = 0;
	uint32		nchunks = 0;
	int			fd;
	HASHCTL		ctl;
	HTAB	   *seen;

	initStringInfo(&data);
	if (!next(arg, &hash, &data))
	{
		pfree(data.data);
		pfree(path);
		pfree(dir);
		return false;
	}
	ctl.keysize = sizeof(ProllyHash);
	ctl.entrysize = sizeof(ProllyHash);
	ctl.hcxt = CurrentMemoryContext;
	seen = hash_create("prolly table file chunks", 1024, &ctl,
					   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	fd = OpenTransientFile(path, O_RDWR | O_CREAT | O_TRUNC | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
	w = PROLLY_CALL(fmt, write_begin, CurrentMemoryContext);
	initStringInfo(&buf);
	do
	{
		ProllyHash	key;
		bool		found;

		/* a chunk given twice is written once */
		hash_key(&key, &hash);
		(void) hash_search(seen, &key, HASH_ENTER, &found);
		if (found)
		{
			resetStringInfo(&data);
			continue;
		}
		PROLLY_CALL(fmt, write_add, w, &hash, data.data, data.len, &buf);
		nchunks++;
		if (buf.len >= APPEND_BATCH)
		{
			table_file_write(s, fd, path, id, offset, buf.data, buf.len);
			offset += buf.len;
			resetStringInfo(&buf);
		}
		resetStringInfo(&data);
		CHECK_FOR_INTERRUPTS();
	} while (next(arg, &hash, &data));
	PROLLY_CALL(fmt, write_finish, w, &buf);
	table_file_write(s, fd, path, id, offset, buf.data, buf.len);
	offset += buf.len;
	if (pg_fsync(fd) != 0)
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", path)));
	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));
	hash_destroy(seen);
	pfree(buf.data);
	pfree(data.data);
	pfree(path);
	pfree(dir);
	*nchunks_out = nchunks;
	*size_out = offset;
	return true;
}

/*
 * Write a table file of the chunks next() gives, and a manifest naming it
 * (with flags) beside the files keep (of m) names and m's segments; the
 * files m names that keep leaves out are removed after.  The manifest's
 * lock is held.
 */
static void
write_table_file(ProllyChunkStore *s, Manifest *m, bool *keep, uint8 flags,
				 ProllyChunkNext next, void *arg)
{
	char	   *dir;
	uint64		id = m->last_id + 1;
	uint64		size;
	uint32		nchunks;
	Manifest	nm;

	if (!write_file(s, id, next, arg, &nchunks, &size))
		return;
	INJECTION_POINT("prolly-table-file-written", NULL);

	nm = *m;
	nm.last_id = id;
	/* (the committed root: one the transaction moves to may roll back) */
	scan_journal(s);
	nm.root = s->root;
	nm.nfiles = 0;
	nm.files = palloc(sizeof(ManifestFile) * (m->nfiles + 1));
	for (int i = 0; i < m->nfiles; i++)
	{
		if (keep[i])
			nm.files[nm.nfiles++] = m->files[i];
	}
	nm.files[nm.nfiles].id = id;
	nm.files[nm.nfiles].nchunks = nchunks;
	nm.files[nm.nfiles].size = size;
	nm.files[nm.nfiles].flags = flags;
	nm.nfiles++;
	replace_manifest(s, &nm, XLOG_CHUNKS_MANIFEST);
	INJECTION_POINT("prolly-manifest-replaced", NULL);

	dir = GetDatabasePath(s->dboid, s->spcoid);
	for (int i = 0; i < m->nfiles; i++)
	{
		if (!keep[i])
			table_file_unlink(s, dir, m->files[i].id, false);
	}
	load_tables(s, true);
	pfree(nm.files);
	pfree(dir);
}

/*
 * Remove the table files and journal segments of dir its manifest m
 * doesn't name, as kept or retiring: those a writer replaced but didn't
 * get to remove (it failed, or the server stopped, in between), or one it
 * was writing.  With the manifest's lock held, no writer is writing one
 * now.
 */
static void
remove_unnamed(ProllyChunkStore *s, const char *dir, const Manifest *m)
{
	DIR		   *d = AllocateDir(dir);
	struct dirent *de;
	List	   *ids = NIL;
	List	   *segs = NIL;
	ListCell   *lc;

	while ((de = ReadDir(d, dir)) != NULL)
	{
		uint64		id;
		char	   *end;
		bool		named = false;
		bool		segment;

		if (strcmp(de->d_name, JOURNAL_FILE) == 0)
		{
			id = 0;
			segment = true;
		}
		else if (strncmp(de->d_name, SEGMENT_PREFIX, strlen(SEGMENT_PREFIX)) == 0)
		{
			id = strtou64(de->d_name + strlen(SEGMENT_PREFIX), &end, 16);
			if (*end != '\0')
				continue;
			segment = true;
		}
		else if (strncmp(de->d_name, "prolly_table_", 13) == 0)
		{
			id = strtou64(de->d_name + 13, &end, 16);
			if (*end != '\0')
				continue;
			segment = false;
		}
		else
			continue;
		if (segment)
		{
			for (int i = 0; i < m->nsegments; i++)
				named |= m->segments[i] == id;
		}
		else
		{
			for (int i = 0; i < m->nfiles; i++)
				named |= m->files[i].id == id;
		}
		for (int i = 0; i < m->nretiring; i++)
			named |= m->retiring[i] == id;
		if (!named)
		{
			uint64	   *p = palloc(sizeof(uint64));

			*p = id;
			if (segment)
				segs = lappend(segs, p);
			else
				ids = lappend(ids, p);
		}
	}
	FreeDir(d);
	foreach(lc, ids)
		table_file_unlink(s, dir, *(uint64 *) lfirst(lc), false);
	foreach(lc, segs)
		table_file_unlink(s, dir, *(uint64 *) lfirst(lc), true);
	list_free_deep(ids);
	list_free_deep(segs);
}

/* Take the table file writers' lock, and read the manifest it protects. */
static void
lock_manifest(ProllyChunkStore *s, Manifest *m)
{
	LOCKTAG		tag;
	char	   *dir;

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("cannot change a chunk store during recovery")));
	PreventCommandIfReadOnly("writing a chunk store's table files");
	SET_LOCKTAG_OBJECT(tag, s->dboid, MANIFEST_LOCK_CLASSID, 0, 0);
	(void) LockAcquire(&tag, ExclusiveLock, false, false);
	dir = GetDatabasePath(s->dboid, s->spcoid);
	(void) read_manifest(dir, m);
	remove_unnamed(s, dir, m);
	pfree(dir);
}

/* Let the table file writers' lock go before the transaction ends. */
static void
unlock_manifest(ProllyChunkStore *s)
{
	LOCKTAG		tag;

	SET_LOCKTAG_OBJECT(tag, s->dboid, MANIFEST_LOCK_CLASSID, 0, 0);
	(void) LockRelease(&tag, ExclusiveLock, false);
}

/* ----------------------------------------------------------------
 * Callbacks
 * ----------------------------------------------------------------
 */

static ProllyChunkStore *
journal_open(Oid dboid, Oid spcoid)
{
	ProllyChunkStore *s;
	MemoryContext old;
	HASHCTL		ctl;

	if (session_store != NULL)
	{
		if (session_store->dboid != dboid)
			elog(ERROR, "a session has only its database's chunk store");
		return session_store;
	}

	old = MemoryContextSwitchTo(TopMemoryContext);
	s = palloc0(sizeof(ProllyChunkStore));
	s->dboid = dboid;
	s->spcoid = spcoid;
	ctl.keysize = sizeof(ProllyHash);
	ctl.entrysize = sizeof(IndexEntry);
	s->index = hash_create("prolly journal index", 1024, &ctl,
						   HASH_ELEM | HASH_BLOBS);
	s->pending_index = hash_create("prolly journal pending", 256, &ctl,
								   HASH_ELEM | HASH_BLOBS);
	initStringInfo(&s->pending);
	MemoryContextSwitchTo(old);

	(void) ensure_journal(s, false);

	RegisterXactCallback(journal_xact_callback, NULL);
	RegisterSubXactCallback(journal_subxact_callback, NULL);
	session_store = s;
	return s;
}

static void
journal_put(ProllyChunkStore *s, const ProllyHash *hash, const void *data,
			Size len)
{
	ProllyHash	key;
	IndexEntry *e;
	bool		found;
	uint8		head[sizeof(TransactionId) + 1 + PROLLY_HASH_MAX_LEN];
	Size		headlen;
	uint64		at;
	TransactionId xid;
	int			level;

	if (hash->len == 0 || hash->len > PROLLY_HASH_MAX_LEN)
		elog(ERROR, "invalid chunk hash length %u", hash->len);
	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("cannot change a chunk store during recovery")));
	if (len > MaxAllocSize - 64)
		elog(ERROR, "chunk of %zu bytes is too large", len);
	xact_note(s);

	hash_key(&key, hash);
	if (hash_search(s->pending_index, &key, HASH_FIND, NULL) != NULL ||
		index_has(s->index, &key) ||
		tables_get(s, &key, NULL))
		return;

	/* pending as it was, if a subtransaction puts its first */
	level = GetCurrentTransactionNestLevel();
	if (level > 1 &&
		(s->pending_saved == NIL ||
		 ((PendingSave *) llast(s->pending_saved))->level < level))
	{
		MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);
		PendingSave *save = palloc(sizeof(PendingSave));

		save->level = level;
		save->flushes = s->flushes;
		save->len = s->pending.len;
		s->pending_saved = lappend(s->pending_saved, save);
		MemoryContextSwitchTo(old);
	}

	/* an XCHUNK: the transaction's until it ends */
	xid = GetTopTransactionId();
	memcpy(head, &xid, sizeof(xid));
	hash_payload(hash, head + sizeof(xid), &headlen);
	headlen += sizeof(xid);
	at = s->pending.len;
	record_append(&s->pending, KIND_XCHUNK, head, headlen, data, len);
	e = hash_search(s->pending_index, &key, HASH_ENTER, &found);
	e->offset = at;
	e->len = s->pending.len - at;
	e->xid = xid;
	e->other = NULL;

	if (s->pending.len >= APPEND_BATCH)
		flush_pending(s);
}

/* The chunk's bytes from its record (rec, len bytes, in path) to out. */
static void
chunk_bytes(const char *path, const char *rec, uint32 len, StringInfo out)
{
	ProllyHash	h;
	Size		after;

	if (!record_valid(rec, len) || !record_hash(rec, len, &h, &after))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupt chunk record in file \"%s\"", path)));
	appendBinaryStringInfo(out, rec + after, len - after - sizeof(uint32));
}

static bool
journal_get(ProllyChunkStore *s, const ProllyHash *hash, StringInfo out)
{
	ProllyHash	key;
	IndexEntry *e;
	Segment    *g;
	char	   *rec;

	hash_key(&key, hash);
	e = hash_search(s->pending_index, &key, HASH_FIND, NULL);
	if (e != NULL)
	{
		chunk_bytes("(pending)", s->pending.data + e->offset, e->len, out);
		return true;
	}
	refresh_segments(s);
	xact_note(s);
	e = index_find(s->index, &key);
	if (e == NULL)
	{
		if (tables_get(s, &key, out))
			return true;
		scan_journal(s);
		e = index_find(s->index, &key);
		if (e == NULL)
		{
			load_tables(s, false);
			if (tables_get(s, &key, out))
				return true;
			return s->through != NULL && s->through(s->through_arg, hash, out);
		}
	}
	g = segment_of(s, e->segment);
	if (g == NULL || !open_segment(g))
		elog(ERROR, "chunk journal segment %llu is missing",
			 (unsigned long long) e->segment);
	rec = palloc(e->len);
	read_exact(g, rec, e->len, e->offset);
	chunk_bytes(g->path, rec, e->len, out);
	pfree(rec);
	return true;
}

static bool
journal_has(ProllyChunkStore *s, const ProllyHash *hash)
{
	ProllyHash	key;

	hash_key(&key, hash);
	if (hash_search(s->pending_index, &key, HASH_FIND, NULL) != NULL)
		return true;
	refresh_segments(s);
	xact_note(s);
	if (stored_has(s, &key))
		return true;
	return s->through != NULL && s->through(s->through_arg, hash, NULL);
}

static void
journal_root(ProllyChunkStore *s, ProllyHash *root)
{
	xact_note(s);
	if (s->moving)
	{
		*root = s->new_root;
		return;
	}
	scan_journal(s);
	*root = s->root;
}

static bool
journal_move_root(ProllyChunkStore *s, const ProllyHash *expected,
				  const ProllyHash *root)
{
	LOCKTAG		tag;
	ProllyHash	current;

	if (root->len == 0 || root->len > PROLLY_HASH_MAX_LEN)
		elog(ERROR, "invalid root hash length %u", root->len);
	PreventCommandIfReadOnly("moving a chunk store's root");

	/* the commit record carries the move */
	(void) GetCurrentTransactionId();

	if (!s->moving)
	{
		SET_LOCKTAG_OBJECT(tag, s->dboid, ROOT_LOCK_CLASSID, 0, 0);
		(void) LockAcquire(&tag, ExclusiveLock, false, false);
	}
	journal_root(s, &current);
	if (current.len != expected->len ||
		memcmp(current.bytes, expected->bytes, current.len) != 0)
		return false;
	if (GetCurrentTransactionNestLevel() > 1 &&
		(s->saved == NIL ||
		 ((MoveSave *) llast(s->saved))->level < GetCurrentTransactionNestLevel()))
	{
		MoveSave   *save = MemoryContextAlloc(TopTransactionContext,
											  sizeof(MoveSave));
		MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);

		save->level = GetCurrentTransactionNestLevel();
		save->moving = s->moving;
		save->new_root = s->new_root;
		s->saved = lappend(s->saved, save);
		MemoryContextSwitchTo(old);
	}
	s->new_root = *root;
	s->moving = true;
	return true;
}

static Size
journal_commit_payload(char **data)
{
	ProllyChunkStore *s = session_store;
	CommitPayload *p;

	if (s == NULL || !s->moving)
		return 0;
	Assert(s->pad_offset != 0);
	p = palloc0(sizeof(CommitPayload));
	p->dboid = s->dboid;
	p->spcoid = s->spcoid;
	p->segment = s->pad_segment->id;
	p->offset = s->pad_offset;
	p->root = s->new_root;
	*data = (char *) p;
	return sizeof(CommitPayload);
}

/* In the commit's critical section: no allocation. */
static void
journal_committed(void)
{
	ProllyChunkStore *s = session_store;

	if (s == NULL || !s->moving)
		return;
	if (FileWrite(s->pad_segment->file, s->root_rec, s->root_rec_len,
				  s->pad_offset, WAIT_EVENT_DATA_FILE_WRITE) != s->root_rec_len)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not write the root in file \"%s\": %m",
						s->pad_segment->path)));
	s->root = s->new_root;
	s->root_segment = s->pad_segment->id;
	s->root_offset = s->pad_offset;
}

static void
journal_commit_redo(const char *data, Size len)
{
	CommitPayload p;
	StringInfoData rec;
	char	   *path;

	if (len != sizeof(CommitPayload))
		elog(PANIC, "chunk journal commit payload of %zu bytes", len);
	memcpy(&p, data, sizeof(p));
	initStringInfo(&rec);
	root_record(&rec, &p.root, false);
	path = segment_path(p.dboid, p.spcoid, p.segment);
	write_at(path, rec.data, rec.len, p.offset);
	pfree(path);
	pfree(rec.data);
}

/*
 * Replay a table file's bytes (the first truncating what a failed writer
 * left), a manifest (after syncing the files it names, which replay wrote
 * without syncing) or an unlink.
 */
static void
table_files_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & PROLLY_XLOG_TYPE_MASK;
	xl_chunks_file xlrec;
	char	   *dir;

	memset(&xlrec, 0, sizeof(xlrec));
	memcpy(&xlrec, XLogRecGetData(record),
		   info == XLOG_CHUNKS_FILE || info == XLOG_CHUNKS_UNLINK ?
		   SizeOfChunksFile : SizeOfChunksManifest);
	dir = GetDatabasePath(xlrec.dboid, xlrec.spcoid);
	if (info == XLOG_CHUNKS_FILE)
	{
		char	   *path = table_file_path(dir, xlrec.id);

		if (xlrec.offset == 0)
		{
			int			fd = OpenTransientFile(path, O_RDWR | O_CREAT | O_TRUNC | PG_BINARY);

			if (fd < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\": %m", path)));
			CloseTransientFile(fd);
		}
		write_at(path, XLogRecGetData(record) + SizeOfChunksFile,
				 XLogRecGetDataLen(record) - SizeOfChunksFile, xlrec.offset);
		pfree(path);
	}
	else if (info != XLOG_CHUNKS_UNLINK)
	{
		const char *data = XLogRecGetData(record) + SizeOfChunksManifest;
		Size		len = XLogRecGetDataLen(record) - SizeOfChunksManifest;
		char	   *temp = dir_file(dir, MANIFEST_TEMP);
		char	   *path = dir_file(dir, MANIFEST_FILE);
		Manifest	m;
		Manifest	before;

		/*
		 * Queries that may read what a swap stopped naming: those running
		 * as it is replayed, cancelled when the files are retired.
		 */
		if (info == XLOG_CHUNKS_SWAP)
			standby_readers_note(xlrec.dboid);
		else if (info == XLOG_CHUNKS_RETIRE)
		{
			standby_readers_resolve(xlrec.dboid);
			(void) read_manifest(dir, &before);
		}
		write_manifest_temp(dir, data, len);
		(void) read_manifest_at(temp, &m);
		for (int i = 0; i < m.nfiles; i++)
		{
			char	   *tpath = table_file_path(dir, m.files[i].id);

			sync_if_exists(tpath);
			pfree(tpath);
		}
		(void) durable_rename(temp, path, ERROR);
		pfree(temp);
		pfree(path);
		manifests_changed();
		if (info == XLOG_CHUNKS_RETIRE)
			retire_files(xlrec.dboid, xlrec.spcoid, &before, &m);
	}
	else
	{
		char	   *path = xlrec.offset == UNLINK_SEGMENT ?
			segment_path(xlrec.dboid, xlrec.spcoid, xlrec.id) :
			table_file_path(dir, xlrec.id);

		if (unlink(path) != 0 && errno != ENOENT)
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m", path)));
		pfree(path);
		manifests_changed();
	}
	pfree(dir);
}

static void
journal_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	xl_chunks_append xlrec;
	char	   *path;

	switch (info & PROLLY_XLOG_TYPE_MASK)
	{
		case XLOG_CHUNKS_APPEND:
			memcpy(&xlrec, XLogRecGetData(record), SizeOfChunksAppend);
			path = segment_path(xlrec.dboid, xlrec.spcoid, xlrec.segment);
			write_at(path, XLogRecGetData(record) + SizeOfChunksAppend,
					 XLogRecGetDataLen(record) - SizeOfChunksAppend, xlrec.offset);
			pfree(path);
			break;
		case XLOG_CHUNKS_FILE:
		case XLOG_CHUNKS_MANIFEST:
		case XLOG_CHUNKS_UNLINK:
		case XLOG_CHUNKS_SEAL:
		case XLOG_CHUNKS_SWAP:
		case XLOG_CHUNKS_RETIRE:
			table_files_redo(record);
			break;
		default:
			elog(PANIC, "chunk journal: unknown record type %u", info);
	}
}

/*
 * fsync the journal's segments in database directory dir, if it has any,
 * and every table file there: those the manifest names (written by replay,
 * unsynced), one being written, and those a new database is being given,
 * which no manifest names yet.
 */
static void
sync_journal_in(const char *dir)
{
	char	   *path;
	DIR		   *d;
	struct dirent *de;

	d = AllocateDir(dir);
	if (d == NULL)
		return;
	while ((de = ReadDir(d, dir)) != NULL)
	{
		if (strncmp(de->d_name, JOURNAL_FILE, strlen(JOURNAL_FILE)) == 0)
		{
			path = dir_file(dir, de->d_name);
			sync_if_exists(path);
			pfree(path);
			continue;
		}
		if (strncmp(de->d_name, "prolly_table_", 13) != 0)
			continue;
		INJECTION_POINT("prolly-checkpoint-table-file", NULL);
		path = dir_file(dir, de->d_name);
		sync_if_exists(path);
		pfree(path);
	}
	FreeDir(d);
}

/* fsync the journals of every database directory under parent */
static void
sync_journals_under(const char *parent)
{
	DIR		   *dir = AllocateDir(parent);
	struct dirent *de;

	if (dir == NULL)
		return;
	while ((de = ReadDir(dir, parent)) != NULL)
	{
		char	   *sub;

		if (strspn(de->d_name, "0123456789") != strlen(de->d_name))
			continue;
		sub = psprintf("%s/%s", parent, de->d_name);
		sync_journal_in(sub);
		pfree(sub);
	}
	FreeDir(dir);
}

/*
 * Make every journal's appends durable.  Appends logged before the redo
 * point were written before it was set (DELAY_CHKPT_START), so this covers
 * them.
 */
static void
journal_checkpoint(void)
{
	DIR		   *tsdir;
	struct dirent *de;

	sync_journals_under("base");

	tsdir = AllocateDir(PG_TBLSPC_DIR);
	if (tsdir == NULL)
		return;
	while ((de = ReadDir(tsdir, PG_TBLSPC_DIR)) != NULL)
	{
		char	   *sub;

		if (strspn(de->d_name, "0123456789") != strlen(de->d_name))
			continue;
		sub = psprintf("%s/%s/%s", PG_TBLSPC_DIR, de->d_name,
					   TABLESPACE_VERSION_DIRECTORY);
		sync_journals_under(sub);
		pfree(sub);
	}
	FreeDir(tsdir);
}

static void
journal_read_through(ProllyChunkStore *s, ProllyChunkGet source,
					 ProllyChunkHint hint, void *arg)
{
	s->through = source;
	s->through_hint = hint;
	s->through_arg = arg;
}

static bool
journal_prefetch(ProllyChunkStore *s, const ProllyHash *hashes, int n)
{
	if (s->through_hint == NULL)
		return false;
	if (n > 0)
		s->through_hint(s->through_arg, hashes, n);
	return true;
}

/*
 * A table file's chunks were checked against the store as the transaction
 * found it (a transfer's walk declines what the store has): if a swap has
 * happened since the transaction began, what it trusted may be gone, and
 * the transaction is retried.
 */
static void
journal_add_table_file(ProllyChunkStore *s, ProllyChunkNext next, void *arg)
{
	Manifest	m;
	bool	   *keep;

	xact_note(s);
	lock_manifest(s, &m);
	if (m.gc_generation != s->xact_generation)
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("the chunk store was collected while the transaction ran"),
				 errhint("Try the transaction again.")));
	keep = palloc(sizeof(bool) * Max(m.nfiles, 1));
	for (int i = 0; i < m.nfiles; i++)
		keep[i] = true;
	write_table_file(s, &m, keep, 0, next, arg);
	pfree(keep);
}

/* conjoin()'s source: every chunk of the table files, once */
typedef struct ConjoinSource
{
	ProllyChunkStore *s;
	List	   *tables;			/* TableFile *, opened for this */
	ListCell   *cur;
	int			i;				/* the next entry of cur's */
	HTAB	   *seen;
} ConjoinSource;

static bool
conjoin_next(void *arg, ProllyHash *hash, StringInfo data)
{
	ConjoinSource *cs = arg;

	while (cs->cur != NULL)
	{
		TableFile  *t = lfirst(cs->cur);
		const ProllyTableFileRoutine *fmt = table_format();

		while (cs->i < PROLLY_CALL(fmt, count, t->tf))
		{
			uint64		offset;
			uint32		len;
			char	   *bytes;
			bool		found;
			ProllyHash	key;

			PROLLY_CALL(fmt, entry, t->tf, cs->i++, hash, &offset, &len);
			hash_key(&key, hash);
			(void) hash_search(cs->seen, &key, HASH_ENTER, &found);
			if (found)
				continue;
			bytes = palloc(len);
			if (!table_file_read(t, offset, bytes, len))
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not read %u bytes at %llu in file \"%s\": %m",
								len, (unsigned long long) offset, t->path)));
			PROLLY_CALL(fmt, decode, t->tf, bytes, len, data);
			pfree(bytes);
			return true;
		}
		cs->cur = lnext(cs->tables, cs->cur);
		cs->i = 0;
	}
	return false;
}

/*
 * Rewrite the table files as one, which the manifest names alone: settled
 * if they all were.
 */
static void
journal_conjoin(ProllyChunkStore *s)
{
	Manifest	m;
	bool	   *keep;
	ConjoinSource cs;
	HASHCTL		ctl;
	char	   *dir;
	ListCell   *lc;
	uint8		flags = MANIFEST_FILE_SETTLED;

	lock_manifest(s, &m);
	if (m.nfiles < 2)
		return;
	for (int i = 0; i < m.nfiles; i++)
		flags &= m.files[i].flags;

	/* the files as the lock's manifest names them, opened for this */
	dir = GetDatabasePath(s->dboid, s->spcoid);
	memset(&cs, 0, sizeof(cs));
	cs.s = s;
	ctl.keysize = sizeof(ProllyHash);
	ctl.entrysize = sizeof(ProllyHash);
	ctl.hcxt = CurrentMemoryContext;
	cs.seen = hash_create("prolly conjoin", 1024, &ctl,
						  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	keep = palloc0(sizeof(bool) * m.nfiles);

	/* (closed whatever happens: a file kept open is never given back) */
	PG_TRY();
	{
		for (int i = 0; i < m.nfiles; i++)
		{
			TableFile  *t = open_table(dir, &m.files[i], CurrentMemoryContext);

			if (t == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("table file %llu of the manifest is missing",
								(unsigned long long) m.files[i].id)));
			cs.tables = lappend(cs.tables, t);
		}
		cs.cur = list_head(cs.tables);
		write_table_file(s, &m, keep, flags, conjoin_next, &cs);
	}
	PG_FINALLY();
	{
		foreach(lc, cs.tables)
		{
			TableFile  *t = lfirst(lc);

			PROLLY_CALL(table_format(), close, t->tf);
			FileClose(t->file);
		}
	}
	PG_END_TRY();
	hash_destroy(cs.seen);
	pfree(keep);
	pfree(dir);
}

static void
journal_table_files(ProllyChunkStore *s, ProllyChunkFileVisit visit, void *arg)
{
	ListCell   *lc;

	load_tables(s, false);
	foreach(lc, s->tables)
	{
		TableFile  *t = lfirst(lc);
		char	   *name = table_file_name(t->id);

		visit(name, PROLLY_CALL(table_format(), count, t->tf), t->size,
			  t->settled, arg);
		pfree(name);
	}
}

/*
 * Seal the segment appends go to: a new one is made (its header written)
 * and a manifest names it after the others, under the append lock, so
 * every append from here on goes to it.  The manifest's lock is held; m is
 * the manifest, and becomes the new one.  Returns the new segment's id.
 */
static uint64
seal_segment(ProllyChunkStore *s, Manifest *m)
{
	uint64		id = m->last_id + 1;
	uint64	   *segs = palloc(sizeof(uint64) * (m->nsegments + 1));

	memcpy(segs, m->segments, sizeof(uint64) * m->nsegments);
	segs[m->nsegments] = id;
	scan_journal(s);
	m->root = s->root;
	m->last_id = id;
	m->segments = segs;
	m->nsegments++;
	LWLockAcquire(ProllyJournalLock, LW_EXCLUSIVE);
	segment_header(s->dboid, s->spcoid, id);
	INJECTION_POINT("prolly-segment-made", NULL);
	replace_manifest(s, m, XLOG_CHUNKS_SEAL);
	LWLockRelease(ProllyJournalLock);
	return id;
}

static void
journal_seal(ProllyChunkStore *s)
{
	Manifest	m;

	lock_manifest(s, &m);
	(void) seal_segment(s, &m);
}

static void
journal_segments(ProllyChunkStore *s, ProllyChunkFileVisit visit, void *arg)
{
	ListCell   *lc;

	scan_journal(s);
	foreach(lc, s->segments)
	{
		Segment    *g = lfirst(lc);
		HASH_SEQ_STATUS status;
		IndexEntry *e;
		int			n = 0;
		char	   *name = strrchr(g->path, '/') + 1;
		pgoff_t		size;

		hash_seq_init(&status, s->index);
		while ((e = hash_seq_search(&status)) != NULL)
		{
			IndexEntry *c = e;

			while (c != NULL && c->segment != g->id)
				c = c->other;
			n += c != NULL;
		}
		size = open_segment(g) ? FileSize(g->file) : 0;
		if (size < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not determine the size of file \"%s\": %m",
							g->path)));
		visit(name, n, (uint64) size, false, arg);
	}
}

/* ----------------------------------------------------------------
 * Garbage collection
 * ----------------------------------------------------------------
 */

/* a file a collection collects: a sealed segment, or a table file */
typedef struct GcFile
{
	bool		segment;
	uint64		id;
	uint64		first;			/* its chunks' first slot */
	uint64		count;
	bool		settled;
	uint64		size;
	char	   *path;
	File		file;			/* a segment's, opened for this */
	uint64	   *offsets;		/* a segment's: each slot's record */
	uint32	   *lens;
	TableFile  *t;				/* a table file, opened for this */
} GcFile;

/* a sealed segment's chunk: its slot */
typedef struct GcSlot
{
	ProllyHash	hash;			/* key */
	uint64		slot;
} GcSlot;

struct ProllyChunkGc
{
	ProllyChunkStore *s;
	MemoryContext cxt;
	bool		full;
	Manifest	m;				/* as the seal left it */
	uint64		segment;		/* where appends go from the seal */
	int			nfiles;
	GcFile	   *files;
	uint64		nslots;
	HTAB	   *slots;			/* the sealed segments' chunks */
	uint8		hash_len;
	List	   *xids;			/* TransactionId: appended to the sealed
								 * segments, in progress at the seal */
	bool		waited;
	Segment    *appended;		/* the segment appended to from the seal */
	uint64		next_id;		/* the next file id to give */
	uint8	   *copied;			/* slots gc_copy() wrote */
	List	   *written;		/* ManifestFile *, the new files */
	bool		window;
	VirtualTransactionId *readers;	/* running at the swap */
	int			nreaders;
};

static char *
dropped_path(Oid dboid, Oid spcoid, uint32 generation)
{
	char	   *dir = GetDatabasePath(dboid, spcoid);
	char	   *path = psprintf("%s/prolly_gc_dropped_%08x", dir, generation);

	pfree(dir);
	return path;
}

static bool
marked(const uint8 *marks, uint64 slot)
{
	return (marks[slot / 8] & (1 << (slot % 8))) != 0;
}

/* The collected file a slot is in. */
static GcFile *
slot_file(ProllyChunkGc *gc, uint64 slot)
{
	int			lo = 0;
	int			hi = gc->nfiles - 1;

	while (lo < hi)
	{
		int			mid = lo + (hi - lo + 1) / 2;

		if (gc->files[mid].first <= slot)
			lo = mid;
		else
			hi = mid - 1;
	}
	Assert(slot >= gc->files[lo].first &&
		   slot < gc->files[lo].first + gc->files[lo].count);
	return &gc->files[lo];
}

/*
 * Wait for every transaction now running in this database, but ours, to
 * end, as CREATE INDEX CONCURRENTLY does: none can read a file named
 * before.
 */
static void
wait_for_readers(VirtualTransactionId *vxids, int n)
{
	for (int i = 0; i < n; i++)
	{
		CHECK_FOR_INTERRUPTS();
		(void) VirtualXactLock(vxids[i], true);
	}
}

/*
 * Remove the files the manifest before named retiring and after doesn't
 * (a table file's or a segment's, by its id), and the lists of what
 * collections dropped.
 */
static void
retire_files(Oid dboid, Oid spcoid, const Manifest *before,
			 const Manifest *after)
{
	char	   *dir = GetDatabasePath(dboid, spcoid);
	DIR		   *d;
	struct dirent *de;

	for (int i = 0; i < before->nretiring; i++)
	{
		bool		still = false;
		char	   *paths[2];

		for (int j = 0; j < after->nretiring; j++)
			still |= after->retiring[j] == before->retiring[i];
		if (still)
			continue;
		paths[0] = table_file_path(dir, before->retiring[i]);
		paths[1] = segment_path(dboid, spcoid, before->retiring[i]);
		for (int k = 0; k < 2; k++)
		{
			if (unlink(paths[k]) != 0 && errno != ENOENT)
				ereport(WARNING,
						(errcode_for_file_access(),
						 errmsg("could not remove file \"%s\": %m", paths[k])));
			pfree(paths[k]);
		}
	}
	d = AllocateDir(dir);
	while (d != NULL && (de = ReadDir(d, dir)) != NULL)
	{
		char	   *path;

		if (strncmp(de->d_name, "prolly_gc_dropped_", 18) != 0)
			continue;
		path = dir_file(dir, de->d_name);
		if (unlink(path) != 0 && errno != ENOENT)
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m", path)));
		pfree(path);
	}
	if (d != NULL)
		FreeDir(d);
	pfree(dir);
}

/*
 * Retire what m names retiring: a manifest without it, then the files
 * removed.  The manifest's lock is held, and nobody reads them.
 */
static void
retire(ProllyChunkStore *s, Manifest *m)
{
	Manifest	nm = *m;

	nm.nretiring = 0;
	scan_journal(s);
	nm.root = s->root;
	replace_manifest(s, &nm, XLOG_CHUNKS_RETIRE);
	retire_files(s->dboid, s->spcoid, m, &nm);
	*m = nm;
}

/* The standby's queries running as a swap is replayed, by database. */
typedef struct StandbyReaders
{
	Oid			dboid;
	VirtualTransactionId *vxids;	/* InvalidVirtualTransactionId at the end */
} StandbyReaders;

static List *standby_readers = NIL;

static void
standby_readers_note(Oid dboid)
{
	MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);
	StandbyReaders *r = palloc(sizeof(StandbyReaders));
	VirtualTransactionId *running = GetConflictingVirtualXIDs(InvalidTransactionId,
															  dboid);
	int			n = 0;

	/* (a copy: the array is GetConflictingVirtualXIDs()'s own) */
	while (VirtualTransactionIdIsValid(running[n]))
		n++;
	r->dboid = dboid;
	r->vxids = palloc(sizeof(VirtualTransactionId) * (n + 1));
	memcpy(r->vxids, running, sizeof(VirtualTransactionId) * (n + 1));
	standby_readers = lappend(standby_readers, r);
	MemoryContextSwitchTo(old);
}

static void
standby_readers_resolve(Oid dboid)
{
	ListCell   *lc;

	foreach(lc, standby_readers)
	{
		StandbyReaders *r = lfirst(lc);

		if (r->dboid != dboid)
			continue;
		ResolveRecoveryConflictWithFileReaders(r->vxids);
		pfree(r->vxids);
		pfree(r);
		standby_readers = foreach_delete_current(standby_readers, lc);
	}
}

/*
 * Index a sealed segment's chunks as slots: each chunk once (its first
 * copy).  Its transactions in progress are noted, to be waited for: an
 * XCHUNK's or a PAD's.
 */
static void
gc_add_segment(ProllyChunkGc *gc, GcFile *f)
{
	uint64		size;
	uint64		offset = JOURNAL_HEADER_SIZE;
	uint64		max = 64;
	char	   *rec = NULL;
	Size		reclen = 0;

	f->file = PathNameOpenFile(f->path, O_RDONLY | PG_BINARY);
	if (f->file < 0)
	{
		if (errno == ENOENT)
			return;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", f->path)));
	}
	size = FileSize(f->file);
	f->size = size;
	f->offsets = palloc(sizeof(uint64) * max);
	f->lens = palloc(sizeof(uint32) * max);
	while (offset + RECORD_OVERHEAD <= size)
	{
		uint32		len;
		uint8		kind;
		ProllyHash	h;
		Size		after;
		TransactionId xid = InvalidTransactionId;

		if (FileRead(f->file, (char *) &len, sizeof(len), offset,
					 WAIT_EVENT_DATA_FILE_READ) != sizeof(len) ||
			len < RECORD_OVERHEAD || offset + len > size)
			break;
		if (len > reclen)
		{
			reclen = Max(len, 8192);
			rec = rec ? repalloc(rec, reclen) : palloc(reclen);
		}
		if (FileRead(f->file, rec, len, offset, WAIT_EVENT_DATA_FILE_READ) != len ||
			!record_valid(rec, len))
			break;
		kind = (uint8) rec[sizeof(uint32)];
		if ((kind == KIND_CHUNK || kind == KIND_XCHUNK) &&
			record_hash_xid(rec, len, &h, &after, &xid))
		{
			ProllyHash	key;
			GcSlot	   *e;
			bool		found;

			hash_key(&key, &h);
			e = hash_search(gc->slots, &key, HASH_ENTER, &found);
			if (!found)
			{
				if (f->count == max)
				{
					max *= 2;
					f->offsets = repalloc(f->offsets, sizeof(uint64) * max);
					f->lens = repalloc(f->lens, sizeof(uint32) * max);
				}
				e->slot = f->first + f->count;
				f->offsets[f->count] = offset;
				f->lens[f->count] = len;
				f->count++;
				if (gc->hash_len == 0)
					gc->hash_len = h.len;
			}
		}
		else if (kind == KIND_PAD && len >= sizeof(uint32) + 1 + sizeof(TransactionId))
			memcpy(&xid, rec + sizeof(uint32) + 1, sizeof(xid));
		if (TransactionIdIsValid(xid) && TransactionIdIsInProgress(xid) &&
			!list_member_xid(gc->xids, xid))
			gc->xids = lappend_xid(gc->xids, xid);
		offset += len;
	}
	if (rec)
		pfree(rec);
}

static ProllyChunkGc *
journal_gc_begin(ProllyChunkStore *s, bool full, ProllyHash *root,
				 uint64 *nslots)
{
	MemoryContext cxt = AllocSetContextCreate(TopTransactionContext,
											  "prolly collection",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	ProllyChunkGc *gc = palloc0(sizeof(ProllyChunkGc));
	char	   *dir = GetDatabasePath(s->dboid, s->spcoid);
	HASHCTL		ctl;
	int			n = 0;
	uint64		slot = 0;

	gc->s = s;
	gc->cxt = cxt;
	gc->full = full;
	if (s->gc != NULL)
		elog(ERROR, "a chunk store collection is running in this transaction");

	/*
	 * One at a time; no conjoin or new table file until the swap (from
	 * then on a transfer older than the swap is refused, not waited for)
	 */
	{
		LOCKTAG		tag;

		SET_LOCKTAG_OBJECT(tag, s->dboid, GC_LOCK_CLASSID, 0, 0);
		(void) LockAcquire(&tag, ExclusiveLock, false, false);
	}

	/*
	 * What an earlier collection left retiring (it failed after its swap),
	 * once nobody who may read it is left: waited for before the
	 * manifest's lock, which they may be waiting for.
	 */
	(void) read_manifest(dir, &gc->m);
	if (gc->m.nretiring > 0)
	{
		int			nvxids;
		VirtualTransactionId *vxids = GetCurrentVirtualXIDs(InvalidTransactionId,
															false, false, 0,
															&nvxids);

		wait_for_readers(vxids, nvxids);
	}
	lock_manifest(s, &gc->m);
	if (gc->m.nretiring > 0)
		retire(s, &gc->m);
	else
		retire_files(s->dboid, s->spcoid, &gc->m, &gc->m);

	gc->segment = seal_segment(s, &gc->m);
	INJECTION_POINT("prolly-gc-sealed", NULL);
	gc->next_id = gc->m.last_id + 1;

	ctl.keysize = sizeof(ProllyHash);
	ctl.entrysize = sizeof(GcSlot);
	ctl.hcxt = cxt;
	gc->slots = hash_create("prolly collection slots", 1024, &ctl,
							HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	gc->files = palloc0(sizeof(GcFile) * (gc->m.nsegments + gc->m.nfiles));

	/* every segment but the one appends go to now */
	for (int i = 0; i < gc->m.nsegments - 1; i++)
	{
		GcFile	   *f = &gc->files[n++];

		f->segment = true;
		f->id = gc->m.segments[i];
		f->first = slot;
		f->path = segment_path(s->dboid, s->spcoid, f->id);
		f->file = -1;
		gc_add_segment(gc, f);
		slot += f->count;
	}
	for (int i = 0; i < gc->m.nfiles; i++)
	{
		GcFile	   *f;
		bool		settled = (gc->m.files[i].flags & MANIFEST_FILE_SETTLED) != 0;

		if (settled && !full)
			continue;
		f = &gc->files[n++];
		f->id = gc->m.files[i].id;
		f->first = slot;
		f->settled = settled;
		f->size = gc->m.files[i].size;
		f->file = -1;
		f->t = open_table(dir, &gc->m.files[i], cxt);
		if (f->t == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("table file %llu of the manifest is missing",
							(unsigned long long) f->id)));
		f->path = f->t->path;
		f->count = PROLLY_CALL(table_format(), count, f->t->tf);
		slot += f->count;
		if (gc->hash_len == 0 && f->count > 0)
		{
			ProllyHash	h;
			uint64		o;
			uint32		l;

			PROLLY_CALL(table_format(), entry, f->t->tf, 0, &h, &o, &l);
			gc->hash_len = h.len;
		}
	}
	gc->nfiles = n;
	gc->nslots = slot;
	gc->appended = palloc0(sizeof(Segment));
	gc->appended->id = gc->segment;
	gc->appended->path = segment_path(s->dboid, s->spcoid, gc->segment);
	gc->appended->file = -1;
	gc->appended->scanned = JOURNAL_HEADER_SIZE;
	s->gc = gc;

	*root = gc->m.root;
	if (root->len > 0)
		gc->hash_len = root->len;
	*nslots = gc->nslots;
	pfree(dir);
	MemoryContextSwitchTo(old);
	return gc;
}

static void
journal_gc_files(ProllyChunkGc *gc, ProllyChunkGcFileVisit visit, void *arg)
{
	for (int i = 0; i < gc->nfiles; i++)
		visit(i, gc->files[i].first, gc->files[i].count, gc->files[i].settled,
			  gc->files[i].size, arg);
}

static int64
journal_gc_find(ProllyChunkGc *gc, const ProllyHash *hash)
{
	ProllyHash	key;
	GcSlot	   *e;

	hash_key(&key, hash);
	e = hash_search(gc->slots, &key, HASH_FIND, NULL);
	if (e != NULL)
		return e->slot;
	for (int i = 0; i < gc->nfiles; i++)
	{
		GcFile	   *f = &gc->files[i];
		int			at;

		if (f->segment)
			continue;
		at = PROLLY_CALL(table_format(), locate, f->t->tf, &key);
		if (at >= 0)
			return f->first + at;
	}
	return -1;
}

static void
journal_gc_read(ProllyChunkGc *gc, uint64 slot, StringInfo out)
{
	GcFile	   *f = slot_file(gc, slot);
	uint64		i = slot - f->first;
	char	   *bytes;
	uint64		offset;
	uint32		len;

	if (f->segment)
	{
		offset = f->offsets[i];
		len = f->lens[i];
	}
	else
	{
		ProllyHash	h;

		PROLLY_CALL(table_format(), entry, f->t->tf, (int) i, &h, &offset, &len);
	}
	bytes = palloc(len);
	if (FileRead(f->segment ? f->file : f->t->file, bytes, len, offset,
				 WAIT_EVENT_DATA_FILE_READ) != len)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read %u bytes at %llu in file \"%s\": %m",
						len, (unsigned long long) offset, f->path)));
	if (f->segment)
		chunk_bytes(f->path, bytes, len, out);
	else
		PROLLY_CALL(table_format(), decode, f->t->tf, bytes, len, out);
	pfree(bytes);
}

/*
 * The root, and the chunks appended since the seal (or the last call).
 * The first call waits for the transactions that appended to the sealed
 * segments and were in progress at the seal: then their PADs are ROOTs or
 * left, and their chunks committed or not, for good.  With window, appends
 * are held off from here until the swap.
 */
static void
journal_gc_new(ProllyChunkGc *gc, bool window, ProllyChunkGcVisit visit,
			   void *arg)
{
	ProllyChunkStore *s = gc->s;
	Segment    *g = gc->appended;
	uint64		size;
	uint64		offset;
	char	   *rec = NULL;
	Size		reclen = 0;
	ListCell   *lc;

	if (!gc->waited)
	{
		foreach(lc, gc->xids)
			XactLockTableWait(lfirst_xid(lc), NULL, NULL, XLTW_None);
		gc->waited = true;
	}
	if (window && !gc->window)
	{
		INJECTION_POINT("prolly-gc-window", NULL);
		LWLockAcquire(ProllyJournalLock, LW_EXCLUSIVE);
		gc->window = true;
	}

	scan_journal(s);
	if (s->root.len > 0)
		visit(&s->root, NULL, 0, arg);

	if (!open_segment(g))
		return;
	size = FileSize(g->file);
	offset = g->scanned;
	while (offset + RECORD_OVERHEAD <= size)
	{
		uint32		len;
		uint8		kind;
		ProllyHash	h;
		Size		after;
		TransactionId xid;

		read_exact(g, (char *) &len, sizeof(len), offset);
		if (len < RECORD_OVERHEAD || offset + len > size)
			break;
		if (len > reclen)
		{
			reclen = Max(len, 8192);
			rec = rec ? repalloc(rec, reclen) : palloc(reclen);
		}
		read_exact(g, rec, len, offset);
		if (!record_valid(rec, len))
			break;
		kind = (uint8) rec[sizeof(uint32)];
		if ((kind == KIND_CHUNK || kind == KIND_XCHUNK) &&
			record_hash_xid(rec, len, &h, &after, &xid))
			visit(&h, rec + after, len - after - sizeof(uint32), arg);
		else if (kind == KIND_ROOT && record_hash(rec, len, &h, &after))
			visit(&h, NULL, 0, arg);
		offset += len;
	}
	g->scanned = offset;
	if (rec)
		pfree(rec);
}

/* gc_copy()'s and gc_swap()'s source: the marked slots not yet written */
typedef struct GcCopy
{
	ProllyChunkGc *gc;
	const uint8 *marks;
	const bool *keep;
	uint64		next;
	const ProllyHashRoutine *hash;
} GcCopy;

static bool
gc_copy_next(void *arg, ProllyHash *hash, StringInfo data)
{
	GcCopy	   *gcc = arg;
	ProllyChunkGc *gc = gcc->gc;

	while (gcc->next < gc->nslots)
	{
		uint64		slot = gcc->next++;
		GcFile	   *f;
		ProllyHashCtx *ctx;

		if (!marked(gcc->marks, slot) || marked(gc->copied, slot))
			continue;
		f = slot_file(gc, slot);
		if (gcc->keep[f - gc->files])
			continue;
		gc->copied[slot / 8] |= 1 << (slot % 8);
		journal_gc_read(gc, slot, data);

		/* checked as it is copied: its bytes are its hash's */
		ctx = PROLLY_CALL(gcc->hash, begin, CurrentMemoryContext);
		PROLLY_CALL(gcc->hash, update, ctx, (const uint8 *) data->data, data->len);
		PROLLY_CALL(gcc->hash, finish, ctx, hash);
		if (journal_gc_find(gc, hash) != (int64) slot)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("chunk in file \"%s\" doesn't match its hash",
							f->path)));
		return true;
	}
	return false;
}

/* Write the marked slots not yet written (but kept files') to a new file. */
static void
gc_write(ProllyChunkGc *gc, const uint8 *marks, const bool *keep)
{
	GcCopy		gcc = {gc, marks, keep, 0, prolly_hash_lookup("sha256")};
	uint32		nchunks;
	uint64		size;

	if (gc->copied == NULL)
		gc->copied = MemoryContextAllocZero(gc->cxt, gc->nslots / 8 + 1);
	if (write_file(gc->s, gc->next_id, gc_copy_next, &gcc, &nchunks, &size))
	{
		ManifestFile *f = MemoryContextAlloc(gc->cxt, sizeof(ManifestFile));
		MemoryContext old = MemoryContextSwitchTo(gc->cxt);

		f->id = gc->next_id++;
		f->nchunks = nchunks;
		f->size = size;
		f->flags = MANIFEST_FILE_SETTLED;
		gc->written = lappend(gc->written, f);
		MemoryContextSwitchTo(old);
	}
}

static int
hash_bytes_cmp(const void *a, const void *b, void *arg)
{
	return memcmp(a, b, *(uint8 *) arg);
}

/*
 * The live chunks written, before the window; and the list of the chunks
 * the swap will drop (those not marked yet: the window may keep some of
 * them after all), for transactions running at the swap to check what
 * they append against.
 */
static void
journal_gc_copy(ProllyChunkGc *gc, const uint8 *marks, const bool *keep)
{
	char	   *path = dropped_path(gc->s->dboid, gc->s->spcoid,
									gc->m.gc_generation + 1);
	char	   *hashes;
	uint64		n = 0;
	int			fd;

	gc_write(gc, marks, keep);
	INJECTION_POINT("prolly-gc-copied", NULL);

	hashes = MemoryContextAllocHuge(gc->cxt, (gc->nslots + 1) * (Size) gc->hash_len);
	for (uint64 slot = 0; slot < gc->nslots; slot++)
	{
		GcFile	   *f;
		ProllyHash	h;
		int64		live;

		if (marked(marks, slot))
			continue;
		f = slot_file(gc, slot);
		if (keep[f - gc->files])
			continue;
		if (f->segment)
		{
			char		head[RECORD_OVERHEAD + sizeof(TransactionId) + 1 + PROLLY_HASH_MAX_LEN];
			uint32		len = Min(f->lens[slot - f->first], sizeof(head));
			Size		after;
			TransactionId xid;

			if (FileRead(f->file, head, len, f->offsets[slot - f->first],
						 WAIT_EVENT_DATA_FILE_READ) != len ||
				!record_hash_xid(head, f->lens[slot - f->first], &h, &after, &xid))
				continue;
		}
		else
		{
			uint64		o;
			uint32		l;

			PROLLY_CALL(table_format(), entry, f->t->tf, (int) (slot - f->first),
						&h, &o, &l);
		}
		if (h.len != gc->hash_len)
			continue;
		/* (another copy of it may be live) */
		live = journal_gc_find(gc, &h);
		if (live >= 0 && marked(marks, live))
			continue;
		memcpy(hashes + n * gc->hash_len, h.bytes, gc->hash_len);
		n++;
	}
	qsort_arg(hashes, n, gc->hash_len, hash_bytes_cmp, &gc->hash_len);
	fd = OpenTransientFile(path, O_RDWR | O_CREAT | O_TRUNC | PG_BINARY);
	if (fd < 0 ||
		write(fd, &gc->hash_len, 1) != 1 ||
		(n > 0 && write(fd, hashes, n * gc->hash_len) != n * gc->hash_len) ||
		CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", path)));
	pfree(hashes);
	pfree(path);
}

/*
 * In the window: the slots marked since gc_copy() written, then the
 * manifest replaced: the new files and those kept (settled ones a default
 * collection didn't collect, files a full one kept whole), the segment
 * appends go to, what was collected and not kept retiring, the next
 * generation.  Appends go on.
 */
static void
journal_gc_swap(ProllyChunkGc *gc, const uint8 *marks, const bool *keep,
				ProllyGcResult *result)
{
	ProllyChunkStore *s = gc->s;
	Manifest	nm;
	ListCell   *lc;
	uint64		freed_bytes = 0;
	uint64		written_bytes = 0;

	Assert(gc->window);
	gc_write(gc, marks, keep);

	memset(&nm, 0, sizeof(nm));
	nm.gc_generation = gc->m.gc_generation + 1;
	nm.last_id = gc->next_id - 1;
	nm.files = palloc(sizeof(ManifestFile) *
					  (gc->m.nfiles + list_length(gc->written) + 1));
	nm.retiring = palloc(sizeof(uint64) * (gc->nfiles + 1));
	for (int i = 0; i < gc->m.nfiles; i++)
	{
		bool		collected = false;
		bool		kept = false;

		for (int j = 0; j < gc->nfiles; j++)
		{
			if (!gc->files[j].segment && gc->files[j].id == gc->m.files[i].id)
			{
				collected = true;
				kept = keep[j];
			}
		}
		if (!collected || kept)
			nm.files[nm.nfiles++] = gc->m.files[i];
	}
	foreach(lc, gc->written)
	{
		ManifestFile *f = lfirst(lc);

		nm.files[nm.nfiles++] = *f;
		written_bytes += f->size;
	}
	for (int j = 0; j < gc->nfiles; j++)
	{
		if (keep[j])
			continue;
		nm.retiring[nm.nretiring++] = gc->files[j].id;
		freed_bytes += gc->files[j].size;
	}
	nm.nsegments = 1;
	nm.segments = &gc->segment;
	scan_journal(s);
	nm.root = s->root;
	replace_manifest(s, &nm, XLOG_CHUNKS_SWAP);
	LWLockRelease(ProllyJournalLock);
	gc->window = false;
	unlock_manifest(s);
	gc->readers = GetCurrentVirtualXIDs(InvalidTransactionId, false, false, 0,
										&gc->nreaders);
	load_tables(s, true);

	if (result)
	{
		memset(result, 0, sizeof(*result));
		for (uint64 slot = 0; slot < gc->nslots; slot++)
		{
			if (keep[slot_file(gc, slot) - gc->files])
				continue;
			if (marked(marks, slot))
				result->chunks_kept++;
			else
				result->chunks_freed++;
		}
		result->bytes_freed = freed_bytes > written_bytes ?
			freed_bytes - written_bytes : 0;
	}
	INJECTION_POINT("prolly-gc-swapped", NULL);
}

/* Close what a collection opened; its memory goes with its context. */
static void
gc_abort(ProllyChunkGc *gc)
{
	for (int i = 0; i < gc->nfiles; i++)
	{
		if (gc->files[i].segment && gc->files[i].file >= 0)
			FileClose(gc->files[i].file);
		else if (gc->files[i].t != NULL)
		{
			if (gc->files[i].t->tf)
				PROLLY_CALL(table_format(), close, gc->files[i].t->tf);
			FileClose(gc->files[i].t->file);
		}
	}
	if (gc->appended != NULL && gc->appended->file >= 0)
		FileClose(gc->appended->file);
	gc->s->gc = NULL;
}

/*
 * Once every transaction running at the swap has ended, what it stopped
 * naming is removed.
 */
static void
journal_gc_retire(ProllyChunkGc *gc)
{
	Manifest	m;

	wait_for_readers(gc->readers, gc->nreaders);
	INJECTION_POINT("prolly-gc-retire", NULL);
	lock_manifest(gc->s, &m);
	retire(gc->s, &m);
	gc_abort(gc);
	MemoryContextDelete(gc->cxt);
}

/*
 * A transaction running at a swap appends chunks naming none the swap
 * dropped: each hash a pending chunk holds (written, as the store's
 * chunks write them, as its length, then the hash) looked up in the lists
 * of what each swap since the transaction began dropped.
 */
static void
check_dropped(ProllyChunkStore *s)
{
	for (uint32 gen = s->xact_generation + 1; gen <= s->gc_generation; gen++)
	{
		char	   *path = dropped_path(s->dboid, s->spcoid, gen);
		int			fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
		struct stat st;
		char	   *list;
		uint8		hlen;
		Size		n;
		HASH_SEQ_STATUS status;
		IndexEntry *e;

		if (fd < 0 || fstat(fd, &st) != 0 || st.st_size < 1)
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("the chunk store was collected while the transaction ran"),
					 errhint("Try the transaction again.")));
		list = palloc(st.st_size);
		if (read(fd, list, st.st_size) != st.st_size)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m", path)));
		CloseTransientFile(fd);
		hlen = (uint8) list[0];
		n = hlen > 0 ? (st.st_size - 1) / hlen : 0;

		hash_seq_init(&status, s->pending_index);
		while ((e = hash_seq_search(&status)) != NULL)
		{
			const char *rec = s->pending.data + e->offset;
			ProllyHash	h;
			Size		after;
			const char *p;
			const char *end;

			if (!record_hash(rec, e->len, &h, &after))
				continue;
			p = rec + after;
			end = rec + e->len - sizeof(uint32);
			for (; n > 0 && p + 1 + hlen <= end; p++)
			{
				if ((uint8) *p != hlen ||
					bsearch_arg(p + 1, list + 1, n, hlen, hash_bytes_cmp, &hlen) == NULL)
					continue;
				hash_seq_term(&status);
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("could not store chunk %s: a chunk it names was collected while the transaction ran",
								prolly_hash_hex(&h)),
						 errhint("Try the transaction again.")));
			}
		}
		pfree(list);
		pfree(path);
	}
}

static Size
journal_shmem_size(void)
{
	return sizeof(JournalShared);
}

static void
journal_shmem_init(void *area)
{
	JournalShared *js = area;

	/* every backend reads the manifests once */
	pg_atomic_init_u64(&js->manifests, 1);
}

/*
 * A new database's table files and manifest: those of the database it was
 * made from, each file copied as a table file is written (WAL-logged a
 * piece at a time), then the manifest naming them.
 */
static void
copy_table_files(Oid src_dboid, Oid src_spcoid, Oid dst_dboid, Oid dst_spcoid)
{
	char	   *sdir = GetDatabasePath(src_dboid, src_spcoid);
	char	   *ddir = GetDatabasePath(dst_dboid, dst_spcoid);
	ProllyChunkStore *dst = palloc0(sizeof(ProllyChunkStore));
	char	   *buf;
	Manifest	m;

	if (!read_manifest(sdir, &m))
		return;
	dst->dboid = dst_dboid;
	dst->spcoid = dst_spcoid;
	buf = palloc(APPEND_BATCH);
	for (int i = 0; i < m.nfiles; i++)
	{
		char	   *spath = table_file_path(sdir, m.files[i].id);
		char	   *dpath = table_file_path(ddir, m.files[i].id);
		int			fd = OpenTransientFile(spath, O_RDONLY | PG_BINARY);
		int			dfd;

		if (fd < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", spath)));
		dfd = OpenTransientFile(dpath, O_RDWR | O_CREAT | O_TRUNC | PG_BINARY);
		if (dfd < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", dpath)));
		for (uint64 offset = 0; offset < m.files[i].size;)
		{
			Size		len = Min((uint64) APPEND_BATCH, m.files[i].size - offset);

			if (pg_pread(fd, buf, len, offset) != len)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not read %zu bytes at %llu in file \"%s\": %m",
								len, (unsigned long long) offset, spath)));
			table_file_write(dst, dfd, dpath, m.files[i].id, offset, buf, len);
			offset += len;
		}
		if (CloseTransientFile(fd) != 0 || CloseTransientFile(dfd) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not close file \"%s\": %m", dpath)));
		fsync_fname(dpath, false);
		pfree(spath);
		pfree(dpath);
	}
	m.nretiring = 0;		/* (the source's, never copied) */
	replace_manifest(dst, &m, XLOG_CHUNKS_MANIFEST);
	pfree(buf);
	pfree(dst);
	pfree(sdir);
	pfree(ddir);
}

/*
 * A new database's store: the files of the database it was made from.
 * The journal's segments are copied a piece at a time as appends to the new
 * one are (WAL-logged and flushed, then written), and the table files and
 * manifest as they are written, so redo, standbys and base backups make
 * them too.  One whose files were copied (the file-copy strategy) has them
 * already.  Nobody else uses the source meanwhile (CREATE DATABASE sees to
 * that), so its files hold still.
 */
static void
journal_copy_database(Oid src_dboid, Oid src_spcoid, Oid dst_dboid,
					  Oid dst_spcoid)
{
	char	   *sdir = GetDatabasePath(src_dboid, src_spcoid);
	char	   *ddir = GetDatabasePath(dst_dboid, dst_spcoid);
	char	   *dseg = segment_path(dst_dboid, dst_spcoid, 0);
	char	   *dman = dir_file(ddir, MANIFEST_FILE);
	struct stat st;
	char	   *buf;
	Manifest	m;
	bool		copied = stat(dseg, &st) == 0 || stat(dman, &st) == 0;

	pfree(dseg);
	pfree(dman);
	if (copied)
		return;
	copy_table_files(src_dboid, src_spcoid, dst_dboid, dst_spcoid);
	(void) read_manifest(sdir, &m);
	buf = palloc(APPEND_BATCH);
	for (int i = 0; i < m.nsegments; i++)
	{
		char	   *src = segment_path(src_dboid, src_spcoid, m.segments[i]);
		char	   *dst = segment_path(dst_dboid, dst_spcoid, m.segments[i]);
		uint64		offset = 0;
		int			fd;
		int			dfd;

		if (stat(src, &st) != 0)
		{
			pfree(src);
			pfree(dst);
			continue;
		}
		fd = OpenTransientFile(src, O_RDONLY | PG_BINARY);
		if (fd < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", src)));
		dfd = OpenTransientFile(dst, O_RDWR | O_CREAT | PG_BINARY);
		if (dfd < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", dst)));
		while (offset < (uint64) st.st_size)
		{
			Size		len = Min((uint64) APPEND_BATCH, (uint64) st.st_size - offset);

			if (pg_pread(fd, buf, len, offset) != len)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not read %zu bytes at %llu in file \"%s\": %m",
								len, (unsigned long long) offset, src)));
			logged_write(dst_dboid, dst_spcoid, m.segments[i], offset, buf, len,
						 -1, dfd, dst);
			offset += len;
		}
		if (CloseTransientFile(fd) != 0 || CloseTransientFile(dfd) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not close file \"%s\": %m", src)));
		fsync_fname(dst, false);
		pfree(src);
		pfree(dst);
	}
	pfree(buf);
	pfree(sdir);
	pfree(ddir);
}

const ProllyChunkStoreRoutine prolly_chunkstore_journal = {
	.info = {
		.iface = PROLLY_IFACE_CHUNK_STORE,
		.impl_id = 1,
		.format_version = JOURNAL_VERSION,
		.name = "journal",
		.description = "an append-only journal of chunk records, and table "
		"files, per database",
	},
	.open = journal_open,
	.put = journal_put,
	.get = journal_get,
	.has = journal_has,
	.root = journal_root,
	.move_root = journal_move_root,
	.commit_payload = journal_commit_payload,
	.committed = journal_committed,
	.commit_redo = journal_commit_redo,
	.checkpoint = journal_checkpoint,
	.redo = journal_redo,
	.add_table_file = journal_add_table_file,
	.conjoin = journal_conjoin,
	.table_files = journal_table_files,
	.shmem_size = journal_shmem_size,
	.shmem_init = journal_shmem_init,
	.copy_database = journal_copy_database,
	.read_through = journal_read_through,
	.prefetch = journal_prefetch,
	.seal = journal_seal,
	.segments = journal_segments,
	.gc_begin = journal_gc_begin,
	.gc_files = journal_gc_files,
	.gc_find = journal_gc_find,
	.gc_read = journal_gc_read,
	.gc_new = journal_gc_new,
	.gc_copy = journal_gc_copy,
	.gc_swap = journal_gc_swap,
	.gc_retire = journal_gc_retire,
};
