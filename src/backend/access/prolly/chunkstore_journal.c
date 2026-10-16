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
 *		ROOT	uint8 hash length, the hash: the store's root from here on
 *		PAD		room for a ROOT record, which its transaction's commit
 *				writes over it (a PAD left means the transaction aborted)
 *
 * Appending.  A backend gathers its transaction's chunks and appends them
 * in batches, and the rest before it commits, under ProllyJournal: a
 * batch's bytes are WAL-logged, the log is flushed, and only then are they
 * written at the file's end.  So the file never holds a byte the WAL
 * doesn't, and redo rewrites whatever a crash lost.  A checkpoint waits for
 * an append in progress (DELAY_CHKPT_START) and fsyncs the journals.
 *
 * Reading.  Each backend indexes the records by hash in memory, scanning
 * the file from where it left off when a lookup misses or the root is
 * asked for.  A record not completely written yet ends the scan, until the
 * next one.
 *
 * The root.  A move locks the store until its transaction ends, so moves
 * are serialized, and they reach the file in commit order: at pre-commit a
 * PAD is appended for the new ROOT record; the commit record carries its
 * offset and the root; once the commit record is flushed, still within the
 * commit's critical section, the ROOT record is written over the PAD (redo
 * of the commit record writes it too).  The root is the last ROOT record.
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
#include "access/prolly_xlog.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "common/relpath.h"
#include "miscadmin.h"
#include "port/pg_crc32c.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

#define JOURNAL_FILE		"prolly_journal"
#define JOURNAL_MAGIC		"PRLYJRN1"
#define JOURNAL_VERSION		1
#define JOURNAL_HEADER_SIZE	16	/* magic, version, reserved */

#define KIND_CHUNK			1
#define KIND_ROOT			2
#define KIND_PAD			3

/* len, kind ... crc */
#define RECORD_OVERHEAD		(sizeof(uint32) + 1 + sizeof(uint32))

/* a backend appends its chunks in batches of about this much */
#define APPEND_BATCH		(1024 * 1024)

/* the root lock: an object of the database no catalog has */
#define ROOT_LOCK_CLASSID	((Oid) 0x504A5254)

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

	/* this transaction's */
	StringInfoData pending;		/* records not appended yet */
	HTAB	   *pending_index;
	bool		moving;
	ProllyHash	new_root;
	List	   *saved;			/* MoveSave *, innermost last */
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

extern const ProllyChunkStoreRoutine prolly_chunkstore_journal;

static ProllyChunkStore *session_store = NULL;

static bool ensure_journal(ProllyChunkStore *s, bool create);

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

/* The hash a CHUNK or ROOT record's payload starts with. */
static bool
record_hash(const char *rec, uint32 len, ProllyHash *hash, Size *after)
{
	const char *payload = rec + sizeof(uint32) + 1;
	uint8		hlen = (uint8) payload[0];

	if (hlen == 0 || hlen > PROLLY_HASH_MAX_LEN ||
		sizeof(uint32) + 1 + 1 + hlen + sizeof(uint32) > len)
		return false;
	memset(hash, 0, sizeof(ProllyHash));
	hash->len = hlen;
	memcpy(hash->bytes, payload + 1, hlen);
	*after = sizeof(uint32) + 1 + 1 + hlen;
	return true;
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
		if (kind == KIND_CHUNK && record_hash(rec, len, &h, &after))
		{
			IndexEntry *e;
			bool		found;
			ProllyHash	key;

			hash_key(&key, &h);
			e = hash_search(s->index, &key, HASH_ENTER, &found);
			if (!found)
			{
				e->offset = offset;
				e->len = len;
			}
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

/* Append the transaction's pending chunks. */
static void
flush_pending(ProllyChunkStore *s)
{
	HASH_SEQ_STATUS status;
	IndexEntry *e;
	uint64		base;

	if (s->pending.len == 0)
		return;
	(void) ensure_journal(s, true);
	base = append_bytes(s, s->pending.data, s->pending.len, false);

	/* the chunks are in the file now */
	hash_seq_init(&status, s->pending_index);
	while ((e = hash_seq_search(&status)) != NULL)
	{
		IndexEntry *f;
		bool		found;

		f = hash_search(s->index, &e->hash, HASH_ENTER, &found);
		if (!found)
		{
			f->offset = base + e->offset;
			f->len = e->len;
		}
		hash_search(s->pending_index, &e->hash, HASH_REMOVE, NULL);
	}
	resetStringInfo(&s->pending);
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
			break;
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
			discard_pending(s);
			s->saved = NIL;
			break;
	}
}

/*
 * A subtransaction's root moves go with it: an aborted one's are undone
 * (its lock on the root, if it took it, is released with it); a committed
 * one's are its parent's.  Its chunks may stay, unreachable.
 */
static void
journal_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
						 SubTransactionId parentSubid, void *arg)
{
	ProllyChunkStore *s = session_store;
	int			level = GetCurrentTransactionNestLevel();
	MoveSave   *last;

	if (s == NULL || s->saved == NIL)
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
	uint8		head[1 + PROLLY_HASH_MAX_LEN];
	Size		headlen;
	uint64		at;

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
		hash_search(s->index, &key, HASH_FIND, NULL) != NULL)
		return;

	hash_payload(hash, head, &headlen);
	at = s->pending.len;
	record_append(&s->pending, KIND_CHUNK, head, headlen, data, len);
	e = hash_search(s->pending_index, &key, HASH_ENTER, &found);
	e->offset = at;
	e->len = s->pending.len - at;

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
	e = hash_search(s->index, &key, HASH_FIND, NULL);
	if (e == NULL)
	{
		scan_journal(s);
		e = hash_search(s->index, &key, HASH_FIND, NULL);
		if (e == NULL)
			return false;
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
		hash_search(s->index, &key, HASH_FIND, NULL) != NULL)
		return true;
	scan_journal(s);
	return hash_search(s->index, &key, HASH_FIND, NULL) != NULL;
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

static void
journal_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	xl_chunks_append xlrec;
	char	   *path;

	if ((info & PROLLY_XLOG_TYPE_MASK) != XLOG_CHUNKS_APPEND)
		elog(PANIC, "chunk journal: unknown record type %u", info);
	memcpy(&xlrec, XLogRecGetData(record), SizeOfChunksAppend);
	path = journal_path(xlrec.dboid, xlrec.spcoid);
	write_at(path, XLogRecGetData(record) + SizeOfChunksAppend,
			 XLogRecGetDataLen(record) - SizeOfChunksAppend, xlrec.offset);
	pfree(path);
}

/* fsync the journal in database directory dir, if it has one */
static void
sync_journal_in(const char *dir)
{
	char	   *path = psprintf("%s/%s", dir, JOURNAL_FILE);
	struct stat st;

	if (stat(path, &st) == 0)
		fsync_fname(path, false);
	pfree(path);
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

const ProllyChunkStoreRoutine prolly_chunkstore_journal = {
	.info = {
		.iface = PROLLY_IFACE_CHUNK_STORE,
		.impl_id = 1,
		.format_version = JOURNAL_VERSION,
		.name = "journal",
		.description = "an append-only journal of chunk records per database",
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
};
