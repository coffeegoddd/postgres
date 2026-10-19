/*-------------------------------------------------------------------------
 *
 * chunkstore_journal.c
 *	  Chunk store: an append-only journal per database.
 *
 * A database's chunks are records in its journal, in its directory in its
 * default tablespace: in segments, files of a header (the journal's version
 * and the store's format: the hash its chunks are named by, and their
 * format's version, which a server of another refuses), then records
 *
 *		uint32 len (of the whole record), uint8 kind, payload,
 *		uint32 CRC-32C of kind and payload
 *
 *		CHUNK	uint8 hash length, the hash, uint8 codec, the chunk as
 *				stored: its bytes (codec 0), or its length (uint32) and
 *				its bytes compressed with LZ4 (codec 1), when the server is
 *				built with LZ4 and that makes it smaller (a server built
 *				without reads the others and refuses those)
 *		XCHUNK	uint32 xid, then as CHUNK: a chunk of the transaction
 *				xid, which others don't see while it is in progress
 *		ROOT	uint8 hash length, the hash, then when the root was moved
 *				to it (int64, a TimestampTz): the store's root from here on
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
 * in batches, and the rest when it commits.  An append reserves its range
 * at the segment's end and WAL-logs its bytes under ProllyJournal, and
 * writes them outside it once the log is flushed: a batch at once (a flush
 * concurrent appends and commits share), a transaction's last append (its
 * remaining chunks and its PAD) once its commit record is flushed, so a
 * commit flushes the log once.  So a segment never holds a byte the WAL
 * doesn't, and redo rewrites whatever a crash lost.  The ranges reserved
 * and not written are listed in shared memory: a reader meeting one skips
 * it, and reads it once written; a checkpoint waits for them before it
 * fsyncs the segments; an abort writes its transaction's (the WAL has them,
 * a standby writes them).  A crash can leave a range the WAL never had at
 * a segment's end, which the next append after a restart cuts off.  A
 * batch is checked first: every chunk its chunks name is in the batch or
 * the store (as of a GC generation no swap ended before the append), or
 * the transaction fails with a serialization failure, so the store never
 * has a chunk without the chunks it names; so is the root a transaction
 * moves to, before its PAD.
 *
 * Reading.  Each backend indexes the records by hash in memory, scanning
 * the file from where it left off when a lookup misses or the root is
 * asked for.  A record not completely written yet ends the scan, until the
 * next one.  A chunk appended before its transaction commits (a batch, or
 * the rest at commit) is an XCHUNK, and isn't found by others while the
 * transaction is in progress: a chunk is others' once its transaction has
 * committed (or aborted: its bytes are whole, and anyone may use them).
 * Another transaction putting the same chunk meanwhile appends its own
 * copy; the index keeps every copy of a chunk until one is known to be
 * everyone's.
 *
 * The root.  A move locks the store until its transaction ends, so moves
 * are serialized, and they reach the file in commit order: as the commit
 * begins a PAD is appended for the new ROOT record; the commit record
 * carries its segment and offset and the root; once the commit record is
 * flushed, still within the commit's critical section, the append is
 * written with the ROOT record in the PAD's place (redo writes the PAD,
 * then the commit record's ROOT over it), in a segment sealed meanwhile
 * as in any.  So the range is in flight until it holds the ROOT, and a
 * collection's window, which waits for the appends in flight, never sees
 * the PAD of a transaction whose ROOT comes after it.  The root is the
 * last ROOT record of the segments named, in their order; if they have
 * none, the manifest's.
 *
 * Table files.  Chunks added together (received from another store, or
 * rewritten by conjoin()) go to an immutable table file of their own,
 * prolly_table_<id> (id in hex), in the table file format.  The manifest,
 * prolly_manifest, records the store's format, as a segment's header does,
 * and names the table files (each settled or not: closed
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
 * of its own, to the transaction's end; a second is refused, not left to
 * wait), never in a transaction that used the store, and abandoned with a
 * subtransaction that aborts; holding the manifest's lock from
 * its seal to its swap.  It seals the journal (a SEAL manifest record),
 * waits for the appends in flight to the sealed segments, and numbers the
 * chunks of the sealed segments and of the table files it collects (all,
 * or those not settled) as slots: a sealed segment's by a hash table of
 * them (what changed since the last collection), a table file's by its
 * index.  Transactions that appended to a sealed segment and were in
 * progress at the seal are waited for before anything new is read: then
 * their PADs are ROOTs or not, for good, and their chunks committed or
 * not.  The marked chunks are copied, each re-hashed, to a new settled
 * table file before the window; the window takes ProllyJournal, so no
 * append is reserved, and waits for those in flight to be written, while
 * the last new chunks are marked and copied and the
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
#ifdef USE_LZ4
#include <lz4.h>
#endif

#include "access/prolly_chunkstore.h"
#include "access/prolly_config.h"
#include "access/prolly_hash.h"
#include "access/prolly_shmem.h"
#include "access/prolly_tablefile.h"
#include "access/prolly_xlog.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "commands/vacuum.h"
#include "common/relpath.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/atomics.h"
#include "portability/instr_time.h"
#include "port/pg_crc32c.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/condition_variable.h"
#include "storage/procarray.h"
#include "storage/spin.h"
#include "replication/slot.h"
#include "storage/standby.h"
#include "utils/hsearch.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

#define JOURNAL_FILE		"prolly_journal"	/* segment 0's */
#define SEGMENT_PREFIX		"prolly_journal_"	/* the others', id in hex */
#define MANIFEST_FILE		"prolly_manifest"
#define MANIFEST_TEMP		"prolly_manifest.tmp"
#define MANIFEST_MAGIC		"PRLYMAN1"
#define MANIFEST_VERSION	4
#define TABLE_FILE_FORMAT	"basic"
#define JOURNAL_MAGIC		"PRLYJRN1"
#define JOURNAL_VERSION		4
#define JOURNAL_HEADER_SIZE	32	/* magic, version, the store's format */

/* the store's format, as a segment's header and the manifest hold it */
#define FORMAT_SIZE			(PROLLY_STORE_HASH_NAME_LEN + 1 + 2)
#define JOURNAL_FORMAT_AT	12	/* in the header, after magic and version */

#define KIND_CHUNK			1
#define KIND_ROOT			2
#define KIND_PAD			3
#define KIND_XCHUNK			4

/* how a chunk record holds its chunk */
#define CODEC_NONE			0
#define CODEC_LZ4			1
#define CODEC_HEAD_MAX		(1 + sizeof(uint32))	/* codec, length */

/* len, kind ... crc */
#define RECORD_OVERHEAD		(sizeof(uint32) + 1 + sizeof(uint32))

/* a ROOT record's (and its PAD's) length, at most */
#define ROOT_RECORD_MAX		(RECORD_OVERHEAD + 1 + PROLLY_HASH_MAX_LEN + \
							 sizeof(TimestampTz))

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

/* a table file's and a segment's entries in the manifest, in bytes */
#define MANIFEST_FILE_ENTRY		21	/* id, chunks, size, flags */
#define MANIFEST_SEGMENT_ENTRY	16	/* id, sealed at */

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
	XLogRecPtr *sealed_at;		/* each one's: the WAL's insert position as
								 * it was sealed, after every append to it
								 * (invalid: the last, or unknown) */
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

/*
 * A range of a segment reserved for an append whose bytes aren't written
 * yet: its backend's, by proc number (len 0: none).  gen counts the
 * reservations made in the slot, so a waiter can tell one it saw from a
 * later one.
 */
typedef struct AppendRange
{
	Oid			dboid;
	Oid			spcoid;
	uint64		segment;
	uint64		offset;
	uint64		len;
	uint64		gen;
} AppendRange;

/* a segment whose end was checked since the server started */
typedef struct CheckedEnd
{
	Oid			dboid;
	Oid			spcoid;
	uint64		segment;
} CheckedEnd;

#define CHECKED_ENDS		32

/*
 * The chunks named from outside the store (keep()) since a collection
 * began, a collection at a time per database, up to KEEP_COLLECTIONS at
 * once; past KEEP_MAX of them, overflow says the collection keeps all, as
 * does one that finds no set free.
 */
#define KEEP_COLLECTIONS	8
#define KEEP_MAX			4096

typedef struct KeepSet
{
	bool		active;
	bool		overflow;
	Oid			dboid;
	Oid			spcoid;
	int			n;
	ProllyHash	hashes[KEEP_MAX];
} KeepSet;

/* a transaction that told keep() of a chunk (xid invalid: none) */
typedef struct KeepTouch
{
	Oid			dboid;
	Oid			spcoid;
	TransactionId xid;
} KeepTouch;

/* shared by every backend */
typedef struct JournalShared
{
	pg_atomic_uint64 manifests;	/* manifests written or replayed */

	/* appends reserved and not written yet; under mutex */
	slock_t		mutex;
	ConditionVariable written;	/* an append's bytes were written */
	CheckedEnd	checked[CHECKED_ENDS];	/* under ProllyJournalLock */
	int			next_checked;
	int			inflight;		/* ranges of len > 0 */

	/* under mutex */
	KeepSet		keeps[KEEP_COLLECTIONS];
	pg_atomic_uint64 keeps_begun;	/* keep sets begun */

	/* a process's own, by proc number: ranges, then (touches()) touches */
	int			nranges;
	AppendRange ranges[FLEXIBLE_ARRAY_MEMBER];
} JournalShared;

#define TOUCHES_OFFSET(nranges) \
	MAXALIGN(offsetof(JournalShared, ranges) + (nranges) * sizeof(AppendRange))

static JournalShared *journal_shared = NULL;

/* what a root move's commit record carries */
typedef struct CommitPayload
{
	Oid			dboid;
	Oid			spcoid;
	uint64		segment;		/* the PAD's */
	uint64		offset;			/* of the PAD the ROOT record goes over */
	ProllyHash	root;
	TimestampTz at;				/* when: the ROOT record's time */
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
	List	   *holes;			/* Hole *: ranges before scanned not indexed,
								 * their appends in flight when scanned */
} Segment;

/* a range of a segment reserved for an append and not written when seen */
typedef struct Hole
{
	uint64		offset;
	uint64		len;
} Hole;

/*
 * An append reserved: its range of a segment, and the WAL record that holds
 * its bytes.
 */
typedef struct Append
{
	Segment    *g;
	uint64		offset;
	Size		len;
	XLogRecPtr	lsn;
} Append;

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

	/*
	 * Reserved as the commit begins: the rest of pending (and the PAD, if
	 * moving), written once the commit record is flushed, or at abort.
	 */
	bool		at_commit;
	Append		commit_append;
	/* the ROOT record, made before the commit's critical section */
	char		root_rec[ROOT_RECORD_MAX];
	uint32		root_rec_len;
	TimestampTz root_at;		/* the time root_rec records */
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
static void check_dropped(ProllyChunkStore *s, const ProllyHash *root);
static void gc_abort(ProllyChunkGc *gc);
static void gc_abort_sub(ProllyChunkGc *gc, int level);
static XLogRecPtr logical_horizon(Oid dboid);
static uint32 gc_record(Segment *g, uint64 offset, uint64 size, char **rec,
						Size *reclen);

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
root_record(StringInfo buf, const ProllyHash *root, TimestampTz at, bool pad)
{
	uint8		payload[1 + PROLLY_HASH_MAX_LEN + sizeof(TimestampTz)];
	Size		len;

	hash_payload(root, payload, &len);
	memcpy(payload + len, &at, sizeof(at));
	len += sizeof(at);
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
 * When a ROOT record's move was made, its payload's hash ending at after (0
 * if the record doesn't say).
 */
static TimestampTz
record_root_time(const char *rec, uint32 len, Size after)
{
	TimestampTz at;

	if (after + sizeof(at) + sizeof(uint32) > len)
		return 0;
	memcpy(&at, rec + after, sizeof(at));
	return at;
}

/*
 * A chunk as a record stores it: its codec (and, compressed, its length) in
 * head (*headlen bytes), then the bytes stored, *stored (*stored_len bytes;
 * palloc'd if not data itself, which the caller frees).
 */
static void
chunk_encode(const void *data, Size len, uint8 *head, Size *headlen,
			 const char **stored, Size *stored_len)
{
	head[0] = CODEC_NONE;
	*headlen = 1;
	*stored = data;
	*stored_len = len;
#ifdef USE_LZ4
	/* (one whose bound is past what palloc gives is stored as is) */
	if (len > 0 && len <= LZ4_MAX_INPUT_SIZE &&
		(Size) LZ4_compressBound((int) len) <= MaxAllocSize)
	{
		int			bound = LZ4_compressBound((int) len);
		char	   *compressed = palloc(bound);
		int			n = LZ4_compress_default(data, compressed, (int) len, bound);

		if (n > 0 && (Size) n + sizeof(uint32) < len)
		{
			uint32		rawlen = (uint32) len;

			head[0] = CODEC_LZ4;
			memcpy(head + 1, &rawlen, sizeof(rawlen));
			*headlen = CODEC_HEAD_MAX;
			*stored = compressed;
			*stored_len = n;
			return;
		}
		pfree(compressed);
	}
#endif
}

/*
 * The chunk a CHUNK or XCHUNK record (whole and valid) holds, its payload
 * starting at after (past the hash): a pointer into the record if it is
 * stored as is, or decompressed into scratch.  *chunk_len gets its length.
 * NULL if the record doesn't decode.
 */
static const char *
record_chunk(const char *rec, uint32 len, Size after, StringInfo scratch,
			 Size *chunk_len)
{
	const char *p = rec + after;
	const char *end = rec + len - sizeof(uint32);

	if (p >= end)
		return NULL;
	if ((uint8) *p == CODEC_NONE)
	{
		*chunk_len = end - (p + 1);
		return p + 1;
	}
	if ((uint8) *p == CODEC_LZ4 && end - p >= (ptrdiff_t) CODEC_HEAD_MAX)
	{
#ifdef USE_LZ4
		uint32		rawlen;
		int			n;

		memcpy(&rawlen, p + 1, sizeof(rawlen));
		/* (LZ4 makes nothing smaller than about a 255th of itself) */
		if (rawlen > MaxAllocSize - 1 ||
			rawlen > (uint64) (end - (p + CODEC_HEAD_MAX)) * 255 + 16)
			return NULL;
		resetStringInfo(scratch);
		enlargeStringInfo(scratch, (int) rawlen);
		p += CODEC_HEAD_MAX;
		n = LZ4_decompress_safe(p, scratch->data, (int) (end - p), (int) rawlen);
		if (n < 0 || (uint32) n != rawlen)
			return NULL;
		scratch->len = n;
		scratch->data[n] = '\0';
		*chunk_len = n;
		return scratch->data;
#else
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("chunk journal record is compressed with LZ4"),
				 errdetail("This server was built without LZ4.")));
#endif
	}
	return NULL;
}

/*
 * A scratch buffer for record_chunk(), in TopMemoryContext: what the last
 * call decoded into it is done with by the next.  One a large chunk grew
 * is made small again.
 */
#define CHUNK_SCRATCH_KEEP	(1024 * 1024)
static StringInfo
chunk_scratch(void)
{
	static StringInfo scratch = NULL;

	if (scratch != NULL && scratch->maxlen > CHUNK_SCRATCH_KEEP)
	{
		destroyStringInfo(scratch);
		scratch = NULL;
	}
	if (scratch == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

		scratch = makeStringInfo();
		MemoryContextSwitchTo(old);
	}
	return scratch;
}

/*
 * record_chunk() of a record whose hash was read: one that doesn't decode
 * is corrupt (in where), not skipped, as what it holds may be reachable.
 */
static const char *
decoded_chunk(const char *rec, uint32 len, Size after, const char *where,
			  Size *chunk_len)
{
	const char *chunk = record_chunk(rec, len, after, chunk_scratch(),
									 chunk_len);

	if (chunk == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupt chunk record in %s", where)));
	return chunk;
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

static bool header_ok(const char *header, Size len);
static void check_header(const char *header, Size len, const char *path);

/*
 * Read the start of a segment's file (as much of its header as it has)
 * into header; how many bytes.
 */
static Size
read_header(File file, char *header, const char *path)
{
	off_t		size = FileSize(file);
	Size		len = Min(size, JOURNAL_HEADER_SIZE);

	if (size < 0 ||
		FileRead(file, header, len, 0, WAIT_EVENT_DATA_FILE_READ) != len)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m", path)));
	return len;
}

/*
 * Open a segment's file; false if it has none yet.  One with its header is
 * checked to be this server's version and format.
 */
static bool
open_segment(Segment *g)
{
	char		header[JOURNAL_HEADER_SIZE];
	Size		len;

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
	len = read_header(g->file, header, g->path);
	if (!header_ok(header, len))
	{
		/* (closed, so the next open checks again) */
		FileClose(g->file);
		g->file = -1;
		check_header(header, len, g->path);
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
	pgstat_count_prolly(PGSTAT_PROLLY_JOURNAL_BYTES_WRITTEN, len);
	MyProc->delayChkptFlags &= ~DELAY_CHKPT_START;
	END_CRIT_SECTION();
}

/* The store's format as its files hold it: hash name, its length, version. */
static void
format_bytes(const ProllyStoreFormat *f, char *out)
{
	memcpy(out, f->hash, PROLLY_STORE_HASH_NAME_LEN);
	out[PROLLY_STORE_HASH_NAME_LEN] = (char) f->hash_len;
	out[PROLLY_STORE_HASH_NAME_LEN + 1] = (char) (f->chunk_format & 0xff);
	out[PROLLY_STORE_HASH_NAME_LEN + 2] = (char) (f->chunk_format >> 8);
}

static void
format_read(const char *in, ProllyStoreFormat *f)
{
	memcpy(f->hash, in, PROLLY_STORE_HASH_NAME_LEN);
	f->hash_len = (uint8) in[PROLLY_STORE_HASH_NAME_LEN];
	f->chunk_format = (uint16) ((uint8) in[PROLLY_STORE_HASH_NAME_LEN + 1] |
								((uint8) in[PROLLY_STORE_HASH_NAME_LEN + 2] << 8));
}

/*
 * Refuse a store file (at path) of another version (what) or format than
 * this server writes: its chunks are named, or laid out, otherwise.  (Its
 * format is read only once its version is this server's: NULL before.)
 */
static void
check_format(const char *path, const char *what, uint32 version,
			 uint32 expected, const char *format)
{
	ProllyStoreFormat theirs;
	const ProllyStoreFormat *mine = prolly_store_format();

	if (version != expected)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("file \"%s\" is a chunk store %s of version %u, not %u",
						path, what, version, expected),
				 errdetail("A server reads only chunk stores made in its own format."),
				 errhint("There is no conversion: restore a dump into a cluster made by this server.")));
	format_read(format, &theirs);
	if (!prolly_store_format_equal(&theirs, mine))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("file \"%s\" is a chunk store %s of another format", path, what),
				 errdetail("Its chunks are %s; this server's are %s.",
						   prolly_store_format_text(&theirs),
						   prolly_store_format_text(mine)),
				 errhint("There is no conversion: restore a dump into a cluster made by this server.")));
}

/*
 * Is the start of a segment's file (len bytes of it) a journal's header, of
 * this server's version and format?  None at all (a file not given one
 * yet) is.
 */
static bool
header_ok(const char *header, Size len)
{
	char		mine[FORMAT_SIZE];
	uint32		version;

	if (len == 0)
		return true;
	if (len < JOURNAL_HEADER_SIZE)
		return false;
	memcpy(&version, header + 8, sizeof(version));
	format_bytes(prolly_store_format(), mine);
	return memcmp(header, JOURNAL_MAGIC, 8) == 0 &&
		version == JOURNAL_VERSION &&
		memcmp(header + JOURNAL_FORMAT_AT, mine, FORMAT_SIZE) == 0;
}

/*
 * Refuse the start of a segment's file that header_ok() didn't take: one
 * of another version or format, or no journal's.  (An older version's
 * header may be shorter than this one's: one with no records yet is only
 * that.)
 */
static void
check_header(const char *header, Size len, const char *path)
{
	uint32		version;

	if (len < 8 + sizeof(version) || memcmp(header, JOURNAL_MAGIC, 8) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("file \"%s\" is not a chunk journal", path)));
	memcpy(&version, header + 8, sizeof(version));
	if (version == JOURNAL_VERSION && len < JOURNAL_HEADER_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("file \"%s\" has a short chunk journal header", path)));
	check_format(path, "journal", version, JOURNAL_VERSION,
				 header + JOURNAL_FORMAT_AT);
	/* (equal formats, written with other bytes past the hash's name) */
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("file \"%s\" has a damaged chunk journal header", path)));
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

	StaticAssertStmt(JOURNAL_FORMAT_AT + FORMAT_SIZE <= JOURNAL_HEADER_SIZE,
					 "a journal's header holds the store's format");
	if (fd < 0 || fstat(fd, &st) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
	if (st.st_size == 0)
	{
		memset(header, 0, sizeof(header));
		memcpy(header, JOURNAL_MAGIC, 8);
		memcpy(header + 8, &version, sizeof(version));
		format_bytes(prolly_store_format(), header + JOURNAL_FORMAT_AT);
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

static JournalShared *shared(void);
static void scan_segment(ProllyChunkStore *s, Segment *g);
static uint64 index_range(ProllyChunkStore *s, Segment *g, uint64 offset,
						  uint64 end, char **rec, Size *reclen);

/*
 * The range reserved and not yet written that holds offset of a segment,
 * if any: how much of it is at offset or after (0: none).  A reader meets
 * one at its start (nothing of it written) or inside it (a write seen in
 * part, its first records whole).
 */
static uint64
append_in_flight(ProllyChunkStore *s, uint64 segment, uint64 offset)
{
	JournalShared *js = shared();
	uint64		len = 0;

	SpinLockAcquire(&js->mutex);
	for (int i = 0; i < js->nranges && js->inflight > 0; i++)
	{
		AppendRange *r = &js->ranges[i];

		if (r->len > 0 && r->segment == segment &&
			r->dboid == s->dboid && r->spcoid == s->spcoid &&
			r->offset <= offset && offset < r->offset + r->len)
		{
			len = r->offset + r->len - offset;
			break;
		}
	}
	SpinLockRelease(&js->mutex);
	return len;
}

/* Is r, in flight, of this database's store (s NULL: any store's)? */
static inline bool
range_of(AppendRange *r, ProllyChunkStore *s)
{
	return r->len > 0 &&
		(s == NULL || (r->dboid == s->dboid && r->spcoid == s->spcoid));
}

/*
 * Whether any range of this database's store in a segment before the one
 * given is reserved and not written.
 */
static bool
appends_in_flight(ProllyChunkStore *s, uint64 before)
{
	JournalShared *js = shared();
	bool		any = false;

	SpinLockAcquire(&js->mutex);
	for (int i = 0; i < js->nranges && js->inflight > 0 && !any; i++)
		any = range_of(&js->ranges[i], s) &&
			(s == NULL || js->ranges[i].segment < before);
	SpinLockRelease(&js->mutex);
	return any;
}

/*
 * Wait for every append reserved in this database's store, in a segment
 * before the one given, to be written (s NULL: every store's).  None is
 * reserved there meanwhile: the caller holds ProllyJournalLock, or those
 * segments are sealed.  An append in flight needs nothing to finish but
 * its own flush.
 */
static void
wait_appends(ProllyChunkStore *s, uint64 before)
{
	JournalShared *js = shared();

	while (appends_in_flight(s, before))
		ConditionVariableSleep(&js->written, WAIT_EVENT_PROLLY_JOURNAL_APPEND);
	ConditionVariableCancelSleep();
}

static bool journal_has(ProllyChunkStore *s, const ProllyHash *hash);

/*
 * The chunks this transaction told keep() of since keeps_begun was
 * kept_here_begun (in TopTransactionContext)
 */
static HTAB *kept_here = NULL;
static uint64 kept_here_begun = 0;

static KeepTouch *
touches(JournalShared *js)
{
	return (KeepTouch *) ((char *) js + TOUCHES_OFFSET(js->nranges));
}

/*
 * A chunk this transaction names from outside the store: its transaction
 * noted, for a collection that begins before it ends to wait for, and the
 * chunk added to the keep set of a collection running now.  Both under
 * the mutex a collection begins under, so one either waits for the
 * transaction or keeps the chunk; and under the journal's lock, which a
 * collection holds from before it last reads its keep set until its
 * swap, so a chunk isn't added once it has been read for the last time.
 * A swap before then may have dropped the chunk: the transaction fails.
 */
static void
journal_keep(ProllyChunkStore *s, const ProllyHash *hash)
{
	JournalShared *js = shared();
	KeepTouch  *t = &touches(js)[MyProcNumber];
	TransactionId xid = GetTopTransactionId();
	uint64		begun = pg_atomic_read_u64(&js->keeps_begun);
	ProllyHash	key;
	bool		found;

	/*
	 * Told already in this transaction, and no keep set begun since: the
	 * transaction is noted, and every set running has the chunk.
	 */
	hash_key(&key, hash);
	if (kept_here != NULL && kept_here_begun != begun)
	{
		hash_destroy(kept_here);
		kept_here = NULL;
	}
	if (kept_here == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(ProllyHash);
		ctl.entrysize = sizeof(ProllyHash);
		ctl.hcxt = TopTransactionContext;
		kept_here = hash_create("prolly chunks kept", 64, &ctl,
								HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		kept_here_begun = begun;
	}
	(void) hash_search(kept_here, &key, HASH_FIND, &found);
	if (found)
		return;

	LWLockAcquire(ProllyJournalLock, LW_SHARED);
	SpinLockAcquire(&js->mutex);
	if (!TransactionIdIsValid(t->xid))
	{
		t->dboid = s->dboid;
		t->spcoid = s->spcoid;
		t->xid = xid;
	}
	for (int i = 0; i < KEEP_COLLECTIONS; i++)
	{
		KeepSet    *k = &js->keeps[i];

		if (!k->active || k->dboid != s->dboid || k->spcoid != s->spcoid)
			continue;
		if (k->n < KEEP_MAX)
			k->hashes[k->n++] = *hash;
		else
			k->overflow = true;
	}
	SpinLockRelease(&js->mutex);
	LWLockRelease(ProllyJournalLock);
	if (!journal_has(s, hash))
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("could not serialize access due to a concurrent collection of the chunk store"),
				 errdetail("A value stored by content was collected meanwhile."),
				 errhint("The transaction might succeed if retried.")));
	(void) hash_search(kept_here, &key, HASH_ENTER, NULL);
}

/*
 * The transaction has ended, or been prepared (its pointers are on pages):
 * a collection need wait for it no more.
 */
static void
keep_touch_end(void)
{
	JournalShared *js;
	KeepTouch  *t;

	kept_here = NULL;			/* (it went with the transaction) */
	if (journal_shared == NULL)
		return;
	js = journal_shared;
	t = &touches(js)[MyProcNumber];
	if (!TransactionIdIsValid(t->xid))
		return;
	SpinLockAcquire(&js->mutex);
	t->xid = InvalidTransactionId;
	SpinLockRelease(&js->mutex);
}

/*
 * A collection begins keeping what keep() is told of, and will wait for
 * the transactions told of one before: its keep set, or -1 if none is free
 * (it then keeps every chunk).
 */
static int
keep_begin(ProllyChunkStore *s, List **xids)
{
	JournalShared *js = shared();
	KeepTouch  *ts = touches(js);
	TransactionId *found = palloc(sizeof(TransactionId) * js->nranges);
	int			nfound = 0;
	int			slot = -1;

	SpinLockAcquire(&js->mutex);
	for (int i = 0; i < KEEP_COLLECTIONS && slot < 0; i++)
		if (!js->keeps[i].active)
			slot = i;
	if (slot >= 0)
	{
		js->keeps[slot].active = true;
		js->keeps[slot].overflow = false;
		js->keeps[slot].dboid = s->dboid;
		js->keeps[slot].spcoid = s->spcoid;
		js->keeps[slot].n = 0;
		pg_atomic_fetch_add_u64(&js->keeps_begun, 1);
	}
	for (int i = 0; i < js->nranges; i++)
	{
		KeepTouch  *t = &ts[i];

		if (TransactionIdIsValid(t->xid) && t->dboid == s->dboid &&
			t->spcoid == s->spcoid)
			found[nfound++] = t->xid;
	}
	SpinLockRelease(&js->mutex);
	for (int i = 0; i < nfound; i++)
		if (!list_member_xid(*xids, found[i]))
			*xids = lappend_xid(*xids, found[i]);
	pfree(found);
	return slot;
}

/* A collection keeps no more: its keep set (*keep) let go. */
static void
keep_end(int *keep)
{
	JournalShared *js = shared();

	if (*keep < 0)
		return;
	SpinLockAcquire(&js->mutex);
	js->keeps[*keep].active = false;
	SpinLockRelease(&js->mutex);
	*keep = -1;
}

/*
 * Wait for the appends in flight now in this database's store (s NULL:
 * every store's) to be written, not for those reserved meanwhile, so a
 * stream of appends can't keep the caller waiting.
 */
static void
wait_reserved(ProllyChunkStore *s)
{
	JournalShared *js = shared();
	int		   *slots = palloc(sizeof(int) * js->nranges);
	uint64	   *gens = palloc(sizeof(uint64) * js->nranges);
	int			n = 0;

	SpinLockAcquire(&js->mutex);
	for (int i = 0; i < js->nranges && n < js->inflight; i++)
	{
		if (range_of(&js->ranges[i], s))
		{
			slots[n] = i;
			gens[n++] = js->ranges[i].gen;
		}
	}
	SpinLockRelease(&js->mutex);
	while (n > 0)
	{
		int			left = 0;

		SpinLockAcquire(&js->mutex);
		for (int j = 0; j < n; j++)
		{
			AppendRange *r = &js->ranges[slots[j]];

			if (r->len > 0 && r->gen == gens[j])
			{
				slots[left] = slots[j];
				gens[left++] = gens[j];
			}
		}
		SpinLockRelease(&js->mutex);
		n = left;
		if (n > 0)
			ConditionVariableSleep(&js->written, WAIT_EVENT_PROLLY_JOURNAL_APPEND);
	}
	ConditionVariableCancelSleep();
	pfree(slots);
	pfree(gens);
}

/* Was a segment's end checked since the server started? */
static bool
end_checked(ProllyChunkStore *s, Segment *g)
{
	JournalShared *js = shared();

	for (int i = 0; i < CHECKED_ENDS; i++)
	{
		CheckedEnd *c = &js->checked[i];

		if (c->segment == g->id && c->dboid == s->dboid &&
			c->spcoid == s->spcoid)
			return true;
	}
	return false;
}

/* The end of the last range in flight in a segment (0: none). */
static uint64
in_flight_end(ProllyChunkStore *s, uint64 segment)
{
	JournalShared *js = shared();
	uint64		end = 0;

	SpinLockAcquire(&js->mutex);
	for (int i = 0; i < js->nranges && js->inflight > 0; i++)
	{
		AppendRange *r = &js->ranges[i];

		if (range_of(r, s) && r->segment == segment)
			end = Max(end, r->offset + r->len);
	}
	SpinLockRelease(&js->mutex);
	return end;
}

/*
 * The segment's end, where the next append goes, under ProllyJournalLock.
 * A crash can leave a range reserved at the end whose bytes the WAL never
 * had (reserved after the last flush): zeros a reader stops at, which would
 * hide whatever came after them.  So the first append to a segment after
 * the server starts (or after its entry in the short list of segments
 * checked is reused) cuts off what follows its last whole record.  Never a
 * range in flight: the scan skips each, and the cut stops at their end.
 */
static uint64
segment_end(ProllyChunkStore *s, Segment *g)
{
	JournalShared *js = shared();
	pgoff_t		size;
	uint64		end;

	if (!open_segment(g) || (size = FileSize(g->file)) < JOURNAL_HEADER_SIZE)
		elog(ERROR, "chunk journal \"%s\" has no header", g->path);
	if (end_checked(s, g))
		return size;
	scan_segment(s, g);
	end = Max(g->scanned, in_flight_end(s, g->id));
	if (end < (uint64) size)
	{
		ereport(LOG,
				(errmsg("cutting %llu bytes no record holds off the end of chunk journal \"%s\"",
						(unsigned long long) (size - end), g->path)));
		if (FileTruncate(g->file, end, WAIT_EVENT_DATA_FILE_TRUNCATE) < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not truncate file \"%s\": %m", g->path)));
		size = end;
	}
	js->checked[js->next_checked].dboid = s->dboid;
	js->checked[js->next_checked].spcoid = s->spcoid;
	js->checked[js->next_checked].segment = g->id;
	js->next_checked = (js->next_checked + 1) % CHECKED_ENDS;
	return size;
}

/*
 * Reserve len bytes at the end of the segment appends go to, and WAL-log
 * data for them, under ProllyJournalLock; the bytes are written by
 * append_write(), outside it, once the log is flushed.  The segments are
 * read again first if a manifest was written since (a seal takes the same
 * lock), so nothing is appended to a segment that was sealed: before the
 * lock, and under it only if another manifest came meanwhile, so the lock
 * is seldom held while a manifest is read.  With checked, the bytes'
 * chunks were checked against the store as of that GC generation: if a
 * swap has happened since (the only manifest that removes chunks), nothing
 * is reserved and false returned (the caller checks them again).
 *
 * The range is the segment's from here on (the file is extended over it)
 * and is listed in shared memory until written, so a reader meeting its
 * zeros skips it rather than stopping there, and a checkpoint waits for
 * its bytes before syncing the segments (journal_checkpoint()): one whose
 * redo point follows the record finds them written.
 */
static bool
append_reserve(ProllyChunkStore *s, const char *data, Size len,
			   const uint32 *checked, Append *a)
{
	JournalShared *js = shared();
	AppendRange *r = &js->ranges[MyProcNumber];
	xl_chunks_append xlrec;
	Segment    *g;
	uint64		size;

	refresh_segments(s);
	/* a segment's first check reads it whole: most of it before the lock */
	g = current_segment(s);
	if (!end_checked(s, g))
		scan_segment(s, g);
	LWLockAcquire(ProllyJournalLock, LW_EXCLUSIVE);
	refresh_segments(s);
	if (checked != NULL && s->gc_generation != *checked)
	{
		LWLockRelease(ProllyJournalLock);
		return false;
	}
	g = current_segment(s);
	size = segment_end(s, g);
	if (FileTruncate(g->file, size + len, WAIT_EVENT_DATA_FILE_EXTEND) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not extend file \"%s\": %m", g->path)));

	Assert(r->len == 0);
	START_CRIT_SECTION();
	SpinLockAcquire(&js->mutex);
	r->dboid = s->dboid;
	r->spcoid = s->spcoid;
	r->segment = g->id;
	r->offset = size;
	r->len = len;
	r->gen++;
	js->inflight++;
	SpinLockRelease(&js->mutex);

	xlrec.hdr.impl_id = prolly_chunkstore_journal.info.impl_id;
	xlrec.dboid = s->dboid;
	xlrec.spcoid = s->spcoid;
	xlrec.segment = g->id;
	xlrec.offset = size;
	XLogBeginInsert();
	XLogRegisterData(&xlrec, SizeOfChunksAppend);
	XLogRegisterData(data, len);
	a->lsn = XLogInsert(RM_PROLLY_CHUNKS_ID, XLOG_CHUNKS_APPEND);
	END_CRIT_SECTION();
	LWLockRelease(ProllyJournalLock);

	a->g = g;
	a->offset = size;
	a->len = len;
	return true;
}

/*
 * Write a reserved append's bytes, once the WAL holding them is flushed
 * (a flush other backends' appends and commits share), and let its range
 * go.  May run in a commit's critical section: allocates nothing.
 */
static void
append_write(Append *a, const char *data)
{
	JournalShared *js = shared();
	AppendRange *r = &js->ranges[MyProcNumber];

	Assert(r->len == a->len && r->offset == a->offset);
	START_CRIT_SECTION();
	XLogFlush(a->lsn);
	if (FileWrite(a->g->file, data, a->len, a->offset,
				  WAIT_EVENT_DATA_FILE_WRITE) != a->len)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not write %zu bytes at %llu in file \"%s\": %m",
						a->len, (unsigned long long) a->offset, a->g->path)));
	pgstat_count_prolly(PGSTAT_PROLLY_JOURNAL_BYTES_WRITTEN, a->len);
	SpinLockAcquire(&js->mutex);
	r->len = 0;
	js->inflight--;
	SpinLockRelease(&js->mutex);
	ConditionVariableBroadcast(&js->written);
	END_CRIT_SECTION();
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
	char	   *rec = NULL;
	Size		reclen = 0;

	if (!open_segment(g))
		return;
	size = FileSize(g->file);

	/*
	 * Each PAD seen: forgotten once it reads as a whole ROOT (taken if later
	 * than the root); kept, to look at again, while it doesn't (still a PAD,
	 * or a ROOT not all written yet).  (Deleted from the list in place: the
	 * list stays the segment's, in its memory.)
	 */
	foreach(lc, g->pads)
	{
		uint64		pad = *(uint64 *) lfirst(lc);
		uint8		kind;

		read_exact(g, (char *) &kind, 1, pad + sizeof(uint32));
		if (kind == KIND_ROOT)
		{
			uint32		len;
			char		buf[ROOT_RECORD_MAX];
			ProllyHash	h;
			Size		after;

			read_exact(g, (char *) &len, sizeof(len), pad);
			if (len <= sizeof(buf))
			{
				read_exact(g, buf, len, pad);
				if (record_valid(buf, len) && record_hash(buf, len, &h, &after))
				{
					if (root_later(s, g->id, pad))
					{
						s->root = h;
						s->root_segment = g->id;
						s->root_offset = pad;
					}
					pfree(lfirst(lc));
					g->pads = foreach_delete_current(g->pads, lc);
				}
			}
		}
	}

	/* ranges skipped while their appends were in flight, if written since */
	foreach(lc, g->holes)
	{
		Hole	   *hole = lfirst(lc);
		uint64		end = hole->offset + hole->len;
		uint64		upto = index_range(s, g, hole->offset, end, &rec, &reclen);

		if (upto == end)
		{
			pfree(hole);
			g->holes = foreach_delete_current(g->holes, lc);
		}
		else
		{
			/* (a write seen in part) */
			hole->offset = upto;
			hole->len = end - upto;
		}
	}

	/*
	 * Up to the first record that isn't whole, skipping a range reserved for
	 * an append in flight.  A range seen to be neither whole nor in flight
	 * may have been written in between: it is read once more.
	 */
	for (bool again = false;;)
	{
		uint64		hole;

		offset = index_range(s, g, offset, size, &rec, &reclen);
		if (offset + RECORD_OVERHEAD > size)
			break;
		hole = append_in_flight(s, g->id, offset);
		if (hole > 0 && offset + hole <= size)
		{
			MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);
			Hole	   *h = palloc(sizeof(Hole));

			h->offset = offset;
			h->len = hole;
			g->holes = lappend(g->holes, h);
			MemoryContextSwitchTo(old);
			offset += hole;
			again = false;
			continue;
		}
		if (again)
			break;
		again = true;
	}
	if (rec)
		pfree(rec);
	g->scanned = offset;
}

/*
 * Index the whole records of a segment in [offset, end), the first being
 * at offset; returns the offset of the first that isn't whole (end if
 * all are).  *rec and *reclen are a buffer to read into, in
 * TopMemoryContext, grown as needed.
 */
static uint64
index_range(ProllyChunkStore *s, Segment *g, uint64 offset, uint64 end,
			char **rec, Size *reclen)
{
	while (offset + RECORD_OVERHEAD <= end)
	{
		uint32		len;
		uint8		kind;
		ProllyHash	h;
		Size		after;
		TransactionId xid;

		read_exact(g, (char *) &len, sizeof(len), offset);
		if (len < RECORD_OVERHEAD || offset + len > end)
			break;
		if (len > *reclen)
		{
			if (*rec)
				pfree(*rec);
			*reclen = Max(len, 8192);
			*rec = MemoryContextAlloc(TopMemoryContext, *reclen);
		}
		read_exact(g, *rec, len, offset);
		if (!record_valid(*rec, len))
			break;
		kind = (uint8) (*rec)[sizeof(uint32)];
		if ((kind == KIND_CHUNK || kind == KIND_XCHUNK) &&
			record_hash_xid(*rec, len, &h, &after, &xid))
		{
			ProllyHash	key;

			hash_key(&key, &h);
			index_add(s->index, &key, g->id, offset, len, xid);
		}
		else if (kind == KIND_ROOT && record_hash(*rec, len, &h, &after))
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
	return offset;
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
		list_free_deep(g->holes);
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
		const char *chunk = NULL;
		Size		chunk_len;

		if (record_hash(rec, e->len, &h, &after))
			chunk = record_chunk(rec, e->len, after, chunk_scratch(), &chunk_len);
		if (chunk == NULL)
			elog(ERROR, "corrupt pending chunk record");
		rc.s = s;
		rc.chunk = &h;
		prolly_chunk_references(chunk, chunk_len, check_reference, &rc);
	}
}

/*
 * The root a transaction moves to is in the store (it may be one the
 * transaction didn't write, an older root moved back to), and no swap
 * since the transaction began dropped it.
 */
static void
check_root(ProllyChunkStore *s)
{
	ProllyHash	key;

	hash_key(&key, &s->new_root);
	if (hash_search(s->pending_index, &key, HASH_FIND, NULL) == NULL &&
		!stored_has(s, &key))
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("could not move the chunk store's root to %s: it is not in the chunk store",
						prolly_hash_hex(&s->new_root)),
				 errdetail("A garbage collection may have removed it after the transaction read it."),
				 errhint("Try the transaction again.")));
	if (xact_straddles(s))
		check_dropped(s, &s->new_root);
}

/* The pending chunks are in segment g from base on: index them there. */
static void
pending_appended(ProllyChunkStore *s, Segment *g, uint64 base)
{
	HASH_SEQ_STATUS status;
	IndexEntry *e;

	hash_seq_init(&status, s->pending_index);
	while ((e = hash_seq_search(&status)) != NULL)
	{
		index_add(s->index, &e->hash, g->id, base + e->offset, e->len, e->xid);
		hash_search(s->pending_index, &e->hash, HASH_REMOVE, NULL);
	}
	resetStringInfo(&s->pending);
	s->flushes++;
}

/*
 * Check the pending chunks (and with pad_at, the root a move goes to)
 * against the store as of a GC generation, and reserve an append of
 * pending's bytes only if no swap came meanwhile (it would have removed
 * chunks).  With pad_at, a PAD for the root's record is added to pending
 * once, at *pad_at.
 */
static void
reserve_pending(ProllyChunkStore *s, Append *a, int *pad_at)
{
	(void) ensure_journal(s, true);
	for (;;)
	{
		uint32		generation;

		refresh_segments(s);
		generation = s->gc_generation;
		load_tables(s, false);
		check_pending(s);
		if (xact_straddles(s))
			check_dropped(s, NULL);
		if (pad_at != NULL)
		{
			check_root(s);
			if (*pad_at < 0)
			{
				*pad_at = s->pending.len;
				root_record(&s->pending, &s->new_root, 0, true);
			}
		}
		if (append_reserve(s, s->pending.data, s->pending.len, &generation, a))
			break;
	}
}

/* Append the transaction's pending chunks. */
static void
flush_pending(ProllyChunkStore *s)
{
	Append		a;

	if (s->pending.len == 0)
		return;
	reserve_pending(s, &a, NULL);
	append_write(&a, s->pending.data);
	pending_appended(s, a.g, a.offset);
}

/*
 * The root a move goes to has its PAD at offset of segment g: keep the
 * ROOT record that takes its place as the append is written (committed()).
 */
static void
root_padded(ProllyChunkStore *s, Segment *g, uint64 offset)
{
	StringInfoData rec;

	s->pad_segment = g;
	s->pad_offset = offset;
	INJECTION_POINT("prolly-root-padded", NULL);
	initStringInfo(&rec);
	s->root_at = GetCurrentTimestamp();
	root_record(&rec, &s->new_root, s->root_at, false);
	Assert(rec.len <= sizeof(s->root_rec));
	memcpy(s->root_rec, rec.data, rec.len);
	s->root_rec_len = rec.len;
	pfree(rec.data);
}

/*
 * As the commit begins, after everything else that runs before it (which
 * may still put chunks): reserve the rest of the pending chunks, and a PAD
 * for the root a move goes to, as one append, whose bytes are written once
 * the commit record is flushed (committed()), so a commit flushes the log
 * once and appends hold the journal's lock only to reserve.  The chunks
 * stay pending (readable) until then.
 */
static void
reserve_at_commit(ProllyChunkStore *s)
{
	int			pad_at = -1;

	if (s->pending.len == 0 && !s->moving)
		return;
	reserve_pending(s, &s->commit_append, s->moving ? &pad_at : NULL);
	s->at_commit = true;
	INJECTION_POINT("prolly-append-reserved", NULL);
	if (s->moving)
		root_padded(s, s->commit_append.g, s->commit_append.offset + pad_at);
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
			/* reserved later, by commit_writes(): chunks may still come */
			break;
		case XACT_EVENT_PARALLEL_PRE_COMMIT:
			/* a parallel worker's commit has no record: written now */
			if (s->pending.len > 0 || s->moving)
			{
				Append		a;
				int			pad_at = -1;

				reserve_pending(s, &a, s->moving ? &pad_at : NULL);
				append_write(&a, s->pending.data);
				pending_appended(s, a.g, a.offset);
				if (s->moving)
					root_padded(s, a.g, a.offset + pad_at);
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
			keep_touch_end();
			/* the rest of the chunks and a root moved, written by committed() */
			Assert(!s->at_commit);
			if (s->commit_append.g != NULL)
			{
				pending_appended(s, s->commit_append.g, s->commit_append.offset);
				s->commit_append.g = NULL;
			}
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
			keep_touch_end();
			/* the WAL has them: written as any append (a PAD stays one) */
			if (s->at_commit)
			{
				append_write(&s->commit_append, s->pending.data);
				s->at_commit = false;
			}
			s->commit_append.g = NULL;
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

	if (event == SUBXACT_EVENT_ABORT_SUB && s->gc != NULL)
		gc_abort_sub(s->gc, level);
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
	/* (open_segment() checks the header) */
	if (!open_segment(g) || FileSize(g->file) < JOURNAL_HEADER_SIZE)
		return false;
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
	char		format[FORMAT_SIZE];

	appendBinaryStringInfo(out, MANIFEST_MAGIC, 8);
	append_le(out, MANIFEST_VERSION, 4);
	format_bytes(prolly_store_format(), format);
	appendBinaryStringInfo(out, format, FORMAT_SIZE);
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
	{
		append_le(out, m->segments[i], 8);
		append_le(out, m->sealed_at ? m->sealed_at[i] : InvalidXLogRecPtr, 8);
	}
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
	m->sealed_at = palloc0(sizeof(XLogRecPtr));
}

/*
 * Read the manifest at path into m; false (m empty, but for segment 0) if
 * there is none.  One of another version or format is refused.
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
	uint32		nsegs;
	uint32		nretiring;

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
	/* (its version first: another's may be laid out, or sized, otherwise) */
	if (st.st_size < 8 + 4 || memcmp(p, MANIFEST_MAGIC, 8) != 0)
		goto corrupt;
	version = (uint32) read_le(p + 8, 4);
	if (version != MANIFEST_VERSION)
		check_format(path, "manifest", version, MANIFEST_VERSION, NULL);
	if (st.st_size < 8 + 4 + FORMAT_SIZE + 4 + 8 + 1 + 4 + 4 + 4 + 4)
		goto corrupt;
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, buf, st.st_size - sizeof(crc));
	FIN_CRC32C(crc);
	if (crc != (pg_crc32c) read_le(end - sizeof(crc), 4))
		goto corrupt;
	end -= sizeof(crc);
	p += 8 + 4;
	check_format(path, "manifest", version, MANIFEST_VERSION, p);
	p += FORMAT_SIZE;
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
	if ((Size) (end - p) < (Size) nfiles * MANIFEST_FILE_ENTRY + 4)
		goto corrupt;
	m->nfiles = nfiles;
	m->files = palloc0(sizeof(ManifestFile) * Max(nfiles, 1));
	for (uint32 i = 0; i < nfiles; i++)
	{
		m->files[i].id = read_le(p, 8);
		m->files[i].nchunks = (uint32) read_le(p + 8, 4);
		m->files[i].size = read_le(p + 12, 8);
		m->files[i].flags = (uint8) p[20];
		p += MANIFEST_FILE_ENTRY;
	}
	nsegs = (uint32) read_le(p, 4);
	p += 4;
	if (nsegs < 1 || (Size) (end - p) < (Size) nsegs * MANIFEST_SEGMENT_ENTRY + 4)
		goto corrupt;
	m->nsegments = nsegs;
	m->segments = palloc(sizeof(uint64) * nsegs);
	m->sealed_at = palloc0(sizeof(XLogRecPtr) * nsegs);
	for (uint32 i = 0; i < nsegs; i++)
	{
		m->segments[i] = read_le(p, 8);
		m->sealed_at[i] = read_le(p + 8, 8);
		p += MANIFEST_SEGMENT_ENTRY;
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
	pgstat_count_prolly(PGSTAT_PROLLY_MANIFEST_BYTES_WRITTEN, len);
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
	pgstat_count_prolly(PGSTAT_PROLLY_TABLE_FILE_BYTES_WRITTEN, len);
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
		const ProllyTableFileRoutine *format = table_format();
		uint32		version = format->version == NULL ? 0 :
			format->version(table_file_read, t, t->size);

		FileClose(t->file);
		if (version != 0 && version != format->info.format_version)
			check_format(t->path, "table file", version,
						 format->info.format_version, NULL);
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
 * files m names that keep leaves out are removed after; copies says the
 * chunks are the store's own, written again (conjoining), and counts them
 * so.  The manifest's lock is held.
 */
static void
write_table_file(ProllyChunkStore *s, Manifest *m, bool *keep, uint8 flags,
				 ProllyChunkNext next, void *arg, bool copies)
{
	char	   *dir;
	uint64		id = m->last_id + 1;
	uint64		size;
	uint32		nchunks;
	Manifest	nm;

	if (!write_file(s, id, next, arg, &nchunks, &size))
		return;
	if (copies)
		pgstat_count_prolly(PGSTAT_PROLLY_CONJOIN_BYTES_COPIED, size);
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
	uint8		head[sizeof(TransactionId) + 1 + PROLLY_HASH_MAX_LEN +
					 CODEC_HEAD_MAX];
	Size		headlen;
	Size		codeclen;
	const char *stored;
	Size		stored_len;
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
	if (s->at_commit)
		elog(ERROR, "cannot put a chunk after the transaction's chunks are appended");
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
	chunk_encode(data, len, head + headlen, &codeclen, &stored, &stored_len);
	headlen += codeclen;
	at = s->pending.len;
	record_append(&s->pending, KIND_XCHUNK, head, headlen, stored, stored_len);
	if (stored != data)
		pfree(unconstify(char *, stored));
	e = hash_search(s->pending_index, &key, HASH_ENTER, &found);
	e->offset = at;
	e->len = s->pending.len - at;
	e->xid = xid;
	e->other = NULL;
	pgstat_count_prolly(PGSTAT_PROLLY_CHUNKS_APPENDED, 1);
	pgstat_count_prolly(PGSTAT_PROLLY_CHUNK_BYTES_APPENDED, len);

	if (s->pending.len >= APPEND_BATCH)
		flush_pending(s);
}

/* The chunk's bytes from its record (rec, len bytes, in path) to out. */
static void
chunk_bytes(const char *path, const char *rec, uint32 len, StringInfo out)
{
	ProllyHash	h;
	Size		after;
	const char *chunk = NULL;
	Size		chunk_len;

	/* (decompressed straight into out, if it holds nothing yet) */
	if (record_valid(rec, len) && record_hash(rec, len, &h, &after))
		chunk = record_chunk(rec, len, after,
							 out->len == 0 ? out : chunk_scratch(), &chunk_len);
	if (chunk == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupt chunk record in file \"%s\"", path)));
	if (chunk != out->data)
		appendBinaryStringInfo(out, chunk, chunk_len);
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
	if (s->at_commit)
		elog(ERROR, "cannot move a chunk store's root after the transaction's chunks are appended");
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
	p->at = s->root_at;
	*data = (char *) p;
	return sizeof(CommitPayload);
}

static bool
journal_commit_writes(void)
{
	ProllyChunkStore *s = session_store;

	if (s == NULL)
		return false;
	reserve_at_commit(s);
	if (s->moving)
		INJECTION_POINT_LOAD("prolly-root-committing");
	return s->at_commit;
}

/*
 * In the commit's critical section: no allocation.  A root moved goes in
 * its PAD's place in the bytes written (the WAL has the PAD, the commit
 * record the ROOT, both flushed), not over the PAD after them: while the
 * range is in flight a collection's window waits for it, and once it is
 * written the window may have read it and marked from what it found.
 */
static void
journal_committed(void)
{
	ProllyChunkStore *s = session_store;
	bool		in_append = false;

	if (s == NULL)
		return;
	if (s->moving)
	{
		uint64		at = s->pad_offset - s->commit_append.offset;

		in_append = s->at_commit && s->pad_segment == s->commit_append.g &&
			s->pad_offset >= s->commit_append.offset &&
			at + s->root_rec_len <= s->commit_append.len;
		Assert(in_append);
		INJECTION_POINT_CACHED("prolly-root-committing", NULL);
		if (in_append)
			memcpy(s->pending.data + at, s->root_rec, s->root_rec_len);
	}
	if (s->at_commit)
	{
		append_write(&s->commit_append, s->pending.data);
		s->at_commit = false;
	}
	if (!s->moving)
		return;
	/* (a PAD not in this append, never expected: the ROOT over it) */
	if (!in_append)
	{
		if (FileWrite(s->pad_segment->file, s->root_rec, s->root_rec_len,
					  s->pad_offset, WAIT_EVENT_DATA_FILE_WRITE) != s->root_rec_len)
			ereport(PANIC,
					(errcode_for_file_access(),
					 errmsg("could not write the root in file \"%s\": %m",
							s->pad_segment->path)));
		pgstat_count_prolly(PGSTAT_PROLLY_JOURNAL_BYTES_WRITTEN, s->root_rec_len);
	}
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
	root_record(&rec, &p.root, p.at, false);
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

	/* the appends reserved before the redo point, written */
	wait_reserved(NULL);
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
	write_table_file(s, &m, keep, 0, next, arg, false);
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
		write_table_file(s, &m, keep, flags, conjoin_next, &cs, true);
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
	XLogRecPtr *sealed = palloc0(sizeof(XLogRecPtr) * (m->nsegments + 1));

	memcpy(segs, m->segments, sizeof(uint64) * m->nsegments);
	if (m->sealed_at)
		memcpy(sealed, m->sealed_at, sizeof(XLogRecPtr) * m->nsegments);
	segs[m->nsegments] = id;
	scan_journal(s);
	m->root = s->root;
	m->last_id = id;
	m->segments = segs;
	m->sealed_at = sealed;
	m->nsegments++;
	LWLockAcquire(ProllyJournalLock, LW_EXCLUSIVE);
	/* after every append to it: each reserved its range under this lock */
	sealed[m->nsegments - 2] = GetXLogInsertRecPtr();
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
journal_segments(ProllyChunkStore *s, bool count, ProllyChunkFileVisit visit,
				 void *arg)
{
	ListCell   *lc;
	int			nsegs;
	int		   *counts;
	int		   *last;
	int			i;

	/* each chunk once per segment it has a copy in, in one pass */
	if (count)
		scan_journal(s);
	else
		refresh_segments(s);
	nsegs = list_length(s->segments);
	counts = palloc0(sizeof(int) * Max(nsegs, 1));
	last = palloc(sizeof(int) * Max(nsegs, 1));
	memset(last, -1, sizeof(int) * Max(nsegs, 1));
	if (count)
	{
		HASH_SEQ_STATUS status;
		IndexEntry *e;
		int			ordinal = 0;

		hash_seq_init(&status, s->index);
		while ((e = hash_seq_search(&status)) != NULL)
		{
			for (IndexEntry *c = e; c != NULL; c = c->other)
			{
				i = 0;
				foreach(lc, s->segments)
				{
					if (((Segment *) lfirst(lc))->id == c->segment)
						break;
					i++;
				}
				if (i < nsegs && last[i] != ordinal)
				{
					counts[i]++;
					last[i] = ordinal;
				}
			}
			ordinal++;
		}
	}
	i = 0;
	foreach(lc, s->segments)
	{
		Segment    *g = lfirst(lc);
		char	   *name = strrchr(g->path, '/') + 1;
		pgoff_t		size;
		int			n = counts[i++];

		size = open_segment(g) ? FileSize(g->file) : 0;
		if (size < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not determine the size of file \"%s\": %m",
							g->path)));
		if (!count)
			n = size > JOURNAL_HEADER_SIZE ? -1 : 0;
		visit(name, n, (uint64) size, false, arg);
	}
	pfree(counts);
	pfree(last);
}

/*
 * The roots the store was moved to, oldest first: the ROOT records of the
 * segments named, in their order, up to where this backend has read them
 * (skipping the ranges whose appends were in flight then).  A collection
 * retires the segments it collected, and their ROOT records with them.
 */
static void
journal_roots(ProllyChunkStore *s, ProllyChunkRootVisit visit, void *arg)
{
	ListCell   *lc;
	char	   *rec = NULL;
	Size		reclen = 0;

	scan_journal(s);
	foreach(lc, s->segments)
	{
		Segment    *g = lfirst(lc);
		uint64		offset = JOURNAL_HEADER_SIZE;
		uint32		len;

		if (!open_segment(g))
			continue;
		while (offset < g->scanned)
		{
			ListCell   *hc;
			bool		skipped = false;

			CHECK_FOR_INTERRUPTS();
			foreach(hc, g->holes)
			{
				Hole	   *hole = lfirst(hc);

				if (hole->offset == offset)
				{
					offset += hole->len;
					skipped = true;
					break;
				}
			}
			if (skipped)
				continue;
			if ((len = gc_record(g, offset, g->scanned, &rec, &reclen)) == 0)
				break;
			if ((uint8) rec[sizeof(uint32)] == KIND_ROOT)
			{
				ProllyHash	h;
				Size		after;

				if (record_hash(rec, len, &h, &after))
					visit(&h, record_root_time(rec, len, after), arg);
			}
			offset += len;
		}
	}
	if (rec)
		pfree(rec);
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
	int			level;			/* the transaction nesting level it began at */
	bool		full;
	bool		settle;			/* the files written are settled */
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
	int			keep;			/* its KeepSet, -1 once let go */
	bool		keep_all;		/* it had none */
	int			nretained;		/* sealed segments not collected: a logical */
	uint64	   *retained;		/* slot may still decode what was appended */
	XLogRecPtr *retained_at;	/* to them (their sealed_at) */
	int			kept_seen;		/* how many of it gc_kept() visited */
	uint64		next_id;		/* the next file id to give */
	uint8	   *copied;			/* slots gc_copy() wrote */
	List	   *written;		/* ManifestFile *, the new files */
	bool		window;
	instr_time	window_began;	/* (a monotonic clock) */
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
 * before.  One prepared meanwhile, or before, isn't waited for: it reads
 * nothing more, and may stay for days.
 */
static void
wait_for_readers(VirtualTransactionId *vxids, int n)
{
	for (int i = 0; i < n; i++)
	{
		CHECK_FOR_INTERRUPTS();
		VirtualXactLockUnprepared(vxids[i]);
	}
}

/* The prepared transaction vxid is the id of (invalid: it is none's). */
static TransactionId
prepared_xid(VirtualTransactionId vxid)
{
	bool		more = false;

	if (VirtualTransactionIdIsRecoveredPreparedXact(vxid))
		return vxid.localTransactionId;
	if (max_prepared_xacts == 0)
		return InvalidTransactionId;
	return TwoPhaseGetXidByVirtualXID(vxid, &more);
}

/*
 * Wait for transaction xid (a top-level one of this database) to end, or
 * to be prepared: true if it was.  Its backend's virtual transaction is
 * waited for, not the xid, which a prepared transaction holds until it is
 * finished, maybe days later.
 */
static bool
wait_unprepared(TransactionId xid)
{
	for (;;)
	{
		VirtualTransactionId *vxids;
		TransactionId *pxids;
		int			n;
		int			running = -1;

		CHECK_FOR_INTERRUPTS();
		if (!TransactionIdIsInProgress(xid))
			return false;
		vxids = GetCurrentVirtualXIDs(InvalidTransactionId, false, false, 0,
									  &n);
		pxids = palloc(sizeof(TransactionId) * Max(n, 1));
		for (int i = 0; i < n; i++)
		{
			pxids[i] = prepared_xid(vxids[i]);
			if (pxids[i] == xid)
			{
				pfree(pxids);
				pfree(vxids);
				return true;
			}
		}

		/*
		 * The backend running it, if one is now (not under a prepared
		 * transaction's id: a backend's own is the same as the one it
		 * prepared until its next transaction begins).
		 */
		for (int i = 0; i < n && running < 0; i++)
		{
			TransactionId pxid;
			TransactionId xmin;
			int			nsubxid;
			bool		overflowed;

			if (TransactionIdIsValid(pxids[i]))
				continue;
			ProcNumberGetTransactionIds(vxids[i].procNumber, &pxid, &xmin,
										&nsubxid, &overflowed);
			if (pxid == xid)
				running = i;
		}
		if (running >= 0)
			VirtualXactLockUnprepared(vxids[running]);
		else
			pg_usleep(1000L);	/* ending, or being prepared */
		pfree(pxids);
		pfree(vxids);
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
	{
		char		header[JOURNAL_HEADER_SIZE];
		Size		hlen = read_header(f->file, header, f->path);

		if (!header_ok(header, hlen))
			check_header(header, hlen, f->path);
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
journal_gc_begin(ProllyChunkStore *s, ProllyGcScope scope, ProllyHash *root,
				 uint64 *nslots)
{
	bool		full = (scope == PROLLY_GC_FULL);
	MemoryContext cxt;
	MemoryContext old;
	ProllyChunkGc *gc;
	char	   *dir;
	HASHCTL		ctl;
	int			n = 0;
	uint64		slot = 0;
	XLogRecPtr	horizon;

	if (s->gc != NULL)
		elog(ERROR, "a chunk store collection is running in this transaction");

	/*
	 * Not in a transaction that has used the store: it would straddle its
	 * own swap, and its chunks appended to a sealed segment are its own to
	 * wait for.
	 */
	if (MyProc->vxid.lxid == s->xact_lxid)
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("cannot collect the chunk store in a transaction that has used it")));

	/*
	 * One at a time, a second refused rather than waiting: the first waits
	 * for every transaction running at its swap, the second among them.  No
	 * conjoin or new table file until the swap (from then on a transfer
	 * older than the swap is refused, not waited for).
	 */
	{
		LOCKTAG		tag;

		SET_LOCKTAG_OBJECT(tag, s->dboid, GC_LOCK_CLASSID, 0, 0);
		if (LockAcquire(&tag, ExclusiveLock, false, true) == LOCKACQUIRE_NOT_AVAIL)
			ereport(ERROR,
					(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					 errmsg("another collection of this database's chunk store is running")));
	}

	cxt = AllocSetContextCreate(TopTransactionContext, "prolly collection",
								ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cxt);
	gc = palloc0(sizeof(ProllyChunkGc));
	dir = GetDatabasePath(s->dboid, s->spcoid);
	gc->s = s;
	gc->cxt = cxt;
	gc->full = full;
	gc->settle = (scope != PROLLY_GC_JOURNAL);
	gc->level = GetCurrentTransactionNestLevel();
	gc->keep = -1;

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
	/* the sealed segments whole before they are read */
	wait_appends(s, gc->segment);
	INJECTION_POINT("prolly-gc-sealed", NULL);
	gc->next_id = gc->m.last_id + 1;

	ctl.keysize = sizeof(ProllyHash);
	ctl.entrysize = sizeof(GcSlot);
	ctl.hcxt = cxt;
	gc->slots = hash_create("prolly collection slots", 1024, &ctl,
							HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	gc->files = palloc0(sizeof(GcFile) * (gc->m.nsegments + gc->m.nfiles));

	/*
	 * Every segment but the one appends go to now, but those sealed after
	 * the oldest position a logical replication slot of the database may
	 * decode from: kept as they are, their chunks roots (gc_retained()).
	 */
	horizon = logical_horizon(s->dboid);
	gc->retained = palloc(sizeof(uint64) * gc->m.nsegments);
	gc->retained_at = palloc(sizeof(XLogRecPtr) * gc->m.nsegments);
	for (int i = 0; i < gc->m.nsegments - 1; i++)
	{
		GcFile	   *f;

		if (XLogRecPtrIsValid(horizon) &&
			(!XLogRecPtrIsValid(gc->m.sealed_at[i]) ||
			 gc->m.sealed_at[i] > horizon))
		{
			gc->retained[gc->nretained] = gc->m.segments[i];
			gc->retained_at[gc->nretained++] = gc->m.sealed_at[i];
			continue;
		}
		f = &gc->files[n++];
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

		if ((settled && !full) || scope == PROLLY_GC_JOURNAL)
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

	/*
	 * Keeping from here on (one that began before the seal is as good: a
	 * transaction that named a chunk since is waited for), its set let go
	 * by gc_abort() if the collection fails.
	 */
	gc->keep = keep_begin(s, &gc->xids);
	gc->keep_all = (gc->keep < 0);
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
 * The oldest position a logical replication slot of database dboid may
 * decode from (invalid: it has none).
 */
static XLogRecPtr
logical_horizon(Oid dboid)
{
	XLogRecPtr	horizon = InvalidXLogRecPtr;

	if (max_replication_slots <= 0)
		return InvalidXLogRecPtr;
	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (int i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *slot = &ReplicationSlotCtl->replication_slots[i];
		XLogRecPtr	lsn = InvalidXLogRecPtr;

		if (!slot->in_use)
			continue;
		SpinLockAcquire(&slot->mutex);
		if (SlotIsLogical(slot) && slot->data.database == dboid)
			lsn = slot->data.restart_lsn;
		SpinLockRelease(&slot->mutex);
		if (XLogRecPtrIsValid(lsn) &&
			(!XLogRecPtrIsValid(horizon) || lsn < horizon))
			horizon = lsn;
	}
	LWLockRelease(ReplicationSlotControlLock);
	return horizon;
}

/*
 * The whole, valid record of segment g at offset, within its first size
 * bytes: its length, its bytes read into *rec (palloc'd, grown as needed,
 * *reclen bytes); 0 if there is none.
 */
static uint32
gc_record(Segment *g, uint64 offset, uint64 size, char **rec, Size *reclen)
{
	uint32		len;

	if (offset + RECORD_OVERHEAD > size)
		return 0;
	read_exact(g, (char *) &len, sizeof(len), offset);
	if (len < RECORD_OVERHEAD || offset + len > size)
		return 0;
	if (len > *reclen)
	{
		*reclen = Max(len, 8192);
		*rec = *rec ? repalloc(*rec, *reclen) : palloc(*reclen);
	}
	read_exact(g, *rec, len, offset);
	return record_valid(*rec, len) ? len : 0;
}

/*
 * Visit every chunk of the sealed segments a collection keeps as they are
 * (a logical replication slot may still decode a row naming it): roots.
 * Not an aborted transaction's, which no row names, nor the ROOT records
 * (decoding reads rows' values, not history).  A transaction in progress
 * has its chunks visited all the same, so it isn't waited for.
 */
static void
journal_gc_retained(ProllyChunkGc *gc, ProllyChunkGcVisit visit, void *arg)
{
	ProllyChunkStore *s = gc->s;

	for (int i = 0; i < gc->nretained; i++)
	{
		Segment		g;
		uint64		size;
		uint64		offset = JOURNAL_HEADER_SIZE;
		char	   *rec = NULL;
		Size		reclen = 0;
		uint32		len;

		memset(&g, 0, sizeof(g));
		g.id = gc->retained[i];
		g.path = segment_path(s->dboid, s->spcoid, g.id);
		g.file = -1;
		if (!open_segment(&g))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("journal segment %llu of the manifest is missing",
							(unsigned long long) g.id)));
		size = FileSize(g.file);
		while ((len = gc_record(&g, offset, size, &rec, &reclen)) > 0)
		{
			uint8		kind = (uint8) rec[sizeof(uint32)];
			ProllyHash	h;
			Size		after;
			TransactionId xid;
			const char *chunk;
			Size		chunk_len;

			CHECK_FOR_INTERRUPTS();
			if ((kind == KIND_CHUNK || kind == KIND_XCHUNK) &&
				record_hash_xid(rec, len, &h, &after, &xid) &&
				!(TransactionIdIsValid(xid) && TransactionIdDidAbort(xid)))
			{
				chunk = decoded_chunk(rec, len, after, g.path, &chunk_len);
				visit(&h, chunk, chunk_len, arg);
			}
			offset += len;
		}
		FileClose(g.file);
		if (rec)
			pfree(rec);
		pfree(g.path);
	}
}

/*
 * Visit the chunks the transactions prepared appended to the sealed
 * segments collected: roots, as they may be named when they are finished.
 */
static void
gc_prepared(ProllyChunkGc *gc, List *prepared, ProllyChunkGcVisit visit,
			void *arg)
{
	for (int i = 0; i < gc->nfiles; i++)
	{
		GcFile	   *f = &gc->files[i];
		uint64		offset = JOURNAL_HEADER_SIZE;
		char	   *rec = NULL;
		Size		reclen = 0;
		uint32		len;

		if (!f->segment || f->file < 0)
			continue;
		while (offset + RECORD_OVERHEAD <= f->size)
		{
			ProllyHash	h;
			Size		after;
			TransactionId xid;

			CHECK_FOR_INTERRUPTS();
			if (FileRead(f->file, (char *) &len, sizeof(len), offset,
						 WAIT_EVENT_DATA_FILE_READ) != sizeof(len) ||
				len < RECORD_OVERHEAD || offset + len > f->size)
				break;
			if (len > reclen)
			{
				reclen = Max(len, 8192);
				rec = rec ? repalloc(rec, reclen) : palloc(reclen);
			}
			if (FileRead(f->file, rec, len, offset,
						 WAIT_EVENT_DATA_FILE_READ) != len ||
				!record_valid(rec, len))
				break;
			if ((uint8) rec[sizeof(uint32)] == KIND_XCHUNK &&
				record_hash_xid(rec, len, &h, &after, &xid) &&
				list_member_xid(prepared, xid))
				visit(&h, NULL, 0, arg);
			offset += len;
		}
		if (rec)
			pfree(rec);
	}
}

/*
 * The root, and the chunks appended since the seal (or the last call).
 * The first call waits for the transactions that appended to the sealed
 * segments and were in progress at the seal: then their PADs are ROOTs or
 * left, and their chunks committed or not, for good.  One prepared isn't
 * waited for (a PAD of its stays one: it can't have moved the root), its
 * chunks roots.  Nor is one that told keep() of a chunk, once prepared: its
 * pointers are on pages.  With window, appends are held off from here
 * until the swap.
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
	uint32		len;
	ListCell   *lc;

	if (!gc->waited)
	{
		List	   *prepared = NIL;

		foreach(lc, gc->xids)
		{
			if (wait_unprepared(lfirst_xid(lc)))
				prepared = lappend_xid(prepared, lfirst_xid(lc));
		}
		gc->waited = true;
		if (prepared != NIL)
			gc_prepared(gc, prepared, visit, arg);
		list_free(prepared);
	}
	if (window && !gc->window)
	{
		INJECTION_POINT("prolly-gc-window", NULL);

		/*
		 * Every append reserved before the window, written: those in
		 * flight now waited for before the lock, so the wait under it (for
		 * what was reserved in between) is only for their writes.
		 */
		wait_reserved(s);
		LWLockAcquire(ProllyJournalLock, LW_EXCLUSIVE);
		wait_appends(s, PG_UINT64_MAX);
		gc->window = true;
		INSTR_TIME_SET_CURRENT(gc->window_began);
	}

	scan_journal(s);
	if (s->root.len > 0)
		visit(&s->root, NULL, 0, arg);

	if (!open_segment(g))
		return;
	size = FileSize(g->file);
	offset = g->scanned;
	while ((len = gc_record(g, offset, size, &rec, &reclen)) > 0)
	{
		uint8		kind = (uint8) rec[sizeof(uint32)];
		ProllyHash	h;
		Size		after;
		TransactionId xid;
		const char *chunk;
		Size		chunk_len;

		if ((kind == KIND_CHUNK || kind == KIND_XCHUNK) &&
			record_hash_xid(rec, len, &h, &after, &xid))
		{
			chunk = decoded_chunk(rec, len, after, g->path, &chunk_len);
			visit(&h, chunk, chunk_len, arg);
		}
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
	bool		paced;			/* by the cost-based vacuum delay */
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

		/* (read and written: a page missed and one dirtied) */
		if (gcc->paced)
		{
			if (VacuumCostActive)
				VacuumCostBalance += VacuumCostPageMiss + VacuumCostPageDirty;
			vacuum_delay_point(false);
		}

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

/*
 * Write the marked slots not yet written (but kept files') to a new file;
 * paced by the cost-based vacuum delay where one is active (not in the
 * window, where interrupts are held).
 */
static void
gc_write(ProllyChunkGc *gc, const uint8 *marks, const bool *keep, bool paced)
{
	GcCopy		gcc = {gc, marks, keep, 0, prolly_content_hash(), paced};
	uint32		nchunks;
	uint64		size;

	if (gc->copied == NULL)
		gc->copied = MemoryContextAllocZero(gc->cxt, gc->nslots / 8 + 1);
	if (write_file(gc->s, gc->next_id, gc_copy_next, &gcc, &nchunks, &size))
	{
		ManifestFile *f = MemoryContextAlloc(gc->cxt, sizeof(ManifestFile));
		MemoryContext old = MemoryContextSwitchTo(gc->cxt);

		pgstat_count_prolly(PGSTAT_PROLLY_GC_BYTES_COPIED, size);
		f->id = gc->next_id++;
		f->nchunks = nchunks;
		f->size = size;
		f->flags = gc->settle ? MANIFEST_FILE_SETTLED : 0;
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
 * Add an unmarked chunk's hash to the dropped list (hashes, n of them so
 * far), unless another copy of it is live or in a file kept whole; returns
 * the new count.
 */
static uint64
dropped_add(ProllyChunkGc *gc, const uint8 *marks, const bool *keep,
			const ProllyHash *h, char *hashes, uint64 n)
{
	int64		live;
	ProllyHash	key;

	if (h->len != gc->hash_len)
		return n;
	live = journal_gc_find(gc, h);
	if (live >= 0 && marked(marks, live))
		return n;
	hash_key(&key, h);
	for (int i = 0; i < gc->nfiles; i++)
	{
		if (keep[i] && !gc->files[i].segment &&
			PROLLY_CALL(table_format(), locate, gc->files[i].t->tf, &key) >= 0)
			return n;
	}
	memcpy(hashes + n * gc->hash_len, h->bytes, gc->hash_len);
	return n + 1;
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
	HASH_SEQ_STATUS status;
	GcSlot	   *e;

	gc_write(gc, marks, keep, true);
	INJECTION_POINT("prolly-gc-copied", NULL);

	hashes = MemoryContextAllocHuge(gc->cxt, (gc->nslots + 1) * (Size) gc->hash_len);

	/* a sealed segment's slots by the table of them, which has each hash */
	hash_seq_init(&status, gc->slots);
	while ((e = hash_seq_search(&status)) != NULL)
	{
		if (!marked(marks, e->slot) && !keep[slot_file(gc, e->slot) - gc->files])
			n = dropped_add(gc, marks, keep, &e->hash, hashes, n);
	}

	/* a table file's by its index */
	for (int i = 0; i < gc->nfiles; i++)
	{
		GcFile	   *f = &gc->files[i];

		if (f->segment || keep[i])
			continue;
		for (uint64 slot = f->first; slot < f->first + f->count; slot++)
		{
			ProllyHash	h;
			uint64		o;
			uint32		l;

			if (marked(marks, slot))
				continue;
			PROLLY_CALL(table_format(), entry, f->t->tf, (int) (slot - f->first),
						&h, &o, &l);
			n = dropped_add(gc, marks, keep, &h, hashes, n);
		}
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
	/* (not WAL-logged: the store's metadata, as a manifest is) */
	pgstat_count_prolly(PGSTAT_PROLLY_MANIFEST_BYTES_WRITTEN, 1 + (uint64) n * gc->hash_len);
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
	uint64		window_usec;

	Assert(gc->window);
	gc_write(gc, marks, keep, false);

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
	nm.nsegments = gc->nretained + 1;
	nm.segments = palloc(sizeof(uint64) * nm.nsegments);
	nm.sealed_at = palloc0(sizeof(XLogRecPtr) * nm.nsegments);
	memcpy(nm.segments, gc->retained, sizeof(uint64) * gc->nretained);
	memcpy(nm.sealed_at, gc->retained_at, sizeof(XLogRecPtr) * gc->nretained);
	nm.segments[gc->nretained] = gc->segment;
	scan_journal(s);
	nm.root = s->root;
	replace_manifest(s, &nm, XLOG_CHUNKS_SWAP);
	LWLockRelease(ProllyJournalLock);
	gc->window = false;
	{
		instr_time	now;

		INSTR_TIME_SET_CURRENT(now);
		INSTR_TIME_SUBTRACT(now, gc->window_began);
		window_usec = INSTR_TIME_GET_MICROSEC(now);
	}
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
		result->window_usec = window_usec;
	}
	keep_end(&gc->keep);
	INJECTION_POINT("prolly-gc-swapped", NULL);
}

/*
 * Visit the chunks keep() was told of since the collection began (since
 * the last call); false if there were too many to remember.
 */
static bool
journal_gc_kept(ProllyChunkGc *gc, ProllyChunkRefVisit visit, void *arg)
{
	JournalShared *js = shared();
	KeepSet    *k;
	ProllyHash *hashes;
	int			n;
	bool		overflow;

	if (gc->keep_all)
		return false;
	if (gc->keep < 0)
		elog(ERROR, "the collection keeps nothing any more");
	k = &js->keeps[gc->keep];

	/*
	 * Only the count under the mutex: the hashes before it are written
	 * before it is raised, and none is changed until the set is let go.
	 */
	SpinLockAcquire(&js->mutex);
	overflow = k->overflow;
	n = k->n - gc->kept_seen;
	SpinLockRelease(&js->mutex);
	hashes = palloc(sizeof(ProllyHash) * Max(n, 1));
	memcpy(hashes, k->hashes + gc->kept_seen, sizeof(ProllyHash) * n);
	for (int i = 0; i < n; i++)
		visit(&hashes[i], arg);
	gc->kept_seen += n;
	pfree(hashes);
	return !overflow;
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
	keep_end(&gc->keep);
	gc->s->gc = NULL;
}

/*
 * A collection begun in a subtransaction that aborts (at nesting level
 * 'level') is abandoned with it: its locks went with the subtransaction.
 */
static void
gc_abort_sub(ProllyChunkGc *gc, int level)
{
	MemoryContext cxt = gc->cxt;

	if (gc->level < level)
		return;
	gc_abort(gc);
	MemoryContextDelete(cxt);
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
 * of what each swap since the transaction began dropped.  With root, the
 * root it moves to instead.
 */
static void
check_dropped(ProllyChunkStore *s, const ProllyHash *root)
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

		if (root != NULL)
		{
			if (n > 0 && root->len == hlen &&
				bsearch_arg(root->bytes, list + 1, n, hlen, hash_bytes_cmp,
							&hlen) != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("could not move the chunk store's root to %s: it was collected while the transaction ran",
								prolly_hash_hex(root)),
						 errhint("Try the transaction again.")));
			pfree(list);
			pfree(path);
			continue;
		}

		hash_seq_init(&status, s->pending_index);
		while ((e = hash_seq_search(&status)) != NULL)
		{
			const char *rec = s->pending.data + e->offset;
			ProllyHash	h;
			Size		after;
			const char *p;
			const char *end;
			Size		chunk_len;

			if (!record_hash(rec, e->len, &h, &after))
				continue;
			p = decoded_chunk(rec, e->len, after, "the transaction's pending chunks",
							  &chunk_len);
			end = p + chunk_len;
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

/* every process that may append has a range of its own */
static int
append_ranges(void)
{
	return MaxBackends + NUM_AUXILIARY_PROCS;
}

static Size
journal_shmem_size(void)
{
	return add_size(TOUCHES_OFFSET(append_ranges()),
					mul_size(append_ranges(), sizeof(KeepTouch)));
}

static void
journal_shmem_init(void *area)
{
	JournalShared *js = area;

	memset(js, 0, journal_shmem_size());
	/* every backend reads the manifests once */
	pg_atomic_init_u64(&js->manifests, 1);
	pg_atomic_init_u64(&js->keeps_begun, 0);
	SpinLockInit(&js->mutex);
	ConditionVariableInit(&js->written);
	js->nranges = append_ranges();
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
	.commit_writes = journal_commit_writes,
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
	.keep = journal_keep,
	.gc_kept = journal_gc_kept,
	.gc_retained = journal_gc_retained,
	.roots = journal_roots,
};
