/*-------------------------------------------------------------------------
 *
 * chunkstore_journal.c
 *	  Chunk store: an append-only journal per database.
 *
 * A database's chunks are records in one file, prolly_journal, in its
 * directory in its default tablespace: a header, then records
 *
 *		uint32 len (of the whole record), uint8 kind, payload,
 *		uint32 CRC-32C of kind and payload
 *
 *		CHUNK	uint8 hash length, the hash, the chunk's bytes
 *		XCHUNK	uint32 xid, then as CHUNK: a chunk of the transaction
 *				xid, which others don't see while it is in progress
 *		ROOT	uint8 hash length, the hash: the store's root from here on
 *		PAD		room for a ROOT record, which its transaction's commit
 *				writes over it (a PAD left means the transaction aborted)
 *
 * Appending.  A backend gathers its transaction's chunks and appends them
 * in batches, and the rest before it commits, under ProllyJournal: a
 * batch's bytes are WAL-logged, the log is flushed, and only then are they
 * written at the file's end.  So the file never holds a byte the WAL
 * doesn't, and redo rewrites whatever a crash lost.  A checkpoint waits for
 * an append in progress (DELAY_CHKPT_START) and fsyncs the journals.  A
 * batch is checked first: every chunk its chunks name is in the batch or
 * the store, or the transaction fails with a serialization failure, so the
 * store never has a chunk without the chunks it names.
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
 * offset and the root; once the commit record is flushed, still within the
 * commit's critical section, the ROOT record is written over the PAD (redo
 * of the commit record writes it too).  The root is the last ROOT record.
 *
 * Table files.  Chunks added together (received from another store, or
 * rewritten by conjoin()) go to an immutable table file of their own,
 * prolly_table_<id> (id in hex), in the table file format.  The manifest,
 * prolly_manifest, names the table files and the last id given, the root
 * as of its writing and the GC generation; it is replaced whole (written
 * aside, then renamed over the old one).  Every byte of a table file is
 * WAL-logged before it is written, as journal appends are; the file is
 * synced before the manifest naming it is logged; a manifest's record
 * carries its contents, and an unlink's names the file.  One writer of
 * table files at a time (a lock on the manifest, until its transaction
 * ends); a file whose writer failed before its manifest stays unnamed,
 * and the next writer, given the same id, writes over it.  Backends notice
 * a new manifest by a counter in shared memory, when a lookup misses.
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
#include "utils/hsearch.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

#define JOURNAL_FILE		"prolly_journal"
#define MANIFEST_FILE		"prolly_manifest"
#define MANIFEST_TEMP		"prolly_manifest.tmp"
#define MANIFEST_MAGIC		"PRLYMAN1"
#define MANIFEST_VERSION	1
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

/* WAL */
#define XLOG_CHUNKS_APPEND	0x10	/* bytes written at an offset */

typedef struct xl_chunks_append
{
	xl_prolly_header hdr;
	Oid			dboid;
	Oid			spcoid;
	uint64		offset;
	/* the bytes follow */
} xl_chunks_append;

#define SizeOfChunksAppend	(offsetof(xl_chunks_append, offset) + sizeof(uint64))

#define XLOG_CHUNKS_FILE		0x20	/* table file bytes written at an offset */
#define XLOG_CHUNKS_MANIFEST	0x30	/* the manifest replaced */
#define XLOG_CHUNKS_UNLINK		0x40	/* a table file removed */

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

/* the manifest: the table files, and the ids given */
typedef struct ManifestFile
{
	uint64		id;
	uint32		nchunks;
	uint64		size;
} ManifestFile;

typedef struct Manifest
{
	uint64		last_id;		/* table file ids given so far */
	uint32		gc_generation;
	ProllyHash	root;			/* as of the manifest's writing */
	int			nfiles;
	ManifestFile *files;
} Manifest;

/* a table file a backend reads */
typedef struct TableFile
{
	uint64		id;
	char	   *path;
	File		file;
	uint64		size;
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
	uint64		offset;			/* of the PAD the ROOT record goes over */
	ProllyHash	root;
} CommitPayload;

/* an index entry: where a chunk's record is */
typedef struct IndexEntry
{
	ProllyHash	hash;			/* key; unused bytes zero */
	uint64		offset;			/* of the record, or in pending */
	uint32		len;			/* of the record */
	TransactionId xid;			/* its transaction, while not known to have
								 * ended; invalid: everyone's */
	struct IndexEntry *other;	/* another copy, while this one isn't */
} IndexEntry;

struct ProllyChunkStore
{
	Oid			dboid;
	Oid			spcoid;
	char	   *path;
	File		file;

	HTAB	   *index;			/* chunks in the file, by hash */
	uint64		scanned;		/* the file is indexed up to here */
	List	   *pads;			/* offsets of PADs seen, which may become ROOTs */
	ProllyHash	root;
	uint64		root_offset;	/* of the last ROOT record seen */

	List	   *tables;			/* TableFile *, as the manifest last read named */
	uint64		manifests_seen; /* the shared count when it was read */

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

/* A ROOT record (or a PAD as long as one), in buf. */
static void
root_record(StringInfo buf, const ProllyHash *root, bool pad)
{
	uint8		payload[1 + PROLLY_HASH_MAX_LEN];
	Size		len;

	hash_payload(root, payload, &len);
	if (pad)
		memset(payload, 0, len);
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
index_add(HTAB *index, const ProllyHash *key, uint64 offset, uint32 len,
		  TransactionId xid)
{
	bool		found;
	IndexEntry *e = hash_search(index, key, HASH_ENTER, &found);
	IndexEntry *last;

	if (!found)
	{
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
 * The file
 * ----------------------------------------------------------------
 */

static void
read_exact(ProllyChunkStore *s, char *buf, Size len, uint64 offset)
{
	ssize_t		n = FileRead(s->file, buf, len, offset, WAIT_EVENT_DATA_FILE_READ);

	if (n != len)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read %zu bytes at %llu in file \"%s\": %m",
						len, (unsigned long long) offset, s->path)));
}

/*
 * Append bytes at the file's end: WAL-logged and flushed first, so the file
 * never holds what the WAL doesn't.  Returns the offset they went to.  The
 * header is appended to an empty file only (header true), and nothing else
 * is.
 */
static uint64
append_bytes(ProllyChunkStore *s, const char *data, Size len, bool header)
{
	xl_chunks_append xlrec;
	XLogRecPtr	lsn;
	pgoff_t		size;
	uint64		offset;
	ssize_t		n;

	LWLockAcquire(ProllyJournalLock, LW_EXCLUSIVE);
	size = FileSize(s->file);
	if (size < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not determine the size of file \"%s\": %m",
						s->path)));
	offset = size;
	if (header != (offset == 0))
	{
		LWLockRelease(ProllyJournalLock);
		if (header)
			return 0;			/* another backend wrote it */
		elog(ERROR, "chunk journal \"%s\" has no header", s->path);
	}

	xlrec.hdr.impl_id = prolly_chunkstore_journal.info.impl_id;
	xlrec.dboid = s->dboid;
	xlrec.spcoid = s->spcoid;
	xlrec.offset = offset;

	Assert((MyProc->delayChkptFlags & DELAY_CHKPT_START) == 0);
	START_CRIT_SECTION();
	MyProc->delayChkptFlags |= DELAY_CHKPT_START;
	XLogBeginInsert();
	XLogRegisterData(&xlrec, SizeOfChunksAppend);
	XLogRegisterData(data, len);
	lsn = XLogInsert(RM_PROLLY_CHUNKS_ID, XLOG_CHUNKS_APPEND);
	XLogFlush(lsn);
	n = FileWrite(s->file, data, len, offset, WAIT_EVENT_DATA_FILE_WRITE);
	if (n != len)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not write %zu bytes at %llu in file \"%s\": %m",
						len, (unsigned long long) offset, s->path)));
	MyProc->delayChkptFlags &= ~DELAY_CHKPT_START;
	END_CRIT_SECTION();
	LWLockRelease(ProllyJournalLock);
	return offset;
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

static char *
journal_path(Oid dboid, Oid spcoid)
{
	char	   *dir = GetDatabasePath(dboid, spcoid);
	char	   *path = psprintf("%s/%s", dir, JOURNAL_FILE);

	pfree(dir);
	return path;
}

/*
 * Index the records written since the last scan: up to the first that
 * isn't whole yet.  A PAD seen may since have become a ROOT.
 */
static void
scan_journal(ProllyChunkStore *s)
{
	uint64		size;
	uint64		offset = s->scanned;
	ListCell   *lc;
	List	   *still_pads = NIL;
	char	   *rec = NULL;
	Size		reclen = 0;

	if (!ensure_journal(s, false))
		return;
	size = FileSize(s->file);

	foreach(lc, s->pads)
	{
		uint64		pad = *(uint64 *) lfirst(lc);
		uint8		kind;

		read_exact(s, (char *) &kind, 1, pad + sizeof(uint32));
		if (kind == KIND_ROOT)
		{
			uint32		len;
			char		buf[RECORD_OVERHEAD + 1 + PROLLY_HASH_MAX_LEN];
			ProllyHash	h;
			Size		after;

			read_exact(s, (char *) &len, sizeof(len), pad);
			if (len <= sizeof(buf))
			{
				read_exact(s, buf, len, pad);
				if (record_valid(buf, len) && record_hash(buf, len, &h, &after) &&
					pad >= s->root_offset)
				{
					s->root = h;
					s->root_offset = pad;
				}
			}
			pfree(lfirst(lc));
		}
		else
			still_pads = lappend(still_pads, lfirst(lc));
	}
	list_free(s->pads);
	s->pads = still_pads;

	while (offset + RECORD_OVERHEAD <= size)
	{
		uint32		len;
		uint8		kind;
		ProllyHash	h;
		Size		after;
		TransactionId xid;

		read_exact(s, (char *) &len, sizeof(len), offset);
		if (len < RECORD_OVERHEAD || offset + len > size)
			break;
		if (len > reclen)
		{
			if (rec)
				pfree(rec);
			reclen = Max(len, 8192);
			rec = MemoryContextAlloc(TopMemoryContext, reclen);
		}
		read_exact(s, rec, len, offset);
		if (!record_valid(rec, len))
			break;
		kind = (uint8) rec[sizeof(uint32)];
		if ((kind == KIND_CHUNK || kind == KIND_XCHUNK) &&
			record_hash_xid(rec, len, &h, &after, &xid))
		{
			ProllyHash	key;

			hash_key(&key, &h);
			index_add(s->index, &key, offset, len, xid);
		}
		else if (kind == KIND_ROOT && record_hash(rec, len, &h, &after))
		{
			s->root = h;
			s->root_offset = offset;
		}
		else if (kind == KIND_PAD)
		{
			uint64	   *pad = MemoryContextAlloc(TopMemoryContext, sizeof(uint64));
			MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

			*pad = offset;
			s->pads = lappend(s->pads, pad);
			MemoryContextSwitchTo(old);
		}
		offset += len;
	}
	if (rec)
		pfree(rec);
	s->scanned = offset;
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
	uint64		base;

	if (s->pending.len == 0)
		return;
	check_pending(s);
	(void) ensure_journal(s, true);
	base = append_bytes(s, s->pending.data, s->pending.len, false);

	/* the chunks are in the file now */
	hash_seq_init(&status, s->pending_index);
	while ((e = hash_seq_search(&status)) != NULL)
	{
		index_add(s->index, &e->hash, base + e->offset, e->len, e->xid);
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
				s->pad_offset = append_bytes(s, pad.data, pad.len, false);
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
 * Open the journal, if it has one; with create, make it (with its header,
 * logged as any append) if not.  A store with none is empty.  In recovery
 * nothing is made: redo makes the journal, and the store reads as it is.
 */
static bool
ensure_journal(ProllyChunkStore *s, bool create)
{
	char		header[JOURNAL_HEADER_SIZE];

	if (s->file >= 0 && FileSize(s->file) >= JOURNAL_HEADER_SIZE)
		return true;
	if (create && RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("cannot change a chunk store during recovery")));
	if (s->file < 0)
	{
		s->file = PathNameOpenFile(s->path,
								   O_RDWR | (create ? O_CREAT : 0) | PG_BINARY);
		if (s->file < 0)
		{
			if (errno == ENOENT && !create)
				return false;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", s->path)));
		}
	}
	if (FileSize(s->file) < JOURNAL_HEADER_SIZE)
	{
		uint32		version = JOURNAL_VERSION;

		if (!create)
			return false;
		memset(header, 0, sizeof(header));
		memcpy(header, JOURNAL_MAGIC, 8);
		memcpy(header + 8, &version, sizeof(version));
		(void) append_bytes(s, header, sizeof(header), true);
	}
	read_exact(s, header, sizeof(header), 0);
	if (memcmp(header, JOURNAL_MAGIC, 8) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("file \"%s\" is not a chunk journal", s->path)));
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
	}
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, out->data, out->len);
	FIN_CRC32C(crc);
	append_le(out, crc, 4);
}

static bool read_manifest_at(const char *path, Manifest *m);

/*
 * Read the manifest in database directory dir into m; false (m empty) if
 * there is none.
 */
static bool
read_manifest(const char *dir, Manifest *m)
{
	char	   *path = dir_file(dir, MANIFEST_FILE);
	bool		found = read_manifest_at(path, m);

	pfree(path);
	return found;
}

/* Read the manifest at path into m; false (m empty) if there is none. */
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

	memset(m, 0, sizeof(*m));
	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	if (fd < 0)
	{
		if (errno == ENOENT)
			return false;
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
	if (version != MANIFEST_VERSION)
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
	if ((Size) (end - p) != (Size) nfiles * 20)
		goto corrupt;
	m->nfiles = nfiles;
	m->files = palloc(sizeof(ManifestFile) * Max(nfiles, 1));
	for (uint32 i = 0; i < nfiles; i++)
	{
		m->files[i].id = read_le(p, 8);
		m->files[i].nchunks = (uint32) read_le(p + 8, 4);
		m->files[i].size = read_le(p + 12, 8);
		p += 20;
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

/*
 * Replace the manifest of s's database with m, WAL-logged first.  A
 * checkpoint waits (DELAY_CHKPT_START), so one whose redo point follows
 * the record finds the new manifest in place.
 */
static void
replace_manifest(ProllyChunkStore *s, const Manifest *m)
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
	lsn = XLogInsert(RM_PROLLY_CHUNKS_ID, XLOG_CHUNKS_MANIFEST);
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

static void
table_file_unlink(ProllyChunkStore *s, const char *dir, uint64 id)
{
	xl_chunks_file xlrec;
	char	   *path = table_file_path(dir, id);

	xlrec.hdr.impl_id = prolly_chunkstore_journal.info.impl_id;
	xlrec.dboid = s->dboid;
	xlrec.spcoid = s->spcoid;
	xlrec.id = id;
	xlrec.offset = 0;
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
 * Write a table file of the chunks next() gives, and a manifest naming it
 * beside the files keep (of m) names; the files m names that keep leaves
 * out are removed after.  The manifest's lock is held.
 */
static void
write_table_file(ProllyChunkStore *s, Manifest *m, bool *keep,
				 ProllyChunkNext next, void *arg)
{
	const ProllyTableFileRoutine *fmt = table_format();
	char	   *dir = GetDatabasePath(s->dboid, s->spcoid);
	uint64		id = m->last_id + 1;
	char	   *path = table_file_path(dir, id);
	ProllyTableFileWriter *w;
	StringInfoData buf;
	StringInfoData data;
	ProllyHash	hash;
	uint64		offset = 0;
	uint32		nchunks = 0;
	int			fd;
	Manifest	nm;

	HASHCTL		ctl;
	HTAB	   *seen;

	initStringInfo(&data);
	if (!next(arg, &hash, &data))
	{
		pfree(data.data);
		pfree(path);
		pfree(dir);
		return;
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
	INJECTION_POINT("prolly-table-file-written", NULL);

	memset(&nm, 0, sizeof(nm));
	nm.last_id = id;
	nm.gc_generation = m->gc_generation;
	/* (the committed root: one the transaction moves to may roll back) */
	scan_journal(s);
	nm.root = s->root;
	nm.files = palloc(sizeof(ManifestFile) * (m->nfiles + 1));
	for (int i = 0; i < m->nfiles; i++)
	{
		if (keep[i])
			nm.files[nm.nfiles++] = m->files[i];
	}
	nm.files[nm.nfiles].id = id;
	nm.files[nm.nfiles].nchunks = nchunks;
	nm.files[nm.nfiles].size = offset;
	nm.nfiles++;
	replace_manifest(s, &nm);
	INJECTION_POINT("prolly-manifest-replaced", NULL);

	for (int i = 0; i < m->nfiles; i++)
	{
		if (!keep[i])
			table_file_unlink(s, dir, m->files[i].id);
	}
	load_tables(s, true);
	pfree(nm.files);
	pfree(buf.data);
	pfree(data.data);
	pfree(path);
	pfree(dir);
}

/*
 * Remove the table files of dir its manifest m doesn't name: those a
 * writer replaced but didn't get to remove (it failed, or the server
 * stopped, in between), or one it was writing.  With the manifest's lock
 * held, no writer is writing one now.
 */
static void
remove_unnamed(ProllyChunkStore *s, const char *dir, const Manifest *m)
{
	DIR		   *d = AllocateDir(dir);
	struct dirent *de;
	List	   *ids = NIL;
	ListCell   *lc;

	while ((de = ReadDir(d, dir)) != NULL)
	{
		uint64		id;
		char	   *end;
		bool		named = false;

		if (strncmp(de->d_name, "prolly_table_", 13) != 0)
			continue;
		id = strtou64(de->d_name + 13, &end, 16);
		if (*end != '\0')
			continue;
		for (int i = 0; i < m->nfiles; i++)
			named |= m->files[i].id == id;
		if (!named)
		{
			uint64	   *p = palloc(sizeof(uint64));

			*p = id;
			ids = lappend(ids, p);
		}
	}
	FreeDir(d);
	foreach(lc, ids)
		table_file_unlink(s, dir, *(uint64 *) lfirst(lc));
	list_free_deep(ids);
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
	s->path = journal_path(dboid, spcoid);
	ctl.keysize = sizeof(ProllyHash);
	ctl.entrysize = sizeof(IndexEntry);
	s->index = hash_create("prolly journal index", 1024, &ctl,
						   HASH_ELEM | HASH_BLOBS);
	s->pending_index = hash_create("prolly journal pending", 256, &ctl,
								   HASH_ELEM | HASH_BLOBS);
	initStringInfo(&s->pending);
	MemoryContextSwitchTo(old);

	s->file = -1;
	s->scanned = JOURNAL_HEADER_SIZE;
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

/* The chunk's bytes from its record (rec, len bytes) to out. */
static void
chunk_bytes(ProllyChunkStore *s, const char *rec, uint32 len, StringInfo out)
{
	ProllyHash	h;
	Size		after;

	if (!record_valid(rec, len) || !record_hash(rec, len, &h, &after))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupt chunk record in file \"%s\"", s->path)));
	appendBinaryStringInfo(out, rec + after, len - after - sizeof(uint32));
}

static bool
journal_get(ProllyChunkStore *s, const ProllyHash *hash, StringInfo out)
{
	ProllyHash	key;
	IndexEntry *e;
	char	   *rec;

	hash_key(&key, hash);
	e = hash_search(s->pending_index, &key, HASH_FIND, NULL);
	if (e != NULL)
	{
		chunk_bytes(s, s->pending.data + e->offset, e->len, out);
		return true;
	}
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
	rec = palloc(e->len);
	read_exact(s, rec, e->len, e->offset);
	chunk_bytes(s, rec, e->len, out);
	pfree(rec);
	return true;
}

static bool
journal_has(ProllyChunkStore *s, const ProllyHash *hash)
{
	ProllyHash	key;

	hash_key(&key, hash);
	if (hash_search(s->pending_index, &key, HASH_FIND, NULL) != NULL ||
		stored_has(s, &key))
		return true;
	return s->through != NULL && s->through(s->through_arg, hash, NULL);
}

static void
journal_root(ProllyChunkStore *s, ProllyHash *root)
{
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
	if (FileWrite(s->file, s->root_rec, s->root_rec_len, s->pad_offset,
				  WAIT_EVENT_DATA_FILE_WRITE) != s->root_rec_len)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not write the root in file \"%s\": %m", s->path)));
	s->root = s->new_root;
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
	path = journal_path(p.dboid, p.spcoid);
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
		   info == XLOG_CHUNKS_MANIFEST ? SizeOfChunksManifest : SizeOfChunksFile);
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
	else if (info == XLOG_CHUNKS_MANIFEST)
	{
		const char *data = XLogRecGetData(record) + SizeOfChunksManifest;
		Size		len = XLogRecGetDataLen(record) - SizeOfChunksManifest;
		char	   *temp = dir_file(dir, MANIFEST_TEMP);
		char	   *path = dir_file(dir, MANIFEST_FILE);
		Manifest	m;

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
	}
	else
	{
		char	   *path = table_file_path(dir, xlrec.id);

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
			path = journal_path(xlrec.dboid, xlrec.spcoid);
			write_at(path, XLogRecGetData(record) + SizeOfChunksAppend,
					 XLogRecGetDataLen(record) - SizeOfChunksAppend, xlrec.offset);
			pfree(path);
			break;
		case XLOG_CHUNKS_FILE:
		case XLOG_CHUNKS_MANIFEST:
		case XLOG_CHUNKS_UNLINK:
			table_files_redo(record);
			break;
		default:
			elog(PANIC, "chunk journal: unknown record type %u", info);
	}
}

/*
 * fsync the journal in database directory dir, if it has one, and every
 * table file there: those the manifest names (written by replay, unsynced),
 * one being written, and those a new database is being given, which no
 * manifest names yet.
 */
static void
sync_journal_in(const char *dir)
{
	char	   *path = psprintf("%s/%s", dir, JOURNAL_FILE);
	DIR		   *d;
	struct dirent *de;

	sync_if_exists(path);
	pfree(path);
	d = AllocateDir(dir);
	if (d == NULL)
		return;
	while ((de = ReadDir(d, dir)) != NULL)
	{
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

static void
journal_add_table_file(ProllyChunkStore *s, ProllyChunkNext next, void *arg)
{
	Manifest	m;
	bool	   *keep;

	lock_manifest(s, &m);
	keep = palloc(sizeof(bool) * Max(m.nfiles, 1));
	for (int i = 0; i < m.nfiles; i++)
		keep[i] = true;
	write_table_file(s, &m, keep, next, arg);
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

/* Rewrite the table files as one, which the manifest names alone. */
static void
journal_conjoin(ProllyChunkStore *s)
{
	Manifest	m;
	bool	   *keep;
	ConjoinSource cs;
	HASHCTL		ctl;
	char	   *dir;
	ListCell   *lc;

	lock_manifest(s, &m);
	if (m.nfiles < 2)
		return;

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
		write_table_file(s, &m, keep, conjoin_next, &cs);
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

		visit(name, PROLLY_CALL(table_format(), count, t->tf), t->size, arg);
		pfree(name);
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
	replace_manifest(dst, &m);
	pfree(buf);
	pfree(dst);
	pfree(sdir);
	pfree(ddir);
}

/*
 * A new database's store: the files of the database it was made from.
 * The journal is copied a piece at a time as appends to the new one are
 * (WAL-logged and flushed, then written), and the table files and manifest
 * as they are written, so redo, standbys and base backups make them too.
 * One whose files were copied (the file-copy strategy) has them already.
 * Nobody else uses the source meanwhile (CREATE DATABASE sees to that), so
 * its files hold still.
 */
static void
journal_copy_database(Oid src_dboid, Oid src_spcoid, Oid dst_dboid,
					  Oid dst_spcoid)
{
	char	   *src = journal_path(src_dboid, src_spcoid);
	char	   *dst = journal_path(dst_dboid, dst_spcoid);
	char	   *ddir = GetDatabasePath(dst_dboid, dst_spcoid);
	char	   *dman = dir_file(ddir, MANIFEST_FILE);
	struct stat st;
	int			fd;
	int			dfd;
	char	   *buf;
	uint64		offset = 0;
	bool		copied = stat(dst, &st) == 0 || stat(dman, &st) == 0;

	pfree(ddir);
	pfree(dman);
	if (copied)
	{
		pfree(src);
		pfree(dst);
		return;
	}
	copy_table_files(src_dboid, src_spcoid, dst_dboid, dst_spcoid);
	if (stat(src, &st) != 0)
	{
		pfree(src);
		pfree(dst);
		return;
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
	buf = palloc(APPEND_BATCH);
	while (offset < (uint64) st.st_size)
	{
		Size		len = Min((uint64) APPEND_BATCH, (uint64) st.st_size - offset);
		xl_chunks_append xlrec;
		XLogRecPtr	lsn;

		if (pg_pread(fd, buf, len, offset) != len)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read %zu bytes at %llu in file \"%s\": %m",
							len, (unsigned long long) offset, src)));
		xlrec.hdr.impl_id = prolly_chunkstore_journal.info.impl_id;
		xlrec.dboid = dst_dboid;
		xlrec.spcoid = dst_spcoid;
		xlrec.offset = offset;

		Assert((MyProc->delayChkptFlags & DELAY_CHKPT_START) == 0);
		START_CRIT_SECTION();
		MyProc->delayChkptFlags |= DELAY_CHKPT_START;
		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfChunksAppend);
		XLogRegisterData(buf, len);
		lsn = XLogInsert(RM_PROLLY_CHUNKS_ID, XLOG_CHUNKS_APPEND);
		XLogFlush(lsn);
		if (pg_pwrite(dfd, buf, len, offset) != len)
			ereport(PANIC,
					(errcode_for_file_access(),
					 errmsg("could not write %zu bytes at %llu in file \"%s\": %m",
							len, (unsigned long long) offset, dst)));
		MyProc->delayChkptFlags &= ~DELAY_CHKPT_START;
		END_CRIT_SECTION();
		offset += len;
	}
	if (CloseTransientFile(fd) != 0 || CloseTransientFile(dfd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", src)));
	fsync_fname(dst, false);
	pfree(buf);
	pfree(src);
	pfree(dst);
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
};
